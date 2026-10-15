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
#include "mem/heap.h"
#include "table/sym.h"
#include "store/enc.h"
#include <stdlib.h>
#include <string.h>

static void enc_setup(void)    { ray_heap_init(); (void)ray_sym_init(); }
static void enc_teardown(void) { ray_sym_destroy(); ray_heap_destroy(); }

/* Deterministic xorshift for test data. */
static uint64_t enc_rng(uint64_t* s) {
    uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x;
}

static test_result_t test_pack_roundtrip_all_widths(void) {
    static const int Ts[4] = { 8, 16, 32, 64 };
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    uint64_t* u = malloc(sizeof(uint64_t) * RAY_ENC_VEC);
    uint64_t* back = malloc(sizeof(uint64_t) * RAY_ENC_VEC);
    uint8_t* buf = malloc(RAY_ENC_VEC * 8);
    TEST_ASSERT_TRUE(u && back && buf);
    for (int ti = 0; ti < 4; ti++) {
        int T = Ts[ti];
        for (int w = 0; w <= T; w++) {
            uint64_t mask = w == 64 ? ~0ull : ((1ull << w) - 1);
            for (int i = 0; i < RAY_ENC_VEC; i++) u[i] = enc_rng(&seed) & mask;
            memset(buf, 0xA5, RAY_ENC_VEC * 8);
            ray_enc_pack(u, T, w, buf);
            ray_enc_unpack(buf, T, w, back);
            for (int i = 0; i < RAY_ENC_VEC; i++)
                TEST_ASSERT_FMT(back[i] == u[i], "T=%d w=%d i=%d", T, w, i);
        }
    }
    free(u); free(back); free(buf);
    PASS();
}

/* The layout itself, not just the round trip: with T=8, w=1, value i is
 * bit (i / L) % 8 of byte i % L where L = 128, because each lane has one
 * 8-bit word (w=1 word per lane) holding its 8 rows LSB first. */
static test_result_t test_pack_layout_t8_w1(void) {
    uint64_t u[RAY_ENC_VEC];
    uint8_t out[RAY_ENC_VEC / 8];
    for (int i = 0; i < RAY_ENC_VEC; i++) u[i] = (i == 3 || i == 128 + 5 || i == 7 * 128 + 127) ? 1 : 0;
    ray_enc_pack(u, 8, 1, out);
    TEST_ASSERT_EQ_U(ray_enc_packed_bytes(1), 128);
    for (int b = 0; b < 128; b++) {
        uint8_t want = 0;
        if (b == 3)   want |= 1u << 0;   /* i=3:   lane 3,   row 0 */
        if (b == 5)   want |= 1u << 1;   /* i=133: lane 5,   row 1 */
        if (b == 127) want |= 1u << 7;   /* i=1023: lane 127, row 7 */
        TEST_ASSERT_FMT(out[b] == want, "byte %d = %02x want %02x", b, out[b], want);
    }
    PASS();
}

/* T=16, w=3: lane l's 16 values occupy 3 consecutive 16-bit words stored at
 * word indexes l, L+l, 2L+l (L = 64); value r of lane l sits at bit 3*r of
 * that 48-bit stream. */
static test_result_t test_pack_layout_t16_w3(void) {
    uint64_t u[RAY_ENC_VEC] = {0};
    int L = 64;
    u[5 + 6 * L] = 5;            /* lane 5, row 6: bits 18..20 -> word 1, bits 2..4 */
    uint16_t words[RAY_ENC_VEC * 3 / 16];
    ray_enc_pack(u, 16, 3, (uint8_t*)words);
    for (int k = 0; k < 3; k++)
        for (int l = 0; l < L; l++) {
            uint16_t want = (k == 1 && l == 5) ? (uint16_t)(5u << 2) : 0;
            TEST_ASSERT_FMT(words[k * L + l] == want, "k=%d l=%d", k, l);
        }
    PASS();
}

static test_result_t test_bitlen(void) {
    TEST_ASSERT_EQ_I(ray_enc_bitlen(0), 0);
    TEST_ASSERT_EQ_I(ray_enc_bitlen(1), 1);
    TEST_ASSERT_EQ_I(ray_enc_bitlen(255), 8);
    TEST_ASSERT_EQ_I(ray_enc_bitlen(256), 9);
    TEST_ASSERT_EQ_I(ray_enc_bitlen(~0ull), 64);
    PASS();
}

static test_result_t test_kind_of(void) {
    ray_enc_kind_t k;
    TEST_ASSERT_TRUE(ray_enc_kind_of(RAY_I16, 0, &k)); TEST_ASSERT_EQ_I(k.T, 16); TEST_ASSERT_TRUE(k.is_signed);
    TEST_ASSERT_TRUE(ray_enc_kind_of(RAY_DATE, 0, &k)); TEST_ASSERT_EQ_I(k.T, 32); TEST_ASSERT_TRUE(k.is_signed);
    TEST_ASSERT_TRUE(ray_enc_kind_of(RAY_TIMESTAMP, 0, &k)); TEST_ASSERT_EQ_I(k.T, 64);
    TEST_ASSERT_TRUE(ray_enc_kind_of(RAY_U8, 0, &k)); TEST_ASSERT_EQ_I(k.T, 8); TEST_ASSERT_FALSE(k.is_signed);
    TEST_ASSERT_TRUE(ray_enc_kind_of(RAY_SYM, RAY_SYM_W16, &k)); TEST_ASSERT_EQ_I(k.T, 16); TEST_ASSERT_FALSE(k.is_signed);
    TEST_ASSERT_FALSE(ray_enc_kind_of(RAY_F64, 0, &k));
    TEST_ASSERT_FALSE(ray_enc_kind_of(RAY_STR, 0, &k));
    PASS();
}

static test_result_t test_logical_and_order(void) {
    ray_enc_kind_t s16 = { 16, true, false }, u8 = { 8, false, false };
    TEST_ASSERT_EQ_I(ray_enc_logical(0xFFFF, s16), -1);
    TEST_ASSERT_EQ_I(ray_enc_logical(0x7FFF, s16), 32767);
    TEST_ASSERT_TRUE(ray_enc_lt(0xFFFF, 0x0001, s16));   /* -1 < 1 */
    TEST_ASSERT_EQ_I(ray_enc_logical(0x80, u8), 128);     /* no sign extension */
    TEST_ASSERT_TRUE(ray_enc_lt(0x7F, 0x80, u8));         /* 127 < 128 */
    PASS();
}

static test_result_t test_load_i64_nulls_fill_min(void) {
    int64_t d[5] = { 40, NULL_I64, -7, NULL_I64, 12 };
    ray_enc_input_t in;
    TEST_ASSERT_TRUE(ray_enc_load(d, RAY_I64, 0, 0, 5, &in));
    TEST_ASSERT_EQ_I(in.n, 5);
    TEST_ASSERT_EQ_I(in.n_nulls, 2);
    TEST_ASSERT_TRUE(in.bitmap);
    TEST_ASSERT_EQ_U(in.nullmap[0], 0x0A);                /* rows 1 and 3 */
    TEST_ASSERT_EQ_I((int64_t)in.bits[1], -7);            /* filled with min */
    TEST_ASSERT_EQ_I((int64_t)in.bits[3], -7);
    TEST_ASSERT_EQ_I((int64_t)in.bits[4], 12);
    PASS();
}

