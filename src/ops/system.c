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
#include "lang/eval.h"  /* LAMBDA_PARAMS */
#include "lang/parse.h"
#include "ops/ops.h"    /* ray_is_lazy, ray_lazy_materialize */
#include "ops/internal.h"   /* ray_group_perpart_runs — (.sys.mem) counter */
#include "mem/heap.h"
#include "mem/sys.h"
#include "core/profile.h"   /* g_ray_profile — (.sys.prof) */
#include "core/qlog.h"      /* g_qlog — (.sys.querylog) */
#include "store/serde.h"
#include "store/splay.h"
#include "store/part.h"
#include "core/ipc.h"
#include "core/poll.h"
#include "core/timer.h"

/* Forward decls: keep this TU decoupled from core/runtime.h — only
 * the opaque accessors are needed. */
void* ray_runtime_get_poll(void);
void  ray_runtime_set_sys_args(void* dict);
void* ray_runtime_get_sys_args(void);
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
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

/* True when `name` (n bytes) is partition-shaped: nonempty, digits and
 * dots only — the same loose classification collect_part_dirs
 * (store/part.c) applies to partition directory names. */
static bool sym_partition_shaped(const char* name, size_t n) {
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++)
        if (name[i] != '.' && (name[i] < '0' || name[i] > '9')) return false;
    return true;
}

/* Symfile resolution (sym-domain spec, "Surface"): for a partition dir
 * (/db/2024.01.01/t/) the table root is the PARTED ROOT — derived from
 * the path shape — and the domain is root/.sym; for a standalone splayed
 * dir the root is the dir itself (dir/.sym).  The symfile is a dotfile so
 * it can never collide with a user column (e.g. a "sym" ticker column).
 * Writes default to the convention unconditionally; reads prefer an
 * existing dir/.sym, then an existing partition root/.sym, else NULL (the
 * load layer raises the
 * loud "sym" error only if SYM columns actually exist). */
static const char* splay_resolve_sym(const char* dir, char* buf, size_t bufsz,
                                     bool for_write) {
    size_t dlen = strlen(dir);
    while (dlen > 1 && dir[dlen - 1] == '/') dlen--; /* strip trailing '/' */

    /* dir/.sym */
    int n = snprintf(buf, bufsz, "%.*s/.sym", (int)dlen, dir);
    if (n < 0 || (size_t)n >= bufsz) return NULL;
    if (!for_write && access(buf, F_OK) == 0) return buf;

    /* partition-shaped parent → parted root's root/sym */
    const char* slash = NULL;
    for (size_t i = dlen; i > 0; i--)
        if (dir[i - 1] == '/') { slash = dir + i - 1; break; }
    if (slash && slash > dir) {
        size_t parent_end = (size_t)(slash - dir);
        const char* pslash = NULL;
        for (size_t i = parent_end; i > 0; i--)
            if (dir[i - 1] == '/') { pslash = dir + i - 1; break; }
        const char* pname = pslash ? pslash + 1 : dir;
        size_t pname_len = (size_t)(dir + parent_end - pname);
        if (sym_partition_shaped(pname, pname_len) && pslash) {
            size_t root_len = (size_t)(pslash - dir);
            if (root_len == 0) root_len = 1; /* "/" root */
            int rn = snprintf(buf, bufsz, "%.*s/.sym", (int)root_len, dir);
            if (rn < 0 || (size_t)rn >= bufsz) return NULL;
            if (for_write || access(buf, F_OK) == 0) return buf;
            return NULL;
        }
    }

    if (for_write) {
        /* Not partition-shaped: the dir itself is the table root. */
        n = snprintf(buf, bufsz, "%.*s/.sym", (int)dlen, dir);
        if (n < 0 || (size_t)n >= bufsz) return NULL;
        return buf;
    }
    return NULL; /* read: no symfile resolvable */
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

/* (.db.splayed.set "dir" table) or (.db.splayed.set "dir" table "sym_path") */
ray_t* ray_set_splayed_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 3)
        return ray_error("domain", ".db.splayed.set expects 2 or 3 arguments, got %lld", (long long)n);

    char dir[1024];
    if (!str_to_cpath(args[0], dir, sizeof(dir)))
        return ray_error("type", ".db.splayed.set expects a string dir path, got %s", ray_type_name(args[0]->type));

    ray_t* tbl = args[1];
    if (!tbl || tbl->type != RAY_TABLE)
        return ray_error("type", ".db.splayed.set expects a table, got %s", ray_type_name(tbl->type));

    char sym[1024];
    const char* sym_path = NULL;
    if (n == 3 && args[2] && args[2]->type == -RAY_STR)
        sym_path = str_to_cpath(args[2], sym, sizeof(sym));
    else
        sym_path = splay_resolve_sym(dir, sym, sizeof(sym), true);

    ray_err_t err = ray_splay_save(tbl, dir, sym_path);
    if (err != RAY_OK) return ray_error(ray_err_code_str(err), NULL);

    /* Build + persist accelerator indexes (STR dictionaries, numeric chunk-zone
     * min/max) inline at each column file's tail so mmap loads get the fast
     * paths — same pass .csv.splayed runs. */
    ray_splay_build_indexes(dir, tbl);

    ray_retain(tbl);
    return tbl;
}

