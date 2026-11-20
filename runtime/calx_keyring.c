/* The system's credential store, behind std.keyring (CLX-380): Windows' Credential Manager,
 * macOS's Keychain, and the Secret Service on Linux, through libsecret. Each keeps a secret under
 * a service and an account, as Python's keyring does, and where it can in the same place keyring
 * keeps it, so the two read each other's secrets.
 *
 * None of them is linked: Windows' advapi32 is looked up when the first call comes, Security and
 * CoreFoundation are opened with dlopen on macOS, and libsecret on Linux, where it's a package that
 * a headless server often hasn't got, and where there's often no Secret Service to talk to even
 * when it is installed. That's `K_UNAVAILABLE`, with why, which a daemon takes as the sign to keep
 * its secrets in a file of its own.
 *
 * Names and secrets come from Calx as bytes with a length, names as UTF-8. A call gives one of
 * std.keyring's error numbers, or 0, and leaves the secret it read, or the words for what went
 * wrong, in a per-thread buffer that `cx_keyring_result_copy` copies out and wipes. A secret is
 * wiped from every buffer of the runtime's before it's freed.
 *
 * Linked only into programs that call them. */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _GNU_SOURCE
#endif
#include "calx_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>
/* The most a generic credential holds, 5 * 512 bytes, as the Windows SDK has it. mingw-w64's
 * wincred.h, which the toolchain Calx ships builds with, still says 512, the limit before Windows
 * Vista, so that one isn't used. */
#define KEYRING_MAX_BLOB 2560
#include <wchar.h>
#else
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#ifndef __APPLE__
#include <sys/stat.h>
#endif
#endif

/* What went wrong, as std.keyring's Error numbers it. */
enum { K_OK, K_NOT_FOUND, K_UNAVAILABLE, K_DENIED, K_TOO_LONG, K_NOT_TEXT, K_BAD_NAME, K_OS };

/* The per-thread result: a secret, or the words for an error. */
static _Thread_local cx_u8* res_buf;
static _Thread_local size_t res_len, res_cap;
static _Thread_local cx_i32 res_code;

/* Zeroes `n` bytes at `p` in a way the compiler can't drop as a store nothing reads. */
static void wipe(void* p, size_t n) {
    if (!p || !n) return;
#ifdef _WIN32
    SecureZeroMemory(p, n);
#else
    volatile cx_u8* v = (volatile cx_u8*)p;
    while (n--) *v++ = 0;
#endif
}

static void res_clear(void) {
    wipe(res_buf, res_len);
    res_len = 0;
}

static int res_set(const void* p, size_t n) {
    res_clear();
    if (n > res_cap) {
        /* Not realloc, which could leave a copy of the old secret where nothing wipes it. */
        cx_u8* bigger = malloc(n);
        if (!bigger) return 0;
        wipe(res_buf, res_cap);
        free(res_buf);
        res_buf = bigger, res_cap = n;
    }
    if (n) memcpy(res_buf, p, n);
    res_len = n;
    return 1;
}

/* An error, with its words, and the system's code for Os. */
static int fail(int kind, cx_i32 code, const char* message) {
    res_code = code;
    res_set(message, strlen(message));
    return kind;
}

cx_usize cx_keyring_result_len(void) { return res_len; }

void cx_keyring_result_copy(cx_u8* out) {
    if (res_len) memcpy(out, res_buf, res_len);
    res_clear();
}

cx_i32 cx_keyring_error_code(void) { return res_code; }

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

#ifdef _WIN32

/* Credential Manager keeps generic credentials by a target name, as keyring does: the secret for
 * an account of a service is under `account@service`, its user name the account. keyring writes
 * the newest under the service's own name, so a secret is looked for there too, if its user name is
 * the account. Text is kept as UTF-16, as keyring and Credential Manager's own window keep it. */

static struct {
    BOOL(WINAPI* write)(PCREDENTIALW, DWORD);
    BOOL(WINAPI* read)(LPCWSTR, DWORD, DWORD, PCREDENTIALW*);
    BOOL(WINAPI* del)(LPCWSTR, DWORD, DWORD);
    VOID(WINAPI* free)(PVOID);
} cred;
static int cred_loaded;
static cx_u32 cred_state;

