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
    { NULL, NULL, NULL, NULL },
};
