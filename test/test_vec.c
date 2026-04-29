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
#include <string.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void vec_setup(void) {
    ray_heap_init();
}

static void vec_teardown(void) {
    ray_heap_destroy();
}

/* ---- vec_new ----------------------------------------------------------- */

static test_result_t test_vec_new(void) {
    ray_t* v = ray_vec_new(RAY_I64, 10);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_TRUE(ray_is_vec(v));
    TEST_ASSERT_EQ_I(v->type, RAY_I64);
    TEST_ASSERT_EQ_I(v->len, 0);
    ray_release(v);

    PASS();
}

/* ---- vec_new invalid type ---------------------------------------------- */

static test_result_t test_vec_new_invalid(void) {
    ray_t* v = ray_vec_new(-1, 10);
    TEST_ASSERT_TRUE(RAY_IS_ERR(v));
    TEST_ASSERT_STR_EQ(ray_err_code(v), "type");
    ray_release(v);

    PASS();
}

/* ---- vec_append -------------------------------------------------------- */

static test_result_t test_vec_append(void) {
    ray_t* v = ray_vec_new(RAY_I64, 4);
    TEST_ASSERT_NOT_NULL(v);

    int64_t vals[] = {10, 20, 30};
    for (int i = 0; i < 3; i++) {
        v = ray_vec_append(v, &vals[i]);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }

    TEST_ASSERT_EQ_I(v->len, 3);

    int64_t* data = (int64_t*)ray_data(v);
    TEST_ASSERT_EQ_I(data[0], 10);
    TEST_ASSERT_EQ_I(data[1], 20);
    TEST_ASSERT_EQ_I(data[2], 30);

    ray_release(v);
    PASS();
}

/* ---- vec_get ----------------------------------------------------------- */

static test_result_t test_vec_get(void) {
    int64_t raw[] = {100, 200, 300, 400};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 4);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    int64_t* p0 = (int64_t*)ray_vec_get(v, 0);
    TEST_ASSERT_EQ_I(*p0, 100);

    int64_t* p3 = (int64_t*)ray_vec_get(v, 3);
    TEST_ASSERT_EQ_I(*p3, 400);

    /* Out of range */
    void* oob = ray_vec_get(v, 4);
    TEST_ASSERT_NULL(oob);
    oob = ray_vec_get(v, -1);
    TEST_ASSERT_NULL(oob);

    ray_release(v);
    PASS();
}

/* ---- vec_set ----------------------------------------------------------- */

static test_result_t test_vec_set(void) {
    int64_t raw[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_NOT_NULL(v);

    int64_t new_val = 99;
    v = ray_vec_set(v, 1, &new_val);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    int64_t* p1 = (int64_t*)ray_vec_get(v, 1);
    TEST_ASSERT_EQ_I(*p1, 99);

    /* Out of range returns error */
    ray_t* err = ray_vec_set(v, 10, &new_val);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));

    ray_release(v);
    PASS();
}

/* ---- vec_from_raw ------------------------------------------------------ */

static test_result_t test_vec_from_raw(void) {
    double raw[] = {1.1, 2.2, 3.3, 4.4, 5.5};
    ray_t* v = ray_vec_from_raw(RAY_F64, raw, 5);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->type, RAY_F64);
    TEST_ASSERT_EQ_I(v->len, 5);

    double* data = (double*)ray_data(v);
    TEST_ASSERT((data[0]) == (1.1), "double == failed");
    TEST_ASSERT((data[4]) == (5.5), "double == failed");

    ray_release(v);
    PASS();
}

/* ---- vec_slice_basic --------------------------------------------------- */

static test_result_t test_vec_slice_basic(void) {
    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);
    TEST_ASSERT_NOT_NULL(v);

    ray_t* s = ray_vec_slice(v, 1, 3);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_EQ_I(s->type, RAY_I64);
    TEST_ASSERT_EQ_I(s->len, 3);
    TEST_ASSERT((s->attrs & RAY_ATTR_SLICE) != (0), "s->attrs & RAY_ATTR_SLICE != 0");

    /* Clean up: release slice first (drops parent ref), then parent */
    ray_release(s);
    ray_release(v);
    PASS();
}

/* ---- vec_slice_access -------------------------------------------------- */

static test_result_t test_vec_slice_access(void) {
    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);

    ray_t* s = ray_vec_slice(v, 2, 2);  /* [30, 40] */
    TEST_ASSERT_NOT_NULL(s);

    int64_t* p0 = (int64_t*)ray_vec_get(s, 0);
    TEST_ASSERT_EQ_I(*p0, 30);

    int64_t* p1 = (int64_t*)ray_vec_get(s, 1);
    TEST_ASSERT_EQ_I(*p1, 40);

    /* Out of range on slice */
    void* oob = ray_vec_get(s, 2);
    TEST_ASSERT_NULL(oob);

    ray_release(s);
    ray_release(v);
    PASS();
}

