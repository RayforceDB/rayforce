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

/* ---- list_new_negative_cap --------------------------------------------- */

/* Negative capacity must return a "range" RAY_ERROR. */
static test_result_t test_list_new_negative_cap(void) {
    ray_t* list = ray_list_new(-1);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_TRUE(RAY_IS_ERR(list));
    PASS();
}

/* ---- list_append_err_inputs -------------------------------------------- */

/* ray_list_append with NULL list returns NULL (early-return). */
static test_result_t test_list_append_err_inputs(void) {
    /* NULL list short-circuits to NULL. */
    ray_t* r = ray_list_append(NULL, NULL);
    TEST_ASSERT_NULL(r);

    /* Error list short-circuits, propagating the error. */
    ray_t* err = ray_error("range", NULL);
    ray_t* r2 = ray_list_append(err, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));

    PASS();
}

/* ---- list_append_null_item --------------------------------------------- */

/* Appending NULL stores a NULL slot without retain; ray_list_get returns NULL. */
static test_result_t test_list_append_null_item(void) {
    ray_t* list = ray_list_new(2);
    list = ray_list_append(list, NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    TEST_ASSERT_EQ_I(list->len, 1);

    ray_t* g = ray_list_get(list, 0);
    TEST_ASSERT_NULL(g);

    ray_release(list);
    PASS();
}

/* ---- list_set_err_inputs ----------------------------------------------- */

/* ray_list_set with NULL/err list and out-of-range idx. */
static test_result_t test_list_set_err_inputs(void) {
    /* NULL list: returns NULL. */
    ray_t* r = ray_list_set(NULL, 0, NULL);
    TEST_ASSERT_NULL(r);

    /* Negative idx: range error. */
    ray_t* list = ray_list_new(2);
    ray_t* a = ray_i64(7);
    list = ray_list_append(list, a);

    ray_t* err = ray_list_set(list, -1, a);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));

    /* Set NULL onto an existing slot: drops old ref, stores NULL, no retain. */
    list = ray_list_set(list, 0, NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    ray_t* got = ray_list_get(list, 0);
    TEST_ASSERT_NULL(got);

    ray_release(a);
    ray_release(list);
    PASS();
}

/* ---- list_get_err_inputs ----------------------------------------------- */

/* ray_list_get on NULL and on an error pointer returns NULL. */
static test_result_t test_list_get_err_inputs(void) {
    ray_t* g = ray_list_get(NULL, 0);
    TEST_ASSERT_NULL(g);

    ray_t* err = ray_error("range", NULL);
    ray_t* g2 = ray_list_get(err, 0);
    TEST_ASSERT_NULL(g2);

    /* Negative idx on a real list: NULL. */
    ray_t* list = ray_list_new(1);
    ray_t* a = ray_i64(1);
    list = ray_list_append(list, a);
    ray_t* g3 = ray_list_get(list, -1);
    TEST_ASSERT_NULL(g3);

    ray_release(a);
    ray_release(list);
    PASS();
}

/* ---- list_insert_at ---------------------------------------------------- */

static test_result_t test_list_insert_at(void) {
    ray_t* list = ray_list_new(2);
    ray_t* a = ray_i64(1);
    ray_t* b = ray_i64(2);
    ray_t* c = ray_i64(3);
    ray_t* d = ray_i64(4);

    /* Append two -> [a, b] */
    list = ray_list_append(list, a);
    list = ray_list_append(list, b);
    TEST_ASSERT_EQ_I(list->len, 2);

    /* Insert at front: [c, a, b] */
    list = ray_list_insert_at(list, 0, c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    TEST_ASSERT_EQ_I(list->len, 3);
    TEST_ASSERT_EQ_PTR(ray_list_get(list, 0), c);
    TEST_ASSERT_EQ_PTR(ray_list_get(list, 1), a);
    TEST_ASSERT_EQ_PTR(ray_list_get(list, 2), b);

    /* Insert at end (idx == len), exercises append branch: [c, a, b, d] */
    list = ray_list_insert_at(list, list->len, d);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    TEST_ASSERT_EQ_I(list->len, 4);
    TEST_ASSERT_EQ_PTR(ray_list_get(list, 3), d);

    /* Range errors */
    ray_t* err1 = ray_list_insert_at(list, -1, a);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err1));
    ray_t* err2 = ray_list_insert_at(list, list->len + 1, a);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err2));

    /* NULL/err input */
    ray_t* err3 = ray_list_insert_at(NULL, 0, a);
    TEST_ASSERT_NULL(err3);

    /* Type error: pass a non-RAY_LIST. ray_i64 produces an atom (type < 0). */
    ray_t* atom = ray_i64(99);
    ray_t* err4 = ray_list_insert_at(atom, 0, a);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err4));
    ray_release(atom);

    ray_release(a);
    ray_release(b);
    ray_release(c);
    ray_release(d);
    ray_release(list);
    PASS();
}

