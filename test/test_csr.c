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

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "store/csr.h"
#include "ops/fvec.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Helper: create a simple graph with edges
 *
 *   0 -> 1, 0 -> 2, 1 -> 2, 1 -> 3, 2 -> 3, 3 -> 0
 *   (6 edges, 4 nodes — a cycle)
 * -------------------------------------------------------------------------- */

static ray_t* make_edge_table(void) {
    int64_t src_data[] = {0, 0, 1, 1, 2, 3};
    int64_t dst_data[] = {1, 2, 2, 3, 3, 0};
    int64_t n = 6;

    ray_t* src_vec = ray_vec_from_raw(RAY_I64, src_data, n);
    ray_t* dst_vec = ray_vec_from_raw(RAY_I64, dst_data, n);

    int64_t src_sym = ray_sym_intern("src", 3);
    int64_t dst_sym = ray_sym_intern("dst", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, src_sym, src_vec);
    tbl = ray_table_add_col(tbl, dst_sym, dst_vec);

    ray_release(src_vec);
    ray_release(dst_vec);
    return tbl;
}

/* --------------------------------------------------------------------------
 * Test: CSR build from edges
 * -------------------------------------------------------------------------- */

static test_result_t test_csr_build(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    TEST_ASSERT_NOT_NULL(edges);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    /* Forward CSR: check degrees */
    TEST_ASSERT_TRUE(rel->fwd.n_nodes == 4);
    TEST_ASSERT_TRUE(rel->fwd.n_edges == 6);
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->fwd, 0) == 2);  /* 0->1, 0->2 */
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->fwd, 1) == 2);  /* 1->2, 1->3 */
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->fwd, 2) == 1);  /* 2->3 */
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->fwd, 3) == 1);  /* 3->0 */

    /* Check neighbors of node 0 */
    int64_t cnt;
    int64_t* nbrs = ray_csr_neighbors(&rel->fwd, 0, &cnt);
    TEST_ASSERT_TRUE(cnt == 2);
    /* Neighbors should be 1 and 2 (order may vary) */
    TEST_ASSERT_TRUE(nbrs[0] == 1 || nbrs[0] == 2);
    TEST_ASSERT_TRUE(nbrs[1] == 1 || nbrs[1] == 2);
    TEST_ASSERT_TRUE(nbrs[0] != nbrs[1]);

    /* Reverse CSR */
    TEST_ASSERT_TRUE(rel->rev.n_nodes == 4);
    TEST_ASSERT_TRUE(rel->rev.n_edges == 6);
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->rev, 0) == 1);  /* 3->0 */
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->rev, 1) == 1);  /* 0->1 */
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->rev, 2) == 2);  /* 0->2, 1->2 */
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->rev, 3) == 2);  /* 1->3, 2->3 */

    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: CSR sorted (for LFTJ)
 * -------------------------------------------------------------------------- */

static test_result_t test_csr_sorted(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, true);
    TEST_ASSERT_NOT_NULL(rel);
    TEST_ASSERT_TRUE(rel->fwd.sorted);
    TEST_ASSERT_TRUE(rel->rev.sorted);

    /* Check sorted adjacency list for node 0 (fwd) */
    int64_t cnt;
    int64_t* nbrs = ray_csr_neighbors(&rel->fwd, 0, &cnt);
    TEST_ASSERT_TRUE(cnt == 2);
    TEST_ASSERT_TRUE(nbrs[0] == 1);
    TEST_ASSERT_TRUE(nbrs[1] == 2);

    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_EXPAND (1-hop forward)
 * -------------------------------------------------------------------------- */

static test_result_t test_expand(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    /* Expand from nodes {0, 1} forward */
    int64_t start_data[] = {0, 1};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 2);

    ray_graph_t* g = ray_graph_new(NULL);
    TEST_ASSERT_NOT_NULL(g);

    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src, rel, 0);
    TEST_ASSERT_NOT_NULL(expand);

    ray_t* result = ray_execute(g, expand);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Node 0 has 2 outgoing, node 1 has 2 outgoing = 4 total */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 4);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_EXPAND (reverse)
 * -------------------------------------------------------------------------- */

static test_result_t test_expand_reverse(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    /* Expand from node 3 reverse — should find nodes pointing TO 3: {1, 2} */
    int64_t start_data[] = {3};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 1);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src, rel, 1);  /* direction=1: reverse */

    ray_t* result = ray_execute(g, expand);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 2);  /* 1->3, 2->3 */

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_VAR_EXPAND (variable-length BFS)
 * -------------------------------------------------------------------------- */

static test_result_t test_var_expand(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    int64_t start_data[] = {0};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 1);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* var_exp = ray_var_expand(g, src, rel, 0, 1, 3, false);

    ray_t* result = ray_execute(g, var_exp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* From node 0 with depth 1..3:
     * depth 1: 0->1, 0->2 (2 results)
     * depth 2: 1->3, 2->3 (but 3 visited only once) => 1 result
     * depth 3: 3->0 (but 0 already visited) => no results
     * Total reachable: nodes 1, 2, 3 at depths 1, 1, 2 = 3 results */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_SHORTEST_PATH
 * -------------------------------------------------------------------------- */

static test_result_t test_shortest_path(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_i64(g, 0);
    ray_op_t* dst = ray_const_i64(g, 3);
    ray_op_t* sp = ray_shortest_path(g, src, dst, rel, 10);

    ray_t* result = ray_execute(g, sp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Shortest path 0->3: 0->1->3 (length 3 nodes) or 0->2->3 */
    int64_t nrows = ray_table_nrows(result);
    TEST_ASSERT_TRUE(nrows == 3);  /* 3 nodes in path */

    /* First node should be 0, last should be 3 */
    int64_t node_sym = ray_sym_intern("_node", 5);
    ray_t* node_col = ray_table_get_col(result, node_sym);
    TEST_ASSERT_NOT_NULL(node_col);
    int64_t* nodes = (int64_t*)ray_data(node_col);
    TEST_ASSERT_TRUE(nodes[0] == 0);
    TEST_ASSERT_TRUE(nodes[nrows - 1] == 3);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_SHORTEST_PATH (no path)
 * -------------------------------------------------------------------------- */

static test_result_t test_shortest_path_no_path(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a graph with no path from 0 to 3: only 0->1 */
    int64_t src_data[] = {0};
    int64_t dst_data[] = {1};
    ray_t* s = ray_vec_from_raw(RAY_I64, src_data, 1);
    ray_t* d = ray_vec_from_raw(RAY_I64, dst_data, 1);
    int64_t src_sym = ray_sym_intern("src", 3);
    int64_t dst_sym = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, src_sym, s);
    edges = ray_table_add_col(edges, dst_sym, d);
    ray_release(s); ray_release(d);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src_op = ray_const_i64(g, 0);
    ray_op_t* dst_op = ray_const_i64(g, 3);
    ray_op_t* sp = ray_shortest_path(g, src_op, dst_op, rel, 10);

    ray_t* result = ray_execute(g, sp);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    TEST_ASSERT_STR_EQ(ray_err_code(result), "range");
    ray_release(result);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_WCO_JOIN (triangle enumeration)
 * -------------------------------------------------------------------------- */

static test_result_t test_wco_join_triangle(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Complete graph K3: 0<->1, 1<->2, 0<->2 */
    int64_t src_data[] = {0, 0, 1, 1, 2, 2};
    int64_t dst_data[] = {1, 2, 0, 2, 0, 1};
    int64_t n = 6;

    ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, n);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, n);
    int64_t src_sym = ray_sym_intern("src", 3);
    int64_t dst_sym = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, src_sym, sv);
    edges = ray_table_add_col(edges, dst_sym, dv);
    ray_release(sv); ray_release(dv);

    /* Build with sorted=true (required for LFTJ) */
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 3, 3, true);
    TEST_ASSERT_NOT_NULL(rel);
    TEST_ASSERT_TRUE(rel->fwd.sorted);

    /* Triangle pattern: 3 vars, 3 rels (all same rel for K3) */
    ray_rel_t* rels[3] = {rel, rel, rel};

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* wco = ray_wco_join(g, rels, 3, 3);
    TEST_ASSERT_NOT_NULL(wco);

    ray_t* result = ray_execute(g, wco);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* K3 has directed triangles */
    TEST_ASSERT_TRUE(ray_table_nrows(result) >= 1);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: Multi-table graph (ray_graph_add_table + ray_scan_table)
 * -------------------------------------------------------------------------- */

static test_result_t test_multi_table(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Table 1: persons */
    int64_t ages[] = {25, 30, 35, 40};
    ray_t* age_vec = ray_vec_from_raw(RAY_I64, ages, 4);
    int64_t age_sym = ray_sym_intern("age", 3);
    ray_t* persons = ray_table_new(1);
    persons = ray_table_add_col(persons, age_sym, age_vec);
    ray_release(age_vec);

    /* Table 2: tasks */
    int64_t priorities[] = {1, 2, 3};
    ray_t* prio_vec = ray_vec_from_raw(RAY_I64, priorities, 3);
    int64_t prio_sym = ray_sym_intern("priority", 8);
    ray_t* tasks = ray_table_new(1);
    tasks = ray_table_add_col(tasks, prio_sym, prio_vec);
    ray_release(prio_vec);

    ray_graph_t* g = ray_graph_new(persons);  /* primary table */
    uint16_t tasks_id = ray_graph_add_table(g, tasks);
    TEST_ASSERT_TRUE(tasks_id == 0);

    /* Scan from persons (primary) */
    ray_op_t* age_scan = ray_scan(g, "age");
    TEST_ASSERT_NOT_NULL(age_scan);

    /* Scan from tasks (registered table) */
    ray_op_t* prio_scan = ray_scan_table(g, tasks_id, "priority");
    TEST_ASSERT_NOT_NULL(prio_scan);

    /* Execute scans */
    ray_t* age_result = ray_execute(g, age_scan);
    TEST_ASSERT_FALSE(RAY_IS_ERR(age_result));
    TEST_ASSERT_TRUE(age_result->len == 4);
    ray_release(age_result);

    ray_t* prio_result = ray_execute(g, prio_scan);
    TEST_ASSERT_FALSE(RAY_IS_ERR(prio_result));
    TEST_ASSERT_TRUE(prio_result->len == 3);
    ray_release(prio_result);

    ray_graph_free(g);
    ray_release(persons);
    ray_release(tasks);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_WCO_JOIN (chain pattern: 3 vars, 2 rels — general LFTJ)
 * -------------------------------------------------------------------------- */

static test_result_t test_wco_join_chain(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Graph: 0->1, 0->2, 1->2, 1->3, 2->3 (directed, no back edges) */
    int64_t src_data[] = {0, 0, 1, 1, 2};
    int64_t dst_data[] = {1, 2, 2, 3, 3};
    int64_t n = 5;

    ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, n);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, n);
    int64_t src_sym = ray_sym_intern("src", 3);
    int64_t dst_sym = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, src_sym, sv);
    edges = ray_table_add_col(edges, dst_sym, dv);
    ray_release(sv); ray_release(dv);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, true);
    TEST_ASSERT_NOT_NULL(rel);

    /* Chain pattern: a->b->c with n_vars=3, n_rels=2
     * rels[0]: a->b, rels[1]: b->c (fallback chain pattern in LFTJ) */
    ray_rel_t* rels[2] = {rel, rel};

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* wco = ray_wco_join(g, rels, 2, 3);
    TEST_ASSERT_NOT_NULL(wco);

    ray_t* result = ray_execute(g, wco);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* 2-hop paths: (0,1,2), (0,1,3), (0,2,3), (1,2,3) = 4 paths */
    int64_t nrows = ray_table_nrows(result);
    TEST_ASSERT_TRUE(nrows == 4);

    /* Verify we have 3 columns: _v0, _v1, _v2 */
    TEST_ASSERT_TRUE(ray_table_ncols(result) == 3);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: Factorized expand (degree counting)
 * -------------------------------------------------------------------------- */

