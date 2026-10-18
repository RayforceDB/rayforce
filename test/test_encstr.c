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
#include "core/pool.h"         /* the first-touch race */
#include "core/timer.h"        /* its barrier deadline */
#include "store/encidx.h"     /* ray_enc_idx_dict_t, ray_enc_dict_validate */
#include "store/colrange.h"
#include "vec/str.h"
#include "table/table.h"
#include "lang/env.h"
#include "lang/format.h"
#include "store/splay.h"
#include "ops/idxop.h"
#include "ops/internal.h"     /* ray_exec_test_str_sel */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ST_TMP "/tmp/rayforce_encstr"        /* always through ray_test_tmp (per-process root) */
/* Buffers a test owns, freed in teardown so an early-returning assertion
 * leaks nothing. */
static ray_enc_buf_t st_buf;
static uint8_t*      st_mem;                 /* malloc'd scratch */
static ray_t*        st_held[8];             /* objects (or errors) a test holds: st_hold */
static ray_col_src_t     st_src;             /* a bound source (st_src_on) */
static bool              st_src_on;
static ray_col_scratch_t st_sc;              /* a range-read scratch (st_sc_on) */
static bool              st_sc_on;
static ray_t* st_hold(ray_t* x) {
    for (int i = 0; i < 8; i++) if (!st_held[i]) { st_held[i] = x; return x; }
    abort();
}
/* x is no longer held (the caller releases it, or holds `by` instead). */
static void st_unhold(ray_t* x, ray_t* by) {
    for (int i = 0; i < 8; i++) if (st_held[i] == x) { st_held[i] = by; return; }
}
/* Before the heap goes: the scratch, the source, then the held objects. */
static void st_release_held(void) {
    if (st_sc_on) { ray_col_scratch_free(&st_sc); st_sc_on = false; }
    if (st_src_on) { ray_col_src_release(&st_src); st_src_on = false; }
    for (int i = 7; i >= 0; i--) {
        if (st_held[i] && RAY_IS_ERR(st_held[i])) ray_error_free(st_held[i]);
        else if (st_held[i]) ray_release(st_held[i]);
        st_held[i] = NULL;
    }
}
static void st_setup(void) {
    ray_heap_init(); (void)ray_sym_init();
    (void)ray_test_rm_rf(ray_test_tmp(ST_TMP)); (void)ray_test_mkdir_p(ray_test_tmp(ST_TMP));
}
static void st_teardown(void) {
    ray_enc_buf_free(&st_buf); st_buf = (ray_enc_buf_t){0};
    free(st_mem); st_mem = NULL;
    st_release_held();
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
/* The outer header's len (bytes 24..31: ray_t.len), the column's rows. */
static int64_t st_rows(const char* path) {
    int64_t n = -1; FILE* f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 24, SEEK_SET); if (fread(&n, 8, 1, f) != 1) n = -1;
    fclose(f); return n;
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
 * The side-entry rules are pinned through the range reads, which validate
 * the same entry as materialization (st_mat). */
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

/* Ruling 3-1: in vector scope a STR_DICT vector copies each distinct pooled
 * entry once and its rows share the offset.  One 40-byte entry on every row
 * of two vectors (forced STR_DICT: selection would take CONST): each range
 * read needs 40 pool bytes, every row points at them, hash32 stays 0. */
static test_result_t test_range_shared_entry(void) {
    const char* p = STP("/se");
    static const char ent[] = "a forty byte entry repeated on every row";   /* 40 bytes */
    TEST_ASSERT_EQ_U(sizeof ent - 1, 40);
    ray_t* v = ray_vec_new(RAY_STR, 2048);
    for (int i = 0; i < 2048; i++) v = ray_str_vec_append(v, ent, 40);
    ray_encw_test_force_str(RAY_ENC_STR_DICT);
    ray_err_t se = ray_col_save_gen(v, p, 2);
    ray_encw_test_force_str(-1);
    ray_release(v);
    TEST_ASSERT_EQ_I(se, RAY_OK);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_STR_DICT);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 1, NULL), RAY_ENC_STR_DICT);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    uint64_t pb0 = 0, pb1 = 0;
    ray_t* e0 = ray_enccol_vec_pool_bytes(ray_enccol_of(c), 0, &pb0);
    ray_t* e1 = ray_enccol_vec_pool_bytes(ray_enccol_of(c), 1, &pb1);
    bool sized = !e0 && !e1 && pb0 == 40 && pb1 == 40;
    if (e0) ray_error_free(e0);
    if (e1) ray_error_free(e1);
    ray_col_src_t src; ray_t* be = ray_col_src_bind(c, &src);
    if (be) { ray_error_free(be); ray_release(c); FAIL("bind"); }
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    bool same = true, shared = true, nohash = true, cap40 = true; int64_t nread = 0;
    for (int64_t r = 0; r < 2048; ) {
        ray_col_range_t rv;
        ray_t* err = ray_col_range(&src, r, 2048 - r, &sc, &rv);
        if (err) { ray_error_free(err); same = false; break; }
        cap40 = cap40 && sc.pool_cap == 40;
        const ray_str_t* d = (const ray_str_t*)rv.data;
        for (int64_t k = 0; k < rv.n; k++) {
            if (d[k].len != 40 || memcmp(ray_str_t_ptr(&d[k], rv.str_pool), ent, 40) != 0) same = false;
            if (d[k].pool_off != d[0].pool_off) shared = false;
            if (d[k].hash32 != 0) nohash = false;
        }
        r += rv.n; nread += rv.n;
    }
    ray_col_scratch_free(&sc); ray_col_src_release(&src); ray_release(c);
    TEST_ASSERT_TRUE(sized);                              /* the entry's bytes once per vector */
    TEST_ASSERT_EQ_I(nread, 2048);
    TEST_ASSERT_TRUE(same);
    TEST_ASSERT_TRUE(shared);
    TEST_ASSERT_TRUE(nohash);
    TEST_ASSERT_TRUE(cap40);                              /* the scratch pool is sized from them */
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
    TEST_ASSERT_FMT(strcmp(st_mat(p), "") == 0, "materialize: %s", st_msg);
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
        TEST_ASSERT_EQ_U(end & 7, 1);                     /* 10 entries of 22 bytes, 27 of 23: 841, padding follows */
        uint8_t x = 1; ST_PATCH(p, bytes + (long)end, &x, 1); ST_SCAN_CORRUPT(p, "STR_DICT padding");
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
    /* the STR_DICT vector-scope sizing (Ruling 3-1): distinct entries
     * added under the UINT32_MAX cap; one past it is rejected before any
     * scratch pool is allocated */
    t = (uint64_t)UINT32_MAX - 40;
    TEST_ASSERT_TRUE(ray_enc_fsst_pool_add(&t, 40, UINT32_MAX));
    TEST_ASSERT_EQ_U(t, UINT32_MAX);
    TEST_ASSERT_FALSE(ray_enc_fsst_pool_add(&t, 13, UINT32_MAX));
    t = 3ull << 30;                                       /* entries of 3 GiB and 2 GiB: over */
    TEST_ASSERT_FALSE(ray_enc_fsst_pool_add(&t, 2ull << 30, UINT32_MAX));
    TEST_ASSERT_EQ_U(t, 3ull << 30);
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

/* ---- Task 2b: STR code bounds reach no ordered-statistics consumer ---- */
static ray_runtime_t* st_rt;
static void st_q_setup(void) {
    st_rt = ray_runtime_create(0, NULL);
    (void)ray_test_rm_rf(ray_test_tmp(ST_TMP)); (void)ray_test_mkdir_p(ray_test_tmp(ST_TMP));
}
static void st_q_teardown(void) {
    free(st_mem); st_mem = NULL;
    st_release_held();
    ray_test_hooks_off(); ray_encw_test_force_str(-1); ray_encw_test_force(-1);
    (void)ray_test_rm_rf(ray_test_tmp(ST_TMP)); ray_runtime_destroy(st_rt);
}
/* The query's formatted result; NULL when it fails (message in st_msg). */
static ray_t* st_q_fmt(const char* q) {
    ray_t* r = ray_eval_str(ray_test_tmp(q));
    if (!r || RAY_IS_ERR(r)) {
        snprintf(st_msg, sizeof st_msg, "%s", ray_error_msg() ? ray_error_msg() : "");
        if (r) ray_error_free(r);
        return NULL;
    }
    ray_t* f = ray_fmt(r, 0); ray_release(r);
    return f;
}
/* Evaluate q for its effect: false when it fails (message in st_msg). */
static bool st_q_run(const char* q) {
    st_msg[0] = 0;
    ray_t* r = ray_eval_str(ray_test_tmp(q));
    if (r && RAY_IS_ERR(r)) {
        snprintf(st_msg, sizeof st_msg, "%s", ray_error_msg() ? ray_error_msg() : "");
        ray_error_free(r);
        return false;
    }
    if (r) ray_release(r);
    return true;
}
#define ST_Q_OK(q) TEST_ASSERT_FMT(st_q_run(q), "%s: %s", (q), st_msg)
/* The query's result is the boolean true. */
static bool st_q_true(const char* q) {
    ray_t* r = ray_eval_str(ray_test_tmp(q));
    bool ok = r && !RAY_IS_ERR(r) && r->type == -RAY_BOOL && r->b8;
    if (r && RAY_IS_ERR(r)) { snprintf(st_msg, sizeof st_msg, "%s", ray_error_msg() ? ray_error_msg() : ""); ray_error_free(r); }
    else if (r) ray_release(r);
    return ok;
}
#define ST_Q_TRUE(q) TEST_ASSERT_FMT(st_q_true(q), "%s: %s", (q), st_msg)
/* The live generation's file of column `col` of the table saved at
 * ST_TMP `rel` (#649 / #713: the table directory holds generations). */
static bool st_live(const char* rel, const char* col, char* out, size_t n) {
    char tmp[256], dir[512];
    snprintf(tmp, sizeof tmp, "%s%s", ST_TMP, rel);
    if (ray_splay_resolve_dir(ray_test_tmp(tmp), dir, sizeof dir) != RAY_OK) return false;
    return (size_t)snprintf(out, n, "%s/%s", dir, col) < n;
}

/* The chokepoint over a three-string column (insertion order z, a, m:
 * codes 0, 1, 2 are not value order), its strings nm[0..2]. */
static test_result_t st_chokepoint(const char* const nm[3]) {
    ray_t* v = ray_vec_new(RAY_STR, 70000);
    for (int64_t i = 0; i < 70000; i++) { const char* x = nm[i % 3]; v = ray_str_vec_append(v, x, strlen(x)); }
    ray_t* tb = ray_table_new(1);
    tb = ray_table_add_col(tb, ray_sym_intern("s", 1), v); ray_release(v);
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("W", 1), tb), RAY_OK);
    ray_release(tb);
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/g0/\" W {compress: false})");
    ray_encw_test_force_str(RAY_ENC_STR_DICT);
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/g2/\" W {compress: true})");
    ray_encw_test_force_str(-1);

    /* the accessors, on the generation-2 column */
    ST_Q_OK("(set T (.db.splayed.get \"" ST_TMP "/g2/\"))");
    ray_t* t = st_hold(ray_eval_str("T"));
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* encv = ray_table_get_col_enc(t, ray_sym_intern("s", 1));
    TEST_ASSERT_TRUE(encv != NULL);
    ray_enccol_t* e = ray_enccol_of(encv);
    ray_t* be = ray_col_src_bind(encv, &st_src);
    if (be) st_hold(be);
    TEST_ASSERT_TRUE(be == NULL);
    st_src_on = true;
    ray_enc_chunk_stat_t cs;
    TEST_ASSERT_TRUE(ray_col_src_chunk_stat(&st_src, 0, &cs));
    TEST_ASSERT_FALSE(cs.has_minmax);
    TEST_ASSERT_TRUE(cs.min == 0 && cs.max == 0);                         /* no codes behind has_minmax */
    TEST_ASSERT_EQ_I(cs.rows, 65536);
    int64_t mn = 7, mx = 7; uint32_t nn = 0;
    TEST_ASSERT_FALSE(ray_col_src_vec_stat(&st_src, 0, &mn, &mx, &nn));
    ray_enc_col_totals_t tot; ray_enccol_dir_totals(e, &tot);
    TEST_ASSERT_TRUE(tot.mm_partial);
    TEST_ASSERT_TRUE(tot.min_i == INT64_MAX && tot.max_i == INT64_MIN);
    ray_t* err = NULL; int64_t lo = -1, hi = -1;
    TEST_ASSERT_TRUE(ray_enccol_code_bounds(e, 0, -1, &lo, &hi, &err));
    TEST_ASSERT_TRUE(err == NULL && lo == 0 && hi == 2);
    lo = hi = -1;
    TEST_ASSERT_TRUE(ray_enccol_code_bounds(e, 0, 0, &lo, &hi, &err));
    TEST_ASSERT_TRUE(err == NULL && lo == 0 && hi == 2);
    TEST_ASSERT_FALSE(ray_enccol_code_bounds(e, 5, -1, &lo, &hi, &err));    /* past the last chunk */
    TEST_ASSERT_NULL(err);                                                  /* none, not an error */

    /* the consumers answer as generation 0 does */
    static const char* const qs[] = {
        "(min (at T 's))", "(max (at T 's))",
        "(count (select {from: T where: (< s \"m\")}))",
        "(select {s: s from: T asc: s take: 3})",
        "(select {s: s from: T desc: s take: 3})",
    };
    for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) {
        ray_t* f[2] = { NULL, NULL };
        for (int g = 0; g < 2; g++) {
            ST_Q_OK(g ? "(set T (.db.splayed.get \"" ST_TMP "/g2/\"))" : "(set T (.db.splayed.get \"" ST_TMP "/g0/\"))");
            f[g] = st_q_fmt(qs[i]);
            if (f[g]) st_hold(f[g]);
            TEST_ASSERT_FMT(f[g] != NULL, "%s gen %d: %s", qs[i], g ? 2 : 0, st_msg);
        }
        bool same = ray_str_len(f[0]) == ray_str_len(f[1]) && memcmp(ray_str_ptr(f[0]), ray_str_ptr(f[1]), ray_str_len(f[0])) == 0;
        TEST_ASSERT_FMT(same, "%s differs from generation 0", qs[i]);
        st_unhold(f[0], NULL); st_unhold(f[1], NULL);
        ray_release(f[0]); ray_release(f[1]);
    }
    PASS();
}

/* Inline strings: the materializing queries need no pool. */
static test_result_t test_stats_chokepoint(void) {
    static const char* const nm[3] = { "zulu", "alpha", "mike" };
    return st_chokepoint(nm);
}

/* The same over pooled strings (over 12 bytes), which materialize through
 * the dictionary-first pool. */
static test_result_t test_stats_chokepoint_pooled(void) {
    static const char* const nm[3] = { "zulu zulu zulu zulu", "alpha alpha alpha", "mike mike mike mike" };
    return st_chokepoint(nm);
}

/* Carry from the Task 3 review: in a STR_DICT vector's code-to-offset
 * table, codes that are multiples of 2048 spread (the high bits of the
 * product index the slot).  One distinct 20-byte entry per row for 32768
 * rows, then a vector whose rows cycle through the codes k * 2048. */
static test_result_t test_dict_slot_spread(void) {
    const char* p = STP("/sp");
    ray_t* v = st_hold(ray_vec_new(RAY_STR, 34816));
    char b[64];
    for (int64_t i = 0; i < 34816; i++) {
        int64_t k = i < 32768 ? i : ((i - 32768) % 16) * 2048;
        snprintf(b, sizeof b, "entry-%012lld-xx", (long long)k);
        ray_t* w = ray_str_vec_append(v, b, strlen(b));
        st_unhold(v, w); v = w;
    }
    ray_encw_test_force_str(RAY_ENC_STR_DICT);
    ray_err_t se = ray_col_save_gen(v, p, 2);
    ray_encw_test_force_str(-1);
    TEST_ASSERT_EQ_I(se, RAY_OK);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 32, NULL), RAY_ENC_STR_DICT);
    ray_t* c = st_hold(ray_col_mmap(p));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_t* be = ray_col_src_bind(c, &st_src);
    if (be) st_hold(be);
    TEST_ASSERT_NULL(be);
    st_src_on = true;
    ray_col_scratch_init(&st_sc); st_sc_on = true;
    bool same = true; int64_t bad = -1;
    for (int64_t r = 0; r < 34816 && same; ) {
        ray_col_range_t rv;
        ray_t* err = ray_col_range(&st_src, r, 34816 - r, &st_sc, &rv);
        if (err) { ray_error_free(err); same = false; bad = r; break; }
        for (int64_t k = 0; k < rv.n && same; k++) {
            size_t lb; const char* want = ray_str_vec_get(v, r + k, &lb);
            const ray_str_t* d = (const ray_str_t*)rv.data + k;
            if (d->len != lb || memcmp(ray_str_t_ptr(d, rv.str_pool), want, lb) != 0) { same = false; bad = r + k; }
        }
        r += rv.n;
    }
    TEST_ASSERT_FMT(same, "row %lld differs", (long long)bad);
    PASS();
}

/* ---- Task 4: the dictionary-first pool; the DICT index (persisted since P6b) ---- */

/* Two STR vectors hold the same strings, row for row. */
static bool st_same(ray_t* a, ray_t* b) {
    if (!a || !b || RAY_IS_ERR(a) || RAY_IS_ERR(b) || a->type != RAY_STR || b->type != RAY_STR || a->len != b->len)
        return false;
    for (int64_t i = 0; i < a->len; i++) {
        size_t la, lb;
        const char* pa = ray_str_vec_get(a, i, &la);
        const char* pb = ray_str_vec_get(b, i, &lb);
        if (la != lb || (la && memcmp(pa, pb, la) != 0)) return false;
    }
    return true;
}

