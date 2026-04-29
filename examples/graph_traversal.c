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
 * graph_traversal.c -- Rayforce example: CSR graph + variable-length expansion
 *
 * Creates a directed graph with 5 nodes and 5 edges:
 *   0 -> 1, 0 -> 2, 1 -> 3, 2 -> 3, 3 -> 4
 *
 * Builds a CSR relationship, then performs a variable-length expansion
 * (BFS) from node 0 with depth 1..2 hops.
 *
 * Build:  cmake -B build -DRAYFORCE_EXAMPLES=ON && cmake --build build
 * Run:    ./build/example_graph_traversal
 */

#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include <stdio.h>

int main(void) {
    ray_heap_init();
    assert(ray_sym_init() == RAY_OK);

    /* --- Build edge table ---------------------------------------------- */

    int64_t src_data[] = {0, 0, 1, 2, 3};
    int64_t dst_data[] = {1, 2, 3, 3, 4};
    int64_t n_edges = 5;

    ray_t* src_vec = ray_vec_from_raw(RAY_I64, src_data, n_edges);
    ray_t* dst_vec = ray_vec_from_raw(RAY_I64, dst_data, n_edges);

    int64_t sym_src = ray_sym_intern("src", 3);
    int64_t sym_dst = ray_sym_intern("dst", 3);

    ray_t* edge_tbl = ray_table_new(2);
    edge_tbl = ray_table_add_col(edge_tbl, sym_src, src_vec);
    edge_tbl = ray_table_add_col(edge_tbl, sym_dst, dst_vec);
    ray_release(src_vec);
    ray_release(dst_vec);

    printf("Edge table: %lld edges\n", (long long)ray_table_nrows(edge_tbl));

    /* --- Build CSR relationship ---------------------------------------- */

    int64_t n_nodes = 5;
    ray_rel_t* rel = ray_rel_from_edges(edge_tbl, "src", "dst",
                                       n_nodes, n_nodes, false);
    if (!rel) {
        printf("ERROR: failed to build CSR relationship\n");
        ray_release(edge_tbl);
        ray_sym_destroy();
        ray_heap_destroy();
        return 1;
    }
    printf("CSR relationship built for %lld nodes\n", (long long)n_nodes);

    /* --- Variable-length expansion from node 0, depth 1..2 ------------- */

    int64_t start_data[] = {0};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 1);

    ray_graph_t* g = ray_graph_new(NULL);
    ray_op_t* src_op = ray_const_vec(g, start_vec);

    /* direction=0 (forward), min_depth=1, max_depth=2, track_path=false */
    ray_op_t* var_exp = ray_var_expand(g, src_op, rel, 0, 1, 2, false);

    ray_t* result = ray_execute(g, var_exp);
    if (RAY_IS_ERR(result)) {
        printf("ERROR: %s\n", ray_err_code(result));
        ray_graph_free(g);
        ray_release(start_vec);
        ray_rel_free(rel);
        ray_release(edge_tbl);
        ray_sym_destroy();
        ray_heap_destroy();
        return 1;
    }

    /* --- Print results ------------------------------------------------- */

    int64_t nrows = ray_table_nrows(result);
    int64_t ncols = ray_table_ncols(result);
    printf("\nVar-expand from node 0, depth 1..2:\n");
    printf("  Result: %lld rows, %lld cols\n",
           (long long)nrows, (long long)ncols);

    /* Expected reachable nodes:
     *   depth 1: 0->1, 0->2          => nodes 1, 2
     *   depth 2: 1->3, 2->3 (dedup)  => node  3
     *   Total: 3 reachable nodes */
    printf("  Expected 3 reachable nodes (1, 2 at depth 1; 3 at depth 2)\n");

    /* Result columns: _start (source node), _end (reached node), _depth */
    ray_t* start_col = ray_table_get_col_idx(result, 0);
    ray_t* end_col   = ray_table_get_col_idx(result, 1);
    ray_t* depth_col = ray_table_get_col_idx(result, 2);

    if (start_col && end_col && depth_col) {
        int64_t* starts = (int64_t*)ray_data(start_col);
        int64_t* ends   = (int64_t*)ray_data(end_col);
        int64_t* depths = (int64_t*)ray_data(depth_col);
        printf("\n  %-8s %-8s %s\n", "start", "end", "depth");
        printf("  %-8s %-8s %s\n", "-----", "---", "-----");
        for (int64_t i = 0; i < nrows; i++) {
            printf("  %-8lld %-8lld %lld\n",
                   (long long)starts[i],
                   (long long)ends[i],
                   (long long)depths[i]);
        }
    }

    /* --- Cleanup ------------------------------------------------------- */

    ray_release(result);
    ray_graph_free(g);
    ray_release(start_vec);
    ray_rel_free(rel);
    ray_release(edge_tbl);
    ray_sym_destroy();
    ray_heap_destroy();

    printf("\nDone.\n");
    return 0;
}
