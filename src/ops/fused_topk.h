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

#ifndef RAY_OPS_FUSED_TOPK_H
#define RAY_OPS_FUSED_TOPK_H

#include "rayforce.h"

/* Maximum K for the fused top-K path.  Bounded so the per-task and
 * global merge buffers stay on stack: `int64_t global_idx[FPK_MAX_K]`
 * is 64 KiB — large enough for typical OFFSET / LIMIT workloads but
 * small enough not to overflow worker thread stacks (default 8 MiB).
 * Queries with K > FPK_MAX_K fall through to the unfused
 * FILTER + SORT + TAKE path which has no buffer-size constraint. */
#define FPK_MAX_K 8192

/* Predicate-shape probe — true when `where_expr` is one of the shapes
 * fused_topk handles (single comparison or AND of comparisons against
 * literal constants on flat columns).  Same vocabulary as fused_group's
 * supported() — they share fp_pred_t. */
int ray_fused_topk_supported(ray_t* where_expr, ray_t* tbl);

/* Run a fused filter+top-K select.  Caller has already verified shape
 * compatibility.  Returns a materialized result table with the selected
 * output columns, or NULL when the runtime gate fails (caller falls back).
 *
 *   tbl              - source table.
 *   where_expr       - predicate AST.
 *   sort_key_syms[]  - column name syms to sort by (1..16, admission-gated
 *                       by FPK_MAX_KEYS — a permanent comparator-arity
 *                       limit, not a lift-able cap).
 *   sort_descs[]     - per-key direction: 0=asc, 1=desc.
 *   n_sort_keys      - number of sort keys (uint32_t carrier; value stays
 *                       <= FPK_MAX_KEYS via the admission check above).
 *   k                - top-K size.
 *   out_col_syms[]   - source column name syms (one per output col).
 *   out_alias_syms[] - alias under which to publish each output col;
 *                       may be NULL when every alias matches the
 *                       corresponding out_col_syms entry.
 *   n_out            - count of output cols; a SELECT output-column count,
 *                       not one of the cut-3-capped join/pivot/asof key
 *                       counts — carries no fixed-size array here, so it
 *                       is uint32_t (unbounded) ahead of its caller's own
 *                       cap eventually lifting.
 *
 * Bypasses the DAG entirely: the predicate is compiled inline against
 * `tbl`'s columns; per-worker bounded heaps are merged after the parallel
 * scan; rows are gathered from `tbl` by index for the final output.
 * Source-column null bitmaps are propagated to the output so a nullable
 * select column survives a top-K gather (the planner gate previously
 * disallowed this).
 *
 * Returns NULL on shape miss (errors during predicate compile etc.) so
 * the caller can fall back to the unfused FILTER + SORT_TAKE path. */
ray_t* ray_fused_topk_select(ray_t* tbl,
                             ray_t* where_expr,
                             const int64_t* sort_key_syms,
                             const uint8_t* sort_descs,
                             uint32_t n_sort_keys,
                             int64_t k,
                             const int64_t* out_col_syms,
                             const int64_t* out_alias_syms,
                             uint32_t n_out);

#endif /* RAY_OPS_FUSED_TOPK_H */
