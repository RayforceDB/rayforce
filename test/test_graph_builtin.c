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
 * Tests for src/ops/graph_builtin.c — direct C-side coverage of the
 * .graph.* builtin entry points (no Rayfall runtime).  Targets the
 * paths that the lang-level test_graph.c tests do not reach:
 *   - widen_to_i64 for I32/I16/U8/BOOL src/dst columns
 *   - I64 → F64 weight coercion in ray_graph_build_fn
 *   - RAY_STR atom column name (vs RAY_SYM) in arg_to_sym
 *   - graph-handle block-copy detach policy in mem/heap.c
 *   - exhaustive info-dict shape (n_nodes / n_edges / sorted / has_weights)
 *   - free idempotency (second free returns "type" error, no crash)
 *
 * Pattern mirrors test_csr.c: ray_heap_init + ray_sym_init in each test,
 * teardown at the end; no libc malloc anywhere.
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "store/csr.h"
#include "lang/internal.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Build a 3-row I64 edge table: 0->1, 1->2, 2->0.  No weight column. */
static ray_t* make_i64_edge_table(void) {
    int64_t src_data[] = {0, 1, 2};
    int64_t dst_data[] = {1, 2, 0};
    ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, 3);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 3);
    int64_t s = ray_sym_intern("src", 3);
    int64_t d = ray_sym_intern("dst", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s, sv); ray_release(sv);
    tbl = ray_table_add_col(tbl, d, dv); ray_release(dv);
    return tbl;
}

/* Build a 3-row table with I64 src/dst plus a weight column of caller-chosen
 * type.  The wt_type/wt_data parameters drive widen-coercion testing. */
static ray_t* make_weighted_table(int8_t wt_type, void* wt_data, int64_t n) {
    int64_t src_data[] = {0, 1, 2};
    int64_t dst_data[] = {1, 2, 0};
    ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, n);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, n);
    ray_t* wv = ray_vec_from_raw(wt_type, wt_data, n);
    int64_t s_sym = ray_sym_intern("src", 3);
    int64_t d_sym = ray_sym_intern("dst", 3);
    int64_t w_sym = ray_sym_intern("weight", 6);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, s_sym, sv); ray_release(sv);
    tbl = ray_table_add_col(tbl, d_sym, dv); ray_release(dv);
    tbl = ray_table_add_col(tbl, w_sym, wv); ray_release(wv);
    return tbl;
}

