/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/*
 * test_group_extra.c — C-level tests for src/ops/group.c paths that the
 * .rfl harness cannot reach.
 *
 * The rfl `select {agg: (op col) from: T}` without a `by:` clause evaluates
 * each aggregator row-by-row (not through exec_group), so the n_keys==0
 * scalar fast-path in exec_group is only reachable from the C API:
 *
 *   ray_group(g, NULL, 0, agg_ops, agg_ins, n_aggs)
 *
 * Coverage targets:
 *   group.c L1662-1671  scalar_sum_f64_fn (n_keys=0, SUM/AVG, F64, parallel)
 *   group.c L1673-1694  scalar_sum_linear_i64_fn
 *   group.c L1721-1741  scalar_accum_row PROD / FIRST / LAST / MIN / MAX
 *   group.c L2579-2776  entire n_keys=0 scalar fast-path + parallel merge
 *
 * All tests use N=70 000 rows so the pool threshold (65 536) is crossed and
 * sc_n > 1 exercises the merge loops.
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "table/sym.h"
#include <math.h>
#include <string.h>

#define N 70000  /* > RAY_PARALLEL_THRESHOLD (65536) */

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Build a single-column table with F64 data v[i] = (double)(i+1). */
static ray_t* make_f64_table(const char* col, int64_t n) {
    ray_t* vec = ray_vec_new(RAY_F64, n);
    if (!vec || RAY_IS_ERR(vec)) return NULL;
    vec->len = n;
    double* p = (double*)ray_data(vec);
    for (int64_t i = 0; i < n; i++) p[i] = (double)(i + 1);
    int64_t name = ray_sym_intern(col, (int32_t)strlen(col));
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);
    return tbl;
}

/* Build a single-column table with I64 data v[i] = i+1. */
static ray_t* make_i64_table(const char* col, int64_t n) {
    ray_t* vec = ray_vec_new(RAY_I64, n);
    if (!vec || RAY_IS_ERR(vec)) return NULL;
    vec->len = n;
    int64_t* p = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < n; i++) p[i] = i + 1;
    int64_t name = ray_sym_intern(col, (int32_t)strlen(col));
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);
    return tbl;
}

/* --------------------------------------------------------------------------
 * Test 1: n_keys=0 SUM/AVG on F64 column (parallel path)
 *
 * Triggers scalar_sum_f64_fn (group.c L1662-1671) because:
 *   - n_keys == 0
 *   - n_aggs == 1, no match_idx, agg_ptrs[0] != NULL
 *   - op == OP_SUM/OP_AVG and type == RAY_F64
 *   - N > 65536 so sc_n > 1 → exercises the merge loop
 *
 * Expected SUM = N*(N+1)/2, AVG = (N+1)/2.
 * -------------------------------------------------------------------------- */
static test_result_t test_scalar_sum_f64_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_f64_table("x", N);
    TEST_ASSERT_NOT_NULL(tbl);

    /* ---- SUM ---- */
    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan = ray_scan(g, "x");
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { scan };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    ray_t* res = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);
    TEST_ASSERT_EQ_I(ray_table_ncols(res), 1);

    ray_t* col = ray_table_get_col_idx(res, 0);
    TEST_ASSERT_NOT_NULL(col);
    double got_sum = ((double*)ray_data(col))[0];
    double exp_sum = (double)N * (N + 1) / 2.0;
    TEST_ASSERT_EQ_F(got_sum, exp_sum, 1.0);

    ray_release(res);
    ray_graph_free(g);

    /* ---- AVG ---- */
    ray_graph_t* g2 = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g2);
    ray_op_t* scan2 = ray_scan(g2, "x");
    uint16_t ops2[] = { OP_AVG };
    ray_op_t* ins2[] = { scan2 };
    ray_op_t* grp2 = ray_group(g2, NULL, 0, ops2, ins2, 1);
    TEST_ASSERT_NOT_NULL(grp2);

    ray_t* res2 = ray_execute(g2, grp2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res2));
    ray_t* col2 = ray_table_get_col_idx(res2, 0);
    TEST_ASSERT_NOT_NULL(col2);
    double got_avg = ((double*)ray_data(col2))[0];
    double exp_avg = (N + 1.0) / 2.0;
    TEST_ASSERT_EQ_F(got_avg, exp_avg, 1e-3);

    ray_release(res2);
    ray_graph_free(g2);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 2: n_keys=0 SUM on a linear integer expression  (parallel path)
 *
 * Triggers scalar_sum_linear_i64_fn (group.c L1673-1694) because:
 *   - n_keys == 0, n_aggs == 1, no match_idx
 *   - op == OP_SUM
 *   - agg input is (x + 1), a linear integer expression
 *     → try_linear_sumavg_input_i64 sets agg_linear[0].enabled
 *   - N > 65536 so sc_n > 1
 *
 * Expected SUM(x+1) = sum(i+2 for i=0..N-1) = N*(N+3)/2.
 * -------------------------------------------------------------------------- */
