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

/*
 * ray_rowsel — morsel-local row-filter selection.
 *
 * Replacement for the bitmap (RAY_SEL) form of g->selection used by
 * OP_FILTER on table inputs.  Stores the surviving rows of a filter
 * as morsel-local uint16 indices instead of a per-row bitmap, so the
 * downstream group / sort / agg hot loops iterate only the live rows
 * with no per-row bitmap test.
 *
 * Layout — single ray_alloc block, contiguous payload at ray_data():
 *
 *   ray_rowsel_t  meta            (24 bytes; at ray_data(block))
 *   uint8_t       seg_flags[]     (n_segs, padded to 8-byte boundary)
 *   uint32_t      seg_offsets[]   (n_segs + 1, prefix sum into idx[])
 *   uint16_t      idx[]           (total_pass entries; only MIX
 *                                  segments contribute)
 *
 * Per-segment flag values are the same NONE / ALL / MIX constants the
 * existing RAY_SEL bitmap uses (src/ops/ops.h):
 *   - NONE: no rows in this morsel pass — consumer skips wholesale.
 *   - ALL:  every row in this morsel passes — seg_offsets[seg+1]
 *           equals seg_offsets[seg], no indices stored, consumer
 *           iterates [seg_start, seg_end) densely.
 *   - MIX:  partial pass — idx[seg_offsets[seg] .. seg_offsets[seg+1])
 *           holds the morsel-local positions (0..1023) of passing
 *           rows in segment order.
 *
 * Lifetime: single-owner.  Producer (ray_rowsel_from_pred / refine)
 * returns a fresh ray_t* with rc=1.  Consumer calls ray_rowsel_release
 * to free.  No COW semantics — selection data is never shared and
 * never serialized.
 *
 * The block is allocated via ray_alloc and uses no specific type tag
 * (zeroed by ray_alloc); nothing in the runtime dispatches on it.
 * The accessors below are the only valid way to read its contents.
 *
 * Note: this is unrelated to the existing RAY_SEL type tag used by
 * src/ops/join.c and src/ops/traverse.c as a generic key-bit set.
 * Those continue to use ray_sel_* unchanged.
 */

#ifndef RAY_ROWSEL_H
#define RAY_ROWSEL_H

#include "rayforce.h"
#include "ops/ops.h"   /* RAY_SEL_NONE/ALL/MIX, RAY_MORSEL_ELEMS */

#include <stdint.h>

/* RAY_MORSEL_ELEMS must fit in uint16_t for morsel-local indices. */
_Static_assert(RAY_MORSEL_ELEMS <= 65536,
               "morsel size exceeds uint16_t index range");

/* Inline header at ray_data(block).  Pointer fields are NOT stored
 * here — they are reconstructed from this header's n_segs / total_pass
 * via the accessor inlines below.  The payload arrays live immediately
 * after this struct in the same allocation. */
typedef struct {
    int64_t  total_pass;   /* number of passing rows                   */
    int64_t  nrows;        /* source row count this selection covers   */
    uint32_t n_segs;       /* ceil(nrows / RAY_MORSEL_ELEMS)            */
    uint32_t _pad;
} ray_rowsel_t;

/* Round n up to a multiple of 8 so the next array starts aligned. */
static inline size_t ray_rowsel_pad8(size_t n) {
    return (n + 7u) & ~(size_t)7u;
}

static inline ray_rowsel_t* ray_rowsel_meta(ray_t* block) {
    return (ray_rowsel_t*)ray_data(block);
}

static inline uint8_t* ray_rowsel_flags(ray_t* block) {
    return (uint8_t*)ray_data(block) + sizeof(ray_rowsel_t);
}

static inline uint32_t* ray_rowsel_offsets(ray_t* block) {
    ray_rowsel_t* m = ray_rowsel_meta(block);
    return (uint32_t*)(ray_rowsel_flags(block) + ray_rowsel_pad8(m->n_segs));
}

static inline uint16_t* ray_rowsel_idx(ray_t* block) {
    ray_rowsel_t* m = ray_rowsel_meta(block);
    return (uint16_t*)(ray_rowsel_offsets(block) + (m->n_segs + 1));
}

