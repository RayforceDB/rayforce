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
#include "store/meta.h"
#include <string.h>
#include <unistd.h>

#define TMP_META_PATH "/tmp/rayforce_test_meta.d"

/* ---- Setup / Teardown -------------------------------------------------- */

static void meta_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void meta_teardown(void) {
    unlink(TMP_META_PATH);
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- test_meta_save_load_roundtrip ------------------------------------- */

static test_result_t test_meta_save_load_roundtrip(void) {
    /* Build a small I64 schema vector */
    int64_t ids[] = {100, 200, 300};
    ray_t* schema = ray_vec_from_raw(RAY_I64, ids, 3);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_FALSE(RAY_IS_ERR(schema));

    /* Save */
    ray_err_t err = ray_meta_save_d(schema, TMP_META_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load back */
    ray_t* loaded = ray_meta_load_d(TMP_META_PATH);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    /* Verify contents */
    TEST_ASSERT_EQ_I(ray_type(loaded), RAY_I64);
    TEST_ASSERT_EQ_I(ray_len(loaded), 3);

    int64_t* out = (int64_t*)ray_data(loaded);
    TEST_ASSERT_EQ_I(out[0], 100);
    TEST_ASSERT_EQ_I(out[1], 200);
    TEST_ASSERT_EQ_I(out[2], 300);

    ray_free(schema);
    ray_free(loaded);
    PASS();
}

/* ---- test_meta_save_null_returns_error --------------------------------- */

static test_result_t test_meta_save_null_returns_error(void) {
    ray_err_t err = ray_meta_save_d(NULL, TMP_META_PATH);
    TEST_ASSERT((err) != (RAY_OK), "err != RAY_OK");

    PASS();
}

/* ---- test_meta_save_err_ptr_returns_error ------------------------------ */

static test_result_t test_meta_save_err_ptr_returns_error(void) {
    ray_t* bad = ray_error("type", NULL);
    ray_err_t err = ray_meta_save_d(bad, TMP_META_PATH);
    TEST_ASSERT((err) != (RAY_OK), "err != RAY_OK");
    ray_release(bad);

    PASS();
}

/* ---- test_meta_load_nonexistent --------------------------------------- */

static test_result_t test_meta_load_nonexistent(void) {
    ray_t* loaded = ray_meta_load_d("/tmp/rayforce_no_such_file_meta.d");
    /* Should return NULL or error pointer for missing file */
    TEST_ASSERT_TRUE(loaded == NULL || RAY_IS_ERR(loaded));

    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t meta_entries[] = {
    { "meta/save_load_roundtrip", test_meta_save_load_roundtrip, meta_setup, meta_teardown },
    { "meta/save_null_returns_error", test_meta_save_null_returns_error, meta_setup, meta_teardown },
    { "meta/save_err_ptr_returns_error", test_meta_save_err_ptr_returns_error, meta_setup, meta_teardown },
    { "meta/load_nonexistent", test_meta_load_nonexistent, meta_setup, meta_teardown },
    { NULL, NULL, NULL, NULL },
};


