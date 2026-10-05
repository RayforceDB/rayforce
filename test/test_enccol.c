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
#include "ops/ops.h"
#include "table/sym.h"
#include "core/runtime.h"
#include "store/col.h"
#include "store/encfmt.h"
#include "store/enc.h"
#include "store/enccol.h"
#include "vec/str.h"
#include "store/splay.h"
#include "store/csr.h"
#include "table/domain.h"
#include "table/table.h"
#include "ops/idxop.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENCCOL_TMP "/tmp/rayforce_enccol"

/* 256 interned symbols, so SYM test ids up to 255 lie inside the runtime domain. */
static void enccol_setup(void) {
    ray_heap_init(); (void)ray_sym_init(); (void)ray_test_mkdir_p(ENCCOL_TMP);
    for (int i = 0; i < 256; i++) { char b[8]; int n = snprintf(b, sizeof b, "s%d", i); (void)ray_sym_intern(b, (size_t)n); }
}
static void enccol_teardown(void) { ray_sym_destroy(); ray_heap_destroy(); (void)ray_test_rm_rf(ENCCOL_TMP); }

static ray_err_t save_i32_rows(const char* path, int64_t n) {
    ray_t* v = ray_vec_new(RAY_I32, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int32_t*)ray_data(v))[i] = (int32_t)(i % 977);
    ray_err_t e = ray_col_save_gen(v, path, 2);
    ray_release(v);
    return e;
}

/* Patch `n` bytes at file offset `off`. */
static void patch(const char* path, long off, const void* b, size_t n) {
    FILE* f = fopen(path, "r+b"); fseek(f, off, SEEK_SET); fwrite(b, 1, n, f); fclose(f);
}

static ray_t* mmap_expect_err(const char* path, const char* code) {
    ray_t* r = ray_col_mmap(path);
    if (!r || !RAY_IS_ERR(r)) { if (r) ray_release(r); return ray_error("fail", "loaded"); }
    if (strcmp(ray_err_code(r), code) != 0) { ray_error_free(r); return ray_error("fail", "code"); }
    ray_error_free(r);
    return NULL;
}

static test_result_t test_open_returns_enccol(void) {
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    TEST_ASSERT_EQ_I(save_i32_rows(ENCCOL_TMP "/a", 70000), RAY_OK);
    ray_t* c = ray_col_mmap(ENCCOL_TMP "/a");
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->type, RAY_ENCCOL);
    TEST_ASSERT_EQ_I(c->len, 70000);
    TEST_ASSERT_EQ_I(c->attrs, 0);
    ray_enccol_t* e = ray_enccol_of(c);
    TEST_ASSERT_EQ_I(e->kind, RAY_ENCCOL_SINGLE);
    TEST_ASSERT_EQ_I(e->type, RAY_I32);
    TEST_ASSERT_EQ_I(e->n_chunks, 2);
    TEST_ASSERT_EQ_I(e->null_count, 0);
    ray_t* copy = ray_alloc_copy(c);                 /* header copy shares the payload */
    TEST_ASSERT_TRUE(copy && !RAY_IS_ERR(copy));
    TEST_ASSERT_EQ_PTR(ray_enccol_of(copy), e);
    TEST_ASSERT_EQ_I(atomic_load(&e->rc), 2);
    ray_release(copy);
    ray_release(c);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.sys_mapped, m0.sys_mapped);   /* mapping released */
    PASS();
}

static test_result_t test_load_rejects(void) {
    const char* p = ENCCOL_TMP "/r";
    uint8_t one = 1, two = 2;
    uint64_t u;
    /* truncation: one byte short */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    { FILE* f = fopen(p, "r+b"); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
      TEST_ASSERT_EQ_I(truncate(p, sz - 1), 0); }
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* header_version 2 */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    patch(p, 32 + 4, &two, 1);
    TEST_ASSERT_NULL(mmap_expect_err(p, "version"));
    /* enc header reserved byte */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    patch(p, 32 + 9, &one, 1);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* col_flags reserved bit */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    patch(p, 32 + 7, &two, 1);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* index_kind HASH without index_off */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    patch(p, 32 + 6, &one, 1);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* index_off without index_kind */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    u = 4096; patch(p, 32 + 32, &u, 8);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* directory: chunk offset moved by 8 */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    u = 96 + 64 + 8; patch(p, 96, &u, 8);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* directory: null_count above the chunk's rows */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    uint32_t nc = 3001; patch(p, 96 + 52, &nc, 4);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* directory: reserved word */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    patch(p, 96 + 60, &one, 1);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* directory: absent flag with nonzero min */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    patch(p, 96 + 56, &one, 1);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* a ZONE recorded on a GUID column: not admitted */
    {
        ray_t* g = ray_vec_new(RAY_GUID, 4); g->len = 4; memset(ray_data(g), 1, 64);
        TEST_ASSERT_EQ_I(ray_col_save_gen(g, p, 2), RAY_OK); ray_release(g);
        uint8_t z = 3; patch(p, 32 + 6, &z, 1);
        TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    }
    /* a STR chunk with statistics present: a newer build's file, nyi */
    {
        ray_t* st = ray_vec_new(RAY_STR, 2);
        st = ray_str_vec_append(st, "a", 1); st = ray_str_vec_append(st, "b", 1);
        TEST_ASSERT_EQ_I(ray_col_save_gen(st, p, 2), RAY_OK); ray_release(st);
        uint32_t fl = 0; patch(p, 96 + 56, &fl, 4);
        int64_t one64 = 1; patch(p, 96 + 24, &one64, 8);
        TEST_ASSERT_NULL(mmap_expect_err(p, "nyi"));
    }
    /* outer header: attrs bit outside the persisted mask (HAS_INDEX) */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    uint8_t a = 0x08; patch(p, 19, &a, 1);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    PASS();
}

