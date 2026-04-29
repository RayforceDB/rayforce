/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "lang/internal.h"
#include "lang/env.h"
#include "lang/parse.h"
#include "mem/heap.h"
#include "store/serde.h"
#include "store/splay.h"
#include "store/part.h"
#include "core/ipc.h"
#include <time.h>
#if !defined(RAY_OS_WINDOWS)
#include <unistd.h>
#endif

/* ══════════════════════════════════════════
 * Serialization / storage
 * ══════════════════════════════════════════ */

/* (ser val) -> serialize to U8 vector with IPC header */
ray_t* ray_ser_fn(ray_t* val) {
    return ray_ser(val);
}

/* (de bytes) -> deserialize from U8 vector */
ray_t* ray_de_fn(ray_t* val) {
    return ray_de(val);
}

/* Build default sym path: dir/sym. Returns NULL if file does not exist. */
static const char* splay_default_sym(const char* dir, char* buf, size_t bufsz,
                                     bool must_exist) {
    int n = snprintf(buf, bufsz, "%s/sym", dir);
    if (n < 0 || (size_t)n >= bufsz) return NULL;
    if (must_exist && access(buf, F_OK) != 0) return NULL;
    return buf;
}

/* Helper: extract null-terminated path from a STR atom into a stack buffer.
 * Returns pointer to buf on success, NULL on failure. */
static const char* str_to_cpath(ray_t* s, char* buf, size_t bufsz) {
    if (!s || s->type != -RAY_STR) return NULL;
    const char* p = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    if (!p || len == 0 || len >= bufsz) return NULL;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

/* (set-splayed "dir" table) or (set-splayed "dir" table "sym_path") */
ray_t* ray_set_splayed_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 3) return ray_error("domain", NULL);

    char dir[1024];
    if (!str_to_cpath(args[0], dir, sizeof(dir))) return ray_error("type", NULL);

    ray_t* tbl = args[1];
    if (!tbl || tbl->type != RAY_TABLE) return ray_error("type", NULL);

    char sym[1024];
    const char* sym_path = NULL;
    if (n == 3 && args[2] && args[2]->type == -RAY_STR)
        sym_path = str_to_cpath(args[2], sym, sizeof(sym));
    else
        sym_path = splay_default_sym(dir, sym, sizeof(sym), false);

    ray_err_t err = ray_splay_save(tbl, dir, sym_path);
    if (err != RAY_OK) return ray_error(ray_err_code_str(err), NULL);

    ray_retain(tbl);
    return tbl;
}

/* (get-splayed "dir") or (get-splayed "dir" "sym_path") */
ray_t* ray_get_splayed_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 2) return ray_error("domain", NULL);

    char dir[1024];
    if (!str_to_cpath(args[0], dir, sizeof(dir))) return ray_error("type", NULL);

    char sym[1024];
    const char* sym_path = NULL;
    if (n == 2 && args[1] && args[1]->type == -RAY_STR)
        sym_path = str_to_cpath(args[1], sym, sizeof(sym));
    else
        sym_path = splay_default_sym(dir, sym, sizeof(sym), true);

    return ray_splay_load(dir, sym_path);
}

/* (get-parted "db_root" `table_name) -- load partitioned table */
ray_t* ray_get_parted_fn(ray_t** args, int64_t n) {
    if (n != 2) return ray_error("domain", NULL);

    char root[1024];
    if (!str_to_cpath(args[0], root, sizeof(root))) return ray_error("type", NULL);

    /* Table name as symbol atom */
    if (!args[1] || args[1]->type != -RAY_SYM) return ray_error("type", NULL);
    ray_t* name_atom = ray_sym_str(args[1]->i64);
    if (!name_atom) return ray_error("name", NULL);

    char name[256];
    size_t nlen = ray_str_len(name_atom);
    if (nlen == 0 || nlen >= sizeof(name)) return ray_error("domain", NULL);
    memcpy(name, ray_str_ptr(name_atom), nlen);
    name[nlen] = '\0';

    return ray_read_parted(root, name);
}

