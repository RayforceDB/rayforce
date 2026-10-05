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
#include "store/enc_once.h"
#include "core/platform.h"
#include "core/runtime.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef RAY_OS_WINDOWS
#  include <windows.h>
#else
#  include <sched.h>
#endif

void ray_enc_yield(void) {
#if defined(RAY_OS_WINDOWS)
    SwitchToThread();
#elif defined(RAY_OS_WASM)
    /* single-threaded: nothing to wait for */
#else
    for (int i = 0; i < 64; i++) RAY_CPU_RELAX();
    sched_yield();
#endif
}

static void once_lock(ray_enc_once_t* o) {
    for (;;) {
        if (!atomic_load_explicit(&o->lock, memory_order_relaxed) &&
            !atomic_exchange_explicit(&o->lock, 1, memory_order_acquire)) return;
        RAY_CPU_RELAX();
    }
}
/* A DRAINING state returns to IDLE once its last waiter has copied the
 * snapshot (under the lock, after waiters was decremented). */
static void once_drain_reset(ray_enc_once_t* o) {
    if (o->waiters == 0 &&
        atomic_load_explicit(&o->state, memory_order_relaxed) == RAY_ENC_ONCE_DRAINING)
        atomic_store_explicit(&o->state, RAY_ENC_ONCE_IDLE, memory_order_release);
}
static void once_unlock(ray_enc_once_t* o) {
    atomic_store_explicit(&o->lock, 0, memory_order_release);
}

void ray_enc_once_init(ray_enc_once_t* o) {
    memset(o, 0, sizeof *o);
    atomic_store(&o->lock, 0);
    atomic_store(&o->state, RAY_ENC_ONCE_IDLE);
}

uint32_t ray_enc_once_waiters(ray_enc_once_t* o) {
    once_lock(o);
    uint32_t w = o->waiters;
    once_unlock(o);
    return w;
}

bool ray_enc_code_transient(const char* code) {
    return code && (strcmp(code, "oom") == 0 || strcmp(code, "cancel") == 0);
}

void ray_enc_snap_set(ray_enc_snap_t* s, const char* code, const char* fmt, ...) {
    memset(s, 0, sizeof *s);
    snprintf(s->code, sizeof s->code, "%s", code ? code : "error");
    if (fmt) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(s->msg, sizeof s->msg, fmt, ap);
        va_end(ap);
    }
    s->is_transient = ray_enc_code_transient(s->code);
}

/* Must run on the thread that created `err`: the message is that thread's
 * thread-local ray_last_err_msg (src/core/runtime.c). */
void ray_enc_snap_take(ray_enc_snap_t* s, ray_t* err) {
    memset(s, 0, sizeof *s);
    const char* code = (err && RAY_IS_ERR(err)) ? ray_err_code(err) : "error";
    snprintf(s->code, sizeof s->code, "%s", code ? code : "error");
    const char* m = ray_error_msg();
    if (m) snprintf(s->msg, sizeof s->msg, "%s", m);
    s->is_transient = ray_enc_code_transient(s->code);
    ray_error_free(err);    /* a no-op on the static OOM sentinel */
}

ray_t* ray_enc_snap_error(const ray_enc_snap_t* s) {
    return s->msg[0] ? ray_error(s->code, "%s", s->msg) : ray_error(s->code, NULL);
}

void ray_enc_once_test_drain(ray_enc_once_t* o, const ray_enc_snap_t* snap, uint32_t waiters) {
    once_lock(o);
    o->snap = *snap; o->waiters = waiters; o->attempt++;
    atomic_store_explicit(&o->state, RAY_ENC_ONCE_DRAINING, memory_order_release);
    once_unlock(o);
}

void ray_enc_once_test_waiter_done(ray_enc_once_t* o, ray_enc_snap_t* out) {
    once_lock(o);
    *out = o->snap;
    o->waiters--;
    once_drain_reset(o);
    once_unlock(o);
}

ray_t* ray_enc_once_get(ray_enc_once_t* o, ray_t** slot, ray_enc_once_fn fn, void* ctx) {
    once_lock(o);
    for (;;) {
        uint32_t st = atomic_load_explicit(&o->state, memory_order_relaxed);
        if (st == RAY_ENC_ONCE_DONE) { ray_t* v = *slot; once_unlock(o); return v; }
        if (st == RAY_ENC_ONCE_FAILED) {
            ray_enc_snap_t s = o->snap; once_unlock(o);
            return ray_enc_snap_error(&s);
        }
        if (st == RAY_ENC_ONCE_RUNNING) {
            uint64_t mine = o->attempt;
            o->waiters++;
            once_unlock(o);
            while (atomic_load_explicit(&o->state, memory_order_acquire) == RAY_ENC_ONCE_RUNNING)
                ray_enc_yield();
            once_lock(o);
            o->waiters--;
            st = atomic_load_explicit(&o->state, memory_order_relaxed);
            if (st == RAY_ENC_ONCE_DRAINING && o->attempt == mine) {
                ray_enc_snap_t s = o->snap;
                once_drain_reset(o);
                once_unlock(o);
                return ray_enc_snap_error(&s);
            }
            continue;                      /* DONE or FAILED: answered above */
        }
        if (st == RAY_ENC_ONCE_DRAINING) {
            /* A failed attempt still has waiters to read its snapshot: do not
             * start a new attempt that would overwrite it. */
            once_unlock(o);
            ray_enc_yield();
            once_lock(o);
            continue;
        }
        /* IDLE: this caller runs the attempt. */
        o->attempt++;
        atomic_store_explicit(&o->state, RAY_ENC_ONCE_RUNNING, memory_order_relaxed);
        once_unlock(o);
        break;
    }
    ray_enc_snap_t fail;
    memset(&fail, 0, sizeof fail);
    ray_t* v = fn(ctx, &fail);
    if (v && RAY_IS_ERR(v)) {              /* fn returned an error object */
        ray_enc_snap_take(&fail, v);       /* on this thread, before the lock */
        v = NULL;
    }
    if (!v && !fail.code[0]) ray_enc_snap_set(&fail, "error", NULL);
    if (!v) fail.is_transient = ray_enc_code_transient(fail.code);
    once_lock(o);
    if (v) {
        *slot = v;
        atomic_store_explicit(&o->state, RAY_ENC_ONCE_DONE, memory_order_release);
        once_unlock(o);
        return v;
    }
    o->snap = fail;
    uint32_t next = !fail.is_transient ? RAY_ENC_ONCE_FAILED
                  : (o->waiters ? RAY_ENC_ONCE_DRAINING : RAY_ENC_ONCE_IDLE);
    atomic_store_explicit(&o->state, next, memory_order_release);
    once_unlock(o);
    return ray_enc_snap_error(&fail);
}
