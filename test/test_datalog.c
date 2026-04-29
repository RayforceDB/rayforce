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
/*
 * test_datalog.c — Tests for the Datalog engine (src/ops/datalog.h)
 *
 * Covers: deep source provenance (CSR offsets + packed source refs).
 */
#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/datalog.h"
#include "table/sym.h"     /* ray_read_sym for SYM column inspection */
#include "lang/eval.h"
#include <stdio.h>         /* fprintf in datalog_rf_setup */
#include <stdlib.h>        /* abort in datalog_rf_setup */
#include <string.h>

/* Forward-declare runtime API used by the full-runtime fixtures.
 * (Test target doesn't pull in core/runtime.h because it redefines
 * ray_vm_t, which clashes with lang/eval.h's definition — a pre-
 * existing duplication kept out of scope for this PR.) */
typedef struct ray_runtime_s ray_runtime_t;
ray_runtime_t* ray_runtime_create(int argc, char** argv);
void           ray_runtime_destroy(ray_runtime_t* rt);

static void datalog_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void datalog_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* Full runtime — required for ray_eval_str("(rule ...)") surface-syntax tests.
 * Setup returns void, so if runtime creation fails we abort explicitly. */
static ray_runtime_t* datalog_rf_rt = NULL;

static void datalog_rf_setup(void) {
    datalog_rf_rt = ray_runtime_create(0, NULL);
    if (!datalog_rf_rt) {
        fprintf(stderr, "datalog_rf_setup: ray_runtime_create returned NULL\n");
        abort();
    }
}

static void datalog_rf_teardown(void) {
    ray_runtime_destroy(datalog_rf_rt);
    datalog_rf_rt = NULL;
}

/* Verify that dl_get_provenance_src_offsets and dl_get_provenance_src_data
 * are populated correctly for a simple one-rule derivation.
 *
 * Program:
 *   EDB: edge(1,2), edge(2,3), edge(3,4)
 *   Rule: path(X,Y) :- edge(X,Y)
 *
 * Expected after eval with DL_FLAG_PROVENANCE:
 *   path has 3 rows (one per edge row).
 *   prov_col[i] = 0  (rule index 0 fired for all rows)
 *   prov_src_offsets = [0, 1, 2, 3]  (one source entry per derived row)
 *   prov_src_data[i] = (edge_rel_idx << 32) | i
 */
static test_result_t test_source_provenance(void) {
    int64_t src_vals[] = {1, 2, 3};
    int64_t dst_vals[] = {2, 3, 4};
    ray_t* src = ray_vec_from_raw(RAY_I64, src_vals, 3);
    ray_t* dst = ray_vec_from_raw(RAY_I64, dst_vals, 3);
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    ray_t* edge = ray_table_new(2);
    TEST_ASSERT_NOT_NULL(edge);
    edge = ray_table_add_col(edge, ray_sym_intern("edge__c0", 8), src);
    TEST_ASSERT_FALSE(RAY_IS_ERR(edge));
    edge = ray_table_add_col(edge, ray_sym_intern("edge__c1", 8), dst);
    TEST_ASSERT_FALSE(RAY_IS_ERR(edge));

    dl_program_t* prog = dl_program_new();
    TEST_ASSERT_NOT_NULL(prog);
    prog->flags |= DL_FLAG_PROVENANCE;

    int edge_idx = dl_add_edb(prog, "edge", edge, 2);
    TEST_ASSERT_EQ_I(edge_idx, 0);

    /* path(X,Y) :- edge(X,Y) */
    dl_rule_t rule;
    dl_rule_init(&rule, "path", 2);
    dl_rule_head_var(&rule, 0, 0);
    dl_rule_head_var(&rule, 1, 1);
    int body = dl_rule_add_atom(&rule, "edge", 2);
    TEST_ASSERT_EQ_I(body, 0);
    dl_body_set_var(&rule, body, 0, 0);
    dl_body_set_var(&rule, body, 1, 1);
    TEST_ASSERT_EQ_I(dl_add_rule(prog, &rule), 0);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    ray_t* out = dl_query(prog, "path");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 3);

    /* Rule-level provenance: all rows attributed to rule 0 */
    ray_t* prov = dl_get_provenance(prog, "path");
    TEST_ASSERT_NOT_NULL(prov);
    TEST_ASSERT_EQ_I((int)ray_len(prov), 3);
    int64_t* pv = (int64_t*)ray_data(prov);
    TEST_ASSERT_EQ_I((int)pv[0], 0);
    TEST_ASSERT_EQ_I((int)pv[1], 0);
    TEST_ASSERT_EQ_I((int)pv[2], 0);

    /* Deep source provenance: CSR offsets and packed source refs */
    ray_t* offsets = dl_get_provenance_src_offsets(prog, "path");
    ray_t* data    = dl_get_provenance_src_data(prog, "path");
    TEST_ASSERT_NOT_NULL(offsets);
    TEST_ASSERT_NOT_NULL(data);

    /* offsets: length nrows+1 = 4; each derived row has exactly 1 source */
    TEST_ASSERT_EQ_I((int)ray_len(offsets), 4);
    TEST_ASSERT_EQ_I((int)ray_len(data), 3);

    int64_t* off      = (int64_t*)ray_data(offsets);
    int64_t* src_data = (int64_t*)ray_data(data);

    TEST_ASSERT_EQ_I((int)off[0], 0);
    TEST_ASSERT_EQ_I((int)off[1], 1);
    TEST_ASSERT_EQ_I((int)off[2], 2);
    TEST_ASSERT_EQ_I((int)off[3], 3);

    /* Each entry encodes (rel_idx << 32) | row_idx */
    for (int i = 0; i < 3; i++) {
        int64_t expected = ((int64_t)edge_idx << 32) | (int64_t)i;
        TEST_ASSERT_TRUE(src_data[i] == expected);
    }

    dl_program_free(prog);
    ray_release(edge);
    ray_release(src);
    ray_release(dst);
    PASS();
}

/* Deep provenance is only populated when DL_FLAG_PROVENANCE is set.
 * Without the flag both getters must return NULL. */
static test_result_t test_source_prov_requires_flag(void) {
    int64_t vals[] = {1, 2};
    ray_t* v = ray_vec_from_raw(RAY_I64, vals, 2);
    TEST_ASSERT_NOT_NULL(v);

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("p__c0", 5), v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    dl_program_t* prog = dl_program_new();
    TEST_ASSERT_NOT_NULL(prog);
    /* DL_FLAG_PROVENANCE intentionally NOT set */

    dl_add_edb(prog, "p", tbl, 1);

    dl_rule_t rule;
    dl_rule_init(&rule, "q", 1);
    dl_rule_head_var(&rule, 0, 0);
    int body = dl_rule_add_atom(&rule, "p", 1);
    dl_body_set_var(&rule, body, 0, 0);
    dl_add_rule(prog, &rule);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    TEST_ASSERT_NULL(dl_get_provenance_src_offsets(prog, "q"));
    TEST_ASSERT_NULL(dl_get_provenance_src_data(prog, "q"));

    dl_program_free(prog);
    ray_release(tbl);
    ray_release(v);
    PASS();
}

/* Verify cmp body literal filters tuples: rule keeps only rows where col0 < 60.
 *
 * Program:
 *   EDB: weight(50), weight(60), weight(75), weight(85)
 *   Rule: small(W) :- weight(W), (< W 60)
 *
 * Expected: small has exactly 1 row = 50.
 */
static test_result_t test_cmp_const_filter(void) {
    int64_t vals[] = {50, 60, 75, 85};
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, 4);
    TEST_ASSERT_NOT_NULL(col);

    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(weight));

    dl_program_t* prog = dl_program_new();
    TEST_ASSERT_NOT_NULL(prog);

    int weight_idx = dl_add_edb(prog, "weight", weight, 1);
    TEST_ASSERT_EQ_I(weight_idx, 0);

    /* small(W) :- weight(W), (< W 60) */
    dl_rule_t rule;
    dl_rule_init(&rule, "small", 1);
    dl_rule_head_var(&rule, 0, 0);  /* head var idx 0 = W */

    int body = dl_rule_add_atom(&rule, "weight", 1);
    dl_body_set_var(&rule, body, 0, 0);  /* weight(W) */

    int cmp = dl_rule_add_cmp_const(&rule, DL_CMP_LT, 0, 60);  /* W < 60 */
    TEST_ASSERT((cmp) >= (0), "cmp >= 0");

    rule.n_vars = 1;
    TEST_ASSERT_EQ_I(dl_add_rule(prog, &rule), 0);
    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    ray_t* out = dl_query(prog, "small");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);

    ray_t* out_col = ray_table_get_col_idx(out, 0);
    TEST_ASSERT_NOT_NULL(out_col);
    int64_t* od = (int64_t*)ray_data(out_col);
    TEST_ASSERT_EQ_I((int)od[0], 50);

    dl_program_free(prog);
    ray_release(weight);
    ray_release(col);
    PASS();
}

/* Verify arithmetic assignment derives a new variable from input columns.
 *
 * Program:
 *   EDB: pair(2, 3), pair(5, 7), pair(10, 1)
 *   Rule: sum_rel(A, B, S) :- pair(A, B), (= S (+ A B))
 *
 * Expected: sum_rel has 3 rows: (2,3,5), (5,7,12), (10,1,11).
 */