/* xorshift64* — ~1ns per 64-bit word, vs rand()'s ~10ns for 1 byte.
 * Per-thread state seeded once with the result of rand() to keep the
 * (guid n) sequence varying across program runs (rand() is itself
 * seeded by the runtime).  v4 UUID quality only requires the version
 * and variant nibbles to be correct; the remaining 122 bits are
 * pseudo-random and xorshift64* is more than sufficient. */
static __thread uint64_t guid_rng_state = 0;

static inline uint64_t guid_rng_next(void) {
    uint64_t x = guid_rng_state;
    if (RAY_UNLIKELY(x == 0)) {
        /* Mix rand() into a non-zero seed.  rand() returns ≤ 31 bits, so
         * combine three calls plus an address-derived constant for
         * thread-distinct initialisation. */
        uint64_t a = (uint64_t)rand();
        uint64_t b = (uint64_t)rand();
        uint64_t c = (uint64_t)rand();
        x = (a << 33) ^ (b << 17) ^ c ^ 0x9E3779B97F4A7C15ULL;
        if (x == 0) x = 0x9E3779B97F4A7C15ULL;
    }
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    guid_rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* (guid n) -> generate n random GUIDs as GUID vector.
 * v4 UUID format: 122 random bits + 4 version-bits (0100) + 2 variant-bits (10). */
ray_t* ray_guid_fn(ray_t* n_arg) {
    if (!n_arg || !is_numeric(n_arg)) return ray_error("type", NULL);
    int64_t n = as_i64(n_arg);
    if (n < 0) return ray_error("domain", NULL);
    ray_t* result = ray_vec_new(RAY_GUID, n);
    if (RAY_IS_ERR(result)) return result;
    result->len = n;
    uint8_t* data = (uint8_t*)ray_data(result);
    /* Two 64-bit RNG calls per UUID give 16 random bytes; then we just
     * stamp the version/variant nibbles. */
    for (int64_t i = 0; i < n; i++) {
        uint64_t lo = guid_rng_next();
        uint64_t hi = guid_rng_next();
        memcpy(data + i * 16, &lo, 8);
        memcpy(data + i * 16 + 8, &hi, 8);
        data[i * 16 + 6] = (data[i * 16 + 6] & 0x0F) | 0x40;  /* version 4 */
        data[i * 16 + 8] = (data[i * 16 + 8] & 0x3F) | 0x80;  /* variant 10 */
    }
    return result;
}

/* ══════════════════════════════════════════
 * Eval, parse, print, system, env builtins
 * ══════════════════════════════════════════ */

/* (eval expr) -- evaluate a parsed expression */
ray_t* ray_eval_builtin_fn(ray_t* x) {
    return ray_eval(x);
}

/* (parse str) -- parse a string into an AST */
ray_t* ray_parse_builtin_fn(ray_t* x) {
    if (x->type != -RAY_STR) return ray_error("type", "parse expects a string");
    const char* src = ray_str_ptr(x);
    if (!src) return ray_error("domain", NULL);
    ray_t* parsed = ray_parse(src);
    return parsed ? parsed : ray_error("parse", NULL);
}

/* (print val) -- print without newline, return the value */
/* print moved to builtins.c alongside println/show */

/* (meta x) -- return metadata about an object as a dict */
ray_t* ray_meta_fn(ray_t* x) {
    if (!x) return ray_error("type", NULL);

    const char* tname = ray_type_name(x->type);
    int64_t type_sym = ray_sym_intern("type", 4);
    int64_t type_id  = ray_sym_intern(tname, strlen(tname));

    /* Build keys SYM vec + vals LIST. */
    int64_t cap = ray_is_atom(x) ? 1 : 2;
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, cap);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(cap);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    keys = ray_vec_append(keys, &type_sym);
    if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
    ray_t* tv = ray_sym(type_id);
    vals = ray_list_append(vals, tv);
    ray_release(tv);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    if (!ray_is_atom(x)) {
        int64_t len_sym = ray_sym_intern("len", 3);
        keys = ray_vec_append(keys, &len_sym);
        if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
        int64_t row_count;
        if (x->type == RAY_DICT)       row_count = ray_dict_len(x);
        else if (x->type == RAY_TABLE) row_count = ray_table_ncols(x);
        else                            row_count = x->len;
        ray_t* lv = make_i64(row_count);
        vals = ray_list_append(vals, lv);
        ray_release(lv);
        if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    }

    return ray_dict_new(keys, vals);
}

