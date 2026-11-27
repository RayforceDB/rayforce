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
#include "table/sym.h"

/* Single global instance.  Zero-initialised: disabled, empty ring. */
ray_qlog_t g_qlog;

void ray_qlog_end(ray_qlog_ctx_t* c, const char* src, size_t src_len,
                  ray_t* result) {
    if (!c->measure.active) return; /* logging was off when query began */
    ray_query_metrics_t metrics;
    ray_query_measure_end(&c->measure, &metrics);

    /* the slot and the row's number in one step (#1159) */
    uint64_t seq = atomic_fetch_add_explicit(&g_qlog.logged, 1, memory_order_relaxed) + 1;
    ray_qlog_row_t* row = &g_qlog.slot[(seq - 1) & (RAY_QLOG_CAP - 1)];
    row->seq         = (int64_t)seq;
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
}

/* Materialize the ring into a fresh table, oldest row first.  Follows the
 * ray_prof_fn pattern in system.c: ray_vec_new allocates capacity but leaves
 * len at 0, so publish len on every column after filling. */
ray_t* ray_qlog_table(int64_t from) {
    uint64_t logged = atomic_load_explicit(&g_qlog.logged, memory_order_relaxed);
    uint64_t kept = logged < RAY_QLOG_CAP ? logged : RAY_QLOG_CAP;
    uint64_t first = logged - kept + 1;           /* the oldest kept row's seq */
    if (from > 0 && (uint64_t)from > first) first = (uint64_t)from;
    int64_t n = first <= logged ? (int64_t)(logged - first + 1) : 0;

    ray_t* c_seq  = ray_vec_new(RAY_I64, n);
    ray_t* c_time = ray_vec_new(RAY_TIMESTAMP, n);
    ray_t* c_dur  = ray_vec_new(RAY_F64, n);
    ray_t* c_rows = ray_vec_new(RAY_I64, n);
    ray_t* c_mem  = ray_vec_new(RAY_F64, n);
    ray_t* c_wrk  = ray_vec_new(RAY_I64, n);
    ray_t* c_par  = ray_vec_new(RAY_F64, n);
    ray_t* c_stat = ray_vec_new(RAY_SYM, n);
    ray_t* c_qry  = ray_vec_new(RAY_STR, n);
    ray_t* fixed[] = { c_seq, c_time, c_dur, c_rows, c_mem, c_wrk, c_par, c_stat };
    ray_t* all[]   = { c_seq, c_time, c_dur, c_rows, c_mem, c_wrk, c_par, c_stat, c_qry };
    for (size_t i = 0; i < sizeof(all)/sizeof(all[0]); i++)
        if (!all[i] || RAY_IS_ERR(all[i])) {
            for (size_t j = 0; j < sizeof(all)/sizeof(all[0]); j++)
                if (all[j] && !RAY_IS_ERR(all[j])) ray_release(all[j]);
            return ray_error("oom", "sys.querylog: column alloc");
        }

    int64_t* seqs  = (int64_t*)ray_data(c_seq);
    int64_t* times = (int64_t*)ray_data(c_time);
    double*  durs  = (double*)ray_data(c_dur);
    int64_t* rows  = (int64_t*)ray_data(c_rows);
    double*  mems  = (double*)ray_data(c_mem);
    int64_t* wrks  = (int64_t*)ray_data(c_wrk);
    double*  pars  = (double*)ray_data(c_par);
    int64_t* stats = (int64_t*)ray_data(c_stat);

    for (int64_t i = 0; i < n; i++) {
        const ray_qlog_row_t* r = &g_qlog.slot[(first - 1 + (uint64_t)i) & (RAY_QLOG_CAP - 1)];
        seqs[i]  = r->seq;
        times[i] = r->time_ns;
        durs[i]  = r->duration_ms;
        rows[i]  = r->result_rows;
        mems[i]  = r->memory_kib;
        wrks[i]  = r->workers;
        pars[i]  = r->parallelism;
        stats[i] = ray_sym_intern(r->status, r->status_len);
        c_qry    = ray_str_vec_append_take(c_qry, r->query, r->query_len);
        if (!c_qry || RAY_IS_ERR(c_qry)) {
            for (size_t j = 0; j < sizeof(fixed)/sizeof(fixed[0]); j++) ray_release(fixed[j]);
            return c_qry ? c_qry : ray_error("oom", "sys.querylog: query column");
        }
    }
    for (size_t i = 0; i < sizeof(fixed)/sizeof(fixed[0]); i++)
        fixed[i]->len = n;

    static const char* names[] = { "seq", "time", "duration-ms", "result-rows",
                                   "memory-kib", "workers",
                                   "parallelism", "status", "query" };
    ray_t* cols[] = { c_seq, c_time, c_dur, c_rows, c_mem, c_wrk, c_par, c_stat, c_qry };
    ray_t* tbl = ray_table_new(9);
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

/* {next dropped capacity}: the seq the next logged row gets, the rows
 * overwritten since start (logged beyond the capacity), and the capacity. */
ray_t* ray_qlog_stats(void) {
    uint64_t logged = atomic_load_explicit(&g_qlog.logged, memory_order_relaxed);
    static const char* const names[3] = { "next", "dropped", "capacity" };
    int64_t vals_i[3] = { (int64_t)logged + 1,
                          logged > RAY_QLOG_CAP ? (int64_t)(logged - RAY_QLOG_CAP) : 0,
                          (int64_t)RAY_QLOG_CAP };
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 3);
    ray_t* vals = ray_list_new(3);
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) {
        /* each is ours: a value released, an error freed (#1864) */
        if (keys && RAY_IS_ERR(keys)) ray_error_free(keys); else ray_release(keys);
        if (vals && RAY_IS_ERR(vals)) ray_error_free(vals); else ray_release(vals);
        return ray_error("oom", "sys.querylog.stats");
    }
    for (int i = 0; i < 3; i++) {
        int64_t k = ray_sym_intern(names[i], strlen(names[i]));
        keys = ray_vec_append_take(keys, &k);
        if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
        ray_t* v = ray_i64(vals_i[i]);
        if (!v || RAY_IS_ERR(v)) { ray_release(keys); ray_release(vals); return v ? v : ray_error("oom", NULL); }
        vals = ray_list_append_take(vals, v); ray_release(v);
        if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    }
    return ray_dict_new(keys, vals);
}