/* (.db.splayed.get "dir") or (.db.splayed.get "dir" "sym_path") */
ray_t* ray_get_splayed_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 2)
        return ray_error("domain", ".db.splayed.get expects 1 or 2 arguments, got %lld", (long long)n);

    char dir[1024];
    if (!str_to_cpath(args[0], dir, sizeof(dir)))
        return ray_error("type", ".db.splayed.get expects a string dir path, got %s", ray_type_name(args[0]->type));

    char sym[1024];
    const char* sym_path = NULL;
    if (n == 2 && args[1] && args[1]->type == -RAY_STR)
        sym_path = str_to_cpath(args[1], sym, sizeof(sym));
    else
        sym_path = splay_resolve_sym(dir, sym, sizeof(sym), false);

    /* sym_path == NULL: no symfile resolvable (no explicit arg, no
     * dir/sym, no partition root/sym).  The load layer raises the loud
     * "sym" error if the table actually has SYM columns; symbol-free
     * tables load fine without one. */
    return ray_read_splayed(dir, sym_path);
}

/* (.db.parted.get "db_root" `table_name) -- load partitioned table */
ray_t* ray_get_parted_fn(ray_t** args, int64_t n) {
    if (n != 2)
        return ray_error("domain", ".db.parted.get expects 2 arguments, got %lld", (long long)n);

    char root[1024];
    if (!str_to_cpath(args[0], root, sizeof(root)))
        return ray_error("type", ".db.parted.get expects a string db root path, got %s", ray_type_name(args[0]->type));

    /* Table name as symbol atom */
    if (!args[1] || args[1]->type != -RAY_SYM)
        return ray_error("type", ".db.parted.get expects a sym table name, got %s", ray_type_name(args[1]->type));
    ray_t* name_atom = ray_sym_str(args[1]->i64);
    if (!name_atom) return ray_error("name", NULL);

    char name[256];
    size_t nlen = ray_str_len(name_atom);
    if (nlen == 0 || nlen >= sizeof(name))
        return ray_error("domain", ".db.parted.get table name length out of range, got %lld", (long long)nlen);
    memcpy(name, ray_str_ptr(name_atom), nlen);
    name[nlen] = '\0';

    return ray_read_parted(root, name);
}

/* (.db.parted.tables "db_root") → sym vector of table names available under
 * the root, suitable for passing to .db.parted.get. */
ray_t* ray_get_parted_tables_fn(ray_t** args, int64_t n) {
    if (n != 1)
        return ray_error("domain", ".db.parted.tables expects 1 argument, got %lld", (long long)n);

    char root[1024];
    if (!str_to_cpath(args[0], root, sizeof(root)))
        return ray_error("type", ".db.parted.tables expects a string db root path, got %s", ray_type_name(args[0]->type));

    return ray_parted_tables(root);
}

/* (.db.parted.fill "db_root") → sym vector of partition names that were
 * filled.  Writes an empty copy of every table into the partitions that lack
 * it (schema from the most recent partition with it). */
ray_t* ray_fill_parted_fn(ray_t** args, int64_t n) {
    if (n != 1)
        return ray_error("domain", ".db.parted.fill expects 1 argument, got %lld", (long long)n);

    char root[1024];
    if (!str_to_cpath(args[0], root, sizeof(root)))
        return ray_error("type", ".db.parted.fill expects a string db root path, got %s", ray_type_name(args[0]->type));

    return ray_parted_fill(root);
}

/* stat/dirent used by the .fs.* filesystem metadata builtins below. */
#include <sys/stat.h>
#include <dirent.h>

/* ══════════════════════════════════════════
 * Filesystem metadata: .fs.size / .fs.list
 *
 * Issue #36 asked for size + existence + listing primitives.  We
 * keep just two — `.fs.size` and `.fs.list` — because every other
 * predicate (exists, is-file, is-dir) is reachable either via
 * try-on-error against these or via the existing shell fallback
 * (`(.sys.cmd "test -e p")` etc.).  Both errors are flagged "io"
 * so a user wrapping the call in `try` can distinguish missing /
 * wrong-kind from a domain mistake without introspecting the
 * message.
 * ══════════════════════════════════════════ */

/* (.fs.size "path") → i64 file size in bytes.  Errors with "io"
 * when the path doesn't exist or names a directory — `try` it if
 * the caller wants those treated as "not a file" rather than a
 * hard error. */
ray_t* ray_fs_size_fn(ray_t* x) {
    if (!ray_is_atom(x) || x->type != -RAY_STR)
        return ray_error("type", ".fs.size expects a string path");
    char path[1024];
    /* x is already a STR atom here; str_to_cpath fails only when the path
     * is empty or exceeds the buffer — a domain/range issue, not a type
     * mismatch (old code: "type" → "domain"). */
    if (!str_to_cpath(x, path, sizeof(path)))
        return ray_error("domain", ".fs.size path is empty or too long, got %lld bytes", (long long)ray_str_len(x));

    struct stat st;
    if (stat(path, &st) != 0)
        return ray_error("io", "%s: %s", path, strerror(errno));
    if (S_ISDIR(st.st_mode))
        return ray_error("io", "%s: is a directory", path);
    return ray_i64((int64_t)st.st_size);
}

/* qsort comparator for sorting directory entries by name.  Filesystem
 * order from readdir is implementation-defined; sorting gives stable
 * output for tests and predictable iteration in user code. */
static int dir_entry_cmp(const void* a, const void* b) {
    const char* sa = *(const char* const*)a;
    const char* sb = *(const char* const*)b;
    return strcmp(sa, sb);
}

