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

/**   I/O builtins, type casting, and misc builtins extracted from eval.c.
 */

#include "lang/eval.h"
#include "lang/internal.h"
#include "lang/env.h"
#include "vec/vec.h"
#include "lang/nfo.h"
#include "lang/parse.h"
#include "core/pool.h"
#include "core/types.h"
#include "io/csv.h"
#include "ops/ops.h"
#include "table/sym.h"
#include "core/profile.h"
#include "mem/sys.h"
#include "lang/format.h"

#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#if !defined(RAY_OS_WINDOWS)
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* ══════════════════════════════════════════
 * I/O builtins: println, show, format, read-csv, write-csv, as, type
 * ══════════════════════════════════════════ */

/* Helper: return the null literal string for a typed null atom (e.g. "0Ni" for I32). */
static const char* null_literal_str(int8_t type) {
    switch (-type) {
        case RAY_I16:       return "0Nh";
        case RAY_I32:       return "0Ni";
        case RAY_I64:       return "0Nl";
        case RAY_F32:       return "0Ne";
        case RAY_F64:       return "0Nf";
        case RAY_DATE:      return "0Nd";
        case RAY_TIME:      return "0Nt";
        case RAY_TIMESTAMP: return "0Np";
        case RAY_SYM:       return "0Ns";
        default:            return "null";
    }
}

/* Helper: print a ray_t value to a file handle */
void ray_lang_print(FILE* fp, ray_t* val) {
    if (!val || RAY_IS_ERR(val)) { fprintf(fp, "error"); return; }
    if (RAY_IS_NULL(val)) { fprintf(fp, "null"); return; }
    /* Materialize lazy handles before printing */
    if (ray_is_lazy(val))
        val = ray_lazy_materialize(val);
    if (!val || RAY_IS_ERR(val)) { fprintf(fp, "error"); return; }
    if (RAY_ATOM_IS_NULL(val)) {
        fprintf(fp, "%s", null_literal_str(val->type));
        return;
    }
    switch (val->type) {
    case -RAY_I64:  fprintf(fp, "%ld", (long)val->i64); break;
    case -RAY_F64: {
        double fv = val->f64;
        if (fv == 0.0 && signbit(fv)) fv = 0.0;
        fprintf(fp, "%g", fv);
        break;
    }
    case -RAY_BOOL: fprintf(fp, "%s", val->b8 ? "true" : "false"); break;
    case -RAY_SYM: {
        ray_t* s = ray_sym_str(val->i64);
        if (s) fprintf(fp, "'%.*s", (int)ray_str_len(s), ray_str_ptr(s));
        else fprintf(fp, "'?");
        break;
    }
    case -RAY_STR: {
        const char* s = ray_str_ptr(val);
        size_t slen = ray_str_len(val);
        fprintf(fp, "%.*s", (int)slen, s);
        break;
    }
    case RAY_LIST: {
        fprintf(fp, "[");
        int64_t len = ray_len(val);
        ray_t** elems = (ray_t**)ray_data(val);
        for (int64_t i = 0; i < len; i++) {
            if (i > 0) fprintf(fp, " ");
            ray_lang_print(fp, elems[i]);
        }
        fprintf(fp, "]");
        break;
    }
    case RAY_TABLE:
        fprintf(fp, "<table %ldx%ld>",
                (long)ray_table_nrows(val), (long)ray_table_ncols(val));
        break;
    case RAY_UNARY: case RAY_BINARY: case RAY_VARY: {
        const char* name = ray_fn_name(val);
        fprintf(fp, "%s", name[0] ? name : "builtin");
        break;
    }
    default: {
        /* Fall back to ray_fmt for everything else: i16, i32, u8, all
         * vector types (I16/I32/F64/SYM/...), DICT, GUID, temporal, etc.
         * Without this println on (println 5i) printed "<type:-4>" — a
         * debug placeholder, not the value. */
        ray_t* s = ray_fmt(val, 0);
        if (s && !RAY_IS_ERR(s)) {
            fprintf(fp, "%.*s", (int)ray_str_len(s), ray_str_ptr(s));
            ray_release(s);
        } else {
            fprintf(fp, "<type:%d>", val->type);
            if (s) ray_release(s);
        }
        break;
    }
    }
}

/* Helper: format string with % placeholders, substituting args.
 * Returns a heap-allocated char* (caller must ray_sys_free) and sets *out_len.
 * If fmt has no %, returns NULL (caller falls back to plain print). */
static char* fmt_interpolate(const char* fmt, size_t flen, ray_t** args, int64_t nargs, int64_t arg_start, size_t* out_len) {
    /* Quick scan: any % in fmt? */
    int has_pct = 0;
    for (size_t i = 0; i < flen; i++) if (fmt[i] == '%') { has_pct = 1; break; }
    if (!has_pct) return NULL;

    /* Build result in a dynamic buffer */
    size_t cap = flen + 256;
    char* buf = ray_sys_alloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;
    int64_t ai = arg_start;

    for (size_t i = 0; i < flen; i++) {
        if (fmt[i] == '%' && ai < nargs) {
            /* Format the arg into a temp buffer */
            char tmp[256];
            ray_t* a = args[ai++];
            if (ray_is_lazy(a)) a = ray_lazy_materialize(a);
            int tlen = 0;
            if (!a || RAY_IS_ERR(a)) {
                tlen = snprintf(tmp, sizeof(tmp), "error");
            } else if (RAY_ATOM_IS_NULL(a)) {
                tlen = snprintf(tmp, sizeof(tmp), "%s", null_literal_str(a->type));
            } else if (a->type == -RAY_I64) {
                tlen = snprintf(tmp, sizeof(tmp), "%ld", (long)a->i64);
            } else if (a->type == -RAY_F64) {
                double fv = a->f64;
                if (fv == 0.0 && signbit(fv)) fv = 0.0;
                tlen = snprintf(tmp, sizeof(tmp), "%g", fv);
            } else if (a->type == -RAY_BOOL) {
                tlen = snprintf(tmp, sizeof(tmp), "%s", a->b8 ? "true" : "false");
            } else if (a->type == -RAY_STR) {
                const char* sp = ray_str_ptr(a);
                size_t sl = ray_str_len(a);
                while (pos + sl + 1 > cap) { cap *= 2; buf = ray_sys_realloc(buf, cap); }
                memcpy(buf + pos, sp, sl);
                pos += sl;
                continue;
            } else if (a->type == -RAY_SYM) {
                ray_t* ss = ray_sym_str(a->i64);
                if (ss) {
                    const char* sp = ray_str_ptr(ss);
                    size_t sl = ray_str_len(ss);
                    while (pos + sl + 1 > cap) { cap *= 2; buf = ray_sys_realloc(buf, cap); }
                    memcpy(buf + pos, sp, sl);
                    pos += sl;
                    ray_release(ss);
                    continue;
                }
                tlen = snprintf(tmp, sizeof(tmp), "'?");
            } else {
                /* Fall back to ray_fmt */
                ray_t* formatted = ray_fmt(a, 0);
                if (formatted && !RAY_IS_ERR(formatted)) {
                    const char* sp = ray_str_ptr(formatted);
                    size_t sl = ray_str_len(formatted);
                    while (pos + sl + 1 > cap) { cap *= 2; buf = ray_sys_realloc(buf, cap); }
                    memcpy(buf + pos, sp, sl);
                    pos += sl;
                    ray_release(formatted);
                    continue;
                }
                if (formatted) ray_release(formatted);
                tlen = snprintf(tmp, sizeof(tmp), "<type:%d>", a->type);
            }
            while (pos + (size_t)tlen + 1 > cap) { cap *= 2; buf = ray_sys_realloc(buf, cap); }
            memcpy(buf + pos, tmp, (size_t)tlen);
            pos += (size_t)tlen;
        } else {
            if (pos + 2 > cap) { cap *= 2; buf = ray_sys_realloc(buf, cap); }
            buf[pos++] = fmt[i];
        }
    }
    buf[pos] = '\0';
    *out_len = pos;
    return buf;
}

/* (println val1 val2 ...) — print values to stdout, newline at end.
 * If first arg is a string with % placeholders, substitutes remaining args. */
ray_t* ray_println_fn(ray_t** args, int64_t n) {
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    /* Format string mode: first arg is a string with % placeholders */
    if (n >= 2 && args[0] && args[0]->type == -RAY_STR) {
        const char* fmt = ray_str_ptr(args[0]);
        size_t flen = ray_str_len(args[0]);
        size_t out_len = 0;
        char* result = fmt_interpolate(fmt, flen, args, n, 1, &out_len);
        if (result) {
            fwrite(result, 1, out_len, stdout);
            fputc('\n', stdout);
            fflush(stdout);
            ray_sys_free(result);
            return RAY_NULL_OBJ;
        }
    }

    for (int64_t i = 0; i < n; i++) {
        if (i > 0) fputc(' ', stdout);
        ray_lang_print(stdout, args[i]);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return RAY_NULL_OBJ;
}

/* (print val1 val2 ...) — like println but without trailing newline */
ray_t* ray_print_fn(ray_t** args, int64_t n) {
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    /* Format string mode: first arg is a string with % placeholders */
    if (n >= 2 && args[0] && args[0]->type == -RAY_STR) {
        const char* fmt = ray_str_ptr(args[0]);
        size_t flen = ray_str_len(args[0]);
        size_t out_len = 0;
        char* result = fmt_interpolate(fmt, flen, args, n, 1, &out_len);
        if (result) {
            fwrite(result, 1, out_len, stdout);
            fflush(stdout);
            ray_sys_free(result);
            return RAY_NULL_OBJ;
        }
    }

    for (int64_t i = 0; i < n; i++) {
        if (i > 0) fputc(' ', stdout);
        ray_lang_print(stdout, args[i]);
    }
    fflush(stdout);
    return RAY_NULL_OBJ;
}

/* (show val1 val2 ...) — print values to stdout using ray_fmt, newline at end */
ray_t* ray_show_fn(ray_t** args, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);
        if (!args[i] || RAY_IS_ERR(args[i])) { fprintf(stdout, "error"); continue; }
        ray_t* formatted = ray_fmt(args[i], 1);
        if (formatted && !RAY_IS_ERR(formatted)) {
            const char* sp = ray_str_ptr(formatted);
            size_t sl = ray_str_len(formatted);
            fwrite(sp, 1, sl, stdout);
            ray_release(formatted);
        } else {
            if (formatted) ray_release(formatted);
            ray_lang_print(stdout, args[i]);
        }
    }
    fputc('\n', stdout);
    fflush(stdout);
    return RAY_NULL_OBJ;
}

/* (format "hello % world %" a b) — string formatting with % placeholders */
ray_t* ray_format_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);
    if (!args[0] || args[0]->type != -RAY_STR) return ray_error("type", NULL);
    const char* fmt = ray_str_ptr(args[0]);
    size_t flen = ray_str_len(args[0]);
    size_t out_len = 0;
    char* result = fmt_interpolate(fmt, flen, args, n, 1, &out_len);
    if (result) {
        ray_t* s = ray_str(result, out_len);
        ray_sys_free(result);
        return s;
    }
    /* No placeholders: return fmt as-is */
    ray_retain(args[0]);
    return args[0];
}

/* (resolve 'name) — check if name exists in env, return value or null.
 * SPECIAL_FORM: does not evaluate args. */
/* (resolve tbl) — replace I64 columns with SYM columns where values are valid sym IDs.
 * This makes query results human-readable (sym names instead of intern IDs).
 * Also accepts (resolve db tbl) for compat — just ignores db. */
ray_t* ray_resolve_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("arity", "resolve expects at least 1 argument");

    /* Evaluate all args */
    ray_t* tbl = NULL;
    if (n == 1) {
        tbl = ray_eval(args[0]);
    } else {
        /* (resolve db tbl) — ignore db, use tbl */
        ray_t* db = ray_eval(args[0]);
        if (db && !RAY_IS_ERR(db)) ray_release(db);
        tbl = ray_eval(args[1]);
    }
    if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : ray_error("type", "resolve: null argument");

    /* Materialize lazy tables */
    if (ray_is_lazy(tbl)) {
        ray_t* mat = ray_lazy_materialize(tbl);
        ray_release(tbl);
        if (!mat || RAY_IS_ERR(mat)) return mat ? mat : ray_error("domain", "resolve: materialization failed");
        tbl = mat;
    }

    /* If not a table, return as-is */
    if (tbl->type != RAY_TABLE) {
        if (tbl->type == -RAY_SYM) {
            ray_t* val = ray_env_get(tbl->i64);
            ray_release(tbl);
            if (!val) return NULL;
            ray_retain(val);
            return val;
        }
        return tbl;
    }

    int64_t ncols = ray_table_ncols(tbl);
    int64_t nrows = ray_table_nrows(tbl);

    /* Build a new table replacing I64 columns with SYM columns where possible */
    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }

    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        int64_t col_name = ray_table_col_name(tbl, c);
        if (!col) continue;

        if (col->type == RAY_I64) {
            /* Try to resolve: convert to SYM only if ALL positive values
             * are valid sym IDs. This avoids converting entity-ID columns
             * where values are plain integers that happen to collide with
             * low sym IDs. */
            int64_t* data = (int64_t*)ray_data(col);
            bool all_user_sym = (nrows > 0);
            /* Only convert if ALL values resolve to user-defined symbols
             * (length >= 2, not single-char operators). This distinguishes
             * symbol references (name='Alice') from entity IDs (e=1). */
            for (int64_t r = 0; r < nrows; r++) {
                if (data[r] <= 0) { all_user_sym = false; break; }
                ray_t* sn = ray_sym_str(data[r]);
                if (!sn) { all_user_sym = false; break; }
                size_t slen = ray_str_len(sn);
                const char* sp = ray_str_ptr(sn);
                /* Single-char or starts with digit/operator -> not a user symbol */
                if (slen < 2 || (sp[0] >= '0' && sp[0] <= '9') ||
                    sp[0] == '+' || sp[0] == '-' || sp[0] == '*' || sp[0] == '/' ||
                    sp[0] == '<' || sp[0] == '>' || sp[0] == '=' || sp[0] == '!' ||
                    sp[0] == '?' || sp[0] == '_') {
                    all_user_sym = false; break;
                }
            }
            if (all_user_sym) {
                /* Convert to SYM column */
                ray_t* sym_col = ray_vec_new(RAY_SYM, nrows);
                if (RAY_IS_ERR(sym_col)) { ray_release(result); ray_release(tbl); return sym_col; }
                for (int64_t r = 0; r < nrows; r++) {
                    sym_col = ray_vec_append(sym_col, &data[r]);
                    if (RAY_IS_ERR(sym_col)) { ray_release(result); ray_release(tbl); return sym_col; }
                }
                result = ray_table_add_col(result, col_name, sym_col);
                ray_release(sym_col);
            } else {
                /* Keep as I64 */
                result = ray_table_add_col(result, col_name, col);
            }
        } else {
            /* Non-I64 column: keep as-is */
            result = ray_table_add_col(result, col_name, col);
        }
        if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }
    }

    ray_release(tbl);
    return result;
}