/* ---- list_insert_at_grow ------------------------------------------------ */

/* Force the realloc/grow branch in ray_list_insert_at. */
static test_result_t test_list_insert_at_grow(void) {
    ray_t* list = ray_list_new(1);
    ray_t* items[16];
    for (int i = 0; i < 16; i++) items[i] = ray_i64((int64_t)i);

    /* Insert each at front — len grows from 0..16, repeatedly hitting grow. */
    for (int i = 0; i < 16; i++) {
        list = ray_list_insert_at(list, 0, items[i]);
        TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    }
    TEST_ASSERT_EQ_I(list->len, 16);
    /* Last inserted at front is items[15]. */
    TEST_ASSERT_EQ_PTR(ray_list_get(list, 0), items[15]);
    TEST_ASSERT_EQ_PTR(ray_list_get(list, 15), items[0]);

    for (int i = 0; i < 16; i++) ray_release(items[i]);
    ray_release(list);
    PASS();
}

/* ---- list_insert_many_parallel ----------------------------------------- */

static test_result_t test_list_insert_many_parallel(void) {
    /* base: [a, b] */
    ray_t* list = ray_list_new(2);
    ray_t* a = ray_i64(10);
    ray_t* b = ray_i64(20);
    list = ray_list_append(list, a);
    list = ray_list_append(list, b);

    /* idxs = [0, 2], vals = [x, y]; expect [x, a, b, y] */
    ray_t* idxs = ray_vec_new(RAY_I64, 2);
    int64_t i0 = 0, i1 = 2;
    idxs = ray_vec_append(idxs, &i0);
    idxs = ray_vec_append(idxs, &i1);

    ray_t* x = ray_i64(100);
    ray_t* y = ray_i64(200);
    ray_t* vals = ray_list_new(2);
    vals = ray_list_append(vals, x);
    vals = ray_list_append(vals, y);

    ray_t* result = ray_list_insert_many(list, idxs, vals);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->len, 4);
    TEST_ASSERT_EQ_PTR(ray_list_get(result, 0), x);
    TEST_ASSERT_EQ_PTR(ray_list_get(result, 1), a);
    TEST_ASSERT_EQ_PTR(ray_list_get(result, 2), b);
    TEST_ASSERT_EQ_PTR(ray_list_get(result, 3), y);

    ray_release(idxs);
    ray_release(vals);
    ray_release(result);
    ray_release(x);
    ray_release(y);
    ray_release(a);
    ray_release(b);
    ray_release(list);
    PASS();
}

/* ---- list_insert_many_broadcast ---------------------------------------- */

static test_result_t test_list_insert_many_broadcast(void) {
    /* base: [a] */
    ray_t* list = ray_list_new(1);
    ray_t* a = ray_i64(10);
    list = ray_list_append(list, a);

    /* idxs = [0, 1, 1] (out-of-order, duplicates), vals = [b] (broadcast) */
    ray_t* idxs = ray_vec_new(RAY_I64, 3);
    int64_t i0 = 1, i1 = 0, i2 = 1;
    idxs = ray_vec_append(idxs, &i0);
    idxs = ray_vec_append(idxs, &i1);
    idxs = ray_vec_append(idxs, &i2);

    ray_t* b = ray_i64(99);
    ray_t* vals = ray_list_new(1);
    vals = ray_list_append(vals, b);

    ray_t* result = ray_list_insert_many(list, idxs, vals);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->len, 4);
    /* All inserted slots point to b; original a is somewhere in the result. */
    int seen_a = 0;
    int seen_b = 0;
    for (int64_t i = 0; i < 4; i++) {
        ray_t* g = ray_list_get(result, i);
        if (g == a) seen_a++;
        if (g == b) seen_b++;
    }
    TEST_ASSERT_EQ_I(seen_a, 1);
    TEST_ASSERT_EQ_I(seen_b, 3);

    ray_release(idxs);
    ray_release(vals);
    ray_release(result);
    ray_release(a);
    ray_release(b);
    ray_release(list);
    PASS();
}