/* --------------------------------------------------------------------------
 * 1. ray_graph_build_fn — minimal happy path, no weight column
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_no_weight(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };

    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    TEST_ASSERT_EQ_I(h->type, -RAY_I64);
    TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);
    TEST_ASSERT_TRUE(h->i64 != 0);

    /* releasing the handle invokes the heap finalizer, which frees the
     * underlying ray_rel_t — no leak. */
    ray_release(h);
    ray_release(sym_src);
    ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 2. ray_graph_build_fn — F64 weight column, info reports has_weights:true
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_with_weight(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double w_data[] = {1.5, 2.5, 3.5};
    ray_t* tbl = make_weighted_table(RAY_F64, w_data, 3);

    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* sym_w   = ray_sym(ray_sym_intern("weight", 6));
    ray_t* args[4] = { tbl, sym_src, sym_dst, sym_w };

    ray_t* h = ray_graph_build_fn(args, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);

    ray_t* info = ray_graph_info_fn(h);
    TEST_ASSERT_FALSE(RAY_IS_ERR(info));
    TEST_ASSERT_EQ_I(info->type, RAY_DICT);

    /* has_weights must be true. */
    ray_t* k = ray_sym(ray_sym_intern("has_weights", 11));
    ray_t* v = ray_dict_get(info, k);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQ_I(v->type, -RAY_BOOL);
    TEST_ASSERT_EQ_I(v->b8, 1);
    ray_release(v);
    ray_release(k);

    ray_release(info);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst); ray_release(sym_w);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 3. ray_graph_build_fn — I32 src column exercises widen_to_i64
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_widen_i32(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int32_t src_data[] = {0, 1, 2};
    int64_t dst_data[] = {1, 2, 0};
    ray_t* sv = ray_vec_from_raw(RAY_I32, src_data, 3);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 3);
    int64_t s_sym = ray_sym_intern("src", 3);
    int64_t d_sym = ray_sym_intern("dst", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_sym, sv); ray_release(sv);
    tbl = ray_table_add_col(tbl, d_sym, dv); ray_release(dv);

    ray_t* sym_src = ray_sym(s_sym);
    ray_t* sym_dst = ray_sym(d_sym);
    ray_t* args[3] = { tbl, sym_src, sym_dst };

    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);

    /* Confirm n_nodes derived correctly — should be 3 (max(src/dst) + 1). */
    ray_rel_t* rel = (ray_rel_t*)(uintptr_t)h->i64;
    TEST_ASSERT_EQ_I(rel->fwd.n_nodes, 3);
    TEST_ASSERT_EQ_I(rel->fwd.n_edges, 3);

    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 4. ray_graph_build_fn — I16 / U8 / BOOL src widen paths
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_widen_i16_u8_bool(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* I16 src */
    {
        int16_t src_data[] = {0, 1, 2};
        int16_t dst_data[] = {1, 2, 0};
        ray_t* sv = ray_vec_from_raw(RAY_I16, src_data, 3);
        ray_t* dv = ray_vec_from_raw(RAY_I16, dst_data, 3);
        int64_t s_sym = ray_sym_intern("src", 3);
        int64_t d_sym = ray_sym_intern("dst", 3);
        ray_t* tbl = ray_table_new(2);
        tbl = ray_table_add_col(tbl, s_sym, sv); ray_release(sv);
        tbl = ray_table_add_col(tbl, d_sym, dv); ray_release(dv);
        ray_t* sym_src = ray_sym(s_sym);
        ray_t* sym_dst = ray_sym(d_sym);
        ray_t* args[3] = { tbl, sym_src, sym_dst };
        ray_t* h = ray_graph_build_fn(args, 3);
        TEST_ASSERT_FALSE(RAY_IS_ERR(h));
        TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);
        ray_release(h);
        ray_release(sym_src); ray_release(sym_dst);
        ray_release(tbl);
    }

    /* U8 src */
    {
        uint8_t src_data[] = {0, 1, 2};
        uint8_t dst_data[] = {1, 2, 0};
        ray_t* sv = ray_vec_from_raw(RAY_U8, src_data, 3);
        ray_t* dv = ray_vec_from_raw(RAY_U8, dst_data, 3);
        int64_t s_sym = ray_sym_intern("src", 3);
        int64_t d_sym = ray_sym_intern("dst", 3);
        ray_t* tbl = ray_table_new(2);
        tbl = ray_table_add_col(tbl, s_sym, sv); ray_release(sv);
        tbl = ray_table_add_col(tbl, d_sym, dv); ray_release(dv);
        ray_t* sym_src = ray_sym(s_sym);
        ray_t* sym_dst = ray_sym(d_sym);
        ray_t* args[3] = { tbl, sym_src, sym_dst };
        ray_t* h = ray_graph_build_fn(args, 3);
        TEST_ASSERT_FALSE(RAY_IS_ERR(h));
        TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);
        ray_release(h);
        ray_release(sym_src); ray_release(sym_dst);
        ray_release(tbl);
    }

    /* BOOL src — trivial graph: 0->1, 1->0 (only two distinct nodes). */
    {
        uint8_t src_data[] = {0, 1};
        uint8_t dst_data[] = {1, 0};
        ray_t* sv = ray_vec_from_raw(RAY_BOOL, src_data, 2);
        ray_t* dv = ray_vec_from_raw(RAY_BOOL, dst_data, 2);
        int64_t s_sym = ray_sym_intern("src", 3);
        int64_t d_sym = ray_sym_intern("dst", 3);
        ray_t* tbl = ray_table_new(2);
        tbl = ray_table_add_col(tbl, s_sym, sv); ray_release(sv);
        tbl = ray_table_add_col(tbl, d_sym, dv); ray_release(dv);
        ray_t* sym_src = ray_sym(s_sym);
        ray_t* sym_dst = ray_sym(d_sym);
        ray_t* args[3] = { tbl, sym_src, sym_dst };
        ray_t* h = ray_graph_build_fn(args, 3);
        TEST_ASSERT_FALSE(RAY_IS_ERR(h));
        TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);
        ray_rel_t* rel = (ray_rel_t*)(uintptr_t)h->i64;
        TEST_ASSERT_EQ_I(rel->fwd.n_nodes, 2);
        TEST_ASSERT_EQ_I(rel->fwd.n_edges, 2);
        ray_release(h);
        ray_release(sym_src); ray_release(sym_dst);
        ray_release(tbl);
    }

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 5. arg_to_sym — RAY_STR column name accepted (interned on the fly)
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_str_column_name(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();

    /* Pass column names as RAY_STR atoms instead of RAY_SYM. */
    ray_t* str_src = ray_str("src", 3);
    ray_t* str_dst = ray_str("dst", 3);
    ray_t* args[3] = { tbl, str_src, str_dst };

    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);

    /* Also exercise the empty-string rejection branch (len == 0 → -1). */
    ray_t* str_empty = ray_str("", 0);
    ray_t* bad_args[3] = { tbl, str_empty, str_dst };
    ray_t* bad = ray_graph_build_fn(bad_args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    ray_error_free(bad);

    ray_release(h);
    ray_release(str_src);
    ray_release(str_dst);
    ray_release(str_empty);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 6. ray_graph_info_fn — every key present, exact types, expected values
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_info_dict_shape(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* info = ray_graph_info_fn(h);
    TEST_ASSERT_FALSE(RAY_IS_ERR(info));
    TEST_ASSERT_EQ_I(info->type, RAY_DICT);
    TEST_ASSERT_EQ_I(ray_dict_len(info), 4);

    struct { const char* name; int8_t type; int64_t expect_int; } expected[] = {
        { "n_nodes",     -RAY_I64,  3 },
        { "n_edges",     -RAY_I64,  3 },
        { "sorted",      -RAY_BOOL, 1 },  /* build passes sorted=true */
        { "has_weights", -RAY_BOOL, 0 },  /* no weight col */
    };

    for (size_t i = 0; i < sizeof(expected)/sizeof(expected[0]); i++) {
        ray_t* k = ray_sym(ray_sym_intern(expected[i].name,
                                           strlen(expected[i].name)));
        ray_t* v = ray_dict_get(info, k);
        TEST_ASSERT_FMT(v != NULL, "missing key '%s'", expected[i].name);
        TEST_ASSERT_FMT(v->type == expected[i].type,
                         "key '%s' has wrong type %d", expected[i].name, v->type);
        int64_t got = (v->type == -RAY_I64) ? v->i64 : (int64_t)v->b8;
        TEST_ASSERT_FMT(got == expected[i].expect_int,
                         "key '%s' got %lld expected %lld",
                         expected[i].name, (long long)got,
                         (long long)expected[i].expect_int);
        ray_release(v);
        ray_release(k);
    }

    ray_release(info);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 7. ray_graph_free_fn — second free returns "type" error, no crash
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_free_idempotent(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    /* First free: succeeds, returns null singleton, clears the GRAPH attr. */
    ray_t* r1 = ray_graph_free_fn(h);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_TRUE(RAY_IS_NULL(r1));
    TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) == 0);
    TEST_ASSERT_EQ_I(h->i64, 0);

    /* Second free: graph_unwrap returns NULL → "type" error, no double-free. */
    ray_t* r2 = ray_graph_free_fn(h);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "type");
    ray_error_free(r2);

    /* And info on a freed handle also errors out — exercises the same
     * unwrap-returns-NULL branch from a different entry point. */
    ray_t* r3 = ray_graph_info_fn(h);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "type");
    ray_error_free(r3);

    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 8. handle release — heap finalizer runs, no double-free / no crash
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_handle_in_release(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);
    TEST_ASSERT_TRUE(h->i64 != 0);

    /* Drop the reference: heap.c's finalizer (lines ~533-538) sees the
     * GRAPH attr set, calls ray_rel_free, clears the slot.  ASan would
     * fire on the next test if we leaked or double-freed. */
    ray_release(h);

    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 9. ray_alloc_copy — graph handles do NOT share ownership.
 *     The mem/heap.c retain-on-copy hook must detach the duplicate
 *     (i64=0, GRAPH attr cleared) so a later release can't double-free
 *     the source's CSR.
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_handle_block_copy(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);
    int64_t orig_ptr = h->i64;
    TEST_ASSERT_TRUE(orig_ptr != 0);

    /* Block-copy via the same path used by COW.  Per the policy in
     * src/mem/heap.c (ray_retain_owned_refs, RAY_ATTR_GRAPH branch): the
     * COPY is detached — i64 zeroed, attr cleared — so freeing it does
     * not touch the source's ray_rel_t. */
    ray_t* copy = ray_alloc_copy(h);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQ_I(copy->type, -RAY_I64);
    TEST_ASSERT_TRUE((copy->attrs & RAY_ATTR_GRAPH) == 0);
    TEST_ASSERT_EQ_I(copy->i64, 0);

    /* Source must be unchanged — still owns the graph. */
    TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);
    TEST_ASSERT_EQ_I(h->i64, orig_ptr);

    /* Releasing the copy is a plain block-free (nothing graph-y to clean
     * up since attr was cleared); releasing the source then frees the
     * underlying ray_rel_t exactly once. */
    ray_release(copy);
    TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);  /* still alive */
    TEST_ASSERT_EQ_I(h->i64, orig_ptr);

    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 10. ray_graph_pagerank_fn — direct call, verify {_node, _rank} schema
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_pagerank_direct(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(build_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    /* (.graph.pagerank h) — defaults: 30 iters, damping 0.85. */
    ray_t* pr_args[1] = { h };
    ray_t* result = ray_graph_pagerank_fn(pr_args, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    /* Both column names must resolve and point to the right types. */
    int64_t node_sym = ray_sym_intern("_node", 5);
    int64_t rank_sym = ray_sym_intern("_rank", 5);
    ray_t* node_col = ray_table_get_col(result, node_sym);
    ray_t* rank_col = ray_table_get_col(result, rank_sym);
    TEST_ASSERT_NOT_NULL(node_col);
    TEST_ASSERT_NOT_NULL(rank_col);
    TEST_ASSERT_EQ_I(node_col->type, RAY_I64);
    TEST_ASSERT_EQ_I(rank_col->type, RAY_F64);

    /* Sanity: pagerank vector should sum (almost) to 1.0. */
    double* ranks = (double*)ray_data(rank_col);
    double sum = 0.0;
    for (int64_t i = 0; i < rank_col->len; i++) sum += ranks[i];
    TEST_ASSERT_EQ_F(sum, 1.0, 1e-3);

    /* Rank validation: also exercise the rank check (n=0 → "rank" error). */
    ray_t* err_rank = ray_graph_pagerank_fn(pr_args, 0);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err_rank));
    TEST_ASSERT_STR_EQ(ray_err_code(err_rank), "rank");
    ray_error_free(err_rank);

    ray_release(result);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Bonus: I64 weight column → F64 coercion path (line 250-251 in build).
 *     Confirms the build succeeds with non-F64 weights and reports
 *     has_weights:true through info.
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_widen_i64_weight(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t w_data[] = {1, 2, 3};
    ray_t* tbl = make_weighted_table(RAY_I64, w_data, 3);
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* sym_w   = ray_sym(ray_sym_intern("weight", 6));
    ray_t* args[4] = { tbl, sym_src, sym_dst, sym_w };

    ray_t* h = ray_graph_build_fn(args, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));
    TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);

    ray_t* info = ray_graph_info_fn(h);
    TEST_ASSERT_FALSE(RAY_IS_ERR(info));
    ray_t* k = ray_sym(ray_sym_intern("has_weights", 11));
    ray_t* v = ray_dict_get(info, k);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQ_I(v->b8, 1);
    ray_release(v);
    ray_release(k);
    ray_release(info);

    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst); ray_release(sym_w);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 12. atom_to_i64 — I32 and I16 atom branches (lines 78-79)
 * -------------------------------------------------------------------------- */

