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
#include "ops/fvec.h"

static test_result_t test_ftable_new_free(void) {
    ray_heap_init();

    ray_ftable_t* ft = ray_ftable_new(3);
    TEST_ASSERT_NOT_NULL(ft);
    TEST_ASSERT_EQ_U(ft->n_cols, 3);

    ray_ftable_free(ft);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_ftable_materialize_flat(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_ftable_t* ft = ray_ftable_new(1);

    /* Create a flat fvec: single value at index 0, cardinality 5 */
    int64_t vals[] = {42};
    ray_t* vec = ray_vec_from_raw(RAY_I64, vals, 1);
    ft->columns[0].vec = vec;
    ft->columns[0].cur_idx = 0;
    ft->columns[0].cardinality = 5;
    ft->n_tuples = 5;

    ray_t* result = ray_ftable_materialize(ft);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 5);

    /* All rows should be 42 */
    ray_t* col = ray_table_get_col_idx(result, 0);
    int64_t* data_ptr = (int64_t*)ray_data(col);
    for (int i = 0; i < 5; i++)
        TEST_ASSERT_EQ_I(data_ptr[i], 42);

    ray_release(result);
    ray_ftable_free(ft);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_ftable_materialize_unflat(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_ftable_t* ft = ray_ftable_new(1);

    int64_t vals[] = {10, 20, 30};
    ray_t* vec = ray_vec_from_raw(RAY_I64, vals, 3);
    ft->columns[0].vec = vec;
    ft->columns[0].cur_idx = -1;  /* unflat */
    ft->columns[0].cardinality = 3;
    ft->n_tuples = 3;

    ray_t* result = ray_ftable_materialize(ft);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    ray_t* col = ray_table_get_col_idx(result, 0);
    int64_t* data_ptr = (int64_t*)ray_data(col);
    TEST_ASSERT_EQ_I(data_ptr[0], 10);
    TEST_ASSERT_EQ_I(data_ptr[1], 20);
    TEST_ASSERT_EQ_I(data_ptr[2], 30);

    ray_release(result);
    ray_ftable_free(ft);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t fvec_entries[] = {
    { "fvec/new_free", test_ftable_new_free, NULL, NULL },
    { "fvec/materialize_flat", test_ftable_materialize_flat, NULL, NULL },
    { "fvec/materialize_unflat", test_ftable_materialize_unflat, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


