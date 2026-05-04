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
#include "vec/str.h"
#include "ops/ops.h"
#include "table/sym.h"
#include <string.h>
#include <stdio.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void str_setup(void) {
    ray_heap_init();
}

static void str_teardown(void) {
    ray_heap_destroy();
}

/* ---- str_ptr SSO ------------------------------------------------------- */

static test_result_t test_str_ptr_sso(void) {
    ray_t* s = ray_str("hello", 5);
    TEST_ASSERT_NOT_NULL(s);

    const char* p = ray_str_ptr(s);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_MEM_EQ(5, p, "hello");

    ray_release(s);
    PASS();
}

/* ---- str_ptr long ------------------------------------------------------ */

static test_result_t test_str_ptr_long(void) {
    const char* text = "this is a longer string";
    size_t len = strlen(text);
    ray_t* s = ray_str(text, len);
    TEST_ASSERT_NOT_NULL(s);

    const char* p = ray_str_ptr(s);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_MEM_EQ(len, p, text);

    ray_release(s);
    PASS();
}

/* ---- str_len ----------------------------------------------------------- */

static test_result_t test_str_len(void) {
    /* SSO */
    ray_t* s1 = ray_str("abc", 3);
    TEST_ASSERT_EQ_U(ray_str_len(s1), 3);
    ray_release(s1);

    /* Empty SSO */
    ray_t* s2 = ray_str("", 0);
    TEST_ASSERT_EQ_U(ray_str_len(s2), 0);
    ray_release(s2);

    /* Long */
    const char* text = "a longer string for testing";
    size_t len = strlen(text);
    ray_t* s3 = ray_str(text, len);
    TEST_ASSERT_EQ_U(ray_str_len(s3), len);
    ray_release(s3);

    PASS();
}

/* ---- str_cmp equal ----------------------------------------------------- */

static test_result_t test_str_cmp_equal(void) {
    ray_t* a = ray_str("hello", 5);
    ray_t* b = ray_str("hello", 5);

    TEST_ASSERT_EQ_I(ray_str_cmp(a, b), 0);

    ray_release(a);
    ray_release(b);
    PASS();
}

/* ---- str_cmp different ------------------------------------------------- */

static test_result_t test_str_cmp_different(void) {
    ray_t* a = ray_str("abc", 3);
    ray_t* b = ray_str("abd", 3);

    int cmp = ray_str_cmp(a, b);
    TEST_ASSERT((cmp) < (0), "cmp < 0");  /* 'c' < 'd' */

    int cmp2 = ray_str_cmp(b, a);
    TEST_ASSERT((cmp2) > (0), "cmp2 > 0");

    ray_release(a);
    ray_release(b);
    PASS();
}

/* ---- str_cmp prefix ---------------------------------------------------- */

static test_result_t test_str_cmp_prefix(void) {
    ray_t* a = ray_str("abc", 3);
    ray_t* b = ray_str("abcde", 5);

    int cmp = ray_str_cmp(a, b);
    TEST_ASSERT((cmp) < (0), "cmp < 0");  /* shorter sorts first */

    int cmp2 = ray_str_cmp(b, a);
    TEST_ASSERT((cmp2) > (0), "cmp2 > 0");

    ray_release(a);
    ray_release(b);
    PASS();
}

/* ---- str_vec_new ------------------------------------------------------- */

static test_result_t test_str_vec_new(void) {
    ray_t* v = ray_vec_new(RAY_STR, 10);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->type, RAY_STR);
    TEST_ASSERT_EQ_I(ray_len(v), 0);
    /* Pool starts as NULL (no long strings yet) */
    TEST_ASSERT_NULL(v->str_pool);
    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_new_zero_cap(void) {
    ray_t* v = ray_vec_new(RAY_STR, 0);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(v->type, RAY_STR);
    ray_release(v);
    PASS();
}

/* ---- str_vec_append inline ---------------------------------------------- */

static test_result_t test_str_vec_append_inline(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);

    /* Append a short string (fits in 12 bytes) */
    v = ray_str_vec_append(v, "hello", 5);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(ray_len(v), 1);

    /* Verify element is inline */
    ray_str_t* elems = (ray_str_t*)ray_data(v);
    TEST_ASSERT_EQ_U(elems[0].len, 5);
    TEST_ASSERT_MEM_EQ(5, elems[0].data, "hello");

    /* Pool should still be NULL (no long strings) */
    TEST_ASSERT_NULL(v->str_pool);

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_append_inline_12(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);

    /* Exactly 12 bytes — should still inline */
    v = ray_str_vec_append(v, "123456789012", 12);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_str_t* elems = (ray_str_t*)ray_data(v);
    TEST_ASSERT_EQ_U(elems[0].len, 12);
    TEST_ASSERT_MEM_EQ(12, elems[0].data, "123456789012");
    TEST_ASSERT_NULL(v->str_pool);

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_append_empty(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);

    v = ray_str_vec_append(v, "", 0);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_str_t* elems = (ray_str_t*)ray_data(v);
    TEST_ASSERT_EQ_U(elems[0].len, 0);
    TEST_ASSERT_NULL(v->str_pool);

    ray_release(v);
    PASS();
}

/* ---- str_vec_append pooled ---------------------------------------------- */

static test_result_t test_str_vec_append_pooled(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);

    /* 13 bytes — must go to pool */
    const char* long_str = "hello world!!";
    v = ray_str_vec_append(v, long_str, 13);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(ray_len(v), 1);

    /* Pool should be allocated */
    TEST_ASSERT_NOT_NULL(v->str_pool);

    /* Element should have prefix and pool offset */
    ray_str_t* elems = (ray_str_t*)ray_data(v);
    TEST_ASSERT_EQ_U(elems[0].len, 13);
    TEST_ASSERT_MEM_EQ(4, elems[0].prefix, "hell");
    TEST_ASSERT_EQ_U(elems[0].pool_off, 0);

    /* Verify pool contains the string */
    const char* pool_data = (const char*)ray_data(v->str_pool);
    TEST_ASSERT_MEM_EQ(13, pool_data + 0, long_str);

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_append_mixed(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);

    /* Mix inline and pooled */
    v = ray_str_vec_append(v, "short", 5);
    v = ray_str_vec_append(v, "this is a long string!", 22);
    v = ray_str_vec_append(v, "tiny", 4);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(ray_len(v), 3);

    ray_str_t* elems = (ray_str_t*)ray_data(v);
    /* First: inline */
    TEST_ASSERT_EQ_U(elems[0].len, 5);
    TEST_ASSERT_MEM_EQ(5, elems[0].data, "short");
    /* Second: pooled */
    TEST_ASSERT_EQ_U(elems[1].len, 22);
    TEST_ASSERT_MEM_EQ(4, elems[1].prefix, "this");
    /* Third: inline */
    TEST_ASSERT_EQ_U(elems[2].len, 4);
    TEST_ASSERT_MEM_EQ(4, elems[2].data, "tiny");

    ray_release(v);
    PASS();
}

/* ---- str_vec_get ------------------------------------------------------- */

