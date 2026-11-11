/* The Calx runtime, as generated code sees it.
 *
 * Every C file the compiler emits includes this header and nothing else, so it includes no
 * system headers: the types come from clang's predefined macros. The helpers here are the parts
 * of Java's semantics that C leaves undefined (CALX_SPEC.md, "Integer and float semantics"). */
#ifndef CALX_RT_H
#define CALX_RT_H

typedef __INT8_TYPE__ cx_i8;
typedef __INT16_TYPE__ cx_i16;
typedef __INT32_TYPE__ cx_i32;
typedef __INT64_TYPE__ cx_i64;
typedef __UINT8_TYPE__ cx_u8;
typedef __UINT16_TYPE__ cx_u16;
typedef __UINT32_TYPE__ cx_u32;
typedef __UINT64_TYPE__ cx_u64;
typedef __SIZE_TYPE__ cx_usize;
typedef _Bool cx_bool;
typedef cx_u16 cx_char;
typedef cx_u32 cx_rune;
typedef float cx_f32;
typedef double cx_f64;

typedef struct {
    const cx_u8* ptr;
    cx_usize len;
} cx_str;

/* The runtime's inline functions have no debug information of their own, so code inlined from
 * them takes the Calx line it was called from, which is what debuggers and stack traces show. */
#define CX_INLINE static inline __attribute__((always_inline, nodebug))
#define CX_COLD __attribute__((cold, noreturn))

/* The runtime's functions (D123). In a program each is a function. In a library, built with
 * `calx build --lib`, each is a pointer to the program's own, filled in when the program opens
 * the library, so the two share one allocator, one output and one way to panic. The compiler
 * reads the list from here: every function declared with `CX_FN`, in order. */
#ifdef CX_LIBRARY
#define CX_FN(ret, name, params) extern ret(*name) params
#else
#define CX_FN(ret, name, params) ret name params
#endif

/* Each function starts with this for every view parameter: a view points at a whole, live,
 * aligned value for the whole call, since views can't be stored or outlive what they view (D48).
 * Saying so lets clang read a view's fields once before a loop instead of on every pass, even
 * after the function is inlined, which a parameter attribute wouldn't survive. */
#if __has_builtin(__builtin_assume_dereferenceable)
#define CX_VIEW(p) (__builtin_assume_dereferenceable((p), sizeof(*(p))), \
                    (void)__builtin_assume_aligned((p), _Alignof(__typeof__(*(p)))))
#else
#define CX_VIEW(p) ((void)__builtin_assume_aligned((p), _Alignof(__typeof__(*(p)))))
#endif

/* A C function's name as the linker sees it, for `extern "C"` declarations: an asm label gives
 * it, so the name Calx calls it by can't meet one a header declares. */
#define CX_SYMBOL_2(prefix, name) #prefix name
#define CX_SYMBOL_1(prefix, name) CX_SYMBOL_2(prefix, name)
#define CX_SYMBOL(name) CX_SYMBOL_1(__USER_LABEL_PREFIX__, name)

/* `String`: UTF-8 bytes. With `cap` 0 it doesn't own them: an empty string, or a string literal,
 * which costs nothing to make. Changing such a string copies it to the heap first. All zero bits
 * is the empty string, so a value that was moved away can be zeroed and dropped again safely. */
typedef struct {
    cx_u8* ptr;
    cx_usize len;
    cx_usize cap;
} cx_String;

/* Startup and exit, called by the generated `main`. */
CX_FN(void, cx_start, (int argc, char** argv));
CX_FN(int, cx_finish, (void));
/* A test program's start, for `calx test`: `cx_start`, then the test it's asked to run, or -1. */
CX_FN(cx_i64, cx_test_start, (int argc, char** argv));

/* The command line, in UTF-8, behind std.env. */
CX_FN(cx_usize, cx_arg_count, (void));
CX_FN(const cx_u8*, cx_arg, (cx_usize i));
CX_FN(cx_usize, cx_arg_len, (cx_usize i));
CX_FN(cx_i64, cx_env_get, (const cx_u8* name, cx_usize n, cx_u8* out, cx_usize cap));
CX_FN(cx_u32, cx_env_os, (void));
CX_FN(const cx_u8*, cx_script, (void));
CX_FN(cx_usize, cx_script_len, (void));

/* Nanoseconds from a clock that only goes forward, behind std.time (D88). */
CX_FN(cx_u64, cx_clock_ns, (void));
/* The wall clock, in milliseconds since 1970 in UTC, and the local time zone's offset from UTC
 * at a moment, in seconds, behind std.time's `Timestamp`. */
CX_FN(cx_i64, cx_clock_unix_ms, (void));
CX_FN(cx_i32, cx_clock_local_offset, (cx_i64 unix_ms));
/* The local time zone's abbreviation at a moment, such as `CET`, written to `out` if it fits in
 * `cap` bytes: its length, more than `cap` if it didn't fit, or -1 where there's none, as on
 * Windows. */
CX_FN(cx_i64, cx_clock_zone_abbreviation, (cx_i64 unix_ms, cx_u8* out, cx_usize cap));

/* Output. Standard output is buffered, and flushed at exit and before a panic. */
CX_FN(void, cx_print_str, (cx_str s));
CX_FN(void, cx_print_i64, (cx_i64 v));
CX_FN(void, cx_print_u64, (cx_u64 v));
CX_FN(void, cx_print_f64, (cx_f64 v));
CX_FN(void, cx_print_f32, (cx_f32 v));
CX_FN(void, cx_print_bool, (cx_bool v));
CX_FN(void, cx_print_char, (cx_char v));
CX_FN(void, cx_print_rune, (cx_rune v));
CX_FN(void, cx_print_newline, (void));
/* Bytes to and from the standard streams, behind std.io. `cx_in_read` gives how many it read,
 * and 0 at the end. */
CX_FN(cx_usize, cx_in_read, (cx_u8* p, cx_usize n));
CX_FN(void, cx_out_write, (const cx_u8* p, cx_usize n));
CX_FN(void, cx_out_flush, (void));

/* `eprint` and `eprintln`: between these, the print functions write to standard error, which
 * isn't buffered. Output waiting for standard output goes first, so the two keep their order.
 * Afterwards they write where they were writing, which matters inside a `show` written by hand
 * (D102): one that calls `eprint` while its value goes into a string. */
typedef struct {
    void* fmt;
    int err;
} cx_out_to;
CX_FN(cx_out_to, cx_err_begin, (void));
CX_FN(void, cx_err_end, (cx_out_to was));

/* A panic: `cx_panic_begin` starts the message on standard error, the print functions add to
 * it, and `cx_panic_end` finishes it, writes the stack trace and exits with status 101. */
CX_FN(void, cx_panic_begin, (const char* at));
CX_COLD CX_FN(void, cx_panic_end, (void));
/* A `@track_caller` function's hidden parameter as a `str`: the place that called it, a zero byte,
 * and the name of the function that call is in, which std.core's `caller_from` takes apart for
 * `caller()` (CLX-175). Panics print only the place, up to the zero. */
CX_INLINE cx_str cx_caller_str(const char* at) {
    cx_usize n = __builtin_strlen(at);
    return (cx_str){(const cx_u8*)at, n + 1 + __builtin_strlen(at + n + 1)};
}
/* Writes Calx frames of the calling thread's stack through `text`, a line each, in calx_trace.c:
 * all of them (`mode` 0), or for a panic in a `parallel for`, the part of them this thread has
 * (1 to 3, which `cx_trace_write` explains). */
__attribute__((cold)) CX_FN(void, cx_trace_write, (void (*text)(const char*), int mode, int ending));
CX_COLD CX_FN(void, cx_panic_index, (cx_usize index, cx_usize length, const char* at));
CX_COLD CX_FN(void, cx_panic_div_zero, (const char* at));
CX_COLD CX_FN(void, cx_panic_unwrap, (const char* at));
CX_COLD CX_FN(void, cx_panic_unwrap_err, (const char* at));
CX_COLD CX_FN(void, cx_panic_insert, (cx_usize index, cx_usize length, const char* at));
CX_COLD CX_FN(void, cx_panic_set_len, (cx_usize length, cx_usize capacity, const char* at));

/* `parallel for`: calls `body(ctx, begin, end)` for chunks covering 0..n, on the worker threads
 * and this one, and returns once every iteration has run. Inside another `parallel for`, its
 * chunks go to the threads that are free, or all run on this one if none can take them. */
CX_FN(void, cx_parallel, (cx_u64 n, void (*body)(void* ctx, cx_u64 begin, cx_u64 end), void* ctx));
/* `parallel(pool) for` (CLX-71): as `cx_parallel`, on a pool std.pool's `Pool.new` made, whose
 * `par_pool*` is `pool`. One thread at a time runs a loop on it; another waits. */
CX_FN(void, cx_parallel_on, (cx_u64 pool, cx_u64 n, void (*body)(void* ctx, cx_u64 begin, cx_u64 end), void* ctx));
/* How many threads `parallel for` uses, and pools of the program's, behind std.pool. */
CX_FN(void, cx_set_workers, (cx_usize n));
CX_FN(cx_usize, cx_workers, (void));
CX_FN(cx_u64, cx_pool_new, (cx_usize threads));
CX_FN(void, cx_pool_close, (cx_u64 pool));

/* Memory, in calx_alloc.c (D95). A block is freed, or reallocated, with the size it was last
 * given, which the compiler always knows, so blocks have no header. Freeing a null pointer does
 * nothing. Running out panics. */
CX_FN(void*, cx_alloc, (cx_usize size));
CX_FN(void*, cx_realloc, (void* p, cx_usize old, cx_usize size));
CX_FN(void, cx_free, (void* p, cx_usize size));
/* As cx_realloc, but only the first `keep` bytes need to come with it, for a list's growing: what
 * it holds, not its room to spare. Called in the runtime only. */
void* cx_realloc_keep(void* p, cx_usize old, cx_usize size, cx_usize keep);
/* How many blocks are allocated, in debug builds; 0 in others. */
CX_FN(cx_usize, cx_alloc_live, (void));
/* Gives the system back the memory of freed blocks that it can, and returns how many bytes. */
CX_FN(cx_usize, cx_alloc_trim, (void));
/* Makes room for `extra` more elements of `size` bytes in a list's buffer of `*cap`, holding
 * `len`, growing it to at least twice its size. Returns the buffer, which may have moved. Only
 * the check for room is inline. The growing is a cold call, out of line so a loop that pushes
 * keeps its registers for the loop, and it's given a copy of the capacity, not the list's own: a
 * call given the address of a field of a list in a local kept the list in memory, where a store
 * to an element might change it, so its pointer, length and capacity were read back after each
 * one (D158). A length is never past its capacity, so `*cap - len` doesn't wrap. */
__attribute__((cold)) CX_FN(void*, cx_grow_to, (void* ptr, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size));
CX_COLD CX_FN(void, cx_panic_capacity, (void));
CX_INLINE void* cx_grow(void* ptr, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size) {
    if (__builtin_expect(extra <= *cap - len, 1)) return ptr;
    cx_usize c = *cap;
    ptr = cx_grow_to(ptr, &c, len, extra, size);
    *cap = c;
    return ptr;
}
/* Room for `extra` more in a deque's ring, which runs from `*head` round its buffer: grown as a
 * list's is, then with the run that wrapped moved so the ring is whole again (D110). The growing
 * is out of line and given copies, as `cx_grow`'s is (D158). */
__attribute__((cold)) CX_FN(void*, cx_deque_grow_to, (void* ptr, cx_usize* head, cx_usize* cap, cx_usize len, cx_usize extra,
                                                    cx_usize size));
CX_INLINE void* cx_deque_grow(void* ptr, cx_usize* head, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size) {
    if (__builtin_expect(extra <= *cap - len, 1)) return ptr;
    cx_usize h = *head, c = *cap;
    ptr = cx_deque_grow_to(ptr, &h, &c, len, extra, size);
    *head = h;
    *cap = c;
    return ptr;
}

/* Queues between threads, and the threads `Thread.spawn` starts, in calx_thread.c (D121). An end
 * of a queue is a pointer to it, whatever its messages are; the queue counts its ends and goes
 * with the last. A message moves in and out as `size` bytes, and `drop` frees what the ones left
 * in a queue own. */
typedef struct cx_queue cx_queue;
typedef struct cx_Sender {
    cx_queue* q;
} cx_Sender;
typedef struct cx_Receiver {
    cx_queue* q;
} cx_Receiver;
typedef struct cx_Thread {
    cx_u64 h; /* the thread's handle, or its pthread_t */
} cx_Thread;
CX_FN(cx_Receiver, cx_queue_new, (cx_usize size, void (*drop)(void*)));
CX_FN(cx_Sender, cx_queue_sender, (const cx_Receiver* r));
CX_FN(cx_Sender, cx_queue_share_sender, (const cx_Sender* s));
CX_FN(cx_Receiver, cx_queue_share_receiver, (const cx_Receiver* r));
/* Moves the message at `message` into the queue, or drops it if no receiver is left. */
CX_FN(void, cx_queue_send, (const cx_Sender* s, void* message, const char* at));
/* Moves the first message into `out`: if there is one (`next`), or once there is, unless every
 * sender has gone (`wait`), or `ns` goes by first (`wait_for`). Returns whether it did. */
CX_FN(cx_bool, cx_queue_next, (const cx_Receiver* r, void* out, const char* at));
CX_FN(cx_bool, cx_queue_wait, (const cx_Receiver* r, void* out, const char* at));
CX_FN(cx_bool, cx_queue_wait_for, (const cx_Receiver* r, void* out, cx_u64 ns, const char* at));
CX_FN(void, cx_drop_Sender, (cx_Sender* s));
CX_FN(void, cx_drop_Receiver, (cx_Receiver* r));
/* Runs `run(task)` on a new thread. `task` is the task, moved to memory from cx_alloc, which
 * `run` frees. */
CX_FN(cx_Thread, cx_thread_spawn, (void (*run)(void*), void* task));
CX_FN(void, cx_thread_join, (cx_Thread* t, const char* at));
CX_FN(void, cx_drop_Thread, (cx_Thread* t));
/* A sender's copy is another end of its queue, as `share()` makes (CLX-115). */
static inline __attribute__((nodebug)) cx_Sender cx_copy_Sender(const cx_Sender* s) { return cx_queue_share_sender(s); }
/* Never called, since a receiver and a thread can't be copied (D121), but a type's copy glue
 * names the copy of each of its fields. */
