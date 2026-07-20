/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

/*
 * test_agg_engine.c — unit tests for the v2 aggregation engine admission
 * gate (agg_v2_can_handle).  These build group DAG nodes via the C API
 * (mirroring test_group_extra.c) and assert the gate verdict directly,
 * without executing the node.
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "ops/agg_engine.h"
#include "ops/agg_acc.h"
#include "ops/agg_registry.h"
#include "ops/rowsel.h"      /* ray_rowsel_from_pred, ray_rowsel_release */
#include "core/pool.h"
#include "table/sym.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* Build a single-column table with the given type, name, n rows = 8. */
static ray_t* make_typed_table(int8_t type, const char* col, int64_t n) {
    ray_t* vec = ray_vec_new(type, n);
    if (!vec || RAY_IS_ERR(vec)) return NULL;
    vec->len = n;
    int64_t name = ray_sym_intern(col, (int32_t)strlen(col));
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);
    return tbl;
}

/* Build a 2-column table: key column "k" of ktype + value column "v" of vtype. */
static ray_t* make_kv_table(int8_t ktype, int8_t vtype, int64_t n) {
    ray_t* kvec = ray_vec_new(ktype, n);
    ray_t* vvec = ray_vec_new(vtype, n);
    if (!kvec || RAY_IS_ERR(kvec) || !vvec || RAY_IS_ERR(vvec)) return NULL;
    kvec->len = n;
    vvec->len = n;
    int64_t kname = ray_sym_intern("k", 1);
    int64_t vname = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, kname, kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, vname, vvec); ray_release(vvec);
    return tbl;
}

/* Case 1: single I64 key + one OP_SUM over an I64 column → admit. */
static test_result_t test_gate_admits_i64_key_sum_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_kv_table(RAY_I64, RAY_I64, 8);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k = ray_scan(g, "k");
    ray_op_t* scan_v = ray_scan(g, "v");
    uint16_t  ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { scan_v };
    ray_op_t* keys[] = { scan_k };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_TRUE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Case 2: two supported-type keys → admit (1b multi-key). */
