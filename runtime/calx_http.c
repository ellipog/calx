/* HTTP and HTTPS requests behind std.http (D118), on the system's own client so that certificates
 * are checked as the system checks them: WinHTTP on Windows, and libcurl on Linux and macOS,
 * loaded with dlopen when the first request starts, so a program that makes none doesn't need it
 * installed. Every macOS has it; on Linux it's a package.
 * Linked only into programs that call them.
 *
 * Each request runs on a thread of its own, the way vanilla asks Mojang's session server on an
 * authenticator thread, so the program's loop goes on while it waits and asks `cx_http_done` when
 * to take the response. Requests are slots in a table, named by an index and a generation, so a
 * request whose response was taken can't be taken again: the Calx side panics instead.
 *
 * Both clients are set up to send the same request: HTTP/1.1, no proxy (as vanilla's dedicated
 * server uses none), redirects followed up to 10 but never from HTTPS to HTTP, no Accept or
 * Accept-Encoding unless the caller adds them, and a User-Agent of "Calx" unless the caller gives
 * one. The timeout is for the whole request. The response's header lines come back as UTF-8
 * "name:value\r\n", the value trimmed, its bytes read as Latin-1 as Java reads them.
 *
 * WebSockets, behind std.http.websocket, are at the end of the file, on the same clients. */
#include "calx_rt.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
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

typedef struct {
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
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    int started;
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

/* --- Windows: WinHTTP ---------------------------------------------------------------------- */

#ifdef _WIN32

static HINTERNET session;
static cx_u32 session_state;

static void session_open(void) {
    session = WinHttpOpen(L"Calx", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
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
    case ERROR_WINHTTP_RESPONSE_DRAIN_OVERFLOW: return E_PROTOCOL;
    default: return E_OS;
    }
}

/* What's left of the time, at least 1 ms, since WinHTTP takes 0 as no limit. */
static DWORD left(ULONGLONG deadline) {
    ULONGLONG now = GetTickCount64();
    return now + 1 >= deadline ? 1 : (DWORD)(deadline - now);
}

static void run(request* r) {
    once(&session_state, session_open);
    if (!session) {
        r->error = E_OS, r->code = (cx_i32)GetLastError();
        return;
    }
    ULONGLONG deadline = GetTickCount64() + r->timeout_ms;
    wchar_t* host = widen(r->host);
    wchar_t* target = widen(r->target);
    wchar_t* head = widen(r->head);
    HINTERNET c = NULL, q = NULL;
    DWORD e = 0;
    c = WinHttpConnect(session, host, (INTERNET_PORT)r->port, 0);
    if (!c) goto failed;
    q = WinHttpOpenRequest(c, r->post ? L"POST" : L"GET", target, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                           r->https ? WINHTTP_FLAG_SECURE : 0);
    if (!q) goto failed;
    int t = (int)r->timeout_ms;
    if (!WinHttpSetTimeouts(q, t, t, t, t)) goto failed;
    DWORD response_timeout = r->timeout_ms;
    if (!WinHttpSetOption(q, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &response_timeout, sizeof response_timeout)) goto failed;
    if (!WinHttpSendRequest(q, head[0] ? head : WINHTTP_NO_ADDITIONAL_HEADERS, head[0] ? (DWORD)-1L : 0, r->body,
                            (DWORD)r->body_len, (DWORD)r->body_len, 0)) {
        goto failed;
    }
    DWORD ms = left(deadline);
    WinHttpSetOption(q, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &ms, sizeof ms);
    WinHttpSetOption(q, WINHTTP_OPTION_RECEIVE_TIMEOUT, &ms, sizeof ms);
    if (!WinHttpReceiveResponse(q, NULL)) goto failed;
    DWORD status = 0, size = sizeof status;
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
    goto done;
failed:
    e = GetLastError();
    r->error = win_error(e), r->code = (cx_i32)e;
done:
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    free(host), free(target), free(head);
}

#else

/* --- Linux: libcurl, loaded at run time ---------------------------------------------------- */

/* libcurl's options and its one info, by the numbers its ABI gives them. */
enum {
    O_WRITEDATA = 10001, O_URL = 10002, O_PROXY = 10004, O_POSTFIELDS = 10015, O_USERAGENT = 10018,
    O_HTTPHEADER = 10023, O_HEADERDATA = 10029, O_WRITEFUNCTION = 20011, O_HEADERFUNCTION = 20079,
    O_POST = 47, O_FOLLOWLOCATION = 52, O_MAXREDIRS = 68, O_HTTPGET = 80, O_HTTP_VERSION = 84,
    O_NOSIGNAL = 99, O_TIMEOUT_MS = 155, O_PROTOCOLS = 181, O_REDIR_PROTOCOLS = 182,
    O_POSTFIELDSIZE_LARGE = 30120, I_RESPONSE_CODE = 0x200002
};

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
} curl;
static int curl_loaded;
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
    /* Not safe to call while another thread uses libcurl, so it's done here, once, before any
     * request's thread starts. A write to a closed TLS connection would raise SIGPIPE, which a
     * server never wants. */
    if (curl.global_init(3) != 0) return;
    struct sigaction sa;
    if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) signal(SIGPIPE, SIG_IGN);
    curl_loaded = 1;
}

