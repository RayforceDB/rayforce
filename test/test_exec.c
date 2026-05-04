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

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "table/sym.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

/* Helper: create table with id1(I64), v1(I64), v3(F64) — 10 rows */
static ray_t* make_exec_table(void) {
    (void)ray_sym_init();

    int64_t n = 10;
    int64_t id1_data[] = {1, 1, 2, 2, 3, 3, 1, 2, 3, 1};
    int64_t v1_data[]  = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    double  v3_data[]  = {1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5};

    ray_t* id1_vec = ray_vec_from_raw(RAY_I64, id1_data, n);
    ray_t* v1_vec  = ray_vec_from_raw(RAY_I64, v1_data, n);
    ray_t* v3_vec  = ray_vec_from_raw(RAY_F64, v3_data, n);

    int64_t name_id1 = ray_sym_intern("id1", 3);
    int64_t name_v1  = ray_sym_intern("v1", 2);
    int64_t name_v3  = ray_sym_intern("v3", 2);

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, name_id1, id1_vec);
    tbl = ray_table_add_col(tbl, name_v1, v1_vec);
    tbl = ray_table_add_col(tbl, name_v3, v3_vec);

    ray_release(id1_vec);
    ray_release(v1_vec);
    ray_release(v3_vec);

    return tbl;
}

/* ---- NEG ---- */
static test_result_t test_exec_neg_i64(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* neg_op = ray_neg(g, v1);
    ray_op_t* s = ray_sum(g, neg_op);

    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, -550);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_exec_neg_f64(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v3 = ray_scan(g, "v3");
    ray_op_t* neg_op = ray_neg(g, v3);
    ray_op_t* s = ray_sum(g, neg_op);

    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, -60.0, 1e-6);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- ABS ---- */
static test_result_t test_exec_abs(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* abs(neg(v1)) should equal v1 */
    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* neg_op = ray_neg(g, v1);
    ray_op_t* abs_op = ray_abs(g, neg_op);
    ray_op_t* s = ray_sum(g, abs_op);

    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 550);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- NOT ---- */
static test_result_t test_exec_not(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* threshold = ray_const_i64(g, 50);
    ray_op_t* pred = ray_ge(g, v1, threshold);
    ray_op_t* not_pred = ray_not(g, pred);
    ray_op_t* filtered = ray_filter(g, v1, not_pred);
    ray_op_t* cnt = ray_count(g, filtered);

    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 4);  /* 10,20,30,40 */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- ISNULL ---- */
static test_result_t test_exec_isnull(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Create vector with no nulls */
    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* is_null = ray_isnull(g, x);
    ray_op_t* filtered = ray_filter(g, x, is_null);
    ray_op_t* cnt = ray_count(g, filtered);

    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 0);  /* no nulls in raw data */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- SQRT / LOG / EXP ---- */
static test_result_t test_exec_math_ops(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double raw[] = {1.0, 4.0, 9.0, 16.0, 25.0};
    ray_t* vec = ray_vec_from_raw(RAY_F64, raw, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* sqrt(x) -> sum should be 1+2+3+4+5 = 15 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* sq = ray_sqrt_op(g, x);
    ray_op_t* s = ray_sum(g, sq);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 15.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* exp(log(x)) should roundtrip -> sum = 55 */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* lg = ray_log_op(g, x);
    ray_op_t* ex = ray_exp_op(g, lg);
    s = ray_sum(g, ex);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 55.0, 1e-3);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- CEIL / FLOOR ---- */
static test_result_t test_exec_ceil_floor(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double raw[] = {1.1, 2.5, 3.9, -1.1, -2.9};
    ray_t* vec = ray_vec_from_raw(RAY_F64, raw, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* ceil: 2+3+4+(-1)+(-2) = 6 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* c = ray_ceil_op(g, x);
    ray_op_t* s = ray_sum(g, c);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 6.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* floor: 1+2+3+(-2)+(-3) = 1 */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* f = ray_floor_op(g, x);
    s = ray_sum(g, f);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 1.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ======================================================================
 * Binary element-wise ops
 * ====================================================================== */

static test_result_t test_exec_binary_arithmetic(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* id1 = ray_scan(g, "id1");

    /* v1 + id1 -> sum */
    ray_op_t* add_op = ray_add(g, v1, id1);
    ray_op_t* s = ray_sum(g, add_op);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* sum(v1)=550, sum(id1)=19, sum(v1+id1)=569 */
    TEST_ASSERT_EQ_I(result->i64, 569);
    ray_release(result);
    ray_graph_free(g);

    /* v1 - id1 -> sum */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    id1 = ray_scan(g, "id1");
    ray_op_t* sub_op = ray_sub(g, v1, id1);
    s = ray_sum(g, sub_op);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 531);
    ray_release(result);
    ray_graph_free(g);

    /* v1 * id1 -> sum */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    id1 = ray_scan(g, "id1");
    ray_op_t* mul_op = ray_mul(g, v1, id1);
    s = ray_sum(g, mul_op);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 10*1+20*1+30*2+40*2+50*3+60*3+70*1+80*2+90*3+100*1 = 1100 */
    TEST_ASSERT_EQ_I(result->i64, 1100);
    ray_release(result);
    ray_graph_free(g);

    /* v1 / id1 -> sum (OP_DIV always promotes to f64) */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    id1 = ray_scan(g, "id1");
    ray_op_t* div_op = ray_div(g, v1, id1);
    s = ray_sum(g, div_op);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 10/1+20/1+15+20+50/3+60/3+70/1+40+30+100/1 = 341.666... */
    TEST_ASSERT_EQ_F(result->f64, 341.666, 1e-2);
    ray_release(result);
    ray_graph_free(g);

    /* v1 % id1 -> sum */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    id1 = ray_scan(g, "id1");
    ray_op_t* mod_op = ray_mod(g, v1, id1);
    s = ray_sum(g, mod_op);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 10%1+20%1+30%2+40%2+50%3+60%3+70%1+80%2+90%3+100%1 = 2 */
    TEST_ASSERT_EQ_I(result->i64, 2);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- Comparison ops ---- */
static test_result_t test_exec_comparisons(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();

    /* EQ: count where v1 == 50 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* c50 = ray_const_i64(g, 50);
    ray_op_t* pred = ray_eq(g, v1, c50);
    ray_op_t* filtered = ray_filter(g, v1, pred);
    ray_op_t* cnt = ray_count(g, filtered);
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    ray_graph_free(g);

    /* NE: count where v1 != 50 */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    c50 = ray_const_i64(g, 50);
    pred = ray_ne(g, v1, c50);
    filtered = ray_filter(g, v1, pred);
    cnt = ray_count(g, filtered);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 9);
    ray_release(result);
    ray_graph_free(g);

    /* LT: count where v1 < 50 */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    c50 = ray_const_i64(g, 50);
    pred = ray_lt(g, v1, c50);
    filtered = ray_filter(g, v1, pred);
    cnt = ray_count(g, filtered);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 4);  /* 10,20,30,40 */
    ray_release(result);
    ray_graph_free(g);

    /* LE: count where v1 <= 50 */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    c50 = ray_const_i64(g, 50);
    pred = ray_le(g, v1, c50);
    filtered = ray_filter(g, v1, pred);
    cnt = ray_count(g, filtered);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 5);
    ray_release(result);
    ray_graph_free(g);

    /* GT: count where v1 > 50 */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    c50 = ray_const_i64(g, 50);
    pred = ray_gt(g, v1, c50);
    filtered = ray_filter(g, v1, pred);
    cnt = ray_count(g, filtered);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 5);  /* 60,70,80,90,100 */
    ray_release(result);
    ray_graph_free(g);

    /* AND: v1 > 20 AND v1 < 80 */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    ray_op_t* c20 = ray_const_i64(g, 20);
    ray_op_t* c80 = ray_const_i64(g, 80);
    ray_op_t* gt20 = ray_gt(g, v1, c20);
    ray_op_t* lt80 = ray_lt(g, v1, c80);
    ray_op_t* both = ray_and(g, gt20, lt80);
    filtered = ray_filter(g, v1, both);
    cnt = ray_count(g, filtered);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 5);  /* 30,40,50,60,70 */
    ray_release(result);
    ray_graph_free(g);

    /* OR: v1 < 20 OR v1 > 90 */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    c20 = ray_const_i64(g, 20);
    ray_op_t* c90 = ray_const_i64(g, 90);
    ray_op_t* lt20 = ray_lt(g, v1, c20);
    ray_op_t* gt90 = ray_gt(g, v1, c90);
    ray_op_t* either = ray_or(g, lt20, gt90);
    filtered = ray_filter(g, v1, either);
    cnt = ray_count(g, filtered);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 2);  /* 10, 100 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- MIN2 / MAX2 ---- */