/* ---- vec_concat -------------------------------------------------------- */

static test_result_t test_vec_concat(void) {
    int64_t a_raw[] = {1, 2, 3};
    int64_t b_raw[] = {4, 5};
    ray_t* a = ray_vec_from_raw(RAY_I64, a_raw, 3);
    ray_t* b = ray_vec_from_raw(RAY_I64, b_raw, 2);

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->len, 5);

    int64_t* data = (int64_t*)ray_data(c);
    TEST_ASSERT_EQ_I(data[0], 1);
    TEST_ASSERT_EQ_I(data[2], 3);
    TEST_ASSERT_EQ_I(data[3], 4);
    TEST_ASSERT_EQ_I(data[4], 5);

    ray_release(a);
    ray_release(b);
    ray_release(c);
    PASS();
}

/* ---- null_inline (<=128 elements) -------------------------------------- */

static test_result_t test_vec_null_inline(void) {
    ray_t* v = ray_vec_new(RAY_I64, 10);
    /* Manually set len for null testing */
    int64_t vals[10];
    for (int i = 0; i < 10; i++) {
        vals[i] = (int64_t)(i * 10);
        v = ray_vec_append(v, &vals[i]);
    }
    TEST_ASSERT_EQ_I(v->len, 10);

    /* Initially no nulls */
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 5));

    /* Set some nulls */
    ray_vec_set_null(v, 3, true);
    ray_vec_set_null(v, 7, true);

    TEST_ASSERT_TRUE(ray_vec_is_null(v, 3));
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 7));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 4));

    /* Clear a null */
    ray_vec_set_null(v, 3, false);
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 3));

    ray_release(v);
    PASS();
}

/* ---- null_external (>128 elements) ------------------------------------- */

static test_result_t test_vec_null_external(void) {
    ray_t* v = ray_vec_new(RAY_U8, 200);

    /* Append 200 elements */
    for (int i = 0; i < 200; i++) {
        uint8_t val = (uint8_t)(i & 0xFF);
        v = ray_vec_append(v, &val);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }
    TEST_ASSERT_EQ_I(v->len, 200);

    /* Set null at index 150 (forces external nullmap) */
    ray_vec_set_null(v, 150, true);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_NULLMAP_EXT);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 150));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 149));

    /* External nullmap is owned by the vector and released with it. */
    ray_release(v);
    PASS();
}

/* ---- slice_release_parent_ref ------------------------------------------- */

static test_result_t test_vec_slice_release_parent_ref(void) {
    int64_t raw[] = {10, 20, 30};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_NOT_NULL(v);

    ray_retain(v); /* guard ref for observing parent rc after slice release */
    TEST_ASSERT_EQ_U(v->rc, 2);

    ray_t* s = ray_vec_slice(v, 1, 2);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_EQ_U(v->rc, 3);

    ray_release(s);
    TEST_ASSERT_EQ_U(v->rc, 2);

    ray_release(v);
    ray_release(v);
    PASS();
}

/* ---- null_external_release_ext_ref -------------------------------------- */

static test_result_t test_vec_null_external_release_ext_ref(void) {
    ray_t* v = ray_vec_new(RAY_U8, 200);
    TEST_ASSERT_NOT_NULL(v);

    for (int i = 0; i < 200; i++) {
        uint8_t val = (uint8_t)(i & 0xFF);
        v = ray_vec_append(v, &val);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }

    ray_vec_set_null(v, 150, true);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_NULLMAP_EXT);
    ray_t* ext = v->ext_nullmap;
    TEST_ASSERT_NOT_NULL(ext);

    ray_retain(ext); /* guard ref */
    TEST_ASSERT_EQ_U(ext->rc, 2);

    ray_release(v);
    TEST_ASSERT_EQ_U(ext->rc, 1);

    ray_release(ext);
    PASS();
}

/* ---- append_grow (test reallocation) ----------------------------------- */

static test_result_t test_vec_append_grow(void) {
    ray_t* v = ray_vec_new(RAY_I32, 1);  /* Start with tiny capacity */

    /* Append many elements to force multiple reallocs */
    for (int i = 0; i < 100; i++) {
        int32_t val = (int32_t)(i * 3);
        v = ray_vec_append(v, &val);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }

    TEST_ASSERT_EQ_I(v->len, 100);

    /* Verify all values are correct after reallocs */
    int32_t* data = (int32_t*)ray_data(v);
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQ_I(data[i], (int32_t)(i * 3));
    }

    ray_release(v);
    PASS();
}

