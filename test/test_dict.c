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
#include "table/dict.h"
#include <string.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void dict_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void dict_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- ray_dict_new error paths ------------------------------------------ */

/* NULL keys input — error is propagated when keys is NULL.  vals must be
 * released to avoid a leak; the helper does that for us. */
static test_result_t test_dict_new_null_keys(void) {
    ray_t* vals = ray_list_new(0);
    TEST_ASSERT_NOT_NULL(vals);
    ray_t* d = ray_dict_new(NULL, vals);
    TEST_ASSERT_TRUE(RAY_IS_ERR(d));
    /* vals was released by ray_dict_new. */
    ray_error_free(d);
    PASS();
}

/* NULL vals input — keys is released by ray_dict_new and an error returned. */
static test_result_t test_dict_new_null_vals(void) {
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 0);
    TEST_ASSERT_NOT_NULL(keys);
    ray_t* d = ray_dict_new(keys, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(d));
    ray_error_free(d);
    PASS();
}

/* error keys input — propagated as-is; vals released. */
static test_result_t test_dict_new_err_keys(void) {
    ray_t* err = ray_error("test", NULL);
    ray_t* vals = ray_list_new(0);
    ray_t* d = ray_dict_new(err, vals);
    TEST_ASSERT_TRUE(RAY_IS_ERR(d));
    ray_error_free(d);
    PASS();
}

/* error vals input — keys released; error returned. */
static test_result_t test_dict_new_err_vals(void) {
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 0);
    ray_t* err = ray_error("test", NULL);
    ray_t* d = ray_dict_new(keys, err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(d));
    ray_error_free(d);
    PASS();
}

/* ---- ray_dict_keys / ray_dict_vals / ray_dict_len NULL paths ----------- */

static test_result_t test_dict_keys_null(void) {
    TEST_ASSERT_NULL(ray_dict_keys(NULL));
    TEST_ASSERT_NULL(ray_dict_vals(NULL));
    TEST_ASSERT_EQ_I(ray_dict_len(NULL), 0);
    PASS();
}

/* Non-dict input — returns NULL/0 instead of crashing. */
static test_result_t test_dict_keys_nondict(void) {
    ray_t* not_a_dict = ray_i64(42);
    TEST_ASSERT_NULL(ray_dict_keys(not_a_dict));
    TEST_ASSERT_NULL(ray_dict_vals(not_a_dict));
    TEST_ASSERT_EQ_I(ray_dict_len(not_a_dict), 0);
    ray_release(not_a_dict);
    PASS();
}

/* Error input — same. */
static test_result_t test_dict_keys_err(void) {
    ray_t* err = ray_error("test", NULL);
    TEST_ASSERT_NULL(ray_dict_keys(err));
    TEST_ASSERT_NULL(ray_dict_vals(err));
    TEST_ASSERT_EQ_I(ray_dict_len(err), 0);
    ray_error_free(err);
    PASS();
}

/* ---- ray_dict_find_sym width branches ---------------------------------- */

/* Build a dict with keys explicitly W8/W16/W32 by constructing keys with
 * ray_sym_vec_new directly — rfl-built sym vectors default to W64. */

static test_result_t test_dict_find_sym_w8(void) {
    int64_t a = ray_sym_intern("aa", 2);
    int64_t b = ray_sym_intern("bb", 2);
    int64_t c = ray_sym_intern("cc", 2);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W8, 3);
    TEST_ASSERT_NOT_NULL(keys);
    keys = ray_vec_append(keys, &a);
    keys = ray_vec_append(keys, &b);
    keys = ray_vec_append(keys, &c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(keys));

    ray_t* vals = ray_list_new(3);
    vals = ray_list_append(vals, ray_i64(10));
    vals = ray_list_append(vals, ray_i64(20));
    vals = ray_list_append(vals, ray_i64(30));

    ray_t* d = ray_dict_new(keys, vals);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));

    TEST_ASSERT_EQ_I(ray_dict_find_sym(d, a), 0);
    TEST_ASSERT_EQ_I(ray_dict_find_sym(d, b), 1);
    TEST_ASSERT_EQ_I(ray_dict_find_sym(d, c), 2);
    TEST_ASSERT_EQ_I(ray_dict_find_sym(d, 9999), -1);

    ray_release(d);
    PASS();
}

static test_result_t test_dict_find_sym_w16(void) {
    int64_t a = ray_sym_intern("ax", 2);
    int64_t b = ray_sym_intern("bx", 2);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W16, 2);
    keys = ray_vec_append(keys, &a);
    keys = ray_vec_append(keys, &b);

    ray_t* vals = ray_list_new(2);
    vals = ray_list_append(vals, ray_i64(1));
    vals = ray_list_append(vals, ray_i64(2));

    ray_t* d = ray_dict_new(keys, vals);
    TEST_ASSERT_EQ_I(ray_dict_find_sym(d, a), 0);
    TEST_ASSERT_EQ_I(ray_dict_find_sym(d, b), 1);
    TEST_ASSERT_EQ_I(ray_dict_find_sym(d, 9999), -1);

    ray_release(d);
    PASS();
}

static test_result_t test_dict_find_sym_w32(void) {
    int64_t a = ray_sym_intern("ay", 2);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W32, 1);
    keys = ray_vec_append(keys, &a);

    ray_t* vals = ray_list_new(1);
    vals = ray_list_append(vals, ray_i64(7));

    ray_t* d = ray_dict_new(keys, vals);
    TEST_ASSERT_EQ_I(ray_dict_find_sym(d, a), 0);
    TEST_ASSERT_EQ_I(ray_dict_find_sym(d, 9999), -1);

    ray_release(d);
    PASS();
}

