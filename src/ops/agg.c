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
#include "ops/ops.h"
#include "mem/heap.h"

#include <stdlib.h>  /* qsort (introselect fallback) */

static int dbl_cmp(const void* a, const void* b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

/* Partition vals[lo..hi] so that vals[k] holds the kth-smallest element,
 * with everything to the left ≤ and everything to the right ≥.  Average
 * O(n) (Hoare quickselect with median-of-three), worst-case O(n log n)
 * via qsort fallback when recursion exceeds 2*log2(range).  Mirrors
 * std::nth_element's contract — the standard quantile/quickselect
 * pattern. */
static void nth_element_dbl(double* a, int64_t lo, int64_t hi, int64_t k) {
    int depth_limit = 0;
    for (int64_t r = hi - lo + 1; r > 0; r >>= 1) depth_limit++;
    depth_limit *= 2;
    while (hi - lo > 16) {
        if (depth_limit-- <= 0) {
            qsort(a + lo, (size_t)(hi - lo + 1), sizeof(double), dbl_cmp);
            return;
        }
        int64_t mid = lo + ((hi - lo) >> 1);
        if (a[lo] > a[mid]) { double t = a[lo]; a[lo] = a[mid]; a[mid] = t; }
        if (a[lo] > a[hi])  { double t = a[lo]; a[lo] = a[hi];  a[hi]  = t; }
        if (a[mid] > a[hi]) { double t = a[mid]; a[mid] = a[hi]; a[hi] = t; }
        /* Park pivot at hi-1; partition (lo, hi-1) with sentinels at both ends. */
        { double t = a[mid]; a[mid] = a[hi - 1]; a[hi - 1] = t; }
        double pivot = a[hi - 1];
        int64_t i = lo, j = hi - 1;
        for (;;) {
            while (a[++i] < pivot) {}
            while (a[--j] > pivot) {}
            if (i >= j) break;
            double t = a[i]; a[i] = a[j]; a[j] = t;
        }
        /* Restore pivot to its final resting position i. */
        { double t = a[i]; a[i] = a[hi - 1]; a[hi - 1] = t; }
        if      (k < i) hi = i - 1;
        else if (k > i) lo = i + 1;
        else            return;
    }
    /* Small range: insertion sort the slice covers vals[lo..hi]. */
    for (int64_t i = lo + 1; i <= hi; i++) {
        double key = a[i];
        int64_t j = i - 1;
        while (j >= lo && a[j] > key) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
}

/* ══════════════════════════════════════════
 * Aggregation builtins
 * ══════════════════════════════════════════ */

/* Build a one-op DAG over a single input vector and execute it. */
#define AGG_VEC_VIA_DAG(x, ctor) do {                       \
    ray_graph_t* g = ray_graph_new(NULL);                   \
    if (!g) return ray_error("oom", NULL);                  \
    ray_op_t* in = ray_graph_input_vec(g, x);              \
    ray_op_t* op = ctor(g, in);                            \
    return ray_lazy_wrap(g, op);                            \
} while(0)

static int agg_parted_numeric_base(int8_t t) {
    return t == RAY_BOOL || t == RAY_U8 || t == RAY_I16 ||
           t == RAY_I32 || t == RAY_I64 || t == RAY_F64 ||
           t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP;
}

static int64_t agg_read_i64(ray_t* v, int64_t i) {
    void* d = ray_data(v);
    switch (v->type) {
    case RAY_BOOL:
    case RAY_U8: return ((uint8_t*)d)[i];
    case RAY_I16: return ((int16_t*)d)[i];
    case RAY_I32:
    case RAY_DATE:
    case RAY_TIME: return ((int32_t*)d)[i];
    case RAY_I64:
    case RAY_TIMESTAMP: return ((int64_t*)d)[i];
    default: return 0;
    }
}

static ray_t* agg_atom_i64_for_type(int8_t t, int64_t v) {
    switch (t) {
    case RAY_BOOL: return ray_bool(v != 0);
    case RAY_U8: return ray_u8((uint8_t)v);
    case RAY_I16: return ray_i16((int16_t)v);
    case RAY_I32: return ray_i32((int32_t)v);
    case RAY_DATE: return ray_date(v);
    case RAY_TIME: return ray_time(v);
    case RAY_TIMESTAMP: return ray_timestamp(v);
    default: return ray_i64(v);
    }
}

static ray_t* agg_parted_sum(ray_t* x) {
    int8_t base = (int8_t)RAY_PARTED_BASETYPE(x->type);
    if (!agg_parted_numeric_base(base) || base == RAY_DATE)
        return ray_error("type", NULL);
    ray_t** segs = (ray_t**)ray_data(x);
    if (base == RAY_F64) {
        double sum = 0.0;
        for (int64_t s = 0; s < x->len; s++) {
            ray_t* seg = segs[s];
            if (!seg) continue;
            double* d = (double*)ray_data(seg);
            int has_nulls = (seg->attrs & RAY_ATTR_HAS_NULLS) != 0;
            for (int64_t i = 0; i < seg->len; i++)
                if (!has_nulls || !ray_vec_is_null(seg, i)) sum += d[i];
        }
        return make_f64(sum);
    }
    int64_t sum = 0;
    for (int64_t s = 0; s < x->len; s++) {
        ray_t* seg = segs[s];
        if (!seg) continue;
        int has_nulls = (seg->attrs & RAY_ATTR_HAS_NULLS) != 0;
        for (int64_t i = 0; i < seg->len; i++)
            if (!has_nulls || !ray_vec_is_null(seg, i)) sum += agg_read_i64(seg, i);
    }
    if (base == RAY_TIME) return ray_time(sum);
    if (base == RAY_TIMESTAMP) return ray_timestamp(sum);
    return make_i64(sum);
}

static ray_t* agg_parted_avg(ray_t* x) {
    int8_t base = (int8_t)RAY_PARTED_BASETYPE(x->type);
    if (!agg_parted_numeric_base(base)) return ray_error("type", NULL);
    ray_t** segs = (ray_t**)ray_data(x);
    double sum = 0.0;
    int64_t cnt = 0;
    for (int64_t s = 0; s < x->len; s++) {
        ray_t* seg = segs[s];
        if (!seg) continue;
        int has_nulls = (seg->attrs & RAY_ATTR_HAS_NULLS) != 0;
        if (base == RAY_F64) {
            double* d = (double*)ray_data(seg);
            for (int64_t i = 0; i < seg->len; i++) {
                if (has_nulls && ray_vec_is_null(seg, i)) continue;
                sum += d[i]; cnt++;
            }
        } else {
            for (int64_t i = 0; i < seg->len; i++) {
                if (has_nulls && ray_vec_is_null(seg, i)) continue;
                sum += (double)agg_read_i64(seg, i); cnt++;
            }
        }
    }
    if (cnt == 0) return ray_typed_null(-RAY_F64);
    return make_f64(sum / (double)cnt);
}

static ray_t* agg_parted_minmax(ray_t* x, int want_max) {
    int8_t base = (int8_t)RAY_PARTED_BASETYPE(x->type);
    if (!agg_parted_numeric_base(base)) return ray_error("type", NULL);
    ray_t** segs = (ray_t**)ray_data(x);
    int found = 0;
    double best_f = 0.0;
    int64_t best_i = 0;
    for (int64_t s = 0; s < x->len; s++) {
        ray_t* seg = segs[s];
        if (!seg) continue;
        int has_nulls = (seg->attrs & RAY_ATTR_HAS_NULLS) != 0;
        if (base == RAY_F64) {
            double* d = (double*)ray_data(seg);
            for (int64_t i = 0; i < seg->len; i++) {
                if (has_nulls && ray_vec_is_null(seg, i)) continue;
                double v = d[i];
                if (!found || (want_max ? v > best_f : v < best_f)) {
                    best_f = v; found = 1;
                }
            }
        } else {
            for (int64_t i = 0; i < seg->len; i++) {
                if (has_nulls && ray_vec_is_null(seg, i)) continue;
                int64_t v = agg_read_i64(seg, i);
                if (!found || (want_max ? v > best_i : v < best_i)) {
                    best_i = v; found = 1;
                }
            }
        }
    }
    if (!found) return ray_typed_null(-base);
    if (base == RAY_F64) return make_f64(best_f);
    return agg_atom_i64_for_type(base, best_i);
}

ray_t* ray_sum_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_SUM);
    if (RAY_IS_PARTED(x->type)) return agg_parted_sum(x);
    if (ray_is_atom(x)) {
        /* u8/i16 scalar sum promotes to i64 */
        if (x->type == -RAY_U8)  return make_i64((int64_t)x->u8);
        if (x->type == -RAY_I16) return make_i64((int64_t)x->i16);
        ray_retain(x); return x;
    }
    if (ray_is_vec(x)) {
        if (x->type == RAY_DATE) return ray_error("type", NULL);
        /* Narrow/temporal types need specific return constructors that the
         * DAG executor doesn't provide — use scalar path for these. */
        if (x->type == RAY_I32 || x->type == RAY_I16 || x->type == RAY_U8 ||
            x->type == RAY_TIME || x->type == RAY_TIMESTAMP) {
            int64_t n = x->len;
            bool has_nulls = (x->attrs & RAY_ATTR_HAS_NULLS) != 0;
            int64_t sum = 0;
            if (x->type == RAY_I32) {
                int32_t* d = (int32_t*)ray_data(x);
                if (has_nulls) { for (int64_t i = 0; i < n; i++) if (!ray_vec_is_null(x, i)) sum += d[i]; }
                else { for (int64_t i = 0; i < n; i++) sum += d[i]; }
                return make_i64(sum);
            } else if (x->type == RAY_I16) {
                int16_t* d = (int16_t*)ray_data(x);
                if (has_nulls) { for (int64_t i = 0; i < n; i++) if (!ray_vec_is_null(x, i)) sum += d[i]; }
                else { for (int64_t i = 0; i < n; i++) sum += d[i]; }
                return make_i64(sum);
            } else if (x->type == RAY_U8) {
                uint8_t* d = (uint8_t*)ray_data(x);
                if (has_nulls) { for (int64_t i = 0; i < n; i++) if (!ray_vec_is_null(x, i)) sum += d[i]; }
                else { for (int64_t i = 0; i < n; i++) sum += d[i]; }
                return make_i64(sum);
            } else if (x->type == RAY_TIME) {
                int32_t* d = (int32_t*)ray_data(x);
                if (has_nulls) { for (int64_t i = 0; i < n; i++) if (!ray_vec_is_null(x, i)) sum += d[i]; }
                else { for (int64_t i = 0; i < n; i++) sum += d[i]; }
                return ray_time(sum);
            } else {
                int64_t* d = (int64_t*)ray_data(x);
                if (has_nulls) { for (int64_t i = 0; i < n; i++) if (!ray_vec_is_null(x, i)) sum += d[i]; }
                else { for (int64_t i = 0; i < n; i++) sum += d[i]; }
                return ray_timestamp(sum);
            }
        }
        /* I64/F64: parallel morsel-driven reduction via DAG executor */
        AGG_VEC_VIA_DAG(x, ray_sum);
    }
    if (!is_list(x)) return ray_error("type", NULL);
    int64_t len = ray_len(x);
    if (len == 0) return make_i64(0);
    ray_t** elems = (ray_t**)ray_data(x);
    int has_float = 0;
    double fsum = 0.0;
    int64_t isum = 0;
    for (int64_t i = 0; i < len; i++) {
        if (!is_numeric(elems[i])) return ray_error("type", NULL);
        if (RAY_ATOM_IS_NULL(elems[i])) {
            if (elems[i]->type == -RAY_F64) has_float = 1;
            continue;
        }
        if (elems[i]->type == -RAY_F64) { has_float = 1; fsum += elems[i]->f64; }
        else if (elems[i]->type == -RAY_I64) { isum += elems[i]->i64; fsum += (double)elems[i]->i64; }
        else { int64_t v = (int64_t)as_f64(elems[i]); isum += v; fsum += (double)v; }
    }
    return has_float ? make_f64(fsum) : make_i64(isum);
}

