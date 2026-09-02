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
#include "table/table.h"   /* ray_parted_nrows */
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

/* ---- table_new error / boundary paths ----------------------------------- */

/* Negative ncols hits the `if (ncols < 0)` range branch (table.c:68). */
static test_result_t test_table_new_negative_ncols(void) {
    ray_t* tbl = ray_table_new(-1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(tbl));
    ray_error_free(tbl);

    tbl = ray_table_new(-1000);
    TEST_ASSERT_TRUE(RAY_IS_ERR(tbl));
    ray_error_free(tbl);
    PASS();
}

/* ncols == 0 takes the non-negative branch and yields an empty schema. */
static test_result_t test_table_new_zero_ncols(void) {
    ray_t* tbl = ray_table_new(0);
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT_EQ_I(ray_table_ncols(tbl), 0);
    TEST_ASSERT_EQ_I(ray_table_nrows(tbl), 0);
    ray_release(tbl);
    PASS();
}

/* Wide table: many single-element columns exercise repeated append/grow. */
static test_result_t test_table_wide(void) {
    ray_t* tbl = ray_table_new(0); /* zero cap → every add_col grows */
    TEST_ASSERT_NOT_NULL(tbl);

    enum { N = 16 };
    ray_t* cols[N] = {0};
    int64_t ids[N] = {0};
    for (int i = 0; i < N; i++) {
        int64_t v = (int64_t)(i * 7 + 1);
        cols[i] = ray_vec_from_raw(RAY_I64, &v, 1);
        TEST_ASSERT_NOT_NULL(cols[i]);
        char name[16];
        int n = snprintf(name, sizeof(name), "w%d", i);
        ids[i] = ray_sym_intern(name, (size_t)n);
        tbl = ray_table_add_col(tbl, ids[i], cols[i]);
        TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    }

    TEST_ASSERT_EQ_I(ray_table_ncols(tbl), N);
    TEST_ASSERT_EQ_I(ray_table_nrows(tbl), 1);
    for (int i = 0; i < N; i++) {
        TEST_ASSERT_EQ_PTR(ray_table_get_col(tbl, ids[i]), cols[i]);
        TEST_ASSERT_EQ_PTR(ray_table_get_col_idx(tbl, i), cols[i]);
        TEST_ASSERT_EQ_I(ray_table_col_name(tbl, i), ids[i]);
        ray_release(cols[i]);
    }
    ray_release(tbl);
    PASS();
}

/* ---- table_add_col error paths ------------------------------------------ */

/* NULL/error table is returned as-is (table.c:104). */
static test_result_t test_table_add_col_null_tbl(void) {
    int64_t v = 1;
    ray_t* col = ray_vec_from_raw(RAY_I64, &v, 1);
    int64_t id = ray_sym_intern("x", 1);

    ray_t* r = ray_table_add_col(NULL, id, col);
    TEST_ASSERT_NULL(r);

    ray_t* err = ray_error("test", NULL);
    r = ray_table_add_col(err, id, col);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(err);

    ray_release(col);
    PASS();
}

/* NULL/error col_vec yields a domain error and consumes the input table. */
static test_result_t test_table_add_col_bad_colvec(void) {
    ray_t* tbl = ray_table_new(2);

    int64_t id = ray_sym_intern("x", 1);
    ray_t* r = ray_table_add_col(tbl, id, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);

    tbl = ray_table_new(2);
    ray_t* err = ray_error("test", NULL);
    r = ray_table_add_col(tbl, id, err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);
    ray_error_free(err);

    PASS();
}

/* ---- get_col / get_col_idx / col_name NULL & error guards --------------- */

static test_result_t test_table_get_col_null_and_err(void) {
    /* NULL and error tables short-circuit (table.c:134). */
    TEST_ASSERT_NULL(ray_table_get_col(NULL, 0));
    ray_t* err = ray_error("test", NULL);
    TEST_ASSERT_NULL(ray_table_get_col(err, 0));

    /* idx-based variant (table.c:151). */
    TEST_ASSERT_NULL(ray_table_get_col_idx(NULL, 0));
    TEST_ASSERT_NULL(ray_table_get_col_idx(err, 0));

    /* name-by-idx variant (table.c:163). */
    TEST_ASSERT_EQ_I(ray_table_col_name(NULL, 0), -1);
    TEST_ASSERT_EQ_I(ray_table_col_name(err, 0), -1);

    ray_error_free(err);
    PASS();
}