static inline __attribute__((nodebug)) cx_Receiver cx_copy_Receiver(const cx_Receiver* r) {
    (void)r;
    __builtin_trap();
}
static inline __attribute__((nodebug)) cx_Thread cx_copy_Thread(const cx_Thread* t) {
    (void)t;
    __builtin_trap();
}

/* Libraries (D123), in calx_lib.c. A library built with `calx build --lib` has one symbol the
 * system can find, `cx_library`, which says what it was built with, fills in its pointers to the
 * program's runtime functions, and lists the functions it exports. Each export has its type as
 * Calx writes it, for messages, and a hash of its shape: its type with the fields and variants of
 * every type in it, so a struct that changed between the two builds doesn't match. */
typedef struct {
    const char* name;
    void* fn;
    const char* type;
    cx_u64 shape;
} cx_lib_export;
typedef struct {
    cx_u64 abi;      /* the compiler and runtime it was built with */
    cx_usize fns;    /* how many runtime functions it takes */
    void (*attach)(void* const* fns);
    const cx_lib_export* (*exports)(cx_usize* count);
} cx_lib_info;
/* A library that's open. Dropping one leaves the library loaded, since functions from it can
 * still be anywhere; `cx_lib_close` unloads it. */
typedef struct cx_Library {
    void* p;
} cx_Library;
/* Opens the library at `path`, or puts why it can't in `error`. */
CX_FN(cx_bool, cx_lib_open, (cx_str path, cx_Library* out, cx_String* error));
/* The function the library exports as `name`, if its type is `type` with that shape, or null
 * with why not in `error`. */
CX_FN(void*, cx_lib_find, (const cx_Library* lib, cx_str name, const char* type, cx_u64 shape, cx_String* error));
CX_FN(cx_bool, cx_lib_has, (const cx_Library* lib, cx_str name));
CX_FN(cx_str, cx_lib_path, (const cx_Library* lib));
CX_FN(void, cx_lib_close, (cx_Library* lib));
CX_FN(void, cx_drop_Library, (cx_Library* lib));
static inline __attribute__((nodebug)) cx_Library cx_copy_Library(const cx_Library* l) {
    (void)l;
    __builtin_trap();
}

/* Arenas (D114). An arena hands out memory from the bottom of its newest chunk up, and frees it
 * all at once. The state is on the heap, so an `Arena` is one pointer, and the lists made from it
 * hold that pointer, never the `Arena` itself. The first chunk is made when it's first needed. */
typedef struct cx_arena_chunk cx_arena_chunk;
struct cx_arena_chunk {
    cx_arena_chunk* prev; /* the chunk before it, which is full */
    cx_usize size;        /* its size in bytes, this header included */
};
typedef struct cx_arena_state {
    cx_u8* top;            /* the first free byte in the newest chunk, or null before the first */
    cx_u8* end;            /* the end of the newest chunk */
    cx_arena_chunk* chunk; /* the newest chunk */
    cx_usize first;        /* the size of the first chunk */
    cx_usize full;         /* the bytes taken from the chunks before the newest */
} cx_arena_state;
typedef struct cx_Arena {
    cx_arena_state* s;
} cx_Arena;
/* An arena whose first chunk holds at least `first` bytes, 8 KiB if it's 0. */
CX_FN(cx_Arena, cx_arena_new, (cx_usize first));
CX_FN(void, cx_drop_Arena, (cx_Arena* a));
/* A new, empty arena with the same first chunk: nothing in an arena belongs to its value. */
CX_FN(cx_Arena, cx_copy_Arena, (const cx_Arena* a));
/* Frees everything taken from the arena. One that needed more than one chunk keeps a single
 * chunk as big as all of them, so the next round of the same work takes no new chunk. */
CX_FN(void, cx_arena_reset, (cx_Arena* a));
/* The bytes taken since the arena was made or reset, alignment included. */
CX_FN(cx_usize, cx_arena_used, (const cx_Arena* a));
/* Moves an arena list's buffer of `len` elements of `size` bytes to `bytes` from a new chunk. */
CX_FN(void*, cx_arena_more, (cx_arena_state* s, void* ptr, cx_usize len, cx_usize bytes, cx_usize size, cx_usize align));
/* Room for `extra` more elements of `size` bytes, aligned to `align`, in an arena list's buffer
 * of `*cap` holding `len`: grown in place if it's the newest block and its chunk has room, or
 * moved to new memory at least twice its size, and at least 64 bytes. Returns the buffer. All
 * but a new chunk is inline, so with `size` and `align` constants it's a few instructions. */
CX_INLINE void* cx_arena_grow(cx_arena_state* s, void* ptr, cx_usize* cap, cx_usize len, cx_usize extra, cx_usize size,
                              cx_usize align) {
    cx_usize unit = size ? size : 1;
    cx_usize max = ((cx_usize)-1 / 4) / unit;
    if (__builtin_expect(len > max || extra > max - len, 0)) cx_panic_capacity();
    cx_usize need = len + extra;
    if (need <= *cap) return ptr;
    cx_usize n = *cap <= max / 2 ? *cap * 2 : max;
    if (n < need) n = need;
    if (n < 64 / unit) n = 64 / unit;
    if (n < 4) n = 4;
    cx_u8* p = (cx_u8*)ptr;
    cx_usize old = *cap * size, bytes = n * size;
    *cap = n;
    /* The newest block grows where it is, while its chunk has room. */
    if (p && p + old == s->top && bytes - old <= (cx_usize)(s->end - s->top)) {
        s->top = p + bytes;
        return p;
    }
    if (s->top) {
        cx_usize pad = (cx_usize)(-(__UINTPTR_TYPE__)s->top & (align - 1));
        cx_usize room = (cx_usize)(s->end - s->top);
        if (pad <= room && bytes <= room - pad) {
            cx_u8* q = s->top + pad;
            s->top = q + bytes;
            if (len) __builtin_memcpy(q, p, len * size);
            return q;
        }
    }
    return cx_arena_more(s, p, len, bytes, size, align);
}
/* `sort` found that a hand-written `Order` contradicts itself (D102). */
CX_COLD CX_FN(void, cx_panic_order, (const char* at));

/* Strings. */
CX_INLINE cx_String cx_string_lit(const char* p, cx_usize len) {
    cx_String s = {(cx_u8*)p, len, 0};
    return s;
}
CX_INLINE cx_str cx_sv(cx_String s) {
    cx_str v = {s.ptr, s.len};
    return v;
}
/* -1, 0 or 1, as `a` comes before, is the same as, or comes after `b`, byte by byte. For UTF-8
 * that's the order of the code points. */
CX_INLINE int cx_str_cmp(cx_str a, cx_str b) {
    cx_usize n = a.len < b.len ? a.len : b.len;
    if (n >= 8) {
        // Most strings differ in their first 8 bytes, which compare as one big-endian integer.
        cx_u64 x, y;
        __builtin_memcpy(&x, a.ptr, 8);
        __builtin_memcpy(&y, b.ptr, 8);
        if (x != y) return __builtin_bswap64(x) < __builtin_bswap64(y) ? -1 : 1;
        int c = __builtin_memcmp(a.ptr + 8, b.ptr + 8, n - 8);
        if (c) return c < 0 ? -1 : 1;
    } else {
        for (cx_usize i = 0; i < n; i++) {
            if (a.ptr[i] != b.ptr[i]) return a.ptr[i] < b.ptr[i] ? -1 : 1;
        }
    }
    return (a.len > b.len) - (a.len < b.len);
}
/* `xs.sort()` on a list of integers, `char`s, `rune`s or `bool`s, from calx_sort.h (D96). */
CX_FN(void, cx_sort_i8, (cx_i8* v, cx_usize n));
CX_FN(void, cx_sort_i16, (cx_i16* v, cx_usize n));
CX_FN(void, cx_sort_i32, (cx_i32* v, cx_usize n));
CX_FN(void, cx_sort_i64, (cx_i64* v, cx_usize n));
CX_FN(void, cx_sort_u8, (cx_u8* v, cx_usize n));
CX_FN(void, cx_sort_u16, (cx_u16* v, cx_usize n));
CX_FN(void, cx_sort_u32, (cx_u32* v, cx_usize n));
CX_FN(void, cx_sort_u64, (cx_u64* v, cx_usize n));
CX_FN(void, cx_sort_usize, (cx_usize* v, cx_usize n));
CX_FN(void, cx_sort_char, (cx_char* v, cx_usize n));
CX_FN(void, cx_sort_rune, (cx_rune* v, cx_usize n));
CX_FN(void, cx_sort_bool, (cx_bool* v, cx_usize n));
/* The capacity of a `String` that views bytes it doesn't own, which aren't a literal's: a part a
 * loop over `split` or `lines` gives. It's never freed, and `copy` copies it, where a capacity of 0,
 * a literal's, is shared. Only the top bit is set, so a drop's test costs what it did. */
#define CX_VIEW_CAP ((cx_usize)1 << 63)
/* A `str` passed for a `String` parameter that's only looked at (CLX-141): a view of its bytes,
 * since a `str` can be a part of memory something else owns. */
CX_INLINE cx_String cx_str_view(cx_str s) {
    cx_String v = {(cx_u8*)s.ptr, s.len, CX_VIEW_CAP};
    return v;
}
CX_INLINE void cx_drop_String(cx_String* s) {
    if ((cx_i64)s->cap > 0) cx_free(s->ptr, s->cap);
}
CX_FN(cx_String, cx_copy_String, (const cx_String* s));
/* A `String` that owns a copy of the bytes. */
CX_FN(cx_String, cx_string_from, (cx_str s));
/* `s.interned()`: a `String` with a capacity of 0, as a literal's, sharing one copy of a short
 * text that's never freed with every other interned string of it, or a copy past the limits. */
CX_FN(cx_String, cx_string_interned, (cx_str s));
/* Makes room for `extra` more bytes in a string's buffer of `*cap`, holding `len`, as `cx_grow_to`
 * does a list's: twice the size, or 16 bytes to start. A literal's string has a capacity of 0 and
 * doesn't own its bytes, so it comes here for any push, and they're copied into a buffer of its
 * own. Only the check for room is inline, and the call is given copies, so a string in a local
 * stays in registers (D160). */
__attribute__((cold)) CX_FN(cx_u8*, cx_string_grow_to, (cx_u8* ptr, cx_usize* cap, cx_usize len, cx_usize extra));
/* Adds a rune when there's no room for 4 bytes, given copies, as `cx_string_grow_to` is. */
CX_FN(cx_u8*, cx_string_push_rune_to, (cx_u8* ptr, cx_usize* cap, cx_usize* len, cx_rune r));
CX_INLINE void cx_string_push_bytes(cx_String* s, const cx_u8* p, cx_usize n) {
    if (n == 0) return;
    cx_u8* ptr = s->ptr;
    cx_usize len = s->len, cap = s->cap;
    /* Neither length is past half the address space, so the sum doesn't wrap. */
    if (__builtin_expect(len + n > cap, 0)) {
        ptr = cx_string_grow_to(ptr, &cap, len, n);
        s->ptr = ptr;
        s->cap = cap;
    }
    __builtin_memcpy(ptr + len, p, n);
    s->len = len + n;
}
CX_INLINE void cx_string_push_rune(cx_String* s, cx_rune r) {
    cx_u8* ptr = s->ptr;
    cx_usize len = s->len, cap = s->cap;
    if (__builtin_expect(len + 4 <= cap, 1)) {
        if (r < 0x80) {
            ptr[len] = (cx_u8)r;
            s->len = len + 1;
            return;
        }
        /* 2 to 4 bytes, with no branch on how many, since text that isn't ASCII mixes them and
         * a branch would guess wrong. All 4 are written, and the length takes the ones that count:
         * `n - 1` continuation bytes of 6 bits each after a lead byte that holds the rest. */
        cx_u32 n = 2 + (r >= 0x800) + (r >= 0x10000), shift = 6 * (n - 1);
        ptr[len] = (cx_u8)(((0xF0E0C000u >> (8 * (n - 1))) & 0xFF) | (r >> shift));
        ptr[len + 1] = (cx_u8)(0x80 | ((r >> (shift - 6)) & 0x3F));
        ptr[len + 2] = (cx_u8)(0x80 | ((r >> ((shift - 12) & 31)) & 0x3F));
        ptr[len + 3] = (cx_u8)(0x80 | ((r >> ((shift - 18) & 31)) & 0x3F));
        s->len = len + n;
        return;
    }
    s->ptr = cx_string_push_rune_to(ptr, &cap, &len, r);
    s->cap = cap;
    s->len = len;
}
CX_FN(void, cx_string_reserve, (cx_String* s, cx_usize extra));
CX_INLINE void cx_string_push_str(cx_String* s, cx_str v) { cx_string_push_bytes(s, v.ptr, v.len); }
CX_INLINE void cx_string_clear(cx_String* s) {
    if (s->cap) {
        s->len = 0;
    } else {
        s->ptr = 0;
        s->len = 0;
    }
}
/* `s.truncate(n)`: the first `n` bytes, where `n` is where a rune starts. A literal's string keeps
 * pointing at the literal, whose bytes its first push copies. */