/* (.sys.gc) -- no-op garbage collection trigger, return 0.  Variadic
 * so the call site can be (.sys.gc) without the dummy-arg ceremony. */
ray_t* ray_gc_fn(ray_t** args, int64_t n) { (void)args; (void)n; return ray_i64(0); }

/* (system cmd) -- run shell command, return exit code */
ray_t* ray_system_fn(ray_t* x) {
    if (x->type != -RAY_STR) return ray_error("type", "system expects a string");
    const char* cmd = ray_str_ptr(x);
    if (!cmd) return ray_error("domain", NULL);
    int rc = system(cmd);
    return make_i64(rc);
}

/* (getenv name) -- get environment variable */
ray_t* ray_getenv_fn(ray_t* x) {
    if (x->type != -RAY_STR) return ray_error("type", "getenv expects a string");
    const char* name = ray_str_ptr(x);
    if (!name) return ray_error("domain", NULL);
    const char* val = getenv(name);
    return val ? ray_str(val, strlen(val)) : ray_str("", 0);
}

/* (setenv name val) -- set environment variable */
#if !defined(RAY_OS_WINDOWS)
extern int setenv(const char*, const char*, int);
#endif
ray_t* ray_setenv_fn(ray_t* name, ray_t* val) {
    if (name->type != -RAY_STR || val->type != -RAY_STR)
        return ray_error("type", "setenv expects two strings");
    const char* n = ray_str_ptr(name);
    const char* v = ray_str_ptr(val);
    if (!n || !v) return ray_error("domain", NULL);
#if defined(RAY_OS_WINDOWS)
    _putenv_s(n, v);
#else
    setenv(n, v, 1);
#endif
    return val;
}

/* ══════════════════════════════════════════
 * Quote, return, args, rc, diverse, get, remove,
 * timer, env, internals, memstat, sysinfo
 * ══════════════════════════════════════════ */

/* (quote expr) -- special form, returns argument unevaluated */
ray_t* ray_quote_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", "quote expects 1 argument");
    ray_retain(args[0]);
    return args[0];
}

/* (return x) -- early return from function (identity in Rayfall) */
ray_t* ray_return_fn(ray_t* x) {
    ray_retain(x);
    return x;
}

/* (args) -- return command-line arguments as a list of strings */
ray_t* ray_args_fn(ray_t* x) {
    (void)x;
    /* Return empty list -- CLI args not wired into eval context */
    ray_t* list = ray_list_new(0);
    if (!list) return ray_error("oom", NULL);
    return list;
}

/* (rc x) -- return reference count of object */
ray_t* ray_rc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return make_i64(0);
    return make_i64((int64_t)x->rc);
}

/* (diverse x) -- check if all elements in a collection are unique */
ray_t* ray_diverse_fn(ray_t* x) {
    if (ray_is_atom(x)) return make_bool(1);
    if (!is_collection(x)) return ray_error("type", "diverse expects a collection");

    int64_t n = ray_len(x);
    if (n <= 1) return make_bool(1);

    ray_t* d = ray_distinct_fn(x);
    if (RAY_IS_ERR(d)) return d;
    int64_t dn = ray_len(d);
    ray_release(d);
    return make_bool(dn == n ? 1 : 0);
}