static int curl_error(int e) {
    switch (e) {
    case 1:
    case 3: return E_BAD_URL;
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
    if (k >= 5 && memcmp(p, "HTTP/", 5) == 0) r->raw.len = 0;
    return add(&r->raw, p, k) ? k : 0;
}

static void run(request* r) {
    void* h = curl.easy_init();
    if (!h) {
        r->error = E_OS, r->code = ENOMEM;
        return;
    }
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
    curl.easy_setopt(h, O_HTTP_VERSION, 2L);
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
    int e = curl.easy_perform(h);
    if (e != 0 && r->error != E_TOO_LONG) {
        r->error = curl_error(e), r->code = e;
    } else if (e == 0) {
        long status = 0;
        curl.easy_getinfo(h, I_RESPONSE_CODE, &status);
        r->status = (cx_u32)status;
        if (!tidy_headers(r, 1)) cx_panic_capacity();
    }
    curl.easy_cleanup(h);
    curl.slist_free_all(list);
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
                     cx_usize body_len, cx_u32 timeout_ms, cx_usize limit) {
    request* r = calloc(1, sizeof(request));
    if (!r) cx_panic_capacity();
    r->post = post;
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
#ifndef _WIN32
    else {
        once(&curl_state, curl_load);
        if (!curl_loaded) r->error = E_UNAVAILABLE;
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
        pthread_join(r->thread, NULL);
#endif
        r->started = 0;
    }
    return r->error;
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

/* --- WebSockets, behind std.http.websocket (CLX-320) ---------------------------------------- *
 *
 * A WebSocket client, RFC 6455. Each connection runs on a thread of its own, which connects,
 * makes the handshake, then reads messages into a queue the program takes them from, answering
 * pings and the server's close by itself. Text is checked to be UTF-8, and a message longer than
 * the limit, or anything that breaks the protocol, closes the connection with the code RFC 6455
 * gives for it.
 *
 * On Linux and macOS, libcurl makes the connection, TCP and TLS with the system's certificate
 * checks, in its connect-only mode, and the handshake and the frames are written here: libcurl's
 * own WebSocket API is missing from most systems' builds. The thread does all the reading and
 * writing, as libcurl and TLS allow one thread at a time on a connection. What the program sends
 * goes into a queue for it, and a byte down a pipe wakes it.
 *
 * On Windows, WinHTTP's WebSocket API does the protocol. The thread receives, and the program's
 * sends go straight out under a lock, once the connection is open, or are kept until then. */

/* Beyond std.http's: the server answered the handshake with another status, and the connection
 * is closed, with the code and reason of the close. */
enum { E_REFUSED = 10, E_CLOSED = 11 };
enum { WS_CONNECTING, WS_OPEN, WS_CLOSED };
enum { OP_CONTINUE = 0, OP_TEXT = 1, OP_BINARY = 2, OP_CLOSE = 8, OP_PING = 9, OP_PONG = 10 };
/* Close codes, RFC 6455 section 7.4.1. */
enum { CLOSE_NORMAL = 1000, CLOSE_PROTOCOL = 1002, CLOSE_NO_STATUS = 1005, CLOSE_ABNORMAL = 1006, CLOSE_NOT_UTF8 = 1007,
       CLOSE_TOO_BIG = 1009 };

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
#ifdef _WIN32
    SRWLOCK mu;
    CONDITION_VARIABLE cv;
    HANDLE thread;
    SRWLOCK send_mu; /* one send at a time on `ws`, the program's or the thread's */
    HINTERNET ws;
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
    InitializeSRWLock(&w->send_mu);
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
#ifndef _WIN32
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

#ifndef _WIN32

/* --- WebSockets on libcurl ------------------------------------------------------------------ */

/* Whether a close frame may carry `code`: RFC 6455 section 7.4, with 1012 to 1014, which IANA
 * has registered since, and the codes for libraries and applications, 3000 to 4999. */
static int ws_close_code_ok(cx_u32 code) {
    if (code >= 3000 && code <= 4999) return 1;
    return code >= 1000 && code <= 1014 && code != 1004 && code != CLOSE_NO_STATUS && code != CLOSE_ABNORMAL;
}


enum { O_CONNECT_ONLY = 141, I_ACTIVESOCKET = 0x500000 + 44, I_LASTSOCKET = 0x200000 + 29, E_CURL_AGAIN = 81 };

/* Random bytes from the system, for the handshake's key and each frame's mask. */
static void ws_random(cx_u8* p, size_t n) {
#ifdef __APPLE__
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

/* Waits for the socket, or the wake pipe, until `deadline`. */
static void ws_poll(wsock* w, int sock, int writing, cx_u64 deadline) {
    struct pollfd fds[2] = {{sock, (short)(POLLIN | (writing ? POLLOUT : 0)), 0}, {w->wake[0], POLLIN, 0}};
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

/* Sends what's in `out` from `*at`, as far as the connection takes it now. 0 if it failed. */
static int ws_send_some(void* h, bytes* out, size_t* at) {
    while (*at < out->len) {
        size_t n = 0;
        int e = curl.easy_send(h, out->p + *at, out->len - *at, &n);
        if (e == E_CURL_AGAIN) break;
        if (e != 0) return 0;
        *at += n;
    }
    if (*at == out->len) out->len = 0, *at = 0;
    return 1;
}

/* Reads what has come onto `in`. 1 if anything came, 0 if nothing has, -1 if the server closed
 * the connection or it failed. */
static int ws_recv_some(void* h, bytes* in) {
    int got_any = 0;
    for (;;) {
        if (!grow(in, 65536)) cx_panic_capacity();
        size_t n = 0;
        int e = curl.easy_recv(h, in->p + in->len, 65536, &n);
        if (e == E_CURL_AGAIN) return got_any;
        if (e != 0 || n == 0) return -1;
        in->len += n;
        got_any = 1;
    }
}

/* Connects, makes the handshake, then reads and writes frames until the connection ends. */
static void ws_run(wsock* w) {
    void* h = curl.easy_init();
    if (!h) {
        ws_fail(w, E_OS, ENOMEM);
        return;
    }
    cx_u64 deadline = ws_now_ms() + w->timeout_ms;
    bytes out = {0}, in = {0}, message = {0};
    curl.easy_setopt(h, O_NOSIGNAL, 1L);
    curl.easy_setopt(h, O_URL, w->url);
    curl.easy_setopt(h, O_PROXY, "");
    curl.easy_setopt(h, O_HTTP_VERSION, 2L); /* HTTP/1.1, so TLS's ALPN offers nothing else */
    curl.easy_setopt(h, O_PROTOCOLS, 3L);
    curl.easy_setopt(h, O_CONNECT_ONLY, 1L);
    curl.easy_setopt(h, O_TIMEOUT_MS, (long)w->timeout_ms);
    int e = curl.easy_perform(h);
    if (e != 0) {
        ws_fail(w, curl_error(e), e);
        curl.easy_cleanup(h);
        return;
    }
    long sock_long = -1;
    int sock = -1;
    if (curl.easy_getinfo(h, I_ACTIVESOCKET, &sock) != 0 || sock < 0) {
        if (curl.easy_getinfo(h, I_LASTSOCKET, &sock_long) == 0) sock = (int)sock_long;
    }
    if (sock < 0) {
        ws_fail(w, E_OS, 0);
        curl.easy_cleanup(h);
        return;
    }

    /* The handshake. */
    cx_u8 nonce[16];
    ws_random(nonce, sizeof nonce);
    char key[25];
    ws_base64(nonce, sizeof nonce, key);
    int bracket = strchr(w->host, ':') != NULL;
    char port[8] = "";
    if (w->port != (w->tls ? 443u : 80u)) {
        char digits[6];
        int k = 0;
        for (unsigned v = w->port; v; v /= 10) digits[k++] = (char)('0' + v % 10);
        port[0] = ':';
        for (int i = 0; i < k; i++) port[1 + i] = digits[k - 1 - i];
        port[1 + k] = 0;
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
        if (!ws_send_some(h, &out, &sent)) {
            failed = E_PROTOCOL;
            break;
        }
        int r = ws_recv_some(h, &in);
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
        if (r == 0) ws_poll(w, sock, out.len > 0, deadline);
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
                ws_signal(w);
                ws_unlock(w);
            }
        }
    }
    if (failed) {
        ws_fail(w, failed, 0);
        free(out.p), free(in.p);
        curl.easy_cleanup(h);
        return;
    }
    /* What came after the handshake's head is the first frames. */
    memmove(in.p, in.p + head_end, in.len - head_end);
    in.len -= head_end;

    /* Frames, until the connection ends. `fragment` is the opcode of the message whose frames are
     * coming, or 0. */
    int fragment = 0, close_sent = 0, close_got = 0;
    cx_u64 close_by = WS_FOREVER;
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
        if (!ws_send_some(h, &out, &sent)) {
            ws_end(w, E_CLOSED, CLOSE_ABNORMAL, NULL, 0);
            break;
        }
        if (close_got && out.len == 0) break;
        int r = ws_recv_some(h, &in);
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
                ws_send_some(h, &out, &sent);
            }
            ws_fail(w, fail_error, 0);
            break;
        }
        if (close_got) {
            /* The answer to the server's close goes out, for a while at most, then it's done. */
            if (close_by == WS_FOREVER) close_by = ws_now_ms() + (w->timeout_ms < 10000 ? w->timeout_ms : 10000);
            if (ws_now_ms() >= close_by) break;
            if (out.len) ws_poll(w, sock, 1, close_by);
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
        if (r == 0) ws_poll(w, sock, out.len > 0, close_by);
    }
    free(out.p), free(in.p), free(message.p), free(queued.p);
    curl.easy_cleanup(h);
}

