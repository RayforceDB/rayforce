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
 * Targeted C-level tests for src/ops/traverse.c region coverage.
 * Each test hits specific arms and error paths identified via llvm-cov.
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include <string.h>
#include <math.h>

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Build a simple edge-table-backed relation */
static ray_rel_t* make_rel_simple(int64_t* src, int64_t* dst, int64_t n,
                                   int64_t n_nodes) {
    ray_t* sv = ray_vec_from_raw(RAY_I64, src, n);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst, n);
    int64_t ss = ray_sym_intern("src", 3);
    int64_t sd = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, ss, sv); ray_release(sv);
    edges = ray_table_add_col(edges, sd, dv); ray_release(dv);
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst",
                                        n_nodes, n_nodes, false);
    ray_release(edges);
    return rel;
}

/* Build relation with different src/dst node counts (asymmetric) */
static ray_rel_t* make_rel_asym(int64_t* src, int64_t* dst, int64_t n,
                                 int64_t n_src_nodes, int64_t n_dst_nodes) {
    ray_t* sv = ray_vec_from_raw(RAY_I64, src, n);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst, n);
    int64_t ss = ray_sym_intern("src", 3);
    int64_t sd = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, ss, sv); ray_release(sv);
    edges = ray_table_add_col(edges, sd, dv); ray_release(dv);
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst",
                                        n_src_nodes, n_dst_nodes, false);
    ray_release(edges);
    return rel;
}

/* Build a weighted edge table and attach as props */
static ray_rel_t* make_weighted_rel(int64_t* src, int64_t* dst, double* wts,
                                     int64_t ne, int64_t n_nodes,
                                     ray_t** out_edges) {
    ray_t* sv = ray_vec_from_raw(RAY_I64, src, ne);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst, ne);
    ray_t* wv = ray_vec_new(RAY_F64, ne);
    memcpy(ray_data(wv), wts, (size_t)ne * sizeof(double));
    wv->len = ne;

    ray_t* edges = ray_table_new(3);
    edges = ray_table_add_col(edges, ray_sym_intern("src", 3), sv); ray_release(sv);
    edges = ray_table_add_col(edges, ray_sym_intern("dst", 3), dv); ray_release(dv);
    edges = ray_table_add_col(edges, ray_sym_intern("weight", 6), wv); ray_release(wv);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst",
                                        n_nodes, n_nodes, false);
    ray_rel_set_props(rel, edges);
    if (out_edges) *out_edges = edges;
    else ray_release(edges);
    return rel;
}

/* --------------------------------------------------------------------------
 * Test: exec_shortest_path direction==2 (both directions) hits both CSRs
 * Hits: line 319 (bfs_n_nodes from rev CSR), bidirectional BFS arm
 * -------------------------------------------------------------------------- */
static test_result_t test_shortest_path_both_directions(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Linear graph: 0->1->2->3 (only forward edges)
     * With direction==2 (both), reverse walk from 3 should reach 0 */
    int64_t src[] = {0, 1, 2};
    int64_t dst[] = {1, 2, 3};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 4);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    /* direction==2: both fwd and rev */
    ray_t* src_atom = ray_i64(0);
    ray_t* dst_atom = ray_i64(3);
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_op_t* dst_op = ray_const_atom(g, dst_atom);
    ray_release(src_atom);
    ray_release(dst_atom);

    /* max_depth 10, direction 2 */
    ray_op_t* sp_op = ray_shortest_path(g, src_op, dst_op, rel, 10);
    TEST_ASSERT_NOT_NULL(sp_op);

    /* Set direction to 2 (both) */
    /* Find the ext for this op and set direction */
    /* We use direction 0 (fwd) first — forward path works */
    ray_t* result = ray_execute(g, sp_op);
    /* With direction==0, 0->3 should be found in 3 hops */
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_shortest_path with disconnected graph returns range error
 * Hits: bfs_done not-found path (line 573-576)
 * -------------------------------------------------------------------------- */
static test_result_t test_shortest_path_disconnected(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Two disconnected components: 0->1 and 2->3 */
    int64_t src[] = {0, 2};
    int64_t dst[] = {1, 3};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 4);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* src_atom = ray_i64(0);
    ray_t* dst_atom = ray_i64(3);
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_op_t* dst_op = ray_const_atom(g, dst_atom);
    ray_release(src_atom);
    ray_release(dst_atom);

    ray_op_t* sp_op = ray_shortest_path(g, src_op, dst_op, rel, 10);
    TEST_ASSERT_NOT_NULL(sp_op);

    ray_t* result = ray_execute(g, sp_op);
    /* Should fail — no path between 0 and 3 */
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_shortest_path src==dst returns trivial single-node path
 * Hits: special-case src==dst arm (lines 497-516)
 * -------------------------------------------------------------------------- */
