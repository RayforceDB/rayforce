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
#include "ops/idxop.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The generation-2 corruption suite.  Each test crafts exactly one rule
 * violation on an otherwise valid file and asserts the error code and a
 * message needle naming the rule that fired (and the column path), so a
 * rule that stops firing, or a different rule firing in its place, fails
 * the test.  Organized by the spec's "Validation" list: headers, directory,
 * side data, descriptors, payloads, codec grammar, lazy validation, fuzz.
 * Index region parsing has its own suite (idxval/); the fuzz below also
 * flips an indexed column's region. */

/* structural flips (headers, directory, descriptors) must be rejected at least this often; measured 94.8% */
#define STRUCT_MIN_PCT 90
#define EC_TMP "/tmp/rayforce_enccorrupt"
static void ec_setup(void) {
    ray_heap_init(); (void)ray_sym_init(); (void)ray_test_mkdir_p(ray_test_tmp(EC_TMP));
    for (int i = 0; i < 256; i++) { char b[8]; int n = snprintf(b, sizeof b, "s%d", i); (void)ray_sym_intern(b, (size_t)n); }
}
static uint8_t* g_img; static size_t g_len;
static void ec_teardown(void) { ray_test_hooks_off(); free(g_img); g_img = NULL; ray_sym_destroy(); ray_heap_destroy(); (void)ray_test_rm_rf(ray_test_tmp(EC_TMP)); }

static bool img_load(const char* p) {
    free(g_img); g_img = NULL; g_len = 0;
    FILE* f = fopen(p, "rb"); if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f); if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    g_len = (size_t)sz; g_img = malloc(g_len ? g_len : 1);
    bool ok = g_img && fread(g_img, 1, g_len, f) == g_len;
    fclose(f);
    return ok;
}
static bool img_store(const char* p, size_t n) {
    FILE* f = fopen(p, "wb"); if (!f) return false;
    bool ok = fwrite(g_img, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static uint64_t rd64(size_t off) { uint64_t x; memcpy(&x, g_img + off, 8); return x; }
static void wr(size_t off, const void* b, size_t n) { memcpy(g_img + off, b, n); }
static void W8(size_t off, uint8_t v)   { wr(off, &v, 1); }
static void W16(size_t off, uint16_t v) { wr(off, &v, 2); }
static void W32(size_t off, uint32_t v) { wr(off, &v, 4); }
static void W64(size_t off, uint64_t v) { wr(off, &v, 8); }

#define DIR(c) (96 + 64 * (size_t)(c))                          /* directory entry c */
#define EH 32                                                   /* enc header */
static size_t desc_at(int c, int j) { return (size_t)rd64(DIR(c)) + 48 * (size_t)j; }
static size_t payload_at(int c, int j) { return (size_t)rd64(DIR(c)) + (size_t)rd64(desc_at(c, j) + 8); }
static size_t chunk_end(int c) { return (size_t)(rd64(DIR(c)) + rd64(DIR(c) + 8)); }

/* Resize the last chunk (nothing follows it) to `nb` bytes, keeping the end offset consistent. */
static void set_chunk_bytes(int c, uint64_t nb) {
    size_t nl = (size_t)rd64(DIR(c)) + (size_t)nb;
    uint8_t* q = realloc(g_img, nl); if (q) g_img = q;
    if (nl > g_len) memset(g_img + g_len, 0, nl - g_len);
    g_len = nl;
    W64(DIR(c) + 8, nb); W64(EH + 56, nl);
}
/* Replace everything after the chunks by a side data region of `n` bytes. */
static void put_side(const uint8_t* bytes, size_t n) {
    size_t prev = chunk_end(0);
    size_t off = (prev + 31) & ~(size_t)31;
    uint8_t* q = realloc(g_img, off + n); if (q) g_img = q;
    memset(g_img + prev, 0, off - prev + n);
    memcpy(g_img + off, bytes, n);
    g_len = off + n;
    W64(EH + 16, off); W64(EH + 24, n); W64(EH + 56, g_len);
}
/* Side table of `ne` entries (kind, offset, length) followed by `total` bytes. */
static void side_table(uint8_t* buf, size_t total, int ne, const uint16_t* kind, const uint64_t* off, const uint64_t* len) {
    memset(buf, 0, total);
    uint32_t n = (uint32_t)ne; memcpy(buf, &n, 4);
    for (int i = 0; i < ne; i++) {
        memcpy(buf + 8 + 24 * i, &kind[i], 2);
        memcpy(buf + 8 + 24 * i + 8, &off[i], 8);
        memcpy(buf + 8 + 24 * i + 16, &len[i], 8);
    }
}

/* every generation-2 reader message opens "column <path>:" */
static bool names_path(const char* path) {
    const char* m = ray_error_msg();
    return m && strncmp(m, "column ", 7) == 0 && strstr(m, path) != NULL;
}
static bool has_needle(const char* needle) {
    const char* m = ray_error_msg();
    return m && strstr(m, needle) != NULL;
}

/* Expect `code` at load (stage 0), at materialization (stage 1) or at a
 * range read of the whole column (stage 2); the message must name the file
 * and contain `needle`. */
static const char* expect(const char* path, int stage, const char* code, const char* needle) {
    static char why[512];
    ray_error_clear();
    ray_t* c = ray_col_mmap(path);
    if (stage == 0) {
        if (!c || !RAY_IS_ERR(c)) { if (c && !RAY_IS_ERR(c)) ray_release(c); snprintf(why, sizeof why, "loaded, want %s [%s]", code, needle); return why; }
        bool ok = strcmp(ray_err_code(c), code) == 0 && names_path(path) && has_needle(needle);
        snprintf(why, sizeof why, "load gave %s: \"%s\", want %s [%s]", ray_err_code(c), ray_error_msg() ? ray_error_msg() : "", code, needle);
        ray_error_free(c);
        return ok ? NULL : why;
    }
    if (!c || RAY_IS_ERR(c)) {
        snprintf(why, sizeof why, "load failed (%s: %s), want %s at stage %d [%s]", c ? ray_err_code(c) : "null",
                 ray_error_msg() ? ray_error_msg() : "", code, stage, needle);
        if (c) ray_error_free(c);
        return why;
    }
    ray_t* m;
    ray_col_src_t src; ray_col_scratch_t sc;
    if (stage == 1) {
        m = ray_enccol_materialize(c);
    } else {
        m = ray_col_src_bind(c, &src);
        if (!m) {
            ray_col_scratch_init(&sc);
            ray_col_range_t r;
            for (int64_t at = 0; at < src.len && !m; at += RAY_ENC_VEC) {
                int64_t n = src.len - at < RAY_ENC_VEC ? src.len - at : RAY_ENC_VEC;
                m = ray_col_range(&src, at, n, &sc, &r);
            }
            ray_col_scratch_free(&sc); ray_col_src_release(&src);
        }
    }
    bool is_err = m && RAY_IS_ERR(m);
    bool ok = is_err && strcmp(ray_err_code(m), code) == 0 && names_path(path) && has_needle(needle);
    snprintf(why, sizeof why, "stage %d gave %s: \"%s\", want %s [%s]", stage,
             is_err ? ray_err_code(m) : "a column", is_err && ray_error_msg() ? ray_error_msg() : "", code, needle);
    if (is_err) ray_error_free(m);
    ray_release(c);
    return ok ? NULL : why;
}

#define EXPECT(path, stage, code, needle) do { const char* w_ = expect(path, stage, code, needle); \
    TEST_ASSERT_FMT(w_ == NULL, "%s:%d %s", __FILE__, __LINE__, w_); } while (0)
/* write the patched image and expect */
#define BAD(path, stage, code, needle) do { TEST_ASSERT_TRUE(img_store(path, g_len)); EXPECT(path, stage, code, needle); } while (0)

static void save_vec(ray_t* v, const char* p) {
    ray_err_t e = ray_col_save_gen(v, p, 2); ray_release(v);
    if (e != RAY_OK) { free(g_img); g_img = NULL; g_len = 0; return; }
    (void)img_load(p);
}
#define SAVE(v, p) do { save_vec((v), (p)); TEST_ASSERT_TRUE(g_img != NULL); } while (0)

#define NEED_ENC(c, j, enc) TEST_ASSERT_FMT(g_img[desc_at(c, j)] == (enc), \
    "descriptor %d.%d is encoding %d, the test pins %d", c, j, g_img[desc_at(c, j)], enc)

/* ---- fixtures (each pinned by hand against ray_enc_choose and asserted before any patch) ---- */

/* BOOL runs of 100 -> top-level RLE, 64 B payload: [u32 11][u32 0]
 * [values hdr: RAW, body 16 @+8][values @+16][ends hdr: RAW, body 24 @+32][ends U16 @+40]. */