/* (timeit expr) — evaluate expression and return time in ms as F64.
 * SPECIAL_FORM: does not pre-evaluate args. */
ray_t* ray_timeit_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);
    int64_t t0 = ray_profile_now_ns();
    ray_t* result = ray_eval(args[0]);
    int64_t t1 = ray_profile_now_ns();
    if (result && !RAY_IS_ERR(result)) ray_release(result);
    double ms = (double)(t1 - t0) / 1e6;
    return make_f64(ms);
}

/* (exit code) — exit the process */
ray_t* ray_exit_fn(ray_t* arg) {
    int code = 0;
    if (arg && is_numeric(arg)) code = (int)as_i64(arg);
    exit(code);
    return NULL; /* unreachable */
}

/* (read-csv path) — read CSV file, return RAY_TABLE */
/* Helper: resolve a type name symbol to a ray type code */
static int8_t resolve_type_name(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return -1;
    const char* name = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    int8_t result = -1;
    if (len == 3 && memcmp(name, "I64", 3) == 0) result = RAY_I64;
    else if (len == 3 && memcmp(name, "I32", 3) == 0) result = RAY_I32;
    else if (len == 3 && memcmp(name, "I16", 3) == 0) result = RAY_I16;
    else if (len == 3 && memcmp(name, "F64", 3) == 0) result = RAY_F64;
    else if (len == 2 && memcmp(name, "B8", 2) == 0) result = RAY_BOOL;
    else if (len == 2 && memcmp(name, "U8", 2) == 0) result = RAY_U8;
    else if (len == 6 && memcmp(name, "SYMBOL", 6) == 0) result = RAY_SYM;
    else if (len == 3 && memcmp(name, "STR", 3) == 0) result = RAY_STR;
    else if (len == 3 && memcmp(name, "F32", 3) == 0) result = RAY_F32;
    else if (len == 4 && memcmp(name, "DATE", 4) == 0) result = RAY_DATE;
    else if (len == 4 && memcmp(name, "TIME", 4) == 0) result = RAY_TIME;
    else if (len == 9 && memcmp(name, "TIMESTAMP", 9) == 0) result = RAY_TIMESTAMP;
    else if (len == 4 && memcmp(name, "GUID", 4) == 0) result = RAY_GUID;
    ray_release(s);
    return result;
}

ray_t* ray_read_csv_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);

    /* (read-csv [types] "path") or (read-csv "path") */
    ray_t* path_obj = NULL;
    ray_t* schema = NULL;
    if (n >= 2 && ray_is_vec(args[0]) && args[0]->type == RAY_SYM) {
        schema = args[0];
        path_obj = args[1];
    } else {
        path_obj = args[0];
    }

    const char* path = NULL;
    if (path_obj->type == -RAY_STR)
        path = ray_str_ptr(path_obj);
    else
        return ray_error("type", NULL);
    if (!path) return ray_error("domain", NULL);

    if (schema) {
        int64_t ncols = schema->len;
        int8_t col_types[256];
        if (ncols > 256) return ray_error("limit", NULL);
        int64_t* sym_ids = (int64_t*)ray_data(schema);
        for (int64_t i = 0; i < ncols; i++) {
            col_types[i] = resolve_type_name(sym_ids[i]);
            if (col_types[i] < 0) return ray_error("type", NULL);
        }
        ray_t* tbl = ray_read_csv_opts(path, 0, true, col_types, (int32_t)ncols);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("io", NULL);
        return tbl;
    }

    ray_t* tbl = ray_read_csv(path);
    if (!tbl || RAY_IS_ERR(tbl)) return ray_error("io", NULL);
    return tbl;
}

/* (write-csv table path) — write table to CSV file */
ray_t* ray_write_csv_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    ray_t* tbl = args[0];
    ray_t* path_obj = args[1];
    if (tbl->type != RAY_TABLE) return ray_error("type", NULL);
    const char* path = NULL;
    if (path_obj->type == -RAY_STR)
        path = ray_str_ptr(path_obj);
    else
        return ray_error("type", NULL);
    if (!path) return ray_error("domain", NULL);
    ray_err_t err = ray_write_csv(tbl, path);
    if (err != RAY_OK) return ray_error(ray_err_code_str(err), NULL);
    return make_i64(0);
}

/* (as 'TypeName value) — type cast */
/* Case-insensitive type name match helper */
static int cast_match(const char* tname, size_t tlen, const char* target) {
    size_t tgt_len = strlen(target);
    if (tlen != tgt_len) return 0;
    for (size_t i = 0; i < tlen; i++) {
        char a = tname[i], b = target[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Helper: copy null bitmap from source vec/list to destination vec. */
static ray_t* cast_vec_copy_nulls(ray_t* vec, ray_t* val) {
    if (ray_is_vec(val)) {
        if (ray_vec_copy_nulls(vec, val) != RAY_OK)
            { ray_release(vec); return ray_error("oom", NULL); }
    } else if (val->type == RAY_LIST) {
        ray_t** le = (ray_t**)ray_data(val);
        for (int64_t j = 0; j < vec->len; j++)
            if (le[j] && RAY_ATOM_IS_NULL(le[j]))
                ray_vec_set_null(vec, j, true);
    }
    return vec;
}

/* Bulk-cast loop over [_lo, _hi).  Reads `R` from `_src_p`, writes `W`
 * to `_dst_p`.  No atom allocations.  The single-threaded path passes
 * the whole [0, n2) range; the parallel worker passes its slice. */
#define CAST_LOOP_RANGE(R, W, EXPR, _lo, _hi) do {                     \
    const R* _src = (const R*)_src_p;                                  \
    W* _dst = (W*)_dst_p;                                              \
    for (int64_t _i = (_lo); _i < (_hi); _i++) {                       \
        R _v = _src[_i];                                               \
        _dst[_i] = (EXPR);                                             \
    }                                                                  \
} while (0)
#define CAST_LOOP(R, W, EXPR) CAST_LOOP_RANGE(R, W, EXPR, 0, n2)

/* Same-byte-rep type relabels (I64↔TIMESTAMP, I32↔DATE↔TIME): the
 * per-element data is identical, so a single memcpy populates the new
 * vector.  Returns true on hit. */
static bool cast_vec_relabel_compat(int8_t a, int8_t b) {
    if (a == b) return true;
    if ((a == RAY_I64 || a == RAY_TIMESTAMP) &&
        (b == RAY_I64 || b == RAY_TIMESTAMP)) return true;
    if ((a == RAY_I32 || a == RAY_DATE || a == RAY_TIME) &&
        (b == RAY_I32 || b == RAY_DATE || b == RAY_TIME)) return true;
    return false;
}

/* Vec→vec numeric cast on raw arrays (no per-element atom allocs).
 * Returns the populated `vec` on success, or NULL if the (in_type,
 * out_type) pair is unsupported here — caller falls back to the generic
 * path.
 *
 * Temporal cross-unit pairs (matched between the per-atom slow path
 * and the fast path):
 *   DATE → TIMESTAMP : days * NS_PER_DAY
 *   TIMESTAMP → DATE : floor-div by NS_PER_DAY (so ns=-1 → -1 day,
 *                       i.e. 1999-12-31, not 2000-01-01).
 *   TIMESTAMP → TIME : floor-mod by NS_PER_DAY then /1_000_000
 *                       (ns→ms within day, always in [0, 86_400_000)).
 * Plain `% / /` would truncate toward zero per C semantics and give
 * wrong components for pre-2000 timestamps; the helpers below give
 * Python-style floor semantics for a positive divisor. */
#define NS_PER_DAY 86400000000000LL

static inline int64_t ts_days_floor(int64_t ns) {
    int64_t q = ns / NS_PER_DAY;
    int64_t r = ns - q * NS_PER_DAY;
    if (r < 0) q -= 1;
    return q;
}
static inline int64_t ts_ns_in_day(int64_t ns) {
    int64_t r = ns % NS_PER_DAY;
    if (r < 0) r += NS_PER_DAY;
    return r;
}

/* Element-wise cast worker: writes _dst_p[lo..hi) from _src_p[lo..hi).
 * Used by both the single-threaded fast path and the parallel dispatch.
 * Returns true on hit; false means caller falls back to the generic
 * (atom) path. */
static bool cast_range_worker(const void* _src_p, void* _dst_p,
                              int64_t lo, int64_t hi,
                              int8_t in_type, int8_t out_type) {
    /* Temporal unit conversions. */
    if (in_type == RAY_DATE && out_type == RAY_TIMESTAMP) {
        CAST_LOOP_RANGE(int32_t, int64_t, (int64_t)_v * NS_PER_DAY, lo, hi);
        return true;
    }
    if (in_type == RAY_TIMESTAMP && out_type == RAY_DATE) {
        /* Floor-div, not truncate-toward-zero: ns=-1 must give -1 day
         * (1999-12-31), not 0 (2000-01-01). */
        CAST_LOOP_RANGE(int64_t, int32_t, (int32_t)ts_days_floor(_v), lo, hi);
        return true;
    }
    if (in_type == RAY_TIMESTAMP && out_type == RAY_TIME) {
        /* Floor-mod ns within day, then ns→ms. */
        CAST_LOOP_RANGE(int64_t, int32_t,
                        (int32_t)(ts_ns_in_day(_v) / 1000000LL), lo, hi);
        return true;
    }
    /* Generic numeric pairs.  The big switch dispatches on (out_type,
     * in_type); each leaf is a tight typed loop the compiler vectorizes. */
#define CL(R, W, EXPR) do { CAST_LOOP_RANGE(R, W, EXPR, lo, hi); return true; } while (0)
    switch (out_type) {
    case RAY_I64: case RAY_TIMESTAMP:
        switch (in_type) {
        case RAY_BOOL:  CL(uint8_t,  int64_t, _v ? 1 : 0);
        case RAY_U8:    CL(uint8_t,  int64_t, (int64_t)_v);
        case RAY_I16:   CL(int16_t,  int64_t, (int64_t)_v);
        case RAY_I32: case RAY_DATE: case RAY_TIME:
                        CL(int32_t,  int64_t, (int64_t)_v);
        case RAY_F64:   CL(double,   int64_t, (int64_t)_v);
        }
        break;
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        switch (in_type) {
        case RAY_BOOL:  CL(uint8_t,  int32_t, _v ? 1 : 0);
        case RAY_U8:    CL(uint8_t,  int32_t, (int32_t)_v);
        case RAY_I16:   CL(int16_t,  int32_t, (int32_t)_v);
        case RAY_I64: case RAY_TIMESTAMP:
                        CL(int64_t,  int32_t, (int32_t)_v);
        case RAY_F64:   CL(double,   int32_t, (int32_t)_v);
        }
        break;
    case RAY_I16:
        switch (in_type) {
        case RAY_BOOL:  CL(uint8_t,  int16_t, _v ? 1 : 0);
        case RAY_U8:    CL(uint8_t,  int16_t, (int16_t)_v);
        case RAY_I32: case RAY_DATE: case RAY_TIME:
                        CL(int32_t,  int16_t, (int16_t)_v);
        case RAY_I64: case RAY_TIMESTAMP:
                        CL(int64_t,  int16_t, (int16_t)_v);
        case RAY_F64:   CL(double,   int16_t, (int16_t)_v);
        }
        break;
    case RAY_U8:
        switch (in_type) {
        case RAY_BOOL:  CL(uint8_t,  uint8_t, _v ? 1 : 0);
        case RAY_I16:   CL(int16_t,  uint8_t, (uint8_t)_v);
        case RAY_I32: case RAY_DATE: case RAY_TIME:
                        CL(int32_t,  uint8_t, (uint8_t)_v);
        case RAY_I64: case RAY_TIMESTAMP:
                        CL(int64_t,  uint8_t, (uint8_t)_v);
        case RAY_F64:   CL(double,   uint8_t, (uint8_t)_v);
        }
        break;
    case RAY_F64:
        switch (in_type) {
        case RAY_BOOL:  CL(uint8_t,  double, _v ? 1.0 : 0.0);
        case RAY_U8:    CL(uint8_t,  double, (double)_v);
        case RAY_I16:   CL(int16_t,  double, (double)_v);
        case RAY_I32: case RAY_DATE: case RAY_TIME:
                        CL(int32_t,  double, (double)_v);
        case RAY_I64: case RAY_TIMESTAMP:
                        CL(int64_t,  double, (double)_v);
        }
        break;
    case RAY_BOOL:
        switch (in_type) {
        case RAY_U8:    CL(uint8_t,  uint8_t, _v != 0 ? 1 : 0);
        case RAY_I16:   CL(int16_t,  uint8_t, _v != 0 ? 1 : 0);
        case RAY_I32: case RAY_DATE: case RAY_TIME:
                        CL(int32_t,  uint8_t, _v != 0 ? 1 : 0);
        case RAY_I64: case RAY_TIMESTAMP:
                        CL(int64_t,  uint8_t, _v != 0 ? 1 : 0);
        case RAY_F64:   CL(double,   uint8_t, _v != 0.0 ? 1 : 0);
        }
        break;
    }
#undef CL
    return false;
}

typedef struct {
    const void* src;
    void*       dst;
    int8_t      in_type;
    int8_t      out_type;
} cast_par_ctx_t;

static void cast_par_fn(void* arg, uint32_t worker_id, int64_t lo, int64_t hi) {
    (void)worker_id;
    /* Honor SIGINT (ray_request_interrupt / ray_interrupted) per task —
     * the pool's own per-task gate checks `pool->cancelled` only, so
     * a Ctrl-C arriving during dispatch wouldn't otherwise short-
     * circuit the workers.  Skip the task on interrupt; the caller
     * post-checks via CANCELLED() and returns an error. */
    if (ray_interrupted()) return;
    cast_par_ctx_t* ctx = (cast_par_ctx_t*)arg;
    cast_range_worker(ctx->src, ctx->dst, lo, hi, ctx->in_type, ctx->out_type);
}

/* Threshold below which the dispatch overhead outweighs the speedup.
 * Memory-bound conversions saturate ~3 GB/s single-thread; with 8
 * workers we approach DRAM peak (~25 GB/s).  Below ~256 K elements the
 * 50 µs dispatch cost dominates. */
#define CAST_PAR_MIN_ELEMS 262144

static ray_t* cast_vec_numeric_fast(ray_t* val, ray_t* vec, int8_t out_type) {
    int8_t in_type = val->type;
    int64_t n2 = val->len;
    ray_pool_t* pool = ray_pool_get();

/* A cast is "cancelled" if EITHER:
 *   (a) the pool's per-query cancel flag is set (e.g. via ray_cancel
 *       from another thread or a long-query timeout), or
 *   (b) the eval-loop interrupt flag is set (Ctrl-C / SIGINT, signalled
 *       by ray_request_interrupt and observed via ray_interrupted).
 * Both must be polled — they're independent signals and either one
 * means the user wants the operation to abort. */
#define CANCELLED() ((pool && atomic_load_explicit(&pool->cancelled,   \
                                                   memory_order_acquire)) \
                     || ray_interrupted())