ray_t* ray_count_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_COUNT);
    if (x->type == RAY_TABLE) return make_i64(ray_table_nrows(x));
    if (x->type == RAY_DICT)  return make_i64(ray_dict_len(x));
    /* String atom: count = string length */
    if (ray_is_atom(x) && (-x->type) == RAY_STR)
        return make_i64((int64_t)ray_str_len(x));
    if (ray_is_vec(x))
        return make_i64(x->len);  /* count = total length including nulls */
    if (!is_list(x)) {
        /* Scalar atom → count 1 */
        if (ray_is_atom(x)) return make_i64(1);
        return ray_error("type", NULL);
    }
    return make_i64(ray_len(x));
}

ray_t* ray_avg_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_AVG);
    if (RAY_IS_PARTED(x->type)) return agg_parted_avg(x);
    if (ray_is_atom(x)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
        if (is_numeric(x)) return make_f64(as_f64(x));
        ray_retain(x); return x;
    }
    if (ray_is_vec(x)) AGG_VEC_VIA_DAG(x, ray_avg);
    if (!is_list(x)) return ray_error("type", NULL);
    int64_t len = ray_len(x);
    if (len == 0) return ray_error("domain", NULL);
    ray_t** elems = (ray_t**)ray_data(x);
    double sum = 0.0;
    int64_t cnt = 0;
    for (int64_t i = 0; i < len; i++) {
        if (!is_numeric(elems[i])) return ray_error("type", NULL);
        if (RAY_ATOM_IS_NULL(elems[i])) continue;
        sum += as_f64(elems[i]); cnt++;
    }
    if (cnt == 0) return ray_typed_null(-RAY_F64);
    return make_f64(sum / (double)cnt);
}