static void cred_load(void) {
    HMODULE m = GetModuleHandleW(L"advapi32.dll");
    if (!m) m = LoadLibraryW(L"advapi32.dll");
    if (!m) return;
    FARPROC fns[4] = {GetProcAddress(m, "CredWriteW"), GetProcAddress(m, "CredReadW"), GetProcAddress(m, "CredDeleteW"),
                      GetProcAddress(m, "CredFree")};
    for (int i = 0; i < 4; i++) {
        if (!fns[i]) return;
    }
    memcpy(&cred.write, &fns[0], sizeof(FARPROC));
    memcpy(&cred.read, &fns[1], sizeof(FARPROC));
    memcpy(&cred.del, &fns[2], sizeof(FARPROC));
    memcpy(&cred.free, &fns[3], sizeof(FARPROC));
    cred_loaded = 1;
}

/* `n` bytes of UTF-8 at `p` as UTF-16, ended by a 0, and how many units without it; NULL if it's
 * out of memory. */
static wchar_t* wide(const cx_u8* p, size_t n, int* units) {
    int u = n ? MultiByteToWideChar(CP_UTF8, 0, (const char*)p, (int)n, NULL, 0) : 0;
    wchar_t* w = malloc(((size_t)u + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    if (u) MultiByteToWideChar(CP_UTF8, 0, (const char*)p, (int)n, w, u);
    w[u] = 0;
    if (units) *units = u;
    return w;
}

/* The system's words for `code`, without the full stop and line end it ends them with. */
static int win_fail(int kind, DWORD code) {
    wchar_t* text = NULL;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                             0, (LPWSTR)&text, 0, NULL);
    while (n && (text[n - 1] == L'\r' || text[n - 1] == L'\n' || text[n - 1] == L' ' || text[n - 1] == L'.')) n--;
    char msg[512];
    int len = n ? WideCharToMultiByte(CP_UTF8, 0, text, (int)n, msg, (int)sizeof msg - 1, NULL, NULL) : 0;
    if (text) LocalFree(text);
    if (len <= 0) len = snprintf(msg, sizeof msg, "error %lu", (unsigned long)code);
    msg[len] = 0;
    return fail(kind, (cx_i32)code, msg);
}

static int win_error(DWORD code) {
    switch (code) {
    case ERROR_NO_SUCH_LOGON_SESSION:
        return fail(K_UNAVAILABLE, (cx_i32)code,
                    "this logon session has no Credential Manager, as a service's or a network logon's hasn't");
    case ERROR_ACCESS_DENIED: return win_fail(K_DENIED, code);
    case ERROR_INVALID_PARAMETER:
    case ERROR_BAD_USERNAME:
    case ERROR_INVALID_FLAGS: return win_fail(K_BAD_NAME, code);
    default: return win_fail(K_OS, code);
    }
}

/* The target names a secret may be under: `account@service`, and then the service's own name.
 * Both NULL if it's out of memory. */
typedef struct {
    wchar_t* compound;
    wchar_t* plain;
    wchar_t* user;
    int compound_units, user_units;
} names;

static int win_names(names* t, const cx_u8* service, size_t service_len, const cx_u8* account, size_t account_len) {
    memset(t, 0, sizeof *t);
    cx_u8* both = malloc(account_len + 1 + service_len);
    if (!both) return 0;
    if (account_len) memcpy(both, account, account_len);
    both[account_len] = '@';
    if (service_len) memcpy(both + account_len + 1, service, service_len);
    t->compound = wide(both, account_len + 1 + service_len, &t->compound_units);
    free(both);
    t->plain = wide(service, service_len, NULL);
    t->user = wide(account, account_len, &t->user_units);
    return t->compound && t->plain && t->user;
}

static void win_names_free(names* t) {
    free(t->compound);
    free(t->plain);
    free(t->user);
}

/* The credential under `target` if its user name is `user`, or NULL with why in the last error. */
static PCREDENTIALW win_read(const wchar_t* target, const wchar_t* user) {
    PCREDENTIALW c = NULL;
    if (!cred.read(target, CRED_TYPE_GENERIC, 0, &c)) return NULL;
    if (wcscmp(c->UserName ? c->UserName : L"", user) != 0) {
        cred.free(c);
        SetLastError(ERROR_NOT_FOUND);
        return NULL;
    }
    return c;
}

static int win_start(names* t, const cx_u8* service, size_t service_len, const cx_u8* account, size_t account_len) {
    once(&cred_state, cred_load);
    if (!cred_loaded) return fail(K_UNAVAILABLE, 0, "this Windows has no Credential Manager");
    if (!win_names(t, service, service_len, account, account_len)) {
        win_names_free(t);
        return fail(K_OS, ERROR_NOT_ENOUGH_MEMORY, "out of memory");
    }
    if (t->compound_units > CRED_MAX_GENERIC_TARGET_NAME_LENGTH) {
        win_names_free(t);
        return fail(K_BAD_NAME, 0, "the service and account are longer than Credential Manager takes, 32767 UTF-16 units together");
    }
    if (t->user_units > CRED_MAX_USERNAME_LENGTH) {
        win_names_free(t);
        return fail(K_BAD_NAME, 0, "the account is longer than Credential Manager takes, 513 UTF-16 units");
    }
    return K_OK;
}

cx_i32 cx_keyring_set(const cx_u8* service, cx_usize service_len, const cx_u8* account, cx_usize account_len, const cx_u8* secret,
                      cx_usize secret_len, cx_u32 text) {
    names t;
    int r = win_start(&t, service, service_len, account, account_len);
    if (r) return r;
    cx_u8* blob = (cx_u8*)secret;
    size_t blob_len = secret_len;
    wchar_t* w = NULL;
    int units = 0;
    if (text) {
        w = wide(secret, secret_len, &units);
        if (!w) {
            win_names_free(&t);
            return fail(K_OS, ERROR_NOT_ENOUGH_MEMORY, "out of memory");
        }
        blob = (cx_u8*)w, blob_len = (size_t)units * 2;
    }
    if (blob_len > KEYRING_MAX_BLOB) {
        r = fail(K_TOO_LONG, 0, "");
    } else {
        CREDENTIALW c;
        memset(&c, 0, sizeof c);
        c.Type = CRED_TYPE_GENERIC;
        c.TargetName = t.compound;
        c.UserName = t.user;
        c.CredentialBlobSize = (DWORD)blob_len;
        c.CredentialBlob = blob_len ? blob : NULL;
        /* For this user on this computer, not roamed to others in a domain. */
        c.Persist = CRED_PERSIST_LOCAL_MACHINE;
        r = cred.write(&c, 0) ? K_OK : win_error(GetLastError());
    }
    if (w) {
        wipe(w, (size_t)units * 2);
        free(w);
    }
    win_names_free(&t);
    return r;
}

cx_i32 cx_keyring_get(const cx_u8* service, cx_usize service_len, const cx_u8* account, cx_usize account_len, cx_u32 text) {
    names t;
    int r = win_start(&t, service, service_len, account, account_len);
    if (r) return r;
    PCREDENTIALW c = win_read(t.compound, t.user);
    if (!c && GetLastError() == ERROR_NOT_FOUND) c = win_read(t.plain, t.user);
    if (!c) {
        DWORD e = GetLastError();
        win_names_free(&t);
        return e == ERROR_NOT_FOUND ? fail(K_NOT_FOUND, 0, "") : win_error(e);
    }
    win_names_free(&t);
    DWORD n = c->CredentialBlobSize;
    if (!text) {
        r = res_set(c->CredentialBlob, n) ? K_OK : fail(K_OS, ERROR_NOT_ENOUGH_MEMORY, "out of memory");
    } else if (n % 2) {
        r = fail(K_NOT_TEXT, 0, "");
    } else {
        int units = (int)(n / 2);
        int len = units ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, (LPCWCH)c->CredentialBlob, units, NULL, 0, NULL, NULL) : 0;
        if (units && len <= 0) {
            r = fail(K_NOT_TEXT, 0, "");
        } else {
            char* utf8 = malloc((size_t)len + 1);
            if (!utf8) {
                r = fail(K_OS, ERROR_NOT_ENOUGH_MEMORY, "out of memory");
            } else {
                if (len) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, (LPCWCH)c->CredentialBlob, units, utf8, len, NULL, NULL);
                r = res_set(utf8, (size_t)len) ? K_OK : fail(K_OS, ERROR_NOT_ENOUGH_MEMORY, "out of memory");
                wipe(utf8, (size_t)len);
                free(utf8);
            }
        }
    }
    wipe(c->CredentialBlob, n);
    cred.free(c);
    return r;
}

