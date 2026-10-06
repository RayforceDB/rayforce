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
#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "core/runtime.h"
#include "store/col.h"
#include "store/colrange.h"
#include "store/enc.h"
#include "store/encfmt.h"
#include "store/encw.h"
#include "store/enccol.h"
#include "vec/str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EC_TMP "/tmp/rayforce_enccorrupt"
static void ec_setup(void) {
    ray_heap_init(); (void)ray_sym_init(); (void)ray_test_mkdir_p(EC_TMP);
    for (int i = 0; i < 256; i++) { char b[8]; int n = snprintf(b, sizeof b, "s%d", i); (void)ray_sym_intern(b, (size_t)n); }
}
static uint8_t* g_img; static size_t g_len;
static void ec_teardown(void) { free(g_img); g_img = NULL; ray_sym_destroy(); ray_heap_destroy(); (void)ray_test_rm_rf(EC_TMP); }

static void img_load(const char* p) {
    free(g_img); FILE* f = fopen(p, "rb"); fseek(f, 0, SEEK_END); g_len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET); g_img = malloc(g_len); fread(g_img, 1, g_len, f); fclose(f);
}
static void img_store(const char* p, size_t n) { FILE* f = fopen(p, "wb"); fwrite(g_img, 1, n, f); fclose(f); }
static uint64_t rd64(size_t off) { uint64_t x; memcpy(&x, g_img + off, 8); return x; }
static void wr(size_t off, const void* b, size_t n) { memcpy(g_img + off, b, n); }
/* file offset of chunk c's descriptor j, and of its payload */
static size_t desc_at(int c, int j) { return (size_t)rd64(96 + 64 * (size_t)c) + 48 * (size_t)j; }
static size_t payload_at(int c, int j) { return (size_t)rd64(96 + 64 * (size_t)c) + (size_t)rd64(desc_at(c, j) + 8); }

/* The error message names the column file. */
static bool ec_names_path(ray_t* err, const char* path) {
    (void)err;
    const char* m = ray_error_msg();
    return m && strstr(m, path) != NULL;
}

/* Expect `code` at load (stage 0) or at materialization (stage 1). */
static const char* expect(const char* path, int stage, const char* code) {
    static char why[256];
    ray_error_clear();
    ray_t* c = ray_col_mmap(path);
    if (stage == 0) {
        if (!c || !RAY_IS_ERR(c)) { if (c) ray_release(c); snprintf(why, sizeof why, "loaded, want %s", code); return why; }
        bool ok = strcmp(ray_err_code(c), code) == 0 && ec_names_path(c, path);
        snprintf(why, sizeof why, "load gave %s want %s (path named: %d)", ray_err_code(c), code, ec_names_path(c, path));
        ray_error_free(c);
        return ok ? NULL : why;
    }
    if (!c || RAY_IS_ERR(c)) { snprintf(why, sizeof why, "load failed (%s)", c ? ray_err_code(c) : "null"); ray_error_free(c); return why; }
    ray_t* m = ray_enccol_materialize(c);
    bool ok = m && RAY_IS_ERR(m) && strcmp(ray_err_code(m), code) == 0 && ec_names_path(m, path);
    snprintf(why, sizeof why, "materialize gave %s want %s (path named: %d)", (m && RAY_IS_ERR(m)) ? ray_err_code(m) : "a column", code,
             (m && RAY_IS_ERR(m)) ? ec_names_path(m, path) : 0);
    if (m && RAY_IS_ERR(m)) ray_error_free(m);
    ray_release(c);
    return ok ? NULL : why;
}

#define EXPECT(path, stage, code) do { const char* w_ = expect(path, stage, code); \
    TEST_ASSERT_FMT(w_ == NULL, "%s:%d %s", __FILE__, __LINE__, w_); } while (0)

static void save_vec(ray_t* v, const char* p) { (void)ray_col_save_gen(v, p, 2); ray_release(v); img_load(p); }

