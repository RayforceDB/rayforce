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
#include <stdatomic.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void cow_setup(void) {
    ray_heap_init();
}

static void cow_teardown(void) {
    ray_heap_destroy();
}

/* ---- retain/release basic ---------------------------------------------- */

static test_result_t test_retain_release(void) {
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* rc starts at 1 */
    TEST_ASSERT_EQ_U(v->rc, 1);

    /* retain -> rc=2 */
    ray_retain(v);
    TEST_ASSERT_EQ_U(v->rc, 2);

    /* release -> rc=1 */
    ray_release(v);
    TEST_ASSERT_EQ_U(v->rc, 1);

    /* release -> rc=0, block freed (don't access v after this) */
    ray_release(v);

    PASS();
}

/* ---- cow sole owner ---------------------------------------------------- */

static test_result_t test_cow_sole_owner(void) {
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    v->type = -RAY_I64;
    v->i64 = 42;

    /* rc=1, sole owner -> cow returns same pointer */
    ray_t* w = ray_cow(v);
    TEST_ASSERT_EQ_PTR(v, w);
    TEST_ASSERT_EQ_U(w->rc, 1);

    ray_release(w);
    PASS();
}

/* ---- cow shared -------------------------------------------------------- */

static test_result_t test_cow_shared(void) {
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    v->type = -RAY_I64;
    v->i64 = 99;

    /* retain to rc=2 (shared) */
    ray_retain(v);
    TEST_ASSERT_EQ_U(v->rc, 2);

    /* cow on shared object -> returns different pointer */
    ray_t* w = ray_cow(v);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w));
    TEST_ASSERT_TRUE((void*)w != (void*)v);

    /* Copy should have rc=1 */
    TEST_ASSERT_EQ_U(w->rc, 1);

    /* Original should have rc=1 (cow decremented from 2 to 1) */
    TEST_ASSERT_EQ_U(v->rc, 1);

    /* Value should be preserved */
    TEST_ASSERT_EQ_I(w->type, -RAY_I64);
    TEST_ASSERT_EQ_I(w->i64, 99);

    ray_release(v);
    ray_release(w);
    PASS();
}

/* ---- null/error safety ------------------------------------------------- */

static test_result_t test_null_error_safety(void) {
    /* These should not crash */
    ray_retain(NULL);
    ray_release(NULL);
    ray_t* r = ray_cow(NULL);
    TEST_ASSERT_NULL(r);

    /* Error objects (new model: ray_error returns a real ray_t with type RAY_ERROR) */
    ray_t* err = ray_error("oom", NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    ray_retain(err);
    ray_release(err);
    ray_t* r2 = ray_cow(err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    ray_release(err);

    PASS();
}

/* ---- cow with vector data ---------------------------------------------- */

static test_result_t test_cow_vector(void) {
    /* Create a vector with actual data */
    size_t data_size = 10 * sizeof(int64_t);
    ray_t* v = ray_alloc(data_size);
    TEST_ASSERT_NOT_NULL(v);
    v->type = RAY_I64;
    v->len = 10;
    int64_t* data = (int64_t*)ray_data(v);
    for (int i = 0; i < 10; i++) {
        data[i] = (int64_t)(i * 100);
    }

    /* Share and cow */
    ray_retain(v);
    ray_t* w = ray_cow(v);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE((void*)w != (void*)v);
    TEST_ASSERT_EQ_I(w->type, RAY_I64);
    TEST_ASSERT_EQ_I(w->len, 10);

    /* Verify data was copied */
    int64_t* wdata = (int64_t*)ray_data(w);
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQ_I(wdata[i], (int64_t)(i * 100));
    }

    /* Modifying copy should not affect original */
    wdata[0] = 999;
    TEST_ASSERT_EQ_I(data[0], 0);

    ray_release(v);
    ray_release(w);
    PASS();
}

/* ---- block_copy retains children --------------------------------------- */

extern ray_t* ray_block_copy(ray_t* src);

static test_result_t test_block_copy_retains_children(void) {
    (void)ray_sym_init();

    int64_t vals[] = {1, 2, 3};
    ray_t* vec = ray_vec_from_raw(RAY_I64, vals, 3);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* Tables now hold a 2-pointer block [schema, cols_list]; retaining the
     * table retains both slots.  Probe the cols list's rc rather than an
     * individual column's, since cols inside the list are owned by the list
     * (the table block doesn't reach through to bump them on copy). */
    ray_t** slots = (ray_t**)ray_data(tbl);
    ray_t* cols_list = slots[1];
    uint32_t rc_before = cols_list->rc;

    /* Copy the table block */
    ray_t* copy = ray_block_copy(tbl);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_FALSE(RAY_IS_ERR(copy));

    /* cols-list ref count should have increased by 1 */
    uint32_t rc_after = cols_list->rc;
    TEST_ASSERT_EQ_U(rc_after, rc_before + 1);

    ray_release(copy);
    ray_release(tbl);
    ray_sym_destroy();

    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t cow_entries[] = {
    { "cow/retain_release", test_retain_release, cow_setup, cow_teardown },
    { "cow/cow_sole_owner", test_cow_sole_owner, cow_setup, cow_teardown },
    { "cow/cow_shared", test_cow_shared, cow_setup, cow_teardown },
    { "cow/null_error_safety", test_null_error_safety, cow_setup, cow_teardown },
    { "cow/cow_vector", test_cow_vector, cow_setup, cow_teardown },
    { "cow/block_copy_retains", test_block_copy_retains_children, cow_setup, cow_teardown },
    { NULL, NULL, NULL, NULL },
};


