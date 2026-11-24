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

/* Floating-point products in row order, and floating-point MIN / MAX, for
 * the scalar aggregate route.
 *
 * A product's rounding depends on the order of its factors.  The release
 * profile allows the compiler to reorder floating-point arithmetic
 * (-fassociative-math, Makefile), which would vectorize a product loop into
 * lanes and change its bits.  This file is compiled with STRICT_FP_CFLAGS
 * (Makefile: FP_STRICT_src/ops/aggseq), so these loops multiply one row
 * after another in every build profile, exactly as the generic per-row path
 * (group.c, scalar_accum_agg) does.  A NaN is a null: it is skipped. */

#include "ops/aggseq.h"

double ray_aggseq_prod_f64(double acc, const double* d, int64_t n, int64_t* nonnull) {
    int64_t nn = 0;
    for (int64_t i = 0; i < n; i++) {
        double x = d[i];
        if (x == x) { acc *= x; nn++; }
    }
    *nonnull += nn;
    return acc;
}

double ray_aggseq_prod_f32(double acc, const float* d, int64_t n, int64_t* nonnull) {
    int64_t nn = 0;
    for (int64_t i = 0; i < n; i++) {
        double x = (double)d[i];
        if (x == x) { acc *= x; nn++; }
    }
    *nonnull += nn;
    return acc;
}

/* MIN / MAX (#1628).  Without a selection: SC_MM_LANES lanes, each an
 * elementwise strict compare-select (no reassociation, so it vectorizes
 * under these flags too); the lanes combined with the same compare.  A NaN
 * never compares below or above, so it is skipped.  The value found is the
 * row-order fold's: an extreme is order-free except for the sign of a zero,
 * so when the extreme is a zero that did not come from m0, the first zero in
 * row order is taken, as the row-by-row fold keeps it.  With a selection:
 * the row-by-row fold itself.  Compiled with -fsigned-zeros: the zero's
 * sign is the input's, never a folded constant. */
#define SC_MM_LANES 8
#define AGGSEQ_MINMAX(NAME, T)                                                        \
double NAME(double m0, const T* d, const uint16_t* idx, int64_t n, bool is_min,        \
            int64_t* nonnull) {                                                         \
    double m = m0;                                                                      \
    if (idx) {                                                                          \
        int64_t nn = 0;                                                                 \
        if (is_min)                                                                     \
            for (int64_t i = 0; i < n; i++) {                                           \
                double x = (double)d[idx[i]];                                           \
                nn += x == x;                                                           \
                m = x < m ? x : m;                                                      \
            }                                                                           \
        else                                                                            \
            for (int64_t i = 0; i < n; i++) {                                           \
                double x = (double)d[idx[i]];                                           \
                nn += x == x;                                                           \
                m = x > m ? x : m;                                                      \
            }                                                                           \
        if (nonnull) *nonnull += nn;                                                    \
        return m;                                                                       \
    }                                                                                   \
    double l[SC_MM_LANES];                                                              \
    int64_t i = 0;                                                                      \
    for (int j = 0; j < SC_MM_LANES; j++) l[j] = m0;                                    \
    if (is_min) {                                                                       \
        for (; i + SC_MM_LANES <= n; i += SC_MM_LANES)                                  \
            for (int j = 0; j < SC_MM_LANES; j++) {                                     \
                double x = (double)d[i + j];                                            \
                l[j] = x < l[j] ? x : l[j];                                             \
            }                                                                           \
        for (; i < n; i++) { double x = (double)d[i]; l[0] = x < l[0] ? x : l[0]; }    \
        for (int j = 0; j < SC_MM_LANES; j++) m = l[j] < m ? l[j] : m;                  \
    } else {                                                                            \
        for (; i + SC_MM_LANES <= n; i += SC_MM_LANES)                                  \
            for (int j = 0; j < SC_MM_LANES; j++) {                                     \
                double x = (double)d[i + j];                                            \
                l[j] = x > l[j] ? x : l[j];                                             \
            }                                                                           \
        for (; i < n; i++) { double x = (double)d[i]; l[0] = x > l[0] ? x : l[0]; }    \
        for (int j = 0; j < SC_MM_LANES; j++) m = l[j] > m ? l[j] : m;                  \
    }                                                                                   \
    if (m == 0.0 && !(m0 == 0.0))                                                       \
        for (i = 0; i < n; i++)                                                         \
            if ((double)d[i] == 0.0) { m = (double)d[i]; break; }                       \
    if (nonnull) {                                                                      \
        int64_t nn = 0;                                                                 \
        for (i = 0; i < n; i++) { double x = (double)d[i]; nn += x == x; }              \
        *nonnull += nn;                                                                 \
    }                                                                                   \
    return m;                                                                           \
}
AGGSEQ_MINMAX(ray_aggseq_minmax_f64, double)
AGGSEQ_MINMAX(ray_aggseq_minmax_f32, float)
#undef AGGSEQ_MINMAX
#undef SC_MM_LANES
