/* HTTP and HTTPS requests behind std.http (D118), on the system's own client so that certificates
 * are checked as the system checks them: WinHTTP on Windows, URLSession on iOS, and libcurl on
 * Linux and macOS, loaded with dlopen when the first request starts, so a program that makes none
 * doesn't need it installed. Every macOS has it; on Linux it's a package.
 * Linked only into programs that call them.
 *
 * Each request runs off the program's thread, the way vanilla asks Mojang's session server on an
 * authenticator thread, so the program's loop goes on while it waits and asks `cx_http_done` when
 * to take the response: on Windows each on a thread of its own, and on Linux and macOS together on
 * one, on a libcurl multi handle, so they share connections as WinHTTP's session shares them.
 * Requests are slots in a table, named by an index and a generation, so a request whose response
 * was taken can't be taken again: the Calx side panics instead.
 *
 * Both clients are set up to send the same request: HTTP/1.1 unless HTTP/2 is asked for, no proxy
 * (as vanilla's dedicated server uses none), redirects followed up to 10 but never from HTTPS to
 * HTTP, no Accept or Accept-Encoding unless the caller adds them, and a User-Agent of "Calx"
 * unless the caller gives one. The timeout is for the whole request. The response's header lines
 * come back as UTF-8 "name:value\r\n", the value trimmed, its bytes read as Latin-1 as Java reads
 * them.
 *
 * HTTP/2 (CLX-383) is WinHTTP's on Windows 10 1607 and later, over TLS alone, and libcurl's where
 * it's built with nghttp2, as Debian's, Ubuntu's, Fedora's and macOS's are, over TLS or as h2c. A
 * connection is kept for the requests after it, and HTTP/2 ones to a host go over one at once.
 *
 * WebSockets, behind std.http.websocket, are at the end of the file: on libcurl's connections on
 * Linux and macOS, on Network.framework's on iOS, and on Winsock and Schannel on Windows. */
#include "calx_rt.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#else
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#ifndef __APPLE__
#include <sys/random.h>
#endif
#endif
#ifdef CX_IOS
/* URLSession and Network.framework, for which this file is built as Objective-C (CLX-410). */
#import <Foundation/Foundation.h>
#include <Network/Network.h>
#endif

/* In calx_rt.c: starts a thread with the stack every thread of a program has (D135). */
#ifdef _WIN32
int cx_os_thread(HANDLE* thread, LPTHREAD_START_ROUTINE run, void* arg);
#else
int cx_os_thread(pthread_t* thread, void* (*run)(void*), void* arg);
#endif

/* Runs `init` once, before anything that needs it. */
static void once(cx_u32* state, void (*init)(void)) {
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) == 2) return;
    cx_u32 idle = 0;
    if (__atomic_compare_exchange_n(state, &idle, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init();
        __atomic_store_n(state, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2) {
        }
    }
}

/* What went wrong, as std.http's Error numbers it. */
enum { E_OK, E_BAD_URL, E_RESOLVE, E_CONNECT, E_TLS, E_TIMEOUT, E_PROTOCOL, E_TOO_LONG, E_UNAVAILABLE, E_OS };

/* The HTTP a request asks for, as std.http's Version numbers it (CLX-383): HTTP/1.1; HTTP/2 if the
 * server offers it in the TLS handshake's ALPN, else HTTP/1.1; or HTTP/2 and nothing else, with
 * prior knowledge on http://, h2c. */
enum { V_HTTP1, V_HTTP2, V_HTTP2_ONLY };

/* A request's times, each in nanoseconds from its start, as `cx_http_times` gives them. */
enum { T_RESOLVE, T_CONNECT, T_TLS, T_FIRST_BYTE, T_TOTAL, T_COUNT };

typedef struct {
    char* p;
    size_t len, cap;
} bytes;

/* Room for `n` more bytes. */
static int grow(bytes* b, size_t n) {
    if (b->cap - b->len >= n) return 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap - b->len < n) cap *= 2;
    char* p = realloc(b->p, cap);
    if (!p) return 0;
    b->p = p, b->cap = cap;
    return 1;
}

static int add(bytes* b, const void* p, size_t n) {
    if (!grow(b, n)) return 0;
    memcpy(b->p + b->len, p, n);
    b->len += n;
    return 1;
}

typedef struct request {
    cx_u32 state; /* 0 running, 1 finished */
    int post, https;
    char *url, *host, *target, *head;
    cx_u8* body;
    size_t body_len, limit;
    unsigned port;
    cx_u32 timeout_ms;
    int error;
    cx_i32 code;
    cx_u32 status;
    bytes raw;  /* the response's header lines as they came */
    bytes lines; /* and as std.http reads them */
    bytes data;
    /* For a progress bar, read by the program's thread while the request's writes them: how much
     * of the body has come, and its Content-Length plus 1, or 0 until that's known or if there's
     * none. */
    cx_u64 received, expected;
    int sized;
    int version;  /* asked for, V_ */
    cx_u32 used;  /* the HTTP the response came in: 10, 11 or 20, for 1.0, 1.1 and 2 */
    int reused;   /* whether it went over a connection a request before it had opened */
    cx_u64 times[T_COUNT];
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
    void* easy;              /* on the transfer thread, libcurl's handle */
    struct curl_slist* list; /* and the header lines it sends */
    struct request* queued;  /* the next request waiting for the transfer thread */
    int redirects;           /* on iOS, how many it has followed */
#endif
    int started; /* 1 on a thread of its own, 2 on the transfer thread */
} request;

static void request_free(request* r) {
    free(r->url), free(r->host), free(r->target), free(r->head), free(r->body);
    free(r->raw.p), free(r->lines.p), free(r->data.p);
    free(r);
}

/* --- the tables of requests and WebSockets ------------------------------------------------- */

/* Each kind has a table of its own, so a handle to one is never taken for the other. */
typedef struct {
    void* p;
    cx_u32 gen;
} slot;

typedef struct {
    slot* slots;
    cx_u32 count;
} table;

static table requests, sockets;
#ifdef _WIN32
static SRWLOCK table_lock = SRWLOCK_INIT;
static void lock(void) { AcquireSRWLockExclusive(&table_lock); }
static void unlock(void) { ReleaseSRWLockExclusive(&table_lock); }
#else
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&table_lock); }
static void unlock(void) { pthread_mutex_unlock(&table_lock); }
#endif

static cx_u64 table_add(table* t, void* p) {
    lock();
    cx_u32 i = 0;
    while (i < t->count && t->slots[i].p) i++;
    if (i == t->count) {
        cx_u32 n = t->count ? t->count * 2 : 16;
        slot* s = realloc(t->slots, n * sizeof(slot));
        if (!s) cx_panic_capacity();
        memset(s + t->count, 0, (n - t->count) * sizeof(slot));
        t->slots = s, t->count = n;
    }
    t->slots[i].p = p;
    cx_u64 h = (cx_u64)t->slots[i].gen << 32 | (i + 1);
    unlock();
    return h;
}

static void* table_find(table* t, cx_u64 h) {
    cx_u32 i = (cx_u32)h - 1, gen = (cx_u32)(h >> 32);
    lock();
    void* p = i < t->count && t->slots[i].gen == gen ? t->slots[i].p : NULL;
    unlock();
    return p;
}

static void table_remove(table* t, cx_u64 h) {
    cx_u32 i = (cx_u32)h - 1, gen = (cx_u32)(h >> 32);
    lock();
    if (i < t->count && t->slots[i].gen == gen && t->slots[i].p) {
        t->slots[i].p = NULL;
        t->slots[i].gen++;
    }
    unlock();
}

static request* request_of(cx_u64 h) { return table_find(&requests, h); }

/* --- URLs ---------------------------------------------------------------------------------- */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int starts_ci(const char* s, size_t n, const char* t) {
    size_t k = strlen(t);
    if (n < k) return 0;
    for (size_t i = 0; i < k; i++) {
        if (lower((unsigned char)s[i]) != t[i]) return 0;
    }
    return 1;
}

static char* dup_text(const char* s, size_t n) {
    char* p = malloc(n + 1);
    if (!p) cx_panic_capacity();
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* "http://" or "https://", a host, a port if it isn't the scheme's, and the rest. Only printable
 * ASCII, so the caller encodes anything else; no user and password before the host; and the
 * fragment, after '#', isn't sent, as browsers and Java don't send it. */
static int parse_url(request* r, const char* u, size_t n) {
    for (size_t k = 0; k < n; k++) {
        if ((unsigned char)u[k] <= 0x20 || (unsigned char)u[k] >= 0x7f) return 0;
    }
    size_t i;
    if (starts_ci(u, n, "https://")) r->https = 1, i = 8;
    else if (starts_ci(u, n, "http://")) r->https = 0, i = 7;
    else return 0;
    size_t end = 0;
    while (end < n && u[end] != '#') end++;
    size_t hs = i, he;
    if (i < end && u[i] == '[') {
        while (i < end && u[i] != ']') i++;
        if (i == end) return 0;
        hs++, he = i++;
    } else {
        while (i < end && u[i] != ':' && u[i] != '/' && u[i] != '?') i++;
        he = i;
    }
    if (he == hs) return 0;
    for (size_t k = hs; k < he; k++) {
        if (u[k] == '@') return 0;
    }
    r->port = r->https ? 443 : 80;
    if (i < end && u[i] == ':') {
        i++;
        size_t ds = i;
        unsigned port = 0;
        while (i < end && u[i] >= '0' && u[i] <= '9' && i - ds < 5) port = port * 10 + (unsigned)(u[i++] - '0');
        if (i > ds) {
            if (port == 0 || port > 65535) return 0;
            r->port = port;
        }
    }
    if (i < end && u[i] != '/' && u[i] != '?') return 0;
    r->host = dup_text(u + hs, he - hs);
    if (i == end || u[i] == '?') {
        r->target = malloc(end - i + 2);
        if (!r->target) cx_panic_capacity();
        r->target[0] = '/';
        memcpy(r->target + 1, u + i, end - i);
        r->target[end - i + 1] = 0;
    } else {
        r->target = dup_text(u + i, end - i);
    }
    r->url = dup_text(u, end);
    return 1;
}

/* Whether the caller's header lines name `name`. */
static int has_header(const char* head, const char* name) {
    size_t k = strlen(name);
    for (const char* line = head; *line;) {
        if (starts_ci(line, strlen(line), name) && line[k] == ':') return 1;
        const char* next = strstr(line, "\r\n");
        if (!next) break;
        line = next + 2;
    }
    return 0;
}

/* The Content-Length the response's header lines in `raw` give, plus 1, or 0 if they give none. */
static cx_u64 content_length(const bytes* raw) {
    const char* p = raw->p;
    size_t n = raw->len, i = 0;
    while (i < n) {
        size_t s = i;
        while (i < n && p[i] != '\n') i++;
        if (starts_ci(p + s, i - s, "content-length:")) {
            size_t k = s + 15;
            while (k < i && (p[k] == ' ' || p[k] == '\t')) k++;
            cx_u64 v = 0;
            int digits = 0;
            while (k < i && p[k] >= '0' && p[k] <= '9' && digits < 19) v = v * 10 + (cx_u64)(p[k++] - '0'), digits++;
            return digits ? v + 1 : 0;
        }
        i++;
    }
    return 0;
}

/* Says how much of the body has come, for a progress bar: with the Content-Length the first
 * time. */
static void note_progress(request* r) {
    if (!r->sized) {
        r->sized = 1;
        __atomic_store_n(&r->expected, content_length(&r->raw), __ATOMIC_RELAXED);
    }
    __atomic_store_n(&r->received, (cx_u64)r->data.len, __ATOMIC_RELAXED);
}

/* The header lines from `raw` as "name:value\r\n" in UTF-8: the status line dropped, each value
 * trimmed, and lines with no name dropped. With `latin1` the bytes are Latin-1, and otherwise
 * UTF-8 already. */
static int tidy_headers(request* r, int latin1) {
    const char* p = r->raw.p;
    size_t n = r->raw.len, i = 0;
    while (i < n) {
        size_t s = i;
        while (i < n && p[i] != '\n') i++;
        size_t e = i++;
        if (e > s && p[e - 1] == '\r') e--;
        if (e - s >= 5 && memcmp(p + s, "HTTP/", 5) == 0) continue;
        size_t c = s;
        while (c < e && p[c] != ':') c++;
        if (c == s || c == e) continue;
        size_t vs = c + 1, ve = e;
        while (vs < ve && (p[vs] == ' ' || p[vs] == '\t')) vs++;
        while (ve > vs && (p[ve - 1] == ' ' || p[ve - 1] == '\t')) ve--;
        for (size_t k = s; k < ve; k++) {
            if (k == c + 1) k = vs;
            if (k == ve) break;
            unsigned char b = (unsigned char)p[k];
            if (latin1 && b >= 0x80) {
                char two[2] = {(char)(0xC0 | b >> 6), (char)(0x80 | (b & 0x3F))};
                if (!add(&r->lines, two, 2)) return 0;
            } else if (!add(&r->lines, &p[k], 1)) {
                return 0;
            }
        }
        if (!add(&r->lines, "\r\n", 2)) return 0;
    }
    return 1;
}

/* The HTTP of a status line, "HTTP/1.1 200 OK" or "HTTP/2 200": 10, 11 or 20, or 0. */
static cx_u32 line_version(const char* p, size_t n) {
    if (n < 6 || memcmp(p, "HTTP/", 5) != 0 || p[5] < '0' || p[5] > '9') return 0;
    cx_u32 v = (cx_u32)(p[5] - '0') * 10;
    if (n >= 8 && p[6] == '.' && p[7] >= '0' && p[7] <= '9') v += (cx_u32)(p[7] - '0');
    return v;
}

/* --- Windows: WinHTTP ---------------------------------------------------------------------- */

#ifdef _WIN32

/* HTTP/2 and the request's times, from Windows 10 1607's and later's WinHTTP, numbered here for
 * SDKs older than them. */
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#define WINHTTP_OPTION_HTTP_PROTOCOL_USED 134
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif
#ifndef WINHTTP_OPTION_REQUEST_TIMES
#define WINHTTP_OPTION_REQUEST_TIMES 142
#endif
#ifndef WINHTTP_OPTION_HTTP_PROTOCOL_REQUIRED
#define WINHTTP_OPTION_HTTP_PROTOCOL_REQUIRED 145
#endif
#ifndef ERROR_WINHTTP_HTTP_PROTOCOL_MISMATCH
#define ERROR_WINHTTP_HTTP_PROTOCOL_MISMATCH (WINHTTP_ERROR_BASE + 190)
#endif

/* WINHTTP_REQUEST_TIMES: performance-counter ticks, at the points WINHTTP_REQUEST_TIME_ENTRY
 * numbers, of which these are used. A step the request didn't take, as on a connection that was
 * open already, keeps the time of the request that took it. */
typedef struct {
    ULONG count;
    ULONGLONG at[64];
} request_times;
enum { W_ACQUIRE = 2, W_RESOLVED = 6, W_CONNECTED = 8, W_TLS_1 = 10, W_TLS_2 = 12, W_TLS_3 = 14, W_HEADERS = 25, W_ENTRIES = 36 };

static HINTERNET session;
static cx_u32 session_state;

static void session_open(void) {
    session = WinHttpOpen(L"Calx", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
}

/* Hosts that HTTP/2 requests go to over TLS. WinHTTP shares kept connections between all of a
 * program's sessions, and a request that offers HTTP/2 goes over a kept HTTP/1.1 connection to
 * the host if there is one, unless it requires HTTP/2. So it requires it, unless the host is
 * known not to take it: a server that doesn't fails it with ERROR_WINHTTP_HTTP_PROTOCOL_MISMATCH,
 * and it's made again in HTTP/1.1. And WinHTTP shares a connection between requests only once
 * it's open, so while one request is opening an HTTP/2 connection to a host, the others wait to go
 * over it, as libcurl's CURLOPT_PIPEWAIT has them wait, rather than each open their own. A host is
 * free, with no request opening one; opening; or open, once a request to it has been answered,
 * when the requests after go at once. */
enum { HOST_FREE, HOST_OPENING, HOST_OPEN };
typedef struct host_state {
    struct host_state* next;
    int state;
    int h1; /* known not to take HTTP/2 */
    unsigned port;
    char name[];
} host_state;
static host_state* hosts;
static SRWLOCK hosts_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE hosts_cond = CONDITION_VARIABLE_INIT;
static DWORD left(ULONGLONG deadline);

/* The request's host, kept for as long as the program runs. Waits, until the deadline, while
 * another request is opening a connection to it, and says whether this one is to open it. */
static host_state* host_wait(request* r, ULONGLONG deadline, int* opening) {
    AcquireSRWLockExclusive(&hosts_lock);
    host_state* h = hosts;
    while (h && (h->port != r->port || strcmp(h->name, r->host) != 0)) h = h->next;
    if (!h) {
        size_t n = strlen(r->host);
        h = calloc(1, sizeof(host_state) + n + 1);
        if (!h) cx_panic_capacity();
        memcpy(h->name, r->host, n + 1);
        h->port = r->port;
        h->next = hosts;
        hosts = h;
    }
    while (!h->h1 && h->state == HOST_OPENING && GetTickCount64() < deadline) {
        SleepConditionVariableSRW(&hosts_cond, &hosts_lock, left(deadline), 0);
    }
    *opening = !h->h1 && h->state == HOST_FREE;
    if (*opening) h->state = HOST_OPENING;
    ReleaseSRWLockExclusive(&hosts_lock);
    return h;
}

static int host_h1(host_state* h) {
    AcquireSRWLockShared(&hosts_lock);
    int h1 = h->h1;
    ReleaseSRWLockShared(&hosts_lock);
    return h1;
}

/* Says the connection the request that's opening one to the host opened has answered, or that it
 * failed, so another may open one. */
static void host_release(host_state* h, int answered) {
    AcquireSRWLockExclusive(&hosts_lock);
    h->state = answered ? HOST_OPEN : HOST_FREE;
    WakeAllConditionVariable(&hosts_cond);
    ReleaseSRWLockExclusive(&hosts_lock);
}

/* Says the server doesn't take HTTP/2, so requests to it go at once, in HTTP/1.1. */
static void host_mark_h1(host_state* h) {
    AcquireSRWLockExclusive(&hosts_lock);
    h->h1 = 1;
    WakeAllConditionVariable(&hosts_cond);
    ReleaseSRWLockExclusive(&hosts_lock);
}

static wchar_t* widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = malloc((size_t)(n > 0 ? n : 1) * sizeof(wchar_t));
    if (!w) cx_panic_capacity();
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}

static int win_error(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return E_BAD_URL;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return E_RESOLVE;
    case ERROR_WINHTTP_CANNOT_CONNECT: return E_CONNECT;
    case ERROR_WINHTTP_TIMEOUT: return E_TIMEOUT;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED: return E_TLS;
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
    case ERROR_WINHTTP_REDIRECT_FAILED:
    case ERROR_WINHTTP_HEADER_SIZE_OVERFLOW:
    case ERROR_WINHTTP_HTTP_PROTOCOL_MISMATCH:
    case ERROR_WINHTTP_RESPONSE_DRAIN_OVERFLOW: return E_PROTOCOL;
    default: return E_OS;
    }
}

