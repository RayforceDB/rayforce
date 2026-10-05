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
#include <string.h>

/* Lookup: for each first byte, the symbols starting with it, longest first.
 * Rebuilt per call from the table (cheap: 255 symbols). */
typedef struct { uint8_t cnt[256]; uint8_t ids[256][255]; } fsst_index_t;

static void fsst_index(const ray_fsst_table_t* t, fsst_index_t* ix) {
    memset(ix->cnt, 0, sizeof ix->cnt);
    for (int len = 8; len >= 1; len--)
        for (int c = 0; c < t->n; c++)
            if (t->len[c] == len) { uint8_t f = t->sym[c][0]; ix->ids[f][ix->cnt[f]++] = (uint8_t)c; }
}

static int fsst_match(const ray_fsst_table_t* t, const fsst_index_t* ix,
                      const uint8_t* s, uint32_t rem) {
    uint8_t f = s[0];
    for (int j = 0; j < ix->cnt[f]; j++) {
        int c = ix->ids[f][j];
        if (t->len[c] <= rem && memcmp(t->sym[c], s, t->len[c]) == 0) return c;
    }
    return -1;
}

int64_t ray_fsst_encode(const ray_fsst_table_t* t, const uint8_t* s, uint32_t len, uint8_t* out) {
    fsst_index_t ix; fsst_index(t, &ix);
    int64_t o = 0;
    for (uint32_t i = 0; i < len;) {
        int c = fsst_match(t, &ix, s + i, len - i);
        if (c >= 0) { out[o++] = (uint8_t)c; i += t->len[c]; }
        else        { out[o++] = RAY_FSST_ESC; out[o++] = s[i++]; }
    }
    return o;
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

/* Candidates live in a small open-addressing map keyed by (len, bytes). */
#define FSST_MAP_BITS 16
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
        if (e->len == 0) { e->len = len; memset(e->b, 0, 8); memcpy(e->b, b, len); e->gain = g; m->n++; return; }
        if (e->len == len && memcmp(e->b, b, len) == 0) { e->gain += g; return; }
    }
}

void ray_fsst_train(const uint8_t* const* strs, const uint32_t* lens, int64_t n,
                    ray_fsst_table_t* t) {
    memset(t, 0, sizeof *t);
    fsst_map_t m = { NULL, 0, 1u << FSST_MAP_BITS };
    m.e = ray_calloc_raw(sizeof(fsst_cand_t) * m.cap);
    if (!m.e) return;                                  /* empty table: everything escapes */
    for (int round = 0; round < 5; round++) {
        memset(m.e, 0, sizeof(fsst_cand_t) * m.cap); m.n = 0;
        fsst_index_t ix; fsst_index(t, &ix);
        for (int64_t si = 0; si < n; si++) {
            const uint8_t* s = strs[si]; uint32_t len = lens[si];
            int prev_len = 0; uint8_t prev[8];
            for (uint32_t i = 0; i < len && m.n < m.cap / 2;) {
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
        /* pick the top 255 by gain: partial selection over the map */
        ray_fsst_table_t nt; memset(&nt, 0, sizeof nt);
        fsst_cand_t top[255]; int nt_n = 0;
        for (uint32_t i = 0; i < m.cap; i++) {
            fsst_cand_t* e = &m.e[i];
            if (!e->len) continue;
            if (nt_n < 255) { top[nt_n++] = *e; }
            else {
                int worst = 0;
                for (int j = 1; j < 255; j++) if (cand_cmp(&top[j], &top[worst]) > 0) worst = j;
                if (cand_cmp(e, &top[worst]) < 0) top[worst] = *e;
            }
        }
        /* stable order: sort the chosen symbols by (gain desc, len desc, bytes) */
        for (int a = 1; a < nt_n; a++)
            for (int b = a; b > 0 && cand_cmp(&top[b], &top[b - 1]) < 0; b--) {
                fsst_cand_t x = top[b]; top[b] = top[b - 1]; top[b - 1] = x;
            }
        nt.n = (uint8_t)nt_n;
        for (int c = 0; c < nt_n; c++) { nt.len[c] = top[c].len; memcpy(nt.sym[c], top[c].b, 8); }
        *t = nt;
    }
    ray_free_raw(m.e);
}
