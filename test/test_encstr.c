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
#include "store/fsst.h"
#include "vec/str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ST_TMP "/tmp/rayforce_encstr"        /* always through ray_test_tmp (per-process root) */
/* Buffers a test owns, freed in teardown so an early-returning assertion
 * leaks nothing. */
static ray_enc_buf_t st_buf;
static uint8_t*      st_mem;                 /* malloc'd scratch */
static void st_setup(void) {
    ray_heap_init(); (void)ray_sym_init();
    (void)ray_test_rm_rf(ray_test_tmp(ST_TMP)); (void)ray_test_mkdir_p(ray_test_tmp(ST_TMP));
}
static void st_teardown(void) {
    ray_enc_buf_free(&st_buf); st_buf = (ray_enc_buf_t){0};
    free(st_mem); st_mem = NULL;
    ray_test_hooks_off(); (void)ray_test_rm_rf(ray_test_tmp(ST_TMP)); ray_sym_destroy(); ray_heap_destroy();
}

#define ST_WHY(s) TEST_ASSERT_TRUE(strcmp(ray_enc_read_why(), (s)) == 0)

static test_result_t test_strdict_body(void) {
    uint64_t codes[RAY_ENC_VEC], out[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) codes[i] = (uint64_t)((i / 200) % 4);
    ray_enc_buf_t* b = &st_buf;
    TEST_ASSERT_TRUE(ray_enc_strdict_write(codes, RAY_ENC_VEC, b));
    TEST_ASSERT_EQ_I((int64_t)b->len, ray_enc_strdict_size(codes, RAY_ENC_VEC));
    TEST_ASSERT_EQ_I(b->p[0], RAY_ENC_RLE);                                /* codes child: runs */
    TEST_ASSERT_EQ_I(ray_enc_strdict_read(b->p, b->len, RAY_ENC_VEC, 4, out), (int64_t)b->len);
    TEST_ASSERT_TRUE(memcmp(codes, out, sizeof codes) == 0);
    TEST_ASSERT_EQ_I(ray_enc_strdict_read(b->p, b->len, RAY_ENC_VEC, 3, out), -1);   /* code 3 >= 3 */
    ST_WHY("STR_DICT code at or above the entry count");
    /* a body followed by more bytes than it uses */
    uint64_t zero = 0;
    size_t used = b->len;
    TEST_ASSERT_TRUE(ray_enc_buf_put(b, &zero, 8));
    TEST_ASSERT_EQ_I(ray_enc_strdict_read(b->p, b->len, RAY_ENC_VEC, 4, out), -1);
    ST_WHY("bytes after the STR_DICT body");
    /* a short body: the child reader names its own rule */
    TEST_ASSERT_EQ_I(ray_enc_strdict_read(b->p, used - 8, RAY_ENC_VEC, 4, out), -1);
    ST_WHY("child body overruns its parent");
    PASS();
}

static const char* const g_urls[] = {
    "http://example.com/a/b?c=1", "http://example.com/a/b?c=2", "https://shop.example.org/item/42",
    "", "x", "http://example.com/", "a fairly long free text title with words",
};
#define N_URLS 7

static void st_strings(int n, const char** s, uint32_t* len) {
    for (int i = 0; i < n; i++) { s[i] = g_urls[(i * 5) % N_URLS]; len[i] = (uint32_t)strlen(s[i]); }
}

static test_result_t test_fsst_body(void) {
    const char* s[RAY_ENC_VEC]; uint32_t len[RAY_ENC_VEC];
    st_strings(RAY_ENC_VEC, s, len);
    ray_fsst_table_t t;
    TEST_ASSERT_EQ_I(ray_fsst_train((const uint8_t* const*)s, len, RAY_ENC_VEC, &t), 1);
    TEST_ASSERT_TRUE(t.n > 0);
    ray_fsst_index_t ix; ray_fsst_index_build(&t, &ix);
    uint64_t sum = 0; for (int i = 0; i < RAY_ENC_VEC; i++) sum += len[i];
    uint8_t* buf = st_mem = malloc(2 * sum + 1);
    TEST_ASSERT_NOT_NULL(buf);
    uint64_t clen[RAY_ENC_VEC], xlen[RAY_ENC_VEC];
    uint64_t total = ray_enc_fsst_encode(&t, &ix, s, len, RAY_ENC_VEC, buf, clen);
    TEST_ASSERT_TRUE(total < sum);
    ray_enc_buf_t* b = &st_buf;
    TEST_ASSERT_TRUE(ray_enc_fsst_write(clen, RAY_ENC_VEC, buf, total, b));
    TEST_ASSERT_EQ_I((int64_t)b->len, ray_enc_fsst_size(clen, RAY_ENC_VEC, total));
    TEST_ASSERT_EQ_I((int64_t)(b->len & 7), 0);
    const uint8_t* bytes;
    uint64_t c2[RAY_ENC_VEC];
    TEST_ASSERT_EQ_I(ray_enc_fsst_check(b->p, b->len, RAY_ENC_VEC, &t, c2, xlen, &bytes), (int64_t)b->len);
    uint64_t at = 0;
    for (int i = 0; i < RAY_ENC_VEC; i++) {
        TEST_ASSERT_EQ_U(c2[i], clen[i]);
        TEST_ASSERT_EQ_U(xlen[i], len[i]);
        char out[64];
        TEST_ASSERT_EQ_I(ray_fsst_decode(&t, bytes + at, (uint32_t)c2[i], (uint8_t*)out, sizeof out), (int64_t)len[i]);
        TEST_ASSERT_TRUE(memcmp(out, s[i], len[i]) == 0);
        at += c2[i];
    }
    TEST_ASSERT_EQ_U(at, total);
    PASS();
}

