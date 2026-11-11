/* The memory behind cx_alloc, cx_realloc and cx_free (D95).
 *
 * Every free says how big the block is, since the compiler always knows: a list frees `cap`
 * elements, a String `cap` bytes, a table what its mask gives. So a block has no header, and a
 * free never has to find out what an address belongs to.
 *
 * Small blocks, up to 32 KiB, come in 40 sizes: steps of 16 bytes up to 128, then four steps for
 * each doubling, so less than a fifth of a block goes unused. Each thread keeps a list of free
 * blocks of each size, and takes from and gives to it with no lock. A thread that frees more than
 * it takes shares its list, about 64 KiB of blocks, as one batch, and a thread that runs out
 * takes one batch before it cuts new blocks, so memory freed by one thread goes back to the
 * threads that allocate. Taking a batch, not every block shared, keeps one thread from holding
 * them all while the others cut new ones: a server whose chunk workers allocated what its main
 * thread freed took 3.6 GB from the system for 0.4 GB of small blocks in use that way, and
 * 0.7 GB with batches.
 *
 * New blocks are cut from 64 KiB pages, each of one size, in a range of addresses reserved once,
 * so a block's page is where it is in the range. Since a page holds one size, every free block of
 * it that's shared is in that size's shared lists, and `trim` counts them there, with only that
 * size's lock held: a page whose every block is there leaves the lists and goes back to the
 * system, and then to whichever size needs a page next. So memory that held one size goes to
 * others, and to the system, as it's freed. A thread that takes a new page trims the size whose
 * shared lists hold the most, now and then, and a program can trim every size when it's quiet.
 * A page with blocks still in use gives back each 4 KiB of it that holds only free blocks, and
 * those blocks leave the lists as holes, which count as free when the page's last block is.
 *
 * Bigger blocks come from the system, rounded to four sizes for each doubling, and a freed one
 * waits in a cache for the next block of its size, up to 256 MiB in all. The Windows heap gives
 * big freed blocks back to the system, so a table that grew and was freed would fault on every
 * page of the next one; from the cache, its pages are still there.
 *
 * Debug builds use malloc instead, with a header that holds each block's size, so a free with
 * the wrong size or a second free stops the program: either is a compiler bug. So do builds
 * with AddressSanitizer or CX_SYSTEM_ALLOC, so tools that watch malloc see every block. With
 * CALX_LEAKS=1, a debug build also keeps where each block was allocated, so a program that ends
 * with blocks still allocated can say where (CLX-261). */

#ifndef _WIN32
#define _GNU_SOURCE /* pthread_getattr_np, for a thread's stack */
#endif
#include "calx_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(CX_DEBUG) || defined(CX_SYSTEM_ALLOC) || __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define CX_MALLOC 1
#endif

/* Blocks this big or bigger can't be asked for: no buffer is bigger than half the address space. */
#define TOO_BIG (((cx_usize)-1 >> 1) + 1)

#ifdef CX_MALLOC

typedef struct {
    cx_usize size;
    cx_usize state;
} block_head; /* 16 bytes, so the block after it is as aligned as malloc's */

#define LIVE 0x4C495645u
#define FREED 0x46524545u

static cx_usize live;

static CX_COLD void bad_free(const char* what) {
    fprintf(stderr, "calx: %s; this is a bug in calx, please report it\n", what);
    fflush(stderr);
    _Exit(102);
}

static block_head* head_of(void* p, cx_usize size) {
    block_head* h = (block_head*)p - 1;
    if (h->state != LIVE) bad_free(h->state == FREED ? "a block was freed twice" : "a block that wasn't allocated was freed");
    if (h->size != size) bad_free("a block was freed with a size it wasn't allocated with");
    return h;
}

/* With CALX_LEAKS=1, each block has a record in front of its header: the return addresses of the
 * calls that allocated it, innermost first, and its place in a list of every block allocated, so
 * the leak check can say where each block left was allocated. Whether to keep them is settled at
 * the first allocation, so every block has a record or none does. */
#define TRACK_FRAMES 14

typedef struct track_rec {
    struct track_rec* prev;
    struct track_rec* next;
    void* at[TRACK_FRAMES];
} track_rec; /* 128 bytes, so the block after it is still as aligned as malloc's */

static track_rec tracked = {&tracked, &tracked, {0}};
static int tracking = -1; /* -1 until the first allocation looks */

