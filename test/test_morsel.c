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
#include "ops/ops.h"
#include <string.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void morsel_setup(void) {
    ray_heap_init();
}

static void morsel_teardown(void) {
    ray_heap_destroy();
}

/* ---- morsel_init ------------------------------------------------------- */

static test_result_t test_morsel_init(void) {
    int64_t raw[10];
    for (int i = 0; i < 10; i++) raw[i] = (int64_t)(i * 10);
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 10);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_morsel_t m;
    ray_morsel_init(&m, v);

    TEST_ASSERT_EQ_PTR(m.vec, v);
    TEST_ASSERT_EQ_I(m.offset, 0);
    TEST_ASSERT_EQ_I(m.len, 10);
    TEST_ASSERT_EQ_U(m.elem_size, 8);  /* I64 = 8 bytes */
    TEST_ASSERT_EQ_I(m.morsel_len, 0);
    TEST_ASSERT_NULL(m.morsel_ptr);
    TEST_ASSERT_NULL(m.null_bits);

    ray_release(v);
    PASS();
}

/* ---- morsel_single (< 1024 elements) ----------------------------------- */

static test_result_t test_morsel_single(void) {
    int64_t raw[5];
    for (int i = 0; i < 5; i++) raw[i] = (int64_t)i;
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);

    ray_morsel_t m;
    ray_morsel_init(&m, v);

    /* First morsel: should contain all 5 elements */
    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_EQ_I(m.morsel_len, 5);
    TEST_ASSERT_EQ_I(m.offset, 0);
    TEST_ASSERT_NOT_NULL(m.morsel_ptr);

    /* Second call: should return false (exhausted) */
    TEST_ASSERT_FALSE(ray_morsel_next(&m));

    ray_release(v);
    PASS();
}

/* ---- morsel_exact (exactly 1024 elements) ------------------------------ */

static test_result_t test_morsel_exact(void) {
    int64_t raw[1024];
    for (int i = 0; i < 1024; i++) raw[i] = (int64_t)i;
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 1024);

    ray_morsel_t m;
    ray_morsel_init(&m, v);

    /* First morsel: exactly 1024 elements */
    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_EQ_I(m.morsel_len, 1024);
    TEST_ASSERT_EQ_I(m.offset, 0);

    /* Second call: exhausted */
    TEST_ASSERT_FALSE(ray_morsel_next(&m));

    ray_release(v);
    PASS();
}

/* ---- morsel_multiple (2500 elements = 1024+1024+452) ------------------- */

static test_result_t test_morsel_multiple(void) {
    int64_t raw[2500];
    for (int i = 0; i < 2500; i++) raw[i] = (int64_t)i;
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 2500);

    ray_morsel_t m;
    ray_morsel_init(&m, v);

    /* Morsel 1: 1024 elements */
    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_EQ_I(m.morsel_len, 1024);
    TEST_ASSERT_EQ_I(m.offset, 0);

    /* Morsel 2: 1024 elements */
    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_EQ_I(m.morsel_len, 1024);
    TEST_ASSERT_EQ_I(m.offset, 1024);

    /* Morsel 3: 452 elements */
    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_EQ_I(m.morsel_len, 452);
    TEST_ASSERT_EQ_I(m.offset, 2048);

    /* Exhausted */
    TEST_ASSERT_FALSE(ray_morsel_next(&m));

    ray_release(v);
    PASS();
}

/* ---- morsel_empty (0 elements) ----------------------------------------- */

static test_result_t test_morsel_empty(void) {
    ray_t* v = ray_vec_new(RAY_I64, 0);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_morsel_t m;
    ray_morsel_init(&m, v);

    /* Should return false immediately */
    TEST_ASSERT_FALSE(ray_morsel_next(&m));

    ray_release(v);
    PASS();
}

/* ---- morsel_data_access (verify I64 data through morsel_ptr) ----------- */

static test_result_t test_morsel_data_access(void) {
    int64_t raw[2500];
    for (int i = 0; i < 2500; i++) raw[i] = (int64_t)(i * 3);
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 2500);

    ray_morsel_t m;
    ray_morsel_init(&m, v);

    int64_t total_checked = 0;

    while (ray_morsel_next(&m)) {
        int64_t* data = (int64_t*)m.morsel_ptr;
        for (int64_t i = 0; i < m.morsel_len; i++) {
            int64_t global_idx = m.offset + i;
            TEST_ASSERT_EQ_I(data[i], global_idx * 3);
            total_checked++;
        }
    }

    TEST_ASSERT_EQ_I(total_checked, 2500);

    ray_release(v);
    PASS();
}

/* ---- morsel_f64 (verify F64 data through morsel_ptr) ------------------- */

static test_result_t test_morsel_f64(void) {
    double raw[2000];
    for (int i = 0; i < 2000; i++) raw[i] = (double)i * 1.5;
    ray_t* v = ray_vec_from_raw(RAY_F64, raw, 2000);

    ray_morsel_t m;
    ray_morsel_init(&m, v);
    TEST_ASSERT_EQ_U(m.elem_size, 8);  /* F64 = 8 bytes */

    int64_t total_checked = 0;

    while (ray_morsel_next(&m)) {
        double* data = (double*)m.morsel_ptr;
        for (int64_t i = 0; i < m.morsel_len; i++) {
            int64_t global_idx = m.offset + i;
            TEST_ASSERT((data[i]) == ((double)global_idx * 1.5), "double == failed");
            total_checked++;
        }
    }

    TEST_ASSERT_EQ_I(total_checked, 2000);

    ray_release(v);
    PASS();
}

/* ---- morsel_bool (verify BOOL data through morsel_ptr) ----------------- */

static test_result_t test_morsel_bool(void) {
    uint8_t raw[50];
    for (int i = 0; i < 50; i++) raw[i] = (uint8_t)(i % 2);
    ray_t* v = ray_vec_from_raw(RAY_BOOL, raw, 50);

    ray_morsel_t m;
    ray_morsel_init(&m, v);
    TEST_ASSERT_EQ_U(m.elem_size, 1);  /* BOOL = 1 byte */

    /* Single morsel (50 < 1024) */
    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_EQ_I(m.morsel_len, 50);

    uint8_t* data = (uint8_t*)m.morsel_ptr;
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_EQ_U(data[i], (uint8_t)(i % 2));
    }

    /* Exhausted */
    TEST_ASSERT_FALSE(ray_morsel_next(&m));

    ray_release(v);
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t morsel_entries[] = {
    { "morsel/init", test_morsel_init, morsel_setup, morsel_teardown },
    { "morsel/single", test_morsel_single, morsel_setup, morsel_teardown },
    { "morsel/exact", test_morsel_exact, morsel_setup, morsel_teardown },
    { "morsel/multiple", test_morsel_multiple, morsel_setup, morsel_teardown },
    { "morsel/empty", test_morsel_empty, morsel_setup, morsel_teardown },
    { "morsel/data_access", test_morsel_data_access, morsel_setup, morsel_teardown },
    { "morsel/f64", test_morsel_f64, morsel_setup, morsel_teardown },
    { "morsel/bool", test_morsel_bool, morsel_setup, morsel_teardown },
    { NULL, NULL, NULL, NULL },
};