static test_result_t test_load_all_null_fills_zero(void) {
    int32_t d[3] = { NULL_I32, NULL_I32, NULL_I32 };
    ray_enc_input_t in;
    TEST_ASSERT_TRUE(ray_enc_load(d, RAY_I32, 0, 0, 3, &in));
    TEST_ASSERT_EQ_I(in.n_nulls, 3);
    for (int i = 0; i < 3; i++) TEST_ASSERT_EQ_U(in.bits[i], 0);
    PASS();
}

static test_result_t test_load_sym_w8_inband(void) {
    uint8_t d[4] = { 0, 127, 128, 255 };                   /* id 0 is the in-band null */
    ray_enc_input_t in;
    TEST_ASSERT_TRUE(ray_enc_load(d, RAY_SYM, RAY_SYM_W8, 0, 4, &in));
    TEST_ASSERT_FALSE(in.bitmap);
    TEST_ASSERT_EQ_I(in.n_nulls, 1);
    TEST_ASSERT_EQ_U(in.bits[0], 0);                       /* stored as itself */
    TEST_ASSERT_EQ_U(in.bits[2], 128);
    ray_enc_stats_t s;
    ray_enc_stats(in.bits, in.n, in.k, &s);
    TEST_ASSERT_EQ_U(s.min, 0);
    TEST_ASSERT_EQ_U(s.max, 255);
    TEST_ASSERT_EQ_I(s.runs, 4);
    PASS();
}

static test_result_t test_stats_runs_and_equal(void) {
    uint64_t v[6] = { 5, 5, 5, 9, 9, 5 };
    ray_enc_kind_t k = { 32, true, false };
    ray_enc_stats_t s;
    ray_enc_stats(v, 6, k, &s);
    TEST_ASSERT_EQ_I(s.runs, 3);
    TEST_ASSERT_FALSE(s.all_equal);
    ray_enc_stats(v, 3, k, &s);
    TEST_ASSERT_TRUE(s.all_equal);
    TEST_ASSERT_EQ_I(s.runs, 1);
    PASS();
}

static test_result_t test_load_offset_start(void) {
    int16_t d[2000];
    for (int i = 0; i < 2000; i++) d[i] = (int16_t)(i - 1000);
    ray_enc_input_t in;
    TEST_ASSERT_TRUE(ray_enc_load(d, RAY_I16, 0, 1024, 976, &in));
    TEST_ASSERT_EQ_I(in.n, 976);
    TEST_ASSERT_EQ_I(ray_enc_logical(in.bits[0], in.k), 24);
    TEST_ASSERT_EQ_I(ray_enc_logical(in.bits[975], in.k), 999);
    PASS();
}

static test_result_t test_sizes_raw_const(void) {
    TEST_ASSERT_EQ_I(ray_enc_size_raw(1024, 16), 2048);
    TEST_ASSERT_EQ_I(ray_enc_size_raw(3, 16), 8);      /* 6 bytes padded to 8 */
    TEST_ASSERT_EQ_I(ray_enc_size_raw(1, 64), 8);
    TEST_ASSERT_EQ_I(ray_enc_size_const(), 8);
    PASS();
}

static test_result_t test_for_width_signed_extremes(void) {
    uint64_t v[3] = { (uint64_t)(INT64_MIN + 1), (uint64_t)INT64_MAX, 0 };
    ray_enc_kind_t k = { 64, true, false };
    uint64_t base;
    int w = ray_enc_for_width(v, 3, k, &base);
    TEST_ASSERT_EQ_U(base, (uint64_t)(INT64_MIN + 1));  /* native min */
    TEST_ASSERT_EQ_I(w, 64);                              /* range 2^64 - 2 */
    TEST_ASSERT_EQ_I(ray_enc_size_for(w), 8 + 8192);
    PASS();
}

static test_result_t test_for_width_narrow(void) {
    uint64_t v[4] = { 0xFFFE, 0xFFFF, 0x0000, 0x0001 };  /* -2,-1,0,1 as I16 */
    ray_enc_kind_t k = { 16, true, false };
    uint64_t base;
    int w = ray_enc_for_width(v, 4, k, &base);
    TEST_ASSERT_EQ_U(base, 0xFFFE);
    TEST_ASSERT_EQ_I(w, 2);                               /* residuals 0..3 */
    PASS();
}

static test_result_t test_delta_width_sorted(void) {
    static uint64_t v[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)(1000000 + 3 * i);
    ray_enc_kind_t k = { 64, true, false };
    uint64_t dbase;
    int w = ray_enc_delta_width(v, RAY_ENC_VEC, k, &dbase);
    TEST_ASSERT_EQ_U(dbase, 3 * 16);                      /* lane stride L=16 */
    TEST_ASSERT_EQ_I(w, 0);
    TEST_ASSERT_EQ_I(ray_enc_size_delta(64, 0), 8 + 8 * 16);
    PASS();
}

/* Encode at the top level, decode, compare; returns the chosen encoding. */
static int enc_roundtrip(const uint64_t* v, int n, ray_enc_kind_t k,
                         const ray_enc_dict_t* dict, int64_t* bytes) {
    ray_enc_buf_t b = {0};
    ray_enc_choice_t c;
    if (!ray_enc_write(v, n, k, RAY_ENC_SLOT_TOP, 0, dict, &b, &c)) return -1;
    if ((int64_t)b.len != c.bytes) { ray_enc_buf_free(&b); return -2; }
    uint64_t out[RAY_ENC_VEC];
    int64_t used = ray_enc_read(b.p, b.len, c.enc, c.width, n, k, RAY_ENC_SLOT_TOP, 0, dict, out);
    int rc = used == (int64_t)b.len ? c.enc : -3;
    for (int i = 0; rc >= 0 && i < n; i++) if (out[i] != (v[i] & (k.T == 64 ? ~0ull : ((1ull << k.T) - 1)))) rc = -4;
    if (bytes) *bytes = c.bytes;
    ray_enc_buf_free(&b);
    return rc;
}