static ray_t* bool_runs(void) {
    ray_t* v = ray_vec_new(RAY_BOOL, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((uint8_t*)ray_data(v))[i] = (uint8_t)((i / 100) & 1);
    return v;
}
static ray_t* bool_const(void) {
    ray_t* v = ray_vec_new(RAY_BOOL, 1024); v->len = 1024; memset(ray_data(v), 0, 1024);
    return v;
}
/* Four wide values at hash-scattered rows -> DICT, FOR_BP w=2 codes child, VALUE_DICT side entry. */
static ray_t* i64_dict(void) {
    static const int64_t vals[4] = { 11, 900000000001ll, 77, 123456789012345ll };
    ray_t* v = ray_vec_new(RAY_I64, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((int64_t*)ray_data(v))[i] = vals[(uint32_t)((uint32_t)i * 2654435761u) >> 30];
    return v;
}
/* Three wide values: an I32 VALUE_DICT of 12 bytes plus 4 bytes of padding. */
static ray_t* i32_dict3(void) {
    static const int32_t vals[3] = { 11, 900000001, 77 };
    ray_t* v = ray_vec_new(RAY_I32, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((int32_t*)ray_data(v))[i] = vals[((uint32_t)((uint32_t)i * 2654435761u) >> 30) % 3];
    return v;
}
/* 100 scattered 32-bit values, a null at every tenth row: RAW with a 128-byte bitmap first. */
static ray_t* i32_nulls_raw(void) {
    ray_t* v = ray_vec_new(RAY_I32, 100); v->len = 100;
    for (int i = 0; i < 100; i++) ((int32_t*)ray_data(v))[i] = i % 10 ? (int32_t)((uint32_t)i * 2654435761u >> 1) : NULL_I32;
    return v;
}
static ray_t* i32_for(void) {
    ray_t* v = ray_vec_new(RAY_I32, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((int32_t*)ray_data(v))[i] = 1000 + (int32_t)(((uint32_t)i * 2654435761u) >> 27);
    return v;
}
/* Rising with a jitter: lane deltas stay narrow, so DELTA_BP wins. */
static ray_t* i32_delta(void) {
    ray_t* v = ray_vec_new(RAY_I32, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((int32_t*)ray_data(v))[i] = i * 3 + (i * 7) % 5;
    return v;
}
static ray_t* i32_allnull(void) {
    ray_t* v = ray_vec_new(RAY_I32, 10); v->len = 10;
    for (int i = 0; i < 10; i++) ((int32_t*)ray_data(v))[i] = NULL_I32;
    return v;
}
static ray_t* f64_plain(void) {
    double d[4] = { 1.0, 2.0, 3.0, 4.0 };
    return ray_vec_from_raw(RAY_F64, d, 4);
}
static ray_t* f64_stats(void) {
    ray_t* f = ray_vec_new(RAY_F64, 100); f->len = 100;
    for (int i = 0; i < 100; i++) ((double*)ray_data(f))[i] = i;
    return f;
}
/* Two non-canonical NaNs at rows 0 and 1: bitmap bits 0,1 and a trailer of two. */
static ray_t* f64_two_odd(void) {
    uint64_t a = 0x7FF8000000000001ull, b = 0x7FF8000000000002ull; double d[4] = { 0, 0, 1.0, 2.0 };
    memcpy(&d[0], &a, 8); memcpy(&d[1], &b, 8);
    return ray_vec_from_raw(RAY_F64, d, 4);
}
static ray_t* f32_odd(void) {
    uint32_t odd = 0x7FC00001u; float d[4] = { 2.5f, NULL_F32, 0, -1.0f };
    memcpy(&d[2], &odd, 4);
    return ray_vec_from_raw(RAY_F32, d, 4);
}
static ray_t* f32_plain(void) {
    float d[3] = { 1.0f, 2.0f, 3.0f };
    return ray_vec_from_raw(RAY_F32, d, 3);
}
static ray_t* f32_const(void) {
    ray_t* v = ray_vec_new(RAY_F32, 64); v->len = 64;
    for (int i = 0; i < 64; i++) ((float*)ray_data(v))[i] = 2.5f;
    return v;
}
static ray_t* guid_raw(void) {
    ray_t* g = ray_vec_new(RAY_GUID, 4); g->len = 4;
    for (int i = 0; i < 64; i++) ((uint8_t*)ray_data(g))[i] = (uint8_t)(i * 37 + 1);
    return g;
}
static ray_t* guid_const(void) {
    ray_t* g = ray_vec_new(RAY_GUID, 4); g->len = 4; memset(ray_data(g), 5, 64);
    return g;
}
/* "short" inline (descriptor 0, bytes 0..15), a pooled string (descriptor 1, bytes 16..31;
 * pool_off at 24, hash32 at 28), pool length @+32, pool @+40. */
static ray_t* str_mixed(void) {
    ray_t* s = ray_vec_new(RAY_STR, 2);
    s = ray_str_vec_append(s, "short", 5);
    return ray_str_vec_append(s, "a pooled string here", 20);
}
static ray_t* str_const(void) {
    ray_t* s = ray_vec_new(RAY_STR, 8);
    for (int i = 0; i < 8; i++) s = ray_str_vec_append(s, "a constant pooled string!", 25);
    return s;
}
/* A SYM vector of `n` ids written with saved count 200. */
static void save_sym200(const uint8_t* ids, int n, const char* p) {
    FILE* f = fopen(p, "wb+");
    if (!f) ray_test_fatal("cannot open %s", p);
    ray_encw_t w;
    (void)ray_encw_begin(&w, f, RAY_SYM, RAY_SYM_W8, n, 200, 0, 0);
    (void)ray_encw_chunk(&w, ids, NULL, n);
    (void)ray_encw_finish(&w);
    ray_encw_free(&w); fclose(f);
    (void)img_load(p);
}
#define SAVE_SYM(ids, n, p) do { save_sym200((ids), (n), (p)); TEST_ASSERT_TRUE(g_img != NULL); } while (0)

/* ---- outer header, enc header ---- */
static test_result_t test_reject_headers(void) {
    const char* p = ray_test_tmp(EC_TMP "/h");
    /* (order 0, "XENC"), (order 2, "XIDX"), (order 2, zero aux): encfmt/discriminator_cross_pairs;
     * version 2, truncation: enccol/load_rejects. Here: every other header rule. */
    SAVE(bool_runs(), p); W8(18, 0);              BAD(p, 0, "corrupt", "not storable in generation 2");
    SAVE(bool_runs(), p); W8(19, 1);              BAD(p, 0, "corrupt", "width bits on a non-SYM column");
    SAVE(bool_runs(), p); W8(19, 0x08);           BAD(p, 0, "corrupt", "outside the persisted set");
    SAVE(bool_runs(), p); W32(20, 5);             BAD(p, 0, "corrupt", "SYM count on a non-SYM column");
    SAVE(bool_runs(), p); W64(24, (uint64_t)-1);  BAD(p, 0, "corrupt", "negative length");
    SAVE(bool_runs(), p); W64(24, 1ull << 40);    BAD(p, 0, "corrupt", "exceeds what the file holds");
    SAVE(bool_runs(), p); W8(5, 1);               BAD(p, 0, "corrupt", "reserved header bytes set");
    SAVE(bool_runs(), p); W8(16, 1);              BAD(p, 0, "corrupt", "header mmod byte set");
    SAVE(bool_runs(), p); W8(EH, 'X');            BAD(p, 0, "corrupt", "missing RENC header");
    SAVE(bool_runs(), p); W16(EH + 4, 2);         BAD(p, 0, "version", "header version 2");
    SAVE(bool_runs(), p); W8(EH + 6, 7);          BAD(p, 0, "corrupt", "index kind 7");
    ray_t* g = guid_raw(); SAVE(g, p); W8(EH + 6, 3); BAD(p, 0, "corrupt", "not admitted on type");  /* ZONE on GUID */
    SAVE(guid_raw(), p);  W8(EH + 7, 1);          BAD(p, 0, "corrupt", "UNIQUE not admitted");
    SAVE(bool_runs(), p); W8(EH + 7, 2);          BAD(p, 0, "corrupt", "reserved column flags");
    SAVE(bool_runs(), p); W32(EH + 8, 1);         BAD(p, 0, "corrupt", "reserved enc header fields");
    SAVE(bool_runs(), p); W32(EH + 12, 1);        BAD(p, 0, "corrupt", "reserved enc header fields");
    SAVE(bool_runs(), p); W32(EH + 40, 1);        BAD(p, 0, "corrupt", "reserved enc header fields");
    SAVE(bool_runs(), p); W32(EH + 44, 1);        BAD(p, 0, "corrupt", "reserved enc header fields");
    SAVE(bool_runs(), p); W64(EH + 48, 1);        BAD(p, 0, "corrupt", "on a non-STR column");
    SAVE(str_mixed(), p); W64(EH + 48, 0x100000000ull); BAD(p, 0, "corrupt", "above 4 GiB");
    SAVE(bool_runs(), p); W64(EH + 56, g_len + 8); BAD(p, 0, "corrupt", "end offset");
    SAVE(bool_runs(), p); TEST_ASSERT_TRUE(img_store(p, 60)); EXPECT(p, 0, "corrupt", "shorter than its headers");
    SAVE(bool_runs(), p); W8(EH + 6, 1);          BAD(p, 0, "corrupt", "disagrees with index offset");   /* HASH, no region */
    SAVE(bool_runs(), p); W64(EH + 32, 4096);     BAD(p, 0, "corrupt", "disagrees with index offset");   /* region, no kind */
    SAVE(bool_runs(), p); W8(EH + 6, 1); W64(EH + 32, g_len - 32); BAD(p, 0, "corrupt", "not at the next 32-aligned offset");
    SAVE(bool_runs(), p); W8(EH + 6, 1); W64(EH + 32, g_len); BAD(p, 0, "corrupt", "index region past the end");
    /* padding before the index region: a 3080-byte chunk end (not 32-aligned) followed by a dirty pad byte */
    SAVE(bool_const(), p); NEED_ENC(0, 0, RAY_ENC_CONST);
    {
        size_t prev = chunk_end(0); TEST_ASSERT_TRUE(prev % 32 != 0);
        size_t off = (prev + 31) & ~(size_t)31;
        uint8_t* q = realloc(g_img, off + 32); g_img = q; memset(g_img + prev, 0, off + 32 - prev);
        g_img[prev] = 1; g_len = off + 32;
        W64(EH + 56, g_len); W8(EH + 6, 1); W64(EH + 32, off);
        BAD(p, 0, "corrupt", "padding before the index region not zero");
    }
    /* an index region shorter than its 80-byte wire root is a truncation; a full
     * 80-byte root loads and fails validation at materialization (the region
     * holds no padded root, so no child) */
    for (size_t rl = 32; rl <= 80; rl += 48) {
        SAVE(bool_runs(), p);
        size_t prev = chunk_end(0), off = (prev + 31) & ~(size_t)31;
        uint8_t* q = realloc(g_img, off + rl); g_img = q; memset(g_img + prev, 0, off + rl - prev);
        g_len = off + rl; W64(EH + 56, g_len); W8(EH + 6, 1); W64(EH + 32, off);
        if (rl < 80) BAD(p, 0, "corrupt", "index region shorter than its 80-byte root");
        else         BAD(p, 1, "corrupt", "index region shorter than its root");
    }
    /* bytes after the last section */
    SAVE(bool_runs(), p); { size_t n = g_len; g_img = realloc(g_img, n + 8); memset(g_img + n, 0, 8); g_len = n + 8; W64(EH + 56, g_len); }
    BAD(p, 0, "corrupt", "bytes after the last section");
    /* a SYM column opened without a symfile (explicit require) */
    {
        uint8_t ids[16]; for (int i = 0; i < 16; i++) ids[i] = (uint8_t)(i % 4);
        ray_t* v = ray_sym_vec_new(RAY_SYM_W8, 16); v->len = 16; memcpy(ray_data(v), ids, 16);
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK); ray_release(v);
        ray_error_clear();
        ray_t* r = ray_col_mmap_splayed_dom(p, NULL);
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "sym");
        TEST_ASSERT_TRUE(has_needle("no symfile resolved") && names_path(p));
        ray_error_free(r);
    }
    PASS();
}

/* ---- chunk directory ---- */
static test_result_t test_reject_directory(void) {
    const char* p = ray_test_tmp(EC_TMP "/d");
    SAVE(bool_runs(), p); W64(DIR(0), rd64(DIR(0)) + 8);           BAD(p, 0, "corrupt", "chunk 0 at");
    SAVE(bool_runs(), p); W64(DIR(0) + 8, 8);                      BAD(p, 0, "corrupt", "below the descriptor area");
    SAVE(bool_runs(), p); W64(DIR(0) + 8, rd64(DIR(0) + 8) - 4);   BAD(p, 0, "corrupt", "not a multiple of 8");
    SAVE(bool_runs(), p); W64(DIR(0) + 8, rd64(DIR(0) + 8) + 8);   BAD(p, 0, "corrupt", "overruns the file");
    SAVE(bool_runs(), p); W32(DIR(0) + 48, 1);                     BAD(p, 0, "corrupt", "reserved directory words");
    SAVE(bool_runs(), p); W32(DIR(0) + 60, 1);                     BAD(p, 0, "corrupt", "reserved directory words");
    SAVE(bool_runs(), p); W32(DIR(0) + 56, 2);                     BAD(p, 0, "corrupt", "reserved directory flag bits");
    SAVE(bool_runs(), p); W32(DIR(0) + 52, 1025);                  BAD(p, 0, "corrupt", "over 1024 rows");
    SAVE(bool_runs(), p); W32(DIR(0) + 52, 1);                     BAD(p, 0, "corrupt", "nulls in a non-nullable column");
    SAVE(i32_allnull(), p); W32(DIR(0) + 56, 0);                   BAD(p, 0, "corrupt", "statistics on an all-null chunk");
    SAVE(guid_raw(), p);    W32(DIR(0) + 56, 0);                   BAD(p, 0, "corrupt", "statistics on GUID");
    SAVE(bool_runs(), p);   W64(DIR(0) + 16, 2); W64(DIR(0) + 24, 1); BAD(p, 0, "corrupt", "min above max");
    SAVE(f64_stats(), p);   { double nan = NAN; wr(DIR(0) + 16, &nan, 8); } BAD(p, 0, "corrupt", "float bounds");
    SAVE(f64_stats(), p);   { double hi = 5.0, lo = 1.0; wr(DIR(0) + 16, &hi, 8); wr(DIR(0) + 24, &lo, 8); } BAD(p, 0, "corrupt", "float bounds");
    SAVE(i32_allnull(), p); W64(DIR(0) + 16, 3);                   BAD(p, 0, "corrupt", "absent statistics not zero");
    SAVE(f64_stats(), p);   W64(DIR(0) + 32, 5);                   BAD(p, 0, "corrupt", "sum on an unsummed type");
    SAVE(i32_allnull(), p); W64(DIR(0) + 32, 5);                   BAD(p, 0, "corrupt", "sum of no values");
    /* a STR chunk with statistics present: a newer build's file (pinned in enccol/load_rejects) */
    PASS();
}

/* ---- side data: table, entries, placement, contents ---- */
static test_result_t test_reject_side_data(void) {
    const char* p = ray_test_tmp(EC_TMP "/s");
    uint8_t sb[256]; uint16_t kind[3]; uint64_t off[3], len[3];
    /* the writer's own side region of an I64 DICT column: table @side, entry @side+32 */
    SAVE(i64_dict(), p); NEED_ENC(0, 0, RAY_ENC_DICT);
    size_t side = (size_t)rd64(EH + 16);
    TEST_ASSERT_TRUE(side != 0 && rd64(EH + 24) == 72);
    uint8_t good[72]; memcpy(good, g_img + side, 72);
    struct { const char* needle; int what; } tbl[] = {
        { "side table reserved word set", 0 }, { "side table entry count 0", 1 }, { "side table entry count 4", 2 },
        { "side table longer than the side data", 3 }, { "reserved fields set", 4 }, { "reserved fields set", 5 },
        { "unknown kind 9", 6 }, { "unknown kind 4", 7 },
        { "offset not the next 32-aligned position", 8 }, { "offset not the next 32-aligned position", 9 },
        { "length 4", 10 }, { "overruns the side data", 11 },
        { "side tail", 12 }, { "side tail longer than padding", 13 },
    };
    for (size_t t = 0; t < sizeof tbl / sizeof *tbl; t++) {
        memcpy(sb, good, 72); size_t n = 72;
        switch (tbl[t].what) {
        case 0: { uint32_t one = 1; memcpy(sb + 4, &one, 4); } break;
        case 1: memset(sb, 0, 4); break;
        case 2: { uint32_t four = 4; memcpy(sb, &four, 4); } break;
        case 3: { uint32_t three = 3; memcpy(sb, &three, 4); } break;
        case 4: sb[10] = 1; break;
        case 5: sb[12] = 1; break;
        case 6: sb[8] = 9; break;
        case 7: sb[8] = 4; break;
        case 8: sb[16] = 33; break;
        case 9: sb[16] = 64; break;
        case 10: sb[24] = 4; break;
        case 11: sb[24] = 0xFF; sb[25] = 0xFF; break;
        case 12: memset(sb + 72, 0, 24); sb[80] = 1; n = 96; break;      /* tail within padding, dirty */
        case 13: memset(sb + 72, 0, 56); n = 128; break;                  /* tail longer than the padding */
        }
        SAVE(i64_dict(), p); put_side(sb, n);
        BAD(p, 0, "corrupt", tbl[t].needle);
    }
    /* duplicate kind, string entry on a non-STR column, dirty gap before an entry */
    kind[0] = 1; kind[1] = 1; off[0] = 64; off[1] = 128; len[0] = 40; len[1] = 40;
    side_table(sb, 168, 2, kind, off, len);
    SAVE(i64_dict(), p); put_side(sb, 168); BAD(p, 0, "corrupt", "duplicate kind 1");
    kind[1] = 2;
    side_table(sb, 168, 2, kind, off, len);
    SAVE(i64_dict(), p); put_side(sb, 168); BAD(p, 0, "corrupt", "string side entry on type");
    side_table(sb, 168, 2, kind, off, len); sb[56] = 1;
    SAVE(i64_dict(), p); put_side(sb, 168); BAD(p, 0, "corrupt", "side padding");
    /* VALUE_DICT entry on a type without a dictionary */
    kind[0] = 1; off[0] = 32; len[0] = 40;
    side_table(sb, 72, 1, kind, off, len);
    SAVE(bool_runs(), p); put_side(sb, 72); BAD(p, 0, "corrupt", "VALUE_DICT on type");
    /* a VALUE_DICT entry on a float column: legal from P5, so a newer build's file */
    SAVE(f64_plain(), p); put_side(sb, 72); BAD(p, 0, "nyi", "float dictionary needs a newer build");
    SAVE(f32_plain(), p); put_side(sb, 72); BAD(p, 0, "nyi", "float dictionary needs a newer build");
    /* side region placement */
    SAVE(bool_runs(), p); put_side(good, 72); W64(EH + 16, rd64(EH + 16) + 8); BAD(p, 0, "corrupt", "side data not at the next 32-aligned offset");
    SAVE(bool_runs(), p); put_side(good, 72); W64(EH + 24, 4);                 BAD(p, 0, "corrupt", "side data length 4");
    SAVE(bool_runs(), p); put_side(good, 72); W64(EH + 24, 1ull << 30);        BAD(p, 0, "corrupt", "side data overruns the file");
    SAVE(bool_runs(), p); W64(EH + 24, 8);                                     BAD(p, 0, "corrupt", "side length without side data");
    SAVE(bool_const(), p);
    {   /* dirty bytes between the last chunk and the side data */
        size_t prev = chunk_end(0); TEST_ASSERT_TRUE(prev % 32 != 0);
        put_side(good, 72); g_img[prev] = 1;
        BAD(p, 0, "corrupt", "padding before the side data not zero");
    }
    /* entry contents, checked once before first use (materialization) */
    for (int k = 0; k < 6; k++) {
        SAVE(i64_dict(), p); NEED_ENC(0, 0, RAY_ENC_DICT);
        size_t entry = side + 32;
        const char* needle = "";
        if (k == 0) { W64(entry + 8, rd64(entry + 16)); needle = "VALUE_DICT values not ascending"; }
        if (k == 1) { W32(entry, 3); needle = "VALUE_DICT entry size"; }
        if (k == 2) { W32(entry + 4, 1); needle = "VALUE_DICT entry reserved word"; }
        if (k == 3) { W32(entry, 0); needle = "VALUE_DICT entry count"; }
        if (k == 4) { W32(entry, 65537); needle = "VALUE_DICT entry count"; }
        if (k == 5) { W64(entry + 8, rd64(entry + 16)); W64(entry + 16, rd64(entry + 8)); needle = "VALUE_DICT values not ascending"; }
        BAD(p, 1, "corrupt", needle);
    }
    {   /* nonzero padding after an odd-count I32 dictionary */
        SAVE(i32_dict3(), p); NEED_ENC(0, 0, RAY_ENC_DICT);
        size_t entry = (size_t)rd64(EH + 16) + 32;
        TEST_ASSERT_EQ_I((int)rd64(EH + 24), 8 + 24 + 8 + 16);
        W8(entry + 8 + 12, 1);
        BAD(p, 1, "corrupt", "VALUE_DICT entry padding");
    }
    {   /* a DICT vector whose side entry is gone */
        SAVE(i64_dict(), p); NEED_ENC(0, 0, RAY_ENC_DICT);
        size_t end = chunk_end(0);
        g_len = end; W64(EH + 16, 0); W64(EH + 24, 0); W64(EH + 56, end);
        BAD(p, 1, "corrupt", "DICT vector without a VALUE_DICT entry");
    }
    PASS();
}

/* ---- vector descriptors (materialization validates a chunk's descriptors before using them) ---- */
static test_result_t test_reject_descriptors(void) {
    const char* p = ray_test_tmp(EC_TMP "/x");
    size_t D;
    SAVE(bool_runs(), p); NEED_ENC(0, 0, RAY_ENC_RLE); D = desc_at(0, 0);
    W8(D, 200);                    BAD(p, 1, "corrupt", "illegal encoding");
    SAVE(bool_runs(), p); W8(desc_at(0, 0), RAY_ENC_ALP);  BAD(p, 1, "corrupt", "illegal encoding");   /* ALP on BOOL */
    SAVE(bool_runs(), p); W16(desc_at(0, 0) + 6, 1);       BAD(p, 1, "corrupt", "descriptor reserved fields");
    SAVE(bool_runs(), p); W32(desc_at(0, 0) + 24, 1);      BAD(p, 1, "corrupt", "descriptor reserved fields");
    SAVE(bool_runs(), p); W32(desc_at(0, 0) + 28, 1);      BAD(p, 1, "corrupt", "descriptor reserved fields");
    SAVE(bool_runs(), p); W16(desc_at(0, 0) + 4, 4);       BAD(p, 1, "corrupt", "flags 0x4");
    SAVE(bool_runs(), p); W16(desc_at(0, 0) + 4, 2);       BAD(p, 1, "corrupt", "flags 0x2");        /* float trailer on a non-float */
    SAVE(bool_runs(), p); W8(desc_at(0, 0) + 1, 65);       BAD(p, 1, "corrupt", "width 65");         /* width on RLE */
    SAVE(i32_for(), p);   NEED_ENC(0, 0, RAY_ENC_FOR_BP); W8(desc_at(0, 0) + 1, 33); BAD(p, 1, "corrupt", "width 33");   /* FOR_BP width above T */
    SAVE(bool_runs(), p); W16(desc_at(0, 0) + 2, 1025);    BAD(p, 1, "corrupt", "above the vector length");
    SAVE(bool_runs(), p); W16(desc_at(0, 0) + 2, 1);       BAD(p, 1, "corrupt", "nulls counted in a non-nullable type");
    SAVE(bool_runs(), p); W64(desc_at(0, 0) + 8, rd64(desc_at(0, 0) + 8) + 8); BAD(p, 1, "corrupt", "payload offset");
    SAVE(bool_runs(), p); W64(desc_at(0, 0) + 16, rd64(desc_at(0, 0) + 16) + 1); BAD(p, 1, "corrupt", "payload size not a multiple of 8");
    SAVE(bool_runs(), p); W64(desc_at(0, 0) + 16, 1 << 20); BAD(p, 1, "corrupt", "payload overruns the chunk");
    SAVE(bool_runs(), p); W8(desc_at(0, 3) + 20, 1);       BAD(p, 1, "corrupt", "trailing descriptor 3 not zero");
    SAVE(bool_runs(), p); set_chunk_bytes(0, rd64(DIR(0) + 8) + 8); BAD(p, 1, "corrupt", "bytes after its payloads");
    SAVE(bool_runs(), p); W16(desc_at(0, 0) + 4, 1); W64(desc_at(0, 0) + 40, 1); BAD(p, 1, "corrupt", "absent statistics not zero");
    SAVE(bool_runs(), p); W64(desc_at(0, 0) + 32, 2); W64(desc_at(0, 0) + 40, 1); BAD(p, 1, "corrupt", "min above max");
    SAVE(f64_stats(), p); W64(desc_at(0, 0) + 32, 0x7FF8000000000000ull); BAD(p, 1, "corrupt", "float bounds");
    SAVE(f64_stats(), p); { double hi = 5.0, lo = 1.0; wr(desc_at(0, 0) + 32, &hi, 8); wr(desc_at(0, 0) + 40, &lo, 8); } BAD(p, 1, "corrupt", "float bounds");
    SAVE(f64_plain(), p); W8(desc_at(0, 0), RAY_ENC_ALP);  BAD(p, 1, "nyi", "needs a newer build");  /* legal, not in P1 */
    SAVE(guid_raw(), p);  W16(desc_at(0, 0) + 4, 0);       BAD(p, 1, "corrupt", "statistics on GUID");
    SAVE(i32_allnull(), p); NEED_ENC(0, 0, RAY_ENC_CONST);
    W8(desc_at(0, 0), RAY_ENC_RAW);                        BAD(p, 1, "corrupt", "all-null vector not CONST");
    SAVE(i32_allnull(), p); W16(desc_at(0, 0) + 4, 0);     BAD(p, 1, "corrupt", "all-null vector carries statistics");
    SAVE(i32_nulls_raw(), p); W16(desc_at(0, 0) + 2, 9);   BAD(p, 1, "corrupt", "descriptor null counts sum to 9");
    {   /* STR statistics on a RAW vector (directory says absent, the descriptor does not) */
        SAVE(str_mixed(), p); W16(desc_at(0, 0) + 4, 0);   BAD(p, 1, "corrupt", "statistics on a RAW / CONST string vector");
    }
    {   /* SYM statistics outside the domain: ids in 1..4, saved count 200 */
        uint8_t ids[4] = { 1, 2, 3, 4 };
        SAVE_SYM(ids, 4, p); W64(desc_at(0, 0) + 40, 250);
        BAD(p, 1, "corrupt", "SYM statistics outside the domain");
    }
    PASS();
}

/* ---- payloads: floats, bitmaps, GUID, STR, SYM, DICT codes ---- */
static test_result_t test_reject_payloads(void) {
    const char* p = ray_test_tmp(EC_TMP "/q");
    size_t at;
    /* null bitmap */
    SAVE(i32_nulls_raw(), p); NEED_ENC(0, 0, RAY_ENC_RAW); at = payload_at(0, 0);
    W8(at, 0xFF);                                          BAD(p, 1, "corrupt", "null bitmap count");
    SAVE(i32_nulls_raw(), p); at = payload_at(0, 0); g_img[at + 12] |= 0x10;   /* bit 100: past the 100 rows */
    BAD(p, 1, "corrupt", "null bitmap tail bits set");
    SAVE(i32_nulls_raw(), p);
    W64(desc_at(0, 0) + 16, 64); set_chunk_bytes(0, 3072 + 64); BAD(p, 1, "corrupt", "null bitmap short");
    SAVE(i32_nulls_raw(), p); at = payload_at(0, 0); W32(at + 128 + 4, (uint32_t)NULL_I32);
    BAD(p, 1, "corrupt", "sentinel outside the bitmap");
    /* floats */
    SAVE(f64_plain(), p); NEED_ENC(0, 0, RAY_ENC_RAW); at = payload_at(0, 0);
    W64(at + 8, 0x7FF8000000000001ull);                    BAD(p, 1, "corrupt", "NaN outside the bitmap");
    SAVE(f64_plain(), p); W64(desc_at(0, 0) + 16, 8); set_chunk_bytes(0, 3072 + 8);
    BAD(p, 1, "corrupt", "float body short");
    SAVE(f64_plain(), p); W16(desc_at(0, 0) + 4, 2);       BAD(p, 1, "corrupt", "trailer missing");
    SAVE(f64_plain(), p); W64(desc_at(0, 0) + 16, 40); set_chunk_bytes(0, 3072 + 40);
    BAD(p, 1, "corrupt", "bytes after the body");
    SAVE(f32_plain(), p); NEED_ENC(0, 0, RAY_ENC_RAW); at = payload_at(0, 0);
    W8(at + 12, 1);                                        BAD(p, 1, "corrupt", "float body padding");
    SAVE(f32_const(), p); NEED_ENC(0, 0, RAY_ENC_CONST); at = payload_at(0, 0);
    W8(at + 4, 1);                                         BAD(p, 1, "corrupt", "float CONST padding");
    /* the trailer (F64: bitmap 128, body 32, trailer @+160: u16 cnt, 6 reserved, positions @+168, values) */
    for (int k = 0; k < 9; k++) {
        SAVE(f64_two_odd(), p); NEED_ENC(0, 0, RAY_ENC_RAW);
        at = payload_at(0, 0);
        TEST_ASSERT_TRUE(g_img[desc_at(0, 0) + 4] & RAY_ENC_DESC_TRAILER);
        size_t t = at + 128 + 32;
        const char* needle = "";
        uint16_t cnt = 0; memcpy(&cnt, g_img + t, 2); TEST_ASSERT_EQ_I(cnt, 2);
        if (k == 0) { W8(t + 2, 1); needle = "trailer reserved bytes"; }
        if (k == 1) { W16(t, 0); needle = "trailer count"; }
        if (k == 2) { W16(t, 3); needle = "trailer count"; }
        if (k == 3) { W16(t, 1); needle = "trailer size"; }
        if (k == 4) { W16(t + 8, 1); W16(t + 10, 0); needle = "trailer positions not ascending"; }
        if (k == 5) { W16(t + 8, 9); needle = "trailer position past the vector"; }
        if (k == 6) { W16(t + 10, 2); needle = "trailer position not in the bitmap"; }
        if (k == 7) { double one = 1.0; wr(t + 16, &one, 8); needle = "trailer value is not a NaN"; }
        if (k == 8) { W8(t + 8 + 4, 1); needle = "trailer position padding"; }
        BAD(p, 1, "corrupt", needle);
    }
    /* F32: one trailer value; 4 value bytes, 4 padding (bitmap 128, body 16, trailer @+144, position @+152, value @+160) */
    SAVE(f32_odd(), p); NEED_ENC(0, 0, RAY_ENC_RAW); at = payload_at(0, 0);
    W8(at + 128 + 16 + 16 + 4, 1);                         BAD(p, 1, "corrupt", "trailer value padding");
    /* GUID */
    SAVE(guid_raw(), p); NEED_ENC(0, 0, RAY_ENC_RAW);
    W64(desc_at(0, 0) + 16, 56); set_chunk_bytes(0, 3072 + 56); BAD(p, 1, "corrupt", "GUID RAW body size");
    SAVE(guid_const(), p); NEED_ENC(0, 0, RAY_ENC_CONST);
    W64(desc_at(0, 0) + 16, 24); set_chunk_bytes(0, 3072 + 24); BAD(p, 1, "corrupt", "GUID CONST body size");
    SAVE(guid_raw(), p); memset(g_img + payload_at(0, 0), 0, 16); BAD(p, 1, "corrupt", "GUID null count");
    /* STR RAW / CONST */
    for (int k = 0; k < 10; k++) {
        SAVE(str_mixed(), p); NEED_ENC(0, 0, RAY_ENC_RAW);
        at = payload_at(0, 0);
        const char* needle = "";
        if (k == 0) { W32(at + 16 + 12, 7); needle = "pooled descriptor hash not zero"; }
        if (k == 1) { W8(at + 4 + 6, 'x'); needle = "inline padding"; }
        if (k == 2) { W32(at + 16 + 8, 1); needle = "pooled descriptor offset"; }
        if (k == 3) { W32(at + 16, 100); needle = "pooled descriptor past the pool"; }
        if (k == 4) { W8(at + 16 + 4, 'Z'); needle = "pooled descriptor prefix"; }
        if (k == 5) { W64(at + 32, 21); needle = "pool not fully used"; }
        if (k == 6) { W64(at + 32, 40); needle = "pool size"; }
        if (k == 7) { W64(at + 32, 0x100000000ull); needle = "pool size above 4 GiB"; }
        if (k == 8) { W8(at + 40 + 20, 1); needle = "pool padding"; }
        if (k == 9) { W64(desc_at(0, 0) + 16, 16); set_chunk_bytes(0, 3072 + 16); needle = "STR RAW body short"; }
        BAD(p, 1, "corrupt", needle);
    }
    SAVE(str_mixed(), p); W16(desc_at(0, 0) + 2, 1); W32(DIR(0) + 52, 1); BAD(p, 1, "corrupt", "STR null count");
    SAVE(str_mixed(), p); W64(EH + 48, rd64(EH + 48) + 1);      BAD(p, 1, "corrupt", "header says");
    SAVE(str_mixed(), p); W64(EH + 48, rd64(EH + 48) - 1);      BAD(p, 1, "corrupt", "exceeds str_pool_bytes");
    for (int k = 0; k < 4; k++) {
        SAVE(str_const(), p); NEED_ENC(0, 0, RAY_ENC_CONST);
        at = payload_at(0, 0);
        const char* needle = "";
        if (k == 0) { W32(at + 4, 1); needle = "STR CONST reserved word"; }
        if (k == 1) { W8(at + 8 + 25, 1); needle = "STR CONST padding"; }
        if (k == 2) { W64(desc_at(0, 0) + 16, rd64(desc_at(0, 0) + 16) + 8); set_chunk_bytes(0, rd64(DIR(0) + 8) + 8); needle = "STR CONST body size"; }
        if (k == 3) { W64(desc_at(0, 0) + 16, 0); set_chunk_bytes(0, 3072); needle = "STR CONST body short"; }
        BAD(p, 1, "corrupt", needle);
    }
    /* DICT codes at or above the entry count: the codes child is FOR_BP w=2 whose base is raised to 4 */
    SAVE(i64_dict(), p); NEED_ENC(0, 0, RAY_ENC_DICT);
    TEST_ASSERT_EQ_I(g_img[payload_at(0, 0)], RAY_ENC_FOR_BP);
    W64(payload_at(0, 0) + 8, 4);                          BAD(p, 1, "corrupt", "DICT code outside the dictionary");
    /* SYM */
    {
        uint8_t ids[1024];
        for (int i = 0; i < 4; i++) ids[i] = (uint8_t)(i + 1);
        SAVE_SYM(ids, 4, p); NEED_ENC(0, 0, RAY_ENC_RAW);
        W8(payload_at(0, 0) + 2, 250);                     BAD(p, 1, "corrupt", "SYM id outside the symbol domain");
        ids[0] = 0;                                         /* one null */
        SAVE_SYM(ids, 4, p); W8(payload_at(0, 0), 7);      BAD(p, 1, "corrupt", "SYM null count");
        for (int i = 0; i < 1024; i++) ids[i] = 5;
        SAVE_SYM(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_CONST);
        W8(payload_at(0, 0), 250);                         BAD(p, 1, "corrupt", "SYM id outside the symbol domain");
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(100 + i % 16);
        SAVE_SYM(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_FOR_BP);
        W64(payload_at(0, 0), 199);                        BAD(p, 1, "corrupt", "SYM id outside the symbol domain");
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(1 + i / 100);
        SAVE_SYM(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_RLE);
        W8(payload_at(0, 0) + 16, 250);                    BAD(p, 1, "corrupt", "SYM id outside the symbol domain");
    }
    PASS();
}

/* ---- codec grammar over a whole file: RAW, RLE, children, FOR_BP, DELTA_BP ---- */
static test_result_t test_reject_codec_grammar(void) {
    const char* p = ray_test_tmp(EC_TMP "/g");
    size_t at;
    for (int k = 0; k < 18; k++) {
        SAVE(bool_runs(), p); NEED_ENC(0, 0, RAY_ENC_RLE);
        at = payload_at(0, 0);
        TEST_ASSERT_EQ_I(g_img[at + 8], RAY_ENC_RAW);
        TEST_ASSERT_EQ_I(g_img[at + 32], RAY_ENC_RAW);
        const char* needle = "";
        if (k == 0)  { W8(at + 16 + 11, 1); needle = "RAW padding not zero"; }
        if (k == 1)  { W32(at + 12, 8); needle = "RAW body short"; }                  /* values child body 8 < 16, and unconsumed */
        if (k == 2)  { W16(at + 42, 50); needle = "RLE run ends not ascending"; }
        if (k == 3)  { W16(at + 60, 1010); needle = "RLE last run end is not the vector length"; }
        if (k == 4)  { W16(at + 60, 1025); needle = "RLE run ends not ascending or past the vector"; }
        if (k == 5)  { W8(at + 8, RAY_ENC_CONST); needle = "encoding illegal in this slot or depth"; }
        if (k == 6)  { W32(at + 12, 0xFFF0); needle = "child body overruns its parent"; }
        if (k == 7)  { W32(at + 12, 17); needle = "child body size not a multiple of 8"; }
        if (k == 8)  { W16(at + 10, 1); needle = "child header reserved bytes not zero"; }
        if (k == 9)  { W32(at + 12, 24); needle = "child body not fully consumed"; }
        if (k == 10) { W8(at + 9, 1); needle = "width on a non-packed encoding"; }
        if (k == 11) { W32(at, 0); needle = "RLE run count out of range"; }
        if (k == 12) { W32(at, 2000); needle = "RLE run count out of range"; }
        if (k == 13) { W32(at + 4, 1); needle = "RLE reserved word not zero"; }
        if (k == 14) { W8(at + 32, RAY_ENC_FOR_BP); W8(at + 33, 17); needle = "FOR_BP width above the type width"; }
        if (k == 15) { W8(at + 32, RAY_ENC_FOR_BP); W8(at + 33, 16); needle = "FOR_BP body short"; }
        if (k == 16) { W8(at + 32, RAY_ENC_FOR_BP); W8(at + 33, 0); W32(at + 36, 8); W64(at + 40, 0x10000); needle = "FOR_BP base above the type width"; }
        if (k == 17) { W8(at + 32, 200); needle = "encoding illegal in this slot or depth"; }
        char want[96]; snprintf(want, sizeof want, "codec body: %s", needle);
        BAD(p, 1, "corrupt", want);
    }
    /* a damaged payload that leaves the body valid but longer than the codec consumed */
    SAVE(bool_runs(), p); at = payload_at(0, 0);
    W64(desc_at(0, 0) + 16, 72); set_chunk_bytes(0, rd64(DIR(0) + 8) + 8);
    BAD(p, 1, "corrupt", "bytes after the codec body");
    /* DELTA_BP: I32 (T 32, 32 lanes): base @+0, lane heads @+8, placeholder residuals from +264 */
    for (int k = 0; k < 3; k++) {
        SAVE(i32_delta(), p); NEED_ENC(0, 0, RAY_ENC_DELTA_BP);
        at = payload_at(0, 0);
        const char* needle = "";
        if (k == 0) { W64(at, 0x100000000ull); needle = "DELTA_BP base above the type width"; }
        if (k == 1) { W64(at + 8, 0x100000000ull); needle = "DELTA_BP head above the type width"; }
        if (k == 2) { W8(at + 8 + 8 * 32, 1); needle = "DELTA_BP placeholder not zero"; }
        char want[96]; snprintf(want, sizeof want, "codec body: %s", needle);
        BAD(p, 1, "corrupt", want);
    }
    /* FOR_BP at the top: base above the type width */
    SAVE(i32_for(), p); NEED_ENC(0, 0, RAY_ENC_FOR_BP);
    W64(payload_at(0, 0), 0x100000000ull);                 BAD(p, 1, "corrupt", "codec body: FOR_BP base above the type width");
    PASS();
}

/* ---- the child cascade at the codec boundary: depth limit, legal slots ---- */
static size_t put_hdr(uint8_t* b, uint8_t enc, uint8_t w, uint32_t bb) {
    memset(b, 0, 8); b[0] = enc; b[1] = w; memcpy(b + 4, &bb, 4);
    return 8;
}
/* RLE body of one run over n values (nr = 1, RAW values child, RAW ends child). Returns its size. */
static size_t rle_body(uint8_t* b, uint32_t value, uint16_t n) {
    uint32_t nr = 1, z = 0; size_t at = 0;
    memcpy(b, &nr, 4); memcpy(b + 4, &z, 4); at = 8;
    at += put_hdr(b + at, RAY_ENC_RAW, 0, 8); memset(b + at, 0, 8); memcpy(b + at, &value, 4); at += 8;
    at += put_hdr(b + at, RAY_ENC_RAW, 0, 8); memset(b + at, 0, 8); memcpy(b + at, &n, 2); at += 8;
    return at;
}
static test_result_t test_reject_depth4(void) {
    uint8_t body[256]; uint64_t out[RAY_ENC_VEC];
    ray_enc_kind_t u32 = { 32, false };
    /* control: a valid RLE body at the top, children at depth 1 */
    size_t n = rle_body(body, 77, 4);
    TEST_ASSERT_EQ_I(n, 40);
    TEST_ASSERT_EQ_I(ray_enc_read(body, n, RAY_ENC_RLE, 0, 4, u32, RAY_ENC_SLOT_TOP, 0, NULL, out), 40);
    TEST_ASSERT_EQ_U(out[0], 77); TEST_ASSERT_EQ_U(out[3], 77);
    /* control: an RLE child (in a codes slot) at depth 2, its own children at depth 3 */
    uint8_t child[256]; size_t cl = put_hdr(child, RAY_ENC_RLE, 0, 40); memcpy(child + cl, body, 40); cl += 40;
    TEST_ASSERT_EQ_I(ray_enc_read(child, cl, 0, 0, 4, u32, RAY_ENC_SLOT_CODES, 2, NULL, out), 48);
    TEST_ASSERT_EQ_U(out[1], 77);
    /* controls: RAW and FOR_BP children at depth 3 */
    uint8_t leaf[64]; size_t ll = put_hdr(leaf, RAY_ENC_RAW, 0, 8); memset(leaf + 8, 0, 8); ll += 8;
    TEST_ASSERT_EQ_I(ray_enc_read(leaf, ll, 0, 0, 2, u32, RAY_ENC_SLOT_CODES, 3, NULL, out), 16);
    ll = put_hdr(leaf, RAY_ENC_FOR_BP, 0, 8); memset(leaf + 8, 0, 8); ll += 8;
    TEST_ASSERT_EQ_I(ray_enc_read(leaf, ll, 0, 0, 2, u32, RAY_ENC_SLOT_CODES, 3, NULL, out), 16);
    /* an RLE child at depth 3 would put its children at depth 4: refused for the slot */
    TEST_ASSERT_EQ_I(ray_enc_read(child, cl, 0, 0, 4, u32, RAY_ENC_SLOT_CODES, 3, NULL, out), -1);
    TEST_ASSERT_TRUE(strstr(ray_enc_read_why(), "encoding illegal in this slot or depth") != NULL);
    /* any child at depth 4 */
    ll = put_hdr(leaf, RAY_ENC_RAW, 0, 8); memset(leaf + 8, 0, 8); ll += 8;
    TEST_ASSERT_EQ_I(ray_enc_read(leaf, ll, 0, 0, 2, u32, RAY_ENC_SLOT_CODES, 4, NULL, out), -1);
    TEST_ASSERT_TRUE(strstr(ray_enc_read_why(), "cascade deeper than the maximum") != NULL);
    PASS();
}

/* ---- Task 3b: the integer fast kernel never accepts what the reference rejects ---- */

/* Two range reads of vector `vec` of the patched image through one column
 * object (the chunk validated by the first when its descriptors are sound):
 * both `corrupt`, naming the path and the rule. */
static const char* expect_twice(const char* path, int64_t vec, const char* needle) {
    static char why[512];
    if (!img_store(path, g_len)) return "store";
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_error_free(c); return "load failed"; }
    ray_col_src_t src;
    ray_t* m = ray_col_src_bind(c, &src);
    if (m) { ray_error_free(m); ray_release(c); return "bind failed"; }
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    const char* res = NULL;
    for (int i = 0; i < 2 && !res; i++) {
        ray_col_range_t r;
        ray_error_clear();
        ray_t* e = ray_col_range(&src, vec * RAY_ENC_VEC, RAY_ENC_VEC, &sc, &r);
        bool ok = e && RAY_IS_ERR(e) && strcmp(ray_err_code(e), "corrupt") == 0 && names_path(path) && has_needle(needle);
        if (!ok) {
            snprintf(why, sizeof why, "read %d gave %s: \"%s\", want corrupt [%s]", i + 1,
                     e && RAY_IS_ERR(e) ? ray_err_code(e) : "a view", e && ray_error_msg() ? ray_error_msg() : "", needle);
            res = why;
        }
        if (e) ray_error_free(e);
    }
    ray_col_scratch_free(&sc); ray_col_src_release(&src);
    ray_release(c);
    return res;
}
#define BAD2(path, vec, needle) do { const char* w_ = expect_twice(path, vec, needle); \
    TEST_ASSERT_FMT(w_ == NULL, "%s:%d %s", __FILE__, __LINE__, w_); } while (0)

/* 1024 rows of `type` (I16 / I32): 5000 + a hash of i in [0, 64) -> FOR_BP width 6. */
static ray_t* fk_for(int8_t type) {
    ray_t* v = ray_vec_new(type, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) {
        int x = 5000 + (int)(((uint32_t)i * 2654435761u) >> 26);
        if (type == RAY_I16) ((int16_t*)ray_data(v))[i] = (int16_t)x;
        else ((int32_t*)ray_data(v))[i] = x;
    }
    return v;
}
/* 2048 I32 rows 3i + i % 5 -> DELTA_BP (32 heads, small deltas) in both vectors. */
static ray_t* fk_delta(void) {
    ray_t* v = ray_vec_new(RAY_I32, 2048); v->len = 2048;
    for (int i = 0; i < 2048; i++) ((int32_t*)ray_data(v))[i] = 3 * i + i % 5;
    return v;
}

/* fk_for with a null every 10th row (a bitmap). */
static ray_t* fk_for_nulls(int8_t type) {
    ray_t* v = fk_for(type);
    for (int i = 0; i < 1024; i += 10) {
        if (type == RAY_I16) ((int16_t*)ray_data(v))[i] = NULL_I16;
        else ((int32_t*)ray_data(v))[i] = NULL_I32;
    }
    v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}
/* fk_delta with nulls at rows 1024 and 1025, two lane heads of vector 1:
 * their fill (the vector's minimum) is close to the true values, so the
 * deltas stay small and the vector stays DELTA_BP. */
static ray_t* fk_delta_nulls(void) {
    ray_t* v = fk_delta();
    ((int32_t*)ray_data(v))[1024] = NULL_I32;
    ((int32_t*)ray_data(v))[1025] = NULL_I32;
    v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}

static test_result_t test_fast_kernel_corrupt_twice(void) {
    const char* p = ray_test_tmp(EC_TMP "/fk");
    size_t at;
    for (int t = 0; t < 2; t++) {
        int8_t type = t ? RAY_I32 : RAY_I16;
        int T = t ? 32 : 16;
        /* base above the type width */
        SAVE(fk_for(type), p); NEED_ENC(0, 0, RAY_ENC_FOR_BP); at = payload_at(0, 0);
        W64(at, (uint64_t)1 << T);                          BAD2(p, 0, "FOR_BP base above the type width");
        /* a value on the sentinel, no bitmap: base moved onto MIN (offset 0 rows decode to it) */
        SAVE(fk_for(type), p); at = payload_at(0, 0);
        W64(at, (uint64_t)1 << (T - 1));                    BAD2(p, 0, "sentinel outside the bitmap");
        /* the body a word short (the vector, its chunk and the file shrink together) */
        SAVE(fk_for(type), p);
        uint64_t pb = rd64(desc_at(0, 0) + 16);
        W64(desc_at(0, 0) + 16, pb - 8); set_chunk_bytes(0, 3072 + pb - 8);
        BAD2(p, 0, "FOR_BP body short");
        /* a word long */
        SAVE(fk_for(type), p);
        W64(desc_at(0, 0) + 16, pb + 8); set_chunk_bytes(0, 3072 + pb + 8);
        BAD2(p, 0, "bytes after the codec body");
        /* width above the type width: a descriptor rule, so never validated */
        SAVE(fk_for(type), p);
        W8(desc_at(0, 0) + 1, (uint8_t)(T + 1));            BAD2(p, 0, "width");
    }
    /* DELTA_BP (second vector: the chunk's first vector stays sound) */
    SAVE(fk_delta(), p); NEED_ENC(0, 1, RAY_ENC_DELTA_BP); at = payload_at(0, 1);
    TEST_ASSERT_TRUE(g_img[desc_at(0, 1) + 1] > 0);        /* width > 0: a placeholder row exists */
    W64(at + 8, (uint64_t)1 << 32);                         BAD2(p, 1, "DELTA_BP head above the type width");
    SAVE(fk_delta(), p); at = payload_at(0, 1);
    W64(at, (uint64_t)1 << 32);                             BAD2(p, 1, "DELTA_BP base above the type width");
    SAVE(fk_delta(), p); at = payload_at(0, 1);
    W32(at + 8 + 8 * 32, 1);                                BAD2(p, 1, "DELTA_BP placeholder not zero");
    SAVE(fk_delta(), p); at = payload_at(0, 1);
    W64(at + 8, (uint64_t)1 << 31);                         BAD2(p, 1, "sentinel outside the bitmap");   /* lane 0 head on MIN */
    /* With a null bitmap: a row outside it on the sentinel (the merge pass's rule).
     * I16 / I32 FOR_BP: the base moved onto the sentinel, so the minimum's
     * rows decode to it, nulls (every 10th row) and non-nulls alike. */
    for (int t = 0; t < 2; t++) {
        int8_t type = t ? RAY_I32 : RAY_I16;
        int T = t ? 32 : 16;
        SAVE(fk_for_nulls(type), p); NEED_ENC(0, 0, RAY_ENC_FOR_BP); at = payload_at(0, 0);
        TEST_ASSERT_TRUE(rd64(desc_at(0, 0)) >> 16 & 0xFFFF);      /* n_nulls > 0: a bitmap leads the payload */
        W64(at + 128, (uint64_t)1 << (T - 1));              BAD2(p, 0, "sentinel outside the bitmap");
    }
    /* I32 DELTA_BP with a bitmap: lane 2's head (row 1026, not null) on the sentinel */
    SAVE(fk_delta_nulls(), p); NEED_ENC(0, 1, RAY_ENC_DELTA_BP); at = payload_at(0, 1);
    TEST_ASSERT_TRUE(rd64(desc_at(0, 1)) >> 16 & 0xFFFF);
    W64(at + 128 + 8 + 8 * 2, (uint64_t)1 << 31);               BAD2(p, 1, "sentinel outside the bitmap");
    /* SYM FOR_BP: id 0 (the null) in a vector whose descriptor counts no nulls */
    uint8_t ids[1024];
    for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(1 + ((uint32_t)i * 2654435761u >> 27));
    SAVE_SYM(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_FOR_BP); at = payload_at(0, 0);
    TEST_ASSERT_EQ_I(rd64(desc_at(0, 0)) >> 16 & 0xFFFF, 0);
    W64(at, 0);                                             BAD2(p, 0, "SYM null count");   /* base 1 -> 0 */
    PASS();
}

/* ---- Task 13b fix round 1: statistics that size memory are checked ----
 * The directory's chunk statistics size engine structures (dense slabs,
 * direct arrays, key ranges), so the reader keeps every decoded value
 * inside them: each vector's statistics nest inside its chunk's (checked
 * with the chunk's descriptors), and each decoded non-null value lies
 * inside its vector's (checked by every decode path).  A violation is
 * `corrupt`; a forged statistic is never trusted for memory. */

#define STATS_OUT "statistics outside the chunk's"
#define VALUE_OUT "value outside its statistics"

/* Descriptor statistics reaching outside the chunk's, a vector without
 * statistics under a chunk with them, statistics outside the type: the
 * chunk's descriptor validation rejects each. */
static test_result_t test_stats_nested(void) {
    const char* p = ray_test_tmp(EC_TMP "/sn");
    size_t D;
    SAVE(i32_for(), p); D = desc_at(0, 0);
    TEST_ASSERT_EQ_U(rd64(D + 40), rd64(DIR(0) + 24));
    W64(D + 40, rd64(DIR(0) + 24) + 1);                     BAD(p, 1, "corrupt", "vector 0 " STATS_OUT);
    SAVE(i32_for(), p); D = desc_at(0, 0);
    W64(D + 32, rd64(DIR(0) + 16) - 1);                     BAD(p, 1, "corrupt", "vector 0 " STATS_OUT);
    SAVE(i32_for(), p); D = desc_at(0, 0);
    W64(D + 40, rd64(DIR(0) + 24) + 1);                     BAD(p, 2, "corrupt", "vector 0 " STATS_OUT);   /* a range read */
    SAVE(f64_stats(), p); { double hi = 100.0; wr(desc_at(0, 0) + 40, &hi, 8); } BAD(p, 1, "corrupt", STATS_OUT);
    SAVE(f64_stats(), p); { double lo = -1.0; wr(desc_at(0, 0) + 32, &lo, 8); }  BAD(p, 1, "corrupt", STATS_OUT);
    {   /* SYM ids 1..4: the vector's maximum 5 is inside the domain, outside the chunk */
        uint8_t ids[4] = { 1, 2, 3, 4 };
        SAVE_SYM(ids, 4, p); W64(desc_at(0, 0) + 40, 5);    BAD(p, 1, "corrupt", STATS_OUT);
    }
    /* the second vector of a chunk: the chunk's range is the union */
    {
        ray_t* v = ray_vec_new(RAY_I32, 2048); v->len = 2048;
        for (int i = 0; i < 2048; i++) ((int32_t*)ray_data(v))[i] = i < 1024 ? 100 + i % 7 : 200 + i % 9;
        SAVE(v, p);
        TEST_ASSERT_EQ_U(rd64(DIR(0) + 16), 100); TEST_ASSERT_EQ_U(rd64(DIR(0) + 24), 208);
        W64(desc_at(0, 1) + 40, 209);                       BAD(p, 1, "corrupt", "vector 1 " STATS_OUT);
    }
    /* a vector without statistics under a chunk with them */
    SAVE(i32_for(), p); D = desc_at(0, 0);
    W16(D + 4, RAY_ENC_STAT_ABSENT); W64(D + 32, 0); W64(D + 40, 0);
    BAD(p, 1, "corrupt", "vector 0 statistics missing under the chunk's");
    SAVE(i32_for(), p); W32(DIR(0) + 56, RAY_ENC_STAT_ABSENT); W64(DIR(0) + 16, 0); W64(DIR(0) + 24, 0);
    BAD(p, 1, "corrupt", "vector 0 statistics under a chunk without them");
    /* statistics outside the type: a vector's (the type is checked before
     * the nesting) ... */
    SAVE(i32_for(), p); W64(desc_at(0, 0) + 40, 1ull << 40);
    BAD(p, 1, "corrupt", "vector 0 statistics outside the type");
    SAVE(fk_for(RAY_I16), p); W64(desc_at(0, 0) + 32, (uint64_t)(int64_t)-40000);
    BAD(p, 1, "corrupt", "vector 0 statistics outside the type");
    SAVE(bool_runs(), p); W64(desc_at(0, 0) + 40, 256);
    BAD(p, 1, "corrupt", "vector 0 statistics outside the type");
    /* ... and the chunk's, at load (fix round 1 of 14b: a directory
     * statistic sizes spans before any descriptor is read) */
    SAVE(i32_for(), p); W64(desc_at(0, 0) + 40, 1ull << 40); W64(DIR(0) + 24, 1ull << 40);
    BAD(p, 0, "corrupt", "chunk 0 statistics outside the type");
    SAVE(i32_for(), p); W64(DIR(0) + 24, (uint64_t)INT64_MAX);
    BAD(p, 0, "corrupt", "chunk 0 statistics outside the type");
    SAVE(i32_for(), p); W64(DIR(0) + 16, (uint64_t)INT64_MIN);
    BAD(p, 0, "corrupt", "chunk 0 statistics outside the type");
    SAVE(fk_for(RAY_I16), p); W64(desc_at(0, 0) + 32, (uint64_t)(int64_t)-40000); W64(DIR(0) + 16, (uint64_t)(int64_t)-40000);
    BAD(p, 0, "corrupt", "chunk 0 statistics outside the type");
    SAVE(bool_runs(), p); W64(desc_at(0, 0) + 40, 256); W64(DIR(0) + 24, 256);
    BAD(p, 0, "corrupt", "chunk 0 statistics outside the type");
    PASS();
}

/* The widened read of vector `vec` of the patched image serves it. */
static bool wide_serves(const char* path, int64_t vec) {
    if (!img_store(path, g_len)) return false;
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_error_free(c); return false; }
    ray_col_src_t src;
    ray_t* m = ray_col_src_bind(c, &src);
    if (m) { ray_error_free(m); ray_release(c); return false; }
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    static int64_t dst[RAY_ENC_VEC];
    int64_t got = -1;
    ray_t* e = ray_col_range_wide(&src, vec * RAY_ENC_VEC, RAY_ENC_VEC, &sc, RAY_I64, dst, &got);
    bool ok = !e && got == RAY_ENC_VEC;
    if (e) ray_error_free(e);
    ray_col_scratch_free(&sc); ray_col_src_release(&src);
    ray_release(c);
    return ok;
}

