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

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "mem/heap.h"   /* BSIZEOF for block-size assertion */
#include "ops/ops.h"
#include "ops/rowsel.h"
#include <string.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────
 * Helpers
 * ────────────────────────────────────────────────────────────────── */

/* Build a RAY_BOOL vec from a literal byte array. */
static ray_t* make_pred(const uint8_t* bytes, int64_t n) {
    ray_t* v = ray_vec_new(RAY_BOOL, n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = n;
    memcpy(ray_data(v), bytes, (size_t)n);
    return v;
}

/* Naive popcount over the raw bool vec — used to cross-check the
 * per-segment popcounts the producer encodes via seg_offsets. */
static int64_t naive_popcount(const uint8_t* p, int64_t n) {
    int64_t c = 0;
    for (int64_t i = 0; i < n; i++) c += p[i] != 0;
    return c;
}

/* Walk a rowsel block and reconstruct the global row indices it
 * encodes (ALL segments expand to dense ranges, MIX uses idx[],
 * NONE skipped).  Used by tests to compare against an oracle. */
static int64_t reconstruct(ray_t* block, int64_t* out) {
    if (!block) return -1;
    ray_rowsel_t*   m       = ray_rowsel_meta(block);
    const uint8_t*  flags   = ray_rowsel_flags(block);
    const uint32_t* offsets = ray_rowsel_offsets(block);
    const uint16_t* idx     = ray_rowsel_idx(block);
    int64_t out_n = 0;
    for (uint32_t s = 0; s < m->n_segs; s++) {
        int64_t base = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t end  = base + RAY_MORSEL_ELEMS;
        if (end > m->nrows) end = m->nrows;
        if (flags[s] == RAY_SEL_NONE) continue;
        if (flags[s] == RAY_SEL_ALL) {
            for (int64_t r = base; r < end; r++) out[out_n++] = r;
            continue;
        }
        const uint16_t* slice = idx + offsets[s];
        uint32_t n = offsets[s + 1] - offsets[s];
        for (uint32_t i = 0; i < n; i++) out[out_n++] = base + slice[i];
    }
    return out_n;
}

/* ──────────────────────────────────────────────────────────────────
 * Tests
 * ────────────────────────────────────────────────────────────────── */

/* Empty input pred — returns an empty selection (n_segs == 0). */
static test_result_t test_rowsel_empty(void) {
    ray_heap_init();
    ray_t* pred = make_pred((const uint8_t*)"", 0);
    TEST_ASSERT_NOT_NULL(pred);
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);
    ray_rowsel_t* m = ray_rowsel_meta(sel);
    TEST_ASSERT_EQ_I(m->total_pass, 0);
    TEST_ASSERT_EQ_I(m->nrows, 0);
    TEST_ASSERT_EQ_I(m->n_segs, 0);
    ray_rowsel_release(sel);
    ray_release(pred);
    ray_heap_destroy();
    PASS();
}

/* All-true pred — convention: returns NULL meaning "all rows pass". */
static test_result_t test_rowsel_all_pass(void) {
    ray_heap_init();
    uint8_t bytes[100];
    memset(bytes, 1, sizeof(bytes));
    ray_t* pred = make_pred(bytes, 100);
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NULL(sel);  /* all-pass → NULL */
    ray_release(pred);
    ray_heap_destroy();
    PASS();
}

/* All-false pred — empty selection, all flags NONE. */
static test_result_t test_rowsel_none_pass(void) {
    ray_heap_init();
    uint8_t bytes[100] = {0};
    ray_t* pred = make_pred(bytes, 100);
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);
    ray_rowsel_t* m = ray_rowsel_meta(sel);
    TEST_ASSERT_EQ_I(m->total_pass, 0);
    TEST_ASSERT_EQ_I(m->nrows, 100);
    TEST_ASSERT_EQ_I(m->n_segs, 1);
    TEST_ASSERT_EQ_I(ray_rowsel_flags(sel)[0], RAY_SEL_NONE);
    ray_rowsel_release(sel);
    ray_release(pred);
    ray_heap_destroy();
    PASS();
}

