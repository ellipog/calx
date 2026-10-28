/* The Calx runtime's TCP networking, behind std.net (D33, D92).
 *
 * A poller owns its connections, and only the thread that opened it uses it. The runtime keeps
 * each connection's buffers: what arrived, waiting for `read`, and what `send` queued, waiting
 * for the socket. `poll` reports what happened as events: a connection accepted, an outgoing one
 * open, bytes received, a connection closed. On Windows that's IOCP, whose operations complete
 * into the runtime's buffers. On Linux it's edge-triggered epoll, and the runtime reads and
 * writes when the socket is ready. Either way the program sees the same events.
 *
 * A connection's id is its slot in the poller and the slot's generation, so a stale id never
 * reaches a newer connection in the same slot. A slot is reused only after the poll that
 * reported its `Closed` event, and, on Windows, once the system has returned every operation.
 *
 * This file is compiled into a program only when it uses std.net, so other programs don't link
 * the socket libraries. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "calx_rt.h"

int cx_in_parallel(void);
CX_COLD void cx_panic_text(const char* at, const char* message);

enum { EV_ACCEPTED = 1, EV_CONNECTED = 2, EV_RECEIVED = 3, EV_CLOSED = 4 };
enum { ERR_IN_USE = 1, ERR_ADDRESS = 2, ERR_REFUSED = 3, ERR_OTHER = 4 };
enum { SLOT_FREE, SLOT_LISTENER, SLOT_CONN };
enum { CONN_CONNECTING, CONN_OPEN, CONN_CLOSING, CONN_CLOSED };

#define NET_NONE 0xFFFFFFFFu
#define NET_INDEX_BITS 24
#define NET_RECV 16384 /* bytes read at a time */

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

#ifdef _WIN32
enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CONNECT };
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
    int reported;  /* a poll before this one returned its Closed event */
    net_buf in, out;
#ifdef _WIN32
    SOCKET s;
    int pending;  /* operations the system still holds */
    int sending;
    int receiving;  /* a receive is pending */
    net_op rop, sop, cop;
    net_buf fly;  /* what the send in flight is sending */
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
    int ep;
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
} net;

static cx_u32 slot_id(const net_slot* s) { return ((cx_u32)s->gen << NET_INDEX_BITS) | s->index; }

static void emit(net* n, cx_u32 kind, net_slot* s) {
    n->ev = net_grow(n->ev, &n->ev_cap, n->nev + 2, sizeof(cx_u32));
    n->ev[n->nev++] = kind;
    n->ev[n->nev++] = slot_id(s);
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

static void release_slot(net* n, net_slot* s) {
    buf_free(&s->in);
    buf_free(&s->out);
#ifdef _WIN32
    buf_free(&s->fly);
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

/* Resolves `host`, which isn't NUL-terminated, to an address. An empty host is every address. */
static struct addrinfo* resolve(const cx_u8* host, cx_usize len, cx_u16 port, int passive) {
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
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (passive) hints.ai_flags = AI_PASSIVE;
    struct addrinfo* res = NULL;
    if (getaddrinfo(len ? name : NULL, service, &hints, &res) != 0 || !res) {
        net_fail(ERR_ADDRESS, 0);
        return NULL;
    }
    return res;
}

static cx_u16 local_port(const struct sockaddr_storage* a) {
    if (a->ss_family == AF_INET6) return ntohs(((const struct sockaddr_in6*)a)->sin6_port);
    return ntohs(((const struct sockaddr_in*)a)->sin_port);
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
    for (size_t i = 0; i < n->nout; i += 2) {
        net_slot* s = n->slots[n->out[i + 1] & (((cx_u32)1 << NET_INDEX_BITS) - 1)];
        if (n->out[i] == EV_RECEIVED) s->received = 0;
    }
    return n->nout / 2;
}

#ifdef _WIN32

static LPFN_ACCEPTEX accept_ex;
static LPFN_CONNECTEX connect_ex;
static volatile LONG wsa_ready; /* 0, 1 while starting, 2 when started */

static int net_start(void) {
    if (InterlockedCompareExchange(&wsa_ready, 1, 0) == 0) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) {
            InterlockedExchange(&wsa_ready, 0);
            return 0;
        }
        SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        GUID a = WSAID_ACCEPTEX, c = WSAID_CONNECTEX;
        DWORD got;
        WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &a, sizeof a, &accept_ex, sizeof accept_ex, &got, NULL, NULL);
        WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &c, sizeof c, &connect_ex, sizeof connect_ex, &got, NULL, NULL);
        closesocket(s);
        InterlockedExchange(&wsa_ready, 2);
    }
    while (wsa_ready == 1) SwitchToThread();
    return wsa_ready == 2;
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