/* Review focus 2: the decoded pool order, element equality, hash32.  Chunk
 * 0 is a STR_DICT chunk; chunk 1 holds an FSST vector and a CONST vector. */
static test_result_t test_pool_order(void) {
    const char* p = STP("/po");
    int64_t n = 65536 + 2048;
    ray_t* v = st_hold(ray_vec_new(RAY_STR, n));
    char b[256];
    for (int64_t i = 0; i < n; i++) {
        const char* s = i < 65536 ? gen_lowcard(i, b) : i < 65536 + 1024 ? gen_urls(i, b) : "a constant long string value";
        ray_t* w = ray_str_vec_append(v, s, strlen(s));
        st_unhold(v, w); v = w;
    }
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_STR_DICT);
    int e64 = st_desc_enc(p, 64, NULL);
    TEST_ASSERT_TRUE(e64 == RAY_ENC_FSST || e64 == RAY_ENC_RAW);      /* FSST trained on chunk 0 */
    TEST_ASSERT_EQ_I(st_desc_enc(p, 65, NULL), RAY_ENC_CONST);
    ray_t* c = st_hold(ray_col_mmap(p));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_t* m = ray_enccol_materialize(c);                  /* borrowed: the column's cache */
    if (m && RAY_IS_ERR(m)) st_hold(m);
    TEST_ASSERT_FMT(m && !RAY_IS_ERR(m), "materialize: %s", ray_error_msg() ? ray_error_msg() : "");
    TEST_ASSERT_TRUE(st_same(m, v));
    TEST_ASSERT_FALSE(ray_index_has(m));                   /* a bare file records no DICT (P6b) */
    const char* pool = (const char*)ray_data(m->str_pool);
    TEST_ASSERT_EQ_U((uint64_t)m->str_pool->len, ray_enccol_of(c)->hdr.str_pool_bytes);
    /* the dictionary region: entries over 12 bytes, once, in code order */
    const ray_enc_sdict_t* sd;
    TEST_ASSERT_NULL(ray_enccol_strdict(ray_enccol_of(c), &sd));
    TEST_ASSERT_NOT_NULL(sd);
    uint64_t at = 0;
    for (uint32_t k = 0; k < sd->n; k++) {
        const char* ep; uint32_t el; ray_enc_sdict_entry(sd, k, &ep, &el);
        if (el <= RAY_STR_INLINE_MAX) continue;
        TEST_ASSERT_TRUE(memcmp(pool + at, ep, el) == 0);
        at += el;
    }
    TEST_ASSERT_EQ_U(at, sd->pooled);
    TEST_ASSERT_TRUE(sd->pooled > 0);
    const ray_str_t* s0 = (const ray_str_t*)ray_data(m);
    for (int64_t i = 0; i < 65536; i++)                   /* STR_DICT rows point into the region */
        if (!ray_str_is_inline(&s0[i])) TEST_ASSERT_TRUE(s0[i].pool_off + s0[i].len <= sd->pooled);
    /* the first pooled row of a non-dictionary vector starts right after it */
    int64_t r1 = 65536;
    while (r1 < m->len && ray_str_is_inline(&s0[r1])) r1++;
    TEST_ASSERT_TRUE(r1 < m->len);
    TEST_ASSERT_EQ_U(s0[r1].pool_off, sd->pooled);
    for (int64_t i = 0; i < m->len; i++) {
        const ray_str_t* s = &s0[i];
        if (ray_str_is_inline(s)) continue;
        uint32_t h = (uint32_t)ray_str_t_hash(s, pool); if (!h) h = 1;
        TEST_ASSERT_EQ_U(s->hash32, h);
    }
    PASS();
}

/* Spec "FSST validation" scopes: a malformed first FSST vector does not
 * stop a range read of the second; materialization fails before writing. */
static test_result_t test_fsst_scopes(void) {
    const char* p = STP("/fx");
    ray_t* v = st_hold(str_col(2048, gen_urls));
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    ray_enc_desc_t d; TEST_ASSERT_EQ_I(st_desc_enc(p, 0, &d), RAY_ENC_FSST);
    const long payload = 96 + 64 + (long)d.payload_off;  /* one chunk: its descriptors follow the directory */
    uint32_t lb; ST_READ(p, payload + 4, &lb, 4);         /* the lengths child's body bytes */
    uint64_t total; ST_READ(p, payload + 8 + lb, &total, 8);
    total += 8;                                           /* total_bytes no longer the sum of lengths */
    ST_PATCH(p, payload + 8 + lb, &total, 8);
    ray_t* c = st_hold(ray_col_mmap(p));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_t* be = ray_col_src_bind(c, &st_src);
    if (be) st_hold(be);
    TEST_ASSERT_NULL(be);
    st_src_on = true;
    ray_col_scratch_init(&st_sc); st_sc_on = true;
    ray_col_range_t rv;
    ray_t* e1 = ray_col_range(&st_src, 1024, 1024, &st_sc, &rv);       /* vector 1: fine */
    if (e1) st_hold(e1);
    TEST_ASSERT_NULL(e1);
    TEST_ASSERT_EQ_I(rv.n, 1024);
    ray_t* e = st_hold(ray_col_range(&st_src, 0, 1024, &st_sc, &rv));
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0);
    uint64_t allocs0 = ray_enccol_test_str_pool_allocs();
    ray_t* m = ray_enccol_materialize(c);
    if (m && RAY_IS_ERR(m)) st_hold(m);
    TEST_ASSERT_TRUE(m && RAY_IS_ERR(m) && strcmp(ray_err_code(m), "corrupt") == 0);
    TEST_ASSERT_EQ_U(ray_enccol_test_str_pool_allocs(), allocs0);       /* rejected before the pool */
    PASS();
}

/* The persisted DICT equals generation 0's before materialization (the
 * region through the encoded object) and after it (the attached root,
 * whose codes vector is the code tier's), with nothing decoded for it. */
static test_result_t test_dict_equals_gen0(void) {
    ST_Q_OK("(set S (as 'STR (map (fn [j] (format \"value % of the domain\" (% (* j 7919) 300))) (til 70000))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/d0/\" (table [s] (list S)) {compress: false})");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/d2/\" (table [s] (list S)) {compress: true})");
    ray_t* t0 = st_hold(ray_eval_str(ray_test_tmp("(.db.splayed.get \"" ST_TMP "/d0/\")")));
    ray_t* t2 = st_hold(ray_eval_str(ray_test_tmp("(.db.splayed.get \"" ST_TMP "/d2/\")")));
    TEST_ASSERT_TRUE(t0 && !RAY_IS_ERR(t0) && t2 && !RAY_IS_ERR(t2));
    int64_t sym = ray_sym_intern("s", 1);
    ray_t* a = ray_table_col(t0, sym);
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a));
    TEST_ASSERT_EQ_I(ray_index_kind(a), RAY_IDX_DICT);
    ray_index_t* x = ray_index_payload(a->index);
    ray_col_meta_t mt;
    TEST_ASSERT_TRUE(ray_table_col_meta(t2, sym, &mt));
    TEST_ASSERT_EQ_I(mt.index_kind, RAY_IDX_DICT);                 /* recorded: metadata, no decode */
    ray_enccol_t* e = ray_enccol_of(ray_table_get_col_enc(t2, sym));
    TEST_ASSERT_NOT_NULL(e);
    ray_enc_counters_reset();
    const ray_enc_idict_t* d = NULL;
    TEST_ASSERT_NULL(ray_enccol_idict(e, &d));
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQ_I(d->n_distinct, x->u.dict.n_distinct);
    TEST_ASSERT_TRUE(memcmp(d->first_occ, ray_data(x->u.dict.first_occ), (size_t)d->n_distinct * 4) == 0);
    for (int64_t r = 0; r < 70000; r += 1000) {                     /* by range, vector scope */
        const int32_t* cd = NULL;
        int64_t n = 70000 - r < 1000 ? 70000 - r : 1000;
        TEST_ASSERT_NULL(ray_enccol_idict_codes(e, r, n, &cd));
        TEST_ASSERT_TRUE(memcmp(cd, (const int32_t*)ray_data(x->u.dict.codes) + r, (size_t)n * 4) == 0);
    }
    ray_t* cv = NULL;
    TEST_ASSERT_NULL(ray_enccol_idict_codes_vec(e, &cv));
    TEST_ASSERT_TRUE(cv->type == RAY_I32 && cv->len == 70000);
    TEST_ASSERT_TRUE(memcmp(ray_data(cv), ray_data(x->u.dict.codes), 70000 * 4) == 0);
    int64_t code = -9;
    char b[64]; int bn = snprintf(b, sizeof b, "value %d of the domain", 7919 % 300);
    TEST_ASSERT_NULL(ray_enccol_idict_code(e, b, (size_t)bn, &code));
    TEST_ASSERT_EQ_I(code, 1);                                       /* row 1's string: code 1 */
    TEST_ASSERT_NULL(ray_enccol_idict_code(e, "absent", 6, &code));
    TEST_ASSERT_EQ_I(code, -1);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
    /* after materialization: the attached root, sharing the code-tier vector */
    ray_t* m = ray_table_col(t2, sym);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_DICT);
    ray_index_t* y = ray_index_payload(m->index);
    TEST_ASSERT_TRUE(y->u.dict.codes == cv);
    TEST_ASSERT_EQ_I(y->u.dict.n_distinct, x->u.dict.n_distinct);
    TEST_ASSERT_TRUE(memcmp(ray_data(y->u.dict.first_occ), ray_data(x->u.dict.first_occ), 300 * 4) == 0);
    TEST_ASSERT_TRUE(st_same(m, a));
    PASS();
}

/* HASH + all STR_DICT: HASH, no DICT.  The persisted DICT (P6b, recorded
 * by every save below) + UNIQUE from a heap source
 * and from an encoded passenger source (a generation-2 store resaved): UNIQUE
 * known before materialization (header), .attr.get equal to the source's
 * after it.  A generation-0 passenger source (the generation-0 store
 * resaved): DICT, and UNIQUE as generation 0 keeps it, which is not at all
 * for STR (the generation-0 save drops a STR column's marker-only UNIQUE, a
 * pre-existing dev behaviour, #864): .attr.get equal to
 * generation 0's.  A short column (< 65536 rows) answers no index without
 * decoding. */
static test_result_t test_dict_variants(void) {
    /* low cardinality, long strings: every chunk is a STR_DICT chunk */
    ST_Q_OK("(set S (as 'STR (map (fn [j] (format \"value % of the domain\" (% (* j 7919) 300))) (til 70000))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/h2/\" (table [s] (list (.idx.hash S))) {compress: true})");
    char fh[600]; TEST_ASSERT_TRUE(st_live("/h2", "s", fh, sizeof fh));
    for (int c = 0; c < 2; c++) TEST_ASSERT_FALSE(st_dirent(fh, c).flags & RAY_ENC_STAT_ABSENT);
    ray_t* t = st_hold(ray_eval_str(ray_test_tmp("(.db.splayed.get \"" ST_TMP "/h2/\")")));
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* col = ray_table_col(t, ray_sym_intern("s", 1));
    TEST_ASSERT_TRUE(col && !RAY_IS_ERR(col));
    TEST_ASSERT_EQ_I(ray_index_kind(col), RAY_IDX_HASH);                 /* HASH, no DICT */
    st_unhold(t, NULL); ray_release(t);
    /* UNIQUE: distinct strings forced to STR_DICT (selection would pick FSST) */
    ST_Q_OK("(set U (.attr.set 'unique (as 'STR (map (fn [j] (format \"unique key %\" j)) (til 70000)))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/u0/\" (table [s] (list U)) {compress: false})");
    ray_encw_test_force_str(RAY_ENC_STR_DICT);
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/u2/\" (table [s] (list U)) {compress: true})");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/uq/\" (.db.splayed.get \"" ST_TMP "/u2/\") {compress: true})");   /* encoded passenger */
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/up/\" (.db.splayed.get \"" ST_TMP "/u0/\") {compress: true})");   /* generation-0 passenger */
    ray_encw_test_force_str(-1);
    static const struct { const char* dir; bool unique; const char* ref; } vs[3] = {
        { "/u2", true,  "U" },
        { "/uq", true,  "U" },
        { "/up", false, "(at (.db.splayed.get \"" ST_TMP "/u0/\") 's)" },
    };
    for (int d = 0; d < 3; d++) {
        char fl[600]; TEST_ASSERT_TRUE(st_live(vs[d].dir, "s", fl, sizeof fl));
        for (int c = 0; c < 2; c++) TEST_ASSERT_FALSE(st_dirent(fl, c).flags & RAY_ENC_STAT_ABSENT);
        char q[300]; snprintf(q, sizeof q, "(.db.splayed.get \"%s%s/\")", ST_TMP, vs[d].dir);
        t = st_hold(ray_eval_str(ray_test_tmp(q)));
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_col_meta_t m;
        ray_enc_counters_reset();
        TEST_ASSERT_TRUE(ray_table_col_meta(t, ray_sym_intern("s", 1), &m));
        TEST_ASSERT_FMT(m.unique == vs[d].unique, "%s: unique %d", vs[d].dir, (int)m.unique);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        col = ray_table_col(t, ray_sym_intern("s", 1));
        TEST_ASSERT_TRUE(col && !RAY_IS_ERR(col));
        TEST_ASSERT_EQ_I(ray_index_kind(col), RAY_IDX_DICT);
        TEST_ASSERT_EQ_I(!!(ray_index_payload(col->index)->markers & RAY_MARK_UNIQUE), vs[d].unique);
        TEST_ASSERT_EQ_I(ray_index_payload(col->index)->u.dict.n_distinct, 70000);
        st_unhold(t, NULL); ray_release(t);
        char qa[700];                                     /* .attr.get equal to the source's */
        snprintf(qa, sizeof qa, "(== (count (.attr.get %s)) (count (.attr.get (at (.db.splayed.get \"%s%s/\") 's))))",
                 vs[d].ref, ST_TMP, vs[d].dir);
        ST_Q_TRUE(qa);
        snprintf(qa, sizeof qa, "(all (== (.attr.get %s) (.attr.get (at (.db.splayed.get \"%s%s/\") 's))))",
                 vs[d].ref, ST_TMP, vs[d].dir);
        ST_Q_TRUE(qa);
    }
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/sh/\" (table [s] (list (take [\"a\" \"b\"] 5000))) {compress: true})");
    t = st_hold(ray_eval_str(ray_test_tmp("(.db.splayed.get \"" ST_TMP "/sh/\")")));
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    TEST_ASSERT_NOT_NULL(ray_table_get_col_enc(t, ray_sym_intern("s", 1)));
    ray_enc_counters_reset();
    ray_t* ix = (ray_t*)1;
    ray_t* ie = ray_table_col_index(t, ray_sym_intern("s", 1), &ix);
    if (ie) st_hold(ie);
    TEST_ASSERT_NULL(ie);
    TEST_ASSERT_NULL(ix);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    PASS();
}

/* Vector scope through a query: a filter whose survivors are all in vector
 * 1 gathers only vector 1's slices, so a malformed vector 0 does not fail it
 * and nothing is materialized; a query reaching vector 0 fails "corrupt". */
static test_result_t test_fsst_scopes_query(void) {
    ST_Q_OK("(set S (as 'STR (map (fn [j] (format \"https://www.example.com/path/%/item?id=%&ref=search\" (% j 5000) (% (* j 7919) 100003))) (til 2048))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/fq/\" (table [s v] (list S (til 2048))) {compress: true})");
    char p[600]; TEST_ASSERT_TRUE(st_live("/fq", "s", p, sizeof p));
    ray_enc_desc_t d; TEST_ASSERT_EQ_I(st_desc_enc(p, 0, &d), RAY_ENC_FSST);
    const long payload = 96 + 64 + (long)d.payload_off;
    uint32_t lb; ST_READ(p, payload + 4, &lb, 4);
    uint64_t total; ST_READ(p, payload + 8 + lb, &total, 8);
    total += 8;
    ST_PATCH(p, payload + 8 + lb, &total, 8);
    ST_Q_OK("(set T (.db.splayed.get \"" ST_TMP "/fq/\"))");
    ray_enc_counters_reset();
    ST_Q_TRUE("(all (== (at (select {s: s from: T where: (>= v 1024)}) 's) (at S (+ 1024 (til 1024)))))");
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_t* r = st_hold(ray_eval_str("(at (select {s: s from: T where: (< v 1024)}) 's)"));
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r) && strcmp(ray_err_code(r), "corrupt") == 0);
    PASS();
}

/* ---- Task 4b: forged STR_DICT codes (helpers shared by later tests) ---- */

/* Vector v of the forged-file column holds one string throughout: entry
 * v % 2 for every vector but the last, which holds entry 2 alone. */
#define SF_ROWS 70000
static const char* const g_sf_s[3] = {
    "forged entry zero, a long string", "forged entry one, a long string", "forged entry two, a long string" };
static int sf_entry(int64_t v) { return v == ray_enc_nvecs(SF_ROWS) - 1 ? 2 : (int)(v & 1); }

/* Code `code` into vector `vec`'s STR_DICT code child, which is CONST or a
 * width-0 FOR_BP (its value, or base, the 8 bytes after the child header),
 * with the descriptor's code bounds (min / max at 32 / 40). */
