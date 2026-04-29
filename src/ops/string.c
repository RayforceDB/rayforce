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

#include "ops/internal.h"
#include "ops/glob.h"

/* ============================================================================
 * OP_LIKE: glob pattern matching on STR / SYM columns.  See ops/glob.[ch].
 * Syntax: * (any), ? (one char), [abc] / [a-z] / [!abc] (character class).
 * ============================================================================ */

ray_t* exec_like(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* pat_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (pat_v && !RAY_IS_ERR(pat_v)) ray_release(pat_v); return input; }
    if (!pat_v || RAY_IS_ERR(pat_v)) { ray_release(input); return pat_v; }

    /* Get pattern string */
    const char* pat_str = ray_str_ptr(pat_v);
    size_t pat_len = ray_str_len(pat_v);

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_BOOL, len);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(input); ray_release(pat_v);
        return result;
    }
    result->len = len;
    uint8_t* dst = (uint8_t*)ray_data(result);

    int8_t in_type = input->type;
    if (in_type == RAY_STR) {
        const ray_str_t* elems; const char* pool;
        str_resolve(input, &elems, &pool);
        for (int64_t i = 0; i < len; i++) {
            const char* sp = ray_str_t_ptr(&elems[i], pool);
            size_t sl = elems[i].len;
            dst[i] = ray_glob_match(sp, sl, pat_str, pat_len) ? 1 : 0;
        }
    } else if (RAY_IS_SYM(in_type)) {
        const void* base = ray_data(input);
        for (int64_t i = 0; i < len; i++) {
            int64_t sym_id = ray_read_sym(base, i, in_type, input->attrs);
            ray_t* s = ray_sym_str(sym_id);
            if (!s) { dst[i] = 0; continue; }
            const char* sp = ray_str_ptr(s);
            size_t sl = ray_str_len(s);
            dst[i] = ray_glob_match(sp, sl, pat_str, pat_len) ? 1 : 0;
        }
    } else {
        memset(dst, 0, (size_t)len);
    }

    ray_release(input); ray_release(pat_v);
    return result;
}

/* Case-insensitive LIKE — same syntax as `like`, ASCII-fold both sides. */

ray_t* exec_ilike(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* pat_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (pat_v && !RAY_IS_ERR(pat_v)) ray_release(pat_v); return input; }
    if (!pat_v || RAY_IS_ERR(pat_v)) { ray_release(input); return pat_v; }

    const char* pat_str = ray_str_ptr(pat_v);
    size_t pat_len = ray_str_len(pat_v);

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_BOOL, len);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(input); ray_release(pat_v);
        return result;
    }
    result->len = len;
    uint8_t* dst = (uint8_t*)ray_data(result);

    int8_t in_type = input->type;
    if (in_type == RAY_STR) {
        const ray_str_t* elems; const char* pool;
        str_resolve(input, &elems, &pool);
        for (int64_t i = 0; i < len; i++) {
            const char* sp = ray_str_t_ptr(&elems[i], pool);
            size_t sl = elems[i].len;
            dst[i] = ray_glob_match_ci(sp, sl, pat_str, pat_len) ? 1 : 0;
        }
    } else if (RAY_IS_SYM(in_type)) {
        const void* base = ray_data(input);
        for (int64_t i = 0; i < len; i++) {
            int64_t sym_id = ray_read_sym(base, i, in_type, input->attrs);
            ray_t* s = ray_sym_str(sym_id);
            if (!s) { dst[i] = 0; continue; }
            dst[i] = ray_glob_match_ci(ray_str_ptr(s), ray_str_len(s), pat_str, pat_len) ? 1 : 0;
        }
    } else {
        memset(dst, 0, (size_t)len);
    }

    ray_release(input); ray_release(pat_v);
    return result;
}

/* ============================================================================
 * String functions: UPPER, LOWER, TRIM, STRLEN, SUBSTR, REPLACE, CONCAT
 *
 * These functions call ray_sym_intern() per output row, which is
 * O(n * sym_table_lookup) per string op.  Acceptable for current workloads;
 * could be optimized with batch interning if profiling shows a bottleneck.
 * ============================================================================ */