/* col_name negative index hits the `idx < 0` branch (table.c:166). */
static test_result_t test_table_col_name_negative_idx(void) {
    ray_t* tbl = ray_table_new(2);
    int64_t id = ray_sym_intern("only", 4);
    int64_t v = 9;
    ray_t* col = ray_vec_from_raw(RAY_I64, &v, 1);
    tbl = ray_table_add_col(tbl, id, col);

    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, -1), -1);
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, -100), -1);
    /* valid still works */
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 0), id);

    ray_release(col);
    ray_release(tbl);
    PASS();
}

/* get_col_idx negative index and exact out-of-range upper bound. */
static test_result_t test_table_get_col_idx_bounds(void) {
    ray_t* tbl = ray_table_new(2);
    int64_t id = ray_sym_intern("c", 1);
    int64_t v = 5;
    ray_t* col = ray_vec_from_raw(RAY_I64, &v, 1);
    tbl = ray_table_add_col(tbl, id, col);

    TEST_ASSERT_NULL(ray_table_get_col_idx(tbl, -1));
    TEST_ASSERT_NULL(ray_table_get_col_idx(tbl, 1));   /* == ncols */
    TEST_ASSERT_NULL(ray_table_get_col_idx(tbl, 1000));
    TEST_ASSERT_EQ_PTR(ray_table_get_col_idx(tbl, 0), col);

    ray_release(col);
    ray_release(tbl);
    PASS();
}

/* Empty table: get_col misses, get_col_idx out of range, col_name -1. */
static test_result_t test_table_empty_lookups(void) {
    ray_t* tbl = ray_table_new(4);
    int64_t id = ray_sym_intern("nope", 4);
    TEST_ASSERT_NULL(ray_table_get_col(tbl, id));
    TEST_ASSERT_NULL(ray_table_get_col_idx(tbl, 0));
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 0), -1);
    TEST_ASSERT_EQ_I(ray_table_nrows(tbl), 0);
    ray_release(tbl);
    PASS();
}

/* ---- set_col_name -------------------------------------------------------- */

/* Exercises the success path plus the null/err-tbl, idx<0, idx>=len and
 * negative bounds guards of ray_table_set_col_name (table.c:176-184). */
static test_result_t test_table_set_col_name(void) {
    ray_t* tbl = ray_table_new(2);
    int64_t id_a = ray_sym_intern("aa", 2);
    int64_t id_b = ray_sym_intern("bb", 2);
    int64_t v = 1;
    ray_t* col = ray_vec_from_raw(RAY_I64, &v, 1);
    tbl = ray_table_add_col(tbl, id_a, col);
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 0), id_a);

    /* success: rename slot 0 */
    ray_table_set_col_name(tbl, 0, id_b);
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 0), id_b);

    /* out-of-range indices are silent no-ops */
    ray_table_set_col_name(tbl, 5, id_a);
    ray_table_set_col_name(tbl, -1, id_a);
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 0), id_b);

    /* NULL / error table are silent no-ops (no crash) */
    ray_table_set_col_name(NULL, 0, id_a);
    ray_t* err = ray_error("test", NULL);
    ray_table_set_col_name(err, 0, id_a);
    ray_error_free(err);

    ray_release(col);
    ray_release(tbl);
    PASS();
}

