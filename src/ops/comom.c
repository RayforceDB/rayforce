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

/* The order-sensitive arithmetic of cov and scov's pivoted sums (#1847,
 * #1897): the pivot's exact move (TwoSum), the sums' re-expression about a
 * moved pivot, the merge, and the finaliser's overflow-safe correction.
 * Their argument holds for the operations in the order written, and two
 * things keep that order, each enough on its own:
 * - the Makefile compiles this file with STRICT_FP_CFLAGS in every profile
 *   (FP_STRICT_src/ops/comom);
 * - the source: gcc honours -fassociative-math only together with
 *   -fno-signed-zeros, which lowers __GCC_IEC_559 below 2, and then the
 *   #error below refuses to build; clang defines no macro for it, and its
 *   pragma below turns reassociation and contraction off for the file.
 * The comoment tests run under the release flags with gcc and with clang
 * in the merge queue (ci.yml) and fail if the order is lost: without both,
 * gcc folded TwoSum away and cov of values close together was wrong in its
 * leading digits; under -fassociative-math clang rewrote sx*(sy/n) into
 * (sx*sy)/n, which overflows where the sums fit.  Asm barriers on the exact
 * steps measured 5% on cov by a 1,000-group key at all cores, where the
 * pivot moves often, and are not needed with either protection in place.
 * The row loops that feed these sums stay in their own files and flags:
 * each row adds one difference x - k, one rounding in any order, and the
 * bound of a sum of n such terms does not depend on the order of the
 * additions. */

#include "ops/ops.h"

#if defined(__FAST_MATH__) || \
    (defined(__GNUC__) && !defined(__clang__) && defined(__GCC_IEC_559) && __GCC_IEC_559 < 2)
#  error "src/ops/comom.c needs strict IEEE flags (STRICT_FP_CFLAGS, Makefile)"
#endif
#if defined(__clang__)
#  pragma clang fp contract(off) reassociate(off)
#endif

/* Sums of n pairs about a pivot re-expressed about that pivot plus (ex, ey):
 *   sxy' = sxy - ex*sy - ey*sx + n*ex*ey    sx' = sx - n*ex    sy' = sy - n*ey */
static void comom_shift(double n, double* sx, double* sy, double* sxy, double ex, double ey) {
    *sxy = *sxy - ex * *sy - ey * *sx + n * ex * ey;
    *sx -= n * ex;
    *sy -= n * ey;
}

/* b - a exactly, as hi + lo: exact in one subtraction when b and a are
 * within a factor of two of each other (Sterbenz) or one is 0, else
 * TwoSum. */
static void comom_diff2(double b, double a, double* hi, double* lo) {
    if (a == 0.0 || b == 0.0 || (a > 0.0 ? (b >= 0.5 * a && b <= 2.0 * a)
                                         : (b <= 0.5 * a && b >= 2.0 * a))) {
        *hi = b - a; *lo = 0.0;
        return;
    }
    double s = b - a;
    double bb = s - b;
    double e1 = b - (s - bb);
    double e2 = -a - bb;
    *hi = s; *lo = e1 + e2;
}

/* An int128 exactly as hi + lo doubles. */
static void comom_i128_2(__int128 d, double* hi, double* lo) {
    double h = (double)d;
    *hi = h;
    *lo = (double)(d - (__int128)h);
}

/* One side's pivot slot moved by about m; its exact move as hi + lo.  An
 * integer pivot moves by m rounded to an integer, kept inside int64 (a move
 * past it stops at the last double-representable step inside). */