/* UPPER / LOWER / TRIM — unary SYM/STR → SYM/STR */
ray_t* exec_string_unary(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;

    int64_t len = input->len;
    bool is_str = (input->type == RAY_STR);

    ray_t* result;
    if (is_str) {
        result = ray_vec_new(RAY_STR, len);
    } else {
        result = ray_vec_new(RAY_SYM, len);
    }
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    if (!is_str) result->len = len;
    int64_t* sym_dst = is_str ? NULL : (int64_t*)ray_data(result);

    const ray_str_t* str_elems = NULL;
    const char* str_pool = NULL;
    if (is_str) str_resolve(input, &str_elems, &str_pool);

    uint16_t opc = op->opcode;
    for (int64_t i = 0; i < len; i++) {
        /* Propagate null */
        if (ray_vec_is_null((ray_t*)input, i)) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                sym_dst[i] = 0;
                ray_vec_set_null(result, i, true);
            }
            continue;
        }
        const char* sp; size_t sl;
        if (is_str) {
            sp = ray_str_t_ptr(&str_elems[i], str_pool);
            sl = str_elems[i].len;
        } else {
            sym_elem(input, i, &sp, &sl);
        }

        char sbuf[8192];
        char* buf = sbuf;
        ray_t* dyn_hdr = NULL;
        if (sl >= sizeof(sbuf)) {
            buf = (char*)scratch_alloc(&dyn_hdr, sl + 1);
            if (!buf) {
                ray_release(result);
                ray_release(input);
                return ray_error("oom", NULL);
            }
        }
        size_t out_len = sl;
        if (opc == OP_UPPER) {
            for (size_t j = 0; j < out_len; j++) buf[j] = (char)toupper((unsigned char)sp[j]);
        } else if (opc == OP_LOWER) {
            for (size_t j = 0; j < out_len; j++) buf[j] = (char)tolower((unsigned char)sp[j]);
        } else { /* OP_TRIM */
            size_t start = 0, end = sl;
            while (start < sl && isspace((unsigned char)sp[start])) start++;
            while (end > start && isspace((unsigned char)sp[end - 1])) end--;
            out_len = end - start;
            memcpy(buf, sp + start, out_len);
        }

        if (is_str) {
            ray_t* prev = result;
            result = ray_str_vec_append(result, buf, out_len);
            if (RAY_IS_ERR(result)) { ray_release(prev); scratch_free(dyn_hdr); break; }
        } else {
            buf[out_len] = '\0';
            sym_dst[i] = ray_sym_intern(buf, out_len);
        }
        scratch_free(dyn_hdr);
    }
    ray_release(input);
    return result;
}

/* LENGTH — SYM → I64 */
ray_t* exec_strlen(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_I64, len);
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    result->len = len;
    int64_t* dst = (int64_t*)ray_data(result);

    if (input->type == RAY_STR) {
        const ray_str_t* elems; const char* pool;
        str_resolve(input, &elems, &pool);
        for (int64_t i = 0; i < len; i++) {
            if (ray_vec_is_null((ray_t*)input, i)) {
                dst[i] = 0;
                ray_vec_set_null(result, i, true);
                continue;
            }
            dst[i] = (int64_t)elems[i].len;
        }
    } else {
        for (int64_t i = 0; i < len; i++) {
            if (ray_vec_is_null((ray_t*)input, i)) {
                dst[i] = 0;
                ray_vec_set_null(result, i, true);
                continue;
            }
            const char* sp; size_t sl;
            sym_elem(input, i, &sp, &sl);
            dst[i] = (int64_t)sl;
        }
    }
    ray_release(input);
    return result;
}

