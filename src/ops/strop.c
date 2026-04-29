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
#include "table/sym.h"
#include "ops/glob.h"

/* ══════════════════════════════════════════
 * String builtins
 * ══════════════════════════════════════════ */

ray_t* ray_split_fn(ray_t* str, ray_t* delim) {
    /* List split: (split list indices) → list of sub-lists */
    if (str->type == RAY_LIST &&
        ray_is_vec(delim) && (delim->type == RAY_I64 || delim->type == RAY_I16 || delim->type == RAY_I32)) {
        int64_t nidx = delim->len;
        if (nidx == 0) return NULL; /* null for empty indices */
        int64_t idx_buf[256];
        if (nidx > 256) return ray_error("limit", NULL);
        for (int64_t ii = 0; ii < nidx; ii++) {
            int alloc = 0;
            ray_t* ie = collection_elem(delim, ii, &alloc);
            idx_buf[ii] = as_i64(ie);
            if (alloc) ray_release(ie);
        }
        int64_t total = str->len;
        ray_t** items = (ray_t**)ray_data(str);
        ray_t* result = ray_list_new(nidx + 1);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t i = 0; i < nidx; i++) {
            int64_t start = idx_buf[i];
            int64_t end = (i + 1 < nidx) ? idx_buf[i + 1] : total;
            int64_t seglen = end - start;
            if (seglen < 0) seglen = 0;
            /* Try to make a typed vector if all elements are same type */
            if (seglen > 0) {
                int8_t first_type = items[start]->type;
                int all_same = 1;
                for (int64_t j = start + 1; j < start + seglen && j < total; j++) {
                    if (items[j]->type != first_type) { all_same = 0; break; }
                }
                if (all_same && first_type < 0 && first_type != -RAY_STR) {
                    int8_t vtype = -first_type;
                    ray_t* vec = ray_vec_new(vtype, seglen);
                    if (!RAY_IS_ERR(vec)) {
                        vec->len = seglen;
                        for (int64_t j = 0; j < seglen && start + j < total; j++)
                            store_typed_elem(vec, j, items[start + j]);
                        result = ray_list_append(result, vec);
                        ray_release(vec);
                        if (RAY_IS_ERR(result)) return result;
                        continue;
                    }
                }
            }
            /* Heterogeneous or string segment: make a sub-list */
            ray_t* seg = ray_list_new(seglen);
            if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
            for (int64_t j = 0; j < seglen && start + j < total; j++) {
                ray_retain(items[start + j]);
                seg = ray_list_append(seg, items[start + j]);
                ray_release(items[start + j]);
                if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
            }
            result = ray_list_append(result, seg);
            ray_release(seg);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    /* Vector/string split: (split vec/str indices) → list of sub-vectors/substrings */
    if ((ray_is_vec(str) || (ray_is_atom(str) && (-str->type) == RAY_STR)) &&
        ray_is_vec(delim) && (delim->type == RAY_I64 || delim->type == RAY_I16 || delim->type == RAY_I32)) {
        int64_t nidx = delim->len;
        if (nidx == 0) return NULL; /* null for empty indices */
        /* Extract indices as i64 */
        int64_t idx_buf[256];
        if (nidx > 256) return ray_error("limit", NULL);
        for (int64_t ii = 0; ii < nidx; ii++) {
            int alloc = 0;
            ray_t* ie = collection_elem(delim, ii, &alloc);
            idx_buf[ii] = as_i64(ie);
            if (alloc) ray_release(ie);
        }
        /* String split by indices */
        if (ray_is_atom(str) && (-str->type) == RAY_STR) {
            const char* sp2 = ray_str_ptr(str);
            size_t total = ray_str_len(str);
            ray_t* result = ray_list_new(nidx + 1);
            if (RAY_IS_ERR(result)) return result;
            for (int64_t i = 0; i < nidx; i++) {
                int64_t start = idx_buf[i];
                int64_t end = (i + 1 < nidx) ? idx_buf[i + 1] : (int64_t)total;
                int64_t seglen = end - start;
                if (seglen < 0) seglen = 0;
                if (start > (int64_t)total) start = (int64_t)total;
                if (start + seglen > (int64_t)total) seglen = (int64_t)total - start;
                ray_t* seg = ray_str(sp2 + start, (size_t)seglen);
                if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
                result = ray_list_append(result, seg);
                ray_release(seg);
                if (RAY_IS_ERR(result)) return result;
            }
            return result;
        }
        /* Vector split by indices */
        int64_t total = str->len;
        int esz = ray_elem_size(str->type);
        ray_t* result = ray_list_new(nidx + 1);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t i = 0; i < nidx; i++) {
            int64_t start = idx_buf[i];
            int64_t end = (i + 1 < nidx) ? idx_buf[i + 1] : total;
            int64_t seglen = end - start;
            if (seglen < 0) seglen = 0;
            ray_t* seg = ray_vec_new(str->type, seglen);
            if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
            seg->len = seglen;
            if (seglen > 0) memcpy(ray_data(seg), (char*)ray_data(str) + start * esz, seglen * esz);
            result = ray_list_append(result, seg);
            ray_release(seg);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    /* Normalize str and delim to string pointers */
    const char *sp, *dp;
    size_t slen, dlen;
    ray_t* sym_str_s = NULL;
    ray_t* sym_str_d = NULL;
    if (str->type == -RAY_STR) { sp = ray_str_ptr(str); slen = ray_str_len(str); }
    else if (str->type == -RAY_SYM) { sym_str_s = ray_sym_str(str->i64); if (!sym_str_s) return ray_error("domain", NULL); sp = ray_str_ptr(sym_str_s); slen = ray_str_len(sym_str_s); }
    /* RAY_CHAR removed — all chars are now -RAY_STR */
    else return ray_error("type", NULL);
    if (delim->type == -RAY_STR) { dp = ray_str_ptr(delim); dlen = ray_str_len(delim); }
    /* RAY_CHAR removed — all chars are now -RAY_STR */
    else { if (sym_str_s) ray_release(sym_str_s); return ray_error("type", NULL); }

    ray_t* result = ray_list_new(8);
    if (RAY_IS_ERR(result)) { if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return result; }

    if (dlen == 0 || slen == 0) {
        ray_t* part = ray_str(sp, slen);
        result = ray_list_append(result, part);
        ray_release(part);
        if (sym_str_s) ray_release(sym_str_s);
        if (sym_str_d) ray_release(sym_str_d);
        return result;
    }

    size_t start = 0;
    for (size_t i = 0; i <= slen - dlen; ) {
        if (memcmp(sp + i, dp, dlen) == 0) {
            ray_t* part = ray_str(sp + start, i - start);
            if (RAY_IS_ERR(part)) { ray_release(result); if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return part; }
            result = ray_list_append(result, part);
            ray_release(part);
            if (RAY_IS_ERR(result)) { if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return result; }
            i += dlen;
            start = i;
        } else {
            i++;
        }
    }
    /* Last part */
    ray_t* part = ray_str(sp + start, slen - start);
    if (RAY_IS_ERR(part)) { ray_release(result); if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return part; }
    result = ray_list_append(result, part);
    ray_release(part);
    if (sym_str_s) ray_release(sym_str_s);
    if (sym_str_d) ray_release(sym_str_d);
    return result;
}

/* (like str pattern) — glob-style pattern matching.
 * Syntax: * (any), ? (one char), [abc] / [a-z] / [!abc] (char class).
 * Implementation lives in src/ops/glob.[ch]; same matcher is used by
 * the DAG executor (string.c::exec_like) for select-where contexts. */
ray_t* ray_like_fn(ray_t* x, ray_t* pattern) {
    /* Pattern must be a string atom */
    if (pattern->type != -RAY_STR) return ray_error("type", "like: pattern must be a string");
    const char* pat = ray_str_ptr(pattern);
    size_t pat_len = ray_str_len(pattern);

    /* Atom: single match */
    if (x->type == -RAY_STR || x->type == -RAY_SYM) {
        const char* s; size_t sl;
        ray_t* sym_str = NULL;
        if (x->type == -RAY_SYM) {
            sym_str = ray_sym_str(x->i64);
            s  = sym_str ? ray_str_ptr(sym_str) : "";
            sl = sym_str ? ray_str_len(sym_str) : 0;
        } else {
            s  = ray_str_ptr(x);
            sl = ray_str_len(x);
        }
        bool m = ray_glob_match(s, sl, pat, pat_len);
        if (sym_str) ray_release(sym_str);
        return make_bool(m ? 1 : 0);
    }

    /* Vector: map over elements */
    if (ray_is_vec(x) && (x->type == RAY_SYM || x->type == RAY_STR)) {
        int64_t n = ray_len(x);
        ray_t* result = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(result)) return result;
        result->len = n;
        uint8_t* out = (uint8_t*)ray_data(result);

        if (x->type == RAY_SYM) {
            int64_t* sym_ids = (int64_t*)ray_data(x);
            for (int64_t i = 0; i < n; i++) {
                ray_t* sym_str = ray_sym_str(sym_ids[i]);
                const char* s = sym_str ? ray_str_ptr(sym_str) : "";
                size_t sl = sym_str ? ray_str_len(sym_str) : 0;
                out[i] = ray_glob_match(s, sl, pat, pat_len) ? 1 : 0;
                if (sym_str) ray_release(sym_str);
            }
        } else {
            /* RAY_STR vector */
            for (int64_t i = 0; i < n; i++) {
                size_t slen;
                const char* s = ray_str_vec_get(x, i, &slen);
                out[i] = (s && ray_glob_match(s, slen, pat, pat_len)) ? 1 : 0;
            }
        }
        return result;
    }

    return ray_error("type", "like: expects string or symbol");
}

ray_t* ray_sym_name_fn(ray_t* x) {
    if (x->type == -RAY_I64) {
        if (x->i64 < 0 || !ray_sym_str(x->i64))
            return ray_error("domain", "sym-name: invalid sym ID");
        return ray_sym(x->i64);
    }
    if (x->type == RAY_I64) {
        int64_t n = x->len;
        const int64_t* data = (const int64_t*)ray_data(x);
        /* Validate all IDs first */
        for (int64_t i = 0; i < n; i++) {
            if (data[i] < 0 || !ray_sym_str(data[i]))
                return ray_error("domain", "sym-name: invalid sym ID in vector");
        }
        ray_t* out = ray_vec_new(RAY_SYM, n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            out = ray_vec_append(out, &data[i]);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    /* Already sym (atom or vector), or empty I64/SYM vector — passthrough */
    if (x->type == -RAY_SYM || x->type == RAY_SYM ||
        ((x->type == RAY_I64 || x->type == RAY_SYM) && x->len == 0)) {
        ray_retain(x); return x;
    }
    return ray_error("type", "sym-name: expected i64 or i64 vector");
}
