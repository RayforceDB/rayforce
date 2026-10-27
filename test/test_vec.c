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
#include "mem/heap.h"
#include "mem/cow.h"
#include "vec/vec.h"
#include "vec/growth.h"
#include "ops/idxop.h"
#include "vec/str.h"
#include "vec/embedding.h"
#include "table/sym.h"
#include "table/domain.h"
#include "lang/internal.h"
#include "core/platform.h"
#include "store/serde.h"
#include <string.h>
#include <math.h>

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

    /* Clear a null.  The caller must restore a real payload value
     * before clearing HAS_NULLS — the stale NULL_I64 sentinel from the
     * prior set-null would otherwise still read back as null. */
    ((int64_t*)ray_data(v))[3] = 30;  /* restore vals[3] = 3 * 10 */
    ray_vec_set_null(v, 3, false);
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 3));

    ray_release(v);
    PASS();
}

/* ---- null_external (>128 elements) ------------------------------------- */

static test_result_t test_vec_null_external(void) {
    /* >128-element nullable vec.  U8 is non-nullable so the test uses
     * I16, whose null state lives as NULL_I16 in the payload. */
    ray_t* v = ray_vec_new(RAY_I16, 200);

    for (int i = 0; i < 200; i++) {
        int16_t val = (int16_t)i;
        v = ray_vec_append(v, &val);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }
    TEST_ASSERT_EQ_I(v->len, 200);

    ray_vec_set_null(v, 150, true);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 150));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 149));

    /* U8 set-null is rejected (U8 is non-nullable). */
    ray_t* u = ray_vec_new(RAY_U8, 4);
    uint8_t z = 0;
    for (int i = 0; i < 4; i++) u = ray_vec_append(u, &z);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(u, 1, true), RAY_ERR_TYPE);
    ray_release(u);

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

/* ---- null_large_release ------------------------------------------------- */

static test_result_t test_vec_null_large_release(void) {
    /* Release-without-leak smoke test on a large nullable vec.  ASAN
     * is the gate. */
    ray_t* v = ray_vec_new(RAY_I16, 200);
    TEST_ASSERT_NOT_NULL(v);

    for (int i = 0; i < 200; i++) {
        int16_t val = (int16_t)i;
        v = ray_vec_append(v, &val);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }

    ray_vec_set_null(v, 150, true);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 150));

    ray_release(v);
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

/* ---- sentinel_is_null: F32 null via NaN -------------------------------- */

static test_result_t test_vec_f32_null_sentinel(void) {
    /* Exercises the RAY_F32 arm of sentinel_is_null (line ~56-59) and
     * ray_vec_set_null_checked's RAY_F32 branch (line ~866). */
    ray_t* v = ray_vec_new(RAY_F32, 4);
    TEST_ASSERT_NOT_NULL(v);
    float vals[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &vals[i]);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 4);

    /* Initially no nulls */
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 2));

    /* Set F32 null — writes NULL_F32 sentinel */
    ray_err_t err = ray_vec_set_null_checked(v, 1, true);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 3));

    /* Set another to null */
    ray_vec_set_null(v, 3, true);
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 3));

    ray_release(v);
    PASS();
}

/* ---- sym_vec_new: invalid width and capacity errors -------------------- */

static test_result_t test_sym_vec_new_errors(void) {
    /* invalid width bits */
    ray_t* bad = ray_sym_vec_new(0xF0, 10);
    TEST_ASSERT_NOT_NULL(bad);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "type");
    ray_release(bad);

    /* negative capacity */
    ray_t* bad2 = ray_sym_vec_new(RAY_SYM_W8, -1);
    TEST_ASSERT_NOT_NULL(bad2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad2));
    TEST_ASSERT_STR_EQ(ray_err_code(bad2), "range");
    ray_release(bad2);

    /* valid W8 sym vec */
    ray_t* w8 = ray_sym_vec_new(RAY_SYM_W8, 8);
    TEST_ASSERT_NOT_NULL(w8);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w8));
    TEST_ASSERT_EQ_I(w8->type, RAY_SYM);
    TEST_ASSERT_EQ_I(w8->attrs & RAY_SYM_W_MASK, RAY_SYM_W8);
    ray_release(w8);

    PASS();
}

/* ---- sym_vec: all width variants (W8, W16, W32) ------------------------ */

static test_result_t test_sym_vec_widths(void) {
    /* W8 */
    ray_t* w8 = ray_sym_vec_new(RAY_SYM_W8, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w8));
    uint8_t id8 = 42;
    w8 = ray_vec_append(w8, &id8);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w8));
    TEST_ASSERT_EQ_I(w8->len, 1);
    uint8_t* d8 = (uint8_t*)ray_data(w8);
    TEST_ASSERT_EQ_I(d8[0], 42);
    /* SYM never null */
    TEST_ASSERT_FALSE(ray_vec_is_null(w8, 0));
    ray_release(w8);

    /* W16 */
    ray_t* w16 = ray_sym_vec_new(RAY_SYM_W16, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w16));
    uint16_t id16 = 1000;
    w16 = ray_vec_append(w16, &id16);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w16));
    uint16_t* d16 = (uint16_t*)ray_data(w16);
    TEST_ASSERT_EQ_I(d16[0], 1000);
    ray_release(w16);

    /* W32 */
    ray_t* w32 = ray_sym_vec_new(RAY_SYM_W32, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w32));
    uint32_t id32 = 99999;
    w32 = ray_vec_append(w32, &id32);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w32));
    uint32_t* d32 = (uint32_t*)ray_data(w32);
    TEST_ASSERT_EQ_I(d32[0], 99999);
    ray_release(w32);

    PASS();
}

/* ---- has_nulls: text types are proven null-free by payload scan -------- */

/* SYM/STR always "may have nulls", so ray_vec_has_nulls must inspect the
 * payload.  Pin the contract at every SYM width, on STR, and through a
 * slice, with lengths that are not multiples of any vector width so the
 * scan's tail handling is exercised: a single id-0 cell anywhere flips
 * the answer. */
static test_result_t test_vec_has_nulls_text(void) {
    static const uint8_t widths[] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64 };
    const int64_t n = 37;
    for (size_t wi = 0; wi < sizeof(widths); wi++) {
        ray_t* v = ray_sym_vec_new(widths[wi], n);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        for (int64_t i = 0; i < n; i++) {
            uint64_t id = (uint64_t)(i + 1);
            uint8_t  b8 = (uint8_t)id; uint16_t b16 = (uint16_t)id;
            uint32_t b32 = (uint32_t)id; int64_t b64 = (int64_t)id;
            const void* src = widths[wi] == RAY_SYM_W8  ? (const void*)&b8  :
                              widths[wi] == RAY_SYM_W16 ? (const void*)&b16 :
                              widths[wi] == RAY_SYM_W32 ? (const void*)&b32 :
                                                          (const void*)&b64;
            v = ray_vec_append(v, src);
            TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        }
        TEST_ASSERT_EQ_I(v->len, n);
        TEST_ASSERT_TRUE(ray_vec_may_have_nulls(v));
        TEST_ASSERT_FALSE(ray_vec_has_nulls(v));

        /* null in the unaligned tail */
        ray_write_sym(ray_data(v), n - 1, 0, RAY_SYM, v->attrs);
        TEST_ASSERT_TRUE(ray_vec_has_nulls(v));
        ray_write_sym(ray_data(v), n - 1, (uint64_t)n, RAY_SYM, v->attrs);
        TEST_ASSERT_FALSE(ray_vec_has_nulls(v));

        /* null in the body; a slice that excludes it stays null-free */
        ray_write_sym(ray_data(v), 5, 0, RAY_SYM, v->attrs);
        TEST_ASSERT_TRUE(ray_vec_has_nulls(v));
        ray_t* clean = ray_vec_slice(v, 6, n - 6);
        TEST_ASSERT_FALSE(RAY_IS_ERR(clean));
        TEST_ASSERT_FALSE(ray_vec_has_nulls(clean));
        ray_t* dirty = ray_vec_slice(v, 3, 4);
        TEST_ASSERT_FALSE(RAY_IS_ERR(dirty));
        TEST_ASSERT_TRUE(ray_vec_has_nulls(dirty));
        ray_release(clean);
        ray_release(dirty);
        ray_release(v);
    }

    /* STR: canonical "" is the null */
    ray_t* s = ray_vec_new(RAY_STR, 3);
    s = ray_str_vec_append(s, "a", 1);
    s = ray_str_vec_append(s, "a long pooled string value", 26);
    s = ray_str_vec_append(s, "bb", 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_FALSE(ray_vec_has_nulls(s));
    ray_vec_set_null(s, 2, true);
    TEST_ASSERT_TRUE(ray_vec_has_nulls(s));
    ray_release(s);

    /* empty text vec is trivially null-free */
    ray_t* e = ray_sym_vec_new(RAY_SYM_W16, 0);
    TEST_ASSERT_FALSE(ray_vec_has_nulls(e));
    ray_release(e);
    PASS();
}

/* ---- slice_of_slice (parent_offset accumulation) ----------------------- */

static test_result_t test_vec_slice_of_slice(void) {
    /* Create base vec [0..9], then slice [2..7] (len=5), then
     * slice that [1..3] (len=2).  The nested slice should resolve
     * to the original parent with accumulated offset 3. */
    int64_t raw[10];
    for (int i = 0; i < 10; i++) raw[i] = (int64_t)(i * 10);
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 10);
    TEST_ASSERT_NOT_NULL(v);

    ray_t* s1 = ray_vec_slice(v, 2, 5);   /* [20,30,40,50,60] */
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s1));
    TEST_ASSERT_EQ_I(s1->len, 5);

    /* Slice-of-slice path: exercises lines 321-324 */
    ray_t* s2 = ray_vec_slice(s1, 1, 2);  /* [30,40] */
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s2));
    TEST_ASSERT_EQ_I(s2->len, 2);

    /* s2 should resolve directly to v (the parent) */
    TEST_ASSERT_EQ_PTR(s2->slice_parent, v);
    TEST_ASSERT_EQ_I(s2->slice_offset, 3); /* offset 2+1=3 */

    int64_t* p0 = (int64_t*)ray_vec_get(s2, 0);
    TEST_ASSERT_EQ_I(*p0, 30);
    int64_t* p1 = (int64_t*)ray_vec_get(s2, 1);
    TEST_ASSERT_EQ_I(*p1, 40);

    ray_release(s2);
    ray_release(s1);
    ray_release(v);
    PASS();
}

/* ---- concat: SYM with mismatched widths (widening path) --------------- */

static test_result_t test_vec_concat_sym_widen(void) {
    /* a=W8 [1,2], b=W16 [300,400] -> result W16 [1,2,300,400]
     * Exercises lines 455-464 (element-by-element widen path). */
    ray_t* a = ray_sym_vec_new(RAY_SYM_W8, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    uint8_t v8_0 = 1, v8_1 = 2;
    a = ray_vec_append(a, &v8_0);
    a = ray_vec_append(a, &v8_1);
    TEST_ASSERT_EQ_I(a->len, 2);

    ray_t* b = ray_sym_vec_new(RAY_SYM_W16, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    uint16_t v16_0 = 300, v16_1 = 400;
    b = ray_vec_append(b, &v16_0);
    b = ray_vec_append(b, &v16_1);
    TEST_ASSERT_EQ_I(b->len, 2);

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->len, 4);
    TEST_ASSERT_EQ_I(c->type, RAY_SYM);
    /* result should use the wider (W16) encoding */
    uint8_t out_width = c->attrs & RAY_SYM_W_MASK;
    TEST_ASSERT_EQ_I(out_width, RAY_SYM_W16);

    /* Verify values via get_sym_id */
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(c, 0), 1);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(c, 1), 2);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(c, 2), 300);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(c, 3), 400);

    ray_release(a);
    ray_release(b);
    ray_release(c);
    PASS();
}

/* ---- insert_at: shift null bits (exercises lines 571-585) -------------- */

static test_result_t test_vec_insert_at_shift_nulls(void) {
    /* Build [10, null, 30], then insert 99 at index 1 → [10, 99, null, 30].
     * Regression for prior bug: a now-removed null-bit shift loop called
     * ray_vec_is_null() AFTER memmove had moved the NULL_I64 sentinel
     * into the next slot, then wrote that null forward, clobbering the
     * real value 30 at d[3].  After fix the loop is gone — memmove
     * already places sentinels correctly. */
    ray_t* v = ray_vec_new(RAY_I64, 4);
    TEST_ASSERT_NOT_NULL(v);
    int64_t v0 = 10, v1 = 0, v2 = 30;
    v = ray_vec_append(v, &v0);
    v = ray_vec_append(v, &v1);
    v = ray_vec_append(v, &v2);
    ray_vec_set_null(v, 1, true);  /* slot 1 = null */
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 1));
    TEST_ASSERT_EQ_I(v->len, 3);

    int64_t new_val = 99;
    v = ray_vec_insert_at(v, 1, &new_val);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 4);

    /* Expected: [10, 99, null, 30].  The 30 at d[3] must NOT be clobbered. */
    const int64_t* d = (const int64_t*)ray_data(v);
    TEST_ASSERT_EQ_I(d[0], 10);
    TEST_ASSERT_EQ_I(d[1], 99);
    TEST_ASSERT_EQ_I(d[3], 30);
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 1));
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 2));   /* shifted from slot 1 */
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 3));  /* value 30 preserved */

    ray_release(v);
    PASS();
}

/* ---- insert_at: insert at beginning and end (fast paths) --------------- */

static test_result_t test_vec_insert_at_boundaries(void) {
    int64_t raw[] = {10, 20, 30};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);

    /* insert at end = append equivalent */
    int64_t val_end = 40;
    v = ray_vec_insert_at(v, 3, &val_end);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 4);
    int64_t* d = (int64_t*)ray_data(v);
    TEST_ASSERT_EQ_I(d[3], 40);

    /* insert at beginning */
    int64_t val_start = 0;
    v = ray_vec_insert_at(v, 0, &val_start);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 5);
    d = (int64_t*)ray_data(v);
    TEST_ASSERT_EQ_I(d[0], 0);
    TEST_ASSERT_EQ_I(d[1], 10);

    /* STR rejected */
    ray_t* sv = ray_vec_new(RAY_STR, 2);
    ray_t* err = ray_vec_insert_at(sv, 0, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    ray_release(sv);

    ray_release(v);
    PASS();
}

/* ---- insert_many: single-element broadcast, parallel, null propagation - */

