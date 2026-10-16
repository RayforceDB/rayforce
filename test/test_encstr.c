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
#include "store/col.h"
#include "store/encw.h"
#include "store/enccol.h"
#include "store/colrange.h"
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

/* ---- the STR writer (Task 2) ---- */
#define STP(p) ray_test_tmp(ST_TMP p)

static ray_t* str_col(int64_t n, const char* (*gen)(int64_t, char*)) {
    ray_t* v = ray_vec_new(RAY_STR, n);
    char buf[256];
    for (int64_t i = 0; i < n; i++) { const char* s = gen(i, buf); v = ray_str_vec_append(v, s, strlen(s)); }
    return v;
}
static const char* gen_lowcard(int64_t i, char* b) { snprintf(b, 64, "category number %lld long", (long long)(i % 37)); return b; }
static const char* gen_urls(int64_t i, char* b) { snprintf(b, 200, "https://www.example.com/path/%lld/item?id=%lld&ref=search", (long long)(i % 5000), (long long)(i * 7919 % 100003)); return b; }
static const char* gen_nullish(int64_t i, char* b) { if (i % 9 == 0) return ""; return gen_lowcard(i, b); }
/* High cardinality in the low-cardinality strings' shape: no dictionary chunk,
 * but an FSST table trained on gen_lowcard compresses them. */
static const char* gen_hc(int64_t i, char* b) { snprintf(b, 64, "category number %lld long", (long long)i); return b; }

/* `path` is already wrapped by ray_test_tmp. */
static int st_desc_enc(const char* path, int64_t vec, ray_enc_desc_t* out) {
    FILE* f = fopen(path, "rb"); if (!f) return -1;
    int64_t c = vec / 64, j = vec % 64;
    uint64_t off; fseek(f, 96 + c * 64, SEEK_SET); if (fread(&off, 8, 1, f) != 1) { fclose(f); return -1; }
    ray_enc_desc_t d; fseek(f, (long)(off + (uint64_t)j * 48), SEEK_SET);
    int ok = fread(&d, 1, 48, f) == 48;
    fclose(f);
    if (!ok) return -1;
    if (out) *out = d;
    return d.encoding;
}
static ray_enc_hdr_t st_hdr(const char* path) {
    ray_enc_hdr_t eh; memset(&eh, 0, sizeof eh);
    FILE* f = fopen(path, "rb"); if (!f) return eh;
    fseek(f, 32, SEEK_SET); if (fread(&eh, 1, 64, f) != 64) memset(&eh, 0, sizeof eh);
    fclose(f);
    return eh;
}
static ray_enc_dirent_t st_dirent(const char* path, int64_t c) {
    ray_enc_dirent_t de; memset(&de, 0, sizeof de);
    FILE* f = fopen(path, "rb"); if (!f) return de;
    fseek(f, 96 + c * 64, SEEK_SET); if (fread(&de, 1, 64, f) != 64) memset(&de, 0, sizeof de);
    fclose(f);
    return de;
}

/* Low cardinality: STR_DICT with code bounds; directory bounds present. */
static test_result_t test_writer_strdict(void) {
    ray_t* v = str_col(70000, gen_nullish);
    ray_err_t e = ray_col_save_gen(v, STP("/sd"), 2);
    ray_release(v);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    ray_enc_desc_t d;
    TEST_ASSERT_EQ_I(st_desc_enc(STP("/sd"), 0, &d), RAY_ENC_STR_DICT);
    TEST_ASSERT_FALSE(d.flags & RAY_ENC_STAT_ABSENT);
    TEST_ASSERT_TRUE(d.min >= 1 && d.max <= 37 && d.min <= d.max);   /* "" (row 0) is code 0; the bounds cover non-null rows */
    TEST_ASSERT_EQ_I(st_desc_enc(STP("/sd"), 68, NULL), RAY_ENC_STR_DICT);   /* chunk 1 reuses the entries */
    ray_enc_dirent_t de = st_dirent(STP("/sd"), 0);
    TEST_ASSERT_FALSE(de.flags & RAY_ENC_STAT_ABSENT);
    TEST_ASSERT_TRUE(de.min >= 1 && de.max <= 37 && de.min <= de.max);
    ray_enc_hdr_t eh = st_hdr(STP("/sd"));
    TEST_ASSERT_TRUE(eh.side_off != 0);
    /* rule 1: the 37 entries over 12 bytes once; no RAW / FSST rows here */
    uint64_t want = 0; char b[64];
    for (int k = 0; k < 37; k++) { uint64_t l = (uint64_t)snprintf(b, sizeof b, "category number %d long", k); if (l > 12) want += l; }
    TEST_ASSERT_EQ_U(eh.str_pool_bytes, want);
    PASS();
}

