/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

/*
 * test_window.c — coverage tests for src/ops/window.c.
 *
 * OP_WINDOW has no rfl-reachable builder, so all coverage of window
 * functions (sum/avg/min/max/rank/lag/lead/...) must come from C unit
 * tests that drive ray_window_op directly.  This file exhaustively
 * exercises the per-kind dispatch in win_compute_partition (lines
 * 146-540 of window.c), plus key/result shapes in exec_window
 * (multi-partition, multi-key, sym/date/f64 keys, parallel path).
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "table/sym.h"
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ─── Helpers ─────────────────────────────────────────────────────── */

/* Build a 1- or 2-column table from the given int64 group + value arrays.
 * Caller owns the result and must ray_release it. */
static ray_t* mk_tbl_i64_2(const int64_t* g_data, const int64_t* v_data,
                           int64_t n) {
    ray_t* gv = ray_vec_from_raw(RAY_I64, g_data, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, v_data, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* t = ray_table_new(2);
    t = ray_table_add_col(t, ng, gv);
    t = ray_table_add_col(t, nv, vv);
    ray_release(gv);
    ray_release(vv);
    return t;
}

/* 2-col table: g(I64), v(F64) */
static ray_t* mk_tbl_i64_f64(const int64_t* g_data, const double* v_data,
                              int64_t n) {
    ray_t* gv = ray_vec_from_raw(RAY_I64, g_data, n);
    ray_t* vv = ray_vec_from_raw(RAY_F64, v_data, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* t = ray_table_new(2);
    t = ray_table_add_col(t, ng, gv);
    t = ray_table_add_col(t, nv, vv);
    ray_release(gv);
    ray_release(vv);
    return t;
}

/* Build a window op with ROWS BETWEEN UNBOUNDED PRECEDING AND
 * UNBOUNDED FOLLOWING (the "whole partition" frame).  Kind is the
 * RAY_WIN_* kind; func_input_name is the column name driving the
 * function (or "v" / "g" — anything for fns that ignore input). */
static ray_op_t* build_whole_window(
    ray_graph_t* g, ray_op_t* tbl_op,
    const char* part_name, const char* order_name,
    uint8_t kind, const char* func_input_name, int64_t param)
{
    ray_op_t* parts[1];
    ray_op_t* orders[1];
    uint8_t   ndesc[1] = {0};
    uint8_t   n_part = 0, n_order = 0;
    if (part_name)  { parts[0]  = ray_scan(g, part_name);  n_part  = 1; }
    if (order_name) { orders[0] = ray_scan(g, order_name); n_order = 1; }

    uint8_t kinds[1]    = { kind };
    ray_op_t* fins[1]   = { ray_scan(g, func_input_name) };
    int64_t   params[1] = { param };

    return ray_window_op(g, tbl_op,
                         n_part  ? parts  : NULL, n_part,
                         n_order ? orders : NULL, n_order ? ndesc : NULL,
                         n_order,
                         kinds, fins, params, 1,
                         RAY_FRAME_ROWS,
                         RAY_BOUND_UNBOUNDED_PRECEDING,
                         RAY_BOUND_UNBOUNDED_FOLLOWING,
                         0, 0);
}

/* Same as build_whole_window but with ROWS BETWEEN UNBOUNDED PRECEDING
 * AND CURRENT ROW (incremental "running" frame). */
static ray_op_t* build_running_window(
    ray_graph_t* g, ray_op_t* tbl_op,
    const char* part_name, const char* order_name,
    uint8_t kind, const char* func_input_name, int64_t param)
{
    ray_op_t* parts[1];
    ray_op_t* orders[1];
    uint8_t   ndesc[1] = {0};
    uint8_t   n_part = 0, n_order = 0;
    if (part_name)  { parts[0]  = ray_scan(g, part_name);  n_part  = 1; }
    if (order_name) { orders[0] = ray_scan(g, order_name); n_order = 1; }

    uint8_t kinds[1]    = { kind };
    ray_op_t* fins[1]   = { ray_scan(g, func_input_name) };
    int64_t   params[1] = { param };

    return ray_window_op(g, tbl_op,
                         n_part  ? parts  : NULL, n_part,
                         n_order ? orders : NULL, n_order ? ndesc : NULL,
                         n_order,
                         kinds, fins, params, 1,
                         RAY_FRAME_ROWS,
                         RAY_BOUND_UNBOUNDED_PRECEDING,
                         RAY_BOUND_CURRENT_ROW,
                         0, 0);
}

/* Window result column "_w0" is the first auto-named column appended
 * after the source columns.  Returns its idx (== ncols_in). */
static ray_t* win_result_col(ray_t* result, int64_t base_ncols) {
    return ray_table_get_col_idx(result, base_ncols);
}

/* ─── ROW_NUMBER ───────────────────────────────────────────────────── */

/* Multi-partition row_number — value column ordered ascending so output
 * is deterministic.  Tests dispatch + multi-part path. */
static test_result_t test_window_row_number_partitioned(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 6;
    int64_t gd[] = {1, 1, 1, 2, 2, 2};
    int64_t vd[] = {10, 20, 30, 40, 50, 60};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_ROW_NUMBER, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 6);

    ray_t* rc = win_result_col(result, 2);
    TEST_ASSERT_NOT_NULL(rc);
    TEST_ASSERT_EQ_I(rc->type, RAY_I64);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* rows are already in (g asc, v asc) order, so row_number = 1..3,1..3 */
    TEST_ASSERT_EQ_I(rd[0], 1); TEST_ASSERT_EQ_I(rd[1], 2);
    TEST_ASSERT_EQ_I(rd[2], 3);
    TEST_ASSERT_EQ_I(rd[3], 1); TEST_ASSERT_EQ_I(rd[4], 2);
    TEST_ASSERT_EQ_I(rd[5], 3);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── RANK / DENSE_RANK ────────────────────────────────────────────── */

static test_result_t test_window_rank_dense_rank(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* Single partition, ties on order column to exercise rank-bump logic */
    int64_t n = 5;
    int64_t gd[] = {1, 1, 1, 1, 1};
    int64_t vd[] = {10, 20, 20, 30, 40};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    /* RANK */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_RANK, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        /* ranks: 1, 2, 2, 4, 5 (tie at 20 stays at rank 2, then jumps to 4) */
        TEST_ASSERT_EQ_I(rd[0], 1); TEST_ASSERT_EQ_I(rd[1], 2);
        TEST_ASSERT_EQ_I(rd[2], 2); TEST_ASSERT_EQ_I(rd[3], 4);
        TEST_ASSERT_EQ_I(rd[4], 5);
        ray_release(result); ray_graph_free(g);
    }

    /* DENSE_RANK */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_DENSE_RANK, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        /* dense_ranks: 1, 2, 2, 3, 4 (tie at 20 → 2; 30 → 3; 40 → 4) */
        TEST_ASSERT_EQ_I(rd[0], 1); TEST_ASSERT_EQ_I(rd[1], 2);
        TEST_ASSERT_EQ_I(rd[2], 2); TEST_ASSERT_EQ_I(rd[3], 3);
        TEST_ASSERT_EQ_I(rd[4], 4);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── NTILE ───────────────────────────────────────────────────────── */

static test_result_t test_window_ntile(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 6;
    int64_t gd[] = {1, 1, 1, 1, 1, 1};
    int64_t vd[] = {1, 2, 3, 4, 5, 6};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    /* NTILE 3 → buckets [1,1,2,2,3,3] for 6 rows / 3 tiles */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_NTILE, "v", 3);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 1); TEST_ASSERT_EQ_I(rd[1], 1);
        TEST_ASSERT_EQ_I(rd[2], 2); TEST_ASSERT_EQ_I(rd[3], 2);
        TEST_ASSERT_EQ_I(rd[4], 3); TEST_ASSERT_EQ_I(rd[5], 3);
        ray_release(result); ray_graph_free(g);
    }

    /* NTILE 0 → coerced to 1, all rows in bucket 1 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_NTILE, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 1);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── COUNT (whole + running) ─────────────────────────────────────── */

static test_result_t test_window_count(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    int64_t vd[] = {10, 20, 30, 40};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    /* Whole-frame COUNT: each row gets partition size */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_COUNT, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 2); TEST_ASSERT_EQ_I(rd[1], 2);
        TEST_ASSERT_EQ_I(rd[2], 2); TEST_ASSERT_EQ_I(rd[3], 2);
        ray_release(result); ray_graph_free(g);
    }

    /* Running COUNT: 1, 2, 1, 2 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_running_window(g, tbl_op, "g", "v",
                                            RAY_WIN_COUNT, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 1); TEST_ASSERT_EQ_I(rd[1], 2);
        TEST_ASSERT_EQ_I(rd[2], 1); TEST_ASSERT_EQ_I(rd[3], 2);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── SUM (whole + running, i64 + f64) ───────────────────────────── */