static bool sf_set_code(const char* p, int64_t vec, int64_t code) {
    ray_enc_desc_t d;
    if (st_desc_enc(p, vec, &d) != RAY_ENC_STR_DICT) return false;
    ray_enc_dirent_t de = st_dirent(p, vec / 64);
    const long body = (long)(de.offset + d.payload_off);
    uint8_t ch[2];
    if (!st_read(p, body, ch, 2)) return false;
    if (!(ch[0] == RAY_ENC_CONST || (ch[0] == RAY_ENC_FOR_BP && ch[1] == 0))) return false;
    const long desc = (long)(de.offset + (uint64_t)(vec % 64) * 48);
    return st_patch(p, body + 8, &code, 8) && st_patch(p, desc + 32, &code, 8) && st_patch(p, desc + 40, &code, 8);
}

/* "" rows: an ordinary entry; the code of "" resolves; equality with the
 * generation-0 DICT holds. */
static test_result_t test_dict_inband_nulls(void) {
    ST_Q_OK("(set S (as 'STR (map (fn [j] (if (== 0 (% j 5)) \"\" (format \"value % of the domain\" (% j 300)))) (til 70000))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/n0/\" (table [s] (list S)) {compress: false})");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/n2/\" (table [s] (list S)) {compress: true})");
    ray_t* t0 = st_hold(ray_eval_str(ray_test_tmp("(.db.splayed.get \"" ST_TMP "/n0/\")")));
    ray_t* t2 = st_hold(ray_eval_str(ray_test_tmp("(.db.splayed.get \"" ST_TMP "/n2/\")")));
    TEST_ASSERT_TRUE(t0 && !RAY_IS_ERR(t0) && t2 && !RAY_IS_ERR(t2));
    int64_t sym = ray_sym_intern("s", 1);
    ray_enccol_t* e = ray_enccol_of(ray_table_get_col_enc(t2, sym));
    TEST_ASSERT_NOT_NULL(e);
    int64_t code = -9;
    TEST_ASSERT_NULL(ray_enccol_idict_code(e, "", 0, &code));
    TEST_ASSERT_EQ_I(code, 0);                                       /* row 0 is "" */
    const ray_enc_idict_t* d = NULL;
    TEST_ASSERT_NULL(ray_enccol_idict(e, &d));
    TEST_ASSERT_TRUE(d && d->ent->null_code == 0);
    ray_t* a = ray_table_col(t0, sym);
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a));
    ray_index_t* x = ray_index_payload(a->index);
    TEST_ASSERT_EQ_I(d->n_distinct, x->u.dict.n_distinct);
    ray_t* cv = NULL;
    TEST_ASSERT_NULL(ray_enccol_idict_codes_vec(e, &cv));
    TEST_ASSERT_TRUE(memcmp(ray_data(cv), ray_data(x->u.dict.codes), 70000 * 4) == 0);
    TEST_ASSERT_TRUE(memcmp(d->first_occ, ray_data(x->u.dict.first_occ), (size_t)d->n_distinct * 4) == 0);
    PASS();
}

/* OOM in each tier: oom, nothing cached, the retry succeeds; allocator
 * accounting back at its start after each failure. */
static test_result_t test_dict_oom_retry(void) {
    ST_Q_OK("(set S (as 'STR (map (fn [j] (format \"value % of the domain\" (% (* j 7919) 300))) (til 70000))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/o/\" (table [s] (list S)) {compress: true})");
    char p[600]; TEST_ASSERT_TRUE(st_live("/o", "s", p, sizeof p));
    ray_t* c = st_hold(ray_col_mmap(p));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_t* e = ray_enccol_of(c);
    const ray_enc_idict_t* d = NULL;
    ray_mem_stats_t m0, m1;
    ray_mem_stats(&m0);
    ray_enccol_test_fail_alloc(1);
    ray_t* e1 = ray_enccol_idict(e, &d);
    ray_enccol_test_fail_alloc(0);
    TEST_ASSERT_TRUE(e1 && strcmp(ray_err_code(e1), "oom") == 0); ray_error_free(e1);
    TEST_ASSERT_NULL(d);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);
    TEST_ASSERT_NULL(ray_enccol_idict(e, &d));
    TEST_ASSERT_NOT_NULL(d);
    ray_t* cv = NULL;
    ray_mem_stats(&m0);
    ray_enccol_test_fail_alloc(1);
    ray_t* e2 = ray_enccol_idict_codes_vec(e, &cv);
    ray_enccol_test_fail_alloc(0);
    TEST_ASSERT_TRUE(e2 && strcmp(ray_err_code(e2), "oom") == 0); ray_error_free(e2);
    TEST_ASSERT_NULL(cv);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);
    TEST_ASSERT_NULL(ray_enccol_idict_codes_vec(e, &cv));
    TEST_ASSERT_TRUE(cv && cv->len == 70000);
    /* the warm answer: rows, codes, first occurrences */
    ray_t* warm = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(warm && !RAY_IS_ERR(warm) && ray_index_kind(warm) == RAY_IDX_DICT);
    TEST_ASSERT_TRUE(ray_index_payload(warm->index)->u.dict.codes == cv);
    /* every injection point of a materialization on a fresh column: oom,
     * nothing leaked once the column goes, and the retry equals the warm
     * answer; the DICT's own points are among them (P6's sweep) */
    bool done = false;
    int dict_tier = 0, dict_root = 0, dict_gate = 0;
    for (int nth = 1; nth <= 64 && !done; nth++) {
        ray_mem_stats(&m0);
        ray_t* fc = ray_col_mmap(p);
        TEST_ASSERT_TRUE(fc && !RAY_IS_ERR(fc));
        ray_error_clear();
        ray_enccol_test_fail_alloc(nth);
        ray_t* r = ray_enccol_materialize(fc);
        ray_enccol_test_fail_alloc(0);
        if (r && RAY_IS_ERR(r)) {
            const char* msg = ray_error_msg() ? ray_error_msg() : "";
            const bool oom = strcmp(ray_err_code(r), "oom") == 0;
            if (strstr(msg, "index parse")) dict_gate++;
            else if (strstr(msg, "DICT index")) dict_root++;
            else if (strstr(msg, "DICT") && !strstr(msg, "STR_DICT")) dict_tier++;   /* not the side entry */
            ray_error_free(r);
            if (!oom) ray_release(fc);
            TEST_ASSERT_FMT(oom, "nth %d: not oom", nth);
            r = ray_enccol_materialize(fc);                 /* the retry */
        } else {
            done = true;
        }
        bool same = r && !RAY_IS_ERR(r) && st_same(r, warm) && ray_index_kind(r) == RAY_IDX_DICT;
        if (same) {
            const ray_index_t* a = ray_index_payload(r->index);
            const ray_index_t* b = ray_index_payload(warm->index);
            same = a->u.dict.n_distinct == b->u.dict.n_distinct &&
                   memcmp(ray_data(a->u.dict.codes), ray_data(b->u.dict.codes), 70000 * 4) == 0 &&
                   memcmp(ray_data(a->u.dict.first_occ), ray_data(b->u.dict.first_occ), 300 * 4) == 0;
        }
        if (r && RAY_IS_ERR(r)) ray_error_free(r);
        ray_release(fc);
        ray_mem_stats(&m1);
        TEST_ASSERT_FMT(same, "nth %d: retry differs from the warm answer", nth);
        TEST_ASSERT_FMT(m1.bytes_allocated == m0.bytes_allocated, "nth %d: %llu vs %llu", nth,
                        (unsigned long long)m1.bytes_allocated, (unsigned long long)m0.bytes_allocated);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQ_I(dict_gate, 1);              /* the attach's gate */
    TEST_ASSERT_EQ_I(dict_root, 2);              /* first_occ, then the root */
    TEST_ASSERT_EQ_I(dict_tier, 2);              /* the entry tier, the code tier */
    PASS();
}


/* ---- Task 5: STR equality and inequality in filters ---- */

/* ST_Q_OK for a long local query text: the message is cut */
#define ST_Q_OK_N(q) TEST_ASSERT_FMT(st_q_run(q), "%.400s: %.400s", (q), st_msg)

/* The generation-2 run's counters of st_same_q: materializations, decoded
 * bytes, and the STR selections it answered (a first conjunct, a
 * refinement). */
typedef struct { uint64_t mat, dec, sel, refine; } st_fk_t;

/* q (table T) on <name>0 and <name>2 (`load` formats the load of the
 * table directory): equal formatted results; the generation-0 run takes
 * no STR selection; the generation-2 run's counters into *k. */
static test_result_t st_same_load(const char* name, const char* load, const char* q, st_fk_t* k) {
    char buf[1024], ld[700];
    ray_t* out[2] = { NULL, NULL };
    for (int g = 0; g < 2; g++) {
        snprintf(ld, sizeof ld, load, ST_TMP, name, g ? 2 : 0);
        snprintf(buf, sizeof buf, "(set T %s)", ld);
        ST_Q_OK_N(buf);
        ray_enc_counters_reset();
        const uint64_t s0 = ray_exec_test_str_sel(0), r0 = ray_exec_test_str_sel(1);
        out[g] = st_q_fmt(q);
        TEST_ASSERT_FMT(out[g] != NULL, "%s gen %d: %s", q, g ? 2 : 0, st_msg);
        st_hold(out[g]);
        ray_enc_counters_t c; ray_enc_counters_get(&c);
        const uint64_t ds = ray_exec_test_str_sel(0) - s0, dr = ray_exec_test_str_sel(1) - r0;
        if (!g) TEST_ASSERT_FMT(ds == 0 && dr == 0, "%s: generation 0 took the STR selection", q);
        else if (k) *k = (st_fk_t){ c.materialized, c.decoded_bytes, ds, dr };
    }
    TEST_ASSERT_FMT(ray_str_len(out[0]) == ray_str_len(out[1]) &&
                    memcmp(ray_str_ptr(out[0]), ray_str_ptr(out[1]), ray_str_len(out[0])) == 0,
                    "%s differs: %.*s vs %.*s", q, (int)ray_str_len(out[0]), ray_str_ptr(out[0]),
                    (int)ray_str_len(out[1]), ray_str_ptr(out[1]));
    for (int g = 0; g < 2; g++) { st_unhold(out[g], NULL); ray_release(out[g]); }
    PASS();
}
#define ST_SPLAY_LOAD "(.db.splayed.get \"%s/%s%d/\")"
static test_result_t st_same_q(const char* name, const char* q, st_fk_t* k) {
    return st_same_load(name, ST_SPLAY_LOAD, q, k);
}
#define ST_SAME(name, q, k) do { test_result_t r_ = st_same_q((name), (q), (k)); \
    if (r_.status != TEST_PASS) return r_; } while (0)
#define ST_SAME_P(name, q, k) do { test_result_t r_ = st_same_load((name), "(.db.parted.get \"%s/%s%d\" 't)", (q), (k)); \
    if (r_.status != TEST_PASS) return r_; } while (0)

/* Table text `tbl` saved as <name>0 (generation 0) and <name>2. */
static test_result_t st_save2(const char* name, const char* tbl) {
    char q[4096];
    snprintf(q, sizeof q, "(set W %s)", tbl); ST_Q_OK_N(q);
    snprintf(q, sizeof q, "(.db.splayed.set \"" ST_TMP "/%s0/\" W {compress: false})", name); ST_Q_OK_N(q);
    snprintf(q, sizeof q, "(.db.splayed.set \"" ST_TMP "/%s2/\" W {compress: true})", name); ST_Q_OK_N(q);
    PASS();
}
#define ST_SAVE2(name, tbl) do { test_result_t r_ = st_save2((name), (tbl)); \
    if (r_.status != TEST_PASS) return r_; } while (0)

/* vectors 0..63 URL-like (FSST, its table trained on them), 64..127 low
 * cardinality (STR_DICT), 128.. short distinct strings (not STR_DICT); ""
 * every 97th row; f a U8 flag */
#define ST_FL_TABLE "(table [s v f] (list (as 'STR (map (fn [j] (if (== 0 (% j 97)) \"\" " \
    "(if (< j 65536) (format \"https://www.example.com/path/%/item?id=%&ref=search\" (% j 5000) (% (* j 7919) 100003)) " \
    "(if (< j 131072) (format \"cat % x\" (% j 37)) (format \"u%\" j))))) (til 140000))) (til 140000) (as 'U8 (% (til 140000) 3))))"
#define ST_FL_URL "https://www.example.com/path/1000/item?id=18763&ref=search"