/* What's left of the time, at least 1 ms, since WinHTTP takes 0 as no limit. */
static DWORD left(ULONGLONG deadline) {
    ULONGLONG now = GetTickCount64();
    return now + 1 >= deadline ? 1 : (DWORD)(deadline - now);
}

/* `d` performance-counter ticks, at `f` a second, in nanoseconds. */
static cx_u64 tick_ns(LONGLONG d, LONGLONG f) {
    if (d <= 0) return 0;
    return (cx_u64)(d / f) * 1000000000u + (cx_u64)(d % f) * 1000000000u / (cx_u64)f;
}

/* The request's times from WinHTTP, from `t0`, when it started, and whether its connection was
 * open already: then it was made before the request asked for one. A WinHTTP without them gives
 * the total alone. */
static void win_times(request* r, HINTERNET q, LONGLONG t0) {
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    r->times[T_TOTAL] = tick_ns(now.QuadPart - t0, f.QuadPart);
    request_times t;
    memset(&t, 0, sizeof t);
    t.count = W_ENTRIES;
    DWORD n = sizeof t;
    if (!WinHttpQueryOption(q, WINHTTP_OPTION_REQUEST_TIMES, &t, &n) || t.count <= W_HEADERS) return;
    LONGLONG at[W_ENTRIES];
    for (int i = 0; i < W_ENTRIES; i++) at[i] = (LONGLONG)t.at[i] - t0;
    r->reused = at[W_CONNECTED] <= (at[W_ACQUIRE] > 0 ? at[W_ACQUIRE] : 0);
    r->times[T_RESOLVE] = tick_ns(at[W_RESOLVED], f.QuadPart);
    r->times[T_FIRST_BYTE] = tick_ns(at[W_HEADERS], f.QuadPart);
    if (r->reused) return;
    r->times[T_CONNECT] = tick_ns(at[W_CONNECTED], f.QuadPart);
    if (r->https) {
        LONGLONG tls = at[W_TLS_1];
        if (at[W_TLS_2] > tls) tls = at[W_TLS_2];
        if (at[W_TLS_3] > tls) tls = at[W_TLS_3];
        r->times[T_TLS] = tick_ns(tls, f.QuadPart);
    }
}

/* One try at the request, from `t0`, its start, requiring HTTP/2 if `require_h2`. Gives 1 if the
 * server doesn't take HTTP/2 and the request may have HTTP/1.1, to be made again. If `*opener`,
 * this request is opening the connection to that host, and says so once it's answered. */
static int attempt(request* r, ULONGLONG deadline, LONGLONG t0, int require_h2, host_state** opener) {
    wchar_t* host = widen(r->host);
    wchar_t* target = widen(r->target);
    wchar_t* head = widen(r->head);
    HINTERNET c = NULL, q = NULL;
    DWORD e = 0;
    int again = 0;
    c = WinHttpConnect(session, host, (INTERNET_PORT)r->port, 0);
    if (!c) goto failed;
    q = WinHttpOpenRequest(c, r->post ? L"POST" : L"GET", target, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                           r->https ? WINHTTP_FLAG_SECURE : 0);
    if (!q) goto failed;
    if (r->version != V_HTTP1) {
        /* Offered in the TLS handshake, so it's HTTP/1.1 on http://. Windows 10 1607 and later
         * have it. Where WinHTTP can't require it, HTTP/2 alone is checked when the response
         * comes. */
        DWORD flag = WINHTTP_PROTOCOL_FLAG_HTTP2, yes = TRUE;
        if (!WinHttpSetOption(q, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &flag, sizeof flag) && r->version == V_HTTP2_ONLY) {
            r->error = E_UNAVAILABLE;
            goto done;
        }
        if (require_h2) WinHttpSetOption(q, WINHTTP_OPTION_HTTP_PROTOCOL_REQUIRED, &yes, sizeof yes);
    }
    int t = (int)left(deadline);
    if (!WinHttpSetTimeouts(q, t, t, t, t)) goto failed;
    DWORD ms = left(deadline);
    if (!WinHttpSetOption(q, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &ms, sizeof ms)) goto failed;
    if (!WinHttpSendRequest(q, head[0] ? head : WINHTTP_NO_ADDITIONAL_HEADERS, head[0] ? (DWORD)-1L : 0, r->body,
                            (DWORD)r->body_len, (DWORD)r->body_len, 0)) {
        goto failed;
    }
    ms = left(deadline);
    WinHttpSetOption(q, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &ms, sizeof ms);
    WinHttpSetOption(q, WINHTTP_OPTION_RECEIVE_TIMEOUT, &ms, sizeof ms);
    if (!WinHttpReceiveResponse(q, NULL)) goto failed;
    if (*opener) {
        host_release(*opener, 1);
        *opener = NULL;
    }
    /* WinHTTP writes an HTTP/2 response's status line as HTTP/1.1's, so the protocol it used
     * says which it was. */
    DWORD used = 0, size = sizeof used;
    if (WinHttpQueryOption(q, WINHTTP_OPTION_HTTP_PROTOCOL_USED, &used, &size) && (used & WINHTTP_PROTOCOL_FLAG_HTTP2)) {
        r->used = 20;
    } else {
        wchar_t version[16] = {0};
        size = sizeof version - sizeof(wchar_t);
        r->used = 11;
        if (WinHttpQueryHeaders(q, WINHTTP_QUERY_VERSION, WINHTTP_HEADER_NAME_BY_INDEX, version, &size, WINHTTP_NO_HEADER_INDEX) &&
            wcscmp(version, L"HTTP/1.0") == 0) {
            r->used = 10;
        }
    }
    if (r->version == V_HTTP2_ONLY && r->used != 20) {
        r->error = E_PROTOCOL;
        goto done;
    }
    DWORD status = 0;
    size = sizeof status;
    if (!WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                             &size, WINHTTP_NO_HEADER_INDEX)) {
        goto failed;
    }
    r->status = status;
    size = 0;
    WinHttpQueryHeaders(q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && size) {
        wchar_t* w = malloc(size + sizeof(wchar_t));
        if (!w) cx_panic_capacity();
        if (WinHttpQueryHeaders(q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, w, &size, WINHTTP_NO_HEADER_INDEX)) {
            /* WinHTTP gives each byte of the headers as the UTF-16 unit of the same number, so
             * this is the Latin-1 reading, in UTF-8. */
            int wn = (int)(size / sizeof(wchar_t));
            int n = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
            if (n > 0 && grow(&r->raw, (size_t)n)) {
                WideCharToMultiByte(CP_UTF8, 0, w, wn, r->raw.p, n, NULL, NULL);
                r->raw.len = (size_t)n;
            }
        }
        free(w);
    }
    if (!tidy_headers(r, 0)) cx_panic_capacity();
    note_progress(r);
    for (;;) {
        if (GetTickCount64() >= deadline) {
            r->error = E_TIMEOUT;
            break;
        }
        ms = left(deadline);
        WinHttpSetOption(q, WINHTTP_OPTION_RECEIVE_TIMEOUT, &ms, sizeof ms);
        size_t want = r->limit - r->data.len + 1;
        if (want > 65536) want = 65536;
        if (!grow(&r->data, want)) cx_panic_capacity();
        DWORD got = 0;
        if (!WinHttpReadData(q, r->data.p + r->data.len, (DWORD)want, &got)) goto failed;
        if (got == 0) break;
        r->data.len += got;
        note_progress(r);
        if (r->data.len > r->limit) {
            r->error = E_TOO_LONG;
            break;
        }
    }
    win_times(r, q, t0);
    goto done;
failed:
    e = GetLastError();
    if (e == ERROR_WINHTTP_HTTP_PROTOCOL_MISMATCH && r->version == V_HTTP2) {
        again = 1;
    } else {
        r->error = win_error(e), r->code = (cx_i32)e;
    }
done:
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    free(host), free(target), free(head);
    return again;
}

static void run(request* r) {
    once(&session_state, session_open);
    if (!session) {
        r->error = E_OS, r->code = (cx_i32)GetLastError();
        return;
    }
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    ULONGLONG deadline = GetTickCount64() + r->timeout_ms;
    /* Over http:// it's HTTP/1.1, with connections side by side. */
    int opening = 0;
    host_state* h = r->version != V_HTTP1 && r->https ? host_wait(r, deadline, &opening) : NULL;
    host_state* opener = opening ? h : NULL;
    int require = r->version == V_HTTP2_ONLY || (h && !host_h1(h));
    if (attempt(r, deadline, t0.QuadPart, require, &opener)) {
        host_mark_h1(h);
        if (opener) host_release(opener, 0);
        opener = NULL;
        attempt(r, deadline, t0.QuadPart, 0, &opener);
    }
    if (opener) host_release(opener, 0);
}

#elif defined(CX_IOS)

/* --- iOS: URLSession ----------------------------------------------------------------------- *
 *
 * iOS has no libcurl, so requests go through URLSession, the system's own client, which checks
 * certificates as the system does, and keeps connections and speaks HTTP/2 over TLS by itself
 * (CLX-410). This part of the file is Objective-C, which the compiler builds it as for iOS, and
 * links Foundation itself, so an app needs nothing more. Every request is a data task on one
 * session, whose delegate, on a queue of its own, gathers the response and finishes the request
 * as the transfer threads do on Linux and macOS: `done_cond` says when one has.
 *
 * URLSession decides some things itself. It always sends Accept, Accept-Language and
 * Accept-Encoding, so Accept-Encoding is `identity` unless the caller gives one, and the body
 * comes as the server sent it; the other two are URLSession's unless the caller gives them. It
 * picks HTTP/1.1 or HTTP/2 over TLS by ALPN for Http1 too, and HTTP/3 where a server offers it;
 * it has no h2c, so Http2Only on http:// is Unavailable, as on Windows. The headers come as
 * URLSession read them, one line for each name. */

__asm__(".linker_option \"-framework\", \"Foundation\"");

static pthread_mutex_t done_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cond = PTHREAD_COND_INITIALIZER;

static void transfer_done(request* r) {
    pthread_mutex_lock(&done_lock);
    __atomic_store_n(&r->state, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&done_cond);
    pthread_mutex_unlock(&done_lock);
}

/* What an NSURLErrorDomain code means to std.http. */
static int url_error(NSInteger e) {
    switch (e) {
    case NSURLErrorBadURL:
    case NSURLErrorUnsupportedURL: return E_BAD_URL;
    case NSURLErrorCannotFindHost:
    case NSURLErrorDNSLookupFailed: return E_RESOLVE;
    case NSURLErrorCannotConnectToHost:
    case NSURLErrorNotConnectedToInternet:
    case NSURLErrorInternationalRoamingOff:
    case NSURLErrorDataNotAllowed: return E_CONNECT;
    case NSURLErrorTimedOut: return E_TIMEOUT;
    case NSURLErrorSecureConnectionFailed:
    case NSURLErrorServerCertificateHasBadDate:
    case NSURLErrorServerCertificateUntrusted:
    case NSURLErrorServerCertificateHasUnknownRoot:
    case NSURLErrorServerCertificateNotYetValid:
    case NSURLErrorClientCertificateRejected:
    case NSURLErrorClientCertificateRequired: return E_TLS;
    case NSURLErrorDataLengthExceedsMaximum: return E_TOO_LONG;
    case NSURLErrorNetworkConnectionLost:
    case NSURLErrorBadServerResponse:
    case NSURLErrorCannotParseResponse:
    case NSURLErrorZeroByteResource:
    case NSURLErrorCannotDecodeRawData:
    case NSURLErrorCannotDecodeContentData:
    case NSURLErrorHTTPTooManyRedirects:
    case NSURLErrorRedirectToNonExistentLocation: return E_PROTOCOL;
    default: return E_OS;
    }
}

/* Nanoseconds from `start` to `at`, or 0 for a step that wasn't taken. */
static cx_u64 since(NSDate* start, NSDate* at) {
    if (!start || !at) return 0;
    NSTimeInterval s = [at timeIntervalSinceDate:start];
    return s > 0 ? (cx_u64)(s * 1e9) : 0;
}

@interface CxSessionDelegate : NSObject <NSURLSessionDataDelegate>
@end

/* Each running task's request, by the task's identifier, used only on the session's queue. */
static NSMutableDictionary* running_tasks;

static request* task_request(NSURLSessionTask* task) {
    NSValue* v = running_tasks[@(task.taskIdentifier)];
    return v ? (request*)v.pointerValue : NULL;
}

/* Ends the request with `error`, which the task's completion then keeps. */
static void task_fail(NSURLSessionTask* task, request* r, int error) {
    if (!r->error) r->error = error;
    [task cancel];
}