CX_COLD CX_FN(void, cx_panic_truncate, (cx_usize n, cx_usize length, const char* at));
CX_INLINE void cx_string_truncate(cx_String* s, cx_usize n, const char* at) {
    if (n >= s->len) return;
    if (__builtin_expect((s->ptr[n] & 0xC0) == 0x80, 0)) cx_panic_truncate(n, s->len, at);
    s->len = n;
}
CX_FN(cx_bool, cx_str_contains, (cx_str hay, cx_str needle));
/* Whether the bytes are UTF-8: no overlong forms, surrogates, or values past U+10FFFF. */
CX_FN(cx_bool, cx_utf8_valid, (const cx_u8* p, cx_usize n));
CX_FN(cx_usize, cx_bytes_below, (const cx_u8* p, cx_usize n, cx_u8 limit));
/* Java's String.hashCode and length(), over the UTF-16 units the text stands for. */
CX_FN(cx_i32, cx_str_java_hash, (cx_str s));
CX_FN(cx_usize, cx_str_utf16_len, (cx_str s));
/* The rune at `*i` in UTF-8 that's known to be valid, moving `*i` past it. */
CX_INLINE cx_rune cx_utf8_next(const cx_u8* p, cx_usize* i) {
    cx_u32 b = p[*i];
    if (b < 0x80) {
        *i += 1;
        return b;
    }
    if (b < 0xE0) {
        cx_rune r = ((b & 0x1F) << 6) | (p[*i + 1] & 0x3F);
        *i += 2;
        return r;
    }
    if (b < 0xF0) {
        cx_rune r = ((b & 0x0F) << 12) | ((cx_u32)(p[*i + 1] & 0x3F) << 6) | (p[*i + 2] & 0x3F);
        *i += 3;
        return r;
    }
    cx_rune r = ((b & 0x07) << 18) | ((cx_u32)(p[*i + 1] & 0x3F) << 12) | ((cx_u32)(p[*i + 2] & 0x3F) << 6) | (p[*i + 3] & 0x3F);
    *i += 4;
    return r;
}
/* The UTF-16 unit at `*i`, as Java sees the text. A rune past U+FFFF gives its high surrogate,
 * and leaves the low one in `*low` for the next call. */
CX_INLINE cx_char cx_utf16_next(const cx_u8* p, cx_usize* i, cx_char* low) {
    if (*low) {
        cx_char c = *low;
        *low = 0;
        return c;
    }
    cx_rune r = cx_utf8_next(p, i);
    if (r < 0x10000) return (cx_char)r;
    r -= 0x10000;
    *low = (cx_char)(0xDC00 | (r & 0x3FF));
    return (cx_char)(0xD800 | (r >> 10));
}

/* `T.parse(text)`. Integers follow Java's Integer.parseInt: an optional sign (no `-` for unsigned
 * types), then ASCII digits, in range. Floats follow Double.parseDouble's grammar, and round
 * correctly. On failure they set `*out` to 0 and return 0. */
CX_FN(cx_bool, cx_parse_i64, (cx_str s, cx_i64 lo, cx_i64 hi, cx_i64* out));
CX_FN(cx_bool, cx_parse_u64, (cx_str s, cx_u64 hi, cx_u64* out));
CX_FN(cx_bool, cx_parse_i64_radix, (cx_str s, cx_u32 radix, cx_i64 lo, cx_i64 hi, cx_i64* out));
CX_FN(cx_bool, cx_parse_u64_radix, (cx_str s, cx_u32 radix, cx_u64 hi, cx_u64* out));
CX_COLD CX_FN(void, cx_panic_radix, (cx_u32 radix, const char* at));

/* Floats come from the runtime's own code, the same bits on every platform (calx_fparse.c,
 * CLX-137). A decimal written plainly, with a sign, a '.' and an exponent perhaps, and 19
 * significant digits or fewer, which is most of them, is read here, inline, in one pass, eight
 * digits at a time; with a significand of up to 2^53 (2^24 for f32) and a power of ten up to
 * 10^22 (10^10), both exact, it's one rounding. The rest go to the runtime: the bits of
 * w * 10^q from a product with a table, and everything else, as `cx_parse_float_rest`. */
CX_FN(cx_u64, cx_parse_float_wq, (cx_u64 w, cx_i64 q, const cx_u8* sig, cx_usize n, cx_i64 exp10, int f32));
CX_FN(cx_bool, cx_parse_float_rest, (cx_str s, int f32, cx_u64* bits));
static const double cx_exact_pow10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                          1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
static const float cx_exact_pow10f[11] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f};
CX_INLINE cx_u64 cx_read8(const cx_u8* p) {
    cx_u64 v;
    __builtin_memcpy(&v, p, 8);  /* little-endian, as every platform Calx runs on is */
    return v;
}
/* Whether all 8 bytes are digits: none above '9' once 0x46 is added, and none below '0'. */
CX_INLINE int cx_eight_digits(cx_u64 v) {
    return (((v + 0x4646464646464646ull) | (v - 0x3030303030303030ull)) & 0x8080808080808080ull) == 0;
}
/* The number 8 digit bytes write, the first most significant: pairs, then fours, then all eight,
 * each step one multiply. */
CX_INLINE cx_u64 cx_eight_value(cx_u64 v) {
    v -= 0x3030303030303030ull;
    v = v * 10 + (v >> 8);
    v = (((v & 0x000000FF000000FFull) * 0x000F424000000064ull) + (((v >> 16) & 0x000000FF000000FFull) * 0x0000271000000001ull)) >> 32;
    return (cx_u32)v;
}
CX_INLINE cx_bool cx_parse_float(cx_str s, int f32, cx_u64* bits) {
    const cx_u8 *p = s.ptr, *end = s.ptr + s.len;
    cx_u64 sign = 0;
    if (p < end && (*p == '-' || *p == '+')) sign = *p++ == '-';
    const cx_u8* start = p;
    cx_u64 w = 0;
    while (end - p >= 8 && cx_eight_digits(cx_read8(p))) {
        w = w * 100000000 + cx_eight_value(cx_read8(p));
        p += 8;
    }
    while (p < end && (cx_u8)(*p - '0') < 10) w = w * 10 + (cx_u64)(*p++ - '0');
    cx_i64 digits = p - start, q = 0;
    if (p < end && *p == '.') {
        const cx_u8* frac = ++p;
        while (end - p >= 8 && cx_eight_digits(cx_read8(p))) {
            w = w * 100000000 + cx_eight_value(cx_read8(p));
            p += 8;
        }
        while (p < end && (cx_u8)(*p - '0') < 10) w = w * 10 + (cx_u64)(*p++ - '0');
        q = frac - p;
        digits -= q;
    }
    const cx_u8* sig_end = p;
    cx_i64 exp10 = 0;
    if (p < end && (*p | 0x20) == 'e') {
        p++;
        int neg = 0;
        if (p < end && (*p == '-' || *p == '+')) neg = *p++ == '-';
        if (p == end || (cx_u8)(*p - '0') >= 10) return cx_parse_float_rest(s, f32, bits);
        while (p < end && (cx_u8)(*p - '0') < 10) {
            if (exp10 < 100000000) exp10 = exp10 * 10 + (*p - '0');
            p++;
        }
        if (neg) exp10 = -exp10;
    }
    if (p != end || digits == 0) return cx_parse_float_rest(s, f32, bits);
    if (digits > 19) {
        /* Leading zeros don't count, and w is right if what's left is 19 digits or fewer. */
        const cx_u8* z = start;
        while (z < sig_end && (*z == '0' || *z == '.')) digits -= *z++ == '0';
        if (digits > 19) return cx_parse_float_rest(s, f32, bits);
    }
    q += exp10;
    cx_u64 r;
    if (w == 0 || q + 19 <= (f32 ? -46 : -324)) {
        r = 0;  /* nearer 0 than the least subnormal */
    } else if (q >= (f32 ? 39 : 309)) {
        r = f32 ? 0x7f800000u : 0x7ff0000000000000ull;  /* past the greatest float */
    } else if (!f32 && w <= ((cx_u64)1 << 53) && q >= -22 && q <= 22) {
        double v = (double)w;
        if (q < 0) v /= cx_exact_pow10[-q];
        else if (q > 0) v *= cx_exact_pow10[q];
        __builtin_memcpy(&r, &v, 8);
    } else if (f32 && w <= ((cx_u64)1 << 24) && q >= -10 && q <= 10) {
        float v = (float)w;
        if (q < 0) v /= cx_exact_pow10f[-q];
        else if (q > 0) v *= cx_exact_pow10f[q];
        cx_u32 b;
        __builtin_memcpy(&b, &v, 4);
        r = b;
    } else {
        r = cx_parse_float_wq(w, q, start, (cx_usize)(sig_end - start), exp10, f32);
    }
    *bits = r | sign << (f32 ? 31 : 63);
    return 1;
}
CX_INLINE cx_bool cx_parse_f64(cx_str s, cx_f64* out) {
    cx_u64 bits = 0;
    cx_bool ok = cx_parse_float(s, 0, &bits);
    __builtin_memcpy(out, &bits, 8);
    return ok;
}
CX_INLINE cx_bool cx_parse_f32(cx_str s, cx_f32* out) {
    cx_u64 bits = 0;
    cx_bool ok = cx_parse_float(s, 1, &bits);
    cx_u32 b = (cx_u32)bits;
    __builtin_memcpy(out, &b, 4);
    return ok;
}
CX_INLINE cx_bool cx_str_starts_with(cx_str s, cx_str p) {
    return p.len <= s.len && (p.len == 0 || __builtin_memcmp(s.ptr, p.ptr, p.len) == 0);
}
CX_INLINE cx_bool cx_str_ends_with(cx_str s, cx_str p) {
    return p.len <= s.len && (p.len == 0 || __builtin_memcmp(s.ptr + s.len - p.len, p.ptr, p.len) == 0);
}

/* Interpolation into a `String`: between `cx_fmt_begin` and `cx_fmt_end`, the print functions
 * add to the string instead of writing output. They nest. */
CX_FN(cx_String*, cx_fmt_begin, (cx_String* s));
CX_FN(void, cx_fmt_end, (cx_String* previous));

/* Whether two strings hold the same bytes, for string patterns. */
CX_INLINE cx_bool cx_str_eq(cx_str a, cx_str b) {
    return a.len == b.len && (a.len == 0 || __builtin_memcmp(a.ptr, b.ptr, a.len) == 0);
}

/* `s[lo..hi]`, or `s[lo..=hi]` with `inclusive`: the bytes from `lo` up to `hi`, which must be in
 * order, in the text, and where runes start (D117). */
CX_COLD CX_FN(void, cx_panic_slice, (cx_str s, cx_usize lo, cx_usize hi, cx_bool inclusive, const char* at));
CX_INLINE cx_bool cx_utf8_starts_at(cx_str s, cx_usize k) { return k == s.len || (s.ptr[k] & 0xC0) != 0x80; }
CX_INLINE cx_str cx_str_slice(cx_str s, cx_usize lo, cx_usize hi, cx_bool inclusive, const char* at) {
    cx_usize end = hi + (inclusive ? 1 : 0);
    if (__builtin_expect(lo > end || end > s.len || end < hi || !cx_utf8_starts_at(s, lo) || !cx_utf8_starts_at(s, end), 0)) {
        cx_panic_slice(s, lo, hi, inclusive, at);
    }
    return (cx_str){s.ptr + lo, end - lo};
}
/* `xs[lo..hi]` of a list or an array of `length` elements, or `xs[lo..=hi]` with `inclusive`:
 * where the elements copied end, after checking that the range is in order and inside. */
CX_COLD CX_FN(void, cx_panic_elements, (cx_usize lo, cx_usize hi, cx_bool inclusive, cx_usize length, const char* at));
CX_INLINE cx_usize cx_slice_end(cx_usize lo, cx_usize hi, cx_bool inclusive, cx_usize length, const char* at) {
    cx_usize end = hi + (inclusive ? 1 : 0);
    if (__builtin_expect(lo > end || end > length || end < hi, 0)) cx_panic_elements(lo, hi, inclusive, length, at);
    return end;
}
/* Where `needle` first or last starts in `hay`, a byte offset, or CX_NOT_FOUND. Both are UTF-8,
 * so a match starts where a rune does. `""` is found at 0, or last at the end. */
CX_FN(cx_usize, cx_str_find, (cx_str hay, cx_str needle));
CX_FN(cx_usize, cx_str_find_last, (cx_str hay, cx_str needle));
/* Where the `n` bytes at `needle` first are in the `len` at `hay`, or CX_NOT_FOUND. */
CX_FN(cx_usize, cx_bytes_find, (const cx_u8* hay, cx_usize len, const cx_u8* needle, cx_usize n));
/* The same, for a needle looked for again and again: which of its bytes is likely rarest is worked
 * out once by `cx_bytes_rare`, and passed as `k`. */
CX_FN(cx_usize, cx_bytes_rare, (const cx_u8* needle, cx_usize n));
CX_FN(cx_usize, cx_bytes_find_rare, (const cx_u8* hay, cx_usize len, const cx_u8* needle, cx_usize n, cx_usize k));
/* For std.regex: the first byte from `from` that's in `set`, or `len`. */
CX_FN(cx_usize, cx_rx_skip, (const cx_u8* hay, cx_usize len, cx_usize from, const cx_u8* set, cx_u32 count, cx_u32 bytes));
/* For std.csv: the first byte from `from` that's `a`, `b` or `c`, or `len`. */
CX_FN(cx_usize, cx_bytes_find3, (const cx_u8* hay, cx_usize len, cx_usize from, cx_u8 a, cx_u8 b, cx_u8 c));
/* For std.html: the same for five bytes. */
CX_FN(cx_usize, cx_bytes_find5, (const cx_u8* hay, cx_usize len, cx_usize from, cx_u8 a, cx_u8 b, cx_u8 c, cx_u8 d, cx_u8 e));
/* For std.textwrap: the first byte from `from` that could start a line break `splitlines` sees. */
CX_FN(cx_usize, cx_bytes_find_break, (const cx_u8* hay, cx_usize len, cx_usize from));
/* For std.unicode's quick check: the first rune from `from` that isn't below `low` or, as `kinds`
 * says, a CJK ideograph (1), a Hangul syllable (2) or in Latin Extended Additional (4), or `len`. */
CX_FN(cx_usize, cx_bytes_skip_starters, (const cx_u8* hay, cx_usize len, cx_usize from, cx_u8 low, cx_u8 kinds));
/* Java's `toLowerCase(Locale.ROOT)` and `toUpperCase(Locale.ROOT)`, in calx_case.c (D117). Text
 * that doesn't change comes back without a copy when `keep` says its bytes are a literal's. */
CX_FN(cx_String, cx_case_lower, (cx_str s, cx_bool keep));
CX_FN(cx_String, cx_case_upper, (cx_str s, cx_bool keep));