/* URL-like strings: FSST; no statistics; the FSST_TABLE side entry. */
static test_result_t test_writer_fsst(void) {
    ray_t* v = str_col(3000, gen_urls);
    ray_err_t e = ray_col_save_gen(v, STP("/fs"), 2);
    ray_release(v);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    ray_enc_desc_t d;
    TEST_ASSERT_EQ_I(st_desc_enc(STP("/fs"), 0, &d), RAY_ENC_FSST);
    TEST_ASSERT_TRUE(d.flags & RAY_ENC_STAT_ABSENT);
    ray_enc_dirent_t de = st_dirent(STP("/fs"), 0);
    TEST_ASSERT_TRUE(de.flags & RAY_ENC_STAT_ABSENT);
    ray_enc_hdr_t eh = st_hdr(STP("/fs"));
    uint64_t want = 0; char b[256];
    for (int64_t i = 0; i < 3000; i++) { uint64_t l = strlen(gen_urls(i, b)); if (l > 12) want += l; }
    TEST_ASSERT_EQ_U(eh.str_pool_bytes, want);                         /* rule 2 */
    FILE* f = fopen(STP("/fs"), "rb"); TEST_ASSERT_NOT_NULL(f);
    uint8_t tb[32]; fseek(f, (long)eh.side_off, SEEK_SET);
    size_t got = fread(tb, 1, 32, f); fclose(f);
    TEST_ASSERT_EQ_I((int)got, 32);
    uint32_t ne; memcpy(&ne, tb, 4); uint16_t k0; memcpy(&k0, tb + 8, 2);
    TEST_ASSERT_EQ_I(ne, 1);
    TEST_ASSERT_EQ_I(k0, RAY_ENC_SIDE_FSST_TABLE);
    PASS();
}

/* Two side entries in kind order, 32-aligned: chunk 0 is a STR_DICT chunk
 * (low cardinality), chunk 1 a plain one (gen_hc, FSST).  A STR_DICT chunk
 * holding an all-null vector (CONST) has no code bounds. */