/* Vector `vec` of the patched image read twice by range through one
 * column: both reads succeed (decode does not hold these values to the
 * statistics; a consumer that sizes memory from them guards it). */
static const char* reads_ok(const char* path, int64_t vec) {
    static char why[512];
    if (!img_store(path, g_len)) return "store";
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_error_free(c); return "load failed"; }
    ray_col_src_t src;
    ray_t* m = ray_col_src_bind(c, &src);
    if (m) { ray_error_free(m); ray_release(c); return "bind failed"; }
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    const char* res = NULL;
    for (int i = 0; i < 2 && !res; i++) {
        ray_col_range_t r;
        ray_error_clear();
        ray_t* e = ray_col_range(&src, vec * RAY_ENC_VEC, RAY_ENC_VEC, &sc, &r);
        if (e) {
            snprintf(why, sizeof why, "read %d gave %s: \"%s\"", i + 1, RAY_IS_ERR(e) ? ray_err_code(e) : "?",
                     ray_error_msg() ? ray_error_msg() : "");
            res = why;
            ray_error_free(e);
        }
    }
    ray_col_scratch_free(&sc); ray_col_src_release(&src);
    ray_release(c);
    return res;
}
#define READS_OK(path, vec) do { const char* w_ = reads_ok(path, vec); \
    TEST_ASSERT_FMT(w_ == NULL, "%s:%d %s", __FILE__, __LINE__, w_); } while (0)

