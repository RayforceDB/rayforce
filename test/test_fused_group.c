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
 * test_fused_group.c — phase-1 OP_FILTERED_GROUP exec coverage.
 *
 * Builds a tiny I64 table directly via the graph API (mirrors
 * test_group_extra.c), constructs an OP_FILTERED_GROUP node with a
 * (==/!= col const) predicate plus a single OP_COUNT aggregate, runs
 * ray_execute, and verifies the materialised key+count table.
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "ops/fused_group.h"
#include "table/sym.h"
#include <string.h>

/* AdvEngineID = [0, 1, 0, 2, 0, 1]
 *   (== 0) → 3 rows (group 0, count 3)
 *   (!= 0) → 3 rows in 2 groups (1 → 2, 2 → 1) */
static ray_t* make_adv_table(void) {
    int64_t adv[] = { 0, 1, 0, 2, 0, 1 };
    ray_t* col = ray_vec_new(RAY_I64, 6);
    if (!col || RAY_IS_ERR(col)) return NULL;
    col->len = 6;
    memcpy(ray_data(col), adv, sizeof(adv));
    int64_t name = ray_sym_intern("AdvEngineID", 11);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, col);
    ray_release(col);
    return tbl;
}

/* (== AdvEngineID 0) by AdvEngineID → 1 row, count = 3 */
static test_result_t test_eq_count(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_table();
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k    = ray_scan(g, "AdvEngineID");
    ray_op_t* scan_pred = ray_scan(g, "AdvEngineID");
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(res->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(cnt_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(cnt_col))[0], 3);

    int64_t adv_sym = ray_sym_intern("AdvEngineID", 11);
    ray_t* key_col = ray_table_get_col(res, adv_sym);
    TEST_ASSERT_NOT_NULL(key_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(key_col))[0], 0);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (!= AdvEngineID 0) by AdvEngineID → 2 rows: {1: 2, 2: 1} (order
 * depends on hash, so verify by lookup). */
static test_result_t test_ne_two_groups(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_table();
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvEngineID");
    ray_op_t* scan_pred = ray_scan(g, "AdvEngineID");
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_NE, scan_pred, zero);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 2);

    int64_t adv_sym = ray_sym_intern("AdvEngineID", 11);
    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* key_col = ray_table_get_col(res, adv_sym);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    TEST_ASSERT_NOT_NULL(key_col);
    TEST_ASSERT_NOT_NULL(cnt_col);
    int64_t* ks = (int64_t*)ray_data(key_col);
    int64_t* cs = (int64_t*)ray_data(cnt_col);
    int64_t got_one = -1, got_two = -1;
    for (int64_t i = 0; i < 2; i++) {
        if (ks[i] == 1) got_one = cs[i];
        else if (ks[i] == 2) got_two = cs[i];
    }
    TEST_ASSERT_EQ_I(got_one, 2);
    TEST_ASSERT_EQ_I(got_two, 1);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Finding #3 (constant truncation) regression: a U8 column compared
 * against a constant > 255 must match 0 rows for `==`, not the
 * truncated value (300 → 44).  Build a U8 column with values
 * {0, 1, 0, 2, 0, 1} (same as AdvEngineID test) and check that
 * fp_eval_cmp folds the out-of-range constant. */
static ray_t* make_adv_u8_table(void) {
    uint8_t adv[] = { 0, 1, 0, 2, 0, 1, 44 };  /* trailing 44 is the
                                                  truncated value of 300 */
    ray_t* col = ray_vec_new(RAY_U8, 7);
    if (!col || RAY_IS_ERR(col)) return NULL;
    col->len = 7;
    memcpy(ray_data(col), adv, sizeof(adv));
    int64_t name = ray_sym_intern("AdvU8", 5);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, col);
    ray_release(col);
    return tbl;
}

static test_result_t test_eq_const_out_of_range_u8(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_u8_table();
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvU8");
    ray_op_t* scan_pred = ray_scan(g, "AdvU8");
    ray_op_t* big       = ray_const_i64(g, 300);  /* out-of-range for U8 */
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, big);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* No U8 row can equal 300 — pre-truncation fix would have matched
     * the value 44 (300 & 0xFF) and returned 1 row. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 0);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* `u8_col != 300` is a tautology — every value is unequal to 300, so
 * with the fold we expect 7 source rows ⇒ count 7 in a single group
 * (BUT keyed by U8 so groups are {0:3, 1:2, 2:1, 44:1} = 4 rows). */
