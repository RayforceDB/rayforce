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
#include "core/runtime.h"
#include "store/enc.h"
#include "store/encfmt.h"
#include "store/col.h"
#include "store/encw.h"
#include "store/enccol.h"
#include "store/colrange.h"
#include "store/enceval.h"
#include "ops/ops.h"
#include "lang/env.h"
#include "lang/format.h"
#include "table/table.h"
#include "vec/str.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FL_TMP "/tmp/rayforce_encfloat"      /* always through ray_test_tmp (per-process root) */
static void fl_setup(void)    { ray_heap_init(); (void)ray_sym_init(); (void)ray_test_rm_rf(ray_test_tmp(FL_TMP)); (void)ray_test_mkdir_p(ray_test_tmp(FL_TMP)); }
/* Buffers a test owns, freed in teardown so an early-returning assertion
 * leaks nothing. */
static ray_enc_buf_t fl_buf;
static uint64_t*     fl_dict;
static uint8_t*      fl_mem;                 /* malloc'd scratch */
static ray_t*        fl_vec;                 /* a test's column */
static ray_t*        fl_map;                 /* a test's mapped generation-2 column */
static ray_enc_vview_t fl_vw;                /* a test's vector view (about 21 KB) */
static void fl_teardown(void) {
    ray_encw_test_force(-1);
    if (fl_vec) { ray_release(fl_vec); fl_vec = NULL; }
    if (fl_map) { ray_release(fl_map); fl_map = NULL; }
    ray_enc_buf_free(&fl_buf); fl_buf = (ray_enc_buf_t){0};
    if (fl_dict) { ray_free_raw(fl_dict); fl_dict = NULL; }
    free(fl_mem); fl_mem = NULL;
    ray_test_hooks_off(); (void)ray_test_rm_rf(ray_test_tmp(FL_TMP)); ray_sym_destroy(); ray_heap_destroy();
}

static uint64_t db(double x) { uint64_t b; memcpy(&b, &x, 8); return b; }
static uint64_t fb(float x)  { uint32_t b; memcpy(&b, &x, 4); return b; }

/* -0.0 as bit patterns: independent of the build's float flags (D10). */
#define NEG0_64 (1ull << 63)
#define NEG0_32 (1ull << 31)

static const ray_enc_kind_t K64F = { 64, false, true };
static const ray_enc_kind_t K32F = { 32, false, true };

/* Dictionary order: numeric, -0.0 before +0.0, F32 widened. */
static test_result_t test_float_order(void) {
    TEST_ASSERT_TRUE(ray_enc_lt(db(-1.0), NEG0_64, K64F));
    TEST_ASSERT_TRUE(ray_enc_lt(NEG0_64, db(0.0), K64F));
    TEST_ASSERT_FALSE(ray_enc_lt(db(0.0), NEG0_64, K64F));
    TEST_ASSERT_FALSE(ray_enc_lt(db(0.0), db(0.0), K64F));
    TEST_ASSERT_TRUE(ray_enc_lt(db(0.0), db(1e-300), K64F));
    TEST_ASSERT_TRUE(ray_enc_lt(NEG0_32, fb(0.0f), K32F));
    TEST_ASSERT_TRUE(ray_enc_lt(fb(-2.5f), fb(1.0f), K32F));
    ray_enc_kind_t k;
    TEST_ASSERT_TRUE(ray_enc_value_kind(RAY_F64, 0, &k));
    TEST_ASSERT_TRUE(k.T == 64 && k.is_float && !k.is_signed);
    TEST_ASSERT_TRUE(ray_enc_value_kind(RAY_I32, 0, &k));
    TEST_ASSERT_TRUE(k.T == 32 && !k.is_float && k.is_signed);
    TEST_ASSERT_FALSE(ray_enc_value_kind(RAY_STR, 0, &k));
    PASS();
}

/* Training keeps both zeros as distinct entries, -0.0 first. */
static test_result_t test_float_train_dict(void) {
    uint64_t v[7] = { db(2.25), db(0.0), db(-3.5), NEG0_64, db(0.0), db(2.25), NEG0_64 };
    uint64_t* d = NULL; uint32_t dn;
    int rc = ray_enc_train_dict(v, 7, K64F, &d, &dn);
    if (rc == 1) fl_dict = d;                                   /* freed in teardown */
    TEST_ASSERT_EQ_I(rc, 1);
    TEST_ASSERT_EQ_I(dn, 4);
    TEST_ASSERT_EQ_U(d[0], db(-3.5)); TEST_ASSERT_EQ_U(d[1], NEG0_64);
    TEST_ASSERT_EQ_U(d[2], db(0.0));  TEST_ASSERT_EQ_U(d[3], db(2.25));
    PASS();
}

/* Float CONST, RLE and DICT bodies through the integer codec, bit-exact;
 * RLE values are RAW only; FOR_BP / DELTA_BP are illegal on floats. */
static test_result_t test_float_bodies(void) {
    uint64_t v[RAY_ENC_VEC], out[RAY_ENC_VEC];
    ray_enc_buf_t* bp = &fl_buf;                                /* freed in teardown */
    ray_enc_choice_t c;
    /* CONST: all -0.0, bit-exact (never folded into +0.0) */
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = NEG0_64;
    TEST_ASSERT_TRUE(ray_enc_body_size(RAY_ENC_CONST, v, RAY_ENC_VEC, K64F, NULL) > 0);
    TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_CONST, v, RAY_ENC_VEC, K64F, NULL, bp, &c));
    memset(out, 0, sizeof out);
    TEST_ASSERT_EQ_I(ray_enc_read(bp->p, bp->len, RAY_ENC_CONST, 0, RAY_ENC_VEC, K64F, RAY_ENC_SLOT_TOP, 0, NULL, out), (int64_t)bp->len);
    TEST_ASSERT_TRUE(memcmp(v, out, sizeof v) == 0);
    bp->len = 0;
    {   /* F32 CONST of -0.0 */
        ray_enc_kind_t k32 = K32F;
        for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = NEG0_32;
        TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_CONST, v, RAY_ENC_VEC, k32, NULL, bp, &c));
        memset(out, 0, sizeof out);
        TEST_ASSERT_EQ_I(ray_enc_read(bp->p, bp->len, RAY_ENC_CONST, 0, RAY_ENC_VEC, k32, RAY_ENC_SLOT_TOP, 0, NULL, out), (int64_t)bp->len);
        for (int i = 0; i < RAY_ENC_VEC; i++) TEST_ASSERT_EQ_U(out[i] & 0xffffffffull, NEG0_32);
        bp->len = 0;
    }
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (i / 100) & 1 ? NEG0_64 : db(0.0);
    TEST_ASSERT_TRUE(ray_enc_body_size(RAY_ENC_RLE, v, RAY_ENC_VEC, K64F, NULL) > 0);
    TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_RLE, v, RAY_ENC_VEC, K64F, NULL, bp, &c));
    TEST_ASSERT_EQ_I(bp->p[8], RAY_ENC_RAW);                           /* values child */
    TEST_ASSERT_EQ_I(ray_enc_read(bp->p, bp->len, RAY_ENC_RLE, 0, RAY_ENC_VEC, K64F, RAY_ENC_SLOT_TOP, 0, NULL, out), (int64_t)bp->len);
    TEST_ASSERT_TRUE(memcmp(v, out, sizeof v) == 0);
    /* a FOR_BP values child is corrupt on floats */
    bp->p[8] = RAY_ENC_FOR_BP;
    TEST_ASSERT_EQ_I(ray_enc_read(bp->p, bp->len, RAY_ENC_RLE, 0, RAY_ENC_VEC, K64F, RAY_ENC_SLOT_TOP, 0, NULL, out), -1);
    bp->len = 0;
    uint64_t dv[4] = { db(-3.5), NEG0_64, db(0.0), db(2.25) };
    ray_enc_dict_t dict = { dv, 4 };
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = dv[(i * 7) % 4];
    TEST_ASSERT_TRUE(ray_enc_body_size(RAY_ENC_DICT, v, RAY_ENC_VEC, K64F, &dict) > 0);
    TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_DICT, v, RAY_ENC_VEC, K64F, &dict, bp, &c));
    TEST_ASSERT_EQ_I(ray_enc_read(bp->p, bp->len, RAY_ENC_DICT, 0, RAY_ENC_VEC, K64F, RAY_ENC_SLOT_TOP, 0, &dict, out), (int64_t)bp->len);
    TEST_ASSERT_TRUE(memcmp(v, out, sizeof v) == 0);
    bp->len = 0;
    TEST_ASSERT_FALSE(ray_enc_write_as(RAY_ENC_FOR_BP, v, RAY_ENC_VEC, K64F, NULL, bp, &c));
    TEST_ASSERT_FALSE(ray_enc_write_as(RAY_ENC_DELTA_BP, v, RAY_ENC_VEC, K64F, NULL, bp, &c));
    TEST_ASSERT_EQ_I(ray_enc_body_size(RAY_ENC_FOR_BP, v, RAY_ENC_VEC, K64F, NULL), -1);
    TEST_ASSERT_EQ_I(ray_enc_body_size(RAY_ENC_CONST, v, RAY_ENC_VEC, K64F, NULL), -1);
    PASS();
}

/* The new slots: ALP ints (I64, spec erratum E-P5-1) admit DELTA_BP, RLE
 * and a signed FOR_BP, ALP_RD left (U8) admits RLE and FOR_BP and not
 * DELTA_BP, and the reader enforces it. */
static test_result_t test_new_slots(void) {
    uint64_t v[RAY_ENC_VEC];
    ray_enc_kind_t k64 = { 64, true, false }, k8 = { 8, false, false };
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = 1000000 + 10 * (uint64_t)i;    /* lane-linear: DELTA_BP w=0 */
    ray_enc_choice_t c = ray_enc_choose(v, RAY_ENC_VEC, k64, RAY_ENC_SLOT_ALP_INTS, 1, NULL);
    TEST_ASSERT_EQ_I(c.enc, RAY_ENC_DELTA_BP);
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)((i / 256) % 3);
    c = ray_enc_choose(v, RAY_ENC_VEC, k8, RAY_ENC_SLOT_ALP_RD_LEFT, 1, NULL);
    TEST_ASSERT_EQ_I(c.enc, RAY_ENC_RLE);
    /* four long runs of widely spread values: RLE wins in the ints slot */
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = 0x0123456789abull * (uint64_t)((i / 256) * 3 + 1);
    c = ray_enc_choose(v, RAY_ENC_VEC, k64, RAY_ENC_SLOT_ALP_INTS, 1, NULL);
    TEST_ASSERT_EQ_I(c.enc, RAY_ENC_RLE);
    /* mixed-sign integers in -1000..1000: FOR_BP in signed order, width 11 */
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)(int64_t)((i * 7919) % 2001 - 1000);
    c = ray_enc_choose(v, RAY_ENC_VEC, k64, RAY_ENC_SLOT_ALP_INTS, 1, NULL);
    TEST_ASSERT_EQ_I(c.enc, RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(c.width, 11);
    /* 3-bit values: FOR_BP w=3 in the left slot (DELTA_BP is illegal there,
     * and at T = 8 its 128 eight-byte starts exceed RAW anyway) */
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)(i & 7);
    c = ray_enc_choose(v, RAY_ENC_VEC, k8, RAY_ENC_SLOT_ALP_RD_LEFT, 1, NULL);
    TEST_ASSERT_EQ_I(c.enc, RAY_ENC_FOR_BP);
    /* that FOR_BP body round-trips in the left slot ... */
    ray_enc_buf_t* bp = &fl_buf;                                /* freed in teardown */
    TEST_ASSERT_TRUE(ray_enc_write(v, RAY_ENC_VEC, k8, RAY_ENC_SLOT_ALP_RD_LEFT, 1, NULL, bp, &c));
    TEST_ASSERT_EQ_I(c.enc, RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(bp->p[0], RAY_ENC_FOR_BP);                  /* child header: enc, width */
    uint64_t out[RAY_ENC_VEC];
    TEST_ASSERT_EQ_I(ray_enc_read(bp->p, bp->len, 0, 0, RAY_ENC_VEC, k8, RAY_ENC_SLOT_ALP_RD_LEFT, 1, NULL, out), (int64_t)bp->len);
    TEST_ASSERT_TRUE(memcmp(v, out, sizeof v) == 0);
    /* ... and the reader rejects it as illegal under a DELTA_BP header in
     * the left slot, and under a DICT header in either slot */
    bp->p[0] = RAY_ENC_DELTA_BP;
    TEST_ASSERT_EQ_I(ray_enc_read(bp->p, bp->len, 0, 0, RAY_ENC_VEC, k8, RAY_ENC_SLOT_ALP_RD_LEFT, 1, NULL, out), -1);
    TEST_ASSERT_TRUE(strstr(ray_enc_read_why(), "illegal") != NULL);
    bp->p[0] = RAY_ENC_DICT;
    TEST_ASSERT_EQ_I(ray_enc_read(bp->p, bp->len, 0, 0, RAY_ENC_VEC, k8, RAY_ENC_SLOT_ALP_RD_LEFT, 1, NULL, out), -1);
    TEST_ASSERT_TRUE(strstr(ray_enc_read_why(), "illegal") != NULL);
    TEST_ASSERT_EQ_I(ray_enc_read(bp->p, bp->len, 0, 0, RAY_ENC_VEC, k64, RAY_ENC_SLOT_ALP_INTS, 1, NULL, out), -1);
    TEST_ASSERT_TRUE(strstr(ray_enc_read_why(), "illegal") != NULL);
    PASS();
}