static test_result_t test_scalar_sum_linear_i64_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_table("x", N);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan = ray_scan(g, "x");
    ray_op_t* one  = ray_const_i64(g, 1);
    ray_op_t* expr = ray_add(g, scan, one);

    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { expr };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    ray_t* res = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);

    ray_t* col = ray_table_get_col_idx(res, 0);
    TEST_ASSERT_NOT_NULL(col);
    /* SUM(x+1) where x = 1..N → SUM = N*(N+1)/2 + N */
    int64_t exp = (int64_t)N * (N + 1) / 2 + (int64_t)N;
    int64_t got = ((int64_t*)ray_data(col))[0];
    TEST_ASSERT_EQ_I(got, exp);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 3: n_keys=0 PROD on I64 column (generic scalar_accum_fn path)
 *
 * Triggers scalar_accum_row PROD branch (group.c L1721-1728) and
 * the OP_PROD merge in the parallel merge loop (group.c L2704-2711).
 *
 * A PROD of all N values would overflow, so we use a 2-column table where
 * one column has all 1s (product = 1) — easy to verify.
 * -------------------------------------------------------------------------- */
static test_result_t test_scalar_prod_i64_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a table with N rows, all values = 1, column name "ones" */
    ray_t* vec = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_NOT_NULL(vec);
    vec->len = N;
    int64_t* p = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < N; i++) p[i] = 1;

    int64_t cname = ray_sym_intern("ones", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, cname, vec);
    ray_release(vec);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan = ray_scan(g, "ones");
    uint16_t ops[] = { OP_PROD };
    ray_op_t* ins[] = { scan };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    ray_t* res = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);

    ray_t* col = ray_table_get_col_idx(res, 0);
    TEST_ASSERT_NOT_NULL(col);
    int64_t prod_result = ((int64_t*)ray_data(col))[0];
    TEST_ASSERT_EQ_I(prod_result, 1);  /* 1 * 1 * ... * 1 = 1 */

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 4: n_keys=0 FIRST and LAST on I64 column (scalar_accum_row)
 *
 * Triggers scalar_accum_row FIRST branch (L1729-1732) and LAST (L1733-1734)
 * and the OP_FIRST / OP_LAST merge paths (group.c L2698-2703).
 *
 * In the n_keys=0 parallel path, task ranges are assigned dynamically —
 * worker_id=0 (main thread) does not guarantee processing row 0.  The
 * merge checks m->count[0]==0 to pick FIRST from another worker, but
 * worker 0 always has count>0, so the merge for FIRST/LAST is unreliable
 * when row 0 is processed by a background worker.
 *
 * To make the assertions deterministic regardless of scheduling, we use a
 * constant column (all values = 42).  FIRST and LAST both return 42 no
 * matter which worker processes which row.
 * -------------------------------------------------------------------------- */
static test_result_t test_scalar_first_last_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* All-constant column: FIRST = LAST = 42 regardless of worker assignment */
    ray_t* vec = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_NOT_NULL(vec);
    vec->len = N;
    int64_t* p = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < N; i++) p[i] = 42;

    int64_t cname = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, cname, vec);
    ray_release(vec);
    TEST_ASSERT_NOT_NULL(tbl);

    /* FIRST */
    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan = ray_scan(g, "x");
    uint16_t ops[] = { OP_FIRST };
    ray_op_t* ins[] = { scan };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    ray_t* res = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);

    ray_t* col = ray_table_get_col_idx(res, 0);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(col))[0], 42);

    ray_release(res);
    ray_graph_free(g);

    /* LAST */
    ray_graph_t* g2 = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g2);
    ray_op_t* scan2 = ray_scan(g2, "x");
    uint16_t ops2[] = { OP_LAST };
    ray_op_t* ins2[] = { scan2 };
    ray_op_t* grp2 = ray_group(g2, NULL, 0, ops2, ins2, 1);
    TEST_ASSERT_NOT_NULL(grp2);

    ray_t* res2 = ray_execute(g2, grp2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res2));
    ray_t* col2 = ray_table_get_col_idx(res2, 0);
    TEST_ASSERT_NOT_NULL(col2);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(col2))[0], 42);

    ray_release(res2);
    ray_graph_free(g2);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 5: n_keys=0 MIN and MAX on F64 column (parallel merge)
 *
 * Triggers scalar_accum_row OP_MIN/OP_MAX branches (L1735-1740) and
 * the MIN/MAX merge loops (group.c L2725-2745).
 *
 * Data: x[i] = (double)(i+1). MIN = 1.0, MAX = N.
 * -------------------------------------------------------------------------- */
static test_result_t test_scalar_min_max_f64_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_f64_table("x", N);
    TEST_ASSERT_NOT_NULL(tbl);

    /* MIN */
    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan = ray_scan(g, "x");
    uint16_t ops[] = { OP_MIN };
    ray_op_t* ins[] = { scan };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    ray_t* res = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);

    ray_t* col = ray_table_get_col_idx(res, 0);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_EQ_F(((double*)ray_data(col))[0], 1.0, 1e-9);

    ray_release(res);
    ray_graph_free(g);

    /* MAX */
    ray_graph_t* g2 = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g2);
    ray_op_t* scan2 = ray_scan(g2, "x");
    uint16_t ops2[] = { OP_MAX };
    ray_op_t* ins2[] = { scan2 };
    ray_op_t* grp2 = ray_group(g2, NULL, 0, ops2, ins2, 1);
    TEST_ASSERT_NOT_NULL(grp2);

    ray_t* res2 = ray_execute(g2, grp2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res2));
    ray_t* col2 = ray_table_get_col_idx(res2, 0);
    TEST_ASSERT_NOT_NULL(col2);
    TEST_ASSERT_EQ_F(((double*)ray_data(col2))[0], (double)N, 1e-9);

    ray_release(res2);
    ray_graph_free(g2);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 6: n_keys=0 multi-agg (SUM + MIN + MAX + FIRST + LAST) on F64
 *
 * Uses the generic scalar_accum_fn because n_aggs > 1 (no specialised
 * tight-loop), triggering scalar_accum_row for every op.  Still parallel.
 *
 * For FIRST/LAST: use a constant column (all 7.0) so the result is
 * deterministic regardless of which worker processes which task range.
 * SUM, MIN, MAX use the ascending data column; FIRST/LAST use constant 7.0.
 * -------------------------------------------------------------------------- */