/* SUBSTR(str, start, len) — 1-based start */
ray_t* exec_substr(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* start_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (start_v && !RAY_IS_ERR(start_v)) ray_release(start_v); return input; }
    if (!start_v || RAY_IS_ERR(start_v)) { ray_release(input); return start_v; }

    /* Get len arg from ext node's literal field */
    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t len_id = (uint32_t)(uintptr_t)ext->literal;
    ray_t* len_v = exec_node(g, &g->nodes[len_id]);
    if (!len_v || RAY_IS_ERR(len_v)) { ray_release(input); ray_release(start_v); return len_v; }

    int64_t nrows = input->len;
    bool is_str = (input->type == RAY_STR);

    ray_t* result;
    if (is_str) {
        result = ray_vec_new(RAY_STR, nrows);
    } else {
        result = ray_vec_new(RAY_SYM, nrows);
    }
    if (!result || RAY_IS_ERR(result)) { ray_release(input); ray_release(start_v); ray_release(len_v); return result; }
    if (!is_str) result->len = nrows;
    int64_t* sym_dst = is_str ? NULL : (int64_t*)ray_data(result);

    const ray_str_t* str_elems = NULL;
    const char* str_pool = NULL;
    if (is_str) str_resolve(input, &str_elems, &str_pool);

    /* start_v and len_v may be atom scalars or vectors.
     * Handle RAY_I32 vectors correctly (read as int32_t, not int64_t). */
    int64_t s_scalar = 0, l_scalar = 0;
    const int64_t* s_data = NULL;
    const int64_t* l_data = NULL;
    const int32_t* s_data_i32 = NULL;
    const int32_t* l_data_i32 = NULL;
    if (start_v->type == -RAY_I64) s_scalar = start_v->i64;
    else if (start_v->type == -RAY_F64) s_scalar = (int64_t)start_v->f64;
    else if (start_v->len == 1) {
        if (start_v->type == RAY_F64)
            s_scalar = (int64_t)((double*)ray_data(start_v))[0];
        else if (start_v->type == RAY_I32)
            s_scalar = (int64_t)((int32_t*)ray_data(start_v))[0];
        else
            s_scalar = ((int64_t*)ray_data(start_v))[0];
    }
    else if (start_v->type == RAY_I32) s_data_i32 = (const int32_t*)ray_data(start_v);
    else s_data = (const int64_t*)ray_data(start_v);
    if (len_v->type == -RAY_I64) l_scalar = len_v->i64;
    else if (len_v->type == -RAY_F64) l_scalar = (int64_t)len_v->f64;
    else if (len_v->len == 1) {
        if (len_v->type == RAY_F64)
            l_scalar = (int64_t)((double*)ray_data(len_v))[0];
        else if (len_v->type == RAY_I32)
            l_scalar = (int64_t)((int32_t*)ray_data(len_v))[0];
        else
            l_scalar = ((int64_t*)ray_data(len_v))[0];
    }
    else if (len_v->type == RAY_I32) l_data_i32 = (const int32_t*)ray_data(len_v);
    else l_data = (const int64_t*)ray_data(len_v);

    for (int64_t i = 0; i < nrows; i++) {
        /* Propagate null — from input, start, or length */
        if (ray_vec_is_null((ray_t*)input, i) ||
            ((s_data || s_data_i32) && ray_vec_is_null((ray_t*)start_v, i)) ||
            ((l_data || l_data_i32) && ray_vec_is_null((ray_t*)len_v, i))) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                sym_dst[i] = 0;
                ray_vec_set_null(result, i, true);
            }
            continue;
        }
        const char* sp; size_t sl;
        if (is_str) {
            sp = ray_str_t_ptr(&str_elems[i], str_pool);
            sl = str_elems[i].len;
        } else {
            sym_elem(input, i, &sp, &sl);
        }
        int64_t st = (s_data ? s_data[i] : s_data_i32 ? (int64_t)s_data_i32[i] : s_scalar) - 1; /* 1-based → 0-based */
        int64_t ln = l_data ? l_data[i] : l_data_i32 ? (int64_t)l_data_i32[i] : l_scalar;
        if (st < 0) st = 0;
        if ((size_t)st >= sl) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
            }
            else { sym_dst[i] = ray_sym_intern("", 0); }
            continue;
        }
        if (ln < 0 || ln > (int64_t)(sl - (size_t)st)) ln = (int64_t)sl - st;
        if (is_str) {
            result = ray_str_vec_append(result, sp + st, (size_t)ln);
            if (RAY_IS_ERR(result)) break;
        } else {
            sym_dst[i] = ray_sym_intern(sp + st, (size_t)ln);
        }
    }
    ray_release(input); ray_release(start_v); ray_release(len_v);
    return result;
}