#define CHECK_CANCEL_OR(retval) do {                                   \
    if (CANCELLED()) return ray_error("cancel", NULL);                 \
    return (retval);                                                   \
} while (0)

    /* Function-entry cancel check — gates ALL paths below (relabel,
     * parallel, and chunked single-thread).  Without this, a cancel
     * pending at entry would still execute the first ~50 µs of any
     * path before being observed. */
    if (CANCELLED()) return ray_error("cancel", NULL);

    /* Same byte-rep types: chunked memcpy.  A single
     * memcpy(_, _, n*esz) on a 10M-element TIMESTAMP relabel is ~80 MB
     * and ~10 ms of opaque work — cancel arriving during it can't
     * interrupt the libc copy, so we'd happily return `vec` even if
     * the user asked to abort.  Break the copy into ~1 MB chunks and
     * poll cancel between them; max in-flight work between checks is
     * one chunk (~100 µs at realistic bandwidth). */
    if (cast_vec_relabel_compat(in_type, out_type)) {
        size_t esz = (size_t)ray_elem_size(out_type);
        if (n2 > 0 && esz > 0) {
            const char* sp = (const char*)ray_data(val);
            char* dp = (char*)ray_data(vec);
            size_t total = (size_t)n2 * esz;
            const size_t chunk_bytes = (size_t)1 << 20;  /* 1 MiB */
            size_t off = 0;
            while (off < total) {
                if (CANCELLED()) return ray_error("cancel", NULL);
                size_t cn = total - off;
                if (cn > chunk_bytes) cn = chunk_bytes;
                memcpy(dp + off, sp + off, cn);
                off += cn;
            }
        }
        /* Post-check: a cancel landing in the final chunk would have
         * been missed by the in-loop check (we copy then exit). */
        if (CANCELLED()) return ray_error("cancel", NULL);
        return vec;
    }

    const void* src_p = ray_data(val);
    void* dst_p = ray_data(vec);

    /* Three return states from this point on (helper does NOT touch
     * `vec`'s reference count):
     *
     *   - `vec`           : success, fully populated, no cancel observed
     *   - error pointer   : cancellation observed at any point — the
     *                       helper bails out as soon as it notices,
     *                       even mid-loop in the single-thread path
     *   - NULL            : (in_type, out_type) pair unsupported here
     *                       AND no cancellation observed — caller may
     *                       safely fall through to the per-atom slow
     *                       path with `vec` still valid */

    if (pool && n2 >= CAST_PAR_MIN_ELEMS && ray_pool_total_workers(pool) >= 2) {
        cast_par_ctx_t pctx = { .src = src_p, .dst = dst_p,
                                .in_type = in_type, .out_type = out_type };
        /* Probe the worker on a single element to verify the pair is
         * supported here.  If unsupported, fall through (NULL) — but
         * still re-check cancel first so a cancel raced into the probe
         * window is not swallowed. */
        if (n2 > 0 && cast_range_worker(src_p, dst_p, 0, 1, in_type, out_type)) {
            ray_pool_dispatch(pool, cast_par_fn, &pctx, n2);
            if (CANCELLED()) return ray_error("cancel", NULL);
            return vec;
        }
        CHECK_CANCEL_OR(NULL);
    }

    /* Chunked single-thread path.  Tight typed loops vectorize well
     * but block cancellation for the whole `n2` range — chunk into
     * cache-sized pieces so cancel is honored within ~one chunk
     * (64K elements ≈ 50 µs at realistic bandwidth). */
    if (n2 == 0)
        CHECK_CANCEL_OR(vec);
    /* Re-check cancel right before the first chunk runs (entry cancel
     * check above is over the whole helper, but if a cancel raced in
     * between the relabel path and here we want to bail before doing
     * any work). */
    if (CANCELLED()) return ray_error("cancel", NULL);
    int64_t chunk = (int64_t)65536;
    int64_t lo = 0;
    int64_t hi = (n2 < chunk) ? n2 : chunk;
    /* Probe the first chunk; if it fails, the (in, out) pair is
     * unsupported here and the caller falls through. */
    if (!cast_range_worker(src_p, dst_p, lo, hi, in_type, out_type))
        CHECK_CANCEL_OR(NULL);
    lo = hi;
    while (lo < n2) {
        if (CANCELLED()) return ray_error("cancel", NULL);
        hi = lo + chunk;
        if (hi > n2) hi = n2;
        cast_range_worker(src_p, dst_p, lo, hi, in_type, out_type);
        lo = hi;
    }
    CHECK_CANCEL_OR(vec);
#undef CHECK_CANCEL_OR
#undef CANCELLED
}

/* Helper: cast a vector/list to a numeric/temporal/bool type.
 * Handles I64, I32, I16, U8, F64, BOOL, DATE, TIME, TIMESTAMP, SYM.
 * Fast path for typed numeric input vectors (no per-element atoms);
 * generic path for RAY_LIST and other shapes. */
static ray_t* cast_vec_numeric(ray_t* type_sym, ray_t* val, int8_t out_type) {
    int64_t n2 = val->len;
    ray_t* vec = ray_vec_new(out_type, n2);
    if (RAY_IS_ERR(vec)) return vec;
    vec->len = n2;

    /* Fast path: typed numeric vec → numeric vec, no list/string. */
    if (ray_is_vec(val) && val->type != RAY_STR && val->type != RAY_SYM &&
        val->type != RAY_GUID && out_type != RAY_SYM) {
        ray_t* fast = cast_vec_numeric_fast(val, vec, out_type);
        /* Three return states (helper does NOT release `vec`):
         *   - vec on success
         *   - error pointer on cancellation — caller releases `vec`
         *   - NULL on unsupported (in_type, out_type) — fall through */
        if (RAY_IS_ERR(fast)) { ray_release(vec); return fast; }
        if (fast != NULL) {
            /* Close the cancellation gap that surrounds the post-cast
             * nullmap copy.  cast_vec_copy_nulls runs after the
             * cancel-aware fast cast — for nullable inputs it does a
             * bitmap copy (and a per-element scan on RAY_LIST inputs
             * of length n2).  A cancel arriving in that window would
             * otherwise be masked by the success return.  Pre-check
             * gates the nullmap work; post-check catches a cancel
             * landing during it. */
            ray_pool_t* fp = ray_pool_get();
#define _FP_CANCELLED() ((fp && atomic_load_explicit(&fp->cancelled, \
                                                     memory_order_acquire)) \
                         || ray_interrupted())
            if (_FP_CANCELLED()) { ray_release(vec); return ray_error("cancel", NULL); }
            ray_t* result = cast_vec_copy_nulls(vec, val);
            if (RAY_IS_ERR(result)) return result;
            if (_FP_CANCELLED()) { ray_release(vec); return ray_error("cancel", NULL); }
#undef _FP_CANCELLED
            return vec;
        }
    }

    /* Fast path: STR vec → SYM vec.  Direct intern from each element's
     * (ptr, len), no atom alloc or recursive cast.  ray_sym_intern uses
     * the table's coarse lock so this stays single-threaded — but it
     * skips ~150 ns of overhead per row. */
    if (out_type == RAY_SYM && ray_is_vec(val) && val->type == RAY_STR) {
        int64_t* ids = (int64_t*)ray_data(vec);
        for (int64_t i = 0; i < n2; i++) {
            size_t slen = 0;
            const char* sp = ray_str_vec_get(val, i, &slen);
            int64_t id = ray_sym_intern(sp ? sp : "", sp ? slen : 0);
            if (id < 0) { ray_release(vec); return ray_error("oom", NULL); }
            ids[i] = id;
        }
        ray_t* result = cast_vec_copy_nulls(vec, val);
        if (RAY_IS_ERR(result)) return result;
        return vec;
    }

    void* out = ray_data(vec);
    for (int64_t i = 0; i < n2; i++) {
        int alloc = 0;
        ray_t* elem = collection_elem(val, i, &alloc);
        if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
        ray_t* cast = ray_cast_fn(type_sym, elem);
        if (alloc) ray_release(elem);
        if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
        switch (out_type) {
        case RAY_I64: case RAY_TIMESTAMP: case RAY_SYM:
            ((int64_t*)out)[i] = cast->i64; break;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            ((int32_t*)out)[i] = cast->i32; break;
        case RAY_I16:  ((int16_t*)out)[i] = cast->i16; break;
        case RAY_U8:   ((uint8_t*)out)[i] = cast->u8;  break;
        case RAY_F64:  ((double*)out)[i]  = cast->f64;  break;
        case RAY_BOOL: ((bool*)out)[i]    = cast->b8;   break;
        default: break;
        }
        ray_release(cast);
    }
    ray_t* result = cast_vec_copy_nulls(vec, val);
    if (RAY_IS_ERR(result)) return result;
    return vec;
}