static test_result_t test_expand_factorized(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    /* Expand nodes {0, 1, 2} forward — factorized should give degree counts */
    int64_t start_data[] = {0, 1, 2};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 3);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src, rel, 0);
    TEST_ASSERT_NOT_NULL(expand);

    /* Manually set factorized flag (normally done by optimizer) */
    ray_op_ext_t* ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == expand->id) {
            ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(ext);
    ext->graph.factorized = 1;

    ray_t* result = ray_execute(g, expand);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Should have 2 columns: _src and _count */
    TEST_ASSERT_TRUE(ray_table_ncols(result) == 2);

    int64_t cnt_sym = ray_sym_intern("_count", 6);
    ray_t* cnt_col = ray_table_get_col(result, cnt_sym);
    TEST_ASSERT_NOT_NULL(cnt_col);

    /* Degrees: node 0=2, node 1=2, node 2=1 */
    int64_t* counts = (int64_t*)ray_data(cnt_col);
    int64_t total_deg = 0;
    for (int64_t i = 0; i < cnt_col->len; i++)
        total_deg += counts[i];
    TEST_ASSERT_TRUE(total_deg == 5);  /* 2 + 2 + 1 = 5 */

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: SIP expand (source-side selection skip)
 * -------------------------------------------------------------------------- */

static test_result_t test_sip_expand(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    /* Create source-side selection: only allow node 0 (skip nodes 1, 2, 3) */
    ray_t* src_sel = ray_sel_new(4);
    TEST_ASSERT_NOT_NULL(src_sel);
    TEST_ASSERT_FALSE(RAY_IS_ERR(src_sel));
    uint64_t* sel_bits = ray_sel_bits(src_sel);
    RAY_SEL_BIT_SET(sel_bits, 0);  /* only node 0 passes */

    /* Expand from nodes {0, 1, 2} forward — but SIP should skip 1, 2 */
    int64_t start_data[] = {0, 1, 2};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 3);

    ray_graph_t* g = ray_graph_new(NULL);

    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src, rel, 0);

    /* Attach SIP selection to the expand ext node */
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == expand->id) {
            g->ext_nodes[i]->graph.sip_sel = src_sel;
            break;
        }
    }

    ray_t* result = ray_execute(g, expand);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Only node 0 should be expanded: degree 2 → 2 output rows */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(src_sel);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: S-Join semijoin filter in exec_join
 * -------------------------------------------------------------------------- */

static test_result_t test_sjoin_filter(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Left table: id column with many rows, most not in right side */
    int64_t n_left = 100;
    ray_t* left_ids = ray_vec_new(RAY_I64, n_left);
    TEST_ASSERT_NOT_NULL(left_ids);
    TEST_ASSERT_FALSE(RAY_IS_ERR(left_ids));
    left_ids->len = n_left;
    int64_t* lid = (int64_t*)ray_data(left_ids);
    for (int64_t i = 0; i < n_left; i++) lid[i] = i;

    ray_t* left_vals = ray_vec_new(RAY_I64, n_left);
    left_vals->len = n_left;
    int64_t* lv = (int64_t*)ray_data(left_vals);
    for (int64_t i = 0; i < n_left; i++) lv[i] = i * 10;

    int64_t id_sym = ray_sym_intern("id", 2);
    int64_t val_sym = ray_sym_intern("val", 3);
    ray_t* left_tbl = ray_table_new(2);
    left_tbl = ray_table_add_col(left_tbl, id_sym, left_ids);
    left_tbl = ray_table_add_col(left_tbl, val_sym, left_vals);
    ray_release(left_ids); ray_release(left_vals);

    /* Right table: small, only ids 5, 10, 15 */
    int64_t rids[] = {5, 10, 15};
    ray_t* right_ids = ray_vec_from_raw(RAY_I64, rids, 3);
    int64_t rvals[] = {500, 1000, 1500};
    ray_t* right_vals = ray_vec_from_raw(RAY_I64, rvals, 3);

    int64_t rval_sym = ray_sym_intern("rval", 4);
    ray_t* right_tbl = ray_table_new(2);
    right_tbl = ray_table_add_col(right_tbl, id_sym, right_ids);
    right_tbl = ray_table_add_col(right_tbl, rval_sym, right_vals);
    ray_release(right_ids); ray_release(right_vals);

    /* Inner join on id — should trigger S-Join (100 > 3*2) */
    ray_graph_t* g = ray_graph_new(left_tbl);
    uint16_t right_id = ray_graph_add_table(g, right_tbl);

    /* Build join: table-producing ops for left/right, key scans for join keys */
    ray_op_t* left_tbl_op  = ray_const_table(g, left_tbl);
    ray_op_t* right_tbl_op = ray_const_table(g, right_tbl);
    ray_op_t* left_scan    = ray_scan(g, "id");
    ray_op_t* right_scan   = ray_scan_table(g, right_id, "id");

    ray_op_t* left_keys[1]  = { left_scan };
    ray_op_t* right_keys[1] = { right_scan };

    ray_op_t* join = ray_join(g, left_tbl_op, left_keys, right_tbl_op, right_keys, 1, 0);
    TEST_ASSERT_NOT_NULL(join);

    ray_t* result = ray_execute(g, join);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Should match exactly 3 rows (ids 5, 10, 15) */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(left_tbl);
    ray_release(right_tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: SIP bitmap auto-construction from optimizer hint
 * -------------------------------------------------------------------------- */

static test_result_t test_sip_auto_build(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    /* Use >64 source nodes to trigger SIP auto-build */
    int64_t start_data[100];
    for (int i = 0; i < 100; i++) start_data[i] = i % 4;
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 100);

    /* --- Graph 1: baseline without SIP hint --- */
    ray_graph_t* g1 = ray_graph_new(NULL);
    ray_op_t* src1 = ray_const_vec(g1, start_vec);
    ray_op_t* expand1 = ray_expand(g1, src1, rel, 0);

    ray_t* baseline = ray_execute(g1, expand1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(baseline));
    TEST_ASSERT_EQ_I(baseline->type, RAY_TABLE);
    int64_t baseline_rows = ray_table_nrows(baseline);
    TEST_ASSERT_TRUE(baseline_rows > 0);
    ray_release(baseline);
    ray_graph_free(g1);

    /* --- Graph 2: with SIP hint set before execution --- */
    ray_graph_t* g2 = ray_graph_new(NULL);
    ray_op_t* src2 = ray_const_vec(g2, start_vec);
    ray_op_t* expand2 = ray_expand(g2, src2, rel, 0);

    /* Set SIP hint flag in pad[2] to trigger auto-build */
    ray_op_ext_t* ext = NULL;
    for (uint32_t i = 0; i < g2->ext_count; i++) {
        if (g2->ext_nodes[i] && g2->ext_nodes[i]->base.id == expand2->id) {
            ext = g2->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(ext);
    ext->base.pad[2] = 1;  /* SIP hint flag */

    ray_t* result = ray_execute(g2, expand2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* All 4 nodes have degree > 0 — SIP should pass all, same count */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == baseline_rows);

    /* Verify sip_sel was auto-built */
    TEST_ASSERT_NOT_NULL(ext->graph.sip_sel);

    ray_release(result);
    ray_graph_free(g2);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: Factorized EXPAND → GROUP pipeline (degree count shortcut)
 * -------------------------------------------------------------------------- */

static test_result_t test_factorized_group(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    /* Graph: 0→1, 0→2, 1→2, 1→3, 2→3, 3→0
     * Out-degrees: 0=2, 1=2, 2=1, 3=1 */
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    int64_t start_data[] = {0, 1, 2, 3};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 4);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src, rel, 0);

    /* Set factorized flag on expand */
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == expand->id) {
            g->ext_nodes[i]->graph.factorized = 1;
            break;
        }
    }

    /* Build GROUP BY _src with COUNT(*) on the expand output */
    ray_op_t* key_src = ray_scan(g, "_src");
    ray_op_t* agg_cnt = ray_scan(g, "_count");
    ray_op_t* keys[1] = { key_src };
    ray_op_t* agg_ins[1] = { agg_cnt };
    uint16_t agg_ops[1] = { OP_COUNT };
    ray_op_t* group = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(group);

    ray_t* result = ray_execute(g, group);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Factorized expand produces 4 rows (one per node with deg > 0).
     * GROUP BY _src COUNT(*) on factorized output should return 4 groups
     * with counts = degrees [2, 2, 1, 1]. */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 4);

    /* Verify total count across groups == total degree (6 edges) */
    int64_t agg_sym = ray_sym_intern("_agg", 4);
    ray_t* agg_col = ray_table_get_col(result, agg_sym);
    TEST_ASSERT_NOT_NULL(agg_col);
    int64_t* agg_data = (int64_t*)ray_data(agg_col);
    int64_t total = 0;
    for (int64_t i = 0; i < agg_col->len; i++) total += agg_data[i];
    TEST_ASSERT_TRUE(total == 6);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ASP-Join (factorized left → filtered right build)
 * -------------------------------------------------------------------------- */