static test_result_t test_writer_two_entries(void) {
    int64_t n = 65536 + 4096;
    ray_t* v = ray_vec_new(RAY_STR, n);
    char b[256];
    for (int64_t i = 0; i < n; i++) {
        const char* s = i < 1024 ? "" : i < 65536 ? gen_lowcard(i, b) : gen_hc(i, b);
        v = ray_str_vec_append(v, s, strlen(s));
    }
    ray_err_t e = ray_col_save_gen(v, STP("/two"), 2);
    ray_release(v);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(st_desc_enc(STP("/two"), 0, NULL), RAY_ENC_CONST);      /* all null */
    ray_enc_desc_t d;
    TEST_ASSERT_EQ_I(st_desc_enc(STP("/two"), 1, &d), RAY_ENC_STR_DICT);
    TEST_ASSERT_FALSE(d.flags & RAY_ENC_STAT_ABSENT);                       /* the vector keeps its bounds (D5) */
    TEST_ASSERT_EQ_I(st_desc_enc(STP("/two"), 64, NULL), RAY_ENC_FSST);
    TEST_ASSERT_TRUE(st_dirent(STP("/two"), 0).flags & RAY_ENC_STAT_ABSENT);  /* CONST in the chunk */
    TEST_ASSERT_TRUE(st_dirent(STP("/two"), 1).flags & RAY_ENC_STAT_ABSENT);
    ray_enc_hdr_t eh = st_hdr(STP("/two"));
    FILE* f = fopen(STP("/two"), "rb");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, (long)eh.side_off, SEEK_SET);
    uint8_t tb[64]; size_t got = fread(tb, 1, 56, f); fclose(f);
    TEST_ASSERT_EQ_I((int)got, 56);
    uint32_t ne; memcpy(&ne, tb, 4);
    TEST_ASSERT_EQ_I(ne, 2);
    uint16_t k0, k1; memcpy(&k0, tb + 8, 2); memcpy(&k1, tb + 32, 2);
    TEST_ASSERT_EQ_I(k0, RAY_ENC_SIDE_STR_DICT); TEST_ASSERT_EQ_I(k1, RAY_ENC_SIDE_FSST_TABLE);
    uint64_t o0, o1; memcpy(&o0, tb + 16, 8); memcpy(&o1, tb + 40, 8);
    TEST_ASSERT_EQ_U(o0, 64);
    TEST_ASSERT_EQ_U(o1 % 32, 0);
    PASS();
}

/* ---- the reader (Task 3): side entries, statistics, vector-scope decode ---- */

/* `path` is already wrapped by ray_test_tmp. */
static bool st_patch(const char* path, long off, const void* b, size_t n) {
    FILE* f = fopen(path, "r+b");
    if (!f) return false;
    bool ok = fseek(f, off, SEEK_SET) == 0 && fwrite(b, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
#define ST_PATCH(path, off, b, n) TEST_ASSERT_TRUE(st_patch((path), (long)(off), (b), (n)))
static bool st_read(const char* path, long off, void* b, size_t n) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fseek(f, off, SEEK_SET) == 0 && fread(b, 1, n, f) == n;
    fclose(f);
    return ok;
}
#define ST_READ(path, off, b, n) TEST_ASSERT_TRUE(st_read((path), (long)(off), (b), (n)))
static char st_msg[512];                     /* the last st_mat / st_scan error's message */
/* The error code of loading `path` and materializing it ("" when both pass). */
static const char* st_mat(const char* path) {
    static char code[16];
    st_msg[0] = 0;
    ray_error_clear();
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) {
        snprintf(code, sizeof code, "%s", c ? ray_err_code(c) : "null");
        snprintf(st_msg, sizeof st_msg, "%s", ray_error_msg() ? ray_error_msg() : "");
        if (c) ray_error_free(c);
        return code;
    }
    ray_t* m = ray_enccol_materialize(c);
    snprintf(code, sizeof code, "%s", RAY_IS_ERR(m) ? ray_err_code(m) : "");
    if (RAY_IS_ERR(m)) snprintf(st_msg, sizeof st_msg, "%s", ray_error_msg() ? ray_error_msg() : "");
    if (RAY_IS_ERR(m)) ray_error_free(m);
    ray_release(c);
    return code;
}
/* The error code of loading `path` and range-reading every vector (vector
 * scope: each STR_DICT / FSST vector decodes alone), "" when both pass.
 * Materialization of a STR_DICT column with pooled entries reports corrupt
 * until the dictionary-first pool lands (D1, Task 4), so the side-entry
 * rules are pinned through the range reads, which validate the same entry. */