/* (get dict key) -- dictionary/table lookup (alias for at) */
ray_t* ray_get_fn(ray_t* dict, ray_t* key) {
    return ray_at_fn(dict, key);
}

/* (remove dict key) -- remove key from dict, return new dict */
ray_t* ray_remove_fn(ray_t* dict, ray_t* key) {
    if (!dict || dict->type != RAY_DICT)
        return ray_error("type", "remove expects a dict");
    ray_retain(dict);
    return ray_dict_remove(dict, key);
}

/* (timer) -- return high-res timestamp in nanoseconds for benchmarking */
ray_t* ray_timer_fn(ray_t* x) {
    (void)x;
    clock_t t = clock();
    int64_t nanos = (int64_t)((double)t / (double)CLOCKS_PER_SEC * 1e9);
    return make_i64(nanos);
}

/* (env) -- return dict of all global environment bindings */
ray_t* ray_env_fn(ray_t* x) {
    (void)x;
    int64_t sym_ids[1024];
    ray_t* vals_buf[1024];
    int32_t count = ray_env_list(sym_ids, vals_buf, 1024);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, count);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(count);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    for (int32_t i = 0; i < count; i++) {
        keys = ray_vec_append(keys, &sym_ids[i]);
        if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
        vals = ray_list_append(vals, vals_buf[i]);
        if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    }
    return ray_dict_new(keys, vals);
}

/* (.sys.build) -- return dict with internal build information */
ray_t* ray_internals_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 2);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(2);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    int64_t ver_sym = ray_sym_intern("version", 7);
    keys = ray_vec_append(keys, &ver_sym);
#ifdef RAYFORCE_VERSION
    ray_t* v1 = ray_str(RAYFORCE_VERSION, strlen(RAYFORCE_VERSION));
#else
    ray_t* v1 = ray_str("unknown", 7);
#endif
    vals = ray_list_append(vals, v1); ray_release(v1);

    int64_t date_sym = ray_sym_intern("build-date", 10);
    keys = ray_vec_append(keys, &date_sym);
#ifdef RAYFORCE_BUILD_DATE
    ray_t* v2 = ray_str(RAYFORCE_BUILD_DATE, strlen(RAYFORCE_BUILD_DATE));
#else
    ray_t* v2 = ray_str("unknown", 7);
#endif
    vals = ray_list_append(vals, v2); ray_release(v2);

    return ray_dict_new(keys, vals);
}

/* (.sys.mem) -- return dict with memory allocator statistics */
ray_t* ray_memstat_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;
    ray_mem_stats_t st;
    ray_mem_stats(&st);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 5);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(5);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    struct { const char* name; size_t nlen; int64_t v; } rows[] = {
        { "alloc-count",     11, (int64_t)st.alloc_count     },
        { "bytes-allocated", 15, (int64_t)st.bytes_allocated },
        { "peak-bytes",      10, (int64_t)st.peak_bytes      },
        { "slab-hits",        9, (int64_t)st.slab_hits       },
        { "sys-current",     11, (int64_t)st.sys_current     },
    };
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        int64_t s = ray_sym_intern(rows[i].name, rows[i].nlen);
        keys = ray_vec_append(keys, &s);
        ray_t* v = make_i64(rows[i].v);
        vals = ray_list_append(vals, v); ray_release(v);
    }

    return ray_dict_new(keys, vals);
}

ray_t* ray_sysinfo_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 3);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(3);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

#if !defined(RAY_OS_WINDOWS)
    int64_t s1 = ray_sym_intern("cores", 5);
    keys = ray_vec_append(keys, &s1);
    ray_t* v1 = make_i64(sysconf(_SC_NPROCESSORS_ONLN));
    vals = ray_list_append(vals, v1); ray_release(v1);

    int64_t s2 = ray_sym_intern("page-size", 9);
    keys = ray_vec_append(keys, &s2);
    ray_t* v2 = make_i64(sysconf(_SC_PAGESIZE));
    vals = ray_list_append(vals, v2); ray_release(v2);

    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    int64_t s3 = ray_sym_intern("total-mem", 9);
    keys = ray_vec_append(keys, &s3);
    ray_t* v3 = make_i64((int64_t)pages * (int64_t)psize);
    vals = ray_list_append(vals, v3); ray_release(v3);
