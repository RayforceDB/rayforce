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

/*  Collection / higher-order builtins — extracted from eval.c  */

#include "lang/internal.h"
#include "core/types.h"
#include "core/pool.h"
#include "mem/sys.h"
#include "ops/hash.h"
#include "ops/internal.h"   /* col_propagate_str_pool */
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════
 * Open-addressing hash set used by distinct/union/except/sect/in
 * to replace O(n×m) linear scans with O(n+m) hash lookups.
 *
 * Slots store the row index of the first occurrence in a single vec,
 * or HS_EMPTY.  Hashing and equality are dispatched through a small
 * vtable keyed by collection type so the same set can probe a typed
 * vec or a boxed RAY_LIST without per-call boxing on the typed path.
 * Nulls aggregate into a separate null-bucket flag, mirroring atom_eq
 * (all nulls compare equal regardless of typed-null vs RAY_NULL_OBJ).
 *
 * Load factor capped at 0.5; growth doubles capacity.
 * ══════════════════════════════════════════ */

#define HS_EMPTY ((int64_t)-1)

typedef struct hashset_t {
    int64_t* slots;        /* cap entries; -1 = empty, else row index */
    ray_t*   block;        /* backing alloc for slots */
    int64_t  cap;          /* power of 2 */
    int64_t  mask;         /* cap - 1 */
    int64_t  count;        /* live entries (excl. null bucket) */
    int      null_seen;    /* 1 if any null has been recorded */
    int64_t  null_idx;     /* row index of first null encountered */
    /* Cached typed-vec data pointers to avoid re-derefs in hot loops. */
    ray_t*   src;          /* source vec or list */
    int8_t   src_type;     /* ray_t.type */
    bool     src_has_nulls;
    void*    src_data;     /* pointer to typed data (or RAY_LIST elements) */
} hashset_t;

/* Hash a single row at index i in src.  Mirrors atom_eq's coercion
 * rules: numeric types normalize through f64 so an I64 atom and an
 * F64 atom holding the same value collide (boxed-list path only — a
 * typed vec is homogeneous, so the dispatch picks one branch). */
static uint64_t hs_hash_row(ray_t* src, int64_t i, int8_t t, void* data) {
    switch (t) {
        case RAY_I64:       return ray_hash_i64(((const int64_t*)data)[i]);
        case RAY_I32:       return ray_hash_i64((int64_t)((const int32_t*)data)[i]);
        case RAY_I16:       return ray_hash_i64((int64_t)((const int16_t*)data)[i]);
        case RAY_U8:        return ray_hash_i64((int64_t)((const uint8_t*)data)[i]);
        case RAY_BOOL:      return ray_hash_i64((int64_t)((const bool*)data)[i]);
        case RAY_F64:       return ray_hash_f64(((const double*)data)[i]);
        case RAY_DATE:      return ray_hash_i64((int64_t)((const int32_t*)data)[i]);
        case RAY_TIME:      return ray_hash_i64((int64_t)((const int32_t*)data)[i]);
        case RAY_TIMESTAMP: return ray_hash_i64(((const int64_t*)data)[i]);
        case RAY_SYM: {
            uint64_t s = ray_read_sym(data, i, src->type, src->attrs);
            return ray_hash_i64((int64_t)s);
        }
        case RAY_GUID:
            return ray_hash_bytes((const uint8_t*)data + i * 16, 16);
        case RAY_STR: {
            size_t l = 0;
            const char* p = ray_str_vec_get(src, i, &l);
            return p ? ray_hash_bytes(p, l) : 0;
        }
        case RAY_LIST: {
            ray_t** elems = (ray_t**)data;
            ray_t* e = elems[i];
            if (!e || RAY_ATOM_IS_NULL(e)) return 0;
            /* Numeric coercion: hash as f64 so distinct numeric types
             * holding the same value collide (atom_eq does the same). */
            if (is_numeric(e)) return ray_hash_f64(as_f64(e));
            switch (e->type) {
                case -RAY_SYM:       return ray_hash_i64(e->i64);
                case -RAY_DATE:
                case -RAY_TIME:      return ray_hash_i64((int64_t)e->i32);
                case -RAY_TIMESTAMP: return ray_hash_i64(e->i64);
                case -RAY_GUID: {
                    const uint8_t* g = e->obj
                        ? (const uint8_t*)ray_data(e->obj)
                        : (const uint8_t*)ray_data((ray_t*)e);
                    return ray_hash_bytes(g, 16);
                }
                case -RAY_STR:
                    return ray_hash_bytes(ray_str_ptr(e), ray_str_len(e));
                default:
                    /* Unknown / unsupported atom kind: hash by type tag. */
                    return ray_hash_i64((int64_t)e->type);
            }
        }
        default:
            return ray_hash_i64(i);
    }
}

/* Compare two rows for equality, dispatched the same way as the hash. */
static int hs_eq_rows(ray_t* a_src, int64_t ai, int8_t at, void* a_data,
                      ray_t* b_src, int64_t bi, int8_t bt, void* b_data) {
    if (at == bt && at != RAY_LIST) {
        switch (at) {
            case RAY_I64:       return ((const int64_t*)a_data)[ai] == ((const int64_t*)b_data)[bi];
            case RAY_I32:       return ((const int32_t*)a_data)[ai] == ((const int32_t*)b_data)[bi];
            case RAY_I16:       return ((const int16_t*)a_data)[ai] == ((const int16_t*)b_data)[bi];
            case RAY_U8:        return ((const uint8_t*)a_data)[ai] == ((const uint8_t*)b_data)[bi];
            case RAY_BOOL:      return ((const bool*)a_data)[ai] == ((const bool*)b_data)[bi];
            case RAY_F64:       return ((const double*)a_data)[ai] == ((const double*)b_data)[bi];
            case RAY_DATE:
            case RAY_TIME:      return ((const int32_t*)a_data)[ai] == ((const int32_t*)b_data)[bi];
            case RAY_TIMESTAMP: return ((const int64_t*)a_data)[ai] == ((const int64_t*)b_data)[bi];
            case RAY_SYM: {
                uint64_t sa = ray_read_sym(a_data, ai, a_src->type, a_src->attrs);
                uint64_t sb = ray_read_sym(b_data, bi, b_src->type, b_src->attrs);
                return sa == sb;
            }
            case RAY_GUID:
                return memcmp((const uint8_t*)a_data + ai * 16,
                              (const uint8_t*)b_data + bi * 16, 16) == 0;
            case RAY_STR: {
                size_t al = 0, bl = 0;
                const char* ap = ray_str_vec_get(a_src, ai, &al);
                const char* bp = ray_str_vec_get(b_src, bi, &bl);
                if (!ap) ap = "";
                if (!bp) bp = "";
                return al == bl && memcmp(ap, bp, al) == 0;
            }
        }
    }
    /* Fall back to atom_eq via boxed values.  Used for cross-type
     * comparisons (e.g. except over typed I64 vs F64 vec) and the
     * RAY_LIST path.  collection_elem allocates a temporary atom for
     * typed vecs; the cost is paid only on collisions / mixed types. */
    int alloc_a = 0, alloc_b = 0;
    ray_t* a = collection_elem(a_src, ai, &alloc_a);
    ray_t* b = collection_elem(b_src, bi, &alloc_b);
    int eq = (a && b) ? atom_eq(a, b) : 0;
    if (alloc_a && a) ray_release(a);
    if (alloc_b && b) ray_release(b);
    return eq;
}

/* True if the row at index i in src is null. */
static inline int hs_row_is_null(ray_t* src, int64_t i, void* data) {
    if (src->type == RAY_LIST) {
        ray_t* e = ((ray_t**)data)[i];
        return !e || RAY_ATOM_IS_NULL(e);
    }
    return (src->attrs & RAY_ATTR_HAS_NULLS) && ray_vec_is_null(src, i);
}

static bool hashset_init(hashset_t* hs, ray_t* src, int64_t hint) {
    int64_t cap = 16;
    /* Cap target: 2× hint to keep load factor under 0.5. */
    while (cap < (hint > 0 ? hint * 2 : 16)) cap *= 2;
    hs->block = ray_alloc((size_t)cap * sizeof(int64_t));
    if (!hs->block || RAY_IS_ERR(hs->block)) { hs->block = NULL; return false; }
    hs->slots = (int64_t*)ray_data(hs->block);
    for (int64_t i = 0; i < cap; i++) hs->slots[i] = HS_EMPTY;
    hs->cap = cap;
    hs->mask = cap - 1;
    hs->count = 0;
    hs->null_seen = 0;
    hs->null_idx = HS_EMPTY;
    hs->src = src;
    hs->src_type = src ? src->type : 0;
    hs->src_has_nulls = src ? ((src->attrs & RAY_ATTR_HAS_NULLS) != 0) : false;
    hs->src_data = src ? ray_data(src) : NULL;
    return true;
}

static void hashset_destroy(hashset_t* hs) {
    if (hs->block) { ray_release(hs->block); hs->block = NULL; }
    hs->slots = NULL;
}

static bool hashset_grow(hashset_t* hs) {
    int64_t old_cap = hs->cap;
    int64_t* old_slots = hs->slots;
    int64_t new_cap = old_cap * 2;
    if (new_cap < old_cap) return false;
    ray_t* nb = ray_alloc((size_t)new_cap * sizeof(int64_t));
    if (!nb || RAY_IS_ERR(nb)) return false;
    int64_t* ns = (int64_t*)ray_data(nb);
    for (int64_t i = 0; i < new_cap; i++) ns[i] = HS_EMPTY;
    int64_t mask = new_cap - 1;
    for (int64_t i = 0; i < old_cap; i++) {
        int64_t ridx = old_slots[i];
        if (ridx == HS_EMPTY) continue;
        uint64_t h = hs_hash_row(hs->src, ridx, hs->src_type, hs->src_data);
        int64_t s = (int64_t)(h & (uint64_t)mask);
        while (ns[s] != HS_EMPTY) s = (s + 1) & mask;
        ns[s] = ridx;
    }
    ray_release(hs->block);
    hs->block = nb;
    hs->slots = ns;
    hs->cap = new_cap;
    hs->mask = mask;
    return true;
}

/* Probe the set for the row (probe_src, probe_i).  Returns the stored
 * row index from the build-side vec on hit, HS_EMPTY on miss. */
static int64_t hashset_find_xrow(hashset_t* hs, ray_t* probe_src, int64_t probe_i,
                                  int8_t probe_type, void* probe_data) {
    if (hs_row_is_null(probe_src, probe_i, probe_data))
        return hs->null_seen ? hs->null_idx : HS_EMPTY;
    uint64_t h = hs_hash_row(probe_src, probe_i, probe_type, probe_data);
    int64_t s = (int64_t)(h & (uint64_t)hs->mask);
    while (hs->slots[s] != HS_EMPTY) {
        int64_t stored = hs->slots[s];
        if (hs_eq_rows(probe_src, probe_i, probe_type, probe_data,
                       hs->src,    stored,   hs->src_type, hs->src_data))
            return stored;
        s = (s + 1) & hs->mask;
    }
    return HS_EMPTY;
}

