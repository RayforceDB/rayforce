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
 * analytics.c -- Rayforce example: group-by + sum aggregation
 *
 * Creates a 12-row sales table (region, category, amount), groups by
 * region, and computes sum(amount) per region.
 *
 * Build:  cmake -B build -DRAYFORCE_EXAMPLES=ON && cmake --build build
 * Run:    ./build/example_analytics
 */

#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include <stdio.h>

int main(void) {
    ray_heap_init();
    assert(ray_sym_init() == RAY_OK);

    /* --- Build a 12-row sales table ------------------------------------ */

    int64_t region_data[]   = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2};
    int64_t amount_data[]   = {100, 200, 150, 300, 400, 50, 250, 350, 175, 225, 125, 275};
    int64_t n = 12;

    ray_t* region_vec = ray_vec_from_raw(RAY_I64, region_data, n);
    ray_t* amount_vec = ray_vec_from_raw(RAY_I64, amount_data, n);

    int64_t sym_region = ray_sym_intern("region", 6);
    int64_t sym_amount = ray_sym_intern("amount", 6);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_region, region_vec);
    tbl = ray_table_add_col(tbl, sym_amount, amount_vec);

    ray_release(region_vec);
    ray_release(amount_vec);

    printf("Input table: %lld rows, %lld cols\n",
           (long long)ray_table_nrows(tbl),
           (long long)ray_table_ncols(tbl));

    /* --- Group by region, sum(amount) ---------------------------------- */

    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* reg_op = ray_scan(g, "region");
    ray_op_t* amt_op = ray_scan(g, "amount");

    ray_op_t* keys[]    = { reg_op };
    ray_op_t* agg_ins[] = { amt_op };
    uint16_t agg_ops[] = { OP_SUM };

    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);

    /* --- Execute ------------------------------------------------------- */

    ray_t* result = ray_execute(g, grp);
    if (RAY_IS_ERR(result)) {
        printf("ERROR: %s\n", ray_err_code(result));
        ray_graph_free(g);
        ray_release(tbl);
        ray_sym_destroy();
        ray_heap_destroy();
        return 1;
    }

    /* --- Print results ------------------------------------------------- */

    int64_t nrows = ray_table_nrows(result);
    int64_t ncols = ray_table_ncols(result);
    printf("Result table: %lld rows, %lld cols\n",
           (long long)nrows, (long long)ncols);

    ray_t* key_col = ray_table_get_col_idx(result, 0);
    ray_t* sum_col = ray_table_get_col_idx(result, 1);
    int64_t* regions = (int64_t*)ray_data(key_col);
    int64_t* sums    = (int64_t*)ray_data(sum_col);

    /* Expected sums:
     *   region 0: 100+200+150+300 = 750
     *   region 1: 400+50+250+350  = 1050
     *   region 2: 175+225+125+275 = 800 */
    printf("\nGroup by region, sum(amount):\n");
    printf("  %-10s %s\n", "region", "sum(amount)");
    printf("  %-10s %s\n", "------", "-----------");
    for (int64_t i = 0; i < nrows; i++) {
        printf("  %-10lld %lld\n", (long long)regions[i], (long long)sums[i]);
    }

    /* --- Cleanup ------------------------------------------------------- */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();

    printf("\nDone.\n");
    return 0;
}
