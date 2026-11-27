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
#include <stdlib.h>
#include "vec/str.h"
#include "lang/eval.h"
#include "core/runtime.h"

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

#ifdef DEBUG
/* ---- a failed growing write leaves a shared input as it was (#1857) ---- */

/* A full vector (its next append must grow it): I64 0, 1, 2, ... */
static ray_t* cow_full_i64(void) {
    ray_t* v = ray_vec_new(RAY_I64, 1);
    if (!v || RAY_IS_ERR(v)) return NULL;
    int64_t x = 0;
    while (!v->len || (size_t)v->len * sizeof(int64_t) < ray_block_data_bytes(v)) {
        ray_t* nv = ray_vec_append(v, &x);
        if (!nv || RAY_IS_ERR(nv)) return NULL;
        v = nv; x++;
    }
    return v;
}
static ray_t* cow_full_list(void) {
    ray_t* l = ray_list_new(1);
    if (!l || RAY_IS_ERR(l)) return NULL;
    while (!l->len || (size_t)l->len * sizeof(ray_t*) < ray_block_data_bytes(l)) {
        ray_t* a = ray_i64(l->len);
        ray_t* nl = ray_list_append(l, a);
        ray_release(a);
        if (!nl || RAY_IS_ERR(nl)) return NULL;
        l = nl;
    }
    return l;
}
static ray_t* cow_full_str(void) {
    ray_t* v = ray_vec_new(RAY_STR, 1);
    if (!v || RAY_IS_ERR(v)) return NULL;
    while (!v->len || (size_t)v->len * sizeof(ray_str_t) < ray_block_data_bytes(v)) {
        ray_t* nv = ray_str_vec_append(v, "a string longer than inline", 27);
        if (!nv || RAY_IS_ERR(nv)) return NULL;
        v = nv;
    }
    return v;
}

typedef ray_t* (*cow_op_t)(ray_t*);
static ray_t* op_vec_append(ray_t* v)     { int64_t x = 99; return ray_vec_append(v, &x); }
static ray_t* op_vec_append_raw(ray_t* v) { int64_t x[3] = {7, 8, 9}; return ray_vec_append_raw(v, x, 3); }
static ray_t* op_vec_insert_at(ray_t* v)  { int64_t x = 99; return ray_vec_insert_at(v, 1, &x); }
static ray_t* op_str_append(ray_t* v)     { return ray_str_vec_append(v, "another long string here", 24); }
static ray_t* op_str_set(ray_t* v)        { return ray_str_vec_set(v, 0, "a replacement longer than inline", 32); }
static ray_t* op_list_append(ray_t* l)    { return ray_list_append(l, RAY_NULL_OBJ); }
static ray_t* op_list_insert_at(ray_t* l) { return ray_list_insert_at(l, 1, RAY_NULL_OBJ); }

/* The nth allocation of `op` on a shared, full input fails, for every n it
 * reaches.  A failure answers an error and leaves the input with both its
 * references, its length and its cells; a success answers a new block.
 * Returns the count of failures after the copy was made, or -1. */
static int cow_sweep(ray_t* (*make)(void), cow_op_t op) {
    int after_copy = 0;
    for (int64_t n = 1; n <= 16; n++) {
        ray_t* v = make();
        if (!v) return -1;
        ray_retain(v);                         /* a second owner: shared */
        int64_t len0 = v->len;
        size_t bytes = (size_t)len0 * (v->type == RAY_LIST ? sizeof(ray_t*) : v->type == RAY_STR ? sizeof(ray_str_t) : 8);
        void* snap = malloc(bytes ? bytes : 1);
        memcpy(snap, ray_data(v), bytes);
        ray_alloc_fail_set_for_test(n, 1);
        ray_t* r = op(v);
        bool fired = ray_alloc_fail_armed_for_test() == 0;
        ray_alloc_fail_set_for_test(0, 0);
        if (r && RAY_IS_ERR(r)) {
            bool ok = v->rc == 2 && v->len == len0 && memcmp(snap, ray_data(v), bytes) == 0;
            ray_error_free(r);
            free(snap);
            if (!ok) { fprintf(stderr, "  allocation %lld fails: the input has rc %u, len %lld (was 2, %lld)\n", (long long)n, (unsigned)v->rc, (long long)v->len, (long long)len0); return -1; }
            if (n > 1) after_copy++;
            ray_release(v); ray_release(v);    /* both owners */
        } else {
            free(snap);
            if (!r || r == v || v->rc != 1) { fprintf(stderr, "  allocation %lld: a success left rc %u (same block %d, fired %d, len %lld cap %zu)\n", (long long)n, (unsigned)v->rc, r == v, fired, (long long)v->len, ray_block_data_bytes(v)); return -1; }
            ray_release(r); ray_release(v);    /* the answer, and the other owner */
        }
        if (!fired) break;
    }
    return after_copy;
}

static test_result_t test_cow_failed_write_keeps_input(void) {
    struct { const char* name; ray_t* (*make)(void); cow_op_t op; } cases[] = {
        { "ray_vec_append",      cow_full_i64,  op_vec_append },
        { "ray_vec_append_raw",  cow_full_i64,  op_vec_append_raw },
        { "ray_vec_insert_at",   cow_full_i64,  op_vec_insert_at },
        { "ray_str_vec_append",  cow_full_str,  op_str_append },
        { "ray_str_vec_set",     cow_full_str,  op_str_set },
        { "ray_list_append",     cow_full_list, op_list_append },
        { "ray_list_insert_at",  cow_full_list, op_list_insert_at },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        int after = cow_sweep(cases[i].make, cases[i].op);
        TEST_ASSERT_FMT(after >= 0, "%s: a failure did not leave the shared input as it was", cases[i].name);
        TEST_ASSERT_FMT(after > 0, "%s: no failure after the copy", cases[i].name);
    }
    PASS();
}