@implementation CxSessionDelegate

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))done {
    request* r = task_request(task);
    if (r && [response isKindOfClass:[NSHTTPURLResponse class]]) {
        NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
        r->status = (cx_u32)http.statusCode;
        r->raw.len = 0;
        for (NSString* name in http.allHeaderFields) {
            NSString* line = [NSString stringWithFormat:@"%@: %@\r\n", name, http.allHeaderFields[name]];
            const char* text = line.UTF8String;
            if (text && !add(&r->raw, text, strlen(text))) cx_panic_capacity();
        }
        long long length = response.expectedContentLength;
        r->sized = 1;
        __atomic_store_n(&r->expected, length >= 0 ? (cx_u64)length + 1 : 0, __ATOMIC_RELAXED);
    }
    done(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
    request* r = task_request(task);
    if (!r || r->error) return;
    __block int too_long = 0;
    [data enumerateByteRangesUsingBlock:^(const void* bytes_at, NSRange range, BOOL* stop) {
        if (r->data.len + range.length > r->limit) {
            too_long = 1;
            *stop = YES;
        } else if (!add(&r->data, bytes_at, range.length)) {
            cx_panic_capacity();
        }
    }];
    if (too_long) task_fail(task, r, E_TOO_LONG);
    else __atomic_store_n(&r->received, (cx_u64)r->data.len, __ATOMIC_RELAXED);
}

/* Up to 10 redirects, and never from https:// to http://, as libcurl and WinHTTP are told. */
- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)next
             completionHandler:(void (^)(NSURLRequest*))done {
    request* r = task_request(task);
    int downgrade = r && r->https && ![next.URL.scheme.lowercaseString isEqualToString:@"https"];
    if (r && (++r->redirects > 10 || downgrade)) {
        task_fail(task, r, E_PROTOCOL);
        done(nil);
        return;
    }
    done(next);
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didFinishCollectingMetrics:(NSURLSessionTaskMetrics*)metrics {
    request* r = task_request(task);
    NSURLSessionTaskTransactionMetrics* t = metrics.transactionMetrics.lastObject;
    if (!r || !t) return;
    NSString* protocol = t.networkProtocolName.lowercaseString;
    r->used = [protocol isEqualToString:@"h2"] ? 20 : [protocol isEqualToString:@"h3"] ? 30 : [protocol isEqualToString:@"http/1.0"] ? 10 : 11;
    r->reused = t.reusedConnection;
    NSDate* start = t.fetchStartDate;
    if (!r->reused) {
        r->times[T_RESOLVE] = since(start, t.domainLookupEndDate);
        r->times[T_CONNECT] = since(start, t.secureConnectionStartDate ? t.secureConnectionStartDate : t.connectEndDate);
        if (r->https) r->times[T_TLS] = since(start, t.secureConnectionEndDate);
    }
    r->times[T_FIRST_BYTE] = since(start, t.responseStartDate ? t.responseStartDate : t.responseEndDate);
    r->times[T_TOTAL] = since(start, t.responseEndDate);
    /* Each step ends no sooner than the one before, as the other systems give them, though
     * URLSession can leave a step out. */
    cx_u64 before = 0;
    for (int i = 0; i < T_COUNT; i++) {
        if (r->times[i] == 0) continue;
        if (r->times[i] < before) r->times[i] = before;
        before = r->times[i];
    }
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    request* r = task_request(task);
    if (!r) return;
    [running_tasks removeObjectForKey:@(task.taskIdentifier)];
    if (r->error) {
        /* a limit or a rule of this file's, which cancelled it */
    } else if (error) {
        int e = [error.domain isEqualToString:NSURLErrorDomain] ? url_error(error.code) : E_OS;
        r->error = e, r->code = (cx_i32)error.code;
    } else if (r->version == V_HTTP2_ONLY && r->used != 20) {
        r->error = E_PROTOCOL;
    } else if (!tidy_headers(r, 0)) {
        cx_panic_capacity();
    }
    transfer_done(r);
}

@end

static NSURLSession* http_session;
static NSOperationQueue* http_queue;
static cx_u32 session_state;

static void session_open(void) {
    @autoreleasepool {
        NSURLSessionConfiguration* c = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        /* No proxy, cookie, cache or credential store, as the other systems' clients have none. */
        c.connectionProxyDictionary = @{};
        c.HTTPCookieStorage = nil;
        c.HTTPShouldSetCookies = NO;
        c.URLCache = nil;
        c.URLCredentialStorage = nil;
        c.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        c.timeoutIntervalForRequest = 24 * 3600;
        c.timeoutIntervalForResource = 24 * 3600;
        c.HTTPMaximumConnectionsPerHost = 16;
        http_queue = [[NSOperationQueue alloc] init];
        http_queue.maxConcurrentOperationCount = 1;
        running_tasks = [[NSMutableDictionary alloc] init];
        http_session = [[NSURLSession sessionWithConfiguration:c delegate:[[[CxSessionDelegate alloc] init] autorelease]
                                                 delegateQueue:http_queue] retain];
    }
}

/* Starts the request on the session. Its whole time is `timeout_ms`, after which it's cancelled. */
static int transfer(request* r) {
    once(&session_state, session_open);
    r->started = 2;
    @autoreleasepool {
        NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:r->url]];
        if (!url) {
            r->error = E_BAD_URL;
            transfer_done(r);
            return 1;
        }
        NSMutableURLRequest* q = [NSMutableURLRequest requestWithURL:url];
        q.HTTPMethod = r->post ? @"POST" : @"GET";
        q.timeoutInterval = 24 * 3600;
        q.HTTPShouldHandleCookies = NO;
        if (r->post) q.HTTPBody = [NSData dataWithBytes:r->body ? (const void*)r->body : "" length:r->body_len];
        for (char* line = r->head; *line;) {
            char* next = strstr(line, "\r\n");
            if (!next) break;
            char* colon = memchr(line, ':', (size_t)(next - line));
            if (colon) {
                NSString* name = [[[NSString alloc] initWithBytes:line length:(NSUInteger)(colon - line) encoding:NSUTF8StringEncoding] autorelease];
                const char* v = colon + 1;
                while (v < next && (*v == ' ' || *v == '\t')) v++;
                NSString* value = [[[NSString alloc] initWithBytes:v length:(NSUInteger)(next - v) encoding:NSUTF8StringEncoding] autorelease];
                if (name && value) [q addValue:value forHTTPHeaderField:name];
            }
            line = next + 2;
        }
        if (!has_header(r->head, "user-agent")) [q setValue:@"Calx" forHTTPHeaderField:@"User-Agent"];
        if (!has_header(r->head, "accept-encoding")) [q setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
        NSURLSessionDataTask* task = [http_session dataTaskWithRequest:q];
        cx_u32 timeout_ms = r->timeout_ms;
        [http_queue addOperationWithBlock:^{
            running_tasks[@(task.taskIdentifier)] = [NSValue valueWithPointer:r];
            [task resume];
        }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ms * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            [http_queue addOperationWithBlock:^{
                request* still = task_request(task);
                if (still) task_fail(task, still, E_TIMEOUT);
            }];
        });
    }
    return 1;
}

/* Never reached on iOS, where every request goes to the session. */
static void run(request* r) {
    r->error = E_UNAVAILABLE;
}

#else

/* --- Linux and macOS: libcurl, loaded at run time ------------------------------------------ */

/* libcurl's options and infos, by the numbers its ABI gives them. */
enum {
    O_WRITEDATA = 10001, O_URL = 10002, O_PROXY = 10004, O_POSTFIELDS = 10015, O_USERAGENT = 10018,
    O_HTTPHEADER = 10023, O_HEADERDATA = 10029, O_PRIVATE = 10103, O_WRITEFUNCTION = 20011, O_HEADERFUNCTION = 20079,
    O_POST = 47, O_FOLLOWLOCATION = 52, O_MAXREDIRS = 68, O_HTTPGET = 80, O_HTTP_VERSION = 84,
    O_NOSIGNAL = 99, O_TIMEOUT_MS = 155, O_PROTOCOLS = 181, O_REDIR_PROTOCOLS = 182, O_PIPEWAIT = 237,
    O_POSTFIELDSIZE_LARGE = 30120, M_PIPELINING = 3,
    I_RESPONSE_CODE = 0x200002, I_NUM_CONNECTS = 0x200000 + 26, I_PRIVATE = 0x100000 + 21,
    /* In microseconds, from libcurl 7.61; before, in seconds, as doubles. */
    I_NAMELOOKUP_T = 0x600000 + 51, I_CONNECT_T = 0x600000 + 52, I_STARTTRANSFER_T = 0x600000 + 54, I_TOTAL_T = 0x600000 + 50,
    I_APPCONNECT_T = 0x600000 + 56, I_NAMELOOKUP = 0x300000 + 4, I_CONNECT = 0x300000 + 5, I_STARTTRANSFER = 0x300000 + 17,
    I_TOTAL = 0x300000 + 3, I_APPCONNECT = 0x300000 + 33
};
/* CURL_HTTP_VERSION_1_1, _2TLS and _2_PRIOR_KNOWLEDGE; CURLPIPE_MULTIPLEX; CURL_VERSION_HTTP2. */
enum { CV_1_1 = 2, CV_2TLS = 4, CV_2_PRIOR = 5, PIPE_MULTIPLEX = 2, F_HTTP2 = 1 << 16 };

/* curl_waitfd and CURLMsg, as multi.h lays them out. */
typedef struct {
    int fd;
    short events, revents;
} curl_waitfd;
typedef struct {
    int msg; /* 1, CURLMSG_DONE: the transfer finished */
    void* easy;
    union {
        void* whatever;
        int result;
    } data;
} curl_msg;
/* The start of curl_version_info_data, which every libcurl has. */
typedef struct {
    int age;
    const char* version;
    unsigned version_num;
    const char* host;
    int features;
} curl_version;

typedef struct curl_slist curl_slist;
static struct {
    void* (*easy_init)(void);
    int (*easy_setopt)(void*, int, ...);
    int (*easy_perform)(void*);
    int (*easy_getinfo)(void*, int, ...);
    void (*easy_cleanup)(void*);
    curl_slist* (*slist_append)(curl_slist*, const char*);
    void (*slist_free_all)(curl_slist*);
    int (*global_init)(long);
    /* For WebSockets, on a connection made in connect-only mode; NULL in a libcurl without them,
     * older than 7.18.2. */
    int (*easy_send)(void*, const void*, size_t, size_t*);
    int (*easy_recv)(void*, void*, size_t, size_t*);
    /* For the transfer thread, which every libcurl since 7.28 has. */
    void* (*multi_init)(void);
    int (*multi_setopt)(void*, int, ...);
    int (*multi_add_handle)(void*, void*);
    int (*multi_remove_handle)(void*, void*);
    int (*multi_perform)(void*, int*);
    int (*multi_wait)(void*, curl_waitfd*, unsigned, int, int*);
    curl_msg* (*multi_info_read)(void*, int*);
    curl_version* (*version_info)(int);
} curl;
static int curl_loaded;
static int curl_http2; /* whether it's built with HTTP/2, nghttp2 */
static cx_u32 curl_state;

static void curl_load(void) {
#ifdef __APPLE__
    /* Every macOS has it, in the shared cache, which dlopen finds by this path. */
    const char* names[] = {"/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib"};
#else
    const char* names[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
#endif
    void* lib = NULL;
    for (int i = 0; i < 3 && !lib; i++) lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
    void** fns[] = {(void**)&curl.easy_init, (void**)&curl.easy_setopt, (void**)&curl.easy_perform, (void**)&curl.easy_getinfo,
                    (void**)&curl.easy_cleanup, (void**)&curl.slist_append, (void**)&curl.slist_free_all, (void**)&curl.global_init};
    const char* syms[] = {"curl_easy_init", "curl_easy_setopt", "curl_easy_perform", "curl_easy_getinfo",
                          "curl_easy_cleanup", "curl_slist_append", "curl_slist_free_all", "curl_global_init"};
    for (int i = 0; i < 8; i++) {
        *fns[i] = dlsym(lib, syms[i]);
        if (!*fns[i]) return;
    }
    *(void**)&curl.easy_send = dlsym(lib, "curl_easy_send");
    *(void**)&curl.easy_recv = dlsym(lib, "curl_easy_recv");
    void** multi[] = {(void**)&curl.multi_init, (void**)&curl.multi_setopt, (void**)&curl.multi_add_handle,
                      (void**)&curl.multi_remove_handle, (void**)&curl.multi_perform, (void**)&curl.multi_wait,
                      (void**)&curl.multi_info_read};
    const char* multi_syms[] = {"curl_multi_init", "curl_multi_setopt", "curl_multi_add_handle", "curl_multi_remove_handle",
                                "curl_multi_perform", "curl_multi_wait", "curl_multi_info_read"};
    int all = 1;
    for (int i = 0; i < 7; i++) {
        *multi[i] = dlsym(lib, multi_syms[i]);
        all = all && *multi[i];
    }
    if (!all) curl.multi_init = NULL;
    *(void**)&curl.version_info = dlsym(lib, "curl_version_info");
    if (curl.version_info) {
        curl_version* v = curl.version_info(0);
        curl_http2 = v && (v->features & F_HTTP2) != 0;
    }
    /* Not safe to call while another thread uses libcurl, so it's done here, once, before any
     * request's thread starts. A write to a closed TLS connection would raise SIGPIPE, which a
     * server never wants. A program ignores it from the start (see `cx_start`), so this is for a
     * static library, whose host may not. */
    if (curl.global_init(3) != 0) return;
    struct sigaction sa;
    if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) signal(SIGPIPE, SIG_IGN);
    curl_loaded = 1;
}

static int curl_error(int e) {
    switch (e) {
    case 3: return E_BAD_URL;
    /* CURLE_UNSUPPORTED_PROTOCOL: the URLs that get here are http:// or https://, so it's the
     * server's answer that isn't HTTP/1.x ("Received HTTP/0.9 when not allowed"), as an
     * HTTP/2-only server's is, or a redirect to another scheme. WinHTTP calls both
     * ERROR_WINHTTP_INVALID_SERVER_RESPONSE and ERROR_WINHTTP_REDIRECT_FAILED. */
    case 1: return E_PROTOCOL;
    case 6: return E_RESOLVE;
    case 7: return E_CONNECT;
    case 28: return E_TIMEOUT;
    case 35:
    case 53:
    case 54:
    case 58:
    case 59:
    case 60:
    case 64:
    case 66:
    case 77:
    case 80:
    case 82:
    case 83:
    case 90:
    case 91:
    case 98: return E_TLS;
    case 8:
    case 16:
    case 18:
    case 47:
    case 52:
    case 55:
    case 56:
    case 61:
    case 92:
    case 95: return E_PROTOCOL;
    default: return E_OS;
    }
}

static size_t on_body(char* p, size_t size, size_t n, void* user) {
    request* r = user;
    size_t k = size * n;
    if (r->data.len + k > r->limit) {
        r->error = E_TOO_LONG;
        return 0;
    }
    if (!add(&r->data, p, k)) return 0;
    note_progress(r);
    return k;
}

static size_t on_header(char* p, size_t size, size_t n, void* user) {
    request* r = user;
    size_t k = size * n;
    /* Each response's status line starts its headers: a redirect's, or a 100 Continue's, are
     * dropped for the next. */
    if (k >= 5 && memcmp(p, "HTTP/", 5) == 0) {
        r->raw.len = 0;
        /* A server that took HTTP/1.1 in the TLS handshake, when the request wants HTTP/2 or
         * nothing: libcurl has no way to offer HTTP/2 alone, as WinHTTP has. */
        if (r->version == V_HTTP2_ONLY && line_version(p, k) != 20) {
            r->error = E_PROTOCOL;
            return 0;
        }
    }
    return add(&r->raw, p, k) ? k : 0;
}

/* One of libcurl's times, in nanoseconds: in microseconds from 7.61, or else in seconds. */
static cx_u64 curl_time(void* h, int us_info, int s_info) {
    long long us = 0;
    if (curl.easy_getinfo(h, us_info, &us) == 0) return us > 0 ? (cx_u64)us * 1000 : 0;
    double s = 0;
    if (curl.easy_getinfo(h, s_info, &s) == 0 && s > 0) return (cx_u64)(s * 1e9);
    return 0;
}

/* A libcurl handle set up for the request, with its header lines in `r->list`, or NULL. */
static void* curl_begin(request* r) {
    void* h = curl.easy_init();
    if (!h) return NULL;
    curl_slist* list = NULL;
    for (char* line = r->head; *line;) {
        char* next = strstr(line, "\r\n");
        if (!next) break;
        *next = 0;
        list = curl.slist_append(list, line);
        *next = '\r';
        line = next + 2;
    }
    /* What libcurl adds on its own, which WinHTTP doesn't, taken off unless the caller gave it. */
    if (!has_header(r->head, "accept")) list = curl.slist_append(list, "Accept:");
    if (!has_header(r->head, "expect")) list = curl.slist_append(list, "Expect:");
    if (r->post && !has_header(r->head, "content-type")) list = curl.slist_append(list, "Content-Type:");
    curl.easy_setopt(h, O_NOSIGNAL, 1L);
    curl.easy_setopt(h, O_URL, r->url);
    curl.easy_setopt(h, O_PROXY, "");
    curl.easy_setopt(h, O_USERAGENT, "Calx");
    /* HTTP/2 needs a libcurl built with nghttp2. Without it, a request that may have HTTP/1.1
     * has it, and one that wants HTTP/2 alone never gets here. */
    long version = r->version == V_HTTP1 || !curl_http2 ? CV_1_1 : r->version == V_HTTP2 ? CV_2TLS : CV_2_PRIOR;
    curl.easy_setopt(h, O_HTTP_VERSION, version);
    /* An HTTP/2 request to a host that a request before it is connecting to waits to go over
     * that connection, rather than open another. */
    if (version != CV_1_1) curl.easy_setopt(h, O_PIPEWAIT, 1L);
    curl.easy_setopt(h, O_PROTOCOLS, 3L);
    curl.easy_setopt(h, O_REDIR_PROTOCOLS, r->https ? 2L : 3L);
    curl.easy_setopt(h, O_FOLLOWLOCATION, 1L);
    curl.easy_setopt(h, O_MAXREDIRS, 10L);
    curl.easy_setopt(h, O_TIMEOUT_MS, (long)r->timeout_ms);
    curl.easy_setopt(h, O_HTTPHEADER, list);
    curl.easy_setopt(h, O_WRITEFUNCTION, on_body);
    curl.easy_setopt(h, O_WRITEDATA, r);
    curl.easy_setopt(h, O_HEADERFUNCTION, on_header);
    curl.easy_setopt(h, O_HEADERDATA, r);
    if (r->post) {
        curl.easy_setopt(h, O_POST, 1L);
        curl.easy_setopt(h, O_POSTFIELDSIZE_LARGE, (long long)r->body_len);
        curl.easy_setopt(h, O_POSTFIELDS, r->body ? (const char*)r->body : "");
    } else {
        curl.easy_setopt(h, O_HTTPGET, 1L);
    }
    curl.easy_setopt(h, O_PRIVATE, r);
    r->list = list;
    return h;
}

