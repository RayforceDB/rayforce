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
#include <string.h>
#include <math.h>

/* --------------------------------------------------------------------------
 * Helper: create a test table with columns id1(I64), v1(I64), v3(F64)
 * -------------------------------------------------------------------------- */

static ray_t* make_test_table(void) {
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

/* --------------------------------------------------------------------------
 * Test: scan + sum
 * -------------------------------------------------------------------------- */

static test_result_t test_scan_sum(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);

    ray_op_t* v1 = ray_scan(g, "v1");
    TEST_ASSERT_NOT_NULL(v1);

    ray_op_t* result_op = ray_sum(g, v1);
    ray_t* result = ray_execute(g, result_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 550);  /* 10+20+...+100 */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: scan + filter + count
 * -------------------------------------------------------------------------- */

static test_result_t test_filter_count(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1 = ray_scan(g, "v1");
    ray_op_t* threshold = ray_const_i64(g, 50);
    ray_op_t* pred = ray_ge(g, v1, threshold);
    ray_op_t* filtered = ray_filter(g, v1, pred);
    ray_op_t* cnt = ray_count(g, filtered);

    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 6);  /* 50,60,70,80,90,100 */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: arithmetic + reduction
 * -------------------------------------------------------------------------- */

static test_result_t test_arithmetic(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v3 = ray_scan(g, "v3");
    ray_op_t* two = ray_const_f64(g, 2.0);
    ray_op_t* doubled = ray_mul(g, v3, two);
    ray_op_t* total = ray_sum(g, doubled);

    ray_t* result = ray_execute(g, total);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* sum(v3) = 60.0, doubled = 120.0 */
    TEST_ASSERT_EQ_F(result->f64, 120.0, 1e-6);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: group by id1, sum(v1)
 * -------------------------------------------------------------------------- */

static test_result_t test_group_sum(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* key = ray_scan(g, "id1");
    ray_op_t* val = ray_scan(g, "v1");

    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Should have 3 groups (id1=1,2,3) */
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 2);
    int64_t nrows = ray_table_nrows(result);
    TEST_ASSERT_EQ_I(nrows, 3);

    /* Verify sums: id1=1: 10+20+70+100=200, id1=2: 30+40+80=150, id1=3: 50+60+90=200 */
    ray_t* sum_col = ray_table_get_col_idx(result, 1);
    TEST_ASSERT_NOT_NULL(sum_col);

    int64_t sum1 = 0, sum2 = 0, sum3 = 0;
    ray_t* id_col = ray_table_get_col_idx(result, 0);
    for (int64_t i = 0; i < nrows; i++) {
        int64_t id = ((int64_t*)ray_data(id_col))[i];
        int64_t s = ((int64_t*)ray_data(sum_col))[i];
        if (id == 1) sum1 = s;
        else if (id == 2) sum2 = s;
        else if (id == 3) sum3 = s;
    }
    TEST_ASSERT_EQ_I(sum1, 200);
    TEST_ASSERT_EQ_I(sum2, 150);
    TEST_ASSERT_EQ_I(sum3, 200);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: graph new/free
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_lifecycle(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQ_U(g->node_count, 0);

    ray_op_t* v1 = ray_scan(g, "v1");
    TEST_ASSERT_NOT_NULL(v1);
    TEST_ASSERT_EQ_U(v1->opcode, OP_SCAN);
    TEST_ASSERT_EQ_U(g->node_count, 1);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: optimizer constant-folds scalar literal expressions
 * -------------------------------------------------------------------------- */

static test_result_t test_optimizer_constant_fold(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_graph_t* g = ray_graph_new(NULL);
    TEST_ASSERT_NOT_NULL(g);

    ray_op_t* c1 = ray_const_i64(g, 2);
    ray_op_t* c2 = ray_const_i64(g, 3);
    ray_op_t* mul = ray_mul(g, c1, c2);       /* 6 */
    ray_op_t* c3 = ray_const_i64(g, 5);
    ray_op_t* add = ray_add(g, mul, c3);      /* 11 */
    TEST_ASSERT_NOT_NULL(add);
    TEST_ASSERT_EQ_I(add->opcode, OP_ADD);

    ray_op_t* opt = ray_optimize(g, add);
    TEST_ASSERT_NOT_NULL(opt);
    TEST_ASSERT_EQ_I(opt->opcode, OP_CONST);

    ray_t* out = ray_execute(g, opt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I(out->type, -RAY_I64);
    TEST_ASSERT_EQ_I(out->i64, 11);

    ray_release(out);
    ray_graph_free(g);

    /* Also verify comparison folding to BOOL atom. */
    g = ray_graph_new(NULL);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* a = ray_const_i64(g, 4);
    ray_op_t* b = ray_const_i64(g, 9);
    ray_op_t* lt = ray_lt(g, a, b);
    ray_op_t* opt_bool = ray_optimize(g, lt);
    TEST_ASSERT_NOT_NULL(opt_bool);
    TEST_ASSERT_EQ_I(opt_bool->opcode, OP_CONST);

    ray_t* out_bool = ray_execute(g, opt_bool);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_bool));
    TEST_ASSERT_EQ_I(out_bool->type, -RAY_BOOL);
    TEST_ASSERT_EQ_I(out_bool->u8, 1);
    ray_release(out_bool);
    ray_graph_free(g);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: optimizer constant-folds DATE/TIME/I32 atom arithmetic
 *
 * Pre-fix: ray_const_atom stored out_type as the negative atom-tag
 * (-RAY_DATE etc.), so fold_binary_const's `case RAY_I32: case RAY_DATE:
 * case RAY_TIME:` arm never fired — promote(-RAY_DATE, -RAY_DATE) returned
 * the unknown rung (RAY_BOOL), routing arithmetic through the BOOL arm
 * which has no OP_SUB handler.  Post-fix: ray_const_atom normalises to
 * the positive type tag, matching ray_const_i64 / _f64 / _bool, so
 * fold_binary_const's I32/DATE/TIME arm fires and replaces the binary
 * op with an OP_CONST literal.
 * -------------------------------------------------------------------------- */

static test_result_t test_optimizer_fold_atom_arith(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* DATE - DATE → I32 (days difference).  2026.05.05 - 2026.05.01 = 4. */
    ray_graph_t* g = ray_graph_new(NULL);
    TEST_ASSERT_NOT_NULL(g);
    /* Date epoch = 2000.01.01.  2026.05.05 = 9621 days, 2026.05.01 = 9617. */
    ray_t* d1 = ray_date(9621);
    ray_t* d2 = ray_date(9617);
    ray_op_t* c1 = ray_const_atom(g, d1);
    ray_op_t* c2 = ray_const_atom(g, d2);
    /* Release the atoms — ray_const_atom retained them. */
    ray_release(d1);
    ray_release(d2);
    ray_op_t* sub = ray_sub(g, c1, c2);
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQ_I(sub->opcode, OP_SUB);

    ray_op_t* opt = ray_optimize(g, sub);
    TEST_ASSERT_NOT_NULL(opt);
    /* The fold must have fired — node rewritten to OP_CONST. */
    TEST_ASSERT_EQ_I(opt->opcode, OP_CONST);

    ray_t* out = ray_execute(g, opt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I(out->type, -RAY_I32);
    TEST_ASSERT_EQ_I(out->i32, 4);
    ray_release(out);
    ray_graph_free(g);

    /* I32 + I32 → I32. */
    g = ray_graph_new(NULL);
    TEST_ASSERT_NOT_NULL(g);
    ray_t* i1 = ray_i32(5);
    ray_t* i2 = ray_i32(7);
    ray_op_t* ci1 = ray_const_atom(g, i1);
    ray_op_t* ci2 = ray_const_atom(g, i2);
    ray_release(i1);
    ray_release(i2);
    ray_op_t* add = ray_add(g, ci1, ci2);
    ray_op_t* opt_i32 = ray_optimize(g, add);
    TEST_ASSERT_NOT_NULL(opt_i32);
    TEST_ASSERT_EQ_I(opt_i32->opcode, OP_CONST);

    ray_t* out_i32 = ray_execute(g, opt_i32);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_i32));
    TEST_ASSERT_EQ_I(out_i32->type, -RAY_I32);
    TEST_ASSERT_EQ_I(out_i32->i32, 12);
    ray_release(out_i32);
    ray_graph_free(g);

    /* TIME - TIME → I32 (delta in milliseconds).  ray_time stores ms since
     * midnight; 11:00 - 09:00 = 7200000 ms. */
    g = ray_graph_new(NULL);
    TEST_ASSERT_NOT_NULL(g);
    ray_t* t1 = ray_time(11LL * 3600 * 1000);
    ray_t* t2 = ray_time(9LL * 3600 * 1000);
    ray_op_t* ct1 = ray_const_atom(g, t1);
    ray_op_t* ct2 = ray_const_atom(g, t2);
    ray_release(t1);
    ray_release(t2);
    ray_op_t* tsub = ray_sub(g, ct1, ct2);
    ray_op_t* opt_t = ray_optimize(g, tsub);
    TEST_ASSERT_NOT_NULL(opt_t);
    TEST_ASSERT_EQ_I(opt_t->opcode, OP_CONST);

    ray_t* out_t = ray_execute(g, opt_t);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_t));
    /* TIME - TIME folds via the I32/DATE/TIME arm → I32 atom carrying the
     * raw 32-bit difference of the millisecond payloads. */
    TEST_ASSERT_EQ_I(out_t->type, -RAY_I32);
    TEST_ASSERT_EQ_I(out_t->i32, 7200000);
    ray_release(out_t);
    ray_graph_free(g);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: optimizer simplifies FILTER with constant predicates
 * -------------------------------------------------------------------------- */

static test_result_t test_optimizer_filter_const_predicate(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_test_table();
    TEST_ASSERT_NOT_NULL(tbl);

    /* FILTER(..., true) -> pass-through */
    ray_graph_t* g_true = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g_true);
    ray_op_t* v1_true = ray_scan(g_true, "v1");
    ray_op_t* pred_true = ray_const_bool(g_true, true);
    ray_op_t* filt_true = ray_filter(g_true, v1_true, pred_true);
    ray_op_t* opt_true = ray_optimize(g_true, filt_true);
    TEST_ASSERT_NOT_NULL(opt_true);
    TEST_ASSERT_EQ_I(opt_true->opcode, OP_MATERIALIZE);
    ray_t* out_true = ray_execute(g_true, opt_true);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_true));
    TEST_ASSERT_EQ_I(out_true->type, RAY_I64);
    TEST_ASSERT_EQ_I(out_true->len, 10);
    ray_release(out_true);
    ray_graph_free(g_true);

    /* FILTER(..., false) -> empty via HEAD 0 */
    ray_graph_t* g_false = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g_false);
    ray_op_t* v1_false = ray_scan(g_false, "v1");
    ray_op_t* pred_false = ray_const_bool(g_false, false);
    ray_op_t* filt_false = ray_filter(g_false, v1_false, pred_false);
    ray_op_t* opt_false = ray_optimize(g_false, filt_false);
    TEST_ASSERT_NOT_NULL(opt_false);
    TEST_ASSERT_EQ_I(opt_false->opcode, OP_HEAD);
    ray_t* out_false = ray_execute(g_false, opt_false);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_false));
    TEST_ASSERT_EQ_I(out_false->type, RAY_I64);
    TEST_ASSERT_EQ_I(out_false->len, 0);
    ray_release(out_false);
    ray_graph_free(g_false);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: group aggregation over affine inputs (SUM/AVG(v1 + const))
 * -------------------------------------------------------------------------- */

static test_result_t test_group_affine_agg_input(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_test_table();
    TEST_ASSERT_NOT_NULL(tbl);

    /* Scalar aggregate: SUM(v1 + 1), AVG(v1 + 1) */
    ray_graph_t* g_scalar = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g_scalar);
    ray_op_t* v1_scalar = ray_scan(g_scalar, "v1");
    ray_op_t* one_scalar = ray_const_i64(g_scalar, 1);
    ray_op_t* expr_scalar = ray_add(g_scalar, v1_scalar, one_scalar);
    uint16_t scalar_ops[] = { OP_SUM, OP_AVG };
    ray_op_t* scalar_ins[] = { expr_scalar, expr_scalar };
    ray_op_t* grp_scalar = ray_group(g_scalar, NULL, 0, scalar_ops, scalar_ins, 2);
    ray_t* out_scalar = ray_execute(g_scalar, grp_scalar);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_scalar));
    TEST_ASSERT_EQ_I(out_scalar->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(out_scalar), 1);
    TEST_ASSERT_EQ_I(ray_table_ncols(out_scalar), 2);

    ray_t* sum_col = ray_table_get_col_idx(out_scalar, 0);
    ray_t* avg_col = ray_table_get_col_idx(out_scalar, 1);
    TEST_ASSERT_NOT_NULL(sum_col);
    TEST_ASSERT_NOT_NULL(avg_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 560);
    TEST_ASSERT_EQ_F(((double*)ray_data(avg_col))[0], 56.0, 1e-6);
    ray_release(out_scalar);
    ray_graph_free(g_scalar);

    /* Scalar variants: SUM(1 + v1), SUM(v1 - 1) */
    ray_graph_t* g_scalar2 = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g_scalar2);
    ray_op_t* v1_s2 = ray_scan(g_scalar2, "v1");
    ray_op_t* one_s2a = ray_const_i64(g_scalar2, 1);
    ray_op_t* one_plus_v1 = ray_add(g_scalar2, one_s2a, v1_s2);
    ray_op_t* one_s2b = ray_const_i64(g_scalar2, 1);
    ray_op_t* v1_minus_one = ray_sub(g_scalar2, v1_s2, one_s2b);
    uint16_t scalar2_ops[] = { OP_SUM, OP_SUM };
    ray_op_t* scalar2_ins[] = { one_plus_v1, v1_minus_one };
    ray_op_t* grp_scalar2 = ray_group(g_scalar2, NULL, 0, scalar2_ops, scalar2_ins, 2);
    ray_t* out_scalar2 = ray_execute(g_scalar2, grp_scalar2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_scalar2));
    TEST_ASSERT_EQ_I(ray_table_nrows(out_scalar2), 1);
    TEST_ASSERT_EQ_I(ray_table_ncols(out_scalar2), 2);
    ray_t* sum_plus_col = ray_table_get_col_idx(out_scalar2, 0);
    ray_t* sum_minus_col = ray_table_get_col_idx(out_scalar2, 1);
    TEST_ASSERT_NOT_NULL(sum_plus_col);
    TEST_ASSERT_NOT_NULL(sum_minus_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_plus_col))[0], 560);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_minus_col))[0], 540);
    ray_release(out_scalar2);
    ray_graph_free(g_scalar2);

    /* Nested scalar expression in agg input: SUM(v1 + (2 * 1)) */
    ray_graph_t* g_nested = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g_nested);
    ray_op_t* v1_nested = ray_scan(g_nested, "v1");
    ray_op_t* c2_nested = ray_const_i64(g_nested, 2);
    ray_op_t* c1_nested = ray_const_i64(g_nested, 1);
    ray_op_t* mul_nested = ray_mul(g_nested, c2_nested, c1_nested);
    ray_op_t* expr_nested = ray_add(g_nested, v1_nested, mul_nested);
    uint16_t nested_ops[] = { OP_SUM };
    ray_op_t* nested_ins[] = { expr_nested };
    ray_op_t* grp_nested = ray_group(g_nested, NULL, 0, nested_ops, nested_ins, 1);
    ray_t* out_nested = ray_execute(g_nested, grp_nested);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_nested));
    TEST_ASSERT_EQ_I(ray_table_nrows(out_nested), 1);
    ray_t* nested_sum_col = ray_table_get_col_idx(out_nested, 0);
    TEST_ASSERT_NOT_NULL(nested_sum_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(nested_sum_col))[0], 570);
    ray_release(out_nested);
    ray_graph_free(g_nested);

    /* Nested constants on both sides: SUM((2*1) + v1), SUM(v1 - (2*1)) */
    ray_graph_t* g_nested2 = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g_nested2);
    ray_op_t* v1_nested2 = ray_scan(g_nested2, "v1");
    ray_op_t* c2_nested2 = ray_const_i64(g_nested2, 2);
    ray_op_t* c1_nested2 = ray_const_i64(g_nested2, 1);
    ray_op_t* mul_nested2 = ray_mul(g_nested2, c2_nested2, c1_nested2);
    ray_op_t* expr_nested_add_lhs = ray_add(g_nested2, mul_nested2, v1_nested2);
    ray_op_t* expr_nested_sub_rhs = ray_sub(g_nested2, v1_nested2, mul_nested2);
    uint16_t nested2_ops[] = { OP_SUM, OP_SUM };
    ray_op_t* nested2_ins[] = { expr_nested_add_lhs, expr_nested_sub_rhs };
    ray_op_t* grp_nested2 = ray_group(g_nested2, NULL, 0, nested2_ops, nested2_ins, 2);
    ray_t* out_nested2 = ray_execute(g_nested2, grp_nested2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_nested2));
    TEST_ASSERT_EQ_I(ray_table_nrows(out_nested2), 1);
    ray_t* nested2_sum_add = ray_table_get_col_idx(out_nested2, 0);
    ray_t* nested2_sum_sub = ray_table_get_col_idx(out_nested2, 1);
    TEST_ASSERT_NOT_NULL(nested2_sum_add);
    TEST_ASSERT_NOT_NULL(nested2_sum_sub);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(nested2_sum_add))[0], 570);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(nested2_sum_sub))[0], 530);
    ray_release(out_nested2);
    ray_graph_free(g_nested2);

    /* Generic linear forms: SUM((v1 + 1) * 2), AVG(v1 + id1 + 1) */
    ray_graph_t* g_linear = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g_linear);
    ray_op_t* v1_linear = ray_scan(g_linear, "v1");
    ray_op_t* id1_linear = ray_scan(g_linear, "id1");
    ray_op_t* one_linear_a = ray_const_i64(g_linear, 1);
    ray_op_t* one_linear_b = ray_const_i64(g_linear, 1);
    ray_op_t* two_linear = ray_const_i64(g_linear, 2);
    ray_op_t* v1_plus_one = ray_add(g_linear, v1_linear, one_linear_a);
    ray_op_t* sum_expr = ray_mul(g_linear, v1_plus_one, two_linear);
    ray_op_t* avg_expr = ray_add(g_linear, ray_add(g_linear, v1_linear, id1_linear), one_linear_b);
    uint16_t linear_ops[] = { OP_SUM, OP_AVG };
    ray_op_t* linear_ins[] = { sum_expr, avg_expr };
    ray_op_t* grp_linear = ray_group(g_linear, NULL, 0, linear_ops, linear_ins, 2);
    ray_t* out_linear = ray_execute(g_linear, grp_linear);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_linear));
    TEST_ASSERT_EQ_I(ray_table_nrows(out_linear), 1);
    ray_t* linear_sum_col = ray_table_get_col_idx(out_linear, 0);
    ray_t* linear_avg_col = ray_table_get_col_idx(out_linear, 1);
    TEST_ASSERT_NOT_NULL(linear_sum_col);
    TEST_ASSERT_NOT_NULL(linear_avg_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(linear_sum_col))[0], 1120);
    TEST_ASSERT_EQ_F(((double*)ray_data(linear_avg_col))[0], 57.9, 1e-6);
    ray_release(out_linear);
    ray_graph_free(g_linear);

    /* Pure constant agg input should broadcast per row: SUM(2 * 1) */
    ray_graph_t* g_const = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g_const);
    ray_op_t* c2 = ray_const_i64(g_const, 2);
    ray_op_t* c1 = ray_const_i64(g_const, 1);
    ray_op_t* mul = ray_mul(g_const, c2, c1);
    uint16_t const_ops[] = { OP_SUM };
    ray_op_t* const_ins[] = { mul };
    ray_op_t* grp_const = ray_group(g_const, NULL, 0, const_ops, const_ins, 1);
    ray_t* out_const = ray_execute(g_const, grp_const);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_const));
    TEST_ASSERT_EQ_I(ray_table_nrows(out_const), 1);
    ray_t* const_sum_col = ray_table_get_col_idx(out_const, 0);
    TEST_ASSERT_NOT_NULL(const_sum_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(const_sum_col))[0], 20);
    ray_release(out_const);
    ray_graph_free(g_const);

    /* Grouped aggregate: id1, SUM(v1 + 1) */
    ray_graph_t* g_group = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g_group);
    ray_op_t* key = ray_scan(g_group, "id1");
    ray_op_t* v1_group = ray_scan(g_group, "v1");
    ray_op_t* one_group = ray_const_i64(g_group, 1);
    ray_op_t* expr_group = ray_add(g_group, v1_group, one_group);
    ray_op_t* keys[] = { key };
    uint16_t ops[] = { OP_SUM };
    ray_op_t* ins[] = { expr_group };
    ray_op_t* grp = ray_group(g_group, keys, 1, ops, ins, 1);
    ray_t* out_group = ray_execute(g_group, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out_group));
    TEST_ASSERT_EQ_I(out_group->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(out_group), 3);
    TEST_ASSERT_EQ_I(ray_table_ncols(out_group), 2);

    ray_t* id_col = ray_table_get_col_idx(out_group, 0);
    ray_t* s_col = ray_table_get_col_idx(out_group, 1);
    TEST_ASSERT_NOT_NULL(id_col);
    TEST_ASSERT_NOT_NULL(s_col);
    int64_t sum1 = 0, sum2 = 0, sum3 = 0;
    for (int64_t i = 0; i < ray_table_nrows(out_group); i++) {
        int64_t id = ((int64_t*)ray_data(id_col))[i];
        int64_t s = ((int64_t*)ray_data(s_col))[i];
        if (id == 1) sum1 = s;
        else if (id == 2) sum2 = s;
        else if (id == 3) sum3 = s;
    }
    TEST_ASSERT_EQ_I(sum1, 204);
    TEST_ASSERT_EQ_I(sum2, 153);
    TEST_ASSERT_EQ_I(sum3, 203);

    ray_release(out_group);
    ray_graph_free(g_group);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Suite
 * -------------------------------------------------------------------------- */

const test_entry_t graph_entries[] = {
    { "graph/lifecycle", test_graph_lifecycle, NULL, NULL },
    { "graph/scan_sum", test_scan_sum, NULL, NULL },
    { "graph/filter_count", test_filter_count, NULL, NULL },
    { "graph/arithmetic", test_arithmetic, NULL, NULL },
    { "graph/group_sum", test_group_sum, NULL, NULL },
    { "graph/opt_fold", test_optimizer_constant_fold, NULL, NULL },
    { "graph/opt_fold_atom_arith", test_optimizer_fold_atom_arith, NULL, NULL },
    { "graph/opt_filter_const", test_optimizer_filter_const_predicate, NULL, NULL },
    { "graph/group_affine_agg", test_group_affine_agg_input, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