static test_result_t test_gate_admits_two_keys(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a 3-col table: k1, k2 (I64), v (I64). */
    ray_t* k1 = ray_vec_new(RAY_I64, 8); k1->len = 8;
    ray_t* k2 = ray_vec_new(RAY_I64, 8); k2->len = 8;
    ray_t* vv = ray_vec_new(RAY_I64, 8); vv->len = 8;
    int64_t s_k1 = ray_sym_intern("k1", 2);
    int64_t s_k2 = ray_sym_intern("k2", 2);
    int64_t s_v  = ray_sym_intern("v",  1);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_k1, k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, s_k2, k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, s_v,  vv); ray_release(vv);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k1 = ray_scan(g, "k1");
    ray_op_t* scan_k2 = ray_scan(g, "k2");
    ray_op_t* scan_v  = ray_scan(g, "v");
    uint16_t  ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { scan_v };
    ray_op_t* keys[] = { scan_k1, scan_k2 };
    ray_op_t* grp = ray_group(g, keys, 2, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_TRUE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Case 3: single I64 key + OP_SUM over an I32 column (not registered) → defer. */
static test_result_t test_gate_defers_sum_i32(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_kv_table(RAY_I64, RAY_I32, 8);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k = ray_scan(g, "k");
    ray_op_t* scan_v = ray_scan(g, "v");
    uint16_t  ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { scan_v };
    ray_op_t* keys[] = { scan_k };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_FALSE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Case 4: single I64 key + OP_COUNT → admit. */
static test_result_t test_gate_admits_count(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_typed_table(RAY_I64, "k", 8);
    TEST_ASSERT_NOT_NULL(tbl);

    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_k = ray_scan(g, "k");
    uint16_t  ops[]  = { OP_COUNT };
    ray_op_t* ins[]  = { scan_k };
    ray_op_t* keys[] = { scan_k };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    TEST_ASSERT_TRUE(agg_v2_can_handle(g, grp, tbl));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ── Dense-grouping eligibility selector (agg_dense_plan) ──────────────── */

/* Resolve the streaming SUM(I64) vtable used by most dense-plan cases. */
static const agg_vtable_t* dense_sum_i64_vt(void) {
    return agg_resolve(OP_SUM, RAY_I64);
}

/* (a) single I64 key, values in [0,99] → dense, slots 100, stride 1. */
static test_result_t test_dense_plan_single_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* k = ray_vec_new(RAY_I64, 100); k->len = 100;
    int64_t* kd = (int64_t*)ray_data(k);
    for (int64_t i = 0; i < 100; i++) kd[i] = i;   /* range [0,99] */

    const agg_vtable_t* vt = dense_sum_i64_vt();
    TEST_ASSERT_NOT_NULL(vt);

    ray_t* keys[] = { k };
    const agg_vtable_t* vts[] = { vt };
    dense_plan_t pl = {0};
    bool ok = agg_dense_plan(keys, 1, vts, 1, 100, &pl);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(pl.ok);
    TEST_ASSERT_EQ_I(pl.total_slots, 100);
    TEST_ASSERT_EQ_I(pl.mins[0], 0);
    TEST_ASSERT_EQ_I(pl.ranges[0], 100);
    TEST_ASSERT_EQ_I(pl.strides[0], 1);

    ray_release(k);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (b) two I64 keys, ranges 10 and 10 → dense, slots equal input rows. */
static test_result_t test_dense_plan_two_keys(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* k1 = ray_vec_new(RAY_I64, 100); k1->len = 100;
    ray_t* k2 = ray_vec_new(RAY_I64, 100); k2->len = 100;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    for (int64_t i = 0; i < 100; i++) { d1[i] = i % 10; d2[i] = i / 10; }

    const agg_vtable_t* vt = dense_sum_i64_vt();
    TEST_ASSERT_NOT_NULL(vt);

    ray_t* keys[] = { k1, k2 };
    const agg_vtable_t* vts[] = { vt };
    dense_plan_t pl = {0};
    bool ok = agg_dense_plan(keys, 2, vts, 1, 100, &pl);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(pl.ok);
    TEST_ASSERT_EQ_I(pl.total_slots, 100);
    TEST_ASSERT_EQ_I(pl.ranges[0], 10);
    TEST_ASSERT_EQ_I(pl.ranges[1], 10);
    TEST_ASSERT_EQ_I(pl.strides[0], 1);
    TEST_ASSERT_EQ_I(pl.strides[1], 10);

    ray_release(k1);
    ray_release(k2);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (c) A two-row input cannot justify a 10,000,001-slot dense state. */
static test_result_t test_dense_plan_huge_range(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* k = ray_vec_new(RAY_I64, 2); k->len = 2;
    int64_t* kd = (int64_t*)ray_data(k);
    kd[0] = 0; kd[1] = 10000000;   /* dense state would exceed input size */

    const agg_vtable_t* vt = dense_sum_i64_vt();
    TEST_ASSERT_NOT_NULL(vt);

    ray_t* keys[] = { k };
    const agg_vtable_t* vts[] = { vt };
    dense_plan_t pl = {0};
    bool ok = agg_dense_plan(keys, 1, vts, 1, 2, &pl);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(pl.ok);

    ray_release(k);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (d) F64 key → not dense (unsupported key type). */
static test_result_t test_dense_plan_f64_key(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* k = ray_vec_new(RAY_F64, 8); k->len = 8;
    double* kd = (double*)ray_data(k);
    for (int64_t i = 0; i < 8; i++) kd[i] = (double)i;

    const agg_vtable_t* vt = dense_sum_i64_vt();
    TEST_ASSERT_NOT_NULL(vt);

    ray_t* keys[] = { k };
    const agg_vtable_t* vts[] = { vt };
    dense_plan_t pl = {0};
    bool ok = agg_dense_plan(keys, 1, vts, 1, 8, &pl);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(pl.ok);

    ray_release(k);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (e) a MEDIAN agg (ACC_BUFFERED) over a dense-eligible key → dense IS allowed:
 * the dense serial driver (agg_run_one) and the dense parallel path both carry
 * the per-group destroy lifecycle, so buffered state is handled.  Agg kind no
 * longer gates dense eligibility — only key type/range/null does. */
static test_result_t test_dense_plan_buffered_agg(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* k = ray_vec_new(RAY_I64, 8); k->len = 8;
    int64_t* kd = (int64_t*)ray_data(k);
    for (int64_t i = 0; i < 8; i++) kd[i] = i % 4;   /* small range, dense-eligible key */

    const agg_vtable_t* med = agg_resolve(OP_MEDIAN, RAY_I64);
    TEST_ASSERT_NOT_NULL(med);
    TEST_ASSERT_TRUE(med->kind == ACC_BUFFERED);

    ray_t* keys[] = { k };
    const agg_vtable_t* vts[] = { med };
    dense_plan_t pl = {0};
    bool ok = agg_dense_plan(keys, 1, vts, 1, 8, &pl);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(pl.ok);
    TEST_ASSERT_TRUE(pl.total_slots == 4);   /* key range [0,3] → 4 dense slots */

    ray_release(k);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (f) I64 key with RAY_ATTR_HAS_NULLS → not dense (nullable key). */
static test_result_t test_dense_plan_nullable_key(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* k = ray_vec_new(RAY_I64, 8); k->len = 8;
    int64_t* kd = (int64_t*)ray_data(k);
    for (int64_t i = 0; i < 8; i++) kd[i] = i % 4;
    k->attrs |= RAY_ATTR_HAS_NULLS;

    const agg_vtable_t* vt = dense_sum_i64_vt();
    TEST_ASSERT_NOT_NULL(vt);

    ray_t* keys[] = { k };
    const agg_vtable_t* vts[] = { vt };
    dense_plan_t pl = {0};
    bool ok = agg_dense_plan(keys, 1, vts, 1, 8, &pl);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_FALSE(pl.ok);

    ray_release(k);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Direct unit test for agg_group_keys (single key): first-occurrence dense gid
 * assignment. Key column {5,3,5,3,5,7} → gids {0,1,0,1,0,2},
 * first_row {0,1,5}, ngroups 3. */
static test_result_t test_group_keys_i_first_occurrence(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t src[] = { 5, 3, 5, 3, 5, 7 };
    const int64_t n = (int64_t)(sizeof(src) / sizeof(src[0]));
    ray_t* col = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col));
    col->len = n;
    memcpy(ray_data(col), src, sizeof(src));

    agg_groups_t out;
    TEST_ASSERT_EQ_I((0), (agg_group_keys(&col, 1, n, &out)));

    TEST_ASSERT_EQ_I((3), (out.ngroups));
    TEST_ASSERT_EQ_I((0), (out.first_row[0]));
    TEST_ASSERT_EQ_I((1), (out.first_row[1]));
    TEST_ASSERT_EQ_I((5), (out.first_row[2]));

    const uint32_t expect[] = { 0, 1, 0, 1, 0, 2 };
    for (int64_t i = 0; i < n; i++) {
        TEST_ASSERT_EQ_I(((int)expect[i]), ((int)out.gids[i]));
    }

    agg_groups_free(&out);
    ray_release(col);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Direct unit test exercising the narrow-width path: I32 key column. */
static test_result_t test_group_keys_i_i32(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int32_t src[] = { 100, 200, 100, 300, 200, 100 };
    const int64_t n = (int64_t)(sizeof(src) / sizeof(src[0]));
    ray_t* col = ray_vec_new(RAY_I32, n);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col));
    col->len = n;
    memcpy(ray_data(col), src, sizeof(src));

    agg_groups_t out;
    TEST_ASSERT_EQ_I((0), (agg_group_keys(&col, 1, n, &out)));

    TEST_ASSERT_EQ_I((3), (out.ngroups));
    TEST_ASSERT_EQ_I((0), (out.first_row[0]));
    TEST_ASSERT_EQ_I((1), (out.first_row[1]));
    TEST_ASSERT_EQ_I((3), (out.first_row[2]));

    const uint32_t expect[] = { 0, 1, 0, 2, 1, 0 };
    for (int64_t i = 0; i < n; i++) {
        TEST_ASSERT_EQ_I(((int)expect[i]), ((int)out.gids[i]));
    }

    agg_groups_free(&out);
    ray_release(col);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Direct unit test for agg_run_one: key {1,1,2}, value {10,20,30}.
 * gids {0,0,1}, ngroups 2. SUM={30,30} MIN={10,30} MAX={20,30} COUNT={2,1}. */
static test_result_t test_agg_run_one_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t ksrc[] = { 1, 1, 2 };
    const int64_t vsrc[] = { 10, 20, 30 };
    const int64_t n = 3;

    ray_t* kcol = ray_vec_new(RAY_I64, n);
    ray_t* vcol = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(kcol);
    TEST_ASSERT_NOT_NULL(vcol);
    kcol->len = n; vcol->len = n;
    memcpy(ray_data(kcol), ksrc, sizeof(ksrc));
    memcpy(ray_data(vcol), vsrc, sizeof(vsrc));

    agg_groups_t gr;
    TEST_ASSERT_EQ_I((0), (agg_group_keys(&kcol, 1, n, &gr)));
    TEST_ASSERT_EQ_I((2), (gr.ngroups));

    struct { uint16_t op; ray_t* val; int64_t e0; int64_t e1; } cases[] = {
        { OP_SUM,   vcol, 30, 30 },
        { OP_MIN,   vcol, 10, 30 },
        { OP_MAX,   vcol, 20, 30 },
        { OP_COUNT, NULL,  2,  1 },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const agg_vtable_t* vt = agg_resolve(cases[c].op, RAY_I64);
        TEST_ASSERT_NOT_NULL(vt);
        ray_t* out = agg_run_one(vt, cases[c].val, gr.gids, n, gr.ngroups, 0);
        TEST_ASSERT_NOT_NULL(out);
        TEST_ASSERT_FALSE(RAY_IS_ERR(out));
        TEST_ASSERT_EQ_I((RAY_I64), (out->type));
        TEST_ASSERT_EQ_I((2), ((int)out->len));
        const int64_t* d = (const int64_t*)ray_data(out);
        TEST_ASSERT_EQ_I((cases[c].e0), (d[0]));
        TEST_ASSERT_EQ_I((cases[c].e1), (d[1]));
        ray_release(out);
    }

    agg_groups_free(&gr);
    ray_release(kcol);
    ray_release(vcol);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Direct unit test for agg_group_keys with two I64 keys: tuple-hash grouping.
 * kA={1,1,2,1}, kB={9,9,8,8} → tuples (1,9),(1,9),(2,8),(1,8)
 * → ngroups 3, gids {0,0,1,2}, first_row {0,2,3}. */
static test_result_t test_group_keys_multi(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t a_src[] = { 1, 1, 2, 1 };
    const int64_t b_src[] = { 9, 9, 8, 8 };
    const int64_t n = (int64_t)(sizeof(a_src) / sizeof(a_src[0]));
    ray_t* ka = ray_vec_new(RAY_I64, n);
    ray_t* kb = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(ka);
    TEST_ASSERT_NOT_NULL(kb);
    ka->len = n; kb->len = n;
    memcpy(ray_data(ka), a_src, sizeof(a_src));
    memcpy(ray_data(kb), b_src, sizeof(b_src));

    ray_t* cols[2] = { ka, kb };
    agg_groups_t out;
    TEST_ASSERT_EQ_I((0), (agg_group_keys(cols, 2, n, &out)));

    TEST_ASSERT_EQ_I((3), (out.ngroups));
    const uint32_t expect_gids[] = { 0, 0, 1, 2 };
    for (int64_t i = 0; i < n; i++) {
        TEST_ASSERT_EQ_I(((int)expect_gids[i]), ((int)out.gids[i]));
    }
    TEST_ASSERT_EQ_I((0), (out.first_row[0]));
    TEST_ASSERT_EQ_I((2), (out.first_row[1]));
    TEST_ASSERT_EQ_I((3), (out.first_row[2]));

    agg_groups_free(&out);
    ray_release(ka);
    ray_release(kb);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ──────────────────────────────────────────────────────────────────────
 * End-to-end differential: OP_GROUP executed with the v2 engine OFF then ON
 * must produce byte-equivalent tables.
 *
 * KEY-ORDER: the OLD engine's single-key path emits groups in *sorted*
 * (ascending key) order — the dense-accumulator path iterates DA slots
 * `for (s = 0; s < n_slots; s++)`, and slot = key - key_min, so output is
 * ordered by key value (group.c:6828).  At N=70000 the OLD engine also takes
 * a hash/parallel path which is NOT first-occurrence either.  The v2 engine
 * emits *first-occurrence* order (agg_group_keys).  These orders genuinely
 * differ, so table_expect_equal compares the two results as MULTISETS keyed
 * on the (single) key column: it builds a permutation of each table sorted by
 * the key column's int-coded value, then compares row-by-row through that
 * permutation.  This is the documented parity target — equality of grouped
 * results is order-independent, and sorting both sides by key is the minimal
 * canonical form that both engines agree on.
 * ────────────────────────────────────────────────────────────────────── */

/* Read column cell `i` widened to int64 (key/int columns); F64 read separately. */
static int64_t col_read_i64(ray_t* c, int64_t i) {
    uint8_t esz = ray_sym_elem_size(c->type, c->attrs);
    if (c->type == RAY_SYM)
        return ray_read_sym(ray_data(c), i, c->type, c->attrs);
    int64_t v = 0;
    memcpy(&v, (char*)ray_data(c) + i * esz, esz);
    if      (esz == 4) v = (int32_t)v;
    else if (esz == 2) v = (int16_t)v;
    else if (esz == 1) v = (int8_t)v;
    return v;
}

/* Composite less-than over key columns 0..n_keys-1 of `tbl`: compare column 0,
 * on tie column 1, etc.; final tie broken by original index for stability.
 * Nulls (sentinel-coded) sort by their int-coded value, which is deterministic
 * and identical in both engines, so they canonicalize consistently. */
static bool row_lt_composite(ray_t* tbl, int n_keys, int64_t pa, int64_t pb) {
    for (int c = 0; c < n_keys; c++) {
        ray_t* key = ray_table_get_col_idx(tbl, c);
        int64_t ka = col_read_i64(key, pa);
        int64_t kb = col_read_i64(key, pb);
        if (ka < kb) return true;
        if (ka > kb) return false;
    }
    return pa < pb;  /* stable tie-break */
}

/* Build a row permutation that sorts `tbl` ascending by the COMPOSITE of key
 * columns 0..n_keys-1 (int-coded), stable by original index. Caller frees. */
/* qsort context (the test runner is single-threaded). Group-by results have
 * unique composite key tuples → no ties → qsort's instability is irrelevant. */
static ray_t* g_perm_tbl;
static int    g_perm_nkeys;
static int perm_cmp(const void* pa, const void* pb) {
    int64_t a = *(const int64_t*)pa, b = *(const int64_t*)pb;
    if (row_lt_composite(g_perm_tbl, g_perm_nkeys, a, b)) return -1;
    if (row_lt_composite(g_perm_tbl, g_perm_nkeys, b, a)) return 1;
    return 0;
}

static int64_t* sort_perm_by_keys(ray_t* tbl, int n_keys) {
    int64_t n = ray_table_nrows(tbl);
    int64_t* perm = malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) perm[i] = i;
    /* O(n log n) — n = ngroups reaches ~30000 on high-card radix shapes, where
     * the previous O(n^2) insertion sort dominated the suite (300s+ per test). */
    g_perm_tbl = tbl; g_perm_nkeys = n_keys;
    qsort(perm, (size_t)(n > 0 ? n : 0), sizeof(int64_t), perm_cmp);
    return perm;
}

/* Compare one scalar element a[ia] vs b[ib] (same type assumed). F64 → epsilon;
 * everything else widened to int64. NOT for LIST cells (see list_cell_equal). */
static bool scalar_elem_equal(ray_t* a, int64_t ia, ray_t* b, int64_t ib) {
    bool na = ray_vec_is_null(a, ia), nb = ray_vec_is_null(b, ib);
    if (na != nb) return false;
    if (na) return true;
    if (a->type == RAY_F64) {
        double xa = ((double*)ray_data(a))[ia];
        double xb = ((double*)ray_data(b))[ib];
        return fabs(xa - xb) < 1e-12 || (xa != xa && xb != xb);
    }
    return col_read_i64(a, ia) == col_read_i64(b, ib);
}

/* Compare two LIST cells (each a native-typed vector produced by topk_take_vec:
 * top=desc, bot=asc).  Element ORDER must match exactly — both engines order
 * each cell identically — so compare element-by-element in order, NOT as a set.
 * A NULL slot (empty/absent cell) on one side must be NULL on the other. */
static bool list_cell_equal(ray_t* ca, ray_t* cb) {
    if ((ca == NULL) != (cb == NULL)) return false;
    if (ca == NULL) return true;
    if (RAY_IS_ERR(ca) || RAY_IS_ERR(cb)) return false;
    if (ca->type != cb->type) return false;
    if (ca->len != cb->len) return false;
    for (int64_t e = 0; e < ca->len; e++)
        if (!scalar_elem_equal(ca, e, cb, e)) return false;
    return true;
}

/* Compare one column cell ra[ia] vs rb[ib] (same type asserted by caller).
 * For RAY_LIST columns recurse into the per-cell vectors via list_cell_equal. */
static bool cell_equal(ray_t* a, int64_t ia, ray_t* b, int64_t ib) {
    if (a->type == RAY_LIST)
        return list_cell_equal(ray_list_get(a, ia), ray_list_get(b, ib));
    return scalar_elem_equal(a, ia, b, ib);
}

/* Compare two grouped result tables as multisets keyed on the COMPOSITE of
 * key columns 0..n_keys-1.  The result layout is {key cols [0..n_keys-1],
 * agg cols [n_keys..]}; the composite row-permutation preserves the
 * key<->aggregate association by applying the SAME permutation to all columns. */
static test_result_t table_expect_equal(ray_t* a, ray_t* b, int n_keys) {
    TEST_ASSERT(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b), "valid tables");
    int64_t nca = ray_table_ncols(a), ncb = ray_table_ncols(b);
    TEST_ASSERT_FMT(nca == ncb, "ncols %lld != %lld",
                    (long long)nca, (long long)ncb);
    int64_t nra = ray_table_nrows(a), nrb = ray_table_nrows(b);
    TEST_ASSERT_FMT(nra == nrb, "nrows %lld != %lld",
                    (long long)nra, (long long)nrb);
    for (int64_t c = 0; c < nca; c++) {
        TEST_ASSERT_FMT(ray_table_col_name(a, c) == ray_table_col_name(b, c),
                        "col %lld name sym mismatch", (long long)c);
        ray_t* ca = ray_table_get_col_idx(a, c);
        ray_t* cb = ray_table_get_col_idx(b, c);
        TEST_ASSERT_FMT(ca->type == cb->type, "col %lld type %d != %d",
                        (long long)c, ca->type, cb->type);
    }
    int64_t* pa = sort_perm_by_keys(a, n_keys);
    int64_t* pb = sort_perm_by_keys(b, n_keys);
    test_result_t res = (test_result_t){ TEST_PASS, NULL };
    for (int64_t r = 0; r < nra && res.status == TEST_PASS; r++) {
        for (int64_t c = 0; c < nca; c++) {
            ray_t* ca = ray_table_get_col_idx(a, c);
            ray_t* cb = ray_table_get_col_idx(b, c);
            if (!cell_equal(ca, pa[r], cb, pb[r])) {
                res = (test_result_t){ TEST_FAIL,
                    "cell mismatch (old vs v2) at sorted row/col" };
                break;
            }
        }
    }
    free(pa); free(pb);
    return res;
}

typedef ray_op_t* (*group_builder_t)(ray_graph_t* g);

/* Execute build() with the v2 flag OFF then ON; compare results as multisets
 * over the first `n_keys` key columns. */
static test_result_t diff_group(ray_t* tbl, group_builder_t build, int n_keys) {
    ray_agg_engine_v2 = false;            /* old path */
    ray_graph_t* g1 = ray_graph_new(tbl);
    ray_t* old_r = ray_execute(g1, build(g1));
    if (ray_is_lazy(old_r)) old_r = ray_lazy_materialize(old_r);

    ray_agg_engine_v2 = true;             /* v2 path */
    ray_graph_t* g2 = ray_graph_new(tbl);
    ray_t* new_r = ray_execute(g2, build(g2));
    if (ray_is_lazy(new_r)) new_r = ray_lazy_materialize(new_r);
    /* restore the default (v2 on) so later tests don't inherit a disabled
     * engine — leaving it false caused a cross-test leak. */
    ray_agg_engine_v2 = true;

    test_result_t res = table_expect_equal(old_r, new_r, n_keys);
    ray_release(old_r); ray_release(new_r);
    ray_graph_free(g1); ray_graph_free(g2);
    return res;
}

/* ── WHERE-filtered differential (g->selection set) ─────────────────────
 * The architectural-enabler path: a pushed WHERE filter installs a rowsel on
 * g->selection before OP_GROUP.  We replicate exactly what the planner produces
 * (FILTER → installs g->selection → OP_GROUP) by building a rowsel from a
 * predicate vector and setting it directly, then dispatching OP_GROUP via
 * exec_group with the v2 flag OFF (legacy applies the selection) then ON (the
 * new compact-table prologue applies it).  Both honor g->selection; the results
 * must match as a key-sorted MULTISET.
 *
 * `mask` is a caller-supplied predicate over [0,nrows): mask[i]!=0 ⇒ row i
 * passes.  This deliberately bypasses the fused OP_FILTERED_GROUP op (we call
 * the OP_GROUP executor directly), so even fused-accepted shapes exercise the
 * general FILTER+OP_GROUP→exec_group_v2 selection path.  Empty selection
 * (no row passes) yields a rowsel with total_pass==0 → correct empty result. */
static ray_t* run_group_with_sel(ray_t* tbl, group_builder_t build,
                                 const uint8_t* mask, int64_t nrows, bool v2) {
    ray_agg_engine_v2 = v2;
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* grp = build(g);

    /* Build a RAY_BOOL predicate vec → rowsel.  from_pred returns NULL when
     * ALL rows pass (the "no selection" convention); we keep selection NULL in
     * that case, which both engines treat as full-table. */
    ray_t* pred = ray_vec_new(RAY_BOOL, nrows); pred->len = nrows;
    uint8_t* pd = (uint8_t*)ray_data(pred);
    for (int64_t i = 0; i < nrows; i++) pd[i] = mask[i] ? 1 : 0;
    ray_t* sel = ray_rowsel_from_pred(pred);
    ray_release(pred);
    g->selection = sel;                   /* borrowed: released below */

    ray_t* out = exec_group(g, grp, tbl, 0);
    if (out && ray_is_lazy(out)) out = ray_lazy_materialize(out);

    if (g->selection) { ray_rowsel_release(g->selection); g->selection = NULL; }
    ray_graph_free(g);
    ray_agg_engine_v2 = true;             /* restore default */
    return out;
}

static test_result_t diff_group_sel(ray_t* tbl, group_builder_t build,
                                    const uint8_t* mask, int64_t nrows, int n_keys) {
    ray_t* old_r = run_group_with_sel(tbl, build, mask, nrows, false);
    ray_t* new_r = run_group_with_sel(tbl, build, mask, nrows, true);
    test_result_t res = table_expect_equal(old_r, new_r, n_keys);
    ray_release(old_r); ray_release(new_r);
    return res;
}

/* ── table builders ─────────────────────────────────────────────────── */

/* I64 key column "k" with n rows: keys cycle through `nk` distinct values
 * in a non-sorted, non-trivial pattern. Value column "v" (I64) = i*7 % 101. */
static ray_t* diff_make_i64(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i * 3 + 1) % nk;     /* distinct keys 0..nk-1, scrambled */
        vd[i] = (i * 7) % 101 - 50;   /* spans negatives */
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* I64 key "k" + F64 value "v". */
static ray_t* diff_make_i64_f64(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_F64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    double*  vd = (double*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i * 3 + 1) % nk;
        vd[i] = (double)((i * 7) % 101) * 0.5 - 12.25;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* SYM key "k" + I64 value "v". Keys are interned "g<j>" for j in 0..nk-1. */
static ray_t* diff_make_sym_i64(int64_t n, int64_t nk) {
    ray_t* kvec = ray_sym_vec_new(RAY_SYM_W64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* vd = (int64_t*)ray_data(vvec);
    /* pre-intern the nk group symbols */
    int64_t gid[64];
    for (int64_t j = 0; j < nk; j++) {
        char b[16]; int m = snprintf(b, sizeof(b), "g%lld", (long long)j);
        gid[j] = ray_sym_intern(b, (size_t)m);
    }
    for (int64_t i = 0; i < n; i++) {
        int64_t j = (i * 3 + 1) % nk;
        ray_write_sym(ray_data(kvec), i, (uint64_t)gid[j], RAY_SYM, kvec->attrs);
        vd[i] = (i * 7) % 101 - 50;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* I64 key "k" + I64 value "v" with HAS_NULLS: every 5th value is null. */
static ray_t* diff_make_i64_nulls(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i * 3 + 1) % nk;
        vd[i] = (i * 7) % 101 - 50;
    }
    for (int64_t i = 0; i < n; i += 5) ray_vec_set_null(vvec, i, true);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* ── HIGH-CARDINALITY builders (stress the per-worker→global merge) ──────
 * These deliberately produce MANY groups so each per-worker local hash table
 * holds many groups and Phase B does real merging work. */

/* Single I64 key "k" = i % nk (≈nk groups) + I64 value "v" = i. SUM/COUNT. */
static ray_t* diff_make_i64_hc(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = i % nk;               /* ≈nk distinct groups */
        vd[i] = i - n / 2;            /* deterministic, spans negatives */
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* Single I64 key "k" = i (ALL DISTINCT → n groups, extreme cardinality)
 * + I64 value "v" = i. COUNT. */
static ray_t* diff_make_i64_distinct(int64_t n, int64_t nk) {
    (void)nk;
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) { kd[i] = i; vd[i] = i; }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* Two I64 keys "k1"=i%200, "k2"=(i/200)%150 + I64 value "v" = i. SUM.
 * The two keys vary at independent rates so the pair space is fully covered:
 * ≈30000 composite groups at N=70000 (NOT i%200/i%150, which collapse to 600
 * because lcm(200,150)=600 aligns the two residues). */
static ray_t* diff_make_2i64_hc(int64_t n, int64_t nk) {
    (void)nk;
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = i % 200;
        d2[i] = (i / 200) % 150;
        vd[i] = i - n / 2;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* ── RADIX-FORCING builders (Phase 1c radix) ─────────────────────────────
 * The radix path is selected for high-card STREAMING int/SYM-key queries whose
 * dense representation would require more slots than the contributing input
 * has rows.  These builders give a wide key range so the structural dense-plan
 * rule selects radix; results are compared byte-for-byte (as a key-sorted
 * multiset) with the old engine. */

/* Single I64 key "k" = (i % 50000) * 7: its range needs more dense slots than
 * the input has rows, so it follows the radix representation. */
static ray_t* diff_make_i64_radix(int64_t n, int64_t nk) {
    (void)nk;
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i % 50000) * 7;      /* wide range → dense rejected → radix */
        vd[i] = i - n / 2;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* Two I64 keys with WIDE composite range → dense plan rejected → radix.
 * k1 = (i%4000)*5 (range ≈20000), k2 = ((i/4000)%4000)*5 → product of ranges
 * greatly exceeds the input row count.  There are up to 16M composite slots
 * but only about 17500 live tuples at N=70000. */
static ray_t* diff_make_2i64_radix(int64_t n, int64_t nk) {
    (void)nk;
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i % 4000) * 5;
        d2[i] = ((i / 4000) % 4000) * 5;
        vd[i] = i - n / 2;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* Single I64 key "k" = (i%50000)*7 + min/max value "v" = i*13 % 100003 - 50000.
 * SUM+COUNT+MIN+MAX over a radix-routed key (init-on-first-sight for min/max). */
static ray_t* diff_make_i64_radix_mm(int64_t n, int64_t nk) {
    (void)nk;
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i % 50000) * 7;
        vd[i] = (i * 13) % 100003 - 50000;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* High-card pearson: single I64 key "k" = (i%20000)*17 (wide range → radix),
 * F64 "x","y" varying at different rates so per-group r is finite. */
static ray_t* diff_make_pearson_radix(int64_t n, int64_t nk) {
    (void)nk;
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* xvec = ray_vec_new(RAY_F64, n); xvec->len = n;
    ray_t* yvec = ray_vec_new(RAY_F64, n); yvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    double*  xd = (double*)ray_data(xvec);
    double*  yd = (double*)ray_data(yvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i % 20000) * 17;         /* dense range exceeds input rows */
        xd[i] = (double)((i % 89) * 1.0);
        yd[i] = (double)(((i * 7) % 83) * 1.0);
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xvec); ray_release(xvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), yvec); ray_release(yvec);
    return tbl;
}

/* Q10-shape 6-key tuple (high card → radix): six int/SYM keys k1..k6 + I64 "v".
 * Keys vary at independent rates so the 6-tuple space is broad (≈ thousands of
 * live tuples at N=70000) and the composite range is astronomically larger than
 * the input row count, so radix exercises the 6-key tuple hash/eq. */
static ray_t* diff_make_6keys_radix(int64_t n, int64_t nk) {
    (void)nk;
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I32, n); k2->len = n;
    ray_t* k3 = ray_vec_new(RAY_I16, n); k3->len = n;
    ray_t* k4 = ray_sym_vec_new(RAY_SYM_W64, n); k4->len = n;
    ray_t* k5 = ray_vec_new(RAY_I64, n); k5->len = n;
    ray_t* k6 = ray_vec_new(RAY_I32, n); k6->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int32_t* d2 = (int32_t*)ray_data(k2);
    int16_t* d3 = (int16_t*)ray_data(k3);
    int64_t* d5 = (int64_t*)ray_data(k5);
    int32_t* d6 = (int32_t*)ray_data(k6);
    int64_t* vd = (int64_t*)ray_data(vv);
    int64_t gid[16];
    for (int64_t j = 0; j < 12; j++) {
        char b[16]; int m = snprintf(b, sizeof(b), "s%lld", (long long)j);
        gid[j] = ray_sym_intern(b, (size_t)m);
    }
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 7 + 1) % 13;
        d2[i] = (int32_t)((i * 5 + 2) % 11);
        d3[i] = (int16_t)((i * 3 + 1) % 7);
        ray_write_sym(ray_data(k4), i, (uint64_t)gid[(i * 2 + 1) % 12], RAY_SYM, k4->attrs);
        d5[i] = (i / 4) % 17;
        d6[i] = (int32_t)((i / 8) % 5);
        vd[i] = i - n / 2;
    }
    ray_t* tbl = ray_table_new(7);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k3", 2), k3); ray_release(k3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k4", 2), k4); ray_release(k4);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k5", 2), k5); ray_release(k5);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k6", 2), k6); ray_release(k6);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* ── multi-key table builders ───────────────────────────────────────── */

/* Two I64 keys "k1","k2" + I64 value "v".  Keys vary at different rates so
 * many (k1,k2) tuples exist and some tuples share k1 (composite-sort coverage). */
static ray_t* diff_make_2i64(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 3 + 1) % nk;
        d2[i] = (i * 5 + 2) % (nk + 1);
        vd[i] = (i * 7) % 101 - 50;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* Two I64 keys + F64 value "v" (for AVG/SUM parity). */
static ray_t* diff_make_2i64_f64(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_F64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    double*  vd = (double*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 3 + 1) % nk;
        d2[i] = (i * 5 + 2) % (nk + 1);
        vd[i] = (double)((i * 7) % 101) * 0.5 - 12.25;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* Three keys: I64 "k1", I32 "k2", SYM "k3" + I64 value "v". */
static ray_t* diff_make_3keys(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I32, n); k2->len = n;
    ray_t* k3 = ray_sym_vec_new(RAY_SYM_W64, n); k3->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int32_t* d2 = (int32_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    int64_t gid[64];
    for (int64_t j = 0; j < nk; j++) {
        char b[16]; int m = snprintf(b, sizeof(b), "g%lld", (long long)j);
        gid[j] = ray_sym_intern(b, (size_t)m);
    }
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 3 + 1) % nk;
        d2[i] = (int32_t)((i * 5 + 2) % (nk + 1));
        int64_t j = (i * 2 + 1) % nk;
        ray_write_sym(ray_data(k3), i, (uint64_t)gid[j], RAY_SYM, k3->attrs);
        vd[i] = (i * 7) % 101 - 50;
    }
    ray_t* tbl = ray_table_new(4);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k3", 2), k3); ray_release(k3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* SYM key "k1" + I64 key "k2" + I64 value "v". */
static ray_t* diff_make_sym_i64_keys(int64_t n, int64_t nk) {
    ray_t* k1 = ray_sym_vec_new(RAY_SYM_W64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    int64_t gid[64];
    for (int64_t j = 0; j < nk; j++) {
        char b[16]; int m = snprintf(b, sizeof(b), "g%lld", (long long)j);
        gid[j] = ray_sym_intern(b, (size_t)m);
    }
    for (int64_t i = 0; i < n; i++) {
        int64_t j = (i * 3 + 1) % nk;
        ray_write_sym(ray_data(k1), i, (uint64_t)gid[j], RAY_SYM, k1->attrs);
        d2[i] = (i * 5 + 2) % (nk + 1);
        vd[i] = (i * 7) % 101 - 50;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* Two I64 keys where key column "k2" has HAS_NULLS (sentinel + attr set):
 * every 3rd row's k2 is null → null-key grouping parity. */
static ray_t* diff_make_2i64_keynull(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 3 + 1) % nk;
        d2[i] = (i * 5 + 2) % (nk + 1);
        vd[i] = (i * 7) % 101 - 50;
    }
    for (int64_t i = 0; i < n; i += 3) ray_vec_set_null(k2, i, true);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* ── var/stddev builders: SMALL-range deterministic values ──────────────
 * v = i % 97 keeps per-group variances small so v2's running {sum,sumsq,cnt}
 * formula and the old engine agree to within the comparator's 1e-12 ABSOLUTE
 * F64 epsilon (cell_equal). nk small ⇒ cnt≥2 per group ⇒ sample var/stddev
 * are defined (no singleton-group nulls). */

/* Single I64 key "k" + I64 value "v" = i % 97. */
static ray_t* diff_make_i64_small(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i * 3 + 1) % nk;
        vd[i] = i % 97;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* Single I64 key "k" + F64 value "v" = (i % 97) * 0.25. */
static ray_t* diff_make_i64_f64_small(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_F64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    double*  vd = (double*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i * 3 + 1) % nk;
        vd[i] = (double)(i % 97) * 0.25;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* Two I64 keys "k1","k2" + I64 value "v" = i % 97 (small range). */
static ray_t* diff_make_2i64_small(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 3 + 1) % nk;
        d2[i] = (i * 5 + 2) % (nk + 1);
        vd[i] = i % 97;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* ── pearson (binary-agg) table builders ────────────────────────────────
 * Pearson needs TWO F64 value columns x,y.  The OP_GROUP node is built via
 * the extended builder that populates ext->agg_ins2[a] = y.  Both
 * engines read ext->agg_ins2 for the y-side, so the SAME built node executes
 * pearson under flag-off (old engine) and flag-on (v2).  Data: x,y vary at
 * different rates so per-group correlation is well-defined and varied; each
 * group spans ≥2 rows with non-constant x and y ⇒ dx,dy≠0 ⇒ finite r. */

/* single I64 key "k" + F64 "x" + F64 "y". */
static ray_t* diff_make_pearson_1k(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* xvec = ray_vec_new(RAY_F64, n); xvec->len = n;
    ray_t* yvec = ray_vec_new(RAY_F64, n); yvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    double*  xd = (double*)ray_data(xvec);
    double*  yd = (double*)ray_data(yvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i * 3 + 1) % nk;
        xd[i] = (double)((i % 89) * 1.0);
        yd[i] = (double)(((i * 7) % 83) * 1.0);
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xvec); ray_release(xvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), yvec); ray_release(yvec);
    return tbl;
}

/* two I64 keys "k1","k2" + F64 "x" + F64 "y". */
static ray_t* diff_make_pearson_2k(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* xvec = ray_vec_new(RAY_F64, n); xvec->len = n;
    ray_t* yvec = ray_vec_new(RAY_F64, n); yvec->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    double*  xd = (double*)ray_data(xvec);
    double*  yd = (double*)ray_data(yvec);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = (i * 3 + 1) % nk;
        d2[i] = (i * 5 + 2) % (nk + 1);
        xd[i] = (double)((i % 89) * 1.0);
        yd[i] = (double)(((i * 7) % 83) * 1.0);
    }
    ray_t* tbl = ray_table_new(4);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xvec); ray_release(xvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), yvec); ray_release(yvec);
    return tbl;
}