ray_t* ray_min_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_MIN);
    if (RAY_IS_PARTED(x->type)) return agg_parted_minmax(x, 0);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (ray_is_vec(x)) AGG_VEC_VIA_DAG(x, ray_min_op);
    if (!is_list(x)) return ray_error("type", NULL);
    int64_t len = ray_len(x);
    if (len == 0) return ray_error("domain", NULL);
    ray_t** elems = (ray_t**)ray_data(x);
    int has_float = 0, found = 0;
    double fmin = 0; int64_t imin = 0;
    for (int64_t i = 0; i < len; i++) {
        if (!is_numeric(elems[i])) return ray_error("type", NULL);
        if (elems[i]->type == -RAY_F64) has_float = 1;
        if (RAY_ATOM_IS_NULL(elems[i])) continue;
        double v = as_f64(elems[i]);
        if (!found || v < fmin) { fmin = v; imin = elems[i]->type == -RAY_I64 ? elems[i]->i64 : 0; found = 1; }
    }
    if (!found) return ray_typed_null(has_float ? -RAY_F64 : -RAY_I64);
    return has_float ? make_f64(fmin) : make_i64(imin);
}

ray_t* ray_max_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_MAX);
    if (RAY_IS_PARTED(x->type)) return agg_parted_minmax(x, 1);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (ray_is_vec(x)) AGG_VEC_VIA_DAG(x, ray_max_op);
    if (!is_list(x)) return ray_error("type", NULL);
    int64_t len = ray_len(x);
    if (len == 0) return ray_error("domain", NULL);
    ray_t** elems = (ray_t**)ray_data(x);
    int has_float = 0, found = 0;
    double fmax = 0; int64_t imax = 0;
    for (int64_t i = 0; i < len; i++) {
        if (!is_numeric(elems[i])) return ray_error("type", NULL);
        if (elems[i]->type == -RAY_F64) has_float = 1;
        if (RAY_ATOM_IS_NULL(elems[i])) continue;
        double v = as_f64(elems[i]);
        if (!found || v > fmax) { fmax = v; imax = elems[i]->type == -RAY_I64 ? elems[i]->i64 : 0; found = 1; }
    }
    if (!found) return ray_typed_null(has_float ? -RAY_F64 : -RAY_I64);
    return has_float ? make_f64(fmax) : make_i64(imax);
}

