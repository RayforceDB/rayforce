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
#include "store/col.h"
#include "store/encfmt.h"
#include "store/encw.h"
#include "store/enccol.h"
#include "store/stream.h"
#include "vec/str.h"
#include "ops/idxop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENCFMT_TMP "/tmp/rayforce_encfmt"

static void encfmt_setup(void)    { ray_heap_init(); (void)ray_sym_init(); (void)ray_test_mkdir_p(ENCFMT_TMP); }
static void encfmt_teardown(void) { ray_encw_test_force(-1); ray_sym_destroy(); ray_heap_destroy(); (void)ray_test_rm_rf(ENCFMT_TMP); }

static void hdr_set(uint8_t h[32], uint8_t order, uint32_t aux0) {
    memset(h, 0, 32);
    memcpy(h, &aux0, 4);
    h[17] = order;
}

static test_result_t test_discriminator_pairs(void) {
    uint8_t h[32];
    hdr_set(h, 2, RAY_ENC_XENC); TEST_ASSERT_EQ_I(ray_col_gen_of(h), 2);
    hdr_set(h, 0, 0);            TEST_ASSERT_EQ_I(ray_col_gen_of(h), 0);
    hdr_set(h, 0, RAY_ENC_XIDX); TEST_ASSERT_EQ_I(ray_col_gen_of(h), 0);
    hdr_set(h, 0, RAY_ENC_XENC); TEST_ASSERT_EQ_I(ray_col_gen_of(h), RAY_ENC_GEN_CORRUPT);
    hdr_set(h, 2, RAY_ENC_XIDX); TEST_ASSERT_EQ_I(ray_col_gen_of(h), RAY_ENC_GEN_CORRUPT);
    hdr_set(h, 2, 0);            TEST_ASSERT_EQ_I(ray_col_gen_of(h), RAY_ENC_GEN_CORRUPT);
    hdr_set(h, 0, 0x01020304u);  TEST_ASSERT_EQ_I(ray_col_gen_of(h), RAY_ENC_GEN_CORRUPT);
    hdr_set(h, 1, 0);            TEST_ASSERT_EQ_I(ray_col_gen_of(h), RAY_ENC_GEN_VERSION);
    PASS();
}

/* An I64 column file of 4 rows in the generation-0 layout (32 + 32 bytes),
 * with its header bytes patched to a cross pair: the loader must say
 * corrupt, never load it as generation 0. */
static ray_t* write_g0_patched(const char* path, uint8_t order, uint32_t aux0) {
    int64_t d[4] = { 1, 2, 3, 4 };
    ray_t* v = ray_vec_from_raw(RAY_I64, d, 4);
    if (!v || RAY_IS_ERR(v)) return v;
    ray_err_t e = ray_col_save(v, path);
    ray_release(v);
    if (e != RAY_OK) return ray_error(ray_err_code_str(e), NULL);
    FILE* f = fopen(path, "r+b");
    if (!f) return ray_error("io", NULL);
    bool ok = fwrite(&aux0, 4, 1, f) == 1 && fseek(f, 17, SEEK_SET) == 0 &&
              fwrite(&order, 1, 1, f) == 1;
    if (fclose(f) != 0) ok = false;
    if (!ok) return ray_error("io", NULL);
    return ray_col_mmap(path);
}

static test_result_t test_discriminator_cross_pairs(void) {
    static const struct { uint8_t order; uint32_t aux0; const char* code; } cases[] = {
        { 0, RAY_ENC_XENC, "corrupt" },
        { 2, RAY_ENC_XIDX, "corrupt" },
        { 2, 0,            "corrupt" },
        { 0, 0x01020304u,  "corrupt" },
        { 1, 0,            "version" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ray_t* r = write_g0_patched(ENCFMT_TMP "/x", cases[i].order, cases[i].aux0);
        bool ok1 = r && RAY_IS_ERR(r) && strcmp(ray_err_code(r), cases[i].code) == 0;
        const char* got1 = (r && RAY_IS_ERR(r)) ? ray_err_code(r) : "(loaded)";
        char got[16]; snprintf(got, sizeof got, "%s", got1);
        if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
        TEST_ASSERT_FMT(ok1, "mmap case %zu: got %s want %s", i, got, cases[i].code);
        r = ray_col_load(ENCFMT_TMP "/x");
        bool ok2 = r && RAY_IS_ERR(r) && strcmp(ray_err_code(r), cases[i].code) == 0;
        if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
        TEST_ASSERT_FMT(ok2, "load case %zu", i);
    }
    PASS();
}

/* A nullable I64 vector whose in-memory aux[0..3] holds junk: the writer
 * must not let it reach the file (it would read back as a cross pair). */
static test_result_t test_gen0_writer_zeroes_aux(void) {
    int64_t d[4] = { 1, 2, INT64_MIN, 4 };
    ray_t* v = ray_vec_from_raw(RAY_I64, d, 4);
    TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v));
    v->attrs |= RAY_ATTR_HAS_NULLS;
    v->aux[0] = 0xDE; v->aux[1] = 0xAD; v->aux[2] = 0xBE; v->aux[3] = 0xEF;
    ray_err_t e = ray_col_save(v, ENCFMT_TMP "/j");
    ray_release(v);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    uint8_t b[4] = { 1, 1, 1, 1 };
    FILE* f = fopen(ENCFMT_TMP "/j", "rb");
    TEST_ASSERT_TRUE(f != NULL);
    size_t n = fread(b, 1, 4, f);
    fclose(f);
    TEST_ASSERT_EQ_I((int)n, 4);
    TEST_ASSERT_TRUE(b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0);
    ray_t* r = ray_col_load(ENCFMT_TMP "/j");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->len, 4);
    ray_release(r);
    PASS();
}

static test_result_t test_length_helpers(void) {
    TEST_ASSERT_EQ_I(ray_enc_nchunks(0), 0);
    TEST_ASSERT_EQ_I(ray_enc_nchunks(1), 1);
    TEST_ASSERT_EQ_I(ray_enc_nchunks(65536), 1);
    TEST_ASSERT_EQ_I(ray_enc_nchunks(65537), 2);
    TEST_ASSERT_EQ_I(ray_enc_nvecs(1025), 2);
    TEST_ASSERT_EQ_I(ray_enc_vec_len(1025, 0), 1024);
    TEST_ASSERT_EQ_I(ray_enc_vec_len(1025, 1), 1);
    TEST_ASSERT_EQ_I(ray_enc_vec_len(2048, 1), 1024);
    TEST_ASSERT_EQ_U(ray_enc_pad8(9), 16);
    TEST_ASSERT_EQ_U(ray_enc_pad32(33), 64);
    TEST_ASSERT_EQ_I(RAY_ENC_LE, 1);
    PASS();
}


static uint64_t f64_bits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }

static test_result_t test_vec_i32_for(void) {
    int32_t d[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d[i] = 5000 + (i % 64);
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_I32, 0, d, NULL, 0, RAY_ENC_VEC, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_FOR_BP);
    TEST_ASSERT_EQ_I(v.desc.width, 6);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 0);
    TEST_ASSERT_EQ_I(v.desc.flags, 0);
    TEST_ASSERT_EQ_I(v.desc.min, 5000);
    TEST_ASSERT_EQ_I(v.desc.max, 5063);
    TEST_ASSERT_EQ_U(v.payload.len, 8 + 1024 * 6 / 8);
    TEST_ASSERT_EQ_U(v.desc.payload_bytes, v.payload.len);
    ray_encw_vec_free(&v);
    PASS();
}

static test_result_t test_vec_i64_nulls_bitmap_and_sum(void) {
    int64_t d[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d[i] = 7;
    d[3] = NULL_I64; d[1000] = NULL_I64; d[0] = INT64_MAX; d[1] = INT64_MAX;
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_I64, 0, d, NULL, 0, RAY_ENC_VEC, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.n_nulls, 2);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 2);
    TEST_ASSERT_EQ_U(v.payload.p[0], 0x08);            /* row 3 */
    TEST_ASSERT_EQ_U(v.payload.p[125], 0x01);          /* row 1000 = byte 125 bit 0 */
    TEST_ASSERT_EQ_I(v.desc.min, 7);
    TEST_ASSERT_EQ_I(v.desc.max, INT64_MAX);
    /* 2*INT64_MAX + 1020*7 = 2^64 + 7138 */
    TEST_ASSERT_EQ_I(v.sum_hi, 1);
    TEST_ASSERT_EQ_U(v.sum_lo, 7138);
    ray_encw_vec_free(&v);
    PASS();
}

static test_result_t test_vec_all_null_is_const(void) {
    int16_t d[3] = { NULL_I16, NULL_I16, NULL_I16 };
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_I16, 0, d, NULL, 0, 3, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(v.desc.flags, RAY_ENC_STAT_ABSENT);
    TEST_ASSERT_EQ_I(v.desc.min, 0);
    TEST_ASSERT_EQ_I(v.desc.max, 0);
    TEST_ASSERT_EQ_U(v.payload.len, 128 + 8);
    TEST_ASSERT_EQ_U(v.payload.p[0], 0x07);
    for (size_t i = 128; i < 136; i++) TEST_ASSERT_EQ_U(v.payload.p[i], 0);
    TEST_ASSERT_FALSE(v.has_stats);
    ray_encw_vec_free(&v);
    PASS();
}

/* Canonical NaN at row 1, a NaN with another payload at row 2: both are
 * nulls in the bitmap; only row 2 goes to the trailer with its raw bits. */