static test_result_t test_vec_insert_many_coverage(void) {
    /* 1. N=0 fast-path: result is a retained copy */
    int64_t raw[] = {10, 20, 30};
    ray_t* base = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_NOT_NULL(base);

    ray_t* empty_idxs = ray_vec_new(RAY_I64, 0);
    empty_idxs->len = 0;
    ray_t* vals_any = ray_vec_new(RAY_I64, 0);
    vals_any->len = 0;
    ray_t* r0 = ray_vec_insert_many(base, empty_idxs, vals_any);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r0));
    TEST_ASSERT_EQ_I(r0->len, 3);
    ray_release(r0);
    ray_release(empty_idxs);
    ray_release(vals_any);

    /* 2. Parallel: insert [99,88] at positions [1,2] */
    int64_t idx_raw[] = {1, 2};
    ray_t* idxs = ray_vec_from_raw(RAY_I64, idx_raw, 2);
    int64_t val_raw[] = {99, 88};
    ray_t* vals = ray_vec_from_raw(RAY_I64, val_raw, 2);
    ray_t* r1 = ray_vec_insert_many(base, idxs, vals);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(r1->len, 5);  /* 3 + 2 */
    int64_t* d1 = (int64_t*)ray_data(r1);
    TEST_ASSERT_EQ_I(d1[0], 10);
    TEST_ASSERT_EQ_I(d1[1], 99);
    TEST_ASSERT_EQ_I(d1[2], 20);
    TEST_ASSERT_EQ_I(d1[3], 88);
    TEST_ASSERT_EQ_I(d1[4], 30);
    ray_release(r1);
    ray_release(idxs);
    ray_release(vals);

    /* 3. Single-element vec broadcast (len=1) — exercises line 759 */
    int64_t bc_idx[] = {0, 2};
    ray_t* bc_idxs = ray_vec_from_raw(RAY_I64, bc_idx, 2);
    int64_t bc_val[] = {77};
    ray_t* bc_vals = ray_vec_from_raw(RAY_I64, bc_val, 1);
    ray_t* r2 = ray_vec_insert_many(base, bc_idxs, bc_vals);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(r2->len, 5);
    int64_t* d2 = (int64_t*)ray_data(r2);
    TEST_ASSERT_EQ_I(d2[0], 77);
    TEST_ASSERT_EQ_I(d2[1], 10);
    TEST_ASSERT_EQ_I(d2[2], 20);
    TEST_ASSERT_EQ_I(d2[3], 77);
    TEST_ASSERT_EQ_I(d2[4], 30);
    ray_release(r2);
    ray_release(bc_idxs);
    ray_release(bc_vals);

    /* 4. Parallel with null propagation from vals and from base */
    ray_t* base_nulls = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_vec_set_null(base_nulls, 2, true);  /* base[2] is null */
    int64_t ni_raw[] = {0};
    ray_t* ni = ray_vec_from_raw(RAY_I64, ni_raw, 1);
    int64_t nv_raw[] = {55};
    ray_t* nv = ray_vec_from_raw(RAY_I64, nv_raw, 1);
    ray_vec_set_null(nv, 0, true);  /* val to insert is null */
    ray_t* r3 = ray_vec_insert_many(base_nulls, ni, nv);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r3));
    TEST_ASSERT_EQ_I(r3->len, 4);
    TEST_ASSERT_TRUE(ray_vec_is_null(r3, 0));   /* inserted null */
    TEST_ASSERT_FALSE(ray_vec_is_null(r3, 1));  /* base[0]=10 */
    TEST_ASSERT_TRUE(ray_vec_is_null(r3, 3));   /* base[2] null propagated */
    ray_release(r3);
    ray_release(ni);
    ray_release(nv);
    ray_release(base_nulls);

    ray_release(base);
    PASS();
}

/* ---- insert_many: error paths ------------------------------------------ */

static test_result_t test_vec_insert_many_errors(void) {
    int32_t i32_raw[] = {1, 2, 3};
    ray_t* base = ray_vec_from_raw(RAY_I32, i32_raw, 3);

    /* wrong idxs type */
    ray_t* bad_idxs = ray_vec_from_raw(RAY_I32, (int32_t[]){0}, 1);
    ray_t* vals1 = ray_vec_from_raw(RAY_I32, (int32_t[]){9}, 1);
    ray_t* r1 = ray_vec_insert_many(base, bad_idxs, vals1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "type");
    ray_release(bad_idxs);
    ray_release(vals1);

    /* STR target rejected */
    ray_t* sv = ray_vec_new(RAY_STR, 2);
    sv = ray_str_vec_append(sv, "x", 1);
    ray_t* i64_idxs = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    ray_t* i64_vals = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    ray_t* r2 = ray_vec_insert_many(sv, i64_idxs, i64_vals);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "type");
    ray_release(sv);
    ray_release(i64_idxs);
    ray_release(i64_vals);

    /* out-of-range index */
    ray_t* oob_idxs = ray_vec_from_raw(RAY_I64, (int64_t[]){99}, 1);
    ray_t* vals2 = ray_vec_from_raw(RAY_I32, (int32_t[]){5}, 1);
    ray_t* r3 = ray_vec_insert_many(base, oob_idxs, vals2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "range");
    ray_release(oob_idxs);
    ray_release(vals2);

    /* vals len mismatch (not 1 and not N) */
    ray_t* idxs2 = ray_vec_from_raw(RAY_I64, (int64_t[]){0, 1}, 2);
    ray_t* vals3 = ray_vec_from_raw(RAY_I32, (int32_t[]){5, 6, 7}, 3);
    ray_t* r4 = ray_vec_insert_many(base, idxs2, vals3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "range");
    ray_release(idxs2);
    ray_release(vals3);

    /* wrong vals type */
    ray_t* tidxs = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    ray_t* wrong_vals = ray_vec_from_raw(RAY_F64, (double[]){1.0}, 1);
    ray_t* r5 = ray_vec_insert_many(base, tidxs, wrong_vals);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    TEST_ASSERT_STR_EQ(ray_err_code(r5), "type");
    ray_release(tidxs);
    ray_release(wrong_vals);

    ray_release(base);
    PASS();
}

/* ---- embedding_new ------------------------------------------------------- */

static test_result_t test_embedding_new(void) {
    /* Exercises ray_embedding_new (lines 1237-1243) */
    ray_t* e = ray_embedding_new(3, 4);  /* 3 rows x 4 dims = 12 F32 */
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(RAY_IS_ERR(e));
    TEST_ASSERT_EQ_I(e->type, RAY_F32);
    TEST_ASSERT_EQ_I(e->len, 12);

    float* d = (float*)ray_data(e);
    d[0] = 1.0f; d[1] = 2.0f; d[2] = 3.0f; d[3] = 4.0f;
    TEST_ASSERT_EQ_F(d[0], 1.0f, 1e-6f);
    TEST_ASSERT_EQ_F(d[3], 4.0f, 1e-6f);

    ray_release(e);
    PASS();
}

/* ---- vec_copy_nulls: slice source path ---------------------------------- */

static test_result_t test_vec_copy_nulls_slice_src(void) {
    /* src is a slice of a nullable vec — exercises lines 1295-1297 */
    int64_t raw[] = {1, 2, 3, 4, 5};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);
    ray_vec_set_null(v, 2, true);
    ray_vec_set_null(v, 4, true);

    /* slice [1..3] = [2, null, 4] */
    ray_t* src = ray_vec_slice(v, 1, 3);
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_FALSE(RAY_IS_ERR(src));

    /* dst is a fresh same-type vec */
    ray_t* dst = ray_vec_new(RAY_I64, 3);
    int64_t fill = 0;
    for (int i = 0; i < 3; i++) dst = ray_vec_append(dst, &fill);
    TEST_ASSERT_EQ_I(dst->len, 3);

    /* Copy nulls from the slice src — null at src[1] (=parent[2]) */
    ray_err_t err = ray_vec_copy_nulls(dst, src);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    TEST_ASSERT_FALSE(ray_vec_is_null(dst, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(dst, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(dst, 2));

    ray_release(dst);
    ray_release(src);
    ray_release(v);
    PASS();
}

/* ---- str_vec: set null, insert_at, compact ----------------------------- */

static test_result_t test_str_vec_null_insert_compact(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);

    /* Append short (inline) and long (pooled) strings */
    v = ray_str_vec_append(v, "hi", 2);
    v = ray_str_vec_append(v, "a_longer_string_exceeds_12bytes", 31);
    v = ray_str_vec_append(v, "mid", 3);
    v = ray_str_vec_append(v, "another_very_long_pooled_string!", 32);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 4);

    /* set_null_checked on STR: STR IS nullable (only SYM/BOOL/U8 are rejected).
     * set_null_checked on a slice must return RAY_ERR_TYPE. */
    ray_t* sv = ray_vec_slice(v, 0, 2);
    TEST_ASSERT_NOT_NULL(sv);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sv));
    ray_err_t err = ray_vec_set_null_checked(sv, 0, true);
    TEST_ASSERT_EQ_I(err, RAY_ERR_TYPE);  /* slice → error */
    ray_release(sv);
    /* On the real vec, SYM writes its canonical id-0 null. */
    ray_t* sym_v = ray_sym_vec_new(RAY_SYM_W64, 2);
    uint64_t sid = 1;
    sym_v = ray_vec_append(sym_v, &sid);
    ray_err_t sym_err = ray_vec_set_null_checked(sym_v, 0, true);
    TEST_ASSERT_EQ_I(sym_err, RAY_OK);
    TEST_ASSERT_TRUE(ray_vec_is_null(sym_v, 0));
    ray_release(sym_v);

    /* insert_at: insert at end */
    v = ray_str_vec_insert_at(v, 4, "end", 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 5);

    /* set overwrites a pooled string with inline (adds dead bytes) */
    v = ray_str_vec_set(v, 1, "short", 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    size_t out_len = 0;
    const char* s = ray_str_vec_get(v, 1, &out_len);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQ_I((int64_t)out_len, 5);

    /* compact: reclaim dead pool bytes */
    v = ray_str_vec_compact(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 5);

    /* verify compact didn't lose pooled content */
    const char* s2 = ray_str_vec_get(v, 3, &out_len);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_EQ_I((int64_t)out_len, 32);

    ray_release(v);
    PASS();
}

/* ---- str_vec: get/set via slice ---------------------------------------- */

static test_result_t test_str_vec_get_null_paths(void) {
    /* Covers ray_str_vec_get null/empty/pooled paths and STR type-reject */
    ray_t* v = ray_vec_new(RAY_STR, 3);
    v = ray_str_vec_append(v, "", 0);          /* empty */
    v = ray_str_vec_append(v, "hello", 5);     /* inline */
    v = ray_str_vec_append(v, "this_str_is_definitely_longer_than_12_bytes", 43); /* pooled */
    TEST_ASSERT_EQ_I(v->len, 3);

    size_t l = 0;
    const char* s0 = ray_str_vec_get(v, 0, &l);
    TEST_ASSERT_NOT_NULL(s0);
    TEST_ASSERT_EQ_I((int64_t)l, 0);

    const char* s1 = ray_str_vec_get(v, 1, &l);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_EQ_I((int64_t)l, 5);

    const char* s2 = ray_str_vec_get(v, 2, &l);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_EQ_I((int64_t)l, 43);

    /* ray_vec_get on STR always returns NULL */
    void* p = ray_vec_get(v, 0);
    TEST_ASSERT_NULL(p);

    /* ray_vec_append on STR returns type error */
    int64_t dummy = 0;
    ray_t* err = ray_vec_append(v, &dummy);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    TEST_ASSERT_STR_EQ(ray_err_code(err), "type");

    ray_release(v);
    PASS();
}

/* ---- from_raw: error paths and zero-count ------------------------------- */

static test_result_t test_vec_from_raw_errors(void) {
    /* RAY_LIST=0 → rejected (type <= 0) */
    ray_t* r1 = ray_vec_from_raw(RAY_LIST, NULL, 0);
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    TEST_ASSERT_STR_EQ(ray_err_code(r1), "type");

    /* negative count */
    ray_t* r2 = ray_vec_from_raw(RAY_I64, NULL, -1);
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "range");

    /* STR rejected */
    ray_t* r3 = ray_vec_from_raw(RAY_STR, NULL, 0);
    TEST_ASSERT_NOT_NULL(r3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "type");

    /* zero-count valid */
    ray_t* r4 = ray_vec_from_raw(RAY_I64, NULL, 0);
    TEST_ASSERT_NOT_NULL(r4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r4));
    TEST_ASSERT_EQ_I(r4->len, 0);
    ray_release(r4);

    /* NOTE: RAY_LIST=0 and RAY_TABLE=98 both fail the type guard in
     * ray_vec_from_raw, making lines 816-821 and 499-503 unreachable
     * via the public API. Documented here as unreachable dead code. */

    PASS();
}

/* ---- insert_many: SYM width mismatch + single-element-broadcast null --- */

static test_result_t test_vec_insert_many_sym_and_bc_null(void) {
    /* 1. SYM width mismatch: vec=W16, vals=W8 → type error (line 673) */
    ray_t* sym16 = ray_sym_vec_new(RAY_SYM_W16, 3);
    uint16_t ids16[] = {10, 20, 30};
    for (int i = 0; i < 3; i++) sym16 = ray_vec_append(sym16, &ids16[i]);
    TEST_ASSERT_EQ_I(sym16->len, 3);

    ray_t* sym_idxs = ray_vec_from_raw(RAY_I64, (int64_t[]){1}, 1);
    ray_t* sym_vals_w8 = ray_sym_vec_new(RAY_SYM_W8, 1);
    uint8_t id8 = 5;
    sym_vals_w8 = ray_vec_append(sym_vals_w8, &id8);
    ray_t* r_sym_err = ray_vec_insert_many(sym16, sym_idxs, sym_vals_w8);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r_sym_err));
    TEST_ASSERT_STR_EQ(ray_err_code(r_sym_err), "type");
    ray_release(sym_idxs);
    ray_release(sym_vals_w8);
    ray_release(sym16);

    /* 2. Single-element broadcast with null value (exercises line 759-763) */
    int64_t base_raw[] = {1, 2, 3};
    ray_t* base = ray_vec_from_raw(RAY_I64, base_raw, 3);

    ray_t* bc_idxs = ray_vec_from_raw(RAY_I64, (int64_t[]){0, 2}, 2);
    /* Build a 1-element vec with a null */
    ray_t* bc_null_val = ray_vec_new(RAY_I64, 1);
    int64_t z = 0;
    bc_null_val = ray_vec_append(bc_null_val, &z);
    ray_vec_set_null(bc_null_val, 0, true);

    ray_t* r_bc = ray_vec_insert_many(base, bc_idxs, bc_null_val);
    TEST_ASSERT_NOT_NULL(r_bc);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_bc));
    TEST_ASSERT_EQ_I(r_bc->len, 5);
    /* Both broadcast slots should be null */
    TEST_ASSERT_TRUE(ray_vec_is_null(r_bc, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(r_bc, 3));
    TEST_ASSERT_FALSE(ray_vec_is_null(r_bc, 1));

    ray_release(bc_null_val);
    ray_release(bc_idxs);
    ray_release(r_bc);
    ray_release(base);
    PASS();
}

/* ---- sentinel_is_null: SYM path (HAS_NULLS + SYM type) ----------------- */

static test_result_t test_vec_sym_is_null_path(void) {
    /* sentinel_is_null for SYM (lines 69-75) is reached when:
     * - vec has RAY_ATTR_HAS_NULLS set AND
     * - vec->type == RAY_SYM
     * BUT ray_vec_set_null_checked rejects SYM, so HAS_NULLS can only be
     * set by direct attr manipulation or via internal code.
     *
     * Calling ray_vec_is_null on a SYM vec with HAS_NULLS clear short-circuits
     * at the vec_any_nulls() gate.  Without HAS_NULLS the SYM sentinel path
     * (lines 69-75) is unreachable from the public API.
     *
     * We verify the public-observable behaviour: SYM always returns false. */
    ray_sym_init();

    ray_t* w8  = ray_sym_vec_new(RAY_SYM_W8,  4);
    ray_t* w16 = ray_sym_vec_new(RAY_SYM_W16, 4);
    ray_t* w32 = ray_sym_vec_new(RAY_SYM_W32, 4);
    ray_t* w64 = ray_sym_vec_new(RAY_SYM_W64, 4);

    uint8_t  id8  = 0;
    uint16_t id16 = 0;
    uint32_t id32 = 0;
    uint64_t id64 = 0;
    w8  = ray_vec_append(w8,  &id8);
    w16 = ray_vec_append(w16, &id16);
    w32 = ray_vec_append(w32, &id32);
    w64 = ray_vec_append(w64, &id64);

    /* Every adaptive width recognizes id 0 as canonical null. */
    TEST_ASSERT_TRUE(ray_vec_is_null(w8,  0));
    TEST_ASSERT_TRUE(ray_vec_is_null(w16, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(w32, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(w64, 0));

    ray_release(w8); ray_release(w16); ray_release(w32); ray_release(w64);
    ray_sym_destroy();
    PASS();
}

/* ---- sentinel_is_null: per-type sentinel detection --------------------- *
 *
 * ray_vec_is_null reaches sentinel_is_null only when HAS_NULLS is set AND
 * the type is in the nullable set.  The existing suite exercises I64, I16,
 * F32 and STR; this fills in the remaining sentinel arms (lines 52-82):
 * F64, I32, DATE, TIME, TIMESTAMP, GUID.  Each null is set via
 * ray_vec_set_null_checked (which writes the type-correct NULL_* sentinel
 * and flips HAS_NULLS) and read back through ray_vec_is_null. */

static test_result_t test_vec_sentinel_all_types(void) {
    struct { int8_t type; const char* name; } cases[] = {
        { RAY_F64,       "f64"       },
        { RAY_I32,       "i32"       },
        { RAY_DATE,      "date"      },
        { RAY_TIME,      "time"      },
        { RAY_TIMESTAMP, "timestamp" },
        { RAY_I64,       "i64"       },
        { RAY_I16,       "i16"       },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        ray_t* v = ray_vec_new(cases[c].type, 8);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));

        /* Append 5 non-null elements (memset-zero raw block, then bump len). */
        uint8_t esz = ray_sym_elem_size(cases[c].type, 0);
        for (int i = 0; i < 5; i++) {
            uint8_t scratch[16] = {0};
            /* Use a non-sentinel value (1) so is_null is definitively false. */
            scratch[0] = 1;
            v = ray_vec_append(v, scratch);
            TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        }
        TEST_ASSERT_EQ_I(v->len, 5);
        (void)esz;

        /* No nulls yet → fast-path gate (HAS_NULLS clear) returns false. */
        TEST_ASSERT_FALSE(ray_vec_is_null(v, 2));

        /* Set element 3 null → exercises this type's sentinel write + read. */
        ray_err_t err = ray_vec_set_null_checked(v, 3, true);
        TEST_ASSERT_EQ_I(err, RAY_OK);
        TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);
        TEST_ASSERT_TRUE(ray_vec_is_null(v, 3));
        TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
        TEST_ASSERT_FALSE(ray_vec_is_null(v, 4));

        ray_release(v);
    }
    PASS();
}