static int track_on(void) {
    int t = __atomic_load_n(&tracking, __ATOMIC_RELAXED);
    if (t < 0) {
        const char* v = getenv("CALX_LEAKS");
        t = v && strcmp(v, "1") == 0;
        __atomic_store_n(&tracking, t, __ATOMIC_RELAXED);
    }
    return t;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK track_lock = SRWLOCK_INIT;
static void track_take(void) { AcquireSRWLockExclusive(&track_lock); }
static void track_give(void) { ReleaseSRWLockExclusive(&track_lock); }

/* Windows unwinds from the program's tables: clang's x64 frame pointer doesn't point at the one
 * before it. */
static __attribute__((noinline)) void track_stack(void** at) {
    USHORT n = RtlCaptureStackBackTrace(1, TRACK_FRAMES, at, NULL);
    for (USHORT i = n; i < TRACK_FRAMES; i++) at[i] = 0;
}
#else
#include <pthread.h>
#include <stdint.h>
static pthread_mutex_t track_lock = PTHREAD_MUTEX_INITIALIZER;
static void track_take(void) { pthread_mutex_lock(&track_lock); }
static void track_give(void) { pthread_mutex_unlock(&track_lock); }

/* This thread's stack, which the walk below never reads outside of; `track_hi` is 1 if it isn't
 * known, so the walk reads nothing. */
static _Thread_local uintptr_t track_lo, track_hi;

/* The frame pointers' chain, which a debug build keeps, since it's compiled at -O0: each frame
 * holds the one before it and the return address after that. */
static __attribute__((noinline)) void track_stack(void** at) {
    if (!track_hi) {
        pthread_attr_t a;
        void* lo;
        size_t size;
        track_hi = 1;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            if (pthread_attr_getstack(&a, &lo, &size) == 0) {
                track_lo = (uintptr_t)lo;
                track_hi = (uintptr_t)lo + size;
            }
            pthread_attr_destroy(&a);
        }
    }
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    int i = 0;
    while (i < TRACK_FRAMES && fp >= track_lo && fp + 2 * sizeof(void*) <= track_hi && fp % sizeof(void*) == 0) {
        void** frame = (void**)fp;
        if (!frame[1]) break;
        at[i++] = frame[1];
        uintptr_t up = (uintptr_t)frame[0];
        if (up <= fp) break;
        fp = up;
    }
    while (i < TRACK_FRAMES) at[i++] = 0;
}
#endif

static void track_link(track_rec* r) {
    track_take();
    r->prev = &tracked;
    r->next = tracked.next;
    tracked.next->prev = r;
    tracked.next = r;
    track_give();
}

static void track_unlink(track_rec* r) {
    track_take();
    r->prev->next = r->next;
    r->next->prev = r->prev;
    track_give();
}

static void to_stderr(const char* s) { fputs(s, stderr); }

int cx_alloc_report(void) {
    if (__atomic_load_n(&tracking, __ATOMIC_RELAXED) != 1) return 0;
    track_take();
    /* Oldest first, which is the list's end, and at most 20. */
    int shown = 0;
    cx_usize left = 0;
    for (track_rec* r = tracked.prev; r != &tracked; r = r->prev) {
        if (shown == 20) {
            left++;
            continue;
        }
        block_head* h = (block_head*)(r + 1);
        int n = 0;
        while (n < TRACK_FRAMES && r->at[n]) n++;
        fprintf(stderr, "a block of %llu bytes, allocated\n", (unsigned long long)h->size);
        cx_trace_frames(to_stderr, r->at, n);
        shown++;
    }
    if (left) fprintf(stderr, "and %llu more\n", (unsigned long long)left);
    track_give();
    return 1;
}

void* cx_alloc(cx_usize size) {
    int t = track_on();
    cx_usize extra = t ? sizeof(track_rec) : 0;
    if (size >= TOO_BIG - sizeof(block_head) - extra) cx_panic_capacity();
    char* m = (char*)malloc(extra + sizeof(block_head) + size);
    if (!m) cx_panic_capacity();
    block_head* h = (block_head*)(m + extra);
    h->size = size;
    h->state = LIVE;
    __atomic_fetch_add(&live, 1, __ATOMIC_RELAXED);
    if (t) {
        track_rec* r = (track_rec*)m;
        track_stack(r->at);
        track_link(r);
    }
    return h + 1;
}

void* cx_realloc(void* p, cx_usize old, cx_usize size) {
    if (!p) return cx_alloc(size);
    int t = track_on();
    cx_usize extra = t ? sizeof(track_rec) : 0;
    if (size >= TOO_BIG - sizeof(block_head) - extra) cx_panic_capacity();
    block_head* was = head_of(p, old);
    /* The block keeps where it was first allocated, and its place in the list moves with it. */
    if (t) track_unlink((track_rec*)was - 1);
    char* m = (char*)realloc((char*)was - extra, extra + sizeof(block_head) + size);
    if (!m) cx_panic_capacity();
    if (t) track_link((track_rec*)m);
    block_head* h = (block_head*)(m + extra);
    h->size = size;
    return h + 1;
}

/* realloc decides what it copies. */
void* cx_realloc_keep(void* p, cx_usize old, cx_usize size, cx_usize keep) {
    (void)keep;
    return cx_realloc(p, old, size);
}

void cx_free(void* p, cx_usize size) {
    if (!p) return;
    block_head* h = head_of(p, size);
    h->state = FREED;
    __atomic_fetch_sub(&live, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&tracking, __ATOMIC_RELAXED) == 1) {
        track_rec* r = (track_rec*)h - 1;
        track_unlink(r);
        free(r);
    } else {
        free(h);
    }
}

