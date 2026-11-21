/* The Calx runtime's TCP networking, behind std.net (D33, D92).
 *
 * A poller owns its connections, and one thread at a time uses it (see net_enter), so each thread
 * with an event loop of its own has its own poller, and they poll at once. The runtime keeps
 * each connection's buffers: what arrived, waiting for `read`, and what `send` queued, waiting
 * for the socket. `poll` reports what happened as events: a connection accepted, an outgoing one
 * open, bytes received, a connection closed, or a waker woke it. On Windows that's IOCP, whose
 * operations complete into the runtime's buffers. On Linux it's edge-triggered epoll, and on macOS
 * a kqueue with EV_CLEAR, which is the same, and the runtime reads and writes when the socket is
 * ready. Either way the program sees the same events.
 *
 * What `send` queues waits for the next poll or flush, so everything a program writes to a
 * connection between two polls goes to the system at once: one `WSASend` or `send` for many
 * writes, which a server that writes many packets a tick needs.
 *
 * `send_file` queues a range of an open file after what's queued: the system sends it from the
 * file without it passing through the program, with TransmitFile on Windows and sendfile on
 * Linux, from a handle of its own to the file. What's written while it goes waits in `after`, and
 * goes once the file has. Files queued while one goes wait in `waiting`, each with what's written
 * after it, and take their turns: on Windows a file's TransmitFile can complete after the other
 * end has every byte and asked for the next, so a server's next file often comes while one is
 * still going.
 *
 * A connection's id is its slot in the poller and the slot's generation, so a stale id never
 * reaches a newer connection in the same slot. A slot is reused only after the poll that
 * reported its `Closed` event, and, on Windows, once the system has returned every operation.
 *
 * UDP sockets are slots of the same poller (CLX-412), so a program's datagrams come as events
 * in the loop its connections do. Each datagram that arrives waits in the slot's `in`, with the
 * address it came from, until the program takes it, and a `Datagram` event says some did. A
 * datagram is sent at once, unless the socket has no room, when it waits in `out`, each with
 * where it goes and the multicast interface it was sent on, for the socket to take it: on Linux
 * and macOS when it says it's writable, and on Windows by an overlapped WSASendTo at a time.
 *
 * This file is compiled into a program only when it uses std.net, so other programs don't link
 * the socket libraries. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <iphlpapi.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#else
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/event.h>
#include <sys/types.h>
#include <sys/uio.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* each socket has SO_NOSIGPIPE instead */
#endif
#else
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#endif
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "calx_rt.h"

int cx_in_parallel(void);
CX_COLD void cx_panic_text(const char* at, const char* message);

enum { EV_ACCEPTED = 1, EV_CONNECTED = 2, EV_RECEIVED = 3, EV_CLOSED = 4, EV_WOKEN = 5, EV_DATAGRAM = 6 };
enum { ERR_IN_USE = 1, ERR_ADDRESS = 2, ERR_REFUSED = 3, ERR_OTHER = 4 };
enum { SLOT_FREE, SLOT_LISTENER, SLOT_CONN, SLOT_UDP };
enum { CONN_CONNECTING, CONN_OPEN, CONN_CLOSING, CONN_CLOSED };

#define NET_NONE 0xFFFFFFFFu
#define NET_INDEX_BITS 24
#define NET_RECV 16384 /* bytes read at a time */
#define NET_UDP_MAX 65536 /* the most a datagram can hold, and more */
#define NET_UDP_KEEP (1 << 20) /* bytes of datagrams a UDP socket keeps waiting, either way */
#define UDP_HEAD 27 /* before each datagram that arrived: its length, then its address (addr_parts) */

static _Thread_local cx_u32 net_err_kind;
static _Thread_local cx_i32 net_err_code;

static void net_fail(cx_u32 kind, cx_i32 code) {
    net_err_kind = kind;
    net_err_code = code;
}

static void net_check_thread(void) {
    if (cx_in_parallel()) {
        cx_panic_text("std.net", "the network can't be used inside a `parallel for`, since what each iteration "
                                 "saw would depend on timing");
    }
}

/* How many pollers the program has opened, which numbers each. */
static cx_u32 net_count;

static CX_COLD void net_out_of_memory(void) { cx_panic_text("<memory>", "out of memory"); }

static void* net_grow(void* p, size_t* cap, size_t need, size_t size) {
    if (need <= *cap) return p;
    size_t c = *cap ? *cap : 16;
    while (c < need) c *= 2;
    void* q = realloc(p, c * size);
    if (!q) net_out_of_memory();
    *cap = c;
    return q;
}

/* Bytes waiting: the live ones are [at, len). */
typedef struct {
    cx_u8* p;
    size_t len, cap, at;
} net_buf;

static void buf_append(net_buf* b, const cx_u8* p, size_t n) {
    if (b->at && b->at == b->len) b->at = b->len = 0;
    b->p = net_grow(b->p, &b->cap, b->len + n, 1);
    memcpy(b->p + b->len, p, n);
    b->len += n;
}

static size_t buf_live(const net_buf* b) { return b->len - b->at; }

static void buf_free(net_buf* b) {
    free(b->p);
    memset(b, 0, sizeof *b);
}

typedef struct net_slot net_slot;

/* A file queued while another is being sent, from `at`, `left` bytes, with what's written after
 * it, which goes once it has. */
typedef struct net_wait {
#ifdef _WIN32
    HANDLE file;
#else
    int file;
#endif
    cx_u64 at, left;
    net_buf after;
    struct net_wait* next;
} net_wait;

#ifdef _WIN32
/* OP_FILE is a TransmitFile; OP_FREAD and OP_FSEND read a part of a file and send it, where
 * TransmitFile is limited (see post_file). */
enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CONNECT, OP_FILE, OP_FREAD, OP_FSEND, OP_URECV, OP_USEND };
#define NET_FILE_CHUNK (256 * 1024) /* bytes of a file read and sent at a time, without TransmitFile */
typedef struct {
    OVERLAPPED ov;
    net_slot* slot;
    int op;
} net_op;
#endif

struct net_slot {
    int kind, state;
    cx_u32 index;
    cx_u8 gen;
    int received;  /* a Received event is waiting in this poll */
    int dirty;     /* bytes were queued since the last flush */
    int reported;  /* a poll before this one returned its Closed event */
    net_buf in, out;
    net_buf after;  /* what was queued while a file is being sent, which goes after it */
    int has_file;   /* a file is being sent: `file`, from `file_at`, `file_left` bytes */
    cx_u64 file_at, file_left;
#ifdef _WIN32
    HANDLE file;
#else
    int file;
#endif
    struct net_wait* waiting;  /* files queued after the one being sent, first to last */
    struct net_wait* waiting_last;
    struct sockaddr_storage peer;  /* the other end, from when it opened */
    int has_peer;
    /* A UDP socket's family and whether it's IPv6 alone, and the multicast interface the
     * program asked for and the one the socket has, which a queued datagram's can differ from. */
    int ufamily, uv6only;
    cx_u32 mif_cur, mif_set;
#ifdef _WIN32
    cx_u8* ubuf;  /* where a datagram is received, NET_UDP_MAX bytes */
    struct sockaddr_storage ufrom, uto;  /* where it came from, and where the one in flight goes */
    int ufrom_len, uto_len;
    SOCKET s;
    int pending;  /* operations the system still holds */
    int sending;
    int receiving;  /* a receive is pending */
    net_op rop, sop, cop;
    net_buf fly;  /* what the send in flight is sending */
    cx_u8* fbuf;  /* the part of the file read and being sent, [fbuf_at, fbuf_len), without TransmitFile */
    DWORD fbuf_len, fbuf_at;
    int tf_turn;  /* its TransmitFile in flight counts in tf_busy */
    ULONGLONG tf_since;  /* when that TransmitFile started, in GetTickCount64's milliseconds */
    SOCKET accept_s;
    int family;
    cx_u8 abuf[2 * (sizeof(SOCKADDR_STORAGE) + 16)];
    cx_u8 rbuf[NET_RECV];
#else
    int fd;
    int writable;
#endif
};

typedef struct {
#ifdef _WIN32
    HANDLE iocp;
#else
    int ep;  /* the epoll, or on macOS the kqueue */
#endif
    net_slot** slots;
    size_t nslots, slots_cap;
    cx_u32* free_ids;
    size_t nfree, free_cap;
    cx_u32* ev; /* events for the next poll, as kind and id pairs */
    size_t nev, ev_cap;
    cx_u32* out; /* the events the last poll returned */
    size_t nout, out_cap;
    cx_u32* retiring; /* slots whose Closed the last poll returned */
    size_t nretiring, retiring_cap;
    cx_u32* dirty; /* connections with bytes queued since the last flush */
    size_t ndirty, dirty_cap;
    int woken;      /* a wake is on its way, set and cleared atomically, so wakes come as one */
    int woke;       /* this poll's events have a Woken already */
    int has_waker;  /* other threads may wake it */
    int busy;       /* a thread is in one of its calls, set and cleared atomically (see net_enter) */
    int signalled;  /* a signal woke it since `cx_net_signalled` last said so, set atomically */
    cx_u32 number;  /* which of the program's pollers it is, from 1, for telling their connections apart */
#if !defined(_WIN32) && !defined(__APPLE__)
    int wake_fd;    /* the eventfd a wake writes to, once there's a waker; macOS's is in the kqueue */
#endif
} net;

static cx_u32 slot_id(const net_slot* s) { return ((cx_u32)s->gen << NET_INDEX_BITS) | s->index; }

/* Each poller is one thread's at a time (CLX-386). Pollers share nothing but the TransmitFile
 * turns below, so each thread can have its own, polling at once with no lock between them. One
 * can move to another thread, as a value that's sent does, but two threads in its calls at once
 * would tear its slots and buffers, so each call holds it, and a second thread that finds it held
 * panics rather than race. The acquire and release also hand what one thread did to the next to
 * hold it. A waker's wake holds nothing, since any thread may wake it. */
