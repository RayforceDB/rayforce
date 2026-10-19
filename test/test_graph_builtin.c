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
#include "core/pool.h"
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

/* A build cut short by an interrupt must report "cancel", not the "oom"
 * that a NULL relation otherwise means (the parallel CSR build gives up on
 * a cancelled dispatch; the threshold is lowered so the small table takes
 * that path). */
static test_result_t test_graph_build_cancel(void) {
    ray_heap_init();
    (void)ray_sym_init();
    if (!ray_pool_par_dispatch_ok(ray_pool_get(), 1, 0)) {
        ray_sym_destroy(); ray_heap_destroy();
        SKIP("no pool worker");
    }

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };

    int64_t saved = ray_csr_par_min_edges;
    ray_csr_par_min_edges = 0;
    ray_request_interrupt();
    ray_t* h = ray_graph_build_fn(build_args, 3);
    ray_clear_interrupt();
    ray_csr_par_min_edges = saved;

    TEST_ASSERT_TRUE(RAY_IS_ERR(h));
    TEST_ASSERT_STR_EQ(ray_err_code(h), "cancel");
    ray_error_free(h);

    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Cancellation: a checkpointed graph algorithm run with a pending interrupt
 * must return a clean "cancel" error — the per-iteration checkpoint fires on
 * the first iteration and unwinds (arena reset) with no leak (ASan verifies).
 * -------------------------------------------------------------------------- */
static test_result_t test_graph_cancel(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(build_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    /* Arm a cancel, then run pagerank; clear the flag before asserting so a
     * failure can't leak the pending interrupt into later tests. */
    ray_request_interrupt();
    ray_t* pr_args[1] = { h };
    ray_t* result = ray_graph_pagerank_fn(pr_args, 1);
    ray_clear_interrupt();

    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    TEST_ASSERT_STR_EQ(ray_err_code(result), "cancel");
    ray_error_free(result);

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

    /* with track=true (bool atom): path tracking is unimplemented — must be a
     * LOUD error, not silently ignored.  (Re-baselined: previously this
     * asserted success while the flag was accepted-and-ignored.) */
    ray_t* track_atom = ray_alloc(0); track_atom->type = -RAY_BOOL; track_atom->b8 = 1;
    ray_t* ve_args3[6] = { h, src_atom, min_atom, max_atom, dir_atom, track_atom };
    ray_t* r3 = ray_graph_var_expand_fn(ve_args3, 6);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "nyi");
    ray_error_free(r3);

    /* track=false (int 0) must still succeed */
    ray_t* notrack_atom = ray_alloc(0); notrack_atom->type = -RAY_I64; notrack_atom->i64 = 0;
    ray_t* ve_args3b[6] = { h, src_atom, min_atom, max_atom, dir_atom, notrack_atom };
    ray_t* r3b = ray_graph_var_expand_fn(ve_args3b, 6);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r3b));
    ray_release(r3b);
    ray_release(notrack_atom);
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
 * 23. graph_unwrap — NULL handle, wrong type, no GRAPH attr
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_unwrap_branches(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* graph_unwrap(NULL) → NULL (line 51) */
    ray_t* r1 = ray_graph_free_fn(NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "type");
    ray_error_free(r1);

    /* graph_unwrap on wrong type — atom with type != -RAY_I64 (line 52) */
    ray_t* str_arg = ray_str("hello", 5);
    ray_t* r2 = ray_graph_info_fn(str_arg);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "type");
    ray_error_free(r2);
    ray_release(str_arg);

    /* graph_unwrap on -RAY_I64 atom WITHOUT GRAPH attr (line 53) */
    ray_t* fake = ray_alloc(0);
    fake->type = -RAY_I64;
    fake->i64  = 12345;
    /* Ensure GRAPH bit is NOT set */
    fake->attrs &= ~RAY_ATTR_GRAPH;
    ray_t* r3 = ray_graph_free_fn(fake);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "type");
    ray_error_free(r3);
    ray_release(fake);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 24. pagerank — integer damping argument (lines 375-376)
 *     atom_to_i64 is called when damping is int; result 0 triggers domain.
 * -------------------------------------------------------------------------- */