static test_result_t test_arith_assignment(void) {
    int64_t a_vals[] = {2, 5, 10};
    int64_t b_vals[] = {3, 7, 1};
    ray_t* a_col = ray_vec_from_raw(RAY_I64, a_vals, 3);
    ray_t* b_col = ray_vec_from_raw(RAY_I64, b_vals, 3);
    TEST_ASSERT_NOT_NULL(a_col);
    TEST_ASSERT_NOT_NULL(b_col);

    ray_t* pair = ray_table_new(2);
    pair = ray_table_add_col(pair, ray_sym_intern("pair__c0", 8), a_col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(pair));
    pair = ray_table_add_col(pair, ray_sym_intern("pair__c1", 8), b_col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(pair));

    dl_program_t* prog = dl_program_new();
    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_EQ_I(dl_add_edb(prog, "pair", pair, 2), 0);

    /* sum_rel(A, B, S) :- pair(A, B), (= S (+ A B)) */
    dl_rule_t rule;
    dl_rule_init(&rule, "sum_rel", 3);
    dl_rule_head_var(&rule, 0, 0);  /* A */
    dl_rule_head_var(&rule, 1, 1);  /* B */
    dl_rule_head_var(&rule, 2, 2);  /* S */

    int body = dl_rule_add_atom(&rule, "pair", 2);
    dl_body_set_var(&rule, body, 0, 0);  /* A */
    dl_body_set_var(&rule, body, 1, 1);  /* B */

    /* expr = (+ A B) */
    dl_expr_t* expr = dl_expr_binop(OP_ADD, dl_expr_var(0), dl_expr_var(1));
    TEST_ASSERT_NOT_NULL(expr);
    int as = dl_rule_add_assign(&rule, 2, DL_OP_EQ, expr);  /* S = A + B */
    TEST_ASSERT((as) >= (0), "as >= 0");

    rule.n_vars = 3;
    TEST_ASSERT_EQ_I(dl_add_rule(prog, &rule), 0);
    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    ray_t* out = dl_query(prog, "sum_rel");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 3);

    ray_t* s_col = ray_table_get_col_idx(out, 2);
    TEST_ASSERT_NOT_NULL(s_col);
    int64_t* sd = (int64_t*)ray_data(s_col);
    /* Sums must include 5, 12, 11 (order may differ). */
    int saw5 = 0, saw12 = 0, saw11 = 0;
    for (int i = 0; i < 3; i++) {
        if (sd[i] == 5)  saw5  = 1;
        if (sd[i] == 12) saw12 = 1;
        if (sd[i] == 11) saw11 = 1;
    }
    TEST_ASSERT_EQ_I(saw5, 1);
    TEST_ASSERT_EQ_I(saw12, 1);
    TEST_ASSERT_EQ_I(saw11, 1);

    dl_program_free(prog);
    ray_release(pair);
    ray_release(a_col);
    ray_release(b_col);
    PASS();
}

/* Float arithmetic: (rule (fres ?x ?z) (trig ?x) (= ?z (+ 1.5 2.5))) -> z = 4.0 */
static test_result_t test_arith_assign_f64(void) {
    int64_t one[] = { 1 };
    ray_t* col = ray_vec_from_raw(RAY_I64, one, 1);
    ray_t* trig = ray_table_new(1);
    trig = ray_table_add_col(trig, ray_sym_intern("trig__c0", 8), col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "trig", trig, 1);

    dl_rule_t r; dl_rule_init(&r, "fres", 2);
    dl_rule_head_var(&r, 0, 0); dl_rule_head_var(&r, 1, 1);
    int b = dl_rule_add_atom(&r, "trig", 1);
    dl_body_set_var(&r, b, 0, 0);

    dl_expr_t* e = dl_expr_binop(OP_ADD, dl_expr_const_f64(1.5), dl_expr_const_f64(2.5));
    dl_rule_add_assign(&r, 1, DL_OP_EQ, e);
    r.n_vars = 2;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "fres");
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    ray_t* z_col = ray_table_get_col_idx(out, 1);
    TEST_ASSERT_NOT_NULL(z_col);
    TEST_ASSERT_EQ_I(z_col->type, RAY_F64);
    double* zd = (double*)ray_data(z_col);
    TEST_ASSERT_EQ_F(zd[0], 4.0, 1e-4);

    dl_program_free(prog);
    ray_release(trig); ray_release(col);
    PASS();
}

/* Verify dl_rule_add_agg populates body fields correctly. */
static test_result_t test_agg_builder(void) {
    dl_rule_t rule;
    dl_rule_init(&rule, "stats", 1);
    dl_rule_head_var(&rule, 0, 0);

    int idx = dl_rule_add_agg(&rule, DL_AGG_COUNT, 0, "weight", 1, 0);
    TEST_ASSERT_EQ_I(idx, 0);
    TEST_ASSERT_EQ_I(rule.body[0].type, DL_AGG);
    TEST_ASSERT_EQ_I(rule.body[0].agg_op, DL_AGG_COUNT);
    TEST_ASSERT_EQ_I(rule.body[0].agg_target_var, 0);
    TEST_ASSERT_STR_EQ(rule.body[0].agg_pred, "weight");
    TEST_ASSERT_EQ_I(rule.body[0].agg_arity, 1);
    TEST_ASSERT_EQ_I(rule.body[0].agg_value_col, 0);
    TEST_ASSERT_EQ_I(rule.n_vars, 1);

    dl_rule_t rule2;
    dl_rule_init(&rule2, "sum_stats", 1);
    dl_rule_head_var(&rule2, 0, 3);
    int idx2 = dl_rule_add_agg(&rule2, DL_AGG_SUM, 3, "readings", 4, 2);
    TEST_ASSERT_EQ_I(idx2, 0);
    TEST_ASSERT_EQ_I(rule2.body[0].agg_op, DL_AGG_SUM);
    TEST_ASSERT_EQ_I(rule2.body[0].agg_target_var, 3);
    TEST_ASSERT_EQ_I(rule2.body[0].agg_arity, 4);
    TEST_ASSERT_EQ_I(rule2.body[0].agg_value_col, 2);
    TEST_ASSERT_EQ_I(rule2.n_vars, 4);
    PASS();
}

/* Aggregates over an IDB must be evaluated in a strictly higher stratum
 * than the IDB itself. Program:
 *   EDB: edge(1,2), edge(2,3)
 *   Rule R0: path(X,Y) :- edge(X,Y)
 *   Rule R1: path_count(N) :- (count ?N path)
 * After stratification: R1.stratum > R0.stratum. */
static test_result_t test_agg_stratifies_above_source(void) {
    int64_t s_vals[] = {1, 2};
    int64_t d_vals[] = {2, 3};
    ray_t* sc = ray_vec_from_raw(RAY_I64, s_vals, 2);
    ray_t* dc = ray_vec_from_raw(RAY_I64, d_vals, 2);
    ray_t* edge = ray_table_new(2);
    edge = ray_table_add_col(edge, ray_sym_intern("edge__c0", 8), sc);
    edge = ray_table_add_col(edge, ray_sym_intern("edge__c1", 8), dc);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "edge", edge, 2);

    dl_rule_t r0; dl_rule_init(&r0, "path", 2);
    dl_rule_head_var(&r0, 0, 0); dl_rule_head_var(&r0, 1, 1);
    int b = dl_rule_add_atom(&r0, "edge", 2);
    dl_body_set_var(&r0, b, 0, 0); dl_body_set_var(&r0, b, 1, 1);
    r0.n_vars = 2;
    dl_add_rule(prog, &r0);

    dl_rule_t r1; dl_rule_init(&r1, "path_count", 1);
    dl_rule_head_var(&r1, 0, 0);
    dl_rule_add_agg(&r1, DL_AGG_COUNT, 0, "path", 2, 0);
    r1.n_vars = 1;
    dl_add_rule(prog, &r1);

    TEST_ASSERT_EQ_I(dl_stratify(prog), 0);
    TEST_ASSERT((prog->rules[1].stratum) > (prog->rules[0].stratum), "prog->rules[1].stratum > prog->rules[0].stratum");

    dl_program_free(prog);
    ray_release(edge); ray_release(sc); ray_release(dc);
    PASS();
}

/* (count ?N weight) where weight has 4 rows -> N = 4. */
static test_result_t test_agg_count_edb(void) {
    int64_t vals[] = {50, 60, 75, 85};
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, 4);
    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);

    dl_rule_t r; dl_rule_init(&r, "wcount", 1);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_add_agg(&r, DL_AGG_COUNT, 0, "weight", 1, 0);
    r.n_vars = 1;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wcount");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    int64_t* od = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    TEST_ASSERT_EQ_I((int)od[0], 4);

    dl_program_free(prog);
    ray_release(weight); ray_release(col);
    PASS();
}

static ray_t* make_weight_edb(void) {
    int64_t vals[] = {50, 60, 75, 85};
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, 4);
    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), col);
    ray_release(col);
    return weight;
}

static test_result_t test_agg_sum(void) {
    ray_t* weight = make_weight_edb();

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);

    dl_rule_t r; dl_rule_init(&r, "wsum", 1);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_add_agg(&r, DL_AGG_SUM, 0, "weight", 1, 0);
    r.n_vars = 1;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wsum");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    int64_t* od = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    TEST_ASSERT_EQ_I((int)od[0], 270);

    dl_program_free(prog);
    ray_release(weight);
    PASS();
}

static test_result_t test_agg_min(void) {
    ray_t* weight = make_weight_edb();

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);

    dl_rule_t r; dl_rule_init(&r, "wmin", 1);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_add_agg(&r, DL_AGG_MIN, 0, "weight", 1, 0);
    r.n_vars = 1;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wmin");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    int64_t* od = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    TEST_ASSERT_EQ_I((int)od[0], 50);

    dl_program_free(prog);
    ray_release(weight);
    PASS();
}

static test_result_t test_agg_max(void) {
    ray_t* weight = make_weight_edb();

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);

    dl_rule_t r; dl_rule_init(&r, "wmax", 1);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_add_agg(&r, DL_AGG_MAX, 0, "weight", 1, 0);
    r.n_vars = 1;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wmax");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    int64_t* od = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    TEST_ASSERT_EQ_I((int)od[0], 85);

    dl_program_free(prog);
    ray_release(weight);
    PASS();
}

static test_result_t test_agg_avg(void) {
    ray_t* weight = make_weight_edb();

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);

    dl_rule_t r; dl_rule_init(&r, "wavg", 1);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_add_agg(&r, DL_AGG_AVG, 0, "weight", 1, 0);
    r.n_vars = 1;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wavg");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    ray_t* avg_col = ray_table_get_col_idx(out, 0);
    TEST_ASSERT_NOT_NULL(avg_col);
    TEST_ASSERT_EQ_I(avg_col->type, RAY_F64);
    double* od = (double*)ray_data(avg_col);
    TEST_ASSERT_EQ_F(od[0], 67.5, 1e-4);

    dl_program_free(prog);
    ray_release(weight);
    PASS();
}