static test_result_t test_ne_const_out_of_range_u8(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_u8_table();
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvU8");
    ray_op_t* scan_pred = ray_scan(g, "AdvU8");
    ray_op_t* big       = ray_const_i64(g, 300);
    ray_op_t* pred      = ray_binop(g, OP_NE, scan_pred, big);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    /* Tautology — every row passes — 4 distinct U8 keys: {0, 1, 2, 44}. */
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 4);

    int64_t cnt_sym = ray_sym_intern("count", 5);
    ray_t* cnt_col = ray_table_get_col(res, cnt_sym);
    int64_t total = 0;
    for (int64_t i = 0; i < ray_table_nrows(res); i++)
        total += ((int64_t*)ray_data(cnt_col))[i];
    TEST_ASSERT_EQ_I(total, 7);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Finding #4 (signed narrow agg read) regression: SUM of an I16
 * column with negative values must produce the correct signed sum,
 * not a sum where -1 is read as 65535. */
static test_result_t test_sum_negative_i16(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Group key — single group so we sum everything together. */
    int64_t k[] = { 0, 0, 0, 0 };
    /* I16 input — -1 + -2 + 3 + 4 = 4.  Pre-fix would compute
     * 65535 + 65534 + 3 + 4 = 131076. */
    int16_t v[] = { -1, -2, 3, 4 };

    ray_t* kcol = ray_vec_new(RAY_I64, 4);
    kcol->len = 4;
    memcpy(ray_data(kcol), k, sizeof(k));
    ray_t* vcol = ray_vec_new(RAY_I16, 4);
    vcol->len = 4;
    memcpy(ray_data(vcol), v, sizeof(v));

    int64_t kn = ray_sym_intern("g", 1);
    int64_t vn = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, kn, kcol); ray_release(kcol);
    tbl = ray_table_add_col(tbl, vn, vcol); ray_release(vcol);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "g");
    ray_op_t* scan_v    = ray_scan(g, "v");
    /* Predicate: (== g 0) — every row passes, single group. */
    ray_op_t* zero      = ray_const_i64(g, 0);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_k, zero);
    uint16_t  agg_ops[] = { OP_SUM, OP_MIN, OP_MAX };
    ray_op_t* agg_ins[] = { scan_v, scan_v, scan_v };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 3);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 1);

    int64_t sum_sym = ray_sym_intern("sum", 3);
    int64_t min_sym = ray_sym_intern("min", 3);
    int64_t max_sym = ray_sym_intern("max", 3);
    ray_t* sum_col = ray_table_get_col(res, sum_sym);
    ray_t* min_col = ray_table_get_col(res, min_sym);
    ray_t* max_col = ray_table_get_col(res, max_sym);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 4);   /* -1+-2+3+4 */
    /* MIN/MAX out cols are RAY_I16, so cast through int16_t. */
    TEST_ASSERT_EQ_I((int64_t)((int16_t*)ray_data(min_col))[0], -2);
    TEST_ASSERT_EQ_I((int64_t)((int16_t*)ray_data(max_col))[0], 4);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (== AdvEngineID 99) → no rows pass → empty result. */
static test_result_t test_eq_no_match(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_adv_table();
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_k    = ray_scan(g, "AdvEngineID");
    ray_op_t* scan_pred = ray_scan(g, "AdvEngineID");
    ray_op_t* big       = ray_const_i64(g, 99);
    ray_op_t* pred      = ray_binop(g, OP_EQ, scan_pred, big);
    uint16_t  agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { scan_k };
    ray_op_t* keys[]    = { scan_k };
    ray_op_t* fused     = ray_filtered_group(g, pred, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(fused);

    ray_t* res = ray_execute(g, fused);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_table_nrows(res), 0);

    ray_release(res);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t fused_group_entries[] = {
    { "fused_group/eq_count",                    test_eq_count,                    NULL, NULL },
    { "fused_group/ne_two_groups",               test_ne_two_groups,               NULL, NULL },
    { "fused_group/eq_no_match",                 test_eq_no_match,                 NULL, NULL },
    { "fused_group/eq_const_out_of_range_u8",    test_eq_const_out_of_range_u8,    NULL, NULL },
    { "fused_group/ne_const_out_of_range_u8",    test_ne_const_out_of_range_u8,    NULL, NULL },
    { "fused_group/sum_negative_i16",            test_sum_negative_i16,            NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