/* Takes what the finished transfer gave, `e` its CURLcode, and lets the handle go. */
static void curl_end(request* r, void* h, int e) {
    if (r->error) {
        /* on_body's or on_header's, which ended the transfer */
    } else if (e != 0) {
        r->error = curl_error(e), r->code = e;
    } else {
        long status = 0;
        curl.easy_getinfo(h, I_RESPONSE_CODE, &status);
        r->status = (cx_u32)status;
        r->used = line_version(r->raw.p, r->raw.len);
        long connects = 1;
        curl.easy_getinfo(h, I_NUM_CONNECTS, &connects);
        r->reused = connects == 0;
        if (!r->reused) {
            r->times[T_RESOLVE] = curl_time(h, I_NAMELOOKUP_T, I_NAMELOOKUP);
            r->times[T_CONNECT] = curl_time(h, I_CONNECT_T, I_CONNECT);
            if (r->https) r->times[T_TLS] = curl_time(h, I_APPCONNECT_T, I_APPCONNECT);
        }
        r->times[T_FIRST_BYTE] = curl_time(h, I_STARTTRANSFER_T, I_STARTTRANSFER);
        r->times[T_TOTAL] = curl_time(h, I_TOTAL_T, I_TOTAL);
        if (!tidy_headers(r, 1)) cx_panic_capacity();
    }
    curl.easy_cleanup(h);
    curl.slist_free_all(r->list);
    r->list = NULL;
}

/* A request on a thread of its own, with a libcurl too old for the transfer thread. */
static void run(request* r) {
    void* h = curl_begin(r);
    if (!h) {
        r->error = E_OS, r->code = ENOMEM;
        return;
    }
    curl_end(r, h, curl.easy_perform(h));
}

/* --- Linux and macOS: the transfer threads ------------------------------------------------- *
 *
 * Requests run together on a thread, on a libcurl multi handle, so a connection a request opens is
 * kept for the next one to the same host, as WinHTTP's session keeps it, and HTTP/2 requests to a
 * host go over one connection at once (CLX-383). There are three, each with its own connections:
 * one for HTTP/1.1 requests (and `Http2` ones over http://, which ALPN can't offer h2 on), one for
 * `Http2` over TLS, and one for `Http2Only`, so that an HTTP/2 request never goes over a kept
 * HTTP/1.1 connection: to a server that would have taken HTTP/2, or, with libcurl 8.5, as h2c's
 * prior knowledge sent down a connection that spoke HTTP/1.1. A thread does every libcurl
 * call on its requests' handles. `cx_http_start` puts a request in its queue, and a byte down a
 * pipe wakes it; it says a request has finished by `done_cond`. */

typedef struct {
    void* multi;
    int running;
    cx_u32 state;
    int wake[2];
    request* head;
    request** tail;
    pthread_mutex_t lock;
} pool;

static pool pools[3] = {{NULL, 0, 0, {-1, -1}, NULL, &pools[0].head, PTHREAD_MUTEX_INITIALIZER},
                        {NULL, 0, 0, {-1, -1}, NULL, &pools[1].head, PTHREAD_MUTEX_INITIALIZER},
                        {NULL, 0, 0, {-1, -1}, NULL, &pools[2].head, PTHREAD_MUTEX_INITIALIZER}};
static pthread_mutex_t done_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cond = PTHREAD_COND_INITIALIZER;

static void transfer_done(request* r) {
    pthread_mutex_lock(&done_lock);
    __atomic_store_n(&r->state, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&done_cond);
    pthread_mutex_unlock(&done_lock);
}

static void* transfers(void* arg) {
    pool* p = arg;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        request* r = p->head;
        p->head = NULL, p->tail = &p->head;
        pthread_mutex_unlock(&p->lock);
        while (r) {
            request* next = r->queued;
            r->easy = curl_begin(r);
            int e = r->easy ? curl.multi_add_handle(p->multi, r->easy) : 0;
            if (!r->easy || e != 0) {
                if (r->easy) curl.easy_cleanup(r->easy);
                curl.slist_free_all(r->list);
                r->list = NULL;
                r->error = E_OS, r->code = r->easy ? e : ENOMEM;
                transfer_done(r);
            }
            r = next;
        }
        int running = 0, left = 0;
        curl.multi_perform(p->multi, &running);
        curl_msg* m;
        while ((m = curl.multi_info_read(p->multi, &left))) {
            if (m->msg != 1) continue;
            void* h = m->easy;
            int e = m->data.result;
            request* q = NULL;
            curl.easy_getinfo(h, I_PRIVATE, &q);
            curl.multi_remove_handle(p->multi, h);
            if (q) {
                curl_end(q, h, e);
                transfer_done(q);
            }
        }
        curl_waitfd wake = {p->wake[0], POLLIN, 0};
        curl.multi_wait(p->multi, &wake, 1, 1000, NULL);
        char drain[64];
        while (read(p->wake[0], drain, sizeof drain) > 0) {
        }
    }
    return NULL;
}

static void pool_open(pool* p) {
    if (!curl.multi_init || !(p->multi = curl.multi_init())) return;
    /* Already so from libcurl 7.62, but not before. */
    curl.multi_setopt(p->multi, M_PIPELINING, (long)PIPE_MULTIPLEX);
    if (pipe(p->wake) != 0) return;
    for (int i = 0; i < 2; i++) {
        fcntl(p->wake[i], F_SETFD, FD_CLOEXEC);
        fcntl(p->wake[i], F_SETFL, fcntl(p->wake[i], F_GETFL) | O_NONBLOCK);
    }
    pthread_t t;
    if (cx_os_thread(&t, transfers, p) != 0) return;
    pthread_detach(t);
    p->running = 1;
}

static void pool_open_1(void) { pool_open(&pools[0]); }
static void pool_open_2(void) { pool_open(&pools[1]); }
static void pool_open_3(void) { pool_open(&pools[2]); }

/* Gives the request to its transfer thread, if there's one. */
static int transfer(request* r) {
    int which = r->version == V_HTTP2_ONLY ? 2 : r->version == V_HTTP2 && r->https ? 1 : 0;
    pool* p = &pools[which];
    once(&p->state, which == 2 ? pool_open_3 : which == 1 ? pool_open_2 : pool_open_1);
    if (!p->running) return 0;
    r->started = 2;
    pthread_mutex_lock(&p->lock);
    r->queued = NULL;
    *p->tail = r;
    p->tail = &r->queued;
    pthread_mutex_unlock(&p->lock);
    char b = 1;
    if (write(p->wake[1], &b, 1) < 0) {
        /* The pipe is full, so the thread will wake anyway. */
    }
    return 1;
}


#endif

/* --- the calls std.http makes -------------------------------------------------------------- */

static void finish(request* r) {
    run(r);
    __atomic_store_n(&r->state, 1, __ATOMIC_RELEASE);
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) {
    finish(p);
    return 0;
}
#else
static void* thread_main(void* p) {
    finish(p);
    return NULL;
}
#endif

/* Starts a request: a GET, or a POST of the body. `head` is the caller's header lines, each
 * ending in "\r\n", which std.http has checked. */
cx_u64 cx_http_start(cx_bool post, const cx_u8* url, cx_usize url_len, const cx_u8* head, cx_usize head_len, const cx_u8* body,
                     cx_usize body_len, cx_u32 timeout_ms, cx_usize limit, cx_u32 version) {
    request* r = calloc(1, sizeof(request));
    if (!r) cx_panic_capacity();
    r->post = post;
    r->version = version <= V_HTTP2_ONLY ? (int)version : V_HTTP1;
    r->timeout_ms = timeout_ms;
    r->limit = limit;
    r->head = dup_text((const char*)head, head_len);
    if (body_len) {
        r->body = malloc(body_len);
        if (!r->body) cx_panic_capacity();
        memcpy(r->body, body, body_len);
    }
    r->body_len = body_len;
    if (!parse_url(r, (const char*)url, url_len)) {
        r->error = E_BAD_URL;
    }
#if defined(_WIN32) || defined(CX_IOS)
    /* WinHTTP and URLSession speak HTTP/2 over TLS alone, not h2c. */
    else if (r->version == V_HTTP2_ONLY && !r->https) {
        r->error = E_UNAVAILABLE;
    }
#else
    else {
        once(&curl_state, curl_load);
        if (!curl_loaded || (r->version == V_HTTP2_ONLY && !curl_http2)) r->error = E_UNAVAILABLE;
    }
#endif
    if (r->error) {
        r->state = 1;
        return table_add(&requests, r);
    }
#ifdef _WIN32
    int e = cx_os_thread(&r->thread, thread_main, r);
    if (e == 0) {
        r->started = 1;
    } else {
        r->error = E_OS, r->code = (cx_i32)e, r->state = 1;
    }
#else
    if (transfer(r)) return table_add(&requests, r);
    int e = cx_os_thread(&r->thread, thread_main, r);
    if (e == 0) {
        r->started = 1;
    } else {
        r->error = E_OS, r->code = e, r->state = 1;
    }
#endif
    return table_add(&requests, r);
}

/* 1 if the request has finished, 0 if it's still running, and -1 if its response was taken. */
cx_i32 cx_http_done(cx_u64 h) {
    request* r = request_of(h);
    if (!r) return -1;
    return (cx_i32)__atomic_load_n(&r->state, __ATOMIC_ACQUIRE);
}

/* Waits for the request to finish, and gives what went wrong, 0 for nothing, or -1 if its
 * response was taken. */
cx_i32 cx_http_wait(cx_u64 h) {
    request* r = request_of(h);
    if (!r) return -1;
    if (r->started) {
#ifdef _WIN32
        WaitForSingleObject(r->thread, INFINITE);
        CloseHandle(r->thread);
#else
        if (r->started == 2) {
            pthread_mutex_lock(&done_lock);
            while (!__atomic_load_n(&r->state, __ATOMIC_ACQUIRE)) pthread_cond_wait(&done_cond, &done_lock);
            pthread_mutex_unlock(&done_lock);
        } else {
            pthread_join(r->thread, NULL);
        }
#endif
        r->started = 0;
    }
    return r->error;
}

/* The HTTP the response came in, 10, 11 or 20 for 1.0, 1.1 and 2, or 0 if it isn't known. */
cx_u32 cx_http_version(cx_u64 h) { return request_of(h)->used; }

/* The request's times, as T_ numbers them, in nanoseconds from its start, then 1 if it went over
 * a connection that was open already, else 0. */
void cx_http_times(cx_u64 h, cx_u64* out) {
    request* r = request_of(h);
    for (int i = 0; i < T_COUNT; i++) out[i] = r->times[i];
    out[T_COUNT] = (cx_u64)r->reused;
}

/* How much of the body has come so far, while the request runs or after, and its Content-Length
 * plus 1, or 0 if that isn't known yet or the response gives none. 0 for both if the response was
 * taken. */
cx_u64 cx_http_received(cx_u64 h) {
    request* r = request_of(h);
    return r ? __atomic_load_n(&r->received, __ATOMIC_RELAXED) : 0;
}

cx_u64 cx_http_expected(cx_u64 h) {
    request* r = request_of(h);
    return r ? __atomic_load_n(&r->expected, __ATOMIC_RELAXED) : 0;
}

/* The system's error code, for E_OS. */
cx_i32 cx_http_code(cx_u64 h) { return request_of(h)->code; }
cx_u32 cx_http_status(cx_u64 h) { return request_of(h)->status; }
cx_usize cx_http_head_len(cx_u64 h) { return request_of(h)->lines.len; }
cx_usize cx_http_body_len(cx_u64 h) { return request_of(h)->data.len; }

void cx_http_head(cx_u64 h, cx_u8* out) {
    request* r = request_of(h);
    if (r->lines.len) memcpy(out, r->lines.p, r->lines.len);
}

void cx_http_body(cx_u64 h, cx_u8* out) {
    request* r = request_of(h);
    if (r->data.len) memcpy(out, r->data.p, r->data.len);
}

/* Lets the request go, once it's been waited for. */
void cx_http_free(cx_u64 h) {
    request* r = request_of(h);
    table_remove(&requests, h);
    if (r) request_free(r);
}

/* --- WebSockets, behind std.http.websocket (CLX-320, CLX-395) ------------------------------- *
 *
 * A WebSocket client, RFC 6455. Each connection runs on a thread of its own, which connects,
 * makes the handshake, then reads messages into a queue the program takes them from, answering
 * pings and the server's close by itself. Text is checked to be UTF-8, and a message longer than
 * the limit, or anything that breaks the protocol, closes the connection with the code RFC 6455
 * gives for it. With a ping interval, the thread pings the server once nothing has come from it
 * for that long, and ends the connection if nothing comes in the ping timeout after, so one whose
 * server or network went away without a FIN, as a laptop's that slept, ends rather than looking
 * open for minutes (CLX-395).
 *
 * The handshake and the frames are written here, the same on every system, over a connection each
 * system makes its own way. On Linux and macOS libcurl makes it, TCP and TLS with the system's
 * certificate checks, in its connect-only mode, as libcurl's own WebSocket API is missing from
 * most systems' builds. On Windows Winsock makes it, with Schannel, the system's TLS, which checks
 * certificates as WinHTTP does: WinHTTP's own WebSocket API can't send a ping, never shows one,
 * and takes no receive timeout once it's open, so with it a dead connection can't be told from a
 * quiet one. The thread does all the reading and writing, as TLS allows one thread at a time on a
 * connection. What the program sends goes into a queue for it, and a byte down a pipe, or an event
 * on Windows, wakes it. */

/* Beyond std.http's: the server answered the handshake with another status, and the connection
 * is closed, with the code and reason of the close. */
enum { E_REFUSED = 10, E_CLOSED = 11 };
enum { WS_CONNECTING, WS_OPEN, WS_CLOSED };
enum { OP_CONTINUE = 0, OP_TEXT = 1, OP_BINARY = 2, OP_CLOSE = 8, OP_PING = 9, OP_PONG = 10 };
/* Close codes, RFC 6455 section 7.4.1. */
enum { CLOSE_NORMAL = 1000, CLOSE_PROTOCOL = 1002, CLOSE_NO_STATUS = 1005, CLOSE_ABNORMAL = 1006, CLOSE_NOT_UTF8 = 1007,
       CLOSE_TOO_BIG = 1009, CLOSE_SERVER_ERROR = 1011 };

typedef struct ws_message {
    struct ws_message* next;
    int kind; /* OP_TEXT or OP_BINARY */
    size_t len;
    cx_u8 data[];
} ws_message;

typedef struct {
    char *url, *host, *target, *head;
    unsigned port;
    int tls;
    cx_u32 timeout_ms;
    size_t limit;
    cx_u32 ping_interval_ms; /* 0 for no pings */
    cx_u32 ping_timeout_ms;
#ifdef _WIN32
    SRWLOCK mu;
    CONDITION_VARIABLE cv;
    HANDLE thread;
    HANDLE wake; /* the event that wakes the thread */
#else
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    int wake[2]; /* the pipe that wakes the thread */
#endif
    int started;
    /* The rest is under `mu`. */
    int state;
    int error;           /* once closed: E_CLOSED, or what went wrong */
    cx_i32 code;         /* the system's code, for E_OS */
    cx_u32 status;       /* the handshake's status, for E_REFUSED */
    cx_u32 close_code;   /* for E_CLOSED */
    bytes close_reason;
    bytes protocol;      /* the Sec-WebSocket-Protocol the server chose */
    ws_message *first, *last;
    bytes taken;         /* the message `cx_http_ws_receive` took */
    bytes out;           /* what the program sends: for each message, its opcode, its length as 8 bytes, then it */
    int closing;         /* the program called close, with this code and reason */
    cx_u32 my_code;
    bytes my_reason;
    cx_u64 heard_ms;     /* when anything last came from the server, in ws_now_ms's milliseconds, once open */
} wsock;

static cx_u64 ws_now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (cx_u64)t.tv_sec * 1000 + (cx_u64)t.tv_nsec / 1000000;
#endif
}

#define WS_FOREVER ((cx_u64)-1)

#ifdef _WIN32
static void ws_init_lock(wsock* w) {
    InitializeSRWLock(&w->mu);
    InitializeConditionVariable(&w->cv);
}
static void ws_lock(wsock* w) { AcquireSRWLockExclusive(&w->mu); }
static void ws_unlock(wsock* w) { ReleaseSRWLockExclusive(&w->mu); }
static void ws_signal(wsock* w) { WakeAllConditionVariable(&w->cv); }
/* Waits, holding `mu`, for a signal or `deadline`, in ws_now_ms's milliseconds: 0 once it's past. */
static int ws_sleep(wsock* w, cx_u64 deadline) {
    DWORD ms = INFINITE;
    if (deadline != WS_FOREVER) {
        cx_u64 now = ws_now_ms();
        if (now >= deadline) return 0;
        ms = deadline - now > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)(deadline - now);
    }
    SleepConditionVariableSRW(&w->cv, &w->mu, ms, 0);
    return 1;
}
#else
static void ws_init_lock(wsock* w) {
    pthread_mutex_init(&w->mu, NULL);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
#ifndef __APPLE__
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&w->cv, &a);
    pthread_condattr_destroy(&a);
}
static void ws_lock(wsock* w) { pthread_mutex_lock(&w->mu); }
static void ws_unlock(wsock* w) { pthread_mutex_unlock(&w->mu); }
static void ws_signal(wsock* w) { pthread_cond_broadcast(&w->cv); }
static int ws_sleep(wsock* w, cx_u64 deadline) {
    if (deadline == WS_FOREVER) {
        pthread_cond_wait(&w->cv, &w->mu);
        return 1;
    }
    cx_u64 now = ws_now_ms();
    if (now >= deadline) return 0;
    cx_u64 ms = deadline - now;
#ifdef __APPLE__
    struct timespec rel = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    pthread_cond_timedwait_relative_np(&w->cv, &w->mu, &rel);
#else
    struct timespec at;
    clock_gettime(CLOCK_MONOTONIC, &at);
    at.tv_sec += (time_t)(ms / 1000);
    at.tv_nsec += (long)(ms % 1000) * 1000000;
    if (at.tv_nsec >= 1000000000) at.tv_sec++, at.tv_nsec -= 1000000000;
    pthread_cond_timedwait(&w->cv, &w->mu, &at);
#endif
    return 1;
}
#endif