/* Deletes the secret under the account's own target name and the one under the service's name if
 * it's the account's, as keyring does. 0 if there was neither, 1 if one was deleted. */
cx_i32 cx_keyring_delete(const cx_u8* service, cx_usize service_len, const cx_u8* account, cx_usize account_len) {
    names t;
    int r = win_start(&t, service, service_len, account, account_len);
    if (r) return r;
    int found = 0;
    const wchar_t* targets[2] = {t.compound, t.plain};
    for (int i = 0; i < 2 && !r; i++) {
        PCREDENTIALW c = win_read(targets[i], t.user);
        if (!c) {
            DWORD e = GetLastError();
            if (e != ERROR_NOT_FOUND) r = win_error(e);
            continue;
        }
        wipe(c->CredentialBlob, c->CredentialBlobSize);
        cred.free(c);
        if (cred.del(targets[i], CRED_TYPE_GENERIC, 0)) {
            found = 1;
        } else if (GetLastError() != ERROR_NOT_FOUND) {
            r = win_error(GetLastError());
        }
    }
    win_names_free(&t);
    if (r) return r;
    return found ? K_OK : fail(K_NOT_FOUND, 0, "");
}

#elif defined(__APPLE__)

/* The Keychain's generic passwords, by service and account, as keyring keeps them, in the login
 * keychain. Security and CoreFoundation are opened rather than linked, so a program that doesn't
 * use them doesn't load them, and their few types are written out here rather than taken from the
 * SDK's headers. A Keychain call can ask the person at the desktop to unlock the keychain, or to
 * let this program read a secret another program wrote, and waits for the answer. */

