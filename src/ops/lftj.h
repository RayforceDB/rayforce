/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_LFTJ_H
#define RAY_LFTJ_H

#include "ops.h"
#include "store/csr.h"

/* Trie iterator over sorted CSR adjacency list */
typedef struct ray_lftj_iter {
    int64_t* targets;        /* pointer into CSR targets data */
    int64_t  start;          /* current range start */
    int64_t  end;            /* current range end */
    int64_t  pos;            /* current position in [start, end) */
} ray_lftj_iter_t;

/* O(1) */
static inline int64_t lftj_key(ray_lftj_iter_t* it) {
    if (!it->targets || it->pos >= it->end) return INT64_MAX;
    return it->targets[it->pos];
}

static inline bool lftj_at_end(ray_lftj_iter_t* it) {
    return !it->targets || it->pos >= it->end;
}

static inline void lftj_next(ray_lftj_iter_t* it) {
    if (it->pos < it->end) it->pos++;
}

/* O(log degree) - binary search within [pos, end) */
static inline void lftj_seek(ray_lftj_iter_t* it, int64_t v) {
    if (!it->targets) { it->pos = it->end; return; }
    int64_t lo = it->pos, hi = it->end;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (it->targets[mid] < v) lo = mid + 1;
        else hi = mid;
    }
    it->pos = lo;
}

/* Open trie level: set iterator to a node's adjacency list */
static inline void lftj_open(ray_lftj_iter_t* it, ray_csr_t* csr, int64_t parent) {
    if (!csr || !csr->offsets || !csr->targets
        || parent < 0 || parent >= csr->n_nodes) {
        it->targets = NULL; it->start = 0; it->end = 0; it->pos = 0;
        return;
    }
    int64_t* o = (int64_t*)ray_data(csr->offsets);
    it->targets = (int64_t*)ray_data(csr->targets);
    it->start = o[parent];
    it->end   = o[parent + 1];
    it->pos   = it->start;
}

/* Leapfrog search: intersect k sorted iterators */
bool leapfrog_search(ray_lftj_iter_t** iters, int k, int64_t* out);

/* --------------------------------------------------------------------------
 * General LFTJ enumeration
 * -------------------------------------------------------------------------- */

#define LFTJ_MAX_VARS 16
#define LFTJ_MAX_ITERS_PER_VAR 8

/* Binding entry: one iterator constraint on a variable.
 * "Open CSR `csr` at the node bound to `bound_var`" */
typedef struct lftj_binding {
    ray_csr_t* csr;           /* CSR to open (fwd or rev of some rel) */
    uint8_t   bound_var;     /* index of already-bound variable providing the parent node */
} lftj_binding_t;

/* Per-variable binding plan */
typedef struct lftj_var_plan {
    lftj_binding_t bindings[LFTJ_MAX_ITERS_PER_VAR];
    uint8_t        n_bindings;
} lftj_var_plan_t;

/* Enumeration context */
typedef struct lftj_enum_ctx {
    lftj_var_plan_t var_plans[LFTJ_MAX_VARS];
    uint8_t         n_vars;
    int64_t         bound[LFTJ_MAX_VARS];   /* currently bound values */

    /* Output buffers (caller-owned, dynamically grown) */
    int64_t**       col_data;    /* [n_vars] arrays of output values */
    int64_t         out_count;
    int64_t         out_cap;
    ray_t*           buf_hdrs[LFTJ_MAX_VARS]; /* scratch headers for realloc */
    bool            oom;         /* set on allocation failure */
} lftj_enum_ctx_t;

/* Build binding plan from relationship array.
 * Assumes variable ordering 0..n_vars-1.
 * For each rel: rel[i] connects src_var→dst_var.
 * The caller encodes this mapping as (src_var, dst_var) pairs.
 * Returns true on success. */
bool lftj_build_plan(lftj_enum_ctx_t* ctx,
                     ray_rel_t** rels, uint8_t n_rels, uint8_t n_vars,
                     const uint8_t* rel_src_var, const uint8_t* rel_dst_var);

/* Build default binding plan for simple patterns.
 * Triangle (n_vars=3, n_rels=3): rels[0]=a→b, rels[1]=b→c, rels[2]=a→c
 * 2-var (n_vars=2): all rels connect var 0→var 1
 * Returns true on success, false if pattern not recognized. */
bool lftj_build_default_plan(lftj_enum_ctx_t* ctx,
                             ray_rel_t** rels, uint8_t n_rels, uint8_t n_vars);

/* Recursive backtracking enumeration.
 * Caller must initialize ctx->col_data, out_cap, out_count=0, buf_hdrs.
 * Populates ctx->col_data with matching tuples. */
void lftj_enumerate(lftj_enum_ctx_t* ctx, uint8_t depth);

#endif /* RAY_LFTJ_H */
