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

#ifndef RAY_DOMAIN_H
#define RAY_DOMAIN_H

/*
 * domain.h -- Symbol-resolution domains.
 *
 * A domain is the dictionary a RAY_SYM vector's indices resolve over.
 * Two kinds:
 *
 *   RUNTIME -- an immortal singleton wrapping the global intern table
 *              (g_sym, src/table/sym.c).  Every runtime-created SYM vec
 *              points at it.  retain/release skip it entirely (no atomic
 *              churn on the vec free hot path).
 *
 *   FILE    -- an mmapped symfile base (the STRL format) plus a growable
 *              in-memory tail of appended symbols, opened through a
 *              process-wide resolved-path-keyed refcounted cache:
 *              opening the same resolved path twice yields the SAME
 *              object — pointer equality is domain identity, always.
 *              The reverse index (find) is built lazily on first use;
 *              per-position string atoms are materialized EAGERLY at
 *              open / append and owned by the domain (borrowed by
 *              callers, exactly like ray_sym_str), making
 *              ray_sym_domain_str a lock-free array read.
 *              ray_sym_domain_new creates a private in-memory instance of
 *              this dictionary, with no backing path or cache entry.
 *
 * Position-0 reservation: position 0 of every non-empty FILE domain is
 * the canonical empty/null symbol "" (mirrors global id 0).  Group kernels
 * retain id 0 as a groupable null key.  ray_sym_domain_open
 * VALIDATES this and refuses (NULL) files that violate it; intern on an
 * empty domain seeds "" at position 0 before the first real symbol, so
 * newly created symfiles always carry it.  Empty vocabularies are fine.
 *
 * Growth (the flip, Task 7b): ray_sym_domain_intern find-or-appends into
 * the shared object — append-only, positions are permanent.  Appends
 * extend the published atom array by REPLACING it with a grown copy
 * (atomic publish; replaced arrays are retired, not freed, so lock-free
 * readers holding the old pointer stay valid for the domain's lifetime).
 * ray_sym_domain_flush persists base+tail via tmp + atomic rename under
 * the symfile's `.lk` flock and verifies the on-disk prefix still
 * matches what this object knows (anything else is a loud
 * concurrent-writer RAY_ERR_CORRUPT — never a silent remap).
 * Note: the spec's hold-the-`.lk`-for-the-writer's-lifetime contract is
 * deferred with multi-process writer stress (out of scope this phase);
 * the flock is taken per flush.
 *
 * Lifecycle: FILE domains are refcounted.  Every attached SYM column
 * holds a ref (taken on attach and in ray_retain_owned_refs, dropped on
 * vec free next to the str_pool/index owned-ref handling).  The cache
 * entry itself is weak: the last release unlinks the entry, munmaps the
 * base mapping and frees the lazy structures — a subsequent open builds
 * a fresh mapping.
 */

#include <rayforce.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct ray_sym_domain_s ray_sym_domain_t;

/* Immortal singleton delegating to the global intern table.
 * (Also declared in rayforce.h for the inline ray_sym_vec_domain.) */
ray_sym_domain_t* ray_sym_runtime_domain(void);

/* Open (or re-use from the cache) the FILE domain for `path` — a symfile
 * in the STRL format.  Returns NULL on I/O or format errors.  The
 * returned object carries one reference per open; release with
 * ray_sym_domain_release.  A cache hit revalidates against the file:
 * external append-only growth EXTENDS the shared object in place;
 * any other divergence (shrunk / rewritten file, or growth while this
 * process holds unflushed appends) returns NULL — loud, never a silent
 * remap. */
ray_sym_domain_t* ray_sym_domain_open(const char* path);

/* Like ray_sym_domain_open, but a missing file yields a fresh EMPTY
 * domain (vocabulary written on first flush).  The save path's
 * open-or-create entry point. */
ray_sym_domain_t* ray_sym_domain_open_or_create(const char* path);

/* A fresh symbol file at `path` that one converter builds alone, the
 * strings appended straight into the mapped file (see table/symimp.h):
 * intern, intern_batch, count, flush / flush_append (append: header and
 * writeback; durable: cut and sync) and release only.  Not cached by path.
 * NULL where unsupported or on failure; the caller then uses
 * ray_sym_domain_open_or_create. */
ray_sym_domain_t* ray_sym_domain_create_import(const char* path);
/* Such a domain's interning counts (RAY_CSV_TRACE), as ray_symimp_stats
 * gives them (table/symimp.h).  False for any other domain. */
struct ray_symimp_stats_s;
bool ray_sym_domain_import_stats(ray_sym_domain_t* dom, struct ray_symimp_stats_s* out);
/* Such a domain's import dictionary (for the grouped import,
 * table/symgrp.h); NULL for any other domain. */
struct ray_symimp_s* ray_sym_domain_import(ray_sym_domain_t* dom);

