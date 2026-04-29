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

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "mem/heap.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void table_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void table_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- table_new ------------------------------------------------------------ */

static test_result_t test_table_new(void) {
    ray_t* tbl = ray_table_new(4);
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT_EQ_I(tbl->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_ncols(tbl), 0);
    ray_t* schema = ray_table_schema(tbl);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQ_U(schema->rc, 1);

    ray_release(tbl);
    PASS();
}

/* ---- table_add_col -------------------------------------------------------- */

static test_result_t test_table_add_col(void) {
    ray_t* tbl = ray_table_new(4);
    TEST_ASSERT_NOT_NULL(tbl);

    int64_t name_id = ray_sym_intern("x", 1);
    int64_t raw[] = {10, 20, 30};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_NOT_NULL(col);

    tbl = ray_table_add_col(tbl, name_id, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT_EQ_I(ray_table_ncols(tbl), 1);

    ray_release(col);
    ray_release(tbl);
    PASS();
}

/* ---- table_get_col_by_name ------------------------------------------------ */

static test_result_t test_table_get_col_by_name(void) {
    ray_t* tbl = ray_table_new(4);
    int64_t name_id = ray_sym_intern("price", 5);
    double raw[] = {1.5, 2.5, 3.5};
    ray_t* col = ray_vec_from_raw(RAY_F64, raw, 3);

    tbl = ray_table_add_col(tbl, name_id, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_t* got = ray_table_get_col(tbl, name_id);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_PTR(got, col);

    /* Non-existent column returns NULL */
    int64_t other_id = ray_sym_intern("missing", 7);
    ray_t* missing = ray_table_get_col(tbl, other_id);
    TEST_ASSERT_NULL(missing);

    ray_release(col);
    ray_release(tbl);
    PASS();
}

/* ---- table_get_col_by_idx ------------------------------------------------- */

static test_result_t test_table_get_col_by_idx(void) {
    ray_t* tbl = ray_table_new(4);
    int64_t name_id = ray_sym_intern("val", 3);
    int64_t raw[] = {100, 200};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 2);

    tbl = ray_table_add_col(tbl, name_id, col);

    ray_t* got = ray_table_get_col_idx(tbl, 0);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_PTR(got, col);

    /* Out of range */
    ray_t* oob = ray_table_get_col_idx(tbl, 1);
    TEST_ASSERT_NULL(oob);
    oob = ray_table_get_col_idx(tbl, -1);
    TEST_ASSERT_NULL(oob);

    ray_release(col);
    ray_release(tbl);
    PASS();
}

/* ---- table_col_name ------------------------------------------------------- */

static test_result_t test_table_col_name(void) {
    ray_t* tbl = ray_table_new(4);
    int64_t id_a = ray_sym_intern("alpha", 5);
    int64_t id_b = ray_sym_intern("beta", 4);
    int64_t raw[] = {1, 2, 3};
    ray_t* col_a = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_t* col_b = ray_vec_from_raw(RAY_I64, raw, 3);

    tbl = ray_table_add_col(tbl, id_a, col_a);
    tbl = ray_table_add_col(tbl, id_b, col_b);

    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 0), id_a);
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 1), id_b);

    /* Out of range */
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 2), -1);

    ray_release(col_a);
    ray_release(col_b);
    ray_release(tbl);
    PASS();
}

/* ---- table_nrows ---------------------------------------------------------- */

static test_result_t test_table_nrows(void) {
    ray_t* tbl = ray_table_new(4);
    /* Empty tbl has 0 rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(tbl), 0);

    int64_t name_id = ray_sym_intern("col1", 4);
    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 5);

    tbl = ray_table_add_col(tbl, name_id, col);
    TEST_ASSERT_EQ_I(ray_table_nrows(tbl), 5);

    ray_release(col);
    ray_release(tbl);
    PASS();
}

/* ---- table_schema --------------------------------------------------------- */

static test_result_t test_table_schema(void) {
    ray_t* tbl = ray_table_new(4);
    ray_t* schema = ray_table_schema(tbl);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_EQ_I(schema->type, RAY_I64);
    TEST_ASSERT_EQ_I(schema->len, 0);  /* no columns yet */

    int64_t id_x = ray_sym_intern("x", 1);
    int64_t raw[] = {1};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 1);
    tbl = ray_table_add_col(tbl, id_x, col);

    schema = ray_table_schema(tbl);
    TEST_ASSERT_EQ_I(schema->len, 1);
    int64_t* ids = (int64_t*)ray_data(schema);
    TEST_ASSERT_EQ_I(ids[0], id_x);

    ray_release(col);
    ray_release(tbl);
    PASS();
}

/* ---- table_multiple_cols -------------------------------------------------- */