/* Appending a generation-0 index region would stamp "XIDX" over "XENC"
 * and produce (order 2, "XIDX"), a file that no longer loads. */
static test_result_t test_append_index_refuses_gen2(void) {
    TEST_ASSERT_EQ_I(save_i32_rows(ENCCOL_TMP "/ai", 3000), RAY_OK);
    ray_t* v = ray_vec_new(RAY_I32, 3000); v->len = 3000;      /* ray_col_load refuses gen 2 until Task 8 */
    for (int64_t i = 0; i < 3000; i++) ((int32_t*)ray_data(v))[i] = (int32_t)(i % 977);
    ray_t* z = ray_index_chunk_zone_compute(v, 8);
    TEST_ASSERT_TRUE(z && !RAY_IS_ERR(z));
    TEST_ASSERT_EQ_I(ray_col_append_index(ENCCOL_TMP "/ai", ray_index_payload(z), 3000, RAY_I32), RAY_ERR_VERSION);
    ray_release(z); ray_release(v);
    ray_t* c = ray_col_mmap(ENCCOL_TMP "/ai");             /* still loads */
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_release(c);
    PASS();
}

static test_result_t test_sym_requires_domain(void) {
    uint8_t ids[16];
    for (int i = 0; i < 16; i++) ids[i] = (uint8_t)(i % 4);
    ray_t* v = ray_sym_vec_new(RAY_SYM_W8, 16);
    v->len = 16; memcpy(ray_data(v), ids, 16);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/s", 2), RAY_OK);
    ray_release(v);
    ray_t* r = ray_col_mmap_splayed_dom(ENCCOL_TMP "/s", NULL);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "sym");
    ray_error_free(r);
    PASS();
}

/* ray_col_load (non-mmap) stays nyi on generation 2 until the materializer
 * lands, and leaves nothing mapped behind. */
static test_result_t test_load_gen2_nyi(void) {
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    TEST_ASSERT_EQ_I(save_i32_rows(ENCCOL_TMP "/ln", 3000), RAY_OK);
    ray_t* r = ray_col_load(ENCCOL_TMP "/ln");
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "nyi");
    ray_error_free(r);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.sys_mapped, m0.sys_mapped);
    PASS();
}

/* A valid open, then corrupt opens on every error path, leave no mapping. */
static test_result_t test_no_mapping_leak(void) {
    const char* p = ENCCOL_TMP "/lk";
    uint8_t two = 2;
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_release(c);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
        if (i == 0) patch(p, 32 + 4, &two, 1);             /* version */
        if (i == 1) patch(p, 32 + 9, &two, 1);             /* corrupt */
        if (i == 2) TEST_ASSERT_EQ_I(truncate(p, 200), 0); /* corrupt, truncated */
        ray_t* r = ray_col_mmap(p);
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        ray_error_free(r);
    }
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.sys_mapped, m0.sys_mapped);
    PASS();
}

/* The big-endian gate (generation 2 answers nyi there, not corrupt) cannot
 * be exercised end to end on a little-endian host: ray_enccol_open's
 * `!ray_enc_gen2_supported()` branch is never taken (the build with
 * -U__BYTE_ORDER__ checks that it compiles).  What is testable here: the
 * discriminator recognises "XENC" bytewise, so on any host a generation-2 file reaches ray_enccol_open
 * instead of being rejected as corrupt before the gate. */
static test_result_t test_be_gate_logic(void) {
    uint8_t h[32] = {0};
    memcpy(h, "XENC", 4); h[17] = RAY_ENC_GEN;
    TEST_ASSERT_EQ_I(ray_col_gen_of(h), 2);
    memcpy(h, "CNEX", 4);                       /* byte-reversed marker is not XENC */
    TEST_ASSERT_EQ_I(ray_col_gen_of(h), RAY_ENC_GEN_CORRUPT);
    PASS();
}

/* ---- fix round 1 ---- */

#define LEN_OFF  ((long)offsetof(ray_t, len))
#define MMOD_OFF ((long)offsetof(ray_t, mmod))
#define RC_OFF   ((long)offsetof(ray_t, rc))

static test_result_t test_load_rejects_more(void) {
    const char* p = ENCCOL_TMP "/m";
    uint8_t one = 1;
    int64_t v64;
    uint64_t u;
    /* crafted lengths: no signed overflow, no huge directory */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    v64 = INT64_MAX; patch(p, LEN_OFF, &v64, 8);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    v64 = INT64_MAX - 1; patch(p, LEN_OFF, &v64, 8);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    v64 = -1; patch(p, LEN_OFF, &v64, 8);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    v64 = 65536 * 3; patch(p, LEN_OFF, &v64, 8);           /* more rows than the directory area can describe */
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* outer reserved bytes and mmod */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    patch(p, 5, &one, 1);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    patch(p, MMOD_OFF, &one, 1);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* SYM count on a non-SYM column */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    { uint32_t sc = 5; patch(p, RC_OFF, &sc, 4); }
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* str_pool_bytes on a non-STR column */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    u = 1; patch(p, 32 + 48, &u, 8);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* trailing bytes after the last section */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    { FILE* f = fopen(p, "ab"); fwrite(&one, 1, 1, f); fclose(f); }
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* directory: chunk size not a multiple of 8 */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    u = 3073; patch(p, 96 + 8, &u, 8);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* directory: min above max */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    v64 = (int64_t)1 << 40; patch(p, 96 + 16, &v64, 8);
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    /* directory: NaN float bound */
    {
        ray_t* f = ray_vec_new(RAY_F64, 3000); f->len = 3000;
        for (int i = 0; i < 3000; i++) ((double*)ray_data(f))[i] = (double)(i % 100);
        TEST_ASSERT_EQ_I(ray_col_save_gen(f, p, 2), RAY_OK); ray_release(f);
        uint64_t nan = 0x7ff8000000000000ull; patch(p, 96 + 16, &nan, 8);
        TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    }
    /* directory: a sum on a type that is not summed */
    {
        ray_t* g = ray_vec_new(RAY_GUID, 4); g->len = 4; memset(ray_data(g), 1, 64);
        TEST_ASSERT_EQ_I(ray_col_save_gen(g, p, 2), RAY_OK); ray_release(g);
        u = 1; patch(p, 96 + 32, &u, 8);
        TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    }
    PASS();
}

