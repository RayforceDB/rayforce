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
#include "ops/idxop.h"
#include "ops/ops.h"
#include <stdatomic.h>
#include <math.h>
#include <stdint.h>
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
            /* Hashes the raw cell id, i.e. the BUILD side's id space.
             * Cross-domain probes never reach this hash — they are
             * translated into the build side's domain up front in
             * hashset_find_xrow (sym-domain Phase 2). */
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
                /* Raw index equality is only meaningful within ONE id
                 * space — gate on domain identity (sym-domain Phase 2);
                 * cross-domain rows fall through to the boxed compare
                 * below (collection_elem re-expresses through each
                 * cell's domain).  Pre-flip both sides are the runtime
                 * singleton: byte-identical fast path. */
                if (ray_sym_vec_domain(a_src) != ray_sym_vec_domain(b_src))
                    break;
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
    /* sym-domain Phase 2: SYM-vec vs SYM-vec probes across DIFFERENT
     * domains cannot compare (or hash) raw ids.  Re-express the probe
     * cell in the build side's domain once, then run the standard raw
     * probe.  Absent from the build domain ⇒ no build row can hold
     * that symbol ⇒ miss.  Same-domain probes (the only pre-flip case)
     * skip this entirely: byte-identical fast path. */
    if (probe_type == RAY_SYM && hs->src_type == RAY_SYM &&
        probe_src->type == RAY_SYM && hs->src && hs->src->type == RAY_SYM) {
        struct ray_sym_domain_s* bdom = ray_sym_vec_domain(hs->src);
        struct ray_sym_domain_s* pdom = ray_sym_vec_domain(probe_src);
        if (bdom != pdom) {
            int64_t pid = ray_read_sym(probe_data, probe_i, probe_type,
                                       probe_src->attrs);
            ray_t* s = ray_sym_domain_str(pdom, pid);
            int64_t xid = s ? ray_sym_domain_find(bdom, ray_str_ptr(s),
                                                  ray_str_len(s)) : -1;
            if (xid < 0) return HS_EMPTY;
            uint64_t hx = ray_hash_i64(xid);
            int64_t sx = (int64_t)(hx & (uint64_t)hs->mask);
            while (hs->slots[sx] != HS_EMPTY) {
                int64_t stored = hs->slots[sx];
                if (ray_read_sym(hs->src_data, stored, hs->src_type,
                                 hs->src->attrs) == xid)
                    return stored;
                sx = (sx + 1) & hs->mask;
            }
            return HS_EMPTY;
        }
    }
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
    if (n < 2) return ray_error("domain", "map: requires at least 2 args (fn and vec), got %lld", (long long)n);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* _bx = NULL;

    if (n == 2) {
        /* Unary map: (map fn vec) */
        ray_t* vec = unbox_vec_arg(args[1], &_bx);
        if (RAY_IS_ERR(vec)) return vec;
        if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", "map: vec arg must be a list, got %s", ray_type_name(vec->type)); }
        int64_t len = ray_len(vec);
        ray_t* result = ray_alloc(len * sizeof(ray_t*));
        if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
        result->type = RAY_LIST;
        result->len = len;
        ray_t** out = (ray_t**)ray_data(result);
        ray_t** elems = (ray_t**)ray_data(vec);
        for (int64_t i = 0; i < len; i++) {
            out[i] = call_fn1(fn, elems[i]);
            /* Materialise a lazy result (e.g. the lambda returned
             * (count (distinct x)) — distinct/count of a vec is lazy).  A LIST
             * of unforced lazy nodes breaks every downstream consumer that
             * doesn't force per element (sum, ==, +). */
            if (ray_is_lazy(out[i])) out[i] = ray_lazy_materialize(out[i]);
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
        if (ray_is_lazy(out[i])) out[i] = ray_lazy_materialize(out[i]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            result->len = 0; ray_release(result); if (_bx) ray_release(_bx);
            return out[i];
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* (pmap fn val vec) — sequential alias for map, kept as a distinct builtin so
 * call sites can signal intent.  Evaluation is NOT parallelised: the applied
 * function runs on the calling thread's VM (the VM is thread-local and eval is
 * single-threaded), so results and semantics are identical to map. */
ray_t* ray_pmap_fn(ray_t** args, int64_t n) {
    return ray_map_fn(args, n);
}

/* (fold fn vec) or (fold fn init vec) — reduce with binary fn */
ray_t* ray_fold_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", "fold: requires at least 2 args (fn and vec), got %lld", (long long)n);
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
        if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", "fold: vec arg must be a list, got %s", ray_type_name(vec->type)); }
        int64_t len = ray_len(vec);
        if (len == 0) { if (_bx) ray_release(_bx); return ray_error("domain", "fold: empty list has no initial value, got %lld", (long long)len); }
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
    if (!is_list(vec)) { ray_release(acc); if (_bx) ray_release(_bx); return ray_error("type", "fold: vec arg must be a list, got %s", ray_type_name(vec->type)); }
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

/* Typed prefix-scan kernels for the native arithmetic operators.  The
 * generic scan below unboxes the vector into a LIST and drives the full
 * call machinery per element — and, worse, RETURNS a LIST of boxed
 * atoms, so every downstream vector op on the running sums degrades to
 * the boxed paths too (the q28 20-row moving average spent ~13ms on
 * 45k rows this way).  For (scan + vec) / (scan * vec) over numeric
 * typed vectors, run one tight per-width loop and return a TYPED
 * vector.  Per-step semantics mirror ray_add_fn / ray_mul_fn exactly:
 * floats — plain IEEE ops, NaN (the F64 null) propagates and sticks;
 * ints — same-width wrap-truncate arithmetic (make_typed_int), null
 * sentinel propagation is sticky, and a wrap that lands on the
 * sentinel reads as null downstream exactly like the boxed path. */
static ray_t* scan_typed_arith(ray_t* fn, ray_t* vec) {
    if (fn->type != RAY_BINARY || !vec || !ray_is_vec(vec) || vec->len <= 0)
        return NULL;
    ray_binary_fn bf = (ray_binary_fn)(uintptr_t)fn->i64;
    bool is_add = (bf == ray_add_fn);
    if (!is_add && bf != ray_mul_fn) return NULL;
    int8_t t = vec->type;
    if (t != RAY_F64 && t != RAY_I64 && t != RAY_I32 && t != RAY_I16)
        return NULL;
    int64_t len = vec->len;
    ray_t* out = ray_vec_new(t, len);
    if (!out || RAY_IS_ERR(out)) { if (out) ray_release(out); return NULL; }
    out->len = len;
    /* Sentinel-bearing partials downstream: mirror the DAG convention —
     * advertise nulls when the source does. */
    out->attrs |= (vec->attrs & RAY_ATTR_HAS_NULLS);
    if (t == RAY_F64) {
        const double* restrict v = (const double*)ray_data(vec);
        double* restrict o = (double*)ray_data(out);
        double acc = v[0];
        o[0] = acc;
        if (is_add) for (int64_t i = 1; i < len; i++) { acc += v[i]; o[i] = acc; }
        else        for (int64_t i = 1; i < len; i++) { acc *= v[i]; o[i] = acc; }
        return out;
    }
    #define SCAN_INT_LOOP(CT, SENT)                                           \
        do {                                                                  \
            const CT* restrict v = (const CT*)ray_data(vec);                  \
            CT* restrict o = (CT*)ray_data(out);                              \
            CT acc = v[0];                                                    \
            o[0] = acc;                                                       \
            for (int64_t i = 1; i < len; i++) {                               \
                CT x = v[i];                                                  \
                if (acc == (SENT) || x == (SENT)) acc = (SENT);               \
                else acc = is_add                                             \
                    ? (CT)((int64_t)acc + (int64_t)x)                         \
                    : (CT)((int64_t)acc * (int64_t)x);                        \
                o[i] = acc;                                                   \
            }                                                                 \
        } while (0)
    if (t == RAY_I64)      SCAN_INT_LOOP(int64_t, NULL_I64);
    else if (t == RAY_I32) SCAN_INT_LOOP(int32_t, NULL_I32);
    else                   SCAN_INT_LOOP(int16_t, NULL_I16);
    #undef SCAN_INT_LOOP
    return out;
}

/* (prior fn vec) — pairwise combinator over adjacent elements:
 * out[i] = (fn v[i] v[i-1]) for i >= 1, out[0] = (fn v[0] v[0]) — the
 * self-pair start matches the concat/take shift idiom it replaces
 * (prev[0] = v[0]), so a comparison's first slot compares equal → false.
 * The six comparison operators over null-free
 * numeric/temporal typed vectors (plus ==//= over SYM, where adjacent
 * same-vector ids compare directly) run one fused pass returning a BOOL
 * vector — no shifted copy, no boxing.  Everything else falls to the
 * generic per-pair application returning a LIST, mirroring scan. */
static ray_t* prior_typed_cmp(ray_t* fn, ray_t* vec) {
    if (fn->type != RAY_BINARY || !vec || !ray_is_vec(vec) || vec->len <= 0)
        return NULL;
    if (vec->attrs & RAY_ATTR_HAS_NULLS) return NULL;
    ray_binary_fn bf = (ray_binary_fn)(uintptr_t)fn->i64;
    int op;                                   /* 0:< 1:> 2:<= 3:>= 4:== 5:!= */
    if      (bf == ray_lt_fn)  op = 0;
    else if (bf == ray_gt_fn)  op = 1;
    else if (bf == ray_lte_fn) op = 2;
    else if (bf == ray_gte_fn) op = 3;
    else if (bf == ray_eq_fn)  op = 4;
    else if (bf == ray_neq_fn) op = 5;
    else return NULL;
    int8_t t = vec->type;
    bool is_sym = (t == RAY_SYM);
    if (is_sym && op != 4 && op != 5) return NULL;  /* SYM ordering → generic */
    if (!is_sym) switch (t) {
        case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
        case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
        case RAY_F64: case RAY_F32: break;
        default: return NULL;
    }
    int64_t n = vec->len;
    ray_t* out = ray_vec_new(RAY_BOOL, n);
    if (!out || RAY_IS_ERR(out)) { if (out) ray_release(out); return NULL; }
    out->len = n;
    uint8_t* restrict o = (uint8_t*)ray_data(out);
    #define PRIOR_CMP_LOOP(READ)                                          \
        do {                                                              \
            int64_t prev = READ(0);                                       \
            o[0] = (uint8_t)((op == 2) | (op == 3) | (op == 4));          \
            for (int64_t i = 1; i < n; i++) {                             \
                int64_t cur = READ(i);                                    \
                uint8_t r;                                                \
                switch (op) {                                             \
                    case 0: r = cur <  prev; break;                       \
                    case 1: r = cur >  prev; break;                       \
                    case 2: r = cur <= prev; break;                       \
                    case 3: r = cur >= prev; break;                       \
                    case 4: r = cur == prev; break;                       \
                    default: r = cur != prev; break;                      \
                }                                                         \
                o[i] = r;                                                 \
                prev = cur;                                               \
            }                                                             \
        } while (0)
    if (is_sym) {
        const void* d = ray_data(vec);
        uint8_t at = vec->attrs;
        #define RD_SYM(i) ((int64_t)ray_read_sym(d, (i), RAY_SYM, at))
        PRIOR_CMP_LOOP(RD_SYM);
        #undef RD_SYM
    } else if (t == RAY_F64 || t == RAY_F32) {
        /* Float compares must stay in the float domain (NaN, fractions). */
        const void* d = ray_data(vec);
        double prev = (t == RAY_F64) ? ((const double*)d)[0]
                                     : (double)((const float*)d)[0];
        o[0] = (uint8_t)((op == 2) | (op == 3) | (op == 4));
        /* NaN self-compare: <=/>=/== on a NaN first element is false —
         * overwrite the constant start for that case. */
        if (prev != prev) o[0] = (uint8_t)(op == 5);
        for (int64_t i = 1; i < n; i++) {
            double cur = (t == RAY_F64) ? ((const double*)d)[i]
                                        : (double)((const float*)d)[i];
            uint8_t r;
            switch (op) {
                case 0: r = cur <  prev; break;
                case 1: r = cur >  prev; break;
                case 2: r = cur <= prev; break;
                case 3: r = cur >= prev; break;
                case 4: r = cur == prev; break;
                default: r = cur != prev; break;
            }
            o[i] = r;
            prev = cur;
        }
    } else {
        const void* d = ray_data(vec);
        uint8_t at = vec->attrs;
        #define RD_I(i) read_col_i64(d, (i), t, at)
        PRIOR_CMP_LOOP(RD_I);
        #undef RD_I
    }
    #undef PRIOR_CMP_LOOP
    return out;
}

ray_t* ray_prior_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", "prior: requires fn and vec args, got %lld", (long long)n);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);
    ray_t* fn = args[0];
    {
        ray_t* fast = prior_typed_cmp(fn, args[1]);
        if (fast) return fast;
    }
    ray_t* _bx = NULL;
    ray_t* vec = unbox_vec_arg(args[1], &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", "prior: vec arg must be a list, got %s", ray_type_name(vec->type)); }
    int64_t len = ray_len(vec);
    ray_t* result = ray_alloc(len > 0 ? len * (int64_t)sizeof(ray_t*) : 0);
    if (!result) { if (_bx) ray_release(_bx); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    if (len == 0) { if (_bx) ray_release(_bx); return result; }
    ray_t** out = (ray_t**)ray_data(result);
    ray_t** elems = (ray_t**)ray_data(vec);
    for (int64_t i = 0; i < len; i++) {
        out[i] = call_fn2(fn, elems[i], elems[i > 0 ? i - 1 : 0]);
        if (RAY_IS_ERR(out[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            result->len = 0; ray_release(result); if (_bx) ray_release(_bx);
            return out[i];
        }
    }
    if (_bx) ray_release(_bx);
    return result;
}

/* (scan fn vec) — running fold, returns vector of partial results */
ray_t* ray_scan_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", "scan: requires at least 2 args (fn and vec), got %lld", (long long)n);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    {
        ray_t* fast = scan_typed_arith(fn, args[1]);
        if (fast) return fast;
    }
    ray_t* _bx = NULL;
    ray_t* vec = unbox_vec_arg(args[1], &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", "scan: vec arg must be a list, got %s", ray_type_name(vec->type)); }
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
        if (nrows != mask->len) return ray_error("length", "filter: mask length must equal table row count %lld, got %lld", (long long)nrows, (long long)mask->len);
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
        if ((int64_t)slen != mlen) return ray_error("length", "filter: mask length must equal string length %lld, got %lld", (long long)slen, (long long)mlen);
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
        if (len != mlen) return ray_error("length", "filter: mask length must equal vec length %lld, got %lld", (long long)len, (long long)mlen);
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
    if (!is_list(vec) || !is_list(mask)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", "filter: vec and mask must be lists, got %s and %s", ray_type_name(vec->type), ray_type_name(mask->type)); }
    int64_t len = ray_len(vec);
    int64_t mlen = ray_len(mask);
    if (len != mlen) return ray_error("length", "filter: mask length must equal vec length %lld, got %lld", (long long)len, (long long)mlen);

    ray_t** velems = (ray_t**)ray_data(vec);
    ray_t** melems = (ray_t**)ray_data(mask);

    /* Validate mask is all booleans */
    for (int64_t i = 0; i < len; i++) {
        if (melems[i]->type != -RAY_BOOL) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", "filter: mask elements must be bool, got %s", ray_type_name(melems[i]->type)); }
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
    if (n < 3) return ray_error("domain", "apply: requires at least 3 args (fn and two vecs), got %lld", (long long)n);
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
    if (!is_list(vec1) || !is_list(vec2)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", "apply: both vec args must be lists, got %s and %s", ray_type_name(vec1->type), ray_type_name(vec2->type)); }
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
    case RAY_LIST: {
        /* Structural compare: lists are equal iff same length AND every
         * pair of elements is atom_eq.  Without this, two structurally-
         * identical lists with different element pointers compared via
         * the default branch's memcmp on ray_t** — i.e. pointer
         * identity, never structurally equal — which broke (group LIST)
         * (every row its own bucket) and dict/distinct fallbacks. */
        if (a->len != b->len) return 0;
        ray_t** ea = (ray_t**)ray_data(a);
        ray_t** eb = (ray_t**)ray_data(b);
        for (int64_t i = 0; i < a->len; i++) {
            if (!ea[i] || !eb[i]) {
                if (ea[i] != eb[i]) return 0;
                continue;
            }
            if (!atom_eq(ea[i], eb[i])) return 0;
        }
        return 1;
    }
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

    /* Fast path: when a fresh, null-free sort index is attached, walk the
     * permutation once to collect the first-in-run row ids.  The walk
     * produces ids in value-ascending order (matching the hashset path's
     * distinct_sort_indices contract) in O(n) with no hash table.
     * Guard on ray_index_has to avoid a consult bump when no index exists
     * (mirrors the find fast-path's !has_nulls && ray_index_has gate). */
    if (x->type != RAY_SYM && x->type != RAY_GUID && x->type != RAY_STR
            && ray_index_has(x)) {
        ray_idx_consults[IDX_SITE_DISTINCT]++;
        int64_t count = 0;
        ray_t* ids = ray_index_distinct_ids(x, &count);
        if (ids) {
            ray_idx_hits[IDX_SITE_DISTINCT]++;
            ray_t* result = gather_by_idx(x, (int64_t*)ray_data(ids), count);
            ray_release(ids);
            return result;
        }
    }

    int64_t idx_stack[256];
    int64_t* idx = (len <= 256) ? idx_stack : (int64_t*)ray_sys_alloc((size_t)len * sizeof(int64_t));
    if (!idx) return ray_error("oom", NULL);

    /* SYM presence-bitmap dedup: a SYM code is a position in [0, domain_count),
     * so for a small domain a domain_count-byte "seen" array collects the
     * first-occurrence row ids in O(n) with NO hashing — identical first-
     * appearance order to the hashset path below.  Gate dc<=len so the array
     * never costs more than the data it replaces; larger domains fall through. */
    if (x->type == RAY_SYM) {
        int64_t dc = ray_sym_domain_count(ray_sym_vec_domain(x));
        if (dc > 0 && dc <= 65536 && dc <= len) {
            uint8_t* seen = (uint8_t*)ray_sys_alloc((size_t)dc);
            if (!seen) { if (idx != idx_stack) ray_sys_free(idx); return ray_error("oom", NULL); }
            memset(seen, 0, (size_t)dc);
            const void* data = ray_data(x);
            int8_t ty = x->type; uint16_t at = x->attrs;
            int64_t count = 0;
            for (int64_t i = 0; i < len; i++) {
                int64_t code = ray_read_sym(data, i, ty, at);
                if (!seen[code]) { seen[code] = 1; idx[count++] = i; }
            }
            ray_sys_free(seen);
            ray_t* result = gather_by_idx(x, idx, count);
            if (idx != idx_stack) ray_sys_free(idx);
            return result;
        }
    }

    /* Dict-encoded STR: dedup on the int32 dict-code vector (codes in
     * [0, n_distinct)) via a presence array — first-occurrence order, no
     * string hashing.  Mirrors the SYM presence path; gated nd<=len so the
     * seen[] array never costs more than the data it replaces. */
    if (x->type == RAY_STR && ray_index_kind(x) == RAY_IDX_DICT) {
        ray_index_t* dix = ray_index_payload(x->index);
        int64_t nd = dix->u.dict.n_distinct;
        ray_t* codes_v = dix->u.dict.codes;
        if (nd > 0 && nd <= len && codes_v && !RAY_IS_ERR(codes_v)
            && codes_v->len == len) {
            const int32_t* codes = (const int32_t*)ray_data(codes_v);
            uint8_t* seen = (uint8_t*)ray_sys_alloc((size_t)nd);
            if (!seen) { if (idx != idx_stack) ray_sys_free(idx); return ray_error("oom", NULL); }
            memset(seen, 0, (size_t)nd);
            int64_t count = 0;
            for (int64_t i = 0; i < len; i++) {
                int32_t c = codes[i];
                if (c >= 0 && c < nd && !seen[c]) { seen[c] = 1; idx[count++] = i; }
            }
            ray_sys_free(seen);
            ray_t* result = gather_by_idx(x, idx, count);
            if (idx != idx_stack) ray_sys_free(idx);
            return result;
        }
    }

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

static ray_t* parted_to_flat_vec(ray_t* x) {
    if (!x || !RAY_IS_PARTED(x->type)) return ray_error("type", NULL);
    int8_t base = (int8_t)RAY_PARTED_BASETYPE(x->type);
    ray_t** segs = (ray_t**)ray_data(x);
    int64_t total = 0;
    for (int64_t s = 0; s < x->len; s++)
        if (segs[s]) total += segs[s]->len;

    ray_t* out = ray_vec_new(base, base == RAY_STR ? 0 : total);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    if (base != RAY_STR) out->len = total;

    int64_t pos = 0;
    for (int64_t s = 0; s < x->len; s++) {
        ray_t* seg = segs[s];
        if (!seg) continue;
        for (int64_t i = 0; i < seg->len; i++) {
            if (base == RAY_STR) {
                size_t slen = 0;
                const char* sp = ray_str_vec_get(seg, i, &slen);
                out = ray_str_vec_append(out, sp ? sp : "", sp ? slen : 0);
                if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
                if (ray_vec_is_null(seg, i)) ray_vec_set_null(out, pos, true);
            } else {
                int allocated = 0;
                ray_t* elem = collection_elem(seg, i, &allocated);
                if (!elem || RAY_IS_ERR(elem) || store_typed_elem(out, pos, elem) != 0) {
                    if (allocated && elem && !RAY_IS_ERR(elem)) ray_release(elem);
                    ray_release(out);
                    return elem && RAY_IS_ERR(elem) ? elem : ray_error("type", NULL);
                }
                if (allocated) ray_release(elem);
            }
            pos++;
        }
    }
    return out;
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

    if (RAY_IS_PARTED(x->type)) {
        ray_t* flat = parted_to_flat_vec(x);
        if (!flat || RAY_IS_ERR(flat)) return flat ? flat : ray_error("oom", NULL);
        ray_t* out = distinct_vec_eager(flat);
        ray_release(flat);
        return out;
    }

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
    if (!is_list(x)) { if (_bx) ray_release(_bx); return ray_error("type", "distinct: argument must be a list, got %s", ray_type_name(x->type)); }
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
            /* Typed kernel first: verdict-LUT for SYM, SIMD small-set for
             * ints, pool-parallel — the same engine the fused WHERE path
             * uses.  Gated off null-bearing operands: this hashset path
             * matches null ∈ {…null…} as TRUE (null equals null), while
             * the WHERE kernel uses null-matches-nothing semantics — the
             * two must not be conflated.  NULL result = unsupported shape
             * (STR etc.): fall through to the hashset probe below. */
            if (!(val->attrs & RAY_ATTR_HAS_NULLS) &&
                !(vec->attrs & RAY_ATTR_HAS_NULLS)) {
                ray_t* fast = ray_in_vec_exec(val, vec, false);
                if (fast) return fast;
            }
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
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", "in: vec arg must be a list, got %s", ray_type_name(vec->type)); }
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
    if (!is_list(vec1)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", "except: first arg must be a list, got %s", ray_type_name(vec1->type)); }
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
    if (!is_list(vec1) || !is_list(vec2)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", "union: both args must be lists, got %s and %s", ray_type_name(vec1->type), ray_type_name(vec2->type)); }
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
    if (!is_list(vec1) || !is_list(vec2)) { if (_bx1) ray_release(_bx1); if (_bx2) ray_release(_bx2); return ray_error("type", "sect: both args must be lists, got %s and %s", ray_type_name(vec1->type), ray_type_name(vec2->type)); }
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
        return ray_error("type", "take: count must be an integer, got %s", ray_type_name(n_obj->type));
    /* Range take: (take collection [start amount]) — slice from start for amount elements */
    if (ray_is_vec(n_obj) && n_obj->type == RAY_I64 && ray_len(n_obj) == 2) {
        int64_t* idx = (int64_t*)ray_data(n_obj);
        int64_t start = idx[0];
        int64_t amount = idx[1];
        if (amount < 0) return ray_error("length", "take: range amount must be non-negative, got %lld", (long long)amount);

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
                ray_release(taken);
                if (RAY_IS_ERR(result)) return result;
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
            int esz = ray_sym_elem_size(vtype, vec->attrs);
            ray_t* result = (vtype == RAY_SYM)
                          ? ray_sym_vec_new(vec->attrs & RAY_SYM_W_MASK, count)
                          : ray_vec_new(vtype, count);
            if (RAY_IS_ERR(result)) return result;
            result->len = count;
            memcpy(ray_data(result), (char*)ray_data(vec) + start * esz, (size_t)(count * esz));
            /* SYM ids copied verbatim — resolve over the source domain. */
            if (vtype == RAY_SYM) ray_sym_vec_adopt_domain(result, vec);
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

        return ray_error("type", "take: range take unsupported for %s", ray_type_name(vec->type));
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
            ray_release(taken);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    /* Dict take: apply take to keys and vals in parallel.  Wrapping for
     * |n| > pair count works the same as for typed vectors. */
    if (vec->type == RAY_DICT && is_numeric(n_obj)) {
        ray_t* keys = ray_dict_keys(vec);
        ray_t* vals = ray_dict_vals(vec);
        if (!keys) return ray_error("type", "take: dict has no keys vector");
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
        int esz = ray_sym_elem_size(vtype, vec->attrs);
        ray_t* result = (vtype == RAY_SYM)
                      ? ray_sym_vec_new(vec->attrs & RAY_SYM_W_MASK, abs_n)
                      : ray_vec_new(vtype, abs_n);
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
        /* SYM ids copied verbatim — resolve over the source domain. */
        if (vtype == RAY_SYM) ray_sym_vec_adopt_domain(result, vec);
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
        { if (_bx) ray_release(_bx); return ray_error("type", "take: requires a list and a numeric count, got %s and %s", ray_type_name(vec->type), ray_type_name(n_obj->type)); }
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
    /* Table column access by symbol key — return the typed vector directly.
     * A column loaded from a splayed/parted table (.db.parted.get) is still in
     * segmented (RAY_IS_PARTED) form; the query path flattens it lazily, but a
     * direct `(at table 'col)` must materialize it so downstream `(at col i)` /
     * count / formatting see a dense vector instead of failing with `type`. */
    if (vec->type == RAY_TABLE && idx->type == -RAY_SYM) {
        ray_t* col = ray_table_get_col(vec, idx->i64);
        if (!col) return ray_error("domain", "at: unknown table column");
        if (RAY_IS_PARTED(col->type)) return parted_to_flat_vec(col);
        if (col->type == RAY_MAPCOMMON) return materialize_mapcommon(col);
        ray_retain(col);
        return col;
    }

    /* Table row access by integer index: (at table 0) → {col1: val1, col2: val2} */
    if (vec->type == RAY_TABLE && ray_is_atom(idx) &&
        (idx->type == -RAY_I64 || idx->type == -RAY_I32 ||
         idx->type == -RAY_I16 || idx->type == -RAY_U8)) {
        int64_t row = as_i64(idx);
        int64_t nrows = ray_table_nrows(vec);
        if (row < 0 || row >= nrows) return ray_error("domain", "at: row index %lld out of range for table of %lld rows", (long long)row, (long long)nrows);
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

    /* Table row selection by index vector: apply the row ids to each
     * column and return a table.  Keep this before the generic collection
     * fallback; otherwise a table indexed by millions of row ids becomes
     * a LIST of row dictionaries. */
    if (vec->type == RAY_TABLE && idx->type == RAY_I64) {
        int64_t nrows = ray_table_nrows(vec);
        int64_t nidx = ray_len(idx);
        int64_t* ids = (int64_t*)ray_data(idx);
        for (int64_t i = 0; i < nidx; i++) {
            if (ids[i] < 0 || ids[i] >= nrows)
                return ray_error("domain", "at: row index %lld out of range for table of %lld rows", (long long)ids[i], (long long)nrows);
        }

        int64_t ncols = ray_table_ncols(vec);
        ray_t* result = ray_table_new(ncols);
        if (!result || RAY_IS_ERR(result)) return result ? result : ray_error("oom", NULL);
        for (int64_t c = 0; c < ncols; c++) {
            ray_t* col = ray_table_get_col_idx(vec, c);
            int64_t name = ray_table_col_name(vec, c);
            if (!col) continue;
            ray_t* gathered = gather_by_idx(col, ids, nidx);
            if (!gathered || RAY_IS_ERR(gathered)) {
                ray_release(result);
                return gathered ? gathered : ray_error("oom", NULL);
            }
            result = ray_table_add_col(result, name, gathered);
            ray_release(gathered);
            if (!result || RAY_IS_ERR(result))
                return result ? result : ray_error("oom", NULL);
        }
        return result;
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
                if (k < 0 || (size_t)k >= slen) return ray_error("domain", "at: string index %lld out of range for length %lld", (long long)k, (long long)slen);
                buf[j] = s[k];
            }
            return ray_str(buf, (size_t)idxlen);
        }
        int64_t i = as_i64(idx);
        if (i < 0 || (size_t)i >= slen) return ray_error("domain", "at: string index %lld out of range for length %lld", (long long)i, (long long)slen);
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
                /* Only out[0..j) are initialised; shrink len so ray_release
                 * frees exactly those and never reads the garbage tail
                 * (out[j..idxlen) is uninitialised). */
                result->len = j;
                ray_release(result);
                return idx_elem;
            }
            ray_t* sub_idx = idx_elem;
            ray_t* val = ray_at_fn(vec, sub_idx);
            if (alloc) ray_release(idx_elem);
            if (RAY_IS_ERR(val)) {
                result->len = j;
                ray_release(result);
                return val;
            }
            out[j] = val;
        }
        return result;
    }

    if (idx->type != -RAY_I64 && idx->type != -RAY_I32 &&
        idx->type != -RAY_I16 && idx->type != -RAY_U8)
        return ray_error("type", "at: index must be an integer, got %s", ray_type_name(idx->type));
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

    if (!is_list(vec)) return ray_error("type", "at: cannot index %s", ray_type_name(vec->type));
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
    /* Vector val: (find vec [v1 v2]) → dense I64 [idx1 idx2], O(n+m) via hashset. */
    if (is_collection(val) && !ray_is_atom(val)) {
        int64_t vlen = ray_len(val);
        ray_t* result = ray_vec_new(RAY_I64, vlen);
        if (RAY_IS_ERR(result)) return result;
        result->len = vlen;
        int64_t* out = (int64_t*)ray_data(result);
        bool any_null = false;

        /* Hash fast path: both sides typed vecs — O(n+m), mirrors ray_in_fn. */
        if (ray_is_vec(val) && ray_is_vec(vec)) {
            hashset_t hs;
            if (!hashset_init(&hs, vec, vec->len)) { ray_release(result); return ray_error("oom", NULL); }
            for (int64_t j = 0; j < vec->len; j++) hashset_insert(&hs, j);
            int8_t vt = val->type; void* vd = ray_data(val);
            for (int64_t i = 0; i < vlen; i++) {
                int64_t row = hashset_find_xrow(&hs, val, i, vt, vd);
                if (row == HS_EMPTY) { out[i] = NULL_I64; any_null = true; }
                else out[i] = row;
            }
            hashset_destroy(&hs);
        } else {
            /* Fallback (LIST/mixed): per-element scalar find into the dense buffer. */
            for (int64_t j = 0; j < vlen; j++) {
                int alloc = 0;
                ray_t* ve = collection_elem(val, j, &alloc);
                ray_t* one = ray_find_fn(vec, ve);   /* scalar → i64 atom or 0Nl */
                if (alloc) ray_release(ve);
                if (RAY_IS_ERR(one)) { ray_release(result); return one; }
                if (RAY_ATOM_IS_NULL(one)) { out[j] = NULL_I64; any_null = true; }
                else out[j] = one->i64;
                ray_release(one);
            }
        }
        if (any_null) result->attrs |= RAY_ATTR_HAS_NULLS;
        return result;
    }
    /* Typed vector: search without boxing */
    if (ray_is_vec(vec)) {
        int64_t len = vec->len;
        bool has_nulls = (vec->attrs & RAY_ATTR_HAS_NULLS) != 0;
        bool val_null = RAY_ATOM_IS_NULL(val);

        /* Hash-index fast path: integer-family needle against an indexed
         * integer-family column without nulls.  Float and cross-family
         * needles fall through to the scan (the scan owns promotion
         * semantics; we do not replicate them here). */
        if (!has_nulls && !val_null && ray_is_atom(val) && ray_index_has(vec)) {
            int64_t needle = 0;
            int eligible = 1;
            switch (val->type) {
            case -RAY_I64:
            case -RAY_TIMESTAMP: needle = val->i64;              break;
            case -RAY_I32:
            case -RAY_DATE:
            case -RAY_TIME:      needle = (int64_t)val->i32;     break;
            case -RAY_I16:       needle = (int64_t)val->i16;     break;
            case -RAY_BOOL:
            case -RAY_U8:        needle = (int64_t)val->b8;      break;
            default:             eligible = 0;                   break;
            }
            if (eligible) {
                ray_idx_consults[IDX_SITE_FIND]++;
                int64_t row = ray_index_find_row(vec, needle);
                if (row >= -1) {   /* -2 means not eligible — fall through */
                    ray_idx_hits[IDX_SITE_FIND]++;
                    if (row < 0)
                        return ray_typed_null(-RAY_I64);
                    return make_i64(row);
                }
            }
        }

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
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", "find: vec arg must be a list, got %s", ray_type_name(vec->type)); }
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
    if (!ray_is_atom(x) || x->type != -RAY_I64) return ray_error("type", "til: argument must be an i64 atom, got %s", ray_type_name(x->type));
    int64_t n = x->i64;
    if (n < 0) return ray_error("domain", "til: count must be non-negative, got %lld", (long long)n);
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
    /* SYM ids copied verbatim — resolve over the source domain. */
    if (vtype == RAY_SYM) ray_sym_vec_adopt_domain(result, x);
    return result;
}

typedef struct {
    ray_t*  src;
    ray_t*  dst;
    ray_pool_t* pool;
    int64_t src_off;
    int64_t dst_off;
    int     esz;
} ts_shift_ctx_t;

static inline bool ts_morsel_checkpoint(ray_pool_t* pool, int64_t local_idx) {
    return (local_idx % RAY_MORSEL_ELEMS) == 0 && pool_cancelled(pool);
}

static void ts_shift_copy_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_shift_ctx_t* c = (ts_shift_ctx_t*)vctx;
    const char* src = (const char*)ray_data(c->src) + (c->src_off + start) * c->esz;
    char* dst = (char*)ray_data(c->dst) + (c->dst_off + start) * c->esz;
    size_t esz = (size_t)c->esz;
    for (int64_t i = start; i < end; i++, src += esz, dst += esz) {
        if (ts_morsel_checkpoint(c->pool, i - start)) return;
        memcpy(dst, src, esz);
    }
}

static ray_t* ts_alloc_like(ray_t* x) {
    if (x->type == RAY_STR)
        return ray_error("type", "time-series shift: string vectors are not supported yet");
    ray_t* out = (x->type == RAY_SYM)
               ? ray_sym_vec_new(x->attrs & RAY_SYM_W_MASK, x->len)
               : ray_vec_new(x->type, x->len);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = x->len;
    if (x->type == RAY_SYM) ray_sym_vec_adopt_domain(out, x);
    return out;
}

static void ts_set_boundary(ray_t* out, int64_t idx) {
    if (!out || idx < 0 || idx >= out->len) return;
    switch (out->type) {
        case RAY_BOOL:
        case RAY_U8:
            ((uint8_t*)ray_data(out))[idx] = 0;
            return;
        case RAY_SYM:
            ray_write_sym(ray_data(out), idx, 0, out->type, out->attrs);
            return;
        default:
            ray_vec_set_null(out, idx, true);
            return;
    }
}

static ray_t* shift_vec_eager(ray_t* x, bool lead) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "time-series shift expects a vector input");
    if (RAY_IS_PARTED(x->type)) {
        ray_t* flat = parted_to_flat_vec(x);
        if (!flat || RAY_IS_ERR(flat)) return flat ? flat : ray_error("oom", NULL);
        ray_t* out = shift_vec_eager(flat, lead);
        ray_release(flat);
        return out;
    }

    int64_t n = x->len;
    ray_t* out = ts_alloc_like(x);
    if (!out || RAY_IS_ERR(out)) return out;
    if (n == 0) return out;
    if (n == 1) {
        ts_set_boundary(out, 0);
        return out;
    }

    int esz = (x->type == RAY_SYM)
            ? ray_sym_elem_size(x->type, x->attrs)
            : ray_elem_size(x->type);
    ray_pool_t* pool = ray_pool_get();
    ts_shift_ctx_t ctx = {
        .src = x,
        .dst = out,
        .pool = pool,
        .src_off = lead ? 1 : 0,
        .dst_off = lead ? 0 : 1,
        .esz = esz
    };
    ray_pool_dispatch(pool, ts_shift_copy_fn, &ctx, n - 1);
    if (pool_cancelled(pool)) { ray_release(out); return ray_error("cancel", "interrupted"); }
    ts_set_boundary(out, lead ? n - 1 : 0);
    if (x->attrs & RAY_ATTR_HAS_NULLS) out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}

ray_t* lag_vec_eager(ray_t* x) {
    return shift_vec_eager(x, false);
}

ray_t* lead_vec_eager(ray_t* x) {
    return shift_vec_eager(x, true);
}

static bool ts_delta_type(int8_t t) {
    switch (t) {
        case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
        case RAY_F32: case RAY_F64:
        case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
            return true;
        default:
            return false;
    }
}

static bool ts_ratio_type(int8_t t) {
    switch (t) {
        case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
        case RAY_F32: case RAY_F64:
            return true;
        default:
            return false;
    }
}

typedef struct {
    ray_t* src;
    ray_t* dst;
    ray_pool_t* pool;
    bool   has_nulls;
} ts_delta_ctx_t;

static void ts_deltas_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_delta_ctx_t* c = (ts_delta_ctx_t*)vctx;
    ray_t* src = c->src;
    int8_t t = src->type;
    const void* d = ray_data(src);

    if (c->dst->type == RAY_F64) {
        double* out = (double*)ray_data(c->dst);
        for (int64_t r = start + 1; r < end + 1; r++) {
            if (ts_morsel_checkpoint(c->pool, r - (start + 1))) return;
            if (c->has_nulls && (ray_vec_is_null(src, r) || ray_vec_is_null(src, r - 1))) {
                out[r] = NULL_F64;
                continue;
            }
            double cur = (t == RAY_F64) ? ((const double*)d)[r]
                       : (t == RAY_F32) ? (double)((const float*)d)[r]
                       : (double)read_col_i64(d, r, t, src->attrs);
            double prev = (t == RAY_F64) ? ((const double*)d)[r - 1]
                        : (t == RAY_F32) ? (double)((const float*)d)[r - 1]
                        : (double)read_col_i64(d, r - 1, t, src->attrs);
            out[r] = ray_f64_fin(cur - prev);
        }
    } else {
        int64_t* out = (int64_t*)ray_data(c->dst);
        for (int64_t r = start + 1; r < end + 1; r++) {
            if (ts_morsel_checkpoint(c->pool, r - (start + 1))) return;
            if (c->has_nulls && (ray_vec_is_null(src, r) || ray_vec_is_null(src, r - 1))) {
                out[r] = NULL_I64;
                continue;
            }
            int64_t cur = read_col_i64(d, r, t, src->attrs);
            int64_t prev = read_col_i64(d, r - 1, t, src->attrs);
            out[r] = (int64_t)((uint64_t)cur - (uint64_t)prev);
        }
    }
}

ray_t* deltas_vec_eager(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "deltas expects a vector input");
    if (RAY_IS_PARTED(x->type)) {
        ray_t* flat = parted_to_flat_vec(x);
        if (!flat || RAY_IS_ERR(flat)) return flat ? flat : ray_error("oom", NULL);
        ray_t* out = deltas_vec_eager(flat);
        ray_release(flat);
        return out;
    }
    if (!ts_delta_type(x->type))
        return ray_error("type", "deltas expects a numeric or temporal vector, got %s", ray_type_name(x->type));

    int64_t n = x->len;
    int8_t out_type = (x->type == RAY_F64 || x->type == RAY_F32) ? RAY_F64 : RAY_I64;
    ray_t* out = ray_vec_new(out_type, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;
    if (n == 0) return out;
    out->attrs |= RAY_ATTR_HAS_NULLS;
    if (out_type == RAY_F64) ((double*)ray_data(out))[0] = NULL_F64;
    else                     ((int64_t*)ray_data(out))[0] = NULL_I64;
    if (n == 1) return out;

    ray_pool_t* pool = ray_pool_get();
    ts_delta_ctx_t ctx = {
        .src = x,
        .dst = out,
        .pool = pool,
        .has_nulls = (x->attrs & RAY_ATTR_HAS_NULLS) != 0
    };
    ray_pool_dispatch(pool, ts_deltas_fn, &ctx, n - 1);
    if (pool_cancelled(pool)) { ray_release(out); return ray_error("cancel", "interrupted"); }
    return out;
}

typedef struct {
    ray_t* src;
    ray_t* dst;
    ray_pool_t* pool;
    bool   has_nulls;
} ts_ratio_ctx_t;

static void ts_ratios_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_ratio_ctx_t* c = (ts_ratio_ctx_t*)vctx;
    ray_t* src = c->src;
    int8_t t = src->type;
    const void* d = ray_data(src);
    double* out = (double*)ray_data(c->dst);

    for (int64_t r = start + 1; r < end + 1; r++) {
        if (ts_morsel_checkpoint(c->pool, r - (start + 1))) return;
        if (c->has_nulls && (ray_vec_is_null(src, r) || ray_vec_is_null(src, r - 1))) {
            out[r] = NULL_F64;
            continue;
        }
        double cur = (t == RAY_F64) ? ((const double*)d)[r]
                   : (t == RAY_F32) ? (double)((const float*)d)[r]
                   : (double)read_col_i64(d, r, t, src->attrs);
        double prev = (t == RAY_F64) ? ((const double*)d)[r - 1]
                    : (t == RAY_F32) ? (double)((const float*)d)[r - 1]
                    : (double)read_col_i64(d, r - 1, t, src->attrs);
        out[r] = (prev != 0.0) ? ray_f64_fin(cur / prev) : NULL_F64;
    }
}

ray_t* ratios_vec_eager(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "ratios expects a vector input");
    if (RAY_IS_PARTED(x->type)) {
        ray_t* flat = parted_to_flat_vec(x);
        if (!flat || RAY_IS_ERR(flat)) return flat ? flat : ray_error("oom", NULL);
        ray_t* out = ratios_vec_eager(flat);
        ray_release(flat);
        return out;
    }
    if (!ts_ratio_type(x->type))
        return ray_error("type", "ratios expects a numeric vector, got %s", ray_type_name(x->type));

    int64_t n = x->len;
    ray_t* out = ray_vec_new(RAY_F64, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;
    if (n == 0) return out;
    out->attrs |= RAY_ATTR_HAS_NULLS;
    ((double*)ray_data(out))[0] = NULL_F64;
    if (n == 1) return out;

    ray_pool_t* pool = ray_pool_get();
    ts_ratio_ctx_t ctx = {
        .src = x,
        .dst = out,
        .pool = pool,
        .has_nulls = (x->attrs & RAY_ATTR_HAS_NULLS) != 0
    };
    ray_pool_dispatch(pool, ts_ratios_fn, &ctx, n - 1);
    if (pool_cancelled(pool)) { ray_release(out); return ray_error("cancel", "interrupted"); }
    return out;
}

typedef struct {
    int64_t first_nonnull; /* offset inside block, -1 when the block is all null */
    uint8_t has_last;
    uint8_t last[16];
} ts_fill_summary_t;

typedef struct {
    ray_t* src;
    ray_t* dst;
    ray_pool_t* pool;
    ts_fill_summary_t* summary;
    ts_fill_summary_t* carry;
    int64_t nblocks;
    int esz;
} ts_fill_ctx_t;

static void ts_fills_pass1_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_fill_ctx_t* c = (ts_fill_ctx_t*)vctx;
    const char* src = (const char*)ray_data(c->src);
    char* dst = (char*)ray_data(c->dst);
    size_t esz = (size_t)c->esz;

    for (int64_t b = start; b < end; b++) {
        int64_t lo = b * RAY_MORSEL_ELEMS;
        int64_t hi = lo + RAY_MORSEL_ELEMS;
        if (hi > c->src->len) hi = c->src->len;

        ts_fill_summary_t s = { .first_nonnull = -1, .has_last = 0 };
        uint8_t last[16] = {0};
        bool have = false;

        for (int64_t r = lo; r < hi; r++) {
            if (ts_morsel_checkpoint(c->pool, r - lo)) return;
            char* out_cell = dst + (size_t)r * esz;
            const char* in_cell = src + (size_t)r * esz;
            if (ray_vec_is_null(c->src, r)) {
                if (have) memcpy(out_cell, last, esz);
                else      ray_vec_set_null(c->dst, r, true);
                continue;
            }
            memcpy(out_cell, in_cell, esz);
            memcpy(last, in_cell, esz);
            have = true;
            if (s.first_nonnull < 0) s.first_nonnull = r - lo;
        }

        if (have) {
            s.has_last = 1;
            memcpy(s.last, last, esz);
        }
        c->summary[b] = s;
    }
}

static void ts_fills_pass2_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_fill_ctx_t* c = (ts_fill_ctx_t*)vctx;
    char* dst = (char*)ray_data(c->dst);
    size_t esz = (size_t)c->esz;

    for (int64_t b = start; b < end; b++) {
        ts_fill_summary_t carry = c->carry[b];
        if (!carry.has_last) continue;

        int64_t lo = b * RAY_MORSEL_ELEMS;
        int64_t hi = lo + RAY_MORSEL_ELEMS;
        if (hi > c->dst->len) hi = c->dst->len;
        int64_t first = c->summary[b].first_nonnull;
        int64_t stop = first >= 0 ? lo + first : hi;

        for (int64_t r = lo; r < stop; r++) {
            if (ts_morsel_checkpoint(c->pool, r - lo)) return;
            memcpy(dst + (size_t)r * esz, carry.last, esz);
        }
    }
}

ray_t* fills_vec_eager(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "fills expects a vector input");
    if (RAY_IS_PARTED(x->type)) {
        ray_t* flat = parted_to_flat_vec(x);
        if (!flat || RAY_IS_ERR(flat)) return flat ? flat : ray_error("oom", NULL);
        ray_t* out = fills_vec_eager(flat);
        ray_release(flat);
        return out;
    }
    if (x->type == RAY_SYM || x->type == RAY_STR ||
        x->type == RAY_BOOL || x->type == RAY_U8 ||
        (!(x->attrs & RAY_ATTR_HAS_NULLS) && !(x->attrs & RAY_ATTR_SLICE))) {
        ray_retain(x);
        return x;
    }

    int64_t n = x->len;
    ray_t* out = ts_alloc_like(x);
    if (!out || RAY_IS_ERR(out)) return out;
    if (n == 0) return out;

    int64_t nblocks = (n + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS;
    ray_t* summary_hdr = NULL;
    ray_t* carry_hdr = NULL;
    ts_fill_summary_t* summary = (ts_fill_summary_t*)scratch_calloc(&summary_hdr, (size_t)nblocks * sizeof(ts_fill_summary_t));
    ts_fill_summary_t* carry = (ts_fill_summary_t*)scratch_calloc(&carry_hdr, (size_t)nblocks * sizeof(ts_fill_summary_t));
    if (!summary || !carry) {
        if (summary_hdr) scratch_free(summary_hdr);
        if (carry_hdr) scratch_free(carry_hdr);
        ray_release(out);
        return ray_error("oom", NULL);
    }

    int esz = (x->type == RAY_SYM) ? ray_sym_elem_size(x->type, x->attrs) : ray_elem_size(x->type);
    ray_pool_t* pool = ray_pool_get();
    ts_fill_ctx_t ctx = {
        .src = x, .dst = out, .pool = pool,
        .summary = summary, .carry = carry,
        .nblocks = nblocks, .esz = esz
    };
    ray_pool_dispatch(pool, ts_fills_pass1_fn, &ctx, nblocks);
    if (pool_cancelled(pool)) {
        scratch_free(summary_hdr); scratch_free(carry_hdr); ray_release(out);
        return ray_error("cancel", "interrupted");
    }

    ts_fill_summary_t csum = { .first_nonnull = -1, .has_last = 0 };
    for (int64_t b = 0; b < nblocks; b++) {
        if ((b & 1023) == 0 && pool_cancelled(pool)) {
            scratch_free(summary_hdr); scratch_free(carry_hdr); ray_release(out);
            return ray_error("cancel", "interrupted");
        }
        carry[b] = csum;
        if (summary[b].has_last) csum = summary[b];
    }

    ray_pool_dispatch(pool, ts_fills_pass2_fn, &ctx, nblocks);
    if (pool_cancelled(pool)) {
        scratch_free(summary_hdr); scratch_free(carry_hdr); ray_release(out);
        return ray_error("cancel", "interrupted");
    }
    scratch_free(summary_hdr);
    scratch_free(carry_hdr);
    return out;
}

typedef struct {
    uint8_t seen;
    int64_t count;
    uint64_t acc_i;
    double acc_f;
    int64_t min_i;
    int64_t max_i;
    double min_f;
    double max_f;
} ts_run_state_t;

typedef struct {
    ray_t* src;
    ray_t* dst;
    ray_pool_t* pool;
    ts_run_state_t* summary;
    ts_run_state_t* carry;
    uint16_t opcode;
    bool is_float;
    bool has_nulls;
    int64_t nblocks;
} ts_run_ctx_t;

static bool ts_numeric_type(int8_t t) {
    switch (t) {
        case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
        case RAY_F32: case RAY_F64:
            return true;
        default:
            return false;
    }
}

static bool ts_running_type(uint16_t opcode, int8_t t) {
    if (opcode == OP_MINS || opcode == OP_MAXS) {
        return ts_numeric_type(t) || t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP;
    }
    return ts_numeric_type(t);
}

static int8_t ts_running_out_type(uint16_t opcode, int8_t t) {
    switch (opcode) {
        case OP_AVGS:
            return RAY_F64;
        case OP_SUMS:
        case OP_PRDS:
            return (t == RAY_F64 || t == RAY_F32) ? RAY_F64 : RAY_I64;
        case OP_MINS:
        case OP_MAXS:
            return (t == RAY_F64 || t == RAY_F32) ? RAY_F64 : t;
        default:
            return RAY_I64;
    }
}

static const char* ts_running_name(uint16_t opcode) {
    switch (opcode) {
        case OP_SUMS: return "sums";
        case OP_AVGS: return "avgs";
        case OP_MINS: return "mins";
        case OP_MAXS: return "maxs";
        case OP_PRDS: return "prds";
        default:      return "running";
    }
}

static void ts_run_init(ts_run_state_t* s, uint16_t opcode) {
    memset(s, 0, sizeof(*s));
    if (opcode == OP_PRDS) {
        s->acc_i = 1;
        s->acc_f = 1.0;
    }
}

static double ts_read_as_f64(ray_t* src, int64_t row) {
    const void* d = ray_data(src);
    if (src->type == RAY_F64) return ((const double*)d)[row];
    if (src->type == RAY_F32) return (double)((const float*)d)[row];
    return (double)read_col_i64(d, row, src->type, src->attrs);
}

static int64_t ts_read_as_i64(ray_t* src, int64_t row) {
    return read_col_i64(ray_data(src), row, src->type, src->attrs);
}

static void ts_run_update(ts_run_state_t* s, ray_t* src, int64_t row,
                          uint16_t opcode, bool is_float) {
    if (is_float) {
        double v = ts_read_as_f64(src, row);
        switch (opcode) {
            case OP_SUMS: s->acc_f += v; break;
            case OP_PRDS: s->acc_f *= v; break;
            case OP_AVGS: s->acc_f += v; s->count++; s->seen = 1; break;
            case OP_MINS:
                if (!s->seen || v < s->min_f) s->min_f = v;
                s->seen = 1;
                break;
            case OP_MAXS:
                if (!s->seen || v > s->max_f) s->max_f = v;
                s->seen = 1;
                break;
        }
        return;
    }

    int64_t v = ts_read_as_i64(src, row);
    switch (opcode) {
        case OP_SUMS: s->acc_i += (uint64_t)v; break;
        case OP_PRDS: s->acc_i *= (uint64_t)v; break;
        case OP_AVGS: s->acc_f += (double)v; s->count++; s->seen = 1; break;
        case OP_MINS:
            if (!s->seen || v < s->min_i) s->min_i = v;
            s->seen = 1;
            break;
        case OP_MAXS:
            if (!s->seen || v > s->max_i) s->max_i = v;
            s->seen = 1;
            break;
    }
}

static void ts_run_combine(ts_run_state_t* dst, const ts_run_state_t* src,
                           uint16_t opcode, bool is_float) {
    switch (opcode) {
        case OP_SUMS:
            if (is_float) dst->acc_f += src->acc_f;
            else          dst->acc_i += src->acc_i;
            return;
        case OP_PRDS:
            if (is_float) dst->acc_f *= src->acc_f;
            else          dst->acc_i *= src->acc_i;
            return;
        case OP_AVGS:
            dst->acc_f += src->acc_f;
            dst->count += src->count;
            if (dst->count > 0) dst->seen = 1;
            return;
        case OP_MINS:
            if (!src->seen) return;
            if (is_float) {
                if (!dst->seen || src->min_f < dst->min_f) dst->min_f = src->min_f;
            } else {
                if (!dst->seen || src->min_i < dst->min_i) dst->min_i = src->min_i;
            }
            dst->seen = 1;
            return;
        case OP_MAXS:
            if (!src->seen) return;
            if (is_float) {
                if (!dst->seen || src->max_f > dst->max_f) dst->max_f = src->max_f;
            } else {
                if (!dst->seen || src->max_i > dst->max_i) dst->max_i = src->max_i;
            }
            dst->seen = 1;
            return;
    }
}

static void ts_run_write(ray_t* out, int64_t row, const ts_run_state_t* s,
                         uint16_t opcode, bool is_float) {
    void* d = ray_data(out);
    switch (opcode) {
        case OP_SUMS:
            if (out->type == RAY_F64) ((double*)d)[row] = ray_f64_fin(s->acc_f);
            else ((int64_t*)d)[row] = (int64_t)s->acc_i;
            return;
        case OP_PRDS:
            if (out->type == RAY_F64) ((double*)d)[row] = ray_f64_fin(s->acc_f);
            else ((int64_t*)d)[row] = (int64_t)s->acc_i;
            return;
        case OP_AVGS:
            if (s->count <= 0) ray_vec_set_null(out, row, true);
            else ((double*)d)[row] = ray_f64_fin(s->acc_f / (double)s->count);
            return;
        case OP_MINS:
            if (!s->seen) { ray_vec_set_null(out, row, true); return; }
            if (is_float) ((double*)d)[row] = ray_f64_fin(s->min_f);
            else write_col_i64(d, row, s->min_i, out->type, out->attrs);
            return;
        case OP_MAXS:
            if (!s->seen) { ray_vec_set_null(out, row, true); return; }
            if (is_float) ((double*)d)[row] = ray_f64_fin(s->max_f);
            else write_col_i64(d, row, s->max_i, out->type, out->attrs);
            return;
    }
}

static void ts_running_summary_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_run_ctx_t* c = (ts_run_ctx_t*)vctx;
    for (int64_t b = start; b < end; b++) {
        int64_t lo = b * RAY_MORSEL_ELEMS;
        int64_t hi = lo + RAY_MORSEL_ELEMS;
        if (hi > c->src->len) hi = c->src->len;

        ts_run_state_t s;
        ts_run_init(&s, c->opcode);
        for (int64_t r = lo; r < hi; r++) {
            if (ts_morsel_checkpoint(c->pool, r - lo)) return;
            if (c->has_nulls && ray_vec_is_null(c->src, r)) continue;
            ts_run_update(&s, c->src, r, c->opcode, c->is_float);
        }
        c->summary[b] = s;
    }
}

static void ts_running_emit_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_run_ctx_t* c = (ts_run_ctx_t*)vctx;
    for (int64_t b = start; b < end; b++) {
        int64_t lo = b * RAY_MORSEL_ELEMS;
        int64_t hi = lo + RAY_MORSEL_ELEMS;
        if (hi > c->src->len) hi = c->src->len;

        ts_run_state_t s = c->carry[b];
        for (int64_t r = lo; r < hi; r++) {
            if (ts_morsel_checkpoint(c->pool, r - lo)) return;
            if (!(c->has_nulls && ray_vec_is_null(c->src, r)))
                ts_run_update(&s, c->src, r, c->opcode, c->is_float);
            ts_run_write(c->dst, r, &s, c->opcode, c->is_float);
        }
    }
}

ray_t* running_vec_eager(ray_t* x, uint16_t opcode) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "%s expects a vector input", ts_running_name(opcode));
    if (RAY_IS_PARTED(x->type)) {
        ray_t* flat = parted_to_flat_vec(x);
        if (!flat || RAY_IS_ERR(flat)) return flat ? flat : ray_error("oom", NULL);
        ray_t* out = running_vec_eager(flat, opcode);
        ray_release(flat);
        return out;
    }
    if (!ts_running_type(opcode, x->type))
        return ray_error("type", "%s expects a numeric vector%s, got %s",
                         ts_running_name(opcode),
                         (opcode == OP_MINS || opcode == OP_MAXS) ? " or temporal vector" : "",
                         ray_type_name(x->type));

    int64_t n = x->len;
    int8_t out_type = ts_running_out_type(opcode, x->type);
    ray_t* out = ray_vec_new(out_type, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;
    if (n == 0) return out;

    int64_t nblocks = (n + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS;
    ray_t* summary_hdr = NULL;
    ray_t* carry_hdr = NULL;
    ts_run_state_t* summary = (ts_run_state_t*)scratch_calloc(&summary_hdr, (size_t)nblocks * sizeof(ts_run_state_t));
    ts_run_state_t* carry = (ts_run_state_t*)scratch_calloc(&carry_hdr, (size_t)nblocks * sizeof(ts_run_state_t));
    if (!summary || !carry) {
        if (summary_hdr) scratch_free(summary_hdr);
        if (carry_hdr) scratch_free(carry_hdr);
        ray_release(out);
        return ray_error("oom", NULL);
    }

    ray_pool_t* pool = ray_pool_get();
    ts_run_ctx_t ctx = {
        .src = x, .dst = out, .pool = pool,
        .summary = summary, .carry = carry,
        .opcode = opcode,
        .is_float = (x->type == RAY_F64 || x->type == RAY_F32),
        .has_nulls = (x->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE)) != 0,
        .nblocks = nblocks
    };

    ray_pool_dispatch(pool, ts_running_summary_fn, &ctx, nblocks);
    if (pool_cancelled(pool)) {
        scratch_free(summary_hdr); scratch_free(carry_hdr); ray_release(out);
        return ray_error("cancel", "interrupted");
    }

    ts_run_state_t csum;
    ts_run_init(&csum, opcode);
    for (int64_t b = 0; b < nblocks; b++) {
        if ((b & 1023) == 0 && pool_cancelled(pool)) {
            scratch_free(summary_hdr); scratch_free(carry_hdr); ray_release(out);
            return ray_error("cancel", "interrupted");
        }
        carry[b] = csum;
        ts_run_combine(&csum, &summary[b], opcode, ctx.is_float);
    }

    ray_pool_dispatch(pool, ts_running_emit_fn, &ctx, nblocks);
    if (pool_cancelled(pool)) {
        scratch_free(summary_hdr); scratch_free(carry_hdr); ray_release(out);
        return ray_error("cancel", "interrupted");
    }
    scratch_free(summary_hdr);
    scratch_free(carry_hdr);
    return out;
}