/* (.fs.list "path") → sym vec of entries, sorted, with `.` and `..`
 * filtered out.  Errors with "io" if the path isn't a directory or
 * doesn't exist — caller can use that as a file/dir discriminator
 * via `try` when they don't want to shell out for the predicate. */
ray_t* ray_fs_list_fn(ray_t* x) {
    if (!ray_is_atom(x) || x->type != -RAY_STR)
        return ray_error("type", ".fs.list expects a string path");
    char path[1024];
    /* x is already a STR atom here; str_to_cpath fails only when the path
     * is empty or exceeds the buffer — a domain/range issue, not a type
     * mismatch (old code: "type" → "domain"). */
    if (!str_to_cpath(x, path, sizeof(path)))
        return ray_error("domain", ".fs.list path is empty or too long, got %lld bytes", (long long)ray_str_len(x));

    DIR* d = opendir(path);
    if (!d) return ray_error("io", "%s: %s", path, strerror(errno));

    /* Collect names into a sys-allocator string array; capacity grows
     * geometrically so big directories don't quadratic-realloc.  Uses
     * ray_sys_alloc/realloc/strdup/free (NOT libc) per project policy —
     * mirrors the collect_part_dirs idiom in src/store/part.c. */
    char** names = NULL;
    int64_t count = 0;
    int64_t cap = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        if (count >= cap) {
            int64_t new_cap = cap == 0 ? 16 : cap * 2;
            char** tmp = (char**)ray_sys_realloc(names, (size_t)new_cap * sizeof(char*));
            if (!tmp) {
                closedir(d);
                for (int64_t i = 0; i < count; i++) ray_sys_free(names[i]);
                ray_sys_free(names);
                return ray_error("oom", NULL);
            }
            names = tmp;
            cap = new_cap;
        }
        char* dup = ray_sys_strdup(ent->d_name);
        if (!dup) {
            closedir(d);
            for (int64_t i = 0; i < count; i++) ray_sys_free(names[i]);
            ray_sys_free(names);
            return ray_error("oom", NULL);
        }
        names[count++] = dup;
    }
    closedir(d);

    /* qsort(NULL, 0, ...) is undefined behaviour per C11 §7.22.5.2/2
     * even when nmemb == 0 (UBSan flags it via qsort's nonnull attr).
     * Skip the call when the directory was empty — an empty array is
     * already sorted. */
    if (count > 0)
        qsort(names, (size_t)count, sizeof(char*), dir_entry_cmp);

    ray_t* result = ray_vec_new(RAY_SYM, count);
    if (!result || RAY_IS_ERR(result)) {
        for (int64_t i = 0; i < count; i++) ray_sys_free(names[i]);
        ray_sys_free(names);
        return result ? result : ray_error("oom", NULL);
    }
    result->len = count;
    int64_t* out = (int64_t*)ray_data(result);
    for (int64_t i = 0; i < count; i++) {
        out[i] = ray_sym_intern(names[i], strlen(names[i]));
        ray_sys_free(names[i]);
    }
    ray_sys_free(names);
    return result;
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
    if (!n_arg || !is_numeric(n_arg))
        return ray_error("type", "guid expects a numeric count, got %s", ray_type_name(n_arg->type));
    int64_t n = as_i64(n_arg);
    if (n < 0) return ray_error("domain", "guid count must be non-negative, got %lld", (long long)n);
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
    if (!src) return ray_error("domain", "parse: empty or invalid source string");
    ray_t* parsed = ray_parse(src);
    return parsed ? parsed : ray_error("parse", NULL);
}

/* (print val) -- print without newline, return the value */
/* print moved to builtins.c alongside println/show */

/* (meta x) -- return metadata about an object as a dict */
ray_t* ray_meta_fn(ray_t* x) {
    /* x is NULL here, so no ->type to report. */
    if (!x) return ray_error("type", "meta expects a value");

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
#ifdef RAY_FUZZING
    /* Shell escape — disabled under fuzzing so untrusted input can't run
     * arbitrary commands in the fuzzer process. */
    (void)x;
    return ray_error("restricted", "shell disabled under fuzzing");
#else
    if (x->type != -RAY_STR) return ray_error("type", "system expects a string");
    const char* cmd = ray_str_ptr(x);
    if (!cmd) return ray_error("domain", ".sys.exec: empty or invalid command string");
    int rc = system(cmd);
    return make_i64(rc);
#endif
}

/* (getenv name) -- get environment variable */
ray_t* ray_getenv_fn(ray_t* x) {
    if (x->type != -RAY_STR) return ray_error("type", "getenv expects a string");
    /* ray strings are length-prefixed and NOT NUL-terminated when pooled
     * (len > RAY_STR_INLINE_MAX), so the raw ray_str_ptr cannot be handed
     * to libc getenv directly — copy into a NUL-terminated buffer first. */
    char namebuf[256];
    const char* name = str_to_cpath(x, namebuf, sizeof(namebuf));
    if (!name) return ray_error("domain", ".os.getenv name is empty or too long, got %lld bytes", (long long)ray_str_len(x));
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
    /* libc setenv needs NUL-terminated args; ray strings are not
     * NUL-terminated when pooled (len > RAY_STR_INLINE_MAX). */
    char namebuf[256];
    const char* n = str_to_cpath(name, namebuf, sizeof(namebuf));
    if (!n) return ray_error("domain", ".os.setenv name is empty or too long, got %lld bytes", (long long)ray_str_len(name));
    char valbuf[4096];
    size_t vlen = ray_str_len(val);
    if (vlen >= sizeof(valbuf))
        return ray_error("range", ".os.setenv value too long, got %lld bytes", (long long)vlen);
    const char* vp = ray_str_ptr(val);
    if (!vp) return ray_error("domain", ".os.setenv: invalid value string");
    if (vlen) memcpy(valbuf, vp, vlen);
    valbuf[vlen] = '\0';
#if defined(RAY_OS_WINDOWS)
    _putenv_s(n, valbuf);
#else
    setenv(n, valbuf, 1);
#endif
    ray_retain(val);
    return val;
}