/* MIN over empty source -> rule produces no row. */
static test_result_t test_agg_min_empty(void) {
    int64_t dummy = 0;
    ray_t* empty_vec = ray_vec_from_raw(RAY_I64, &dummy, 0);
    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), empty_vec);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);

    dl_rule_t r; dl_rule_init(&r, "wmin", 1);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_add_agg(&r, DL_AGG_MIN, 0, "weight", 1, 0);
    r.n_vars = 1;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wmin");
    /* Either NULL (no rel) or a 0-row table is acceptable "no row" semantics. */
    if (out) TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 0);

    dl_program_free(prog);
    ray_release(weight); ray_release(empty_vec);
    PASS();
}

/* COUNT over empty source -> 1 row with value 0 (well-defined). */
static test_result_t test_agg_count_empty(void) {
    int64_t dummy = 0;
    ray_t* empty_vec = ray_vec_from_raw(RAY_I64, &dummy, 0);
    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), empty_vec);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);

    dl_rule_t r; dl_rule_init(&r, "wcnt", 1);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_add_agg(&r, DL_AGG_COUNT, 0, "weight", 1, 0);
    r.n_vars = 1;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wcnt");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    int64_t* od = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    TEST_ASSERT_EQ_I((int)od[0], 0);

    dl_program_free(prog);
    ray_release(weight); ray_release(empty_vec);
    PASS();
}

/* MAX over empty source -> rule produces no row. */
static test_result_t test_agg_max_empty(void) {
    int64_t dummy = 0;
    ray_t* empty_vec = ray_vec_from_raw(RAY_I64, &dummy, 0);
    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), empty_vec);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);

    dl_rule_t r; dl_rule_init(&r, "wmax", 1);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_add_agg(&r, DL_AGG_MAX, 0, "weight", 1, 0);
    r.n_vars = 1;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wmax");
    if (out) TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 0);

    dl_program_free(prog);
    ray_release(weight); ray_release(empty_vec);
    PASS();
}

/* SUM over empty source -> one row with value 0 (additive identity). */
static test_result_t test_agg_sum_empty(void) {
    int64_t dummy = 0;
    ray_t* empty_vec = ray_vec_from_raw(RAY_I64, &dummy, 0);
    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), empty_vec);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);

    dl_rule_t r; dl_rule_init(&r, "wsum", 1);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_add_agg(&r, DL_AGG_SUM, 0, "weight", 1, 0);
    r.n_vars = 1;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wsum");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    int64_t* od = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    TEST_ASSERT_EQ_I((int)od[0], 0);

    dl_program_free(prog);
    ray_release(weight); ray_release(empty_vec);
    PASS();
}

/* AVG over empty source -> rule produces no row. */
static test_result_t test_agg_avg_empty(void) {
    int64_t dummy = 0;
    ray_t* empty_vec = ray_vec_from_raw(RAY_I64, &dummy, 0);
    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), empty_vec);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);

    dl_rule_t r; dl_rule_init(&r, "wavg", 1);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_add_agg(&r, DL_AGG_AVG, 0, "weight", 1, 0);
    r.n_vars = 1;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wavg");
    if (out) TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 0);

    dl_program_free(prog);
    ray_release(weight); ray_release(empty_vec);
    PASS();
}

/* weight_by_user(user_id, kg): (1,50), (1,60), (2,75), (2,85)
 * Rule: user_count(?u, ?n) :- count(?n, weight_by_user) by (?u, col 0)
 * Expected: (1,2), (2,2) */
static test_result_t test_agg_count_grouped(void) {
    int64_t users[]   = {1, 1, 2, 2};
    int64_t weights[] = {50, 60, 75, 85};
    ray_t* u_col = ray_vec_from_raw(RAY_I64, users, 4);
    ray_t* w_col = ray_vec_from_raw(RAY_I64, weights, 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("weight_by_user__c0", 18), u_col);
    tbl = ray_table_add_col(tbl, ray_sym_intern("weight_by_user__c1", 18), w_col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight_by_user", tbl, 2);

    dl_rule_t r; dl_rule_init(&r, "user_count", 2);
    dl_rule_head_var(&r, 0, 0);  /* u */
    dl_rule_head_var(&r, 1, 1);  /* n */
    int idx = dl_rule_add_agg(&r, DL_AGG_COUNT, 1, "weight_by_user", 2, 0);
    int key_vars[] = { 0 };
    int key_cols[] = { 0 };
    TEST_ASSERT_EQ_I(dl_rule_agg_set_group(&r, idx, key_vars, key_cols, 1), 0);
    r.n_vars = 2;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "user_count");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 2);

    /* Rows may appear in any order; check both keys present with count == 2. */
    int64_t* uo = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    int64_t* no = (int64_t*)ray_data(ray_table_get_col_idx(out, 1));
    int seen_u1 = 0, seen_u2 = 0;
    for (int i = 0; i < 2; i++) {
        if (uo[i] == 1) { TEST_ASSERT_EQ_I((int)no[i], 2); seen_u1 = 1; }
        else if (uo[i] == 2) { TEST_ASSERT_EQ_I((int)no[i], 2); seen_u2 = 1; }
    }
    TEST_ASSERT_EQ_I(seen_u1 && seen_u2, 1);

    dl_program_free(prog);
    ray_release(tbl); ray_release(u_col); ray_release(w_col);
    PASS();
}

/* Rule: user_sum(?u, ?s) :- sum(?s, weight_by_user col 1) by (?u, col 0)
 * Expected: (1, 110), (2, 160) */
static test_result_t test_agg_sum_grouped(void) {
    int64_t users[]   = {1, 1, 2, 2};
    int64_t weights[] = {50, 60, 75, 85};
    ray_t* u_col = ray_vec_from_raw(RAY_I64, users, 4);
    ray_t* w_col = ray_vec_from_raw(RAY_I64, weights, 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("weight_by_user__c0", 18), u_col);
    tbl = ray_table_add_col(tbl, ray_sym_intern("weight_by_user__c1", 18), w_col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight_by_user", tbl, 2);

    dl_rule_t r; dl_rule_init(&r, "user_sum", 2);
    dl_rule_head_var(&r, 0, 0);  /* u */
    dl_rule_head_var(&r, 1, 1);  /* s */
    int idx = dl_rule_add_agg(&r, DL_AGG_SUM, 1, "weight_by_user", 2, 1);
    int key_vars[] = { 0 };
    int key_cols[] = { 0 };
    TEST_ASSERT_EQ_I(dl_rule_agg_set_group(&r, idx, key_vars, key_cols, 1), 0);
    r.n_vars = 2;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "user_sum");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 2);

    int64_t* uo = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    int64_t* so = (int64_t*)ray_data(ray_table_get_col_idx(out, 1));
    int seen_u1 = 0, seen_u2 = 0;
    for (int i = 0; i < 2; i++) {
        if (uo[i] == 1) { TEST_ASSERT_EQ_I((int)so[i], 110); seen_u1 = 1; }
        else if (uo[i] == 2) { TEST_ASSERT_EQ_I((int)so[i], 160); seen_u2 = 1; }
    }
    TEST_ASSERT_EQ_I(seen_u1 && seen_u2, 1);

    dl_program_free(prog);
    ray_release(tbl); ray_release(u_col); ray_release(w_col);
    PASS();
}

/* Surface syntax: (rule (wcount ?n) (count ?n weight)) */
static test_result_t test_agg_parse_count_scalar(void) {
    ray_t* ok = ray_eval_str("(rule (wcount ?n) (count ?n weight))");
    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_TRUE(!RAY_IS_ERR(ok));
    ray_release(ok);

    int64_t vals[] = {50, 60, 75, 85};
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, 4);
    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);
    dl_append_global_rules(prog);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wcount");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    int64_t* od = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    TEST_ASSERT_EQ_I((int)od[0], 4);

    dl_program_free(prog);
    ray_release(weight); ray_release(col);
    PASS();
}

/* Surface syntax: (rule (wsum ?s) (sum ?s weight 0)) */
static test_result_t test_agg_parse_sum_scalar(void) {
    ray_t* ok = ray_eval_str("(rule (wsum ?s) (sum ?s weight 0))");
    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_TRUE(!RAY_IS_ERR(ok));
    ray_release(ok);

    ray_t* weight = make_weight_edb();

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);
    dl_append_global_rules(prog);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "wsum");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    int64_t* od = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    TEST_ASSERT_EQ_I((int)od[0], 270);

    dl_program_free(prog);
    ray_release(weight);
    PASS();
}

/* Surface syntax: (count ?n weight_by_user by ?u 0) */
static test_result_t test_agg_parse_count_grouped(void) {
    ray_t* ok = ray_eval_str(
        "(rule (user_count ?u ?n) (count ?n weight_by_user by ?u 0))");
    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_TRUE(!RAY_IS_ERR(ok));
    ray_release(ok);

    int64_t users[]   = {1, 1, 2, 2};
    int64_t weights[] = {50, 60, 75, 85};
    ray_t* u_col = ray_vec_from_raw(RAY_I64, users, 4);
    ray_t* w_col = ray_vec_from_raw(RAY_I64, weights, 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("weight_by_user__c0", 18), u_col);
    tbl = ray_table_add_col(tbl, ray_sym_intern("weight_by_user__c1", 18), w_col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight_by_user", tbl, 2);
    dl_append_global_rules(prog);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "user_count");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 2);

    int64_t* uo = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    int64_t* no = (int64_t*)ray_data(ray_table_get_col_idx(out, 1));
    int seen_u1 = 0, seen_u2 = 0;
    for (int i = 0; i < 2; i++) {
        if (uo[i] == 1) { TEST_ASSERT_EQ_I((int)no[i], 2); seen_u1 = 1; }
        else if (uo[i] == 2) { TEST_ASSERT_EQ_I((int)no[i], 2); seen_u2 = 1; }
    }
    TEST_ASSERT_EQ_I(seen_u1 && seen_u2, 1);

    dl_program_free(prog);
    ray_release(tbl); ray_release(u_col); ray_release(w_col);
    PASS();
}