ray_t* ray_cast_fn(ray_t* type_sym, ray_t* val) {
    if (type_sym->type != -RAY_SYM) return ray_error("type", NULL);
    /* Null propagation: casting a typed null atom produces a typed null of target type */
    if (ray_is_atom(val) && RAY_ATOM_IS_NULL(val)) {
        ray_t* s2 = ray_sym_str(type_sym->i64);
        if (!s2) return ray_error("domain", NULL);
        const char* tn = ray_str_ptr(s2);
        size_t tl = ray_str_len(s2);
        int8_t tt = 0;
        if (cast_match(tn, tl, "I64") || cast_match(tn, tl, "i64")) tt = -RAY_I64;
        else if (cast_match(tn, tl, "I32") || cast_match(tn, tl, "i32")) tt = -RAY_I32;
        else if (cast_match(tn, tl, "I16") || cast_match(tn, tl, "i16")) tt = -RAY_I16;
        else if (cast_match(tn, tl, "U8") || cast_match(tn, tl, "u8")) tt = -RAY_U8;
        else if (cast_match(tn, tl, "F64") || cast_match(tn, tl, "f64")) tt = -RAY_F64;
        else if (cast_match(tn, tl, "BOOL") || cast_match(tn, tl, "bool") || cast_match(tn, tl, "B8") || cast_match(tn, tl, "b8")) tt = -RAY_BOOL;
        else if (cast_match(tn, tl, "SYMBOL") || cast_match(tn, tl, "symbol") || cast_match(tn, tl, "sym")) tt = -RAY_SYM;
        else if (cast_match(tn, tl, "DATE") || cast_match(tn, tl, "date")) tt = -RAY_DATE;
        else if (cast_match(tn, tl, "TIME") || cast_match(tn, tl, "time")) tt = -RAY_TIME;
        else if (cast_match(tn, tl, "TIMESTAMP") || cast_match(tn, tl, "timestamp")) tt = -RAY_TIMESTAMP;
        else if (cast_match(tn, tl, "GUID") || cast_match(tn, tl, "guid")) tt = -RAY_GUID;
        else if (cast_match(tn, tl, "STR") || cast_match(tn, tl, "str")) { ray_release(s2); return ray_str("", 0); }
        ray_release(s2);
        if (tt) return ray_typed_null(tt);
        return ray_error("domain", NULL);
    }
    ray_t* s = ray_sym_str(type_sym->i64);
    if (!s) return ray_error("domain", NULL);
    const char* tname = ray_str_ptr(s);
    size_t tlen = ray_str_len(s);

    /* Cast to I64 / i64 */
    if (cast_match(tname, tlen, "I64") || cast_match(tname, tlen, "i64")) {
        ray_release(s);
        if (val->type == -RAY_I64) { ray_retain(val); return val; }
        if (val->type == -RAY_F64) return make_i64((int64_t)val->f64);
        if (val->type == -RAY_BOOL) return make_i64(val->b8 ? 1 : 0);
        if (val->type == -RAY_I32 || val->type == -RAY_DATE || val->type == -RAY_TIME)
            return make_i64(val->i32);
        if (val->type == -RAY_TIMESTAMP) return make_i64(val->i64);
        if (val->type == -RAY_I16) return make_i64(val->i16);
        if (val->type == -RAY_U8) return make_i64(val->u8);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            if (!sp) return ray_error("domain", NULL);
            char* end;
            int64_t v = strtoll(sp, &end, 10);
            if (end == sp) return ray_error("domain", NULL);
            return make_i64(v);
        }
        /* Vector/list cast */
        if (ray_is_vec(val) || val->type == RAY_LIST)
            return cast_vec_numeric(type_sym, val, RAY_I64);
        return ray_error("type", NULL);
    }
    /* Cast to I32 / i32 */
    if (cast_match(tname, tlen, "I32") || cast_match(tname, tlen, "i32")) {
        ray_release(s);
        if (val->type == -RAY_I32) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_i32(val->b8 ? 1 : 0);
        if (val->type == -RAY_U8)  return ray_i32((int32_t)val->u8);
        if (val->type == -RAY_I16) return ray_i32(val->i16);
        if (val->type == -RAY_I64) return ray_i32((int32_t)val->i64);
        if (val->type == -RAY_F64) return ray_i32((int32_t)val->f64);
        if (val->type == -RAY_DATE || val->type == -RAY_TIME) return ray_i32(val->i32);
        if (val->type == -RAY_TIMESTAMP) return ray_i32((int32_t)val->i64);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val); char* end;
            long v = strtol(sp, &end, 10);
            if (end == sp) return ray_error("domain", NULL);
            return ray_i32((int32_t)v);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST)
            return cast_vec_numeric(type_sym, val, RAY_I32);
        return ray_error("type", NULL);
    }
    /* Cast to I16 / i16 */
    if (cast_match(tname, tlen, "I16") || cast_match(tname, tlen, "i16")) {
        ray_release(s);
        if (val->type == -RAY_I16) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_i16(val->b8 ? 1 : 0);
        if (val->type == -RAY_U8)  return ray_i16((int16_t)val->u8);
        if (val->type == -RAY_I32) return ray_i16((int16_t)val->i32);
        if (val->type == -RAY_I64) return ray_i16((int16_t)val->i64);
        if (val->type == -RAY_F64) return ray_i16((int16_t)val->f64);
        if (val->type == -RAY_DATE || val->type == -RAY_TIME) return ray_i16((int16_t)val->i32);
        if (val->type == -RAY_TIMESTAMP) return ray_i16((int16_t)val->i64);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val); char* end;
            long v = strtol(sp, &end, 10);
            if (end == sp) return ray_error("domain", NULL);
            return ray_i16((int16_t)v);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST)
            return cast_vec_numeric(type_sym, val, RAY_I16);
        return ray_error("type", NULL);
    }
    /* Cast to F64 / f64 */
    if (cast_match(tname, tlen, "F64") || cast_match(tname, tlen, "f64")) {
        ray_release(s);
        if (val->type == -RAY_F64) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return make_f64(val->b8 ? 1.0 : 0.0);
        if (val->type == -RAY_I64) return make_f64((double)val->i64);
        if (val->type == -RAY_I32) return make_f64((double)val->i32);
        if (val->type == -RAY_I16) return make_f64((double)val->i16);
        if (val->type == -RAY_U8)  return make_f64((double)val->u8);
        if (val->type == -RAY_DATE || val->type == -RAY_TIME) return make_f64((double)val->i32);
        if (val->type == -RAY_TIMESTAMP) return make_f64((double)val->i64);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            if (!sp) return ray_error("domain", NULL);
            char* end;
            double v = strtod(sp, &end);
            if (end == sp) return ray_error("domain", NULL);
            return make_f64(v);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST)
            return cast_vec_numeric(type_sym, val, RAY_F64);
        return ray_error("type", NULL);
    }
    /* Cast to B8/BOOL/b8 */
    if (cast_match(tname, tlen, "BOOL") || cast_match(tname, tlen, "B8") || cast_match(tname, tlen, "b8")) {
        ray_release(s);
        if (val->type == -RAY_BOOL) { ray_retain(val); return val; }
        if (val->type == -RAY_I64) return make_bool(val->i64 != 0 ? 1 : 0);
        if (val->type == -RAY_I32) return make_bool(val->i32 != 0 ? 1 : 0);
        if (val->type == -RAY_I16) return make_bool(val->i16 != 0 ? 1 : 0);
        if (val->type == -RAY_U8) return make_bool(val->u8 != 0 ? 1 : 0);
        if (val->type == -RAY_F64) return make_bool(val->f64 != 0.0 ? 1 : 0);
        if (val->type == -RAY_DATE) return make_bool(val->i32 != 0 ? 1 : 0);
        if (val->type == -RAY_TIME) return make_bool(val->i32 != 0 ? 1 : 0);
        if (val->type == -RAY_TIMESTAMP) return make_bool(val->i64 != 0 ? 1 : 0);
        if (val->type == -RAY_STR) return make_bool(ray_str_len(val) > 0 ? 1 : 0);
        /* Vector cast: b8/B8 */
        if (ray_is_vec(val) || val->type == RAY_LIST)
            return cast_vec_numeric(type_sym, val, RAY_BOOL);
        return ray_error("type", NULL);
    }
    /* Cast to STR/str */
    if (cast_match(tname, tlen, "STR") || cast_match(tname, tlen, "str")) {
        ray_release(s);
        if (val->type == -RAY_STR) { ray_retain(val); return val; }
        if (val->type == -RAY_SYM) {
            ray_t* sym_str = ray_sym_str(val->i64);
            return sym_str ? sym_str : ray_str("", 0);
        }
        if (val->type == -RAY_I64) {
            char buf[32]; int n2 = snprintf(buf, sizeof(buf), "%lld", (long long)val->i64);
            return ray_str(buf, (size_t)n2);
        }
        if (val->type == -RAY_I32) {
            char buf[32]; int n2 = snprintf(buf, sizeof(buf), "%d", (int)val->i32);
            return ray_str(buf, (size_t)n2);
        }
        if (val->type == -RAY_I16) {
            char buf[32]; int n2 = snprintf(buf, sizeof(buf), "%d", (int)val->i16);
            return ray_str(buf, (size_t)n2);
        }
        if (val->type == -RAY_F64) {
            double fv = val->f64;
            if (fv == 0.0 && signbit(fv)) fv = 0.0;
            char buf[32]; int n2 = snprintf(buf, sizeof(buf), "%g", fv);
            return ray_str(buf, (size_t)n2);
        }
        if (val->type == -RAY_BOOL) {
            return val->b8 ? ray_str("true", 4) : ray_str("false", 5);
        }
        /* Fallback: use ray_fmt for any other atom type */
        if (ray_is_atom(val)) {
            ray_t* formatted = ray_fmt(val, 0);
            if (formatted && !RAY_IS_ERR(formatted)) return formatted;
            if (formatted) ray_release(formatted);
        }
        /* Vector/list -> STR vector: cast each element to string */
        if (ray_is_vec(val) || val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_STR, n2);
            if (!vec || RAY_IS_ERR(vec)) return vec ? vec : ray_error("oom", NULL);
            for (int64_t i = 0; i < n2; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
                ray_t* cast = ray_cast_fn(type_sym, elem);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                const char* sp = ray_str_ptr(cast);
                size_t slen = ray_str_len(cast);
                vec = ray_str_vec_append(vec, sp ? sp : "", sp ? slen : 0);
                ray_release(cast);
                if (RAY_IS_ERR(vec)) return vec;
            }
            ray_t* result = cast_vec_copy_nulls(vec, val);
            if (RAY_IS_ERR(result)) return result;
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to SYMBOL/sym */
    if (cast_match(tname, tlen, "SYMBOL") || cast_match(tname, tlen, "sym") || cast_match(tname, tlen, "symbol")) {
        ray_release(s);
        if (val->type == -RAY_SYM) { ray_retain(val); return val; }
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            size_t slen = ray_str_len(val);
            int64_t id = ray_sym_intern(sp, slen);
            return ray_sym(id);
        }
        /* Integer/bool atom -> symbol: convert to plain number string */
        if (ray_is_atom(val) && (is_numeric(val) || val->type == -RAY_BOOL)) {
            char buf[64]; int n2;
            if (val->type == -RAY_BOOL)     n2 = snprintf(buf, sizeof(buf), "%d", (int)val->b8);
            else if (val->type == -RAY_U8)  n2 = snprintf(buf, sizeof(buf), "%u", (unsigned)val->u8);
            else if (val->type == -RAY_I16) n2 = snprintf(buf, sizeof(buf), "%d", (int)val->i16);
            else if (val->type == -RAY_I32) n2 = snprintf(buf, sizeof(buf), "%d", (int)val->i32);
            else if (val->type == -RAY_F64) {
                double fv = val->f64;
                if (fv == 0.0 && signbit(fv)) fv = 0.0;
                n2 = snprintf(buf, sizeof(buf), "%.17g", fv);
            }
            else n2 = snprintf(buf, sizeof(buf), "%lld", (long long)as_i64(val));
            if (n2 > 0) {
                int64_t id = ray_sym_intern(buf, (size_t)n2);
                return ray_sym(id);
            }
        }
        /* Temporal/guid atom -> symbol: use ray_fmt for formatting */
        if (ray_is_atom(val) && (is_temporal(val) || val->type == -RAY_GUID)) {
            ray_t* formatted = ray_fmt(val, 0);
            if (formatted && !RAY_IS_ERR(formatted)) {
                const char* sp = ray_str_ptr(formatted);
                size_t slen = ray_str_len(formatted);
                int64_t id = ray_sym_intern(sp, slen);
                ray_release(formatted);
                return ray_sym(id);
            }
            if (formatted) ray_release(formatted);
        }
        /* Vector cast: SYMBOL vec from other vecs */
        if (ray_is_vec(val) || val->type == RAY_LIST)
            return cast_vec_numeric(type_sym, val, RAY_SYM);
        return ray_error("type", NULL);
    }
    /* Cast to DATE/date */
    if (cast_match(tname, tlen, "DATE") || cast_match(tname, tlen, "date")) {
        ray_release(s);
        if (val->type == -RAY_DATE) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_date((int64_t)val->b8);
        if (val->type == -RAY_U8)  return ray_date((int64_t)val->u8);
        if (val->type == -RAY_I16) return ray_date((int64_t)val->i16);
        if (val->type == -RAY_I32) return ray_date((int64_t)val->i32);
        if (val->type == -RAY_I64) return ray_date(val->i64);
        if (val->type == -RAY_F64) return ray_date((int64_t)val->f64);
        if (val->type == -RAY_TIME) return ray_date((int64_t)val->i32);
        if (val->type == -RAY_TIMESTAMP) return ray_date(ts_days_floor(val->i64));
        if (val->type == -RAY_STR) {
            /* Parse "YYYY.MM.DD" format */
            const char* sp = ray_str_ptr(val);
            int y, m, d2;
            if (sscanf(sp, "%d.%d.%d", &y, &m, &d2) != 3) return ray_error("domain", NULL);
            int64_t days = 0;
            { int ty;
              for (ty = 2000; ty < y; ty++) days += (ty % 4 == 0 && (ty % 100 != 0 || ty % 400 == 0)) ? 366 : 365;
              for (ty = y; ty < 2000; ty++) days -= (ty % 4 == 0 && (ty % 100 != 0 || ty % 400 == 0)) ? 366 : 365;
            }
            { static const int md2[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
              int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
              for (int mi = 1; mi < m; mi++) days += md2[mi] + (mi == 2 && leap ? 1 : 0);
              days += d2 - 1;
            }
            return ray_date(days);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST)
            return cast_vec_numeric(type_sym, val, RAY_DATE);
        return ray_error("type", NULL);
    }
    /* Cast to TIME/time */
    if (cast_match(tname, tlen, "TIME") || cast_match(tname, tlen, "time")) {
        ray_release(s);
        if (val->type == -RAY_TIME) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_time((int64_t)val->b8);
        if (val->type == -RAY_U8)  return ray_time((int64_t)val->u8);
        if (val->type == -RAY_I16) return ray_time((int64_t)val->i16);
        if (val->type == -RAY_I32) return ray_time((int64_t)val->i32);
        if (val->type == -RAY_I64) return ray_time(val->i64);
        if (val->type == -RAY_F64) return ray_time((int64_t)val->f64);
        if (val->type == -RAY_DATE) return ray_time((int64_t)val->i32);
        if (val->type == -RAY_TIMESTAMP)
            /* TIMESTAMP is ns since epoch; TIME stores ms-of-day.  Use
             * floor-mod (not C-style truncate-toward-zero %) so pre-
             * 2000 timestamps give time-of-day in [0, 86_400_000) ms,
             * matching wall-clock semantics. */
            return ray_time((int64_t)(ts_ns_in_day(val->i64) / 1000000LL));
        if (val->type == -RAY_STR) {
            /* Parse "HH:MM:SS[.mmm]" */
            const char* sp = ray_str_ptr(val);
            int th = 0, tm = 0, ts = 0, tms = 0;
            int nr = sscanf(sp, "%d:%d:%d", &th, &tm, &ts);
            if (nr < 2) return ray_error("domain", NULL);
            const char* dot = strchr(sp, '.');
            if (dot) {
                dot++;
                char mbuf[4] = "000";
                int mi = 0;
                while (*dot >= '0' && *dot <= '9' && mi < 3) mbuf[mi++] = *dot++;
                tms = (int)strtol(mbuf, NULL, 10);
            }
            int32_t ms = (int32_t)th * 3600000 + (int32_t)tm * 60000 + (int32_t)ts * 1000 + tms;
            return ray_time((int64_t)ms);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST)
            return cast_vec_numeric(type_sym, val, RAY_TIME);
        return ray_error("type", NULL);
    }
    /* Cast to TIMESTAMP/timestamp */
    if (cast_match(tname, tlen, "TIMESTAMP") || cast_match(tname, tlen, "timestamp")) {
        ray_release(s);
        if (val->type == -RAY_TIMESTAMP) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_timestamp((int64_t)val->b8);
        if (val->type == -RAY_U8)  return ray_timestamp((int64_t)val->u8);
        if (val->type == -RAY_I16) return ray_timestamp((int64_t)val->i16);
        if (val->type == -RAY_I32) return ray_timestamp((int64_t)val->i32);
        if (val->type == -RAY_I64) return ray_timestamp(val->i64);
        if (val->type == -RAY_F64) return ray_timestamp((int64_t)val->f64);
        if (val->type == -RAY_TIME) return ray_timestamp((int64_t)val->i32);
        if (val->type == -RAY_DATE) {
            int64_t days = val->i32;
            return ray_timestamp(days * 24LL * 60 * 60 * 1000000000LL);
        }
        /* ISO string -> timestamp: "YYYY-MM-DD[T ]HH:MM:SS[.nnn...]" or "YYYY.MM.DDDHH:MM:SS.nnn..." */
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            size_t sl = ray_str_len(val);
            if (sl < 10) return ray_error("domain", NULL);
            int y, m, d, hh = 0, mm = 0, ss = 0;
            long long frac = 0;
            /* Try both formats: YYYY-MM-DD and YYYY.MM.DD */
            int parsed = sscanf(sp, "%d-%d-%d", &y, &m, &d);
            /* parse date: try YYYY-MM-DD then YYYY.MM.DD */
            if (parsed != 3) {
                parsed = sscanf(sp, "%d.%d.%d", &y, &m, &d);
                /* YYYY.MM.DD format */
            }
            if (parsed != 3) return ray_error("domain", NULL);
            /* Parse optional time part */
            if (sl > 10 && (sp[10] == 'T' || sp[10] == ' ' || sp[10] == 'D')) {
                sscanf(sp + 11, "%d:%d:%d", &hh, &mm, &ss);
                /* Parse fractional seconds */
                const char* dot = memchr(sp + 11, '.', sl - 11);
                if (dot) {
                    dot++;
                    char fbuf[10] = "000000000";
                    int fi = 0;
                    while (*dot >= '0' && *dot <= '9' && fi < 9) fbuf[fi++] = *dot++;
                    frac = strtoll(fbuf, NULL, 10);
                }
            }
            /* Convert to days since 2000-01-01 */
            int64_t days = 0;
            { int ty;
              for (ty = 2000; ty < y; ty++) days += (ty % 4 == 0 && (ty % 100 != 0 || ty % 400 == 0)) ? 366 : 365;
              for (ty = y; ty < 2000; ty++) days -= (ty % 4 == 0 && (ty % 100 != 0 || ty % 400 == 0)) ? 366 : 365;
            }
            { static const int md[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
              int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
              for (int mi = 1; mi < m; mi++) days += md[mi] + (mi == 2 && leap ? 1 : 0);
              days += d - 1;
            }
            int64_t ns = days * 86400000000000LL + (int64_t)hh * 3600000000000LL +
                         (int64_t)mm * 60000000000LL + (int64_t)ss * 1000000000LL + frac;
            /* Handle timezone offset: Z, +HH:MM, -HH:MM, +HHMM, -HHMM */
            if (sl > 19) {
                const char* tz = sp + 19; /* after YYYY-MM-DDTHH:MM:SS */
                /* Skip fractional seconds */
                if (tz < sp + sl && *tz == '.') {
                    tz++;
                    while (tz < sp + sl && *tz >= '0' && *tz <= '9') tz++;
                }
                if (tz < sp + sl) {
                    if (*tz == 'Z') {
                        /* UTC, no adjustment */
                    } else if (*tz == '+' || *tz == '-') {
                        int tz_sign = (*tz == '+') ? 1 : -1;
                        int tz_hh = 0, tz_mm = 0;
                        tz++;
                        /* Parse HH:MM or HHMM */
                        if (tz + 4 < sp + sl && tz[2] == ':') {
                            sscanf(tz, "%2d:%2d", &tz_hh, &tz_mm);
                        } else {
                            sscanf(tz, "%2d%2d", &tz_hh, &tz_mm);
                        }
                        int64_t tz_ns = ((int64_t)tz_hh * 3600 + (int64_t)tz_mm * 60) * 1000000000LL;
                        ns -= tz_sign * tz_ns;
                    }
                }
            }
            return ray_timestamp(ns);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST)
            return cast_vec_numeric(type_sym, val, RAY_TIMESTAMP);
        return ray_error("type", NULL);
    }
    /* Cast to GUID/guid */
    if (cast_match(tname, tlen, "GUID") || cast_match(tname, tlen, "guid")) {
        ray_release(s);
        if (val->type == -RAY_GUID) { ray_retain(val); return val; }
        if (val->type == -RAY_STR) {
            /* Parse UUID string: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" */
            const char* sp = ray_str_ptr(val);
            size_t sl = ray_str_len(val);
            if (sl < 36) return ray_error("domain", NULL);
            uint8_t bytes[16];
            const char* p = sp;
            for (int bi = 0; bi < 16; bi++) {
                if (*p == '-') p++;
                char hi = *p++;
                char lo = *p++;
                int h = (hi >= 'a') ? hi - 'a' + 10 : (hi >= 'A') ? hi - 'A' + 10 : hi - '0';
                int l = (lo >= 'a') ? lo - 'a' + 10 : (lo >= 'A') ? lo - 'A' + 10 : lo - '0';
                bytes[bi] = (uint8_t)((h << 4) | l);
            }
            return ray_guid(bytes);
        }
        /* Vector of GUIDs: empty vector cast */
        if (ray_is_vec(val) && val->len == 0)
            return ray_vec_new(RAY_GUID, 0);
        /* List of strings -> GUID vector */
        if (val->type == RAY_LIST) {
            int64_t n2 = val->len;
            ray_t* vec = ray_vec_new(RAY_GUID, n2);
            if (RAY_IS_ERR(vec)) return vec;
            vec->len = n2;
            uint8_t* data = (uint8_t*)ray_data(vec);
            ray_t** items = (ray_t**)ray_data(val);
            for (int64_t i = 0; i < n2; i++) {
                ray_t* cast = ray_cast_fn(type_sym, items[i]);
                if (RAY_IS_ERR(cast)) { ray_release(vec); return cast; }
                if (cast->obj) memcpy(data + i * 16, ray_data(cast->obj), 16);
                else memcpy(data + i * 16, ray_data(cast), 16);
                ray_release(cast);
            }
            ray_t* result = cast_vec_copy_nulls(vec, val);
            if (RAY_IS_ERR(result)) return result;
            return vec;
        }
        return ray_error("type", NULL);
    }
    /* Cast to U8/u8 */
    if (cast_match(tname, tlen, "U8") || cast_match(tname, tlen, "u8")) {
        ray_release(s);
        if (val->type == -RAY_U8) { ray_retain(val); return val; }
        if (val->type == -RAY_BOOL) return ray_u8(val->b8 ? 1 : 0);
        if (val->type == -RAY_I16) return ray_u8((uint8_t)val->i16);
        if (val->type == -RAY_I32) return ray_u8((uint8_t)val->i32);
        if (val->type == -RAY_I64) return ray_u8((uint8_t)val->i64);
        if (val->type == -RAY_F64) return ray_u8((uint8_t)val->f64);
        if (val->type == -RAY_STR) {
            const char* sp = ray_str_ptr(val);
            char* end; long v = strtol(sp, &end, 10);
            if (end == sp) return ray_error("domain", NULL);
            return ray_u8((uint8_t)v);
        }
        /* Vector cast */
        if (ray_is_vec(val) || val->type == RAY_LIST)
            return cast_vec_numeric(type_sym, val, RAY_U8);
        return ray_error("type", NULL);
    }
    /* Cast to DICT */
    if (cast_match(tname, tlen, "DICT")) {
        ray_release(s);
        if (val->type == RAY_DICT) { ray_retain(val); return val; }
        /* Table -> Dict */
        if (val->type == RAY_TABLE) {
            int64_t ncols = ray_table_ncols(val);
            ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, ncols);
            if (RAY_IS_ERR(keys)) return keys;
            ray_t* vals = ray_list_new(ncols);
            if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
            for (int64_t c = 0; c < ncols; c++) {
                int64_t col_name = ray_table_col_name(val, c);
                ray_t* col_val = ray_table_get_col_idx(val, c);
                keys = ray_vec_append(keys, &col_name);
                if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
                vals = ray_list_append(vals, col_val);
                if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
            }
            return ray_dict_new(keys, vals);
        }
        return ray_error("type", NULL);
    }
    /* Cast to TABLE */
    if (cast_match(tname, tlen, "TABLE")) {
        ray_release(s);
        if (val->type == RAY_TABLE) { ray_retain(val); return val; }
        /* Dict -> Table */
        if (val->type == RAY_DICT) {
            ray_t* dkeys = ray_dict_keys(val);
            ray_t* dvals = ray_dict_vals(val);
            int64_t ncols = dkeys ? dkeys->len : 0;
            if (!dkeys || dkeys->type != RAY_SYM || !dvals || dvals->type != RAY_LIST)
                return ray_error("type", NULL);
            ray_t** col_ptrs = (ray_t**)ray_data(dvals);
            ray_t* tbl = ray_table_new(ncols);
            if (RAY_IS_ERR(tbl)) return tbl;
            for (int64_t c = 0; c < ncols; c++) {
                int64_t col_name = ray_read_sym(ray_data(dkeys), c, RAY_SYM, dkeys->attrs);
                ray_t* col_val = col_ptrs[c];
                ray_retain(col_val);
                tbl = ray_table_add_col(tbl, col_name, col_val);
                ray_release(col_val);
                if (RAY_IS_ERR(tbl)) return tbl;
            }
            return tbl;
        }
        return ray_error("type", NULL);
    }
    ray_release(s);
    return ray_error("domain", NULL);
}