ray_t* ray_first_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_FIRST);
    /* String first: return first char */
    if (ray_is_atom(x) && (-x->type) == RAY_STR) {
        size_t slen = ray_str_len(x);
        if (slen == 0) return ray_error("domain", NULL);
        const char* p = ray_str_ptr(x);
        return ray_str(p, 1);
    }
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    /* Table first: return first row as dict */
    if (x->type == RAY_TABLE) {
        if (ray_table_nrows(x) == 0) return ray_error("domain", NULL);
        ray_t* idx = make_i64(0);
        ray_t* result = ray_at_fn(x, idx);
        ray_release(idx);
        return result;
    }
    if (ray_is_vec(x)) {
        if (ray_len(x) == 0) return ray_typed_null(-x->type);
        /* For non-I64/F64 types route through collection_elem which
         * preserves the element type.  The DAG path widens to i64 for
         * DATE/TIME/TIMESTAMP/BOOL/U8 — bypass it. */
        if (x->type == RAY_SYM   || x->type == RAY_I32  || x->type == RAY_I16 ||
            x->type == RAY_GUID  || x->type == RAY_STR  || x->type == RAY_BOOL ||
            x->type == RAY_U8    || x->type == RAY_DATE || x->type == RAY_TIME ||
            x->type == RAY_TIMESTAMP) {
            int alloc = 0;
            return collection_elem(x, 0, &alloc);
        }
        AGG_VEC_VIA_DAG(x, ray_first);
    }
    if (!is_list(x)) return ray_error("type", NULL);
    if (ray_len(x) == 0) return ray_typed_null(-RAY_I64);
    ray_t* elem = ((ray_t**)ray_data(x))[0];
    ray_retain(elem);
    return elem;
}