static net* net_enter(cx_u64 h) {
    net_check_thread();
    net* n = (net*)(uintptr_t)h;
    int idle = 0;
    if (!__atomic_compare_exchange_n(&n->busy, &idle, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        cx_panic_text("std.net", "two threads used one `Net` at once: a `Net` is one thread's at a time, so each "
                                 "thread that polls needs its own, and other threads wake it with a `Waker`");
    }
    return n;
}

static void net_leave(net* n) { __atomic_store_n(&n->busy, 0, __ATOMIC_RELEASE); }

/* A new poller's number: never 0, which a connection made from an id has, for any poller. */
static void net_number(net* n) {
    do n->number = __atomic_add_fetch(&net_count, 1, __ATOMIC_RELAXED);
    while (n->number == 0);
}

#ifdef _WIN32
static void tf_release(net_slot* s);
#endif

static void emit(net* n, cx_u32 kind, net_slot* s) {
    n->ev = net_grow(n->ev, &n->ev_cap, n->nev + 2, sizeof(cx_u32));
    n->ev[n->nev++] = kind;
    n->ev[n->nev++] = slot_id(s);
}

/* A wake arrived. Clearing the flag before the program sees the event means a wake after this
 * one sends another, so none is lost: what a thread queued before waking is there to take once
 * the poll returns. */
static void woke(net* n) {
    __atomic_store_n(&n->woken, 0, __ATOMIC_SEQ_CST);
    if (n->woke) return;
    n->woke = 1;
    n->ev = net_grow(n->ev, &n->ev_cap, n->nev + 2, sizeof(cx_u32));
    n->ev[n->nev++] = EV_WOKEN;
    n->ev[n->nev++] = NET_NONE;
}

static net_slot* new_slot(net* n, int kind) {
    net_slot* s;
    if (n->nfree) {
        s = n->slots[n->free_ids[--n->nfree]];
        cx_u32 index = s->index;
        cx_u8 gen = s->gen;
        memset(s, 0, sizeof *s);
        s->index = index;
        s->gen = (cx_u8)(gen + 1 == 0xFF ? 0 : gen + 1);
    } else {
        if (n->nslots >= ((size_t)1 << NET_INDEX_BITS) - 1) return NULL;
        s = calloc(1, sizeof *s);
        if (!s) net_out_of_memory();
        n->slots = net_grow(n->slots, &n->slots_cap, n->nslots + 1, sizeof *n->slots);
        s->index = (cx_u32)n->nslots;
        n->slots[n->nslots++] = s;
    }
    s->kind = kind;
#ifdef _WIN32
    s->s = INVALID_SOCKET;
    s->accept_s = INVALID_SOCKET;
#else
    s->fd = -1;
#endif
    return s;
}

#ifdef _WIN32
static void close_file(HANDLE f) { CloseHandle(f); }
#else
static void close_file(int f) { close(f); }
#endif

/* Closes the runtime's handle of the file a connection was sending, if it was. */
static void drop_file(net_slot* s) {
    if (!s->has_file) return;
    close_file(s->file);
    s->has_file = 0;
    s->file_left = 0;
}

/* Closes the files waiting their turns, and frees what was written after them. */
static void drop_waiting(net_slot* s) {
    while (s->waiting) {
        net_wait* w = s->waiting;
        s->waiting = w->next;
        close_file(w->file);
        buf_free(&w->after);
        free(w);
    }
    s->waiting_last = NULL;
}

/* The file is sent: what was queued while it went goes next, then the next file waiting, if one
 * is, with what was written after it waiting behind that. `out` is empty then, since the file
 * only starts once it is. */
static void file_done(net_slot* s) {
    drop_file(s);
    net_buf t = s->out;
    s->out = s->after;
    s->after = t;
    s->after.len = s->after.at = 0;
    net_wait* w = s->waiting;
    if (!w) return;
    s->waiting = w->next;
    if (!s->waiting) s->waiting_last = NULL;
    s->has_file = 1;
    s->file = w->file;
    s->file_at = w->at;
    s->file_left = w->left;
    t = s->after;
    s->after = w->after;
    w->after = t;
    buf_free(&w->after);
    free(w);
}

static void release_slot(net* n, net_slot* s) {
    drop_file(s);
    drop_waiting(s);
    buf_free(&s->after);
    buf_free(&s->in);
    buf_free(&s->out);
#ifdef _WIN32
    buf_free(&s->fly);
    free(s->fbuf);
    s->fbuf = NULL;
    s->fbuf_at = s->fbuf_len = 0;
    free(s->ubuf);
    s->ubuf = NULL;
    tf_release(s);
#endif
    s->kind = SLOT_FREE;
    n->free_ids = net_grow(n->free_ids, &n->free_cap, n->nfree + 1, sizeof(cx_u32));
    n->free_ids[n->nfree++] = s->index;
}

static net_slot* conn_of(net* n, cx_u32 id) {
    cx_u32 index = id & (((cx_u32)1 << NET_INDEX_BITS) - 1);
    if (index >= n->nslots) return NULL;
    net_slot* s = n->slots[index];
    if (s->kind != SLOT_CONN || s->gen != (cx_u8)(id >> NET_INDEX_BITS) || s->reported) return NULL;
    return s;
}

/* Resolves `host`, which isn't NUL-terminated, to an address for TCP, or for UDP if `dgram`. An
 * empty host is every address. */
static struct addrinfo* resolve(const cx_u8* host, cx_usize len, cx_u16 port, int passive, int dgram) {
    char name[256], service[8];
    if (len >= sizeof name) {
        net_fail(ERR_ADDRESS, 0);
        return NULL;
    }
    /* A name or an address has only these characters, so anything else fails without asking a
       name server. ':' and '%' are for IPv6 addresses and their zones. */
    for (cx_usize i = 0; i < len; i++) {
        cx_u8 c = host[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '.' || c == '-' || c == '_' || c == ':' || c == '%';
        if (!ok) {
            net_fail(ERR_ADDRESS, 0);
            return NULL;
        }
    }
    memcpy(name, host, len);
    name[len] = 0;
    int k = 0;
    char digits[8];
    unsigned p = port;
    do {
        digits[k++] = (char)('0' + p % 10);
        p /= 10;
    } while (p);
    for (int i = 0; i < k; i++) service[i] = digits[k - 1 - i];
    service[k] = 0;
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = dgram ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_protocol = dgram ? IPPROTO_UDP : IPPROTO_TCP;
    if (passive) hints.ai_flags = AI_PASSIVE;
    struct addrinfo* res = NULL;
    if (getaddrinfo(len ? name : NULL, service, &hints, &res) != 0 || !res) {
        net_fail(ERR_ADDRESS, 0);
        return NULL;
    }
    return res;
}

/* The address to listen on. For every address it's the IPv6 one, which takes IPv4 clients too with
 * IPV6_V6ONLY off, since getaddrinfo lists 0.0.0.0 first on Linux and :: first on Windows. */
static const struct addrinfo* listen_addr(const struct addrinfo* a, int every) {
    if (every) {
        for (const struct addrinfo* p = a; p; p = p->ai_next) {
            if (p->ai_family == AF_INET6) return p;
        }
    }
    return a;
}

static cx_u16 local_port(const struct sockaddr_storage* a) {
    if (a->ss_family == AF_INET6) return ntohs(((const struct sockaddr_in6*)a)->sin6_port);
    return ntohs(((const struct sockaddr_in*)a)->sin_port);
}

#ifdef _WIN32
typedef SOCKET net_sock;
typedef int net_socklen;
#define SOCK_OF(s) ((s)->s)
#else
typedef int net_sock;
typedef socklen_t net_socklen;
#define SOCK_OF(s) ((s)->fd)
#endif

/* An address as std.net passes it, 22 bytes: the address in [0, 16), the port in [16, 18) and an
 * IPv6 scope in [18, 22), both big-endian. It gives the family, 4 or 6, or 0 if it isn't IP. An
 * IPv4-mapped IPv6 address, as an IPv4 client of a socket that takes both families has, comes
 * back as IPv4. */
static cx_u32 addr_parts(const struct sockaddr_storage* sa, cx_u8* out) {
    memset(out, 0, 22);
    if (sa->ss_family == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)sa;
        memcpy(out, &a->sin_addr, 4);
        memcpy(out + 16, &a->sin_port, 2);
        return 4;
    }
    if (sa->ss_family != AF_INET6) return 0;
    const struct sockaddr_in6* a = (const struct sockaddr_in6*)sa;
    const cx_u8* b = (const cx_u8*)&a->sin6_addr;
    static const cx_u8 mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    memcpy(out + 16, &a->sin6_port, 2);
    if (memcmp(b, mapped, 12) == 0) {
        memcpy(out, b + 12, 4);
        return 4;
    }
    memcpy(out, b, 16);
    cx_u32 scope = (cx_u32)a->sin6_scope_id;
    out[18] = (cx_u8)(scope >> 24);
    out[19] = (cx_u8)(scope >> 16);
    out[20] = (cx_u8)(scope >> 8);
    out[21] = (cx_u8)scope;
    return 6;
}

/* The other way: the address std.net gave, of `family`, for a socket of `sock_family`, which
 * takes an IPv4 address as IPv4-mapped if it's IPv6. 0 for an IPv6 address and an IPv4 socket. */
static int addr_in(const cx_u8* in, cx_u32 family, int sock_family, struct sockaddr_storage* sa, net_socklen* len) {
    memset(sa, 0, sizeof *sa);
    if (family == 4 && sock_family == AF_INET) {
        struct sockaddr_in* a = (struct sockaddr_in*)sa;
        a->sin_family = AF_INET;
        memcpy(&a->sin_addr, in, 4);
        memcpy(&a->sin_port, in + 16, 2);
        *len = (net_socklen)sizeof *a;
        return 1;
    }
    if (sock_family != AF_INET6 || (family != 4 && family != 6)) return 0;
    struct sockaddr_in6* a = (struct sockaddr_in6*)sa;
    a->sin6_family = AF_INET6;
    cx_u8* b = (cx_u8*)&a->sin6_addr;
    if (family == 4) {
        b[10] = b[11] = 0xFF;
        memcpy(b + 12, in, 4);
    } else {
        memcpy(b, in, 16);
        a->sin6_scope_id = ((cx_u32)in[18] << 24) | ((cx_u32)in[19] << 16) | ((cx_u32)in[20] << 8) | in[21];
    }
    memcpy(&a->sin6_port, in + 16, 2);
    *len = (net_socklen)sizeof *a;
    return 1;
}

/* A datagram arrived on a UDP socket: it waits, after its length and where it came from, for the
 * program to take it. One that would put more than NET_UDP_KEEP bytes in waiting is dropped, as
 * the system drops what arrives at a socket whose buffer is full. */
static void udp_arrived(net* n, net_slot* s, const cx_u8* p, size_t len, const struct sockaddr_storage* from) {
    if (buf_live(&s->in) + len + UDP_HEAD > NET_UDP_KEEP) return;
    cx_u8 head[UDP_HEAD];
    cx_u32 l = (cx_u32)len;
    memcpy(head, &l, 4);
    head[4] = (cx_u8)addr_parts(from, head + 5);
    buf_append(&s->in, head, UDP_HEAD);
    if (len) buf_append(&s->in, p, len);
    if (!s->received) {
        s->received = 1;
        emit(n, EV_DATAGRAM, s);
    }
}

/* A datagram waiting to be sent is its length, the multicast interface it was sent on, the
 * length of where it goes, that, then its bytes. */
#define UDP_OUT_HEAD 12

static void udp_queue(net_slot* s, const cx_u8* p, size_t len, const struct sockaddr_storage* to, net_socklen to_len) {
    cx_u32 head[3] = {(cx_u32)len, s->mif_cur, (cx_u32)to_len};
    buf_append(&s->out, (const cx_u8*)head, UDP_OUT_HEAD);
    buf_append(&s->out, (const cx_u8*)to, (size_t)to_len);
    if (len) buf_append(&s->out, p, len);
}

/* Moves what's left to the start of a buffer read from its front, once most of it has been. */
static void buf_compact(net_buf* b) {
    if (b->at < 65536 || b->at < b->len / 2) return;
    memmove(b->p, b->p + b->at, b->len - b->at);
    b->len -= b->at;
    b->at = 0;
}

/* Sets the interface the socket's multicast datagrams go out on, by its index, 0 for the system's
 * choice: 1 if it could. */
static int set_mif(net_sock fd, int family, int v6only, cx_u32 index) {
    if (family == AF_INET6) {
        unsigned int v = index;
        int ok = setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, (const char*)&v, sizeof v) == 0;
        if (v6only) return ok;
        /* A socket for both families sends IPv4 multicast too, where the system lets it say how. */
    }
#ifdef _WIN32
    DWORD v4 = htonl(index); /* an index is an address in 0.0.0.0/8 */
    int ok4 = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&v4, sizeof v4) == 0;
#elif defined(__APPLE__)
#ifdef IP_MULTICAST_IFINDEX
    unsigned int v4 = index;
    int ok4 = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IFINDEX, &v4, sizeof v4) == 0;
#else
    /* macOS before IP_MULTICAST_IFINDEX names the interface by an address of its own. */
    struct in_addr at;
    at.s_addr = htonl(INADDR_ANY);
    if (index) {
        struct ifaddrs* all = NULL;
        if (getifaddrs(&all) == 0) {
            for (struct ifaddrs* i = all; i; i = i->ifa_next) {
                if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && if_nametoindex(i->ifa_name) == index) {
                    at = ((struct sockaddr_in*)i->ifa_addr)->sin_addr;
                    break;
                }
            }
            freeifaddrs(all);
        }
    }
    int ok4 = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &at, sizeof at) == 0;
#endif
#else
    struct ip_mreqn m;
    memset(&m, 0, sizeof m);
    m.imr_ifindex = (int)index;
    int ok4 = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &m, sizeof m) == 0;
#endif
    return family == AF_INET6 ? 1 : ok4;
}

/* The socket's multicast interface as a datagram sent on `index` needs it. */
static void udp_mif(net_slot* s, cx_u32 index) {
    if (s->mif_set == index) return;
    if (set_mif(SOCK_OF(s), s->ufamily, s->uv6only, index)) s->mif_set = index;
}

/* Reads the datagram at the front of `out`: where it goes, its interface and its bytes. */
static const cx_u8* udp_front(net_slot* s, size_t* len, cx_u32* mif, struct sockaddr_storage* to, net_socklen* to_len) {
    cx_u32 head[3];
    const cx_u8* at = s->out.p + s->out.at;
    memcpy(head, at, UDP_OUT_HEAD);
    *len = head[0];
    *mif = head[1];
    *to_len = (net_socklen)head[2];
    memset(to, 0, sizeof *to);
    memcpy(to, at + UDP_OUT_HEAD, head[2]);
    return at + UDP_OUT_HEAD + head[2];
}

static void udp_pop(net_slot* s, size_t len, net_socklen to_len) {
    s->out.at += UDP_OUT_HEAD + (size_t)to_len + len;
    if (s->out.at >= s->out.len) s->out.at = s->out.len = 0;
    else buf_compact(&s->out);
}