cx_usize cx_alloc_live(void) { return __atomic_load_n(&live, __ATOMIC_RELAXED); }

/* malloc keeps what it keeps. */
cx_usize cx_alloc_trim(void) { return 0; }
void cx_alloc_idle(void) {}

#else

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif

#define SMALL_MAX 32768
#define CLASSES 40
/* Small blocks' pages, and the most of them the reserved range holds: 16 GiB. */
#define PAGE_SHIFT 16
#define PAGE ((cx_usize)1 << PAGE_SHIFT)
#define ARENA_PAGES ((cx_u32)1 << 18)
#define NO_PAGE 0xFFFFFFFFu
#define LARGE_CLASSES 192
/* Blocks bigger than this aren't rounded up or cached: a quarter more of 64 MiB is too much to
 * commit for nothing, and such blocks are rarely freed and made again. */
#define HUGE_BLOCK ((cx_usize)64 << 20)
#define CACHE_MAX ((cx_usize)256 << 20)
#define OS_PAGE ((cx_usize)4096)

/* --- the system ------------------------------------------------------------------------- */

static void* os_alloc(cx_usize n) {
#ifdef _WIN32
    void* p = VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = NULL;
#endif
    if (!p) cx_panic_capacity();
    return p;
}

static void os_free(void* p, cx_usize n) {
#ifdef _WIN32
    (void)n;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, n);
#endif
}

/* A spin lock. Each one is held for a few instructions, so waiting is short, but a thread that
 * waits long lets the holder run. */
static void lock(int* l) {
    int spins = 0;
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(l, __ATOMIC_RELAXED)) {
            if (++spins < 64) {
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#endif
            } else {
#ifdef _WIN32
                SwitchToThread();
#else
                sched_yield();
#endif
                spins = 0;
            }
        }
    }
}

static void unlock(int* l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

/* --- sizes ------------------------------------------------------------------------------ */

/* The size a request of `n` bytes is cut as, 1 to 32 KiB: 16 to 128 in steps of 16, then for
 * each doubling from 2^b, four steps of 2^(b - 2). */
static inline unsigned small_class(cx_usize n) {
    if (n <= 128) return n ? (unsigned)((n - 1) >> 4) : 0;
    unsigned b = 63u - (unsigned)__builtin_clzll((unsigned long long)(n - 1));
    return 8u + (b - 7u) * 4u + (unsigned)(((n - 1) >> (b - 2)) & 3);
}

static const cx_u32 sizes[CLASSES] = {
    16,   32,   48,   64,   80,   96,    112,   128,   160,   192,   224,   256,   320,   384,
    448,  512,  640,  768,  896,  1024,  1280,  1536,  1792,  2048,  2560,  3072,  3584,  4096,
    5120, 6144, 7168, 8192, 10240, 12288, 14336, 16384, 20480, 24576, 28672, 32768,
};

static inline cx_usize class_size(unsigned c) { return sizes[c]; }

/* The same steps for blocks over 32 KiB, which are multiples of 8 KiB. */
static inline unsigned large_class(cx_usize n) {
    unsigned b = 63u - (unsigned)__builtin_clzll((unsigned long long)(n - 1));
    return (b - 15u) * 4u + (unsigned)(((n - 1) >> (b - 2)) & 3);
}

static inline cx_usize large_size(unsigned c) {
    unsigned b = 15 + c / 4, step = c % 4;
    return ((cx_usize)1 << b) + ((cx_usize)(step + 1) << (b - 2));
}

static inline cx_usize pages(cx_usize n) { return (n + OS_PAGE - 1) & ~(OS_PAGE - 1); }

/* --- pages ----------------------------------------------------------------------------- */

/* A page is new until it's first taken; cut from by one thread; sealed once that thread cuts no
 * more from it, with `cut` the blocks it cut; then free, given back to the system, until it's
 * taken again. */
enum { PAGE_NEW = 0, PAGE_CUTTING = 1, PAGE_SEALED = 2, PAGE_FREE = 3 };

typedef struct {
    cx_u16 cut;
    cx_u16 seen;  /* while a trim counts: its blocks in the shared lists */
    cx_u16 holes; /* blocks given back with the system pages they're on, in no list (CLX-119) */
    cx_u8 state;
    cx_u8 cls;
} page_info;

/* Each page's state, and the free pages' numbers, ARENA_PAGES of each: 3 MiB, in a range reserved
 * with the arena's and committed TABLE_STEP entries at a time as pages are first taken, so a
 * program that uses 64 MiB of small blocks has 12 KiB of them. As static arrays they cost every
 * program all 3 MiB on Windows, which charges an image's writable sections in full when it loads,
 * touched or not. Only the entries of pages taken, below `next_page`, are ever read. */
#define TABLE_STEP 1024u
static cx_u8* arena;
static int arena_tried;
static page_info* page_of;
static int page_lock;
static cx_u32 next_page;
static cx_u32* free_pages;
static cx_u32 free_count;
static cx_u32 table_room; /* entries of both committed */
/* Pages taken since the last trim a thread made as it took one. */
static cx_u32 taken_since_trim;

static inline cx_u32 page_number(const void* p) {
    cx_usize at = (cx_usize)((const cx_u8*)p - arena);
    return arena && (const cx_u8*)p >= arena && at < (cx_usize)ARENA_PAGES << PAGE_SHIFT ? (cx_u32)(at >> PAGE_SHIFT) : NO_PAGE;
}

static cx_u8* reserve(cx_usize n) {
#ifdef _WIN32
    return (cx_u8*)VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_READWRITE);
#else
    void* a = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return a == MAP_FAILED ? NULL : (cx_u8*)a;
#endif
}