/* ---- sentinel_is_null: GUID arm (16-byte all-zero null) ---------------- */

static test_result_t test_vec_guid_null_sentinel(void) {
    /* GUID null = 16 zero bytes.  Exercises sentinel_is_null RAY_GUID arm
     * (lines 78-82) and ray_vec_set_null_checked RAY_GUID arm (lines 873-874). */
    ray_t* v = ray_vec_new(RAY_GUID, 4);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    for (int i = 0; i < 3; i++) {
        uint8_t guid[16];
        memset(guid, (uint8_t)(i + 1), 16);  /* non-zero → not null */
        v = ray_vec_append(v, guid);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }
    TEST_ASSERT_EQ_I(v->len, 3);

    /* Before set-null: a non-zero GUID is not null. */
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));

    ray_err_t err = ray_vec_set_null_checked(v, 1, true);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 2));

    /* All 16 bytes of element 1 should now be zero. */
    const uint8_t* g1 = (const uint8_t*)ray_data(v) + 1 * 16;
    for (int b = 0; b < 16; b++) TEST_ASSERT_EQ_I(g1[b], 0);

    ray_release(v);
    PASS();
}

/* ---- vec_is_null: out-of-range and NULL/err guards --------------------- */

static test_result_t test_vec_is_null_guards(void) {
    /* ray_vec_is_null returns false for NULL / error / OOB indices
     * (lines 1246-1247). */
    TEST_ASSERT_FALSE(ray_vec_is_null(NULL, 0));

    int64_t raw[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_vec_set_null(v, 1, true);  /* HAS_NULLS set so the gate doesn't short-circuit */

    TEST_ASSERT_FALSE(ray_vec_is_null(v, -1));  /* idx < 0 */
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 3));   /* idx >= len */
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 99));  /* idx >> len */
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 1));    /* in-range null still works */

    ray_release(v);
    PASS();
}

/* ---- set_null_checked: range + null/err guards ------------------------- */

static test_result_t test_vec_set_null_checked_guards(void) {
    /* NULL vec → RAY_ERR_TYPE (line 837). */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(NULL, 0, true), RAY_ERR_TYPE);

    int64_t raw[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);

    /* idx out of range → RAY_ERR_RANGE (line 839). */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, -1, true), RAY_ERR_RANGE);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 3, true), RAY_ERR_RANGE);

    /* is_null=false is a no-op that still returns RAY_OK (clear path,
     * line 862 false branch). */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 0, false), RAY_OK);
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));

    ray_release(v);
    PASS();
}

/* ---- vec_set: SYM and DATE element types ------------------------------- */

static test_result_t test_vec_set_more_types(void) {
    /* ray_vec_set on a SYM vec (adaptive width) and a DATE vec — exercises
     * the generic memcpy slot-write for non-I64 types. */
    ray_t* sym = ray_sym_vec_new(RAY_SYM_W16, 3);
    uint16_t ids[] = {10, 20, 30};
    for (int i = 0; i < 3; i++) sym = ray_vec_append(sym, &ids[i]);
    TEST_ASSERT_EQ_I(sym->len, 3);

    uint16_t new_id = 999;
    sym = ray_vec_set(sym, 1, &new_id);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sym));
    uint16_t* sd = (uint16_t*)ray_data(sym);
    TEST_ASSERT_EQ_I(sd[1], 999);
    ray_release(sym);

    /* DATE vec */
    int32_t dates[] = {100, 200, 300};
    ray_t* dv = ray_vec_from_raw(RAY_DATE, dates, 3);
    int32_t nd = 12345;
    dv = ray_vec_set(dv, 2, &nd);
    TEST_ASSERT_FALSE(RAY_IS_ERR(dv));
    int32_t* dd = (int32_t*)ray_data(dv);
    TEST_ASSERT_EQ_I(dd[2], 12345);

    /* set on negative index → range error */
    ray_t* e = ray_vec_set(dv, -1, &nd);
    TEST_ASSERT_TRUE(RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "range");

    /* set on STR vec → type error */
    ray_t* strv = ray_vec_new(RAY_STR, 1);
    strv = ray_str_vec_append(strv, "x", 1);
    ray_t* e2 = ray_vec_set(strv, 0, &nd);
    TEST_ASSERT_TRUE(RAY_IS_ERR(e2));
    TEST_ASSERT_STR_EQ(ray_err_code(e2), "type");
    ray_release(strv);

    ray_release(dv);
    PASS();
}

/* ---- vec_append: error guards ------------------------------------------ */

static test_result_t test_vec_append_guards(void) {
    /* NULL passthrough */
    TEST_ASSERT_NULL(ray_vec_append(NULL, NULL));

    /* error passthrough */
    ray_t* err = ray_error("range", NULL);
    ray_t* r = ray_vec_append(err, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    /* STR rejected (line 217) */
    ray_t* strv = ray_vec_new(RAY_STR, 1);
    int64_t dummy = 0;
    ray_t* e = ray_vec_append(strv, &dummy);
    TEST_ASSERT_TRUE(RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "type");
    ray_release(strv);

    PASS();
}

/* ---- insert_at: growth past capacity + various types ------------------- */

static test_result_t test_vec_insert_at_grow(void) {
    /* Force the grow-past-capacity realloc branch (lines 544-561) by
     * starting tiny and inserting beyond capacity, always at index 0
     * (the worst-case memmove path). */
    ray_t* v = ray_vec_new(RAY_I32, 1);
    TEST_ASSERT_NOT_NULL(v);

    for (int i = 0; i < 64; i++) {
        int32_t val = (int32_t)(i + 1);
        v = ray_vec_insert_at(v, 0, &val);  /* always prepend */
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }
    TEST_ASSERT_EQ_I(v->len, 64);
    /* Last inserted (64) is at front; first inserted (1) is at back. */
    int32_t* d = (int32_t*)ray_data(v);
    TEST_ASSERT_EQ_I(d[0], 64);
    TEST_ASSERT_EQ_I(d[63], 1);

    /* insert_at error guards */
    ray_t* err = ray_vec_insert_at(v, -1, d);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    TEST_ASSERT_STR_EQ(ray_err_code(err), "range");
    err = ray_vec_insert_at(v, 999, d);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    TEST_ASSERT_STR_EQ(ray_err_code(err), "range");

    /* NULL passthrough + error passthrough */
    TEST_ASSERT_NULL(ray_vec_insert_at(NULL, 0, NULL));

    ray_release(v);
    PASS();
}

/* ---- insert_at: middle insertion (memmove shift) ----------------------- */

static test_result_t test_vec_insert_at_middle(void) {
    /* Insert in the middle (idx < old_len) → exercises the memmove
     * shift branch (line 567). */
    int64_t raw[] = {10, 20, 30, 40};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 4);

    int64_t mid = 25;
    v = ray_vec_insert_at(v, 1, &mid);  /* [10, 25, 20, 30, 40] */
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 5);
    int64_t* d = (int64_t*)ray_data(v);
    TEST_ASSERT_EQ_I(d[0], 10);
    TEST_ASSERT_EQ_I(d[1], 25);
    TEST_ASSERT_EQ_I(d[2], 20);
    TEST_ASSERT_EQ_I(d[3], 30);
    TEST_ASSERT_EQ_I(d[4], 40);

    ray_release(v);
    PASS();
}

/* ---- insert_vec_at: middle splice (head/tail slice + concat) ----------- */

static test_result_t test_vec_insert_vec_at_middle(void) {
    /* idx in (0, len) hits the head=slice, tail=slice, concat-concat path
     * (lines 613-626). */
    int64_t base_raw[] = {1, 2, 3, 4};
    ray_t* base = ray_vec_from_raw(RAY_I64, base_raw, 4);
    int64_t src_raw[] = {97, 98};
    ray_t* src = ray_vec_from_raw(RAY_I64, src_raw, 2);

    ray_t* r = ray_vec_insert_vec_at(base, 2, src);  /* [1,2,97,98,3,4] */
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->len, 6);
    int64_t* d = (int64_t*)ray_data(r);
    TEST_ASSERT_EQ_I(d[0], 1);
    TEST_ASSERT_EQ_I(d[1], 2);
    TEST_ASSERT_EQ_I(d[2], 97);
    TEST_ASSERT_EQ_I(d[3], 98);
    TEST_ASSERT_EQ_I(d[4], 3);
    TEST_ASSERT_EQ_I(d[5], 4);
    ray_release(r);

    /* idx == len → plain concat fast path (line 609) */
    ray_t* r_end = ray_vec_insert_vec_at(base, base->len, src);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_end));
    TEST_ASSERT_EQ_I(r_end->len, 6);
    int64_t* de = (int64_t*)ray_data(r_end);
    TEST_ASSERT_EQ_I(de[4], 97);
    TEST_ASSERT_EQ_I(de[5], 98);
    ray_release(r_end);

    /* idx == 0 → reversed concat fast path (line 611) */
    ray_t* r_begin = ray_vec_insert_vec_at(base, 0, src);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_begin));
    TEST_ASSERT_EQ_I(r_begin->len, 6);
    int64_t* db = (int64_t*)ray_data(r_begin);
    TEST_ASSERT_EQ_I(db[0], 97);
    TEST_ASSERT_EQ_I(db[1], 98);
    TEST_ASSERT_EQ_I(db[2], 1);
    ray_release(r_begin);

    /* Error guards: NULL/err passthrough, type mismatch, OOB idx */
    TEST_ASSERT_NULL(ray_vec_insert_vec_at(NULL, 0, src));
    ray_t* f32 = ray_vec_from_raw(RAY_F32, (float[]){1.0f}, 1);
    ray_t* mism = ray_vec_insert_vec_at(base, 0, f32);
    TEST_ASSERT_TRUE(RAY_IS_ERR(mism));
    TEST_ASSERT_STR_EQ(ray_err_code(mism), "type");
    ray_release(f32);

    ray_t* oob = ray_vec_insert_vec_at(base, 99, src);
    TEST_ASSERT_TRUE(RAY_IS_ERR(oob));
    TEST_ASSERT_STR_EQ(ray_err_code(oob), "range");

    ray_release(base);
    ray_release(src);
    PASS();
}

/* ---- insert_vec_at: middle splice with nulls --------------------------- */

static test_result_t test_vec_insert_vec_at_middle_nulls(void) {
    /* Middle splice where both base and src carry nulls — the head/tail
     * slices propagate parent nulls through concat. */
    int64_t base_raw[] = {1, 2, 3, 4};
    ray_t* base = ray_vec_from_raw(RAY_I64, base_raw, 4);
    ray_vec_set_null(base, 0, true);  /* base[0] null */
    ray_vec_set_null(base, 3, true);  /* base[3] null */

    int64_t src_raw[] = {97, 98};
    ray_t* src = ray_vec_from_raw(RAY_I64, src_raw, 2);
    ray_vec_set_null(src, 1, true);   /* src[1] null */

    ray_t* r = ray_vec_insert_vec_at(base, 2, src);  /* [N,2,97,N,3,N] */
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->len, 6);
    TEST_ASSERT_TRUE(ray_vec_is_null(r, 0));   /* base[0] */
    TEST_ASSERT_FALSE(ray_vec_is_null(r, 1));  /* base[1]=2 */
    TEST_ASSERT_FALSE(ray_vec_is_null(r, 2));  /* src[0]=97 */
    TEST_ASSERT_TRUE(ray_vec_is_null(r, 3));   /* src[1] null */
    TEST_ASSERT_FALSE(ray_vec_is_null(r, 4));  /* base[2]=3 */
    TEST_ASSERT_TRUE(ray_vec_is_null(r, 5));   /* base[3] null */

    ray_release(r);
    ray_release(src);
    ray_release(base);
    PASS();
}

/* ---- concat: LIST element retain path ---------------------------------- */

static test_result_t test_vec_concat_list_retain(void) {
    /* RAY_LIST concat: ray_vec_concat goes through the generic memcpy path
     * and then the LIST/TABLE child-pointer retain loop (lines 507-512).
     * ray_list_append retains the item, so we release our own refs after. */
    ray_t* x = ray_i64(11);
    ray_t* y = ray_i64(22);
    ray_t* z = ray_i64(33);

    ray_t* a = ray_list_new(2);
    a = ray_list_append(a, x);
    a = ray_list_append(a, y);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    TEST_ASSERT_EQ_I(a->len, 2);

    ray_t* b = ray_list_new(1);
    b = ray_list_append(b, z);
    TEST_ASSERT_FALSE(RAY_IS_ERR(b));

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->type, RAY_LIST);
    TEST_ASSERT_EQ_I(c->len, 3);

    /* Each child must be a valid, retained atom. */
    ray_t** ptrs = (ray_t**)ray_data(c);
    TEST_ASSERT_NOT_NULL(ptrs[0]);
    TEST_ASSERT_NOT_NULL(ptrs[2]);
    TEST_ASSERT_EQ_I(ptrs[0]->i64, 11);
    TEST_ASSERT_EQ_I(ptrs[2]->i64, 33);

    ray_release(c);
    ray_release(a);
    ray_release(b);
    /* Drop our original atom refs (list_append + concat each retained). */
    ray_release(x);
    ray_release(y);
    ray_release(z);
    PASS();
}

/* ---- concat: GUID (generic memcpy path) -------------------------------- */

static test_result_t test_vec_concat_guid(void) {
    /* GUID is not STR/SYM, so concat takes the same-width memcpy path
     * (lines 474-486) with 16-byte elements; verify byte-exact merge. */
    ray_t* a = ray_vec_new(RAY_GUID, 2);
    uint8_t g0[16], g1[16];
    memset(g0, 0xA1, 16);
    memset(g1, 0xB2, 16);
    a = ray_vec_append(a, g0);
    a = ray_vec_append(a, g1);
    TEST_ASSERT_EQ_I(a->len, 2);

    ray_t* b = ray_vec_new(RAY_GUID, 1);
    uint8_t g2[16];
    memset(g2, 0xC3, 16);
    b = ray_vec_append(b, g2);

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->len, 3);
    const uint8_t* cd = (const uint8_t*)ray_data(c);
    TEST_ASSERT_EQ_I(cd[0 * 16], 0xA1);
    TEST_ASSERT_EQ_I(cd[1 * 16], 0xB2);
    TEST_ASSERT_EQ_I(cd[2 * 16], 0xC3);

    ray_release(a);
    ray_release(b);
    ray_release(c);
    PASS();
}

/* ---- concat: NULL / error passthrough guards --------------------------- */

static test_result_t test_vec_concat_guards(void) {
    int64_t raw[] = {1, 2};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 2);

    /* a NULL → returns a (NULL) */
    TEST_ASSERT_NULL(ray_vec_concat(NULL, v));
    /* b NULL → returns b (NULL) */
    TEST_ASSERT_NULL(ray_vec_concat(v, NULL));

    /* a is error → returns a */
    ray_t* err = ray_error("range", NULL);
    ray_t* r = ray_vec_concat(err, v);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    ray_release(v);
    PASS();
}