/* ---- list_insert_many_empty -------------------------------------------- */

/* N == 0 path: returns the same list with bumped refcount. */
static test_result_t test_list_insert_many_empty(void) {
    ray_t* list = ray_list_new(1);
    ray_t* a = ray_i64(7);
    list = ray_list_append(list, a);

    ray_t* idxs = ray_vec_new(RAY_I64, 0);
    ray_t* vals = ray_list_new(0);

    uint64_t rc_before = list->rc;
    ray_t* result = ray_list_insert_many(list, idxs, vals);
    TEST_ASSERT_EQ_PTR(result, list);
    TEST_ASSERT_EQ_U(list->rc, rc_before + 1);

    ray_release(result);  /* drops the extra ref */
    ray_release(idxs);
    ray_release(vals);
    ray_release(a);
    ray_release(list);
    PASS();
}

/* ---- list_insert_many_errs --------------------------------------------- */

static test_result_t test_list_insert_many_errs(void) {
    ray_t* list = ray_list_new(2);
    ray_t* a = ray_i64(1);
    list = ray_list_append(list, a);

    /* NULL inputs propagate. */
    ray_t* r = ray_list_insert_many(NULL, NULL, NULL);
    TEST_ASSERT_NULL(r);

    ray_t* idxs_ok = ray_vec_new(RAY_I64, 1);
    int64_t z = 0;
    idxs_ok = ray_vec_append(idxs_ok, &z);

    ray_t* vals_ok = ray_list_new(1);
    ray_t* v = ray_i64(42);
    vals_ok = ray_list_append(vals_ok, v);

    /* idxs NULL: returns NULL. */
    r = ray_list_insert_many(list, NULL, vals_ok);
    TEST_ASSERT_NULL(r);

    /* vals NULL: returns NULL. */
    r = ray_list_insert_many(list, idxs_ok, NULL);
    TEST_ASSERT_NULL(r);

    /* Wrong list type — pass an atom as the list arg. */
    ray_t* atom = ray_i64(0);
    r = ray_list_insert_many(atom, idxs_ok, vals_ok);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(atom);

    /* Wrong idxs type — pass a RAY_F64 vec. */
    ray_t* fidxs = ray_vec_new(RAY_F64, 1);
    double f = 0.0;
    fidxs = ray_vec_append(fidxs, &f);
    r = ray_list_insert_many(list, fidxs, vals_ok);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(fidxs);

    /* Wrong vals type — pass a RAY_I64 vec where a RAY_LIST is required. */
    ray_t* ivals = ray_vec_new(RAY_I64, 1);
    int64_t one = 1;
    ivals = ray_vec_append(ivals, &one);
    r = ray_list_insert_many(list, idxs_ok, ivals);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(ivals);

    /* Out-of-range idx (idx > old_len). list->len == 1, so idx=5 is too big. */
    ray_t* idxs_oor = ray_vec_new(RAY_I64, 1);
    int64_t big = 5;
    idxs_oor = ray_vec_append(idxs_oor, &big);
    r = ray_list_insert_many(list, idxs_oor, vals_ok);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(idxs_oor);

    /* vals.len mismatch: vals has 2 elements, idxs has 1 (and 1 != 2 != broadcast=1). */
    ray_t* vals_bad = ray_list_new(2);
    ray_t* v1 = ray_i64(1);
    ray_t* v2 = ray_i64(2);
    vals_bad = ray_list_append(vals_bad, v1);
    vals_bad = ray_list_append(vals_bad, v2);
    r = ray_list_insert_many(list, idxs_ok, vals_bad);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(v1);
    ray_release(v2);
    ray_release(vals_bad);

    ray_release(v);
    ray_release(idxs_ok);
    ray_release(vals_ok);
    ray_release(a);
    ray_release(list);
    PASS();
}

/* ---- list_append_cow --------------------------------------------------- */