/* qsort comparator state for distinct_sort_indices: thread-local so
 * the standard qsort entry point can pull it without a context arg.
 * Single-threaded VM-eval is the only caller, so TLS is fine. */
static _Thread_local ray_t* g_dsort_src;
static _Thread_local int8_t g_dsort_type;
static _Thread_local const void* g_dsort_data;

static int distinct_sort_cmp(const void* a, const void* b) {
    int64_t ia = *(const int64_t*)a;
    int64_t ib = *(const int64_t*)b;
    switch (g_dsort_type) {
        case RAY_I64: case RAY_TIMESTAMP: {
            int64_t va = ((const int64_t*)g_dsort_data)[ia];
            int64_t vb = ((const int64_t*)g_dsort_data)[ib];
            return (va > vb) - (va < vb);
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            int32_t va = ((const int32_t*)g_dsort_data)[ia];
            int32_t vb = ((const int32_t*)g_dsort_data)[ib];
            return (va > vb) - (va < vb);
        }
        case RAY_I16: {
            int16_t va = ((const int16_t*)g_dsort_data)[ia];
            int16_t vb = ((const int16_t*)g_dsort_data)[ib];
            return (va > vb) - (va < vb);
        }
        case RAY_U8: case RAY_BOOL: {
            uint8_t va = ((const uint8_t*)g_dsort_data)[ia];
            uint8_t vb = ((const uint8_t*)g_dsort_data)[ib];
            return (va > vb) - (va < vb);
        }
        case RAY_F64: {
            double va = ((const double*)g_dsort_data)[ia];
            double vb = ((const double*)g_dsort_data)[ib];
            return (va > vb) - (va < vb);
        }
        default: {
            /* Fall back to boxed compare for less-common element kinds. */
            int alloc_a = 0, alloc_b = 0;
            ray_t* a_e = collection_elem(g_dsort_src, ia, &alloc_a);
            ray_t* b_e = collection_elem(g_dsort_src, ib, &alloc_b);
            double va = a_e ? as_f64(a_e) : 0.0;
            double vb = b_e ? as_f64(b_e) : 0.0;
            if (alloc_a && a_e) ray_release(a_e);
            if (alloc_b && b_e) ray_release(b_e);
            return (va > vb) - (va < vb);
        }
    }
}

/* Sort `count` indices by their numeric value in `src`.  Preserves
 * the existing `distinct` semantic of returning numeric output sorted. */
static void distinct_sort_indices(ray_t* src, int64_t* idx, int64_t count) {
    g_dsort_src = src;
    g_dsort_type = src->type;
    g_dsort_data = ray_data(src);
    qsort(idx, (size_t)count, sizeof(int64_t), distinct_sort_cmp);
}

/* Insert row i (from the set's build-side src) if absent.
 * Returns true if newly inserted, false if duplicate.  On grow OOM
 * the set silently keeps the previous capacity (caller proceeds). */
static bool hashset_insert(hashset_t* hs, int64_t i) {
    if (hs_row_is_null(hs->src, i, hs->src_data)) {
        if (hs->null_seen) return false;
        hs->null_seen = 1;
        hs->null_idx = i;
        return true;
    }
    if (hs->count * 2 >= hs->cap) {
        if (!hashset_grow(hs)) { /* fall through, may degrade */ }
    }
    uint64_t h = hs_hash_row(hs->src, i, hs->src_type, hs->src_data);
    int64_t s = (int64_t)(h & (uint64_t)hs->mask);
    while (hs->slots[s] != HS_EMPTY) {
        int64_t stored = hs->slots[s];
        if (hs_eq_rows(hs->src, i,      hs->src_type, hs->src_data,
                       hs->src, stored, hs->src_type, hs->src_data))
            return false;
        s = (s + 1) & hs->mask;
    }
    hs->slots[s] = i;
    hs->count++;
    return true;
}

/* ══════════════════════════════════════════
 * Higher-order functions
 * ══════════════════════════════════════════ */

/* (map fn val vec) — apply binary fn(val, elem) to each element of vec.
 * Also supports (map fn vec) for unary mapping. */