static const char* st_scan(const char* path) {
    static char code[16];
    st_msg[0] = 0;
    ray_error_clear();
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) {
        snprintf(code, sizeof code, "%s", c ? ray_err_code(c) : "null");
        snprintf(st_msg, sizeof st_msg, "%s", ray_error_msg() ? ray_error_msg() : "");
        if (c) ray_error_free(c);
        return code;
    }
    ray_col_src_t src;
    ray_t* err = ray_col_src_bind(c, &src);
    if (!err) {
        ray_col_scratch_t sc; ray_col_scratch_init(&sc);
        for (int64_t r = 0; r < src.len && !err; ) {
            ray_col_range_t rv;
            err = ray_col_range(&src, r, src.len - r, &sc, &rv);
            if (!err) r += rv.n;
        }
        ray_col_scratch_free(&sc); ray_col_src_release(&src);
    }
    snprintf(code, sizeof code, "%s", err ? ray_err_code(err) : "");
    if (err) snprintf(st_msg, sizeof st_msg, "%s", ray_error_msg() ? ray_error_msg() : "");
    if (err) ray_error_free(err);
    ray_release(c);
    return code;
}
/* st_scan gave corrupt and its message names the column and `needle`. */
#define ST_SCAN_CORRUPT(path, needle) do { TEST_ASSERT_STR_EQ(st_scan(path), "corrupt"); \
    TEST_ASSERT_FMT(strncmp(st_msg, "column ", 7) == 0 && strstr(st_msg, (path)) && strstr(st_msg, (needle)), \
                    "message \"%s\", want [%s]", st_msg, (needle)); } while (0)

/* Range reads decode each vector alone (vector scope): chunk 0 is a
 * STR_DICT chunk ("" every 9th row, in band), chunk 1 FSST.  Nothing is
 * materialized, and range reads leave hash32 0. */
static test_result_t test_range_reads_str(void) {
    const char* p = STP("/rr");
    ray_t* v = ray_vec_new(RAY_STR, 70000);
    char b[256];
    for (int64_t i = 0; i < 70000; i++) { const char* s = i >= 65536 ? gen_hc(i, b) : gen_nullish(i, b); v = ray_str_vec_append(v, s, strlen(s)); }
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_STR_DICT);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 64, NULL), RAY_ENC_FSST);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_col_src_t src; TEST_ASSERT_NULL(ray_col_src_bind(c, &src));
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_enc_counters_reset();
    bool same = true; int64_t bad = -1;
    for (int64_t r = 0; r < 70000 && same; ) {
        ray_col_range_t rv;
        ray_t* err = ray_col_range(&src, r, 70000 - r, &sc, &rv);
        if (err) { ray_error_free(err); same = false; bad = r; break; }
        for (int64_t k = 0; k < rv.n && same; k++) {
            size_t lb; const char* want = ray_str_vec_get(v, r + k, &lb);
            const ray_str_t* d = (const ray_str_t*)rv.data + k;
            if (d->len != lb || (lb && memcmp(ray_str_t_ptr(d, rv.str_pool), want, lb) != 0) ||
                (lb > RAY_STR_INLINE_MAX && d->hash32 != 0)) { same = false; bad = r + k; }
        }
        r += rv.n;
    }
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    ray_col_scratch_free(&sc); ray_col_src_release(&src); ray_release(c); ray_release(v);
    TEST_ASSERT_FMT(same, "row %lld differs", (long long)bad);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    PASS();
}