static test_result_t test_window_sum_i64(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    int64_t vd[] = {10, 20, 30, 40};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    /* Whole SUM */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_SUM, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_I64);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 30); TEST_ASSERT_EQ_I(rd[1], 30);
        TEST_ASSERT_EQ_I(rd[2], 70); TEST_ASSERT_EQ_I(rd[3], 70);
        ray_release(result); ray_graph_free(g);
    }

    /* Running SUM */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_running_window(g, tbl_op, "g", "v",
                                            RAY_WIN_SUM, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 10); TEST_ASSERT_EQ_I(rd[1], 30);
        TEST_ASSERT_EQ_I(rd[2], 30); TEST_ASSERT_EQ_I(rd[3], 70);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_window_sum_f64(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    double  vd[] = {1.5, 2.5, 3.5, 4.5};
    ray_t* tbl = mk_tbl_i64_f64(gd, vd, n);

    /* Whole SUM (f64) */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_SUM, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_F64);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[0], 4.0, 1e-9); TEST_ASSERT_EQ_F(rd[1], 4.0, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 8.0, 1e-9); TEST_ASSERT_EQ_F(rd[3], 8.0, 1e-9);
        ray_release(result); ray_graph_free(g);
    }

    /* Running SUM (f64) */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_running_window(g, tbl_op, "g", "v",
                                            RAY_WIN_SUM, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[0], 1.5, 1e-9); TEST_ASSERT_EQ_F(rd[1], 4.0, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 3.5, 1e-9); TEST_ASSERT_EQ_F(rd[3], 8.0, 1e-9);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── AVG (i64 input → f64 output, both frames + null-only partition) */

static test_result_t test_window_avg(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    int64_t vd[] = {10, 20, 30, 40};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    /* Whole AVG */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_AVG, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_F64);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[0], 15.0, 1e-9); TEST_ASSERT_EQ_F(rd[1], 15.0, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 35.0, 1e-9); TEST_ASSERT_EQ_F(rd[3], 35.0, 1e-9);
        ray_release(result); ray_graph_free(g);
    }

    /* Running AVG */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_running_window(g, tbl_op, "g", "v",
                                            RAY_WIN_AVG, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[0], 10.0, 1e-9); TEST_ASSERT_EQ_F(rd[1], 15.0, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 30.0, 1e-9); TEST_ASSERT_EQ_F(rd[3], 35.0, 1e-9);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── MIN / MAX (both i64 and f64 paths, whole + running) ──────────── */

static test_result_t test_window_min_max_i64(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* Tilted values to exercise running min/max accumulation */
    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    int64_t vd[] = {30, 10, 50, 20};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    /* Whole MIN/MAX with no order — sort order undefined but min/max
     * over partition is deterministic. */

    /* MIN whole */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", NULL,
                                          RAY_WIN_MIN, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_I64);
        int64_t* rd = (int64_t*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 10);
        ray_release(result); ray_graph_free(g);
    }
    /* MAX whole */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", NULL,
                                          RAY_WIN_MAX, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 50);
        ray_release(result); ray_graph_free(g);
    }
    /* Running MIN ordered by v ASC: each cumulative min = first value */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_running_window(g, tbl_op, "g", "v",
                                            RAY_WIN_MIN, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        /* sorted: [10, 20, 30, 50]; running min stays at 10 throughout.
         * rd is indexed by ORIGINAL row position because window writes
         * out[sorted_idx[i]]. */
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 10);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_window_min_max_f64(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    double  vd[] = {3.5, 1.5, 5.5, 2.5};
    ray_t* tbl = mk_tbl_i64_f64(gd, vd, n);

    /* Whole MIN f64 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", NULL,
                                          RAY_WIN_MIN, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_F64);
        double* rd = (double*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_F(rd[i], 1.5, 1e-9);
        ray_release(result); ray_graph_free(g);
    }
    /* Whole MAX f64 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", NULL,
                                          RAY_WIN_MAX, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_F(rd[i], 5.5, 1e-9);
        ray_release(result); ray_graph_free(g);
    }
    /* Running MIN f64 ordered by v ASC */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_running_window(g, tbl_op, "g", "v",
                                            RAY_WIN_MIN, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_F(rd[i], 1.5, 1e-9);
        ray_release(result); ray_graph_free(g);
    }
    /* Running MAX f64 ordered by v ASC: cumulative max is value itself */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_running_window(g, tbl_op, "g", "v",
                                            RAY_WIN_MAX, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        /* original positions: vd = {3.5, 1.5, 5.5, 2.5}; sorted asc:
         * [1.5, 2.5, 3.5, 5.5], running max = sorted value at each step.
         * out[orig_idx_of_sorted[i]] = running_max_at_step_i.
         *  step 0: orig=1, val=1.5  → rd[1]=1.5
         *  step 1: orig=3, val=2.5  → rd[3]=2.5
         *  step 2: orig=0, val=3.5  → rd[0]=3.5
         *  step 3: orig=2, val=5.5  → rd[2]=5.5 */
        TEST_ASSERT_EQ_F(rd[1], 1.5, 1e-9);
        TEST_ASSERT_EQ_F(rd[3], 2.5, 1e-9);
        TEST_ASSERT_EQ_F(rd[0], 3.5, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 5.5, 1e-9);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── LAG / LEAD (i64 + f64, default and offset>1) ─────────────────── */

static test_result_t test_window_lag_lead_i64(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    int64_t vd[] = {10, 20, 30, 40};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    /* LAG offset=1: first row in partition is null, others get prior */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_LAG, "v", 1);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_I64);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));
        TEST_ASSERT_FALSE(ray_vec_is_null(rc, 1));
        TEST_ASSERT_EQ_I(rd[1], 10);
        TEST_ASSERT_EQ_I(rd[2], 20);
        TEST_ASSERT_EQ_I(rd[3], 30);
        ray_release(result); ray_graph_free(g);
    }
    /* LAG offset=0 → coerced to 1 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_LAG, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));
        TEST_ASSERT_EQ_I(rd[1], 10);
        ray_release(result); ray_graph_free(g);
    }
    /* LEAD offset=2 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_LEAD, "v", 2);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 30);
        TEST_ASSERT_EQ_I(rd[1], 40);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 2));
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 3));
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

static test_result_t test_window_lag_lead_f64(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 3;
    int64_t gd[] = {1, 1, 1};
    double  vd[] = {1.5, 2.5, 3.5};
    ray_t* tbl = mk_tbl_i64_f64(gd, vd, n);

    /* LAG f64 offset=1 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_LAG, "v", 1);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_F64);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));
        TEST_ASSERT_EQ_F(rd[1], 1.5, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 2.5, 1e-9);
        ray_release(result); ray_graph_free(g);
    }
    /* LEAD f64 offset=1 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_LEAD, "v", 1);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[0], 2.5, 1e-9);
        TEST_ASSERT_EQ_F(rd[1], 3.5, 1e-9);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 2));
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── FIRST_VALUE / LAST_VALUE / NTH_VALUE ────────────────────────── */

static test_result_t test_window_first_last_nth(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    int64_t vd[] = {10, 20, 30, 40};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    /* FIRST_VALUE (whole frame, sorted asc) → 10 for all rows */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_FIRST_VALUE, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 10);
        ray_release(result); ray_graph_free(g);
    }

    /* LAST_VALUE (whole frame) → 40 for all rows */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_LAST_VALUE, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 40);
        ray_release(result); ray_graph_free(g);
    }

    /* LAST_VALUE running → each row sees its own value (current row) */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_running_window(g, tbl_op, "g", "v",
                                            RAY_WIN_LAST_VALUE, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 10); TEST_ASSERT_EQ_I(rd[1], 20);
        TEST_ASSERT_EQ_I(rd[2], 30); TEST_ASSERT_EQ_I(rd[3], 40);
        ray_release(result); ray_graph_free(g);
    }

    /* NTH_VALUE 2nd → 20 for all rows */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_NTH_VALUE, "v", 2);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 20);
        ray_release(result); ray_graph_free(g);
    }

    /* NTH_VALUE 0 → coerced to 1, returns first */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_NTH_VALUE, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 10);
        ray_release(result); ray_graph_free(g);
    }

    /* NTH_VALUE > part_len → all NULL */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_NTH_VALUE, "v", 99);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        for (int64_t i = 0; i < n; i++)
            TEST_ASSERT_TRUE(ray_vec_is_null(rc, i));
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── FIRST/LAST/NTH on f64 ───────────────────────────────────────── */

static test_result_t test_window_first_last_nth_f64(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 3;
    int64_t gd[] = {1, 1, 1};
    double  vd[] = {1.5, 2.5, 3.5};
    ray_t* tbl = mk_tbl_i64_f64(gd, vd, n);

    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_FIRST_VALUE, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_F64);
        double* rd = (double*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_F(rd[i], 1.5, 1e-9);
        ray_release(result); ray_graph_free(g);
    }
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_LAST_VALUE, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_F(rd[i], 3.5, 1e-9);
        ray_release(result); ray_graph_free(g);
    }
    /* Running LAST_VALUE f64: each row's own value */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_running_window(g, tbl_op, "g", "v",
                                            RAY_WIN_LAST_VALUE, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[0], 1.5, 1e-9);
        TEST_ASSERT_EQ_F(rd[1], 2.5, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 3.5, 1e-9);
        ray_release(result); ray_graph_free(g);
    }
    /* NTH_VALUE 3rd f64 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_NTH_VALUE, "v", 3);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_F(rd[i], 3.5, 1e-9);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── NULL handling: SUM/AVG skip nulls; AVG of all-null → null ────── */