ray_t* ray_map_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* _bx = NULL;

    if (n == 2) {
        /* Unary map: (map fn vec) */
        ray_t* vec = unbox_vec_arg(args[1], &_bx);
        if (RAY_IS_ERR(vec)) return vec;
        if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
        int64_t len = ray_len(vec);
        ray_t* result = ray_alloc(len * sizeof(ray_t*));
        if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
        result->type = RAY_LIST;
        result->len = len;
        ray_t** out = (ray_t**)ray_data(result);
        ray_t** elems = (ray_t**)ray_data(vec);
        for (int64_t i = 0; i < len; i++) {
            out[i] = call_fn1(fn, elems[i]);
            if (RAY_IS_ERR(out[i])) {
                for (int64_t j = 0; j < i; j++) ray_release(out[j]);
                result->len = 0; ray_release(result); if (_bx) ray_release(_bx);
                return out[i];
            }
        }
        if (_bx) ray_release(_bx);
        return result;
    }

    /* Binary map: (map fn val vec) — apply fn(val, elem) */
    ray_t* val = args[1];
    ray_t* vec = unbox_vec_arg(args[2], &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    /* If vec is scalar, just call fn(val, vec) once */
    if (!is_list(vec)) {
        if (_bx) ray_release(_bx);
        return call_fn2(fn, val, args[2]);
    }
    int64_t len = ray_len(vec);
    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        out[i] = call_fn2(fn, val, elems[i]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            result->len = 0; ray_release(result); if (_bx) ray_release(_bx);
            return out[i];
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* (pmap fn val vec) — same as map, parallel not implemented yet (sequential fallback) */
ray_t* ray_pmap_fn(ray_t** args, int64_t n) {
    return ray_map_fn(args, n);
}

/* (fold fn vec) or (fold fn init vec) — reduce with binary fn */
ray_t* ray_fold_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* vec;
    ray_t* acc;
    ray_t* _bx = NULL;
    if (n == 2) {
        /* (fold fn vec) — use first element as initial value */
        vec = unbox_vec_arg(args[1], &_bx);
        if (RAY_IS_ERR(vec)) return vec;
        if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
        int64_t len = ray_len(vec);
        if (len == 0) { if (_bx) ray_release(_bx); return ray_error("domain", NULL); }
        ray_t** elems = (ray_t**)ray_data(vec);
        ray_retain(elems[0]);
        acc = elems[0];
        for (int64_t i = 1; i < len; i++) {
            ray_t* next = call_fn2(fn, acc, elems[i]);
            ray_release(acc);
            if (RAY_IS_ERR(next)) { if (_bx) ray_release(_bx); return next; }
            acc = next;
        }
        if (_bx) ray_release(_bx);
        return acc;
    }

    /* (fold fn init vec) */
    ray_retain(args[1]);
    acc = args[1];
    vec = unbox_vec_arg(args[2], &_bx);
    if (RAY_IS_ERR(vec)) { ray_release(acc); return vec; }
    if (!is_list(vec)) { ray_release(acc); if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        ray_t* next = call_fn2(fn, acc, elems[i]);
        ray_release(acc);
        if (RAY_IS_ERR(next)) { if (_bx) ray_release(_bx); return next; }
        acc = next;
    }
    if (_bx) ray_release(_bx);
    return acc;
}

/* (scan fn vec) — running fold, returns vector of partial results */
ray_t* ray_scan_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* _bx = NULL;
    ray_t* vec = unbox_vec_arg(args[1], &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    if (len == 0) {
        if (_bx) ray_release(_bx);
        ray_t* result = ray_alloc(0);
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = 0;
        return result;
    }

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** elems = (ray_t**)ray_data(vec);

    ray_retain(elems[0]);
    out[0] = elems[0];
    for (int64_t i = 1; i < len; i++) {
        out[i] = call_fn2(fn, out[i - 1], elems[i]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            result->len = 0; ray_release(result); if (_bx) ray_release(_bx);
            return out[i];
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* (filter vec mask) — filter vector by boolean mask */
ray_t* ray_filter_fn(ray_t* vec, ray_t* mask) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    if (ray_is_lazy(mask)) mask = ray_lazy_materialize(mask);

    /* Table filter: apply mask to each column */
    if (vec->type == RAY_TABLE && ray_is_vec(mask) && mask->type == RAY_BOOL) {
        int64_t ncols = ray_table_ncols(vec);
        int64_t nrows = ray_table_nrows(vec);
        if (nrows != mask->len) return ray_error("length", NULL);
        ray_t* result = ray_table_new(ncols);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t c = 0; c < ncols; c++) {
            int64_t cn = ray_table_col_name(vec, c);
            ray_t* src_col = ray_table_get_col_idx(vec, c);
            ray_t* filtered = ray_filter_fn(src_col, mask);
            if (RAY_IS_ERR(filtered)) { ray_release(result); return filtered; }
            result = ray_table_add_col(result, cn, filtered);
            ray_release(filtered);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }

    /* String filter: STR atom + bool mask → filter characters */
    if (ray_is_atom(vec) && (-vec->type) == RAY_STR && ray_is_vec(mask) && mask->type == RAY_BOOL) {
        const char* sp = ray_str_ptr(vec);
        size_t slen = ray_str_len(vec);
        int64_t mlen = mask->len;
        if ((int64_t)slen != mlen) return ray_error("length", NULL);
        bool* mb = (bool*)ray_data(mask);
        int64_t count = 0;
        for (int64_t i = 0; i < mlen; i++) if (mb[i]) count++;
        char buf[8192];
        if ((size_t)count > sizeof(buf)) return ray_error("limit", NULL);
        int64_t j = 0;
        for (int64_t i = 0; i < mlen; i++) {
            if (mb[i]) buf[j++] = sp[i];
        }
        return ray_str(buf, (size_t)count);
    }

    /* Fast path: typed vector + typed bool mask */
    if (ray_is_vec(vec) && ray_is_vec(mask) && mask->type == RAY_BOOL) {
        int64_t len = vec->len;
        int64_t mlen = mask->len;
        if (len != mlen) return ray_error("length", NULL);
        bool* mb = (bool*)ray_data(mask);

        /* Count true values */
        int64_t count = 0;
        for (int64_t i = 0; i < len; i++) if (mb[i]) count++;

        int8_t vtype = vec->type;
        int esz = ray_elem_size(vtype);
        ray_t* result = ray_vec_new(vtype, count);
        if (RAY_IS_ERR(result)) return result;
        result->len = count;
        char* src = (char*)ray_data(vec);
        char* dst = (char*)ray_data(result);
        int64_t j = 0;
        for (int64_t i = 0; i < len; i++) {
            if (mb[i]) {
                memcpy(dst + j * esz, src + i * esz, esz);
                if (ray_vec_is_null(vec, i))
                    ray_vec_set_null(result, j, true);
                j++;
            }
        }
        return result;
    }

    /* Fallback: boxed list path */
    ray_t *_bx1 = NULL, *_bx2 = NULL;
    vec = unbox_vec_arg(vec, &_bx1);
    if (RAY_IS_ERR(vec)) return vec;
    mask = unbox_vec_arg(mask, &_bx2);
    if (RAY_IS_ERR(mask)) { if (_bx1) ray_release(_bx1); return mask; }
    if (!is_list(vec) || !is_list(mask)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    int64_t mlen = ray_len(mask);
    if (len != mlen) return ray_error("length", NULL);

    ray_t** velems = (ray_t**)ray_data(vec);
    ray_t** melems = (ray_t**)ray_data(mask);

    /* Validate mask is all booleans */
    for (int64_t i = 0; i < len; i++) {
        if (melems[i]->type != -RAY_BOOL) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    }
    /* Count true values */
    int64_t count = 0;
    for (int64_t i = 0; i < len; i++) {
        if (melems[i]->b8) count++;
    }

    ray_t* result = ray_alloc(count * sizeof(ray_t*));
    if (!result) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = count;
    ray_t** out = (ray_t**)ray_data(result);
    int64_t j = 0;
    for (int64_t i = 0; i < len; i++) {
        if (melems[i]->b8) {
            ray_retain(velems[i]);
            out[j++] = velems[i];
        }
    }
    if (_bx1) ray_release(_bx1);
    if (_bx2) ray_release(_bx2);
    return result;
}

/* (apply fn vec1 vec2) — zip-apply fn element-wise over two vectors */
ray_t* ray_apply_fn(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];

    /* If both args are scalars, just call fn(a, b) once */
    if (ray_is_atom(args[1]) && ray_is_atom(args[2]))
        return call_fn2(fn, args[1], args[2]);

    ray_t *_bx1 = NULL, *_bx2 = NULL;
    ray_t* vec1 = unbox_vec_arg(args[1], &_bx1);
    if (RAY_IS_ERR(vec1)) return vec1;
    ray_t* vec2 = unbox_vec_arg(args[2], &_bx2);
    if (RAY_IS_ERR(vec2)) { if (_bx1) ray_release(_bx1); return vec2; }
    if (!is_list(vec1) || !is_list(vec2)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    int64_t len1 = ray_len(vec1);
    int64_t len2 = ray_len(vec2);
    int64_t len = len1 < len2 ? len1 : len2;

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** e1 = (ray_t**)ray_data(vec1);
    ray_t** e2 = (ray_t**)ray_data(vec2);

    for (int64_t i = 0; i < len; i++) {
        out[i] = call_fn2(fn, e1[i], e2[i]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            result->len = 0; ray_release(result); if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2);
            return out[i];
        }
    }
    if (_bx1) ray_release(_bx1);
    if (_bx2) ray_release(_bx2);
    return result;
}

/* ══════════════════════════════════════════
 * Collection operations
 * ══════════════════════════════════════════ */

/* Helper: compare two atoms for equality (value-based) */
int atom_eq(ray_t* a, ray_t* b) {
    int a_null = RAY_ATOM_IS_NULL(a);
    int b_null = RAY_ATOM_IS_NULL(b);
    if (a_null && b_null) return 1;
    if (a_null || b_null) return 0;
    if (a->type != b->type) {
        if (is_numeric(a) && is_numeric(b))
            return as_f64(a) == as_f64(b);
        return 0;
    }
    switch (a->type) {
    case -RAY_I64:  return a->i64 == b->i64;
    case -RAY_I32:  return a->i32 == b->i32;
    case -RAY_I16:  return a->i16 == b->i16;
    case -RAY_U8:   return a->u8 == b->u8;
    case -RAY_F64:  return a->f64 == b->f64;
    case -RAY_BOOL: return a->b8 == b->b8;
    case -RAY_SYM:  return a->i64 == b->i64;
    case -RAY_DATE: case -RAY_TIME:
        return a->i32 == b->i32;
    case -RAY_TIMESTAMP:
        return a->i64 == b->i64;
    case -RAY_GUID: {
        const uint8_t* ga = a->obj ? (const uint8_t*)ray_data(a->obj) : (const uint8_t*)ray_data((ray_t*)a);
        const uint8_t* gb = b->obj ? (const uint8_t*)ray_data(b->obj) : (const uint8_t*)ray_data((ray_t*)b);
        return memcmp(ga, gb, 16) == 0;
    }
    case -RAY_STR:
        return ray_str_len(a) == ray_str_len(b) &&
               memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) == 0;
    default:
        /* Vector equality: same type and length, element-wise comparison */
        if (a->type > 0 && a->type == b->type && a->len == b->len) {
            int esz = ray_elem_size(a->type);
            return memcmp(ray_data(a), ray_data(b), (size_t)(a->len * esz)) == 0;
        }
        return 0;
    }
}

/* Forward declaration */
ray_t* list_to_typed_vec(ray_t* list, int8_t orig_vec_type);

/* Eager vector dedup — called by the DAG executor's OP_DISTINCT case.
 * Factored out so the executor doesn't go through ray_distinct_fn, which
 * is now a lazy producer for vectors and would re-wrap into a chain. */
ray_t* distinct_vec_eager(ray_t* x) {
    int64_t len = ray_len(x);
    if (len == 0) { ray_retain(x); return x; }

    int64_t idx_stack[256];
    int64_t* idx = (len <= 256) ? idx_stack : (int64_t*)ray_sys_alloc((size_t)len * sizeof(int64_t));
    if (!idx) return ray_error("oom", NULL);

    hashset_t hs;
    if (!hashset_init(&hs, x, len)) {
        if (idx != idx_stack) ray_sys_free(idx);
        return ray_error("oom", NULL);
    }
    int64_t count = 0;
    for (int64_t i = 0; i < len; i++) {
        if (hashset_insert(&hs, i)) idx[count++] = i;
    }
    hashset_destroy(&hs);

    /* Sort unique indices by value for numeric/temporal types — preserves
     * pre-existing distinct semantics.  qsort-based; was O(count^2). */
    if (x->type != RAY_SYM && x->type != RAY_GUID && x->type != RAY_STR && count > 1) {
        distinct_sort_indices(x, idx, count);
    }

    ray_t* result = gather_by_idx(x, idx, count);
    if (idx != idx_stack) ray_sys_free(idx);
    return result;
}

/* (distinct x) — remove duplicates. Dispatches on type:
 *   table → deduplicate rows (via DAG GROUP with zero aggs)
 *   vector → remove duplicate elements, preserving first occurrence
 *   string → unique chars, sorted */
ray_t* ray_distinct_fn(ray_t* x) {
    /* Extend an existing lazy chain — must come before the lazy-materialise
     * guard so the chain is preserved rather than collapsed. */
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_DISTINCT);

    /* Table distinct: dispatch to table-specific implementation */
    if (x->type == RAY_TABLE)
        return ray_table_distinct_fn(x);

    /* String distinct: unique chars, sorted */
    if (ray_is_atom(x) && (-x->type) == RAY_STR) {
        const char* sp = ray_str_ptr(x);
        size_t slen = ray_str_len(x);
        if (slen == 0) { ray_retain(x); return x; }
        char uniq[256];
        int nu = 0;
        for (size_t i = 0; i < slen; i++) {
            int dup = 0;
            for (int j = 0; j < nu; j++) { if (uniq[j] == sp[i]) { dup = 1; break; } }
            if (!dup && nu < 256) uniq[nu++] = sp[i];
        }
        /* Sort */
        for (int i = 0; i < nu - 1; i++)
            for (int j = i + 1; j < nu; j++)
                if ((unsigned char)uniq[i] > (unsigned char)uniq[j]) { char t = uniq[i]; uniq[i] = uniq[j]; uniq[j] = t; }
        return ray_str(uniq, (size_t)nu);
    }

    /* Typed vector path: start a fresh lazy chain.  The DAG executor will
     * call distinct_vec_eager() when the chain is materialised. */
    if (ray_is_vec(x)) {
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return ray_error("oom", NULL);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_distinct_op(g, in);
        return ray_lazy_wrap(g, op);
    }

    ray_t* _bx = NULL;
    x = unbox_vec_arg(x, &_bx);
    if (RAY_IS_ERR(x)) return x;
    if (!is_list(x)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(x);
    if (len == 0) { if (_bx) ray_release(_bx); ray_retain(x); return x; }
    ray_t** elems = (ray_t**)ray_data(x);

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    ray_t** out = (ray_t**)ray_data(result);
    int64_t count = 0;

    for (int64_t i = 0; i < len; i++) {
        int dup = 0;
        for (int64_t j = 0; j < count; j++) {
            if (atom_eq(out[j], elems[i])) { dup = 1; break; }
        }
        if (!dup) {
            ray_retain(elems[i]);
            out[count++] = elems[i];
        }
    }
    result->len = count;
    /* Sort: atoms before vectors (scalars have negative type) */
    for (int64_t i = 0; i < count - 1; i++) {
        for (int64_t j = i + 1; j < count; j++) {
            int ai = ray_is_atom(out[i]);
            int aj = ray_is_atom(out[j]);
            if (!ai && aj) {
                ray_t* tmp = out[i]; out[i] = out[j]; out[j] = tmp;
            }
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* (in val vec) — check membership */
ray_t* ray_in_fn(ray_t* val, ray_t* vec) {
    if (ray_is_lazy(val)) val = ray_lazy_materialize(val);
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    /* STR in STR: for each char of val, check membership in vec string */
    if (ray_is_atom(val) && (-val->type) == RAY_STR && ray_is_atom(vec) && (-vec->type) == RAY_STR) {
        const char* vp = ray_str_ptr(val);
        size_t vlen = ray_str_len(val);
        const char* sp = ray_str_ptr(vec);
        size_t slen = ray_str_len(vec);
        ray_t* result = ray_vec_new(RAY_BOOL, (int64_t)vlen);
        if (RAY_IS_ERR(result)) return result;
        result->len = (int64_t)vlen;
        bool* out = (bool*)ray_data(result);
        for (size_t i = 0; i < vlen; i++) {
            out[i] = false;
            for (size_t j = 0; j < slen; j++) {
                if (vp[i] == sp[j]) { out[i] = true; break; }
            }
        }
        return result;
    }
    /* Scalar in scalar: equality check */
    if (ray_is_atom(val) && ray_is_atom(vec))
        return make_bool(atom_eq(val, vec) ? 1 : 0);
    /* STR in LIST: for each char of val, check membership in list elements */
    if (ray_is_atom(val) && (-val->type) == RAY_STR && (vec->type == RAY_LIST)) {
        const char* vp = ray_str_ptr(val);
        size_t vlen = ray_str_len(val);
        ray_t* result = ray_vec_new(RAY_BOOL, (int64_t)vlen);
        if (RAY_IS_ERR(result)) return result;
        result->len = (int64_t)vlen;
        bool* out_b = (bool*)ray_data(result);
        ray_t** list_elems = (ray_t**)ray_data(vec);
        int64_t list_len = ray_len(vec);
        for (size_t i = 0; i < vlen; i++) {
            out_b[i] = false;
            ray_t* ch = ray_str(&vp[i], 1);
            for (int64_t j = 0; j < list_len; j++) {
                if (atom_eq(ch, list_elems[j])) { out_b[i] = true; break; }
            }
            ray_release(ch);
        }
        return result;
    }
    /* Vector val: map in over each element */
    if (is_collection(val) && !ray_is_atom(val)) {
        int64_t vlen = ray_len(val);
        if (vlen == 0) {
            /* Empty collection → return empty list */
            return ray_list_new(0);
        }
        /* Hash-based fast path: both sides are typed vecs of compatible
         * shape and the result is a row-aligned bool vec.  Build a
         * hashset over `vec` once, probe per element of `val`.  Was
         * O(len(val)×len(vec)); now O(len(val)+len(vec)). */
        if (ray_is_vec(val) && ray_is_vec(vec)) {
            ray_t* result = ray_vec_new(RAY_BOOL, vlen);
            if (RAY_IS_ERR(result)) return result;
            result->len = vlen;
            bool* out = (bool*)ray_data(result);
            hashset_t hs;
            if (!hashset_init(&hs, vec, vec->len)) {
                ray_release(result);
                return ray_error("oom", NULL);
            }
            for (int64_t j = 0; j < vec->len; j++) hashset_insert(&hs, j);
            int8_t val_type = val->type;
            void*  val_data = ray_data(val);
            for (int64_t i = 0; i < vlen; i++) {
                out[i] = hashset_find_xrow(&hs, val, i, val_type, val_data) != HS_EMPTY;
            }
            hashset_destroy(&hs);
            return result;
        }
        /* Probe first element to check if result is scalar or vector */
        int alloc0 = 0;
        ray_t* e0 = collection_elem(val, 0, &alloc0);
        if (RAY_IS_ERR(e0)) return e0;
        ray_t* r0 = ray_in_fn(e0, vec);
        if (alloc0) ray_release(e0);
        if (RAY_IS_ERR(r0)) return r0;
        if (ray_is_atom(r0) && r0->type == -RAY_BOOL) {
            /* All results are scalar bools — use typed bool vector */
            ray_t* result = ray_vec_new(RAY_BOOL, vlen);
            if (RAY_IS_ERR(result)) { ray_release(r0); return result; }
            result->len = vlen;
            bool* out = (bool*)ray_data(result);
            out[0] = r0->b8;
            ray_release(r0);
            for (int64_t i = 1; i < vlen; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(result); return elem; }
                ray_t* r = ray_in_fn(elem, vec);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(r)) { ray_release(result); return r; }
                out[i] = r->b8;
                ray_release(r);
            }
            return result;
        } else {
            /* Results are non-scalar — collect as list */
            ray_t* result = ray_list_new(vlen);
            if (RAY_IS_ERR(result)) { ray_release(r0); return result; }
            result = ray_list_append(result, r0);
            ray_release(r0);
            if (RAY_IS_ERR(result)) return result;
            for (int64_t i = 1; i < vlen; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(val, i, &alloc);
                if (RAY_IS_ERR(elem)) { ray_release(result); return elem; }
                ray_t* r = ray_in_fn(elem, vec);
                if (alloc) ray_release(elem);
                if (RAY_IS_ERR(r)) { ray_release(result); return r; }
                result = ray_list_append(result, r);
                ray_release(r);
                if (RAY_IS_ERR(result)) return result;
            }
            return result;
        }
    }
    /* Typed vector: search without boxing */
    if (ray_is_vec(vec) && ray_is_atom(val)) {
        int64_t len = vec->len;
        bool has_nulls = (vec->attrs & RAY_ATTR_HAS_NULLS) != 0;
        bool val_null = RAY_ATOM_IS_NULL(val);
        if (has_nulls) {
            for (int64_t i = 0; i < len; i++) {
                if (ray_vec_is_null(vec, i)) {
                    if (val_null) return make_bool(1);
                    continue;
                }
                int alloc = 0;
                ray_t* elem = collection_elem(vec, i, &alloc);
                int eq = atom_eq(val, elem);
                if (alloc) ray_release(elem);
                if (eq) return make_bool(1);
            }
        } else {
            if (val_null) return make_bool(0); /* no nulls in vec, null val → no match */
            for (int64_t i = 0; i < len; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(vec, i, &alloc);
                int eq = atom_eq(val, elem);
                if (alloc) ray_release(elem);
                if (eq) return make_bool(1);
            }
        }
        return make_bool(0);
    }
    ray_t* _bx = NULL;
    vec = unbox_vec_arg(vec, &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        if (atom_eq(val, elems[i])) { if (_bx) ray_release(_bx); return make_bool(1); }
    }
    if (_bx) ray_release(_bx);
    return make_bool(0);
}

/* Helper: convert a boxed list result back to a typed vector if the original was typed */
ray_t* list_to_typed_vec(ray_t* list, int8_t orig_vec_type) {
    if (!list || RAY_IS_ERR(list) || list->type != RAY_LIST) return list;
    int64_t count = list->len;
    /* For SYM and STR types, only convert when empty (to get [] instead of ()) */
    if (orig_vec_type == RAY_SYM || orig_vec_type == RAY_STR) {
        if (count == 0) {
            ray_release(list);
            return ray_vec_new(orig_vec_type, 0);
        }
        return list; /* Keep as boxed list for non-empty SYM/STR */
    }
    ray_t* vec = ray_vec_new(orig_vec_type, count);
    if (RAY_IS_ERR(vec)) return vec;
    vec->len = count;
    ray_t** elems = (ray_t**)ray_data(list);
    for (int64_t i = 0; i < count; i++)
        store_typed_elem(vec, i, elems[i]);
    /* Release the list (ray_release_owned_refs handles child elements) */
    ray_release(list);
    return vec;
}

/* (vec_elem_in helper removed — replaced by hashset-based lookups in
 * except/union/sect/in/distinct.) */

/* (except vec1 vec2) — elements in vec1 not in vec2 */
ray_t* ray_except_fn(ray_t* vec1, ray_t* vec2) {
    if (ray_is_lazy(vec1)) vec1 = ray_lazy_materialize(vec1);
    if (ray_is_lazy(vec2)) vec2 = ray_lazy_materialize(vec2);

    /* Typed vector path: hash-based.  Was O(len1×len2); now O(len1+len2). */
    if (ray_is_vec(vec1) && (ray_is_vec(vec2) || ray_is_atom(vec2))) {
        int64_t len1 = vec1->len;
        int64_t idx_stack[256];
        int64_t* idx = (len1 <= 256) ? idx_stack : (int64_t*)ray_sys_alloc((size_t)len1 * sizeof(int64_t));
        if (!idx) return ray_error("oom", NULL);
        int64_t count = 0;
        if (ray_is_atom(vec2)) {
            /* Scalar: filter out matching elements (single compare per row). */
            for (int64_t i = 0; i < len1; i++) {
                int alloc = 0;
                ray_t* elem = collection_elem(vec1, i, &alloc);
                int eq = atom_eq(elem, vec2);
                if (alloc) ray_release(elem);
                if (!eq) idx[count++] = i;
            }
        } else {
            hashset_t hs;
            if (!hashset_init(&hs, vec2, vec2->len)) {
                if (idx != idx_stack) ray_sys_free(idx);
                return ray_error("oom", NULL);
            }
            for (int64_t j = 0; j < vec2->len; j++) hashset_insert(&hs, j);
            int8_t v1_type = vec1->type;
            void*  v1_data = ray_data(vec1);
            for (int64_t i = 0; i < len1; i++) {
                if (hashset_find_xrow(&hs, vec1, i, v1_type, v1_data) == HS_EMPTY)
                    idx[count++] = i;
            }
            hashset_destroy(&hs);
        }
        ray_t* result = gather_by_idx(vec1, idx, count);
        if (idx != idx_stack) ray_sys_free(idx);
        return result;
    }

    /* Boxed list fallback */
    int8_t orig_type = ray_is_vec(vec1) ? vec1->type : -1;
    ray_t *_bx1 = NULL, *_bx2 = NULL;
    vec1 = unbox_vec_arg(vec1, &_bx1);
    if (RAY_IS_ERR(vec1)) return vec1;
    vec2 = unbox_vec_arg(vec2, &_bx2);
    if (RAY_IS_ERR(vec2)) { if (_bx1) ray_release(_bx1); return vec2; }
    if (!is_list(vec1)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    int64_t len1 = ray_len(vec1);
    ray_t** e1 = (ray_t**)ray_data(vec1);

    ray_t* result = ray_alloc(len1 * sizeof(ray_t*));
    if (!result) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    ray_t** out = (ray_t**)ray_data(result);
    int64_t count = 0;

    if (ray_is_atom(vec2)) {
        for (int64_t i = 0; i < len1; i++) {
            if (!atom_eq(e1[i], vec2)) { ray_retain(e1[i]); out[count++] = e1[i]; }
        }
    } else {
        int64_t len2 = ray_len(vec2);
        ray_t** e2 = (ray_t**)ray_data(vec2);
        for (int64_t i = 0; i < len1; i++) {
            int found = 0;
            for (int64_t j = 0; j < len2; j++) {
                if (atom_eq(e1[i], e2[j])) { found = 1; break; }
            }
            if (!found) { ray_retain(e1[i]); out[count++] = e1[i]; }
        }
    }
    result->len = count;
    if (_bx1) ray_release(_bx1);
    if (_bx2) ray_release(_bx2);
    if (orig_type >= 0 && count == 0) { ray_release(result); return ray_vec_new(orig_type, 0); }
    return result;
}

/* (union vec1 vec2) — elements in vec1 + elements in vec2 not already in vec1 */
ray_t* ray_union_fn(ray_t* vec1, ray_t* vec2) {
    if (ray_is_lazy(vec1)) vec1 = ray_lazy_materialize(vec1);
    if (ray_is_lazy(vec2)) vec2 = ray_lazy_materialize(vec2);

    /* Typed vector path: hash-based.  Was O(len1×len2); now O(len1+len2). */
    if (ray_is_vec(vec1) && ray_is_vec(vec2)) {
        int64_t len2 = vec2->len;
        int64_t idx_stack[256];
        int64_t* idx = (len2 <= 256) ? idx_stack : (int64_t*)ray_sys_alloc((size_t)len2 * sizeof(int64_t));
        if (!idx) return ray_error("oom", NULL);
        hashset_t hs;
        if (!hashset_init(&hs, vec1, vec1->len)) {
            if (idx != idx_stack) ray_sys_free(idx);
            return ray_error("oom", NULL);
        }
        for (int64_t j = 0; j < vec1->len; j++) hashset_insert(&hs, j);
        int8_t v2_type = vec2->type;
        void*  v2_data = ray_data(vec2);
        int64_t extra = 0;
        for (int64_t i = 0; i < len2; i++) {
            if (hashset_find_xrow(&hs, vec2, i, v2_type, v2_data) == HS_EMPTY)
                idx[extra++] = i;
        }
        hashset_destroy(&hs);
        ray_t* part2 = gather_by_idx(vec2, idx, extra);
        if (idx != idx_stack) ray_sys_free(idx);
        if (RAY_IS_ERR(part2)) return part2;
        ray_t* result = ray_concat_fn(vec1, part2);
        ray_release(part2);
        return result;
    }

    /* Boxed list fallback */
    ray_t *_bx1 = NULL, *_bx2 = NULL;
    vec1 = unbox_vec_arg(vec1, &_bx1);
    if (RAY_IS_ERR(vec1)) return vec1;
    vec2 = unbox_vec_arg(vec2, &_bx2);
    if (RAY_IS_ERR(vec2)) { if (_bx1) ray_release(_bx1); return vec2; }
    if (!is_list(vec1) || !is_list(vec2)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    int64_t len1 = ray_len(vec1), len2 = ray_len(vec2);
    ray_t** e1 = (ray_t**)ray_data(vec1);
    ray_t** e2 = (ray_t**)ray_data(vec2);

    ray_t* result = ray_alloc((len1 + len2) * sizeof(ray_t*));
    if (!result) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    ray_t** out = (ray_t**)ray_data(result);
    int64_t count = 0;
    for (int64_t i = 0; i < len1; i++) { ray_retain(e1[i]); out[count++] = e1[i]; }
    for (int64_t i = 0; i < len2; i++) {
        int found = 0;
        for (int64_t j = 0; j < count; j++)
            if (atom_eq(out[j], e2[i])) { found = 1; break; }
        if (!found) { ray_retain(e2[i]); out[count++] = e2[i]; }
    }
    result->len = count;
    if (_bx1) ray_release(_bx1);
    if (_bx2) ray_release(_bx2);
    return result;
}

/* (sect vec1 vec2) — intersection: elements in both */
ray_t* ray_sect_fn(ray_t* vec1, ray_t* vec2) {
    if (ray_is_lazy(vec1)) vec1 = ray_lazy_materialize(vec1);
    if (ray_is_lazy(vec2)) vec2 = ray_lazy_materialize(vec2);

    /* Typed vector path: hash-based.  Was O(len1×len2); now O(len1+len2). */
    if (ray_is_vec(vec1) && ray_is_vec(vec2)) {
        int64_t len1 = vec1->len;
        int64_t idx_stack[256];
        int64_t* idx = (len1 <= 256) ? idx_stack : (int64_t*)ray_sys_alloc((size_t)len1 * sizeof(int64_t));
        if (!idx) return ray_error("oom", NULL);
        hashset_t hs;
        if (!hashset_init(&hs, vec2, vec2->len)) {
            if (idx != idx_stack) ray_sys_free(idx);
            return ray_error("oom", NULL);
        }
        for (int64_t j = 0; j < vec2->len; j++) hashset_insert(&hs, j);
        int8_t v1_type = vec1->type;
        void*  v1_data = ray_data(vec1);
        int64_t count = 0;
        for (int64_t i = 0; i < len1; i++) {
            if (hashset_find_xrow(&hs, vec1, i, v1_type, v1_data) != HS_EMPTY)
                idx[count++] = i;
        }
        hashset_destroy(&hs);
        ray_t* result = gather_by_idx(vec1, idx, count);
        if (idx != idx_stack) ray_sys_free(idx);
        return result;
    }

    /* Boxed list fallback */
    ray_t *_bx1 = NULL, *_bx2 = NULL;
    vec1 = unbox_vec_arg(vec1, &_bx1);
    if (RAY_IS_ERR(vec1)) return vec1;
    vec2 = unbox_vec_arg(vec2, &_bx2);
    if (RAY_IS_ERR(vec2)) { if (_bx1) ray_release(_bx1); return vec2; }
    if (!is_list(vec1) || !is_list(vec2)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", NULL); }
    int64_t len1 = ray_len(vec1);
    ray_t** e1 = (ray_t**)ray_data(vec1);
    ray_t** e2 = (ray_t**)ray_data(vec2);
    int64_t len2 = ray_len(vec2);

    ray_t* result = ray_alloc(len1 * sizeof(ray_t*));
    if (!result) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    ray_t** out = (ray_t**)ray_data(result);
    int64_t count = 0;
    for (int64_t i = 0; i < len1; i++) {
        for (int64_t j = 0; j < len2; j++) {
            if (atom_eq(e1[i], e2[j])) { ray_retain(e1[i]); out[count++] = e1[i]; break; }
        }
    }
    result->len = count;
    if (_bx1) ray_release(_bx1);
    if (_bx2) ray_release(_bx2);
    return result;
}

/* (take vec n) — first n elements (positive) or last |n| elements (negative) */
ray_t* ray_take_fn(ray_t* vec, ray_t* n_obj) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    /* N must be an integer (or 2-elem i64 vector for range-take).  Reject
     * floats up front: as_i64(f64) reads the bit pattern and would cause
     * e.g. (take 1.0 2.0) to attempt a 4.6-quintillion-element allocation
     * and surface as "oom" — misleading for what is really a type error. */
    if (ray_is_atom(n_obj) && n_obj->type == -RAY_F64)
        return ray_error("type", NULL);
    /* Range take: (take collection [start amount]) — slice from start for amount elements */
    if (ray_is_vec(n_obj) && n_obj->type == RAY_I64 && ray_len(n_obj) == 2) {
        int64_t* idx = (int64_t*)ray_data(n_obj);
        int64_t start = idx[0];
        int64_t amount = idx[1];
        if (amount < 0) return ray_error("length", NULL);

        /* Table range take */
        if (vec->type == RAY_TABLE) {
            int64_t ncols = ray_table_ncols(vec);
            ray_t* result = ray_table_new(ncols);
            if (RAY_IS_ERR(result)) return result;
            for (int64_t i = 0; i < ncols; i++) {
                ray_t* col = ray_table_get_col_idx(vec, i);
                int64_t name_id = ray_table_col_name(vec, i);
                ray_t* taken = ray_take_fn(col, n_obj);
                if (RAY_IS_ERR(taken)) { ray_release(result); return taken; }
                result = ray_table_add_col(result, name_id, taken);
                if (RAY_IS_ERR(result)) { ray_release(taken); return result; }
            }
            return result;
        }

        /* String range take */
        if (ray_is_atom(vec) && (-vec->type) == RAY_STR) {
            const char* s = ray_str_ptr(vec);
            int64_t slen = (int64_t)ray_str_len(vec);
            if (start < 0) start = slen + start;
            if (start < 0) start = 0;
            if (start >= slen) return ray_str("", 0);
            int64_t end = start + amount;
            if (end > slen) end = slen;
            return ray_str(s + start, (size_t)(end - start));
        }

        /* Typed vector range take */
        if (ray_is_vec(vec)) {
            int64_t len = ray_len(vec);
            if (start < 0) start = len + start;
            if (start < 0) start = 0;
            if (start >= len) {
                ray_t* empty = ray_vec_new(vec->type, 0);
                /* Empty STR result still needs the source's pool reference
                 * so downstream ops have a consistent owner. */
                if (vec->type == RAY_STR && !RAY_IS_ERR(empty))
                    col_propagate_str_pool(empty, vec);
                return empty;
            }
            int64_t end = start + amount;
            if (end > len) end = len;
            int64_t count = end - start;
            int8_t vtype = vec->type;
            int esz = ray_elem_size(vtype);
            ray_t* result = ray_vec_new(vtype, count);
            if (RAY_IS_ERR(result)) return result;
            result->len = count;
            memcpy(ray_data(result), (char*)ray_data(vec) + start * esz, (size_t)(count * esz));
            /* RAY_STR: the copied ray_str_t records still reference the
             * source's str_pool by pool_off — propagate the pool ray_t
             * (with retain) so the result owns a valid backing store. */
            if (vtype == RAY_STR) col_propagate_str_pool(result, vec);
            /* Propagate null bitmap — check parent's flag for slices */
            bool has_nulls = (vec->attrs & RAY_ATTR_HAS_NULLS) ||
                             ((vec->attrs & RAY_ATTR_SLICE) && vec->slice_parent &&
                              (vec->slice_parent->attrs & RAY_ATTR_HAS_NULLS));
            if (has_nulls) {
                for (int64_t i = 0; i < count; i++)
                    if (ray_vec_is_null(vec, start + i))
                        ray_vec_set_null(result, i, true);
            }
            return result;
        }

        /* Dict range take — slice both keys and vals in parallel. */
        if (vec->type == RAY_DICT) {
            ray_t* keys = ray_dict_keys(vec);
            ray_t* vals = ray_dict_vals(vec);
            int64_t len = keys ? keys->len : 0;
            if (start < 0) start = len + start;
            if (start < 0) start = 0;
            int64_t end = start + amount;
            if (end > len) end = len;
            if (end < start) end = start;
            int64_t count = end - start;

            ray_t* nk = ray_vec_slice(keys, start, count);
            if (!nk || RAY_IS_ERR(nk)) return nk ? nk : ray_error("oom", NULL);

            ray_t* nv;
            if (vals && vals->type == RAY_LIST) {
                nv = ray_alloc(count * sizeof(ray_t*));
                if (!nv) { ray_release(nk); return ray_error("oom", NULL); }
                nv->type = RAY_LIST;
                nv->len  = count;
                ray_t** vsrc = (ray_t**)ray_data(vals);
                ray_t** vdst = (ray_t**)ray_data(nv);
                for (int64_t i = 0; i < count; i++) {
                    vdst[i] = vsrc[start + i];
                    if (vdst[i]) ray_retain(vdst[i]);
                }
            } else {
                nv = ray_vec_slice(vals, start, count);
                if (!nv || RAY_IS_ERR(nv)) { ray_release(nk); return nv ? nv : ray_error("oom", NULL); }
            }
            return ray_dict_new(nk, nv);
        }

        /* Boxed list range take */
        if (vec->type == RAY_LIST) {
            int64_t len = ray_len(vec);
            if (start < 0) start = len + start;
            if (start < 0) start = 0;
            if (start >= len) {
                ray_t* result = ray_alloc(0);
                result->type = RAY_LIST;
                result->len = 0;
                return result;
            }
            int64_t end = start + amount;
            if (end > len) end = len;
            int64_t count = end - start;
            ray_t** elems = (ray_t**)ray_data(vec);
            ray_t* result = ray_alloc(count * sizeof(ray_t*));
            if (!result) return ray_error("oom", NULL);
            result->type = RAY_LIST;
            result->len = count;
            ray_t** out = (ray_t**)ray_data(result);
            for (int64_t i = 0; i < count; i++) {
                ray_retain(elems[start + i]);
                out[i] = elems[start + i];
            }
            return result;
        }

        return ray_error("type", NULL);
    }
    /* Char take: (take 'a' n) → string of n copies of char */
    if (ray_is_atom(vec) && vec->type == -RAY_STR && ray_str_len(vec) == 1 && ray_is_atom(n_obj) && is_numeric(n_obj)) {
        int64_t n = as_i64(n_obj);
        int64_t count = n < 0 ? -n : n;
        char buf[8192];
        if (count > (int64_t)sizeof(buf)) return ray_error("limit", NULL);
        for (int64_t i = 0; i < count; i++) buf[i] = vec->sdata[0];
        return ray_str(buf, (size_t)count);
    }
    /* Scalar take: (take value n) → repeat value n times */
    if (ray_is_atom(vec) && (-vec->type) != RAY_STR && ray_is_atom(n_obj) && is_numeric(n_obj)) {
        int64_t n = as_i64(n_obj);
        int64_t count = n < 0 ? -n : n;
        int8_t vtype = -(vec->type);
        ray_t* result = ray_vec_new(vtype, count);
        if (RAY_IS_ERR(result)) return result;
        result->len = count;
        for (int64_t i = 0; i < count; i++)
            store_typed_elem(result, i, vec);
        return result;
    }
    /* String take: (take "hello" 3) → "hel", with wrapping extension */
    if (ray_is_atom(vec) && (-vec->type) == RAY_STR && ray_is_atom(n_obj) && is_numeric(n_obj)) {
        const char* s = ray_str_ptr(vec);
        int64_t slen = (int64_t)ray_str_len(vec);
        int64_t n = as_i64(n_obj);
        int64_t abs_n = n < 0 ? -n : n;
        char buf[8192];
        if (abs_n > (int64_t)sizeof(buf)) return ray_error("limit", NULL);
        if (slen == 0) {
            return ray_str("", 0);
        }
        if (n >= 0) {
            for (int64_t i = 0; i < abs_n; i++) buf[i] = s[i % slen];
        } else {
            for (int64_t i = 0; i < abs_n; i++) {
                int64_t si = slen - (abs_n - i) % slen;
                if (si == slen) si = 0;
                buf[i] = s[si];
            }
        }
        return ray_str(buf, (size_t)abs_n);
    }
    /* Table take: apply take to each column */
    if (vec->type == RAY_TABLE && is_numeric(n_obj)) {
        int64_t ncols = ray_table_ncols(vec);
        ray_t* result = ray_table_new(ncols);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t i = 0; i < ncols; i++) {
            ray_t* col = ray_table_get_col_idx(vec, i);
            int64_t name_id = ray_table_col_name(vec, i);
            ray_t* taken = ray_take_fn(col, n_obj);
            if (RAY_IS_ERR(taken)) { ray_release(result); return taken; }
            result = ray_table_add_col(result, name_id, taken);
            if (RAY_IS_ERR(result)) { ray_release(taken); return result; }
        }
        return result;
    }
    /* Dict take: apply take to keys and vals in parallel.  Wrapping for
     * |n| > pair count works the same as for typed vectors. */
    if (vec->type == RAY_DICT && is_numeric(n_obj)) {
        ray_t* keys = ray_dict_keys(vec);
        ray_t* vals = ray_dict_vals(vec);
        if (!keys) return ray_error("type", NULL);
        ray_t* nk = ray_take_fn(keys, n_obj);
        if (RAY_IS_ERR(nk)) return nk;
        ray_t* nv = vals ? ray_take_fn(vals, n_obj) : ray_list_new(0);
        if (!nv || RAY_IS_ERR(nv)) { ray_release(nk); return nv ? nv : ray_error("oom", NULL); }
        return ray_dict_new(nk, nv);
    }
    /* Typed vector take with extension */
    if (ray_is_vec(vec) && is_numeric(n_obj)) {
        int64_t len = ray_len(vec);
        int64_t n = as_i64(n_obj);
        int64_t abs_n = n < 0 ? -n : n;
        int8_t vtype = vec->type;
        int esz = ray_elem_size(vtype);
        ray_t* result = ray_vec_new(vtype, abs_n);
        if (RAY_IS_ERR(result)) return result;
        result->len = abs_n;
        char* src = (char*)ray_data(vec);
        char* dst = (char*)ray_data(result);
        if (len == 0) {
            memset(dst, 0, (size_t)(abs_n * esz));
        } else if (n >= 0 && abs_n > 0) {
            /* Doubling tile-copy: O(log(abs_n/len)) memcpys instead of
             * abs_n calls of esz bytes each.  Invariant: after every
             * memcpy `copied` is a multiple of `len`, so dst[0..copied)
             * holds a perfect tile and we can keep doubling from dst[0].
             * The final partial copy is < copied so it stays within the
             * already-tiled prefix. */
            int64_t to_copy = abs_n < len ? abs_n : len;
            memcpy(dst, src, (size_t)(to_copy * esz));
            int64_t copied = to_copy;
            while (copied + copied <= abs_n) {
                memcpy(dst + copied * esz, dst, (size_t)(copied * esz));
                copied *= 2;
            }
            int64_t remaining = abs_n - copied;
            if (remaining > 0)
                memcpy(dst + copied * esz, dst, (size_t)(remaining * esz));
        } else if (n < 0) {
            /* Negative: take from end with wrap */
            for (int64_t i = 0; i < abs_n; i++) {
                int64_t si = len - (abs_n - i) % len;
                if (si == len) si = 0;
                memcpy(dst + i * esz, src + si * esz, esz);
            }
        }
        /* RAY_STR: the copied ray_str_t records still reference the
         * source's str_pool by pool_off — propagate the pool ray_t
         * (with retain) so the result owns a valid backing store.
         * Without this, a subsequent op (e.g. asc) would see
         * result->str_pool == NULL while the elements still point
         * past the SSO threshold, tripping the assertion in
         * ray_str_t_ptr / strsort_repack_window / strkey_cmp. */
        if (vtype == RAY_STR) col_propagate_str_pool(result, vec);
        /* Propagate null bitmap — check parent's flag for slices */
        bool has_nulls = len > 0 &&
                         ((vec->attrs & RAY_ATTR_HAS_NULLS) ||
                          ((vec->attrs & RAY_ATTR_SLICE) && vec->slice_parent &&
                           (vec->slice_parent->attrs & RAY_ATTR_HAS_NULLS)));
        if (has_nulls) {
            if (n >= 0) {
                for (int64_t i = 0; i < abs_n; i++)
                    if (ray_vec_is_null(vec, i % len))
                        ray_vec_set_null(result, i, true);
            } else {
                for (int64_t i = 0; i < abs_n; i++) {
                    int64_t si = len - (abs_n - i) % len;
                    if (si == len) si = 0;
                    if (ray_vec_is_null(vec, si))
                        ray_vec_set_null(result, i, true);
                }
            }
        }
        return result;
    }
    ray_t* _bx = NULL;
    vec = unbox_vec_arg(vec, &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec) || !is_numeric(n_obj))
        { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    int64_t n = as_i64(n_obj);
    ray_t** elems = (ray_t**)ray_data(vec);

    int64_t abs_n = n < 0 ? -n : n;
    int64_t elem_count = abs_n;
    ray_t* result = ray_alloc(elem_count * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = elem_count;
    ray_t** out = (ray_t**)ray_data(result);
    if (len == 0) {
        result->len = 0;
    } else if (n >= 0) {
        for (int64_t i = 0; i < elem_count; i++) {
            ray_retain(elems[i % len]);
            out[i] = elems[i % len];
        }
    } else {
        for (int64_t i = 0; i < elem_count; i++) {
            int64_t si = len - (elem_count - i) % len;
            if (si == len) si = 0;
            ray_retain(elems[si]);
            out[i] = elems[si];
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* (at vec idx) or (at table 'col) — index into vector or table */
ray_t* ray_at_fn(ray_t* vec, ray_t* idx) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    /* Table column access by symbol key — return the typed vector directly */
    if (vec->type == RAY_TABLE && idx->type == -RAY_SYM) {
        ray_t* col = ray_table_get_col(vec, idx->i64);
        if (!col) return ray_error("domain", NULL);
        ray_retain(col);
        return col;
    }

    /* Table row access by integer index: (at table 0) → {col1: val1, col2: val2} */
    if (vec->type == RAY_TABLE && ray_is_atom(idx) &&
        (idx->type == -RAY_I64 || idx->type == -RAY_I32 ||
         idx->type == -RAY_I16 || idx->type == -RAY_U8)) {
        int64_t row = as_i64(idx);
        int64_t nrows = ray_table_nrows(vec);
        if (row < 0 || row >= nrows) return ray_error("domain", NULL);
        int64_t ncols = ray_table_ncols(vec);
        /* Build a dict: keys SYM vec + vals LIST */
        ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, ncols);
        if (RAY_IS_ERR(keys)) return keys;
        ray_t* vals = ray_list_new(ncols);
        if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
        for (int64_t c = 0; c < ncols; c++) {
            int64_t key_id = ray_table_col_name(vec, c);
            keys = ray_vec_append(keys, &key_id);
            if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
            ray_t* col = ray_table_get_col_idx(vec, c);
            int alloc = 0;
            ray_t* val = collection_elem(col, row, &alloc);
            if (RAY_IS_ERR(val)) { ray_release(keys); ray_release(vals); return val; }
            vals = ray_list_append(vals, val);
            if (alloc) ray_release(val);
            if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
        }
        return ray_dict_new(keys, vals);
    }

    /* Dict key access: (at dict key) → value or 0Nl if missing */
    if (vec->type == RAY_DICT) {
        ray_t* v = ray_dict_get(vec, idx);
        if (v) return v;
        return ray_typed_null(-RAY_I64); /* 0Nl for missing key */
    }

    /* String indexing: (at "hello" 1) → 'e', (at "hello" [0 4]) → "ho" */
    if (ray_is_atom(vec) && (-vec->type) == RAY_STR) {
        const char* s = ray_str_ptr(vec);
        size_t slen = ray_str_len(vec);
        if (is_collection(idx)) {
            /* Multiple indices → build string from chars */
            int64_t idxlen = ray_len(idx);
            char buf[8192];
            if ((size_t)idxlen > sizeof(buf)) return ray_error("limit", NULL);
            for (int64_t j = 0; j < idxlen; j++) {
                int alloc = 0;
                ray_t* ie = collection_elem(idx, j, &alloc);
                int64_t k = as_i64(ie);
                if (alloc) ray_release(ie);
                if (k < 0 || (size_t)k >= slen) return ray_error("domain", NULL);
                buf[j] = s[k];
            }
            return ray_str(buf, (size_t)idxlen);
        }
        int64_t i = as_i64(idx);
        if (i < 0 || (size_t)i >= slen) return ray_error("domain", NULL);
        /* Return 1-char string atom */
        return ray_str(&s[i], 1);
    }

    /* Vector index: (at vec [i j k]) → vector of values */
    if (is_collection(idx) && idx->type != -RAY_SYM) {
        int64_t idxlen = ray_len(idx);
        ray_t* result = ray_alloc(idxlen * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = idxlen;
        ray_t** out = (ray_t**)ray_data(result);
        for (int64_t j = 0; j < idxlen; j++) {
            int alloc = 0;
            ray_t* idx_elem = collection_elem(idx, j, &alloc);
            if (RAY_IS_ERR(idx_elem)) {
                for (int64_t k = 0; k < j; k++) ray_release(out[k]);
                ray_release(result);
                return idx_elem;
            }
            ray_t* sub_idx = idx_elem;
            ray_t* val = ray_at_fn(vec, sub_idx);
            if (alloc) ray_release(idx_elem);
            if (RAY_IS_ERR(val)) {
                for (int64_t k = 0; k < j; k++) ray_release(out[k]);
                ray_release(result);
                return val;
            }
            out[j] = val;
        }
        return result;
    }

    if (idx->type != -RAY_I64 && idx->type != -RAY_I32 &&
        idx->type != -RAY_I16 && idx->type != -RAY_U8)
        return ray_error("type", NULL);
    int64_t i = as_i64(idx);

    /* Typed vector: extract element directly */
    if (ray_is_vec(vec)) {
        int64_t len = ray_len(vec);
        if (i < 0 || i >= len) return ray_typed_null(-vec->type); /* out of bounds → typed null */
        int alloc = 0;
        ray_t* elem = collection_elem(vec, i, &alloc);
        /* collection_elem always allocates for typed vecs, so elem is owned */
        return elem;
    }

    if (!is_list(vec)) return ray_error("type", NULL);
    int64_t len = ray_len(vec);
    if (i < 0 || i >= len) return ray_typed_null(-RAY_I64); /* out of bounds → 0Nl */
    ray_t* elem = ((ray_t**)ray_data(vec))[i];
    ray_retain(elem);
    return elem;
}

/* (find vec val) — index of first occurrence, or -1 */
ray_t* ray_find_fn(ray_t* vec, ray_t* val) {
    if (ray_is_lazy(vec)) vec = ray_lazy_materialize(vec);
    if (ray_is_lazy(val)) val = ray_lazy_materialize(val);
    /* String find: (find "hello" 'l') → index of char in string */
    if (ray_is_atom(vec) && (-vec->type) == RAY_STR && ray_is_atom(val) && val->type == -RAY_STR && ray_str_len(val) == 1) {
        const char* s = ray_str_ptr(vec);
        size_t slen = ray_str_len(vec);
        char c = ray_str_ptr(val)[0];
        for (size_t i = 0; i < slen; i++) {
            if (s[i] == c) return make_i64((int64_t)i);
        }
        return ray_typed_null(-RAY_I64);
    }
    /* Vector val: (find vec [v1 v2]) → [idx1 idx2] */
    if (is_collection(val)) {
        /* If vec is empty, return empty vector */
        if (is_collection(vec) && ray_len(vec) == 0)
            return ray_vec_new(RAY_I64, 0);
        int64_t vlen = ray_len(val);
        ray_t* result = ray_alloc(vlen * sizeof(ray_t*));
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = vlen;
        ray_t** out = (ray_t**)ray_data(result);
        for (int64_t j = 0; j < vlen; j++) {
            int alloc = 0;
            ray_t* ve = collection_elem(val, j, &alloc);
            out[j] = ray_find_fn(vec, ve);
            if (alloc) ray_release(ve);
            if (RAY_IS_ERR(out[j])) {
                for (int64_t k = 0; k < j; k++) ray_release(out[k]);
                ray_release(result);
                return out[j];
            }
        }
        return result;
    }
    /* Typed vector: search without boxing */
    if (ray_is_vec(vec)) {
        int64_t len = vec->len;
        bool has_nulls = (vec->attrs & RAY_ATTR_HAS_NULLS) != 0;
        bool val_null = RAY_ATOM_IS_NULL(val);
        if (has_nulls) {
            for (int64_t i = 0; i < len; i++) {
                if (ray_vec_is_null(vec, i)) {
                    if (val_null) return make_i64(i);
                    continue;
                }
                if (val_null) continue;
                int alloc = 0;
                ray_t* elem = collection_elem(vec, i, &alloc);
                int eq = atom_eq(elem, val);
                if (alloc) ray_release(elem);
                if (eq) return make_i64(i);
            }
        } else {
            if (!val_null) {
                for (int64_t i = 0; i < len; i++) {
                    int alloc = 0;
                    ray_t* elem = collection_elem(vec, i, &alloc);
                    int eq = atom_eq(elem, val);
                    if (alloc) ray_release(elem);
                    if (eq) return make_i64(i);
                }
            }
        }
        return ray_typed_null(-RAY_I64);
    }
    ray_t* _bx = NULL;
    vec = unbox_vec_arg(vec, &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        if (atom_eq(elems[i], val)) { if (_bx) ray_release(_bx); return make_i64(i); }
    }
    if (_bx) ray_release(_bx);
    return ray_typed_null(-RAY_I64); /* 0Nl = not found */
}

/* (til n) — generate integer sequence [0, 1, ..., n-1] */
static void til_fill(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    int64_t* out = (int64_t*)ctx;
    for (int64_t i = start; i < end; i++)
        out[i] = i;
}

ray_t* ray_til_fn(ray_t* x) {
    if (!ray_is_atom(x) || x->type != -RAY_I64) return ray_error("type", NULL);
    int64_t n = x->i64;
    if (n < 0) return ray_error("domain", NULL);
    if (n == 0) return ray_vec_new(RAY_I64, 0);

    ray_t* vec = ray_vec_new(RAY_I64, n);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    vec->len = n;
    int64_t* out = (int64_t*)ray_data(vec);
    ray_pool_dispatch(ray_pool_get(), til_fill, out, n);
    return vec;
}

/* Shared eager kernel — called by the OP_REVERSE executor. */
ray_t* reverse_vec_eager(ray_t* x) {
    int64_t len = x->len;
    if (len <= 1) { ray_retain(x); return x; }
    int8_t vtype = x->type;
    if (vtype == RAY_STR) {
        ray_t* result = ray_vec_new(RAY_STR, len);
        if (RAY_IS_ERR(result)) return result;
        bool has_nulls = (x->attrs & RAY_ATTR_HAS_NULLS) != 0;
        if (has_nulls) {
            for (int64_t i = 0; i < len; i++) {
                if (ray_vec_is_null(x, len - 1 - i)) {
                    result = ray_str_vec_append(result, "", 0);
                    if (!RAY_IS_ERR(result))
                        ray_vec_set_null(result, result->len - 1, true);
                } else {
                    size_t slen;
                    const char* sp = ray_str_vec_get(x, len - 1 - i, &slen);
                    result = ray_str_vec_append(result, sp ? sp : "", sp ? slen : 0);
                }
                if (RAY_IS_ERR(result)) return result;
            }
        } else {
            for (int64_t i = 0; i < len; i++) {
                size_t slen;
                const char* sp = ray_str_vec_get(x, len - 1 - i, &slen);
                result = ray_str_vec_append(result, sp ? sp : "", sp ? slen : 0);
                if (RAY_IS_ERR(result)) return result;
            }
        }
        return result;
    }
    ray_t* result = (vtype == RAY_SYM)
        ? ray_sym_vec_new(x->attrs & RAY_SYM_W_MASK, len)
        : ray_vec_new(vtype, len);
    if (!result || RAY_IS_ERR(result)) return result ? result : ray_error("oom", NULL);
    result->len = len;
    int esz = ray_elem_size(vtype);
    if (vtype == RAY_SYM) esz = ray_sym_elem_size(vtype, x->attrs);
    char* src = (char*)ray_data(x);
    char* dst = (char*)ray_data(result);
    bool has_nulls = (x->attrs & RAY_ATTR_HAS_NULLS) != 0;
    if (has_nulls) {
        for (int64_t i = 0; i < len; i++) {
            memcpy(dst + i * esz, src + (len - 1 - i) * esz, esz);
            if (ray_vec_is_null(x, len - 1 - i))
                ray_vec_set_null(result, i, true);
        }
    } else {
        for (int64_t i = 0; i < len; i++)
            memcpy(dst + i * esz, src + (len - 1 - i) * esz, esz);
    }
    return result;
}

/* (reverse vec) — reverse a vector */
ray_t* ray_reverse_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;

    /* Extend an existing lazy chain. */
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_REVERSE);

    if (ray_is_atom(x)) { ray_retain(x); return x; }

    /* Typed vector path: start a fresh lazy chain.  The DAG executor will
     * call reverse_vec_eager() when the chain is materialised. */
    if (ray_is_vec(x)) {
        ray_graph_t* g = ray_graph_new(NULL);
        if (!g) return ray_error("oom", NULL);
        ray_op_t* in = ray_graph_input_vec(g, x);
        ray_op_t* op = ray_reverse_op(g, in);
        return ray_lazy_wrap(g, op);
    }

    /* Boxed list path — eager (DAG cannot yet express list reversal) */
    ray_t* _bx = NULL;
    x = unbox_vec_arg(x, &_bx);
    if (RAY_IS_ERR(x)) return x;
    if (!is_list(x)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(x);
    ray_t** elems = (ray_t**)ray_data(x);

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    for (int64_t i = 0; i < len; i++) {
        ray_retain(elems[len - 1 - i]);
        out[i] = elems[len - 1 - i];
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* ══════════════════════════════════════════
 * Binary search
 * ══════════════════════════════════════════ */

/* (rand n max) → vector of n random i64 in [0, max) */
ray_t* ray_rand_fn(ray_t* a, ray_t* b) {
    if (!ray_is_atom(a) || !ray_is_atom(b)) return ray_error("type", NULL);
    int64_t n, mx;
    if (a->type == -RAY_I64) n = a->i64;
    else if (a->type == -RAY_I32) n = a->i32;
    else return ray_error("type", NULL);
    if (b->type == -RAY_I64) mx = b->i64;
    else if (b->type == -RAY_I32) mx = b->i32;
    else return ray_error("type", NULL);
    if (n < 0) return ray_error("domain", NULL);
    if (mx <= 0) return ray_error("domain", NULL);
    if (n == 0) return ray_vec_new(RAY_I64, 0);
    ray_t* vec = ray_vec_new(RAY_I64, n);
    if (RAY_IS_ERR(vec)) return vec;
    int64_t* d = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < n; i++) d[i] = (int64_t)(rand() % mx);
    vec->len = n;
    return vec;
}

/* (bin sorted-vec val) → rightmost index where sorted[i] <= val, -1 if none */
ray_t* ray_bin_fn(ray_t* sorted, ray_t* val) {
    if (!ray_is_vec(sorted) || sorted->type != RAY_I64)
        return ray_error("type", NULL);
    int64_t* d = (int64_t*)ray_data(sorted);
    int64_t n = sorted->len;

    if (ray_is_atom(val) && (val->type == -RAY_I64 || val->type == -RAY_I32)) {
        int64_t v = val->i64;
        int64_t lo = 0, hi = n - 1, result = -1;
        while (lo <= hi) {
            int64_t mid = lo + (hi - lo) / 2;
            if (d[mid] <= v) { result = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        return ray_i64(result);
    }
    if (ray_is_vec(val) && val->type == RAY_I64) {
        int64_t* vals = (int64_t*)ray_data(val);
        int64_t vn = val->len;
        ray_t* rvec = ray_vec_new(RAY_I64, vn);
        if (RAY_IS_ERR(rvec)) return rvec;
        int64_t* out = (int64_t*)ray_data(rvec);
        for (int64_t i = 0; i < vn; i++) {
            int64_t v = vals[i];
            int64_t lo = 0, hi = n - 1, r = -1;
            while (lo <= hi) {
                int64_t mid = lo + (hi - lo) / 2;
                if (d[mid] <= v) { r = mid; lo = mid + 1; }
                else hi = mid - 1;
            }
            out[i] = r;
        }
        rvec->len = vn;
        return rvec;
    }
    return ray_error("type", NULL);
}

/* (binr sorted-vec val) → leftmost index where sorted[i] >= val */
ray_t* ray_binr_fn(ray_t* sorted, ray_t* val) {
    if (!ray_is_vec(sorted) || sorted->type != RAY_I64)
        return ray_error("type", NULL);
    int64_t* d = (int64_t*)ray_data(sorted);
    int64_t n = sorted->len;

    if (ray_is_atom(val) && (val->type == -RAY_I64 || val->type == -RAY_I32)) {
        int64_t v = val->i64;
        int64_t lo = 0, hi = n - 1, result = n;
        while (lo <= hi) {
            int64_t mid = lo + (hi - lo) / 2;
            if (d[mid] >= v) { result = mid; hi = mid - 1; }
            else lo = mid + 1;
        }
        return ray_i64(result >= n ? n - 1 : result);
    }
    if (ray_is_vec(val) && val->type == RAY_I64) {
        int64_t* vals = (int64_t*)ray_data(val);
        int64_t vn = val->len;
        ray_t* rvec = ray_vec_new(RAY_I64, vn);
        if (RAY_IS_ERR(rvec)) return rvec;
        int64_t* out = (int64_t*)ray_data(rvec);
        for (int64_t i = 0; i < vn; i++) {
            int64_t v = vals[i];
            int64_t lo = 0, hi = n - 1, r = n;
            while (lo <= hi) {
                int64_t mid = lo + (hi - lo) / 2;
                if (d[mid] >= v) { r = mid; hi = mid - 1; }
                else lo = mid + 1;
            }
            out[i] = r >= n ? n - 1 : r;
        }
        rvec->len = vn;
        return rvec;
    }
    return ray_error("type", NULL);
}

/* ══════════════════════════════════════════
 * Map variants
 * ══════════════════════════════════════════ */

/* (map-left fn fixed vec) → apply fn(fixed, elem) for each elem in vec */
/* Helper for map-left/map-right: iterate over vec calling fn with two args */
static ray_t* map_iterate(ray_t* fn, ray_t* fixed, ray_t* vec, int fixed_is_left) {
    /* If both are scalars, just call once */
    if (!ray_is_vec(vec) && vec->type != RAY_LIST) {
        if (fixed_is_left)
            return call_fn2(fn, fixed, vec);
        else
            return call_fn2(fn, vec, fixed);
    }

    int64_t vn = vec->len;
    ray_t* stack_results[4096];
    ray_t** results = stack_results;
    if (vn > 4096) {
        results = (ray_t**)ray_sys_alloc((size_t)vn * sizeof(ray_t*));
        if (!results) return ray_error("oom", NULL);
    }

    for (int64_t i = 0; i < vn; i++) {
        int alloc = 0;
        ray_t* elem = collection_elem(vec, i, &alloc);
        if (fixed_is_left)
            results[i] = call_fn2(fn, fixed, elem);
        else
            results[i] = call_fn2(fn, elem, fixed);
        if (alloc) ray_release(elem);
        if (RAY_IS_ERR(results[i])) {
            ray_t* err = results[i];
            for (int64_t j = 0; j < i; j++) ray_release(results[j]);
            if (results != stack_results) ray_sys_free(results);
            return err;
        }
    }
    ray_t* out = ray_enlist_fn(results, vn);
    for (int64_t i = 0; i < vn; i++) ray_release(results[i]);
    if (results != stack_results) ray_sys_free(results);
    return out;
}

/* (map-left fn fixed vec) → apply fn(fixed, elem) for each elem in vec.
 * If vec is scalar but fixed is a vector, auto-swap (iterate over fixed). */
ray_t* ray_map_left_fn(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("domain", NULL);
    ray_t* fn = args[0];
    ray_t* fixed = args[1];
    ray_t* vec = args[2];

    /* Auto-detect: if vec is scalar but fixed is a vector, swap roles */
    if (!ray_is_vec(vec) && vec->type != RAY_LIST &&
        (ray_is_vec(fixed) || fixed->type == RAY_LIST)) {
        return map_iterate(fn, vec, fixed, 0); /* fn(elem_of_fixed, vec) — but we want fn(fixed=scalar, elem) */
    }

    return map_iterate(fn, fixed, vec, 1); /* fn(fixed, elem) */
}

/* (map-right fn vec fixed) → apply fn(elem, fixed) for each elem in vec.
 * If vec is scalar but fixed is a vector, auto-swap (iterate over fixed). */
ray_t* ray_map_right_fn(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("domain", NULL);
    ray_t* fn = args[0];
    ray_t* vec = args[1];
    ray_t* fixed = args[2];

    /* Auto-detect: if vec is scalar but fixed is a vector, swap roles */
    if (!ray_is_vec(vec) && vec->type != RAY_LIST &&
        (ray_is_vec(fixed) || fixed->type == RAY_LIST)) {
        return map_iterate(fn, vec, fixed, 1); /* fn(vec_scalar, elem_of_fixed) */
    }

    return map_iterate(fn, fixed, vec, 0); /* fn(elem, fixed) */
}

/* ══════════════════════════════════════════
 * Fold/scan variants
 * ══════════════════════════════════════════ */

/* (fold-left fn init coll) — left fold with explicit initial value */
ray_t* ray_fold_left_fn(ray_t** args, int64_t n) {
    /* Same as (fold fn init coll) — fold already goes left-to-right */
    return ray_fold_fn(args, n);
}

/* (fold-right fn init coll) — right fold */
ray_t* ray_fold_right_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* _bx = NULL;
    ray_t* acc;
    ray_t* vec;

    if (n == 2) {
        /* (fold-right fn vec) — use last element as initial value */
        vec = unbox_vec_arg(args[1], &_bx);
        if (RAY_IS_ERR(vec)) return vec;
        if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
        int64_t len = ray_len(vec);
        if (len == 0) { if (_bx) ray_release(_bx); return ray_error("domain", NULL); }
        ray_t** elems = (ray_t**)ray_data(vec);
        ray_retain(elems[len - 1]);
        acc = elems[len - 1];
        for (int64_t i = len - 2; i >= 0; i--) {
            ray_t* next = call_fn2(fn, elems[i], acc);
            ray_release(acc);
            if (RAY_IS_ERR(next)) { if (_bx) ray_release(_bx); return next; }
            acc = next;
        }
        if (_bx) ray_release(_bx);
        return acc;
    }

    /* (fold-right fn init coll) */
    ray_retain(args[1]);
    acc = args[1];
    vec = unbox_vec_arg(args[2], &_bx);
    if (RAY_IS_ERR(vec)) { ray_release(acc); return vec; }
    if (!is_list(vec)) { ray_release(acc); if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = len - 1; i >= 0; i--) {
        ray_t* next = call_fn2(fn, elems[i], acc);
        ray_release(acc);
        if (RAY_IS_ERR(next)) { if (_bx) ray_release(_bx); return next; }
        acc = next;
    }
    if (_bx) ray_release(_bx);
    return acc;
}

/* (scan-left fn vec) — running left fold (same as scan) */
ray_t* ray_scan_left_fn(ray_t** args, int64_t n) {
    return ray_scan_fn(args, n);
}

/* (scan-right fn vec) — running right fold, returns vector of partial results */
ray_t* ray_scan_right_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* _bx = NULL;
    ray_t* vec = unbox_vec_arg(args[1], &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", NULL); }
    int64_t len = ray_len(vec);
    if (len == 0) {
        if (_bx) ray_release(_bx);
        ray_t* result = ray_alloc(0);
        if (!result) return ray_error("oom", NULL);
        result->type = RAY_LIST;
        result->len = 0;
        return result;
    }

    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** elems = (ray_t**)ray_data(vec);

    ray_retain(elems[len - 1]);
    out[len - 1] = elems[len - 1];
    for (int64_t i = len - 2; i >= 0; i--) {
        out[i] = call_fn2(fn, elems[i], out[i + 1]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = i + 1; j < len; j++) ray_release(out[j]);
            result->len = 0; ray_release(result); if (_bx) ray_release(_bx);
            return out[i];
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}