/* ── top_n / bot_n (LIST-cell) table builders ───────────────────────────
 * VARIED group sizes: keys are skewed so that with nk small/large some groups
 * hold FEWER than K rows (→ short LIST cell, no padding) and some hold MANY
 * more (→ K-length cell).  Value range is wide and signed so top (desc) vs
 * bot (asc) orderings are non-trivial, and distinct (i*37 % big) so the kept
 * set + element order are deterministic and the two engines must agree.
 *
 * Group-size skew: key = (i % (1 + i % nk)) keeps group 0 huge and higher
 * groups progressively smaller — but to also produce groups SMALLER than K we
 * use a triangular assignment: row i → group (i*i) % nk, which gives a very
 * uneven histogram (some groups get 1-2 rows, others dozens). */

/* single I64 key "k" (skewed sizes) + I64 value "v" (wide, signed, distinct). */
static ray_t* diff_make_topk_i64(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = ((i * i) + i) % nk;       /* uneven histogram → varied sizes */
        vd[i] = (i * 37) % 9973 - 4986;   /* wide, signed, distinct-ish */
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* single I64 key "k" (skewed sizes) + F64 value "v". */
static ray_t* diff_make_topk_i64_f64(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_F64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    double*  vd = (double*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = ((i * i) + i) % nk;
        vd[i] = (double)((i * 37) % 9973) * 0.25 - 1246.5;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* single I64 key "k" (skewed sizes) + I64 value "v" GLOBALLY DISTINCT (= i*7+1,
 * strictly increasing → no ties anywhere).  Used for K > group-size so each cell
 * is the full sorted group with an UNAMBIGUOUS element order (no tie-break skew
 * between the old radix path and v2). */
static ray_t* diff_make_topk_i64_distinctv(int64_t n, int64_t nk) {
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = ((i * i) + i) % nk;       /* uneven histogram → varied sizes */
        vd[i] = i * 7 + 1;                /* strictly increasing → all distinct */
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* two I64 keys "k1","k2" (skewed composite sizes) + I64 value "v". */
static ray_t* diff_make_topk_2i64(int64_t n, int64_t nk) {
    ray_t* k1 = ray_vec_new(RAY_I64, n); k1->len = n;
    ray_t* k2 = ray_vec_new(RAY_I64, n); k2->len = n;
    ray_t* vv = ray_vec_new(RAY_I64, n); vv->len = n;
    int64_t* d1 = (int64_t*)ray_data(k1);
    int64_t* d2 = (int64_t*)ray_data(k2);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < n; i++) {
        d1[i] = ((i * i) + i) % nk;
        d2[i] = (i * 5 + 2) % (nk + 1);
        vd[i] = (i * 37) % 9973 - 4986;
    }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k1", 2), k1); ray_release(k1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k2", 2), k2); ray_release(k2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v",  1), vv); ray_release(vv);
    return tbl;
}

/* ── per-shape group builders (graph-local) ─────────────────────────── */
static ray_op_t* gb_sum(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 1);
}
static ray_op_t* gb_count(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k");
    uint16_t ops[] = { OP_COUNT }; ray_op_t* ins[] = { k }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 1);
}
static ray_op_t* gb_minmax(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_MIN, OP_MAX };
    ray_op_t* ins[] = { v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 2);
}
static ray_op_t* gb_four(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_COUNT, OP_MIN, OP_MAX };
    ray_op_t* ins[] = { v, v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 4);
}
static ray_op_t* gb_f64_four(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_MIN, OP_MAX, OP_AVG };
    ray_op_t* ins[] = { v, v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 4);
}
static ray_op_t* gb_sum_count(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_COUNT };
    ray_op_t* ins[] = { v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 2);
}
/* ── var/stddev group builders ──────────────────────────────────────── */
static ray_op_t* gb_stddev(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_STDDEV }; ray_op_t* ins[] = { v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 1);
}
static ray_op_t* gb_var_all(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_VAR, OP_VAR_POP, OP_STDDEV, OP_STDDEV_POP };
    ray_op_t* ins[] = { v, v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 4);
}
static ray_op_t* gb_2k_stddev_pop(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_STDDEV_POP }; ray_op_t* ins[] = { v };
    ray_op_t* keys[] = { k1, k2 };
    return ray_group(g, keys, 2, ops, ins, 1);
}
static ray_op_t* gb_sum_stddev_count(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_STDDEV, OP_COUNT };
    ray_op_t* ins[] = { v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 3);
}
static ray_op_t* gb_nulls(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_MIN, OP_MAX };
    ray_op_t* ins[] = { v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 3);
}

