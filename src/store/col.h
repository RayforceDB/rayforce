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

#ifndef RAY_COL_H
#define RAY_COL_H

#include <rayforce.h>
#include <string.h>
#include <stdio.h>
#include "store/colrange.h"

struct ray_sym_domain_s;

/* On-disk column format generation, carried in the 32-byte header's `order`
 * byte (offset 17).  The on-disk header IS the in-memory ray_t allocator
 * layout (payload at offset 32); there is NO separate envelope.
 *
 * Placement rationale: of the header bytes, only mmod(16) and order(17) are
 * on-disk-free (written 0 and recomputed on load).  aux(0-15) is RESERVED
 * for postponed on-disk index persistence (min/max zone map) and must not
 * be squatted; rc(20-23) carries the SYM saved dictionary count;
 * type/attrs/len are live data.  So the generation lives in `order`.
 *
 * A SINGLE byte = MAJOR generation.  Compatibility is gated by major only:
 * minor/additive changes stay backward-compatible, so one byte (0-255
 * generations) suffices.  There is NO magic — the type allowlist +
 * len-vs-filesize + SYM rc saved-count already validate file integrity;
 * this byte only gates the format generation.
 *
 * The CURRENT generation is 0, and this is deliberate.  `order` is
 * on-disk-free and was written 0 by every engine that produced today's
 * splayed layout, so declaring the shipped format generation 0 means all
 * existing on-disk data is read as-is — no migration, no orphaned databases.
 * (An earlier build briefly stamped 1, which rejected every pre-existing file
 * with a "version" error: see ray_col_check_format.  Generation 0 reclaims
 * that data.)  Generation 1 is intentionally SKIPPED because those stray
 * order==1 files carry the identical generation-0 layout; the next genuine
 * breaking layout change bumps MAJOR straight to 2, at which point
 * generation-0 files are correctly rejected and must be migrated rather than
 * silently mis-decoded. */
#define RAY_COL_FORMAT_MAJOR    ((uint8_t)0)

/* Stamp the format generation into a 32-byte on-disk header's `order` byte.
 * Does NOT touch aux (reserved for postponed index persistence).  Writes the
 * current generation; the runtime `order` is recomputed on load. */
static inline void ray_col_stamp_format(ray_t* header) {
    header->order = RAY_COL_FORMAT_MAJOR;
}

/* Accepts the generations this build reads (0 and 2); the pair check that
 * separates them is ray_col_gen_of (store/encfmt.h). */
static inline ray_err_t ray_col_check_format(const ray_t* header) {
    return (header->order == RAY_COL_FORMAT_MAJOR || header->order == 2)
           ? RAY_OK : RAY_ERR_VERSION;
}

/* Allowlist of attr bits the save path persists; see the rationale in
 * col.c (loaders mask untrusted disk attrs to this set).  Literal values
 * because this header does not include table/sym.h; col.c asserts them. */
#define COL_DISK_ATTRS_MASK ((uint8_t)(0x03 /* RAY_SYM_W_MASK */ | \
                                       0x20 /* RAY_ATTR_SORTED */ | \
                                       0x40 /* RAY_ATTR_HAS_NULLS */))

/* STR column files: a writer that stored every pooled descriptor's content
 * hash (ray_str_t.hash32) says so by putting this value in the header's
 * `rc` field, which is otherwise 0 on disk for STR (RAY_SYM uses it for
 * the saved dictionary count).  A reader that sees the mark takes the
 * descriptors as written; without it (files from before the hash cache,
 * whose final four descriptor bytes are padding) the loaders recompute the
 * hashes and repair them in the private mapping.  Mapping a marked file
 * therefore costs no pass over the pool and leaves the descriptor pages
 * shared with the page cache. */
#define RAY_COL_STR_HASHED      ((uint32_t)0x48534148)  /* "HASH" */

static inline void ray_col_stamp_str_hashed(ray_t* header) {
    header->rc = RAY_COL_STR_HASHED;
}

/* Column file I/O.
 *
 * The bare save/load/mmap entry points keep PROCESS-LOCAL semantics for
 * RAY_SYM columns: cells are written as runtime intern ids (re-expressed
 * through the vec's domain when it is a FILE domain) and loads attach
 * the runtime singleton, bounds-validated against the global table.
 *
 * Tables persisted as splayed dirs use the *_sym_encoded / *_dom
 * variants: on-disk SYM data is positions in the table's symfile and
 * loads attach that symfile's domain (sym-domain architecture spec). */
ray_err_t ray_col_save(ray_t* vec, const char* path);
ray_err_t ray_col_save_bulk(ray_t* vec, const char* path);
/* Write `vec` in the given generation (0 or 2).  ray_col_save / _bulk stay
 * generation 0 only.  A column generation 2 cannot hold is written as 0. */
ray_err_t ray_col_save_gen(ray_t* vec, const char* path, uint8_t gen);
ray_err_t ray_col_save_bulk_gen(ray_t* vec, const char* path, uint8_t gen);