/* ---- Task 17a: the CONST / RLE / RAW kernels and the RAW view never accept what the reference rejects ---- */

/* 1024 I16 rows in 11 runs of 100 (1000, 1003, ...; the last 24 rows) ->
 * RLE with RAW children: [nr][0] @0, values hdr @8, body @16 (24 B), ends
 * hdr @40, body @48 (24 B).  nulls: rows 300..399 null (a bitmap first). */
static ray_t* rk_i16_runs(bool nulls) {
    ray_t* v = ray_vec_new(RAY_I16, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++)
        ((int16_t*)ray_data(v))[i] = nulls && i >= 300 && i < 400 ? NULL_I16 : (int16_t)(1000 + 3 * (i / 100));
    if (nulls) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}
/* 65536 + 2048 distinct I64 rows: the first chunk takes a VALUE_DICT, the
 * second chunk's two vectors are RAW without nulls (global vectors 64, 65). */
static ray_t* rk_i64_raw2(void) {
    ray_t* v = ray_vec_new(RAY_I64, 67584); v->len = 67584;
    for (int64_t i = 0; i < 67584; i++) {
        uint64_t h = (uint64_t)(i + 1) * 0x9E3779B97F4A7C15ull;      /* a 64-bit mix: no runs, no lane deltas */
        h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 29;
        ((int64_t*)ray_data(v))[i] = (int64_t)h == NULL_I64 ? 1 : (int64_t)h;
    }
    return v;
}

