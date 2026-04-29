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

#include <rayforce.h>
#include "ops/ops.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Layout size computation
 *
 * Data payload after 32-byte ray_t header:
 *   ray_sel_meta_t          16 bytes
 *   seg_flags[n_segs]      align8(n_segs) bytes
 *   seg_popcnt[n_segs]     align8(n_segs * 2) bytes
 *   bits[n_words]          n_words * 8 bytes
 * -------------------------------------------------------------------------- */

static size_t sel_data_size(int64_t nrows) {
    uint32_t n_segs = (uint32_t)((nrows + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);
    uint32_t n_words = (uint32_t)((nrows + 63) / 64);

    size_t sz = sizeof(ray_sel_meta_t);
    sz += (n_segs + 7u) & ~(size_t)7;           /* seg_flags, 8-aligned */
    sz += ((size_t)n_segs * 2 + 7u) & ~(size_t)7; /* seg_popcnt, 8-aligned */
    sz += (size_t)n_words * 8;                   /* bits */
    return sz;
}

/* --------------------------------------------------------------------------
 * ray_sel_new — allocate a selection with all bits zero (no rows pass)
 * -------------------------------------------------------------------------- */

ray_t* ray_sel_new(int64_t nrows) {
    if (nrows < 0) return ray_error("range", NULL);

    size_t dsz = sel_data_size(nrows);
    ray_t* s = ray_alloc(dsz);
    if (!s || RAY_IS_ERR(s)) return s;

    s->type = RAY_SEL;
    s->len  = nrows;
    memset(ray_data(s), 0, dsz);

    ray_sel_meta_t* m = ray_sel_meta(s);
    m->total_pass = 0;
    m->n_segs = (uint32_t)((nrows + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);
    /* seg_flags[] already zero = RAY_SEL_NONE, seg_popcnt[] = 0, bits[] = 0 */

    return s;
}

/* --------------------------------------------------------------------------
 * ray_sel_recompute — rebuild seg_flags + seg_popcnt from bits[]
 *
 * Called after direct writes into bits[] (e.g., fused predicate evaluation).
 * -------------------------------------------------------------------------- */

void ray_sel_recompute(ray_t* sel) {
    if (!sel || sel->type != RAY_SEL) return;

    ray_sel_meta_t* m = ray_sel_meta(sel);
    uint8_t*  flags  = ray_sel_flags(sel);
    uint16_t* pcnt   = ray_sel_popcnt(sel);
    uint64_t* bits   = ray_sel_bits(sel);

    int64_t total = 0;
    int64_t nrows = sel->len;
    uint32_t n_segs = m->n_segs;

    for (uint32_t seg = 0; seg < n_segs; seg++) {
        int64_t seg_start = (int64_t)seg * RAY_MORSEL_ELEMS;
        int64_t seg_rows  = nrows - seg_start;
        if (seg_rows > RAY_MORSEL_ELEMS) seg_rows = RAY_MORSEL_ELEMS;

        /* Count bits in this segment's words */
        uint32_t word_start = (uint32_t)(seg_start / 64);
        uint32_t word_end   = (uint32_t)((seg_start + seg_rows + 63) / 64);
        int64_t seg_pop = 0;
        for (uint32_t w = word_start; w < word_end; w++)
            seg_pop += __builtin_popcountll(bits[w]);

        /* Handle partial last word: mask out trailing bits beyond nrows */
        if (seg == n_segs - 1 && (nrows & 63)) {
            uint32_t last_w = word_end - 1;
            uint32_t valid_bits = (uint32_t)(nrows & 63);
            uint64_t trail_mask = (1ULL << valid_bits) - 1;
            /* Subtract overcounted trailing bits */
            seg_pop -= __builtin_popcountll(bits[last_w] & ~trail_mask);
        }

        pcnt[seg] = (uint16_t)seg_pop;
        total += seg_pop;

        if (seg_pop == 0)
            flags[seg] = RAY_SEL_NONE;
        else if (seg_pop == seg_rows)
            flags[seg] = RAY_SEL_ALL;
        else
            flags[seg] = RAY_SEL_MIX;
    }

    m->total_pass = total;
}

/* --------------------------------------------------------------------------
 * ray_sel_from_pred — convert a RAY_BOOL byte-per-row vector to RAY_SEL
 * -------------------------------------------------------------------------- */

ray_t* ray_sel_from_pred(ray_t* pred) {
    if (!pred || RAY_IS_ERR(pred)) return pred;
    if (pred->type != RAY_BOOL) return ray_error("type", NULL);

    int64_t nrows = pred->len;
    ray_t* sel = ray_sel_new(nrows);
    if (!sel || RAY_IS_ERR(sel)) return sel;

    /* Pack byte-per-row into bitpacked uint64_t words */
    uint64_t* bits = ray_sel_bits(sel);
    const uint8_t* src = (const uint8_t*)ray_data(pred);

    int64_t full_words = nrows / 64;
    for (int64_t w = 0; w < full_words; w++) {
        uint64_t word = 0;
        const uint8_t* p = src + w * 64;
        for (int b = 0; b < 64; b++)
            word |= (uint64_t)(p[b] != 0) << b;
        bits[w] = word;
    }

    /* Remainder bits */
    int64_t rem = nrows & 63;
    if (rem) {
        uint64_t word = 0;
        const uint8_t* p = src + full_words * 64;
        for (int64_t b = 0; b < rem; b++)
            word |= (uint64_t)(p[b] != 0) << b;
        bits[full_words] = word;
    }

    ray_sel_recompute(sel);
    return sel;
}

/* --------------------------------------------------------------------------
 * ray_sel_and — AND two selections of equal length, returns new RAY_SEL
 * -------------------------------------------------------------------------- */

ray_t* ray_sel_and(ray_t* a, ray_t* b) {
    if (!a || RAY_IS_ERR(a)) return a;
    if (!b || RAY_IS_ERR(b)) return b;
    if (a->type != RAY_SEL || b->type != RAY_SEL)
        return ray_error("type", NULL);
    if (a->len != b->len)
        return ray_error("range", NULL);

    int64_t nrows = a->len;
    ray_t* out = ray_sel_new(nrows);
    if (!out || RAY_IS_ERR(out)) return out;

    uint64_t* dst = ray_sel_bits(out);
    const uint64_t* sa = ray_sel_bits(a);
    const uint64_t* sb = ray_sel_bits(b);
    uint32_t n_words = (uint32_t)((nrows + 63) / 64);

    for (uint32_t w = 0; w < n_words; w++)
        dst[w] = sa[w] & sb[w];

    ray_sel_recompute(out);
    return out;
}