/* ══════════════════════════════════════════
 * Quote, return, args, rc, diverse, get, remove,
 * timer, env, internals, memstat, sysinfo
 * ══════════════════════════════════════════ */

/* (quote expr) -- special form, returns argument unevaluated.
 * A bare-name symbol argument becomes a LITERAL symbol (≡ the tick form
 * 'x): return a flag-set COPY — args[0] is the shared unevaluated AST
 * node, re-evaluated in loops / reused lambda bodies, so never mutate it
 * in place.  ray_sym() freshly allocates (mirrors parse.c's tick path),
 * so setting ATTR_QUOTED on the copy is safe.  Already-literal symbols
 * and all non-symbol values pass through unchanged. */
ray_t* ray_quote_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", "quote expects 1 argument");
    ray_t* a = args[0];
    if (a->type == -RAY_SYM && !(a->attrs & ATTR_QUOTED)) {
        ray_t* q = ray_sym(a->i64);
        if (RAY_IS_ERR(q)) return q;
        q->attrs |= ATTR_QUOTED;
        return q;
    }
    ray_retain(a);
    return a;
}

/* (return) | (return x) — early exit from enclosing compiled lambda.
 * Outside a compiled lambda (e.g. value position: (map return xs), or
 * REPL top-level) this collapses to: identity for one arg, null for
 * zero, domain error otherwise. The early-exit semantics are emitted
 * by the bytecode compiler — see compile.c. */
ray_t* ray_return_fn(ray_t** args, int64_t n) {
    if (n == 0) return RAY_NULL_OBJ;
    if (n == 1) { ray_retain(args[0]); return args[0]; }
    return ray_error("domain", "return expects 0 or 1 argument");
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
    /* ray_distinct_fn returns lazy for concrete vecs; materialise here
     * since we need the concrete length to compare. */
    if (ray_is_lazy(d)) {
        d = ray_lazy_materialize(d);
        if (!d || RAY_IS_ERR(d)) return d ? d : ray_error("oom", NULL);
    }
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

/* (.time.now) -- current monotonic time in milliseconds */
ray_t* ray_time_now_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("domain", ".time.now takes no arguments");
    return make_i64(ray_time_now_ms());
}

/* (.time.timer.set ms num fn) -- schedule fn to fire every ms; returns id */
ray_t* ray_time_timer_set_fn(ray_t** args, int64_t n) {
    if (n != 3)
        return ray_error("domain", ".time.timer.set expects 3 arguments");
    if (args[0]->type != -RAY_I64) return ray_error("type", "ms must be i64");
    if (args[1]->type != -RAY_I64) return ray_error("type", "num must be i64");
    if (args[2]->type != RAY_LAMBDA)
        return ray_error("type", "fn must be a lambda");

    int64_t ms  = args[0]->i64;
    int64_t num = args[1]->i64;
    if (ms  < 0) return ray_error("domain", "ms must be non-negative");
    if (num < 0) return ray_error("domain", "num must be non-negative");

    /* Check lambda arity: declared params list must have exactly 1 element. */
    ray_t* params = LAMBDA_PARAMS(args[2]);
    if (!params || ray_len(params) != 1)
        return ray_error("domain", "fn must accept exactly 1 argument");

    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    if (!poll) return ray_error("domain", "no poll loop active");

    if (!poll->timers) {
        poll->timers = ray_timers_create(16);
        if (!poll->timers) return ray_error("oom", NULL);
    }

    int64_t id = ray_timers_add((ray_timers_t*)poll->timers, ms, num, args[2]);
    if (id < 0) return ray_error("oom", NULL);
    return make_i64(id);
}

/* (.time.timer.del id) -- cancel a timer; returns null */
ray_t* ray_time_timer_del_fn(ray_t* id) {
    if (!id || id->type != -RAY_I64) return ray_error("type", "id must be i64");
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    if (!poll || !poll->timers) return RAY_NULL_OBJ;
    ray_timers_del((ray_timers_t*)poll->timers, id->i64);
    return RAY_NULL_OBJ;
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

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 16);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(16);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    struct { const char* name; size_t nlen; int64_t v; } rows[] = {
        { "alloc-count",     11, (int64_t)st.alloc_count       },
        { "bytes-allocated", 15, (int64_t)st.bytes_allocated   },
        { "direct-bytes",    12, (int64_t)st.direct_bytes      },
        { "peak-bytes",      10, (int64_t)st.peak_bytes        },
        { "slab-hits",        9, (int64_t)st.slab_hits         },
        { "sys-current",     11, (int64_t)st.sys_current       },
        { "sys-mapped",      10, (int64_t)st.sys_mapped        },
        { "sys-mapped-peak", 15, (int64_t)st.sys_mapped_peak   },
        { "anon-committed",  14, ray_heap_anon_committed()     },
        { "anon-peak",        9, ray_heap_anon_peak()          },
        { "anon-watermark",  14, ray_heap_anon_watermark()     },
        { "group-perpart-runs", 18, ray_group_perpart_runs()   },
        { "asof-perpart-runs",  17, ray_asof_perpart_runs()    },
        { "sort-perpart-runs",  17, ray_sort_perpart_runs()    },
        { "window-perpart-runs", 19, ray_window_perpart_runs() },
        { "join-perpart-runs",   17, ray_join_perpart_runs()   },
    };
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        int64_t s = ray_sym_intern(rows[i].name, rows[i].nlen);
        keys = ray_vec_append(keys, &s);
        ray_t* v = make_i64(rows[i].v);
        vals = ray_list_append(vals, v); ray_release(v);
    }

    return ray_dict_new(keys, vals);
}