static void post_send(net* n, net_slot* s) {
    if (s->sending || s->state == CONN_CONNECTING || s->state == CONN_CLOSED) return;
    if (!buf_live(&s->fly)) {
        if (!buf_live(&s->out)) return;
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
    }
}

static void wait_events(net* n, cx_i64 timeout_ms) {
    DWORD wait = timeout_ms < 0 ? INFINITE : timeout_ms > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)timeout_ms;
    for (;;) {
        OVERLAPPED_ENTRY entries[64];
        ULONG got = 0;
        if (!GetQueuedCompletionStatusEx(n->iocp, entries, 64, &got, wait, FALSE)) return;
        for (ULONG i = 0; i < got; i++) completed(n, &entries[i]);
        if (got < 64) return;
        wait = 0;
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
    n->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!n->iocp) {
        net_fail(ERR_OTHER, (cx_i32)GetLastError());
        free(n);
        return 0;
    }
    return (cx_u64)(uintptr_t)n;
}

cx_i32 cx_net_listen(cx_u64 h, const cx_u8* host, cx_usize len, cx_u16 port) {
    net_check_thread();
    net* n = (net*)(uintptr_t)h;
    struct addrinfo* a = resolve(host, len, port, 1);
    if (!a) return -1;
    SOCKET s = WSASocketW(a->ai_family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET) {
        fail_os();
        freeaddrinfo(a);
        return -1;
    }
    BOOL yes = TRUE, no = FALSE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&yes, sizeof yes);
    /* An IPv6 listener takes IPv4 clients too, as it does on Linux. */
    if (a->ai_family == AF_INET6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&no, sizeof no);
    int family = a->ai_family;
    if (bind(s, a->ai_addr, (int)a->ai_addrlen) != 0 || listen(s, SOMAXCONN) != 0 ||
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

cx_u32 cx_net_connect(cx_u64 h, const cx_u8* host, cx_usize len, cx_u16 port) {
    net_check_thread();
    net* n = (net*)(uintptr_t)h;
    struct addrinfo* a = resolve(host, len, port, 0);
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
        for (ULONG i = 0; i < got; i++) ((net_op*)entries[i].lpOverlapped)->slot->pending--;
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

static void flush(net* n, net_slot* s) {
    if (s->state == CONN_CONNECTING || s->state == CONN_CLOSED) return;
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
    if (!buf_live(&s->out)) {
        s->out.at = s->out.len = 0;
        if (s->state == CONN_CLOSING) lost(n, s);
    }
}

static int watch(net* n, net_slot* s, int fd, cx_u32 events) {
    struct epoll_event e;
    memset(&e, 0, sizeof e);
    e.events = events;
    e.data.u32 = s->index;
    return epoll_ctl(n->ep, EPOLL_CTL_ADD, fd, &e);
}

static void opened(net* n, net_slot* s) {
    int yes = 1;
    setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
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
        int fd = accept4(l->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
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
        if (watch(n, c, fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET) != 0) {
            close(fd);
            c->fd = -1;
            release_slot(n, c);
            continue;
        }
        emit(n, EV_ACCEPTED, c);
        opened(n, c);
    }
}

static void ready(net* n, net_slot* s, cx_u32 e) {
    if (s->kind == SLOT_LISTENER) {
        accept_all(n, s);
        return;
    }
    if (s->state == CONN_CLOSED) return;
    if (s->state == CONN_CONNECTING) {
        if (!(e & (EPOLLOUT | EPOLLERR | EPOLLHUP))) return;
        int err = 0;
        socklen_t len = sizeof err;
        if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            lost(n, s);
            return;
        }
        emit(n, EV_CONNECTED, s);
        opened(n, s);
    }
    if (e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) readable(n, s);
    if (s->state != CONN_CLOSED && (e & EPOLLOUT)) {
        s->writable = 1;
        flush(n, s);
    }
}

static void wait_events(net* n, cx_i64 timeout_ms) {
    int wait = timeout_ms < 0 ? -1 : timeout_ms > 0x7FFFFFFF ? 0x7FFFFFFF : (int)timeout_ms;
    for (;;) {
        struct epoll_event evs[64];
        int got = epoll_wait(n->ep, evs, 64, wait);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return;
        for (int i = 0; i < got; i++) ready(n, n->slots[evs[i].data.u32], evs[i].events);
        if (got < 64) return;
        wait = 0;
    }
}

cx_u64 cx_net_open(void) {
    net_check_thread();
    net* n = calloc(1, sizeof *n);
    if (!n) net_out_of_memory();
    n->ep = epoll_create1(EPOLL_CLOEXEC);
    if (n->ep < 0) {
        fail_os();
        free(n);
        return 0;
    }
    return (cx_u64)(uintptr_t)n;
}

cx_i32 cx_net_listen(cx_u64 h, const cx_u8* host, cx_usize len, cx_u16 port) {
    net_check_thread();
    net* n = (net*)(uintptr_t)h;
    struct addrinfo* a = resolve(host, len, port, 1);
    if (!a) return -1;
    int fd = socket(a->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    int yes = 1, no = 0;
    if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    if (fd >= 0 && a->ai_family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof no);
    if (fd < 0 || bind(fd, a->ai_addr, a->ai_addrlen) != 0 || listen(fd, SOMAXCONN) != 0) {
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
    if (!l || watch(n, l, fd, EPOLLIN | EPOLLET) != 0) {
        fail_os();
        close(fd);
        if (l) release_slot(n, l);
        return -1;
    }
    l->fd = fd;
    return local_port(&at);
}

cx_u32 cx_net_connect(cx_u64 h, const cx_u8* host, cx_usize len, cx_u16 port) {
    net_check_thread();
    net* n = (net*)(uintptr_t)h;
    struct addrinfo* a = resolve(host, len, port, 0);
    if (!a) return NET_NONE;
    int fd = socket(a->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
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
    if (watch(n, c, fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET) != 0 || (r != 0 && errno != EINPROGRESS)) {
        lost(n, c);
    }
    return slot_id(c);
}

static void close_all(net* n) {
    for (size_t i = 0; i < n->nslots; i++) {
        if (n->slots[i]->fd >= 0) close(n->slots[i]->fd);
    }
    close(n->ep);
}

#endif

/* Closes every socket, and frees the poller. */
void cx_net_free(cx_u64 h) {
    net_check_thread();
    net* n = (net*)(uintptr_t)h;
    close_all(n);
    for (size_t i = 0; i < n->nslots; i++) {
        net_slot* s = n->slots[i];
        buf_free(&s->in);
        buf_free(&s->out);
#ifdef _WIN32
        buf_free(&s->fly);
#endif
        free(s);
    }
    free(n->slots);
    free(n->free_ids);
    free(n->ev);
    free(n->out);
    free(n->retiring);
    free(n);
}

cx_usize cx_net_poll(cx_u64 h, cx_i64 timeout_ms) {
    net_check_thread();
    net* n = (net*)(uintptr_t)h;
    closed_seen(n);
    retire(n);
    wait_events(n, n->nev ? 0 : timeout_ms);
    return deliver(n);
}

cx_u32 cx_net_event_kind(cx_u64 h, cx_usize i) { return ((net*)(uintptr_t)h)->out[2 * i]; }
cx_u32 cx_net_event_conn(cx_u64 h, cx_usize i) { return ((net*)(uintptr_t)h)->out[2 * i + 1]; }

cx_usize cx_net_available(cx_u64 h, cx_u32 id) {
    net_check_thread();
    net_slot* s = conn_of((net*)(uintptr_t)h, id);
    return s ? buf_live(&s->in) : 0;
}

cx_usize cx_net_read(cx_u64 h, cx_u32 id, cx_u8* to, cx_usize max) {
    net_check_thread();
    net_slot* s = conn_of((net*)(uintptr_t)h, id);
    if (!s) return 0;
    size_t k = buf_live(&s->in);
    if (k > max) k = max;
    memcpy(to, s->in.p + s->in.at, k);
    s->in.at += k;
    if (s->in.at == s->in.len) s->in.at = s->in.len = 0;
    return k;
}

void cx_net_send(cx_u64 h, cx_u32 id, const cx_u8* p, cx_usize len) {
    net_check_thread();
    net* n = (net*)(uintptr_t)h;
    net_slot* s = conn_of(n, id);
    if (!s || len == 0 || s->state == CONN_CLOSED || s->state == CONN_CLOSING) return;
    buf_append(&s->out, p, len);
#ifdef _WIN32
    post_send(n, s);
#else
    flush(n, s);
#endif
}

void cx_net_close(cx_u64 h, cx_u32 id) {
    net_check_thread();
    net* n = (net*)(uintptr_t)h;
    net_slot* s = conn_of(n, id);
    if (!s || s->state == CONN_CLOSED || s->state == CONN_CLOSING) return;
#ifdef _WIN32
    if (s->state == CONN_CONNECTING) lost(n, s);
    else if (!s->sending && !buf_live(&s->out)) finish(n, s);
    else s->state = CONN_CLOSING;
#else
    if (s->state == CONN_CONNECTING || !buf_live(&s->out)) lost(n, s);
    else s->state = CONN_CLOSING;
#endif
}

cx_u32 cx_net_error_kind(void) { return net_err_kind; }
cx_i32 cx_net_error_code(void) { return net_err_code; }
