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
 * join_tables.c -- Rayforce example: inner join of two tables
 *
 * Creates an orders table (5 rows: order_id, customer_id, amount) and
 * a customers table (3 rows: customer_id, score). Performs an inner
 * join on customer_id and prints the result shape.
 *
 * Build:  cmake -B build -DRAYFORCE_EXAMPLES=ON && cmake --build build
 * Run:    ./build/example_join_tables
 */

#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include <stdio.h>

int main(void) {
    ray_heap_init();
    assert(ray_sym_init() == RAY_OK);

    /* --- Orders table: 5 rows ------------------------------------------ */

    int64_t oid_data[] = {1, 2, 3, 4, 5};
    int64_t cid_data[] = {10, 20, 10, 30, 20};
    int64_t amt_data[] = {500, 300, 700, 200, 450};
    int64_t n_orders = 5;

    ray_t* oid_vec = ray_vec_from_raw(RAY_I64, oid_data, n_orders);
    ray_t* cid_vec = ray_vec_from_raw(RAY_I64, cid_data, n_orders);
    ray_t* amt_vec = ray_vec_from_raw(RAY_I64, amt_data, n_orders);

    int64_t sym_oid = ray_sym_intern("order_id",    8);
    int64_t sym_cid = ray_sym_intern("customer_id", 11);
    int64_t sym_amt = ray_sym_intern("amount",      6);

    ray_t* orders = ray_table_new(3);
    orders = ray_table_add_col(orders, sym_oid, oid_vec);
    orders = ray_table_add_col(orders, sym_cid, cid_vec);
    orders = ray_table_add_col(orders, sym_amt, amt_vec);
    ray_release(oid_vec);
    ray_release(cid_vec);
    ray_release(amt_vec);

    printf("Orders table:    %lld rows, %lld cols\n",
           (long long)ray_table_nrows(orders),
           (long long)ray_table_ncols(orders));

    /* --- Customers table: 3 rows --------------------------------------- */

    int64_t cust_cid_data[]   = {10, 20, 30};
    int64_t cust_score_data[] = {85, 92, 78};
    int64_t n_customers = 3;

    ray_t* cust_cid_vec   = ray_vec_from_raw(RAY_I64, cust_cid_data,   n_customers);
    ray_t* cust_score_vec = ray_vec_from_raw(RAY_I64, cust_score_data, n_customers);

    int64_t sym_score = ray_sym_intern("score", 5);

    ray_t* customers = ray_table_new(2);
    customers = ray_table_add_col(customers, sym_cid, cust_cid_vec);
    customers = ray_table_add_col(customers, sym_score, cust_score_vec);
    ray_release(cust_cid_vec);
    ray_release(cust_score_vec);

    printf("Customers table: %lld rows, %lld cols\n",
           (long long)ray_table_nrows(customers),
           (long long)ray_table_ncols(customers));

    /* --- Inner join on customer_id ------------------------------------- */

    ray_graph_t* g = ray_graph_new(orders);

    ray_op_t* left_op  = ray_const_table(g, orders);
    ray_op_t* right_op = ray_const_table(g, customers);

    ray_op_t* key_op     = ray_scan(g, "customer_id");
    ray_op_t* lk_arr[]   = { key_op };
    ray_op_t* rk_arr[]   = { key_op };

    /* join_type 0 = inner join */
    ray_op_t* join_op = ray_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    ray_t* result = ray_execute(g, join_op);
    if (RAY_IS_ERR(result)) {
        printf("ERROR: %s\n", ray_err_code(result));
        ray_graph_free(g);
        ray_release(orders);
        ray_release(customers);
        ray_sym_destroy();
        ray_heap_destroy();
        return 1;
    }

    /* --- Print results ------------------------------------------------- */

    int64_t nrows = ray_table_nrows(result);
    int64_t ncols = ray_table_ncols(result);
    printf("\nInner join result: %lld rows, %lld cols\n",
           (long long)nrows, (long long)ncols);

    /* Every order has a matching customer (all cids 10,20,30 exist),
     * so we expect 5 result rows with columns from both tables. */
    printf("  Expected: 5 rows (every order matched a customer)\n");

    /* Print joined data */
    if (nrows > 0) {
        ray_t* res_cid = ray_table_get_col(result, sym_cid);
        ray_t* res_amt = ray_table_get_col(result, sym_amt);
        ray_t* res_score = ray_table_get_col(result, sym_score);

        if (res_cid && res_amt && res_score) {
            int64_t* cids   = (int64_t*)ray_data(res_cid);
            int64_t* amts   = (int64_t*)ray_data(res_amt);
            int64_t* scores = (int64_t*)ray_data(res_score);

            printf("\n  %-14s %-8s %s\n", "customer_id", "amount", "score");
            printf("  %-14s %-8s %s\n", "-----------", "------", "-----");
            for (int64_t i = 0; i < nrows; i++) {
                printf("  %-14lld %-8lld %lld\n",
                       (long long)cids[i],
                       (long long)amts[i],
                       (long long)scores[i]);
            }
        }
    }

    /* --- Cleanup ------------------------------------------------------- */

    ray_release(result);
    ray_graph_free(g);
    ray_release(orders);
    ray_release(customers);
    ray_sym_destroy();
    ray_heap_destroy();

    printf("\nDone.\n");
    return 0;
}
