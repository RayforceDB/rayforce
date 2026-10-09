/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_SYMIMP_H
#define RAY_SYMIMP_H

/* The import dictionary: a symbol file one converter builds alone.
 *
 * Interned strings are appended straight into the mapped file as its STRL
 * records ([u32 len | bytes], position i = i-th record), so the file is the
 * dictionary's storage: nothing is copied out at the end and its pages are
 * the page cache's to write back and evict.  In memory there are only the
 * record offsets and a hash index split into shards; a hit is found
 * without a lock, and new strings are added under one.  Position 0 is "".  POSIX only:
 * ray_symimp_create returns NULL elsewhere, and on any failure to set up,
 * and the caller keeps the ordinary domain. */

#include <rayforce.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ray_symimp_s ray_symimp_t;

/* Create (truncating) the symbol file at `path`.  `count` receives the
 * number of entries as they are added. */
ray_symimp_t* ray_symimp_create(const char* path, _Atomic(int64_t)* count);
/* Position of each string, adding the new ones; thread-safe.  hashes[i] is
 * ray_hash_bytes of strs[i], all 64 bits (shard, home slot and tag come
 * from separate bits).  False on allocation or file growth failure. */
bool ray_symimp_intern_batch(ray_symimp_t* m, int64_t n, const char* const* strs,
                             const size_t* lens, const uint64_t* hashes,
                             int64_t* out_pos);
/* Write the header count and start writing back what was added since the
 * last call; `durable` cuts the file to its records and syncs it. */
ray_err_t ray_symimp_sync(ray_symimp_t* m, bool durable);
void ray_symimp_free(ray_symimp_t* m);

/* What the interning did, for the converters' RAY_CSV_TRACE: the counts
 * since the last call (which resets them), then the sizes now.  Counted
 * per batch whether or not anyone reads them. */
typedef struct ray_symimp_stats_s {
    int64_t strings;     /* strings interned */
    int64_t dedup;       /* of them repeats inside their batch */
    int64_t probes;      /* lock-free lookups: each batch's distinct strings */
    int64_t hits;        /* of them found */
    int64_t added;       /* strings appended (the other misses were added
                          * by another batch in the meantime) */
    int64_t slots;       /* index slots read by every lookup */
    int64_t false_tags;  /* of them with the hash but another string */
    int64_t grows;       /* shard tables doubled */
    int64_t grow_bytes;  /* bytes of the tables rehashed into new ones */
    int64_t count;       /* entries */
    int64_t rec_bytes;   /* the file's records and header */
    int64_t tab_bytes;   /* the shard tables */
    int64_t off_bytes;   /* the record offsets */
} ray_symimp_stats_t;
void ray_symimp_stats(ray_symimp_t* m, ray_symimp_stats_t* out);

/* Records placed by the caller (table/symgrp.h, the grouped import), which
 * keeps its own index: positions [*pos0, *pos0 + n) and `bytes` bytes of
 * records from file offset *off0, the file grown (and its blocks allocated)
 * to hold them.  The records are then written with ray_symimp_put, from any
 * thread, each range by one writer.  From the first reserve on,
 * ray_symimp_intern_batch fails: its shard tables miss these records. */
bool ray_symimp_reserve(ray_symimp_t* m, int64_t n, int64_t bytes, int64_t* pos0, int64_t* off0);
void ray_symimp_put(ray_symimp_t* m, int64_t pos, int64_t off, const char* s, uint32_t len);
/* Record `pos` of the file, written: its bytes (in the mapping) and length,
 * and the offset of its length prefix. */
const char* ray_symimp_get(const ray_symimp_t* m, int64_t pos, uint32_t* len);
int64_t ray_symimp_offset(const ray_symimp_t* m, int64_t pos);
/* Ask for file bytes [off, off + len) to be read ahead (MADV_WILLNEED). */
void ray_symimp_willneed(const ray_symimp_t* m, int64_t off, int64_t len);

#endif