/* set_col_name COWs a shared schema rather than mutating it in place. */
static test_result_t test_table_set_col_name_shared(void) {
    ray_t* tbl = ray_table_new(2);
    int64_t id_a = ray_sym_intern("p", 1);
    int64_t id_b = ray_sym_intern("q", 1);
    int64_t v = 1;
    ray_t* col = ray_vec_from_raw(RAY_I64, &v, 1);
    tbl = ray_table_add_col(tbl, id_a, col);

    /* Pin a second ref to the schema so set_col_name must COW it. */
    ray_t* schema = ray_table_schema(tbl);
    ray_retain(schema);
    TEST_ASSERT((schema->rc) >= (2), "schema should be shared");

    ray_table_set_col_name(tbl, 0, id_b);
    TEST_ASSERT_EQ_I(ray_table_col_name(tbl, 0), id_b);
    /* original (pinned) schema is unchanged by the COW */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(schema))[0], id_a);

    ray_release(schema);
    ray_release(col);
    ray_release(tbl);
    PASS();
}

/* ---- ncols / nrows / schema NULL & error guards ------------------------- */

static test_result_t test_table_accessors_null_and_err(void) {
    ray_t* err = ray_error("test", NULL);

    TEST_ASSERT_EQ_I(ray_table_ncols(NULL), 0);
    TEST_ASSERT_EQ_I(ray_table_ncols(err), 0);

    TEST_ASSERT_EQ_I(ray_table_nrows(NULL), 0);
    TEST_ASSERT_EQ_I(ray_table_nrows(err), 0);

    TEST_ASSERT_NULL(ray_table_schema(NULL));
    TEST_ASSERT_NULL(ray_table_schema(err));

    TEST_ASSERT_EQ_I(ray_parted_nrows(NULL), 0);
    TEST_ASSERT_EQ_I(ray_parted_nrows(err), 0);

    ray_error_free(err);
    PASS();
}

/* ray_parted_nrows on a plain (non-parted) vector returns vec->len directly. */
static test_result_t test_parted_nrows_plain_vec(void) {
    int64_t raw[] = {1, 2, 3, 4};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 4);
    TEST_ASSERT_EQ_I(ray_parted_nrows(v), 4);
    ray_release(v);

    ray_t* empty = ray_vec_new(RAY_I64, 0);
    TEST_ASSERT_EQ_I(ray_parted_nrows(empty), 0);
    ray_release(empty);
    PASS();
}

/* nrows where the first column is empty: returns 0 from first_col->len. */
static test_result_t test_table_nrows_empty_col(void) {
    ray_t* tbl = ray_table_new(1);
    int64_t id = ray_sym_intern("e", 1);
    ray_t* col = ray_vec_new(RAY_I64, 0); /* zero-length column */
    tbl = ray_table_add_col(tbl, id, col);
    TEST_ASSERT_EQ_I(ray_table_ncols(tbl), 1);
    TEST_ASSERT_EQ_I(ray_table_nrows(tbl), 0);
    ray_release(col);
    ray_release(tbl);
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
    { "table/new_negative_ncols", test_table_new_negative_ncols, table_setup, table_teardown },
    { "table/new_zero_ncols", test_table_new_zero_ncols, table_setup, table_teardown },
    { "table/wide", test_table_wide, table_setup, table_teardown },
    { "table/add_col_null_tbl", test_table_add_col_null_tbl, table_setup, table_teardown },
    { "table/add_col_bad_colvec", test_table_add_col_bad_colvec, table_setup, table_teardown },
    { "table/get_col_null_and_err", test_table_get_col_null_and_err, table_setup, table_teardown },
    { "table/col_name_negative_idx", test_table_col_name_negative_idx, table_setup, table_teardown },
    { "table/get_col_idx_bounds", test_table_get_col_idx_bounds, table_setup, table_teardown },
    { "table/empty_lookups", test_table_empty_lookups, table_setup, table_teardown },
    { "table/set_col_name", test_table_set_col_name, table_setup, table_teardown },
    { "table/set_col_name_shared", test_table_set_col_name_shared, table_setup, table_teardown },
    { "table/accessors_null_and_err", test_table_accessors_null_and_err, table_setup, table_teardown },
    { "table/parted_nrows_plain_vec", test_parted_nrows_plain_vec, table_setup, table_teardown },
    { "table/nrows_empty_col", test_table_nrows_empty_col, table_setup, table_teardown },
    { NULL, NULL, NULL, NULL },
};