/* ---- concat: empty operands -------------------------------------------- */

static test_result_t test_vec_concat_empty(void) {
    /* Concat with an empty vec on either side (a->len==0 / b->len==0). */
    int64_t raw[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_t* empty = ray_vec_new(RAY_I64, 0);

    ray_t* c1 = ray_vec_concat(empty, v);   /* [] , [1 2 3] */
    TEST_ASSERT_FALSE(RAY_IS_ERR(c1));
    TEST_ASSERT_EQ_I(c1->len, 3);
    int64_t* d1 = (int64_t*)ray_data(c1);
    TEST_ASSERT_EQ_I(d1[0], 1);
    ray_release(c1);

    ray_t* c2 = ray_vec_concat(v, empty);   /* [1 2 3] , [] */
    TEST_ASSERT_FALSE(RAY_IS_ERR(c2));
    TEST_ASSERT_EQ_I(c2->len, 3);
    ray_release(c2);

    ray_t* c3 = ray_vec_concat(empty, empty);  /* [] , [] */
    TEST_ASSERT_FALSE(RAY_IS_ERR(c3));
    TEST_ASSERT_EQ_I(c3->len, 0);
    ray_release(c3);

    ray_release(v);
    ray_release(empty);
    PASS();
}

/* ---- concat: STR with empty/inline and pooled merge -------------------- */

static test_result_t test_vec_concat_str(void) {
    /* STR concat path (lines 361-440): merges two string vecs with both
     * inline and pooled elements, rebasing pool offsets. */
    ray_t* a = ray_vec_new(RAY_STR, 2);
    a = ray_str_vec_append(a, "hi", 2);                                   /* inline */
    a = ray_str_vec_append(a, "this_is_a_long_pooled_string_aaa", 32);    /* pooled */
    TEST_ASSERT_EQ_I(a->len, 2);

    ray_t* b = ray_vec_new(RAY_STR, 2);
    b = ray_str_vec_append(b, "another_long_pooled_string_bbbb", 31);     /* pooled */
    b = ray_str_vec_append(b, "yo", 2);                                   /* inline */
    TEST_ASSERT_EQ_I(b->len, 2);

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->len, 4);

    size_t l = 0;
    const char* s0 = ray_str_vec_get(c, 0, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 2);
    TEST_ASSERT_MEM_EQ(2, s0, "hi");
    const char* s1 = ray_str_vec_get(c, 1, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 32);
    TEST_ASSERT_MEM_EQ(32, s1, "this_is_a_long_pooled_string_aaa");
    const char* s2 = ray_str_vec_get(c, 2, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 31);
    TEST_ASSERT_MEM_EQ(31, s2, "another_long_pooled_string_bbbb");
    const char* s3 = ray_str_vec_get(c, 3, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 2);
    TEST_ASSERT_MEM_EQ(2, s3, "yo");

    ray_release(a);
    ray_release(b);
    ray_release(c);
    PASS();
}

/* ---- concat: STR null propagation -------------------------------------- */

static test_result_t test_vec_concat_str_null(void) {
    ray_t* a = ray_vec_new(RAY_STR, 2);
    a = ray_str_vec_append(a, "x", 1);
    a = ray_str_vec_append(a, "yy", 2);
    ray_vec_set_null(a, 0, true);  /* STR: set_null -> canonical "" */
    TEST_ASSERT_TRUE(ray_vec_is_null(a, 0));

    ray_t* b = ray_vec_new(RAY_STR, 1);
    b = ray_str_vec_append(b, "zzz", 3);
    ray_vec_set_null(b, 0, true);  /* b[0] null */

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->len, 3);
    TEST_ASSERT_TRUE(ray_vec_is_null(c, 0));   /* a[0] -> canonical null */
    TEST_ASSERT_FALSE(ray_vec_is_null(c, 1));  /* a[1]=yy */
    TEST_ASSERT_TRUE(ray_vec_is_null(c, 2));   /* b[0] -> canonical null */

    ray_release(a);
    ray_release(b);
    ray_release(c);
    PASS();
}

/* Canonical text payloads must propagate even without HAS_NULLS.  Exercise
 * both operands, chunk/tail boundaries, all SYM width pairs and slices whose
 * parent has nulls outside the selected range. */
static test_result_t test_vec_concat_text_null_scan(void) {
    const uint8_t widths[] = {RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64};
    const int64_t positions[] = {-1, 0, 255, 256, 258};
    for (int kind = 0; kind < 2; kind++) {
        for (int wa = 0; wa < (kind ? 4 : 1); wa++) {
            for (int wb = 0; wb < (kind ? 4 : 1); wb++) {
                for (int side = 0; side < 2; side++) {
                    for (int p = 0; p < 5; p++) {
                        ray_t* v[2];
                        for (int s = 0; s < 2; s++) {
                            v[s] = kind ? ray_sym_vec_new(widths[s ? wb : wa], 261)
                                        : ray_vec_new(RAY_STR, 261);
                            TEST_ASSERT_NOT_NULL(v[s]);
                            TEST_ASSERT_FALSE(RAY_IS_ERR(v[s]));
                            if (kind) {
                                v[s]->len = 261;
                                for (int64_t i = 0; i < 261; i++)
                                    ray_write_sym(ray_data(v[s]), i, 1, RAY_SYM, v[s]->attrs);
                            } else {
                                for (int64_t i = 0; i < 261; i++)
                                    v[s] = ray_str_vec_append(v[s], "a pooled string longer than inline", 33);
                            }
                            ray_vec_set_null(v[s], 0, true);
                            ray_vec_set_null(v[s], 260, true);
                            if (s == side && positions[p] >= 0)
                                ray_vec_set_null(v[s], positions[p] + 1, true);
                            v[s]->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
                        }
                        ray_t* a = ray_vec_slice(v[0], 1, 259);
                        ray_t* b = ray_vec_slice(v[1], 1, 259);
                        ray_t* c = ray_vec_concat(a, b);
                        TEST_ASSERT_NOT_NULL(c);
                        TEST_ASSERT_FALSE(RAY_IS_ERR(c));
                        TEST_ASSERT_EQ_I(c->len, 518);
                        TEST_ASSERT_EQ_I(!!(c->attrs & RAY_ATTR_HAS_NULLS), positions[p] >= 0);
                        TEST_ASSERT_FALSE(c->attrs & (RAY_ATTR_SLICE | RAY_ATTR_HAS_INDEX | RAY_ATTR_SORTED));
                        /* Result owns its pool/domain independently of inputs. */
                        ray_release(a); ray_release(b);
                        ray_release(v[0]); ray_release(v[1]);
                        for (int64_t i = 0; i < c->len; i++) {
                            bool is_null = positions[p] >= 0 && i == side * 259 + positions[p];
                            TEST_ASSERT_EQ_I(ray_vec_is_null(c, i), is_null);
                            if (kind) {
                                TEST_ASSERT_EQ_I(ray_read_sym(ray_data(c), i, RAY_SYM, c->attrs), !is_null);
                            } else if (!is_null) {
                                size_t len;
                                const char* str = ray_str_vec_get(c, i, &len);
                                TEST_ASSERT_EQ_U(len, 33);
                                TEST_ASSERT_MEM_EQ(33, str, "a pooled string longer than inline");
                            } else {
                                ray_str_t zero = {0};
                                TEST_ASSERT_MEM_EQ(sizeof(zero), &((ray_str_t*)ray_data(c))[i], &zero);
                            }
                        }
                        ray_release(c);
                    }
                }
            }
        }
    }
    PASS();
}

static test_result_t test_vec_concat_text_empty(void) {
    for (int kind = 0; kind < 2; kind++) {
        ray_t* empty = ray_vec_new(kind ? RAY_SYM : RAY_STR, 0);
        ray_t* nulls = ray_vec_new(kind ? RAY_SYM : RAY_STR, 2);
        nulls->len = 2;
        ray_vec_set_null(nulls, 0, true);
        ray_vec_set_null(nulls, 1, true);
        nulls->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
        for (int side = 0; side < 3; side++) {
            ray_t* c = ray_vec_concat(side == 0 ? nulls : empty, side == 1 ? nulls : empty);
            TEST_ASSERT_NOT_NULL(c);
            TEST_ASSERT_FALSE(RAY_IS_ERR(c));
            TEST_ASSERT_EQ_I(c->len, side == 2 ? 0 : 2);
            TEST_ASSERT_EQ_I(!!(c->attrs & RAY_ATTR_HAS_NULLS), side != 2);
            for (int64_t i = 0; i < c->len; i++) TEST_ASSERT_TRUE(ray_vec_is_null(c, i));
            ray_release(c);
        }
        ray_release(nulls); ray_release(empty);
    }
    PASS();
}

/* ---- str_vec_append: pool growth across many large strings ------------- */

static test_result_t test_str_vec_append_pool_grow(void) {
    /* Append enough large strings to force the pool to realloc past its
     * initial capacity (lines 972-985), plus element-array growth. */
    ray_t* v = ray_vec_new(RAY_STR, 1);
    TEST_ASSERT_NOT_NULL(v);

    char big[64];
    memset(big, 'Q', sizeof(big));
    for (int i = 0; i < 40; i++) {
        /* Vary the first byte so each is distinct; all > 12 bytes → pooled. */
        big[0] = (char)('A' + (i % 26));
        v = ray_str_vec_append(v, big, sizeof(big));
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }
    TEST_ASSERT_EQ_I(v->len, 40);

    /* Verify a sampling read back correctly. */
    size_t l = 0;
    const char* s = ray_str_vec_get(v, 39, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 64);
    TEST_ASSERT_EQ_I(s[0], 'A' + (39 % 26));

    ray_release(v);
    PASS();
}

/* ---- str_vec_set: grow pool + inline overwrite of pooled --------------- */

static test_result_t test_str_vec_set_paths(void) {
    ray_t* v = ray_vec_new(RAY_STR, 2);
    v = ray_str_vec_append(v, "short", 5);                         /* inline */
    v = ray_str_vec_append(v, "x", 1);                             /* inline */
    TEST_ASSERT_EQ_I(v->len, 2);

    /* set element 0 to a long pooled string (allocates the pool, lines
     * 1096-1136). */
    v = ray_str_vec_set(v, 0, "now_a_long_pooled_string_value!!", 32);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    size_t l = 0;
    const char* s0 = ray_str_vec_get(v, 0, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 32);
    TEST_ASSERT_MEM_EQ(32, s0, "now_a_long_pooled_string_value!!");

    /* Overwrite the pooled element with an inline one — old bytes become
     * dead (lines 1088-1090). */
    v = ray_str_vec_set(v, 0, "tiny", 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    const char* s0b = ray_str_vec_get(v, 0, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 4);
    TEST_ASSERT_MEM_EQ(4, s0b, "tiny");

    /* Overwrite a pooled element with another pooled one (dead-byte track
     * on the second pooled→pooled set, lines 1126-1128). */
    v = ray_str_vec_set(v, 1, "first_pooled_value_here_yes!!!!!", 32);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v = ray_str_vec_set(v, 1, "second_pooled_value_here_too!!!!", 32);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    const char* s1 = ray_str_vec_get(v, 1, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 32);
    TEST_ASSERT_MEM_EQ(32, s1, "second_pooled_value_here_too!!!!");

    /* Error guards */
    ray_t* e = ray_str_vec_set(v, 99, "x", 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "range");
    ray_t* iv = ray_vec_from_raw(RAY_I64, (int64_t[]){1}, 1);
    ray_t* e2 = ray_str_vec_set(iv, 0, "x", 1);  /* non-STR → type */
    TEST_ASSERT_TRUE(RAY_IS_ERR(e2));
    TEST_ASSERT_STR_EQ(ray_err_code(e2), "type");
    ray_release(iv);

    ray_release(v);
    PASS();
}

/* ---- str_vec_get/append: error and slice guards ------------------------ */

static test_result_t test_str_vec_guards(void) {
    /* ray_str_vec_get on non-STR / NULL / OOB returns NULL with *out_len=0. */
    size_t l = 99;
    TEST_ASSERT_NULL(ray_str_vec_get(NULL, 0, &l));
    TEST_ASSERT_EQ_I((int64_t)l, 0);

    int64_t raw[] = {1};
    ray_t* iv = ray_vec_from_raw(RAY_I64, raw, 1);
    l = 99;
    TEST_ASSERT_NULL(ray_str_vec_get(iv, 0, &l));  /* wrong type */
    TEST_ASSERT_EQ_I((int64_t)l, 0);
    ray_release(iv);

    ray_t* v = ray_vec_new(RAY_STR, 1);
    v = ray_str_vec_append(v, "hi", 2);
    l = 99;
    TEST_ASSERT_NULL(ray_str_vec_get(v, 5, &l));   /* OOB */
    TEST_ASSERT_EQ_I((int64_t)l, 0);

    /* ray_str_vec_append on a non-STR vec → type error */
    ray_t* iv2 = ray_vec_from_raw(RAY_I64, raw, 1);
    ray_t* e = ray_str_vec_append(iv2, "x", 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "type");
    ray_release(iv2);

    /* ray_str_vec_get via slice */
    v = ray_str_vec_append(v, "pooled_string_longer_than_twelve", 32);
    ray_t* s = ray_vec_slice(v, 1, 1);  /* slice of the pooled element */
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    const char* sp = ray_str_vec_get(s, 0, &l);
    TEST_ASSERT_NOT_NULL(sp);
    TEST_ASSERT_EQ_I((int64_t)l, 32);
    TEST_ASSERT_MEM_EQ(32, sp, "pooled_string_longer_than_twelve");
    ray_release(s);

    ray_release(v);
    PASS();
}

/* ---- str_vec_insert_at: middle + boundaries ---------------------------- */

static test_result_t test_str_vec_insert_at_paths(void) {
    ray_t* v = ray_vec_new(RAY_STR, 3);
    v = ray_str_vec_append(v, "aa", 2);
    v = ray_str_vec_append(v, "long_pooled_string_one_here!!!!!", 32);
    v = ray_str_vec_append(v, "cc", 2);
    TEST_ASSERT_EQ_I(v->len, 3);

    /* Insert a pooled string in the middle (idx=1). */
    v = ray_str_vec_insert_at(v, 1, "another_long_pooled_inserted_!!!", 32);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 4);
    size_t l = 0;
    const char* s1 = ray_str_vec_get(v, 1, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 32);
    TEST_ASSERT_MEM_EQ(32, s1, "another_long_pooled_inserted_!!!");
    /* Original element shifted to index 2. */
    const char* s2 = ray_str_vec_get(v, 2, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 32);
    TEST_ASSERT_MEM_EQ(32, s2, "long_pooled_string_one_here!!!!!");

    /* Insert at front (idx=0). */
    v = ray_str_vec_insert_at(v, 0, "front", 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 5);
    const char* s0 = ray_str_vec_get(v, 0, &l);
    TEST_ASSERT_MEM_EQ(5, s0, "front");

    /* Error guards: non-STR type and OOB idx. */
    ray_t* iv = ray_vec_from_raw(RAY_I64, (int64_t[]){1}, 1);
    ray_t* e = ray_str_vec_insert_at(iv, 0, "x", 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "type");
    ray_release(iv);
    ray_t* e2 = ray_str_vec_insert_at(v, 99, "x", 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(e2));
    TEST_ASSERT_STR_EQ(ray_err_code(e2), "range");

    ray_release(v);
    PASS();
}

/* ---- str_vec_compact: nulls + no-dead early return + non-STR guard ----- */

