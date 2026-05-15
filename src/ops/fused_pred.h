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

#ifndef RAY_OPS_FUSED_PRED_H
#define RAY_OPS_FUSED_PRED_H

#include "rayforce.h"
#include "ops/internal.h"
#include "ops/glob.h"

#define FP_PRED_MAX_CHILDREN 8

typedef enum {
    FP_EQ = 0,
    FP_NE = 1,
    FP_LT = 2,
    FP_LE = 3,
    FP_GT = 4,
    FP_GE = 5,
    FP_LIKE = 6,
    FP_IN = 7,
} fp_op_t;

/* fold values: when the predicate constant is provably outside the
 * column's representable range, we don't run the per-row compare at
 * all — the result is mathematically all-true or all-false.  Without
 * this fold, casting the constant down to the storage width silently
 * truncates (e.g. `u8_col == 300` becoming `u8_col == 44`). */
typedef enum {
    FP_FOLD_NONE  = 0,
    FP_FOLD_FALSE = 1,
    FP_FOLD_TRUE  = 2,
} fp_fold_t;

typedef struct {
    fp_op_t      op;
    int8_t       col_type;
    uint8_t      col_attrs;
    uint8_t      col_esz;
    uint8_t      fold;        /* fp_fold_t — set when cval is out-of-range */
    const void*  col_base;
    ray_t*       col_obj;
    int64_t      col_len;
    int64_t      cval;
    int          cval_in_dict;
    int64_t      cvals[16];
    uint8_t      n_cvals;
    const char*  pat_str;
    size_t       pat_len;
    ray_glob_compiled_t pat_compiled;
    ray_t*       aux_hdr;
    uint8_t*      like_lut;
    uint32_t     like_lut_count;
    ray_t**      like_sym_strings;
} fp_cmp_t;

typedef struct {
    fp_cmp_t children[FP_PRED_MAX_CHILDREN];
    uint8_t  n_children;
} fp_pred_t;

/* Evaluate a single comparison over rows [start, end), writing 0/1 into
 * bits[0..end-start). */
void fp_eval_cmp(const fp_cmp_t* p, int64_t start, int64_t end, uint8_t* bits);

/* Evaluate the full (possibly ANDed) predicate; AND of children into bits[]. */
void fp_eval_pred(const fp_pred_t* p, int64_t start, int64_t end, uint8_t* bits);

/* Walk an OP_AND tree of leaf comparisons and populate fp_pred_t.
 * Returns 0 on success, -1 if any leaf can't be compiled or fan-in
 * exceeds FP_PRED_MAX_CHILDREN. */
int fp_compile_pred(ray_graph_t* g, ray_op_t* pred_op, ray_t* tbl,
                    fp_pred_t* out);

void fp_pred_cleanup(fp_pred_t* p);

#endif /* RAY_OPS_FUSED_PRED_H */