static test_result_t test_pagerank_int_damping(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    /* damping as I64 int = 0 → atom_to_i64 returns 0 → domain error */
    ray_t* iter_atom = ray_alloc(0); iter_atom->type = -RAY_I64; iter_atom->i64 = 10;
    ray_t* damp_int  = ray_alloc(0); damp_int->type  = -RAY_I64; damp_int->i64  = 0;
    ray_t* pr_args[3] = { h, iter_atom, damp_int };
    ray_t* r1 = ray_graph_pagerank_fn(pr_args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "domain");
    ray_error_free(r1);

    /* damping as I64 int = 1 → atom_to_i64 returns 1 → damping=1.0 → domain (>=1.0) */
    damp_int->i64 = 1;
    ray_t* r2 = ray_graph_pagerank_fn(pr_args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "domain");
    ray_error_free(r2);

    /* damping as I32 int = 0 → exercises atom_is_int for I32 on damping arg */
    ray_t* damp_i32 = ray_alloc(0); damp_i32->type = -RAY_I32; damp_i32->i32 = 0;
    ray_t* pr_args2[3] = { h, iter_atom, damp_i32 };
    ray_t* r3 = ray_graph_pagerank_fn(pr_args2, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "domain");
    ray_error_free(r3);
    ray_release(damp_i32);

    /* damping as F64 out of range: 0.0 → domain */
    ray_t* damp_zero = ray_alloc(0); damp_zero->type = -RAY_F64; damp_zero->f64 = 0.0;
    ray_t* pr_args3[3] = { h, iter_atom, damp_zero };
    ray_t* r4 = ray_graph_pagerank_fn(pr_args3, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "domain");
    ray_error_free(r4);
    ray_release(damp_zero);

    /* damping as F64 = 1.0 → domain */
    ray_t* damp_one = ray_alloc(0); damp_one->type = -RAY_F64; damp_one->f64 = 1.0;
    ray_t* pr_args4[3] = { h, iter_atom, damp_one };
    ray_t* r5 = ray_graph_pagerank_fn(pr_args4, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    TEST_ASSERT_STR_EQ(ray_err_code(r5), "domain");
    ray_error_free(r5);
    ray_release(damp_one);

    /* pagerank n > 3 → rank error */
    ray_t* extra = ray_alloc(0); extra->type = -RAY_I64; extra->i64 = 0;
    ray_t* pr_args5[4] = { h, iter_atom, damp_int, extra };
    ray_t* r6 = ray_graph_pagerank_fn(pr_args5, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r6));
    TEST_ASSERT_STR_EQ(ray_err_code(r6), "rank");
    ray_error_free(r6);
    ray_release(extra);

    /* pagerank iter=65536 → domain error (v > 65535) */
    ray_t* big_iter = ray_alloc(0); big_iter->type = -RAY_I64; big_iter->i64 = 65536;
    ray_t* pr_args6[2] = { h, big_iter };
    ray_t* r7 = ray_graph_pagerank_fn(pr_args6, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r7));
    TEST_ASSERT_STR_EQ(ray_err_code(r7), "domain");
    ray_error_free(r7);
    ray_release(big_iter);

    ray_release(damp_int);
    ray_release(iter_atom);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 25. louvain — type and domain errors (lines 455-458)
 * -------------------------------------------------------------------------- */

static test_result_t test_louvain_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(build_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    /* non-int iter → type error */
    ray_t* str_iter = ray_str("x", 1);
    ray_t* lo_args[2] = { h, str_iter };
    ray_t* r1 = ray_graph_louvain_fn(lo_args, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "type");
    ray_error_free(r1);
    ray_release(str_iter);

    /* iter=0 → domain error */
    ray_t* zero = ray_alloc(0); zero->type = -RAY_I64; zero->i64 = 0;
    ray_t* lo_args2[2] = { h, zero };
    ray_t* r2 = ray_graph_louvain_fn(lo_args2, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "domain");
    ray_error_free(r2);
    ray_release(zero);

    /* iter=65536 → domain error */
    ray_t* big = ray_alloc(0); big->type = -RAY_I64; big->i64 = 65536;
    ray_t* lo_args3[2] = { h, big };
    ray_t* r3 = ray_graph_louvain_fn(lo_args3, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "domain");
    ray_error_free(r3);
    ray_release(big);

    /* wrong arity n=3 → rank error */
    ray_t* extra = ray_alloc(0); extra->type = -RAY_I64; extra->i64 = 1;
    ray_t* lo_args4[3] = { h, extra, extra };
    ray_t* r4 = ray_graph_louvain_fn(lo_args4, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "rank");
    ray_error_free(r4);
    ray_release(extra);

    /* non-handle → type error */
    ray_t* lo_args5[1] = { tbl };
    ray_t* r5 = ray_graph_louvain_fn(lo_args5, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    TEST_ASSERT_STR_EQ(ray_err_code(r5), "type");
    ray_error_free(r5);

    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 26. dfs — all error branches (lines 502-513)
 * -------------------------------------------------------------------------- */

static test_result_t test_dfs_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(build_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* src = ray_alloc(0); src->type = -RAY_I64; src->i64 = 0;

    /* non-int src → type error */
    ray_t* str_src = ray_str("x", 1);
    ray_t* d_args1[2] = { h, str_src };
    ray_t* r1 = ray_graph_dfs_fn(d_args1, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "type");
    ray_error_free(r1);
    ray_release(str_src);

    /* non-int max_depth → type error */
    ray_t* str_depth = ray_str("x", 1);
    ray_t* d_args2[3] = { h, src, str_depth };
    ray_t* r2 = ray_graph_dfs_fn(d_args2, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "type");
    ray_error_free(r2);
    ray_release(str_depth);

    /* max_depth=-1 → domain error (v < 0) */
    ray_t* neg_depth = ray_alloc(0); neg_depth->type = -RAY_I64; neg_depth->i64 = -1;
    ray_t* d_args3[3] = { h, src, neg_depth };
    ray_t* r3 = ray_graph_dfs_fn(d_args3, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "domain");
    ray_error_free(r3);
    ray_release(neg_depth);

    /* max_depth=256 → domain error (v > 255) */
    ray_t* big_depth = ray_alloc(0); big_depth->type = -RAY_I64; big_depth->i64 = 256;
    ray_t* d_args4[3] = { h, src, big_depth };
    ray_t* r4 = ray_graph_dfs_fn(d_args4, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "domain");
    ray_error_free(r4);
    ray_release(big_depth);

    /* max_depth=0 → valid (only source node) */
    ray_t* zero_depth = ray_alloc(0); zero_depth->type = -RAY_I64; zero_depth->i64 = 0;
    ray_t* d_args5[3] = { h, src, zero_depth };
    ray_t* r5 = ray_graph_dfs_fn(d_args5, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r5));
    TEST_ASSERT_EQ_I(ray_table_nrows(r5), 1);
    ray_release(r5);
    ray_release(zero_depth);

    /* n=1 → rank error */
    ray_t* d_args6[1] = { h };
    ray_t* r6 = ray_graph_dfs_fn(d_args6, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r6));
    TEST_ASSERT_STR_EQ(ray_err_code(r6), "rank");
    ray_error_free(r6);

    /* n=4 → rank error */
    ray_t* extra = ray_alloc(0); extra->type = -RAY_I64; extra->i64 = 0;
    ray_t* d_args7[4] = { h, src, extra, extra };
    ray_t* r7 = ray_graph_dfs_fn(d_args7, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r7));
    TEST_ASSERT_STR_EQ(ray_err_code(r7), "rank");
    ray_error_free(r7);
    ray_release(extra);

    ray_release(src);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 27. betweenness/closeness — error branches (lines 542-583)
 * -------------------------------------------------------------------------- */

