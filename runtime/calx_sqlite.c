/* SQLite behind std.sqlite (CLX-108). SQLite itself is runtime/sqlite/sqlite3.c, its amalgamation
 * unchanged, which the compiler builds once on its own (toolchain.cpp). This file is the thin layer
 * std.sqlite calls, linked only into programs that use it.
 *
 * A connection is a `conn`, which std.sqlite holds as a number. It keeps the statements it has
 * prepared, up to CACHE of them, keyed by their SQL, as Python's sqlite3 keeps its own, so running
 * the same SQL again skips SQLite's parser and planner. A statement handed out is busy until it's
 * given back, reset and with its values cleared; the same SQL asked for while it's busy gets a
 * statement of its own, which is finalized when it's given back. Closing finalizes the idle
 * statements and closes SQLite's connection with sqlite3_close_v2, which keeps it for the
 * statements still out; the conn is freed when the last of them comes back.
 *
 * A call that fails keeps SQLite's extended result code and a copy of its message in the conn, for
 * std.sqlite to copy out. Pointers std.sqlite gives for empty text or bytes may be null, which
 * SQLite would read as NULL, so those are bound as empty text and a zero-length blob. */
#include "calx_rt.h"
#include "sqlite/sqlite3.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CACHE 32

typedef struct {
    sqlite3_stmt* stmt;
    char* sql; /* its SQL, the key, not NUL-terminated */
    size_t len;
    cx_u64 hash;
    cx_u64 used; /* when it was last handed out, to evict the least recently used */
    int busy;
} entry;

typedef struct {
    sqlite3* db;
    int closed;
    int out;       /* statements handed out and not given back */
    int code;      /* the last error's extended result code, 0 if the last call didn't fail */
    char* message; /* and its message */
    cx_u64 clock;
    entry cache[CACHE];
} conn;

#define CONN(h) ((conn*)(intptr_t)(h))
#define STMT(h) ((sqlite3_stmt*)(intptr_t)(h))

static void fail_with(conn* c, int code, const char* message) {
    free(c->message);
    size_t n = strlen(message);
    c->message = (char*)malloc(n + 1);
    if (c->message) memcpy(c->message, message, n + 1);
    c->code = code ? code : SQLITE_ERROR;
}

static void fail_db(conn* c, int rc) {
    if (c->db) fail_with(c, sqlite3_extended_errcode(c->db), sqlite3_errmsg(c->db));
    else fail_with(c, rc, sqlite3_errstr(rc));
}

static void free_conn(conn* c) {
    for (int i = 0; i < CACHE; i++) free(c->cache[i].sql);
    free(c->message);
    free(c);
}

/* A NUL-terminated copy of `n` bytes, or null if there's no memory or they hold a NUL, which
 * would cut the text short where SQLite reads it. */
static char* terminated(conn* c, const cx_u8* p, size_t n, const char* what) {
    if (n && memchr(p, 0, n)) {
        fail_with(c, SQLITE_MISUSE, what);
        return NULL;
    }
    char* z = (char*)malloc(n + 1);
    if (!z) {
        fail_with(c, SQLITE_NOMEM, "out of memory");
        return NULL;
    }
    if (n) memcpy(z, p, n);
    z[n] = 0;
    return z;
}

/* Opens the database at `path`, made if it isn't there unless `read_only`, as Python's
 * sqlite3.connect opens it, waiting up to 5 seconds for another connection's lock as its default
 * timeout does. A failed open gives a closed conn holding the error, which cx_sqlite_close frees;
 * 0 only if there's no memory for one. */
cx_i64 cx_sqlite_open(const cx_u8* path, cx_usize n, cx_i32 read_only) {
    conn* c = (conn*)calloc(1, sizeof(conn));
    if (!c) return 0;
    char* z = terminated(c, path, n, "the path has a NUL byte in it");
    if (!z) {
        c->closed = 1;
        return (cx_i64)(intptr_t)c;
    }
    int flags = read_only ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    int rc = sqlite3_open_v2(z, &c->db, flags, NULL);
    free(z);
    if (rc != SQLITE_OK) {
        fail_db(c, rc);
        sqlite3_close_v2(c->db);
        c->db = NULL;
        c->closed = 1;
        return (cx_i64)(intptr_t)c;
    }
    sqlite3_extended_result_codes(c->db, 1);
    sqlite3_busy_timeout(c->db, 5000);
    return (cx_i64)(intptr_t)c;
}