typedef const void* CFTypeRef;
typedef long CFIndex;
typedef cx_i32 OSStatus;
typedef cx_u32 CFStringEncoding;
enum { UTF8_ENCODING = 0x08000100 };
enum {
    errSecSuccess = 0, errSecUserCanceled = -128, errSecAuthFailed = -25293, errSecNoSuchKeychain = -25294,
    errSecNotAvailable = -25291, errSecReadOnly = -25292, errSecDuplicateItem = -25299, errSecItemNotFound = -25300,
    errSecNoDefaultKeychain = -25307, errSecInteractionNotAllowed = -25308, errSecInteractionRequired = -25315,
    errSecMissingEntitlement = -34018
};

static struct {
    CFTypeRef (*string_create)(CFTypeRef, const cx_u8*, CFIndex, CFStringEncoding, cx_u8);
    cx_u8 (*string_get)(CFTypeRef, char*, CFIndex, CFStringEncoding);
    CFTypeRef (*data_create)(CFTypeRef, const cx_u8*, CFIndex);
    CFIndex (*data_length)(CFTypeRef);
    const cx_u8* (*data_bytes)(CFTypeRef);
    void* (*dict_create)(CFTypeRef, CFIndex, const void*, const void*);
    void (*dict_set)(void*, CFTypeRef, CFTypeRef);
    void (*release)(CFTypeRef);
    OSStatus (*item_add)(CFTypeRef, CFTypeRef*);
    OSStatus (*item_copy)(CFTypeRef, CFTypeRef*);
    OSStatus (*item_update)(CFTypeRef, CFTypeRef);
    OSStatus (*item_delete)(CFTypeRef);
    CFTypeRef (*error_text)(OSStatus, void*);
    const void* key_callbacks;
    const void* value_callbacks;
    CFTypeRef yes, cls, generic, service, account, data, return_data, limit, limit_one;
} sec;
static int sec_loaded;
static cx_u32 sec_state;

