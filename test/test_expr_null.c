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

/* test/test_expr_null.c — null-aware fused expression tests */
#include "test.h"
#include <rayforce.h>
#include "core/types.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "mem/heap.h"
#include "table/sym.h"
#include <limits.h>
#include <string.h>
#include <math.h>

/* Build an I64 column with nulls at given indices. */
static ray_t* vec_i64_with_nulls(const int64_t* vals, int64_t n,
                                 const int64_t* null_idx, int64_t n_nulls) {
    ray_t* v = ray_vec_from_raw(RAY_I64, (void*)vals, n);
    for (int64_t i = 0; i < n_nulls; i++)
        ray_vec_set_null(v, null_idx[i], true);
    return v;
}

static ray_t* vec_i32_with_nulls(const int32_t* vals, int64_t n,
                                 const int64_t* null_idx, int64_t n_nulls) {
    ray_t* v = ray_vec_from_raw(RAY_I32, (void*)vals, n);
    for (int64_t i = 0; i < n_nulls; i++)
        ray_vec_set_null(v, null_idx[i], true);
    return v;
}

static ray_t* vec_i16_with_nulls(const int16_t* vals, int64_t n,
                                 const int64_t* null_idx, int64_t n_nulls) {
    ray_t* v = ray_vec_from_raw(RAY_I16, (void*)vals, n);
    for (int64_t i = 0; i < n_nulls; i++)
        ray_vec_set_null(v, null_idx[i], true);
    return v;
}

/* Compare two vectors element-wise: same length, same null positions,
 * same non-null values (F64 compared bitwise-as-null + 1e-12 tolerance).
 * Convention: a=fused, b=fallback (matches the diff_run call site). */
static test_result_t vec_expect_equal(ray_t* a, ray_t* b) {
    TEST_ASSERT(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b), "valid results");
    TEST_ASSERT_FMT(a->type == b->type, "type %d != %d", a->type, b->type);
    TEST_ASSERT_FMT(a->len == b->len, "len %lld != %lld",
                    (long long)a->len, (long long)b->len);
    for (int64_t i = 0; i < a->len; i++) {
        bool na = ray_vec_is_null(a, i), nb = ray_vec_is_null(b, i);
        TEST_ASSERT_FMT(na == nb, "null mismatch at %lld: fused=%d fallback=%d",
                        (long long)i, na, nb);
        if (na) continue;
        if (a->type == RAY_F64) {
            double xa = ((double*)ray_data(a))[i];
            double xb = ((double*)ray_data(b))[i];
            /* Both-NaN is an acceptable match: F64 ops (e.g. DIV by zero)
             * legitimately produce non-null NaN in both fused and fallback
             * paths; the harness contract is fused≡fallback, not absolute
             * correctness. Null-position equivalence is checked above. */
            TEST_ASSERT_FMT(fabs(xa - xb) < 1e-12 || (xa != xa && xb != xb),
                            "f64 mismatch at %lld: fused=%g fallback=%g",
                            (long long)i, xa, xb);
        } else {
            /* Hoist elem size: type equality asserted above, so a->type == b->type. */
            uint8_t esz = ray_elem_size(a->type);
            int64_t xa = 0, xb = 0;
            memcpy(&xa, (char*)ray_data(a) + i * esz, esz);
            memcpy(&xb, (char*)ray_data(b) + i * esz, esz);
            /* Sign-extend narrow values so diagnostic messages are readable
             * (e.g. I32 -1 prints as -1, not 4294967295). */
            if      (esz == 4) { xa = (int32_t)xa; xb = (int32_t)xb; }
            else if (esz == 2) { xa = (int16_t)xa; xb = (int16_t)xb; }
            else if (esz == 1) { xa = (int8_t)xa;  xb = (int8_t)xb;  }
            TEST_ASSERT_FMT(xa == xb, "mismatch at %lld: fused=%lld fallback=%lld",
                            (long long)i, (long long)xa, (long long)xb);
        }
    }
    PASS();
}

/* Sum all bail counters across all bail reasons. */
static uint64_t expr_bails_total(void) {
    uint64_t t = 0;
    for (int i = 0; i < EXPR_BAIL__N; i++) t += ray_expr_bail_counts[i];
    return t;
}

/* Execute builder() twice — fused-eligible and forced-fallback — and
 * compare. expect_fused: assert the fused path actually compiled.
 *
 * Root-compile detection uses BOTH ok and bails snapshots.  Using ok alone
 * is a false-positive: when the root bails, exec.c recursively evaluates
 * children, and an all-non-null child subtree can compile (incrementing ok)
 * even though root fusion never happened.  A genuine root-level compile
 * produces ok_delta >= 1 with bail_delta == 0; a root bail produces
 * bail_delta >= 1 (the root contributes at least one bail count). */
typedef ray_op_t* (*expr_builder_t)(ray_graph_t* g);

static test_result_t diff_run(ray_t* tbl, expr_builder_t builder,
                              bool expect_fused) {
    uint64_t ok_before    = ray_expr_compile_ok;
    uint64_t bails_before = expr_bails_total();

    ray_graph_t* g1 = ray_graph_new(tbl);
    ray_t* fused = ray_execute(g1, builder(g1));

    /* Capture bails AFTER the first execute but BEFORE disabling fusion,
     * so the disabled second run's (uncounted) path doesn't skew the delta. */
    bool compiled = (ray_expr_compile_ok > ok_before) &&
                    (expr_bails_total() == bails_before);

    ray_expr_disable = true;
    ray_graph_t* g2 = ray_graph_new(tbl);
    ray_t* fall = ray_execute(g2, builder(g2));
    ray_expr_disable = false;

    test_result_t res = vec_expect_equal(fused, fall);
    if (res.status == TEST_PASS && expect_fused && !compiled)
        res = (test_result_t){ TEST_FAIL,
            "expected fused compile but expr_compile bailed" };

    ray_release(fused); ray_release(fall);
    ray_graph_free(g1); ray_graph_free(g2);
    return res;
}

static test_result_t test_expr_bail_counter_nulls(void) {
    ray_heap_init();
    (void)ray_sym_init();
    int64_t vals[] = {1, 2, 3, 4, 5, 6, 7, 8};
    int64_t nidx[] = {2, 5};
    ray_t* col = vec_i64_with_nulls(vals, 8, nidx, 2);
    TEST_ASSERT(col->attrs & RAY_ATTR_HAS_NULLS, "attr set");

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);

    /* Task 7: nullable i64 arithmetic now has a null-aware kernel variant.
     * The fused path compiles and executes — no EXPR_BAIL_NULL_SHAPE fires. */
    uint64_t ok_before   = ray_expr_compile_ok;
    uint64_t bail_before = 0;
    for (int i = 0; i < EXPR_BAIL__N; i++) bail_before += ray_expr_bail_counts[i];
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* e = ray_add(g, x, ray_const_i64(g, 1));
    ray_t* r = ray_execute(g, e);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* Fused compile should succeed: ok increments, bails do not. */
    TEST_ASSERT(ray_expr_compile_ok > ok_before,
                "nullable i64+const now compiles in fused path (Task 7)");
    uint64_t bail_after = 0;
    for (int i = 0; i < EXPR_BAIL__N; i++) bail_after += ray_expr_bail_counts[i];
    TEST_ASSERT(bail_after == bail_before,
                "no bail for nullable i64+const (kernel variant live)");

    ray_release(r); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_nullfree_stream_unchanged(void) {
    ray_heap_init(); (void)ray_sym_init();
    int64_t vals[] = {1, 2, 3, 4, 5, 6, 7, 8};
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, 8);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* e = ray_mul(g, ray_add(g, ray_scan(g, "x"),
                                     ray_const_i64(g, 1)),
                          ray_const_i64(g, 2));
    ray_expr_t ex;
    TEST_ASSERT(expr_compile(g, tbl, e, &ex), "null-free compiles");
    for (uint8_t i = 0; i < ex.n_ins; i++)
        TEST_ASSERT(ex.ins[i].null_aware == 0, "no null_aware on null-free");
    for (uint8_t r2 = 0; r2 < ex.n_regs; r2++)
        TEST_ASSERT(!ex.regs[r2].nullable, "no nullable regs on null-free");

    ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static ray_op_t* build_add_const(ray_graph_t* g) {
    return ray_add(g, ray_scan(g, "x"), ray_const_i64(g, 7));
}

