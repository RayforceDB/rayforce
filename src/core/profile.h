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

#ifndef RAY_PROFILE_H
#define RAY_PROFILE_H

#include <stdint.h>
#include <stdbool.h>

#if defined(RAY_OS_WINDOWS)
#include <windows.h>
#else
#include <time.h>
/* clock_gettime / CLOCK_MONOTONIC may be hidden under strict -std=c17
 * without _POSIX_C_SOURCE.  Provide fallback declarations. */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
int clock_gettime(int clk_id, struct timespec *tp);
#endif
#endif

/* ===== Span-based execution profiler =====
 *
 * Zero overhead when inactive — every call guards on g_ray_profile.active.
 * Activated by REPL :t command; lives entirely outside hot morsel loops.
 */

#define RAY_PROFILE_SPANS_MAX 2048

typedef enum {
    RAY_PROF_SPAN_START,
    RAY_PROF_SPAN_END,
    RAY_PROF_SPAN_TICK
} ray_prof_span_type_t;

typedef struct {
    ray_prof_span_type_t type;
    const char*          msg;
    int64_t              ts;   /* nanoseconds (monotonic) */

    /* Optional per-span payload, populated by the executor when profiling
     * is active (0 otherwise).  Snapshots are recorded at both the START
     * and END of a span; the consumer takes END-minus-START deltas.  These
     * fields carry no cost when inactive — the span is simply not created. */
    int64_t  sys_cur;     /* process bytes in use (ray_sys) at this instant */
    int64_t  rows_out;    /* result element/row count (END spans)            */
    uint64_t qs_rows;     /* cumulative qstats rows_done at this instant      */
    uint64_t qs_busy_ns;  /* cumulative qstats worker busy-ns at this instant */
    uint64_t qs_tasks;    /* cumulative qstats tasks_run at this instant      */
    uint32_t qs_workers;  /* qstats workers that had run a task (END spans)   */
} ray_prof_span_t;

typedef struct {
    bool              active;
    int32_t           n;
    ray_prof_span_t   spans[RAY_PROFILE_SPANS_MAX];
} ray_profile_t;

/* Single global instance (the in-flight query) plus a snapshot of the
 * most recently completed profiled query.  Because query execution is
 * lazy, `(.sys.prof)` typed after a query must read the snapshot, not the
 * live buffer (which the next eval resets): the snapshot is taken at the
 * start of each eval, preserving the query that just finished. */
extern ray_profile_t g_ray_profile;
extern ray_profile_t g_ray_profile_last;

/* Preserve the just-completed query into the snapshot.  Cheap no-op when
 * inactive.  Called at eval boundaries, before ray_profile_reset(). */
static inline void ray_profile_snapshot(void) {
    if (g_ray_profile.active && g_ray_profile.n > 0)
        g_ray_profile_last = g_ray_profile;
}

static inline int64_t ray_profile_now_ns(void) {
#if defined(RAY_OS_WINDOWS)
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (int64_t)((double)cnt.QuadPart / (double)freq.QuadPart * 1e9);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

static inline void ray_profile_reset(void) {
    g_ray_profile.n = 0;
}

/* Append a span and return it so the caller may fill the payload fields
 * (via core/qstats.h and mem/sys.h, which profile.h must not include —
 * qstats.h includes profile.h).  Returns NULL when inactive or the log is
 * full, so callers that only need timing can ignore the result. */
static inline ray_prof_span_t* ray_profile_span_start(const char* name) {
    if (!g_ray_profile.active) return NULL;
    if (g_ray_profile.n >= RAY_PROFILE_SPANS_MAX) return NULL;
    ray_prof_span_t* s = &g_ray_profile.spans[g_ray_profile.n++];
    *s = (ray_prof_span_t){ .type = RAY_PROF_SPAN_START, .msg = name,
                            .ts = ray_profile_now_ns() };
    return s;
}

static inline ray_prof_span_t* ray_profile_span_end(const char* name) {
    if (!g_ray_profile.active) return NULL;
    if (g_ray_profile.n >= RAY_PROFILE_SPANS_MAX) return NULL;
    ray_prof_span_t* s = &g_ray_profile.spans[g_ray_profile.n++];
    *s = (ray_prof_span_t){ .type = RAY_PROF_SPAN_END, .msg = name,
                            .ts = ray_profile_now_ns() };
    return s;
}

static inline void ray_profile_tick(const char* msg) {
    if (!g_ray_profile.active) return;
    if (g_ray_profile.n >= RAY_PROFILE_SPANS_MAX) return;
    ray_prof_span_t* s = &g_ray_profile.spans[g_ray_profile.n++];
    *s = (ray_prof_span_t){ .type = RAY_PROF_SPAN_TICK, .msg = msg,
                            .ts = ray_profile_now_ns() };
}

#endif /* RAY_PROFILE_H */