static test_result_t test_filter_strdict(void) {
    ST_SAVE2("fl", ST_FL_TABLE);
    char p[600]; TEST_ASSERT_TRUE(st_live("/fl2", "s", p, sizeof p));
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_FSST);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 64, NULL), RAY_ENC_STR_DICT);
    TEST_ASSERT_TRUE(st_desc_enc(p, 128, NULL) != RAY_ENC_STR_DICT);
    st_fk_t k;
    ST_SAME("fl", "(count (select {from: T where: (== s \"cat 5 x\")}))", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    TEST_ASSERT_EQ_U(k.sel, 1);
    ST_SAME("fl", "(count (select {from: T where: (!= s \"cat 5 x\")}))", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    TEST_ASSERT_EQ_U(k.sel, 1);
    ST_SAME("fl", "(count (select {from: T where: (== \"cat 5 x\" s)}))", &k);   /* the constant first */
    TEST_ASSERT_EQ_U(k.sel, 1);
    /* absent everywhere: the STR_DICT chunk decided, the FSST vectors decoded */
    ST_SAME("fl", "(count (select {from: T where: (== s \"nope\")}))", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    TEST_ASSERT_TRUE(k.dec > 0);
    ST_SAME("fl", "(count (select {from: T where: (!= s \"nope\")}))", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    /* an FSST-chunk value; the select returns rows by range gather */
    ST_SAME("fl", "(select {v: v from: T where: (== s \"" ST_FL_URL "\")})", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    ST_SAME("fl", "(select {v: v s: s from: T where: (== s \"cat 36 x\")})", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    /* nulls (""): byte equality, both sides */
    ST_SAME("fl", "(count (select {from: T where: (== s \"\")}))", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    TEST_ASSERT_EQ_U(k.sel, 1);
    ST_SAME("fl", "(count (select {from: T where: (!= s \"\")}))", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    PASS();
}

/* Only the STR_DICT chunk: code bounds decide the vectors, nothing decoded. */
static test_result_t test_filter_decided(void) {
    ST_SAVE2("fd", "(table [s] (list (as 'STR (map (fn [j] (format \"cat % x\" (% j 37))) (til 70000)))))");
    st_fk_t k;
    ST_SAME("fd", "(count (select {from: T where: (== s \"not a category\")}))", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    TEST_ASSERT_EQ_U(k.dec, 0);
    ST_SAME("fd", "(count (select {from: T where: (== s \"cat 3 x\")}))", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    TEST_ASSERT_EQ_U(k.dec, 0);                                       /* codes compared, not strings */
    ST_SAME("fd", "(count (select {from: T where: (!= s \"cat 3 x\")}))", &k);
    TEST_ASSERT_EQ_U(k.dec, 0);
    /* the `z, a, m` column of compress_str.rfl: code order is not value order */
    ST_SAVE2("fz", "(table [s] (list (as 'STR (at [\"z\" \"a\" \"m\"] (% (til 70000) 3)))))");
    char p[600]; TEST_ASSERT_TRUE(st_live("/fz2", "s", p, sizeof p));
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_STR_DICT);
    ST_SAME("fz", "(count (select {from: T where: (== s \"a\")}))", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    TEST_ASSERT_EQ_U(k.dec, 0);
    ST_SAME("fz", "(count (select {from: T where: (== s \"b\")}))", &k);   /* between codes, absent */
    TEST_ASSERT_EQ_U(k.dec, 0);
    ST_SAME("fz", "(count (select {from: T where: (!= s \"m\")}))", &k);
    TEST_ASSERT_EQ_U(k.dec, 0);
    PASS();
}

/* Chained conjuncts (ruling R1): the optimizer splits the WHERE, runs the
 * conjunct it ranks most selective first (opt.c filter_rank_key: a STR
 * `==` ranks first, a STR `!=` after any equality and a narrow flag), and a
 * later STR conjunct refines the installed selection
 * (exec_pred_refine_selection), on codes.  Generation-0 equal, nothing
 * materialized, every query through a STR selection; `refines` marks the
 * queries whose STR conjunct runs second. */
static test_result_t test_filter_chained(void) {
    ST_SAVE2("fc", ST_FL_TABLE);
    static const struct { const char* q; bool refines; } qs[] = {
        /* the brief's pair: the STR equality ranks first either way */
        { "(count (select {from: T where: (and (> v 100) (== s \"cat 5 x\"))}))", false },
        { "(count (select {from: T where: (and (== s \"cat 5 x\") (> v 100))}))", false },
        { "(count (select {from: T where: (and (< v 70000) (!= s \"cat 5 x\"))}))", false },
        { "(count (select {from: T where: (and (>= v 60000) (== s \"\"))}))", false },
        { "(select {v: v from: T where: (and (< v 3000) (== s \"cat 5 x\"))})", false },
        /* the STR conjunct second */
        { "(count (select {from: T where: (and (== f 1) (!= s \"\"))}))", true },
        { "(count (select {from: T where: (and (!= s \"\") (== f 1))}))", true },
        { "(count (select {from: T where: (and (== f 2) (!= s \"cat 5 x\"))}))", true },
        { "(count (select {from: T where: (and (== f 0) (!= s \"" ST_FL_URL "\"))}))", true },
        { "(count (select {from: T where: (and (in v [5 97 1000 70005 70006 131100 139999]) (!= s \"\"))}))", true },
        { "(select {v: v s: s from: T where: (and (in v [5 97 1000 70005 70006 131100 139999]) (!= s \"cat 6 x\"))})", true },
        { "(count (select {from: T where: (and (== s \"cat 5 x\") (!= s \"cat 6 x\"))}))", true },
        { "(count (select {from: T where: (and (== s \"cat 5 x\") (== s \"cat 6 x\"))}))", true },
        { "(count (select {from: T where: (and (== s \"cat 5 x\") (== s \"cat 5 x\"))}))", true },
        { "(count (select {from: T where: (and (== f 1) (!= s \"nope\"))}))", true },
        { "(count (select {from: T where: (and (== f 1) (> v 130000) (!= s \"\"))}))", true },
    };
    for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) {
        st_fk_t k;
        ST_SAME("fc", qs[i].q, &k);
        TEST_ASSERT_FMT(k.mat == 0, "%s: %llu materializations", qs[i].q, (unsigned long long)k.mat);
        TEST_ASSERT_FMT(k.sel + k.refine >= 1, "%s: no STR selection", qs[i].q);
        if (qs[i].refines) TEST_ASSERT_FMT(k.refine == 1, "%s: %llu refinements", qs[i].q, (unsigned long long)k.refine);
        else TEST_ASSERT_FMT(k.sel == 1 && k.refine == 0, "%s: %llu first conjuncts, %llu refinements", qs[i].q,
                             (unsigned long long)k.sel, (unsigned long long)k.refine);
    }
    PASS();
}

/* Parted: two encoded partitions with their own dictionaries (a code of
 * one is not a code of the other), a generation-0 partition and a ragged
 * partition without the column (a synthetic all-null segment), each
 * against the same partitions saved as generation 0. */
static test_result_t test_filter_parted(void) {
    static const char* const parts[4] = {
        "(table [v s] (list (til 70000) (as 'STR (map (fn [j] (format \"cat % x\" (% j 37))) (til 70000)))))",
        "(table [v s] (list (til 3000) (as 'STR (map (fn [j] (if (== 0 (% j 7)) \"\" (format \"cat % x\" (% (+ j 11) 41)))) (til 3000)))))",
        "(table [v s] (list (til 5000) (as 'STR (map (fn [j] (format \"https://www.example.com/path/%/item?id=%&ref=search\" (% j 999) j)) (til 5000)))))",
        "(table [v] (list (til 2000)))",
    };
    for (int gen = 0; gen <= 2; gen += 2)
        for (int p = 0; p < 4; p++) {
            char q[1200];
            /* partition 2 stays generation 0 in the encoded store */
            const bool enc = gen == 2 && p != 1;
            snprintf(q, sizeof q, "(.db.splayed.set \"" ST_TMP "/fp%d/2024.01.0%d/t/\" %s {compress: %s})",
                     gen, p + 1, parts[p], enc ? "true" : "false");
            ST_Q_OK_N(q);
        }
    static const struct { const char* q; bool refines; } qs[] = {
        { "(count (select {from: T where: (== s \"cat 5 x\")}))", false },
        { "(count (select {from: T where: (!= s \"cat 5 x\")}))", false },
        { "(count (select {from: T where: (== s \"cat 40 x\")}))", false },  /* partition 2 only */
        { "(count (select {from: T where: (== s \"nope\")}))", false },
        { "(count (select {from: T where: (== s \"\")}))", false },  /* partitions 2 and 4 */
        { "(count (select {from: T where: (!= s \"\")}))", false },
        { "(count (select {from: T where: (== s \"https://www.example.com/path/7/item?id=4003&ref=search\")}))", false },
        { "(sum (at (select {from: T where: (== s \"cat 5 x\")}) 'v))", false },
        { "(count (select {from: T where: (and (> v 1000) (== s \"cat 5 x\"))}))", false },
        { "(count (select {from: T where: (and (> v 1000) (!= s \"\"))}))", false },
        { "(count (select {from: T where: (and (in v [0 7 14 1000 2999 4999 69999]) (!= s \"\"))}))", true },
        { "(count (select {from: T where: (and (== v 7) (!= s \"cat 5 x\"))}))", true },
    };
    for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) {
        st_fk_t k;
        ST_SAME_P("fp", qs[i].q, &k);
        TEST_ASSERT_FMT(k.mat == 0, "%s: %llu materializations", qs[i].q, (unsigned long long)k.mat);
        /* `refines`: the `in` / `==` on v runs first, the STR conjunct refines */
        if (qs[i].refines) TEST_ASSERT_FMT(k.refine == 1, "%s: %llu refinements", qs[i].q, (unsigned long long)k.refine);
        else TEST_ASSERT_FMT(k.sel == 1 && k.refine == 0, "%s: %llu first conjuncts, %llu refinements", qs[i].q,
                             (unsigned long long)k.sel, (unsigned long long)k.refine);
    }
    PASS();
}

/* Mixed-encoding chunks (D4 / D5 nesting): chunk 0 is not a dictionary
 * chunk and mixes FSST vectors (URLs) with a CONST vector (one string) and
 * an all-null CONST vector; chunk 1 is a STR_DICT chunk holding an all-null
 * CONST vector, so it has no directory bounds over vectors that do. */
#define ST_MX_TABLE "(table [s v] (list (as 'STR (map (fn [j] " \
    "(if (and (>= j 3072) (< j 4096)) \"one constant string for this vector\" " \
    "(if (and (>= j 5120) (< j 6144)) \"\" " \
    "(if (< j 65536) (format \"https://www.example.com/path/%/item?id=%&ref=search\" (% j 5000) (% (* j 7919) 100003)) " \
    "(if (and (>= j 71680) (< j 72704)) \"\" (format \"cat % x\" (% j 37))))))) (til 131072))) (til 131072)))"
static test_result_t test_filter_mixed_chunks(void) {
    ST_SAVE2("fm", ST_MX_TABLE);
    char p[600]; TEST_ASSERT_TRUE(st_live("/fm2", "s", p, sizeof p));
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_FSST);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 3, NULL), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 5, NULL), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 64, NULL), RAY_ENC_STR_DICT);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 70, NULL), RAY_ENC_CONST);
    TEST_ASSERT_TRUE(st_dirent(p, 0).flags & RAY_ENC_STAT_ABSENT);
    TEST_ASSERT_TRUE(st_dirent(p, 1).flags & RAY_ENC_STAT_ABSENT);   /* not every vector STR_DICT */
    ray_enc_desc_t d; TEST_ASSERT_EQ_I(st_desc_enc(p, 64, &d), RAY_ENC_STR_DICT);
    TEST_ASSERT_FALSE(d.flags & RAY_ENC_STAT_ABSENT);                 /* the vector keeps its bounds */
    static const char* const lits[] = {
        "cat 5 x", "", "one constant string for this vector", "nope",
        "https://www.example.com/path/1000/item?id=18763&ref=search",
    };
    for (size_t i = 0; i < sizeof lits / sizeof *lits; i++)
        for (int ne = 0; ne < 2; ne++) {
            char q[400]; st_fk_t k;
            snprintf(q, sizeof q, "(count (select {from: T where: (%s s \"%s\")}))", ne ? "!=" : "==", lits[i]);
            ST_SAME("fm", q, &k);
            TEST_ASSERT_FMT(k.mat == 0 && k.sel == 1, "%s: %llu materializations, %llu selections", q,
                            (unsigned long long)k.mat, (unsigned long long)k.sel);
        }
    PASS();
}

/* A single-file column whose materialization is published is declined
 * (its STR filter keeps the bool path, which reads it in place): the
 * answer equals generation 0 before and after the materialization. */
static test_result_t test_filter_materialized(void) {
    ST_SAVE2("fx", ST_FL_TABLE);
    static const char* const qs[] = {
        "(count (select {from: T where: (== s \"cat 5 x\")}))",
        "(count (select {from: T where: (!= s \"\")}))",
        "(count (select {from: T where: (and (== f 1) (!= s \"cat 5 x\"))}))",
    };
    for (int mat = 0; mat < 2; mat++)
        for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) {
            if (!mat) {                                   /* not materialized: the STR selection */
                st_fk_t k;
                ST_SAME("fx", qs[i], &k);
                TEST_ASSERT_FMT(k.sel + k.refine == 1, "%s: not through the STR selection", qs[i]);
                continue;
            }
            /* materialize generation 2's column, then filter the same table */
            ST_Q_OK("(set T2 (.db.splayed.get \"" ST_TMP "/fx2/\"))");
            ray_t* t = st_hold(ray_eval_str("T2"));
            TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
            ray_t* encv = ray_table_get_col_enc(t, ray_sym_intern("s", 1));
            TEST_ASSERT_TRUE(encv != NULL);
            ray_t* m = ray_enccol_materialize(encv);              /* borrowed: the column's cache */
            TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
            TEST_ASSERT_TRUE(ray_enccol_decoded_peek(encv) != NULL);
            ST_Q_OK("(set T (.db.splayed.get \"" ST_TMP "/fx0/\"))");
            ray_t* a = st_q_fmt(qs[i]);
            TEST_ASSERT_FMT(a != NULL, "%s gen 0: %s", qs[i], st_msg);
            st_hold(a);
            ST_Q_OK("(set T T2)");
            const uint64_t s0 = ray_exec_test_str_sel(0), r0 = ray_exec_test_str_sel(1);
            ray_t* b = st_q_fmt(qs[i]);
            TEST_ASSERT_FMT(b != NULL, "%s materialized: %s", qs[i], st_msg);
            st_hold(b);
            TEST_ASSERT_FMT(ray_exec_test_str_sel(0) == s0 && ray_exec_test_str_sel(1) == r0,
                            "%s: a published materialization took the STR selection", qs[i]);
            TEST_ASSERT_FMT(ray_str_len(a) == ray_str_len(b) && memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) == 0,
                            "%s differs after materializing", qs[i]);
            st_unhold(a, NULL); ray_release(a); st_unhold(b, NULL); ray_release(b);
            st_unhold(t, NULL); ray_release(t);
        }
    PASS();
}

/* A forged code (outside the entry) fails the filter "corrupt"; it never
 * answers from the bytes it would have read. */
static test_result_t test_filter_forged(void) {
    ray_t* v = st_hold(ray_vec_new(RAY_STR, SF_ROWS));
    for (int64_t i = 0; i < SF_ROWS; i++) {
        const int k = sf_entry(i / RAY_ENC_VEC);
        ray_t* v2 = ray_str_vec_append(v, g_sf_s[k], strlen(g_sf_s[k]));
        st_unhold(v, v2); v = v2;
    }
    ray_t* tb = ray_table_new(1);
    tb = ray_table_add_col(tb, ray_sym_intern("s", 1), v);
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("W", 1), tb), RAY_OK);
    ray_release(tb);
    ray_encw_test_force_str(RAY_ENC_STR_DICT);              /* selection keeps one-string vectors CONST */
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/ff/\" W {compress: true})");
    ray_encw_test_force_str(-1);
    char p[600]; TEST_ASSERT_TRUE(st_live("/ff", "s", p, sizeof p));
    ST_Q_OK("(set T (.db.splayed.get \"" ST_TMP "/ff/\"))");
    ST_Q_TRUE("(== 34816 (count (select {from: T where: (== s \"forged entry one, a long string\")})))");   /* 34 odd vectors of 1024 rows */
    TEST_ASSERT_TRUE(sf_set_code(p, 3, 9));                  /* 3 entries: code 9 is outside */
    ST_Q_OK("(set T (.db.splayed.get \"" ST_TMP "/ff/\"))");
    for (int q = 0; q < 2; q++) {
        ray_t* r = st_hold(ray_eval_str(q ? "(count (select {from: T where: (!= s \"forged entry one, a long string\")}))"
                                          : "(count (select {from: T where: (== s \"forged entry one, a long string\")}))"));
        TEST_ASSERT_FMT(r && RAY_IS_ERR(r) && strcmp(ray_err_code(r), "corrupt") == 0, "query %d: %s", q,
                        r && RAY_IS_ERR(r) ? ray_err_code(r) : "an answer");
    }
    PASS();
}

/* ---- Task 6: group-by on dictionary codes, cross-segment translation ---- */

/* Rewrite column `col` (the column of W) of the table saved at ST_TMP `rel`
 * chunk by chunk through the writer, chunk c's STR vectors forced to
 * forces[c] (-1: selection), into the live generation's file (same type,
 * name and length as the file it replaces). */
static test_result_t st_rewrite(const char* rel, const char* col, int64_t n_chunks, const int* forces) {
    char path[600], tmp[620];
    TEST_ASSERT_TRUE(st_live(rel, col, path, sizeof path));
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    char q[64]; snprintf(q, sizeof q, "(at W '%s)", col);
    ray_t* w = st_hold(ray_eval_str(q));
    TEST_ASSERT_TRUE(w && !RAY_IS_ERR(w) && w->type == RAY_STR);
    const int64_t n = w->len;
    TEST_ASSERT_TRUE(n > (n_chunks - 1) * RAY_ENC_CHUNK && n <= n_chunks * RAY_ENC_CHUNK);
    FILE* f = fopen(tmp, "wb+");
    TEST_ASSERT_TRUE(f != NULL);
    ray_encw_t ew;
    ray_err_t rc = ray_encw_begin(&ew, f, RAY_STR, 0, n, 0, 0, 0);
    const char* pool = w->str_pool ? (const char*)ray_data(w->str_pool) : NULL;
    for (int64_t c = 0; rc == RAY_OK && c < n_chunks; c++) {
        const int64_t m = n - c * RAY_ENC_CHUNK < RAY_ENC_CHUNK ? n - c * RAY_ENC_CHUNK : RAY_ENC_CHUNK;
        ray_encw_test_force_str(forces[c]);
        rc = ray_encw_chunk(&ew, (const ray_str_t*)ray_data(w) + c * RAY_ENC_CHUNK, pool, m);
    }
    ray_encw_test_force_str(-1);
    if (rc == RAY_OK) rc = ray_encw_finish(&ew);
    ray_encw_free(&ew); fclose(f);
    TEST_ASSERT_EQ_I(rc, RAY_OK);
    TEST_ASSERT_EQ_I(rename(tmp, path), 0);
    st_unhold(w, NULL); ray_release(w);
    PASS();
}
#define ST_REWRITE(rel, col, nc, forces) do { test_result_t r_ = st_rewrite((rel), (col), (nc), (forces)); \
    if (r_.status != TEST_PASS) return r_; } while (0)

/* Review focus 7: one hash.  Chunk 0 RAW (the cursor path hashes each
 * ray_str_t), chunk 1 STR_DICT (the remap hashes each entry's bytes), the
 * same three long strings in both: three groups, as generation 0. */
static test_result_t test_remap_hash_parity(void) {
    ST_SAVE2("hp", "(table [s v m] (list (as 'STR (at [\"first long string value\" \"second long string value\" "
                   "\"third long string value\"] (% (til 131072) 3))) (til 131072) (as 'U8 (% (til 131072) 2))))");
    static const int forces[2] = { RAY_ENC_RAW, RAY_ENC_STR_DICT };
    ST_REWRITE("/hp2", "s", 2, forces);
    char p[600]; TEST_ASSERT_TRUE(st_live("/hp2", "s", p, sizeof p));
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_RAW);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 64, NULL), RAY_ENC_STR_DICT);
    st_fk_t k;
    ST_Q_OK("(set T (.db.splayed.get \"" ST_TMP "/hp2/\"))");
    ST_Q_TRUE("(== 3 (count (select {c: (count v) from: T by: s})))");
    ST_SAME("hp", "(select {c: (count v) from: T by: s})", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    ST_SAME("hp", "(select {c: (count v) from: T by: {s: s m: m}})", &k);
    TEST_ASSERT_EQ_U(k.mat, 0);
    PASS();
}

/* Review focus 3: partitions with conflicting codes.  Partition 1 meets
 * "red" first, partition 2 "blue": the same string has different codes,
 * the same code different strings. */
static test_result_t test_cross_segment_codes(void) {
    static const char* const parts[2] = {
        "(table [s v m] (list (as 'STR (at [\"red long value\" \"blue long value\" \"green\"] (% (til 70000) 3))) (til 70000) (as 'U8 (% (til 70000) 2))))",
        "(table [s v m] (list (as 'STR (at [\"blue long value\" \"red long value\" \"amber\"] (% (til 70000) 3))) (til 70000) (as 'U8 (% (til 70000) 2))))",
    };
    /* subject: the STR_DICT remap; the persisted DICT would take the key (P6b) */
    ray_col_test_no_dict(true);
    for (int g = 0; g <= 2; g += 2)
        for (int p = 0; p < 2; p++) {
            char q[1200];
            snprintf(q, sizeof q, "(.db.splayed.set \"" ST_TMP "/cs%d/2024.01.0%d/t/\" %s {compress: %s})",
                     g, p + 1, parts[p], g ? "true" : "false");
            ST_Q_OK_N(q);
        }
    ray_col_test_no_dict(false);
    for (int p = 0; p < 2; p++) {
        char rel[64], f[600];
        snprintf(rel, sizeof rel, "/cs2/2024.01.0%d/t", p + 1);
        TEST_ASSERT_TRUE(st_live(rel, "s", f, sizeof f));
        TEST_ASSERT_EQ_I(st_hdr(f).index_kind, 0);
        TEST_ASSERT_EQ_I(st_desc_enc(f, 0, NULL), RAY_ENC_STR_DICT);
        TEST_ASSERT_EQ_I(st_desc_enc(f, 64, NULL), RAY_ENC_STR_DICT);
    }
    /* `codes`: every vector read is a STR_DICT vector and only s is read
     * by range, so nothing is decoded (the remap reads codes) */
    static const struct { const char* q; bool codes; } qs[] = {
        { "(count (select {from: T where: (== s \"red long value\")}))", true },
        { "(count (select {from: T where: (!= s \"blue long value\")}))", true },
        { "(select {v: v from: T where: (== s \"amber\") take: 5})", false },
        { "(select {c: (count v) from: T by: s})", true },
        { "(select {c: (count v) from: T by: s asc: s})", true },
        { "(select {c: (count v) from: T by: {s: s m: m} asc: [s m]})", false },
        /* under a selection: ids first-occurrence over the selected rows */
        { "(select {c: (count v) from: T where: (!= s \"red long value\") by: s})", false },
        { "(select {c: (count v) from: T where: (== s \"blue long value\") by: s})", false },
        { "(select {c: (count v) from: T where: (> v 69990) by: s})", false },
        { "(select {c: (count v) s: (sum v) from: T where: (and (> v 1000) (!= s \"green\")) by: s})", false },
    };
    for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) {
        st_fk_t k;
        ST_SAME_P("cs", qs[i].q, &k);
        TEST_ASSERT_FMT(k.mat == 0, "%s: %llu materializations", qs[i].q, (unsigned long long)k.mat);
        if (qs[i].codes) TEST_ASSERT_FMT(k.dec == 0, "%s: %llu bytes decoded", qs[i].q, (unsigned long long)k.dec);
    }
    /* a computed second key materializes v (its path binds the referenced
     * columns, any generation-2 type); the STR key stays codes */
    ST_SAME_P("cs", "(select {c: (count v) from: T by: {s: s r: (% v 2)} asc: [s r]})", NULL);
    ST_Q_TRUE("(not (at (.idx.info T 's) 'materialized))");
    PASS();
}