/* Frees the slots whose Closed event the last poll returned, once nothing else holds them. */
static void retire(net* n) {
    size_t keep = 0;
    for (size_t i = 0; i < n->nretiring; i++) {
        net_slot* s = n->slots[n->retiring[i]];
#ifdef _WIN32
        if (s->pending) {
            n->retiring[keep++] = s->index;
            continue;
        }
#endif
        release_slot(n, s);
    }
    n->nretiring = keep;
}

/* At the start of a poll: the connections whose Closed event the last poll returned are done
 * with, since the program has had until now to read what arrived before. */
static void closed_seen(net* n) {
    for (size_t i = 0; i < n->nout; i += 2) {
        if (n->out[i] == EV_CLOSED) {
            net_slot* s = n->slots[n->out[i + 1] & (((cx_u32)1 << NET_INDEX_BITS) - 1)];
            s->reported = 1;
            n->retiring = net_grow(n->retiring, &n->retiring_cap, n->nretiring + 1, sizeof(cx_u32));
            n->retiring[n->nretiring++] = s->index;
        }
    }
    n->nout = 0;
}

/* Hands the events gathered to the program. */
static size_t deliver(net* n) {
    cx_u32* t = n->out;
    size_t tc = n->out_cap;
    n->out = n->ev;
    n->out_cap = n->ev_cap;
    n->nout = n->nev;
    n->ev = t;
    n->ev_cap = tc;
    n->nev = 0;
    n->woke = 0;
    for (size_t i = 0; i < n->nout; i += 2) {
        if (n->out[i] == EV_RECEIVED || n->out[i] == EV_DATAGRAM) {
            n->slots[n->out[i + 1] & (((cx_u32)1 << NET_INDEX_BITS) - 1)]->received = 0;
        }
    }
    return n->nout / 2;
}

#ifdef _WIN32

static LPFN_ACCEPTEX accept_ex;
static LPFN_CONNECTEX connect_ex;
static LPFN_TRANSMITFILE transmit_file;
/* Windows for desktops runs only two TransmitFile calls at once, system-wide, and queues the
 * rest, so a third connection sent a big file got nothing until one of the first two was done
 * (CLX-199). There each call sends at most 4 MiB, so a slow reader gives its turn back between
 * calls. Past two in flight, a connection still queues a TransmitFile while the calls are going
 * quickly: the last to finish took at most NET_TF_FAST_MS and finished within NET_TF_RECENT_MS,
 * as on a fast network, where waiting for a turn costs less than copying. Otherwise it reads the
 * file a part at a time and sends that, which copies each byte twice on this thread but waits
 * for no one. Server editions have no such limit. */
static int transmit_limited;
static int parts_only; /* CALX_NET_FILE_PARTS=1: always parts, to test them where TransmitFile isn't limited */
static volatile LONG tf_busy;       /* this program's TransmitFile calls in flight, where limited */
static volatile LONG tf_fast;       /* the last to finish was quick */
static volatile LONG64 tf_done_at;  /* when it finished */
#define NET_TF_TURNS 2
#define NET_TF_LIMITED_PART ((cx_u64)4 << 20)
#define NET_TF_FAST_MS 50
#define NET_TF_RECENT_MS 100
static volatile LONG wsa_ready; /* 0, 1 while starting, 2 when started */

static int net_start(void) {
    if (InterlockedCompareExchange(&wsa_ready, 1, 0) == 0) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
            InterlockedExchange(&wsa_ready, 0);
            return 0;
        }
        SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        GUID a = WSAID_ACCEPTEX, c = WSAID_CONNECTEX, t = WSAID_TRANSMITFILE;
        DWORD got;
        WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &a, sizeof a, &accept_ex, sizeof accept_ex, &got, NULL, NULL);
        WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &c, sizeof c, &connect_ex, sizeof connect_ex, &got, NULL, NULL);
        WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &t, sizeof t, &transmit_file, sizeof transmit_file, &got, NULL, NULL);
        closesocket(s);
        OSVERSIONINFOEXW v = {0};
        v.dwOSVersionInfoSize = sizeof v;
        v.wProductType = VER_NT_WORKSTATION;
        transmit_limited = VerifyVersionInfoW(&v, VER_PRODUCT_TYPE, VerSetConditionMask(0, VER_PRODUCT_TYPE, VER_EQUAL)) != 0;
        char parts[4];
        parts_only = GetEnvironmentVariableA("CALX_NET_FILE_PARTS", parts, sizeof parts) == 1 && parts[0] == '1';
        if (parts_only) transmit_limited = 1;
        InterlockedExchange(&wsa_ready, 2);
    }
    /* Acquire, so a thread that waited sees the functions the starting one found: volatile
     * orders nothing on Arm (CLX-274). */
    while (__atomic_load_n(&wsa_ready, __ATOMIC_ACQUIRE) == 1) SwitchToThread();
    return __atomic_load_n(&wsa_ready, __ATOMIC_ACQUIRE) == 2;
}

static void fail_os(void) {
    int e = WSAGetLastError();
    net_fail(e == WSAEADDRINUSE ? ERR_IN_USE : e == WSAECONNREFUSED ? ERR_REFUSED
             : e == WSAEADDRNOTAVAIL ? ERR_ADDRESS : ERR_OTHER, e);
}

static void op_reset(net_op* op, net_slot* s, int kind) {
    memset(&op->ov, 0, sizeof op->ov);
    op->slot = s;
    op->op = kind;
}

/* Whether the next part goes by TransmitFile: if one of NET_TF_TURNS is free, or the calls are
 * going quickly. The turn is given back when the call finishes, with tf_release. */
static int tf_take(net_slot* s) {
    if (parts_only) return 0;
    ULONGLONG now = GetTickCount64();
    if (InterlockedIncrement(&tf_busy) > NET_TF_TURNS &&
        !(tf_fast && now - (ULONGLONG)InterlockedCompareExchange64(&tf_done_at, 0, 0) <= NET_TF_RECENT_MS)) {
        InterlockedDecrement(&tf_busy);
        return 0;
    }
    s->tf_turn = 1;
    s->tf_since = now;
    return 1;
}

static void tf_release(net_slot* s) {
    if (!s->tf_turn) return;
    s->tf_turn = 0;
    InterlockedDecrement(&tf_busy);
}

/* A TransmitFile finished, how quickly. */
static void tf_finished(net_slot* s) {
    if (!s->tf_turn) return;
    ULONGLONG now = GetTickCount64();
    InterlockedExchange(&tf_fast, now - s->tf_since <= NET_TF_FAST_MS);
    InterlockedExchange64(&tf_done_at, (LONG64)now);
}

/* The memory for a file's parts goes once no file is left to send, so a connection that sent
 * one holds nothing for it. */
static void parts_done(net_slot* s) {
    if (s->has_file || !s->fbuf) return;
    free(s->fbuf);
    s->fbuf = NULL;
}

static void lost(net* n, net_slot* s) {
    if (s->state == CONN_CLOSED) return;
    s->state = CONN_CLOSED;
    if (s->s != INVALID_SOCKET) {
        closesocket(s->s); /* what the system holds comes back aborted */
        s->s = INVALID_SOCKET;
    }
    emit(n, EV_CLOSED, s);
}

/* Closes a connection the program closed, once what it queued is sent. Closing a socket while a
 * receive is pending resets the connection, which can throw away what was sent before the peer
 * read it, so the receive is cancelled first, and the socket closes when it comes back. */
static void finish(net* n, net_slot* s) {
    if (!s->receiving) {
        lost(n, s);
        return;
    }
    s->state = CONN_CLOSED;
    CancelIoEx((HANDLE)s->s, &s->rop.ov);
    emit(n, EV_CLOSED, s);
}

static void post_recv(net* n, net_slot* s) {
    WSABUF b = {NET_RECV, (char*)s->rbuf};
    DWORD flags = 0;
    op_reset(&s->rop, s, OP_RECV);
    if (WSARecv(s->s, &b, 1, NULL, &flags, &s->rop.ov, NULL) == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        lost(n, s);
        return;
    }
    s->pending++;
    s->receiving = 1;
}

/* The next part of the file being sent, at its offset: with TransmitFile, at most 1 GiB at a
 * time, or 4 MiB where it's limited. Where it's limited and both turns are taken, or a part read
 * is still being sent, the rest of that part is sent, or the next part read, NET_FILE_CHUNK
 * bytes, into `fbuf`. The file stays the one being sent until its last part has gone, so what's
 * written after it still waits. */
static void post_file(net* n, net_slot* s) {
    if (transmit_limited && (s->fbuf_at < s->fbuf_len || !tf_take(s))) {
        if (s->fbuf_at < s->fbuf_len) {
            WSABUF b = {s->fbuf_len - s->fbuf_at, (char*)s->fbuf + s->fbuf_at};
            op_reset(&s->sop, s, OP_FSEND);
            if (WSASend(s->s, &b, 1, NULL, 0, &s->sop.ov, NULL) == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                lost(n, s);
                return;
            }
        } else {
            if (!s->fbuf) {
                s->fbuf = malloc(NET_FILE_CHUNK);
                if (!s->fbuf) net_out_of_memory();
            }
            DWORD part = s->file_left > NET_FILE_CHUNK ? NET_FILE_CHUNK : (DWORD)s->file_left;
            op_reset(&s->sop, s, OP_FREAD);
            s->sop.ov.Offset = (DWORD)s->file_at;
            s->sop.ov.OffsetHigh = (DWORD)(s->file_at >> 32);
            if (!ReadFile(s->file, s->fbuf, part, NULL, &s->sop.ov) && GetLastError() != ERROR_IO_PENDING) {
                lost(n, s);
                return;
            }
        }
        s->pending++;
        s->sending = 1;
        return;
    }
    cx_u64 most = transmit_limited ? NET_TF_LIMITED_PART : (cx_u64)1 << 30;
    DWORD want = (DWORD)(s->file_left > most ? most : s->file_left);
    op_reset(&s->sop, s, OP_FILE);
    s->sop.ov.Offset = (DWORD)s->file_at;
    s->sop.ov.OffsetHigh = (DWORD)(s->file_at >> 32);
    if (!transmit_file(s->s, s->file, want, 0, &s->sop.ov, NULL, 0) && WSAGetLastError() != WSA_IO_PENDING) {
        tf_release(s);
        lost(n, s);
        return;
    }
    s->pending++;
    s->sending = 1;
}

static void post_send(net* n, net_slot* s) {
    if (s->sending || s->state == CONN_CONNECTING || s->state == CONN_CLOSED) return;
    if (!buf_live(&s->fly)) {
        if (!buf_live(&s->out)) {
            if (s->has_file) post_file(n, s);
            return;
        }
        net_buf t = s->fly;
        s->fly = s->out;
        s->out = t;
        s->out.len = s->out.at = 0;
    }
    WSABUF b = {(ULONG)buf_live(&s->fly), (char*)s->fly.p + s->fly.at};
    op_reset(&s->sop, s, OP_SEND);
    if (WSASend(s->s, &b, 1, NULL, 0, &s->sop.ov, NULL) == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        lost(n, s);
        return;
    }
    s->pending++;
    s->sending = 1;
}

static void opened(net* n, net_slot* s) {
    BOOL yes = TRUE;
    setsockopt(s->s, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof yes);
    int len = (int)sizeof s->peer;
    s->has_peer = getpeername(s->s, (struct sockaddr*)&s->peer, &len) == 0;
    s->state = CONN_OPEN;
    post_recv(n, s);
    post_send(n, s);
}