/* xorshift64 for random float data (the ALP_RD tests). */
static uint64_t g_fseed = 7;
static uint64_t frng(void) { uint64_t x = g_fseed; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return g_fseed = x; }

static int alp_roundtrip(const uint64_t* v, int n, int T, const ray_enc_alp_cands_t* c, int64_t* size_out) {
    int bi;
    int64_t sz = ray_enc_alp_size(v, n, T, c, &bi);
    if (sz < 0) return -1;
    ray_enc_buf_t b = {0};
    if (!ray_enc_alp_write(v, n, T, c->c[bi].e, c->c[bi].f, &b)) return -2;
    if ((int64_t)b.len != sz) { ray_enc_buf_free(&b); return -3; }
    uint64_t out[RAY_ENC_VEC];
    int64_t u = ray_enc_alp_read(b.p, b.len, n, T, out);
    ray_enc_buf_free(&b);
    if (u != sz) return -4;
    uint64_t m = T == 64 ? ~0ull : 0xFFFFFFFFull;
    for (int i = 0; i < n; i++) if (out[i] != (v[i] & m)) return -5;
    if (size_out) *size_out = sz;
    return 0;
}

/* Two-decimal prices: ALP with e - f = 2 wins by far over RAW. */
static test_result_t test_alp_decimals(void) {
    uint64_t v[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = db((double)((i * 7919) % 100000) / 100.0);
    ray_enc_alp_cands_t c;
    ray_enc_alp_train(v, RAY_ENC_VEC, 64, &c);
    TEST_ASSERT_TRUE(c.n >= 1);
    int64_t sz;
    TEST_ASSERT_EQ_I(alp_roundtrip(v, RAY_ENC_VEC, 64, &c, &sz), 0);
    TEST_ASSERT_TRUE(sz * 8 <= 8192 * 7);
    /* the pick is pinned: (2, 0) loses to rounding (n * 0.01 is not n / 100),
     * (14, 12) is the smallest pair exact on every sample */
    TEST_ASSERT_EQ_I(c.n, 1);
    TEST_ASSERT_TRUE(c.c[0].e == 14 && c.c[0].f == 12);
    /* F32 tenths */
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = fb((float)((i * 31) % 5000) / 10.0f);
    ray_enc_alp_train(v, RAY_ENC_VEC, 32, &c);
    TEST_ASSERT_TRUE(c.n >= 1);
    TEST_ASSERT_EQ_I(alp_roundtrip(v, RAY_ENC_VEC, 32, &c, &sz), 0);
    TEST_ASSERT_TRUE(sz * 8 <= 4096 * 7);
    /* pinned: (2, 1) is exact on the 32 samples and ties keep the smaller pair */
    TEST_ASSERT_EQ_I(c.n, 1);
    TEST_ASSERT_TRUE(c.c[0].e == 2 && c.c[0].f == 1);
    /* training is deterministic */
    ray_enc_alp_cands_t c2;
    ray_enc_alp_train(v, RAY_ENC_VEC, 32, &c2);
    TEST_ASSERT_TRUE(c.n == c2.n && memcmp(c.c, c2.c, sizeof c.c[0] * (size_t)c.n) == 0);
    PASS();
}

/* Mixed-sign two-decimal values in [-10, 10]: the signed ints child packs
 * them at their real width (spec erratum E-P5-1), far below RAW. */
static test_result_t test_alp_signed(void) {
    uint64_t v[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = db((double)((i * 7919) % 2001 - 1000) / 100.0);
    ray_enc_alp_cands_t c;
    ray_enc_alp_train(v, RAY_ENC_VEC, 64, &c);
    TEST_ASSERT_TRUE(c.n >= 1);
    int64_t sz;
    TEST_ASSERT_EQ_I(alp_roundtrip(v, RAY_ENC_VEC, 64, &c, &sz), 0);
    TEST_ASSERT_TRUE(sz * 8 <= 8192 * 4);
    /* F32 the same */
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = fb((float)((i * 7919) % 2001 - 1000) / 100.0f);
    ray_enc_alp_train(v, RAY_ENC_VEC, 32, &c);
    TEST_ASSERT_TRUE(c.n >= 1);
    TEST_ASSERT_EQ_I(alp_roundtrip(v, RAY_ENC_VEC, 32, &c, &sz), 0);
    TEST_ASSERT_TRUE(sz * 8 <= 4096 * 4);
    PASS();
}

/* Review focus 3: edges are exact or exceptions, never wrong values.
 * -0.0 and NaNs are bit patterns (D10). */
static test_result_t test_alp_edges(void) {
    static const double edge[] = { 0.0, 1e300, -1e300, 1e-300, 4.9e-324, INFINITY, -INFINITY,
                                   9.0e18, -9.0e18, 8.999999999999999e17, 0.1, 1.0 / 3.0, -2.5, 123456789.123 };
    static const uint64_t ebits[] = { NEG0_64, 0x7ff8000000000000ull /* canonical NaN */,
                                      0x7ff8000000000123ull /* payload */, 0xfff8000000000000ull /* negative */,
                                      0x7ff0000000000001ull /* signalling */, 0xfff00000deadbeefull };
    enum { NE = sizeof edge / sizeof edge[0], NB = sizeof ebits / sizeof ebits[0] };
    uint64_t v[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = i % (NE + NB) < NE ? db(edge[i % (NE + NB)]) : ebits[i % (NE + NB) - NE];
    ray_enc_alp_cands_t c = { { { 2, 0 }, { 14, 0 }, { 18, 18 }, { 0, 0 }, { 3, 1 } }, 5 };
    TEST_ASSERT_EQ_I(alp_roundtrip(v, RAY_ENC_VEC, 64, &c, NULL), 0);
    for (int n = 1; n <= 1024; n = n * 3 + 1) TEST_ASSERT_EQ_I(alp_roundtrip(v, n, 64, &c, NULL), 0);
    static const uint64_t fedge[] = { NEG0_32, 0, 0, 0, 0, 0, 0x7fc00000u, 0x7fc00123u, 0xffc00000u, 0x7f800001u };
    static const float fval[] = { 0.0f, 16777217.0f, 3.4e38f, 1e-38f, 0.1f, -7.25f };
    for (int i = 0; i < RAY_ENC_VEC; i++) {
        int k = i % 10;
        v[i] = k >= 1 && k <= 5 ? fb(fval[k]) : fedge[k];           /* k 1..5: values, else bit patterns */
    }
    TEST_ASSERT_EQ_I(alp_roundtrip(v, RAY_ENC_VEC, 32, &c, NULL), 0);
    /* every value an exception: still legal */
    for (int i = 0; i < 16; i++) v[i] = i & 1 ? NEG0_64 : 0x7ff8000000000123ull;
    TEST_ASSERT_EQ_I(alp_roundtrip(v, 16, 64, &c, NULL), 0);
    PASS();
}

/* Grammar rules: e <= 18, f <= e, positions ascending below n, padding. */
static test_result_t test_alp_rejects(void) {
    uint64_t v[64];
    for (int i = 0; i < 64; i++) v[i] = i % 9 == 0 ? NEG0_64 : db((double)i * 0.25);
    ray_enc_buf_t* bp = &fl_buf;                                /* freed in teardown */
    TEST_ASSERT_TRUE(ray_enc_alp_write(v, 64, 64, 2, 0, bp));
    uint64_t out[RAY_ENC_VEC];
    TEST_ASSERT_EQ_I(ray_enc_alp_read(bp->p, bp->len, 64, 64, out), (int64_t)bp->len);
    uint8_t* m = fl_mem = malloc(bp->len);                      /* freed in teardown */
    TEST_ASSERT_NOT_NULL(m);
    memcpy(m, bp->p, bp->len); m[0] = 19;                     TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, 64, out), -1);
    memcpy(m, bp->p, bp->len); m[1] = 3;                      TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, 64, out), -1);
    memcpy(m, bp->p, bp->len); m[5] = 1;                      TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, 64, out), -1);
    memcpy(m, bp->p, bp->len); { uint16_t x = 65; memcpy(m + 2, &x, 2); } TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, 64, out), -1);
    /* positions: the first two swapped */
    uint32_t cb; memcpy(&cb, bp->p + 12, 4);
    size_t pos_at = 8 + 8 + cb;
    memcpy(m, bp->p, bp->len); { uint16_t a, c2; memcpy(&a, m + pos_at, 2); memcpy(&c2, m + pos_at + 2, 2);
                                 memcpy(m + pos_at, &c2, 2); memcpy(m + pos_at + 2, &a, 2); }
    TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, 64, out), -1);
    TEST_ASSERT_EQ_I(ray_enc_alp_read(bp->p, bp->len - 8, 64, 64, out), -1);      /* truncated */
    /* the last position at n */
    {   uint32_t ne = 8; size_t last = pos_at + 2 * (ne - 1);
        memcpy(m, bp->p, bp->len); uint16_t q = 64; memcpy(m + last, &q, 2);
        TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, 64, out), -1); }
    /* padding: 5 exceptions leave 6 pad bytes after the positions and, at
     * F32, 4 pad bytes after the raw elements; any non-zero pad is corrupt */
    for (int T = 32; T <= 64; T += 32) {
        for (int i = 0; i < 64; i++)
            v[i] = i % 13 == 0 ? (T == 64 ? NEG0_64 : NEG0_32) : (T == 64 ? db((double)i * 0.25) : fb((float)i * 0.25f));
        bp->len = 0;
        TEST_ASSERT_TRUE(ray_enc_alp_write(v, 64, T, 2, 0, bp));
        TEST_ASSERT_EQ_I(bp->p[2], 5);                                           /* n_exc */
        TEST_ASSERT_EQ_I(ray_enc_alp_read(bp->p, bp->len, 64, T, out), (int64_t)bp->len);
        free(fl_mem); m = fl_mem = malloc(bp->len);
        TEST_ASSERT_NOT_NULL(m);
        memcpy(&cb, bp->p + 12, 4);
        pos_at = 8 + 8 + cb;
        size_t raw_at = pos_at + 16, raw_len = 5u * (size_t)(T / 8);
        TEST_ASSERT_EQ_I((int64_t)bp->len, (int64_t)(raw_at + ((raw_len + 7) & ~(size_t)7)));
        memcpy(m, bp->p, bp->len); m[pos_at + 10] = 1;  TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, T, out), -1);
        memcpy(m, bp->p, bp->len); m[pos_at + 15] = 0x80; TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, T, out), -1);
        if (T == 32) {
            memcpy(m, bp->p, bp->len); m[raw_at + raw_len] = 1; TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, T, out), -1);
            memcpy(m, bp->p, bp->len); m[raw_at + raw_len + 3] = 1; TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, T, out), -1);
        }
        /* a position at n, and one past it */
        memcpy(m, bp->p, bp->len); { uint16_t q = 64; memcpy(m + pos_at + 8, &q, 2); }
        TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, T, out), -1);
        memcpy(m, bp->p, bp->len); { uint16_t q = 1000; memcpy(m + pos_at + 8, &q, 2); }
        TEST_ASSERT_EQ_I(ray_enc_alp_read(m, bp->len, 64, T, out), -1);
    }
    /* argument rules: T, candidate count and pairs */
    TEST_ASSERT_FALSE(ray_enc_alp_write(v, 64, 16, 2, 0, bp));
    TEST_ASSERT_EQ_I(ray_enc_alp_read(bp->p, bp->len, 64, 16, out), -1);
    int bi;
    ray_enc_alp_cands_t bad = { { { 2, 0 } }, RAY_ENC_ALP_CANDS + 1 };
    TEST_ASSERT_EQ_I(ray_enc_alp_size(v, 64, 64, &bad, &bi), -1);
    bad.n = 1; bad.c[0].e = 19; bad.c[0].f = 0;
    TEST_ASSERT_EQ_I(ray_enc_alp_size(v, 64, 64, &bad, &bi), -1);
    bad.c[0].e = 2; bad.c[0].f = 3;
    TEST_ASSERT_EQ_I(ray_enc_alp_size(v, 64, 64, &bad, &bi), -1);
    bad.c[0].f = 0;
    TEST_ASSERT_TRUE(ray_enc_alp_size(v, 64, 64, &bad, &bi) > 0);
    TEST_ASSERT_EQ_I(ray_enc_alp_size(v, 64, 16, &bad, &bi), -1);
    PASS();
}