static test_result_t test_asp_join(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Left table: small factorized output with _src keys {2, 5, 8} */
    int64_t lids[] = {2, 5, 8};
    int64_t lcnts[] = {10, 20, 30};
    ray_t* left_src = ray_vec_from_raw(RAY_I64, lids, 3);
    ray_t* left_cnt = ray_vec_from_raw(RAY_I64, lcnts, 3);

    int64_t id_sym = ray_sym_intern("id", 2);
    int64_t cnt_sym = ray_sym_intern("_count", 6);
    ray_t* left_tbl = ray_table_new(2);
    left_tbl = ray_table_add_col(left_tbl, id_sym, left_src);
    left_tbl = ray_table_add_col(left_tbl, cnt_sym, left_cnt);
    ray_release(left_src); ray_release(left_cnt);

    /* Right table: large, ids 0..99 — most don't match left */
    int64_t n_right = 100;
    ray_t* right_ids = ray_vec_new(RAY_I64, n_right);
    TEST_ASSERT_NOT_NULL(right_ids);
    TEST_ASSERT_FALSE(RAY_IS_ERR(right_ids));
    right_ids->len = n_right;
    int64_t* rid = (int64_t*)ray_data(right_ids);
    for (int64_t i = 0; i < n_right; i++) rid[i] = i;

    int64_t val_sym = ray_sym_intern("val", 3);
    ray_t* right_vals = ray_vec_new(RAY_I64, n_right);
    right_vals->len = n_right;
    int64_t* rv = (int64_t*)ray_data(right_vals);
    for (int64_t i = 0; i < n_right; i++) rv[i] = i * 100;

    ray_t* right_tbl = ray_table_new(2);
    right_tbl = ray_table_add_col(right_tbl, id_sym, right_ids);
    right_tbl = ray_table_add_col(right_tbl, val_sym, right_vals);
    ray_release(right_ids); ray_release(right_vals);

    /* Inner join: left (3 rows) × right (100 rows) on id
     * ASP-Join triggers when right_rows > left_rows * 2 and left has _count */
    ray_graph_t* g = ray_graph_new(left_tbl);
    uint16_t right_id = ray_graph_add_table(g, right_tbl);

    ray_op_t* left_tbl_op  = ray_const_table(g, left_tbl);
    ray_op_t* right_tbl_op = ray_const_table(g, right_tbl);
    ray_op_t* left_scan    = ray_scan(g, "id");
    ray_op_t* right_scan   = ray_scan_table(g, right_id, "id");

    ray_op_t* left_keys[1]  = { left_scan };
    ray_op_t* right_keys[1] = { right_scan };

    ray_op_t* join = ray_join(g, left_tbl_op, left_keys, right_tbl_op, right_keys, 1, 0);
    TEST_ASSERT_NOT_NULL(join);

    ray_t* result = ray_execute(g, join);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Should match exactly 3 rows (ids 2, 5, 8) */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(left_tbl);
    ray_release(right_tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_EXPAND direction==2 (both forward + reverse)
 * -------------------------------------------------------------------------- */

static test_result_t test_expand_both(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    /* Expand from node 1 in BOTH directions:
     * Fwd: 1->2, 1->3 (2 edges)
     * Rev: 0->1 (1 edge — nodes pointing TO 1)
     * Total: 3 */
    int64_t start_data[] = {1};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 1);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src, rel, 2);  /* direction=2: both */

    ray_t* result = ray_execute(g, expand);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_VAR_EXPAND reverse direction
 * -------------------------------------------------------------------------- */

static test_result_t test_var_expand_reverse(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    /* From node 3, reverse (follow incoming edges):
     * depth 1: 3<-1, 3<-2 => nodes 1, 2
     * depth 2: 1<-0, 2<-0 => node 0 (visited once)
     * Total reachable: 3 */
    int64_t start_data[] = {3};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 1);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* var_exp = ray_var_expand(g, src, rel, 1, 1, 3, false);

    ray_t* result = ray_execute(g, var_exp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_VAR_EXPAND direction==2 (both)
 * -------------------------------------------------------------------------- */

static test_result_t test_var_expand_both(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    /* From node 0, both directions, depth 1..1:
     * Fwd: 0->1, 0->2 (nodes 1, 2)
     * Rev: 3->0 (node 3)
     * Total reachable at depth 1: 3 */
    int64_t start_data[] = {0};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 1);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* var_exp = ray_var_expand(g, src, rel, 2, 1, 1, false);

    ray_t* result = ray_execute(g, var_exp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_SHORTEST_PATH reverse direction
 * -------------------------------------------------------------------------- */

static test_result_t test_shortest_path_reverse(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    /* Reverse shortest path from 0 to 3: follow incoming edges from 0.
     * Rev edges of 0: 3->0 (node 3). Path: 0 -> 3 (via reverse). Length 2 nodes. */
    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_i64(g, 0);
    ray_op_t* dst = ray_const_i64(g, 3);
    ray_op_t* sp = ray_shortest_path(g, src, dst, rel, 10);

    /* Set direction to reverse */
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == sp->id) {
            g->ext_nodes[i]->graph.direction = 1;
            break;
        }
    }

    ray_t* result = ray_execute(g, sp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* Reverse path 0->3: 0's rev neighbor is 3. Direct path of 2 nodes. */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 2);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: CSR save/load/mmap roundtrip
 * -------------------------------------------------------------------------- */

static test_result_t test_csr_save_load(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, true);
    TEST_ASSERT_NOT_NULL(rel);

    /* Save */
    const char* dir = "/tmp/test_csr_save";
    ray_err_t err = ray_rel_save(rel, dir);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load */
    ray_rel_t* loaded = ray_rel_load(dir);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_TRUE(loaded->fwd.n_nodes == 4);
    TEST_ASSERT_TRUE(loaded->fwd.n_edges == 6);
    TEST_ASSERT_TRUE(loaded->rev.n_nodes == 4);
    TEST_ASSERT_TRUE(loaded->rev.n_edges == 6);
    TEST_ASSERT_TRUE(loaded->fwd.sorted);

    /* Verify neighbor data matches */
    int64_t cnt_orig, cnt_loaded;
    int64_t* nbrs_orig = ray_csr_neighbors(&rel->fwd, 0, &cnt_orig);
    int64_t* nbrs_loaded = ray_csr_neighbors(&loaded->fwd, 0, &cnt_loaded);
    TEST_ASSERT_TRUE(cnt_orig == cnt_loaded);
    for (int64_t i = 0; i < cnt_orig; i++)
        TEST_ASSERT_TRUE(nbrs_orig[i] == nbrs_loaded[i]);

    ray_rel_free(loaded);

    /* Mmap */
    ray_rel_t* mmapped = ray_rel_mmap(dir);
    TEST_ASSERT_NOT_NULL(mmapped);
    TEST_ASSERT_TRUE(mmapped->fwd.n_nodes == 4);
    TEST_ASSERT_TRUE(mmapped->fwd.n_edges == 6);

    ray_rel_free(mmapped);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: CSR with out-of-range node IDs (should silently skip)
 * -------------------------------------------------------------------------- */

static test_result_t test_csr_out_of_range(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Edges: 0->1, 0->99 (99 is out of range for n_nodes=4) */
    int64_t src_data[] = {0, 0};
    int64_t dst_data[] = {1, 99};
    ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, 2);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 2);
    int64_t src_sym = ray_sym_intern("src", 3);
    int64_t dst_sym = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, src_sym, sv);
    edges = ray_table_add_col(edges, dst_sym, dv);
    ray_release(sv); ray_release(dv);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    /* Fwd CSR filters by src key: both src=0 are valid, so 2 fwd edges */
    TEST_ASSERT_TRUE(rel->fwd.n_edges == 2);
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->fwd, 0) == 2);

    /* Rev CSR filters by dst key: dst=1 is valid, dst=99 is out-of-range → 1 edge */
    TEST_ASSERT_TRUE(rel->rev.n_edges == 1);
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->rev, 1) == 1);

    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: CSR with empty edge table
 * -------------------------------------------------------------------------- */

static test_result_t test_csr_empty(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* sv = ray_vec_new(RAY_I64, 1);
    sv->len = 0;
    ray_t* dv = ray_vec_new(RAY_I64, 1);
    dv->len = 0;
    int64_t src_sym = ray_sym_intern("src", 3);
    int64_t dst_sym = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, src_sym, sv);
    edges = ray_table_add_col(edges, dst_sym, dv);
    ray_release(sv); ray_release(dv);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);
    TEST_ASSERT_TRUE(rel->fwd.n_edges == 0);
    TEST_ASSERT_TRUE(rel->rev.n_edges == 0);
    TEST_ASSERT_TRUE(rel->fwd.n_nodes == 4);

    /* All degrees should be 0 */
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_TRUE(ray_csr_degree(&rel->fwd, i) == 0);

    /* Expand from empty graph should return 0 rows */
    int64_t start_data[] = {0, 1};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 2);
    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src, rel, 0);
    ray_t* result = ray_execute(g, expand);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: CSR type validation (non-I64 columns rejected)
 * -------------------------------------------------------------------------- */

static test_result_t test_csr_type_validation(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build edge table with F64 columns (should be rejected) */
    double src_data[] = {0.0, 1.0};
    double dst_data[] = {1.0, 2.0};
    ray_t* sv = ray_vec_from_raw(RAY_F64, src_data, 2);
    ray_t* dv = ray_vec_from_raw(RAY_F64, dst_data, 2);
    int64_t src_sym = ray_sym_intern("src", 3);
    int64_t dst_sym = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, src_sym, sv);
    edges = ray_table_add_col(edges, dst_sym, dv);
    ray_release(sv); ray_release(dv);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 3, 3, false);
    TEST_ASSERT_EQ_PTR(rel, NULL);  /* Should fail due to type check */

    /* Also test negative n_nodes */
    ray_t* edges2 = make_edge_table();
    ray_rel_t* rel2 = ray_rel_from_edges(edges2, "src", "dst", -1, 4, false);
    TEST_ASSERT_EQ_PTR(rel2, NULL);

    ray_release(edges2);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: Graph with self-loop edges
 * -------------------------------------------------------------------------- */

static test_result_t test_self_loop(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Graph with self-loops: 0->0, 0->1, 1->1 */
    int64_t src_data[] = {0, 0, 1};
    int64_t dst_data[] = {0, 1, 1};
    ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, 3);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 3);
    int64_t src_sym = ray_sym_intern("src", 3);
    int64_t dst_sym = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, src_sym, sv);
    edges = ray_table_add_col(edges, dst_sym, dv);
    ray_release(sv); ray_release(dv);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 2, 2, false);
    TEST_ASSERT_NOT_NULL(rel);
    TEST_ASSERT_TRUE(rel->fwd.n_edges == 3);

    /* Expand from node 0: self-loop 0->0 + 0->1 = 2 results */
    int64_t start_data[] = {0};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 1);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src, rel, 0);
    ray_t* result = ray_execute(g, expand);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: Expand with empty source vector
 * -------------------------------------------------------------------------- */

static test_result_t test_expand_empty_src(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_t* start_vec = ray_vec_new(RAY_I64, 1);
    start_vec->len = 0;

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src, rel, 0);
    ray_t* result = ray_execute(g, expand);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_rel_build (FK-based CSR construction)
 * -------------------------------------------------------------------------- */

static test_result_t test_rel_build(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Table with FK column "ref" pointing to target nodes 0..2 */
    int64_t refs[] = {2, 0, 1, 2};
    ray_t* ref_vec = ray_vec_from_raw(RAY_I64, refs, 4);
    int64_t ref_sym = ray_sym_intern("ref", 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ref_sym, ref_vec);
    ray_release(ref_vec);

    ray_rel_t* rel = ray_rel_build(tbl, "ref", 3, true);
    TEST_ASSERT_NOT_NULL(rel);

    /* Fwd: src=row index (0..3), dst=ref value */
    TEST_ASSERT_TRUE(rel->fwd.n_nodes == 4);  /* 4 rows */
    TEST_ASSERT_TRUE(rel->fwd.n_edges == 4);
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->fwd, 0) == 1);  /* row 0 -> ref 2 */
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->fwd, 1) == 1);  /* row 1 -> ref 0 */

    /* Rev: dst=ref target (0..2), src=row index */
    TEST_ASSERT_TRUE(rel->rev.n_nodes == 3);  /* 3 target nodes */
    TEST_ASSERT_TRUE(rel->rev.n_edges == 4);
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->rev, 0) == 1);  /* target 0 <- row 1 */
    TEST_ASSERT_TRUE(ray_csr_degree(&rel->rev, 2) == 2);  /* target 2 <- rows 0,3 */

    TEST_ASSERT_TRUE(rel->fwd.sorted);

    /* Error cases */
    TEST_ASSERT_EQ_PTR(ray_rel_build(NULL, "ref", 3, false), NULL);
    TEST_ASSERT_EQ_PTR(ray_rel_build(tbl, "nonexistent", 3, false), NULL);
    TEST_ASSERT_EQ_PTR(ray_rel_build(tbl, "ref", -1, false), NULL);

    ray_rel_free(rel);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_SHORTEST_PATH direction==2 (bidirectional BFS)
 * -------------------------------------------------------------------------- */

