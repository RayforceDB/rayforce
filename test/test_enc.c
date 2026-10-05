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
    ray_enc_kind_t s16 = { 16, true }, u8 = { 8, false };
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
    ray_enc_kind_t k = { 32, true };
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
    ray_enc_kind_t k = { 64, true };
    uint64_t base;
    int w = ray_enc_for_width(v, 3, k, &base);
    TEST_ASSERT_EQ_U(base, (uint64_t)(INT64_MIN + 1));  /* native min */
    TEST_ASSERT_EQ_I(w, 64);                              /* range 2^64 - 2 */
    TEST_ASSERT_EQ_I(ray_enc_size_for(w), 8 + 8192);
    PASS();
}

static test_result_t test_for_width_narrow(void) {
    uint64_t v[4] = { 0xFFFE, 0xFFFF, 0x0000, 0x0001 };  /* -2,-1,0,1 as I16 */
    ray_enc_kind_t k = { 16, true };
    uint64_t base;
    int w = ray_enc_for_width(v, 4, k, &base);
    TEST_ASSERT_EQ_U(base, 0xFFFE);
    TEST_ASSERT_EQ_I(w, 2);                               /* residuals 0..3 */
    PASS();
}

static test_result_t test_delta_width_sorted(void) {
    static uint64_t v[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)(1000000 + 3 * i);
    ray_enc_kind_t k = { 64, true };
    uint64_t dbase;
    int w = ray_enc_delta_width(v, RAY_ENC_VEC, k, &dbase);
    TEST_ASSERT_EQ_U(dbase, 3 * 16);                      /* lane stride L=16 */
    TEST_ASSERT_EQ_I(w, 0);
    TEST_ASSERT_EQ_I(ray_enc_size_delta(64, 0), 8 + 8 * 16);
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
    { NULL, NULL, NULL, NULL },
};