/* ---- ALP_RD ---- */
static int alprd_roundtrip(const uint64_t* v, int n, int T, int64_t* sz) {
    int64_t s = ray_enc_alprd_size(v, n, T);
    ray_enc_buf_t b = {0};
    if (!ray_enc_alprd_write(v, n, T, &b)) { ray_enc_buf_free(&b); return -1; }
    if ((int64_t)b.len != s) { ray_enc_buf_free(&b); return -2; }
    uint64_t out[RAY_ENC_VEC];
    int64_t u = ray_enc_alprd_read(b.p, b.len, n, T, out);
    ray_enc_buf_free(&b);
    if (u != s) return -3;
    uint64_t m = T == 64 ? ~0ull : 0xFFFFFFFFull;
    for (int i = 0; i < n; i++) if (out[i] != (v[i] & m)) return -4;
    if (sz) *sz = s;
    return 0;
}

/* Review focus 4: lengths 1, 1023, 1024; F64 in [1, 2) and F32. */
static test_result_t test_alprd_lengths(void) {
    g_fseed = 7;                                                   /* the same data under any filter */
    uint64_t v[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = db(1.0 + (double)(frng() >> 11) / 9007199254740992.0);
    int64_t sz;
    TEST_ASSERT_EQ_I(alprd_roundtrip(v, RAY_ENC_VEC, 64, &sz), 0);
    TEST_ASSERT_TRUE(sz * 8 <= 8192 * 7);                          /* left parts: one sign/exponent */
    TEST_ASSERT_EQ_I(alprd_roundtrip(v, 1, 64, NULL), 0);
    TEST_ASSERT_EQ_I(alprd_roundtrip(v, 1023, 64, NULL), 0);
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = frng() & 0x7FEFFFFFFFFFFFFFull;   /* random non-NaN bits */
    TEST_ASSERT_EQ_I(alprd_roundtrip(v, RAY_ENC_VEC, 64, NULL), 0);    /* many exceptions */
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = fb(1.0f + (float)(frng() >> 41) / 8388608.0f);
    TEST_ASSERT_EQ_I(alprd_roundtrip(v, RAY_ENC_VEC, 32, NULL), 0);
    TEST_ASSERT_EQ_I(alprd_roundtrip(v, 1, 32, NULL), 0);
    PASS();
}

static test_result_t test_alprd_rejects(void) {
    uint64_t v[64];
    for (int i = 0; i < 64; i++) v[i] = db(i == 5 ? -123.0 : 1.0 + i / 64.0);   /* row 5: another sign */
    ray_enc_buf_t* bp = &fl_buf;                                   /* freed in teardown */
    TEST_ASSERT_TRUE(ray_enc_alprd_write(v, 64, 64, bp));
    uint64_t out[RAY_ENC_VEC];
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(bp->p, bp->len, 64, 64, out), (int64_t)bp->len);
    uint8_t rb = bp->p[0], dn = bp->p[1];
    uint8_t* m = fl_mem = malloc(bp->len);                         /* freed in teardown */
    TEST_ASSERT_NOT_NULL(m);
    memcpy(m, bp->p, bp->len); m[0] = 47;              TEST_ASSERT_EQ_I(ray_enc_alprd_read(m, bp->len, 64, 64, out), -1);   /* < T-16 */
    memcpy(m, bp->p, bp->len); m[0] = 64;              TEST_ASSERT_EQ_I(ray_enc_alprd_read(m, bp->len, 64, 64, out), -1);   /* > T-1 */
    memcpy(m, bp->p, bp->len); m[1] = 0;               TEST_ASSERT_EQ_I(ray_enc_alprd_read(m, bp->len, 64, 64, out), -1);
    memcpy(m, bp->p, bp->len); m[1] = 9;               TEST_ASSERT_EQ_I(ray_enc_alprd_read(m, bp->len, 64, 64, out), -1);
    memcpy(m, bp->p, bp->len); m[4] = 1;               TEST_ASSERT_EQ_I(ray_enc_alprd_read(m, bp->len, 64, 64, out), -1);
    TEST_ASSERT_TRUE(dn >= 2);            /* row 5's sign differs at every right_bits: two left parts */
    memcpy(m, bp->p, bp->len); memcpy(m + 8, bp->p + 10, 2); memcpy(m + 10, bp->p + 8, 2);
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(m, bp->len, 64, 64, out), -1);                               /* dict not ascending */
    /* a non-zero pad bit in the packed right parts (row 1023's lane word) */
    uint32_t lb; memcpy(&lb, bp->p + 8 + ((2 * dn + 7) & ~7) + 4, 4);
    size_t packed_at = 8 + ((2 * dn + 7) & ~7) + 8 + lb;
    size_t packed_len = ray_enc_packed_bytes(rb);
    memcpy(m, bp->p, bp->len); m[packed_at + packed_len - 1] |= 0x80;
    memset(out, 0xAA, sizeof out);
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(m, bp->len, 64, 64, out), -1);
    for (int i = 0; i < RAY_ENC_VEC; i++) TEST_ASSERT_EQ_U(out[i], 0);                             /* never partly decoded */
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(bp->p, bp->len - 8, 64, 64, out), -1);                       /* truncated */
    /* argument rules: T and n */
    TEST_ASSERT_EQ_I(ray_enc_alprd_size(v, 64, 16), -1);
    TEST_ASSERT_EQ_I(ray_enc_alprd_size(v, 0, 64), -1);
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(bp->p, bp->len, 64, 16, out), -1);
    size_t len = bp->len;
    TEST_ASSERT_FALSE(ray_enc_alprd_write(v, 64, 16, bp));
    TEST_ASSERT_EQ_I((int64_t)bp->len, (int64_t)len);                                                /* no partial body */
    PASS();
}

/* Exception framing on a hand-built body (spec "Reject rules": ALP/ALP_RD
 * exception framing; Review focus 4): T = 64, right_bits = 52, dict
 * {0x3FF, 0x400}, four rows, rows 2 and 3 exceptions with raw left parts
 * 0x3FE and 0x401. */
static size_t alprd_body(uint8_t* b, const uint8_t codes[4], const uint16_t pos[2], const uint16_t lefts[2]) {
    memset(b, 0, 8 + 8 + 16 + 6656 + 8 + 8);
    b[0] = 52; b[1] = 2; uint16_t ne = 2; memcpy(b + 2, &ne, 2);
    uint16_t dict[2] = { 0x3FF, 0x400 }; memcpy(b + 8, dict, 4);          /* dict, padded to 8 */
    uint32_t bb = 8;                                                       /* left child: RAW, 4 codes + pad */
    b[16] = RAY_ENC_RAW; memcpy(b + 20, &bb, 4); memcpy(b + 24, codes, 4);
    size_t at = 32 + ray_enc_packed_bytes(52);                             /* right parts: all zero */
    memcpy(b + at, pos, 4);   at += 8;
    memcpy(b + at, lefts, 4); at += 8;
    return at;
}
static test_result_t test_alprd_exceptions(void) {
    static uint8_t b[8 + 8 + 16 + 6656 + 8 + 8];
    uint8_t codes[4] = { 0, 1, 0, 0 };
    uint16_t pos[2] = { 2, 3 }, lefts[2] = { 0x3FE, 0x401 };
    uint64_t out[RAY_ENC_VEC];
    size_t n = alprd_body(b, codes, pos, lefts);
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), (int64_t)n);
    TEST_ASSERT_EQ_U(out[0], 0x3FFull << 52); TEST_ASSERT_EQ_U(out[1], 0x400ull << 52);
    TEST_ASSERT_EQ_U(out[2], 0x3FEull << 52); TEST_ASSERT_EQ_U(out[3], 0x401ull << 52);
    uint8_t c2[4] = { 0, 1, 1, 0 };                                        /* exception row with code 1 */
    n = alprd_body(b, c2, pos, lefts);
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), -1);
    TEST_ASSERT_STR_EQ(ray_enc_read_why(), "ALP_RD exception row with a non-zero left code");   /* D8 */
    uint16_t big[2] = { 0x1000, 0x401 };                                   /* raw left at 2^(T-right_bits) */
    n = alprd_body(b, codes, pos, big);
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), -1);
    uint16_t swapped[2] = { 3, 2 };                                        /* positions not ascending */
    n = alprd_body(b, codes, swapped, lefts);
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), -1);
    uint16_t past[2] = { 2, 4 };                                           /* position at n */
    n = alprd_body(b, codes, past, lefts);
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), -1);
    uint8_t c3[4] = { 0, 2, 0, 0 };                                        /* code >= dict_n, not an exception row */
    n = alprd_body(b, c3, pos, lefts);
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), -1);
    /* single-byte forgeries of the valid body: offsets 8 dict, 16 child,
     * 32 right parts, 32 + 6656 positions, + 8 lefts */
    const size_t pos_at = 32 + 6656, lefts_at = pos_at + 8;
    n = alprd_body(b, codes, pos, lefts);
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), (int64_t)n);
    { uint16_t x = 5; memcpy(b + 2, &x, 2); }                              /* n_exc > n */
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), -1);
    n = alprd_body(b, codes, pos, lefts);
    { uint16_t x = 0x1000; memcpy(b + 10, &x, 2); }                        /* dict entry at 2^(T-right_bits) */
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), -1);
    n = alprd_body(b, codes, pos, lefts); b[12] = 1;                       /* dict pad */
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), -1);
    n = alprd_body(b, codes, pos, lefts); b[pos_at + 5] = 1;               /* positions pad */
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), -1);
    n = alprd_body(b, codes, pos, lefts); b[lefts_at + 7] = 0x80;          /* lefts pad */
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), -1);
    n = alprd_body(b, codes, pos, lefts);
    TEST_ASSERT_EQ_I(ray_enc_alprd_read(b, n, 4, 64, out), (int64_t)n);    /* the base body still reads */
    PASS();
}

/* Bit-exact on special values (spec "P5": NaN payloads included), all as
 * bit patterns (D10): NaNs with payloads and both signs, +-inf, +-0.0,
 * denormals, extremes, ordinary values of both signs. */
