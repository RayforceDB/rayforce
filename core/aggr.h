/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef AGGR_H
#define AGGR_H

#include "rayforce.h"

// ============================================================================
// Aggregation API (called from TYPE_MAPGROUP dispatch)
// ============================================================================

obj_p aggr_sum(obj_p val, obj_p index);
obj_p aggr_first(obj_p val, obj_p index);
obj_p aggr_last(obj_p val, obj_p index);
obj_p aggr_avg(obj_p val, obj_p index);
obj_p aggr_max(obj_p val, obj_p index);
obj_p aggr_min(obj_p val, obj_p index);
obj_p aggr_count(obj_p val, obj_p index);
obj_p aggr_med(obj_p val, obj_p index);
obj_p aggr_dev(obj_p val, obj_p index);
obj_p aggr_collect(obj_p val, obj_p index);
obj_p aggr_row(obj_p val, obj_p index);

// ============================================================================
// Fused aggregation (DuckDB-style single-pass)
// ============================================================================

enum {
    AGGR_ID_SUM = 0, AGGR_ID_COUNT, AGGR_ID_FIRST, AGGR_ID_LAST,
    AGGR_ID_AVG, AGGR_ID_MAX, AGGR_ID_MIN, AGGR_ID_MED, AGGR_ID_DEV
};

typedef struct {
    i8_t func_id;      // AGGR_ID_*
    i64_t col_idx;     // Column index in table (-1 if using col_ptr)
    obj_p col_ptr;     // Pre-evaluated virtual column (NULL = use col_idx)
} fused_plan_t;

// Forward-declare query_ctx_t to avoid circular include
struct query_ctx_t;

// Identify aggregate function by function pointer; returns AGGR_ID_* or -1
i8_t aggr_identify_func(obj_p fn);

// One-pass fused aggregation: populates ctx->ngroups, first_rows, last_rows,
// and writes result vectors into results[0..nplan-1].
nil_t aggr_fused_compute(struct query_ctx_t *ctx, fused_plan_t *plan, i64_t nplan, obj_p *results);

#endif  // AGGR_H