/* Finalizes the idle statements and closes the connection, rolling back a transaction left open.
 * The conn itself goes once no statement is out. */
cx_i32 cx_sqlite_close(cx_i64 h) {
    conn* c = CONN(h);
    int rc = SQLITE_OK;
    if (!c->closed) {
        for (int i = 0; i < CACHE; i++) {
            entry* e = &c->cache[i];
            if (e->stmt && !e->busy) {
                sqlite3_finalize(e->stmt);
                free(e->sql);
                memset(e, 0, sizeof *e);
            }
        }
        rc = sqlite3_close_v2(c->db);
        c->closed = 1;
    }
    if (c->out == 0) free_conn(c);
    return rc;
}

cx_i32 cx_sqlite_error_code(cx_i64 h) { return CONN(h)->code; }

cx_usize cx_sqlite_error_len(cx_i64 h) {
    conn* c = CONN(h);
    return c->message ? strlen(c->message) : 0;
}

void cx_sqlite_error_copy(cx_i64 h, cx_u8* out) {
    conn* c = CONN(h);
    if (c->message) memcpy(out, c->message, strlen(c->message));
}

static int closed(conn* c) {
    if (!c->closed) return 0;
    fail_with(c, SQLITE_MISUSE, "the database is closed");
    return 1;
}

static cx_u64 fnv(const cx_u8* p, size_t n) {
    cx_u64 h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* Whether only white space and comments are between `s` and `end`, as SQLite reads them. */
static int only_space(const char* s, const char* end) {
    while (s < end) {
        char ch = *s;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') {
            s++;
        } else if (ch == '-' && s + 1 < end && s[1] == '-') {
            while (s < end && *s != '\n') s++;
        } else if (ch == '/' && s + 1 < end && s[1] == '*') {
            s += 2;
            while (s + 1 < end && !(s[0] == '*' && s[1] == '/')) s++;
            s = s + 1 < end ? s + 2 : end; /* a comment that isn't closed runs to the end */
        } else {
            return 0;
        }
    }
    return 1;
}

/* The statement `sql` holds, from the cache if it ran before and isn't busy. 0 with no error if
 * it holds only white space and comments, and 0 with one if it doesn't prepare or holds more than
 * one statement, as Python's execute refuses. */
cx_i64 cx_sqlite_prepare(cx_i64 h, const cx_u8* sql, cx_usize n) {
    conn* c = CONN(h);
    c->code = 0;
    if (closed(c)) return 0;
    if (n > 1000000000) {
        fail_with(c, SQLITE_TOOBIG, "the SQL is too long");
        return 0;
    }
    cx_u64 hash = fnv(sql, n);
    c->clock++;
    entry* spare = NULL; /* an empty slot, or else the idle one used longest ago */
    for (int i = 0; i < CACHE; i++) {
        entry* e = &c->cache[i];
        if (e->busy) continue;
        if (e->stmt && e->hash == hash && e->len == n && memcmp(e->sql, sql, n) == 0) {
            e->busy = 1;
            e->used = c->clock;
            c->out++;
            return (cx_i64)(intptr_t)e->stmt;
        }
        if (!spare || (spare->stmt && (!e->stmt || e->used < spare->used))) spare = e;
    }
    sqlite3_stmt* st = NULL;
    const char* tail = NULL;
    int rc = sqlite3_prepare_v3(c->db, (const char*)sql, (int)n, SQLITE_PREPARE_PERSISTENT, &st, &tail);
    if (rc != SQLITE_OK) {
        fail_db(c, rc);
        return 0;
    }
    if (!st) return 0;
    if (tail && !only_space(tail, (const char*)sql + n)) {
        sqlite3_finalize(st);
        fail_with(c, SQLITE_MISUSE, "the SQL holds more than one statement; execute_script runs several");
        return 0;
    }
    c->out++;
    if (spare) {
        char* key = (char*)malloc(n ? n : 1);
        if (key) {
            if (spare->stmt) {
                sqlite3_finalize(spare->stmt);
                free(spare->sql);
            }
            if (n) memcpy(key, sql, n);
            spare->stmt = st;
            spare->sql = key;
            spare->len = n;
            spare->hash = hash;
            spare->used = c->clock;
            spare->busy = 1;
        }
    }
    return (cx_i64)(intptr_t)st;
}