static test_result_t test_betweenness_closeness_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(build_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    /* --- betweenness --- */

    /* non-int sample → type error */
    ray_t* str_samp = ray_str("x", 1);
    ray_t* b_args1[2] = { h, str_samp };
    ray_t* r1 = ray_graph_betweenness_fn(b_args1, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "type");
    ray_error_free(r1);
    ray_release(str_samp);

    /* sample=-1 → domain error */
    ray_t* neg = ray_alloc(0); neg->type = -RAY_I64; neg->i64 = -1;
    ray_t* b_args2[2] = { h, neg };
    ray_t* r2 = ray_graph_betweenness_fn(b_args2, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "domain");
    ray_error_free(r2);

    /* sample=65536 → domain error */
    ray_t* big = ray_alloc(0); big->type = -RAY_I64; big->i64 = 65536;
    ray_t* b_args3[2] = { h, big };
    ray_t* r3 = ray_graph_betweenness_fn(b_args3, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "domain");
    ray_error_free(r3);

    /* n=3 → rank error */
    ray_t* ex = ray_alloc(0); ex->type = -RAY_I64; ex->i64 = 0;
    ray_t* b_args4[3] = { h, ex, ex };
    ray_t* r4 = ray_graph_betweenness_fn(b_args4, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "rank");
    ray_error_free(r4);
    ray_release(ex);

    /* --- closeness --- */

    /* non-int sample → type error */
    ray_t* str_samp2 = ray_str("x", 1);
    ray_t* c_args1[2] = { h, str_samp2 };
    ray_t* r5 = ray_graph_closeness_fn(c_args1, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    TEST_ASSERT_STR_EQ(ray_err_code(r5), "type");
    ray_error_free(r5);
    ray_release(str_samp2);

    /* sample=-1 → domain error */
    ray_t* c_args2[2] = { h, neg };
    ray_t* r6 = ray_graph_closeness_fn(c_args2, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r6));
    TEST_ASSERT_STR_EQ(ray_err_code(r6), "domain");
    ray_error_free(r6);

    /* sample=65536 → domain error */
    ray_t* c_args3[2] = { h, big };
    ray_t* r7 = ray_graph_closeness_fn(c_args3, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r7));
    TEST_ASSERT_STR_EQ(ray_err_code(r7), "domain");
    ray_error_free(r7);

    /* n=3 → rank error */
    ray_t* ex2 = ray_alloc(0); ex2->type = -RAY_I64; ex2->i64 = 0;
    ray_t* c_args4[3] = { h, ex2, ex2 };
    ray_t* r8 = ray_graph_closeness_fn(c_args4, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r8));
    TEST_ASSERT_STR_EQ(ray_err_code(r8), "rank");
    ray_error_free(r8);
    ray_release(ex2);

    /* closeness with sample=1 → valid (exercises the sampled path) */
    ray_t* samp1 = ray_alloc(0); samp1->type = -RAY_I64; samp1->i64 = 1;
    ray_t* c_args5[2] = { h, samp1 };
    ray_t* r9 = ray_graph_closeness_fn(c_args5, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r9));
    ray_release(r9);
    ray_release(samp1);

    ray_release(neg); ray_release(big);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 28. random_walk — error branches (lines 604-614)
 * -------------------------------------------------------------------------- */

