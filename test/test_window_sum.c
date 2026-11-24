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

/* test/test_window_sum.c — F64 msum, mavg and avgs read only their own
 * window (#1548).
 *
 * A window's total was the difference of two running prefix sums, so a
 * huge, infinite or NaN value early in the column reached every later
 * window: a large prefix cancelled the window's low-order bits, and an
 * overflowed one turned every later window null.  These tests compare
 * each window against a naive sum of the window's own values, to within
 * the error bound of summing those values directly.  The inputs put huge
 * values, infinities, NaN payloads and nulls at the edges and inside, the
 * windows cover 1, 2, the morsel size around 1024, the row count and more
 * than the row count, and the large inputs span many morsels and pool
 * tasks so the carries across chunk boundaries are exercised. */
#include "test.h"
#include <rayforce.h>
#include "core/runtime.h"
#include "lang/eval.h"
#include "vec/vec.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void ws_setup(void) {
    ray_runtime_create(0, NULL);
}

static void ws_teardown(void) {
    ray_runtime_destroy(__RUNTIME);
}

static uint64_t ws_rng = 0x9E3779B97F4A7C15ull;

static uint64_t ws_next(void) {
    ws_rng ^= ws_rng << 13;
    ws_rng ^= ws_rng >> 7;
    ws_rng ^= ws_rng << 17;
    return ws_rng;
}