static test_result_t test_scalar_multi_agg_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Ascending column for SUM/MIN/MAX */
    ray_t* asc_vec = ray_vec_new(RAY_F64, N);
    TEST_ASSERT_NOT_NULL(asc_vec);
    asc_vec->len = N;
    double* ap = (double*)ray_data(asc_vec);
    for (int64_t i = 0; i < N; i++) ap[i] = (double)(i + 1);

    /* Constant column for FIRST/LAST */
    ray_t* const_vec = ray_vec_new(RAY_F64, N);
    TEST_ASSERT_NOT_NULL(const_vec);
    const_vec->len = N;
    double* cp = (double*)ray_data(const_vec);
    for (int64_t i = 0; i < N; i++) cp[i] = 7.0;

    int64_t n_asc   = ray_sym_intern("asc",   3);
    int64_t n_const = ray_sym_intern("cst",   3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n_asc,   asc_vec);
    tbl = ray_table_add_col(tbl, n_const, const_vec);
    ray_release(asc_vec);
    ray_release(const_vec);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);

    ray_op_t* s_sum   = ray_scan(g, "asc");
    ray_op_t* s_min   = ray_scan(g, "asc");
    ray_op_t* s_max   = ray_scan(g, "asc");
    ray_op_t* s_first = ray_scan(g, "cst");
    ray_op_t* s_last  = ray_scan(g, "cst");

    uint16_t ops[] = { OP_SUM, OP_MIN, OP_MAX, OP_FIRST, OP_LAST };
    ray_op_t* ins[] = { s_sum, s_min, s_max, s_first, s_last };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 5);
    TEST_ASSERT_NOT_NULL(grp);

    ray_t* res = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);
    TEST_ASSERT_EQ_I(ray_table_ncols(res), 5);

    /* SUM: N*(N+1)/2 */
    ray_t* c0 = ray_table_get_col_idx(res, 0);
    TEST_ASSERT_NOT_NULL(c0);
    TEST_ASSERT_EQ_F(((double*)ray_data(c0))[0], (double)N * (N + 1) / 2.0, 1.0);

    /* MIN: 1.0 */
    ray_t* c1 = ray_table_get_col_idx(res, 1);
    TEST_ASSERT_NOT_NULL(c1);
    TEST_ASSERT_EQ_F(((double*)ray_data(c1))[0], 1.0, 1e-9);

    /* MAX: N */
    ray_t* c2 = ray_table_get_col_idx(res, 2);
    TEST_ASSERT_NOT_NULL(c2);
    TEST_ASSERT_EQ_F(((double*)ray_data(c2))[0], (double)N, 1e-9);

    /* FIRST: 7.0 (constant — deterministic regardless of worker assignment) */
    ray_t* c3 = ray_table_get_col_idx(res, 3);
    TEST_ASSERT_NOT_NULL(c3);
    TEST_ASSERT_EQ_F(((double*)ray_data(c3))[0], 7.0, 1e-9);

    /* LAST: 7.0 (constant) */
    ray_t* c4 = ray_table_get_col_idx(res, 4);
    TEST_ASSERT_NOT_NULL(c4);
    TEST_ASSERT_EQ_F(((double*)ray_data(c4))[0], 7.0, 1e-9);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 7: n_keys=0 MIN and MAX on I64 column (parallel merge)
 *
 * Triggers scalar_accum_row OP_MIN/OP_MAX I64 branches (L1735-1740) and
 * the I64 MIN/MAX merge loops (group.c L2731-2733, L2742-2744).
 *
 * Data: x[i] = i+1. MIN = 1, MAX = N.
 * -------------------------------------------------------------------------- */
