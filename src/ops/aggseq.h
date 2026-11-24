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

#ifndef RAY_OPS_AGGSEQ_H
#define RAY_OPS_AGGSEQ_H

#include <stdbool.h>
#include <stdint.h>

/* acc times every non-null (non-NaN) value of d[0 .. n), one after another
 * in row order, under strict IEEE arithmetic (aggseq.c).  *nonnull is
 * increased by the number of non-null values multiplied in. */
double ray_aggseq_prod_f64(double acc, const double* d, int64_t n, int64_t* nonnull);
double ray_aggseq_prod_f32(double acc, const float* d, int64_t n, int64_t* nonnull);


/* MIN (is_min) or MAX of the non-null (non-NaN) values of d[0 .. n), or of
 * d[idx[0 .. n)] when idx is given, folded into m0 the way the row-by-row
 * strict compare folds them: the result is the first value in row order
 * that is below (above) every earlier one and m0, bits and zero sign
 * included.  When nonnull is not NULL it is increased by the number of
 * non-null values.  Strict IEEE (aggseq.c): the release profile's
 * -fno-signed-zeros would let the compiler fold away the sign of a zero. */
double ray_aggseq_minmax_f64(double m0, const double* d, const uint16_t* idx, int64_t n,
                             bool is_min, int64_t* nonnull);
double ray_aggseq_minmax_f32(double m0, const float* d, const uint16_t* idx, int64_t n,
                             bool is_min, int64_t* nonnull);

#endif
