/* HTTP and HTTPS requests behind std.http (D118), on the system's own client so that certificates
 * are checked as the system checks them: WinHTTP on Windows, and libcurl on Linux, loaded with
 * dlopen when the first request starts, so a program that makes none doesn't need it installed.
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
 * "name:value\r\n", the value trimmed, its bytes read as Latin-1 as Java reads them. */
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
#include <pthread.h>
#include <signal.h>
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

/* --- the table of requests ----------------------------------------------------------------- */

typedef struct {
    request* r;
    cx_u32 gen;
} slot;

static slot* slots;
static cx_u32 slot_count;
#ifdef _WIN32
static SRWLOCK table_lock = SRWLOCK_INIT;
static void lock(void) { AcquireSRWLockExclusive(&table_lock); }
static void unlock(void) { ReleaseSRWLockExclusive(&table_lock); }
#else
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&table_lock); }
static void unlock(void) { pthread_mutex_unlock(&table_lock); }
#endif

static cx_u64 table_add(request* r) {
    lock();
    cx_u32 i = 0;
    while (i < slot_count && slots[i].r) i++;
    if (i == slot_count) {
        cx_u32 n = slot_count ? slot_count * 2 : 16;
        slot* s = realloc(slots, n * sizeof(slot));
        if (!s) cx_panic_capacity();
        memset(s + slot_count, 0, (n - slot_count) * sizeof(slot));
        slots = s, slot_count = n;
    }
    slots[i].r = r;
    cx_u64 h = (cx_u64)slots[i].gen << 32 | (i + 1);
    unlock();
    return h;
}

static request* table_find(cx_u64 h) {
    cx_u32 i = (cx_u32)h - 1, gen = (cx_u32)(h >> 32);
    lock();
    request* r = i < slot_count && slots[i].gen == gen ? slots[i].r : NULL;
    unlock();
    return r;
}

static void table_remove(cx_u64 h) {
    cx_u32 i = (cx_u32)h - 1, gen = (cx_u32)(h >> 32);
    lock();
    if (i < slot_count && slots[i].gen == gen && slots[i].r) {
        slots[i].r = NULL;
        slots[i].gen++;
    }
    unlock();
}

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

static char* dup(const char* s, size_t n) {
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
    r->host = dup(u + hs, he - hs);
    if (i == end || u[i] == '?') {
        r->target = malloc(end - i + 2);
        if (!r->target) cx_panic_capacity();
        r->target[0] = '/';
        memcpy(r->target + 1, u + i, end - i);
        r->target[end - i + 1] = 0;
    } else {
        r->target = dup(u + i, end - i);
    }
    r->url = dup(u, end);
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
} curl;
static int curl_loaded;
static cx_u32 curl_state;

static void curl_load(void) {
    const char* names[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
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
    return add(&r->data, p, k) ? k : 0;
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
    r->head = dup((const char*)head, head_len);
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
        return table_add(r);
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
    return table_add(r);
}

/* 1 if the request has finished, 0 if it's still running, and -1 if its response was taken. */
cx_i32 cx_http_done(cx_u64 h) {
    request* r = table_find(h);
    if (!r) return -1;
    return (cx_i32)__atomic_load_n(&r->state, __ATOMIC_ACQUIRE);
}

/* Waits for the request to finish, and gives what went wrong, 0 for nothing, or -1 if its
 * response was taken. */
cx_i32 cx_http_wait(cx_u64 h) {
    request* r = table_find(h);
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

/* The system's error code, for E_OS. */
cx_i32 cx_http_code(cx_u64 h) { return table_find(h)->code; }
cx_u32 cx_http_status(cx_u64 h) { return table_find(h)->status; }
cx_usize cx_http_head_len(cx_u64 h) { return table_find(h)->lines.len; }
cx_usize cx_http_body_len(cx_u64 h) { return table_find(h)->data.len; }

void cx_http_head(cx_u64 h, cx_u8* out) {
    request* r = table_find(h);
    if (r->lines.len) memcpy(out, r->lines.p, r->lines.len);
}

void cx_http_body(cx_u64 h, cx_u8* out) {
    request* r = table_find(h);
    if (r->data.len) memcpy(out, r->data.p, r->data.len);
}

/* Lets the request go, once it's been waited for. */
void cx_http_free(cx_u64 h) {
    request* r = table_find(h);
    table_remove(h);
    if (r) request_free(r);
}