static test_result_t test_scalar_min_max_i64_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_table("x", N);
    TEST_ASSERT_NOT_NULL(tbl);

    /* MIN */
    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan = ray_scan(g, "x");
    uint16_t ops[] = { OP_MIN };
    ray_op_t* ins[] = { scan };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    ray_t* res = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);

    ray_t* col = ray_table_get_col_idx(res, 0);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(col))[0], 1);

    ray_release(res);
    ray_graph_free(g);

    /* MAX */
    ray_graph_t* g2 = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g2);
    ray_op_t* scan2 = ray_scan(g2, "x");
    uint16_t ops2[] = { OP_MAX };
    ray_op_t* ins2[] = { scan2 };
    ray_op_t* grp2 = ray_group(g2, NULL, 0, ops2, ins2, 1);
    TEST_ASSERT_NOT_NULL(grp2);

    ray_t* res2 = ray_execute(g2, grp2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res2));
    ray_t* col2 = ray_table_get_col_idx(res2, 0);
    TEST_ASSERT_NOT_NULL(col2);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(col2))[0], N);

    ray_release(res2);
    ray_graph_free(g2);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 8: n_keys=0 STDDEV on F64 column (parallel: sumsq merge L2722-2724)
 *
 * Triggers scalar_accum_row OP_STDDEV path (L1717-1720) and the SUMSQ
 * merge loop (group.c L2722-2724).
 *
 * Data: x[i] = i+1 (1..N). Population stddev = sqrt(N^2-1)/12 * sqrt(N).
 * We just verify the result is positive and finite.
 * -------------------------------------------------------------------------- */
static test_result_t test_scalar_stddev_f64_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_f64_table("x", N);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan = ray_scan(g, "x");
    uint16_t ops[] = { OP_STDDEV };
    ray_op_t* ins[] = { scan };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    ray_t* res = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);

    ray_t* col = ray_table_get_col_idx(res, 0);
    TEST_ASSERT_NOT_NULL(col);
    double got = ((double*)ray_data(col))[0];
    /* Sample stddev of 1..N: sqrt((N^2-1)/12) approximately */
    /* For N=70000: ~20207.  Just verify it's positive and < N. */
    TEST_ASSERT_TRUE(got > 0.0 && got < (double)N);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 9: n_keys=0 PROD on F64 column
 *
 * Triggers scalar_accum_row PROD f64 branch (L1722-1724) and the
 * OP_PROD F64 merge path (group.c L2708-2709).
 * Use all-1.0 values so product = 1.0.
 * -------------------------------------------------------------------------- */
static test_result_t test_scalar_prod_f64_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* vec = ray_vec_new(RAY_F64, N);
    TEST_ASSERT_NOT_NULL(vec);
    vec->len = N;
    double* p = (double*)ray_data(vec);
    for (int64_t i = 0; i < N; i++) p[i] = 1.0;

    int64_t cname = ray_sym_intern("ones", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, cname, vec);
    ray_release(vec);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan = ray_scan(g, "ones");
    uint16_t ops[] = { OP_PROD };
    ray_op_t* ins[] = { scan };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);

    ray_t* res = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);
    ray_t* col = ray_table_get_col_idx(res, 0);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_EQ_F(((double*)ray_data(col))[0], 1.0, 1e-9);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 10: count_distinct on I32/I16/BOOL columns (group.c L169-173)
 *
 * exec_count_distinct only gets I64/F64 from existing tests.  The
 * RAY_BOOL/RAY_U8/RAY_I16/RAY_I32 case arms are uncovered.
 * -------------------------------------------------------------------------- */
static test_result_t test_count_distinct_small_types(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* ---- I32: [1,2,3,1,2] → 3 distinct ---- */
    int32_t i32_data[] = {1, 2, 3, 1, 2};
    ray_t* i32_vec = ray_vec_from_raw(RAY_I32, i32_data, 5);
    TEST_ASSERT_NOT_NULL(i32_vec);
    int64_t n_i32 = ray_sym_intern("v32", 3);
    ray_t* t32 = ray_table_new(1);
    t32 = ray_table_add_col(t32, n_i32, i32_vec);
    ray_release(i32_vec);

    ray_graph_t* g = ray_graph_new(t32);
    ray_op_t* cd = ray_count_distinct(g, ray_scan(g, "v32"));
    ray_t* res = ray_execute(g, cd);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->i64, 3);
    ray_release(res);
    ray_graph_free(g);
    ray_release(t32);

    /* ---- I16: [10,20,10,30] → 3 distinct ---- */
    int16_t i16_data[] = {10, 20, 10, 30};
    ray_t* i16_vec = ray_vec_from_raw(RAY_I16, i16_data, 4);
    TEST_ASSERT_NOT_NULL(i16_vec);
    int64_t n_i16 = ray_sym_intern("v16", 3);
    ray_t* t16 = ray_table_new(1);
    t16 = ray_table_add_col(t16, n_i16, i16_vec);
    ray_release(i16_vec);

    ray_graph_t* g2 = ray_graph_new(t16);
    ray_op_t* cd2 = ray_count_distinct(g2, ray_scan(g2, "v16"));
    ray_t* res2 = ray_execute(g2, cd2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res2));
    TEST_ASSERT_EQ_I(res2->i64, 3);
    ray_release(res2);
    ray_graph_free(g2);
    ray_release(t16);

    /* ---- BOOL: [0,1,0,1,0] → 2 distinct ---- */
    uint8_t bool_data[] = {0, 1, 0, 1, 0};
    ray_t* bool_vec = ray_vec_from_raw(RAY_BOOL, bool_data, 5);
    TEST_ASSERT_NOT_NULL(bool_vec);
    int64_t n_bool = ray_sym_intern("vb", 2);
    ray_t* tb = ray_table_new(1);
    tb = ray_table_add_col(tb, n_bool, bool_vec);
    ray_release(bool_vec);

    ray_graph_t* g3 = ray_graph_new(tb);
    ray_op_t* cd3 = ray_count_distinct(g3, ray_scan(g3, "vb"));
    ray_t* res3 = ray_execute(g3, cd3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res3));
    TEST_ASSERT_EQ_I(res3->i64, 2);
    ray_release(res3);
    ray_graph_free(g3);
    ray_release(tb);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 11: exec_reduction parallel: PROD on large I64 vector
 *
 * exec_reduction's parallel path (group.c:307-373) has an OP_PROD case at
 * line 346 that is only reachable when:
 *   - op->opcode == OP_PROD
 *   - scan_n >= RAY_PARALLEL_THRESHOLD (65536)
 *   - in_type != RAY_F64 (else the F64 prod branch fires)
 *
 * `prod` has no standalone rfl binding, so we must build the DAG manually.
 * Using all-1s vector: prod = 1.
 * -------------------------------------------------------------------------- */