/* ── median group builders (ACC_BUFFERED) ───────────────────────────── */
static ray_op_t* gb_median(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_MEDIAN }; ray_op_t* ins[] = { v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 1);
}
static ray_op_t* gb_2k_median(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_MEDIAN }; ray_op_t* ins[] = { v };
    ray_op_t* keys[] = { k1, k2 };
    return ray_group(g, keys, 2, ops, ins, 1);
}
/* heterogeneous: streaming sum + buffered median + streaming count over 1 I64 key
 * (puts a BUFFERED agg alongside streaming ones in the same AoS block). */
static ray_op_t* gb_sum_median_count(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_MEDIAN, OP_COUNT };
    ray_op_t* ins[] = { v, v, v }; ray_op_t* keys[] = { k };
    return ray_group(g, keys, 1, ops, ins, 3);
}

/* ── top_n / bot_n group builders (LIST-cell, ACC_BUFFERED) ──────────────
 * Built via the extended builder so the OP_GROUP node carries ext->agg_k[a]=K.  The OLD
 * engine (flag-off) executes OP_TOP_N/OP_BOT_N on this OP_GROUP node via the
 * LIST-cell path (ray_topk_per_group_buf); v2 (flag-on, gate admits agg_k) via
 * the ACC_BUFFERED top/bot accumulators.  Both emit one LIST-typed output
 * column, cells = native-typed vectors ordered top=desc / bot=asc. */