/* ray_fsst_train's verdict (D3): no string bytes trains no table. */
static test_result_t test_fsst_train_verdict(void) {
    const uint8_t* s[3] = { (const uint8_t*)"", (const uint8_t*)"", (const uint8_t*)"" };
    uint32_t len[3] = { 0, 0, 0 };
    ray_fsst_table_t t; memset(&t, 0xAB, sizeof t);
    TEST_ASSERT_EQ_I(ray_fsst_train(s, len, 3, &t), 0);
    TEST_ASSERT_EQ_I(t.n, 0);
    TEST_ASSERT_EQ_I(ray_fsst_train(s, len, 0, &t), 0);
    TEST_ASSERT_EQ_I(t.n, 0);
    const uint8_t* s1[1] = { (const uint8_t*)"abc" }; uint32_t l1[1] = { 3 };
    TEST_ASSERT_EQ_I(ray_fsst_train(s1, l1, 1, &t), 1);
    TEST_ASSERT_TRUE(t.n > 0);
    PASS();
}

/* Spec "Reject rules" (FSST): a code in [n_symbols, 255), a trailing escape,
 * an escape whose literal is in the next slice, a total not the sum,
 * non-zero padding, bytes after the body. */
static test_result_t test_fsst_body_rejects(void) {
    ray_fsst_table_t t; memset(&t, 0, sizeof t);
    t.n = 2; t.len[0] = 2; memcpy(t.sym[0], "ab", 2); t.len[1] = 1; t.sym[1][0] = 'c';
    uint64_t clen[2] = { 3, 2 };
    uint8_t good[5] = { 0, 255, 'z', 1, 0 };                             /* "abz", "cab" */
    ray_enc_buf_t* b = &st_buf;
    TEST_ASSERT_TRUE(ray_enc_fsst_write(clen, 2, good, 5, b));
    uint64_t c2[2], x2[2]; const uint8_t* bytes;
    TEST_ASSERT_EQ_I(ray_enc_fsst_check(b->p, b->len, 2, &t, c2, x2, &bytes), (int64_t)b->len);
    TEST_ASSERT_EQ_U(x2[0], 3); TEST_ASSERT_EQ_U(x2[1], 3);
    size_t at = (size_t)(bytes - b->p);
    uint8_t* m = st_mem = malloc(b->len + 8);
    TEST_ASSERT_NOT_NULL(m);
    memcpy(m, b->p, b->len); m[at] = 7;                                  /* 2 <= 7 < 255 */
    TEST_ASSERT_EQ_I(ray_enc_fsst_check(m, b->len, 2, &t, c2, x2, &bytes), -1);
    ST_WHY("FSST slice");
    memcpy(m, b->p, b->len); m[at + 5] = 1;                              /* padding byte */
    TEST_ASSERT_EQ_I(ray_enc_fsst_check(m, b->len, 2, &t, c2, x2, &bytes), -1);
    ST_WHY("FSST padding");
    memcpy(m, b->p, b->len); memset(m + b->len, 0, 8);                   /* 8 more bytes */
    TEST_ASSERT_EQ_I(ray_enc_fsst_check(m, b->len + 8, 2, &t, c2, x2, &bytes), -1);
    ST_WHY("bytes after the FSST body");
    TEST_ASSERT_EQ_I(ray_enc_fsst_check(m, b->len - 8, 2, &t, c2, x2, &bytes), -1);   /* cut short */
    ST_WHY("FSST compressed bytes short");
    ray_enc_buf_free(b); *b = (ray_enc_buf_t){0};
    uint64_t cl3[2] = { 2, 3 };                                          /* escape ends slice 0 */
    uint8_t bad[5] = { 0, 255, 'z', 1, 0 };
    TEST_ASSERT_TRUE(ray_enc_fsst_write(cl3, 2, bad, 5, b));
    TEST_ASSERT_EQ_I(ray_enc_fsst_check(b->p, b->len, 2, &t, c2, x2, &bytes), -1);
    ST_WHY("FSST slice");
    ray_enc_buf_free(b); *b = (ray_enc_buf_t){0};
    uint64_t cl4[1] = { 2 };
    uint8_t tail[2] = { 0, 255 };                                        /* trailing escape */
    TEST_ASSERT_TRUE(ray_enc_fsst_write(cl4, 1, tail, 2, b));
    TEST_ASSERT_EQ_I(ray_enc_fsst_check(b->p, b->len, 1, &t, c2, x2, &bytes), -1);
    ST_WHY("FSST slice");
    /* total_bytes not the sum of lengths */
    uint32_t lb; memcpy(&lb, b->p + 4, 4);
    uint64_t wrong = 3; memcpy(b->p + 8 + lb, &wrong, 8);
    TEST_ASSERT_EQ_I(ray_enc_fsst_check(b->p, b->len, 1, &t, c2, x2, &bytes), -1);
    ST_WHY("FSST total not the sum of lengths");
    /* no room for total_bytes after the lengths child */
    TEST_ASSERT_EQ_I(ray_enc_fsst_check(b->p, 8 + lb, 1, &t, c2, x2, &bytes), -1);
    ST_WHY("FSST total short");
    PASS();
}

const test_entry_t encstr_entries[] = {
    { "encstr/strdict_body",       test_strdict_body,       st_setup, st_teardown },
    { "encstr/fsst_body",          test_fsst_body,          st_setup, st_teardown },
    { "encstr/fsst_train_verdict", test_fsst_train_verdict, st_setup, st_teardown },
    { "encstr/fsst_body_rejects",  test_fsst_body_rejects,  st_setup, st_teardown },
    { NULL, NULL, NULL, NULL },
};
