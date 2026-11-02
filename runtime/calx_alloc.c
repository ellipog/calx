/* The memory behind cx_alloc, cx_realloc and cx_free (D95).
 *
 * Every free says how big the block is, since the compiler always knows: a list frees `cap`
 * elements, a String `cap` bytes, a table what its mask gives. So a block has no header, and a
 * free never has to find out what an address belongs to.
 *
 * Small blocks, up to 32 KiB, come in 40 sizes: steps of 16 bytes up to 128, then four steps for
 * each doubling, so less than a fifth of a block goes unused. Each thread keeps a list of free
 * blocks of each size, and takes from and gives to it with no lock. A thread that frees more than
 * it takes shares its list, about 256 KiB of blocks, as one batch, and a thread that runs out
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
 *
 * Bigger blocks come from the system, rounded to four sizes for each doubling, and a freed one
 * waits in a cache for the next block of its size, up to 256 MiB in all. The Windows heap gives
 * big freed blocks back to the system, so a table that grew and was freed would fault on every
 * page of the next one; from the cache, its pages are still there.
 *
 * Debug builds use malloc instead, with a header that holds each block's size, so a free with
 * the wrong size or a second free stops the program: either is a compiler bug. So do builds
 * with AddressSanitizer or CX_SYSTEM_ALLOC, so tools that watch malloc see every block. */

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

void* cx_alloc(cx_usize size) {
    if (size >= TOO_BIG - sizeof(block_head)) cx_panic_capacity();
    block_head* h = (block_head*)malloc(sizeof(block_head) + size);
    if (!h) cx_panic_capacity();
    h->size = size;
    h->state = LIVE;
    __atomic_fetch_add(&live, 1, __ATOMIC_RELAXED);
    return h + 1;
}

void* cx_realloc(void* p, cx_usize old, cx_usize size) {
    if (!p) return cx_alloc(size);
    if (size >= TOO_BIG - sizeof(block_head)) cx_panic_capacity();
    block_head* h = (block_head*)realloc(head_of(p, old), sizeof(block_head) + size);
    if (!h) cx_panic_capacity();
    h->size = size;
    return h + 1;
}

void cx_free(void* p, cx_usize size) {
    if (!p) return;
    block_head* h = head_of(p, size);
    h->state = FREED;
    __atomic_fetch_sub(&live, 1, __ATOMIC_RELAXED);
    free(h);
}

cx_usize cx_alloc_live(void) { return __atomic_load_n(&live, __ATOMIC_RELAXED); }

/* malloc keeps what it keeps. */
cx_usize cx_alloc_trim(void) { return 0; }

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
    cx_u16 seen; /* while a trim counts: its blocks in the shared lists */
    cx_u8 state;
    cx_u8 cls;
} page_info;

static cx_u8* arena;
static int arena_tried;
static page_info page_of[ARENA_PAGES];
static int page_lock;
static cx_u32 next_page;
static cx_u32 free_pages[ARENA_PAGES];
static cx_u32 free_count;
/* Pages taken since the last trim a thread made as it took one. */
static cx_u32 taken_since_trim;

static inline cx_u32 page_number(const void* p) {
    cx_usize at = (cx_usize)((const cx_u8*)p - arena);
    return arena && (const cx_u8*)p >= arena && at < (cx_usize)ARENA_PAGES << PAGE_SHIFT ? (cx_u32)(at >> PAGE_SHIFT) : NO_PAGE;
}

static void reserve_arena(void) {
    cx_usize n = (cx_usize)ARENA_PAGES << PAGE_SHIFT;
#ifdef _WIN32
    arena = (cx_u8*)VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_READWRITE);
#else
    void* a = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    arena = a == MAP_FAILED ? NULL : (cx_u8*)a;