/* Compute the total bytes needed for the inline payload.
 * `idx_count` is the number of uint16_t entries the idx[] array
 * needs to hold — this is the sum of popcounts over MIX segments
 * only, NOT the total passing-row count.  ALL segments contribute
 * zero to idx[]. */
static inline size_t ray_rowsel_payload_bytes(int64_t nrows, int64_t idx_count) {
    uint32_t n_segs = (uint32_t)((nrows + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);
    if (nrows <= 0) n_segs = 0;
    return sizeof(ray_rowsel_t)
         + ray_rowsel_pad8(n_segs)
         + (size_t)(n_segs + 1) * sizeof(uint32_t)
         + (size_t)idx_count    * sizeof(uint16_t);
}

/* Allocate a rowsel block.
 *
 * `nrows`      — source row count this selection covers.
 * `total_pass` — number of passing rows (ALL + MIX).  Stored in
 *                meta; consumers read it for sizing decisions.
 * `idx_count`  — number of uint16_t slots the idx[] array needs.
 *                Equal to the sum of popcounts over segments
 *                tagged MIX in the final layout.  ALL and NONE
 *                segments contribute zero.
 *
 * Header fields are populated; arrays are uninitialized.  Caller
 * fills seg_flags, seg_offsets, and idx, then hands the block off
 * (g->selection, etc.) or releases via ray_rowsel_release.
 * Returns NULL on OOM. */
ray_t* ray_rowsel_new(int64_t nrows, int64_t total_pass, int64_t idx_count);

/* Release a rowsel block.  Equivalent to ray_release / ray_free of
 * the underlying allocation — exposed under its own name for clarity
 * at call sites. */
void ray_rowsel_release(ray_t* block);

/* Build a rowsel from a RAY_BOOL predicate vector.
 *
 * pred must be a flat RAY_BOOL vec (byte-per-row).  Returns:
 *   - NULL if all rows pass (the all-pass convention is "no
 *     selection", same as g->selection == NULL).
 *   - A fresh rowsel block (rc=1) otherwise, including the
 *     none-pass case (zero-length idx, all flags NONE).
 *
 * The build runs in two parallel passes when nrows is large enough
 * to benefit (>= RAY_PARALLEL_THRESHOLD): pass 1 computes per-segment
 * popcount + flag, an inline prefix sum fills seg_offsets, pass 2
 * writes the morsel-local indices into the global idx[] (each worker
 * writes its own non-overlapping slice).  Smaller pred vecs run the
 * same logic single-threaded. */
ray_t* ray_rowsel_from_pred(ray_t* pred);

/* Flatten a rowsel into a dense int64 array of global row indices,
 * sorted ascending.  Length of the array is `meta->total_pass`.
 *
 * Returned block is a ray_t* byte buffer whose ray_data() points to
 * an `int64_t[total_pass]`.  Consumer gets a raw pointer via
 * ray_data() and releases the block when done via ray_release.
 * Returns NULL on OOM.
 *
 * Used by exec_group and similar consumers that can't cheaply walk
 * the morsel-local rowsel inline (yet) — they dispatch workers over
 * [0, total_pass) using the flattened indices directly. */
ray_t* ray_rowsel_to_indices(ray_t* sel);

/* Refine an existing rowsel by AND-ing it with a fresh predicate vec.
 *
 * Used by chained OP_FILTER on a table input that already has a
 * g->selection.  Walks `existing`'s surviving rows, tests pred at each,
 * emits a new rowsel containing only the positions that pass both.
 * Returns NULL if the result is all-pass (impossible here unless
 * existing was already all-pass), or a fresh block otherwise.
 *
 * Does not consume `existing` — caller is responsible for releasing
 * the old selection after replacing it. */
ray_t* ray_rowsel_refine(ray_t* existing, ray_t* pred);

/* Intersect two rowsels over the same source row domain.  Both inputs are
 * borrowed; the returned rowsel is fresh.  Runs in O(n_segs + mixed ids),
 * preserving NONE/ALL compression without materializing a bool vector. */
ray_t* ray_rowsel_intersect(ray_t* a, ray_t* b);

/* ──────────────────────────────────────────────────────────────────
 * Streaming builder — per-morsel selection emitter
 *
 * Foundation for the fused predicate→selection path: instead of
 * materializing a whole-table RAY_BOOL vec and scanning it twice with
 * ray_rowsel_from_pred, a predicate op streams each morsel's bools
 * through rowsel_emit_segment as it produces them.  The builder
 * classifies each segment NONE/ALL/MIX and accumulates the MIX
 * morsel-local indices into a growable scratch array; rowsel_builder_
 * finish then lays the accumulated state into a standard ray_rowsel_new
 * block whose bytes are identical to the whole-vec path's output.
 *
 * A builder owns a contiguous, ascending range of segments and must
 * receive emit_segment calls in increasing `seg` order.  finish takes
 * an array of per-worker builders whose segment ranges concatenate, in
 * order, to cover [0, n_segs); for the single-threaded case n_workers
 * is 1.
 * ────────────────────────────────────────────────────────────────── */
typedef struct {
    uint8_t*  seg_flags;   /* [n_segs] RAY_SEL_NONE/ALL/MIX             */
    uint32_t* seg_off;     /* [n_segs+1] prefix sum into this builder's
                            * idx[] (count for seg s is
                            * seg_off[s+1] - seg_off[s])                */
    uint16_t* idx;         /* growable; MIX morsel-local indices        */
    uint32_t  n_segs;      /* segments this builder covers              */
    uint32_t  idx_cap;     /* idx[] capacity in uint16_t entries        */
    uint32_t  idx_len;     /* idx[] occupancy in uint16_t entries       */
    int64_t   total_pass;  /* passing rows (ALL + MIX) across segments  */
} rowsel_builder_t;

/* Initialize a builder to cover `n_segs` segments.  Allocates
 * seg_flags[n_segs], seg_off[n_segs+1], and a geometrically growable
 * idx[] via the raw allocator.  All counters start at zero. */
void rowsel_builder_init(rowsel_builder_t* b, uint32_t n_segs);

/* Classify one morsel's `n` bools (n <= RAY_MORSEL_ELEMS) into segment
 * `seg`: NONE if zero set, ALL if all set, else MIX with the
 * morsel-local uint16 positions of set bits appended to idx[].
 * Accumulates total_pass.
 *
 * CONTRACT: `seg` is a builder-LOCAL index in [0, b->n_segs).  It is
 * NOT a global segment id.  A parallel caller that partitions segments
 * across workers must give each worker its own builder covering only its
 * assigned range [local_start, local_end), and pass builder-relative
 * indices (0 .. local_n_segs-1) to rowsel_emit_segment — NOT the
 * global segment numbers.  Passing a global id would index
 * b->seg_flags[]/b->seg_off[] past their allocated size, silently
 * corrupting the heap.
 *
 * Segments must be emitted in strictly ascending `seg` order within a
 * builder (i.e. seg=0, 1, 2, … in order — gaps are not allowed).
 *
 * For rowsel_builder_finish over multiple builders: the workers'
 * builder-local segment ranges must concatenate, in worker order, to
 * the complete global segment range [0, n_segs); equivalently,
 * sum(builders[w].n_segs for w in 0..n_workers) must equal the block's
 * global n_segs = ceil(nrows / RAY_MORSEL_ELEMS). */
void rowsel_emit_segment(rowsel_builder_t* b, uint32_t seg,
                         const uint8_t* morsel_bool, int64_t n);

/* Concatenate `n_workers` builders (segment ranges globally ordered by
 * worker index) into a fresh ray_rowsel_new(nrows, total_pass, idx_count)
 * block with the standard layout: seg_flags, seg_offsets (prefix sum
 * into idx[]), idx[].  Byte-identical to ray_rowsel_from_pred over the
 * same bools.  Frees each builder's scratch arrays before returning.
 * Returns NULL on OOM.
 *
 * INVARIANT: sum(builders[w].n_segs for w in 0..n_workers) must equal
 * ceil(nrows / RAY_MORSEL_ELEMS).  Violated if a partition over-covers
 * (asserts) or under-covers (asserts); either would leave seg_flags[]
 * entries uninitialized (ray_alloc does NOT zero the data area). */
ray_t* rowsel_builder_finish(rowsel_builder_t* builders, uint32_t n_workers,
                             int64_t nrows);

#endif /* RAY_ROWSEL_H */