/* Real writer output with a VALUE_DICT side entry loads; a damaged side
 * table does not. */
static const int64_t g_dv[4] = { 11, 900000000001ll, 77, 123456789012345ll };
static test_result_t test_value_dict_side(void) {
    const char* p = ENCCOL_TMP "/vd";
    int64_t d[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d[i] = g_dv[(uint32_t)((uint32_t)i * 2654435761u) >> 30];
    ray_t* v = ray_vec_from_raw(RAY_I64, d, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_t* e = ray_enccol_of(c);
    TEST_ASSERT_TRUE(e->vdict_off != 0);
    TEST_ASSERT_EQ_U(e->vdict_off % 32, 0);
    uint64_t side_off = e->hdr.side_off;
    ray_release(c);
    uint32_t one = 1;
    patch(p, (long)side_off + 4, &one, 4);                /* side table reserved word */
    TEST_ASSERT_NULL(mmap_expect_err(p, "corrupt"));
    PASS();
}

/* SYM with a domain: the saved-count rule and the directory id bound. */
static ray_t* sym_col_save(const char* p) {
    ray_t* v = ray_sym_vec_new(RAY_SYM_W8, 16);
    v->len = 16;
    for (int i = 0; i < 16; i++) ((uint8_t*)ray_data(v))[i] = (uint8_t)((i % 4) + 1);
    ray_err_t e = ray_col_save_gen(v, p, 2);
    ray_release(v);
    return e == RAY_OK ? NULL : ray_error("fail", "save");
}
static test_result_t test_sym_domain_rules(void) {
    const char* p = ENCCOL_TMP "/sd";
    ray_sym_domain_t* dom = ray_sym_runtime_domain();
    TEST_ASSERT_NULL(sym_col_save(p));
    ray_t* c = ray_col_mmap_splayed_dom(p, dom);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(ray_enccol_of(c)->type, RAY_SYM);
    ray_release(c);
    /* saved count above the domain's */
    uint32_t big = 1000000; patch(p, RC_OFF, &big, 4);
    ray_t* r = ray_col_mmap_splayed_dom(p, dom);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    /* directory max above the bound */
    TEST_ASSERT_NULL(sym_col_save(p));
    int64_t mx = 1000000; patch(p, 96 + 24, &mx, 8);
    r = ray_col_mmap_splayed_dom(p, dom);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    /* directory min below 1 */
    TEST_ASSERT_NULL(sym_col_save(p));
    int64_t mn = 0; patch(p, 96 + 16, &mn, 8);
    r = ray_col_mmap_splayed_dom(p, dom);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    PASS();
}

/* Plain-vector consumers refuse an encoded column until their tasks land. */
static test_result_t test_consumers_refuse(void) {
    /* (a) a complete generation-0 relation maps; one file rewritten as
     * generation 2 (required or optional) makes the map fail */
    {
        int64_t sd[] = {0, 0, 1, 1, 2, 3}, dd[] = {1, 2, 2, 3, 3, 0};
        ray_t* sv = ray_vec_from_raw(RAY_I64, sd, 6); ray_t* dv = ray_vec_from_raw(RAY_I64, dd, 6);
        ray_t* et = ray_table_new(2);
        et = ray_table_add_col(et, ray_sym_intern("src", 3), sv);
        et = ray_table_add_col(et, ray_sym_intern("dst", 3), dv);
        ray_release(sv); ray_release(dv);
        ray_rel_t* rel = ray_rel_from_edges(et, "src", "dst", 4, 4, true);
        TEST_ASSERT_TRUE(rel != NULL);
        const char* rd = ENCCOL_TMP "/rel";
        TEST_ASSERT_EQ_I(ray_rel_save(rel, rd), RAY_OK);
        ray_rel_free(rel); ray_release(et);
        int64_t md[5] = {0, 1, 2, 0, 1};                   /* plain meta, whatever the saver wrote */
        ray_t* mv = ray_vec_from_raw(RAY_I64, md, 5);
        TEST_ASSERT_EQ_I(ray_col_save(mv, ENCCOL_TMP "/rel/meta"), RAY_OK);
        ray_release(mv);
        ray_rel_t* good = ray_rel_mmap(rd);
        TEST_ASSERT_TRUE(good != NULL);
        TEST_ASSERT_TRUE(good->fwd.rowmap != NULL);        /* the optional file is really there */
        ray_rel_free(good);
        static const char* files[] = { "fwd_offsets", "fwd_targets", "rev_offsets", "rev_targets",
                                       "fwd_rowmap", "rev_rowmap", "meta" };
        for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
            char fp[256], bak[260];
            snprintf(fp, sizeof fp, "%s/%s", rd, files[i]);
            snprintf(bak, sizeof bak, "%s.bak", fp);
            TEST_ASSERT_EQ_I(rename(fp, bak), 0);
            ray_t* enc = ray_vec_new(RAY_I64, 8); enc->len = 8;
            for (int k = 0; k < 8; k++) ((int64_t*)ray_data(enc))[k] = k;
            TEST_ASSERT_EQ_I(ray_col_save_gen(enc, fp, 2), RAY_OK);
            ray_release(enc);
            TEST_ASSERT_NULL(ray_rel_mmap(rd));
            TEST_ASSERT_NULL(ray_rel_load(rd));
            TEST_ASSERT_EQ_I(rename(bak, fp), 0);
            ray_rel_t* again = ray_rel_mmap(rd);
            TEST_ASSERT_TRUE(again != NULL);
            ray_rel_free(again);
        }
    }
    /* (b) hash index build */
    TEST_ASSERT_EQ_I(save_i32_rows(ENCCOL_TMP "/hc", 3000), RAY_OK);
    TEST_ASSERT_EQ_I(ray_splay_hash_column(ENCCOL_TMP "/hc", NULL), RAY_ERR_NYI);
    ray_t* ok = ray_col_mmap(ENCCOL_TMP "/hc");            /* file untouched */
    TEST_ASSERT_TRUE(ok && !RAY_IS_ERR(ok));
    ray_release(ok);
    /* (c) splayed table load: one plain column, one generation-2 file */
    ray_t* tbl = ray_table_new(2);
    ray_t* a = ray_vec_new(RAY_I64, 3000); a->len = 3000;
    ray_t* b = ray_vec_new(RAY_I32, 3000); b->len = 3000;
    for (int i = 0; i < 3000; i++) { ((int64_t*)ray_data(a))[i] = i; ((int32_t*)ray_data(b))[i] = i % 977; }
    tbl = ray_table_add_col(tbl, ray_sym_intern("ca", 2), a);
    tbl = ray_table_add_col(tbl, ray_sym_intern("cb", 2), b);
    ray_release(a); ray_release(b);
    TEST_ASSERT_EQ_I(ray_splay_save(tbl, ENCCOL_TMP "/tb", NULL), RAY_OK);
    ray_release(tbl);
    TEST_ASSERT_EQ_I(save_i32_rows(ENCCOL_TMP "/tb/cb", 3000), RAY_OK);   /* now generation 2 */
    ray_t* r = ray_read_splayed(ENCCOL_TMP "/tb", NULL);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "nyi");
    ray_error_free(r);
    PASS();
}