static void post_accept(net* n, net_slot* l) {
    l->accept_s = WSASocketW(l->family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (l->accept_s == INVALID_SOCKET) return;
    op_reset(&l->cop, l, OP_ACCEPT);
    DWORD got;
    int size = sizeof(SOCKADDR_STORAGE) + 16;
    if (!accept_ex(l->s, l->accept_s, l->abuf, 0, size, size, &got, &l->cop.ov) && WSAGetLastError() != ERROR_IO_PENDING) {
        closesocket(l->accept_s);
        l->accept_s = INVALID_SOCKET;
        return;
    }
    l->pending++;
    (void)n;
}

/* Keeps a receive pending on a UDP socket, into `ubuf`. A receive that fails at once, as one can
 * after an ICMP error the system still reports, is tried again a few times. */
static void post_udp_recv(net* n, net_slot* s) {
    (void)n;
    for (int tries = 0; tries < 8 && s->state != CONN_CLOSED; tries++) {
        WSABUF b = {NET_UDP_MAX, (char*)s->ubuf};
        DWORD flags = 0;
        s->ufrom_len = (int)sizeof s->ufrom;
        op_reset(&s->rop, s, OP_URECV);
        if (WSARecvFrom(s->s, &b, 1, NULL, &flags, (struct sockaddr*)&s->ufrom, &s->ufrom_len, &s->rop.ov, NULL) == 0 ||
            WSAGetLastError() == WSA_IO_PENDING) {
            s->pending++;
            s->receiving = 1;
            return;
        }
    }
}

/* Sends the datagram at the front of what waits, overlapped, one at a time, each on its own
 * multicast interface. One the system refuses at once is dropped, as one it can't deliver is. */
static void udp_send_queued(net* n, net_slot* s) {
    (void)n;
    while (!s->sending && s->state != CONN_CLOSED && buf_live(&s->out)) {
        size_t len;
        cx_u32 mif;
        const cx_u8* p = udp_front(s, &len, &mif, &s->uto, &s->uto_len);
        s->fly.at = s->fly.len = 0;
        if (len) buf_append(&s->fly, p, len);
        udp_pop(s, len, s->uto_len);
        udp_mif(s, mif);
        WSABUF b = {(ULONG)len, (char*)s->fly.p};
        op_reset(&s->sop, s, OP_USEND);
        if (WSASendTo(s->s, &b, 1, NULL, 0, (struct sockaddr*)&s->uto, s->uto_len, &s->sop.ov, NULL) == 0 ||
            WSAGetLastError() == WSA_IO_PENDING) {
            s->pending++;
            s->sending = 1;
        }
    }
    if (!buf_live(&s->out)) s->out.at = s->out.len = 0;
}

static void completed(net* n, OVERLAPPED_ENTRY* e) {
    net_op* op = (net_op*)e->lpOverlapped;
    net_slot* s = op->slot;
    DWORD bytes = e->dwNumberOfBytesTransferred;
    int ok = op->ov.Internal == 0;
    s->pending--;
    switch (op->op) {
        case OP_ACCEPT: {
            SOCKET a = s->accept_s;
            s->accept_s = INVALID_SOCKET;
            if (!ok) {
                closesocket(a);
            } else {
                setsockopt(a, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char*)&s->s, sizeof s->s);
                net_slot* c = new_slot(n, SLOT_CONN);
                if (!c || !CreateIoCompletionPort((HANDLE)a, n->iocp, 0, 0)) {
                    closesocket(a);
                } else {
                    c->s = a;
                    emit(n, EV_ACCEPTED, c);
                    opened(n, c);
                }
            }
            post_accept(n, s);
            break;
        }
        case OP_CONNECT:
            if (s->state == CONN_CLOSED) break;
            if (!ok) {
                lost(n, s);
                break;
            }
            setsockopt(s->s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
            emit(n, EV_CONNECTED, s);
            opened(n, s);
            break;
        case OP_RECV:
            s->receiving = 0;
            if (s->state == CONN_CLOSED) {
                /* The receive `finish` cancelled: now the socket closes gracefully. */
                if (s->s != INVALID_SOCKET) {
                    shutdown(s->s, SD_SEND);
                    closesocket(s->s);
                    s->s = INVALID_SOCKET;
                }
                break;
            }
            if (!ok || bytes == 0) {
                lost(n, s);
                break;
            }
            buf_append(&s->in, s->rbuf, bytes);
            if (!s->received) {
                s->received = 1;
                emit(n, EV_RECEIVED, s);
            }
            post_recv(n, s);
            break;
        case OP_SEND:
            s->sending = 0;
            if (s->state == CONN_CLOSED) break;
            if (!ok) {
                lost(n, s);
                break;
            }
            s->fly.at += bytes;
            if (s->fly.at >= s->fly.len) s->fly.at = s->fly.len = 0;
            post_send(n, s);
            if (s->state == CONN_CLOSING && !s->sending) finish(n, s);
            break;
        case OP_FILE:
            s->sending = 0;
            if (ok && bytes) tf_finished(s);
            tf_release(s);
            if (s->state == CONN_CLOSED) break;
            /* A file shorter than the range would leave the other end waiting for the rest. */
            if (!ok || bytes == 0) {
                lost(n, s);
                break;
            }
            s->file_at += bytes;
            s->file_left -= bytes < s->file_left ? bytes : s->file_left;
            if (!s->file_left) {
                file_done(s);
                parts_done(s);
            }
            post_send(n, s);
            if (s->state == CONN_CLOSING && !s->sending) finish(n, s);
            break;
        case OP_FREAD:
            s->sending = 0;
            if (s->state == CONN_CLOSED) break;
            /* A file shorter than the range, as with TransmitFile. */
            if (!ok || bytes == 0) {
                lost(n, s);
                break;
            }
            s->fbuf_at = 0;
            s->fbuf_len = bytes;
            s->file_at += bytes;
            s->file_left -= bytes < s->file_left ? bytes : s->file_left;
            post_file(n, s);
            break;
        case OP_FSEND:
            s->sending = 0;
            if (s->state == CONN_CLOSED) break;
            if (!ok) {
                lost(n, s);
                break;
            }
            s->fbuf_at += bytes;
            if (s->fbuf_at >= s->fbuf_len) {
                s->fbuf_at = s->fbuf_len = 0;
                if (!s->file_left) {
                    file_done(s);
                    parts_done(s);
                }
            }
            post_send(n, s);
            if (s->state == CONN_CLOSING && !s->sending) finish(n, s);
            break;
        case OP_URECV:
            s->receiving = 0;
            if (s->state == CONN_CLOSED) break;
            /* A datagram cut short, too big for the buffer, comes back as an error, and is dropped. */
            if (ok) udp_arrived(n, s, s->ubuf, bytes, &s->ufrom);
            post_udp_recv(n, s);
            break;
        case OP_USEND:
            s->sending = 0;
            if (s->state == CONN_CLOSED) break;
            udp_send_queued(n, s);
            break;
    }
}

/* Waits until something the program is told of happens, or the time is up. Some completions are
 * the runtime's own, such as a send or a cancelled receive finishing, and give no event, so
 * those don't end the wait. */
static void wait_events(net* n, cx_i64 timeout_ms) {
    ULONGLONG deadline = timeout_ms > 0 ? GetTickCount64() + (ULONGLONG)timeout_ms : 0;
    DWORD wait = timeout_ms < 0 ? INFINITE : timeout_ms > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)timeout_ms;
    for (;;) {
        OVERLAPPED_ENTRY entries[64];
        ULONG got = 0;
        if (GetQueuedCompletionStatusEx(n->iocp, entries, 64, &got, wait, FALSE)) {
            /* A wake is posted with no operation. */
            for (ULONG i = 0; i < got; i++) {
                if (entries[i].lpOverlapped) completed(n, &entries[i]);
                else woke(n);
            }
            if (got == 64) {
                /* There may be more already. */
                wait = 0;
                continue;
            }
        } else if (GetLastError() != WAIT_TIMEOUT) {
            return;
        }
        if (n->nev || timeout_ms == 0) return;
        if (timeout_ms > 0) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) return;
            wait = (DWORD)(deadline - now);
        } else {
            wait = INFINITE;
        }
    }
}

cx_u64 cx_net_open(void) {
    net_check_thread();
    if (!net_start()) {
        net_fail(ERR_OTHER, WSAGetLastError());
        return 0;
    }
    net* n = calloc(1, sizeof *n);
    if (!n) net_out_of_memory();
    net_number(n);
    n->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!n->iocp) {
        net_fail(ERR_OTHER, (cx_i32)GetLastError());
        free(n);
        return 0;
    }
    return (cx_u64)(uintptr_t)n;
}

static cx_i32 net_listen(net* n, const cx_u8* host, cx_usize len, cx_u16 port) {
    struct addrinfo* a = resolve(host, len, port, 1, 0);
    if (!a) return -1;
    const struct addrinfo* b = listen_addr(a, len == 0);
    SOCKET s = WSASocketW(b->ai_family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET && b != a) {
        /* No IPv6 on this machine: every IPv4 address, then. */
        b = a;
        s = WSASocketW(b->ai_family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    }
    if (s == INVALID_SOCKET) {
        fail_os();
        freeaddrinfo(a);
        return -1;
    }
    BOOL yes = TRUE, no = FALSE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&yes, sizeof yes);
    /* An IPv6 listener takes IPv4 clients too, as it does on Linux. */
    if (b->ai_family == AF_INET6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&no, sizeof no);
    int family = b->ai_family;
    if (bind(s, b->ai_addr, (int)b->ai_addrlen) != 0 || listen(s, SOMAXCONN) != 0 ||
        !CreateIoCompletionPort((HANDLE)s, n->iocp, 0, 0)) {
        fail_os();
        closesocket(s);
        freeaddrinfo(a);
        return -1;
    }
    freeaddrinfo(a);
    struct sockaddr_storage at;
    int at_len = sizeof at;
    getsockname(s, (struct sockaddr*)&at, &at_len);
    net_slot* l = new_slot(n, SLOT_LISTENER);
    if (!l) {
        net_fail(ERR_OTHER, 0);
        closesocket(s);
        return -1;
    }
    l->s = s;
    l->family = family;
    post_accept(n, l);
    return local_port(&at);
}

static cx_u32 net_connect(net* n, const cx_u8* host, cx_usize len, cx_u16 port) {
    struct addrinfo* a = resolve(host, len, port, 0, 0);
    if (!a) return NET_NONE;
    SOCKET s = WSASocketW(a->ai_family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    struct sockaddr_storage any;
    memset(&any, 0, sizeof any);
    any.ss_family = (ADDRESS_FAMILY)a->ai_family;
    int any_len = a->ai_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr*)&any, any_len) != 0 ||
        !CreateIoCompletionPort((HANDLE)s, n->iocp, 0, 0)) {
        fail_os();
        if (s != INVALID_SOCKET) closesocket(s);
        freeaddrinfo(a);
        return NET_NONE;
    }
    net_slot* c = new_slot(n, SLOT_CONN);
    if (!c) {
        net_fail(ERR_OTHER, 0);
        closesocket(s);
        freeaddrinfo(a);
        return NET_NONE;
    }
    c->s = s;
    c->state = CONN_CONNECTING;
    op_reset(&c->cop, c, OP_CONNECT);
    if (!connect_ex(s, a->ai_addr, (int)a->ai_addrlen, NULL, 0, NULL, &c->cop.ov) && WSAGetLastError() != ERROR_IO_PENDING) {
        freeaddrinfo(a);
        lost(n, c);
        return slot_id(c);
    }
    freeaddrinfo(a);
    c->pending++;
    return slot_id(c);
}

static void close_now(net_slot* s) {
    if (s->s != INVALID_SOCKET) closesocket(s->s);
    s->s = INVALID_SOCKET;
    if (s->accept_s != INVALID_SOCKET) closesocket(s->accept_s);
    s->accept_s = INVALID_SOCKET;
}

static void close_all(net* n) {
    for (size_t i = 0; i < n->nslots; i++) close_now(n->slots[i]);
    /* Wait for the system to give back what it holds, so it writes into nothing freed. */
    for (;;) {
        int held = 0;
        for (size_t i = 0; i < n->nslots; i++) held += n->slots[i]->pending;
        if (!held) break;
        OVERLAPPED_ENTRY entries[64];
        ULONG got = 0;
        if (!GetQueuedCompletionStatusEx(n->iocp, entries, 64, &got, 1000, FALSE)) break;
        for (ULONG i = 0; i < got; i++) {
            net_op* op = (net_op*)entries[i].lpOverlapped;
            if (!op) continue;
            if (op->op == OP_FILE) tf_release(op->slot);
            op->slot->pending--;
        }
    }
    CloseHandle(n->iocp);
}

#else

static void fail_os(void) {
    int e = errno;
    net_fail(e == EADDRINUSE ? ERR_IN_USE : e == ECONNREFUSED ? ERR_REFUSED : e == EADDRNOTAVAIL ? ERR_ADDRESS : ERR_OTHER, e);
}

static void lost(net* n, net_slot* s) {
    if (s->state == CONN_CLOSED) return;
    s->state = CONN_CLOSED;
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    emit(n, EV_CLOSED, s);
}

/* sendfile, giving EPIPE where the other end has gone rather than SIGPIPE, which would end the
 * program without a word: send's MSG_NOSIGNAL has no sendfile form, so the signal is blocked
 * around it, and taken if the call raised it, as calx_proc.c's write_quiet does. sendfile sends
 * in pieces, and one that sent some before the other end went gives back what it sent with the
 * signal raised all the same, so a short count is looked at too. */
