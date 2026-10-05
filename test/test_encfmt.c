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
    { NULL, NULL, NULL, NULL },
};