static void reserve_arena(void) {
    cx_u8* tables = reserve((cx_usize)ARENA_PAGES * (sizeof(page_info) + sizeof(cx_u32)));
    if (!tables) return;
    arena = reserve((cx_usize)ARENA_PAGES << PAGE_SHIFT);
    page_of = (page_info*)tables;
    free_pages = (cx_u32*)(tables + (cx_usize)ARENA_PAGES * sizeof(page_info));
}

/* Room in the tables for page `n`'s entries, with the page lock held: false if the system has no
 * memory to commit for them. Linux gives a reserved range's pages as they're touched. */
static int table_room_for(cx_u32 n) {
    if (n < table_room) return 1;
#ifdef _WIN32
    if (!VirtualAlloc(page_of + table_room, TABLE_STEP * sizeof(page_info), MEM_COMMIT, PAGE_READWRITE)) return 0;
    if (!VirtualAlloc(free_pages + table_room, TABLE_STEP * sizeof(cx_u32), MEM_COMMIT, PAGE_READWRITE)) return 0;
#endif
    table_room += TABLE_STEP;
    return 1;
}

/* A page for blocks of class `c`, from the free ones, or a new one, committed; or, where no
 * range could be reserved or it's all in use, one from the system that's never given back. */
static cx_u8* take_page(unsigned c, cx_u32* number) {
    lock(&page_lock);
    if (!arena_tried) {
        arena_tried = 1;
        reserve_arena();
    }
    cx_u32 n = NO_PAGE;
    if (free_count) n = free_pages[--free_count];
    else if (arena && next_page < ARENA_PAGES && table_room_for(next_page)) n = next_page++;
    if (n != NO_PAGE) {
        page_of[n].state = PAGE_CUTTING;
        page_of[n].cls = (cx_u8)c;
        page_of[n].cut = 0;
        page_of[n].seen = 0;
        page_of[n].holes = 0;
    }
    unlock(&page_lock);
    *number = n;
    if (n == NO_PAGE) return (cx_u8*)os_alloc(PAGE);
    cx_u8* p = arena + ((cx_usize)n << PAGE_SHIFT);
#ifdef _WIN32
    if (!VirtualAlloc(p, PAGE, MEM_COMMIT, PAGE_READWRITE)) cx_panic_capacity();
#endif
    return p;
}

/* Gives `n` bytes at `p` back to the system, a multiple of OS_PAGE in the range; the range's
 * memory is committed again when its page is next taken. */
static void decommit(cx_u8* p, cx_usize n) {
#ifdef _WIN32
    VirtualFree(p, n, MEM_DECOMMIT);
#else
    madvise(p, n, MADV_DONTNEED);
#endif
}

/* Gives page `n` back to the system, and to the free pages. */
static void give_page(cx_u32 n) {
    decommit(arena + ((cx_usize)n << PAGE_SHIFT), PAGE);
    lock(&page_lock);
    page_of[n].state = PAGE_FREE;
    page_of[n].cut = 0;
    page_of[n].holes = 0;
    free_pages[free_count++] = n;
    unlock(&page_lock);
}

/* --- small blocks ----------------------------------------------------------------------- */

/* A free block's first word points at the next one. A list's last block is remembered, so a
 * whole list moves between a thread and the shared lists in one step. Each size is cut from a
 * page of its own, from `bump` up to `end`. */
typedef struct {
    void* free[CLASSES];
    void* last[CLASSES];
    cx_u32 count[CLASSES];
    cx_u8* bump[CLASSES];
    cx_u8* end[CLASSES];
    cx_u32 page[CLASSES]; /* the page cut from, or NO_PAGE; 0 until the first */
    cx_u8 cutting[CLASSES];
    int known; /* asked to be told when the thread ends */
} heap;

/* A list of free blocks one thread shared, which another takes whole. */
typedef struct {
    void* head;
    void* last;
    cx_u32 count;
} batch;

/* The batches of one size that threads shared, which threads that run out take one at a time,
 * so each takes about what one gave, and none takes every free block of its size while others
 * cut new ones. Past BATCHES, a batch joins the last. */
#define BATCHES 1024

typedef struct {
    int lock;
    cx_u32 count; /* blocks in all the batches */
    cx_u32 n;
    batch* b; /* BATCHES of them, from the system when the size is first shared, as `page_of` */
} shared_list;

