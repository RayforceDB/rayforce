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
#include "table/sym.h"
#include "core/platform.h"
#include "core/runtime.h"
#include "store/enc_once.h"
#include <stdatomic.h>
#include <string.h>

static void once_setup(void)    { ray_heap_init(); (void)ray_sym_init(); }
static void once_teardown(void) { ray_sym_destroy(); ray_heap_destroy(); }

typedef struct {
    ray_enc_once_t once;
    ray_t*         slot;
    _Atomic(int)   calls;
    int            mode;         /* 0 succeed, 1 transient until waiters, 2 deterministic */
    uint32_t       want_waiters;
} once_ctx_t;

static ray_t* once_fn(void* raw, ray_enc_snap_t* fail) {
    once_ctx_t* c = raw;
    int n = atomic_fetch_add(&c->calls, 1) + 1;
    if (c->mode == 1 && n == 1) {
        /* Hold the attempt until every other thread is waiting on it. */
        for (int spin = 0; spin < 50000000 &&
             ray_enc_once_waiters(&c->once) < c->want_waiters; spin++)
            ray_enc_yield();
        ray_enc_snap_set(fail, "oom", "attempt %d failed", n);
        fail->is_transient = true;
        return NULL;
    }
    if (c->mode == 2) {
        ray_t* e = ray_error("corrupt", "col /x: bad descriptor (chunk 3)");
        ray_enc_snap_take(fail, e);
        return NULL;
    }
    int64_t v = 7;
    return ray_vec_from_raw(RAY_I64, &v, 1);
}

typedef struct { once_ctx_t* c; ray_t* got; char code[8]; char msg[256]; } once_thr_t;

static void once_thread(void* raw) {
    once_thr_t* t = raw;
    ray_t* r = ray_enc_once_get(&t->c->once, &t->c->slot, once_fn, t->c);
    t->got = r;
    if (r && RAY_IS_ERR(r)) {
        snprintf(t->code, sizeof t->code, "%s", ray_err_code(r));
        snprintf(t->msg, sizeof t->msg, "%s", ray_error_msg() ? ray_error_msg() : "");
        ray_error_free(r);
        t->got = NULL;
    }
    ray_heap_abandon();
}

static test_result_t test_once_success_shared(void) {
    once_ctx_t c; memset(&c, 0, sizeof c); ray_enc_once_init(&c.once);
    once_thr_t t[8]; ray_thread_t th[8];
    for (int i = 0; i < 8; i++) { memset(&t[i], 0, sizeof t[i]); t[i].c = &c;
        TEST_ASSERT_EQ_I(ray_thread_create(&th[i], once_thread, &t[i]), RAY_OK); }
    for (int i = 0; i < 8; i++) ray_thread_join(th[i]);
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 1);
    for (int i = 0; i < 8; i++) TEST_ASSERT_EQ_PTR(t[i].got, c.slot);
    ray_release(c.slot);
    PASS();
}

static test_result_t test_once_many_waiters_transient(void) {
    once_ctx_t c; memset(&c, 0, sizeof c); ray_enc_once_init(&c.once);
    c.mode = 1; c.want_waiters = 7;
    once_thr_t t[8]; ray_thread_t th[8];
    for (int i = 0; i < 8; i++) { memset(&t[i], 0, sizeof t[i]); t[i].c = &c;
        TEST_ASSERT_EQ_I(ray_thread_create(&th[i], once_thread, &t[i]), RAY_OK); }
    for (int i = 0; i < 8; i++) ray_thread_join(th[i]);
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 1);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_FMT(strcmp(t[i].code, "oom") == 0, "thread %d code %s", i, t[i].code);
        TEST_ASSERT_FMT(strcmp(t[i].msg, "attempt 1 failed") == 0, "thread %d msg %s", i, t[i].msg);
    }
    /* The state reset: the next caller retries and succeeds. */
    ray_t* r = ray_enc_once_get(&c.once, &c.slot, once_fn, &c);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 2);
    ray_release(c.slot);
    PASS();
}

static test_result_t test_once_deterministic_cached(void) {
    once_ctx_t c; memset(&c, 0, sizeof c); ray_enc_once_init(&c.once);
    c.mode = 2;
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    for (int i = 0; i < 5; i++) {
        ray_error_clear();
        ray_t* r = ray_enc_once_get(&c.once, &c.slot, once_fn, &c);
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_STR_EQ(ray_error_msg(), "col /x: bad descriptor (chunk 3)");
        ray_error_free(r);
    }
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 1);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);    /* no leaked error objects */
    PASS();
}

static test_result_t test_once_transient_codes(void) {
    TEST_ASSERT_TRUE(ray_enc_code_transient("oom"));
    TEST_ASSERT_TRUE(ray_enc_code_transient("cancel"));
    TEST_ASSERT_FALSE(ray_enc_code_transient("corrupt"));
    TEST_ASSERT_FALSE(ray_enc_code_transient("nyi"));
    ray_enc_snap_t s;
    ray_enc_snap_take(&s, RAY_OOM_OBJ);                 /* the static sentinel is safe */
    TEST_ASSERT_STR_EQ(s.code, "oom");
    TEST_ASSERT_TRUE(s.is_transient);
    PASS();
}

const test_entry_t enconce_entries[] = {
    { "enconce/success_shared",        test_once_success_shared,        once_setup, once_teardown },
    { "enconce/many_waiters_transient",test_once_many_waiters_transient,once_setup, once_teardown },
    { "enconce/deterministic_cached",  test_once_deterministic_cached,  once_setup, once_teardown },
    { "enconce/transient_codes",       test_once_transient_codes,       once_setup, once_teardown },
    { NULL, NULL, NULL, NULL },
};
