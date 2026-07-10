/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 */

#include "core/qmeasure.h"

#include "core/profile.h"
#include "core/qstats.h"
#include "mem/sys.h"

void ray_query_workers_begin(bool capture, bool progress) {
    uint32_t mode = (capture ? RAY_QS_PROF : 0) |
                    (progress ? RAY_QS_PROGRESS : 0);
    ray_qstats_set_mode(mode);
    if (mode) ray_qstats_reset(0);
}

void ray_query_workers_snapshot(uint32_t* workers, uint64_t* busy_ns) {
    uint64_t max_busy = 0;
    ray_qstats_agg(workers, busy_ns, &max_busy);
}

void ray_query_measure_begin(ray_query_measure_t* scope) {
    if (!scope) return;
    scope->prior_qstats_mode = ray_qstats_mode();
    ray_qstats_set_mode(scope->prior_qstats_mode | RAY_QS_PROF);
    ray_qstats_reset(0);
    ray_sys_get_stat(&scope->memory0, NULL);
    scope->t0_ns = ray_profile_now_ns();
    scope->active = true;
}

void ray_query_measure_end(ray_query_measure_t* scope, ray_query_metrics_t* out) {
    if (!scope || !scope->active) {
        if (out) *out = (ray_query_metrics_t){0};
        return;
    }

    int64_t elapsed = ray_profile_now_ns() - scope->t0_ns;
    int64_t memory = 0;
    ray_sys_get_stat(&memory, NULL);
    uint32_t workers = 0;
    uint64_t busy_ns = 0;
    ray_query_workers_snapshot(&workers, &busy_ns);
    ray_qstats_set_mode(scope->prior_qstats_mode);
    scope->active = false;

    if (!out) return;
    if (elapsed < 0) elapsed = 0;
    *out = (ray_query_metrics_t){
        .time_ns = elapsed,
        .memory_bytes = memory - scope->memory0,
        .workers = workers,
        .worker_busy_ns = busy_ns,
        .parallelism = elapsed > 0 ? (double)busy_ns / (double)elapsed : 0.0,
    };
}