static bool ts_cell_equal(ray_t* x, int64_t a, int64_t b) {
    bool an = ray_vec_is_null(x, a);
    bool bn = ray_vec_is_null(x, b);
    if (an || bn) return an == bn;

    const void* d = ray_data(x);
    switch (x->type) {
        case RAY_BOOL:
        case RAY_U8:
            return ((const uint8_t*)d)[a] == ((const uint8_t*)d)[b];
        case RAY_I16:
            return ((const int16_t*)d)[a] == ((const int16_t*)d)[b];
        case RAY_I32:
        case RAY_DATE:
        case RAY_TIME:
            return ((const int32_t*)d)[a] == ((const int32_t*)d)[b];
        case RAY_I64:
        case RAY_TIMESTAMP:
            return ((const int64_t*)d)[a] == ((const int64_t*)d)[b];
        case RAY_F32:
            return ((const float*)d)[a] == ((const float*)d)[b];
        case RAY_F64:
            return ((const double*)d)[a] == ((const double*)d)[b];
        case RAY_SYM:
            return read_col_i64(d, a, x->type, x->attrs) == read_col_i64(d, b, x->type, x->attrs);
        case RAY_GUID:
            return memcmp((const uint8_t*)d + a * 16, (const uint8_t*)d + b * 16, 16) == 0;
        case RAY_STR: {
            size_t al = 0, bl = 0;
            const char* ap = ray_str_vec_get(x, a, &al);
            const char* bp = ray_str_vec_get(x, b, &bl);
            return al == bl && (!al || (ap && bp && memcmp(ap, bp, al) == 0));
        }
        default:
            return false;
    }
}