/* (.sys.prof) — materialize the last profiled query's span log as a table.
 *
 * One row per completed operator span (START/END pair) and per phase tick
 * (parse / eval / materialize / optimizer passes), in completion order.
 * Columns:
 *   operator       the span label (symbol)
 *   depth          nesting depth (0 = top level)
 *   cumulative-ms  wall-clock milliseconds, including children
 *   exclusive-ms   self time — this span minus its children
 *   percent        share of total query time (exclusive-based, sums to ~100)
 *   rows           result element/row count (operator spans)
 *   allocated-kib  net process bytes allocated across the span, KiB
 *   workers        worker threads that ran a task for this span
 *   busy-ms        summed worker busy time, ms (parallelism)
 *   parallelism    effective parallelism = busy time / wall time
 *
 * Because it returns an ordinary table, an IPC client gets the profile
 * with no protocol work.  Empty (0 rows) when the profiler is inactive or
 * no query has run.  Profiling is toggled with `:t` / the `-t` flag.
 */
ray_t* ray_prof_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;

    /* Read the snapshot of the last completed query — NOT the in-flight
     * buffer, which currently holds this `(.sys.prof)` call's own spans.
     * The snapshot is taken at each eval boundary (ray_profile_snapshot),
     * so it holds the fully-materialized query that ran just before. */
    const ray_profile_t* p = &g_ray_profile_last;
    int32_t ns = p->n;

    /* Count output rows: one per END and per TICK span. */
    int64_t nrows = 0;
    for (int32_t i = 0; i < ns; i++)
        if (p->spans[i].type != RAY_PROF_SPAN_START) nrows++;

    ray_t* c_op    = ray_vec_new(RAY_SYM, nrows);
    ray_t* c_depth = ray_vec_new(RAY_I64, nrows);
    ray_t* c_dur   = ray_vec_new(RAY_F64, nrows);
    ray_t* c_self  = ray_vec_new(RAY_F64, nrows);
    ray_t* c_pct   = ray_vec_new(RAY_F64, nrows);
    ray_t* c_rows  = ray_vec_new(RAY_I64, nrows);
    ray_t* c_alloc = ray_vec_new(RAY_F64, nrows);
    ray_t* c_wrk   = ray_vec_new(RAY_I64, nrows);
    ray_t* c_busy  = ray_vec_new(RAY_F64, nrows);
    ray_t* c_par   = ray_vec_new(RAY_F64, nrows);
    ray_t* cols[] = { c_op, c_depth, c_dur, c_self, c_pct,
                      c_rows, c_alloc, c_wrk, c_busy, c_par };
    for (size_t i = 0; i < sizeof(cols)/sizeof(cols[0]); i++)
        if (!cols[i] || RAY_IS_ERR(cols[i])) {
            for (size_t j = 0; j < sizeof(cols)/sizeof(cols[0]); j++)
                if (cols[j] && !RAY_IS_ERR(cols[j])) ray_release(cols[j]);
            return ray_error("oom", "sys.prof: column alloc");
        }

    int64_t* op_ids  = (int64_t*)ray_data(c_op);
    int64_t* depths  = (int64_t*)ray_data(c_depth);
    double*  durs    = (double*)ray_data(c_dur);
    double*  selfs   = (double*)ray_data(c_self);
    double*  pcts    = (double*)ray_data(c_pct);
    int64_t* rows    = (int64_t*)ray_data(c_rows);
    double*  allocs  = (double*)ray_data(c_alloc);
    int64_t* wrks    = (int64_t*)ray_data(c_wrk);
    double*  busys   = (double*)ray_data(c_busy);
    double*  pars    = (double*)ray_data(c_par);

    /* Total wall time = the whole profiled window (first START → last END).
     * Used for the `pct` share-of-total column, like EXPLAIN ANALYZE. */
    int64_t total_ns = ns > 0 ? (p->spans[ns-1].ts - p->spans[0].ts) : 0;

    /* Walk the flat span log, pairing START/END via a depth stack.
     * child_ns[d] accumulates the wall time of the direct children of the
     * span currently open at depth d, so each span's EXCLUSIVE (self) time
     * is its own wall minus that sum — standard per-operator
     * exclusive timing (cumulative is derivable by summing the subtree). */
    int32_t stack[256];
    int64_t child_ns[256] = {0};
    int32_t sp = 0;
    int64_t prev_ts = ns > 0 ? p->spans[0].ts : 0;
    int64_t r = 0;
    for (int32_t i = 0; i < ns; i++) {
        const ray_prof_span_t* s = &p->spans[i];
        if (s->type == RAY_PROF_SPAN_START) {
            if (sp < 256) { stack[sp] = i; child_ns[sp] = 0; }
            sp++;
        } else if (s->type == RAY_PROF_SPAN_END) {
            if (sp > 0) sp--;
            const ray_prof_span_t* st = (sp < 256 && sp >= 0) ? &p->spans[stack[sp]] : s;
            int64_t wall = s->ts - st->ts;
            int64_t self = (sp < 256 && sp >= 0) ? wall - child_ns[sp] : wall;
            if (self < 0) self = 0;
            /* Fold this span's wall into its parent's child accumulator. */
            if (sp - 1 >= 0 && sp - 1 < 256) child_ns[sp - 1] += wall;
            int64_t busy = (int64_t)(s->qs_busy_ns - st->qs_busy_ns);
            const char* nm = s->msg ? s->msg : "?";
            op_ids[r] = ray_sym_intern(nm, strlen(nm));
            depths[r] = sp;
            durs[r]   = (double)wall / 1e6;
            selfs[r]  = (double)self / 1e6;
            /* Percent is EXCLUSIVE-based so the column partitions the query
             * and sums to ~100% — a direct "where did time go" ranking. */
            pcts[r]   = total_ns > 0 ? (double)self * 100.0 / (double)total_ns : 0.0;
            rows[r]   = s->rows_out;
            allocs[r] = (double)(s->sys_cur - st->sys_cur) / 1024.0;
            wrks[r]   = (int64_t)s->qs_workers;
            busys[r]  = (double)busy / 1e6;
            pars[r]   = wall > 0 ? (double)busy / (double)wall : 0.0;
            r++;
        } else { /* TICK: a point phase (parse / eval / optimizer pass). */
            int64_t wall = s->ts - prev_ts;
            /* A tick is a leaf phase — count it against its parent's self. */
            if (sp - 1 >= 0 && sp - 1 < 256) child_ns[sp - 1] += wall;
            const char* nm = s->msg ? s->msg : "?";
            op_ids[r] = ray_sym_intern(nm, strlen(nm));
            depths[r] = sp;
            durs[r]   = (double)wall / 1e6;
            selfs[r]  = (double)wall / 1e6;
            pcts[r]   = total_ns > 0 ? (double)wall * 100.0 / (double)total_ns : 0.0;
            rows[r]   = 0; allocs[r] = 0.0;
            wrks[r]   = 0; busys[r] = 0.0; pars[r] = 0.0;
            r++;
        }
        prev_ts = s->ts;
    }

    /* ray_vec_new allocates capacity but leaves len at 0; publish the
     * filled row count on every column. */
    for (size_t i = 0; i < sizeof(cols)/sizeof(cols[0]); i++)
        cols[i]->len = r;

    /* Assemble the table. */
    static const char* names[] = { "operator","depth","cumulative-ms","exclusive-ms",
                                   "percent","rows","allocated-kib",
                                   "workers","busy-ms","parallelism" };
    ray_t* tbl = ray_table_new(10);
    if (!tbl || RAY_IS_ERR(tbl)) {
        for (size_t i = 0; i < sizeof(cols)/sizeof(cols[0]); i++) ray_release(cols[i]);
        return tbl ? tbl : ray_error("oom", "sys.prof: table");
    }
    for (size_t i = 0; i < sizeof(cols)/sizeof(cols[0]); i++) {
        int64_t nid = ray_sym_intern(names[i], strlen(names[i]));
        tbl = ray_table_add_col(tbl, nid, cols[i]);
        ray_release(cols[i]);
        if (RAY_IS_ERR(tbl)) return tbl;
    }
    return tbl;
}