static test_result_t test_vec_f64_trailer(void) {
    double d[4]; uint64_t odd = 0x7FF8000000000001ull;
    d[0] = 2.5; d[1] = NULL_F64; memcpy(&d[2], &odd, 8); d[3] = -1.0;
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_F64, 0, d, NULL, 0, 4, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 2);
    TEST_ASSERT_EQ_I(v.desc.flags, RAY_ENC_DESC_TRAILER);
    TEST_ASSERT_EQ_U((uint64_t)v.desc.min, f64_bits(-1.0));
    TEST_ASSERT_EQ_U((uint64_t)v.desc.max, f64_bits(2.5));
    /* bitmap 128 | RAW 4*8 (nulls filled with the minimum -1.0) | trailer */
    const uint8_t* p = v.payload.p;
    TEST_ASSERT_EQ_U(p[0], 0x06);
    uint64_t b1; memcpy(&b1, p + 128 + 8, 8);
    TEST_ASSERT_EQ_U(b1, f64_bits(-1.0));
    const uint8_t* t = p + 128 + 32;
    uint16_t cnt; memcpy(&cnt, t, 2);
    TEST_ASSERT_EQ_I(cnt, 1);
    uint16_t pos; memcpy(&pos, t + 8, 2);
    TEST_ASSERT_EQ_I(pos, 2);
    uint64_t raw; memcpy(&raw, t + 16, 8);            /* positions padded to 8 */
    TEST_ASSERT_EQ_U(raw, odd);
    TEST_ASSERT_EQ_U(v.payload.len, 128 + 32 + 8 + 8 + 8);
    ray_encw_vec_free(&v);
    PASS();
}

/* CONST merges equal bit patterns only: -0.0 and +0.0 stay RAW. */
static test_result_t test_vec_f64_const_and_zeros(void) {
    double d[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d[i] = 1.5;
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_F64, 0, d, NULL, 0, RAY_ENC_VEC, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_CONST);
    TEST_ASSERT_EQ_U(v.payload.len, 8);
    d[0] = -0.0; d[1] = 0.0;
    for (int i = 2; i < RAY_ENC_VEC; i++) d[i] = 0.0;
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_F64, 0, d, NULL, 0, RAY_ENC_VEC, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_RAW);
    TEST_ASSERT_EQ_U((uint64_t)v.desc.min, f64_bits(-0.0));   /* first of the tied zeros */
    TEST_ASSERT_EQ_U((uint64_t)v.desc.max, f64_bits(-0.0));
    ray_encw_vec_free(&v);
    PASS();
}

/* A CONST vector of equal values plus two non-canonical NaNs: the trailer
 * counts in both estimates, (8 + tb) * 8 <= (raw + tb) * 7. */
static test_result_t test_vec_f64_const_with_trailer(void) {
    double d[8]; uint64_t odd = 0x7FF8000000000005ull;
    for (int i = 0; i < 8; i++) d[i] = 4.0;
    memcpy(&d[3], &odd, 8); memcpy(&d[6], &odd, 8);
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_F64, 0, d, NULL, 0, 8, NULL, &v), RAY_OK);
    /* raw 64, tb = 8 + 8 + 16 = 32: (8+32)*8 = 320 <= (64+32)*7 = 672 */
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(v.desc.flags, RAY_ENC_DESC_TRAILER);
    TEST_ASSERT_EQ_U(v.payload.len, 128 + 8 + 32);
    ray_encw_vec_free(&v);
    PASS();
}

static test_result_t test_vec_forced(void) {
    int32_t d[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d[i] = 5000 + (i % 64);
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    ray_encw_test_force(RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_I32, 0, d, NULL, 0, RAY_ENC_VEC, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_RAW);
    ray_encw_test_force(RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_I32, 0, d, NULL, 0, RAY_ENC_VEC, NULL, &v), RAY_ERR_NYI);
    ray_encw_test_force(-1);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_I32, 0, d, NULL, 0, RAY_ENC_VEC, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_FOR_BP);
    ray_encw_vec_free(&v);
    PASS();
}

static test_result_t test_vec_guid(void) {
    uint8_t g[4][16]; memset(g, 0, sizeof g);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 16; j++) g[i][j] = (uint8_t)(j + 1);
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_GUID, 0, g, NULL, 0, 4, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_CONST);
    TEST_ASSERT_EQ_U(v.payload.len, 16);
    TEST_ASSERT_EQ_I(v.desc.flags, RAY_ENC_STAT_ABSENT);
    memset(g[2], 0, 16);                                 /* in-band null */
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_GUID, 0, g, NULL, 0, 4, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 1);
    TEST_ASSERT_EQ_U(v.payload.len, 64);
    ray_encw_vec_free(&v);
    PASS();
}

static ray_t* str_vec3(const char* a, const char* b, const char* c) {
    ray_t* s = ray_vec_new(RAY_STR, 3);
    s = ray_str_vec_append(s, a, strlen(a));
    s = ray_str_vec_append(s, b, strlen(b));
    return ray_str_vec_append(s, c, strlen(c));
}

static test_result_t test_vec_str_raw_and_const(void) {
    ray_t* s = str_vec3("", "short", "a very long string 1");    /* 0, 5, 20 bytes */
    const char* pool = s->str_pool ? (const char*)ray_data(s->str_pool) : NULL;
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    ray_err_t rc = ray_encw_vec(RAY_STR, 0, ray_data(s), pool, 0, 3, NULL, &v);
    if (rc != RAY_OK) { ray_release(s); ray_encw_vec_free(&v); }
    TEST_ASSERT_EQ_I(rc, RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 1);
    TEST_ASSERT_EQ_I(v.desc.flags, RAY_ENC_STAT_ABSENT);
    TEST_ASSERT_EQ_U(v.str_pooled, 20);
    TEST_ASSERT_EQ_U(v.payload.len, 3 * 16 + 8 + 24);
    const uint8_t* p = v.payload.p;
    uint32_t len; memcpy(&len, p + 16, 4);
    TEST_ASSERT_EQ_I(len, 5);
    TEST_ASSERT_TRUE(memcmp(p + 20, "short\0\0\0\0\0\0\0", 12) == 0);  /* zero padded */
    memcpy(&len, p + 32, 4);
    TEST_ASSERT_EQ_I(len, 20);
    TEST_ASSERT_TRUE(memcmp(p + 36, "a ve", 4) == 0);                  /* prefix */
    uint32_t off, h; memcpy(&off, p + 40, 4); memcpy(&h, p + 44, 4);
    TEST_ASSERT_EQ_I(off, 0);
    TEST_ASSERT_EQ_I(h, 0);                                            /* hash32 0 */
    uint64_t pb; memcpy(&pb, p + 48, 8);
    TEST_ASSERT_EQ_U(pb, 20);
    TEST_ASSERT_TRUE(memcmp(p + 56, "a very long string 1", 20) == 0);
    ray_release(s);

    s = str_vec3("same long string here", "same long string here", "same long string here");
    pool = (const char*)ray_data(s->str_pool);
    rc = ray_encw_vec(RAY_STR, 0, ray_data(s), pool, 0, 3, NULL, &v);
    if (rc != RAY_OK) { ray_release(s); ray_encw_vec_free(&v); }
    TEST_ASSERT_EQ_I(rc, RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_CONST);
    TEST_ASSERT_EQ_U(v.str_pooled, 3 * 21);                   /* once per occurrence */
    TEST_ASSERT_EQ_U(v.payload.len, 8 + 24);
    memcpy(&len, v.payload.p, 4);
    TEST_ASSERT_EQ_I(len, 21);
    ray_release(s);
    ray_encw_vec_free(&v);
    PASS();
}

static test_result_t test_vec_sym_w8_inband(void) {
    uint8_t d[4] = { 0, 127, 128, 255 };
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_SYM, RAY_SYM_W8, d, NULL, 0, 4, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 1);
    TEST_ASSERT_EQ_I(v.desc.min, 127);                 /* id 0 is the null, excluded */
    TEST_ASSERT_EQ_I(v.desc.max, 255);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_RAW);
    TEST_ASSERT_EQ_U(v.payload.len, 8);                /* no bitmap for in-band nulls */
    ray_encw_vec_free(&v);
    PASS();
}

static test_result_t test_vec_bool_rle(void) {
    uint8_t d[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d[i] = (uint8_t)((i / 100) & 1);
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_BOOL, 0, d, NULL, 0, RAY_ENC_VEC, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_RLE);
    TEST_ASSERT_EQ_I(v.desc.min, 0);
    TEST_ASSERT_EQ_I(v.desc.max, 1);
    TEST_ASSERT_EQ_I(v.sum_hi, 0);
    TEST_ASSERT_EQ_U(v.sum_lo, 500);                   /* rows 100..199, 300..399, ... */
    ray_encw_vec_free(&v);
    PASS();
}

#define RT(call) do { test_result_t r_ = (call); if (r_.status != TEST_PASS) return r_; } while (0)

/* Decode the codec body of a vector payload and compare with what the
 * loader fed the encoder (null slots hold the fill). */
static test_result_t roundtrip(int8_t type, uint8_t attrs, const void* data, int n,
                               const ray_enc_dict_t* dict, int force, int want_enc) {
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    ray_encw_test_force(force);
    ray_err_t rc = ray_encw_vec(type, attrs, data, NULL, 0, n, dict, &v);
    ray_encw_test_force(-1);
    TEST_ASSERT_EQ_I(rc, RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, want_enc);
    ray_enc_input_t* in = malloc(sizeof *in);
    uint64_t* out = malloc(RAY_ENC_VEC * sizeof *out);
    TEST_ASSERT_TRUE(in && out);
    TEST_ASSERT_TRUE(ray_enc_load(data, type, attrs, 0, n, in));
    size_t off = (ray_enc_has_bitmap(type) && v.n_nulls > 0) ? 128 : 0;
    int64_t used = ray_enc_read(v.payload.p + off, v.payload.len - off, v.desc.encoding,
                                v.desc.width, n, in->k, RAY_ENC_SLOT_TOP, 0, dict, out);
    TEST_ASSERT_TRUE(used > 0);
    int bad = -1;
    for (int i = 0; i < n && bad < 0; i++) if (out[i] != in->bits[i]) bad = i;
    TEST_ASSERT_EQ_I(bad, -1);
    free(in); free(out);
    ray_encw_vec_free(&v);
    PASS();
}