static test_result_t test_shortest_path_both(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Graph where path 0->3 requires bidirectional traversal:
     * Fwd: 0->1  Rev: 3->2->1
     * Combined path: 0 -fwd-> 1 <-rev- 2 <-rev- 3
     * Actually: with both, from 0 we see fwd(0)={1} and rev(0)={3},
     * so the shortest bidirectional path 0->3 is just 0->3 (1 hop via rev). */
    ray_t* edges = make_edge_table();  /* 0->1,0->2,1->2,1->3,2->3,3->0 */
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_i64(g, 0);
    ray_op_t* dst = ray_const_i64(g, 3);
    ray_op_t* sp = ray_shortest_path(g, src, dst, rel, 10);

    /* Set direction to both */
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == sp->id) {
            g->ext_nodes[i]->graph.direction = 2;
            break;
        }
    }

    ray_t* result = ray_execute(g, sp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* With both fwd+rev from 0: fwd neighbors={1,2}, rev neighbors={3}.
     * BFS finds 3 at depth 1 via rev. Path: [0, 3] = 2 nodes. */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 2);

    int64_t node_sym = ray_sym_intern("_node", 5);
    ray_t* node_col = ray_table_get_col(result, node_sym);
    int64_t* nodes = (int64_t*)ray_data(node_col);
    TEST_ASSERT_TRUE(nodes[0] == 0);
    TEST_ASSERT_TRUE(nodes[1] == 3);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: var_expand with max_depth==0 (should return empty)
 * -------------------------------------------------------------------------- */

static test_result_t test_var_expand_depth0(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    int64_t start_data[] = {0};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 1);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* var_exp = ray_var_expand(g, src, rel, 0, 1, 0, false);

    ray_t* result = ray_execute(g, var_exp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: WCO join with unsorted rels (should return error)
 * -------------------------------------------------------------------------- */

static test_result_t test_wco_unsorted(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build with sorted=false */
    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_FALSE(rel->fwd.sorted);

    ray_rel_t* rels[3] = {rel, rel, rel};
    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* wco = ray_wco_join(g, rels, 3, 3);
    TEST_ASSERT_NOT_NULL(wco);

    ray_t* result = ray_execute(g, wco);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: Expand with out-of-range source node IDs at execution time
 * -------------------------------------------------------------------------- */

static test_result_t test_expand_oob_src(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    /* Source vector has node 99 (out of range) mixed with valid nodes */
    int64_t start_data[] = {0, 99, 1};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 3);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src, rel, 0);
    ray_t* result = ray_execute(g, expand);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    /* Node 0: 2 edges, node 99: skipped, node 1: 2 edges = 4 total */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 4);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: Triangle exact count verification
 * -------------------------------------------------------------------------- */

static test_result_t test_triangle_exact(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Complete bidirectional graph K3: 0<->1, 1<->2, 0<->2 */
    int64_t src_data[] = {0, 0, 1, 1, 2, 2};
    int64_t dst_data[] = {1, 2, 0, 2, 0, 1};
    ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, 6);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 6);
    int64_t src_sym = ray_sym_intern("src", 3);
    int64_t dst_sym = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, src_sym, sv);
    edges = ray_table_add_col(edges, dst_sym, dv);
    ray_release(sv); ray_release(dv);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 3, 3, true);
    ray_rel_t* rels[3] = {rel, rel, rel};

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* wco = ray_wco_join(g, rels, 3, 3);
    ray_t* result = ray_execute(g, wco);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* K3 triangle pattern with variable ordering 0<1<2:
     * The LFTJ enumerates a where fwd[a] intersects:
     *   var1 b: neighbors of a (via fwd)
     *   var2 c: fwd[b] intersect fwd[a] (c > b due to ordering)
     * For K3 bidirectional: each (a,b,c) with a<b<c in neighbor sets.
     * With all edges bidirectional, root iterates 0,1,2.
     * a=0: fwd[0]={1,2}, b candidates. b=1: fwd[1] intersect fwd[0] = {0,2} intersect {1,2} = {2}. c=2. Emit (0,1,2).
     * b=2: fwd[2] intersect fwd[0] = {0,1} intersect {1,2} = {1}. c=1. Emit (0,2,1).
     * a=1: fwd[1]={0,2}. b=0: fwd[0] intersect fwd[1] = {1,2} intersect {0,2} = {2}. Emit (1,0,2).
     * b=2: fwd[2] intersect fwd[1] = {0,1} intersect {0,2} = {0}. Emit (1,2,0).
     * a=2: fwd[2]={0,1}. b=0: fwd[0] intersect fwd[2] = {1,2} intersect {0,1} = {1}. Emit (2,0,1).
     * b=1: fwd[1] intersect fwd[2] = {0,2} intersect {0,1} = {0}. Emit (2,1,0).
     * Total: 6 directed triangles */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 6);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: LFTJ chain with 4 variables (a→b→c→d)
 * -------------------------------------------------------------------------- */

static test_result_t test_wco_chain4(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Graph: 0->1, 0->2, 1->2, 1->3, 2->3 */
    int64_t src_data[] = {0, 0, 1, 1, 2};
    int64_t dst_data[] = {1, 2, 2, 3, 3};
    ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, 5);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 5);
    int64_t src_sym = ray_sym_intern("src", 3);
    int64_t dst_sym = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, src_sym, sv);
    edges = ray_table_add_col(edges, dst_sym, dv);
    ray_release(sv); ray_release(dv);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, true);

    /* Chain pattern: a→b→c→d with n_vars=4, n_rels=3 */
    ray_rel_t* rels[3] = {rel, rel, rel};
    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* wco = ray_wco_join(g, rels, 3, 4);
    TEST_ASSERT_NOT_NULL(wco);

    ray_t* result = ray_execute(g, wco);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* 3-hop paths: (0,1,2,3), (0,1,3,?), (0,2,3,?)
     * 0→1→2→3: valid. 0→1→3→?: no edges from 3. 0→2→3→?: no edges from 3.
     * 1→2→3→?: no edges from 3.
     * Only valid 3-hop: (0,1,2,3) = 1 path */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 1);

    /* Verify columns */
    TEST_ASSERT_TRUE(ray_table_ncols(result) == 4);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: shortest_path src == dst (zero-hop)
 * -------------------------------------------------------------------------- */

static test_result_t test_shortest_path_self(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src_op = ray_const_i64(g, 2);
    ray_op_t* dst_op = ray_const_i64(g, 2);
    ray_op_t* sp = ray_shortest_path(g, src_op, dst_op, rel, 10);
    TEST_ASSERT_NOT_NULL(sp);

    ray_t* result = ray_execute(g, sp);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* src == dst: should return single row with node=2, depth=0 */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 1);
    ray_t* node_col = ray_table_get_col(result, ray_sym_intern("_node", 5));
    ray_t* depth_col = ray_table_get_col(result, ray_sym_intern("_depth", 6));
    TEST_ASSERT_NOT_NULL(node_col);
    TEST_ASSERT_NOT_NULL(depth_col);
    TEST_ASSERT_TRUE(((int64_t*)ray_data(node_col))[0] == 2);
    TEST_ASSERT_TRUE(((int64_t*)ray_data(depth_col))[0] == 0);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: save/load verifies reverse CSR data
 * -------------------------------------------------------------------------- */

static test_result_t test_save_load_rev(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, true);
    TEST_ASSERT_NOT_NULL(rel);

    const char* dir = "/tmp/test_csr_rev";
    ray_err_t err = ray_rel_save(rel, dir);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_rel_t* loaded = ray_rel_load(dir);
    TEST_ASSERT_NOT_NULL(loaded);

    /* Verify reverse CSR matches original */
    TEST_ASSERT_TRUE(loaded->rev.n_nodes == rel->rev.n_nodes);
    TEST_ASSERT_TRUE(loaded->rev.n_edges == rel->rev.n_edges);
    for (int64_t i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(ray_csr_degree(&loaded->rev, i) == ray_csr_degree(&rel->rev, i));
    }
    /* Verify sorted flags persisted */
    TEST_ASSERT_TRUE(loaded->fwd.sorted == true);
    TEST_ASSERT_TRUE(loaded->rev.sorted == true);

    ray_rel_free(loaded);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: 4-clique WCO join (LFTJ with 4 vars, 6 rels)
 * -------------------------------------------------------------------------- */

static test_result_t test_wco_4clique(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Complete graph K5: 5 nodes, all pairs connected (10 directed edges).
     * 4-cliques in K5: C(5,4) = 5 */
    int64_t src_d[20], dst_d[20];
    int k = 0;
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++) {
            src_d[k] = i; dst_d[k] = j; k++;
            src_d[k] = j; dst_d[k] = i; k++;
        }
    ray_t* sv = ray_vec_from_raw(RAY_I64, src_d, k);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_d, k);
    int64_t ssym = ray_sym_intern("src", 3);
    int64_t dsym = ray_sym_intern("dst", 3);
    ray_t* etbl = ray_table_new(2);
    etbl = ray_table_add_col(etbl, ssym, sv);
    etbl = ray_table_add_col(etbl, dsym, dv);
    ray_release(sv); ray_release(dv);

    ray_rel_t* rel = ray_rel_from_edges(etbl, "src", "dst", 5, 5, true);
    TEST_ASSERT_NOT_NULL(rel);

    /* 4-clique: 6 rels = all pairs among 4 variables */
    ray_rel_t* rels[6] = {rel, rel, rel, rel, rel, rel};
    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* wco = ray_wco_join(g, rels, 6, 4);
    TEST_ASSERT_NOT_NULL(wco);

    ray_t* result = ray_execute(g, wco);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* K5 bidirectional: each node's fwd neighbors are all 4 others.
     * LFTJ enumerates all ordered 4-tuples of distinct nodes:
     * 5 * 4 * 3 * 2 = 120 permutations (= 5 cliques * 4! orderings). */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 120);
    TEST_ASSERT_TRUE(ray_table_ncols(result) == 4);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(etbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: fvec module — ftable create/materialize
 * -------------------------------------------------------------------------- */

static test_result_t test_fvec_materialize(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Create ftable with 2 columns: one flat, one unflat */
    ray_ftable_t* ft = ray_ftable_new(2);
    TEST_ASSERT_NOT_NULL(ft);
    TEST_ASSERT_EQ_I(ft->n_cols, 2);

    /* Column 0: flat — single value replicated 5 times */
    int64_t vals0[] = {42, 99, 7};
    ray_t* v0 = ray_vec_from_raw(RAY_I64, vals0, 3);
    ft->columns[0].vec = v0;
    ft->columns[0].cur_idx = 1;           /* value at index 1 = 99 */
    ft->columns[0].cardinality = 5;

    /* Column 1: unflat — full vector of 5 elements */
    int64_t vals1[] = {10, 20, 30, 40, 50};
    ray_t* v1 = ray_vec_from_raw(RAY_I64, vals1, 5);
    ft->columns[1].vec = v1;
    ft->columns[1].cur_idx = -1;          /* unflat */
    ft->columns[1].cardinality = 5;

    /* Materialize */
    ray_t* result = ray_ftable_materialize(ft);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 5);
    TEST_ASSERT_TRUE(ray_table_ncols(result) == 2);

    /* Verify flat column: all 5 values should be 99 */
    ray_t* c0 = ray_table_get_col(result, ray_sym_intern("_c0", 3));
    TEST_ASSERT_NOT_NULL(c0);
    int64_t* d0 = (int64_t*)ray_data(c0);
    for (int64_t i = 0; i < 5; i++)
        TEST_ASSERT_TRUE(d0[i] == 99);

    /* Verify unflat column: exact copy */
    ray_t* c1 = ray_table_get_col(result, ray_sym_intern("_c1", 3));
    TEST_ASSERT_NOT_NULL(c1);
    int64_t* d1 = (int64_t*)ray_data(c1);
    for (int64_t i = 0; i < 5; i++)
        TEST_ASSERT_TRUE(d1[i] == vals1[i]);

    ray_release(result);
    ray_ftable_free(ft);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: fvec — empty ftable materialization
 * -------------------------------------------------------------------------- */

static test_result_t test_fvec_empty(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 0 columns → error */
    ray_t* err = ray_ftable_materialize(NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));

    /* ftable with columns but no vec set → produces table with 0 real cols */
    ray_ftable_t* ft = ray_ftable_new(1);
    TEST_ASSERT_NOT_NULL(ft);
    ft->columns[0].vec = NULL;
    ray_t* result = ray_ftable_materialize(ft);
    /* With no actual columns added, result should still be a valid table */
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    ray_release(result);
    ray_ftable_free(ft);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: fvec — ftable free with semijoin
 * -------------------------------------------------------------------------- */

static test_result_t test_fvec_semijoin(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_ftable_t* ft = ray_ftable_new(1);
    TEST_ASSERT_NOT_NULL(ft);

    /* Set a semijoin bitmap */
    ray_t* sel = ray_sel_new(64);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sel));
    ft->semijoin = sel;

    /* Set a column */
    int64_t vals[] = {1, 2, 3};
    ft->columns[0].vec = ray_vec_from_raw(RAY_I64, vals, 3);
    ft->columns[0].cur_idx = -1;
    ft->columns[0].cardinality = 3;

    ray_t* result = ray_ftable_materialize(ft);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 3);
    ray_release(result);

    /* Free should clean up semijoin bitmap too */
    ray_ftable_free(ft);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: factorized expand with direction=1 (reverse)
 * -------------------------------------------------------------------------- */