/* Single morsel, mixed pred — verify reconstruction. */
static test_result_t test_rowsel_single_morsel_mixed(void) {
    ray_heap_init();
    /* 10 rows: positions 1, 3, 4, 7 set */
    uint8_t bytes[10] = {0,1,0,1,1,0,0,1,0,0};
    ray_t* pred = make_pred(bytes, 10);
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);
    ray_rowsel_t* m = ray_rowsel_meta(sel);
    TEST_ASSERT_EQ_I(m->total_pass, 4);
    TEST_ASSERT_EQ_I(m->n_segs, 1);
    TEST_ASSERT_EQ_I(ray_rowsel_flags(sel)[0], RAY_SEL_MIX);
    int64_t out[10];
    int64_t n = reconstruct(sel, out);
    TEST_ASSERT_EQ_I(n, 4);
    TEST_ASSERT_EQ_I(out[0], 1);
    TEST_ASSERT_EQ_I(out[1], 3);
    TEST_ASSERT_EQ_I(out[2], 4);
    TEST_ASSERT_EQ_I(out[3], 7);
    ray_rowsel_release(sel);
    ray_release(pred);
    ray_heap_destroy();
    PASS();
}

/* Multi-morsel pred with one ALL segment, one NONE segment, one MIX
 * segment.  Forces the producer to dispatch all three flag paths. */
static test_result_t test_rowsel_multi_morsel(void) {
    ray_heap_init();
    int64_t nrows = 3 * RAY_MORSEL_ELEMS;
    uint8_t* bytes = (uint8_t*)ray_data(ray_alloc((size_t)nrows));
    TEST_ASSERT_NOT_NULL(bytes);
    /* Seg 0: all true (ALL), Seg 1: all false (NONE),
     * Seg 2: every other row (MIX, 512 passing). */
    for (int64_t i = 0; i < RAY_MORSEL_ELEMS; i++) bytes[i] = 1;
    for (int64_t i = RAY_MORSEL_ELEMS; i < 2 * RAY_MORSEL_ELEMS; i++) bytes[i] = 0;
    for (int64_t i = 0; i < RAY_MORSEL_ELEMS; i++)
        bytes[2 * RAY_MORSEL_ELEMS + i] = (uint8_t)(i & 1);
    ray_t* pred = make_pred(bytes, nrows);
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);

    const uint8_t* flags = ray_rowsel_flags(sel);
    TEST_ASSERT_EQ_I(flags[0], RAY_SEL_ALL);
    TEST_ASSERT_EQ_I(flags[1], RAY_SEL_NONE);
    TEST_ASSERT_EQ_I(flags[2], RAY_SEL_MIX);

    ray_rowsel_t* m = ray_rowsel_meta(sel);
    TEST_ASSERT_EQ_I(m->n_segs, 3);
    TEST_ASSERT_EQ_I(m->total_pass, RAY_MORSEL_ELEMS + RAY_MORSEL_ELEMS / 2);

    /* Reconstruct and compare to oracle. */
    int64_t* oracle = (int64_t*)ray_data(ray_alloc((size_t)m->total_pass * sizeof(int64_t)));
    int64_t  oracle_n = 0;
    for (int64_t i = 0; i < nrows; i++) if (bytes[i]) oracle[oracle_n++] = i;
    TEST_ASSERT_EQ_I(oracle_n, m->total_pass);

    int64_t* recon = (int64_t*)ray_data(ray_alloc((size_t)m->total_pass * sizeof(int64_t)));
    int64_t  recon_n = reconstruct(sel, recon);
    TEST_ASSERT_EQ_I(recon_n, m->total_pass);
    for (int64_t i = 0; i < recon_n; i++)
        TEST_ASSERT_EQ_I(recon[i], oracle[i]);

    ray_rowsel_release(sel);
    ray_release(pred);
    ray_heap_destroy();
    PASS();
}

/* Last morsel is partial (nrows not a multiple of RAY_MORSEL_ELEMS).
 * The "ALL" determination uses the morsel's actual length, not 1024. */
