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

static void list_setup(void) {
    ray_heap_init();
}

static void list_teardown(void) {
    ray_heap_destroy();
}

/* ---- list_new ---------------------------------------------------------- */

static test_result_t test_list_new(void) {
    ray_t* list = ray_list_new(4);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    TEST_ASSERT_EQ_I(list->type, RAY_LIST);
    TEST_ASSERT_EQ_I(list->len, 0);
    TEST_ASSERT_FALSE(ray_is_atom(list));
    TEST_ASSERT_FALSE(ray_is_vec(list));  /* type==0, neither atom nor vec */

    ray_release(list);
    PASS();
}

/* ---- list_append_get --------------------------------------------------- */

static test_result_t test_list_append_get(void) {
    ray_t* list = ray_list_new(4);

    ray_t* a = ray_i64(42);
    ray_t* b = ray_f64(3.14);

    list = ray_list_append(list, a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    TEST_ASSERT_EQ_I(list->len, 1);

    list = ray_list_append(list, b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    TEST_ASSERT_EQ_I(list->len, 2);

    ray_t* got0 = ray_list_get(list, 0);
    TEST_ASSERT_EQ_PTR(got0, a);
    TEST_ASSERT_EQ_I(got0->i64, 42);

    ray_t* got1 = ray_list_get(list, 1);
    TEST_ASSERT_EQ_PTR(got1, b);
    TEST_ASSERT((got1->f64) == (3.14), "double == failed");

    /* Out of range */
    ray_t* oob = ray_list_get(list, 2);
    TEST_ASSERT_NULL(oob);

    /* Release items, then list.
     * list_append retained a and b, so we release our original refs. */
    ray_release(a);
    ray_release(b);
    /* Now the list holds the only refs. Destroy arena cleans up. */

    ray_release(list);
    PASS();
}

/* ---- list_set ---------------------------------------------------------- */

static test_result_t test_list_set(void) {
    ray_t* list = ray_list_new(4);
    ray_t* a = ray_i64(10);
    ray_t* b = ray_i64(20);
    ray_t* c = ray_i64(30);

    list = ray_list_append(list, a);
    list = ray_list_append(list, b);
    TEST_ASSERT_EQ_I(list->len, 2);

    /* Replace index 0 with c */
    list = ray_list_set(list, 0, c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));

    ray_t* got = ray_list_get(list, 0);
    TEST_ASSERT_EQ_PTR(got, c);
    TEST_ASSERT_EQ_I(got->i64, 30);

    /* Out of range */
    ray_t* err = ray_list_set(list, 5, a);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));

    ray_release(a);
    ray_release(b);
    ray_release(c);
    ray_release(list);
    PASS();
}

/* ---- list_grow --------------------------------------------------------- */

static test_result_t test_list_grow(void) {
    ray_t* list = ray_list_new(1);

    /* Append many items to force reallocation */
    ray_t* items[20];
    for (int i = 0; i < 20; i++) {
        items[i] = ray_i64((int64_t)i);
        list = ray_list_append(list, items[i]);
        TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    }

    TEST_ASSERT_EQ_I(list->len, 20);

    /* Verify all items */
    for (int i = 0; i < 20; i++) {
        ray_t* got = ray_list_get(list, (int64_t)i);
        TEST_ASSERT_NOT_NULL(got);
        TEST_ASSERT_EQ_I(got->i64, (int64_t)i);
    }

    for (int i = 0; i < 20; i++) ray_release(items[i]);
    ray_release(list);
    PASS();
}

/* ---- list_empty -------------------------------------------------------- */

static test_result_t test_list_empty(void) {
    ray_t* list = ray_list_new(0);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    TEST_ASSERT_EQ_I(list->len, 0);

    ray_t* got = ray_list_get(list, 0);
    TEST_ASSERT_NULL(got);

    ray_release(list);
    PASS();
}

/* ---- list_mixed_types -------------------------------------------------- */

static test_result_t test_list_mixed_types(void) {
    ray_t* list = ray_list_new(4);

    ray_t* a = ray_i64(42);
    ray_t* b = ray_f64(2.718);
    ray_t* c = ray_bool(true);
    ray_t* d = ray_str("hi", 2);

    list = ray_list_append(list, a);
    list = ray_list_append(list, b);
    list = ray_list_append(list, c);
    list = ray_list_append(list, d);

    TEST_ASSERT_EQ_I(list->len, 4);

    ray_t* g0 = ray_list_get(list, 0);
    TEST_ASSERT_EQ_I(g0->type, -RAY_I64);
    TEST_ASSERT_EQ_I(g0->i64, 42);

    ray_t* g1 = ray_list_get(list, 1);
    TEST_ASSERT_EQ_I(g1->type, -RAY_F64);
    TEST_ASSERT((g1->f64) == (2.718), "double == failed");

    ray_t* g2 = ray_list_get(list, 2);
    TEST_ASSERT_EQ_I(g2->type, -RAY_BOOL);
    TEST_ASSERT_EQ_U(g2->b8, 1);

    ray_t* g3 = ray_list_get(list, 3);
    TEST_ASSERT_EQ_I(g3->type, -RAY_STR);

    ray_release(a);
    ray_release(b);
    ray_release(c);
    ray_release(d);
    ray_release(list);
    PASS();
}

/* ---- list_release_drops_item_ref ---------------------------------------- */

static test_result_t test_list_release_drops_item_ref(void) {
    ray_t* list = ray_list_new(1);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));

    ray_t* item = ray_i64(42);
    TEST_ASSERT_NOT_NULL(item);
    TEST_ASSERT_FALSE(RAY_IS_ERR(item));

    list = ray_list_append(list, item);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    TEST_ASSERT_EQ_U(item->rc, 2);

    ray_release(list);
    TEST_ASSERT_EQ_U(item->rc, 1);

    ray_release(item);
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t list_entries[] = {
    { "list/new", test_list_new, list_setup, list_teardown },
    { "list/append_get", test_list_append_get, list_setup, list_teardown },
    { "list/set", test_list_set, list_setup, list_teardown },
    { "list/grow", test_list_grow, list_setup, list_teardown },
    { "list/empty", test_list_empty, list_setup, list_teardown },
    { "list/mixed_types", test_list_mixed_types, list_setup, list_teardown },
    { "list/release_drops_item_ref", test_list_release_drops_item_ref, list_setup, list_teardown },
    { NULL, NULL, NULL, NULL },
};


