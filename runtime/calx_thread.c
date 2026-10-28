/* std.thread: the threads a program's core runs its services on, and the queues they talk to the
 * rest through (D49, D121). Linked only into programs that use them.
 *
 * A queue is a ring of messages, each `size` bytes, moved in and out by copying their bytes: a
 * Calx value has one owner, so a message the queue holds is the queue's until a receiver takes
 * it. A lock guards the ring, with a condition a waiting receiver sleeps on. The queue counts its
 * ends, and the last one to go frees it, with any messages still in it. Once every receiver has
 * gone, nothing could take a message, so the queue drops those it holds and every one sent
 * after; once every sender has gone, `wait` gives `none` when the ring is empty, which is how a
 * service's loop ends. */

#include "calx_rt.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

int cx_in_parallel(void);
CX_COLD void cx_panic_text(const char* at, const char* message);
void cx_service_started(void);
#ifdef _WIN32
int cx_os_thread(HANDLE* thread, LPTHREAD_START_ROUTINE run, void* arg);
#else
int cx_os_thread(pthread_t* thread, void* (*run)(void*), void* arg);
#endif
void cx_service_enter(void);
void cx_service_leave(void);

struct cx_queue {
#ifdef _WIN32
    SRWLOCK lock;
    CONDITION_VARIABLE ready;
#else
    pthread_mutex_t lock;
    pthread_cond_t ready;
#endif
    unsigned char* ring;
    cx_usize cap, head, len; /* in messages */
    cx_usize size;           /* each message's bytes */
    void (*drop)(void*);     /* frees what a message owns, or null when it owns nothing */
    cx_usize senders, receivers, waiting;
};

#ifdef _WIN32
static void q_lock(cx_queue* q) { AcquireSRWLockExclusive(&q->lock); }
static void q_unlock(cx_queue* q) { ReleaseSRWLockExclusive(&q->lock); }
static void q_wait(cx_queue* q) { SleepConditionVariableSRW(&q->ready, &q->lock, INFINITE, 0); }
static void q_wake_one(cx_queue* q) { WakeConditionVariable(&q->ready); }
static void q_wake_all(cx_queue* q) { WakeAllConditionVariable(&q->ready); }
#else
static void q_lock(cx_queue* q) { pthread_mutex_lock(&q->lock); }
static void q_unlock(cx_queue* q) { pthread_mutex_unlock(&q->lock); }
static void q_wait(cx_queue* q) { pthread_cond_wait(&q->ready, &q->lock); }
static void q_wake_one(cx_queue* q) { pthread_cond_signal(&q->ready); }
static void q_wake_all(cx_queue* q) { pthread_cond_broadcast(&q->ready); }
#endif

/* Which message came first would depend on the threads, one thread or many, so it's the same
 * panic either way. */
static void q_check(const char* at) {
    if (cx_in_parallel()) {
        cx_panic_text(at, "a queue can't be used inside a `parallel for`, since the order of the iterations' "
                          "messages would depend on timing");
    }
}

static unsigned char* slot(cx_queue* q, cx_usize i) {
    cx_usize at = q->head + i;
    if (at >= q->cap) at -= q->cap;
    return q->ring + at * q->size;
}

/* Drops the messages in a ring taken from its queue, and frees it. It's outside the lock, since a
 * message can hold an end of the same queue, whose drop takes the lock. */
static void drop_ring(unsigned char* ring, cx_usize cap, cx_usize head, cx_usize len, cx_usize size, void (*drop)(void*)) {
    if (drop) {
        for (cx_usize i = 0, at = head; i < len; i++, at = at + 1 == cap ? 0 : at + 1) drop(ring + at * size);
    }
    if (cap) cx_free(ring, cap * size);
}

/* Frees a queue that no end reaches, so nothing else can hold its lock. */
static void q_free(cx_queue* q) {
    drop_ring(q->ring, q->cap, q->head, q->len, q->size, q->drop);
#ifndef _WIN32
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->ready);
#endif
    cx_free(q, sizeof *q);
}

cx_Receiver cx_queue_new(cx_usize size, void (*drop)(void*)) {
    cx_queue* q = (cx_queue*)cx_alloc(sizeof *q);
    memset(q, 0, sizeof *q);
#ifdef _WIN32
    InitializeSRWLock(&q->lock);
    InitializeConditionVariable(&q->ready);
#else
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->ready, NULL);
#endif
    q->size = size ? size : 1;
    q->drop = drop;
    q->receivers = 1;
    return (cx_Receiver){q};
}

cx_Sender cx_queue_sender(const cx_Receiver* r) {
    cx_queue* q = r->q;
    q_lock(q);
    q->senders++;
    q_unlock(q);
    return (cx_Sender){q};
}

cx_Sender cx_queue_share_sender(const cx_Sender* s) {
    cx_queue* q = s->q;
    q_lock(q);
    q->senders++;
    q_unlock(q);
    return (cx_Sender){q};
}

cx_Receiver cx_queue_share_receiver(const cx_Receiver* r) {
    cx_queue* q = r->q;
    q_lock(q);
    q->receivers++;
    q_unlock(q);
    return (cx_Receiver){q};
}