static test_result_t test_diff_i64_add_nullable_prelanding(void) {
    ray_heap_init(); (void)ray_sym_init();
    int64_t vals[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int64_t nidx[] = {0, 4, 9};
    ray_t* col = vec_i64_with_nulls(vals, 10, nidx, 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);

    /* Task 7: null-aware i64 arithmetic kernel landed; expect_fused=true. */
    test_result_t r = diff_run(tbl, build_add_const, true);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ---- Step 1: new diff tests for nullable F64 fused execution ---- */

/* (f * 2.0 + 1.0) > 5.0 — arith chain into null-aware f64 compare */
static ray_op_t* build_f64_chain(ray_graph_t* g) {
    ray_op_t* f = ray_scan(g, "f");
    ray_op_t* m = ray_mul(g, f, ray_const_f64(g, 2.0));
    ray_op_t* a = ray_add(g, m, ray_const_f64(g, 1.0));
    return ray_gt(g, a, ray_const_f64(g, 5.0));
}

static test_result_t test_diff_f64_chain_nullable(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 10;
    double raw[10];
    for (int64_t i = 0; i < n; i++) raw[i] = 0.5 + (double)i;
    ray_t* col = ray_vec_from_raw(RAY_F64, raw, n);
    ray_vec_set_null(col, 1, true);
    ray_vec_set_null(col, 6, true);

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), col);
    ray_release(col);

    test_result_t r = diff_run(tbl, build_f64_chain, true);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* x / 2.0 — nullable i64 promoted to f64 via inserted CAST */
static ray_op_t* build_i64_promoted(ray_graph_t* g) {
    return ray_div(g, ray_scan(g, "x"), ray_const_f64(g, 2.0));
}

static test_result_t test_diff_i64_promoted_nullable(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t vals[] = {2, 4, 6, 8, 10, 12, 14, 16, 18, 20};
    int64_t nidx[] = {0, 5, 9};
    ray_t* col = vec_i64_with_nulls(vals, 10, nidx, 3);

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);

    test_result_t r = diff_run(tbl, build_i64_promoted, true);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_f64_chain_parallel(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 100000;
    ray_t* col = ray_vec_new(RAY_F64, n);
    col->len = n;
    double* d = (double*)ray_data(col);
    for (int64_t i = 0; i < n; i++) d[i] = 0.5 + (double)(i % 10);
    /* null every 1000th row */
    for (int64_t i = 0; i < n; i += 1000)
        ray_vec_set_null(col, i, true);

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), col);
    ray_release(col);

    test_result_t r = diff_run(tbl, build_f64_chain, true);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ---- Step 6: null-free promotion case ---- */

/* x + f — forces expr_ensure_type cast from i64 to f64 */
static ray_op_t* build_i64_plus_f64(ray_graph_t* g) {
    return ray_add(g, ray_scan(g, "x"), ray_scan(g, "f"));
}

static test_result_t test_nullfree_promotion_invariance(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t xvals[] = {1, 2, 3, 4, 5, 6, 7, 8};
    double  fvals[] = {1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5};
    ray_t* xcol = ray_vec_from_raw(RAY_I64, xvals, 8);
    ray_t* fcol = ray_vec_from_raw(RAY_F64, fvals, 8);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), fcol);
    ray_release(xcol); ray_release(fcol);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_expr_t ex;
    TEST_ASSERT(expr_compile(g, tbl, build_i64_plus_f64(g), &ex),
                "null-free promotion compiles");
    /* No INPUT is nullable, so the compiler marks no instruction null_aware and
     * no reg nullable.  Single-null float model: an F64 producer that may yield
     * 0Nf from finite inputs (overflow) gets HAS_NULLS via a PRECISE post-scan
     * in expr_eval_full (expr_last_op_produces_f64_null +
     * mark_f64_nonfinite_as_null) — NOT the compile-time nullable flag — so
     * this null-free-promotion compile invariant is preserved unchanged. */
    for (uint8_t i = 0; i < ex.n_ins; i++)
        TEST_ASSERT(ex.ins[i].null_aware == 0,
                    "no null_aware on null-free promotion");
    for (uint8_t r2 = 0; r2 < ex.n_regs; r2++)
        TEST_ASSERT(!ex.regs[r2].nullable,
                    "no nullable regs on null-free promotion");

    ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* (f * 2.0) + 1.0 — pure arithmetic output, stays F64, no comparison.
 * Verifies: fused == fallback for numeric output; AND that the result
 * vec has RAY_ATTR_HAS_NULLS set with nulls exactly at the input null
 * positions {2,7} (numeric-output attr propagation). */
static ray_op_t* build_f64_arith_output(ray_graph_t* g) {
    ray_op_t* f = ray_scan(g, "f");
    ray_op_t* m = ray_mul(g, f, ray_const_f64(g, 2.0));
    return ray_add(g, m, ray_const_f64(g, 1.0));
}

static test_result_t test_diff_f64_arith_output(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 10;
    double raw[10];
    for (int64_t i = 0; i < n; i++) raw[i] = 0.5 + (double)i;
    ray_t* col = ray_vec_from_raw(RAY_F64, raw, n);
    ray_vec_set_null(col, 2, true);
    ray_vec_set_null(col, 7, true);

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), col);
    ray_release(col);

    /* diff_run: fused == fallback */
    test_result_t r = diff_run(tbl, build_f64_arith_output, true);
    if (r.status != TEST_PASS) {
        ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
        return r;
    }

    /* Re-execute normally and check attr propagation + null positions */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_t* res = ray_execute(g, build_f64_arith_output(g));
    TEST_ASSERT(res && !RAY_IS_ERR(res), "execute succeeded");
    TEST_ASSERT(res->attrs & RAY_ATTR_HAS_NULLS, "HAS_NULLS attr set on numeric output");
    TEST_ASSERT(ray_vec_is_null(res, 2), "null at index 2");
    TEST_ASSERT(ray_vec_is_null(res, 7), "null at index 7");
    for (int64_t i = 0; i < n; i++) {
        if (i == 2 || i == 7) continue;
        TEST_ASSERT_FMT(!ray_vec_is_null(res, i),
                        "unexpected null at index %lld", (long long)i);
    }
    ray_release(res); ray_graph_free(g);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ---- Task 6: null-aware i64 comparisons, AND/OR, ISNULL ---- */

/* Fixture: 10-row table with two nullable i64 columns.
 *   x: vals 1..10, nulls at {0,4,9}
 *   y: vals 10..1 (decreasing), nulls at {0,3,8} */
