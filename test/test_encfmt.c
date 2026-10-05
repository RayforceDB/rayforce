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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENCFMT_TMP "/tmp/rayforce_encfmt"

static void encfmt_setup(void)    { ray_heap_init(); (void)ray_sym_init(); (void)ray_test_mkdir_p(ENCFMT_TMP); }
static void encfmt_teardown(void) { ray_sym_destroy(); ray_heap_destroy(); (void)ray_test_rm_rf(ENCFMT_TMP); }

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

const test_entry_t encfmt_entries[] = {
    { "encfmt/discriminator_pairs",       test_discriminator_pairs,       encfmt_setup, encfmt_teardown },
    { "encfmt/discriminator_cross_pairs", test_discriminator_cross_pairs, encfmt_setup, encfmt_teardown },
    { "encfmt/gen0_writer_zeroes_aux",    test_gen0_writer_zeroes_aux,    encfmt_setup, encfmt_teardown },
    { "encfmt/length_helpers",            test_length_helpers,            encfmt_setup, encfmt_teardown },
    { NULL, NULL, NULL, NULL },
};