/* 1024 I32 rows of 7 -> CONST, 8 B: the value, then zero padding. */
static ray_t* rk_i32_const(void) {
    ray_t* v = ray_vec_new(RAY_I32, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((int32_t*)ray_data(v))[i] = 7;
    return v;
}

/* Range reads of vectors a and b in turn through one column object, twice:
 * a reads (decoded, not in place: its chunk's RAW check failed on b), b is
 * `corrupt` naming the path and the rule each time. */
static const char* view_memo(const char* path, int64_t a, int64_t b, const char* needle) {
    static char why[512];
    if (!img_store(path, g_len)) return "store";
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_error_free(c); return "load failed"; }
    ray_enccol_t* e = ray_enccol_of(c);
    ray_col_src_t src;
    ray_t* m = ray_col_src_bind(c, &src);
    if (m) { ray_error_free(m); ray_release(c); return "bind failed"; }
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    const char* res = NULL;
    for (int i = 0; i < 4 && !res; i++) {
        ray_col_range_t r;
        ray_error_clear();
        ray_t* err = ray_col_range(&src, (i & 1 ? b : a) * RAY_ENC_VEC, RAY_ENC_VEC, &sc, &r);
        if (!(i & 1)) {
            if (err || ((const uint8_t*)r.data >= e->base && (const uint8_t*)r.data < e->base + e->size)) {
                snprintf(why, sizeof why, "read %d of vector %lld gave %s", i + 1, (long long)a, err ? ray_err_code(err) : "a view in place");
                res = why;
            }
        } else if (!(err && RAY_IS_ERR(err) && strcmp(ray_err_code(err), "corrupt") == 0 && names_path(path) && has_needle(needle))) {
            snprintf(why, sizeof why, "read %d of vector %lld gave %s: \"%s\", want corrupt [%s]", i + 1, (long long)b,
                     err ? ray_err_code(err) : "a view", err && ray_error_msg() ? ray_error_msg() : "", needle);
            res = why;
        }
        if (err) ray_error_free(err);
    }
    ray_col_scratch_free(&sc); ray_col_src_release(&src);
    ray_release(c);
    return res;
}
#define VIEW_MEMO(path, a, b, needle) do { const char* w_ = view_memo(path, a, b, needle); \
    TEST_ASSERT_FMT(w_ == NULL, "%s:%d %s", __FILE__, __LINE__, w_); } while (0)