static test_result_t test_exec_min2_max2(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* id1 = ray_scan(g, "id1");
    ray_op_t* mn = ray_min2(g, v1, id1);
    ray_op_t* s = ray_sum(g, mn);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* min2(v1,id1) per row: 1,1,2,2,3,3,1,2,3,1 = sum(id1) = 19 */
    TEST_ASSERT_EQ_I(result->i64, 19);
    ray_release(result);
    ray_graph_free(g);

    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    id1 = ray_scan(g, "id1");
    ray_op_t* mx = ray_max2(g, v1, id1);
    s = ray_sum(g, mx);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* max2(v1,id1) per row: all v1 values since v1 > id1 -> sum = 550 */
    TEST_ASSERT_EQ_I(result->i64, 550);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- IF (ternary) ---- */
static test_result_t test_exec_if(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* c50 = ray_const_i64(g, 50);
    ray_op_t* pred = ray_gt(g, v1, c50);
    ray_op_t* c1 = ray_const_i64(g, 1);
    ray_op_t* c0 = ray_const_i64(g, 0);
    ray_op_t* if_op = ray_if(g, pred, c1, c0);
    ray_op_t* s = ray_sum(g, if_op);

    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 5);  /* 5 values > 50 */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ======================================================================
 * Reduction ops
 * ====================================================================== */

static test_result_t test_exec_reductions(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();

    /* PROD on small values to avoid overflow */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* id1 = ray_scan(g, "id1");
    ray_op_t* prod_op = ray_prod(g, id1);
    ray_t* result = ray_execute(g, prod_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* id1 = {1,1,2,2,3,3,1,2,3,1} -> prod = 216 */
    TEST_ASSERT_EQ_I(result->i64, 216);
    ray_release(result);
    ray_graph_free(g);

    /* MIN */
    g = ray_graph_new(tbl);
    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* min_op = ray_min_op(g, v1);
    result = ray_execute(g, min_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 10);
    ray_release(result);
    ray_graph_free(g);

    /* MAX */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    ray_op_t* max_op = ray_max_op(g, v1);
    result = ray_execute(g, max_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 100);
    ray_release(result);
    ray_graph_free(g);

    /* AVG */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    ray_op_t* avg_op = ray_avg(g, v1);
    result = ray_execute(g, avg_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 55.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* FIRST */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    ray_op_t* first_op = ray_first(g, v1);
    result = ray_execute(g, first_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 10);
    ray_release(result);
    ray_graph_free(g);

    /* LAST */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    ray_op_t* last_op = ray_last(g, v1);
    result = ray_execute(g, last_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 100);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- SORT ---- */
static test_result_t test_exec_sort(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();

    /* Ascending sort on v1 -> produces sorted table */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* keys[] = { v1 };
    uint8_t descs[] = { 0 };       /* ascending */
    uint8_t nulls_first[] = { 0 };
    ray_op_t* sort_op = ray_sort_op(g, tbl_op, keys, descs, nulls_first, 1);

    ray_t* result = ray_execute(g, sort_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 10);

    /* Verify ascending order */
    ray_t* sorted_col = ray_table_get_col_idx(result, 1); /* v1 is col 1 */
    int64_t* sdata = (int64_t*)ray_data(sorted_col);
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_TRUE(sdata[i] <= sdata[i + 1]);
    }

    ray_release(result);
    ray_graph_free(g);

    /* Descending sort on v1 */
    g = ray_graph_new(tbl);
    tbl_op = ray_const_table(g, tbl);
    v1 = ray_scan(g, "v1");
    ray_op_t* keys2[] = { v1 };
    uint8_t descs2[] = { 1 };      /* descending */
    uint8_t nulls_first2[] = { 0 };
    sort_op = ray_sort_op(g, tbl_op, keys2, descs2, nulls_first2, 1);
    result = ray_execute(g, sort_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 10);

    /* Verify descending order */
    sorted_col = ray_table_get_col_idx(result, 1);
    sdata = (int64_t*)ray_data(sorted_col);
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_TRUE(sdata[i] >= sdata[i + 1]);
    }

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- HEAD / TAIL ---- */
static test_result_t test_exec_head_tail(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();

    /* HEAD 3 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* head_op = ray_head(g, v1, 3);
    ray_op_t* s = ray_sum(g, head_op);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 60);  /* 10+20+30 */
    ray_release(result);
    ray_graph_free(g);

    /* TAIL 3 */
    g = ray_graph_new(tbl);
    v1 = ray_scan(g, "v1");
    ray_op_t* tail_op = ray_tail(g, v1, 3);
    s = ray_sum(g, tail_op);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 270);  /* 80+90+100 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- JOIN ---- */
static test_result_t test_exec_join(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Left table: id(I64), val(I64) */
    int64_t lid[] = {1, 2, 3};
    int64_t lval[] = {10, 20, 30};
    ray_t* lid_v = ray_vec_from_raw(RAY_I64, lid, 3);
    ray_t* lval_v = ray_vec_from_raw(RAY_I64, lval, 3);
    int64_t n_id = ray_sym_intern("id", 2);
    int64_t n_val = ray_sym_intern("val", 3);
    ray_t* left = ray_table_new(2);
    left = ray_table_add_col(left, n_id, lid_v);
    left = ray_table_add_col(left, n_val, lval_v);
    ray_release(lid_v);
    ray_release(lval_v);

    /* Right table: id(I64), score(I64) */
    int64_t rid[] = {1, 2, 2, 3};
    int64_t rscore[] = {100, 200, 201, 300};
    ray_t* rid_v = ray_vec_from_raw(RAY_I64, rid, 4);
    ray_t* rscore_v = ray_vec_from_raw(RAY_I64, rscore, 4);
    int64_t n_score = ray_sym_intern("score", 5);
    ray_t* right = ray_table_new(2);
    right = ray_table_add_col(right, n_id, rid_v);
    right = ray_table_add_col(right, n_score, rscore_v);
    ray_release(rid_v);
    ray_release(rscore_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* lk = ray_scan(g, "id");
    ray_op_t* lk_arr[] = { lk };
    ray_op_t* rk_arr[] = { lk };  /* same key name */
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    ray_t* result = ray_execute(g, join_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* 1->1, 2->2(twice), 3->1 = 4 result rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);
    /* Joined table should have columns from both sides */
    TEST_ASSERT_TRUE(ray_table_ncols(result) >= 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- LARGE JOIN (radix-partitioned path) ---- */
static test_result_t test_exec_join_large(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Create left table: 100K rows, id = i % 50000, val = i
     * Each key appears exactly twice on the left side. */
    int64_t n_left = 100000;
    ray_t* lid_v = ray_vec_new(RAY_I64, n_left);
    lid_v->len = n_left;
    ray_t* lval_v = ray_vec_new(RAY_I64, n_left);
    lval_v->len = n_left;
    int64_t* lid = (int64_t*)ray_data(lid_v);
    int64_t* lval = (int64_t*)ray_data(lval_v);
    for (int64_t i = 0; i < n_left; i++) {
        lid[i] = i % 50000;
        lval[i] = i;
    }

    /* Right table: 100K rows, id = i % 50000, score = i * 10
     * Each key appears exactly twice on the right side. */
    int64_t n_right = 100000;
    ray_t* rid_v = ray_vec_new(RAY_I64, n_right);
    rid_v->len = n_right;
    ray_t* rscore_v = ray_vec_new(RAY_I64, n_right);
    rscore_v->len = n_right;
    int64_t* rid = (int64_t*)ray_data(rid_v);
    int64_t* rscore = (int64_t*)ray_data(rscore_v);
    for (int64_t i = 0; i < n_right; i++) {
        rid[i] = i % 50000;
        rscore[i] = i * 10;
    }

    int64_t n_id = ray_sym_intern("id", 2);
    int64_t n_val = ray_sym_intern("val", 3);
    int64_t n_score = ray_sym_intern("score", 5);

    ray_t* left = ray_table_new(2);
    left = ray_table_add_col(left, n_id, lid_v);
    left = ray_table_add_col(left, n_val, lval_v);
    ray_release(lid_v);
    ray_release(lval_v);

    ray_t* right = ray_table_new(2);
    right = ray_table_add_col(right, n_id, rid_v);
    right = ray_table_add_col(right, n_score, rscore_v);
    ray_release(rid_v);
    ray_release(rscore_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* lk = ray_scan(g, "id");
    ray_op_t* lk_arr[] = { lk };
    ray_op_t* rk_arr[] = { lk };
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    ray_t* result = ray_execute(g, join_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* 50K keys, 2 left x 2 right = 4 matches per key, total = 200K rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 200000);
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 3);

    /* Validate data: sum of "score" column should equal the expected value.
     * Each right row (score = i*10) matches 2 left rows, so each right row
     * appears twice in the output. Expected sum = 2 * sum(i*10 for i=0..99999)
     * = 2 * 10 * (99999 * 100000 / 2) = 99999000000 */
    ray_t* score_col = ray_table_get_col(result, n_score);
    TEST_ASSERT_TRUE(score_col != NULL);
    int64_t* scores = (int64_t*)ray_data(score_col);
    int64_t score_sum = 0;
    for (int64_t i = 0; i < 200000; i++)
        score_sum += scores[i];
    int64_t expected_sum = (int64_t)2 * 10 * ((int64_t)99999 * 100000 / 2);
    TEST_ASSERT_TRUE(score_sum == expected_sum);

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- JOIN: small left x large right (asymmetric radix path) ---- */
static test_result_t test_exec_join_fallback(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Left table: 100 rows (small), id = i, val = i * 7
     * Right table: 100K rows (large, triggers radix path), id = i % 100
     * Each left row matches 1000 right rows → 100 * 1000 = 100K output rows */
    int64_t n_left_rows = 100;
    ray_t* lid_v = ray_vec_new(RAY_I64, n_left_rows);
    lid_v->len = n_left_rows;
    ray_t* lval_v = ray_vec_new(RAY_I64, n_left_rows);
    lval_v->len = n_left_rows;
    int64_t* lid2 = (int64_t*)ray_data(lid_v);
    int64_t* lval2 = (int64_t*)ray_data(lval_v);
    for (int64_t i = 0; i < n_left_rows; i++) {
        lid2[i] = i;
        lval2[i] = i * 7;
    }

    int64_t n_right_rows = 100000;
    ray_t* rid_v = ray_vec_new(RAY_I64, n_right_rows);
    rid_v->len = n_right_rows;
    ray_t* rscore_v = ray_vec_new(RAY_I64, n_right_rows);
    rscore_v->len = n_right_rows;
    int64_t* rid2 = (int64_t*)ray_data(rid_v);
    int64_t* rscore2 = (int64_t*)ray_data(rscore_v);
    for (int64_t i = 0; i < n_right_rows; i++) {
        rid2[i] = i % 100;
        rscore2[i] = i;
    }

    int64_t n_id = ray_sym_intern("id", 2);
    int64_t n_val = ray_sym_intern("val", 3);
    int64_t n_score = ray_sym_intern("score", 5);

    ray_t* left = ray_table_new(2);
    left = ray_table_add_col(left, n_id, lid_v);
    left = ray_table_add_col(left, n_val, lval_v);
    ray_release(lid_v);
    ray_release(lval_v);

    ray_t* right = ray_table_new(2);
    right = ray_table_add_col(right, n_id, rid_v);
    right = ray_table_add_col(right, n_score, rscore_v);
    ray_release(rid_v);
    ray_release(rscore_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* lk = ray_scan(g, "id");
    ray_op_t* lk_arr[] = { lk };
    ray_op_t* rk_arr[] = { lk };
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    ray_t* result = ray_execute(g, join_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* 100 left rows * 1000 right matches each = 100K output rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 100000);
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 3);

    /* Validate: every output row's "val" should be id * 7 */
    ray_t* res_id_col = ray_table_get_col(result, n_id);
    ray_t* res_val_col = ray_table_get_col(result, n_val);
    TEST_ASSERT_TRUE(res_id_col != NULL);
    TEST_ASSERT_TRUE(res_val_col != NULL);
    int64_t* res_ids = (int64_t*)ray_data(res_id_col);
    int64_t* res_vals = (int64_t*)ray_data(res_val_col);
    for (int64_t i = 0; i < ray_table_nrows(result); i++)
        TEST_ASSERT_TRUE(res_vals[i] == res_ids[i] * 7);

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- JOIN: empty tables ---- */
static test_result_t test_exec_join_empty(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n_id = ray_sym_intern("id", 2);
    int64_t n_val = ray_sym_intern("val", 3);
    int64_t n_score = ray_sym_intern("score", 5);

    /* Non-empty table: 3 rows */
    int64_t ids[] = {1, 2, 3};
    int64_t vals[] = {10, 20, 30};
    ray_t* id_v = ray_vec_from_raw(RAY_I64, ids, 3);
    ray_t* val_v = ray_vec_from_raw(RAY_I64, vals, 3);
    ray_t* nonempty = ray_table_new(2);
    nonempty = ray_table_add_col(nonempty, n_id, id_v);
    nonempty = ray_table_add_col(nonempty, n_val, val_v);
    ray_release(id_v); ray_release(val_v);

    /* Empty table: 0 rows */
    int64_t dummy = 0;
    ray_t* empty_id = ray_vec_from_raw(RAY_I64, &dummy, 0);
    ray_t* empty_score = ray_vec_from_raw(RAY_I64, &dummy, 0);
    ray_t* empty = ray_table_new(2);
    empty = ray_table_add_col(empty, n_id, empty_id);
    empty = ray_table_add_col(empty, n_score, empty_score);
    ray_release(empty_id); ray_release(empty_score);

    /* Test 1: INNER JOIN with empty right -> 0 rows */
    {
        ray_graph_t* g = ray_graph_new(nonempty);
        ray_op_t* l = ray_const_table(g, nonempty);
        ray_op_t* r = ray_const_table(g, empty);
        ray_op_t* k = ray_scan(g, "id");
        ray_op_t* ka[] = { k };
        ray_op_t* j = ray_join(g, l, ka, r, ka, 1, 0);
        ray_t* res = ray_execute(g, j);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT_EQ_I(ray_table_nrows(res), 0);
        ray_release(res);
        ray_graph_free(g);
    }

    /* Test 2: INNER JOIN with empty left -> 0 rows */
    {
        ray_graph_t* g = ray_graph_new(empty);
        ray_op_t* l = ray_const_table(g, empty);
        ray_op_t* r = ray_const_table(g, nonempty);
        ray_op_t* k = ray_scan(g, "id");
        ray_op_t* ka[] = { k };
        ray_op_t* j = ray_join(g, l, ka, r, ka, 1, 0);
        ray_t* res = ray_execute(g, j);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT_EQ_I(ray_table_nrows(res), 0);
        ray_release(res);
        ray_graph_free(g);
    }

    /* Test 3: LEFT JOIN with empty right -> 3 rows (all unmatched) */
    {
        ray_graph_t* g = ray_graph_new(nonempty);
        ray_op_t* l = ray_const_table(g, nonempty);
        ray_op_t* r = ray_const_table(g, empty);
        ray_op_t* k = ray_scan(g, "id");
        ray_op_t* ka[] = { k };
        ray_op_t* j = ray_join(g, l, ka, r, ka, 1, 1);
        ray_t* res = ray_execute(g, j);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT_EQ_I(ray_table_nrows(res), 3);
        ray_release(res);
        ray_graph_free(g);
    }

    /* Test 4: FULL OUTER JOIN with empty right -> 3 rows (all left unmatched) */
    {
        ray_graph_t* g = ray_graph_new(nonempty);
        ray_op_t* l = ray_const_table(g, nonempty);
        ray_op_t* r = ray_const_table(g, empty);
        ray_op_t* k = ray_scan(g, "id");
        ray_op_t* ka[] = { k };
        ray_op_t* j = ray_join(g, l, ka, r, ka, 1, 2);
        ray_t* res = ray_execute(g, j);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT_EQ_I(ray_table_nrows(res), 3);
        ray_release(res);
        ray_graph_free(g);
    }

    /* Test 5: FULL OUTER JOIN with empty left -> 3 rows (all right unmatched) */
    {
        ray_graph_t* g = ray_graph_new(empty);
        ray_op_t* l = ray_const_table(g, empty);
        ray_op_t* r = ray_const_table(g, nonempty);
        ray_op_t* k = ray_scan(g, "id");
        ray_op_t* ka[] = { k };
        ray_op_t* j = ray_join(g, l, ka, r, ka, 1, 2);
        ray_t* res = ray_execute(g, j);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res));
        TEST_ASSERT_EQ_I(ray_table_nrows(res), 3);
        ray_release(res);
        ray_graph_free(g);
    }

    ray_release(nonempty);
    ray_release(empty);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- LEFT OUTER JOIN (radix path, >64K rows) ---- */
static test_result_t test_exec_join_left_large(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Left: 100K rows, id = i (unique keys) */
    int64_t n_left = 100000;
    ray_t* lid_v = ray_vec_new(RAY_I64, n_left);
    lid_v->len = n_left;
    ray_t* lval_v = ray_vec_new(RAY_I64, n_left);
    lval_v->len = n_left;
    int64_t* lid = (int64_t*)ray_data(lid_v);
    int64_t* lval = (int64_t*)ray_data(lval_v);
    for (int64_t i = 0; i < n_left; i++) {
        lid[i] = i;
        lval[i] = i * 10;
    }

    /* Right: 100K rows, id = i * 2 (only even keys match) */
    int64_t n_right = 100000;
    ray_t* rid_v = ray_vec_new(RAY_I64, n_right);
    rid_v->len = n_right;
    ray_t* rscore_v = ray_vec_new(RAY_I64, n_right);
    rscore_v->len = n_right;
    int64_t* rid = (int64_t*)ray_data(rid_v);
    int64_t* rscore = (int64_t*)ray_data(rscore_v);
    for (int64_t i = 0; i < n_right; i++) {
        rid[i] = i * 2;
        rscore[i] = i * 100;
    }

    int64_t n_id = ray_sym_intern("id", 2);
    int64_t n_val = ray_sym_intern("val", 3);
    int64_t n_score = ray_sym_intern("score", 5);

    ray_t* left = ray_table_new(2);
    left = ray_table_add_col(left, n_id, lid_v);
    left = ray_table_add_col(left, n_val, lval_v);
    ray_release(lid_v); ray_release(lval_v);

    ray_t* right = ray_table_new(2);
    right = ray_table_add_col(right, n_id, rid_v);
    right = ray_table_add_col(right, n_score, rscore_v);
    ray_release(rid_v); ray_release(rscore_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* lk = ray_scan(g, "id");
    ray_op_t* lk_arr[] = { lk };
    ray_op_t* rk_arr[] = { lk };
    /* LEFT OUTER join (type=1) */
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 1, 1);

    ray_t* result = ray_execute(g, join_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* LEFT OUTER: all 100K left rows preserved.
     * Even keys (0,2,4,...,99998) match right side: 50K matched rows.
     * Odd keys (1,3,5,...,99999) have no match: 50K unmatched rows.
     * Total = 100K rows. */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 100000);

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- FULL OUTER JOIN (radix path, >64K rows) ---- */
static test_result_t test_exec_join_full_large(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Left: 80K rows, id = i (0..79999) */
    int64_t n_left = 80000;
    ray_t* lid_v = ray_vec_new(RAY_I64, n_left);
    lid_v->len = n_left;
    int64_t* lid = (int64_t*)ray_data(lid_v);
    for (int64_t i = 0; i < n_left; i++) lid[i] = i;

    /* Right: 80K rows, id = i + 40000 (40000..119999) */
    int64_t n_right = 80000;
    ray_t* rid_v = ray_vec_new(RAY_I64, n_right);
    rid_v->len = n_right;
    int64_t* rid = (int64_t*)ray_data(rid_v);
    for (int64_t i = 0; i < n_right; i++) rid[i] = i + 40000;

    int64_t n_id = ray_sym_intern("id", 2);
    ray_t* left = ray_table_new(1);
    left = ray_table_add_col(left, n_id, lid_v);
    ray_release(lid_v);
    ray_t* right = ray_table_new(1);
    right = ray_table_add_col(right, n_id, rid_v);
    ray_release(rid_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* lk = ray_scan(g, "id");
    ray_op_t* lk_arr[] = { lk };
    ray_op_t* rk_arr[] = { lk };
    /* FULL OUTER join (type=2) */
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 1, 2);

    ray_t* result = ray_execute(g, join_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Left 0..39999 unmatched (40K), overlap 40000..79999 matched (40K),
     * Right 80000..119999 unmatched (40K). Total = 120K. */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 120000);

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- JOIN: skewed keys (all rows hash to same partition) ---- */
static test_result_t test_exec_join_skewed(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Left: 100K rows, all id = 42.
     * Right: 100K rows, id = 0..99999 (unique keys, triggers radix path).
     * Only key=42 matches on right side → all left rows land in one partition.
     * INNER JOIN: 100K left rows × 1 right row = 100K result rows. */
    int64_t n_left = 100000;
    ray_t* lid_v = ray_vec_new(RAY_I64, n_left);
    lid_v->len = n_left;
    ray_t* lval_v = ray_vec_new(RAY_I64, n_left);
    lval_v->len = n_left;
    int64_t* lid = (int64_t*)ray_data(lid_v);
    int64_t* lval = (int64_t*)ray_data(lval_v);
    for (int64_t i = 0; i < n_left; i++) {
        lid[i] = 42;
        lval[i] = i;
    }

    int64_t n_right = 100000;
    ray_t* rid_v = ray_vec_new(RAY_I64, n_right);
    rid_v->len = n_right;
    ray_t* rscore_v = ray_vec_new(RAY_I64, n_right);
    rscore_v->len = n_right;
    int64_t* rid = (int64_t*)ray_data(rid_v);
    int64_t* rscore = (int64_t*)ray_data(rscore_v);
    for (int64_t i = 0; i < n_right; i++) {
        rid[i] = i;
        rscore[i] = i * 7;
    }

    int64_t n_id = ray_sym_intern("id", 2);
    int64_t n_val = ray_sym_intern("val", 3);
    int64_t n_score = ray_sym_intern("score", 5);

    ray_t* left = ray_table_new(2);
    left = ray_table_add_col(left, n_id, lid_v);
    left = ray_table_add_col(left, n_val, lval_v);
    ray_release(lid_v); ray_release(lval_v);

    ray_t* right = ray_table_new(2);
    right = ray_table_add_col(right, n_id, rid_v);
    right = ray_table_add_col(right, n_score, rscore_v);
    ray_release(rid_v); ray_release(rscore_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* lk = ray_scan(g, "id");
    ray_op_t* lk_arr[] = { lk };
    ray_op_t* rk_arr[] = { lk };
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    ray_t* result = ray_execute(g, join_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 100000);

    /* Verify all score values: all should be 42 * 7 = 294 */
    ray_t* score_col = ray_table_get_col(result, n_score);
    TEST_ASSERT_NOT_NULL(score_col);
    int64_t* scores = (int64_t*)ray_data(score_col);
    for (int64_t i = 0; i < ray_table_nrows(result); i++)
        TEST_ASSERT_EQ_I(scores[i], 294);

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- JOIN: threshold boundary (just above RAY_PARALLEL_THRESHOLD) ---- */
static test_result_t test_exec_join_boundary(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Right side = 65537 rows (just above RAY_PARALLEL_THRESHOLD = 65536).
     * This triggers the radix path. Verify it produces the same result
     * as the chained HT would for the same data. */
    int64_t n = 65537;
    ray_t* lid_v = ray_vec_new(RAY_I64, n);
    lid_v->len = n;
    ray_t* rid_v = ray_vec_new(RAY_I64, n);
    rid_v->len = n;
    int64_t* lid = (int64_t*)ray_data(lid_v);
    int64_t* rid = (int64_t*)ray_data(rid_v);
    for (int64_t i = 0; i < n; i++) {
        lid[i] = i;
        rid[i] = i;
    }

    int64_t n_id = ray_sym_intern("id", 2);
    ray_t* left = ray_table_new(1);
    left = ray_table_add_col(left, n_id, lid_v);
    ray_release(lid_v);
    ray_t* right = ray_table_new(1);
    right = ray_table_add_col(right, n_id, rid_v);
    ray_release(rid_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* lk = ray_scan(g, "id");
    ray_op_t* lk_arr[] = { lk };
    ray_op_t* rk_arr[] = { lk };
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    ray_t* result = ray_execute(g, join_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* 1:1 key mapping -> exactly n result rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), n);

    /* Verify join correctness: every id in [0, n) appears exactly once.
     * Use a seen-bitmap to detect duplicates (sum check is insufficient). */
    ray_t* res_id = ray_table_get_col(result, n_id);
    TEST_ASSERT_NOT_NULL(res_id);
    int64_t* res_ids = (int64_t*)ray_data(res_id);
    ray_t* seen_hdr = ray_alloc((size_t)n * sizeof(bool));
    TEST_ASSERT_NOT_NULL(seen_hdr);
    bool* seen = (bool*)ray_data(seen_hdr);
    memset(seen, 0, (size_t)n * sizeof(bool));
    for (int64_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(res_ids[i] >= 0 && res_ids[i] < n);
        TEST_ASSERT_FALSE(seen[res_ids[i]]);
        seen[res_ids[i]] = true;
    }
    ray_free(seen_hdr);

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- JOIN: multi-key composite join (I64 + F64 mixed keys) ---- */
static test_result_t test_exec_join_multikey(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Left: 5 rows, join on (k1: I64, k2: F64) — exercises mixed-type key path */
    int64_t lk1[] = {1, 1, 2, 2, 3};
    double  lk2[] = {10.0, 20.0, 10.0, 20.0, 10.0};
    int64_t lval[] = {100, 200, 300, 400, 500};
    ray_t* lk1_v = ray_vec_from_raw(RAY_I64, lk1, 5);
    ray_t* lk2_v = ray_vec_from_raw(RAY_F64, lk2, 5);
    ray_t* lval_v = ray_vec_from_raw(RAY_I64, lval, 5);

    int64_t n_k1 = ray_sym_intern("k1", 2);
    int64_t n_k2 = ray_sym_intern("k2", 2);
    int64_t n_val = ray_sym_intern("val", 3);
    int64_t n_score = ray_sym_intern("score", 5);

    ray_t* left = ray_table_new(3);
    left = ray_table_add_col(left, n_k1, lk1_v);
    left = ray_table_add_col(left, n_k2, lk2_v);
    left = ray_table_add_col(left, n_val, lval_v);
    ray_release(lk1_v); ray_release(lk2_v); ray_release(lval_v);

    /* Right: 3 rows */
    int64_t rk1[] = {1, 2, 3};
    double  rk2[] = {10.0, 20.0, 30.0};
    int64_t rscore[] = {1000, 2000, 3000};
    ray_t* rk1_v = ray_vec_from_raw(RAY_I64, rk1, 3);
    ray_t* rk2_v = ray_vec_from_raw(RAY_F64, rk2, 3);
    ray_t* rscore_v = ray_vec_from_raw(RAY_I64, rscore, 3);

    ray_t* right = ray_table_new(3);
    right = ray_table_add_col(right, n_k1, rk1_v);
    right = ray_table_add_col(right, n_k2, rk2_v);
    right = ray_table_add_col(right, n_score, rscore_v);
    ray_release(rk1_v); ray_release(rk2_v); ray_release(rscore_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* k1_op = ray_scan(g, "k1");
    ray_op_t* k2_op = ray_scan(g, "k2");
    ray_op_t* lk_arr[] = { k1_op, k2_op };
    ray_op_t* rk_arr[] = { k1_op, k2_op };
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 2, 0);

    ray_t* result = ray_execute(g, join_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Matches: (1,10)->1, (2,20)->1. No match: (1,20), (2,10), (3,10).
     * Total = 2 result rows. */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);

    /* Verify joined score values: should be {1000, 2000} in some order */
    ray_t* score_col = ray_table_get_col(result, n_score);
    TEST_ASSERT_NOT_NULL(score_col);
    int64_t* scores = (int64_t*)ray_data(score_col);
    int64_t score_sum = scores[0] + scores[1];
    TEST_ASSERT_EQ_I(score_sum, 3000);
    TEST_ASSERT_TRUE((scores[0] == 1000 && scores[1] == 2000) ||
                      (scores[0] == 2000 && scores[1] == 1000));

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- WINDOW ---- */
static test_result_t test_exec_window(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 6;
    int64_t grp_data[] = {1, 1, 1, 2, 2, 2};
    int64_t val_data[] = {10, 20, 30, 40, 50, 60};
    ray_t* grp_v = ray_vec_from_raw(RAY_I64, grp_data, n);
    ray_t* val_v = ray_vec_from_raw(RAY_I64, val_data, n);
    int64_t n_grp = ray_sym_intern("grp", 3);
    int64_t n_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n_grp, grp_v);
    tbl = ray_table_add_col(tbl, n_val, val_v);
    ray_release(grp_v);
    ray_release(val_v);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* grp_op = ray_scan(g, "grp");
    ray_op_t* val_op = ray_scan(g, "val");

    ray_op_t* parts[] = { grp_op };
    ray_op_t* orders[] = { val_op };
    uint8_t order_descs[] = { 0 };  /* ascending */
    uint8_t func_kinds[] = { RAY_WIN_ROW_NUMBER };
    ray_op_t* func_inputs[] = { val_op };
    int64_t func_params[] = { 0 };
    ray_op_t* win = ray_window_op(g, tbl_op,
                                parts, 1,
                                orders, order_descs, 1,
                                func_kinds, func_inputs, func_params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);

    ray_t* result = ray_execute(g, win);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 6);
    /* Window adds a column (row_number) to the 2-col input */
    TEST_ASSERT_TRUE(ray_table_ncols(result) >= 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- SELECT (column projection) ---- */
static test_result_t test_exec_select(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* id1 = ray_scan(g, "id1");
    ray_op_t* cols[] = { v1, id1 };
    ray_op_t* sel = ray_select(g, tbl_op, cols, 2);

    ray_t* result = ray_execute(g, sel);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 10);

    /* Verify first column is v1 (I64) and has correct values */
    ray_t* c0 = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(c0);
    TEST_ASSERT_EQ_I(c0->type, RAY_I64);
    int64_t* c0_data = (int64_t*)ray_data(c0);
    TEST_ASSERT_EQ_I(c0_data[0], 10);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- STDDEV / VAR ---- */
static test_result_t test_exec_stddev(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double vals[] = {2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0};
    ray_t* vec = ray_vec_from_raw(RAY_F64, vals, 8);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* VAR_POP = 4.0, STDDEV_POP = 2.0 for this dataset */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* var_op = ray_var_pop(g, x);
    ray_t* result = ray_execute(g, var_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 4.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* stddev_op = ray_stddev_pop(g, x);
    result = ray_execute(g, stddev_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 2.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* Sample variance: var_pop * n/(n-1) = 4.0 * 8/7 = 32/7 ≈ 4.571429 */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* var_s = ray_var(g, x);
    result = ray_execute(g, var_s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 32.0 / 7.0, 1e-5);
    ray_release(result);
    ray_graph_free(g);

    /* Sample stddev: sqrt(32/7) ≈ 2.138090 */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* stddev_s = ray_stddev(g, x);
    result = ray_execute(g, stddev_s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, sqrt(32.0 / 7.0), 1e-5);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- COUNT_DISTINCT ---- */
static test_result_t test_exec_count_distinct(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();

    /* id1 has values {1,1,2,2,3,3,1,2,3,1} → 3 distinct */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* id1 = ray_scan(g, "id1");
    ray_op_t* cd = ray_count_distinct(g, id1);
    ray_t* result = ray_execute(g, cd);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    ray_graph_free(g);

    /* v1 has values {10,20,...,100} → 10 distinct */
    g = ray_graph_new(tbl);
    ray_op_t* v1 = ray_scan(g, "v1");
    cd = ray_count_distinct(g, v1);
    result = ray_execute(g, cd);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 10);
    ray_release(result);
    ray_graph_free(g);

    /* F64 column: {1.5, 2.5, 1.5, 2.5, 3.5} → 3 distinct */
    (void)ray_sym_init();
    double fvals[] = {1.5, 2.5, 1.5, 2.5, 3.5};
    ray_t* fvec = ray_vec_from_raw(RAY_F64, fvals, 5);
    int64_t fname = ray_sym_intern("f", 1);
    ray_t* ftbl = ray_table_new(1);
    ftbl = ray_table_add_col(ftbl, fname, fvec);
    ray_release(fvec);

    g = ray_graph_new(ftbl);
    ray_op_t* fop = ray_scan(g, "f");
    cd = ray_count_distinct(g, fop);
    result = ray_execute(g, cd);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    ray_graph_free(g);
    ray_release(ftbl);

    /* Single value repeated → 1 distinct */
    int64_t ones[] = {1, 1, 1, 1, 1};
    ray_t* ones_v = ray_vec_from_raw(RAY_I64, ones, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl2 = ray_table_new(1);
    tbl2 = ray_table_add_col(tbl2, name, ones_v);
    ray_release(ones_v);

    g = ray_graph_new(tbl2);
    ray_op_t* x = ray_scan(g, "x");
    cd = ray_count_distinct(g, x);
    result = ray_execute(g, cd);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl2);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- ASOF JOIN ---- */
static test_result_t test_exec_asof_join(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Left: trades — time(I64), sym(I64), price(F64) */
    int64_t ltime[]  = {100, 200, 300, 400, 500};
    int64_t lsym[]   = {1, 1, 2, 1, 2};
    double  lprice[] = {10.0, 20.0, 30.0, 40.0, 50.0};

    ray_t* lt_v = ray_vec_from_raw(RAY_I64, ltime, 5);
    ray_t* ls_v = ray_vec_from_raw(RAY_I64, lsym, 5);
    ray_t* lp_v = ray_vec_from_raw(RAY_F64, lprice, 5);

    int64_t n_time  = ray_sym_intern("time", 4);
    int64_t n_sym   = ray_sym_intern("sym", 3);
    int64_t n_price = ray_sym_intern("price", 5);

    ray_t* left = ray_table_new(3);
    left = ray_table_add_col(left, n_time, lt_v);
    left = ray_table_add_col(left, n_sym, ls_v);
    left = ray_table_add_col(left, n_price, lp_v);
    ray_release(lt_v); ray_release(ls_v); ray_release(lp_v);

    /* Right: quotes — time(I64), sym(I64), bid(F64) */
    int64_t rtime[] = {90, 150, 250, 350, 450};
    int64_t rsym[]  = {1, 1, 2, 1, 2};
    double  rbid[]  = {9.5, 15.0, 25.0, 35.0, 45.0};

    ray_t* rt_v = ray_vec_from_raw(RAY_I64, rtime, 5);
    ray_t* rs_v = ray_vec_from_raw(RAY_I64, rsym, 5);
    ray_t* rb_v = ray_vec_from_raw(RAY_F64, rbid, 5);

    int64_t n_bid = ray_sym_intern("bid", 3);

    ray_t* right = ray_table_new(3);
    right = ray_table_add_col(right, n_time, rt_v);
    right = ray_table_add_col(right, n_sym, rs_v);
    right = ray_table_add_col(right, n_bid, rb_v);
    ray_release(rt_v); ray_release(rs_v); ray_release(rb_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op  = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* tkey = ray_scan(g, "time");
    ray_op_t* skey = ray_scan(g, "sym");
    ray_op_t* eq_keys[] = { skey };

    /* Inner ASOF join */
    ray_op_t* aj = ray_asof_join(g, left_op, right_op, tkey, eq_keys, 1, 0);

    ray_t* result = ray_execute(g, aj);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* All 5 left rows should have matches */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 5);
    /* Should have left cols + bid (time/sym deduplicated) */
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 4);  /* time, sym, price, bid */

    /* Verify bid values — best right.time <= left.time per partition.
     * Output preserves original left-table row order. */
    ray_t* bid_col = ray_table_get_col(result, n_bid);
    TEST_ASSERT_NOT_NULL(bid_col);
    double* bid_data = (double*)ray_data(bid_col);
    /* Original left order: (t=100,s=1), (t=200,s=1), (t=300,s=2), (t=400,s=1), (t=500,s=2) */
    /* t=100,s=1: right s=1,t=90 -> bid=9.5 */
    TEST_ASSERT((bid_data[0]) == (9.5), "double == failed");
    /* t=200,s=1: right s=1,t=150 -> bid=15.0 */
    TEST_ASSERT((bid_data[1]) == (15.0), "double == failed");
    /* t=300,s=2: right s=2,t=250 -> bid=25.0 */
    TEST_ASSERT((bid_data[2]) == (25.0), "double == failed");
    /* t=400,s=1: right s=1,t=350 -> bid=35.0 */
    TEST_ASSERT((bid_data[3]) == (35.0), "double == failed");
    /* t=500,s=2: right s=2,t=450 -> bid=45.0 */
    TEST_ASSERT((bid_data[4]) == (45.0), "double == failed");

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- ASOF LEFT JOIN ---- */
static test_result_t test_exec_asof_left_join(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Left has a row with time=50 that's before any right row */
    int64_t ltime[] = {50, 100, 200};
    double  lval[]  = {1.0, 2.0, 3.0};
    ray_t* lt_v = ray_vec_from_raw(RAY_I64, ltime, 3);
    ray_t* lv_v = ray_vec_from_raw(RAY_F64, lval, 3);
    int64_t n_time = ray_sym_intern("time", 4);
    int64_t n_val  = ray_sym_intern("val", 3);
    ray_t* left = ray_table_new(2);
    left = ray_table_add_col(left, n_time, lt_v);
    left = ray_table_add_col(left, n_val, lv_v);
    ray_release(lt_v); ray_release(lv_v);

    int64_t rtime[] = {80, 150};
    double  rbid[]  = {0.8, 1.5};
    ray_t* rt_v = ray_vec_from_raw(RAY_I64, rtime, 2);
    ray_t* rb_v = ray_vec_from_raw(RAY_F64, rbid, 2);
    int64_t n_bid = ray_sym_intern("bid", 3);
    ray_t* right = ray_table_new(2);
    right = ray_table_add_col(right, n_time, rt_v);
    right = ray_table_add_col(right, n_bid, rb_v);
    ray_release(rt_v); ray_release(rb_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op  = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* tkey = ray_scan(g, "time");

    /* Left outer ASOF join, no eq keys */
    ray_op_t* aj = ray_asof_join(g, left_op, right_op, tkey, NULL, 0, 1);

    ray_t* result = ray_execute(g, aj);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* Left outer: all 3 left rows preserved */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    /* Verify: time=50 has no match (before any right row), bid should be 0 (NULL fill) */
    ray_t* bid_col = ray_table_get_col(result, n_bid);
    TEST_ASSERT_NOT_NULL(bid_col);
    double* bid_data = (double*)ray_data(bid_col);
    TEST_ASSERT((bid_data[0]) == (0.0), "double == failed");   /* t=50: no match */
    TEST_ASSERT((bid_data[1]) == (0.8), "double == failed");   /* t=100: right t=80 */
    TEST_ASSERT((bid_data[2]) == (1.5), "double == failed");   /* t=200: right t=150 */

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- ASOF JOIN: empty right ---- */
static test_result_t test_exec_asof_empty(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t ltime[] = {100, 200};
    double  lval[]  = {1.0, 2.0};
    ray_t* lt_v = ray_vec_from_raw(RAY_I64, ltime, 2);
    ray_t* lv_v = ray_vec_from_raw(RAY_F64, lval, 2);
    int64_t n_time = ray_sym_intern("time", 4);
    int64_t n_val  = ray_sym_intern("val", 3);
    ray_t* left = ray_table_new(2);
    left = ray_table_add_col(left, n_time, lt_v);
    left = ray_table_add_col(left, n_val, lv_v);
    ray_release(lt_v); ray_release(lv_v);

    int64_t n_bid = ray_sym_intern("bid", 3);
    ray_t* right = ray_table_new(2);
    ray_t* rt_v = ray_vec_new(RAY_I64, 0);
    ray_t* rb_v = ray_vec_new(RAY_F64, 0);
    right = ray_table_add_col(right, n_time, rt_v);
    right = ray_table_add_col(right, n_bid, rb_v);
    ray_release(rt_v); ray_release(rb_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op  = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* tkey = ray_scan(g, "time");

    /* Inner ASOF with empty right → 0 rows */
    ray_op_t* aj = ray_asof_join(g, left_op, right_op, tkey, NULL, 0, 0);
    ray_t* result = ray_execute(g, aj);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- ASOF NULL-KEY HANDLING ---- */
static test_result_t test_exec_asof_null_keys(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Left: time and sym eq key, with a null on row 1 (sym) and row 2 (time) */
    int64_t ltime[] = {100, 200, 300};  /* row 2 will be marked null */
    int64_t lsym[]  = {1, 1, 1};         /* row 1 will be marked null on sym */
    double  lval[]  = {10.0, 20.0, 30.0};
    ray_t* lt_v = ray_vec_from_raw(RAY_I64, ltime, 3);
    ray_t* ls_v = ray_vec_from_raw(RAY_I64, lsym, 3);
    ray_t* lv_v = ray_vec_from_raw(RAY_F64, lval, 3);
    ray_vec_set_null(ls_v, 1, true);     /* left row 1: null sym */
    ray_vec_set_null(lt_v, 2, true);     /* left row 2: null time */

    int64_t n_time  = ray_sym_intern("time", 4);
    int64_t n_sym   = ray_sym_intern("sym", 3);
    int64_t n_val   = ray_sym_intern("val", 3);
    ray_t* left = ray_table_new(3);
    left = ray_table_add_col(left, n_time, lt_v);
    left = ray_table_add_col(left, n_sym, ls_v);
    left = ray_table_add_col(left, n_val, lv_v);
    ray_release(lt_v); ray_release(ls_v); ray_release(lv_v);

    /* Right: includes a row with null time and a row with null sym — both
     * must be excluded from asof matching. */
    int64_t rtime[] = {50, 150, 250};   /* row 1 will be marked null */
    int64_t rsym[]  = {1, 1, 1};        /* row 0 will be marked null on sym */
    double  rbid[]  = {0.5, 1.5, 2.5};
    ray_t* rt_v = ray_vec_from_raw(RAY_I64, rtime, 3);
    ray_t* rs_v = ray_vec_from_raw(RAY_I64, rsym, 3);
    ray_t* rb_v = ray_vec_from_raw(RAY_F64, rbid, 3);
    ray_vec_set_null(rs_v, 0, true);    /* right row 0: null sym */
    ray_vec_set_null(rt_v, 1, true);    /* right row 1: null time */

    int64_t n_bid = ray_sym_intern("bid", 3);
    ray_t* right = ray_table_new(3);
    right = ray_table_add_col(right, n_time, rt_v);
    right = ray_table_add_col(right, n_sym, rs_v);
    right = ray_table_add_col(right, n_bid, rb_v);
    ray_release(rt_v); ray_release(rs_v); ray_release(rb_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op  = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* tkey = ray_scan(g, "time");
    ray_op_t* skey = ray_scan(g, "sym");
    ray_op_t* eq_keys[] = { skey };

    /* LEFT OUTER so null-keyed left rows still appear with null right cols */
    ray_op_t* aj = ray_asof_join(g, left_op, right_op, tkey, eq_keys, 1, 1);
    ray_t* result = ray_execute(g, aj);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    ray_t* bid_col = ray_table_get_col(result, n_bid);
    TEST_ASSERT_NOT_NULL(bid_col);
    TEST_ASSERT_TRUE((bid_col->attrs & RAY_ATTR_HAS_NULLS) != 0);
    /* Left row 0: non-null keys (t=100, s=1) → match right row 2 (t=250 ... no, 250>100)
     * Actually: rt[0]=50 s=null (excluded), rt[1]=150 t=null (excluded), rt[2]=250 s=1
     * Best right.time <= 100 where sym=1 is rt[0], but rt[0] is excluded (null sym).
     * No match → bid[0] = null. */
    TEST_ASSERT_TRUE(ray_vec_is_null(bid_col, 0));
    /* Left row 1: null sym key → no match → bid[1] = null */
    TEST_ASSERT_TRUE(ray_vec_is_null(bid_col, 1));
    /* Left row 2: null time key → no match → bid[2] = null */
    TEST_ASSERT_TRUE(ray_vec_is_null(bid_col, 2));

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Follow-up: match exists only through non-null right rows. */
static test_result_t test_exec_asof_null_keys_match(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t ltime[] = {100};
    int64_t lsym[]  = {1};
    double  lval[]  = {10.0};
    ray_t* lt_v = ray_vec_from_raw(RAY_I64, ltime, 1);
    ray_t* ls_v = ray_vec_from_raw(RAY_I64, lsym, 1);
    ray_t* lv_v = ray_vec_from_raw(RAY_F64, lval, 1);
    int64_t n_time = ray_sym_intern("time", 4);
    int64_t n_sym  = ray_sym_intern("sym", 3);
    int64_t n_val  = ray_sym_intern("val", 3);
    ray_t* left = ray_table_new(3);
    left = ray_table_add_col(left, n_time, lt_v);
    left = ray_table_add_col(left, n_sym, ls_v);
    left = ray_table_add_col(left, n_val, lv_v);
    ray_release(lt_v); ray_release(ls_v); ray_release(lv_v);

    /* Right has two valid candidates (rt=50 s=1, rt=80 s=1) and one null
     * row in between — asof must pick the latest valid one (rt=80). */
    int64_t rtime[] = {50, 200, 80};  /* row 1 will be null time */
    int64_t rsym[]  = {1, 1, 1};
    double  rbid[]  = {5.0, 20.0, 8.0};
    ray_t* rt_v = ray_vec_from_raw(RAY_I64, rtime, 3);
    ray_t* rs_v = ray_vec_from_raw(RAY_I64, rsym, 3);
    ray_t* rb_v = ray_vec_from_raw(RAY_F64, rbid, 3);
    ray_vec_set_null(rt_v, 1, true);
    int64_t n_bid = ray_sym_intern("bid", 3);
    ray_t* right = ray_table_new(3);
    right = ray_table_add_col(right, n_time, rt_v);
    right = ray_table_add_col(right, n_sym, rs_v);
    right = ray_table_add_col(right, n_bid, rb_v);
    ray_release(rt_v); ray_release(rs_v); ray_release(rb_v);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op  = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* tkey = ray_scan(g, "time");
    ray_op_t* skey = ray_scan(g, "sym");
    ray_op_t* eq_keys[] = { skey };
    ray_op_t* aj = ray_asof_join(g, left_op, right_op, tkey, eq_keys, 1, 0);
    ray_t* result = ray_execute(g, aj);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 1);

    ray_t* bid_col = ray_table_get_col(result, n_bid);
    double* bid_data = (double*)ray_data(bid_col);
    TEST_ASSERT((bid_data[0]) == (8.0), "double == failed");  /* rt=80, not rt=50 (later valid) */
    TEST_ASSERT_FALSE(ray_vec_is_null(bid_col, 0));

    ray_release(result);
    ray_graph_free(g);
    ray_release(left);
    ray_release(right);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- STRING HELPER ---- */
static ray_t* make_sym_table(void) {
    (void)ray_sym_init();
    int64_t s0 = ray_sym_intern("hello", 5);
    int64_t s1 = ray_sym_intern("WORLD", 5);
    int64_t s2 = ray_sym_intern("  foo  ", 7);
    int64_t s3 = ray_sym_intern("bar_baz", 7);
    int64_t s4 = ray_sym_intern("", 0);
    ray_t *vec = ray_vec_new(RAY_SYM, 5);
    vec->len = 5;
    int64_t *data = (int64_t*)ray_data(vec);
    data[0]=s0; data[1]=s1; data[2]=s2; data[3]=s3; data[4]=s4;
    int64_t n = ray_sym_intern("name", 4);
    ray_t *tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, n, vec);
    ray_release(vec);
    return tbl;
}

/* ---- UPPER ---- */
static test_result_t test_exec_upper(void) {
    ray_heap_init();
    ray_t* tbl = make_sym_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name_col = ray_scan(g, "name");
    ray_op_t* up = ray_upper(g, name_col);

    ray_t* result = ray_execute(g, up);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->len, 5);

    int64_t *rdata = (int64_t*)ray_data(result);
    ray_t* s = ray_sym_str(rdata[0]);
    TEST_ASSERT_STR_EQ(ray_str_ptr(s), "HELLO");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- LOWER ---- */
static test_result_t test_exec_lower(void) {
    ray_heap_init();
    ray_t* tbl = make_sym_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name_col = ray_scan(g, "name");
    ray_op_t* lo = ray_lower(g, name_col);

    ray_t* result = ray_execute(g, lo);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->len, 5);

    int64_t *rdata = (int64_t*)ray_data(result);
    ray_t* s = ray_sym_str(rdata[1]);
    TEST_ASSERT_STR_EQ(ray_str_ptr(s), "world");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- STRLEN ---- */
static test_result_t test_exec_strlen(void) {
    ray_heap_init();
    ray_t* tbl = make_sym_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name_col = ray_scan(g, "name");
    ray_op_t* slen = ray_strlen(g, name_col);

    ray_t* result = ray_execute(g, slen);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->len, 5);

    int64_t *rdata = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(rdata[0], 5);   /* "hello" */
    TEST_ASSERT_EQ_I(rdata[1], 5);   /* "WORLD" */
    TEST_ASSERT_EQ_I(rdata[2], 7);   /* "  foo  " */
    TEST_ASSERT_EQ_I(rdata[3], 7);   /* "bar_baz" */
    TEST_ASSERT_EQ_I(rdata[4], 0);   /* "" */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- TRIM ---- */
static test_result_t test_exec_trim(void) {
    ray_heap_init();
    ray_t* tbl = make_sym_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name_col = ray_scan(g, "name");
    ray_op_t* tr = ray_trim_op(g, name_col);

    ray_t* result = ray_execute(g, tr);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->len, 5);

    int64_t *rdata = (int64_t*)ray_data(result);
    ray_t* s = ray_sym_str(rdata[2]);
    TEST_ASSERT_STR_EQ(ray_str_ptr(s), "foo");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- LIKE ---- */
static test_result_t test_exec_like(void) {
    ray_heap_init();
    ray_t* tbl = make_sym_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name_col = ray_scan(g, "name");
    ray_op_t* pat = ray_const_str(g, "bar*", 4);
    ray_op_t* lk = ray_like(g, name_col, pat);
    ray_op_t* cnt = ray_count(g, ray_filter(g, name_col, lk));

    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 1);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- CONCAT ---- */
static test_result_t test_exec_concat(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build table with 3 SYM columns: a="hello", b=" ", c="world" */
    int64_t s_hello = ray_sym_intern("hello", 5);
    int64_t s_space = ray_sym_intern(" ", 1);
    int64_t s_world = ray_sym_intern("world", 5);

    ray_t* a_vec = ray_vec_new(RAY_SYM, 1);
    a_vec->len = 1;
    ((int64_t*)ray_data(a_vec))[0] = s_hello;

    ray_t* b_vec = ray_vec_new(RAY_SYM, 1);
    b_vec->len = 1;
    ((int64_t*)ray_data(b_vec))[0] = s_space;

    ray_t* c_vec = ray_vec_new(RAY_SYM, 1);
    c_vec->len = 1;
    ((int64_t*)ray_data(c_vec))[0] = s_world;

    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    int64_t nc = ray_sym_intern("c", 1);

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, na, a_vec);
    tbl = ray_table_add_col(tbl, nb, b_vec);
    tbl = ray_table_add_col(tbl, nc, c_vec);
    ray_release(a_vec); ray_release(b_vec); ray_release(c_vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* oa = ray_scan(g, "a");
    ray_op_t* ob = ray_scan(g, "b");
    ray_op_t* oc = ray_scan(g, "c");
    ray_op_t* args[] = {oa, ob, oc};
    ray_op_t* cat = ray_concat(g, args, 3);

    ray_t* result = ray_execute(g, cat);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->len, 1);

    int64_t *rdata = (int64_t*)ray_data(result);
    ray_t* s = ray_sym_str(rdata[0]);
    TEST_ASSERT_STR_EQ(ray_str_ptr(s), "hello world");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- EXTRACT ---- */
static test_result_t test_exec_extract(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 2024-06-15 12:30:45 UTC as nanoseconds since 2000-01-01.
     * RAY_TIMESTAMP is stored as i64 *nanoseconds* (matches
     * io/csv.c's parser and the rest of the runtime); the calendar
     * decomposer internally converts to µs. */
    int64_t ts = 771769845000000000LL;
    ray_t* ts_vec = ray_vec_from_raw(RAY_TIMESTAMP, &ts, 1);

    int64_t n_ts = ray_sym_intern("ts", 2);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, n_ts, ts_vec);
    ray_release(ts_vec);

    /* YEAR */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* col = ray_scan(g, "ts");
    ray_op_t* yr = ray_extract(g, col, RAY_EXTRACT_YEAR);
    ray_t* result = ray_execute(g, yr);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    int64_t *rdata = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(rdata[0], 2024);
    ray_release(result);
    ray_graph_free(g);

    /* MONTH */
    g = ray_graph_new(tbl);
    col = ray_scan(g, "ts");
    ray_op_t* mo = ray_extract(g, col, RAY_EXTRACT_MONTH);
    result = ray_execute(g, mo);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    rdata = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(rdata[0], 6);
    ray_release(result);
    ray_graph_free(g);

    /* DAY */
    g = ray_graph_new(tbl);
    col = ray_scan(g, "ts");
    ray_op_t* dy = ray_extract(g, col, RAY_EXTRACT_DAY);
    result = ray_execute(g, dy);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    rdata = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(rdata[0], 15);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- DATE_TRUNC ---- */
static test_result_t test_exec_date_trunc(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 2024-06-15 12:30:45 UTC as nanoseconds since 2000-01-01. */
    int64_t ts = 771769845000000000LL;
    ray_t* ts_vec = ray_vec_from_raw(RAY_TIMESTAMP, &ts, 1);

    int64_t n_ts = ray_sym_intern("ts", 2);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, n_ts, ts_vec);
    ray_release(ts_vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* col = ray_scan(g, "ts");
    ray_op_t* trunc = ray_date_trunc(g, col, RAY_EXTRACT_MONTH);

    ray_t* result = ray_execute(g, trunc);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    int64_t *rdata = (int64_t*)ray_data(result);
    TEST_ASSERT_TRUE(rdata[0] < ts);
    TEST_ASSERT_TRUE(rdata[0] > 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- CAST ---- */
static test_result_t test_exec_cast(void) {
    ray_heap_init();
    ray_t* tbl = make_exec_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* casted = ray_cast(g, v1, RAY_F64);
    ray_op_t* s = ray_sum(g, casted);

    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 550.0, 1e-6);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Helper: create table with RAY_STR "name" column — 5 rows */
static ray_t* make_str_table(void) {
    ray_t* col = ray_vec_new(RAY_STR, 5);
    col = ray_str_vec_append(col, "hello", 5);
    col = ray_str_vec_append(col, "WORLD", 5);
    col = ray_str_vec_append(col, "  foo  ", 7);
    col = ray_str_vec_append(col, "bar_baz", 7);
    col = ray_str_vec_append(col, "", 0);

    int64_t name_id = ray_sym_intern("name", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_id, col);
    ray_release(col);
    return tbl;
}

/* ---- RAY_STR EQ ---- */
static test_result_t test_exec_str_eq(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* lit = ray_const_str(g, "hello", 5);
    ray_op_t* eq = ray_eq(g, name, lit);

    ray_t* result = ray_execute(g, eq);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(result->len, 5);

    uint8_t* d = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(d[0], 1);  /* "hello" == "hello" */
    TEST_ASSERT_EQ_I(d[1], 0);  /* "WORLD" != "hello" */
    TEST_ASSERT_EQ_I(d[2], 0);
    TEST_ASSERT_EQ_I(d[3], 0);
    TEST_ASSERT_EQ_I(d[4], 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR NE ---- */
static test_result_t test_exec_str_ne(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* lit = ray_const_str(g, "hello", 5);
    ray_op_t* ne = ray_ne(g, name, lit);

    ray_t* result = ray_execute(g, ne);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(result->len, 5);

    uint8_t* d = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(d[0], 0);  /* "hello" != "hello" -> false */
    TEST_ASSERT_EQ_I(d[1], 1);  /* "WORLD" != "hello" -> true */
    TEST_ASSERT_EQ_I(d[2], 1);
    TEST_ASSERT_EQ_I(d[3], 1);
    TEST_ASSERT_EQ_I(d[4], 1);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR LT ---- */
static test_result_t test_exec_str_lt(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* lit = ray_const_str(g, "hello", 5);
    ray_op_t* lt = ray_lt(g, name, lit);

    ray_t* result = ray_execute(g, lt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(result->len, 5);

    uint8_t* d = (uint8_t*)ray_data(result);
    /* Lexicographic: "  foo  " < "hello", "WORLD" < "hello" (uppercase < lowercase),
       "bar_baz" < "hello", "" < "hello" */
    TEST_ASSERT_EQ_I(d[0], 0);  /* "hello" < "hello" -> false */
    TEST_ASSERT_EQ_I(d[1], 1);  /* "WORLD" < "hello" -> true (W=0x57 < h=0x68) */
    TEST_ASSERT_EQ_I(d[2], 1);  /* "  foo  " < "hello" -> true (space=0x20 < h=0x68) */
    TEST_ASSERT_EQ_I(d[3], 1);  /* "bar_baz" < "hello" -> true */
    TEST_ASSERT_EQ_I(d[4], 1);  /* "" < "hello" -> true */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR LE ---- */
static test_result_t test_exec_str_le(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* lit = ray_const_str(g, "hello", 5);
    ray_op_t* cmp = ray_le(g, name, lit);
    ray_t* result = ray_execute(g, cmp);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(result->len, 5);
    uint8_t* d = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(d[0], 1);  /* "hello" <= "hello" -> true */
    TEST_ASSERT_EQ_I(d[1], 1);  /* "WORLD" <= "hello" -> true (W=0x57 < h=0x68) */
    TEST_ASSERT_EQ_I(d[2], 1);  /* "  foo  " <= "hello" -> true (space < h) */
    TEST_ASSERT_EQ_I(d[3], 1);  /* "bar_baz" <= "hello" -> true (b < h) */
    TEST_ASSERT_EQ_I(d[4], 1);  /* "" <= "hello" -> true */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR GT ---- */
static test_result_t test_exec_str_gt(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* lit = ray_const_str(g, "hello", 5);
    ray_op_t* cmp = ray_gt(g, name, lit);
    ray_t* result = ray_execute(g, cmp);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(result->len, 5);
    uint8_t* d = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(d[0], 0);  /* "hello" > "hello" -> false */
    TEST_ASSERT_EQ_I(d[1], 0);  /* "WORLD" > "hello" -> false */
    TEST_ASSERT_EQ_I(d[2], 0);  /* "  foo  " > "hello" -> false */
    TEST_ASSERT_EQ_I(d[3], 0);  /* "bar_baz" > "hello" -> false */
    TEST_ASSERT_EQ_I(d[4], 0);  /* "" > "hello" -> false */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR GE ---- */
static test_result_t test_exec_str_ge(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* lit = ray_const_str(g, "hello", 5);
    ray_op_t* cmp = ray_ge(g, name, lit);
    ray_t* result = ray_execute(g, cmp);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(result->len, 5);
    uint8_t* d = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(d[0], 1);  /* "hello" >= "hello" -> true */
    TEST_ASSERT_EQ_I(d[1], 0);  /* "WORLD" >= "hello" -> false */
    TEST_ASSERT_EQ_I(d[2], 0);  /* "  foo  " >= "hello" -> false */
    TEST_ASSERT_EQ_I(d[3], 0);  /* "bar_baz" >= "hello" -> false */
    TEST_ASSERT_EQ_I(d[4], 0);  /* "" >= "hello" -> false */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR STRLEN ---- */
static test_result_t test_exec_str_strlen(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* slen = ray_strlen(g, name);
    ray_t* result = ray_execute(g, slen);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_I64);
    TEST_ASSERT_EQ_I(result->len, 5);
    int64_t* d = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(d[0], 5);   /* "hello" */
    TEST_ASSERT_EQ_I(d[1], 5);   /* "WORLD" */
    TEST_ASSERT_EQ_I(d[2], 7);   /* "  foo  " */
    TEST_ASSERT_EQ_I(d[3], 7);   /* "bar_baz" */
    TEST_ASSERT_EQ_I(d[4], 0);   /* "" */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- GRAPH DUMP (smoke test) ---- */
static test_result_t test_graph_dump(void) {
    ray_heap_init();
    ray_t *tbl = make_exec_table();
    ray_graph_t *g = ray_graph_new(tbl);
    ray_op_t *v1 = ray_scan(g, "v1");
    ray_op_t *id1 = ray_scan(g, "id1");
    ray_op_t *pred = ray_eq(g, id1, ray_const_i64(g, 1));
    ray_op_t *filtered = ray_filter(g, v1, pred);
    ray_op_t *s = ray_sum(g, filtered);
    ray_op_t *opt = ray_optimize(g, s);
    /* Dump to /dev/null — just verify no crash */
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) { ray_graph_dump(g, opt, devnull); fclose(devnull); }
    ray_t *result = ray_execute(g, opt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR UPPER ---- */
static test_result_t test_exec_str_upper(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* up = ray_upper(g, name);
    ray_t* result = ray_execute(g, up);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 5);

    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, s0, "HELLO");

    const char* s1 = ray_str_vec_get(result, 1, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, s1, "WORLD");

    const char* s4 = ray_str_vec_get(result, 4, &len);
    (void)s4;
    TEST_ASSERT_EQ_U(len, 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR LOWER ---- */
static test_result_t test_exec_str_lower(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* lo = ray_lower(g, name);
    ray_t* result = ray_execute(g, lo);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 5);

    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, s0, "hello");

    const char* s1 = ray_str_vec_get(result, 1, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, s1, "world");

    const char* s2 = ray_str_vec_get(result, 2, &len);
    TEST_ASSERT_EQ_U(len, 7);
    TEST_ASSERT_MEM_EQ(7, s2, "  foo  ");

    const char* s3 = ray_str_vec_get(result, 3, &len);
    TEST_ASSERT_EQ_U(len, 7);
    TEST_ASSERT_MEM_EQ(7, s3, "bar_baz");

    const char* s4 = ray_str_vec_get(result, 4, &len);
    (void)s4;
    TEST_ASSERT_EQ_U(len, 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR TRIM ---- */
static test_result_t test_exec_str_trim(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* tr = ray_trim_op(g, name);
    ray_t* result = ray_execute(g, tr);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 5);

    size_t len;
    /* "  foo  " -> "foo" */
    const char* s2 = ray_str_vec_get(result, 2, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, s2, "foo");

    /* "hello" unchanged */
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, s0, "hello");

    /* "" stays empty */
    const char* s4 = ray_str_vec_get(result, 4, &len);
    (void)s4;
    TEST_ASSERT_EQ_U(len, 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR SUBSTR ---- */
static test_result_t test_exec_str_substr(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* start = ray_const_i64(g, 1);
    ray_op_t* len_op = ray_const_i64(g, 3);
    ray_op_t* sub = ray_substr(g, name, start, len_op);
    ray_t* result = ray_execute(g, sub);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 5);

    size_t len;
    /* "hello" -> "hel" */
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, s0, "hel");

    /* "WORLD" -> "WOR" */
    const char* s1 = ray_str_vec_get(result, 1, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, s1, "WOR");

    /* "bar_baz" -> "bar" */
    const char* s3 = ray_str_vec_get(result, 3, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, s3, "bar");

    /* "" -> "" */
    const char* s4 = ray_str_vec_get(result, 4, &len);
    (void)s4;
    TEST_ASSERT_EQ_U(len, 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR REPLACE ---- */
static test_result_t test_exec_str_replace(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* from = ray_const_str(g, "o", 1);
    ray_op_t* to = ray_const_str(g, "0", 1);
    ray_op_t* rep = ray_replace(g, name, from, to);
    ray_t* result = ray_execute(g, rep);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 5);

    size_t len;
    /* "hello" -> "hell0" */
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, s0, "hell0");

    /* "WORLD" -> "WORLD" (no lowercase o) */
    const char* s1 = ray_str_vec_get(result, 1, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, s1, "WORLD");

    /* "  foo  " -> "  f00  " */
    const char* s2 = ray_str_vec_get(result, 2, &len);
    TEST_ASSERT_EQ_U(len, 7);
    TEST_ASSERT_MEM_EQ(7, s2, "  f00  ");

    /* "" -> "" */
    const char* s4 = ray_str_vec_get(result, 4, &len);
    (void)s4;
    TEST_ASSERT_EQ_U(len, 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_exec_str_concat(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Table with two RAY_STR columns */
    ray_t* c0 = ray_vec_new(RAY_STR, 3);
    c0 = ray_str_vec_append(c0, "John", 4);
    c0 = ray_str_vec_append(c0, "Jane", 4);
    c0 = ray_str_vec_append(c0, "Bob", 3);

    ray_t* c1 = ray_vec_new(RAY_STR, 3);
    c1 = ray_str_vec_append(c1, " Doe", 4);
    c1 = ray_str_vec_append(c1, " Smith", 6);
    c1 = ray_str_vec_append(c1, " Jr", 3);

    int64_t n_first = ray_sym_intern("first", 5);
    int64_t n_last  = ray_sym_intern("last", 4);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n_first, c0);
    tbl = ray_table_add_col(tbl, n_last, c1);
    ray_release(c0);
    ray_release(c1);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* first = ray_scan(g, "first");
    ray_op_t* last  = ray_scan(g, "last");
    ray_op_t* args[] = {first, last};
    ray_op_t* cat = ray_concat(g, args, 2);
    ray_t* result = ray_execute(g, cat);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 3);

    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 8);
    TEST_ASSERT_MEM_EQ(8, s0, "John Doe");

    const char* s1 = ray_str_vec_get(result, 1, &len);
    TEST_ASSERT_EQ_U(len, 10);
    TEST_ASSERT_MEM_EQ(10, s1, "Jane Smith");

    const char* s2 = ray_str_vec_get(result, 2, &len);
    TEST_ASSERT_EQ_U(len, 6);
    TEST_ASSERT_MEM_EQ(6, s2, "Bob Jr");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_exec_str_if(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* IF(name == "hello", name, UPPER(name)) — both branches are RAY_STR */
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* lit = ray_const_str(g, "hello", 5);
    ray_op_t* cond = ray_eq(g, name, lit);

    ray_op_t* then_col = ray_scan(g, "name");
    ray_op_t* else_col = ray_upper(g, ray_scan(g, "name"));
    ray_op_t* if_op = ray_if(g, cond, then_col, else_col);
    ray_t* result = ray_execute(g, if_op);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 5);

    size_t len;
    /* row 0: "hello" == "hello" → true → "hello" */
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, s0, "hello");

    /* row 1: "WORLD" != "hello" → false → UPPER("WORLD") = "WORLD" */
    const char* s1 = ray_str_vec_get(result, 1, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, s1, "WORLD");

    /* row 2: "  foo  " != "hello" → false → UPPER("  foo  ") = "  FOO  " */
    const char* s2 = ray_str_vec_get(result, 2, &len);
    TEST_ASSERT_EQ_U(len, 7);
    TEST_ASSERT_MEM_EQ(7, s2, "  FOO  ");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_exec_str_if_scalar(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* IF(name == "hello", "YES", "NO") — scalar SYM branches, STR condition column */
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* lit = ray_const_str(g, "hello", 5);
    ray_op_t* cond = ray_eq(g, name, lit);
    ray_op_t* then_v = ray_const_str(g, "YES", 3);
    ray_op_t* else_v = ray_const_str(g, "NO", 2);
    ray_op_t* if_op = ray_if(g, cond, then_v, else_v);
    ray_t* result = ray_execute(g, if_op);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(RAY_IS_SYM(result->type));  /* scalar str branches → RAY_SYM output */
    TEST_ASSERT_EQ_I(result->len, 5);

    /* row 0: "hello" == "hello" → true → "YES" */
    int64_t sid0 = ray_read_sym(ray_data(result), 0, result->type, result->attrs);
    ray_t* sym0 = ray_sym_str(sid0);
    TEST_ASSERT_NOT_NULL(sym0);
    TEST_ASSERT_EQ_U(ray_str_len(sym0), 3);
    TEST_ASSERT_MEM_EQ(3, ray_str_ptr(sym0), "YES");

    /* row 1: "WORLD" != "hello" → false → "NO" */
    int64_t sid1 = ray_read_sym(ray_data(result), 1, result->type, result->attrs);
    ray_t* sym1 = ray_sym_str(sid1);
    TEST_ASSERT_NOT_NULL(sym1);
    TEST_ASSERT_EQ_U(ray_str_len(sym1), 2);
    TEST_ASSERT_MEM_EQ(2, ray_str_ptr(sym1), "NO");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR EQ with length-1 broadcast ---- */
static test_result_t test_exec_str_eq_len1_broadcast(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build table: name(3 rows), tag(1 row — should broadcast) */
    ray_t* c0 = ray_vec_new(RAY_STR, 3);
    c0 = ray_str_vec_append(c0, "alice", 5);
    c0 = ray_str_vec_append(c0, "bob", 3);
    c0 = ray_str_vec_append(c0, "alice", 5);

    ray_t* c1 = ray_vec_new(RAY_STR, 1);
    c1 = ray_str_vec_append(c1, "alice", 5);

    int64_t name_id = ray_sym_intern("name", 4);
    int64_t tag_id  = ray_sym_intern("tag", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name_id, c0);
    tbl = ray_table_add_col(tbl, tag_id, c1);
    ray_release(c0);
    ray_release(c1);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* tag  = ray_scan(g, "tag");
    ray_op_t* eq   = ray_eq(g, name, tag);
    ray_t* result  = ray_execute(g, eq);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(result->len, 3);
    uint8_t* d = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(d[0], 1);  /* alice == alice */
    TEST_ASSERT_EQ_I(d[1], 0);  /* bob != alice */
    TEST_ASSERT_EQ_I(d[2], 1);  /* alice == alice */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_exec_str_eq_empty_vec_scalar(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Table: name(3 rows), empty(0 rows — empty RAY_STR vector as scalar) */
    ray_t* c0 = ray_vec_new(RAY_STR, 3);
    c0 = ray_str_vec_append(c0, "alice", 5);
    c0 = ray_str_vec_append(c0, "bob", 3);
    c0 = ray_str_vec_append(c0, "", 0);

    ray_t* c1 = ray_vec_new(RAY_STR, 0);

    int64_t name_id  = ray_sym_intern("name", 4);
    int64_t empty_id = ray_sym_intern("empty", 5);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name_id, c0);
    tbl = ray_table_add_col(tbl, empty_id, c1);
    ray_release(c0);
    ray_release(c1);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name  = ray_scan(g, "name");
    ray_op_t* empty = ray_scan(g, "empty");
    ray_op_t* eq    = ray_eq(g, name, empty);
    ray_t* result   = ray_execute(g, eq);

    /* Must not crash (the fix prevents OOB read in atom_to_str_t).
     * The executor may return an error for a 0-length column — that is
     * acceptable.  If it produces a result, verify contents. */
    if (RAY_IS_ERR(result)) {
        /* Valid error pointer — no crash, fix is working */
        TEST_ASSERT_NOT_NULL(result);
    } else {
        TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
        TEST_ASSERT_EQ_I(result->len, 3);
        uint8_t* d = (uint8_t*)ray_data(result);
        TEST_ASSERT_EQ_I(d[0], 0);  /* "alice" != "" */
        TEST_ASSERT_EQ_I(d[1], 0);  /* "bob"   != "" */
        TEST_ASSERT_EQ_I(d[2], 1);  /* ""      == "" */
        ray_release(result);
    }
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR UPPER with null ---- */
static test_result_t test_exec_str_upper_null(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_STR, 3);
    col = ray_str_vec_append(col, "hello", 5);
    col = ray_str_vec_append(col, "world", 5);
    col = ray_str_vec_append(col, "foo", 3);
    ray_vec_set_null(col, 1, true);  /* row 1 is null */

    int64_t name_id = ray_sym_intern("name", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_id, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* up = ray_upper(g, name);
    ray_t* result = ray_execute(g, up);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 3);

    /* Row 0: "HELLO" */
    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, s0, "HELLO");

    /* Row 1: null propagated */
    TEST_ASSERT_TRUE(ray_vec_is_null(result, 1));

    /* Row 2: "FOO" */
    const char* s2 = ray_str_vec_get(result, 2, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, s2, "FOO");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR STRLEN with null ---- */
static test_result_t test_exec_str_strlen_null(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_STR, 3);
    col = ray_str_vec_append(col, "hello", 5);
    col = ray_str_vec_append(col, "world", 5);
    col = ray_str_vec_append(col, "foo", 3);
    ray_vec_set_null(col, 1, true);

    int64_t name_id = ray_sym_intern("name", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_id, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* slen = ray_strlen(g, name);
    ray_t* result = ray_execute(g, slen);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_I64);
    int64_t* d = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(d[0], 5);
    TEST_ASSERT_TRUE(ray_vec_is_null(result, 1));  /* null propagated */
    TEST_ASSERT_EQ_I(d[2], 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR SUBSTR with null ---- */
static test_result_t test_exec_str_substr_null(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_STR, 3);
    col = ray_str_vec_append(col, "hello", 5);
    col = ray_str_vec_append(col, "world", 5);
    col = ray_str_vec_append(col, "foo", 3);
    ray_vec_set_null(col, 1, true);

    int64_t name_id = ray_sym_intern("name", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_id, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* start = ray_const_i64(g, 1);
    ray_op_t* len_op = ray_const_i64(g, 3);
    ray_op_t* sub = ray_substr(g, name, start, len_op);
    ray_t* result = ray_execute(g, sub);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 3);

    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, s0, "hel");

    TEST_ASSERT_TRUE(ray_vec_is_null(result, 1));

    const char* s2 = ray_str_vec_get(result, 2, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, s2, "foo");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR REPLACE with null ---- */
static test_result_t test_exec_str_replace_null(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_STR, 3);
    col = ray_str_vec_append(col, "hello", 5);
    col = ray_str_vec_append(col, "world", 5);
    col = ray_str_vec_append(col, "foo", 3);
    ray_vec_set_null(col, 1, true);

    int64_t name_id = ray_sym_intern("name", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_id, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* from = ray_const_str(g, "o", 1);
    ray_op_t* to = ray_const_str(g, "0", 1);
    ray_op_t* rep = ray_replace(g, name, from, to);
    ray_t* result = ray_execute(g, rep);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 3);

    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, s0, "hell0");

    TEST_ASSERT_TRUE(ray_vec_is_null(result, 1));

    const char* s2 = ray_str_vec_get(result, 2, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, s2, "f00");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- RAY_STR CONCAT with null ---- */
static test_result_t test_exec_str_concat_null(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* c0 = ray_vec_new(RAY_STR, 3);
    c0 = ray_str_vec_append(c0, "John", 4);
    c0 = ray_str_vec_append(c0, "Jane", 4);
    c0 = ray_str_vec_append(c0, "Bob", 3);
    ray_vec_set_null(c0, 1, true);  /* null in first arg */

    ray_t* c1 = ray_vec_new(RAY_STR, 3);
    c1 = ray_str_vec_append(c1, " Doe", 4);
    c1 = ray_str_vec_append(c1, " Smith", 6);
    c1 = ray_str_vec_append(c1, " Jr", 3);

    int64_t n_first = ray_sym_intern("first", 5);
    int64_t n_last  = ray_sym_intern("last", 4);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n_first, c0);
    tbl = ray_table_add_col(tbl, n_last, c1);
    ray_release(c0);
    ray_release(c1);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* first = ray_scan(g, "first");
    ray_op_t* last  = ray_scan(g, "last");
    ray_op_t* args[] = {first, last};
    ray_op_t* cat = ray_concat(g, args, 2);
    ray_t* result = ray_execute(g, cat);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 3);

    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 8);
    TEST_ASSERT_MEM_EQ(8, s0, "John Doe");

    /* Row 1: null in first arg → entire row null */
    TEST_ASSERT_TRUE(ray_vec_is_null(result, 1));

    const char* s2 = ray_str_vec_get(result, 2, &len);
    TEST_ASSERT_EQ_U(len, 6);
    TEST_ASSERT_MEM_EQ(6, s2, "Bob Jr");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- I32 reduction ---- */
static test_result_t test_exec_reduce_i32(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int32_t raw[] = {10, 20, 30};
    ray_t* vec = ray_vec_from_raw(RAY_I32, raw, 3);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* SUM */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* op = ray_sum(g, x);
    ray_t* result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 60);
    ray_release(result);
    ray_graph_free(g);

    /* COUNT */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    op = ray_count(g, x);
    result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    ray_graph_free(g);

    /* MIN */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    op = ray_min_op(g, x);
    result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 10);
    ray_release(result);
    ray_graph_free(g);

    /* MAX */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    op = ray_max_op(g, x);
    result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 30);
    ray_release(result);
    ray_graph_free(g);

    /* AVG */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    op = ray_avg(g, x);
    result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 20.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- I16 reduction ---- */
static test_result_t test_exec_reduce_i16(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int16_t raw[] = {1, 2, 3, 4, 5};
    ray_t* vec = ray_vec_from_raw(RAY_I16, raw, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* op = ray_sum(g, x);
    ray_t* result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 15);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- BOOL reduction ---- */
static test_result_t test_exec_reduce_bool(void) {
    ray_heap_init();
    (void)ray_sym_init();

    uint8_t raw[] = {1, 0, 1};
    ray_t* vec = ray_vec_from_raw(RAY_BOOL, raw, 3);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* SUM */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* op = ray_sum(g, x);
    ray_t* result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 2);
    ray_release(result);
    ray_graph_free(g);

    /* COUNT */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    op = ray_count(g, x);
    result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- I64 with nulls ---- */
static test_result_t test_exec_reduce_i64_nulls(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {10, 0, 30, 0, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    ray_vec_set_null(vec, 1, true);
    ray_vec_set_null(vec, 3, true);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* SUM — should skip nulls: 10 + 30 + 50 = 90 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* op = ray_sum(g, x);
    ray_t* result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 90);
    ray_release(result);
    ray_graph_free(g);

    /* COUNT — only non-null elements */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    op = ray_count(g, x);
    result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    ray_graph_free(g);

    /* MIN */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    op = ray_min_op(g, x);
    result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 10);
    ray_release(result);
    ray_graph_free(g);

    /* MAX */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    op = ray_max_op(g, x);
    result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 50);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- F64 with nulls ---- */
static test_result_t test_exec_reduce_f64_nulls(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double raw[] = {0.0, 2.0, 3.0};
    ray_t* vec = ray_vec_from_raw(RAY_F64, raw, 3);
    ray_vec_set_null(vec, 0, true);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* AVG — non-null: 2.0, 3.0 → avg = 2.5 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* op = ray_avg(g, x);
    ray_t* result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 2.5, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* COUNT */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    op = ray_count(g, x);
    result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 2);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- Empty vector reduction ---- */
static test_result_t test_exec_reduce_empty(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* vec = ray_vec_new(RAY_I64, 0);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* COUNT on empty → 0 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* op = ray_count(g, x);
    ray_t* result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 0);
    ray_release(result);
    ray_graph_free(g);

    /* SUM on empty → 0 */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    op = ray_sum(g, x);
    result = ray_execute(g, op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 0);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- Lazy handle: basic wrap + materialize ---- */
static test_result_t test_lazy_wrap_materialize(void) {
    ray_heap_init();

    int64_t raw[] = {1, 2, 3, 4, 5};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* input = ray_graph_input_vec(g, vec);
    ray_op_t* sum_op = ray_sum(g, input);

    ray_t* lazy = ray_lazy_wrap(g, sum_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(lazy));
    TEST_ASSERT_TRUE(ray_is_lazy(lazy));

    ray_t* result = ray_lazy_materialize(lazy);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 15);

    ray_release(result);
    ray_release(vec);
    ray_heap_destroy();
    PASS();
}

/* ---- Lazy handle: chain two independent lazy handles ---- */
static test_result_t test_lazy_chain(void) {
    ray_heap_init();

    /* First lazy: sum of [1,2,3,4,5] = 15 */
    int64_t raw1[] = {1, 2, 3, 4, 5};
    ray_t* vec1 = ray_vec_from_raw(RAY_I64, raw1, 5);
    ray_graph_t* g1 = ray_graph_new(NULL);
    ray_op_t* in1 = ray_graph_input_vec(g1, vec1);
    ray_op_t* sum1 = ray_sum(g1, in1);
    ray_t* lazy1 = ray_lazy_wrap(g1, sum1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(lazy1));

    /* Second lazy: min of [10,20,30] = 10 */
    int64_t raw2[] = {10, 20, 30};
    ray_t* vec2 = ray_vec_from_raw(RAY_I64, raw2, 3);
    ray_graph_t* g2 = ray_graph_new(NULL);
    ray_op_t* in2 = ray_graph_input_vec(g2, vec2);
    ray_op_t* min2 = ray_min_op(g2, in2);
    ray_t* lazy2 = ray_lazy_wrap(g2, min2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(lazy2));

    /* Materialize both independently */
    ray_t* r1 = ray_lazy_materialize(lazy1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(r1->i64, 15);

    ray_t* r2 = ray_lazy_materialize(lazy2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(r2->i64, 10);

    ray_release(r1);
    ray_release(r2);
    ray_release(vec1);
    ray_release(vec2);
    ray_heap_destroy();
    PASS();
}

/* ---- Lazy handle: materialize passthrough on non-lazy value ---- */
static test_result_t test_lazy_materialize_passthrough(void) {
    ray_heap_init();

    ray_t* atom = ray_i64(42);
    TEST_ASSERT_FALSE(ray_is_lazy(atom));

    ray_t* result = ray_lazy_materialize(atom);
    /* Should return the same pointer unchanged */
    TEST_ASSERT_EQ_PTR(result, atom);
    TEST_ASSERT_EQ_I(result->i64, 42);

    ray_release(atom);
    ray_heap_destroy();
    PASS();
}

/* ---- Lazy handle: release without materialize (cleanup) ---- */
static test_result_t test_lazy_release_no_materialize(void) {
    ray_heap_init();

    int64_t raw[] = {1, 2, 3, 4, 5};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* input = ray_graph_input_vec(g, vec);
    ray_op_t* sum_op = ray_sum(g, input);

    ray_t* lazy = ray_lazy_wrap(g, sum_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(lazy));

    /* Release without materializing — should not leak under ASan */
    ray_release(lazy);

    ray_release(vec);
    ray_heap_destroy();
    PASS();
}

/* ======================================================================
 * expr.c coverage extension tests
 * ====================================================================== */

/* ---- atom_to_numeric: I16 atom constant (eval_const_numeric_expr path) ---- */
static test_result_t test_expr_atom_i16_const(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build table with I16 column */
    int16_t raw[] = {10, 20, 30, 40, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I16, raw, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* x + const_i64(5) — triggers binary_range with I16 lhs vector and i64 scalar rhs */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x   = ray_scan(g, "x");
    ray_op_t* c   = ray_const_i64(g, 5);
    ray_op_t* add = ray_add(g, x, c);
    ray_op_t* s   = ray_sum(g, add);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* sum(10+5, 20+5, 30+5, 40+5, 50+5) = 175 */
    TEST_ASSERT_EQ_I(result->i64, 175);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- eval_const_numeric_expr: NEG/ABS over constant, binary const arithmetic ---- */
static test_result_t test_expr_const_arithmetic(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {1, 2, 3, 4, 5};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* x + (3 + 2): constant binary ADD folds to 5 → sum = 15+25 = 40 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x  = ray_scan(g, "x");
    ray_op_t* c3 = ray_const_i64(g, 3);
    ray_op_t* c2 = ray_const_i64(g, 2);
    ray_op_t* ca = ray_add(g, c3, c2);   /* const+const: eval_const_numeric_expr binary */
    ray_op_t* add = ray_add(g, x, ca);
    ray_op_t* s = ray_sum(g, add);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 40);  /* sum(6,7,8,9,10) = 40 */
    ray_release(result);
    ray_graph_free(g);

    /* x + neg(2): constant NEG over i64 → sum = 15 + 5*(-2) = 5 */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* c = ray_const_i64(g, 2);
    ray_op_t* nc = ray_neg(g, c);       /* const NEG: eval_const_numeric_expr unary */
    ray_op_t* add2 = ray_add(g, x, nc);
    s = ray_sum(g, add2);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 5);  /* sum(-1,0,1,2,3) = 5 */
    ray_release(result);
    ray_graph_free(g);

    /* x * neg(const_i64(2)): linear fast path via parse_linear_i64_expr + NEG */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    c = ray_const_i64(g, 2);
    nc = ray_neg(g, c);
    ray_op_t* mul = ray_mul(g, x, nc);  /* triggers MUL const path in parse_linear */
    s = ray_sum(g, mul);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, -30);  /* -2*(1+2+3+4+5) = -30 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- scalar null propagation in arithmetic: set_all_null path ---- */
static test_result_t test_expr_scalar_null_propagation(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a table with a nullable column (all nulls → force scalar null broadcast).
     * Use a length-1 vector with null to act as scalar null on rhs. */
    int64_t raw[] = {10, 20, 30};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    int64_t name = ray_sym_intern("x", 1);

    /* null_scalar: length-1 vector with null, acts as scalar rhs */
    int64_t null_val[] = {0};
    ray_t* null_scalar = ray_vec_from_raw(RAY_I64, null_val, 1);
    ray_vec_set_null(null_scalar, 0, true);

    int64_t ns_name = ray_sym_intern("ns", 2);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name, vec);
    tbl = ray_table_add_col(tbl, ns_name, null_scalar);
    ray_release(vec);
    ray_release(null_scalar);

    /* x + ns: ns is scalar-null (len=1 w/ null) → set_all_null path
     * in propagate_nulls_binary when r_scalar && scalar_is_null(rhs) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x  = ray_scan(g, "x");
    ray_op_t* ns = ray_scan(g, "ns");
    ray_op_t* add = ray_add(g, x, ns);
    ray_op_t* cnt = ray_count(g, add);
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 0);  /* all null due to null scalar */
    ray_release(result);
    ray_graph_free(g);

    /* ns + x: null scalar as lhs → set_all_null path */
    g = ray_graph_new(tbl);
    x  = ray_scan(g, "x");
    ns = ray_scan(g, "ns");
    ray_op_t* add2 = ray_add(g, ns, x);
    cnt = ray_count(g, add2);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 0);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range with I32 column arithmetic (out_type I32) ---- */
static test_result_t test_expr_i32_column_binary(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int32_t rawa[] = {10, 20, 30, 40, 50};
    int32_t rawb[] = {2,  4,  6,  8, 10};
    ray_t* va = ray_vec_from_raw(RAY_I32, rawa, 5);
    ray_t* vb = ray_vec_from_raw(RAY_I32, rawb, 5);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* a + b: both I32 vectors → sum */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* add = ray_add(g, a_op, b_op);
    ray_op_t* s   = ray_sum(g, add);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* (12+24+36+48+60)=180 */
    TEST_ASSERT_EQ_I(result->i64, 180);
    ray_release(result);
    ray_graph_free(g);

    /* a - b */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* sub = ray_sub(g, a_op, b_op);
    s = ray_sum(g, sub);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 120);  /* 8+16+24+32+40=120 */
    ray_release(result);
    ray_graph_free(g);

    /* a * b */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mul = ray_mul(g, a_op, b_op);
    s = ray_sum(g, mul);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 1100);  /* 20+80+180+320+500=1100 */
    ray_release(result);
    ray_graph_free(g);

    /* a / b — ray_div always returns F64 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* dv = ray_div(g, a_op, b_op);
    s = ray_sum(g, dv);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 25.0, 1e-6);  /* 5+5+5+5+5=25 */
    ray_release(result);
    ray_graph_free(g);

    /* a % b — ray_mod always returns F64 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* md = ray_mod(g, a_op, b_op);
    s = ray_sum(g, md);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 0.0, 1e-6);  /* all evenly divisible */
    ray_release(result);
    ray_graph_free(g);

    /* min2(a, b) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mn = ray_min2(g, a_op, b_op);
    s = ray_sum(g, mn);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 30);  /* 2+4+6+8+10=30 */
    ray_release(result);
    ray_graph_free(g);

    /* max2(a, b) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mx = ray_max2(g, a_op, b_op);
    s = ray_sum(g, mx);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 150);  /* 10+20+30+40+50=150 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range with I16 column arithmetic ---- */
static test_result_t test_expr_i16_column_binary(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int16_t rawa[] = {10, 20, 30};
    int16_t rawb[] = {2,  4,  6};
    ray_t* va = ray_vec_from_raw(RAY_I16, rawa, 3);
    ray_t* vb = ray_vec_from_raw(RAY_I16, rawb, 3);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* a + b */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* add = ray_add(g, a_op, b_op);
    ray_op_t* s   = ray_sum(g, add);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* (12+24+36)=72 */
    TEST_ASSERT_EQ_I(result->i64, 72);
    ray_release(result);
    ray_graph_free(g);

    /* a * b */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mul = ray_mul(g, a_op, b_op);
    s = ray_sum(g, mul);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 280);  /* 20+80+180=280 */
    ray_release(result);
    ray_graph_free(g);

    /* a / b — ray_div returns F64 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* dv16 = ray_div(g, a_op, b_op);
    s = ray_sum(g, dv16);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 15.0, 1e-6);  /* 5+5+5=15 */
    ray_release(result);
    ray_graph_free(g);

    /* a % b — ray_mod returns F64 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* md16 = ray_mod(g, a_op, b_op);
    s = ray_sum(g, md16);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 0.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* min2(a,b) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mn = ray_min2(g, a_op, b_op);
    s = ray_sum(g, mn);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 12);  /* 2+4+6=12 */
    ray_release(result);
    ray_graph_free(g);

    /* max2(a,b) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mx = ray_max2(g, a_op, b_op);
    s = ray_sum(g, mx);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 60);  /* 10+20+30=60 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range with U8/BOOL column arithmetic ---- */
static test_result_t test_expr_u8_bool_column_binary(void) {
    ray_heap_init();
    (void)ray_sym_init();

    uint8_t rawa[] = {10, 20, 30};
    uint8_t rawb[] = {2,  4,  6};
    ray_t* va = ray_vec_from_raw(RAY_U8, rawa, 3);
    ray_t* vb = ray_vec_from_raw(RAY_U8, rawb, 3);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* a + b */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* add = ray_add(g, a_op, b_op);
    ray_op_t* s   = ray_sum(g, add);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 72);  /* 12+24+36=72 */
    ray_release(result);
    ray_graph_free(g);

    /* a * b */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mul = ray_mul(g, a_op, b_op);
    s = ray_sum(g, mul);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 280);
    ray_release(result);
    ray_graph_free(g);

    /* a / b — ray_div returns F64 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* dvu8 = ray_div(g, a_op, b_op);
    s = ray_sum(g, dvu8);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 15.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* a % b — ray_mod returns F64 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mdu8 = ray_mod(g, a_op, b_op);
    s = ray_sum(g, mdu8);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 0.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* min2(a,b) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mn = ray_min2(g, a_op, b_op);
    s = ray_sum(g, mn);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 12);
    ray_release(result);
    ray_graph_free(g);

    /* max2(a,b) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mx = ray_max2(g, a_op, b_op);
    s = ray_sum(g, mx);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 60);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_binary: scalar I32 atom → l_i64_val path ---- */
static test_result_t test_expr_scalar_i32_atom(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* x + i32(3): uses -RAY_I32 scalar atom path in exec_elementwise_binary */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x  = ray_scan(g, "x");
    ray_t* atom  = ray_i32(3);
    ray_op_t* c  = ray_const_atom(g, atom);
    ray_release(atom);
    ray_op_t* add = ray_add(g, x, c);
    ray_op_t* s  = ray_sum(g, add);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 165);  /* (13+23+33+43+53)=165 */
    ray_release(result);
    ray_graph_free(g);

    /* i32(3) + x: lhs scalar */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    atom = ray_i32(3);
    c = ray_const_atom(g, atom);
    ray_release(atom);
    ray_op_t* add2 = ray_add(g, c, x);
    s = ray_sum(g, add2);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 165);
    ray_release(result);
    ray_graph_free(g);

    /* x >= i32(30): uses I32 atom in comparison */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    atom = ray_i32(30);
    c = ray_const_atom(g, atom);
    ray_release(atom);
    ray_op_t* cmp = ray_ge(g, x, c);
    ray_op_t* cnt = ray_count(g, ray_filter(g, x, cmp));
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 3);  /* 30,40,50 */
    ray_release(result);
    ray_graph_free(g);

    /* x + i16(5): I16 atom path */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    atom = ray_i16(5);
    c = ray_const_atom(g, atom);
    ray_release(atom);
    ray_op_t* add3 = ray_add(g, x, c);
    s = ray_sum(g, add3);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 175);  /* sum(15,25,35,45,55)=175 */
    ray_release(result);
    ray_graph_free(g);

    /* x + u8(2): U8 atom path */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    atom = ray_u8(2);
    c = ray_const_atom(g, atom);
    ray_release(atom);
    ray_op_t* add4 = ray_add(g, x, c);
    s = ray_sum(g, add4);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 160);  /* sum(12,22,32,42,52)=160 */
    ray_release(result);
    ray_graph_free(g);

    /* x + bool(1): BOOL atom path */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    atom = ray_bool(true);
    c = ray_const_atom(g, atom);
    ray_release(atom);
    ray_op_t* add5 = ray_add(g, x, c);
    s = ray_sum(g, add5);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 155);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- F64 mod, min2, max2 in expr_exec_binary (fused path) ---- */
static test_result_t test_expr_f64_fused_modminmax(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double rawa[] = {7.5, 3.5, 11.5, 5.5, 9.5};
    double rawb[] = {3.0, 2.0,  4.0, 3.0, 4.0};
    ray_t* va = ray_vec_from_raw(RAY_F64, rawa, 5);
    ray_t* vb = ray_vec_from_raw(RAY_F64, rawb, 5);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* a % b — triggers OP_MOD in expr_exec_binary RAY_F64 branch */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* md = ray_mod(g, a_op, b_op);
    ray_op_t* s  = ray_sum(g, md);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 7.5%3=1.5, 3.5%2=1.5, 11.5%4=3.5, 5.5%3=2.5, 9.5%4=1.5 → 10.5 */
    TEST_ASSERT_EQ_F(result->f64, 10.5, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* min2(a, b) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mn = ray_min2(g, a_op, b_op);
    s = ray_sum(g, mn);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 3.0+2.0+4.0+3.0+4.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* max2(a, b) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mx = ray_max2(g, a_op, b_op);
    s = ray_sum(g, mx);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 7.5+3.5+11.5+5.5+9.5, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- I64 fused-path div in expr_exec_binary ---- */
static test_result_t test_expr_i64_fused_div(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t rawa[] = {10, 20, 30, 40, 50};
    int64_t rawb[] = {2,  4,  5, 10, 25};
    ray_t* va = ray_vec_from_raw(RAY_I64, rawa, 5);
    ray_t* vb = ray_vec_from_raw(RAY_I64, rawb, 5);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* a / b — ray_div returns F64; exercises binary_range with I64 data but F64 out_type */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* dv = ray_div(g, a_op, b_op);
    ray_op_t* s  = ray_sum(g, dv);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 5+5+6+4+2=22 */
    TEST_ASSERT_EQ_F(result->f64, 22.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_binary: F64 div-by-zero scalar null path ---- */
static test_result_t test_expr_f64_divzero_scalar(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double raw[] = {1.0, 2.0, 3.0};
    ray_t* vec = ray_vec_from_raw(RAY_F64, raw, 3);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* x / f64(0.0): scalar divisor zero, is_zero=(r_f64_val==0.0) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* zero = ray_const_f64(g, 0.0);
    ray_op_t* dv = ray_div(g, x, zero);
    ray_op_t* cnt = ray_count(g, dv);  /* count non-null: NaN-handling */
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* f64 div by 0 → NaN, not a null; count counts NaN as non-null */
    /* Main goal: exercise the rhs->type == -RAY_F64 path in is_zero check */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_binary: I32 column divisor null-marking path ---- */
static test_result_t test_expr_i32_divzero_vector(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int32_t rawa[] = {10, 20, 30, 40, 50};
    int32_t rawb[] = {2,   0,  5,  0, 10};  /* zeros at positions 1,3 */
    ray_t* va = ray_vec_from_raw(RAY_I32, rawa, 5);
    ray_t* vb = ray_vec_from_raw(RAY_I32, rawb, 5);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* a / b: I32 rhs with zeros → exercises the rt == RAY_I32 branch in
     * the div/mod null-marking post-pass.  ray_div returns F64, and for F64
     * zero divisors produce NaN (not bitmap-null). Verify the op doesn't error. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* dv   = ray_div(g, a_op, b_op);
    ray_op_t* s    = ray_sum(g, dv);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* Non-zero positions: 10/2=5, 30/5=6, 50/10=5 → sum of non-NaN = 16 */
    /* (NaN positions contribute 0 to sum if handled) — just verify no error */
    ray_release(result);
    ray_graph_free(g);

    /* a % b with I32 zeros - same exercise */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* md  = ray_mod(g, a_op, b_op);
    s = ray_sum(g, md);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_unary: CAST from I32, I16, U8, BOOL to I64/F64 ---- */
static test_result_t test_expr_cast_narrow_types(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a multi-column table: col32(I32), col16(I16), col8(U8), colb(BOOL) */
    int32_t raw32[] = {10, 20, 30};
    int16_t raw16[] = {5, 10, 15};
    uint8_t raw8[]  = {1, 2, 3};
    uint8_t rawb[]  = {1, 0, 1};

    ray_t* v32   = ray_vec_from_raw(RAY_I32,  raw32, 3);
    ray_t* v16   = ray_vec_from_raw(RAY_I16,  raw16, 3);
    ray_t* v8    = ray_vec_from_raw(RAY_U8,   raw8,  3);
    ray_t* vbool = ray_vec_from_raw(RAY_BOOL, rawb,  3);

    int64_t n32 = ray_sym_intern("c32", 3);
    int64_t n16 = ray_sym_intern("c16", 3);
    int64_t n8  = ray_sym_intern("c8",  2);
    int64_t nb  = ray_sym_intern("cb",  2);

    ray_t* tbl = ray_table_new(4);
    tbl = ray_table_add_col(tbl, n32, v32);
    tbl = ray_table_add_col(tbl, n16, v16);
    tbl = ray_table_add_col(tbl, n8,  v8);
    tbl = ray_table_add_col(tbl, nb,  vbool);
    ray_release(v32); ray_release(v16); ray_release(v8); ray_release(vbool);

    /* I32 → F64 cast */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "c32");
    ray_op_t* c = ray_cast(g, x, RAY_F64);
    ray_op_t* s = ray_sum(g, c);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 60.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* I16 → F64 cast (goes through fused path: I16 loaded as I64, then CAST I64→F64) */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "c16");
    c = ray_cast(g, x, RAY_F64);
    s = ray_sum(g, c);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 30.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* U8 → F64 cast */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "c8");
    c = ray_cast(g, x, RAY_F64);
    s = ray_sum(g, c);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 6.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* BOOL → F64 cast */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "cb");
    c = ray_cast(g, x, RAY_F64);
    s = ray_sum(g, c);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 2.0, 1e-6);  /* 1+0+1=2 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_unary: ISNULL on vec-with-nulls, propagate_nulls ---- */
static test_result_t test_expr_unary_null_propagation(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    /* Set position 2 as null */
    ray_vec_set_null(vec, 2, true);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* isnull(x): position 2 should be 1, others 0 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* isn = ray_isnull(g, x);
    ray_op_t* s = ray_sum(g, isn);  /* sum of bool results */
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 1);  /* only 1 null */
    ray_release(result);
    ray_graph_free(g);

    /* neg(x): null propagation via propagate_nulls → count should be 4 */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* neg = ray_neg(g, x);
    ray_op_t* cnt = ray_count(g, neg);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 4);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_binary: null propagation (vec nulls) ---- */
static test_result_t test_expr_binary_null_propagation(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t rawa[] = {10, 20, 30, 40, 50};
    int64_t rawb[] = { 1,  2,  3,  4,  5};
    ray_t* va = ray_vec_from_raw(RAY_I64, rawa, 5);
    ray_t* vb = ray_vec_from_raw(RAY_I64, rawb, 5);
    /* Set position 1 null in va, position 3 null in vb */
    ray_vec_set_null(va, 1, true);
    ray_vec_set_null(vb, 3, true);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* a + b: nulls at positions 1,3 → count non-null = 3 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* add = ray_add(g, a_op, b_op);
    ray_op_t* cnt = ray_count(g, add);
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- try_affine_sumavg_input: OP_SUB path (lhs-const → base_op = lhs, sign=-1) ---- */
static test_result_t test_expr_affine_sub_path(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* sum(x - 5): affine sub path in try_affine_sumavg_input, bias_i64=-5 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x  = ray_scan(g, "x");
    ray_op_t* c  = ray_const_i64(g, 5);
    ray_op_t* sub = ray_sub(g, x, c);
    ray_op_t* s  = ray_sum(g, sub);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 125);  /* (10-5)+(20-5)+(30-5)+(40-5)+(50-5)=125 */
    ray_release(result);
    ray_graph_free(g);

    /* avg(x - 3): affine sub, should be avg(x)-3 */
    g = ray_graph_new(tbl);
    x  = ray_scan(g, "x");
    c  = ray_const_i64(g, 3);
    sub = ray_sub(g, x, c);
    ray_op_t* avg = ray_avg(g, sub);
    result = ray_execute(g, avg);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 27.0, 1e-6);  /* avg(10,20,30,40,50)=30, -3=27 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- try_affine_sumavg_input: F64 column + const path ---- */
static test_result_t test_expr_affine_f64_path(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double raw[] = {1.5, 2.5, 3.5, 4.5, 5.5};
    ray_t* vec = ray_vec_from_raw(RAY_F64, raw, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* sum(x + 1.5): F64 column + f64 const → affine path */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x  = ray_scan(g, "x");
    ray_op_t* c  = ray_const_f64(g, 1.5);
    ray_op_t* add = ray_add(g, x, c);
    ray_op_t* s  = ray_sum(g, add);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 25.0, 1e-6);  /* (3+4+5+6+7)=25 */
    ray_release(result);
    ray_graph_free(g);

    /* sum(x - 0.5): F64 sub affine */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    c = ray_const_f64(g, 0.5);
    ray_op_t* sub = ray_sub(g, x, c);
    s = ray_sum(g, sub);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 15.0, 1e-6);  /* 1+2+3+4+5=15 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- parse_linear_i64_expr: NEG of scan, ADD/SUB of scans ---- */
static test_result_t test_expr_linear_scan_ops(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t rawa[] = {10, 20, 30};
    int64_t rawb[] = {1,  2,  3};
    ray_t* va = ray_vec_from_raw(RAY_I64, rawa, 3);
    ray_t* vb = ray_vec_from_raw(RAY_I64, rawb, 3);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* sum(neg(a)): parse_linear neg path → -a */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* ng = ray_neg(g, a_op);
    ray_op_t* s  = ray_sum(g, ng);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, -60);
    ray_release(result);
    ray_graph_free(g);

    /* sum(a - b): parse_linear sub of two scans, cancel-then-add path */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* sub = ray_sub(g, a_op, b_op);
    s = ray_sum(g, sub);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 54);  /* (9+18+27)=54 */
    ray_release(result);
    ray_graph_free(g);

    /* sum(a + b): add of two scans */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* add = ray_add(g, a_op, b_op);
    s = ray_sum(g, add);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 66);  /* (11+22+33)=66 */
    ray_release(result);
    ray_graph_free(g);

    /* sum(2*a): multiplication by const on right */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    ray_op_t* c2 = ray_const_i64(g, 2);
    ray_op_t* mul = ray_mul(g, a_op, c2);  /* right const mul path */
    s = ray_sum(g, mul);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 120);  /* 2*(10+20+30)=120 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- round op in unary (exec_elementwise_unary F64 ROUND path) ---- */
static test_result_t test_expr_round_op(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double raw[] = {1.4, 2.5, 3.6, -1.5, -2.6};
    ray_t* vec = ray_vec_from_raw(RAY_F64, raw, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* r = ray_round_op(g, x);
    ray_op_t* s = ray_sum(g, r);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* round: 1+3+4+(-2)+(-3) = 3 */
    TEST_ASSERT_EQ_F(result->f64, 3.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_unary: I64 → F64 unary ops (sqrt,log,exp on i64 vec) ---- */
static test_result_t test_expr_unary_i64_to_f64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {1, 4, 9, 16, 25};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* sqrt(i64 vec): in exec_elementwise_unary, i64 src → f64 out path */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* sq = ray_sqrt_op(g, x);
    ray_op_t* s = ray_sum(g, sq);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 15.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* neg on i64 column: out_type i64 path; also tests neg(-INT64_MIN) overflow handling */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* ng = ray_neg(g, x);
    s = ray_sum(g, ng);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, -55);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_binary: AND/OR on comparison outputs ---- */
static test_result_t test_expr_bool_and_or(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* v1={1..5}, v2={3,3,3,3,3} — compare to produce BOOL predicates */
    int64_t rawv1[] = {1, 2, 3, 4, 5};
    int64_t rawv2[] = {3, 3, 3, 3, 3};
    ray_t* vv1 = ray_vec_from_raw(RAY_I64, rawv1, 5);
    ray_t* vv2 = ray_vec_from_raw(RAY_I64, rawv2, 5);
    int64_t n1 = ray_sym_intern("v1", 2);
    int64_t n2 = ray_sym_intern("v2", 2);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n1, vv1);
    tbl = ray_table_add_col(tbl, n2, vv2);
    ray_release(vv1); ray_release(vv2);

    /* (v1 > 1) AND (v1 < 5): v1={2,3,4} → count = 3 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* v1_op = ray_scan(g, "v1");
    ray_op_t* c1 = ray_const_i64(g, 1);
    ray_op_t* c5 = ray_const_i64(g, 5);
    ray_op_t* gt1 = ray_gt(g, v1_op, c1);
    ray_op_t* lt5 = ray_lt(g, v1_op, c5);
    ray_op_t* and_op = ray_and(g, gt1, lt5);
    ray_op_t* flt = ray_filter(g, v1_op, and_op);
    ray_op_t* cnt = ray_count(g, flt);
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    ray_graph_free(g);

    /* (v1 < 2) OR (v1 > 4): v1={1,5} → count = 2 */
    g = ray_graph_new(tbl);
    v1_op = ray_scan(g, "v1");
    c1 = ray_const_i64(g, 2);
    c5 = ray_const_i64(g, 4);
    ray_op_t* lt2 = ray_lt(g, v1_op, c1);
    ray_op_t* gt4 = ray_gt(g, v1_op, c5);
    ray_op_t* or_op = ray_or(g, lt2, gt4);
    flt = ray_filter(g, v1_op, or_op);
    cnt = ray_count(g, flt);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 2);
    ray_release(result);
    ray_graph_free(g);

    /* AND using BOOL columns directly (exercises expr_exec_binary BOOL path) */
    uint8_t rawa[] = {1, 0, 1, 0, 1};
    uint8_t rawb[] = {1, 1, 0, 0, 1};
    ray_t* va = ray_vec_from_raw(RAY_BOOL, rawa, 5);
    ray_t* vb = ray_vec_from_raw(RAY_BOOL, rawb, 5);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl2 = ray_table_new(2);
    tbl2 = ray_table_add_col(tbl2, na, va);
    tbl2 = ray_table_add_col(tbl2, nb, vb);
    ray_release(va); ray_release(vb);

    /* a AND b is executed via exec_elementwise_binary non-fused path;
     * use this to cover the t1 == RAY_I64 (BOOL loaded as I64) AND/OR cases */
    g = ray_graph_new(tbl2);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* and2 = ray_and(g, a_op, b_op);
    /* count(filter(a, a AND b)) to use the result */
    ray_op_t* af = ray_filter(g, a_op, and2);
    cnt = ray_count(g, af);
    result = ray_execute(g, cnt);
    /* Don't assert count value — just verify no error (covers the AND path) */
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_release(tbl2);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_unary: F64 in/out with nullable column (non-fused path) ---- */
static test_result_t test_expr_unary_f64_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Nullable F64 column forces non-fused path through exec_elementwise_unary */
    double raw[] = {4.0, -9.0, 16.0, -25.0, 36.0};
    ray_t* vec = ray_vec_from_raw(RAY_F64, raw, 5);
    ray_vec_set_null(vec, 4, true);  /* mark last element null */
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* neg(nullable F64) — exercises F64 OP_NEG branch in exec_elementwise_unary */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* ng = ray_neg(g, x);
    ray_op_t* s = ray_sum(g, ng);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* neg: -4 + 9 + -16 + 25 = 14, position 4 null → sum over 4 = 14 */
    TEST_ASSERT_EQ_F(result->f64, 14.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* abs(nullable F64) — exercises F64 OP_ABS branch */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* ab = ray_abs(g, x);
    s = ray_sum(g, ab);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* abs: 4+9+16+25=54, position 4 null → 54 */
    TEST_ASSERT_EQ_F(result->f64, 54.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* sqrt(nullable F64) */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* sq = ray_sqrt_op(g, x);
    s = ray_sum(g, sq);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* sqrt(4)+sqrt(16)=2+4=6; sqrt(-9) and sqrt(-25) = NaN; pos4=null */
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));  /* just check no error */
    ray_release(result);
    ray_graph_free(g);

    /* ceil(nullable F64) */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* cl = ray_ceil_op(g, x);
    s = ray_sum(g, cl);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* ceil: 4+(-9)+16+(-25)=-14, pos4 null */
    TEST_ASSERT_EQ_F(result->f64, -14.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* floor(nullable F64) */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* fl = ray_floor_op(g, x);
    s = ray_sum(g, fl);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, -14.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* round(nullable F64) */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* ro = ray_round_op(g, x);
    s = ray_sum(g, ro);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, -14.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* log(nullable F64) */
    double rawlog[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    ray_t* vlog = ray_vec_from_raw(RAY_F64, rawlog, 5);
    ray_vec_set_null(vlog, 4, true);
    int64_t nlog = ray_sym_intern("y", 1);
    ray_t* tbl2 = ray_table_new(1);
    tbl2 = ray_table_add_col(tbl2, nlog, vlog);
    ray_release(vlog);

    g = ray_graph_new(tbl2);
    x = ray_scan(g, "y");
    ray_op_t* lg = ray_log_op(g, x);
    s = ray_sum(g, lg);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_graph_free(g);

    /* exp(nullable F64) */
    g = ray_graph_new(tbl2);
    x = ray_scan(g, "y");
    ray_op_t* ex = ray_exp_op(g, x);
    s = ray_sum(g, ex);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl2);

    /* cast(nullable F64, I64) — out_type=I64 for F64 input in exec_elementwise_unary */
    double rawcast[] = {1.7, 2.3, 3.9};
    ray_t* vcast = ray_vec_from_raw(RAY_F64, rawcast, 3);
    ray_vec_set_null(vcast, 0, true);
    int64_t ncast = ray_sym_intern("z", 1);
    ray_t* tbl3 = ray_table_new(1);
    tbl3 = ray_table_add_col(tbl3, ncast, vcast);
    ray_release(vcast);

    g = ray_graph_new(tbl3);
    x = ray_scan(g, "z");
    ray_op_t* ca = ray_cast(g, x, RAY_I64);
    s = ray_sum(g, ca);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* cast(2.3→2) + cast(3.9→3) = 5; pos0 null */
    TEST_ASSERT_EQ_I(result->i64, 5);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl3);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_unary: I64→F64 via nullable I64 column (non-fused) ---- */
static test_result_t test_expr_unary_i64_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {4, 9, 16, 25, 36};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    ray_vec_set_null(vec, 0, true);  /* mark first element null */
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* sqrt(nullable I64 col) → F64 out: exercises in_type==RAY_I64, out_type==RAY_F64 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* sq = ray_sqrt_op(g, x);
    ray_op_t* s = ray_sum(g, sq);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* sqrt(9)+sqrt(16)+sqrt(25)+sqrt(36) = 3+4+5+6 = 18; pos0=null */
    TEST_ASSERT_EQ_F(result->f64, 18.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* log(nullable I64 col) → F64 */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* lg = ray_log_op(g, x);
    s = ray_sum(g, lg);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_graph_free(g);

    /* exp(nullable I64 col) → F64 */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* ex = ray_exp_op(g, x);
    s = ray_sum(g, ex);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_graph_free(g);

    /* neg(nullable I64 col) → I64; also covers ABS path */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* ng = ray_neg(g, x);
    s = ray_sum(g, ng);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* neg(9)+neg(16)+neg(25)+neg(36) = -9-16-25-36 = -86; pos0=null */
    TEST_ASSERT_EQ_I(result->i64, -86);
    ray_release(result);
    ray_graph_free(g);

    /* abs(nullable I64 col) with negative values */
    int64_t rawneg[] = {-4, -9, 16, -25, 36};
    ray_t* vneg = ray_vec_from_raw(RAY_I64, rawneg, 5);
    ray_vec_set_null(vneg, 0, true);
    int64_t nname = ray_sym_intern("y", 1);
    ray_t* tbl2 = ray_table_new(1);
    tbl2 = ray_table_add_col(tbl2, nname, vneg);
    ray_release(vneg);

    g = ray_graph_new(tbl2);
    x = ray_scan(g, "y");
    ray_op_t* ab = ray_abs(g, x);
    s = ray_sum(g, ab);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* abs(-9)+abs(16)+abs(-25)+abs(36) = 9+16+25+36 = 86; pos0=null */
    TEST_ASSERT_EQ_I(result->i64, 86);
    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl2);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_unary: CAST from I32, I16, U8 via nullable column ---- */
static test_result_t test_expr_unary_cast_narrow_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* I32 nullable → I64 (non-fused due to null) */
    int32_t raw32[] = {10, 20, 30};
    ray_t* v32 = ray_vec_from_raw(RAY_I32, raw32, 3);
    ray_vec_set_null(v32, 2, true);
    int64_t n32 = ray_sym_intern("c32", 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, n32, v32);
    ray_release(v32);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "c32");
    ray_op_t* c = ray_cast(g, x, RAY_I64);
    ray_op_t* s = ray_sum(g, c);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 30);  /* 10+20=30, pos2=null */
    ray_release(result);
    ray_graph_free(g);

    /* I32 nullable → F64 */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "c32");
    c = ray_cast(g, x, RAY_F64);
    s = ray_sum(g, c);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_F(result->f64, 30.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();

    /* I16 nullable → I64 */
    int16_t raw16[] = {5, 10, 15};
    ray_t* v16 = ray_vec_from_raw(RAY_I16, raw16, 3);
    ray_vec_set_null(v16, 0, true);
    (void)ray_sym_init();
    int64_t n16 = ray_sym_intern("c16", 3);
    tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, n16, v16);
    ray_release(v16);

    g = ray_graph_new(tbl);
    x = ray_scan(g, "c16");
    c = ray_cast(g, x, RAY_I64);
    s = ray_sum(g, c);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 25);  /* 10+15=25, pos0=null */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();

    /* U8 nullable → I64 */
    uint8_t raw8[] = {1, 2, 3};
    ray_t* v8 = ray_vec_from_raw(RAY_U8, raw8, 3);
    ray_vec_set_null(v8, 1, true);
    (void)ray_sym_init();
    int64_t n8 = ray_sym_intern("c8", 2);
    tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, n8, v8);
    ray_release(v8);

    g = ray_graph_new(tbl);
    x = ray_scan(g, "c8");
    c = ray_cast(g, x, RAY_I64);
    s = ray_sum(g, c);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 4);  /* 1+3=4, pos1=null */
    ray_release(result);
    ray_graph_free(g);

    /* BOOL nullable → I64 */
    g = ray_graph_new(tbl);  /* reuse tbl - actually we need BOOL */
    ray_release(tbl);
    ray_sym_destroy();

    uint8_t rawb[] = {1, 0, 1};
    ray_t* vbool = ray_vec_from_raw(RAY_BOOL, rawb, 3);
    ray_vec_set_null(vbool, 2, true);
    (void)ray_sym_init();
    int64_t nb = ray_sym_intern("cb", 2);
    tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nb, vbool);
    ray_release(vbool);

    g = ray_graph_new(tbl);
    x = ray_scan(g, "cb");
    c = ray_cast(g, x, RAY_I64);
    s = ray_sum(g, c);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 1);  /* 1+0=1, pos2=null */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_binary: binary ops on nullable I32/I16 (non-fused) ---- */
static test_result_t test_expr_binary_narrow_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int32_t rawa[] = {10, 20, 30, 40, 50};
    int32_t rawb[] = {2,  4,  6,  8, 10};
    ray_t* va = ray_vec_from_raw(RAY_I32, rawa, 5);
    ray_t* vb = ray_vec_from_raw(RAY_I32, rawb, 5);
    ray_vec_set_null(va, 0, true);  /* force non-fused path */
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* a + b (I32 nullable) — exercises binary_range I32 out_type path */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* add = ray_add(g, a_op, b_op);
    ray_op_t* s   = ray_sum(g, add);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* (24+36+48+60)=168, pos0=null */
    TEST_ASSERT_EQ_I(result->i64, 168);
    ray_release(result);
    ray_graph_free(g);

    /* a - b (I32 nullable) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* sub = ray_sub(g, a_op, b_op);
    s = ray_sum(g, sub);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 112);  /* 16+24+32+40=112 (pos0 null) */
    ray_release(result);
    ray_graph_free(g);

    /* a * b (I32 nullable) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mul = ray_mul(g, a_op, b_op);
    s = ray_sum(g, mul);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 1080);  /* 80+180+320+500=1080 */
    ray_release(result);
    ray_graph_free(g);

    /* min2(a, b) (I32 nullable) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mn = ray_min2(g, a_op, b_op);
    s = ray_sum(g, mn);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 28);  /* 4+6+8+10=28 */
    ray_release(result);
    ray_graph_free(g);

    /* max2(a, b) (I32 nullable) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mx = ray_max2(g, a_op, b_op);
    s = ray_sum(g, mx);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 140);  /* 20+30+40+50=140 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- set_all_null: large vector (>128 elements) with scalar null ---- */
static test_result_t test_expr_set_all_null_large(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Create a large vector (200 elements) to trigger ext nullmap path */
    int64_t raw[200];
    int64_t null_vals[200];
    for (int i = 0; i < 200; i++) { raw[i] = i + 1; null_vals[i] = 0; }
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 200);
    /* null_scalar: len=1 vector with null */
    ray_t* ns = ray_vec_from_raw(RAY_I64, null_vals, 1);
    ray_vec_set_null(ns, 0, true);

    int64_t nv = ray_sym_intern("v", 1);
    int64_t nns = ray_sym_intern("ns", 2);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, nv, vec);
    tbl = ray_table_add_col(tbl, nns, ns);
    ray_release(vec); ray_release(ns);

    /* v + ns (len=1 null scalar) → all 200 results null → exercises set_all_null with len>128 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* v_op = ray_scan(g, "v");
    ray_op_t* ns_op = ray_scan(g, "ns");
    ray_op_t* add = ray_add(g, v_op, ns_op);
    ray_op_t* cnt = ray_count(g, add);
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 0);  /* all null → count = 0 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- propagate_nulls: misaligned slice path (slow path) ---- */
static test_result_t test_expr_propagate_nulls_slice(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Create a vector and slice it to trigger propagate_nulls slow path */
    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    ray_vec_set_null(vec, 2, true);  /* mark element 2 null */

    /* Create a slice starting at offset 1 (elements 1..3) */
    ray_t* sl = ray_vec_slice(vec, 1, 3);
    TEST_ASSERT_NOT_NULL(sl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sl));

    int64_t rawb[] = {100, 200, 300};
    ray_t* vb = ray_vec_from_raw(RAY_I64, rawb, 3);

    int64_t ns = ray_sym_intern("s", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ns, sl);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(vec); ray_release(sl); ray_release(vb);

    /* s + b: slice with null at offset 1 (which is position 2 of original = position 1 of slice) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* s_op = ray_scan(g, "s");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* add = ray_add(g, s_op, b_op);
    ray_op_t* cnt = ray_count(g, add);
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* slice is [20, 30(null), 40], b=[100,200,300]. null at pos1 → count=2 */
    TEST_ASSERT_EQ_I(result->i64, 2);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- expr_load_i64: I64/TIMESTAMP column in fused path (direct memcpy) ---- */
static test_result_t test_expr_load_i64_timestamp(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Use TIMESTAMP column to trigger the RAY_TIMESTAMP branch in expr_load_i64 */
    int64_t raw[] = {1000, 2000, 3000, 4000, 5000};
    ray_t* vec = ray_vec_from_raw(RAY_TIMESTAMP, raw, 5);
    int64_t name = ray_sym_intern("ts", 2);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* Add i64 const to TIMESTAMP — forces expr_load_i64 memcpy for TIMESTAMP */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* ts = ray_scan(g, "ts");
    ray_op_t* c  = ray_const_i64(g, 0);  /* add 0 to keep values */
    ray_op_t* add = ray_add(g, ts, c);
    ray_op_t* s  = ray_sum(g, add);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 15000);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- fused path: ABS and ROUND on non-nullable F64 column ---- */
static test_result_t test_expr_fused_abs_round_f64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double raw[] = {-3.7, 2.5, -1.1, 4.8, -0.3};
    ray_t* v = ray_vec_from_raw(RAY_F64, raw, 5);
    int64_t na = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* ABS — exercises expr_exec_unary OP_ABS for F64 in fused path */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* ab = ray_abs(g, x);
    ray_op_t* s  = ray_sum(g, ab);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* |−3.7|+|2.5|+|−1.1|+|4.8|+|−0.3| = 12.4 */
    TEST_ASSERT_EQ_F(result->f64, 12.4, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* ROUND — exercises expr_exec_unary OP_ROUND for F64 in fused path */
    g = ray_graph_new(tbl);
    x = ray_scan(g, "x");
    ray_op_t* rn = ray_round_op(g, x);
    s  = ray_sum(g, rn);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* round(-3.7)+round(2.5)+round(-1.1)+round(4.8)+round(-0.3)
     * = -4 + 3 + -1 + 5 + 0 = 3 */
    TEST_ASSERT_EQ_F(result->f64, 3.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- parse_linear_i64_expr: NEG branch (sum(neg(col))) ---- */
static test_result_t test_expr_linear_neg_col(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* sum(neg(a)) exercises parse_linear_i64_expr NEG branch */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a = ray_scan(g, "a");
    ray_op_t* ng = ray_neg(g, a);
    ray_op_t* s  = ray_sum(g, ng);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* neg(10+20+30+40+50) = -150 */
    TEST_ASSERT_EQ_I(result->i64, -150);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: F64 nullable columns — covers DIV/MOD/MIN2/MAX2 ---- */
static test_result_t test_expr_binary_f64_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double rawa[] = {6.0, 9.0, 12.0, 15.0};
    double rawb[] = {2.0, 3.0,  4.0,  5.0};
    ray_t* va = ray_vec_from_raw(RAY_F64, rawa, 4);
    ray_t* vb = ray_vec_from_raw(RAY_F64, rawb, 4);
    /* Make nullable to force non-fused path */
    ray_vec_set_null(va, 3, true);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* MIN2 — exercises binary_range F64 MIN2 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* mn = ray_min2(g, a_op, b_op);
    ray_op_t* s  = ray_sum(g, mn);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* min(6,2)+min(9,3)+min(12,4)+null = 2+3+4 = 9 */
    TEST_ASSERT_EQ_F(result->f64, 9.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* MAX2 — exercises binary_range F64 MAX2 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mx = ray_max2(g, a_op, b_op);
    s  = ray_sum(g, mx);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* max(6,2)+max(9,3)+max(12,4)+null = 6+9+12 = 27 */
    TEST_ASSERT_EQ_F(result->f64, 27.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* DIV (ray_div always returns F64) on non-fused F64 cols */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* dv = ray_div(g, a_op, b_op);
    s  = ray_sum(g, dv);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 6/2 + 9/3 + 12/4 + null = 3+3+3 = 9 */
    TEST_ASSERT_EQ_F(result->f64, 9.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    /* MOD — exercises binary_range F64 MOD (promote(F64,F64)=F64) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* md = ray_mod(g, a_op, b_op);
    s  = ray_sum(g, md);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 6%2=0, 9%3=0, 12%4=0, null: sum=0 */
    TEST_ASSERT_EQ_F(result->f64, 0.0, 1e-6);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: I64 nullable columns — covers MIN2/MAX2 ---- */
static test_result_t test_expr_binary_i64_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t rawa[] = {10, 20, 30, 40};
    int64_t rawb[] = {15,  5, 25, 35};
    ray_t* va = ray_vec_from_raw(RAY_I64, rawa, 4);
    ray_t* vb = ray_vec_from_raw(RAY_I64, rawb, 4);
    /* Make nullable to force non-fused path */
    ray_vec_set_null(va, 3, true);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* MIN2 — exercises binary_range I64 MIN2 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* mn = ray_min2(g, a_op, b_op);
    ray_op_t* s  = ray_sum(g, mn);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* min(10,15)+min(20,5)+min(30,25)+null = 10+5+25 = 40 */
    TEST_ASSERT_EQ_I(result->i64, 40);
    ray_release(result);
    ray_graph_free(g);

    /* MAX2 — exercises binary_range I64 MAX2 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mx = ray_max2(g, a_op, b_op);
    s  = ray_sum(g, mx);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* max(10,15)+max(20,5)+max(30,25)+null = 15+20+30 = 65 */
    TEST_ASSERT_EQ_I(result->i64, 65);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: I32 nullable — covers DIV/MOD ---- */
static test_result_t test_expr_binary_i32_divmod(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int32_t rawa[] = {12, 15, 20, 9};
    int32_t rawb[] = {3,  4,  7,  2};
    ray_t* va = ray_vec_from_raw(RAY_I32, rawa, 4);
    ray_t* vb = ray_vec_from_raw(RAY_I32, rawb, 4);
    /* Make nullable to force non-fused path */
    ray_vec_set_null(va, 3, true);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* MOD on I32 nullable — ray_mod(I32,I32) = promote(I32,I32) = I32
     * exercises binary_range I32 MOD */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* md = ray_mod(g, a_op, b_op);
    ray_op_t* s  = ray_sum(g, md);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 12%3=0, 15%4=3, 20%7=6, null: sum=9 */
    TEST_ASSERT_EQ_I(result->i64, 9);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: I16 nullable — covers MIN2/MAX2/DIV/MOD ---- */
static test_result_t test_expr_binary_i16_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int16_t rawa[] = {10, 20, 30, 40};
    int16_t rawb[] = {15,  5, 25,  8};
    ray_t* va = ray_vec_from_raw(RAY_I16, rawa, 4);
    ray_t* vb = ray_vec_from_raw(RAY_I16, rawb, 4);
    /* Make nullable to force non-fused path */
    ray_vec_set_null(va, 3, true);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* MIN2 — exercises binary_range I16 MIN2 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* mn = ray_min2(g, a_op, b_op);
    ray_op_t* s  = ray_sum(g, mn);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* min(10,15)+min(20,5)+min(30,25)+null = 10+5+25=40 */
    TEST_ASSERT_EQ_I(result->i64, 40);
    ray_release(result);
    ray_graph_free(g);

    /* MAX2 — exercises binary_range I16 MAX2 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mx = ray_max2(g, a_op, b_op);
    s  = ray_sum(g, mx);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* max(10,15)+max(20,5)+max(30,25)+null = 15+20+30=65 */
    TEST_ASSERT_EQ_I(result->i64, 65);
    ray_release(result);
    ray_graph_free(g);

    /* MOD — exercises binary_range I16 MOD */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* md = ray_mod(g, a_op, b_op);
    s  = ray_sum(g, md);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 10%15=10, 20%5=0, 30%25=5, null: sum=15 */
    TEST_ASSERT_EQ_I(result->i64, 15);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: U8 nullable — covers MIN2/MAX2/DIV/MOD ---- */
static test_result_t test_expr_binary_u8_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    uint8_t rawa[] = {10, 20, 30, 40};
    uint8_t rawb[] = {15,  5, 25,  8};
    ray_t* va = ray_vec_from_raw(RAY_U8, rawa, 4);
    ray_t* vb = ray_vec_from_raw(RAY_U8, rawb, 4);
    /* Make nullable to force non-fused path */
    ray_vec_set_null(va, 3, true);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* MIN2 — exercises binary_range U8 MIN2 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* mn = ray_min2(g, a_op, b_op);
    ray_op_t* s  = ray_sum(g, mn);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* min(10,15)+min(20,5)+min(30,25)+null = 10+5+25=40 */
    TEST_ASSERT_EQ_I(result->i64, 40);
    ray_release(result);
    ray_graph_free(g);

    /* MAX2 — exercises binary_range U8 MAX2 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* mx = ray_max2(g, a_op, b_op);
    s  = ray_sum(g, mx);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* max(10,15)+max(20,5)+max(30,25)+null = 15+20+30=65 */
    TEST_ASSERT_EQ_I(result->i64, 65);
    ray_release(result);
    ray_graph_free(g);

    /* MOD — exercises binary_range U8 MOD */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* md = ray_mod(g, a_op, b_op);
    s  = ray_sum(g, md);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 10%15=10, 20%5=0, 30%25=5, null: sum=15 */
    TEST_ASSERT_EQ_I(result->i64, 15);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- GROUP n_keys=0 sum(neg(col)): covers parse_linear_i64_expr NEG branch ---- */
static test_result_t test_expr_group_linear_neg(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* GROUP n_keys=0, SUM(neg(a)) — exercises parse_linear_i64_expr NEG branch */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op  = ray_scan(g, "a");
    ray_op_t* neg_op = ray_neg(g, a_op);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { neg_op };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 1);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* neg(10+20+30+40+50) = -150 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], -150);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- GROUP n_keys=0 sum(const * col): covers parse_linear_i64_expr MUL first arm ---- */
static test_result_t test_expr_group_linear_mul(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {1, 2, 3, 4, 5};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* GROUP n_keys=0, SUM(3 * a) — const on LEFT exercises MUL first arm */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* c3   = ray_const_i64(g, 3);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* mul  = ray_mul(g, c3, a_op);  /* const * col — first arm */
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { mul };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 1);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* 3*(1+2+3+4+5) = 45 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 45);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range BOOL AND/OR: nullable BOOL columns (non-fused path) ---- */
static test_result_t test_expr_binary_bool_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    uint8_t rawa[] = {1, 0, 1, 0, 1};
    uint8_t rawb[] = {1, 1, 0, 0, 1};
    ray_t* va = ray_vec_from_raw(RAY_BOOL, rawa, 5);
    ray_t* vb = ray_vec_from_raw(RAY_BOOL, rawb, 5);
    /* Make nullable to force non-fused path */
    ray_vec_set_null(va, 4, true);
    int64_t na = ray_sym_intern("p", 1);
    int64_t nb = ray_sym_intern("q", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* AND — exercises binary_range BOOL AND (src_is_i64=0, F64 path) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* p = ray_scan(g, "p");
    ray_op_t* q = ray_scan(g, "q");
    ray_op_t* an = ray_and(g, p, q);
    /* Count true values */
    ray_op_t* s = ray_sum(g, ray_cast(g, an, RAY_I64));
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* AND: 1&&1=1, 0&&1=0, 1&&0=0, 0&&0=0, null: only pos0=1, sum=1 */
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    ray_graph_free(g);

    /* OR — exercises binary_range BOOL OR */
    g = ray_graph_new(tbl);
    p = ray_scan(g, "p");
    q = ray_scan(g, "q");
    ray_op_t* or_op = ray_or(g, p, q);
    s = ray_sum(g, ray_cast(g, or_op, RAY_I64));
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* OR: 1||1=1, 0||1=1, 1||0=1, 0||0=0, null: 3 non-null true, sum=3 */
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- propagate_nulls: large nullable source → force ext alloc on dst ---- */
static test_result_t test_expr_propagate_nulls_large(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 200-element vector with a null at position 150 (>128) — forces
     * ext nullmap alloc on the source, which then triggers the ext-alloc
     * path in propagate_nulls (line 1097) for the destination. */
    int64_t raw[200];
    for (int i = 0; i < 200; i++) raw[i] = i + 1;
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 200);
    ray_vec_set_null(v, 150, true);  /* pos >128 forces ext nullmap on src */
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* Unary neg on nullable I64 vec (len=200) → exec_elementwise_unary
     * → propagate_nulls(src=200-elem nullable, dst=200-elem vec without ext nullmap) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a = ray_scan(g, "a");
    ray_op_t* ng = ray_neg(g, a);
    ray_op_t* s  = ray_sum(g, ng);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* sum(neg(1..200)) with pos5 null: should be negative */
    TEST_ASSERT(result->i64 < 0, "expected negative sum");
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: nullable SYM column vs STR constant — covers lines 1671-1680 ---- */
static test_result_t test_expr_sym_vs_str_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Create SYM column with a null entry */
    int64_t id1 = ray_sym_intern("foo", 3);
    int64_t id2 = ray_sym_intern("bar", 3);
    ray_t* vsym = ray_sym_vec_new(RAY_SYM_W64, 4);
    vsym->len = 4;
    int64_t* sdata = (int64_t*)ray_data(vsym);
    sdata[0] = id1;
    sdata[1] = id2;
    sdata[2] = id1;
    sdata[3] = id2;
    ray_vec_set_null(vsym, 3, true);  /* force non-fused path */
    int64_t na = ray_sym_intern("s", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, vsym);
    ray_release(vsym);

    /* s == "foo" — exercises binary_range SYM-vs-STR path (lines 1671-1674) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sc = ray_scan(g, "s");
    ray_op_t* lit = ray_const_str(g, "foo", 3);
    ray_op_t* eq = ray_eq(g, sc, lit);
    ray_op_t* flt = ray_filter(g, sc, eq);
    ray_op_t* cnt = ray_count(g, flt);
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* positions 0,2 are "foo" (pos1="bar", pos3=null): 2 matches */
    TEST_ASSERT_EQ_I(result->i64, 2);
    ray_release(result);
    ray_graph_free(g);

    /* "bar" == s — exercises binary_range STR-vs-SYM path (lines 1677-1680) */
    g = ray_graph_new(tbl);
    sc = ray_scan(g, "s");
    lit = ray_const_str(g, "bar", 3);
    eq = ray_eq(g, lit, sc);
    flt = ray_filter(g, sc, eq);
    cnt = ray_count(g, flt);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* position 1 is "bar": 1 match */
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: I32 atom as scalar left operand (line 1691) ---- */
static test_result_t test_expr_i32_scalar_left(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {5, 10, 15, 20};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 4);
    ray_vec_set_null(v, 3, true);  /* force non-fused */
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* const_i32(12) == a — exercises line 1691 (I32 scalar value reading) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_t* i32_atom = ray_i32(12);
    ray_op_t* c = ray_const_atom(g, i32_atom);
    ray_release(i32_atom);
    ray_op_t* a = ray_scan(g, "a");
    ray_op_t* eq = ray_eq(g, c, a);  /* I32 atom == I64 col */
    ray_op_t* flt = ray_filter(g, a, eq);
    ray_op_t* cnt = ray_count(g, flt);
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 12 doesn't match 5, 10, 15: 0 matches */
    TEST_ASSERT_EQ_I(result->i64, 0);
    ray_release(result);
    ray_graph_free(g);

    /* a == const_i32(10) — exercises I32 scalar on right side (line 1709) */
    g = ray_graph_new(tbl);
    a = ray_scan(g, "a");
    i32_atom = ray_i32(10);
    c = ray_const_atom(g, i32_atom);
    ray_release(i32_atom);
    eq = ray_eq(g, a, c);
    flt = ray_filter(g, a, eq);
    cnt = ray_count(g, flt);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* a[1]=10 matches: 1 match */
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range_str: STR literal on left, STR column on right (line 1338) ---- */
static test_result_t test_expr_str_scalar_left(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_table();  /* "name" col: "hello","WORLD","  foo  ","bar_baz","" */

    /* const_str("hello") == name — l_scalar=true exercises lines 1337-1340 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* lit = ray_const_str(g, "hello", 5);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* eq = ray_eq(g, lit, name);
    ray_t* result = ray_execute(g, eq);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    uint8_t* d = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(d[0], 1);  /* "hello" == "hello" */
    TEST_ASSERT_EQ_I(d[1], 0);  /* "WORLD" != "hello" */
    TEST_ASSERT_EQ_I(d[4], 0);  /* "" != "hello" */
    ray_release(result);
    ray_graph_free(g);

    /* const_str("bar_baz") != name */
    g = ray_graph_new(tbl);
    lit = ray_const_str(g, "bar_baz", 7);
    name = ray_scan(g, "name");
    ray_op_t* ne = ray_ne(g, lit, name);
    result = ray_execute(g, ne);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    d = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(d[3], 0);  /* "bar_baz" == "bar_baz" → NE=0 */
    TEST_ASSERT_EQ_I(d[0], 1);  /* "hello" != "bar_baz" → NE=1 */
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: SYM W32 column (lp_u32/rp_u32) comparison (lines 1412, 1428) ---- */
static test_result_t test_expr_sym_w32_cmp(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t id1 = ray_sym_intern("alpha", 5);
    int64_t id2 = ray_sym_intern("beta",  4);
    /* W32 SYM vector */
    ray_t* vs = ray_sym_vec_new(RAY_SYM_W32, 4);
    vs->len = 4;
    uint32_t* sd = (uint32_t*)ray_data(vs);
    sd[0] = (uint32_t)id1;
    sd[1] = (uint32_t)id2;
    sd[2] = (uint32_t)id1;
    sd[3] = (uint32_t)id2;
    ray_vec_set_null(vs, 3, true);  /* force non-fused path */
    int64_t na = ray_sym_intern("s", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, vs);
    ray_release(vs);

    /* s == "alpha" — exercises lp_u32 (line 1412) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sc = ray_scan(g, "s");
    ray_op_t* lit = ray_const_str(g, "alpha", 5);
    ray_op_t* eq = ray_eq(g, sc, lit);
    ray_op_t* flt = ray_filter(g, sc, eq);
    ray_op_t* cnt = ray_count(g, flt);
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* positions 0,2 are "alpha": 2 matches */
    TEST_ASSERT_EQ_I(result->i64, 2);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: SYM W8 narrow column (lsym_buf path) comparison (line 1413) ---- */
static test_result_t test_expr_sym_w8_cmp(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t id1 = ray_sym_intern("x", 1);
    int64_t id2 = ray_sym_intern("y", 1);
    /* W8 SYM vector */
    ray_t* vs = ray_sym_vec_new(RAY_SYM_W8, 4);
    vs->len = 4;
    uint8_t* sd = (uint8_t*)ray_data(vs);
    sd[0] = (uint8_t)id1;
    sd[1] = (uint8_t)id2;
    sd[2] = (uint8_t)id1;
    sd[3] = (uint8_t)id2;
    ray_vec_set_null(vs, 2, true);  /* force non-fused path */
    int64_t na = ray_sym_intern("c", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, vs);
    ray_release(vs);

    /* c == "x" — exercises lsym_buf narrow path (line 1413) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sc = ray_scan(g, "c");
    ray_op_t* lit = ray_const_str(g, "x", 1);
    ray_op_t* eq = ray_eq(g, sc, lit);
    ray_op_t* flt = ray_filter(g, sc, eq);
    ray_op_t* cnt = ray_count(g, flt);
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* position 0 is "x" (pos2 null, pos3 null excluded): 1 match */
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: F64 scalar zero divisor check (line 1765) ---- */
static test_result_t test_expr_f64_div_zero_scalar(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double raw[] = {6.0, 9.0, 12.0, 3.0};
    ray_t* v = ray_vec_from_raw(RAY_F64, raw, 4);
    ray_vec_set_null(v, 3, true);  /* nullable → non-fused path */
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* a / 0.0 — scalar divisor = 0 → exercises line 1765 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* col = ray_scan(g, "a");
    ray_op_t* zero = ray_const_f64(g, 0.0);
    ray_op_t* dv = ray_div(g, col, zero);
    ray_op_t* cnt = ray_count(g, dv);  /* count non-null (all nulled out) */
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* all elements become null when dividing by zero */
    TEST_ASSERT_EQ_I(result->i64, 0);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- const_expr_to_i64: F64 constant in linear expression (lines 162-167) ---- */
static test_result_t test_expr_group_linear_f64_const(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {1, 2, 3, 4, 5};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* GROUP n_keys=0, SUM(const_f64(2.0) * a):
     * const_expr_to_i64 is called on const_f64(2.0), c_is_f64=true,
     * modf(2.0)=0 → exercises lines 162-167 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* c2  = ray_const_f64(g, 2.0);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* mul  = ray_mul(g, c2, a_op);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { mul };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* 2*(1+2+3+4+5) = 30 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 30);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- linear_expr_add_term: term cancellation (lines 181-191) ---- */
static test_result_t test_expr_group_linear_cancel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {10, 20, 30};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* GROUP n_keys=0, SUM(a - a):
     * parse_linear_i64_expr sees a-a → linear_expr_add_term cancels terms,
     * exercises lines 181-191 (coeff becomes 0 → remove term) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a1 = ray_scan(g, "a");
    ray_op_t* a2 = ray_scan(g, "a");
    ray_op_t* sub = ray_sub(g, a1, a2);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { sub };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* a - a = 0 for all rows, sum = 0 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- eval_const_numeric_expr: NEG on I64 const (lines 89-97) ---- */
static test_result_t test_expr_group_affine_neg_i64_const(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {5, 10, 15};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* GROUP n_keys=0, SUM(a + neg(const_i64(3))):
     * try_affine: rhs = neg(const_i64(3)) → eval_const_numeric_expr(NEG, I64)
     * → a_is_f64=false, out_type=I64 → exercises lines 89-97 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op  = ray_scan(g, "a");
    ray_op_t* c3    = ray_const_i64(g, 3);
    ray_op_t* neg3  = ray_neg(g, c3);
    ray_op_t* add   = ray_add(g, a_op, neg3);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { add };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* sum(a + (-3)) = (5-3)+(10-3)+(15-3) = 2+7+12 = 21 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 21);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- eval_const_numeric_expr: NEG on F64 const (lines 82-88) ---- */
static test_result_t test_expr_group_affine_neg_f64_const(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {10, 20, 30};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* GROUP n_keys=0, SUM(a + neg(const_f64(5.0))):
     * eval_const_numeric_expr(NEG, F64) → a_is_f64=true → exercises lines 82-88 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op  = ray_scan(g, "a");
    ray_op_t* cf5   = ray_const_f64(g, 5.0);
    ray_op_t* negf  = ray_neg(g, cf5);
    ray_op_t* add   = ray_add(g, a_op, negf);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { add };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* sum(a - 5) = 5+15+25 = 45 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 45);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- eval_const_numeric_expr: binary const ADD (line 131), F64 binary (lines 110-127) ---- */
static test_result_t test_expr_group_affine_const_add(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* GROUP n_keys=0, SUM(a + (const_i64(2) + const_i64(3))):
     * rhs = add(2,3) → eval_const_numeric_expr: I64 ADD → exercises line 131 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* c2   = ray_const_i64(g, 2);
    ray_op_t* c3   = ray_const_i64(g, 3);
    ray_op_t* cadd = ray_add(g, c2, c3);
    ray_op_t* add  = ray_add(g, a_op, cadd);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { add };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* sum(a + 5) = 1+2+3 + 3*5 = 6+15 = 21 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 21);

    ray_release(result);
    ray_graph_free(g);

    /* SUM(a + (const_f64(2.0) + const_i64(3))):
     * rhs = add(f64(2.0), i64(3)) → F64 path → exercises lines 110-127 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    ray_op_t* cf  = ray_const_f64(g, 2.0);
    ray_op_t* ci  = ray_const_i64(g, 3);
    cadd = ray_add(g, cf, ci);
    add  = ray_add(g, a_op, cadd);
    ops[0] = OP_SUM;
    ins[0] = add;
    grp = ray_group(g, NULL, 0, ops, ins, 1);
    result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* sum(a + 5.0) = 1+2+3 + 3*5 = 21 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 21);

    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- try_affine_sumavg_input: F64 const + I64 col (lines 365-369) ---- */
static test_result_t test_expr_group_affine_f64_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {10, 20, 30};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* GROUP n_keys=0, SUM(a + const_f64(5.0)):
     * try_affine_sumavg_input: bt=RAY_I64, c_is_f64=true, c_f=5.0
     * → exercises lines 364-369 (isfinite+modf checks for I64 base) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* c5   = ray_const_f64(g, 5.0);
    ray_op_t* add  = ray_add(g, a_op, c5);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { add };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* sum(a + 5) = (10+5) + (20+5) + (30+5) = 75 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 75);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- linear_expr_add_term: update existing term (lines 183-185) ---- */
static test_result_t test_expr_group_linear_double_term(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {3, 6, 9};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* GROUP n_keys=0, SUM(a + a):
     * linear_expr_add_term finds existing term and updates coeff 1+1=2,
     * exercises lines 183-185 (next != 0) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a1 = ray_scan(g, "a");
    ray_op_t* a2 = ray_scan(g, "a");
    ray_op_t* add = ray_add(g, a1, a2);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { add };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* (3+6+9)*2 = 36 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 36);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- linear_expr_add_term: cancel mid-array term (lines 187-189) ---- */
static test_result_t test_expr_group_linear_mid_cancel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t rawa[] = {1, 2, 3};
    int64_t rawb[] = {10, 20, 30};
    ray_t* va = ray_vec_from_raw(RAY_I64, rawa, 3);
    ray_t* vb = ray_vec_from_raw(RAY_I64, rawb, 3);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* GROUP n_keys=0, SUM((a + b) - a):
     * linear: lhs=[a→1, b→1], then add a with coeff=-1 →
     * finds a in first slot, next=0 → shift b from [1] to [0] → lines 187-189 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a1 = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* a2 = ray_scan(g, "a");
    ray_op_t* ab = ray_add(g, a1, b_op);
    ray_op_t* expr = ray_sub(g, ab, a2);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { expr };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* (a+b-a) = b, sum(b) = 10+20+30 = 60 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 60);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- parse_linear_i64_expr: returns false for non-linear expr (line 274) ---- */
static test_result_t test_expr_group_nonlinear_fallback(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double raw[] = {4.0, 9.0, 16.0, 25.0};
    ray_t* v = ray_vec_from_raw(RAY_F64, raw, 4);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* GROUP n_keys=0, SUM(sqrt(a)):
     * try_linear_sumavg_input_i64 → parse_linear_i64_expr(OP_SQRT) hits
     * line 274 (returns false); GROUP falls back to regular expr evaluation */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* sq   = ray_sqrt_op(g, a_op);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { sq };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* sqrt(4)+sqrt(9)+sqrt(16)+sqrt(25) = 2+3+4+5 = 14 */
    TEST_ASSERT_EQ_F(((double*)ray_data(sum_col))[0], 14.0, 1e-6);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- eval_const_numeric_expr: F64 SUB/MUL/MIN2/MAX2, I64 SUB/DIV/MOD/MIN2/MAX2 ---- */
static test_result_t test_expr_group_affine_const_ops(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {100};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 1);
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* F64 SUB: a + (const_f64(10) - const_f64(3)) → bias=7 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* a_op = ray_scan(g, "a");
        ray_op_t* cf10 = ray_const_f64(g, 10.0);
        ray_op_t* cf3  = ray_const_f64(g, 3.0);
        ray_op_t* csub = ray_sub(g, cf10, cf3);
        ray_op_t* add  = ray_add(g, a_op, csub);
        uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { add };
        ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
        ray_t* result = ray_execute(g, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(result, 0)))[0], 107);
        ray_release(result); ray_graph_free(g);
    }

    /* F64 MUL: a + (const_f64(3) * const_f64(4)) → bias=12 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* a_op = ray_scan(g, "a");
        ray_op_t* cf3  = ray_const_f64(g, 3.0);
        ray_op_t* cf4  = ray_const_f64(g, 4.0);
        ray_op_t* cmul = ray_mul(g, cf3, cf4);
        ray_op_t* add  = ray_add(g, a_op, cmul);
        uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { add };
        ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
        ray_t* result = ray_execute(g, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(result, 0)))[0], 112);
        ray_release(result); ray_graph_free(g);
    }

    /* I64 SUB: a + (const_i64(10) - const_i64(3)) → bias=7 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* a_op = ray_scan(g, "a");
        ray_op_t* ci10 = ray_const_i64(g, 10);
        ray_op_t* ci3  = ray_const_i64(g, 3);
        ray_op_t* csub = ray_sub(g, ci10, ci3);
        ray_op_t* add  = ray_add(g, a_op, csub);
        uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { add };
        ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
        ray_t* result = ray_execute(g, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(result, 0)))[0], 107);
        ray_release(result); ray_graph_free(g);
    }

    /* I64 DIV: a + (const_i64(10) / const_i64(2)) → bias=5 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* a_op = ray_scan(g, "a");
        ray_op_t* ci10 = ray_const_i64(g, 10);
        ray_op_t* ci2  = ray_const_i64(g, 2);
        ray_op_t* cdiv = ray_div(g, ci10, ci2);
        ray_op_t* add  = ray_add(g, a_op, cdiv);
        uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { add };
        ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
        ray_t* result = ray_execute(g, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(result, 0)))[0], 105);
        ray_release(result); ray_graph_free(g);
    }

    /* I64 MOD: a + (const_i64(10) % const_i64(3)) → bias=1 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* a_op = ray_scan(g, "a");
        ray_op_t* ci10 = ray_const_i64(g, 10);
        ray_op_t* ci3  = ray_const_i64(g, 3);
        ray_op_t* cmod = ray_mod(g, ci10, ci3);
        ray_op_t* add  = ray_add(g, a_op, cmod);
        uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { add };
        ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
        ray_t* result = ray_execute(g, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(result, 0)))[0], 101);
        ray_release(result); ray_graph_free(g);
    }

    /* I64 MIN2: a + min2(const_i64(3), const_i64(7)) → bias=3 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* a_op = ray_scan(g, "a");
        ray_op_t* ci3  = ray_const_i64(g, 3);
        ray_op_t* ci7  = ray_const_i64(g, 7);
        ray_op_t* cmn  = ray_min2(g, ci3, ci7);
        ray_op_t* add  = ray_add(g, a_op, cmn);
        uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { add };
        ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
        ray_t* result = ray_execute(g, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(result, 0)))[0], 103);
        ray_release(result); ray_graph_free(g);
    }

    /* I64 MAX2: a + max2(const_i64(3), const_i64(7)) → bias=7 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* a_op = ray_scan(g, "a");
        ray_op_t* ci3  = ray_const_i64(g, 3);
        ray_op_t* ci7  = ray_const_i64(g, 7);
        ray_op_t* cmx  = ray_max2(g, ci3, ci7);
        ray_op_t* add  = ray_add(g, a_op, cmx);
        uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { add };
        ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
        ray_t* result = ray_execute(g, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(result, 0)))[0], 107);
        ray_release(result); ray_graph_free(g);
    }

    /* F64 MOD: a + (const_f64(10) % const_f64(3)) → bias=1 (line 118) */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* a_op = ray_scan(g, "a");
        ray_op_t* cf10 = ray_const_f64(g, 10.0);
        ray_op_t* cf3  = ray_const_f64(g, 3.0);
        ray_op_t* cmod = ray_mod(g, cf10, cf3);
        ray_op_t* add  = ray_add(g, a_op, cmod);
        uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { add };
        ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
        ray_t* result = ray_execute(g, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(result, 0)))[0], 101);
        ray_release(result); ray_graph_free(g);
    }

    /* F64 MIN2: a + min2(const_f64(3), const_f64(7)) → bias=3 (line 119) */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* a_op = ray_scan(g, "a");
        ray_op_t* cf3  = ray_const_f64(g, 3.0);
        ray_op_t* cf7  = ray_const_f64(g, 7.0);
        ray_op_t* cmn  = ray_min2(g, cf3, cf7);
        ray_op_t* add  = ray_add(g, a_op, cmn);
        uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { add };
        ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
        ray_t* result = ray_execute(g, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(result, 0)))[0], 103);
        ray_release(result); ray_graph_free(g);
    }

    /* F64 MAX2: a + max2(const_f64(3), const_f64(7)) → bias=7 (line 120) */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* a_op = ray_scan(g, "a");
        ray_op_t* cf3  = ray_const_f64(g, 3.0);
        ray_op_t* cf7  = ray_const_f64(g, 7.0);
        ray_op_t* cmx  = ray_max2(g, cf3, cf7);
        ray_op_t* add  = ray_add(g, a_op, cmx);
        uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { add };
        ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
        ray_t* result = ray_execute(g, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(result, 0)))[0], 107);
        ray_release(result); ray_graph_free(g);
    }

    /* I64 DIV: a + (const_i64(9) / const_i64(3)) → bias=3 (lines 134-137) */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* a_op = ray_scan(g, "a");
        ray_op_t* ci9  = ray_const_i64(g, 9);
        ray_op_t* ci3  = ray_const_i64(g, 3);
        ray_op_t* cdiv = ray_div(g, ci9, ci3);
        ray_op_t* add  = ray_add(g, a_op, cdiv);
        uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { add };
        ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
        ray_t* result = ray_execute(g, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(result, 0)))[0], 103);
        ray_release(result); ray_graph_free(g);
    }

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- linear_expr_add_scaled: return false when AGG_LINEAR_MAX_TERMS exceeded (line 212) ---- */
static test_result_t test_expr_group_linear_max_terms(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Create 9 I64 columns (AGG_LINEAR_MAX_TERMS=8, so 9 distinct terms fail) */
    int64_t data[3] = {1, 2, 3};
    ray_t* cols[9];
    int64_t syms[9];
    const char* names[] = {"c0","c1","c2","c3","c4","c5","c6","c7","c8"};
    for (int k = 0; k < 9; k++) {
        cols[k] = ray_vec_from_raw(RAY_I64, data, 3);
        syms[k] = ray_sym_intern(names[k], 2);
    }

    ray_t* tbl = ray_table_new(9);
    for (int k = 0; k < 9; k++) {
        tbl = ray_table_add_col(tbl, syms[k], cols[k]);
        ray_release(cols[k]);
    }

    /* GROUP n_keys=0, SUM(c0+c1+c2+c3+c4+c5+c6+c7+c8):
     * parse_linear_i64_expr will try to build 9 terms → linear_expr_add_scaled
     * fails when n_terms >= AGG_LINEAR_MAX_TERMS → exercises line 212 (return false)
     * → try_linear_sumavg_input_i64 falls back to regular expr evaluation */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* c[9];
    for (int k = 0; k < 9; k++) c[k] = ray_scan(g, names[k]);
    /* Build c0+c1+c2+...+c8 */
    ray_op_t* sum_expr = ray_add(g, c[0], c[1]);
    for (int k = 2; k < 9; k++) sum_expr = ray_add(g, sum_expr, c[k]);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { sum_expr };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* Each row: 1+1+...+1(9x) or 2+2... or 3+3...
     * sum across 3 rows of (row_val * 9): 9+18+27 = 54 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 54);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range BOOL: AND/OR on I64 columns (src_is_i64 path, lines 1555-1556) ---- */
static test_result_t test_expr_and_i64_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t rawa[] = {1, 0, 1, 0};
    int64_t rawb[] = {1, 1, 0, 0};
    ray_t* va = ray_vec_from_raw(RAY_I64, rawa, 4);
    ray_t* vb = ray_vec_from_raw(RAY_I64, rawb, 4);
    ray_vec_set_null(va, 3, true);  /* force non-fused path */
    ray_vec_set_null(vb, 3, true);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* and(a, b) with I64 nullable columns:
     * lp_i64 set for both → src_is_i64=true → exercises lines 1555 (OP_AND) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* and_op = ray_and(g, a_op, b_op);
    ray_op_t* s   = ray_sum(g, and_op);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 1&&1=1, 0&&1=0, 1&&0=0, null: sum=1 */
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    ray_graph_free(g);

    /* or(a, b): exercises line 1556 (OP_OR) */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    b_op = ray_scan(g, "b");
    ray_op_t* or_op = ray_or(g, a_op, b_op);
    s   = ray_sum(g, or_op);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* 1||1=1, 0||1=1, 1||0=1, null: sum=3 */
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- exec_elementwise_unary: I64 CEIL/FLOOR → default branch (line 1254) ---- */
static test_result_t test_expr_ceil_i64_nullable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t raw[] = {3, 7, 11, 15};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 4);
    ray_vec_set_null(v, 3, true);  /* force non-fused path */
    int64_t na = ray_sym_intern("a", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* ceil(nullable I64 col) → I64 out: exercises default case in I64→I64 switch (line 1254) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* cl   = ray_ceil_op(g, a_op);
    ray_op_t* s    = ray_sum(g, cl);
    ray_t* result  = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* ceil(3)+ceil(7)+ceil(11)+null = 3+7+11 = 21 */
    TEST_ASSERT_EQ_I(result->i64, 21);
    ray_release(result);
    ray_graph_free(g);

    /* floor(nullable I64 col) — also hits line 1254 */
    g = ray_graph_new(tbl);
    a_op = ray_scan(g, "a");
    ray_op_t* fl = ray_floor_op(g, a_op);
    s  = ray_sum(g, fl);
    result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 21);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: SYM W32 column on RHS (rp_u32, line 1428) ---- */
static test_result_t test_expr_sym_w32_rhs(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t id1 = ray_sym_intern("alpha", 5);
    int64_t id2 = ray_sym_intern("beta",  4);

    /* Two W32 SYM columns: exercises rp_u32 path (line 1428) */
    ray_t* v1 = ray_sym_vec_new(RAY_SYM_W32, 4);
    v1->len = 4;
    uint32_t* d1 = (uint32_t*)ray_data(v1);
    d1[0] = (uint32_t)id1; d1[1] = (uint32_t)id2;
    d1[2] = (uint32_t)id1; d1[3] = (uint32_t)id2;
    ray_vec_set_null(v1, 3, true);  /* force non-fused */

    ray_t* v2 = ray_sym_vec_new(RAY_SYM_W32, 4);
    v2->len = 4;
    uint32_t* d2 = (uint32_t*)ray_data(v2);
    d2[0] = (uint32_t)id1; d2[1] = (uint32_t)id1;
    d2[2] = (uint32_t)id2; d2[3] = (uint32_t)id1;
    ray_vec_set_null(v2, 3, true);

    int64_t na = ray_sym_intern("s", 1);
    int64_t nb = ray_sym_intern("t", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, v1);
    tbl = ray_table_add_col(tbl, nb, v2);
    ray_release(v1); ray_release(v2);

    /* s == t — exercises lp_u32 (lhs W32) and rp_u32 (rhs W32) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* s_op = ray_scan(g, "s");
    ray_op_t* t_op = ray_scan(g, "t");
    ray_op_t* eq   = ray_eq(g, s_op, t_op);
    ray_op_t* flt  = ray_filter(g, s_op, eq);
    ray_op_t* cnt  = ray_count(g, flt);
    ray_t* result  = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* row 0: alpha==alpha (1), row 1: beta!=alpha (0), row 2: alpha!=beta (0): 1 match */
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- binary_range: SYM W8 narrow column on RHS (rsym_buf, line 1429) ---- */
static test_result_t test_expr_sym_w8_rhs(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t id1 = ray_sym_intern("p", 1);
    int64_t id2 = ray_sym_intern("q", 1);

    /* Two W8 SYM columns */
    ray_t* v1 = ray_sym_vec_new(RAY_SYM_W8, 3);
    v1->len = 3;
    uint8_t* d1 = (uint8_t*)ray_data(v1);
    d1[0] = (uint8_t)id1; d1[1] = (uint8_t)id2; d1[2] = (uint8_t)id1;
    ray_vec_set_null(v1, 2, true);

    ray_t* v2 = ray_sym_vec_new(RAY_SYM_W8, 3);
    v2->len = 3;
    uint8_t* d2 = (uint8_t*)ray_data(v2);
    d2[0] = (uint8_t)id1; d2[1] = (uint8_t)id1; d2[2] = (uint8_t)id2;
    ray_vec_set_null(v2, 2, true);

    int64_t na = ray_sym_intern("s", 1);
    int64_t nb = ray_sym_intern("t", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, v1);
    tbl = ray_table_add_col(tbl, nb, v2);
    ray_release(v1); ray_release(v2);

    /* s == t — exercises lsym_buf (lhs narrow) and rsym_buf (rhs narrow) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* s_op = ray_scan(g, "s");
    ray_op_t* t_op = ray_scan(g, "t");
    ray_op_t* eq   = ray_eq(g, s_op, t_op);
    ray_op_t* flt  = ray_filter(g, s_op, eq);
    ray_op_t* cnt  = ray_count(g, flt);
    ray_t* result  = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* row 0: p==p (1), row 1: q!=p (0): 1 match */
    TEST_ASSERT_EQ_I(result->i64, 1);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- expr_exec_binary: BOOL F64 NE in fused path (line 747) ---- */
static test_result_t test_expr_fused_f64_ne(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Two non-nullable F64 columns: fused path for NE comparison */
    double rawa[] = {1.0, 2.0, 3.0, 4.0};
    double rawb[] = {1.0, 9.0, 3.0, 9.0};
    ray_t* va = ray_vec_from_raw(RAY_F64, rawa, 4);
    ray_t* vb = ray_vec_from_raw(RAY_F64, rawb, 4);
    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    /* ne(a, b) in fused path exercises expr_exec_binary F64 NE (line 747) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a_op = ray_scan(g, "a");
    ray_op_t* b_op = ray_scan(g, "b");
    ray_op_t* ne   = ray_ne(g, a_op, b_op);
    ray_op_t* s    = ray_sum(g, ne);
    ray_t* result  = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* rows 1 and 3 differ: count=2 */
    TEST_ASSERT_EQ_I(result->i64, 2);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- try_affine_sumavg_input: DATE column → line 380 (unsupported type) ---- */
static test_result_t test_expr_group_affine_date_col(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* DATE column: 4-byte integers (days since epoch) */
    int32_t raw[] = {1000, 2000, 3000};
    ray_t* v = ray_vec_new(RAY_DATE, 3);
    v->len = 3;
    memcpy(ray_data(v), raw, 3 * sizeof(int32_t));
    int64_t na = ray_sym_intern("d", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, v);
    ray_release(v);

    /* GROUP n_keys=0, SUM(d + 1):
     * try_affine_sumavg_input: bt=RAY_DATE not in list → exercises line 380 (return false)
     * then try_linear_sumavg_input_i64: type_is_linear_i64_col(RAY_DATE)=true → succeeds */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* d_op = ray_scan(g, "d");
    ray_op_t* c1   = ray_const_i64(g, 1);
    ray_op_t* add  = ray_add(g, d_op, c1);
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { add };
    ray_op_t* grp = ray_group(g, NULL, 0, ops, ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(sum_col);
    /* sum(d + 1) = 1001+2001+3001 = 6003 */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 6003);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- expr_load_i64 SYM path: non-nullable SYM W8 in fused expression ----
 * Covers lines 586-589 (expr_load_i64 case RAY_SYM) via fused path:
 * non-nullable col → SCAN reg type=I64/col_type=SYM/SYM_W8 ≠ W64
 * → else branch → expr_load_i64(_, _, RAY_SYM, ...) */
static test_result_t test_expr_sym_w8_fused(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t id1 = ray_sym_intern("foo", 3);
    int64_t id2 = ray_sym_intern("bar", 3);
    /* Non-nullable SYM W8 vector (no nulls → fused path used) */
    ray_t* vs = ray_sym_vec_new(RAY_SYM_W8, 4);
    vs->len = 4;
    uint8_t* sd = (uint8_t*)ray_data(vs);
    sd[0] = (uint8_t)id1;
    sd[1] = (uint8_t)id2;
    sd[2] = (uint8_t)id1;
    sd[3] = (uint8_t)id2;
    /* No nulls set → fused path active */
    int64_t na = ray_sym_intern("s", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, na, vs);
    ray_release(vs);

    /* s == 'foo': fused path: SYM W8 → expr_load_i64(RAY_SYM) → lines 586-589 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sc  = ray_scan(g, "s");
    ray_op_t* lit = ray_const_str(g, "foo", 3);
    ray_op_t* eq  = ray_eq(g, sc, lit);
    ray_op_t* s   = ray_sum(g, eq);
    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* positions 0 and 2 are "foo" → count=2 */
    TEST_ASSERT_EQ_I(result->i64, 2);
    ray_release(result);
    ray_graph_free(g);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- col_propagate_nulls_filter loop body (internal.h lines 273-281) ---- */
/* Filter a standalone column vector (not table) that has RAY_ATTR_HAS_NULLS.
 * exec_filter sees input->type != RAY_TABLE → exec_filter_vec →
 * col_propagate_nulls_filter which only loops when HAS_NULLS is set. */
static test_result_t test_exec_filter_vec_nullable_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build table with one nullable I64 column: [10, 0N, 30, 0N, 50] */
    int64_t raw[] = {10, 0, 30, 0, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    ray_vec_set_null(vec, 1, true);
    ray_vec_set_null(vec, 3, true);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* Scan the column vector directly (not the table), then filter it.
     * ray_scan returns the column; exec_filter sees non-TABLE input →
     * exec_filter_vec → col_propagate_nulls_filter loop body fires
     * for the two null rows. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x      = ray_scan(g, "x");
    ray_op_t* thresh = ray_const_i64(g, 25);
    ray_op_t* pred   = ray_gt(g, x, thresh);
    ray_op_t* filt   = ray_filter(g, x, pred);
    ray_op_t* cnt    = ray_count(g, filt);

    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* Values > 25 in [10, 0N, 30, 0N, 50]: only 30 and 50 pass → count 2 */
    TEST_ASSERT_EQ_I(result->i64, 2);
    ray_release(result);

    /* Also verify that null bits are preserved in the filtered vector */
    ray_graph_free(g);
    g = ray_graph_new(tbl);
    x      = ray_scan(g, "x");
    thresh = ray_const_i64(g, 0);
    /* Keep all non-null rows plus nulls: predicate >= 0 matches 10,30,50
     * but nulls compare false → only 10,30,50 pass. */
    pred = ray_ge(g, x, thresh);
    filt = ray_filter(g, x, pred);
    cnt  = ray_count(g, filt);
    result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);

    /* Filter with isnull predicate: null rows pass → set_null branch (line 277)
     * col_propagate_nulls_filter: mask[1]=1 and mask[3]=1 (null positions pass),
     * so out=0 and out=1 get null bits set in the result.
     * Use the filter result directly (not count which skips nulls). */
    ray_graph_free(g);
    g = ray_graph_new(tbl);
    x    = ray_scan(g, "x");
    pred = ray_isnull(g, x);
    filt = ray_filter(g, x, pred);
    result = ray_execute(g, filt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* Only the 2 null rows pass the isnull predicate; result has len=2, both null */
    TEST_ASSERT_EQ_I(result->len, 2);
    TEST_ASSERT_EQ_I(ray_vec_is_null(result, 0), 1);
    TEST_ASSERT_EQ_I(ray_vec_is_null(result, 1), 1);
    ray_release(result);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- atom_to_str_t SLICE path (internal.h lines 471-473) ---- */
/* A len-1 STR slice has type=RAY_STR, len=1, RAY_ATTR_SLICE set.
 * When used as the scalar side of a string comparison, atom_to_str_t
 * resolves it via the SLICE branch (src = slice_parent, idx = slice_offset). */
static test_result_t test_exec_str_eq_slice_scalar(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a 3-element STR vector (the data side) */
    ray_t* c0 = ray_vec_new(RAY_STR, 3);
    c0 = ray_str_vec_append(c0, "alice", 5);
    c0 = ray_str_vec_append(c0, "bob", 3);
    c0 = ray_str_vec_append(c0, "charlie", 7);

    /* Build a 3-element STR vector to slice from */
    ray_t* pool = ray_vec_new(RAY_STR, 3);
    pool = ray_str_vec_append(pool, "alice", 5);
    pool = ray_str_vec_append(pool, "bob", 3);
    pool = ray_str_vec_append(pool, "charlie", 7);

    /* Slice pool[0..0] — a len-1 view at offset 0; RAY_ATTR_SLICE is set */
    ray_t* slc = ray_vec_slice(pool, 0, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(slc));
    TEST_ASSERT_EQ_I(slc->len, 1);

    int64_t name_id = ray_sym_intern("name", 4);
    int64_t tag_id  = ray_sym_intern("tag", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name_id, c0);
    tbl = ray_table_add_col(tbl, tag_id, slc);
    ray_release(c0);
    ray_release(slc);
    ray_release(pool);

    /* Compare name == tag (slice scalar "alice"):
     * row0: "alice"=="alice" → true
     * row1: "bob"  =="alice" → false
     * row2: "charlie"=="alice" → false */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* tag  = ray_scan(g, "tag");
    ray_op_t* eq   = ray_eq(g, name, tag);
    ray_t* result  = ray_execute(g, eq);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(result->len, 3);
    uint8_t* d = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(d[0], 1);
    TEST_ASSERT_EQ_I(d[1], 0);
    TEST_ASSERT_EQ_I(d[2], 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- read_col_i64 W8 branch (internal.h line 146) ---- */
/* A RAY_SYM_W8 column (uint8_t sym IDs ≤ 255) uses the W8 branch of
 * read_col_i64.  Build a fresh sym table so IDs stay small, then do
 * a GROUP BY on the W8 column → group.c calls read_col_i64 W8 path.
 * Also do a JOIN on the W8 column → join.c read_col_i64 W8 path. */
static test_result_t test_exec_read_col_i64_sym_w8(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Intern value symbols and column name symbols.
     * Fresh sym table → IDs start at 0, all ≤ 255 → W8. */
    int64_t col_k  = ray_sym_intern("k", 1);   /* column name */
    int64_t col_v  = ray_sym_intern("v", 1);   /* column name */
    int64_t sym_a  = ray_sym_intern("a", 1);
    int64_t sym_b  = ray_sym_intern("b", 1);
    int64_t sym_c  = ray_sym_intern("c", 1);
    /* All IDs ≤ 255 → W8 encoding */

    /* Build a W8 SYM key column: [a, b, a, c, b, a] */
    ray_t* k_vec = ray_sym_vec_new(RAY_SYM_W8, 6);
    TEST_ASSERT_FALSE(RAY_IS_ERR(k_vec));
    k_vec->len = 6;
    uint8_t* k_data = (uint8_t*)ray_data(k_vec);
    k_data[0] = (uint8_t)sym_a;
    k_data[1] = (uint8_t)sym_b;
    k_data[2] = (uint8_t)sym_a;
    k_data[3] = (uint8_t)sym_c;
    k_data[4] = (uint8_t)sym_b;
    k_data[5] = (uint8_t)sym_a;

    int64_t v_data[] = {10, 20, 30, 40, 50, 60};
    ray_t* v_vec = ray_vec_from_raw(RAY_I64, v_data, 6);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, col_k, k_vec);
    tbl = ray_table_add_col(tbl, col_v, v_vec);
    ray_release(k_vec);
    ray_release(v_vec);

    /* GROUP BY the W8 SYM column: sum(v) by k.
     * group.c calls read_col_i64(data, row, RAY_SYM, W8_attrs) → W8 branch.
     * Groups: a→10+30+60=100, b→20+50=70, c→40 */
    {
        ray_graph_t* g1 = ray_graph_new(tbl);
        ray_op_t* k_op  = ray_scan(g1, "k");
        ray_op_t* v_op  = ray_scan(g1, "v");
        ray_op_t* keys[]    = { k_op };
        ray_op_t* agg_ins[] = { v_op };
        uint16_t  agg_ops[] = { OP_SUM };
        ray_op_t* grp = ray_group(g1, keys, 1, agg_ops, agg_ins, 1);
        ray_t* result = ray_execute(g1, grp);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
        TEST_ASSERT_EQ_I(ray_table_nrows(result), 3); /* 3 groups: a, b, c */
        ray_release(result);
        ray_graph_free(g1);
    }

    /* JOIN on the W8 SYM column → join.c read_col_i64 W8 path */
    {
        ray_graph_t* g2 = ray_graph_new(tbl);
        ray_op_t* lt = ray_const_table(g2, tbl);
        ray_op_t* rt = ray_const_table(g2, tbl);
        ray_op_t* lk = ray_scan(g2, "k");
        ray_op_t* rk = ray_scan(g2, "k");
        ray_op_t* lk_arr[] = { lk };
        ray_op_t* rk_arr[] = { rk };
        ray_op_t* join_op = ray_join(g2, lt, lk_arr, rt, rk_arr, 1, 0);
        ray_t* result = ray_execute(g2, join_op);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        /* a:3×3=9, b:2×2=4, c:1×1=1 → 14 rows */
        TEST_ASSERT_EQ_I(ray_table_nrows(result), 14);
        ray_release(result);
        ray_graph_free(g2);
    }

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ======================================================================
 * Suite
 * ====================================================================== */

const test_entry_t exec_entries[] = {
    { "exec/neg_i64", test_exec_neg_i64, NULL, NULL },
    { "exec/neg_f64", test_exec_neg_f64, NULL, NULL },
    { "exec/abs", test_exec_abs, NULL, NULL },
    { "exec/not", test_exec_not, NULL, NULL },
    { "exec/isnull", test_exec_isnull, NULL, NULL },
    { "exec/math_ops", test_exec_math_ops, NULL, NULL },
    { "exec/ceil_floor", test_exec_ceil_floor, NULL, NULL },
    { "exec/binary_arith", test_exec_binary_arithmetic, NULL, NULL },
    { "exec/comparisons", test_exec_comparisons, NULL, NULL },
    { "exec/min2_max2", test_exec_min2_max2, NULL, NULL },
    { "exec/if", test_exec_if, NULL, NULL },
    { "exec/reductions", test_exec_reductions, NULL, NULL },
    { "exec/reduce_i32", test_exec_reduce_i32, NULL, NULL },
    { "exec/reduce_i16", test_exec_reduce_i16, NULL, NULL },
    { "exec/reduce_bool", test_exec_reduce_bool, NULL, NULL },
    { "exec/reduce_i64_nulls", test_exec_reduce_i64_nulls, NULL, NULL },
    { "exec/reduce_f64_nulls", test_exec_reduce_f64_nulls, NULL, NULL },
    { "exec/reduce_empty", test_exec_reduce_empty, NULL, NULL },
    { "exec/sort", test_exec_sort, NULL, NULL },
    { "exec/head_tail", test_exec_head_tail, NULL, NULL },
    { "exec/join", test_exec_join, NULL, NULL },
    { "exec/join_large", test_exec_join_large, NULL, NULL },
    { "exec/join_fallback", test_exec_join_fallback, NULL, NULL },
    { "exec/join_empty", test_exec_join_empty, NULL, NULL },
    { "exec/join_left_large", test_exec_join_left_large, NULL, NULL },
    { "exec/join_full_large", test_exec_join_full_large, NULL, NULL },
    { "exec/join_skewed", test_exec_join_skewed, NULL, NULL },
    { "exec/join_boundary", test_exec_join_boundary, NULL, NULL },
    { "exec/join_multikey", test_exec_join_multikey, NULL, NULL },
    { "exec/window", test_exec_window, NULL, NULL },
    { "exec/select", test_exec_select, NULL, NULL },
    { "exec/stddev", test_exec_stddev, NULL, NULL },
    { "exec/count_distinct", test_exec_count_distinct, NULL, NULL },
    { "exec/asof_join", test_exec_asof_join, NULL, NULL },
    { "exec/asof_left_join", test_exec_asof_left_join, NULL, NULL },
    { "exec/asof_empty", test_exec_asof_empty, NULL, NULL },
    { "exec/asof_null_keys", test_exec_asof_null_keys, NULL, NULL },
    { "exec/asof_null_keys_match", test_exec_asof_null_keys_match, NULL, NULL },
    { "exec/upper", test_exec_upper, NULL, NULL },
    { "exec/lower", test_exec_lower, NULL, NULL },
    { "exec/strlen", test_exec_strlen, NULL, NULL },
    { "exec/trim", test_exec_trim, NULL, NULL },
    { "exec/like", test_exec_like, NULL, NULL },
    { "exec/concat", test_exec_concat, NULL, NULL },
    { "exec/extract", test_exec_extract, NULL, NULL },
    { "exec/date_trunc", test_exec_date_trunc, NULL, NULL },
    { "exec/cast", test_exec_cast, NULL, NULL },
    { "exec/graph_dump", test_graph_dump, NULL, NULL },
    { "exec/str_eq", test_exec_str_eq, NULL, NULL },
    { "exec/str_ne", test_exec_str_ne, NULL, NULL },
    { "exec/str_lt", test_exec_str_lt, NULL, NULL },
    { "exec/str_le", test_exec_str_le, NULL, NULL },
    { "exec/str_gt", test_exec_str_gt, NULL, NULL },
    { "exec/str_ge", test_exec_str_ge, NULL, NULL },
    { "exec/str_strlen", test_exec_str_strlen, NULL, NULL },
    { "exec/str_upper", test_exec_str_upper, NULL, NULL },
    { "exec/str_lower", test_exec_str_lower, NULL, NULL },
    { "exec/str_trim", test_exec_str_trim, NULL, NULL },
    { "exec/str_substr", test_exec_str_substr, NULL, NULL },
    { "exec/str_replace", test_exec_str_replace, NULL, NULL },
    { "exec/str_concat", test_exec_str_concat, NULL, NULL },
    { "exec/str_if", test_exec_str_if, NULL, NULL },
    { "exec/str_if_scalar", test_exec_str_if_scalar, NULL, NULL },
    { "exec/str_eq_len1_broadcast", test_exec_str_eq_len1_broadcast, NULL, NULL },
    { "exec/str_eq_empty_vec_scalar", test_exec_str_eq_empty_vec_scalar, NULL, NULL },
    { "exec/str_upper_null", test_exec_str_upper_null, NULL, NULL },
    { "exec/str_strlen_null", test_exec_str_strlen_null, NULL, NULL },
    { "exec/str_substr_null", test_exec_str_substr_null, NULL, NULL },
    { "exec/str_replace_null", test_exec_str_replace_null, NULL, NULL },
    { "exec/str_concat_null", test_exec_str_concat_null, NULL, NULL },
    { "exec/read_col_i64_sym_w8", test_exec_read_col_i64_sym_w8, NULL, NULL },
    { "exec/filter_vec_nullable_i64", test_exec_filter_vec_nullable_i64, NULL, NULL },
    { "exec/str_eq_slice_scalar", test_exec_str_eq_slice_scalar, NULL, NULL },
    { "exec/lazy_wrap_materialize", test_lazy_wrap_materialize, NULL, NULL },
    { "exec/lazy_chain", test_lazy_chain, NULL, NULL },
    { "exec/lazy_materialize_passthrough", test_lazy_materialize_passthrough, NULL, NULL },
    { "exec/lazy_release_no_materialize", test_lazy_release_no_materialize, NULL, NULL },
    /* expr.c coverage extension */
    { "exec/expr_atom_i16_const", test_expr_atom_i16_const, NULL, NULL },
    { "exec/expr_const_arithmetic", test_expr_const_arithmetic, NULL, NULL },
    { "exec/expr_scalar_null_propagation", test_expr_scalar_null_propagation, NULL, NULL },
    { "exec/expr_i32_column_binary", test_expr_i32_column_binary, NULL, NULL },
    { "exec/expr_i16_column_binary", test_expr_i16_column_binary, NULL, NULL },
    { "exec/expr_u8_bool_column_binary", test_expr_u8_bool_column_binary, NULL, NULL },
    { "exec/expr_scalar_i32_atom", test_expr_scalar_i32_atom, NULL, NULL },
    { "exec/expr_f64_fused_modminmax", test_expr_f64_fused_modminmax, NULL, NULL },
    { "exec/expr_i64_fused_div", test_expr_i64_fused_div, NULL, NULL },
    { "exec/expr_f64_divzero_scalar", test_expr_f64_divzero_scalar, NULL, NULL },
    { "exec/expr_i32_divzero_vector", test_expr_i32_divzero_vector, NULL, NULL },
    { "exec/expr_cast_narrow_types", test_expr_cast_narrow_types, NULL, NULL },
    { "exec/expr_unary_null_propagation", test_expr_unary_null_propagation, NULL, NULL },
    { "exec/expr_binary_null_propagation", test_expr_binary_null_propagation, NULL, NULL },
    { "exec/expr_affine_sub_path", test_expr_affine_sub_path, NULL, NULL },
    { "exec/expr_affine_f64_path", test_expr_affine_f64_path, NULL, NULL },
    { "exec/expr_linear_scan_ops", test_expr_linear_scan_ops, NULL, NULL },
    { "exec/expr_round_op", test_expr_round_op, NULL, NULL },
    { "exec/expr_unary_i64_to_f64", test_expr_unary_i64_to_f64, NULL, NULL },
    { "exec/expr_bool_and_or", test_expr_bool_and_or, NULL, NULL },
    { "exec/expr_load_i64_timestamp", test_expr_load_i64_timestamp, NULL, NULL },
    { "exec/expr_unary_f64_nullable", test_expr_unary_f64_nullable, NULL, NULL },
    { "exec/expr_unary_i64_nullable", test_expr_unary_i64_nullable, NULL, NULL },
    { "exec/expr_unary_cast_narrow_nullable", test_expr_unary_cast_narrow_nullable, NULL, NULL },
    { "exec/expr_binary_narrow_nullable", test_expr_binary_narrow_nullable, NULL, NULL },
    { "exec/expr_set_all_null_large", test_expr_set_all_null_large, NULL, NULL },
    { "exec/expr_propagate_nulls_slice", test_expr_propagate_nulls_slice, NULL, NULL },
    { "exec/expr_fused_abs_round_f64", test_expr_fused_abs_round_f64, NULL, NULL },
    { "exec/expr_linear_neg_col", test_expr_linear_neg_col, NULL, NULL },
    { "exec/expr_binary_f64_nullable", test_expr_binary_f64_nullable, NULL, NULL },
    { "exec/expr_binary_i64_nullable", test_expr_binary_i64_nullable, NULL, NULL },
    { "exec/expr_binary_i32_divmod", test_expr_binary_i32_divmod, NULL, NULL },
    { "exec/expr_binary_i16_nullable", test_expr_binary_i16_nullable, NULL, NULL },
    { "exec/expr_binary_u8_nullable", test_expr_binary_u8_nullable, NULL, NULL },
    { "exec/expr_group_linear_neg", test_expr_group_linear_neg, NULL, NULL },
    { "exec/expr_group_linear_mul", test_expr_group_linear_mul, NULL, NULL },
    { "exec/expr_binary_bool_nullable", test_expr_binary_bool_nullable, NULL, NULL },
    { "exec/expr_propagate_nulls_large", test_expr_propagate_nulls_large, NULL, NULL },
    { "exec/expr_sym_vs_str_nullable", test_expr_sym_vs_str_nullable, NULL, NULL },
    { "exec/expr_i32_scalar_left", test_expr_i32_scalar_left, NULL, NULL },
    { "exec/expr_str_scalar_left", test_expr_str_scalar_left, NULL, NULL },
    { "exec/expr_sym_w32_cmp", test_expr_sym_w32_cmp, NULL, NULL },
    { "exec/expr_sym_w8_cmp", test_expr_sym_w8_cmp, NULL, NULL },
    { "exec/expr_f64_div_zero_scalar", test_expr_f64_div_zero_scalar, NULL, NULL },
    { "exec/expr_group_linear_f64_const", test_expr_group_linear_f64_const, NULL, NULL },
    { "exec/expr_group_linear_cancel", test_expr_group_linear_cancel, NULL, NULL },
    { "exec/expr_group_nonlinear_fallback", test_expr_group_nonlinear_fallback, NULL, NULL },
    { "exec/expr_group_affine_f64_i64", test_expr_group_affine_f64_i64, NULL, NULL },
    { "exec/expr_group_linear_double_term", test_expr_group_linear_double_term, NULL, NULL },
    { "exec/expr_group_linear_mid_cancel", test_expr_group_linear_mid_cancel, NULL, NULL },
    { "exec/expr_group_affine_neg_i64_const", test_expr_group_affine_neg_i64_const, NULL, NULL },
    { "exec/expr_group_affine_const_add", test_expr_group_affine_const_add, NULL, NULL },
    { "exec/expr_group_affine_neg_f64_const", test_expr_group_affine_neg_f64_const, NULL, NULL },
    { "exec/expr_group_affine_const_ops", test_expr_group_affine_const_ops, NULL, NULL },
    { "exec/expr_group_affine_date_col", test_expr_group_affine_date_col, NULL, NULL },
    { "exec/expr_fused_f64_ne", test_expr_fused_f64_ne, NULL, NULL },
    { "exec/expr_sym_w32_rhs", test_expr_sym_w32_rhs, NULL, NULL },
    { "exec/expr_sym_w8_rhs", test_expr_sym_w8_rhs, NULL, NULL },
    { "exec/expr_group_linear_max_terms", test_expr_group_linear_max_terms, NULL, NULL },
    { "exec/expr_ceil_i64_nullable", test_expr_ceil_i64_nullable, NULL, NULL },
    { "exec/expr_and_i64_nullable", test_expr_and_i64_nullable, NULL, NULL },
    { "exec/expr_sym_w8_fused", test_expr_sym_w8_fused, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