static void ws_free(wsock* w) {
    for (ws_message* m = w->first; m;) {
        ws_message* next = m->next;
        free(m);
        m = next;
    }
    free(w->url), free(w->host), free(w->target), free(w->head);
    free(w->close_reason.p), free(w->protocol.p), free(w->taken.p), free(w->out.p), free(w->my_reason.p);
#ifdef _WIN32
    if (w->wake) CloseHandle(w->wake);
#else
    if (w->wake[0] >= 0) close(w->wake[0]);
    if (w->wake[1] >= 0) close(w->wake[1]);
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv);
#endif
    free(w);
}

/* Ends the connection with `error`, unless it has ended already, and wakes the program. Holds `mu`. */
static void ws_end_locked(wsock* w, int error, cx_u32 close_code, const cx_u8* reason, size_t reason_len) {
    if (w->state == WS_CLOSED) return;
    w->state = WS_CLOSED;
    w->error = error;
    w->close_code = close_code;
    w->close_reason.len = 0;
    if (reason_len && !add(&w->close_reason, reason, reason_len)) cx_panic_capacity();
    ws_signal(w);
}

static void ws_end(wsock* w, int error, cx_u32 close_code, const cx_u8* reason, size_t reason_len) {
    ws_lock(w);
    ws_end_locked(w, error, close_code, reason, reason_len);
    ws_unlock(w);
}

static void ws_fail(wsock* w, int error, cx_i32 code) {
    ws_lock(w);
    if (w->state != WS_CLOSED) w->code = code;
    ws_end_locked(w, error, 0, NULL, 0);
    ws_unlock(w);
}

/* Adds a whole message the server sent to the queue. */
static void ws_deliver(wsock* w, int kind, const cx_u8* p, size_t n) {
    ws_message* m = malloc(sizeof(ws_message) + n);
    if (!m) cx_panic_capacity();
    m->next = NULL, m->kind = kind, m->len = n;
    if (n) memcpy(m->data, p, n);
    ws_lock(w);
    if (w->last) w->last->next = m;
    else w->first = m;
    w->last = m;
    ws_signal(w);
    ws_unlock(w);
}

/* Whether the bytes are UTF-8: no overlong forms, no surrogates, nothing past U+10FFFF. */
static int ws_utf8(const cx_u8* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        cx_u8 c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t len;
        cx_u32 cp, min;
        if (c >= 0xC2 && c <= 0xDF) len = 2, cp = c & 0x1F, min = 0x80;
        else if (c >= 0xE0 && c <= 0xEF) len = 3, cp = c & 0x0F, min = 0x800;
        else if (c >= 0xF0 && c <= 0xF4) len = 4, cp = c & 0x07, min = 0x10000;
        else return 0;
        if (n - i < len) return 0;
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (p[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += len;
    }
    return 1;
}

/* Whether a close frame may carry `code`: RFC 6455 section 7.4, with 1012 to 1014, which IANA
 * has registered since, and the codes for libraries and applications, 3000 to 4999. */
static int ws_close_code_ok(cx_u32 code) {
    if (code >= 3000 && code <= 4999) return 1;
    return code >= 1000 && code <= 1014 && code != 1004 && code != CLOSE_NO_STATUS && code != CLOSE_ABNORMAL;
}

/* Random bytes from the system, for the handshake's key and each frame's mask. */
static void ws_random(cx_u8* p, size_t n) {
#ifdef _WIN32
    if (BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        cx_panic_text("<websocket>", "the system gave no random bytes");
    }
#elif defined(__APPLE__)
    arc4random_buf(p, n);
#else
    while (n) {
        ssize_t got = getrandom(p, n, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            cx_panic_text("<websocket>", "the system gave no random bytes");
        }
        p += got, n -= (size_t)got;
    }
#endif
}

/* SHA-1, for checking the server's Sec-WebSocket-Accept. */
static void ws_sha1(const cx_u8* data, size_t n, cx_u8 out[20]) {
    cx_u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t blocks = (n + 9 + 63) / 64;
    for (size_t b = 0; b < blocks; b++) {
        cx_u8 chunk[64];
        for (size_t i = 0; i < 64; i++) {
            size_t k = b * 64 + i;
            chunk[i] = k < n ? data[k] : k == n ? 0x80 : 0;
        }
        if (b == blocks - 1) {
            cx_u64 bits = (cx_u64)n * 8;
            for (int i = 0; i < 8; i++) chunk[63 - i] = (cx_u8)(bits >> (8 * i));
        }
        cx_u32 w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (cx_u32)chunk[4 * i] << 24 | (cx_u32)chunk[4 * i + 1] << 16 | (cx_u32)chunk[4 * i + 2] << 8 | chunk[4 * i + 3];
        }
        for (int i = 16; i < 80; i++) {
            cx_u32 x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = x << 1 | x >> 31;
        }
        cx_u32 a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            cx_u32 f, k;
            if (i < 20) f = (bb & c) | (~bb & d), k = 0x5A827999;
            else if (i < 40) f = bb ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (bb & c) | (bb & d) | (c & d), k = 0x8F1BBCDC;
            else f = bb ^ c ^ d, k = 0xCA62C1D6;
            cx_u32 t = (a << 5 | a >> 27) + f + e + k + w[i];
            e = d, d = c, c = bb << 30 | bb >> 2, bb = a, a = t;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (cx_u8)(h[i] >> 24), out[4 * i + 1] = (cx_u8)(h[i] >> 16), out[4 * i + 2] = (cx_u8)(h[i] >> 8);
        out[4 * i + 3] = (cx_u8)h[i];
    }
}

static void ws_base64(const cx_u8* p, size_t n, char* out) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        cx_u32 v = (cx_u32)p[i] << 16 | (i + 1 < n ? (cx_u32)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[o++] = digits[v >> 18 & 63];
        out[o++] = digits[v >> 12 & 63];
        out[o++] = i + 1 < n ? digits[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? digits[v & 63] : '=';
    }
    out[o] = 0;
}

/* A frame from the client, which RFC 6455 has masked by 4 random bytes, onto `out`. */
static void ws_frame(bytes* out, int op, const cx_u8* p, size_t n) {
    cx_u8 head[14];
    size_t k = 0;
    head[k++] = (cx_u8)(0x80 | op);
    if (n < 126) {
        head[k++] = (cx_u8)(0x80 | n);
    } else if (n < 65536) {
        head[k++] = 0x80 | 126, head[k++] = (cx_u8)(n >> 8), head[k++] = (cx_u8)n;
    } else {
        head[k++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) head[k++] = (cx_u8)((cx_u64)n >> (8 * i));
    }
    cx_u8* mask = head + k;
    ws_random(mask, 4);
    k += 4;
    if (!add(out, head, k) || !grow(out, n)) cx_panic_capacity();
    cx_u8* to = (cx_u8*)out->p + out->len;
    for (size_t i = 0; i < n; i++) to[i] = p[i] ^ mask[i & 3];
    out->len += n;
}

/* A close frame with `code` and `reason`, or none for CLOSE_NO_STATUS. */
static void ws_close_frame(bytes* out, cx_u32 code, const cx_u8* reason, size_t reason_len) {
    cx_u8 body[125];
    size_t n = 0;
    if (code != CLOSE_NO_STATUS) {
        if (reason_len > 123) reason_len = 123;
        body[0] = (cx_u8)(code >> 8), body[1] = (cx_u8)code;
        if (reason_len) memcpy(body + 2, reason, reason_len);
        n = 2 + reason_len;
    }
    ws_frame(out, OP_CLOSE, body, n);
}

/* The decimal digits of `port`. */
static void ws_port_text(unsigned port, char out[8]) {
    char digits[6];
    int k = 0;
    do {
        digits[k++] = (char)('0' + port % 10);
        port /= 10;
    } while (port && k < 5);
    for (int i = 0; i < k; i++) out[i] = digits[k - 1 - i];
    out[k] = 0;
}

/* The value of the header `name` in the handshake's response `head`, or NULL, with its length. */
static const char* ws_header(const char* head, size_t n, const char* name, size_t* len) {
    size_t k = strlen(name), i = 0;
    while (i < n) {
        size_t s = i;
        while (i < n && head[i] != '\n') i++;
        size_t e = i++;
        if (e > s && head[e - 1] == '\r') e--;
        if (e - s > k && head[s + k] == ':' && starts_ci(head + s, k, name)) {
            size_t vs = s + k + 1;
            while (vs < e && (head[vs] == ' ' || head[vs] == '\t')) vs++;
            while (e > vs && (head[e - 1] == ' ' || head[e - 1] == '\t')) e--;
            *len = e - vs;
            return head + vs;
        }
    }
    return NULL;
}

/* Whether the comma-separated `list` has `token`, in any case. */
static int ws_has_token(const char* list, size_t n, const char* token) {
    size_t k = strlen(token), i = 0;
    while (i < n) {
        while (i < n && (list[i] == ' ' || list[i] == '\t' || list[i] == ',')) i++;
        size_t s = i;
        while (i < n && list[i] != ',') i++;
        size_t e = i;
        while (e > s && (list[e - 1] == ' ' || list[e - 1] == '\t')) e--;
        if (e - s == k && starts_ci(list + s, k, token)) return 1;
    }
    return 0;
}

/* Each system's connection, `ws_link`, gives the rest of the client these:
 *
 *   ws_connect(w, l, deadline, &code)  makes the connection, TCP and TLS for wss://, by
 *                                      `deadline`: 0, or what went wrong and the system's code
 *   ws_send_some(l, out, &at)          sends what's in `out` from `at`, as far as the connection
 *                                      takes it now: 0 if it failed
 *   ws_unsent(l)                       whether the connection holds bytes it hasn't sent yet
 *   ws_recv_some(l, in)                reads what has come onto `in`: 1 if anything came, 0 if
 *                                      nothing has, -1 if the server ended the connection or it
 *                                      failed, after what came before the end
 *   ws_poll(w, l, writing, deadline)   waits for the connection, to read, or to write too, for
 *                                      the program's wake, or for `deadline`
 *   ws_disconnect(l)                   closes it and lets it go */

#if defined(CX_IOS)

/* --- WebSockets' connections on Network.framework ------------------------------------------- *
 *
 * iOS has no libcurl to make the connection, so Network.framework makes it: TCP, and TLS for
 * wss://, with the system's certificate checks (CLX-410). It works by callbacks on a queue of its
 * own, so what comes is kept in `in` until the connection's thread reads it, and a byte down a
 * pipe wakes the thread when anything has come, the connection has ended, or a send has gone,
 * which `ws_poll` waits for beside the program's wake. A send hands its bytes over at once and
 * counts as unsent until Network.framework says it went, so a close waits for what was sent
 * before it. */

__asm__(".linker_option \"-framework\", \"Network\"");

typedef struct ws_link_state {
    pthread_mutex_t mu;
    bytes in;        /* what's come and not been read */
    size_t sending;  /* bytes handed to a send that hasn't gone */
    int ended;       /* the server ended its side, or the connection failed: nothing more will come */
    int broken;      /* the connection failed or was cancelled, or a send failed: nothing more goes */
    int pipe[2];     /* a byte down it says one of those happened */
    int refs;        /* the connection's thread, and each callback still to come */
} ws_link_state;

typedef struct {
    nw_connection_t conn;
    dispatch_queue_t queue;
    ws_link_state* s;
} ws_link;

static void link_poke(ws_link_state* s) {
    char b = 1;
    if (write(s->pipe[1], &b, 1) < 0) {
        /* The pipe is full, so the thread will wake anyway. */
    }
}

/* Lets one holder of the state go, and frees it after the last. */
static void link_release(ws_link_state* s) {
    pthread_mutex_lock(&s->mu);
    int last = --s->refs == 0;
    pthread_mutex_unlock(&s->mu);
    if (!last) return;
    close(s->pipe[0]), close(s->pipe[1]);
    free(s->in.p);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

/* Reads what comes, into `in`, until the connection ends. */
static void link_receive(nw_connection_t conn, ws_link_state* s) {
    pthread_mutex_lock(&s->mu);
    s->refs++;
    pthread_mutex_unlock(&s->mu);
    nw_connection_receive(conn, 1, 65536, ^(dispatch_data_t content, nw_content_context_t context, bool complete, nw_error_t error) {
        (void)context;
        pthread_mutex_lock(&s->mu);
        if (content) {
            dispatch_data_apply(content, ^bool(dispatch_data_t region, size_t offset, const void* p, size_t n) {
                (void)region, (void)offset;
                if (!add(&s->in, p, n)) cx_panic_capacity();
                return true;
            });
        }
        /* On a stream, complete is the server's FIN. */
        int over = error != NULL || complete;
        if (over) s->ended = 1;
        if (error) s->broken = 1;
        pthread_mutex_unlock(&s->mu);
        link_poke(s);
        if (!over) link_receive(conn, s);
        link_release(s);
    });
}

/* What a Network.framework error means to std.http, with the system's code. */
static int link_error(nw_error_t e, cx_i32* code) {
    if (!e) {
        *code = 0;
        return E_CONNECT;
    }
    int c = nw_error_get_error_code(e);
    *code = c;
    switch (nw_error_get_error_domain(e)) {
    case nw_error_domain_dns: return E_RESOLVE;
    case nw_error_domain_tls: return E_TLS;
    case nw_error_domain_posix: return c == ETIMEDOUT ? E_TIMEOUT : c == ECONNREFUSED || c == EHOSTUNREACH || c == ENETUNREACH ? E_CONNECT : E_OS;
    default: return E_OS;
    }
}

static int ws_connect(wsock* w, ws_link* l, cx_u64 deadline, cx_i32* code) {
    l->conn = NULL, l->queue = NULL;
    ws_link_state* s = calloc(1, sizeof *s);
    if (!s) cx_panic_capacity();
    if (pipe(s->pipe) != 0) {
        *code = errno;
        free(s);
        return E_OS;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(s->pipe[i], F_SETFD, FD_CLOEXEC);
        fcntl(s->pipe[i], F_SETFL, fcntl(s->pipe[i], F_GETFL) | O_NONBLOCK);
    }
    pthread_mutex_init(&s->mu, NULL);
    s->refs = 2; /* the thread's, and the state handler's until the connection is cancelled */
    l->s = s;
    char port[8];
    ws_port_text(w->port, port);
    nw_endpoint_t at = nw_endpoint_create_host(w->host, port);
    nw_parameters_t params = w->tls ? nw_parameters_create_secure_tcp(NW_PARAMETERS_DEFAULT_CONFIGURATION, NW_PARAMETERS_DEFAULT_CONFIGURATION)
                                    : nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
    /* No proxy, as the other systems' connections take none. */
    nw_parameters_set_prefer_no_proxy(params, true);
    l->conn = nw_connection_create(at, params);
    nw_release(at);
    nw_release(params);
    l->queue = dispatch_queue_create("calx.websocket", DISPATCH_QUEUE_SERIAL);
    nw_connection_set_queue(l->conn, l->queue);
    /* Its state, as it opens: 1 once ready, -1 once it failed, with the error. */
    __block int state = 0;
    __block nw_error_t failure = NULL;
    dispatch_semaphore_t changed = dispatch_semaphore_create(0);
    nw_connection_set_state_changed_handler(l->conn, ^(nw_connection_state_t now, nw_error_t error) {
        if (state != 0) {
            if (now == nw_connection_state_failed || now == nw_connection_state_cancelled) {
                pthread_mutex_lock(&s->mu);
                s->ended = s->broken = 1;
                pthread_mutex_unlock(&s->mu);
                link_poke(s);
            }
            if (now == nw_connection_state_cancelled) link_release(s);
            return;
        }
        if (now == nw_connection_state_ready) {
            state = 1;
        } else if (now == nw_connection_state_failed || now == nw_connection_state_cancelled ||
                   (now == nw_connection_state_waiting && error)) {
            /* Waiting is what it does when it can't connect yet, as a refused port or no network
             * gives, which here is as good as failing. */
            if (error) failure = nw_retain(error);
            state = -1;
        } else {
            return;
        }
        dispatch_semaphore_signal(changed);
    });
    nw_connection_start(l->conn);
    cx_u64 now = ws_now_ms();
    long waited = now < deadline ? dispatch_semaphore_wait(changed, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(deadline - now) * NSEC_PER_MSEC)) : 1;
    dispatch_release(changed);
    __block int opened = 0;
    __block nw_error_t why = NULL;
    dispatch_sync(l->queue, ^{
        opened = state;
        why = failure;
        if (state == 0) state = -1; /* what comes after is too late */
    });
    (void)waited;
    if (opened != 1) {
        int e = opened == 0 ? E_TIMEOUT : link_error(why, code);
        if (why) nw_release(why);
        nw_connection_cancel(l->conn);
        nw_release(l->conn);
        dispatch_release(l->queue);
        link_release(s);
        return e;
    }
    link_receive(l->conn, s);
    return 0;
}