/* REPLACE(str, from, to) */
ray_t* exec_replace(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* from_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (from_v && !RAY_IS_ERR(from_v)) ray_release(from_v); return input; }
    if (!from_v || RAY_IS_ERR(from_v)) { ray_release(input); return from_v; }

    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t to_id = (uint32_t)(uintptr_t)ext->literal;
    ray_t* to_v = exec_node(g, &g->nodes[to_id]);
    if (!to_v || RAY_IS_ERR(to_v)) { ray_release(input); ray_release(from_v); return to_v; }

    /* from_v and to_v should be string constants (SYM atoms) */
    const char* from_str = ray_str_ptr(from_v);
    size_t from_len = ray_str_len(from_v);
    const char* to_str = ray_str_ptr(to_v);
    size_t to_len = ray_str_len(to_v);

    int64_t nrows = input->len;
    bool is_str = (input->type == RAY_STR);

    ray_t* result;
    if (is_str) {
        result = ray_vec_new(RAY_STR, nrows);
    } else {
        result = ray_vec_new(RAY_SYM, nrows);
    }
    if (!result || RAY_IS_ERR(result)) { ray_release(input); ray_release(from_v); ray_release(to_v); return result; }
    if (!is_str) result->len = nrows;
    int64_t* sym_dst = is_str ? NULL : (int64_t*)ray_data(result);

    const ray_str_t* str_elems = NULL;
    const char* str_pool = NULL;
    if (is_str) str_resolve(input, &str_elems, &str_pool);

    for (int64_t i = 0; i < nrows; i++) {
        /* Propagate null */
        if (ray_vec_is_null((ray_t*)input, i)) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                sym_dst[i] = 0;
                ray_vec_set_null(result, i, true);
            }
            continue;
        }
        const char* sp; size_t sl;
        if (is_str) {
            sp = ray_str_t_ptr(&str_elems[i], str_pool);
            sl = str_elems[i].len;
        } else {
            sym_elem(input, i, &sp, &sl);
        }
        /* Simple find-and-replace-all */
        /* Worst case: every char is a match, each replaced by to_len bytes.
         * Guard against size_t overflow when to_len >> from_len. */
        size_t n_matches = (from_len > 0) ? sl / from_len : 0;
        size_t worst;
        if (from_len > 0 && to_len > from_len && n_matches > SIZE_MAX / to_len) {
            worst = SIZE_MAX; /* overflow → cap at max; scratch_alloc will OOM */
        } else if (from_len > 0 && to_len >= from_len) {
            /* Expanding or same-size: max output when every chunk matches */
            worst = n_matches * to_len + (sl % from_len) + 1;
        } else {
            /* Shrinking or from_len==0: max output when nothing matches → sl */
            worst = sl + 1;
        }
        char sbuf[8192];
        char* buf = sbuf;
        ray_t* dyn_hdr = NULL;
        if (worst > sizeof(sbuf)) {
            buf = (char*)scratch_alloc(&dyn_hdr, worst);
            if (!buf) {
                ray_release(result);
                ray_release(input); ray_release(from_v); ray_release(to_v);
                return ray_error("oom", NULL);
            }
        }
        size_t buf_cap = dyn_hdr ? worst : sizeof(sbuf);
        size_t bi = 0;
        for (size_t j = 0; j < sl; ) {
            if (from_len > 0 && j + from_len <= sl && memcmp(sp + j, from_str, from_len) == 0) {
                if (bi + to_len < buf_cap) { memcpy(buf + bi, to_str, to_len); bi += to_len; }
                j += from_len;
            } else {
                if (bi < buf_cap - 1) buf[bi++] = sp[j];
                j++;
            }
        }
        if (is_str) {
            ray_t* prev = result;
            result = ray_str_vec_append(result, buf, bi);
            if (RAY_IS_ERR(result)) { ray_release(prev); scratch_free(dyn_hdr); break; }
        } else {
            buf[bi] = '\0';
            sym_dst[i] = ray_sym_intern(buf, bi);
        }
        scratch_free(dyn_hdr);
    }
    ray_release(input); ray_release(from_v); ray_release(to_v);
    return result;
}