static test_result_t test_choose_const(void) {
    uint64_t v[1000];
    for (int i = 0; i < 1000; i++) v[i] = 42;
    ray_enc_kind_t k = { 32, true, false };
    int64_t bytes;
    TEST_ASSERT_EQ_I(enc_roundtrip(v, 1000, k, NULL, &bytes), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(bytes, 8);
    PASS();
}

static test_result_t test_choose_rle_runs(void) {
    static uint64_t v[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)(i / 100) * 1000003ull;  /* 11 runs, wide values */
    ray_enc_kind_t k = { 64, true, false };
    TEST_ASSERT_EQ_I(enc_roundtrip(v, RAY_ENC_VEC, k, NULL, NULL), RAY_ENC_RLE);
    PASS();
}

static test_result_t test_choose_for(void) {
    static uint64_t v[RAY_ENC_VEC];
    uint64_t s = 7;
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = 5000 + (enc_rng(&s) & 0x3F);  /* 6-bit spread */
    ray_enc_kind_t k = { 32, true, false };
    int64_t bytes;
    TEST_ASSERT_EQ_I(enc_roundtrip(v, RAY_ENC_VEC, k, NULL, &bytes), RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(bytes, 8 + 1024 * 6 / 8);
    PASS();
}

static test_result_t test_choose_delta(void) {
    static uint64_t v[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = 1700000000000ull + 1000ull * (uint64_t)i;  /* timestamps */
    ray_enc_kind_t k = { 64, true, false };
    TEST_ASSERT_EQ_I(enc_roundtrip(v, RAY_ENC_VEC, k, NULL, NULL), RAY_ENC_DELTA_BP);
    PASS();
}

static test_result_t test_keep_raw_when_gain_small(void) {
    static uint64_t v[RAY_ENC_VEC];
    uint64_t s = 99;
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = enc_rng(&s) & 0xFFFF;  /* full 16-bit spread */
    ray_enc_kind_t k = { 16, false, false };
    TEST_ASSERT_EQ_I(enc_roundtrip(v, RAY_ENC_VEC, k, NULL, NULL), RAY_ENC_RAW);
    PASS();
}

static test_result_t test_dict_wide_sparse_values(void) {
    static uint64_t v[RAY_ENC_VEC];
    static const uint64_t vals[4] = { 11ull, 900000000001ull, 77ull, 123456789012345ull };
    /* Pseudo-random picks: a period dividing the delta lane stride (16) would make
     * DELTA_BP the smaller encoding (all lane deltas zero), which is correct but
     * not what this test exercises. */
    uint64_t s = 31;
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = vals[enc_rng(&s) & 3];
    ray_enc_kind_t k = { 64, true, false };
    uint64_t* d; uint32_t dn;
    TEST_ASSERT_EQ_I(ray_enc_train_dict(v, RAY_ENC_VEC, k, &d, &dn), 1);
    TEST_ASSERT_EQ_I(dn, 4);
    TEST_ASSERT_EQ_U(d[0], 11ull);
    TEST_ASSERT_EQ_U(d[3], 123456789012345ull);
    ray_enc_dict_t dict = { d, dn };
    TEST_ASSERT_EQ_I(enc_roundtrip(v, RAY_ENC_VEC, k, &dict, NULL), RAY_ENC_DICT);
    ray_free_raw(d);
    PASS();
}

static test_result_t test_roundtrip_lengths_and_types(void) {
    static const int lens[6] = { 1, 2, 1023, 1024, 17, 512 };
    static const ray_enc_kind_t kinds[5] = { { 8, false, false }, { 16, true, false }, { 32, true, false }, { 64, true, false }, { 16, false, false } };
    uint64_t s = 12345;
    for (int ki = 0; ki < 5; ki++)
        for (int li = 0; li < 6; li++)
            for (int shape = 0; shape < 4; shape++) {
                static uint64_t v[RAY_ENC_VEC];
                int n = lens[li];
                for (int i = 0; i < n; i++) {
                    uint64_t x = enc_rng(&s);
                    v[i] = shape == 0 ? x : shape == 1 ? (x & 3) : shape == 2 ? (uint64_t)(i / 37) : 7;
                }
                int rc = enc_roundtrip(v, n, kinds[ki], NULL, NULL);
                TEST_ASSERT_FMT(rc >= 0, "kind T=%d n=%d shape=%d rc=%d", kinds[ki].T, n, shape, rc);
            }
    PASS();
}

static test_result_t test_i64_extremes_roundtrip(void) {
    uint64_t v[6] = { (uint64_t)(INT64_MIN + 1), (uint64_t)INT64_MAX, 0,
                      (uint64_t)(INT64_MIN + 1), (uint64_t)-5, (uint64_t)INT64_MAX };
    ray_enc_kind_t k = { 64, true, false };
    TEST_ASSERT_TRUE(enc_roundtrip(v, 6, k, NULL, NULL) >= 0);
    PASS();
}

/* DICT at the top whose codes child is RLE: ~100 distinct wide values in runs of
 * 10.  Top-level RLE would need RAW wide values (8 B per run); DICT with RLE
 * codes (4 B per run) is smaller.  DICT is legal only at the top, so this is the
 * deepest nesting the encoder emits (DICT -> RLE codes -> RAW/FOR children). */
static test_result_t test_dict_codes_rle_nested(void) {
    static uint64_t v[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = 0x123456789ABull * (uint64_t)((i / 10) % 100 + 1);
    ray_enc_kind_t k = { 64, true, false };
    uint64_t* d; uint32_t dn;
    TEST_ASSERT_EQ_I(ray_enc_train_dict(v, RAY_ENC_VEC, k, &d, &dn), 1);
    ray_enc_dict_t dict = { d, dn };
    ray_enc_buf_t b = {0};
    ray_enc_choice_t c;
    TEST_ASSERT_TRUE(ray_enc_write(v, RAY_ENC_VEC, k, RAY_ENC_SLOT_TOP, 0, &dict, &b, &c));
    TEST_ASSERT_EQ_I(c.enc, RAY_ENC_DICT);
    TEST_ASSERT_EQ_I(b.p[0], RAY_ENC_RLE);     /* codes child header: encoding */
    uint64_t out[RAY_ENC_VEC];
    TEST_ASSERT_EQ_I(ray_enc_read(b.p, b.len, c.enc, c.width, RAY_ENC_VEC, k, RAY_ENC_SLOT_TOP, 0, &dict, out), (int64_t)b.len);
    for (int i = 0; i < RAY_ENC_VEC; i++) TEST_ASSERT_FMT(out[i] == v[i], "i=%d", i);
    ray_enc_buf_free(&b);
    ray_free_raw(d);
    PASS();
}

/* A valid depth-1 RLE body for k={32,true}, n=4 (nr runs <= 2): values RAW child
 * (U32), ends RAW child (U16).  40 bytes. */
static void rle_body(uint8_t* b, uint32_t nr, uint32_t v0, uint32_t v1, uint16_t e0, uint16_t e1) {
    memset(b, 0, 40);
    memcpy(b, &nr, 4);
    b[8] = RAY_ENC_RAW; uint32_t bb = 8; memcpy(b + 12, &bb, 4);
    memcpy(b + 16, &v0, 4); memcpy(b + 20, &v1, 4);
    b[24] = RAY_ENC_RAW; memcpy(b + 28, &bb, 4);
    memcpy(b + 32, &e0, 2); memcpy(b + 34, &e1, 2);
}

static test_result_t test_read_rejects_malformed(void) {
    ray_enc_kind_t k = { 32, true, false }, k16 = { 16, false, false };
    uint8_t b[400];
    uint64_t out[RAY_ENC_VEC];
#define RLE_READ(av) ray_enc_read(b, (av), RAY_ENC_RLE, 0, 4, k, RAY_ENC_SLOT_TOP, 0, NULL, out)
    rle_body(b, 1, 5, 0, 4, 0);
    TEST_ASSERT_EQ_I(RLE_READ(40), 40);                       /* control */
    TEST_ASSERT_EQ_I(RLE_READ(39), -1);                       /* truncated */
    rle_body(b, 1, 5, 0, 4, 0); b[10] = 1;                    /* child header reserved */
    TEST_ASSERT_EQ_I(RLE_READ(40), -1);
    rle_body(b, 1, 5, 0, 4, 0); b[20] = 1;                    /* RAW padding of values */
    TEST_ASSERT_EQ_I(RLE_READ(40), -1);
    rle_body(b, 1, 5, 0, 4, 0); b[9] = 1;                     /* width on a RAW child */
    TEST_ASSERT_EQ_I(RLE_READ(40), -1);
    rle_body(b, 1, 5, 0, 4, 0); b[8] = RAY_ENC_FOR_BP; b[9] = 33;   /* width > T */
    TEST_ASSERT_EQ_I(RLE_READ(40), -1);
    rle_body(b, 1, 5, 0, 4, 0); b[12] = 7;                    /* body_bytes not multiple of 8 */
    TEST_ASSERT_EQ_I(RLE_READ(40), -1);
    rle_body(b, 1, 5, 0, 4, 0); b[13] = 4;                    /* body_bytes past avail */
    TEST_ASSERT_EQ_I(RLE_READ(40), -1);
    rle_body(b, 2, 5, 6, 2, 4);
    TEST_ASSERT_EQ_I(RLE_READ(40), 40);                       /* control, two runs */
    rle_body(b, 2, 5, 6, 4, 2);                               /* ends not ascending */
    TEST_ASSERT_EQ_I(RLE_READ(40), -1);
    rle_body(b, 1, 5, 0, 3, 0);                               /* last end != n */
    TEST_ASSERT_EQ_I(RLE_READ(40), -1);

    /* top-level bodies */
    memset(b, 0, sizeof b);
    uint64_t base = 5; memcpy(b, &base, 8);                   /* FOR, w=0 */
    TEST_ASSERT_EQ_I(ray_enc_read(b, 8, RAY_ENC_FOR_BP, 0, 4, k16, RAY_ENC_SLOT_TOP, 0, NULL, out), 8);
    base = 1ull << 16; memcpy(b, &base, 8);                   /* FOR base bit above T */
    TEST_ASSERT_EQ_I(ray_enc_read(b, 8, RAY_ENC_FOR_BP, 0, 4, k16, RAY_ENC_SLOT_TOP, 0, NULL, out), -1);
    memset(b, 0, sizeof b);
    TEST_ASSERT_EQ_I(ray_enc_read(b, 8, RAY_ENC_FOR_BP, 17, 4, k16, RAY_ENC_SLOT_TOP, 0, NULL, out), -1);  /* w > T */
    TEST_ASSERT_EQ_I(ray_enc_read(b, 8, RAY_ENC_RAW, 1, 4, k16, RAY_ENC_SLOT_TOP, 0, NULL, out), -1);      /* width on RAW */
    TEST_ASSERT_EQ_I(ray_enc_read(b, 8, RAY_ENC_RAW, 0, 3, k16, RAY_ENC_SLOT_TOP, 0, NULL, out), 8);
    b[7] = 1;                                                 /* RAW padding (n=3 U16 leaves 2 B) */
    TEST_ASSERT_EQ_I(ray_enc_read(b, 8, RAY_ENC_RAW, 0, 3, k16, RAY_ENC_SLOT_TOP, 0, NULL, out), -1);

    int64_t dsz = ray_enc_size_delta(32, 0);                  /* DELTA, k = I32 */
    static uint8_t db[4096];
    memset(db, 0, sizeof db);
    TEST_ASSERT_EQ_I(ray_enc_read(db, (size_t)dsz, RAY_ENC_DELTA_BP, 0, 100, k, RAY_ENC_SLOT_TOP, 0, NULL, out), dsz);
    uint64_t dbase = 1ull << 32; memcpy(db, &dbase, 8);       /* dbase bit above T */
    TEST_ASSERT_EQ_I(ray_enc_read(db, (size_t)dsz, RAY_ENC_DELTA_BP, 0, 100, k, RAY_ENC_SLOT_TOP, 0, NULL, out), -1);
    memset(db, 0, sizeof db);
    int64_t dsz1 = ray_enc_size_delta(32, 1);                 /* placeholder residual non-zero */
    memset(db + 8 + 8 * 32, 0xFF, (size_t)(dsz1 - 8 - 8 * 32));
    TEST_ASSERT_EQ_I(ray_enc_read(db, (size_t)dsz1, RAY_ENC_DELTA_BP, 1, 100, k, RAY_ENC_SLOT_TOP, 0, NULL, out), -1);

    /* DELTA as a depth-3 child */
    memset(b, 0, sizeof b);
    b[0] = RAY_ENC_DELTA_BP; uint32_t bb = 8; memcpy(b + 4, &bb, 4);
    TEST_ASSERT_EQ_I(ray_enc_read(b, 16, 0, 0, 2, ((ray_enc_kind_t){ 16, false, false }), RAY_ENC_SLOT_RLE_ENDS, 3, NULL, out), -1);

    /* DICT code >= dictionary count (top-level DICT, RAW codes child) */
    static const uint64_t dv[1] = { 77 };
    ray_enc_dict_t dict = { dv, 1 };
    memset(b, 0, sizeof b);
    b[0] = RAY_ENC_RAW; bb = 8; memcpy(b + 4, &bb, 4);
    TEST_ASSERT_EQ_I(ray_enc_read(b, 16, RAY_ENC_DICT, 0, 2, k, RAY_ENC_SLOT_TOP, 0, &dict, out), 16);
    uint32_t bad = 1; memcpy(b + 12, &bad, 4);
    TEST_ASSERT_EQ_I(ray_enc_read(b, 16, RAY_ENC_DICT, 0, 2, k, RAY_ENC_SLOT_TOP, 0, &dict, out), -1);
#undef RLE_READ
    PASS();
}

static test_result_t test_read_rejects_bad_child(void) {
    /* An RLE body whose values child claims encoding CONST (illegal as a child). */
    uint8_t body[64] = {0};
    uint32_t nruns = 1; memcpy(body, &nruns, 4);
    body[8] = RAY_ENC_CONST;               /* child header: encoding */
    uint32_t bb = 8; memcpy(body + 12, &bb, 4);
    ray_enc_kind_t k = { 32, true, false };
    uint64_t out[RAY_ENC_VEC];
    TEST_ASSERT_EQ_I(ray_enc_read(body, sizeof body, RAY_ENC_RLE, 0, 4, k, RAY_ENC_SLOT_TOP, 0, NULL, out), -1);
    PASS();
}

/* ---- independent byte-level references (spec "Encodings"), no codec calls ---- */
static uint64_t ref_mask(int T) { return T == 64 ? ~0ull : ((1ull << T) - 1); }

/* Bit-by-bit packer: value i is lane i % L, row i / L; its bit b is bit
 * (row*w + b) of the lane stream, i.e. bit (that % T) of word (that / T),
 * stored at word index k*L + l. */
static void ref_pack(const uint64_t* u, int T, int w, uint8_t* out) {
    int L = RAY_ENC_VEC / T;
    memset(out, 0, (size_t)RAY_ENC_VEC * (size_t)w / 8);
    for (int i = 0; i < RAY_ENC_VEC; i++) {
        int l = i % L, r = i / L;
        for (int b = 0; b < w; b++) {
            if (!((u[i] >> b) & 1)) continue;
            int bit = r * w + b, k = bit / T, o = bit % T;
            size_t byte = ((size_t)k * (size_t)L + (size_t)l) * (size_t)(T / 8) + (size_t)(o / 8);
            out[byte] |= (uint8_t)(1u << (o % 8));
        }
    }
}

static int ref_bitlen(uint64_t x) { int n = 0; while (x) { n++; x >>= 1; } return n; }
static int64_t ref_logical(uint64_t b, int T) {
    b &= ref_mask(T);
    if (T < 64 && ((b >> (T - 1)) & 1)) return (int64_t)(b | ~ref_mask(T));
    return (int64_t)b;
}

/* Expected FOR_BP body for n logical values (padded with base). */
static size_t ref_for_body(const uint64_t* v, int n, int T, uint8_t* exp, int* wout) {
    uint64_t m = ref_mask(T);
    int64_t mn = ref_logical(v[0], T), mx = mn;
    for (int i = 1; i < n; i++) {
        int64_t x = ref_logical(v[i], T);
        if (x < mn) mn = x;
        if (x > mx) mx = x;
    }
    uint64_t base = (uint64_t)mn & m;
    int w = ref_bitlen(((uint64_t)mx - (uint64_t)mn) & m);
    static uint64_t u[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) u[i] = i < n ? ((v[i] - base) & m) : 0;
    memcpy(exp, &base, 8);
    ref_pack(u, T, w, exp + 8);
    *wout = w;
    return 8 + (size_t)RAY_ENC_VEC * (size_t)w / 8;
}

/* Expected DELTA_BP body: v padded with its last value, lane-stride deltas. */
static size_t ref_delta_body(const uint64_t* v, int n, int T, uint8_t* exp, int* wout) {
    uint64_t m = ref_mask(T);
    int L = RAY_ENC_VEC / T;
    static uint64_t p[RAY_ENC_VEC], d[RAY_ENC_VEC], r[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) p[i] = (i < n ? v[i] : v[n - 1]) & m;
    uint64_t db = ~0ull;
    for (int i = L; i < RAY_ENC_VEC; i++) { d[i] = (p[i] - p[i - L]) & m; if (d[i] < db) db = d[i]; }
    uint64_t mx = 0;
    for (int i = 0; i < RAY_ENC_VEC; i++) { r[i] = i < L ? 0 : ((d[i] - db) & m); if (r[i] > mx) mx = r[i]; }
    int w = ref_bitlen(mx);
    memcpy(exp, &db, 8);
    for (int l = 0; l < L; l++) memcpy(exp + 8 + 8 * (size_t)l, &p[l], 8);
    ref_pack(r, T, w, exp + 8 + 8 * (size_t)L);
    *wout = w;
    return 8 + 8 * (size_t)L + (size_t)RAY_ENC_VEC * (size_t)w / 8;
}

static void for_data(uint64_t* v, int n, int T, int64_t lo, uint64_t span, uint64_t seed) {
    for (int i = 0; i < n; i++) v[i] = (uint64_t)(lo + (int64_t)(enc_rng(&seed) % span)) & ref_mask(T);
}
static void delta_data(uint64_t* v, int n, int T) {
    for (int i = 0; i < n; i++) v[i] = (uint64_t)(int64_t)(-500 + 7 * i + (i % 3)) & ref_mask(T);
}

static int layout_case(int want_enc, int T, int n, bool delta, int expect_w) {
    static uint64_t v[RAY_ENC_VEC];
    static uint8_t exp[8 + 8 * 128 + RAY_ENC_VEC * 8];
    ray_enc_kind_t k = { (uint8_t)T, true, false };
    if (delta) delta_data(v, n, T);
    else for_data(v, n, T, -200, expect_w == 9 ? 512 : 1000, 0x1234567ull + (uint64_t)T + (uint64_t)n);
    int w; size_t len = delta ? ref_delta_body(v, n, T, exp, &w) : ref_for_body(v, n, T, exp, &w);
    if (expect_w && w != expect_w) return 1;
    ray_enc_buf_t b = {0};
    ray_enc_choice_t c;
    if (!ray_enc_write(v, n, k, RAY_ENC_SLOT_TOP, 0, NULL, &b, &c)) return 2;
    int rc = 0;
    if (c.enc != want_enc) rc = 3;
    else if (c.width != w) rc = 4;
    else if (b.len != len) rc = 5;
    else if (memcmp(b.p, exp, len)) rc = 6;
    ray_enc_buf_free(&b);
    return rc;
}

static test_result_t test_layout_for_partial(void) {
    static const int Ts[2] = { 32, 64 }, ns[2] = { 500, 700 };
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++)
            TEST_ASSERT_FMT(layout_case(RAY_ENC_FOR_BP, Ts[a], ns[b], false, 0) == 0, "FOR T=%d n=%d", Ts[a], ns[b]);
    TEST_ASSERT_EQ_I(layout_case(RAY_ENC_FOR_BP, 16, 1000, false, 9), 0);   /* straddling width */
    PASS();
}

static test_result_t test_layout_delta_partial(void) {
    static const int Ts[2] = { 32, 64 }, ns[2] = { 500, 700 };
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++)
            TEST_ASSERT_FMT(layout_case(RAY_ENC_DELTA_BP, Ts[a], ns[b], true, 0) == 0, "DELTA T=%d n=%d", Ts[a], ns[b]);
    PASS();
}

/* T=16, w=3, value 5 at lane 5 row 5: lane bits 15..17 straddle words 0 and 1. */
static test_result_t test_pack_layout_straddle(void) {
    uint64_t u[RAY_ENC_VEC] = {0};
    int L = 64;
    u[5 + 5 * L] = 5;            /* 0b101: bit 0 -> word 0 bit 15; bits 1,2 (0b10) -> word 1 bits 0..1 */
    uint16_t words[RAY_ENC_VEC * 3 / 16];
    ray_enc_pack(u, 16, 3, (uint8_t*)words);
    for (int k = 0; k < 3; k++)
        for (int l = 0; l < L; l++) {
            uint16_t want = 0;
            if (k == 0 && l == 5) want = 0x8000;
            if (k == 1 && l == 5) want = 0x0002;
            TEST_ASSERT_FMT(words[k * L + l] == want, "k=%d l=%d got %04x want %04x", k, l, words[k * L + l], want);
        }
    uint64_t back[RAY_ENC_VEC];
    ray_enc_unpack((const uint8_t*)words, 16, 3, back);
    TEST_ASSERT_EQ_U(back[5 + 5 * L], 5);
    PASS();
}

/* Sign-extended input (bits above T set) encodes to the same bytes as the
 * masked input, and the result decodes to the masked values. */
static test_result_t test_encode_sign_extended(void) {
    static uint64_t ext[RAY_ENC_VEC], msk[RAY_ENC_VEC], out[RAY_ENC_VEC];
    for (int ci = 0; ci < 2; ci++) {
        int T = ci ? 32 : 16;
        ray_enc_kind_t k = { (uint8_t)T, true, false };
        for (int delta = 0; delta < 2; delta++) {
            for (int i = 0; i < RAY_ENC_VEC; i++) {
                int64_t x = delta ? -500 + 7 * i + (i % 3) : -(i & 1);
                ext[i] = (uint64_t)x;                    /* sign-extended to 64 bits */
                msk[i] = (uint64_t)x & ref_mask(T);
            }
            ray_enc_buf_t a = {0}, b = {0};
            ray_enc_choice_t ca, cb;
            TEST_ASSERT_TRUE(ray_enc_write(ext, RAY_ENC_VEC, k, RAY_ENC_SLOT_TOP, 0, NULL, &a, &ca));
            TEST_ASSERT_TRUE(ray_enc_write(msk, RAY_ENC_VEC, k, RAY_ENC_SLOT_TOP, 0, NULL, &b, &cb));
            TEST_ASSERT_EQ_I(ca.enc, delta ? RAY_ENC_DELTA_BP : RAY_ENC_FOR_BP);
            TEST_ASSERT_TRUE(ca.enc == cb.enc && a.len == b.len && !memcmp(a.p, b.p, a.len));
            int64_t used = ray_enc_read(a.p, a.len, ca.enc, ca.width, RAY_ENC_VEC, k, RAY_ENC_SLOT_TOP, 0, NULL, out);
            TEST_ASSERT_EQ_I(used, (int64_t)a.len);
            for (int i = 0; i < RAY_ENC_VEC; i++) TEST_ASSERT_EQ_U(out[i], msk[i]);
            ray_enc_buf_free(&a); ray_enc_buf_free(&b);
        }
    }
    /* The small case from the review: I16 {-1, 0}. */
    uint64_t two[2] = { ~0ull, 0 }, o2[2];
    ray_enc_kind_t k16 = { 16, true, false };
    ray_enc_buf_t t = {0}; ray_enc_choice_t ct;
    TEST_ASSERT_TRUE(ray_enc_write(two, 2, k16, RAY_ENC_SLOT_TOP, 0, NULL, &t, &ct));
    TEST_ASSERT_EQ_I(ray_enc_read(t.p, t.len, ct.enc, ct.width, 2, k16, RAY_ENC_SLOT_TOP, 0, NULL, o2), (int64_t)t.len);
    TEST_ASSERT_EQ_U(o2[0], 0xFFFFull);
    TEST_ASSERT_EQ_U(o2[1], 0);
    ray_enc_buf_free(&t);
    PASS();
}

static test_result_t test_load_rejects_bad_n(void) {
    static int32_t d[RAY_ENC_VEC + 8];
    static ray_enc_input_t in;
    TEST_ASSERT_FALSE(ray_enc_load(d, RAY_I32, 0, 0, RAY_ENC_VEC + 1, &in));
    TEST_ASSERT_FALSE(ray_enc_load(d, RAY_I32, 0, 0, -1, &in));
    TEST_ASSERT_TRUE(ray_enc_load(d, RAY_I32, 0, 0, RAY_ENC_VEC, &in));
    TEST_ASSERT_TRUE(ray_enc_load(d, RAY_I32, 0, 0, 0, &in));
    PASS();
}

static test_result_t test_buf_free_clears_oom(void) {
    ray_enc_buf_t b = {0};
    b.oom = true;
    ray_enc_buf_free(&b);
    TEST_ASSERT_FALSE(b.oom);
    TEST_ASSERT_TRUE(ray_enc_buf_put(&b, "x", 1));
    ray_enc_buf_free(&b);
    PASS();
}

static test_result_t test_train_dict_tristate(void) {
    static uint64_t v[RAY_ENC_VEC * 70];
    ray_enc_kind_t k = { 64, true, false };
    uint64_t* d = NULL; uint32_t dn = 0;
    TEST_ASSERT_EQ_I(ray_enc_train_dict(v, 0, k, &d, &dn), 0);
    for (int64_t i = 0; i < (int64_t)RAY_ENC_VEC * 70; i++) v[i] = (uint64_t)i;   /* 71680 > 1<<16 distinct */
    TEST_ASSERT_EQ_I(ray_enc_train_dict(v, (int64_t)RAY_ENC_VEC * 70, k, &d, &dn), 0);
    for (int64_t i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)(i % 5);
    TEST_ASSERT_EQ_I(ray_enc_train_dict(v, RAY_ENC_VEC, k, &d, &dn), 1);
    TEST_ASSERT_EQ_I(dn, 5);
    ray_free_raw(d);
    PASS();
}

/* Child header: encoding, width, bytes of body at +4. */
static size_t put_child(uint8_t* p, uint8_t enc, uint32_t body_bytes) {
    memset(p, 0, 8); p[0] = enc; memcpy(p + 4, &body_bytes, 4);
    return 8;
}

/* DICT is illegal as an RLE values child.  Control pair: a valid RLE body
 * with a RAW values child reads; the same body with the values child's
 * encoding byte set to DICT (codes child well formed) is rejected. */
static test_result_t test_read_rejects_dict_child(void) {
    ray_enc_kind_t k = { 64, true, false };
    uint64_t dv[1] = { 7 };
    ray_enc_dict_t dict = { dv, 1 };
    uint64_t out[RAY_ENC_VEC];
    uint8_t body[96];
    /* control: one run of 4 values, values RAW (one u64), ends RAW U16 (one) */
    memset(body, 0, sizeof body);
    uint32_t nruns = 1; memcpy(body, &nruns, 4);
    size_t at = 8;
    at += put_child(body + at, RAY_ENC_RAW, 8);
    uint64_t seven = 7; memcpy(body + at, &seven, 8); at += 8;
    at += put_child(body + at, RAY_ENC_RAW, (uint32_t)ray_enc_size_raw(1, 16));
    uint16_t end4 = 4; memcpy(body + at, &end4, 2); at += (size_t)ray_enc_size_raw(1, 16);
    int64_t used = ray_enc_read(body, sizeof body, RAY_ENC_RLE, 0, 4, k,
                                RAY_ENC_SLOT_TOP, 0, &dict, out);
    TEST_ASSERT_EQ_I(used, (int64_t)at);
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQ_I(out[i], 7);
    /* variant: values child DICT, its body is a well-formed codes child (RAW U32, one code 0) */
    uint8_t bad[128];
    memset(bad, 0, sizeof bad);
    memcpy(bad, &nruns, 4);
    at = 8;
    uint32_t cbytes = (uint32_t)ray_enc_size_raw(1, 32);
    at += put_child(bad + at, RAY_ENC_DICT, 8 + cbytes);
    at += put_child(bad + at, RAY_ENC_RAW, cbytes);
    at += cbytes;                                   /* code 0 */
    at += put_child(bad + at, RAY_ENC_RAW, (uint32_t)ray_enc_size_raw(1, 16));
    memcpy(bad + at, &end4, 2);
    TEST_ASSERT_EQ_I(ray_enc_read(bad, sizeof bad, RAY_ENC_RLE, 0, 4, k,
                                  RAY_ENC_SLOT_TOP, 0, &dict, out), -1);
    PASS();
}

/* Training sorts in the kind's native order. */
static test_result_t test_train_dict_native_order(void) {
    uint64_t s16[6] = { 0xFFFE, 0x0003, 0x8000, 0x7FFF, 0xFFFE, 0x0000 }; /* -2,3,-32768,32767,-2,0 */
    ray_enc_kind_t ks = { 16, true, false };
    uint64_t* d; uint32_t dn;
    TEST_ASSERT_EQ_I(ray_enc_train_dict(s16, 6, ks, &d, &dn), 1);
    TEST_ASSERT_EQ_I(dn, 5);
    static const uint64_t want_s[5] = { 0x8000, 0xFFFE, 0x0000, 0x0003, 0x7FFF };
    for (int i = 0; i < 5; i++) TEST_ASSERT_FMT(d[i] == want_s[i], "signed i=%d", i);
    ray_free_raw(d);
    uint64_t u8[4] = { 128, 127, 255, 0 };
    ray_enc_kind_t ku = { 8, false, false };
    TEST_ASSERT_EQ_I(ray_enc_train_dict(u8, 4, ku, &d, &dn), 1);
    static const uint64_t want_u[4] = { 0, 127, 128, 255 };
    for (int i = 0; i < 4; i++) TEST_ASSERT_FMT(d[i] == want_u[i], "unsigned i=%d", i);
    ray_free_raw(d);
    PASS();
}

/* Forced writes round-trip and reach what selection never picks. */
static int forced_roundtrip(uint8_t enc, const uint64_t* v, int n, ray_enc_kind_t k,
                            const ray_enc_dict_t* dict, ray_enc_buf_t* b, ray_enc_choice_t* c) {
    b->len = 0;
    if (!ray_enc_write_as(enc, v, n, k, dict, b, c)) return -1;
    if ((int64_t)b->len != c->bytes || c->enc != enc) return -2;
    uint64_t out[RAY_ENC_VEC];
    if (ray_enc_read(b->p, b->len, c->enc, c->width, n, k, RAY_ENC_SLOT_TOP, 0, dict, out) != (int64_t)b->len) return -3;
    uint64_t m = k.T == 64 ? ~0ull : ((1ull << k.T) - 1);
    for (int i = 0; i < n; i++) if (out[i] != (v[i] & m)) return -4;
    return 0;
}

static test_result_t test_write_as_forced(void) {
    static uint64_t v[RAY_ENC_VEC];
    ray_enc_buf_t b = {0};
    ray_enc_choice_t c;
    /* DELTA_BP at w = T: extremes alternating by lane row (row = i / L). */
    static const int Ts[3] = { 16, 32, 64 };
    for (int t = 0; t < 3; t++) {
        int T = Ts[t], L = RAY_ENC_VEC / T;
        uint64_t m = T == 64 ? ~0ull : ((1ull << T) - 1);
        int64_t hi = T == 16 ? 32767 : T == 32 ? 2147483647ll : INT64_MAX;
        for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)(((i / L) & 1) ? hi : -hi) & m;
        ray_enc_kind_t k = { (uint8_t)T, true, false };
        TEST_ASSERT_EQ_I(forced_roundtrip(RAY_ENC_DELTA_BP, v, RAY_ENC_VEC, k, NULL, &b, &c), 0);
        TEST_ASSERT_EQ_I(c.width, T);
        TEST_ASSERT_TRUE(ray_enc_choose(v, RAY_ENC_VEC, k, RAY_ENC_SLOT_TOP, 0, NULL).enc != RAY_ENC_DELTA_BP);
    }
    /* DICT over a constant vector: the codes child is FOR_BP at width 0 (a constant child at depth 1). */
    ray_enc_kind_t k64 = { 64, true, false };
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = 900000000001ull;
    uint64_t dv[2] = { 11, 900000000001ull };
    ray_enc_dict_t dict = { dv, 2 };
    TEST_ASSERT_EQ_I(forced_roundtrip(RAY_ENC_DICT, v, RAY_ENC_VEC, k64, &dict, &b, &c), 0);
    TEST_ASSERT_EQ_I(b.p[0], RAY_ENC_FOR_BP);            /* codes child header */
    TEST_ASSERT_EQ_I(b.p[1], 0);                         /* width 0 */
    /* RLE with FOR_BP values (512 runs of two, 16 distinct values). */
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = 1000 + (uint64_t)((i / 2) % 16);
    TEST_ASSERT_EQ_I(forced_roundtrip(RAY_ENC_RLE, v, RAY_ENC_VEC, k64, NULL, &b, &c), 0);
    TEST_ASSERT_EQ_I(b.p[8], RAY_ENC_FOR_BP);            /* values child */
    /* RLE with DELTA_BP ends (1024 runs of one: ends 1..1024, lane step 64). */
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)i * 3;
    TEST_ASSERT_EQ_I(forced_roundtrip(RAY_ENC_RLE, v, RAY_ENC_VEC, k64, NULL, &b, &c), 0);
    uint32_t vb; memcpy(&vb, b.p + 12, 4);
    TEST_ASSERT_EQ_I(b.p[16 + vb], RAY_ENC_DELTA_BP);    /* ends child */
    /* illegal or inapplicable */
    ray_enc_kind_t ku8 = { 8, false, false };
    TEST_ASSERT_EQ_I(forced_roundtrip(RAY_ENC_DELTA_BP, v, 16, ku8, NULL, &b, &c), -1);  /* unsigned */
    TEST_ASSERT_EQ_I(forced_roundtrip(RAY_ENC_CONST, v, 16, k64, NULL, &b, &c), -1);     /* unequal */
    TEST_ASSERT_EQ_I(forced_roundtrip(RAY_ENC_DICT, v, 16, k64, &dict, &b, &c), -1);     /* not in dict */
    ray_enc_buf_free(&b);
    PASS();
}