/* A list of strings with a `List<String>`'s layout, as the text functions below build one. */
typedef struct {
    cx_String* ptr;
    cx_usize len;
    cx_usize cap;
} cx_StringList;
/* Text methods, in calx_text.c: `trim` (`ends` 1 for the start, 2 the end, 3 both), `replace`,
 * `repeat`, `pad_start` and `pad_end`, `split`, `split_whitespace`, `lines`, and a list's `join`.
 * `trim` and the two below give a part of the text, which the emitter copies only where it's
 * kept, as it does a slice. */
CX_FN(cx_str, cx_text_trim, (cx_str s, cx_u32 ends));
CX_FN(cx_String, cx_text_replace, (cx_str s, cx_str from, cx_str to, const char* at));
CX_FN(cx_String, cx_text_repeat, (cx_str s, cx_usize n, const char* at));
CX_FN(cx_String, cx_text_pad, (cx_str s, cx_usize width, cx_rune fill, cx_bool start, const char* at));
CX_FN(cx_StringList, cx_text_split, (cx_str s, cx_str sep));
CX_FN(cx_StringList, cx_text_split_whitespace, (cx_str s));
CX_FN(cx_StringList, cx_text_lines, (cx_str s));
CX_FN(cx_String, cx_text_join, (const cx_String* parts, cx_usize n, cx_str sep, const char* at));
CX_FN(cx_String, cx_text_join_str, (const cx_str* parts, cx_usize n, cx_str sep, const char* at));
CX_FN(cx_bool, cx_text_words_next, (cx_str s, cx_usize* at, cx_str* part));
/* `{x:spec}`: how a value is written, as the compiler read the format (CLX-93). `align` is '<',
 * '>', '^', or 0 for the type's own; `kind` is 'x', 'X', 'o', 'b', 'e', 'E', '%', or 0. Integers,
 * text and runes are in calx_format.c, floats in calx_float.c. Signed integers in hex, octal or
 * binary come as their bits, unsigned, so they're written as Java's `toHexString` writes them. */
typedef struct {
    cx_u32 fill;
    cx_u32 width;
    cx_i32 precision; /* -1 for none */
    cx_u8 align;
    cx_u8 flags;
    cx_u8 kind;
} cx_spec;
#define CX_SPEC_PLUS 1
#define CX_SPEC_ALT 2
#define CX_SPEC_ZERO 4
#define CX_SPEC_COMMA 8
#define CX_SPEC_UNDER 16
CX_FN(void, cx_format_i64, (cx_i64 v, cx_spec s));
CX_FN(void, cx_format_u64, (cx_u64 v, cx_spec s));
CX_FN(void, cx_format_str, (cx_str v, cx_spec s));
CX_FN(void, cx_format_rune, (cx_rune v, cx_spec s));
CX_FN(void, cx_format_f64, (cx_f64 v, cx_spec s));
CX_FN(void, cx_format_f32, (cx_f32 v, cx_spec s));
/* The decimal digits of v, written backwards from `end`, two at a time; their count. */
CX_INLINE int cx_u64_text(cx_u64 v, char* end) {
    static const char pairs[201] = "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
                                   "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
                                   "8081828384858687888990919293949596979899";
    char* p = end;
    while (v >= 100) {
        cx_u64 q = v / 100;
        p -= 2;
        __builtin_memcpy(p, pairs + 2 * (v - q * 100), 2);
        v = q;
    }
    if (v >= 10) {
        p -= 2;
        __builtin_memcpy(p, pairs + 2 * v, 2);
    } else {
        *--p = (char)('0' + v);
    }
    return (int)(end - p);
}
/* A number, from its parts: `digits` the integer digits, which grouping and `0` apply to, and
 * `rest` what follows them. For calx_float.c. */
void cx_format_number(cx_str sign, cx_str prefix, cx_str digits, cx_str rest, cx_spec s, int group);
CX_INLINE cx_str cx_text_trim_prefix(cx_str s, cx_str p) {
    return cx_str_starts_with(s, p) ? (cx_str){s.ptr + p.len, s.len - p.len} : s;
}
CX_INLINE cx_str cx_text_trim_suffix(cx_str s, cx_str p) {
    return cx_str_ends_with(s, p) ? (cx_str){s.ptr, s.len - p.len} : s;
}

/* MD5, SHA-1, SHA-256, SHA-512 and SHA-384 (kind 0 to 4) and HMAC-SHA256, in calx_digest.c (D118).
 * A context is CX_DIGEST_CTX bytes, with no alignment needed. `cx_digest_finish` writes the
 * digest, 16 to 64 bytes, and returns its length. */
#define CX_DIGEST_CTX 224
CX_FN(void, cx_digest_start, (cx_u8* ctx, cx_u32 kind));
CX_FN(void, cx_digest_add, (cx_u8* ctx, const cx_u8* p, cx_usize n));
CX_FN(cx_usize, cx_digest_finish, (cx_u8* ctx, cx_u8* out));
CX_FN(void, cx_digest_hmac_sha256, (const cx_u8* key, cx_usize key_len, const cx_u8* msg, cx_usize msg_len, cx_u8* out));

/* Random bytes from the system, AES-128-CFB8 and RSA, in calx_crypto.c (D118). The contexts are
 * CX_CFB8_CTX, CX_RSA_CTX and CX_RSA_PUBLIC_CTX bytes, with no alignment needed. */
#define CX_CFB8_CTX 208
#define CX_RSA_CTX 912
#define CX_RSA_PUBLIC_CTX 1568
CX_FN(void, cx_crypto_random, (cx_u8* p, cx_usize n));
CX_FN(void, cx_cfb8_start, (cx_u8* ctx, const cx_u8* key, const cx_u8* iv));
CX_FN(void, cx_cfb8_encrypt, (cx_u8* ctx, cx_u8* p, cx_usize n));
CX_FN(void, cx_cfb8_decrypt, (cx_u8* ctx, cx_u8* p, cx_usize n));
CX_FN(cx_bool, cx_rsa_generate, (cx_u8* ctx, cx_u32 bits));
CX_FN(cx_usize, cx_rsa_primes, (const cx_u8* ctx, cx_u8* out));
CX_FN(cx_bool, cx_rsa_from_primes, (cx_u8* ctx, const cx_u8* in, cx_usize len));
CX_FN(cx_usize, cx_rsa_public_der, (const cx_u8* ctx, cx_u8* out));
CX_FN(cx_bool, cx_rsa_encrypt, (const cx_u8* ctx, const cx_u8* msg, cx_usize len, cx_u8* out));
CX_FN(cx_bool, cx_rsa_private_op, (const cx_u8* ctx, const cx_u8* in, cx_usize len, cx_u8* out));
CX_FN(cx_bool, cx_rsa_public_setup, (cx_u8* ctx, const cx_u8* mod, cx_usize len, const cx_u8* e, cx_usize elen));
CX_FN(cx_bool, cx_rsa_public_op, (const cx_u8* ctx, const cx_u8* in, cx_u8* out));
CX_FN(cx_i64, cx_rsa_decrypt, (const cx_u8* ctx, const cx_u8* in, cx_usize len, cx_u8* out));
CX_FN(cx_usize, cx_rsa_size, (const cx_u8* ctx));

/* zlib, gzip and raw deflate (format 0, 1 and 2), in calx_zlib.c (D118). An inflate context is
 * CX_INFLATE_CTX bytes, aligned as cx_alloc aligns. `cx_inflate` returns 0 at the stream's end, 1
 * when it needs more room, 2 when it stopped at the end of the last room, and -1, -2, -3 or -4
 * for a stream that's corrupt, cut off, fails its checksum, or needs a preset dictionary. */
#define CX_INFLATE_CTX 31232
CX_FN(cx_u32, cx_zlib_crc32, (cx_u32 crc, const cx_u8* p, cx_usize n));
CX_FN(cx_u32, cx_zlib_adler32, (cx_u32 adler, const cx_u8* p, cx_usize n));
CX_FN(void, cx_inflate_start, (cx_u8* ctx, cx_u32 format, cx_usize start));
CX_FN(cx_i32, cx_inflate, (cx_u8* ctx, const cx_u8* in, cx_usize n, cx_u8* out, cx_usize end, cx_bool final));
CX_FN(cx_usize, cx_inflate_len, (const cx_u8* ctx));
CX_FN(cx_usize, cx_inflate_used, (const cx_u8* ctx));
CX_FN(cx_usize, cx_deflate_bound, (cx_usize n, cx_u32 format));
CX_FN(cx_usize, cx_deflate, (const cx_u8* in, cx_usize n, cx_u32 format, cx_u32 level, cx_u8* out));

/* TCP sockets driven by events, in calx_net.c, behind std.net (D92). A `Net` is a handle, and
 * so is each connection in it. */
CX_FN(cx_u64, cx_net_open, (void));
CX_FN(cx_i32, cx_net_listen, (cx_u64 h, const cx_u8* host, cx_usize len, cx_u16 port));
CX_FN(cx_u32, cx_net_connect, (cx_u64 h, const cx_u8* host, cx_usize len, cx_u16 port));
CX_FN(cx_usize, cx_net_poll, (cx_u64 h, cx_i64 timeout_ms));
CX_FN(cx_u32, cx_net_event_kind, (cx_u64 h, cx_usize i));
CX_FN(cx_u32, cx_net_event_conn, (cx_u64 h, cx_usize i));
CX_FN(cx_usize, cx_net_available, (cx_u64 h, cx_u32 id));
CX_FN(cx_usize, cx_net_read, (cx_u64 h, cx_u32 id, cx_u8* to, cx_usize max));
CX_FN(void, cx_net_send, (cx_u64 h, cx_u32 id, const cx_u8* p, cx_usize len));
CX_FN(cx_usize, cx_net_queued, (cx_u64 h, cx_u32 id));
CX_FN(cx_bool, cx_net_send_file, (cx_u64 h, cx_u32 id, cx_i64 file, cx_u64 at, cx_u64 len));
CX_FN(void, cx_net_close, (cx_u64 h, cx_u32 id));
CX_FN(void, cx_net_flush, (cx_u64 h));
CX_FN(cx_bool, cx_net_waker, (cx_u64 h));
CX_FN(void, cx_net_wake, (cx_u64 h));
CX_FN(cx_u32, cx_net_peer, (cx_u64 h, cx_u32 id, cx_u8* out));
CX_FN(cx_u32, cx_net_error_kind, (void));
CX_FN(cx_i32, cx_net_error_code, (void));

/* HTTP and HTTPS requests, each on a thread of its own, in calx_http.c (D118). A request is a
 * handle that's never 0. `cx_http_done` and `cx_http_wait` give -1 for one that was freed, and
 * `cx_http_wait` gives what went wrong as std.http's Error numbers it, or 0. The getters are for a
 * request that was waited for. */
CX_FN(cx_u64, cx_http_start, (cx_bool post, const cx_u8* url, cx_usize url_len, const cx_u8* head, cx_usize head_len, const cx_u8* body,
                              cx_usize body_len, cx_u32 timeout_ms, cx_usize limit));
CX_FN(cx_i32, cx_http_done, (cx_u64 h));
CX_FN(cx_i32, cx_http_wait, (cx_u64 h));
CX_FN(cx_i32, cx_http_code, (cx_u64 h));
CX_FN(cx_u32, cx_http_status, (cx_u64 h));
CX_FN(cx_usize, cx_http_head_len, (cx_u64 h));
CX_FN(void, cx_http_head, (cx_u64 h, cx_u8* out));
CX_FN(cx_usize, cx_http_body_len, (cx_u64 h));
CX_FN(void, cx_http_body, (cx_u64 h, cx_u8* out));
CX_FN(void, cx_http_free, (cx_u64 h));
/* How much of the body has come, and its Content-Length plus 1 or 0, while the request runs, for a
 * progress bar. */
CX_FN(cx_u64, cx_http_received, (cx_u64 h));
CX_FN(cx_u64, cx_http_expected, (cx_u64 h));

/* LZ4 as lz4-java's block streams write it, in calx_lz4.c (D118). `cx_lz4_decode` carries on from
 * `state`, four numbers: where the next header is, the output's length, what the rest decompresses
 * to, and the length lz4-java's buffer would have, 21 to start. It
 * gives 0 at the stream's end, 1 when it needs more room, 2 when it stopped at the end of the last
 * room, and -1, -2 or -3 for a stream that's corrupt, cut off, or fails a checksum. */
CX_FN(cx_u32, cx_lz4_xxh32, (const cx_u8* p, cx_usize n, cx_u32 seed));
CX_FN(cx_usize, cx_lz4_bound, (cx_usize n));
CX_FN(cx_usize, cx_lz4_compress, (const cx_u8* in, cx_usize n, cx_u8* out));
CX_FN(cx_i32, cx_lz4_decode, (const cx_u8* in, cx_usize n, cx_usize* state, cx_u8* out, cx_usize end, cx_bool last));

/* The pixel work of std.image.jpeg's encoder, in calx_jpeg.c (CLX-148). `cx_jpg_divisors` turns 64
 * quantizers (1 to 255, row order) into the 192 values the DCT divides by. `cx_jpg_fdct2` puts
 * the blocks of 8 by 8 samples at a and b, rows `stride` bytes apart, through the DCT and divides
 * them by q, into oa and ob in row order, and gives each one's nonzero AC coefficients as bits in
 * zigzag order in masks; `cx_jpg_fdct1` does one block and gives its mask. `cx_jpg_ycc` turns n
 * RGB pixels into Y, Cb and Cr. `cx_jpg_down22` averages 2n samples of two rows into n, and
 * `cx_jpg_down21` 2n of one. `cx_jpg_huff` Huffman-codes `mcus` MCUs of blocks, each `luma` luma
 * blocks and then a block of each other component, onto out from len, which has 512 bytes a
 * block to spare, and gives the new length. Its tables are the luma and chroma ones, 1024 numbers
 * each: the DC codes, their lengths, the AC codes and theirs. st carries the bits not yet
 * written, their count and the three DC predictions from call to call; `cx_jpg_huff_end` pads
 * and writes the last of them. For a progressive JPEG (CLX-213), `cx_jpg_pack` puts n blocks'
 * DCs into dc and their nonzero AC coefficients, by their masks, into vals from offs[i], and gives
 * how many; `cx_jpg_scan` codes one scan of the blocks order[0..n) so packed, comp[i] each one's
 * component, spectral band ss to se and successive approximation ah, al. With gather it counts
 * the codes into counts, 256 a table, and otherwise writes the scan from out + len with tabs'
 * codes, padded and stuffed. */
