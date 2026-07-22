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
 *
 * Position-0 reservation: position 0 of every non-empty FILE domain is
 * the ordinary empty symbol "" (mirrors global id 0).  SYM has no distinct
 * null; group kernels treat id 0 as an ordinary value.  ray_sym_domain_open
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

/* No-ops on the runtime singleton. */
void ray_sym_domain_retain(ray_sym_domain_t* dom);
void ray_sym_domain_release(ray_sym_domain_t* dom);

/* Borrowed string atom for position `pos` (NULL if out of range).
 * RUNTIME: delegates to ray_sym_str.  FILE: lazily materialized atom
 * owned by the domain (arena-backed, RAY_ATTR_ARENA) — valid for the
 * domain's lifetime, do not release. */
ray_t* ray_sym_domain_str(ray_sym_domain_t* dom, int64_t pos);

/* Position of `str` in the domain, or -1 if absent.
 * FILE: builds the reverse index on first call (O(|vocabulary|)). */
int64_t ray_sym_domain_find(ray_sym_domain_t* dom, const char* str, size_t len);

/* Position → runtime-intern-id translation table.
 *
 *   RUNTIME: NULL — ids ARE runtime ids, callers use them as-is.
 *   FILE:    `count` entries, lut[pos] = ray_sym_intern(vocab[pos]);
 *            built ONCE on first request (double-checked under the
 *            domain lock, atomically published), then lock-free reads
 *            for the domain's lifetime.
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

/* RAY_SYM_AUDIT=1 support (cached at ray_sym_init): when set,
 * ray_sym_vec_cell cross-checks every resolution and aborts with full
 * context on an invariant violation (unresolvable atom / position out
 * of domain range).  OFF by default — a single predictable branch. */
extern uint8_t ray_g_sym_audit;
void ray_sym_audit_cell(ray_t* vec, int64_t row, int64_t pos, ray_t* resolved);

#endif /* RAY_DOMAIN_H */