static test_result_t test_atom_to_i64_narrow(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a graph using I32 pagerank iter (atom_to_i64 I32 branch) */
    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    /* Pass I32 atom as iter arg to pagerank to exercise atom_to_i64 I32 branch */
    ray_t* iter_i32 = ray_alloc(0);
    iter_i32->type = -RAY_I32;
    iter_i32->i32 = 5;
    ray_t* pr_args[2] = { h, iter_i32 };
    ray_t* result = ray_graph_pagerank_fn(pr_args, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_release(iter_i32);

    /* Pass I16 atom as iter arg to pagerank to exercise atom_to_i64 I16 branch */
    ray_t* iter_i16 = ray_alloc(0);
    iter_i16->type = -RAY_I16;
    iter_i16->i16 = 5;
    ray_t* pr_args2[2] = { h, iter_i16 };
    ray_t* result2 = ray_graph_pagerank_fn(pr_args2, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result2));
    ray_release(result2);
    ray_release(iter_i16);

    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 13. pagerank — bad iter type → "type" error (line 374)
 * -------------------------------------------------------------------------- */

static test_result_t test_pagerank_bad_iter_type(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    /* Pass a boolean as iter — not atom_is_int, should return "type" error */
    ray_t* bool_arg = ray_alloc(0);
    bool_arg->type = -RAY_BOOL;
    bool_arg->b8 = 1;
    ray_t* pr_args[2] = { h, bool_arg };
    ray_t* result = ray_graph_pagerank_fn(pr_args, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    TEST_ASSERT_STR_EQ(ray_err_code(result), "type");
    ray_error_free(result);
    ray_release(bool_arg);

    /* Also test domain error: iter=0 → "domain" */
    ray_t* zero_iter = ray_alloc(0);
    zero_iter->type = -RAY_I64;
    zero_iter->i64 = 0;
    ray_t* pr_args2[2] = { h, zero_iter };
    ray_t* result2 = ray_graph_pagerank_fn(pr_args2, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result2));
    TEST_ASSERT_STR_EQ(ray_err_code(result2), "domain");
    ray_error_free(result2);
    ray_release(zero_iter);

    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 14. ray_graph_cluster_fn — happy path (line 527, fully uncovered)
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_cluster_direct(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(build_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* cl_args[1] = { h };
    ray_t* result = ray_graph_cluster_fn(cl_args, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    ray_release(result);

    /* wrong arity → "rank" error */
    ray_t* bad = ray_graph_cluster_fn(cl_args, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "rank");
    ray_error_free(bad);

    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 15. ray_graph_build_fn — weight column error paths:
 *     a) weight col missing (name error)
 *     b) weight col length mismatch (length error)
 *     c) weight col unsupported type (type error)
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_weight_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* a) weight sym not in table → "name" error */
    {
        ray_t* tbl = make_i64_edge_table();
        ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
        ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
        ray_t* sym_w   = ray_sym(ray_sym_intern("weight", 6)); /* not in table */
        ray_t* args[4] = { tbl, sym_src, sym_dst, sym_w };
        ray_t* r = ray_graph_build_fn(args, 4);
        TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "name");
        ray_error_free(r);
        ray_release(sym_src); ray_release(sym_dst); ray_release(sym_w);
        ray_release(tbl);
    }

    /* b) weight col length mismatch → "length" error */
    {
        /* Make a 3-row edge table but weight col of length 2 */
        int64_t src_data[] = {0, 1, 2};
        int64_t dst_data[] = {1, 2, 0};
        double  w_data[]   = {1.0, 2.0};  /* only 2 rows */
        ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, 3);
        ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 3);
        ray_t* wv = ray_vec_from_raw(RAY_F64, w_data, 2);
        int64_t s_sym = ray_sym_intern("src", 3);
        int64_t d_sym = ray_sym_intern("dst", 3);
        int64_t w_sym = ray_sym_intern("weight", 6);
        ray_t* tbl = ray_table_new(3);
        tbl = ray_table_add_col(tbl, s_sym, sv); ray_release(sv);
        tbl = ray_table_add_col(tbl, d_sym, dv); ray_release(dv);
        tbl = ray_table_add_col(tbl, w_sym, wv); ray_release(wv);
        ray_t* sym_src = ray_sym(s_sym);
        ray_t* sym_dst = ray_sym(d_sym);
        ray_t* sym_w   = ray_sym(w_sym);
        ray_t* args[4] = { tbl, sym_src, sym_dst, sym_w };
        ray_t* r = ray_graph_build_fn(args, 4);
        TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "length");
        ray_error_free(r);
        ray_release(sym_src); ray_release(sym_dst); ray_release(sym_w);
        ray_release(tbl);
    }

    /* c) weight col unsupported type (I16) → "type" error */
    {
        int64_t src_data[] = {0, 1, 2};
        int64_t dst_data[] = {1, 2, 0};
        int16_t w_data[]   = {1, 2, 3};
        ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, 3);
        ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 3);
        ray_t* wv = ray_vec_from_raw(RAY_I16, w_data, 3);
        int64_t s_sym = ray_sym_intern("src", 3);
        int64_t d_sym = ray_sym_intern("dst", 3);
        int64_t w_sym = ray_sym_intern("weight", 6);
        ray_t* tbl = ray_table_new(3);
        tbl = ray_table_add_col(tbl, s_sym, sv); ray_release(sv);
        tbl = ray_table_add_col(tbl, d_sym, dv); ray_release(dv);
        tbl = ray_table_add_col(tbl, w_sym, wv); ray_release(wv);
        ray_t* sym_src = ray_sym(s_sym);
        ray_t* sym_dst = ray_sym(d_sym);
        ray_t* sym_w   = ray_sym(w_sym);
        ray_t* args[4] = { tbl, sym_src, sym_dst, sym_w };
        ray_t* r = ray_graph_build_fn(args, 4);
        TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "type");
        ray_error_free(r);
        ray_release(sym_src); ray_release(sym_dst); ray_release(sym_w);
        ray_release(tbl);
    }

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 16. ray_graph_build_fn — I32 and F32 weight coercion paths (lines 253-258)
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_weight_i32_f32(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* I32 weight column → coerced to F64 */
    {
        int32_t w_data[] = {1, 2, 3};
        ray_t* tbl = make_weighted_table(RAY_I32, w_data, 3);
        ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
        ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
        ray_t* sym_w   = ray_sym(ray_sym_intern("weight", 6));
        ray_t* args[4] = { tbl, sym_src, sym_dst, sym_w };
        ray_t* h = ray_graph_build_fn(args, 4);
        TEST_ASSERT_FALSE(RAY_IS_ERR(h));
        TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);
        ray_t* info = ray_graph_info_fn(h);
        TEST_ASSERT_FALSE(RAY_IS_ERR(info));
        ray_t* k = ray_sym(ray_sym_intern("has_weights", 11));
        ray_t* v = ray_dict_get(info, k);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_EQ_I(v->b8, 1);
        ray_release(v); ray_release(k); ray_release(info);
        ray_release(h);
        ray_release(sym_src); ray_release(sym_dst); ray_release(sym_w);
        ray_release(tbl);
    }

    /* F32 weight column → coerced to F64 */
    {
        float w_data[] = {1.0f, 2.0f, 3.0f};
        ray_t* tbl = make_weighted_table(RAY_F32, w_data, 3);
        ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
        ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
        ray_t* sym_w   = ray_sym(ray_sym_intern("weight", 6));
        ray_t* args[4] = { tbl, sym_src, sym_dst, sym_w };
        ray_t* h = ray_graph_build_fn(args, 4);
        TEST_ASSERT_FALSE(RAY_IS_ERR(h));
        TEST_ASSERT_TRUE((h->attrs & RAY_ATTR_GRAPH) != 0);
        ray_release(h);
        ray_release(sym_src); ray_release(sym_dst); ray_release(sym_w);
        ray_release(tbl);
    }

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 17. ray_graph_build_fn — misc validation error paths
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_validation_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* wrong arity (n < 3) → "rank" error */
    {
        ray_t* tbl = make_i64_edge_table();
        ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
        ray_t* args[2] = { tbl, sym_src };
        ray_t* r = ray_graph_build_fn(args, 2);
        TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "rank");
        ray_error_free(r);
        ray_release(sym_src);
        ray_release(tbl);
    }

    /* not-a-table arg → "type" error */
    {
        ray_t* not_tbl = ray_alloc(0);
        not_tbl->type = -RAY_I64;
        not_tbl->i64 = 42;
        ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
        ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
        ray_t* args[3] = { not_tbl, sym_src, sym_dst };
        ray_t* r = ray_graph_build_fn(args, 3);
        TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "type");
        ray_error_free(r);
        ray_release(sym_src); ray_release(sym_dst);
        ray_release(not_tbl);
    }

    /* arg_to_sym: neither SYM nor STR type → "type" error */
    {
        ray_t* tbl = make_i64_edge_table();
        ray_t* int_arg = ray_alloc(0);
        int_arg->type = -RAY_I64;
        int_arg->i64 = 0;
        ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
        ray_t* args[3] = { tbl, int_arg, sym_dst };
        ray_t* r = ray_graph_build_fn(args, 3);
        TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "type");
        ray_error_free(r);
        ray_release(int_arg); ray_release(sym_dst);
        ray_release(tbl);
    }

    /* src column not found in table → "name" error */
    {
        ray_t* tbl = make_i64_edge_table();
        ray_t* sym_missing = ray_sym(ray_sym_intern("nosuchcol", 9));
        ray_t* sym_dst     = ray_sym(ray_sym_intern("dst", 3));
        ray_t* args[3] = { tbl, sym_missing, sym_dst };
        ray_t* r = ray_graph_build_fn(args, 3);
        TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "name");
        ray_error_free(r);
        ray_release(sym_missing); ray_release(sym_dst);
        ray_release(tbl);
    }

    /* src/dst length mismatch → "length" error */
    {
        int64_t src_data[] = {0, 1, 2};
        int64_t dst_data[] = {1, 2};  /* shorter */
        ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, 3);
        ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 2);
        int64_t s_sym = ray_sym_intern("src", 3);
        int64_t d_sym = ray_sym_intern("dst", 3);
        ray_t* tbl = ray_table_new(2);
        tbl = ray_table_add_col(tbl, s_sym, sv); ray_release(sv);
        tbl = ray_table_add_col(tbl, d_sym, dv); ray_release(dv);
        ray_t* sym_src = ray_sym(s_sym);
        ray_t* sym_dst = ray_sym(d_sym);
        ray_t* args[3] = { tbl, sym_src, sym_dst };
        ray_t* r = ray_graph_build_fn(args, 3);
        TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "length");
        ray_error_free(r);
        ray_release(sym_src); ray_release(sym_dst);
        ray_release(tbl);
    }

    /* widen_to_i64 default branch: F32 src → "type" error */
    {
        float src_data[] = {0.0f, 1.0f, 2.0f};
        int64_t dst_data[] = {1, 2, 0};
        ray_t* sv = ray_vec_from_raw(RAY_F32, src_data, 3);
        ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 3);
        int64_t s_sym = ray_sym_intern("src", 3);
        int64_t d_sym = ray_sym_intern("dst", 3);
        ray_t* tbl = ray_table_new(2);
        tbl = ray_table_add_col(tbl, s_sym, sv); ray_release(sv);
        tbl = ray_table_add_col(tbl, d_sym, dv); ray_release(dv);
        ray_t* sym_src = ray_sym(s_sym);
        ray_t* sym_dst = ray_sym(d_sym);
        ray_t* args[3] = { tbl, sym_src, sym_dst };
        ray_t* r = ray_graph_build_fn(args, 3);
        TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "type");
        ray_error_free(r);
        ray_release(sym_src); ray_release(sym_dst);
        ray_release(tbl);
    }

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 18. ray_graph_dijkstra_fn — weighted graph, all optional-arg branches
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_dijkstra_direct(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double w_data[] = {1.0, 2.0, 3.0};
    ray_t* tbl = make_weighted_table(RAY_F64, w_data, 3);
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* sym_w   = ray_sym(ray_sym_intern("weight", 6));
    ray_t* build_args[4] = { tbl, sym_src, sym_dst, sym_w };
    ray_t* h = ray_graph_build_fn(build_args, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* src_atom = ray_alloc(0);
    src_atom->type = -RAY_I64;
    src_atom->i64 = 0;

    /* single-source (no dst, no max_depth) */
    ray_t* d_args1[2] = { h, src_atom };
    ray_t* r1 = ray_graph_dijkstra_fn(d_args1, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(r1->type, RAY_TABLE);
    ray_release(r1);

    /* with dst=null (nil) → single-source mode */
    ray_t* null_dst = RAY_NULL_OBJ;
    ray_t* d_args2[3] = { h, src_atom, null_dst };
    ray_t* r2 = ray_graph_dijkstra_fn(d_args2, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    ray_release(r2);

    /* with dst=integer */
    ray_t* dst_atom = ray_alloc(0);
    dst_atom->type = -RAY_I64;
    dst_atom->i64 = 2;
    ray_t* d_args3[3] = { h, src_atom, dst_atom };
    ray_t* r3 = ray_graph_dijkstra_fn(d_args3, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r3));
    ray_release(r3);

    /* with max_depth argument */
    ray_t* depth_atom = ray_alloc(0);
    depth_atom->type = -RAY_I64;
    depth_atom->i64 = 10;
    ray_t* d_args4[4] = { h, src_atom, dst_atom, depth_atom };
    ray_t* r4 = ray_graph_dijkstra_fn(d_args4, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r4));
    ray_release(r4);
    ray_release(depth_atom);

    /* wrong arity → "rank" error */
    ray_t* bad = ray_graph_dijkstra_fn(d_args1, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "rank");
    ray_error_free(bad);

    /* no weight column → "schema" error */
    ray_t* noweight_tbl = make_i64_edge_table();
    ray_t* nw_sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* nw_sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* nw_args[3] = { noweight_tbl, nw_sym_src, nw_sym_dst };
    ray_t* nw_h = ray_graph_build_fn(nw_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(nw_h));
    ray_t* nw_src = ray_alloc(0); nw_src->type = -RAY_I64; nw_src->i64 = 0;
    ray_t* dij_nw[2] = { nw_h, nw_src };
    ray_t* schema_err = ray_graph_dijkstra_fn(dij_nw, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(schema_err));
    TEST_ASSERT_STR_EQ(ray_err_code(schema_err), "schema");
    ray_error_free(schema_err);
    ray_release(nw_src);
    ray_release(nw_h);
    ray_release(nw_sym_src); ray_release(nw_sym_dst);
    ray_release(noweight_tbl);

    /* dst type error: non-int, non-null */
    ray_t* str_dst = ray_str("x", 1);
    ray_t* d_type_args[3] = { h, src_atom, str_dst };
    ray_t* type_err = ray_graph_dijkstra_fn(d_type_args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(type_err));
    TEST_ASSERT_STR_EQ(ray_err_code(type_err), "type");
    ray_error_free(type_err);
    ray_release(str_dst);

    ray_release(src_atom); ray_release(dst_atom);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst); ray_release(sym_w);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 19. ray_graph_shortest_path_fn — with and without max_depth
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_shortest_path_direct(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* src_atom = ray_alloc(0); src_atom->type = -RAY_I64; src_atom->i64 = 0;
    ray_t* dst_atom = ray_alloc(0); dst_atom->type = -RAY_I64; dst_atom->i64 = 2;

    /* basic call */
    ray_t* sp_args[3] = { h, src_atom, dst_atom };
    ray_t* r1 = ray_graph_shortest_path_fn(sp_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(r1->type, RAY_TABLE);
    ray_release(r1);

    /* with max_depth */
    ray_t* depth_atom = ray_alloc(0); depth_atom->type = -RAY_I64; depth_atom->i64 = 5;
    ray_t* sp_args2[4] = { h, src_atom, dst_atom, depth_atom };
    ray_t* r2 = ray_graph_shortest_path_fn(sp_args2, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    ray_release(r2);
    ray_release(depth_atom);

    /* wrong arity */
    ray_t* bad = ray_graph_shortest_path_fn(sp_args, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "rank");
    ray_error_free(bad);

    ray_release(src_atom); ray_release(dst_atom);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 20. ray_graph_expand_fn — with direction argument
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_expand_direct(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* src_atom = ray_alloc(0); src_atom->type = -RAY_I64; src_atom->i64 = 0;

    /* basic: forward expand */
    ray_t* ex_args[2] = { h, src_atom };
    ray_t* r1 = ray_graph_expand_fn(ex_args, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    ray_release(r1);

    /* with direction=1 (reverse) */
    ray_t* dir_atom = ray_alloc(0); dir_atom->type = -RAY_I64; dir_atom->i64 = 1;
    ray_t* ex_args2[3] = { h, src_atom, dir_atom };
    ray_t* r2 = ray_graph_expand_fn(ex_args2, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    ray_release(r2);
    ray_release(dir_atom);

    /* domain error: direction=3 */
    ray_t* bad_dir = ray_alloc(0); bad_dir->type = -RAY_I64; bad_dir->i64 = 3;
    ray_t* ex_args3[3] = { h, src_atom, bad_dir };
    ray_t* r3 = ray_graph_expand_fn(ex_args3, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "domain");
    ray_error_free(r3);
    ray_release(bad_dir);

    /* wrong arity */
    ray_t* bad = ray_graph_expand_fn(ex_args, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "rank");
    ray_error_free(bad);

    ray_release(src_atom);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 21. ray_graph_var_expand_fn — with direction and track_path
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_var_expand_direct(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* src_atom  = ray_alloc(0); src_atom->type  = -RAY_I64; src_atom->i64  = 0;
    ray_t* min_atom  = ray_alloc(0); min_atom->type  = -RAY_I64; min_atom->i64  = 1;
    ray_t* max_atom  = ray_alloc(0); max_atom->type  = -RAY_I64; max_atom->i64  = 3;

    /* basic: min=1 max=3 */
    ray_t* ve_args[4] = { h, src_atom, min_atom, max_atom };
    ray_t* r1 = ray_graph_var_expand_fn(ve_args, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    ray_release(r1);

    /* with direction=1 */
    ray_t* dir_atom = ray_alloc(0); dir_atom->type = -RAY_I64; dir_atom->i64 = 1;
    ray_t* ve_args2[5] = { h, src_atom, min_atom, max_atom, dir_atom };
    ray_t* r2 = ray_graph_var_expand_fn(ve_args2, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    ray_release(r2);

    /* with track=true (bool atom) */
    ray_t* track_atom = ray_alloc(0); track_atom->type = -RAY_BOOL; track_atom->b8 = 1;
    ray_t* ve_args3[6] = { h, src_atom, min_atom, max_atom, dir_atom, track_atom };
    ray_t* r3 = ray_graph_var_expand_fn(ve_args3, 6);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r3));
    ray_release(r3);
    ray_release(track_atom);
    ray_release(dir_atom);

    /* domain error: min > max */
    ray_t* big_min = ray_alloc(0); big_min->type = -RAY_I64; big_min->i64 = 5;
    ray_t* ve_args4[4] = { h, src_atom, big_min, max_atom };
    ray_t* r4 = ray_graph_var_expand_fn(ve_args4, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "domain");
    ray_error_free(r4);
    ray_release(big_min);

    /* wrong arity */
    ray_t* bad = ray_graph_var_expand_fn(ve_args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "rank");
    ray_error_free(bad);

    ray_release(src_atom); ray_release(min_atom); ray_release(max_atom);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 22. remaining algorithm happy paths: connected, louvain, degree,
 *     topsort, dfs, betweenness, closeness, mst, random_walk, k_shortest
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_algorithms_coverage(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build unweighted graph for most algorithms */
    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    /* connected components */
    ray_t* cc_args[1] = { h };
    ray_t* r_cc = ray_graph_connected_fn(cc_args, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_cc));
    TEST_ASSERT_EQ_I(r_cc->type, RAY_TABLE);
    ray_release(r_cc);

    /* connected wrong arity */
    ray_t* bad_cc = ray_graph_connected_fn(cc_args, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad_cc));
    ray_error_free(bad_cc);

    /* louvain */
    ray_t* lo_args[1] = { h };
    ray_t* r_lo = ray_graph_louvain_fn(lo_args, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_lo));
    TEST_ASSERT_EQ_I(r_lo->type, RAY_TABLE);
    ray_release(r_lo);

    /* louvain with iter arg */
    ray_t* iter_atom = ray_alloc(0); iter_atom->type = -RAY_I64; iter_atom->i64 = 10;
    ray_t* lo_args2[2] = { h, iter_atom };
    ray_t* r_lo2 = ray_graph_louvain_fn(lo_args2, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_lo2));
    ray_release(r_lo2);
    ray_release(iter_atom);

    /* degree centrality */
    ray_t* deg_args[1] = { h };
    ray_t* r_deg = ray_graph_degree_fn(deg_args, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_deg));
    TEST_ASSERT_EQ_I(r_deg->type, RAY_TABLE);
    ray_release(r_deg);

    /* topsort — use a DAG (0->1, 1->2, 0->2) instead of the cyclic graph */
    {
        int64_t dag_src[] = {0, 1, 0};
        int64_t dag_dst[] = {1, 2, 2};
        ray_t* ds = ray_vec_from_raw(RAY_I64, dag_src, 3);
        ray_t* dd = ray_vec_from_raw(RAY_I64, dag_dst, 3);
        int64_t ss = ray_sym_intern("src", 3);
        int64_t ds2 = ray_sym_intern("dst", 3);
        ray_t* dag_tbl = ray_table_new(2);
        dag_tbl = ray_table_add_col(dag_tbl, ss, ds); ray_release(ds);
        dag_tbl = ray_table_add_col(dag_tbl, ds2, dd); ray_release(dd);
        ray_t* dag_ssym = ray_sym(ss);
        ray_t* dag_dsym = ray_sym(ds2);
        ray_t* dag_args[3] = { dag_tbl, dag_ssym, dag_dsym };
        ray_t* hdag = ray_graph_build_fn(dag_args, 3);
        TEST_ASSERT_FALSE(RAY_IS_ERR(hdag));
        ray_t* ts_args[1] = { hdag };
        ray_t* r_ts = ray_graph_topsort_fn(ts_args, 1);
        TEST_ASSERT_FALSE(RAY_IS_ERR(r_ts));
        ray_release(r_ts);
        ray_release(hdag);
        ray_release(dag_ssym); ray_release(dag_dsym);
        ray_release(dag_tbl);
    }

    /* dfs from node 0 */
    ray_t* src_atom = ray_alloc(0); src_atom->type = -RAY_I64; src_atom->i64 = 0;
    ray_t* dfs_args[2] = { h, src_atom };
    ray_t* r_dfs = ray_graph_dfs_fn(dfs_args, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_dfs));
    TEST_ASSERT_EQ_I(r_dfs->type, RAY_TABLE);
    ray_release(r_dfs);

    /* dfs with max_depth */
    ray_t* depth_atom = ray_alloc(0); depth_atom->type = -RAY_I64; depth_atom->i64 = 5;
    ray_t* dfs_args2[3] = { h, src_atom, depth_atom };
    ray_t* r_dfs2 = ray_graph_dfs_fn(dfs_args2, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_dfs2));
    ray_release(r_dfs2);
    ray_release(depth_atom);
    ray_release(src_atom);

    /* betweenness */
    ray_t* bet_args[1] = { h };
    ray_t* r_bet = ray_graph_betweenness_fn(bet_args, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_bet));
    ray_release(r_bet);

    /* betweenness with sample */
    ray_t* samp = ray_alloc(0); samp->type = -RAY_I64; samp->i64 = 0;
    ray_t* bet_args2[2] = { h, samp };
    ray_t* r_bet2 = ray_graph_betweenness_fn(bet_args2, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_bet2));
    ray_release(r_bet2);
    ray_release(samp);

    /* closeness */
    ray_t* clo_args[1] = { h };
    ray_t* r_clo = ray_graph_closeness_fn(clo_args, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_clo));
    ray_release(r_clo);

    /* random walk */
    ray_t* src2 = ray_alloc(0); src2->type = -RAY_I64; src2->i64 = 0;
    ray_t* rw_args[2] = { h, src2 };
    ray_t* r_rw = ray_graph_random_walk_fn(rw_args, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_rw));
    ray_release(r_rw);

    /* random walk with walk_len */
    ray_t* wlen = ray_alloc(0); wlen->type = -RAY_I64; wlen->i64 = 5;
    ray_t* rw_args2[3] = { h, src2, wlen };
    ray_t* r_rw2 = ray_graph_random_walk_fn(rw_args2, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_rw2));
    ray_release(r_rw2);
    ray_release(wlen);
    ray_release(src2);

    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);

    /* Build weighted graph for mst and k_shortest */
    double w_data[] = {1.0, 2.0, 3.0};
    ray_t* tbl2 = make_weighted_table(RAY_F64, w_data, 3);
    ray_t* s2 = ray_sym(ray_sym_intern("src", 3));
    ray_t* d2 = ray_sym(ray_sym_intern("dst", 3));
    ray_t* w2 = ray_sym(ray_sym_intern("weight", 6));
    ray_t* wargs[4] = { tbl2, s2, d2, w2 };
    ray_t* hw = ray_graph_build_fn(wargs, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(hw));

    /* mst */
    ray_t* mst_args[1] = { hw };
    ray_t* r_mst = ray_graph_mst_fn(mst_args, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_mst));
    ray_release(r_mst);

    /* mst wrong arity → "rank" */
    ray_t* bad_mst = ray_graph_mst_fn(mst_args, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad_mst));
    ray_error_free(bad_mst);

    /* mst no weight → "schema" */
    ray_t* nw_tbl = make_i64_edge_table();
    ray_t* nws = ray_sym(ray_sym_intern("src", 3));
    ray_t* nwd = ray_sym(ray_sym_intern("dst", 3));
    ray_t* nw_bargs[3] = { nw_tbl, nws, nwd };
    ray_t* nw_h = ray_graph_build_fn(nw_bargs, 3);
    ray_t* mst_nw[1] = { nw_h };
    ray_t* mst_schema = ray_graph_mst_fn(mst_nw, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(mst_schema));
    TEST_ASSERT_STR_EQ(ray_err_code(mst_schema), "schema");
    ray_error_free(mst_schema);
    ray_release(nw_h); ray_release(nws); ray_release(nwd); ray_release(nw_tbl);

    /* k_shortest */
    ray_t* ks_src = ray_alloc(0); ks_src->type = -RAY_I64; ks_src->i64 = 0;
    ray_t* ks_dst = ray_alloc(0); ks_dst->type = -RAY_I64; ks_dst->i64 = 2;
    ray_t* ks_k   = ray_alloc(0); ks_k->type   = -RAY_I64; ks_k->i64   = 2;
    ray_t* ks_args[4] = { hw, ks_src, ks_dst, ks_k };
    ray_t* r_ks = ray_graph_k_shortest_fn(ks_args, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_ks));
    ray_release(r_ks);

    /* k_shortest wrong arity → "rank" */
    ray_t* bad_ks = ray_graph_k_shortest_fn(ks_args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad_ks));
    TEST_ASSERT_STR_EQ(ray_err_code(bad_ks), "rank");
    ray_error_free(bad_ks);

    /* k_shortest k=0 → "domain" */
    ray_t* zero_k = ray_alloc(0); zero_k->type = -RAY_I64; zero_k->i64 = 0;
    ray_t* ks_args2[4] = { hw, ks_src, ks_dst, zero_k };
    ray_t* r_ks2 = ray_graph_k_shortest_fn(ks_args2, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r_ks2));
    TEST_ASSERT_STR_EQ(ray_err_code(r_ks2), "domain");
    ray_error_free(r_ks2);
    ray_release(zero_k);

    /* k_shortest no weight → "schema" */
    ray_t* nw_tbl2 = make_i64_edge_table();
    ray_t* nws2 = ray_sym(ray_sym_intern("src", 3));
    ray_t* nwd2 = ray_sym(ray_sym_intern("dst", 3));
    ray_t* nw_bargs2[3] = { nw_tbl2, nws2, nwd2 };
    ray_t* nw_h2 = ray_graph_build_fn(nw_bargs2, 3);
    ray_t* ks_nw[4] = { nw_h2, ks_src, ks_dst, ks_k };
    ray_t* ks_schema = ray_graph_k_shortest_fn(ks_nw, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(ks_schema));
    TEST_ASSERT_STR_EQ(ray_err_code(ks_schema), "schema");
    ray_error_free(ks_schema);
    ray_release(nw_h2); ray_release(nws2); ray_release(nwd2); ray_release(nw_tbl2);

    ray_release(ks_src); ray_release(ks_dst); ray_release(ks_k);
    ray_release(hw);
    ray_release(s2); ray_release(d2); ray_release(w2);
    ray_release(tbl2);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Suite definition
 * -------------------------------------------------------------------------- */