CX_FN(void, cx_jpg_divisors, (const cx_u16* table, cx_u16* q));
CX_FN(void, cx_jpg_fdct2, (const cx_u8* a, const cx_u8* b, cx_usize stride, const cx_u16* q, cx_i16* oa, cx_i16* ob, cx_u64* masks));
CX_FN(cx_u64, cx_jpg_fdct1, (const cx_u8* a, cx_usize stride, const cx_u16* q, cx_i16* oa));
CX_FN(void, cx_jpg_ycc, (const cx_u8* rgb, cx_usize n, cx_u8* y, cx_u8* cb, cx_u8* cr));
CX_FN(void, cx_jpg_down22, (const cx_u8* r0, const cx_u8* r1, cx_usize n, cx_u8* out));
CX_FN(void, cx_jpg_down21, (const cx_u8* r0, cx_usize n, cx_u8* out));
CX_FN(cx_usize, cx_jpg_huff, (cx_u8* out, cx_usize len, cx_u64* st, const cx_u32* tabs, const cx_i16* coefs, const cx_u64* masks, cx_usize mcus, cx_usize luma, cx_usize ncomp));
CX_FN(cx_usize, cx_jpg_huff_end, (cx_u8* out, cx_usize len, cx_u64* st));
CX_FN(cx_usize, cx_jpg_pack, (const cx_i16* coefs, const cx_u64* masks, cx_usize n, cx_i16* dc, cx_i16* vals, cx_u32* offs));
CX_FN(cx_usize, cx_jpg_scan, (cx_u8* out, cx_usize len, cx_u32 gather, cx_u64* counts, const cx_u32* tabs, const cx_i16* dc, const cx_i16* vals,
                              const cx_u32* offs, const cx_u64* masks, const cx_u32* order, const cx_u8* comp, cx_usize n, cx_u32 ss, cx_u32 se,
                              cx_u32 ah, cx_u32 al));

/* std.log's shared settings, in calx_log.c (CLX-172): the root logger's level, format, date format
 * and log file, which the whole program shares, as Python's logging module's root logger. */
CX_FN(cx_i32, cx_log_config, (cx_i32 level, const cx_u8* format, cx_usize format_len, const cx_u8* datefmt, cx_usize datefmt_len,
                              const cx_u8* path, cx_usize path_len, cx_bool append));
CX_FN(cx_bool, cx_log_ready, (void));
CX_FN(cx_i32, cx_log_level, (void));
CX_FN(void, cx_log_set_level, (cx_i32 level));
CX_FN(cx_usize, cx_log_text, (cx_u32 which, cx_u8* out, cx_usize cap));
CX_FN(cx_bool, cx_log_to_file, (void));
CX_FN(void, cx_log_write, (const cx_u8* p, cx_usize n));
CX_FN(cx_i64, cx_log_start_ms, (cx_i64 now));
CX_FN(cx_u32, cx_log_pid, (void));

/* LZMA and LZMA2, the xz branch filters and CRC-64, in calx_xz.c, behind std.xz (CLX-173). A
 * decoder's context is CX_XZ_CTX bytes, aligned as cx_alloc aligns. The decoders write no further
 * than `cap`, give 0 at the end, 1 when they need more room, -1 for corrupt data, and -2 when the
 * data ends early, and carry on from where they stopped when called again with more room.
 * `cx_xz_len` is how far the output got, and `cx_xz_used` how many bytes of the input were taken.
 *
 * `cx_xz_lzma2_size` reads an LZMA2 stream's chunk headers for the size of what it decodes to, and
 * where it ends, into info[0] and info[1]. `cx_xz_lzma2_start` sets an LZMA2 decoder up with the
 * dictionary size its filter's properties give, to write from out[start], and
 * `cx_xz_lzma1_start` sets a .lzma file's decoder up from its header. */
#define CX_XZ_CTX 28672
CX_FN(cx_i32, cx_xz_lzma2_size, (const cx_u8* in, cx_usize n, cx_u64* info));
CX_FN(void, cx_xz_lzma2_start, (cx_u8* ctx, cx_u64 dict, cx_usize start));
CX_FN(cx_i32, cx_xz_lzma2, (cx_u8* ctx, const cx_u8* in, cx_usize n, cx_u8* out, cx_usize cap));
CX_FN(cx_i32, cx_xz_lzma1_start, (cx_u8* ctx, cx_u32 props, cx_u64 dict, cx_u64 size, cx_usize start));
CX_FN(cx_i32, cx_xz_lzma1, (cx_u8* ctx, const cx_u8* in, cx_usize n, cx_u8* out, cx_usize cap));
CX_FN(cx_usize, cx_xz_len, (const cx_u8* ctx));
CX_FN(cx_usize, cx_xz_used, (const cx_u8* ctx));
CX_FN(void, cx_xz_delta, (cx_u8* buf, cx_usize n, cx_u32 dist));
CX_FN(cx_i32, cx_xz_bcj, (cx_u32 id, cx_u8* buf, cx_usize n, cx_u32 start));
CX_FN(cx_u64, cx_xz_crc64, (cx_u64 crc, const cx_u8* p, cx_usize n));
/* Decodes blocks whose headers gave both sizes at once, on `parallel for`'s threads, each
 * CX_XZ_BLOCK numbers in `desc`, as calx_xz.c says, and sets ok[k] to whether block k decoded to
 * its size, with its padding and check right. */
#define CX_XZ_BLOCK 14
CX_FN(void, cx_xz_blocks, (const cx_u8* in, const cx_u64* desc, cx_usize n, cx_u8* out, cx_u8* ok));
/* For std.xz's Decompressor, which takes a file a piece at a time (CLX-179). `cx_xz_resume` counts
 * the input from 0 again and says whether more may come: then LZMA stops between symbols while a
 * symbol's most input is left, giving 2, and LZMA2 at a chunk that isn't all there, giving -3, so
 * each carries on when called again with more. `cx_xz_slide` moves what a decoder still needs to
 * the front of its output, matches' reach and all from `keep` on, and gives how far. A chain of
 * CX_XZ_CHAIN bytes undoes a block's filters before LZMA2 a piece at a time: `cx_xz_chain_start`
 * takes their IDs and arguments in the header's order, and `cx_xz_chain` takes what's come out of
 * LZMA2 and isn't through them all, from `at` into the block, and gives how much now is. */
CX_FN(void, cx_xz_resume, (cx_u8* ctx, cx_i32 partial));
CX_FN(cx_usize, cx_xz_slide, (cx_u8* ctx, cx_u8* buf, cx_usize keep));
#define CX_XZ_CHAIN 1024
CX_FN(void, cx_xz_chain_start, (cx_u8* ctx, const cx_u64* filters, cx_u32 count));
CX_FN(cx_usize, cx_xz_chain, (cx_u8* ctx, cx_u8* buf, cx_usize n, cx_u64 at, cx_i32 last));
/* LZMA2 and LZMA compression, in calx_xzenc.c, behind std.xz's `compress` (CLX-176).
 * `cx_xze_start` compresses `n` bytes in blocks, with the settings in `opt` that calx_xzenc.c
 * lists, on `parallel for`'s threads, and gives a handle: sizes[k] is how big block k's LZMA2 is,
 * with its end, and `cx_xze_take` copies it out. `cx_xze_end` frees it all. */
CX_FN(cx_u64, cx_xze_start, (const cx_u8* in, cx_u64 n, const cx_u64* opt, cx_u64* sizes));
CX_FN(cx_u64, cx_xze_blocks, (cx_u64 n, cx_u64 block));
CX_FN(void, cx_xze_take, (cx_u64 h, cx_u64 b, cx_u8* dst));
CX_FN(void, cx_xze_end, (cx_u64 h));
/* Zstandard decompression, in calx_zstd.c, behind std.zstd (CLX-181). `cx_zstd_frame` starts the
 * frame at in[at..n) in a context of `cx_zstd_ctx_size()` bytes, and `cx_zstd_blocks` decodes its
 * blocks straight onto `out`; `cx_zstd_dict` reads a dictionary's tables into
 * `cx_zstd_ent_size()` bytes. They give calx_zstd.c's ZS_ codes, negative for errors. */
CX_FN(cx_usize, cx_zstd_ctx_size, (void));
CX_FN(cx_usize, cx_zstd_ent_size, (void));
CX_FN(void, cx_zstd_init, (cx_u8* ctx));
CX_FN(void, cx_zstd_end, (cx_u8* ctx));
CX_FN(cx_i32, cx_zstd_dict, (const cx_u8* d, cx_usize n, cx_u8* ent, cx_u64* info));
CX_FN(cx_i32, cx_zstd_frame, (cx_u8* ctx, const cx_u8* in, cx_usize n, cx_usize at, const cx_u8* ent, cx_u32 dict_id, cx_u64 window_max, cx_u64* info));
CX_FN(cx_i32, cx_zstd_blocks,
      (cx_u8* ctx, const cx_u8* in, cx_usize n, cx_u8* out, cx_usize start, cx_usize* len, cx_usize cap, cx_usize limit, const cx_u8* dict, cx_usize dict_n, cx_u64* info));
CX_FN(cx_u64, cx_zstd_xxh64, (const cx_u8* p, cx_usize n, cx_u64 seed));
/* A trained dictionary's tables as the compressor takes them, in `cx_zstd_dict_params_size()`
 * bytes (CLX-186). */
CX_FN(cx_usize, cx_zstd_dict_params_size, (void));
CX_FN(cx_i32, cx_zstd_dict_params, (const cx_u8* d, cx_usize n, cx_u8* out));
/* Zstandard compression, in calx_zstdenc.c, behind std.zstd's `compress` (CLX-183).
 * `cx_zse_compress` writes the frame for in[0..n) at `level` into out, which has
 * `cx_zse_bound(n)` bytes, and gives its size; flags 1 asks for a checksum, which std.zstd
 * appends. `cx_zse_compress_dict` does it with a dictionary's content, its whole size, which
 * picks a small input's settings as libzstd does (CLX-189), its ID and, if has_params, tables
 * (CLX-186), and its key from `cx_zse_dict_key`, which the tables kept for its content are found
 * by, or 0 to find them by the content (CLX-190). */
CX_FN(cx_u64, cx_zse_bound, (cx_u64 n));
CX_FN(cx_u64, cx_zse_compress, (const cx_u8* in, cx_u64 n, cx_u8* out, cx_i32 level, cx_u32 flags));
CX_FN(cx_u64, cx_zse_compress_dict, (const cx_u8* in, cx_u64 n, cx_u8* out, cx_i32 level, cx_u32 flags, const cx_u8* content, cx_u64 content_n,
                                     cx_u64 dict_n, cx_u32 dict_id, const cx_u8* params, cx_u32 has_params, cx_u64 key));
CX_FN(cx_u64, cx_zse_dict_key, (void));
/* Trains a dictionary of at most cap bytes from count samples laid one after another, sizes[i]
 * bytes each, as libzstd's ZDICT_trainFromBuffer does (CLX-191): writes it into out and gives its
 * size, or 0 for too few samples or bytes, or a cap under 256. */
CX_FN(cx_u64, cx_zse_train, (const cx_u8* samples, const cx_u64* sizes, cx_u64 count, cx_u8* out, cx_u64 cap, cx_i32 level));
/* As libzstd's ZDICT_finalizeDictionary: content[0..cn) with tables for `level` from the samples,
 * at most cap bytes, into out: gives its size, or 0 for a cap under 256 or the content's size. */
CX_FN(cx_u64, cx_zse_finalize, (const cx_u8* content, cx_u64 cn, const cx_u8* samples, const cx_u64* sizes, cx_u64 count, cx_u8* out, cx_u64 cap, cx_i32 level));

/* SQLite, in calx_sqlite.c over runtime/sqlite/sqlite3.c, behind std.sqlite (CLX-108). A connection
 * and a statement are each a number, the pointer behind it. A call that fails leaves SQLite's
 * extended result code and a copy of its message in the connection, for `cx_sqlite_error_code`,
 * `cx_sqlite_error_len` and `cx_sqlite_error_copy`. Text and bytes come out as a length, then a
 * copy into room that long. */
CX_FN(cx_i64, cx_sqlite_open, (const cx_u8* path, cx_usize n, cx_i32 read_only));
CX_FN(cx_i32, cx_sqlite_close, (cx_i64 h));
CX_FN(cx_i32, cx_sqlite_error_code, (cx_i64 h));
CX_FN(cx_usize, cx_sqlite_error_len, (cx_i64 h));
CX_FN(void, cx_sqlite_error_copy, (cx_i64 h, cx_u8* out));
CX_FN(cx_i64, cx_sqlite_prepare, (cx_i64 h, const cx_u8* sql, cx_usize n));
CX_FN(void, cx_sqlite_release, (cx_i64 h, cx_i64 s));
CX_FN(cx_i32, cx_sqlite_bind_null, (cx_i64 h, cx_i64 s, cx_i32 i));
CX_FN(cx_i32, cx_sqlite_bind_int, (cx_i64 h, cx_i64 s, cx_i32 i, cx_i64 v));
CX_FN(cx_i32, cx_sqlite_bind_real, (cx_i64 h, cx_i64 s, cx_i32 i, cx_f64 v));
CX_FN(cx_i32, cx_sqlite_bind_text, (cx_i64 h, cx_i64 s, cx_i32 i, const cx_u8* p, cx_usize n));
CX_FN(cx_i32, cx_sqlite_bind_blob, (cx_i64 h, cx_i64 s, cx_i32 i, const cx_u8* p, cx_usize n));
CX_FN(cx_i32, cx_sqlite_parameter_count, (cx_i64 s));
CX_FN(cx_i32, cx_sqlite_parameter_index, (cx_i64 s, const cx_u8* name, cx_usize n));
CX_FN(cx_i32, cx_sqlite_step, (cx_i64 h, cx_i64 s));
CX_FN(cx_i64, cx_sqlite_run, (cx_i64 h, cx_i64 s));
CX_FN(void, cx_sqlite_reset, (cx_i64 s));
CX_FN(void, cx_sqlite_clear, (cx_i64 s));
CX_FN(cx_i32, cx_sqlite_column_count, (cx_i64 s));
CX_FN(cx_i32, cx_sqlite_column_type, (cx_i64 s, cx_i32 col));
CX_FN(cx_i64, cx_sqlite_column_int, (cx_i64 s, cx_i32 col));
CX_FN(cx_f64, cx_sqlite_column_real, (cx_i64 s, cx_i32 col));
CX_FN(cx_usize, cx_sqlite_column_text_len, (cx_i64 s, cx_i32 col));
CX_FN(void, cx_sqlite_column_text_copy, (cx_i64 s, cx_i32 col, cx_u8* out));
CX_FN(cx_usize, cx_sqlite_column_blob_len, (cx_i64 s, cx_i32 col));
CX_FN(void, cx_sqlite_column_blob_copy, (cx_i64 s, cx_i32 col, cx_u8* out));
CX_FN(cx_usize, cx_sqlite_column_name_len, (cx_i64 s, cx_i32 col));
CX_FN(void, cx_sqlite_column_name_copy, (cx_i64 s, cx_i32 col, cx_u8* out));
CX_FN(cx_i32, cx_sqlite_exec, (cx_i64 h, const cx_u8* sql, cx_usize n));
CX_FN(cx_i64, cx_sqlite_last_insert_id, (cx_i64 h));
CX_FN(cx_i32, cx_sqlite_autocommit, (cx_i64 h));
CX_FN(cx_usize, cx_sqlite_version_len, (void));
CX_FN(void, cx_sqlite_version_copy, (cx_u8* out));