static uint64_t g_seed = 1;
static uint64_t rng(void) { uint64_t x = g_seed; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return g_seed = x; }

/* A column of `type` and `n` rows in shape s: 0 random, 1 small range,
 * 2 runs, 3 constant, 4 sparse nulls, 5 all null. */
static ray_t* make_col(int8_t type, int64_t n, int s) {
    if (type == RAY_STR) {
        ray_t* v = ray_vec_new(RAY_STR, n);
        char buf[64];
        for (int64_t i = 0; i < n; i++) {
            int len;
            if (s == 5 || (s == 4 && i % 7 == 0)) len = 0;
            else if (s == 3) len = snprintf(buf, sizeof buf, "constant string value");
            else if (s == 2) len = snprintf(buf, sizeof buf, "run %lld long tail text", (long long)(i / 300));
            else len = snprintf(buf, sizeof buf, "%llu", (unsigned long long)(rng() % (s == 1 ? 50 : 100000000000ull)));
            v = ray_str_vec_append(v, buf, (size_t)len);
        }
        if (s >= 4) v->attrs |= RAY_ATTR_HAS_NULLS;
        return v;
    }
    ray_t* v = type == RAY_SYM ? ray_sym_vec_new(RAY_SYM_W16, n) : ray_vec_new(type, n);
    v->len = n;
    uint8_t esz = ray_sym_elem_size(type, v->attrs);
    uint8_t* d = ray_data(v);
    for (int64_t i = 0; i < n; i++) {
        uint64_t x = s == 0 ? rng() : s == 1 ? rng() % 64 : s == 2 ? (uint64_t)(i / 300) * 7919 : 12345;
        bool null = s == 5 || (s == 4 && i % 7 == 0);
        switch (type) {
        case RAY_BOOL: d[i] = (uint8_t)(x & 1); break;
        case RAY_U8:   d[i] = (uint8_t)x; break;
        case RAY_I16:  { int16_t y = null ? NULL_I16 : (int16_t)(x % 30000); memcpy(d + i * 2, &y, 2); break; }
        case RAY_I32: case RAY_DATE: case RAY_TIME:
                       { int32_t y = null ? NULL_I32 : (int32_t)(x % 2000000000); memcpy(d + i * 4, &y, 4); break; }
        case RAY_I64: case RAY_TIMESTAMP:
                       { int64_t y = null ? NULL_I64 : (int64_t)(x >> 1); memcpy(d + i * 8, &y, 8); break; }
        case RAY_F32:  { float y = null ? NULL_F32 : (float)(int64_t)(x % 100000) / 8.0f; memcpy(d + i * 4, &y, 4); break; }
        case RAY_F64:  { double y = null ? NULL_F64 : (double)(int64_t)(x % 1000000) / 16.0; memcpy(d + i * 8, &y, 8); break; }
        case RAY_GUID: { memset(d + i * 16, 0, 16); if (!null) { memcpy(d + i * 16, &x, 8); d[i * 16 + 15] = 1; } break; }
        case RAY_SYM:  ray_write_sym(d, i, null ? 0 : 1 + x % 200, RAY_SYM, v->attrs); break;
        }
        (void)esz;
    }
    if (s >= 4 && type != RAY_BOOL && type != RAY_U8) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}