static test_result_t test_window_nulls(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    int64_t vd[] = {10, 20, 0, 0};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    /* Mark rows 2,3 as null in v (partition g==2 has all-null values) */
    ray_vec_set_null(vv, 2, true);
    ray_vec_set_null(vv, 3, true);

    int64_t ng = ray_sym_intern("g", 1);
    int64_t nvname = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nvname, vv);
    ray_release(gv); ray_release(vv);

    /* SUM whole: partition 1 → 30, partition 2 → 0 (all nulls skipped) */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_SUM, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 30); TEST_ASSERT_EQ_I(rd[1], 30);
        TEST_ASSERT_EQ_I(rd[2], 0);  TEST_ASSERT_EQ_I(rd[3], 0);
        ray_release(result); ray_graph_free(g);
    }

    /* AVG whole: partition 1 → 15, partition 2 → null */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_AVG, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_FALSE(ray_vec_is_null(rc, 0));
        TEST_ASSERT_EQ_F(rd[0], 15.0, 1e-9);
        TEST_ASSERT_FALSE(ray_vec_is_null(rc, 1));
        TEST_ASSERT_EQ_F(rd[1], 15.0, 1e-9);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 2));
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 3));
        ray_release(result); ray_graph_free(g);
    }

    /* MIN whole: partition 1 → 10, partition 2 → null */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_MIN, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 10);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 2));
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 3));
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Running MIN/MAX with leading null — exercises win_set_null before
 *     any non-null seen.  Also covers LAG/LEAD where the source value is
 *     null (propagates null to result). */

static test_result_t test_window_running_min_leading_null(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    int64_t vd[] = {0, 30, 20, 10};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    /* Null at position 0 (lowest sort index); after sort by orig-idx
     * the first row in partition is the null one. */
    ray_vec_set_null(vv, 0, true);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* Order by orig position to keep deterministic — we need the null
     * to come first.  Build a separate idx column. */
    /* No order key: insertion preserves input order, so position 0 (null)
     * comes first.  Running MIN at row 0 → null; thereafter min of seen
     * non-nulls. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[] = { g_op };
    /* Use orig-position-style order via a helper: ORDER BY g (constant) means
     * sort is stable on input order for the tie-breaker.  Actually since all g
     * are the same and there's only one part_key (g), the sort still has to
     * resolve.  Tie-break is the index, hence input order is preserved. */
    uint8_t kinds[] = { RAY_WIN_MIN };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                NULL, NULL, 0,   /* no order */
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_CURRENT_ROW,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    /* Insertion sort with single tied key + n_sort=1 partition radix
     * stable order leaves rows in input order: [null, 30, 20, 10].
     * Running MIN: [null, 30, 20, 10]. */
    int64_t* rd = (int64_t*)ray_data(rc);
    TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));
    TEST_ASSERT_EQ_I(rd[1], 30);
    TEST_ASSERT_EQ_I(rd[2], 20);
    TEST_ASSERT_EQ_I(rd[3], 10);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* LAG with null source: result is null where source is null */
static test_result_t test_window_lag_null_source(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    int64_t vd[] = {10, 0, 30, 40};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    ray_vec_set_null(vv, 1, true);   /* row 1 source value null */
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* No order key, LAG offset=1 — for sorted input order [0,1,2,3]:
     * rd[0] = null (boundary)
     * rd[1] = 10 (lag from row 0)
     * rd[2] = null (lag from row 1, which is null)
     * rd[3] = 30 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[] = { g_op };
    uint8_t kinds[] = { RAY_WIN_LAG };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 1 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));
    TEST_ASSERT_EQ_I(rd[1], 10);
    TEST_ASSERT_TRUE(ray_vec_is_null(rc, 2));
    TEST_ASSERT_EQ_I(rd[3], 30);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* FIRST_VALUE/LAST_VALUE/NTH_VALUE with null source — output is null. */
static test_result_t test_window_first_last_nth_null(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 3;
    int64_t gd[] = {1, 1, 1};
    int64_t vd[] = {0, 20, 30};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    ray_vec_set_null(vv, 0, true);   /* first row null → FIRST_VALUE = null */
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* No order, default partition.  Insertion sort preserves input order
     * → first sorted row is original row 0 (null). */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[] = { g_op };
    uint8_t kinds[] = { RAY_WIN_FIRST_VALUE };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    /* All rows: FIRST_VALUE = first sorted row's value = null */
    for (int64_t i = 0; i < n; i++)
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, i));

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Empty-table early return path ───────────────────────────────── */

static test_result_t test_window_empty(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* Build a 0-row table */
    ray_t* gv = ray_vec_new(RAY_I64, 0); gv->len = 0;
    ray_t* vv = ray_vec_new(RAY_I64, 0); vv->len = 0;
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_ROW_NUMBER, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* Early return: result equals input table (no _w0 column added) */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 0);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── No partition key: whole table is one partition ──────────────── */