static void ws_disconnect(ws_link* l) {
    ws_link_state* s = l->s;
    /* The end of what this side sends, a FIN, after everything sent before it, as closing a socket
     * gives on the other systems. Cancelling at once would reset the connection, which can lose the
     * close frame before it. Then the server's FIN is waited for, as RFC 6455 has the server close
     * first, for as long as a second. */
    pthread_mutex_lock(&s->mu);
    int broken = s->broken;
    if (!broken) s->sending++, s->refs++;
    pthread_mutex_unlock(&s->mu);
    if (!broken) {
        nw_connection_send(l->conn, NULL, NW_CONNECTION_FINAL_MESSAGE_CONTEXT, true, ^(nw_error_t error) {
            pthread_mutex_lock(&s->mu);
            s->sending--;
            if (error) s->broken = 1;
            pthread_mutex_unlock(&s->mu);
            link_poke(s);
            link_release(s);
        });
    }
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&s->mu);
        int busy = !s->broken && (s->sending > 0 || !s->ended);
        pthread_mutex_unlock(&s->mu);
        if (!busy) break;
        struct timespec ten = {0, 10000000};
        nanosleep(&ten, NULL);
    }
    nw_connection_cancel(l->conn);
    nw_release(l->conn);
    dispatch_release(l->queue);
    link_release(s);
}

/* Hands what's in `out` from `at` to the connection, while less than 4 MiB of it is still going. */
static int ws_send_some(ws_link* l, bytes* out, size_t* at) {
    ws_link_state* s = l->s;
    pthread_mutex_lock(&s->mu);
    int broken = s->broken;
    size_t room = s->sending < (4u << 20) ? (4u << 20) - s->sending : 0;
    pthread_mutex_unlock(&s->mu);
    if (broken && *at < out->len) return 0;
    size_t n = out->len - *at < room ? out->len - *at : room;
    if (n) {
        dispatch_data_t data = dispatch_data_create(out->p + *at, n, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        pthread_mutex_lock(&s->mu);
        s->sending += n;
        s->refs++;
        pthread_mutex_unlock(&s->mu);
        nw_connection_send(l->conn, data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, false, ^(nw_error_t error) {
            pthread_mutex_lock(&s->mu);
            s->sending -= n;
            if (error) s->ended = s->broken = 1;
            pthread_mutex_unlock(&s->mu);
            link_poke(s);
            link_release(s);
        });
        dispatch_release(data);
        *at += n;
    }
    if (*at == out->len) out->len = 0, *at = 0;
    return 1;
}

static int ws_unsent(ws_link* l) {
    pthread_mutex_lock(&l->s->mu);
    int some = l->s->sending > 0 && !l->s->broken;
    pthread_mutex_unlock(&l->s->mu);
    return some;
}

static int ws_recv_some(ws_link* l, bytes* in) {
    ws_link_state* s = l->s;
    pthread_mutex_lock(&s->mu);
    int got = s->in.len > 0, ended = s->ended;
    if (got) {
        if (!add(in, s->in.p, s->in.len)) cx_panic_capacity();
        s->in.len = 0;
    }
    pthread_mutex_unlock(&s->mu);
    return got ? 1 : ended ? -1 : 0;
}

static void ws_poll(wsock* w, ws_link* l, int writing, cx_u64 deadline) {
    (void)writing; /* a send that goes pokes the pipe too */
    struct pollfd fds[2] = {{l->s->pipe[0], POLLIN, 0}, {w->wake[0], POLLIN, 0}};
    int ms = -1;
    if (deadline != WS_FOREVER) {
        cx_u64 now = ws_now_ms();
        ms = now >= deadline ? 0 : deadline - now > 0x7FFFFFFF ? 0x7FFFFFFF : (int)(deadline - now);
    }
    if (poll(fds, 2, ms) > 0) {
        char drain[64];
        if (fds[0].revents & POLLIN) {
            while (read(l->s->pipe[0], drain, sizeof drain) > 0) {
            }
        }
        if (fds[1].revents & POLLIN) {
            while (read(w->wake[0], drain, sizeof drain) > 0) {
            }
        }
    }
}

#elif !defined(_WIN32)

/* --- WebSockets' connections on libcurl ----------------------------------------------------- */

enum { O_CONNECT_ONLY = 141, I_ACTIVESOCKET = 0x500000 + 44, I_LASTSOCKET = 0x200000 + 29, E_CURL_AGAIN = 81 };

typedef struct {
    void* h;
    int sock;
} ws_link;

static int ws_connect(wsock* w, ws_link* l, cx_u64 deadline, cx_i32* code) {
    (void)deadline; /* libcurl's own timeout is the same */
    l->sock = -1;
    l->h = curl.easy_init();
    if (!l->h) {
        *code = ENOMEM;
        return E_OS;
    }
    void* h = l->h;
    curl.easy_setopt(h, O_NOSIGNAL, 1L);
    curl.easy_setopt(h, O_URL, w->url);
    curl.easy_setopt(h, O_PROXY, "");
    curl.easy_setopt(h, O_HTTP_VERSION, 2L); /* HTTP/1.1, so TLS's ALPN offers nothing else */
    curl.easy_setopt(h, O_PROTOCOLS, 3L);
    curl.easy_setopt(h, O_CONNECT_ONLY, 1L);
    curl.easy_setopt(h, O_TIMEOUT_MS, (long)w->timeout_ms);
    int e = curl.easy_perform(h);
    if (e != 0) {
        *code = e;
        curl.easy_cleanup(h);
        return curl_error(e);
    }
    long sock_long = -1;
    int sock = -1;
    if (curl.easy_getinfo(h, I_ACTIVESOCKET, &sock) != 0 || sock < 0) {
        if (curl.easy_getinfo(h, I_LASTSOCKET, &sock_long) == 0) sock = (int)sock_long;
    }
    if (sock < 0) {
        *code = 0;
        curl.easy_cleanup(h);
        return E_OS;
    }
    l->sock = sock;
    return 0;
}

static void ws_disconnect(ws_link* l) { curl.easy_cleanup(l->h); }

static int ws_send_some(ws_link* l, bytes* out, size_t* at) {
    while (*at < out->len) {
        size_t n = 0;
        int e = curl.easy_send(l->h, out->p + *at, out->len - *at, &n);
        if (e == E_CURL_AGAIN) break;
        if (e != 0) return 0;
        *at += n;
    }
    if (*at == out->len) out->len = 0, *at = 0;
    return 1;
}

static int ws_unsent(ws_link* l) {
    (void)l;
    return 0;
}

static int ws_recv_some(ws_link* l, bytes* in) {
    int got_any = 0;
    for (;;) {
        if (!grow(in, 65536)) cx_panic_capacity();
        size_t n = 0;
        int e = curl.easy_recv(l->h, in->p + in->len, 65536, &n);
        if (e == E_CURL_AGAIN) return got_any;
        if (e != 0 || n == 0) return -1;
        in->len += n;
        got_any = 1;
    }
}

static void ws_poll(wsock* w, ws_link* l, int writing, cx_u64 deadline) {
    struct pollfd fds[2] = {{l->sock, (short)(POLLIN | (writing ? POLLOUT : 0)), 0}, {w->wake[0], POLLIN, 0}};
    int ms = -1;
    if (deadline != WS_FOREVER) {
        cx_u64 now = ws_now_ms();
        ms = now >= deadline ? 0 : deadline - now > 0x7FFFFFFF ? 0x7FFFFFFF : (int)(deadline - now);
    }
    if (poll(fds, 2, ms) > 0 && (fds[1].revents & POLLIN)) {
        char drain[64];
        while (read(w->wake[0], drain, sizeof drain) > 0) {
        }
    }
}

#else

/* --- WebSockets' connections on Winsock and Schannel ---------------------------------------- */

/* Schannel's calls, from secur32.dll, looked up rather than linked, and the little of sspi.h and
 * schannel.h they need, written out, as mingw-w64's headers lack SCH_CREDENTIALS. */
typedef struct {
    ULONG_PTR lower, upper;
} ws_sec_handle;
typedef struct {
    ULONG size, type;
    void* p;
} ws_sec_buffer;
typedef struct {
    ULONG version, count;
    ws_sec_buffer* buffers;
} ws_sec_desc;
typedef struct {
    ULONG header, trailer, max_message, buffers, block_size;
} ws_stream_sizes;
/* SCH_CREDENTIALS, from Windows 10 1809, which TLS 1.3 needs. */
typedef struct {
    DWORD version, cred_format, creds;
    void* cred_list;
    void* root_store;
    DWORD mappers;
    void* mapper_list;
    DWORD session_lifespan, flags, tls_parameters;
    void* tls_parameter_list;
} ws_sch_credentials;
/* SCHANNEL_CRED, for the Windows before, which stop at TLS 1.2. */
typedef struct {
    DWORD version, creds;
    void* cred_list;
    void* root_store;
    DWORD mappers;
    void* mapper_list;
    DWORD algs;
    void* alg_list;
    DWORD protocols, min_strength, max_strength, session_lifespan, flags, cred_format;
} ws_schannel_cred;

enum {
    SSPI_CRED_OUTBOUND = 2,
    SSPI_SCHANNEL_CRED_VERSION = 4,
    SSPI_SCH_CREDENTIALS_VERSION = 5,
    SSPI_NO_DEFAULT_CREDS = 0x10,
    SSPI_AUTO_CRED_VALIDATION = 0x20,
    SSPI_USE_STRONG_CRYPTO = 0x00400000,
    /* ISC_REQ_SEQUENCE_DETECT, _REPLAY_DETECT, _CONFIDENTIALITY, _ALLOCATE_MEMORY, _EXTENDED_ERROR, _STREAM */
    SSPI_WANT = 0x8 | 0x4 | 0x10 | 0x100 | 0x4000 | 0x8000,
    SSPI_EMPTY = 0,
    SSPI_DATA = 1,
    SSPI_TOKEN = 2,
    SSPI_EXTRA = 5,
    SSPI_TRAILER = 6,
    SSPI_HEADER = 7,
    SSPI_ALERT = 17,
    SSPI_STREAM_SIZES = 4,
};
#define SSPI_OK 0L
#define SSPI_CONTINUE ((LONG)0x00090312L)
#define SSPI_INCOMPLETE ((LONG)0x80090318L)
#define SSPI_INCOMPLETE_CREDENTIALS ((LONG)0x00090320L)
#define SSPI_CONTEXT_EXPIRED ((LONG)0x00090317L)
#define SSPI_RENEGOTIATE ((LONG)0x00090321L)

static struct {
    LONG(WINAPI* acquire)(wchar_t*, wchar_t*, ULONG, void*, void*, void*, void*, ws_sec_handle*, LARGE_INTEGER*);
    LONG(WINAPI* init)(ws_sec_handle*, ws_sec_handle*, wchar_t*, ULONG, ULONG, ULONG, ws_sec_desc*, ULONG, ws_sec_handle*, ws_sec_desc*,
                       ULONG*, LARGE_INTEGER*);
    LONG(WINAPI* delete_context)(ws_sec_handle*);
    LONG(WINAPI* free_cred)(ws_sec_handle*);
    LONG(WINAPI* free_buffer)(void*);
    LONG(WINAPI* query)(ws_sec_handle*, ULONG, void*);
    LONG(WINAPI* encrypt)(ws_sec_handle*, ULONG, ws_sec_desc*, ULONG);
    LONG(WINAPI* decrypt)(ws_sec_handle*, ws_sec_desc*, ULONG, ULONG*);
} sspi;
static int sspi_loaded, wsa_started;
static cx_u32 sspi_state, wsa_state;

static void sspi_load(void) {
    HMODULE m = LoadLibraryW(L"secur32.dll");
    if (!m) return;
    const char* names[8] = {"AcquireCredentialsHandleW", "InitializeSecurityContextW", "DeleteSecurityContext", "FreeCredentialsHandle",
                            "FreeContextBuffer",         "QueryContextAttributesW",    "EncryptMessage",        "DecryptMessage"};
    FARPROC fns[8];
    for (int i = 0; i < 8; i++) {
        fns[i] = GetProcAddress(m, names[i]);
        if (!fns[i]) return;
    }
    memcpy(&sspi.acquire, &fns[0], sizeof(FARPROC));
    memcpy(&sspi.init, &fns[1], sizeof(FARPROC));
    memcpy(&sspi.delete_context, &fns[2], sizeof(FARPROC));
    memcpy(&sspi.free_cred, &fns[3], sizeof(FARPROC));
    memcpy(&sspi.free_buffer, &fns[4], sizeof(FARPROC));
    memcpy(&sspi.query, &fns[5], sizeof(FARPROC));
    memcpy(&sspi.encrypt, &fns[6], sizeof(FARPROC));
    memcpy(&sspi.decrypt, &fns[7], sizeof(FARPROC));
    sspi_loaded = 1;
}

static void wsa_start(void) {
    WSADATA d;
    wsa_started = WSAStartup(MAKEWORD(2, 2), &d) == 0;
}

typedef struct {
    SOCKET s;
    WSAEVENT ev;  /* set when the socket can be read, written after it was full, or closed */
    int blocked;  /* a send found the socket full, and it hasn't said it has room since */
    int tls;
    ws_sec_handle cred, ctx;
    int has_cred, has_ctx;
    wchar_t* target; /* the host, which the certificate must name */
    ws_stream_sizes sizes;
    bytes raw;       /* what came and isn't decrypted yet */
    bytes sealed;    /* what's encrypted and isn't sent yet, from sealed_at */
    size_t sealed_at;
    int renegotiating; /* TLS 1.3 sent a message after the handshake, which Schannel is reading */
} ws_link;

static void ws_poll(wsock* w, ws_link* l, int writing, cx_u64 deadline) {
    /* The socket's event says it has room only after a send found it full, so a writer that
     * hasn't found it full yet goes and tries. */
    if (writing && !l->blocked) return;
    DWORD ms = INFINITE;
    if (deadline != WS_FOREVER) {
        cx_u64 now = ws_now_ms();
        ms = now >= deadline ? 0 : deadline - now > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)(deadline - now);
    }
    HANDLE handles[2] = {l->ev, w->wake};
    WaitForMultipleObjects(2, handles, FALSE, ms);
    WSANETWORKEVENTS happened;
    if (WSAEnumNetworkEvents(l->s, l->ev, &happened) == 0 && (happened.lNetworkEvents & (FD_WRITE | FD_CLOSE))) l->blocked = 0;
}

/* Sends what's sealed, as far as the socket takes it now: 0 if it failed. */
static int ws_flush(ws_link* l) {
    while (l->sealed_at < l->sealed.len) {
        size_t n = l->sealed.len - l->sealed_at;
        int k = send(l->s, l->sealed.p + l->sealed_at, n > (1 << 20) ? (1 << 20) : (int)n, 0);
        if (k > 0) {
            l->sealed_at += (size_t)k;
            continue;
        }
        if (k < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            l->blocked = 1;
            break;
        }
        return 0;
    }
    if (l->sealed_at == l->sealed.len) l->sealed.len = 0, l->sealed_at = 0;
    return 1;
}

/* Sends all of `p`, waiting while the socket is full, until `deadline`: 0 if it couldn't. */
static int ws_send_all(wsock* w, ws_link* l, const void* p, size_t n, cx_u64 deadline) {
    if (n && !add(&l->sealed, p, n)) cx_panic_capacity();
    for (;;) {
        if (!ws_flush(l)) return 0;
        if (l->sealed.len == 0) return 1;
        if (ws_now_ms() >= deadline) return 0;
        ws_poll(w, l, 1, deadline);
    }
}

/* Reads what has come onto `raw`, waiting for something until `deadline`: 1 if something came,
 * 0 if the time is up, -1 if the connection ended or failed. */
static int ws_read_raw(wsock* w, ws_link* l, cx_u64 deadline) {
    for (;;) {
        if (!grow(&l->raw, 16384)) cx_panic_capacity();
        int n = recv(l->s, l->raw.p + l->raw.len, 16384, 0);
        if (n > 0) {
            l->raw.len += (size_t)n;
            return 1;
        }
        if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) return -1;
        if (ws_now_ms() >= deadline) return 0;
        ws_poll(w, l, 0, deadline);
    }
}

/* What Schannel gave to send, sealed already, onto `sealed`, and the buffers it gave let go. */
static void ws_take_token(ws_link* l, ws_sec_buffer* outb) {
    if (outb[0].p && outb[0].size && !add(&l->sealed, outb[0].p, outb[0].size)) cx_panic_capacity();
    if (outb[0].p) sspi.free_buffer(outb[0].p);
    if (outb[1].p) sspi.free_buffer(outb[1].p);
}

/* What InitializeSecurityContext didn't read of `raw`, kept for the next call or the first
 * records, and what it read dropped. */
static void ws_keep_extra(ws_link* l, ws_sec_buffer* inb) {
    size_t rest = inb[1].type == SSPI_EXTRA ? inb[1].size : 0;
    if (rest > l->raw.len) rest = l->raw.len;
    memmove(l->raw.p, l->raw.p + l->raw.len - rest, rest);
    l->raw.len = rest;
}

