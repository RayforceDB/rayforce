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
    ray_t*  index;          /* after close: the zone kept for a hash candidate;
                             * the caller releases it AND sets it to NULL, or
                             * must not call abort afterwards */
    bool    wants_hash;     /* after close: build a hash by re-reading the file */
    bool    finished;       /* finish done: the tmp file is complete, not yet renamed */
} ray_col_stream_t;

ray_err_t ray_col_stream_open(ray_col_stream_t* w, const char* dir, int64_t name,
                              int8_t type, struct ray_sym_domain_s* domain);
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
ray_err_t ray_col_stream_publish(ray_col_stream_t* w, bool durable);
/* finish every writer as one pool task per column (serially without a
 * pool), then publish them in order.  The first finish error is returned;
 * on any error every writer is aborted and nothing is published.
 * `col_ns` (optional, n entries) receives each column's finish time. */
ray_err_t ray_col_stream_close_all(ray_col_stream_t* w, int64_t n, bool durable,
                                   int64_t* col_ns);
/* Build the hash index of every closed writer whose zone asked for one
 * (wants_hash) by re-reading its published file, and release the zones.
 * Several candidates build as pool tasks (one column per task, in waves
 * sized from the RAM budget); a single one builds with the parallel
 * builder.  Best effort: a failed build leaves the column with its zone. */
void ray_col_stream_hash_all(ray_col_stream_t* w, int64_t n, int64_t* col_ns);
/* Build the hash index of one closed writer whose zone asked for one, right
 * away and with the whole pool, while its file is still in the page cache;
 * the zone is released and a later ray_col_stream_hash_all skips the
 * writer.  Best effort, as there.  Call outside any pool dispatch. */
void ray_col_stream_hash_one(ray_col_stream_t* w);
/* Hash builds in flight at once: clamp(ram_limit / 4 / (58 B * max(rows, 1)),
 * 1, candidates).  Pure; `candidates` >= 1 expected. */
int64_t ray_col_stream_hash_wave(int64_t ram_limit, int64_t rows, int64_t candidates);
void ray_col_stream_abort(ray_col_stream_t* w);
#endif