static test_result_t test_alprd_specials(void) {
    static const uint64_t s64[] = {
        0x7ff8000000000000ull, 0x7ff8000000000123ull, 0xfff8000000000000ull, 0x7ff0000000000001ull,
        0xfff00000deadbeefull, 0x7ff0000000000000ull, 0xfff0000000000000ull, 0, NEG0_64,
        1, 0x800fffffffffffffull, 0x000fffffffffffffull, 0x8000000000000001ull, 0x7fefffffffffffffull,
        0xffefffffffffffffull, 0x0010000000000000ull, 0x3ff0000000000000ull, 0xc00921fb54442d18ull };
    static const uint64_t s32[] = {
        0x7fc00000u, 0x7fc00123u, 0xffc00000u, 0x7f800001u, 0xffbadbadu, 0x7f800000u, 0xff800000u, 0,
        NEG0_32, 1, 0x807fffffu, 0x007fffffu, 0x80000001u, 0x7f7fffffu, 0xff7fffffu, 0x3f800000u, 0xc0490fdbu };
    enum { N64 = sizeof s64 / sizeof s64[0], N32 = sizeof s32 / sizeof s32[0] };
    uint64_t v[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = s64[(i * 7) % N64];
    TEST_ASSERT_EQ_I(alprd_roundtrip(v, RAY_ENC_VEC, 64, NULL), 0);
    for (int n = 1; n <= RAY_ENC_VEC; n = n * 3 + 1) TEST_ASSERT_EQ_I(alprd_roundtrip(v, n, 64, NULL), 0);
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = s32[(i * 5) % N32];
    TEST_ASSERT_EQ_I(alprd_roundtrip(v, RAY_ENC_VEC, 32, NULL), 0);
    for (int n = 1; n <= RAY_ENC_VEC; n = n * 3 + 1) TEST_ASSERT_EQ_I(alprd_roundtrip(v, n, 32, NULL), 0);
    PASS();
}

/* The choice, then a write of the chosen encoding: its length is the
 * chosen size and it reads back (0, else a negative step). */
static int choice_enc(const uint64_t* v, int n, int T, const ray_enc_dict_t* d,
                      const ray_enc_alp_cands_t* c, int64_t extra, ray_enc_choice_t* ch) {
    *ch = ray_enc_float_choose(v, n, T, d, c, extra);
    ray_enc_buf_t* bp = &fl_buf;                                   /* freed in teardown */
    ray_enc_choice_t wc;
    bp->len = 0;
    if (!ray_enc_float_write(ch->enc, v, n, T, d, c, bp, &wc)) return -1;
    if (wc.enc != ch->enc || (int64_t)bp->len != ch->bytes || wc.bytes != ch->bytes) return -2;
    uint64_t out[RAY_ENC_VEC];
    if (ray_enc_float_read(bp->p, bp->len, ch->enc, n, T, d, out) != (int64_t)bp->len) return -3;
    uint64_t m = T == 64 ? ~0ull : 0xFFFFFFFFull;
    for (int i = 0; i < n; i++) if (out[i] != (v[i] & m)) return -4;
    return 0;
}

/* The choice: CONST, RLE, DICT, ALP, ALP_RD and RAW each win somewhere,
 * and the chosen size is what the write produces. */
static test_result_t test_float_choice(void) {
    g_fseed = 7;
    uint64_t v[RAY_ENC_VEC];
    ray_enc_alp_cands_t c;
    ray_enc_choice_t ch;
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = db(1.5);
    TEST_ASSERT_EQ_I(choice_enc(v, RAY_ENC_VEC, 64, NULL, NULL, 0, &ch), 0);
    TEST_ASSERT_EQ_I(ch.enc, RAY_ENC_CONST);
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = db((double)((frng() % 1000)) * 1.0000001);
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = v[(i / 128) * 128];   /* runs of 128 of awkward values */
    TEST_ASSERT_EQ_I(choice_enc(v, RAY_ENC_VEC, 64, NULL, NULL, 0, &ch), 0);
    TEST_ASSERT_EQ_I(ch.enc, RAY_ENC_RLE);
    uint64_t dv[4] = { db(-3.5), NEG0_64, db(0.0), db(2.25) };
    ray_enc_dict_t dict = { dv, 4 };
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = dv[(uint32_t)(i * 2654435761u) >> 30];
    TEST_ASSERT_EQ_I(choice_enc(v, RAY_ENC_VEC, 64, &dict, NULL, 0, &ch), 0);
    TEST_ASSERT_EQ_I(ch.enc, RAY_ENC_DICT);
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = db((double)((i * 7919) % 100000) / 100.0);
    ray_enc_alp_train(v, RAY_ENC_VEC, 64, &c);
    TEST_ASSERT_EQ_I(choice_enc(v, RAY_ENC_VEC, 64, NULL, &c, 0, &ch), 0);
    TEST_ASSERT_EQ_I(ch.enc, RAY_ENC_ALP);
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = db(1.0 + (double)(frng() >> 11) / 9007199254740992.0);
    TEST_ASSERT_EQ_I(choice_enc(v, RAY_ENC_VEC, 64, NULL, &c, 0, &ch), 0);
    TEST_ASSERT_EQ_I(ch.enc, RAY_ENC_ALP_RD);
    /* the trailer counts on both sides of the 1/8 rule: the winner stays
     * up to extra = 7*raw - 8*bytes and gives way to RAW one byte later */
    const int64_t raw = ray_enc_size_raw(RAY_ENC_VEC, 64), s = ch.bytes;
    const int64_t e0 = 7 * raw - 8 * s;
    TEST_ASSERT_TRUE(e0 > 0);
    TEST_ASSERT_EQ_I(choice_enc(v, RAY_ENC_VEC, 64, NULL, &c, e0, &ch), 0);
    TEST_ASSERT_EQ_I(ch.enc, RAY_ENC_ALP_RD);
    TEST_ASSERT_EQ_I(ch.bytes, s);                                 /* extra is not in the body size */
    TEST_ASSERT_EQ_I(choice_enc(v, RAY_ENC_VEC, 64, NULL, &c, e0 + 1, &ch), 0);
    TEST_ASSERT_EQ_I(ch.enc, RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(ch.bytes, raw);
    for (int i = 0; i < 8; i++) v[i] = frng() & 0x7FEFFFFFFFFFFFFFull;
    TEST_ASSERT_EQ_I(choice_enc(v, 8, 64, NULL, &c, 0, &ch), 0);
    TEST_ASSERT_EQ_I(ch.enc, RAY_ENC_RAW);
    /* every encoding forced through float_write / float_read */
    static const uint8_t encs[] = { RAY_ENC_RAW, RAY_ENC_CONST, RAY_ENC_RLE, RAY_ENC_DICT, RAY_ENC_ALP, RAY_ENC_ALP_RD };
    ray_enc_buf_t* bp = &fl_buf;                                   /* freed in teardown */
    for (size_t k = 0; k < sizeof encs; k++) {
        for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = encs[k] == RAY_ENC_CONST ? NEG0_64 : dv[(i / 7) % 4];
        bp->len = 0;
        TEST_ASSERT_TRUE(ray_enc_float_write(encs[k], v, RAY_ENC_VEC, 64, &dict, &c, bp, &ch));
        TEST_ASSERT_EQ_I(ch.enc, encs[k]);
        TEST_ASSERT_EQ_I(ch.bytes, (int64_t)bp->len);
        uint64_t out[RAY_ENC_VEC];
        TEST_ASSERT_EQ_I(ray_enc_float_read(bp->p, bp->len, encs[k], RAY_ENC_VEC, 64, &dict, out), (int64_t)bp->len);
        TEST_ASSERT_TRUE(memcmp(out, v, sizeof v) == 0);
    }
    /* CONST does not apply to unequal values */
    bp->len = 0;
    TEST_ASSERT_FALSE(ray_enc_float_write(RAY_ENC_CONST, v, RAY_ENC_VEC, 64, &dict, &c, bp, &ch));
    TEST_ASSERT_EQ_I((int64_t)bp->len, 0);
    PASS();
}

/* ---- the column writer (Task 4); the round trips decode through the
 * generation-2 reader, so they first pass with Task 5 ---- */
static ray_t* f64_col(const double* d, int64_t n) { return ray_vec_from_raw(RAY_F64, d, n); }
/* Releases the test's column (the teardown releases it on a failed assertion). */
static void fl_drop(void) { if (fl_vec) { ray_release(fl_vec); fl_vec = NULL; } }

/* The top-level encoding of vector `vec` in a generation-2 file, -1 on a
 * short file. */
static int desc_enc(const char* path, int64_t vec) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[96]; if (fread(h, 1, 96, f) != 96) { fclose(f); return -1; }
    int64_t c = vec / 64, j = vec % 64;
    uint64_t off;
    if (fseek(f, 96 + c * 64, SEEK_SET) != 0 || fread(&off, 8, 1, f) != 1) { fclose(f); return -1; }
    uint8_t d[48];
    if (fseek(f, (long)(off + (uint64_t)j * 48), SEEK_SET) != 0) { fclose(f); return -1; }
    int r = fread(d, 1, 48, f) == 48 ? d[0] : -1;
    fclose(f);
    return r;
}

/* n bytes of path at off. */
static bool fl_read(const char* path, long off, void* b, size_t n) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fseek(f, off, SEEK_SET) == 0 && fread(b, 1, n, f) == n;
    fclose(f);
    return ok;
}

/* The (e, f) of the ALP body of vector j of chunk c (no null bitmap) as
 * e * 256 + f, -1 on a short file. */
static int fl_alp_ef(const char* path, int64_t c, int j) {
    uint64_t off, poff;
    if (!fl_read(path, 96 + (long)c * 64, &off, 8) || !fl_read(path, (long)(off + (uint64_t)j * 48 + 8), &poff, 8)) return -1;
    uint8_t ef[2];
    return fl_read(path, (long)(off + poff), ef, 2) ? ef[0] * 256 + ef[1] : -1;
}
static bool fl_cands_have(const ray_enc_alp_cands_t* c, int ef) {
    for (int i = 0; i < c->n; i++) if (c->c[i].e * 256 + c->c[i].f == ef) return true;
    return false;
}

/* Saves gen 2 (optionally forced), loads, compares the payload bit for bit. */
static int fl_roundtrip(ray_t* v, int force, const char* path) {
    ray_encw_test_force(force);
    ray_err_t e = ray_col_save_gen(v, path, 2);
    ray_encw_test_force(-1);
    if (e != RAY_OK) return -1;
    ray_t* b = ray_col_load(path);
    if (!b || RAY_IS_ERR(b)) { if (b) ray_error_free(b); return -2; }
    size_t esz = v->type == RAY_F32 ? 4 : 8;
    int ok = b->len == v->len && memcmp(ray_data(b), ray_data(v), (size_t)v->len * esz) == 0;
    ray_release(b);
    return ok ? 0 : -3;
}

/* Selection, one shape per vector of chunk 1.  Chunk 0 holds two-decimal
 * values: the frozen VALUE_DICT covers only them, so it does not apply to
 * chunk 1, while the frozen ALP candidates (e = 2) do.  A one-chunk column
 * would let DICT win every vector: a first chunk has at most 65536 distinct
 * values, so the dictionary always trains (P0 rule). */