static test_result_t test_random_walk_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(build_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* src = ray_alloc(0); src->type = -RAY_I64; src->i64 = 0;

    /* n=1 → rank error */
    ray_t* r_args1[1] = { h };
    ray_t* r1 = ray_graph_random_walk_fn(r_args1, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "rank");
    ray_error_free(r1);

    /* n=4 → rank error */
    ray_t* ex = ray_alloc(0); ex->type = -RAY_I64; ex->i64 = 0;
    ray_t* r_args2[4] = { h, src, ex, ex };
    ray_t* r2 = ray_graph_random_walk_fn(r_args2, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "rank");
    ray_error_free(r2);
    ray_release(ex);

    /* non-int src → type error */
    ray_t* str_src = ray_str("x", 1);
    ray_t* r_args3[2] = { h, str_src };
    ray_t* r3 = ray_graph_random_walk_fn(r_args3, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "type");
    ray_error_free(r3);
    ray_release(str_src);

    /* non-int walk_len → type error */
    ray_t* str_wl = ray_str("x", 1);
    ray_t* r_args4[3] = { h, src, str_wl };
    ray_t* r4 = ray_graph_random_walk_fn(r_args4, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "type");
    ray_error_free(r4);
    ray_release(str_wl);

    /* walk_len=0 → domain error */
    ray_t* zero_wl = ray_alloc(0); zero_wl->type = -RAY_I64; zero_wl->i64 = 0;
    ray_t* r_args5[3] = { h, src, zero_wl };
    ray_t* r5 = ray_graph_random_walk_fn(r_args5, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    TEST_ASSERT_STR_EQ(ray_err_code(r5), "domain");
    ray_error_free(r5);
    ray_release(zero_wl);

    /* walk_len=-1 → domain error */
    ray_t* neg_wl = ray_alloc(0); neg_wl->type = -RAY_I64; neg_wl->i64 = -1;
    ray_t* r_args6[3] = { h, src, neg_wl };
    ray_t* r6 = ray_graph_random_walk_fn(r_args6, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r6));
    TEST_ASSERT_STR_EQ(ray_err_code(r6), "domain");
    ray_error_free(r6);
    ray_release(neg_wl);

    /* walk_len=65536 → domain error */
    ray_t* big_wl = ray_alloc(0); big_wl->type = -RAY_I64; big_wl->i64 = 65536;
    ray_t* r_args7[3] = { h, src, big_wl };
    ray_t* r7 = ray_graph_random_walk_fn(r_args7, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r7));
    TEST_ASSERT_STR_EQ(ray_err_code(r7), "domain");
    ray_error_free(r7);
    ray_release(big_wl);

    ray_release(src);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 29. shortest_path — error branches (lines 660-671)
 * -------------------------------------------------------------------------- */

static test_result_t test_shortest_path_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(build_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* s = ray_alloc(0); s->type = -RAY_I64; s->i64 = 0;
    ray_t* d = ray_alloc(0); d->type = -RAY_I64; d->i64 = 1;

    /* non-int src → type error */
    ray_t* str_src = ray_str("x", 1);
    ray_t* sp_args1[3] = { h, str_src, d };
    ray_t* r1 = ray_graph_shortest_path_fn(sp_args1, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "type");
    ray_error_free(r1);
    ray_release(str_src);

    /* non-int dst → type error */
    ray_t* str_dst = ray_str("x", 1);
    ray_t* sp_args2[3] = { h, s, str_dst };
    ray_t* r2 = ray_graph_shortest_path_fn(sp_args2, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "type");
    ray_error_free(r2);
    ray_release(str_dst);

    /* non-int max_depth → type error */
    ray_t* str_md = ray_str("x", 1);
    ray_t* sp_args3[4] = { h, s, d, str_md };
    ray_t* r3 = ray_graph_shortest_path_fn(sp_args3, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "type");
    ray_error_free(r3);
    ray_release(str_md);

    /* max_depth=0 → domain error (v <= 0) */
    ray_t* zero_md = ray_alloc(0); zero_md->type = -RAY_I64; zero_md->i64 = 0;
    ray_t* sp_args4[4] = { h, s, d, zero_md };
    ray_t* r4 = ray_graph_shortest_path_fn(sp_args4, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "domain");
    ray_error_free(r4);
    ray_release(zero_md);

    /* max_depth=256 → domain error (v > 255) */
    ray_t* big_md = ray_alloc(0); big_md->type = -RAY_I64; big_md->i64 = 256;
    ray_t* sp_args5[4] = { h, s, d, big_md };
    ray_t* r5 = ray_graph_shortest_path_fn(sp_args5, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    TEST_ASSERT_STR_EQ(ray_err_code(r5), "domain");
    ray_error_free(r5);
    ray_release(big_md);

    /* n=5 → rank error */
    ray_t* ex = ray_alloc(0); ex->type = -RAY_I64; ex->i64 = 0;
    ray_t* sp_args6[5] = { h, s, d, ex, ex };
    ray_t* r6 = ray_graph_shortest_path_fn(sp_args6, 5);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r6));
    TEST_ASSERT_STR_EQ(ray_err_code(r6), "rank");
    ray_error_free(r6);
    ray_release(ex);

    ray_release(s); ray_release(d);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 30. expand — type error for direction (line 696)
 * -------------------------------------------------------------------------- */