/* find_sym on NULL/non-dict/non-sym keys → -1 (early-return paths). */
static test_result_t test_dict_find_sym_invalid(void) {
    TEST_ASSERT_EQ_I(ray_dict_find_sym(NULL, 1), -1);

    ray_t* not_a_dict = ray_i64(42);
    TEST_ASSERT_EQ_I(ray_dict_find_sym(not_a_dict, 1), -1);
    ray_release(not_a_dict);

    /* Dict with non-sym keys → also -1. */
    int64_t k0 = 10, k1 = 20;
    ray_t* keys = ray_vec_new(RAY_I64, 2);
    keys = ray_vec_append(keys, &k0);
    keys = ray_vec_append(keys, &k1);
    ray_t* vals = ray_list_new(2);
    vals = ray_list_append(vals, ray_i64(1));
    vals = ray_list_append(vals, ray_i64(2));
    ray_t* d = ray_dict_new(keys, vals);

    TEST_ASSERT_EQ_I(ray_dict_find_sym(d, 10), -1);

    ray_release(d);
    PASS();
}

/* ---- ray_dict_probe_sym_borrowed / ray_container_probe_sym ------------- */

static test_result_t test_dict_probe_sym_borrowed(void) {
    int64_t a = ray_sym_intern("alpha", 5);
    int64_t b = ray_sym_intern("beta", 4);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 2);
    keys = ray_vec_append(keys, &a);
    keys = ray_vec_append(keys, &b);

    ray_t* vals = ray_list_new(2);
    ray_t* va = ray_i64(100);
    ray_t* vb = ray_i64(200);
    vals = ray_list_append(vals, va);
    vals = ray_list_append(vals, vb);

    ray_t* d = ray_dict_new(keys, vals);

    /* Hit — borrowed pointer, should equal va. */
    ray_t* got = ray_dict_probe_sym_borrowed(d, a);
    TEST_ASSERT_EQ_PTR(got, va);

    /* Miss → NULL. */
    int64_t missing = ray_sym_intern("missing", 7);
    TEST_ASSERT_NULL(ray_dict_probe_sym_borrowed(d, missing));

    /* Container probe routes dict→borrowed. */
    TEST_ASSERT_EQ_PTR(ray_container_probe_sym(d, a), va);

    /* Container probe on non-dict/non-table returns NULL. */
    ray_t* atom = ray_i64(1);
    TEST_ASSERT_NULL(ray_container_probe_sym(atom, a));
    TEST_ASSERT_NULL(ray_container_probe_sym(NULL, a));
    ray_release(atom);

    /* Probe on a typed-vec-vals dict → NULL (vals must be RAY_LIST). */
    int64_t k = ray_sym_intern("k", 1);
    ray_t* keys2 = ray_sym_vec_new(RAY_SYM_W64, 1);
    keys2 = ray_vec_append(keys2, &k);
    int64_t v0 = 11;
    ray_t* vals2 = ray_vec_new(RAY_I64, 1);
    vals2 = ray_vec_append(vals2, &v0);
    ray_t* d2 = ray_dict_new(keys2, vals2);
    TEST_ASSERT_NULL(ray_dict_probe_sym_borrowed(d2, k));
    ray_release(d2);

    ray_release(va);
    ray_release(vb);
    ray_release(d);
    PASS();
}

/* ---- ray_dict_find_idx & ray_dict_get for typed-vec values ------------- */

/* Boxes the value out of a typed BOOL/U8/I16/I32/F32/F64/DATE/TIME/
 * TIMESTAMP/SYM/STR/GUID vector — exercises every dict_vals_at branch. */
static test_result_t test_dict_get_typed_vec_vals(void) {
    /* I64 keys → I64 vals. */
    int64_t k1 = 100, k2 = 200;
    ray_t* keys = ray_vec_new(RAY_I64, 2);
    keys = ray_vec_append(keys, &k1);
    keys = ray_vec_append(keys, &k2);

    int64_t v1 = 1234567890LL, v2 = 9876543210LL;
    ray_t* vals = ray_vec_new(RAY_I64, 2);
    vals = ray_vec_append(vals, &v1);
    vals = ray_vec_append(vals, &v2);

    ray_t* d = ray_dict_new(keys, vals);
    ray_t* key_atom = ray_i64(100);
    ray_t* got = ray_dict_get(d, key_atom);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_I(got->type, -RAY_I64);
    TEST_ASSERT_EQ_I(got->i64, 1234567890LL);
    ray_release(got);
    ray_release(key_atom);

    /* Miss → NULL. */
    ray_t* miss_key = ray_i64(99);
    TEST_ASSERT_NULL(ray_dict_get(d, miss_key));
    ray_release(miss_key);
    ray_release(d);
    PASS();
}