static test_result_t test_encoder_selects(void) {
    const int64_t base = 65536;
    static double d[65536 + 5 * 1024];
    uint64_t s = 9;
    for (int64_t i = 0; i < base; i++) d[i] = (double)((i * 7919) % 100000) / 100.0;
    for (int i = 0; i < 1024; i++) d[base + i] = 1.25;                                          /* CONST */
    for (int i = 0; i < 1024; i++) d[base + 1024 + i] = 5000.0 + (double)(i / 128) * 1.0000001; /* RLE */
    for (int i = 0; i < 1024; i++) d[base + 2048 + i] = 2000.0 + (double)((i * 7919) % 100000) / 100.0;  /* ALP */
    for (int i = 0; i < 1024; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                                     d[base + 3072 + i] = 1.0 + (double)(s >> 11) / 9007199254740992.0; }  /* ALP_RD */
    for (int i = 0; i < 1024; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17;
                                     uint64_t b = s & 0x7FEFFFFFFFFFFFFFull; memcpy(&d[base + 4096 + i], &b, 8); }  /* RAW */
    fl_vec = f64_col(d, base + 5 * 1024);
    const char* p = ray_test_tmp(FL_TMP "/sel");
    TEST_ASSERT_EQ_I(fl_roundtrip(fl_vec, -1, p), 0);
    TEST_ASSERT_EQ_I(desc_enc(p, 64), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(desc_enc(p, 65), RAY_ENC_RLE);
    TEST_ASSERT_EQ_I(desc_enc(p, 66), RAY_ENC_ALP);
    TEST_ASSERT_EQ_I(desc_enc(p, 67), RAY_ENC_ALP_RD);
    TEST_ASSERT_EQ_I(desc_enc(p, 68), RAY_ENC_RAW);
    fl_drop();
    PASS();
}

/* Canonical and non-canonical NaNs with ALP exceptions, in every forced
 * encoding.  -0.0 is written as its bit pattern (D10). */
static test_result_t test_nan_payloads(void) {
    static double d[2048];
    uint64_t odd = 0x7FF8000000000123ull, neg = 0xFFF8000000000000ull, nz = NEG0_64;
    for (int i = 0; i < 2048; i++) {
        d[i] = (double)(i % 50) / 4.0;
        if (i % 17 == 0) memcpy(&d[i], &nz, 8);           /* ALP exception */
        if (i % 23 == 0) d[i] = NULL_F64;                 /* canonical null */
        if (i % 29 == 0) memcpy(&d[i], &odd, 8);          /* trailer */
        if (i % 31 == 0) memcpy(&d[i], &neg, 8);          /* trailer */
    }
    fl_vec = f64_col(d, 2048);
    fl_vec->attrs |= RAY_ATTR_HAS_NULLS;
    static const int forces[] = { -1, RAY_ENC_RAW, RAY_ENC_RLE, RAY_ENC_DICT, RAY_ENC_ALP, RAY_ENC_ALP_RD };
    for (size_t k = 0; k < sizeof forces / sizeof forces[0]; k++)
        TEST_ASSERT_FMT(fl_roundtrip(fl_vec, forces[k], ray_test_tmp(FL_TMP "/nan")) == 0, "force %d", forces[k]);
    fl_drop();
    /* F32: the same with float NaN payloads */
    static float f[2048];
    uint32_t fodd = 0x7FC00042u, fnz = (uint32_t)NEG0_32;
    for (int i = 0; i < 2048; i++) {
        f[i] = (float)(i % 40) / 8.0f;
        if (i % 19 == 0) memcpy(&f[i], &fodd, 4);
        if (i % 21 == 0) f[i] = NULL_F32;
        if (i % 13 == 0) memcpy(&f[i], &fnz, 4);
    }
    fl_vec = ray_vec_from_raw(RAY_F32, f, 2048);
    fl_vec->attrs |= RAY_ATTR_HAS_NULLS;
    for (size_t k = 0; k < sizeof forces / sizeof forces[0]; k++)
        TEST_ASSERT_FMT(fl_roundtrip(fl_vec, forces[k], ray_test_tmp(FL_TMP "/nan32")) == 0, "f32 force %d", forces[k]);
    fl_drop();
    /* forced CONST: one value plus canonical and non-canonical NaNs */
    static double cst[1024];
    for (int i = 0; i < 1024; i++) {
        cst[i] = 2.5;
        if (i % 9 == 0) cst[i] = NULL_F64;
        if (i % 10 == 3) memcpy(&cst[i], &odd, 8);
    }
    fl_vec = f64_col(cst, 1024);
    fl_vec->attrs |= RAY_ATTR_HAS_NULLS;
    const char* p = ray_test_tmp(FL_TMP "/nanc");
    TEST_ASSERT_EQ_I(fl_roundtrip(fl_vec, RAY_ENC_CONST, p), 0);
    TEST_ASSERT_EQ_I(desc_enc(p, 0), RAY_ENC_CONST);
    fl_drop();
    PASS();
}

/* All-null float vectors at lengths 1, 1023, 1024 stay CONST under any
 * force (the six float encodings) and under selection. */
static test_result_t test_all_null_floats(void) {
    static const int lens[] = { 1, 1023, 1024 };
    static const int forces[] = { -1, RAY_ENC_RAW, RAY_ENC_CONST, RAY_ENC_RLE, RAY_ENC_DICT, RAY_ENC_ALP, RAY_ENC_ALP_RD };
    const char* p = ray_test_tmp(FL_TMP "/an");
    for (int t = 0; t < 2; t++)                          /* F64 and F32: every nullable float type */
        for (int l = 0; l < 3; l++) {
            int8_t ty = t ? RAY_F32 : RAY_F64;
            fl_vec = ray_vec_new(ty, lens[l]); fl_vec->len = lens[l];
            for (int i = 0; i < lens[l]; i++) {
                if (t) ((float*)ray_data(fl_vec))[i] = NULL_F32; else ((double*)ray_data(fl_vec))[i] = NULL_F64;
            }
            fl_vec->attrs |= RAY_ATTR_HAS_NULLS;
            for (size_t k = 0; k < sizeof forces / sizeof forces[0]; k++) {
                TEST_ASSERT_FMT(fl_roundtrip(fl_vec, forces[k], p) == 0, "type %d len %d force %d", ty, lens[l], forces[k]);
                TEST_ASSERT_FMT(desc_enc(p, 0) == RAY_ENC_CONST, "type %d len %d force %d: encoding %d",
                                ty, lens[l], forces[k], desc_enc(p, 0));
            }
            fl_drop();
        }
    PASS();
}

/* VALUE_DICT and ALP candidates come from the first chunk only, so a
 * column whose second chunk has new values encodes them otherwise (the
 * stream writer's agreement is encfloat/stream_equals_resident, Task 7). */
static test_result_t test_first_chunk_training(void) {
    int64_t n = 65536 + 4096;
    fl_vec = ray_vec_new(RAY_F64, n); fl_vec->len = n;
    for (int64_t i = 0; i < n; i++) ((double*)ray_data(fl_vec))[i] = i < 65536 ? (double)(i % 6) * 0.5 : 1e-7 * (double)i;
    const char* p = ray_test_tmp(FL_TMP "/tr");
    TEST_ASSERT_EQ_I(fl_roundtrip(fl_vec, -1, p), 0);
    TEST_ASSERT_EQ_I(desc_enc(p, 0), RAY_ENC_DICT);
    TEST_ASSERT_TRUE(desc_enc(p, 64) != RAY_ENC_DICT);
    fl_drop();
    /* ALP candidates: chunk 0 holds two-decimal values, chunk 1
     * one-decimal values, whose own training gives another (e, f).  The
     * candidates trained on chunk 0 still apply to them (wider ints), so
     * chunk 1's ALP vector carries one of those; alone (forced ALP: its
     * own chunk 0 trains), the same values take a pair outside them. */
    n = 65536 + 1024;
    fl_vec = ray_vec_new(RAY_F64, n); fl_vec->len = n;
    for (int64_t i = 0; i < n; i++)
        ((double*)ray_data(fl_vec))[i] = i < 65536 ? (double)((i * 7919) % 100000) / 100.0
                                                   : 3000.0 + (double)(((i - 65536) * 7919) % 10000) / 10.0;
    ray_enc_alp_cands_t c0;
    ray_enc_alp_train((const uint64_t*)ray_data(fl_vec), 65536, 64, &c0);
    TEST_ASSERT_TRUE(c0.n > 0);
    TEST_ASSERT_EQ_I(fl_roundtrip(fl_vec, -1, p), 0);
    TEST_ASSERT_EQ_I(desc_enc(p, 64), RAY_ENC_ALP);
    int ef1 = fl_alp_ef(p, 1, 0);
    TEST_ASSERT_FMT(fl_cands_have(&c0, ef1), "chunk 1 (e, f) = (%d, %d) is not a chunk-0 candidate", ef1 >> 8, ef1 & 255);
    fl_drop();
    fl_vec = ray_vec_new(RAY_F64, 1024); fl_vec->len = 1024;
    for (int64_t i = 0; i < 1024; i++) ((double*)ray_data(fl_vec))[i] = 3000.0 + (double)((i * 7919) % 10000) / 10.0;
    TEST_ASSERT_EQ_I(fl_roundtrip(fl_vec, RAY_ENC_ALP, p), 0);        /* alone, DICT would win */
    TEST_ASSERT_EQ_I(desc_enc(p, 0), RAY_ENC_ALP);
    int efa = fl_alp_ef(p, 0, 0);
    TEST_ASSERT_FMT(!fl_cands_have(&c0, efa), "alone (e, f) = (%d, %d) is also a chunk-0 candidate", efa >> 8, efa & 255);
    fl_drop();
    /* An F32 VALUE_DICT has 4-byte elements, in dictionary order:
     * -3.5, -0.0, +0.0, 2.25 (the zeros as bit patterns, D10). */
    static const uint32_t fv[4] = { 0xC0600000u, 0x80000000u, 0x00000000u, 0x40100000u };
    fl_vec = ray_vec_new(RAY_F32, 1024); fl_vec->len = 1024;
    for (int i = 0; i < 1024; i++) memcpy((float*)ray_data(fl_vec) + i, &fv[(i * 7) % 4], 4);
    TEST_ASSERT_EQ_I(fl_roundtrip(fl_vec, RAY_ENC_DICT, p), 0);
    TEST_ASSERT_EQ_I(desc_enc(p, 0), RAY_ENC_DICT);
    ray_enc_hdr_t eh;
    TEST_ASSERT_TRUE(fl_read(p, 32, &eh, sizeof eh));
    uint8_t side[8 + 24 + 8 + 16];
    TEST_ASSERT_EQ_U(eh.side_len, sizeof side);                        /* one entry, 24 bytes at 32 */
    TEST_ASSERT_TRUE(fl_read(p, (long)eh.side_off, side, sizeof side));
    static const uint8_t want[sizeof side] = {
        1, 0, 0, 0,  0, 0, 0, 0,                                       /* one entry */
        1, 0, 0, 0, 0, 0, 0, 0,  32, 0, 0, 0, 0, 0, 0, 0,  24, 0, 0, 0, 0, 0, 0, 0,   /* VALUE_DICT, @32, 24 B */
        4, 0, 0, 0,  0, 0, 0, 0,                                       /* n = 4 */
        0x00, 0x00, 0x60, 0xC0,  0x00, 0x00, 0x00, 0x80,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x10, 0x40,
    };
    for (size_t i = 0; i < sizeof side; i++)
        TEST_ASSERT_FMT(side[i] == want[i], "side byte %zu: 0x%02x, want 0x%02x", i, side[i], want[i]);
    fl_drop();
    PASS();
}

/* ---- the reader (Task 5) ---- */
static void fl_patch(const char* path, long off, const void* b, size_t n) {
    FILE* f = fopen(path, "r+b");
    if (!f) ray_test_fatal("cannot open %s", path);
    if (fseek(f, off, SEEK_SET) != 0 || fwrite(b, 1, n, f) != n) { fclose(f); ray_test_fatal("cannot patch %s", path); }
    fclose(f);
}

/* The error code materialization gives ("" on success), its message in
 * fl_msg. */
static char fl_msg[512];
static const char* fl_mat_code(const char* path) {
    static char code[16];
    fl_msg[0] = 0;
    ray_error_clear();
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) {
        snprintf(code, sizeof code, "%s", c ? ray_err_code(c) : "null");
        snprintf(fl_msg, sizeof fl_msg, "%s", ray_error_msg() ? ray_error_msg() : "");
        if (c) ray_error_free(c);
        return code;
    }
    ray_t* m = ray_enccol_materialize(c);                  /* borrowed on success */
    snprintf(code, sizeof code, "%s", RAY_IS_ERR(m) ? ray_err_code(m) : "");
    if (RAY_IS_ERR(m)) { snprintf(fl_msg, sizeof fl_msg, "%s", ray_error_msg() ? ray_error_msg() : ""); ray_error_free(m); }
    ray_release(c);
    return code;
}

/* Range reads of the whole column at path equal v bit for bit: 0, else a
 * negative step. */
static int fl_range_equal(const char* path, ray_t* v) {
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_error_free(c); return -1; }
    ray_col_src_t s;
    ray_t* err = ray_col_src_bind(c, &s);
    if (err) { ray_error_free(err); ray_release(c); return -2; }
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    const size_t esz = v->type == RAY_F32 ? 4 : 8;
    int rc = 0;
    int64_t at = 0;
    while (at < v->len && rc == 0) {
        ray_col_range_t r;
        err = ray_col_range(&s, at, v->len - at, &sc, &r);
        if (err) { ray_error_free(err); rc = -3; break; }
        if (r.n <= 0) { rc = -4; break; }
        if (memcmp(r.data, (const uint8_t*)ray_data(v) + (size_t)at * esz, (size_t)r.n * esz) != 0) rc = -5;
        at += r.n;
    }
    ray_col_scratch_free(&sc); ray_col_src_release(&s); ray_release(c);
    return rc;
}

/* Every encoding round-trips through load (materialization) and range reads. */
static test_result_t test_decode_paths(void) {
    static double d[3 * 1024];
    for (int i = 0; i < 3 * 1024; i++) d[i] = (double)((i * 7919) % 100000) / 100.0;
    fl_vec = f64_col(d, 3 * 1024);
    const char* p = ray_test_tmp(FL_TMP "/dp");
    static const int forces[] = { RAY_ENC_RAW, RAY_ENC_RLE, RAY_ENC_DICT, RAY_ENC_ALP, RAY_ENC_ALP_RD };
    for (size_t k = 0; k < sizeof forces / sizeof forces[0]; k++) {
        if (forces[k] == RAY_ENC_DICT) continue;           /* 3072 distinct values: covered in nan_payloads */
        TEST_ASSERT_FMT(fl_roundtrip(fl_vec, forces[k], p) == 0, "force %d", forces[k]);
        for (int64_t j = 0; j < 3; j++) TEST_ASSERT_FMT(desc_enc(p, j) == forces[k], "force %d vector %lld", forces[k], (long long)j);
        TEST_ASSERT_FMT(fl_range_equal(p, fl_vec) == 0, "force %d range read %d", forces[k], fl_range_equal(p, fl_vec));
    }
    fl_drop();
    PASS();
}

/* VALUE_DICT invariants for floats: no NaN, strictly ascending in
 * dictionary order (the two zeros are -0.0 then +0.0). */
static test_result_t test_float_dict_rejects(void) {
    static double d[1024];
    static const uint64_t dv[4] = { 0xC00C000000000000ull, NEG0_64, 0, 0x4002000000000000ull };   /* -3.5, -0.0, +0.0, 2.25 */
    for (int i = 0; i < 1024; i++) memcpy(&d[i], &dv[(i * 7) % 4], 8);
    fl_vec = f64_col(d, 1024);
    const char* p = ray_test_tmp(FL_TMP "/vd");
    TEST_ASSERT_EQ_I(fl_roundtrip(fl_vec, RAY_ENC_DICT, p), 0);
    TEST_ASSERT_EQ_I(desc_enc(p, 0), RAY_ENC_DICT);
    TEST_ASSERT_EQ_I(fl_range_equal(p, fl_vec), 0);
    ray_enc_hdr_t eh;
    TEST_ASSERT_TRUE(fl_read(p, 32, &eh, sizeof eh));
    long vals = (long)eh.side_off + 32 + 8;                            /* the entry's first value */
    uint64_t x;
    for (int i = 0; i < 4; i++) { TEST_ASSERT_TRUE(fl_read(p, vals + 8 * i, &x, 8)); TEST_ASSERT_EQ_U(x, dv[i]); }
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "");
    uint64_t pz = 0, nz = NEG0_64, nan = 0x7FF8000000000000ull;
    fl_patch(p, vals + 8, &pz, 8); fl_patch(p, vals + 16, &nz, 8);   /* +0 before -0 */
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "corrupt");
    TEST_ASSERT_FMT(strstr(fl_msg, "VALUE_DICT values not ascending") != NULL, "%s", fl_msg);
    fl_patch(p, vals + 8, &nz, 8); fl_patch(p, vals + 16, &pz, 8);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "");
    fl_patch(p, vals + 24, &nan, 8);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "corrupt");
    TEST_ASSERT_FMT(strstr(fl_msg, "VALUE_DICT value is a NaN") != NULL, "%s", fl_msg);
    fl_drop();
    PASS();
}

/* ALP / ALP_RD framing in a file, and the trailer after a codec body, are
 * corrupt at materialization (spec "Reject rules"). */
