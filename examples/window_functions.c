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
 * window_functions.c -- Rayforce example: RANK window function
 *
 * Creates a 6-row table (dept, revenue) and computes RANK() partitioned
 * by dept, ordered by revenue DESC.
 *
 * Build:  cmake -B build -DRAYFORCE_EXAMPLES=ON && cmake --build build
 * Run:    ./build/example_window_functions
 */

#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include <stdio.h>

int main(void) {
    ray_heap_init();
    assert(ray_sym_init() == RAY_OK);

    /* --- Build a 6-row table ------------------------------------------- */

    int64_t dept_data[]    = {1, 1, 1, 2, 2, 2};
    int64_t revenue_data[] = {300, 100, 200, 500, 400, 600};
    int64_t n = 6;

    ray_t* dept_vec    = ray_vec_from_raw(RAY_I64, dept_data,    n);
    ray_t* revenue_vec = ray_vec_from_raw(RAY_I64, revenue_data, n);

    int64_t sym_dept    = ray_sym_intern("dept",    4);
    int64_t sym_revenue = ray_sym_intern("revenue", 7);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_dept,    dept_vec);
    tbl = ray_table_add_col(tbl, sym_revenue, revenue_vec);
    ray_release(dept_vec);
    ray_release(revenue_vec);

    printf("Input table: %lld rows, %lld cols\n",
           (long long)ray_table_nrows(tbl),
           (long long)ray_table_ncols(tbl));

    /* --- Window: RANK() PARTITION BY dept ORDER BY revenue DESC -------- */

    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* tbl_op     = ray_const_table(g, tbl);
    ray_op_t* dept_op    = ray_scan(g, "dept");
    ray_op_t* revenue_op = ray_scan(g, "revenue");

    ray_op_t* parts[]       = { dept_op };
    ray_op_t* orders[]      = { revenue_op };
    uint8_t  order_descs[] = { 1 };            /* 1 = descending */
    uint8_t  func_kinds[]  = { RAY_WIN_RANK };
    ray_op_t* func_inputs[] = { revenue_op };
    int64_t  func_params[] = { 0 };

    ray_op_t* win = ray_window_op(g, tbl_op,
                                parts, 1,
                                orders, order_descs, 1,
                                func_kinds, func_inputs, func_params, 1,
                                RAY_FRAME_ROWS,
                                RAY_BOUND_UNBOUNDED_PRECEDING,
                                RAY_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);

    ray_t* result = ray_execute(g, win);
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

    /* The window op appends a rank column to the original 2 columns */
    printf("  (original 2 cols + 1 window column = %lld cols)\n",
           (long long)ncols);

    /* Print all columns */
    if (nrows > 0 && ncols >= 3) {
        ray_t* d_col = ray_table_get_col_idx(result, 0);
        ray_t* r_col = ray_table_get_col_idx(result, 1);
        ray_t* w_col = ray_table_get_col_idx(result, 2);

        if (d_col && r_col && w_col) {
            int64_t* depts    = (int64_t*)ray_data(d_col);
            int64_t* revenues = (int64_t*)ray_data(r_col);
            int64_t* ranks    = (int64_t*)ray_data(w_col);

            printf("\n  %-6s %-10s %s\n", "dept", "revenue", "rank");
            printf("  %-6s %-10s %s\n", "----", "-------", "----");
            for (int64_t i = 0; i < nrows; i++) {
                printf("  %-6lld %-10lld %lld\n",
                       (long long)depts[i],
                       (long long)revenues[i],
                       (long long)ranks[i]);
            }
        }
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