static test_result_t test_runs_kernel_corrupt_twice(void) {
    const char* p = ray_test_tmp(EC_TMP "/rk");
    size_t at;
    /* RLE I16 with RAW children */
#define RK_RUNS() do { SAVE(rk_i16_runs(false), p); NEED_ENC(0, 0, RAY_ENC_RLE); at = payload_at(0, 0); \
        TEST_ASSERT_TRUE(rd64(at) == 11 && g_img[at + 8] == RAY_ENC_RAW && g_img[at + 40] == RAY_ENC_RAW); } while (0)
    RK_RUNS(); W32(at + 4, 1);                      BAD2(p, 0, "RLE reserved word not zero");
    RK_RUNS(); W32(at, 0);                          BAD2(p, 0, "RLE run count out of range");
    RK_RUNS(); W32(at, 2000);                       BAD2(p, 0, "RLE run count out of range");
    RK_RUNS(); W8(at + 8 + 2, 1);                   BAD2(p, 0, "child header reserved bytes not zero");
    RK_RUNS(); W32(at + 8 + 4, 20);                 BAD2(p, 0, "child body size not a multiple of 8");
    RK_RUNS(); W32(at + 8 + 4, 4096);               BAD2(p, 0, "child body overruns its parent");
    RK_RUNS(); W8(at + 8, RAY_ENC_RLE);             BAD2(p, 0, "encoding illegal in this slot or depth");
    RK_RUNS(); W8(at + 40, RAY_ENC_CONST);          BAD2(p, 0, "encoding illegal in this slot or depth");
    RK_RUNS(); W8(at + 8 + 1, 3);                   BAD2(p, 0, "width on a non-packed encoding");
    RK_RUNS(); W8(at + 16 + 22, 1);                 BAD2(p, 0, "RAW padding not zero");
    RK_RUNS(); W16(at + 48 + 4, 150);               BAD2(p, 0, "RLE run ends not ascending or past the vector");
    RK_RUNS(); W16(at + 48 + 10, 2000);             BAD2(p, 0, "RLE run ends not ascending or past the vector");
    RK_RUNS(); W16(at + 48 + 20, 1023);             BAD2(p, 0, "RLE last run end is not the vector length");
    RK_RUNS(); W16(at + 16 + 6, 0x8000);            BAD2(p, 0, "sentinel outside the bitmap");
    {   /* the payload a word long, a word short */
        RK_RUNS(); uint64_t pb = rd64(desc_at(0, 0) + 16);
        W64(desc_at(0, 0) + 16, pb + 8); set_chunk_bytes(0, 3072 + pb + 8);  BAD2(p, 0, "bytes after the codec body");
        RK_RUNS();
        W64(desc_at(0, 0) + 16, pb - 8); set_chunk_bytes(0, 3072 + pb - 8);  BAD2(p, 0, "child body overruns its parent");
    }
    /* with a bitmap (rows 300..399): a run outside it on the sentinel is
     * corrupt; the run under it on the sentinel reads */
    SAVE(rk_i16_runs(true), p); NEED_ENC(0, 0, RAY_ENC_RLE); at = payload_at(0, 0) + 128;
    TEST_ASSERT_TRUE(rd64(desc_at(0, 0)) >> 16 & 0xFFFF);
    TEST_ASSERT_TRUE(rd64(at) == 11 && g_img[at + 8] == RAY_ENC_RAW);
    W16(at + 16 + 4, 0x8000);                       BAD2(p, 0, "sentinel outside the bitmap");   /* rows 200..299 */
    SAVE(rk_i16_runs(true), p); at = payload_at(0, 0) + 128;
    W16(at + 16 + 6, 0x8000);                       READS_OK(p, 0);                                /* rows 300..399 */
    /* CONST I32 */
    SAVE(rk_i32_const(), p); NEED_ENC(0, 0, RAY_ENC_CONST); at = payload_at(0, 0);
    W32(at, 0x80000000u);                           BAD2(p, 0, "sentinel outside the bitmap");
    SAVE(rk_i32_const(), p); at = payload_at(0, 0);
    W8(at + 5, 1);                                  BAD2(p, 0, "RAW padding not zero");
    /* SYM RLE (ids 1 + i / 100, bound 200): values RAW 11 B @16, ends hdr @32 */
    {
        uint8_t ids[1024];
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(1 + i / 100);
        SAVE_SYM(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_RLE); at = payload_at(0, 0);
        TEST_ASSERT_TRUE(g_img[at + 8] == RAY_ENC_RAW && g_img[at + 32] == RAY_ENC_RAW);
        W8(at + 16 + 3, 250);                       BAD2(p, 0, "SYM id outside the symbol domain");
        SAVE_SYM(ids, 1024, p); at = payload_at(0, 0);
        W8(at + 16 + 3, 0);                         BAD2(p, 0, "SYM null count");
        for (int i = 0; i < 1024; i++) ids[i] = 5;
        SAVE_SYM(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_CONST); at = payload_at(0, 0);
        W8(at, 0);                                  BAD2(p, 0, "SYM null count");
        SAVE_SYM(ids, 1024, p); at = payload_at(0, 0);
        W8(at, 250);                                BAD2(p, 0, "SYM id outside the symbol domain");
        /* RAW SYM (read in place when sound), 1023 ids: one byte of padding */
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(1 + ((uint32_t)i * 2654435761u >> 24) % 199);
        SAVE_SYM(ids, 1023, p); NEED_ENC(0, 0, RAY_ENC_RAW); at = payload_at(0, 0);
        W8(at + 1023, 1);                           BAD2(p, 0, "RAW padding not zero");
        SAVE_SYM(ids, 1023, p); at = payload_at(0, 0);
        W8(at + 700, 250);                          BAD2(p, 0, "SYM id outside the symbol domain");
        SAVE_SYM(ids, 1023, p); at = payload_at(0, 0);
        W8(at + 700, 0);                            BAD2(p, 0, "SYM null count");
    }
    /* RAW I32 with a bitmap (decoded, not read in place) */
    SAVE(i32_nulls_raw(), p); NEED_ENC(0, 0, RAY_ENC_RAW); at = payload_at(0, 0);
    W32(at + 128 + 4, (uint32_t)NULL_I32);          BAD2(p, 0, "sentinel outside the bitmap");
    /* RAW I64 without nulls (read in place when sound), chunk 1 */
    SAVE(rk_i64_raw2(), p); NEED_ENC(1, 0, RAY_ENC_RAW); NEED_ENC(1, 1, RAY_ENC_RAW);
    TEST_ASSERT_EQ_U(rd64(desc_at(1, 1)) >> 16 & 0xFFFF, 0);
    W64(payload_at(1, 1) + 8 * 5, (uint64_t)1 << 63);   BAD2(p, 65, "sentinel outside the bitmap");
    /* one corrupt vector: its chunk is never cached as checked, so the
     * sound vector beside it is decoded, and the corrupt one reported, on
     * every read */
    VIEW_MEMO(p, 64, 65, "sentinel outside the bitmap");
#undef RK_RUNS
    PASS();
}

/* The decode-side contract for a descriptor moved inward past a real value
 * (still inside its chunk's): the encodings whose check is free to fuse
 * (DICT's gather, SYM's id check, the floats' NaN scan) are `corrupt` on
 * the first read and the second; FOR_BP, DELTA_BP, RLE and RAW integer
 * vectors, read natively or widened, decode as stored: the consumers that
 * size memory from statistics guard them (scanrange/v2_forged_*). */