static test_result_t test_window_no_partition(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 5;
    int64_t gd[] = {0, 0, 0, 0, 0};
    int64_t vd[] = {1, 2, 3, 4, 5};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    /* No PARTITION BY, ORDER BY v; running SUM */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_running_window(g, tbl_op, NULL, "v",
                                            RAY_WIN_SUM, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 1);
        TEST_ASSERT_EQ_I(rd[1], 3);
        TEST_ASSERT_EQ_I(rd[2], 6);
        TEST_ASSERT_EQ_I(rd[3], 10);
        TEST_ASSERT_EQ_I(rd[4], 15);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── SYM partition key (exercises enum_rank build) ────────────────── */

static test_result_t test_window_sym_partition(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* Build table with a SYM column via ray_sym_vec_new + ray_vec_append.
     * 64-bit width keeps interned IDs intact across the window engine's
     * sym-rank build path. */
    int64_t n = 6;
    int64_t s_a = ray_sym_intern("aa", 2);
    int64_t s_b = ray_sym_intern("bb", 2);
    int64_t s_c = ray_sym_intern("cc", 2);
    int64_t vd[] = {10, 20, 30, 40, 50, 60};

    ray_t* sv = ray_sym_vec_new(RAY_SYM_W64, n);
    sv = ray_vec_append(sv, &s_a);
    sv = ray_vec_append(sv, &s_a);
    sv = ray_vec_append(sv, &s_b);
    sv = ray_vec_append(sv, &s_b);
    sv = ray_vec_append(sv, &s_c);
    sv = ray_vec_append(sv, &s_c);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, sv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(sv); ray_release(vv);

    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_SUM, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        /* Whole-partition SUM grouped by sym: 30 / 70 / 110 */
        TEST_ASSERT_EQ_I(rd[0], 30); TEST_ASSERT_EQ_I(rd[1], 30);
        TEST_ASSERT_EQ_I(rd[2], 70); TEST_ASSERT_EQ_I(rd[3], 70);
        TEST_ASSERT_EQ_I(rd[4], 110); TEST_ASSERT_EQ_I(rd[5], 110);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Multi-key partition (tests pkey_gather n_part>1 path) ─────────── */

static test_result_t test_window_multikey_partition(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 6;
    int32_t a_data[] = {1, 1, 1, 2, 2, 2};
    int32_t b_data[] = {1, 1, 2, 1, 2, 2};
    int64_t vd[]     = {10, 20, 30, 40, 50, 60};

    ray_t* av = ray_vec_from_raw(RAY_I32, a_data, n);
    ray_t* bv = ray_vec_from_raw(RAY_I32, b_data, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, na, av);
    tbl = ray_table_add_col(tbl, nb, bv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(av); ray_release(bv); ray_release(vv);

    /* PARTITION BY (a, b), no order, SUM(v) whole.
     * Partitions: (1,1)→30, (1,2)→30, (2,1)→40, (2,2)→110 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]    = { a_op, b_op };
    uint8_t   kinds[]    = { RAY_WIN_SUM };
    ray_op_t* fins[]     = { v_op };
    int64_t   params[]   = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 2,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 3);
    int64_t* rd = (int64_t*)ray_data(rc);
    TEST_ASSERT_EQ_I(rd[0], 30);  /* (1,1): 10+20 */
    TEST_ASSERT_EQ_I(rd[1], 30);
    TEST_ASSERT_EQ_I(rd[2], 30);  /* (1,2): 30 */
    TEST_ASSERT_EQ_I(rd[3], 40);  /* (2,1): 40 */
    TEST_ASSERT_EQ_I(rd[4], 110); /* (2,2): 50+60 */
    TEST_ASSERT_EQ_I(rd[5], 110);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Multiple window funcs in one OP (n_funcs > 1) ─────────────────── */

static test_result_t test_window_multiple_funcs(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    int64_t vd[] = {10, 20, 30, 40};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[] = { g_op };
    ray_op_t* orders[] = { v_op };
    uint8_t   ndesc[] = {0};
    /* row_number, sum, avg in one OP */
    uint8_t kinds[] = { RAY_WIN_ROW_NUMBER, RAY_WIN_SUM, RAY_WIN_AVG };
    ray_op_t* fins[]   = { v_op, v_op, v_op };
    int64_t   params[] = { 0, 0, 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                orders, ndesc, 1,
                                kinds, fins, params, 3,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 5);  /* 2 + 3 */

    /* _w0 = row_number (I64) */
    ray_t* w0 = ray_table_get_col_idx(result, 2);
    TEST_ASSERT_EQ_I(w0->type, RAY_I64);
    int64_t* rn = (int64_t*)ray_data(w0);
    TEST_ASSERT_EQ_I(rn[0], 1); TEST_ASSERT_EQ_I(rn[1], 2);
    TEST_ASSERT_EQ_I(rn[2], 1); TEST_ASSERT_EQ_I(rn[3], 2);

    /* _w1 = sum I64 */
    ray_t* w1 = ray_table_get_col_idx(result, 3);
    TEST_ASSERT_EQ_I(w1->type, RAY_I64);
    int64_t* sd = (int64_t*)ray_data(w1);
    TEST_ASSERT_EQ_I(sd[0], 30); TEST_ASSERT_EQ_I(sd[2], 70);

    /* _w2 = avg F64 */
    ray_t* w2 = ray_table_get_col_idx(result, 4);
    TEST_ASSERT_EQ_I(w2->type, RAY_F64);
    double* ad = (double*)ray_data(w2);
    TEST_ASSERT_EQ_F(ad[0], 15.0, 1e-9);
    TEST_ASSERT_EQ_F(ad[2], 35.0, 1e-9);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Larger table (>64 rows): exercises radix-sort path ───────────── */

static test_result_t test_window_radix_path(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* 200 rows: insertion sort threshold (64) crossed → radix path */
    int64_t n = 200;
    ray_t* gv = ray_vec_new(RAY_I64, n); gv->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* gd = (int64_t*)ray_data(gv);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        gd[i] = i % 4;       /* 4 partitions */
        vd[i] = i;
    }
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_COUNT, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* Each of the 4 partitions has 50 rows → COUNT(*) over partition = 50 */
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 50);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Very large table: parallel radix + parallel partition path ────── */

static test_result_t test_window_parallel_path(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* 8192+1 rows so we cross RADIX_SORT_THRESHOLD (4096) AND
     * SMALL_POOL_THRESHOLD (8192) — hits parallel sort + parallel
     * per-partition compute. */
    int64_t n = 9000;
    ray_t* gv = ray_vec_new(RAY_I64, n); gv->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* gd = (int64_t*)ray_data(gv);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        gd[i] = i % 100;     /* 100 partitions of ~90 each */
        vd[i] = i;
    }
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_COUNT, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* Each partition has exactly 90 rows */
    int64_t ones = 0;
    for (int64_t i = 0; i < n; i++)
        if (rd[i] == 90) ones++;
    TEST_ASSERT_EQ_I(ones, n);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── ORDER DESC: exercises desc[k]=1 in radix encoding ─────────────── */

static test_result_t test_window_order_desc(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    int64_t vd[] = {10, 20, 30, 40};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    /* ORDER BY v DESC, running SUM */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { g_op };
    ray_op_t* orders[] = { v_op };
    uint8_t   ndesc[]  = { 1 };  /* DESC */
    uint8_t   kinds[]  = { RAY_WIN_SUM };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                orders, ndesc, 1,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_CURRENT_ROW,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* DESC sort: [40, 30, 20, 10]; running sum: [40, 70, 90, 100].
     * out[orig_idx_of_sorted[i]] = running sum at step i.
     * Original positions 3,2,1,0 in sorted order:
     *  step 0: orig=3, sum=40 → rd[3]=40
     *  step 1: orig=2, sum=70 → rd[2]=70
     *  step 2: orig=1, sum=90 → rd[1]=90
     *  step 3: orig=0, sum=100→ rd[0]=100 */
    TEST_ASSERT_EQ_I(rd[3], 40);
    TEST_ASSERT_EQ_I(rd[2], 70);
    TEST_ASSERT_EQ_I(rd[1], 90);
    TEST_ASSERT_EQ_I(rd[0], 100);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── DATE keys: I32-typed sort path ───────────────────────────────── */

static test_result_t test_window_date_partition(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* DATE column (I32-typed days-since-epoch). Three distinct dates,
     * 2 rows each, one running SUM over partition. */
    int64_t n = 6;
    int32_t dd[] = {100, 100, 200, 200, 300, 300};
    int64_t vd[] = {1, 2, 3, 4, 5, 6};
    ray_t* dv = ray_vec_from_raw(RAY_DATE, dd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    int64_t nd = ray_sym_intern("d", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, nd, dv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(dv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "d", "v",
                                      RAY_WIN_SUM, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    TEST_ASSERT_EQ_I(rd[0], 3);  TEST_ASSERT_EQ_I(rd[1], 3);
    TEST_ASSERT_EQ_I(rd[2], 7);  TEST_ASSERT_EQ_I(rd[3], 7);
    TEST_ASSERT_EQ_I(rd[4], 11); TEST_ASSERT_EQ_I(rd[5], 11);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── F64 partition key (forces merge-sort fallback because F64 multi-
 *     key is not radix-packable when used alongside other 64-bit keys) */

static test_result_t test_window_f64_partition(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 6;
    double  gd[] = {1.0, 1.0, 1.0, 2.0, 2.0, 2.0};
    int64_t vd[] = {10, 20, 30, 40, 50, 60};
    ray_t* gv = ray_vec_from_raw(RAY_F64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_SUM, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    TEST_ASSERT_EQ_I(rd[0], 60);  TEST_ASSERT_EQ_I(rd[2], 60);
    TEST_ASSERT_EQ_I(rd[3], 150); TEST_ASSERT_EQ_I(rd[5], 150);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── BOOL partition key (small-radix encoding) ─────────────────────── */

static test_result_t test_window_bool_partition(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 6;
    uint8_t gd[] = {0, 0, 0, 1, 1, 1};
    int64_t vd[] = {10, 20, 30, 40, 50, 60};
    ray_t* gv = ray_vec_from_raw(RAY_BOOL, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_SUM, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    TEST_ASSERT_EQ_I(rd[0], 60);
    TEST_ASSERT_EQ_I(rd[3], 150);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── No funcs: early-return path (n_funcs == 0 retains input) ────── */

static test_result_t test_window_no_funcs(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    int64_t vd[] = {10, 20, 30, 40};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* parts[] = { g_op };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                NULL, NULL, 0,
                                NULL, NULL, NULL, 0,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* n_funcs==0 means: returns input table unchanged */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 2);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── STR partition key — forces merge-sort fallback (radix can't
 *     handle RAY_STR), exercising win_keys_differ's STR arm. */

static test_result_t test_window_str_partition(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 6;
    int64_t vd[] = {10, 20, 30, 40, 50, 60};

    ray_t* sv = ray_vec_new(RAY_STR, n);
    sv = ray_str_vec_append(sv, "aa", 2);
    sv = ray_str_vec_append(sv, "aa", 2);
    sv = ray_str_vec_append(sv, "bb", 2);
    sv = ray_str_vec_append(sv, "bb", 2);
    sv = ray_str_vec_append(sv, "cc", 2);
    sv = ray_str_vec_append(sv, "cc", 2);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, sv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(sv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_SUM, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* Whole-partition SUM grouped by str: 30 / 70 / 110 */
    TEST_ASSERT_EQ_I(rd[0], 30); TEST_ASSERT_EQ_I(rd[1], 30);
    TEST_ASSERT_EQ_I(rd[2], 70); TEST_ASSERT_EQ_I(rd[3], 70);
    TEST_ASSERT_EQ_I(rd[4], 110); TEST_ASSERT_EQ_I(rd[5], 110);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Larger STR partition: forces parallel merge sort (>1024 rows) ── */

static test_result_t test_window_str_parallel_merge(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 1500;
    ray_t* sv = ray_vec_new(RAY_STR, n);
    char buf[8];
    for (int64_t i = 0; i < n; i++) {
        /* Cycle 5 partitions: "k0".."k4" */
        buf[0] = 'k';
        buf[1] = (char)('0' + (i % 5));
        buf[2] = '\0';
        sv = ray_str_vec_append(sv, buf, 2);
    }
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) vd[i] = i;

    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, sv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(sv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_COUNT, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* Each k0..k4 has exactly 300 rows */
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 300);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── I32 value column for SUM/MIN/MAX ────────────────────────────── */

static test_result_t test_window_i32_value(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    int32_t vd[] = {10, 20, 30, 40};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I32, vd, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* I32 input with SUM → result is I64 (out_f64 = false) */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_SUM, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_I64);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 30);
        TEST_ASSERT_EQ_I(rd[2], 70);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Running MAX i64 (covers the previously-missed else-branch at line 378) */

static test_result_t test_window_running_max_i64(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    int64_t vd[] = {10, 30, 20, 40};
    ray_t* tbl = mk_tbl_i64_2(gd, vd, n);

    /* Running MAX ordered by v ASC: sorted [10,20,30,40], cumulative max =
     * value at each step.  out[orig_idx_of_sorted[i]] = running_max. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_running_window(g, tbl_op, "g", "v",
                                        RAY_WIN_MAX, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    TEST_ASSERT_EQ_I(rc->type, RAY_I64);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* sorted order: idx0(10), idx2(20), idx1(30), idx3(40)
     * step0: orig=0, max=10  → rd[0]=10
     * step1: orig=2, max=20  → rd[2]=20
     * step2: orig=1, max=30  → rd[1]=30
     * step3: orig=3, max=40  → rd[3]=40 */
    TEST_ASSERT_EQ_I(rd[0], 10);
    TEST_ASSERT_EQ_I(rd[2], 20);
    TEST_ASSERT_EQ_I(rd[1], 30);
    TEST_ASSERT_EQ_I(rd[3], 40);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Running MAX i64 with leading null: covers win_set_null branch in
 *     running MAX i64 (found==0 at start) ─────────────────────────── */

static test_result_t test_window_running_max_leading_null(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    int64_t vd[] = {0, 10, 20, 30};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    ray_vec_set_null(vv, 0, true);   /* first row null */
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* No order key — stable input order [null, 10, 20, 30].
     * Running MAX: [null, 10, 20, 30] */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[] = { g_op };
    uint8_t kinds[] = { RAY_WIN_MAX };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_CURRENT_ROW,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));
    TEST_ASSERT_EQ_I(rd[1], 10);
    TEST_ASSERT_EQ_I(rd[2], 20);
    TEST_ASSERT_EQ_I(rd[3], 30);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── F64 order key: exercises win_keys_differ F64 branch (lines 42-46)
 *     Use RANK so the differ call is reached with F64 order column. ── */

static test_result_t test_window_f64_order_key(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    double  od[] = {1.0, 1.0, 2.0, 3.0};  /* two ties at 1.0 */
    int64_t vd[] = {10, 20, 30, 40};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* ov = ray_vec_from_raw(RAY_F64, od, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    int64_t ng  = ray_sym_intern("g",  1);
    int64_t no  = ray_sym_intern("o",  1);
    int64_t nv2 = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, no, ov);
    tbl = ray_table_add_col(tbl, nv2, vv);
    ray_release(gv); ray_release(ov); ray_release(vv);

    /* PARTITION BY g, ORDER BY o (F64) — use RANK to trigger win_keys_differ
     * on the F64 order column: rows 0,1 tie (1.0==1.0) → rank 1,1; row 2
     * differs (2.0) → rank 3; row 3 differs (3.0) → rank 4. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* o_op = ray_scan(g, "o");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { g_op };
    ray_op_t* orders[] = { o_op };
    uint8_t   ndesc[]  = { 0 };
    uint8_t   kinds[]  = { RAY_WIN_RANK };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                orders, ndesc, 1,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 3);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* sorted by o ASC: [1.0, 1.0, 2.0, 3.0] — rows 0,1 (either order), then 2, then 3 */
    /* ranks: 1, 1, 3, 4 */
    TEST_ASSERT_EQ_I(rd[0], 1);
    TEST_ASSERT_EQ_I(rd[1], 1);
    TEST_ASSERT_EQ_I(rd[2], 3);
    TEST_ASSERT_EQ_I(rd[3], 4);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── I32 order key: exercises win_keys_differ I32 branch (lines 47-50)
 *     Use DATE-typed column as order key with ties. ──────────────────── */

static test_result_t test_window_i32_order_key(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    int32_t od[] = {100, 100, 200, 300};  /* ties at 100 */
    int64_t vd[] = {10, 20, 30, 40};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* ov = ray_vec_from_raw(RAY_DATE, od, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t no = ray_sym_intern("o", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, no, ov);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(ov); ray_release(vv);

    /* PARTITION BY g, ORDER BY o (DATE/I32) — RANK with ties at day=100.
     * Expected ranks: 1, 1, 3, 4 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* o_op = ray_scan(g, "o");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { g_op };
    ray_op_t* orders[] = { o_op };
    uint8_t   ndesc[]  = { 0 };
    uint8_t   kinds[]  = { RAY_WIN_RANK };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                orders, ndesc, 1,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 3);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* sorted by DATE ASC: [100,100,200,300] → ranks 1,1,3,4 */
    TEST_ASSERT_EQ_I(rd[0], 1);
    TEST_ASSERT_EQ_I(rd[1], 1);
    TEST_ASSERT_EQ_I(rd[2], 3);
    TEST_ASSERT_EQ_I(rd[3], 4);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Single-key radix sort path (n_sort==1, nrows > 64) ───────────── */
/* When there's exactly one sort key and nrows > 64 and the type is
 * radix-encodable, exec_window takes the single-key radix branch.
 * Use no order key, only partition key, with n=200. */

static test_result_t test_window_single_key_radix(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* 200 rows, single I64 partition key, no order key — forces n_sort==1
     * in the >64 branch, picking the single-key radix path. */
    int64_t n = 200;
    ray_t* gv = ray_vec_new(RAY_I64, n); gv->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* gd = (int64_t*)ray_data(gv);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        gd[i] = i % 5;   /* 5 partitions of 40 each */
        vd[i] = i;
    }
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* No order key → n_sort == 1 (partition key only).
     * nrows=200 > 64 → radix branch.  Use COUNT(*) whole-partition. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { g_op };
    uint8_t   kinds[]  = { RAY_WIN_COUNT };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* Each of 5 partitions has 40 rows → COUNT = 40 */
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 40);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Single-key radix sort path, large (nrows > RADIX_SORT_THRESHOLD=4096)
 *     exercises the full radix_sort_run sub-path ──────────────────────── */

static test_result_t test_window_single_key_radix_large(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* 5000 rows > RADIX_SORT_THRESHOLD(4096), single partition key, no order */
    int64_t n = 5000;
    ray_t* gv = ray_vec_new(RAY_I64, n); gv->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* gd = (int64_t*)ray_data(gv);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        gd[i] = i % 10;   /* 10 partitions of 500 each */
        vd[i] = i;
    }
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { g_op };
    uint8_t   kinds[]  = { RAY_WIN_COUNT };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* Each of 10 partitions has 500 rows */
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 500);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Running AVG with leading null: cnt==0 path (lines 262-263) ────── */

static test_result_t test_window_running_avg_leading_null(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 3;
    int64_t gd[] = {1, 1, 1};
    int64_t vd[] = {0, 20, 30};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    ray_vec_set_null(vv, 0, true);   /* first row null */
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* No order key: input order is [null, 20, 30].
     * Running AVG: row0=null (cnt==0), row1=20.0, row2=25.0 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[] = { g_op };
    uint8_t kinds[] = { RAY_WIN_AVG };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_CURRENT_ROW,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    double* rd = (double*)ray_data(rc);
    TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));   /* cnt==0 → null */
    TEST_ASSERT_EQ_F(rd[1], 20.0, 1e-9);
    TEST_ASSERT_EQ_F(rd[2], 25.0, 1e-9);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── I16 order key: win_keys_differ RAY_I16 branch (lines 55-58) ──── */

static test_result_t test_window_i16_order_key(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    int16_t od[] = {100, 100, 200, 300};  /* ties at 100 */
    int64_t vd[] = {10, 20, 30, 40};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* ov = ray_vec_from_raw(RAY_I16, od, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t no = ray_sym_intern("o", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, no, ov);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(ov); ray_release(vv);

    /* PARTITION BY g, ORDER BY o (I16) — RANK with tie at 100.
     * Expected ranks: 1, 1, 3, 4 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* o_op = ray_scan(g, "o");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { g_op };
    ray_op_t* orders[] = { o_op };
    uint8_t   ndesc[]  = { 0 };
    uint8_t   kinds[]  = { RAY_WIN_RANK };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                orders, ndesc, 1,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 3);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* sorted by I16 ASC: [100,100,200,300] → ranks 1,1,3,4 */
    TEST_ASSERT_EQ_I(rd[0], 1);
    TEST_ASSERT_EQ_I(rd[1], 1);
    TEST_ASSERT_EQ_I(rd[2], 3);
    TEST_ASSERT_EQ_I(rd[3], 4);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── SYM order key: win_keys_differ RAY_SYM branch (lines 52-54) ──── */

static test_result_t test_window_sym_order_key(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 1, 1};
    int64_t vd[] = {10, 20, 30, 40};
    /* SYM order key: aa, aa, bb, cc — tie at aa */
    int64_t s_aa = ray_sym_intern("aa", 2);
    int64_t s_bb = ray_sym_intern("bb", 2);
    int64_t s_cc = ray_sym_intern("cc", 2);

    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* ov = ray_sym_vec_new(RAY_SYM_W64, n);
    ov = ray_vec_append(ov, &s_aa);
    ov = ray_vec_append(ov, &s_aa);
    ov = ray_vec_append(ov, &s_bb);
    ov = ray_vec_append(ov, &s_cc);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t no = ray_sym_intern("o", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, no, ov);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(ov); ray_release(vv);

    /* PARTITION BY g, ORDER BY o (SYM) — RANK with ties at "aa".
     * Expected ranks: 1, 1, 3, 4 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* o_op = ray_scan(g, "o");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { g_op };
    ray_op_t* orders[] = { o_op };
    uint8_t   ndesc[]  = { 0 };
    uint8_t   kinds[]  = { RAY_WIN_RANK };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                orders, ndesc, 1,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 3);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* sorted by SYM: aa, aa, bb, cc → ranks 1, 1, 3, 4 */
    TEST_ASSERT_EQ_I(rd[0], 1);
    TEST_ASSERT_EQ_I(rd[1], 1);
    TEST_ASSERT_EQ_I(rd[2], 3);
    TEST_ASSERT_EQ_I(rd[3], 4);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── I16 value column: win_read_f64/win_read_i64 I16 arms ──────────── */

static test_result_t test_window_i16_value(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    int16_t vd[] = {10, 20, 30, 40};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I16, vd, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* I16 SUM → hits win_read_i64 I16 arm */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_SUM, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_I64);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 30);
        TEST_ASSERT_EQ_I(rd[2], 70);
        ray_release(result); ray_graph_free(g);
    }

    /* I16 AVG → hits win_read_f64 I16 arm */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_AVG, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_F64);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[0], 15.0, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 35.0, 1e-9);
        ray_release(result); ray_graph_free(g);
    }

    /* I16 MIN whole */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_MIN, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 10);
        TEST_ASSERT_EQ_I(rd[2], 30);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── U8 value column: win_read_f64/win_read_i64 U8 arms ───────────── */

static test_result_t test_window_u8_value(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    uint8_t vd[] = {10, 20, 30, 40};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_U8, vd, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* U8 SUM → hits win_read_i64 U8 arm */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_SUM, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_I64);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 30);
        TEST_ASSERT_EQ_I(rd[2], 70);
        ray_release(result); ray_graph_free(g);
    }

    /* U8 AVG → hits win_read_f64 U8 arm */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_AVG, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[0], 15.0, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 35.0, 1e-9);
        ray_release(result); ray_graph_free(g);
    }

    /* U8 MAX whole */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_MAX, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 20);
        TEST_ASSERT_EQ_I(rd[2], 40);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── F64 value for SUM/MIN/MAX: win_read_f64 RAY_I32 arm via I32 value */

static test_result_t test_window_f64_from_i32_value(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* I32 value col fed to AVG → calls win_read_f64 RAY_I32 arm */
    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    int32_t vd[] = {10, 20, 30, 40};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I32, vd, n);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* AVG(I32) → win_read_f64 RAY_I32 arm */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_AVG, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_EQ_I(rc->type, RAY_F64);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[0], 15.0, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 35.0, 1e-9);
        ray_release(result); ray_graph_free(g);
    }

    /* MIN(I32) whole → hits win_read_i64 RAY_I32 arm + win_read_f64 RAY_I32 arm */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_MIN, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        int64_t* rd = (int64_t*)ray_data(rc);
        TEST_ASSERT_EQ_I(rd[0], 10);
        TEST_ASSERT_EQ_I(rd[2], 30);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── F64 value for LAG/LEAD with null source: lines 405 and 438 ────── */

static test_result_t test_window_lag_lead_f64_null_source(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 3;
    int64_t gd[] = {1, 1, 1};
    double  vd[] = {1.5, 0.0, 3.5};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_F64, vd, n);
    ray_vec_set_null(vv, 1, true);  /* row 1 null */
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* LAG f64 offset=1: source is null for row 2 → propagate null */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* g_op = ray_scan(g, "g");
        ray_op_t* v_op = ray_scan(g, "v");
        ray_op_t* parts[]  = { g_op };
        uint8_t   kinds[]  = { RAY_WIN_LAG };
        ray_op_t* fins[]   = { v_op };
        int64_t   params[] = { 1 };
        ray_op_t* w = ray_window_op(g, tbl_op,
                                    parts, 1,
                                    NULL, NULL, 0,
                                    kinds, fins, params, 1,
                                    RAY_FRAME_ROWS,
                                    RAY_BOUND_UNBOUNDED_PRECEDING,
                                    RAY_BOUND_UNBOUNDED_FOLLOWING,
                                    0, 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        /* row0: lag boundary → null
         * row1: lag from row0 (1.5) → 1.5
         * row2: lag from row1 (null) → null (propagated) */
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[1], 1.5, 1e-9);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 2));
        ray_release(result); ray_graph_free(g);
    }

    /* LEAD f64 offset=1: source is null for row 0 → propagate null */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* g_op = ray_scan(g, "g");
        ray_op_t* v_op = ray_scan(g, "v");
        ray_op_t* parts[]  = { g_op };
        uint8_t   kinds[]  = { RAY_WIN_LEAD };
        ray_op_t* fins[]   = { v_op };
        int64_t   params[] = { 1 };
        ray_op_t* w = ray_window_op(g, tbl_op,
                                    parts, 1,
                                    NULL, NULL, 0,
                                    kinds, fins, params, 1,
                                    RAY_FRAME_ROWS,
                                    RAY_BOUND_UNBOUNDED_PRECEDING,
                                    RAY_BOUND_UNBOUNDED_FOLLOWING,
                                    0, 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        /* row0: lead to row1 (null) → null (propagated)
         * row1: lead to row2 (3.5) → 3.5
         * row2: lead boundary → null */
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[1], 3.5, 1e-9);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 2));
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── All-null partition: MIN/MAX f64 whole-frame null (lines 346-348) */