/* Surface syntax: (between ?w lo hi) -> two cmp literals; weight 50,60,75,85 -> mid: 60, 75 */
static test_result_t test_between_sugar_parse(void) {
    ray_t* ok = ray_eval_str(
        "(rule (mid ?w) (weight ?w) (between ?w 60 80))");
    TEST_ASSERT_NOT_NULL(ok);
    TEST_ASSERT_TRUE(!RAY_IS_ERR(ok));
    ray_release(ok);

    int64_t vals[] = {50, 60, 75, 85};
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, 4);
    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);
    dl_append_global_rules(prog);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "mid");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 2);

    int64_t* od = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    int seen60 = 0, seen75 = 0;
    for (int i = 0; i < 2; i++) {
        if (od[i] == 60) seen60 = 1;
        else if (od[i] == 75) seen75 = 1;
    }
    TEST_ASSERT_EQ_I(seen60 && seen75, 1);

    dl_program_free(prog);
    ray_release(weight); ray_release(col);
    PASS();
}

/* Mixed i64 + f64: (= ?z (+ 1.5 ?x)) promotes to RAY_F64. */
static test_result_t test_arith_assign_f64_mixed(void) {
    int64_t one[] = { 1 };
    ray_t* col = ray_vec_from_raw(RAY_I64, one, 1);
    ray_t* trig = ray_table_new(1);
    trig = ray_table_add_col(trig, ray_sym_intern("trig__c0", 8), col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "trig", trig, 1);

    dl_rule_t r; dl_rule_init(&r, "fres", 2);
    dl_rule_head_var(&r, 0, 0); dl_rule_head_var(&r, 1, 1);
    int b = dl_rule_add_atom(&r, "trig", 1);
    dl_body_set_var(&r, b, 0, 0);

    dl_expr_t* e = dl_expr_binop(OP_ADD, dl_expr_const_f64(1.5), dl_expr_var(0));
    dl_rule_add_assign(&r, 1, DL_OP_EQ, e);
    r.n_vars = 2;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "fres");
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    ray_t* z_col = ray_table_get_col_idx(out, 1);
    TEST_ASSERT_NOT_NULL(z_col);
    TEST_ASSERT_EQ_I(z_col->type, RAY_F64);
    double* zd = (double*)ray_data(z_col);
    TEST_ASSERT_EQ_F(zd[0], 2.5, 1e-4);

    dl_program_free(prog);
    ray_release(trig); ray_release(col);
    PASS();
}

/* A5 aggregate parser rejects COUNT with explicit value column. */
static test_result_t test_agg_parse_reject_count_with_col(void) {
    ray_t* r = ray_eval_str("(rule (w ?n) (count ?n weight 0))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(r);
    PASS();
}

/* A5 aggregate parser rejects SUM without value column index. */
static test_result_t test_agg_parse_reject_sum_without_col(void) {
    ray_t* r = ray_eval_str("(rule (w ?s) (sum ?s weight))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(r);
    PASS();
}

/* A5 aggregate parser rejects incomplete `by` clause (missing column after key var). */
static test_result_t test_agg_parse_reject_by_missing_col(void) {
    ray_t* r = ray_eval_str("(rule (w ?n) (count ?n weight by ?k))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(r);
    PASS();
}

/* A5 aggregate parser rejects non-variable aggregate target. */
static test_result_t test_agg_parse_reject_non_var_target(void) {
    ray_t* r = ray_eval_str("(rule (w ?x) (count 5 weight))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(r);
    PASS();
}

/* =====================================================================
 * Head-constant rules (Phase B dep: rayforce2 rule heads may contain
 * RAY_SYM / RAY_I64 / RAY_F64 literals alongside variables).  These
 * tests exist to prevent regression of a previously-reverted attempt
 * that corrupted memory across IDB boundaries.
 * ===================================================================== */

/* (rule (band_small W) (weight W) (< W 60)) — head slot 0 is a variable.
 * Analogous to test_cmp_const_filter but uses dl_rule_head_const for a
 * one-slot symbolic band label so the output table has a SYM column. */
static test_result_t test_rule_head_const_single_rule(void) {
    int64_t vals[] = { 50, 70, 90 };
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, 3);
    TEST_ASSERT_NOT_NULL(col);

    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(weight));

    dl_program_t* prog = dl_program_new();
    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_EQ_I(dl_add_edb(prog, "weight", weight, 1), 0);

    /* (rule (band "small")    (weight ?W)  (< ?W 60)) */
    dl_rule_t rule;
    dl_rule_init(&rule, "band", 1);
    int64_t sym_small = ray_sym_intern("small", 5);
    dl_rule_head_const_typed(&rule, 0, sym_small, RAY_SYM);

    int body = dl_rule_add_atom(&rule, "weight", 1);
    dl_body_set_var(&rule, body, 0, 0);  /* binds ?W = col 0 */
    int cmp = dl_rule_add_cmp_const(&rule, DL_CMP_LT, 0, 60);
    TEST_ASSERT((cmp) >= (0), "cmp >= 0");

    rule.n_vars = 1;
    TEST_ASSERT_EQ_I(dl_add_rule(prog, &rule), 0);
    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    ray_t* out = dl_query(prog, "band");
    TEST_ASSERT_NOT_NULL(out);
    /* One row (50 < 60).  Duplicate elimination must leave a single
     * ("small",) tuple because all surviving weights broadcast the
     * same head constant. */
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    ray_t* oc = ray_table_get_col_idx(out, 0);
    TEST_ASSERT_NOT_NULL(oc);
    TEST_ASSERT_EQ_I(oc->type, RAY_SYM);
    int64_t got = ray_read_sym(ray_data(oc), 0, oc->type, oc->attrs);
    TEST_ASSERT_EQ_I((int)got, (int)sym_small);

    dl_program_free(prog);
    ray_release(weight); ray_release(col);
    PASS();
}

/* Head slot holds an I64 constant alongside a variable.
 * (rule (ev X 1) (pair ?X ?_)) */
static test_result_t test_rule_head_const_i64(void) {
    int64_t a_vals[] = { 10, 20 };
    int64_t b_vals[] = {  1,  2 };
    ray_t* a = ray_vec_from_raw(RAY_I64, a_vals, 2);
    ray_t* b = ray_vec_from_raw(RAY_I64, b_vals, 2);
    ray_t* pair = ray_table_new(2);
    pair = ray_table_add_col(pair, ray_sym_intern("pair__c0", 8), a);
    pair = ray_table_add_col(pair, ray_sym_intern("pair__c1", 8), b);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "pair", pair, 2);

    /* (rule (ev ?X 1) (pair ?X ?Y)) */
    dl_rule_t r; dl_rule_init(&r, "ev", 2);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_head_const_typed(&r, 1, 1, RAY_I64);
    int bi = dl_rule_add_atom(&r, "pair", 2);
    dl_body_set_var(&r, bi, 0, 0);
    dl_body_set_var(&r, bi, 1, 1);
    r.n_vars = 2;
    TEST_ASSERT_EQ_I(dl_add_rule(prog, &r), 0);
    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    ray_t* out = dl_query(prog, "ev");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 2);
    ray_t* c1 = ray_table_get_col_idx(out, 1);
    TEST_ASSERT_EQ_I(c1->type, RAY_I64);
    int64_t* d = (int64_t*)ray_data(c1);
    TEST_ASSERT_EQ_I((int)d[0], 1);
    TEST_ASSERT_EQ_I((int)d[1], 1);

    dl_program_free(prog);
    ray_release(pair); ray_release(a); ray_release(b);
    PASS();
}

/* THE FAILURE CASE the previous attempt blew up on:
 *   R1: (foo "small") :- (edge ?U ?V)        head = constant SYM
 *   R2: (bar ?B)      :- (foo ?B)            reads R1's constant-head IDB
 * Expected: bar contains one row "small".
 * Previously: cross-IDB broadcast column dangled; crash or UB.
 */
static test_result_t test_rule_head_const_cross_idb(void) {
    int64_t u_vals[] = { 1, 2 };
    int64_t v_vals[] = { 2, 3 };
    ray_t* u = ray_vec_from_raw(RAY_I64, u_vals, 2);
    ray_t* v = ray_vec_from_raw(RAY_I64, v_vals, 2);
    ray_t* edge = ray_table_new(2);
    edge = ray_table_add_col(edge, ray_sym_intern("edge__c0", 8), u);
    edge = ray_table_add_col(edge, ray_sym_intern("edge__c1", 8), v);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "edge", edge, 2);

    int64_t sym_small = ray_sym_intern("small", 5);

    /* R1: (foo "small") :- (edge ?U ?V) */
    dl_rule_t r1; dl_rule_init(&r1, "foo", 1);
    dl_rule_head_const_typed(&r1, 0, sym_small, RAY_SYM);
    int r1b = dl_rule_add_atom(&r1, "edge", 2);
    dl_body_set_var(&r1, r1b, 0, 0);
    dl_body_set_var(&r1, r1b, 1, 1);
    r1.n_vars = 2;
    TEST_ASSERT((dl_add_rule(prog, &r1)) >= (0), "dl_add_rule(prog, &r1) >= 0");

    /* R2: (bar ?B) :- (foo ?B) */
    dl_rule_t r2; dl_rule_init(&r2, "bar", 1);
    dl_rule_head_var(&r2, 0, 0);
    int r2b = dl_rule_add_atom(&r2, "foo", 1);
    dl_body_set_var(&r2, r2b, 0, 0);
    r2.n_vars = 1;
    TEST_ASSERT((dl_add_rule(prog, &r2)) >= (0), "dl_add_rule(prog, &r2) >= 0");

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    ray_t* foo = dl_query(prog, "foo");
    TEST_ASSERT_NOT_NULL(foo);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(foo), 1);

    ray_t* bar = dl_query(prog, "bar");
    TEST_ASSERT_NOT_NULL(bar);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(bar), 1);
    ray_t* bc = ray_table_get_col_idx(bar, 0);
    TEST_ASSERT_NOT_NULL(bc);
    TEST_ASSERT_EQ_I(bc->type, RAY_SYM);
    int64_t got = ray_read_sym(ray_data(bc), 0, bc->type, bc->attrs);
    TEST_ASSERT_EQ_I((int)got, (int)sym_small);

    dl_program_free(prog);
    ray_release(edge); ray_release(u); ray_release(v);
    PASS();
}