static test_result_t test_rowsel_partial_last_morsel(void) {
    ray_heap_init();
    int64_t nrows = RAY_MORSEL_ELEMS + 5;  /* one full morsel + 5 rows */
    uint8_t* bytes = (uint8_t*)ray_data(ray_alloc((size_t)nrows));
    /* Seg 0: alternating (MIX, 512 passing).
     * Seg 1: 5 rows, all true → ALL. */
    for (int64_t i = 0; i < RAY_MORSEL_ELEMS; i++) bytes[i] = (uint8_t)(i & 1);
    for (int64_t i = 0; i < 5; i++) bytes[RAY_MORSEL_ELEMS + i] = 1;
    ray_t* pred = make_pred(bytes, nrows);
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);

    const uint8_t* flags = ray_rowsel_flags(sel);
    TEST_ASSERT_EQ_I(flags[0], RAY_SEL_MIX);
    TEST_ASSERT_EQ_I(flags[1], RAY_SEL_ALL);
    TEST_ASSERT_EQ_I(ray_rowsel_meta(sel)->total_pass, RAY_MORSEL_ELEMS / 2 + 5);
    TEST_ASSERT_EQ_I(ray_rowsel_meta(sel)->n_segs, 2);
    ray_rowsel_release(sel);
    ray_release(pred);
    ray_heap_destroy();
    PASS();
}

/* Producer over a range that crosses the parallel threshold so the
 * pool dispatch fires.  Cross-checks reconstruction against the
 * naive oracle. */
static test_result_t test_rowsel_parallel(void) {
    ray_heap_init();
    int64_t nrows = RAY_PARALLEL_THRESHOLD * 2 + 173;  /* parallel path + odd remainder */
    uint8_t* bytes = (uint8_t*)ray_data(ray_alloc((size_t)nrows));
    for (int64_t i = 0; i < nrows; i++) bytes[i] = (i % 7 == 0);
    ray_t* pred = make_pred(bytes, nrows);
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);

    int64_t expected_pass = naive_popcount(bytes, nrows);
    TEST_ASSERT_EQ_I(ray_rowsel_meta(sel)->total_pass, expected_pass);

    int64_t* recon = (int64_t*)ray_data(ray_alloc((size_t)expected_pass * sizeof(int64_t)));
    int64_t  recon_n = reconstruct(sel, recon);
    TEST_ASSERT_EQ_I(recon_n, expected_pass);
    int64_t check = 0;
    for (int64_t i = 0; i < nrows; i++) {
        if (bytes[i]) {
            TEST_ASSERT_EQ_I(recon[check], i);
            check++;
        }
    }
    TEST_ASSERT_EQ_I(check, expected_pass);
    ray_rowsel_release(sel);
    ray_release(pred);
    ray_heap_destroy();
    PASS();
}

/* Many ALL segments + a few MIX — verifies that idx[] is sized for
 * MIX-contributed entries only, not total_pass.  Without this fix the
 * producer over-allocates idx[] to ~total_pass uint16s, which on a
 * 10M-row 99%-selective filter wastes ~20 MB.
 *
 * Test shape: 4 morsels, segments 0..2 are all-true (ALL, contribute
 * 0 to idx[]), segment 3 has 7 mixed bits.  total_pass should be
 * 3*1024 + 7 = 3079 but the underlying allocation should size idx[]
 * for only 7 entries. */
