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
#include "store/csr.h"
#include <string.h>

/* Helper: create a test table with columns id1(I64), v1(I64), v3(F64) */
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

/*
 * Test: filter with AND-combined predicates in "wrong" order.
 *
 * DAG: FILTER(AND(id1_eq, v3_gt), SCAN(v1))
 *   - id1_eq is cheap (I64 eq const) but listed first
 *   - v3_gt is expensive (F64 range cmp) but listed second
 *   - Optimizer should later split AND and reorder so cheap is innermost
 *
 * Baseline correctness: id1=1 AND v3>5.0 → count=2
 */
static test_result_t test_filter_reorder_by_type(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1    = ray_scan(g, "v1");
    ray_op_t* id1   = ray_scan(g, "id1");
    ray_op_t* v3    = ray_scan(g, "v3");
    ray_op_t* c1    = ray_const_i64(g, 1);
    ray_op_t* c5    = ray_const_f64(g, 5.0);

    ray_op_t* id1_eq = ray_eq(g, id1, c1);    /* cheap: const cmp + eq */
    ray_op_t* v3_gt  = ray_gt(g, v3, c5);     /* more expensive: range */

    /* AND with "wrong" order: cheap pred first, expensive second */
    ray_op_t* combined = ray_and(g, id1_eq, v3_gt);
    ray_op_t* filt = ray_filter(g, v1, combined);
    ray_op_t* cnt = ray_count(g, filt);

    /* Execute and verify correctness: id1=1 AND v3>5.0
     * Rows: id1={1,1,2,2,3,3,1,2,3,1}, v3={1.5,2.5,...,10.5}
     * id1=1 rows: indices 0,1,6,9 → v3={1.5,2.5,7.5,10.5}
     * v3>5.0 from those: indices 6,9 → count=2 */
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

/*
 * Test: AND(pred_a, pred_b) in a single filter gets split into
 * two chained filters for independent reordering.
 *
 * DAG: FILTER(AND(v3 > 5.0, id1 = 1), SCAN(v1))
 * After: FILTER(v3 > 5.0, FILTER(id1 = 1, SCAN(v1)))
 * Verify via correctness — same result as test above.
 */
static test_result_t test_filter_and_split(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1    = ray_scan(g, "v1");
    ray_op_t* id1   = ray_scan(g, "id1");
    ray_op_t* v3    = ray_scan(g, "v3");
    ray_op_t* c1    = ray_const_i64(g, 1);
    ray_op_t* c5    = ray_const_f64(g, 5.0);

    ray_op_t* id1_eq = ray_eq(g, id1, c1);
    ray_op_t* v3_gt  = ray_gt(g, v3, c5);
    ray_op_t* combined = ray_and(g, v3_gt, id1_eq);

    ray_op_t* filt = ray_filter(g, v1, combined);
    ray_op_t* cnt = ray_count(g, filt);

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

/*
 * Test: verify that after optimization, the inner filter (closest to scan)
 * has the cheaper predicate.
 *
 * Build: FILTER(eq_on_i64, FILTER(gt_on_f64, SCAN))
 * eq_on_i64 costs: const(+0) + i64_width(+3) + eq(+0) = 3
 * gt_on_f64 costs: const(+0) + f64_width(+3) + range(+2) = 5
 *
 * eq is cheaper, so after reorder the chain should be:
 *   FILTER(gt_on_f64, FILTER(eq_on_i64, SCAN))
 * i.e., outer predicate = gt_on_f64, inner predicate = eq_on_i64
 */
static test_result_t test_filter_reorder_dag(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1     = ray_scan(g, "v1");
    ray_op_t* id1    = ray_scan(g, "id1");
    ray_op_t* v3     = ray_scan(g, "v3");
    ray_op_t* c1     = ray_const_i64(g, 1);
    ray_op_t* c5     = ray_const_f64(g, 5.0);

    ray_op_t* eq_pred = ray_eq(g, id1, c1);     /* cost=3: const+i64+eq */
    ray_op_t* gt_pred = ray_gt(g, v3, c5);      /* cost=5: const+f64+gt */

    /* Build in WRONG order: cheap eq is outer, expensive gt is inner */
    ray_op_t* filt_inner = ray_filter(g, v1, gt_pred);
    ray_op_t* filt_outer = ray_filter(g, filt_inner, eq_pred);

    uint32_t eq_pred_id = eq_pred->id;
    uint32_t gt_pred_id = gt_pred->id;

    ray_op_t* opt = ray_optimize(g, filt_outer);
    TEST_ASSERT_NOT_NULL(opt);

    /* After reorder: outer should have gt (expensive), inner should have eq (cheap).
     * The pass swaps predicates, so:
     *   chain[0] (outer) gets the higher cost pred
     *   chain[1] (inner) gets the lower cost pred */
    TEST_ASSERT_EQ_I(opt->opcode, OP_FILTER);
    ray_op_t* inner = opt->inputs[0];
    TEST_ASSERT_EQ_I(inner->opcode, OP_FILTER);

    /* Inner pred should be eq (cheaper), outer pred should be gt (more expensive) */
    TEST_ASSERT_EQ_I(inner->inputs[1]->id, eq_pred_id);
    TEST_ASSERT_EQ_I(opt->inputs[1]->id, gt_pred_id);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: predicate pushdown past projection.
 *
 * Build: FILTER(id1 = 1, PROJECT([id1, v1], SCAN))
 * After pushdown: PROJECT([id1, v1], FILTER(id1 = 1, SCAN))
 *
 * Verify both correctness and DAG structure.
 * id1=1 rows: indices 0,1,6,9 → count=4
 */
static test_result_t test_pushdown_past_select(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* Build: FILTER(pred, SELECT([id1, v1], SCAN)) */
    ray_op_t* v1   = ray_scan(g, "v1");
    ray_op_t* id1  = ray_scan(g, "id1");
    ray_op_t* c1   = ray_const_i64(g, 1);
    ray_op_t* pred = ray_eq(g, id1, c1);

    ray_op_t* sel_cols[] = { id1, v1 };
    ray_op_t* sel = ray_select(g, v1, sel_cols, 2);
    uint32_t sel_id = sel->id;
    ray_op_t* filt = ray_filter(g, sel, pred);

    /* Optimize and capture the new root (pushdown moves filter below select) */
    ray_op_t* opt_root = ray_optimize(g, filt);

    /* Verify DAG structure: filter should have been pushed below select */
    ray_op_t* sel_after = &g->nodes[sel_id];
    TEST_ASSERT_EQ_I(sel_after->opcode, OP_SELECT);
    TEST_ASSERT_EQ_I(sel_after->inputs[0]->opcode, OP_FILTER);

    /* Verify the optimized root is the select node (filter was pushed below) */
    TEST_ASSERT_EQ_U(opt_root->id, sel_id);

    /* Execute COUNT from the pushed-down filter to validate correctness.
     * The filter (now below select) should still produce the right row count. */
    ray_op_t* cnt = ray_count(g, sel_after->inputs[0]);
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 4);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: FILTER above GROUP correctness (GROUP pushdown is disabled).
 *
 * Build: FILTER(id1 = 1, GROUP(id1, SUM(v1)))
 * After: unchanged (GROUP pushdown disabled — executor key/agg scans
 *        bypass filter, so pushdown would produce wrong results).
 *
 * Verify correctness of filter-above-group execution. Result: id1=1, sum=200
 */
static test_result_t test_pushdown_past_group(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* key = ray_scan(g, "id1");
    ray_op_t* val = ray_scan(g, "v1");
    ray_op_t* keys[] = { key };
    uint16_t agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { val };
    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);

    /* Filter on the group key column (id1 = 1).
     * GROUP pushdown is disabled (executor key/agg scans bypass filter),
     * so this tests FILTER-above-GROUP correctness only. */
    ray_op_t* id1_scan = ray_scan(g, "id1");
    ray_op_t* c1 = ray_const_i64(g, 1);
    ray_op_t* pred = ray_eq(g, id1_scan, c1);
    ray_op_t* filt = ray_filter(g, grp, pred);

    /* Verify correctness */
    ray_t* result = ray_execute(g, filt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 1);
    ray_t* sum_col = ray_table_get_col_idx(result, 1);
    TEST_ASSERT_NOT_NULL(sum_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 200);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: projection pushdown marks unreachable SCAN nodes dead.
 *
 * Build: SUM(SCAN("v1")) — only v1 is referenced.
 * After optimization, id1 and v3 scans are not in the DAG,
 * so they should not affect the result.
 * sum(v1) = 10+20+30+40+50+60+70+80+90+100 = 550
 */
static test_result_t test_projection_pushdown(void) {
    ray_heap_init();
    ray_t *tbl = make_test_table();
    ray_graph_t *g = ray_graph_new(tbl);

    /* Only reference v1 — id1 and v3 should not affect result */
    ray_op_t *v1 = ray_scan(g, "v1");
    ray_op_t *s = ray_sum(g, v1);
    ray_op_t *opt = ray_optimize(g, s);

    ray_t *result = ray_execute(g, opt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 550);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning smoke test.
 *
 * Build: SUM(FILTER(EQ(SCAN(id1), CONST(1)), SCAN(v1)))
 * Verify correctness: id1=1 rows: indices 0,1,6,9 → v1={10,20,70,100} → sum=200
 *
 * The partition pruning pass only activates for RAY_MAPCOMMON columns,
 * so with regular I64 columns this verifies the pass is a safe no-op.
 */
static test_result_t test_partition_pruning_smoke(void) {
    ray_heap_init();
    ray_t *tbl = make_test_table();
    ray_graph_t *g = ray_graph_new(tbl);

    ray_op_t *id1 = ray_scan(g, "id1");
    ray_op_t *v1 = ray_scan(g, "v1");
    ray_op_t *c1 = ray_const_i64(g, 1);
    ray_op_t *pred = ray_eq(g, id1, c1);
    ray_op_t *flt = ray_filter(g, v1, pred);
    ray_op_t *s = ray_sum(g, flt);

    ray_op_t *opt = ray_optimize(g, s);
    ray_t *result = ray_execute(g, opt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    /* id1=1 rows: v1={10,20,70,100} -> sum=200 */
    TEST_ASSERT_EQ_I(result->i64, 200);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning produces correct seg_mask bitmap.
 *
 * Build a parted table with 4 partitions keyed by I64 values [100, 200, 300, 400].
 * Filter: pkey >= 300.  Expected: bits 2,3 set (300,400), bits 0,1 clear (100,200).
 */
static test_result_t test_partition_pruning_mask(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build MAPCOMMON column with 4 I64 partition keys */
    ray_t* key_values = ray_vec_new(RAY_I64, 4);
    TEST_ASSERT_NOT_NULL(key_values);
    key_values->len = 4;
    int64_t keys[] = {100, 200, 300, 400};
    memcpy(ray_data(key_values), keys, sizeof(keys));

    ray_t* row_counts = ray_vec_new(RAY_I64, 4);
    TEST_ASSERT_NOT_NULL(row_counts);
    row_counts->len = 4;
    int64_t counts[] = {5, 5, 5, 5};
    memcpy(ray_data(row_counts), counts, sizeof(counts));

    /* MAPCOMMON: stores [key_values, row_counts] as pointer array */
    ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(mapcommon);
    mapcommon->type = RAY_MAPCOMMON;
    mapcommon->len = 2; /* 2 pointers */
    ((ray_t**)ray_data(mapcommon))[0] = key_values;
    ((ray_t**)ray_data(mapcommon))[1] = row_counts;

    /* Build 4 segments for data column */
    ray_t* segs[4];
    for (int i = 0; i < 4; i++) {
        segs[i] = ray_vec_new(RAY_I64, 5);
        TEST_ASSERT_NOT_NULL(segs[i]);
        segs[i]->len = 5;
        int64_t* d = (int64_t*)ray_data(segs[i]);
        for (int j = 0; j < 5; j++) d[j] = (i + 1) * 10 + j;
    }

    /* Build parted data column */
    ray_t* val_parted = ray_alloc(4 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(val_parted);
    val_parted->type = RAY_PARTED_BASE + RAY_I64;
    val_parted->len = 4;
    for (int i = 0; i < 4; i++)
        ((ray_t**)ray_data(val_parted))[i] = segs[i];

    /* Build table: pkey (MAPCOMMON), val (parted I64) */
    int64_t sym_pkey = ray_sym_intern("pkey", 4);
    int64_t sym_val  = ray_sym_intern("val", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pkey, mapcommon);
    tbl = ray_table_add_col(tbl, sym_val, val_parted);

    /* Build DAG: FILTER(SCAN(val), GE(SCAN(pkey), CONST(300))) */
    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);

    ray_op_t* scan_val  = ray_scan(g, "val");
    ray_op_t* scan_pkey = ray_scan(g, "pkey");
    ray_op_t* c300      = ray_const_i64(g, 300);
    ray_op_t* ge_pred   = ray_ge(g, scan_pkey, c300);
    ray_op_t* filt      = ray_filter(g, scan_val, ge_pred);

    /* Optimize — should produce seg_mask */
    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    /* Find the ext node for scan_val — it should have seg_mask set */
    ray_op_ext_t* val_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == scan_val->id) {
            val_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(val_ext);
    TEST_ASSERT_NOT_NULL(val_ext->seg_mask);

    /* Verify bitmap: bits 2,3 set (keys 300,400 >= 300), bits 0,1 clear */
    uint64_t expected = (1ULL << 2) | (1ULL << 3);
    TEST_ASSERT_TRUE(val_ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(mapcommon);
    ray_release(val_parted);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning works for OP_IN predicates.
 *
 * 4 partitions keyed by I64 values [100, 200, 300, 400].
 * Filter: pkey IN [100, 300].  Expected seg_mask: bits 0,2 set.
 */
static test_result_t test_partition_pruning_in(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* key_values = ray_vec_new(RAY_I64, 4);
    key_values->len = 4;
    int64_t keys[] = {100, 200, 300, 400};
    memcpy(ray_data(key_values), keys, sizeof(keys));

    ray_t* row_counts = ray_vec_new(RAY_I64, 4);
    row_counts->len = 4;
    int64_t counts[] = {5, 5, 5, 5};
    memcpy(ray_data(row_counts), counts, sizeof(counts));

    ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
    mapcommon->type = RAY_MAPCOMMON;
    mapcommon->len = 2;
    ((ray_t**)ray_data(mapcommon))[0] = key_values;
    ((ray_t**)ray_data(mapcommon))[1] = row_counts;

    ray_t* segs[4];
    for (int i = 0; i < 4; i++) {
        segs[i] = ray_vec_new(RAY_I64, 5);
        segs[i]->len = 5;
        int64_t* d = (int64_t*)ray_data(segs[i]);
        for (int j = 0; j < 5; j++) d[j] = (i + 1) * 10 + j;
    }

    ray_t* val_parted = ray_alloc(4 * sizeof(ray_t*));
    val_parted->type = RAY_PARTED_BASE + RAY_I64;
    val_parted->len = 4;
    for (int i = 0; i < 4; i++)
        ((ray_t**)ray_data(val_parted))[i] = segs[i];

    int64_t sym_pkey = ray_sym_intern("pkey", 4);
    int64_t sym_val  = ray_sym_intern("val", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pkey, mapcommon);
    tbl = ray_table_add_col(tbl, sym_val, val_parted);

    /* Build DAG: FILTER(SCAN(val), IN(SCAN(pkey), CONST(vec [100 300]))) */
    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);

    ray_op_t* scan_val  = ray_scan(g, "val");
    ray_op_t* scan_pkey = ray_scan(g, "pkey");

    /* Build literal set vector [100, 300] */
    ray_t* set_vec = ray_vec_new(RAY_I64, 2);
    set_vec->len = 2;
    ((int64_t*)ray_data(set_vec))[0] = 100;
    ((int64_t*)ray_data(set_vec))[1] = 300;
    ray_op_t* set_const = ray_const_vec(g, set_vec);
    ray_release(set_vec);  /* graph retained it */

    ray_op_t* in_pred = ray_in(g, scan_pkey, set_const);
    ray_op_t* filt    = ray_filter(g, scan_val, in_pred);

    /* Optimize — should produce seg_mask with bits 0,2 set */
    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* val_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == scan_val->id) {
            val_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(val_ext);
    TEST_ASSERT_NOT_NULL(val_ext->seg_mask);

    /* bits 0,2 set — partitions keyed 100 and 300 are in the set */
    uint64_t expected = (1ULL << 0) | (1ULL << 2);
    TEST_ASSERT_TRUE(val_ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(mapcommon);
    ray_release(val_parted);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning must NOT fire when the key type class and
 * the literal type class differ (SYM vs int-family).  Raw bit
 * comparison across namespaces would produce random spurious
 * matches — safer to skip pruning entirely and let the per-row
 * executor filter handle it.
 *
 * Setup: SYM-keyed partitions with a literal i64 set.  After
 * optimization, the scan's seg_mask must remain unset (no pruning).
 */
static test_result_t test_partition_pruning_in_type_mismatch(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Partition keys are SYM IDs (interned). */
    int64_t s_aapl = ray_sym_intern("AAPL", 4);
    int64_t s_goog = ray_sym_intern("GOOG", 4);
    int64_t s_msft = ray_sym_intern("MSFT", 4);
    int64_t s_ibm  = ray_sym_intern("IBM",  3);

    ray_t* key_values = ray_vec_new(RAY_SYM, 4);
    key_values->len = 4;
    int64_t* kd = (int64_t*)ray_data(key_values);
    kd[0] = s_aapl; kd[1] = s_goog; kd[2] = s_msft; kd[3] = s_ibm;

    ray_t* row_counts = ray_vec_new(RAY_I64, 4);
    row_counts->len = 4;
    int64_t counts[] = {5, 5, 5, 5};
    memcpy(ray_data(row_counts), counts, sizeof(counts));

    ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
    mapcommon->type = RAY_MAPCOMMON;
    mapcommon->len = 2;
    ((ray_t**)ray_data(mapcommon))[0] = key_values;
    ((ray_t**)ray_data(mapcommon))[1] = row_counts;

    ray_t* segs[4];
    for (int i = 0; i < 4; i++) {
        segs[i] = ray_vec_new(RAY_I64, 5);
        segs[i]->len = 5;
        int64_t* d = (int64_t*)ray_data(segs[i]);
        for (int j = 0; j < 5; j++) d[j] = (i + 1) * 10 + j;
    }

    ray_t* val_parted = ray_alloc(4 * sizeof(ray_t*));
    val_parted->type = RAY_PARTED_BASE + RAY_I64;
    val_parted->len = 4;
    for (int i = 0; i < 4; i++)
        ((ray_t**)ray_data(val_parted))[i] = segs[i];

    int64_t sym_pkey = ray_sym_intern("pkey", 4);
    int64_t sym_val  = ray_sym_intern("val", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pkey, mapcommon);
    tbl = ray_table_add_col(tbl, sym_val, val_parted);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_val  = ray_scan(g, "val");
    ray_op_t* scan_pkey = ray_scan(g, "pkey");

    /* TYPE-MISMATCH SET: sym-keyed partition, int literal set.
     * The sym IDs for AAPL/GOOG/MSFT/IBM could randomly equal 1, 2,
     * or 3 — and even if they don't, comparing sym IDs to raw ints
     * is nonsensical.  Pruning must skip this predicate entirely. */
    ray_t* set_vec = ray_vec_new(RAY_I64, 3);
    set_vec->len = 3;
    ((int64_t*)ray_data(set_vec))[0] = 1;
    ((int64_t*)ray_data(set_vec))[1] = 2;
    ((int64_t*)ray_data(set_vec))[2] = 3;
    ray_op_t* set_const = ray_const_vec(g, set_vec);
    ray_release(set_vec);

    ray_op_t* in_pred = ray_in(g, scan_pkey, set_const);
    ray_op_t* filt    = ray_filter(g, scan_val, in_pred);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* val_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == scan_val->id) {
            val_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(val_ext);
    /* Pruning must NOT have fired — seg_mask stays NULL. */
    TEST_ASSERT_EQ_PTR(val_ext->seg_mask, NULL);

    ray_graph_free(g);
    ray_release(mapcommon);
    ray_release(val_parted);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning for OP_NOT_IN is the complement.
 * pkey NOT IN [100, 300] → bits 1,3 set (keys 200 and 400).
 */
static test_result_t test_partition_pruning_not_in(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* key_values = ray_vec_new(RAY_I64, 4);
    key_values->len = 4;
    int64_t keys[] = {100, 200, 300, 400};
    memcpy(ray_data(key_values), keys, sizeof(keys));

    ray_t* row_counts = ray_vec_new(RAY_I64, 4);
    row_counts->len = 4;
    int64_t counts[] = {5, 5, 5, 5};
    memcpy(ray_data(row_counts), counts, sizeof(counts));

    ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
    mapcommon->type = RAY_MAPCOMMON;
    mapcommon->len = 2;
    ((ray_t**)ray_data(mapcommon))[0] = key_values;
    ((ray_t**)ray_data(mapcommon))[1] = row_counts;

    ray_t* segs[4];
    for (int i = 0; i < 4; i++) {
        segs[i] = ray_vec_new(RAY_I64, 5);
        segs[i]->len = 5;
        int64_t* d = (int64_t*)ray_data(segs[i]);
        for (int j = 0; j < 5; j++) d[j] = (i + 1) * 10 + j;
    }

    ray_t* val_parted = ray_alloc(4 * sizeof(ray_t*));
    val_parted->type = RAY_PARTED_BASE + RAY_I64;
    val_parted->len = 4;
    for (int i = 0; i < 4; i++)
        ((ray_t**)ray_data(val_parted))[i] = segs[i];

    int64_t sym_pkey = ray_sym_intern("pkey", 4);
    int64_t sym_val  = ray_sym_intern("val", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pkey, mapcommon);
    tbl = ray_table_add_col(tbl, sym_val, val_parted);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_val  = ray_scan(g, "val");
    ray_op_t* scan_pkey = ray_scan(g, "pkey");

    ray_t* set_vec = ray_vec_new(RAY_I64, 2);
    set_vec->len = 2;
    ((int64_t*)ray_data(set_vec))[0] = 100;
    ((int64_t*)ray_data(set_vec))[1] = 300;
    ray_op_t* set_const = ray_const_vec(g, set_vec);
    ray_release(set_vec);

    ray_op_t* nin_pred = ray_not_in(g, scan_pkey, set_const);
    ray_op_t* filt     = ray_filter(g, scan_val, nin_pred);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* val_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == scan_val->id) {
            val_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(val_ext);
    TEST_ASSERT_NOT_NULL(val_ext->seg_mask);

    uint64_t expected = (1ULL << 1) | (1ULL << 3);
    TEST_ASSERT_TRUE(val_ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(mapcommon);
    ray_release(val_parted);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: OP_WINDOW DCE & projection-pushdown walk.
 *
 * Build: SCAN(grp), SCAN(val) -> WINDOW(part=[grp], order=[val], func=ROW_NUMBER(val))
 * Run ray_optimize.  This forces projection_pushdown's BFS, mark_live's
 * DCE walk, and pass_type_inference to walk through ext->window.{part_keys,
 * order_keys, func_inputs}.  Verify all three columns survived (none
 * marked OP_FLAG_DEAD).
 */
static test_result_t test_opt_window_dce(void) {
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
    uint8_t order_descs[] = { 0 };
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
    TEST_ASSERT_NOT_NULL(win);

    uint32_t grp_id = grp_op->id;
    uint32_t val_id = val_op->id;

    ray_op_t* opt = ray_optimize(g, win);
    TEST_ASSERT_NOT_NULL(opt);
    TEST_ASSERT_EQ_I(opt->opcode, OP_WINDOW);

    /* Both scans referenced through window's ext (part_keys, order_keys,
     * func_inputs) must survive DCE/projection-pushdown. */
    TEST_ASSERT_FALSE(g->nodes[grp_id].flags & OP_FLAG_DEAD);
    TEST_ASSERT_FALSE(g->nodes[val_id].flags & OP_FLAG_DEAD);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: predicate pushdown past OP_EXPAND.
 *
 * Build: FILTER(EXPAND(SCAN_const_vec, rel), pred(SCAN_pred_const))
 * The predicate references a CONST scan in the source subtree, so the
 * pushdown must rewrite into:
 *      EXPAND(FILTER(SCAN_const_vec, pred), rel)
 *
 * Note: predicate must reference scans reachable from EXPAND's source
 * input.  We use a const-only predicate that has NO scans (n_scans==0,
 * which collect_pred_scans returns) -- no, we need n_scans > 0 for the
 * pushdown to fire.  So we build a graph with a small table that has
 * a column we can scan + a const compare.
 */
static test_result_t test_opt_pushdown_past_expand(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build node table with a column 'flag' we can filter on. */
    int64_t n = 4;
    int64_t flag_data[] = {1, 0, 1, 0};
    int64_t id_data[]   = {0, 1, 2, 3};
    ray_t* flag_v = ray_vec_from_raw(RAY_I64, flag_data, n);
    ray_t* id_v   = ray_vec_from_raw(RAY_I64, id_data, n);
    int64_t s_flag = ray_sym_intern("flag", 4);
    int64_t s_id   = ray_sym_intern("id", 2);
    ray_t* node_tbl = ray_table_new(2);
    node_tbl = ray_table_add_col(node_tbl, s_id, id_v);
    node_tbl = ray_table_add_col(node_tbl, s_flag, flag_v);
    ray_release(flag_v);
    ray_release(id_v);

    /* Edges: 0->1, 0->2, 1->3, 2->3 */
    int64_t src_data[] = {0, 0, 1, 2};
    int64_t dst_data[] = {1, 2, 3, 3};
    ray_t* src_v = ray_vec_from_raw(RAY_I64, src_data, 4);
    ray_t* dst_v = ray_vec_from_raw(RAY_I64, dst_data, 4);
    int64_t s_src = ray_sym_intern("src", 3);
    int64_t s_dst = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, s_src, src_v);
    edges = ray_table_add_col(edges, s_dst, dst_v);
    ray_release(src_v);
    ray_release(dst_v);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(node_tbl);

    /* Source: scan 'flag' (so the predicate's scan is reachable from
     * EXPAND's source subtree).  Predicate: flag = 1. */
    ray_op_t* flag_scan = ray_scan(g, "flag");
    ray_op_t* c1   = ray_const_i64(g, 1);
    ray_op_t* pred = ray_eq(g, flag_scan, c1);

    ray_op_t* expand = ray_expand(g, flag_scan, rel, 0);
    TEST_ASSERT_NOT_NULL(expand);
    uint32_t expand_id = expand->id;

    ray_op_t* filt = ray_filter(g, expand, pred);
    TEST_ASSERT_NOT_NULL(filt);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    /* After pushdown, root is the EXPAND (filter pushed below it). */
    TEST_ASSERT_EQ_U(opt->id, expand_id);
    TEST_ASSERT_EQ_I(opt->opcode, OP_EXPAND);
    /* EXPAND.inputs[0] should now be a FILTER node (the pushed filter). */
    TEST_ASSERT_NOT_NULL(opt->inputs[0]);
    TEST_ASSERT_EQ_I(opt->inputs[0]->opcode, OP_FILTER);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(node_tbl);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: predicate-pushdown past EXPAND must NOT fire when the predicate
 * references a scan that is NOT in the EXPAND's source subtree.
 *
 * We can't easily build this in C without a multi-table graph, but we can
 * still exercise collect_pred_scans's truncation/unknown path by
 * building a predicate with multiple chained scans where the source is a
 * const-vec (not a scan).  In that case is_reachable_from will return
 * false for the predicate's scan, all_source becomes false, and pushdown
 * is skipped.  This still walks collect_pred_scans + is_reachable_from.
 */
static test_result_t test_opt_pushdown_expand_blocked(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 4;
    int64_t flag_data[] = {1, 0, 1, 0};
    int64_t id_data[]   = {0, 1, 2, 3};
    ray_t* flag_v = ray_vec_from_raw(RAY_I64, flag_data, n);
    ray_t* id_v   = ray_vec_from_raw(RAY_I64, id_data, n);
    int64_t s_flag = ray_sym_intern("flag", 4);
    int64_t s_id   = ray_sym_intern("id", 2);
    ray_t* node_tbl = ray_table_new(2);
    node_tbl = ray_table_add_col(node_tbl, s_id, id_v);
    node_tbl = ray_table_add_col(node_tbl, s_flag, flag_v);
    ray_release(flag_v);
    ray_release(id_v);

    int64_t src_data[] = {0, 1, 2};
    int64_t dst_data[] = {1, 2, 3};
    ray_t* src_v = ray_vec_from_raw(RAY_I64, src_data, 3);
    ray_t* dst_v = ray_vec_from_raw(RAY_I64, dst_data, 3);
    int64_t s_src = ray_sym_intern("src", 3);
    int64_t s_dst = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, s_src, src_v);
    edges = ray_table_add_col(edges, s_dst, dst_v);
    ray_release(src_v);
    ray_release(dst_v);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(node_tbl);

    /* EXPAND's source is a const vec literal — has no SCAN.
     * Predicate references a SCAN, which is NOT in source subtree.
     * Pushdown must skip; we just verify it doesn't crash and
     * the filter remains above the expand. */
    int64_t start_data[] = {0, 1};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 2);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_release(start_vec);

    ray_op_t* expand = ray_expand(g, src, rel, 0);
    TEST_ASSERT_NOT_NULL(expand);
    uint32_t expand_id = expand->id;

    /* Predicate: flag = 1 — its SCAN is NOT in the expand source subtree. */
    ray_op_t* flag_scan = ray_scan(g, "flag");
    ray_op_t* c1 = ray_const_i64(g, 1);
    ray_op_t* pred = ray_eq(g, flag_scan, c1);

    ray_op_t* filt = ray_filter(g, expand, pred);
    uint32_t filt_id = filt->id;

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);
    /* Pushdown blocked: filter remains the root, expand stays below. */
    TEST_ASSERT_EQ_U(opt->id, filt_id);
    TEST_ASSERT_EQ_I(opt->opcode, OP_FILTER);
    TEST_ASSERT_EQ_U(opt->inputs[0]->id, expand_id);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(node_tbl);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: graph_alloc_node_opt realloc fix-up.
 *
 * Build a graph with > GRAPH_INIT_CAP (4096) nodes, then add one
 * filter with an AND predicate.  When the optimizer's split_and_filter
 * pass calls graph_alloc_node_opt to allocate the new outer-filter
 * node, the array is at capacity and must realloc + fix up all stored
 * input pointers.
 *
 * We pad the graph by allocating many ray_const_i64 nodes (cheap, each
 * adds one node) until we're just shy of cap, then build the AND filter
 * on top.  The split must produce the correct structure after realloc.
 *
 * We also include OP_GROUP / OP_SELECT / OP_SORT / OP_WINDOW nodes
 * (with structural ext->keys/columns/part_keys pointers) to exercise
 * the per-opcode fix-up branches in graph_alloc_node_opt.  Those nodes
 * are not in the optimized root's live subgraph, so DCE will mark
 * them dead — but the realloc fix-up still walks them.
 */
static test_result_t test_opt_realloc_during_split(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* Build the predicate FIRST, before we cause a realloc — we want
     * stored input pointers across the AND/EQ predicate so the fix-up
     * has something to relocate. */
    ray_op_t* v1   = ray_scan(g, "v1");
    ray_op_t* id1  = ray_scan(g, "id1");
    ray_op_t* v3   = ray_scan(g, "v3");
    ray_op_t* c1   = ray_const_i64(g, 1);
    ray_op_t* c5   = ray_const_f64(g, 5.0);
    ray_op_t* eq   = ray_eq(g, id1, c1);
    ray_op_t* gt   = ray_gt(g, v3, c5);
    ray_op_t* combined = ray_and(g, eq, gt);
    ray_op_t* filt = ray_filter(g, v1, combined);
    uint32_t filt_id = filt->id;
    uint32_t eq_id   = eq->id;
    uint32_t gt_id   = gt->id;

    /* Build extra structural ops to exercise the per-opcode fix-up
     * branches in graph_alloc_node_opt.  These are NOT in the live
     * subgraph reachable from filt — DCE will mark them dead — but
     * they exist in g->ext_nodes when realloc fires. */
    {
        /* OP_GROUP with key + agg input */
        ray_op_t* key = ray_scan(g, "id1");
        ray_op_t* val = ray_scan(g, "v1");
        ray_op_t* keys[] = { key };
        uint16_t agg_ops[] = { OP_SUM };
        ray_op_t* agg_ins[] = { val };
        (void)ray_group(g, keys, 1, agg_ops, agg_ins, 1);

        /* OP_SELECT with column array */
        ray_op_t* sel_v1 = ray_scan(g, "v1");
        ray_op_t* sel_id1 = ray_scan(g, "id1");
        ray_op_t* sel_cols[] = { sel_v1, sel_id1 };
        (void)ray_select(g, sel_v1, sel_cols, 2);

        /* OP_WINDOW with part/order/func keys */
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w_grp = ray_scan(g, "id1");
        ray_op_t* w_val = ray_scan(g, "v1");
        ray_op_t* w_parts[] = { w_grp };
        ray_op_t* w_orders[] = { w_val };
        uint8_t w_descs[] = { 0 };
        uint8_t w_kinds[] = { RAY_WIN_ROW_NUMBER };
        ray_op_t* w_ins[] = { w_val };
        int64_t w_params[] = { 0 };
        (void)ray_window_op(g, tbl_op,
                            w_parts, 1,
                            w_orders, w_descs, 1,
                            w_kinds, w_ins, w_params, 1,
                            RAY_FRAME_ROWS,
                            RAY_BOUND_UNBOUNDED_PRECEDING,
                            RAY_BOUND_UNBOUNDED_FOLLOWING,
                            0, 0);
    }

    /* Now pad the graph until we're at exactly node_cap, so the very
     * next allocation (split_and_filter's outer node) forces a realloc. */
    while (g->node_count < g->node_cap) {
        (void)ray_const_i64(g, 0);
    }
    /* node_count == node_cap now — next allocation will realloc */
    TEST_ASSERT_EQ_U(g->node_count, g->node_cap);

    uint32_t cap_before = g->node_cap;

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    /* After AND-split, the original FILTER node (filt_id) should have
     * one of the predicates (eq or gt), and a NEW outer FILTER (id ==
     * old node_count == cap_before) wraps it with the other predicate. */
    TEST_ASSERT_TRUE(g->node_cap > cap_before);  /* realloc fired */

    /* Walk from new root: must be FILTER -> FILTER -> SCAN(v1) chain,
     * with predicates pointing at eq and gt (post-fix-up). */
    TEST_ASSERT_EQ_I(opt->opcode, OP_FILTER);
    TEST_ASSERT_NOT_NULL(opt->inputs[0]);
    TEST_ASSERT_EQ_I(opt->inputs[0]->opcode, OP_FILTER);

    /* After AND-split + reorder, the inner predicate is the cheaper
     * (eq on i64) and outer predicate is gt on f64 — but here we just
     * need the predicates to point to valid live nodes (no use-after-
     * realloc).  Both pred nodes' IDs must still resolve to live ops. */
    ray_op_t* outer_pred = opt->inputs[1];
    ray_op_t* inner = opt->inputs[0];
    ray_op_t* inner_pred = inner->inputs[1];
    TEST_ASSERT_NOT_NULL(outer_pred);
    TEST_ASSERT_NOT_NULL(inner_pred);
    TEST_ASSERT_TRUE(outer_pred->id == eq_id || outer_pred->id == gt_id);
    TEST_ASSERT_TRUE(inner_pred->id == eq_id || inner_pred->id == gt_id);
    TEST_ASSERT_TRUE(outer_pred->id != inner_pred->id);
    /* Pointer correctness: the resolved nodes should match g->nodes[id]. */
    TEST_ASSERT_EQ_PTR(outer_pred, &g->nodes[outer_pred->id]);
    TEST_ASSERT_EQ_PTR(inner_pred, &g->nodes[inner_pred->id]);

    /* And the filter chain bottoms out at SCAN(v1) — verifying the
     * input-pointer fix-up walked the whole chain correctly. */
    ray_op_t* scan_node = inner->inputs[0];
    TEST_ASSERT_NOT_NULL(scan_node);
    TEST_ASSERT_EQ_I(scan_node->opcode, OP_SCAN);

    /* Original filter id is still valid (it's now the inner filter
     * after split, since split_and_filter rewrites filter_node in
     * place to be the inner). */
    TEST_ASSERT_TRUE(filt_id < g->node_count);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t opt_entries[] = {
    { "opt/filter_reorder_type", test_filter_reorder_by_type, NULL, NULL },
    { "opt/filter_and_split", test_filter_and_split, NULL, NULL },
    { "opt/filter_reorder_dag", test_filter_reorder_dag, NULL, NULL },
    { "opt/pushdown_select", test_pushdown_past_select, NULL, NULL },
    { "opt/pushdown_group", test_pushdown_past_group, NULL, NULL },
    { "opt/projection_pushdown", test_projection_pushdown, NULL, NULL },
    { "opt/partition_pruning", test_partition_pruning_smoke, NULL, NULL },
    { "opt/partition_pruning_mask", test_partition_pruning_mask, NULL, NULL },
    { "opt/partition_pruning_in", test_partition_pruning_in, NULL, NULL },
    { "opt/partition_pruning_not_in", test_partition_pruning_not_in, NULL, NULL },
    { "opt/partition_pruning_in_type_mismatch", test_partition_pruning_in_type_mismatch, NULL, NULL },
    { "opt/window_dce", test_opt_window_dce, NULL, NULL },
    { "opt/pushdown_past_expand", test_opt_pushdown_past_expand, NULL, NULL },
    { "opt/pushdown_expand_blocked", test_opt_pushdown_expand_blocked, NULL, NULL },
    { "opt/realloc_during_split", test_opt_realloc_during_split, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