/* Constant head slot holding an F64. */
static test_result_t test_rule_head_const_f64(void) {
    int64_t vals[] = { 1 };
    ray_t* col = ray_vec_from_raw(RAY_I64, vals, 1);
    ray_t* trig = ray_table_new(1);
    trig = ray_table_add_col(trig, ray_sym_intern("trig__c0", 8), col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "trig", trig, 1);

    /* (rule (pi 3.14) (trig ?X)) */
    dl_rule_t r; dl_rule_init(&r, "pi", 1);
    dl_rule_head_const_f64(&r, 0, 3.14);
    int b = dl_rule_add_atom(&r, "trig", 1);
    dl_body_set_var(&r, b, 0, 0);
    r.n_vars = 1;
    TEST_ASSERT((dl_add_rule(prog, &r)) >= (0), "dl_add_rule(prog, &r) >= 0");
    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    ray_t* out = dl_query(prog, "pi");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    ray_t* oc = ray_table_get_col_idx(out, 0);
    TEST_ASSERT_EQ_I(oc->type, RAY_F64);
    double* d = (double*)ray_data(oc);
    TEST_ASSERT_EQ_F(d[0], 3.14, 1e-4);

    dl_program_free(prog);
    ray_release(trig); ray_release(col);
    PASS();
}

/* Constant head combined with an aggregate body literal.
 * (rule (stat "total" ?N) (count ?N weight))
 * Expected: stat = [("total", 4)]. */
static test_result_t test_rule_head_const_with_agg(void) {
    int64_t wv[] = { 50, 60, 75, 85 };
    ray_t* wc = ray_vec_from_raw(RAY_I64, wv, 4);
    ray_t* weight = ray_table_new(1);
    weight = ray_table_add_col(weight, ray_sym_intern("weight__c0", 10), wc);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "weight", weight, 1);

    int64_t sym_total = ray_sym_intern("total", 5);

    dl_rule_t r; dl_rule_init(&r, "stat", 2);
    dl_rule_head_const_typed(&r, 0, sym_total, RAY_SYM);
    dl_rule_head_var(&r, 1, 0);  /* ?N */
    dl_rule_add_agg(&r, DL_AGG_COUNT, 0, "weight", 1, 0);
    r.n_vars = 1;
    TEST_ASSERT((dl_add_rule(prog, &r)) >= (0), "dl_add_rule(prog, &r) >= 0");
    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    ray_t* out = dl_query(prog, "stat");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    ray_t* label = ray_table_get_col_idx(out, 0);
    TEST_ASSERT_EQ_I(label->type, RAY_SYM);
    int64_t lsym = ray_read_sym(ray_data(label), 0, label->type, label->attrs);
    TEST_ASSERT_EQ_I((int)lsym, (int)sym_total);

    ray_t* nc = ray_table_get_col_idx(out, 1);
    int64_t* nd = (int64_t*)ray_data(nc);
    TEST_ASSERT_EQ_I((int)nd[0], 4);

    dl_program_free(prog);
    ray_release(weight); ray_release(wc);
    PASS();
}

/* Negation over a relation that is derived via a constant head.
 * EDB: kind(1,'big'), kind(2,'big'), kind(3,'small')
 * R1:  (small ?X) :- (kind ?X 'small')
 * R2:  (big   ?X) :- (kind ?X ?K), not (small ?X)
 *
 * This exercises two cross-IDB head-const shapes at once:
 *   (a) R1's head is a variable, but R2 reads a relation whose SCHEMA
 *       came from a typed const EDB (kind's second col is SYM).
 *   (b) stratification must place R2 after R1 since R2 negates (small).
 *
 * The point of this test is that the stratified negation path still
 * works when sym-typed IDB columns are present — the broadcast-const
 * machinery must not leak into other columns or break antijoin. */
static test_result_t test_rule_head_const_with_negation(void) {
    int64_t sym_big = ray_sym_intern("big", 3);
    int64_t sym_small = ray_sym_intern("small", 5);
    int64_t id_vals[] = { 1, 2, 3 };
    int64_t k_vals [] = { sym_big, sym_big, sym_small };

    ray_t* id_col = ray_vec_from_raw(RAY_I64, id_vals, 3);
    /* Build a SYM vec for the kind column.  ray_vec_from_raw on RAY_SYM
     * would need width-aware packing; simpler to use ray_vec_new + write. */
    ray_t* k_col = ray_vec_new(RAY_SYM, 3);
    k_col->len = 3;
    for (int i = 0; i < 3; i++) {
        ray_write_sym(ray_data(k_col), i, (uint64_t)k_vals[i],
                      k_col->type, k_col->attrs);
    }
    ray_t* kind = ray_table_new(2);
    kind = ray_table_add_col(kind, ray_sym_intern("kind__c0", 8), id_col);
    kind = ray_table_add_col(kind, ray_sym_intern("kind__c1", 8), k_col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "kind", kind, 2);

    /* R1: (small ?X) :- (kind ?X "small") */
    dl_rule_t r1; dl_rule_init(&r1, "small", 1);
    dl_rule_head_var(&r1, 0, 0);
    int r1b = dl_rule_add_atom(&r1, "kind", 2);
    dl_body_set_var(&r1, r1b, 0, 0);
    dl_body_set_const(&r1, r1b, 1, sym_small);
    r1.n_vars = 1;
    TEST_ASSERT((dl_add_rule(prog, &r1)) >= (0), "dl_add_rule(prog, &r1) >= 0");

    /* R2: (big ?X) :- (kind ?X ?K), not (small ?X) */
    dl_rule_t r2; dl_rule_init(&r2, "big", 1);
    dl_rule_head_var(&r2, 0, 0);
    int r2b = dl_rule_add_atom(&r2, "kind", 2);
    dl_body_set_var(&r2, r2b, 0, 0);
    dl_body_set_var(&r2, r2b, 1, 1);  /* ?K */
    int r2n = dl_rule_add_neg(&r2, "small", 1);
    dl_body_set_var(&r2, r2n, 0, 0);
    r2.n_vars = 2;
    TEST_ASSERT((dl_add_rule(prog, &r2)) >= (0), "dl_add_rule(prog, &r2) >= 0");

    TEST_ASSERT_EQ_I(dl_stratify(prog), 0);
    TEST_ASSERT((prog->rules[1].stratum) > (prog->rules[0].stratum), "prog->rules[1].stratum > prog->rules[0].stratum");

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    ray_t* big = dl_query(prog, "big");
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(big), 2);

    dl_program_free(prog);
    ray_release(kind); ray_release(id_col); ray_release(k_col);
    PASS();
}

/* Stratification: when a constant-head IDB is referenced (positively *or*
 * through negation) by another rule, the dependency is preserved and the
 * negating rule is placed in a strictly higher stratum.
 *
 * R1: (marker ?X "seen") :- (src ?X)
 * R2: (unseen ?X)        :- (src ?X), not (marker ?X ?K)
 *
 * R1 emits one row per src, each with a broadcast SYM.  R2 negates on
 * the bound var ?X, which the evaluator antijoin-drops.  Net result:
 * unseen is empty and R2 must be in a higher stratum than R1. */
static test_result_t test_rule_head_const_stratification(void) {
    int64_t src_vals[] = { 1, 2 };
    ray_t* sc = ray_vec_from_raw(RAY_I64, src_vals, 2);
    ray_t* src = ray_table_new(1);
    src = ray_table_add_col(src, ray_sym_intern("src__c0", 7), sc);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "src", src, 1);

    int64_t sym_seen = ray_sym_intern("seen", 4);

    dl_rule_t r1; dl_rule_init(&r1, "marker", 2);
    dl_rule_head_var(&r1, 0, 0);
    dl_rule_head_const_typed(&r1, 1, sym_seen, RAY_SYM);
    int r1b = dl_rule_add_atom(&r1, "src", 1);
    dl_body_set_var(&r1, r1b, 0, 0);
    r1.n_vars = 1;
    TEST_ASSERT((dl_add_rule(prog, &r1)) >= (0), "dl_add_rule(prog, &r1) >= 0");

    dl_rule_t r2; dl_rule_init(&r2, "unseen", 1);
    dl_rule_head_var(&r2, 0, 0);
    int r2b = dl_rule_add_atom(&r2, "src", 1);
    dl_body_set_var(&r2, r2b, 0, 0);
    int r2n = dl_rule_add_neg(&r2, "marker", 2);
    dl_body_set_var(&r2, r2n, 0, 0);      /* ?X bound from body */
    dl_body_set_var(&r2, r2n, 1, 1);      /* ?K body-only var */
    r2.n_vars = 2;
    TEST_ASSERT((dl_add_rule(prog, &r2)) >= (0), "dl_add_rule(prog, &r2) >= 0");

    TEST_ASSERT_EQ_I(dl_stratify(prog), 0);
    TEST_ASSERT((prog->rules[1].stratum) > (prog->rules[0].stratum), "prog->rules[1].stratum > prog->rules[0].stratum");

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    /* marker has 2 rows, each with its own x and broadcast SYM. */
    ray_t* m = dl_query(prog, "marker");
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(m), 2);
    ray_t* msym = ray_table_get_col_idx(m, 1);
    TEST_ASSERT_EQ_I(msym->type, RAY_SYM);

    /* Every src row has a marker, so unseen is empty. */
    ray_t* un = dl_query(prog, "unseen");
    TEST_ASSERT_NOT_NULL(un);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(un), 0);

    dl_program_free(prog);
    ray_release(src); ray_release(sc);
    PASS();
}

/* Conflicting head-const types for the same IDB slot across rules must
 * surface via dl_eval == -1 rather than a silent stderr print.  Rule A
 * commits slot 1 to RAY_SYM; rule B tries to commit it to RAY_F64. */
