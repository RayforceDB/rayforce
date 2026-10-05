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

#include "store/fsst.h"
#include <rayforce.h>
#include <stdlib.h>
#include <string.h>

/* Index: symbols grouped by first byte (start/cnt into ids), within a group
 * longest first, ties by code.  Built once per table, matched per position. */
void ray_fsst_index_build(const ray_fsst_table_t* t, ray_fsst_index_t* ix) {
    memset(ix, 0, sizeof *ix);
    for (int c = 0; c < t->n; c++) ix->cnt[t->sym[c][0]]++;
    uint8_t pos[256]; int acc = 0;
    for (int f = 0; f < 256; f++) { ix->start[f] = (uint8_t)acc; pos[f] = (uint8_t)acc; acc += ix->cnt[f]; }
    for (int len = 8; len >= 1; len--)
        for (int c = 0; c < t->n; c++)
            if (t->len[c] == len) ix->ids[pos[t->sym[c][0]]++] = (uint8_t)c;
}

static int fsst_match(const ray_fsst_table_t* t, const ray_fsst_index_t* ix,
                      const uint8_t* s, uint32_t rem) {
    uint8_t f = s[0];
    const uint8_t* ids = ix->ids + ix->start[f];
    for (int j = 0; j < ix->cnt[f]; j++) {
        int c = ids[j];
        if (t->len[c] <= rem && memcmp(t->sym[c], s, t->len[c]) == 0) return c;
    }
    return -1;
}

int64_t ray_fsst_encode_ix(const ray_fsst_table_t* t, const ray_fsst_index_t* ix,
                           const uint8_t* s, uint32_t len, uint8_t* out) {
    int64_t o = 0;
    for (uint32_t i = 0; i < len;) {
        int c = fsst_match(t, ix, s + i, len - i);
        if (c >= 0) { out[o++] = (uint8_t)c; i += t->len[c]; }
        else        { out[o++] = RAY_FSST_ESC; out[o++] = s[i++]; }
    }
    return o;
}

int64_t ray_fsst_encode(const ray_fsst_table_t* t, const uint8_t* s, uint32_t len, uint8_t* out) {
    ray_fsst_index_t ix; ray_fsst_index_build(t, &ix);
    return ray_fsst_encode_ix(t, &ix, s, len, out);
}

int64_t ray_fsst_expanded_len(const ray_fsst_table_t* t, const uint8_t* in, uint32_t len) {
    int64_t x = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        if (c == RAY_FSST_ESC) { if (i + 1 >= len) return -1; i++; x += 1; }
        else if (c < t->n) x += t->len[c];
        else return -1;
    }
    return x;
}

int64_t ray_fsst_decode(const ray_fsst_table_t* t, const uint8_t* in, uint32_t len,
                        uint8_t* out, size_t cap) {
    int64_t x = ray_fsst_expanded_len(t, in, len);
    if (x < 0 || (size_t)x > cap) return -1;
    size_t o = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        if (c == RAY_FSST_ESC) out[o++] = in[++i];
        else { memcpy(out + o, t->sym[c], t->len[c]); o += t->len[c]; }
    }
    return x;
}

int64_t ray_fsst_table_bytes(const ray_fsst_table_t* t) {
    return 8 + (((int64_t)t->n + 7) & ~(int64_t)7) + 8 * (int64_t)t->n;
}

/* ---- training ----
 * Five rounds.  Each round compresses the sample with the current table and
 * counts how often each symbol occurs (count1) and how often each pair of
 * consecutive symbols occurs (count2, pairs whose concatenation is at most
 * 8 bytes).  Escaped bytes count as single-byte candidate symbols.  The next
 * table is the 255 candidates with the highest gain (count * length), ties
 * broken by length, then bytes, so training is deterministic. */
typedef struct { uint8_t len; uint8_t b[8]; uint64_t gain; } fsst_cand_t;

static int cand_cmp(const fsst_cand_t* a, const fsst_cand_t* b) {
    if (a->gain != b->gain) return a->gain > b->gain ? -1 : 1;
    if (a->len != b->len)   return a->len > b->len ? -1 : 1;
    return memcmp(a->b, b->b, 8);
}