/* #1857's repro: a select whose `format` output is added to a copy of the
 * table under an allocation failure.  A failed append to the shared schema
 * dropped the source table's reference, and the next select read the freed
 * schema (a SIGSEGV under ASan).  Every run answers the answer or `oom`, and
 * the table stays readable. */
static void cow_rt_setup(void) { ray_runtime_create(0, NULL); }
static void cow_rt_teardown(void) { ray_runtime_destroy(__RUNTIME); }

static test_result_t test_cow_table_add_col_fail(void) {
    ray_t* t = ray_eval_str("(set TB57 (table [k s v f] (list (% (til 40) 4) (at ['a 'b 'c] (% (til 40) 3)) (til 40) (as 'F64 (% (til 40) 5)))))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "setup");
    ray_release(t);
    const char* q = "(select {r: (format \"% %\" s v) from: TB57})";
    int ooms = 0;
    for (int64_t n = 1; n <= 4000; n++) {
        ray_alloc_fail_set_for_test(n, 1);
        ray_t* r = ray_eval_str(q);
        bool fired = ray_alloc_fail_armed_for_test() == 0;
        ray_alloc_fail_set_for_test(0, 0);
        if (r && RAY_IS_ERR(r)) {
            TEST_ASSERT_FMT(strcmp(ray_err_code(r), "oom") == 0, "allocation %lld fails: %s, not oom", (long long)n, ray_err_code(r));
            ray_error_free(r);
            ooms++;
        } else if (r) ray_release(r);
        ray_heap_gc();   /* a block freed too early goes back: the read below faults */
        ray_t* c = ray_eval_str("(count (select {s: s from: TB57}))");   /* the table is still whole */
        TEST_ASSERT_FMT(c && !RAY_IS_ERR(c) && c->i64 == 40, "allocation %lld fails: the table is not whole after", (long long)n);
        ray_release(c);
        if (!fired) break;
    }
    TEST_ASSERT_TRUE(ooms > 0);
    ray_t* z = ray_eval_str("(set TB57 0)");
    if (z && !RAY_IS_ERR(z)) ray_release(z);
    PASS();
}

/* assert-fact retains each column of the facts table and appends to it (a
 * copy, as the table shares it).  A failed append leaves the column with
 * that retain (#1857), which assert-fact must drop: a reference kept would
 * keep the table's columns alive after the table goes. */
static test_result_t test_cow_assert_fact_fail(void) {
    /* symbols interned once, before the trace: they are kept for good */
    ray_t* w = ray_eval_str("(assert-fact (assert-fact (datoms) 9 'age 1) 8 'age 2)");
    if (w && !RAY_IS_ERR(w)) ray_release(w); else ray_error_free(w);
    w = ray_eval_str("(set Db9 0)");   /* bound before: rebinding it frees the atom it held */
    if (w && !RAY_IS_ERR(w)) ray_release(w);
    ray_clear_error_trace();
    ray_heap_gc();
    TEST_ASSERT_TRUE(ray_mem_trace_begin());
    ray_t* t = ray_eval_str("(set Db9 (assert-fact (assert-fact (datoms) 1 'age 30) 2 'age 40))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "setup");
    ray_release(t);
    int ooms = 0;
    for (int64_t n = 1; n <= 2000; n++) {
        ray_alloc_fail_set_for_test(n, 1);
        ray_t* r = ray_eval_str("(assert-fact Db9 3 'age 50)");
        bool fired = ray_alloc_fail_armed_for_test() == 0;
        ray_alloc_fail_set_for_test(0, 0);
        if (r && RAY_IS_ERR(r)) { ray_error_free(r); ooms++; }
        else if (r) ray_release(r);
        ray_clear_error_trace();
        if (!fired) break;
    }
    ray_t* z = ray_eval_str("(set Db9 0)");
    if (z && !RAY_IS_ERR(z)) ray_release(z);
    ray_clear_error_trace();
    ray_heap_gc();
    ray_mem_trace_t tr;
    ray_mem_trace_end(&tr);
    TEST_ASSERT_TRUE(ooms > 0);
    TEST_ASSERT_FMT(tr.net_bytes == 0, "the facts table's columns outlive it: %lld bytes kept", (long long)tr.net_bytes);
    PASS();
}
#endif

const test_entry_t cow_entries[] = {
    { "cow/retain_release", test_retain_release, cow_setup, cow_teardown },
    { "cow/cow_sole_owner", test_cow_sole_owner, cow_setup, cow_teardown },
    { "cow/cow_shared", test_cow_shared, cow_setup, cow_teardown },
    { "cow/null_error_safety", test_null_error_safety, cow_setup, cow_teardown },
    { "cow/cow_vector", test_cow_vector, cow_setup, cow_teardown },
    { "cow/block_copy_retains", test_block_copy_retains_children, cow_setup, cow_teardown },
#ifdef DEBUG
    { "cow/failed_write_keeps_input", test_cow_failed_write_keeps_input, cow_setup, cow_teardown },
    { "cow/table_add_col_fail", test_cow_table_add_col_fail, cow_rt_setup, cow_rt_teardown },
    { "cow/assert_fact_fail", test_cow_assert_fact_fail, cow_rt_setup, cow_rt_teardown },
#endif
    { NULL, NULL, NULL, NULL },
};