static test_result_t test_rule_head_const_type_conflict(void) {
    int64_t xs[] = {1};
    ray_t* xc = ray_vec_from_raw(RAY_I64, xs, 1);
    ray_t* src = ray_table_new(1);
    src = ray_table_add_col(src, ray_sym_intern("src__c0", 7), xc);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "src", src, 1);

    /* Rule A: (tag ?x "sym") — slot 1 committed to RAY_SYM. */
    dl_rule_t a; dl_rule_init(&a, "tag", 2);
    dl_rule_head_var(&a, 0, 0);
    dl_rule_head_const_typed(&a, 1, ray_sym_intern("sym", 3), RAY_SYM);
    int ab = dl_rule_add_atom(&a, "src", 1);
    dl_body_set_var(&a, ab, 0, 0);
    a.n_vars = 1;
    TEST_ASSERT((dl_add_rule(prog, &a)) >= (0), "dl_add_rule(prog, &a) >= 0");

    /* Rule B: (tag ?x 3.14) — conflicting head-const type for slot 1. */
    dl_rule_t b; dl_rule_init(&b, "tag", 2);
    dl_rule_head_var(&b, 0, 0);
    dl_rule_head_const_f64(&b, 1, 3.14);
    int bb = dl_rule_add_atom(&b, "src", 1);
    dl_body_set_var(&b, bb, 0, 0);
    b.n_vars = 1;
    TEST_ASSERT((dl_add_rule(prog, &b)) >= (0), "dl_add_rule(prog, &b) >= 0");

    /* Conflict must surface as dl_eval == -1 (no stderr print). */
    TEST_ASSERT_EQ_I(dl_eval(prog), -1);

    dl_program_free(prog);
    ray_release(src); ray_release(xc);
    PASS();
}

/* Surface syntax round-trip for head constants: (rule (foo "a" ?x) ...) */
static test_result_t test_rule_head_const_surface_syntax(void) {
    /* Register a one-row EDB: src(1).  Then declare a global rule that
     * writes (foo "a" ?x) :- (src ?x) and drive a query rule through
     * ray_eval_str so the surface parser is exercised. */
    ray_t* r = ray_eval_str(
        "(rule (foo \"a\" ?x) (src ?x))"
    );
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);

    PASS();
}

/* Surface syntax also must accept string constants in BODY positions so
 * that (not (mark "seen")) and (kind ?x "small") parse cleanly.  This
 * mirrors the Phase B rule shape used by ray-exomem. */
static test_result_t test_rule_body_const_surface_syntax(void) {
    ray_t* r = ray_eval_str(
        "(rule (q ?x) (kind ?x \"small\"))"
    );
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    PASS();
}

/* Grouped MIN: user_min(?u, ?m) :- min(?m, weight_by_user col 1) by (?u, col 0)
 * Data: user 1 -> {50,60}, user 2 -> {75,85}.  Expected: (1,50), (2,75). */
static test_result_t test_agg_min_grouped(void) {
    int64_t users[]   = {1, 1, 2, 2};
    int64_t weights[] = {50, 60, 75, 85};
    ray_t* u_col = ray_vec_from_raw(RAY_I64, users, 4);
    ray_t* w_col = ray_vec_from_raw(RAY_I64, weights, 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("wbu__c0", 7), u_col);
    tbl = ray_table_add_col(tbl, ray_sym_intern("wbu__c1", 7), w_col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "wbu", tbl, 2);

    dl_rule_t r; dl_rule_init(&r, "user_min", 2);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_head_var(&r, 1, 1);
    int idx = dl_rule_add_agg(&r, DL_AGG_MIN, 1, "wbu", 2, 1);
    int key_vars[] = { 0 };
    int key_cols[] = { 0 };
    TEST_ASSERT_EQ_I(dl_rule_agg_set_group(&r, idx, key_vars, key_cols, 1), 0);
    r.n_vars = 2;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "user_min");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 2);

    int64_t* uo = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    int64_t* mo = (int64_t*)ray_data(ray_table_get_col_idx(out, 1));
    int seen_u1 = 0, seen_u2 = 0;
    for (int i = 0; i < 2; i++) {
        if (uo[i] == 1) { TEST_ASSERT_EQ_I((int)mo[i], 50); seen_u1 = 1; }
        else if (uo[i] == 2) { TEST_ASSERT_EQ_I((int)mo[i], 75); seen_u2 = 1; }
    }
    TEST_ASSERT_EQ_I(seen_u1 && seen_u2, 1);

    dl_program_free(prog);
    ray_release(tbl); ray_release(u_col); ray_release(w_col);
    PASS();
}

/* Grouped MAX: Expected: (1,60), (2,85). */
static test_result_t test_agg_max_grouped(void) {
    int64_t users[]   = {1, 1, 2, 2};
    int64_t weights[] = {50, 60, 75, 85};
    ray_t* u_col = ray_vec_from_raw(RAY_I64, users, 4);
    ray_t* w_col = ray_vec_from_raw(RAY_I64, weights, 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("wbu__c0", 7), u_col);
    tbl = ray_table_add_col(tbl, ray_sym_intern("wbu__c1", 7), w_col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "wbu", tbl, 2);

    dl_rule_t r; dl_rule_init(&r, "user_max", 2);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_head_var(&r, 1, 1);
    int idx = dl_rule_add_agg(&r, DL_AGG_MAX, 1, "wbu", 2, 1);
    int key_vars[] = { 0 };
    int key_cols[] = { 0 };
    TEST_ASSERT_EQ_I(dl_rule_agg_set_group(&r, idx, key_vars, key_cols, 1), 0);
    r.n_vars = 2;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "user_max");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 2);

    int64_t* uo = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    int64_t* mo = (int64_t*)ray_data(ray_table_get_col_idx(out, 1));
    int seen_u1 = 0, seen_u2 = 0;
    for (int i = 0; i < 2; i++) {
        if (uo[i] == 1) { TEST_ASSERT_EQ_I((int)mo[i], 60); seen_u1 = 1; }
        else if (uo[i] == 2) { TEST_ASSERT_EQ_I((int)mo[i], 85); seen_u2 = 1; }
    }
    TEST_ASSERT_EQ_I(seen_u1 && seen_u2, 1);

    dl_program_free(prog);
    ray_release(tbl); ray_release(u_col); ray_release(w_col);
    PASS();
}

/* Grouped AVG: Expected: (1, 55.0), (2, 80.0).  AVG promotes to RAY_F64. */
static test_result_t test_agg_avg_grouped(void) {
    int64_t users[]   = {1, 1, 2, 2};
    int64_t weights[] = {50, 60, 75, 85};
    ray_t* u_col = ray_vec_from_raw(RAY_I64, users, 4);
    ray_t* w_col = ray_vec_from_raw(RAY_I64, weights, 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("wbu__c0", 7), u_col);
    tbl = ray_table_add_col(tbl, ray_sym_intern("wbu__c1", 7), w_col);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "wbu", tbl, 2);

    dl_rule_t r; dl_rule_init(&r, "user_avg", 2);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_head_var(&r, 1, 1);
    int idx = dl_rule_add_agg(&r, DL_AGG_AVG, 1, "wbu", 2, 1);
    int key_vars[] = { 0 };
    int key_cols[] = { 0 };
    TEST_ASSERT_EQ_I(dl_rule_agg_set_group(&r, idx, key_vars, key_cols, 1), 0);
    r.n_vars = 2;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "user_avg");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 2);

    int64_t* uo = (int64_t*)ray_data(ray_table_get_col_idx(out, 0));
    ray_t* avg_col = ray_table_get_col_idx(out, 1);
    TEST_ASSERT_EQ_I(avg_col->type, RAY_F64);
    double* ao = (double*)ray_data(avg_col);
    int seen_u1 = 0, seen_u2 = 0;
    for (int i = 0; i < 2; i++) {
        if (uo[i] == 1) { TEST_ASSERT_EQ_F(ao[i], 55.0, 1e-4); seen_u1 = 1; }
        else if (uo[i] == 2) { TEST_ASSERT_EQ_F(ao[i], 80.0, 1e-4); seen_u2 = 1; }
    }
    TEST_ASSERT_EQ_I(seen_u1 && seen_u2, 1);

    dl_program_free(prog);
    ray_release(tbl); ray_release(u_col); ray_release(w_col);
    PASS();
}

/* Scalar SUM/AVG over an RAY_F64 value column.
 * Regression: the scalar aggregate path previously accepted only RAY_I64
 * columns and silently returned 0 for RAY_F64, producing valid-looking but
 * wrong results. */
static test_result_t test_agg_scalar_f64(void) {
    double vs[] = {1.5, 2.5, 3.0, 4.0};  /* sum = 11.0, avg = 2.75 */
    ray_t* vcol = ray_vec_from_raw(RAY_F64, vs, 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("m__c0", 5), vcol);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "m", tbl, 1);

    /* total_sum(?s) :- (sum ?s m 0).  Target var 0, value col 0. */
    dl_rule_t rs; dl_rule_init(&rs, "total_sum", 1);
    dl_rule_head_var(&rs, 0, 0);
    dl_rule_add_agg(&rs, DL_AGG_SUM, 0, "m", 1, 0);
    rs.n_vars = 1;
    dl_add_rule(prog, &rs);

    /* total_avg(?a) :- (avg ?a m 0). */
    dl_rule_t ra; dl_rule_init(&ra, "total_avg", 1);
    dl_rule_head_var(&ra, 0, 0);
    dl_rule_add_agg(&ra, DL_AGG_AVG, 0, "m", 1, 0);
    ra.n_vars = 1;
    dl_add_rule(prog, &ra);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);

    ray_t* s_out = dl_query(prog, "total_sum");
    TEST_ASSERT_NOT_NULL(s_out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(s_out), 1);
    ray_t* s_col = ray_table_get_col_idx(s_out, 0);
    TEST_ASSERT_EQ_I(s_col->type, RAY_F64);
    TEST_ASSERT_EQ_F(((double*)ray_data(s_col))[0], 11.0, 1e-4);

    ray_t* a_out = dl_query(prog, "total_avg");
    TEST_ASSERT_NOT_NULL(a_out);
    ray_t* a_col = ray_table_get_col_idx(a_out, 0);
    TEST_ASSERT_EQ_I(a_col->type, RAY_F64);
    TEST_ASSERT_EQ_F(((double*)ray_data(a_col))[0], 2.75, 1e-4);

    dl_program_free(prog);
    ray_release(tbl); ray_release(vcol);
    PASS();
}

/* Empty-source SUM over an RAY_F64 column must still emit a RAY_F64 result
 * column (value 0.0), not RAY_I64 0.  Regression from the scalar-agg F64
 * fix: is_float was only flipped inside the src_nrows > 0 branch, so an
 * empty f64 SUM fell through to the i64 path. */