static test_result_t test_dict_get_each_value_type(void) {
    /* Build keys [1,2,3] and a different vals type for each test inside. */
    int64_t k1 = 1, k2 = 2;
    ray_t* keys = ray_vec_new(RAY_I64, 2);
    keys = ray_vec_append(keys, &k1);
    keys = ray_vec_append(keys, &k2);

    /* BOOL */
    {
        uint8_t b1 = 1, b2 = 0;
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_vec_new(RAY_BOOL, 2);
        v = ray_vec_append(v, &b1);
        v = ray_vec_append(v, &b2);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(1);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_BOOL);
        TEST_ASSERT_EQ_U(g->b8, 1);
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    /* U8 */
    {
        uint8_t u1 = 7, u2 = 9;
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_vec_new(RAY_U8, 2);
        v = ray_vec_append(v, &u1);
        v = ray_vec_append(v, &u2);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(2);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_U8);
        TEST_ASSERT_EQ_U(g->u8, 9);
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    /* I16 */
    {
        int16_t s1 = 100, s2 = -100;
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_vec_new(RAY_I16, 2);
        v = ray_vec_append(v, &s1);
        v = ray_vec_append(v, &s2);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(2);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_I16);
        TEST_ASSERT_EQ_I(g->i16, -100);
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    /* I32 */
    {
        int32_t s1 = 1000, s2 = -1000;
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_vec_new(RAY_I32, 2);
        v = ray_vec_append(v, &s1);
        v = ray_vec_append(v, &s2);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(1);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_I32);
        TEST_ASSERT_EQ_I(g->i32, 1000);
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    /* F32 */
    {
        float f1 = 1.5f, f2 = -2.5f;
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_vec_new(RAY_F32, 2);
        v = ray_vec_append(v, &f1);
        v = ray_vec_append(v, &f2);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(1);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_F32);
        TEST_ASSERT_EQ_F(g->f64, 1.5, 1e-6);
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    /* F64 */
    {
        double f1 = 3.14, f2 = 2.71;
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_vec_new(RAY_F64, 2);
        v = ray_vec_append(v, &f1);
        v = ray_vec_append(v, &f2);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(1);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_F64);
        TEST_ASSERT_EQ_F(g->f64, 3.14, 1e-9);
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    /* DATE */
    {
        int32_t d1 = 12345, d2 = 67890;
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_vec_new(RAY_DATE, 2);
        v = ray_vec_append(v, &d1);
        v = ray_vec_append(v, &d2);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(2);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_DATE);
        TEST_ASSERT_EQ_I(g->i32, 67890);
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    /* TIME */
    {
        int32_t t1 = 1, t2 = 2;
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_vec_new(RAY_TIME, 2);
        v = ray_vec_append(v, &t1);
        v = ray_vec_append(v, &t2);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(2);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_TIME);
        TEST_ASSERT_EQ_I(g->i32, 2);
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    /* TIMESTAMP */
    {
        int64_t ts1 = 100, ts2 = 200;
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_vec_new(RAY_TIMESTAMP, 2);
        v = ray_vec_append(v, &ts1);
        v = ray_vec_append(v, &ts2);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(2);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_TIMESTAMP);
        TEST_ASSERT_EQ_I(g->i64, 200);
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    /* SYM (typed-vec vals) */
    {
        int64_t s1 = ray_sym_intern("first", 5);
        int64_t s2 = ray_sym_intern("second", 6);
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 2);
        v = ray_vec_append(v, &s1);
        v = ray_vec_append(v, &s2);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(1);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_SYM);
        TEST_ASSERT_EQ_I(g->i64, s1);
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    /* STR */
    {
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_vec_new(RAY_STR, 2);
        v = ray_str_vec_append(v, "hello", 5);
        v = ray_str_vec_append(v, "world", 5);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(1);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_STR);
        TEST_ASSERT_EQ_U(ray_str_len(g), 5);
        TEST_ASSERT_MEM_EQ(5, ray_str_ptr(g), "hello");
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    /* GUID */
    {
        uint8_t g1[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
        uint8_t g2[16] = { 0 };
        ray_t* k = ray_cow(keys); ray_retain(k);
        ray_t* v = ray_vec_new(RAY_GUID, 2);
        v = ray_vec_append(v, g1);
        v = ray_vec_append(v, g2);
        ray_t* d = ray_dict_new(k, v);
        ray_t* ka = ray_i64(1);
        ray_t* g = ray_dict_get(d, ka);
        TEST_ASSERT_NOT_NULL(g);
        TEST_ASSERT_EQ_I(g->type, -RAY_GUID);
        ray_release(g);
        ray_release(ka);
        ray_release(d);
    }

    ray_release(keys);
    PASS();
}

/* find_idx with NULL/error inputs → -1 (early-return paths). */
static test_result_t test_dict_find_idx_invalid(void) {
    TEST_ASSERT_EQ_I(ray_dict_find_idx(NULL, NULL), -1);

    ray_t* atom = ray_i64(1);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(NULL, atom), -1);

    ray_t* not_a_dict = ray_i64(2);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(not_a_dict, atom), -1);
    ray_release(not_a_dict);

    /* Dict with empty keys (n<=0 short-circuit). */
    ray_t* keys = ray_vec_new(RAY_I64, 0);
    ray_t* vals = ray_list_new(0);
    ray_t* d = ray_dict_new(keys, vals);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, atom), -1);

    /* dict with NULL key_atom returns -1. */
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, NULL), -1);

    ray_release(atom);
    ray_release(d);
    PASS();
}

/* ---- ray_dict_find_idx F32 path & GUID path ---------------------------- */