/* Files and directories, in calx_fs.c, behind std.fs. A handle is an `cx_i64`, negative when
 * opening failed; a function that fails returns 0 or a negative number and leaves why for
 * `cx_fs_error_kind` and `cx_fs_error_code`, and one that makes text or a list leaves it for
 * `cx_fs_result_len` and `cx_fs_result_copy`, each per thread. They're declared here, as every
 * function std calls is, so a library reaches them through the program (D123, CLX-154). */
CX_FN(cx_i32, cx_fs_absolute, (const cx_u8* path, cx_usize len));
CX_FN(cx_i32, cx_fs_close, (cx_i64 h));
CX_FN(cx_i32, cx_fs_copy, (const cx_u8* from, cx_usize from_len, const cx_u8* to, cx_usize to_len));
CX_FN(cx_i32, cx_fs_current_dir, (void));
CX_FN(cx_i32, cx_fs_error_code, (void));
CX_FN(cx_u32, cx_fs_error_kind, (void));
CX_FN(cx_u32, cx_fs_kind, (const cx_u8* path, cx_usize len, cx_u32 follow));
CX_FN(cx_i32, cx_fs_list, (const cx_u8* path, cx_usize len));
CX_FN(cx_i32, cx_fs_list_typed, (const cx_u8* path, cx_usize len));
CX_FN(cx_i64, cx_fs_lock, (const cx_u8* path, cx_usize len, const cx_u8* bytes, cx_usize n));
CX_FN(cx_i32, cx_fs_make_dir, (const cx_u8* path, cx_usize len));
CX_FN(cx_i32, cx_fs_make_link, (const cx_u8* target, cx_usize target_len, const cx_u8* path, cx_usize len));
CX_FN(cx_i64, cx_fs_open, (const cx_u8* path, cx_usize len, cx_u32 write));
CX_FN(cx_i64, cx_fs_read, (cx_i64 h, cx_u8* to, cx_usize n));
CX_FN(cx_i32, cx_fs_read_link, (const cx_u8* path, cx_usize len));
CX_FN(cx_i32, cx_fs_real_path, (const cx_u8* path, cx_usize len));
CX_FN(cx_i32, cx_fs_remove_dir, (const cx_u8* path, cx_usize len));
CX_FN(cx_i32, cx_fs_remove_file, (const cx_u8* path, cx_usize len));
CX_FN(cx_i32, cx_fs_rename, (const cx_u8* from, cx_usize from_len, const cx_u8* to, cx_usize to_len, cx_u32 sync));
CX_FN(void, cx_fs_result_copy, (cx_u8* to));
CX_FN(cx_usize, cx_fs_result_len, (void));
CX_FN(cx_i32, cx_fs_seek, (cx_i64 h, cx_u64 at));
CX_FN(cx_u8, cx_fs_separator, (void));
CX_FN(cx_i32, cx_fs_set_current_dir, (const cx_u8* path, cx_usize len));
CX_FN(cx_i32, cx_fs_set_mode, (const cx_u8* path, cx_usize len, cx_u32 mode));
CX_FN(cx_i32, cx_fs_set_modified, (const cx_u8* path, cx_usize len, cx_i64 ms));
CX_FN(cx_i64, cx_fs_size, (cx_i64 h));
CX_FN(cx_i32, cx_fs_stat, (const cx_u8* path, cx_usize len));
CX_FN(cx_u32, cx_fs_stat_mode, (void));
CX_FN(cx_u32, cx_fs_stat_kind, (void));
CX_FN(cx_i32, cx_fs_handle_stat, (cx_i64 h));
CX_FN(cx_i64, cx_fs_stat_modified, (void));
CX_FN(cx_i64, cx_fs_stat_size, (void));
CX_FN(cx_i32, cx_fs_sync, (cx_i64 h));
CX_FN(cx_i32, cx_fs_temp_dir, (void));
CX_FN(cx_i32, cx_fs_unlock, (cx_i64 h));
CX_FN(cx_i32, cx_fs_write, (cx_i64 h, const cx_u8* from, cx_usize n));

/* Other programs, in calx_proc.c, behind std.process: started in a group that ends with the
 * program, talked to while they run, and waited for. Errors and results are left per thread, as
 * calx_fs.c leaves them. */
CX_FN(cx_usize, cx_proc_available, (cx_i64 h, cx_u32 i));
CX_FN(void, cx_proc_close_input, (cx_i64 h));
CX_FN(cx_i32, cx_proc_code, (cx_i64 h));
CX_FN(cx_i32, cx_proc_error_code, (void));
CX_FN(cx_u32, cx_proc_error_kind, (void));
CX_FN(cx_i32, cx_proc_exe, (void));
CX_FN(void, cx_proc_free, (cx_i64 h));
CX_FN(cx_i64, cx_proc_group, (void));
CX_FN(void, cx_proc_group_free, (cx_i64 g));
CX_FN(cx_u32, cx_proc_id, (cx_i64 h));
CX_FN(void, cx_proc_kill, (cx_i64 h));
CX_FN(cx_i64, cx_proc_line, (cx_i64 h, cx_u64 until));
CX_FN(cx_i32, cx_proc_poll, (cx_i64 h));
CX_FN(void, cx_proc_result_copy, (cx_u8* to));
CX_FN(cx_usize, cx_proc_result_len, (void));
CX_FN(cx_i32, cx_proc_signal, (cx_i64 h));
CX_FN(void, cx_proc_usage, (cx_i64 h, cx_u64* out));
CX_FN(cx_i64, cx_proc_some, (cx_i64 h, cx_u64 until));
CX_FN(cx_i64, cx_proc_spawn, (const cx_u8* spec_p, cx_usize spec_n, const cx_u8* input, cx_usize input_n, cx_i64 g));
CX_FN(cx_u32, cx_proc_state, (cx_i64 h));
CX_FN(void, cx_proc_take, (cx_i64 h, cx_u32 i, cx_u8* to, cx_usize n));
CX_FN(void, cx_proc_wait, (cx_i64 h));
CX_FN(cx_usize, cx_proc_wait_any, (const cx_i64* hs, cx_usize n));
CX_FN(cx_i32, cx_proc_which, (const cx_u8* name, cx_usize n));
CX_FN(cx_i32, cx_proc_write, (cx_i64 h, const cx_u8* d, cx_usize n));

/* This process, the terminal and sleeping, in calx_rt.c: std.process's memory, CPU time,
 * processors and `exit`, std.term's raw mode and size, whether standard output is a terminal, and
 * `std.thread.sleep`. */
CX_FN(cx_u64, cx_process_cpu_ns, (void));
CX_FN(void, cx_process_exit, (cx_u8 code));
CX_FN(cx_u64, cx_process_memory, (void));
CX_FN(cx_u64, cx_process_memory_limit, (void));
CX_FN(cx_u32, cx_process_processors, (void));
CX_FN(cx_u32, cx_out_is_terminal, (void));
CX_FN(void, cx_term_at_exit, (const cx_u8* p, cx_usize n));
CX_FN(void, cx_term_cooked, (void));
CX_FN(cx_u32, cx_term_raw, (void));
CX_FN(cx_usize, cx_term_read, (cx_u8* p, cx_usize n));
CX_FN(cx_u64, cx_term_size, (void));
CX_FN(void, cx_thread_sleep, (cx_u64 ns));

/* Hashing, for `Map` and `Set` (D94). The same value hashes the same on every platform and in
 * every run, so a table's layout, and the order a loop goes over it, depend only on what was
 * done to it. A word is mixed in with a folded multiply: the 128-bit product's halves, xored. */
#define CX_HASH_SEED 0x243F6A8885A308D3ull
#define CX_HASH_MUL 0x9E3779B97F4A7C15ull
CX_INLINE cx_u64 cx_hash_mix(cx_u64 a, cx_u64 b) {
    __uint128_t r = (__uint128_t)a * b;
    return (cx_u64)r ^ (cx_u64)(r >> 64);
}
CX_INLINE cx_u64 cx_hash_word(cx_u64 h, cx_u64 w) { return cx_hash_mix(h ^ w, CX_HASH_MUL); }
CX_FN(cx_u64, cx_hash_bytes, (const cx_u8* p, cx_usize n));
CX_INLINE cx_u64 cx_hash_str(cx_str s) { return cx_hash_bytes(s.ptr, s.len); }

/* The tables behind `Map` and `Set`: open addressing, with a control byte per slot that's EMPTY,
 * DELETED, or the top 7 bits of the hash of what's there. Lookups compare 16 control bytes at
 * once, and the 16 after the last slot repeat the first 16, so a group never wraps. A table has
 * a power of two slots, at least 4, filled to at most 7/8. An empty table has no slots and a mask
 * of 0, and its control bytes are `cx_table_empty`'s, which every lookup stops at.
 *
 * One allocation holds the slots, then the control bytes: the slots end where the control bytes
 * start, so a table needs no pointer to its slots. They're in order, so slot `i` is at the slots'
 * start plus `i`, which a loop over lookups works out once; hashbrown's slots, which go back from
 * the control bytes, took a subtraction at every key, and lookups 10% longer (CLX-234). A linked
 * table's list comes after the control bytes (D109). */
#define CX_SLOT(S, ctrl, mask, i) (((S*)(ctrl) - ((mask) + 1))[i])
#define CX_CTRL_EMPTY ((cx_u8)0x80)
#define CX_CTRL_DELETED ((cx_u8)0xFE)
#define CX_GROUP 16
#define CX_NOT_FOUND ((cx_usize)-1)

/* Where the first byte that's `a` or `b` is in `p` up to `end`, or `end`: sixteen bytes at a time
 * with SSE2, which every x86-64 has, so a short part costs no call, as memchr's would. */
#if defined(__x86_64__)
typedef char cx_v16 __attribute__((vector_size(16)));
#endif
CX_INLINE const cx_u8* cx_find2(const cx_u8* p, const cx_u8* end, cx_u8 a, cx_u8 b) {
#if defined(__x86_64__)
    cx_v16 va = (cx_v16){0} + (char)a, vb = (cx_v16){0} + (char)b;
    while (end - p >= 16) {
        cx_v16 v;
        __builtin_memcpy(&v, p, 16);
        int m = __builtin_ia32_pmovmskb128((cx_v16)((v == va) | (v == vb)));
        if (m) return p + __builtin_ctz((unsigned)m);
        p += 16;
    }
#endif
    while (p < end && *p != a && *p != b) p++;
    return p;
}
/* A loop over `s.split(sep)`: the next part from `*at`, and where the one after starts, or
 * CX_NOT_FOUND after the last. 0 when there are no more. */
CX_INLINE cx_bool cx_text_split_next(cx_str s, cx_str sep, cx_usize* at, cx_str* part) {
    cx_usize i = *at;
    if (i == CX_NOT_FOUND) return 0;
    if (sep.len == 1) {
        const cx_u8* e = cx_find2(s.ptr + i, s.ptr + s.len, sep.ptr[0], sep.ptr[0]);
        cx_usize k = (cx_usize)(e - s.ptr);
        *part = (cx_str){s.ptr + i, k - i};
        *at = k == s.len ? CX_NOT_FOUND : k + 1;
        return 1;
    }
    if (sep.len == 0) {
        if (i >= s.len) return 0;
        cx_usize j = i;
        cx_utf8_next(s.ptr, &j);
        *part = (cx_str){s.ptr + i, j - i};
        *at = j;
        return 1;
    }
    cx_usize k = cx_str_find((cx_str){s.ptr + i, s.len - i}, sep);
    if (k == CX_NOT_FOUND) {
        *part = (cx_str){s.ptr + i, s.len - i};
        *at = CX_NOT_FOUND;
        return 1;
    }
    *part = (cx_str){s.ptr + i, k};
    *at = i + k + sep.len;
    return 1;
}
/* A loop over `s.lines()`: the next line from `*at`, without the `\n`, `\r\n` or `\r` that ends
 * it, as Java's `readLine` and `std.io.Lines` end lines. */
CX_INLINE cx_bool cx_text_lines_next(cx_str s, cx_usize* at, cx_str* part) {
    cx_usize i = *at;
    if (i >= s.len) return 0;
    cx_usize j = (cx_usize)(cx_find2(s.ptr + i, s.ptr + s.len, '\n', '\r') - s.ptr);
    *part = (cx_str){s.ptr + i, j - i};
    *at = j + 1 + (j + 1 < s.len && s.ptr[j] == '\r' && s.ptr[j + 1] == '\n');
    return 1;
}
#ifdef CX_LIBRARY
/* A library's tables start at the program's, so either can free the other's (D123). */
extern const cx_u8* cx_table_empty_at;
#define cx_table_empty cx_table_empty_at
#else
extern const cx_u8 cx_table_empty[CX_GROUP];
#endif