static ssize_t sendfile_quiet(int out, int in, off_t* at, size_t n) {
#ifdef __APPLE__
    /* macOS's sendfile takes the file first, and says in `len` what it sent even when it stops
     * early with an error, such as EAGAIN once the socket is full. Each socket's SO_NOSIGPIPE keeps
     * SIGPIPE away, and macOS has no sigtimedwait for the dance below. */
    off_t len = (off_t)n;
    int rc = sendfile(in, out, *at, &len, NULL, 0);
    if (len > 0) {
        *at += len;
        return (ssize_t)len;
    }
    return rc == 0 ? 0 : -1;
#else
    sigset_t pipe_set, old, pending;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old);
    sigpending(&pending);
    int was = sigismember(&pending, SIGPIPE);
    ssize_t r = sendfile(out, in, at, n);
    int e = errno;
    if (!was && (r < 0 ? e == EPIPE : (size_t)r < n)) {
        struct timespec zero = {0, 0};
        while (sigtimedwait(&pipe_set, NULL, &zero) < 0 && errno == EINTR) {
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = e;
    return r;
#endif
}

static void flush(net* n, net_slot* s) {
    if (s->state == CONN_CONNECTING || s->state == CONN_CLOSED) return;
    for (;;) {
        while (s->writable && buf_live(&s->out)) {
            ssize_t r = send(s->fd, s->out.p + s->out.at, buf_live(&s->out), MSG_NOSIGNAL);
            if (r > 0) {
                s->out.at += (size_t)r;
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                s->writable = 0;
            } else if (r < 0 && errno == EINTR) {
                continue;
            } else {
                lost(n, s);
                return;
            }
        }
        if (buf_live(&s->out) || !s->has_file) break;
        s->out.at = s->out.len = 0;
        while (s->writable && s->file_left) {
            off_t at = (off_t)s->file_at;
            size_t want = s->file_left > ((cx_u64)1 << 30) ? (size_t)1 << 30 : (size_t)s->file_left;
            ssize_t r = sendfile_quiet(s->fd, s->file, &at, want);
            if (r > 0) {
                s->file_at += (cx_u64)r;
                s->file_left -= (cx_u64)r;
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                s->writable = 0;
            } else if (r < 0 && errno == EINTR) {
                continue;
            } else {
                /* An error, or a file shorter than the range, which would leave the other end
                 * waiting for the rest. */
                lost(n, s);
                return;
            }
        }
        if (s->file_left) break;
        file_done(s);
    }
    if (!buf_live(&s->out) && !s->has_file) {
        s->out.at = s->out.len = 0;
        if (s->state == CONN_CLOSING) lost(n, s);
    }
}

/* What a socket is ready for, as `ready` takes it from epoll's or kqueue's events: bytes or the
 * end to read, room to write, or an error or hang-up. */
enum { RD_IN = 1, RD_OUT = 2, RD_ERR = 4 };

/* Watches the socket, edge-triggered: a listener for connections, a connection for reads and
 * writes. */
static int watch(net* n, net_slot* s, int fd, int conn) {
#ifdef __APPLE__
    struct kevent e[2];
    EV_SET(&e[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, (void*)(uintptr_t)s->index);
    EV_SET(&e[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, (void*)(uintptr_t)s->index);
    return kevent(n->ep, e, conn ? 2 : 1, NULL, 0, NULL);
#else
    struct epoll_event e;
    memset(&e, 0, sizeof e);
    e.events = conn ? EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET : EPOLLIN | EPOLLET;
    e.data.u32 = s->index;
    return epoll_ctl(n->ep, EPOLL_CTL_ADD, fd, &e);
#endif
}

#ifdef __APPLE__
/* A socket made nonblocking and close-on-exec, as Linux's SOCK_NONBLOCK and SOCK_CLOEXEC make it,
 * which macOS has neither of, and with SO_NOSIGPIPE, as it has no MSG_NOSIGNAL. -1, with the
 * socket closed, if it can't be. */
static int own_socket(int fd) {
    if (fd < 0) return -1;
    int yes = 1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}
#endif

static int new_socket(int family) {
#ifdef __APPLE__
    return own_socket(socket(family, SOCK_STREAM, IPPROTO_TCP));
#else
    return socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
#endif
}

static void opened(net* n, net_slot* s) {
    int yes = 1;
    setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
    socklen_t len = sizeof s->peer;
    s->has_peer = getpeername(s->fd, (struct sockaddr*)&s->peer, &len) == 0;
    s->state = CONN_OPEN;
    s->writable = 1;
    flush(n, s);
}

static void readable(net* n, net_slot* s) {
    int got = 0, end = 0;
    for (;;) {
        s->in.p = net_grow(s->in.p, &s->in.cap, s->in.len + NET_RECV, 1);
        ssize_t r = recv(s->fd, s->in.p + s->in.len, s->in.cap - s->in.len, 0);
        if (r > 0) {
            s->in.len += (size_t)r;
            got = 1;
        } else if (r == 0) {
            end = 1;
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            end = errno != EAGAIN && errno != EWOULDBLOCK;
            break;
        }
    }
    if (got && !s->received) {
        s->received = 1;
        emit(n, EV_RECEIVED, s);
    }
    if (end) lost(n, s);
}

static void accept_all(net* n, net_slot* l) {
    for (;;) {
#ifdef __APPLE__
        int fd = accept(l->fd, NULL, NULL);
        if (fd >= 0 && own_socket(fd) < 0) continue;
#else
        int fd = accept4(l->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return;
        }
        net_slot* c = new_slot(n, SLOT_CONN);
        if (!c) {
            close(fd);
            continue;
        }
        c->fd = fd;
        if (watch(n, c, fd, 1) != 0) {
            close(fd);
            c->fd = -1;
            release_slot(n, c);
            continue;
        }
        emit(n, EV_ACCEPTED, c);
        opened(n, c);
    }
}

/* Sends the datagrams that waited for room, each on its own multicast interface, while the socket
 * takes them. One the system refuses is dropped, as one it can't deliver is. */
static void udp_flush(net_slot* s) {
    while (s->writable && s->state != CONN_CLOSED && buf_live(&s->out)) {
        size_t len;
        cx_u32 mif;
        struct sockaddr_storage to;
        net_socklen to_len;
        const cx_u8* p = udp_front(s, &len, &mif, &to, &to_len);
        udp_mif(s, mif);
        ssize_t r = sendto(s->fd, p, len, 0, (struct sockaddr*)&to, to_len);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
            s->writable = 0;
            break;
        }
        udp_pop(s, len, to_len);
    }
    if (!buf_live(&s->out)) s->out.at = s->out.len = 0;
}

/* Takes every datagram that has arrived on a UDP socket. */
static void udp_readable(net* n, net_slot* s) {
    static _Thread_local cx_u8* buf;
    if (!buf) {
        buf = malloc(NET_UDP_MAX);
        if (!buf) net_out_of_memory();
    }
    /* Edge-triggered, so until the system has none left. */
    int errors = 0;
    while (errors < 16) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof from;
        ssize_t r = recvfrom(s->fd, buf, NET_UDP_MAX, 0, (struct sockaddr*)&from, &from_len);
        if (r >= 0) {
            udp_arrived(n, s, buf, (size_t)r, &from);
            errors = 0;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else if (errno != EINTR) {
            /* One a datagram sent earlier brought back, such as an ICMP unreachable, which reading
             * reports once: the socket goes on. */
            errors++;
        }
    }
}

static void ready(net* n, net_slot* s, cx_u32 e) {
    if (s->kind == SLOT_LISTENER) {
        accept_all(n, s);
        return;
    }
    if (s->kind == SLOT_UDP) {
        if (s->state == CONN_CLOSED) return;
        if (e & (RD_IN | RD_ERR)) udp_readable(n, s);
        if (e & RD_OUT) {
            s->writable = 1;
            udp_flush(s);
        }
        return;
    }
    if (s->state == CONN_CLOSED) return;
    if (s->state == CONN_CONNECTING) {
        if (!(e & (RD_OUT | RD_ERR))) return;
        int err = 0;
        socklen_t len = sizeof err;
        if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            lost(n, s);
            return;
        }
        emit(n, EV_CONNECTED, s);
        opened(n, s);
    }
    if (e & (RD_IN | RD_ERR)) readable(n, s);
    if (s->state != CONN_CLOSED && (e & RD_OUT)) {
        s->writable = 1;
        flush(n, s);
    }
}

static cx_i64 monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (cx_i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Waits until something the program is told of happens, or the time is up. Some readiness is the
 * runtime's own, such as a socket taking more of what's queued, and gives no event, so that
 * doesn't end the wait. */
static void wait_events(net* n, cx_i64 timeout_ms) {
    cx_i64 deadline = timeout_ms > 0 ? monotonic_ms() + timeout_ms : 0;
    int wait = timeout_ms < 0 ? -1 : timeout_ms > 0x7FFFFFFF ? 0x7FFFFFFF : (int)timeout_ms;
    for (;;) {
#ifdef __APPLE__
        struct kevent evs[64];
        struct timespec ts = {wait / 1000, (long)(wait % 1000) * 1000000};
        int got = kevent(n->ep, NULL, 0, evs, 64, wait < 0 ? NULL : &ts);
        if (got < 0 && errno != EINTR) return;
        if (got > 0) {
            for (int i = 0; i < got; i++) {
                if (evs[i].filter == EVFILT_USER) {
                    woke(n);  /* EV_CLEAR set it back */
                    continue;
                }
                /* The end, or an error, reads as readable, which finds it, as epoll's RDHUP does. */
                cx_u32 e = evs[i].filter == EVFILT_WRITE ? RD_OUT : RD_IN;
                if (evs[i].flags & (EV_EOF | EV_ERROR)) e |= RD_IN | RD_ERR;
                ready(n, n->slots[(uintptr_t)evs[i].udata], e);
            }
#else
        struct epoll_event evs[64];
        int got = epoll_wait(n->ep, evs, 64, wait);
        if (got < 0 && errno != EINTR) return;
        if (got > 0) {
            for (int i = 0; i < got; i++) {
                if (evs[i].data.u32 != NET_NONE) {
                    cx_u32 k = evs[i].events, e = 0;
                    if (k & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) e |= RD_IN;
                    if (k & EPOLLOUT) e |= RD_OUT;
                    if (k & (EPOLLHUP | EPOLLERR)) e |= RD_ERR;
                    ready(n, n->slots[evs[i].data.u32], e);
                    continue;
                }
                /* The eventfd: reading it sets it back to 0. */
                cx_u64 count;
                ssize_t r = read(n->wake_fd, &count, sizeof count);
                (void)r;
                woke(n);
            }
#endif
            if (got == 64) {
                /* There may be more already. */
                wait = 0;
                continue;
            }
        }
        if (n->nev || timeout_ms == 0) return;
        if (timeout_ms > 0) {
            cx_i64 left = deadline - monotonic_ms();
            if (left <= 0) return;
            wait = left > 0x7FFFFFFF ? 0x7FFFFFFF : (int)left;
        } else {
            wait = -1;
        }
    }
}

cx_u64 cx_net_open(void) {
    net_check_thread();
    net* n = calloc(1, sizeof *n);
    if (!n) net_out_of_memory();
    net_number(n);
#ifdef __APPLE__
    n->ep = kqueue();  /* never inherited by a child, so no close-on-exec needed */
#else
    n->ep = epoll_create1(EPOLL_CLOEXEC);
#endif
    if (n->ep < 0) {
        fail_os();
        free(n);
        return 0;
    }
    return (cx_u64)(uintptr_t)n;
}

static cx_i32 net_listen(net* n, const cx_u8* host, cx_usize len, cx_u16 port) {
    struct addrinfo* a = resolve(host, len, port, 1, 0);
    if (!a) return -1;
    const struct addrinfo* b = listen_addr(a, len == 0);
    int fd = new_socket(b->ai_family);
    if (fd < 0 && b != a) {
        /* No IPv6 on this machine: every IPv4 address, then. */
        b = a;
        fd = new_socket(b->ai_family);
    }
    int yes = 1, no = 0;
    if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    if (fd >= 0 && b->ai_family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof no);
    if (fd < 0 || bind(fd, b->ai_addr, b->ai_addrlen) != 0 || listen(fd, SOMAXCONN) != 0) {
        fail_os();
        if (fd >= 0) close(fd);
        freeaddrinfo(a);
        return -1;
    }
    freeaddrinfo(a);
    struct sockaddr_storage at;
    socklen_t at_len = sizeof at;
    getsockname(fd, (struct sockaddr*)&at, &at_len);
    net_slot* l = new_slot(n, SLOT_LISTENER);
    if (!l || watch(n, l, fd, 0) != 0) {
        fail_os();
        close(fd);
        if (l) release_slot(n, l);
        return -1;
    }
    l->fd = fd;
    return local_port(&at);
}

static cx_u32 net_connect(net* n, const cx_u8* host, cx_usize len, cx_u16 port) {
    struct addrinfo* a = resolve(host, len, port, 0, 0);
    if (!a) return NET_NONE;
    int fd = new_socket(a->ai_family);
    if (fd < 0) {
        fail_os();
        freeaddrinfo(a);
        return NET_NONE;
    }
    net_slot* c = new_slot(n, SLOT_CONN);
    if (!c) {
        net_fail(ERR_OTHER, 0);
        close(fd);
        freeaddrinfo(a);
        return NET_NONE;
    }
    c->fd = fd;
    c->state = CONN_CONNECTING;
    int r = connect(fd, a->ai_addr, a->ai_addrlen);
    freeaddrinfo(a);
    if (watch(n, c, fd, 1) != 0 || (r != 0 && errno != EINPROGRESS)) {
        lost(n, c);
    }
    return slot_id(c);
}

static void close_all(net* n) {
    for (size_t i = 0; i < n->nslots; i++) {
        if (n->slots[i]->fd >= 0) close(n->slots[i]->fd);
    }
#ifndef __APPLE__
    if (n->has_waker) close(n->wake_fd);
#endif
    close(n->ep);
}

#endif

/* Hands the system what was queued on each connection since the last flush. */
static void send_dirty(net* n) {
    for (size_t i = 0; i < n->ndirty; i++) {
        net_slot* s = n->slots[n->dirty[i]];
        s->dirty = 0;
        if (s->kind != SLOT_CONN) continue;
#ifdef _WIN32
        post_send(n, s);
#else
        flush(n, s);
#endif
    }
    n->ndirty = 0;
}

/* Closes every socket, and frees the poller. */
void cx_net_free(cx_u64 h) {
    net* n = net_enter(h);
    close_all(n);
    for (size_t i = 0; i < n->nslots; i++) {
        net_slot* s = n->slots[i];
        drop_file(s);
        drop_waiting(s);
        buf_free(&s->after);
        buf_free(&s->in);
        buf_free(&s->out);
#ifdef _WIN32
        buf_free(&s->fly);
        free(s->fbuf);
        free(s->ubuf);
        tf_release(s);
#endif
        free(s);
    }
    free(n->slots);
    free(n->free_ids);
    free(n->ev);
    free(n->out);
    free(n->retiring);
    free(n->dirty);
    free(n);
}

cx_usize cx_net_poll(cx_u64 h, cx_i64 timeout_ms) {
    net* n = net_enter(h);
    send_dirty(n);
    closed_seen(n);
    retire(n);
    wait_events(n, n->nev ? 0 : timeout_ms);
    size_t got = deliver(n);
    net_leave(n);
    return got;
}

/* What the last poll returned, which the thread that polled reads straight after. */
cx_u32 cx_net_event_kind(cx_u64 h, cx_usize i) { return ((net*)(uintptr_t)h)->out[2 * i]; }
cx_u32 cx_net_event_conn(cx_u64 h, cx_usize i) { return ((net*)(uintptr_t)h)->out[2 * i + 1]; }

/* The poller's number, which std.net puts in each `Conn` it gives, so another poller's is told
 * apart. It never changes, so any thread may ask. */
cx_u32 cx_net_number(cx_u64 h) { return ((net*)(uintptr_t)h)->number; }

cx_usize cx_net_available(cx_u64 h, cx_u32 id) {
    net* n = net_enter(h);
    net_slot* s = conn_of(n, id);
    cx_usize k = s ? buf_live(&s->in) : 0;
    net_leave(n);
    return k;
}

cx_usize cx_net_read(cx_u64 h, cx_u32 id, cx_u8* to, cx_usize max) {
    net* n = net_enter(h);
    net_slot* s = conn_of(n, id);
    size_t k = 0;
    if (s) {
        k = buf_live(&s->in);
        if (k > max) k = max;
        memcpy(to, s->in.p + s->in.at, k);
        s->in.at += k;
        if (s->in.at == s->in.len) s->in.at = s->in.len = 0;
    }
    net_leave(n);
    return k;
}

/* What was queued on a connection that the system hasn't taken yet: on Windows, with what the
 * send in flight still holds. */
static cx_usize queued(const net_slot* s) {
    if (!s) return 0;
    cx_u64 left = buf_live(&s->out) + buf_live(&s->after) + s->file_left;
    for (const net_wait* w = s->waiting; w; w = w->next) left += w->left + buf_live(&w->after);
#ifdef _WIN32
    left += buf_live(&s->fly) + (s->fbuf_len - s->fbuf_at);
#endif
    return (cx_usize)left;
}

cx_usize cx_net_queued(cx_u64 h, cx_u32 id) {
    net* n = net_enter(h);
    cx_usize left = queued(conn_of(n, id));
    net_leave(n);
    return left;
}

void cx_net_send(cx_u64 h, cx_u32 id, const cx_u8* p, cx_usize len) {
    net* n = net_enter(h);
    net_slot* s = conn_of(n, id);
    if (s && len != 0 && s->state != CONN_CLOSED && s->state != CONN_CLOSING) {
        buf_append(s->waiting_last ? &s->waiting_last->after : s->has_file ? &s->after : &s->out, p, len);
        if (!s->dirty) {
            s->dirty = 1;
            n->dirty = net_grow(n->dirty, &n->dirty_cap, n->ndirty + 1, sizeof(cx_u32));
            n->dirty[n->ndirty++] = s->index;
        }
    }
    net_leave(n);
}

/* Queues `n` bytes of the open file `file`, from `at`, after what's queued, files too: 0 if the
 * connection is closing or closed, or the file can't be opened again. The runtime's handle is its
 * own, so the program may close its own at once. */
static cx_bool send_file(net* n, cx_u32 id, cx_i64 file, cx_u64 at, cx_u64 len) {
    net_slot* s = conn_of(n, id);
    if (!s || s->state == CONN_CLOSED || s->state == CONN_CLOSING) return 0;
    if (len == 0) return 1;
#ifdef _WIN32
    if (!transmit_file) return 0;
    /* The file opened again, not its handle copied: a copy shares the program's file object,
     * whose position TransmitFile reads from, so two connections sent the same file at once, or
     * the program reading it meanwhile, cut each other short at the end of the file. Opened
     * overlapped, it has no position, and the offset given is the one used. */
    HANDLE copy = ReOpenFile((HANDLE)(intptr_t)file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN);
    if (copy == INVALID_HANDLE_VALUE) return 0;
    /* Where its parts may be read, those reads finish on the connections' completion port. */
    if (transmit_limited && !CreateIoCompletionPort(copy, n->iocp, 0, 0)) {
        CloseHandle(copy);
        return 0;
    }
#else
    int copy = dup((int)file);
    if (copy < 0) return 0;
#endif
    if (s->has_file) {
        /* Its turn comes after the file being sent, what's written after that, and any files
         * already waiting. */
        net_wait* w = (net_wait*)calloc(1, sizeof *w);
        if (!w) {
            close_file(copy);
            return 0;
        }
        w->file = copy;
        w->at = at;
        w->left = len;
        if (s->waiting_last) s->waiting_last->next = w;
        else s->waiting = w;
        s->waiting_last = w;
        return 1;
    }
    s->file = copy;
    s->has_file = 1;
    s->file_at = at;
    s->file_left = len;
    if (!s->dirty) {
        s->dirty = 1;
        n->dirty = net_grow(n->dirty, &n->dirty_cap, n->ndirty + 1, sizeof(cx_u32));
        n->dirty[n->ndirty++] = s->index;
    }
    return 1;
}

cx_bool cx_net_send_file(cx_u64 h, cx_u32 id, cx_i64 file, cx_u64 at, cx_u64 len) {
    net* n = net_enter(h);
    cx_bool ok = send_file(n, id, file, at, len);
    net_leave(n);
    return ok;
}

/* Sends what was queued on every connection, without waiting for the next poll. */
void cx_net_flush(cx_u64 h) {
    net* n = net_enter(h);
    send_dirty(n);
    net_leave(n);
}

/* Lets other threads wake the poller's polls: 1 once they can. */
static cx_bool waker(net* n) {
    if (n->has_waker) return 1;
#ifdef __APPLE__
    /* On macOS a wake triggers a user event in the kqueue, which EV_CLEAR sets back once seen. */
    struct kevent e;
    EV_SET(&e, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(n->ep, &e, 1, NULL, 0, NULL) != 0) return 0;
#elif !defined(_WIN32)
    /* On Windows a wake is a completion posted to the port, which needs nothing made first. */
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) return 0;
    struct epoll_event e;
    memset(&e, 0, sizeof e);
    e.events = EPOLLIN | EPOLLET;
    e.data.u32 = NET_NONE;
    if (epoll_ctl(n->ep, EPOLL_CTL_ADD, fd, &e) != 0) {
        close(fd);
        return 0;
    }
    n->wake_fd = fd;
#endif
    n->has_waker = 1;
    return 1;
}

cx_bool cx_net_waker(cx_u64 h) {
    net* n = net_enter(h);
    cx_bool ok = waker(n);
    net_leave(n);
    return ok;
}

/* Wakes the poller's poll, or its next, from any thread. A wake while one is on its way is
 * part of it. */
void cx_net_wake(cx_u64 h) {
    net_check_thread();
    net* n = (net*)(uintptr_t)h;
    if (__atomic_exchange_n(&n->woken, 1, __ATOMIC_SEQ_CST)) return;
#ifdef _WIN32
    PostQueuedCompletionStatus(n->iocp, 0, 0, NULL);
#elif defined(__APPLE__)
    struct kevent e;
    EV_SET(&e, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent(n->ep, &e, 1, NULL, 0, NULL);
#else
    cx_u64 one = 1;
    ssize_t r = write(n->wake_fd, &one, sizeof one);
    (void)r;
#endif
}

/* Whether every connection has sent what was queued on it, and each the program closed has
 * closed: on Windows, once the receive `finish` cancelled has come back and the socket with it. */
static int settled(net* n) {
    for (size_t i = 0; i < n->nslots; i++) {
        net_slot* s = n->slots[i];
        if (s->kind == SLOT_UDP && s->state != CONN_CLOSED) {
#ifdef _WIN32
            if (s->sending) return 0;
#endif
            if (buf_live(&s->out)) return 0;
            continue;
        }
        if (s->kind != SLOT_CONN || s->state == CONN_CONNECTING) continue;
        if (s->state == CONN_CLOSING) return 0;
#ifdef _WIN32
        if (s->state == CONN_CLOSED) {
            if (s->s != INVALID_SOCKET) return 0;
            continue;
        }
        if (s->sending) return 0;
#else
        if (s->state == CONN_CLOSED) continue;
#endif
        if (buf_live(&s->out) || s->has_file || s->waiting) return 0;
    }
    return 1;
}

/* Waits up to `timeout_ms`, or forever if it's negative, until what was queued has gone and what
 * was closed has closed, for a program about to end, which would cut both off. 1 if they have.
 * What happens meanwhile is kept for the next poll. */
static cx_bool drain(net* n, cx_i64 timeout_ms) {
#ifdef _WIN32
    ULONGLONG start = GetTickCount64();
#else
    cx_i64 start = monotonic_ms();
#endif
    for (;;) {
        send_dirty(n);
        if (settled(n)) return 1;
#ifdef _WIN32
        cx_i64 spent = (cx_i64)(GetTickCount64() - start);
#else
        cx_i64 spent = monotonic_ms() - start;
#endif
        if (timeout_ms >= 0 && spent >= timeout_ms) return 0;
        cx_i64 slice = timeout_ms < 0 || timeout_ms - spent > 10 ? 10 : timeout_ms - spent;
        wait_events(n, slice);
    }
}

cx_bool cx_net_drain(cx_u64 h, cx_i64 timeout_ms) {
    net* n = net_enter(h);
    cx_bool ok = drain(n, timeout_ms);
    net_leave(n);
    return ok;
}

/* Each signal std.signal handles wakes the poller's poll, as a waker does, from the thread that
 * hears of it. The list is in calx_rt.c, so a program without std.signal needs nothing more. */
void cx_signal_wakes_add(void (*wake)(cx_u64), cx_u64 arg);

/* A signal's wake, which says it was a signal too, for `cx_net_signalled`. Every poller asked to
 * is woken, so each hears of each signal, whichever thread takes it from std.signal. */
static void signal_wake(cx_u64 h) {
    __atomic_store_n(&((net*)(uintptr_t)h)->signalled, 1, __ATOMIC_SEQ_CST);
    cx_net_wake(h);
}

cx_bool cx_net_wake_on_signal(cx_u64 h) {
    net* n = net_enter(h);
    cx_bool ok = waker(n);
    net_leave(n);
    if (ok) cx_signal_wakes_add(signal_wake, h);
    return ok;
}

/* Whether a signal woke the poller since this last said so, which it then forgets. Any thread may
 * ask. */
cx_bool cx_net_signalled(cx_u64 h) {
    return __atomic_exchange_n(&((net*)(uintptr_t)h)->signalled, 0, __ATOMIC_SEQ_CST) != 0;
}

static void close_conn(net* n, cx_u32 id) {
    net_slot* s = conn_of(n, id);
    if (!s || s->state == CONN_CLOSED || s->state == CONN_CLOSING) return;
    if (s->state == CONN_CONNECTING) {
        lost(n, s);
        return;
    }
    /* What was queued goes first. */
#ifdef _WIN32
    post_send(n, s);
    if (s->state == CONN_CLOSED) return;
    if (!s->sending && !buf_live(&s->out)) finish(n, s);
    else s->state = CONN_CLOSING;
#else
    flush(n, s);
    if (s->state == CONN_CLOSED) return;
    if (!buf_live(&s->out) && !s->has_file) lost(n, s);
    else s->state = CONN_CLOSING;
#endif
}

void cx_net_close(cx_u64 h, cx_u32 id) {
    net* n = net_enter(h);
    close_conn(n, id);
    net_leave(n);
}

/* The other end of the connection, as it was when the connection opened: 4 or 6 for its family,
 * or 0 if there's no such connection or the system didn't say. The address goes in out[0..16),
 * the port in out[16..18) and an IPv6 scope in out[18..22), both big-endian. An IPv4 client of
 * a socket that listens on IPv6 too comes back as IPv4, not as an IPv4-mapped IPv6 address. */
static cx_u32 peer(const net_slot* s, cx_u8* out) {
    memset(out, 0, 22);
    if (!s || !s->has_peer) return 0;
    return addr_parts(&s->peer, out);
}

cx_u32 cx_net_peer(cx_u64 h, cx_u32 id, cx_u8* out) {
    net* n = net_enter(h);
    cx_u32 family = peer(conn_of(n, id), out);
    net_leave(n);
    return family;
}

cx_i32 cx_net_listen(cx_u64 h, const cx_u8* host, cx_usize len, cx_u16 port) {
    net* n = net_enter(h);
    cx_i32 bound = net_listen(n, host, len, port);
    net_leave(n);
    return bound;
}

cx_u32 cx_net_connect(cx_u64 h, const cx_u8* host, cx_usize len, cx_u16 port) {
    net* n = net_enter(h);
    cx_u32 id = net_connect(n, host, len, port);
    net_leave(n);
    return id;
}

/* UDP (CLX-412). */

static net_slot* udp_of(net* n, cx_u32 id) {
    cx_u32 index = id & (((cx_u32)1 << NET_INDEX_BITS) - 1);
    if (index >= n->nslots) return NULL;
    net_slot* s = n->slots[index];
    if (s->kind != SLOT_UDP || s->gen != (cx_u8)(id >> NET_INDEX_BITS) || s->reported) return NULL;
    return s;
}

static net_sock udp_socket(int family) {
#ifdef _WIN32
    return WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
#elif defined(__APPLE__)
    return own_socket(socket(family, SOCK_DGRAM, IPPROTO_UDP));
#else
    return socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
#endif
}

#ifdef _WIN32
#define NET_BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef SIO_UDP_NETRESET
#define SIO_UDP_NETRESET _WSAIOW(IOC_VENDOR, 15)
#endif
#else
#define NET_BAD_SOCK (-1)
#define close_sock close
#endif

/* Opens a UDP socket bound to `host` and `port`, the system's choice of port if it's 0. An empty
 * host is every address, IPv4 and IPv6, with one socket that takes both, or IPv4 alone where the
 * machine has no IPv6. An IPv6 address, such as `::`, is IPv6 alone. With `reuse`, other sockets
 * that ask for it can bind the same port, as each program with an mDNS responder does on 5353,
 * and each gets the multicast datagrams sent to it. */
static cx_u32 udp_open(net* n, const cx_u8* host, cx_usize len, cx_u16 port, int reuse) {
    struct addrinfo* a = resolve(host, len, port, 1, 1);
    if (!a) return NET_NONE;
    const struct addrinfo* b = listen_addr(a, len == 0);
    net_sock fd = udp_socket(b->ai_family);
    if (fd == NET_BAD_SOCK && b != a) {
        b = a;
        fd = udp_socket(b->ai_family);
    }
    if (fd == NET_BAD_SOCK) {
        fail_os();
        freeaddrinfo(a);
        return NET_NONE;
    }
    int family = b->ai_family;
    int v6only = family == AF_INET6 && len != 0;
    int yes = 1, flag = v6only;
#ifdef _WIN32
    BOOL win_yes = TRUE;
    setsockopt(fd, SOL_SOCKET, reuse ? SO_REUSEADDR : SO_EXCLUSIVEADDRUSE, (const char*)&win_yes, sizeof win_yes);
    /* Nonblocking, so a send the socket has no room for comes back at once and waits in `out`, while
     * receives stay overlapped. An ICMP unreachable that a datagram sent brings would end the next
     * receive with an error, which these turn off. */
    u_long nonblocking = 1;
    ioctlsocket(fd, FIONBIO, &nonblocking);
    BOOL off = FALSE;
    DWORD got;
    WSAIoctl(fd, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    WSAIoctl(fd, SIO_UDP_NETRESET, &off, sizeof off, NULL, 0, &got, NULL, NULL);
#else
    if (reuse) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#ifdef __APPLE__
        /* macOS shares a port between sockets only with this as well, as mDNSResponder's has it. */
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof yes);
#endif
    }
#endif
    (void)yes;
    if (family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&flag, sizeof flag);
#if defined(__linux__)
    /* Linux gives a socket the datagrams of every group any socket on the machine joined, unless
     * it's told not to; other systems give it only its own groups'. */
    int none = 0;
#ifndef IP_MULTICAST_ALL
#define IP_MULTICAST_ALL 49
#endif
#ifndef IPV6_MULTICAST_ALL
#define IPV6_MULTICAST_ALL 29
#endif
    if (family == AF_INET || !v6only) setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &none, sizeof none);
    if (family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_ALL, &none, sizeof none);
#endif
    if (bind(fd, b->ai_addr, (net_socklen)b->ai_addrlen) != 0) {
        fail_os();
        close_sock(fd);
        freeaddrinfo(a);
        return NET_NONE;
    }
    freeaddrinfo(a);
    net_slot* s = new_slot(n, SLOT_UDP);
    if (!s) {
        net_fail(ERR_OTHER, 0);
        close_sock(fd);
        return NET_NONE;
    }
    SOCK_OF(s) = fd;
    s->ufamily = family;
    s->uv6only = family == AF_INET || v6only;
    s->state = CONN_OPEN;
#ifdef _WIN32
    s->ubuf = malloc(NET_UDP_MAX);
    if (!s->ubuf) net_out_of_memory();
    if (!CreateIoCompletionPort((HANDLE)fd, n->iocp, 0, 0)) {
        fail_os();
        closesocket(fd);
        s->s = INVALID_SOCKET;
        release_slot(n, s);
        return NET_NONE;
    }
    post_udp_recv(n, s);
#else
    if (watch(n, s, fd, 1) != 0) {
        fail_os();
        close(fd);
        s->fd = -1;
        release_slot(n, s);
        return NET_NONE;
    }
    s->writable = 1;
#endif
    return slot_id(s);
}

cx_u32 cx_net_udp_open(cx_u64 h, const cx_u8* host, cx_usize len, cx_u16 port, cx_bool reuse) {
    net* n = net_enter(h);
    cx_u32 id = udp_open(n, host, len, port, reuse);
    net_leave(n);
    return id;
}

/* The port the socket is bound to, or 0 once it's closed. */
cx_u16 cx_net_udp_port(cx_u64 h, cx_u32 id) {
    net* n = net_enter(h);
    net_slot* s = udp_of(n, id);
    cx_u16 port = 0;
    if (s) {
        struct sockaddr_storage at;
        net_socklen at_len = sizeof at;
        if (getsockname(SOCK_OF(s), (struct sockaddr*)&at, &at_len) == 0) port = local_port(&at);
    }
    net_leave(n);
    return port;
}

/* Sends `len` bytes to the address std.net gives, of `family`: at once, unless the socket has no
 * room, when it waits its turn. 0 if it went or waits, or -1 with the error. A socket that has
 * 1 MiB waiting already drops it, as a network would. */
static cx_i32 udp_send(net* n, net_slot* s, const cx_u8* p, cx_usize len, const cx_u8* to, cx_u32 family) {
    struct sockaddr_storage sa;
    net_socklen sa_len;
    if (!addr_in(to, family, s->ufamily, &sa, &sa_len)) {
        net_fail(ERR_ADDRESS, 0);
        return -1;
    }
#ifdef _WIN32
    int idle = !s->sending && !buf_live(&s->out);
#else
    int idle = s->writable && !buf_live(&s->out);
#endif
    if (idle) {
        udp_mif(s, s->mif_cur);
        for (;;) {
            if (sendto(SOCK_OF(s), (const char*)p, (int)len, 0, (struct sockaddr*)&sa, sa_len) >= 0) return 0;
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
                s->writable = 0;
                break;
            }
            if (errno == EINTR) continue;
#endif
            fail_os();
            return -1;
        }
    }
    if (buf_live(&s->out) + UDP_OUT_HEAD + (size_t)sa_len + len > NET_UDP_KEEP) return 0;
    udp_queue(s, p, len, &sa, sa_len);
#ifdef _WIN32
    udp_send_queued(n, s);
#else
    (void)n;
#endif
    return 0;
}