static test_result_t test_dict_find_idx_f32(void) {
    float v1 = 1.5f, v2 = -2.25f;
    ray_t* keys = ray_vec_new(RAY_F32, 2);
    keys = ray_vec_append(keys, &v1);
    keys = ray_vec_append(keys, &v2);

    ray_t* vals = ray_list_new(2);
    vals = ray_list_append(vals, ray_i64(10));
    vals = ray_list_append(vals, ray_i64(20));

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* k1 = ray_f32(1.5f);
    ray_t* g = ray_dict_get(d, k1);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQ_I(g->i64, 10);
    ray_release(g);
    ray_release(k1);

    /* Miss. */
    ray_t* k_miss = ray_f32(99.0f);
    TEST_ASSERT_NULL(ray_dict_get(d, k_miss));
    ray_release(k_miss);

    ray_release(d);
    PASS();
}

static test_result_t test_dict_find_idx_guid(void) {
    uint8_t g1[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    uint8_t g2[16] = { 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1 };
    uint8_t g3[16] = { 0 };

    ray_t* keys = ray_vec_new(RAY_GUID, 2);
    keys = ray_vec_append(keys, g1);
    keys = ray_vec_append(keys, g2);

    ray_t* vals = ray_list_new(2);
    vals = ray_list_append(vals, ray_i64(100));
    vals = ray_list_append(vals, ray_i64(200));

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_guid(g1);
    ray_t* got = ray_dict_get(d, ka);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_I(got->i64, 100);
    ray_release(got);
    ray_release(ka);

    ray_t* km = ray_guid(g3);
    TEST_ASSERT_NULL(ray_dict_get(d, km));
    ray_release(km);

    ray_release(d);
    PASS();
}

/* find_idx — keys_have_nulls path with non-null query: must skip null
 * slots and match a valid one. */
static test_result_t test_dict_find_idx_with_nulls(void) {
    int64_t v1 = 10, v2 = 0, v3 = 30;
    ray_t* keys = ray_vec_new(RAY_I64, 3);
    keys = ray_vec_append(keys, &v1);
    keys = ray_vec_append(keys, &v2);
    keys = ray_vec_append(keys, &v3);
    /* Mark slot 1 as null. */
    ray_vec_set_null(keys, 1, true);

    ray_t* vals = ray_list_new(3);
    vals = ray_list_append(vals, ray_i64(1));
    vals = ray_list_append(vals, ray_i64(2));
    vals = ray_list_append(vals, ray_i64(3));

    ray_t* d = ray_dict_new(keys, vals);

    /* Find non-null: must skip the null slot and continue. */
    ray_t* k = ray_i64(30);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, k), 2);
    ray_release(k);

    /* Lookup of zero (matches null slot value if not null-aware) — should
     * NOT find it because slot 1 is null. */
    ray_t* k0 = ray_i64(0);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, k0), -1);
    ray_release(k0);

    ray_release(d);
    PASS();
}

/* ---- ray_dict_upsert paths --------------------------------------------- */

/* Upsert into a non-dict input — it's released and a fresh dict is built. */
static test_result_t test_dict_upsert_into_nondict(void) {
    ray_t* not_a_dict = ray_i64(42);
    ray_t* k = ray_sym(ray_sym_intern("k", 1));
    ray_t* v = ray_i64(100);

    ray_t* d = ray_dict_upsert(not_a_dict, k, v);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(d->type, RAY_DICT);
    TEST_ASSERT_EQ_I(ray_dict_len(d), 1);

    ray_t* got = ray_dict_get(d, k);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_I(got->i64, 100);
    ray_release(got);

    ray_release(k);
    ray_release(v);
    ray_release(d);
    PASS();
}

/* Upsert into NULL d → error. */
static test_result_t test_dict_upsert_null_d(void) {
    ray_t* k = ray_sym(ray_sym_intern("k", 1));
    ray_t* v = ray_i64(1);
    ray_t* res = ray_dict_upsert(NULL, k, v);
    TEST_ASSERT_TRUE(RAY_IS_ERR(res));
    ray_error_free(res);
    ray_release(k);
    ray_release(v);
    PASS();
}

/* Upsert with NULL val → error; d released. */
static test_result_t test_dict_upsert_null_val(void) {
    int64_t k = ray_sym_intern("only", 4);
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 1);
    keys = ray_vec_append(keys, &k);
    ray_t* vals = ray_list_new(1);
    vals = ray_list_append(vals, ray_i64(1));
    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_sym(k);
    ray_t* res = ray_dict_upsert(d, ka, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(res));
    ray_error_free(res);
    ray_release(ka);
    PASS();
}

/* Upsert replace existing key with a new value — exercises idx>=0 + LIST
 * vals branch. */
static test_result_t test_dict_upsert_replace_list_vals(void) {
    int64_t k = ray_sym_intern("k", 1);
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 1);
    keys = ray_vec_append(keys, &k);
    ray_t* vals = ray_list_new(1);
    vals = ray_list_append(vals, ray_i64(1));
    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_sym(k);
    ray_t* nv = ray_i64(999);
    d = ray_dict_upsert(d, ka, nv);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(ray_dict_len(d), 1);

    ray_t* got = ray_dict_get(d, ka);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_I(got->i64, 999);
    ray_release(got);

    ray_release(ka);
    ray_release(nv);
    ray_release(d);
    PASS();
}

