/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   qstats — per-worker query-statistics counter slab.
 *
 *   A race-free substrate for observing parallel execution: each worker
 *   writes only to its own cache-line-padded slot, and the main thread
 *   sums the slots after a dispatch barrier (or samples them on its wait
 *   path).  It is the shared foundation for two features:
 *     - per-query statistics (parallelism payloads on profile spans), and
 *     - long-query progress reporting.
 *
 *   Zero cost when disabled: a worker reads the global `mode` once per
 *   task (~8K elements, never per row) and takes a predicted-not-taken
 *   branch — the same shape the pool already pays for its `cancelled`
 *   check.  Clock reads happen only under the PROF bit.  No allocation,
 *   ever: the slot array is a fixed static.
 */

#ifndef RAY_QSTATS_H
#define RAY_QSTATS_H

#include <stdatomic.h>
#include <stdint.h>

#include "core/profile.h"    /* ray_profile_now_ns */
#include "core/platform.h"   /* RAY_ALIGN */

/* Fixed slot count (power of two so worker ids mask cleanly).  256 slots ×
 * 64 bytes = 16 KiB static — comfortably covers any real core count while
 * dodging any per-pool allocation/lifetime question. */
#define RAY_QS_MAX_WORKERS 256u

/* Mode bits (set on the main thread before a dispatch). */
#define RAY_QS_PROF     0x1u   /* capture busy-ns / task counts for stats */
#define RAY_QS_PROGRESS 0x2u   /* accumulate rows_done for progress pumping */

/* One worker's counters.  rows_done is the only field read cross-thread
 * (by the progress pump), so it is atomic; the rest are written and read
 * only by the owning worker / the main thread after a barrier. */
typedef struct RAY_ALIGN(64) {
    _Atomic(uint64_t) rows_done;   /* elements processed (relaxed adds) */
    uint64_t          tasks_run;   /* tasks executed by this worker */
    uint64_t          busy_ns;     /* time spent inside task fns */
    char              _pad[64 - 3 * 8];
} ray_wstat_t;

typedef struct {
    _Atomic(uint32_t) mode;              /* RAY_QS_* bits, 0 = disabled */
    uint64_t          phase_rows_total;  /* set by dispatch; main-thread only */
    ray_wstat_t       slot[RAY_QS_MAX_WORKERS];
} ray_qstats_t;

/* Single global instance (defined in pool.c). */
extern ray_qstats_t g_qstats;

/* ── main-thread control (call between dispatches, never in a hot loop) ── */

/* Enable/disable capture.  Passing 0 disables everything. */
static inline void ray_qstats_set_mode(uint32_t mode) {
    atomic_store_explicit(&g_qstats.mode, mode, memory_order_relaxed);
}

static inline uint32_t ray_qstats_mode(void) {
    return atomic_load_explicit(&g_qstats.mode, memory_order_relaxed);
}

/* Zero all slots and set the phase row total.  Cheap enough to call at the
 * start of every dispatch when capture is active. */
static inline void ray_qstats_reset(uint64_t phase_rows_total) {
    g_qstats.phase_rows_total = phase_rows_total;
    for (uint32_t i = 0; i < RAY_QS_MAX_WORKERS; i++) {
        atomic_store_explicit(&g_qstats.slot[i].rows_done, 0, memory_order_relaxed);
        g_qstats.slot[i].tasks_run = 0;
        g_qstats.slot[i].busy_ns   = 0;
    }
}

/* Aggregate reads — call after a dispatch barrier (no concurrent writers). */
static inline uint64_t ray_qstats_sum_rows(void) {
    uint64_t r = 0;
    for (uint32_t i = 0; i < RAY_QS_MAX_WORKERS; i++)
        r += atomic_load_explicit(&g_qstats.slot[i].rows_done, memory_order_relaxed);
    return r;
}

/* Number of workers that ran at least one task, plus the busy-ns sum and
 * max — enough to derive effective parallelism and load balance. */
static inline void ray_qstats_agg(uint32_t* workers_used,
                                  uint64_t* busy_ns_sum,
                                  uint64_t* busy_ns_max) {
    uint32_t used = 0; uint64_t sum = 0, mx = 0;
    for (uint32_t i = 0; i < RAY_QS_MAX_WORKERS; i++) {
        if (g_qstats.slot[i].tasks_run) {
            used++;
            sum += g_qstats.slot[i].busy_ns;
            if (g_qstats.slot[i].busy_ns > mx) mx = g_qstats.slot[i].busy_ns;
        }
    }
    if (workers_used) *workers_used = used;
    if (busy_ns_sum)  *busy_ns_sum  = sum;
    if (busy_ns_max)  *busy_ns_max  = mx;
}

/* ── per-task hooks (called by the pool around each t->fn) ────────────── */

/* Read the mode once per task.  Returns the mode; stashes a start
 * timestamp in *t0 iff PROF is active (0 otherwise). */
static inline uint32_t ray_qstats_task_begin(int64_t* t0) {
    uint32_t m = atomic_load_explicit(&g_qstats.mode, memory_order_relaxed);
    *t0 = (m & RAY_QS_PROF) ? ray_profile_now_ns() : 0;
    return m;
}

/* Record this task's contribution to its worker's slot. */
static inline void ray_qstats_task_end(uint32_t mode, uint32_t worker_id,
                                       int64_t n_rows, int64_t t0) {
    if (!mode) return;
    ray_wstat_t* s = &g_qstats.slot[worker_id & (RAY_QS_MAX_WORKERS - 1)];
    atomic_fetch_add_explicit(&s->rows_done, (uint64_t)n_rows, memory_order_relaxed);
    if (mode & RAY_QS_PROF) {
        s->tasks_run++;
        s->busy_ns += (uint64_t)(ray_profile_now_ns() - t0);
    }
}

#endif /* RAY_QSTATS_H */