cx_i32 cx_net_udp_send(cx_u64 h, cx_u32 id, const cx_u8* p, cx_usize len, const cx_u8* to, cx_u32 family) {
    net* n = net_enter(h);
    net_slot* s = udp_of(n, id);
    cx_i32 r = -1;
    if (!s) net_fail(ERR_OTHER, 0);
    else r = udp_send(n, s, p, len, to, family);
    net_leave(n);
    return r;
}

/* The length of the next datagram waiting on the socket, or -1 if there's none. */
cx_i64 cx_net_udp_size(cx_u64 h, cx_u32 id) {
    net* n = net_enter(h);
    net_slot* s = udp_of(n, id);
    cx_i64 k = -1;
    if (s && buf_live(&s->in) >= UDP_HEAD) {
        cx_u32 l;
        memcpy(&l, s->in.p + s->in.at, 4);
        k = l;
    }
    net_leave(n);
    return k;
}

/* Takes the next datagram: its bytes to `to`, which has room for them, and where it came from to
 * `from`, 22 bytes, as `addr_parts` writes it. Its family, or 0 if there was none. */
cx_u32 cx_net_udp_take(cx_u64 h, cx_u32 id, cx_u8* to, cx_u8* from) {
    net* n = net_enter(h);
    net_slot* s = udp_of(n, id);
    cx_u32 family = 0;
    if (s && buf_live(&s->in) >= UDP_HEAD) {
        const cx_u8* at = s->in.p + s->in.at;
        cx_u32 l;
        memcpy(&l, at, 4);
        family = at[4];
        memcpy(from, at + 5, 22);
        if (l) memcpy(to, at + UDP_HEAD, l);
        s->in.at += UDP_HEAD + l;
        if (s->in.at >= s->in.len) s->in.at = s->in.len = 0;
        else buf_compact(&s->in);
    }
    net_leave(n);
    return family;
}