/* Training sample budget: when the input holds more than this many bytes, every
 * stride-th string is used (stride = ceil(total / budget)), so the sample spans
 * the whole input evenly.  The candidate map holds 2^17 slots; a new key is
 * dropped once it is half full while existing keys keep counting.  With a
 * 256 KiB sample that cap is rarely reached. */
#define FSST_SAMPLE_BYTES (1u << 18)

static int cand_qcmp(const void* a, const void* b) {
    return cand_cmp((const fsst_cand_t*)a, (const fsst_cand_t*)b);
}

/* Candidates live in a small open-addressing map keyed by (len, bytes). */
#define FSST_MAP_BITS 17
typedef struct { fsst_cand_t* e; uint32_t n, cap; } fsst_map_t;

static uint64_t cand_key_hash(uint8_t len, const uint8_t* b) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ len;
    for (int i = 0; i < len; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static void map_add(fsst_map_t* m, uint8_t len, const uint8_t* b, uint64_t g) {
    uint32_t mask = m->cap - 1;
    for (uint32_t i = (uint32_t)cand_key_hash(len, b) & mask;; i = (i + 1) & mask) {
        fsst_cand_t* e = &m->e[i];
        if (e->len == 0) { if (m->n >= m->cap / 2) return; e->len = len; memset(e->b, 0, 8); memcpy(e->b, b, len); e->gain = g; m->n++; return; }
        if (e->len == len && memcmp(e->b, b, len) == 0) { e->gain += g; return; }
    }
}

void ray_fsst_train(const uint8_t* const* strs, const uint32_t* lens, int64_t n,
                    ray_fsst_table_t* t) {
    memset(t, 0, sizeof *t);
    uint64_t total = 0;
    for (int64_t i = 0; i < n; i++) total += lens[i];
    int64_t stride = total > FSST_SAMPLE_BYTES ? (int64_t)((total + FSST_SAMPLE_BYTES - 1) / FSST_SAMPLE_BYTES) : 1;
    fsst_map_t m = { NULL, 0, 1u << FSST_MAP_BITS };
    m.e = ray_calloc_raw(sizeof(fsst_cand_t) * m.cap);
    fsst_cand_t* all = ray_calloc_raw(sizeof(fsst_cand_t) * (m.cap / 2 + 1));
    if (!m.e || !all) { ray_free_raw(m.e); ray_free_raw(all); return; }  /* empty table: everything escapes */
    for (int round = 0; round < 5; round++) {
        memset(m.e, 0, sizeof(fsst_cand_t) * m.cap); m.n = 0;
        ray_fsst_index_t ix; ray_fsst_index_build(t, &ix);
        for (int64_t si = 0; si < n; si += stride) {
            const uint8_t* s = strs[si]; uint32_t len = lens[si];
            int prev_len = 0; uint8_t prev[8];
            for (uint32_t i = 0; i < len;) {
                int c = t->n ? fsst_match(t, &ix, s + i, len - i) : -1;
                uint8_t cl = c >= 0 ? t->len[c] : 1;
                const uint8_t* cb = c >= 0 ? t->sym[c] : s + i;
                map_add(&m, cl, cb, cl);                         /* count1 * len */
                if (prev_len && prev_len + cl <= 8) {
                    uint8_t cat[8]; memcpy(cat, prev, (size_t)prev_len); memcpy(cat + prev_len, cb, cl);
                    map_add(&m, (uint8_t)(prev_len + cl), cat, (uint64_t)(prev_len + cl));  /* count2 * len */
                }
                memcpy(prev, cb, cl); prev_len = cl;
                i += cl;
            }
        }
        /* total order (gain, len, bytes) over distinct keys: sort, keep the first 255 */
        size_t k = 0;
        for (uint32_t i = 0; i < m.cap; i++) if (m.e[i].len) all[k++] = m.e[i];
        qsort(all, k, sizeof *all, cand_qcmp);
        if (k > 255) k = 255;
        ray_fsst_table_t nt; memset(&nt, 0, sizeof nt);
        nt.n = (uint8_t)k;
        for (size_t c = 0; c < k; c++) { nt.len[c] = all[c].len; memcpy(nt.sym[c], all[c].b, 8); }
        *t = nt;
    }
    ray_free_raw(all);
    ray_free_raw(m.e);
}