#if defined(__SSE2__)
typedef char cx_group_v __attribute__((vector_size(16), aligned(1)));
CX_INLINE cx_u32 cx_group_match(const cx_u8* g, cx_u8 b) {
    cx_group_v v = *(const cx_group_v*)g;
    return (cx_u32)__builtin_ia32_pmovmskb128(v == (char)b);
}
/* EMPTY or DELETED: the control bytes with the top bit set. */
CX_INLINE cx_u32 cx_group_free(const cx_u8* g) {
    return (cx_u32)__builtin_ia32_pmovmskb128(*(const cx_group_v*)g);
}
#else
CX_INLINE cx_u32 cx_group_match(const cx_u8* g, cx_u8 b) {
    cx_u32 m = 0;
    for (int i = 0; i < CX_GROUP; i++) m |= (cx_u32)(g[i] == b) << i;
    return m;
}
CX_INLINE cx_u32 cx_group_free(const cx_u8* g) {
    cx_u32 m = 0;
    for (int i = 0; i < CX_GROUP; i++) m |= (cx_u32)(g[i] >> 7) << i;
    return m;
}
#endif
CX_INLINE cx_u32 cx_group_empty(const cx_u8* g) { return cx_group_match(g, CX_CTRL_EMPTY); }
/* The slots that hold a value. A loop over a table goes a group at a time: from 0 in steps of
 * 16 while the step is at most the mask, then over these bits. A table smaller than a group has
 * EMPTY bytes after its slots, so only its own show. */
CX_INLINE cx_u32 cx_group_full(const cx_u8* g) { return ~cx_group_free(g) & 0xFFFFu; }
CX_INLINE cx_u8 cx_table_tag(cx_u64 h) { return (cx_u8)(h >> 57); }
/* How many values fit in a table of `buckets` slots. */
CX_INLINE cx_usize cx_table_capacity(cx_usize buckets) { return buckets < 8 ? buckets - 1 : buckets / 8 * 7; }
/* Sets slot `i`'s control byte, and its copy past the end if it has one. */
CX_INLINE void cx_table_set(cx_u8* ctrl, cx_usize mask, cx_usize i, cx_u8 c) {
    ctrl[i] = c;
    ctrl[((i - CX_GROUP) & mask) + CX_GROUP] = c;
}
/* The first EMPTY or DELETED slot where the hash `h` would go. A group can show a slot past the
 * end of a table smaller than a group, which wraps to a full one; then the first group has one. */
CX_INLINE cx_usize cx_table_free_slot(const cx_u8* ctrl, cx_usize mask, cx_u64 h) {
    cx_usize pos = (cx_usize)h & mask, stride = 0;
    for (;;) {
        cx_u32 bits = cx_group_free(ctrl + pos);
        if (bits) {
            cx_usize i = (pos + (cx_usize)__builtin_ctz(bits)) & mask;
            if (__builtin_expect(ctrl[i] < 0x80, 0)) i = (cx_usize)__builtin_ctz(cx_group_free(ctrl));
            return i;
        }
        stride += CX_GROUP;
        pos = (pos + stride) & mask;
    }
}
/* Marks slot `i` free after its value is taken out. It can become EMPTY, which ends lookups, only
 * if no lookup could have passed it: if a group around it already has an EMPTY. */
CX_INLINE cx_bool cx_table_erase(cx_u8* ctrl, cx_usize mask, cx_usize i) {
    cx_u32 before = cx_group_empty(ctrl + ((i - CX_GROUP) & mask));
    cx_u32 after = cx_group_empty(ctrl + i);
    /* The EMPTY bytes just before slot `i` and from it, counted without a branch: 16 when the
     * group has none. clang 19 counts a 16-bit value directly; before it, an extra bit stops the
     * count at 16. */
#if __has_builtin(__builtin_clzg)
    int lead = __builtin_clzg((cx_u16)before, 16);
    int trail = __builtin_ctzg((cx_u16)after, 16);
#else
    int lead = __builtin_clz((before << 16) | 0x8000u);
    int trail = __builtin_ctz(after | 0x10000u);
#endif
    cx_bool empty = lead + trail < CX_GROUP;
    cx_table_set(ctrl, mask, i, empty ? CX_CTRL_EMPTY : CX_CTRL_DELETED);
    return empty;
}
/* The slot count to grow to when a value needs room: double, or the same count again if half
 * the room went to DELETED slots, which rebuilding frees. */
CX_INLINE cx_usize cx_table_grown(cx_usize buckets, cx_usize len) {
    if (buckets == 0) return 4;
    return len + 1 <= cx_table_capacity(buckets) / 2 ? buckets : buckets * 2;
}
/* The slot count with room for `n` values. */
CX_FN(cx_usize, cx_table_buckets_for, (cx_usize n));
/* A table of `buckets` slots of `size` bytes, all EMPTY, with `extra` bytes after its control
 * bytes: gives the control bytes, with the slots before them. */
CX_FN(cx_u8*, cx_table_alloc, (cx_usize buckets, cx_usize size, cx_usize extra));
/* Frees what cx_table_alloc gave. */
CX_FN(void, cx_table_free, (cx_u8* ctrl, cx_usize buckets, cx_usize size, cx_usize extra));

/* A `LinkedMap` or a `LinkedSet` is a table as above with a list through its full slots, in the
 * order their keys went in (D109). The list is two `cx_u32`s a slot after the control bytes, so
 * both are in the same cache line: `link[2 * at]` is the one before and `link[2 * at + 1]` the one
 * after, where `at` is the slot plus 1. `at` 0 is the list's own head, whose links are its last
 * and first slots, so a list that's empty is two zeros, and taking a slot out or putting one in
 * takes no branch. Only full slots' links are ever read. */
CX_INLINE cx_usize cx_lk_bytes(cx_usize buckets) { return (buckets + 1) * 2 * sizeof(cx_u32); }
CX_INLINE cx_u32* cx_lk_links(cx_u8* ctrl, cx_usize mask) { return (cx_u32*)(ctrl + mask + 1 + CX_GROUP); }
/* The first slot plus 1 (`end` 1) or the last (`end` 0), or 0 when the table is empty. */
CX_INLINE cx_u32 cx_lk_end(cx_u8* ctrl, cx_usize mask, int end) { return mask ? cx_lk_links(ctrl, mask)[end] : 0; }
CX_INLINE void cx_lk_unlink(cx_u32* link, cx_usize i) {
    cx_u32 prev = link[2 * i + 2], next = link[2 * i + 3];
    link[2 * prev + 1] = next;
    link[2 * next] = prev;
}
CX_INLINE void cx_lk_push_back(cx_u32* link, cx_usize i) {
    cx_u32 at = (cx_u32)i + 1, last = link[0];
    link[2 * at] = last;
    link[2 * at + 1] = 0;
    link[2 * last + 1] = at;
    link[0] = at;
}
CX_INLINE void cx_lk_push_front(cx_u32* link, cx_usize i) {
    cx_u32 at = (cx_u32)i + 1, first = link[1];
    link[2 * at] = 0;
    link[2 * at + 1] = first;
    link[2 * first] = at;
    link[1] = at;
}
/* Moves slot `i`, which is in the list, to its front or its back. */
CX_INLINE void cx_lk_move(cx_u32* link, cx_usize i, cx_bool front) {
    if (link[front] == (cx_u32)i + 1) return;
    cx_lk_unlink(link, i);
    if (front) cx_lk_push_front(link, i);
    else cx_lk_push_back(link, i);
}
CX_COLD CX_FN(void, cx_panic_key, (const char* at));
/* A `Handle<T>`: the index of a slot in a `SlotMap<T>` and the generation it had when its value
 * went in (CLX-69). The same struct for every `T`. */
typedef struct {
    cx_u32 index;
    cx_u32 gen;
} cx_handle;
/* The handle as one number, which compares and hashes as the two do: the index, then the
 * generation. */
CX_INLINE cx_u64 cx_handle_key(cx_handle h) { return (cx_u64)h.index << 32 | h.gen; }
/* Prints `Handle(3, 1)`. */
CX_FN(void, cx_print_handle, (cx_handle h));
/* `m[h]` with a handle whose value was removed. */
CX_COLD CX_FN(void, cx_panic_handle, (cx_handle h, const char* at));
/* `m.range(a..b)` on a `SortedMap` or a `SortedSet`, with `b` before `a` (D112). */
CX_COLD CX_FN(void, cx_panic_range, (const char* at));
/* A panic that says `message`, as `(a..b).step_by(0)` does (CLX-101). */
CX_COLD CX_FN(void, cx_panic_text, (const char* at, const char* message));

/* `u64.div_wide(hi, lo, d)`: the 128-bit number `hi:lo` divided by `d`, which the caller has
 * checked is more than `hi`, so the quotient fits (CLX-102). x86-64 divides 128 bits by 64 in one
 * instruction, which clang's 128-bit `/` doesn't use, since it can't know the quotient fits;
 * elsewhere that `/` does it. */
CX_INLINE cx_u64 cx_div_wide_u64(cx_u64 hi, cx_u64 lo, cx_u64 d, cx_u64* rem) {
#if defined(__x86_64__)
    cx_u64 q, r;
    __asm__("divq %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "rm"(d));
    *rem = r;
    return q;
#else
    unsigned __int128 n = ((unsigned __int128)hi << 64) | lo;
    cx_u64 q = (cx_u64)(n / d);
    *rem = lo - q * d; /* below `d`, so its low 64 bits are all of it */
    return q;
#endif
}
/* `q.remove_marked(marks)` on a `PriorityQueue`, without one mark for each element (D113). */
CX_COLD CX_FN(void, cx_panic_marks, (cx_usize marks, cx_usize length, const char* at));
/* `m.remove_marked(marks)` on an `OpenHashMap` or an `OpenHashSet`, without one mark for each key
 * (D173). */
CX_COLD CX_FN(void, cx_panic_key_marks, (cx_usize marks, cx_usize keys, const char* at));
/* An `OpenHashMap`'s or an `OpenHashSet`'s load factor isn't between 0 and 1, or it's asked for
 * more keys than fastutil's tables hold. */
CX_COLD CX_FN(void, cx_panic_load_factor, (const char* at));
CX_COLD CX_FN(void, cx_panic_table_size, (const char* at));

/* The tables behind `JavaHashMap` and `JavaHashSet`, in calx_jtable.c (D105): Java's HashMap,
 * which iterates in Java's order. `heads` is a power of two bins, each 0 when empty, the first
 * node plus one for a list, and minus that for a red-black tree. The nodes are in one array, each
 * starting with its key's spread hash and the next node in its bin, or -1; the compiler's node
 * type follows them with the key, or the key and value. A node that's removed waits in `spare`
 * for the next key. Tree bins keep their links in `tree`, beside the nodes, made with the first. */
typedef struct {
    cx_i32 prev, parent, left, right;
    cx_bool red;
} cx_jtnode;
typedef struct {
    cx_i32* heads;    /* `cap` bins, or null before the first key */
    void* nodes;      /* room for `room` nodes, `used` of them handed out */
    cx_jtnode* tree;  /* the tree links, for `room` nodes, or null */
    cx_usize len;     /* the keys */
    cx_u32 cap;       /* the bins */
    cx_u32 thr;       /* Java's threshold: the table doubles when `len` passes it. Before the first
                       * table, the bins Java's `new HashMap<>(n)` asked for, or 0 for 16 */
    cx_u32 used;
    cx_u32 room;
    cx_u32 spare;     /* a removed node plus one, or 0; its `next` is the one removed before it */
} cx_jtable;
/* Whether node `a`'s key orders before (-1), with (0) or after (1) node `b`'s, for tree bins. */
typedef int (*cx_jt_order)(const void* nodes, cx_i32 a, cx_i32 b);
#define CX_JT_HASH(nodes, size, i) (((cx_i32*)((char*)(nodes) + (cx_usize)(i) * (size)))[0])
#define CX_JT_NEXT(nodes, size, i) (((cx_i32*)((char*)(nodes) + (cx_usize)(i) * (size)))[1])
/* Java spreads a key's `hashCode` so its top bits pick bins in small tables too. */
CX_INLINE cx_i32 cx_jt_spread(cx_i32 h) { return h ^ (cx_i32)((cx_u32)h >> 16); }
/* The first node of a bin, or -1. */
CX_INLINE cx_i32 cx_jt_first(cx_i32 head) { return (head < 0 ? -head : head) - 1; }
CX_INLINE cx_i32 cx_java_long_hash(cx_u64 v) { return (cx_i32)(v ^ (v >> 32)); }
/* `Float.hashCode` and `Double.hashCode`: the bits, with every NaN as Java's one NaN. */
CX_INLINE cx_i32 cx_java_float_hash(cx_f32 f) {
    cx_u32 b = 0x7FC00000u;
    if (f == f) __builtin_memcpy(&b, &f, 4);
    return (cx_i32)b;
}
CX_INLINE cx_i32 cx_java_double_hash(cx_f64 d) {
    cx_u64 b = 0x7FF8000000000000ull;
    if (d == d) __builtin_memcpy(&b, &d, 8);
    return (cx_i32)(b ^ (b >> 32));
}
/* The bins Java's `new HashMap<>(n)` starts with: the power of two at least `n`. */
CX_FN(cx_u32, cx_jt_size_for, (cx_usize n));
/* A node for a new key, with its hash and `next` still to set. The nodes can move. */
CX_FN(cx_i32, cx_jt_new_node, (cx_jtable* m, cx_usize size));
CX_INLINE void cx_jt_release(cx_jtable* m, cx_usize size, cx_i32 i) {
    CX_JT_NEXT(m->nodes, size, i) = (cx_i32)m->spare - 1;
    m->spare = (cx_u32)i + 1;
}
/* Java's resize: the first table, or twice the bins, each bin split in order. */
CX_FN(void, cx_jt_resize, (cx_jtable* m, cx_usize size, cx_jt_order order));
/* A list bin that a key made too long: a tree, or a table twice the size if it's small. */
CX_FN(void, cx_jt_treeify_bin, (cx_jtable* m, cx_usize size, cx_jt_order order, cx_i32 h));
/* Adds the node `x`, with its hash and key set, to the tree in bin `b`. */
CX_FN(void, cx_jt_tree_put, (cx_jtable* m, cx_usize size, cx_jt_order order, cx_u32 b, cx_i32 x));
/* Takes the node `z` out of the tree in bin `b`, which may become a list. */
CX_FN(void, cx_jt_tree_remove, (cx_jtable* m, cx_usize size, cx_u32 b, cx_i32 z));
CX_FN(void, cx_jt_reserve, (cx_jtable* m, cx_usize size, cx_jt_order order, cx_usize n));
CX_FN(void, cx_jt_free, (cx_jtable* m, cx_usize size));
CX_FN(cx_jtable, cx_jt_copy, (const cx_jtable* m, cx_usize size));
CX_FN(void, cx_jt_clear, (cx_jtable* m));
/* Java's `String.compareTo`, by UTF-16 unit: -1, 0 or 1. */
CX_FN(int, cx_jt_str_order, (cx_str a, cx_str b));

/* The tables behind `OpenHashMap` and `OpenHashSet` (D107): fastutil's open hash tables, which
 * iterate in fastutil's order. A table is `n` slots, a power of two, found by linear probing from
 * the slot its key's mixed hash picks. An integer key of 0 marks an empty slot, so the key 0
 * itself is kept in slot `n`, past the others, and `zero` says whether it's there. Other keys
 * have a hash word per slot, 0 when it's empty. The compiler's table type starts with its slots
 * and those words, then this header; finding, adding, removing and rehashing are the compiler's,
 * for each key type, and the sizes fastutil picks are here, in calx_ohtable.c. */
typedef struct {
    cx_u32 n;         /* the slots, or 0 in a table made without a size, before its first use */
    cx_u32 mask;      /* n - 1 */
    cx_u32 len;       /* the keys */
    cx_u32 max_fill;  /* an insert that finds `len` at this rehashes */
    cx_u32 min_n;     /* the slots it was made with, which removing keys won't shrink it below */
    cx_f32 f;         /* the load factor */
    cx_bool zero;     /* integer keys: whether 0 is a key */
} cx_ohead;
/* `HashCommon.mix`: the hash an `int`, a `long` or an object's `hashCode` places a key by. */
CX_INLINE cx_u32 cx_oh_mix32(cx_i32 x) {
    cx_u32 h = (cx_u32)x * 0x9E3779B9u;
    return h ^ (h >> 16);
}
CX_INLINE cx_u32 cx_oh_mix64(cx_u64 x) {
    cx_u64 h = x * 0x9E3779B97F4A7C15ull;
    h ^= h >> 32;
    return (cx_u32)(h ^ (h >> 16));
}
/* What a slot of a key that isn't an integer keeps of its mixed hash: the 30 bits that place it
 * in any table, with the top bit set, so no full slot's word is 0. */
CX_INLINE cx_u32 cx_oh_tag(cx_u32 h) { return (h & 0x3FFFFFFFu) | 0x80000000u; }
/* fastutil's constructor: the header for `expected` keys at the load factor `f`. */
CX_FN(cx_ohead, cx_oh_new, (cx_usize expected, cx_f32 f, const char* at));
/* A table made without a size becomes fastutil's default one, for 16 keys at 0.75, when it's
 * first used. */
CX_INLINE void cx_oh_ready(cx_ohead* h) {
    if (__builtin_expect(!h->n, 0)) *h = (cx_ohead){ 32, 31, 0, 24, 32, 0.75f, 0 };
}
/* `HashCommon.arraySize` and `maxFill`. */
CX_FN(cx_u32, cx_oh_array_size, (cx_u64 expected, cx_f32 f));
CX_FN(cx_u32, cx_oh_max_fill, (cx_u32 n, cx_f32 f));
/* The slots `putAll` and `addAll` make room in before adding `c` keys. */
CX_FN(cx_u32, cx_oh_reserve_size, (const cx_ohead* h, cx_usize c));
/* The slots `trim()` shrinks the table to, or 0 when it stays. */
CX_FN(cx_u32, cx_oh_trim_size, (const cx_ohead* h));

/* Bounds checks. */
CX_INLINE cx_usize cx_index(cx_usize i, cx_usize length, const char* at) {
    if (__builtin_expect(i >= length, 0)) cx_panic_index(i, length, at);
    return i;
}

/* Integer division (D55): division by zero panics, and MIN / -1 gives MIN, as in Java.
 * i8 and i16 divide as i32 and wrap back, so they can't overflow. */
#define CX_DIV_SMALL(T)                                                                  \
    CX_INLINE cx_##T cx_div_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        return (cx_##T)((cx_i32)a / (cx_i32)b);                                          \
    }                                                                                    \
    CX_INLINE cx_##T cx_rem_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        return (cx_##T)((cx_i32)a % (cx_i32)b);                                          \
    }