typedef struct {
    ray_t* src;
    ray_t* dst;
    ray_pool_t* pool;
} ts_differ_ctx_t;

static void ts_differ_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_differ_ctx_t* c = (ts_differ_ctx_t*)vctx;
    uint8_t* out = (uint8_t*)ray_data(c->dst);
    for (int64_t r = start + 1; r < end + 1; r++) {
        if (ts_morsel_checkpoint(c->pool, r - (start + 1))) return;
        out[r] = ts_cell_equal(c->src, r, r - 1) ? 0 : 1;
    }
}

ray_t* differ_vec_eager(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "differ expects a vector input");
    if (RAY_IS_PARTED(x->type)) {
        ray_t* flat = parted_to_flat_vec(x);
        if (!flat || RAY_IS_ERR(flat)) return flat ? flat : ray_error("oom", NULL);
        ray_t* out = differ_vec_eager(flat);
        ray_release(flat);
        return out;
    }

    int64_t n = x->len;
    ray_t* out = ray_vec_new(RAY_BOOL, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;
    if (n == 0) return out;
    ((uint8_t*)ray_data(out))[0] = 1;
    if (n == 1) return out;

    ray_pool_t* pool = ray_pool_get();
    ts_differ_ctx_t ctx = { .src = x, .dst = out, .pool = pool };
    ray_pool_dispatch(pool, ts_differ_fn, &ctx, n - 1);
    if (pool_cancelled(pool)) { ray_release(out); return ray_error("cancel", "interrupted"); }
    return out;
}