/* (.sys.querylog) — the in-memory query-statistics ring as a table.
 *
 * One row per completed query (oldest first), capped at the ring capacity.
 * Ambient server-side query-log statistics (a system query_log table): read
 * back with ordinary queries, no extra protocol.  Each call materializes a
 * fresh snapshot, so anything a caller does to the returned table (upsert,
 * delete) touches the copy, never the ring.  Empty until logging is enabled
 * (`-Q` flag or `(.sys.querylog.enable 1)`).  Columns:
 *
 *   time         wall-clock time the query finished (timestamp)
 *   duration-ms  total wall time
 *   result-rows  rows in the result (scalar => 1)
 *   memory-kib   net process allocation across the query, KiB
 *   workers      worker threads that ran a task
 *   parallelism  worker busy time / wall time
 *   status       `ok`, or the error kind on failure
 *   query        source text (truncated)
 */
ray_t* ray_qlog_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("domain", ".sys.querylog takes no arguments");
    return ray_qlog_table();
}

/* (.sys.querylog.enable [flag]) — toggle query-statistics logging.  No arg
 * flips the current state; 0 disables; non-zero enables.  Returns the new
 * state as i64 (0/1).  Restricted: it changes server-wide behaviour. */
ray_t* ray_qlog_enable_fn(ray_t** args, int64_t n) {
    bool on;
    if (n == 0) {
        on = !ray_qlog_enabled();
    } else if (n == 1) {
        if (args[0]->type != -RAY_I64) return ray_error("type", "flag must be i64");
        on = args[0]->i64 != 0;
    } else {
        return ray_error("domain", ".sys.querylog.enable takes 0 or 1 arguments");
    }
    ray_qlog_set_enabled(on);
    return make_i64(on ? 1 : 0);
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

    int64_t s3 = ray_sym_intern("total-mem", 9);
    keys = ray_vec_append(keys, &s3);
    ray_t* v3 = make_i64(ray_sys_total_ram());
    vals = ray_list_append(vals, v3); ray_release(v3);
#else
    int64_t s1 = ray_sym_intern("cores", 5);
    keys = ray_vec_append(keys, &s1);
    ray_t* v1 = make_i64(1);
    vals = ray_list_append(vals, v1); ray_release(v1);
#endif

    return ray_dict_new(keys, vals);
}