static test_result_t test_factorized_reverse(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    /* Expand nodes {0, 1, 2} reverse — degree counts from rev CSR */
    int64_t start_data[] = {0, 1, 2};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 3);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_op_t* expand = ray_expand(g, src, rel, 1);  /* direction=1 rev */
    TEST_ASSERT_NOT_NULL(expand);

    /* Set factorized flag */
    ray_op_ext_t* ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == expand->id) {
            ext = g->ext_nodes[i]; break;
        }
    }
    TEST_ASSERT_NOT_NULL(ext);
    ext->graph.factorized = 1;

    ray_t* result = ray_execute(g, expand);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    /* Reverse degrees: node 0 has 1 in-edge (3->0), node 1 has 1 (0->1),
     * node 2 has 2 in-edges (0->2, 1->2) */
    TEST_ASSERT_TRUE(ray_table_ncols(result) == 2);
    int64_t cnt_sym = ray_sym_intern("_count", 6);
    ray_t* cnt_col = ray_table_get_col(result, cnt_sym);
    TEST_ASSERT_NOT_NULL(cnt_col);
    int64_t* counts = (int64_t*)ray_data(cnt_col);
    int64_t nrows = ray_table_nrows(result);
    /* All 3 nodes should have degree > 0, so all 3 appear */
    TEST_ASSERT_TRUE(nrows == 3);
    /* Sum of degrees: 1 + 1 + 2 = 4 */
    int64_t sum = 0;
    for (int64_t i = 0; i < nrows; i++) sum += counts[i];
    TEST_ASSERT_TRUE(sum == 4);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: LFTJ with disconnected/empty result
 * -------------------------------------------------------------------------- */