static ray_op_t* gb_top2(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_TOP_N }; ray_op_t* ins[] = { v };
    int64_t  kk[]  = { 2 };        ray_op_t* keys[] = { k };
    return ray_group_build(g, keys, 1, ops, ins, NULL, kk, 1);
}
static ray_op_t* gb_bot3(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_BOT_N }; ray_op_t* ins[] = { v };
    int64_t  kk[]  = { 3 };        ray_op_t* keys[] = { k };
    return ray_group_build(g, keys, 1, ops, ins, NULL, kk, 1);
}
static ray_op_t* gb_2k_top2(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_TOP_N }; ray_op_t* ins[] = { v };
    int64_t  kk[]  = { 2 };        ray_op_t* keys[] = { k1, k2 };
    return ray_group_build(g, keys, 2, ops, ins, NULL, kk, 1);
}
/* K=100 > every group → each cell = the full sorted group (no padding). */
static ray_op_t* gb_top100(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_TOP_N }; ray_op_t* ins[] = { v };
    int64_t  kk[]  = { 100 };      ray_op_t* keys[] = { k };
    return ray_group_build(g, keys, 1, ops, ins, NULL, kk, 1);
}
static ray_op_t* gb_top2048(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_TOP_N }; ray_op_t* ins[] = { v };
    int64_t  kk[]  = { 2048 };     ray_op_t* keys[] = { k };
    return ray_group_build(g, keys, 1, ops, ins, NULL, kk, 1);
}
/* heterogeneous: sum + top2 (LIST col) + count, single I64 key. */
static ray_op_t* gb_sum_top2_count(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k"); ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_TOP_N, OP_COUNT };
    ray_op_t* ins[] = { v, v, v };
    int64_t   kk[]  = { 0, 2, 0 };
    ray_op_t* keys[] = { k };
    return ray_group_build(g, keys, 1, ops, ins, NULL, kk, 3);
}

/* ── multi-key group builders ───────────────────────────────────────── */
static ray_op_t* gb_2k_sum(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { v };
    ray_op_t* keys[] = { k1, k2 };
    return ray_group(g, keys, 2, ops, ins, 1);
}
static ray_op_t* gb_2k_four(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM, OP_COUNT, OP_MIN, OP_MAX };
    ray_op_t* ins[] = { v, v, v, v }; ray_op_t* keys[] = { k1, k2 };
    return ray_group(g, keys, 2, ops, ins, 4);
}
static ray_op_t* gb_3k_count(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* k3 = ray_scan(g, "k3");
    uint16_t ops[] = { OP_COUNT }; ray_op_t* ins[] = { k1 };
    ray_op_t* keys[] = { k1, k2, k3 };
    return ray_group(g, keys, 3, ops, ins, 1);
}
/* Q10-shape: six keys k1..k6, single SUM(v). */
static ray_op_t* gb_6k_sum(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* k3 = ray_scan(g, "k3"); ray_op_t* k4 = ray_scan(g, "k4");
    ray_op_t* k5 = ray_scan(g, "k5"); ray_op_t* k6 = ray_scan(g, "k6");
    ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_SUM }; ray_op_t* ins[] = { v };
    ray_op_t* keys[] = { k1, k2, k3, k4, k5, k6 };
    return ray_group(g, keys, 6, ops, ins, 1);
}
static ray_op_t* gb_2k_avg(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* v = ray_scan(g, "v");
    uint16_t ops[] = { OP_AVG }; ray_op_t* ins[] = { v };
    ray_op_t* keys[] = { k1, k2 };
    return ray_group(g, keys, 2, ops, ins, 1);
}

/* ── pearson (binary-agg) group builders: use the extended builder to set agg_ins2 ── */
static ray_op_t* gb_pearson_1k(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k");
    ray_op_t* x = ray_scan(g, "x"); ray_op_t* y = ray_scan(g, "y");
    uint16_t  ops[]  = { OP_PEARSON_CORR };
    ray_op_t* ins[]  = { x };
    ray_op_t* ins2[] = { y };
    ray_op_t* keys[] = { k };
    return ray_group_build(g, keys, 1, ops, ins, ins2, NULL, 1);
}
static ray_op_t* gb_pearson_2k(ray_graph_t* g) {
    ray_op_t* k1 = ray_scan(g, "k1"); ray_op_t* k2 = ray_scan(g, "k2");
    ray_op_t* x = ray_scan(g, "x"); ray_op_t* y = ray_scan(g, "y");
    uint16_t  ops[]  = { OP_PEARSON_CORR };
    ray_op_t* ins[]  = { x };
    ray_op_t* ins2[] = { y };
    ray_op_t* keys[] = { k1, k2 };
    return ray_group_build(g, keys, 2, ops, ins, ins2, NULL, 1);
}
/* heterogeneous: sum(x) + pearson(x,y) + count over single I64 key. */
static ray_op_t* gb_sum_pearson_count(ray_graph_t* g) {
    ray_op_t* k = ray_scan(g, "k");
    ray_op_t* x = ray_scan(g, "x"); ray_op_t* y = ray_scan(g, "y");
    uint16_t  ops[]  = { OP_SUM, OP_PEARSON_CORR, OP_COUNT };
    ray_op_t* ins[]  = { x, x, x };
    ray_op_t* ins2[] = { NULL, y, NULL };
    ray_op_t* keys[] = { k };
    return ray_group_build(g, keys, 1, ops, ins, ins2, NULL, 3);
}

/* ── runner: each shape over a small AND a large (N=70000) table ────── */
#define DIFF_SHAPE(name, maker, builder, nkeys)                           \
    static test_result_t name(void) {                                     \
        ray_heap_init(); (void)ray_sym_init();                            \
        test_result_t r;                                                  \
        ray_t* small = maker(13, 4);                                      \
        r = diff_group(small, builder, (nkeys)); ray_release(small);      \
        if (r.status != TEST_PASS) {                                      \
            ray_sym_destroy(); ray_heap_destroy(); return r; }            \
        ray_t* big = maker(70000, 37);                                    \
        r = diff_group(big, builder, (nkeys)); ray_release(big);          \
        ray_sym_destroy(); ray_heap_destroy();                            \
        return r;                                                         \
    }