typedef struct {
    uint64_t sum_i;
    double   sum_f;
    double   sum_sq_f;
    int64_t  count;
} ts_mprefix_state_t;

typedef struct {
    ray_t* src;
    ray_t* dst;
    ray_pool_t* pool;
    uint64_t* prefix_i;
    double* prefix_f;
    double* prefix_sq_f;
    int64_t* prefix_count;
    ts_mprefix_state_t* summary;
    ts_mprefix_state_t* carry;
    uint16_t opcode;
    bool has_nulls;
    int64_t window;
} ts_mprefix_ctx_t;

static const char* ts_moving_name(uint16_t opcode) {
    switch (opcode) {
        case OP_MSUM:   return "msum";
        case OP_MAVG:   return "mavg";
        case OP_MMIN:   return "mmin";
        case OP_MMAX:   return "mmax";
        case OP_MCOUNT: return "mcount";
        case OP_MVAR:   return "mvar";
        case OP_MDEV:   return "mdev";
        default:        return "moving";
    }
}

static bool ts_moving_type(uint16_t opcode, int8_t t) {
    switch (opcode) {
        case OP_MCOUNT:
            return true;
        case OP_MMIN:
        case OP_MMAX:
            return ts_numeric_type(t) || t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP;
        case OP_MVAR:
        case OP_MDEV:
            return ts_numeric_type(t);
        default:
            return ts_numeric_type(t);
    }
}