static test_result_t test_expand_type_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(build_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* src = ray_alloc(0); src->type = -RAY_I64; src->i64 = 0;

    /* non-int direction → type error */
    ray_t* str_dir = ray_str("x", 1);
    ray_t* ex_args[3] = { h, src, str_dir };
    ray_t* r1 = ray_graph_expand_fn(ex_args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "type");
    ray_error_free(r1);
    ray_release(str_dir);

    /* non-int src → type error */
    ray_t* str_src = ray_str("x", 1);
    ray_t* ex_args2[2] = { h, str_src };
    ray_t* r2 = ray_graph_expand_fn(ex_args2, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "type");
    ray_error_free(r2);
    ray_release(str_src);

    /* direction=-1 → domain error */
    ray_t* neg_dir = ray_alloc(0); neg_dir->type = -RAY_I64; neg_dir->i64 = -1;
    ray_t* ex_args3[3] = { h, src, neg_dir };
    ray_t* r3 = ray_graph_expand_fn(ex_args3, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "domain");
    ray_error_free(r3);
    ray_release(neg_dir);

    /* direction=2 → valid (both directions) */
    ray_t* both_dir = ray_alloc(0); both_dir->type = -RAY_I64; both_dir->i64 = 2;
    ray_t* ex_args4[3] = { h, src, both_dir };
    ray_t* r4 = ray_graph_expand_fn(ex_args4, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r4));
    ray_release(r4);
    ray_release(both_dir);

    ray_release(src);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 31. var_expand — direction/track error branches (lines 735-744)
 * -------------------------------------------------------------------------- */

static test_result_t test_var_expand_extra_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(build_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* src_a = ray_alloc(0); src_a->type = -RAY_I64; src_a->i64 = 0;
    ray_t* min_a = ray_alloc(0); min_a->type = -RAY_I64; min_a->i64 = 1;
    ray_t* max_a = ray_alloc(0); max_a->type = -RAY_I64; max_a->i64 = 2;

    /* non-int direction → type error (line 735) */
    ray_t* str_dir = ray_str("x", 1);
    ray_t* ve_args1[5] = { h, src_a, min_a, max_a, str_dir };
    ray_t* r1 = ray_graph_var_expand_fn(ve_args1, 5);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "type");
    ray_error_free(r1);
    ray_release(str_dir);

    /* direction=-1 → domain error (line 737, v < 0) */
    ray_t* neg_dir = ray_alloc(0); neg_dir->type = -RAY_I64; neg_dir->i64 = -1;
    ray_t* ve_args2[5] = { h, src_a, min_a, max_a, neg_dir };
    ray_t* r2 = ray_graph_var_expand_fn(ve_args2, 5);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "domain");
    ray_error_free(r2);
    ray_release(neg_dir);

    /* direction=3 → domain error (line 737, v > 2) */
    ray_t* big_dir = ray_alloc(0); big_dir->type = -RAY_I64; big_dir->i64 = 3;
    ray_t* ve_args3[5] = { h, src_a, min_a, max_a, big_dir };
    ray_t* r3 = ray_graph_var_expand_fn(ve_args3, 5);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "domain");
    ray_error_free(r3);
    ray_release(big_dir);

    /* non-bool/non-int track → type error (line 741-742) */
    ray_t* dir_ok = ray_alloc(0); dir_ok->type = -RAY_I64; dir_ok->i64 = 0;
    ray_t* str_track = ray_str("x", 1);
    ray_t* ve_args4[6] = { h, src_a, min_a, max_a, dir_ok, str_track };
    ray_t* r4 = ray_graph_var_expand_fn(ve_args4, 6);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "type");
    ray_error_free(r4);
    ray_release(str_track);

    /* track as integer (1) → exercises atom_to_i64 path, then loud nyi
     * (path tracking unimplemented).  Re-baselined: was accepted-and-ignored. */
    ray_t* int_track = ray_alloc(0); int_track->type = -RAY_I64; int_track->i64 = 1;
    ray_t* ve_args5[6] = { h, src_a, min_a, max_a, dir_ok, int_track };
    ray_t* r5 = ray_graph_var_expand_fn(ve_args5, 6);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    TEST_ASSERT_STR_EQ(ray_err_code(r5), "nyi");
    ray_error_free(r5);
    ray_release(int_track);

    /* track as integer (0) → track=false path */
    ray_t* int_track0 = ray_alloc(0); int_track0->type = -RAY_I64; int_track0->i64 = 0;
    ray_t* ve_args6[6] = { h, src_a, min_a, max_a, dir_ok, int_track0 };
    ray_t* r6 = ray_graph_var_expand_fn(ve_args6, 6);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r6));
    ray_release(r6);
    ray_release(int_track0);

    /* n=7 → rank error */
    ray_t* ex = ray_alloc(0); ex->type = -RAY_I64; ex->i64 = 0;
    ray_t* ve_args7[7] = { h, src_a, min_a, max_a, dir_ok, ex, ex };
    ray_t* r7 = ray_graph_var_expand_fn(ve_args7, 7);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r7));
    TEST_ASSERT_STR_EQ(ray_err_code(r7), "rank");
    ray_error_free(r7);
    ray_release(ex);

    /* min > max → domain error (line 730) */
    ray_t* big_min = ray_alloc(0); big_min->type = -RAY_I64; big_min->i64 = 5;
    ray_t* small_max = ray_alloc(0); small_max->type = -RAY_I64; small_max->i64 = 2;
    ray_t* ve_args8[4] = { h, src_a, big_min, small_max };
    ray_t* r8 = ray_graph_var_expand_fn(ve_args8, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r8));
    TEST_ASSERT_STR_EQ(ray_err_code(r8), "domain");
    ray_error_free(r8);
    ray_release(big_min); ray_release(small_max);

    /* min < 0 → domain error */
    ray_t* neg_min = ray_alloc(0); neg_min->type = -RAY_I64; neg_min->i64 = -1;
    ray_t* ve_args9[4] = { h, src_a, neg_min, max_a };
    ray_t* r9 = ray_graph_var_expand_fn(ve_args9, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r9));
    TEST_ASSERT_STR_EQ(ray_err_code(r9), "domain");
    ray_error_free(r9);
    ray_release(neg_min);

    /* max > 255 → domain error */
    ray_t* huge_max = ray_alloc(0); huge_max->type = -RAY_I64; huge_max->i64 = 256;
    ray_t* ve_args10[4] = { h, src_a, min_a, huge_max };
    ray_t* r10 = ray_graph_var_expand_fn(ve_args10, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r10));
    TEST_ASSERT_STR_EQ(ray_err_code(r10), "domain");
    ray_error_free(r10);
    ray_release(huge_max);

    ray_release(dir_ok);
    ray_release(src_a); ray_release(min_a); ray_release(max_a);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 32. dijkstra — type/domain for max_depth (lines 424-428)
 * -------------------------------------------------------------------------- */

