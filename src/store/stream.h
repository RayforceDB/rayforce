/* Copyright (c) 2026 Anton Kundenko. MIT license; see LICENSE. */
#ifndef RAY_STORE_STREAM_H
#define RAY_STORE_STREAM_H
#include <rayforce.h>
#include <stdio.h>
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
     * first time it is met and looked up here after. */
    int64_t*  lut_id;    /* [COL_STREAM_LUT] runtime id per slot, -1 empty */
    uint32_t* lut_pos;   /* [COL_STREAM_LUT] position per slot */
} ray_col_stream_t;

ray_err_t ray_col_stream_open(ray_col_stream_t* w, const char* dir, int64_t name,
                              int8_t type, struct ray_sym_domain_s* domain);
ray_err_t ray_col_stream_append(ray_col_stream_t* w, ray_t* column);
ray_err_t ray_col_stream_close(ray_col_stream_t* w, bool durable);
void ray_col_stream_abort(ray_col_stream_t* w);
#endif