/* Appending to a shared list (rc > 1) exercises the COW-copy branch. */
static test_result_t test_list_append_cow(void) {
    ray_t* list = ray_list_new(2);
    ray_t* a = ray_i64(1);
    ray_t* b = ray_i64(2);
    list = ray_list_append(list, a);

    /* Bump refcount so COW must make a copy */
    ray_retain(list);
    ray_t* shared = list;

    ray_t* list2 = ray_list_append(list, b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list2));
    TEST_ASSERT_EQ_I(list2->len, 2);
    /* shared still has len==1 */
    TEST_ASSERT_EQ_I(shared->len, 1);

    /* Release the extra ref and copies */
    ray_release(shared);
    ray_release(list2);
    ray_release(a);
    ray_release(b);
    PASS();
}

/* ---- list_set_cow ------------------------------------------------------ */

/* Calling ray_list_set on a shared list (rc > 1) exercises the COW path. */
static test_result_t test_list_set_cow(void) {
    ray_t* list = ray_list_new(2);
    ray_t* a = ray_i64(10);
    ray_t* b = ray_i64(20);
    list = ray_list_append(list, a);
    list = ray_list_append(list, b);

    /* Bump rc so COW must copy */
    ray_retain(list);
    ray_t* shared = list;

    ray_t* c = ray_i64(99);
    ray_t* list2 = ray_list_set(list, 0, c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list2));

    /* list2 is a COW copy; shared[0] is still a */
    ray_t* got_shared = ray_list_get(shared, 0);
    TEST_ASSERT_EQ_PTR(got_shared, a);

    /* list2[0] is c */
    ray_t* got_new = ray_list_get(list2, 0);
    TEST_ASSERT_EQ_PTR(got_new, c);

    ray_release(shared);
    ray_release(list2);
    ray_release(a);
    ray_release(b);
    ray_release(c);
    PASS();
}

/* ---- list_insert_at_cow ------------------------------------------------ */

/* ray_list_insert_at on a shared list exercises the COW path. */
static test_result_t test_list_insert_at_cow(void) {
    ray_t* list = ray_list_new(2);
    ray_t* a = ray_i64(1);
    ray_t* b = ray_i64(2);
    list = ray_list_append(list, a);
    list = ray_list_append(list, b);

    /* Bump rc so COW must copy */
    ray_retain(list);
    ray_t* shared = list;

    ray_t* c = ray_i64(0);
    ray_t* list2 = ray_list_insert_at(list, 0, c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list2));
    TEST_ASSERT_EQ_I(list2->len, 3);

    /* shared is unchanged */
    TEST_ASSERT_EQ_I(shared->len, 2);
    TEST_ASSERT_EQ_PTR(ray_list_get(shared, 0), a);

    ray_release(shared);
    ray_release(list2);
    ray_release(a);
    ray_release(b);
    ray_release(c);
    PASS();
}

/* ---- list_set_err_ptr -------------------------------------------------- */

/* Passing an error pointer as list to ray_list_set propagates it. */
static test_result_t test_list_set_err_ptr(void) {
    ray_t* err = ray_error("range", NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));

    ray_t* r = ray_list_set(err, 0, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_PTR(r, err);

    PASS();
}

/* ---- list_insert_at_err_ptr -------------------------------------------- */

/* Passing an error pointer as list to ray_list_insert_at propagates it. */
static test_result_t test_list_insert_at_err_ptr(void) {
    ray_t* err = ray_error("type", NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));

    ray_t* r = ray_list_insert_at(err, 0, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_PTR(r, err);

    PASS();
}

/* ---- list_insert_many_err_ptrs ----------------------------------------- */

/* Passing error pointers for list, idxs, and vals to ray_list_insert_many. */
static test_result_t test_list_insert_many_err_ptrs(void) {
    ray_t* list = ray_list_new(1);
    ray_t* a = ray_i64(1);
    list = ray_list_append(list, a);

    ray_t* idxs = ray_vec_new(RAY_I64, 1);
    int64_t z = 0;
    idxs = ray_vec_append(idxs, &z);

    ray_t* vals = ray_list_new(1);
    ray_t* v = ray_i64(42);
    vals = ray_list_append(vals, v);

    /* Error pointer as list propagates. */
    ray_t* err_list = ray_error("type", NULL);
    ray_t* r = ray_list_insert_many(err_list, idxs, vals);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_PTR(r, err_list);

    /* Error pointer as idxs propagates. */
    ray_t* err_idxs = ray_error("type", NULL);
    r = ray_list_insert_many(list, err_idxs, vals);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_PTR(r, err_idxs);

    /* Error pointer as vals propagates. */
    ray_t* err_vals = ray_error("type", NULL);
    r = ray_list_insert_many(list, idxs, err_vals);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_PTR(r, err_vals);

    ray_release(idxs);
    ray_release(vals);
    ray_release(a);
    ray_release(v);
    ray_release(list);
    PASS();
}