const test_entry_t graph_builtin_entries[] = {
    { "graph_builtin/build_no_weight",      test_graph_build_no_weight,      NULL, NULL },
    { "graph_builtin/build_with_weight",    test_graph_build_with_weight,    NULL, NULL },
    { "graph_builtin/build_widen_i32",      test_graph_build_widen_i32,      NULL, NULL },
    { "graph_builtin/build_widen_small",    test_graph_build_widen_i16_u8_bool, NULL, NULL },
    { "graph_builtin/build_str_col_name",   test_graph_build_str_column_name, NULL, NULL },
    { "graph_builtin/info_dict_shape",      test_graph_info_dict_shape,      NULL, NULL },
    { "graph_builtin/free_idempotent",      test_graph_free_idempotent,      NULL, NULL },
    { "graph_builtin/handle_in_release",    test_graph_handle_in_release,    NULL, NULL },
    { "graph_builtin/handle_block_copy",    test_graph_handle_block_copy,    NULL, NULL },
    { "graph_builtin/pagerank_direct",      test_graph_pagerank_direct,      NULL, NULL },
    { "graph_builtin/build_widen_i64_w",    test_graph_build_widen_i64_weight, NULL, NULL },
    { "graph_builtin/atom_to_i64_narrow",   test_atom_to_i64_narrow,         NULL, NULL },
    { "graph_builtin/pagerank_bad_iter",    test_pagerank_bad_iter_type,     NULL, NULL },
    { "graph_builtin/cluster_direct",       test_graph_cluster_direct,       NULL, NULL },
    { "graph_builtin/build_weight_errors",  test_graph_build_weight_errors,  NULL, NULL },
    { "graph_builtin/build_weight_i32_f32", test_graph_build_weight_i32_f32, NULL, NULL },
    { "graph_builtin/build_validation",     test_graph_build_validation_errors, NULL, NULL },
    { "graph_builtin/dijkstra_direct",      test_graph_dijkstra_direct,      NULL, NULL },
    { "graph_builtin/shortest_path_direct", test_graph_shortest_path_direct, NULL, NULL },
    { "graph_builtin/expand_direct",        test_graph_expand_direct,        NULL, NULL },
    { "graph_builtin/var_expand_direct",    test_graph_var_expand_direct,    NULL, NULL },
    { "graph_builtin/algorithms_coverage",  test_graph_algorithms_coverage,  NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