static test_result_t test_table_multiple_cols(void) {
    ray_t* tbl = ray_table_new(8);

    int64_t id_a = ray_sym_intern("a", 1);
    int64_t id_b = ray_sym_intern("b", 1);
    int64_t id_c = ray_sym_intern("c", 1);

    int64_t raw_a[] = {1, 2, 3};
    double  raw_b[] = {1.1, 2.2, 3.3};
    uint8_t raw_c[] = {1, 0, 1};

    ray_t* col_a = ray_vec_from_raw(RAY_I64, raw_a, 3);
    ray_t* col_b = ray_vec_from_raw(RAY_F64, raw_b, 3);
    ray_t* col_c = ray_vec_from_raw(RAY_BOOL, raw_c, 3);

    tbl = ray_table_add_col(tbl, id_a, col_a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_b, col_b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_c, col_c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    TEST_ASSERT_EQ_I(ray_table_ncols(tbl), 3);
    TEST_ASSERT_EQ_I(ray_table_nrows(tbl), 3);

    /* Verify by name */
    TEST_ASSERT_EQ_PTR(ray_table_get_col(tbl, id_a), col_a);
    TEST_ASSERT_EQ_PTR(ray_table_get_col(tbl, id_b), col_b);
    TEST_ASSERT_EQ_PTR(ray_table_get_col(tbl, id_c), col_c);

    /* Verify by index */
    TEST_ASSERT_EQ_PTR(ray_table_get_col_idx(tbl, 0), col_a);
    TEST_ASSERT_EQ_PTR(ray_table_get_col_idx(tbl, 1), col_b);
    TEST_ASSERT_EQ_PTR(ray_table_get_col_idx(tbl, 2), col_c);

    /* Verify column names */
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 0), id_a);
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 1), id_b);
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 2), id_c);

    /* Verify schema */
    ray_t* schema = ray_table_schema(tbl);
    TEST_ASSERT_EQ_I(schema->len, 3);
    int64_t* ids = (int64_t*)ray_data(schema);
    TEST_ASSERT_EQ_I(ids[0], id_a);
    TEST_ASSERT_EQ_I(ids[1], id_b);
    TEST_ASSERT_EQ_I(ids[2], id_c);

    /* Verify data integrity */
    int64_t* data_a = (int64_t*)ray_data(col_a);
    TEST_ASSERT_EQ_I(data_a[0], 1);
    TEST_ASSERT_EQ_I(data_a[2], 3);

    double* data_b = (double*)ray_data(col_b);
    TEST_ASSERT((data_b[1]) == (2.2), "double == failed");

    ray_release(col_a);
    ray_release(col_b);
    ray_release(col_c);
    ray_release(tbl);
    PASS();
}

/* ---- table_realloc_preserves_all_cols ----------------------------------- */

static test_result_t test_table_realloc_preserves_all_cols(void) {
    ray_t* tbl = ray_table_new(1); /* force growth while appending */
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_t* cols[4] = {0};
    int64_t vals[4] = {11, 22, 33, 44};

    for (int i = 0; i < 4; i++) {
        cols[i] = ray_vec_from_raw(RAY_I64, &vals[i], 1);
        TEST_ASSERT_NOT_NULL(cols[i]);
        TEST_ASSERT_FALSE(RAY_IS_ERR(cols[i]));

        char name[8];
        int n = snprintf(name, sizeof(name), "c%d", i);
        TEST_ASSERT((n) > (0), "n > 0");
        int64_t name_id = ray_sym_intern(name, (size_t)n);

        tbl = ray_table_add_col(tbl, name_id, cols[i]);
        TEST_ASSERT_NOT_NULL(tbl);
        TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    }

    TEST_ASSERT_EQ_I(ray_table_ncols(tbl), 4);
    for (int i = 0; i < 4; i++) {
        ray_t* col = ray_table_get_col_idx(tbl, i);
        TEST_ASSERT_NOT_NULL(col);
        TEST_ASSERT_EQ_I(col->type, RAY_I64);
        TEST_ASSERT_EQ_I(col->len, 1);
        int64_t* data = (int64_t*)ray_data(col);
        TEST_ASSERT_EQ_I(data[0], vals[i]);
    }

    for (int i = 0; i < 4; i++) ray_release(cols[i]);
    ray_release(tbl);
    PASS();
}

/* ---- table_release_drops_col_ref ---------------------------------------- */

static test_result_t test_table_release_drops_col_ref(void) {
    int64_t raw[] = {7, 8, 9};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col));

    ray_t* tbl = ray_table_new(1);
    int64_t name_id = ray_sym_intern("x", 1);
    tbl = ray_table_add_col(tbl, name_id, col);
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    TEST_ASSERT_EQ_U(col->rc, 2);
    ray_release(tbl);
    TEST_ASSERT_EQ_U(col->rc, 1);

    ray_release(col);
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t table_entries[] = {
    { "table/new", test_table_new, table_setup, table_teardown },
    { "table/add_col", test_table_add_col, table_setup, table_teardown },
    { "table/get_col_by_name", test_table_get_col_by_name, table_setup, table_teardown },
    { "table/get_col_by_idx", test_table_get_col_by_idx, table_setup, table_teardown },
    { "table/col_name", test_table_col_name, table_setup, table_teardown },
    { "table/nrows", test_table_nrows, table_setup, table_teardown },
    { "table/schema", test_table_schema, table_setup, table_teardown },
    { "table/multiple_cols", test_table_multiple_cols, table_setup, table_teardown },
    { "table/realloc_preserves_all_cols", test_table_realloc_preserves_all_cols, table_setup, table_teardown },
    { "table/release_drops_col_ref", test_table_release_drops_col_ref, table_setup, table_teardown },
    { NULL, NULL, NULL, NULL },
};