static test_result_t test_str_vec_compact_paths(void) {
    /* Build a STR vec with pooled strings, set one null, overwrite another
     * (creating dead bytes), then compact.  Exercises ray_str_vec_compact's
     * null-skip (lines 1197, 1218) and live-rewrite paths. */
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "pooled_alpha_string_one_here!!!!", 32);
    v = ray_str_vec_append(v, "pooled_beta_string_two_here!!!!!", 32);
    v = ray_str_vec_append(v, "pooled_gamma_string_three_here!!", 32);
    v = ray_str_vec_append(v, "inline", 6);
    TEST_ASSERT_EQ_I(v->len, 4);

    /* set element 0 → creates dead bytes for the old pooled string. */
    v = ray_str_vec_set(v, 0, "tiny0", 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* set element 1 null (STR is nullable). */
    ray_vec_set_null(v, 1, true);  /* STR: set_null -> canonical "" */
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 1));

    /* compact reclaims dead bytes; null + inline elements are skipped. */
    v = ray_str_vec_compact(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 4);

    /* element 2 (the only surviving pooled string) must still read back. */
    size_t l = 0;
    const char* s2 = ray_str_vec_get(v, 2, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 32);
    TEST_ASSERT_MEM_EQ(32, s2, "pooled_gamma_string_three_here!!");
    /* element 1 remains the canonical empty/null after compact. */
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 1));

    /* compact with no dead bytes is a no-op early return (line 1182). */
    ray_t* same = ray_str_vec_compact(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(same));

    /* compact on non-STR → type error. */
    ray_t* iv = ray_vec_from_raw(RAY_I64, (int64_t[]){1}, 1);
    ray_t* e = ray_str_vec_compact(iv);
    TEST_ASSERT_TRUE(RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "type");
    ray_release(iv);

    ray_release(v);
    PASS();
}

/* ---- str_vec_compact: all-dead pool frees pool entirely ---------------- */

static test_result_t test_str_vec_compact_all_dead(void) {
    /* When every pooled string is overwritten/nulled, live_size==0 and the
     * pool is freed (lines 1201-1205). */
    ray_t* v = ray_vec_new(RAY_STR, 2);
    v = ray_str_vec_append(v, "first_pooled_to_be_killed_now!!!", 32);
    v = ray_str_vec_append(v, "second_pooled_also_dead_soon!!!!", 32);
    TEST_ASSERT_EQ_I(v->len, 2);

    /* Replace both with inline strings → all pooled bytes dead. */
    v = ray_str_vec_set(v, 0, "ii0", 3);
    v = ray_str_vec_set(v, 1, "ii1", 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    v = ray_str_vec_compact(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->len, 2);
    TEST_ASSERT_NULL(v->str_pool);  /* pool freed */

    size_t l = 0;
    const char* s0 = ray_str_vec_get(v, 0, &l);
    TEST_ASSERT_EQ_I((int64_t)l, 3);
    TEST_ASSERT_MEM_EQ(3, s0, "ii0");

    ray_release(v);
    PASS();
}

/* ---- copy_nulls: non-slice source, no-nulls, length mismatch ----------- */

static test_result_t test_vec_copy_nulls_paths(void) {
    /* NULL guard (line 1290). */
    int64_t raw[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_EQ_I(ray_vec_copy_nulls(NULL, v), RAY_ERR_TYPE);
    TEST_ASSERT_EQ_I(ray_vec_copy_nulls(v, NULL), RAY_ERR_TYPE);

    /* Source with no nulls → early RAY_OK (lines 1299-1301). */
    ray_t* dst = ray_vec_new(RAY_I64, 3);
    int64_t z = 0;
    for (int i = 0; i < 3; i++) dst = ray_vec_append(dst, &z);
    TEST_ASSERT_EQ_I(ray_vec_copy_nulls(dst, v), RAY_OK);
    TEST_ASSERT_FALSE(ray_vec_is_null(dst, 0));

    /* Non-slice source with nulls → copy loop (lines 1303-1308). */
    ray_t* src = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_vec_set_null(src, 1, true);
    TEST_ASSERT_EQ_I(ray_vec_copy_nulls(dst, src), RAY_OK);
    TEST_ASSERT_FALSE(ray_vec_is_null(dst, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(dst, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(dst, 2));

    /* Length-mismatch: dst shorter than src → loop bounded by min len. */
    ray_t* dst2 = ray_vec_new(RAY_I64, 2);
    for (int i = 0; i < 2; i++) dst2 = ray_vec_append(dst2, &z);
    TEST_ASSERT_EQ_I(ray_vec_copy_nulls(dst2, src), RAY_OK);
    TEST_ASSERT_TRUE(ray_vec_is_null(dst2, 1));

    ray_release(dst2);
    ray_release(src);
    ray_release(dst);
    ray_release(v);
    PASS();
}

/* ---- vec_get: slice path + STR/NULL guards ----------------------------- */

static test_result_t test_vec_get_paths(void) {
    /* NULL / error guards (line 301). */
    TEST_ASSERT_NULL(ray_vec_get(NULL, 0));

    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);

    /* Slice-path get with OOB on the slice (line 308 idx>=len). */
    ray_t* s = ray_vec_slice(v, 1, 2);  /* [20, 30] */
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    int64_t* p = (int64_t*)ray_vec_get(s, 0);
    TEST_ASSERT_EQ_I(*p, 20);
    TEST_ASSERT_NULL(ray_vec_get(s, -1));  /* idx < 0 on slice */
    TEST_ASSERT_NULL(ray_vec_get(s, 2));   /* idx >= slice len */
    ray_release(s);

    ray_release(v);
    PASS();
}

/* ---- sym_vec: W64 append + get_sym_id round-trip ----------------------- */

static test_result_t test_sym_vec_w64(void) {
    /* W64 path through append and get_sym_id (default width). */
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    int64_t ids[] = {1, 1000000, 5};
    for (int i = 0; i < 3; i++) v = ray_vec_append(v, &ids[i]);
    TEST_ASSERT_EQ_I(v->len, 3);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 0), 1);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 1), 1000000);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 2), 5);
    /* ray_vec_new(RAY_SYM, ...) delegates to ray_sym_vec_new(W64). */
    ray_t* v2 = ray_vec_new(RAY_SYM, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v2));
    TEST_ASSERT_EQ_I(v2->type, RAY_SYM);
    TEST_ASSERT_EQ_I(v2->attrs & RAY_SYM_W_MASK, RAY_SYM_W64);
    ray_release(v2);
    ray_release(v);
    PASS();
}

/* ---- concat: SYM same width fast path (memcpy) ------------------------- */

static test_result_t test_vec_concat_sym_same_width(void) {
    /* Same-width SYM concat takes the fast memcpy path (a_esz == b_esz),
     * NOT the element-by-element widen path. */
    ray_t* a = ray_sym_vec_new(RAY_SYM_W16, 2);
    uint16_t a0 = 100, a1 = 200;
    a = ray_vec_append(a, &a0);
    a = ray_vec_append(a, &a1);
    ray_t* b = ray_sym_vec_new(RAY_SYM_W16, 2);
    uint16_t b0 = 300, b1 = 400;
    b = ray_vec_append(b, &b0);
    b = ray_vec_append(b, &b1);

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->len, 4);
    TEST_ASSERT_EQ_I(c->attrs & RAY_SYM_W_MASK, RAY_SYM_W16);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(c, 0), 100);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(c, 3), 400);

    ray_release(a);
    ray_release(b);
    ray_release(c);
    PASS();
}

/* ---- insert_many: broadcast atom + GUID atom paths --------------------- */

static test_result_t test_vec_insert_many_atom(void) {
    /* Atom broadcast (vals->type < 0): insert the same I64 atom at 2 spots. */
    int64_t base_raw[] = {1, 2, 3};
    ray_t* base = ray_vec_from_raw(RAY_I64, base_raw, 3);

    ray_t* idxs = ray_vec_from_raw(RAY_I64, (int64_t[]){0, 3}, 2);
    ray_t* atom = ray_i64(777);  /* negative type → broadcast atom */
    TEST_ASSERT_FALSE(RAY_IS_ERR(atom));

    ray_t* r = ray_vec_insert_many(base, idxs, atom);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->len, 5);  /* [777,1,2,3,777] */
    int64_t* d = (int64_t*)ray_data(r);
    TEST_ASSERT_EQ_I(d[0], 777);
    TEST_ASSERT_EQ_I(d[1], 1);
    TEST_ASSERT_EQ_I(d[4], 777);

    ray_release(r);
    ray_release(atom);
    ray_release(idxs);

    /* Atom type-mismatch: I32 base with I64 atom → type error. */
    int32_t b32[] = {1, 2};
    ray_t* base32 = ray_vec_from_raw(RAY_I32, b32, 2);
    ray_t* idx1 = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    ray_t* atom64 = ray_i64(5);
    ray_t* e = ray_vec_insert_many(base32, idx1, atom64);
    TEST_ASSERT_TRUE(RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "type");
    ray_release(atom64);
    ray_release(idx1);
    ray_release(base32);

    ray_release(base);
    PASS();
}

/* ---- insert_many: NULL / error passthrough guards ---------------------- */

static test_result_t test_vec_insert_many_guards(void) {
    int64_t raw[] = {1, 2};
    ray_t* base = ray_vec_from_raw(RAY_I64, raw, 2);
    ray_t* idxs = ray_vec_from_raw(RAY_I64, (int64_t[]){0}, 1);
    ray_t* vals = ray_vec_from_raw(RAY_I64, (int64_t[]){9}, 1);

    /* vec NULL → returns vec (NULL) */
    TEST_ASSERT_NULL(ray_vec_insert_many(NULL, idxs, vals));
    /* idxs NULL → returns idxs (NULL) */
    TEST_ASSERT_NULL(ray_vec_insert_many(base, NULL, vals));
    /* vals NULL → returns vals (NULL) */
    TEST_ASSERT_NULL(ray_vec_insert_many(base, idxs, NULL));

    ray_release(vals);
    ray_release(idxs);
    ray_release(base);
    PASS();
}

/* ---- from_raw: SYM default-W64 and LIST/TABLE guard -------------------- */

static test_result_t test_vec_from_raw_sym(void) {
    /* RAY_SYM from_raw defaults to W64 (line 801). */
    int64_t ids[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_SYM, ids, 3);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->type, RAY_SYM);
    TEST_ASSERT_EQ_I(v->attrs & RAY_SYM_W_MASK, RAY_SYM_W64);
    TEST_ASSERT_EQ_I(ray_vec_get_sym_id(v, 1), 2);
    ray_release(v);

    /* GUID from_raw (16-byte elements). */
    uint8_t guids[32];
    memset(guids, 0xAB, 16);
    memset(guids + 16, 0xCD, 16);
    ray_t* g = ray_vec_from_raw(RAY_GUID, guids, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(g));
    TEST_ASSERT_EQ_I(g->len, 2);
    const uint8_t* gd = (const uint8_t*)ray_data(g);
    TEST_ASSERT_EQ_I(gd[0], 0xAB);
    TEST_ASSERT_EQ_I(gd[16], 0xCD);
    ray_release(g);
    PASS();
}

/* ---- from_raw: sentinel nulls in raw input must set HAS_NULLS ---------- */