static void sec_load(void) {
    void* cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW | RTLD_LOCAL);
    void* s = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_NOW | RTLD_LOCAL);
    if (!cf || !s) return;
    void** cf_fns[] = {(void**)&sec.string_create, (void**)&sec.string_get, (void**)&sec.data_create, (void**)&sec.data_length,
                       (void**)&sec.data_bytes, (void**)&sec.dict_create, (void**)&sec.dict_set, (void**)&sec.release};
    const char* cf_syms[] = {"CFStringCreateWithBytes", "CFStringGetCString", "CFDataCreate", "CFDataGetLength",
                             "CFDataGetBytePtr", "CFDictionaryCreateMutable", "CFDictionarySetValue", "CFRelease"};
    for (int i = 0; i < 8; i++) {
        *cf_fns[i] = dlsym(cf, cf_syms[i]);
        if (!*cf_fns[i]) return;
    }
    void** sec_fns[] = {(void**)&sec.item_add, (void**)&sec.item_copy, (void**)&sec.item_update, (void**)&sec.item_delete,
                        (void**)&sec.error_text};
    const char* sec_syms[] = {"SecItemAdd", "SecItemCopyMatching", "SecItemUpdate", "SecItemDelete", "SecCopyErrorMessageString"};
    for (int i = 0; i < 5; i++) {
        *sec_fns[i] = dlsym(s, sec_syms[i]);
        if (!*sec_fns[i]) return;
    }
    sec.key_callbacks = dlsym(cf, "kCFTypeDictionaryKeyCallBacks");
    sec.value_callbacks = dlsym(cf, "kCFTypeDictionaryValueCallBacks");
    if (!sec.key_callbacks || !sec.value_callbacks) return;
    /* The constants are variables that hold each CFStringRef, and dlsym gives where each is. */
    CFTypeRef* consts[] = {&sec.yes, &sec.cls, &sec.generic, &sec.service, &sec.account, &sec.data, &sec.return_data, &sec.limit,
                           &sec.limit_one};
    void* libs[] = {cf, s, s, s, s, s, s, s, s};
    const char* const_syms[] = {"kCFBooleanTrue", "kSecClass", "kSecClassGenericPassword", "kSecAttrService", "kSecAttrAccount",
                                "kSecValueData", "kSecReturnData", "kSecMatchLimit", "kSecMatchLimitOne"};
    for (int i = 0; i < 9; i++) {
        CFTypeRef* at = dlsym(libs[i], const_syms[i]);
        if (!at || !*at) return;
        *consts[i] = *at;
    }
    sec_loaded = 1;
}

static int mac_fail(int kind, OSStatus code) {
    char msg[512];
    msg[0] = 0;
    CFTypeRef text = sec.error_text(code, NULL);
    if (text) {
        if (!sec.string_get(text, msg, sizeof msg, UTF8_ENCODING)) msg[0] = 0;
        sec.release(text);
    }
    if (!msg[0]) snprintf(msg, sizeof msg, "error %d", (int)code);
    return fail(kind, code, msg);
}

static int mac_error(OSStatus code) {
    switch (code) {
    case errSecNotAvailable:
    case errSecNoSuchKeychain:
    case errSecNoDefaultKeychain:
    case errSecMissingEntitlement: return mac_fail(K_UNAVAILABLE, code);
    case errSecUserCanceled:
    case errSecAuthFailed:
    case errSecInteractionNotAllowed:
    case errSecInteractionRequired:
    case errSecReadOnly: return mac_fail(K_DENIED, code);
    default: return mac_fail(K_OS, code);
    }
}

/* A query for the generic password of `account` of `service`, or NULL if it's out of memory. */
static void* mac_query(const cx_u8* service, size_t service_len, const cx_u8* account, size_t account_len) {
    void* q = sec.dict_create(NULL, 0, sec.key_callbacks, sec.value_callbacks);
    if (!q) return NULL;
    CFTypeRef s = sec.string_create(NULL, service, (CFIndex)service_len, UTF8_ENCODING, 0);
    CFTypeRef a = sec.string_create(NULL, account, (CFIndex)account_len, UTF8_ENCODING, 0);
    if (!s || !a) {
        if (s) sec.release(s);
        if (a) sec.release(a);
        sec.release(q);
        return NULL;
    }
    sec.dict_set(q, sec.cls, sec.generic);
    sec.dict_set(q, sec.service, s);
    sec.dict_set(q, sec.account, a);
    sec.release(s);
    sec.release(a);
    return q;
}

static int mac_start(void** q, const cx_u8* service, size_t service_len, const cx_u8* account, size_t account_len) {
    once(&sec_state, sec_load);
    if (!sec_loaded) return fail(K_UNAVAILABLE, 0, "can't open the Security framework");
    *q = mac_query(service, service_len, account, account_len);
    return *q ? K_OK : fail(K_OS, 0, "out of memory");
}

cx_i32 cx_keyring_set(const cx_u8* service, cx_usize service_len, const cx_u8* account, cx_usize account_len, const cx_u8* secret,
                      cx_usize secret_len, cx_u32 text) {
    (void)text;
    void* q;
    int r = mac_start(&q, service, service_len, account, account_len);
    if (r) return r;
    CFTypeRef data = sec.data_create(NULL, secret, (CFIndex)secret_len);
    void* change = sec.dict_create(NULL, 0, sec.key_callbacks, sec.value_callbacks);
    if (!data || !change) {
        r = fail(K_OS, 0, "out of memory");
    } else {
        /* Changing the secret keeps who may read it; adding one lets this program. An update to
         * empty data leaves the old secret in place, so an empty one replaces the item instead. */
        sec.dict_set(change, sec.data, data);
        OSStatus s;
        if (secret_len == 0) {
            s = sec.item_delete(q);
            if (s == errSecSuccess) s = errSecItemNotFound;
        } else {
            s = sec.item_update(q, change);
        }
        if (s == errSecItemNotFound) {
            sec.dict_set(q, sec.data, data);
            s = sec.item_add(q, NULL);
        }
        r = s == errSecSuccess ? K_OK : mac_error(s);
    }
    if (data) sec.release(data);
    if (change) sec.release(change);
    sec.release(q);
    return r;
}