/* n is bounded before the stack arrays are touched; a failed write leaves the
 * buffer as it was; bits above T are ignored. */
static test_result_t test_write_bounds_rollback_masking(void) {
    static uint64_t v[RAY_ENC_VEC + 1];
    ray_enc_buf_t b = {0};
    ray_enc_choice_t c;
    ray_enc_kind_t k64 = { 64, true, false };
    TEST_ASSERT_TRUE(!ray_enc_write(v, -1, k64, RAY_ENC_SLOT_TOP, 0, NULL, &b, &c));
    TEST_ASSERT_TRUE(!ray_enc_write(v, RAY_ENC_VEC + 1, k64, RAY_ENC_SLOT_TOP, 0, NULL, &b, &c));
    TEST_ASSERT_TRUE(!ray_enc_write_as(RAY_ENC_RAW, v, -1, k64, NULL, &b, &c));
    TEST_ASSERT_TRUE(!ray_enc_write_as(RAY_ENC_RAW, v, RAY_ENC_VEC + 1, k64, NULL, &b, &c));
    TEST_ASSERT_EQ_I(b.len, 0);
    /* rollback: a successful write first, then a failing forced write appends nothing */
    for (int i = 0; i < 16; i++) v[i] = (uint64_t)i;
    TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_RAW, v, 16, k64, NULL, &b, &c));
    size_t len0 = b.len;
    TEST_ASSERT_TRUE(len0 > 0);
    TEST_ASSERT_TRUE(!ray_enc_write_as(RAY_ENC_CONST, v, 16, k64, NULL, &b, &c));
    TEST_ASSERT_EQ_I(b.len, len0);
    /* sign-extended I16 input encodes like the masked input */
    ray_enc_kind_t k16 = { 16, true, false };
    uint64_t sx[64], mk[64];
    for (int i = 0; i < 64; i++) { int16_t x = (int16_t)(i * 37 - 1000); sx[i] = (uint64_t)(int64_t)x; mk[i] = (uint64_t)(uint16_t)x; }
    ray_enc_buf_t b1 = {0}, b2 = {0};
    TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_FOR_BP, sx, 64, k16, NULL, &b1, &c));
    TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_FOR_BP, mk, 64, k16, NULL, &b2, &c));
    TEST_ASSERT_EQ_I(b1.len, b2.len);
    TEST_ASSERT_TRUE(memcmp(b1.p, b2.p, b1.len) == 0);
    ray_enc_buf_free(&b1); ray_enc_buf_free(&b2); ray_enc_buf_free(&b);
    PASS();
}