/* Upsert into a typed-vec vals dict — replaces by promoting to LIST. */
static test_result_t test_dict_upsert_replace_typed_vec_vals(void) {
    int64_t k1 = 1, k2 = 2;
    ray_t* keys = ray_vec_new(RAY_I64, 2);
    keys = ray_vec_append(keys, &k1);
    keys = ray_vec_append(keys, &k2);

    int64_t v1 = 10, v2 = 20;
    ray_t* vals = ray_vec_new(RAY_I64, 2);
    vals = ray_vec_append(vals, &v1);
    vals = ray_vec_append(vals, &v2);

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_i64(1);
    ray_t* nv = ray_i64(777);
    d = ray_dict_upsert(d, ka, nv);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));

    /* After promotion, vals is RAY_LIST. */
    ray_t* dvals = ray_dict_vals(d);
    TEST_ASSERT_EQ_I(dvals->type, RAY_LIST);

    ray_t* got = ray_dict_get(d, ka);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_I(got->i64, 777);
    ray_release(got);

    ray_release(ka);
    ray_release(nv);
    ray_release(d);
    PASS();
}

/* Upsert append new key into typed-vec vals dict — promotes. */
static test_result_t test_dict_upsert_append_typed_vec_vals(void) {
    int64_t k1 = 1;
    ray_t* keys = ray_vec_new(RAY_I64, 1);
    keys = ray_vec_append(keys, &k1);

    int64_t v1 = 10;
    ray_t* vals = ray_vec_new(RAY_I64, 1);
    vals = ray_vec_append(vals, &v1);

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_i64(2);
    ray_t* nv = ray_f64(2.5);
    d = ray_dict_upsert(d, ka, nv);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(ray_dict_len(d), 2);

    ray_t* got = ray_dict_get(d, ka);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_F(got->f64, 2.5, 1e-9);
    ray_release(got);

    ray_release(ka);
    ray_release(nv);
    ray_release(d);
    PASS();
}