/* Takes back a statement cx_sqlite_prepare gave: reset, its values cleared and idle in the cache,
 * or finalized if it isn't in it or the connection is closed. */
void cx_sqlite_release(cx_i64 h, cx_i64 s) {
    conn* c = CONN(h);
    sqlite3_stmt* st = STMT(s);
    c->out--;
    int cached = 0;
    for (int i = 0; i < CACHE; i++) {
        entry* e = &c->cache[i];
        if (e->stmt != st) continue;
        cached = 1;
        if (c->closed) {
            sqlite3_finalize(st);
            free(e->sql);
            memset(e, 0, sizeof *e);
        } else {
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            e->busy = 0;
        }
        break;
    }
    if (!cached) sqlite3_finalize(st);
    if (c->closed && c->out == 0) free_conn(c);
}

static cx_i32 bound(conn* c, int rc) {
    if (rc == SQLITE_OK) return 0;
    fail_db(c, rc);
    return rc;
}

cx_i32 cx_sqlite_bind_null(cx_i64 h, cx_i64 s, cx_i32 i) { return bound(CONN(h), sqlite3_bind_null(STMT(s), i)); }

cx_i32 cx_sqlite_bind_int(cx_i64 h, cx_i64 s, cx_i32 i, cx_i64 v) { return bound(CONN(h), sqlite3_bind_int64(STMT(s), i, v)); }

cx_i32 cx_sqlite_bind_real(cx_i64 h, cx_i64 s, cx_i32 i, cx_f64 v) { return bound(CONN(h), sqlite3_bind_double(STMT(s), i, v)); }

cx_i32 cx_sqlite_bind_text(cx_i64 h, cx_i64 s, cx_i32 i, const cx_u8* p, cx_usize n) {
    return bound(CONN(h), sqlite3_bind_text64(STMT(s), i, n ? (const char*)p : "", n, SQLITE_TRANSIENT, SQLITE_UTF8));
}

cx_i32 cx_sqlite_bind_blob(cx_i64 h, cx_i64 s, cx_i32 i, const cx_u8* p, cx_usize n) {
    if (n == 0) return bound(CONN(h), sqlite3_bind_zeroblob(STMT(s), i, 0));
    return bound(CONN(h), sqlite3_bind_blob64(STMT(s), i, p, n, SQLITE_TRANSIENT));
}

cx_i32 cx_sqlite_parameter_count(cx_i64 s) { return sqlite3_bind_parameter_count(STMT(s)); }

/* The index of the parameter written `name` in the SQL, such as `:id`, or 0 if there's none. */
cx_i32 cx_sqlite_parameter_index(cx_i64 s, const cx_u8* name, cx_usize n) {
    char small[128];
    char* z = n < sizeof small ? small : (char*)malloc(n + 1);
    if (!z || (n && memchr(name, 0, n))) {
        if (z != small) free(z);
        return 0;
    }
    if (n) memcpy(z, name, n);
    z[n] = 0;
    int i = sqlite3_bind_parameter_index(STMT(s), z);
    if (z != small) free(z);
    return i;
}

/* Runs the statement to its next row: 1 if there's one, 0 when it's done, -1 if it failed. Done,
 * it's reset, keeping its values, so it can be bound again, which SQLite refuses until it is. */
cx_i32 cx_sqlite_step(cx_i64 h, cx_i64 s) {
    conn* c = CONN(h);
    if (closed(c)) return -1;
    int rc = sqlite3_step(STMT(s));
    if (rc == SQLITE_ROW) return 1;
    if (rc == SQLITE_DONE) {
        sqlite3_reset(STMT(s));
        return 0;
    }
    fail_db(c, rc);
    sqlite3_reset(STMT(s));
    return -1;
}

/* Runs the statement to its end, passing over any rows, then resets it, keeping its values. Gives
 * the rows an INSERT, UPDATE or DELETE changed, not counting triggers', 0 for any other statement,
 * or -1 if it failed. */