static test_result_t test_vec_roundtrip_codecs(void) {
    int32_t a[RAY_ENC_VEC]; int64_t b[RAY_ENC_VEC]; uint8_t bo[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) a[i] = 5000 + (i % 64);
    a[7] = NULL_I32; a[900] = NULL_I32;
    RT(roundtrip(RAY_I32, 0, a, RAY_ENC_VEC, NULL, RAY_ENC_RAW, RAY_ENC_RAW));
    RT(roundtrip(RAY_I32, 0, a, RAY_ENC_VEC, NULL, -1, RAY_ENC_FOR_BP));
    RT(roundtrip(RAY_I32, 0, a, RAY_ENC_VEC, NULL, RAY_ENC_FOR_BP, RAY_ENC_FOR_BP));
    for (int i = 0; i < RAY_ENC_VEC; i++) a[i] = 42;
    RT(roundtrip(RAY_I32, 0, a, RAY_ENC_VEC, NULL, -1, RAY_ENC_CONST));
    for (int i = 0; i < RAY_ENC_VEC; i++) b[i] = 1000 + (int64_t)i * 3;
    b[5] = NULL_I64;
    RT(roundtrip(RAY_I64, 0, b, RAY_ENC_VEC, NULL, RAY_ENC_DELTA_BP, RAY_ENC_DELTA_BP));
    for (int i = 0; i < RAY_ENC_VEC; i++) b[i] = (int64_t)(i / 100) * 1000003;
    RT(roundtrip(RAY_I64, 0, b, RAY_ENC_VEC, NULL, RAY_ENC_RLE, RAY_ENC_RLE));
    for (int i = 0; i < RAY_ENC_VEC; i++) bo[i] = (uint8_t)((i / 100) & 1);
    RT(roundtrip(RAY_BOOL, 0, bo, RAY_ENC_VEC, NULL, -1, RAY_ENC_RLE));
    RT(roundtrip(RAY_U8, 0, bo, RAY_ENC_VEC, NULL, RAY_ENC_RAW, RAY_ENC_RAW));
    PASS();
}

static test_result_t test_vec_roundtrip_dict(void) {
    int32_t a[RAY_ENC_VEC];
    static const int32_t vals[3] = { 10, 1000000, 2000000000 };
    uint64_t dv[3] = { 10, 1000000, 2000000000 };
    ray_enc_dict_t dict = { dv, 3 };
    for (int i = 0; i < RAY_ENC_VEC; i++) a[i] = vals[(i * 7) % 3 == 0 ? 0 : (i % 2) + 1];
    RT(roundtrip(RAY_I32, 0, a, RAY_ENC_VEC, &dict, -1, RAY_ENC_DICT));
    RT(roundtrip(RAY_I32, 0, a, RAY_ENC_VEC, &dict, RAY_ENC_DICT, RAY_ENC_DICT));
    /* a value outside the dictionary cannot be forced onto DICT */
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    a[3] = 11;
    ray_encw_test_force(RAY_ENC_DICT);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_I32, 0, a, NULL, 0, RAY_ENC_VEC, &dict, &v), RAY_ERR_NYI);
    ray_encw_test_force(-1);
    /* SYM and BOOL ignore a dictionary */
    uint8_t sy[RAY_ENC_VEC], bo[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) { sy[i] = (uint8_t)(1 + (i * 7) % 3); bo[i] = (uint8_t)(i & 1); }
    uint64_t sd[3] = { 1, 2, 3 }, bd[2] = { 0, 1 };
    ray_enc_dict_t sdict = { sd, 3 }, bdict = { bd, 2 };
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_SYM, RAY_SYM_W8, sy, NULL, 0, RAY_ENC_VEC, &sdict, &v), RAY_OK);
    TEST_ASSERT_TRUE(v.desc.encoding != RAY_ENC_DICT);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_BOOL, 0, bo, NULL, 0, RAY_ENC_VEC, &bdict, &v), RAY_OK);
    TEST_ASSERT_TRUE(v.desc.encoding != RAY_ENC_DICT);
    ray_encw_vec_free(&v);
    PASS();
}

static test_result_t test_vec_sums_literal(void) {
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    int32_t neg[3] = { -5, -6, 3 };
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_I32, 0, neg, NULL, 0, 3, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.sum_hi, -1);
    TEST_ASSERT_EQ_U(v.sum_lo, 0xFFFFFFFFFFFFFFF8ull);        /* -8 */
    int32_t tm[3] = { 1000, 2000, NULL_I32 };
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_TIME, 0, tm, NULL, 0, 3, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.sum_hi, 0);
    TEST_ASSERT_EQ_U(v.sum_lo, 3000);
    int32_t dt[3] = { 1, 2, 3 };
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_DATE, 0, dt, NULL, 0, 3, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.sum_hi, 0);
    TEST_ASSERT_EQ_U(v.sum_lo, 0);
    int64_t ts[3] = { 1, 2, 3 };
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_TIMESTAMP, 0, ts, NULL, 0, 3, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.sum_hi, 0);
    TEST_ASSERT_EQ_U(v.sum_lo, 0);
    ray_encw_vec_free(&v);
    PASS();
}

static test_result_t test_vec_f32_trailer(void) {
    float d[4]; uint32_t odd = 0x7FC00001u;
    d[0] = 1.5f; d[1] = NULL_F32; memcpy(&d[2], &odd, 4); d[3] = -2.0f;
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_F32, 0, d, NULL, 0, 4, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 2);
    TEST_ASSERT_EQ_I(v.desc.flags, RAY_ENC_DESC_TRAILER);
    TEST_ASSERT_EQ_U((uint64_t)v.desc.min, f64_bits(-2.0));   /* widened to double */
    TEST_ASSERT_EQ_U((uint64_t)v.desc.max, f64_bits(1.5));
    /* bitmap 128 | RAW 4*4 | trailer: count 8, position 8, raw 8 */
    TEST_ASSERT_EQ_U(v.payload.len, 128 + 16 + 8 + 8 + 8);
    const uint8_t* p = v.payload.p;
    TEST_ASSERT_EQ_U(p[0], 0x06);
    float fill; memcpy(&fill, p + 128 + 4, 4);
    TEST_ASSERT_TRUE(fill == -2.0f);
    uint16_t cnt, pos; uint32_t raw;
    memcpy(&cnt, p + 144, 2); memcpy(&pos, p + 152, 2); memcpy(&raw, p + 160, 4);
    TEST_ASSERT_EQ_I(cnt, 1);
    TEST_ASSERT_EQ_I(pos, 2);
    TEST_ASSERT_EQ_U(raw, odd);
    ray_encw_vec_free(&v);
    PASS();
}

static test_result_t test_vec_all_null_kinds(void) {
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    ray_t* s = str_vec3("", "", "");
    ray_err_t rc = ray_encw_vec(RAY_STR, 0, ray_data(s), NULL, 0, 3, NULL, &v);
    ray_release(s);
    TEST_ASSERT_EQ_I(rc, RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 3);
    TEST_ASSERT_EQ_U(v.payload.len, 8);
    uint8_t g[4][16]; memset(g, 0, sizeof g);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_GUID, 0, g, NULL, 0, 4, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 4);
    TEST_ASSERT_EQ_U(v.payload.len, 16);
    double f[3] = { NULL_F64, NULL_F64, NULL_F64 };
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_F64, 0, f, NULL, 0, 3, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 3);
    TEST_ASSERT_EQ_I(v.desc.flags, RAY_ENC_STAT_ABSENT);
    TEST_ASSERT_EQ_U(v.payload.len, 128 + 8);
    /* a forced non-RAW/CONST encoding leaves an all-null vector CONST */
    ray_encw_test_force(RAY_ENC_DICT);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_F64, 0, f, NULL, 0, 3, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_GUID, 0, g, NULL, 0, 4, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_CONST);
    ray_encw_test_force(-1);
    /* all-null with a non-canonical NaN: the raw bits go to the trailer */
    uint64_t odd = 0x7FF8000000000001ull;
    double h[3]; h[0] = NULL_F64; memcpy(&h[1], &odd, 8); h[2] = NULL_F64;
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_F64, 0, h, NULL, 0, 3, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.encoding, RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(v.desc.flags, RAY_ENC_STAT_ABSENT | RAY_ENC_DESC_TRAILER);
    TEST_ASSERT_EQ_U(v.payload.len, 128 + 8 + 8 + 8 + 8);
    uint64_t raw; memcpy(&raw, v.payload.p + 128 + 8 + 16, 8);
    TEST_ASSERT_EQ_U(raw, odd);
    ray_encw_vec_free(&v);
    PASS();
}

static test_result_t test_vec_sym_w16_w32(void) {
    ray_encw_vec_t v; ray_encw_vec_init(&v);
    uint16_t d16[3] = { 0, 5, 1000 };
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_SYM, RAY_SYM_W16, d16, NULL, 0, 3, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 1);
    TEST_ASSERT_EQ_I(v.desc.min, 5);
    TEST_ASSERT_EQ_I(v.desc.max, 1000);
    uint32_t d32[3] = { 7, 0, 70000 };
    TEST_ASSERT_EQ_I(ray_encw_vec(RAY_SYM, RAY_SYM_W32, d32, NULL, 0, 3, NULL, &v), RAY_OK);
    TEST_ASSERT_EQ_I(v.desc.n_nulls, 1);
    TEST_ASSERT_EQ_I(v.desc.min, 7);
    TEST_ASSERT_EQ_I(v.desc.max, 70000);
    ray_encw_vec_free(&v);
    RT(roundtrip(RAY_SYM, RAY_SYM_W16, d16, 3, NULL, RAY_ENC_RAW, RAY_ENC_RAW));
    PASS();
}


static uint8_t* read_file(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t* b = malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    *n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (*n != (size_t)sz) { free(b); return NULL; }
    return b;
}