static test_result_t test_dijkstra_depth_errors(void) {
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

    ray_t* src = ray_alloc(0); src->type = -RAY_I64; src->i64 = 0;
    ray_t* dst = ray_alloc(0); dst->type = -RAY_I64; dst->i64 = 1;

    /* non-int max_depth → type error (line 425) */
    ray_t* str_md = ray_str("x", 1);
    ray_t* d_args1[4] = { h, src, dst, str_md };
    ray_t* r1 = ray_graph_dijkstra_fn(d_args1, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "type");
    ray_error_free(r1);
    ray_release(str_md);

    /* max_depth=0 → domain error (line 426, v <= 0) */
    ray_t* zero_md = ray_alloc(0); zero_md->type = -RAY_I64; zero_md->i64 = 0;
    ray_t* d_args2[4] = { h, src, dst, zero_md };
    ray_t* r2 = ray_graph_dijkstra_fn(d_args2, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "domain");
    ray_error_free(r2);
    ray_release(zero_md);

    /* max_depth=-1 → domain error */
    ray_t* neg_md = ray_alloc(0); neg_md->type = -RAY_I64; neg_md->i64 = -1;
    ray_t* d_args3[4] = { h, src, dst, neg_md };
    ray_t* r3 = ray_graph_dijkstra_fn(d_args3, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "domain");
    ray_error_free(r3);
    ray_release(neg_md);

    /* max_depth=256 → domain error (v > 255) */
    ray_t* big_md = ray_alloc(0); big_md->type = -RAY_I64; big_md->i64 = 256;
    ray_t* d_args4[4] = { h, src, dst, big_md };
    ray_t* r4 = ray_graph_dijkstra_fn(d_args4, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "domain");
    ray_error_free(r4);
    ray_release(big_md);

    /* n=5 → rank error */
    ray_t* ex = ray_alloc(0); ex->type = -RAY_I64; ex->i64 = 0;
    ray_t* d_args5[5] = { h, src, dst, ex, ex };
    ray_t* r5 = ray_graph_dijkstra_fn(d_args5, 5);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    TEST_ASSERT_STR_EQ(ray_err_code(r5), "rank");
    ray_error_free(r5);
    ray_release(ex);

    /* non-int src → type error (line 409) */
    ray_t* str_src = ray_str("x", 1);
    ray_t* d_args6[2] = { h, str_src };
    ray_t* r6 = ray_graph_dijkstra_fn(d_args6, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r6));
    TEST_ASSERT_STR_EQ(ray_err_code(r6), "type");
    ray_error_free(r6);
    ray_release(str_src);

    ray_release(src); ray_release(dst);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst); ray_release(sym_w);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 33. k_shortest — type errors for individual args (lines 633-634)
 * -------------------------------------------------------------------------- */