static test_result_t test_vec_from_raw_sentinel_nulls(void) {
    /* #445: a binding passing INT64_MIN through from_raw got a vector whose
     * payload held the null sentinel while HAS_NULLS stayed clear, so
     * ray_vec_is_null (gated on the attr for numeric types) denied the null
     * and it rendered as -9223372036854775808 — including after an IPC
     * round trip.  Construction must scan the raw payload. */
    int64_t iv[] = { 1, NULL_I64, 3 };
    ray_t* v = ray_vec_from_raw(RAY_I64, iv, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 1));

    /* Serialize/deserialize keeps both the flag and the null. */
    ray_t* wire = ray_ser(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(wire));
    ray_t* back = ray_de(wire);
    TEST_ASSERT_FALSE(RAY_IS_ERR(back));
    TEST_ASSERT_TRUE(back->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(ray_vec_is_null(back, 1));
    ray_release(back);
    ray_release(wire);
    ray_release(v);

    /* F64: any NaN bit pattern is the sentinel. */
    double fv[] = { 1.5, (double)NAN, 3.5 };
    ray_t* f = ray_vec_from_raw(RAY_F64, fv, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(f));
    TEST_ASSERT_TRUE(f->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(ray_vec_is_null(f, 1));
    ray_t* fwire = ray_ser(f);
    ray_t* fback = ray_de(fwire);
    TEST_ASSERT_FALSE(RAY_IS_ERR(fback));
    TEST_ASSERT_TRUE(ray_vec_is_null(fback, 1));
    ray_release(fback);
    ray_release(fwire);
    ray_release(f);

    /* I32/DATE width, I16, and GUID all-zero sentinel. */
    int32_t dv[] = { 100, NULL_I32 };
    ray_t* d = ray_vec_from_raw(RAY_DATE, dv, 2);
    TEST_ASSERT_TRUE(d->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(ray_vec_is_null(d, 1));
    ray_release(d);

    int16_t sv[] = { 7, NULL_I16 };
    ray_t* s = ray_vec_from_raw(RAY_I16, sv, 2);
    TEST_ASSERT_TRUE(s->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(ray_vec_is_null(s, 1));
    ray_release(s);

    uint8_t gv[32];
    memset(gv, 0xAB, 16);
    memset(gv + 16, 0, 16);
    ray_t* g = ray_vec_from_raw(RAY_GUID, gv, 2);
    TEST_ASSERT_TRUE(g->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_FALSE(ray_vec_is_null(g, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(g, 1));
    ray_release(g);

    /* Clean input must NOT pick up the flag. */
    int64_t cv[] = { 1, 2, 3 };
    ray_t* c = ray_vec_from_raw(RAY_I64, cv, 3);
    TEST_ASSERT_FALSE(c->attrs & RAY_ATTR_HAS_NULLS);
    ray_release(c);
    PASS();
}

/* ---- slice: NULL/error passthrough + zero-length ----------------------- */

static test_result_t test_vec_slice_guards(void) {
    /* NULL passthrough (line 323). */
    TEST_ASSERT_NULL(ray_vec_slice(NULL, 0, 0));

    int64_t raw[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);

    /* Negative offset / len → range error (line 324). */
    ray_t* e1 = ray_vec_slice(v, -1, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(e1));
    ray_t* e2 = ray_vec_slice(v, 0, -1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(e2));
    ray_t* e3 = ray_vec_slice(v, 4, 0);  /* offset > len */
    TEST_ASSERT_TRUE(RAY_IS_ERR(e3));

    /* Zero-length slice at a valid offset is OK. */
    ray_t* z = ray_vec_slice(v, 1, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(z));
    TEST_ASSERT_EQ_I(z->len, 0);
    ray_release(z);

    ray_release(v);
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

/* A bulk append must not round payload and then let the heap round the
 * header-inclusive allocation a second time. Compare allocation capacity,
 * not a process-wide high-water mark or whole-query incidental objects. */
static test_result_t test_vec_growth_raw_block_sizing(void) {
    const int64_t n = 333004;
    ray_t* source = ray_vec_new(RAY_I64, n);
    ray_t* dst = ray_vec_new(RAY_I64, 0);
    ray_t* fresh = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_NOT_NULL(dst);
    TEST_ASSERT_NOT_NULL(fresh);
    TEST_ASSERT_FALSE(RAY_IS_ERR(source));
    TEST_ASSERT_FALSE(RAY_IS_ERR(dst));
    TEST_ASSERT_FALSE(RAY_IS_ERR(fresh));
    source->len = n;
    int64_t* values = (int64_t*)ray_data(source);
    for (int64_t i = 0; i < n; i++) values[i] = i - 12345;
    dst = ray_vec_append_raw(dst, values, n);
    TEST_ASSERT_NOT_NULL(dst);
    TEST_ASSERT_FALSE(RAY_IS_ERR(dst));
    const size_t grown_bytes = ray_block_data_bytes(dst);
    const size_t fresh_bytes = ray_block_data_bytes(fresh);
    const bool values_ok = dst->len == n &&
        memcmp(ray_data(dst), values, (size_t)n * sizeof(int64_t)) == 0;
    const bool buddy = !ray_is_direct(dst) && !ray_is_direct(fresh);
    ray_release(source);
    ray_release(dst);
    ray_release(fresh);
    TEST_ASSERT_TRUE(values_ok);
    TEST_ASSERT_TRUE(buddy);
    TEST_ASSERT_FMT(grown_bytes == fresh_bytes,
                    "grown payload capacity %zu, fresh %zu", grown_bytes, fresh_bytes);
    PASS();
}

/* Pure arithmetic checks never try to allocate the extreme sizes. */
static test_result_t test_vec_growth_arithmetic(void) {
    const size_t widths[] = { 1, 2, 4, 8, 16 };
    const size_t limit = ray_cell_growth_limit();
    size_t bytes = 123;
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        size_t w = widths[i];
        TEST_ASSERT_TRUE(ray_cell_growth_bytes(17, 23, w, &bytes));
        TEST_ASSERT_EQ_I(bytes, 40 * w);
        TEST_ASSERT_TRUE(ray_cell_growth_bytes(0, 0, w, &bytes));
        TEST_ASSERT_EQ_I(bytes, 0);
        TEST_ASSERT_FALSE(ray_cell_growth_bytes(-1, 1, w, &bytes));
        TEST_ASSERT_FALSE(ray_cell_growth_bytes(1, -1, w, &bytes));
        TEST_ASSERT_FALSE(ray_cell_growth_bytes(INT64_MAX, 1, w, &bytes));
        TEST_ASSERT_FALSE(ray_cell_growth_bytes((int64_t)(limit / w), 1, w, &bytes));
        for (unsigned k = 6; k <= 24; k++) {
            size_t payload = ((size_t)1 << k) - 32;
            size_t n = payload / w;
            TEST_ASSERT_TRUE(ray_cell_growth_bytes((int64_t)n, 0, w, &bytes));
            size_t request = 0;
            TEST_ASSERT_TRUE(ray_cell_growth_request(0, bytes, &request));
            TEST_ASSERT_EQ_I(request, bytes);
            TEST_ASSERT_EQ_I(ray_order_for_size(request), k);
        }
    }
    TEST_ASSERT_FALSE(ray_cell_growth_bytes(0, 1, 0, &bytes));
    TEST_ASSERT_FALSE(ray_cell_growth_bytes(INT64_MAX, 0, 16, &bytes));
    TEST_ASSERT_FALSE(ray_cell_growth_request(0, limit + 1, &bytes));
    TEST_ASSERT_FALSE(ray_cell_growth_request(limit + 1, 1, &bytes));
    size_t large = limit / 2 + 1;
    TEST_ASSERT_TRUE(ray_cell_growth_request(large, large + 1, &bytes));
    TEST_ASSERT_EQ_I(bytes, limit);
#if SIZE_MAX > UINT32_MAX
    large = (size_t)(UINT64_C(1) << 40); /* above the largest buddy order */
    TEST_ASSERT_TRUE(ray_cell_growth_request(large, large + 8, &bytes));
    TEST_ASSERT_EQ_I(bytes, large * 2);
#endif
    PASS();
}

/* Model every reserve boundary for a one-cell append sequence. The model
 * uses the allocator's documented size rules, not an elapsed-time threshold.
 * Removing direct doubling takes thousands of page-sized steps, not <=64. */
static test_result_t test_vec_growth_geometry(void) {
    const size_t target = (size_t)40 * 1024 * 1024;
    size_t room = 0, copied = 0;
    unsigned reserves = 0;
    while (room < target) {
        size_t required = room + sizeof(int64_t), request = 0;
        TEST_ASSERT_TRUE(ray_cell_growth_request(room, required, &request));
        copied += room;
        uint8_t order = ray_order_for_size(request);
        size_t next = order < RAY_HEAP_POOL_ORDER
            ? ((size_t)1 << order) - 32
            : ((request + RAY_DIRECT_HDR + 32 + 4095) & ~(size_t)4095) - RAY_DIRECT_HDR - 32;
        TEST_ASSERT_TRUE(next >= required);
        room = next;
        reserves++;
        TEST_ASSERT_TRUE(reserves <= 64);
    }
    TEST_ASSERT_TRUE(copied < 2 * room);
    PASS();
}

static test_result_t test_vec_growth_direct_reserve(void) {
    /* This size goes directly to a page-rounded mapping, not a buddy pool. */
    const int64_t n = ((int64_t)1 << 21) + 1024;
    ray_t* v = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    const size_t old_room = ray_block_data_bytes(v);
    const int64_t old_len = (int64_t)(old_room / sizeof(int64_t));
    v->len = old_len;
    int64_t* data = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < old_len; i++) data[i] = i ^ 0x55;
    int64_t first = -777;
    v = ray_vec_append_raw(v, &first, 1);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    const size_t new_room = ray_block_data_bytes(v);
    ray_t* after_reserve = v;
    bool stable = true;
    for (int64_t i = 0; i < 1024; i++) {
        v = ray_vec_append_raw(v, &i, 1);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        if (v != after_reserve) stable = false;
    }
    bool values_ok = v->len == old_len + 1025;
    data = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < old_len; i++)
        if (data[i] != (i ^ 0x55)) values_ok = false;
    if (data[old_len] != first) values_ok = false;
    for (int64_t i = 0; i < 1024; i++)
        if (data[old_len + 1 + i] != i) values_ok = false;
    ray_release(v);
    TEST_ASSERT_TRUE(new_room >= 2 * old_room);
    TEST_ASSERT_TRUE(stable);
    TEST_ASSERT_TRUE(values_ok);
    PASS();
}

/* Raw storage uses physical element widths, including all four SYM
 * encodings. The raw API does not translate domains or infer null hints. */
static test_result_t test_vec_growth_raw_widths(void) {
    const int8_t types[] = { RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_F32, RAY_F64, RAY_GUID };
    const uint8_t sym_widths[] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64 };
    for (size_t k = 0; k < sizeof(types) / sizeof(types[0]) + sizeof(sym_widths); k++) {
        bool sym = k >= sizeof(types) / sizeof(types[0]);
        uint8_t sw = sym ? sym_widths[k - sizeof(types) / sizeof(types[0])] : 0;
        ray_t* v = sym ? ray_sym_vec_new(sw, 0) : ray_vec_new(types[k], 0);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        const int64_t n = 97;
        size_t width = ray_sym_elem_size(v->type, v->attrs);
        _Alignas(16) uint8_t raw[97 * 16];
        memset(raw, 0, sizeof raw);
        if (sym) {
            for (int64_t i = 0; i < n; i++)
                ray_write_sym(raw, i, (uint64_t)(i % 13), RAY_SYM, v->attrs);
        } else {
            for (size_t i = 0; i < (size_t)n * width; i++) raw[i] = (uint8_t)(i * 17 + 1);
        }
        uint8_t attrs = v->attrs;
        v = ray_vec_append_raw(v, raw, n);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        bool ok = v->len == n && (v->attrs & RAY_SYM_W_MASK) == (attrs & RAY_SYM_W_MASK) &&
            memcmp(ray_data(v), raw, (size_t)n * width) == 0;
        ray_release(v);
        TEST_ASSERT_TRUE(ok);
    }
    PASS();
}

/* Failed size checks/zero batches precede COW and index mutation. A real
 * growth detaches only the mutated copy's stale index; the retained old
 * snapshot stays usable. Raw callers still propagate nullable hints. */
static test_result_t test_vec_growth_indexed_snapshot(void) {
    const int64_t original[] = { 7, 3, 7, 9, 3, 1 };
    int64_t more[97];
    for (int64_t i = 0; i < 97; i++) more[i] = i == 48 ? NULL_I64 : i + 100;
    for (int order = 0; order < 2; order++) {
        ray_t* v = ray_vec_from_raw(RAY_I64, original, 6);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&v)));
        ray_t* old = v;
        ray_t* index = v->index;
        ray_retain(old);
        TEST_ASSERT_EQ_I(ray_atomic_load(&old->rc), 2);
        TEST_ASSERT(ray_vec_append_raw(v, NULL, 0) == v, "zero append must be an ownership no-op");
        ray_t* error = ray_vec_append_raw(v, more, INT64_MAX);
        bool rejected = error && RAY_IS_ERR(error);
        if (rejected) ray_error_free(error);
        TEST_ASSERT_TRUE(rejected);
        TEST_ASSERT_EQ_I(ray_atomic_load(&old->rc), 2);
        TEST_ASSERT_TRUE(ray_index_has(old));
        TEST_ASSERT(old->index == index, "failed reserve replaced index");
        v = ray_vec_append_raw(v, more, 97);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        v->attrs |= RAY_ATTR_HAS_NULLS; /* caller propagation, not raw append */
        bool ok = v != old && v->len == 103 && old->len == 6 &&
            !ray_index_has(v) && ray_index_has(old) && old->index == index &&
            memcmp(ray_data(old), original, sizeof original) == 0 &&
            memcmp(ray_data(v), original, sizeof original) == 0 &&
            memcmp((int64_t*)ray_data(v) + 6, more, sizeof more) == 0 &&
            ray_vec_is_null(v, 54);
        if (order) { ray_release(old); ray_release(v); }
        else       { ray_release(v); ray_release(old); }
        TEST_ASSERT_TRUE(ok);
    }
    PASS();
}

static test_result_t test_vec_growth_list_refs(void) {
    for (int order = 0; order < 2; order++) {
        ray_t* item = ray_i64(42);
        ray_t* v = ray_list_new(4);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        for (int i = 0; i < 4; i++) v = ray_list_append(v, item);
        ray_t* old = v;
        ray_retain(old);
        v = ray_list_insert_at(v, 1, item); /* COW and a full four-cell block */
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        for (int i = 0; i < 128; i++) {
            v = ray_list_append(v, item);
            TEST_ASSERT_NOT_NULL(v);
            TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        }
        bool ok = old != v && old->len == 4 && v->len == 133 &&
            ray_atomic_load(&item->rc) == 138;
        for (int64_t i = 0; i < old->len; i++)
            if (((ray_t**)ray_data(old))[i] != item) ok = false;
        for (int64_t i = 0; i < v->len; i++)
            if (((ray_t**)ray_data(v))[i] != item) ok = false;
        if (order) {
            ray_release(old);
            if (ray_atomic_load(&item->rc) != 134) ok = false;
            ray_release(v);
        } else {
            ray_release(v);
            if (ray_atomic_load(&item->rc) != 5) ok = false;
            ray_release(old);
        }
        if (ray_atomic_load(&item->rc) != 1) ok = false;
        ray_release(item);
        TEST_ASSERT_TRUE(ok);
    }
    PASS();
}

static bool growth_string_is(ray_t* v, int64_t i, const char* wanted) {
    size_t len = 0, n = strlen(wanted);
    const char* p = ray_str_vec_get(v, i, &len);
    return len == n && (n == 0 || (p && memcmp(p, wanted, n) == 0));
}

static test_result_t test_vec_growth_str_refs(void) {
    const char* a = "a-long-string-for-a-shared-pool";
    const char* b = "another-long-string-for-the-pool";
    for (int order = 0; order < 2; order++) {
        ray_t* v = ray_vec_new(RAY_STR, 2);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        v = ray_str_vec_append(v, a, strlen(a));
        v = ray_str_vec_append(v, b, strlen(b));
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        ray_t* old = v;
        ray_t* old_pool = v->str_pool;
        ray_retain(old);
        for (int i = 0; i < 33; i++) {
            const char* text = i % 3 == 0 ? "" : i % 3 == 1 ? "tiny" : a;
            v = ray_str_vec_append(v, text, strlen(text));
            TEST_ASSERT_NOT_NULL(v);
            TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        }
        bool ok = old != v && old->len == 2 && v->len == 35 &&
            old_pool != v->str_pool && ray_atomic_load(&old_pool->rc) == 1 &&
            ray_atomic_load(&v->str_pool->rc) == 1 &&
            growth_string_is(old, 0, a) && growth_string_is(old, 1, b) &&
            growth_string_is(v, 0, a) && growth_string_is(v, 1, b) &&
            !ray_vec_has_nulls(old) && ray_vec_has_nulls(v);
        for (int i = 0; i < 33; i++) {
            const char* text = i % 3 == 0 ? "" : i % 3 == 1 ? "tiny" : a;
            if (!growth_string_is(v, i + 2, text)) ok = false;
        }
        if (order) {
            ray_release(old);
            if (!growth_string_is(v, 0, a)) ok = false;
            ray_release(v);
        } else {
            ray_release(v);
            if (!growth_string_is(old, 1, b)) ok = false;
            ray_release(old);
        }
        TEST_ASSERT_TRUE(ok);
    }
    PASS();
}

/* ---- slice setter value semantics --------------------------------------- */

/* Success consumes one owner; failure leaves it with the caller. Keep that
 * protocol even on an unexpected error so the final assertion is leak-free. */
static bool set_slice_owned(ray_t** owner, int64_t row, const void* elem) {
    ray_t* result = ray_vec_set(*owner, row, elem);
    if (!result) return false;
    if (RAY_IS_ERR(result)) {
        ray_error_free(result);
        return false;
    }
    *owner = result;
    return true;
}

static bool set_slice_plain(ray_t* v) {
    return !(v->attrs & (RAY_ATTR_SLICE | RAY_ATTR_SORTED)) && !ray_index_has(v);
}

/* Use the actual find operator, not a model of sorted/hash lookup. */
static bool set_slice_find_keys(ray_t* v, ray_t* keys, int64_t a, int64_t b) {
    ray_t* found = ray_find_fn(v, keys);
    if (!found) return false;
    if (RAY_IS_ERR(found)) {
        ray_error_free(found);
        return false;
    }
    int64_t expected[] = {a, b};
    bool ok = found->type == RAY_I64 && found->len == 2 &&
        memcmp(ray_data(found), expected, sizeof expected) == 0;
    ray_release(found);
    return ok;
}

static test_result_t test_vec_set_slice_isolation(void) {
    const int64_t raw[] = {1, 2, 3};
    const int64_t probes[] = {2, 9};
    const int64_t changed[] = {9, 3};
    for (int mode = 0; mode < 3; mode++) {
        ray_t* parent = ray_vec_from_raw(RAY_I64, raw, 3);
        TEST_ASSERT_NOT_NULL(parent);
        TEST_ASSERT_FALSE(RAY_IS_ERR(parent));
        parent->attrs |= RAY_ATTR_SORTED; /* truthful sorted fixture */
        TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&parent)));
        ray_t* index = parent->index;
        ray_t* keys = ray_vec_from_raw(RAY_I64, probes, 2);
        TEST_ASSERT_NOT_NULL(keys);
        TEST_ASSERT_FALSE(RAY_IS_ERR(keys));
        bool before = set_slice_find_keys(parent, keys, 1, NULL_I64);
        ray_t* outer = ray_vec_slice(parent, 0, 3);
        TEST_ASSERT_NOT_NULL(outer);
        TEST_ASSERT_FALSE(RAY_IS_ERR(outer));
        ray_t* view = mode == 2 ? ray_vec_slice(outer, 1, 2) :
                                 ray_vec_slice(parent, 1, 2);
        ray_t* overlap = ray_vec_slice(parent, 0, 2);
        TEST_ASSERT_NOT_NULL(overlap);
        TEST_ASSERT_FALSE(RAY_IS_ERR(overlap));
        TEST_ASSERT_NOT_NULL(view);
        TEST_ASSERT_FALSE(RAY_IS_ERR(view));
        /* Slices don't inherit parent accelerators. A separately sorted
         * slice is legal; detachment must discard its own marker too. */
        view->attrs |= RAY_ATTR_SORTED;
        ray_t* alias = NULL;
        if (mode == 1) { alias = view; ray_retain(alias); }
        int64_t nine = 9;
        bool wrote = set_slice_owned(&view, 0, &nine);
        bool ok = wrote && before && set_slice_plain(view) && view->len == 2 &&
            memcmp(ray_data(view), changed, sizeof changed) == 0 &&
            memcmp(ray_data(parent), raw, sizeof raw) == 0 &&
            memcmp(ray_data(outer), raw, sizeof raw) == 0 &&
            memcmp(ray_data(overlap), raw, 2 * sizeof(int64_t)) == 0 &&
            (parent->attrs & RAY_ATTR_SORTED) && ray_index_has(parent) &&
            parent->index == index;
        if (alias) ok = ok && (alias->attrs & RAY_ATTR_SORTED) &&
            memcmp(ray_data(alias), raw + 1, 2 * sizeof(int64_t)) == 0;
        /* Deliberately evaluate actual indexed find even if payload checks
         * failed: baseline's stale index otherwise masks the changed bytes. */
        bool parent_find = set_slice_find_keys(parent, keys, 1, NULL_I64);
        bool result_find = set_slice_find_keys(view, keys, NULL_I64, 0);
        ray_release(alias);
        ray_release(overlap);
        ray_release(outer);
        ray_release(parent);
        /* Result payload must remain readable after all old owners go. */
        ok = ok && memcmp(ray_data(view), changed, sizeof changed) == 0;
        ray_release(view);
        ray_release(keys);
        TEST_ASSERT_TRUE(ok && parent_find && result_find);
    }
    PASS();
}

