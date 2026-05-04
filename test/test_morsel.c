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
#include "ops/idxop.h"
#include "store/col.h"
#include "core/morsel.h"
#include <string.h>
#include <unistd.h>

#define TMP_MORSEL_COL "/tmp/rayforce_morsel_test_col.dat"

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

/* ---- ray_morsel_init_range -------------------------------------------- */

/* Field setup: range [start, end) over a 100-elem I64 vector.  m->len in
 * the morsel struct holds the END index (not length) — ray_morsel_next
 * compares offset >= len directly. */
static test_result_t test_morsel_init_range_basic(void) {
    int64_t raw[100];
    for (int i = 0; i < 100; i++) raw[i] = (int64_t)i;
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 100);
    TEST_ASSERT_NOT_NULL(v);

    ray_morsel_t m;
    ray_morsel_init_range(&m, v, 20, 80);

    TEST_ASSERT_EQ_PTR(m.vec, v);
    TEST_ASSERT_EQ_I(m.offset, 20);
    TEST_ASSERT_EQ_I(m.len, 80);
    TEST_ASSERT_EQ_U(m.elem_size, 8);
    TEST_ASSERT_EQ_I(m.morsel_len, 0);
    TEST_ASSERT_NULL(m.morsel_ptr);
    TEST_ASSERT_NULL(m.null_bits);

    ray_release(v);
    PASS();
}

/* Iterate a sub-range — should yield exactly the elements in [20, 80). */
static test_result_t test_morsel_init_range_iterate(void) {
    int64_t raw[100];
    for (int i = 0; i < 100; i++) raw[i] = (int64_t)(i * 7);
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 100);

    ray_morsel_t m;
    ray_morsel_init_range(&m, v, 20, 80);

    int64_t total = 0;
    while (ray_morsel_next(&m)) {
        int64_t* data = (int64_t*)m.morsel_ptr;
        for (int64_t i = 0; i < m.morsel_len; i++) {
            int64_t global = m.offset + i;
            TEST_ASSERT_EQ_I(data[i], global * 7);
            total++;
        }
    }
    TEST_ASSERT_EQ_I(total, 60);

    ray_release(v);
    PASS();
}

/* Empty range (start == end) — ray_morsel_next must return false on the
 * first call without dereferencing morsel_ptr. */
static test_result_t test_morsel_init_range_empty(void) {
    int64_t raw[10];
    for (int i = 0; i < 10; i++) raw[i] = (int64_t)i;
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 10);

    ray_morsel_t m;
    ray_morsel_init_range(&m, v, 5, 5);
    TEST_ASSERT_FALSE(ray_morsel_next(&m));

    ray_release(v);
    PASS();
}

/* Multi-morsel range: [500, 2700) over a 3000-element vec — produces
 * 1024 + 1024 + 152 morsels.  Validates offset accounting across morsel
 * boundaries within a sub-range. */
static test_result_t test_morsel_init_range_multi(void) {
    ray_t* v = ray_vec_new(RAY_I64, 3000);
    int64_t* raw = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < 3000; i++) raw[i] = i;

    ray_morsel_t m;
    ray_morsel_init_range(&m, v, 500, 2700);

    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_EQ_I(m.offset, 500);
    TEST_ASSERT_EQ_I(m.morsel_len, 1024);
    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_EQ_I(m.offset, 1524);
    TEST_ASSERT_EQ_I(m.morsel_len, 1024);
    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_EQ_I(m.offset, 2548);
    TEST_ASSERT_EQ_I(m.morsel_len, 152);
    TEST_ASSERT_FALSE(ray_morsel_next(&m));

    ray_release(v);
    PASS();
}

/* Inline-nullmap path in ray_morsel_next: vec with HAS_NULLS, offset<128,
 * no NULLMAP_EXT.  Drives line 96-100 (the inline-bitmap branch). */
static test_result_t test_morsel_nulls_inline(void) {
    int64_t raw[32];
    for (int i = 0; i < 32; i++) raw[i] = (int64_t)i;
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 32);
    ray_vec_set_null(v, 5,  true);
    ray_vec_set_null(v, 17, true);

    ray_morsel_t m;
    ray_morsel_init(&m, v);
    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_NOT_NULL(m.null_bits);

    ray_release(v);
    PASS();
}

/* External-nullmap path: vec with >128 elements + HAS_NULLS forces
 * RAY_ATTR_NULLMAP_EXT, exercising line 92-95 of morsel.c. */
static test_result_t test_morsel_nulls_external(void) {
    ray_t* v = ray_vec_new(RAY_I64, 200);
    int64_t* raw = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < 200; i++) raw[i] = i;
    ray_vec_set_null(v, 10,  true);
    ray_vec_set_null(v, 150, true);

    ray_morsel_t m;
    ray_morsel_init(&m, v);
    while (ray_morsel_next(&m)) {
        TEST_ASSERT_NOT_NULL(m.null_bits);
    }

    ray_release(v);
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

/* ─── HAS_INDEX + mmap-advise paths ────────────────────────── */