#endif
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
    else if (arena && next_page < ARENA_PAGES) n = next_page++;
    if (n != NO_PAGE) {
        page_of[n].state = PAGE_CUTTING;
        page_of[n].cls = (cx_u8)c;
        page_of[n].cut = 0;
        page_of[n].seen = 0;
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

/* Gives page `n` back to the system, and to the free pages. */
static void give_page(cx_u32 n) {
    cx_u8* p = arena + ((cx_usize)n << PAGE_SHIFT);
#ifdef _WIN32
    VirtualFree(p, PAGE, MEM_DECOMMIT);
#else
    madvise(p, PAGE, MADV_DONTNEED);
#endif
    lock(&page_lock);
    page_of[n].state = PAGE_FREE;
    page_of[n].cut = 0;
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
    batch b[BATCHES];
} shared_list;

static _Thread_local heap tls;
static shared_list shared[CLASSES];

/* How many free blocks of each size a thread keeps before it shares them: 256 KiB of them, and
 * at least 16. */
static const cx_u32 kept[CLASSES] = {
    16384, 8192, 5461, 4096, 3276, 2730, 2340, 2048, 1638, 1365, 1170, 1024, 819, 682,
    585,   512,  409,  341,  292,  256,  204,  170,  146,  128,  102,  85,   73,  64,
    51,    42,   36,   32,   25,   21,   18,   16,   16,   16,   16,   16,
};

static void share(heap* h, unsigned c) {
    shared_list* s = &shared[c];
    lock(&s->lock);
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

/* Gives back each sealed page of class `c` whose every block is in the shared lists: the lists
 * are counted with the class's lock held, so none of those blocks can be taken meanwhile, and
 * those blocks leave them. Returns the bytes given back. */
static cx_usize trim_class(unsigned c) {
    shared_list* s = &shared[c];
    if (!arena || !__atomic_load_n(&s->count, __ATOMIC_RELAXED)) return 0;
    cx_u32 most = __atomic_load_n(&next_page, __ATOMIC_ACQUIRE);
    if (!most) return 0;
    /* The pages it sees, each once. */
    cx_u32* seen = (cx_u32*)os_alloc(pages((cx_usize)most * sizeof(cx_u32)));
    cx_u32 nseen = 0;
    cx_u32 going = 0;
    lock(&s->lock);
    for (cx_u32 i = 0; i < s->n; i++) {
        for (void* q = s->b[i].head; q; q = *(void**)q) {
            cx_u32 n = page_number(q);
            if (n == NO_PAGE || __atomic_load_n(&page_of[n].state, __ATOMIC_ACQUIRE) != PAGE_SEALED) continue;
            if (page_of[n].seen++ == 0) seen[nseen++] = n;
        }
    }
    /* A page goes when the lists hold every block cut from it: its state says so until it's given
     * back, and every other page's count starts again at 0. */
    for (cx_u32 i = 0; i < nseen; i++) {
        page_info* pi = &page_of[seen[i]];
        if (pi->cut && pi->seen == pi->cut) {
            pi->state = PAGE_FREE;
            seen[going++] = seen[i];
        }
        pi->seen = 0;
    }
    if (going) {
        cx_u32 kept_batches = 0, total = 0;
        for (cx_u32 i = 0; i < s->n; i++) {
            void* head = NULL;
            void* tail = NULL;
            cx_u32 count = 0;
            for (void* q = s->b[i].head; q;) {
                void* next = *(void**)q;
                cx_u32 n = page_number(q);
                if (n == NO_PAGE || page_of[n].state != PAGE_FREE) {
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
    for (cx_u32 i = 0; i < going; i++) give_page(seen[i]);
    os_free(seen, pages((cx_usize)most * sizeof(cx_u32)));
    return (cx_usize)going << PAGE_SHIFT;
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
    if (bytes >= TRIM_AT) (void)trim_class(most);
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

#ifdef _WIN32
static DWORD fls_index;
static void NTAPI thread_ends(void* h) {
    if (h) give_back((heap*)h);
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
#ifdef _WIN32
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

void* cx_realloc(void* p, cx_usize old, cx_usize n) {
    if (!p) return cx_alloc(n);
    /* A block of the same size class already fits. */
    if (old <= SMALL_MAX && n <= SMALL_MAX) {
        if (small_class(old) == small_class(n)) return p;
    } else if (old > SMALL_MAX && n > SMALL_MAX && old <= HUGE_BLOCK && n <= HUGE_BLOCK) {
        if (large_class(old) == large_class(n)) return p;
    }
    void* q = cx_alloc(n);
    memcpy(q, p, old < n ? old : n);
    cx_free(p, old);
    return q;
}

cx_usize cx_alloc_live(void) { return 0; }

/* Gives the system back what freed blocks hold that it can: this thread's free blocks are shared
 * first, then each small class's whole free pages go, and the big blocks the cache keeps. Free
 * blocks other threads keep for themselves, at most 256 KiB of each size each, stay. Returns the
 * bytes given back. */
cx_usize cx_alloc_trim(void) {
    heap* h = &tls;
    for (unsigned c = 0; c < CLASSES; c++) {
        if (h->free[c]) share(h, c);
    }
    cx_usize bytes = 0;
    for (unsigned c = 0; c < CLASSES; c++) bytes += trim_class(c);
    return bytes + trim_large();
}

#endif