/* Group-by under a STR_DICT filter on a column shorter than
 * RAY_DICT_MIN_ROWS: the ids are first-occurrence over the selected rows
 * (generation 0 groups such a column as STR, in that order). */
static test_result_t test_groupby_selection(void) {
    ST_SAVE2("gs", "(table [s v m] (list (as 'STR (map (fn [j] (if (== 0 (% j 13)) \"\" "
                   "(format \"group key % long text\" (% (* j 7) 23)))) (til 40000))) (til 40000) (as 'U8 (% (til 40000) 4))))");
    char p[600]; TEST_ASSERT_TRUE(st_live("/gs2", "s", p, sizeof p));
    TEST_ASSERT_EQ_I(st_desc_enc(p, 1, NULL), RAY_ENC_STR_DICT);
    static const char* const qs[] = {
        "(select {c: (count v) from: T by: s})",
        "(select {c: (count v) from: T where: (!= s \"\") by: s})",
        "(select {c: (count v) from: T where: (!= s \"group key 0 long text\") by: s})",
        "(select {c: (count v) from: T where: (== s \"group key 5 long text\") by: s})",
        "(select {c: (count v) from: T where: (> v 39000) by: s})",
        "(select {c: (count v) from: T where: (and (> v 20000) (!= s \"\")) by: {s: s m: m}})",
    };
    for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) {
        st_fk_t k;
        ST_SAME("gs", qs[i], &k);
        TEST_ASSERT_FMT(k.mat == 0, "%s: %llu materializations", qs[i], (unsigned long long)k.mat);
        if (i == 0) TEST_ASSERT_FMT(k.dec == 0, "%s: %llu bytes decoded", qs[i], (unsigned long long)k.dec);
    }
    PASS();
}

/* Spec "Mixed STR group-by": one unparted column mixing STR_DICT, FSST,
 * CONST and RAW at chunk boundaries, equal strings crossing every
 * boundary, alone and inside a composite key. */
static test_result_t test_mixed_str_groupby(void) {
    /* rows 131072..132095 (vector 128) are all equal: CONST by selection;
     * the URLs are near-distinct, so unforced chunk 2 is a plain chunk */
    ST_SAVE2("mx", "(table [s v m] (list (as 'STR (map (fn [j] (if (and (>= j 131072) (< j 132096)) \"shared long key text\" "
                   "(if (< (% j 3) 2) (at [\"shared long key text\" \"another shared key text\" \"tiny\"] (% (div j 3) 3)) "
                   "(format \"https://e.com/%/%\" (% j 50) j)))) (til 262144))) (til 262144) (as 'U8 (% (til 262144) 3))))");
    static const int forces[4] = { RAY_ENC_STR_DICT, RAY_ENC_FSST, -1, RAY_ENC_RAW };
    ST_REWRITE("/mx2", "s", 4, forces);
    char p[600]; TEST_ASSERT_TRUE(st_live("/mx2", "s", p, sizeof p));
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_STR_DICT);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 64, NULL), RAY_ENC_FSST);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 128, NULL), RAY_ENC_CONST);
    TEST_ASSERT_TRUE(st_desc_enc(p, 129, NULL) != RAY_ENC_STR_DICT);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 192, NULL), RAY_ENC_RAW);
    static const char* const qs[] = {
        "(select {c: (count v) from: T by: s asc: s})",
        "(select {c: (count v) from: T by: s})",
        "(select {c: (count v) from: T by: {s: s m: m} asc: [s m]})",
        "(select {c: (count v) from: T by: {s: s m: m}})",
        "(select {c: (count v) from: T where: (!= s \"shared long key text\") by: s asc: s})",
    };
    for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) {
        st_fk_t k;
        ST_SAME("mx", qs[i], &k);
        TEST_ASSERT_FMT(k.mat == 0, "%s: %llu materializations", qs[i], (unsigned long long)k.mat);
    }
    /* a computed second key materializes v (its path binds the referenced
     * columns, any generation-2 type); the STR key stays codes and ranges */
    ST_SAME("mx", "(select {c: (count v) from: T by: {s: s r: (% v 3)} asc: [s r]})", NULL);
    ST_Q_TRUE("(not (at (.idx.info T 's) 'materialized))");
    PASS();
}

/* Task 6 review Minor 3: a group-by over a forged STR_DICT vector.  (a)
 * code 9 of three entries: the descriptors reject it, so the view reports
 * it; (b) a vector of codes 0 / 1 (bounds [0, 1]) whose FOR_BP base is
 * moved to 1: codes 1 / 2, rows outside the bounds, so the view declines
 * and the group path's cursor read decodes it, which reports it.  Either
 * way the group-by answers `corrupt`, as the filter path does.  (A sound
 * STR_DICT vector is never declined: every decline is a file the decode
 * rejects.) */
static int gf_entry(int64_t i) { return i / RAY_ENC_VEC == ray_enc_nvecs(SF_ROWS) - 1 ? 2 : (int)(i & 1); }
static test_result_t test_groupby_forged(void) {
    for (int f = 0; f < 2; f++) {
        ray_t* v = st_hold(ray_vec_new(RAY_STR, SF_ROWS));
        for (int64_t i = 0; i < SF_ROWS; i++) {
            const int k = f ? gf_entry(i) : sf_entry(i / RAY_ENC_VEC);
            ray_t* v2 = ray_str_vec_append(v, g_sf_s[k], strlen(g_sf_s[k]));
            st_unhold(v, v2); v = v2;
        }
        ray_t* tb = ray_table_new(2);
        tb = ray_table_add_col(tb, ray_sym_intern("s", 1), v);
        ray_t* iv = ray_vec_new(RAY_I64, SF_ROWS);
        for (int64_t i = 0; i < SF_ROWS; i++) iv = ray_vec_append(iv, &i);
        tb = ray_table_add_col(tb, ray_sym_intern("v", 1), iv);
        ray_release(iv);
        TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("W", 1), tb), RAY_OK);
        ray_release(tb);
        st_unhold(v, NULL); ray_release(v);
        ray_encw_test_force_str(RAY_ENC_STR_DICT);          /* selection keeps one-string vectors CONST */
        /* subject: the STR_DICT remap; the persisted DICT would take the key (P6b) */
        ray_col_test_no_dict(true);
        ST_Q_OK(f ? "(.db.splayed.set \"" ST_TMP "/gb/\" W {compress: true})"
                  : "(.db.splayed.set \"" ST_TMP "/ga/\" W {compress: true})");
        ray_col_test_no_dict(false);
        ray_encw_test_force_str(-1);
        char p[600]; TEST_ASSERT_TRUE(st_live(f ? "/gb" : "/ga", "s", p, sizeof p));
        TEST_ASSERT_EQ_I(st_hdr(p).index_kind, 0);
        const char* ld = f ? "(set T (.db.splayed.get \"" ST_TMP "/gb/\"))" : "(set T (.db.splayed.get \"" ST_TMP "/ga/\"))";
        ST_Q_OK(ld);
        /* unforged: the codes path answers (the three entries' counts) */
        ST_Q_TRUE("(all (== [34816 34816 368] (at (select {c: (count v) from: T by: s}) 'c)))");
        ST_Q_TRUE("(not (at (.idx.info T 's) 'materialized))");     /* codes, not a materialization */
        if (f == 0) TEST_ASSERT_TRUE(sf_set_code(p, 3, 9));
        else {
            ray_enc_desc_t d;
            TEST_ASSERT_EQ_I(st_desc_enc(p, 3, &d), RAY_ENC_STR_DICT);
            TEST_ASSERT_TRUE(d.min == 0 && d.max == 1);
            ray_enc_dirent_t de = st_dirent(p, 0);
            const long body = (long)(de.offset + d.payload_off);
            uint8_t ch[2]; ST_READ(p, body, ch, 2);
            TEST_ASSERT_TRUE(ch[0] == RAY_ENC_FOR_BP && ch[1] == 1);
            const uint64_t base = 1;
            ST_PATCH(p, body + 8, &base, 8);
        }
        ST_Q_OK(ld);
        static const char* const qs[3] = {
            "(select {c: (count v) from: T by: s})",
            "(count (select {from: T where: (== s \"forged entry zero, a long string\")}))",
            "(count (select {from: T where: (!= s \"forged entry zero, a long string\")}))",
        };
        for (int q = 0; q < 3; q++) {
            ray_t* r = ray_eval_str(qs[q]);
            const bool bad = r && RAY_IS_ERR(r) && strcmp(ray_err_code(r), "corrupt") == 0;
            const char* got = r && RAY_IS_ERR(r) ? ray_err_code(r) : "an answer";
            if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
            TEST_ASSERT_FMT(bad, "case %d %s: %s", f, qs[q], got);
        }
    }
    PASS();
}

/* Task 6 review Minor 3: the segment remap's allocation fails through the
 * materialization fail hook: the group-by answers `oom` and leaks nothing;
 * the retry answers.  The column is all STR_DICT (no vector decodes), and
 * after a warm run the remap is the query's only hooked allocation. */
static test_result_t test_groupby_remap_oom(void) {
    ray_t* v = st_hold(str_col(SF_ROWS, gen_lowcard));
    ray_t* tb = ray_table_new(2);
    tb = ray_table_add_col(tb, ray_sym_intern("s", 1), v);
    ray_t* iv = ray_vec_new(RAY_I64, SF_ROWS);
    for (int64_t i = 0; i < SF_ROWS; i++) iv = ray_vec_append(iv, &i);
    tb = ray_table_add_col(tb, ray_sym_intern("v", 1), iv);
    ray_release(iv);
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("W", 1), tb), RAY_OK);
    ray_release(tb);
    /* subject: the STR_DICT remap; the persisted DICT would take the key (P6b) */
    ray_col_test_no_dict(true);
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/ro/\" W {compress: true})");
    ray_col_test_no_dict(false);
    char p[600]; TEST_ASSERT_TRUE(st_live("/ro", "s", p, sizeof p));
    TEST_ASSERT_EQ_I(st_hdr(p).index_kind, 0);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_STR_DICT);
    TEST_ASSERT_EQ_I(st_desc_enc(p, ray_enc_nvecs(SF_ROWS) - 1, NULL), RAY_ENC_STR_DICT);
    ST_Q_OK("(set T (.db.splayed.get \"" ST_TMP "/ro/\"))");
    static const char q[] = "(select {c: (count v) from: T by: s})";
    ray_t* want = st_hold(st_q_fmt(q));                     /* warm: the dictionary's once-state */
    TEST_ASSERT_NOT_NULL(want);
    ST_Q_TRUE("(== 37 (count (select {c: (count v) from: T by: s})))");
    ST_Q_TRUE("(not (at (.idx.info T 's) 'materialized))");
    /* nth 2 is never reached: one hooked allocation (the counter's next
     * check consumes the last step) */
    ray_enccol_test_fail_alloc(2);
    ray_t* r = st_q_fmt(q);
    TEST_ASSERT_NOT_NULL(r);
    ray_release(r);
    TEST_ASSERT_TRUE(ray_enccol_test_nth_alloc_fails());
    ray_enccol_test_fail_alloc(0);
    for (int k = 0; k < 2; k++) {
        ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
        ray_enccol_test_fail_alloc(1);
        r = ray_eval_str(q);
        ray_enccol_test_fail_alloc(0);
        const bool oom = r && RAY_IS_ERR(r) && strcmp(ray_err_code(r), "oom") == 0;
        const char* got = r && RAY_IS_ERR(r) ? ray_err_code(r) : "an answer";
        if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
        TEST_ASSERT_FMT(oom, "round %d: %s", k, got);
        ray_mem_stats(&m1);
        TEST_ASSERT_FMT(m1.bytes_allocated == m0.bytes_allocated, "round %d: %llu vs %llu", k,
                        (unsigned long long)m1.bytes_allocated, (unsigned long long)m0.bytes_allocated);
    }
    r = st_hold(st_q_fmt(q));                              /* the retry */
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(ray_str_len(r) == ray_str_len(want) && memcmp(ray_str_ptr(r), ray_str_ptr(want), ray_str_len(r)) == 0);
    PASS();
}

/* ---- Task 7: abandonment, representation, stream identity ---- */

/* The whole file at `path` (malloc'd, NULL on failure); its size in *n. */
static uint8_t* slurp_file(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t* p = NULL;
    long sz = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (sz >= 0 && fseek(f, 0, SEEK_SET) == 0 && (p = malloc((size_t)sz + 1)) != NULL &&
        fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); p = NULL; }
    fclose(f);
    *n = p ? (size_t)sz : 0;
    return p;
}

/* Review focus 4: past 1<<20 entries the accumulator is abandoned.  Each
 * random printable 24-byte string appears twice, so a chunk's dictionary
 * (32768 entries) beats its plain encodings until the cap: chunks 0..31
 * are STR_DICT, chunks 32 and 33 FSST or RAW. */
static void abandon_str(int64_t pair, char* b) {
    uint64_t x = 0x9E3779B97F4A7C15ull * (uint64_t)(pair + 1);
    for (int k = 0; k < 24; k++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; b[k] = (char)(33 + x % 94); }
}
static test_result_t test_abandonment(void) {
    const char* p = STP("/ab");
    int64_t n = 2 * ((int64_t)RAY_ENCW_SDICT_CAP + 65536);
    TEST_ASSERT_EQ_I(n, 2228224);
    ray_t* v = st_hold(ray_vec_new(RAY_STR, n));
    char b[24];
    for (int64_t i = 0; i < n; i++) {
        abandon_str(i / 2, b);
        ray_t* v2 = ray_str_vec_append(v, b, 24);
        st_unhold(v, v2); v = v2;
    }
    TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v) && v->len == n);
    TEST_ASSERT_EQ_I(ray_col_save_bulk_gen(v, p, 2), RAY_OK);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 0, NULL), RAY_ENC_STR_DICT);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 31 * 64, NULL), RAY_ENC_STR_DICT);
    TEST_ASSERT_EQ_I(st_desc_enc(p, 31 * 64 + 63, NULL), RAY_ENC_STR_DICT);
    TEST_ASSERT_TRUE(st_desc_enc(p, 32 * 64, NULL) != RAY_ENC_STR_DICT);
    TEST_ASSERT_TRUE(st_desc_enc(p, ray_enc_nvecs(n) - 1, NULL) != RAY_ENC_STR_DICT);
    ray_t* c = st_hold(ray_col_mmap(p));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    const ray_enc_sdict_t* sd;
    TEST_ASSERT_NULL(ray_enccol_strdict(ray_enccol_of(c), &sd));
    TEST_ASSERT_TRUE(sd && sd->n == RAY_ENCW_SDICT_CAP);
    TEST_ASSERT_FALSE(ray_enccol_may_have_index(c));       /* a bare file: no recorded kind, no UNIQUE (P6b) */
    ray_t* m = ray_enccol_materialize(c);                  /* borrowed: the column's cache */
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m) && st_same(m, v));
    TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_NONE);
    PASS();
}

/* Review focus 5: equal long strings at distinct pool offsets with differing
 * cached hashes are one value for CONST and for STR_DICT. */