/* New private, refcounted in-memory dictionary. Supports the same concurrent
 * append and resolution operations as FILE domains, without a cache entry or
 * backing file. Attached vectors keep it alive. flush returns RAY_ERR_DOMAIN. */
ray_sym_domain_t* ray_sym_domain_new(void);

/* No-ops on the runtime singleton. */
void ray_sym_domain_retain(ray_sym_domain_t* dom);
void ray_sym_domain_release(ray_sym_domain_t* dom);

/* Borrowed string atom for position `pos` (NULL if out of range).
 * RUNTIME: delegates to ray_sym_str.  FILE: lazily materialized atom
 * owned by the domain (arena-backed, RAY_ATTR_ARENA) — valid for the
 * domain's lifetime, do not release. */
ray_t* ray_sym_domain_str(ray_sym_domain_t* dom, int64_t pos);

/* Raw vocabulary access for a FILE domain: the bytes of the entries the
 * file holds, read straight from the mapping — no atom, no lock.  Pin
 * takes an atomically published snapshot of (map, block table, count); an
 * entry is found from the checkpoint of its block of RAY_SYM_INDEX_STRIDE
 * entries.
 *
 * LIFETIME: a snapshot stays readable across in-process appends
 * (ray_sym_domain_intern, _batch, flush) for as long as its holder keeps
 * the domain alive.  It does NOT survive a reopen of the same symfile
 * after another process grew it (ray_sym_domain_open on a cached path:
 * the extend frees the mapping and block table it replaces).  Nothing
 * counts pins, so this is a rule for callers: never hold a snapshot across
 * a call that may reopen (.db.parted.get, splayed reads,
 * ray_sym_domain_open), and never reopen from a thread while another runs
 * a kernel over the domain.  Today every reopen runs on the calling thread
 * between kernels.
 *
 * Returns false for the runtime domain or an empty file; the caller then
 * resolves through ray_sym_domain_str as before.  Only positions below
 * `count` (the file prefix) are readable this way; runtime-appended
 * positions keep going through ray_sym_domain_str.  ray_sym_domain_raw_str
 * returns NULL (and *len 0) for a position out of range.  Unpin is a no-op
 * kept for symmetry.
 *
 * A compressed symfile (each record FSST-coded, see domain.c) has `z` set:
 * its strings are decoded into a small ring of buffers of the calling
 * thread, so a pointer ray_sym_domain_raw_str returns for it stays valid
 * only until that thread's RAY_SYM_RAW_RING-th next read of a compressed
 * entry.  Enough for a comparison of two entries; a caller that keeps
 * pointers longer copies the bytes (or reads their lengths with
 * ray_sym_domain_raw_len, which decodes nothing). */
#define RAY_SYM_INDEX_STRIDE 28   /* entries in a block of the index: 64 bytes with its offset */
typedef struct {
    uint64_t off;                          /* byte offset of the block's first record */
    uint16_t len[RAY_SYM_INDEX_STRIDE];    /* the entries' string lengths, 65535 = 65535 or longer */
} ray_sym_ixblk_t;
_Static_assert(sizeof(ray_sym_ixblk_t) == 64, "an index block is one cache line");
#define RAY_SYM_RAW_RING 4   /* decode buffers per thread for compressed entries */
typedef struct {
    const unsigned char* map;
    const ray_sym_ixblk_t* ib;   /* block k: starts at map + ib[k].off; NULL: none */
    int64_t              count;
    void*                blk;    /* the domain's block table */
    ray_sym_domain_t*    dom;
    const void*          z;      /* the symbol table of a compressed file; NULL: plain */
} ray_sym_domain_raw_t;
bool ray_sym_domain_raw_pin(ray_sym_domain_t* dom, ray_sym_domain_raw_t* out);
/* Reads the file of a FILE domain whole into the page cache, in parallel,
 * when the memory left after it (the RAM limit less the committed heap and
 * the files read so far) is at least `headroom`; else leaves it lazy.  Once
 * per domain.  RAY_SYM_PRELOAD=0 never reads, =1 always; RAY_SYM_TRACE
 * prints the decision. */
void ray_sym_domain_preload(ray_sym_domain_t* dom, int64_t headroom);
void ray_sym_domain_raw_unpin(ray_sym_domain_t* dom);
const char* ray_sym_domain_raw_str_z(const ray_sym_domain_raw_t* r, int64_t pos, size_t* len);
/* Entry `pos` (below r->count) of a compressed file decoded into `out`, which
 * holds its length (ray_sym_domain_raw_len) and 8 bytes more: a caller that
 * keeps the bytes places them itself. */