static test_result_t test_str_vec_get_inline(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);

    size_t len;
    const char* ptr = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_NOT_NULL(ptr);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, ptr, "hello");

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_get_pooled(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "this is a long string!", 22);

    size_t len;
    const char* ptr = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_NOT_NULL(ptr);
    TEST_ASSERT_EQ_U(len, 22);
    TEST_ASSERT_MEM_EQ(22, ptr, "this is a long string!");

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_get_oob(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);

    size_t len;
    const char* ptr = ray_str_vec_get(v, 1, &len);
    TEST_ASSERT_NULL(ptr);

    ray_release(v);
    PASS();
}

/* ---- str_vec_set ------------------------------------------------------- */

static test_result_t test_str_vec_set_inline_to_inline(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);

    v = ray_str_vec_set(v, 0, "world", 5);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    size_t len;
    const char* ptr = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, ptr, "world");

    /* No pool needed */
    TEST_ASSERT_NULL(v->str_pool);

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_set_pooled_to_pooled(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "original long string!", 21);

    v = ray_str_vec_set(v, 0, "replacement string!!", 20);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    size_t len;
    const char* ptr = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 20);
    TEST_ASSERT_MEM_EQ(20, ptr, "replacement string!!");

    /* Old 21 bytes are dead in pool */
    /* Pool used should be 21 (old dead) + 20 (new) = 41 */
    TEST_ASSERT_EQ_I(v->str_pool->len, 41);

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_set_oob(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);

    ray_t* r = ray_str_vec_set(v, 5, "x", 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_set_inline_to_pooled(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "short", 5);

    /* Replace inline with pooled */
    v = ray_str_vec_set(v, 0, "this is now a long string!", 26);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    size_t len;
    const char* ptr = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 26);
    TEST_ASSERT_MEM_EQ(26, ptr, "this is now a long string!");
    TEST_ASSERT_NOT_NULL(v->str_pool);

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_set_pooled_to_inline(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "this is a long string!!", 23);

    /* Replace pooled with inline */
    v = ray_str_vec_set(v, 0, "tiny", 4);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    size_t len;
    const char* ptr = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 4);
    TEST_ASSERT_MEM_EQ(4, ptr, "tiny");

    ray_release(v);
    PASS();
}

/* ---- str_vec_get/set negative index ------------------------------------ */

static test_result_t test_str_vec_get_negative(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);

    size_t len = SIZE_MAX;
    const char* ptr = ray_str_vec_get(v, -1, &len);
    TEST_ASSERT_NULL(ptr);
    /* len should be zeroed on error */
    TEST_ASSERT_EQ_U(len, 0);

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_set_negative(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);

    ray_t* r = ray_str_vec_set(v, -1, "x", 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    ray_release(v);
    PASS();
}

/* ---- str_vec_compact --------------------------------------------------- */

static test_result_t test_str_vec_compact(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);

    /* Fill with pooled strings */
    v = ray_str_vec_append(v, "original string one!", 20);
    v = ray_str_vec_append(v, "original string two!", 20);

    /* Overwrite first — creates 20 dead bytes */
    v = ray_str_vec_set(v, 0, "replacement str one!", 20);

    TEST_ASSERT_EQ_I(v->str_pool->len, 60);  /* 20+20+20 */

    /* Compact */
    v = ray_str_vec_compact(v);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* Pool should shrink: only 40 live bytes (string two + replacement) */
    TEST_ASSERT_EQ_I(v->str_pool->len, 40);

    /* Strings still readable */
    size_t len;
    const char* p0 = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 20);
    TEST_ASSERT_MEM_EQ(20, p0, "replacement str one!");

    const char* p1 = ray_str_vec_get(v, 1, &len);
    TEST_ASSERT_EQ_U(len, 20);
    TEST_ASSERT_MEM_EQ(20, p1, "original string two!");

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_compact_noop(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);

    /* No pooled strings — compact should be a no-op */
    v = ray_str_vec_append(v, "short", 5);
    v = ray_str_vec_compact(v);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_NULL(v->str_pool);

    size_t len;
    const char* ptr = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, ptr, "short");

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_compact_all_dead(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);

    /* Append pooled, then overwrite to inline — all pool bytes dead */
    v = ray_str_vec_append(v, "this is a long string!", 22);
    v = ray_str_vec_set(v, 0, "short", 5);

    v = ray_str_vec_compact(v);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* Pool should be freed entirely */
    TEST_ASSERT_NULL(v->str_pool);

    size_t len;
    const char* ptr = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, ptr, "short");

    ray_release(v);
    PASS();
}

/* ---- compact: all pool bytes dead after overwrite ----------------------- */

static test_result_t test_str_vec_compact_all_pool_dead(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);

    /* Append two pooled strings */
    v = ray_str_vec_append(v, "first long pooled str!", 22);
    v = ray_str_vec_append(v, "second long pooled st!", 22);

    /* Overwrite both with inline — all 44 pool bytes become dead */
    v = ray_str_vec_set(v, 0, "a", 1);
    v = ray_str_vec_set(v, 1, "b", 1);

    /* Compact should free pool entirely */
    v = ray_str_vec_compact(v);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_NULL(v->str_pool);

    /* Strings still readable */
    size_t len;
    const char* p0 = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 1);
    TEST_ASSERT_MEM_EQ(1, p0, "a");

    ray_release(v);
    PASS();
}

/* ---- compact: saturated dead counter ----------------------------------- */

static test_result_t test_str_vec_compact_saturated_dead(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);

    /* Append one pooled string (>12 bytes so it goes to pool) */
    v = ray_str_vec_append(v, "this is a pooled string!", 24);

    /* Force the dead-byte counter to UINT32_MAX to simulate saturation.
     * The counter is stored in the first 4 bytes of str_pool->nullmap. */
    TEST_ASSERT_NOT_NULL(v->str_pool);
    uint32_t saturated = UINT32_MAX;
    memcpy(v->str_pool->nullmap, &saturated, 4);

    /* Compact must scan elements for true live size instead of using
     * pool_used - dead (which would underflow). */
    v = ray_str_vec_compact(v);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* The pooled string must survive compact */
    TEST_ASSERT_NOT_NULL(v->str_pool);
    size_t len;
    const char* s = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 24);
    TEST_ASSERT_MEM_EQ(24, s, "this is a pooled string!");

    ray_release(v);
    PASS();
}

/* ---- str_t_eq inline --------------------------------------------------- */

static test_result_t test_str_t_eq_inline(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);
    v = ray_str_vec_append(v, "hello", 5);
    v = ray_str_vec_append(v, "world", 5);

    ray_str_t* elems = (ray_str_t*)ray_data(v);
    TEST_ASSERT_TRUE(ray_str_t_eq(&elems[0], NULL, &elems[1], NULL));
    TEST_ASSERT_FALSE(ray_str_t_eq(&elems[0], NULL, &elems[2], NULL));

    ray_release(v);
    PASS();
}

/* ---- str_t_eq pooled --------------------------------------------------- */