static int8_t ts_moving_out_type(uint16_t opcode, int8_t t) {
    switch (opcode) {
        case OP_MCOUNT:
            return RAY_I64;
        case OP_MAVG:
        case OP_MVAR:
        case OP_MDEV:
            return RAY_F64;
        case OP_MSUM:
            return (t == RAY_F64 || t == RAY_F32) ? RAY_F64 : RAY_I64;
        case OP_MMIN:
        case OP_MMAX:
            return (t == RAY_F64 || t == RAY_F32) ? RAY_F64 : t;
        default:
            return RAY_I64;
    }
}

static bool ts_size_overflows(int64_t n, size_t esz) {
    return n < 0 || (uint64_t)n > ((uint64_t)SIZE_MAX / (uint64_t)esz);
}

static void ts_mprefix_scan_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_mprefix_ctx_t* c = (ts_mprefix_ctx_t*)vctx;
    for (int64_t b = start; b < end; b++) {
        int64_t lo = b * RAY_MORSEL_ELEMS;
        int64_t hi = lo + RAY_MORSEL_ELEMS;
        if (hi > c->src->len) hi = c->src->len;

        uint64_t sum_i = 0;
        double sum_f = 0.0;
        double sum_sq_f = 0.0;
        int64_t count = 0;
        for (int64_t r = lo; r < hi; r++) {
            if (ts_morsel_checkpoint(c->pool, r - lo)) return;
            bool nn = !(c->has_nulls && ray_vec_is_null(c->src, r));
            if (nn) {
                if (c->prefix_i) sum_i += (uint64_t)ts_read_as_i64(c->src, r);
                if (c->prefix_f || c->prefix_sq_f) {
                    double v = ts_read_as_f64(c->src, r);
                    if (c->prefix_f) sum_f += v;
                    if (c->prefix_sq_f) sum_sq_f += v * v;
                }
                count++;
            }
            if (c->prefix_i)      c->prefix_i[r + 1] = sum_i;
            if (c->prefix_f)      c->prefix_f[r + 1] = sum_f;
            if (c->prefix_sq_f)   c->prefix_sq_f[r + 1] = sum_sq_f;
            if (c->prefix_count)  c->prefix_count[r + 1] = count;
        }
        c->summary[b] = (ts_mprefix_state_t){
            .sum_i = sum_i, .sum_f = sum_f, .sum_sq_f = sum_sq_f, .count = count
        };
    }
}

