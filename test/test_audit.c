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
#include "ops/rowsel.h"
#include <string.h>
#include <math.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Build a 5-row table: id=[1,1,2,2,3], val=[10,20,30,40,50] */
static ray_t* make_audit_table(void) {
    (void)ray_sym_init();

    int64_t id_data[]  = {1, 1, 2, 2, 3};
    int64_t val_data[] = {10, 20, 30, 40, 50};

    ray_t* id_vec  = ray_vec_from_raw(RAY_I64, id_data, 5);
    ray_t* val_vec = ray_vec_from_raw(RAY_I64, val_data, 5);

    int64_t name_id  = ray_sym_intern("id", 2);
    int64_t name_val = ray_sym_intern("val", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name_id, id_vec);
    tbl = ray_table_add_col(tbl, name_val, val_vec);

    ray_release(id_vec);
    ray_release(val_vec);
    return tbl;
}

/* Build a 10-row table: id=[1,1,2,2,3,3,1,2,3,1], val=[10..100 step 10] */
static ray_t* make_audit_table_10(void) {
    (void)ray_sym_init();

    int64_t id_data[]  = {1, 1, 2, 2, 3, 3, 1, 2, 3, 1};
    int64_t val_data[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};

    ray_t* id_vec  = ray_vec_from_raw(RAY_I64, id_data, 10);
    ray_t* val_vec = ray_vec_from_raw(RAY_I64, val_data, 10);

    int64_t name_id  = ray_sym_intern("id", 2);
    int64_t name_val = ray_sym_intern("val", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name_id, id_vec);
    tbl = ray_table_add_col(tbl, name_val, val_vec);

    ray_release(id_vec);
    ray_release(val_vec);
    return tbl;
}

/* Build a RAY_SEL bitmap from a boolean array. */
static ray_t* make_selection(const uint8_t* mask, int64_t n) {
    ray_t* bvec = ray_vec_from_raw(RAY_BOOL, mask, n);
    ray_t* sel = ray_rowsel_from_pred(bvec);
    ray_release(bvec);
    return sel;
}

/* -----------------------------------------------------------------------
 * Smoke test — basic table creation + SUM
 * ----------------------------------------------------------------------- */

static test_result_t test_audit_smoke(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* s   = ray_sum(g, val);

    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 150);  /* 10+20+30+40+50 */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* -----------------------------------------------------------------------
 * Category 1: Selection bitmap + GROUP BY regression tests
 * ----------------------------------------------------------------------- */

/* Selection mask=[0,0,1,1,1] (rows where id>1) + GROUP BY id, SUM(val)
 * Expected: 2 groups — {id=2: sum=70, id=3: sum=50}, total=120 */