/* Joins, or leaves, the multicast group whose address std.net gives, of `family`, on the
 * interface with that index, or the system's choice if it's 0. 1 if it could. An IPv4 group's
 * interface is named by its index as an address in 0.0.0.0/8 on Windows, and by the index itself
 * elsewhere. */
static int udp_join(net_slot* s, const cx_u8* group, cx_u32 family, cx_u32 index, int leave) {
    net_sock fd = SOCK_OF(s);
    if (family == 6) {
        if (s->ufamily != AF_INET6) {
            net_fail(ERR_ADDRESS, 0);
            return 0;
        }
        struct ipv6_mreq m;
        memset(&m, 0, sizeof m);
        memcpy(&m.ipv6mr_multiaddr, group, 16);
        m.ipv6mr_interface = index;
        if (setsockopt(fd, IPPROTO_IPV6, leave ? IPV6_LEAVE_GROUP : IPV6_JOIN_GROUP, (const char*)&m, sizeof m) != 0) {
            fail_os();
            return 0;
        }
        return 1;
    }
    if (family != 4 || (s->ufamily == AF_INET6 && s->uv6only)) {
        net_fail(ERR_ADDRESS, 0);
        return 0;
    }
    int r;
#ifdef _WIN32
    struct ip_mreq m;
    memset(&m, 0, sizeof m);
    memcpy(&m.imr_multiaddr, group, 4);
    m.imr_interface.s_addr = htonl(index);
    r = setsockopt(fd, IPPROTO_IP, leave ? IP_DROP_MEMBERSHIP : IP_ADD_MEMBERSHIP, (const char*)&m, sizeof m);
#elif defined(__APPLE__)
    struct group_req g;
    memset(&g, 0, sizeof g);
    g.gr_interface = index;
    struct sockaddr_in* ga = (struct sockaddr_in*)&g.gr_group;
    ga->sin_family = AF_INET;
    ga->sin_len = sizeof *ga;
    memcpy(&ga->sin_addr, group, 4);
    r = setsockopt(fd, IPPROTO_IP, leave ? MCAST_LEAVE_GROUP : MCAST_JOIN_GROUP, &g, sizeof g);
#else
    struct ip_mreqn m;
    memset(&m, 0, sizeof m);
    memcpy(&m.imr_multiaddr, group, 4);
    m.imr_ifindex = (int)index;
    r = setsockopt(fd, IPPROTO_IP, leave ? IP_DROP_MEMBERSHIP : IP_ADD_MEMBERSHIP, &m, sizeof m);
#endif
    if (r != 0) {
        fail_os();
        return 0;
    }
    return 1;
}