#else

/* --- WebSockets on WinHTTP ------------------------------------------------------------------ */

#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif

/* WinHTTP's WebSocket calls, from Windows 8, looked up rather than linked, as Calx's headers
 * are Windows 7's. */
static struct {
    HINTERNET(WINAPI* complete_upgrade)(HINTERNET, DWORD_PTR);
    DWORD(WINAPI* send)(HINTERNET, int, void*, DWORD);
    DWORD(WINAPI* receive)(HINTERNET, void*, DWORD, DWORD*, int*);
    DWORD(WINAPI* shutdown)(HINTERNET, USHORT, void*, DWORD);
    DWORD(WINAPI* query_close)(HINTERNET, USHORT*, void*, DWORD, DWORD*);
} wsapi;
static int wsapi_loaded;
static cx_u32 wsapi_state;

static void wsapi_load(void) {
    HMODULE m = GetModuleHandleW(L"winhttp.dll");
    if (!m) m = LoadLibraryW(L"winhttp.dll");
    if (!m) return;
    FARPROC fns[5] = {GetProcAddress(m, "WinHttpWebSocketCompleteUpgrade"), GetProcAddress(m, "WinHttpWebSocketSend"),
                      GetProcAddress(m, "WinHttpWebSocketReceive"), GetProcAddress(m, "WinHttpWebSocketShutdown"),
                      GetProcAddress(m, "WinHttpWebSocketQueryCloseStatus")};
    for (int i = 0; i < 5; i++) {
        if (!fns[i]) return;
    }
    memcpy(&wsapi.complete_upgrade, &fns[0], sizeof(FARPROC));
    memcpy(&wsapi.send, &fns[1], sizeof(FARPROC));
    memcpy(&wsapi.receive, &fns[2], sizeof(FARPROC));
    memcpy(&wsapi.shutdown, &fns[3], sizeof(FARPROC));
    memcpy(&wsapi.query_close, &fns[4], sizeof(FARPROC));
    wsapi_loaded = 1;
}