#define CX_DIV_SIGNED(T, U)                                                              \
    CX_INLINE cx_##T cx_div_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        if (b == -1) return (cx_##T)((U)0 - (U)a);                                       \
        return a / b;                                                                    \
    }                                                                                    \
    CX_INLINE cx_##T cx_rem_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        if (b == -1) return 0;                                                           \
        return a % b;                                                                    \
    }
#define CX_DIV_UNSIGNED(T)                                                               \
    CX_INLINE cx_##T cx_div_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        return (cx_##T)(a / b);                                                          \
    }                                                                                    \
    CX_INLINE cx_##T cx_rem_##T(cx_##T a, cx_##T b, const char* at) {                    \
        if (__builtin_expect(b == 0, 0)) cx_panic_div_zero(at);                          \
        return (cx_##T)(a % b);                                                          \
    }
CX_DIV_SMALL(i8)
CX_DIV_SMALL(i16)
CX_DIV_SIGNED(i32, cx_u32)
CX_DIV_SIGNED(i64, cx_u64)
CX_DIV_UNSIGNED(u8)
CX_DIV_UNSIGNED(u16)
CX_DIV_UNSIGNED(u32)
CX_DIV_UNSIGNED(u64)
CX_DIV_UNSIGNED(usize)

/* Java's Math.floorDiv and Math.floorMod. */
#define CX_FLOOR(T)                                                                      \
    CX_INLINE cx_##T cx_floor_div_##T(cx_##T a, cx_##T b, const char* at) {              \
        cx_##T q = cx_div_##T(a, b, at);                                                 \
        if ((a ^ b) < 0 && (cx_##T)(q * b) != a) q--;                                   \
        return q;                                                                        \
    }                                                                                    \
    CX_INLINE cx_##T cx_floor_mod_##T(cx_##T a, cx_##T b, const char* at) {              \
        cx_##T r = cx_rem_##T(a, b, at);                                                 \
        if ((r ^ b) < 0 && r != 0) r = (cx_##T)(r + b);                                  \
        return r;                                                                        \
    }
CX_FLOOR(i8)
CX_FLOOR(i16)
CX_FLOOR(i32)
CX_FLOOR(i64)

/* Java's float-to-integer casts: clamp to the range, and NaN gives 0. */
static inline __attribute__((nodebug)) cx_i32 cx_f64_to_i32_clamp(cx_f64 d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return -2147483647 - 1;
    return (cx_i32)d;
}
static inline __attribute__((nodebug)) cx_i64 cx_f64_to_i64_clamp(cx_f64 d) {
    if (d != d) return 0;
    if (d >= 9223372036854775807.0) return 9223372036854775807LL;
    if (d <= -9223372036854775808.0) return -9223372036854775807LL - 1;
    return (cx_i64)d;
}
static inline __attribute__((nodebug)) cx_i32 cx_f32_to_i32_clamp(cx_f32 f) {
    if (f != f) return 0;
    if (f >= 2147483648.0f) return 2147483647;
    if (f <= -2147483648.0f) return -2147483647 - 1;
    return (cx_i32)f;
}
static inline __attribute__((nodebug)) cx_i64 cx_f32_to_i64_clamp(cx_f32 f) {
    if (f != f) return 0;
    if (f >= 9223372036854775808.0f) return 9223372036854775807LL;
    if (f <= -9223372036854775808.0f) return -9223372036854775807LL - 1;
    return (cx_i64)f;
}
#if defined(__x86_64__)
/* x86's truncating conversions give the type's MIN for NaN and for anything out of range, so one
 * test of the result finds every case that needs clamping: one compare instead of three. */
typedef double cx_v2df __attribute__((vector_size(16)));
typedef float cx_v4sf __attribute__((vector_size(16)));
CX_INLINE cx_i32 cx_f64_to_i32(cx_f64 d) {
    cx_i32 r = __builtin_ia32_cvttsd2si((cx_v2df){d, 0.0});
    if (__builtin_expect(r == -2147483647 - 1, 0)) return cx_f64_to_i32_clamp(d);
    return r;
}
CX_INLINE cx_i64 cx_f64_to_i64(cx_f64 d) {
    cx_i64 r = __builtin_ia32_cvttsd2si64((cx_v2df){d, 0.0});
    if (__builtin_expect(r == -9223372036854775807LL - 1, 0)) return cx_f64_to_i64_clamp(d);
    return r;
}
CX_INLINE cx_i32 cx_f32_to_i32(cx_f32 f) {
    cx_i32 r = __builtin_ia32_cvttss2si((cx_v4sf){f, 0.0f, 0.0f, 0.0f});
    if (__builtin_expect(r == -2147483647 - 1, 0)) return cx_f32_to_i32_clamp(f);
    return r;
}
CX_INLINE cx_i64 cx_f32_to_i64(cx_f32 f) {
    cx_i64 r = __builtin_ia32_cvttss2si64((cx_v4sf){f, 0.0f, 0.0f, 0.0f});
    if (__builtin_expect(r == -9223372036854775807LL - 1, 0)) return cx_f32_to_i64_clamp(f);
    return r;
}
#else
CX_INLINE cx_i32 cx_f64_to_i32(cx_f64 d) { return cx_f64_to_i32_clamp(d); }
CX_INLINE cx_i64 cx_f64_to_i64(cx_f64 d) { return cx_f64_to_i64_clamp(d); }
CX_INLINE cx_i32 cx_f32_to_i32(cx_f32 f) { return cx_f32_to_i32_clamp(f); }
CX_INLINE cx_i64 cx_f32_to_i64(cx_f32 f) { return cx_f32_to_i64_clamp(f); }
#endif

/* Java's Math.round: the nearest integer, with halves rounded up, clamped as a cast is. x - floor(x)
 * is exact, except just below 0, where it can round up to 1, which still rounds to 0 as it should. */
CX_INLINE cx_i64 cx_round_f64(cx_f64 d) {
    cx_f64 f = __builtin_floor(d);
    return cx_f64_to_i64(d - f >= 0.5 ? f + 1.0 : f);
}
CX_INLINE cx_i32 cx_round_f32(cx_f32 x) {
    cx_f32 f = __builtin_floorf(x);
    return cx_f32_to_i32(x - f >= 0.5f ? f + 1.0f : f);
}

/* abs, min and max. Integer abs wraps, so abs(MIN) is MIN, as in Java. */
#define CX_INT_MINMAX(T)                                                                 \
    CX_INLINE cx_##T cx_min_##T(cx_##T a, cx_##T b) { return a <= b ? a : b; }           \
    CX_INLINE cx_##T cx_max_##T(cx_##T a, cx_##T b) { return a >= b ? a : b; }
#define CX_INT_ABS(T, U)                                                                 \
    CX_INLINE cx_##T cx_abs_##T(cx_##T a) { return (cx_##T)(a < 0 ? (U)0 - (U)a : (U)a); }
CX_INT_MINMAX(i8)
CX_INT_MINMAX(i16)
CX_INT_MINMAX(i32)
CX_INT_MINMAX(i64)
CX_INT_MINMAX(u8)
CX_INT_MINMAX(u16)
CX_INT_MINMAX(u32)
CX_INT_MINMAX(u64)
CX_INT_MINMAX(usize)
CX_INT_ABS(i8, cx_u32)
CX_INT_ABS(i16, cx_u32)
CX_INT_ABS(i32, cx_u32)
CX_INT_ABS(i64, cx_u64)

/* Java's Math.min and Math.max on floats: NaN if either is NaN, and -0.0 is less than 0.0. */
#define CX_FLOAT_MINMAX(T, SIGNBIT)                                                      \
    CX_INLINE cx_##T cx_min_##T(cx_##T a, cx_##T b) {                                    \
        if (a != a) return a;                                                            \
        if (b != b) return b;                                                            \
        if (a == b) return SIGNBIT(a) ? a : b;                                           \
        return a < b ? a : b;                                                            \
    }                                                                                    \
    CX_INLINE cx_##T cx_max_##T(cx_##T a, cx_##T b) {                                    \
        if (a != a) return a;                                                            \
        if (b != b) return b;                                                            \
        if (a == b) return SIGNBIT(a) ? b : a;                                           \
        return a > b ? a : b;                                                            \
    }
CX_FLOAT_MINMAX(f32, __builtin_signbit)
CX_FLOAT_MINMAX(f64, __builtin_signbit)
CX_INLINE cx_f32 cx_abs_f32(cx_f32 a) { return __builtin_fabsf(a); }
CX_INLINE cx_f64 cx_abs_f64(cx_f64 a) { return __builtin_fabs(a); }
/* Java's `Double.compare` and `Float.compare`, a total order: -0.0 comes before 0.0, and NaN after
 * everything, equal to itself. Values that `<` orders take the first two tests; for the rest,
 * every NaN is made the same, and the bits ordered as a signed integer, which puts -0.0 first. */
#define CX_TOTAL_CMP(T, I, U, NAN_BITS)                                                      CX_INLINE int cx_total_cmp_##T(cx_##T a, cx_##T b) {                                        if (a < b) return -1;                                                                    if (a > b) return 1;                                                                     I x, y;                                                                                  __builtin_memcpy(&x, &a, sizeof x);                                                      __builtin_memcpy(&y, &b, sizeof y);                                                      if (a != a) x = (I)(U)NAN_BITS;                                                          if (b != b) y = (I)(U)NAN_BITS;                                                          return (x > y) - (x < y);                                                            }
CX_TOTAL_CMP(f32, cx_i32, cx_u32, 0x7fc00000u)
CX_TOTAL_CMP(f64, cx_i64, cx_u64, 0x7ff8000000000000ull)

#endif
