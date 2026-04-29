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

#include "lftj.h"
#include <string.h>

/* Grow output buffers when full. Returns false on OOM. */
static bool lftj_grow_output(lftj_enum_ctx_t* ctx) {
    if (ctx->out_cap > INT64_MAX / 2) return false;
    int64_t new_cap = ctx->out_cap < 64 ? 64 : ctx->out_cap * 2;
    /* Allocate all new blocks first (atomic: no state change on failure) */
    ray_t* new_hdrs[LFTJ_MAX_VARS];
    for (uint8_t v = 0; v < ctx->n_vars; v++) {
        new_hdrs[v] = ray_alloc((size_t)new_cap * sizeof(int64_t));
        if (!new_hdrs[v]) {
            for (uint8_t j = 0; j < v; j++) ray_free(new_hdrs[j]);
            return false;
        }
        memcpy(ray_data(new_hdrs[v]), ctx->col_data[v],
               (size_t)ctx->out_count * sizeof(int64_t));
    }
    /* Commit: swap pointers (no allocation can fail past here) */
    for (uint8_t v = 0; v < ctx->n_vars; v++) {
        ray_free(ctx->buf_hdrs[v]);
        ctx->buf_hdrs[v] = new_hdrs[v];
        ctx->col_data[v] = (int64_t*)ray_data(new_hdrs[v]);
    }
    ctx->out_cap = new_cap;
    return true;
}

/* --------------------------------------------------------------------------
 * Leapfrog search: intersect k sorted iterators
 * Returns true + sets *out if intersection found.
 * -------------------------------------------------------------------------- */

bool leapfrog_search(ray_lftj_iter_t** iters, int k, int64_t* out) {
    if (k <= 0) return false;

    /* Check for any exhausted iterator */
    for (int i = 0; i < k; i++)
        if (lftj_at_end(iters[i])) return false;

    /* Find initial max */
    int max_idx = 0;
    for (int i = 1; i < k; i++)
        if (lftj_key(iters[i]) > lftj_key(iters[max_idx])) max_idx = i;

    for (;;) {
        int64_t max_val = lftj_key(iters[max_idx]);
        int next = (max_idx + 1) % k;

        lftj_seek(iters[next], max_val);
        if (lftj_at_end(iters[next])) return false;

        if (lftj_key(iters[next]) == max_val) {
            /* Check all iterators agree */
            bool all_equal = true;
            for (int i = 0; i < k; i++) {
                if (lftj_key(iters[i]) != max_val) {
                    all_equal = false;
                    break;
                }
            }
            if (all_equal) {
                *out = max_val;
                return true;
            }
        }
        max_idx = next;
    }
}

/* --------------------------------------------------------------------------
 * Binding plan construction
 * -------------------------------------------------------------------------- */

bool lftj_build_plan(lftj_enum_ctx_t* ctx,
                     ray_rel_t** rels, uint8_t n_rels, uint8_t n_vars,
                     const uint8_t* rel_src_var, const uint8_t* rel_dst_var) {
    if (n_vars > LFTJ_MAX_VARS) return false;
    ctx->n_vars = n_vars;

    for (uint8_t v = 0; v < n_vars; v++)
        ctx->var_plans[v].n_bindings = 0;

    /* For each relationship, add bindings to the appropriate variables.
     * A relationship rel[i] connecting src_var→dst_var adds:
     *   - If dst_var has higher index: binding on dst_var using fwd CSR, bound_var=src_var
     *   - If src_var has higher index: binding on src_var using rev CSR, bound_var=dst_var
     *
     * For the first variable (depth 0), we need a special "root" iterator
     * that enumerates all nodes. We handle this differently: depth-0 variable
     * gets bindings from all rels where it's the src, using a full range iterator.
     */
    for (uint8_t r = 0; r < n_rels; r++) {
        uint8_t sv = rel_src_var[r];
        uint8_t dv = rel_dst_var[r];

        /* Self-loop (sv == dv) is invalid — skip it */
        if (sv == dv) continue;
        if (sv >= n_vars || dv >= n_vars) return false;

        /* Add binding to the later-bound variable */
        if (sv < dv) {
            /* sv is bound first; add fwd binding to dv */
            lftj_var_plan_t* vp = &ctx->var_plans[dv];
            if (vp->n_bindings >= LFTJ_MAX_ITERS_PER_VAR) return false;
            vp->bindings[vp->n_bindings].csr = &rels[r]->fwd;
            vp->bindings[vp->n_bindings].bound_var = sv;
            vp->n_bindings++;
        } else {
            /* dv is bound first; add rev binding to sv */
            lftj_var_plan_t* vp = &ctx->var_plans[sv];
            if (vp->n_bindings >= LFTJ_MAX_ITERS_PER_VAR) return false;
            vp->bindings[vp->n_bindings].csr = &rels[r]->rev;
            vp->bindings[vp->n_bindings].bound_var = dv;
            vp->n_bindings++;
        }
    }

    return true;
}