/* Decode every vector of the generation-2 file and compare with `v`. */
static test_result_t check_decode(ray_t* v, const char* path) {
    ray_t* c = ray_col_mmap(path);
    TEST_ASSERT_FMT(c && !RAY_IS_ERR(c), "mmap %s: %s", path, c ? ray_err_code(c) : "null");
    ray_enccol_t* e = ray_enccol_of(c);
    uint8_t esz = ray_enccol_esz(e);
    uint8_t out[RAY_ENC_VEC * 16];
    char pool[RAY_ENC_VEC * 64];
    const char* spool = (v->type == RAY_STR && v->str_pool) ? (const char*)ray_data(v->str_pool) : NULL;
    for (int64_t vec = 0; vec < ray_enc_nvecs(v->len); vec++) {
        ray_t* err = ray_enccol_decode_vec(e, vec, out, pool, 0, sizeof pool);
        TEST_ASSERT_FMT(err == NULL, "vec %lld: %s %s", (long long)vec, err ? ray_err_code(err) : "", ray_error_msg() ? ray_error_msg() : "");
        int vn = ray_enc_vec_len(v->len, vec);
        if (v->type == RAY_STR) {
            const ray_str_t* a = (const ray_str_t*)ray_data(v) + vec * RAY_ENC_VEC;
            const ray_str_t* b = (const ray_str_t*)out;
            for (int i = 0; i < vn; i++) {
                TEST_ASSERT_FMT(a[i].len == b[i].len, "str len row %d", i);
                TEST_ASSERT_FMT(memcmp(ray_str_t_ptr(&a[i], spool), ray_str_t_ptr(&b[i], pool), a[i].len) == 0, "str bytes row %d", i);
            }
        } else {
            const uint8_t* a = (const uint8_t*)ray_data(v) + (size_t)vec * RAY_ENC_VEC * esz;
            TEST_ASSERT_FMT(memcmp(a, out, (size_t)vn * esz) == 0, "type %d vec %lld bytes differ", (int)v->type, (long long)vec);
        }
    }
    ray_release(c);
    PASS();
}

static test_result_t test_roundtrip_types_shapes(void) {
    static const int8_t types[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIME,
                                    RAY_TIMESTAMP, RAY_F32, RAY_F64, RAY_GUID, RAY_SYM, RAY_STR };
    for (size_t t = 0; t < sizeof types; t++)
        for (int s = 0; s < 6; s++) {
            if ((types[t] == RAY_BOOL || types[t] == RAY_U8) && s >= 4) continue;
            ray_t* v = make_col(types[t], 3000, s);
            TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/rt", 2), RAY_OK);
            test_result_t r = check_decode(v, ENCCOL_TMP "/rt");
            ray_release(v);
            if (r.status != TEST_PASS) return r;
        }
    PASS();
}

static test_result_t test_roundtrip_lengths(void) {
    static const int64_t lens[] = { 1, 1023, 1024, 1025, 65535, 65536, 65537 };
    static const int8_t types[] = { RAY_I32, RAY_F64, RAY_STR, RAY_SYM };
    for (size_t t = 0; t < sizeof types; t++)
        for (size_t l = 0; l < sizeof lens / sizeof lens[0]; l++) {
            ray_t* v = make_col(types[t], lens[l], 4);
            TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/len", 2), RAY_OK);
            test_result_t r = check_decode(v, ENCCOL_TMP "/len");
            ray_release(v);
            if (r.status != TEST_PASS) return r;
        }
    PASS();
}