static test_result_t test_agg_scalar_f64_sum_empty(void) {
    ray_t* vcol = ray_vec_new(RAY_F64, 0);
    TEST_ASSERT_NOT_NULL(vcol);
    vcol->len = 0;
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("m__c0", 5), vcol);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "m", tbl, 1);

    dl_rule_t rs; dl_rule_init(&rs, "total_sum", 1);
    dl_rule_head_var(&rs, 0, 0);
    dl_rule_add_agg(&rs, DL_AGG_SUM, 0, "m", 1, 0);
    rs.n_vars = 1;
    dl_add_rule(prog, &rs);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "total_sum");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 1);
    ray_t* sc = ray_table_get_col_idx(out, 0);
    TEST_ASSERT_EQ_I(sc->type, RAY_F64);
    TEST_ASSERT_EQ_F(((double*)ray_data(sc))[0], 0.0, 1e-4);

    dl_program_free(prog);
    ray_release(tbl); ray_release(vcol);
    PASS();
}

/* Scalar SUM with an out-of-range value column must be rejected even when
 * the source is empty.  Regression: the bounds check lived inside the
 * src_nrows > 0 branch, so an empty source bypassed it and silently emitted
 * the SUM identity (0 or 0.0) against an invalid column index. */
static test_result_t test_agg_scalar_value_col_oor_empty(void) {
    ray_t* vcol = ray_vec_new(RAY_I64, 0);
    TEST_ASSERT_NOT_NULL(vcol);
    vcol->len = 0;
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("m__c0", 5), vcol);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "m", tbl, 1);

    dl_rule_t rs; dl_rule_init(&rs, "bad_sum", 1);
    dl_rule_head_var(&rs, 0, 0);
    /* value_col=42 is out of range for the arity-1 "m" relation. */
    dl_rule_add_agg(&rs, DL_AGG_SUM, 0, "m", 1, 42);
    rs.n_vars = 1;
    dl_add_rule(prog, &rs);

    TEST_ASSERT_EQ_I(dl_eval(prog), -1);
    ray_t* out = dl_query(prog, "bad_sum");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 0);

    dl_program_free(prog);
    ray_release(tbl); ray_release(vcol);
    PASS();
}

/* Grouped aggregate with an out-of-range group-key column must be rejected
 * cleanly (no crash, no bogus rows).  Regression: the grouped path indexed
 * src_rel->col_names[key_col] without bounds-checking. */
static test_result_t test_agg_grouped_key_col_oor(void) {
    int64_t us[] = {1, 2, 1}, ws[] = {10, 20, 30};
    ray_t* uc = ray_vec_from_raw(RAY_I64, us, 3);
    ray_t* wc = ray_vec_from_raw(RAY_I64, ws, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("wbu__c0", 7), uc);
    tbl = ray_table_add_col(tbl, ray_sym_intern("wbu__c1", 7), wc);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "wbu", tbl, 2);

    dl_rule_t r; dl_rule_init(&r, "bad_group", 2);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_head_var(&r, 1, 1);
    int idx = dl_rule_add_agg(&r, DL_AGG_SUM, 1, "wbu", 2, 1);
    int key_vars[] = { 0 };
    int key_cols[] = { 99 };  /* out-of-range: wbu has arity 2 */
    TEST_ASSERT_EQ_I(dl_rule_agg_set_group(&r, idx, key_vars, key_cols, 1), 0);
    r.n_vars = 2;
    dl_add_rule(prog, &r);

    /* dl_eval must surface the compile-time rejection as failure rather
     * than silently producing an empty result. */
    TEST_ASSERT_EQ_I(dl_eval(prog), -1);
    ray_t* out = dl_query(prog, "bad_group");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 0);

    dl_program_free(prog);
    ray_release(tbl); ray_release(uc); ray_release(wc);
    PASS();
}

/* A rule that passes a narrow-width RAY_SYM body column through a head var
 * must produce a correct SYM column.  Regression: dl_project allocated the
 * destination with ray_vec_new(RAY_SYM, …), which always creates a W64 vec,
 * then memcpy'd using the source's narrower element size — leaving the upper
 * bytes of each W64 slot uninitialized and producing bogus sym IDs when read
 * back. */
static test_result_t test_project_narrow_sym(void) {
    /* Build a W8-width SYM column with 3 distinct sym IDs (all fit in one byte). */
    int64_t ks[] = {7, 11, 13};
    int64_t tag_syms[] = {
        ray_sym_intern("a", 1),
        ray_sym_intern("b", 1),
        ray_sym_intern("c", 1),
    };
    /* Force narrow W8 storage — the fix path only matters when src is narrower
     * than the default W64 ray_vec_new would pick. */
    ray_t* tcol = ray_sym_vec_new(RAY_SYM_W8, 3);
    TEST_ASSERT_NOT_NULL(tcol);
    tcol->len = 3;
    for (int i = 0; i < 3; i++)
        ray_write_sym(ray_data(tcol), i, tag_syms[i], tcol->type, tcol->attrs);
    ray_t* kcol = ray_vec_from_raw(RAY_I64, ks, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("e__c0", 5), kcol);
    tbl = ray_table_add_col(tbl, ray_sym_intern("e__c1", 5), tcol);

    dl_program_t* prog = dl_program_new();
    dl_add_edb(prog, "e", tbl, 2);

    /* out(?k, ?t) :- (e ?k ?t) — passes the narrow-SYM column through. */
    dl_rule_t r; dl_rule_init(&r, "out", 2);
    dl_rule_head_var(&r, 0, 0);
    dl_rule_head_var(&r, 1, 1);
    int bidx = dl_rule_add_atom(&r, "e", 2);
    dl_body_set_var(&r, bidx, 0, 0);
    dl_body_set_var(&r, bidx, 1, 1);
    r.n_vars = 2;
    dl_add_rule(prog, &r);

    TEST_ASSERT_EQ_I(dl_eval(prog), 0);
    ray_t* out = dl_query(prog, "out");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(out), 3);

    ray_t* ok = ray_table_get_col_idx(out, 0);
    ray_t* ot = ray_table_get_col_idx(out, 1);
    TEST_ASSERT_EQ_I(ot->type, RAY_SYM);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ok))[i], ks[i]);
        int64_t got = ray_read_sym(ray_data(ot), i, ot->type, ot->attrs);
        TEST_ASSERT_EQ_I((int)got, (int)tag_syms[i]);
    }

    dl_program_free(prog);
    ray_release(tbl); ray_release(kcol); ray_release(tcol);
    PASS();
}

/* Auto-register env-bound EDB: bind a table as "extra" in the ray env,
 * then run a query whose rule body references "extra" without explicit
 * dl_add_edb — ray_query_fn should auto-discover it.
 *
 * Setup: eav has (1, attr, 100). env has extra(10, 20).
 * Rule: result(?x, ?a) :- (eav ?x ?_ ?_) (extra ?a ?_)
 * Expected: one row (1, 10) — the cross-product constrained to 1 eav row. */
static test_result_t test_env_bound_edb_auto_register(void) {
    /* Build a 1-row EAV table and bind it in the env as "mydb" */
    int64_t es[] = {1}, as[] = {42}, vs[] = {100};
    ray_t* ec = ray_vec_from_raw(RAY_I64, es, 1);
    ray_t* ac = ray_vec_from_raw(RAY_I64, as, 1);
    ray_t* vc = ray_vec_from_raw(RAY_I64, vs, 1);
    ray_t* eav = ray_table_new(3);
    eav = ray_table_add_col(eav, ray_sym_intern("e", 1), ec);
    eav = ray_table_add_col(eav, ray_sym_intern("a", 1), ac);
    eav = ray_table_add_col(eav, ray_sym_intern("v", 1), vc);
    int64_t db_sym = ray_sym_intern("mydb", 4);
    ray_env_set(db_sym, eav);

    /* Build a 1-row "extra" table and bind it in the env */
    int64_t x0[] = {10}, x1[] = {20};
    ray_t* xc0 = ray_vec_from_raw(RAY_I64, x0, 1);
    ray_t* xc1 = ray_vec_from_raw(RAY_I64, x1, 1);
    ray_t* extra = ray_table_new(2);
    extra = ray_table_add_col(extra, ray_sym_intern("extra__c0", 9), xc0);
    extra = ray_table_add_col(extra, ray_sym_intern("extra__c1", 9), xc1);
    int64_t extra_sym = ray_sym_intern("extra", 5);
    ray_env_set(extra_sym, extra);

    /* Run query through ray_eval_str which invokes ray_query_fn internally.
     * "mydb" resolves to the EAV table via env lookup.
     * The rule body references "extra" which is only in the env, not pre-registered
     * as an EDB — ray_query_fn should auto-discover it. */
    ray_t* result = ray_eval_str(
        "(query mydb (find ?x ?a) (where (eav ?x ?p ?v) (extra ?a ?b)))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(result), 1);

    int64_t* r0 = (int64_t*)ray_data(ray_table_get_col_idx(result, 0));
    int64_t* r1 = (int64_t*)ray_data(ray_table_get_col_idx(result, 1));
    TEST_ASSERT_EQ_I((int)r0[0], 1);
    TEST_ASSERT_EQ_I((int)r1[0], 10);

    ray_release(result);
    ray_env_set(extra_sym, NULL);
    ray_env_set(db_sym, NULL);
    ray_release(extra); ray_release(xc0); ray_release(xc1);
    ray_release(eav); ray_release(ec); ray_release(ac); ray_release(vc);
    PASS();
}

/* Auto-register env-bound EDB for an aggregate source whose arity isn't 1.
 * Regression: aggregate parsing used to hardcode pred_arity=1 when prog was
 * NULL (surface syntax, globals), so env_val->ncols != 1 made the env auto-
 * register skip the binding and the query later failed at compile time. */
static test_result_t test_env_bound_agg_auto_register(void) {
    /* EAV (as `mydb`): one row so the primary scan yields a single match. */
    int64_t es[] = {1}, as[] = {42}, vs[] = {100};
    ray_t* ec = ray_vec_from_raw(RAY_I64, es, 1);
    ray_t* ac = ray_vec_from_raw(RAY_I64, as, 1);
    ray_t* vc = ray_vec_from_raw(RAY_I64, vs, 1);
    ray_t* eav = ray_table_new(3);
    eav = ray_table_add_col(eav, ray_sym_intern("e", 1), ec);
    eav = ray_table_add_col(eav, ray_sym_intern("a", 1), ac);
    eav = ray_table_add_col(eav, ray_sym_intern("v", 1), vc);
    int64_t db_sym = ray_sym_intern("mydb", 4);
    ray_env_set(db_sym, eav);

    /* Arity-2 env-bound source table named `salaries`.  The aggregate
     * (sum ?s salaries 1) needs to auto-register this via the env at query
     * time even though the surface-syntax parser didn't know its arity. */
    int64_t sid[] = {1, 2, 3};
    int64_t sal[] = {100, 200, 300};  /* sum = 600 */
    ray_t* sidc = ray_vec_from_raw(RAY_I64, sid, 3);
    ray_t* salc = ray_vec_from_raw(RAY_I64, sal, 3);
    ray_t* salaries = ray_table_new(2);
    salaries = ray_table_add_col(salaries, ray_sym_intern("salaries__c0", 12), sidc);
    salaries = ray_table_add_col(salaries, ray_sym_intern("salaries__c1", 12), salc);
    int64_t sal_sym = ray_sym_intern("salaries", 8);
    ray_env_set(sal_sym, salaries);

    ray_t* result = ray_eval_str(
        "(query mydb (find ?s) (where (total ?s))"
        "  (rules ((total ?s) (sum ?s salaries 1))))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I((int)ray_table_nrows(result), 1);
    ray_t* sc = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sc))[0], 600);

    ray_release(result);
    ray_env_set(sal_sym, NULL);
    ray_env_set(db_sym, NULL);
    ray_release(salaries); ray_release(sidc); ray_release(salc);
    ray_release(eav); ray_release(ec); ray_release(ac); ray_release(vc);
    PASS();
}