static _Thread_local heap tls;
static shared_list shared[CLASSES];

/* How many free blocks of each size a thread keeps before it shares them: 64 KiB of them, and
 * at least 16. With 256 KiB, Calcite's 31 chunk workers peaked at 1,204 MB generating a world,
 * and at 1,132 MB with 64 KiB, as fast; less than 64 KiB saved little more (CLX-120). */
static const cx_u32 kept[CLASSES] = {
    4096, 2048, 1365, 1024, 819, 682, 585, 512, 409, 341, 292, 256, 204, 170,
    146,  128,  102,  85,   73,  64,  51,  42,  36,  32,  25,  21,  18,  16,
    16,   16,   16,   16,   16,  16,  16,  16,  16,  16,  16,  16,
};

static void share(heap* h, unsigned c) {
    shared_list* s = &shared[c];
    lock(&s->lock);
    if (!s->b) s->b = (batch*)os_alloc(BATCHES * sizeof(batch));
    if (s->n < BATCHES) {
        s->b[s->n].head = h->free[c];
        s->b[s->n].last = h->last[c];
        s->b[s->n].count = h->count[c];
        s->n++;
    } else {
        batch* b = &s->b[BATCHES - 1];
        *(void**)h->last[c] = b->head;
        b->head = h->free[c];
        b->count += h->count[c];
    }
    /* Atomic, since `refill` reads it without the lock to see whether to take any. */
    __atomic_store_n(&s->count, s->count + h->count[c], __ATOMIC_RELAXED);
    unlock(&s->lock);
    h->free[c] = NULL;
    h->last[c] = NULL;
    h->count[c] = 0;
}

/* Puts a block of exactly class `c`'s size on this thread's list. */
static inline void put_free(heap* h, unsigned c, void* p) {
    *(void**)p = h->free[c];
    if (!h->free[c]) h->last[c] = p;
    h->free[c] = p;
    h->count[c]++;
}

/* This thread cuts no more from its page of class `c`: the page is sealed with how many blocks
 * were cut from it, and trims may give it back once they're all free. */
static void seal(heap* h, unsigned c) {
    if (!h->cutting[c]) return;
    h->cutting[c] = 0;
    cx_u32 n = h->page[c];
    if (n == NO_PAGE) return;
    cx_u8* start = arena + ((cx_usize)n << PAGE_SHIFT);
    page_of[n].cut = (cx_u16)((cx_usize)(h->bump[c] - start) / class_size(c));
    __atomic_store_n(&page_of[n].state, (cx_u8)PAGE_SEALED, __ATOMIC_RELEASE);
}

/* The system pages in a page, which a page with blocks in use can give back one by one. */
#define OS_PAGES (PAGE / OS_PAGE)

/* The system pages a block covers, as bits of a page's OS_PAGES. */
static inline cx_u32 block_span(cx_u32 i, cx_usize size) {
    cx_usize from = (cx_usize)i * size, to = from + size - 1;
    cx_u32 first = (cx_u32)(from / OS_PAGE), last = (cx_u32)(to / OS_PAGE);
    return (cx_u32)(((2ull << last) - 1) & ~((1ull << first) - 1));
}

/* Gives back each sealed page of class `c` whose every block is in the shared lists: the lists
 * are counted with the class's lock held, so none of those blocks can be taken meanwhile, and
 * those blocks leave them. A page that still has blocks in use, but whose free blocks fill a
 * system page or more, has its free blocks mapped, and each system page that holds only free
 * blocks, or none, goes back: its blocks leave the lists as holes, which count as free when the
 * rest of the page is (CLX-119). That's only `with_holes`, for a program gone quiet: while it's
 * busy, free blocks are kept for the next ones. Returns the bytes given back. */