static test_result_t test_vec_set_slice_backing_elem(void) {
    const int64_t raw[] = {11, 22, 33, 44};
    const int64_t expected[] = {44, 33};
    for (int shared = 0; shared < 2; shared++) {
        ray_t* parent = ray_vec_from_raw(RAY_I64, raw, 4);
        TEST_ASSERT_NOT_NULL(parent);
        TEST_ASSERT_FALSE(RAY_IS_ERR(parent));
        ray_t* view = ray_vec_slice(parent, 1, 2);
        TEST_ASSERT_NOT_NULL(view);
        TEST_ASSERT_FALSE(RAY_IS_ERR(view));
        ray_t* alias = NULL;
        if (shared) { alias = view; ray_retain(alias); }
        /* elem is outside the slice, but inside its backing allocation.
         * In the unique case the slice is now the last owner of parent. */
        const void* elem = ray_vec_get(parent, 3);
        ray_release(parent);
        bool wrote = set_slice_owned(&view, 0, elem);
        bool ok = wrote && set_slice_plain(view) && view->len == 2 &&
            memcmp(ray_data(view), expected, sizeof expected) == 0;
        if (alias) ok = ok &&
            memcmp(ray_data(alias), raw + 1, 2 * sizeof(int64_t)) == 0;
        ray_release(alias);
        ok = ok && memcmp(ray_data(view), expected, sizeof expected) == 0;
        ray_release(view);
        TEST_ASSERT_TRUE(ok);
    }
    PASS();
}

static test_result_t test_vec_set_slice_primitive_nulls(void) {
    const int8_t types[] = {RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64,
        RAY_F32, RAY_F64, RAY_DATE, RAY_TIME, RAY_TIMESTAMP, RAY_TIMESPAN, RAY_GUID};
    for (size_t c = 0; c < sizeof types / sizeof types[0]; c++) {
        int8_t type = types[c];
        size_t width = ray_elem_size(type);
        /* All typed reads are from allocated vectors. These byte arrays
         * are only memcpy/memcmp inputs, never cast to wider pointers. */
        uint8_t raw[48], replacement[16], snapshot[48];
        memset(raw, type == RAY_BOOL ? 1 : 0x11, sizeof raw);
        memset(replacement, type == RAY_BOOL ? 0 : 0x22, sizeof replacement);
        bool nullable = type != RAY_BOOL && type != RAY_U8;
        for (int insert_null = 0; insert_null < (nullable ? 2 : 1); insert_null++) {
            ray_t* parent = ray_vec_from_raw(type, raw, 3);
            TEST_ASSERT_NOT_NULL(parent);
            TEST_ASSERT_FALSE(RAY_IS_ERR(parent));
            ray_t* null_value = ray_vec_from_raw(type, raw, 1);
            TEST_ASSERT_NOT_NULL(null_value);
            TEST_ASSERT_FALSE(RAY_IS_ERR(null_value));
            if (nullable) {
                TEST_ASSERT_EQ_I(ray_vec_set_null_checked(null_value, 0, true), RAY_OK);
                if (!insert_null)
                    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(parent, 1, true), RAY_OK);
            }
            memcpy(snapshot, ray_data(parent), 3 * width);
            uint8_t parent_attrs = parent->attrs;
            ray_t* view = ray_vec_slice(parent, 1, 1);
            TEST_ASSERT_NOT_NULL(view);
            TEST_ASSERT_FALSE(RAY_IS_ERR(view));
            const void* value = insert_null ? ray_data(null_value) : replacement;
            bool wrote = set_slice_owned(&view, 0, value);
            bool ok = wrote && set_slice_plain(view) && view->type == type && view->len == 1 &&
                memcmp(ray_data(view), value, width) == 0 &&
                memcmp(ray_data(parent), snapshot, 3 * width) == 0 &&
                parent->attrs == parent_attrs;
            if (nullable) ok = ok && (view->attrs & RAY_ATTR_HAS_NULLS) &&
                ray_vec_is_null(view, 0) == (insert_null != 0);
            ray_release(parent);
            ok = ok && memcmp(ray_data(view), value, width) == 0;
            ray_release(null_value);
            ray_release(view);
            TEST_ASSERT_TRUE(ok);
        }
    }
    PASS();
}

static void set_slice_sym_setup(void) {
    ray_heap_init();
    ray_sym_init();
}

static void set_slice_sym_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* A private refcounted dictionary exercises domain ownership without files
 * or the immortal runtime singleton masking a missing retain. Real mmap/cache
 * FILE-domain coverage remains separate from this bounded fixture. */
static test_result_t test_vec_set_slice_sym_widths(void) {
    const uint8_t widths[] = {RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64};
    for (size_t c = 0; c < sizeof widths / sizeof widths[0]; c++) {
        ray_sym_domain_t* domain = ray_sym_domain_new();
        TEST_ASSERT_NOT_NULL(domain);
        TEST_ASSERT_EQ_I(ray_sym_domain_intern(domain, "alpha", 5), 1);
        TEST_ASSERT_EQ_I(ray_sym_domain_intern(domain, "beta", 4), 2);
        TEST_ASSERT_EQ_I(ray_sym_domain_intern(domain, "gamma", 5), 3);
        ray_t* parent = ray_sym_vec_new(widths[c], 3);
        TEST_ASSERT_NOT_NULL(parent);
        TEST_ASSERT_FALSE(RAY_IS_ERR(parent));
        parent->len = 3; /* constructor reserves capacity only */
        parent->sym_domain = domain;
        ray_sym_domain_retain(domain);
        ray_write_sym(ray_data(parent), 0, 1, RAY_SYM, parent->attrs);
        ray_write_sym(ray_data(parent), 1, 0, RAY_SYM, parent->attrs);
        ray_write_sym(ray_data(parent), 2, 3, RAY_SYM, parent->attrs);
        parent->attrs |= RAY_ATTR_HAS_NULLS;
        uint8_t snapshot[24];
        size_t width = RAY_SYM_ELEM(parent->attrs);
        memcpy(snapshot, ray_data(parent), 3 * width);
        ray_t* view = ray_vec_slice(parent, 1, 2);
        TEST_ASSERT_NOT_NULL(view);
        TEST_ASSERT_FALSE(RAY_IS_ERR(view));
        ray_t* alias = view;
        ray_retain(alias);
        /* Source is an aligned SYM slot with the same physical width. */
        bool wrote = set_slice_owned(&view, 0, ray_vec_get(parent, 0));
        bool ok = wrote && set_slice_plain(view) && view->type == RAY_SYM && view->len == 2 &&
            (view->attrs & RAY_SYM_W_MASK) == widths[c] &&
            ray_sym_vec_domain(view) == domain && (view->attrs & RAY_ATTR_HAS_NULLS) &&
            !ray_vec_is_null(view, 0) && ray_vec_is_null(alias, 0) &&
            memcmp(ray_data(parent), snapshot, 3 * width) == 0 &&
            memcmp(ray_data(alias), snapshot + width, 2 * width) == 0 &&
            ray_read_sym(ray_data(view), 0, RAY_SYM, view->attrs) == 1 &&
            ray_read_sym(ray_data(view), 1, RAY_SYM, view->attrs) == 3;
        /* Also insert canonical null into a fresh tail slice. */
        ray_t* tail = ray_vec_slice(parent, 2, 1);
        TEST_ASSERT_NOT_NULL(tail);
        TEST_ASSERT_FALSE(RAY_IS_ERR(tail));
        bool nulled = set_slice_owned(&tail, 0, ray_vec_get(parent, 1));
        ok = ok && nulled && set_slice_plain(tail) && ray_vec_is_null(tail, 0) &&
            (tail->attrs & RAY_SYM_W_MASK) == widths[c] &&
            ray_sym_vec_domain(tail) == domain &&
            memcmp(ray_data(parent), snapshot, 3 * width) == 0;
        ray_release(tail);
        ray_release(alias);
        ray_release(parent);
        ray_sym_domain_release(domain);
        /* No external domain, parent or old view owner remains. Borrowed
         * atoms are not released; view alone must keep their domain alive. */
        ray_t* first = ray_sym_vec_cell(view, 0);
        ray_t* second = ray_sym_vec_cell(view, 1);
        ok = ok && first && second && strcmp(ray_str_ptr(first), "alpha") == 0 &&
            strcmp(ray_str_ptr(second), "gamma") == 0;
        ray_release(view);
        TEST_ASSERT_TRUE(ok);
    }
    PASS();
}

/* ---- #1035: append/raw/insert must materialize slice payloads ------------ */

/* Slice failures preserve the owner; ordinary controls below avoid any growth
 * after a successful COW. This does not redefine general growth-failure ownership.
 * Never inspect a consumed unique header; src may belong to its parent. */
static bool grow_slice_owned(ray_t** owner, int op, int64_t at,
                             const void* src, int64_t count) {
    ray_t* out = op == 0 ? ray_vec_append(*owner, src) : op == 1 ?
        ray_vec_append_raw(*owner, src, count) : ray_vec_insert_at(*owner, at, src);
    if (!out) return false;
    if (RAY_IS_ERR(out)) { ray_error_free(out); return false; }
    *owner = out;
    return true;
}

static bool grow_slice_values(ray_t* v, const int64_t* values, int64_t n) {
    return v->type == RAY_I64 && v->len == n &&
        memcmp(ray_data(v), values, (size_t)n * sizeof(*values)) == 0;
}

