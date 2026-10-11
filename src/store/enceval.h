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

#endif /* RAY_ENCEVAL_H */