static test_result_t test_float_file_rejects(void) {
    static double d[1024];
    for (int i = 0; i < 1024; i++) { d[i] = (double)(i % 100) / 4.0; if (i % 17 == 0) memcpy(&d[i], &(uint64_t){ NEG0_64 }, 8); }
    fl_vec = f64_col(d, 1024);
    const char* ar = ray_test_tmp(FL_TMP "/ar");
    TEST_ASSERT_EQ_I(fl_roundtrip(fl_vec, RAY_ENC_ALP, ar), 0);
    TEST_ASSERT_EQ_I(desc_enc(ar, 0), RAY_ENC_ALP);
    const long payload = 96 + 64 + RAY_ENC_DESC_AREA;                  /* chunk 0, vector 0, no bitmap */
    uint8_t e19 = 19;
    fl_patch(ar, payload, &e19, 1);
    TEST_ASSERT_STR_EQ(fl_mat_code(ar), "corrupt");
    TEST_ASSERT_FMT(strstr(fl_msg, "chunk 0 vector 0 codec body: ALP exponent above 18") != NULL, "%s", fl_msg);
    const char* rd = ray_test_tmp(FL_TMP "/rd");
    TEST_ASSERT_EQ_I(fl_roundtrip(fl_vec, RAY_ENC_ALP_RD, rd), 0);
    TEST_ASSERT_EQ_I(desc_enc(rd, 0), RAY_ENC_ALP_RD);
    uint8_t rb = 40;                                                   /* below T-16 */
    fl_patch(rd, payload, &rb, 1);
    TEST_ASSERT_STR_EQ(fl_mat_code(rd), "corrupt");
    TEST_ASSERT_FMT(strstr(fl_msg, "codec body: ALP_RD right width outside T-16..T-1") != NULL, "%s", fl_msg);
    fl_drop();
    /* a trailer after an ALP body: one non-canonical NaN, so the payload
     * ends with [u16 1, 6 reserved][position, padded to 8][the raw NaN] */
    d[5] = 0; memcpy(&d[5], &(uint64_t){ 0x7FF8000000000077ull }, 8);
    fl_vec = f64_col(d, 1024);
    fl_vec->attrs |= RAY_ATTR_HAS_NULLS;
    const char* tr = ray_test_tmp(FL_TMP "/tr");
    TEST_ASSERT_EQ_I(fl_roundtrip(fl_vec, RAY_ENC_ALP, tr), 0);
    TEST_ASSERT_EQ_I(desc_enc(tr, 0), RAY_ENC_ALP);
    TEST_ASSERT_EQ_I(fl_range_equal(tr, fl_vec), 0);
    uint64_t pbytes;
    TEST_ASSERT_TRUE(fl_read(tr, 96 + 64 + 16, &pbytes, 8));         /* descriptor 0's payload_bytes */
    uint64_t raw;
    TEST_ASSERT_TRUE(fl_read(tr, payload + (long)pbytes - 8, &raw, 8));
    TEST_ASSERT_EQ_U(raw, 0x7FF8000000000077ull);
    uint64_t one = db(1.0);
    fl_patch(tr, payload + (long)pbytes - 8, &one, 8);
    TEST_ASSERT_STR_EQ(fl_mat_code(tr), "corrupt");
    TEST_ASSERT_FMT(strstr(fl_msg, "trailer value is not a NaN") != NULL, "%s", fl_msg);
    fl_drop();
    PASS();
}

/* The trailer's bytes count on both sides of the 1/8 gain rule at the
 * writer.  1 + 52 random mantissa bits: ALP_RD of 8 + 8 + 16 + 6656 =
 * 6688 B against RAW 8192, so a trailer of more than 7 * 8192 - 8 * 6688 =
 * 3840 B flips the choice to RAW.  384 NaN rows (filled alike, so the
 * same body): canonical, no trailer, ALP_RD; 383 with payloads, a trailer
 * of 8 + 768 + 3064 = 3840 B, still ALP_RD; 384 with payloads, 3848 B,
 * RAW.  Left out of the choice, the trailer would give ALP_RD all three. */
static test_result_t test_trailer_counts_in_choice(void) {
    static double d[RAY_ENC_VEC];
    static const int odd[3] = { 0, 383, 384 };
    for (int k = 0; k < 3; k++) {
        uint64_t s = 11;
        int nan_rows = 0;
        for (int i = 0; i < RAY_ENC_VEC; i++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            d[i] = 1.0 + (double)(s >> 12) / 4503599627370496.0;
            if (i % 8 < 3) {
                if (nan_rows++ < odd[k]) memcpy(&d[i], &(uint64_t){ 0x7FF8000000000100ull + (uint64_t)i }, 8);
                else d[i] = NULL_F64;
            }
        }
        ray_encw_vec_t v; ray_encw_vec_init(&v);
        ray_err_t e = ray_encw_vec(RAY_F64, RAY_ATTR_HAS_NULLS, d, NULL, 0, RAY_ENC_VEC, NULL, &v);
        int enc = v.desc.encoding, flags = v.desc.flags, nn = v.desc.n_nulls;
        ray_encw_vec_free(&v);
        TEST_ASSERT_EQ_I(e, RAY_OK);
        TEST_ASSERT_EQ_I(nn, 384);
        TEST_ASSERT_FMT(enc == (k == 2 ? RAY_ENC_RAW : RAY_ENC_ALP_RD), "%d payload NaNs: encoding %d", odd[k], enc);
        TEST_ASSERT_EQ_I(!!(flags & RAY_ENC_DESC_TRAILER), k > 0);
    }
    PASS();
}

/* ---- float views, evaluators and their consumers (Task 6) ---- */
static double bd(uint64_t b) { double x; memcpy(&x, &b, 8); return x; }

/* d as a column of type ty (F32: each value narrowed, -0.0 by its bit
 * pattern, D10; a NaN becomes the type's null), HAS_NULLS when a NaN is in. */
static ray_t* fl_typed(int8_t ty, const double* d, int64_t n) {
    bool nul = false;
    for (int64_t i = 0; i < n; i++) nul |= d[i] != d[i];
    ray_t* v;
    if (ty == RAY_F64) v = f64_col(d, n);
    else {
        v = ray_vec_new(RAY_F32, n); v->len = n;
        float* o = (float*)ray_data(v);
        for (int64_t i = 0; i < n; i++) {
            if (db(d[i]) == NEG0_64) { uint32_t z = (uint32_t)NEG0_32; memcpy(&o[i], &z, 4); }
            else o[i] = d[i] != d[i] ? NULL_F32 : (float)d[i];
        }
    }
    if (nul) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}

/* d saved at path as generation 2, every float vector forced to enc (-1:
 * selection), then mapped into fl_map.  False on any failure. */
static bool fl_view_col(int8_t ty, int enc, const double* d, int64_t n, const char* path) {
    fl_drop();
    if (fl_map) { ray_release(fl_map); fl_map = NULL; }
    fl_vec = fl_typed(ty, d, n);
    ray_encw_test_force_float(enc);
    ray_err_t e = ray_col_save_gen(fl_vec, path, 2);
    ray_encw_test_force_float(-1);
    fl_drop();
    if (e != RAY_OK) return false;
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_error_free(c); return false; }
    fl_map = c;
    return true;
}
/* path mapped again into fl_map (after a patch). */
static bool fl_remap(const char* path) {
    if (fl_map) { ray_release(fl_map); fl_map = NULL; }
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_error_free(c); return false; }
    fl_map = c;
    return true;
}
/* Vector vec of fl_map as a view into fl_vw: 1 / 0 / -1 (the error freed). */
static int fl_vview(int64_t vec) {
    ray_t* err = NULL;
    int r = ray_enccol_vview(ray_enccol_of(fl_map), vec, &fl_vw, &err);
    if (err) ray_error_free(err);
    return r;
}

/* The evaluators equal a direct scalar evaluation under the predicate
 * comparator (-0.0 == +0.0), for every operator, both zeros, both null
 * results, F64 and F32 (widened), over RLE and DICT views. */