static test_result_t test_rowsel_all_segments_compact(void) {
    ray_heap_init();
    int64_t nrows = 4 * RAY_MORSEL_ELEMS;
    ray_t* buf = ray_alloc((size_t)nrows);
    uint8_t* bytes = (uint8_t*)ray_data(buf);
    memset(bytes, 1, 3 * RAY_MORSEL_ELEMS);
    /* Segment 3: only 7 set bits, in a partial pattern. */
    memset(bytes + 3 * RAY_MORSEL_ELEMS, 0, RAY_MORSEL_ELEMS);
    int positions[] = {2, 5, 100, 333, 700, 900, 1023};
    for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++)
        bytes[3 * RAY_MORSEL_ELEMS + positions[i]] = 1;

    ray_t* pred = make_pred(bytes, nrows);
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);

    ray_rowsel_t* m = ray_rowsel_meta(sel);
    TEST_ASSERT_EQ_I(m->total_pass, 3 * RAY_MORSEL_ELEMS + 7);
    TEST_ASSERT_EQ_I(m->n_segs, 4);

    const uint8_t* flags = ray_rowsel_flags(sel);
    TEST_ASSERT_EQ_I(flags[0], RAY_SEL_ALL);
    TEST_ASSERT_EQ_I(flags[1], RAY_SEL_ALL);
    TEST_ASSERT_EQ_I(flags[2], RAY_SEL_ALL);
    TEST_ASSERT_EQ_I(flags[3], RAY_SEL_MIX);

    /* seg_offsets[n_segs] must equal 7 — the actual idx[]
     * occupancy.  This part is correct in BOTH the buggy and
     * fixed code (offsets are computed by walking popcounts) so
     * it's a sanity check, not the regression assertion. */
    const uint32_t* offsets = ray_rowsel_offsets(sel);
    TEST_ASSERT_EQ_I(offsets[0], 0);
    TEST_ASSERT_EQ_I(offsets[1], 0);
    TEST_ASSERT_EQ_I(offsets[2], 0);
    TEST_ASSERT_EQ_I(offsets[3], 0);
    TEST_ASSERT_EQ_I(offsets[4], 7);

    /* Real regression assertion: the underlying ray_alloc block
     * size must reflect idx_count=7, not idx_count=total_pass.
     *
     * Fixed payload   = sizeof(meta)24 + pad8(4)8 + (4+1)*4 + 7*2     = 66 B
     * Buggy payload   = sizeof(meta)24 + pad8(4)8 + (4+1)*4 + 3079*2  = 6210 B
     *
     * Buddy allocator rounds to the next power-of-two order, so
     * the fixed block is order 7 (128 B) and the buggy block is
     * order 13 (8 KB).  Assert the block's actual size is less
     * than the buggy expectation. */
    size_t fixed_payload = ray_rowsel_payload_bytes(nrows, 7);
    size_t buggy_payload = ray_rowsel_payload_bytes(nrows, m->total_pass);
    TEST_ASSERT((fixed_payload) < (buggy_payload), "fixed_payload < buggy_payload");
    size_t actual_block = BSIZEOF(sel->order);
    TEST_ASSERT((actual_block) < (buggy_payload), "actual_block < buggy_payload");

    /* Reconstruct: 3072 dense rows from segments 0..2, plus 7
     * indexed rows from segment 3. */
    int64_t* recon = (int64_t*)ray_data(ray_alloc((size_t)m->total_pass * sizeof(int64_t)));
    int64_t recon_n = reconstruct(sel, recon);
    TEST_ASSERT_EQ_I(recon_n, m->total_pass);
    /* Spot-check the segment-3 indices land in the right spots. */
    int64_t base = 3 * RAY_MORSEL_ELEMS;
    TEST_ASSERT_EQ_I(recon[m->total_pass - 7], base + 2);
    TEST_ASSERT_EQ_I(recon[m->total_pass - 1], base + 1023);

    ray_rowsel_release(sel);
    ray_release(pred);
    ray_release(buf);
    ray_heap_destroy();
    PASS();
}

/* Refine: existing rowsel ANDed with a second pred shrinks correctly. */
static test_result_t test_rowsel_refine(void) {
    ray_heap_init();
    int64_t nrows = 100;
    uint8_t a[100], b[100];
    for (int64_t i = 0; i < nrows; i++) {
        a[i] = (i % 2 == 0);   /* even */
        b[i] = (i % 3 == 0);   /* mult-of-3 */
    }
    ray_t* pa = make_pred(a, nrows);
    ray_t* pb = make_pred(b, nrows);

    ray_t* s1 = ray_rowsel_from_pred(pa);
    TEST_ASSERT_NOT_NULL(s1);
    ray_t* s2 = ray_rowsel_refine(s1, pb);
    TEST_ASSERT_NOT_NULL(s2);

    /* Expected survivors: even AND multiple of 3 → 0, 6, 12, …, 96 → 17 rows. */
    int64_t expect = 0;
    for (int64_t i = 0; i < nrows; i++) if (a[i] && b[i]) expect++;
    TEST_ASSERT_EQ_I(ray_rowsel_meta(s2)->total_pass, expect);

    int64_t recon[100];
    int64_t recon_n = reconstruct(s2, recon);
    TEST_ASSERT_EQ_I(recon_n, expect);
    int64_t check = 0;
    for (int64_t i = 0; i < nrows; i++) {
        if (a[i] && b[i]) {
            TEST_ASSERT_EQ_I(recon[check], i);
            check++;
        }
    }
    ray_rowsel_release(s2);
    ray_rowsel_release(s1);
    ray_release(pa);
    ray_release(pb);
    ray_heap_destroy();
    PASS();
}

/* to_indices: flattening must produce sorted global row indices
 * matching the oracle. */
