/* Copyright (c) 2026 Anton Kundenko. MIT license; see LICENSE. */
#ifndef RAY_STORE_STREAM_H
#define RAY_STORE_STREAM_H
#include <rayforce.h>
#include <stdio.h>
#include "ops/idxop.h"
/* One column, appended in bounded batches. SYM target domain is borrowed.
 * Close commits the column; caller flushes its symbol domain first and writes
 * the table schema last. STR pools spool to disk, with native 32-bit offsets. */
typedef struct {
    FILE* fp;
    FILE* pool_fp;
    uint64_t pool_bytes;
    char pool_path[1024];
    char path[1024];
    char tmp_path[1024];
    int8_t type;
    uint8_t attrs;
    int64_t rows;
    bool had_nulls;
    /* SYM columns: the target symfile's domain (shared, borrowed from
     * the caller).  Cells are encoded as positions in it — the freshly
     * parsed chunk vecs are runtime-domain, so each cell's string is
     * find-or-appended into the domain (the flip, Task 7b).  Width is
     * fixed at W32: a streaming writer can't know the final vocabulary
     * before the last chunk, and W32 covers any STRL count. */
    struct ray_sym_domain_s* dom;
    /* runtime id -> domain position, direct-mapped: the chunk vecs are
     * runtime-domain and a column's values repeat across rows and chunks,
     * so a value is interned into the symfile's domain (a locked probe) the
     * first time it is met and looked up here after.  Made by the first
     * runtime-domain chunk: NULL while every chunk is already encoded over
     * `dom` (a converter's own symbol decode) and needs no translation. */
    int64_t*  lut_id;    /* [COL_STREAM_LUT] runtime id per slot, -1 empty */
    uint32_t* lut_pos;   /* [COL_STREAM_LUT] position per slot */
    ray_zone_acc_t* zone;   /* numeric / temporal: the running chunk zone */
    ray_dict_acc_t* dict;   /* STR: the running dictionary */
    bool    want_dict;      /* generation 2, STR: index_begin asked for the DICT (built at finish) */
    bool    finished;       /* finish done: the tmp file is complete, not yet renamed */
    uint8_t gen;            /* output generation: 0, or 2 (transcoded at finish) */
    uint64_t dec_pool;      /* generation 2: decoded STR pool bytes so far */
} ray_col_stream_t;

/* gen 0 writes today's format; gen 2 stages the same raw payload and
 * transcodes it chunk by chunk at finish (any other gen: RAY_ERR_VERSION). */
ray_err_t ray_col_stream_open(ray_col_stream_t* w, const char* dir, int64_t name,
                              int8_t type, struct ray_sym_domain_s* domain, uint8_t gen);
/* Start building the column's index from global row start_row. Call after
 * open and before the first append; no-op for types without an index. */
ray_err_t ray_col_stream_index_begin(ray_col_stream_t* w, int64_t start_row);
/* Fold src's zone into dst's (parallel Parquet tasks). Frees src's zone. */
ray_err_t ray_col_stream_index_merge(ray_col_stream_t* dst, ray_col_stream_t* src);
/* On error only ray_col_stream_abort is valid (payload bytes were written
 * before the index feed). */
ray_err_t ray_col_stream_append(ray_col_stream_t* w, ray_t* column);
ray_err_t ray_col_stream_close(ray_col_stream_t* w, bool durable);
/* close in two halves, so a converter can finish all its columns as pool
 * tasks (pool merge, index region, header, fclose, fsync) and publish them
 * serially.  finish touches only the writer's own files and never dispatches,
 * so it is safe inside a pool task; publish is the rename (+ dir sync).  On a
 * finish error the tmp file is already removed; the caller still calls abort
 * on every writer. */
ray_err_t ray_col_stream_finish(ray_col_stream_t* w, bool durable);
/* Refuses (RAY_ERR_DOMAIN) a writer whose finish never ran; a no-op on one
 * already published or aborted. */
ray_err_t ray_col_stream_publish(ray_col_stream_t* w, bool durable);
/* finish every writer as one pool task per column (serially without a
 * pool), then publish them in order.  The first finish error is returned;
 * a task the pool skipped (cancellation) is RAY_ERR_CANCEL; on any error
 * every writer is aborted and nothing is published.
 * `col_ns` (optional, n entries) receives each column's finish time. */
ray_err_t ray_col_stream_close_all(ray_col_stream_t* w, int64_t n, bool durable,
                                   int64_t* col_ns);
void ray_col_stream_abort(ray_col_stream_t* w);
#endif