/* Parse argv into the .sys.args dict.  Recognized launcher flags become
 * typed top-level entries (i64 / bool / str); everything after `--` becomes
 * a str->str `user` subdict via `-k v` / `--k v` pairing.  Auth passwords
 * (-u/-U) are intentionally NOT exposed. */
ray_t* ray_build_sys_args(int argc, char** argv) {
    const char* file = "";
    const char* log  = "";
    int64_t port = 0, cores = 0;
    bool timeit = false, interactive = false, querylog = false;
    int user_start = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { user_start = i + 1; break; }
        else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
            interactive = true;
        else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc)
            port = (int64_t)atoll(argv[++i]);
        else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--cores") == 0) && i + 1 < argc) {
            long long v = atoll(argv[++i]); if (v < 0) v = 0; cores = (int64_t)v;
        }
        else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--timeit") == 0) && i + 1 < argc)
            timeit = (atoll(argv[++i]) != 0);
        else if ((strcmp(argv[i], "-Q") == 0 || strcmp(argv[i], "--querylog") == 0) && i + 1 < argc)
            querylog = (atoll(argv[++i]) != 0);
        else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) && i + 1 < argc)
            file = argv[++i];
        else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) i++;   /* skip secret */
        else if (strcmp(argv[i], "-U") == 0 && i + 1 < argc) i++;   /* skip secret */
        else if ((strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-L") == 0) && i + 1 < argc)
            log = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { /* ignore */ }
        else file = argv[i];
    }

    /* user subdict (built by upsert so dup keys collapse, last wins) */
    ray_t* uk = ray_sym_vec_new(RAY_SYM_W64, 0);
    ray_t* uv = ray_list_new(0);
    ray_t* user = ray_dict_new(uk, uv);
    if (user_start >= 0) {
        for (int i = user_start; i < argc; ) {
            const char* tok = argv[i];
            if (tok[0] == '-') {
                const char* name = tok; while (*name == '-') name++;
                if (*name == '\0') { i++; continue; }   /* lone - / -- */
                const char* value = "";
                if (i + 1 < argc && argv[i + 1][0] != '-') { value = argv[i + 1]; i += 2; }
                else i += 1;
                ray_t* ka = ray_sym(ray_sym_intern(name, strlen(name)));
                ray_t* va = ray_str(value, strlen(value));
                ray_t* nu = ray_dict_upsert(user, ka, va);
                ray_release(ka); ray_release(va);
                if (RAY_IS_ERR(nu)) {
                    ray_release(nu);                              /* drop the error obj */
                    user = ray_dict_new(ray_sym_vec_new(RAY_SYM_W64, 0), ray_list_new(0));
                    break;                                        /* `user` already freed by upsert */
                }
                user = nu;
            } else {
                i++;   /* bare value with no key — ignore */
            }
        }
    }

    /* top-level dict: 8 entries (file, port, cores, timeit, querylog, interactive, log, user) */
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 8);
    ray_t* vals = ray_list_new(8);

    int64_t k;
    ray_t* v;
    k = ray_sym_intern("file", 4);        keys = ray_vec_append(keys, &k);
    v = ray_str(file, strlen(file));      vals = ray_list_append(vals, v); ray_release(v);
    k = ray_sym_intern("port", 4);        keys = ray_vec_append(keys, &k);
    v = ray_i64(port);                    vals = ray_list_append(vals, v); ray_release(v);
    k = ray_sym_intern("cores", 5);       keys = ray_vec_append(keys, &k);
    v = ray_i64(cores);                   vals = ray_list_append(vals, v); ray_release(v);
    k = ray_sym_intern("timeit", 6);      keys = ray_vec_append(keys, &k);
    v = ray_bool(timeit);                 vals = ray_list_append(vals, v); ray_release(v);
    k = ray_sym_intern("querylog", 8);    keys = ray_vec_append(keys, &k);
    v = ray_bool(querylog);               vals = ray_list_append(vals, v); ray_release(v);
    k = ray_sym_intern("interactive", 11);keys = ray_vec_append(keys, &k);
    v = ray_bool(interactive);            vals = ray_list_append(vals, v); ray_release(v);
    k = ray_sym_intern("log", 3);         keys = ray_vec_append(keys, &k);
    v = ray_str(log, strlen(log));        vals = ray_list_append(vals, v); ray_release(v);
    k = ray_sym_intern("user", 4);        keys = ray_vec_append(keys, &k);
    vals = ray_list_append(vals, user);   ray_release(user);

    return ray_dict_new(keys, vals);
}

/* (.sys.args) -- return the application-arguments dict (empty if unset) */
ray_t* ray_sys_args_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("domain", ".sys.args takes no arguments");
    ray_t* d = (ray_t*)ray_runtime_get_sys_args();
    if (d) { ray_retain(d); return d; }
    return ray_dict_new(ray_sym_vec_new(RAY_SYM_W64, 0), ray_list_new(0));
}

/* ══════════════════════════════════════════
 * IPC builtins
 * ══════════════════════════════════════════ */

