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

#endif /* RAY_ROWSEL_H */