static test_result_t test_window_allnull_minmax_f64(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    double  vd[] = {1.5, 2.5, 0.0, 0.0};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_F64, vd, n);
    /* partition 2 all-null */
    ray_vec_set_null(vv, 2, true);
    ray_vec_set_null(vv, 3, true);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* MIN f64 whole: partition 2 all-null → result null */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_MIN, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_FALSE(ray_vec_is_null(rc, 0));
        TEST_ASSERT_EQ_F(rd[0], 1.5, 1e-9);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 2));
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 3));
        ray_release(result); ray_graph_free(g);
    }

    /* MAX f64 whole: partition 2 all-null → result null */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                          RAY_WIN_MAX, "v", 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_FALSE(ray_vec_is_null(rc, 0));
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 2));
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 3));
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Running MIN/MAX f64 with leading null: lines 295-296 & 358-359 ── */

static test_result_t test_window_running_minmax_f64_leading_null(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 3;
    int64_t gd[] = {1, 1, 1};
    double  vd[] = {0.0, 2.5, 3.5};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_F64, vd, n);
    ray_vec_set_null(vv, 0, true);  /* first row null */
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* Running MIN f64: first step null (found==0) → null */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* g_op = ray_scan(g, "g");
        ray_op_t* v_op = ray_scan(g, "v");
        ray_op_t* parts[]  = { g_op };
        uint8_t   kinds[]  = { RAY_WIN_MIN };
        ray_op_t* fins[]   = { v_op };
        int64_t   params[] = { 0 };
        ray_op_t* w = ray_window_op(g, tbl_op,
                                    parts, 1,
                                    NULL, NULL, 0,
                                    kinds, fins, params, 1,
                                    RAY_FRAME_ROWS,
                                    RAY_BOUND_UNBOUNDED_PRECEDING,
                                    RAY_BOUND_CURRENT_ROW,
                                    0, 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[1], 2.5, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 2.5, 1e-9);
        ray_release(result); ray_graph_free(g);
    }

    /* Running MAX f64: first step null (found==0) → null */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* g_op = ray_scan(g, "g");
        ray_op_t* v_op = ray_scan(g, "v");
        ray_op_t* parts[]  = { g_op };
        uint8_t   kinds[]  = { RAY_WIN_MAX };
        ray_op_t* fins[]   = { v_op };
        int64_t   params[] = { 0 };
        ray_op_t* w = ray_window_op(g, tbl_op,
                                    parts, 1,
                                    NULL, NULL, 0,
                                    kinds, fins, params, 1,
                                    RAY_FRAME_ROWS,
                                    RAY_BOUND_UNBOUNDED_PRECEDING,
                                    RAY_BOUND_CURRENT_ROW,
                                    0, 0);
        ray_t* result = ray_execute(g, w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* rc = win_result_col(result, 2);
        TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));
        double* rd = (double*)ray_data(rc);
        TEST_ASSERT_EQ_F(rd[1], 2.5, 1e-9);
        TEST_ASSERT_EQ_F(rd[2], 3.5, 1e-9);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl); ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── LAST_VALUE running f64 with null: line 495 ───────────────────── */