/* Side-entry and statistics reject rules (before any lookup / decode). */
static test_result_t test_side_rejects(void) {
    const char* p = STP("/sr");
    ray_t* v = str_col(5000, gen_lowcard);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_STR_DICT);
    ray_enc_hdr_t eh = st_hdr(p);
    const long e0 = (long)eh.side_off + 32;               /* the STR_DICT entry (the only one) */
    TEST_ASSERT_STR_EQ(st_scan(p), "");
    uint32_t n; ST_READ(p, e0, &n, 4);
    TEST_ASSERT_EQ_U(n, 37);
    uint64_t o1, o2; ST_READ(p, e0 + 16, &o1, 8); ST_READ(p, e0 + 24, &o2, 8);
    const long bytes = e0 + 8 + 8 * (long)(n + 1);
    /* the lookup of a good file finds its codes (first occurrence) */
    {
        ray_t* c = ray_col_mmap(p);
        TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
        int64_t c1 = 7, cx = 7;
        ray_t* e1 = ray_enccol_strdict_code(ray_enccol_of(c), "category number 1 long", 22, &c1);
        ray_t* e2 = ray_enccol_strdict_code(ray_enccol_of(c), "category number 99 long", 23, &cx);
        ray_release(c);
        TEST_ASSERT_NULL(e1); TEST_ASSERT_NULL(e2);
        TEST_ASSERT_EQ_I(c1, 1); TEST_ASSERT_EQ_I(cx, -1);
    }
    /* duplicate entries: make entry 1 a copy of entry 0 */
    TEST_ASSERT_EQ_U(o2 - o1, o1);
    {
        char e0b[64]; TEST_ASSERT_TRUE(o1 <= sizeof e0b);
        ST_READ(p, bytes, e0b, (size_t)o1);
        ST_PATCH(p, bytes + (long)o1, e0b, (size_t)o1);
        ST_SCAN_CORRUPT(p, "STR_DICT entries not distinct");
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    }
    uint64_t bad = o2 + 1;                                /* offsets not nondecreasing */
    ST_PATCH(p, e0 + 16, &bad, 8);
    ST_SCAN_CORRUPT(p, "STR_DICT offsets");
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    /* offsets[0] != 0: the lookup of an equality filter fails on the
     * dictionary's validation, before any code is compared */
    uint64_t o0 = 1; ST_PATCH(p, e0 + 8, &o0, 8);
    {
        ray_t* c = ray_col_mmap(p);
        TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
        int64_t code = 7;
        ray_t* err = ray_enccol_strdict_code(ray_enccol_of(c), "category number 1 long", 22, &code);
        bool is_corrupt = err && strcmp(ray_err_code(err), "corrupt") == 0;
        if (err) ray_error_free(err);
        ray_release(c);
        TEST_ASSERT_TRUE(is_corrupt);
        TEST_ASSERT_EQ_I(code, 7);                        /* no code produced */
    }
    ST_SCAN_CORRUPT(p, "STR_DICT offsets");
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    /* reserved word; a non-zero padding byte after the entries */
    uint32_t one = 1; ST_PATCH(p, e0 + 4, &one, 4);
    ST_SCAN_CORRUPT(p, "STR_DICT entry reserved word");
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    {
        uint64_t end; ST_READ(p, e0 + 8 + 8 * (long)n, &end, 8);
        if (end & 7) { uint8_t x = 1; ST_PATCH(p, bytes + (long)end, &x, 1); ST_SCAN_CORRUPT(p, "STR_DICT padding"); }
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    }
    /* n = 0 and n above the cap */
    uint32_t n0 = 0; ST_PATCH(p, e0, &n0, 4);
    ST_SCAN_CORRUPT(p, "STR code bounds outside the dictionary");   /* at load: every bound is >= 0 */
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    uint32_t nbig = RAY_ENCW_SDICT_CAP + 1; ST_PATCH(p, e0, &nbig, 4);
    ST_SCAN_CORRUPT(p, "STR_DICT entry count");
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    /* a directory code bound at the entry count is corrupt at load */
    int64_t big = (int64_t)n; ST_PATCH(p, 96 + 24, &big, 8);
    TEST_ASSERT_STR_EQ(st_mat(p), "corrupt");
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    /* a vector code bound at the entry count (directory raised with it):
     * corrupt at the descriptors */
    ST_PATCH(p, 96 + 24, &(int64_t){ (int64_t)n - 1 }, 8);
    {
        ray_enc_dirent_t de = st_dirent(p, 0);
        TEST_ASSERT_EQ_I(de.max, (int64_t)n - 1);
        uint64_t doff = de.offset;
        ST_PATCH(p, (long)doff + 40, &(int64_t){ (int64_t)n }, 8);   /* vector 0's max */
    }
    ST_SCAN_CORRUPT(p, "STR code bounds outside the dictionary");
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    /* a code outside its vector's bounds: narrow vector 0's max below a code it holds */
    {
        ray_enc_desc_t d; TEST_ASSERT_EQ_I(st_desc_enc(p, 0, &d), RAY_ENC_STR_DICT);
        TEST_ASSERT_TRUE(d.max > d.min);
        ray_enc_dirent_t de = st_dirent(p, 0);
        ST_PATCH(p, (long)de.offset + 40, &(int64_t){ d.max - 1 }, 8);
        ST_SCAN_CORRUPT(p, "code outside its statistics");
    }
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    /* statistics dropped from a STR_DICT vector */
    {
        ray_enc_dirent_t de = st_dirent(p, 0);
        uint16_t fl = RAY_ENC_STAT_ABSENT; int64_t z = 0;
        ST_PATCH(p, (long)de.offset + 4, &fl, 2);
        ST_PATCH(p, (long)de.offset + 32, &z, 8); ST_PATCH(p, (long)de.offset + 40, &z, 8);
        ST_SCAN_CORRUPT(p, "statistics missing on a STR_DICT vector");
    }
    ray_release(v);
    PASS();
}