void cx_queue_send(const cx_Sender* s, void* message, const char* at) {
    q_check(at);
    cx_queue* q = s->q;
    q_lock(q);
    if (!q->receivers) {
        /* Nothing can ever take it. */
        q_unlock(q);
        if (q->drop) q->drop(message);
        return;
    }
    if (q->len == q->cap) {
        cx_usize cap = q->cap ? q->cap * 2 : 16;
        unsigned char* ring = (unsigned char*)cx_alloc(cap * q->size);
        for (cx_usize i = 0; i < q->len; i++) memcpy(ring + i * q->size, slot(q, i), q->size);
        if (q->cap) cx_free(q->ring, q->cap * q->size);
        q->ring = ring;
        q->cap = cap;
        q->head = 0;
    }
    memcpy(slot(q, q->len), message, q->size);
    q->len++;
    if (q->waiting) q_wake_one(q);
    q_unlock(q);
}

/* Takes the first message into `out`, under the lock. */
static void take(cx_queue* q, void* out) {
    memcpy(out, q->ring + q->head * q->size, q->size);
    q->head = q->head + 1 == q->cap ? 0 : q->head + 1;
    q->len--;
}

cx_bool cx_queue_next(const cx_Receiver* r, void* out, const char* at) {
    q_check(at);
    cx_queue* q = r->q;
    q_lock(q);
    cx_bool got = q->len != 0;
    if (got) take(q, out);
    q_unlock(q);
    return got;
}

cx_bool cx_queue_wait(const cx_Receiver* r, void* out, const char* at) {
    q_check(at);
    cx_queue* q = r->q;
    q_lock(q);
    while (!q->len && q->senders) {
        q->waiting++;
        q_wait(q);
        q->waiting--;
    }
    cx_bool got = q->len != 0;
    if (got) take(q, out);
    q_unlock(q);
    return got;
}

/* A moved-out end is zero, and dropping it does nothing. */
void cx_drop_Sender(cx_Sender* s) {
    cx_queue* q = s->q;
    if (!q) return;
    q_lock(q);
    q->senders--;
    int last = !q->senders && !q->receivers;
    /* A receiver waiting for a message now knows none can come. */
    if (!q->senders && q->waiting) q_wake_all(q);
    q_unlock(q);
    if (last) q_free(q);
}

void cx_drop_Receiver(cx_Receiver* r) {
    cx_queue* q = r->q;
    if (!q) return;
    q_lock(q);
    q->receivers--;
    if (q->receivers) {
        q_unlock(q);
        return;
    }
    /* Nothing can take the messages left, so they go now, and every one sent from now on when
     * it's sent. A sender that's left may be in one of them, and the last sender to go frees the
     * queue then; with none left, nothing reaches the queue but this. */
    int last = !q->senders;
    unsigned char* ring = q->ring;
    cx_usize cap = q->cap, head = q->head, len = q->len, size = q->size;
    void (*drop)(void*) = q->drop;
    q->ring = 0;
    q->cap = q->head = q->len = 0;
    q_unlock(q);
    drop_ring(ring, cap, head, len, size, drop);
    if (last) q_free(q);
}

/* --- threads ------------------------------------------------------------------------------- */

typedef struct {
    void (*run)(void*);
    void* task;
} thread_start;

static void begin(thread_start* s) {
    void (*run)(void*) = s->run;
    void* task = s->task;
    cx_free(s, sizeof *s);
    cx_service_enter();
    run(task);
    cx_service_leave();
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) {
    begin((thread_start*)p);
    return 0;
}
#else
static void* thread_main(void* p) {
    begin((thread_start*)p);
    return NULL;
}
#endif

cx_Thread cx_thread_spawn(void (*run)(void*), void* task) {
    thread_start* s = (thread_start*)cx_alloc(sizeof *s);
    s->run = run;
    s->task = task;
    /* Counted before it starts, so a program that ends at once knows a thread is running. */
    cx_service_started();
#ifdef _WIN32
    HANDLE h;
    if (cx_os_thread(&h, thread_main, s)) cx_panic_text("std.thread", "can't start a thread");
    return (cx_Thread){(cx_u64)(uintptr_t)h};
#else
    pthread_t t;
    if (cx_os_thread(&t, thread_main, s)) cx_panic_text("std.thread", "can't start a thread");
    return (cx_Thread){(cx_u64)t};
#endif
}

void cx_thread_join(cx_Thread* t, const char* at) {
    if (cx_in_parallel()) {
        cx_panic_text(at, "a thread can't be joined inside a `parallel for`, which would wait on timing");
    }
#ifdef _WIN32
    HANDLE h = (HANDLE)(uintptr_t)t->h;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
#else
    pthread_join((pthread_t)t->h, NULL);
#endif
}

/* A thread that's dropped without `join` carries on by itself until it ends, or the program does. */
void cx_drop_Thread(cx_Thread* t) {
    if (!t->h) return;
#ifdef _WIN32
    CloseHandle((HANDLE)(uintptr_t)t->h);
#else
    pthread_detach((pthread_t)t->h);
#endif
}