static test_result_t test_file_headers(void) {
    int32_t d[1025];
    for (int i = 0; i < 1025; i++) d[i] = (i % 10 == 0) ? NULL_I32 : i;
    ray_t* v = ray_vec_from_raw(RAY_I32, d, 1025);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/h", 2), RAY_OK);
    ray_release(v);
    size_t n; uint8_t* b = read_file(ENCFMT_TMP "/h", &n);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQ_I(ray_col_gen_of(b), 2);
    ray_t hdr; memcpy(&hdr, b, 32);
    TEST_ASSERT_EQ_I(hdr.type, RAY_I32);
    TEST_ASSERT_EQ_I(hdr.len, 1025);
    TEST_ASSERT_TRUE(hdr.attrs & RAY_ATTR_HAS_NULLS);
    ray_enc_hdr_t eh; memcpy(&eh, b + 32, 64);
    TEST_ASSERT_TRUE(memcmp(eh.magic, "RENC", 4) == 0);
    TEST_ASSERT_EQ_I(eh.header_version, 1);
    TEST_ASSERT_EQ_U(eh.end_off, n);
    TEST_ASSERT_EQ_U(eh.side_off, 0);
    TEST_ASSERT_EQ_U(eh.index_off, 0);
    ray_enc_dirent_t de; memcpy(&de, b + 96, 64);
    TEST_ASSERT_EQ_U(de.offset, 96 + 64);
    TEST_ASSERT_EQ_U(de.offset + de.bytes, n);
    TEST_ASSERT_EQ_I(de.null_count, 103);              /* rows 0,10,...,1020 */
    TEST_ASSERT_EQ_I(de.min, 1);
    TEST_ASSERT_EQ_I(de.max, 1024);
    free(b);
    PASS();
}

static test_result_t test_trailing_descriptors_zero(void) {
    int32_t d[1025];
    for (int i = 0; i < 1025; i++) d[i] = i;
    ray_t* v = ray_vec_from_raw(RAY_I32, d, 1025);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/t", 2), RAY_OK);
    ray_release(v);
    size_t n; uint8_t* b = read_file(ENCFMT_TMP "/t", &n);
    const uint8_t* c = b + 160;                         /* chunk 0 descriptors */
    ray_enc_desc_t d0, d1; memcpy(&d0, c, 48); memcpy(&d1, c + 48, 48);
    TEST_ASSERT_EQ_U(d0.payload_off, RAY_ENC_DESC_AREA);
    TEST_ASSERT_EQ_U(d1.payload_off, ray_enc_pad8(d0.payload_off + d0.payload_bytes));
    for (int j = 2; j < 64; j++)
        for (int k = 0; k < 48; k++) TEST_ASSERT_FMT(c[j * 48 + k] == 0, "desc %d byte %d", j, k);
    free(b);
    PASS();
}

/* Chunk 0 holds INT64_MIN+1, INT64_MAX and nulls; chunk 1 is all null. */
static test_result_t test_dir_sums_extremes(void) {
    int64_t len = 65536 + 10;
    ray_t* v = ray_vec_new(RAY_I64, len);
    v->len = len;
    int64_t* d = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < len; i++) d[i] = NULL_I64;
    d[0] = INT64_MIN + 1; d[1] = INT64_MAX; d[2] = INT64_MAX; d[5] = -3;
    v->attrs |= RAY_ATTR_HAS_NULLS;
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/s", 2), RAY_OK);
    ray_release(v);
    size_t n; uint8_t* b = read_file(ENCFMT_TMP "/s", &n);
    ray_enc_dirent_t e0, e1; memcpy(&e0, b + 96, 64); memcpy(&e1, b + 160, 64);
    int64_t hi = 0; uint64_t lo = 0;
    ray_i128_add(&hi, &lo, INT64_MIN + 1); ray_i128_add(&hi, &lo, INT64_MAX);
    ray_i128_add(&hi, &lo, INT64_MAX);     ray_i128_add(&hi, &lo, -3);
    TEST_ASSERT_EQ_U(e0.sum_lo, lo);
    TEST_ASSERT_EQ_I(e0.sum_hi, hi);
    TEST_ASSERT_EQ_I(e0.null_count, 65536 - 4);
    TEST_ASSERT_EQ_I(e0.min, INT64_MIN + 1);
    TEST_ASSERT_EQ_I(e0.max, INT64_MAX);
    TEST_ASSERT_EQ_I(e0.flags, 0);
    TEST_ASSERT_EQ_I(e1.null_count, 10);
    TEST_ASSERT_EQ_I(e1.flags, RAY_ENC_STAT_ABSENT);
    TEST_ASSERT_EQ_I(e1.min, 0);
    TEST_ASSERT_EQ_I(e1.max, 0);
    TEST_ASSERT_EQ_U(e1.sum_lo, 0);
    TEST_ASSERT_EQ_I(e1.sum_hi, 0);
    free(b);
    PASS();
}

/* Four wide values at hash-scattered rows (not lane-periodic, so DELTA_BP
 * is wide), pinned by hand against ray_enc_choose: RAW 8192 B, FOR_BP
 * w=47 6024 B, DELTA_BP w=64 8328 B, RLE ~760 runs, DICT with a FOR_BP
 * w=2 codes child 8 + 8 + 256 = 272 B.  DICT wins. */
static const int64_t g_dict_vals[4] = { 11, 900000000001ll, 77, 123456789012345ll };
static int64_t dict_val(int i) { return g_dict_vals[(uint32_t)((uint32_t)i * 2654435761u) >> 30]; }