/* FSST reject rules in a file, at materialization (spec "Reject rules":
 * a code between n_symbols and 255, a trailing escape, an escape whose
 * literal is in the next slice, a total not equal to str_pool_bytes), with
 * no pool allocated, so nothing is written.  Strings of 'a' only keep the
 * trained table at a few symbols, so a code in [n_symbols, 255) exists. */
static const char* gen_as(int64_t i, char* b) {
    int len = 13 + (int)i;                                /* every row distinct: no dictionary chunk */
    memset(b, 'a', (size_t)len); b[len] = 0;
    return b;
}
static test_result_t test_fsst_rejects(void) {
    const char* p = STP("/fr");
    ray_t* v = ray_vec_new(RAY_STR, 2048);
    char* b = (char*)(st_mem = malloc(2048 + 16));
    TEST_ASSERT_NOT_NULL(b);
    for (int64_t i = 0; i < 2048; i++) { const char* s = gen_as(i, b); v = ray_str_vec_append(v, s, strlen(s)); }
    ray_err_t se = ray_col_save_gen(v, p, 2);
    ray_release(v);
    TEST_ASSERT_EQ_I(se, RAY_OK);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_FSST);
    ray_enc_hdr_t eh = st_hdr(p);
    const long tab = (long)eh.side_off + 32;              /* the FSST_TABLE entry (the only entry) */
    FILE* f = fopen(p, "rb"); TEST_ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END); size_t fn = (size_t)ftell(f);
    free(st_mem); uint8_t* img = st_mem = malloc(fn);
    if (!img) { fclose(f); FAIL("malloc"); }
    fseek(f, 0, SEEK_SET); size_t got = fread(img, 1, fn, f); fclose(f);
    TEST_ASSERT_EQ_U(got, fn);
    uint8_t ns = img[tab];
    TEST_ASSERT_TRUE(ns >= 1 && ns < 254);
    /* vector 0's body: lengths child, total, slices (no bitmap for STR) */
    const long c0 = 96 + 64;                              /* one chunk: its descriptors follow the directory */
    ray_enc_desc_t d0; memcpy(&d0, img + c0, 48);
    const uint8_t* body = img + c0 + d0.payload_off;
    uint64_t clen[RAY_ENC_VEC];
    const ray_enc_kind_t k32 = { .T = 32 };
    int64_t a = ray_enc_read(body, (size_t)d0.payload_bytes, 0, 0, RAY_ENC_VEC, k32, RAY_ENC_SLOT_FSST_LENGTHS, 1, NULL, clen);
    TEST_ASSERT_TRUE(a > 0 && clen[0] >= 1);
    const long slice0 = c0 + (long)d0.payload_off + (long)a + 8;
    uint64_t allocs0 = ray_enccol_test_str_pool_allocs();
    TEST_ASSERT_STR_EQ(st_mat(p), "");
    TEST_ASSERT_EQ_U(ray_enccol_test_str_pool_allocs(), allocs0 + 1);     /* a good file allocates its pool once */
    #define FR_RESET() do { FILE* g_ = fopen(p, "wb"); TEST_ASSERT_NOT_NULL(g_); \
        size_t w_ = fwrite(img, 1, fn, g_); fclose(g_); TEST_ASSERT_EQ_U(w_, fn); } while (0)
    #define FR_EXPECT(needle) do { uint64_t c_ = ray_enccol_test_str_pool_allocs(); \
        TEST_ASSERT_STR_EQ(st_mat(p), "corrupt"); \
        TEST_ASSERT_FMT(strstr(st_msg, (needle)) != NULL, "message \"%s\", want [%s]", st_msg, (needle)); \
        TEST_ASSERT_EQ_U(ray_enccol_test_str_pool_allocs(), c_); FR_RESET(); } while (0)   /* no pool: nothing written */
    uint8_t code = ns;                    ST_PATCH(p, slice0, &code, 1);                       FR_EXPECT("codec body: FSST slice");
    uint8_t esc = RAY_FSST_ESC;           ST_PATCH(p, slice0 + (long)clen[0] - 1, &esc, 1);    FR_EXPECT("codec body: FSST slice");  /* trailing; its literal would be in slice 1 */
    uint64_t sp = eh.str_pool_bytes + 1;  ST_PATCH(p, 32 + 48, &sp, 8);                        FR_EXPECT("header says");
    sp = eh.str_pool_bytes - 1;           ST_PATCH(p, 32 + 48, &sp, 8);                        FR_EXPECT("exceeds str_pool_bytes");
    uint8_t zero = 0;                     ST_PATCH(p, tab + 8, &zero, 1);                      FR_EXPECT("FSST_TABLE symbol length");  /* a symbol length 0 */
    uint8_t nine = 9;                     ST_PATCH(p, tab + 8, &nine, 1);                      FR_EXPECT("FSST_TABLE symbol length");  /* a symbol length 9 */
    uint8_t one = 1;                      ST_PATCH(p, tab + 1, &one, 1);                       FR_EXPECT("FSST_TABLE reserved bytes");  /* reserved byte */
    {   /* a byte past symbol 0's length */
        uint8_t l0 = img[tab + 8];
        if (l0 < 8) { uint8_t x = 0x5A; ST_PATCH(p, tab + 8 + (long)(((uint64_t)ns + 7) & ~(uint64_t)7) + l0, &x, 1); FR_EXPECT("FSST_TABLE symbol padding"); }
    }
    #undef FR_EXPECT
    #undef FR_RESET
    PASS();
}