static test_result_t test_representation_not_value(void) {
    ray_t* v = st_hold(ray_vec_new(RAY_STR, 2048));
    ray_t* u = st_hold(ray_vec_new(RAY_STR, 2048));        /* the same strings, untouched hashes */
    for (int i = 0; i < 2048; i++) {
        ray_t* v2 = ray_str_vec_append(v, "the same long string value", 26);
        st_unhold(v, v2); v = v2;
        ray_t* u2 = ray_str_vec_append(u, "the same long string value", 26);
        st_unhold(u, u2); u = u2;
    }
    ray_str_t* s = (ray_str_t*)ray_data(v);
    TEST_ASSERT_TRUE(s[0].pool_off != s[1].pool_off);
    s[1].hash32 = s[0].hash32 ^ 0x5A5A;                    /* a different cached hash */
    TEST_ASSERT_TRUE(s[1].hash32 != s[0].hash32);
    static const struct { const char* rel; int enc; } fs[] = {
        { "/rc", RAY_ENC_CONST }, { "/rd", RAY_ENC_STR_DICT }, { "/uc", RAY_ENC_CONST }, { "/ud", RAY_ENC_STR_DICT } };
    for (int k = 0; k < 4; k++) {
        char rel[64]; snprintf(rel, sizeof rel, ST_TMP "%s", fs[k].rel);
        ray_encw_test_force_str(fs[k].enc);
        ray_err_t e = ray_col_save_gen(k < 2 ? v : u, ray_test_tmp(rel), 2);
        ray_encw_test_force_str(-1);
        TEST_ASSERT_EQ_I(e, RAY_OK);
    }
    TEST_ASSERT_EQ_I(st_desc_enc(STP("/rc"), 0, NULL), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(st_desc_enc(STP("/rc"), 1, NULL), RAY_ENC_CONST);
    TEST_ASSERT_EQ_I(st_desc_enc(STP("/rd"), 0, NULL), RAY_ENC_STR_DICT);
    /* the cached hash is representation: the files equal the untouched ones */
    size_t na, nb;
    uint8_t* fa = slurp_file(STP("/rc"), &na); uint8_t* fb = slurp_file(STP("/uc"), &nb);
    bool same = fa && fb && na == nb && memcmp(fa, fb, na) == 0;
    free(fa); free(fb);
    TEST_ASSERT_TRUE(same);
    fa = slurp_file(STP("/rd"), &na); fb = slurp_file(STP("/ud"), &nb);
    same = fa && fb && na == nb && memcmp(fa, fb, na) == 0;
    free(fa); free(fb);
    TEST_ASSERT_TRUE(same);
    ray_t* c = st_hold(ray_col_mmap(STP("/rd")));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    const ray_enc_sdict_t* sd;
    TEST_ASSERT_NULL(ray_enccol_strdict(ray_enccol_of(c), &sd));
    TEST_ASSERT_TRUE(sd && sd->n == 1);
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m) && st_same(m, v));
    c = st_hold(ray_col_mmap(STP("/rc")));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m) && st_same(m, v));
    PASS();
}

/* The stream writer (.csv.splayed) and the resident writer write the same
 * STR file, dictionary and FSST table included: the stream, the resave of
 * the encoded source (range reads), and the plain in-memory vector through
 * ray_col_save_gen and ray_col_save_bulk_gen (the converters' path).  Order
 * 0 is the brief's (low cardinality first: 64 STR_DICT, 4 FSST, 69 RAW
 * vectors); order 1 puts the URLs first (an FSST table trained on them:
 * 64 FSST vectors, then 73 STR_DICT). */
static const char* se_row(int o, int64_t i, char* b) {
    return (i < 70000) == (o == 0) ? gen_lowcard(i, b) : gen_urls(i, b);
}
static test_result_t test_stream_equals_resident(void) {
    char b[256], q[600], csv[64], cs[64], rs[64];
    for (int o = 0; o < 2; o++) {
        snprintf(csv, sizeof csv, ST_TMP "/in%d.csv", o);
        snprintf(cs, sizeof cs, "/cs%d", o); snprintf(rs, sizeof rs, "/rs%d", o);
        FILE* f = fopen(ray_test_tmp(csv), "w");
        TEST_ASSERT_NOT_NULL(f);
        fprintf(f, "s,v\n");
        for (int64_t i = 0; i < 140000; i++) fprintf(f, "%s,%lld\n", se_row(o, i, b), (long long)i);
        fclose(f);
        snprintf(q, sizeof q, "(.csv.splayed {compress: true} [STR I64] \"%s\" \"" ST_TMP "%s/\")", csv, cs);
        ST_Q_OK(q);
        snprintf(q, sizeof q, "(.db.splayed.set \"" ST_TMP "%s/\" (.db.splayed.get \"" ST_TMP "%s/\") {compress: true})", rs, cs);
        ST_Q_OK(q);
        ray_t* v = st_hold(ray_vec_new(RAY_STR, 140000));
        for (int64_t i = 0; i < 140000; i++) {
            const char* sv = se_row(o, i, b);
            ray_t* v2 = ray_str_vec_append(v, sv, strlen(sv));
            st_unhold(v, v2); v = v2;
        }
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, STP("/pl"), 2), RAY_OK);
        TEST_ASSERT_EQ_I(ray_col_save_bulk_gen(v, STP("/pb"), 2), RAY_OK);
        st_unhold(v, NULL); ray_release(v);
        char pc[600], pr[600];
        TEST_ASSERT_TRUE(st_live(cs, "s", pc, sizeof pc));
        TEST_ASSERT_TRUE(st_live(rs, "s", pr, sizeof pr));
        /* the subject: the dictionary and the FSST table both in use */
        TEST_ASSERT_EQ_I(st_desc_enc(pc, o ? 64 : 0, NULL), RAY_ENC_STR_DICT);
        TEST_ASSERT_EQ_I(st_desc_enc(pc, o ? 0 : 64, NULL), RAY_ENC_FSST);
        if (o == 0) TEST_ASSERT_EQ_I(st_desc_enc(pc, 96, NULL), RAY_ENC_RAW);
        size_t na;
        uint8_t* pa = slurp_file(pc, &na);
        TEST_ASSERT_NOT_NULL(pa);
        free(st_mem); st_mem = pa;                         /* freed in teardown */
        /* the stream and the resave both carry the DICT (P6b): byte identity in full */
        size_t nr; uint8_t* pr2 = slurp_file(pr, &nr);
        const bool rsame = pr2 && nr == na && memcmp(pa, pr2, na) == 0;
        free(pr2);
        TEST_ASSERT_FMT(rsame, "order %d: resave differs from the stream", o);
        ray_enc_hdr_t eh; memcpy(&eh, pa + 32, sizeof eh);
        TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_DICT);
        TEST_ASSERT_TRUE(eh.index_off > 96 && eh.index_off < na);
        /* the bare files carry none: each is the stream's bytes before its
         * region's 32-alignment padding, its header without the region
         * (index_kind 0, index_off 0, end_off the bare file's size) */
        const uint64_t cut = eh.index_off;
        const char* const bare[2] = { STP("/pl"), STP("/pb") };
        for (int k = 0; k < 2; k++) {
            size_t nb; uint8_t* pb = slurp_file(bare[k], &nb);
            bool same = pb && nb >= 96 && nb <= cut && ray_enc_pad32(nb) == cut;
            if (same) {
                uint8_t* norm = malloc(nb);
                same = norm != NULL;
                if (same) {
                    memcpy(norm, pa, nb);
                    ray_enc_hdr_t nh = eh; nh.index_kind = 0; nh.index_off = 0; nh.end_off = nb;
                    memcpy(norm + 32, &nh, sizeof nh);
                    same = memcmp(pb, norm, nb) == 0;
                    for (uint64_t i = nb; same && i < cut; i++) same = pa[i] == 0;
                }
                free(norm);
            }
            free(pb);
            TEST_ASSERT_FMT(same, "order %d bare writer %d differs from the stream outside the region", o, k);
        }
    }
    PASS();
}

/* ---- P6b: the persisted DICT (spec "Writing the index", "STR dictionary index") ---- */

/* The live file's region view of column `col` of the table at ST_TMP `rel`:
 * NULL with *v filled (Task 1's validator, both tiers), or the error.
 * *m (owned) holds the file bytes, 32-aligned. */
static ray_t* st_dict_view(const char* rel, const char* col, uint8_t** m, ray_enc_dict_view_t* v) {
    char p[600];
    *m = NULL;
    if (!st_live(rel, col, p, sizeof p)) return ray_error("io", "no live file");
    ray_enc_hdr_t eh = st_hdr(p);
    if (eh.index_kind != RAY_IDX_DICT) return ray_error("domain", "index_kind %u", eh.index_kind);
    size_t n = 0; uint8_t* raw = slurp_file(p, &n);
    if (!raw) return ray_error("io", "read");
    *m = aligned_alloc(32, (n + 31) & ~(size_t)31);
    if (!*m) { free(raw); return ray_error("oom", "copy"); }
    memcpy(*m, raw, n); free(raw);
    ray_t* e = ray_enc_dict_validate(*m + eh.index_off, eh.end_off - eh.index_off,
                                     st_rows(p), p, v);
    return e ? e : ray_enc_dict_codes_check(v, NULL, p);
}

/* Generation 0's condition, per write path: 65535 rows (none), 65536
 * (DICT), a HASH column (HASH), a bare file (none), a staged save (none on
 * both generations: it builds no indexes), the test hook (none); the
 * region's codes equal generation 0's. */
static test_result_t test_idict_build_condition(void) {
    ST_Q_OK("(set S (as 'STR (map (fn [j] (format \"value % of the domain\" (% (* j 7919) 300))) (til 65536))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/c65536/\" (table [s] (list S)) {compress: true})");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/c65535/\" (table [s] (list (take S 65535))) {compress: true})");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/ch/\" (table [s] (list (.idx.hash S))) {compress: true})");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/c0/\" (table [s] (list S)) {compress: false})");
    char p[600];
    TEST_ASSERT_TRUE(st_live("/c65535", "s", p, sizeof p)); TEST_ASSERT_EQ_I(st_hdr(p).index_kind, 0);
    TEST_ASSERT_TRUE(st_live("/ch", "s", p, sizeof p));     TEST_ASSERT_EQ_I(st_hdr(p).index_kind, RAY_IDX_HASH);
    uint8_t* m = NULL; ray_enc_dict_view_t v;
    ray_t* e = st_dict_view("/c65536", "s", &m, &v);
    free(st_mem); st_mem = m;                          /* freed in teardown */
    if (e) st_hold(e);
    TEST_ASSERT_NULL(e);
    TEST_ASSERT_EQ_I(v.n_distinct, 300);
    /* generation 0's passenger DICT over the same data */
    ray_t* t0 = st_hold(ray_eval_str(ray_test_tmp("(.db.splayed.get \"" ST_TMP "/c0/\")")));
    TEST_ASSERT_TRUE(t0 && !RAY_IS_ERR(t0));
    ray_t* a = ray_table_col(t0, ray_sym_intern("s", 1));
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a));
    TEST_ASSERT_EQ_I(ray_index_kind(a), RAY_IDX_DICT);
    ray_index_t* x = ray_index_payload(a->index);
    TEST_ASSERT_EQ_I(x->u.dict.n_distinct, v.n_distinct);
    TEST_ASSERT_TRUE(memcmp(ray_data(x->u.dict.codes), v.codes, 65536 * 4) == 0);
    TEST_ASSERT_TRUE(memcmp(ray_data(x->u.dict.first_occ), v.first_occ, 300 * 4) == 0);
    /* a bare file: no DICT */
    ray_t* sv = st_hold(ray_eval_str("S"));
    TEST_ASSERT_EQ_I(ray_col_save_gen(sv, STP("/bare"), 2), RAY_OK);
    TEST_ASSERT_EQ_I(st_hdr(STP("/bare")).index_kind, 0);
    /* a staged save (the parted converter's leaf save): generation 0
     * attaches no DICT there, so generation 2 records none */
    ray_t* tb = st_hold(ray_eval_str("(table [s] (list S))"));
    TEST_ASSERT_TRUE(tb && !RAY_IS_ERR(tb));
    TEST_ASSERT_EQ_I(ray_splay_save_staged_bulk_mode(tb, STP("/sg0"), NULL, RAY_COMPRESS_OFF), RAY_OK);
    TEST_ASSERT_EQ_I(ray_splay_save_staged_bulk_mode(tb, STP("/sg2"), NULL, RAY_COMPRESS_ON), RAY_OK);
    ray_t* s0 = st_hold(ray_eval_str(ray_test_tmp("(.db.splayed.get \"" ST_TMP "/sg0/\")")));
    TEST_ASSERT_TRUE(s0 && !RAY_IS_ERR(s0));
    ray_t* a0 = ray_table_col(s0, ray_sym_intern("s", 1));
    TEST_ASSERT_TRUE(a0 && !RAY_IS_ERR(a0));
    TEST_ASSERT_EQ_I(ray_index_kind(a0), RAY_IDX_NONE);
    TEST_ASSERT_TRUE(st_live("/sg2", "s", p, sizeof p));
    const ray_enc_hdr_t sh = st_hdr(p);
    TEST_ASSERT_TRUE(memcmp(sh.magic, "RENC", 4) == 0);   /* a generation-2 file */
    TEST_ASSERT_EQ_I(sh.index_kind, 0);
    /* the hook suppresses the build */
    ray_col_test_no_dict(true);
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/cn/\" (table [s] (list S)) {compress: true})");
    ray_col_test_no_dict(false);
    TEST_ASSERT_TRUE(st_live("/cn", "s", p, sizeof p)); TEST_ASSERT_EQ_I(st_hdr(p).index_kind, 0);
    PASS();
}

/* A generation-2 table whose column records a DICT, resaved through a
 * path without index building, records none; resaved with index building,
 * it records the same region bytes. */
static test_result_t test_idict_not_inherited(void) {
    ST_Q_OK("(set S (as 'STR (map (fn [j] (format \"value % of the domain\" (% (* j 7919) 300))) (til 70000))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/n1/\" (table [s] (list S)) {compress: true})");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/n2/\" (.db.splayed.get \"" ST_TMP "/n1/\") {compress: true})");
    char p1[600], p2[600];
    TEST_ASSERT_TRUE(st_live("/n1", "s", p1, sizeof p1));
    TEST_ASSERT_TRUE(st_live("/n2", "s", p2, sizeof p2));
    TEST_ASSERT_EQ_I(st_hdr(p1).index_kind, RAY_IDX_DICT);
    size_t a = 0, b = 0;
    uint8_t* fa = slurp_file(p1, &a); uint8_t* fb = slurp_file(p2, &b);
    const bool same = fa && fb && a == b && memcmp(fa, fb, a) == 0;
    free(fa); free(fb);
    TEST_ASSERT_TRUE(same);
    /* the column writer without the save's decision: ray_col_save_ctx with kind 0 */
    ray_t* t = st_hold(ray_eval_str(ray_test_tmp("(.db.splayed.get \"" ST_TMP "/n1/\")")));
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* enc = ray_table_get_col_enc(t, ray_sym_intern("s", 1));
    TEST_ASSERT_NOT_NULL(enc);
    ray_col_write_t w; memset(&w, 0, sizeof w);
    TEST_ASSERT_NULL(ray_col_src_bind(enc, &w.src));
    w.name = "s"; w.gen = 2;
    const ray_err_t se = ray_col_save_ctx(&w, STP("/inh"));
    ray_col_src_release(&w.src);
    TEST_ASSERT_EQ_I(se, RAY_OK);
    TEST_ASSERT_EQ_I(st_hdr(STP("/inh")).index_kind, 0);
    TEST_ASSERT_EQ_U(st_hdr(STP("/inh")).index_off, 0);
    PASS();
}

/* Load rules: kind 7 on a non-STR column, and kind 7 without a region. */
static test_result_t test_idict_load_rules(void) {
    ST_Q_OK("(set S (as 'STR (map (fn [j] (format \"value % of the domain\" (% (* j 7919) 300))) (til 70000))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/l/\" (table [s] (list S)) {compress: true})");
    char p[600]; TEST_ASSERT_TRUE(st_live("/l", "s", p, sizeof p));
    ray_enc_hdr_t eh = st_hdr(p);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_DICT);
    uint64_t zero = 0;
    ST_PATCH(p, 32 + 32, &zero, 8);                 /* index_off 0 with kind 7 */
    TEST_ASSERT_STR_EQ(st_mat(p), "corrupt");
    TEST_ASSERT_FMT(strstr(st_msg, "index kind 7 disagrees") != NULL, "message \"%s\"", st_msg);
    ST_PATCH(p, 32 + 32, &eh.index_off, 8);         /* restore */
    ray_t* iv = st_hold(ray_vec_new(RAY_I64, 70000));
    iv->len = 70000;
    for (int i = 0; i < 70000; i++) ((int64_t*)ray_data(iv))[i] = i;
    TEST_ASSERT_EQ_I(ray_col_save_gen(iv, STP("/i"), 2), RAY_OK);
    /* an I64 file recording kind 7 with index_off 0: the type rule fires first */
    uint8_t k7 = RAY_IDX_DICT;
    ST_PATCH(STP("/i"), 32 + 6, &k7, 1);
    TEST_ASSERT_STR_EQ(st_mat(STP("/i")), "corrupt");
    TEST_ASSERT_FMT(strstr(st_msg, "DICT") != NULL, "message \"%s\"", st_msg);
    PASS();
}

/* ---- P6b Task 3: the persisted DICT's reader ---- */

/* Real text's shape: all-"" vectors (CONST), FSST vectors, STR_DICT
 * vectors and "" rows in one column, over 3 chunks.  P6's runtime DICT
 * never covered it; the persisted DICT equals generation 0's.  Chunk 1
 * is high-cardinality text (FSST: not a dictionary chunk by the 7/8 gain
 * rule) but for its all-"" vectors; chunks 0 and 2 are dictionary chunks. */