cx_i32 cx_keyring_get(const cx_u8* service, cx_usize service_len, const cx_u8* account, cx_usize account_len, cx_u32 text) {
    (void)text;
    void* q;
    int r = mac_start(&q, service, service_len, account, account_len);
    if (r) return r;
    sec.dict_set(q, sec.return_data, sec.yes);
    sec.dict_set(q, sec.limit, sec.limit_one);
    CFTypeRef data = NULL;
    OSStatus s = sec.item_copy(q, &data);
    sec.release(q);
    if (s == errSecItemNotFound) return fail(K_NOT_FOUND, 0, "");
    if (s != errSecSuccess) return mac_error(s);
    /* An item with an empty secret may come back with no data. */
    if (!data) return res_set(NULL, 0) ? K_OK : fail(K_OS, 0, "out of memory");
    r = res_set(sec.data_bytes(data), (size_t)sec.data_length(data)) ? K_OK : fail(K_OS, 0, "out of memory");
    /* CFData's bytes can't be wiped through its API without casting away const; they're
     * CoreFoundation's to free. */
    sec.release(data);
    return r;
}

cx_i32 cx_keyring_delete(const cx_u8* service, cx_usize service_len, const cx_u8* account, cx_usize account_len) {
    void* q;
    int r = mac_start(&q, service, service_len, account, account_len);
    if (r) return r;
    OSStatus s = sec.item_delete(q);
    sec.release(q);
    if (s == errSecItemNotFound) return fail(K_NOT_FOUND, 0, "");
    return s == errSecSuccess ? K_OK : mac_error(s);
}

#else

/* The Secret Service, through libsecret, which talks to whatever provides it on the session bus:
 * GNOME Keyring, KWallet or KeePassXC. A secret is an item with the attributes `service` and
 * `username`, as Python keyring's backends give it, in the default collection. Looked up by those
 * two alone, so the items keyring wrote are found too.
 *
 * libsecret's calls wait on D-Bus, and storing in a locked collection asks the person to unlock it.
 * With no display there's nobody to ask, so each call is cancelled after 10 s if no display is set
 * and 2 minutes if one is: a server finds out there's no store rather than waiting forever. */

typedef struct {
    cx_u32 domain;
    int code;
    char* message;
} GError;
typedef struct {
    const char* name;
    int type;
} SchemaAttribute;
typedef struct {
    const char* name;
    int flags;
    SchemaAttribute attributes[32];
    int reserved;
    void* reserved1;
    void* reserved2;
    void* reserved3;
    void* reserved4;
    void* reserved5;
    void* reserved6;
    void* reserved7;
} SecretSchema;