static test_result_t test_eval_f64(void) {
    static double d[1024];
    const double dv[4] = { -3.5, bd(NEG0_64), 0.0, 2.25 };
    for (int i = 0; i < 1024; i++) d[i] = dv[(i / 3) % 4];
    d[100] = NULL_F64; d[513] = NULL_F64;
    const double consts[] = { 0.0, bd(NEG0_64), -3.5, 2.25, 1.0, -10.0, 3.0 };
    static const int encs[] = { RAY_ENC_RLE, RAY_ENC_DICT };
    static const int8_t tys[] = { RAY_F64, RAY_F32 };
    const char* p = ray_test_tmp(FL_TMP "/ev");
    for (size_t t = 0; t < 2; t++)
    for (size_t k = 0; k < 2; k++) {
        TEST_ASSERT_TRUE(fl_view_col(tys[t], encs[k], d, 1024, p));
        TEST_ASSERT_EQ_I(desc_enc(p, 0), encs[k]);
        TEST_ASSERT_EQ_I(fl_vview(0), 1);
        TEST_ASSERT_EQ_I(fl_vw.enc, encs[k]);
        TEST_ASSERT_EQ_I(fl_vw.k.T, tys[t] == RAY_F32 ? 32 : 64);
        TEST_ASSERT_TRUE(fl_vw.k.is_float);
        TEST_ASSERT_EQ_I(fl_vw.n_nulls, 2);
        TEST_ASSERT_NOT_NULL(fl_vw.nullmap);
        uint8_t bits[1024];
        for (uint16_t op = OP_EQ; op <= OP_GE; op++)
            for (size_t ci = 0; ci < sizeof consts / sizeof consts[0]; ci++)
                for (uint8_t nr = 0; nr < 2; nr++) {
                    memset(bits, 0xAA, sizeof bits);
                    TEST_ASSERT_TRUE(ray_enc_eval_cmp_f64(&fl_vw, op, consts[ci], nr, bits));
                    const double c = consts[ci];
                    for (int i = 0; i < 1024; i++) {
                        uint8_t want;
                        if (d[i] != d[i]) want = nr;
                        else switch (op) {
                            case OP_EQ: want = d[i] == c; break; case OP_NE: want = d[i] != c; break;
                            case OP_LT: want = d[i] <  c; break; case OP_LE: want = d[i] <= c; break;
                            case OP_GT: want = d[i] >  c; break; default:    want = d[i] >= c; break; }
                        TEST_ASSERT_FMT(bits[i] == want, "type %d enc %d op %u c %g nr %u row %d: %u",
                                        tys[t], encs[k], op, c, nr, i, bits[i]);
                    }
                }
        /* declines: a NaN constant (the null constant), an operator outside
         * OP_EQ .. OP_GE; the integer evaluator declines the float view */
        TEST_ASSERT_FALSE(ray_enc_eval_cmp_f64(&fl_vw, OP_EQ, NULL_F64, 0, bits));
        TEST_ASSERT_FALSE(ray_enc_eval_cmp_f64(&fl_vw, OP_GE + 1, 0.0, 0, bits));
        TEST_ASSERT_FALSE(ray_enc_eval_cmp(&fl_vw, OP_EQ, 0, 0, bits));
        TEST_ASSERT_EQ_I(ray_enc_const_decision_f64(&fl_vw, OP_EQ, 0.0, 0), -1);   /* not CONST */
    }
    /* CONST: one value for every row */
    static double cst[1024];
    for (int i = 0; i < 1024; i++) cst[i] = bd(NEG0_64);
    TEST_ASSERT_TRUE(fl_view_col(RAY_F64, RAY_ENC_CONST, cst, 1024, p));
    TEST_ASSERT_EQ_I(desc_enc(p, 0), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(fl_vview(0), 1);
    uint8_t bits[1024];
    TEST_ASSERT_TRUE(ray_enc_eval_cmp_f64(&fl_vw, OP_EQ, 0.0, 0, bits));
    for (int i = 0; i < 1024; i++) TEST_ASSERT_EQ_I(bits[i], 1);
    TEST_ASSERT_TRUE(ray_enc_eval_cmp_f64(&fl_vw, OP_LT, 0.0, 0, bits));
    for (int i = 0; i < 1024; i++) TEST_ASSERT_EQ_I(bits[i], 0);
    PASS();
}

/* A nullable CONST float vector is decided whole only when the value's
 * result equals null_result. */
static test_result_t test_const_decision_f64(void) {
    static double d[1024];
    for (int i = 0; i < 1024; i++) d[i] = i == 7 ? NULL_F64 : bd(NEG0_64);
    const char* p = ray_test_tmp(FL_TMP "/cd");
    TEST_ASSERT_TRUE(fl_view_col(RAY_F64, RAY_ENC_CONST, d, 1024, p));
    TEST_ASSERT_EQ_I(desc_enc(p, 0), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(fl_vview(0), 1);
    TEST_ASSERT_EQ_I(ray_enc_const_decision_f64(&fl_vw, OP_LE, 0.0, 1), 1);        /* -0 <= 0 and null -> 1 */
    TEST_ASSERT_EQ_I(ray_enc_const_decision_f64(&fl_vw, OP_EQ, 0.0, 0), -1);       /* -0 == 0 but null -> 0 */
    TEST_ASSERT_EQ_I(ray_enc_const_decision_f64(&fl_vw, OP_GT, 0.0, 0), 0);
    TEST_ASSERT_EQ_I(ray_enc_const_decision_f64(&fl_vw, OP_EQ, NULL_F64, 0), -1);  /* a NaN constant */
    PASS();
}

/* The float RLE / DICT views make dec_floats' checks, and decline what
 * decode rejects (D7); a trailer, RAW, ALP and ALP_RD decline (D6). */
static test_result_t test_view_checks(void) {
    const long desc = 96 + 64, payload = desc + RAY_ENC_DESC_AREA;    /* chunk 0, vector 0 */
    const char* p = ray_test_tmp(FL_TMP "/vc");
    static double d[1024];
    /* RLE: 16 runs of 64 rows, 1.5 .. 16.5, no nulls: [nr, rsv][values
     * child header][16 values][ends child] */
    for (int i = 0; i < 1024; i++) d[i] = 1.5 + (double)(i / 64);
    TEST_ASSERT_TRUE(fl_view_col(RAY_F64, RAY_ENC_RLE, d, 1024, p));
    TEST_ASSERT_EQ_I(desc_enc(p, 0), RAY_ENC_RLE);
    TEST_ASSERT_EQ_I(fl_vview(0), 1);
    TEST_ASSERT_EQ_I(fl_vw.n_runs, 16);
    TEST_ASSERT_EQ_U(fl_vw.run_vals.q[3], db(4.5));
    TEST_ASSERT_EQ_I(fl_vw.run_ends.h[3], 256);
    TEST_ASSERT_TRUE(fl_vw.nullmap == NULL);
    uint8_t enc0; TEST_ASSERT_TRUE(fl_read(p, payload + 8, &enc0, 1));
    TEST_ASSERT_EQ_I(enc0, RAY_ENC_RAW);
    uint64_t v0, nan = 0x7FF8000000000000ull, half = db(0.5), two = db(2.0), smin;
    TEST_ASSERT_TRUE(fl_read(p, payload + 16, &v0, 8));
    TEST_ASSERT_EQ_U(v0, db(1.5));
    /* a run value that is a NaN on non-null rows */
    fl_patch(p, payload + 16, &nan, 8);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 0);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "corrupt");
    TEST_ASSERT_FMT(strstr(fl_msg, "NaN outside the bitmap") != NULL, "%s", fl_msg);
    /* a run value below the statistics */
    fl_patch(p, payload + 16, &half, 8);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 0);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "corrupt");
    TEST_ASSERT_FMT(strstr(fl_msg, "value outside its statistics") != NULL, "%s", fl_msg);
    fl_patch(p, payload + 16, &v0, 8);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 1);
    /* the statistics raised above the first run */
    TEST_ASSERT_TRUE(fl_read(p, desc + 32, &smin, 8));
    fl_patch(p, desc + 32, &two, 8);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 0);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "corrupt");
    TEST_ASSERT_FMT(strstr(fl_msg, "value outside its statistics") != NULL, "%s", fl_msg);
    fl_patch(p, desc + 32, &smin, 8);
    /* a FOR_BP values child: legal for integers only */
    uint8_t forbp = RAY_ENC_FOR_BP;
    fl_patch(p, payload + 8, &forbp, 1);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 0);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "corrupt");
    fl_patch(p, payload + 8, &enc0, 1);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 1);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "");

    /* nullable RLE: a run wholly in the bitmap is never read, so its
     * stored value may be anything (a NaN here); decode agrees */
    for (int i = 0; i < 1024; i++) d[i] = i >= 192 && i < 256 ? NULL_F64 : 1.5 + (double)(i / 64);
    TEST_ASSERT_TRUE(fl_view_col(RAY_F64, RAY_ENC_RLE, d, 1024, p));
    TEST_ASSERT_EQ_I(desc_enc(p, 0), RAY_ENC_RLE);
    TEST_ASSERT_EQ_I(fl_vview(0), 1);
    TEST_ASSERT_NOT_NULL(fl_vw.nullmap);
    int nullrun = -1;
    for (uint32_t r = 0, prev = 0; r < fl_vw.n_runs; prev = fl_vw.run_ends.h[r++])
        if (prev == 192 && fl_vw.run_ends.h[r] == 256) nullrun = (int)r;
    TEST_ASSERT_FMT(nullrun >= 0, "no run holds exactly the null rows (%u runs)", fl_vw.n_runs);
    const long nbody = payload + 128;                                  /* after the bitmap */
    fl_patch(p, nbody + 16 + 8 * nullrun, &nan, 8);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 1);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "");
    TEST_ASSERT_EQ_I(fl_range_equal(p, (fl_vec = fl_typed(RAY_F64, d, 1024))), 0);
    fl_drop();
    /* the run next to it holds non-null rows: a NaN there is corrupt */
    fl_patch(p, nbody + 16 + 8 * (nullrun + 1), &nan, 8);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 0);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "corrupt");

    /* DICT: -3.5, -0.0, +0.0, 2.25; [codes child header][base][packed] */
    static const uint64_t dv[4] = { 0xC00C000000000000ull, NEG0_64, 0, 0x4002000000000000ull };
    for (int i = 0; i < 1024; i++) memcpy(&d[i], &dv[(i * 7) % 4], 8);
    TEST_ASSERT_TRUE(fl_view_col(RAY_F64, RAY_ENC_DICT, d, 1024, p));
    TEST_ASSERT_EQ_I(desc_enc(p, 0), RAY_ENC_DICT);
    TEST_ASSERT_EQ_I(fl_vview(0), 1);
    TEST_ASSERT_EQ_I(fl_vw.enc, RAY_ENC_DICT);
    TEST_ASSERT_EQ_U(fl_vw.dict_n, 4);
    for (int i = 0; i < 1024; i++) TEST_ASSERT_EQ_U(fl_vw.dict[fl_vw.codes[i]], dv[(i * 7) % 4]);
    TEST_ASSERT_TRUE(fl_read(p, payload, &enc0, 1));
    TEST_ASSERT_EQ_I(enc0, RAY_ENC_FOR_BP);
    /* the statistics' min at +0.0: the -3.5 rows fall outside */
    uint64_t pz = 0, nz = NEG0_64, base, four = 4;
    TEST_ASSERT_TRUE(fl_read(p, desc + 32, &smin, 8));
    fl_patch(p, desc + 32, &pz, 8);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 0);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "corrupt");
    TEST_ASSERT_FMT(strstr(fl_msg, "value outside its statistics") != NULL, "%s", fl_msg);
    fl_patch(p, desc + 32, &smin, 8);
    /* codes past the dictionary (the FOR_BP base raised) */
    TEST_ASSERT_TRUE(fl_read(p, payload + 8, &base, 8));
    fl_patch(p, payload + 8, &four, 8);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 0);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "corrupt");
    fl_patch(p, payload + 8, &base, 8);
    /* a codes child other than FOR_BP */
    uint8_t raw = RAY_ENC_RAW;
    fl_patch(p, payload, &raw, 1);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 0);
    fl_patch(p, payload, &enc0, 1);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 1);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "");
    /* zeros against the statistics compare equal (the predicate
     * comparator): -0.0 rows under a +0.0 min are inside, as decode reads them */
    static const uint64_t zv[3] = { NEG0_64, 0, 0x4002000000000000ull };
    for (int i = 0; i < 1024; i++) memcpy(&d[i], &zv[i % 3], 8);
    TEST_ASSERT_TRUE(fl_view_col(RAY_F64, RAY_ENC_DICT, d, 1024, p));
    TEST_ASSERT_EQ_I(desc_enc(p, 0), RAY_ENC_DICT);
    TEST_ASSERT_TRUE(fl_read(p, desc + 32, &smin, 8));
    TEST_ASSERT_EQ_U(smin, nz);
    fl_patch(p, desc + 32, &pz, 8);
    TEST_ASSERT_TRUE(fl_remap(p)); TEST_ASSERT_EQ_I(fl_vview(0), 1);
    TEST_ASSERT_STR_EQ(fl_mat_code(p), "");

    /* a trailer (a non-canonical NaN) declines every float view (D6); the
     * range read still answers */
    static const int tforces[] = { RAY_ENC_CONST, RAY_ENC_RLE, RAY_ENC_DICT };
    for (size_t k = 0; k < 3; k++) {
        for (int i = 0; i < 1024; i++) d[i] = k == 0 ? 2.5 : 1.5 + (double)(i / 64);
        memcpy(&d[5], &(uint64_t){ 0x7FF8000000000077ull }, 8);
        TEST_ASSERT_TRUE(fl_view_col(RAY_F64, tforces[k], d, 1024, p));
        TEST_ASSERT_EQ_I(desc_enc(p, 0), tforces[k]);
        uint16_t fl; TEST_ASSERT_TRUE(fl_read(p, desc + 4, &fl, 2));
        TEST_ASSERT_TRUE(fl & RAY_ENC_DESC_TRAILER);
        TEST_ASSERT_EQ_I(fl_vview(0), 0);
        fl_vec = fl_typed(RAY_F64, d, 1024);
        memcpy((double*)ray_data(fl_vec) + 5, &(uint64_t){ 0x7FF8000000000077ull }, 8);
        TEST_ASSERT_FMT(fl_range_equal(p, fl_vec) == 0, "force %d", tforces[k]);
        fl_drop();
    }
    /* RAW, ALP and ALP_RD decline: the caller decodes */
    static const int dforces[] = { RAY_ENC_RAW, RAY_ENC_ALP, RAY_ENC_ALP_RD };
    for (int i = 0; i < 1024; i++) d[i] = (double)((i * 7919) % 100000) / 100.0;
    for (size_t k = 0; k < 3; k++) {
        TEST_ASSERT_TRUE(fl_view_col(RAY_F64, dforces[k], d, 1024, p));
        TEST_ASSERT_EQ_I(desc_enc(p, 0), dforces[k]);
        TEST_ASSERT_EQ_I(fl_vview(0), 0);
    }
    PASS();
}

/* ---- the view consumers over a float column (D7) ---- */
static ray_runtime_t* g_flrt;
static void fl_rt_setup(void) {
    g_flrt = ray_runtime_create(0, NULL);
    (void)ray_test_rm_rf(ray_test_tmp(FL_TMP)); (void)ray_test_mkdir_p(ray_test_tmp(FL_TMP));
}
static void fl_rt_teardown(void) {
    ray_test_hooks_off(); ray_encw_test_force(-1);
    if (fl_map) { ray_release(fl_map); fl_map = NULL; }
    (void)ray_test_rm_rf(ray_test_tmp(FL_TMP)); ray_runtime_destroy(g_flrt);
}
static ray_t* fl_eval(const char* src) { return ray_eval_str(ray_test_tmp(src)); }
static bool fl_eval_ok(const char* src) {
    ray_t* r = fl_eval(src);
    if (r && RAY_IS_ERR(r)) { fprintf(stderr, "fl_eval_ok: %s: %s\n", src, ray_error_msg()); ray_error_free(r); return false; }
    if (r) ray_release(r);
    return true;
}
/* q on generation 0 (table vc0) and generation 2 (vc2) as T: equal types
 * and formatted results. */
static test_result_t fl_same(const char* q) {
    ray_t* out[2];
    for (int gen = 0; gen < 2; gen++) {
        TEST_ASSERT_TRUE(fl_eval_ok(gen ? "(set T (.db.splayed.get \"" FL_TMP "/vc2/\"))"
                                        : "(set T (.db.splayed.get \"" FL_TMP "/vc0/\"))"));
        out[gen] = fl_eval(q);
        TEST_ASSERT_FMT(out[gen] && !RAY_IS_ERR(out[gen]), "%s gen %d: %s", q, gen, ray_error_msg());
    }
    TEST_ASSERT_FMT(out[0]->type == out[1]->type, "%s: type %d vs %d", q, out[0]->type, out[1]->type);
    ray_t* fa = ray_fmt(out[0], 0); ray_t* fb2 = ray_fmt(out[1], 0);
    bool eq = ray_str_len(fa) == ray_str_len(fb2) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb2), ray_str_len(fa)) == 0;
    if (!eq) fprintf(stderr, "gen 0: %.*s\ngen 2: %.*s\n", (int)ray_str_len(fa), ray_str_ptr(fa), (int)ray_str_len(fb2), ray_str_ptr(fb2));
    ray_release(fa); ray_release(fb2); ray_release(out[0]); ray_release(out[1]);
    TEST_ASSERT_FMT(eq, "%s differs", q);
    PASS();
}
#define FL_SAME(q) do { test_result_t r_ = fl_same(q); if (r_.status != TEST_PASS) return r_; } while (0)

/* -0.0 / +0.0 alternating (with 1.5, -2.0 and NaN nulls), in runs forced
 * RLE and then scattered forced DICT: every consumer of a float view (group keys, first-n keys,
 * take, count, in, sum, the fused filter) equals generation 0, the zeros
 * grouping together. */