/* The size limits no small file reaches, on the bound arithmetic (D13). */
static test_result_t test_bound_arith(void) {
    const uint64_t big = (uint64_t)UINT32_MAX + 1;
    uint64_t o_max[2] = { 0, UINT32_MAX };                /* an entry of UINT32_MAX bytes: legal */
    TEST_ASSERT_TRUE(ray_enc_sdict_offsets_ok(o_max, 1, big));
    uint64_t o_over[2] = { 0, big };                      /* UINT32_MAX + 1 bytes */
    TEST_ASSERT_FALSE(ray_enc_sdict_offsets_ok(o_over, 1, big));
    uint64_t o_dec[3] = { 0, 10, 5 };                     /* a decreasing pair */
    TEST_ASSERT_FALSE(ray_enc_sdict_offsets_ok(o_dec, 2, 16));
    uint64_t o_ok[3] = { 0, 5, 10 };
    TEST_ASSERT_TRUE(ray_enc_sdict_offsets_ok(o_ok, 2, 16));
    uint64_t o_one[2] = { 1, 5 };                         /* offsets[0] = 1 */
    TEST_ASSERT_FALSE(ray_enc_sdict_offsets_ok(o_one, 1, 8));
    TEST_ASSERT_FALSE(ray_enc_sdict_offsets_ok(o_ok, 2, 24));   /* area 8 too long */
    TEST_ASSERT_FALSE(ray_enc_sdict_offsets_ok(o_ok, 2, 8));    /* area 8 too short */
    uint64_t o_empty[2] = { 0, 0 };                       /* one empty entry, no area */
    TEST_ASSERT_TRUE(ray_enc_sdict_offsets_ok(o_empty, 1, 0));
    uint64_t t = 0;
    TEST_ASSERT_TRUE(ray_enc_fsst_pool_add(&t, UINT32_MAX, UINT64_MAX));
    TEST_ASSERT_EQ_U(t, UINT32_MAX);
    TEST_ASSERT_FALSE(ray_enc_fsst_pool_add(&t, big, UINT64_MAX));             /* an expansion above 4 GiB */
    TEST_ASSERT_EQ_U(t, UINT32_MAX);                                         /* unchanged */
    t = UINT64_MAX - 5;
    TEST_ASSERT_FALSE(ray_enc_fsst_pool_add(&t, 10, UINT64_MAX));              /* the sum overflows */
    TEST_ASSERT_EQ_U(t, UINT64_MAX - 5);
    t = 100;
    TEST_ASSERT_FALSE(ray_enc_fsst_pool_add(&t, 10, 109));                     /* one byte over cap */
    TEST_ASSERT_TRUE(ray_enc_fsst_pool_add(&t, 10, 110));
    TEST_ASSERT_EQ_U(t, 110);
    PASS();
}