cx_i64 cx_sqlite_run(cx_i64 h, cx_i64 s) {
    conn* c = CONN(h);
    if (closed(c)) return -1;
    sqlite3_stmt* st = STMT(s);
    sqlite3_int64 before = sqlite3_total_changes64(c->db);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    }
    if (rc != SQLITE_DONE) {
        fail_db(c, rc);
        sqlite3_reset(st);
        return -1;
    }
    /* sqlite3_changes keeps the last INSERT, UPDATE or DELETE's count through other statements. */
    sqlite3_int64 n = sqlite3_total_changes64(c->db) == before ? 0 : sqlite3_changes64(c->db);
    sqlite3_reset(st);
    return n;
}

void cx_sqlite_reset(cx_i64 s) { sqlite3_reset(STMT(s)); }

void cx_sqlite_clear(cx_i64 s) { sqlite3_clear_bindings(STMT(s)); }

cx_i32 cx_sqlite_column_count(cx_i64 s) { return sqlite3_column_count(STMT(s)); }

/* SQLite's type for the column in the current row: 1 integer, 2 float, 3 text, 4 blob, 5 null. */
cx_i32 cx_sqlite_column_type(cx_i64 s, cx_i32 col) { return sqlite3_column_type(STMT(s), col); }

cx_i64 cx_sqlite_column_int(cx_i64 s, cx_i32 col) { return sqlite3_column_int64(STMT(s), col); }

cx_f64 cx_sqlite_column_real(cx_i64 s, cx_i32 col) { return sqlite3_column_double(STMT(s), col); }

/* The column's value as UTF-8 text, converted as SQLite converts it: its length, then a copy. */
cx_usize cx_sqlite_column_text_len(cx_i64 s, cx_i32 col) {
    sqlite3_column_text(STMT(s), col);
    return (cx_usize)sqlite3_column_bytes(STMT(s), col);
}

void cx_sqlite_column_text_copy(cx_i64 s, cx_i32 col, cx_u8* out) {
    const unsigned char* p = sqlite3_column_text(STMT(s), col);
    int n = sqlite3_column_bytes(STMT(s), col);
    if (p && n > 0) memcpy(out, p, (size_t)n);
}

/* The column's value as bytes: its length, then a copy. */
cx_usize cx_sqlite_column_blob_len(cx_i64 s, cx_i32 col) {
    sqlite3_column_blob(STMT(s), col);
    return (cx_usize)sqlite3_column_bytes(STMT(s), col);
}

void cx_sqlite_column_blob_copy(cx_i64 s, cx_i32 col, cx_u8* out) {
    const void* p = sqlite3_column_blob(STMT(s), col);
    int n = sqlite3_column_bytes(STMT(s), col);
    if (p && n > 0) memcpy(out, p, (size_t)n);
}

/* The column's name, as the query names it: its length, then a copy. */
cx_usize cx_sqlite_column_name_len(cx_i64 s, cx_i32 col) {
    const char* p = sqlite3_column_name(STMT(s), col);
    return p ? strlen(p) : 0;
}

void cx_sqlite_column_name_copy(cx_i64 s, cx_i32 col, cx_u8* out) {
    const char* p = sqlite3_column_name(STMT(s), col);
    if (p) memcpy(out, p, strlen(p));
}

/* Runs every statement in `sql` in turn, as Python's executescript does. */
cx_i32 cx_sqlite_exec(cx_i64 h, const cx_u8* sql, cx_usize n) {
    conn* c = CONN(h);
    c->code = 0;
    if (closed(c)) return SQLITE_MISUSE;
    char* z = terminated(c, sql, n, "the SQL has a NUL byte in it");
    if (!z) return c->code;
    char* message = NULL;
    int rc = sqlite3_exec(c->db, z, NULL, NULL, &message);
    free(z);
    if (rc != SQLITE_OK) {
        fail_with(c, sqlite3_extended_errcode(c->db), message ? message : sqlite3_errstr(rc));
        sqlite3_free(message);
        return rc;
    }
    return 0;
}

cx_i64 cx_sqlite_last_insert_id(cx_i64 h) { return sqlite3_last_insert_rowid(CONN(h)->db); }

cx_i32 cx_sqlite_autocommit(cx_i64 h) { return sqlite3_get_autocommit(CONN(h)->db); }

cx_usize cx_sqlite_version_len(void) { return strlen(sqlite3_libversion()); }

void cx_sqlite_version_copy(cx_u8* out) { memcpy(out, sqlite3_libversion(), strlen(sqlite3_libversion())); }