/* Pinned by hand against ray_enc_choose (and asserted before any patch):
 * BOOL runs of 100 -> top-level RLE, 64 B: [u32 11][u32 0]
 * [values hdr: RAW, body 16 @+8][values @+16][ends hdr: RAW, body 24 @+32]
 * [ends U16 @+40]; BOOL has no DICT, so no dictionary competes. */
static ray_t* bool_runs(void) {
    ray_t* v = ray_vec_new(RAY_BOOL, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((uint8_t*)ray_data(v))[i] = (uint8_t)((i / 100) & 1);
    return v;
}

/* Four wide values at hash-scattered rows -> DICT with a FOR_BP w=2 codes
 * child (272 B) and a VALUE_DICT side entry. */
static ray_t* i64_dict(void) {
    static const int64_t vals[4] = { 11, 900000000001ll, 77, 123456789012345ll };
    ray_t* v = ray_vec_new(RAY_I64, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((int64_t*)ray_data(v))[i] = vals[(uint32_t)((uint32_t)i * 2654435761u) >> 30];
    return v;
}

#define NEED_ENC(c, j, enc) TEST_ASSERT_FMT(g_img[desc_at(c, j)] == (enc), \
    "descriptor %d.%d is encoding %d, the test pins %d", c, j, g_img[desc_at(c, j)], enc)

/* A SYM vector of `n` ids written with saved count 200. */
static void save_sym200(const uint8_t* ids, int n, const char* p) {
    FILE* f = fopen(p, "wb+");
    ray_encw_t w;
    (void)ray_encw_begin(&w, f, RAY_SYM, RAY_SYM_W8, n, 200, 0, 0);
    (void)ray_encw_chunk(&w, ids, NULL, n);
    (void)ray_encw_finish(&w);
    ray_encw_free(&w); fclose(f);
    img_load(p);
}

static test_result_t test_reject_headers(void) {
    const char* p = EC_TMP "/h";
    uint8_t b; uint16_t v16; uint32_t v32; uint64_t v64;
    save_vec(bool_runs(), p);
    b = 0; wr(17, &b, 1); img_store(p, g_len); EXPECT(p, 0, "corrupt");             /* (order 0, "XENC") */
    save_vec(bool_runs(), p);
    v32 = RAY_ENC_XIDX; wr(0, &v32, 4); img_store(p, g_len); EXPECT(p, 0, "corrupt"); /* (order 2, "XIDX") */
    save_vec(bool_runs(), p);
    v32 = 0; wr(0, &v32, 4); img_store(p, g_len); EXPECT(p, 0, "corrupt");          /* (order 2, zero aux) */
    save_vec(bool_runs(), p);
    v16 = 2; wr(36, &v16, 2); img_store(p, g_len); EXPECT(p, 0, "version");
    save_vec(bool_runs(), p);
    b = 'X'; wr(32, &b, 1); img_store(p, g_len); EXPECT(p, 0, "corrupt");           /* magic */
    save_vec(bool_runs(), p);
    b = 7; wr(38, &b, 1); img_store(p, g_len); EXPECT(p, 0, "corrupt");             /* index kind 7 */
    save_vec(bool_runs(), p);
    v64 = 1; wr(32 + 48, &v64, 8); img_store(p, g_len); EXPECT(p, 0, "corrupt");    /* str_pool_bytes on BOOL */
    /* STR: str_pool_bytes above UINT32_MAX (load) and one byte off the built pool (materialization) */
    for (int k = 0; k < 2; k++) {
        ray_t* st = ray_vec_new(RAY_STR, 2);
        st = ray_str_vec_append(st, "a pooled string here", 20); st = ray_str_vec_append(st, "x", 1);
        save_vec(st, p);
        v64 = k == 0 ? 0x100000000ull : rd64(32 + 48) + 1;
        wr(32 + 48, &v64, 8); img_store(p, g_len);
        if (k == 0) EXPECT(p, 0, "corrupt"); else EXPECT(p, 1, "corrupt");
    }
    save_vec(bool_runs(), p);
    img_store(p, g_len - 8); EXPECT(p, 0, "corrupt");                                /* truncated */
    save_vec(bool_runs(), p);
    b = 1; wr(4, &b, 1); img_store(p, g_len); EXPECT(p, 0, "corrupt");              /* aux reserved byte */
    PASS();
}

static test_result_t test_reject_directory(void) {
    const char* p = EC_TMP "/d";
    uint32_t v32; uint64_t v64; int64_t i64;
    save_vec(bool_runs(), p);
    v64 = rd64(104) + 8; wr(104, &v64, 8); img_store(p, g_len); EXPECT(p, 0, "corrupt");   /* chunk bytes overrun */
    save_vec(bool_runs(), p);
    v32 = 2; wr(96 + 56, &v32, 4); img_store(p, g_len); EXPECT(p, 0, "corrupt");           /* flags bit 1 */
    save_vec(bool_runs(), p);
    i64 = -1; wr(96 + 24, &i64, 8); img_store(p, g_len); EXPECT(p, 0, "corrupt");          /* max below min */
    ray_t* f = ray_vec_new(RAY_F64, 100); f->len = 100;
    for (int i = 0; i < 100; i++) ((double*)ray_data(f))[i] = i;
    save_vec(f, p);
    v64 = 5; wr(96 + 32, &v64, 8); img_store(p, g_len); EXPECT(p, 0, "corrupt");           /* sum on F64 */
    PASS();
}

static test_result_t test_reject_descriptors(void) {
    const char* p = EC_TMP "/x";
    uint8_t b; uint16_t v16;
    save_vec(bool_runs(), p);
    b = 200; wr(desc_at(0, 0), &b, 1); img_store(p, g_len); EXPECT(p, 1, "corrupt");       /* unknown encoding */
    save_vec(bool_runs(), p);
    NEED_ENC(0, 0, RAY_ENC_RLE);
    b = RAY_ENC_ALP; wr(desc_at(0, 0), &b, 1); img_store(p, g_len); EXPECT(p, 1, "corrupt"); /* ALP on BOOL */
    save_vec(bool_runs(), p);
    b = 65; wr(desc_at(0, 0) + 1, &b, 1); img_store(p, g_len); EXPECT(p, 1, "corrupt");    /* width on RLE */
    save_vec(bool_runs(), p);
    v16 = 1025; wr(desc_at(0, 0) + 2, &v16, 2); img_store(p, g_len); EXPECT(p, 1, "corrupt"); /* n_nulls > vlen */
    save_vec(bool_runs(), p);
    b = 1; wr(desc_at(0, 3) + 20, &b, 1); img_store(p, g_len); EXPECT(p, 1, "corrupt");    /* trailing descriptor */
    ray_t* f = ray_vec_new(RAY_F64, 100); f->len = 100;
    for (int i = 0; i < 100; i++) ((double*)ray_data(f))[i] = i;
    save_vec(f, p);
    b = RAY_ENC_ALP; wr(desc_at(0, 0), &b, 1); img_store(p, g_len); EXPECT(p, 1, "nyi");   /* legal, not in P1 */
    /* an all-null vector that is not CONST */
    ray_t* n = ray_vec_new(RAY_I32, 10); n->len = 10;
    for (int i = 0; i < 10; i++) ((int32_t*)ray_data(n))[i] = NULL_I32;
    save_vec(n, p);
    b = RAY_ENC_RAW; wr(desc_at(0, 0), &b, 1); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    /* an all-null vector with statistics present (min = max = 0, flag cleared) */
    n = ray_vec_new(RAY_I32, 10); n->len = 10;
    for (int i = 0; i < 10; i++) ((int32_t*)ray_data(n))[i] = NULL_I32;
    save_vec(n, p);
    NEED_ENC(0, 0, RAY_ENC_CONST);
    v16 = 0; wr(desc_at(0, 0) + 4, &v16, 2); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    PASS();
}

static test_result_t test_reject_payloads(void) {
    const char* p = EC_TMP "/q";
    uint8_t b; uint16_t v16; uint32_t v32;
    /* RLE run ends not ascending: ends[1] (U16 at +42) below ends[0] = 100 */
    save_vec(bool_runs(), p);
    NEED_ENC(0, 0, RAY_ENC_RLE);
    TEST_ASSERT_EQ_I(g_img[payload_at(0, 0) + 8], RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(g_img[payload_at(0, 0) + 32], RAY_ENC_RAW);
    v16 = 50; wr(payload_at(0, 0) + 42, &v16, 2); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    /* RLE last run end short of the vector length (ends[10] at +60, was 1024) */
    save_vec(bool_runs(), p);
    v16 = 1000; wr(payload_at(0, 0) + 60, &v16, 2); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    /* RLE run end beyond the vector length */
    save_vec(bool_runs(), p);
    v16 = 1025; wr(payload_at(0, 0) + 60, &v16, 2); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    /* CONST as a child */
    save_vec(bool_runs(), p);
    b = RAY_ENC_CONST; wr(payload_at(0, 0) + 8, &b, 1); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    /* child body_bytes overrun */
    save_vec(bool_runs(), p);
    v32 = 0xFFF0; wr(payload_at(0, 0) + 12, &v32, 4); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    /* null bitmap count disagrees with n_nulls */
    ray_t* v = ray_vec_new(RAY_I32, 100); v->len = 100;
    for (int i = 0; i < 100; i++) ((int32_t*)ray_data(v))[i] = i % 10 ? i : NULL_I32;
    save_vec(v, p);
    b = 0xFF; wr(payload_at(0, 0) + 5, &b, 1); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    /* float trailer position not in the bitmap */
    double d[4]; uint64_t odd = 0x7FF8000000000001ull;
    d[0] = 2.5; d[1] = NULL_F64; memcpy(&d[2], &odd, 8); d[3] = -1.0;
    save_vec(ray_vec_from_raw(RAY_F64, d, 4), p);
    v16 = 0; wr(payload_at(0, 0) + 128 + 32 + 8, &v16, 2); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    /* STR RAW: hash32 nonzero; inline padding; pool_off off by one */
    for (int k = 0; k < 3; k++) {
        ray_t* s = ray_vec_new(RAY_STR, 2);
        s = ray_str_vec_append(s, "short", 5);
        s = ray_str_vec_append(s, "a pooled string here", 20);
        save_vec(s, p);
        size_t at = payload_at(0, 0);
        if (k == 0) { v32 = 7; wr(at + 16 + 12, &v32, 4); }
        if (k == 1) { b = 'x'; wr(at + 4 + 6, &b, 1); }
        if (k == 2) { v32 = 1; wr(at + 16 + 8, &v32, 4); }
        img_store(p, g_len); EXPECT(p, 1, "corrupt");
    }
    /* DICT codes at or above the VALUE_DICT entry count: the codes child is
     * FOR_BP w=2; its base (@payload + 8) set to 4 makes every code >= 4 */
    save_vec(i64_dict(), p);
    NEED_ENC(0, 0, RAY_ENC_DICT);
    TEST_ASSERT_EQ_I(g_img[payload_at(0, 0)], RAY_ENC_FOR_BP);
    uint64_t base4 = 4; wr(payload_at(0, 0) + 8, &base4, 8); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    /* SYM ids at or above the saved SYM count (200) in RAW, CONST, FOR_BP
     * and RLE vectors whose statistics still claim ids below it */
    uint8_t ids[1024];
    for (int i = 0; i < 4; i++) ids[i] = (uint8_t)(i + 1);
    save_sym200(ids, 4, p); NEED_ENC(0, 0, RAY_ENC_RAW);
    b = 250; wr(payload_at(0, 0) + 2, &b, 1); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    for (int i = 0; i < 1024; i++) ids[i] = 5;
    save_sym200(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_CONST);
    wr(payload_at(0, 0), &b, 1); img_store(p, g_len); EXPECT(p, 1, "corrupt");
    for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(100 + i % 16);
    save_sym200(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_FOR_BP);
    uint64_t sb = 199; wr(payload_at(0, 0), &sb, 8); img_store(p, g_len); EXPECT(p, 1, "corrupt");  /* 199 + 1.. >= 200 */
    for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(1 + i / 100);
    save_sym200(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_RLE);
    wr(payload_at(0, 0) + 16, &b, 1); img_store(p, g_len); EXPECT(p, 1, "corrupt");                /* first run value */
    PASS();
}

static test_result_t test_reject_side_data(void) {
    const char* p = EC_TMP "/s";
    for (int k = 0; k < 5; k++) {
        save_vec(i64_dict(), p);
        NEED_ENC(0, 0, RAY_ENC_DICT);
        size_t side = (size_t)rd64(32 + 16), entry = side + 32;
        TEST_ASSERT_TRUE(side != 0);
        uint32_t one = 1; uint16_t kind9 = 9;
        if (k == 0) { int64_t x = rd64(entry + 8 + 8); wr(entry + 8, &x, 8); }   /* entries not ascending */
        if (k == 1) { uint32_t n = 3; wr(entry, &n, 4); }                          /* count disagrees with size */
        if (k == 2) wr(side + 4, &one, 4);                                         /* table reserved word */
        if (k == 3) wr(side + 8, &kind9, 2);                                       /* unknown kind */
        if (k == 4) wr(entry + 4, &one, 4);                                        /* entry reserved word */
        img_store(p, g_len);
        /* contents are checked before first use; structure at load */
        if (k <= 1 || k == 4) EXPECT(p, 1, "corrupt"); else EXPECT(p, 0, "corrupt");
    }
    PASS();
}

/* Damage in a range nobody reads stays unread; whoever reads it gets corrupt. */
static test_result_t test_lazy_validation(void) {
    const char* p = EC_TMP "/lazy";
    ray_t* v = ray_vec_new(RAY_I32, 2 * 65536); v->len = 2 * 65536;
    for (int i = 0; i < 2 * 65536; i++) ((int32_t*)ray_data(v))[i] = i % 100;
    save_vec(v, p);
    uint8_t b = 1; wr(desc_at(1, 0) + 6, &b, 1); img_store(p, g_len);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_col_src_t s; ray_col_src_bind(c, &s);
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    TEST_ASSERT_NULL(ray_col_range(&s, 0, 1024, &sc, &r));               /* chunk 0 fine */
    ray_t* e = ray_col_range(&s, 65536, 1024, &sc, &r);                  /* chunk 1 read */
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e) && strcmp(ray_err_code(e), "corrupt") == 0);
    ray_error_free(e);
    ray_col_scratch_free(&sc); ray_col_src_release(&s);
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && RAY_IS_ERR(m));
    ray_error_free(m);
    ray_release(c);
    PASS();
}

