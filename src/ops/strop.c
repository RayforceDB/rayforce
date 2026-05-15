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
#include "ops/internal.h"
#include "table/sym.h"
#include "table/table.h"
#include "ops/glob.h"

/* ══════════════════════════════════════════
 * String builtins
 * ══════════════════════════════════════════ */

static bool strlen_atom_value(ray_t* x, int64_t* out) {
    if (x->type == -RAY_STR) {
        *out = (int64_t)ray_str_len(x);
        return true;
    }
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        *out = s ? (int64_t)ray_str_len(s) : 0;
        return true;
    }
    return false;
}

static bool strlen_vec_value(ray_t* x, int64_t row, int64_t* out) {
    if (x->type == RAY_STR) {
        size_t slen = 0;
        const char* s = ray_str_vec_get(x, row, &slen);
        *out = s ? (int64_t)slen : 0;
        return true;
    }
    if (x->type == RAY_SYM) {
        int64_t sid = ray_read_sym(ray_data(x), row, x->type, x->attrs);
        ray_t* s = ray_sym_str(sid);
        *out = s ? (int64_t)ray_str_len(s) : 0;
        return true;
    }
    return false;
}

static ray_t* strlen_vec(ray_t* x) {
    if (x->type != RAY_STR && x->type != RAY_SYM)
        return ray_error("type", "strlen: expected string or symbol");

    int64_t n = x->len;
    ray_t* out = ray_vec_new(RAY_I64, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;
    int64_t* dst = (int64_t*)ray_data(out);
    bool has_nulls = (x->attrs & RAY_ATTR_HAS_NULLS) != 0;

    for (int64_t i = 0; i < n; i++) {
        if (has_nulls && ray_vec_is_null(x, i)) {
            dst[i] = 0;
            ray_vec_set_null(out, i, true);
            continue;
        }
        strlen_vec_value(x, i, &dst[i]);
    }
    return out;
}

static ray_t* strlen_mapcommon(ray_t* x) {
    ray_t** ptrs = (ray_t**)ray_data(x);
    ray_t* keys = ptrs[0];
    ray_t* counts = ptrs[1];
    if (!keys || !counts || RAY_IS_ERR(keys) || RAY_IS_ERR(counts))
        return ray_error("domain", "strlen: invalid partition column");
    if (keys->type != RAY_STR && keys->type != RAY_SYM)
        return ray_error("type", "strlen: expected string or symbol");

    int64_t total = ray_parted_nrows(x);
    ray_t* out = ray_vec_new(RAY_I64, total);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = total;
    int64_t* dst = (int64_t*)ray_data(out);
    const int64_t* cnt = (const int64_t*)ray_data(counts);

    int64_t off = 0;
    for (int64_t p = 0; p < counts->len; p++) {
        int64_t v = 0;
        bool is_null = (keys->attrs & RAY_ATTR_HAS_NULLS) && ray_vec_is_null(keys, p);
        if (!is_null) strlen_vec_value(keys, p, &v);
        for (int64_t r = 0; r < cnt[p]; r++) {
            dst[off] = v;
            if (is_null) ray_vec_set_null(out, off, true);
            off++;
        }
    }
    return out;
}

static ray_t* strlen_parted(ray_t* x) {
    int8_t base = (int8_t)RAY_PARTED_BASETYPE(x->type);
    if (base != RAY_STR && base != RAY_SYM)
        return ray_error("type", "strlen: expected string or symbol");

    int64_t total = ray_parted_nrows(x);
    ray_t* out = ray_vec_new(RAY_I64, total);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = total;
    int64_t* dst = (int64_t*)ray_data(out);

    ray_t** segs = (ray_t**)ray_data(x);
    int64_t off = 0;
    for (int64_t s = 0; s < x->len; s++) {
        ray_t* seg = segs[s];
        if (!seg || RAY_IS_ERR(seg)) continue;
        bool has_nulls = (seg->attrs & RAY_ATTR_HAS_NULLS) != 0;
        for (int64_t i = 0; i < seg->len; i++) {
            if (has_nulls && ray_vec_is_null(seg, i)) {
                dst[off] = 0;
                ray_vec_set_null(out, off, true);
            } else {
                strlen_vec_value(seg, i, &dst[off]);
            }
            off++;
        }
    }
    return out;
}

ray_t* ray_strlen_fn(ray_t* x) {
    int64_t len = 0;
    if (strlen_atom_value(x, &len)) return ray_i64(len);
    if (ray_is_vec(x)) return strlen_vec(x);
    if (x->type == RAY_MAPCOMMON) return strlen_mapcommon(x);
    if (RAY_IS_PARTED(x->type)) return strlen_parted(x);
    return ray_error("type", "strlen: expected string or symbol");
}

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

    /* Pre-compile the pattern once.  Most benchmark LIKE shapes are
     * `*literal*` (substring) which collapses to a memmem call — the
     * libc-provided implementation is SIMD on glibc/Apple/BSD.  When the
     * shape is RAY_GLOB_SHAPE_NONE we keep the iterative matcher. */
    ray_glob_compiled_t pc = ray_glob_compile(pat, pat_len);
    bool use_simple = pc.shape != RAY_GLOB_SHAPE_NONE;

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
        bool m = use_simple ? ray_glob_match_compiled(&pc, s, sl)
                            : ray_glob_match(s, sl, pat, pat_len);
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
            /* SYM column is dictionary-encoded with adaptive widths
             * (W8/W16/W32/W64).  Two bugs to avoid:
             *   (a) Reading the column as int64_t* is wrong for any
             *       width != W64 — must use ray_read_sym.
             *   (b) ray_sym_str returns a borrowed pointer; releasing
             *       it would decrement the global sym table entry.
             *
             * Fast path: a SYM column with N rows references at most
             * D = ray_sym_count() distinct sym_ids.  Build a
             * sym_id → bool LUT with a "seen" bitmap so each sym_id
             * runs the glob matcher at most once.  For LIKE on URL
             * (1.7M unique values, 5M rows) this turns an O(n_rows)
             * pattern-scan into O(n_distinct + n_rows) — the second
             * pass is a single byte load + table lookup per row. */
            const void* base = ray_data(x);
            int8_t in_type = x->type;
            uint8_t in_attrs = x->attrs;

            /* The global sym table can be much larger than the set of
             * IDs this column references (e.g. BrowserCountry with 54
             * uniques in a process that's also loaded URL with 1.7M
             * uniques).  Lazy-resolve via the seen bitmap so we only
             * match against sym_ids actually touched.  ray_sym_strings_borrow
             * snapshots the strings array under one lock so each lookup
             * is a plain pointer load. */
            ray_t** sym_strings = NULL;
            uint32_t dict_n = 0;
            ray_sym_strings_borrow(&sym_strings, &dict_n);
            ray_t* lut_hdr = NULL;
            ray_t* seen_hdr = NULL;
            uint8_t* lut = NULL;
            uint8_t* seen = NULL;
            if (dict_n > 0) {
                lut  = (uint8_t*)scratch_alloc (&lut_hdr,  (size_t)dict_n);
                seen = (uint8_t*)scratch_calloc(&seen_hdr, (size_t)dict_n);
            }
            if (lut && seen) {
                /* Out-of-range sym IDs ("unknown" or "missing") match
                 * the pattern as if the string were empty — same
                 * semantics as the OOM fallback at line 332 below and
                 * the atom case at line 217 above (sym_str==NULL → "").
                 * Compute the empty-string match once so the row pass
                 * can short-circuit out-of-range sids without taking
                 * the matcher per row. */
                uint8_t empty_match = (use_simple
                                       ? ray_glob_match_compiled(&pc, "", 0)
                                       : ray_glob_match("", 0, pat, pat_len))
                                      ? 1 : 0;

                /* First pass: discover the unique sym_ids referenced and
                 * resolve each pattern match exactly once.  Second pass:
                 * width-specialised LUT projection so the per-row loop
                 * is a tight gather. */
                int sym_w = (int)(in_attrs & RAY_SYM_W_MASK);
                #define DICT_PASS(LOAD)                                       \
                    for (int64_t i = 0; i < n; i++) {                         \
                        int64_t sid = (LOAD);                                 \
                        if ((uint64_t)sid >= (uint64_t)dict_n) continue;      \
                        if (!seen[sid]) {                                     \
                            ray_t* s = sym_strings[sid];                      \
                            const char* sp = s ? ray_str_ptr(s) : "";         \
                            size_t sl = s ? ray_str_len(s) : 0;               \
                            lut[sid] = (use_simple                            \
                                        ? ray_glob_match_compiled(&pc, sp, sl)\
                                        : ray_glob_match(sp, sl, pat, pat_len)) \
                                       ? 1 : 0;                               \
                            seen[sid] = 1;                                    \
                        }                                                     \
                    }
                #define ROW_PASS(LOAD)                                        \
                    for (int64_t i = 0; i < n; i++) {                         \
                        int64_t sid = (LOAD);                                 \
                        out[i] = ((uint64_t)sid < (uint64_t)dict_n)           \
                                 ? lut[sid]                                   \
                                 : empty_match;                               \
                    }
                switch (sym_w) {
                case RAY_SYM_W8: {
                    const uint8_t* d = (const uint8_t*)base;
                    DICT_PASS(d[i]) ROW_PASS(d[i]) break;
                }
                case RAY_SYM_W16: {
                    const uint16_t* d = (const uint16_t*)base;
                    DICT_PASS(d[i]) ROW_PASS(d[i]) break;
                }
                case RAY_SYM_W32: {
                    const uint32_t* d = (const uint32_t*)base;
                    DICT_PASS(d[i]) ROW_PASS(d[i]) break;
                }
                case RAY_SYM_W64:
                default: {
                    const int64_t* d = (const int64_t*)base;
                    DICT_PASS(d[i]) ROW_PASS(d[i]) break;
                }
                }
                #undef DICT_PASS
                #undef ROW_PASS
                scratch_free(lut_hdr);
                scratch_free(seen_hdr);
            } else {
                /* OOM building the LUT: fall back to per-row scan.  Still
                 * uses ray_read_sym for adaptive-width correctness. */
                if (lut_hdr) scratch_free(lut_hdr);
                if (seen_hdr) scratch_free(seen_hdr);
                for (int64_t i = 0; i < n; i++) {
                    int64_t sid = ray_read_sym(base, i, in_type, in_attrs);
                    ray_t* s = (sym_strings && (uint64_t)sid < (uint64_t)dict_n)
                               ? sym_strings[sid] : NULL;
                    const char* sp = s ? ray_str_ptr(s) : "";
                    size_t sl = s ? ray_str_len(s) : 0;
                    out[i] = (use_simple
                              ? ray_glob_match_compiled(&pc, sp, sl)
                              : ray_glob_match(sp, sl, pat, pat_len)) ? 1 : 0;
                }
            }
        } else {
            /* RAY_STR vector */
            for (int64_t i = 0; i < n; i++) {
                size_t slen;
                const char* s = ray_str_vec_get(x, i, &slen);
                bool m = false;
                if (s) {
                    m = use_simple ? ray_glob_match_compiled(&pc, s, slen)
                                   : ray_glob_match(s, slen, pat, pat_len);
                }
                out[i] = m ? 1 : 0;
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