/* ---- list_insert_many_large -------------------------------------------- */

/* Insert many items into a non-trivial list to cover the merge loop more
   thoroughly (r == old_len iteration, boundary cases in the merge). */
static test_result_t test_list_insert_many_large(void) {
    /* base: [0, 1, 2, 3, 4] */
    ray_t* list = ray_list_new(5);
    ray_t* items[5];
    for (int i = 0; i < 5; i++) {
        items[i] = ray_i64((int64_t)i);
        list = ray_list_append(list, items[i]);
    }
    TEST_ASSERT_EQ_I(list->len, 5);

    /* Insert at positions 0, 2, 5 (end) — out of order to exercise sort */
    ray_t* idxs = ray_vec_new(RAY_I64, 3);
    int64_t p0 = 5, p1 = 0, p2 = 2;
    idxs = ray_vec_append(idxs, &p0);
    idxs = ray_vec_append(idxs, &p1);
    idxs = ray_vec_append(idxs, &p2);

    ray_t* x = ray_i64(10);
    ray_t* y = ray_i64(20);
    ray_t* z = ray_i64(30);
    ray_t* vals = ray_list_new(3);
    vals = ray_list_append(vals, x);
    vals = ray_list_append(vals, y);
    vals = ray_list_append(vals, z);

    /* After sorted insertion at pre-insertion positions [0, 2, 5]:
       y inserted at 0, z inserted at 2, x inserted at 5 (end)
       Merge: r=0 -> y, items[0]; r=1 -> items[1]; r=2 -> z, items[2];
              r=3 -> items[3]; r=4 -> items[4]; r=5 -> x
       => [y, 0, 1, z, 2, 3, 4, x] */
    ray_t* result = ray_list_insert_many(list, idxs, vals);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->len, 8);
    TEST_ASSERT_EQ_PTR(ray_list_get(result, 0), y);
    TEST_ASSERT_EQ_PTR(ray_list_get(result, 3), z);
    TEST_ASSERT_EQ_PTR(ray_list_get(result, 7), x);

    ray_release(idxs);
    ray_release(vals);
    ray_release(result);
    for (int i = 0; i < 5; i++) ray_release(items[i]);
    ray_release(x);
    ray_release(y);
    ray_release(z);
    ray_release(list);
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
    { "list/new_negative_cap", test_list_new_negative_cap, list_setup, list_teardown },
    { "list/append_err_inputs", test_list_append_err_inputs, list_setup, list_teardown },
    { "list/append_null_item", test_list_append_null_item, list_setup, list_teardown },
    { "list/set_err_inputs", test_list_set_err_inputs, list_setup, list_teardown },
    { "list/get_err_inputs", test_list_get_err_inputs, list_setup, list_teardown },
    { "list/insert_at", test_list_insert_at, list_setup, list_teardown },
    { "list/insert_at_grow", test_list_insert_at_grow, list_setup, list_teardown },
    { "list/insert_many_parallel", test_list_insert_many_parallel, list_setup, list_teardown },
    { "list/insert_many_broadcast", test_list_insert_many_broadcast, list_setup, list_teardown },
    { "list/insert_many_empty", test_list_insert_many_empty, list_setup, list_teardown },
    { "list/insert_many_errs", test_list_insert_many_errs, list_setup, list_teardown },
    { "list/append_cow", test_list_append_cow, list_setup, list_teardown },
    { "list/set_cow", test_list_set_cow, list_setup, list_teardown },
    { "list/insert_at_cow", test_list_insert_at_cow, list_setup, list_teardown },
    { "list/set_err_ptr", test_list_set_err_ptr, list_setup, list_teardown },
    { "list/insert_at_err_ptr", test_list_insert_at_err_ptr, list_setup, list_teardown },
    { "list/insert_many_err_ptrs", test_list_insert_many_err_ptrs, list_setup, list_teardown },
    { "list/insert_many_large", test_list_insert_many_large, list_setup, list_teardown },
    { NULL, NULL, NULL, NULL },
};