/* A rule whose compile step deliberately fails (out-of-range value column
 * on a SUM aggregate) must surface the failure all the way up through
 * (query ...), not silently return an empty table.  Regression: dl_eval
 * used to swallow dl_compile_rule NULL returns and unconditionally report
 * success. */
static test_result_t test_eval_surfaces_compile_failure(void) {
    /* Need an EAV table bound in the env so `(query db …)` has a valid
     * first argument. */
    int64_t es[] = {1}, as[] = {42}, vs[] = {100};
    ray_t* ec = ray_vec_from_raw(RAY_I64, es, 1);
    ray_t* ac = ray_vec_from_raw(RAY_I64, as, 1);
    ray_t* vc = ray_vec_from_raw(RAY_I64, vs, 1);
    ray_t* eav = ray_table_new(3);
    eav = ray_table_add_col(eav, ray_sym_intern("e", 1), ec);
    eav = ray_table_add_col(eav, ray_sym_intern("a", 1), ac);
    eav = ray_table_add_col(eav, ray_sym_intern("v", 1), vc);
    int64_t db_sym = ray_sym_intern("mydb", 4);
    ray_env_set(db_sym, eav);

    /* Env-bound arity-1 source; (sum ?s broken 99) indexes a nonexistent
     * value column. */
    int64_t xs[] = {10, 20, 30};
    ray_t* xc = ray_vec_from_raw(RAY_I64, xs, 3);
    ray_t* broken = ray_table_new(1);
    broken = ray_table_add_col(broken, ray_sym_intern("broken__c0", 10), xc);
    int64_t broken_sym = ray_sym_intern("broken", 6);
    ray_env_set(broken_sym, broken);

    ray_t* result = ray_eval_str(
        "(query mydb (find ?s) (where (bad ?s))"
        "  (rules ((bad ?s) (sum ?s broken 99))))");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    ray_release(result);
    ray_env_set(broken_sym, NULL);
    ray_env_set(db_sym, NULL);
    ray_release(broken); ray_release(xc);
    ray_release(eav); ray_release(ec); ray_release(ac); ray_release(vc);
    PASS();
}

/* ray_release() is a deliberate no-op for RAY_ERROR objects, so callers
 * that claim to be "releasing" an error under the refcount API actually
 * leak the block.  ray_error_free() is the escape hatch that calls
 * ray_free() directly.  This test watches bytes_allocated across a burst
 * of create/free cycles: without a real free the counter would climb. */
static test_result_t test_error_free_reclaims(void) {
    ray_mem_stats_t before, after;
    ray_mem_stats(&before);
    for (int i = 0; i < 256; i++) {
        ray_t* e = ray_error("test", "iter=%d", i);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_TRUE(RAY_IS_ERR(e));
        ray_error_free(e);
    }
    ray_mem_stats(&after);

    /* alloc_count must have grown by at least the 256 we produced. */
    TEST_ASSERT(((size_t)(after.alloc_count - before.alloc_count)) >= (256), "(size_t)(after.alloc_count - before.alloc_count) >= 256");
    /* free_count must have grown by the same amount — if ray_error_free
     * was still a no-op, free_count would lag alloc_count by 256. */
    TEST_ASSERT(((size_t)(after.free_count - before.free_count)) >= (256), "(size_t)(after.free_count - before.free_count) >= 256");
    /* Live-bytes must not have grown — the loop is steady-state. */
    TEST_ASSERT((after.bytes_allocated) <= (before.bytes_allocated), "after.bytes_allocated <= before.bytes_allocated");
    PASS();
}

const test_entry_t datalog_entries[] = {
    { "datalog/source_provenance", test_source_provenance, datalog_setup, datalog_teardown },
    { "datalog/source_prov_requires_flag", test_source_prov_requires_flag, datalog_setup, datalog_teardown },
    { "datalog/cmp_const_filter", test_cmp_const_filter, datalog_setup, datalog_teardown },
    { "datalog/arith_assignment", test_arith_assignment, datalog_setup, datalog_teardown },
    { "datalog/arith_assign_f64", test_arith_assign_f64, datalog_setup, datalog_teardown },
    { "datalog/arith_assign_f64_mixed", test_arith_assign_f64_mixed, datalog_setup, datalog_teardown },
    { "datalog/agg_builder", test_agg_builder, datalog_setup, datalog_teardown },
    { "datalog/agg_stratifies_above_source", test_agg_stratifies_above_source, datalog_setup, datalog_teardown },
    { "datalog/agg_count_edb", test_agg_count_edb, datalog_setup, datalog_teardown },
    { "datalog/agg_sum", test_agg_sum, datalog_setup, datalog_teardown },
    { "datalog/agg_min", test_agg_min, datalog_setup, datalog_teardown },
    { "datalog/agg_max", test_agg_max, datalog_setup, datalog_teardown },
    { "datalog/agg_avg", test_agg_avg, datalog_setup, datalog_teardown },
    { "datalog/agg_min_empty", test_agg_min_empty, datalog_setup, datalog_teardown },
    { "datalog/agg_max_empty", test_agg_max_empty, datalog_setup, datalog_teardown },
    { "datalog/agg_sum_empty", test_agg_sum_empty, datalog_setup, datalog_teardown },
    { "datalog/agg_avg_empty", test_agg_avg_empty, datalog_setup, datalog_teardown },
    { "datalog/agg_count_empty", test_agg_count_empty, datalog_setup, datalog_teardown },
    { "datalog/agg_count_grouped", test_agg_count_grouped, datalog_setup, datalog_teardown },
    { "datalog/agg_sum_grouped", test_agg_sum_grouped, datalog_setup, datalog_teardown },
    { "datalog/agg_min_grouped", test_agg_min_grouped, datalog_setup, datalog_teardown },
    { "datalog/agg_max_grouped", test_agg_max_grouped, datalog_setup, datalog_teardown },
    { "datalog/agg_avg_grouped", test_agg_avg_grouped, datalog_setup, datalog_teardown },
    { "datalog/agg_parse_count_scalar", test_agg_parse_count_scalar, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/agg_parse_sum_scalar", test_agg_parse_sum_scalar, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/agg_parse_count_grouped", test_agg_parse_count_grouped, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/agg_parse_reject_count_with_col", test_agg_parse_reject_count_with_col, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/agg_parse_reject_sum_without_col", test_agg_parse_reject_sum_without_col, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/agg_parse_reject_by_missing_col", test_agg_parse_reject_by_missing_col, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/agg_parse_reject_non_var_target", test_agg_parse_reject_non_var_target, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/between_sugar_parse", test_between_sugar_parse, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/rule_head_const_single_rule", test_rule_head_const_single_rule, datalog_setup, datalog_teardown },
    { "datalog/rule_head_const_i64", test_rule_head_const_i64, datalog_setup, datalog_teardown },
    { "datalog/rule_head_const_cross_idb", test_rule_head_const_cross_idb, datalog_setup, datalog_teardown },
    { "datalog/rule_head_const_f64", test_rule_head_const_f64, datalog_setup, datalog_teardown },
    { "datalog/rule_head_const_with_agg", test_rule_head_const_with_agg, datalog_setup, datalog_teardown },
    { "datalog/rule_head_const_with_negation", test_rule_head_const_with_negation, datalog_setup, datalog_teardown },
    { "datalog/rule_head_const_stratification", test_rule_head_const_stratification, datalog_setup, datalog_teardown },
    { "datalog/rule_head_const_type_conflict", test_rule_head_const_type_conflict, datalog_setup, datalog_teardown },
    { "datalog/rule_head_const_surface_syntax", test_rule_head_const_surface_syntax, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/rule_body_const_surface_syntax", test_rule_body_const_surface_syntax, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/env_bound_edb_auto_register", test_env_bound_edb_auto_register, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/env_bound_agg_auto_register", test_env_bound_agg_auto_register, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/eval_surfaces_compile_failure", test_eval_surfaces_compile_failure, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/error_free_reclaims", test_error_free_reclaims, datalog_rf_setup, datalog_rf_teardown },
    { "datalog/agg_scalar_f64", test_agg_scalar_f64, datalog_setup, datalog_teardown },
    { "datalog/agg_scalar_f64_sum_empty", test_agg_scalar_f64_sum_empty, datalog_setup, datalog_teardown },
    { "datalog/agg_scalar_value_col_oor_empty", test_agg_scalar_value_col_oor_empty, datalog_setup, datalog_teardown },
    { "datalog/agg_grouped_key_col_oor", test_agg_grouped_key_col_oor, datalog_setup, datalog_teardown },
    { "datalog/project_narrow_sym", test_project_narrow_sym, datalog_setup, datalog_teardown },
    { NULL, NULL, NULL, NULL },
};