static void comom_move_side(bool im, double* k, double m, double* hi, double* lo) {
    *hi = 0.0; *lo = 0.0;
    if (!(__builtin_fabs(m) <= 1.7976931348623157e308)) return;   /* a NaN or an infinity */
    if (im) {
        if (!(__builtin_fabs(m) < 1e30)) return;   /* past any int64 move: from an overflowed sum */
        int64_t kv = ray_comom_pivot_iv(*k), nk;
        double r = __builtin_rint(m);
        if (__builtin_fabs(r) < 0x1p62 && !__builtin_add_overflow(kv, (int64_t)r, &nk)) {
            *k = ray_comom_pivot_i(nk);   /* the common case, in int64 */
            *hi = r;
            return;
        }
        __int128 t = (__int128)kv + (__int128)r;
        if (t > (__int128)INT64_MAX || t < (__int128)INT64_MIN) {
            __int128 lim = t > (__int128)INT64_MAX ? (__int128)INT64_MAX : (__int128)INT64_MIN;
            r = (double)(lim - (__int128)kv);
            while ((__int128)kv + (__int128)r > (__int128)INT64_MAX ||
                   (__int128)kv + (__int128)r < (__int128)INT64_MIN)
                r = __builtin_nextafter(r, 0.0);
            t = (__int128)kv + (__int128)r;
        }
        *k = ray_comom_pivot_i((int64_t)t);
        *hi = r;
        return;
    }
    double nk = *k + m;
    comom_diff2(nk, *k, hi, lo);
    *k = nk;
}

void ray_comom_repivot_cold(unsigned im, double n, double* pivx, double* pivy, double* sx,
                            double* sy, double* sxy) {
    double inv = 1.0 / n;   /* n a power of two: sum/n is exact */
    double mx = *sx * inv, my = *sy * inv;
    double hx, lx, hy, ly;
    comom_move_side((im & RAY_COMOM_IMX) != 0, pivx, mx, &hx, &lx);
    comom_move_side((im & RAY_COMOM_IMY) != 0, pivy, my, &hy, &ly);
    comom_shift(n, sx, sy, sxy, hx, hy);
    if (lx != 0.0 || ly != 0.0) comom_shift(n, sx, sy, sxy, lx, ly);
}

/* The exact distance b - a of two pivot slots of one side, as hi + lo. */
static void comom_kdiff(bool im, double b, double a, double* hi, double* lo) {
    if (im) {
        int64_t d;
        if (!__builtin_sub_overflow(ray_comom_pivot_iv(b), ray_comom_pivot_iv(a), &d) &&
            d > -(INT64_C(1) << 53) && d < (INT64_C(1) << 53)) {
            *hi = (double)d; *lo = 0.0;   /* exact in one double */
            return;
        }
        comom_i128_2((__int128)ray_comom_pivot_iv(b) - (__int128)ray_comom_pivot_iv(a), hi, lo);
    } else
        comom_diff2(b, a, hi, lo);
}

void ray_comom_merge(unsigned im, double nd, double* pivx, double* pivy, double* sx, double* sy,
                     double* sxy, double ns, double kxs, double kys, double sxs, double sys,
                     double sxys) {
    if (ns <= 0) return;
    if (nd <= 0) {
        *pivx = kxs; *pivy = kys; *sx = sxs; *sy = sys; *sxy = sxys;
        return;
    }
    if (ns > nd) {
        /* the source is the larger: its pivot is the one kept */
        double t;
        t = *pivx; *pivx = kxs; kxs = t;   t = *pivy; *pivy = kys; kys = t;
        t = *sx; *sx = sxs; sxs = t;   t = *sy; *sy = sys; sys = t;
        t = *sxy; *sxy = sxys; sxys = t;
        t = nd; nd = ns; ns = t;
    }
    /* the source's sums about its pivot ks re-expressed about kd = ks + (kd - ks) */
    double hx, lx, hy, ly;
    comom_kdiff((im & RAY_COMOM_IMX) != 0, *pivx, kxs, &hx, &lx);
    comom_kdiff((im & RAY_COMOM_IMY) != 0, *pivy, kys, &hy, &ly);
    comom_shift(ns, &sxs, &sys, &sxys, hx, hy);
    if (lx != 0.0 || ly != 0.0) comom_shift(ns, &sxs, &sys, &sxys, lx, ly);
    *sx += sxs; *sy += sys; *sxy += sxys;
}

/* The correction sx*sy/n: formed while the product is finite (the raw
 * formula's own form); where it would pass DBL_MAX, as sx*(sy/n), whose
 * terms stay within the scale of the sums: |sx*(sy/n)| <= sqrt(sxx*syy).
 * Both forms take one rounding. */
double ray_comom_cov(double dn, double sx, double sy, double sxy, double div) {
    double p = sx * sy;
    double c = __builtin_fabs(p) <= 1.7976931348623157e308 ? p / dn : sx * (sy / dn);
    return (sxy - c) / div;
}