bool lftj_build_default_plan(lftj_enum_ctx_t* ctx,
                             ray_rel_t** rels, uint8_t n_rels, uint8_t n_vars) {
    if (n_vars == 3 && n_rels == 3) {
        /* Triangle: rels[0]=a→b, rels[1]=b→c, rels[2]=a→c */
        uint8_t src_vars[3] = {0, 1, 0};
        uint8_t dst_vars[3] = {1, 2, 2};
        return lftj_build_plan(ctx, rels, n_rels, n_vars, src_vars, dst_vars);
    } else if (n_vars == 2) {
        /* All rels connect var 0→var 1 */
        uint8_t src_vars[16], dst_vars[16];
        if (n_rels > 16) return false;
        for (uint8_t r = 0; r < n_rels; r++) {
            src_vars[r] = 0;
            dst_vars[r] = 1;
        }
        return lftj_build_plan(ctx, rels, n_rels, n_vars, src_vars, dst_vars);
    } else if (n_vars == 4 && n_rels == 6) {
        /* 4-clique: rels[0]=a→b, rels[1]=a→c, rels[2]=a→d,
         *           rels[3]=b→c, rels[4]=b→d, rels[5]=c→d */
        uint8_t src_vars[6] = {0, 0, 0, 1, 1, 2};
        uint8_t dst_vars[6] = {1, 2, 3, 2, 3, 3};
        return lftj_build_plan(ctx, rels, n_rels, n_vars, src_vars, dst_vars);
    }

    /* Fallback: chain pattern — rel[i] connects var i→var i+1 */
    if (n_rels == n_vars - 1) {
        uint8_t src_vars[16], dst_vars[16];
        if (n_rels > 16) return false;
        for (uint8_t r = 0; r < n_rels; r++) {
            src_vars[r] = r;
            dst_vars[r] = r + 1;
        }
        return lftj_build_plan(ctx, rels, n_rels, n_vars, src_vars, dst_vars);
    }

    return false;
}

/* --------------------------------------------------------------------------
 * Recursive backtracking enumeration
 *
 * At each depth d, open iterators for variable d's bindings using the
 * currently bound values, then leapfrog-intersect to find valid bindings.
 * At the last depth, emit tuples to output.
 * -------------------------------------------------------------------------- */

void lftj_enumerate(lftj_enum_ctx_t* ctx, uint8_t depth) {
    if (ctx->oom) return;

    if (depth == ctx->n_vars) {
        /* All variables bound — emit tuple */
        if (ctx->out_count >= ctx->out_cap) {
            if (!lftj_grow_output(ctx)) {
                ctx->oom = true;
                return;
            }
        }
        for (uint8_t v = 0; v < ctx->n_vars; v++)
            ctx->col_data[v][ctx->out_count] = ctx->bound[v];
        ctx->out_count++;
        return;
    }

    lftj_var_plan_t* vp = &ctx->var_plans[depth];

    if (vp->n_bindings == 0) {
        /* Root variable (depth 0 with no bindings): iterate all nodes.
         * Use the first rel's fwd CSR to determine node range. */
        if (depth != 0) return;  /* non-root var must have bindings */

        /* Find max n_nodes across all CSRs in the query */
        int64_t n_nodes = 0;
        for (uint8_t v = 0; v < ctx->n_vars; v++) {
            for (uint8_t b = 0; b < ctx->var_plans[v].n_bindings; b++) {
                if (ctx->var_plans[v].bindings[b].csr) {
                    int64_t nn = ctx->var_plans[v].bindings[b].csr->n_nodes;
                    if (nn > n_nodes) n_nodes = nn;
                }
            }
        }
        if (n_nodes == 0) return;

        for (int64_t a = 0; a < n_nodes; a++) {
            ctx->bound[0] = a;
            lftj_enumerate(ctx, 1);
            if (ctx->oom) return;
        }
        return;
    }

    /* Open iterators for this variable's bindings */
    ray_lftj_iter_t iter_buf[LFTJ_MAX_ITERS_PER_VAR];
    ray_lftj_iter_t* iter_ptrs[LFTJ_MAX_ITERS_PER_VAR];

    for (uint8_t b = 0; b < vp->n_bindings; b++) {
        lftj_binding_t* bind = &vp->bindings[b];
        if (!bind->csr) return;
        int64_t parent = ctx->bound[bind->bound_var];
        if (parent < 0 || parent >= bind->csr->n_nodes) return;
        lftj_open(&iter_buf[b], bind->csr, parent);
        iter_ptrs[b] = &iter_buf[b];
    }

    /* Leapfrog intersect */
    int64_t val;
    while (leapfrog_search(iter_ptrs, vp->n_bindings, &val)) {
        ctx->bound[depth] = val;
        lftj_enumerate(ctx, depth + 1);
        if (ctx->oom) return;
        /* Advance all iterators past current match */
        for (uint8_t b = 0; b < vp->n_bindings; b++)
            lftj_next(iter_ptrs[b]);
    }
}