/* (type val) — return the type code of a value */
/* ray_type_name moved to internal.h */

ray_t* ray_type_fn(ray_t* val) {
    if (!val || RAY_IS_NULL(val)) return ray_sym(ray_sym_intern("null", 4));
    const char* name = ray_type_name(val->type);
    int64_t id = ray_sym_intern(name, strlen(name));
    return ray_sym(id);
}

/* (read path) — read a file's contents as a string */
ray_t* ray_read_file_fn(ray_t* path_obj) {
    if (path_obj->type != -RAY_STR) return ray_error("type", NULL);
    const char* path = ray_str_ptr(path_obj);
    if (!path) return ray_error("domain", NULL);
    FILE* fp = fopen(path, "rb");
    if (!fp) return ray_error("io", NULL);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return ray_error("io", NULL); }
    /* Use ray_alloc for the buffer */
    ray_t* buf = ray_alloc((size_t)sz + 1);
    if (!buf || RAY_IS_ERR(buf)) { fclose(fp); return ray_error("oom", NULL); }
    char* data = (char*)ray_data(buf);
    size_t rd = fread(data, 1, (size_t)sz, fp);
    fclose(fp);
    data[rd] = '\0';
    ray_t* result = ray_str(data, rd);
    ray_release(buf);
    return result;
}

/* (load path) — read and evaluate a Rayfall script file via mmap */
ray_t* ray_load_file_fn(ray_t* path_obj) {
    if (path_obj->type != -RAY_STR) return ray_error("type", NULL);
    const char* path = ray_str_ptr(path_obj);
    if (!path) return ray_error("domain", NULL);
    size_t path_len = ray_str_len(path_obj);

#if defined(RAY_OS_WINDOWS)
    /* Windows: fall back to fread */
    FILE* fp = fopen(path, "r");
    if (!fp) return ray_error("io", NULL);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return ray_error("io", NULL); }
    if (sz == 0) { fclose(fp); return ray_i64(0); }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return ray_error("oom", NULL); }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';

    ray_t* nfo = ray_nfo_create(path, path_len, buf, rd);
    ray_t* parsed = ray_parse_with_nfo(buf, nfo);
    if (RAY_IS_ERR(parsed)) { ray_release(nfo); free(buf); return parsed; }

    ray_t* prev_nfo = ray_eval_get_nfo();
    ray_eval_set_nfo(nfo);
    ray_t* result = ray_eval(parsed);
    ray_eval_set_nfo(prev_nfo);

    ray_release(parsed);
    ray_release(nfo);
    free(buf);
    return result;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ray_error("io", NULL);
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0) { close(fd); return ray_error("io", NULL); }
    size_t sz = (size_t)st.st_size;
    if (sz == 0) { close(fd); return ray_i64(0); }
    char* map = (char*)mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return ray_error("io", NULL);
    /* Copy to NUL-terminated buffer -- mmap region may not have a trailing NUL */
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { munmap(map, sz); return ray_error("oom", NULL); }
    memcpy(buf, map, sz);
    buf[sz] = '\0';
    munmap(map, sz);

    ray_t* nfo = ray_nfo_create(path, path_len, buf, sz);
    ray_t* parsed = ray_parse_with_nfo(buf, nfo);
    if (RAY_IS_ERR(parsed)) { ray_release(nfo); free(buf); return parsed; }

    ray_t* prev_nfo = ray_eval_get_nfo();
    ray_eval_set_nfo(nfo);
    ray_t* result = ray_eval(parsed);
    ray_eval_set_nfo(prev_nfo);

    ray_release(parsed);
    ray_release(nfo);
    free(buf);
    return result;
#endif
}

/* (write path content) — write string to a file */
ray_t* ray_write_file_fn(ray_t* path_obj, ray_t* content) {
    if (path_obj->type != -RAY_STR) return ray_error("type", NULL);
    if (content->type != -RAY_STR) return ray_error("type", NULL);
    const char* path = ray_str_ptr(path_obj);
    const char* data = ray_str_ptr(content);
    size_t len = ray_str_len(content);
    if (!path || !data) return ray_error("domain", NULL);
    FILE* fp = fopen(path, "wb");
    if (!fp) return ray_error("io", NULL);
    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);
    if (written != len) return ray_error("io", NULL);
    return make_i64(0);
}

/* ══════════════════════════════════════════
 * Additional builtins (ported from rayforce)
 * ══════════════════════════════════════════ */