ray_t* ray_last_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_LAST);
    /* String last: return last char */
    if (ray_is_atom(x) && (-x->type) == RAY_STR) {
        size_t slen = ray_str_len(x);
        if (slen == 0) return ray_error("domain", NULL);
        const char* p = ray_str_ptr(x);
        return ray_str(p + slen - 1, 1);
    }
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    /* Table last: return last row as dict */
    if (x->type == RAY_TABLE) {
        int64_t nrows = ray_table_nrows(x);
        if (nrows == 0) return ray_error("domain", NULL);
        ray_t* idx = make_i64(nrows - 1);
        ray_t* result = ray_at_fn(x, idx);
        ray_release(idx);
        return result;
    }
    if (ray_is_vec(x)) {
        if (ray_len(x) == 0) return ray_typed_null(-x->type);
        /* See ray_first_fn for rationale on the type whitelist. */
        if (x->type == RAY_SYM   || x->type == RAY_I32  || x->type == RAY_I16 ||
            x->type == RAY_GUID  || x->type == RAY_STR  || x->type == RAY_BOOL ||
            x->type == RAY_U8    || x->type == RAY_DATE || x->type == RAY_TIME ||
            x->type == RAY_TIMESTAMP) {
            int alloc = 0;
            return collection_elem(x, ray_len(x) - 1, &alloc);
        }
        AGG_VEC_VIA_DAG(x, ray_last);
    }
    if (!is_list(x)) return ray_error("type", NULL);
    int64_t len = ray_len(x);
    if (len == 0) return ray_typed_null(-RAY_I64);
    ray_t* elem = ((ray_t**)ray_data(x))[len - 1];
    ray_retain(elem);
    return elem;
}