static const char* gen_realtext(int64_t i, char* b) {
    const int64_t v = i / 1024;
    if (v % 7 == 3) return "";                                          /* an all-"" vector */
    if (i % 13 == 0) return "";
    return v / 64 == 1 ? gen_hc(i, b) : gen_lowcard(i, b);
}
static test_result_t test_idict_mixed_encodings(void) {
    ray_t* v = st_hold(str_col(3 * 65536, gen_realtext));
    ray_t* tb = st_hold(ray_table_new(1));
    ray_t* tb2 = ray_table_add_col(tb, ray_sym_intern("s", 1), v);
    st_unhold(tb, tb2); tb = tb2;
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("W", 1), tb), RAY_OK);
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/mx0/\" W {compress: false})");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/mx2/\" W {compress: true})");
    char p[600]; TEST_ASSERT_TRUE(st_live("/mx2", "s", p, sizeof p));
    int encs[16] = { 0 };
    for (int64_t vv = 0; vv < 3 * 64; vv++) encs[st_desc_enc(p, vv, NULL) & 15]++;
    TEST_ASSERT_FMT(encs[RAY_ENC_CONST] > 0 && encs[RAY_ENC_FSST] > 0 && encs[RAY_ENC_STR_DICT] > 0,
                    "CONST %d FSST %d STR_DICT %d", encs[RAY_ENC_CONST], encs[RAY_ENC_FSST], encs[RAY_ENC_STR_DICT]);
    ray_t* t0 = st_hold(ray_eval_str(ray_test_tmp("(.db.splayed.get \"" ST_TMP "/mx0/\")")));
    ray_t* t2 = st_hold(ray_eval_str(ray_test_tmp("(.db.splayed.get \"" ST_TMP "/mx2/\")")));
    TEST_ASSERT_TRUE(t0 && !RAY_IS_ERR(t0) && t2 && !RAY_IS_ERR(t2));
    int64_t sym = ray_sym_intern("s", 1);
    ray_t* a = ray_table_col(t0, sym);
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a) && ray_index_kind(a) == RAY_IDX_DICT);
    ray_index_t* x = ray_index_payload(a->index);
    ray_enccol_t* e = ray_enccol_of(ray_table_get_col_enc(t2, sym));
    TEST_ASSERT_NOT_NULL(e);
    const ray_enc_idict_t* d = NULL;
    TEST_ASSERT_NULL(ray_enccol_idict(e, &d));
    TEST_ASSERT_TRUE(d && d->n_distinct == x->u.dict.n_distinct);
    TEST_ASSERT_TRUE(memcmp(d->first_occ, ray_data(x->u.dict.first_occ), (size_t)d->n_distinct * 4) == 0);
    ray_t* cv = NULL;
    TEST_ASSERT_NULL(ray_enccol_idict_codes_vec(e, &cv));
    TEST_ASSERT_TRUE(memcmp(ray_data(cv), ray_data(x->u.dict.codes), (size_t)3 * 65536 * 4) == 0);
    TEST_ASSERT_TRUE(d->ent->null_code != UINT32_MAX);                  /* "" is an entry */
    PASS();
}

/* Forged regions, one tier at a time: the entry tier fails the DICT's
 * every use; a code-tier forgery fails materialization and the code-tier
 * vector, while a vector-scope read of other rows succeeds; an
 * out-of-range code fails the vector-scope read that covers it. */
static test_result_t test_idict_forged(void) {
    ST_Q_OK("(set S (as 'STR (map (fn [j] (format \"value % of the domain\" (% (* j 7919) 300))) (til 70000))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/f/\" (table [s] (list S)) {compress: true})");
    char p[600]; TEST_ASSERT_TRUE(st_live("/f", "s", p, sizeof p));
    ray_enc_hdr_t eh = st_hdr(p);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_DICT);
    ray_enc_idx_dict_t dd; ST_READ(p, eh.index_off + 16, &dd, sizeof dd);
    size_t n0; uint8_t* good = slurp_file(p, &n0);
    TEST_ASSERT_NOT_NULL(good);
    free(st_mem); st_mem = good;
    /* (a) entry tier: first_occ[1] = 0 (not ascending) */
    int32_t zero = 0;
    ST_PATCH(p, eh.index_off + dd.first_occ + 4, &zero, 4);
    TEST_ASSERT_STR_EQ(st_mat(p), "corrupt");
    TEST_ASSERT_FMT(strstr(st_msg, "first occurrence of code 1") != NULL && strstr(st_msg, p), "%s", st_msg);
    ray_t* c = st_hold(ray_col_mmap(p));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    const ray_enc_idict_t* d = NULL;
    ray_t* e1 = ray_enccol_idict(ray_enccol_of(c), &d);
    TEST_ASSERT_TRUE(e1 && strcmp(ray_err_code(e1), "corrupt") == 0); ray_error_free(e1);
    TEST_ASSERT_NULL(d);
    ray_t* e2 = ray_enccol_idict(ray_enccol_of(c), &d);              /* cached */
    TEST_ASSERT_TRUE(e2 && strcmp(ray_err_code(e2), "corrupt") == 0); ray_error_free(e2);
    const int32_t* cd = NULL;
    ray_t* e2b = ray_enccol_idict_codes(ray_enccol_of(c), 0, 1024, &cd);
    TEST_ASSERT_TRUE(e2b && strcmp(ray_err_code(e2b), "corrupt") == 0 && cd == NULL); ray_error_free(e2b);
    st_unhold(c, NULL); ray_release(c);
    ST_PATCH(p, 0, good, n0);                                         /* restore */
    /* (b) code tier: row 5 takes code 299, whose first occurrence is row 299
     * ((j * 7919) % 300 visits every value in its first 300 rows) */
    int32_t late = 299;
    ST_PATCH(p, eh.index_off + dd.codes + 5 * 4, &late, 4);
    TEST_ASSERT_STR_EQ(st_mat(p), "corrupt");
    TEST_ASSERT_FMT(strstr(st_msg, "before its first occurrence") != NULL, "%s", st_msg);
    c = st_hold(ray_col_mmap(p));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_t* e = ray_enccol_of(c);
    TEST_ASSERT_NULL(ray_enccol_idict_codes(e, 0, 1024, &cd));        /* row 5 read: its code is < n */
    TEST_ASSERT_EQ_I(cd[5], 299);
    TEST_ASSERT_NULL(ray_enccol_idict_codes(e, 4096, 1024, &cd));
    ray_t* cv = NULL;
    ray_t* e3 = ray_enccol_idict_codes_vec(e, &cv);
    TEST_ASSERT_TRUE(e3 && strcmp(ray_err_code(e3), "corrupt") == 0); ray_error_free(e3);
    TEST_ASSERT_NULL(cv);
    st_unhold(c, NULL); ray_release(c);
    ST_PATCH(p, 0, good, n0);
    /* (c) an out-of-range code fails the range read covering it */
    int32_t big = 300;
    ST_PATCH(p, eh.index_off + dd.codes + 9000 * 4, &big, 4);
    c = st_hold(ray_col_mmap(p));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    e = ray_enccol_of(c);
    TEST_ASSERT_NULL(ray_enccol_idict_codes(e, 0, 1024, &cd));
    ray_t* e4 = ray_enccol_idict_codes(e, 8192, 1024, &cd);
    TEST_ASSERT_TRUE(e4 && strcmp(ray_err_code(e4), "corrupt") == 0 && cd == NULL);
    const bool at9000 = strstr(ray_error_msg() ? ray_error_msg() : "", "at row 9000") != NULL;
    ray_error_free(e4);
    TEST_ASSERT_TRUE(at9000);
    /* out of the column's rows: range, never a read past the codes */
    ray_t* e5 = ray_enccol_idict_codes(e, 69999, 2, &cd);
    TEST_ASSERT_TRUE(e5 && strcmp(ray_err_code(e5), "range") == 0); ray_error_free(e5);
    PASS();
}

/* ---- P6b Task 4: group-by on DICT codes ---- */

/* s's own decoded bytes (the per-column counter) on the loaded table T */
static uint64_t st_col_decoded(const char* col) {
    ray_t* t = ray_eval_str("T");
    if (!t || RAY_IS_ERR(t)) { if (t) ray_error_free(t); return UINT64_MAX; }
    ray_enccol_t* e = ray_enccol_of(ray_table_get_col_enc(t, ray_sym_intern(col, strlen(col))));
    uint64_t n = e ? atomic_load(&e->decoded_bytes) : UINT64_MAX;
    ray_release(t);
    return n;
}

/* q18 / q34 / q35 / composite / selection shapes on a DICT column: the
 * answers equal generation 0 (by asc: unordered group output is
 * unspecified), nothing materialized.  Decoded bytes: the key-only shapes
 * decode nothing at all; the shapes reading u (I64) or f (U8) by range
 * decode those columns' vectors, so for every shape the key column's own
 * counter (e->decoded_bytes) stays 0. */
static test_result_t test_idict_groupby(void) {
    ST_SAVE2("gb", "(table [s u f] (list (as 'STR (map (fn [j] (if (== 3 (% (div j 1024) 7)) \"\" "
             "(if (== 1 (% (div j 1024) 5)) (format \"https://e.org/%/%\" j (* j 31)) (format \"category number % long\" (% j 37))))) (til 140000))) "
             "(% (til 140000) 11) (as 'U8 (% (til 140000) 3))))");
    char p[600]; TEST_ASSERT_TRUE(st_live("/gb2", "s", p, sizeof p));
    TEST_ASSERT_EQ_I(st_hdr(p).index_kind, RAY_IDX_DICT);
    static const struct { const char* q; bool key_only; } qs[] = {
        { "(select {n: (count s) from: T by: s asc: s})", true },                     /* count s by s (q34) */
        { "(select {one: 1 s: s n: (count s) from: T by: s asc: s})", true },         /* q35's shape */
        { "(select {n: (count u) from: T by: s asc: s})", true },                     /* count reads no u value */
        { "(select {n: (count u) from: T by: {u: u s: s} asc: [u s]})", false },      /* composite (q18) */
        { "(select {n: (count u) from: T where: (== f 1) by: s asc: s})", false },    /* under a selection */
    };
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        st_fk_t k = { 0 };
        ST_SAME("gb", qs[i].q, &k);
        TEST_ASSERT_FMT(k.mat == 0, "%s: materialized %llu", qs[i].q, (unsigned long long)k.mat);
        if (qs[i].key_only)
            TEST_ASSERT_FMT(k.dec == 0, "%s: decoded %llu", qs[i].q, (unsigned long long)k.dec);
        else
            TEST_ASSERT_FMT(k.dec > 0, "%s: u / f read without a decode", qs[i].q);
        const uint64_t sd = st_col_decoded("s");
        TEST_ASSERT_FMT(sd == 0, "%s: s decoded %llu", qs[i].q, (unsigned long long)sd);
    }
    /* an aggregate other than count reading the key: generation 0's rule
     * declines the codes; the answer still equals generation 0 */
    ST_SAME("gb", "(select {m: (max (strlen s)) from: T by: s asc: s})", NULL);
    /* a count-distinct beside the key: the stashed result codes' consumer */
    ST_SAME("gb", "(select {d: (count (distinct u)) from: T by: s asc: s})", NULL);
    /* the counter sees s's range decodes: a filter decodes the URL
     * vectors (not STR_DICT) by range */
    ST_SAME("gb", "(count (select {from: T where: (== s \"https://e.org/1024/31744\")}))", NULL);
    TEST_ASSERT_TRUE(st_col_decoded("s") > 0);
    /* Task 3 review Minor 5: by: s and distinct on the DICT column equal
     * generation 0 (sorted) */
    ST_SAME("gb", "(select {from: T by: s asc: s})", NULL);
    ST_SAME("gb", "(asc (distinct (at T 's)))", NULL);
    ST_SAME("gb", "(count (distinct (at T 's)))", NULL);
    PASS();
}

/* Parted: segments with conflicting codes (the same string, different
 * codes; the same code, different strings), one segment below 65536 rows
 * (no DICT, STR_DICT vectors): generation-0 answers.  An aggregate reading
 * the key (count s) declines the string-domain swap (strdom_key_ok, P6's
 * rule, COUNT included) and is compared on answers only; count v keeps
 * the key on the remapped codes. */
static test_result_t test_idict_groupby_parted(void) {
    ST_Q_OK("(set P0 (table [s v] (list (as 'STR (map (fn [j] (format \"k%\" (% j 50))) (til 70000))) (til 70000))))");
    ST_Q_OK("(set P1 (table [s v] (list (as 'STR (map (fn [j] (format \"k%\" (- 49 (% j 50)))) (til 70000))) (til 70000))))");
    ST_Q_OK("(set P2 (table [s v] (list (as 'STR (map (fn [j] (format \"z%\" (% j 7))) (til 1000))) (til 1000))))");
    for (int g = 0; g <= 2; g += 2)
        for (int p = 0; p < 3; p++) {
            char q[512];
            snprintf(q, sizeof q, "(.db.splayed.set \"" ST_TMP "/gp%d/2024.01.0%d/t/\" P%d {compress: %s})",
                     g, p + 1, p, g ? "true" : "false");
            ST_Q_OK(q);
        }
    char p[600];
    TEST_ASSERT_TRUE(st_live("/gp2/2024.01.01/t", "s", p, sizeof p)); TEST_ASSERT_EQ_I(st_hdr(p).index_kind, RAY_IDX_DICT);
    TEST_ASSERT_TRUE(st_live("/gp2/2024.01.02/t", "s", p, sizeof p)); TEST_ASSERT_EQ_I(st_hdr(p).index_kind, RAY_IDX_DICT);
    TEST_ASSERT_TRUE(st_live("/gp2/2024.01.03/t", "s", p, sizeof p)); TEST_ASSERT_EQ_I(st_hdr(p).index_kind, 0);
    static const char* const qs[] = {
        "(select {n: (count v) from: T by: s asc: s})",
        "(select {n: (count v) from: T where: (!= s \"k3\") by: s asc: s})",
        "(select {n: (count v) from: T where: (> v 500) by: s asc: s})",
    };
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
        st_fk_t k = { 0 };
        ST_SAME_P("gp", qs[i], &k);
        TEST_ASSERT_FMT(k.mat == 0, "%s: materialized %llu", qs[i], (unsigned long long)k.mat);
        if (i == 0) TEST_ASSERT_FMT(k.dec == 0, "%s: decoded %llu", qs[i], (unsigned long long)k.dec);
    }
    ST_SAME_P("gp", "(select {n: (count s) from: T by: s asc: s})", NULL);
    ST_SAME_P("gp", "(select {n: (count s) from: T where: (!= s \"k3\") by: s asc: s})", NULL);
    /* the DICT segments are read by their codes: a code past n_distinct in
     * partition 2's region fails the group-by naming the row */
    TEST_ASSERT_TRUE(st_live("/gp2/2024.01.02/t", "s", p, sizeof p));
    ray_enc_hdr_t eh = st_hdr(p);
    ray_enc_idx_dict_t dd; ST_READ(p, eh.index_off + 16, &dd, sizeof dd);
    int32_t big = 50;
    ST_PATCH(p, eh.index_off + dd.codes + 9000 * 4, &big, 4);
    ST_Q_OK("(set T (.db.parted.get \"" ST_TMP "/gp2\" 't))");
    TEST_ASSERT_FALSE(st_q_run("(select {n: (count v) from: T by: s})"));
    TEST_ASSERT_FMT(strstr(st_msg, "DICT code 50 at row 9000 outside 0 .. 49") != NULL, "%s", st_msg);
    PASS();
}

/* A forged code tier (row 5 takes code 299 before its first occurrence)
 * fails the group-by with corrupt naming the file; the next query fails
 * the same way (cached). */
static test_result_t test_idict_groupby_forged(void) {
    ST_SAVE2("gf", "(table [s] (list (as 'STR (map (fn [j] (format \"value % of the domain\" (% (* j 7919) 300))) (til 70000)))))");
    char p[600]; TEST_ASSERT_TRUE(st_live("/gf2", "s", p, sizeof p));
    ray_enc_hdr_t eh = st_hdr(p);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_DICT);
    ray_enc_idx_dict_t dd; ST_READ(p, eh.index_off + 16, &dd, sizeof dd);
    int32_t late = 299;                         /* row 5, before code 299's first occurrence (row 299) */
    ST_PATCH(p, eh.index_off + dd.codes + 5 * 4, &late, 4);
    ST_Q_OK("(set T (.db.splayed.get \"" ST_TMP "/gf2/\"))");
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_FALSE(st_q_run("(select {n: (count s) from: T by: s})"));
        /* the file named as the load spells it: the table directory, then s */
        TEST_ASSERT_FMT(strstr(st_msg, "before its first occurrence") != NULL &&
                        strstr(st_msg, "rayforce_encstr/gf2/") != NULL && strstr(st_msg, "/s: DICT code 299 at row 5"),
                        "%s", st_msg);
    }
    PASS();
}

/* Task 1's carry: the validator leaves entry distinctness to the reader.
 * A region whose entry 2 repeats entry 1 passes ray_enc_dict_validate and
 * fails the entry tier (the hash build), so no lookup, range read or
 * materialization ever trusts its codes. */
