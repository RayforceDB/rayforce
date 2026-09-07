/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/* clock_gettime, CLOCK_MONOTONIC, and nanosleep require POSIX.1-2001+
 * exposure on glibc.  macOS exposes them by default; Windows uses the
 * QueryPerformanceCounter/Sleep branch below and ignores this. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "core/timer.h"
#include "core/runtime.h"   /* ray_error_msg */
#include "lang/internal.h"
#include "mem/sys.h"
#include <stdio.h>
#include <string.h>
#if defined(RAY_OS_WINDOWS)
#include <windows.h>
#else
#include <time.h>
#endif

int64_t ray_time_now_ms(void) {
#if defined(RAY_OS_WINDOWS)
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    /* Split the multiply to avoid losing sub-second precision: integer
     * cnt/freq alone rounds to whole seconds before scaling. */
    int64_t sec = cnt.QuadPart / freq.QuadPart;
    int64_t rem = cnt.QuadPart % freq.QuadPart;
    return sec * 1000 + (rem * 1000) / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
#endif
}

ray_timers_t* ray_timers_create(int64_t initial_cap) {
    if (initial_cap < 1) initial_cap = 1;
    ray_timers_t* t = (ray_timers_t*)ray_sys_alloc(sizeof(ray_timers_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->heap = (ray_timer_t**)ray_sys_alloc((size_t)initial_cap * sizeof(ray_timer_t*));
    if (!t->heap) { ray_sys_free(t); return NULL; }
    t->cap = initial_cap;
    return t;
}

void ray_timers_destroy(ray_timers_t* t) {
    if (!t) return;
    for (int64_t i = 0; i < t->n; i++) {
        ray_release(t->heap[i]->fn);
        ray_sys_free(t->heap[i]);
    }
    ray_sys_free(t->heap);
    ray_sys_free(t);
}

/* Min-heap ordering: lower exp_ms wins; ties broken by lower id. */
static int heap_less(const ray_timer_t* a, const ray_timer_t* b) {
    if (a->exp_ms != b->exp_ms) return a->exp_ms < b->exp_ms;
    return a->id < b->id;
}

static void heap_swap(ray_timer_t** heap, int64_t i, int64_t j) {
    ray_timer_t* tmp = heap[i];
    heap[i] = heap[j];
    heap[j] = tmp;
}

static void heap_up(ray_timer_t** heap, int64_t i) {
    while (i > 0) {
        int64_t parent = (i - 1) / 2;
        if (heap_less(heap[i], heap[parent])) {
            heap_swap(heap, i, parent);
            i = parent;
        } else break;
    }
}

/* Used by the delete and fire-expired paths. */
static void heap_down(ray_timer_t** heap, int64_t n, int64_t i) {
    for (;;) {
        int64_t l = 2 * i + 1;
        int64_t r = 2 * i + 2;
        int64_t best = i;
        if (l < n && heap_less(heap[l], heap[best])) best = l;
        if (r < n && heap_less(heap[r], heap[best])) best = r;
        if (best == i) break;
        heap_swap(heap, i, best);
        i = best;
    }
}

static bool heap_grow(ray_timers_t* t) {
    int64_t new_cap = t->cap * 2;
    ray_timer_t** new_heap = (ray_timer_t**)ray_sys_alloc(
        (size_t)new_cap * sizeof(ray_timer_t*));
    if (!new_heap) return false;
    memcpy(new_heap, t->heap, (size_t)t->n * sizeof(ray_timer_t*));
    ray_sys_free(t->heap);
    t->heap = new_heap;
    t->cap = new_cap;
    return true;
}

int64_t ray_timers_add(ray_timers_t* t, int64_t tic_ms, int64_t num, ray_t* fn) {
    if (!t || !fn) return -1;
    if (t->n >= t->cap && !heap_grow(t)) return -1;

    ray_timer_t* timer = (ray_timer_t*)ray_sys_alloc(sizeof(ray_timer_t));
    if (!timer) return -1;
    timer->id     = t->next_id++;
    timer->tic_ms = tic_ms;
    timer->exp_ms = ray_time_now_ms() + tic_ms;
    timer->num    = num;
    timer->fn     = fn;
    ray_retain(fn);

    t->heap[t->n] = timer;
    heap_up(t->heap, t->n);
    t->n++;
    return timer->id;
}

bool ray_timers_del(ray_timers_t* t, int64_t id) {
    if (!t) return false;
    for (int64_t i = 0; i < t->n; i++) {
        if (t->heap[i]->id != id) continue;
        ray_release(t->heap[i]->fn);
        ray_sys_free(t->heap[i]);
        t->n--;
        if (i < t->n) {
            t->heap[i] = t->heap[t->n];
            /* Rebalance: may need to sift up or down. */
            heap_down(t->heap, t->n, i);
            heap_up(t->heap, i);
        }
        return true;
    }
    return false;
}

int64_t ray_timers_next_deadline_ms(ray_timers_t* t) {
    if (!t || t->n == 0) return INT64_MAX;
    return t->heap[0]->exp_ms;
}

uint32_t ray_timers_fire_expired(ray_timers_t* t) {
    if (!t || t->n == 0) return 0;

    int64_t now = ray_time_now_ms();
    uint32_t fired = 0;

    while (t->n > 0 && t->heap[0]->exp_ms <= now) {
        /* Pop the head: swap with tail, sift down. */
        ray_timer_t* timer = t->heap[0];
        fired++;
        t->n--;
        if (t->n > 0) {
            t->heap[0] = t->heap[t->n];
            heap_down(t->heap, t->n, 0);
        }

        /* Fire the callback.  call_fn1 borrows fn and arg; caller
         * releases both.  The result (if any) is a new ref the caller
         * must release. */
        ray_t* arg    = make_i64(now);
        ray_t* result = call_fn1(timer->fn, arg);
        ray_release(arg);
        if (result) {
            if (RAY_IS_ERR(result)) {
                /* An error formats as its code alone; the message lives
                 * in the runtime's last-error slot.  Print both, as the
                 * REPL does, so a failing poll names its peer and cause. */
                char code[8] = {0};
                memcpy(code, result->sdata, result->slen < 7 ? result->slen : 7);
                const char* detail = ray_error_msg();
                fprintf(stderr, "timer %lld: error: %s%s%s\n",
                        (long long)timer->id, code,
                        detail ? ": " : "", detail ? detail : "");
                ray_error_free(result);
            } else {
                ray_release(result);
            }
        }

        /* Re-schedule or free.  Re-arm from the schedule, so the cadence
         * stays exact while callbacks are cheap, but never into the past:
         * a callback that overran its period would otherwise leave the
         * timer T/tic deadlines behind, and this loop would fire it that
         * many times back-to-back before the poll loop served any I/O.
         * Missed intervals are skipped, not replayed. */
        if (timer->num == 0 || timer->num > 1) {
            if (timer->num > 1) timer->num--;
            now = ray_time_now_ms();
            timer->exp_ms += timer->tic_ms;
            if (timer->exp_ms <= now) timer->exp_ms = now + timer->tic_ms;
            t->heap[t->n] = timer;
            heap_up(t->heap, t->n);
            t->n++;
        } else {
            /* num == 1: last fire just happened. */
            ray_release(timer->fn);
            ray_sys_free(timer);
        }

        /* Refresh `now` in case callbacks took meaningful time. */
        now = ray_time_now_ms();
    }
    return fired;
}

void ray_timers_pump_for(ray_timers_t* t, int64_t budget_ms) {
    if (!t || budget_ms <= 0) return;
    int64_t end = ray_time_now_ms() + budget_ms;
    while (ray_time_now_ms() < end) {
        ray_timers_fire_expired(t);
#if defined(RAY_OS_WINDOWS)
        Sleep(1);
#else
        struct timespec slice = { 0, 1000000L };  /* 1 ms */
        nanosleep(&slice, NULL);
#endif
    }
    ray_timers_fire_expired(t);
}