static void ts_mprefix_adjust_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_mprefix_ctx_t* c = (ts_mprefix_ctx_t*)vctx;
    for (int64_t b = start; b < end; b++) {
        int64_t lo = b * RAY_MORSEL_ELEMS;
        int64_t hi = lo + RAY_MORSEL_ELEMS;
        if (hi > c->src->len) hi = c->src->len;
        ts_mprefix_state_t carry = c->carry[b];

        for (int64_t r = lo; r < hi; r++) {
            if (ts_morsel_checkpoint(c->pool, r - lo)) return;
            if (c->prefix_i)      c->prefix_i[r + 1] += carry.sum_i;
            if (c->prefix_f)      c->prefix_f[r + 1] += carry.sum_f;
            if (c->prefix_sq_f)   c->prefix_sq_f[r + 1] += carry.sum_sq_f;
            if (c->prefix_count)  c->prefix_count[r + 1] += carry.count;
        }
    }
}

static void ts_mprefix_emit_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_mprefix_ctx_t* c = (ts_mprefix_ctx_t*)vctx;
    void* out = ray_data(c->dst);
    for (int64_t r = start; r < end; r++) {
        if (ts_morsel_checkpoint(c->pool, r - start)) return;
        int64_t lo = (r >= c->window - 1) ? (r - c->window + 1) : 0;
        int64_t hi = r + 1;
        switch (c->opcode) {
            case OP_MCOUNT:
                ((int64_t*)out)[r] = c->prefix_count[hi] - c->prefix_count[lo];
                break;
            case OP_MSUM:
                if (c->prefix_f)
                    ((double*)out)[r] = ray_f64_fin(c->prefix_f[hi] - c->prefix_f[lo]);
                else
                    ((int64_t*)out)[r] = (int64_t)(c->prefix_i[hi] - c->prefix_i[lo]);
                break;
            case OP_MAVG: {
                int64_t cnt = c->prefix_count[hi] - c->prefix_count[lo];
                if (cnt <= 0) {
                    ray_vec_set_null(c->dst, r, true);
                } else {
                    ((double*)out)[r] = ray_f64_fin((c->prefix_f[hi] - c->prefix_f[lo]) / (double)cnt);
                }
                break;
            }
            case OP_MVAR:
            case OP_MDEV: {
                int64_t cnt = c->prefix_count[hi] - c->prefix_count[lo];
                if (cnt <= 0) {
                    ray_vec_set_null(c->dst, r, true);
                } else {
                    double sum = c->prefix_f[hi] - c->prefix_f[lo];
                    double sum_sq = c->prefix_sq_f[hi] - c->prefix_sq_f[lo];
                    double mean = sum / (double)cnt;
                    double var = sum_sq / (double)cnt - mean * mean;
                    if (var < 0.0) var = 0.0;
                    ((double*)out)[r] = ray_f64_fin(c->opcode == OP_MDEV ? sqrt(var) : var);
                }
                break;
            }
        }
    }
}