/* WinHTTP's buffer types. */
enum { BT_BINARY = 0, BT_BINARY_FRAGMENT = 1, BT_TEXT = 2, BT_TEXT_FRAGMENT = 3, BT_CLOSE = 4 };

/* Sends a message the program queued before the connection was open, or sends it now. Holds
 * send_mu. */
static DWORD ws_win_send(wsock* w, int op, const cx_u8* p, size_t n) {
    return wsapi.send(w->ws, op == OP_TEXT ? BT_TEXT : BT_BINARY, (void*)p, (DWORD)n);
}

static void ws_run(wsock* w) {
    once(&session_state, session_open);
    once(&wsapi_state, wsapi_load);
    if (!session || !wsapi_loaded) {
        ws_fail(w, !session ? E_OS : E_UNAVAILABLE, (cx_i32)GetLastError());
        return;
    }
    wchar_t* host = widen(w->host);
    wchar_t* target = widen(w->target);
    wchar_t* head = widen(w->head);
    HINTERNET c = NULL, q = NULL, ws = NULL;
    DWORD e = 0;
    int t = (int)w->timeout_ms;
    c = WinHttpConnect(session, host, (INTERNET_PORT)w->port, 0);
    if (!c) goto failed;
    q = WinHttpOpenRequest(c, L"GET", target, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, w->tls ? WINHTTP_FLAG_SECURE : 0);
    if (!q) goto failed;
    if (!WinHttpSetTimeouts(q, t, t, t, t)) goto failed;
    if (!WinHttpSetOption(q, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) goto failed;
    if (!WinHttpSendRequest(q, head[0] ? head : WINHTTP_NO_ADDITIONAL_HEADERS, head[0] ? (DWORD)-1L : 0, NULL, 0, 0, 0)) goto failed;
    if (!WinHttpReceiveResponse(q, NULL)) goto failed;
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        goto failed;
    }
    if (status != 101) {
        ws_lock(w);
        w->status = status;
        ws_unlock(w);
        ws_fail(w, E_REFUSED, 0);
        goto done;
    }
    wchar_t proto[256];
    size = sizeof proto;
    if (WinHttpQueryHeaders(q, WINHTTP_QUERY_CUSTOM, L"Sec-WebSocket-Protocol", proto, &size, WINHTTP_NO_HEADER_INDEX)) {
        char p8[768];
        int n8 = WideCharToMultiByte(CP_UTF8, 0, proto, (int)(size / sizeof(wchar_t)), p8, sizeof p8, NULL, NULL);
        ws_lock(w);
        if (n8 > 0 && !add(&w->protocol, p8, (size_t)n8)) cx_panic_capacity();
        ws_unlock(w);
    }
    /* A receive waits as long as it takes, since a WebSocket may be quiet for hours: WinHTTP
     * takes a timeout of 0 as none. The WebSocket's handle starts with the request's. */
    DWORD forever = 0;
    WinHttpSetTimeouts(q, t, t, t, 0);
    ws = wsapi.complete_upgrade(q, 0);
    if (!ws) goto failed;
    WinHttpCloseHandle(q);
    q = NULL;
    WinHttpSetOption(ws, WINHTTP_OPTION_RECEIVE_TIMEOUT, &forever, sizeof forever);
    /* Open: what the program queued goes first. */
    AcquireSRWLockExclusive(&w->send_mu);
    ws_lock(w);
    w->ws = ws;
    bytes queued = w->out;
    w->out = (bytes){0};
    int closing = w->closing;
    if (w->state == WS_CONNECTING) w->state = WS_OPEN;
    ws_signal(w);
    ws_unlock(w);
    for (size_t i = 0; i + 9 <= queued.len;) {
        int op = (cx_u8)queued.p[i];
        cx_u64 n = 0;
        for (int k = 0; k < 8; k++) n = n << 8 | (cx_u8)queued.p[i + 1 + k];
        if (!closing) ws_win_send(w, op, (const cx_u8*)queued.p + i + 9, (size_t)n);
        i += 9 + (size_t)n;
    }
    free(queued.p);
    if (closing) {
        ws_lock(w);
        wsapi.shutdown(ws, (USHORT)w->my_code, w->my_reason.len ? w->my_reason.p : NULL, (DWORD)w->my_reason.len);
        ws_unlock(w);
    }
    ReleaseSRWLockExclusive(&w->send_mu);
    /* Messages, until the connection ends. */
    bytes message = {0};
    for (;;) {
        if (!grow(&message, 65536)) cx_panic_capacity();
        DWORD got = 0;
        int type = 0;
        DWORD r = wsapi.receive(ws, message.p + message.len, 65536, &got, &type);
        if (r != 0) {
            /* The handle closed under it by `close`, or the connection lost. */
            ws_end(w, E_CLOSED, CLOSE_ABNORMAL, NULL, 0);
            break;
        }
        if (type == BT_CLOSE) {
            USHORT code = CLOSE_NO_STATUS;
            char reason[123];
            DWORD reason_len = 0;
            if (wsapi.query_close(ws, &code, reason, sizeof reason, &reason_len) != 0) code = CLOSE_NO_STATUS, reason_len = 0;
            AcquireSRWLockExclusive(&w->send_mu);
            ws_lock(w);
            int answered = w->closing;
            ws_unlock(w);
            if (!answered) wsapi.shutdown(ws, code == CLOSE_NO_STATUS ? CLOSE_NORMAL : code, NULL, 0);
            ReleaseSRWLockExclusive(&w->send_mu);
            ws_end(w, E_CLOSED, code, (const cx_u8*)reason, reason_len);
            break;
        }
        message.len += got;
        if (message.len > w->limit) {
            AcquireSRWLockExclusive(&w->send_mu);
            wsapi.shutdown(ws, CLOSE_TOO_BIG, NULL, 0);
            ReleaseSRWLockExclusive(&w->send_mu);
            ws_fail(w, E_TOO_LONG, 0);
            break;
        }
        if (type == BT_TEXT || type == BT_BINARY) {
            if (type == BT_TEXT && !ws_utf8((const cx_u8*)message.p, message.len)) {
                AcquireSRWLockExclusive(&w->send_mu);
                wsapi.shutdown(ws, CLOSE_NOT_UTF8, NULL, 0);
                ReleaseSRWLockExclusive(&w->send_mu);
                ws_fail(w, E_PROTOCOL, 0);
                break;
            }
            ws_deliver(w, type == BT_TEXT ? OP_TEXT : OP_BINARY, (const cx_u8*)message.p, message.len);
            message.len = 0;
        }
    }
    free(message.p);
    goto done;
failed:
    e = GetLastError();
    ws_fail(w, win_error(e), (cx_i32)e);
done:
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    free(host), free(target), free(head);
}