#else
    int64_t s1 = ray_sym_intern("cores", 5);
    keys = ray_vec_append(keys, &s1);
    ray_t* v1 = make_i64(1);
    vals = ray_list_append(vals, v1); ray_release(v1);
#endif

    return ray_dict_new(keys, vals);
}

/* ══════════════════════════════════════════
 * IPC builtins
 * ══════════════════════════════════════════ */

/* (hopen "host:port[:user:password]") → i64 handle */
ray_t* ray_hopen_fn(ray_t* x) {
    if (!ray_is_atom(x) || x->type != -RAY_STR)
        return ray_error("type", NULL);

    const char* s = ray_str_ptr(x);
    size_t slen = ray_str_len(x);

    /* Split on colons */
    const char* parts[4] = {0};
    size_t part_lens[4] = {0};
    int n_parts = 0;
    const char* start = s;
    for (size_t i = 0; i <= slen && n_parts < 4; i++) {
        if (i == slen || s[i] == ':') {
            parts[n_parts] = start;
            part_lens[n_parts] = (size_t)(&s[i] - start);
            n_parts++;
            start = &s[i + 1];
        }
    }
    if (n_parts < 2) return ray_error("domain", NULL);

    char host[256];
    if (part_lens[0] >= sizeof(host)) return ray_error("domain", NULL);
    memcpy(host, parts[0], part_lens[0]);
    host[part_lens[0]] = '\0';

    char port_str[8];
    if (part_lens[1] >= sizeof(port_str)) return ray_error("domain", NULL);
    memcpy(port_str, parts[1], part_lens[1]);
    port_str[part_lens[1]] = '\0';
    int port = atoi(port_str);
    if (port <= 0 || port > 65535) return ray_error("domain", NULL);

    char user[128] = "";
    char password[128] = "";
    if (n_parts >= 4) {
        if (part_lens[2] < sizeof(user)) {
            memcpy(user, parts[2], part_lens[2]);
            user[part_lens[2]] = '\0';
        }
        if (part_lens[3] < sizeof(password)) {
            memcpy(password, parts[3], part_lens[3]);
            password[part_lens[3]] = '\0';
        }
    }

    const char* pw_ptr = (n_parts >= 4) ? password : NULL;
    const char* us_ptr = (n_parts >= 4) ? user : NULL;

    int64_t h = ray_ipc_connect(host, (uint16_t)port, us_ptr, pw_ptr);
    if (h == -2) return ray_error("access", "server requires authentication");
    if (h == -3) return ray_error("access", "authentication failed");
    if (h < 0) return ray_error("io", "connection refused: %s:%d", host, port);

    return make_i64(h);
}

/* (hclose handle) → null */
ray_t* ray_hclose_fn(ray_t* x) {
    if (!ray_is_atom(x) || (x->type != -RAY_I64 && x->type != -RAY_I32))
        return ray_error("type", NULL);
    int64_t h = (x->type == -RAY_I64) ? x->i64 : x->i32;
    ray_ipc_close(h);
    return RAY_NULL_OBJ;
}

/* (hsend handle msg) → result */
ray_t* ray_hsend_fn(ray_t* handle, ray_t* msg) {
    if (!ray_is_atom(handle) || (handle->type != -RAY_I64 && handle->type != -RAY_I32))
        return ray_error("type", NULL);
    int64_t h = (handle->type == -RAY_I64) ? handle->i64 : handle->i32;
    /* Validate message is serializable (reject builtins, etc.) */
    if (ray_serde_size(msg) <= 0)
        return ray_error("type", "message not serializable");
    return ray_ipc_send(h, msg);
}