/* Helper: copy non-null vec elements to double scratch buffer, compacted.
 * scratch->len is set to the number of non-null values copied.
 * Returns scratch ray_t* (caller must ray_release), or error. */
static ray_t* vec_to_f64_scratch(ray_t* x, double** out_vals) {
    int64_t len = ray_len(x);
    ray_t* scratch = ray_alloc(len * sizeof(double));
    if (!scratch) return ray_error("oom", NULL);
    scratch->type = RAY_F64;
    double* vals = (double*)ray_data(scratch);
    int64_t cnt = 0;
    if (x->type == RAY_I64) {
        int64_t* d = (int64_t*)ray_data(x);
        for (int64_t i = 0; i < len; i++) { if (!ray_vec_is_null(x, i)) vals[cnt++] = (double)d[i]; }
    } else if (x->type == RAY_F64) {
        double* d = (double*)ray_data(x);
        for (int64_t i = 0; i < len; i++) { if (!ray_vec_is_null(x, i)) vals[cnt++] = d[i]; }
    } else if (x->type == RAY_I32) {
        int32_t* d = (int32_t*)ray_data(x);
        for (int64_t i = 0; i < len; i++) { if (!ray_vec_is_null(x, i)) vals[cnt++] = (double)d[i]; }
    } else if (x->type == RAY_I16) {
        int16_t* d = (int16_t*)ray_data(x);
        for (int64_t i = 0; i < len; i++) { if (!ray_vec_is_null(x, i)) vals[cnt++] = (double)d[i]; }
    } else if (x->type == RAY_U8) {
        uint8_t* d = (uint8_t*)ray_data(x);
        for (int64_t i = 0; i < len; i++) { if (!ray_vec_is_null(x, i)) vals[cnt++] = (double)d[i]; }
    } else {
        ray_release(scratch);
        return ray_error("type", NULL);
    }
    scratch->len = cnt;
    *out_vals = vals;
    return scratch;
}

ray_t* ray_med_fn(ray_t* x) {
    /* Note: after Phase 1.5 the dispatcher always materialises non-LAZY_AWARE
     * fn args, so x is already concrete here.  The inline materialise guard
     * that was here was unreachable and has been removed. */
    if (RAY_IS_ERR(x)) return x;
    /* Scalar: median of single value → f64 */
    if (ray_is_atom(x)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
        if (is_numeric(x)) return make_f64(as_f64(x));
        return ray_error("type", NULL);
    }
    int64_t len;
    ray_t* scratch = NULL;
    double* vals = NULL;

    if (ray_is_vec(x)) {
        len = ray_len(x);
        if (len == 0) return ray_typed_null(-RAY_F64);
        scratch = vec_to_f64_scratch(x, &vals);
        if (RAY_IS_ERR(scratch)) return scratch;
    } else if (is_list(x)) {
        len = ray_len(x);
        if (len == 0) return ray_typed_null(-RAY_F64);
        ray_t** elems = (ray_t**)ray_data(x);
        scratch = ray_alloc(len * sizeof(double));
        if (!scratch) return ray_error("oom", NULL);
        scratch->type = RAY_F64;
        scratch->len = 0;
        vals = (double*)ray_data(scratch);
        int64_t cnt_l = 0;
        for (int64_t i = 0; i < len; i++) {
            if (ray_is_atom(elems[i]) && RAY_ATOM_IS_NULL(elems[i])) continue;
            if (!is_numeric(elems[i])) { ray_release(scratch); return ray_error("type", NULL); }
            vals[cnt_l++] = as_f64(elems[i]);
        }
        scratch->len = cnt_l;
    } else {
        return ray_error("type", NULL);
    }

    /* scratch->len holds the count of non-null values (already compacted) */
    int64_t cnt = scratch->len;
    if (cnt == 0) { ray_release(scratch); return ray_typed_null(-RAY_F64); }

    /* O(n) average partial-sort.  Two-call quantile-interpolator
     * pattern: for odd n one nth_element places the middle; for even
     * n a second nth_element on the right half locates the upper
     * middle.  Replaces an O(n^2) insertion sort that hung for
     * groups larger than ~10k. */
    int64_t k = cnt / 2;
    double median;
    if (cnt % 2 == 1) {
        nth_element_dbl(vals, 0, cnt - 1, k);
        median = vals[k];
    } else {
        nth_element_dbl(vals, 0, cnt - 1, k - 1);
        nth_element_dbl(vals, k, cnt - 1, k);
        median = (vals[k - 1] + vals[k]) / 2.0;
    }
    ray_release(scratch);
    return make_f64(median);
}