static test_result_t test_idict_dup_entry(void) {
    ST_Q_OK("(set S (as 'STR (map (fn [j] (format \"value % of the domain\" (% (* j 7919) 300))) (til 70000))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/dup/\" (table [s] (list S)) {compress: true})");
    char p[600]; TEST_ASSERT_TRUE(st_live("/dup", "s", p, sizeof p));
    ray_enc_hdr_t eh = st_hdr(p);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_DICT);
    ray_enc_idx_dict_t dd; ST_READ(p, eh.index_off + 16, &dd, sizeof dd);
    uint64_t of[4]; ST_READ(p, eh.index_off + dd.offs, of, sizeof of);
    /* codes 1 and 2 are values 119 and 238: entries of one length */
    TEST_ASSERT_TRUE(of[2] - of[1] == of[3] - of[2]);
    char ent[64]; TEST_ASSERT_TRUE(of[2] - of[1] < sizeof ent);
    ST_READ(p, eh.index_off + dd.bytes + of[1], ent, of[2] - of[1]);
    ST_PATCH(p, eh.index_off + dd.bytes + of[2], ent, of[2] - of[1]);
    /* the validator alone accepts it */
    size_t nf; uint8_t* f = slurp_file(p, &nf);
    TEST_ASSERT_NOT_NULL(f);
    free(st_mem); st_mem = f;
    const uint64_t rsz = eh.end_off - eh.index_off;
    uint8_t* rb = aligned_alloc(32, (size_t)((rsz + 31) & ~(uint64_t)31));
    TEST_ASSERT_NOT_NULL(rb);
    memcpy(rb, f + eh.index_off, (size_t)rsz);
    ray_enc_dict_view_t dv;
    ray_t* ve = ray_enc_dict_validate(rb, rsz, 70000, p, &dv);
    free(rb);
    if (ve) ray_error_free(ve);
    TEST_ASSERT_NULL(ve);
    /* the entry tier refuses it, cached, for every use */
    TEST_ASSERT_STR_EQ(st_mat(p), "corrupt");
    TEST_ASSERT_FMT(strstr(st_msg, "DICT entries not distinct") != NULL && strstr(st_msg, p), "%s", st_msg);
    ray_t* c = st_hold(ray_col_mmap(p));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_t* e = ray_enccol_of(c);
    for (int k = 0; k < 2; k++) {
        const ray_enc_idict_t* d = (const ray_enc_idict_t*)1;
        ray_t* e1 = ray_enccol_idict(e, &d);
        TEST_ASSERT_TRUE(e1 && strcmp(ray_err_code(e1), "corrupt") == 0 && d == NULL); ray_error_free(e1);
    }
    int64_t code = -9;
    ray_t* e2 = ray_enccol_idict_code(e, ent, of[2] - of[1], &code);
    TEST_ASSERT_TRUE(e2 && strcmp(ray_err_code(e2), "corrupt") == 0 && code == -9); ray_error_free(e2);
    const int32_t* cd = NULL;
    ray_t* e3 = ray_enccol_idict_codes(e, 0, 1024, &cd);
    TEST_ASSERT_TRUE(e3 && strcmp(ray_err_code(e3), "corrupt") == 0 && cd == NULL); ray_error_free(e3);
    ray_t* cv = NULL;
    ray_t* e4 = ray_enccol_idict_codes_vec(e, &cv);
    TEST_ASSERT_TRUE(e4 && strcmp(ray_err_code(e4), "corrupt") == 0 && cv == NULL); ray_error_free(e4);
    PASS();
}

/* The runtime DICT is gone: a bare all-STR_DICT file of 70000 rows (no
 * recorded kind) materializes without a DICT, as generation 0's bare
 * ray_col_save does, and ray_enccol_may_have_index is false. */
static test_result_t test_idict_runtime_gone(void) {
    ST_Q_OK("(set S (as 'STR (map (fn [j] (format \"value % of the domain\" (% (* j 7919) 300))) (til 70000))))");
    ray_t* sv = st_hold(ray_eval_str("S"));
    TEST_ASSERT_TRUE(sv && !RAY_IS_ERR(sv));
    TEST_ASSERT_EQ_I(ray_col_save_gen(sv, STP("/rt"), 2), RAY_OK);
    for (int k = 0; k < 2; k++) TEST_ASSERT_FALSE(st_dirent(STP("/rt"), k).flags & RAY_ENC_STAT_ABSENT);  /* all STR_DICT */
    TEST_ASSERT_EQ_I(st_hdr(STP("/rt")).index_kind, 0);
    ray_t* c = st_hold(ray_col_mmap(STP("/rt")));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    TEST_ASSERT_FALSE(ray_enccol_may_have_index(c));
    const ray_enc_idict_t* d = (const ray_enc_idict_t*)1;
    TEST_ASSERT_NULL(ray_enccol_idict(ray_enccol_of(c), &d));
    TEST_ASSERT_NULL(d);
    int64_t code = 0;
    TEST_ASSERT_NULL(ray_enccol_idict_code(ray_enccol_of(c), "x", 1, &code));
    TEST_ASSERT_EQ_I(code, -2);
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_NONE);
    PASS();
}

/* Review fix 4: pool workers race the first touch of both tiers on a fresh
 * column.  Sound file: every racer sees the one entry-tier blob, the one
 * code-tier vector and the right codes (code of row i is i % 300), and the
 * racers' heaps together keep exactly what one serial first touch keeps
 * (the published blob and vector; no loser keeps a copy).  Forged files (a
 * duplicate entry: the entry tier; a late code: the code tier): every racer
 * gets the one cached corrupt verdict, and no racer's heap keeps anything
 * of the failed attempt.  Every iteration requires every racer to have
 * left the barrier with all racers arrived, so a barrier timeout (racers
 * run one after another) fails instead of passing serially. */
#define IR_SLOTS 64
typedef struct {
    ray_enccol_t*      e;
    int                mode;                   /* 0 sound, 1 entry tier forged, 2 code tier forged */
    _Atomic(int)       arrived;
    int                want;                   /* soft barrier: the pool's worker count */
    _Atomic(int)       next;
    const void*        d[IR_SLOTS];
    const void*        cv[IR_SLOTS];
    char               msg[IR_SLOTS][128];
    int                bad[IR_SLOTS];
    int64_t            leak[IR_SLOTS];         /* the racer's heap delta over its window */
    int                met[IR_SLOTS];          /* the racer left the barrier with every racer arrived */
    uint32_t           wid[IR_SLOTS];
} ir_race_t;
static void ir_race_task(void* ctx, uint32_t wid, int64_t lo, int64_t hi) {
    (void)lo; (void)hi;
    ir_race_t* x = ctx;
    const int k = atomic_fetch_add(&x->next, 1);
    if (k >= IR_SLOTS) return;
    x->wid[k] = wid;
    atomic_fetch_add(&x->arrived, 1);
    /* bounded by wall clock, not a spin count: a late wake-up (a loaded
     * machine, sanitizers) still meets the barrier; only "never arrived"
     * times out, and met[k] makes that fail the test */
    const int64_t deadline = ray_time_now_ms() + 2000;
    for (unsigned spin = 1; atomic_load(&x->arrived) < x->want; spin++)
        if (!(spin & 4095) && ray_time_now_ms() > deadline) break;
    x->met[k] = atomic_load(&x->arrived) >= x->want;   /* else a timeout: the racers ran serially */
    ray_mem_stats_t m0, m1;
    const ray_enc_idict_t* d = NULL;
    ray_t* cv = NULL;
    if (x->mode == 2) {                        /* the entry tier holds: measure the code tier alone */
        ray_t* err = ray_enccol_idict(x->e, &d);
        if (err) { x->bad[k] = 1; ray_error_free(err); return; }
        x->d[k] = d;
    }
    ray_mem_stats(&m0);
    ray_t* err = x->mode == 2 ? NULL : ray_enccol_idict(x->e, &d);
    if (!err && x->mode != 2) x->d[k] = d;
    if (!err) err = ray_enccol_idict_codes_vec(x->e, &cv);
    if (!err) {
        ray_mem_stats(&m1);
        x->leak[k] = (int64_t)m1.bytes_allocated - (int64_t)m0.bytes_allocated;
    }
    if (err) {
        snprintf(x->msg[k], sizeof x->msg[k], "%s: %s", ray_err_code(err), ray_error_msg() ? ray_error_msg() : "");
        ray_error_free(err);
        ray_mem_stats(&m1);
        x->leak[k] = (int64_t)m1.bytes_allocated - (int64_t)m0.bytes_allocated;
        return;
    }
    x->cv[k] = cv;
    const int32_t* all = (const int32_t*)ray_data(cv);
    for (int64_t i = 0; i < cv->len; i++) if (all[i] != (int32_t)(i % 300)) { x->bad[k] = 1; break; }
    const int64_t r0 = ((int64_t)k * 1000) % 69000;
    const int32_t* cd = NULL;
    ray_t* re = ray_enccol_idict_codes(x->e, r0, 1000, &cd);
    if (re) { ray_error_free(re); x->bad[k] = 1; return; }
    for (int64_t i = 0; i < 1000; i++) if (cd[i] != (int32_t)((r0 + i) % 300)) { x->bad[k] = 1; break; }
}

static test_result_t test_idict_first_touch_race(void) {
    ST_Q_OK("(set S (as 'STR (map (fn [j] (format \"value % of the domain\" (% (* j 7919) 300))) (til 70000))))");
    ST_Q_OK("(.db.splayed.set \"" ST_TMP "/ra/\" (table [s] (list S)) {compress: true})");
    char p[600]; TEST_ASSERT_TRUE(st_live("/ra", "s", p, sizeof p));
    ray_enc_hdr_t eh = st_hdr(p);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_DICT);
    ray_enc_idx_dict_t dd; ST_READ(p, eh.index_off + 16, &dd, sizeof dd);
    uint64_t of[4]; ST_READ(p, eh.index_off + dd.offs, of, sizeof of);
    TEST_ASSERT_TRUE(of[2] - of[1] == of[3] - of[2] && of[2] - of[1] < 64);
    size_t n0; uint8_t* good = slurp_file(p, &n0);
    TEST_ASSERT_NOT_NULL(good);
    free(st_mem); st_mem = good;
    ray_pool_t* pool = ray_pool_get();
    TEST_ASSERT_NOT_NULL(pool);
    const uint32_t nw = ray_pool_total_workers(pool);
    const uint32_t ntask = nw * 2 < IR_SLOTS ? nw * 2 : IR_SLOTS;
    /* What one serial first touch keeps: on this thread's buddy heap (kept)
     * and in direct mappings (kept_direct, process-wide).  The racers' per-
     * thread bytes_allocated deltas are compared with `kept`, which assumes
     * every heap rounds a block alike (true: one buddy policy for all
     * threads); direct mappings are process-wide, so they are compared once
     * per iteration around the whole dispatch (the main thread allocates
     * nothing else meanwhile). */
    int64_t kept = 0, kept_direct = 0;
    {
        ray_t* c = ray_col_mmap(p);
        TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
        ray_mem_stats_t m0, m1;
        const ray_enc_idict_t* d = NULL; ray_t* cv = NULL;
        ray_mem_stats(&m0);
        ray_t* err = ray_enccol_idict(ray_enccol_of(c), &d);
        if (!err) err = ray_enccol_idict_codes_vec(ray_enccol_of(c), &cv);
        ray_mem_stats(&m1);
        ray_release(c);
        if (err) ray_error_free(err);
        TEST_ASSERT_TRUE(err == NULL && d && cv);
        kept = (int64_t)m1.bytes_allocated - (int64_t)m0.bytes_allocated;
        kept_direct = (int64_t)m1.direct_bytes - (int64_t)m0.direct_bytes;
        TEST_ASSERT_TRUE(kept > 0);
    }
    static ir_race_t x;
    bool workers_seen = false;
    for (int mode = 0; mode < 3; mode++) {
        ST_PATCH(p, 0, good, n0);
        if (mode == 1) {                                   /* entry 2 repeats entry 1 */
            char ent[64]; ST_READ(p, eh.index_off + dd.bytes + of[1], ent, of[2] - of[1]);
            ST_PATCH(p, eh.index_off + dd.bytes + of[2], ent, of[2] - of[1]);
        } else if (mode == 2) {                            /* row 5 takes code 299 */
            int32_t late = 299;
            ST_PATCH(p, eh.index_off + dd.codes + 5 * 4, &late, 4);
        }
        for (int it = 0; it < 40; it++) {
            ray_t* c = ray_col_mmap(p);
            TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
            memset(&x, 0, sizeof x);
            x.e = ray_enccol_of(c); x.mode = mode; x.want = (int)nw;
            ray_mem_stats_t dm0, dm1;
            ray_mem_stats(&dm0);
            ray_pool_dispatch_n(pool, ir_race_task, &x, ntask);
            ray_mem_stats(&dm1);
            const int64_t held_direct = (int64_t)dm1.direct_bytes - (int64_t)dm0.direct_bytes;
            const int n = atomic_load(&x.next) < (int)ntask ? atomic_load(&x.next) : (int)ntask;
            bool ok = n == (int)ntask;
            int64_t held = 0;
            for (int k = 0; k < n && ok; k++) {
                ok = !x.bad[k] && x.met[k] && (mode == 0 ? x.leak[k] >= 0 : x.leak[k] == 0);
                held += x.leak[k];
                if (mode == 0) ok = ok && x.msg[k][0] == 0 && x.d[k] == x.d[0] && x.cv[k] == x.cv[0] &&
                                    x.cv[k] == (const void*)x.e->icodes && x.d[k] != NULL;
                else ok = ok && x.cv[k] == NULL && strcmp(x.msg[k], x.msg[0]) == 0 &&
                          strncmp(x.msg[k], "corrupt: ", 9) == 0 &&
                          strstr(x.msg[k], mode == 1 ? "DICT entries not distinct" : "before its first occurrence") &&
                          (mode == 1 ? x.d[k] == NULL : x.d[k] == x.d[0] && x.d[k] != NULL);
            }
            if (mode && ok) ok = (mode == 1 ? x.e->idict == NULL : x.e->icodes == NULL);   /* nothing published */
            if (!mode && ok) ok = held == kept && held_direct == kept_direct;   /* the one blob and vector, nothing else */
            if (mode && ok) ok = held_direct == 0;
            for (int k = 1; k < n; k++) if (x.wid[k] != x.wid[0]) { workers_seen = true; break; }
            ray_release(c);
            TEST_ASSERT_FMT(ok, "mode %d iteration %d (%d tasks, held %lld of %lld, direct %lld of %lld): %s", mode, it, n,
                            (long long)held, (long long)kept, (long long)held_direct,
                            (long long)(mode ? 0 : kept_direct), n ? x.msg[0] : "");
        }
    }
    if (pool->n_workers > 0) TEST_ASSERT_TRUE(workers_seen);   /* the racers ran on more than one worker */
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
    { "encstr/range_shared_entry", test_range_shared_entry, st_setup, st_teardown },
    { "encstr/side_rejects",       test_side_rejects,       st_setup, st_teardown },
    { "encstr/fsst_rejects",       test_fsst_rejects,       st_setup, st_teardown },
    { "encstr/bound_arith",        test_bound_arith,        st_setup, st_teardown },
    { "encstr/force_str_wins",     test_force_str_wins,     st_setup, st_teardown },
    { "encstr/stats_chokepoint",   test_stats_chokepoint,   st_q_setup, st_q_teardown },
    { "encstr/stats_chokepoint_pooled", test_stats_chokepoint_pooled, st_q_setup, st_q_teardown },
    { "encstr/dict_slot_spread",   test_dict_slot_spread,   st_setup, st_teardown },
    { "encstr/pool_order",         test_pool_order,         st_setup, st_teardown },
    { "encstr/fsst_scopes",        test_fsst_scopes,        st_setup, st_teardown },
    { "encstr/dict_equals_gen0",   test_dict_equals_gen0,   st_q_setup, st_q_teardown },
    { "encstr/dict_variants",      test_dict_variants,      st_q_setup, st_q_teardown },
    { "encstr/fsst_scopes_query",  test_fsst_scopes_query,  st_q_setup, st_q_teardown },
    { "encstr/idict_forged",       test_idict_forged,       st_q_setup, st_q_teardown },
    { "encstr/dict_inband_nulls",  test_dict_inband_nulls,  st_q_setup, st_q_teardown },
    { "encstr/dict_oom_retry",     test_dict_oom_retry,     st_q_setup, st_q_teardown },
    { "encstr/filter_strdict",     test_filter_strdict,     st_q_setup, st_q_teardown },
    { "encstr/filter_decided",     test_filter_decided,     st_q_setup, st_q_teardown },
    { "encstr/filter_chained",     test_filter_chained,     st_q_setup, st_q_teardown },
    { "encstr/filter_parted",      test_filter_parted,      st_q_setup, st_q_teardown },
    { "encstr/filter_mixed_chunks", test_filter_mixed_chunks, st_q_setup, st_q_teardown },
    { "encstr/filter_materialized", test_filter_materialized, st_q_setup, st_q_teardown },
    { "encstr/filter_forged",      test_filter_forged,      st_q_setup, st_q_teardown },
    { "encstr/remap_hash_parity",  test_remap_hash_parity,  st_q_setup, st_q_teardown },
    { "encstr/cross_segment_codes", test_cross_segment_codes, st_q_setup, st_q_teardown },
    { "encstr/groupby_selection",  test_groupby_selection,  st_q_setup, st_q_teardown },
    { "encstr/mixed_str_groupby",  test_mixed_str_groupby,  st_q_setup, st_q_teardown },
    { "encstr/groupby_forged",     test_groupby_forged,     st_q_setup, st_q_teardown },
    { "encstr/groupby_remap_oom",  test_groupby_remap_oom,  st_q_setup, st_q_teardown },
    { "encstr/abandonment",               test_abandonment,               st_setup,   st_teardown },
    { "encstr/representation_not_value",  test_representation_not_value,  st_setup,   st_teardown },
    { "encstr/stream_equals_resident",    test_stream_equals_resident,    st_q_setup, st_q_teardown },
    { "encstr/idict_build_condition", test_idict_build_condition, st_q_setup, st_q_teardown },
    { "encstr/idict_not_inherited",   test_idict_not_inherited,   st_q_setup, st_q_teardown },
    { "encstr/idict_load_rules",      test_idict_load_rules,      st_q_setup, st_q_teardown },
    { "encstr/idict_mixed_encodings", test_idict_mixed_encodings, st_q_setup, st_q_teardown },
    { "encstr/idict_dup_entry",       test_idict_dup_entry,       st_q_setup, st_q_teardown },
    { "encstr/idict_runtime_gone",    test_idict_runtime_gone,    st_q_setup, st_q_teardown },
    { "encstr/idict_first_touch_race", test_idict_first_touch_race, st_q_setup, st_q_teardown },
    { "encstr/idict_groupby",        test_idict_groupby,        st_q_setup, st_q_teardown },
    { "encstr/idict_groupby_parted", test_idict_groupby_parted, st_q_setup, st_q_teardown },
    { "encstr/idict_groupby_forged", test_idict_groupby_forged, st_q_setup, st_q_teardown },
    { NULL, NULL, NULL, NULL },
};
