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
#include "store/col.h"

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
    ray_compress_t mode;    /* the call's tri-state compress option */
    char cur[1024];         /* destination's current generation dir, "" when none (under the lock) */
    ray_err_t cur_err;      /* resolving cur failed: only an inheriting column fails on it */
    uint8_t* gens;          /* resolved generation per column file (ray_alloc_raw), freed by finish */
    int64_t ngens;
    bool sym_col;           /* per-column symfiles (RAY_SYMFILE_COLUMN); set after begin */
} ray_splay_write_t;

ray_err_t ray_splay_write_begin(const char* dir, ray_splay_write_t* write);
/* begin with the call's compress option; the generations are resolved
 * per file by ray_splay_write_table_ctx against write->cur. */
ray_err_t ray_splay_write_begin_mode(const char* dir, ray_compress_t mode,
                                     ray_splay_write_t* write);
ray_err_t ray_splay_write_finish(ray_splay_write_t* write, ray_err_t result,
                                  bool durable);
/* Write only to a fresh/unpublished directory owned by the caller. */
ray_err_t ray_splay_write_table(ray_t* tbl, const char* dir,
                                 const char* sym_path, bool durable);
/* Write into write->dir under the held lock, resolving each file's
 * generation from write->mode and write->cur (kept in write->gens). */
ray_err_t ray_splay_write_table_ctx(ray_t* tbl, ray_splay_write_t* write,
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
/* The same with the call's compress option (ray_compress_t); the forms
 * above pass RAY_COMPRESS_ABSENT, so every caller inherits. */
ray_err_t ray_splay_save_mode(ray_t* tbl, const char* dir, const char* sym_path,
                              ray_compress_t mode);
ray_err_t ray_splay_save_bulk_mode(ray_t* tbl, const char* dir, const char* sym_path,
                                   ray_compress_t mode);
ray_err_t ray_splay_save_staged_bulk_mode(ray_t* tbl, const char* dir, const char* sym_path,
                                          ray_compress_t mode);
/* The symfile layout of a save.  SHARED (the default): one symfile,
 * sym_path, holds the vocabulary of every SYM column.  COLUMN: each SYM
 * column gets a symfile of its own next to its column file,
 * `<dir>/.<col>.sym` (ray_splay_col_sym_path; dir is the directory the
 * columns are written to, the selected generation once the table has one),
 * holding that column's strings only, so a scan of one column reads none
 * of another's; sym_path is then not written.  The loaders find a column's
 * own symfile by that name and resolve the column over it, whatever symfile
 * they were given; a column without one resolves over the shared domain as
 * before. */
typedef enum { RAY_SYMFILE_SHARED = 0, RAY_SYMFILE_COLUMN = 1 } ray_symfile_t;
ray_err_t ray_splay_save_layout(ray_t* tbl, const char* dir, const char* sym_path,
                                ray_compress_t mode, ray_symfile_t layout);
/* The `symfile` option of an options dict (NULL or no key: SHARED): the
 * symbol 'shared or 'column, anything else a domain error.  Returns NULL or
 * an owned error. */
ray_t*    ray_symfile_opt_parse(ray_t* opts, ray_symfile_t* layout);
/* `<dir>/.<name>.sym` into out; false when it does not fit. */
bool      ray_splay_col_sym_path(char* out, size_t cap, const char* dir,
                                 const char* name, size_t name_len);
ray_t*    ray_splay_load(const char* dir, const char* sym_path);
ray_t*    ray_read_splayed(const char* dir, const char* sym_path);

/* Append chunk-zone index regions to the column files of a splayed store
 * written from a resident table, so later mmap loads get block-skip.  Used by
 * `.db.splayed.set` only; the CSV and Parquet converters build their indexes
 * inline in the stream writer.  Best-effort, per numeric column. */
void      ray_splay_build_indexes(const char* dir, ray_t* tbl);

/* Loader accepting a shared FILE domain. It resolves the generation first,
 * then refreshes the cached domain to include any externally appended symbols. */
ray_t*    ray_read_splayed_dom(const char* dir, struct ray_sym_domain_s* dom);

#endif /* RAY_SPLAY_H */