/* ---- type_correctness -------------------------------------------------- */

static test_result_t test_vec_type_correctness(void) {
    /* Test different vector types */
    ray_t* v_bool = ray_vec_new(RAY_BOOL, 4);
    TEST_ASSERT_EQ_I(v_bool->type, RAY_BOOL);
    TEST_ASSERT_TRUE(ray_is_vec(v_bool));
    TEST_ASSERT_FALSE(ray_is_atom(v_bool));
    ray_release(v_bool);

    ray_t* v_f64 = ray_vec_new(RAY_F64, 4);
    TEST_ASSERT_EQ_I(v_f64->type, RAY_F64);
    ray_release(v_f64);

    ray_t* v_u8 = ray_vec_new(RAY_U8, 4);
    TEST_ASSERT_EQ_I(v_u8->type, RAY_U8);
    ray_release(v_u8);

    PASS();
}

/* ---- empty_vec --------------------------------------------------------- */

static test_result_t test_vec_empty(void) {
    ray_t* v = ray_vec_new(RAY_I64, 0);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 0);

    /* Get on empty returns NULL */
    void* p = ray_vec_get(v, 0);
    TEST_ASSERT_NULL(p);

    ray_release(v);
    PASS();
}

/* ---- vec_bool ---------------------------------------------------------- */

static test_result_t test_vec_bool(void) {
    ray_t* v = ray_vec_new(RAY_BOOL, 4);
    TEST_ASSERT_NOT_NULL(v);

    uint8_t vals[] = {1, 0, 1, 0};
    for (int i = 0; i < 4; i++) {
        v = ray_vec_append(v, &vals[i]);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }

    TEST_ASSERT_EQ_I(v->len, 4);

    uint8_t* data = (uint8_t*)ray_data(v);
    TEST_ASSERT_EQ_U(data[0], 1);
    TEST_ASSERT_EQ_U(data[1], 0);
    TEST_ASSERT_EQ_U(data[2], 1);
    TEST_ASSERT_EQ_U(data[3], 0);

    ray_release(v);
    PASS();
}

/* ---- vec_concat_type_mismatch ------------------------------------------ */

static test_result_t test_vec_concat_type_mismatch(void) {
    int64_t a_raw[] = {1};
    double b_raw[] = {2.0};
    ray_t* a = ray_vec_from_raw(RAY_I64, a_raw, 1);
    ray_t* b = ray_vec_from_raw(RAY_F64, b_raw, 1);

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_TRUE(RAY_IS_ERR(c));
    TEST_ASSERT_STR_EQ(ray_err_code(c), "type");
    ray_release(c);

    ray_release(a);
    ray_release(b);
    PASS();
}

/* ---- vec_slice_out_of_range -------------------------------------------- */

static test_result_t test_vec_slice_range(void) {
    int64_t raw[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);

    ray_t* s = ray_vec_slice(v, 2, 2);  /* offset+len > vec->len */
    TEST_ASSERT_TRUE(RAY_IS_ERR(s));

    ray_release(v);
    PASS();
}

/* ---- vec_slice_null ---------------------------------------------------- */

static test_result_t test_vec_slice_null(void) {
    int64_t vals[] = {10, 20, 30, 40};
    ray_t* v = ray_vec_from_raw(RAY_I64, vals, 4);
    ray_vec_set_null(v, 1, true);
    ray_vec_set_null(v, 3, true);

    ray_t* s = ray_vec_slice(v, 1, 2);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));

    /* Slice index 0 = parent index 1 = null */
    TEST_ASSERT_TRUE(ray_vec_is_null(s, 0));
    /* Slice index 1 = parent index 2 = not null */
    TEST_ASSERT_FALSE(ray_vec_is_null(s, 1));

    ray_release(s);
    ray_release(v);
    PASS();
}

/* ---- vec_concat_null_propagation --------------------------------------- */

static test_result_t test_vec_concat_null(void) {
    int64_t a_raw[] = {1, 2, 3};
    int64_t b_raw[] = {4, 5};
    ray_t* a = ray_vec_from_raw(RAY_I64, a_raw, 3);
    ray_t* b = ray_vec_from_raw(RAY_I64, b_raw, 2);

    ray_vec_set_null(a, 1, true);   /* a[1] = null */
    ray_vec_set_null(b, 0, true);   /* b[0] = null */

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->len, 5);

    TEST_ASSERT_FALSE(ray_vec_is_null(c, 0));  /* a[0]=1 */
    TEST_ASSERT_TRUE(ray_vec_is_null(c, 1));   /* a[1]=null */
    TEST_ASSERT_FALSE(ray_vec_is_null(c, 2));  /* a[2]=3 */
    TEST_ASSERT_TRUE(ray_vec_is_null(c, 3));   /* b[0]=null */
    TEST_ASSERT_FALSE(ray_vec_is_null(c, 4));  /* b[1]=5 */

    ray_release(c);
    ray_release(a);
    ray_release(b);
    PASS();
}