/* (enlist a b c ...) -> typed vector from atoms */
ray_t* ray_enlist_fn(ray_t** args, int64_t n) {
    if (n == 0) return ray_vec_new(RAY_I64, 0);
    /* Determine type from first arg */
    int8_t atype = args[0]->type;
    bool homogeneous = true;
    bool has_float = (atype == -RAY_F64);
    bool has_int = (atype == -RAY_I64);
    for (int64_t i = 1; i < n; i++) {
        if (args[i]->type != atype) homogeneous = false;
        if (args[i]->type == -RAY_F64) has_float = true;
        if (args[i]->type == -RAY_I64) has_int = true;
    }
    /* Mixed int/float -> promote to f64 */
    if (!homogeneous && has_float && has_int) {
        ray_t* vec = ray_vec_new(RAY_F64, n);
        if (RAY_IS_ERR(vec)) return vec;
        double* d = (double*)ray_data(vec);
        for (int64_t i = 0; i < n; i++)
            d[i] = (args[i]->type == -RAY_F64) ? args[i]->f64 : (double)args[i]->i64;
        vec->len = n;
        for (int64_t i = 0; i < n; i++) {
            if (RAY_ATOM_IS_NULL(args[i]))
                ray_vec_set_null(vec, i, true);
        }
        return vec;
    }
    if (homogeneous && atype < 0) {
        int8_t vtype = -atype;
        ray_t* vec = ray_vec_new(vtype, n);
        if (RAY_IS_ERR(vec)) return vec;
        switch (vtype) {
        case RAY_I64: case RAY_TIMESTAMP: {
            int64_t* d = (int64_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->i64;
            break;
        }
        case RAY_F64: {
            double* d = (double*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->f64;
            break;
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            int32_t* d = (int32_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->i32;
            break;
        }
        case RAY_I16: {
            int16_t* d = (int16_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->i16;
            break;
        }
        case RAY_BOOL: {
            bool* d = (bool*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->b8;
            break;
        }
        case RAY_SYM: {
            int64_t* d = (int64_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->i64;
            break;
        }
        case RAY_U8: {
            uint8_t* d = (uint8_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = args[i]->u8;
            break;
        }
        case RAY_STR: {
            ray_t* svec = ray_vec_new(RAY_STR, n);
            if (RAY_IS_ERR(svec)) { ray_free(vec); return svec; }
            for (int64_t i = 0; i < n; i++) {
                svec = ray_str_vec_append(svec, ray_str_ptr(args[i]), ray_str_len(args[i]));
                if (RAY_IS_ERR(svec)) return svec;
            }
            ray_free(vec);
            return svec;
        }
        case RAY_GUID: {
            uint8_t* d = (uint8_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) {
                const uint8_t* gd = args[i]->obj ? (const uint8_t*)ray_data(args[i]->obj) : (const uint8_t*)ray_data(args[i]);
                memcpy(d + i * 16, gd, 16);
            }
            break;
        }
        default: goto as_list;
        }
        vec->len = n;
        for (int64_t i = 0; i < n; i++) {
            if (RAY_ATOM_IS_NULL(args[i]))
                ray_vec_set_null(vec, i, true);
        }
        return vec;
    }
as_list:;
    /* Heterogeneous -> list */
    ray_t* lst = ray_list_new((int32_t)n);
    if (RAY_IS_ERR(lst)) return lst;
    for (int64_t i = 0; i < n; i++) {
        ray_retain(args[i]);
        lst = ray_list_append(lst, args[i]);
        ray_release(args[i]);
        if (RAY_IS_ERR(lst)) return lst;
    }
    return lst;
}

/* (dict keys vals) -> dict.  Wraps two parallel containers as a [keys,
 * vals] block.  When vals is shorter than keys, the tail is filled with
 * typed null I64.  Both inputs are copied (refs retained) — caller keeps
 * ownership of the originals. */
ray_t* ray_dict_fn(ray_t* keys, ray_t* vals) {
    if (!ray_is_vec(keys)) return ray_error("type", NULL);
    int64_t n = keys->len;

    /* Hold a fresh ref to keys so ownership is transferred into the dict. */
    ray_retain(keys);

    /* Materialize vals as RAY_LIST of length n. */
    ray_t* vlist = ray_list_new(n);
    if (RAY_IS_ERR(vlist)) { ray_release(keys); return vlist; }
    for (int64_t i = 0; i < n; i++) {
        ray_t* v;
        int alloc = 0;
        if (vals->type == RAY_LIST) {
            v = (i < vals->len) ? ((ray_t**)ray_data(vals))[i] : NULL;
        } else if (ray_is_vec(vals)) {
            v = collection_elem(vals, i, &alloc);
        } else {
            v = vals;
        }
        if (v && !RAY_IS_ERR(v)) {
            vlist = ray_list_append(vlist, v);
            if (alloc) ray_release(v);
        } else {
            ray_t* null_val = ray_typed_null(-RAY_I64);
            vlist = ray_list_append(vlist, null_val);
            ray_release(null_val);
        }
        if (RAY_IS_ERR(vlist)) { ray_release(keys); return vlist; }
    }
    return ray_dict_new(keys, vlist);
}

/* (nil? x) -> true if x is null */
ray_t* ray_nil_fn(ray_t* x) {
    if (!x || RAY_IS_NULL(x)) return ray_bool(true);
    if (ray_is_atom(x) && RAY_ATOM_IS_NULL(x)) return ray_bool(true);
    return ray_bool(false);
}

/* (where bool-vec) -> indices of true values */
ray_t* ray_where_fn(ray_t* x) {
    if (!ray_is_vec(x) || x->type != RAY_BOOL)
        return ray_error("type", NULL);
    bool* data = (bool*)ray_data(x);
    int64_t n = x->len;
    /* Count trues */
    int64_t cnt = 0;
    for (int64_t i = 0; i < n; i++) if (data[i]) cnt++;
    ray_t* result = ray_vec_new(RAY_I64, cnt);
    if (RAY_IS_ERR(result)) return result;
    int64_t* out = (int64_t*)ray_data(result);
    int64_t j = 0;
    for (int64_t i = 0; i < n; i++) if (data[i]) out[j++] = i;
    result->len = cnt;
    return result;
}

/* (group vec) -> dict mapping each unique value to its indices */
/* ---------------------------------------------------------------------------
 * Open-address hash set for ray_group_fn's scalar / GUID fast paths.
 *
 * Each slot holds either GHT_EMPTY or an already-allocated group index.
 * Lookups compare keys by calling back into the caller with the stored
 * group index — the caller already knows whether the key shape is a
 * plain int64 (scalar) or 16 bytes of guid material in the source
 * column.  Load factor is capped at 0.5; grow on overflow.
 *
 * The table is ref-counted via ray_alloc so the main bookkeeping code
 * can free it in one place on every exit path.
 * ------------------------------------------------------------------------- */

#define GHT_EMPTY 0xFFFFFFFFu

typedef struct group_ht_t {
    ray_t*     block;   /* backing ray_alloc block */
    uint32_t*  slots;   /* cap entries */
    uint32_t   cap;     /* power of 2 */
    uint32_t   mask;    /* cap - 1 */
    uint32_t   count;   /* live entries */
} group_ht_t;

static bool group_ht_init(group_ht_t* h, uint32_t initial_cap) {
    uint32_t cap = 16;
    while (cap < initial_cap) cap *= 2;
    h->block = ray_alloc((size_t)cap * sizeof(uint32_t));
    if (!h->block || RAY_IS_ERR(h->block)) { h->block = NULL; return false; }
    h->slots = (uint32_t*)ray_data(h->block);
    h->cap   = cap;
    h->mask  = cap - 1;
    h->count = 0;
    for (uint32_t i = 0; i < cap; i++) h->slots[i] = GHT_EMPTY;
    return true;
}

static void group_ht_free(group_ht_t* h) {
    if (h->block) ray_free(h->block);
    h->block = NULL;
    h->slots = NULL;
    h->cap = h->mask = h->count = 0;
}

/* Rehash callback: given the stored group index, return the hash for
 * it.  This lets us grow without recomputing raw keys — caller knows
 * how to translate gi back to a key. */
typedef uint64_t (*group_ht_gi_hash_fn)(uint32_t gi, void* ctx);

static bool group_ht_grow(group_ht_t* h, group_ht_gi_hash_fn hash_gi, void* ctx) {
    uint32_t new_cap = h->cap * 2;
    if (new_cap < h->cap) return false;  /* overflow */
    ray_t* new_block = ray_alloc((size_t)new_cap * sizeof(uint32_t));
    if (!new_block || RAY_IS_ERR(new_block)) return false;
    uint32_t* new_slots = (uint32_t*)ray_data(new_block);
    uint32_t new_mask = new_cap - 1;
    for (uint32_t i = 0; i < new_cap; i++) new_slots[i] = GHT_EMPTY;
    for (uint32_t i = 0; i < h->cap; i++) {
        uint32_t gi = h->slots[i];
        if (gi == GHT_EMPTY) continue;
        uint64_t hh = hash_gi(gi, ctx);
        uint32_t slot = (uint32_t)(hh & new_mask);
        while (new_slots[slot] != GHT_EMPTY) slot = (slot + 1) & new_mask;
        new_slots[slot] = gi;
    }
    ray_free(h->block);
    h->block = new_block;
    h->slots = new_slots;
    h->cap   = new_cap;
    h->mask  = new_mask;
    return true;
}

static inline uint64_t mix64(uint64_t h) {
    /* Murmur3 fmix64 */
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 33;
    return h;
}

static inline uint64_t hash_guid(const uint8_t* g) {
    uint64_t a, b;
    memcpy(&a, g,     8);
    memcpy(&b, g + 8, 8);
    return mix64(a ^ (b * 0x9E3779B97F4A7C15ULL));
}

static inline uint64_t hash_i64(int64_t v) {
    return mix64((uint64_t)v);
}

/* Context for GUID rehash: the 16-byte source base and, indirectly,
 * gvals — which stores the row_idx of the first occurrence per group. */
typedef struct {
    const uint8_t* base;
    const int64_t* gvals;
} ght_guid_ctx_t;

static uint64_t ght_guid_hash_gi(uint32_t gi, void* ctx) {
    ght_guid_ctx_t* c = (ght_guid_ctx_t*)ctx;
    return hash_guid(c->base + c->gvals[gi] * 16);
}

typedef struct { const int64_t* gvals; } ght_i64_ctx_t;
static uint64_t ght_i64_hash_gi(uint32_t gi, void* ctx) {
    ght_i64_ctx_t* c = (ght_i64_ctx_t*)ctx;
    return hash_i64(c->gvals[gi]);
}

/* Grow the per-group bookkeeping arrays used by ray_group_fn.
 * Doubles capacity; copies existing entries; returns false on OOM.
 * Caller is responsible for cleaning up and returning an error if this fails. */
static bool group_grow(ray_t** val_block, ray_t** ivblock,
                       int64_t** gvals, ray_t*** idx_vecs,
                       int64_t cur_count, int64_t* max_groups) {
    int64_t new_max = *max_groups * 2;
    if (new_max <= *max_groups) return false;  /* overflow */
    ray_t* new_val = ray_alloc((size_t)new_max * sizeof(int64_t));
    if (!new_val || RAY_IS_ERR(new_val)) return false;
    ray_t* new_iv = ray_alloc((size_t)new_max * sizeof(ray_t*));
    if (!new_iv || RAY_IS_ERR(new_iv)) { ray_free(new_val); return false; }
    memcpy(ray_data(new_val), *gvals, (size_t)cur_count * sizeof(int64_t));
    memcpy(ray_data(new_iv), *idx_vecs, (size_t)cur_count * sizeof(ray_t*));
    ray_free(*val_block);
    ray_free(*ivblock);
    *val_block = new_val;
    *ivblock = new_iv;
    *gvals = (int64_t*)ray_data(new_val);
    *idx_vecs = (ray_t**)ray_data(new_iv);
    *max_groups = new_max;
    return true;
}

ray_t* ray_group_fn(ray_t* x) {
    if (!ray_is_vec(x) && x->type != RAY_LIST)
        return ray_error("type", NULL);
    int64_t n = x->len;
    if (n == 0) {
        ray_t* keys = ray_list_new(0);
        if (RAY_IS_ERR(keys)) return keys;
        ray_t* vals = ray_list_new(0);
        if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
        return ray_dict_new(keys, vals);
    }

    /* Collect unique values; the scalar and RAY_GUID paths grow these
     * arrays on demand via group_grow().  The RAY_LIST and RAY_STR
     * paths below still cap at this initial size (they have their own
     * side buffers that aren't yet wired into group_grow); starting at
     * 1024 preserves their prior behaviour. */
    int64_t max_groups = n < 1024 ? n : 1024;
    ray_t* val_block = ray_alloc((size_t)(max_groups * sizeof(int64_t)));
    if (RAY_IS_ERR(val_block)) return val_block;
    int64_t* gvals = (int64_t*)ray_data(val_block);

    /* For each group, store indices in a separate i64 vector */
    ray_t** idx_vecs = NULL;
    ray_t* ivblock = ray_alloc((size_t)(max_groups * sizeof(ray_t*)));
    if (RAY_IS_ERR(ivblock)) { ray_free(val_block); return ivblock; }
    idx_vecs = (ray_t**)ray_data(ivblock);
    int64_t ngroups = 0;

    /* For LIST type, use atom_eq-based grouping with stored keys */
    if (x->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(x);
        /* Store group keys as ray_t* pointers */
        ray_t* kblock = ray_alloc((size_t)(max_groups * sizeof(ray_t*)));
        if (RAY_IS_ERR(kblock)) { ray_free(val_block); ray_free(ivblock); return kblock; }
        ray_t** gkeys = (ray_t**)ray_data(kblock);

        for (int64_t i = 0; i < n; i++) {
            ray_t* elem = elems[i];
            int64_t gi = -1;
            for (int64_t g = 0; g < ngroups; g++) {
                if (atom_eq(gkeys[g], elem)) { gi = g; break; }
            }
            if (gi < 0) {
                if (ngroups >= max_groups) {
                    for (int64_t g = 0; g < ngroups; g++) ray_release(idx_vecs[g]);
                    ray_free(val_block); ray_free(ivblock); ray_free(kblock);
                    return ray_error("limit", NULL);
                }
                gi = ngroups++;
                gkeys[gi] = elem;
                idx_vecs[gi] = ray_vec_new(RAY_I64, 0);
            }
            idx_vecs[gi] = ray_vec_append(idx_vecs[gi], &i);
        }
        /* Build dict: keys as RAY_LIST (heterogeneous atoms), vals as
         * RAY_LIST of I64 idx vectors. */
        ray_t* keys_lst = ray_list_new(ngroups);
        if (RAY_IS_ERR(keys_lst)) { ray_free(kblock); goto gfail; }
        ray_t* vals_lst = ray_list_new(ngroups);
        if (RAY_IS_ERR(vals_lst)) { ray_release(keys_lst); ray_free(kblock); goto gfail; }
        for (int64_t g = 0; g < ngroups; g++) {
            keys_lst = ray_list_append(keys_lst, gkeys[g]);
            if (RAY_IS_ERR(keys_lst)) { ray_release(vals_lst); ray_free(kblock); goto gfail; }
            vals_lst = ray_list_append(vals_lst, idx_vecs[g]);
            ray_release(idx_vecs[g]);
            idx_vecs[g] = NULL;
            if (RAY_IS_ERR(vals_lst)) { ray_release(keys_lst); ray_free(kblock); goto gfail; }
        }
        ray_free(val_block); ray_free(ivblock); ray_free(kblock);
        return ray_dict_new(keys_lst, vals_lst);
    }

    /* RAY_GUID: 16-byte fixed-width grouping via open-address hash set
     * keyed on the guid bytes.  Previously this was an O(N²) linear
     * scan against every existing group, which made (group guid_col)
     * and (select ... by: OrderId) on a 10M row table effectively
     * infinite. */
    if (x->type == RAY_GUID) {
        const uint8_t* base = (const uint8_t*)ray_data(x);
        group_ht_t ht;
        uint32_t seed_cap = (uint32_t)(n < 64 ? 64 : (n < 1048576 ? (n * 2) : 2097152));
        if (!group_ht_init(&ht, seed_cap)) {
            ray_free(val_block); ray_free(ivblock);
            return ray_error("oom", NULL);
        }
        ght_guid_ctx_t gctx = { .base = base, .gvals = gvals };
        ray_progress_update("group", "guid-scan", 0, (uint64_t)n);
        for (int64_t i = 0; i < n; i++) {
            if (((i) & 65535) == 0) {
                if (ray_interrupted()) {
                    for (int64_t g = 0; g < ngroups; g++) ray_release(idx_vecs[g]);
                    group_ht_free(&ht);
                    ray_free(val_block); ray_free(ivblock);
                    return ray_error("cancel", "interrupted");
                }
                ray_progress_update(NULL, NULL, (uint64_t)i, (uint64_t)n);
            }
            const uint8_t* cur = base + i * 16;
            uint64_t h = hash_guid(cur);
            uint32_t slot = (uint32_t)(h & ht.mask);
            uint32_t gi_found = GHT_EMPTY;
            while (ht.slots[slot] != GHT_EMPTY) {
                uint32_t gi = ht.slots[slot];
                if (memcmp(base + gvals[gi] * 16, cur, 16) == 0) {
                    gi_found = gi;
                    break;
                }
                slot = (slot + 1) & ht.mask;
            }
            int64_t gi;
            if (gi_found != GHT_EMPTY) {
                gi = gi_found;
            } else {
                if (ngroups >= max_groups) {
                    if (!group_grow(&val_block, &ivblock, &gvals, &idx_vecs,
                                    ngroups, &max_groups)) {
                        for (int64_t g = 0; g < ngroups; g++) ray_release(idx_vecs[g]);
                        group_ht_free(&ht);
                        ray_free(val_block); ray_free(ivblock);
                        return ray_error("oom", NULL);
                    }
                    gctx.gvals = gvals;
                }
                gi = ngroups++;
                gvals[gi] = i;  /* store row index of first occurrence */
                idx_vecs[gi] = ray_vec_new(RAY_I64, 0);
                ht.slots[slot] = (uint32_t)gi;
                ht.count++;
                /* Grow at load factor 0.5 */
                if (ht.count * 2 > ht.cap) {
                    if (!group_ht_grow(&ht, ght_guid_hash_gi, &gctx)) {
                        for (int64_t g = 0; g < ngroups; g++) ray_release(idx_vecs[g]);
                        group_ht_free(&ht);
                        ray_free(val_block); ray_free(ivblock);
                        return ray_error("oom", NULL);
                    }
                }
            }
            idx_vecs[gi] = ray_vec_append(idx_vecs[gi], &i);
        }
        group_ht_free(&ht);
        /* Keys: dense GUID vector built from collected gvals; vals: LIST of idx vecs. */
        ray_t* keys_vec = ray_vec_new(RAY_GUID, ngroups);
        if (RAY_IS_ERR(keys_vec)) goto gfail;
        for (int64_t g = 0; g < ngroups; g++)
            keys_vec = ray_vec_append(keys_vec, base + gvals[g] * 16);
        if (RAY_IS_ERR(keys_vec)) goto gfail;
        ray_t* vals_lst = ray_list_new(ngroups);
        if (RAY_IS_ERR(vals_lst)) { ray_release(keys_vec); goto gfail; }
        for (int64_t g = 0; g < ngroups; g++) {
            vals_lst = ray_list_append(vals_lst, idx_vecs[g]);
            ray_release(idx_vecs[g]);
            idx_vecs[g] = NULL;
            if (RAY_IS_ERR(vals_lst)) { ray_release(keys_vec); goto gfail; }
        }
        ray_free(val_block); ray_free(ivblock);
        return ray_dict_new(keys_vec, vals_lst);
    }

    /* RAY_STR: string-based grouping using ray_str_vec_get */
    if (x->type == RAY_STR) {
        /* Store group keys as (ptr, len) pairs -- use a scratch block for strings */
        ray_t* skblock = ray_alloc((size_t)(max_groups * sizeof(ray_t*)));
        if (RAY_IS_ERR(skblock)) { ray_free(val_block); ray_free(ivblock); return skblock; }
        ray_t** str_keys = (ray_t**)ray_data(skblock);

        for (int64_t i = 0; i < n; i++) {
            size_t slen = 0;
            const char* sp = ray_str_vec_get(x, i, &slen);

            int64_t gi = -1;
            for (int64_t g = 0; g < ngroups; g++) {
                size_t gsl = ray_str_len(str_keys[g]);
                const char* gsp = ray_str_ptr(str_keys[g]);
                if (gsl == slen && (slen == 0 || memcmp(gsp, sp, slen) == 0)) {
                    gi = g; break;
                }
            }
            if (gi < 0) {
                if (ngroups >= max_groups) {
                    for (int64_t g = 0; g < ngroups; g++) {
                        ray_release(str_keys[g]);
                        ray_release(idx_vecs[g]);
                    }
                    ray_free(val_block); ray_free(ivblock); ray_free(skblock);
                    return ray_error("limit", NULL);
                }
                gi = ngroups++;
                str_keys[gi] = ray_str(sp ? sp : "", slen);
                idx_vecs[gi] = ray_vec_new(RAY_I64, 0);
            }
            idx_vecs[gi] = ray_vec_append(idx_vecs[gi], &i);
        }

        /* Build dict: keys as RAY_STR vec from str_keys, vals as LIST of idx vecs. */
        ray_t* keys_vec = ray_vec_new(RAY_STR, ngroups);
        if (RAY_IS_ERR(keys_vec)) {
            for (int64_t g = 0; g < ngroups; g++) {
                ray_release(str_keys[g]);
                ray_release(idx_vecs[g]);
            }
            ray_free(val_block); ray_free(ivblock); ray_free(skblock);
            return ray_error("domain", NULL);
        }
        for (int64_t g = 0; g < ngroups; g++) {
            keys_vec = ray_str_vec_append(keys_vec, ray_str_ptr(str_keys[g]), ray_str_len(str_keys[g]));
            ray_release(str_keys[g]);
        }
        ray_t* vals_lst = ray_list_new(ngroups);
        if (RAY_IS_ERR(vals_lst)) {
            ray_release(keys_vec); ray_free(skblock); goto gfail;
        }
        for (int64_t g = 0; g < ngroups; g++) {
            vals_lst = ray_list_append(vals_lst, idx_vecs[g]);
            ray_release(idx_vecs[g]);
            idx_vecs[g] = NULL;
            if (RAY_IS_ERR(vals_lst)) { ray_release(keys_vec); ray_free(skblock); goto gfail; }
        }
        ray_free(val_block); ray_free(ivblock); ray_free(skblock);
        return ray_dict_new(keys_vec, vals_lst);
    }

    /* Scalar fast path: every primitive-typed vector packs its group
     * key into an int64 (sym id, raw integer, date/time/timestamp, bool).
     * Use an open-address hash set so high-cardinality group-by stays
     * linear in n rather than the historical O(N²) per-row linear scan. */
    group_ht_t ht;
    uint32_t seed_cap = (uint32_t)(n < 64 ? 64 : (n < 1048576 ? (n * 2) : 2097152));
    if (!group_ht_init(&ht, seed_cap)) {
        ray_free(val_block); ray_free(ivblock);
        return ray_error("oom", NULL);
    }
    ght_i64_ctx_t sctx = { .gvals = gvals };
    /* Null routing: null inputs share the same storage value as a legitimate
     * zero/sentinel (e.g. NULL_I64's atom stores i64=0, NULL_I32 stores
     * i32=0).  Without a separate null bucket the hash table would conflate
     * `0Nl` with a real `0`, silently merging two semantically distinct
     * groups.  Track a single `null_gi` and route every null row there;
     * non-null rows continue to use the value-keyed hash table. */
    int64_t null_gi = -1;
    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(x, i)) {
            if (null_gi < 0) {
                if (ngroups >= max_groups) {
                    if (!group_grow(&val_block, &ivblock, &gvals, &idx_vecs,
                                    ngroups, &max_groups)) {
                        for (int64_t g = 0; g < ngroups; g++) ray_release(idx_vecs[g]);
                        group_ht_free(&ht);
                        ray_free(val_block); ray_free(ivblock);
                        return ray_error("oom", NULL);
                    }
                    sctx.gvals = gvals;
                }
                null_gi = ngroups++;
                gvals[null_gi] = 0;          /* placeholder; key value set later */
                idx_vecs[null_gi] = ray_vec_new(RAY_I64, 0);
            }
            idx_vecs[null_gi] = ray_vec_append(idx_vecs[null_gi], &i);
            continue;
        }
        int64_t v;
        if (x->type == RAY_SYM || x->type == RAY_I64 || x->type == RAY_TIMESTAMP)
            v = ((int64_t*)ray_data(x))[i];
        else if (x->type == RAY_I32 || x->type == RAY_DATE || x->type == RAY_TIME)
            v = ((int32_t*)ray_data(x))[i];
        else if (x->type == RAY_I16)
            v = ((int16_t*)ray_data(x))[i];
        else if (x->type == RAY_BOOL || x->type == RAY_U8)
            v = ((uint8_t*)ray_data(x))[i];
        else if (x->type == RAY_F64 || x->type == RAY_F32) {
            /* Hash by IEEE-754 bit pattern, not row index — the previous
             * `v = i` fallback put every float row in its own group and
             * the keys_vec build path then reinterpreted those row
             * indices as floats.  Two adjustments keep the bit-pattern
             * approach consistent with atom_eq's IEEE semantics
             * (`a->f64 == b->f64`):
             *   - +0.0 and -0.0 hash equal: canonicalise -0.0 to 0.0.
             *   - Each NaN is its own group (NaN != NaN under IEEE).
             *     Route NaN rows through the dedicated nan-group path
             *     below so the hash table never matches them. */
            double f = (x->type == RAY_F64)
                ? ((double*)ray_data(x))[i]
                : (double)((float*)ray_data(x))[i];
            if (f != f) {
                /* NaN — own bucket per row, just like the null routing. */
                if (ngroups >= max_groups) {
                    if (!group_grow(&val_block, &ivblock, &gvals, &idx_vecs,
                                    ngroups, &max_groups)) {
                        for (int64_t g = 0; g < ngroups; g++) ray_release(idx_vecs[g]);
                        group_ht_free(&ht);
                        ray_free(val_block); ray_free(ivblock);
                        return ray_error("oom", NULL);
                    }
                    sctx.gvals = gvals;
                }
                int64_t gi_nan = ngroups++;
                memcpy(&gvals[gi_nan], &f, sizeof(f));
                idx_vecs[gi_nan] = ray_vec_new(RAY_I64, 0);
                idx_vecs[gi_nan] = ray_vec_append(idx_vecs[gi_nan], &i);
                continue;
            }
            if (f == 0.0) f = 0.0;   /* canonicalise -0.0 → +0.0 */
            memcpy(&v, &f, sizeof(v));
        } else
            v = i;

        uint64_t h = hash_i64(v);
        uint32_t slot = (uint32_t)(h & ht.mask);
        uint32_t gi_found = GHT_EMPTY;
        while (ht.slots[slot] != GHT_EMPTY) {
            uint32_t gi = ht.slots[slot];
            if (gvals[gi] == v) { gi_found = gi; break; }
            slot = (slot + 1) & ht.mask;
        }
        int64_t gi;
        if (gi_found != GHT_EMPTY) {
            gi = gi_found;
        } else {
            if (ngroups >= max_groups) {
                if (!group_grow(&val_block, &ivblock, &gvals, &idx_vecs,
                                ngroups, &max_groups)) {
                    for (int64_t g = 0; g < ngroups; g++) ray_release(idx_vecs[g]);
                    group_ht_free(&ht);
                    ray_free(val_block); ray_free(ivblock);
                    return ray_error("oom", NULL);
                }
                sctx.gvals = gvals;
            }
            gi = ngroups++;
            gvals[gi] = v;
            idx_vecs[gi] = ray_vec_new(RAY_I64, 0);
            ht.slots[slot] = (uint32_t)gi;
            ht.count++;
            if (ht.count * 2 > ht.cap) {
                if (!group_ht_grow(&ht, ght_i64_hash_gi, &sctx)) {
                    for (int64_t g = 0; g < ngroups; g++) ray_release(idx_vecs[g]);
                    group_ht_free(&ht);
                    ray_free(val_block); ray_free(ivblock);
                    return ray_error("oom", NULL);
                }
            }
        }
        idx_vecs[gi] = ray_vec_append(idx_vecs[gi], &i);
    }
    group_ht_free(&ht);

    /* Build dict: keys vec mirrors x's element type; vals LIST of idx vecs. */
    int8_t key_type = x->type;
    ray_t* keys_vec;
    if (key_type == RAY_SYM) keys_vec = ray_sym_vec_new(RAY_SYM_W64, ngroups);
    else                     keys_vec = ray_vec_new(key_type, ngroups);
    if (RAY_IS_ERR(keys_vec)) goto gfail;

    for (int64_t g = 0; g < ngroups; g++) {
        switch (key_type) {
            case RAY_SYM:
            case RAY_I64:
            case RAY_TIMESTAMP: {
                int64_t v = gvals[g];
                keys_vec = ray_vec_append(keys_vec, &v); break;
            }
            case RAY_I32:
            case RAY_DATE:
            case RAY_TIME: {
                int32_t v = (int32_t)gvals[g];
                keys_vec = ray_vec_append(keys_vec, &v); break;
            }
            case RAY_I16: { int16_t v = (int16_t)gvals[g]; keys_vec = ray_vec_append(keys_vec, &v); break; }
            case RAY_BOOL:
            case RAY_U8:  { uint8_t v = (uint8_t)gvals[g]; keys_vec = ray_vec_append(keys_vec, &v); break; }
            case RAY_F64: {
                /* gvals[g] holds the IEEE-754 bit pattern packed by the
                 * row-loop above; reinterpret rather than int->double
                 * cast (which would produce 0.0/1.0/2.0… instead of the
                 * actual float values). */
                double v;
                memcpy(&v, &gvals[g], sizeof(v));
                keys_vec = ray_vec_append(keys_vec, &v);
                break;
            }
            case RAY_F32: {
                double f;
                memcpy(&f, &gvals[g], sizeof(f));
                float  v = (float)f;
                keys_vec = ray_vec_append(keys_vec, &v);
                break;
            }
            default:      keys_vec = ray_vec_append(keys_vec, &gvals[g]); break;
        }
        if (RAY_IS_ERR(keys_vec)) goto gfail;
        /* If the source column had a null at any row in this group, mark
         * the group's key as null so dict rendering / lookup can recover
         * the null semantics (the integer-value key alone collides with a
         * legitimate zero/sentinel value).  All rows in a value-equality
         * group share the same null-or-not status, so a single probe of
         * the first row index suffices. */
        if (idx_vecs[g] && idx_vecs[g]->len > 0) {
            int64_t first_row = ((int64_t*)ray_data(idx_vecs[g]))[0];
            if (ray_vec_is_null(x, first_row))
                ray_vec_set_null(keys_vec, g, true);
        }
    }

    ray_t* vals_lst = ray_list_new(ngroups);
    if (RAY_IS_ERR(vals_lst)) { ray_release(keys_vec); goto gfail; }
    for (int64_t g = 0; g < ngroups; g++) {
        vals_lst = ray_list_append(vals_lst, idx_vecs[g]);
        ray_release(idx_vecs[g]);
        idx_vecs[g] = NULL;
        if (RAY_IS_ERR(vals_lst)) { ray_release(keys_vec); goto gfail; }
    }
    ray_free(val_block);
    ray_free(ivblock);
    return ray_dict_new(keys_vec, vals_lst);

gfail:
    for (int64_t g = 0; g < ngroups; g++)
        if (idx_vecs[g]) ray_release(idx_vecs[g]);
    ray_free(val_block);
    ray_free(ivblock);
    return ray_error("domain", NULL);
}

/* (concat a b) -> concatenate vectors/strings/dicts/tables */
ray_t* ray_concat_fn(ray_t* a, ray_t* b) {
    /* Helper: get string content from atom (STR or CHAR), stripping trailing nulls */
    {
        int a_is_str = ray_is_atom(a) && ((-a->type) == RAY_STR);
        int b_is_str = ray_is_atom(b) && ((-b->type) == RAY_STR);
        if (a_is_str && b_is_str) {
            const char *ap, *bp;
            size_t la, lb;
            ap = ray_str_ptr(a); la = ray_str_len(a);
            bp = ray_str_ptr(b); lb = ray_str_len(b);
            /* Strip trailing null bytes */
            while (la > 0 && ap[la - 1] == '\0') la--;
            while (lb > 0 && bp[lb - 1] == '\0') lb--;
            char buf[8192];
            if (la + lb > sizeof(buf)) return ray_error("limit", NULL);
            memcpy(buf, ap, la);
            memcpy(buf + la, bp, lb);
            return ray_str(buf, la + lb);
        }
    }
    /* Vector concat: same type — delegate to ray_vec_concat which handles
     * null bitmap propagation, SYM width promotion, and STR pool merging. */
    if (ray_is_vec(a) && ray_is_vec(b) && a->type == b->type)
        return ray_vec_concat(a, b);
    /* Concat typed vec + boxed list or boxed list + typed vec -> boxed list */
    if ((ray_is_vec(a) && b->type == RAY_LIST) || (a->type == RAY_LIST && ray_is_vec(b))) {
        ray_t* la = (a->type == RAY_LIST) ? a : NULL;
        ray_t* lb = (b->type == RAY_LIST) ? b : NULL;
        ray_t* va = ray_is_vec(a) ? a : NULL;
        ray_t* vb = ray_is_vec(b) ? b : NULL;
        int64_t na = a->len, nb = b->len;
        ray_t* result = ray_alloc((na + nb) * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = na + nb;
        ray_t** out = (ray_t**)ray_data(result);
        for (int64_t i = 0; i < na; i++) {
            if (va) {
                int alloc = 0;
                out[i] = collection_elem(va, i, &alloc);
            } else {
                out[i] = ((ray_t**)ray_data(la))[i];
                ray_retain(out[i]);
            }
        }
        for (int64_t i = 0; i < nb; i++) {
            if (vb) {
                int alloc = 0;
                out[na + i] = collection_elem(vb, i, &alloc);
            } else {
                out[na + i] = ((ray_t**)ray_data(lb))[i];
                ray_retain(out[na + i]);
            }
        }
        return result;
    }
    /* Boxed list concat */
    if (a->type == RAY_LIST && b->type == RAY_LIST) {
        int64_t na = a->len, nb = b->len;
        ray_t* result = ray_alloc((na + nb) * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = na + nb;
        ray_t** out = (ray_t**)ray_data(result);
        ray_t** ae = (ray_t**)ray_data(a);
        ray_t** be = (ray_t**)ray_data(b);
        for (int64_t i = 0; i < na; i++) { ray_retain(ae[i]); out[i] = ae[i]; }
        for (int64_t i = 0; i < nb; i++) { ray_retain(be[i]); out[na + i] = be[i]; }
        return result;
    }
    /* Vector concat: mixed types -> boxed list (preserves original element types) */
    if (ray_is_vec(a) && ray_is_vec(b) && a->type != b->type) {
        int64_t na = a->len, nb = b->len;
        ray_t* result = ray_alloc((na + nb) * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = na + nb;
        ray_t** out = (ray_t**)ray_data(result);
        for (int64_t i = 0; i < na; i++) {
            int alloc = 0;
            out[i] = collection_elem(a, i, &alloc);
            /* collection_elem always allocates for typed vecs, so ownership transfers */
        }
        for (int64_t i = 0; i < nb; i++) {
            int alloc = 0;
            out[na + i] = collection_elem(b, i, &alloc);
        }
        return result;
    }
    /* Atom + vector or vector + atom -> append */
    if (ray_is_atom(a) && ray_is_vec(b) && (-a->type) == b->type) {
        int64_t nb = b->len;
        int esz = ray_elem_size(b->type);
        ray_t* result = ray_vec_new(b->type, 1 + nb);
        if (RAY_IS_ERR(result)) return result;
        /* Copy atom value as first element */
        switch (b->type) {
        case RAY_I64: case RAY_TIMESTAMP: case RAY_SYM:
            ((int64_t*)ray_data(result))[0] = a->i64; break;
        case RAY_F64:
            ((double*)ray_data(result))[0] = a->f64; break;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            ((int32_t*)ray_data(result))[0] = a->i32; break;
        case RAY_I16:
            ((int16_t*)ray_data(result))[0] = a->i16; break;
        case RAY_BOOL:
            ((bool*)ray_data(result))[0] = a->b8; break;
        case RAY_U8:
            ((uint8_t*)ray_data(result))[0] = a->u8; break;
        case RAY_GUID: {
            const uint8_t* gd = a->obj ? (const uint8_t*)ray_data(a->obj) : (const uint8_t*)ray_data((ray_t*)a);
            memcpy(ray_data(result), gd, 16); break;
        }
        default: ray_free(result); return ray_error("type", NULL);
        }
        memcpy((char*)ray_data(result) + esz, ray_data(b), (size_t)(nb * esz));
        result->len = 1 + nb;
        return result;
    }
    if (ray_is_vec(a) && ray_is_atom(b) && a->type == (-b->type)) {
        int64_t na = a->len;
        int esz = ray_elem_size(a->type);
        ray_t* result = ray_vec_new(a->type, na + 1);
        if (RAY_IS_ERR(result)) return result;
        memcpy(ray_data(result), ray_data(a), (size_t)(na * esz));
        switch (a->type) {
        case RAY_I64: case RAY_TIMESTAMP: case RAY_SYM:
            ((int64_t*)ray_data(result))[na] = b->i64; break;
        case RAY_F64:
            ((double*)ray_data(result))[na] = b->f64; break;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            ((int32_t*)ray_data(result))[na] = b->i32; break;
        case RAY_I16:
            ((int16_t*)ray_data(result))[na] = b->i16; break;
        case RAY_BOOL:
            ((bool*)ray_data(result))[na] = b->b8; break;
        case RAY_U8:
            ((uint8_t*)ray_data(result))[na] = b->u8; break;
        case RAY_GUID: {
            const uint8_t* gd = b->obj ? (const uint8_t*)ray_data(b->obj) : (const uint8_t*)ray_data((ray_t*)b);
            memcpy((uint8_t*)ray_data(result) + na * 16, gd, 16); break;
        }
        default: ray_free(result); return ray_error("type", NULL);
        }
        result->len = na + 1;
        return result;
    }
    /* Atom + atom of same type -> 2-element vector */
    if (ray_is_atom(a) && ray_is_atom(b) && a->type == b->type && a->type != -RAY_STR) {
        int8_t vtype = -(a->type);
        ray_t* result = ray_vec_new(vtype, 2);
        if (RAY_IS_ERR(result)) return result;
        result->len = 2;
        switch (vtype) {
        case RAY_I64: case RAY_TIMESTAMP: case RAY_SYM:
            ((int64_t*)ray_data(result))[0] = a->i64;
            ((int64_t*)ray_data(result))[1] = b->i64;
            break;
        case RAY_F64:
            ((double*)ray_data(result))[0] = a->f64;
            ((double*)ray_data(result))[1] = b->f64;
            break;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            ((int32_t*)ray_data(result))[0] = a->i32;
            ((int32_t*)ray_data(result))[1] = b->i32;
            break;
        case RAY_I16:
            ((int16_t*)ray_data(result))[0] = a->i16;
            ((int16_t*)ray_data(result))[1] = b->i16;
            break;
        case RAY_BOOL:
            ((bool*)ray_data(result))[0] = a->b8;
            ((bool*)ray_data(result))[1] = b->b8;
            break;
        case RAY_U8:
            ((uint8_t*)ray_data(result))[0] = a->u8;
            ((uint8_t*)ray_data(result))[1] = b->u8;
            break;
        case RAY_GUID: {
            const uint8_t* ga = a->obj ? (const uint8_t*)ray_data(a->obj) : (const uint8_t*)ray_data((ray_t*)a);
            const uint8_t* gb = b->obj ? (const uint8_t*)ray_data(b->obj) : (const uint8_t*)ray_data((ray_t*)b);
            memcpy(ray_data(result), ga, 16);
            memcpy((uint8_t*)ray_data(result) + 16, gb, 16);
            break;
        }
        default: ray_free(result); return ray_error("type", NULL);
        }
        return result;
    }
    /* Dict concat: merge — keys/vals from b overwrite a's. */
    if (a->type == RAY_DICT && b->type == RAY_DICT) {
        ray_retain(a);
        ray_t* out = a;
        ray_t* bk = ray_dict_keys(b);
        ray_t* bv = ray_dict_vals(b);
        if (!bk || !bv) return out;
        int64_t bn = bk->len;
        for (int64_t i = 0; i < bn; i++) {
            /* Synthesize a key atom view from bk and the value pointer from bv. */
            ray_t k_storage; memset(&k_storage, 0, sizeof(k_storage));
            ray_t* k = NULL;
            if (bk->type == RAY_LIST) {
                k = ((ray_t**)ray_data(bk))[i];
            } else if (bk->type == RAY_SYM) {
                k_storage.type = -RAY_SYM;
                k_storage.i64  = ray_read_sym(ray_data(bk), i, RAY_SYM, bk->attrs);
                k = &k_storage;
            } else if (bk->type == RAY_I64 || bk->type == RAY_TIMESTAMP) {
                k_storage.type = -bk->type;
                k_storage.i64  = ((int64_t*)ray_data(bk))[i];
                k = &k_storage;
            } else {
                /* Heterogeneous element types fall back to boxing via collection_elem. */
                int alloc = 0;
                k = collection_elem(bk, i, &alloc);
                ray_t* v;
                if (bv->type == RAY_LIST) v = ((ray_t**)ray_data(bv))[i];
                else { int va = 0; v = collection_elem(bv, i, &va); (void)va; }
                out = ray_dict_upsert(out, k, v);
                if (alloc) ray_release(k);
                if (!out || RAY_IS_ERR(out)) return out;
                continue;
            }
            ray_t* v;
            if (bv->type == RAY_LIST) v = ((ray_t**)ray_data(bv))[i];
            else { int va = 0; v = collection_elem(bv, i, &va); (void)va; }
            out = ray_dict_upsert(out, k, v);
            if (!out || RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    /* Table concat: append rows */
    if (a->type == RAY_TABLE && b->type == RAY_TABLE) {
        int64_t ncols_a = ray_table_ncols(a);
        int64_t ncols_b = ray_table_ncols(b);
        /* Match columns of a in b by name */
        ray_t* result = ray_table_new((int32_t)ncols_a);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t c = 0; c < ncols_a; c++) {
            int64_t col_name_a = ray_table_col_name(a, c);
            ray_t* acol = ray_table_get_col_idx(a, c);
            /* Find matching column in b by name */
            ray_t* bcol = NULL;
            for (int64_t j = 0; j < ncols_b; j++) {
                if (ray_table_col_name(b, j) == col_name_a) {
                    bcol = ray_table_get_col_idx(b, j);
                    break;
                }
            }
            if (!bcol) {
                /* Column not present in b — schema mismatch is a "value"
                 * error (the table values have incompatible columns), not
                 * a "domain" error (which semantically means out-of-range). */
                ray_release(result);
                return ray_error("value", NULL);
            }
            /* Type check: columns must have the same type */
            if (acol->type != bcol->type) {
                ray_release(result);
                return ray_error("type", NULL);
            }
            ray_t* col = ray_concat_fn(acol, bcol);
            if (RAY_IS_ERR(col)) { ray_release(result); return col; }
            result = ray_table_add_col(result, col_name_a, col);
            ray_release(col);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    /* Atom + boxed list -> prepend atom to list */
    if (ray_is_atom(a) && b->type == RAY_LIST && b->type != RAY_DICT) {
        int64_t nb = b->len;
        ray_t* result = ray_alloc((1 + nb) * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = 1 + nb;
        ray_t** out = (ray_t**)ray_data(result);
        ray_retain(a);
        out[0] = a;
        ray_t** be = (ray_t**)ray_data(b);
        for (int64_t i = 0; i < nb; i++) { ray_retain(be[i]); out[1 + i] = be[i]; }
        return result;
    }
    /* Boxed list + atom -> append atom to list */
    if (a->type == RAY_LIST && a->type != RAY_DICT && ray_is_atom(b)) {
        int64_t na = a->len;
        ray_t* result = ray_alloc((na + 1) * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = na + 1;
        ray_t** out = (ray_t**)ray_data(result);
        ray_t** ae = (ray_t**)ray_data(a);
        for (int64_t i = 0; i < na; i++) { ray_retain(ae[i]); out[i] = ae[i]; }
        ray_retain(b);
        out[na] = b;
        return result;
    }
    /* Atom + atom of different types -> 2-element boxed list */
    if (ray_is_atom(a) && ray_is_atom(b) && a->type != b->type) {
        ray_t* result = ray_alloc(2 * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = 2;
        ray_t** out = (ray_t**)ray_data(result);
        ray_retain(a); out[0] = a;
        ray_retain(b); out[1] = b;
        return result;
    }
    return ray_error("type", NULL);
}

/* (raze list-of-vecs) -> flattened vector */
ray_t* ray_raze_fn(ray_t* x) {
    /* Scalar passthrough */
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    /* Typed vector passthrough */
    if (ray_is_vec(x)) { ray_retain(x); return x; }
    if (x->type != RAY_LIST)
        return ray_error("type", NULL);
    int64_t n = x->len;
    if (n == 0) return ray_list_new(0);
    ray_t** items = (ray_t**)ray_data(x);
    /* Try to concat all items */
    ray_t* result = items[0];
    ray_retain(result);
    for (int64_t i = 1; i < n; i++) {
        ray_t* next = ray_concat_fn(result, items[i]);
        ray_release(result);
        if (RAY_IS_ERR(next)) return next;
        result = next;
    }
    return result;
}

/* (within vals [lo hi]) -> bool vector, true where lo <= val <= hi */
ray_t* ray_within_fn(ray_t* vals, ray_t* range) {
    if (!ray_is_vec(vals) || !ray_is_vec(range) || range->len != 2)
        return ray_error("type", NULL);
    int64_t n = vals->len;
    ray_t* result = ray_vec_new(RAY_BOOL, n);
    if (RAY_IS_ERR(result)) return result;
    bool* out = (bool*)ray_data(result);

    if (vals->type == RAY_I64) {
        int64_t* d = (int64_t*)ray_data(vals);
        int64_t* r = (int64_t*)ray_data(range);
        int64_t lo = r[0], hi = r[1];
        for (int64_t i = 0; i < n; i++) out[i] = (d[i] >= lo && d[i] <= hi);
    } else if (vals->type == RAY_F64) {
        double* d = (double*)ray_data(vals);
        double* r = (double*)ray_data(range);
        double lo = r[0], hi = r[1];
        for (int64_t i = 0; i < n; i++) out[i] = (d[i] >= lo && d[i] <= hi);
    } else if (vals->type == RAY_I32 || vals->type == RAY_DATE || vals->type == RAY_TIME) {
        int32_t* d = (int32_t*)ray_data(vals);
        int32_t* r = (int32_t*)ray_data(range);
        int32_t lo = r[0], hi = r[1];
        for (int64_t i = 0; i < n; i++) out[i] = (d[i] >= lo && d[i] <= hi);
    } else {
        ray_free(result);
        return ray_error("type", NULL);
    }
    result->len = n;
    return result;
}

/* (div a b) -> float division (always returns f64) */
ray_t* ray_fdiv_fn(ray_t* a, ray_t* b) {
    if (!ray_is_atom(a) || !ray_is_atom(b)) return ray_error("type", NULL);
    if (!is_numeric(a) || !is_numeric(b)) return ray_error("type", NULL);
    /* Null propagation */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_F64);
    double fa = as_f64(a), fb = as_f64(b);
    if (fb == 0.0) return ray_typed_null(-RAY_F64);
    return make_f64(fa / fb);
}