static ray_t* moving_prefix_vec_eager(ray_t* x, uint16_t opcode, int64_t window, int8_t out_type) {
    int64_t n = x->len;
    ray_t* out = ray_vec_new(out_type, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;
    if (n == 0) return out;

    int64_t nblocks = (n + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS;
    int64_t entries = n + 1;
    ray_t *pi_hdr = NULL, *pf_hdr = NULL, *psq_hdr = NULL, *pc_hdr = NULL, *sum_hdr = NULL, *carry_hdr = NULL;
    uint64_t* prefix_i = NULL;
    double* prefix_f = NULL;
    double* prefix_sq_f = NULL;
    int64_t* prefix_count = NULL;

    bool need_i = (opcode == OP_MSUM && out_type == RAY_I64);
    bool need_sq = (opcode == OP_MVAR || opcode == OP_MDEV);
    bool need_f = (opcode == OP_MAVG || need_sq || (opcode == OP_MSUM && out_type == RAY_F64));
    bool need_count = (opcode == OP_MAVG || opcode == OP_MCOUNT || need_sq);

    if (need_i) {
        if (ts_size_overflows(entries, sizeof(uint64_t))) { ray_release(out); return ray_error("oom", NULL); }
        prefix_i = (uint64_t*)scratch_calloc(&pi_hdr, (size_t)entries * sizeof(uint64_t));
    }
    if (need_f) {
        if (ts_size_overflows(entries, sizeof(double))) {
            if (pi_hdr) scratch_free(pi_hdr);
            ray_release(out);
            return ray_error("oom", NULL);
        }
        prefix_f = (double*)scratch_calloc(&pf_hdr, (size_t)entries * sizeof(double));
    }
    if (need_sq) {
        if (ts_size_overflows(entries, sizeof(double))) {
            if (pi_hdr) scratch_free(pi_hdr);
            if (pf_hdr) scratch_free(pf_hdr);
            ray_release(out);
            return ray_error("oom", NULL);
        }
        prefix_sq_f = (double*)scratch_calloc(&psq_hdr, (size_t)entries * sizeof(double));
    }
    if (need_count) {
        if (ts_size_overflows(entries, sizeof(int64_t))) {
            if (pi_hdr) scratch_free(pi_hdr);
            if (pf_hdr) scratch_free(pf_hdr);
            if (psq_hdr) scratch_free(psq_hdr);
            ray_release(out);
            return ray_error("oom", NULL);
        }
        prefix_count = (int64_t*)scratch_calloc(&pc_hdr, (size_t)entries * sizeof(int64_t));
    }
    ts_mprefix_state_t* summary = (ts_mprefix_state_t*)scratch_calloc(&sum_hdr, (size_t)nblocks * sizeof(ts_mprefix_state_t));
    ts_mprefix_state_t* carry = (ts_mprefix_state_t*)scratch_calloc(&carry_hdr, (size_t)nblocks * sizeof(ts_mprefix_state_t));
    if ((need_i && !prefix_i) || (need_f && !prefix_f) || (need_sq && !prefix_sq_f) || (need_count && !prefix_count) || !summary || !carry) {
        if (pi_hdr) scratch_free(pi_hdr);
        if (pf_hdr) scratch_free(pf_hdr);
        if (psq_hdr) scratch_free(psq_hdr);
        if (pc_hdr) scratch_free(pc_hdr);
        if (sum_hdr) scratch_free(sum_hdr);
        if (carry_hdr) scratch_free(carry_hdr);
        ray_release(out);
        return ray_error("oom", NULL);
    }

    ray_pool_t* pool = ray_pool_get();
    ts_mprefix_ctx_t ctx = {
        .src = x, .dst = out, .pool = pool,
        .prefix_i = prefix_i, .prefix_f = prefix_f, .prefix_sq_f = prefix_sq_f,
        .prefix_count = prefix_count,
        .summary = summary, .carry = carry,
        .opcode = opcode,
        .has_nulls = (x->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE)) != 0,
        .window = window
    };

    ray_pool_dispatch(pool, ts_mprefix_scan_fn, &ctx, nblocks);
    if (pool_cancelled(pool)) goto cancelled;

    ts_mprefix_state_t run = {0};
    for (int64_t b = 0; b < nblocks; b++) {
        if ((b & 1023) == 0 && pool_cancelled(pool)) goto cancelled;
        carry[b] = run;
        run.sum_i += summary[b].sum_i;
        run.sum_f += summary[b].sum_f;
        run.sum_sq_f += summary[b].sum_sq_f;
        run.count += summary[b].count;
    }

    ray_pool_dispatch(pool, ts_mprefix_adjust_fn, &ctx, nblocks);
    if (pool_cancelled(pool)) goto cancelled;
    ray_pool_dispatch(pool, ts_mprefix_emit_fn, &ctx, n);
    if (pool_cancelled(pool)) goto cancelled;

    if (pi_hdr) scratch_free(pi_hdr);
    if (pf_hdr) scratch_free(pf_hdr);
    if (psq_hdr) scratch_free(psq_hdr);
    if (pc_hdr) scratch_free(pc_hdr);
    scratch_free(sum_hdr);
    scratch_free(carry_hdr);
    return out;

cancelled:
    if (pi_hdr) scratch_free(pi_hdr);
    if (pf_hdr) scratch_free(pf_hdr);
    if (psq_hdr) scratch_free(psq_hdr);
    if (pc_hdr) scratch_free(pc_hdr);
    scratch_free(sum_hdr);
    scratch_free(carry_hdr);
    ray_release(out);
    return ray_error("cancel", "interrupted");
}

typedef struct {
    ray_t* src;
    ray_t* dst;
    ray_pool_t* pool;
    uint16_t opcode;
    bool is_float;
    bool has_nulls;
    int64_t window;
    int64_t ntasks;
    _Atomic(int) oom;
} ts_mminmax_ctx_t;

static inline int64_t ts_task_lo(int64_t n, int64_t task, int64_t ntasks) {
    int64_t q = n / ntasks;
    int64_t r = n % ntasks;
    return q * task + (r * task) / ntasks;
}

static bool ts_mminmax_better(ts_mminmax_ctx_t* c, int64_t a, int64_t b) {
    if (c->is_float) {
        double av = ts_read_as_f64(c->src, a);
        double bv = ts_read_as_f64(c->src, b);
        return c->opcode == OP_MMIN ? av <= bv : av >= bv;
    }
    int64_t av = ts_read_as_i64(c->src, a);
    int64_t bv = ts_read_as_i64(c->src, b);
    return c->opcode == OP_MMIN ? av <= bv : av >= bv;
}

static void ts_mminmax_write(ts_mminmax_ctx_t* c, int64_t out_row, int64_t src_row) {
    void* out = ray_data(c->dst);
    if (c->is_float) ((double*)out)[out_row] = ray_f64_fin(ts_read_as_f64(c->src, src_row));
    else write_col_i64(out, out_row, ts_read_as_i64(c->src, src_row), c->dst->type, c->dst->attrs);
}

static void ts_mminmax_task_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    ts_mminmax_ctx_t* c = (ts_mminmax_ctx_t*)vctx;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;

    int64_t n = c->src->len;
    for (int64_t task = start; task < end; task++) {
        int64_t lo = ts_task_lo(n, task, c->ntasks);
        int64_t hi = ts_task_lo(n, task + 1, c->ntasks);
        if (lo >= hi) continue;
        int64_t scan_lo = (lo >= c->window - 1) ? (lo - c->window + 1) : 0;
        int64_t scan_len = hi - scan_lo;
        int64_t cap_i64 = c->window < scan_len ? c->window : scan_len;
        if (cap_i64 < 1) cap_i64 = 1;
        if (ts_size_overflows(cap_i64, sizeof(int64_t))) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }

        ray_t* dq_hdr = NULL;
        int64_t* dq = (int64_t*)scratch_alloc(&dq_hdr, (size_t)cap_i64 * sizeof(int64_t));
        if (!dq) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }

        int64_t head = 0;
        int64_t len = 0;
        for (int64_t r = scan_lo; r < hi; r++) {
            if (ts_morsel_checkpoint(c->pool, r - scan_lo)) { scratch_free(dq_hdr); return; }
            int64_t expire = (r >= c->window - 1) ? (r - c->window + 1) : 0;
            while (len > 0 && dq[head] < expire) {
                head = (head + 1) % cap_i64;
                len--;
            }

            bool nn = !(c->has_nulls && ray_vec_is_null(c->src, r));
            if (nn) {
                while (len > 0) {
                    int64_t back_pos = (head + len - 1) % cap_i64;
                    if (!ts_mminmax_better(c, r, dq[back_pos])) break;
                    len--;
                }
                dq[(head + len) % cap_i64] = r;
                len++;
            }

            if (r >= lo) {
                if (len == 0) ray_vec_set_null(c->dst, r, true);
                else          ts_mminmax_write(c, r, dq[head]);
            }
        }
        scratch_free(dq_hdr);
    }
}

static ray_t* moving_minmax_vec_eager(ray_t* x, uint16_t opcode, int64_t window, int8_t out_type) {
    int64_t n = x->len;
    ray_t* out = ray_vec_new(out_type, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;
    if (n == 0) return out;

    ray_pool_t* pool = ray_pool_get();
    uint32_t workers = pool ? pool->n_workers + 1 : 1;
    int64_t overlap_tasks = (window > 0) ? (n + window - 1) / window : 1;
    if (overlap_tasks < 1) overlap_tasks = 1;
    uint32_t ntasks = workers;
    if ((int64_t)ntasks > overlap_tasks) ntasks = (uint32_t)overlap_tasks;
    if (ntasks < 1) ntasks = 1;
    if (n < RAY_PARALLEL_THRESHOLD) ntasks = 1;

    ts_mminmax_ctx_t ctx = {
        .src = x, .dst = out, .pool = pool,
        .opcode = opcode,
        .is_float = (x->type == RAY_F64 || x->type == RAY_F32),
        .has_nulls = (x->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE)) != 0,
        .window = window,
        .ntasks = (int64_t)ntasks
    };
    atomic_store_explicit(&ctx.oom, 0, memory_order_relaxed);

    if (ntasks == 1) ts_mminmax_task_fn(&ctx, 0, 0, 1);
    else             ray_pool_dispatch_n(pool, ts_mminmax_task_fn, &ctx, ntasks);

    if (atomic_load_explicit(&ctx.oom, memory_order_relaxed)) {
        ray_release(out);
        return ray_error("oom", NULL);
    }
    if (pool_cancelled(pool)) {
        ray_release(out);
        return ray_error("cancel", "interrupted");
    }
    return out;
}