static test_result_t test_rowsel_to_indices(void) {
    ray_heap_init();
    int64_t nrows = 2 * RAY_MORSEL_ELEMS + 13;
    ray_t* buf = ray_alloc((size_t)nrows);
    uint8_t* bytes = (uint8_t*)ray_data(buf);
    for (int64_t i = 0; i < nrows; i++) bytes[i] = (i % 5 == 0);
    ray_t* pred = make_pred(bytes, nrows);
    ray_t* sel = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(sel);

    ray_t* idx_block = ray_rowsel_to_indices(sel);
    TEST_ASSERT_NOT_NULL(idx_block);
    int64_t* idx = (int64_t*)ray_data(idx_block);

    int64_t oracle_n = ray_rowsel_meta(sel)->total_pass;
    int64_t k = 0;
    for (int64_t i = 0; i < nrows; i++) {
        if (bytes[i]) {
            TEST_ASSERT_EQ_I(idx[k], i);
            k++;
        }
    }
    TEST_ASSERT_EQ_I(k, oracle_n);

    ray_release(idx_block);
    ray_rowsel_release(sel);
    ray_release(pred);
    ray_release(buf);
    ray_heap_destroy();
    PASS();
}

/* Refine on a NULL existing — should behave like from_pred(pred). */
static test_result_t test_rowsel_refine_null_existing(void) {
    ray_heap_init();
    uint8_t bytes[10] = {1,0,1,0,1,0,1,0,1,0};
    ray_t* pred = make_pred(bytes, 10);
    ray_t* sel = ray_rowsel_refine(NULL, pred);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQ_I(ray_rowsel_meta(sel)->total_pass, 5);
    ray_rowsel_release(sel);
    ray_release(pred);
    ray_heap_destroy();
    PASS();
}

/* MIX∩MIX must initialize its output flag even when the recycled rowsel
 * payload previously held ALL.  The allocator zeroes object headers, not
 * payload bytes, so reading the output flag before assigning it can turn a
 * partial intersection into an entire morsel. */
static test_result_t test_rowsel_intersect_mixed_reused_block(void) {
    ray_heap_init();
    enum { N = RAY_MORSEL_ELEMS };
    uint8_t a_bits[N];
    uint8_t b_bits[N];
    memset(a_bits, 0, sizeof(a_bits));
    memset(b_bits, 0, sizeof(b_bits));

    const int64_t a_start = N / 4;
    const int64_t b_end = (N * 5) / 8;
    for (int64_t i = a_start; i < N; i++) a_bits[i] = 1;
    for (int64_t i = 0; i < b_end; i++) b_bits[i] = 1;

    ray_t* a_pred = make_pred(a_bits, N);
    ray_t* b_pred = make_pred(b_bits, N);
    TEST_ASSERT_NOT_NULL(a_pred);
    TEST_ASSERT_NOT_NULL(b_pred);
    ray_t* a = ray_rowsel_from_pred(a_pred);
    ray_t* b = ray_rowsel_from_pred(b_pred);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQ_I(ray_rowsel_flags(a)[0], RAY_SEL_MIX);
    TEST_ASSERT_EQ_I(ray_rowsel_flags(b)[0], RAY_SEL_MIX);

    const int64_t expected = b_end - a_start;
    ray_t* poison = ray_rowsel_new(N, expected, expected);
    TEST_ASSERT_NOT_NULL(poison);
    ray_rowsel_flags(poison)[0] = RAY_SEL_ALL;
    ray_rowsel_release(poison);

    ray_t* out = ray_rowsel_intersect(a, b);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQ_I(ray_rowsel_meta(out)->total_pass, expected);
    TEST_ASSERT_EQ_I(ray_rowsel_flags(out)[0], RAY_SEL_MIX);
    int64_t rows[N];
    int64_t count = reconstruct(out, rows);
    TEST_ASSERT_EQ_I(count, expected);
    TEST_ASSERT_EQ_I(rows[0], a_start);
    TEST_ASSERT_EQ_I(rows[count - 1], b_end - 1);

    ray_rowsel_release(out);
    ray_rowsel_release(a);
    ray_rowsel_release(b);
    ray_release(a_pred);
    ray_release(b_pred);
    ray_heap_destroy();
    PASS();
}