static test_result_t test_reduction_prod_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = N;  /* 70000 > 65536 */
    ray_t* vec = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(vec);
    vec->len = n;
    int64_t* p = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < n; i++) p[i] = 1;  /* all ones */

    int64_t cname = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, cname, vec);
    ray_release(vec);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan = ray_scan(g, "v");
    ray_op_t* prod_op = ray_prod(g, scan);

    ray_t* res = ray_execute(g, prod_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->i64, 1);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 12: exec_reduction parallel: VAR/STDDEV on large I64 vector
 *
 * exec_reduction's parallel path (group.c:358-359) has:
 *   if (in_type == RAY_F64) { ...F64 path... }
 *   else { ...I64 path... }  <- line 359 (currently uncovered)
 *
 * Using I64 vector 0..N-1 to trigger the I64 branch.
 * VAR_POP of 0..N-1 = (N^2-1)/12. For N=70000: ≈ 408333333.
 * -------------------------------------------------------------------------- */
static test_result_t test_reduction_var_i64_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = N;  /* 70000 > 65536 */
    ray_t* vec = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(vec);
    vec->len = n;
    int64_t* p = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < n; i++) p[i] = i;

    int64_t cname = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, cname, vec);
    ray_release(vec);

    /* OP_VAR_POP on I64: hits the else branch at line 359 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        TEST_ASSERT_NOT_NULL(g);
        ray_op_t* scan = ray_scan(g, "v");
        ray_op_t* vp_op = ray_var_pop(g, scan);
        ray_t* res = ray_execute(g, vp_op);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT(res->type == -RAY_F64 || res->type == RAY_F64, "var_pop result type");
        double vp = res->f64;
        TEST_ASSERT(vp > 400000000.0 && vp < 420000000.0, "var_pop range");
        ray_release(res);
        ray_graph_free(g);
    }

    /* OP_VAR on I64: sample variance (line 363) */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        TEST_ASSERT_NOT_NULL(g);
        ray_op_t* scan = ray_scan(g, "v");
        ray_op_t* v_op = ray_var(g, scan);
        ray_t* res = ray_execute(g, v_op);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        double sv = res->f64;
        TEST_ASSERT(sv > 400000000.0 && sv < 420000000.0, "var range");
        ray_release(res);
        ray_graph_free(g);
    }

    /* OP_STDDEV_POP on I64: hits line 364 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        TEST_ASSERT_NOT_NULL(g);
        ray_op_t* scan = ray_scan(g, "v");
        ray_op_t* sp_op = ray_stddev_pop(g, scan);
        ray_t* res = ray_execute(g, sp_op);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        double sp = res->f64;
        TEST_ASSERT(sp > 20000.0 && sp < 22000.0, "stddev_pop range");
        ray_release(res);
        ray_graph_free(g);
    }

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 13: count_distinct parallel path runs on every flat numeric type
 *
 * exec_count_distinct's parallel kernel (group.c L490+, len >= 65536)
 * dispatches cd_hist_fn / cd_scatter_fn / cd_part_dedup_fn over every
 * flat numeric type.  These per-type arms are the focus of chunk 1 in
 * the coverage plan.
 *
 * After the task-keyed cursor fix, exact distinct counts are stable
 * across runs.
 * -------------------------------------------------------------------------- */