static test_result_t test_view_consumers(void) {
    const int64_t n = 70000;                                      /* two chunks */
    static const int forces[] = { RAY_ENC_RLE, RAY_ENC_DICT };
    for (size_t fi = 0; fi < 2; fi++) {
        ray_t* k = ray_vec_new(RAY_I64, n);
        ray_t* h = ray_vec_new(RAY_I64, n);
        ray_t* f = ray_vec_new(RAY_F64, n);
        TEST_ASSERT_TRUE(k && h && f);
        k->len = h->len = f->len = n;
        const double pat[6] = { bd(NEG0_64), 0.0, 1.5, bd(NEG0_64), 0.0, -2.0 };
        for (int64_t i = 0; i < n; i++) {
            ((int64_t*)ray_data(k))[i] = i;
            ((int64_t*)ray_data(h))[i] = i % 3;
            /* RLE: runs of 37 rows; DICT: scattered, so its codes child is
             * FOR_BP (runs would make it RLE, which the view declines) */
            const int64_t at = forces[fi] == RAY_ENC_RLE ? (i / 37) % 6 : (i * 7) % 6;
            ((double*)ray_data(f))[i] = i % 997 == 5 ? NULL_F64 : pat[at];
        }
        f->attrs |= RAY_ATTR_HAS_NULLS;
        ray_t* tb = ray_table_new(3);
        tb = ray_table_add_col(tb, ray_sym_intern("k", 1), k); ray_release(k);
        tb = ray_table_add_col(tb, ray_sym_intern("h", 1), h); ray_release(h);
        tb = ray_table_add_col(tb, ray_sym_intern("f", 1), f); ray_release(f);
        TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("W", 1), tb), RAY_OK);
        ray_release(tb);
        TEST_ASSERT_TRUE(fl_eval_ok("(.db.splayed.set \"" FL_TMP "/vc0/\" W {compress: false})"));
        ray_encw_test_force_float(forces[fi]);
        bool ok = fl_eval_ok("(.db.splayed.set \"" FL_TMP "/vc2/\" W {compress: true})");
        ray_encw_test_force_float(-1);
        TEST_ASSERT_TRUE(ok);
        /* every vector of f is a view of the forced encoding */
        TEST_ASSERT_TRUE(fl_eval_ok("(set T (.db.splayed.get \"" FL_TMP "/vc2/\"))"));
        ray_t* t = fl_eval("T");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_t* encv = ray_table_get_col_enc(t, ray_sym_intern("f", 1));
        TEST_ASSERT_NOT_NULL(encv);
        for (int64_t v = 0; v < (n + 1023) / 1024; v++) {
            ray_t* err = NULL;
            int r = ray_enccol_vview(ray_enccol_of(encv), v, &fl_vw, &err);
            if (err) ray_error_free(err);
            TEST_ASSERT_FMT(r == 1 && fl_vw.enc == forces[fi], "force %d vector %lld: view %d enc %d",
                            forces[fi], (long long)v, r, fl_vw.enc);
        }
        ray_release(t);
        static const char* const qs[] = {
            "(select {c: (count k) s: (sum k) from: T by: f})",
            "(select {c: (count k) from: T by: {f: f h: h}})",
            "(select {c: (count k) from: T by: f take: 2})",
            "(select {c: (count k) s: (sum k) from: T by: {f: f h: h} take: 4})",
            "(select {c: (count k) from: T by: {h: h f: f} take: 5})",
            "(take (at T 'f) 100)",
            "(select {f: f k: k from: T where: (> k 1000) take: 50})",
            "(count (select {from: T where: (== f 0.0)}))",
            "(count (select {from: T where: (== f -0.0)}))",
            "(count (select {from: T where: (!= f 0.0)}))",
            "(count (select {from: T where: (< f 0.0)}))",
            "(count (select {from: T where: (<= f -0.0)}))",
            "(count (select {from: T where: (>= f -0.0)}))",
            "(count (select {from: T where: (> f -1.0)}))",
            "(count (select {from: T where: (in f [0.0 1.5])}))",
            "(count (select {from: T where: (in f [-0.0])}))",
            "(count (select {from: T where: (not (in f [-2.0]))}))",
            "(sum (at T 'f))",
            "(select {s: (sum f) c: (count f) from: T where: (>= k 1000)})",
            "(select {s: (sum k) c: (count k) from: T where: (== f 0.0)})",
            "(select {s: (sum k) from: T where: (and (== f -0.0) (> k 500))})",
            "(select {s: (sum k) from: T where: (or (< f -1.0) (> f 1.0))})",
            NULL,
        };
        for (int i = 0; qs[i]; i++) FL_SAME(qs[i]);
        /* the fused filter answers f from its views: nothing of f decoded */
        TEST_ASSERT_TRUE(fl_eval_ok("(set T (.db.splayed.get \"" FL_TMP "/vc2/\"))"));
        ray_t* r = fl_eval("(select {k: k from: T where: (>= f 0.0)})");
        TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
        ray_release(r);
        t = fl_eval("T");
        encv = ray_table_get_col_enc(t, ray_sym_intern("f", 1));
        uint64_t fdec = encv ? atomic_load(&ray_enccol_of(encv)->decoded_bytes) : 1;
        ray_release(t);
        TEST_ASSERT_FMT(fdec == 0, "force %d: f decoded %llu bytes", forces[fi], (unsigned long long)fdec);
        TEST_ASSERT_TRUE(fl_eval_ok("(set T 0)"));
    }
    PASS();
}

/* Bytes decoded from column col of generation 2 (vc2) while q runs on a
 * fresh load of it (the column's own counter), or UINT64_MAX on an error. */
static uint64_t fl_q_decoded(const char* q, const char* col) {
    if (!fl_eval_ok("(set T (.db.splayed.get \"" FL_TMP "/vc2/\"))")) return UINT64_MAX;
    ray_t* r = fl_eval(q);
    if (!r || RAY_IS_ERR(r)) { if (r) ray_error_free(r); return UINT64_MAX; }
    ray_release(r);
    ray_t* t = fl_eval("T");
    if (!t || RAY_IS_ERR(t)) { if (t) ray_error_free(t); return UINT64_MAX; }
    ray_t* encv = ray_table_get_col_enc(t, ray_sym_intern(col, strlen(col)));
    uint64_t b = encv ? atomic_load(&ray_enccol_of(encv)->decoded_bytes) : UINT64_MAX;
    ray_release(t);
    return b;
}

/* compress_float.rfl's filters with the encoding forced and asserted, so
 * they run on views: F64 f and F32 g, forced CONST (one value per vector,
 * some nulls), RLE (runs of 3, the rfl's zs) and DICT (scattered), equal to
 * generation 0, and nothing of the filtered column decoded. */
static test_result_t test_filter_views(void) {
    const int64_t n = 70000;                                      /* two chunks */
    static const int forces[] = { RAY_ENC_CONST, RAY_ENC_RLE, RAY_ENC_DICT };
    const double dv[4] = { -3.5, bd(NEG0_64), 0.0, 2.25 };
    const double cv[3] = { bd(NEG0_64), 0.0, 2.25 };
    for (size_t fi = 0; fi < 3; fi++) {
        ray_t* k = ray_vec_new(RAY_I64, n);
        ray_t* f = ray_vec_new(RAY_F64, n);
        TEST_ASSERT_TRUE(k && f);
        k->len = f->len = n;
        double* fd = (double*)ray_data(f);
        for (int64_t i = 0; i < n; i++) {
            ((int64_t*)ray_data(k))[i] = i;
            fd[i] = forces[fi] == RAY_ENC_CONST ? cv[(i / 1024) % 3]
                  : forces[fi] == RAY_ENC_RLE   ? dv[(i / 3) % 4] : dv[(i * 7) % 4];
            if (i % 5000 == 7) fd[i] = NULL_F64;
        }
        f->attrs |= RAY_ATTR_HAS_NULLS;
        ray_t* g = fl_typed(RAY_F32, fd, n);
        ray_t* tb = ray_table_new(3);
        tb = ray_table_add_col(tb, ray_sym_intern("k", 1), k); ray_release(k);
        tb = ray_table_add_col(tb, ray_sym_intern("f", 1), f); ray_release(f);
        tb = ray_table_add_col(tb, ray_sym_intern("g", 1), g); ray_release(g);
        TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("W", 1), tb), RAY_OK);
        ray_release(tb);
        TEST_ASSERT_TRUE(fl_eval_ok("(.db.splayed.set \"" FL_TMP "/vc0/\" W {compress: false})"));
        ray_encw_test_force_float(forces[fi]);
        bool ok = fl_eval_ok("(.db.splayed.set \"" FL_TMP "/vc2/\" W {compress: true})");
        ray_encw_test_force_float(-1);
        TEST_ASSERT_TRUE(ok);
        /* every vector of f and g is a view of the forced encoding */
        TEST_ASSERT_TRUE(fl_eval_ok("(set T (.db.splayed.get \"" FL_TMP "/vc2/\"))"));
        ray_t* t = fl_eval("T");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        for (int ci = 0; ci < 2; ci++) {
            ray_t* encv = ray_table_get_col_enc(t, ray_sym_intern(ci ? "g" : "f", 1));
            TEST_ASSERT_NOT_NULL(encv);
            for (int64_t v = 0; v < (n + 1023) / 1024; v++) {
                ray_t* err = NULL;
                int r = ray_enccol_vview(ray_enccol_of(encv), v, &fl_vw, &err);
                if (err) ray_error_free(err);
                TEST_ASSERT_FMT(r == 1 && fl_vw.enc == forces[fi], "force %d column %s vector %lld: view %d enc %d",
                                forces[fi], ci ? "g" : "f", (long long)v, r, fl_vw.enc);
            }
        }
        ray_release(t);
        static const char* const conds[] = {
            "(== %s 0.0)", "(== %s -0.0)", "(< %s 0.0)", "(>= %s -0.0)", "(!= %s 0.0)",
            "(> %s 1.0)", "(<= %s -3.5)", NULL,
        };
        char cnd[64], q[256];
        for (int ci = 0; ci < 2; ci++)
            for (int i = 0; conds[i]; i++) {
                snprintf(cnd, sizeof cnd, conds[i], ci ? "g" : "f");
                snprintf(q, sizeof q, "(count (select {from: T where: %s}))", cnd);
                FL_SAME(q);
                snprintf(q, sizeof q, "(select {s: (sum k) c: (count k) from: T where: %s})", cnd);
                FL_SAME(q);
                snprintf(q, sizeof q, "(select {k: k from: T where: %s})", cnd);
                uint64_t dec = fl_q_decoded(q, ci ? "g" : "f");
                TEST_ASSERT_FMT(dec == 0, "force %d %s: decoded %llu bytes", forces[fi], q, (unsigned long long)dec);
            }
        TEST_ASSERT_TRUE(fl_eval_ok("(set T 0)"));
    }
    PASS();
}

const test_entry_t encfloat_entries[] = {
    { "encfloat/float_order",      test_float_order,      fl_setup, fl_teardown },
    { "encfloat/float_train_dict", test_float_train_dict, fl_setup, fl_teardown },
    { "encfloat/float_bodies",     test_float_bodies,     fl_setup, fl_teardown },
    { "encfloat/new_slots",        test_new_slots,        fl_setup, fl_teardown },
    { "encfloat/alp_decimals",     test_alp_decimals,     fl_setup, fl_teardown },
    { "encfloat/alp_signed",       test_alp_signed,       fl_setup, fl_teardown },
    { "encfloat/alp_edges",        test_alp_edges,        fl_setup, fl_teardown },
    { "encfloat/alp_rejects",      test_alp_rejects,      fl_setup, fl_teardown },
    { "encfloat/alprd_lengths",    test_alprd_lengths,    fl_setup, fl_teardown },
    { "encfloat/alprd_rejects",    test_alprd_rejects,    fl_setup, fl_teardown },
    { "encfloat/alprd_exceptions", test_alprd_exceptions, fl_setup, fl_teardown },
    { "encfloat/alprd_specials",   test_alprd_specials,   fl_setup, fl_teardown },
    { "encfloat/float_choice",     test_float_choice,     fl_setup, fl_teardown },
    { "encfloat/encoder_selects",      test_encoder_selects,      fl_setup, fl_teardown },
    { "encfloat/nan_payloads",         test_nan_payloads,         fl_setup, fl_teardown },
    { "encfloat/all_null_floats",      test_all_null_floats,      fl_setup, fl_teardown },
    { "encfloat/first_chunk_training", test_first_chunk_training, fl_setup, fl_teardown },
    { "encfloat/decode_paths",         test_decode_paths,         fl_setup, fl_teardown },
    { "encfloat/float_dict_rejects",   test_float_dict_rejects,   fl_setup, fl_teardown },
    { "encfloat/float_file_rejects",   test_float_file_rejects,   fl_setup, fl_teardown },
    { "encfloat/trailer_counts_in_choice", test_trailer_counts_in_choice, fl_setup, fl_teardown },
    { "encfloat/eval_f64",             test_eval_f64,             fl_setup, fl_teardown },
    { "encfloat/const_decision_f64",   test_const_decision_f64,   fl_setup, fl_teardown },
    { "encfloat/view_checks",          test_view_checks,          fl_setup, fl_teardown },
    { "encfloat/view_consumers",       test_view_consumers,       fl_rt_setup, fl_rt_teardown },
    { "encfloat/filter_views",         test_filter_views,         fl_rt_setup, fl_rt_teardown },
    { NULL, NULL, NULL, NULL },
};
