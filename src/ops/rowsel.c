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
 * ray_rowsel — implementation.  See src/ops/rowsel.h for the data
 * layout and lifetime contract.
 */

#include "ops/rowsel.h"
#include "ops/ops.h"
#include "core/pool.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────
 * Allocation helpers
 * ────────────────────────────────────────────────────────────────── */

ray_t* ray_rowsel_new(int64_t nrows, int64_t total_pass, int64_t idx_count) {
    if (nrows < 0 || total_pass < 0 || total_pass > nrows ||
        idx_count < 0 || idx_count > total_pass) return NULL;

    size_t payload = ray_rowsel_payload_bytes(nrows, idx_count);
    ray_t* block = ray_alloc(payload);
    if (!block) return NULL;

    /* ray_alloc zeroes the 32-byte header but NOT the data area.
     * Initialize the inline meta header explicitly; arrays are filled
     * by the producer after this call. */
    ray_rowsel_t* m = ray_rowsel_meta(block);
    m->total_pass = total_pass;
    m->nrows      = nrows;
    m->n_segs     = (uint32_t)((nrows + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);
    if (nrows <= 0) m->n_segs = 0;
    m->_pad       = 0;

    return block;
}

void ray_rowsel_release(ray_t* block) {
    if (block) ray_release(block);
}

/* ──────────────────────────────────────────────────────────────────
 * Producer — parallel two-pass build from a RAY_BOOL pred vec
 * ────────────────────────────────────────────────────────────────── */

/* Pass 1 worker context.  Each worker owns a disjoint segment range
 * [start, end) and writes per-segment popcounts into popcount[]. */
typedef struct {
    const uint8_t* pred_data;
    int64_t        nrows;
    uint32_t*      popcount;     /* one entry per segment */
} rowsel_pass1_ctx_t;

static void rowsel_pass1_fn(void* vctx, uint32_t worker_id,
                            int64_t start_seg, int64_t end_seg) {
    (void)worker_id;
    rowsel_pass1_ctx_t* c = (rowsel_pass1_ctx_t*)vctx;
    const uint8_t* pred = c->pred_data;
    int64_t nrows = c->nrows;
    uint32_t* popcount = c->popcount;

    for (int64_t seg = start_seg; seg < end_seg; seg++) {
        int64_t base = seg * RAY_MORSEL_ELEMS;
        int64_t end  = base + RAY_MORSEL_ELEMS;
        if (end > nrows) end = nrows;
        uint32_t n = 0;
        for (int64_t r = base; r < end; r++)
            n += pred[r] != 0;
        popcount[seg] = n;
    }
}

/* Pass 2 worker context.  Each worker owns a disjoint segment range
 * and writes morsel-local indices into the (already-sized) idx[]
 * array.  Workers never overlap because each segment's slice
 * idx[seg_offsets[seg] .. seg_offsets[seg+1]) is exclusive. */
typedef struct {
    const uint8_t*  pred_data;
    int64_t         nrows;
    const uint8_t*  seg_flags;
    const uint32_t* seg_offsets;
    uint16_t*       idx;
} rowsel_pass2_ctx_t;

static void rowsel_pass2_fn(void* vctx, uint32_t worker_id,
                            int64_t start_seg, int64_t end_seg) {
    (void)worker_id;
    rowsel_pass2_ctx_t* c = (rowsel_pass2_ctx_t*)vctx;
    const uint8_t* pred = c->pred_data;
    int64_t nrows = c->nrows;

    for (int64_t seg = start_seg; seg < end_seg; seg++) {
        if (c->seg_flags[seg] != RAY_SEL_MIX) continue;  /* NONE / ALL: nothing to write */
        int64_t base = seg * RAY_MORSEL_ELEMS;
        int64_t end  = base + RAY_MORSEL_ELEMS;
        if (end > nrows) end = nrows;
        uint16_t* out = c->idx + c->seg_offsets[seg];
        uint32_t  out_n = 0;
        for (int64_t r = base; r < end; r++) {
            if (pred[r])
                out[out_n++] = (uint16_t)(r - base);
        }
        /* sanity: out_n must equal seg_offsets[seg+1] - seg_offsets[seg] */
    }
}

ray_t* ray_rowsel_from_pred(ray_t* pred) {
    if (!pred || pred->type != RAY_BOOL) return NULL;
    int64_t nrows = pred->len;
    if (nrows == 0) {
        /* Empty source — empty selection. */
        return ray_rowsel_new(0, 0, 0);
    }

    const uint8_t* pred_data = (const uint8_t*)ray_data(pred);
    uint32_t n_segs = (uint32_t)((nrows + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);

    /* Temporary popcount[seg] buffer.  ray_alloc returns a ray_t*
     * whose data area is the byte buffer we need. */
    ray_t* pop_block = ray_alloc((size_t)n_segs * sizeof(uint32_t));
    if (!pop_block) return NULL;
    uint32_t* popcount = (uint32_t*)ray_data(pop_block);

    /* Pass 1 — parallel popcount per segment. */
    rowsel_pass1_ctx_t p1 = {
        .pred_data = pred_data,
        .nrows     = nrows,
        .popcount  = popcount,
    };
    ray_pool_t* pool = ray_pool_get();
    if (pool && nrows >= RAY_PARALLEL_THRESHOLD)
        ray_pool_dispatch(pool, rowsel_pass1_fn, &p1, (int64_t)n_segs);
    else
        rowsel_pass1_fn(&p1, 0, 0, (int64_t)n_segs);

    /* Single sweep: classify each segment and accumulate both
     * total_pass (ALL + MIX rows, for meta) and idx_count (MIX rows
     * only, for sizing idx[]).  Walking popcount[] sequentially —
     * n_segs is at most ~10K for a 10M-row table, trivial. */
    int64_t total_pass = 0;
    int64_t idx_count  = 0;
    for (uint32_t s = 0; s < n_segs; s++) {
        int64_t seg_start = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t seg_end   = seg_start + RAY_MORSEL_ELEMS;
        if (seg_end > nrows) seg_end = nrows;
        int64_t seg_len = seg_end - seg_start;
        uint32_t pc = popcount[s];
        total_pass += pc;
        if (pc != 0 && (int64_t)pc != seg_len)
            idx_count += pc;
    }

    if (total_pass == nrows) {
        /* All rows pass — convention is "no selection". */
        ray_release(pop_block);
        return NULL;
    }

    /* Allocate the result block sized for the MIX-contributed
     * indices only.  ALL and NONE segments add nothing to idx[]. */
    ray_t* block = ray_rowsel_new(nrows, total_pass, idx_count);
    if (!block) {
        ray_release(pop_block);
        return NULL;
    }

    /* Fill seg_flags + seg_offsets in a second sequential walk over
     * popcount[].  cum accumulates MIX-contributed indices to build
     * the prefix sum into idx[]. */
    uint8_t*  seg_flags   = ray_rowsel_flags(block);
    uint32_t* seg_offsets = ray_rowsel_offsets(block);
    uint32_t cum = 0;
    for (uint32_t s = 0; s < n_segs; s++) {
        seg_offsets[s] = cum;
        int64_t seg_start = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t seg_end   = seg_start + RAY_MORSEL_ELEMS;
        if (seg_end > nrows) seg_end = nrows;
        int64_t seg_len = seg_end - seg_start;
        uint32_t pc = popcount[s];
        /* NONE/ALL/MIX classify — must stay byte-identical to the classify
         * in rowsel_emit_segment (rowsel.c ~line 278-291).  See
         * rowsel/emit_equiv test.  Edit both or neither. */
        if (pc == 0) {
            seg_flags[s] = RAY_SEL_NONE;
        } else if ((int64_t)pc == seg_len) {
            seg_flags[s] = RAY_SEL_ALL;
            /* ALL contributes nothing to idx[]; cum unchanged. */
        } else {
            seg_flags[s] = RAY_SEL_MIX;
            cum += pc;
        }
    }
    seg_offsets[n_segs] = cum;

    /* Pass 2 — parallel index write into idx[]. */
    if (cum > 0) {
        rowsel_pass2_ctx_t p2 = {
            .pred_data   = pred_data,
            .nrows       = nrows,
            .seg_flags   = seg_flags,
            .seg_offsets = seg_offsets,
            .idx         = ray_rowsel_idx(block),
        };
        if (pool && nrows >= RAY_PARALLEL_THRESHOLD)
            ray_pool_dispatch(pool, rowsel_pass2_fn, &p2, (int64_t)n_segs);
        else
            rowsel_pass2_fn(&p2, 0, 0, (int64_t)n_segs);
    }

    ray_release(pop_block);
    return block;
}

/* ──────────────────────────────────────────────────────────────────
 * Streaming builder — per-morsel selection emitter
 *
 * rowsel_emit_segment applies the exact same per-segment classify as
 * ray_rowsel_from_pred's sequential sweep (NONE if zero set, ALL if all
 * set with no idx stored, else MIX with morsel-local indices), so a
 * builder fed each morsel in order produces a block byte-identical to
 * the whole-vec path.  rowsel_builder_finish stitches per-worker
 * builders (segment ranges concatenated in global order) into the
 * standard ray_rowsel_new layout.
 * ────────────────────────────────────────────────────────────────── */

void rowsel_builder_init(rowsel_builder_t* b, uint32_t n_segs) {
    b->n_segs     = n_segs;
    /* ray_calloc_raw zeros the allocation; seg_flags must be zero-initialised
     * so that a task whose scratch alloc fails (expr_full_fn early-return) has
     * valid all-NONE state without ever calling rowsel_emit_segment.
     * RAY_SEL_NONE == 0 — verified (ops.h), so memset-0 is correct.
     * seg_off zeroed for the same reason: rowsel_builder_finish reads
     * seg_off[s+1]-seg_off[s] for MIX segments; garbage deltas → OOB memcpy. */
    b->seg_flags  = (uint8_t*)ray_calloc_raw(n_segs ? n_segs : 1);
    b->seg_off    = (uint32_t*)ray_calloc_raw((size_t)(n_segs + 1) * sizeof(uint32_t));
    b->idx_cap    = n_segs ? n_segs * 8u : 8u;
    b->idx        = (uint16_t*)ray_alloc_raw((size_t)b->idx_cap * sizeof(uint16_t));
    b->idx_len    = 0;
    b->total_pass = 0;
    /* All three allocs must succeed — raw allocator is fatal-by-contract
     * (no recovery path in Rayforce; same as ray_alloc failure callers). */
    assert(b->seg_flags && "rowsel_builder_init: OOM on seg_flags");
    assert(b->seg_off   && "rowsel_builder_init: OOM on seg_off");
    assert(b->idx       && "rowsel_builder_init: OOM on idx");
    /* seg_off[0] is already 0 from ray_calloc_raw; kept explicit for clarity. */
    b->seg_off[0] = 0;
}

void rowsel_emit_segment(rowsel_builder_t* b, uint32_t seg,
                         const uint8_t* morsel_bool, int64_t n) {
    /* seg is a BUILDER-LOCAL index — must be in [0, b->n_segs).
     * Passing a global segment id here would index past the allocated
     * seg_flags[]/seg_off[] arrays and silently corrupt the heap. */
    assert(seg < b->n_segs && "rowsel_emit_segment: seg is not builder-local");

    /* Mark this segment's start offset into the local idx[]. */
    b->seg_off[seg] = b->idx_len;

    uint32_t pc = 0;
    for (int64_t i = 0; i < n; i++) pc += morsel_bool[i] != 0;
    b->total_pass += pc;

    /* NONE/ALL/MIX classify — must stay byte-identical to the sweep in
     * ray_rowsel_from_pred (rowsel.c ~line 204-213).  See rowsel/emit_equiv
     * test.  Edit both or neither. */
    if (pc == 0) {
        b->seg_flags[seg] = RAY_SEL_NONE;
    } else if ((int64_t)pc == n) {
        b->seg_flags[seg] = RAY_SEL_ALL;   /* ALL contributes nothing to idx[] */
    } else {
        b->seg_flags[seg] = RAY_SEL_MIX;
        /* Grow idx[] geometrically to fit this segment's pc indices. */
        if (b->idx_len + pc > b->idx_cap) {
            uint32_t newcap = b->idx_cap ? b->idx_cap : 8u;
            while (newcap < b->idx_len + pc) newcap *= 2u;
            b->idx = (uint16_t*)ray_realloc_raw(b->idx, (size_t)newcap * sizeof(uint16_t));
            b->idx_cap = newcap;
        }
        for (int64_t i = 0; i < n; i++)
            if (morsel_bool[i]) b->idx[b->idx_len++] = (uint16_t)i;
    }

    b->seg_off[seg + 1] = b->idx_len;
}

ray_t* rowsel_builder_finish(rowsel_builder_t* builders, uint32_t n_workers,
                             int64_t nrows) {
    int64_t total_pass = 0;
    int64_t idx_count  = 0;
    for (uint32_t w = 0; w < n_workers; w++) {
        total_pass += builders[w].total_pass;
        idx_count  += builders[w].idx_len;
    }

    ray_t* block = ray_rowsel_new(nrows, total_pass, idx_count);
    if (block) {
        uint8_t*  seg_flags   = ray_rowsel_flags(block);
        uint32_t* seg_offsets = ray_rowsel_offsets(block);
        uint16_t* idx_out     = ray_rowsel_idx(block);
        uint32_t  n_segs      = ray_rowsel_meta(block)->n_segs;

        uint32_t gseg = 0;
        uint32_t cum  = 0;
        for (uint32_t w = 0; w < n_workers; w++) {
            rowsel_builder_t* b = &builders[w];
            for (uint32_t s = 0; s < b->n_segs; s++, gseg++) {
                /* Over-covering partition: more builder segs than the
                 * block has — violates the n_segs-sum invariant. */
                assert(gseg < n_segs &&
                       "rowsel_builder_finish: builders over-cover n_segs");
                seg_flags[gseg]   = b->seg_flags[s];
                seg_offsets[gseg] = cum;
                if (b->seg_flags[s] == RAY_SEL_MIX) {
                    uint32_t cnt = b->seg_off[s + 1] - b->seg_off[s];
                    memcpy(idx_out + cum, b->idx + b->seg_off[s],
                           (size_t)cnt * sizeof(uint16_t));
                    cum += cnt;
                }
            }
        }
        /* Under-covering partition: fewer builder segs than the block
         * has — the tail would be uninitialized garbage (ray_alloc does
         * NOT zero the data area).  Assert and zero defensively. */
        assert(gseg == n_segs &&
               "rowsel_builder_finish: builders under-cover n_segs");
        for (uint32_t s = gseg; s < n_segs; s++) {
            seg_flags[s]   = RAY_SEL_NONE;
            seg_offsets[s] = cum;
        }
        seg_offsets[n_segs] = cum;
    }

    /* Free the builders' scratch regardless of success. */
    for (uint32_t w = 0; w < n_workers; w++) {
        ray_free_raw(builders[w].seg_flags);
        ray_free_raw(builders[w].seg_off);
        ray_free_raw(builders[w].idx);
        builders[w].seg_flags = NULL;
        builders[w].seg_off   = NULL;
        builders[w].idx       = NULL;
    }

    return block;
}

/* ──────────────────────────────────────────────────────────────────
 * ray_rowsel_to_indices — flatten to a dense int64 array
 * ────────────────────────────────────────────────────────────────── */

/* Pass 2 worker context for ray_rowsel_to_indices. */
typedef struct {
    const uint8_t*  flags;
    const uint32_t* offsets;
    const uint16_t* idx;
    const uint32_t* flat_offsets;  /* per-segment offset into out[] */
    int64_t*        out;
    int64_t         nrows;
} rowsel_to_idx_ctx_t;

static void rowsel_to_idx_fn(void* vctx, uint32_t worker_id,
                             int64_t start_seg, int64_t end_seg) {
    (void)worker_id;
    rowsel_to_idx_ctx_t* c = (rowsel_to_idx_ctx_t*)vctx;
    int64_t nrows = c->nrows;
    for (int64_t seg = start_seg; seg < end_seg; seg++) {
        uint8_t f = c->flags[seg];
        if (f == RAY_SEL_NONE) continue;
        int64_t base = seg * RAY_MORSEL_ELEMS;
        int64_t end  = base + RAY_MORSEL_ELEMS;
        if (end > nrows) end = nrows;
        int64_t j = c->flat_offsets[seg];
        if (f == RAY_SEL_ALL) {
            for (int64_t r = base; r < end; r++) c->out[j++] = r;
        } else {
            const uint16_t* slice = c->idx + c->offsets[seg];
            uint32_t n = c->offsets[seg + 1] - c->offsets[seg];
            for (uint32_t i = 0; i < n; i++) c->out[j++] = base + slice[i];
        }
    }
}

ray_t* ray_rowsel_to_indices(ray_t* sel) {
    if (!sel) return NULL;
    ray_rowsel_t*   m       = ray_rowsel_meta(sel);
    const uint8_t*  flags   = ray_rowsel_flags(sel);
    const uint32_t* offsets = ray_rowsel_offsets(sel);
    const uint16_t* idx     = ray_rowsel_idx(sel);
    int64_t nrows      = m->nrows;
    int64_t total_pass = m->total_pass;
    uint32_t n_segs    = m->n_segs;

    ray_t* block = ray_alloc((size_t)total_pass * sizeof(int64_t));
    if (!block) return NULL;
    int64_t* out = (int64_t*)ray_data(block);

    if (total_pass == 0 || n_segs == 0) return block;

    /* Build per-segment flat offsets into out[].  Sequential prefix
     * sum over n_segs entries — cheap (n_segs ≈ nrows/1024). */
    ray_t* fo_block = ray_alloc((size_t)n_segs * sizeof(uint32_t));
    if (!fo_block) { ray_release(block); return NULL; }
    uint32_t* flat_offsets = (uint32_t*)ray_data(fo_block);
    uint32_t cum = 0;
    for (uint32_t s = 0; s < n_segs; s++) {
        flat_offsets[s] = cum;
        uint8_t f = flags[s];
        if (f == RAY_SEL_NONE) continue;
        if (f == RAY_SEL_ALL) {
            int64_t base = (int64_t)s * RAY_MORSEL_ELEMS;
            int64_t end  = base + RAY_MORSEL_ELEMS;
            if (end > nrows) end = nrows;
            cum += (uint32_t)(end - base);
        } else {
            cum += offsets[s + 1] - offsets[s];
        }
    }

    /* Parallel write: each worker fills its own segment range into
     * out[] using flat_offsets to find the start of each segment.
     * Slices are non-overlapping by construction. */
    rowsel_to_idx_ctx_t ctx = {
        .flags        = flags,
        .offsets      = offsets,
        .idx          = idx,
        .flat_offsets = flat_offsets,
        .out          = out,
        .nrows        = nrows,
    };
    ray_pool_t* pool = ray_pool_get();
    if (pool && nrows >= RAY_PARALLEL_THRESHOLD)
        ray_pool_dispatch(pool, rowsel_to_idx_fn, &ctx, (int64_t)n_segs);
    else
        rowsel_to_idx_fn(&ctx, 0, 0, (int64_t)n_segs);

    ray_release(fo_block);
    return block;
}

/* ──────────────────────────────────────────────────────────────────
 * Refine — chained filter
 * ────────────────────────────────────────────────────────────────── */

/* refine: walk `existing`'s surviving rows, test pred at each, emit a
 * new selection.  Sequential — chained filters are typically applied
 * to already-shrunk row sets where parallelism doesn't pay back the
 * dispatch overhead.  Pass 2 will revisit if measurement says
 * otherwise. */
ray_t* ray_rowsel_refine(ray_t* existing, ray_t* pred) {
    if (!existing) return ray_rowsel_from_pred(pred);
    if (!pred || pred->type != RAY_BOOL) return NULL;

    ray_rowsel_t*  em = ray_rowsel_meta(existing);
    int64_t        nrows = em->nrows;
    if (pred->len != nrows) return NULL;

    const uint8_t*  pred_data    = (const uint8_t*)ray_data(pred);
    const uint8_t*  e_flags      = ray_rowsel_flags(existing);
    const uint32_t* e_offsets    = ray_rowsel_offsets(existing);
    const uint16_t* e_idx        = ray_rowsel_idx(existing);
    uint32_t        n_segs       = em->n_segs;

    /* Pass 1 — count survivors per segment. */
    ray_t* pop_block = ray_alloc((size_t)n_segs * sizeof(uint32_t));
    if (!pop_block) return NULL;
    uint32_t* popcount = (uint32_t*)ray_data(pop_block);
    memset(popcount, 0, (size_t)n_segs * sizeof(uint32_t));

    int64_t total_pass = 0;
    int64_t idx_count  = 0;
    for (uint32_t s = 0; s < n_segs; s++) {
        uint8_t f = e_flags[s];
        if (f == RAY_SEL_NONE) continue;
        int64_t base = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t end  = base + RAY_MORSEL_ELEMS;
        if (end > nrows) end = nrows;
        int64_t seg_len = end - base;
        uint32_t n = 0;
        if (f == RAY_SEL_ALL) {
            for (int64_t r = base; r < end; r++)
                n += pred_data[r] != 0;
        } else { /* MIX */
            const uint16_t* src = e_idx + e_offsets[s];
            uint32_t src_n = e_offsets[s + 1] - e_offsets[s];
            for (uint32_t i = 0; i < src_n; i++) {
                int64_t r = base + src[i];
                n += pred_data[r] != 0;
            }
        }
        popcount[s] = n;
        total_pass += n;
        /* This segment will be MIX in the output (and contribute to
         * idx[]) iff some-but-not-all of its rows pass. */
        if (n != 0 && (int64_t)n != seg_len)
            idx_count += n;
    }

    if (total_pass == nrows) {
        /* Refinement somehow ended up matching every source row.
         * Should be impossible unless `existing` was already
         * effectively all-pass and pred is all-true — but handle it. */
        ray_release(pop_block);
        return NULL;
    }

    ray_t* block = ray_rowsel_new(nrows, total_pass, idx_count);
    if (!block) {
        ray_release(pop_block);
        return NULL;
    }
    uint8_t*  seg_flags   = ray_rowsel_flags(block);
    uint32_t* seg_offsets = ray_rowsel_offsets(block);
    uint16_t* idx_out     = ray_rowsel_idx(block);

    uint32_t cum = 0;
    for (uint32_t s = 0; s < n_segs; s++) {
        seg_offsets[s] = cum;
        int64_t base = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t end  = base + RAY_MORSEL_ELEMS;
        if (end > nrows) end = nrows;
        int64_t seg_len = end - base;
        uint32_t pc = popcount[s];
        if (pc == 0) {
            seg_flags[s] = RAY_SEL_NONE;
            continue;
        }
        if ((int64_t)pc == seg_len) {
            seg_flags[s] = RAY_SEL_ALL;
            continue;
        }
        seg_flags[s] = RAY_SEL_MIX;

        /* Pass 2 (inlined, sequential) — write the surviving
         * morsel-local indices for this segment. */
        uint16_t* dst = idx_out + cum;
        uint32_t  dn  = 0;
        uint8_t f = e_flags[s];
        if (f == RAY_SEL_ALL) {
            for (int64_t r = base; r < end; r++)
                if (pred_data[r])
                    dst[dn++] = (uint16_t)(r - base);
        } else { /* MIX in existing */
            const uint16_t* src = e_idx + e_offsets[s];
            uint32_t src_n = e_offsets[s + 1] - e_offsets[s];
            for (uint32_t i = 0; i < src_n; i++) {
                int64_t r = base + src[i];
                if (pred_data[r])
                    dst[dn++] = (uint16_t)(r - base);
            }
        }
        cum += pc;
    }
    seg_offsets[n_segs] = cum;

    ray_release(pop_block);
    return block;
}

ray_t* ray_rowsel_intersect(ray_t* a, ray_t* b) {
    if (!a || !b) return NULL;
    ray_rowsel_t* am = ray_rowsel_meta(a);
    ray_rowsel_t* bm = ray_rowsel_meta(b);
    if (am->nrows != bm->nrows || am->n_segs != bm->n_segs) return NULL;

    uint32_t ns = am->n_segs;
    const uint8_t* af = ray_rowsel_flags(a);
    const uint8_t* bf = ray_rowsel_flags(b);
    const uint32_t* ao = ray_rowsel_offsets(a);
    const uint32_t* bo = ray_rowsel_offsets(b);
    const uint16_t* ai = ray_rowsel_idx(a);
    const uint16_t* bi = ray_rowsel_idx(b);
    int64_t total = 0;
    int64_t idx_count = 0;

    /* Count the intersection per segment.  MIX arrays are sorted by every
     * producer, so MIX∩MIX is a linear two-cursor walk. */
    for (uint32_t s = 0; s < ns; s++) {
        int64_t ss = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t se = ss + RAY_MORSEL_ELEMS;
        if (se > am->nrows) se = am->nrows;
        int64_t seg_len = se - ss;
        int64_t pc = 0;
        if (af[s] == RAY_SEL_NONE || bf[s] == RAY_SEL_NONE) {
            pc = 0;
        } else if (af[s] == RAY_SEL_ALL && bf[s] == RAY_SEL_ALL) {
            pc = seg_len;
        } else if (af[s] == RAY_SEL_ALL) {
            pc = bo[s + 1] - bo[s];
        } else if (bf[s] == RAY_SEL_ALL) {
            pc = ao[s + 1] - ao[s];
        } else {
            uint32_t ap = ao[s], ae = ao[s + 1];
            uint32_t bp = bo[s], be = bo[s + 1];
            while (ap < ae && bp < be) {
                uint16_t av = ai[ap], bv = bi[bp];
                if (av < bv) ap++;
                else if (bv < av) bp++;
                else { pc++; ap++; bp++; }
            }
        }
        total += pc;
        if (pc > 0 && pc != seg_len) idx_count += pc;
    }

    ray_t* out = ray_rowsel_new(am->nrows, total, idx_count);
    if (!out) return NULL;
    uint8_t* of = ray_rowsel_flags(out);
    uint32_t* oo = ray_rowsel_offsets(out);
    uint16_t* oi = ray_rowsel_idx(out);
    uint32_t cum = 0;

    for (uint32_t s = 0; s < ns; s++) {
        oo[s] = cum;
        int64_t ss = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t se = ss + RAY_MORSEL_ELEMS;
        if (se > am->nrows) se = am->nrows;
        int64_t seg_len = se - ss;

        if (af[s] == RAY_SEL_NONE || bf[s] == RAY_SEL_NONE) {
            of[s] = RAY_SEL_NONE;
        } else if (af[s] == RAY_SEL_ALL && bf[s] == RAY_SEL_ALL) {
            of[s] = RAY_SEL_ALL;
        } else {
            const uint16_t* src = NULL;
            uint32_t begin = 0, end = 0;
            if (af[s] == RAY_SEL_ALL) {
                src = bi; begin = bo[s]; end = bo[s + 1];
            } else if (bf[s] == RAY_SEL_ALL) {
                src = ai; begin = ao[s]; end = ao[s + 1];
            }

            if (src) {
                uint32_t pc = end - begin;
                if ((int64_t)pc == seg_len) {
                    of[s] = RAY_SEL_ALL;
                } else {
                    for (uint32_t p = begin; p < end; p++) oi[cum++] = src[p];
                    of[s] = pc == 0 ? RAY_SEL_NONE : RAY_SEL_MIX;
                }
            } else {
                uint32_t ap = ao[s], ae = ao[s + 1];
                uint32_t bp = bo[s], be = bo[s + 1];
                uint32_t count = 0;
                uint32_t cp = ap, dp = bp;
                while (cp < ae && dp < be) {
                    uint16_t av = ai[cp], bv = bi[dp];
                    if (av < bv) cp++;
                    else if (bv < av) dp++;
                    else { count++; cp++; dp++; }
                }
                if ((int64_t)count == seg_len) {
                    of[s] = RAY_SEL_ALL;
                    continue;
                }
                while (ap < ae && bp < be) {
                    uint16_t av = ai[ap], bv = bi[bp];
                    if (av < bv) ap++;
                    else if (bv < av) bp++;
                    else { oi[cum++] = av; ap++; bp++; }
                }
            }
            if (of[s] != RAY_SEL_ALL)
                of[s] = cum == oo[s] ? RAY_SEL_NONE : RAY_SEL_MIX;
        }
    }
    oo[ns] = cum;
    return out;
}