/* Write context (spec "Writers"): a bound source (plain or encoded), the
 * resolved generation and the SYM target domain.  Handles plain->0,
 * plain->2, encoded->0 (materialize, then the generation-0 writer) and
 * encoded->2 (range reads, SYM ids remapped into the target domain, no
 * whole-column materialization).  On failure every temporary is released and
 * the staging file removed; nothing is renamed into place. */
/* The tri-state `compress` option of a save (spec "Generation
 * resolution"): explicit true / false set every file; absent inherits. */
typedef enum { RAY_COMPRESS_ABSENT = 0, RAY_COMPRESS_ON = 1, RAY_COMPRESS_OFF = 2 } ray_compress_t;

enum { RAY_COL_IDX_KEEP = 0, RAY_COL_IDX_REBUILD = 1, RAY_COL_IDX_DROP = 2 };
typedef struct {
    ray_col_src_t src;                    /* bound by the caller (ray_col_src_bind); not released here */
    const char*   name;                   /* column name, diagnostics */
    uint8_t       gen;                    /* resolved generation: 0 or 2 */
    struct ray_sym_domain_s* target_dom;  /* SYM: destination symfile domain; NULL = bare */
    uint8_t       index_decision;         /* RAY_COL_IDX_*: generation 2 keeps, rebuilds or drops a
                                           * region index; generation 0 takes KEEP only */
    uint8_t       index_kind;             /* generation 2: kind to record (0 = from the source) */
    bool          durable;
} ray_col_write_t;
ray_err_t ray_col_save_ctx(ray_col_write_t* w, const char* path);
void      ray_col_test_fail_alloc(int64_t nth);   /* test hook; 0 disarms */
/* Spec "Writing the index" (P6b): generation 0's STR DICT build condition,
 * STR and RAY_DICT_MIN_ROWS <= len <= INT32_MAX (splay_build_index_col,
 * ray_dict_acc_finish).  The callers add their own path rule (index
 * building asked for, no HASH).  False while ray_col_test_no_dict is on. */
bool      ray_col_dict_wanted(int8_t type, int64_t len);
/* Test hook: true makes ray_col_dict_wanted answer false (a generation-2
 * STR save writes no DICT); false disarms (ray_test_hooks_off). */
void      ray_col_test_no_dict(bool on);

/* Generation resolution, decided by a save under the destination's
 * .write.lock.  `dest_cur` is the destination's current published
 * generation directory, NULL or "" when it holds no published table.
 * ON: 2 for every file generation 2 can hold; OFF: 0; ABSENT: 2 when the
 * source is a RAY_ENCCOL or the destination's current generation stores the
 * file `name` as generation 2, else 0.  Linked, nested and list columns
 * are always 0; an indexed column is generation 2 with its index region
 * (a SYM column's HASH or PART rebuilt over the target ids). */
/* lossy: read errors read as 0 — writers use the *_err forms */
uint8_t ray_col_file_gen(const char* path);   /* 2 when the file is generation 2, else 0 (missing included) */
/* lossy: read errors read as 0 — writers use the *_err forms */
uint8_t ray_col_resolve_gen(ray_compress_t mode, ray_t* src,
                            const char* dest_cur, const char* name, size_t name_len);
/* The same rule for writers that have only the column's type (streams).
 * lossy: read errors read as 0 — writers use the *_err forms */
uint8_t ray_col_resolve_gen_type(ray_compress_t mode, int8_t type,
                                 const char* dest_cur, const char* name, size_t name_len);
/* Checked forms, for savers: a missing destination file is generation 0,
 * but a file that cannot be read (permissions, short, bad discriminator) or
 * a path that overflows is an error, never a silent 0.  The uint8_t forms
 * above read every error as 0. */
ray_err_t ray_col_file_gen_err(const char* path, uint8_t* gen);
/* First bytes of a magic-prefixed (header-less, generation-0) column file? */
bool      ray_col_header_is_ext(const void* head, size_t n);
ray_err_t ray_col_resolve_gen_err(ray_compress_t mode, ray_t* src, const char* dest_cur,
                                  const char* name, size_t name_len, uint8_t* gen);
/* Would resolving `src` under `mode` consult the destination?  Only an
 * inheriting (ABSENT) save of a plain eligible column does. */
bool      ray_col_resolve_needs_dest(ray_compress_t mode, ray_t* src);
/* The same question for writers that have only the column's type (streams). */
bool      ray_col_resolve_type_needs_dest(ray_compress_t mode, int8_t type);
ray_err_t ray_col_resolve_gen_type_err(ray_compress_t mode, int8_t type, const char* dest_cur,
                                       const char* name, size_t name_len, uint8_t* gen);
/* Parse the `compress` option from `opts` (a RAY_DICT with SYM keys; NULL:
 * absent).  strict: any key other than `compress` is a domain error.  A
 * non-bool `compress` is a domain error.  Returns NULL or an owned error. */
