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

const test_entry_t enc_entries[] = {
    { "enc/pack_roundtrip_all_widths", test_pack_roundtrip_all_widths, enc_setup, enc_teardown },
    { "enc/pack_layout_t8_w1",         test_pack_layout_t8_w1,         enc_setup, enc_teardown },
    { "enc/pack_layout_t16_w3",        test_pack_layout_t16_w3,        enc_setup, enc_teardown },
    { "enc/bitlen",                    test_bitlen,                    enc_setup, enc_teardown },
    { NULL, NULL, NULL, NULL },
};