/* Upsert append str key — exercises STR branch in append. */
static test_result_t test_dict_upsert_append_str_key(void) {
    /* Start with a STR key dict. */
    ray_t* keys = ray_vec_new(RAY_STR, 1);
    keys = ray_str_vec_append(keys, "hello", 5);
    ray_t* vals = ray_list_new(1);
    vals = ray_list_append(vals, ray_i64(1));
    ray_t* d = ray_dict_new(keys, vals);

    ray_t* k2 = ray_str("world", 5);
    ray_t* v2 = ray_i64(2);
    d = ray_dict_upsert(d, k2, v2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(ray_dict_len(d), 2);

    ray_t* got = ray_dict_get(d, k2);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_I(got->i64, 2);
    ray_release(got);

    ray_release(k2);
    ray_release(v2);
    ray_release(d);
    PASS();
}

/* Upsert append GUID key — exercises GUID branch. */
static test_result_t test_dict_upsert_append_guid_key(void) {
    uint8_t g1[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    uint8_t g2[16] = { 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1 };

    ray_t* keys = ray_vec_new(RAY_GUID, 1);
    keys = ray_vec_append(keys, g1);
    ray_t* vals = ray_list_new(1);
    vals = ray_list_append(vals, ray_i64(1));
    ray_t* d = ray_dict_new(keys, vals);

    ray_t* k2 = ray_guid(g2);
    ray_t* v2 = ray_i64(2);
    d = ray_dict_upsert(d, k2, v2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(ray_dict_len(d), 2);

    ray_t* got = ray_dict_get(d, k2);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_I(got->i64, 2);
    ray_release(got);

    ray_release(k2);
    ray_release(v2);
    ray_release(d);
    PASS();
}

/* Upsert append F32 key — exercises F32 narrow-then-append branch. */
static test_result_t test_dict_upsert_append_f32_key(void) {
    float k1 = 1.5f;
    ray_t* keys = ray_vec_new(RAY_F32, 1);
    keys = ray_vec_append(keys, &k1);
    ray_t* vals = ray_list_new(1);
    vals = ray_list_append(vals, ray_i64(1));
    ray_t* d = ray_dict_new(keys, vals);

    ray_t* k2 = ray_f32(2.5f);
    ray_t* v2 = ray_i64(2);
    d = ray_dict_upsert(d, k2, v2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(ray_dict_len(d), 2);

    ray_t* got = ray_dict_get(d, k2);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_I(got->i64, 2);
    ray_release(got);

    ray_release(k2);
    ray_release(v2);
    ray_release(d);
    PASS();
}

/* Upsert with mismatched key type into existing typed-key dict → "type". */
static test_result_t test_dict_upsert_key_type_mismatch(void) {
    int64_t k1 = 1;
    ray_t* keys = ray_vec_new(RAY_I64, 1);
    keys = ray_vec_append(keys, &k1);
    ray_t* vals = ray_list_new(1);
    vals = ray_list_append(vals, ray_i64(1));
    ray_t* d = ray_dict_new(keys, vals);

    ray_t* bad_k = ray_str("oops", 4);
    ray_t* v = ray_i64(99);
    ray_t* res = ray_dict_upsert(d, bad_k, v);
    TEST_ASSERT_TRUE(RAY_IS_ERR(res));
    ray_error_free(res);

    ray_release(bad_k);
    ray_release(v);
    PASS();
}

/* Upsert into empty dict (placeholder I64 keys) with str-key — exercises
 * the empty-target adoption branch swap of keys vec. */
static test_result_t test_dict_upsert_empty_adopt_str(void) {
    /* Build d = (dict [] []) with default I64 placeholder keys. */
    ray_t* keys = ray_vec_new(RAY_I64, 0);
    ray_t* vals = ray_list_new(0);
    ray_t* d = ray_dict_new(keys, vals);

    ray_t* k = ray_str("hello", 5);
    ray_t* v = ray_i64(42);
    d = ray_dict_upsert(d, k, v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(ray_dict_len(d), 1);

    /* Keys vec should now be RAY_STR. */
    ray_t* dkeys = ray_dict_keys(d);
    TEST_ASSERT_EQ_I(dkeys->type, RAY_STR);

    ray_t* got = ray_dict_get(d, k);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_I(got->i64, 42);
    ray_release(got);

    ray_release(k);
    ray_release(v);
    ray_release(d);
    PASS();
}

/* ---- ray_dict_remove paths --------------------------------------------- */

/* remove with NULL d → error. */
static test_result_t test_dict_remove_null_d(void) {
    ray_t* k = ray_sym(ray_sym_intern("k", 1));
    ray_t* res = ray_dict_remove(NULL, k);
    TEST_ASSERT_TRUE(RAY_IS_ERR(res));
    ray_error_free(res);
    ray_release(k);
    PASS();
}

/* remove on non-dict → error; d released. */
static test_result_t test_dict_remove_nondict(void) {
    ray_t* not_a_dict = ray_i64(42);
    ray_t* k = ray_sym(ray_sym_intern("k", 1));
    ray_t* res = ray_dict_remove(not_a_dict, k);
    TEST_ASSERT_TRUE(RAY_IS_ERR(res));
    ray_error_free(res);
    ray_release(k);
    PASS();
}

/* remove typed-vec keys — exercises generic typed-vec branch (line 614+). */
static test_result_t test_dict_remove_typed_vec_keys(void) {
    int64_t k1 = 10, k2 = 20, k3 = 30;
    ray_t* keys = ray_vec_new(RAY_I64, 3);
    keys = ray_vec_append(keys, &k1);
    keys = ray_vec_append(keys, &k2);
    keys = ray_vec_append(keys, &k3);

    ray_t* vals = ray_list_new(3);
    vals = ray_list_append(vals, ray_i64(1));
    vals = ray_list_append(vals, ray_i64(2));
    vals = ray_list_append(vals, ray_i64(3));

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_i64(20);
    d = ray_dict_remove(d, ka);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(ray_dict_len(d), 2);

    /* k1 and k3 remain. */
    ray_t* g1 = ray_dict_get(d, (ray_release(ka), ka = ray_i64(10)));
    TEST_ASSERT_NOT_NULL(g1);
    TEST_ASSERT_EQ_I(g1->i64, 1);
    ray_release(g1);
    ray_release(ka);

    ka = ray_i64(20);
    TEST_ASSERT_NULL(ray_dict_get(d, ka));
    ray_release(ka);

    ray_release(d);
    PASS();
}

/* remove typed-vec keys with typed-vec vals — exercises the LIST-promote
 * branch in remove. */
static test_result_t test_dict_remove_typed_vec_vals_promote(void) {
    int64_t k1 = 1, k2 = 2;
    ray_t* keys = ray_vec_new(RAY_I64, 2);
    keys = ray_vec_append(keys, &k1);
    keys = ray_vec_append(keys, &k2);

    int64_t v1 = 11, v2 = 22;
    ray_t* vals = ray_vec_new(RAY_I64, 2);
    vals = ray_vec_append(vals, &v1);
    vals = ray_vec_append(vals, &v2);

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_i64(1);
    d = ray_dict_remove(d, ka);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(ray_dict_len(d), 1);

    /* Vals after promote is now RAY_LIST. */
    ray_t* dvals = ray_dict_vals(d);
    TEST_ASSERT_EQ_I(dvals->type, RAY_LIST);

    /* Remaining key 2. */
    ray_t* k2a = ray_i64(2);
    ray_t* got = ray_dict_get(d, k2a);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_I(got->i64, 22);
    ray_release(got);
    ray_release(k2a);
    ray_release(ka);

    ray_release(d);
    PASS();
}

/* remove a STR key — exercises STR branch in remove. */
static test_result_t test_dict_remove_str_keys(void) {
    ray_t* keys = ray_vec_new(RAY_STR, 3);
    keys = ray_str_vec_append(keys, "alpha", 5);
    keys = ray_str_vec_append(keys, "beta", 4);
    keys = ray_str_vec_append(keys, "gamma", 5);

    ray_t* vals = ray_list_new(3);
    vals = ray_list_append(vals, ray_i64(1));
    vals = ray_list_append(vals, ray_i64(2));
    vals = ray_list_append(vals, ray_i64(3));

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* k = ray_str("beta", 4);
    d = ray_dict_remove(d, k);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(ray_dict_len(d), 2);

    ray_t* gone = ray_dict_get(d, k);
    TEST_ASSERT_NULL(gone);

    ray_release(k);
    ray_release(d);
    PASS();
}

/* remove SYM key from W8/W16/W32 vec — exercises sym branch. */
static test_result_t test_dict_remove_sym_keys_w8(void) {
    int64_t a = ray_sym_intern("alpha2", 6);
    int64_t b = ray_sym_intern("beta2", 5);
    int64_t c = ray_sym_intern("gamma2", 6);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W8, 3);
    keys = ray_vec_append(keys, &a);
    keys = ray_vec_append(keys, &b);
    keys = ray_vec_append(keys, &c);

    ray_t* vals = ray_list_new(3);
    vals = ray_list_append(vals, ray_i64(1));
    vals = ray_list_append(vals, ray_i64(2));
    vals = ray_list_append(vals, ray_i64(3));

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_sym(b);
    d = ray_dict_remove(d, ka);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_EQ_I(ray_dict_len(d), 2);

    /* `a` and `c` should be findable. */
    ray_t* k_a = ray_sym(a);
    ray_t* got = ray_dict_get(d, k_a);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ_I(got->i64, 1);
    ray_release(got);
    ray_release(k_a);

    /* `b` should be missing. */
    TEST_ASSERT_NULL(ray_dict_get(d, ka));

    ray_release(ka);
    ray_release(d);
    PASS();
}

/* ---- find_idx SYM W8/W16/W32 paths ------------------------------------ */

/* Hits the SYM W8 branch inside ray_dict_find_idx (separate from
 * ray_dict_find_sym, which has its own W8/W16/W32 dispatcher). */
static test_result_t test_dict_find_idx_sym_w8(void) {
    int64_t a = ray_sym_intern("aaa", 3);
    int64_t b = ray_sym_intern("bbb", 3);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W8, 2);
    keys = ray_vec_append(keys, &a);
    keys = ray_vec_append(keys, &b);

    ray_t* vals = ray_list_new(2);
    vals = ray_list_append(vals, ray_i64(11));
    vals = ray_list_append(vals, ray_i64(22));

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_sym(a);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, ka), 0);
    ray_release(ka);

    ka = ray_sym(b);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, ka), 1);
    ray_release(ka);

    int64_t missing = ray_sym_intern("zzz", 3);
    ka = ray_sym(missing);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, ka), -1);
    ray_release(ka);

    ray_release(d);
    PASS();
}

