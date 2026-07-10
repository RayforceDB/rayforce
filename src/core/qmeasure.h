/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Shared per-query measurement lifecycle. Query logging, profiler spans,
 *   and `.mem.ts` consume this layer so their wall time, process memory,
 *   worker count, busy time, and parallelism have one definition.
 */

#ifndef RAY_QMEASURE_H
#define RAY_QMEASURE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t  time_ns;
    int64_t  memory_bytes;
    uint32_t workers;
    uint64_t worker_busy_ns;
    double   parallelism;
} ray_query_metrics_t;

typedef struct {
    int64_t  t0_ns;
    int64_t  memory0;
    uint32_t prior_qstats_mode;
    bool     active;
} ray_query_measure_t;

void ray_query_workers_begin(bool capture, bool progress);
void ray_query_workers_snapshot(uint32_t* workers, uint64_t* busy_ns);
void ray_query_measure_begin(ray_query_measure_t* scope);
void ray_query_measure_end(ray_query_measure_t* scope, ray_query_metrics_t* out);

#endif /* RAY_QMEASURE_H */