/* uniform in [-1, 1) */
static double ws_unit(void) {
    return (double)(ws_next() >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}

enum { WS_PLAIN, WS_SPIKES, WS_NULLS, WS_INF, WS_NAN, WS_HUGE, WS_NPAT };
static const char* const WS_PAT[] = {
    "plain", "spikes", "nulls", "inf", "nan", "huge"
};

/* An F64 column of n rows in the given pattern, bound to X. */
static ray_t* ws_column(int pat, int64_t n) {
    ray_t* v = ray_vec_new(RAY_F64, n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = n;
    double* d = (double*)ray_data(v);
    for (int64_t i = 0; i < n; i++) {
        d[i] = ws_unit() * 100.0;
        switch (pat) {
            case WS_SPIKES:
                /* Large values that leave a running prefix far above the
                 * windows between them, without overflowing it. */
                if (i == 0 || ws_next() % 300 == 0) d[i] = ws_unit() * 1e300;
                break;
            case WS_INF:
                /* Payload infinities: not a Rayfall value, but a column
                 * can carry one from the C API or an import. */
                if (i == 1 || ws_next() % 500 == 0)
                    d[i] = (ws_next() & 1) ? INFINITY : -INFINITY;
                break;
            case WS_NAN:
                /* A NaN payload with the null flag clear. */
                if (i == 2 || ws_next() % 500 == 0) d[i] = NAN;
                break;
            case WS_HUGE:
                /* Every window total overflows somewhere, while every
                 * mean is finite. */
                d[i] = (0.5 + 0.5 * fabs(ws_unit())) * DBL_MAX * ((ws_next() & 1) ? 1.0 : -1.0);
                if (ws_next() % 3 == 0) d[i] = fabs(d[i]);
                break;
        }
    }
    if (pat == WS_NULLS) {
        for (int64_t i = 0; i < n; i++)
            if (i == 0 || i == n - 1 || ws_next() % 20 == 0)
                ray_vec_set_null(v, i, true);
        /* a run of nulls longer than the small windows */
        for (int64_t i = n / 3; i < n / 3 + 40 && i < n; i++)
            ray_vec_set_null(v, i, true);
    }
    return v;
}

typedef struct {
    double abs;    /* sum of |x| / count, which cannot overflow */
    double mean;   /* sum of x / count, which cannot overflow */
    int64_t count;
    bool nonfinite;  /* a value in the window is inf or NaN */
} ws_ref_t;

static ws_ref_t ws_window(ray_t* v, int64_t lo, int64_t hi) {
    const double* d = (const double*)ray_data(v);
    bool flagged = ray_vec_may_have_nulls(v);
    ws_ref_t r = { 0.0, 0.0, 0, false };
    for (int64_t i = lo; i <= hi; i++) {
        if (flagged && ray_vec_is_null(v, i)) continue;
        r.count++;
        if (!isfinite(d[i])) r.nonfinite = true;
    }
    for (int64_t i = lo; i <= hi; i++) {
        if (flagged && ray_vec_is_null(v, i)) continue;
        r.abs += fabs(d[i]) / (double)(r.count ? r.count : 1);
        r.mean += d[i] / (double)r.count;
    }
    return r;
}

/* Rows checked for a window of w over n rows: all of them for the small
 * windows, else the rows around the morsel, task and block boundaries
 * plus a stride, which keeps the naive reference affordable. */
static bool ws_pick(int64_t r, int64_t n, int64_t w) {
    if (w <= 64 || n <= 4096) return true;
    int64_t m = r % 1024, b = r % w;
    return r < 3 || r > n - 4 || m < 2 || m > 1021 || b < 2 || b > w - 3 ||
           r % 8192 < 2 || r % 61 == 0;
}

static ray_t* ws_eval(const char* fmt, int64_t w) {
    char src[128];
    snprintf(src, sizeof src, fmt, (long long)w);
    return ray_eval_str(src);
}

/* Check msum and mavg of window w over X against the naive reference. */
static test_result_t ws_check(ray_t* x, int pat, int64_t w) {
    int64_t n = x->len;
    ray_t* s = ws_eval("(msum %lld X)", w);
    ray_t* a = ws_eval("(mavg %lld X)", w);
    TEST_ASSERT_FMT(s && !RAY_IS_ERR(s) && s->type == RAY_F64 && s->len == n,
                    "msum %lld failed on %s", (long long)w, WS_PAT[pat]);
    TEST_ASSERT_FMT(a && !RAY_IS_ERR(a) && a->type == RAY_F64 && a->len == n,
                    "mavg %lld failed on %s", (long long)w, WS_PAT[pat]);
    TEST_ASSERT_NULLS_AGREE(s, "msum");
    TEST_ASSERT_NULLS_AGREE(a, "mavg");
    const double* sd = (const double*)ray_data(s);
    const double* ad = (const double*)ray_data(a);
    for (int64_t r = 0; r < n; r++) {
        if (!ws_pick(r, n, w)) continue;
        int64_t lo = r - w + 1 < 0 ? 0 : r - w + 1;
        ws_ref_t ref = ws_window(x, lo, r);
        double m = (double)(r - lo + 2);
        if (ref.nonfinite) {
            TEST_ASSERT_FMT(sd[r] != sd[r] && ad[r] != ad[r],
                            "%s w=%lld row %lld: a window holding inf/NaN is not null",
                            WS_PAT[pat], (long long)w, (long long)r);
            continue;
        }
        /* The mean: within 2m ulps of the window's mean magnitude. */
        if (ref.count == 0) {
            TEST_ASSERT_FMT(ad[r] != ad[r], "%s w=%lld row %lld: empty mavg not null",
                            WS_PAT[pat], (long long)w, (long long)r);
        } else {
            double tol = 2.0 * m * DBL_EPSILON * ref.abs;
            TEST_ASSERT_FMT(ad[r] == ad[r] && fabs(ad[r] - ref.mean) <= tol,
                            "%s mavg w=%lld row %lld: got %.17g, window mean %.17g (tol %g)",
                            WS_PAT[pat], (long long)w, (long long)r, ad[r], ref.mean, tol);
        }
        /* The sum, measured per row so nothing here overflows: within
         * 2m ulps of the window's magnitude.  A total past DBL_MAX is null;
         * one below it is a number, whatever order would overflow on the
         * way there.  The all-null window sums to 0.0. */
        if (ref.count == 0) {
            TEST_ASSERT_FMT(sd[r] == 0.0, "%s w=%lld row %lld: empty msum %.17g",
                            WS_PAT[pat], (long long)w, (long long)r, sd[r]);
            continue;
        }
        double cnt = (double)ref.count;
        double tol = 2.0 * m * DBL_EPSILON * ref.abs;
        double lo_mag = (fabs(ref.mean) - tol) * cnt, hi_mag = (fabs(ref.mean) + tol) * cnt;
        if (lo_mag > DBL_MAX) {
            TEST_ASSERT_FMT(sd[r] != sd[r], "%s msum w=%lld row %lld: %.17g past DBL_MAX not null",
                            WS_PAT[pat], (long long)w, (long long)r, sd[r]);
        } else if (hi_mag < DBL_MAX) {
            TEST_ASSERT_FMT(sd[r] == sd[r] && fabs(sd[r] / cnt - ref.mean) <= tol,
                            "%s msum w=%lld row %lld: got %.17g, window sum %.17g (tol %g)",
                            WS_PAT[pat], (long long)w, (long long)r, sd[r], ref.mean * cnt, tol * cnt);
        }
    }
    ray_release(s);
    ray_release(a);
    PASS();
}

static test_result_t ws_run(int64_t n, const int64_t* windows, int nw) {
    int64_t xsym = ray_sym_intern("X", 1);
    for (int pat = 0; pat < WS_NPAT; pat++) {
        ray_t* x = ws_column(pat, n);
        TEST_ASSERT_NOT_NULL(x);
        TEST_ASSERT_FMT(ray_env_set(xsym, x) == RAY_OK, "bind X");
        for (int k = 0; k < nw; k++) {
            int64_t w = windows[k] > 0 ? windows[k] : n - windows[k];
            if (w < 1) continue;
            test_result_t res = ws_check(x, pat, w);
            if (res.status != TEST_PASS) { ray_release(x); return res; }
        }
        ray_release(x);
    }
    PASS();
}

/* Windows below 1 mean n - w: n, n + 1, n + 5 and n - 1. */
static test_result_t test_window_sum_small(void) {
    static const int64_t W[] = { 1, 2, 3, 5, 0, -1, -5 };
    for (int64_t n = 1; n <= 40; n += 3) {
        test_result_t res = ws_run(n, W, (int)(sizeof W / sizeof W[0]));
        if (res.status != TEST_PASS) return res;
        int64_t wm1[] = { n - 1 };
        res = ws_run(n, wm1, 1);
        if (res.status != TEST_PASS) return res;
    }
    PASS();
}

static test_result_t test_window_sum_morsels(void) {
    static const int64_t W[] = { 1, 2, 7, 1023, 1024, 1025, 0, -3 };
    return ws_run(3 * 1024 + 17, W, (int)(sizeof W / sizeof W[0]));
}

/* 70,001 rows: nine pool tasks of 8,192 rows and 69 morsels, with blocks
 * that start and end inside, at, and across their boundaries. */
static test_result_t test_window_sum_parallel(void) {
    static const int64_t W[] = { 1, 2, 3, 100, 1000, 1024, 1025, 8193, 20000, 0, -1 };
    return ws_run(70001, W, (int)(sizeof W / sizeof W[0]));
}

/* avgs: the running mean against sum/count, and finite after the running
 * sum overflows. */
static test_result_t test_window_sum_avgs(void) {
    int64_t xsym = ray_sym_intern("X", 1);
    static const int64_t N[] = { 1, 5, 1025, 70001 };
    for (size_t t = 0; t < sizeof N / sizeof N[0]; t++) {
        int64_t n = N[t];
        for (int pat = 0; pat < WS_NPAT; pat++) {
            ray_t* x = ws_column(pat, n);
            TEST_ASSERT_NOT_NULL(x);
            TEST_ASSERT_FMT(ray_env_set(xsym, x) == RAY_OK, "bind X");
            ray_t* a = ray_eval_str("(avgs X)");
            TEST_ASSERT_FMT(a && !RAY_IS_ERR(a) && a->len == n, "avgs on %s", WS_PAT[pat]);
            TEST_ASSERT_NULLS_AGREE(a, "avgs");
            const double* d = (const double*)ray_data(x);
            const double* ad = (const double*)ray_data(a);
            bool flagged = ray_vec_may_have_nulls(x);
            /* Running reference: the mean as sum(x / count) over the prefix
             * is O(n^2); the scaled running sum is exact in scaling and
             * cannot overflow. */
            double acc = 0.0, abs_acc = 0.0, scale = ldexp(1.0, -20);
            int64_t count = 0;
            bool nonfinite = false;
            for (int64_t r = 0; r < n; r++) {
                if (!(flagged && ray_vec_is_null(x, r))) {
                    if (!isfinite(d[r])) nonfinite = true;
                    acc += d[r] * scale;
                    abs_acc += fabs(d[r]) * scale;
                    count++;
                }
                if (nonfinite || count == 0) {
                    TEST_ASSERT_FMT(ad[r] != ad[r], "%s n=%lld row %lld: avgs not null",
                                    WS_PAT[pat], (long long)n, (long long)r);
                    continue;
                }
                double mean = acc / (double)count / scale;
                double tol = 2.0 * (double)(count + 1) * DBL_EPSILON * (abs_acc / (double)count / scale);
                TEST_ASSERT_FMT(ad[r] == ad[r] && fabs(ad[r] - mean) <= tol,
                                "%s n=%lld avgs row %lld: got %.17g, running mean %.17g (tol %g)",
                                WS_PAT[pat], (long long)n, (long long)r, ad[r], mean, tol);
            }
            ray_release(a);
            ray_release(x);
        }
    }
    PASS();
}

const test_entry_t window_sum_entries[] = {
    { "window_sum/small",    test_window_sum_small,    ws_setup, ws_teardown },
    { "window_sum/morsels",  test_window_sum_morsels,  ws_setup, ws_teardown },
    { "window_sum/parallel", test_window_sum_parallel, ws_setup, ws_teardown },
    { "window_sum/avgs",     test_window_sum_avgs,     ws_setup, ws_teardown },
    { NULL, NULL, NULL, NULL },
};
