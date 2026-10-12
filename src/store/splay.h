/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_SPLAY_H
#define RAY_SPLAY_H

#include <rayforce.h>
#include "store/fileio.h"

struct ray_sym_domain_s;

/* Internal publication protocol shared by the table and streaming CSV writers.
 * begin serializes writers; finish publishes only on success and always unlocks.
 * Publication keeps only the current generation; the others, and the first
 * write's root columns, are removed best-effort after each successful publish. */
typedef struct {
    ray_fd_t lock;
    bool locked;
    bool unlink_lock;       /* lock failed with nobody holding it: remove the file */
    bool staged;
    uint64_t* before;       /* in-place: inodes in root when the lock was taken */
    size_t nbefore;
    bool before_known;
    char root[1024];
    char created[1024];     /* shallowest directory of root begin created */
    char dir[1024];
    char generation[256];
} ray_splay_write_t;

ray_err_t ray_splay_write_begin(const char* dir, ray_splay_write_t* write);
ray_err_t ray_splay_write_finish(ray_splay_write_t* write, ray_err_t result,
                                  bool durable);
/* Write only to a fresh/unpublished directory owned by the caller. */
ray_err_t ray_splay_write_table(ray_t* tbl, const char* dir,
                                 const char* sym_path, bool durable);
/* The returned generation may be removed by the next publish: open every file
 * of a read right after resolving, and resolve again if one is missing (the
 * splayed loaders and ray_splay_has_schema do). */
ray_err_t ray_splay_resolve_dir(const char* dir, char* out, size_t out_sz);
/* Does dir hold a published table (a schema in its selected generation)? */
bool ray_splay_has_schema(const char* dir);

/* Splayed table I/O.
 *
 * sym_path names the table's symfile (domain): save distinct-merges the
 * table's SYM vocabulary into it (append-only) and encodes SYM columns
 * as positions; load attaches its FILE domain to every SYM column.
 * Symbol-free tables neither write nor require a symfile; a SYM column
 * with no resolvable symfile is a loud "sym" error. */
ray_err_t ray_splay_save(ray_t* tbl, const char* dir, const char* sym_path);
ray_err_t ray_splay_save_bulk(ray_t* tbl, const char* dir, const char* sym_path);
/* Private import directory only: caller holds the shared symbol domain alive
 * and flushes it before publishing the root. Bulk writes are not durable. */
ray_err_t ray_splay_save_staged_bulk(ray_t* tbl, const char* dir, const char* sym_path);
ray_t*    ray_splay_load(const char* dir, const char* sym_path);
ray_t*    ray_read_splayed(const char* dir, const char* sym_path);

/* Append chunk-zone index regions to the column files of a splayed store
 * written from a resident table, so later mmap loads get block-skip.  Used by
 * `.db.splayed.set` only; the CSV and Parquet converters build their indexes
 * inline in the stream writer.  Best-effort, per numeric column. */
void      ray_splay_build_indexes(const char* dir, ray_t* tbl);
/* Append a hash index to the column file at path, using its kept chunk zone
 * Re-reads the column file, appends a hash, and falls back to appending
 * `zone` when no hash results. The caller keeps ownership of zone. Best effort. */
ray_err_t ray_splay_hash_column(const char* path, ray_t* zone);

/* Loader accepting a shared FILE domain. It resolves the generation first,
 * then refreshes the cached domain to include any externally appended symbols. */
ray_t*    ray_read_splayed_dom(const char* dir, struct ray_sym_domain_s* dom);

#endif /* RAY_SPLAY_H */