ray_t* moving_vec_eager(ray_t* x, uint16_t opcode, int64_t window) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (!ray_is_vec(x)) return ray_error("type", "%s expects a vector input", ts_moving_name(opcode));
    if (window < 1) return ray_error("range", "%s window must be >= 1", ts_moving_name(opcode));
    if (RAY_IS_PARTED(x->type)) {
        ray_t* flat = parted_to_flat_vec(x);
        if (!flat || RAY_IS_ERR(flat)) return flat ? flat : ray_error("oom", NULL);
        ray_t* out = moving_vec_eager(flat, opcode, window);
        ray_release(flat);
        return out;
    }
    if (!ts_moving_type(opcode, x->type)) {
        return ray_error("type", "%s expects %s, got %s",
                         ts_moving_name(opcode),
                         (opcode == OP_MMIN || opcode == OP_MMAX) ? "a numeric or temporal vector" :
                         (opcode == OP_MCOUNT) ? "a vector" : "a numeric vector",
                         ray_type_name(x->type));
    }

    int8_t out_type = ts_moving_out_type(opcode, x->type);
    if (opcode == OP_MMIN || opcode == OP_MMAX)
        return moving_minmax_vec_eager(x, opcode, window, out_type);
    return moving_prefix_vec_eager(x, opcode, window, out_type);
}

static ray_t* ts_start_lazy_unary(ray_t* x, ray_op_t* (*ctor)(ray_graph_t*, ray_op_t*)) {
    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* in = ray_graph_input_vec(g, x);
    ray_op_t* op = ctor(g, in);
    return ray_lazy_wrap(g, op);
}

static ray_t* ts_start_lazy_moving(ray_t* x, uint16_t opcode, int64_t window) {
    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* in = ray_graph_input_vec(g, x);
    ray_op_t* op = NULL;
    switch (opcode) {
        case OP_MSUM:   op = ray_msum_op(g, in, window); break;
        case OP_MAVG:   op = ray_mavg_op(g, in, window); break;
        case OP_MMIN:   op = ray_mmin_op(g, in, window); break;
        case OP_MMAX:   op = ray_mmax_op(g, in, window); break;
        case OP_MCOUNT: op = ray_mcount_op(g, in, window); break;
        case OP_MVAR:   op = ray_mvar_op(g, in, window); break;
        case OP_MDEV:   op = ray_mdev_op(g, in, window); break;
    }
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    return ray_lazy_wrap(g, op);
}

static bool ts_window_arg(ray_t* n, int64_t* out) {
    if (!n || !ray_is_atom(n) || RAY_ATOM_IS_NULL(n)) return false;
    switch (n->type) {
        case -RAY_I64:  *out = n->i64; return true;
        case -RAY_I32:  *out = (int64_t)n->i32; return true;
        case -RAY_I16:  *out = (int64_t)n->i16; return true;
        case -RAY_U8:
        case -RAY_BOOL: *out = (int64_t)n->u8; return true;
        default: return false;
    }
}

static ray_t* ts_moving_fn(ray_t* n, ray_t* x, uint16_t opcode) {
    int64_t window = 0;
    if (!ts_window_arg(n, &window))
        return ray_error("type", "%s window must be an integer atom", ts_moving_name(opcode));
    if (window < 1)
        return ray_error("range", "%s window must be >= 1", ts_moving_name(opcode));
    if (ray_is_lazy(x)) return ray_lazy_append_param(x, opcode, window);
    if (ray_is_vec(x)) return ts_start_lazy_moving(x, opcode, window);
    return ray_error("type", "%s expects a vector input, got %s", ts_moving_name(opcode), ray_type_name(x ? x->type : 0));
}

ray_t* ray_lag_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_LAG);
    if (ray_is_vec(x)) return ts_start_lazy_unary(x, ray_lag_op);
    return ray_error("type", "lag expects a vector input, got %s", ray_type_name(x ? x->type : 0));
}

ray_t* ray_lead_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_LEAD);
    if (ray_is_vec(x)) return ts_start_lazy_unary(x, ray_lead_op);
    return ray_error("type", "lead expects a vector input, got %s", ray_type_name(x ? x->type : 0));
}

ray_t* ray_deltas_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_DELTAS);
    if (ray_is_vec(x)) return ts_start_lazy_unary(x, ray_deltas_op);
    return ray_error("type", "deltas expects a vector input, got %s", ray_type_name(x ? x->type : 0));
}

ray_t* ray_ratios_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_RATIOS);
    if (ray_is_vec(x)) return ts_start_lazy_unary(x, ray_ratios_op);
    return ray_error("type", "ratios expects a vector input, got %s", ray_type_name(x ? x->type : 0));
}

ray_t* ray_fills_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_FILLS);
    if (ray_is_vec(x)) return ts_start_lazy_unary(x, ray_fills_op);
    return ray_error("type", "fills expects a vector input, got %s", ray_type_name(x ? x->type : 0));
}

ray_t* ray_sums_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_SUMS);
    if (ray_is_vec(x)) return ts_start_lazy_unary(x, ray_sums_op);
    return ray_error("type", "sums expects a vector input, got %s", ray_type_name(x ? x->type : 0));
}

ray_t* ray_avgs_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_AVGS);
    if (ray_is_vec(x)) return ts_start_lazy_unary(x, ray_avgs_op);
    return ray_error("type", "avgs expects a vector input, got %s", ray_type_name(x ? x->type : 0));
}

ray_t* ray_mins_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_MINS);
    if (ray_is_vec(x)) return ts_start_lazy_unary(x, ray_mins_op);
    return ray_error("type", "mins expects a vector input, got %s", ray_type_name(x ? x->type : 0));
}

ray_t* ray_maxs_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_MAXS);
    if (ray_is_vec(x)) return ts_start_lazy_unary(x, ray_maxs_op);
    return ray_error("type", "maxs expects a vector input, got %s", ray_type_name(x ? x->type : 0));
}

ray_t* ray_prds_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_PRDS);
    if (ray_is_vec(x)) return ts_start_lazy_unary(x, ray_prds_op);
    return ray_error("type", "prds expects a vector input, got %s", ray_type_name(x ? x->type : 0));
}

ray_t* ray_differ_fn(ray_t* x) {
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_DIFFER);
    if (ray_is_vec(x)) return ts_start_lazy_unary(x, ray_differ_op);
    return ray_error("type", "differ expects a vector input, got %s", ray_type_name(x ? x->type : 0));
}

ray_t* ray_msum_fn(ray_t* n, ray_t* x) {
    return ts_moving_fn(n, x, OP_MSUM);
}

ray_t* ray_mavg_fn(ray_t* n, ray_t* x) {
    return ts_moving_fn(n, x, OP_MAVG);
}

ray_t* ray_mmin_fn(ray_t* n, ray_t* x) {
    return ts_moving_fn(n, x, OP_MMIN);
}

ray_t* ray_mmax_fn(ray_t* n, ray_t* x) {
    return ts_moving_fn(n, x, OP_MMAX);
}

ray_t* ray_mcount_fn(ray_t* n, ray_t* x) {
    return ts_moving_fn(n, x, OP_MCOUNT);
}

ray_t* ray_mvar_fn(ray_t* n, ray_t* x) {
    return ts_moving_fn(n, x, OP_MVAR);
}

ray_t* ray_mdev_fn(ray_t* n, ray_t* x) {
    return ts_moving_fn(n, x, OP_MDEV);
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
    if (!is_list(x)) { if (_bx) ray_release(_bx); return ray_error("type", "reverse: argument must be a list, got %s", ray_type_name(x->type)); }
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
    if (!ray_is_atom(a) || !ray_is_atom(b)) return ray_error("type", "rand: count and max must be atoms, got %s and %s", ray_type_name(a->type), ray_type_name(b->type));
    int64_t n, mx;
    if (a->type == -RAY_I64) n = a->i64;
    else if (a->type == -RAY_I32) n = a->i32;
    else return ray_error("type", "rand: count must be an integer, got %s", ray_type_name(a->type));
    if (b->type == -RAY_I64) mx = b->i64;
    else if (b->type == -RAY_I32) mx = b->i32;
    else return ray_error("type", "rand: max must be an integer, got %s", ray_type_name(b->type));
    if (n < 0) return ray_error("domain", "rand: count must be non-negative, got %lld", (long long)n);
    if (mx <= 0) return ray_error("domain", "rand: max must be positive, got %lld", (long long)mx);
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
        return ray_error("type", "bin: sorted arg must be an i64 vector, got %s", ray_type_name(sorted->type));
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
    return ray_error("type", "bin: val must be an i64 atom or vector, got %s", ray_type_name(val->type));
}

/* (binr sorted-vec val) → leftmost index where sorted[i] >= val */
ray_t* ray_binr_fn(ray_t* sorted, ray_t* val) {
    if (!ray_is_vec(sorted) || sorted->type != RAY_I64)
        return ray_error("type", "binr: sorted arg must be an i64 vector, got %s", ray_type_name(sorted->type));
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
    return ray_error("type", "binr: val must be an i64 atom or vector, got %s", ray_type_name(val->type));
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

/* (map-left fn left right): iterate the LEFT operand, pairing each element with
 * the whole RIGHT → fn(left_i, right).
 * (map-right fn left right): iterate the RIGHT operand, pairing the whole LEFT
 * with each element → fn(left, right_i).
 *
 * The iterated side is fixed by the operator, not auto-detected. When that side
 * is an atom there is nothing to iterate, so fn is applied once to (left,right);
 * for an atomic fn that single call is exactly its broadcast over the other
 * operand, so map-left/map-right agree with plain atomic application there.
 * Every call goes through call_fn2, which routes atomic builtins through the
 * broadcasting engine (so a held vector conforms element-wise) and hands
 * lambdas their arguments whole. */
ray_t* ray_map_left_fn(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("domain", "map-left: requires exactly 3 args (fn, left, right), got %lld", (long long)n);
    /* iterate left (vec), hold right (fixed) → fn(left_i, right) */
    return map_iterate(args[0], args[2], args[1], 0);
}

ray_t* ray_map_right_fn(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("domain", "map-right: requires exactly 3 args (fn, left, right), got %lld", (long long)n);
    /* iterate right (vec), hold left (fixed) → fn(left, right_i) */
    return map_iterate(args[0], args[1], args[2], 1);
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
    if (n < 2) return ray_error("domain", "fold-right: requires at least 2 args (fn and vec), got %lld", (long long)n);
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
        if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", "fold-right: vec arg must be a list, got %s", ray_type_name(vec->type)); }
        int64_t len = ray_len(vec);
        if (len == 0) { if (_bx) ray_release(_bx); return ray_error("domain", "fold-right: empty list has no initial value, got %lld", (long long)len); }
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
    if (!is_list(vec)) { ray_release(acc); if (_bx) ray_release(_bx); return ray_error("type", "fold-right: vec arg must be a list, got %s", ray_type_name(vec->type)); }
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
    if (n < 2) return ray_error("domain", "scan-right: requires at least 2 args (fn and vec), got %lld", (long long)n);
    for (int64_t i = 0; i < n; i++)
        if (ray_is_lazy(args[i])) args[i] = ray_lazy_materialize(args[i]);

    ray_t* fn = args[0];
    ray_t* _bx = NULL;
    ray_t* vec = unbox_vec_arg(args[1], &_bx);
    if (RAY_IS_ERR(vec)) return vec;
    if (!is_list(vec)) { if (_bx) ray_release(_bx); return ray_error("type", "scan-right: vec arg must be a list, got %s", ray_type_name(vec->type)); }
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