const test_entry_t enc_entries[] = {
    { "enc/pack_roundtrip_all_widths", test_pack_roundtrip_all_widths, enc_setup, enc_teardown },
    { "enc/pack_layout_t8_w1",         test_pack_layout_t8_w1,         enc_setup, enc_teardown },
    { "enc/pack_layout_t16_w3",        test_pack_layout_t16_w3,        enc_setup, enc_teardown },
    { "enc/bitlen",                    test_bitlen,                    enc_setup, enc_teardown },
    { "enc/kind_of",                   test_kind_of,                   enc_setup, enc_teardown },
    { "enc/logical_and_order",         test_logical_and_order,         enc_setup, enc_teardown },
    { "enc/load_i64_nulls_fill_min",   test_load_i64_nulls_fill_min,   enc_setup, enc_teardown },
    { "enc/load_all_null_fills_zero",  test_load_all_null_fills_zero,  enc_setup, enc_teardown },
    { "enc/load_sym_w8_inband",        test_load_sym_w8_inband,        enc_setup, enc_teardown },
    { "enc/stats_runs_and_equal",      test_stats_runs_and_equal,      enc_setup, enc_teardown },
    { "enc/load_offset_start",         test_load_offset_start,         enc_setup, enc_teardown },
    { "enc/sizes_raw_const",            test_sizes_raw_const,            enc_setup, enc_teardown },
    { "enc/for_width_signed_extremes",  test_for_width_signed_extremes,  enc_setup, enc_teardown },
    { "enc/for_width_narrow",           test_for_width_narrow,           enc_setup, enc_teardown },
    { "enc/delta_width_sorted",         test_delta_width_sorted,         enc_setup, enc_teardown },
    { "enc/choose_const",               test_choose_const,               enc_setup, enc_teardown },
    { "enc/choose_rle_runs",            test_choose_rle_runs,            enc_setup, enc_teardown },
    { "enc/choose_for",                 test_choose_for,                 enc_setup, enc_teardown },
    { "enc/choose_delta",               test_choose_delta,               enc_setup, enc_teardown },
    { "enc/keep_raw_when_gain_small",   test_keep_raw_when_gain_small,   enc_setup, enc_teardown },
    { "enc/dict_wide_sparse_values",    test_dict_wide_sparse_values,    enc_setup, enc_teardown },
    { "enc/roundtrip_lengths_and_types",test_roundtrip_lengths_and_types,enc_setup, enc_teardown },
    { "enc/i64_extremes_roundtrip",     test_i64_extremes_roundtrip,     enc_setup, enc_teardown },
    { "enc/dict_codes_rle_nested",      test_dict_codes_rle_nested,      enc_setup, enc_teardown },
    { "enc/read_rejects_malformed",     test_read_rejects_malformed,     enc_setup, enc_teardown },
    { "enc/read_rejects_bad_child",     test_read_rejects_bad_child,     enc_setup, enc_teardown },
    { "enc/layout_for_partial",         test_layout_for_partial,         enc_setup, enc_teardown },
    { "enc/layout_delta_partial",       test_layout_delta_partial,       enc_setup, enc_teardown },
    { "enc/pack_layout_straddle",       test_pack_layout_straddle,       enc_setup, enc_teardown },
    { "enc/encode_sign_extended",       test_encode_sign_extended,       enc_setup, enc_teardown },
    { "enc/load_rejects_bad_n",         test_load_rejects_bad_n,         enc_setup, enc_teardown },
    { "enc/buf_free_clears_oom",        test_buf_free_clears_oom,        enc_setup, enc_teardown },
    { "enc/train_dict_tristate",        test_train_dict_tristate,        enc_setup, enc_teardown },
    { "enc/read_rejects_dict_child", test_read_rejects_dict_child, enc_setup, enc_teardown },
    { "enc/train_dict_native_order", test_train_dict_native_order, enc_setup, enc_teardown },
    { "enc/write_as_forced",         test_write_as_forced,         enc_setup, enc_teardown },
    { "enc/write_bounds_rollback_masking", test_write_bounds_rollback_masking, enc_setup, enc_teardown },
    { NULL, NULL, NULL, NULL },
};