static test_result_t test_sel_group_sum(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);

    /* Set selection: rows 2,3,4 pass (id>1) */
    uint8_t mask[] = {0, 0, 1, 1, 1};
    ray_t* sel = make_selection(mask, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ray_retain(sel);
    g->selection = sel;

    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    int64_t nrows = ray_table_nrows(result);
    TEST_ASSERT_EQ_I(nrows, 2);  /* groups: id=2, id=3 */

    /* Verify sums: id=2: 30+40=70, id=3: 50 */
    ray_t* id_col  = ray_table_get_col_idx(result, 0);
    ray_t* sum_col = ray_table_get_col_idx(result, 1);
    int64_t total = 0;
    int64_t sum2 = 0, sum3 = 0;
    for (int64_t i = 0; i < nrows; i++) {
        int64_t id = ((int64_t*)ray_data(id_col))[i];
        int64_t s  = ((int64_t*)ray_data(sum_col))[i];
        total += s;
        if (id == 2) sum2 = s;
        else if (id == 3) sum3 = s;
    }
    TEST_ASSERT_EQ_I(total, 120);
    TEST_ASSERT_EQ_I(sum2, 70);
    TEST_ASSERT_EQ_I(sum3, 50);

    ray_release(result);
    ray_release(sel);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Same selection + GROUP BY id, COUNT(val) → 2 groups */
static test_result_t test_sel_group_count(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);

    uint8_t mask[] = {0, 0, 1, 1, 1};
    ray_t* sel = make_selection(mask, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ray_retain(sel);
    g->selection = sel;

    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_COUNT };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    int64_t nrows = ray_table_nrows(result);
    TEST_ASSERT_EQ_I(nrows, 2);  /* groups: id=2, id=3 */

    /* Verify counts: id=2: 2 rows, id=3: 1 row */
    ray_t* id_col  = ray_table_get_col_idx(result, 0);
    ray_t* cnt_col = ray_table_get_col_idx(result, 1);
    int64_t cnt2 = 0, cnt3 = 0;
    for (int64_t i = 0; i < nrows; i++) {
        int64_t id = ((int64_t*)ray_data(id_col))[i];
        int64_t c  = ((int64_t*)ray_data(cnt_col))[i];
        if (id == 2) cnt2 = c;
        else if (id == 3) cnt3 = c;
    }
    TEST_ASSERT_EQ_I(cnt2, 2);
    TEST_ASSERT_EQ_I(cnt3, 1);

    ray_release(result);
    ray_release(sel);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Selection + scalar SUM(val) (no GROUP BY keys) → 120 */
static test_result_t test_sel_group_scalar(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);

    uint8_t mask[] = {0, 0, 1, 1, 1};
    ray_t* sel = make_selection(mask, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ray_retain(sel);
    g->selection = sel;

    ray_op_t* val = ray_scan(g, "val");
    uint16_t agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { val };

    ray_op_t* grp = ray_group(g, NULL, 0, agg_ops, agg_ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    int64_t nrows = ray_table_nrows(result);
    TEST_ASSERT_EQ_I(nrows, 1);

    ray_t* sum_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 120);

    ray_release(result);
    ray_release(sel);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* -----------------------------------------------------------------------
 * Category 1: Selection bitmap + SORT regression tests
 * ----------------------------------------------------------------------- */

/* Selection mask=[1,0,1,0,1] (rows 0,2,4) + SORT by val DESC
 * Expected: [50,30,10], nrows==3 */
static test_result_t test_sel_sort(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);

    uint8_t mask[] = {1, 0, 1, 0, 1};
    ray_t* sel = make_selection(mask, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ray_retain(sel);
    g->selection = sel;

    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* val_key = ray_scan(g, "val");
    ray_op_t* keys[] = { val_key };
    uint8_t descs[] = { 1 };           /* descending */
    uint8_t nulls_first[] = { 0 };
    ray_op_t* sort_op = ray_sort_op(g, tbl_op, keys, descs, nulls_first, 1);

    ray_t* result = ray_execute(g, sort_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    /* Verify descending order: [50, 30, 10] */
    int64_t name_val = ray_sym_intern("val", 3);
    ray_t* val_col = ray_table_get_col(result, name_val);
    TEST_ASSERT_NOT_NULL(val_col);
    int64_t* vdata = (int64_t*)ray_data(val_col);
    TEST_ASSERT_EQ_I(vdata[0], 50);
    TEST_ASSERT_EQ_I(vdata[1], 30);
    TEST_ASSERT_EQ_I(vdata[2], 10);

    ray_release(result);
    ray_release(sel);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* The full reported bug chain: selection + GROUP BY + SORT.
 * Table 10 rows: id=[1,1,2,2,3,3,1,2,3,1], val=[10..100].
 * Selection: mask=[0,0,1,1,1,1,0,1,1,0] (id>1 → 6 rows pass).
 * GROUP BY id, SUM(val) → {2: 30+40+80=150, 3: 50+60+90=200}.
 * SORT by id ASC → [(2,150), (3,200)].
 * Verify nrows==2, ids=[2,3], vals=[150,200]. */
static test_result_t test_sel_group_sort(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table_10();

    ray_graph_t* g = ray_graph_new(tbl);

    uint8_t mask[] = {0, 0, 1, 1, 1, 1, 0, 1, 1, 0};
    ray_t* sel = make_selection(mask, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ray_retain(sel);
    g->selection = sel;

    /* GROUP BY id, SUM(val) */
    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* grp_keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, grp_keys, 1, agg_ops, agg_ins, 1);

    /* SORT by id ASC — use group result as table input */
    ray_op_t* sort_keys[] = { key };
    uint8_t descs[] = { 0 };           /* ascending */
    uint8_t nulls_first[] = { 0 };
    ray_op_t* sort_op = ray_sort_op(g, grp, sort_keys, descs, nulls_first, 1);

    ray_t* result = ray_execute(g, sort_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    int64_t nrows = ray_table_nrows(result);
    TEST_ASSERT_EQ_I(nrows, 2);

    /* Verify sorted: id=[2,3], sum=[150,200] */
    ray_t* id_col  = ray_table_get_col_idx(result, 0);
    ray_t* sum_col = ray_table_get_col_idx(result, 1);
    TEST_ASSERT_NOT_NULL(id_col);
    TEST_ASSERT_NOT_NULL(sum_col);

    int64_t* ids  = (int64_t*)ray_data(id_col);
    int64_t* sums = (int64_t*)ray_data(sum_col);
    TEST_ASSERT_EQ_I(ids[0], 2);
    TEST_ASSERT_EQ_I(ids[1], 3);
    TEST_ASSERT_EQ_I(sums[0], 150);
    TEST_ASSERT_EQ_I(sums[1], 200);

    ray_release(result);
    ray_release(sel);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* -----------------------------------------------------------------------
 * Category 1: Selection bitmap + JOIN / WINDOW / ASOF
 * ----------------------------------------------------------------------- */

/* Selection on left table + INNER JOIN on id.
 * Left: make_audit_table() (id=[1,1,2,2,3], val=[10,20,30,40,50]).
 * Right: id=[2,3], label=[200,300].
 * Selection: mask=[0,0,1,1,1] (rows 2,3,4).
 * INNER JOIN on id → 3 rows (id=2 twice, id=3 once). */
static test_result_t test_sel_join(void) {
    ray_heap_init();
    ray_t* left = make_audit_table();

    /* Build right table: id=[2,3], label=[200,300] */
    int64_t rid_data[]   = {2, 3};
    int64_t rlabel_data[] = {200, 300};
    ray_t* rid_vec   = ray_vec_from_raw(RAY_I64, rid_data, 2);
    ray_t* rlabel_vec = ray_vec_from_raw(RAY_I64, rlabel_data, 2);
    int64_t name_id    = ray_sym_intern("id", 2);
    int64_t name_label = ray_sym_intern("label", 5);
    ray_t* right = ray_table_new(2);
    right = ray_table_add_col(right, name_id, rid_vec);
    right = ray_table_add_col(right, name_label, rlabel_vec);
    ray_release(rid_vec);
    ray_release(rlabel_vec);

    ray_graph_t* g = ray_graph_new(left);

    uint8_t mask[] = {0, 0, 1, 1, 1};
    ray_t* sel = make_selection(mask, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ray_retain(sel);
    g->selection = sel;

    ray_op_t* left_op  = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* lk = ray_scan(g, "id");
    ray_op_t* lk_arr[] = { lk };
    ray_op_t* rk_arr[] = { lk };
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    ray_t* result = ray_execute(g, join_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    ray_release(result);
    ray_release(sel);
    ray_release(right);
    ray_graph_free(g);
    ray_release(left);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Selection + WINDOW ROW_NUMBER() OVER (ORDER BY val ASC).
 * Table: make_audit_table(). Selection: mask=[1,1,1,0,0] (3 rows).
 * Result: nrows==3, with row numbers 1,2,3. */
static test_result_t test_sel_window(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);

    uint8_t mask[] = {1, 1, 1, 0, 0};
    ray_t* sel = make_selection(mask, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ray_retain(sel);
    g->selection = sel;

    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* val_op = ray_scan(g, "val");
    ray_op_t* orders[] = { val_op };
    uint8_t order_descs[] = { 0 };
    uint8_t func_kinds[] = { RAY_WIN_ROW_NUMBER };
    ray_op_t* func_inputs[] = { val_op };
    int64_t func_params[] = { 0 };
    ray_op_t* win = ray_window_op(g, tbl_op,
                                NULL, 0,
                                orders, order_descs, 1,
                                func_kinds, func_inputs, func_params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);

    ray_t* result = ray_execute(g, win);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    ray_release(result);
    ray_release(sel);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Selection + inner ASOF JOIN.
 * Left: time=[100,200,300,400,500], val=[1.0..5.0]. Selection: [1,1,0,0,1].
 * Right: time=[90,150,450], bid=[0.9,1.5,4.5].
 * Inner ASOF → nrows==3. */
static test_result_t test_sel_asof_join(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build left table: time(I64), val(F64) */
    int64_t ltime_data[] = {100, 200, 300, 400, 500};
    double  lval_data[]  = {1.0, 2.0, 3.0, 4.0, 5.0};
    ray_t* ltime_vec = ray_vec_from_raw(RAY_I64, ltime_data, 5);
    ray_t* lval_vec  = ray_vec_from_raw(RAY_F64, lval_data, 5);
    int64_t name_time = ray_sym_intern("time", 4);
    int64_t name_val  = ray_sym_intern("val", 3);
    ray_t* left = ray_table_new(2);
    left = ray_table_add_col(left, name_time, ltime_vec);
    left = ray_table_add_col(left, name_val, lval_vec);
    ray_release(ltime_vec);
    ray_release(lval_vec);

    /* Build right table: time(I64), bid(F64) */
    int64_t rtime_data[] = {90, 150, 450};
    double  rbid_data[]  = {0.9, 1.5, 4.5};
    ray_t* rtime_vec = ray_vec_from_raw(RAY_I64, rtime_data, 3);
    ray_t* rbid_vec  = ray_vec_from_raw(RAY_F64, rbid_data, 3);
    int64_t name_bid = ray_sym_intern("bid", 3);
    ray_t* right = ray_table_new(2);
    right = ray_table_add_col(right, name_time, rtime_vec);
    right = ray_table_add_col(right, name_bid, rbid_vec);
    ray_release(rtime_vec);
    ray_release(rbid_vec);

    ray_graph_t* g = ray_graph_new(left);

    uint8_t mask[] = {1, 1, 0, 0, 1};
    ray_t* sel = make_selection(mask, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ray_retain(sel);
    g->selection = sel;

    ray_op_t* left_op  = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* tkey = ray_scan(g, "time");
    ray_op_t* aj = ray_asof_join(g, left_op, right_op, tkey, NULL, 0, 0);

    ray_t* result = ray_execute(g, aj);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    ray_release(result);
    ray_release(sel);
    ray_release(right);
    ray_graph_free(g);
    ray_release(left);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* -----------------------------------------------------------------------
 * Category 1: Chained selection + boundary cases
 * ----------------------------------------------------------------------- */

/* Two selections ANDed: mask_a=[0,0,1,1,1,1,0,1,1,0], mask_b=[1,1,1,1,1,1,0,0,0,0].
 * AND → [0,0,1,1,1,1,0,0,0,0] → rows 2,3,4,5.
 * GROUP BY id, SUM(val) → 2 groups {2:70, 3:110}. */
static test_result_t test_sel_chained(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table_10();

    ray_graph_t* g = ray_graph_new(tbl);

    /* AND two boolean masks at the source level, then build a single
     * rowsel from the conjunction.  Equivalent semantics to the old
     * ray_sel_and path that operated on bitmap selections. */
    uint8_t mask_a[] = {0, 0, 1, 1, 1, 1, 0, 1, 1, 0};
    uint8_t mask_b[] = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0};
    uint8_t mask_and[10];
    for (int i = 0; i < 10; i++) mask_and[i] = mask_a[i] & mask_b[i];
    g->selection = make_selection(mask_and, 10);
    TEST_ASSERT_NOT_NULL(g->selection);

    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* All rows filtered out (mask all zeros). GROUP BY → 0-row result. */
static test_result_t test_sel_all_filtered(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);

    uint8_t mask[] = {0, 0, 0, 0, 0};
    ray_t* sel = make_selection(mask, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ray_retain(sel);
    g->selection = sel;

    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 0);

    ray_release(result);
    ray_release(sel);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* All rows pass (mask all ones). GROUP BY id, SUM(val) → 3 groups, total=150. */
static test_result_t test_sel_none_filtered(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);

    uint8_t mask[] = {1, 1, 1, 1, 1};
    ray_t* sel = make_selection(mask, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ray_retain(sel);
    g->selection = sel;

    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    /* Sum all values across groups */
    ray_t* sum_col = ray_table_get_col_idx(result, 1);
    int64_t total = 0;
    for (int64_t i = 0; i < 3; i++) {
        total += ((int64_t*)ray_data(sum_col))[i];
    }
    TEST_ASSERT_EQ_I(total, 150);

    ray_release(result);
    ray_release(sel);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* -----------------------------------------------------------------------
 * Category 1: FILTER lazy/eager + HAVING fusion
 * ----------------------------------------------------------------------- */

/* FILTER(table, id>1) → lazy path creates RAY_SEL. Verify nrows==3. */
static test_result_t test_filter_lazy(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* id_col = ray_scan(g, "id");
    ray_op_t* one = ray_const_i64(g, 1);
    ray_op_t* pred = ray_gt(g, id_col, one);
    ray_op_t* filt = ray_filter(g, tbl_op, pred);

    ray_t* result = ray_execute(g, filt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Chained FILTER: FILTER(FILTER(table, id>1), val<50).
 * id>1: rows (2,30),(2,40),(3,50). val<50: (2,30),(2,40) → nrows==2. */
static test_result_t test_filter_lazy_chained(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* id_col = ray_scan(g, "id");
    ray_op_t* val_col = ray_scan(g, "val");
    ray_op_t* one = ray_const_i64(g, 1);
    ray_op_t* fifty = ray_const_i64(g, 50);
    ray_op_t* pred1 = ray_gt(g, id_col, one);
    ray_op_t* filt1 = ray_filter(g, tbl_op, pred1);
    ray_op_t* pred2 = ray_lt(g, val_col, fifty);
    ray_op_t* filt2 = ray_filter(g, filt1, pred2);

    ray_t* result = ray_execute(g, filt2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* HAVING fusion: GROUP BY id, SUM(val) → FILTER(GROUP, sum>40).
 * Groups: {1:30, 2:70, 3:50}. HAVING >40 → {2:70, 3:50} → nrows==2. */
static test_result_t test_having_fusion(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_op_t* forty = ray_const_i64(g, 40);
    /* After GROUP BY, SUM(val) column is named "val_sum" */
    ray_op_t* sum_scan = ray_scan(g, "val_sum");
    ray_op_t* pred = ray_gt(g, sum_scan, forty);
    ray_op_t* having = ray_filter(g, grp, pred);

    ray_t* result = ray_execute(g, having);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* HAVING with selection: 10-row table + selection + GROUP + HAVING.
 * Selection: mask=[0,0,1,1,1,1,0,1,1,0] → rows 2..5,7,8.
 * id=[2,2,3,3,2,3], val=[30,40,50,60,80,90].
 * GROUP BY id, SUM(val) → {2: 30+40+80=150, 3: 50+60+90=200}.
 * HAVING sum>160 → {3:200} → nrows==1. */
static test_result_t test_having_with_selection(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table_10();

    ray_graph_t* g = ray_graph_new(tbl);

    uint8_t mask[] = {0, 0, 1, 1, 1, 1, 0, 1, 1, 0};
    ray_t* sel = make_selection(mask, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ray_retain(sel);
    g->selection = sel;

    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_op_t* threshold = ray_const_i64(g, 160);
    /* After GROUP BY, SUM(val) column is named "val_sum" */
    ray_op_t* sum_scan = ray_scan(g, "val_sum");
    ray_op_t* pred = ray_gt(g, sum_scan, threshold);
    ray_op_t* having = ray_filter(g, grp, pred);

    ray_t* result = ray_execute(g, having);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 1);

    ray_release(result);
    ray_release(sel);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* -----------------------------------------------------------------------
 * Category 2: GROUP BY fallback path tests (no selection bitmap)
 * ----------------------------------------------------------------------- */

/* Small table GROUP BY (sequential path). 3 groups, total sum = 150. */
static test_result_t test_group_small(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    /* Verify total sum across all groups is 150 */
    ray_t* sum_col = ray_table_get_col_idx(result, 1);
    int64_t total = 0;
    for (int64_t i = 0; i < 3; i++) {
        total += ((int64_t*)ray_data(sum_col))[i];
    }
    TEST_ASSERT_EQ_I(total, 150);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* GROUP BY with 4 aggregates: SUM, COUNT, MIN, MAX. ncols==5, nrows==3. */
static test_result_t test_group_multi_agg(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* v1 = ray_scan(g, "val");
    ray_op_t* v2 = ray_scan(g, "val");
    ray_op_t* v3 = ray_scan(g, "val");
    ray_op_t* v4 = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { v1, v2, v3, v4 };
    uint16_t agg_ops[] = { OP_SUM, OP_COUNT, OP_MIN, OP_MAX };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 4);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 5);  /* id + 4 aggs */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* GROUP BY val (each row unique group). COUNT(val). nrows==5. */
static test_result_t test_group_single_row_groups(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* val_key = ray_scan(g, "val");
    ray_op_t* val_agg = ray_scan(g, "val");
    ray_op_t* keys[] = { val_key };
    ray_op_t* agg_ins[] = { val_agg };
    uint16_t agg_ops[] = { OP_COUNT };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_t* result = ray_execute(g, grp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 5);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* -----------------------------------------------------------------------
 * Category 2: SORT / JOIN / FILTER fallback tests (no selection bitmap)
 * ----------------------------------------------------------------------- */

/* Multi-column sort: SORT BY id DESC, val ASC.
 * Expected: ids=[3,2,2,1,1], within id=2 vals=[30,40], within id=1 vals=[10,20]. */
static test_result_t test_sort_small(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* k1 = ray_scan(g, "id");
    ray_op_t* k2 = ray_scan(g, "val");
    ray_op_t* keys[] = { k1, k2 };
    uint8_t descs[] = { 1, 0 };        /* id DESC, val ASC */
    uint8_t nulls_first[] = { 0, 0 };
    ray_op_t* sort_op = ray_sort_op(g, tbl_op, keys, descs, nulls_first, 2);

    ray_t* result = ray_execute(g, sort_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 5);

    int64_t name_id  = ray_sym_intern("id", 2);
    int64_t name_val = ray_sym_intern("val", 3);
    ray_t* id_col  = ray_table_get_col(result, name_id);
    ray_t* val_col = ray_table_get_col(result, name_val);
    int64_t* ids  = (int64_t*)ray_data(id_col);
    int64_t* vals = (int64_t*)ray_data(val_col);

    /* ids should be [3,2,2,1,1] */
    TEST_ASSERT_EQ_I(ids[0], 3);
    TEST_ASSERT_EQ_I(ids[1], 2);
    TEST_ASSERT_EQ_I(ids[2], 2);
    TEST_ASSERT_EQ_I(ids[3], 1);
    TEST_ASSERT_EQ_I(ids[4], 1);

    /* within id=2: vals=[30,40] (ASC) */
    TEST_ASSERT_EQ_I(vals[1], 30);
    TEST_ASSERT_EQ_I(vals[2], 40);

    /* within id=1: vals=[10,20] (ASC) */
    TEST_ASSERT_EQ_I(vals[3], 10);
    TEST_ASSERT_EQ_I(vals[4], 20);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* INNER JOIN: Left=make_audit_table(), Right: id=[1,2], x=[100,200].
 * id=1 matches 2 left rows, id=2 matches 2 left rows → 4 result rows. */
static test_result_t test_join_small(void) {
    ray_heap_init();
    ray_t* left = make_audit_table();

    /* Build right table: id=[1,2], x=[100,200] */
    int64_t rid_data[] = {1, 2};
    int64_t rx_data[]  = {100, 200};
    ray_t* rid_vec = ray_vec_from_raw(RAY_I64, rid_data, 2);
    ray_t* rx_vec  = ray_vec_from_raw(RAY_I64, rx_data, 2);
    int64_t name_id = ray_sym_intern("id", 2);
    int64_t name_x  = ray_sym_intern("x", 1);
    ray_t* right = ray_table_new(2);
    right = ray_table_add_col(right, name_id, rid_vec);
    right = ray_table_add_col(right, name_x, rx_vec);
    ray_release(rid_vec);
    ray_release(rx_vec);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op  = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* lk = ray_scan(g, "id");
    ray_op_t* lk_arr[] = { lk };
    ray_op_t* rk_arr[] = { lk };
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    ray_t* result = ray_execute(g, join_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    ray_release(result);
    ray_release(right);
    ray_graph_free(g);
    ray_release(left);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Filter on vector input (eager path). val > 30 → 2 rows (40, 50). */
static test_result_t test_filter_eager(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* threshold = ray_const_i64(g, 30);
    ray_op_t* pred = ray_gt(g, val, threshold);
    ray_op_t* filtered = ray_filter(g, val, pred);
    ray_op_t* cnt = ray_count(g, filtered);

    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* -----------------------------------------------------------------------
 * Category 3: Error path edge cases (empty tables)
 * ----------------------------------------------------------------------- */

/* Empty table (0 rows): GROUP BY id, SUM(val) → should not crash. */
static test_result_t test_group_empty_table(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* id_vec  = ray_vec_new(RAY_I64, 0);
    ray_t* val_vec = ray_vec_new(RAY_I64, 0);
    int64_t name_id  = ray_sym_intern("id", 2);
    int64_t name_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name_id, id_vec);
    tbl = ray_table_add_col(tbl, name_val, val_vec);
    ray_release(id_vec);
    ray_release(val_vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_t* result = ray_execute(g, grp);
    /* Accept either empty result or error — just must not crash */
    if (result && !RAY_IS_ERR(result)) {
        ray_release(result);
    }

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Empty 1-column table. SORT by val ASC → 0-row result, no crash. */
static test_result_t test_sort_empty_table(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* val_vec = ray_vec_new(RAY_I64, 0);
    int64_t name_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_val, val_vec);
    ray_release(val_vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* val_key = ray_scan(g, "val");
    ray_op_t* keys[] = { val_key };
    uint8_t descs[] = { 0 };
    uint8_t nulls_first[] = { 0 };
    ray_op_t* sort_op = ray_sort_op(g, tbl_op, keys, descs, nulls_first, 1);

    ray_t* result = ray_execute(g, sort_op);
    /* Accept either empty result or error — just must not crash */
    if (result && !RAY_IS_ERR(result)) {
        TEST_ASSERT_EQ_I(ray_table_nrows(result), 0);
        ray_release(result);
    }

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Left: make_audit_table(). Right: empty table (0 rows with id column).
 * INNER JOIN → 0 result rows. */
static test_result_t test_join_empty_table(void) {
    ray_heap_init();
    ray_t* left = make_audit_table();

    /* Build empty right table with "id" column */
    ray_t* rid_vec = ray_vec_new(RAY_I64, 0);
    int64_t name_id = ray_sym_intern("id", 2);
    ray_t* right = ray_table_new(1);
    right = ray_table_add_col(right, name_id, rid_vec);
    ray_release(rid_vec);

    ray_graph_t* g = ray_graph_new(left);
    ray_op_t* left_op  = ray_const_table(g, left);
    ray_op_t* right_op = ray_const_table(g, right);
    ray_op_t* lk = ray_scan(g, "id");
    ray_op_t* lk_arr[] = { lk };
    ray_op_t* rk_arr[] = { lk };
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    ray_t* result = ray_execute(g, join_op);
    /* Accept either empty result or error — just must not crash */
    if (result && !RAY_IS_ERR(result)) {
        TEST_ASSERT_EQ_I(ray_table_nrows(result), 0);
        ray_release(result);
    }

    ray_release(right);
    ray_graph_free(g);
    ray_release(left);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* -----------------------------------------------------------------------
 * Category 4: Optimizer correctness tests
 * ----------------------------------------------------------------------- */

/* Integer division by zero: val / 0 → each element should produce NaN (F64 null)
 * because ray_div forces F64 output, and F64 div-by-zero returns NaN.
 * SUM of all-NaN = NaN. */
static test_result_t test_const_fold_div_zero(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* zero = ray_const_i64(g, 0);
    ray_op_t* div_op = ray_div(g, val, zero);
    ray_op_t* s = ray_sum(g, div_op);

    ray_t* result = ray_execute(g, s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(isnan(result->f64));

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* INT64_MIN / -1 overflow: should not trap, accept any result. */
static test_result_t test_const_fold_int_overflow(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t big[] = { INT64_MIN };
    ray_t* v = ray_vec_from_raw(RAY_I64, big, 1);
    int64_t name_x = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_x, v);
    ray_release(v);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "x");
    ray_op_t* neg1 = ray_const_i64(g, -1);
    ray_op_t* div_op = ray_div(g, x, neg1);
    ray_op_t* s = ray_sum(g, div_op);

    ray_t* result = ray_execute(g, s);
    /* Just verify no crash/trap — accept any result */
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Predicate pushdown must NOT push filter past GROUP BY.
 * GROUP BY id, SUM(val) → FILTER(GROUP, sum_col > 40).
 * Groups: {1:30, 2:70, 3:50}. After filter >40: {2:70, 3:50} → nrows==2. */
static test_result_t test_predicate_pushdown_group(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_op_t* forty = ray_const_i64(g, 40);
    /* After GROUP BY, SUM(val) column is named "val_sum" */
    ray_op_t* sum_scan = ray_scan(g, "val_sum");
    ray_op_t* pred = ray_gt(g, sum_scan, forty);
    ray_op_t* having = ray_filter(g, grp, pred);

    ray_t* result = ray_execute(g, having);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* DCE must preserve GROUP key and agg_in SCAN nodes.
 * Build GROUP BY, optimize, then execute optimized plan.
 * If DCE kills needed nodes, execution crashes. */
static test_result_t test_dce_preserves_group_keys(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* key = ray_scan(g, "id");
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* keys[] = { key };
    ray_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    ray_op_t* opt = ray_optimize(g, grp);
    TEST_ASSERT_NOT_NULL(opt);

    ray_t* result = ray_execute(g, opt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Filter reorder correctness: two chained filters.
 * FILTER(FILTER(table, id>1), val<50). COUNT result.
 * id>1: rows (2,30),(2,40),(3,50). val<50: (2,30),(2,40) → count==2. */
static test_result_t test_filter_reorder_correctness(void) {
    ray_heap_init();
    ray_t* tbl = make_audit_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* id1 = ray_scan(g, "id");
    ray_op_t* one = ray_const_i64(g, 1);
    ray_op_t* pred1 = ray_gt(g, id1, one);
    ray_op_t* f1 = ray_filter(g, tbl_op, pred1);

    ray_op_t* val1 = ray_scan(g, "val");
    ray_op_t* fifty = ray_const_i64(g, 50);
    ray_op_t* pred2 = ray_lt(g, val1, fifty);
    ray_op_t* f2 = ray_filter(g, f1, pred2);

    ray_op_t* cnt = ray_count(g, f2);

    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* -----------------------------------------------------------------------
 * OP_WCO_JOIN selection audit findings
 * -----------------------------------------------------------------------
 *
 * FINDING: OP_WCO_JOIN does not check or compact g->selection before
 * calling exec_wco_join(). Analysis shows this is SAFE for the following
 * reasons:
 *
 * 1. exec_wco_join() operates entirely on CSR relation structures
 *    (ray_rel_t) via Leapfrog Triejoin (LFTJ). It does NOT read from
 *    g->table or scan columns from it. The g->selection bitmap is
 *    designed to filter rows of g->table, which is irrelevant here.
 *
 * 2. ray_wco_join() creates nodes with arity=0 (no table inputs).
 *    It takes pre-built CSR rels directly, so there is no input table
 *    whose rows could be filtered by g->selection.
 *
 * 3. OP_EXPAND (the typical predecessor in graph query pipelines) does
 *    NOT set or clear g->selection. Instead, it uses its own SIP bitmap
 *    mechanism (ext->graph.sip_sel) for source-side filtering. The two
 *    filtering mechanisms are independent.
 *
 * 4. The final catch-all compaction in ray_execute() (exec.c ~line 12501)
 *    would apply g->selection to WCO_JOIN's result table if selection
 *    were non-NULL. In practice, g->selection is always NULL in graph
 *    query contexts because:
 *    (a) Graph queries are typically built fresh with ray_graph_new(),
 *        which initializes selection to NULL.
 *    (b) No graph-path opcode (EXPAND, VAR_EXPAND, SHORTEST_PATH,
 *        WCO_JOIN) sets g->selection.
 *
 * 5. Adding compaction before exec_wco_join would be WRONG because the
 *    selection bitmap length is based on g->table->nrows, which has no
 *    relation to the WCO_JOIN output rows (determined by CSR topology).
 *
 * STATUS: SAFE — no fix needed.
 * ----------------------------------------------------------------------- */

/* -----------------------------------------------------------------------
 * Task 12: Detailed exec_group / exec_join / sel_compact memory audit
 * -----------------------------------------------------------------------
 *
 * EXEC_GROUP (lines 5712-7049) — scratch_alloc / scratch_calloc sites:
 *
 * 1. sc_hdr (line 5966): scratch_calloc for scalar accumulator array.
 *    Freed: alloc_ok=false (6007), OOM (6100), normal (6110).
 *    STATUS: SAFE
 *
 * 2. Per-worker sc_acc[w].{_h_sum,_h_min,_h_max,_h_sumsq,_h_count}
 *    (lines 5974-6003): freed via da_accum_free() on alloc_ok=false
 *    (6006), OOM (6100), normal (6110).
 *    STATUS: SAFE
 *
 * 3. accums_hdr (line 6275): scratch_calloc for DA accumulator array.
 *    Freed: alloc_ok=false (6318), OOM (6507->FIXED), normal (6567).
 *    STATUS: SAFE
 *
 * 4. Per-worker accums[w].{_h_sum,_h_min,_h_max,_h_sumsq,_h_count}
 *    (lines 6282-6313): freed via da_accum_free() on alloc_ok=false
 *    (6316-6317), normal (6491-6492 for w>0, 6567 for w==0).
 *    STATUS: SAFE
 *
 * 5. Dense compaction arrays _h_dsum, _h_dmin, _h_dmax, _h_dsq, _h_dcnt
 *    (lines 6536-6540): freed at 6563-6565. Only reachable after
 *    ray_table_new succeeds, so OOM before these is not an issue.
 *    STATUS: SAFE
 *
 * 6. radix_bufs_hdr (line 6616): scratch_calloc for radix partition bufs.
 *    Freed: OOM fallback (6664,6674), cancel (7034), cleanup (7034).
 *    STATUS: SAFE
 *
 * 7. Per-buf radix_bufs[i]._hdr (line 6635): freed in cleanup (7034),
 *    OOM fallback (6663,6674), cancel (6407-6410).
 *    STATUS: SAFE
 *
 * 8. part_hts_hdr (line 6671): scratch_calloc for partition hash tables.
 *    Freed: cleanup (7041).
 *    STATUS: SAFE
 *
 * 9. name_dyn_hdr (line 6817): scratch_alloc for long column names.
 *    Freed: immediately after use (6828).
 *    STATUS: SAFE
 *
 * EXEC_GROUP — ray_retain / ray_release pairs:
 *
 * 10. key_owned[k] / key_vecs[k] (lines 5830-5831): expression keys
 *     retained via exec_node. Released: broadcast OOM (5909-5911),
 *     scalar exit (6113-6114), DA exit (6570-6571), cleanup (7045-7046).
 *     STATUS: SAFE (after fix)
 *
 * 11. agg_owned[a] / agg_vecs[a] (lines 5877,5887): expression aggs.
 *     Released: broadcast OOM (5906-5908), scalar exit (6111-6112),
 *     DA exit (6568-6569), cleanup (7043-7044).
 *     STATUS: SAFE (after fix)
 *
 * 12. src_col retain/release (lines 5769-5771): factorized shortcut.
 *     Released immediately after ray_table_add_col.
 *     STATUS: SAFE
 *
 * 13. cnt_col retain/release (lines 5778,5786): factorized shortcut.
 *     Released immediately after ray_table_add_col. On error, result
 *     is released (5788-5789).
 *     STATUS: SAFE
 *
 * BUGS FOUND AND FIXED:
 *
 * BUG A (line ~6099): Scalar path — ray_table_new() failure returned
 *   directly without freeing agg_owned[]/key_owned[] expression vectors.
 *   FIX: Added agg_owned/key_owned cleanup before return.
 *
 * BUG B (line ~6510): DA path — ray_table_new() failure returned
 *   directly without freeing agg_owned[]/key_owned[] expression vectors.
 *   FIX: Added agg_owned/key_owned cleanup before return.
 *
 * BUG C (line ~6842): Sequential fallback — group_ht_init() failure
 *   returned directly without freeing agg_owned[]/key_owned[].
 *   FIX: Changed bare return to `goto cleanup` pattern.
 *
 * EXEC_JOIN (lines 8300-8911) — allocation sites:
 *
 * 14. r_hash_hdr, l_hash_hdr (lines 8356-8360): scratch_alloc for
 *     hash arrays. Freed: OOM (8362-8363), after partition (8388-8389).
 *     STATUS: SAFE
 *
 * 15. r_parts_hdr, l_parts_hdr (lines 8382-8387): partition arrays.
 *     Freed: OOM (8392-8401), cancel (8410), normal (8495-8496).
 *     STATUS: SAFE
 *
 * 16. matched_right_hdr (line 8416): scratch_calloc for FULL OUTER.
 *     Freed: OOM (8424,8445), cancel (8484), cleanup (8906).
 *     STATUS: SAFE
 *
 * 17. pcounts_hdr, pp_meta_hdr (lines 8431-8436): per-partition
 *     count/metadata. Freed: OOM (8438-8439), cancel (8483),
 *     normal (8561-8562).
 *     STATUS: SAFE
 *
 * 18. l_idx_hdr, r_idx_hdr (lines 8511-8512, 8719-8720): output pair
 *     arrays. Freed: OOM (8514-8515), cleanup (8903-8904).
 *     STATUS: SAFE
 *
 * 19. ht_next_hdr, ht_heads_hdr (lines 8574-8577): chained HT arrays.
 *     Freed: OOM (8579), cleanup (8901-8902).
 *     STATUS: SAFE
 *
 * 20. counts_hdr (line 8665): morsel count array.
 *     Freed: OOM (8668 implicit in join_cleanup), cleanup (8905).
 *     STATUS: SAFE
 *
 * 21. sjoin_sel, asp_sel (lines 8601,8649): ray_sel_new for semijoin.
 *     Released: cleanup (8907-8908).
 *     STATUS: SAFE
 *
 * SEL_COMPACT (lines 2328-2480) — allocation sites:
 *
 * 22. idx_hdr (line 2365): scratch_alloc for match_idx.
 *     Freed: OOM in ray_table_new (2393), normal (2478).
 *     On scratch_alloc failure: returns tbl with ray_retain (2367), safe.
 *     STATUS: SAFE
 *
 * 23. new_cols[c] via ray_vec_new (line 2417): output column vectors.
 *     Released: ray_release after ray_table_add_col (2475). On failure,
 *     new_cols[c]=NULL and skipped.
 *     STATUS: SAFE
 *
 * SUMMARY: 3 bugs found (all in exec_group), all fixed.
 *   - All three were missing agg_owned[]/key_owned[] cleanup on
 *     OOM-triggered early returns from the scalar, DA, and sequential
 *     fallback paths.
 *   - exec_join: all paths SAFE (uses join_cleanup label consistently).
 *   - sel_compact: all paths SAFE (single allocation, single free).
 * ----------------------------------------------------------------------- */

/* -----------------------------------------------------------------------
 * Test array + suite registration
 * ----------------------------------------------------------------------- */

const test_entry_t audit_entries[] = {
    { "audit/smoke", test_audit_smoke, NULL, NULL },
    { "audit/sel_group_sum", test_sel_group_sum, NULL, NULL },
    { "audit/sel_group_count", test_sel_group_count, NULL, NULL },
    { "audit/sel_group_scalar", test_sel_group_scalar, NULL, NULL },
    { "audit/sel_sort", test_sel_sort, NULL, NULL },
    { "audit/sel_group_sort", test_sel_group_sort, NULL, NULL },
    { "audit/sel_join", test_sel_join, NULL, NULL },
    { "audit/sel_window", test_sel_window, NULL, NULL },
    { "audit/sel_asof_join", test_sel_asof_join, NULL, NULL },
    { "audit/sel_chained", test_sel_chained, NULL, NULL },
    { "audit/sel_all_filtered", test_sel_all_filtered, NULL, NULL },
    { "audit/sel_none_filtered", test_sel_none_filtered, NULL, NULL },
    { "audit/filter_lazy", test_filter_lazy, NULL, NULL },
    { "audit/filter_lazy_chained", test_filter_lazy_chained, NULL, NULL },
    { "audit/having_fusion", test_having_fusion, NULL, NULL },
    { "audit/having_with_selection", test_having_with_selection, NULL, NULL },
    { "audit/group_small", test_group_small, NULL, NULL },
    { "audit/group_multi_agg", test_group_multi_agg, NULL, NULL },
    { "audit/group_single_row_groups", test_group_single_row_groups, NULL, NULL },
    { "audit/sort_small", test_sort_small, NULL, NULL },
    { "audit/join_small", test_join_small, NULL, NULL },
    { "audit/filter_eager", test_filter_eager, NULL, NULL },
    { "audit/group_empty_table", test_group_empty_table, NULL, NULL },
    { "audit/sort_empty_table", test_sort_empty_table, NULL, NULL },
    { "audit/join_empty_table", test_join_empty_table, NULL, NULL },
    { "audit/const_fold_div_zero", test_const_fold_div_zero, NULL, NULL },
    { "audit/const_fold_int_overflow", test_const_fold_int_overflow, NULL, NULL },
    { "audit/predicate_pushdown_group", test_predicate_pushdown_group, NULL, NULL },
    { "audit/dce_preserves_group_keys", test_dce_preserves_group_keys, NULL, NULL },
    { "audit/filter_reorder_correctness", test_filter_reorder_correctness, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