static cx_usize trim_class(unsigned c, int with_holes) {
    shared_list* s = &shared[c];
    if (!arena || !__atomic_load_n(&s->count, __ATOMIC_RELAXED)) return 0;
    cx_u32 most = __atomic_load_n(&next_page, __ATOMIC_ACQUIRE);
    if (!most) return 0;
    cx_usize size = class_size(c);
    /* The pages it sees, each once; those of them that go, and those still in use that may give
     * back some of their system pages. */
    cx_usize list_bytes = pages((cx_usize)most * sizeof(cx_u32));
    cx_u32* seen = (cx_u32*)os_alloc(list_bytes);
    cx_u32* part = (cx_u32*)os_alloc(list_bytes);
    cx_u32 nseen = 0, going = 0, parts = 0;
    /* For each page in `part`, from `slot` by its number: which of its blocks are free, a bit
     * each, and which of its system pages go. */
    cx_u32* slot = NULL;
    cx_u8* free_bits = NULL;
    cx_u32* gone = NULL;
    cx_usize bits_each = (PAGE / size + 7) / 8;
    cx_usize bits_bytes = 0, gone_bytes = 0;
    cx_usize holed = 0;
    lock(&s->lock);
    for (cx_u32 i = 0; i < s->n; i++) {
        for (void* q = s->b[i].head; q; q = *(void**)q) {
            cx_u32 n = page_number(q);
            if (n == NO_PAGE || __atomic_load_n(&page_of[n].state, __ATOMIC_ACQUIRE) != PAGE_SEALED) continue;
            if (page_of[n].seen++ == 0) seen[nseen++] = n;
        }
    }
    /* A page goes when the lists and its holes hold every block cut from it: its state says so
     * until it's given back, and every other page's count starts again at 0. */
    for (cx_u32 i = 0; i < nseen; i++) {
        cx_u32 n = seen[i];
        page_info* pi = &page_of[n];
        if (pi->cut && pi->seen + pi->holes == pi->cut) {
            pi->state = PAGE_FREE;
            seen[going++] = n;
        } else if (with_holes && (cx_usize)pi->seen * size >= OS_PAGE) {
            part[parts++] = n;
        }
        pi->seen = 0;
    }
    if (parts) {
        slot = (cx_u32*)os_alloc(list_bytes);
        bits_bytes = pages((cx_usize)parts * bits_each);
        free_bits = (cx_u8*)os_alloc(bits_bytes);
        gone_bytes = pages((cx_usize)parts * sizeof(cx_u32));
        gone = (cx_u32*)os_alloc(gone_bytes);
        for (cx_u32 k = 0; k < parts; k++) slot[part[k]] = k + 1;
        for (cx_u32 i = 0; i < s->n; i++) {
            for (void* q = s->b[i].head; q; q = *(void**)q) {
                cx_u32 n = page_number(q);
                if (n == NO_PAGE || !slot[n]) continue;
                cx_u32 b = (cx_u32)((cx_usize)((cx_u8*)q - (arena + ((cx_usize)n << PAGE_SHIFT))) / size);
                free_bits[(cx_usize)(slot[n] - 1) * bits_each + b / 8] |= (cx_u8)(1u << (b % 8));
            }
        }
        /* A system page goes when every block cut that's on it is free; the blocks on those that
         * go become holes, all free, so no block in use is on one. */
        for (cx_u32 k = 0; k < parts; k++) {
            page_info* pi = &page_of[part[k]];
            const cx_u8* bits = free_bits + (cx_usize)k * bits_each;
            cx_u32 keep = 0;
            for (cx_u32 b = 0; b < pi->cut; b++) {
                if (!(bits[b / 8] >> (b % 8) & 1)) keep |= block_span(b, size);
            }
            /* Holes aren't in the lists, so the system pages given back before are kept as they are. */
            cx_u32 drop = ~keep & ((1u << OS_PAGES) - 1);
            gone[k] = drop;
            if (!drop) continue;
            cx_u32 holes = 0;
            for (cx_u32 b = 0; b < pi->cut; b++) {
                if (block_span(b, size) & drop) holes++;
            }
            pi->holes = (cx_u16)(pi->holes + holes);
        }
    }
    if (going || parts) {
        cx_u32 kept_batches = 0, total = 0;
        for (cx_u32 i = 0; i < s->n; i++) {
            void* head = NULL;
            void* tail = NULL;
            cx_u32 count = 0;
            for (void* q = s->b[i].head; q;) {
                void* next = *(void**)q;
                cx_u32 n = page_number(q);
                int keep = n == NO_PAGE || page_of[n].state != PAGE_FREE;
                if (keep && n != NO_PAGE && slot && slot[n]) {
                    cx_u32 b = (cx_u32)((cx_usize)((cx_u8*)q - (arena + ((cx_usize)n << PAGE_SHIFT))) / size);
                    keep = !(block_span(b, size) & gone[slot[n] - 1]);
                }
                if (keep) {
                    if (tail) *(void**)tail = q;
                    else head = q;
                    tail = q;
                    count++;
                }
                q = next;
            }
            if (!count) continue;
            *(void**)tail = NULL;
            s->b[kept_batches].head = head;
            s->b[kept_batches].last = tail;
            s->b[kept_batches].count = count;
            kept_batches++;
            total += count;
        }
        s->n = kept_batches;
        __atomic_store_n(&s->count, total, __ATOMIC_RELAXED);
    }
    unlock(&s->lock);
    /* What's no longer in any list goes back: whole pages, and runs of system pages. */
    for (cx_u32 i = 0; i < going; i++) give_page(seen[i]);
    for (cx_u32 k = 0; k < parts; k++) {
        cx_u32 drop = gone[k];
        cx_u8* page = arena + ((cx_usize)part[k] << PAGE_SHIFT);
        for (cx_u32 j = 0; j < OS_PAGES;) {
            if (!(drop >> j & 1)) {
                j++;
                continue;
            }
            cx_u32 run = j;
            while (run < OS_PAGES && drop >> run & 1) run++;
            decommit(page + (cx_usize)j * OS_PAGE, (cx_usize)(run - j) * OS_PAGE);
            holed += (cx_usize)(run - j) * OS_PAGE;
            j = run;
        }
    }
    if (parts) {
        os_free(slot, list_bytes);
        os_free(free_bits, bits_bytes);
        os_free(gone, gone_bytes);
    }
    os_free(part, list_bytes);
    os_free(seen, list_bytes);
    return ((cx_usize)going << PAGE_SHIFT) + holed;
}