#endif

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
 * "\r\n", which std.http.websocket has checked. The connection may take `timeout_ms` to open, and
 * a message may be `limit` bytes long. */
cx_u64 cx_http_ws_open(const cx_u8* url, cx_usize url_len, const cx_u8* head, cx_usize head_len, cx_u32 timeout_ms, cx_usize limit) {
    wsock* w = calloc(1, sizeof(wsock));
    if (!w) cx_panic_capacity();
    ws_init_lock(w);
    w->timeout_ms = timeout_ms;
    w->limit = limit;
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
#ifndef _WIN32
    if (!error) {
        once(&curl_state, curl_load);
        if (!curl_loaded || !curl.easy_send || !curl.easy_recv) error = E_UNAVAILABLE;
    }
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
#ifndef _WIN32
    char b = 1;
    ssize_t n = write(w->wake[1], &b, 1);
    (void)n;
#else
    (void)w;
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

/* Sends a message, OP_TEXT or OP_BINARY, or queues it while the connection opens: 0, or what the
 * connection ended with. */
cx_i32 cx_http_ws_send(cx_u64 h, cx_u32 op, const cx_u8* p, cx_usize n) {
    wsock* w = ws_of(h);
    if (!w) return -1;
#ifdef _WIN32
    AcquireSRWLockExclusive(&w->send_mu);
#endif
    ws_lock(w);
    int state = w->state, error = w->error;
    if (state == WS_CLOSED || w->closing) {
        ws_unlock(w);
#ifdef _WIN32
        ReleaseSRWLockExclusive(&w->send_mu);
#endif
        return state == WS_CLOSED ? error : E_CLOSED;
    }
#ifdef _WIN32
    if (state == WS_OPEN) {
        ws_unlock(w);
        DWORD e = ws_win_send(w, (int)op, p, n);
        ReleaseSRWLockExclusive(&w->send_mu);
        return e == 0 ? 0 : E_CLOSED;
    }
#endif
    cx_u8 rec[9];
    rec[0] = (cx_u8)op;
    for (int k = 0; k < 8; k++) rec[1 + k] = (cx_u8)((cx_u64)n >> (56 - 8 * k));
    if (!add(&w->out, rec, 9) || (n && !add(&w->out, p, n))) cx_panic_capacity();
    ws_unlock(w);
#ifdef _WIN32
    ReleaseSRWLockExclusive(&w->send_mu);
#endif
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
#ifdef _WIN32
    AcquireSRWLockExclusive(&w->send_mu);
#endif
    ws_lock(w);
    int was = w->state;
    if (!w->closing) {
        w->closing = 1;
        w->my_code = code;
        if (reason_len && !add(&w->my_reason, reason, reason_len > 123 ? 123 : reason_len)) cx_panic_capacity();
    }
#ifdef _WIN32
    HINTERNET ws = w->ws;
    if (was == WS_OPEN && ws) wsapi.shutdown(ws, (USHORT)code, w->my_reason.len ? w->my_reason.p : NULL, (DWORD)w->my_reason.len);
#endif
    ws_unlock(w);
#ifdef _WIN32
    ReleaseSRWLockExclusive(&w->send_mu);
#endif
    ws_wake(w);
    if (w->started) {
        /* The thread ends once the server answers, the time's up, or it fails. */
        cx_u64 deadline = ws_now_ms() + wait + (was == WS_CONNECTING ? w->timeout_ms : 0);
        ws_lock(w);
        while (w->state != WS_CLOSED && ws_sleep(w, deadline)) {
        }
        ws_unlock(w);
#ifdef _WIN32
        /* A receive still waiting returns once its handle closes. */
        if (w->ws) WinHttpCloseHandle(w->ws);
        WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
#else
        pthread_join(w->thread, NULL);
#endif
    }
    ws_free(w);
}
