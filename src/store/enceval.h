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
#ifndef RAY_ENCEVAL_H
#define RAY_ENCEVAL_H

#include "store/enccol.h"
#include "ops/ops.h"           /* OP_EQ .. OP_GE */

/* Encoded comparison and membership over a validated vector view
 * (ray_enccol_vview), never expanding the vector.  The semantics are the
 * predicate comparator's: values are logical_T in native order (signed for
 * I16, I32, I64 and the temporal types, unsigned for BOOL, U8 and SYM ids);
 * floats compare numerically with -0.0 == +0.0, a NaN being the null.
 * SYM ids decide == / != / in only: an ordered operator on SYM declines,
 * never ordering ids (#738).  Every buffer is the caller's or on the stack. */

/* Comparison operators: OP_EQ .. OP_GE of src/ops/ops.h. */
/* `c` and the members of `set` are integer constants in the column's
 * logical domain (a SYM constant is an id of the column's domain).  A
 * caller holding a float literal against an integer column never
 * truncates it into c (generation 0 compares those in double): it
 * declines and decodes.  A null constant is the caller's to fold too.
 * An operator outside OP_EQ .. OP_GE declines (false / -1). */
/* (x op c) over a viewed vector into bits[0..n), integer kinds (every
 * encoding P1 writes for integer, temporal, BOOL, U8 and SYM), with null
 * masking: rows whose bitmap bit is set get `null_result`.  False when
 * the view has no evaluator (RAW, DELTA_BP, a float type): decode. */
bool ray_enc_eval_cmp(const ray_enc_vview_t* v, uint16_t op, int64_t c,
                      uint8_t null_result, uint8_t* bits);
/* (x op c) of each run of an RLE view without bitmap nulls into
 * pass[0..n_runs): the run-level answer ray_enc_eval_cmp expands into
 * bits, for a consumer that takes runs (a selection emitted from the
 * runs).  False for any other view, a bitmap, or an operator
 * ray_enc_eval_cmp declines. */
bool ray_enc_eval_cmp_runs(const ray_enc_vview_t* v, uint16_t op, int64_t c, uint8_t* pass);
/* (x in set): set holds nset logical values, sorted ascending, distinct. */
bool ray_enc_eval_in(const ray_enc_vview_t* v, const int64_t* set, int nset,
                     uint8_t null_result, uint8_t* bits);
/* Apply the null mask: bits[i] = null_result where the bitmap bit is set. */
void ray_enc_mask_nulls(const ray_enc_vview_t* v, uint8_t null_result, uint8_t* bits);
/* Whole-vector answer of a CONST view: 1 / 0 when every row has the same
 * result (the value's result equals null_result, or no nulls), else -1. */
int  ray_enc_const_decision(const ray_enc_vview_t* v, uint16_t op, int64_t c, uint8_t null_result);
/* The same for a float CONST view (F32 / F64 cbits) under the predicate
 * comparator (-0.0 == +0.0); -1 for any other view and for a NaN c
 * (generation 0 reads a NaN literal as the null constant). */
int  ray_enc_const_decision_f64(const ray_enc_vview_t* v, uint16_t op, double c, uint8_t null_result);

/* Exact 128-bit sum of the non-null logical values of a fully selected
 * integer vector, and its non-null count.  False when the encoding has no
 * sum rule (DICT, nullable RLE; RAW and DELTA_BP have no view): decode.
 * Integer types summed as ray_encw_type_summable lists them (BOOL, U8,
 * I16, I32, I64, TIME): floats, DATE, TIMESTAMP and SYM decline, and so
 * does a BOOL / U8 view holding nulls (their kernels count every row). */
bool ray_enc_vec_sum(const ray_enc_vview_t* v, int64_t* hi, uint64_t* lo, int64_t* n_nonnull);

/* Rows of a viewed vector without bitmap nulls (SYM: ids, EQ / NE only)
 * that compare true against c: CONST n or 0, RLE the lengths of the
 * passing runs, FOR_BP a count over the packed residuals (fix 6), DICT a
 * count of codes in the interval.  False when the view carries a bitmap
 * with nulls or has no rule (a float view, an operator outside OP_EQ ..
 * OP_GE): read the vector and count it as today.  c is in the column's
 * logical domain, as for ray_enc_eval_cmp. */
bool ray_enc_count_cmp(const ray_enc_vview_t* v, uint16_t op, int64_t c, int64_t* count);
/* The runs of a CONST / RLE view as (logical value, first row, end row)
 * triples through fn, in row order; false for any other view.  A SYM
 * value is its id, a BOOL / U8 one unsigned, the others signed: the value
 * the vector's decode holds at those rows (a bitmap null row reads as the
 * run's stored value: a caller that must see nulls declines a view with
 * a nullmap). */
typedef void (*ray_enc_run_fn)(void* ctx, int64_t value, int32_t lo, int32_t hi);
bool ray_enc_runs(const ray_enc_vview_t* v, void* ctx, ray_enc_run_fn fn);

/* The [min, max] of the run values of a CONST / RLE view (nulls not
 * excluded: a caller declines a view with a bitmap). */
void ray_enc_view_range(const ray_enc_vview_t* v, int64_t* mn, int64_t* mx);

#endif /* RAY_ENCEVAL_H */