/* ray_encw_test_force_str wins over ray_encw_test_force on STR vectors. */
static test_result_t test_force_str_wins(void) {
    ray_t* v = str_col(3000, gen_urls);                   /* unequal strings: CONST does not apply */
    ray_encw_test_force(RAY_ENC_CONST);
    ray_err_t e1 = ray_col_save_gen(v, STP("/fw1"), 2);
    ray_encw_test_force_str(RAY_ENC_RAW);
    ray_err_t e2 = ray_col_save_gen(v, STP("/fw2"), 2);
    ray_encw_test_force(RAY_ENC_RAW);
    ray_encw_test_force_str(RAY_ENC_FSST);
    ray_err_t e3 = ray_col_save_gen(v, STP("/fw3"), 2);
    ray_encw_test_force_str(-1); ray_encw_test_force(-1);
    ray_release(v);
    TEST_ASSERT_TRUE(e1 != RAY_OK);                                           /* the general force alone: nyi */
    TEST_ASSERT_EQ_I(e2, RAY_OK);
    TEST_ASSERT_EQ_I(st_desc_enc(STP("/fw2"), 0, NULL), RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(e3, RAY_OK);
    TEST_ASSERT_EQ_I(st_desc_enc(STP("/fw3"), 0, NULL), RAY_ENC_FSST);
    TEST_ASSERT_STR_EQ(st_scan(STP("/fw3")), "");
    PASS();
}

const test_entry_t encstr_entries[] = {
    { "encstr/strdict_body",       test_strdict_body,       st_setup, st_teardown },
    { "encstr/fsst_body",          test_fsst_body,          st_setup, st_teardown },
    { "encstr/fsst_train_verdict", test_fsst_train_verdict, st_setup, st_teardown },
    { "encstr/fsst_body_rejects",  test_fsst_body_rejects,  st_setup, st_teardown },
    { "encstr/writer_strdict",     test_writer_strdict,     st_setup, st_teardown },
    { "encstr/writer_fsst",        test_writer_fsst,        st_setup, st_teardown },
    { "encstr/writer_two_entries", test_writer_two_entries, st_setup, st_teardown },
    { "encstr/range_reads_str",    test_range_reads_str,    st_setup, st_teardown },
    { "encstr/side_rejects",       test_side_rejects,       st_setup, st_teardown },
    { "encstr/fsst_rejects",       test_fsst_rejects,       st_setup, st_teardown },
    { "encstr/bound_arith",        test_bound_arith,        st_setup, st_teardown },
    { "encstr/force_str_wins",     test_force_str_wins,     st_setup, st_teardown },
    { NULL, NULL, NULL, NULL },
};