/* How often a thread taking a page trims: after this many pages, 64 MiB, and then only the class
 * whose shared lists hold the most, if that's at least 16 MiB. */
#define TRIM_EVERY 1024
#define TRIM_AT ((cx_usize)16 << 20)

static void trim_some(void) {
    unsigned most = 0;
    cx_usize bytes = 0;
    for (unsigned c = 0; c < CLASSES; c++) {
        cx_usize b = (cx_usize)__atomic_load_n(&shared[c].count, __ATOMIC_RELAXED) * class_size(c);
        if (b > bytes) {
            bytes = b;
            most = c;
        }
    }
    if (bytes >= TRIM_AT) (void)trim_class(most, 0);
}

/* A new page to cut class `c` from; the one cut from before is sealed. */
static void new_page(heap* h, unsigned c) {
    seal(h, c);
    if (__atomic_add_fetch(&taken_since_trim, 1, __ATOMIC_RELAXED) >= TRIM_EVERY) {
        __atomic_store_n(&taken_since_trim, 0, __ATOMIC_RELAXED);
        trim_some();
    }
    cx_u32 n;
    cx_u8* p = take_page(c, &n);
    h->page[c] = n;
    h->cutting[c] = 1;
    h->bump[c] = p;
    h->end[c] = p + PAGE / class_size(c) * class_size(c);
}

/* When a thread ends, its pages are sealed and its free blocks go to the shared lists. */
static void give_back(heap* h) {
    for (unsigned c = 0; c < CLASSES; c++) {
        seal(h, c);
        h->bump[c] = h->end[c] = NULL;
        if (h->free[c]) share(h, c);
    }
}

/* Set as the process exits: a thread that ends then gives nothing back. */
static int exiting;

static void note_exit(void) {
    __atomic_store_n(&exiting, 1, __ATOMIC_RELEASE);
}

#ifdef _WIN32
static DWORD fls_index;
/* As the process exits, Windows kills its other threads wherever they are, maybe holding a shared
 * list's lock, then runs this for the thread exiting it: giving back would take that lock and
 * spin forever. What it would give back goes with the process, so it gives back nothing once
 * the loader's shutting down, or the C runtime's exiting. */
typedef BOOLEAN(NTAPI* shutdown_check)(void);
static shutdown_check shutting_down;
static void NTAPI thread_ends(void* h) {
    if (!h || __atomic_load_n(&exiting, __ATOMIC_ACQUIRE)) return;
    if (shutting_down && shutting_down()) return;
    give_back((heap*)h);
}
#else
static pthread_key_t key;
static void thread_ends(void* h) {
    if (h) give_back((heap*)h);
}
#endif
static int ready_lock, ready;

static void know(heap* h) {
    h->known = 1;
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) {
        lock(&ready_lock);
        if (!ready) {
            atexit(note_exit);
#ifdef _WIN32
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            if (ntdll) shutting_down = (shutdown_check)(void*)GetProcAddress(ntdll, "RtlDllShutdownInProgress");
            fls_index = FlsAlloc(thread_ends);
#else
            pthread_key_create(&key, thread_ends);
#endif
            __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
        }
        unlock(&ready_lock);
    }
#ifdef _WIN32
    if (fls_index != FLS_OUT_OF_INDEXES) FlsSetValue(fls_index, h);
#else
    pthread_setspecific(key, h);
#endif
}

/* This thread has no free block of class `c`: take a batch of the shared ones, or cut a new one. */
static __attribute__((noinline)) void* refill(heap* h, unsigned c) {
    if (!h->known) know(h);
    shared_list* s = &shared[c];
    if (__atomic_load_n(&s->count, __ATOMIC_RELAXED)) {
        lock(&s->lock);
        void* p = NULL;
        if (s->n) {
            batch b = s->b[--s->n];
            p = b.head;
            h->free[c] = *(void**)p;
            h->last[c] = h->free[c] ? b.last : NULL;
            h->count[c] = b.count - 1;
            __atomic_store_n(&s->count, s->count - b.count, __ATOMIC_RELAXED);
        }
        unlock(&s->lock);
        if (p) return p;
    }
    cx_usize size = class_size(c);
    if ((cx_usize)(h->end[c] - h->bump[c]) < size) new_page(h, c);
    void* p = h->bump[c];
    h->bump[c] += size;
    return p;
}

/* --- big blocks ------------------------------------------------------------------------- */

static int large_lock;
static void* large_free[LARGE_CLASSES];
static cx_usize cached;