void ray_sym_domain_raw_decode(const ray_sym_domain_raw_t* r, int64_t pos, char* out);
static inline const char* ray_sym_domain_raw_str(const ray_sym_domain_raw_t* r,
                                                 int64_t pos, size_t* len) {
    if (pos < 0 || pos >= r->count || !r->ib) { *len = 0; return NULL; }
    if (r->z) return ray_sym_domain_raw_str_z(r, pos, len);
    /* past the records before the entry by their lengths (65535: the
     * record header's) */
    const int64_t k = pos / RAY_SYM_INDEX_STRIDE;
    const unsigned char* p = r->map + r->ib[k].off;
    for (int64_t i = k * RAY_SYM_INDEX_STRIDE; i < pos; i++) {
        uint32_t l = r->ib[k].len[i - k * RAY_SYM_INDEX_STRIDE];
        if (l == 65535) memcpy(&l, p, 4);
        p += 4 + (size_t)l;
    }
    uint32_t l; memcpy(&l, p, 4);
    *len = l;
    return (const char*)p + 4;
}
/* The length of entry `pos`'s string (-1 where ray_sym_domain_raw_str would
 * give NULL): from its length (2 bytes in the index), without reading the
 * record, unless the string is 65535 bytes or longer (the escape) — then
 * its record header (4 bytes).  For a function of a string's length per
 * symbol (strlen per code), so that a vocabulary's lengths are read and
 * not its strings. */
static inline int64_t ray_sym_domain_raw_len(const ray_sym_domain_raw_t* r, int64_t pos) {
    if (pos < 0 || pos >= r->count || !r->ib) return -1;
    const uint16_t e = r->ib[pos / RAY_SYM_INDEX_STRIDE].len[pos % RAY_SYM_INDEX_STRIDE];
    if (e < 65535) return e;
    size_t l = 0;
    (void)ray_sym_domain_raw_str(r, pos, &l);
    return (int64_t)l;
}
/* The same for any domain and position (the appended ones from their atom). */
int64_t ray_sym_domain_strlen(ray_sym_domain_t* dom, int64_t pos);

/* What the record index did for a FILE domain, for tests: the open took
 * its blocks from the index file (covering `covered` entries), and whether
 * the lengths are read from the index file. */
typedef struct {
    bool    from_index;    /* the open took the table from the index file */
    bool    lens_mapped;   /* the lengths are read from the index file, as needed */
    int64_t covered;       /* entries the index covered */
} ray_sym_domain_ix_info_t;
bool ray_sym_domain_ix_info(ray_sym_domain_t* dom, ray_sym_domain_ix_info_t* out);

/* What the record index guarantees, and where it stops (domain.c):
 *   - An index is taken only for the very file it was written for: the
 *     inode, the modification and status-change times and the size it
 *     records match the file mapped.  The status-change time cannot be set
 *     back by a writer, so an in-place rewrite, even with its modification
 *     time restored, is not taken for the same file.  Any other index —
 *     another file of the name, the file grown or rewritten by a writer
 *     that left the index as it was, an earlier format — is not used: the
 *     open walks every record once and writes the index anew.  Where the
 *     platform has no such identity (Windows, WASM) every open walks.
 *   - The symfile and its index are ours: an index taken is read as
 *     written, its blocks and the records they name are not checked. */

/* The symfile's record index, `<symfile>.ix` (format in domain.c): written
 * by the writers of a symfile so that opening it reads neither the records
 * nor their lengths.  Save: ckpt[k] is the byte offset of the record of
 * entry k * RAY_SYM_INDEX_STRIDE, n = ceil(count / RAY_SYM_INDEX_STRIDE),
 * lens[i] the length of entry i's string (65535: 65535 or longer), the first
 * `count` records ending at byte `end`; best effort, and a no-op for a small
 * file.  Remove: before a new file is made under the name. */
void ray_sym_domain_index_save(const char* path, const uint64_t* ckpt, int64_t n,
                               const uint16_t* lens, int64_t count, uint64_t end);
void ray_sym_domain_index_remove(const char* path);

/* Position of `str` in the domain, or -1 if absent.
 * FILE: builds the reverse index on first call (O(|vocabulary|)). */
int64_t ray_sym_domain_find(ray_sym_domain_t* dom, const char* str, size_t len);

/* Position → runtime-intern-id translation table.
 *
 *   RUNTIME: NULL — ids ARE runtime ids, callers use them as-is.
 *   FILE:    `count` entries, lut[pos] = ray_sym_intern(vocab[pos]);
 *            built ONCE on first request (double-checked under the
 *            domain lock, atomically published), then lock-free reads.
 *            Invalidated by growth: an append retires it (the pointer
 *            stays valid memory), a reopen after external growth frees
 *            it — same rule as a raw snapshot, see ray_sym_domain_raw_pin.
 *
 * Building the LUT INTERNS the whole vocabulary into the global table —
 * the sanctioned, SEQUENTIAL one-time cost of crossing a FILE domain
 * into runtime-id space (O(|vocabulary|), never O(rows)).  Contract:
 * any path that translates ids inside ray_pool_dispatch workers must
 * obtain the LUT during sequential setup (join/window setup do) —
 * interning inside a worker violates sym.c's frozen-table rule.
 * Returns NULL on OOM (FILE domains with a non-empty vocabulary). */