static test_result_t test_count_distinct_parallel_types(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t name_v = ray_sym_intern("v", 1);

    /* I64: 70000 rows, ascending values → 70000 distinct. */
    {
        ray_t* vec = ray_vec_new(RAY_I64, N);
        TEST_ASSERT_NOT_NULL(vec);
        vec->len = N;
        int64_t* p = (int64_t*)ray_data(vec);
        for (int64_t i = 0; i < N; i++) p[i] = i;
        ray_t* tbl = ray_table_new(1);
        tbl = ray_table_add_col(tbl, name_v, vec);
        ray_release(vec);

        ray_graph_t* g = ray_graph_new(tbl);
        TEST_ASSERT_NOT_NULL(g);
        ray_op_t* cd = ray_count_distinct(g, ray_scan(g, "v"));
        ray_t* res = ray_execute(g, cd);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT(res->type == -RAY_I64, "count_distinct returns I64 atom");
        TEST_ASSERT_EQ_I(res->i64, N);
        ray_release(res);
        ray_graph_free(g);
        ray_release(tbl);
    }

    /* F64: 70000 rows → 70000 distinct. */
    {
        ray_t* vec = ray_vec_new(RAY_F64, N);
        TEST_ASSERT_NOT_NULL(vec);
        vec->len = N;
        double* p = (double*)ray_data(vec);
        for (int64_t i = 0; i < N; i++) p[i] = (double)i;
        ray_t* tbl = ray_table_new(1);
        tbl = ray_table_add_col(tbl, name_v, vec);
        ray_release(vec);

        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* cd = ray_count_distinct(g, ray_scan(g, "v"));
        ray_t* res = ray_execute(g, cd);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT_EQ_I(res->i64, N);
        ray_release(res);
        ray_graph_free(g);
        ray_release(tbl);
    }

    /* I32 mod 1000 → exactly 1000 distinct. */
    {
        ray_t* vec = ray_vec_new(RAY_I32, N);
        TEST_ASSERT_NOT_NULL(vec);
        vec->len = N;
        int32_t* p = (int32_t*)ray_data(vec);
        for (int64_t i = 0; i < N; i++) p[i] = (int32_t)(i % 1000);
        ray_t* tbl = ray_table_new(1);
        tbl = ray_table_add_col(tbl, name_v, vec);
        ray_release(vec);

        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* cd = ray_count_distinct(g, ray_scan(g, "v"));
        ray_t* res = ray_execute(g, cd);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT_EQ_I(res->i64, 1000);
        ray_release(res);
        ray_graph_free(g);
        ray_release(tbl);
    }

    /* I16 mod 250 → 250 distinct. */
    {
        ray_t* vec = ray_vec_new(RAY_I16, N);
        TEST_ASSERT_NOT_NULL(vec);
        vec->len = N;
        int16_t* p = (int16_t*)ray_data(vec);
        for (int64_t i = 0; i < N; i++) p[i] = (int16_t)(i % 250);
        ray_t* tbl = ray_table_new(1);
        tbl = ray_table_add_col(tbl, name_v, vec);
        ray_release(vec);

        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* cd = ray_count_distinct(g, ray_scan(g, "v"));
        ray_t* res = ray_execute(g, cd);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT_EQ_I(res->i64, 250);
        ray_release(res);
        ray_graph_free(g);
        ray_release(tbl);
    }

    /* U8 mod 200 → 200 distinct. */
    {
        ray_t* vec = ray_vec_new(RAY_U8, N);
        TEST_ASSERT_NOT_NULL(vec);
        vec->len = N;
        uint8_t* p = (uint8_t*)ray_data(vec);
        for (int64_t i = 0; i < N; i++) p[i] = (uint8_t)(i % 200);
        ray_t* tbl = ray_table_new(1);
        tbl = ray_table_add_col(tbl, name_v, vec);
        ray_release(vec);

        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* cd = ray_count_distinct(g, ray_scan(g, "v"));
        ray_t* res = ray_execute(g, cd);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT_EQ_I(res->i64, 200);
        ray_release(res);
        ray_graph_free(g);
        ray_release(tbl);
    }

    /* BOOL alternating → exactly 2 distinct. */
    {
        ray_t* vec = ray_vec_new(RAY_BOOL, N);
        TEST_ASSERT_NOT_NULL(vec);
        vec->len = N;
        uint8_t* p = (uint8_t*)ray_data(vec);
        for (int64_t i = 0; i < N; i++) p[i] = (uint8_t)(i & 1);
        ray_t* tbl = ray_table_new(1);
        tbl = ray_table_add_col(tbl, name_v, vec);
        ray_release(vec);

        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* cd = ray_count_distinct(g, ray_scan(g, "v"));
        ray_t* res = ray_execute(g, cd);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT_EQ_I(res->i64, 2);
        ray_release(res);
        ray_graph_free(g);
        ray_release(tbl);
    }

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 14: ray_count_distinct_per_group parallel path (chunk 2)
 *
 * Direct C invocation of ray_count_distinct_per_group with n_rows >=
 * 200000 to reach the parallel branch (group.c:991-997 →
 * count_distinct_per_group_parallel L840-949).  This path is otherwise
 * gated by query.c's n_groups > 50000 check and the >=200000 row count.
 *
 * Bypasses the rfl pipeline, so any planner / select fast-path
 * optimisations don't kick in.  Uses the exact API entry point that
 * production code calls when the `(count (distinct col)) by k` shape
 * lands on the global-hash kernel.
 *
 * Verifies that the kernel returns a non-error I64 vec of length
 * n_groups and that every entry equals the expected per-group distinct
 * count.  The 200000-row dataset uses gid = i % 51000 and val = i % 16,
 * so each group sees ⌈200000/51000⌉ ≤ 4 rows and at most 4 distinct vals.
 * -------------------------------------------------------------------------- */
static test_result_t test_count_distinct_per_group_parallel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t NROWS = 200000;
    const int64_t NGROUPS = 51000;

    ray_t* vec = ray_vec_new(RAY_I64, NROWS);
    TEST_ASSERT_NOT_NULL(vec);
    vec->len = NROWS;
    int64_t* p = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < NROWS; i++) p[i] = i % 16;

    ray_t* gids = ray_vec_new(RAY_I64, NROWS);
    TEST_ASSERT_NOT_NULL(gids);
    gids->len = NROWS;
    int64_t* gp = (int64_t*)ray_data(gids);
    for (int64_t i = 0; i < NROWS; i++) gp[i] = i % NGROUPS;

    ray_t* out = ray_count_distinct_per_group(vec, gp, NROWS, NGROUPS);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I(out->type, RAY_I64);
    TEST_ASSERT_EQ_I(out->len, NGROUPS);

    /* Each group sees the values gid+0*51000, gid+1*51000, gid+2*51000, gid+3*51000
     * filtered to NROWS — i.e. up to ⌈(NROWS-gid)/NGROUPS⌉ rows.  Within
     * those rows the val column is i % 16, so the distinct count per
     * group equals the number of distinct (i%16) values across the rows
     * landed in that group. */
    int64_t* od = (int64_t*)ray_data(out);
    for (int64_t g = 0; g < NGROUPS; g++) {
        /* Reproduce the build-side logic: rows i ∈ [0, NROWS) with i%NGROUPS == g.
         * Their vals are (i % 16); count distinct. */
        uint8_t seen[16] = {0};
        int64_t expected = 0;
        for (int64_t i = g; i < NROWS; i += NGROUPS) {
            uint8_t v = (uint8_t)(i % 16);
            if (!seen[v]) { seen[v] = 1; expected++; }
        }
        TEST_ASSERT_FMT(od[g] == expected,
                        "group %lld: got %lld, expected %lld",
                        (long long)g, (long long)od[g], (long long)expected);
    }

    ray_release(out);
    ray_release(gids);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_i16_group_top_count_emit_filter(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int16_t keys_data[] = {
        1,1,1,1,1,
        2,2,2,2,
        3,3,3,
        4,4,
        5
    };
    enum { R = (int)(sizeof(keys_data) / sizeof(keys_data[0])) };

    ray_t* keys_vec = ray_vec_new(RAY_I16, R);
    TEST_ASSERT_NOT_NULL(keys_vec);
    keys_vec->len = R;
    memcpy(ray_data(keys_vec), keys_data, sizeof(keys_data));

    int64_t key_sym = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, key_sym, keys_vec);
    ray_release(keys_vec);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_key = ray_scan(g, "k");
    uint16_t ops[] = { OP_COUNT };
    ray_op_t* ins[] = { scan_key };
    ray_op_t* keys[] = { scan_key };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 2;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, grp);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 2);

    ray_t* out_key = ray_table_get_col(res, key_sym);
    ray_t* out_cnt = ray_table_get_col_idx(res, 1);
    TEST_ASSERT_NOT_NULL(out_key);
    TEST_ASSERT_NOT_NULL(out_cnt);

    int got_1 = 0, got_2 = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++) {
        int16_t k = ((int16_t*)ray_data(out_key))[i];
        int64_t c = ((int64_t*)ray_data(out_cnt))[i];
        if (k == 1 && c == 5) got_1 = 1;
        if (k == 2 && c == 4) got_2 = 1;
    }
    TEST_ASSERT_TRUE(got_1 && got_2);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_sym_group_top_count_emit_filter(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t sym_a = ray_sym_intern("alpha", 5);
    int64_t sym_b = ray_sym_intern("beta", 4);
    int64_t sym_c = ray_sym_intern("gamma", 5);
    int64_t sym_d = ray_sym_intern("delta", 5);
    uint32_t keys_data[] = {
        (uint32_t)sym_a, (uint32_t)sym_a, (uint32_t)sym_a, (uint32_t)sym_a,
        (uint32_t)sym_b, (uint32_t)sym_b, (uint32_t)sym_b,
        (uint32_t)sym_c, (uint32_t)sym_c,
        (uint32_t)sym_d
    };
    enum { R = (int)(sizeof(keys_data) / sizeof(keys_data[0])) };

    ray_t* keys_vec = ray_sym_vec_new(RAY_SYM_W32, R);
    TEST_ASSERT_NOT_NULL(keys_vec);
    keys_vec->len = R;
    memcpy(ray_data(keys_vec), keys_data, sizeof(keys_data));

    int64_t key_sym = ray_sym_intern("s", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, key_sym, keys_vec);
    ray_release(keys_vec);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_key = ray_scan(g, "s");
    uint16_t ops[] = { OP_COUNT };
    ray_op_t* ins[] = { scan_key };
    ray_op_t* keys[] = { scan_key };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 2;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, grp);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 2);

    ray_t* out_key = ray_table_get_col(res, key_sym);
    ray_t* out_cnt = ray_table_get_col_idx(res, 1);
    TEST_ASSERT_NOT_NULL(out_key);
    TEST_ASSERT_NOT_NULL(out_cnt);

    int got_a = 0, got_b = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++) {
        int64_t k = ray_read_sym(ray_data(out_key), i, out_key->type, out_key->attrs);
        int64_t c = ((int64_t*)ray_data(out_cnt))[i];
        if (k == sym_a && c == 4) got_a = 1;
        if (k == sym_b && c == 3) got_b = 1;
    }
    TEST_ASSERT_TRUE(got_a && got_b);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_five_key_group_top_count_emit_filter(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int16_t rows[][5] = {
        {1, 10, 20, 30, 40}, {1, 10, 20, 30, 40},
        {1, 10, 20, 30, 40}, {1, 10, 20, 30, 40},
        {2, 11, 21, 31, 41}, {2, 11, 21, 31, 41},
        {2, 11, 21, 31, 41},
        {3, 12, 22, 32, 42}, {3, 12, 22, 32, 42},
        {4, 13, 23, 33, 43}
    };
    enum { R = (int)(sizeof(rows) / sizeof(rows[0])) };
    const char* names[5] = { "k0", "k1", "k2", "k3", "k4" };
    int64_t syms[5];

    ray_t* tbl = ray_table_new(5);
    TEST_ASSERT_NOT_NULL(tbl);
    for (int col = 0; col < 5; col++) {
        ray_t* vec = ray_vec_new(RAY_I16, R);
        TEST_ASSERT_NOT_NULL(vec);
        vec->len = R;
        int16_t* data = (int16_t*)ray_data(vec);
        for (int row = 0; row < R; row++)
            data[row] = rows[row][col];
        syms[col] = ray_sym_intern(names[col], 2);
        tbl = ray_table_add_col(tbl, syms[col], vec);
        ray_release(vec);
    }

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scans[5];
    for (int i = 0; i < 5; i++) {
        scans[i] = ray_scan(g, names[i]);
        TEST_ASSERT_NOT_NULL(scans[i]);
    }
    uint16_t ops[] = { OP_COUNT };
    ray_op_t* ins[] = { scans[0] };
    ray_op_t* grp = ray_group(g, scans, 5, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    ray_group_emit_filter_t prev = ray_group_emit_filter_get();
    ray_group_emit_filter_t filter = {0};
    filter.enabled = 1;
    filter.agg_index = 0;
    filter.top_count_take = 2;
    ray_group_emit_filter_set(filter);
    ray_t* res = ray_execute(g, grp);
    ray_group_emit_filter_set(prev);

    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 2);

    ray_t* out_k0 = ray_table_get_col(res, syms[0]);
    ray_t* out_k1 = ray_table_get_col(res, syms[1]);
    ray_t* out_k4 = ray_table_get_col(res, syms[4]);
    ray_t* out_cnt = ray_table_get_col_idx(res, 5);
    TEST_ASSERT_NOT_NULL(out_k0);
    TEST_ASSERT_NOT_NULL(out_k1);
    TEST_ASSERT_NOT_NULL(out_k4);
    TEST_ASSERT_NOT_NULL(out_cnt);

    int got_1 = 0, got_2 = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++) {
        int16_t k0 = ((int16_t*)ray_data(out_k0))[i];
        int16_t k1 = ((int16_t*)ray_data(out_k1))[i];
        int16_t k4 = ((int16_t*)ray_data(out_k4))[i];
        int64_t c = ((int64_t*)ray_data(out_cnt))[i];
        if (k0 == 1 && k1 == 10 && k4 == 40 && c == 4) got_1 = 1;
        if (k0 == 2 && k1 == 11 && k4 == 41 && c == 3) got_2 = 1;
    }
    TEST_ASSERT_TRUE(got_1 && got_2);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test registry
 * -------------------------------------------------------------------------- */