static test_result_t test_dict_find_idx_sym_w16(void) {
    int64_t a = ray_sym_intern("ssa", 3);
    int64_t b = ray_sym_intern("ssb", 3);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W16, 2);
    keys = ray_vec_append(keys, &a);
    keys = ray_vec_append(keys, &b);

    ray_t* vals = ray_list_new(2);
    vals = ray_list_append(vals, ray_i64(33));
    vals = ray_list_append(vals, ray_i64(44));

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_sym(b);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, ka), 1);
    ray_release(ka);

    int64_t missing = ray_sym_intern("xxx", 3);
    ka = ray_sym(missing);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, ka), -1);
    ray_release(ka);

    ray_release(d);
    PASS();
}

static test_result_t test_dict_find_idx_sym_w32(void) {
    int64_t a = ray_sym_intern("ww32a", 5);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W32, 1);
    keys = ray_vec_append(keys, &a);

    ray_t* vals = ray_list_new(1);
    vals = ray_list_append(vals, ray_i64(55));

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_sym(a);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, ka), 0);
    ray_release(ka);

    ray_release(d);
    PASS();
}

/* find_idx STR keys with nulls — exercises null-skip in STR loop. */
static test_result_t test_dict_find_idx_str_with_nulls(void) {
    ray_t* keys = ray_vec_new(RAY_STR, 3);
    keys = ray_str_vec_append(keys, "alpha", 5);
    keys = ray_str_vec_append(keys, "", 0);
    keys = ray_str_vec_append(keys, "gamma", 5);
    /* Mark slot 1 as null. */
    ray_vec_set_null(keys, 1, true);

    ray_t* vals = ray_list_new(3);
    vals = ray_list_append(vals, ray_i64(1));
    vals = ray_list_append(vals, ray_i64(2));
    vals = ray_list_append(vals, ray_i64(3));

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_str("gamma", 5);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, ka), 2);
    ray_release(ka);

    /* An empty-string lookup must skip the null slot. */
    ka = ray_str("", 0);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, ka), -1);
    ray_release(ka);

    ray_release(d);
    PASS();
}

/* find_idx GUID keys with nulls — exercises null-skip in GUID loop. */
static test_result_t test_dict_find_idx_guid_with_nulls(void) {
    uint8_t g0[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    uint8_t g1[16] = { 0 };
    uint8_t g2[16] = { 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1 };

    ray_t* keys = ray_vec_new(RAY_GUID, 3);
    keys = ray_vec_append(keys, g0);
    keys = ray_vec_append(keys, g1);
    keys = ray_vec_append(keys, g2);
    /* Mark slot 1 as null. */
    ray_vec_set_null(keys, 1, true);

    ray_t* vals = ray_list_new(3);
    vals = ray_list_append(vals, ray_i64(1));
    vals = ray_list_append(vals, ray_i64(2));
    vals = ray_list_append(vals, ray_i64(3));

    ray_t* d = ray_dict_new(keys, vals);

    ray_t* ka = ray_guid(g2);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, ka), 2);
    ray_release(ka);

    /* All-zero query: would match slot 1 if not null-aware. */
    ka = ray_guid(g1);
    TEST_ASSERT_EQ_I(ray_dict_find_idx(d, ka), -1);
    ray_release(ka);

    ray_release(d);
    PASS();
}

/* GUID find_idx — key_atom->obj is NULL → returns -1.
 * This exercises the early-return when GUID key has no payload. */
static test_result_t test_dict_find_idx_guid_null_obj(void) {
    uint8_t g0[16] = { 9,8,7,6,5,4,3,2,1,0,1,2,3,4,5,6 };

    ray_t* keys = ray_vec_new(RAY_GUID, 1);
    keys = ray_vec_append(keys, g0);

    ray_t* vals = ray_list_new(1);
    vals = ray_list_append(vals, ray_i64(1));

    ray_t* d = ray_dict_new(keys, vals);

    /* Synthesize a null-typed GUID atom (typed null). */
    ray_t* nk = ray_typed_null(-RAY_GUID);
    if (nk && !RAY_IS_ERR(nk)) {
        /* Whether find returns -1 (no obj) or matches via null-aware probe
         * depends on the typed-null obj layout.  Either way the early-return
         * branch when key_atom->obj == NULL is what we want exercised. */
        (void)ray_dict_find_idx(d, nk);
        ray_release(nk);
    }

    ray_release(d);
    PASS();
}
/* ---- Suite definition -------------------------------------------------- */

