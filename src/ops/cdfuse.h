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

#ifndef RAY_OPS_CDFUSE_H
#define RAY_OPS_CDFUSE_H

#include "rayforce.h"

/* Minimum row count for the fused kernel; below it the existing group +
 * count-distinct rewrite is already fast enough and the scatter buffers
 * are not worth their allocation. */
#define CDF_MIN_ROWS 262144

/* Fused grouped count-distinct: for each distinct value of key_col,
 * count the distinct values of val_col over rows [0, nrows) (all rows;
 * selection handling is the caller's job — Task 4 only wires the
 * unfiltered path and declines otherwise).
 * Returns a RAY_TABLE with three I64 columns in this exact order:
 *   k[out_n], u[out_n], _first[out_n]
 * (keys, distinct counts, first source row) in stable first-seen key
 * order, or NULL when the shape is unsupported (caller falls back to the
 * existing rewrite).  key_col/val_col must be flat vectors of
 * I64/I32/I16/SYM with no nulls. */
ray_t* ray_cd_fused(ray_t* key_col, ray_t* val_col, int64_t nrows);

#endif /* RAY_OPS_CDFUSE_H */