/* A DICT vector makes the writer emit one VALUE_DICT side entry. */
static test_result_t test_side_value_dict(void) {
    int64_t d[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d[i] = dict_val(i);
    ray_t* v = ray_vec_from_raw(RAY_I64, d, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/d", 2), RAY_OK);
    ray_release(v);
    size_t n; uint8_t* b = read_file(ENCFMT_TMP "/d", &n);
    ray_enc_desc_t d0; memcpy(&d0, b + 160, 48);
    TEST_ASSERT_EQ_I(d0.encoding, RAY_ENC_DICT);         /* before any offset is trusted */
    TEST_ASSERT_EQ_U(d0.payload_bytes, 272);
    ray_enc_hdr_t eh; memcpy(&eh, b + 32, 64);
    TEST_ASSERT_TRUE(eh.side_off != 0);
    TEST_ASSERT_EQ_U(eh.side_off % 32, 0);
    TEST_ASSERT_EQ_U(eh.side_off + eh.side_len, n);
    const uint8_t* s = b + eh.side_off;
    uint32_t ne; memcpy(&ne, s, 4);
    TEST_ASSERT_EQ_I(ne, 1);
    uint16_t kind; memcpy(&kind, s + 8, 2);
    TEST_ASSERT_EQ_I(kind, RAY_ENC_SIDE_VALUE_DICT);
    uint64_t eoff; memcpy(&eoff, s + 16, 8);
    TEST_ASSERT_EQ_U(eoff, 32);
    uint32_t dn; memcpy(&dn, s + eoff, 4);
    TEST_ASSERT_EQ_I(dn, 4);
    int64_t first; memcpy(&first, s + eoff + 8, 8);
    TEST_ASSERT_EQ_I(first, 11);                        /* ascending */
    free(b);
    PASS();
}

/* A trained dictionary no vector selects is not written: a lane-periodic
 * sequence of the same four values is DELTA_BP at width 0 (136 B). */
static test_result_t test_side_unused_dict_not_written(void) {
    int64_t d[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d[i] = g_dict_vals[(i * 7) % 4];
    ray_t* v = ray_vec_from_raw(RAY_I64, d, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/nd", 2), RAY_OK);
    ray_release(v);
    size_t n; uint8_t* b = read_file(ENCFMT_TMP "/nd", &n);
    ray_enc_desc_t d0; memcpy(&d0, b + 160, 48);
    TEST_ASSERT_EQ_I(d0.encoding, RAY_ENC_DELTA_BP);
    ray_enc_hdr_t eh; memcpy(&eh, b + 32, 64);
    TEST_ASSERT_EQ_U(eh.side_off, 0);
    TEST_ASSERT_EQ_U(eh.side_len, 0);
    free(b);
    PASS();
}

/* Spec "Materialization" step 3: the admission matrix, one predicate
 * shared by the writer, the load validator and the attach. */
static test_result_t test_admission_matrix(void) {
    static const int8_t num[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_F32, RAY_F64,
                                  RAY_DATE, RAY_TIME, RAY_TIMESTAMP };
    for (size_t i = 0; i < sizeof num; i++) {
        TEST_ASSERT_TRUE(ray_index_kind_admits(RAY_IDX_HASH, num[i]));
        TEST_ASSERT_TRUE(ray_index_kind_admits(RAY_IDX_SORT, num[i]));
        TEST_ASSERT_TRUE(ray_index_kind_admits(RAY_IDX_BLOOM, num[i]));
        TEST_ASSERT_TRUE(ray_index_kind_admits(RAY_IDX_PART, num[i]));
        TEST_ASSERT_TRUE(ray_index_kind_admits(RAY_IDX_ZONE, num[i]));
        TEST_ASSERT_TRUE(ray_index_kind_admits(RAY_IDX_CHUNK_ZONE, num[i]));
        TEST_ASSERT_TRUE(ray_index_kind_admits(RAY_IDX_NONE, num[i]));
        TEST_ASSERT_FALSE(ray_index_kind_admits(RAY_IDX_DICT, num[i]));
    }
    TEST_ASSERT_TRUE(ray_index_kind_admits(RAY_IDX_HASH, RAY_SYM));
    TEST_ASSERT_TRUE(ray_index_kind_admits(RAY_IDX_PART, RAY_SYM));
    TEST_ASSERT_FALSE(ray_index_kind_admits(RAY_IDX_SORT, RAY_SYM));
    TEST_ASSERT_FALSE(ray_index_kind_admits(RAY_IDX_BLOOM, RAY_SYM));
    TEST_ASSERT_FALSE(ray_index_kind_admits(RAY_IDX_ZONE, RAY_SYM));
    TEST_ASSERT_TRUE(ray_index_kind_admits(RAY_IDX_HASH, RAY_STR));
    TEST_ASSERT_TRUE(ray_index_kind_admits(RAY_IDX_DICT, RAY_STR));
    TEST_ASSERT_FALSE(ray_index_kind_admits(RAY_IDX_PART, RAY_STR));
    TEST_ASSERT_FALSE(ray_index_kind_admits(RAY_IDX_BLOOM, RAY_STR));
    TEST_ASSERT_FALSE(ray_index_kind_admits(RAY_IDX_NONE, RAY_GUID));
    TEST_ASSERT_FALSE(ray_index_kind_admits(RAY_IDX_HASH, RAY_GUID));
    PASS();
}

static test_result_t test_empty_column(void) {
    ray_t* v = ray_vec_new(RAY_I64, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/e", 2), RAY_OK);
    ray_release(v);
    size_t n; uint8_t* b = read_file(ENCFMT_TMP "/e", &n);
    TEST_ASSERT_EQ_U(n, 96);
    ray_enc_hdr_t eh; memcpy(&eh, b + 32, 64);
    TEST_ASSERT_EQ_U(eh.end_off, 96);
    free(b);
    PASS();
}

/* Compile with -DENC_GOLDEN_KEEP to regenerate the fixtures into a kept
 * directory; otherwise the emit test writes into the (removed) test dir. */
#ifdef ENC_GOLDEN_KEEP
#  define ENC_GOLDEN_OUT "/tmp/rayforce_enc_golden"
#else
#  define ENC_GOLDEN_OUT ENCFMT_TMP "/golden_emit"
#endif
#define ENC_GOLDEN_DIR "test/data/enc"

static uint64_t g_rng;
static uint64_t golden_rng(void) { uint64_t x = g_rng; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return g_rng = x; }

/* Golden case i: its vector, its name, the encoding forced for every vector
 * (-1 = selection) and the top-level encoding vector 0 must have, pinned by
 * hand against ray_enc_choose (sizes in the comments).  NULL past the last. */
static int g_kid[4];   /* pinned child-slot encodings of vector 0 (-1 = unpinned); see golden_kids */
static ray_t* golden_vec(int i, const char** name, int* force, int* want) {
    for (int j = 0; j < 4; j++) g_kid[j] = -1;
    g_rng = 0x9E3779B97F4A7C15ull + (uint64_t)i;
    ray_t* v = NULL;
    *force = -1;
    switch (i) {
    case 0: { *name = "i32_for"; *want = RAY_ENC_FOR_BP;            /* w=6: 776 B */
              int32_t d[1024]; for (int k = 0; k < 1024; k++) d[k] = 5000 + (k % 64); v = ray_vec_from_raw(RAY_I32, d, 1024); break; }
    case 1: { *name = "i64_const"; *want = RAY_ENC_CONST;            /* 8 B */
              v = ray_vec_new(RAY_I64, 2000); v->len = 2000; for (int k = 0; k < 2000; k++) ((int64_t*)ray_data(v))[k] = 42; break; }
    case 2: { *name = "i64_delta"; *want = RAY_ENC_DELTA_BP;         /* w=0: 136 B; DICT 1296 B */
              int64_t d[1024]; for (int k = 0; k < 1024; k++) d[k] = 1700000000000ll + 1000ll * k; v = ray_vec_from_raw(RAY_I64, d, 1024); break; }
    case 3: { g_kid[1] = RAY_ENC_RLE; g_kid[2] = RAY_ENC_RAW; g_kid[3] = RAY_ENC_RAW;
              *name = "i64_dict_codes_rle"; *want = RAY_ENC_DICT;    /* codes RLE: 104 B; top RLE 136 B */
              int64_t d[1024]; for (int k = 0; k < 1024; k++) d[k] = (int64_t)(k / 100) * 1000003ll; v = ray_vec_from_raw(RAY_I64, d, 1024); break; }
    case 4: { g_kid[1] = RAY_ENC_FOR_BP;
              *name = "i64_dict"; *want = RAY_ENC_DICT;              /* VALUE_DICT side entry; codes FOR w=2: 272 B */
              int64_t d[1024]; for (int k = 0; k < 1024; k++) d[k] = dict_val(k); v = ray_vec_from_raw(RAY_I64, d, 1024); break; }
    case 5: { *name = "i32_nulls"; *want = RAY_ENC_FOR_BP;           /* w=10: 1288 B; DICT 1296 B */
              int32_t d[1024]; for (int k = 0; k < 1024; k++) d[k] = (k % 10 == 0) ? NULL_I32 : k; v = ray_vec_from_raw(RAY_I32, d, 1024); v->attrs |= RAY_ATTR_HAS_NULLS; break; }
    case 6: { *name = "i16_all_null"; *want = RAY_ENC_CONST;
              int16_t d[3] = { NULL_I16, NULL_I16, NULL_I16 }; v = ray_vec_from_raw(RAY_I16, d, 3); v->attrs |= RAY_ATTR_HAS_NULLS; break; }
    case 7: { *name = "u8_raw"; *want = RAY_ENC_RAW;
              uint8_t d[1024]; for (int k = 0; k < 1024; k++) d[k] = (uint8_t)golden_rng(); v = ray_vec_from_raw(RAY_U8, d, 1024); break; }
    case 8: { *name = "bool_rle"; *want = RAY_ENC_RLE;               /* top-level RLE, RAW values and ends: 64 B */
              uint8_t d[1024]; for (int k = 0; k < 1024; k++) d[k] = (uint8_t)((k / 100) & 1); v = ray_vec_from_raw(RAY_BOOL, d, 1024); break; }
    case 9: { *name = "f64_trailer"; *want = RAY_ENC_RAW;
              double d[4]; uint64_t odd = 0x7FF8000000000001ull; d[0] = 2.5; d[1] = NULL_F64; memcpy(&d[2], &odd, 8); d[3] = -1.0; v = ray_vec_from_raw(RAY_F64, d, 4); v->attrs |= RAY_ATTR_HAS_NULLS; break; }
    case 10: { *name = "f64_const"; *want = RAY_ENC_CONST;
               double d[1024]; for (int k = 0; k < 1024; k++) d[k] = 1.5; v = ray_vec_from_raw(RAY_F64, d, 1024); break; }
    case 11: { *name = "f32_zeros"; *want = RAY_ENC_RAW;
               float d[8] = { -0.0f, 0.0f, 1.0f, -2.0f, 0.0f, -0.0f, 3.5f, 1.0f }; v = ray_vec_from_raw(RAY_F32, d, 8); break; }
    case 12: { *name = "guid_mixed"; *want = RAY_ENC_RAW;
               uint8_t g[3][16]; for (int k = 0; k < 3; k++) for (int j = 0; j < 16; j++) g[k][j] = (uint8_t)(k * 16 + j + 1);
               memset(g[1], 0, 16); v = ray_vec_from_raw(RAY_GUID, g, 3); break; }
    case 13: { *name = "str_mixed"; *want = RAY_ENC_RAW; v = ray_vec_new(RAY_STR, 4);
               v = ray_str_vec_append(v, "", 0); v = ray_str_vec_append(v, "short", 5);
               v = ray_str_vec_append(v, "a very long string 1", 20); v = ray_str_vec_append(v, "a very long string 1", 20); break; }
    case 14: { *name = "str_const"; *want = RAY_ENC_CONST; v = ray_vec_new(RAY_STR, 3);
               for (int k = 0; k < 3; k++) { v = ray_str_vec_append(v, "same long string here", 21); }
               break; }
    case 15: { *name = "i64_empty"; *want = -1; v = ray_vec_new(RAY_I64, 0); break; }
    case 16: { *name = "i32_two_chunks"; *want = RAY_ENC_FOR_BP;
               v = ray_vec_new(RAY_I32, 65537); v->len = 65537; for (int k = 0; k < 65537; k++) ((int32_t*)ray_data(v))[k] = k % 1000; break; }
    case 17: { *name = "str_inline"; *want = RAY_ENC_RAW; v = ray_vec_new(RAY_STR, 3);
               v = ray_str_vec_append(v, "ab", 2); v = ray_str_vec_append(v, "twelve bytes", 12); v = ray_str_vec_append(v, "x", 1); break; }
    case 18: { *name = "str_pooled"; *want = RAY_ENC_RAW; v = ray_vec_new(RAY_STR, 2);
               v = ray_str_vec_append(v, "thirteen byte", 13); v = ray_str_vec_append(v, "another pooled one", 18); break; }
    case 19: { *name = "str_repeated"; *want = RAY_ENC_RAW; v = ray_vec_new(RAY_STR, 4);
               v = ray_str_vec_append(v, "repeated long text", 18); v = ray_str_vec_append(v, "short", 5);
               v = ray_str_vec_append(v, "repeated long text", 18); v = ray_str_vec_append(v, "repeated long text", 18); break; }
    /* forced encodings (spec: every encoding also forced) */
    case 20: case 21: case 22: {
               int T = i == 20 ? 16 : i == 21 ? 32 : 64, L = 1024 / T;
               *name = i == 20 ? "i16_delta_wT" : i == 21 ? "i32_delta_wT" : "i64_delta_wT";
               *force = RAY_ENC_DELTA_BP; *want = RAY_ENC_DELTA_BP;   /* extremes by lane row: w = T */
               int8_t ty = T == 16 ? RAY_I16 : T == 32 ? RAY_I32 : RAY_I64;
               v = ray_vec_new(ty, 1024); v->len = 1024;
               for (int k = 0; k < 1024; k++) {
                   bool up = (k / L) & 1;
                   if (T == 16) ((int16_t*)ray_data(v))[k] = up ? 32767 : -32767;
                   else if (T == 32) ((int32_t*)ray_data(v))[k] = up ? 2147483647 : -2147483647;
                   else ((int64_t*)ray_data(v))[k] = up ? INT64_MAX : INT64_MIN + 1;
               }
               break; }
    case 23: { g_kid[1] = RAY_ENC_FOR_BP;
               *name = "i64_dict_const_child"; *force = RAY_ENC_DICT; *want = RAY_ENC_DICT;  /* codes FOR w=0 at depth 1 */
               v = ray_vec_new(RAY_I64, 1024); v->len = 1024; for (int k = 0; k < 1024; k++) ((int64_t*)ray_data(v))[k] = 900000000001ll; break; }
    case 24: { g_kid[1] = RAY_ENC_FOR_BP;
               *name = "i64_rle_values_for"; *force = RAY_ENC_RLE; *want = RAY_ENC_RLE;      /* values FOR w=4 */
               v = ray_vec_new(RAY_I64, 1024); v->len = 1024; for (int k = 0; k < 1024; k++) ((int64_t*)ray_data(v))[k] = 1000 + (k / 2) % 16; break; }
    case 25: { g_kid[2] = RAY_ENC_DELTA_BP;
               *name = "i64_rle_ends_delta"; *force = RAY_ENC_RLE; *want = RAY_ENC_RLE;      /* ends DELTA w=0 */
               v = ray_vec_new(RAY_I64, 1024); v->len = 1024; for (int k = 0; k < 1024; k++) ((int64_t*)ray_data(v))[k] = (int64_t)k * 3; break; }
    /* ends of ~790 irregular runs: RAW 1580 B, DELTA w=7 1416 B, FOR w=10 1288 B */
    case 26: { g_kid[2] = RAY_ENC_FOR_BP;
               *name = "i64_rle_ends_for"; *force = RAY_ENC_RLE; *want = RAY_ENC_RLE;
               v = ray_vec_new(RAY_I64, 1024); v->len = 1024;
               int64_t val = 0;
               for (int k = 0; k < 1024; ) {
                   int run = (golden_rng() % 64 == 0) ? 20 : 1;
                   for (int j = 0; j < run && k < 1024; j++) ((int64_t*)ray_data(v))[k++] = val;
                   val += 7;
               }
               break; }
    /* three rows, dictionary of three: codes RAW 16 B beats FOR (8 + 256 B) */
    case 27: { g_kid[1] = RAY_ENC_RAW;
               *name = "i64_dict_codes_raw"; *force = RAY_ENC_DICT; *want = RAY_ENC_DICT;
               int64_t d[3] = { 5, 900000000001ll, -77 }; v = ray_vec_from_raw(RAY_I64, d, 3); break; }
    default: return NULL;
    }
    return v;
}

/* SYM golden cases are written through the column writer directly so the
 * saved SYM count does not depend on the process symbol table. */
static ray_err_t golden_sym(int i, const char* dir, const char** name) {
    static uint32_t ids[1024];
    uint8_t attrs; uint32_t count;
    if (i == 0) { *name = "sym_w8_for"; attrs = RAY_SYM_W8; count = 256;            /* FOR w=4: 520 B */
                  for (int k = 0; k < 1024; k++) ids[k] = 100 + (uint32_t)(k % 16); }
    else if (i == 1) { *name = "sym_w32_rle"; attrs = RAY_SYM_W32; count = 0xFFFFFFFFu;  /* 512 runs: 3096 B < RAW 4096 */
                  g_rng = 77; for (int k = 0; k < 1024; k += 2) ids[k] = ids[k + 1] = 1 + (uint32_t)(golden_rng() % 0xFFFFFFF0u); }
    else return RAY_ERR_RANGE;
    uint8_t buf[4096];
    for (int k = 0; k < 1024; k++) ray_write_sym(buf, k, ids[k], RAY_SYM, attrs);
    char path[512]; snprintf(path, sizeof path, "%s/%s.bin", dir, *name);
    FILE* f = fopen(path, "wb+");
    if (!f) return RAY_ERR_IO;
    ray_encw_t w;
    ray_err_t e = ray_encw_begin(&w, f, RAY_SYM, attrs, 1024, count, 0, 0);
    if (e == RAY_OK) e = ray_encw_chunk(&w, buf, NULL, 1024);
    if (e == RAY_OK) e = ray_encw_finish(&w);
    ray_encw_free(&w);
    fclose(f);
    return e;
}

static const int g_sym_want[2] = { RAY_ENC_FOR_BP, RAY_ENC_RLE };

/* Encodings of vector 0 read from the file: [0] top level; RLE: [1] values,
 * [2] ends; DICT: [1] codes and, when the codes are RLE, [2] their values and
 * [3] their ends.  -1 where the slot does not exist or the file is short. */
static void golden_kids(const char* path, int kid[4]) {
    for (int j = 0; j < 4; j++) kid[j] = -1;
    size_t n; uint8_t* b = read_file(path, &n);
    if (!b) return;
    uint64_t off = 0;                                   /* chunk 0 follows the directory */
    if (n >= 96 + 64) memcpy(&off, b + 96, 8);
    if (off >= 160 && off + 48 <= n) {
        ray_enc_desc_t d; memcpy(&d, b + off, 48);
        kid[0] = d.encoding;
        const uint8_t* p = b + off + d.payload_off;
        const uint8_t* end = b + n;
        uint32_t bb;
        if (d.encoding == RAY_ENC_RLE && p + 24 <= end) {
            kid[1] = p[8];
            memcpy(&bb, p + 12, 4);
            if (p + 16 + bb < end) kid[2] = p[16 + bb];
        } else if (d.encoding == RAY_ENC_DICT && p + 8 <= end) {
            kid[1] = p[0];
            if (p[0] == RAY_ENC_RLE && p + 32 <= end) {
                kid[2] = p[16];
                memcpy(&bb, p + 20, 4);
                if (p + 24 + bb < end) kid[3] = p[24 + bb];
            }
        }
    }
    free(b);
}

static int file_desc0_encoding(const char* path) {
    int kid[4]; golden_kids(path, kid);
    return kid[0];
}

/* Emits every case and checks vector 0's encoding against its pin. */
static const char* golden_emit_all(const char* dir) {
    static char why[256];
    (void)ray_test_mkdir_p(dir);
    for (int i = 0;; i++) {
        const char* name; int force, want;
        ray_t* v = golden_vec(i, &name, &force, &want);
        if (!v) break;
        char path[512]; snprintf(path, sizeof path, "%s/%s.bin", dir, name);
        ray_encw_test_force(force);
        ray_err_t e = ray_col_save_gen(v, path, 2);
        ray_encw_test_force(-1);
        ray_release(v);
        if (e != RAY_OK) { snprintf(why, sizeof why, "%s: save %d", name, (int)e); return why; }
        if (want >= 0 && file_desc0_encoding(path) != want) {
            snprintf(why, sizeof why, "%s: encoding %d, pinned %d", name, file_desc0_encoding(path), want);
            return why;
        }
        int kid[4]; golden_kids(path, kid);
        for (int j = 1; j < 4; j++)
            if (g_kid[j] >= 0 && kid[j] != g_kid[j]) {
                snprintf(why, sizeof why, "%s: child slot %d encoding %d, pinned %d", name, j, kid[j], g_kid[j]);
                return why;
            }
    }
    for (int i = 0; i < 2; i++) {
        const char* name; ray_err_t e = golden_sym(i, dir, &name);
        char path[512]; snprintf(path, sizeof path, "%s/%s.bin", dir, name);
        if (e != RAY_OK || file_desc0_encoding(path) != g_sym_want[i]) {
            snprintf(why, sizeof why, "%s: save %d / encoding %d", name, (int)e, file_desc0_encoding(path));
            return why;
        }
    }
    return NULL;
}

static const char* const g_golden_names[] = {
    "i32_for", "i64_const", "i64_delta", "i64_dict_codes_rle", "i64_dict", "i32_nulls", "i16_all_null",
    "u8_raw", "bool_rle", "f64_trailer", "f64_const", "f32_zeros", "guid_mixed", "str_mixed",
    "str_const", "i64_empty", "i32_two_chunks", "str_inline", "str_pooled", "str_repeated",
    "i16_delta_wT", "i32_delta_wT", "i64_delta_wT", "i64_dict_const_child", "i64_rle_values_for",
    "i64_rle_ends_delta", "i64_rle_ends_for", "i64_dict_codes_raw", "sym_w8_for", "sym_w32_rle", NULL,
};

static test_result_t test_golden_emit(void) {
    const char* why = golden_emit_all(ENC_GOLDEN_OUT);
    TEST_ASSERT_FMT(why == NULL, "%s", why);
    PASS();
}

static test_result_t test_golden_match(void) {
    const char* why = golden_emit_all(ENCFMT_TMP "/golden");
    TEST_ASSERT_FMT(why == NULL, "%s", why);
    const char* const* names = g_golden_names;
    for (size_t i = 0; names[i]; i++) {
        char a[512], b[512];
        snprintf(a, sizeof a, "%s/%s.bin", ENC_GOLDEN_DIR, names[i]);
        snprintf(b, sizeof b, "%s/golden/%s.bin", ENCFMT_TMP, names[i]);
        size_t na = 0, nb = 0; uint8_t* pa = read_file(a, &na); uint8_t* pb = read_file(b, &nb);
        bool ok = pa && pb && na == nb && memcmp(pa, pb, na) == 0;
        bool have_fixture = pa != NULL;
        free(pa); free(pb);
        TEST_ASSERT_FMT(have_fixture, "missing fixture %s", a);
        TEST_ASSERT_FMT(ok, "%s differs from its fixture", names[i]);
    }
    PASS();
}

static test_result_t test_golden_stream(void) {
    (void)ray_test_mkdir_p(ENCFMT_TMP "/gs");
    for (int i = 0;; i++) {
        const char* name; int force, want;
        ray_t* v = golden_vec(i, &name, &force, &want);
        if (!v) break;
        ray_col_stream_t w;
        TEST_ASSERT_EQ_I(ray_col_stream_open(&w, ENCFMT_TMP "/gs", ray_sym_intern(name, strlen(name)), v->type, NULL, 2), RAY_OK);
        ray_encw_test_force(force);
        ray_err_t e = v->len ? ray_col_stream_append(&w, v) : RAY_OK;
        if (e == RAY_OK) e = ray_col_stream_close(&w, false);
        ray_encw_test_force(-1);
        ray_col_stream_abort(&w);
        ray_release(v);
        TEST_ASSERT_FMT(e == RAY_OK, "%s: stream %d", name, (int)e);
        char a[512], b[512];
        snprintf(a, sizeof a, "%s/%s.bin", ENC_GOLDEN_DIR, name);
        snprintf(b, sizeof b, "%s/gs/%s", ENCFMT_TMP, name);
        size_t na, nb; uint8_t* pa = read_file(a, &na); uint8_t* pb = read_file(b, &nb);
        TEST_ASSERT_FMT(pa && pb && na == nb && memcmp(pa, pb, na) == 0, "%s: stream differs from the fixture", name);
        free(pa); free(pb);
    }
    PASS();
}

/* Every checked-in fixture decodes, through the generation-2 reader, to the
 * vector its generator regenerates.  sym_w32_rle is excluded: its saved
 * count (0xFFFFFFFF) lies above any symbol domain a test can build. */
static test_result_t test_golden_decode_matches(void) {
    for (int i = 0; i < 256; i++) { char b[8]; int n = snprintf(b, sizeof b, "s%d", i); (void)ray_sym_intern(b, (size_t)n); }
    const char* why = golden_emit_all(ENCFMT_TMP "/gdm");        /* also regenerates the SYM id table */
    TEST_ASSERT_FMT(why == NULL, "%s", why);
    for (int i = 0;; i++) {
        const char* name; int force, want;
        ray_t* v = golden_vec(i, &name, &force, &want);
        if (!v) break;
        char path[512]; snprintf(path, sizeof path, "%s/%s.bin", ENC_GOLDEN_DIR, name);
        ray_t* c = ray_col_mmap(path);
        TEST_ASSERT_FMT(c && !RAY_IS_ERR(c), "fixture %s does not load", name);
        ray_enccol_t* e = ray_enccol_of(c);
        TEST_ASSERT_FMT(e->len == v->len && e->type == v->type, "%s: shape", name);
        uint8_t esz = ray_enccol_esz(e);
        uint8_t out[RAY_ENC_VEC * 16]; char pool[RAY_ENC_VEC * 64];
        const char* spool = (v->type == RAY_STR && v->str_pool) ? (const char*)ray_data(v->str_pool) : NULL;
        for (int64_t vec = 0; vec < ray_enc_nvecs(v->len); vec++) {
            ray_t* err = ray_enccol_decode_vec(e, vec, out, pool, 0, sizeof pool);
            TEST_ASSERT_FMT(err == NULL, "%s vec %lld: %s", name, (long long)vec, err ? ray_err_code(err) : "");
            int vn = ray_enc_vec_len(v->len, vec);
            if (v->type == RAY_STR) {
                const ray_str_t* a = (const ray_str_t*)ray_data(v) + vec * RAY_ENC_VEC;
                const ray_str_t* b = (const ray_str_t*)out;
                for (int r = 0; r < vn; r++) {
                    TEST_ASSERT_FMT(a[r].len == b[r].len, "%s str len row %d", name, r);
                    TEST_ASSERT_FMT(memcmp(ray_str_t_ptr(&a[r], spool), ray_str_t_ptr(&b[r], pool), a[r].len) == 0, "%s str row %d", name, r);
                }
            } else {
                const uint8_t* a = (const uint8_t*)ray_data(v) + (size_t)vec * RAY_ENC_VEC * esz;
                TEST_ASSERT_FMT(memcmp(a, out, (size_t)vn * esz) == 0, "%s vec %lld differs from its input", name, (long long)vec);
            }
        }
        ray_release(c);
        ray_release(v);
    }
    /* SYM fixture: ids from the generator's table. */
    const char* sname; (void)golden_sym(0, ENCFMT_TMP "/gdm", &sname);
    char path[512]; snprintf(path, sizeof path, "%s/%s.bin", ENC_GOLDEN_DIR, sname);
    ray_t* c = ray_col_mmap(path);
    TEST_ASSERT_FMT(c && !RAY_IS_ERR(c), "fixture %s does not load", sname);
    uint8_t out[RAY_ENC_VEC * 16];
    ray_t* err = ray_enccol_decode_vec(ray_enccol_of(c), 0, out, NULL, 0, 0);
    TEST_ASSERT_NULL(err);
    for (int k = 0; k < 1024; k++) TEST_ASSERT_EQ_I(out[k], 100 + (k % 16));
    ray_release(c);
    PASS();
}

/* Pooled strings (longer than 12 bytes) reach the encoder through the
 * column's real pool.  Row i has 1 + i % 29 bytes, so inline (<= 12) and
 * pooled strings alternate; pooled rows are unique (a decimal row tag).
 * Chunk 1 is read back cell by cell: a wrong per-chunk stride or a wrong
 * pool would change its strings or offsets. */
static int str_row(int64_t i, char* s) {
    int k = 1 + (int)(i % 29);
    if (k > 12) { char tag[16]; snprintf(tag, sizeof tag, "%07d", (int)i); memset(s, 'a' + (int)(i % 26), (size_t)k); memcpy(s, tag, 7); }
    else memset(s, 'a' + (int)(i % 26), (size_t)k);
    return k;
}

static test_result_t test_str_pooled_column(void) {
    int64_t len = 65536 + 40;
    ray_t* v = ray_vec_new(RAY_STR, len);
    char s[40];
    uint64_t pooled = 0, chunk1_pooled = 0;
    for (int64_t i = 0; i < len; i++) {
        int k = str_row(i, s);
        v = ray_str_vec_append(v, s, (size_t)k);
        if (k > 12) { pooled += (uint64_t)k; if (i >= 65536) chunk1_pooled += (uint64_t)k; }
    }
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/sp", 2), RAY_OK);
    ray_release(v);
    size_t n; uint8_t* b = read_file(ENCFMT_TMP "/sp", &n);
    TEST_ASSERT_NOT_NULL(b);
    ray_enc_hdr_t eh; memcpy(&eh, b + 32, 64);
    TEST_ASSERT_EQ_U(eh.str_pool_bytes, pooled);
    TEST_ASSERT_EQ_U(eh.end_off, n);
    ray_enc_dirent_t e1; memcpy(&e1, b + 96 + 64, 64);
    ray_enc_desc_t d0; memcpy(&d0, b + e1.offset, 48);
    TEST_ASSERT_EQ_I(d0.encoding, RAY_ENC_RAW);
    const uint8_t* cells = b + e1.offset + d0.payload_off;
    uint64_t pb; memcpy(&pb, cells + 40 * 16, 8);
    TEST_ASSERT_EQ_U(pb, chunk1_pooled);
    const uint8_t* pool = cells + 40 * 16 + 8;
    uint32_t want_off = 0;
    for (int j = 0; j < 40; j++) {
        int k = str_row(65536 + j, s);
        uint32_t len_c; memcpy(&len_c, cells + j * 16, 4);
        TEST_ASSERT_EQ_I(len_c, k);
        if (k <= 12) {
            TEST_ASSERT_TRUE(memcmp(cells + j * 16 + 4, s, (size_t)k) == 0);
        } else {
            uint32_t off; memcpy(&off, cells + j * 16 + 8, 4);
            TEST_ASSERT_EQ_U(off, want_off);
            TEST_ASSERT_TRUE(memcmp(pool + off, s, (size_t)k) == 0);
            want_off += (uint32_t)k;
        }
    }
    free(b);
    PASS();
}

static int file_gen(const char* path) {
    size_t n; uint8_t* b = read_file(path, &n);
    int g = (b && n >= 32) ? ray_col_gen_of(b) : -99;
    free(b);
    return g;
}

static bool files_equal(const char* a, const char* b) {
    size_t na, nb; uint8_t* pa = read_file(a, &na); uint8_t* pb = read_file(b, &nb);
    bool eq = pa && pb && na == nb && memcmp(pa, pb, na) == 0;
    free(pa); free(pb);
    return eq;
}

static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static ray_t* i64_ramp(int64_t len) {
    ray_t* v = ray_vec_new(RAY_I64, len);
    v->len = len;
    for (int64_t i = 0; i < len; i++) ((int64_t*)ray_data(v))[i] = i * 3;
    return v;
}

/* Columns generation 2 cannot hold are written as generation 0. */
static test_result_t test_gen0_fallback(void) {
    ray_t* v = i64_ramp(2000);
    ray_t* r = ray_index_attach_hash(&v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/fh", 2), RAY_OK);
    TEST_ASSERT_EQ_I(file_gen(ENCFMT_TMP "/fh"), 0);
    ray_release(v);

    ray_t* l = ray_list_new(2);
    l = ray_list_append(l, ray_i64(5));
    ray_err_t e = ray_col_save_gen(l, ENCFMT_TMP "/fl", 2);
    TEST_ASSERT_EQ_I(e, ray_col_save(l, ENCFMT_TMP "/fl0"));
    TEST_ASSERT_TRUE(files_equal(ENCFMT_TMP "/fl", ENCFMT_TMP "/fl0"));   /* the gen-0 bytes */
    ray_release(l);

    v = i64_ramp(100);
    v->attrs |= RAY_ATTR_HAS_LINK;                       /* link bit only: ineligible */
    e = ray_col_save_gen(v, ENCFMT_TMP "/fk", 2);
    TEST_ASSERT_EQ_I(e, ray_col_save(v, ENCFMT_TMP "/fk0"));
    TEST_ASSERT_TRUE(files_equal(ENCFMT_TMP "/fk", ENCFMT_TMP "/fk0"));   /* the gen-0 bytes */
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_LINK;
    ray_release(v);

    /* a plain column of the same shape is generation 2 */
    v = i64_ramp(2000);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/fp", 2), RAY_OK);
    TEST_ASSERT_EQ_I(file_gen(ENCFMT_TMP "/fp"), 2);
    /* generation 0 is explicit */
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/f0", 0), RAY_OK);
    TEST_ASSERT_EQ_I(file_gen(ENCFMT_TMP "/f0"), 0);
    /* generation 1 does not exist */
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/f1", 1), RAY_ERR_VERSION);
    TEST_ASSERT_FALSE(file_exists(ENCFMT_TMP "/f1"));
    ray_release(v);
    PASS();
}

static test_result_t test_bulk_gen(void) {
    ray_t* v = i64_ramp(70000);
    TEST_ASSERT_EQ_I(ray_col_save_bulk_gen(v, ENCFMT_TMP "/b2", 2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/d2", 2), RAY_OK);
    size_t na, nb; uint8_t* a = read_file(ENCFMT_TMP "/b2", &na); uint8_t* b = read_file(ENCFMT_TMP "/d2", &nb);
    TEST_ASSERT_TRUE(a && b && na == nb && memcmp(a, b, na) == 0);   /* durability changes no bytes */
    free(a); free(b);
    TEST_ASSERT_EQ_I(file_gen(ENCFMT_TMP "/b2"), 2);
    TEST_ASSERT_FALSE(file_exists(ENCFMT_TMP "/b2.tmp"));
    TEST_ASSERT_EQ_I(ray_col_save_bulk_gen(v, ENCFMT_TMP "/b0", 0), RAY_OK);
    TEST_ASSERT_EQ_I(file_gen(ENCFMT_TMP "/b0"), 0);
    TEST_ASSERT_EQ_I(ray_col_save_bulk_gen(v, ENCFMT_TMP "/b1", 1), RAY_ERR_VERSION);
    ray_release(v);
    PASS();
}

/* The enc header records ZONE / CHUNK_ZONE and the UNIQUE flag. */
static test_result_t test_header_index_meta(void) {
    ray_t* v = i64_ramp(70000);
    ray_t* r = ray_index_attach_zone(&v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_payload(v->index)->markers |= RAY_MARK_UNIQUE;
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/iz", 2), RAY_OK);
    ray_release(v);
    size_t n; uint8_t* b = read_file(ENCFMT_TMP "/iz", &n);
    TEST_ASSERT_EQ_I(file_gen(ENCFMT_TMP "/iz"), 2);
    ray_enc_hdr_t eh; memcpy(&eh, b + 32, 64);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_ZONE);
    TEST_ASSERT_EQ_I(eh.col_flags, RAY_ENC_FLAG_UNIQUE);
    TEST_ASSERT_EQ_U(eh.index_off, 0);
    free(b);

    v = i64_ramp(70000);
    r = ray_index_attach_chunk_zone(&v, 16);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCFMT_TMP "/ic", 2), RAY_OK);
    ray_release(v);
    b = read_file(ENCFMT_TMP "/ic", &n);
    memcpy(&eh, b + 32, 64);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_CHUNK_ZONE);
    TEST_ASSERT_EQ_I(eh.col_flags, 0);
    free(b);
    PASS();
}

/* A ZONE root on a type the matrix does not admit is refused, nothing left behind. */
static test_result_t test_admission_refusal(void) {
    ray_t* v = i64_ramp(100);
    ray_t* r = ray_index_attach_zone(&v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    v->type = RAY_SYM;                                   /* matrix: SYM carries no ZONE */
    ray_err_t e = ray_col_save_gen(v, ENCFMT_TMP "/nyi", 2);
    v->type = RAY_I64;
    ray_release(v);
    TEST_ASSERT_EQ_I(e, RAY_ERR_NYI);
    TEST_ASSERT_FALSE(file_exists(ENCFMT_TMP "/nyi"));
    TEST_ASSERT_FALSE(file_exists(ENCFMT_TMP "/nyi.tmp"));
    PASS();
}

/* An I/O failure returns an error and leaves neither a target nor a .tmp. */
static test_result_t test_save_io_failure(void) {
    ray_t* v = i64_ramp(100);
    TEST_ASSERT_TRUE(ray_col_save_gen(v, ENCFMT_TMP "/nodir/x", 2) != RAY_OK);
    TEST_ASSERT_FALSE(file_exists(ENCFMT_TMP "/nodir/x.tmp"));
    /* the rename fails: the target is an existing directory */
    (void)ray_test_mkdir_p(ENCFMT_TMP "/isdir");
    TEST_ASSERT_TRUE(ray_col_save_gen(v, ENCFMT_TMP "/isdir", 2) != RAY_OK);
    TEST_ASSERT_FALSE(file_exists(ENCFMT_TMP "/isdir.tmp"));
    TEST_ASSERT_TRUE(ray_col_save_bulk_gen(v, ENCFMT_TMP "/isdir", 2) != RAY_OK);
    TEST_ASSERT_FALSE(file_exists(ENCFMT_TMP "/isdir.tmp"));
    ray_release(v);
    PASS();
}

const test_entry_t encfmt_entries[] = {
    { "encfmt/discriminator_pairs",       test_discriminator_pairs,       encfmt_setup, encfmt_teardown },
    { "encfmt/discriminator_cross_pairs", test_discriminator_cross_pairs, encfmt_setup, encfmt_teardown },
    { "encfmt/gen0_writer_zeroes_aux",    test_gen0_writer_zeroes_aux,    encfmt_setup, encfmt_teardown },
    { "encfmt/length_helpers",            test_length_helpers,            encfmt_setup, encfmt_teardown },
    { "encfmt/vec_i32_for",                 test_vec_i32_for,                 encfmt_setup, encfmt_teardown },
    { "encfmt/vec_i64_nulls_bitmap_and_sum",test_vec_i64_nulls_bitmap_and_sum,encfmt_setup, encfmt_teardown },
    { "encfmt/vec_all_null_is_const",       test_vec_all_null_is_const,       encfmt_setup, encfmt_teardown },
    { "encfmt/vec_f64_trailer",             test_vec_f64_trailer,             encfmt_setup, encfmt_teardown },
    { "encfmt/vec_f64_const_and_zeros",     test_vec_f64_const_and_zeros,     encfmt_setup, encfmt_teardown },
    { "encfmt/vec_f64_const_trailer",       test_vec_f64_const_with_trailer,  encfmt_setup, encfmt_teardown },
    { "encfmt/vec_forced",                  test_vec_forced,                  encfmt_setup, encfmt_teardown },
    { "encfmt/vec_guid",                    test_vec_guid,                    encfmt_setup, encfmt_teardown },
    { "encfmt/vec_str_raw_and_const",       test_vec_str_raw_and_const,       encfmt_setup, encfmt_teardown },
    { "encfmt/vec_sym_w8_inband",           test_vec_sym_w8_inband,           encfmt_setup, encfmt_teardown },
    { "encfmt/vec_bool_rle",                test_vec_bool_rle,                encfmt_setup, encfmt_teardown },
    { "encfmt/vec_roundtrip_codecs",        test_vec_roundtrip_codecs,        encfmt_setup, encfmt_teardown },
    { "encfmt/vec_roundtrip_dict",          test_vec_roundtrip_dict,          encfmt_setup, encfmt_teardown },
    { "encfmt/vec_sums_literal",            test_vec_sums_literal,            encfmt_setup, encfmt_teardown },
    { "encfmt/vec_f32_trailer",             test_vec_f32_trailer,             encfmt_setup, encfmt_teardown },
    { "encfmt/vec_all_null_kinds",          test_vec_all_null_kinds,          encfmt_setup, encfmt_teardown },
    { "encfmt/vec_sym_w16_w32",             test_vec_sym_w16_w32,             encfmt_setup, encfmt_teardown },
    { "encfmt/golden_decode_matches",       test_golden_decode_matches,       encfmt_setup, encfmt_teardown },
    { "encfmt/file_headers",              test_file_headers,              encfmt_setup, encfmt_teardown },
    { "encfmt/trailing_descriptors_zero", test_trailing_descriptors_zero, encfmt_setup, encfmt_teardown },
    { "encfmt/dir_sums_extremes",         test_dir_sums_extremes,         encfmt_setup, encfmt_teardown },
    { "encfmt/side_value_dict",           test_side_value_dict,           encfmt_setup, encfmt_teardown },
    { "encfmt/side_unused_dict",          test_side_unused_dict_not_written, encfmt_setup, encfmt_teardown },
    { "encfmt/admission_matrix",          test_admission_matrix,          encfmt_setup, encfmt_teardown },
    { "encfmt/empty_column",              test_empty_column,              encfmt_setup, encfmt_teardown },
    { "encfmt/str_pooled_column",         test_str_pooled_column,         encfmt_setup, encfmt_teardown },
    { "encfmt/gen0_fallback",             test_gen0_fallback,             encfmt_setup, encfmt_teardown },
    { "encfmt/bulk_gen",                  test_bulk_gen,                  encfmt_setup, encfmt_teardown },
    { "encfmt/header_index_meta",         test_header_index_meta,         encfmt_setup, encfmt_teardown },
    { "encfmt/admission_refusal",         test_admission_refusal,         encfmt_setup, encfmt_teardown },
    { "encfmt/save_io_failure",           test_save_io_failure,           encfmt_setup, encfmt_teardown },
    { "encfmt/golden_emit",               test_golden_emit,               encfmt_setup, encfmt_teardown },
    { "encfmt/golden_match",              test_golden_match,              encfmt_setup, encfmt_teardown },
    { "encfmt/golden_stream", test_golden_stream, encfmt_setup, encfmt_teardown },
    { NULL, NULL, NULL, NULL },
};