cx_bool cx_net_udp_join(cx_u64 h, cx_u32 id, const cx_u8* group, cx_u32 family, cx_u32 index, cx_bool leave) {
    net* n = net_enter(h);
    net_slot* s = udp_of(n, id);
    int ok = 0;
    if (!s) net_fail(ERR_OTHER, 0);
    else ok = udp_join(s, group, family, index, leave);
    net_leave(n);
    return ok != 0;
}

enum { UDP_MULTICAST_HOPS = 1, UDP_MULTICAST_LOOP, UDP_HOPS, UDP_BROADCAST, UDP_MULTICAST_IF };

/* Sets an option, for IPv4 and for IPv6 as the socket has them: 1 if it could. */
static int udp_option(net_slot* s, cx_u32 which, cx_u32 value) {
    net_sock fd = SOCK_OF(s);
    int v6 = s->ufamily == AF_INET6, v4 = !v6 || !s->uv6only;
    int ok6 = 1, ok4 = 1;
    if (which == UDP_MULTICAST_IF) {
        if (!set_mif(fd, s->ufamily, s->uv6only, value)) {
            fail_os();
            return 0;
        }
        s->mif_cur = s->mif_set = value;
        return 1;
    }
    if (which == UDP_BROADCAST) {
        int b = value != 0;
        ok4 = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&b, sizeof b) == 0;
    } else if (which == UDP_HOPS) {
        int hops = (int)value;
        if (v6) ok6 = setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, (const char*)&hops, sizeof hops) == 0;
        if (v4) ok4 = setsockopt(fd, IPPROTO_IP, IP_TTL, (const char*)&hops, sizeof hops) == 0 || v6;
    } else {
        int six = (int)value;
        int name6 = which == UDP_MULTICAST_HOPS ? IPV6_MULTICAST_HOPS : IPV6_MULTICAST_LOOP;
        int name4 = which == UDP_MULTICAST_HOPS ? IP_MULTICAST_TTL : IP_MULTICAST_LOOP;
        if (v6) ok6 = setsockopt(fd, IPPROTO_IPV6, name6, (const char*)&six, sizeof six) == 0;
        if (v4) {
#if defined(__APPLE__)
            /* macOS takes these two IPv4 options as a byte. */
            unsigned char four = (unsigned char)value;
#elif defined(_WIN32)
            DWORD four = value;
#else
            int four = (int)value;
#endif
            /* On a socket for both families, the IPv4 one is the system's to refuse. */
            ok4 = setsockopt(fd, IPPROTO_IP, name4, (const char*)&four, sizeof four) == 0 || v6;
        }
    }
    if (!ok6 || !ok4) {
        fail_os();
        return 0;
    }
    return 1;
}

cx_bool cx_net_udp_option(cx_u64 h, cx_u32 id, cx_u32 which, cx_u32 value) {
    net* n = net_enter(h);
    net_slot* s = udp_of(n, id);
    int ok = 0;
    if (!s) net_fail(ERR_OTHER, 0);
    else ok = udp_option(s, which, value);
    net_leave(n);
    return ok != 0;
}

/* Closes the socket. What waited to be sent is dropped, and so is what arrived. Its slot is freed
 * at the next poll, or, on Windows, once the system gives back what it holds. */
void cx_net_udp_close(cx_u64 h, cx_u32 id) {
    net* n = net_enter(h);
    net_slot* s = udp_of(n, id);
    if (s) {
        s->state = CONN_CLOSED;
        s->reported = 1;
        if (SOCK_OF(s) != NET_BAD_SOCK) close_sock(SOCK_OF(s));
        SOCK_OF(s) = NET_BAD_SOCK;
        s->in.at = s->in.len = 0;
        s->out.at = s->out.len = 0;
        n->retiring = net_grow(n->retiring, &n->retiring_cap, n->nretiring + 1, sizeof(cx_u32));
        n->retiring[n->nretiring++] = s->index;
    }
    net_leave(n);
}

/* The machine's network interfaces, for std.net's `interfaces`, put together by the first call,
 * on the thread that asks, and copied out by the second. Each is its index, 4 bytes big-endian,
 * flags (1 up, 2 loopback, 4 multicast), its name's length, 2 bytes, the name in UTF-8, how many
 * addresses it has, 2 bytes, and each address: its family, 4 or 6, 22 bytes as `addr_parts` writes
 * them, and its prefix length. */
static _Thread_local net_buf if_list;

static void if_put16(cx_u16 v) {
    cx_u8 b[2] = {(cx_u8)(v >> 8), (cx_u8)v};
    buf_append(&if_list, b, 2);
}

static void if_start(cx_u32 index, int flags, const char* name, size_t name_len) {
    cx_u8 b[5] = {(cx_u8)(index >> 24), (cx_u8)(index >> 16), (cx_u8)(index >> 8), (cx_u8)index, (cx_u8)flags};
    buf_append(&if_list, b, 5);
    if (name_len > 0xFFFF) name_len = 0xFFFF;
    if_put16((cx_u16)name_len);
    buf_append(&if_list, (const cx_u8*)name, name_len);
}

static void if_address(const struct sockaddr* sa, int prefix) {
    struct sockaddr_storage st;
    memset(&st, 0, sizeof st);
    memcpy(&st, sa, sa->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
    cx_u8 b[24];
    b[0] = (cx_u8)addr_parts(&st, b + 1);
#ifdef __APPLE__
    /* macOS keeps a link-local address's scope in its third and fourth bytes too. */
    if (b[0] == 6 && b[1] == 0xFE && (b[2] & 0xC0) == 0x80) b[3] = b[4] = 0;
#endif
    b[23] = (cx_u8)prefix;
    buf_append(&if_list, b, 24);
}

#ifndef _WIN32
/* How many bits a netmask has set. */
static int mask_bits(const struct sockaddr* m) {
    if (!m) return 0;
    const cx_u8* p;
    int len;
    if (m->sa_family == AF_INET6) {
        p = (const cx_u8*)&((const struct sockaddr_in6*)m)->sin6_addr;
        len = 16;
    } else {
        p = (const cx_u8*)&((const struct sockaddr_in*)m)->sin_addr;
        len = 4;
    }
    int bits = 0;
    for (int i = 0; i < len; i++) {
        for (cx_u8 b = p[i]; b; b <<= 1) bits++;
    }
    return bits;
}
#endif

/* The length of the list, or -1 with the error. */
cx_i64 cx_net_interfaces(void) {
    net_check_thread();
    if_list.at = if_list.len = 0;
#ifdef _WIN32
    if (!net_start()) {
        net_fail(ERR_OTHER, WSAGetLastError());
        return -1;
    }
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES* list = NULL;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        free(list);
        list = malloc(size);
        if (!list) net_out_of_memory();
        r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 NULL, list, &size);
    }
    if (r != NO_ERROR) {
        free(list);
        net_fail(ERR_OTHER, (cx_i32)r);
        return -1;
    }
    for (IP_ADAPTER_ADDRESSES* a = list; a; a = a->Next) {
        char name[512];
        int name_len = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof name, NULL, NULL);
        name_len = name_len > 0 ? name_len - 1 : 0;
        int flags = (a->OperStatus == IfOperStatusUp ? 1 : 0) | (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ? 2 : 0) |
                    ((a->Flags & IP_ADAPTER_NO_MULTICAST) ? 0 : 4);
        if_start(a->IfIndex ? a->IfIndex : a->Ipv6IfIndex, flags, name, (size_t)name_len);
        cx_u16 count = 0;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            int f = u->Address.lpSockaddr->sa_family;
            if ((f == AF_INET || f == AF_INET6) && count < 0xFFFF) count++;
        }
        if_put16(count);
        cx_u16 put = 0;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u && put < count; u = u->Next) {
            int f = u->Address.lpSockaddr->sa_family;
            if (f != AF_INET && f != AF_INET6) continue;
            if_address(u->Address.lpSockaddr, u->OnLinkPrefixLength);
            put++;
        }
    }
    free(list);
#else
    struct ifaddrs* all = NULL;
    if (getifaddrs(&all) != 0) {
        fail_os();
        return -1;
    }
    /* getifaddrs gives an entry for each address, so each interface's are gathered under the first
     * entry with its name. */
    for (struct ifaddrs* i = all; i; i = i->ifa_next) {
        int first = 1;
        for (struct ifaddrs* j = all; j != i; j = j->ifa_next) {
            if (strcmp(j->ifa_name, i->ifa_name) == 0) {
                first = 0;
                break;
            }
        }
        if (!first) continue;
        unsigned flags = (unsigned)i->ifa_flags;
        int f = ((flags & IFF_UP) && (flags & IFF_RUNNING) ? 1 : 0) | ((flags & IFF_LOOPBACK) ? 2 : 0) | ((flags & IFF_MULTICAST) ? 4 : 0);
        if_start(if_nametoindex(i->ifa_name), f, i->ifa_name, strlen(i->ifa_name));
        cx_u16 count = 0;
        for (struct ifaddrs* j = i; j; j = j->ifa_next) {
            if (strcmp(j->ifa_name, i->ifa_name) != 0 || !j->ifa_addr) continue;
            int fam = j->ifa_addr->sa_family;
            if ((fam == AF_INET || fam == AF_INET6) && count < 0xFFFF) count++;
        }
        if_put16(count);
        cx_u16 put = 0;
        for (struct ifaddrs* j = i; j && put < count; j = j->ifa_next) {
            if (strcmp(j->ifa_name, i->ifa_name) != 0 || !j->ifa_addr) continue;
            int fam = j->ifa_addr->sa_family;
            if (fam != AF_INET && fam != AF_INET6) continue;
            if_address(j->ifa_addr, mask_bits(j->ifa_netmask));
            put++;
        }
    }
    freeifaddrs(all);
#endif
    return (cx_i64)if_list.len;
}

void cx_net_interfaces_copy(cx_u8* to) {
    if (if_list.len) memcpy(to, if_list.p, if_list.len);
    buf_free(&if_list);
}

/* The machine's name, as Python's `socket.gethostname()` gives it, into `to`, which has room for
 * `cap` bytes: its length, or 0 if the system wouldn't say. */
cx_usize cx_net_host_name(cx_u8* to, cx_usize cap) {
    char name[256];
    memset(name, 0, sizeof name);
#ifdef _WIN32
    if (!net_start()) return 0;
#endif
    if (gethostname(name, (int)sizeof name - 1) != 0) return 0;
    size_t len = strlen(name);
    if (len > cap) len = cap;
    memcpy(to, name, len);
    return len;
}

/* Why the last call that failed on this thread did: each thread has its own. */
cx_u32 cx_net_error_kind(void) { return net_err_kind; }
cx_i32 cx_net_error_code(void) { return net_err_code; }