static test_result_t test_morsel_mmap_advise(void) {
    int64_t raw[8];
    for (int i = 0; i < 8; i++) raw[i] = (int64_t)(i + 1);
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 8);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, TMP_MORSEL_COL);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    ray_release(vec);

    /* Load via mmap -> mmod == 1 */
    ray_t* mapped = ray_col_mmap(TMP_MORSEL_COL);
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_U(mapped->mmod, 1);

    /* ray_morsel_init must hit the vec->mmod==1 branch (lines 49-51) */
    ray_morsel_t m;
    ray_morsel_init(&m, mapped);
    TEST_ASSERT_EQ_PTR(m.vec, mapped);
    TEST_ASSERT_EQ_I(m.len, 8);

    /* Consume all elements */
    int64_t count = 0;
    while (ray_morsel_next(&m)) {
        int64_t* data = (int64_t*)m.morsel_ptr;
        for (int64_t i = 0; i < m.morsel_len; i++) {
            TEST_ASSERT_EQ_I(data[i], m.offset + i + 1);
            count++;
        }
    }
    TEST_ASSERT_EQ_I(count, 8);

    ray_release(mapped);
    unlink(TMP_MORSEL_COL);
    PASS();
}

static test_result_t test_morsel_has_index_inline_nulls(void) {
    int64_t xs[] = {10, 20, 30, 40, 50};
    ray_t* v = ray_vec_from_raw(RAY_I64, xs, 5);
    TEST_ASSERT_NOT_NULL(v);

    /* Set null at index 1 -> inline bitmap */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);

    /* Attach index — displaces nullmap, stores snapshot in ix->saved_nullmap */
    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);

    ray_morsel_t m;
    ray_morsel_init(&m, w);

    /* ray_morsel_next must hit the HAS_INDEX + inline path (lines 84,89-90) */
    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_EQ_I(m.morsel_len, 5);
    TEST_ASSERT_NOT_NULL(m.null_bits);

    /* Bit 1 should be set */
    int bit1 = (m.null_bits[1 / 8] >> (1 % 8)) & 1;
    TEST_ASSERT_EQ_I(bit1, 1);
    /* Bit 0 should be clear */
    int bit0 = (m.null_bits[0 / 8] >> (0 % 8)) & 1;
    TEST_ASSERT_EQ_I(bit0, 0);

    TEST_ASSERT_FALSE(ray_morsel_next(&m));

    ray_release(w);
    PASS();
}

static test_result_t test_morsel_has_index_ext_nulls(void) {
    /* > 128 elements forces external nullmap */
    int64_t n = 200;
    ray_t* v = ray_vec_new(RAY_I64, 0);
    TEST_ASSERT_NOT_NULL(v);
    int64_t z = 0;
    for (int64_t i = 0; i < n; i++) {
        v = ray_vec_append(v, &z);
        TEST_ASSERT_NOT_NULL(v);
    }
    TEST_ASSERT_EQ_I(v->len, n);

    /* null at 150 -> forces NULLMAP_EXT */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 150, true), RAY_OK);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_NULLMAP_EXT);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);
    /* NULLMAP_EXT cleared in parent; stored in ix->saved_attrs */
    TEST_ASSERT_FALSE(w->attrs & RAY_ATTR_NULLMAP_EXT);

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_TRUE(ix->saved_attrs & RAY_ATTR_NULLMAP_EXT);

    ray_morsel_t m;
    ray_morsel_init(&m, w);

    /* First morsel: hits HAS_INDEX + saved_attrs NULLMAP_EXT (lines 85-88) */
    TEST_ASSERT_TRUE(ray_morsel_next(&m));
    TEST_ASSERT_NOT_NULL(m.null_bits);

    /* Bit 150 should be set */
    int bit150 = (m.null_bits[150 / 8] >> (150 % 8)) & 1;
    TEST_ASSERT_EQ_I(bit150, 1);

    TEST_ASSERT_FALSE(ray_morsel_next(&m));

    ray_release(w);
    PASS();
}

const test_entry_t morsel_entries[] = {
    { "morsel/init", test_morsel_init, morsel_setup, morsel_teardown },
    { "morsel/single", test_morsel_single, morsel_setup, morsel_teardown },
    { "morsel/exact", test_morsel_exact, morsel_setup, morsel_teardown },
    { "morsel/multiple", test_morsel_multiple, morsel_setup, morsel_teardown },
    { "morsel/empty", test_morsel_empty, morsel_setup, morsel_teardown },
    { "morsel/data_access", test_morsel_data_access, morsel_setup, morsel_teardown },
    { "morsel/f64", test_morsel_f64, morsel_setup, morsel_teardown },
    { "morsel/bool", test_morsel_bool, morsel_setup, morsel_teardown },
    { "morsel/init_range_basic",   test_morsel_init_range_basic,   morsel_setup, morsel_teardown },
    { "morsel/init_range_iterate", test_morsel_init_range_iterate, morsel_setup, morsel_teardown },
    { "morsel/init_range_empty",   test_morsel_init_range_empty,   morsel_setup, morsel_teardown },
    { "morsel/init_range_multi",   test_morsel_init_range_multi,   morsel_setup, morsel_teardown },
    { "morsel/nulls_inline",       test_morsel_nulls_inline,       morsel_setup, morsel_teardown },
    { "morsel/nulls_external",     test_morsel_nulls_external,     morsel_setup, morsel_teardown },
    { "morsel/mmap_advise",        test_morsel_mmap_advise,        morsel_setup, morsel_teardown },
    { "morsel/has_index_inline_nulls", test_morsel_has_index_inline_nulls, morsel_setup, morsel_teardown },
    { "morsel/has_index_ext_nulls", test_morsel_has_index_ext_nulls, morsel_setup, morsel_teardown },
    { NULL, NULL, NULL, NULL },
};