static test_result_t test_stats_values(void) {
    const char* p = ray_test_tmp(EC_TMP "/sv");
    size_t D;
    for (int t = 0; t < 2; t++) {
        int8_t type = t ? RAY_I32 : RAY_I16;
        SAVE(fk_for(type), p); NEED_ENC(0, 0, RAY_ENC_FOR_BP); D = desc_at(0, 0);
        W64(D + 40, rd64(D + 40) - 1);                      READS_OK(p, 0);
        TEST_ASSERT_TRUE(wide_serves(p, 0));
        SAVE(fk_for_nulls(type), p); NEED_ENC(0, 0, RAY_ENC_FOR_BP); D = desc_at(0, 0);
        W64(D + 32, rd64(D + 32) + 1);                      READS_OK(p, 0);
    }
    SAVE(fk_delta(), p); NEED_ENC(0, 1, RAY_ENC_DELTA_BP); D = desc_at(0, 1);
    W64(D + 40, rd64(D + 40) - 1);                          READS_OK(p, 1);
    SAVE(bool_runs(), p); NEED_ENC(0, 0, RAY_ENC_RLE);
    W64(desc_at(0, 0) + 40, 0);                             READS_OK(p, 0);
    SAVE(i32_nulls_raw(), p); NEED_ENC(0, 0, RAY_ENC_RAW); D = desc_at(0, 0);
    W64(D + 40, rd64(D + 40) - 1);                          READS_OK(p, 0);
    /* DICT over FOR_BP codes: the kernel's gather (values 11, 77, 900000000001, 123456789012345) */
    SAVE(i64_dict(), p); NEED_ENC(0, 0, RAY_ENC_DICT); D = desc_at(0, 0);
    W64(D + 40, 900000000001ull);                           BAD2(p, 0, VALUE_OUT);
    EXPECT(p, 1, "corrupt", VALUE_OUT);
    SAVE(i64_dict(), p); D = desc_at(0, 0);
    W64(D + 32, 77);                                        BAD2(p, 0, VALUE_OUT);
    {   /* SYM FOR_BP, ids 1..32 */
        uint8_t ids[1024];
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(1 + ((uint32_t)i * 2654435761u >> 27));
        SAVE_SYM(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_FOR_BP); D = desc_at(0, 0);
        TEST_ASSERT_EQ_U(rd64(D + 32), 1); TEST_ASSERT_EQ_U(rd64(D + 40), 32);
        W64(D + 40, 31);                                    BAD2(p, 0, VALUE_OUT);
        SAVE_SYM(ids, 1024, p); W64(desc_at(0, 0) + 32, 2); BAD2(p, 0, VALUE_OUT);
        /* the reference decode of a SYM RLE vector */
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(1 + i / 100);
        SAVE_SYM(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_RLE);
        W64(desc_at(0, 0) + 40, 10);                        BAD2(p, 0, VALUE_OUT);
    }
    /* integer RAW without nulls, read in place: I64 as decoded (not held
     * to the statistics), SYM checked as its decode checks it */
    SAVE(rk_i64_raw2(), p); NEED_ENC(1, 0, RAY_ENC_RAW); D = desc_at(1, 0);
    W64(D + 40, rd64(D + 40) - 1);                          READS_OK(p, 64);
    {
        uint8_t ids[1024];
        for (int i = 0; i < 1024; i++) ids[i] = (uint8_t)(1 + ((uint32_t)i * 2654435761u >> 24) % 199);
        SAVE_SYM(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_RAW); D = desc_at(0, 0);
        W64(D + 40, rd64(D + 40) - 1);                      BAD2(p, 0, VALUE_OUT);
        SAVE_SYM(ids, 1024, p); D = desc_at(0, 0);
        W64(D + 32, rd64(D + 32) + 1);                      BAD2(p, 0, VALUE_OUT);
        /* CONST SYM 9: the chunk widened to [9, 10], the vector moved to [10, 10] */
        for (int i = 0; i < 1024; i++) ids[i] = 9;
        SAVE_SYM(ids, 1024, p); NEED_ENC(0, 0, RAY_ENC_CONST); D = desc_at(0, 0);
        TEST_ASSERT_EQ_U(rd64(D + 40), 9);
        W64(DIR(0) + 24, 10); W64(D + 32, 10); W64(D + 40, 10); BAD2(p, 0, VALUE_OUT);
    }
    /* floats: RAW without nulls (read in place), with a NaN null (the copy) */
    SAVE(f64_stats(), p); NEED_ENC(0, 0, RAY_ENC_RAW);
    { double hi = 98.0; wr(desc_at(0, 0) + 40, &hi, 8); }  BAD2(p, 0, VALUE_OUT);
    SAVE(f64_stats(), p); { double lo = 0.5; wr(desc_at(0, 0) + 32, &lo, 8); } BAD2(p, 0, VALUE_OUT);
    {
        ray_t* f = f64_stats();
        ((double*)ray_data(f))[10] = NAN; f->attrs |= RAY_ATTR_HAS_NULLS;
        SAVE(f, p); TEST_ASSERT_TRUE(rd64(desc_at(0, 0)) >> 16 & 0xFFFF);
        double hi = 98.0; wr(desc_at(0, 0) + 40, &hi, 8);   BAD2(p, 0, VALUE_OUT);
    }
    SAVE(f32_plain(), p); { double hi = 2.5; wr(desc_at(0, 0) + 40, &hi, 8); } BAD2(p, 0, VALUE_OUT);
    PASS();
}

/* The consumer guard at the range read (ray_col_src_t.stats_guard): a
 * value outside its chunk's directory statistics is `corrupt` naming the
 * path, on both reads; an unpatched vector reads; a widened read declines
 * so the range read checks; the materialization is checked once. */
static const char* guard_reads(const char* path, int64_t vec, bool want_bad) {
    static char why[512];
    if (!img_store(path, g_len)) return "store";
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_error_free(c); return "load failed"; }
    ray_col_src_t src;
    ray_t* m = ray_col_src_bind(c, &src);
    if (m) { ray_error_free(m); ray_release(c); return "bind failed"; }
    src.stats_guard = true;
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    const char* res = NULL;
    static int64_t dst[RAY_ENC_VEC];
    int64_t got = -1;
    ray_t* we = ray_col_range_wide(&src, vec * RAY_ENC_VEC, RAY_ENC_VEC, &sc, RAY_I64, dst, &got);
    if (we || got != 0) { res = "the widened read did not decline under the guard"; if (we) ray_error_free(we); }
    for (int i = 0; i < 2 && !res; i++) {
        ray_col_range_t r;
        ray_error_clear();
        ray_t* e = ray_col_range(&src, vec * RAY_ENC_VEC, RAY_ENC_VEC, &sc, &r);
        bool bad = e && RAY_IS_ERR(e) && strcmp(ray_err_code(e), "corrupt") == 0 && names_path(path) &&
                   has_needle("value outside the chunk's statistics");
        if (want_bad ? !bad : e != NULL) {
            snprintf(why, sizeof why, "guarded read %d gave %s: \"%s\"", i + 1, e ? ray_err_code(e) : "a view",
                     e && ray_error_msg() ? ray_error_msg() : "");
            res = why;
        }
        if (e) ray_error_free(e);
    }
    /* through a cursor over the published materialization */
    if (!res) {
        ray_t* mat = ray_enccol_materialize(c);
        if (mat && !RAY_IS_ERR(mat)) {
            for (int i = 0; i < 2 && !res; i++) {
                ray_col_cursor_t cur; ray_col_cursor_init(&cur, &src);
                const ray_col_range_t* v;
                ray_error_clear();
                ray_t* e = ray_col_cursor_at(&cur, vec * RAY_ENC_VEC, &v);
                bool bad = e && strcmp(ray_err_code(e), "corrupt") == 0 && names_path(path);
                if (want_bad ? !bad : e != NULL) {
                    snprintf(why, sizeof why, "materialized read %d gave %s", i + 1, e ? ray_err_code(e) : "a view");
                    res = why;
                }
                if (e) ray_error_free(e);
                ray_col_cursor_free(&cur);
            }
            /* the plain and in-place accessors (the serial strategies'
             * unstage, the walks' base pointers): the same check, so a
             * guarded source never hands out unchecked values */
            for (int i = 0; i < 2 && !res; i++) {
                ray_t* rd = ray_col_src_ready(&src);
                const void* bp = ray_col_src_base(&src);
                if (want_bad ? (rd || bp) : (rd != mat || bp != ray_data(mat))) {
                    snprintf(why, sizeof why, "ready read %d gave %s", i + 1, rd ? "the materialization" : "nothing");
                    res = why;
                    break;
                }
                ray_t* pl = NULL;
                ray_error_clear();
                ray_t* e = ray_col_src_plain(&src, &pl);
                bool bad = e && strcmp(ray_err_code(e), "corrupt") == 0 && names_path(path) &&
                           has_needle("value outside the chunk's statistics") && !pl;
                if (want_bad ? !bad : (e != NULL || pl != mat)) {
                    snprintf(why, sizeof why, "plain read %d gave %s", i + 1, e ? ray_err_code(e) : "the column");
                    res = why;
                }
                if (e) ray_error_free(e);
            }
            /* unguarded, the materialization is ready as ever */
            if (!res) {
                ray_col_src_t un;
                ray_t* be = ray_col_src_bind(c, &un);
                if (be) { ray_error_free(be); res = "bind failed"; }
                else {
                    ray_t* pl = NULL;
                    ray_t* e = ray_col_src_plain(&un, &pl);
                    if (ray_col_src_ready(&un) != mat || e || pl != mat) res = "the unguarded source did not read in place";
                    if (e) ray_error_free(e);
                    ray_col_src_release(&un);
                }
            }
        } else if (mat) ray_error_free(mat);
    }
    ray_col_scratch_free(&sc); ray_col_src_release(&src);
    ray_release(c);
    return res;
}
#define GUARD_READS(path, vec, bad) do { const char* w_ = guard_reads(path, vec, bad); \
    TEST_ASSERT_FMT(w_ == NULL, "%s:%d %s", __FILE__, __LINE__, w_); } while (0)

static test_result_t test_stats_guard(void) {
    const char* p = ray_test_tmp(EC_TMP "/sg");
    for (int t = 0; t < 2; t++) {
        int8_t type = t ? RAY_I32 : RAY_I16;
        SAVE(fk_for(type), p); GUARD_READS(p, 0, false);
        /* the chunk's maximum (and the vector's, to nest) below a real value */
        uint64_t mx = rd64(DIR(0) + 24);
        W64(DIR(0) + 24, mx - 1); W64(desc_at(0, 0) + 40, mx - 1);   GUARD_READS(p, 0, true);
        SAVE(fk_for(type), p);
        uint64_t mn = rd64(DIR(0) + 16);
        W64(DIR(0) + 16, mn + 1); W64(desc_at(0, 0) + 32, mn + 1);   GUARD_READS(p, 0, true);
        /* with a null bitmap: the nulls take no part */
        SAVE(fk_for_nulls(type), p); GUARD_READS(p, 0, false);
        mx = rd64(DIR(0) + 24);
        W64(DIR(0) + 24, mx - 1); W64(desc_at(0, 0) + 40, mx - 1);   GUARD_READS(p, 0, true);
    }
    SAVE(fk_delta(), p); GUARD_READS(p, 1, false);
    W64(DIR(0) + 24, rd64(DIR(0) + 24) - 1); W64(desc_at(0, 1) + 40, rd64(DIR(0) + 24));
    GUARD_READS(p, 1, true);
    /* RLE (I16, RAW children) and RAW I64 read in place (chunk 1) */
    SAVE(rk_i16_runs(false), p); GUARD_READS(p, 0, false);
    W64(DIR(0) + 24, rd64(DIR(0) + 24) - 1); W64(desc_at(0, 0) + 40, rd64(DIR(0) + 24));
    GUARD_READS(p, 0, true);
    SAVE(rk_i64_raw2(), p); NEED_ENC(1, 0, RAY_ENC_RAW); GUARD_READS(p, 64, false);
    {   /* the chunk's maximum (and its vector's) one below the real one */
        const int top = (int64_t)rd64(desc_at(1, 0) + 40) > (int64_t)rd64(desc_at(1, 1) + 40) ? 0 : 1;
        TEST_ASSERT_EQ_U(rd64(desc_at(1, top) + 40), rd64(DIR(1) + 24));
        W64(DIR(1) + 24, rd64(DIR(1) + 24) - 1); W64(desc_at(1, top) + 40, rd64(DIR(1) + 24));
        GUARD_READS(p, 64 + top, true);
    }
    {   /* I64 and U8 */
        ray_t* v = ray_vec_new(RAY_I64, 1024); v->len = 1024;
        for (int i = 0; i < 1024; i++) ((int64_t*)ray_data(v))[i] = -5000000000ll + i % 300;
        SAVE(v, p); GUARD_READS(p, 0, false);
        W64(DIR(0) + 16, (uint64_t)(-5000000000ll + 1)); W64(desc_at(0, 0) + 32, (uint64_t)(-5000000000ll + 1));
        GUARD_READS(p, 0, true);
        ray_t* u = ray_vec_new(RAY_U8, 1024); u->len = 1024;
        for (int i = 0; i < 1024; i++) ((uint8_t*)ray_data(u))[i] = (uint8_t)(10 + i % 200);
        SAVE(u, p); GUARD_READS(p, 0, false);
        W64(DIR(0) + 24, 208); W64(desc_at(0, 0) + 40, 208);         GUARD_READS(p, 0, true);
    }
    PASS();
}

/* The point read (the samplers' row read): a row above a descriptor
 * maximum patched below it is corrupt, naming the path, on every read; a
 * row inside it is served.  FOR_BP and DICT. */
static const char* point_reads(const char* path, const int64_t* want, int64_t n, int64_t over, bool patched) {
    static char why[512];
    if (!img_store(path, g_len)) return "store";
    ray_t* c = ray_col_mmap(path);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_error_free(c); return "load failed"; }
    ray_col_src_t src;
    ray_t* m = ray_col_src_bind(c, &src);
    if (m) { ray_error_free(m); ray_release(c); return "bind failed"; }
    const char* res = NULL;
    int64_t bad = 0;
    for (int pass = 0; pass < 2 && !res; pass++)
        for (int64_t r = 0; r < n && !res; r++) {
            int64_t x = 0; ray_t* err = NULL;
            ray_error_clear();
            int pr = ray_col_src_point_i64(&src, r, &x, &err);
            if (want[r] > over) {
                bool ok = pr == -1 && err && strcmp(ray_err_code(err), "corrupt") == 0 && names_path(path) && has_needle(VALUE_OUT);
                if (!ok) { snprintf(why, sizeof why, "pass %d row %lld (%lld): point read %d \"%s\", want corrupt", pass, (long long)r,
                                    (long long)want[r], pr, err && ray_error_msg() ? ray_error_msg() : ""); res = why; }
                bad += pass == 0;
            } else if (pr != 1 || x != want[r]) {
                snprintf(why, sizeof why, "pass %d row %lld: point read %d value %lld, want %lld", pass, (long long)r, pr,
                         (long long)x, (long long)want[r]);
                res = why;
            }
            if (err) ray_error_free(err);
        }
    if (!res && patched != (bad > 0)) res = patched ? "no row above the patched maximum" : "a row above the maximum";
    ray_col_src_release(&src);
    ray_release(c);
    return res;
}
#define POINT_READS(path, want, n, over, patched) do { const char* w_ = point_reads(path, want, n, over, patched); \
    TEST_ASSERT_FMT(w_ == NULL, "%s:%d %s", __FILE__, __LINE__, w_); } while (0)