/* One step of the handshake, or of reading what TLS 1.3 sends after it, with what's in `raw`. */
static LONG ws_tls_step(ws_link* l, int first) {
    ws_sec_buffer inb[2] = {{(ULONG)l->raw.len, SSPI_TOKEN, l->raw.p}, {0, SSPI_EMPTY, NULL}};
    ws_sec_desc ind = {0, 2, inb};
    ws_sec_buffer outb[2] = {{0, SSPI_TOKEN, NULL}, {0, SSPI_ALERT, NULL}};
    ws_sec_desc outd = {0, 2, outb};
    ULONG got = 0;
    LONG st = sspi.init(&l->cred, first ? NULL : &l->ctx, l->target, SSPI_WANT, 0, 0, first ? NULL : &ind, 0, &l->ctx, &outd, &got, NULL);
    if (first && (st == SSPI_OK || st == SSPI_CONTINUE || st == SSPI_INCOMPLETE_CREDENTIALS)) l->has_ctx = 1;
    ws_take_token(l, outb);
    if (!first && st != SSPI_INCOMPLETE) ws_keep_extra(l, inb);
    return st;
}

/* The TLS handshake, by `deadline`: 0, or what went wrong. */
static int ws_tls_handshake(wsock* w, ws_link* l, cx_u64 deadline, cx_i32* code) {
    once(&sspi_state, sspi_load);
    if (!sspi_loaded) {
        *code = (cx_i32)GetLastError();
        return E_OS;
    }
    ws_sch_credentials creds = {0};
    creds.version = SSPI_SCH_CREDENTIALS_VERSION;
    creds.flags = SSPI_NO_DEFAULT_CREDS | SSPI_AUTO_CRED_VALIDATION;
    wchar_t package[] = L"Microsoft Unified Security Protocol Provider";
    LONG st = sspi.acquire(NULL, package, SSPI_CRED_OUTBOUND, NULL, &creds, NULL, NULL, &l->cred, NULL);
    if (st != SSPI_OK) {
        ws_schannel_cred old = {0};
        old.version = SSPI_SCHANNEL_CRED_VERSION;
        old.flags = SSPI_NO_DEFAULT_CREDS | SSPI_AUTO_CRED_VALIDATION | SSPI_USE_STRONG_CRYPTO;
        st = sspi.acquire(NULL, package, SSPI_CRED_OUTBOUND, NULL, &old, NULL, NULL, &l->cred, NULL);
    }
    if (st != SSPI_OK) {
        *code = (cx_i32)st;
        return E_TLS;
    }
    l->has_cred = 1;
    l->target = widen(w->host);
    st = ws_tls_step(l, 1);
    for (;;) {
        if (l->sealed.len && !ws_send_all(w, l, NULL, 0, deadline)) {
            *code = 0;
            return ws_now_ms() >= deadline ? E_TIMEOUT : E_TLS;
        }
        if (st == SSPI_OK) break;
        if (st != SSPI_CONTINUE && st != SSPI_INCOMPLETE && st != SSPI_INCOMPLETE_CREDENTIALS) {
            *code = (cx_i32)st;
            return E_TLS;
        }
        /* With no certificate of its own to give, it goes on without. */
        if (st == SSPI_INCOMPLETE || (st == SSPI_CONTINUE && l->raw.len == 0)) {
            int r = ws_read_raw(w, l, deadline);
            if (r == 0) {
                *code = 0;
                return E_TIMEOUT;
            }
            if (r < 0) {
                *code = (cx_i32)WSAGetLastError();
                return E_TLS;
            }
        }
        st = ws_tls_step(l, 0);
    }
    st = sspi.query(&l->ctx, SSPI_STREAM_SIZES, &l->sizes);
    if (st != SSPI_OK) {
        *code = (cx_i32)st;
        return E_TLS;
    }
    return 0;
}

static void ws_disconnect(ws_link* l) {
    if (l->has_ctx) sspi.delete_context(&l->ctx);
    if (l->has_cred) sspi.free_cred(&l->cred);
    if (l->s != INVALID_SOCKET) closesocket(l->s);
    if (l->ev != WSA_INVALID_EVENT) WSACloseEvent(l->ev);
    free(l->target), free(l->raw.p), free(l->sealed.p);
}

/* Waits for a connect started on `s` until `deadline`: 0 once it's made, the WSA error that
 * failed it, or -1 if the time's up. */
static int ws_wait_connect(SOCKET s, cx_u64 deadline) {
    cx_u64 now = ws_now_ms();
    if (now >= deadline) return -1;
    cx_u64 ms = deadline - now;
    if (ms > 0x7FFFFFFF) ms = 0x7FFFFFFF;
    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(s, &wr);
    FD_SET(s, &ex);
    struct timeval tv = {(long)(ms / 1000), (long)(ms % 1000) * 1000};
    int n = select(0, NULL, &wr, &ex, &tv);
    if (n == 0) return -1;
    if (n < 0) return WSAGetLastError();
    int err = 0, len = sizeof err;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0) return WSAGetLastError();
    return FD_ISSET(s, &ex) && !err ? WSAECONNREFUSED : err;
}

static int ws_connect(wsock* w, ws_link* l, cx_u64 deadline, cx_i32* code) {
    memset(l, 0, sizeof *l);
    l->s = INVALID_SOCKET;
    l->ev = WSA_INVALID_EVENT;
    l->tls = w->tls;
    once(&wsa_state, wsa_start);
    if (!wsa_started) {
        *code = (cx_i32)WSAGetLastError();
        return E_OS;
    }
    char port[8];
    ws_port_text(w->port, port);
    struct addrinfo hints = {0}, *found = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    int e = getaddrinfo(w->host, port, &hints, &found);
    if (e != 0) {
        *code = e;
        return E_RESOLVE;
    }
    /* Each of the host's addresses in turn, until one answers or the time's up. */
    int failed = E_CONNECT;
    for (struct addrinfo* a = found; a && l->s == INVALID_SOCKET; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) {
            *code = WSAGetLastError();
            continue;
        }
        u_long on = 1;
        ioctlsocket(s, FIONBIO, &on);
        int r = connect(s, a->ai_addr, (int)a->ai_addrlen) == 0 ? 0 : WSAGetLastError();
        if (r == WSAEWOULDBLOCK) r = ws_wait_connect(s, deadline);
        if (r == 0) {
            l->s = s;
            break;
        }
        closesocket(s);
        if (r < 0) {
            failed = E_TIMEOUT, *code = 0;
            break;
        }
        *code = r;
    }
    freeaddrinfo(found);
    if (l->s == INVALID_SOCKET) return failed;
    *code = 0;
    BOOL nodelay = TRUE;
    setsockopt(l->s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof nodelay);
    l->ev = WSACreateEvent();
    if (l->ev == WSA_INVALID_EVENT || WSAEventSelect(l->s, l->ev, FD_READ | FD_WRITE | FD_CLOSE) != 0) {
        *code = (cx_i32)WSAGetLastError();
        ws_disconnect(l);
        return E_OS;
    }
    if (l->tls) {
        e = ws_tls_handshake(w, l, deadline, code);
        if (e) {
            ws_disconnect(l);
            return e;
        }
    }
    return 0;
}

static int ws_send_some(ws_link* l, bytes* out, size_t* at) {
    if (!l->tls) {
        while (*at < out->len) {
            size_t n = out->len - *at;
            int k = send(l->s, out->p + *at, n > (1 << 20) ? (1 << 20) : (int)n, 0);
            if (k > 0) {
                *at += (size_t)k;
                continue;
            }
            if (k < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
                l->blocked = 1;
                break;
            }
            return 0;
        }
        if (*at == out->len) out->len = 0, *at = 0;
        return 1;
    }
    /* Sealed into TLS records, at most the largest Schannel makes each, to go as the socket takes
     * them. */
    while (*at < out->len) {
        size_t n = out->len - *at;
        if (n > l->sizes.max_message) n = l->sizes.max_message;
        if (!grow(&l->sealed, l->sizes.header + n + l->sizes.trailer)) cx_panic_capacity();
        char* rec = l->sealed.p + l->sealed.len;
        memcpy(rec + l->sizes.header, out->p + *at, n);
        ws_sec_buffer b[4] = {{l->sizes.header, SSPI_HEADER, rec},
                              {(ULONG)n, SSPI_DATA, rec + l->sizes.header},
                              {l->sizes.trailer, SSPI_TRAILER, rec + l->sizes.header + n},
                              {0, SSPI_EMPTY, NULL}};
        ws_sec_desc d = {0, 4, b};
        if (sspi.encrypt(&l->ctx, 0, &d, 0) != SSPI_OK) return 0;
        l->sealed.len += (size_t)b[0].size + b[1].size + b[2].size;
        *at += n;
    }
    out->len = 0, *at = 0;
    return ws_flush(l);
}

static int ws_unsent(ws_link* l) { return l->sealed.len > l->sealed_at; }

/* Decrypts the whole records in `raw` onto `in`: 1 if anything came, 0 if nothing did, -1 if the
 * server ended the TLS session or it failed. */
static int ws_unseal(ws_link* l, bytes* in) {
    int got = 0;
    for (;;) {
        if (l->renegotiating) {
            LONG st = ws_tls_step(l, 0);
            if (st == SSPI_INCOMPLETE) break;
            if (st != SSPI_OK && st != SSPI_CONTINUE) return -1;
            l->renegotiating = st == SSPI_CONTINUE;
            if (l->raw.len == 0) break;
            continue;
        }
        if (l->raw.len == 0) break;
        ws_sec_buffer b[4] = {{(ULONG)l->raw.len, SSPI_DATA, l->raw.p}, {0, SSPI_EMPTY, NULL}, {0, SSPI_EMPTY, NULL}, {0, SSPI_EMPTY, NULL}};
        ws_sec_desc d = {0, 4, b};
        LONG st = sspi.decrypt(&l->ctx, &d, 0, NULL);
        if (st == SSPI_INCOMPLETE) break;
        if (st != SSPI_OK && st != SSPI_RENEGOTIATE && st != SSPI_CONTEXT_EXPIRED) return -1;
        size_t rest = 0;
        for (int i = 0; i < 4; i++) {
            if (b[i].type == SSPI_DATA && b[i].size) {
                if (!add(in, b[i].p, b[i].size)) cx_panic_capacity();
                got = 1;
            } else if (b[i].type == SSPI_EXTRA) {
                rest = b[i].size;
            }
        }
        if (st == SSPI_CONTEXT_EXPIRED) return -1;
        if (rest > l->raw.len) rest = l->raw.len;
        memmove(l->raw.p, l->raw.p + l->raw.len - rest, rest);
        l->raw.len = rest;
        /* TLS 1.3's messages after the handshake, as a new session ticket or a key update, go
         * back to InitializeSecurityContext. */
        if (st == SSPI_RENEGOTIATE) l->renegotiating = 1;
    }
    return got;
}

static int ws_recv_some(ws_link* l, bytes* in) {
    int got_any = 0, ended = 0;
    bytes* to = l->tls ? &l->raw : in;
    for (;;) {
        if (!grow(to, 65536)) cx_panic_capacity();
        int n = recv(l->s, to->p + to->len, 65536, 0);
        if (n > 0) {
            to->len += (size_t)n;
            got_any = 1;
            continue;
        }
        if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) ended = 1;
        break;
    }
    if (l->tls) {
        got_any = 0;
        int r = ws_unseal(l, in);
        if (r > 0) got_any = 1;
        if (r < 0) ended = 1;
    }
    return ended ? -1 : got_any;
}

#endif

/* Makes the handshake over the connection `l`, by `deadline`, then reads and writes frames until
 * the connection ends. */