static test_result_t test_k_shortest_type_errors(void) {
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

    ray_t* i0 = ray_alloc(0); i0->type = -RAY_I64; i0->i64 = 0;
    ray_t* i1 = ray_alloc(0); i1->type = -RAY_I64; i1->i64 = 1;
    ray_t* i2 = ray_alloc(0); i2->type = -RAY_I64; i2->i64 = 2;

    /* non-int src → type error */
    ray_t* str = ray_str("x", 1);
    ray_t* ks_args1[4] = { h, str, i1, i2 };
    ray_t* r1 = ray_graph_k_shortest_fn(ks_args1, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "type");
    ray_error_free(r1);

    /* non-int dst → type error */
    ray_t* ks_args2[4] = { h, i0, str, i2 };
    ray_t* r2 = ray_graph_k_shortest_fn(ks_args2, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "type");
    ray_error_free(r2);

    /* non-int k → type error */
    ray_t* ks_args3[4] = { h, i0, i1, str };
    ray_t* r3 = ray_graph_k_shortest_fn(ks_args3, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "type");
    ray_error_free(r3);
    ray_release(str);

    /* k=65536 → domain error (k_v > 65535) */
    ray_t* big_k = ray_alloc(0); big_k->type = -RAY_I64; big_k->i64 = 65536;
    ray_t* ks_args4[4] = { h, i0, i1, big_k };
    ray_t* r4 = ray_graph_k_shortest_fn(ks_args4, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "domain");
    ray_error_free(r4);
    ray_release(big_k);

    /* k=-1 → domain error */
    ray_t* neg_k = ray_alloc(0); neg_k->type = -RAY_I64; neg_k->i64 = -1;
    ray_t* ks_args5[4] = { h, i0, i1, neg_k };
    ray_t* r5 = ray_graph_k_shortest_fn(ks_args5, 4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    TEST_ASSERT_STR_EQ(ray_err_code(r5), "domain");
    ray_error_free(r5);
    ray_release(neg_k);

    /* n=3 → rank error */
    ray_t* ks_args6[3] = { h, i0, i1 };
    ray_t* r6 = ray_graph_k_shortest_fn(ks_args6, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r6));
    TEST_ASSERT_STR_EQ(ray_err_code(r6), "rank");
    ray_error_free(r6);

    /* n=5 → rank error */
    ray_t* ks_args7[5] = { h, i0, i1, i2, i0 };
    ray_t* r7 = ray_graph_k_shortest_fn(ks_args7, 5);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r7));
    TEST_ASSERT_STR_EQ(ray_err_code(r7), "rank");
    ray_error_free(r7);

    ray_release(i0); ray_release(i1); ray_release(i2);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst); ray_release(sym_w);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 34. build with n > 4 → rank error (line 157)
 * -------------------------------------------------------------------------- */

static test_result_t test_build_rank_n5(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* extra   = ray_sym(ray_sym_intern("extra", 5));

    /* n=5 → rank error */
    ray_t* args[5] = { tbl, sym_src, sym_dst, extra, extra };
    ray_t* r = ray_graph_build_fn(args, 5);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "rank");
    ray_error_free(r);

    /* n=2 → rank error */
    ray_t* args2[2] = { tbl, sym_src };
    ray_t* r2 = ray_graph_build_fn(args2, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "rank");
    ray_error_free(r2);

    ray_release(extra);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 35. degree/topsort/cluster — wrong-arity rank errors
 * -------------------------------------------------------------------------- */