ray_t* ray_compress_opt_parse(ray_t* opts, bool strict, ray_compress_t* mode);
void    ray_store_test_force_compress(bool on);   /* equivalence run: absent behaves as true */

/* Can generation 2 hold this plain vector (type, no link)?  Index meta:
 * the kind (every fresh kind) and the UNIQUE flag a plain column records;
 * a stale root records none. */
bool ray_col_gen2_eligible(ray_t* col);
void ray_col_gen2_index_meta(ray_t* col, uint8_t* index_kind, uint8_t* col_flags);
/* Validate the complete object graph accepted by ray_col_save without
 * opening or modifying a file.  Splayed saves use this before mutating a
 * symfile or any already-committed column generation. */
ray_err_t ray_col_save_preflight(ray_t* vec);
ray_t*    ray_col_load(const char* path);
ray_t*    ray_col_mmap(const char* path);

/* Write a RAY_SYM column re-encoded as positions in `target` (width =
 * ray_sym_dict_width(domain count); header rc = domain count at save).
 * Caller must have interned the column's distinct symbols into `target`
 * and FLUSHED the domain first for live-table writes. Private import staging
 * may defer the flush until workers join, before publishing the root.
 * A cell whose symbol is absent from `target` is RAY_ERR_CORRUPT. */
ray_err_t ray_col_save_sym_encoded(ray_t* vec, const char* path,
                                   struct ray_sym_domain_s* target,
                                   bool durable);

/* Domain-attaching loaders for splayed/parted tables.  `dom` is the
 * table's symfile domain; RAY_SYM columns attach it (retained per
 * column) and bounds-validate against its count.  A RAY_SYM column with
 * dom == NULL is a loud "sym" error — a stored SYM column without a
 * resolvable symfile must never resolve against incidental state. */
ray_t*    ray_col_load_dom(const char* path, struct ray_sym_domain_s* dom);
ray_t*    ray_col_mmap_splayed_dom(const char* path, struct ray_sym_domain_s* dom);

/* Scattered reads of a mapped column.  A sampler or a gather that reads
 * scattered rows of a column mapped from a file and not cached waits on one
 * page fault per row; asking for the rows' pages first turns that into
 * about one wait.  ray_col_cold: `col` is a mapped fixed-width column whose
 * pages are not resident (a few page probes), so a pre-pass pays off.
 * ray_col_want_row: request the page of one row (no checks).
 * ray_col_want_rows: request the pages of rows idx[0..n) of a mapped
 * column, skipping a column found resident: each page once and runs of
 * adjacent pages as one range, whatever order the rows come in (the whole
 * column for n >= its length). */
bool      ray_col_cold(ray_t* col);
void      ray_col_want_row(ray_t* col, int64_t row);
void      ray_col_want_rows(ray_t* col, const int64_t* idx, int64_t n);

/* Append an inline index region to an existing (index-less) column file — used
 * by the streaming .csv.splayed builder which writes raw columns first.  `ix`
 * is a const ray_index_t* (void to avoid the ops/ type dependency in this
 * store/ header). */
ray_err_t ray_col_append_index(const char* path, const void* ix,
                               int64_t col_len, int8_t col_type);

/* Build the hash index of `col`, the payload of index-less column file
 * `path`, and append it as the inline region, built in place: the file is
 * extended (its blocks allocated) and the region mapped, and the arrays are
 * written there once instead of staged in memory and copied in.  Same bytes
 * as ray_col_append_index of the index ray_index_attach_hash builds.  The
 * marker is stamped last; on failure the file is cut back to the payload.
 * RAY_ERR_NYI (the file left at, or cut back to, its payload) where the
 * in-place build does not apply and the caller builds in memory and appends
 * instead: a STR column; a platform or filesystem where the region's blocks
 * cannot be held before the mapping writes them (anything but Linux on
 * ext4, xfs or tmpfs: overlayfs too, whose upper layer cannot be told);
 * or a region that could not be allocated
 * or mapped this time.  Any other error is final (RAY_ERR_CANCEL, OOM, an
 * I/O error on the marker, RAY_ERR_CORRUPT for a file already indexed).
 * `trace` is a ray_hash_trace_t* (NULL: none), void as `ix` above. */
ray_err_t ray_col_build_hash_index(const char* path, ray_t* col, void* trace);

/* Seek f to its end, pad to 32 bytes and write ix's inline region there;
 * flushes. Does not touch the header. The caller stamps the marker with
 * ray_col_stamp_index or rolls the file back on error. */
ray_err_t ray_col_write_index_region(FILE* f, const void* ix_v, int64_t* payload_end_out);

/* Flush f and cut it to `len` bytes: undoes a partial index region. */
bool ray_col_truncate(FILE* f, int64_t len);

/* Put the "index present" marker into a 32-byte header image. */
void      ray_col_stamp_index(ray_t* hdr);


#endif /* RAY_COL_H */