static void ws_talk(wsock* w, ws_link* l, cx_u64 deadline) {
    bytes out = {0}, in = {0}, message = {0};
    cx_u8 nonce[16];
    ws_random(nonce, sizeof nonce);
    char key[25];
    ws_base64(nonce, sizeof nonce, key);
    int bracket = strchr(w->host, ':') != NULL;
    char port[9] = "";
    if (w->port != (w->tls ? 443u : 80u)) {
        port[0] = ':';
        ws_port_text(w->port, port + 1);
    }
    const char* parts[] = {"GET ", w->target, " HTTP/1.1\r\nHost: ", bracket ? "[" : "", w->host, bracket ? "]" : "", port,
                           "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ", key,
                           "\r\nSec-WebSocket-Version: 13\r\n", has_header(w->head, "user-agent") ? "" : "User-Agent: Calx\r\n",
                           w->head, "\r\n"};
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        if (!add(&out, parts[i], strlen(parts[i]))) cx_panic_capacity();
    }
    size_t sent = 0, head_end = 0, scanned = 0;
    int failed = 0;
    for (;;) {
        if (!ws_send_some(l, &out, &sent)) {
            failed = E_PROTOCOL;
            break;
        }
        int r = ws_recv_some(l, &in);
        /* The head ends at its blank line, which may have come with the server's close, as a
         * refusal's does. */
        for (size_t i = scanned; i + 3 < in.len; i++) {
            if (memcmp(in.p + i, "\r\n\r\n", 4) == 0) {
                head_end = i + 4;
                break;
            }
        }
        if (head_end) break;
        scanned = in.len > 3 ? in.len - 3 : 0;
        if (r < 0) {
            failed = E_PROTOCOL;
            break;
        }
        if (in.len > 65536) {
            failed = E_PROTOCOL;
            break;
        }
        if (ws_now_ms() >= deadline) {
            failed = E_TIMEOUT;
            break;
        }
        if (r == 0) ws_poll(w, l, out.len > 0 || ws_unsent(l), deadline);
    }
    if (!failed) {
        /* "HTTP/1.1 101 Switching Protocols", the upgrade, and the key's answer. */
        const char* head = in.p;
        unsigned status = 0;
        size_t i = 0;
        while (i < head_end && head[i] != ' ') i++;
        while (i < head_end && head[i] == ' ') i++;
        for (int d = 0; d < 3 && i < head_end && head[i] >= '0' && head[i] <= '9'; d++) status = status * 10 + (unsigned)(head[i++] - '0');
        if (head_end < 5 || memcmp(head, "HTTP/", 5) != 0 || status < 100) {
            failed = E_PROTOCOL;
        } else if (status != 101) {
            ws_lock(w);
            w->status = status;
            ws_unlock(w);
            failed = E_REFUSED;
        } else {
            char accept[64];
            memcpy(accept, key, 24);
            memcpy(accept + 24, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
            cx_u8 digest[20];
            ws_sha1((const cx_u8*)accept, 60, digest);
            char want[29];
            ws_base64(digest, 20, want);
            size_t ul = 0, cl = 0, al = 0, xl = 0, pl = 0;
            const char* up = ws_header(head, head_end, "upgrade", &ul);
            const char* conn = ws_header(head, head_end, "connection", &cl);
            const char* acc = ws_header(head, head_end, "sec-websocket-accept", &al);
            const char* ext = ws_header(head, head_end, "sec-websocket-extensions", &xl);
            const char* proto = ws_header(head, head_end, "sec-websocket-protocol", &pl);
            if (!up || !ws_has_token(up, ul, "websocket") || !conn || !ws_has_token(conn, cl, "upgrade") || !acc || al != 28 ||
                memcmp(acc, want, 28) != 0 || (ext && xl)) {
                failed = E_PROTOCOL;
            } else {
                ws_lock(w);
                if (proto && pl && !add(&w->protocol, proto, pl)) cx_panic_capacity();
                if (w->state == WS_CONNECTING) w->state = WS_OPEN;
                w->heard_ms = ws_now_ms();
                ws_signal(w);
                ws_unlock(w);
            }
        }
    }
    if (failed) {
        ws_fail(w, failed, 0);
        free(out.p), free(in.p);
        return;
    }
    /* What came after the handshake's head is the first frames. */
    memmove(in.p, in.p + head_end, in.len - head_end);
    in.len -= head_end;

    /* Frames, until the connection ends. `fragment` is the opcode of the message whose frames are
     * coming, or 0. The keepalive: when anything last came from the server, how many reads have
     * brought something, and, while a ping waits, when it went and that count then. */
    int fragment = 0, close_sent = 0, close_got = 0;
    cx_u64 close_by = WS_FOREVER;
    cx_u64 heard = ws_now_ms(), reads = 0, pinged = 0, pinged_reads = 0;
    cx_u32 pings = 0;
    size_t taken_in = 0;
    bytes queued = {0};
    for (;;) {
        /* The program's messages and its close, made into frames. */
        ws_lock(w);
        if (w->out.len) {
            bytes t = queued;
            queued = w->out;
            w->out = t;
            w->out.len = 0;
        }
        int closing = w->closing;
        cx_u32 my_code = w->my_code;
        ws_unlock(w);
        for (size_t i = 0; i + 9 <= queued.len;) {
            int op = (cx_u8)queued.p[i];
            cx_u64 n = 0;
            for (int k = 0; k < 8; k++) n = n << 8 | (cx_u8)queued.p[i + 1 + k];
            if (!close_sent) ws_frame(&out, op, (const cx_u8*)queued.p + i + 9, (size_t)n);
            i += 9 + (size_t)n;
        }
        queued.len = 0;
        if (closing && !close_sent) {
            ws_lock(w);
            ws_close_frame(&out, my_code, (const cx_u8*)w->my_reason.p, w->my_reason.len);
            ws_unlock(w);
            close_sent = 1;
            cx_u64 wait = w->timeout_ms < 10000 ? w->timeout_ms : 10000;
            close_by = ws_now_ms() + wait;
        }
        if (!ws_send_some(l, &out, &sent)) {
            ws_end(w, E_CLOSED, CLOSE_ABNORMAL, NULL, 0);
            break;
        }
        if (close_got && out.len == 0 && !ws_unsent(l)) break;
        size_t before = in.len;
        int r = ws_recv_some(l, &in);
        if (in.len > before) {
            heard = ws_now_ms();
            reads++;
            ws_lock(w);
            w->heard_ms = heard;
            ws_unlock(w);
        }
        /* Each whole frame that has come. */
        int fail_code = 0, fail_error = 0;
        while (!fail_code && !close_got) {
            size_t avail = in.len - taken_in;
            const cx_u8* p = (const cx_u8*)in.p + taken_in;
            if (avail < 2) break;
            int fin = p[0] & 0x80, rsv = p[0] & 0x70, op = p[0] & 0x0F, masked = p[1] & 0x80;
            cx_u64 n = p[1] & 0x7F;
            size_t at = 2;
            if (n == 126) {
                if (avail < 4) break;
                n = (cx_u64)p[2] << 8 | p[3], at = 4;
            } else if (n == 127) {
                if (avail < 10) break;
                n = 0;
                for (int k = 0; k < 8; k++) n = n << 8 | p[2 + k];
                at = 10;
            }
            int control = op & 8;
            if (rsv || masked || (n >> 63) || (control && (!fin || n > 125)) || (op > OP_BINARY && op < OP_CLOSE) || op > OP_PONG ||
                (op == OP_CONTINUE && !fragment) || ((op == OP_TEXT || op == OP_BINARY) && fragment)) {
                fail_code = CLOSE_PROTOCOL, fail_error = E_PROTOCOL;
                break;
            }
            if (!control && message.len + n > w->limit) {
                fail_code = CLOSE_TOO_BIG, fail_error = E_TOO_LONG;
                break;
            }
            if (avail - at < n) break;
            const cx_u8* body = p + at;
            taken_in += at + (size_t)n;
            if (op == OP_PING) {
                if (!close_sent) ws_frame(&out, OP_PONG, body, (size_t)n);
            } else if (op == OP_PONG) {
            } else if (op == OP_CLOSE) {
                cx_u32 code = CLOSE_NO_STATUS;
                if (n == 1) {
                    fail_code = CLOSE_PROTOCOL, fail_error = E_PROTOCOL;
                    break;
                }
                if (n >= 2) {
                    code = (cx_u32)body[0] << 8 | body[1];
                    if (!ws_close_code_ok(code)) {
                        fail_code = CLOSE_PROTOCOL, fail_error = E_PROTOCOL;
                        break;
                    }
                    if (!ws_utf8(body + 2, (size_t)n - 2)) {
                        fail_code = CLOSE_NOT_UTF8, fail_error = E_PROTOCOL;
                        break;
                    }
                }
                if (!close_sent) {
                    ws_close_frame(&out, code, NULL, 0);
                    close_sent = 1;
                }
                close_got = 1;
                ws_end(w, E_CLOSED, code, n > 2 ? body + 2 : NULL, n > 2 ? (size_t)n - 2 : 0);
            } else {
                if (op != OP_CONTINUE) fragment = op;
                if (!add(&message, body, (size_t)n)) cx_panic_capacity();
                if (fin) {
                    if (fragment == OP_TEXT && !ws_utf8((const cx_u8*)message.p, message.len)) {
                        fail_code = CLOSE_NOT_UTF8, fail_error = E_PROTOCOL;
                        break;
                    }
                    if (!close_sent) ws_deliver(w, fragment, (const cx_u8*)message.p, message.len);
                    message.len = 0;
                    fragment = 0;
                }
            }
        }
        if (taken_in > 0 && (taken_in == in.len || taken_in > 1 << 20)) {
            memmove(in.p, in.p + taken_in, in.len - taken_in);
            in.len -= taken_in;
            taken_in = 0;
        }
        if (fail_code) {
            /* Close with the code that says why, as far as the connection takes it now. */
            if (!close_sent) {
                ws_close_frame(&out, (cx_u32)fail_code, NULL, 0);
                ws_send_some(l, &out, &sent);
            }
            ws_fail(w, fail_error, 0);
            break;
        }
        if (close_got) {
            /* The answer to the server's close goes out, for a while at most, then it's done. */
            if (close_by == WS_FOREVER) close_by = ws_now_ms() + (w->timeout_ms < 10000 ? w->timeout_ms : 10000);
            if (ws_now_ms() >= close_by) break;
            /* What the frames above added goes now, as Network.framework's poll waits for news, not
             * for room to write. */
            if (out.len && !ws_send_some(l, &out, &sent)) continue;
            if (out.len || ws_unsent(l)) ws_poll(w, l, 1, close_by);
            continue;
        }
        if (r < 0) {
            ws_end(w, E_CLOSED, CLOSE_ABNORMAL, NULL, 0);
            break;
        }
        if (close_sent && ws_now_ms() >= close_by) {
            ws_fail(w, E_TIMEOUT, 0);
            break;
        }
        /* The keepalive: a ping once nothing has come for the interval, and the end if nothing
         * comes for the timeout after it. Anything that comes answers it, not just its pong, as a
         * pong may wait behind a long message. */
        cx_u64 wake_at = close_by;
        if (w->ping_interval_ms && !close_sent) {
            cx_u64 now = ws_now_ms();
            if (pinged && reads != pinged_reads) pinged = 0;
            if (!pinged) {
                cx_u64 due = heard + w->ping_interval_ms;
                if (now >= due) {
                    pings++;
                    cx_u8 body[4] = {(cx_u8)(pings >> 24), (cx_u8)(pings >> 16), (cx_u8)(pings >> 8), (cx_u8)pings};
                    ws_frame(&out, OP_PING, body, 4);
                    pinged = now, pinged_reads = reads;
                    continue;
                }
                if (due < wake_at) wake_at = due;
            } else {
                cx_u64 dead = pinged + w->ping_timeout_ms;
                if (now >= dead) {
                    /* Gone quiet: told so, as far as the connection takes it now, and let go. */
                    static const char why[] = "keepalive ping timeout";
                    ws_close_frame(&out, CLOSE_SERVER_ERROR, (const cx_u8*)why, sizeof why - 1);
                    ws_send_some(l, &out, &sent);
                    ws_fail(w, E_TIMEOUT, 0);
                    break;
                }
                if (dead < wake_at) wake_at = dead;
            }
        }
        if (r == 0) {
            /* A pong the frames above added goes before the wait, as in the close's case. */
            if (out.len && !ws_send_some(l, &out, &sent)) continue;
            ws_poll(w, l, out.len > 0 || ws_unsent(l), wake_at);
        }
    }
    free(out.p), free(in.p), free(message.p), free(queued.p);
}

/* Connects, makes the handshake, then reads and writes frames until the connection ends. */
static void ws_run(wsock* w) {
    cx_u64 deadline = ws_now_ms() + w->timeout_ms;
    ws_link link;
    cx_i32 code = 0;
    int e = ws_connect(w, &link, deadline, &code);
    if (e) {
        ws_fail(w, e, code);
        return;
    }
    ws_talk(w, &link, deadline);
    ws_disconnect(&link);
}

#ifdef _WIN32
static DWORD WINAPI ws_thread_main(LPVOID p) {
    ws_run(p);
    return 0;
}
#else
static void* ws_thread_main(void* p) {
    ws_run(p);
    return NULL;
}
#endif

/* Starts connecting to `url`, `ws://` or `wss://`, with the caller's header lines, each ending in
 * "\r\n", which std.http.websocket has checked. The connection may take `timeout_ms` to open, a
 * message may be `limit` bytes long, and with a ping interval it's pinged when it's been quiet
 * that long, and ends if it stays quiet for the ping timeout after. */
cx_u64 cx_http_ws_open(const cx_u8* url, cx_usize url_len, const cx_u8* head, cx_usize head_len, cx_u32 timeout_ms, cx_usize limit,
                       cx_u32 ping_interval_ms, cx_u32 ping_timeout_ms) {
    wsock* w = calloc(1, sizeof(wsock));
    if (!w) cx_panic_capacity();
    ws_init_lock(w);
    w->timeout_ms = timeout_ms;
    w->limit = limit;
    w->ping_interval_ms = ping_interval_ms;
    w->ping_timeout_ms = ping_timeout_ms;
    w->head = dup_text((const char*)head, head_len);
    w->state = WS_CONNECTING;
#ifndef _WIN32
    w->wake[0] = w->wake[1] = -1;
#endif
    /* ws:// is http:// and wss:// is https:// to std.http's URL reader. */
    request r = {0};
    int ok = 0;
    if (url_len >= 6 && starts_ci((const char*)url, url_len, "wss://")) {
        char* u = malloc(url_len + 3);
        if (!u) cx_panic_capacity();
        memcpy(u, "https://", 8);
        memcpy(u + 8, url + 6, url_len - 6);
        ok = parse_url(&r, u, url_len + 2);
        free(u);
    } else if (url_len >= 5 && starts_ci((const char*)url, url_len, "ws://")) {
        char* u = malloc(url_len + 3);
        if (!u) cx_panic_capacity();
        memcpy(u, "http://", 7);
        memcpy(u + 7, url + 5, url_len - 5);
        ok = parse_url(&r, u, url_len + 2);
        free(u);
    }
    w->url = r.url, w->host = r.host, w->target = r.target, w->port = r.port, w->tls = r.https;
    int error = ok ? 0 : E_BAD_URL;
#ifdef _WIN32
    if (!error) {
        w->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!w->wake) error = E_OS, w->code = (cx_i32)GetLastError();
    }
#else
#ifndef CX_IOS
    if (!error) {
        once(&curl_state, curl_load);
        if (!curl_loaded || !curl.easy_send || !curl.easy_recv) error = E_UNAVAILABLE;
    }
#endif
    if (!error) {
        if (pipe(w->wake) != 0) {
            error = E_OS, w->code = errno;
            w->wake[0] = w->wake[1] = -1;
        } else {
            for (int i = 0; i < 2; i++) {
                fcntl(w->wake[i], F_SETFD, FD_CLOEXEC);
                fcntl(w->wake[i], F_SETFL, fcntl(w->wake[i], F_GETFL) | O_NONBLOCK);
            }
        }
    }
#endif
    if (!error) {
        int e = cx_os_thread(&w->thread, ws_thread_main, w);
        if (e == 0) w->started = 1;
        else error = E_OS, w->code = (cx_i32)e;
    }
    if (error) {
        w->state = WS_CLOSED;
        w->error = error;
    }
    return table_add(&sockets, w);
}

static wsock* ws_of(cx_u64 h) { return table_find(&sockets, h); }

/* Wakes the connection's thread to send what's queued. */
static void ws_wake(wsock* w) {
#ifdef _WIN32
    if (w->wake) SetEvent(w->wake);
#else
    char b = 1;
    ssize_t n = write(w->wake[1], &b, 1);
    (void)n;
#endif
}

/* 0 while it's connecting, 1 once it's open, 2 once it's closed, and -1 if it was let go. */
cx_i32 cx_http_ws_state(cx_u64 h) {
    wsock* w = ws_of(h);
    if (!w) return -1;
    ws_lock(w);
    int s = w->state;
    ws_unlock(w);
    return s;
}

/* Waits while it's connecting, and gives what went wrong, 0 if it's open. */
cx_i32 cx_http_ws_wait_open(cx_u64 h) {
    wsock* w = ws_of(h);
    if (!w) return -1;
    ws_lock(w);
    while (w->state == WS_CONNECTING) ws_sleep(w, WS_FOREVER);
    int e = w->state == WS_OPEN ? 0 : w->error;
    ws_unlock(w);
    return e;
}

/* The milliseconds since anything last came from the server, or since it opened: 0 while it's
 * opening, or if it never opened. */
cx_u64 cx_http_ws_idle_ms(cx_u64 h) {
    wsock* w = ws_of(h);
    if (!w) return 0;
    ws_lock(w);
    cx_u64 heard = w->heard_ms;
    ws_unlock(w);
    if (!heard) return 0;
    cx_u64 now = ws_now_ms();
    return now > heard ? now - heard : 0;
}

/* Sends a message, OP_TEXT or OP_BINARY, or queues it while the connection opens: 0, or what the
 * connection ended with. */
cx_i32 cx_http_ws_send(cx_u64 h, cx_u32 op, const cx_u8* p, cx_usize n) {
    wsock* w = ws_of(h);
    if (!w) return -1;
    ws_lock(w);
    int state = w->state, error = w->error;
    if (state == WS_CLOSED || w->closing) {
        ws_unlock(w);
        return state == WS_CLOSED ? error : E_CLOSED;
    }
    cx_u8 rec[9];
    rec[0] = (cx_u8)op;
    for (int k = 0; k < 8; k++) rec[1 + k] = (cx_u8)((cx_u64)n >> (56 - 8 * k));
    if (!add(&w->out, rec, 9) || (n && !add(&w->out, p, n))) cx_panic_capacity();
    ws_unlock(w);
    ws_wake(w);
    return 0;
}

/* Takes the next message, waiting up to `timeout_ms` for one: its opcode, with its bytes for
 * cx_http_ws_taken; 0 if none came in the time; or, once the connection has ended and every
 * message before the end is taken, what it ended with, as a negative number. */
cx_i32 cx_http_ws_receive(cx_u64 h, cx_u32 timeout_ms) {
    wsock* w = ws_of(h);
    if (!w) return -1000;
    cx_u64 deadline = ws_now_ms() + timeout_ms;
    ws_lock(w);
    for (;;) {
        if (w->first) {
            ws_message* m = w->first;
            w->first = m->next;
            if (!w->first) w->last = NULL;
            w->taken.len = 0;
            if (m->len && !add(&w->taken, m->data, m->len)) cx_panic_capacity();
            int kind = m->kind;
            free(m);
            ws_unlock(w);
            return kind;
        }
        if (w->state == WS_CLOSED) {
            int e = w->error;
            ws_unlock(w);
            return -e;
        }
        if (!ws_sleep(w, deadline)) break;
    }
    ws_unlock(w);
    return 0;
}

cx_usize cx_http_ws_taken_len(cx_u64 h) { return ws_of(h)->taken.len; }

void cx_http_ws_taken(cx_u64 h, cx_u8* out) {
    wsock* w = ws_of(h);
    if (w->taken.len) memcpy(out, w->taken.p, w->taken.len);
    w->taken.len = 0;
}

/* What the connection ended with, beyond its kind: the system's code for E_OS, the handshake's
 * status for E_REFUSED, and the close's code for E_CLOSED. */
cx_i32 cx_http_ws_code(cx_u64 h) {
    wsock* w = ws_of(h);
    ws_lock(w);
    cx_i32 c = w->error == E_REFUSED ? (cx_i32)w->status : w->error == E_CLOSED ? (cx_i32)w->close_code : w->code;
    ws_unlock(w);
    return c;
}

/* The close's reason, or the protocol the server chose: 0 for the reason, 1 for the protocol. */
cx_usize cx_http_ws_text_len(cx_u64 h, cx_u32 which) {
    wsock* w = ws_of(h);
    ws_lock(w);
    size_t n = which ? w->protocol.len : w->close_reason.len;
    ws_unlock(w);
    return n;
}

void cx_http_ws_text(cx_u64 h, cx_u32 which, cx_u8* out) {
    wsock* w = ws_of(h);
    ws_lock(w);
    bytes* b = which ? &w->protocol : &w->close_reason;
    if (b->len) memcpy(out, b->p, b->len);
    ws_unlock(w);
}

/* Closes the connection with `code` and `reason`, waiting for the server to answer for as long
 * as the connection's timeout, up to 10 seconds, and lets it go. */
void cx_http_ws_close(cx_u64 h, cx_u32 code, const cx_u8* reason, cx_usize reason_len) {
    wsock* w = ws_of(h);
    if (!w) return;
    table_remove(&sockets, h);
    cx_u64 wait = w->timeout_ms < 10000 ? w->timeout_ms : 10000;
    ws_lock(w);
    int was = w->state;
    if (!w->closing) {
        w->closing = 1;
        w->my_code = code;
        if (reason_len && !add(&w->my_reason, reason, reason_len > 123 ? 123 : reason_len)) cx_panic_capacity();
    }
    ws_unlock(w);
    ws_wake(w);
    if (w->started) {
        /* The thread ends once the server answers, the time's up, or it fails. */
        cx_u64 deadline = ws_now_ms() + wait + (was == WS_CONNECTING ? w->timeout_ms : 0);
        ws_lock(w);
        while (w->state != WS_CLOSED && ws_sleep(w, deadline)) {
        }
        ws_unlock(w);
#ifdef _WIN32
        WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
#else
        pthread_join(w->thread, NULL);
#endif
    }
    ws_free(w);
}