static test_result_t test_single_arg_algo_rank_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = make_i64_edge_table();
    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));
    ray_t* build_args[3] = { tbl, sym_src, sym_dst };
    ray_t* h = ray_graph_build_fn(build_args, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(h));

    ray_t* ex = ray_alloc(0); ex->type = -RAY_I64; ex->i64 = 0;

    /* degree n=2 → rank error */
    ray_t* deg_args[2] = { h, ex };
    ray_t* r1 = ray_graph_degree_fn(deg_args, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "rank");
    ray_error_free(r1);

    /* topsort n=2 → rank error */
    ray_t* ts_args[2] = { h, ex };
    ray_t* r2 = ray_graph_topsort_fn(ts_args, 2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "rank");
    ray_error_free(r2);

    /* degree n=0 → rank error */
    ray_t* r3 = ray_graph_degree_fn(deg_args, 0);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "rank");
    ray_error_free(r3);

    /* topsort non-handle → type error */
    ray_t* ts_args2[1] = { tbl };
    ray_t* r4 = ray_graph_topsort_fn(ts_args2, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "type");
    ray_error_free(r4);

    /* degree non-handle → type error */
    ray_t* deg_args2[1] = { tbl };
    ray_t* r5 = ray_graph_degree_fn(deg_args2, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    TEST_ASSERT_STR_EQ(ray_err_code(r5), "type");
    ray_error_free(r5);

    ray_release(ex);
    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 36. widen_to_i64 — SYM column path (line 125-127)
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_widen_sym_col(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a SYM-typed src column: intern three symbols, store their IDs
     * as SYM vec entries.  The node IDs will be the raw symbol IDs,
     * which are small non-negative integers — valid for graph node IDs. */
    int64_t sym0 = ray_sym_intern("n0", 2);
    int64_t sym1 = ray_sym_intern("n1", 2);
    int64_t sym2 = ray_sym_intern("n2", 2);

    /* Make a SYM vec manually.  SYM vecs store i64 symbol IDs. */
    ray_t* sv = ray_sym_vec_new(RAY_SYM_W64, 3);
    if (!sv || RAY_IS_ERR(sv)) { ray_sym_destroy(); ray_heap_destroy(); FAIL("sv alloc"); }
    sv = ray_vec_append(sv, &sym0);
    sv = ray_vec_append(sv, &sym1);
    sv = ray_vec_append(sv, &sym2);

    /* dst column as plain I64 */
    int64_t dst_data[] = {1, 2, 0};
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

    /* The graph should have been built successfully with widened SYM IDs */
    ray_rel_t* rel = (ray_rel_t*)(uintptr_t)h->i64;
    TEST_ASSERT_TRUE(rel->fwd.n_nodes > 0);
    TEST_ASSERT_EQ_I(rel->fwd.n_edges, 3);

    ray_release(h);
    ray_release(sym_src); ray_release(sym_dst);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 37. build — NULL first arg → type error
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_null_arg(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* sym_src = ray_sym(ray_sym_intern("src", 3));
    ray_t* sym_dst = ray_sym(ray_sym_intern("dst", 3));

    /* NULL table → type error (line 159 !tbl branch) */
    ray_t* args[3] = { NULL, sym_src, sym_dst };
    ray_t* r = ray_graph_build_fn(args, 3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "type");
    ray_error_free(r);

    ray_release(sym_src); ray_release(sym_dst);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * 38. max_plus_one — negative IDs return 0 (line 144)
 *     Verify graph_build handles correctly when all IDs are negative.
 *     This cannot happen in normal usage since node IDs should be >= 0,
 *     but the branch exists for safety.
 *     NOTE: Unreachable in practice because negative node IDs would cause
 *     issues in CSR construction. This branch is documented as defensive.
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * 39. widen_to_i64 — non-vec input returns NULL (line 103)
 * -------------------------------------------------------------------------- */

static test_result_t test_graph_build_non_vec_column(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a table where the src column is a non-vec (atom).
     * This triggers the !ray_is_vec(col) check in widen_to_i64. */
    ray_t* atom_val = ray_alloc(0);
    atom_val->type = -RAY_I64;
    atom_val->i64  = 42;
    int64_t dst_data[] = {1, 2, 0};
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 3);

    int64_t s_sym = ray_sym_intern("src", 3);
    int64_t d_sym = ray_sym_intern("dst", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_sym, atom_val); ray_release(atom_val);
    tbl = ray_table_add_col(tbl, d_sym, dv); ray_release(dv);

    ray_t* sym_src = ray_sym(s_sym);
    ray_t* sym_dst = ray_sym(d_sym);
    ray_t* args[3] = { tbl, sym_src, sym_dst };

    ray_t* r = ray_graph_build_fn(args, 3);
    /* Should fail with type error because non-vec col */
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);

    ray_release(sym_src); ray_release(sym_dst);
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
    { "graph_builtin/cancel",               test_graph_cancel,               NULL, NULL },
    { "graph_builtin/build_cancel",         test_graph_build_cancel,         NULL, NULL },
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
    { "graph_builtin/unwrap_branches",      test_graph_unwrap_branches,      NULL, NULL },
    { "graph_builtin/pagerank_int_damping", test_pagerank_int_damping,       NULL, NULL },
    { "graph_builtin/louvain_errors",       test_louvain_errors,             NULL, NULL },
    { "graph_builtin/dfs_errors",           test_dfs_errors,                 NULL, NULL },
    { "graph_builtin/bet_clo_errors",       test_betweenness_closeness_errors, NULL, NULL },
    { "graph_builtin/random_walk_errors",   test_random_walk_errors,         NULL, NULL },
    { "graph_builtin/shortest_path_errors", test_shortest_path_errors,       NULL, NULL },
    { "graph_builtin/expand_type_errors",   test_expand_type_errors,         NULL, NULL },
    { "graph_builtin/var_expand_extra",     test_var_expand_extra_errors,    NULL, NULL },
    { "graph_builtin/dijkstra_depth_errors",test_dijkstra_depth_errors,      NULL, NULL },
    { "graph_builtin/k_shortest_type_errs", test_k_shortest_type_errors,     NULL, NULL },
    { "graph_builtin/build_rank_n5",        test_build_rank_n5,              NULL, NULL },
    { "graph_builtin/single_arg_rank_errs", test_single_arg_algo_rank_errors, NULL, NULL },
    { "graph_builtin/widen_sym_col",        test_graph_build_widen_sym_col,  NULL, NULL },
    { "graph_builtin/build_null_arg",       test_graph_build_null_arg,       NULL, NULL },
    { "graph_builtin/build_non_vec_col",    test_graph_build_non_vec_column, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