static test_result_t test_stats_point_read(void) {
    const char* p = ray_test_tmp(EC_TMP "/sp");
    static int64_t want[1024];
    ray_t* v = i32_for();
    for (int i = 0; i < 1024; i++) want[i] = ((int32_t*)ray_data(v))[i];
    SAVE(v, p); NEED_ENC(0, 0, RAY_ENC_FOR_BP);
    int64_t mx = (int64_t)rd64(desc_at(0, 0) + 40);
    POINT_READS(p, want, 1024, mx, false);                   /* control: every row served */
    W64(desc_at(0, 0) + 40, (uint64_t)(mx - 1));
    POINT_READS(p, want, 1024, mx - 1, true);
    v = i64_dict();
    for (int i = 0; i < 1024; i++) want[i] = ((int64_t*)ray_data(v))[i];
    SAVE(v, p); NEED_ENC(0, 0, RAY_ENC_DICT);
    POINT_READS(p, want, 1024, INT64_MAX, false);
    W64(desc_at(0, 0) + 40, 900000000001ull);
    POINT_READS(p, want, 1024, 900000000001ll, true);
    PASS();
}

/* Damage in a range nobody reads stays unread; whoever reads it gets corrupt. */
static test_result_t test_lazy_validation(void) {
    const char* p = ray_test_tmp(EC_TMP "/lazy");
    ray_t* v = ray_vec_new(RAY_I32, 2 * 65536); v->len = 2 * 65536;
    for (int i = 0; i < 2 * 65536; i++) ((int32_t*)ray_data(v))[i] = i % 100;
    SAVE(v, p);
    W8(desc_at(1, 0) + 6, 1); TEST_ASSERT_TRUE(img_store(p, g_len));
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    TEST_ASSERT_NULL(ray_col_range(&s, 0, 1024, &sc, &r));               /* chunk 0 fine */
    ray_error_clear();
    ray_t* e = ray_col_range(&s, 65536, 1024, &sc, &r);                  /* chunk 1 read */
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e) && strcmp(ray_err_code(e), "corrupt") == 0);
    TEST_ASSERT_TRUE(has_needle("descriptor reserved fields") && names_path(p));
    ray_error_free(e);
    ray_col_scratch_free(&sc); ray_col_src_release(&s);
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && RAY_IS_ERR(m));
    ray_error_free(m);
    ray_release(c);
    /* the aware path: an out-of-domain SYM id fails a range read of its own vector, not its neighbours */
    uint8_t ids[2048];
    for (int i = 0; i < 2048; i++) ids[i] = (uint8_t)(1 + i % 7);
    SAVE_SYM(ids, 2048, p); NEED_ENC(0, 0, RAY_ENC_FOR_BP);
    W64(payload_at(0, 1), 199);   /* second vector: ids 199.. >= the saved count 200 */
    TEST_ASSERT_TRUE(img_store(p, g_len));
    c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    TEST_ASSERT_NULL(ray_col_src_bind(c, &s)); ray_col_scratch_init(&sc);
    TEST_ASSERT_NULL(ray_col_range(&s, 0, 1024, &sc, &r));
    ray_error_clear();
    e = ray_col_range(&s, 1024, 1024, &sc, &r);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e) && strcmp(ray_err_code(e), "corrupt") == 0);
    TEST_ASSERT_TRUE(has_needle("SYM id outside the symbol domain") && names_path(p));
    ray_error_free(e);
    ray_col_scratch_free(&sc); ray_col_src_release(&s);
    ray_release(c);
    PASS();
}

static uint64_t g_fz;
static uint64_t fz(void) { uint64_t x = g_fz; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return g_fz = x; }

static bool ec_allowed_code(const char* c) {
    return strcmp(c, "corrupt") == 0 || strcmp(c, "nyi") == 0 || strcmp(c, "version") == 0;
}

/* One mutated image: rejected (true) or a well-formed result (false); `bad` set on a violation. */
static bool fuzz_one(const char* p, const char** bad) {
    ray_t* c = ray_col_mmap(p);
    bool rejected = false;
    if (c && !RAY_IS_ERR(c)) {
        ray_enccol_t* e = ray_enccol_of(c);
        int8_t want_type = e ? e->type : 0; int64_t want_len = e ? e->len : -1;
        ray_t* m = ray_enccol_materialize(c);
        if (m && RAY_IS_ERR(m)) {
            if (!ec_allowed_code(ray_err_code(m))) *bad = "materialize: error code outside {corrupt, nyi, version}";
            ray_error_free(m); rejected = true;
        } else if (!m) *bad = "materialize returned NULL";
        else if (m->type != want_type || m->len != want_len) *bad = "materialized vector disagrees with the header";
        else if (ray_index_kind(m) == RAY_IDX_HASH) {
            /* an accepted region may answer wrongly, never unsafely: every
             * probed row is in bounds and the row list ascends */
            for (int64_t key = 0; key < 8 && !*bad; key++) {
                const int64_t* rows = NULL; int64_t n = 0;
                if (ray_index_hash_group(m, key, &rows, &n) != 1) continue;
                for (int64_t i = 0; i < n && !*bad; i++)
                    if (rows[i] < 0 || rows[i] >= m->len || (i && rows[i] <= rows[i - 1]))
                        *bad = "hash probe: row out of bounds or out of order";
            }
        }
        ray_release(c);
    } else if (c) {
        if (!ec_allowed_code(ray_err_code(c))) *bad = "load: error code outside {corrupt, nyi, version}";
        ray_error_free(c); rejected = true;
    } else *bad = "ray_col_mmap returned NULL";
    return rejected;
}

/* .idx.info reads descriptors without decoding: a forged encoding byte is
 * "corrupt", not a wrong report. */
static test_result_t test_idx_info_corrupt(void) {
    const char* p = ray_test_tmp(EC_TMP "/info");
    SAVE(bool_runs(), p); NEED_ENC(0, 0, RAY_ENC_RLE);
    W8(desc_at(0, 0), 0xFF);
    TEST_ASSERT_TRUE(img_store(p, g_len));
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_info_t ci;
    ray_t* err = ray_enccol_info(c, &ci);
    TEST_ASSERT_TRUE(err && RAY_IS_ERR(err) && strcmp(ray_err_code(err), "corrupt") == 0);
    ray_error_free(err);
    int64_t nid = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nid, c);
    TEST_ASSERT_TRUE(tbl && !RAY_IS_ERR(tbl));
    ray_t* sym = ray_sym(nid);
    ray_t* args[2] = { tbl, sym };
    ray_t* e = ray_idx_info_vary_fn(args, 2);
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e) && strcmp(ray_err_code(e), "corrupt") == 0);
    ray_error_free(e);
    ray_release(sym); ray_release(tbl); ray_release(c);
    PASS();
}

/* Random byte flips: an error or a well-formed result, never a crash, a
 * hang or a read outside the mapping (ASan/UBSan).  Whole-file flips must
 * reject something; flips confined to the headers, directory and
 * descriptors (everything before the first payload) must reject nearly all. */
static test_result_t test_fuzz_flips(void) {
    const char* p = ray_test_tmp(EC_TMP "/fz");
    g_fz = 0xC0FFEE;
    int rej_all_total = 0, structural_total = 0, structural_rej = 0;
    for (int base = 0; base < 6; base++) {
        ray_t* v;
        if (base == 0) v = bool_runs();
        else if (base == 1) { v = ray_vec_new(RAY_I32, 70000); v->len = 70000;
            for (int i = 0; i < 70000; i++) ((int32_t*)ray_data(v))[i] = i % 7 ? i % 300 : NULL_I32; }
        else if (base == 2) v = f64_two_odd();
        else if (base == 3) { v = ray_vec_new(RAY_STR, 50);
            for (int i = 0; i < 50; i++) v = ray_str_vec_append(v, i % 3 ? "a long pooled string value" : "s", i % 3 ? 26 : 1); }
        else if (base == 4) v = i64_dict();
        else {   /* an indexed column: flips land in its index region too */
            v = ray_vec_new(RAY_I64, 70000); v->len = 70000;
            for (int i = 0; i < 70000; i++) ((int64_t*)ray_data(v))[i] = (i * 13) % 300;
            ray_t* r = ray_index_attach_hash(&v);
            TEST_ASSERT_FMT(r && !RAY_IS_ERR(r), "attach hash failed");
            v = r;
        }
        SAVE(v, p);
        uint8_t* orig = malloc(g_len); TEST_ASSERT_TRUE(orig != NULL); memcpy(orig, g_img, g_len);
        size_t first_payload = (size_t)rd64(DIR(0)) + 3072;
        size_t index_off = (size_t)rd64(EH + 32);
        if (base == 5) {
            TEST_ASSERT_FMT(g_img[EH + 6] == RAY_IDX_HASH && index_off > first_payload && index_off < g_len,
                            "indexed column: kind %d, region at %zu of %zu", g_img[EH + 6], index_off, g_len);
            ray_t* c = ray_col_mmap(p);                       /* the unmutated file attaches its HASH */
            TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
            ray_t* m = ray_enccol_materialize(c);
            bool hashed = m && !RAY_IS_ERR(m) && ray_index_kind(m) == RAY_IDX_HASH;
            if (m && RAY_IS_ERR(m)) ray_error_free(m);
            ray_release(c);
            TEST_ASSERT_TRUE(hashed);
        }
        int rejected = 0;
        for (int it = 0; it < 300; it++) {
            memcpy(g_img, orig, g_len);
            int flips = 1 + (int)(fz() % 3);
            for (int k = 0; k < flips; k++) {
                size_t pos = (size_t)(fz() % g_len); uint8_t bit = (uint8_t)(1u << (fz() % 8));
                g_img[pos] ^= bit;
            }
            const char* bad = NULL;
            if (!img_store(p, g_len)) bad = "could not write the mutated image";
            else if (fuzz_one(p, &bad)) rejected++;
            if (bad) { free(orig); TEST_ASSERT_FMT(0, "base %d iter %d: %s", base, it, bad); }
        }
        TEST_ASSERT_FMT(rejected > 0, "base %d: %d of 300 mutated images rejected", base, rejected);
        rej_all_total += rejected;
        /* structural region only: one flip each in [0, first_payload) */
        for (int it = 0; it < 200; it++) {
            memcpy(g_img, orig, g_len);
            size_t pos = (size_t)(fz() % first_payload); uint8_t bit = (uint8_t)(1u << (fz() % 8));
            g_img[pos] ^= bit;
            const char* bad = NULL;
            if (!img_store(p, g_len)) bad = "could not write the mutated image";
            else { structural_total++; if (fuzz_one(p, &bad)) structural_rej++; }
            if (bad) { free(orig); TEST_ASSERT_FMT(0, "base %d structural iter %d: %s", base, it, bad); }
        }
        if (base == 5) {   /* index region only: one to three flips in [index_off, end) */
            int region_rej = 0;
            for (int it = 0; it < 300; it++) {
                memcpy(g_img, orig, g_len);
                int flips = 1 + (int)(fz() % 3);
                for (int k = 0; k < flips; k++) {
                    size_t pos = index_off + (size_t)(fz() % (g_len - index_off)); uint8_t bit = (uint8_t)(1u << (fz() % 8));
                    g_img[pos] ^= bit;
                }
                const char* bad = NULL;
                if (!img_store(p, g_len)) bad = "could not write the mutated image";
                else if (fuzz_one(p, &bad)) region_rej++;
                if (bad) { free(orig); TEST_ASSERT_FMT(0, "region iter %d: %s", it, bad); }
            }
            TEST_ASSERT_FMT(region_rej > 0, "index region: %d of 300 mutated images rejected", region_rej);
        }
        free(orig);
    }
    TEST_ASSERT_FMT(structural_rej * 100 >= structural_total * STRUCT_MIN_PCT,
                    "structural flips rejected %d of %d", structural_rej, structural_total);
    (void)rej_all_total;
    PASS();
}

const test_entry_t enccorrupt_entries[] = {
    { "enccorrupt/headers",       test_reject_headers,       ec_setup, ec_teardown },
    { "enccorrupt/directory",     test_reject_directory,     ec_setup, ec_teardown },
    { "enccorrupt/side_data",     test_reject_side_data,     ec_setup, ec_teardown },
    { "enccorrupt/descriptors",   test_reject_descriptors,   ec_setup, ec_teardown },
    { "enccorrupt/payloads",      test_reject_payloads,      ec_setup, ec_teardown },
    { "enccorrupt/codec_grammar", test_reject_codec_grammar, ec_setup, ec_teardown },
    { "enccorrupt/depth4",        test_reject_depth4,        ec_setup, ec_teardown },
    { "enccorrupt/lazy",          test_lazy_validation,      ec_setup, ec_teardown },
    { "enccorrupt/fast_kernel_twice", test_fast_kernel_corrupt_twice, ec_setup, ec_teardown },
    { "enccorrupt/runs_kernel_twice", test_runs_kernel_corrupt_twice, ec_setup, ec_teardown },
    { "enccorrupt/stats_nested",  test_stats_nested,         ec_setup, ec_teardown },
    { "enccorrupt/stats_values",  test_stats_values,         ec_setup, ec_teardown },
    { "enccorrupt/stats_point_read", test_stats_point_read,  ec_setup, ec_teardown },
    { "enccorrupt/stats_guard",   test_stats_guard,          ec_setup, ec_teardown },
    { "enccorrupt/idx_info",      test_idx_info_corrupt,     ec_setup, ec_teardown },
    { "enccorrupt/fuzz_flips",    test_fuzz_flips,           ec_setup, ec_teardown },
    { NULL, NULL, NULL, NULL },
};
