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
#include "store/enccol.h"
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
    { NULL, NULL, NULL, NULL },
};