/* ---- vec_concat_slice_null_propagation --------------------------------- */

static test_result_t test_vec_concat_slice_null(void) {
    int64_t raw[] = {10, 20, 30, 40};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 4);
    ray_vec_set_null(v, 1, true);  /* v[1] = null */

    ray_t* s = ray_vec_slice(v, 1, 2);  /* s = [null, 30] */

    int64_t b_raw[] = {50};
    ray_t* b = ray_vec_from_raw(RAY_I64, b_raw, 1);

    ray_t* c = ray_vec_concat(s, b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->len, 3);

    TEST_ASSERT_TRUE(ray_vec_is_null(c, 0));   /* slice[0]=null */
    TEST_ASSERT_FALSE(ray_vec_is_null(c, 1));  /* slice[1]=30 */
    TEST_ASSERT_FALSE(ray_vec_is_null(c, 2));  /* b[0]=50 */

    ray_release(c);
    ray_release(s);
    ray_release(b);
    ray_release(v);
    PASS();
}

/* ---- vec_new_oom_returns_error ----------------------------------------
 *
 * Regression: ray_alloc returns plain NULL on out-of-memory paths (heap
 * pool exhaustion, single-block size > RAY_HEAP_MAX_ORDER).  Before this
 * fix, ray_vec_new propagated the NULL untouched, which let downstream
 * `if (RAY_IS_ERR(v))` checks miss the failure entirely — the user-visible
 * symptom being silent (set k (til 10000000000)) → "k undefined".
 *
 * This test triggers the size > MAX_ORDER path with an absurdly large
 * capacity; the result must be a real error pointer, not NULL. */
static test_result_t test_vec_new_oom_returns_error(void) {
    /* 1e11 elements * 8 bytes = 800 GB > 256 GB pool max — guaranteed to
     * fail in ray_alloc with "order > RAY_HEAP_MAX_ORDER".  This avoids
     * relying on overcommit settings or actual physical memory state. */
    ray_t* v = ray_vec_new(RAY_I64, (int64_t)100000000000LL);
    TEST_ASSERT_NOT_NULL(v);            /* must not be silent NULL */
    TEST_ASSERT_TRUE(RAY_IS_ERR(v));    /* must be a real error */
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t vec_entries[] = {
    { "vec/new", test_vec_new, vec_setup, vec_teardown },
    { "vec/new_invalid", test_vec_new_invalid, vec_setup, vec_teardown },
    { "vec/new_oom_returns_error", test_vec_new_oom_returns_error, vec_setup, vec_teardown },
    { "vec/append", test_vec_append, vec_setup, vec_teardown },
    { "vec/get", test_vec_get, vec_setup, vec_teardown },
    { "vec/set", test_vec_set, vec_setup, vec_teardown },
    { "vec/from_raw", test_vec_from_raw, vec_setup, vec_teardown },
    { "vec/slice_basic", test_vec_slice_basic, vec_setup, vec_teardown },
    { "vec/slice_access", test_vec_slice_access, vec_setup, vec_teardown },
    { "vec/concat", test_vec_concat, vec_setup, vec_teardown },
    { "vec/null_inline", test_vec_null_inline, vec_setup, vec_teardown },
    { "vec/null_external", test_vec_null_external, vec_setup, vec_teardown },
    { "vec/slice_release_parent_ref", test_vec_slice_release_parent_ref, vec_setup, vec_teardown },
    { "vec/null_external_release_ext_ref", test_vec_null_external_release_ext_ref, vec_setup, vec_teardown },
    { "vec/append_grow", test_vec_append_grow, vec_setup, vec_teardown },
    { "vec/type_correctness", test_vec_type_correctness, vec_setup, vec_teardown },
    { "vec/empty", test_vec_empty, vec_setup, vec_teardown },
    { "vec/bool", test_vec_bool, vec_setup, vec_teardown },
    { "vec/concat_type_mismatch", test_vec_concat_type_mismatch, vec_setup, vec_teardown },
    { "vec/slice_range", test_vec_slice_range, vec_setup, vec_teardown },
    { "vec/slice_null", test_vec_slice_null, vec_setup, vec_teardown },
    { "vec/concat_null", test_vec_concat_null, vec_setup, vec_teardown },
    { "vec/concat_slice_null", test_vec_concat_slice_null, vec_setup, vec_teardown },
    { NULL, NULL, NULL, NULL },
};