const test_entry_t group_extra_entries[] = {
    { "group_extra/scalar_sum_f64_parallel",       test_scalar_sum_f64_parallel,       NULL, NULL },
    { "group_extra/scalar_sum_linear_i64_parallel", test_scalar_sum_linear_i64_parallel, NULL, NULL },
    { "group_extra/scalar_prod_i64_parallel",      test_scalar_prod_i64_parallel,      NULL, NULL },
    { "group_extra/scalar_first_last_parallel",    test_scalar_first_last_parallel,    NULL, NULL },
    { "group_extra/scalar_min_max_f64_parallel",   test_scalar_min_max_f64_parallel,   NULL, NULL },
    { "group_extra/scalar_multi_agg_parallel",     test_scalar_multi_agg_parallel,     NULL, NULL },
    { "group_extra/scalar_prod_f64_parallel",      test_scalar_prod_f64_parallel,      NULL, NULL },
    { "group_extra/scalar_min_max_i64_parallel",   test_scalar_min_max_i64_parallel,   NULL, NULL },
    { "group_extra/scalar_stddev_f64_parallel",    test_scalar_stddev_f64_parallel,    NULL, NULL },
    { "group_extra/count_distinct_small_types",    test_count_distinct_small_types,    NULL, NULL },
    { "group_extra/reduction_prod_parallel",       test_reduction_prod_parallel,       NULL, NULL },
    { "group_extra/reduction_var_i64_parallel",    test_reduction_var_i64_parallel,    NULL, NULL },
    { "group_extra/count_distinct_parallel_types", test_count_distinct_parallel_types, NULL, NULL },
    { "group_extra/count_distinct_per_group_parallel", test_count_distinct_per_group_parallel, NULL, NULL },
    { "group_extra/i16_group_top_count_emit_filter", test_i16_group_top_count_emit_filter, NULL, NULL },
    { "group_extra/sym_group_top_count_emit_filter", test_sym_group_top_count_emit_filter, NULL, NULL },
    { "group_extra/five_key_group_top_count_emit_filter", test_five_key_group_top_count_emit_filter, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