/* (hopen "host:port[:user:password]" [timeout-ms]) → i64 handle.
 * The optional second argument bounds the TCP connect and handshake in
 * milliseconds; omitted leaves the default budget. */
ray_t* ray_hopen_fn(ray_t** args, int64_t n) {
#ifdef RAY_FUZZING
    /* No outbound connections under fuzzing. */
    (void)args; (void)n;
    return ray_error("restricted", "ipc.open disabled under fuzzing");
#else
    if (n < 1 || n > 2)
        return ray_error("rank", ".ipc.open expects 1 or 2 arguments: \"host:port[:user:password]\" [timeout-ms]");
#endif

    ray_t* x = args[0];
    if (!ray_is_atom(x) || x->type != -RAY_STR)
        return ray_error("type", ".ipc.open expects a string \"host:port[:user:password]\", got %s", ray_type_name(x->type));

    /* Optional connect timeout in milliseconds (0 = use default). */
    int timeout_ms = 0;
    if (n == 2) {
        ray_t* t = args[1];
        if (!ray_is_atom(t) || (t->type != -RAY_I64 && t->type != -RAY_I32))
            return ray_error("type", ".ipc.open timeout must be an integer (milliseconds), got %s", ray_type_name(t->type));
        int64_t tv = (t->type == -RAY_I64) ? t->i64 : t->i32;
        if (tv < 0) return ray_error("domain", ".ipc.open timeout must be >= 0, got %lld", (long long)tv);
        timeout_ms = (tv > INT_MAX) ? INT_MAX : (int)tv;
    }

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
    if (n_parts < 2) return ray_error("domain", ".ipc.open expects \"host:port[:user:password]\", missing port");

    char host[256];
    if (part_lens[0] >= sizeof(host)) return ray_error("domain", ".ipc.open host too long, got %lld bytes", (long long)part_lens[0]);
    memcpy(host, parts[0], part_lens[0]);
    host[part_lens[0]] = '\0';

    char port_str[8];
    if (part_lens[1] >= sizeof(port_str)) return ray_error("domain", ".ipc.open port field too long, got %lld bytes", (long long)part_lens[1]);
    memcpy(port_str, parts[1], part_lens[1]);
    port_str[part_lens[1]] = '\0';
    int port = atoi(port_str);
    if (port <= 0 || port > 65535) return ray_error("domain", ".ipc.open port must be in 1..65535, got %d", port);

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

    int64_t h = ray_ipc_connect(host, (uint16_t)port, us_ptr, pw_ptr, timeout_ms);
    if (h == -2) return ray_error("access", "server requires authentication");
    if (h == -3) return ray_error("access", "authentication failed");
    if (h == -5) return ray_error("io", "connection timed out: %s:%d", host, port);
    if (h < 0) return ray_error("io", "connection refused: %s:%d", host, port);

    return make_i64(h);
}

/* (hclose handle) → null */
ray_t* ray_hclose_fn(ray_t* x) {
    if (!ray_is_atom(x) || (x->type != -RAY_I64 && x->type != -RAY_I32))
        return ray_error("type", ".ipc.close expects an i64 or i32 handle, got %s", ray_type_name(x->type));
    int64_t h = (x->type == -RAY_I64) ? x->i64 : x->i32;
    ray_ipc_close(h);
    return RAY_NULL_OBJ;
}

/* (hsend handle msg) → result */
ray_t* ray_hsend_fn(ray_t* handle, ray_t* msg) {
    if (!ray_is_atom(handle) || (handle->type != -RAY_I64 && handle->type != -RAY_I32))
        return ray_error("type", ".ipc.send expects an i64 or i32 handle, got %s", ray_type_name(handle->type));
    int64_t h = (handle->type == -RAY_I64) ? handle->i64 : handle->i32;
    /* Validate message is serializable (reject builtins, etc.) */
    if (ray_serde_size(msg) <= 0)
        return ray_error("type", "message not serializable");
    return ray_ipc_send(h, msg);
}

/* (.ipc.post handle msg) → null on local send, error on failure.
 * Async fire-and-forget counterpart to (.ipc.send ...): the server runs
 * the message through `.ipc.on.async` (or default eval) and sends NO
 * response, so only a LOCAL send failure is observable here. */
ray_t* ray_hpost_fn(ray_t* handle, ray_t* msg) {
    if (!ray_is_atom(handle) || (handle->type != -RAY_I64 && handle->type != -RAY_I32))
        return ray_error("type", ".ipc.post expects an i64 or i32 handle, got %s", ray_type_name(handle->type));
    int64_t h = (handle->type == -RAY_I64) ? handle->i64 : handle->i32;
    /* Validate message is serializable (reject builtins, etc.) */
    if (ray_serde_size(msg) <= 0)
        return ray_error("type", "message not serializable");
    ray_err_t rc = ray_ipc_send_async(h, msg);
    if (rc != RAY_OK)
        return ray_error("io", "ipc async send failed");
    return RAY_NULL_OBJ;
}

/* (.ipc.handle) → i64 current connection handle inside any `.ipc.on.*`
 * hook, or -1 outside any hook.  Registered variadic so both
 * `(.ipc.handle)` (no args) and `(.ipc.handle 0)` (one arg, ignored)
 * work — matches the convention of `.sys.gc` / `.sys.info`. */
ray_t* ray_ipc_handle_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;
    return make_i64(ray_ipc_current_handle());
}
