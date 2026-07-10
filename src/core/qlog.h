/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   qlog — in-memory query-statistics ring.
 *
 *   Ambient, server-side per-query statistics: one summary row per completed
 *   query, held in a fixed-capacity ring and exposed as an ordinary table via
 *   `(.sys.querylog)`.  The model is a server-side query_log system table — the
 *   stats are read back with normal queries, no extra protocol — but in its
 *   simplest, durable-free form.  A later phase may flush the ring to a
 *   date-partitioned table; that is out of scope here.
 *
 *   Zero cost when disabled: the capture hook checks one relaxed flag and takes
 *   a predicted-not-taken branch.  When enabled, capture runs once per query at
 *   the single-threaded eval boundary — an O(1) struct write into the ring, no
 *   allocation, no per-row work.  The ring needs no lock because only the eval
 *   thread writes it.
 */

#ifndef RAY_QLOG_H
#define RAY_QLOG_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

#include <rayforce.h>
#include "core/qmeasure.h"

/* Ring capacity (power of two so the write index masks cleanly) and the inline
 * caps for the two variable-length fields — both bounded so the ring is a
 * fixed static with no per-row allocation. */
#define RAY_QLOG_CAP        4096u
#define RAY_QLOG_QUERY_MAX  256u   /* source text, longer is truncated       */
#define RAY_QLOG_STATUS_MAX 16u    /* "ok" or an error kind ("type", "limit")*/

/* One completed query's summary.  POD, fixed size — written by value into the
 * ring, so no ownership or lifetime concerns. */
typedef struct {
    int64_t  time_ns;       /* wall-clock finish time (RAY_TIMESTAMP units)  */
    double   duration_ms;   /* total wall time                               */
    int64_t  result_rows;   /* rows in the result (atom => 1)                */
    double   memory_kib;    /* net system-allocation delta over the query    */
    double   parallelism;   /* qstats busy_ns / wall                         */
    int32_t  workers;       /* qstats distinct workers                       */
    uint16_t query_len;
    uint8_t  status_len;
    char     query[RAY_QLOG_QUERY_MAX];
    char     status[RAY_QLOG_STATUS_MAX];
} ray_qlog_row_t;

typedef struct {
    _Atomic(uint8_t) enabled;   /* dedicated switch, default 0               */
    uint32_t         head;      /* next write index (mod CAP)                */
    uint32_t         count;     /* rows written, saturates at CAP            */
    ray_qlog_row_t   slot[RAY_QLOG_CAP];
} ray_qlog_t;

/* Single global instance (defined in qlog.c). */
extern ray_qlog_t g_qlog;

static inline bool ray_qlog_enabled(void) {
    return atomic_load_explicit(&g_qlog.enabled, memory_order_relaxed) != 0;
}

static inline void ray_qlog_set_enabled(bool on) {
    atomic_store_explicit(&g_qlog.enabled, on ? 1u : 0u, memory_order_relaxed);
}

/* Query logging wraps the same measurement scope used by `.mem.ts`. */
typedef struct { ray_query_measure_t measure; } ray_qlog_ctx_t;

static inline void ray_qlog_begin(ray_qlog_ctx_t* c) {
    c->measure = (ray_query_measure_t){0};
    if (ray_qlog_enabled()) ray_query_measure_begin(&c->measure);
}

/* Close a query and append its summary row.  No-op when logging was off at
 * begin.  `src` is the query source (truncated to RAY_QLOG_QUERY_MAX); `result`
 * is the final, materialized result (an error object sets a non-ok status). */
void ray_qlog_end(ray_qlog_ctx_t* c, const char* src, size_t src_len,
                  ray_t* result);

/* Materialize the ring into a fresh table (oldest row first).  Returns an
 * empty, well-formed table when the ring is empty. */
ray_t* ray_qlog_table(void);

#endif /* RAY_QLOG_H */