static test_result_t test_vec_grow_slice_isolation(void) {
    const int64_t raw[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const int64_t probes[] = {3, 9}, extra[] = {9, 10, 11};
    for (int op = 0; op < 3; op++) for (int mode = 0; mode < 3; mode++)
    for (int at = 0; at <= (op == 2 ? 2 : 0); at++) {
        ray_t* parent = ray_vec_from_raw(RAY_I64, raw, 8);
        TEST_ASSERT_NOT_NULL(parent);
        TEST_ASSERT_FALSE(RAY_IS_ERR(parent));
        parent->attrs |= RAY_ATTR_SORTED;
        ray_t* attached = ray_index_attach_hash(&parent);
        bool ok = attached && !RAY_IS_ERR(attached);
        if (attached && RAY_IS_ERR(attached)) ray_error_free(attached);
        TEST_ASSERT_TRUE(ok);
        ray_t* index = parent->index;
        uint8_t attrs = parent->attrs;
        ray_t* keys = ray_vec_from_raw(RAY_I64, probes, 2);
        ray_t* outer = ray_vec_slice(parent, 0, 6);
        ray_t* view = ray_vec_slice(mode == 2 ? outer : parent, 1, 2);
        ray_t* overlap = ray_vec_slice(parent, 2, 3);
        TEST_ASSERT_NOT_NULL(view);
        TEST_ASSERT_FALSE(RAY_IS_ERR(view));
        view->attrs |= RAY_ATTR_SORTED;
        ray_t* alias = mode == 1 ? view : NULL;
        if (alias) ray_retain(alias);
        int64_t expected[5] = {2, 3, 9, 10, 11};
        int64_t n = op == 1 ? 5 : 3;
        if (op == 2) {
            for (int i = 0; i < 3; i++)
                expected[i] = i < at ? raw[i + 1] : i == at ? 9 : raw[i];
        }
        bool wrote = grow_slice_owned(&view, op, at, extra, 3);
        ok = ok && wrote && set_slice_plain(view) && grow_slice_values(view, expected, n) &&
            grow_slice_values(parent, raw, 8) && grow_slice_values(outer, raw, 6) &&
            grow_slice_values(overlap, raw + 2, 3) && parent->attrs == attrs &&
            parent->index == index && ray_index_has(parent);
        if (alias) ok = ok && grow_slice_values(alias, raw + 1, 2) &&
            (alias->attrs & RAY_ATTR_SORTED);
        bool indexed = set_slice_find_keys(parent, keys, 2, NULL_I64);
        ray_t* dropped = ray_index_drop(&parent);
        bool drop_ok = dropped && !RAY_IS_ERR(dropped);
        if (dropped && RAY_IS_ERR(dropped)) ray_error_free(dropped);
        parent->attrs &= ~RAY_ATTR_SORTED;
        bool plain = set_slice_find_keys(parent, keys, 2, NULL_I64);
        ray_release(alias); ray_release(overlap); ray_release(outer); ray_release(parent);
        ok = ok && grow_slice_values(view, expected, n);
        ray_release(view); ray_release(keys);
        TEST_ASSERT_TRUE(ok && indexed && drop_ok && plain);
    }
    PASS();
}

static test_result_t test_vec_grow_slice_backing_source(void) {
    int64_t raw[256];
    for (int i = 0; i < 256; i++) raw[i] = i + 100;
    for (int op = 0; op < 3; op++) for (int outside = 0; outside < 2; outside++)
    for (int shared = 0; shared < 2; shared++) {
        ray_t* parent = ray_vec_from_raw(RAY_I64, raw, 256);
        ray_t* view = ray_vec_slice(parent, 1, 2);
        ray_t* alias = shared ? view : NULL;
        if (alias) ray_retain(alias);
        int start = outside ? 140 : 1;
        const void* src = ray_vec_get(parent, start);
        int64_t expected[130] = {101, 102};
        int count = op == 1 && outside ? 100 : 1;
        if (op == 2) { expected[0] = raw[start]; expected[1] = 101; expected[2] = 102; }
        else memcpy(expected + 2, raw + start, (size_t)count * sizeof(int64_t));
        /* View is the last parent owner in the unique case. Raw 100 crosses
         * header allocation slack; its outside source is disjoint even on
         * baseline, and all baseline destination writes stay in parent. */
        ray_release(parent);
        bool wrote = grow_slice_owned(&view, op, 0, src, count);
        bool ok = wrote && set_slice_plain(view) &&
            grow_slice_values(view, expected, 2 + count);
        if (alias) ok = ok && grow_slice_values(alias, raw + 1, 2);
        ray_release(alias);
        ok = ok && grow_slice_values(view, expected, 2 + count);
        ray_release(view);
        TEST_ASSERT_TRUE(ok);
    }
    PASS();
}

static test_result_t test_vec_grow_slice_zero_raw(void) {
    const int64_t raw[] = {1, 2, 3};
    for (int shared = 0; shared < 2; shared++) for (int empty = 0; empty < 2; empty++) {
        ray_t* parent = ray_vec_from_raw(RAY_I64, raw, 3);
        ray_t* view = ray_vec_slice(parent, empty ? 3 : 1, empty ? 0 : 2);
        if (shared) ray_retain(view);
        uint32_t rc = ray_atomic_load(&view->rc);
        uint8_t attrs = view->attrs;
        ray_t* result = ray_vec_append_raw(view, NULL, 0);
        bool ok = result == view && ray_atomic_load(&view->rc) == rc &&
            view->attrs == attrs && view->len == (empty ? 0 : 2) &&
            grow_slice_values(parent, raw, 3);
        /* The specified no-op returns a borrowed identical pointer. */
        if (result && RAY_IS_ERR(result)) ray_error_free(result);
        else if (result && result != view) ray_release(result);
        if (shared) ray_release(view);
        ray_release(view); ray_release(parent);
        TEST_ASSERT_TRUE(ok);
    }
    PASS();
}

static test_result_t test_vec_grow_slice_hints_controls(void) {
    const int64_t raw[] = {10, 20, 30}, extra = 40;
    for (int op = 0; op < 3; op++) for (int hint = 0; hint < 3; hint++)
    for (int empty = 0; empty < 2; empty++) {
        ray_t* parent = ray_vec_from_raw(RAY_I64, raw, 3);
        if (hint == 1) parent->attrs |= RAY_ATTR_HAS_NULLS;
        ray_t* view = hint == 0 ? ray_vec_from_raw(RAY_I64, raw + 1, empty ? 0 : 1) :
            ray_vec_slice(parent, empty ? 3 : 1, empty ? 0 : 1);
        if (hint == 2) view->attrs |= RAY_ATTR_HAS_NULLS;
        /* Hints are conservative, including a parent-only hint added after
         * view construction. No raw sentinel scan is required by this test. */
        if (hint == 1) view->attrs &= ~RAY_ATTR_HAS_NULLS;
        uint8_t attrs = parent->attrs;
        bool wrote = grow_slice_owned(&view, op, empty ? 0 : 1, &extra, 1);
        const int64_t expected[] = {20, 40};
        bool ok = wrote && set_slice_plain(view) && grow_slice_values(view, empty ? expected + 1 : expected, empty ? 1 : 2) &&
            grow_slice_values(parent, raw, 3) && parent->attrs == attrs &&
            (!hint || (view->attrs & RAY_ATTR_HAS_NULLS));
        ray_release(view); ray_release(parent);
        TEST_ASSERT_TRUE(ok);
    }
    /* A fresh scalar result records its inserted sentinel without a scan. */
    for (int op = 0; op <= 2; op += 2) {
        ray_t* parent = ray_vec_from_raw(RAY_I64, raw, 3);
        ray_t* view = ray_vec_slice(parent, 1, 1);
        const int64_t nil = NULL_I64;
        bool wrote = grow_slice_owned(&view, op, op == 2 ? 0 : 1, &nil, 1);
        bool ok = wrote && set_slice_plain(view) && view->len == 2 &&
            (view->attrs & RAY_ATTR_HAS_NULLS) &&
            ray_vec_is_null(view, op == 2 ? 0 : 1) &&
            grow_slice_values(parent, raw, 3);
        ray_release(view); ray_release(parent);
        TEST_ASSERT_TRUE(ok);
    }
    PASS();
}

static test_result_t test_vec_grow_slice_sym_widths(void) {
    const uint8_t widths[] = {RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64};
    for (size_t w = 0; w < sizeof widths; w++) for (int op = 0; op < 3; op++)
    for (int null_source = 0; null_source < 2; null_source++) {
        ray_sym_domain_t* domain = ray_sym_domain_new();
        TEST_ASSERT_NOT_NULL(domain);
        TEST_ASSERT_EQ_I(ray_sym_domain_intern(domain, "alpha", 5), 1);
        TEST_ASSERT_EQ_I(ray_sym_domain_intern(domain, "beta", 4), 2);
        ray_t* parent = ray_sym_vec_new(widths[w], 4);
        TEST_ASSERT_NOT_NULL(parent);
        TEST_ASSERT_FALSE(RAY_IS_ERR(parent));
        parent->len = 4;
        parent->sym_domain = domain;
        ray_sym_domain_retain(domain);
        const uint64_t ids[] = {1, 2, 1, 0};
        for (int i = 0; i < 4; i++)
            ray_write_sym(ray_data(parent), i, ids[i], RAY_SYM, parent->attrs);
        size_t width = RAY_SYM_ELEM(parent->attrs);
        uint8_t snapshot[32];
        memcpy(snapshot, ray_data(parent), 4 * width);
        ray_t* view = ray_vec_slice(parent, 1, 1);
        ray_t* alias = view;
        ray_retain(alias);
        /* Raw callers, not append_raw, propagate source sentinel hints. */
        if (op == 1 && null_source) view->attrs |= RAY_ATTR_HAS_NULLS;
        const void* src = ray_vec_get(parent, null_source ? 3 : 0);
        bool wrote = grow_slice_owned(&view, op, 0, src, 1);
        int64_t expected[] = {2, null_source ? 0 : 1};
        if (op == 2) { expected[1] = 2; expected[0] = null_source ? 0 : 1; }
        bool ok = wrote && set_slice_plain(view) && view->type == RAY_SYM && view->len == 2 &&
            (view->attrs & RAY_SYM_W_MASK) == widths[w] && ray_sym_vec_domain(view) == domain &&
            ray_read_sym(ray_data(view), 0, RAY_SYM, view->attrs) == expected[0] &&
            ray_read_sym(ray_data(view), 1, RAY_SYM, view->attrs) == expected[1] &&
            (!null_source || ((view->attrs & RAY_ATTR_HAS_NULLS) &&
                ray_vec_is_null(view, op == 2 ? 0 : 1))) &&
            memcmp(ray_data(parent), snapshot, 4 * width) == 0 &&
            alias->len == 1 && ray_read_sym(ray_data(alias), 0, RAY_SYM, alias->attrs) == 2;
        ray_release(alias); ray_release(parent); ray_sym_domain_release(domain);
        /* No external dictionary reference: detached result alone owns it. */
        ray_t* cell = ray_sym_vec_cell(view, op == 2 ? 1 : 0);
        ok = ok && cell && strcmp(ray_str_ptr(cell), "beta") == 0;
        if (!null_source) {
            cell = ray_sym_vec_cell(view, op == 2 ? 0 : 1);
            ok = ok && cell && strcmp(ray_str_ptr(cell), "alpha") == 0;
        }
        ray_release(view);
        TEST_ASSERT_TRUE(ok);
    }
    PASS();
}

/* Header-reuse equivalence, not a guarantee about arena payload lifetimes. */
static test_result_t test_vec_grow_ordinary_cow_modes(void) {
    bool saved_sync = ray_rc_sync;
    const int64_t raw[] = {1, 2}, appended[] = {1, 2, 9}, inserted[] = {1, 9, 2};
    const int64_t extra = 9;
    for (int sync = 0; sync < 2; sync++) for (int shared = 0; shared < 2; shared++)
    for (int op = 0; op < 3; op++) {
        ray_rc_sync = sync != 0;
        ray_t* v = ray_vec_from_raw(RAY_I64, raw, 2);
        bool ok = v && !RAY_IS_ERR(v);
        if (!ok) {
            if (v && RAY_IS_ERR(v)) ray_error_free(v);
            ray_rc_sync = saved_sync;
            TEST_ASSERT_TRUE(ok);
        }
        v->attrs |= RAY_ATTR_SORTED;
        ray_t* status = ray_index_attach_hash(&v);
        ok = status && !RAY_IS_ERR(status);
        if (status && RAY_IS_ERR(status)) ray_error_free(status);
        ray_t* alias = shared ? v : NULL;
        if (alias) ray_retain(alias);
        uintptr_t old_address = (uintptr_t)v, index_address = (uintptr_t)v->index;
        uint8_t attrs = v->attrs;
        /* len2 fits the original and copied block with one added cell, so a
         * successful shared COW cannot be followed by a second growth failure. */
        bool wrote = ok && grow_slice_owned(&v, op, 1, &extra, 1);
        ok = wrote && grow_slice_values(v, op == 2 ? inserted : appended, 3) &&
            set_slice_plain(v) && v->rc == 1 &&
            ((uintptr_t)v == old_address) == !shared;
        if (alias) ok = ok && alias->rc == 1 && grow_slice_values(alias, raw, 2) &&
            alias->attrs == attrs && (uintptr_t)alias->index == index_address;
        ray_release(alias); ray_release(v);
        ray_t arena_header = {0};
        arena_header.type = RAY_I64; arena_header.attrs = RAY_ATTR_ARENA;
        ok = ok && ray_cow_can_reuse_header(&arena_header) &&
            ray_cow(&arena_header) == &arena_header;
        ray_rc_sync = saved_sync;
        TEST_ASSERT_TRUE(ok);
    }
    PASS();
}

const test_entry_t vec_entries[] = {
    { "vec/grow_ordinary_cow_modes", test_vec_grow_ordinary_cow_modes, vec_setup, vec_teardown },
    { "vec/grow_slice_isolation", test_vec_grow_slice_isolation, vec_setup, vec_teardown },
    { "vec/grow_slice_backing_source", test_vec_grow_slice_backing_source, vec_setup, vec_teardown },
    { "vec/grow_slice_zero_raw", test_vec_grow_slice_zero_raw, vec_setup, vec_teardown },
    { "vec/grow_slice_hints_controls", test_vec_grow_slice_hints_controls, vec_setup, vec_teardown },
    { "vec/grow_slice_sym_widths", test_vec_grow_slice_sym_widths, set_slice_sym_setup, set_slice_sym_teardown },

    { "vec/set_slice_isolation", test_vec_set_slice_isolation, vec_setup, vec_teardown },
    { "vec/set_slice_backing_elem", test_vec_set_slice_backing_elem, vec_setup, vec_teardown },
    { "vec/set_slice_primitive_nulls", test_vec_set_slice_primitive_nulls, vec_setup, vec_teardown },
    { "vec/set_slice_sym_widths", test_vec_set_slice_sym_widths, set_slice_sym_setup, set_slice_sym_teardown },
    { "vec/growth_list_refs", test_vec_growth_list_refs, vec_setup, vec_teardown },
    { "vec/growth_str_refs", test_vec_growth_str_refs, vec_setup, vec_teardown },
    { "vec/growth_raw_widths", test_vec_growth_raw_widths, vec_setup, vec_teardown },
    { "vec/growth_indexed_snapshot", test_vec_growth_indexed_snapshot, vec_setup, vec_teardown },
    { "vec/growth_arithmetic", test_vec_growth_arithmetic, vec_setup, vec_teardown },
    { "vec/growth_geometry", test_vec_growth_geometry, vec_setup, vec_teardown },
    { "vec/growth_direct_reserve", test_vec_growth_direct_reserve, vec_setup, vec_teardown },
    { "vec/growth_raw_block_sizing", test_vec_growth_raw_block_sizing, vec_setup, vec_teardown },
    { "vec/new", test_vec_new, vec_setup, vec_teardown },
    { "vec/new_invalid", test_vec_new_invalid, vec_setup, vec_teardown },
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
    { "vec/null_large_release", test_vec_null_large_release, vec_setup, vec_teardown },
    { "vec/append_grow", test_vec_append_grow, vec_setup, vec_teardown },
    { "vec/type_correctness", test_vec_type_correctness, vec_setup, vec_teardown },
    { "vec/empty", test_vec_empty, vec_setup, vec_teardown },
    { "vec/bool", test_vec_bool, vec_setup, vec_teardown },
    { "vec/concat_type_mismatch", test_vec_concat_type_mismatch, vec_setup, vec_teardown },
    { "vec/slice_range", test_vec_slice_range, vec_setup, vec_teardown },
    { "vec/slice_null", test_vec_slice_null, vec_setup, vec_teardown },
    { "vec/concat_null", test_vec_concat_null, vec_setup, vec_teardown },
    { "vec/concat_slice_null", test_vec_concat_slice_null, vec_setup, vec_teardown },
    { "vec/f32_null_sentinel", test_vec_f32_null_sentinel, vec_setup, vec_teardown },
    { "vec/sym_vec_new_errors", test_sym_vec_new_errors, vec_setup, vec_teardown },
    { "vec/sym_vec_widths", test_sym_vec_widths, vec_setup, vec_teardown },
    { "vec/slice_of_slice", test_vec_slice_of_slice, vec_setup, vec_teardown },
    { "vec/concat_sym_widen", test_vec_concat_sym_widen, vec_setup, vec_teardown },
    { "vec/insert_at_shift_nulls", test_vec_insert_at_shift_nulls, vec_setup, vec_teardown },
    { "vec/insert_at_boundaries", test_vec_insert_at_boundaries, vec_setup, vec_teardown },
    { "vec/insert_many_coverage", test_vec_insert_many_coverage, vec_setup, vec_teardown },
    { "vec/insert_many_errors", test_vec_insert_many_errors, vec_setup, vec_teardown },
    { "vec/embedding_new", test_embedding_new, vec_setup, vec_teardown },
    { "vec/copy_nulls_slice_src", test_vec_copy_nulls_slice_src, vec_setup, vec_teardown },
    { "vec/str_null_insert_compact", test_str_vec_null_insert_compact, vec_setup, vec_teardown },
    { "vec/str_get_null_paths", test_str_vec_get_null_paths, vec_setup, vec_teardown },
    { "vec/from_raw_errors", test_vec_from_raw_errors, vec_setup, vec_teardown },
    { "vec/insert_many_sym_and_bc_null", test_vec_insert_many_sym_and_bc_null, vec_setup, vec_teardown },
    { "vec/sym_is_null_path", test_vec_sym_is_null_path, vec_setup, vec_teardown },
    { "vec/sentinel_all_types", test_vec_sentinel_all_types, vec_setup, vec_teardown },
    { "vec/guid_null_sentinel", test_vec_guid_null_sentinel, vec_setup, vec_teardown },
    { "vec/is_null_guards", test_vec_is_null_guards, vec_setup, vec_teardown },
    { "vec/set_null_checked_guards", test_vec_set_null_checked_guards, vec_setup, vec_teardown },
    { "vec/set_more_types", test_vec_set_more_types, vec_setup, vec_teardown },
    { "vec/append_guards", test_vec_append_guards, vec_setup, vec_teardown },
    { "vec/insert_at_grow", test_vec_insert_at_grow, vec_setup, vec_teardown },
    { "vec/insert_at_middle", test_vec_insert_at_middle, vec_setup, vec_teardown },
    { "vec/insert_vec_at_middle", test_vec_insert_vec_at_middle, vec_setup, vec_teardown },
    { "vec/insert_vec_at_middle_nulls", test_vec_insert_vec_at_middle_nulls, vec_setup, vec_teardown },
    { "vec/concat_list_retain", test_vec_concat_list_retain, vec_setup, vec_teardown },
    { "vec/concat_guid", test_vec_concat_guid, vec_setup, vec_teardown },
    { "vec/concat_guards", test_vec_concat_guards, vec_setup, vec_teardown },
    { "vec/concat_empty", test_vec_concat_empty, vec_setup, vec_teardown },
    { "vec/concat_str", test_vec_concat_str, vec_setup, vec_teardown },
    { "vec/concat_str_null", test_vec_concat_str_null, vec_setup, vec_teardown },
    { "vec/concat_text_null_scan", test_vec_concat_text_null_scan, vec_setup, vec_teardown },
    { "vec/concat_text_empty", test_vec_concat_text_empty, vec_setup, vec_teardown },
    { "vec/str_append_pool_grow", test_str_vec_append_pool_grow, vec_setup, vec_teardown },
    { "vec/str_set_paths", test_str_vec_set_paths, vec_setup, vec_teardown },
    { "vec/str_guards", test_str_vec_guards, vec_setup, vec_teardown },
    { "vec/str_insert_at_paths", test_str_vec_insert_at_paths, vec_setup, vec_teardown },
    { "vec/str_compact_paths", test_str_vec_compact_paths, vec_setup, vec_teardown },
    { "vec/str_compact_all_dead", test_str_vec_compact_all_dead, vec_setup, vec_teardown },
    { "vec/copy_nulls_paths", test_vec_copy_nulls_paths, vec_setup, vec_teardown },
    { "vec/get_paths", test_vec_get_paths, vec_setup, vec_teardown },
    { "vec/sym_vec_w64", test_sym_vec_w64, vec_setup, vec_teardown },
    { "vec/has_nulls_text", test_vec_has_nulls_text, vec_setup, vec_teardown },
    { "vec/concat_sym_same_width", test_vec_concat_sym_same_width, vec_setup, vec_teardown },
    { "vec/insert_many_atom", test_vec_insert_many_atom, vec_setup, vec_teardown },
    { "vec/insert_many_guards", test_vec_insert_many_guards, vec_setup, vec_teardown },
    { "vec/from_raw_sym", test_vec_from_raw_sym, vec_setup, vec_teardown },
    { "vec/from_raw_sentinel_nulls", test_vec_from_raw_sentinel_nulls, vec_setup, vec_teardown },
    { "vec/slice_guards", test_vec_slice_guards, vec_setup, vec_teardown },
    { NULL, NULL, NULL, NULL },
};

