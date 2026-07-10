/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   qlog — in-memory query-statistics ring (backing store + materializer).
 *   See core/qlog.h for the model and zero-cost-off argument.
 */

#include <string.h>

#include <rayforce.h>
#include "core/qlog.h"

/* Single global instance.  Zero-initialised: disabled, empty ring. */
ray_qlog_t g_qlog;

void ray_qlog_end(ray_qlog_ctx_t* c, const char* src, size_t src_len,
                  ray_t* result) {
    if (!c->measure.active) return; /* logging was off when query began */
    ray_query_metrics_t metrics;
    ray_query_measure_end(&c->measure, &metrics);

    ray_qlog_row_t* row = &g_qlog.slot[g_qlog.head & (RAY_QLOG_CAP - 1)];
    row->time_ns     = ray_timestamp_now_ns();
    row->duration_ms = (double)metrics.time_ns / 1e6;
    row->memory_kib  = (double)metrics.memory_bytes / 1024.0;

    /* Result summary + status.  ray_len on a scalar atom returns the value,
     * not a count (the len field aliases i64), so guard atoms as one row. */
    const char* status = "ok";
    if (RAY_IS_ERR(result)) {
        const char* code = ray_err_code(result);
        status = code ? code : "error";
        row->result_rows = 0;
    } else if (!result || RAY_IS_NULL(result)) {
        row->result_rows = 0;
    } else {
        row->result_rows = (result->type == RAY_TABLE) ? ray_table_nrows(result)
                         : ray_is_atom(result)         ? 1
                         :                                (int64_t)ray_len(result);
    }

    row->workers     = (int32_t)metrics.workers;
    row->parallelism = metrics.parallelism;

    /* Bounded copies — no allocation, ring memory stays fixed. */
    size_t qn = src ? src_len : 0;
    if (qn > RAY_QLOG_QUERY_MAX) qn = RAY_QLOG_QUERY_MAX;
    if (qn) memcpy(row->query, src, qn);
    row->query_len = (uint16_t)qn;

    size_t sn = strlen(status);
    if (sn > RAY_QLOG_STATUS_MAX) sn = RAY_QLOG_STATUS_MAX;
    memcpy(row->status, status, sn);
    row->status_len = (uint8_t)sn;

    g_qlog.head++;
    if (g_qlog.count < RAY_QLOG_CAP) g_qlog.count++;
}

/* Materialize the ring into a fresh table, oldest row first.  Follows the
 * ray_prof_fn pattern in system.c: ray_vec_new allocates capacity but leaves
 * len at 0, so publish len on every column after filling. */
ray_t* ray_qlog_table(void) {
    int64_t n = (int64_t)g_qlog.count;

    ray_t* c_time = ray_vec_new(RAY_TIMESTAMP, n);
    ray_t* c_dur  = ray_vec_new(RAY_F64, n);
    ray_t* c_rows = ray_vec_new(RAY_I64, n);
    ray_t* c_mem  = ray_vec_new(RAY_F64, n);
    ray_t* c_wrk  = ray_vec_new(RAY_I64, n);
    ray_t* c_par  = ray_vec_new(RAY_F64, n);
    ray_t* c_stat = ray_vec_new(RAY_SYM, n);
    ray_t* c_qry  = ray_vec_new(RAY_STR, n);
    ray_t* fixed[] = { c_time, c_dur, c_rows, c_mem, c_wrk, c_par, c_stat };
    ray_t* all[]   = { c_time, c_dur, c_rows, c_mem, c_wrk, c_par, c_stat, c_qry };
    for (size_t i = 0; i < sizeof(all)/sizeof(all[0]); i++)
        if (!all[i] || RAY_IS_ERR(all[i])) {
            for (size_t j = 0; j < sizeof(all)/sizeof(all[0]); j++)
                if (all[j] && !RAY_IS_ERR(all[j])) ray_release(all[j]);
            return ray_error("oom", "sys.querylog: column alloc");
        }

    int64_t* times = (int64_t*)ray_data(c_time);
    double*  durs  = (double*)ray_data(c_dur);
    int64_t* rows  = (int64_t*)ray_data(c_rows);
    double*  mems  = (double*)ray_data(c_mem);
    int64_t* wrks  = (int64_t*)ray_data(c_wrk);
    double*  pars  = (double*)ray_data(c_par);
    int64_t* stats = (int64_t*)ray_data(c_stat);

    uint32_t start = (g_qlog.head - g_qlog.count) & (RAY_QLOG_CAP - 1);
    for (int64_t i = 0; i < n; i++) {
        const ray_qlog_row_t* r = &g_qlog.slot[(start + (uint32_t)i) & (RAY_QLOG_CAP - 1)];
        times[i] = r->time_ns;
        durs[i]  = r->duration_ms;
        rows[i]  = r->result_rows;
        mems[i]  = r->memory_kib;
        wrks[i]  = r->workers;
        pars[i]  = r->parallelism;
        stats[i] = ray_sym_intern(r->status, r->status_len);
        c_qry    = ray_str_vec_append(c_qry, r->query, r->query_len);
        if (!c_qry || RAY_IS_ERR(c_qry)) {
            for (size_t j = 0; j < sizeof(fixed)/sizeof(fixed[0]); j++) ray_release(fixed[j]);
            return c_qry ? c_qry : ray_error("oom", "sys.querylog: query column");
        }
    }
    for (size_t i = 0; i < sizeof(fixed)/sizeof(fixed[0]); i++)
        fixed[i]->len = n;

    static const char* names[] = { "time", "duration-ms", "result-rows",
                                   "memory-kib", "workers",
                                   "parallelism", "status", "query" };
    ray_t* cols[] = { c_time, c_dur, c_rows, c_mem, c_wrk, c_par, c_stat, c_qry };
    ray_t* tbl = ray_table_new(8);
    if (!tbl || RAY_IS_ERR(tbl)) {
        for (size_t i = 0; i < sizeof(cols)/sizeof(cols[0]); i++) ray_release(cols[i]);
        return tbl ? tbl : ray_error("oom", "sys.querylog: table");
    }
    for (size_t i = 0; i < sizeof(cols)/sizeof(cols[0]); i++) {
        int64_t nid = ray_sym_intern(names[i], strlen(names[i]));
        tbl = ray_table_add_col(tbl, nid, cols[i]);
        ray_release(cols[i]);
        if (RAY_IS_ERR(tbl)) return tbl;
    }
    return tbl;
}