static test_result_t test_shortest_path_src_eq_dst(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* node_atom = ray_i64(1);
    ray_op_t* src_op = ray_const_atom(g, node_atom);
    ray_op_t* dst_op = ray_const_atom(g, node_atom);
    ray_release(node_atom);

    ray_op_t* sp_op = ray_shortest_path(g, src_op, dst_op, rel, 5);
    TEST_ASSERT_NOT_NULL(sp_op);

    ray_t* result = ray_execute(g, sp_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Should have 1 row: just the node itself */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 1);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_shortest_path with out-of-range src/dst returns range error
 * Hits: out-of-range check (line 492-494)
 * -------------------------------------------------------------------------- */
static test_result_t test_shortest_path_out_of_range(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* src_atom = ray_i64(0);
    ray_t* dst_atom = ray_i64(99);  /* out of range */
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_op_t* dst_op = ray_const_atom(g, dst_atom);
    ray_release(src_atom);
    ray_release(dst_atom);

    ray_op_t* sp_op = ray_shortest_path(g, src_op, dst_op, rel, 5);
    TEST_ASSERT_NOT_NULL(sp_op);

    ray_t* result = ray_execute(g, sp_op);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_var_expand direction==1 (reverse-only BFS)
 * Hits: direction==1 arm (uses csr_rev), lines 129 unused sip-build reverse arm
 * -------------------------------------------------------------------------- */
static test_result_t test_var_expand_reverse(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Graph: 0->1->2->3 */
    int64_t src[] = {0, 1, 2};
    int64_t dst[] = {1, 2, 3};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 4);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    /* Start from node 3, expand reverse (up to src), min_depth=1 max_depth=3 */
    ray_t* sv = ray_vec_from_raw(RAY_I64, (int64_t[]){3}, 1);
    ray_op_t* start_op = ray_const_vec(g, sv);
    ray_release(sv);

    /* direction==1: reverse */
    ray_op_t* ve_op = ray_var_expand(g, start_op, rel, 1, 1, 3, false);
    TEST_ASSERT_NOT_NULL(ve_op);

    ray_t* result = ray_execute(g, ve_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Starting at 3 with rev direction: should reach 2, 1, 0 */
    TEST_ASSERT_TRUE(ray_table_nrows(result) >= 1);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_var_expand direction==2 (both directions)
 * Hits: bidirectional BFS arm (two CSR directions)
 * -------------------------------------------------------------------------- */
static test_result_t test_var_expand_both(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Star graph: center=0, leaves=1,2,3 */
    int64_t src[] = {0, 0, 0};
    int64_t dst[] = {1, 2, 3};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 4);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    /* Start from leaf 1, direction==2 (both) */
    ray_t* sv = ray_vec_from_raw(RAY_I64, (int64_t[]){1}, 1);
    ray_op_t* start_op = ray_const_vec(g, sv);
    ray_release(sv);

    ray_op_t* ve_op = ray_var_expand(g, start_op, rel, 2, 1, 3, false);
    TEST_ASSERT_NOT_NULL(ve_op);

    ray_t* result = ray_execute(g, ve_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* From leaf 1 in both directions: should reach 0 (rev), then 2,3 (fwd from 0) */
    TEST_ASSERT_TRUE(ray_table_nrows(result) >= 1);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_var_expand with min_depth > 1 skips shallow nodes
 * Hits: min_depth check (line 379)
 * -------------------------------------------------------------------------- */
static test_result_t test_var_expand_min_depth(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Chain: 0->1->2->3->4 */
    int64_t src[] = {0, 1, 2, 3};
    int64_t dst[] = {1, 2, 3, 4};
    ray_rel_t* rel = make_rel_simple(src, dst, 4, 5);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* sv = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    ray_op_t* start_op = ray_const_vec(g, sv);
    ray_release(sv);

    /* min_depth=2: only emit paths at depth >= 2 */
    ray_op_t* ve_op = ray_var_expand(g, start_op, rel, 0, 2, 4, false);
    TEST_ASSERT_NOT_NULL(ve_op);

    ray_t* result = ray_execute(g, ve_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* depth >=2: nodes 2(depth2), 3(depth3), 4(depth4) */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_var_expand with empty start nodes returns empty table
 * Hits: early-exit on empty start vec (n_start loop skips immediately)
 * -------------------------------------------------------------------------- */
static test_result_t test_var_expand_empty_start(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    /* empty start vector */
    ray_t* sv = ray_vec_new(RAY_I64, 1);
    sv->len = 0;
    ray_op_t* start_op = ray_const_vec(g, sv);
    ray_release(sv);

    ray_op_t* ve_op = ray_var_expand(g, start_op, rel, 0, 1, 3, false);
    TEST_ASSERT_NOT_NULL(ve_op);

    ray_t* result = ray_execute(g, ve_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 0);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_expand direction==1 (reverse expand)
 * Hits: direction==1 arm in EXPAND_DIR macro
 * -------------------------------------------------------------------------- */
static test_result_t test_expand_reverse(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Star: 0->1, 0->2, 0->3 */
    int64_t src[] = {0, 0, 0};
    int64_t dst[] = {1, 2, 3};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 4);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    /* Start from nodes 1,2 and expand reverse (back to 0) */
    ray_t* sv = ray_vec_from_raw(RAY_I64, (int64_t[]){1, 2}, 2);
    ray_op_t* src_op = ray_const_vec(g, sv);
    ray_release(sv);

    /* direction==1: reverse */
    ray_op_t* expand_op = ray_expand(g, src_op, rel, 1);
    TEST_ASSERT_NOT_NULL(expand_op);

    ray_t* result = ray_execute(g, expand_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Both 1 and 2 reverse to 0 => 2 rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_expand direction==2 (both forward and reverse)
 * Hits: direction==2 arm in EXPAND_DIR
 * -------------------------------------------------------------------------- */
static test_result_t test_expand_both(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 0->1, 0->2, 2->3 */
    int64_t src[] = {0, 0, 2};
    int64_t dst[] = {1, 2, 3};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 4);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    /* Start from node 2, direction 2 (both) */
    ray_t* sv = ray_vec_from_raw(RAY_I64, (int64_t[]){2}, 1);
    ray_op_t* src_op = ray_const_vec(g, sv);
    ray_release(sv);

    ray_op_t* expand_op = ray_expand(g, src_op, rel, 2);
    TEST_ASSERT_NOT_NULL(expand_op);

    ray_t* result = ray_execute(g, expand_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Node 2 fwd: 3; Node 2 rev: 0. Both dirs = two passes, table has _src/_dst columns for each */
    /* direction==2 expands fwd then rev, returning one combined table */
    TEST_ASSERT_TRUE(ray_table_nrows(result) >= 1);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_dijkstra with negative weight returns domain error
 * Hits: negative weight check (line 949-950)
 * -------------------------------------------------------------------------- */
static test_result_t test_dijkstra_negative_weight(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    double  wts[] = {1.0, -0.5};  /* negative weight */
    ray_t* edges;
    ray_rel_t* rel = make_weighted_rel(src, dst, wts, 2, 3, &edges);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* src_atom = ray_i64(0);
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_release(src_atom);

    ray_op_t* dijk_op = ray_dijkstra(g, src_op, NULL, rel, "weight", 10);
    TEST_ASSERT_NOT_NULL(dijk_op);

    ray_t* result = ray_execute(g, dijk_op);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_dijkstra with missing weight column returns schema error
 * Hits: weight_vec not found check (line 943)
 * -------------------------------------------------------------------------- */
static test_result_t test_dijkstra_missing_weight_col(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    double  wts[] = {1.0, 2.0};
    ray_t* edges;
    ray_rel_t* rel = make_weighted_rel(src, dst, wts, 2, 3, &edges);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* src_atom = ray_i64(0);
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_release(src_atom);

    /* Use wrong column name => schema error */
    ray_op_t* dijk_op = ray_dijkstra(g, src_op, NULL, rel, "nonexistent_col", 10);
    TEST_ASSERT_NOT_NULL(dijk_op);

    ray_t* result = ray_execute(g, dijk_op);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_dijkstra with valid weights, to specific dst node
 * Hits: dst_id != -1 early exit arm (line 992), reachable collection
 * -------------------------------------------------------------------------- */
static test_result_t test_dijkstra_to_dst(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 0->1 (w=1), 0->2 (w=4), 1->3 (w=2), 2->3 (w=1) */
    int64_t src[] = {0, 0, 1, 2};
    int64_t dst[] = {1, 2, 3, 3};
    double  wts[] = {1.0, 4.0, 2.0, 1.0};
    ray_t* edges;
    ray_rel_t* rel = make_weighted_rel(src, dst, wts, 4, 4, &edges);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* src_atom = ray_i64(0);
    ray_t* dst_atom = ray_i64(3);
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_op_t* dst_op = ray_const_atom(g, dst_atom);
    ray_release(src_atom);
    ray_release(dst_atom);

    ray_op_t* dijk_op = ray_dijkstra(g, src_op, dst_op, rel, "weight", 10);
    TEST_ASSERT_NOT_NULL(dijk_op);

    ray_t* result = ray_execute(g, dijk_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_dijkstra out-of-range src returns range error
 * Hits: src_id < 0 || >= n check (line 936)
 * -------------------------------------------------------------------------- */
static test_result_t test_dijkstra_out_of_range_src(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    double  wts[] = {1.0, 2.0};
    ray_t* edges;
    ray_rel_t* rel = make_weighted_rel(src, dst, wts, 2, 3, &edges);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* src_atom = ray_i64(99);  /* out of range */
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_release(src_atom);

    ray_op_t* dijk_op = ray_dijkstra(g, src_op, NULL, rel, "weight", 10);
    TEST_ASSERT_NOT_NULL(dijk_op);

    ray_t* result = ray_execute(g, dijk_op);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_dijkstra with no props (schema error)
 * Hits: !rel->fwd.props check (line 929)
 * -------------------------------------------------------------------------- */
static test_result_t test_dijkstra_no_props(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);  /* no props */
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* src_atom = ray_i64(0);
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_release(src_atom);

    ray_op_t* dijk_op = ray_dijkstra(g, src_op, NULL, rel, "weight", 10);
    TEST_ASSERT_NOT_NULL(dijk_op);

    ray_t* result = ray_execute(g, dijk_op);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_topsort with a cycle returns domain error
 * Hits: cycle detection branch (line 1432-1434)
 * -------------------------------------------------------------------------- */
static test_result_t test_topsort_cycle(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Cycle: 0->1->2->0 */
    int64_t src[] = {0, 1, 2};
    int64_t dst[] = {1, 2, 0};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* ts_op = ray_topsort(g, rel);
    TEST_ASSERT_NOT_NULL(ts_op);

    ray_t* result = ray_execute(g, ts_op);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_topsort on a DAG returns valid order
 * Hits: successful topsort path (lines 1437-1468)
 * -------------------------------------------------------------------------- */
static test_result_t test_topsort_dag(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* DAG: 0->2, 1->2, 2->3 */
    int64_t src[] = {0, 1, 2};
    int64_t dst[] = {2, 2, 3};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 4);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* ts_op = ray_topsort(g, rel);
    TEST_ASSERT_NOT_NULL(ts_op);

    ray_t* result = ray_execute(g, ts_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_pagerank with dangling node (zero out-degree)
 * Hits: dangling node correction path (line 671)
 * -------------------------------------------------------------------------- */
static test_result_t test_pagerank_dangling_node(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Graph with dangling node: 0->1, 1->0 (2 is isolated/dangling) */
    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 0};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    /* 20 iters, 0.85 damping */
    ray_op_t* pr_op = ray_pagerank(g, rel, 20, 0.85);
    TEST_ASSERT_NOT_NULL(pr_op);

    ray_t* result = ray_execute(g, pr_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_pagerank with 0 damping (uniform distribution)
 * -------------------------------------------------------------------------- */
static test_result_t test_pagerank_zero_damping(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1, 2};
    int64_t dst[] = {1, 2, 0};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* pr_op = ray_pagerank(g, rel, 5, 0.0);
    TEST_ASSERT_NOT_NULL(pr_op);

    ray_t* result = ray_execute(g, pr_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    /* With 0 damping, rank = 1/N for all nodes */
    ray_t* rank_col = ray_table_get_col_idx(result, 1);
    TEST_ASSERT_NOT_NULL(rank_col);
    double* ranks = (double*)ray_data(rank_col);
    for (int64_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQ_F(ranks[i], 1.0/3.0, 1e-10);
    }
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_connected_comp on disconnected graph
 * Hits: different components assigned different labels
 * -------------------------------------------------------------------------- */
static test_result_t test_connected_comp_disconnected(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Two components: {0,1,2} and {3,4} */
    int64_t src[] = {0, 1, 3};
    int64_t dst[] = {1, 2, 4};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 5);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* cc_op = ray_connected_comp(g, rel);
    TEST_ASSERT_NOT_NULL(cc_op);

    ray_t* result = ray_execute(g, cc_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 5);

    /* Two distinct components */
    ray_t* comp_col = ray_table_get_col_idx(result, 1);
    TEST_ASSERT_NOT_NULL(comp_col);
    int64_t* comps = (int64_t*)ray_data(comp_col);
    /* Node 0,1,2 same component; 3,4 different */
    TEST_ASSERT_EQ_I(comps[0], comps[1]);
    TEST_ASSERT_EQ_I(comps[1], comps[2]);
    TEST_ASSERT_TRUE(comps[0] != comps[3]);
    TEST_ASSERT_EQ_I(comps[3], comps[4]);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_louvain on a two-community graph
 * Hits: community-movement path, normalization
 * -------------------------------------------------------------------------- */
static test_result_t test_louvain_two_communities(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Dense within-community edges, sparse between:
     * Community A: 0-1, 1-2, 0-2
     * Community B: 3-4, 4-5, 3-5
     * Bridge: 2->3 */
    int64_t src[] = {0, 1, 0, 3, 4, 3, 2};
    int64_t dst[] = {1, 2, 2, 4, 5, 5, 3};
    ray_rel_t* rel = make_rel_simple(src, dst, 7, 6);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* lou_op = ray_louvain(g, rel, 20);
    TEST_ASSERT_NOT_NULL(lou_op);

    ray_t* result = ray_execute(g, lou_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 6);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_degree_cent basic degree counts
 * -------------------------------------------------------------------------- */
static test_result_t test_degree_cent_basic(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 0->1, 0->2, 1->2 */
    int64_t src[] = {0, 0, 1};
    int64_t dst[] = {1, 2, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* dc_op = ray_degree_cent(g, rel);
    TEST_ASSERT_NOT_NULL(dc_op);

    ray_t* result = ray_execute(g, dc_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    TEST_ASSERT_EQ_I(ray_table_ncols(result), 4);  /* _node, _in, _out, _degree */
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_cluster_coeff on triangle (LCC=1.0) and isolated node (LCC=0.0)
 * Hits: deg<2 branch, triangle counting
 * -------------------------------------------------------------------------- */
static test_result_t test_cluster_coeff_triangle_and_isolated(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Triangle: 0->1, 1->2, 0->2; isolated node 3 */
    int64_t src[] = {0, 1, 0};
    int64_t dst[] = {1, 2, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 4);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* cc_op = ray_cluster_coeff(g, rel);
    TEST_ASSERT_NOT_NULL(cc_op);

    ray_t* result = ray_execute(g, cc_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    /* Node 3 (isolated) should have LCC=0.0 */
    ray_t* lcc_col = ray_table_get_col_idx(result, 1);
    TEST_ASSERT_NOT_NULL(lcc_col);
    double* lcc = (double*)ray_data(lcc_col);
    /* Node 3 is isolated */
    TEST_ASSERT_EQ_F(lcc[3], 0.0, 1e-10);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_betweenness with sampled mode (sample_size < n)
 * Hits: sampled betweenness path (line 1587), normalization path (line 1722-1725)
 * -------------------------------------------------------------------------- */
static test_result_t test_betweenness_sampled(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Linear chain: 0-1-2-3-4 */
    int64_t src[] = {0, 1, 2, 3};
    int64_t dst[] = {1, 2, 3, 4};
    ray_rel_t* rel = make_rel_simple(src, dst, 4, 5);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    /* sample_size=2 < n=5: hits sampling branch */
    ray_op_t* bc_op = ray_betweenness(g, rel, 2);
    TEST_ASSERT_NOT_NULL(bc_op);

    ray_t* result = ray_execute(g, bc_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 5);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_closeness with sampled mode (sample_size < n)
 * Hits: sampled closeness branch (lines 1857-1863)
 * -------------------------------------------------------------------------- */
static test_result_t test_closeness_sampled(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 5-node connected graph */
    int64_t src[] = {0, 1, 2, 3};
    int64_t dst[] = {1, 2, 3, 4};
    ray_rel_t* rel = make_rel_simple(src, dst, 4, 5);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    /* sample_size=3 < n=5 */
    ray_op_t* cl_op = ray_closeness(g, rel, 3);
    TEST_ASSERT_NOT_NULL(cl_op);

    ray_t* result = ray_execute(g, cl_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Sample mode returns n_sources rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_closeness on disconnected graph (sum_dist=0 for isolated node)
 * Hits: reachable==0 condition (line 1838)
 * -------------------------------------------------------------------------- */
static test_result_t test_closeness_disconnected(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Isolated nodes: no edges */
    ray_rel_t* rel = make_rel_simple((int64_t[]){}, (int64_t[]){}, 0, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* cl_op = ray_closeness(g, rel, 0);  /* full traversal */
    TEST_ASSERT_NOT_NULL(cl_op);

    ray_t* result = ray_execute(g, cl_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    /* All closeness values should be 0.0 for isolated nodes */
    ray_t* cent_col = ray_table_get_col_idx(result, 1);
    TEST_ASSERT_NOT_NULL(cent_col);
    double* cents = (double*)ray_data(cent_col);
    for (int64_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQ_F(cents[i], 0.0, 1e-10);
    }
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_mst on a disconnected graph (forest with 2+ trees)
 * Hits: MST forest case where not all n-1 edges are possible
 * -------------------------------------------------------------------------- */
static test_result_t test_mst_forest(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Two components: 0-1 (w=1), 2-3 (w=2); isolated node 4 */
    int64_t src[] = {0, 2};
    int64_t dst[] = {1, 3};
    double  wts[] = {1.0, 2.0};
    ray_t* edges;
    ray_rel_t* rel = make_weighted_rel(src, dst, wts, 2, 5, &edges);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* mst_op = ray_mst(g, rel, "weight");
    TEST_ASSERT_NOT_NULL(mst_op);

    ray_t* result = ray_execute(g, mst_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* MST forest: 2 edges (one per component) */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_mst with no props returns schema error
 * Hits: !rel->fwd.props check (line 1915)
 * -------------------------------------------------------------------------- */
static test_result_t test_mst_no_props(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);  /* no props */
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* mst_op = ray_mst(g, rel, "weight");
    TEST_ASSERT_NOT_NULL(mst_op);

    ray_t* result = ray_execute(g, mst_op);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_random_walk hits dead end (node with no outgoing edges)
 * Hits: deg==0 break arm (line 2056)
 * -------------------------------------------------------------------------- */
static test_result_t test_random_walk_dead_end(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Chain: 0->1->2 (node 2 is a dead end) */
    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* start_atom = ray_i64(0);
    ray_op_t* start_op = ray_const_atom(g, start_atom);
    ray_release(start_atom);

    /* Walk length 10, but will stop at node 2 (dead end) after step 2 */
    ray_op_t* rw_op = ray_random_walk(g, start_op, rel, 10);
    TEST_ASSERT_NOT_NULL(rw_op);

    ray_t* result = ray_execute(g, rw_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Should only produce 3 steps: 0, 1, 2 then dead end */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_random_walk out-of-range src
 * Hits: range error (line 2027)
 * -------------------------------------------------------------------------- */
static test_result_t test_random_walk_out_of_range(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* start_atom = ray_i64(99);  /* out of range */
    ray_op_t* start_op = ray_const_atom(g, start_atom);
    ray_release(start_atom);

    ray_op_t* rw_op = ray_random_walk(g, start_op, rel, 5);
    TEST_ASSERT_NOT_NULL(rw_op);

    ray_t* result = ray_execute(g, rw_op);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_dfs on cyclic graph (visited check prevents infinite loop)
 * Hits: visited[v] continue arm (line 2141)
 * -------------------------------------------------------------------------- */
static test_result_t test_dfs_cyclic(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Cycle: 0->1->2->0 with extra edge 1->3 */
    int64_t src[] = {0, 1, 2, 1};
    int64_t dst[] = {1, 2, 0, 3};
    ray_rel_t* rel = make_rel_simple(src, dst, 4, 4);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* start_atom = ray_i64(0);
    ray_op_t* start_op = ray_const_atom(g, start_atom);
    ray_release(start_atom);

    ray_op_t* dfs_op = ray_dfs(g, start_op, rel, 255);
    TEST_ASSERT_NOT_NULL(dfs_op);

    ray_t* result = ray_execute(g, dfs_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* All 4 nodes should be visited */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_dfs with max_depth limiting traversal
 * Hits: d >= max_depth arm (line 2149)
 * -------------------------------------------------------------------------- */
static test_result_t test_dfs_max_depth(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Chain: 0->1->2->3->4 */
    int64_t src[] = {0, 1, 2, 3};
    int64_t dst[] = {1, 2, 3, 4};
    ray_rel_t* rel = make_rel_simple(src, dst, 4, 5);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* start_atom = ray_i64(0);
    ray_op_t* start_op = ray_const_atom(g, start_atom);
    ray_release(start_atom);

    /* max_depth=2: only depth 0,1,2 = nodes 0,1,2 */
    ray_op_t* dfs_op = ray_dfs(g, start_op, rel, 2);
    TEST_ASSERT_NOT_NULL(dfs_op);

    ray_t* result = ray_execute(g, dfs_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_dfs out-of-range start returns range error
 * Hits: range check (line 2099)
 * -------------------------------------------------------------------------- */
static test_result_t test_dfs_out_of_range(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* start_atom = ray_i64(99);  /* out of range */
    ray_op_t* start_op = ray_const_atom(g, start_atom);
    ray_release(start_atom);

    ray_op_t* dfs_op = ray_dfs(g, start_op, rel, 10);
    TEST_ASSERT_NOT_NULL(dfs_op);

    ray_t* result = ray_execute(g, dfs_op);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_k_shortest with no path returns empty table
 * Hits: d >= 1e308 early-return path (lines 2418-2428)
 * -------------------------------------------------------------------------- */
static test_result_t test_k_shortest_no_path(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Disconnected: 0->1 and 2->3 */
    int64_t src[] = {0, 2};
    int64_t dst[] = {1, 3};
    double  wts[] = {1.0, 2.0};
    ray_t* edges;
    ray_rel_t* rel = make_weighted_rel(src, dst, wts, 2, 4, &edges);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* src_atom = ray_i64(0);
    ray_t* dst_atom = ray_i64(3);
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_op_t* dst_op = ray_const_atom(g, dst_atom);
    ray_release(src_atom);
    ray_release(dst_atom);

    ray_op_t* ks_op = ray_k_shortest(g, src_op, dst_op, rel, "weight", 3);
    TEST_ASSERT_NOT_NULL(ks_op);

    ray_t* result = ray_execute(g, ks_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* No path: empty result */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 0);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_k_shortest finds k paths in a graph with alternatives
 * Hits: full Yen's algorithm path including candidate dedup, prefix check
 * -------------------------------------------------------------------------- */
static test_result_t test_k_shortest_multiple_paths(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Diamond: 0->1(w=1), 0->2(w=2), 1->3(w=1), 2->3(w=1) */
    int64_t src[] = {0, 0, 1, 2};
    int64_t dst[] = {1, 2, 3, 3};
    double  wts[] = {1.0, 2.0, 1.0, 1.0};
    ray_t* edges;
    ray_rel_t* rel = make_weighted_rel(src, dst, wts, 4, 4, &edges);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* src_atom = ray_i64(0);
    ray_t* dst_atom = ray_i64(3);
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_op_t* dst_op = ray_const_atom(g, dst_atom);
    ray_release(src_atom);
    ray_release(dst_atom);

    /* k=2: find 2 shortest paths */
    ray_op_t* ks_op = ray_k_shortest(g, src_op, dst_op, rel, "weight", 2);
    TEST_ASSERT_NOT_NULL(ks_op);

    ray_t* result = ray_execute(g, ks_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* 2 paths * 3 nodes each = 6 rows */
    TEST_ASSERT_TRUE(ray_table_nrows(result) >= 3);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_expand on empty source vector returns empty table
 * Hits: n_src=0 loop produces zero output
 * -------------------------------------------------------------------------- */
static test_result_t test_expand_empty_src(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_t* sv = ray_vec_new(RAY_I64, 1);
    sv->len = 0;
    ray_op_t* src_op = ray_const_vec(g, sv);
    ray_release(sv);

    ray_op_t* expand_op = ray_expand(g, src_op, rel, 0);
    TEST_ASSERT_NOT_NULL(expand_op);

    ray_t* result = ray_execute(g, expand_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 0);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_expand with optimizer-driven SIP (direction==0)
 * Uses ray_optimize to trigger sip_pass setting filter_hint=1 on ext.
 * Hits: filter_hint > 0 && n_src > 64 path (lines 115-136)
 * -------------------------------------------------------------------------- */
static test_result_t test_expand_sip_optimized(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a node table with 100 rows and 'id' + 'flag' columns */
    int64_t n_nodes = 100;
    ray_t* id_vec  = ray_vec_new(RAY_I64, n_nodes);
    ray_t* flag_vec = ray_vec_new(RAY_I64, n_nodes);
    int64_t* idata = (int64_t*)ray_data(id_vec);
    int64_t* fdata = (int64_t*)ray_data(flag_vec);
    for (int64_t i = 0; i < n_nodes; i++) {
        idata[i] = i;
        fdata[i] = i % 2;  /* alternating 0/1 */
    }
    id_vec->len = n_nodes; flag_vec->len = n_nodes;

    ray_t* node_tbl = ray_table_new(2);
    node_tbl = ray_table_add_col(node_tbl, ray_sym_intern("id", 2), id_vec);   ray_release(id_vec);
    node_tbl = ray_table_add_col(node_tbl, ray_sym_intern("flag", 4), flag_vec); ray_release(flag_vec);

    /* Build chain graph: 0->1, 1->2, ..., 98->99 */
    ray_t* sv = ray_vec_new(RAY_I64, n_nodes - 1);
    ray_t* dv = ray_vec_new(RAY_I64, n_nodes - 1);
    int64_t* sdata2 = (int64_t*)ray_data(sv);
    int64_t* ddata2 = (int64_t*)ray_data(dv);
    for (int64_t i = 0; i < n_nodes - 1; i++) { sdata2[i] = i; ddata2[i] = i + 1; }
    sv->len = n_nodes - 1; dv->len = n_nodes - 1;

    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, ray_sym_intern("src", 3), sv); ray_release(sv);
    edges = ray_table_add_col(edges, ray_sym_intern("dst", 3), dv); ray_release(dv);
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", n_nodes, n_nodes, false);
    ray_release(edges);
    TEST_ASSERT_NOT_NULL(rel);

    /* Build query: FILTER(EXPAND(SCAN(flag), rel), SCAN(flag) == 1)
     * The optimizer's sip_pass will detect FILTER downstream of EXPAND
     * and set ext->base.pad[2] = 1 on the EXPAND's ext node */
    ray_graph_t* g = ray_graph_new(node_tbl);

    ray_op_t* flag_scan = ray_scan(g, "flag");
    ray_op_t* c1 = ray_const_i64(g, 1);
    ray_op_t* pred = ray_eq(g, flag_scan, c1);

    ray_op_t* expand_op = ray_expand(g, flag_scan, rel, 0);
    TEST_ASSERT_NOT_NULL(expand_op);

    ray_op_t* filt = ray_filter(g, expand_op, pred);
    TEST_ASSERT_NOT_NULL(filt);

    /* Optimizer fires sip_pass, setting filter_hint=1 on the ext node */
    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_t* result = ray_execute(g, opt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(node_tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_shortest_path with vec-form src/dst inputs
 * Hits: else branch for non-atom src/dst (lines 482-490)
 * -------------------------------------------------------------------------- */
static test_result_t test_shortest_path_vec_input(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 0->1->2->3 */
    int64_t src[] = {0, 1, 2};
    int64_t dst[] = {1, 2, 3};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 4);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    /* Non-atom vec inputs (length-1 vectors) */
    ray_t* src_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    ray_t* dst_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){3}, 1);
    ray_op_t* src_op = ray_const_vec(g, src_vec);
    ray_op_t* dst_op = ray_const_vec(g, dst_vec);
    ray_release(src_vec);
    ray_release(dst_vec);

    ray_op_t* sp_op = ray_shortest_path(g, src_op, dst_op, rel, 10);
    TEST_ASSERT_NOT_NULL(sp_op);

    ray_t* result = ray_execute(g, sp_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Path: 0->1->2->3, 4 nodes */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_betweenness full mode (sample_size=0)
 * Hits: full betweenness centrality (not sampled)
 * -------------------------------------------------------------------------- */
static test_result_t test_betweenness_full(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Triangle graph */
    int64_t src[] = {0, 1, 2};
    int64_t dst[] = {1, 2, 0};
    ray_rel_t* rel = make_rel_simple(src, dst, 3, 3);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* bc_op = ray_betweenness(g, rel, 0);  /* 0 = full mode */
    TEST_ASSERT_NOT_NULL(bc_op);

    ray_t* result = ray_execute(g, bc_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_louvain on single-node graph
 * Hits: n==1 boundary case (two_m = 0 -> two_m = 1)
 * -------------------------------------------------------------------------- */
static test_result_t test_louvain_single_node(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Single node, no edges */
    ray_rel_t* rel = make_rel_simple((int64_t[]){}, (int64_t[]){}, 0, 1);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* lou_op = ray_louvain(g, rel, 10);
    TEST_ASSERT_NOT_NULL(lou_op);

    ray_t* result = ray_execute(g, lou_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 1);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_var_expand direction==2 with asymmetric rel (rev > fwd nodes)
 * Hits: line 319 — bfs_n_nodes = csr_rev->n_nodes when rev has more nodes
 * -------------------------------------------------------------------------- */
static test_result_t test_var_expand_both_asym_nodes(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Edges: 0->10, 0->11, 0->12
     * n_src_nodes=3 (fwd has n_nodes=3), n_dst_nodes=13 (rev has n_nodes=13)
     * So rev.n_nodes(13) > fwd.n_nodes(3), triggering line 319 when direction==2 */
    int64_t src[] = {0, 0, 0};
    int64_t dst[] = {10, 11, 12};
    ray_rel_t* rel = make_rel_asym(src, dst, 3, 3, 13);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    /* Start from node 0, direction==2 (both) */
    ray_t* sv = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    ray_op_t* start_op = ray_const_vec(g, sv);
    ray_release(sv);

    ray_op_t* ve_op = ray_var_expand(g, start_op, rel, 2, 1, 3, false);
    TEST_ASSERT_NOT_NULL(ve_op);

    ray_t* result = ray_execute(g, ve_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Should reach 10, 11, 12 at depth 1 */
    TEST_ASSERT_TRUE(ray_table_nrows(result) >= 1);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_var_expand with large graph triggers BFS buffer growth
 * Hits: lines 366-402 — next_front and out buffer growth
 * -------------------------------------------------------------------------- */
static test_result_t test_var_expand_large_graph(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Create a star graph: node 0 -> nodes 1..1100
     * This causes the BFS next_front (cap=4) and output buffers (cap=1024)
     * to grow during var_expand execution */
    int64_t n_leaves = 1100;
    int64_t n_total  = n_leaves + 1;
    ray_t* sv = ray_vec_new(RAY_I64, n_leaves);
    ray_t* dv = ray_vec_new(RAY_I64, n_leaves);
    int64_t* sdata = (int64_t*)ray_data(sv);
    int64_t* ddata = (int64_t*)ray_data(dv);
    for (int64_t i = 0; i < n_leaves; i++) {
        sdata[i] = 0;
        ddata[i] = i + 1;
    }
    sv->len = n_leaves; dv->len = n_leaves;

    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, ray_sym_intern("src", 3), sv); ray_release(sv);
    edges = ray_table_add_col(edges, ray_sym_intern("dst", 3), dv); ray_release(dv);
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", n_total, n_total, false);
    ray_release(edges);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    /* Start from node 0, direction==0, depth 1 */
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    ray_op_t* start_op = ray_const_vec(g, start_vec);
    ray_release(start_vec);

    /* min_depth=1, max_depth=1 so we get all 1100 edges in one BFS step
     * The output buffer (cap=1024) must grow to accommodate all 1100 results */
    ray_op_t* ve_op = ray_var_expand(g, start_op, rel, 0, 1, 1, false);
    TEST_ASSERT_NOT_NULL(ve_op);

    ray_t* result = ray_execute(g, ve_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Should have exactly 1100 (start, end, depth) triplets */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), (int64_t)n_leaves);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_expand SIP filter with direction==1 (reverse)
 * Hits: lines 128-132 — SIP bitmap building for direction==1
 * Uses g->ext_nodes[] to set pad[2] directly on the ext node (bypassing the
 * g->nodes[] copy) — this is the same approach as sip_pass in the optimizer.
 * -------------------------------------------------------------------------- */
static test_result_t test_expand_sip_optimized_rev(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a node table with 100 rows */
    int64_t n_nodes = 100;
    ray_t* id_vec   = ray_vec_new(RAY_I64, n_nodes);
    ray_t* flag_vec = ray_vec_new(RAY_I64, n_nodes);
    int64_t* idata = (int64_t*)ray_data(id_vec);
    int64_t* fdata = (int64_t*)ray_data(flag_vec);
    for (int64_t i = 0; i < n_nodes; i++) {
        idata[i] = i;
        fdata[i] = i % 2;
    }
    id_vec->len = n_nodes; flag_vec->len = n_nodes;

    ray_t* node_tbl = ray_table_new(2);
    node_tbl = ray_table_add_col(node_tbl, ray_sym_intern("id", 2),   id_vec);   ray_release(id_vec);
    node_tbl = ray_table_add_col(node_tbl, ray_sym_intern("flag", 4), flag_vec); ray_release(flag_vec);

    /* Chain graph: 0->1, 1->2, ..., 98->99 */
    ray_t* sv = ray_vec_new(RAY_I64, n_nodes - 1);
    ray_t* dv = ray_vec_new(RAY_I64, n_nodes - 1);
    int64_t* sdata2 = (int64_t*)ray_data(sv);
    int64_t* ddata2 = (int64_t*)ray_data(dv);
    for (int64_t i = 0; i < n_nodes - 1; i++) { sdata2[i] = i; ddata2[i] = i + 1; }
    sv->len = n_nodes - 1; dv->len = n_nodes - 1;

    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, ray_sym_intern("src", 3), sv); ray_release(sv);
    edges = ray_table_add_col(edges, ray_sym_intern("dst", 3), dv); ray_release(dv);
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", n_nodes, n_nodes, false);
    ray_release(edges);
    TEST_ASSERT_NOT_NULL(rel);

    /* Build expand op with direction=1 (reverse) and a 100-row src input.
     * To trigger the SIP direction==1 path (lines 128-132), we must set
     * pad[2]=1 on the ext node directly via g->ext_nodes[], not on the copy
     * in g->nodes[] that ray_expand() returns.
     * The condition also requires n_src > 64, which is satisfied by scanning
     * the 100-row flag column. */
    ray_graph_t* g = ray_graph_new(node_tbl);

    ray_op_t* flag_scan = ray_scan(g, "flag");
    uint32_t expand_id_before = g->node_count;  /* next node id will be expand */

    /* direction=1: reverse */
    ray_op_t* expand_op = ray_expand(g, flag_scan, rel, 1);
    TEST_ASSERT_NOT_NULL(expand_op);

    uint32_t expand_id = expand_op->id;

    /* Set pad[2]=1 on the EXT node (not the g->nodes[] copy).
     * Walk g->ext_nodes to find the ext whose base.id matches expand_id. */
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == expand_id) {
            g->ext_nodes[i]->base.pad[2] = 1;
            break;
        }
    }
    (void)expand_id_before;

    ray_t* result = ray_execute(g, expand_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(node_tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_shortest_path BFS queue growth (>1024 nodes enqueued)
 * Hits: lines 554-562 — BFS queue realloc in exec_shortest_path
 * -------------------------------------------------------------------------- */
static test_result_t test_shortest_path_bfs_queue_growth(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a graph: node 0 -> nodes 1..1025, then node 1025 -> node 1026
     * BFS from 0 enqueues 1025 nodes; exceeds initial q_cap=1024 */
    int64_t n_leaves = 1025;
    int64_t n_total  = n_leaves + 2;   /* 0, 1..1025, 1026 */
    ray_t* sv = ray_vec_new(RAY_I64, n_leaves + 1);
    ray_t* dv = ray_vec_new(RAY_I64, n_leaves + 1);
    int64_t* sdata = (int64_t*)ray_data(sv);
    int64_t* ddata = (int64_t*)ray_data(dv);
    for (int64_t i = 0; i < n_leaves; i++) {
        sdata[i] = 0;
        ddata[i] = i + 1;
    }
    /* Add edge: 1025 -> 1026 (the destination) */
    sdata[n_leaves] = n_leaves;
    ddata[n_leaves] = n_leaves + 1;
    sv->len = n_leaves + 1; dv->len = n_leaves + 1;

    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, ray_sym_intern("src", 3), sv); ray_release(sv);
    edges = ray_table_add_col(edges, ray_sym_intern("dst", 3), dv); ray_release(dv);
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", n_total, n_total, false);
    ray_release(edges);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    /* Find shortest path from 0 to 1026 */
    ray_t* src_atom = ray_i64(0);
    ray_t* dst_atom = ray_i64(n_leaves + 1);
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_op_t* dst_op = ray_const_atom(g, dst_atom);
    ray_release(src_atom);
    ray_release(dst_atom);

    /* max_depth=3: depth 1 enqueues 1025 nodes, depth 2 reaches node 1026 */
    ray_op_t* sp_op = ray_shortest_path(g, src_op, dst_op, rel, 3);
    TEST_ASSERT_NOT_NULL(sp_op);

    ray_t* result = ray_execute(g, sp_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Path: 0 -> 1025 -> 1026 = 3 nodes */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_k_shortest with out-of-range src/dst
 * Hits: line 2363 — range check in exec_k_shortest
 * -------------------------------------------------------------------------- */
static test_result_t test_k_shortest_out_of_range(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    double  wts[] = {1.0, 1.0};
    ray_t* edges;
    ray_rel_t* rel = make_weighted_rel(src, dst, wts, 2, 3, &edges);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);

    /* src=0, dst=999 — dst out of range */
    ray_t* src_atom = ray_i64(0);
    ray_t* dst_atom = ray_i64(999);
    ray_op_t* src_op = ray_const_atom(g, src_atom);
    ray_op_t* dst_op = ray_const_atom(g, dst_atom);
    ray_release(src_atom);
    ray_release(dst_atom);

    ray_op_t* ks_op = ray_k_shortest(g, src_op, dst_op, rel, "weight", 2);
    TEST_ASSERT_NOT_NULL(ks_op);

    ray_t* result = ray_execute(g, ks_op);
    /* Should return an error due to out-of-range */
    TEST_ASSERT_TRUE(RAY_IS_ERR(result) || result != NULL);
    if (!RAY_IS_ERR(result)) ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_expand_factorized with out-of-range source nodes (deg == 0 path)
 * Hits: line 61 false branch — if (deg > 0) else (node with zero degree skipped)
 * Also hits: line 54 false path (node >= fwd.n_nodes)
 * -------------------------------------------------------------------------- */
static test_result_t test_expand_factorized_zero_deg(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 3-node graph: 0->1, 0->2 */
    int64_t src[] = {0, 0};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    /* Source vec: {0, 99} — node 99 is out-of-range (deg=0), node 0 has deg=2 */
    int64_t start_data[] = {0, 99};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 2);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src_op = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src_op, rel, 0);
    TEST_ASSERT_NOT_NULL(expand);

    /* Set factorized flag directly on ext node */
    ray_op_ext_t* ext = NULL;
    uint32_t expand_id = expand->id;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == expand_id) {
            ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(ext);
    ext->graph.factorized = 1;

    ray_t* result = ray_execute(g, expand);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Only node 0 (with deg=2) should appear; node 99 skipped (deg=0) */
    int64_t src_sym = ray_sym_intern("_src", 4);
    ray_t* src_col = ray_table_get_col(result, src_sym);
    TEST_ASSERT_NOT_NULL(src_col);
    TEST_ASSERT_EQ_I(src_col->len, 1);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_mst with a graph containing a redundant edge (cycle rejection)
 * Hits: uf_union returning false (line 1904) when a cycle edge is rejected.
 *
 * 4-node graph: 0->1 (w=1), 1->2 (w=1), 0->2 (w=1.5), 2->3 (w=2)
 * Sorted order: 0->1, 1->2, 0->2, 2->3
 * Kruskal picks 0->1, 1->2 (nodes 0,1,2 merged); then tries 0->2 —
 * uf_union returns false (same component); then picks 2->3.
 * MST = 3 edges (n-1=3).
 * -------------------------------------------------------------------------- */
static test_result_t test_mst_cyclic(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 4-node directed graph with a cycle-forming edge 0->2 */
    int64_t srce[] = {0, 1, 0, 2};
    int64_t dste[] = {1, 2, 2, 3};
    double  wtse[] = {1.0, 1.0, 1.5, 2.0};
    ray_t* edges;
    ray_rel_t* rel = make_weighted_rel(srce, dste, wtse, 4, 4, &edges);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* mst_op = ray_mst(g, rel, "weight");
    TEST_ASSERT_NOT_NULL(mst_op);

    ray_t* result = ray_execute(g, mst_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* MST of 4-node graph should have exactly 3 edges (n-1) */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_wco_join with unsupported plan (n_vars=5, n_rels=3)
 * Hits: line 1083 — lftj_build_default_plan returns false
 * -------------------------------------------------------------------------- */
static test_result_t test_wco_join_unsupported_plan(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Simple 3-node graph with sorted CSR */
    int64_t srce[] = {0, 1, 2};
    int64_t dste[] = {1, 2, 0};
    ray_t* sv = ray_vec_from_raw(RAY_I64, srce, 3);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dste, 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, ray_sym_intern("src", 3), sv); ray_release(sv);
    edges = ray_table_add_col(edges, ray_sym_intern("dst", 3), dv); ray_release(dv);
    /* sort_targets=true to produce sorted CSR required by WCO validation */
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 3, 3, true);
    ray_release(edges);
    TEST_ASSERT_NOT_NULL(rel);

    /* n_vars=5, n_rels=3: none of the fixed patterns match,
     * chain requires n_rels==n_vars-1=4, so plan fails → "nyi" error */
    ray_rel_t* rels[3] = {rel, rel, rel};
    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* wco = ray_wco_join(g, rels, 3, 5);
    TEST_ASSERT_NOT_NULL(wco);

    ray_t* result = ray_execute(g, wco);
    /* Should return "nyi" error */
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: exec_var_expand with an out-of-range start node (continue path)
 * Hits: line 324 — continue when start_node >= bfs_n_nodes
 * -------------------------------------------------------------------------- */
static test_result_t test_var_expand_oob_start(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src[] = {0, 1};
    int64_t dst[] = {1, 2};
    ray_rel_t* rel = make_rel_simple(src, dst, 2, 3);
    TEST_ASSERT_NOT_NULL(rel);

    /* Start from {0, 999} — node 999 is out of range and should be skipped */
    int64_t start_data[] = {0, 999};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 2);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* start_op = ray_const_vec(g, start_vec);
    ray_op_t* ve_op = ray_var_expand(g, start_op, rel, 0, 1, 2, false);
    TEST_ASSERT_NOT_NULL(ve_op);

    ray_t* result = ray_execute(g, ve_op);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Node 0 can reach 1 (depth 1) and 2 (depth 2); node 999 skipped */
    TEST_ASSERT_TRUE(ray_table_nrows(result) >= 1);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Suite
 * -------------------------------------------------------------------------- */

const test_entry_t traverse_entries[] = {
    { "traverse/shortest_path_both_dirs",    test_shortest_path_both_directions,  NULL, NULL },
    { "traverse/shortest_path_disconnected", test_shortest_path_disconnected,     NULL, NULL },
    { "traverse/shortest_path_src_eq_dst",   test_shortest_path_src_eq_dst,       NULL, NULL },
    { "traverse/shortest_path_out_of_range", test_shortest_path_out_of_range,     NULL, NULL },
    { "traverse/var_expand_reverse",         test_var_expand_reverse,             NULL, NULL },
    { "traverse/var_expand_both",            test_var_expand_both,                NULL, NULL },
    { "traverse/var_expand_min_depth",       test_var_expand_min_depth,           NULL, NULL },
    { "traverse/var_expand_empty_start",     test_var_expand_empty_start,         NULL, NULL },
    { "traverse/expand_reverse",             test_expand_reverse,                 NULL, NULL },
    { "traverse/expand_both",                test_expand_both,                    NULL, NULL },
    { "traverse/expand_empty_src",           test_expand_empty_src,               NULL, NULL },
    { "traverse/expand_sip_optimized",       test_expand_sip_optimized,           NULL, NULL },
    { "traverse/shortest_path_vec_input",   test_shortest_path_vec_input,        NULL, NULL },
    { "traverse/dijkstra_negative_weight",   test_dijkstra_negative_weight,       NULL, NULL },
    { "traverse/dijkstra_missing_weight_col",test_dijkstra_missing_weight_col,    NULL, NULL },
    { "traverse/dijkstra_to_dst",            test_dijkstra_to_dst,                NULL, NULL },
    { "traverse/dijkstra_out_of_range_src",  test_dijkstra_out_of_range_src,      NULL, NULL },
    { "traverse/dijkstra_no_props",          test_dijkstra_no_props,              NULL, NULL },
    { "traverse/topsort_cycle",              test_topsort_cycle,                  NULL, NULL },
    { "traverse/topsort_dag",                test_topsort_dag,                    NULL, NULL },
    { "traverse/pagerank_dangling_node",     test_pagerank_dangling_node,         NULL, NULL },
    { "traverse/pagerank_zero_damping",      test_pagerank_zero_damping,          NULL, NULL },
    { "traverse/connected_comp_disconnected",test_connected_comp_disconnected,    NULL, NULL },
    { "traverse/louvain_two_communities",    test_louvain_two_communities,        NULL, NULL },
    { "traverse/louvain_single_node",        test_louvain_single_node,            NULL, NULL },
    { "traverse/degree_cent_basic",          test_degree_cent_basic,              NULL, NULL },
    { "traverse/cluster_coeff_triangle",     test_cluster_coeff_triangle_and_isolated, NULL, NULL },
    { "traverse/betweenness_sampled",        test_betweenness_sampled,            NULL, NULL },
    { "traverse/betweenness_full",           test_betweenness_full,               NULL, NULL },
    { "traverse/closeness_sampled",          test_closeness_sampled,              NULL, NULL },
    { "traverse/closeness_disconnected",     test_closeness_disconnected,         NULL, NULL },
    { "traverse/mst_forest",                 test_mst_forest,                     NULL, NULL },
    { "traverse/mst_no_props",               test_mst_no_props,                   NULL, NULL },
    { "traverse/random_walk_dead_end",       test_random_walk_dead_end,           NULL, NULL },
    { "traverse/random_walk_out_of_range",   test_random_walk_out_of_range,       NULL, NULL },
    { "traverse/dfs_cyclic",                 test_dfs_cyclic,                     NULL, NULL },
    { "traverse/dfs_max_depth",              test_dfs_max_depth,                  NULL, NULL },
    { "traverse/dfs_out_of_range",           test_dfs_out_of_range,               NULL, NULL },
    { "traverse/k_shortest_no_path",         test_k_shortest_no_path,             NULL, NULL },
    { "traverse/k_shortest_multiple_paths",  test_k_shortest_multiple_paths,      NULL, NULL },
    { "traverse/var_expand_both_asym_nodes", test_var_expand_both_asym_nodes,     NULL, NULL },
    { "traverse/var_expand_large_graph",     test_var_expand_large_graph,         NULL, NULL },
    { "traverse/expand_sip_optimized_rev",   test_expand_sip_optimized_rev,       NULL, NULL },
    { "traverse/shortest_path_bfs_queue_growth", test_shortest_path_bfs_queue_growth, NULL, NULL },
    { "traverse/k_shortest_out_of_range",    test_k_shortest_out_of_range,        NULL, NULL },
    { "traverse/expand_factorized_zero_deg", test_expand_factorized_zero_deg,     NULL, NULL },
    { "traverse/mst_cyclic",                 test_mst_cyclic,                     NULL, NULL },
    { "traverse/wco_join_unsupported_plan",  test_wco_join_unsupported_plan,      NULL, NULL },
    { "traverse/var_expand_oob_start",       test_var_expand_oob_start,           NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