const int64_t* ray_sym_domain_runtime_lut(ray_sym_domain_t* dom);

/* Find-or-append.  RUNTIME: delegates to ray_sym_intern.  FILE:
 * append-only in-memory tail over the mmapped base; an empty domain is
 * seeded with "" at position 0 before the first real symbol.  Returns
 * the position, or -1 on OOM.  Appends are visible to every holder of
 * the shared object immediately; ray_sym_domain_flush persists them. */
int64_t ray_sym_domain_intern(ray_sym_domain_t* dom, const char* str, size_t len);

/* Batch find-or-append of n strings (hashes[i] = (uint32_t)ray_hash_bytes
 * of strs[i]); out_pos[i] receives the position.  Repeats inside the
 * batch resolve to one position.  FILE: the lookup of the existing
 * vocabulary runs in parallel on the pool; new strings are appended in
 * batch order (a string's first occurrence), whatever the worker count.  RUNTIME: one ray_sym_intern per entry.
 * Returns false on OOM (out_pos then holds -1 for the entries not
 * resolved). */
bool ray_sym_domain_intern_batch(ray_sym_domain_t* dom, int64_t n,
                                 const char* const* strs, const size_t* lens,
                                 const uint32_t* hashes, int64_t* out_pos);
#ifdef DEBUG
enum {
    RAY_SYM_DOMAIN_CANCEL_PROBE = 1,
    RAY_SYM_DOMAIN_CANCEL_DEDUPE = 2,
    RAY_SYM_DOMAIN_CANCEL_INSERT = 3   /* before the commit's insert dispatch */
};
void ray_sym_domain_batch_cancel_for_test(int phase);
#endif
/* The same with hashes[i] = ray_hash_bytes of strs[i], all 64 bits: the
 * import dictionary keys on them (the 32-bit entry point hashes its strings
 * again for it); the other domains take their low 32 bits. */
bool ray_sym_domain_intern_batch64(ray_sym_domain_t* dom, int64_t n,
                                   const char* const* strs, const size_t* lens,
                                   const uint64_t* hashes, int64_t* out_pos);

/* Number of entries in the domain. */
int64_t ray_sym_domain_count(ray_sym_domain_t* dom);

/* Resolved (realpath) symfile path; NULL for the runtime domain. */
const char* ray_sym_domain_path(ray_sym_domain_t* dom);

/* Write base + dirty tail to the symfile (tmp + atomic rename under the
 * `.lk` flock).  Verifies the on-disk file still matches this object's
 * persisted prefix first — divergence is a hard RAY_ERR_CORRUPT
 * ("concurrent writer"), never a silent remap.  No-op (RAY_OK) when
 * nothing new was interned.  RUNTIME: RAY_OK (the global table owns its
 * own persistence). */
ray_err_t ray_sym_domain_flush(ray_sym_domain_t* dom, bool durable);

/* Append the entries interned since the last flush to the symfile in place
 * (header count rewritten after the records), instead of rewriting the
 * whole file through a tmp + rename.  For a symfile one writer builds
 * alone, such as a converter's staging domain, flushed as it grows so the
 * last flush writes only the tail.  Same `.lk` lock as
 * ray_sym_domain_flush; the file must still have the persisted size.
 * `durable` also syncs the file and its directory, even when nothing new
 * was interned. */
ray_err_t ray_sym_domain_flush_append(ray_sym_domain_t* dom, bool durable);

/* Rewrite the plain symfile at `path` compressed: each string coded with an
 * FSST symbol table trained on a sample of them (domain.c, "STRZ"), the
 * positions, lengths and index kept.  For a file written whole and read
 * after, such as one column's vocabulary: reads of an entry then decode it
 * (ray_sym_domain_raw_str), and the file and the memory it takes when read
 * shrink.  No domain may have the file open (RAY_ERR_DOMAIN).  A file with
 * no entries, one already compressed or one no table helps stays as it is
 * (RAY_OK). */
ray_err_t ray_sym_domain_compress(const char* path);

/* RAY_SYM_AUDIT=1 support (cached at ray_sym_init): when set,
 * ray_sym_vec_cell cross-checks every resolution and aborts with full
 * context on an invariant violation (unresolvable atom / position out
 * of domain range).  OFF by default — a single predictable branch. */
extern uint8_t ray_g_sym_audit;
void ray_sym_audit_cell(ray_t* vec, int64_t row, int64_t pos, ray_t* resolved);

#endif /* RAY_DOMAIN_H */