/* ── dense-serial differential shapes (small low-card table ONLY) ──────
 * Single small (N<RAY_PARALLEL_THRESHOLD) low-cardinality int-key table → the
 * SERIAL path with a dense plan: agg_group_keys_dense (direct index, no hash).
 * Asserts the dense result is byte-identical to the old hash engine. */
#define DIFF_SHAPE_SMALL(name, maker, builder, nkeys, n, nk)              \
    static test_result_t name(void) {                                    \
        ray_heap_init(); (void)ray_sym_init();                           \
        ray_t* small = maker((n), (nk));                                 \
        test_result_t r = diff_group(small, builder, (nkeys));           \
        ray_release(small);                                              \
        ray_sym_destroy(); ray_heap_destroy();                           \
        return r;                                                        \
    }
/* single I64 key, sum+count+min+max (gb_four), small low-card → dense serial */
DIFF_SHAPE_SMALL(test_dense_serial_i64_four,  diff_make_i64_small,  gb_four,  1, 40, 5)
/* single I64 key, min+max only, small low-card → dense serial */
DIFF_SHAPE_SMALL(test_dense_serial_i64_minmax, diff_make_i64_small, gb_minmax, 1, 40, 5)
/* two I64 keys, sum, small low-card composite → dense serial (mixed-radix) */
DIFF_SHAPE_SMALL(test_dense_serial_2k_sum,    diff_make_2i64_small, gb_2k_sum, 2, 40, 5)

DIFF_SHAPE(test_diff_group_i64_sum,      diff_make_i64,      gb_sum,   1)
DIFF_SHAPE(test_diff_group_i64_count,    diff_make_i64,      gb_count, 1)
DIFF_SHAPE(test_diff_group_i64_minmax,   diff_make_i64,      gb_minmax, 1)
DIFF_SHAPE(test_diff_group_i64_four,     diff_make_i64,      gb_four,  1)
DIFF_SHAPE(test_diff_group_sym_sum,      diff_make_sym_i64,  gb_sum,   1)
DIFF_SHAPE(test_diff_group_f64_four,     diff_make_i64_f64,  gb_f64_four, 1)
DIFF_SHAPE(test_diff_group_nulls_minmax, diff_make_i64_nulls, gb_nulls, 1)
/* review 2.10 sub-item 2: count(v) where v HAS NULLS — pins that the legacy
 * and v2 group engines AGREE, and (given the documented rule "counts include
 * nulls", test/rfl/agg/null_aware.rfl) both count ALL group rows (COUNT(*)
 * semantics), not just live/non-null values.  v2 treats COUNT as input-less
 * (NULL valid mask ⇒ every row counted); legacy increments per row
 * unconditionally.  This differential guards that agreement against regression. */
DIFF_SHAPE(test_diff_group_nulls_count,  diff_make_i64_nulls, gb_sum_count, 1)

/* var/stddev differential shapes (serial small + parallel N=70000) */
DIFF_SHAPE(test_diff_group_i64_stddev,    diff_make_i64_small,     gb_stddev,           1)
DIFF_SHAPE(test_diff_group_i64_var_all,   diff_make_i64_small,     gb_var_all,          1)
DIFF_SHAPE(test_diff_group_f64_stddev,    diff_make_i64_f64_small, gb_stddev,           1)
DIFF_SHAPE(test_diff_group_2k_stddev_pop, diff_make_2i64_small,    gb_2k_stddev_pop,    2)
DIFF_SHAPE(test_diff_group_mixed_stddev,  diff_make_i64_small,     gb_sum_stddev_count, 1)

/* ── median differential shapes (serial small + parallel N=70000) ──────
 * ACC_BUFFERED median: small table exercises the SERIAL buffered lifecycle
 * (init → update → finalize → destroy); N=70000 (≥ RAY_PARALLEL_THRESHOLD)
 * forces the PARALLEL path (per-worker local buffers → merge/concatenate →
 * global finalize → destroy). The data builders give VARIED group sizes —
 * both EVEN and ODD counts — so even-count median = mean of two middles is
 * exercised. v2 MEDIAN out_type is F64 (matches scalar ray_med_fn). */
DIFF_SHAPE(test_diff_group_i64_median,   diff_make_i64,       gb_median,   1)
DIFF_SHAPE(test_diff_group_f64_median,   diff_make_i64_f64,   gb_median,   1)
DIFF_SHAPE(test_diff_group_2k_median,    diff_make_2i64,      gb_2k_median, 2)
DIFF_SHAPE(test_diff_group_mixed_median, diff_make_i64,       gb_sum_median_count, 1)
DIFF_SHAPE(test_diff_group_nulls_median, diff_make_i64_nulls, gb_median,   1)

/* ── top_n / bot_n LIST-cell differential shapes (serial small + parallel) ──
 * Small (N=13, nk=4) exercises the SERIAL buffered top/bot lifecycle and short
 * cells (groups smaller than K).  N=70000 (≥ RAY_PARALLEL_THRESHOLD) forces the
 * PARALLEL buffered path that assembles the LIST column from per-worker buffers.
 * The comparator compares each LIST cell element-by-element IN ORDER. */
DIFF_SHAPE(test_diff_group_top2,      diff_make_topk_i64,     gb_top2,  1)
DIFF_SHAPE(test_diff_group_bot3_f64,  diff_make_topk_i64_f64, gb_bot3,  1)
DIFF_SHAPE(test_diff_group_2k_top2,   diff_make_topk_2i64,    gb_2k_top2, 2)
DIFF_SHAPE(test_diff_group_mixed_top2, diff_make_topk_i64,    gb_sum_top2_count, 1)

/* K=100 larger than EVERY group → each cell is the full sorted group.
 * Custom runner: use MANY keys (nk huge) at N=70000 so groups stay < 100
 * even on the parallel path.  Small table (N=13) trivially has groups < 100. */
