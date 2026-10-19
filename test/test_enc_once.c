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
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "core/platform.h"
#include "core/runtime.h"
#include "store/enc_once.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>

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
        return NULL;
    }
    if (c->mode == 6) {                 /* hold, then fail deterministically */
        for (int spin = 0; spin < 50000000 &&
             ray_enc_once_waiters(&c->once) < c->want_waiters; spin++)
            ray_enc_yield();
        ray_enc_snap_set(fail, "corrupt", "held attempt %d", n);
        return NULL;
    }
    if (c->mode == 9 && n == 1) return ray_error("fpenv", NULL);
    if (c->mode == 3) return ray_error("corrupt", "returned error object");
    if (c->mode == 4) return NULL;      /* NULL with an empty fail */
    if (c->mode == 8) { ray_enc_snap_set(fail, "cancel", "cancelled %d", n); return NULL; }
    if (c->mode == 2) {
        ray_t* e = ray_error("corrupt", "column /x: bad descriptor (chunk 3)");
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
        TEST_ASSERT_STR_EQ(ray_error_msg(), "column /x: bad descriptor (chunk 3)");
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
    TEST_ASSERT_TRUE(ray_enc_code_transient("fpenv"));
    TEST_ASSERT_FALSE(ray_enc_code_transient("corrupt"));
    TEST_ASSERT_FALSE(ray_enc_code_transient("nyi"));
    ray_enc_snap_t s;
    ray_enc_snap_take(&s, RAY_OOM_OBJ);                 /* the static sentinel is safe */
    TEST_ASSERT_STR_EQ(s.code, "oom");
    TEST_ASSERT_TRUE(s.is_transient);
    PASS();
}

static test_result_t test_once_draining_blocks_new_attempt(void) {
    once_ctx_t c; memset(&c, 0, sizeof c); ray_enc_once_init(&c.once);
    ray_enc_snap_t x; ray_enc_snap_set(&x, "oom", "X-msg");
    ray_enc_once_test_drain(&c.once, &x, 1);
    once_thr_t t; memset(&t, 0, sizeof t); t.c = &c;
    ray_thread_t th;
    TEST_ASSERT_EQ_I(ray_thread_create(&th, once_thread, &t), RAY_OK);
    /* Bounded observation: while a waiter is outstanding the caller must not
     * run fn nor overwrite the snapshot. */
    struct timespec ts = { 0, 1000000 };
    for (int i = 0; i < 200; i++) {
        TEST_ASSERT_EQ_I(atomic_load(&c.calls), 0);
        TEST_ASSERT_EQ_U(ray_enc_once_waiters(&c.once), 1);
        nanosleep(&ts, NULL);
    }
    TEST_ASSERT_STR_EQ(c.once.snap.code, "oom");
    TEST_ASSERT_STR_EQ(c.once.snap.msg, "X-msg");
    ray_enc_snap_t got;
    ray_enc_once_test_waiter_done(&c.once, &got);
    ray_thread_join(th);
    TEST_ASSERT_STR_EQ(got.code, "oom");
    TEST_ASSERT_STR_EQ(got.msg, "X-msg");
    TEST_ASSERT_TRUE(got.is_transient);
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 1);
    TEST_ASSERT_TRUE(t.got != NULL && t.got == c.slot);
    ray_release(c.slot);
    PASS();
}

static test_result_t test_once_waiters_get_deterministic(void) {
    once_ctx_t c; memset(&c, 0, sizeof c); ray_enc_once_init(&c.once);
    c.mode = 6; c.want_waiters = 7;
    once_thr_t t[8]; ray_thread_t th[8];
    for (int i = 0; i < 8; i++) { memset(&t[i], 0, sizeof t[i]); t[i].c = &c;
        TEST_ASSERT_EQ_I(ray_thread_create(&th[i], once_thread, &t[i]), RAY_OK); }
    for (int i = 0; i < 8; i++) ray_thread_join(th[i]);
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 1);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_STR_EQ(t[i].code, "corrupt");
        TEST_ASSERT_STR_EQ(t[i].msg, "held attempt 1");
    }
    ray_t* r = ray_enc_once_get(&c.once, &c.slot, once_fn, &c);   /* still cached */
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 1);
    ray_error_free(r);
    PASS();
}

static test_result_t test_once_error_object_returned(void) {
    once_ctx_t c; memset(&c, 0, sizeof c); ray_enc_once_init(&c.once);
    c.mode = 3;
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    for (int i = 0; i < 3; i++) {
        ray_error_clear();
        ray_t* r = ray_enc_once_get(&c.once, &c.slot, once_fn, &c);
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_STR_EQ(ray_error_msg(), "returned error object");
        ray_error_free(r);
    }
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 1);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);
    PASS();
}

static test_result_t test_once_null_without_snapshot(void) {
    once_ctx_t c; memset(&c, 0, sizeof c); ray_enc_once_init(&c.once);
    c.mode = 4;
    for (int i = 0; i < 3; i++) {
        ray_t* r = ray_enc_once_get(&c.once, &c.slot, once_fn, &c);
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "error");
        ray_error_free(r);
    }
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 1);      /* cached as deterministic */
    PASS();
}

static test_result_t test_once_transient_no_waiters_retries(void) {
    once_ctx_t c; memset(&c, 0, sizeof c); ray_enc_once_init(&c.once);
    c.mode = 1; c.want_waiters = 0;
    ray_t* r = ray_enc_once_get(&c.once, &c.slot, once_fn, &c);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "oom");
    ray_error_free(r);
    TEST_ASSERT_EQ_U(atomic_load(&c.once.state), RAY_ENC_ONCE_IDLE);
    r = ray_enc_once_get(&c.once, &c.slot, once_fn, &c);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 2);
    ray_release(c.slot);
    PASS();
}