static ray_t* var_stddev_core(ray_t* x, int sample, int take_sqrt);


ray_t* ray_dev_fn(ray_t* x) { return var_stddev_core(x, 0, 1); }

/* Shared core for variance / stddev in sample or population mode.
 * sample=1 -> divide sum-of-squares by (n-1); sample=0 -> divide by n.
 * take_sqrt=1 -> stddev; take_sqrt=0 -> variance. */
static ray_t* var_stddev_core(ray_t* x, int sample, int take_sqrt) {
    /* Note: after Phase 1.5 the dispatcher always materialises non-LAZY_AWARE
     * fn args, so x is already concrete here.  The inline materialise guard
     * that was here was unreachable and has been removed. */
    if (RAY_IS_ERR(x)) return x;
    if (ray_is_atom(x)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
        if (is_numeric(x)) return sample ? ray_typed_null(-RAY_F64) : make_f64(0.0);
        return ray_error("type", NULL);
    }

    double* vals = NULL;
    ray_t*  scratch = NULL;
    int64_t cnt = 0;

    if (ray_is_vec(x)) {
        if (ray_len(x) == 0) return ray_typed_null(-RAY_F64);
        scratch = vec_to_f64_scratch(x, &vals);
        if (RAY_IS_ERR(scratch)) return scratch;
        cnt = scratch->len;
    } else if (is_list(x)) {
        int64_t len = ray_len(x);
        if (len == 0) return ray_typed_null(-RAY_F64);
        ray_t** elems = (ray_t**)ray_data(x);
        /* Use a fresh f64 vec as a scratch buffer so we reuse the vec path's cleanup. */
        scratch = ray_vec_new(RAY_F64, len);
        if (RAY_IS_ERR(scratch)) return scratch;
        vals = (double*)ray_data(scratch);
        for (int64_t i = 0; i < len; i++) {
            if (!is_numeric(elems[i])) { ray_release(scratch); return ray_error("type", NULL); }
            if (!RAY_ATOM_IS_NULL(elems[i])) vals[cnt++] = as_f64(elems[i]);
        }
        scratch->len = cnt;
    } else {
        return ray_error("type", NULL);
    }

    if (cnt == 0 || (sample && cnt <= 1)) {
        ray_release(scratch);
        return ray_typed_null(-RAY_F64);
    }

    double sum = 0.0;
    for (int64_t i = 0; i < cnt; i++) sum += vals[i];
    double mean = sum / (double)cnt;
    double sqdiff = 0.0;
    for (int64_t i = 0; i < cnt; i++) { double d = vals[i] - mean; sqdiff += d * d; }
    ray_release(scratch);
    double divisor = sample ? (double)(cnt - 1) : (double)cnt;
    double v = sqdiff / divisor;
    return make_f64(take_sqrt ? sqrt(v) : v);
}

ray_t* ray_stddev_fn(ray_t* x)     { return var_stddev_core(x, 1, 1); }
ray_t* ray_stddev_pop_fn(ray_t* x) { return var_stddev_core(x, 0, 1); }
ray_t* ray_var_fn(ray_t* x)        { return var_stddev_core(x, 1, 0); }
ray_t* ray_var_pop_fn(ray_t* x)    { return var_stddev_core(x, 0, 0); }