static ray_t* make_task6_table(void) {
    int64_t xv[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int64_t xi[] = {0, 4, 9};
    int64_t yv[] = {10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    int64_t yi[] = {0, 3, 8};
    ray_t* xcol = vec_i64_with_nulls(xv, 10, xi, 3);
    ray_t* ycol = vec_i64_with_nulls(yv, 10, yi, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), ycol);
    ray_release(xcol); ray_release(ycol);
    return tbl;
}

/* x > 5 */
static ray_op_t* build_x_gt_const(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "x"), ray_const_i64(g, 5));
}
/* x == 3 */
static ray_op_t* build_x_eq_const(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "x"), ray_const_i64(g, 3));
}
/* x != 3 */
static ray_op_t* build_x_ne_const(ray_graph_t* g) {
    return ray_ne(g, ray_scan(g, "x"), ray_const_i64(g, 3));
}
/* x <= y */
static ray_op_t* build_x_le_y(ray_graph_t* g) {
    return ray_le(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
/* (x > 1) and (y < 5) */
static ray_op_t* build_and_expr(ray_graph_t* g) {
    return ray_and(g,
        ray_gt(g, ray_scan(g, "x"), ray_const_i64(g, 1)),
        ray_lt(g, ray_scan(g, "y"), ray_const_i64(g, 5)));
}
/* (x > 1) or (y < 5) */
static ray_op_t* build_or_expr(ray_graph_t* g) {
    return ray_or(g,
        ray_gt(g, ray_scan(g, "x"), ray_const_i64(g, 1)),
        ray_lt(g, ray_scan(g, "y"), ray_const_i64(g, 5)));
}
/* isnull(x) */
static ray_op_t* build_isnull_x(ray_graph_t* g) {
    return ray_isnull(g, ray_scan(g, "x"));
}

static test_result_t test_diff_i64_gt_const(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_gt_const, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_eq_const(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_eq_const, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_ne_const(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_ne_const, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_le_y(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_le_y, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_and(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_and_expr, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_or(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_or_expr, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_isnull_x(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_isnull_x, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* isnull over a NULL-FREE i64 column: result must be all-false BOOL vec.
 * Pre-change: ISNULL falls through ot=RAY_I64 in expr_compile so it bails
 * (BOOL output type is wrong → sentinel path is absent) — the fused path
 * would produce garbage if it didn't bail, but the capability choke fires
 * EXPR_BAIL_NULL_SHAPE and the fallback runs instead.  This test pins the
 * correct all-false result regardless of whether fusion fires. */
static test_result_t test_isnull_nonnullable_fused(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t vals[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, 10);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_t* res = ray_execute(g, ray_isnull(g, ray_scan(g, "x")));
    TEST_ASSERT(res && !RAY_IS_ERR(res), "execute succeeded");
    TEST_ASSERT(res->type == RAY_BOOL, "result is BOOL");
    TEST_ASSERT_FMT(res->len == 10, "len %lld", (long long)res->len);
    uint8_t* d = (uint8_t*)ray_data(res);
    for (int64_t i = 0; i < 10; i++)
        TEST_ASSERT_FMT(d[i] == 0, "expected false at %lld, got %d",
                        (long long)i, (int)d[i]);
    ray_release(res); ray_graph_free(g);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ---- Raw nullable i64 AND/OR: both inputs are nullable i64 columns ----
 * Unlike the Task 6 tests (which feed AND/OR from comparison outputs that
 * are non-nullable BOOLs), these feed raw nullable i64 values directly so
 * the null_aware I64 BOOL kernel cells are exercised.
 *
 * Fixture:
 *   x: vals {1,0,3,4,5,6,7,8,9,10,INT64_MIN+1,INT64_MIN+2}  nulls {0,4,9}
 *   y: vals {1,2,0,4,5,6,7,8,9,10,1,0}                     nulls {0,3,8}
 *
 * Interesting rows:
 *   row 0: x null,  y null   → both null
 *   row 2: x=3,     y=0      → y is zero (falsy non-null)
 *   row 3: x=4,     y null   → y null, x truthy
 *   row 4: x null,  y=5      → x null, y truthy
 *   row 8: x=9,     y null   → y null, x truthy
 *   row 9: x null,  y=10     → x null, y truthy
 *   remaining rows: both non-null, varying truthiness
 *   row 10: valid near-NULL_I64 value && 1 -> true
 *   row 11: valid near-NULL_I64 value || 0 -> true
 *
 * Expected fallback semantics (any null → 0):
 *   AND: null-on-either → 0; else (a && b) ? 1 : 0
 *   OR:  null-on-either → 0; else (a || b) ? 1 : 0
 */
static ray_t* make_raw_andor_table(void) {
    int64_t xv[] = {1, 0, 3, 4, 5, 6, 7, 8, 9, 10, INT64_MIN + 1, INT64_MIN + 2};
    int64_t xi[] = {0, 4, 9};
    int64_t yv[] = {1, 2, 0, 4, 5, 6, 7, 8, 9, 10, 1, 0};
    int64_t yi[] = {0, 3, 8};
    ray_t* xcol = vec_i64_with_nulls(xv, 12, xi, 3);
    ray_t* ycol = vec_i64_with_nulls(yv, 12, yi, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), ycol);
    ray_release(xcol); ray_release(ycol);
    return tbl;
}

static ray_op_t* build_and_raw(ray_graph_t* g) {
    return ray_and(g, ray_scan(g, "x"), ray_scan(g, "y"));
}

static ray_op_t* build_or_raw(ray_graph_t* g) {
    return ray_or(g, ray_scan(g, "x"), ray_scan(g, "y"));
}

/* x < 5 — LT with a constant, exercises the null-aware LT comparison cell */
static ray_op_t* build_x_lt_const(ray_graph_t* g) {
    return ray_lt(g, ray_scan(g, "x"), ray_const_i64(g, 5));
}

/* x >= y — GE between two nullable i64 columns */
static ray_op_t* build_x_ge_y(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "x"), ray_scan(g, "y"));
}

static test_result_t test_diff_i64_lt_const(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_lt_const, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_ge_y(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task6_table();
    test_result_t r = diff_run(tbl, build_x_ge_y, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_and_raw(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_raw_andor_table();
    test_result_t r = diff_run(tbl, build_and_raw, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i64_or_raw(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_raw_andor_table();
    test_result_t r = diff_run(tbl, build_or_raw, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Narrow-int AND/OR fallback: the fallback binary_range reads raw column
 * memory, so an I32/I16 null arrives as (double)INT32_MIN / (double)INT16_MIN,
 * NOT the widened NULL_I64 the VM kernel sees.  The truthiness helper must
 * treat every per-type null sentinel as false so fallback ≡ fused for these
 * types (regression guard for the I32/I16 null-truthiness path). */
static ray_t* make_raw_andor_table_i32(void) {
    int32_t xv[] = {1, 0, 3, 4, 5, 6, 7, 8, 9, 10};
    int64_t xi[] = {0, 4, 9};
    int32_t yv[] = {1, 2, 0, 4, 5, 6, 7, 8, 9, 10};
    int64_t yi[] = {0, 3, 8};
    ray_t* xcol = vec_i32_with_nulls(xv, 10, xi, 3);
    ray_t* ycol = vec_i32_with_nulls(yv, 10, yi, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), ycol);
    ray_release(xcol); ray_release(ycol);
    return tbl;
}

static ray_t* make_raw_andor_table_i16(void) {
    int16_t xv[] = {1, 0, 3, 4, 5, 6, 7, 8, 9, 10};
    int64_t xi[] = {0, 4, 9};
    int16_t yv[] = {1, 2, 0, 4, 5, 6, 7, 8, 9, 10};
    int64_t yi[] = {0, 3, 8};
    ray_t* xcol = vec_i16_with_nulls(xv, 10, xi, 3);
    ray_t* ycol = vec_i16_with_nulls(yv, 10, yi, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), ycol);
    ray_release(xcol); ray_release(ycol);
    return tbl;
}

static test_result_t test_diff_i32_and_raw(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_raw_andor_table_i32();
    test_result_t r = diff_run(tbl, build_and_raw, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i32_or_raw(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_raw_andor_table_i32();
    test_result_t r = diff_run(tbl, build_or_raw, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i16_and_raw(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_raw_andor_table_i16();
    test_result_t r = diff_run(tbl, build_and_raw, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_i16_or_raw(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_raw_andor_table_i16();
    test_result_t r = diff_run(tbl, build_or_raw, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ---- Task 7: null-aware i64 arithmetic + f64 min2/max2 ---- */

/* Two-column i64 fixture:
 *   x: {1,2,3,4,5,6,7,8,9,10}  nulls {0,4,9}
 *   y: {0,3,0,7,6,5,4,3,2,1}   nulls {0,3,8}
 *
 * y has zero values at non-null rows 2 and 6 (for div/mod zero-divisor tests).
 * Null rows 0 and 3 have value 0 in raw storage but are null-flagged. */
static ray_t* make_task7_table(void) {
    int64_t xv[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int64_t xi[] = {0, 4, 9};
    int64_t yv[] = {0, 3, 0, 7, 6, 5, 4, 3, 2, 1};
    int64_t yi[] = {0, 3, 8};
    ray_t* xcol = vec_i64_with_nulls(xv, 10, xi, 3);
    ray_t* ycol = vec_i64_with_nulls(yv, 10, yi, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), ycol);
    ray_release(xcol); ray_release(ycol);
    return tbl;
}

static ray_op_t* build_i64_sub(ray_graph_t* g) {
    return ray_sub(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
static ray_op_t* build_i64_mul(ray_graph_t* g) {
    return ray_mul(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
static ray_op_t* build_i64_div(ray_graph_t* g) {
    /* y has zero at rows 2,6 (non-null) → zero-divisor null; y null at 0,3,8 → propagate null */
    return ray_div(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
static ray_op_t* build_i64_mod(ray_graph_t* g) {
    return ray_mod(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
static ray_op_t* build_i64_min2(ray_graph_t* g) {
    return ray_min2(g, ray_scan(g, "x"), ray_scan(g, "y"));
}
static ray_op_t* build_i64_max2(ray_graph_t* g) {
    return ray_max2(g, ray_scan(g, "x"), ray_scan(g, "y"));
}

static test_result_t test_diff_i64_sub(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_sub, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_mul(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_mul, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_div(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_div, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_mod(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_mod, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_min2(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_min2, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_max2(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_max2, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Unary NEG/ABS: include INT64_MIN+1 and large-magnitude values.
 * Fixture:
 *   x: {1, -1, INT64_MIN+1, INT64_MAX, 0, 42, -42, 100, -100, 7}
 *   nulls at {0, 5}
 *
 * INT64_MIN+1 negated = INT64_MAX (no overflow).
 * INT64_MAX negated = INT64_MIN+1 via unsigned wrap (no overflow but result is negative large).
 * Note: INT64_MIN itself would overflow via unsigned wrap → NULL_I64; not placed at a non-null
 * index to keep the fixture simple (overflow sentinel is exercised by mark_i64_overflow_as_null). */
static ray_t* make_neg_abs_table(void) {
    int64_t xv[] = {1, -1, INT64_MIN+1, INT64_MAX, 0, 42, -42, 100, -100, 7};
    int64_t xi[] = {0, 5};
    ray_t* xcol = vec_i64_with_nulls(xv, 10, xi, 2);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    ray_release(xcol);
    return tbl;
}

static ray_op_t* build_i64_neg(ray_graph_t* g) {
    return ray_neg(g, ray_scan(g, "x"));
}
static ray_op_t* build_i64_abs(ray_graph_t* g) {
    return ray_abs(g, ray_scan(g, "x"));
}

static test_result_t test_diff_i64_neg(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_neg_abs_table();
    test_result_t r = diff_run(tbl, build_i64_neg, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_i64_abs(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_neg_abs_table();
    test_result_t r = diff_run(tbl, build_i64_abs, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Deep chain: ((x + 1) * y - 3) % 7
 * Exercises scratch-register sentinel flow through a multi-op chain.
 * Uses make_task7_table (y has zero divisors for % but 7 is the right divisor here — no zero). */
static ray_op_t* build_i64_deep_chain(ray_graph_t* g) {
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* y = ray_scan(g, "y");
    ray_op_t* xp1 = ray_add(g, x, ray_const_i64(g, 1));
    ray_op_t* mul = ray_mul(g, xp1, y);
    ray_op_t* sub = ray_sub(g, mul, ray_const_i64(g, 3));
    return ray_mod(g, sub, ray_const_i64(g, 7));
}

static test_result_t test_diff_i64_deep_chain(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_task7_table();
    test_result_t r = diff_run(tbl, build_i64_deep_chain, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* F64 min2/max2 over two nullable F64 columns (no zero-div shapes).
 * Fixture:
 *   f: {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0}  nulls {1, 6}
 *   g: {10.0, 9.0, 8.0, 7.0, 6.0, 5.0, 4.0, 3.0, 2.0, 1.0}  nulls {2, 7} */
static ray_t* make_f64_minmax_table(void) {
    double fv[] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0};
    double gv[] = {10.0, 9.0, 8.0, 7.0, 6.0, 5.0, 4.0, 3.0, 2.0, 1.0};
    ray_t* fcol = ray_vec_from_raw(RAY_F64, fv, 10);
    ray_vec_set_null(fcol, 1, true);
    ray_vec_set_null(fcol, 6, true);
    ray_t* gcol = ray_vec_from_raw(RAY_F64, gv, 10);
    ray_vec_set_null(gcol, 2, true);
    ray_vec_set_null(gcol, 7, true);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), fcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("g", 1), gcol);
    ray_release(fcol); ray_release(gcol);
    return tbl;
}

static ray_op_t* build_f64_min2(ray_graph_t* g) {
    return ray_min2(g, ray_scan(g, "f"), ray_scan(g, "g"));
}
static ray_op_t* build_f64_max2(ray_graph_t* g) {
    return ray_max2(g, ray_scan(g, "f"), ray_scan(g, "g"));
}

static test_result_t test_diff_f64_min2(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_f64_minmax_table();
    test_result_t r = diff_run(tbl, build_f64_min2, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}
static test_result_t test_diff_f64_max2(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_f64_minmax_table();
    test_result_t r = diff_run(tbl, build_f64_max2, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ---- Task 7 guard: non-parted len-1 scalar-broadcast column bails fused compile ----
 *
 * ray_table_add_col does NOT validate column length — a len-1 column can be
 * appended to a 5-row table.  ray_table_nrows() uses the FIRST column's length,
 * so nrows=5 but b->len=1.  The guard in expr_compile (SCAN branch) detects
 * col->len != nrows for non-parted columns and fires EXPR_BAIL_OTHER, preventing
 * the fused evaluator from overreading b.  The fallback exec_elementwise_binary
 * handles the len-1 column correctly as a scalar broadcast (r_scalar=true path).
 *
 * Table:
 *   x: {10, 20, 30, 40, 50}          — 5 rows, no nulls
 *   b: {7}                            — 1 row (scalar-broadcast, no nulls)
 *
 * expr_compile called directly on x+b: must return false, EXPR_BAIL_OTHER++
 * ray_execute (fused bails → fallback): result == {17, 27, 37, 47, 57}
 * diff_run (expect_fused=false): fused-via-fallback == forced-fallback */
static test_result_t test_scalar_broadcast_col_bails(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* x: 5-row column — drives ray_table_nrows() == 5 */
    int64_t xv[] = {10, 20, 30, 40, 50};
    ray_t* xcol = ray_vec_from_raw(RAY_I64, xv, 5);
    /* b: len-1 column — legitimate scalar-broadcast, but not an atom (type > 0) */
    int64_t bv[] = {7};
    ray_t* bcol = ray_vec_from_raw(RAY_I64, bv, 1);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("b", 1), bcol);
    ray_release(xcol);
    ray_release(bcol);

    /* --- Part 1: expr_compile directly must bail with EXPR_BAIL_OTHER --- */
    ray_graph_t* gc = ray_graph_new(tbl);
    ray_op_t* sx = ray_scan(gc, "x");
    ray_op_t* sb = ray_scan(gc, "b");
    ray_op_t* add = ray_add(gc, sx, sb);

    uint64_t bail_other_before = ray_expr_bail_counts[EXPR_BAIL_OTHER];
    ray_expr_t ex;
    bool compiled = expr_compile(gc, tbl, add, &ex);
    TEST_ASSERT_FALSE(compiled);
    TEST_ASSERT_FMT(ray_expr_bail_counts[EXPR_BAIL_OTHER] > bail_other_before,
                    "EXPR_BAIL_OTHER must increment (was %llu, now %llu)",
                    (unsigned long long)bail_other_before,
                    (unsigned long long)ray_expr_bail_counts[EXPR_BAIL_OTHER]);
    ray_graph_free(gc);

    /* --- Part 2: ray_execute bails fused and falls back to scalar-broadcast path ---
     * The fallback exec_elementwise_binary treats len-1 as r_scalar=true;
     * result should be {17, 27, 37, 47, 57}. */
    ray_graph_t* g1 = ray_graph_new(tbl);
    ray_t* res = ray_execute(g1, ray_add(g1, ray_scan(g1, "x"), ray_scan(g1, "b")));
    TEST_ASSERT(res && !RAY_IS_ERR(res), "ray_execute fallback succeeded");
    TEST_ASSERT_FMT(res->type == RAY_I64, "result type I64, got %d", (int)res->type);
    TEST_ASSERT_FMT(res->len == 5, "result len 5, got %lld", (long long)res->len);
    int64_t* rd = (int64_t*)ray_data(res);
    for (int64_t i = 0; i < 5; i++) {
        int64_t expected = xv[i] + bv[0];
        TEST_ASSERT_FMT(rd[i] == expected,
                        "row %lld: expected %lld got %lld",
                        (long long)i, (long long)expected, (long long)rd[i]);
    }
    ray_release(res);
    ray_graph_free(g1);

    /* --- Part 3: diff_run — fused-via-fallback == forced-fallback (expect_fused=false) --- */
    {
        /* reuse tbl — build fresh graph inside diff_run via builder */
        ray_graph_t* gf = ray_graph_new(tbl);
        ray_t* fused_res = ray_execute(gf, ray_add(gf, ray_scan(gf, "x"), ray_scan(gf, "b")));

        ray_expr_disable = true;
        ray_graph_t* gfall = ray_graph_new(tbl);
        ray_t* fall_res = ray_execute(gfall, ray_add(gfall, ray_scan(gfall, "x"), ray_scan(gfall, "b")));
        ray_expr_disable = false;

        test_result_t cmp = vec_expect_equal(fused_res, fall_res);
        ray_release(fused_res); ray_release(fall_res);
        ray_graph_free(gf); ray_graph_free(gfall);
        if (cmp.status != TEST_PASS) {
            ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
            return cmp;
        }
    }

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- Task 8: null-aware narrow demotion casts (I64→I32, I64→I16, F64→I32) ----
 *
 * Builders:
 *   (cast I32 (x + 1)) — i64 arithmetic then narrow; x values 1-10 (fit i16),
 *                         nulls at {0,4,9}; uses make_cast_narrow_table()
 *   (cast I16 (x + 1)) — identical shape, I16 destination
 *   (cast I32 f)        — direct f64→I32; f values 1.0-10.0, nulls at {1,6};
 *                         uses make_f64_minmax_table()
 *
 * Fallback semantics (Step 2 finding):
 *   exec_elementwise_unary does plain truncation for non-null values, then calls
 *   propagate_nulls which writes the type-correct NULL_I32/NULL_I16 sentinel via
 *   ray_vec_set_null at null positions.  ray_vec_is_null is sentinel-based, so the
 *   fused null_aware kernel must write NULL_I32/NULL_I16 for null input lanes to
 *   match the fallback's output exactly.  F64 null-check: NaN detection (a[j]!=a[j]).
 *   No out-of-range handling in fallback (values are small in this fixture); plain
 *   (int32_t) / (int16_t) truncation is the contract for non-null lanes.
 *
 * Narrow dt (I32/I16) only appears in the fused compile via an explicit OP_CAST node;
 * the ot derivation for non-CAST ops produces only I64/F64/BOOL (confirmed in
 * expr.c:636–647).  So no arithmetic produces narrow scratch registers — only CAST. */

static ray_t* make_cast_narrow_table(void) {
    int64_t xv[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int64_t xi[] = {0, 4, 9};
    ray_t* xcol = vec_i64_with_nulls(xv, 10, xi, 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    ray_release(xcol);
    return tbl;
}

/* (cast I32 (x + 1)) */
static ray_op_t* build_cast_i32(ray_graph_t* g) {
    ray_op_t* x  = ray_scan(g, "x");
    ray_op_t* s  = ray_add(g, x, ray_const_i64(g, 1));
    return ray_cast(g, s, RAY_I32);
}

/* (cast I16 (x + 1)) */
static ray_op_t* build_cast_i16(ray_graph_t* g) {
    ray_op_t* x  = ray_scan(g, "x");
    ray_op_t* s  = ray_add(g, x, ray_const_i64(g, 1));
    return ray_cast(g, s, RAY_I16);
}

/* (cast I32 f) — direct f64→I32 narrow */
static ray_op_t* build_cast_f64_to_i32(ray_graph_t* g) {
    return ray_cast(g, ray_scan(g, "f"), RAY_I32);
}

static test_result_t test_diff_cast_i32(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_cast_narrow_table();
    test_result_t r = diff_run(tbl, build_cast_i32, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_cast_i16(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_cast_narrow_table();
    test_result_t r = diff_run(tbl, build_cast_i16, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_diff_cast_f64_to_i32(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = make_f64_minmax_table();
    test_result_t r = diff_run(tbl, build_cast_f64_to_i32, true);
    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return r;
}

static test_result_t test_cast_f64_narrow_saturates(void) {
    TEST_ASSERT(ray_cast_f64_to_i32_null(NAN) == NULL_I32, "i32 NaN -> null");
    TEST_ASSERT(ray_cast_f64_to_i32_null(1e300) == INT32_MAX, "i32 overflow clamps high");
    TEST_ASSERT(ray_cast_f64_to_i32_null(-1e300) == INT32_MIN + 1, "i32 overflow preserves null sentinel");
    TEST_ASSERT(ray_cast_f64_to_i16_null(NAN) == NULL_I16, "i16 NaN -> null");
    TEST_ASSERT(ray_cast_f64_to_i16_null(1e300) == INT16_MAX, "i16 overflow clamps high");
    TEST_ASSERT(ray_cast_f64_to_i16_null(-1e300) == INT16_MIN + 1, "i16 overflow preserves null sentinel");
    TEST_ASSERT(ray_cast_f64_to_u8_null(NAN) == 0, "u8 NaN -> false/nullish");
    TEST_ASSERT(ray_cast_f64_to_u8_null(1e300) == UINT8_MAX, "u8 overflow clamps high");
    TEST_ASSERT(ray_cast_f64_to_u8_null(-1e300) == 0, "u8 underflow clamps low");
    PASS();
}

/* ---- Task 9: fused agg input — attr propagation gates sentinel-aware aggregation ----
 *
 * Table: k=[1,1,2,2] (group key), x=[10,20,NULL,NULL] (values).
 * Fused expression: x * 2  (MUL bypasses try_affine_sumavg_input which
 * only handles OP_ADD/OP_SUB; try_linear_sumavg_input_i64 only fires for
 * n_keys==0).  So expr_compile fires in group.c for n_keys=1.
 *
 * The critical invariant this test pins:
 *   expr_eval_full MUST set RAY_ATTR_HAS_NULLS on the output vec when the
 *   out register is nullable.  Without this attr, the accumulator's
 *   sentinel-skip mask (da_int_null_mask / sc_int_null_mask) is never set,
 *   NULL_I64 sentinels are summed as raw INT_MIN values, and non-null counts
 *   are wrong — all silently.
 *
 * Part A: direct attr check — compile x*2, eval, assert HAS_NULLS on output.
 *
 * Part B: GROUP BY k, AVG(x*2) — AVG returns NULL for all-null group (nn==0
 *   → NULL_F64; the `nn==0` guard in emit_agg_columns for AVG is the gate).
 *   group k=1: avg((10*2),(20*2)) = avg(20,40) = 30.0
 *   group k=2: all inputs NULL → result IS NULL
 *   This proves the attr is load-bearing: without HAS_NULLS, da_any_nullable
 *   is false, nn_count is never allocated, nn falls back to counts[gi]=2 (>0),
 *   and AVG would return 0.0/2 = 0.0 instead of NULL.
 *
 * Part C: GROUP BY k, AVG(x*2) fused == fallback (cross-check).
 *
 * Group order is hash-bucket order — scan key column to locate groups. */
static test_result_t test_fused_agg_input_allnull_group(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* k=[1,1,2,2], x=[10,20,NULL,NULL] */
    int64_t kv[] = {1, 1, 2, 2};
    int64_t xv[] = {10, 20, 0, 0};
    int64_t xi[] = {2, 3};
    ray_t* kcol = ray_vec_from_raw(RAY_I64, kv, 4);
    ray_t* xcol = vec_i64_with_nulls(xv, 4, xi, 2);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xcol);
    ray_release(kcol);
    ray_release(xcol);

    /* ----- Part A: direct attr check ----- */
    {
        uint64_t ok_before    = ray_expr_compile_ok;
        uint64_t bails_before = expr_bails_total();

        ray_graph_t* gA = ray_graph_new(tbl);
        ray_op_t* xA  = ray_scan(gA, "x");
        ray_op_t* mulA = ray_mul(gA, xA, ray_const_i64(gA, 2));

        ray_expr_t exA;
        bool compiled = expr_compile(gA, tbl, mulA, &exA);
        TEST_ASSERT(compiled, "Part A: x*2 expr_compile must succeed for nullable i64");
        TEST_ASSERT(ray_expr_compile_ok > ok_before,
                    "Part A: compile_ok must increment");
        TEST_ASSERT(expr_bails_total() == bails_before,
                    "Part A: no bails for x*2 nullable");

        /* out register must be marked nullable */
        TEST_ASSERT(exA.regs[exA.out_reg].nullable,
                    "Part A: out register must be nullable");

        ray_t* evec = expr_eval_full(&exA, 4);
        TEST_ASSERT(evec && !RAY_IS_ERR(evec), "Part A: eval succeeded");
        TEST_ASSERT(evec->attrs & RAY_ATTR_HAS_NULLS,
                    "Part A: expr_eval_full output MUST have RAY_ATTR_HAS_NULLS");
        /* Also verify the null positions are correct (rows 2 and 3) */
        TEST_ASSERT(!ray_vec_is_null(evec, 0), "Part A: row 0 non-null");
        TEST_ASSERT(!ray_vec_is_null(evec, 1), "Part A: row 1 non-null");
        TEST_ASSERT( ray_vec_is_null(evec, 2), "Part A: row 2 null");
        TEST_ASSERT( ray_vec_is_null(evec, 3), "Part A: row 3 null");
        /* And non-null values are correct: 10*2=20, 20*2=40 */
        int64_t* ev = (int64_t*)ray_data(evec);
        TEST_ASSERT_FMT(ev[0] == 20, "Part A: row 0 expected 20, got %lld", (long long)ev[0]);
        TEST_ASSERT_FMT(ev[1] == 40, "Part A: row 1 expected 40, got %lld", (long long)ev[1]);

        ray_release(evec);
        ray_graph_free(gA);
    }

    /* ----- Parts B and C: GROUP BY k, AVG(x*2) — fused and fallback -----
     * AVG uses nn_count (gated on HAS_NULLS) as its divisor.  If HAS_NULLS
     * is missing: da_any_nullable=false, nn_count=NULL, nn falls back to
     * counts[gi]=2, AVG = 0/2 = 0.0 (WRONG).
     * If HAS_NULLS is set: nn_count=0 for group 2 → null finalization. */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) ray_expr_disable = true;

        uint64_t ok_before    = ray_expr_compile_ok;
        uint64_t bails_before = expr_bails_total();

        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* k_op    = ray_scan(g, "k");
        ray_op_t* x_op    = ray_scan(g, "x");
        /* x * 2: MUL bypasses try_affine_sumavg_input (ADD/SUB only) */
        ray_op_t* x2      = ray_mul(g, x_op, ray_const_i64(g, 2));
        ray_op_t* keys[1]    = { k_op };
        ray_op_t* agg_ins[1] = { x2 };
        uint16_t  agg_ops[1] = { OP_AVG };
        ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
        ray_t* result = ray_execute(g, grp);

        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
        TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);

        /* Check fused engagement on pass 0 only */
        if (pass == 0) {
            bool fused = (ray_expr_compile_ok > ok_before) &&
                         (expr_bails_total() == bails_before);
            TEST_ASSERT(fused,
                "Part B: fused agg compile must fire (compile_ok advanced, no bails)");
        }

        /* result layout for n_keys=1: col 0 = k (key), col 1 = _0 (avg agg F64) */
        ray_t* key_col = ray_table_get_col_idx(result, 0);
        ray_t* avg_col = ray_table_get_col_idx(result, 1);
        TEST_ASSERT_NOT_NULL(key_col);
        TEST_ASSERT_NOT_NULL(avg_col);

        int64_t* kd = (int64_t*)ray_data(key_col);

        bool found_k1 = false, found_k2 = false;
        for (int64_t r = 0; r < 2; r++) {
            if (kd[r] == 1) {
                found_k1 = true;
                TEST_ASSERT_FMT(!ray_vec_is_null(avg_col, r),
                                "pass %d: group k=1 avg must NOT be null", pass);
                double v = ((double*)ray_data(avg_col))[r];
                TEST_ASSERT_FMT(fabs(v - 30.0) < 1e-9,
                                "pass %d: group k=1 avg(x*2) expected 30.0, got %g",
                                pass, v);
            } else if (kd[r] == 2) {
                found_k2 = true;
                /* Without HAS_NULLS: nn fallback = counts[gi]=2 → avg=0/2=0.0 (wrong).
                 * With HAS_NULLS: nn_count=0 → NULL finalization (correct). */
                TEST_ASSERT_FMT(ray_vec_is_null(avg_col, r),
                                "pass %d: group k=2 avg(x*2) must be NULL "
                                "(HAS_NULLS gates nn_count; without it AVG would return 0.0)",
                                pass);
            }
        }
        TEST_ASSERT_FMT(found_k1, "pass %d: group k=1 not found in result", pass);
        TEST_ASSERT_FMT(found_k2, "pass %d: group k=2 not found in result", pass);

        ray_release(result);
        ray_graph_free(g);

        if (pass == 1) ray_expr_disable = false;
    }

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- Task 10: parted nullable columns through the fused path ---------------
 *
 * Fixture: 3 segments of I64, each 4 rows.
 *   seg0 = [1, 2, 3, 4]          — no nulls
 *   seg1 = [5, 6, 7, 8]          — nulls at local indices {1, 3}
 *   seg2 = [9, 10, 11, 12]       — no nulls
 * Expression: x + 7
 *
 * Part A — diff_run on the parted table.
 *   Streaming mode (exec.c dag_can_stream → true for elementwise ops)
 *   calls exec_node per segment with a flat copy of each segment's table.
 *   Each flat segment's expr_compile sees that segment's attrs:
 *     seg0/seg2: nullable=false, null_aware=false (fast path)
 *     seg1:      nullable=true,  null_aware=true  (sentinel-preserving path)
 *   Fused and fallback both concatenate segment results → must match.
 *   expect_fused=true: the flat-segment expr_compile fires on each segment.
 *
 * Part B — compile-time union: expr_compile on the raw parted table.
 *   When group.c (or similar) calls expr_compile(g, parted_tbl, ...) the
 *   compile sees ALL segments; the SCAN handler unions HAS_NULLS across segs.
 *   Assert regs[out_reg].nullable == true (seg1 poisons the union).
 *   Assert has_parted == true.
 *
 * Part C — expr_eval_full_parted correctness.
 *   eval on the compiled parted expr (has_parted=true) → expr_eval_full
 *   dispatches to expr_eval_full_parted.  Morsel evaluator uses compile-time
 *   nullable (=true union) for every segment; no-null segments just never
 *   match the sentinel check.  Verify output values and null positions:
 *     rows  0-3: {8,9,10,11}  — non-null (seg0)
 *     rows  4-7: {12,NULL,14,NULL}  — nulls at global {5,7} (seg1)
 *     rows 8-11: {16,17,18,19}  — non-null (seg2)
 *   Also assert output vector carries RAY_ATTR_HAS_NULLS. */

/* Build a 3-segment parted I64 column: seg0 all non-null, seg1 with 2 nulls,
 * seg2 all non-null.  Returns owned parted wrapper; caller releases. */
static ray_t* make_parted_i64_col(void) {
    int64_t v0[] = {1, 2, 3, 4};
    int64_t v1[] = {5, 6, 7, 8};
    int64_t v2[] = {9, 10, 11, 12};

    ray_t* s0 = ray_vec_from_raw(RAY_I64, v0, 4);
    ray_t* s1 = ray_vec_from_raw(RAY_I64, v1, 4);
    ray_t* s2 = ray_vec_from_raw(RAY_I64, v2, 4);
    ray_vec_set_null(s1, 1, true); /* local index 1 → global row 5 */
    ray_vec_set_null(s1, 3, true); /* local index 3 → global row 7 */

    /* Parted wrapper: data = array of 3 ray_t* pointers. */
    ray_t* col = ray_alloc(3 * sizeof(ray_t*));
    col->type = (int8_t)(RAY_PARTED_BASE + RAY_I64);
    col->len  = 3;
    col->attrs = 0;
    memset(col->aux, 0, sizeof(col->aux));
    ray_t** ptrs = (ray_t**)ray_data(col);
    ptrs[0] = s0;
    ptrs[1] = s1;
    ptrs[2] = s2;
    return col;
}

/* x + 7 (reuses build_add_const which scans "x") */

static test_result_t test_diff_parted_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = make_parted_i64_col();
    TEST_ASSERT(col && !RAY_IS_ERR(col), "parted col");

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), col);
    ray_release(col);
    TEST_ASSERT(tbl && !RAY_IS_ERR(tbl), "parted tbl");

    /* ----- Part A: streaming diff (per-segment fused compile) ----- */
    test_result_t r = diff_run(tbl, build_add_const, true);
    if (r.status != TEST_PASS) {
        ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
        return r;
    }

    /* ----- Part B: compile-time union check on raw parted table ----- */
    ray_t* xcol = ray_table_get_col(tbl, ray_sym_intern("x", 1));
    TEST_ASSERT(xcol && RAY_IS_PARTED(xcol->type), "col is parted");

    ray_graph_t* gc = ray_graph_new(tbl);
    ray_op_t* root_b = build_add_const(gc);
    ray_expr_t ex;
    bool ok = expr_compile(gc, tbl, root_b, &ex);
    TEST_ASSERT(ok, "expr_compile on parted tbl succeeds");
    TEST_ASSERT(ex.has_parted, "has_parted set");
    TEST_ASSERT(ex.regs[ex.out_reg].nullable,
                "compile-time nullable is true (union across segments)");
    /* The SCAN register for 'x' must also be marked nullable. */
    bool scan_nullable = false;
    for (uint8_t r2 = 0; r2 < ex.n_regs; r2++) {
        if (ex.regs[r2].kind == REG_SCAN && ex.regs[r2].nullable) {
            scan_nullable = true; break;
        }
    }
    TEST_ASSERT(scan_nullable, "SCAN reg nullable = union across segs");

    /* ----- Part C: expr_eval_full_parted correctness ----- */
    int64_t nrows = ray_table_nrows(tbl); /* 12 = 4+4+4 */
    TEST_ASSERT_FMT(nrows == 12, "nrows=%lld", (long long)nrows);

    ray_t* res = expr_eval_full(&ex, nrows);
    TEST_ASSERT(res && !RAY_IS_ERR(res), "eval result valid");
    TEST_ASSERT_FMT(res->len == 12, "result len=%lld", (long long)res->len);
    TEST_ASSERT(res->attrs & RAY_ATTR_HAS_NULLS, "output carries HAS_NULLS");

    /* Expected output: seg0→{8,9,10,11}, seg1→{12,NULL,14,NULL}, seg2→{16,17,18,19} */
    int64_t expect_val[] = {8,9,10,11, 12,0,14,0, 16,17,18,19};
    int      expect_null[] = {0,0,0,0, 0,1,0,1, 0,0,0,0};
    int64_t* d = (int64_t*)ray_data(res);
    for (int64_t i = 0; i < 12; i++) {
        bool is_n = ray_vec_is_null(res, i);
        TEST_ASSERT_FMT(is_n == (bool)expect_null[i],
                        "null at %lld: got=%d want=%d", (long long)i, is_n, expect_null[i]);
        if (!is_n) {
            TEST_ASSERT_FMT(d[i] == expect_val[i],
                            "val at %lld: got=%lld want=%lld",
                            (long long)i, (long long)d[i], (long long)expect_val[i]);
        }
    }

    ray_release(res);
    ray_graph_free(gc);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- F64 AND/OR choke: nullable F64 inputs to AND/OR bail EXPR_BAIL_NULL_SHAPE ----
 *
 * Shape: ray_and(g, scan("f"), scan("g")) where f and g are nullable F64 columns.
 * Graph-build (graph.c): ray_and always emits OP_AND with out_type RAY_BOOL —
 *   inputs are accepted regardless of type; rejection happens at compile time.
 * expr_compile: the BOOL output path sets ins_null_aware=true for F64 AND/OR
 *   (op==OP_AND || OP_OR overrides the F64 type check at line ~702), then the
 *   null-capability choke fires because expr_null_capable returns false for
 *   (dt=BOOL, t1=F64, op=OP_AND) — only I64 operands are wired.
 *   Result: EXPR_BAIL_NULL_SHAPE is counted; fused falls through to fallback.
 *
 * Assertions:
 *   - ray_expr_bail_counts[EXPR_BAIL_NULL_SHAPE] advances after the first execute
 *   - diff_run(tbl, build_f64_and, false): values agree fused-via-fallback == fallback
 *
 * Table: reuses make_f64_minmax_table() which has nullable F64 columns "f" and "g". */
static ray_op_t* build_f64_and(ray_graph_t* g) {
    return ray_and(g, ray_scan(g, "f"), ray_scan(g, "g"));
}

static test_result_t test_diff_f64_andor_chokes(void) {
    ray_heap_init(); (void)ray_sym_init();

    ray_t* tbl = make_f64_minmax_table();

    /* Snapshot EXPR_BAIL_NULL_SHAPE before first execute */
    uint64_t null_shape_before = ray_expr_bail_counts[EXPR_BAIL_NULL_SHAPE];

    /* First execute: fused compile should bail and fall through to fallback */
    ray_graph_t* g1 = ray_graph_new(tbl);
    ray_t* r1 = ray_execute(g1, build_f64_and(g1));
    TEST_ASSERT(r1 && !RAY_IS_ERR(r1), "first execute succeeded via fallback");

    TEST_ASSERT_FMT(ray_expr_bail_counts[EXPR_BAIL_NULL_SHAPE] > null_shape_before,
                    "EXPR_BAIL_NULL_SHAPE must advance (was %llu, now %llu)",
                    (unsigned long long)null_shape_before,
                    (unsigned long long)ray_expr_bail_counts[EXPR_BAIL_NULL_SHAPE]);

    ray_release(r1);
    ray_graph_free(g1);

    /* diff_run: expect_fused=false — shape must bail, but fused-via-fallback == fallback */
    test_result_t res = diff_run(tbl, build_f64_and, false);

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    return res;
}

const test_entry_t expr_null_entries[] = {
    { "expr_null/bail_counter",            test_expr_bail_counter_nulls,          NULL, NULL },
    { "expr_null/nullfree_invariance",     test_nullfree_stream_unchanged,        NULL, NULL },
    { "expr_null/diff_i64_add",            test_diff_i64_add_nullable_prelanding, NULL, NULL },
    { "expr_null/diff_f64_chain",          test_diff_f64_chain_nullable,          NULL, NULL },
    { "expr_null/diff_i64_promoted",       test_diff_i64_promoted_nullable,       NULL, NULL },
    { "expr_null/diff_f64_chain_parallel", test_diff_f64_chain_parallel,          NULL, NULL },
    { "expr_null/nullfree_promotion",      test_nullfree_promotion_invariance,    NULL, NULL },
    { "expr_null/diff_f64_arith_output",   test_diff_f64_arith_output,            NULL, NULL },
    /* Task 6: null-aware i64 comparisons, AND/OR, ISNULL */
    { "expr_null/diff_i64_gt_const",       test_diff_i64_gt_const,                NULL, NULL },
    { "expr_null/diff_i64_eq_const",       test_diff_i64_eq_const,                NULL, NULL },
    { "expr_null/diff_i64_ne_const",       test_diff_i64_ne_const,                NULL, NULL },
    { "expr_null/diff_i64_le_y",           test_diff_i64_le_y,                    NULL, NULL },
    { "expr_null/diff_i64_and",            test_diff_i64_and,                     NULL, NULL },
    { "expr_null/diff_i64_or",             test_diff_i64_or,                      NULL, NULL },
    { "expr_null/diff_isnull_x",           test_diff_isnull_x,                    NULL, NULL },
    { "expr_null/isnull_nonnullable",      test_isnull_nonnullable_fused,          NULL, NULL },
    /* LT/GE: additional null-aware comparison cells */
    { "expr_null/diff_i64_lt_const",       test_diff_i64_lt_const,                NULL, NULL },
    { "expr_null/diff_i64_ge_y",           test_diff_i64_ge_y,                    NULL, NULL },
    /* Raw nullable i64 AND/OR: exercises the null_aware I64 BOOL kernel directly */
    { "expr_null/diff_i64_and_raw",        test_diff_i64_and_raw,                 NULL, NULL },
    { "expr_null/diff_i64_or_raw",         test_diff_i64_or_raw,                  NULL, NULL },
    { "expr_null/diff_i32_and_raw",        test_diff_i32_and_raw,                 NULL, NULL },
    { "expr_null/diff_i32_or_raw",         test_diff_i32_or_raw,                  NULL, NULL },
    { "expr_null/diff_i16_and_raw",        test_diff_i16_and_raw,                 NULL, NULL },
    { "expr_null/diff_i16_or_raw",         test_diff_i16_or_raw,                  NULL, NULL },
    /* Task 7: null-aware i64 arithmetic */
    { "expr_null/diff_i64_sub",            test_diff_i64_sub,                     NULL, NULL },
    { "expr_null/diff_i64_mul",            test_diff_i64_mul,                     NULL, NULL },
    { "expr_null/diff_i64_div",            test_diff_i64_div,                     NULL, NULL },
    { "expr_null/diff_i64_mod",            test_diff_i64_mod,                     NULL, NULL },
    { "expr_null/diff_i64_min2",           test_diff_i64_min2,                    NULL, NULL },
    { "expr_null/diff_i64_max2",           test_diff_i64_max2,                    NULL, NULL },
    { "expr_null/diff_i64_neg",            test_diff_i64_neg,                     NULL, NULL },
    { "expr_null/diff_i64_abs",            test_diff_i64_abs,                     NULL, NULL },
    { "expr_null/diff_i64_deep_chain",     test_diff_i64_deep_chain,              NULL, NULL },
    /* Task 7: null-aware f64 min2/max2 */
    { "expr_null/diff_f64_min2",           test_diff_f64_min2,                    NULL, NULL },
    { "expr_null/diff_f64_max2",           test_diff_f64_max2,                    NULL, NULL },
    /* Task 7 guard: non-parted len-1 scalar-broadcast column bails fused compile */
    { "expr_null/scalar_broadcast_bails",  test_scalar_broadcast_col_bails,       NULL, NULL },
    /* Task 8: null-aware narrow demotion casts */
    { "expr_null/diff_cast_i32",           test_diff_cast_i32,                    NULL, NULL },
    { "expr_null/diff_cast_i16",           test_diff_cast_i16,                    NULL, NULL },
    { "expr_null/diff_cast_f64_to_i32",    test_diff_cast_f64_to_i32,             NULL, NULL },
    { "expr_null/cast_f64_narrow_saturates", test_cast_f64_narrow_saturates,      NULL, NULL },
    /* Task 9: fused agg inputs — all-null group finalizes to NULL */
    { "expr_null/fused_agg_allnull_group", test_fused_agg_input_allnull_group,    NULL, NULL },
    /* Task 10: parted nullable columns through the fused path */
    { "expr_null/diff_parted_nullable",    test_diff_parted_nullable,             NULL, NULL },
    /* F64 AND/OR choke: nullable F64 operands must bail EXPR_BAIL_NULL_SHAPE */
    { "expr_null/diff_f64_andor_chokes",   test_diff_f64_andor_chokes,            NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