/* Matched by the attributes alone, not the schema's name (SECRET_SCHEMA_DONT_MATCH_NAME). */
static const SecretSchema schema = {"org.freedesktop.Secret.Generic", 1 << 1, {{"service", 0}, {"username", 0}, {NULL, 0}}, 0,
                                    NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static struct {
    int (*store)(const SecretSchema*, const char*, const char*, void*, void*, GError**, ...);
    void* (*lookup)(const SecretSchema*, void*, GError**, ...);
    int (*clear)(const SecretSchema*, void*, GError**, ...);
    void* (*value_new)(const char*, long, const char*);
    const char* (*value_get)(void*, size_t*);
    void (*value_unref)(void*);
    cx_u32 (*secret_quark)(void);
    cx_u32 (*dbus_quark)(void);
    cx_u32 (*io_quark)(void);
    void* (*cancellable_new)(void);
    void (*cancel)(void*);
    void (*unref)(void*);
    void (*error_free)(GError*);
} ss;
static int ss_loaded;
static cx_u32 ss_state;

static void ss_load(void) {
    const char* names[] = {"libsecret-1.so.0", "libsecret-1.so"};
    void* lib = NULL;
    for (int i = 0; i < 2 && !lib; i++) lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
    /* libsecret brings GLib and GIO with it, and dlsym looks in what a library brought. The
     * binary calls are from libsecret 0.19, of 2019. */
    void** fns[] = {(void**)&ss.store,        (void**)&ss.lookup,          (void**)&ss.clear,      (void**)&ss.value_new,
                    (void**)&ss.value_get,    (void**)&ss.value_unref,     (void**)&ss.secret_quark, (void**)&ss.dbus_quark,
                    (void**)&ss.io_quark,     (void**)&ss.cancellable_new, (void**)&ss.cancel,     (void**)&ss.unref,
                    (void**)&ss.error_free};
    const char* syms[] = {"secret_password_store_binary_sync", "secret_password_lookup_binary_sync", "secret_password_clear_sync",
                          "secret_value_new",                  "secret_value_get",                   "secret_value_unref",
                          "secret_error_get_quark",            "g_dbus_error_quark",                 "g_io_error_quark",
                          "g_cancellable_new",                 "g_cancellable_cancel",               "g_object_unref",
                          "g_error_free"};
    for (int i = 0; i < 13; i++) {
        *fns[i] = dlsym(lib, syms[i]);
        if (!*fns[i]) return;
    }
    ss_loaded = 1;
}

static int has_text(const char* name) {
    const char* v = getenv(name);
    return v && *v;
}

/* Whether there's a session bus to look for the Secret Service on: one the environment names, or
 * the one systemd starts for each user, which GLib finds by itself. Without either GLib would try to
 * start one through X11, which a server hasn't got. */
static int has_bus(void) {
    if (has_text("DBUS_SESSION_BUS_ADDRESS")) return 1;
    const char* dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) return 0;
    char path[4096];
    if ((size_t)snprintf(path, sizeof path, "%s/bus", dir) >= sizeof path) return 0;
    struct stat st;
    return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

/* Cancels a call that takes longer than `ms`. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int done, fired, started;
    unsigned ms;
    void* cancellable;
    pthread_t thread;
} watch;

static void* watch_run(void* arg) {
    watch* w = arg;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += w->ms / 1000;
    until.tv_nsec += (long)(w->ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
    pthread_mutex_lock(&w->lock);
    while (!w->done) {
        if (pthread_cond_timedwait(&w->wake, &w->lock, &until) == ETIMEDOUT) {
            if (!w->done) {
                w->fired = 1;
                ss.cancel(w->cancellable);
            }
            break;
        }
    }
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

static void watch_start(watch* w) {
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->wake, NULL);
    w->done = w->fired = 0;
    w->ms = has_text("DISPLAY") || has_text("WAYLAND_DISPLAY") ? 120000 : 10000;
    w->cancellable = ss.cancellable_new();
    w->started = w->cancellable && pthread_create(&w->thread, NULL, watch_run, w) == 0;
}

static void watch_stop(watch* w) {
    if (w->started) {
        pthread_mutex_lock(&w->lock);
        w->done = 1;
        pthread_cond_signal(&w->wake);
        pthread_mutex_unlock(&w->lock);
        pthread_join(w->thread, NULL);
    }
    if (w->cancellable) ss.unref(w->cancellable);
    pthread_cond_destroy(&w->wake);
    pthread_mutex_destroy(&w->lock);
}

/* What a GError means, which it frees. */
static int ss_error(GError* e, const watch* w) {
    int kind = K_OS;
    char msg[512];
    snprintf(msg, sizeof msg, "%s", e->message ? e->message : "the Secret Service failed");
    if (w->fired) {
        kind = K_UNAVAILABLE;
        snprintf(msg, sizeof msg, "the Secret Service didn't answer within %u s", w->ms / 1000);
    } else if (e->domain == ss.dbus_quark()) {
        switch (e->code) {
        case 2:   /* SERVICE_UNKNOWN */
        case 3:   /* NAME_HAS_NO_OWNER */
            kind = K_UNAVAILABLE;
            snprintf(msg, sizeof msg, "nothing on the session bus provides the Secret Service, such as GNOME Keyring or KeePassXC");
            break;
        case 4:   /* NO_REPLY */
        case 5:   /* IO_ERROR */
        case 6:   /* BAD_ADDRESS */
        case 11:  /* NO_SERVER */
        case 12:  /* TIMEOUT */
        case 15:  /* DISCONNECTED */
        case 20:  /* TIMED_OUT */
            kind = K_UNAVAILABLE;
            break;
        case 9:   /* ACCESS_DENIED */
        case 10:  /* AUTH_FAILED */
            kind = K_DENIED;
            break;
        default: break;
        }
    } else if (e->domain == ss.io_quark()) {
        /* Connecting to the bus failed. */
        kind = K_UNAVAILABLE;
    } else if (e->domain == ss.secret_quark() && e->code == 2) {
        /* IS_LOCKED: the collection is locked, and it wasn't unlocked when asked. */
        kind = K_DENIED;
    }
    cx_i32 code = e->code;
    ss.error_free(e);
    return fail(kind, code, msg);
}

/* NUL-terminated copies of the names, for libsecret. */
static int ss_start(char** s, char** a, const cx_u8* service, size_t service_len, const cx_u8* account, size_t account_len) {
    *s = *a = NULL;
    once(&ss_state, ss_load);
    if (!ss_loaded) return fail(K_UNAVAILABLE, 0, "libsecret isn't installed, which the Secret Service is reached through");
    if (!has_bus()) return fail(K_UNAVAILABLE, 0, "there's no D-Bus session bus to find the Secret Service on");
    *s = malloc(service_len + 1);
    *a = malloc(account_len + 1);
    if (!*s || !*a) {
        free(*s);
        free(*a);
        return fail(K_OS, ENOMEM, "out of memory");
    }
    memcpy(*s, service, service_len);
    (*s)[service_len] = 0;
    memcpy(*a, account, account_len);
    (*a)[account_len] = 0;
    return K_OK;
}

cx_i32 cx_keyring_set(const cx_u8* service, cx_usize service_len, const cx_u8* account, cx_usize account_len, const cx_u8* secret,
                      cx_usize secret_len, cx_u32 text) {
    char *s, *a;
    int r = ss_start(&s, &a, service, service_len, account, account_len);
    if (r) return r;
    /* The label people see in Seahorse, as keyring words it. */
    size_t label_cap = service_len + account_len + 32;
    char* label = malloc(label_cap);
    void* value = ss.value_new((const char*)secret, (long)secret_len, text ? "text/plain" : "application/octet-stream");
    if (!label || !value) {
        r = fail(K_OS, ENOMEM, "out of memory");
    } else {
        snprintf(label, label_cap, "Password for '%s' on '%s'", a, s);
        watch w;
        watch_start(&w);
        GError* e = NULL;
        int ok = ss.store(&schema, NULL, label, value, w.cancellable, &e, "service", s, "username", a, NULL);
        watch_stop(&w);
        r = ok ? K_OK : e ? ss_error(e, &w) : fail(K_OS, 0, "the Secret Service failed");
    }
    if (value) ss.value_unref(value);
    free(label);
    free(s);
    free(a);
    return r;
}

cx_i32 cx_keyring_get(const cx_u8* service, cx_usize service_len, const cx_u8* account, cx_usize account_len, cx_u32 text) {
    (void)text;
    char *s, *a;
    int r = ss_start(&s, &a, service, service_len, account, account_len);
    if (r) return r;
    watch w;
    watch_start(&w);
    GError* e = NULL;
    void* value = ss.lookup(&schema, w.cancellable, &e, "service", s, "username", a, NULL);
    watch_stop(&w);
    free(s);
    free(a);
    if (e) {
        if (value) ss.value_unref(value);
        return ss_error(e, &w);
    }
    if (!value) return fail(K_NOT_FOUND, 0, "");
    size_t n = 0;
    const char* p = ss.value_get(value, &n);
    r = res_set(p, n) ? K_OK : fail(K_OS, ENOMEM, "out of memory");
    /* libsecret keeps a secret in memory it wipes when it frees it. */
    ss.value_unref(value);
    return r;
}

cx_i32 cx_keyring_delete(const cx_u8* service, cx_usize service_len, const cx_u8* account, cx_usize account_len) {
    char *s, *a;
    int r = ss_start(&s, &a, service, service_len, account, account_len);
    if (r) return r;
    watch w;
    watch_start(&w);
    GError* e = NULL;
    int removed = ss.clear(&schema, w.cancellable, &e, "service", s, "username", a, NULL);
    watch_stop(&w);
    free(s);
    free(a);
    if (e) return ss_error(e, &w);
    return removed ? K_OK : fail(K_NOT_FOUND, 0, "");
}

#endif