/* Signedness at the domain edges and the 127/128 boundary. */
static test_result_t test_roundtrip_edges(void) {
    int64_t d64[6] = { INT64_MIN + 1, INT64_MAX, 0, NULL_I64, -1, INT64_MAX };
    ray_t* v = ray_vec_from_raw(RAY_I64, d64, 6); v->attrs |= RAY_ATTR_HAS_NULLS;
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/e", 2), RAY_OK);
    test_result_t r = check_decode(v, ENCCOL_TMP "/e"); ray_release(v);
    if (r.status != TEST_PASS) return r;
    int16_t d16[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d16[i] = (int16_t)(-32767 + (i % 3) * 32767);
    v = ray_vec_from_raw(RAY_I16, d16, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/e", 2), RAY_OK);
    r = check_decode(v, ENCCOL_TMP "/e"); ray_release(v);
    if (r.status != TEST_PASS) return r;
    uint8_t u8[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) u8[i] = (uint8_t)(127 + (i & 1));
    v = ray_vec_from_raw(RAY_U8, u8, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/e", 2), RAY_OK);
    r = check_decode(v, ENCCOL_TMP "/e"); ray_release(v);
    return r;
}

/* The checked-in fixtures decode to the vectors that produced them. */
static test_result_t test_golden_decode(void) {
    static const char* names[] = { "i32_for", "i64_const", "i64_delta", "i64_dict_codes_rle", "i64_dict",
                                   "i32_nulls", "i16_all_null", "u8_raw", "bool_rle", "f64_trailer",
                                   "f64_const", "f32_zeros", "guid_mixed", "str_mixed", "str_const",
                                   "i64_empty", "i32_two_chunks", "str_inline", "str_pooled", "str_repeated",
                                   "i16_delta_wT", "i32_delta_wT", "i64_delta_wT", "i64_dict_const_child",
                                   "i64_rle_values_for", "i64_rle_ends_delta" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[256]; snprintf(p, sizeof p, "test/data/enc/%s.bin", names[i]);
        ray_t* c = ray_col_mmap(p);
        TEST_ASSERT_FMT(c && !RAY_IS_ERR(c), "fixture %s does not load", names[i]);
        ray_enccol_t* e = ray_enccol_of(c);
        uint8_t out[RAY_ENC_VEC * 16]; char pool[RAY_ENC_VEC * 64];
        for (int64_t vec = 0; vec < ray_enc_nvecs(e->len); vec++) {
            ray_t* err = ray_enccol_decode_vec(e, vec, out, pool, 0, sizeof pool);
            TEST_ASSERT_FMT(err == NULL, "fixture %s vec %lld", names[i], (long long)vec);
        }
        ray_release(c);
    }
    PASS();
}

/* The VALUE_DICT entry is validated once whoever asks first; a transient
 * failure of that validation is retried (spec "Validation": once-state). */
typedef struct { ray_enccol_t* e; ray_t* err; } dthr_t;
static void dict_decode_thread(void* raw) {
    dthr_t* t = raw;
    uint8_t out[RAY_ENC_VEC * 8];
    t->err = ray_enccol_decode_vec(t->e, 0, out, NULL, 0, 0);
    if (t->err) { ray_error_free(t->err); t->err = (ray_t*)1; }
    ray_heap_abandon();
}

static test_result_t test_dict_once_concurrent(void) {
    static const int64_t vals[4] = { 11, 900000000001ll, 77, 123456789012345ll };
    ray_t* v = ray_vec_new(RAY_I64, 1024); v->len = 1024;
    for (int i = 0; i < 1024; i++) ((int64_t*)ray_data(v))[i] = vals[(uint32_t)((uint32_t)i * 2654435761u) >> 30];
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/dc", 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ENCCOL_TMP "/dc");
    ray_enccol_t* e = ray_enccol_of(c);
    ray_enc_desc_t d0; memcpy(&d0, e->base + 160, 48);
    TEST_ASSERT_EQ_I(d0.encoding, RAY_ENC_DICT);
    ray_enccol_test_fail_alloc(1);                   /* the validation's allocation */
    uint8_t out[RAY_ENC_VEC * 8];
    ray_t* r = ray_enccol_decode_vec(e, 0, out, NULL, 0, 0);
    ray_enccol_test_fail_alloc(0);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "oom");
    ray_error_free(r);
    TEST_ASSERT_NULL(e->dict);                       /* not cached */
    dthr_t t[8]; ray_thread_t th[8];
    for (int i = 0; i < 8; i++) { t[i].e = e; t[i].err = NULL; ray_thread_create(&th[i], dict_decode_thread, &t[i]); }
    for (int i = 0; i < 8; i++) { ray_thread_join(th[i]); TEST_ASSERT_NULL(t[i].err); }
    TEST_ASSERT_NOT_NULL(e->dict);
    ray_release(c);
    PASS();
}

static test_result_t test_decode_rejects_payload(void) {
    /* FOR_BP vector whose base has bits above T: */
    int32_t d[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d[i] = 5000 + (i % 64);
    ray_t* v = ray_vec_from_raw(RAY_I32, d, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/p", 2), RAY_OK);
    ray_release(v);
    uint64_t bad_base = 0x100000000ull;               /* payload starts at 160 + 3072 */
    patch(ENCCOL_TMP "/p", 160 + 3072, &bad_base, 8);
    ray_t* c = ray_col_mmap(ENCCOL_TMP "/p");          /* load passes: payload unread */
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    uint8_t out[4096];
    ray_t* err = ray_enccol_decode_vec(ray_enccol_of(c), 0, out, NULL, 0, 0);
    TEST_ASSERT_TRUE(err && RAY_IS_ERR(err));
    TEST_ASSERT_STR_EQ(ray_err_code(err), "corrupt");
    TEST_ASSERT_TRUE(strstr(ray_error_msg(), ENCCOL_TMP "/p") != NULL);
    ray_error_free(err);
    ray_release(c);
    /* A trailing descriptor that is not all zero: */
    v = ray_vec_from_raw(RAY_I32, d, 10);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/p", 2), RAY_OK);
    ray_release(v);
    uint8_t one = 1;
    patch(ENCCOL_TMP "/p", 160 + 48 * 5 + 3, &one, 1);
    c = ray_col_mmap(ENCCOL_TMP "/p");
    err = ray_enccol_decode_vec(ray_enccol_of(c), 0, out, NULL, 0, 0);
    TEST_ASSERT_TRUE(err && RAY_IS_ERR(err));
    ray_error_free(err);
    ray_release(c);
    PASS();
}

/* ---- hardening: crafted input never reads out of bounds, never half-writes ---- */

#define CANARY 0xA5

/* A corrupt vector leaves `out` and the pool untouched. */
static test_result_t decode_expect_corrupt(ray_enccol_t* e, int64_t vec, const char* what) {
    uint8_t out[RAY_ENC_VEC * 16]; char pool[RAY_ENC_VEC * 64];
    memset(out, CANARY, sizeof out); memset(pool, CANARY, sizeof pool);
    ray_t* err = ray_enccol_decode_vec(e, vec, out, pool, 0, sizeof pool);
    TEST_ASSERT_FMT(err && RAY_IS_ERR(err), "%s: decoded", what);
    TEST_ASSERT_FMT(strcmp(ray_err_code(err), "corrupt") == 0, "%s: code %s", what, ray_err_code(err));
    ray_error_free(err);
    for (size_t i = 0; i < sizeof out; i++) TEST_ASSERT_FMT(out[i] == CANARY, "%s: out written at %zu", what, i);
    for (size_t i = 0; i < sizeof pool; i++) TEST_ASSERT_FMT(pool[i] == (char)CANARY, "%s: pool written at %zu", what, i);
    PASS();
}

/* Grow the single vector of a one-chunk file by 8 zero bytes inside its
 * payload extent: payload_bytes, the chunk size and the end offset all
 * move, so the structure stays consistent and only the body is wrong. */
static void grow_payload(const char* path) {
    FILE* f = fopen(path, "r+b");
    uint64_t chunk_bytes, end_off, pb;
    fseek(f, 96 + 8, SEEK_SET); fread(&chunk_bytes, 8, 1, f);
    fseek(f, 32 + 56, SEEK_SET); fread(&end_off, 8, 1, f);
    fseek(f, 160 + 16, SEEK_SET); fread(&pb, 8, 1, f);
    chunk_bytes += 8; end_off += 8; pb += 8;
    fseek(f, 96 + 8, SEEK_SET); fwrite(&chunk_bytes, 8, 1, f);
    fseek(f, 32 + 56, SEEK_SET); fwrite(&end_off, 8, 1, f);
    fseek(f, 160 + 16, SEEK_SET); fwrite(&pb, 8, 1, f);
    uint8_t z[8] = {0};
    fseek(f, 0, SEEK_END); fwrite(z, 1, 8, f);
    fclose(f);
}

static test_result_t test_decode_trailing_bytes(void) {
    static const struct { int8_t type; int s; } cases[] = {
        { RAY_I32, 1 }, { RAY_I64, 3 }, { RAY_I16, 4 }, { RAY_U8, 0 }, { RAY_BOOL, 2 },
        { RAY_F64, 0 }, { RAY_F32, 4 }, { RAY_GUID, 0 }, { RAY_STR, 0 }, { RAY_STR, 3 }, { RAY_SYM, 1 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ray_t* v = make_col(cases[i].type, 1000, cases[i].s);
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/tb", 2), RAY_OK);
        ray_release(v);
        ray_t* c = ray_col_mmap(ENCCOL_TMP "/tb");
        TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
        TEST_ASSERT_EQ_U(ray_enccol_of(c)->hdr.side_off, 0);     /* the payload is the file's tail */
        ray_release(c);
        grow_payload(ENCCOL_TMP "/tb");
        c = ray_col_mmap(ENCCOL_TMP "/tb");
        TEST_ASSERT_FMT(c && !RAY_IS_ERR(c), "type %d shape %d: load", (int)cases[i].type, cases[i].s);
        char what[64]; snprintf(what, sizeof what, "type %d shape %d", (int)cases[i].type, cases[i].s);
        test_result_t r = decode_expect_corrupt(ray_enccol_of(c), 0, what);
        ray_release(c);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* DICT never dereferences a missing dictionary. */
static test_result_t test_dict_null_dictionary(void) {
    uint64_t v[16]; uint64_t dv[2] = { 10, 20 };
    for (int i = 0; i < 16; i++) v[i] = dv[i & 1];
    ray_enc_kind_t k; TEST_ASSERT_TRUE(ray_enc_kind_of(RAY_I64, 0, &k));
    ray_enc_dict_t d = { dv, 2 };
    ray_enc_buf_t buf = {0}; ray_enc_choice_t ch;
    TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_DICT, v, 16, k, &d, &buf, &ch));
    uint64_t out[16];
    TEST_ASSERT_EQ_I(ray_enc_read(buf.p, buf.len, RAY_ENC_DICT, 0, 16, k, RAY_ENC_SLOT_TOP, 0, &d, out), (int64_t)buf.len);
    TEST_ASSERT_TRUE(ray_enc_read(buf.p, buf.len, RAY_ENC_DICT, 0, 16, k, RAY_ENC_SLOT_TOP, 0, NULL, out) < 0);
    ray_enc_dict_t empty = { NULL, 0 };
    TEST_ASSERT_TRUE(ray_enc_read(buf.p, buf.len, RAY_ENC_DICT, 0, 16, k, RAY_ENC_SLOT_TOP, 0, &empty, out) < 0);
    ray_enc_buf_free(&buf);
    PASS();
}

/* SYM ids must fit the attrs width and lie below the domain bound. */
static test_result_t test_sym_width_and_bound(void) {
    for (int i = 256; i < 400; i++) { char b[8]; int n = snprintf(b, sizeof b, "s%d", i); (void)ray_sym_intern(b, (size_t)n); }
    /* ids above 255 in a column whose attrs say W8: */
    ray_t* v = ray_sym_vec_new(RAY_SYM_W16, 2048); v->len = 2048;
    for (int i = 0; i < 2048; i++) ((uint16_t*)ray_data(v))[i] = (uint16_t)(260 + i % 100);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/sw", 2), RAY_OK);
    ray_release(v);
    uint8_t w8 = RAY_SYM_W8;
    patch(ENCCOL_TMP "/sw", (long)offsetof(ray_t, attrs), &w8, 1);
    ray_t* c = ray_col_mmap(ENCCOL_TMP "/sw");
    if (c && RAY_IS_ERR(c)) {
        TEST_ASSERT_STR_EQ(ray_err_code(c), "corrupt");
        ray_error_free(c);
    } else {
        TEST_ASSERT_NOT_NULL(c);
        for (int64_t vec = 0; vec < 2; vec++) {
            test_result_t r = decode_expect_corrupt(ray_enccol_of(c), vec, "W8 with wide ids");
            if (r.status != TEST_PASS) { ray_release(c); return r; }
        }
        ray_release(c);
    }
    /* ids at or above the saved count; statistics made absent so only the
     * payload says so: */
    v = make_col(RAY_SYM, 1000, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ENCCOL_TMP "/sb", 2), RAY_OK);
    ray_release(v);
    uint32_t bound = 50; patch(ENCCOL_TMP "/sb", RC_OFF, &bound, 4);
    uint64_t z = 0; uint32_t fl = RAY_ENC_STAT_ABSENT; uint16_t fl16 = RAY_ENC_STAT_ABSENT;
    patch(ENCCOL_TMP "/sb", 96 + 16, &z, 8); patch(ENCCOL_TMP "/sb", 96 + 24, &z, 8); patch(ENCCOL_TMP "/sb", 96 + 56, &fl, 4);
    patch(ENCCOL_TMP "/sb", 160 + 32, &z, 8); patch(ENCCOL_TMP "/sb", 160 + 40, &z, 8); patch(ENCCOL_TMP "/sb", 160 + 4, &fl16, 2);
    c = ray_col_mmap(ENCCOL_TMP "/sb");
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    TEST_ASSERT_EQ_U(ray_enccol_of(c)->sym_bound, 50);
    test_result_t r = decode_expect_corrupt(ray_enccol_of(c), 0, "id above the bound");
    ray_release(c);
    return r;
}

/* Flip bits of every byte of the file past the outer header, one byte at a
 * time, in several fixtures: a load either fails or every vector decodes to
 * OK or a corrupt / nyi error, and a failed decode leaves `out` untouched. */
static test_result_t sweep_file(const char* name, uint8_t mask, long* positions) {
    char src[256]; snprintf(src, sizeof src, "test/data/enc/%s.bin", name);
    FILE* f = fopen(src, "rb");
    TEST_ASSERT_FMT(f != NULL, "fixture %s", name);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n);
    TEST_ASSERT_TRUE(fread(b, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    const char* path = ENCCOL_TMP "/sweep";
    f = fopen(path, "wb"); fwrite(b, 1, (size_t)n, f); fclose(f);
    const char* bad = NULL;
    for (long pos = 32; pos < n && !bad; pos++) {
        uint8_t m = b[pos] ^ mask;
        patch(path, pos, &m, 1);
        ray_t* c = ray_col_mmap(path);
        if (c && RAY_IS_ERR(c)) { ray_error_free(c); }
        else if (c) {
            ray_enccol_t* e = ray_enccol_of(c);
            for (int64_t vec = 0; e && vec < ray_enc_nvecs(e->len) && !bad; vec++) {
                uint8_t out[RAY_ENC_VEC * 16]; char pool[RAY_ENC_VEC * 64];
                memset(out, CANARY, sizeof out);
                ray_t* err = ray_enccol_decode_vec(e, vec, out, pool, 0, sizeof pool);
                if (!err) continue;
                if (strcmp(ray_err_code(err), "corrupt") != 0 && strcmp(ray_err_code(err), "nyi") != 0) bad = "error code";
                for (size_t i = 0; i < sizeof out && !bad; i++) if (out[i] != CANARY) bad = "out written by a failed decode";
                ray_error_free(err);
            }
            ray_release(c);
        }
        patch(path, pos, &b[pos], 1);
        (*positions)++;
        if (bad) { free(b); TEST_ASSERT_FMT(0, "%s byte %ld mask 0x%02x: %s", name, pos, mask, bad); }
    }
    free(b);
    PASS();
}

static test_result_t test_decode_mutation_sweep(void) {
    static const char* names[] = { "i32_for", "i64_dict", "str_pooled", "str_mixed", "f64_trailer",
                                   "i64_rle_ends_for", "guid_mixed", "i32_nulls", "bool_rle", "i64_dict_codes_rle" };
    long positions = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        test_result_t r = sweep_file(names[i], 0xFF, &positions);
        if (r.status != TEST_PASS) return r;
        r = sweep_file(names[i], 0x01, &positions);
        if (r.status != TEST_PASS) return r;
    }
    TEST_ASSERT_TRUE(positions >= 2000);
    PASS();
}

const test_entry_t enccol_entries[] = {
    { "enccol/open_returns_enccol", test_open_returns_enccol, enccol_setup, enccol_teardown },
    { "enccol/load_rejects",        test_load_rejects,        enccol_setup, enccol_teardown },
    { "enccol/sym_requires_domain", test_sym_requires_domain, enccol_setup, enccol_teardown },
    { "enccol/append_index_gen2",   test_append_index_refuses_gen2, enccol_setup, enccol_teardown },
    { "enccol/load_gen2_nyi",       test_load_gen2_nyi,       enccol_setup, enccol_teardown },
    { "enccol/no_mapping_leak",     test_no_mapping_leak,     enccol_setup, enccol_teardown },
    { "enccol/load_rejects_more",   test_load_rejects_more,   enccol_setup, enccol_teardown },
    { "enccol/value_dict_side",     test_value_dict_side,     enccol_setup, enccol_teardown },
    { "enccol/sym_domain_rules",    test_sym_domain_rules,    enccol_setup, enccol_teardown },
    { "enccol/consumers_refuse",    test_consumers_refuse,    enccol_setup, enccol_teardown },
    { "enccol/be_gate_logic",       test_be_gate_logic,       enccol_setup, enccol_teardown },
    { "enccol/roundtrip_types_shapes", test_roundtrip_types_shapes, enccol_setup, enccol_teardown },
    { "enccol/roundtrip_lengths",      test_roundtrip_lengths,      enccol_setup, enccol_teardown },
    { "enccol/roundtrip_edges",        test_roundtrip_edges,        enccol_setup, enccol_teardown },
    { "enccol/golden_decode",          test_golden_decode,          enccol_setup, enccol_teardown },
    { "enccol/decode_rejects_payload", test_decode_rejects_payload, enccol_setup, enccol_teardown },
    { "enccol/dict_once_concurrent",   test_dict_once_concurrent,   enccol_setup, enccol_teardown },
    { "enccol/decode_trailing_bytes",  test_decode_trailing_bytes,  enccol_setup, enccol_teardown },
    { "enccol/dict_null_dictionary",   test_dict_null_dictionary,   enccol_setup, enccol_teardown },
    { "enccol/sym_width_and_bound",    test_sym_width_and_bound,    enccol_setup, enccol_teardown },
    { "enccol/decode_mutation_sweep",  test_decode_mutation_sweep,  enccol_setup, enccol_teardown },
    { NULL, NULL, NULL, NULL },
};