static test_result_t test_diff_group_top100(void) {
    ray_heap_init(); (void)ray_sym_init();
    test_result_t r;
    ray_t* small = diff_make_topk_i64_distinctv(13, 4);
    r = diff_group(small, gb_top100, 1); ray_release(small);
    if (r.status != TEST_PASS) { ray_sym_destroy(); ray_heap_destroy(); return r; }
    /* nk=4000 ⇒ ~17 rows/group average at N=70000, all groups < 100. */
    ray_t* big = diff_make_topk_i64_distinctv(70000, 4000);
    r = diff_group(big, gb_top100, 1); ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* K above the former fixed stack ceiling.  One 3000-row group ensures both
 * engines materialize and compare a full 2048-element typed result cell. */
static test_result_t test_diff_group_top2048(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* tbl = diff_make_topk_i64_distinctv(3000, 1);
    test_result_t r = diff_group(tbl, gb_top2048, 1);
    ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 * WHERE-FILTERED differentials (architectural enabler: g->selection routes
 * through exec_group_v2's compact-table prologue).  Each runs OP_GROUP with a
 * rowsel installed, flag OFF (legacy) vs ON (v2 compact path), and asserts the
 * results match as a key-sorted MULTISET.  Selectivity, key arity, key type
 * (incl. SYM domain), F64 values, fused-rejected aggs (var/median) and the
 * empty selection (0 rows pass) are all covered across serial (N=13) and
 * parallel (N=70000) sizes.
 * ══════════════════════════════════════════════════════════════════════ */

/* Selectivity predicates over row index i (deterministic, table-agnostic). */
static uint8_t sel_high(int64_t i) { return (i % 17 == 0); }   /* ≈6% pass  */
static uint8_t sel_low(int64_t i)  { return (i % 7 != 0); }    /* ≈86% pass */
static uint8_t sel_none(int64_t i) { (void)i; return 0; }      /* 0% pass   */

typedef uint8_t (*sel_pred_t)(int64_t);

/* Build a mask of length n from a predicate; caller frees. */
static uint8_t* sel_mask(sel_pred_t p, int64_t n) {
    uint8_t* m = malloc((size_t)(n > 0 ? n : 1));
    for (int64_t i = 0; i < n; i++) m[i] = p(i);
    return m;
}

/* Run one filtered differential over a freshly-built table.  maker(n,nk) builds
 * the table; builder builds the OP_GROUP; pred selects rows. */
static test_result_t run_sel_case(ray_t* (*maker)(int64_t, int64_t),
                                  int64_t n, int64_t nk, group_builder_t builder,
                                  int n_keys, sel_pred_t pred) {
    ray_t* tbl = maker(n, nk);
    uint8_t* mask = sel_mask(pred, n);
    test_result_t r = diff_group_sel(tbl, builder, mask, n, n_keys);
    free(mask);
    ray_release(tbl);
    return r;
}

/* Generate a test running serial (N=13) + parallel (N=70000) at the three
 * selectivities (high, low, none/empty). */
#define SEL_SHAPE(name, maker, builder, nkeys, nk_small, nk_big)              \
    static test_result_t name(void) {                                        \
        ray_heap_init(); (void)ray_sym_init();                               \
        test_result_t r = { TEST_PASS, NULL };                               \
        sel_pred_t preds[] = { sel_high, sel_low, sel_none };                 \
        for (int p = 0; p < 3 && r.status == TEST_PASS; p++)                  \
            r = run_sel_case(maker, 13, (nk_small), builder, (nkeys), preds[p]); \
        for (int p = 0; p < 3 && r.status == TEST_PASS; p++)                  \
            r = run_sel_case(maker, 70000, (nk_big), builder, (nkeys), preds[p]); \
        ray_sym_destroy(); ray_heap_destroy();                               \
        return r;                                                            \
    }

/* single-key, fused-accepted aggs (sum/count/min/max/avg) */
SEL_SHAPE(test_sel_i64_sum,     diff_make_i64,     gb_sum,    1, 4, 37)
SEL_SHAPE(test_sel_i64_count,   diff_make_i64,     gb_count,  1, 4, 37)
SEL_SHAPE(test_sel_i64_four,    diff_make_i64,     gb_four,   1, 4, 37)
SEL_SHAPE(test_sel_f64_four,    diff_make_i64_f64, gb_f64_four, 1, 4, 37)
/* single-key, fused-REJECTED aggs (var family, median) → general FILTER path */
SEL_SHAPE(test_sel_i64_var,     diff_make_i64,     gb_var_all, 1, 4, 37)
SEL_SHAPE(test_sel_i64_median,  diff_make_i64,     gb_median,  1, 4, 37)
SEL_SHAPE(test_sel_mixed_med,   diff_make_i64,     gb_sum_median_count, 1, 4, 37)
/* SYM key (domain must survive the gather) */
SEL_SHAPE(test_sel_sym_sum,     diff_make_sym_i64, gb_sum,    1, 4, 37)
/* multi-key */
SEL_SHAPE(test_sel_2k_sum,      diff_make_2i64,    gb_2k_sum,  2, 4, 37)
SEL_SHAPE(test_sel_2k_median,   diff_make_2i64,    gb_2k_median, 2, 4, 37)

/* multi-key shapes (Phase 1b) */
DIFF_SHAPE(test_diff_group_2k_sum,    diff_make_2i64,        gb_2k_sum,   2)
DIFF_SHAPE(test_diff_group_2k_four,   diff_make_2i64,        gb_2k_four,  2)
DIFF_SHAPE(test_diff_group_3k_count,  diff_make_3keys,       gb_3k_count, 3)
DIFF_SHAPE(test_diff_group_symk_sum,  diff_make_sym_i64_keys, gb_2k_sum,  2)
DIFF_SHAPE(test_diff_group_2k_avg,    diff_make_2i64_f64,    gb_2k_avg,   2)
DIFF_SHAPE(test_diff_group_2k_keynull, diff_make_2i64_keynull, gb_2k_sum, 2)

/* ══════════════════════════════════════════════════════════════════════
 * HIGH-CARDINALITY differentials (Phase 1c).  These inputs are large enough to
 * exercise the engine's parallel two-phase path; many groups make every local
 * table and the merge do real work. Compared as a key-sorted multiset with the
 * old engine.
 * ══════════════════════════════════════════════════════════════════════ */

#define HC_N 70000

/* Execute build() on tbl with the v2 engine and return the group count
 * (result nrows).  Confirms a shape truly is high-cardinality before we lean
 * on it to stress the parallel merge. */
static int64_t v2_group_count(ray_t* tbl, group_builder_t build) {
    ray_agg_engine_v2 = true;
    ray_graph_t* g = ray_graph_new(tbl);
    ray_t* out = ray_execute(g, build(g));
    if (ray_is_lazy(out)) out = ray_lazy_materialize(out);
    /* leave v2 on (the default) — restoring avoids a cross-test leak. */
    ray_agg_engine_v2 = true;
    int64_t ng = (out && !RAY_IS_ERR(out)) ? ray_table_nrows(out) : -1;
    if (out) ray_release(out);
    ray_graph_free(g);
    return ng;
}

/* ≈20000 groups: k = i % 20000, SUM + COUNT. */
static test_result_t test_diff_group_hc_i64_sumcount(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_i64_hc(HC_N, 20000);
    int64_t ng = v2_group_count(big, gb_sum_count);
    TEST_ASSERT_FMT(ng == 20000, "expected 20000 groups, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_sum_count, 1);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ≈30000 composite groups: (i%200, (i/200)%150), SUM. */
static test_result_t test_diff_group_hc_2k_sum(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_2i64_hc(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_2k_sum);
    TEST_ASSERT_FMT(ng == 30000, "expected 30000 groups, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_2k_sum, 2);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Extreme: k = i (all 70000 distinct), COUNT. */
static test_result_t test_diff_group_hc_distinct_count(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_i64_distinct(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_count);
    TEST_ASSERT_FMT(ng == HC_N, "expected %d groups, got %lld", HC_N, (long long)ng);
    test_result_t r = diff_group(big, gb_count, 1);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 * RADIX differentials (Phase 1c).  Wide key value ranges force the DENSE plan
 * to be rejected so the selector routes these high-card STREAMING int/SYM-key
 * queries to exec_group_v2_parallel_radix (256 disjoint hash partitions, no
 * serial merge).  N=70000 ≥ RAY_PARALLEL_THRESHOLD → parallel.  Compared as a
 * key-sorted MULTISET vs the OLD engine.  The group-count asserts double as the
 * "high-card" guarantee (so dense is genuinely rejected).
 * ══════════════════════════════════════════════════════════════════════ */

/* ≈50000 groups: k=(i%50000)*7, SUM+COUNT → radix. */
static test_result_t test_diff_group_radix_sumcount(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_i64_radix(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_sum_count);
    TEST_ASSERT_FMT(ng == 50000, "expected 50000 groups, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_sum_count, 1);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ≈50000 groups: k=(i%50000)*7, SUM+COUNT+MIN+MAX → radix (min/max init-on-sight). */
static test_result_t test_diff_group_radix_minmax(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_i64_radix_mm(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_four);
    TEST_ASSERT_FMT(ng == 50000, "expected 50000 groups, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_four, 1);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Two I64 keys, wide composite range → radix, SUM. */
static test_result_t test_diff_group_radix_2k_sum(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_2i64_radix(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_2k_sum);
    /* All (i%4000, i/4000) pairs are distinct at N=70000; their composite
     * dense range exceeds the input size, which structurally selects radix. */
    TEST_ASSERT_FMT(ng == 70000, "expected 70000 groups, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_2k_sum, 2);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* High-card PEARSON over a radix-routed single I64 key. */
static test_result_t test_diff_group_radix_pearson(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_pearson_radix(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_pearson_1k);
    TEST_ASSERT_FMT(ng == 20000, "expected 20000 groups, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_pearson_1k, 1);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* Q10-shape 6-key tuple, high card → radix, SUM. */
static test_result_t test_diff_group_radix_6k(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_6keys_radix(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_6k_sum);
    TEST_ASSERT_FMT(ng > 1000, "expected high-card 6-key tuple, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_6k_sum, 6);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 * SPARSE LOW-CARDINALITY differential.
 *
 * Few groups (64) whose key VALUES are wide/sparse: k=(i%64)*100000001 spans
 * 0..~6.3e9, so a dense representation would be superlinear in the input even
 * though cardinality is tiny.  The deterministic sparse-key route must match
 * the old engine exactly for sum/count/min/max (key-sorted multiset).
 * ══════════════════════════════════════════════════════════════════════ */
static ray_t* diff_make_i64_sparse_lowcard(int64_t n, int64_t nk) {
    (void)nk;
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i % 64) * 100000001LL;   /* 64 groups, range 0..~6.3e9 → dense rejected */
        vd[i] = (i * 7) % 9973 - 4986;    /* wide, signed (exercises sum/min/max) */
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* 64 groups, sparse keys, SUM+COUNT+MIN+MAX. */
static test_result_t test_diff_group_smallhash_sparse_lowcard(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_i64_sparse_lowcard(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_four);
    TEST_ASSERT_FMT(ng == 64, "expected 64 groups, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_four, 1);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ── RADIX + BUFFERED differentials (Q8-shape) ───────────────────────────
 * High-card int key (wide range → dense rejected) with a BUFFERED agg
 * (median F64 / top_n LIST).  Routes through exec_group_v2_parallel_radix,
 * which now runs the buffered init/update_batch/finalize/destroy lifecycle and
 * (for top_n) the LIST-column emit.  Compared byte-for-byte vs the OLD hash
 * engine — the new radix-buffered result must match exactly. */

/* Single I64 key "k"=(i%50000)*7 (≈50000 groups → radix) + I64 value "v" wide,
 * signed, distinct-ish (so top ordering is non-trivial). */
static ray_t* diff_make_i64_radix_buf(int64_t n, int64_t nk) {
    (void)nk;
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* vvec = ray_vec_new(RAY_I64, n); vvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    int64_t* vd = (int64_t*)ray_data(vvec);
    for (int64_t i = 0; i < n; i++) {
        kd[i] = (i % 50000) * 7;          /* wide range → dense rejected → radix */
        vd[i] = (i * 37) % 9973 - 4986;   /* wide, signed, distinct-ish */
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vvec); ray_release(vvec);
    return tbl;
}

/* ≈50000 groups, MEDIAN (buffered, F64 out) → radix-buffered scalar emit. */
static test_result_t test_diff_group_radix_median(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_i64_radix_buf(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_median);
    TEST_ASSERT_FMT(ng == 50000, "expected 50000 groups, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_median, 1);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ≈50000 groups, TOP2 (buffered, LIST out) → radix-buffered LIST emit. */
static test_result_t test_diff_group_radix_top2(void) {
    ray_heap_init(); (void)ray_sym_init();
    ray_t* big = diff_make_i64_radix_buf(HC_N, 0);
    int64_t ng = v2_group_count(big, gb_top2);
    TEST_ASSERT_FMT(ng == 50000, "expected 50000 groups, got %lld", (long long)ng);
    test_result_t r = diff_group(big, gb_top2, 1);
    ray_release(big);
    ray_sym_destroy(); ray_heap_destroy();
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 * DETERMINISM: v2's first-occurrence order is "sort groups by min(row index)",
 * claimed worker-count-independent by construction (agg_engine.c Phase A tracks
 * MIN first_row per local group, Phase B keeps the global MIN, Phase C sorts by
 * it).  exec_group_v2_parallel allocates exactly ray_pool_total_workers()
 * per-worker locals and agg_phaseA_fn indexes locals[wid] — so worker count
 * DIRECTLY changes how rows partition into local tables and how the merge runs.
 *
 * The pool worker count IS controllable from a test: ray_pool_destroy() +
 * ray_pool_init(n) reconfigures the singleton (precedent: test_pool.c).
 * ray_pool_total_workers(p) = n_workers + 1 (the calling thread counts), so to
 * get total worker counts {1,2,8} we init with n_workers {0,1,7}.
 *
 * We run the SAME high-card query at N=70000 across those worker counts and
 * assert ALL results carry the SAME GROUPS + VALUES, compared as a MULTISET
 * (table_expect_equal sorts both sides by key first).  Group-by output ROW
 * ORDER is now UNSPECIFIED (callers ORDER BY for order), so it may legitimately
 * vary with worker count — only the {group, value} content must be invariant.
 * A full {1,2,8}-worker CI matrix (design §9 / GAPS) is the proper gate; this
 * single-process version reaches it via pool reinit.
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_diff_group_determinism_workers(void) {
    ray_heap_init(); (void)ray_sym_init();

    const uint32_t worker_cfgs[] = { 0, 1, 7 };  /* total workers 1, 2, 8 */
    const int      ncfg = (int)(sizeof(worker_cfgs) / sizeof(worker_cfgs[0]));
    ray_t* results[3] = { NULL, NULL, NULL };
    test_result_t res = (test_result_t){ TEST_PASS, NULL };

    ray_agg_engine_v2 = true;
    for (int c = 0; c < ncfg; c++) {
        ray_pool_destroy();
        ray_pool_init(worker_cfgs[c]);

        ray_t* tbl = diff_make_i64_hc(HC_N, 20000);
        ray_graph_t* g = ray_graph_new(tbl);
        ray_t* out = ray_execute(g, gb_sum_count(g));
        if (ray_is_lazy(out)) out = ray_lazy_materialize(out);
        results[c] = out;
        ray_graph_free(g);
        ray_release(tbl);
    }
    /* leave v2 on (the default) — restoring avoids a cross-test leak. */
    ray_agg_engine_v2 = true;

    /* Restore default pool for subsequent tests. */
    ray_pool_destroy();
    ray_pool_init(0);

    /* All three must carry the same groups + values (multiset; row order is
     * unspecified and may vary with worker count). gb_sum_count has 1 key col. */
    for (int c = 1; c < ncfg && res.status == TEST_PASS; c++)
        res = table_expect_equal(results[0], results[c], 1);

    for (int c = 0; c < ncfg; c++)
        if (results[c]) ray_release(results[c]);

    ray_sym_destroy(); ray_heap_destroy();
    return res;
}

/* ══════════════════════════════════════════════════════════════════════
 * PEARSON r-vs-r² DISCOVERY + v2/old differential.
 *
 * DISCOVERY: build a single perfectly anti-correlated group — key all same,
 * x={1,2,3,4}, y={4,3,2,1} — and run it flag-OFF (old engine).  Signed r = -1
 * for this data; r² = +1.  Inspect the single result value to settle which
 * the old engine emits, so v2 (signed r) can be confirmed/adjusted to match.
 * ══════════════════════════════════════════════════════════════════════ */
static test_result_t test_pearson_old_engine_r_vs_r2(void) {
    ray_heap_init(); (void)ray_sym_init();

    const int64_t n = 4;
    ray_t* kvec = ray_vec_new(RAY_I64, n); kvec->len = n;
    ray_t* xvec = ray_vec_new(RAY_F64, n); xvec->len = n;
    ray_t* yvec = ray_vec_new(RAY_F64, n); yvec->len = n;
    int64_t* kd = (int64_t*)ray_data(kvec);
    double*  xd = (double*)ray_data(xvec);
    double*  yd = (double*)ray_data(yvec);
    const double xs[] = { 1, 2, 3, 4 };
    const double ys[] = { 4, 3, 2, 1 };
    for (int64_t i = 0; i < n; i++) { kd[i] = 7; xd[i] = xs[i]; yd[i] = ys[i]; }
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kvec); ray_release(kvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("x", 1), xvec); ray_release(xvec);
    tbl = ray_table_add_col(tbl, ray_sym_intern("y", 1), yvec); ray_release(yvec);

    /* Flag OFF → old engine. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_t* out = ray_execute(g, gb_pearson_1k(g));
    if (ray_is_lazy(out)) out = ray_lazy_materialize(out);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I((1), ((int)ray_table_nrows(out)));
    /* result layout {k, pearson}; agg col is last. */
    ray_t* rc = ray_table_get_col_idx(out, ray_table_ncols(out) - 1);
    TEST_ASSERT_EQ_I((RAY_F64), (rc->type));
    double r = ((double*)ray_data(rc))[0];
    /* Anti-correlated: signed r → ≈ -1.0; r² → ≈ +1.0. */
    TEST_ASSERT_FMT(fabs(r - (-1.0)) < 1e-9,
                    "old engine pearson = %.15g (expected signed r = -1.0; "
                    "if ~+1.0 it produces r^2)", r);

    ray_release(out);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* Shape 1: single I64 key, pearson(x,y).  small + N=70000 (parallel). */
DIFF_SHAPE(test_diff_group_pearson_1k, diff_make_pearson_1k, gb_pearson_1k, 1)
/* Shape 2: two I64 keys, pearson(x,y). */
DIFF_SHAPE(test_diff_group_pearson_2k, diff_make_pearson_2k, gb_pearson_2k, 2)
/* Shape 3: heterogeneous sum(x)+pearson(x,y)+count over single I64 key. */
DIFF_SHAPE(test_diff_group_pearson_mixed, diff_make_pearson_1k, gb_sum_pearson_count, 1)

const test_entry_t agg_engine_entries[] = {
    { "pearson_old_engine_r_vs_r2",  test_pearson_old_engine_r_vs_r2, NULL, NULL },
    { "diff_group_pearson_1k",       test_diff_group_pearson_1k,    NULL, NULL },
    { "diff_group_pearson_2k",       test_diff_group_pearson_2k,    NULL, NULL },
    { "diff_group_pearson_mixed",    test_diff_group_pearson_mixed, NULL, NULL },
    { "dense_serial_i64_four",       test_dense_serial_i64_four,   NULL, NULL },
    { "dense_serial_i64_minmax",     test_dense_serial_i64_minmax, NULL, NULL },
    { "dense_serial_2k_sum",         test_dense_serial_2k_sum,     NULL, NULL },
    { "diff_group_i64_sum",          test_diff_group_i64_sum,      NULL, NULL },
    { "diff_group_i64_count",        test_diff_group_i64_count,    NULL, NULL },
    { "diff_group_i64_minmax",       test_diff_group_i64_minmax,   NULL, NULL },
    { "diff_group_i64_four",         test_diff_group_i64_four,     NULL, NULL },
    { "diff_group_sym_sum",          test_diff_group_sym_sum,      NULL, NULL },
    { "diff_group_f64_four",         test_diff_group_f64_four,     NULL, NULL },
    { "diff_group_nulls_minmax",     test_diff_group_nulls_minmax, NULL, NULL },
    { "diff_group_nulls_count",      test_diff_group_nulls_count,  NULL, NULL },
    { "diff_group_i64_median",       test_diff_group_i64_median,    NULL, NULL },
    { "diff_group_f64_median",       test_diff_group_f64_median,    NULL, NULL },
    { "diff_group_2k_median",        test_diff_group_2k_median,     NULL, NULL },
    { "diff_group_mixed_median",     test_diff_group_mixed_median,  NULL, NULL },
    { "diff_group_nulls_median",     test_diff_group_nulls_median,  NULL, NULL },
    { "diff_group_i64_stddev",       test_diff_group_i64_stddev,    NULL, NULL },
    { "diff_group_i64_var_all",      test_diff_group_i64_var_all,   NULL, NULL },
    { "diff_group_f64_stddev",       test_diff_group_f64_stddev,    NULL, NULL },
    { "diff_group_2k_stddev_pop",    test_diff_group_2k_stddev_pop, NULL, NULL },
    { "diff_group_mixed_stddev",     test_diff_group_mixed_stddev,  NULL, NULL },
    { "diff_group_top2",             test_diff_group_top2,         NULL, NULL },
    { "diff_group_bot3_f64",         test_diff_group_bot3_f64,     NULL, NULL },
    { "diff_group_2k_top2",          test_diff_group_2k_top2,      NULL, NULL },
    { "diff_group_mixed_top2",       test_diff_group_mixed_top2,   NULL, NULL },
    { "diff_group_top100",           test_diff_group_top100,       NULL, NULL },
    { "diff_group_top2048",          test_diff_group_top2048,      NULL, NULL },
    { "diff_group_2k_sum",           test_diff_group_2k_sum,       NULL, NULL },
    { "diff_group_2k_four",          test_diff_group_2k_four,      NULL, NULL },
    { "diff_group_3k_count",         test_diff_group_3k_count,     NULL, NULL },
    { "diff_group_symk_sum",         test_diff_group_symk_sum,     NULL, NULL },
    { "diff_group_2k_avg",           test_diff_group_2k_avg,       NULL, NULL },
    { "diff_group_2k_keynull",       test_diff_group_2k_keynull,   NULL, NULL },
    { "diff_group_hc_i64_sumcount",  test_diff_group_hc_i64_sumcount,  NULL, NULL },
    { "diff_group_hc_2k_sum",        test_diff_group_hc_2k_sum,        NULL, NULL },
    { "diff_group_hc_distinct_count", test_diff_group_hc_distinct_count, NULL, NULL },
    { "diff_group_radix_sumcount",   test_diff_group_radix_sumcount,   NULL, NULL },
    { "diff_group_radix_minmax",     test_diff_group_radix_minmax,     NULL, NULL },
    { "diff_group_radix_2k_sum",     test_diff_group_radix_2k_sum,     NULL, NULL },
    { "diff_group_radix_pearson",    test_diff_group_radix_pearson,    NULL, NULL },
    { "diff_group_radix_6k",         test_diff_group_radix_6k,         NULL, NULL },
    { "diff_group_smallhash_sparse_lowcard", test_diff_group_smallhash_sparse_lowcard, NULL, NULL },
    { "diff_group_radix_median",     test_diff_group_radix_median,     NULL, NULL },
    { "diff_group_radix_top2",       test_diff_group_radix_top2,       NULL, NULL },
    { "diff_group_determinism_workers", test_diff_group_determinism_workers, NULL, NULL },
    { "sel_i64_sum",                 test_sel_i64_sum,                 NULL, NULL },
    { "sel_i64_count",               test_sel_i64_count,               NULL, NULL },
    { "sel_i64_four",                test_sel_i64_four,                NULL, NULL },
    { "sel_f64_four",                test_sel_f64_four,                NULL, NULL },
    { "sel_i64_var",                 test_sel_i64_var,                 NULL, NULL },
    { "sel_i64_median",              test_sel_i64_median,              NULL, NULL },
    { "sel_mixed_med",               test_sel_mixed_med,               NULL, NULL },
    { "sel_sym_sum",                 test_sel_sym_sum,                 NULL, NULL },
    { "sel_2k_sum",                  test_sel_2k_sum,                  NULL, NULL },
    { "sel_2k_median",               test_sel_2k_median,               NULL, NULL },
    { "gate_admits_i64_key_sum_i64", test_gate_admits_i64_key_sum_i64, NULL, NULL },
    { "gate_admits_two_keys",        test_gate_admits_two_keys,        NULL, NULL },
    { "gate_defers_sum_i32",         test_gate_defers_sum_i32,         NULL, NULL },
    { "gate_admits_count",           test_gate_admits_count,           NULL, NULL },
    { "dense_plan_single_i64",       test_dense_plan_single_i64,       NULL, NULL },
    { "dense_plan_two_keys",         test_dense_plan_two_keys,         NULL, NULL },
    { "dense_plan_huge_range",       test_dense_plan_huge_range,       NULL, NULL },
    { "dense_plan_f64_key",          test_dense_plan_f64_key,          NULL, NULL },
    { "dense_plan_buffered_agg",     test_dense_plan_buffered_agg,     NULL, NULL },
    { "dense_plan_nullable_key",     test_dense_plan_nullable_key,     NULL, NULL },
    { "group_keys_i_first_occurrence", test_group_keys_i_first_occurrence, NULL, NULL },
    { "group_keys_i_i32",            test_group_keys_i_i32,            NULL, NULL },
    { "group_keys_multi",            test_group_keys_multi,            NULL, NULL },
    { "agg_run_one_i64",             test_agg_run_one_i64,             NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