/* CONCAT(a, b, ...) */
ray_t* exec_concat(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    int64_t raw_nargs = ext->sym;
    if (raw_nargs < 2 || raw_nargs > 255) return ray_error("domain", NULL);
    int n_args = (int)raw_nargs;

    /* Evaluate all inputs */
    ray_t* args_stack[16];
    ray_t** args = args_stack;
    ray_t* args_hdr = NULL;
    if (n_args > 16) {
        args = (ray_t**)scratch_calloc(&args_hdr, (size_t)n_args * sizeof(ray_t*));
        if (!args) return ray_error("oom", NULL);
    }

    args[0] = exec_node(g, op->inputs[0]);
    args[1] = exec_node(g, op->inputs[1]);
    uint32_t* trail = (uint32_t*)((char*)(ext + 1));
    for (int i = 2; i < n_args; i++) {
        args[i] = exec_node(g, &g->nodes[trail[i - 2]]);
    }
    /* Error check */
    for (int i = 0; i < n_args; i++) {
        if (!args[i] || RAY_IS_ERR(args[i])) {
            ray_t* err = args[i];
            for (int j = 0; j < n_args; j++) {
                if (j != i && args[j] && !RAY_IS_ERR(args[j])) ray_release(args[j]);
            }
            scratch_free(args_hdr);
            return err;
        }
    }

    /* Derive nrows from first vector arg (scalar args have byte-length in len) */
    int64_t nrows = 1;
    bool out_str = false;
    for (int a = 0; a < n_args; a++) {
        int8_t at = args[a]->type;
        if (at == RAY_STR) { out_str = true; if (nrows == 1) nrows = args[a]->len; }
        if (RAY_IS_SYM(at)) { if (nrows == 1) nrows = args[a]->len; }
        if (!ray_is_atom(args[a]) && nrows == 1) { nrows = args[a]->len; }
    }
    ray_t* result = ray_vec_new(out_str ? RAY_STR : RAY_SYM, nrows);
    if (!result || RAY_IS_ERR(result)) {
        for (int i = 0; i < n_args; i++) ray_release(args[i]);
        scratch_free(args_hdr);
        return result;
    }
    if (!out_str) result->len = nrows;
    int64_t* dst = out_str ? NULL : (int64_t*)ray_data(result);

    for (int64_t r = 0; r < nrows; r++) {
        /* Check if any arg is null at this row */
        bool any_null = false;
        for (int a = 0; a < n_args; a++) {
            if (ray_is_atom(args[a])) {
                if (RAY_ATOM_IS_NULL(args[a])) { any_null = true; break; }
            } else if (ray_vec_is_null((ray_t*)args[a], r < args[a]->len ? r : 0)) {
                any_null = true;
                break;
            }
        }
        if (any_null) {
            if (out_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                dst[r] = 0;
                ray_vec_set_null(result, r, true);
            }
            continue;
        }
        /* Pre-scan to compute total concat length for this row */
        size_t total = 0;
        for (int a = 0; a < n_args; a++) {
            int8_t t = args[a]->type;
            if (t == RAY_STR) {
                const ray_str_t* elems; const char* p;
                str_resolve(args[a], &elems, &p);
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                total += elems[ar].len;
            } else if (RAY_IS_SYM(t)) {
                const char* sp; size_t sl;
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                sym_elem(args[a], ar, &sp, &sl);
                total += sl;
            } else if (t == -RAY_STR) {
                total += ray_str_len(args[a]);
            }
        }
        char sbuf[8192];
        char* buf = sbuf;
        ray_t* dyn_hdr = NULL;
        size_t buf_cap = sizeof(sbuf);
        if (total >= sizeof(sbuf)) {
            buf = (char*)scratch_alloc(&dyn_hdr, total + 1);
            if (!buf) {
                ray_release(result);
                for (int i = 0; i < n_args; i++) ray_release(args[i]);
                scratch_free(args_hdr);
                return ray_error("oom", NULL);
            }
            buf_cap = total + 1;
        }
        size_t bi = 0;
        for (int a = 0; a < n_args; a++) {
            int8_t t = args[a]->type;
            if (t == RAY_STR) {
                const ray_str_t* elems; const char* pool;
                str_resolve(args[a], &elems, &pool);
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                const char* sp = ray_str_t_ptr(&elems[ar], pool);
                size_t sl = elems[ar].len;
                if (bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            } else if (RAY_IS_SYM(t)) {
                const char* sp; size_t sl;
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                sym_elem(args[a], ar, &sp, &sl);
                if (bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            } else if (t == -RAY_STR) {
                const char* sp = ray_str_ptr(args[a]);
                size_t sl = ray_str_len(args[a]);
                if (sp && bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            }
        }
        if (out_str) {
            ray_t* prev = result;
            result = ray_str_vec_append(result, buf, bi);
            if (RAY_IS_ERR(result)) { ray_release(prev); scratch_free(dyn_hdr); break; }
        } else {
            buf[bi] = '\0';
            dst[r] = ray_sym_intern(buf, bi);
        }
        scratch_free(dyn_hdr);
    }
    for (int i = 0; i < n_args; i++) ray_release(args[i]);
    scratch_free(args_hdr);
    return result;
}