static void* large_alloc(cx_usize n) {
    if (n > HUGE_BLOCK) return os_alloc(pages(n));
    unsigned c = large_class(n);
    if (__atomic_load_n(&large_free[c], __ATOMIC_RELAXED)) {
        lock(&large_lock);
        void* p = large_free[c];
        if (p) {
            __atomic_store_n(&large_free[c], *(void**)p, __ATOMIC_RELAXED);
            cached -= large_size(c);
        }
        unlock(&large_lock);
        if (p) return p;
    }
    return os_alloc(large_size(c));
}

static void large_release(void* p, cx_usize n) {
    if (n > HUGE_BLOCK) {
        os_free(p, pages(n));
        return;
    }
    unsigned c = large_class(n);
    cx_usize size = large_size(c);
    lock(&large_lock);
    if (cached + size <= CACHE_MAX) {
        *(void**)p = large_free[c];
        __atomic_store_n(&large_free[c], p, __ATOMIC_RELAXED);
        cached += size;
        p = NULL;
    }
    unlock(&large_lock);
    if (p) os_free(p, size);
}

/* Gives back every big block the cache holds. Returns the bytes. */
static cx_usize trim_large(void) {
    void* lists[LARGE_CLASSES];
    lock(&large_lock);
    for (unsigned c = 0; c < LARGE_CLASSES; c++) {
        lists[c] = large_free[c];
        __atomic_store_n(&large_free[c], NULL, __ATOMIC_RELAXED);
    }
    cx_usize bytes = cached;
    cached = 0;
    unlock(&large_lock);
    for (unsigned c = 0; c < LARGE_CLASSES; c++) {
        for (void* p = lists[c]; p;) {
            void* next = *(void**)p;
            os_free(p, large_size(c));
            p = next;
        }
    }
    return bytes;
}

/* --- the interface ---------------------------------------------------------------------- */

void* cx_alloc(cx_usize n) {
    if (n <= SMALL_MAX) {
        unsigned c = small_class(n);
        heap* h = &tls;
        void* p = h->free[c];
        if (__builtin_expect(p != NULL, 1)) {
            h->free[c] = *(void**)p;
            h->count[c]--;
            return p;
        }
        return refill(h, c);
    }
    if (n >= TOO_BIG) cx_panic_capacity();
    return large_alloc(n);
}

void cx_free(void* p, cx_usize n) {
    if (!p) return;
    if (n <= SMALL_MAX) {
        unsigned c = small_class(n);
        heap* h = &tls;
        put_free(h, c, p);
        if (__builtin_expect(h->count[c] > kept[c], 0)) share(h, c);
        return;
    }
    large_release(p, n);
}

void* cx_realloc(void* p, cx_usize old, cx_usize n) { return cx_realloc_keep(p, old, n, old); }

/* Only the first `keep` bytes move to a new block: a list that reserved room it hasn't used
 * copies what it holds, not the room. */
void* cx_realloc_keep(void* p, cx_usize old, cx_usize n, cx_usize keep) {
    if (!p) return cx_alloc(n);
    /* A block of the same size class already fits. */
    if (old <= SMALL_MAX && n <= SMALL_MAX) {
        if (small_class(old) == small_class(n)) return p;
    } else if (old > SMALL_MAX && n > SMALL_MAX && old <= HUGE_BLOCK && n <= HUGE_BLOCK) {
        if (large_class(old) == large_class(n)) return p;
    }
    void* q = cx_alloc(n);
    const cx_usize most = old < n ? old : n;
    memcpy(q, p, keep < most ? keep : most);
    cx_free(p, old);
    return q;
}

cx_usize cx_alloc_live(void) { return 0; }
int cx_alloc_report(void) { return 0; }

/* Gives the system back what freed blocks hold that it can: this thread's free blocks are shared
 * first, then each small class's whole free pages go, with `with_holes` the free system pages of
 * those still in use too, and the big blocks the cache keeps. Free blocks other threads keep for
 * themselves, at most 64 KiB of each size each, stay. Returns the bytes given back. */
static cx_usize trim_all(int with_holes) {
    heap* h = &tls;
    for (unsigned c = 0; c < CLASSES; c++) {
        if (h->free[c]) share(h, c);
    }
    cx_usize bytes = 0;
    for (unsigned c = 0; c < CLASSES; c++) bytes += trim_class(c, with_holes);
    return bytes + trim_large();
}

/* This thread has waited a while on a queue, with nothing to do: it cuts no more from its pages
 * and shares its free blocks, as it would if it ended, so a trim can give back the pages they
 * held up, and then trims. A busy thread keeps them, so it takes and frees without a lock; an
 * idle one's would only sit there (CLX-117). Other threads may be busy, so it gives back whole
 * pages, and leaves the free blocks of pages in use for them. */
void cx_alloc_idle(void) {
    heap* h = &tls;
    for (unsigned c = 0; c < CLASSES; c++) {
        seal(h, c);
        h->bump[c] = h->end[c] = NULL;
        if (h->free[c]) share(h, c);
    }
    (void)trim_all(0);
}

/* `std.memory.trim`: a program gone quiet gives back all it can, holes too (CLX-119). */
cx_usize cx_alloc_trim(void) { return trim_all(1); }

#endif
