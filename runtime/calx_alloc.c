/* The memory behind cx_alloc, cx_realloc and cx_free (D95).
 *
 * Every free says how big the block is, since the compiler always knows: a list frees `cap`
 * elements, a String `cap` bytes, a table what its mask gives. So a block has no header, and a
 * free never has to find out what an address belongs to.
 *
 * Small blocks, up to 32 KiB, come in 40 sizes: steps of 16 bytes up to 128, then four steps for
 * each doubling, so less than a fifth of a block goes unused. Each thread keeps a list of free
 * blocks of each size, and takes from and gives to it with no lock. A thread that frees more than
 * it takes moves its list to a shared one, which a thread that runs out takes from first, so
 * memory freed by one thread goes back to the threads that allocate. New blocks are cut from
 * 1 MiB chunks of fresh memory.
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
#define CHUNK ((cx_usize)1 << 20)
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

/* --- small blocks ----------------------------------------------------------------------- */

/* A free block's first word points at the next one. A list's last block is remembered, so a
 * whole list moves between a thread and the shared lists in one step. */
typedef struct {
    void* free[CLASSES];
    void* last[CLASSES];
    cx_u32 count[CLASSES];
    cx_u8* bump; /* where the next new block is cut from, up to `end` */
    cx_u8* end;
    int known; /* asked to be told when the thread ends */
} heap;

typedef struct {
    int lock;
    cx_u32 count;
    void* head;
    void* last;
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

/* The rest of a chunk a thread no longer cuts from, for the next thread that needs one. */
typedef struct spare {
    struct spare* next;
    cx_u8* end;
} spare;

static int spare_lock;
static spare* spares;

static void share(heap* h, unsigned c) {
    shared_list* s = &shared[c];
    lock(&s->lock);
    *(void**)h->last[c] = s->head;
    if (!s->head) s->last = h->last[c];
    s->head = h->free[c];
    /* Atomic, since `take` reads it without the lock to see whether to take it. */
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

/* A new chunk to cut from. What's left of the old one becomes free blocks, biggest first. */
static void new_chunk(heap* h) {
    for (;;) {
        cx_usize left = (cx_usize)(h->end - h->bump);
        if (left < 16) break;
        unsigned c = small_class(left > SMALL_MAX ? SMALL_MAX : left);
        if (class_size(c) > left) c--;
        put_free(h, c, h->bump);
        h->bump += class_size(c);
    }
    spare* s = NULL;
    if (__atomic_load_n(&spares, __ATOMIC_RELAXED)) {
        lock(&spare_lock);
        s = spares;
        if (s) spares = s->next;
        unlock(&spare_lock);
    }
    if (s) {
        h->bump = (cx_u8*)s;
        h->end = s->end;
    } else {
        h->bump = (cx_u8*)os_alloc(CHUNK);
        h->end = h->bump + CHUNK;
    }
}

/* When a thread ends, its free blocks and the rest of its chunk go to the shared lists. */
static void give_back(heap* h) {
    for (unsigned c = 0; c < CLASSES; c++) {
        if (h->free[c]) share(h, c);
    }
    if ((cx_usize)(h->end - h->bump) >= sizeof(spare)) {
        spare* s = (spare*)h->bump;
        s->end = h->end;
        lock(&spare_lock);
        s->next = spares;
        spares = s;
        unlock(&spare_lock);
    }
    h->bump = h->end = NULL;
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

/* This thread has no free block of class `c`: take the shared ones, or cut a new one. */
static __attribute__((noinline)) void* refill(heap* h, unsigned c) {
    if (!h->known) know(h);
    shared_list* s = &shared[c];
    if (__atomic_load_n(&s->count, __ATOMIC_RELAXED)) {
        lock(&s->lock);
        void* p = s->head;
        if (p) {
            h->free[c] = *(void**)p;
            h->last[c] = h->free[c] ? s->last : NULL;
            h->count[c] = s->count - 1;
            s->head = s->last = NULL;
            __atomic_store_n(&s->count, 0, __ATOMIC_RELAXED);
        }
        unlock(&s->lock);
        if (p) return p;
    }
    cx_usize size = class_size(c);
    if ((cx_usize)(h->end - h->bump) < size) new_chunk(h);
    void* p = h->bump;
    h->bump += size;
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

#endif