static test_result_t test_str_t_eq_pooled(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "a]longer string here!", 21);
    v = ray_str_vec_append(v, "a]longer string here!", 21);
    v = ray_str_vec_append(v, "a]longer string nope!", 21);

    ray_str_t* elems = (ray_str_t*)ray_data(v);
    const char* pool = (const char*)ray_data(v->str_pool);
    TEST_ASSERT_TRUE(ray_str_t_eq(&elems[0], pool, &elems[1], pool));
    /* Same prefix "a]lo" but different content */
    TEST_ASSERT_FALSE(ray_str_t_eq(&elems[0], pool, &elems[2], pool));

    ray_release(v);
    PASS();
}

/* ---- str_t_cmp order --------------------------------------------------- */

static test_result_t test_str_t_cmp_order(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "apple", 5);
    v = ray_str_vec_append(v, "banana", 6);
    v = ray_str_vec_append(v, "apple", 5);

    ray_str_t* elems = (ray_str_t*)ray_data(v);
    TEST_ASSERT((ray_str_t_cmp(&elems[0], NULL, &elems[1], NULL)) < (0), "ray_str_t_cmp(&elems[0], NULL, &elems[1], NULL) < 0");
    TEST_ASSERT((ray_str_t_cmp(&elems[1], NULL, &elems[0], NULL)) > (0), "ray_str_t_cmp(&elems[1], NULL, &elems[0], NULL) > 0");
    TEST_ASSERT_EQ_I(ray_str_t_cmp(&elems[0], NULL, &elems[2], NULL), 0);

    ray_release(v);
    PASS();
}

/* ---- str_vec_null ------------------------------------------------------ */

static test_result_t test_str_vec_null(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);
    v = ray_str_vec_append(v, "", 0);  /* empty, not null */
    v = ray_str_vec_append(v, "world", 5);
    TEST_ASSERT_EQ_I(ray_len(v), 3);

    /* Mark row 1 as null */
    ray_vec_set_null(v, 1, true);
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 2));

    /* Non-null rows still readable */
    size_t len;
    const char* ptr = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, ptr, "hello");

    ray_release(v);
    PASS();
}

/* ---- null with pooled strings ------------------------------------------ */

static test_result_t test_str_vec_null_pooled(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "this is a long string!!", 23);
    v = ray_str_vec_append(v, "another long string!!!", 22);
    v = ray_str_vec_append(v, "short", 5);

    /* Set null on row 1 — must not corrupt str_pool */
    ray_vec_set_null(v, 1, true);
    TEST_ASSERT_TRUE(ray_vec_is_null(v, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 2));

    /* Pool and strings must still be intact */
    TEST_ASSERT_NOT_NULL(v->str_pool);
    size_t len;
    const char* p0 = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 23);
    TEST_ASSERT_MEM_EQ(23, p0, "this is a long string!!");

    const char* p2 = ray_str_vec_get(v, 2, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, p2, "short");

    ray_release(v);
    PASS();
}

/* ---- grow stress test -------------------------------------------------- */

static test_result_t test_str_vec_grow(void) {
    ray_t* v = ray_vec_new(RAY_STR, 2);  /* small initial capacity */

    /* Append 200 strings, mixing inline and pooled, forcing multiple reallocs */
    for (int i = 0; i < 200; i++) {
        char buf[64];
        int slen;
        if (i % 3 == 0) {
            /* Pooled: > 12 bytes */
            slen = snprintf(buf, sizeof(buf), "long-string-number-%04d!", i);
        } else {
            /* Inline: <= 12 bytes */
            slen = snprintf(buf, sizeof(buf), "s%03d", i);
        }
        v = ray_str_vec_append(v, buf, (size_t)slen);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }

    TEST_ASSERT_EQ_I(ray_len(v), 200);

    /* Verify all strings are readable */
    for (int i = 0; i < 200; i++) {
        char buf[64];
        int slen;
        if (i % 3 == 0) {
            slen = snprintf(buf, sizeof(buf), "long-string-number-%04d!", i);
        } else {
            slen = snprintf(buf, sizeof(buf), "s%03d", i);
        }
        size_t got_len;
        const char* ptr = ray_str_vec_get(v, i, &got_len);
        TEST_ASSERT_NOT_NULL(ptr);
        TEST_ASSERT_EQ_U(got_len, (size_t)slen);
        TEST_ASSERT_MEM_EQ((size_t)slen, ptr, buf);
    }

    ray_release(v);
    PASS();
}

/* ---- Slice tests ------------------------------------------------------- */

static test_result_t test_str_vec_slice(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);
    v = ray_str_vec_append(v, "this is a long string!", 22);
    v = ray_str_vec_append(v, "world", 5);
    v = ray_str_vec_append(v, "another long string!!", 21);

    ray_t* s = ray_vec_slice(v, 1, 2);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_EQ_I(ray_len(s), 2);

    /* Read through slice — pooled string */
    size_t len;
    const char* p0 = ray_str_vec_get(s, 0, &len);
    TEST_ASSERT_EQ_U(len, 22);
    TEST_ASSERT_MEM_EQ(22, p0, "this is a long string!");

    /* Read through slice — inline string */
    const char* p1 = ray_str_vec_get(s, 1, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, p1, "world");

    ray_release(s);
    ray_release(v);
    PASS();
}

/* ---- Concat tests ------------------------------------------------------ */

static test_result_t test_str_vec_concat_vecs(void) {
    ray_t* a = ray_vec_new(RAY_STR, 2);
    a = ray_str_vec_append(a, "hello", 5);
    a = ray_str_vec_append(a, "this is long string a!", 22);

    ray_t* b = ray_vec_new(RAY_STR, 2);
    b = ray_str_vec_append(b, "world", 5);
    b = ray_str_vec_append(b, "this is long string b!", 22);

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(ray_len(c), 4);

    size_t len;
    const char* p0 = ray_str_vec_get(c, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, p0, "hello");

    const char* p1 = ray_str_vec_get(c, 1, &len);
    TEST_ASSERT_EQ_U(len, 22);
    TEST_ASSERT_MEM_EQ(22, p1, "this is long string a!");

    const char* p2 = ray_str_vec_get(c, 2, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, p2, "world");

    const char* p3 = ray_str_vec_get(c, 3, &len);
    TEST_ASSERT_EQ_U(len, 22);
    TEST_ASSERT_MEM_EQ(22, p3, "this is long string b!");

    ray_release(c);
    ray_release(a);
    ray_release(b);
    PASS();
}

/* ====================================================================
 * Graph-level string op tests (src/ops/string.c coverage)
 * ==================================================================== */

/* Helper: build a small SYM table with one "name" column */
static ray_t* make_str_sym_table(void) {
    (void)ray_sym_init();
    int64_t s0 = ray_sym_intern("hello", 5);
    int64_t s1 = ray_sym_intern("WORLD", 5);
    int64_t s2 = ray_sym_intern("  foo  ", 7);
    ray_t* vec = ray_sym_vec_new(RAY_SYM_W64, 3);
    vec->len = 3;
    int64_t* d = (int64_t*)ray_data(vec);
    d[0] = s0; d[1] = s1; d[2] = s2;
    int64_t n = ray_sym_intern("name", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, n, vec);
    ray_release(vec);
    return tbl;
}

