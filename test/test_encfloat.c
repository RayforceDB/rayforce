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
static void fl_teardown(void) {
    ray_enc_buf_free(&fl_buf); fl_buf = (ray_enc_buf_t){0};
    if (fl_dict) { ray_free_raw(fl_dict); fl_dict = NULL; }
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
    TEST_ASSERT_TRUE(ray_enc_lt(db(-1.0), db(-0.0), K64F));
    TEST_ASSERT_TRUE(ray_enc_lt(db(-0.0), db(0.0), K64F));
    TEST_ASSERT_FALSE(ray_enc_lt(db(0.0), db(-0.0), K64F));
    TEST_ASSERT_FALSE(ray_enc_lt(db(0.0), db(0.0), K64F));
    TEST_ASSERT_TRUE(ray_enc_lt(db(0.0), db(1e-300), K64F));
    TEST_ASSERT_TRUE(ray_enc_lt(fb(-0.0f), fb(0.0f), K32F));
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
    uint64_t v[7] = { db(2.25), db(0.0), db(-3.5), db(-0.0), db(0.0), db(2.25), db(-0.0) };
    uint64_t* d = NULL; uint32_t dn;
    int rc = ray_enc_train_dict(v, 7, K64F, &d, &dn);
    if (rc == 1) fl_dict = d;                                   /* freed in teardown */
    TEST_ASSERT_EQ_I(rc, 1);
    TEST_ASSERT_EQ_I(dn, 4);
    TEST_ASSERT_EQ_U(d[0], db(-3.5)); TEST_ASSERT_EQ_U(d[1], db(-0.0));
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
    uint64_t dv[4] = { db(-3.5), db(-0.0), db(0.0), db(2.25) };
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

/* The new slots: ALP ints (U64) admit DELTA_BP and RLE, ALP_RD left (U8)
 * admits RLE and FOR_BP and not DELTA_BP, and the reader enforces it. */
static test_result_t test_new_slots(void) {
    uint64_t v[RAY_ENC_VEC];
    ray_enc_kind_t k64 = { 64, false, false }, k8 = { 8, false, false };
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

const test_entry_t encfloat_entries[] = {
    { "encfloat/float_order",      test_float_order,      fl_setup, fl_teardown },
    { "encfloat/float_train_dict", test_float_train_dict, fl_setup, fl_teardown },
    { "encfloat/float_bodies",     test_float_bodies,     fl_setup, fl_teardown },
    { "encfloat/new_slots",        test_new_slots,        fl_setup, fl_teardown },
    { NULL, NULL, NULL, NULL },
};