/* Streaming emitter must produce a block byte-identical to the
 * whole-vec ray_rowsel_from_pred over the same bools.  Pattern spans
 * >1 morsel with a NONE seg, an ALL seg, and MIX segs. */
static test_result_t test_rowsel_emit_equiv(void) {
    ray_heap_init();
    /* nrows spanning >1 morsel segment, a NONE seg, an ALL seg, a MIX seg */
    int64_t nrows = (int64_t)RAY_MORSEL_ELEMS * 3 + 7;
    ray_t* pred = ray_vec_new(RAY_BOOL, nrows);
    TEST_ASSERT_NOT_NULL(pred);
    uint8_t* p = (uint8_t*)ray_data(pred);
    for (int64_t i = 0; i < nrows; i++) {
        uint32_t seg = (uint32_t)(i / RAY_MORSEL_ELEMS);
        p[i] = (seg == 0) ? 0                 /* NONE */
             : (seg == 1) ? 1                 /* ALL  */
             : (uint8_t)((i & 3) == 0);       /* MIX  */
    }
    pred->len = nrows;
    ray_t* ref = ray_rowsel_from_pred(pred);
    TEST_ASSERT_NOT_NULL(ref);

    /* streaming: one builder, emit each segment from the same bool */
    uint32_t n_segs = (uint32_t)((nrows + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);
    rowsel_builder_t b;
    rowsel_builder_init(&b, n_segs);
    for (uint32_t s = 0; s < n_segs; s++) {
        int64_t lo = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t n  = (lo + RAY_MORSEL_ELEMS <= nrows) ? RAY_MORSEL_ELEMS : (nrows - lo);
        rowsel_emit_segment(&b, s, p + lo, n);
    }
    ray_t* got = rowsel_builder_finish(&b, 1, nrows);
    TEST_ASSERT_NOT_NULL(got);

    /* compare meta + the three arrays byte-for-byte */
    TEST_ASSERT(ray_rowsel_meta(got)->total_pass == ray_rowsel_meta(ref)->total_pass, "total_pass");
    TEST_ASSERT(ray_rowsel_meta(got)->n_segs    == ray_rowsel_meta(ref)->n_segs, "n_segs");
    TEST_ASSERT(memcmp(ray_rowsel_flags(got),   ray_rowsel_flags(ref),   n_segs) == 0, "flags");
    TEST_ASSERT(memcmp(ray_rowsel_offsets(got), ray_rowsel_offsets(ref), (size_t)(n_segs+1)*sizeof(uint32_t)) == 0, "offsets");
    /* meta has no idx_count field; idx occupancy is offsets[n_segs]. */
    int64_t ic = ray_rowsel_offsets(ref)[n_segs];
    TEST_ASSERT(memcmp(ray_rowsel_idx(got), ray_rowsel_idx(ref), (size_t)ic*sizeof(uint16_t)) == 0, "idx");

    ray_release(pred);
    ray_rowsel_release(ref);
    ray_rowsel_release(got);
    ray_heap_destroy();
    PASS();
}

/* ──────────────────────────────────────────────────────────────────
 * Suite registration
 * ────────────────────────────────────────────────────────────────── */

const test_entry_t rowsel_entries[] = {
    { "rowsel/empty", test_rowsel_empty, NULL, NULL },
    { "rowsel/all_pass", test_rowsel_all_pass, NULL, NULL },
    { "rowsel/none_pass", test_rowsel_none_pass, NULL, NULL },
    { "rowsel/single_morsel_mixed", test_rowsel_single_morsel_mixed, NULL, NULL },
    { "rowsel/multi_morsel", test_rowsel_multi_morsel, NULL, NULL },
    { "rowsel/partial_last_morsel", test_rowsel_partial_last_morsel, NULL, NULL },
    { "rowsel/parallel", test_rowsel_parallel, NULL, NULL },
    { "rowsel/all_segments_compact", test_rowsel_all_segments_compact, NULL, NULL },
    { "rowsel/to_indices", test_rowsel_to_indices, NULL, NULL },
    { "rowsel/refine", test_rowsel_refine, NULL, NULL },
    { "rowsel/refine_null_existing", test_rowsel_refine_null_existing, NULL, NULL },
    { "rowsel/intersect_mixed_reused_block", test_rowsel_intersect_mixed_reused_block, NULL, NULL },
    { "rowsel/emit_equiv", test_rowsel_emit_equiv, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