/* exec_like: non-STR/non-SYM input → memset(dst,0) else-branch */
static test_result_t test_str_like_non_string_type(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build an I64 column — exec_like's else branch fills with 0 */
    ray_t* col = ray_vec_new(RAY_I64, 3);
    col->len = 3;
    int64_t* cd = (int64_t*)ray_data(col);
    cd[0] = 1; cd[1] = 2; cd[2] = 3;

    int64_t nm = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nm, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* pat = ray_const_str(g, "1*", 2);
    ray_op_t* lk  = ray_like(g, val, pat);
    ray_t* result = ray_execute(g, lk);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(result->len, 3);
    uint8_t* rd = (uint8_t*)ray_data(result);
    /* All false — I64 is not a string type */
    TEST_ASSERT_EQ_I(rd[0], 0);
    TEST_ASSERT_EQ_I(rd[1], 0);
    TEST_ASSERT_EQ_I(rd[2], 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_ilike: non-STR/non-SYM input → memset(dst,0) else-branch */
static test_result_t test_str_ilike_non_string_type(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_I64, 2);
    col->len = 2;
    int64_t* cd = (int64_t*)ray_data(col);
    cd[0] = 10; cd[1] = 20;

    int64_t nm = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nm, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* val = ray_scan(g, "val");
    ray_op_t* pat = ray_const_str(g, "*", 1);
    ray_op_t* ilk = ray_ilike(g, val, pat);
    ray_t* result = ray_execute(g, ilk);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    uint8_t* rd = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(rd[0], 0);
    TEST_ASSERT_EQ_I(rd[1], 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_ilike: STR column, case-insensitive match */
static test_result_t test_str_ilike_str_column(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_STR, 3);
    col = ray_str_vec_append(col, "Hello", 5);
    col = ray_str_vec_append(col, "WORLD", 5);
    col = ray_str_vec_append(col, "foo", 3);

    int64_t nm = ray_sym_intern("name", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nm, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* pat  = ray_const_str(g, "hello", 5);
    ray_op_t* ilk  = ray_ilike(g, name, pat);
    ray_t* result  = ray_execute(g, ilk);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_BOOL);
    uint8_t* rd = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(rd[0], 1);  /* "Hello" ilike "hello" */
    TEST_ASSERT_EQ_I(rd[1], 0);  /* "WORLD" ilike "hello" — no */
    TEST_ASSERT_EQ_I(rd[2], 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_string_unary (UPPER): SYM column with null → sym_dst[i]=0 + set_null */
static test_result_t test_str_upper_sym_null(void) {
    ray_heap_init();
    ray_t* tbl = make_str_sym_table();

    /* mark row 1 null in the SYM column */
    ray_t* col = ray_table_get_col(tbl, ray_sym_intern("name", 4));
    ray_vec_set_null(col, 1, true);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* up   = ray_upper(g, name);
    ray_t* result  = ray_execute(g, up);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_SYM);
    TEST_ASSERT_EQ_I(result->len, 3);

    /* row 0: HELLO */
    int64_t* rd = (int64_t*)ray_data(result);
    ray_t* s0 = ray_sym_str(rd[0]);
    TEST_ASSERT_STR_EQ(ray_str_ptr(s0), "HELLO");

    /* row 1: null */
    TEST_ASSERT_TRUE(ray_vec_is_null(result, 1));

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_string_unary (LOWER): SYM column with null */
static test_result_t test_str_lower_sym_null(void) {
    ray_heap_init();
    ray_t* tbl = make_str_sym_table();

    ray_t* col = ray_table_get_col(tbl, ray_sym_intern("name", 4));
    ray_vec_set_null(col, 0, true);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* lo   = ray_lower(g, name);
    ray_t* result  = ray_execute(g, lo);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_SYM);
    TEST_ASSERT_TRUE(ray_vec_is_null(result, 0));

    /* row 1: "world" */
    int64_t* rd = (int64_t*)ray_data(result);
    ray_t* s1 = ray_sym_str(rd[1]);
    TEST_ASSERT_STR_EQ(ray_str_ptr(s1), "world");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_strlen: SYM column with null → null propagation in SYM branch */
static test_result_t test_str_strlen_sym_null(void) {
    ray_heap_init();
    ray_t* tbl = make_str_sym_table();

    ray_t* col = ray_table_get_col(tbl, ray_sym_intern("name", 4));
    ray_vec_set_null(col, 2, true);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name  = ray_scan(g, "name");
    ray_op_t* slen  = ray_strlen(g, name);
    ray_t* result   = ray_execute(g, slen);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_I64);
    int64_t* rd = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(rd[0], 5);   /* "hello" */
    TEST_ASSERT_EQ_I(rd[1], 5);   /* "WORLD" */
    TEST_ASSERT_TRUE(ray_vec_is_null(result, 2));  /* null propagated */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_substr: SYM column → SYM output */
static test_result_t test_str_substr_sym(void) {
    ray_heap_init();
    ray_t* tbl = make_str_sym_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name   = ray_scan(g, "name");
    ray_op_t* start  = ray_const_i64(g, 2);   /* 1-based → skip first char */
    ray_op_t* len_op = ray_const_i64(g, 3);
    ray_op_t* sub    = ray_substr(g, name, start, len_op);
    ray_t* result    = ray_execute(g, sub);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_SYM);
    TEST_ASSERT_EQ_I(result->len, 3);

    int64_t* rd = (int64_t*)ray_data(result);
    /* "hello"[2..4] = "ell" */
    ray_t* s0 = ray_sym_str(rd[0]);
    TEST_ASSERT_STR_EQ(ray_str_ptr(s0), "ell");
    /* "WORLD"[2..4] = "ORL" */
    ray_t* s1 = ray_sym_str(rd[1]);
    TEST_ASSERT_STR_EQ(ray_str_ptr(s1), "ORL");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_substr: SYM column with null → null propagation in SYM branch */
static test_result_t test_str_substr_sym_null(void) {
    ray_heap_init();
    ray_t* tbl = make_str_sym_table();

    ray_t* col = ray_table_get_col(tbl, ray_sym_intern("name", 4));
    ray_vec_set_null(col, 1, true);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name   = ray_scan(g, "name");
    ray_op_t* start  = ray_const_i64(g, 1);
    ray_op_t* len_op = ray_const_i64(g, 2);
    ray_op_t* sub    = ray_substr(g, name, start, len_op);
    ray_t* result    = ray_execute(g, sub);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_SYM);
    TEST_ASSERT_TRUE(ray_vec_is_null(result, 1));

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_substr: SYM column — start beyond string length → empty sym */
static test_result_t test_str_substr_sym_out_of_range(void) {
    ray_heap_init();
    ray_t* tbl = make_str_sym_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name   = ray_scan(g, "name");
    ray_op_t* start  = ray_const_i64(g, 100);  /* way beyond any string */
    ray_op_t* len_op = ray_const_i64(g, 3);
    ray_op_t* sub    = ray_substr(g, name, start, len_op);
    ray_t* result    = ray_execute(g, sub);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_SYM);
    /* all rows: empty string */
    int64_t* rd = (int64_t*)ray_data(result);
    ray_t* s0 = ray_sym_str(rd[0]);
    TEST_ASSERT_EQ_U(ray_str_len(s0), 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_replace: SYM column → SYM output */
static test_result_t test_str_replace_sym(void) {
    ray_heap_init();
    ray_t* tbl = make_str_sym_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* from = ray_const_str(g, "l", 1);
    ray_op_t* to   = ray_const_str(g, "L", 1);  /* same length replace */
    ray_op_t* rep  = ray_replace(g, name, from, to);
    ray_t* result  = ray_execute(g, rep);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_SYM);
    TEST_ASSERT_EQ_I(result->len, 3);

    int64_t* rd = (int64_t*)ray_data(result);
    ray_t* s0 = ray_sym_str(rd[0]);
    TEST_ASSERT_STR_EQ(ray_str_ptr(s0), "heLLo");  /* "hello" -> "heLLo" */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_replace: SYM column with null */
static test_result_t test_str_replace_sym_null(void) {
    ray_heap_init();
    ray_t* tbl = make_str_sym_table();

    ray_t* col = ray_table_get_col(tbl, ray_sym_intern("name", 4));
    ray_vec_set_null(col, 0, true);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* from = ray_const_str(g, "o", 1);
    ray_op_t* to   = ray_const_str(g, "0", 1);
    ray_op_t* rep  = ray_replace(g, name, from, to);
    ray_t* result  = ray_execute(g, rep);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_SYM);
    TEST_ASSERT_TRUE(ray_vec_is_null(result, 0));

    /* row 1: "WORLD" → no lowercase o → "WORLD" */
    int64_t* rd = (int64_t*)ray_data(result);
    ray_t* s1 = ray_sym_str(rd[1]);
    TEST_ASSERT_STR_EQ(ray_str_ptr(s1), "WORLD");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_replace: shrink path (to_len < from_len) on STR column */
static test_result_t test_str_replace_str_shrink(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_STR, 3);
    col = ray_str_vec_append(col, "aabbaabb", 8);
    col = ray_str_vec_append(col, "xyzxyz", 6);
    col = ray_str_vec_append(col, "cc", 2);

    int64_t nm = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nm, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* val  = ray_scan(g, "val");
    ray_op_t* from = ray_const_str(g, "aa", 2);  /* 2-char from */
    ray_op_t* to   = ray_const_str(g, "A", 1);   /* 1-char to → shrink */
    ray_op_t* rep  = ray_replace(g, val, from, to);
    ray_t* result  = ray_execute(g, rep);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);

    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 6);
    TEST_ASSERT_MEM_EQ(6, s0, "AbbAbb");  /* "aabbaabb" → "AbbAbb" */

    const char* s1 = ray_str_vec_get(result, 1, &len);
    TEST_ASSERT_EQ_U(len, 6);
    TEST_ASSERT_MEM_EQ(6, s1, "xyzxyz");  /* no match */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_concat: >16 args → scratch_calloc path for args array */
static test_result_t test_str_concat_many_args(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a table with 17 STR columns */
    enum { NCOLS = 17 };
    ray_t* tbl = ray_table_new(NCOLS);
    ray_t* cols[NCOLS];
    int64_t colnames[NCOLS];
    for (int i = 0; i < NCOLS; i++) {
        char nbuf[8];
        snprintf(nbuf, sizeof(nbuf), "c%d", i);
        colnames[i] = ray_sym_intern(nbuf, strlen(nbuf));

        cols[i] = ray_vec_new(RAY_STR, 1);
        cols[i] = ray_str_vec_append(cols[i], "x", 1);
        tbl = ray_table_add_col(tbl, colnames[i], cols[i]);
        ray_release(cols[i]);
    }

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* ops[NCOLS];
    for (int i = 0; i < NCOLS; i++) {
        char nbuf[8];
        snprintf(nbuf, sizeof(nbuf), "c%d", i);
        ops[i] = ray_scan(g, nbuf);
    }
    ray_op_t* cat = ray_concat(g, ops, NCOLS);
    ray_t* result = ray_execute(g, cat);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 1);

    size_t len;
    (void)ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, NCOLS);  /* 17 x "x" = "xxxxxxxxxxxxxxxxx" */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_concat: SYM output with null → dst[r]=0 + set_null */
static test_result_t test_str_concat_sym_null(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a SYM table with two columns, null in second col row 1 */
    int64_t sa = ray_sym_intern("hello", 5);
    int64_t sb = ray_sym_intern("world", 5);

    ray_t* ca = ray_sym_vec_new(RAY_SYM_W64, 2);
    ca->len = 2;
    int64_t* da = (int64_t*)ray_data(ca);
    da[0] = sa; da[1] = sa;

    ray_t* cb = ray_sym_vec_new(RAY_SYM_W64, 2);
    cb->len = 2;
    int64_t* db = (int64_t*)ray_data(cb);
    db[0] = sb; db[1] = sb;
    ray_vec_set_null(cb, 1, true);  /* null in second arg, row 1 */

    int64_t n1 = ray_sym_intern("a", 1);
    int64_t n2 = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n1, ca);
    tbl = ray_table_add_col(tbl, n2, cb);
    ray_release(ca);
    ray_release(cb);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* oa = ray_scan(g, "a");
    ray_op_t* ob = ray_scan(g, "b");
    ray_op_t* args[] = {oa, ob};
    ray_op_t* cat = ray_concat(g, args, 2);
    ray_t* result = ray_execute(g, cat);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* row 0: "hello" + "world" = "helloworld" */
    TEST_ASSERT_FALSE(ray_vec_is_null(result, 0));
    /* row 1: second arg is null → entire row null */
    TEST_ASSERT_TRUE(ray_vec_is_null(result, 1));

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_trim: SYM column with null */
static test_result_t test_str_trim_sym_null(void) {
    ray_heap_init();
    ray_t* tbl = make_str_sym_table();

    ray_t* col = ray_table_get_col(tbl, ray_sym_intern("name", 4));
    ray_vec_set_null(col, 2, true);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* tr   = ray_trim_op(g, name);
    ray_t* result  = ray_execute(g, tr);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_SYM);
    TEST_ASSERT_EQ_I(result->len, 3);
    TEST_ASSERT_TRUE(ray_vec_is_null(result, 2));

    int64_t* rd = (int64_t*)ray_data(result);
    ray_t* s0 = ray_sym_str(rd[0]);
    TEST_ASSERT_STR_EQ(ray_str_ptr(s0), "hello");  /* no whitespace */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_like: STR column (verifies STR arm of exec_like) */
static test_result_t test_str_like_str_column(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_STR, 3);
    col = ray_str_vec_append(col, "foobar", 6);
    col = ray_str_vec_append(col, "baz", 3);
    col = ray_str_vec_append(col, "fooXXX", 6);

    int64_t nm = ray_sym_intern("name", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nm, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* pat  = ray_const_str(g, "foo*", 4);
    ray_op_t* lk   = ray_like(g, name, pat);
    ray_t* result  = ray_execute(g, lk);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    uint8_t* rd = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(rd[0], 1);
    TEST_ASSERT_EQ_I(rd[1], 0);
    TEST_ASSERT_EQ_I(rd[2], 1);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_ilike: SYM column, case-insensitive */
static test_result_t test_str_ilike_sym_column(void) {
    ray_heap_init();
    ray_t* tbl = make_str_sym_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name = ray_scan(g, "name");
    ray_op_t* pat  = ray_const_str(g, "HELLO", 5);
    ray_op_t* ilk  = ray_ilike(g, name, pat);
    ray_t* result  = ray_execute(g, ilk);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    uint8_t* rd = (uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(rd[0], 1);  /* "hello" ilike "HELLO" */
    TEST_ASSERT_EQ_I(rd[1], 0);
    TEST_ASSERT_EQ_I(rd[2], 0);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_substr: F64 scalar start → -RAY_F64 branch (line 289) */
static test_result_t test_str_substr_f64_scalar_start(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_sym_table();  /* SYM: "hello","WORLD","  foo  " */

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name   = ray_scan(g, "name");
    ray_op_t* start  = ray_const_f64(g, 2.0);  /* F64 scalar → -RAY_F64 branch */
    ray_op_t* len_op = ray_const_i64(g, 3);
    ray_op_t* sub    = ray_substr(g, name, start, len_op);
    ray_t* result    = ray_execute(g, sub);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_SYM);
    /* "hello"[2..4] = "ell" */
    int64_t* rd = (int64_t*)ray_data(result);
    ray_t* s0 = ray_sym_str(rd[0]);
    TEST_ASSERT_EQ_U(ray_str_len(s0), 3);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_substr: F64 scalar len → -RAY_F64 branch on len (line 301) */
static test_result_t test_str_substr_f64_scalar_len(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_str_sym_table();

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name   = ray_scan(g, "name");
    ray_op_t* start  = ray_const_i64(g, 1);
    ray_op_t* len_op = ray_const_f64(g, 3.0);  /* F64 scalar for len */
    ray_op_t* sub    = ray_substr(g, name, start, len_op);
    ray_t* result    = ray_execute(g, sub);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_SYM);
    int64_t* rd = (int64_t*)ray_data(result);
    ray_t* s0 = ray_sym_str(rd[0]);
    TEST_ASSERT_EQ_U(ray_str_len(s0), 3);  /* "hel" */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_substr: I32 vector start → s_data_i32 branch (line 298) */
static test_result_t test_str_substr_i32_vec_start(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_STR, 3);
    col = ray_str_vec_append(col, "hello", 5);
    col = ray_str_vec_append(col, "world", 5);
    col = ray_str_vec_append(col, "foobar", 6);

    int32_t start_raw[] = {1, 2, 3};
    ray_t* start_col = ray_vec_from_raw(RAY_I32, start_raw, 3);

    int64_t nm = ray_sym_intern("name", 4);
    int64_t ns = ray_sym_intern("start", 5);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, nm, col);
    tbl = ray_table_add_col(tbl, ns, start_col);
    ray_release(col);
    ray_release(start_col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name   = ray_scan(g, "name");
    ray_op_t* start  = ray_scan(g, "start");   /* I32 vector */
    ray_op_t* len_op = ray_const_i64(g, 3);
    ray_op_t* sub    = ray_substr(g, name, start, len_op);
    ray_t* result    = ray_execute(g, sub);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    /* row0: "hello"[0..2] = "hel" */
    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, s0, "hel");
    /* row1: "world"[1..3] = "orl" */
    const char* s1 = ray_str_vec_get(result, 1, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, s1, "orl");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_substr: I64 vector start → s_data branch (line 299) */
static test_result_t test_str_substr_i64_vec_start(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_STR, 3);
    col = ray_str_vec_append(col, "hello", 5);
    col = ray_str_vec_append(col, "world", 5);
    col = ray_str_vec_append(col, "ray", 3);

    int64_t start_raw[] = {1, 3, 2};
    ray_t* start_col = ray_vec_from_raw(RAY_I64, start_raw, 3);

    int64_t nm = ray_sym_intern("name", 4);
    int64_t ns = ray_sym_intern("start", 5);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, nm, col);
    tbl = ray_table_add_col(tbl, ns, start_col);
    ray_release(col);
    ray_release(start_col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name   = ray_scan(g, "name");
    ray_op_t* start  = ray_scan(g, "start");   /* I64 vector */
    ray_op_t* len_op = ray_const_i64(g, 2);
    ray_op_t* sub    = ray_substr(g, name, start, len_op);
    ray_t* result    = ray_execute(g, sub);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 2);
    TEST_ASSERT_MEM_EQ(2, s0, "he");  /* "hello"[0..1] */

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_substr: I32 vector len → l_data_i32 branch (line 310) */
static test_result_t test_str_substr_i32_vec_len(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_STR, 3);
    col = ray_str_vec_append(col, "hello", 5);
    col = ray_str_vec_append(col, "world", 5);
    col = ray_str_vec_append(col, "foobar", 6);

    int32_t len_raw[] = {2, 3, 4};
    ray_t* len_col = ray_vec_from_raw(RAY_I32, len_raw, 3);

    int64_t nm = ray_sym_intern("name", 4);
    int64_t nl = ray_sym_intern("lenv", 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, nm, col);
    tbl = ray_table_add_col(tbl, nl, len_col);
    ray_release(col);
    ray_release(len_col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name   = ray_scan(g, "name");
    ray_op_t* start  = ray_const_i64(g, 1);
    ray_op_t* len_op = ray_scan(g, "lenv");   /* I32 vector */
    ray_op_t* sub    = ray_substr(g, name, start, len_op);
    ray_t* result    = ray_execute(g, sub);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 2);
    TEST_ASSERT_MEM_EQ(2, s0, "he");
    const char* s1 = ray_str_vec_get(result, 1, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, s1, "wor");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_substr: I64 vector len → l_data branch (line 311) */
static test_result_t test_str_substr_i64_vec_len(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_STR, 2);
    col = ray_str_vec_append(col, "hello", 5);
    col = ray_str_vec_append(col, "world", 5);

    int64_t len_raw[] = {4, 2};
    ray_t* len_col = ray_vec_from_raw(RAY_I64, len_raw, 2);

    int64_t nm = ray_sym_intern("name", 4);
    int64_t nl = ray_sym_intern("lenv", 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, nm, col);
    tbl = ray_table_add_col(tbl, nl, len_col);
    ray_release(col);
    ray_release(len_col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* name   = ray_scan(g, "name");
    ray_op_t* start  = ray_const_i64(g, 1);
    ray_op_t* len_op = ray_scan(g, "lenv");   /* I64 vector */
    ray_op_t* sub    = ray_substr(g, name, start, len_op);
    ray_t* result    = ray_execute(g, sub);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    size_t len;
    const char* s0 = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, 4);
    TEST_ASSERT_MEM_EQ(4, s0, "hell");
    const char* s1 = ray_str_vec_get(result, 1, &len);
    TEST_ASSERT_EQ_U(len, 2);
    TEST_ASSERT_MEM_EQ(2, s1, "wo");

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* exec_string_unary: large string (>= 8192 bytes) → scratch_alloc branch */
static test_result_t test_str_upper_large_string(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a string of 8200 'a' chars — forces scratch_alloc path */
    enum { BIG = 8200 };
    char big[BIG];
    memset(big, 'a', BIG);

    ray_t* col = ray_vec_new(RAY_STR, 1);
    col = ray_str_vec_append(col, big, BIG);

    int64_t nm = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nm, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* val  = ray_scan(g, "val");
    ray_op_t* up   = ray_upper(g, val);
    ray_t* result  = ray_execute(g, up);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_STR);
    TEST_ASSERT_EQ_I(result->len, 1);

    size_t len;
    const char* s = ray_str_vec_get(result, 0, &len);
    TEST_ASSERT_EQ_U(len, BIG);
    /* All chars should be uppercase 'A' */
    TEST_ASSERT_EQ_I(s[0], 'A');
    TEST_ASSERT_EQ_I(s[BIG - 1], 'A');

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

static test_result_t test_str_t_hash_inline(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);
    v = ray_str_vec_append(v, "hello", 5);
    v = ray_str_vec_append(v, "world", 5);

    ray_str_t* elems = (ray_str_t*)ray_data(v);
    uint64_t h0 = ray_str_t_hash(&elems[0], NULL);
    uint64_t h1 = ray_str_t_hash(&elems[1], NULL);
    uint64_t h2 = ray_str_t_hash(&elems[2], NULL);

    /* Same strings -> same hash */
    TEST_ASSERT_TRUE(h0 == h1);
    /* Different strings -> different hash (extremely likely) */
    TEST_ASSERT_TRUE(h0 != h2);

    ray_release(v);
    PASS();
}

static test_result_t test_str_t_hash_pooled(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "this is a long string!", 22);
    v = ray_str_vec_append(v, "this is a long string!", 22);
    v = ray_str_vec_append(v, "different long string!", 22);

    ray_str_t* elems = (ray_str_t*)ray_data(v);
    const char* pool = (const char*)ray_data(v->str_pool);
    uint64_t h0 = ray_str_t_hash(&elems[0], pool);
    uint64_t h1 = ray_str_t_hash(&elems[1], pool);
    uint64_t h2 = ray_str_t_hash(&elems[2], pool);

    TEST_ASSERT_TRUE(h0 == h1);
    TEST_ASSERT_TRUE(h0 != h2);

    ray_release(v);
    PASS();
}

static test_result_t test_str_t_hash_empty(void) {
    ray_t* v = ray_vec_new(RAY_STR, 2);
    v = ray_str_vec_append(v, "", 0);
    v = ray_str_vec_append(v, "", 0);

    ray_str_t* elems = (ray_str_t*)ray_data(v);
    uint64_t h0 = ray_str_t_hash(&elems[0], NULL);
    uint64_t h1 = ray_str_t_hash(&elems[1], NULL);
    TEST_ASSERT_TRUE(h0 == h1);

    ray_release(v);
    PASS();
}

static test_result_t test_str_vec_concat_pooled_rebase(void) {
    /* Verify that concat of two pooled RAY_STR vectors correctly rebases
     * pool offsets so that the second vector's strings resolve correctly. */
    ray_t* a = ray_vec_new(RAY_STR, 2);
    a = ray_str_vec_append(a, "this is a long pooled string a!", 30);
    ray_t* b = ray_vec_new(RAY_STR, 2);
    b = ray_str_vec_append(b, "this is a long pooled string b!", 30);

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(ray_len(c), 2);

    /* Verify rebased pool offset resolves correctly */
    size_t len;
    const char* p1 = ray_str_vec_get(c, 1, &len);
    TEST_ASSERT_EQ_U(len, 30);
    TEST_ASSERT_MEM_EQ(30, p1, "this is a long pooled string b!");

    ray_release(c);
    ray_release(a);
    ray_release(b);
    PASS();
}

static test_result_t test_str_vec_concat_nulls(void) {
    ray_t* a = ray_vec_new(RAY_STR, 3);
    a = ray_str_vec_append(a, "hello", 5);
    a = ray_str_vec_append(a, "world", 5);
    a = ray_str_vec_append(a, "foo", 3);
    ray_vec_set_null(a, 1, true);

    ray_t* b = ray_vec_new(RAY_STR, 2);
    b = ray_str_vec_append(b, "bar", 3);
    b = ray_str_vec_append(b, "baz", 3);
    ray_vec_set_null(b, 0, true);

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(ray_len(c), 5);

    /* a's nulls preserved */
    TEST_ASSERT_FALSE(ray_vec_is_null(c, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(c, 1));   /* a[1] was null */
    TEST_ASSERT_FALSE(ray_vec_is_null(c, 2));
    /* b's nulls preserved at offset a->len */
    TEST_ASSERT_TRUE(ray_vec_is_null(c, 3));    /* b[0] was null */
    TEST_ASSERT_FALSE(ray_vec_is_null(c, 4));

    ray_release(c);
    ray_release(a);
    ray_release(b);
    PASS();
}

/* ---- str_vec_slice_null ------------------------------------------------ */

static test_result_t test_str_vec_slice_null(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);
    v = ray_str_vec_append(v, "world", 5);
    v = ray_str_vec_append(v, "foo", 3);
    v = ray_str_vec_append(v, "bar", 3);
    ray_vec_set_null(v, 1, true);
    ray_vec_set_null(v, 3, true);

    /* Slice [1..3) — includes null at parent index 1 */
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

static test_result_t test_str_vec_cow_append(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);
    v = ray_str_vec_append(v, "this is a long string!", 22);

    /* Share the vector (rc=2) */
    ray_retain(v);
    ray_t* shared = v;

    /* Append to shared vec — triggers COW */
    v = ray_str_vec_append(v, "new", 3);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(ray_len(v), 3);

    /* Original should still have 2 elements */
    TEST_ASSERT_EQ_I(ray_len(shared), 2);

    /* Both should have correct data */
    size_t len;
    const char* p = ray_str_vec_get(v, 2, &len);
    TEST_ASSERT_EQ_U(len, 3);
    TEST_ASSERT_MEM_EQ(3, p, "new");

    const char* orig = ray_str_vec_get(shared, 1, &len);
    TEST_ASSERT_EQ_U(len, 22);
    TEST_ASSERT_MEM_EQ(22, orig, "this is a long string!");

    ray_release(v);
    ray_release(shared);
    PASS();
}

static test_result_t test_str_vec_cow_set(void) {
    ray_t* v = ray_vec_new(RAY_STR, 4);
    v = ray_str_vec_append(v, "hello", 5);
    v = ray_str_vec_append(v, "world", 5);

    /* Share */
    ray_retain(v);
    ray_t* shared = v;

    /* Set on shared vec — triggers COW */
    v = ray_str_vec_set(v, 0, "changed", 7);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* Original preserved */
    size_t len;
    const char* orig = ray_str_vec_get(shared, 0, &len);
    TEST_ASSERT_EQ_U(len, 5);
    TEST_ASSERT_MEM_EQ(5, orig, "hello");

    const char* changed = ray_str_vec_get(v, 0, &len);
    TEST_ASSERT_EQ_U(len, 7);
    TEST_ASSERT_MEM_EQ(7, changed, "changed");

    ray_release(v);
    ray_release(shared);
    PASS();
}

const test_entry_t str_entries[] = {
    { "str/ptr_sso", test_str_ptr_sso, str_setup, str_teardown },
    { "str/ptr_long", test_str_ptr_long, str_setup, str_teardown },
    { "str/len", test_str_len, str_setup, str_teardown },
    { "str/cmp_equal", test_str_cmp_equal, str_setup, str_teardown },
    { "str/cmp_different", test_str_cmp_different, str_setup, str_teardown },
    { "str/cmp_prefix", test_str_cmp_prefix, str_setup, str_teardown },
    { "str/vec_new", test_str_vec_new, str_setup, str_teardown },
    { "str/vec_new_zero", test_str_vec_new_zero_cap, str_setup, str_teardown },
    { "str/vec_append_inline", test_str_vec_append_inline, str_setup, str_teardown },
    { "str/vec_append_inline_12", test_str_vec_append_inline_12, str_setup, str_teardown },
    { "str/vec_append_empty", test_str_vec_append_empty, str_setup, str_teardown },
    { "str/vec_append_pooled", test_str_vec_append_pooled, str_setup, str_teardown },
    { "str/vec_append_mixed", test_str_vec_append_mixed, str_setup, str_teardown },
    { "str/vec_get_inline", test_str_vec_get_inline, str_setup, str_teardown },
    { "str/vec_get_pooled", test_str_vec_get_pooled, str_setup, str_teardown },
    { "str/vec_get_oob", test_str_vec_get_oob, str_setup, str_teardown },
    { "str/vec_set_i2i", test_str_vec_set_inline_to_inline, str_setup, str_teardown },
    { "str/vec_set_p2p", test_str_vec_set_pooled_to_pooled, str_setup, str_teardown },
    { "str/vec_set_oob", test_str_vec_set_oob, str_setup, str_teardown },
    { "str/vec_set_i2p", test_str_vec_set_inline_to_pooled, str_setup, str_teardown },
    { "str/vec_set_p2i", test_str_vec_set_pooled_to_inline, str_setup, str_teardown },
    { "str/vec_get_neg", test_str_vec_get_negative, str_setup, str_teardown },
    { "str/vec_set_neg", test_str_vec_set_negative, str_setup, str_teardown },
    { "str/vec_compact", test_str_vec_compact, str_setup, str_teardown },
    { "str/vec_compact_noop", test_str_vec_compact_noop, str_setup, str_teardown },
    { "str/vec_compact_all_dead", test_str_vec_compact_all_dead, str_setup, str_teardown },
    { "str/vec_compact_all_pool_dead", test_str_vec_compact_all_pool_dead, str_setup, str_teardown },
    { "str/vec_compact_saturated_dead", test_str_vec_compact_saturated_dead, str_setup, str_teardown },
    { "str/t_eq_inline", test_str_t_eq_inline, str_setup, str_teardown },
    { "str/t_eq_pooled", test_str_t_eq_pooled, str_setup, str_teardown },
    { "str/t_cmp_order", test_str_t_cmp_order, str_setup, str_teardown },
    { "str/vec_null", test_str_vec_null, str_setup, str_teardown },
    { "str/vec_null_pooled", test_str_vec_null_pooled, str_setup, str_teardown },
    { "str/vec_grow", test_str_vec_grow, str_setup, str_teardown },
    { "str/vec_slice", test_str_vec_slice, str_setup, str_teardown },
    { "str/vec_concat", test_str_vec_concat_vecs, str_setup, str_teardown },
    { "str/t_hash_inline", test_str_t_hash_inline, str_setup, str_teardown },
    { "str/t_hash_pooled", test_str_t_hash_pooled, str_setup, str_teardown },
    { "str/t_hash_empty", test_str_t_hash_empty, str_setup, str_teardown },
    { "str/vec_concat_pooled_rebase", test_str_vec_concat_pooled_rebase, str_setup, str_teardown },
    { "str/vec_concat_nulls", test_str_vec_concat_nulls, str_setup, str_teardown },
    { "str/vec_slice_null", test_str_vec_slice_null, str_setup, str_teardown },
    { "str/vec_cow_append", test_str_vec_cow_append, str_setup, str_teardown },
    { "str/vec_cow_set", test_str_vec_cow_set, str_setup, str_teardown },
    /* Graph-level string.c coverage tests */
    { "str/like_non_string", test_str_like_non_string_type, NULL, NULL },
    { "str/ilike_non_string", test_str_ilike_non_string_type, NULL, NULL },
    { "str/ilike_str_col", test_str_ilike_str_column, NULL, NULL },
    { "str/ilike_sym_col", test_str_ilike_sym_column, NULL, NULL },
    { "str/like_str_col", test_str_like_str_column, NULL, NULL },
    { "str/upper_sym_null", test_str_upper_sym_null, NULL, NULL },
    { "str/lower_sym_null", test_str_lower_sym_null, NULL, NULL },
    { "str/trim_sym_null", test_str_trim_sym_null, NULL, NULL },
    { "str/strlen_sym_null", test_str_strlen_sym_null, NULL, NULL },
    { "str/substr_sym", test_str_substr_sym, NULL, NULL },
    { "str/substr_sym_null", test_str_substr_sym_null, NULL, NULL },
    { "str/substr_sym_oor", test_str_substr_sym_out_of_range, NULL, NULL },
    { "str/replace_sym", test_str_replace_sym, NULL, NULL },
    { "str/replace_sym_null", test_str_replace_sym_null, NULL, NULL },
    { "str/replace_str_shrink", test_str_replace_str_shrink, NULL, NULL },
    { "str/concat_many_args", test_str_concat_many_args, NULL, NULL },
    { "str/concat_sym_null", test_str_concat_sym_null, NULL, NULL },
    { "str/substr_f64_scalar_start", test_str_substr_f64_scalar_start, NULL, NULL },
    { "str/substr_f64_scalar_len", test_str_substr_f64_scalar_len, NULL, NULL },
    { "str/substr_i32_vec_start", test_str_substr_i32_vec_start, NULL, NULL },
    { "str/substr_i64_vec_start", test_str_substr_i64_vec_start, NULL, NULL },
    { "str/substr_i32_vec_len", test_str_substr_i32_vec_len, NULL, NULL },
    { "str/substr_i64_vec_len", test_str_substr_i64_vec_len, NULL, NULL },
    { "str/upper_large_string", test_str_upper_large_string, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