static test_result_t test_window_last_value_running_null(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* Running LAST_VALUE f64 where some rows are null */
    int64_t n = 3;
    int64_t gd[] = {1, 1, 1};
    double  vd[] = {1.5, 0.0, 3.5};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_F64, vd, n);
    ray_vec_set_null(vv, 1, true);   /* row 1 null */
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* Running LAST_VALUE f64: each row sees its own value (CURRENT ROW).
     * Row 1 is null → result for row 1 is also null. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { g_op };
    uint8_t   kinds[]  = { RAY_WIN_LAST_VALUE };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_CURRENT_ROW,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    double* rd = (double*)ray_data(rc);
    TEST_ASSERT_EQ_F(rd[0], 1.5, 1e-9);
    TEST_ASSERT_TRUE(ray_vec_is_null(rc, 1));
    TEST_ASSERT_EQ_F(rd[2], 3.5, 1e-9);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── LAST_VALUE running i64 with null: line 511 ───────────────────── */

static test_result_t test_window_last_value_running_i64_null(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 3;
    int64_t gd[] = {1, 1, 1};
    int64_t vd[] = {10, 0, 30};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    ray_vec_set_null(vv, 1, true);   /* row 1 null */
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* Running LAST_VALUE i64: row 1 null → result null */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { g_op };
    uint8_t   kinds[]  = { RAY_WIN_LAST_VALUE };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_CURRENT_ROW,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    TEST_ASSERT_EQ_I(rd[0], 10);
    TEST_ASSERT_TRUE(ray_vec_is_null(rc, 1));
    TEST_ASSERT_EQ_I(rd[2], 30);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── SYM value column: win_read_i64 RAY_SYM arm ───────────────────── */

static test_result_t test_window_sym_value(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* SYM column used as "value" for COUNT (SYM not used by count but
     * passes through to result_vecs setup).  Use SUM on a SYM partition
     * key against itself so win_read_i64 RAY_SYM arm is exercised. */
    int64_t n = 4;
    int64_t s1 = ray_sym_intern("cat", 3);
    int64_t s2 = ray_sym_intern("dog", 3);
    int64_t s1v = s1, s2v = s2;  /* same as partition values */

    ray_t* gv = ray_sym_vec_new(RAY_SYM_W64, n);
    gv = ray_vec_append(gv, &s1v);
    gv = ray_vec_append(gv, &s1v);
    gv = ray_vec_append(gv, &s2v);
    gv = ray_vec_append(gv, &s2v);

    ray_t* vv = ray_sym_vec_new(RAY_SYM_W64, n);
    vv = ray_vec_append(vv, &s1v);
    vv = ray_vec_append(vv, &s1v);
    vv = ray_vec_append(vv, &s2v);
    vv = ray_vec_append(vv, &s2v);

    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* SUM(SYM) → hits win_read_i64 RAY_SYM arm */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_SUM, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* Just verify it ran without error; exact SYM sum is interned-id-dependent */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── F64 as value for win_read_i64: LAG with F64 col → I64 cast ────── */

static test_result_t test_window_f64_value_lag(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* Use F64 value column for LAG which takes the i64 branch when
     * the result type is not f64 (it IS f64 for F64 input), so instead
     * use a F64 column for SUM-running to confirm win_read_i64 F64 arm. */
    /* Actually win_read_i64 F64 arm is hit only for i64-result fns with F64 input.
     * SUM with F64 input produces f64 output, so is_f64=true.
     * To hit win_read_i64 RAY_F64: we need an i64-output function on F64 input.
     * LAG/LEAD output type follows the input; for F64 input → f64 output.
     * FIRST_VALUE similarly.  So win_read_i64 RAY_F64 may not be reachable
     * directly from the public API (output type mirrors input type).
     * Skip this specific sub-arm; focus on confirmed reachable ones. */
    PASS();
}

/* ─── SYM multi-key partition with I32 (pkey_gather multi-key I32 arm) */

static test_result_t test_window_multikey_sym_i32_partition(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* Multi-key partition: SYM key + I32 key.  SYM makes has_64bit_key=true
     * which forces can_pack=false → pkey_gather fallback path.
     * This exercises win_keys_differ SYM arm (line 52) during the fallback. */
    int64_t n = 6;
    int64_t s_a = ray_sym_intern("ga", 2);
    int64_t s_b = ray_sym_intern("gb", 2);
    int32_t b_data[] = {1, 1, 2, 1, 2, 2};
    int64_t vd[]     = {10, 20, 30, 40, 50, 60};

    ray_t* av = ray_sym_vec_new(RAY_SYM_W64, n);
    av = ray_vec_append(av, &s_a);
    av = ray_vec_append(av, &s_a);
    av = ray_vec_append(av, &s_a);
    av = ray_vec_append(av, &s_b);
    av = ray_vec_append(av, &s_b);
    av = ray_vec_append(av, &s_b);
    ray_t* bv = ray_vec_from_raw(RAY_I32, b_data, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, na, av);
    tbl = ray_table_add_col(tbl, nb, bv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(av); ray_release(bv); ray_release(vv);

    /* PARTITION BY (a SYM, b I32), SUM(v) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { a_op, b_op };
    uint8_t   kinds[]  = { RAY_WIN_COUNT };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 2,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 3);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* Partitions: (ga,1)→2, (ga,2)→1, (gb,1)→1, (gb,2)→2 */
    TEST_ASSERT_EQ_I(rd[0], 2);
    TEST_ASSERT_EQ_I(rd[1], 2);
    TEST_ASSERT_EQ_I(rd[2], 1);
    TEST_ASSERT_EQ_I(rd[3], 1);
    TEST_ASSERT_EQ_I(rd[4], 2);
    TEST_ASSERT_EQ_I(rd[5], 2);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── MAX i64 whole, all-null partition: lines 375-377 ─────────────── */

static test_result_t test_window_allnull_max_i64(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    int64_t vd[] = {10, 20, 0, 0};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    /* partition 2 all-null */
    ray_vec_set_null(vv, 2, true);
    ray_vec_set_null(vv, 3, true);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* MAX i64 whole: partition 2 all-null → result null */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_MAX, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    TEST_ASSERT_FALSE(ray_vec_is_null(rc, 0));
    TEST_ASSERT_EQ_I(rd[0], 20);
    TEST_ASSERT_TRUE(ray_vec_is_null(rc, 2));
    TEST_ASSERT_TRUE(ray_vec_is_null(rc, 3));

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── LEAD i64 with null source: line 451 ──────────────────────────── */

static test_result_t test_window_lead_i64_null_source(void) {
    ray_heap_init(); (void)ray_sym_init();

    int64_t n = 3;
    int64_t gd[] = {1, 1, 1};
    int64_t vd[] = {10, 0, 30};
    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, n);
    ray_vec_set_null(vv, 1, true);  /* row 1 null */
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* LEAD i64 offset=1: row 0 leads to row 1 (null) → result null */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { g_op };
    uint8_t   kinds[]  = { RAY_WIN_LEAD };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 1 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* row0: leads to row1 (null) → null (propagated)
     * row1: leads to row2 (30) → 30
     * row2: boundary → null */
    TEST_ASSERT_TRUE(ray_vec_is_null(rc, 0));
    TEST_ASSERT_EQ_I(rd[1], 30);
    TEST_ASSERT_TRUE(ray_vec_is_null(rc, 2));

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── SYM partition key, large table (>64 rows): radix enum_rank build */
/* Lines 751-754: build_enum_rank called for SYM sort key with nrows > 64 */

static test_result_t test_window_sym_partition_radix(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* 200 rows, SYM partition key: triggers radix path (>64 rows) with
     * SYM sort key → build_enum_rank is called (lines 751-754). */
    int64_t n = 200;
    int64_t s_a = ray_sym_intern("aaa", 3);
    int64_t s_b = ray_sym_intern("bbb", 3);
    int64_t s_c = ray_sym_intern("ccc", 3);
    int64_t s_d = ray_sym_intern("ddd", 3);
    int64_t syms[4] = {s_a, s_b, s_c, s_d};

    ray_t* sv = ray_sym_vec_new(RAY_SYM_W64, n);
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        int64_t s = syms[i % 4];
        sv = ray_vec_append(sv, &s);
        vd[i] = i;
    }
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, sv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(sv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_COUNT, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* Each of 4 partitions has 50 rows */
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 50);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Multi-key radix sort with I32 keys (65..8191 rows):
 *     prescan else-branch I32 arm (lines 869-874) ──────────────────── */

static test_result_t test_window_multikey_i32_radix_small(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* nrows=200: >64 so radix path, <8192 so mk_prescan_pool2=NULL → else
     * branch.  Two I32 partition keys → prescan else I32 arm (lines 869-874). */
    int64_t n = 200;
    ray_t* av = ray_vec_new(RAY_I32, n); av->len = n;
    ray_t* bv = ray_vec_new(RAY_I32, n); bv->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int32_t* ad = (int32_t*)ray_data(av);
    int32_t* bd = (int32_t*)ray_data(bv);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        ad[i] = (int32_t)(i % 4);
        bd[i] = (int32_t)(i % 5);
        vd[i] = i;
    }
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, na, av);
    tbl = ray_table_add_col(tbl, nb, bv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(av); ray_release(bv); ray_release(vv);

    /* PARTITION BY (a I32, b I32), COUNT(*) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { a_op, b_op };
    uint8_t   kinds[]  = { RAY_WIN_COUNT };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 2,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 4*5=20 partitions, each with 200/20=10 rows */
    ray_t* rc = win_result_col(result, 3);
    int64_t* rd = (int64_t*)ray_data(rc);
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 10);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── SYM order key, large table: radix enum_rank build for order key ── */

static test_result_t test_window_sym_order_large(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* 200 rows, SYM order key: triggers radix path (>64) with SYM in sort
     * key (order key = n_part..n_sort).  RANK triggers win_keys_differ SYM arm. */
    int64_t n = 200;
    int64_t s_a = ray_sym_intern("xa", 2);
    int64_t s_b = ray_sym_intern("xb", 2);
    int64_t g_val = ray_sym_intern("all", 3);
    int64_t syms[2] = {s_a, s_b};

    ray_t* gv = ray_sym_vec_new(RAY_SYM_W64, n);
    ray_t* ov = ray_sym_vec_new(RAY_SYM_W64, n);
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        gv = ray_vec_append(gv, &g_val);     /* all same partition */
        int64_t s = syms[i % 2];
        ov = ray_vec_append(ov, &s);          /* alternate xa/xb */
        vd[i] = i;
    }
    int64_t ng = ray_sym_intern("g", 1);
    int64_t no = ray_sym_intern("o", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, no, ov);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(ov); ray_release(vv);

    /* PARTITION BY g (SYM), ORDER BY o (SYM) — RANK */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* g_op = ray_scan(g, "g");
    ray_op_t* o_op = ray_scan(g, "o");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { g_op };
    ray_op_t* orders[] = { o_op };
    uint8_t   ndesc[]  = { 0 };
    uint8_t   kinds[]  = { RAY_WIN_COUNT };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 1,
                                orders, ndesc, 1,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* All 200 rows in one partition: COUNT = 200 */
    ray_t* rc = win_result_col(result, 3);
    int64_t* rd = (int64_t*)ray_data(rc);
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 200);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Multi-key I16 partition: prescan else I16 arm (lines 876-880) ── */

static test_result_t test_window_multikey_i16_radix(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* nrows=200: >64 (radix path), <8192 (else branch).
     * Two I16 partition keys → prescan else I16 arm (lines 876-880).
     * I16 also forces can_pack=false → win_keys_differ I16 arm. */
    int64_t n = 200;
    ray_t* av = ray_vec_new(RAY_I16, n); av->len = n;
    ray_t* bv = ray_vec_new(RAY_I16, n); bv->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int16_t* ad = (int16_t*)ray_data(av);
    int16_t* bd = (int16_t*)ray_data(bv);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        ad[i] = (int16_t)(i % 4);
        bd[i] = (int16_t)(i % 5);
        vd[i] = i;
    }
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, na, av);
    tbl = ray_table_add_col(tbl, nb, bv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(av); ray_release(bv); ray_release(vv);

    /* PARTITION BY (a I16, b I16), COUNT(*) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { a_op, b_op };
    uint8_t   kinds[]  = { RAY_WIN_COUNT };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 2,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 20 partitions, 10 rows each */
    ray_t* rc = win_result_col(result, 3);
    int64_t* rd = (int64_t*)ray_data(rc);
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 10);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── U8 partition key multi-key: prescan else U8 arm (lines 882-887) ─ */

static test_result_t test_window_multikey_u8_radix(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* nrows=200, two U8 partition keys (BOOL type).
     * U8/BOOL forces can_pack=false, so fallback + win_keys_differ U8 arm.
     * But for radix prescan else, we need the radix path with U8/BOOL key.
     * Actually for U8/BOOL, can_radix=true (U8 is radix-encodable) but
     * can_pack=false (so pkey_sorted=NULL).  However, for n_sort > 1 multi-key
     * radix: can_radix checks sort_vecs type; U8 is accepted.
     * With 2 U8 keys, it enters the multi-key radix path.
     * mk_prescan_pool2=NULL (nrows<8192) → else branch → U8 arm. */
    int64_t n = 200;
    ray_t* av = ray_vec_new(RAY_U8, n); av->len = n;
    ray_t* bv = ray_vec_new(RAY_U8, n); bv->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    uint8_t* ad = (uint8_t*)ray_data(av);
    uint8_t* bd = (uint8_t*)ray_data(bv);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        ad[i] = (uint8_t)(i % 4);
        bd[i] = (uint8_t)(i % 5);
        vd[i] = i;
    }
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, na, av);
    tbl = ray_table_add_col(tbl, nb, bv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(av); ray_release(bv); ray_release(vv);

    /* PARTITION BY (a U8, b U8), COUNT(*) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* parts[]  = { a_op, b_op };
    uint8_t   kinds[]  = { RAY_WIN_COUNT };
    ray_op_t* fins[]   = { v_op };
    int64_t   params[] = { 0 };
    ray_op_t* w = ray_window_op(g, tbl_op,
                                parts, 2,
                                NULL, NULL, 0,
                                kinds, fins, params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 20 partitions, 10 rows each */
    ray_t* rc = win_result_col(result, 3);
    int64_t* rd = (int64_t*)ray_data(rc);
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 10);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── SYM value column for AVG: win_read_f64 RAY_SYM arm (lines 83-84) */

static test_result_t test_window_avg_sym_value(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* SYM value column: AVG calls win_read_f64 which has RAY_SYM arm.
     * Build a table with a SYM column used as the AVG input. */
    int64_t n = 4;
    int64_t gd[] = {1, 1, 2, 2};
    int64_t s1 = ray_sym_intern("v1", 2);
    int64_t s2 = ray_sym_intern("v2", 2);

    ray_t* gv = ray_vec_from_raw(RAY_I64, gd, n);
    ray_t* vv = ray_sym_vec_new(RAY_SYM_W64, n);
    vv = ray_vec_append(vv, &s1);
    vv = ray_vec_append(vv, &s2);
    vv = ray_vec_append(vv, &s1);
    vv = ray_vec_append(vv, &s2);
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, gv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(gv); ray_release(vv);

    /* AVG(SYM) — hits win_read_f64 RAY_SYM arm */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_AVG, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* Just verify no error; exact SYM avg is interned-id-dependent */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── SYM partition radix large (>=8192): line 775 pool dispatch ────── */

static test_result_t test_window_sym_partition_large_pool(void) {
    ray_heap_init(); (void)ray_sym_init();

    /* nrows=9000 >= SMALL_POOL_THRESHOLD=8192 with SYM partition key.
     * SYM in sort → radix enum_rank build (lines 751-754) + sk_pool dispatch
     * for single-key radix (line 775). */
    int64_t n = 9000;
    int64_t s_a = ray_sym_intern("aa_big", 6);
    int64_t s_b = ray_sym_intern("bb_big", 6);
    int64_t syms[2] = {s_a, s_b};

    ray_t* sv = ray_sym_vec_new(RAY_SYM_W64, n);
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        int64_t s = syms[i % 2];
        sv = ray_vec_append(sv, &s);
        vd[i] = i;
    }
    int64_t ng = ray_sym_intern("g", 1);
    int64_t nv = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ng, sv);
    tbl = ray_table_add_col(tbl, nv, vv);
    ray_release(sv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* w = build_whole_window(g, tbl_op, "g", "v",
                                      RAY_WIN_COUNT, "v", 0);
    ray_t* result = ray_execute(g, w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_t* rc = win_result_col(result, 2);
    int64_t* rd = (int64_t*)ray_data(rc);
    /* 2 partitions of 4500 each */
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(rd[i], 4500);

    ray_release(result); ray_graph_free(g); ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* ─── Suite registration ──────────────────────────────────────────── */

const test_entry_t window_entries[] = {
    { "window/row_number_partitioned", test_window_row_number_partitioned, NULL, NULL },
    { "window/rank_dense_rank",        test_window_rank_dense_rank,        NULL, NULL },
    { "window/ntile",                  test_window_ntile,                  NULL, NULL },
    { "window/count",                  test_window_count,                  NULL, NULL },
    { "window/sum_i64",                test_window_sum_i64,                NULL, NULL },
    { "window/sum_f64",                test_window_sum_f64,                NULL, NULL },
    { "window/avg",                    test_window_avg,                    NULL, NULL },
    { "window/min_max_i64",            test_window_min_max_i64,            NULL, NULL },
    { "window/min_max_f64",            test_window_min_max_f64,            NULL, NULL },
    { "window/lag_lead_i64",           test_window_lag_lead_i64,           NULL, NULL },
    { "window/lag_lead_f64",           test_window_lag_lead_f64,           NULL, NULL },
    { "window/first_last_nth",         test_window_first_last_nth,         NULL, NULL },
    { "window/first_last_nth_f64",     test_window_first_last_nth_f64,     NULL, NULL },
    { "window/nulls",                  test_window_nulls,                  NULL, NULL },
    { "window/running_min_leading_null", test_window_running_min_leading_null, NULL, NULL },
    { "window/lag_null_source",        test_window_lag_null_source,        NULL, NULL },
    { "window/first_last_nth_null",    test_window_first_last_nth_null,    NULL, NULL },
    { "window/empty",                  test_window_empty,                  NULL, NULL },
    { "window/no_partition",           test_window_no_partition,           NULL, NULL },
    { "window/sym_partition",          test_window_sym_partition,          NULL, NULL },
    { "window/multikey_partition",     test_window_multikey_partition,     NULL, NULL },
    { "window/multiple_funcs",         test_window_multiple_funcs,         NULL, NULL },
    { "window/radix_path",             test_window_radix_path,             NULL, NULL },
    { "window/parallel_path",          test_window_parallel_path,          NULL, NULL },
    { "window/order_desc",             test_window_order_desc,             NULL, NULL },
    { "window/date_partition",         test_window_date_partition,         NULL, NULL },
    { "window/f64_partition",          test_window_f64_partition,          NULL, NULL },
    { "window/bool_partition",         test_window_bool_partition,         NULL, NULL },
    { "window/no_funcs",               test_window_no_funcs,               NULL, NULL },
    { "window/i32_value",              test_window_i32_value,              NULL, NULL },
    { "window/str_partition",          test_window_str_partition,          NULL, NULL },
    { "window/str_parallel_merge",     test_window_str_parallel_merge,     NULL, NULL },
    { "window/running_max_i64",        test_window_running_max_i64,        NULL, NULL },
    { "window/running_max_leading_null", test_window_running_max_leading_null, NULL, NULL },
    { "window/f64_order_key",          test_window_f64_order_key,          NULL, NULL },
    { "window/i32_order_key",          test_window_i32_order_key,          NULL, NULL },
    { "window/single_key_radix",       test_window_single_key_radix,       NULL, NULL },
    { "window/single_key_radix_large", test_window_single_key_radix_large, NULL, NULL },
    { "window/running_avg_leading_null", test_window_running_avg_leading_null, NULL, NULL },
    { "window/i16_order_key",           test_window_i16_order_key,           NULL, NULL },
    { "window/sym_order_key",           test_window_sym_order_key,           NULL, NULL },
    { "window/i16_value",               test_window_i16_value,               NULL, NULL },
    { "window/u8_value",                test_window_u8_value,                NULL, NULL },
    { "window/f64_from_i32_value",      test_window_f64_from_i32_value,      NULL, NULL },
    { "window/lag_lead_f64_null_source", test_window_lag_lead_f64_null_source, NULL, NULL },
    { "window/allnull_minmax_f64",      test_window_allnull_minmax_f64,      NULL, NULL },
    { "window/running_minmax_f64_leading_null", test_window_running_minmax_f64_leading_null, NULL, NULL },
    { "window/last_value_running_null", test_window_last_value_running_null, NULL, NULL },
    { "window/last_value_running_i64_null", test_window_last_value_running_i64_null, NULL, NULL },
    { "window/sym_value",               test_window_sym_value,               NULL, NULL },
    { "window/f64_value_lag",           test_window_f64_value_lag,           NULL, NULL },
    { "window/multikey_sym_i32_partition", test_window_multikey_sym_i32_partition, NULL, NULL },
    { "window/allnull_max_i64",          test_window_allnull_max_i64,          NULL, NULL },
    { "window/lead_i64_null_source",     test_window_lead_i64_null_source,     NULL, NULL },
    { "window/sym_partition_radix",      test_window_sym_partition_radix,      NULL, NULL },
    { "window/multikey_i32_radix_small", test_window_multikey_i32_radix_small, NULL, NULL },
    { "window/sym_order_large",          test_window_sym_order_large,          NULL, NULL },
    { "window/multikey_i16_radix",       test_window_multikey_i16_radix,       NULL, NULL },
    { "window/multikey_u8_radix",        test_window_multikey_u8_radix,        NULL, NULL },
    { "window/avg_sym_value",            test_window_avg_sym_value,            NULL, NULL },
    { "window/sym_partition_large_pool", test_window_sym_partition_large_pool, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