/* Codec grammar edge: a child chain deeper than 3 is rejected. */
static test_result_t test_reject_depth4(void) {
    uint8_t body[256] = {0};
    /* A child at depth 3 may only be RAW or FOR_BP: an RLE child there would
     * put its own children at depth 4, and the reader refuses it. */
    ray_enc_kind_t k = { 32, false };
    uint64_t out[RAY_ENC_VEC];
    body[0] = RAY_ENC_RLE;                       /* child header at depth 3: RLE */
    uint32_t bb = 8; memcpy(body + 4, &bb, 4);
    TEST_ASSERT_EQ_I(ray_enc_read(body, sizeof body, 0, 0, 4, k, RAY_ENC_SLOT_CODES, 3, NULL, out), -1);
    PASS();
}

static uint64_t g_fz = 0xC0FFEE;
static uint64_t fz(void) { uint64_t x = g_fz; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return g_fz = x; }

static bool ec_allowed_code(const char* c) {
    static const char* ok[] = { "corrupt", "nyi", "version", "range", "io" };
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) if (strcmp(c, ok[i]) == 0) return true;
    return false;
}

/* Random byte flips: corrupt or a result, never a crash, a hang or a read
 * outside the mapping (ASan/UBSan). */
static test_result_t test_fuzz_flips(void) {
    const char* p = EC_TMP "/fz";
    for (int base = 0; base < 5; base++) {
        ray_t* v;
        if (base == 0) v = bool_runs();
        else if (base == 1) { v = ray_vec_new(RAY_I32, 70000); v->len = 70000;
            for (int i = 0; i < 70000; i++) ((int32_t*)ray_data(v))[i] = i % 7 ? i % 300 : NULL_I32; }
        else if (base == 2) { double d[4]; uint64_t odd = 0x7FF8000000000001ull; d[0] = 2.5; d[1] = NULL_F64;
            memcpy(&d[2], &odd, 8); d[3] = -1.0; v = ray_vec_from_raw(RAY_F64, d, 4); }
        else if (base == 3) { v = ray_vec_new(RAY_STR, 50);
            for (int i = 0; i < 50; i++) v = ray_str_vec_append(v, i % 3 ? "a long pooled string value" : "s", i % 3 ? 26 : 1); }
        else v = i64_dict();
        int8_t want_type = v->type; int64_t want_len = v->len;
        save_vec(v, p);
        uint8_t* orig = malloc(g_len); memcpy(orig, g_img, g_len);
        for (int it = 0; it < 300; it++) {
            memcpy(g_img, orig, g_len);
            int flips = 1 + (int)(fz() % 3);
            for (int k = 0; k < flips; k++) g_img[fz() % g_len] ^= (uint8_t)(1u << (fz() % 8));
            img_store(p, g_len);
            ray_t* c = ray_col_mmap(p);
            const char* bad = NULL;
            if (c && !RAY_IS_ERR(c)) {
                ray_t* m = ray_enccol_materialize(c);
                if (m && RAY_IS_ERR(m)) {
                    if (!ec_allowed_code(ray_err_code(m))) bad = "materialize: error code outside the allowed set";
                    ray_error_free(m);
                } else if (!m) bad = "materialize returned NULL";
                else if (m->type != want_type || m->len != want_len) bad = "materialized vector is not well formed";
                ray_release(c);
            } else if (c) {
                if (!ec_allowed_code(ray_err_code(c))) bad = "load: error code outside the allowed set";
                ray_error_free(c);
            } else bad = "ray_col_mmap returned NULL";
            if (bad) { free(orig); TEST_ASSERT_FMT(0, "base %d iter %d: %s", base, it, bad); }
        }
        free(orig);
    }
    PASS();
}

const test_entry_t enccorrupt_entries[] = {
    { "enccorrupt/headers",     test_reject_headers,     ec_setup, ec_teardown },
    { "enccorrupt/directory",   test_reject_directory,   ec_setup, ec_teardown },
    { "enccorrupt/descriptors", test_reject_descriptors, ec_setup, ec_teardown },
    { "enccorrupt/payloads",    test_reject_payloads,    ec_setup, ec_teardown },
    { "enccorrupt/side_data",   test_reject_side_data,   ec_setup, ec_teardown },
    { "enccorrupt/lazy",        test_lazy_validation,    ec_setup, ec_teardown },
    { "enccorrupt/depth4",      test_reject_depth4,      ec_setup, ec_teardown },
    { "enccorrupt/fuzz_flips",  test_fuzz_flips,         ec_setup, ec_teardown },
    { NULL, NULL, NULL, NULL },
};