static test_result_t test_once_transient_no_leak(void) {
    once_ctx_t c; memset(&c, 0, sizeof c); ray_enc_once_init(&c.once);
    c.mode = 8;                                       /* always cancel */
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    for (int i = 0; i < 20; i++) {
        ray_error_clear();
        ray_t* r = ray_enc_once_get(&c.once, &c.slot, once_fn, &c);
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "cancel");
        ray_error_free(r);
    }
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 20);
    c.mode = 0;
    ray_t* r = ray_enc_once_get(&c.once, &c.slot, once_fn, &c);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(c.slot);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);
    PASS();
}

/* Exercise the actual error-object -> snapshot -> error-object path, as
 * used when a decoding worker rejects its own FP controls. No FP state is
 * changed here: this tests classification/retry, not platform detection. */
static test_result_t test_once_fpenv_retries(void) {
    once_ctx_t c; memset(&c, 0, sizeof c); ray_enc_once_init(&c.once);
    c.mode = 9;
    ray_t* r = ray_enc_once_get(&c.once, &c.slot, once_fn, &c);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "fpenv");
    TEST_ASSERT_EQ_I(ray_err_from_obj(r), RAY_ERR_FPENV);
    TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), "FE_TONEAREST"));
    ray_error_free(r);
    TEST_ASSERT_EQ_U(atomic_load(&c.once.state), RAY_ENC_ONCE_IDLE);
    TEST_ASSERT_TRUE(c.slot == NULL);
    r = ray_enc_once_get(&c.once, &c.slot, once_fn, &c);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_EQ_PTR(r, c.slot);
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 2);
    TEST_ASSERT_EQ_U(atomic_load(&c.once.state), RAY_ENC_ONCE_DONE);
    TEST_ASSERT_EQ_PTR(ray_enc_once_get(&c.once, &c.slot, once_fn, &c), r);
    TEST_ASSERT_EQ_I(atomic_load(&c.calls), 2);
    ray_release(c.slot);
    PASS();
}

static test_result_t test_once_fpenv_snapshot(void) {
    TEST_ASSERT_STR_EQ(ray_err_code_str(RAY_ERR_FPENV), "fpenv");
    ray_enc_snap_t s;
    ray_enc_snap_set(&s, "fpenv", "worker %d controls unsupported", 3);
    TEST_ASSERT_TRUE(s.is_transient);
    ray_t* e = ray_enc_snap_error(&s);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_EQ_I(ray_err_from_obj(e), RAY_ERR_FPENV);
    TEST_ASSERT_STR_EQ(ray_error_msg(), "worker 3 controls unsupported");
    ray_enc_snap_take(&s, e);
    TEST_ASSERT_TRUE(s.is_transient);
    TEST_ASSERT_STR_EQ(s.code, "fpenv");
    TEST_ASSERT_STR_EQ(s.msg, "worker 3 controls unsupported");

    /* Enum-only save conversions must not lose the explanation. */
    e = ray_error(ray_err_code_str(RAY_ERR_FPENV), NULL);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), "FE_TONEAREST"));
    TEST_ASSERT_TRUE(strstr(ray_error_msg(), "masked exceptions") != NULL);
    TEST_ASSERT_TRUE(strstr(ray_error_msg(), "no FTZ/DAZ") != NULL);
    ray_enc_snap_take(&s, e);
    TEST_ASSERT_TRUE(s.is_transient);
    TEST_ASSERT_STR_EQ(s.code, "fpenv");
    TEST_ASSERT_TRUE(strstr(s.msg, "FE_TONEAREST") != NULL);
    e = ray_enc_snap_error(&s);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_EQ_I(ray_err_from_obj(e), RAY_ERR_FPENV);
    TEST_ASSERT_STR_EQ(ray_error_msg(), s.msg);
    ray_error_free(e);

    /* Other constructors retain their NULL-format clearing behavior. */
    e = ray_error("domain", NULL);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_TRUE(ray_error_msg() == NULL);
    ray_error_free(e);
    PASS();
}

const test_entry_t enconce_entries[] = {
    { "enconce/success_shared",        test_once_success_shared,        once_setup, once_teardown },
    { "enconce/many_waiters_transient",test_once_many_waiters_transient,once_setup, once_teardown },
    { "enconce/deterministic_cached",  test_once_deterministic_cached,  once_setup, once_teardown },
    { "enconce/transient_codes",       test_once_transient_codes,       once_setup, once_teardown },
    { "enconce/draining_blocks_new_attempt", test_once_draining_blocks_new_attempt, once_setup, once_teardown },
    { "enconce/waiters_get_deterministic", test_once_waiters_get_deterministic, once_setup, once_teardown },
    { "enconce/error_object_returned", test_once_error_object_returned, once_setup, once_teardown },
    { "enconce/null_without_snapshot", test_once_null_without_snapshot, once_setup, once_teardown },
    { "enconce/transient_no_waiters_retries", test_once_transient_no_waiters_retries, once_setup, once_teardown },
    { "enconce/transient_no_leak", test_once_transient_no_leak, once_setup, once_teardown },
    { "enconce/fpenv_retries", test_once_fpenv_retries, once_setup, once_teardown },
    { "enconce/fpenv_snapshot", test_once_fpenv_snapshot, once_setup, once_teardown },
    { NULL, NULL, NULL, NULL },
};