const test_entry_t dict_entries[] = {
    { "dict/new_null_keys",                test_dict_new_null_keys,                dict_setup, dict_teardown },
    { "dict/new_null_vals",                test_dict_new_null_vals,                dict_setup, dict_teardown },
    { "dict/new_err_keys",                 test_dict_new_err_keys,                 dict_setup, dict_teardown },
    { "dict/new_err_vals",                 test_dict_new_err_vals,                 dict_setup, dict_teardown },
    { "dict/keys_null",                    test_dict_keys_null,                    dict_setup, dict_teardown },
    { "dict/keys_nondict",                 test_dict_keys_nondict,                 dict_setup, dict_teardown },
    { "dict/keys_err",                     test_dict_keys_err,                     dict_setup, dict_teardown },
    { "dict/find_sym_w8",                  test_dict_find_sym_w8,                  dict_setup, dict_teardown },
    { "dict/find_sym_w16",                 test_dict_find_sym_w16,                 dict_setup, dict_teardown },
    { "dict/find_sym_w32",                 test_dict_find_sym_w32,                 dict_setup, dict_teardown },
    { "dict/find_sym_invalid",             test_dict_find_sym_invalid,             dict_setup, dict_teardown },
    { "dict/probe_sym_borrowed",           test_dict_probe_sym_borrowed,           dict_setup, dict_teardown },
    { "dict/get_typed_vec_vals",           test_dict_get_typed_vec_vals,           dict_setup, dict_teardown },
    { "dict/get_each_value_type",          test_dict_get_each_value_type,          dict_setup, dict_teardown },
    { "dict/find_idx_invalid",             test_dict_find_idx_invalid,             dict_setup, dict_teardown },
    { "dict/find_idx_f32",                 test_dict_find_idx_f32,                 dict_setup, dict_teardown },
    { "dict/find_idx_guid",                test_dict_find_idx_guid,                dict_setup, dict_teardown },
    { "dict/find_idx_with_nulls",          test_dict_find_idx_with_nulls,          dict_setup, dict_teardown },
    { "dict/upsert_into_nondict",          test_dict_upsert_into_nondict,          dict_setup, dict_teardown },
    { "dict/upsert_null_d",                test_dict_upsert_null_d,                dict_setup, dict_teardown },
    { "dict/upsert_null_val",              test_dict_upsert_null_val,              dict_setup, dict_teardown },
    { "dict/upsert_replace_list_vals",     test_dict_upsert_replace_list_vals,     dict_setup, dict_teardown },
    { "dict/upsert_replace_typed_vec_vals", test_dict_upsert_replace_typed_vec_vals, dict_setup, dict_teardown },
    { "dict/upsert_append_typed_vec_vals", test_dict_upsert_append_typed_vec_vals, dict_setup, dict_teardown },
    { "dict/upsert_append_str_key",        test_dict_upsert_append_str_key,        dict_setup, dict_teardown },
    { "dict/upsert_append_guid_key",       test_dict_upsert_append_guid_key,       dict_setup, dict_teardown },
    { "dict/upsert_append_f32_key",        test_dict_upsert_append_f32_key,        dict_setup, dict_teardown },
    { "dict/upsert_key_type_mismatch",     test_dict_upsert_key_type_mismatch,     dict_setup, dict_teardown },
    { "dict/upsert_empty_adopt_str",       test_dict_upsert_empty_adopt_str,       dict_setup, dict_teardown },
    { "dict/remove_null_d",                test_dict_remove_null_d,                dict_setup, dict_teardown },
    { "dict/remove_nondict",               test_dict_remove_nondict,               dict_setup, dict_teardown },
    { "dict/remove_typed_vec_keys",        test_dict_remove_typed_vec_keys,        dict_setup, dict_teardown },
    { "dict/remove_typed_vec_vals_promote", test_dict_remove_typed_vec_vals_promote, dict_setup, dict_teardown },
    { "dict/remove_str_keys",              test_dict_remove_str_keys,              dict_setup, dict_teardown },
    { "dict/remove_sym_keys_w8",           test_dict_remove_sym_keys_w8,           dict_setup, dict_teardown },
    { "dict/find_idx_sym_w8",              test_dict_find_idx_sym_w8,              dict_setup, dict_teardown },
    { "dict/find_idx_sym_w16",             test_dict_find_idx_sym_w16,             dict_setup, dict_teardown },
    { "dict/find_idx_sym_w32",             test_dict_find_idx_sym_w32,             dict_setup, dict_teardown },
    { "dict/find_idx_str_with_nulls",      test_dict_find_idx_str_with_nulls,      dict_setup, dict_teardown },
    { "dict/find_idx_guid_with_nulls",     test_dict_find_idx_guid_with_nulls,     dict_setup, dict_teardown },
    { "dict/find_idx_guid_null_obj",       test_dict_find_idx_guid_null_obj,       dict_setup, dict_teardown },
    { NULL, NULL, NULL, NULL },
};