static test_result_t test_wco_empty_result(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Graph with no triangles: 0->1, 2->3 (two disconnected edges) */
    int64_t src_d[] = {0, 2};
    int64_t dst_d[] = {1, 3};
    ray_t* sv = ray_vec_from_raw(RAY_I64, src_d, 2);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_d, 2);
    int64_t ssym = ray_sym_intern("src", 3);
    int64_t dsym = ray_sym_intern("dst", 3);
    ray_t* etbl = ray_table_new(2);
    etbl = ray_table_add_col(etbl, ssym, sv);
    etbl = ray_table_add_col(etbl, dsym, dv);
    ray_release(sv); ray_release(dv);

    ray_rel_t* rel = ray_rel_from_edges(etbl, "src", "dst", 4, 4, true);
    TEST_ASSERT_NOT_NULL(rel);

    /* Triangle pattern (n_vars=3, n_rels=3) — no triangles exist */
    ray_rel_t* rels[3] = {rel, rel, rel};
    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* wco = ray_wco_join(g, rels, 3, 3);
    TEST_ASSERT_NOT_NULL(wco);

    ray_t* result = ray_execute(g, wco);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 0);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(etbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: var_expand with min_depth > max_depth (should return empty)
 * -------------------------------------------------------------------------- */

static test_result_t test_var_expand_bad_range(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    int64_t start_data[] = {0};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 1);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src = ray_const_vec(g, start_vec);
    /* min_depth=5, max_depth=2 → no results possible */
    ray_op_t* ve = ray_var_expand(g, src, rel, 0, 5, 2, false);
    TEST_ASSERT_NOT_NULL(ve);

    ray_t* result = ray_execute(g, ve);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    /* min > max means no depth qualifies → 0 rows */
    TEST_ASSERT_TRUE(ray_table_nrows(result) == 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * test_degree_cent: in/out/total degree from CSR offsets
 * -------------------------------------------------------------------------- */
static test_result_t test_degree_cent(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_t* tbl = ray_table_new(1);
    ray_t* dummy_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("_dummy", 6), dummy_vec);
    ray_release(dummy_vec);
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* dc = ray_degree_cent(g, rel);
    TEST_ASSERT_NOT_NULL(dc);

    ray_t* result = ray_execute(g, dc);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    /* Graph: 0→1, 0→2, 1→2, 1→3, 2→3, 3→0
     * Node 0: out=2, in=1, total=3
     * Node 1: out=2, in=1, total=3
     * Node 2: out=1, in=2, total=3
     * Node 3: out=1, in=2, total=3 */
    int64_t out_sym = ray_sym_intern("_out_degree", 11);
    int64_t in_sym  = ray_sym_intern("_in_degree", 10);
    int64_t deg_sym = ray_sym_intern("_degree", 7);

    ray_t* out_col = ray_table_get_col(result, out_sym);
    ray_t* in_col  = ray_table_get_col(result, in_sym);
    ray_t* deg_col = ray_table_get_col(result, deg_sym);

    TEST_ASSERT_NOT_NULL(out_col);
    TEST_ASSERT_NOT_NULL(in_col);
    TEST_ASSERT_NOT_NULL(deg_col);

    int64_t* out_d = (int64_t*)ray_data(out_col);
    int64_t* in_d  = (int64_t*)ray_data(in_col);
    int64_t* deg_d = (int64_t*)ray_data(deg_col);

    /* Node 0: out=2, in=1, total=3 */
    TEST_ASSERT_EQ_I(out_d[0], 2);
    TEST_ASSERT_EQ_I(in_d[0], 1);
    TEST_ASSERT_EQ_I(deg_d[0], 3);
    /* Node 1: out=2, in=1, total=3 */
    TEST_ASSERT_EQ_I(out_d[1], 2);
    TEST_ASSERT_EQ_I(in_d[1], 1);
    TEST_ASSERT_EQ_I(deg_d[1], 3);
    /* Node 2: out=1, in=2, total=3 */
    TEST_ASSERT_EQ_I(out_d[2], 1);
    TEST_ASSERT_EQ_I(in_d[2], 2);
    TEST_ASSERT_EQ_I(deg_d[2], 3);
    /* Node 3: out=1, in=2, total=3 */
    TEST_ASSERT_EQ_I(out_d[3], 1);
    TEST_ASSERT_EQ_I(in_d[3], 2);
    TEST_ASSERT_EQ_I(deg_d[3], 3);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* DAG: 0→1, 0→2, 1→3, 2→3 (4 nodes, 4 edges, no cycles) */
static ray_t* make_dag_edge_table(void) {
    int64_t src_data[] = {0, 0, 1, 2};
    int64_t dst_data[] = {1, 2, 3, 3};
    int64_t n = 4;

    ray_t* src_vec = ray_vec_from_raw(RAY_I64, src_data, n);
    ray_t* dst_vec = ray_vec_from_raw(RAY_I64, dst_data, n);

    int64_t src_sym = ray_sym_intern("src", 3);
    int64_t dst_sym = ray_sym_intern("dst", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, src_sym, src_vec);
    ray_release(src_vec);
    tbl = ray_table_add_col(tbl, dst_sym, dst_vec);
    ray_release(dst_vec);
    return tbl;
}

static test_result_t test_topsort(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_dag_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_t* tbl = ray_table_new(1);
    ray_t* dummy_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("_dummy", 6), dummy_vec);
    ray_release(dummy_vec);
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* ts = ray_topsort(g, rel);
    TEST_ASSERT_NOT_NULL(ts);

    ray_t* result = ray_execute(g, ts);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    /* DAG: 0→1, 0→2, 1→3, 2→3
     * Valid orderings: 0 must come before 1,2; 1,2 before 3 */
    int64_t order_sym = ray_sym_intern("_order", 6);
    ray_t* order_col = ray_table_get_col(result, order_sym);
    TEST_ASSERT_NOT_NULL(order_col);
    int64_t* ord = (int64_t*)ray_data(order_col);

    /* Order values must be a valid permutation of [0..3] */
    uint8_t seen[4] = {0};
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(ord[i] >= 0 && ord[i] < 4);
        TEST_ASSERT_FALSE(seen[ord[i]]);
        seen[ord[i]] = 1;
    }
    /* Node 0 must come before 1,2; node 3 must come after 1,2 */
    TEST_ASSERT_TRUE(ord[0] < ord[1]);
    TEST_ASSERT_TRUE(ord[0] < ord[2]);
    TEST_ASSERT_TRUE(ord[3] > ord[1]);
    TEST_ASSERT_TRUE(ord[3] > ord[2]);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_topsort_cycle(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Cyclic graph: 0→1, 0→2, 1→2, 1→3, 2→3, 3→0 */
    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_t* tbl = ray_table_new(1);
    ray_t* dummy_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("_dummy", 6), dummy_vec);
    ray_release(dummy_vec);
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* ts = ray_topsort(g, rel);
    ray_t* result = ray_execute(g, ts);

    /* Cycle detected — should return error */
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_dfs(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    /* Build a table with source node = 0 */
    ray_t* src_tbl = ray_table_new(1);
    ray_t* src_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    src_tbl = ray_table_add_col(src_tbl, ray_sym_intern("src", 3), src_vec);
    ray_release(src_vec);

    ray_graph_t* g = ray_graph_new(src_tbl);
    ray_op_t* src_op = ray_scan(g, "src");
    ray_op_t* dfs = ray_dfs(g, src_op, rel, 255);
    TEST_ASSERT_NOT_NULL(dfs);

    ray_t* result = ray_execute(g, dfs);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    /* All 4 nodes should be reachable from node 0 */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    /* Node 0 should have depth 0 and parent -1 */
    int64_t node_sym   = ray_sym_intern("_node", 5);
    int64_t depth_sym  = ray_sym_intern("_depth", 6);
    int64_t parent_sym = ray_sym_intern("_parent", 7);

    ray_t* node_col   = ray_table_get_col(result, node_sym);
    ray_t* depth_col  = ray_table_get_col(result, depth_sym);
    ray_t* parent_col = ray_table_get_col(result, parent_sym);
    TEST_ASSERT_NOT_NULL(node_col);
    TEST_ASSERT_NOT_NULL(depth_col);
    TEST_ASSERT_NOT_NULL(parent_col);

    int64_t* nodes   = (int64_t*)ray_data(node_col);
    int64_t* depths  = (int64_t*)ray_data(depth_col);
    int64_t* parents = (int64_t*)ray_data(parent_col);

    /* First node in DFS order should be source (node 0) */
    TEST_ASSERT_EQ_I(nodes[0], 0);
    TEST_ASSERT_EQ_I(depths[0], 0);
    TEST_ASSERT_EQ_I(parents[0], -1);

    /* All 4 nodes must be distinct and valid */
    uint8_t node_seen[4] = {0};
    for (int64_t i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(nodes[i] >= 0 && nodes[i] < 4);
        TEST_ASSERT_FALSE(node_seen[nodes[i]]);
        node_seen[nodes[i]] = 1;
        TEST_ASSERT((depths[i]) >= (0), "depths[i] >= 0");
        /* Non-root nodes must have a valid parent */
        if (i > 0) {
            TEST_ASSERT_TRUE(parents[i] >= 0 && parents[i] < 4);
        }
    }

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(src_tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_dfs_max_depth(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_dag_edge_table();  /* 0→1, 0→2, 1→3, 2→3 */
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_t* src_tbl = ray_table_new(1);
    ray_t* src_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    src_tbl = ray_table_add_col(src_tbl, ray_sym_intern("src", 3), src_vec);
    ray_release(src_vec);

    ray_graph_t* g = ray_graph_new(src_tbl);
    ray_op_t* src_op = ray_scan(g, "src");
    ray_op_t* dfs = ray_dfs(g, src_op, rel, 1);  /* max depth = 1 */

    ray_t* result = ray_execute(g, dfs);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    /* With max_depth=1 from node 0: nodes 0, 1, 2 (not 3) */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    /* Verify correct nodes and depths */
    int64_t node_sym   = ray_sym_intern("_node", 5);
    int64_t depth_sym  = ray_sym_intern("_depth", 6);
    ray_t* node_col  = ray_table_get_col(result, node_sym);
    ray_t* depth_col = ray_table_get_col(result, depth_sym);
    int64_t* ns = (int64_t*)ray_data(node_col);
    int64_t* ds = (int64_t*)ray_data(depth_col);
    uint8_t found[4] = {0};
    for (int64_t i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(ds[i] <= 1);
        TEST_ASSERT_TRUE(ns[i] >= 0 && ns[i] < 4);
        found[ns[i]] = 1;
    }
    /* Node 3 should not be reached at depth 1 */
    TEST_ASSERT_TRUE(found[0] && found[1] && found[2]);
    TEST_ASSERT_FALSE(found[3]);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(src_tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * test_cluster_coeff: clustering coefficient via triangle counting
 * -------------------------------------------------------------------------- */
static test_result_t test_cluster_coeff(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Graph: 0→1, 0→2, 1→2, 1→3, 2→3, 3→0
     * Treat as undirected: node 0 neighbors={1,2,3}, node 1 neighbors={0,2,3}, etc.
     * All 4 nodes form a near-clique. */
    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_t* tbl = ray_table_new(1);
    ray_t* dummy_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("_dummy", 6), dummy_vec);
    ray_release(dummy_vec);
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* cc = ray_cluster_coeff(g, rel);
    TEST_ASSERT_NOT_NULL(cc);

    ray_t* result = ray_execute(g, cc);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    /* Verify exact clustering coefficients.
     * Graph edges: 0->1, 0->2, 1->2, 1->3, 2->3, 3->0 (directed).
     * Undirected neighbors: 0={1,2,3}, 1={0,2,3}, 2={0,1,3}, 3={0,1,2}.
     * Formula: directed_fwd_edges_between_neighbors / (deg * (deg-1)).
     * Node 0 (deg=3, pairs=6): fwd edges among {1,2,3}: 1->2,1->3,2->3 = 3; coeff=3/6=0.5
     * Node 1 (deg=3, pairs=6): fwd edges among {0,2,3}: 0->2,2->3,3->0 = 3; coeff=3/6=0.5
     * Node 2 (deg=3, pairs=6): fwd edges among {0,1,3}: 0->1,1->3,3->0 = 3; coeff=3/6=0.5
     * Node 3 (deg=3, pairs=6): fwd edges among {0,1,2}: 0->1,0->2,1->2 = 3; coeff=3/6=0.5 */
    int64_t coeff_sym = ray_sym_intern("_coefficient", 12);
    ray_t* coeff_col = ray_table_get_col(result, coeff_sym);
    TEST_ASSERT_NOT_NULL(coeff_col);
    double* coeffs = (double*)ray_data(coeff_col);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT((coeffs[i]) >= (0.49), "double >= failed");
        TEST_ASSERT((coeffs[i]) <= (0.51), "double <= failed");
    }

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: Random walk
 * -------------------------------------------------------------------------- */

static test_result_t test_random_walk(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_t* src_tbl = ray_table_new(1);
    ray_t* src_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    src_tbl = ray_table_add_col(src_tbl, ray_sym_intern("src", 3), src_vec);
    ray_release(src_vec);

    ray_graph_t* g = ray_graph_new(src_tbl);
    ray_op_t* src_op = ray_scan(g, "src");
    ray_op_t* rw = ray_random_walk(g, src_op, rel, 10);
    TEST_ASSERT_NOT_NULL(rw);

    ray_t* result = ray_execute(g, rw);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    /* Should have 11 rows (start + 10 steps) */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 11);

    /* First node should be source (0) */
    int64_t node_sym = ray_sym_intern("_node", 5);
    ray_t* node_col = ray_table_get_col(result, node_sym);
    int64_t* nodes = (int64_t*)ray_data(node_col);
    TEST_ASSERT_EQ_I(nodes[0], 0);

    /* All nodes should be valid (0..3) and consecutive pairs must be edges.
     * Graph edges: 0->1, 0->2, 1->2, 1->3, 2->3, 3->0 */
    int edges_src[] = {0, 0, 1, 1, 2, 3};
    int edges_dst[] = {1, 2, 2, 3, 3, 0};
    int n_edges = 6;
    for (int i = 0; i < 11; i++) {
        TEST_ASSERT_TRUE(nodes[i] >= 0);
        TEST_ASSERT_TRUE(nodes[i] < 4);
        if (i > 0) {
            bool valid_edge = false;
            for (int e = 0; e < n_edges; e++) {
                if (edges_src[e] == nodes[i-1] && edges_dst[e] == nodes[i]) {
                    valid_edge = true;
                    break;
                }
            }
            TEST_ASSERT_TRUE(valid_edge);
        }
    }

    /* Step column should be 0..10 */
    int64_t step_sym = ray_sym_intern("_step", 5);
    ray_t* step_col = ray_table_get_col(result, step_sym);
    int64_t* steps = (int64_t*)ray_data(step_col);
    for (int i = 0; i < 11; i++) {
        TEST_ASSERT_EQ_I(steps[i], i);
    }

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(src_tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: A* shortest path
 * -------------------------------------------------------------------------- */

/* Weighted graph with lat/lon node properties:
 * 5 nodes, 6 edges:
 *   0->1 (w=1.0), 0->2 (w=4.0), 1->3 (w=2.0), 2->3 (w=1.0), 3->4 (w=3.0), 1->4 (w=10.0)
 * Node coordinates: 0=(0,0), 1=(1,0), 2=(0,2), 3=(2,1), 4=(3,0) */
static void make_astar_graph(ray_t** out_edges, ray_rel_t** out_rel,
                             ray_t** out_node_props) {
    int64_t src[] = {0, 0, 1, 2, 3, 1};
    int64_t dst[] = {1, 2, 3, 3, 4, 4};
    double  wts[] = {1.0, 4.0, 2.0, 1.0, 3.0, 10.0};
    int64_t ne = 6;

    ray_t* sv = ray_vec_from_raw(RAY_I64, src, ne);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst, ne);
    ray_t* wv = ray_vec_new(RAY_F64, ne);
    memcpy(ray_data(wv), wts, sizeof(wts));
    wv->len = ne;

    ray_t* edges = ray_table_new(3);
    edges = ray_table_add_col(edges, ray_sym_intern("src", 3), sv); ray_release(sv);
    edges = ray_table_add_col(edges, ray_sym_intern("dst", 3), dv); ray_release(dv);
    edges = ray_table_add_col(edges, ray_sym_intern("weight", 6), wv); ray_release(wv);

    *out_rel = ray_rel_from_edges(edges, "src", "dst", 5, 5, false);
    ray_rel_set_props(*out_rel, edges);
    *out_edges = edges;

    /* Node property table with lat/lon */
    double lat_arr[] = {0.0, 1.0, 0.0, 2.0, 3.0};
    double lon_arr[] = {0.0, 0.0, 2.0, 1.0, 0.0};
    ray_t* nv = ray_vec_from_raw(RAY_I64, (int64_t[]){0,1,2,3,4}, 5);
    ray_t* latv = ray_vec_new(RAY_F64, 5);
    memcpy(ray_data(latv), lat_arr, sizeof(lat_arr));
    latv->len = 5;
    ray_t* lonv = ray_vec_new(RAY_F64, 5);
    memcpy(ray_data(lonv), lon_arr, sizeof(lon_arr));
    lonv->len = 5;

    ray_t* np = ray_table_new(3);
    np = ray_table_add_col(np, ray_sym_intern("_node", 5), nv); ray_release(nv);
    np = ray_table_add_col(np, ray_sym_intern("lat", 3), latv); ray_release(latv);
    np = ray_table_add_col(np, ray_sym_intern("lon", 3), lonv); ray_release(lonv);
    *out_node_props = np;
}

static test_result_t test_astar(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges; ray_rel_t* rel; ray_t* node_props;
    make_astar_graph(&edges, &rel, &node_props);

    ray_graph_t* g = ray_graph_new(edges);
    ray_op_t* src = ray_const_i64(g, 0);
    ray_op_t* dst = ray_const_i64(g, 4);
    ray_op_t* as = ray_astar(g, src, dst, rel, "weight", "lat", "lon", node_props, 255);
    TEST_ASSERT_NOT_NULL(as);

    ray_t* result = ray_execute(g, as);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    /* Should find path to node 4 with dist=6.0 (0->1->3->4: 1+2+3) */
    int64_t nrows = ray_table_nrows(result);
    TEST_ASSERT((nrows) > (0), "nrows > 0");

    int64_t node_sym = ray_sym_intern("_node", 5);
    int64_t dist_sym = ray_sym_intern("_dist", 5);
    ray_t* node_col = ray_table_get_col(result, node_sym);
    ray_t* dist_col = ray_table_get_col(result, dist_sym);
    int64_t* nodes = (int64_t*)ray_data(node_col);
    double* dists = (double*)ray_data(dist_col);

    bool found = false;
    for (int64_t i = 0; i < nrows; i++) {
        if (nodes[i] == 4) {
            TEST_ASSERT((dists[i]) >= (5.99), "double >= failed");
            TEST_ASSERT((dists[i]) <= (6.01), "double <= failed");
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(node_props);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_k_shortest(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges; ray_rel_t* rel; ray_t* node_props;
    make_astar_graph(&edges, &rel, &node_props);

    ray_graph_t* g = ray_graph_new(edges);
    ray_op_t* src = ray_const_i64(g, 0);
    ray_op_t* dst = ray_const_i64(g, 4);
    ray_op_t* ks = ray_k_shortest(g, src, dst, rel, "weight", 3);
    TEST_ASSERT_NOT_NULL(ks);

    ray_t* result = ray_execute(g, ks);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    /* Should find at least 2 paths: 0->1->3->4 (6.0) and 0->2->3->4 (8.0) */
    int64_t nrows = ray_table_nrows(result);
    TEST_ASSERT((nrows) >= (2), "nrows >= 2");

    /* Check path_id column exists */
    int64_t pid_sym = ray_sym_intern("_path_id", 8);
    ray_t* pid_col = ray_table_get_col(result, pid_sym);
    TEST_ASSERT_NOT_NULL(pid_col);

    /* First path should have lowest total distance */
    int64_t dist_sym = ray_sym_intern("_dist", 5);
    ray_t* dist_col = ray_table_get_col(result, dist_sym);
    double* dists = (double*)ray_data(dist_col);
    /* First row of path 0 should be source with dist 0 */
    TEST_ASSERT((dists[0]) >= (-0.01), "double >= failed");
    TEST_ASSERT((dists[0]) <= (0.01), "double <= failed");

    /* Verify path_id 0 exists and ends at dst with dist ~6.0 */
    int64_t node_sym = ray_sym_intern("_node", 5);
    ray_t* node_col = ray_table_get_col(result, node_sym);
    int64_t* nodes_arr = (int64_t*)ray_data(node_col);
    int64_t* pids = (int64_t*)ray_data(pid_col);

    /* Find last row of path 0 */
    int64_t last_p0 = 0;
    for (int64_t i = 0; i < nrows; i++) {
        if (pids[i] == 0) last_p0 = i;
    }
    TEST_ASSERT_EQ_I(nodes_arr[last_p0], 4);
    TEST_ASSERT((dists[last_p0]) >= (5.99), "double >= failed");
    TEST_ASSERT((dists[last_p0]) <= (6.01), "double <= failed");

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(node_props);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: betweenness centrality (Brandes, exact)
 * -------------------------------------------------------------------------- */

static test_result_t test_betweenness(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* DAG: 0->1, 0->2, 1->3, 2->3
     * Nodes 1 and 2 are bridges -- should have nonzero betweenness. */
    ray_t* edges = make_dag_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_t* tbl = ray_table_new(1);
    ray_t* dummy_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("_dummy", 6), dummy_vec);
    ray_release(dummy_vec);
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* bc = ray_betweenness(g, rel, 0);  /* exact */
    TEST_ASSERT_NOT_NULL(bc);

    ray_t* result = ray_execute(g, bc);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    int64_t cent_sym = ray_sym_intern("_centrality", 11);
    ray_t* cent_col = ray_table_get_col(result, cent_sym);
    TEST_ASSERT_NOT_NULL(cent_col);
    double* cents = (double*)ray_data(cent_col);

    /* Undirected K_{2,2} (0-1, 0-2, 1-3, 2-3): each node is the sole
     * intermediary for exactly one pair (e.g., node 0 mediates {1,2} via
     * 1-0-2, but sigma_{1,2}=2 since 1-3-2 also exists), giving C_B = 0.5.
     * By symmetry all four nodes have equal betweenness. */
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT((cents[i] - 0.5) >= (-1e-9), "double >= failed");
        TEST_ASSERT((cents[i] - 0.5) <= (1e-9), "double <= failed");
    }

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: betweenness centrality (Brandes, sampled)
 * -------------------------------------------------------------------------- */

static test_result_t test_betweenness_sampled(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_t* tbl = ray_table_new(1);
    ray_t* dummy_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("_dummy", 6), dummy_vec);
    ray_release(dummy_vec);
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* bc = ray_betweenness(g, rel, 2);  /* sample 2 sources */
    ray_t* result = ray_execute(g, bc);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    /* Verify centrality values are non-negative */
    int64_t cent_sym = ray_sym_intern("_centrality", 11);
    ray_t* cent_col = ray_table_get_col(result, cent_sym);
    TEST_ASSERT_NOT_NULL(cent_col);
    double* cents = (double*)ray_data(cent_col);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT((cents[i]) >= (0.0), "double >= failed");
    }

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_closeness(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();  /* 4-node cycle */
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_t* tbl = ray_table_new(1);
    ray_t* dummy_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("_dummy", 6), dummy_vec);
    ray_release(dummy_vec);
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* cc = ray_closeness(g, rel, 0);  /* exact */
    ray_t* result = ray_execute(g, cc);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    int64_t cent_sym = ray_sym_intern("_centrality", 11);
    ray_t* cent_col = ray_table_get_col(result, cent_sym);
    double* cents = (double*)ray_data(cent_col);

    /* All nodes should have positive closeness in a connected graph */
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT((cents[i]) > (0.0), "double > failed");
        TEST_ASSERT((cents[i]) <= (1.0), "double <= failed");
    }

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_closeness_sampled(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();  /* 4-node cycle */
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);

    ray_t* tbl = ray_table_new(1);
    ray_t* dummy_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("_dummy", 6), dummy_vec);
    ray_release(dummy_vec);
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* cc = ray_closeness(g, rel, 2);  /* sample 2 sources */
    ray_t* result = ray_execute(g, cc);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);

    /* Verify centrality values are positive */
    int64_t cent_sym = ray_sym_intern("_centrality", 11);
    ray_t* cent_col = ray_table_get_col(result, cent_sym);
    TEST_ASSERT_NOT_NULL(cent_col);
    double* cents = (double*)ray_data(cent_col);
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT((cents[i]) > (0.0), "double > failed");
    }

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_mst(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges; ray_rel_t* rel; ray_t* node_props;
    make_astar_graph(&edges, &rel, &node_props);
    /* 5 nodes, 6 edges: 0->1(1), 0->2(4), 1->3(2), 2->3(1), 3->4(3), 1->4(10)
     * MST (undirected) should pick: 0-1(1), 2-3(1), 1-3(2), 3-4(3) = total 7
     * (skip 0-2(4) and 1-4(10)) */

    ray_t* tbl = ray_table_new(1);
    ray_t* dummy_vec = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("_dummy", 6), dummy_vec);
    ray_release(dummy_vec);
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* mst = ray_mst(g, rel, "weight");
    TEST_ASSERT_NOT_NULL(mst);

    ray_t* result = ray_execute(g, mst);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    /* MST of 5 nodes has 4 edges */
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    /* Total weight should be 7.0 */
    int64_t w_sym = ray_sym_intern("_weight", 7);
    ray_t* w_col = ray_table_get_col(result, w_sym);
    TEST_ASSERT_NOT_NULL(w_col);
    double* ws = (double*)ray_data(w_col);
    double total = 0.0;
    for (int i = 0; i < 4; i++) total += ws[i];
    TEST_ASSERT((total) >= (6.99), "double >= failed");
    TEST_ASSERT((total) <= (7.01), "double <= failed");

    ray_release(result);
    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(edges);
    ray_release(node_props);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_rel_neighbors public API (fwd + rev + edge cases)
 * -------------------------------------------------------------------------- */

static test_result_t test_rel_neighbors_api(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, true);
    TEST_ASSERT_NOT_NULL(rel);

    /* Forward (direction = 0): node 0 -> {1, 2} (sorted) */
    int64_t cnt = -1;
    const int64_t* nbrs = ray_rel_neighbors(rel, 0, 0, &cnt);
    TEST_ASSERT_NOT_NULL(nbrs);
    TEST_ASSERT_EQ_I(cnt, 2);
    TEST_ASSERT_EQ_I(nbrs[0], 1);
    TEST_ASSERT_EQ_I(nbrs[1], 2);

    /* Forward node 3 -> {0} */
    cnt = -1;
    nbrs = ray_rel_neighbors(rel, 3, 0, &cnt);
    TEST_ASSERT_NOT_NULL(nbrs);
    TEST_ASSERT_EQ_I(cnt, 1);
    TEST_ASSERT_EQ_I(nbrs[0], 0);

    /* Reverse (direction = 1): node 3 has incoming from {1, 2} */
    cnt = -1;
    nbrs = ray_rel_neighbors(rel, 3, 1, &cnt);
    TEST_ASSERT_NOT_NULL(nbrs);
    TEST_ASSERT_EQ_I(cnt, 2);
    TEST_ASSERT_EQ_I(nbrs[0], 1);
    TEST_ASSERT_EQ_I(nbrs[1], 2);

    /* Out-of-range node — should return NULL with cnt = 0 */
    cnt = -1;
    nbrs = ray_rel_neighbors(rel, 99, 0, &cnt);
    TEST_ASSERT_EQ_PTR((void*)nbrs, NULL);
    TEST_ASSERT_EQ_I(cnt, 0);

    /* Negative node — should return NULL with cnt = 0 */
    cnt = -1;
    nbrs = ray_rel_neighbors(rel, -1, 0, &cnt);
    TEST_ASSERT_EQ_PTR((void*)nbrs, NULL);
    TEST_ASSERT_EQ_I(cnt, 0);

    /* NULL rel — should return NULL with cnt = 0 (covers !rel branch) */
    cnt = -1;
    nbrs = ray_rel_neighbors(NULL, 0, 0, &cnt);
    TEST_ASSERT_EQ_PTR((void*)nbrs, NULL);
    TEST_ASSERT_EQ_I(cnt, 0);

    /* NULL rel without out_count must not crash (covers if-out_count branch) */
    nbrs = ray_rel_neighbors(NULL, 0, 0, NULL);
    TEST_ASSERT_EQ_PTR((void*)nbrs, NULL);

    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_rel_n_nodes public API (fwd, rev, NULL)
 * -------------------------------------------------------------------------- */

static test_result_t test_rel_n_nodes_api(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Asymmetric: 4 src nodes, 5 dst nodes */
    int64_t src_data[] = {0, 1, 2, 3};
    int64_t dst_data[] = {4, 0, 1, 2};
    ray_t* sv = ray_vec_from_raw(RAY_I64, src_data, 4);
    ray_t* dv = ray_vec_from_raw(RAY_I64, dst_data, 4);
    int64_t ss = ray_sym_intern("src", 3);
    int64_t ds = ray_sym_intern("dst", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ss, sv);
    tbl = ray_table_add_col(tbl, ds, dv);
    ray_release(sv); ray_release(dv);

    ray_rel_t* rel = ray_rel_from_edges(tbl, "src", "dst", 4, 5, false);
    TEST_ASSERT_NOT_NULL(rel);

    /* direction = 0: forward CSR carries n_src_nodes = 4 */
    TEST_ASSERT_EQ_I(ray_rel_n_nodes(rel, 0), 4);
    /* direction = 1: reverse CSR carries n_dst_nodes = 5 */
    TEST_ASSERT_EQ_I(ray_rel_n_nodes(rel, 1), 5);
    /* Non-zero direction values other than 1 fall through to fwd (only ==1 picks rev) */
    TEST_ASSERT_EQ_I(ray_rel_n_nodes(rel, 2), 4);

    /* NULL rel — returns 0 */
    TEST_ASSERT_EQ_I(ray_rel_n_nodes(NULL, 0), 0);
    TEST_ASSERT_EQ_I(ray_rel_n_nodes(NULL, 1), 0);

    ray_rel_free(rel);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_rel_set_props attaches & releases properly
 * -------------------------------------------------------------------------- */

static test_result_t test_rel_set_props_api(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    /* Build a small props table */
    int64_t w[] = {10, 20, 30, 40, 50, 60};
    ray_t* w_vec = ray_vec_from_raw(RAY_I64, w, 6);
    int64_t w_sym = ray_sym_intern("w", 1);
    ray_t* props = ray_table_new(1);
    props = ray_table_add_col(props, w_sym, w_vec);
    ray_release(w_vec);

    /* Initially both csr.props are NULL */
    TEST_ASSERT_EQ_PTR((void*)rel->fwd.props, NULL);
    TEST_ASSERT_EQ_PTR((void*)rel->rev.props, NULL);

    ray_rel_set_props(rel, props);
    TEST_ASSERT_EQ_PTR((void*)rel->fwd.props, (void*)props);
    TEST_ASSERT_EQ_PTR((void*)rel->rev.props, (void*)props);

    /* Calling again should release the old (same) pointer and re-retain */
    ray_rel_set_props(rel, props);
    TEST_ASSERT_EQ_PTR((void*)rel->fwd.props, (void*)props);

    /* NULL guards: must be no-op */
    ray_rel_set_props(NULL, props);
    ray_rel_set_props(rel, NULL);

    ray_release(props);
    ray_rel_free(rel);  /* releases retained props twice */
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_rel_save / load error paths
 * -------------------------------------------------------------------------- */

static test_result_t test_rel_save_load_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    /* NULL rel → RAY_ERR_IO */
    TEST_ASSERT_EQ_I(ray_rel_save(NULL, "/tmp/test_csr_err"), RAY_ERR_IO);
    /* NULL dir → RAY_ERR_IO */
    TEST_ASSERT_EQ_I(ray_rel_save(rel, NULL), RAY_ERR_IO);

    /* mkdir on a path under a non-existent parent — fails (not EEXIST) */
    TEST_ASSERT_EQ_I(ray_rel_save(rel, "/no/such/parent/dir"), RAY_ERR_IO);

    /* ray_rel_load on NULL or non-existent dir → NULL */
    TEST_ASSERT_EQ_PTR(ray_rel_load(NULL), NULL);
    TEST_ASSERT_EQ_PTR(ray_rel_load("/tmp/this_csr_dir_must_not_exist_xyz"), NULL);
    TEST_ASSERT_EQ_PTR(ray_rel_mmap(NULL), NULL);
    TEST_ASSERT_EQ_PTR(ray_rel_mmap("/tmp/this_csr_dir_must_not_exist_xyz"), NULL);

    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: save then mmap (mmap path of csr_load_impl) + idempotent save (EEXIST branch)
 * -------------------------------------------------------------------------- */

static test_result_t test_rel_save_mmap_reuse(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, true);
    TEST_ASSERT_NOT_NULL(rel);

    const char* dir = "/tmp/test_csr_mmap_reuse";
    /* First save creates dir */
    TEST_ASSERT_EQ_I(ray_rel_save(rel, dir), RAY_OK);
    /* Second save into existing dir hits the EEXIST branch (mkdir returns -1, errno=EEXIST) */
    TEST_ASSERT_EQ_I(ray_rel_save(rel, dir), RAY_OK);

    /* mmap path */
    ray_rel_t* mm = ray_rel_mmap(dir);
    TEST_ASSERT_NOT_NULL(mm);
    TEST_ASSERT_EQ_I(mm->fwd.n_nodes, rel->fwd.n_nodes);
    TEST_ASSERT_EQ_I(mm->fwd.n_edges, rel->fwd.n_edges);

    /* Sanity: neighbors via public API match */
    int64_t cnt_o, cnt_m;
    const int64_t* o = ray_rel_neighbors(rel, 1, 0, &cnt_o);
    const int64_t* m = ray_rel_neighbors(mm, 1, 0, &cnt_m);
    TEST_ASSERT_EQ_I(cnt_o, cnt_m);
    for (int64_t i = 0; i < cnt_o; i++) TEST_ASSERT_EQ_I(o[i], m[i]);

    ray_rel_free(mm);
    ray_rel_free(rel);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_rel_free(NULL) is a safe no-op (covers null guard)
 * -------------------------------------------------------------------------- */

static test_result_t test_rel_free_null(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_rel_free(NULL);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: ray_rel_from_edges argument validation paths
 * -------------------------------------------------------------------------- */

static test_result_t test_rel_from_edges_errors(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* edges = make_edge_table();

    /* NULL edge table */
    TEST_ASSERT_EQ_PTR(ray_rel_from_edges(NULL, "src", "dst", 4, 4, false), NULL);

    /* Non-table input (vector) — type != RAY_TABLE branch */
    int64_t junk[] = {0, 1, 2};
    ray_t* v = ray_vec_from_raw(RAY_I64, junk, 3);
    TEST_ASSERT_EQ_PTR(ray_rel_from_edges(v, "src", "dst", 4, 4, false), NULL);
    ray_release(v);

    /* Missing column name → ray_table_get_col returns NULL */
    TEST_ASSERT_EQ_PTR(ray_rel_from_edges(edges, "no_such_col", "dst", 4, 4, false), NULL);
    TEST_ASSERT_EQ_PTR(ray_rel_from_edges(edges, "src", "no_such_col", 4, 4, false), NULL);

    /* Negative n_dst_nodes */
    TEST_ASSERT_EQ_PTR(ray_rel_from_edges(edges, "src", "dst", 4, -1, false), NULL);

    /* Mismatched column lengths: build new table where src and dst differ in length */
    int64_t s2[] = {0, 1};
    int64_t d2[] = {0};
    ray_t* sv = ray_vec_from_raw(RAY_I64, s2, 2);
    ray_t* dv = ray_vec_from_raw(RAY_I64, d2, 1);
    int64_t ss = ray_sym_intern("src", 3);
    int64_t ds = ray_sym_intern("dst", 3);
    ray_t* bad = ray_table_new(2);
    bad = ray_table_add_col(bad, ss, sv);
    bad = ray_table_add_col(bad, ds, dv);
    ray_release(sv); ray_release(dv);
    TEST_ASSERT_EQ_PTR(ray_rel_from_edges(bad, "src", "dst", 4, 4, false), NULL);
    ray_release(bad);

    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Suite definition
 * -------------------------------------------------------------------------- */

const test_entry_t csr_entries[] = {
    { "csr/build", test_csr_build, NULL, NULL },
    { "csr/sorted", test_csr_sorted, NULL, NULL },
    { "csr/expand", test_expand, NULL, NULL },
    { "csr/expand_reverse", test_expand_reverse, NULL, NULL },
    { "csr/var_expand", test_var_expand, NULL, NULL },
    { "csr/shortest_path", test_shortest_path, NULL, NULL },
    { "csr/shortest_path_no", test_shortest_path_no_path, NULL, NULL },
    { "csr/wco_join", test_wco_join_triangle, NULL, NULL },
    { "csr/wco_chain", test_wco_join_chain, NULL, NULL },
    { "csr/factorized", test_expand_factorized, NULL, NULL },
    { "csr/sip_expand", test_sip_expand, NULL, NULL },
    { "csr/sip_auto", test_sip_auto_build, NULL, NULL },
    { "csr/sjoin", test_sjoin_filter, NULL, NULL },
    { "csr/asp_join", test_asp_join, NULL, NULL },
    { "csr/fact_group", test_factorized_group, NULL, NULL },
    { "csr/multi_table", test_multi_table, NULL, NULL },
    { "csr/expand_both", test_expand_both, NULL, NULL },
    { "csr/var_rev", test_var_expand_reverse, NULL, NULL },
    { "csr/var_both", test_var_expand_both, NULL, NULL },
    { "csr/sp_reverse", test_shortest_path_reverse, NULL, NULL },
    { "csr/save_load", test_csr_save_load, NULL, NULL },
    { "csr/out_of_range", test_csr_out_of_range, NULL, NULL },
    { "csr/empty", test_csr_empty, NULL, NULL },
    { "csr/type_check", test_csr_type_validation, NULL, NULL },
    { "csr/self_loop", test_self_loop, NULL, NULL },
    { "csr/empty_src", test_expand_empty_src, NULL, NULL },
    { "csr/rel_build", test_rel_build, NULL, NULL },
    { "csr/sp_both", test_shortest_path_both, NULL, NULL },
    { "csr/var_depth0", test_var_expand_depth0, NULL, NULL },
    { "csr/wco_unsorted", test_wco_unsorted, NULL, NULL },
    { "csr/expand_oob_src", test_expand_oob_src, NULL, NULL },
    { "csr/triangle_exact", test_triangle_exact, NULL, NULL },
    { "csr/wco_chain4", test_wco_chain4, NULL, NULL },
    { "csr/sp_self", test_shortest_path_self, NULL, NULL },
    { "csr/save_load_rev", test_save_load_rev, NULL, NULL },
    { "csr/wco_4clique", test_wco_4clique, NULL, NULL },
    { "csr/fvec_mat", test_fvec_materialize, NULL, NULL },
    { "csr/fvec_empty", test_fvec_empty, NULL, NULL },
    { "csr/fvec_semijoin", test_fvec_semijoin, NULL, NULL },
    { "csr/fact_rev", test_factorized_reverse, NULL, NULL },
    { "csr/wco_empty", test_wco_empty_result, NULL, NULL },
    { "csr/var_bad_range", test_var_expand_bad_range, NULL, NULL },
    { "csr/degree_cent", test_degree_cent, NULL, NULL },
    { "csr/topsort", test_topsort, NULL, NULL },
    { "csr/topsort_cycle", test_topsort_cycle, NULL, NULL },
    { "csr/dfs", test_dfs, NULL, NULL },
    { "csr/dfs_max_depth", test_dfs_max_depth, NULL, NULL },
    { "csr/cluster_coeff", test_cluster_coeff, NULL, NULL },
    { "csr/random_walk", test_random_walk, NULL, NULL },
    { "csr/astar", test_astar, NULL, NULL },
    { "csr/k_shortest", test_k_shortest, NULL, NULL },
    { "csr/betweenness", test_betweenness, NULL, NULL },
    { "csr/betweenness_s", test_betweenness_sampled, NULL, NULL },
    { "csr/closeness", test_closeness, NULL, NULL },
    { "csr/closeness_s", test_closeness_sampled, NULL, NULL },
    { "csr/mst", test_mst, NULL, NULL },
    { "csr/rel_neighbors_api", test_rel_neighbors_api, NULL, NULL },
    { "csr/rel_n_nodes_api", test_rel_n_nodes_api, NULL, NULL },
    { "csr/rel_set_props_api", test_rel_set_props_api, NULL, NULL },
    { "csr/rel_save_load_errors", test_rel_save_load_errors, NULL, NULL },
    { "csr/rel_save_mmap_reuse", test_rel_save_mmap_reuse, NULL, NULL },
    { "csr/rel_free_null", test_rel_free_null, NULL, NULL },
    { "csr/rel_from_edges_errors", test_rel_from_edges_errors, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


