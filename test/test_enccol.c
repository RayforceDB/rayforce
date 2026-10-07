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
#include "store/encw.h"
#include "store/part.h"
#include "store/colrange.h"
#include <math.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENCCOL_TMP "/tmp/rayforce_enccol"

/* 256 interned symbols, so SYM test ids up to 255 lie inside the runtime domain. */
static void enccol_setup(void) {
    ray_heap_init(); (void)ray_sym_init(); (void)ray_test_mkdir_p(ray_test_tmp(ENCCOL_TMP));
    for (int i = 0; i < 256; i++) { char b[8]; int n = snprintf(b, sizeof b, "s%d", i); (void)ray_sym_intern(b, (size_t)n); }
}
static void enccol_teardown(void) { ray_test_hooks_off(); ray_sym_destroy(); ray_heap_destroy(); (void)ray_test_rm_rf(ray_test_tmp(ENCCOL_TMP)); }

static ray_err_t save_i32_rows(const char* path, int64_t n) {
    ray_t* v = ray_vec_new(RAY_I32, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int32_t*)ray_data(v))[i] = (int32_t)(i % 977);
    ray_err_t e = ray_col_save_gen(v, path, 2);
    ray_release(v);
    return e;
}

/* Patch `n` bytes at file offset `off`. */
static void patch(const char* path, long off, const void* b, size_t n) {
    FILE* f = fopen(path, "r+b");
    if (!f) ray_test_fatal("cannot open %s", path);
    fseek(f, off, SEEK_SET); fwrite(b, 1, n, f); fclose(f);
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
    TEST_ASSERT_EQ_I(save_i32_rows(ray_test_tmp(ENCCOL_TMP "/a"), 70000), RAY_OK);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/a"));
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
    const char* p = ray_test_tmp(ENCCOL_TMP "/r");
    uint8_t one = 1, two = 2;
    uint64_t u;
    /* truncation: one byte short */
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3000), RAY_OK);
    { FILE* f = fopen(p, "r+b"); TEST_ASSERT_TRUE(f != NULL); fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
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
    TEST_ASSERT_EQ_I(save_i32_rows(ray_test_tmp(ENCCOL_TMP "/ai"), 3000), RAY_OK);
    ray_t* v = ray_vec_new(RAY_I32, 3000); v->len = 3000;
    for (int64_t i = 0; i < 3000; i++) ((int32_t*)ray_data(v))[i] = (int32_t)(i % 977);
    ray_t* z = ray_index_chunk_zone_compute(v, 8);
    TEST_ASSERT_TRUE(z && !RAY_IS_ERR(z));
    TEST_ASSERT_EQ_I(ray_col_append_index(ray_test_tmp(ENCCOL_TMP "/ai"), ray_index_payload(z), 3000, RAY_I32), RAY_ERR_VERSION);
    ray_release(z); ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/ai"));             /* still loads */
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_release(c);
    PASS();
}

static test_result_t test_sym_requires_domain(void) {
    uint8_t ids[16];
    for (int i = 0; i < 16; i++) ids[i] = (uint8_t)(i % 4);
    ray_t* v = ray_sym_vec_new(RAY_SYM_W8, 16);
    v->len = 16; memcpy(ray_data(v), ids, 16);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/s"), 2), RAY_OK);
    ray_release(v);
    ray_t* r = ray_col_mmap_splayed_dom(ray_test_tmp(ENCCOL_TMP "/s"), NULL);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "sym");
    ray_error_free(r);
    PASS();
}

/* ray_col_load (non-mmap) on generation 2 returns the plain vector and
 * leaves nothing mapped behind once it is released. */
static test_result_t test_load_gen2_plain(void) {
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    TEST_ASSERT_EQ_I(save_i32_rows(ray_test_tmp(ENCCOL_TMP "/ln"), 3000), RAY_OK);
    ray_t* r = ray_col_load(ray_test_tmp(ENCCOL_TMP "/ln"));
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_I32);
    TEST_ASSERT_EQ_I(r->len, 3000);
    ray_release(r);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.sys_mapped, m0.sys_mapped);
    PASS();
}

/* A valid open, then corrupt opens on every error path, leave no mapping. */
static test_result_t test_no_mapping_leak(void) {
    const char* p = ray_test_tmp(ENCCOL_TMP "/lk");
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
    const char* p = ray_test_tmp(ENCCOL_TMP "/m");
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
    { FILE* f = fopen(p, "ab"); TEST_ASSERT_TRUE(f != NULL); fwrite(&one, 1, 1, f); fclose(f); }
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
    const char* p = ray_test_tmp(ENCCOL_TMP "/vd");
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
    const char* p = ray_test_tmp(ENCCOL_TMP "/sd");
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

static bool csr_col_equal(const ray_t* a, const ray_t* b) {
    if (!a || !b) return a == b;
    return a->type == b->type && a->len == b->len && memcmp(ray_data((ray_t*)a), ray_data((ray_t*)b), (size_t)a->len * 8) == 0;
}
static bool csr_equal(const ray_csr_t* a, const ray_csr_t* b) {
    return csr_col_equal(a->offsets, b->offsets) && csr_col_equal(a->targets, b->targets) &&
           csr_col_equal(a->rowmap, b->rowmap) && a->n_nodes == b->n_nodes && a->n_edges == b->n_edges;
}

/* The mapping loader refuses an encoded column; the copying loader materializes it. */
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
        const char* rd = ray_test_tmp(ENCCOL_TMP "/rel");
        TEST_ASSERT_EQ_I(ray_rel_save(rel, rd), RAY_OK);
        ray_rel_free(rel); ray_release(et);
        int64_t md[5] = {0, 1, 2, 0, 1};                   /* plain meta, whatever the saver wrote */
        ray_t* mv = ray_vec_from_raw(RAY_I64, md, 5);
        TEST_ASSERT_EQ_I(ray_col_save(mv, ray_test_tmp(ENCCOL_TMP "/rel/meta")), RAY_OK);
        ray_release(mv);
        ray_rel_t* base = ray_rel_load(rd);                 /* the generation-0 load */
        TEST_ASSERT_TRUE(base != NULL);
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
            ray_t* orig = ray_col_load(bak);                /* the original column, rewritten as generation 2 */
            TEST_ASSERT_TRUE(orig && !RAY_IS_ERR(orig));
            TEST_ASSERT_EQ_I(ray_col_save_gen(orig, fp, 2), RAY_OK);
            ray_release(orig);
            TEST_ASSERT_NULL(ray_rel_mmap(rd));             /* mapping refuses an encoded column */
            ray_rel_t* copied = ray_rel_load(rd);           /* the copying loader materializes it */
            TEST_ASSERT_TRUE(copied != NULL);
            TEST_ASSERT_TRUE(csr_equal(&copied->fwd, &base->fwd) && csr_equal(&copied->rev, &base->rev));
            ray_rel_free(copied);
            TEST_ASSERT_EQ_I(rename(bak, fp), 0);
            ray_rel_t* again = ray_rel_mmap(rd);
            TEST_ASSERT_TRUE(again != NULL);
            ray_rel_free(again);
        }
        ray_rel_free(base);
    }
    /* (c) splayed table load: one plain column, one generation-2 file */
    ray_t* tbl = ray_table_new(2);
    ray_t* a = ray_vec_new(RAY_I64, 3000); a->len = 3000;
    ray_t* b = ray_vec_new(RAY_I32, 3000); b->len = 3000;
    for (int i = 0; i < 3000; i++) { ((int64_t*)ray_data(a))[i] = i; ((int32_t*)ray_data(b))[i] = i % 977; }
    tbl = ray_table_add_col(tbl, ray_sym_intern("ca", 2), a);
    tbl = ray_table_add_col(tbl, ray_sym_intern("cb", 2), b);
    ray_release(a); ray_release(b);
    TEST_ASSERT_EQ_I(ray_splay_save(tbl, ray_test_tmp(ENCCOL_TMP "/tb"), NULL), RAY_OK);
    ray_release(tbl);
    TEST_ASSERT_EQ_I(save_i32_rows(ray_test_tmp(ENCCOL_TMP "/tb/cb"), 3000), RAY_OK);   /* now generation 2 */
    ray_t* r = ray_read_splayed(ray_test_tmp(ENCCOL_TMP "/tb"), NULL);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));          /* the loader keeps the encoded column */
    TEST_ASSERT_NULL(ray_table_get_col_enc(r, ray_sym_intern("ca", 2)));
    TEST_ASSERT_TRUE(ray_table_get_col_enc(r, ray_sym_intern("cb", 2)) != NULL);
    ray_t* pcb = ray_table_get_col(r, ray_sym_intern("cb", 2));   /* the legacy getter decodes */
    TEST_ASSERT_TRUE(pcb && pcb->type == RAY_I32 && pcb->len == 3000);
    TEST_ASSERT_EQ_I(((int32_t*)ray_data(pcb))[2000], 2000 % 977);
    ray_release(r);
    PASS();
}

static uint64_t g_seed = 1;
static uint8_t g_symw = RAY_SYM_W16;      /* SYM width make_col builds */
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
    ray_t* v = type == RAY_SYM ? ray_sym_vec_new(g_symw, n) : ray_vec_new(type, n);
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
    static const uint8_t widths[] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64 };
    for (size_t t = 0; t < sizeof types; t++)
        for (size_t w = 0; w < (types[t] == RAY_SYM ? sizeof widths : 1); w++)
            for (int s = 0; s < 6; s++) {
                if ((types[t] == RAY_BOOL || types[t] == RAY_U8) && s >= 4) continue;
                g_symw = widths[w];
                ray_t* v = make_col(types[t], 3000, s);
                g_symw = RAY_SYM_W16;
                TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/rt"), 2), RAY_OK);
                test_result_t r = check_decode(v, ray_test_tmp(ENCCOL_TMP "/rt"));
                ray_release(v);
                if (r.status != TEST_PASS) return r;
            }
    PASS();
}

static test_result_t test_roundtrip_lengths(void) {
    static const int64_t lens[] = { 1, 1023, 1024, 1025, 65535, 65536, 65537 };
    static const int8_t types[] = { RAY_I32, RAY_F64, RAY_STR, RAY_SYM };
    static const uint8_t widths[] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64 };
    for (size_t t = 0; t < sizeof types; t++)
        for (size_t w = 0; w < (types[t] == RAY_SYM ? sizeof widths : 1); w++)
            for (size_t l = 0; l < sizeof lens / sizeof lens[0]; l++) {
                g_symw = widths[w];
                ray_t* v = make_col(types[t], lens[l], 4);
                g_symw = RAY_SYM_W16;
                TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/len"), 2), RAY_OK);
                test_result_t r = check_decode(v, ray_test_tmp(ENCCOL_TMP "/len"));
                ray_release(v);
                if (r.status != TEST_PASS) return r;
            }
    PASS();
}

/* Signedness at the domain edges and the 127/128 boundary. */
static test_result_t test_roundtrip_edges(void) {
    int64_t d64[6] = { INT64_MIN + 1, INT64_MAX, 0, NULL_I64, -1, INT64_MAX };
    ray_t* v = ray_vec_from_raw(RAY_I64, d64, 6); v->attrs |= RAY_ATTR_HAS_NULLS;
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/e"), 2), RAY_OK);
    test_result_t r = check_decode(v, ray_test_tmp(ENCCOL_TMP "/e")); ray_release(v);
    if (r.status != TEST_PASS) return r;
    int16_t d16[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d16[i] = (int16_t)(-32767 + (i % 3) * 32767);
    v = ray_vec_from_raw(RAY_I16, d16, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/e"), 2), RAY_OK);
    r = check_decode(v, ray_test_tmp(ENCCOL_TMP "/e")); ray_release(v);
    if (r.status != TEST_PASS) return r;
    uint8_t u8[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) u8[i] = (uint8_t)(127 + (i & 1));
    v = ray_vec_from_raw(RAY_U8, u8, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/e"), 2), RAY_OK);
    r = check_decode(v, ray_test_tmp(ENCCOL_TMP "/e")); ray_release(v);
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
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/dc"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/dc"));
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

/* ---- Task 3b: the integer fast kernel against the reference decode ---- */

/* One case: values (T-bit patterns, vlen rows) encoded as `enc`, decoded
 * by the fast kernel at every output it serves and by the reference path;
 * nulls are rows set in `bm` (NULL: none).  want_w >= 0 pins the width the
 * writer chose.  The fast kernel must take a sound vector. */
static test_result_t fk_case(int8_t type, uint8_t attrs, uint8_t enc, const uint64_t* vals, int vlen,
                             const uint8_t* bm, int want_w, const char* what) {
    ray_enc_kind_t k; TEST_ASSERT_TRUE(ray_enc_kind_of(type, attrs, &k));
    ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
    TEST_ASSERT_FMT(ray_enc_write_as(enc, vals, vlen, k, NULL, &b, &ch), "%s: write_as", what);
    if (want_w >= 0) TEST_ASSERT_FMT(ch.width == want_w, "%s: width %d, want %d", what, ch.width, want_w);
    int n_nulls = 0;
    if (type == RAY_SYM) { for (int i = 0; i < vlen; i++) n_nulls += vals[i] == 0; }
    else if (bm) { for (int i = 0; i < vlen; i++) n_nulls += (bm[i >> 3] >> (i & 7)) & 1; }
    const uint8_t* bmp = n_nulls && type != RAY_SYM ? bm : NULL;
    uint32_t bound = 1u << 30;
    static uint8_t ref[RAY_ENC_VEC * 8 + 8], fast[RAY_ENC_VEC * 8 + 8];
    memset(ref, 0xA5, sizeof ref); memset(fast, 0x5A, sizeof fast);
    int rr = ray_enccol_test_decode_body(type, attrs, bound, enc, ch.width, n_nulls, bmp, b.p, b.len, vlen, false, 0, ref);
    int fr = ray_enccol_test_decode_body(type, attrs, bound, enc, ch.width, n_nulls, bmp, b.p, b.len, vlen, true, 0, fast);
    TEST_ASSERT_FMT(rr == 1, "%s: reference %d", what, rr);
    TEST_ASSERT_FMT(fr == 1, "%s: fast kernel %d", what, fr);
    size_t esz = (size_t)k.T / 8;
    TEST_ASSERT_FMT(memcmp(ref, fast, (size_t)vlen * esz) == 0, "%s: fast differs", what);
    TEST_ASSERT_FMT(fast[(size_t)vlen * esz] == 0x5A, "%s: wrote past the vector", what);
    /* the expression register widening (narrow non-SYM types) */
    if (type != RAY_SYM && k.T < 64) {
        static int64_t wi[RAY_ENC_VEC + 1]; static double wf[RAY_ENC_VEC + 1];
        wi[vlen] = 77; wf[vlen] = 77.0;
        TEST_ASSERT_FMT(ray_enccol_test_decode_body(type, attrs, bound, enc, ch.width, n_nulls, bmp, b.p, b.len, vlen, true, RAY_I64, wi) == 1, "%s: wide i64", what);
        TEST_ASSERT_FMT(ray_enccol_test_decode_body(type, attrs, bound, enc, ch.width, n_nulls, bmp, b.p, b.len, vlen, true, RAY_F64, wf) == 1, "%s: wide f64", what);
        for (int i = 0; i < vlen; i++) {
            int64_t x = k.T == 8 ? (int64_t)ref[i] : k.T == 16 ? (int64_t)((int16_t*)ref)[i] : (int64_t)((int32_t*)ref)[i];
            bool null = bmp && ((bmp[i >> 3] >> (i & 7)) & 1);
            TEST_ASSERT_FMT(wi[i] == (null ? NULL_I64 : x), "%s: wide i64 row %d", what, i);
            TEST_ASSERT_FMT(null ? isnan(wf[i]) : wf[i] == (double)x, "%s: wide f64 row %d", what, i);
        }
        TEST_ASSERT_FMT(wi[vlen] == 77 && wf[vlen] == 77.0, "%s: wide wrote past the vector", what);
    }
    ray_enc_buf_free(&b);
    PASS();
}

/* Sentinel of a signed kind as a T-bit pattern. */
static uint64_t fk_sentinel(int T) { return T == 64 ? (uint64_t)1 << 63 : (uint64_t)1 << (T - 1); }

static test_result_t test_fast_kernel_equal(void) {
    static const struct { int8_t type; uint8_t attrs; } kinds[] = {
        { RAY_U8, 0 }, { RAY_BOOL, 0 }, { RAY_I16, 0 }, { RAY_I32, 0 }, { RAY_I64, 0 }, { RAY_DATE, 0 }, { RAY_TIME, 0 },
        { RAY_SYM, RAY_SYM_W8 }, { RAY_SYM, RAY_SYM_W16 }, { RAY_SYM, RAY_SYM_W32 }, { RAY_SYM, RAY_SYM_W64 },
    };
    static const int lens[] = { 1024, 1000, 77, 1 };
    static uint64_t v[RAY_ENC_VEC];
    static uint8_t bm[RAY_ENC_VEC / 8];
    char what[160];
    g_seed = 0x3b3b;
    for (size_t ki = 0; ki < sizeof kinds / sizeof kinds[0]; ki++) {
        int8_t type = kinds[ki].type; uint8_t attrs = kinds[ki].attrs;
        ray_enc_kind_t k; ray_enc_kind_of(type, attrs, &k);
        const int T = k.T;
        const uint64_t m = T == 64 ? ~0ull : (1ull << T) - 1;
        const bool sym = type == RAY_SYM, sgn = k.is_signed;
        int widths[5] = { 0, 1, T / 2 + 1, T - 1, T };
        for (int wi = 0; wi < 5; wi++) {
            int w = widths[wi];
            if (sym && w > 29) continue;                     /* ids stay below the bound 1 << 30 */
            for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
                int n = lens[li];
                if (w > 0 && n == 1) continue;                /* one row has range 0 */
                /* FOR_BP: offsets [0, span] over a base.  Signed kinds keep
                 * the logical order and stay off the sentinel: a full-width
                 * signed range is [MIN + 1, MAX], span 2^T - 2, still width T.
                 * SYM ids are 1..2^T - 1 (0 is the null). */
                uint64_t span = w == 0 ? 0 : (w == 64 ? ~0ull : (1ull << w) - 1);
                uint64_t base;
                if (sym) {
                    uint64_t top = m < (1ull << 30) - 1 ? m : (1ull << 30) - 1;   /* the largest id: below the bound */
                    if (w == T) span = m - 1;
                    base = 1 + rng() % (top - span);
                } else if (!sgn) {
                    base = rng() % (m - span + 1);
                } else {
                    if (w == T) span -= 1;
                    __int128 lo = -(((__int128)1) << (T - 1)) + 1, hi = (((__int128)1) << (T - 1)) - 1;
                    uint64_t room = (uint64_t)(hi - lo + 1 - (__int128)span);   /* bases b with b + span <= MAX */
                    __int128 lb = lo + (__int128)(rng() % room);
                    if (wi & 1) lb = lo + (__int128)(rng() % (room / 2 + 1));   /* the lower half: negative */
                    base = (uint64_t)lb & m;
                }
                for (int i = 0; i < n; i++) v[i] = (base + (span ? rng() % span : 0)) & m;
                if (span && n > 1) { v[0] = base; v[n - 1] = (base + span) & m; }
                snprintf(what, sizeof what, "FOR type %d attrs %d w %d n %d", type, attrs, w, n);
                test_result_t r = fk_case(type, attrs, RAY_ENC_FOR_BP, v, n, NULL, n == 1024 ? w : -1, what);
                if (r.status != TEST_PASS) return r;
                if (sgn) {
                    /* nulls in the bitmap (any bits underneath), one at the sentinel */
                    memset(bm, 0, sizeof bm);
                    for (int i = 0; i < n; i += 13) bm[i >> 3] |= (uint8_t)(1u << (i & 7));
                    if (n > 5) { v[5] = fk_sentinel(T); bm[0] |= 1u << 5; }
                    snprintf(what, sizeof what, "FOR nulls type %d w %d n %d", type, w, n);
                    if ((r = fk_case(type, attrs, RAY_ENC_FOR_BP, v, n, bm, -1, what)).status != TEST_PASS) return r;
                    /* DELTA_BP: lane walks with steps in [s0, s0 + span]; a value on the sentinel becomes a null */
                    int L = RAY_ENC_VEC / T;
                    uint64_t s0 = rng() & m;
                    memset(bm, 0, sizeof bm);
                    for (int i = 0; i < n; i++) {
                        uint64_t step = (s0 + (span ? rng() % (span + 1) : 0)) & m;
                        v[i] = i < L ? rng() & m : (v[i - L] + step) & m;
                    }
                    for (int i = 0; i < n; i++) if (v[i] == fk_sentinel(T)) bm[i >> 3] |= (uint8_t)(1u << (i & 7));
                    snprintf(what, sizeof what, "DELTA type %d w %d n %d", type, w, n);
                    if ((r = fk_case(type, attrs, RAY_ENC_DELTA_BP, v, n, bm, -1, what)).status != TEST_PASS) return r;
                }
            }
        }
    }
    /* a sentinel outside the bitmap: the fast kernel declines, the reference reports it */
    for (int i = 0; i < RAY_ENC_VEC; i++) v[i] = (uint64_t)(uint16_t)(int16_t)(i % 50 - 25);
    v[700] = fk_sentinel(16);
    ray_enc_kind_t k16 = { 16, true };
    ray_enc_buf_t b = { 0 }; ray_enc_choice_t ch;
    TEST_ASSERT_TRUE(ray_enc_write_as(RAY_ENC_FOR_BP, v, RAY_ENC_VEC, k16, NULL, &b, &ch));
    static uint8_t out[RAY_ENC_VEC * 8];
    TEST_ASSERT_EQ_I(ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, ch.width, 0, NULL, b.p, b.len, RAY_ENC_VEC, true, 0, out), 0);
    TEST_ASSERT_EQ_I(ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, ch.width, 0, NULL, b.p, b.len, RAY_ENC_VEC, false, 0, out), -1);
    /* the same sentinel with a bitmap that flags other rows, not 700: every
     * output (native, widened i64 and f64) declines, the reference reports it */
    {
        static uint8_t nb[RAY_ENC_VEC / 8];
        static int64_t wi[RAY_ENC_VEC]; static double wf[RAY_ENC_VEC];
        memset(nb, 0, sizeof nb);
        int nn = 0;
        for (int i = 3; i < RAY_ENC_VEC; i += 50) if (i != 700) { nb[i >> 3] |= (uint8_t)(1u << (i & 7)); nn++; }
        TEST_ASSERT_EQ_I(ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, ch.width, nn, nb, b.p, b.len, RAY_ENC_VEC, true, 0, out), 0);
        TEST_ASSERT_EQ_I(ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, ch.width, nn, nb, b.p, b.len, RAY_ENC_VEC, true, RAY_I64, wi), 0);
        TEST_ASSERT_EQ_I(ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, ch.width, nn, nb, b.p, b.len, RAY_ENC_VEC, true, RAY_F64, wf), 0);
        TEST_ASSERT_EQ_I(ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, ch.width, nn, nb, b.p, b.len, RAY_ENC_VEC, false, 0, out), -1);
        nb[700 >> 3] |= (uint8_t)(1u << (700 & 7));                         /* flagged: a null, sound */
        TEST_ASSERT_EQ_I(ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, ch.width, nn + 1, nb, b.p, b.len, RAY_ENC_VEC, true, RAY_I64, wi), 1);
        TEST_ASSERT_TRUE(wi[700] == NULL_I64);
    }
    /* a body one word short and one word long: declined, reported */
    TEST_ASSERT_EQ_I(ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, ch.width, 0, NULL, b.p, b.len - 8, RAY_ENC_VEC, true, 0, out), 0);
    TEST_ASSERT_EQ_I(ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, ch.width, 0, NULL, b.p, b.len - 8, RAY_ENC_VEC, false, 0, out), -1);
    ray_enc_buf_free(&b);
    /* The no-sentinel proof's edge (FOR_BP values are base + u, u < 2^w):
     * base 0x7FF1, width 4 reaches the I16 sentinel exactly at u = 15. */
    static uint64_t body[1 + RAY_ENC_VEC * 4 / 64], u[RAY_ENC_VEC];
    static int64_t wide[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) u[i] = (uint64_t)(i % 15);
    for (int hit = 1; hit >= 0; hit--) {
        u[600] = hit ? 15 : 14;
        memset(body, 0, sizeof body);
        body[0] = 0x7FF1;
        ray_enc_pack(u, 16, 4, (uint8_t*)(body + 1));
        int fr = ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, 4, 0, NULL, (const uint8_t*)body, sizeof body, RAY_ENC_VEC, true, 0, out);
        int wr = ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, 4, 0, NULL, (const uint8_t*)body, sizeof body, RAY_ENC_VEC, true, RAY_I64, wide);
        int rr = ray_enccol_test_decode_body(RAY_I16, 0, 0, RAY_ENC_FOR_BP, 4, 0, NULL, (const uint8_t*)body, sizeof body, RAY_ENC_VEC, false, 0, out);
        TEST_ASSERT_FMT(fr == (hit ? 0 : 1) && wr == fr && rr == (hit ? -1 : 1), "edge hit %d: fast %d wide %d ref %d", hit, fr, wr, rr);
        if (!hit) TEST_ASSERT_EQ_I(wide[600], 0x7FF1 + 14);
    }
    PASS();
}

/* A chunk's descriptors are validated once per column, whichever reader
 * comes first (range reads by any scratch, materialization), not once per
 * vector; a corrupt chunk is never marked and fails every read. */
static test_result_t test_desc_validated_once(void) {
    const char* p = ray_test_tmp(ENCCOL_TMP "/vo");
    TEST_ASSERT_EQ_I(save_i32_rows(p, 3 * 65536), RAY_OK);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    uint64_t v0 = ray_enccol_test_desc_validations();
    for (int64_t at = 0; at < 65536; at += RAY_ENC_VEC) {
        TEST_ASSERT_NULL(ray_col_range(&s, at, RAY_ENC_VEC, &sc, &r));
        TEST_ASSERT_EQ_I(((const int32_t*)r.data)[3], (int32_t)((at + 3) % 977));
    }
    TEST_ASSERT_EQ_U(ray_enccol_test_desc_validations() - v0, 1);     /* 64 vectors, one validation */
    ray_col_scratch_t sc2; ray_col_scratch_init(&sc2);
    TEST_ASSERT_NULL(ray_col_range(&s, 65536 + 5000, 10, &sc, &r));
    TEST_ASSERT_NULL(ray_col_range(&s, 65536 + 9000, 10, &sc2, &r));   /* another scratch: still validated */
    TEST_ASSERT_EQ_U(ray_enccol_test_desc_validations() - v0, 2);
    ray_t* m = ray_enccol_materialize(c);                              /* only chunk 2 is new */
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    TEST_ASSERT_EQ_U(ray_enccol_test_desc_validations() - v0, 3);
    ray_col_scratch_free(&sc2); ray_col_scratch_free(&sc); ray_col_src_release(&s);
    ray_release(c);
    /* chunk 1 descriptor 0 reserved bytes set: corrupt on every read, re-validated each time */
    uint64_t off = 0;
    FILE* f = fopen(p, "rb");
    TEST_ASSERT_TRUE(f != NULL);
    TEST_ASSERT_TRUE(fseek(f, (long)(RAY_ENC_DIR_OFF + sizeof(ray_enc_dirent_t)), SEEK_SET) == 0 && fread(&off, 8, 1, f) == 1);
    fclose(f);
    uint8_t one = 1;
    patch(p, (long)off + 6, &one, 1);
    c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    TEST_ASSERT_NULL(ray_col_src_bind(c, &s)); ray_col_scratch_init(&sc);
    v0 = ray_enccol_test_desc_validations();
    for (int i = 0; i < 3; i++) {
        ray_t* e = ray_col_range(&s, 65536 + 1024 * i, 1024, &sc, &r);
        TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
        TEST_ASSERT_STR_EQ(ray_err_code(e), "corrupt");
        ray_error_free(e);
        TEST_ASSERT_EQ_U(ray_enccol_test_desc_validations() - v0, (uint64_t)i + 1);
    }
    TEST_ASSERT_NULL(ray_col_range(&s, 0, 1024, &sc, &r));             /* chunk 0 unaffected */
    ray_col_scratch_free(&sc); ray_col_src_release(&s);
    ray_release(c);
    PASS();
}

/* The scratch references the column it counts for: freeing it after its
 * source and column are released is safe and still counts (ASan). */
static test_result_t test_scratch_outlives_source(void) {
    const char* p = ray_test_tmp(ENCCOL_TMP "/so");
    TEST_ASSERT_EQ_I(save_i32_rows(p, 5000), RAY_OK);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    ray_col_range_t r;
    ray_enc_counters_reset();
    int64_t live = ray_enccol_test_live();
    TEST_ASSERT_NULL(ray_col_range(&s, 1024, 1024, &sc, &r));
    ray_col_src_release(&s);
    ray_release(c);                                   /* the scratch's reference keeps the column */
    TEST_ASSERT_EQ_I(ray_enccol_test_live(), live);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);             /* not counted yet */
    ray_col_scratch_free(&sc);                        /* counts, then drops the last reference */
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 1024 * 4);
    TEST_ASSERT_EQ_I(ray_enccol_test_live(), live - 1);
    PASS();
}

/* Concurrent first touch: eight threads range-read every vector of the same
 * unvalidated chunk at once, each in its own order; every answer is right
 * and the chunk ends validated. */
typedef struct { ray_col_src_t* s; int seed; int bad; } ftouch_t;
static void first_touch_thread(void* raw) {
    ftouch_t* t = raw;
    ray_col_scratch_t sc; ray_col_scratch_init(&sc);
    for (int i = 0; i < 64; i++) {
        int j = (i * 37 + t->seed * 11) % 64;
        ray_col_range_t r;
        ray_t* e = ray_col_range(t->s, (int64_t)j * RAY_ENC_VEC, RAY_ENC_VEC, &sc, &r);
        if (e) { ray_error_free(e); t->bad++; continue; }
        for (int k = 0; k < RAY_ENC_VEC; k++)
            if (((const int32_t*)r.data)[k] != (int32_t)(((int64_t)j * RAY_ENC_VEC + k) % 977)) { t->bad++; break; }
    }
    ray_col_scratch_free(&sc);
    ray_heap_abandon();
}

static test_result_t test_desc_first_touch_concurrent(void) {
    const char* p = ray_test_tmp(ENCCOL_TMP "/ft");
    TEST_ASSERT_EQ_I(save_i32_rows(p, 65536), RAY_OK);
    for (int round = 0; round < 4; round++) {
        ray_t* c = ray_col_mmap(p);
        TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
        ray_col_src_t s; TEST_ASSERT_NULL(ray_col_src_bind(c, &s));
        ray_enccol_t* e = ray_enccol_of(c);
        TEST_ASSERT_EQ_I(atomic_load(&e->desc_ok[0]), 0);
        ftouch_t t[8]; ray_thread_t th[8];
        for (int i = 0; i < 8; i++) { t[i].s = &s; t[i].seed = i + round; t[i].bad = 0; ray_thread_create(&th[i], first_touch_thread, &t[i]); }
        for (int i = 0; i < 8; i++) { ray_thread_join(th[i]); TEST_ASSERT_EQ_I(t[i].bad, 0); }
        TEST_ASSERT_EQ_I(atomic_load(&e->desc_ok[0]), 1);
        ray_col_src_release(&s);
        ray_release(c);
    }
    PASS();
}

static test_result_t test_decode_rejects_payload(void) {
    /* FOR_BP vector whose base has bits above T: */
    int32_t d[RAY_ENC_VEC];
    for (int i = 0; i < RAY_ENC_VEC; i++) d[i] = 5000 + (i % 64);
    ray_t* v = ray_vec_from_raw(RAY_I32, d, RAY_ENC_VEC);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/p"), 2), RAY_OK);
    ray_release(v);
    uint64_t bad_base = 0x100000000ull;               /* payload starts at 160 + 3072 */
    patch(ray_test_tmp(ENCCOL_TMP "/p"), 160 + 3072, &bad_base, 8);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/p"));          /* load passes: payload unread */
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    uint8_t out[4096];
    ray_t* err = ray_enccol_decode_vec(ray_enccol_of(c), 0, out, NULL, 0, 0);
    TEST_ASSERT_TRUE(err && RAY_IS_ERR(err));
    TEST_ASSERT_STR_EQ(ray_err_code(err), "corrupt");
    TEST_ASSERT_TRUE(strstr(ray_error_msg(), ray_test_tmp(ENCCOL_TMP "/p")) != NULL);
    ray_error_free(err);
    ray_release(c);
    /* A trailing descriptor that is not all zero: */
    v = ray_vec_from_raw(RAY_I32, d, 10);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/p"), 2), RAY_OK);
    ray_release(v);
    uint8_t one = 1;
    patch(ray_test_tmp(ENCCOL_TMP "/p"), 160 + 48 * 5 + 3, &one, 1);
    c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/p"));
    err = ray_enccol_decode_vec(ray_enccol_of(c), 0, out, NULL, 0, 0);
    TEST_ASSERT_TRUE(err && RAY_IS_ERR(err));
    TEST_ASSERT_STR_EQ(ray_err_code(err), "corrupt");
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
    if (!f) ray_test_fatal("cannot open %s", path);
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
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/tb"), 2), RAY_OK);
        ray_release(v);
        ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/tb"));
        TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
        TEST_ASSERT_EQ_U(ray_enccol_of(c)->hdr.side_off, 0);     /* the payload is the file's tail */
        ray_release(c);
        grow_payload(ray_test_tmp(ENCCOL_TMP "/tb"));
        c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/tb"));
        TEST_ASSERT_FMT(c && !RAY_IS_ERR(c), "type %d shape %d: load", (int)cases[i].type, cases[i].s);
        char what[64]; snprintf(what, sizeof what, "type %d shape %d", (int)cases[i].type, cases[i].s);
        test_result_t r = decode_expect_corrupt(ray_enccol_of(c), 0, what);
        ray_release(c);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* A DICT body is refused without a usable dictionary.  The explicit NULL
 * guards in the codec's size / write / read bodies sit behind the
 * legality check every public entry runs first, so they are not reachable
 * directly; this pins the observable behaviour of the public entries. */
static test_result_t test_dict_null_dictionary(void) {
    uint64_t v[16]; uint64_t dv[2] = { 10, 20 };
    for (int i = 0; i < 16; i++) v[i] = dv[i & 1];
    ray_enc_kind_t k; TEST_ASSERT_TRUE(ray_enc_kind_of(RAY_I64, 0, &k));
    ray_enc_dict_t d = { dv, 2 };
    ray_enc_buf_t buf = {0}, buf2 = {0}; ray_enc_choice_t ch;
    bool wrote = ray_enc_write_as(RAY_ENC_DICT, v, 16, k, &d, &buf, &ch);
    bool wrote_null = ray_enc_write_as(RAY_ENC_DICT, v, 16, k, NULL, &buf2, &ch);
    uint64_t out[16];
    int64_t good = wrote ? ray_enc_read(buf.p, buf.len, RAY_ENC_DICT, 0, 16, k, RAY_ENC_SLOT_TOP, 0, &d, out) : -2;
    int64_t nul = wrote ? ray_enc_read(buf.p, buf.len, RAY_ENC_DICT, 0, 16, k, RAY_ENC_SLOT_TOP, 0, NULL, out) : -2;
    ray_enc_dict_t empty = { NULL, 0 };
    int64_t emp = wrote ? ray_enc_read(buf.p, buf.len, RAY_ENC_DICT, 0, 16, k, RAY_ENC_SLOT_TOP, 0, &empty, out) : -2;
    size_t blen = buf.len;
    ray_enc_buf_free(&buf); ray_enc_buf_free(&buf2);
    TEST_ASSERT_TRUE(wrote);
    TEST_ASSERT_FALSE(wrote_null);
    TEST_ASSERT_EQ_I(good, (int64_t)blen);
    TEST_ASSERT_TRUE(nul < 0);
    TEST_ASSERT_TRUE(emp < 0);
    PASS();
}

/* Out-of-range vector numbers are refused by every column-global entry. */
static test_result_t test_vec_range_checks(void) {
    ray_t* v = make_col(RAY_STR, 2500, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/vr"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/vr"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_t* e = ray_enccol_of(c);
    int64_t nv = ray_enc_nvecs(e->len);
    TEST_ASSERT_EQ_I(nv, 3);
    static const int64_t bad[] = { -1, 3, 64, 1000000 };
    int bad_ok = 1;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint64_t pb = 7; int nn = 7;
        ray_t* err = ray_enccol_vec_pool_bytes(e, bad[i], &pb);
        if (!err || strcmp(ray_err_code(err), "range") != 0 || pb != 0) bad_ok = 0;
        if (err) ray_error_free(err);
        if (ray_enccol_vec_nulls(e, bad[i], &nn) != -1) bad_ok = 0;
    }
    uint64_t pb; int nn;
    ray_t* e0 = ray_enccol_vec_pool_bytes(e, nv - 1, &pb);
    int good_ok = e0 == NULL && ray_enccol_vec_nulls(e, nv - 1, &nn) == 0 && nn == 0;
    if (e0) ray_error_free(e0);
    ray_release(c);
    TEST_ASSERT_TRUE(bad_ok);
    TEST_ASSERT_TRUE(good_ok);
    PASS();
}

/* The descriptors' null counts must add up to the directory's. */
static test_result_t test_descriptor_null_sum(void) {
    ray_t* v = make_col(RAY_I32, 3000, 4);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/ns"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/ns"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enc_dirent_t de; memcpy(&de, ray_enccol_of(c)->base + 96, sizeof de);
    ray_release(c);
    TEST_ASSERT_TRUE(de.null_count > 0);
    uint32_t nc = de.null_count - 1; patch(ray_test_tmp(ENCCOL_TMP "/ns"), 96 + 52, &nc, 4);
    c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/ns"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));                  /* directory alone is plausible */
    ray_t* err = ray_enccol_decode_vec(ray_enccol_of(c), 0, (uint8_t[RAY_ENC_VEC * 16]){0}, NULL, 0, 0);
    bool ok = err && RAY_IS_ERR(err) && strcmp(ray_err_code(err), "corrupt") == 0;
    if (err) ray_error_free(err);
    ray_release(c);
    TEST_ASSERT_TRUE(ok);
    PASS();
}

/* STR statistics on a RAW / CONST vector are corrupt, not "newer build". */
static test_result_t test_str_stats_corrupt(void) {
    ray_t* v = make_col(RAY_STR, 100, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/ss"), 2), RAY_OK);
    ray_release(v);
    uint16_t fl = 0; patch(ray_test_tmp(ENCCOL_TMP "/ss"), 160 + 4, &fl, 2);     /* descriptor claims statistics */
    uint64_t one = 1; patch(ray_test_tmp(ENCCOL_TMP "/ss"), 160 + 40, &one, 8);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/ss"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_t* err = ray_enccol_decode_vec(ray_enccol_of(c), 0, (uint8_t[RAY_ENC_VEC * 16]){0}, (char[RAY_ENC_VEC * 64]){0}, 0, RAY_ENC_VEC * 64);
    bool ok = err && RAY_IS_ERR(err) && strcmp(ray_err_code(err), "corrupt") == 0;
    if (err) ray_error_free(err);
    ray_release(c);
    TEST_ASSERT_TRUE(ok);
    PASS();
}

/* SYM ids must fit the attrs width and lie below the domain bound. */
static test_result_t test_sym_width_and_bound(void) {
    for (int i = 256; i < 400; i++) { char b[8]; int n = snprintf(b, sizeof b, "s%d", i); (void)ray_sym_intern(b, (size_t)n); }
    /* ids above 255 in a column whose attrs say W8: */
    ray_t* v = ray_sym_vec_new(RAY_SYM_W16, 2048); v->len = 2048;
    for (int i = 0; i < 2048; i++) ((uint16_t*)ray_data(v))[i] = (uint16_t)(260 + i % 100);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/sw"), 2), RAY_OK);
    ray_release(v);
    uint8_t w8 = RAY_SYM_W8;
    patch(ray_test_tmp(ENCCOL_TMP "/sw"), (long)offsetof(ray_t, attrs), &w8, 1);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/sw"));
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
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/sb"), 2), RAY_OK);
    ray_release(v);
    uint32_t bound = 50; patch(ray_test_tmp(ENCCOL_TMP "/sb"), RC_OFF, &bound, 4);
    uint64_t z = 0; uint32_t fl = RAY_ENC_STAT_ABSENT; uint16_t fl16 = RAY_ENC_STAT_ABSENT;
    patch(ray_test_tmp(ENCCOL_TMP "/sb"), 96 + 16, &z, 8); patch(ray_test_tmp(ENCCOL_TMP "/sb"), 96 + 24, &z, 8); patch(ray_test_tmp(ENCCOL_TMP "/sb"), 96 + 56, &fl, 4);
    patch(ray_test_tmp(ENCCOL_TMP "/sb"), 160 + 32, &z, 8); patch(ray_test_tmp(ENCCOL_TMP "/sb"), 160 + 40, &z, 8); patch(ray_test_tmp(ENCCOL_TMP "/sb"), 160 + 4, &fl16, 2);
    c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/sb"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    TEST_ASSERT_EQ_U(ray_enccol_of(c)->sym_bound, 50);
    test_result_t r = decode_expect_corrupt(ray_enccol_of(c), 0, "id above the bound");
    ray_release(c);
    return r;
}

/* Flip bits of every byte of the file past the outer header, one byte at a
 * time, in several fixtures: a load either fails or every vector decodes to
 * OK or a corrupt / nyi error, and a failed decode leaves `out` untouched. */
static bool sweep_nyi_ok(const uint8_t* b, long n, long pos) {
    /* a descriptor's encoding byte, or a STR directory entry's flags word */
    int64_t len; memcpy(&len, b + offsetof(ray_t, len), 8);
    int8_t type = (int8_t)b[offsetof(ray_t, type)];
    for (int64_t c = 0; c < ray_enc_nchunks(len); c++) {
        uint64_t off; memcpy(&off, b + 96 + 64 * c, 8);
        if (type == RAY_STR && pos >= 96 + 64 * c + 56 && pos < 96 + 64 * c + 60) return true;
        for (int j = 0; j < 64; j++) if ((long)(off + 48 * j) == pos && pos < n) return true;
    }
    return false;
}

static test_result_t sweep_file(const char* name, uint8_t mask, long* positions) {
    char src[256]; snprintf(src, sizeof src, "test/data/enc/%s.bin", name);
    FILE* f = fopen(src, "rb");
    TEST_ASSERT_FMT(f != NULL, "fixture %s", name);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n);
    TEST_ASSERT_TRUE(fread(b, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    const char* path = ray_test_tmp(ENCCOL_TMP "/sweep");
    f = fopen(path, "wb");
    if (!f) free(b);
    TEST_ASSERT_TRUE(f != NULL);
    fwrite(b, 1, (size_t)n, f); fclose(f);
    const char* bad = NULL;
    for (long pos = 32; pos < n && !bad; pos++) {
        uint8_t m = b[pos] ^ mask;
        patch(path, pos, &m, 1);
        ray_t* c = ray_col_mmap(path);
        if (c && RAY_IS_ERR(c)) {
            if (strcmp(ray_err_code(c), "nyi") == 0 && !sweep_nyi_ok(b, n, pos)) bad = "nyi at load outside the allowed bytes";
            ray_error_free(c);
        }
        else if (c) {
            ray_enccol_t* e = ray_enccol_of(c);
            for (int64_t vec = 0; e && vec < ray_enc_nvecs(e->len) && !bad; vec++) {
                uint8_t out[RAY_ENC_VEC * 16]; char pool[RAY_ENC_VEC * 64];
                memset(out, CANARY, sizeof out); memset(pool, CANARY, sizeof pool);
                ray_t* err = ray_enccol_decode_vec(e, vec, out, pool, 0, sizeof pool);
                if (!err) continue;
                const char* code = ray_err_code(err);
                if (strcmp(code, "nyi") == 0) { if (!sweep_nyi_ok(b, n, pos)) bad = "nyi outside the allowed bytes"; }
                else if (strcmp(code, "corrupt") != 0) bad = "error code";
                for (size_t i = 0; i < sizeof out && !bad; i++) if (out[i] != CANARY) bad = "out written by a failed decode";
                for (size_t i = 0; i < sizeof pool && !bad; i++) if (pool[i] != (char)CANARY) bad = "pool written by a failed decode";
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

/* ---- Task 8: materialization ---- */

/* Same vector saved as generation 0 and 2: the generation-0 load and the
 * materialized generation-2 column are equal. */
static test_result_t check_equivalence(ray_t* v) {
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/g0"), 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/g2"), 2), RAY_OK);
    ray_t* a = ray_col_load(ray_test_tmp(ENCCOL_TMP "/g0"));
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/g2"));
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a) && c && !RAY_IS_ERR(c));
    ray_t* b = ray_enccol_materialize(c);
    TEST_ASSERT_FMT(b && !RAY_IS_ERR(b), "materialize: %s", b ? ray_err_code(b) : "null");
    TEST_ASSERT_EQ_I(b->type, a->type);
    TEST_ASSERT_EQ_I(b->len, a->len);
    TEST_ASSERT_EQ_I(b->attrs & COL_DISK_ATTRS_MASK, a->attrs & COL_DISK_ATTRS_MASK);
    if (a->type == RAY_STR) {
        const ray_str_t* x = ray_data(a); const ray_str_t* y = ray_data(b);
        const char* px = a->str_pool ? ray_data(a->str_pool) : NULL;
        const char* py = b->str_pool ? ray_data(b->str_pool) : NULL;
        for (int64_t i = 0; i < a->len; i++) {
            TEST_ASSERT_FMT(x[i].len == y[i].len, "len row %lld", (long long)i);
            TEST_ASSERT_FMT(memcmp(ray_str_t_ptr(&x[i], px), ray_str_t_ptr(&y[i], py), x[i].len) == 0, "row %lld", (long long)i);
            if (!ray_str_is_inline(&y[i])) TEST_ASSERT_FMT(y[i].hash32 == x[i].hash32, "hash32 row %lld", (long long)i);
        }
    } else {
        TEST_ASSERT_TRUE(memcmp(ray_data(a), ray_data(b), (size_t)a->len * ray_sym_elem_size(a->type, a->attrs)) == 0);
    }
    if (a->type == RAY_SYM) {
        TEST_ASSERT_EQ_I(b->attrs & RAY_SYM_W_MASK, a->attrs & RAY_SYM_W_MASK);   /* width class */
        TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(b), ray_sym_vec_domain(a));        /* domain */
    }
    TEST_ASSERT_EQ_PTR(ray_enccol_materialize(c), b);       /* cached */
    ray_retain(b);
    ray_release(c);                                          /* retained column outlives the object */
    TEST_ASSERT_EQ_I(b->len, a->len);
    ray_release(b);
    ray_release(a);
    PASS();
}

static test_result_t test_materialize_equivalence(void) {
    static const int8_t types[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIME,
                                    RAY_TIMESTAMP, RAY_F32, RAY_F64, RAY_GUID, RAY_SYM, RAY_STR };
    for (size_t t = 0; t < sizeof types; t++)
        for (int s = 0; s < 6; s++) {
            if ((types[t] == RAY_BOOL || types[t] == RAY_U8) && s >= 4) continue;
            ray_t* v = make_col(types[t], 140000, s);       /* three chunks, parallel decode */
            test_result_t r = check_equivalence(v);
            ray_release(v);
            if (r.status != TEST_PASS) return r;
        }
    PASS();
}

/* Every type with nulls at the boundary lengths; SYM at every width. */
static test_result_t test_materialize_lengths(void) {
    static const int8_t types[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIME,
                                    RAY_TIMESTAMP, RAY_F32, RAY_F64, RAY_GUID, RAY_SYM, RAY_STR };
    static const int64_t lens[] = { 0, 1, 1023, 1024, 1025, 65535, 65536, 65537 };
    static const uint8_t widths[] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64 };
    for (size_t t = 0; t < sizeof types; t++)
        for (size_t l = 0; l < sizeof lens / sizeof *lens; l++)
            for (int s = 0; s < 5; s += 4)                   /* shapes 0 (no nulls) and 4 (nulls) */
                for (size_t w = 0; w < (types[t] == RAY_SYM ? 4u : 1u); w++) {
                    g_symw = widths[w];
                    ray_t* v = make_col(types[t], lens[l], s);
                    test_result_t r = check_equivalence(v);
                    ray_release(v);
                    g_symw = RAY_SYM_W16;
                    if (r.status != TEST_PASS) return r;
                }
    PASS();
}

/* Float payloads survive bit for bit: NaN payloads, -0.0, infinities, the null sentinel. */
static test_result_t test_materialize_float_bits(void) {
    static const uint64_t d64[] = { 0x7ff8000000000123ull, 0x7ff0000000000001ull, 0x8000000000000000ull,
                                    0x0000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
                                    0xfff8000000000abcull, 0x3ff0000000000000ull };
    static const uint32_t d32[] = { 0x7fc00123u, 0x7f800001u, 0x80000000u, 0u, 0x7f800000u, 0xff800000u, 0xffc00abcu, 0x3f800000u };
    for (int k = 0; k < 2; k++) {
        int64_t n = 70000;
        ray_t* v = ray_vec_new(k ? RAY_F32 : RAY_F64, n); v->len = n;
        for (int64_t i = 0; i < n; i++) {
            if (k) memcpy((uint8_t*)ray_data(v) + i * 4, &d32[(i / 3) % 8], 4);
            else   memcpy((uint8_t*)ray_data(v) + i * 8, &d64[(i / 3) % 8], 8);
        }
        test_result_t r = check_equivalence(v);
        ray_release(v);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* ---- Task 25: decoded peek and the in-place column list ---- */

static void peek_thread(void* arg) {
    ray_t* c = arg;
    for (int i = 0; i < 20000; i++) {
        ray_t* d = ray_enccol_decoded_peek(c);
        if (d) (void)d->len;
    }
}
static void peek_mat_thread(void* arg) { (void)ray_enccol_materialize((ray_t*)arg); }

static test_result_t test_decoded_peek(void) {
    ray_t* v = make_col(RAY_I64, 5000, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/pk"), 2), RAY_OK);
    ray_release(v);
    ray_enc_counters_reset();
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/pk"));
    TEST_ASSERT_NULL(ray_enccol_decoded_peek(c));
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);                   /* a peek decodes nothing */
    ray_thread_t th[3];
    ray_thread_create(&th[0], peek_thread, c);
    ray_thread_create(&th[1], peek_thread, c);
    ray_thread_create(&th[2], peek_mat_thread, c);
    for (int i = 0; i < 3; i++) ray_thread_join(th[i]);
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    TEST_ASSERT_EQ_PTR(ray_enccol_decoded_peek(c), m);
    ray_release(c);
    PASS();
}

static test_result_t test_cols_inplace_encoded(void) {
    ray_t* v = make_col(RAY_I64, 100, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/ci"), 2), RAY_OK);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/ci"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_t* t = ray_table_new(2);
    t = ray_table_add_col(t, ray_sym_intern("a", 1), v);
    TEST_ASSERT_TRUE(ray_table_cols_inplace(t) != NULL);
    t = ray_table_add_col(t, ray_sym_intern("b", 1), c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t));
    TEST_ASSERT_NULL(ray_table_cols_inplace(t));           /* an encoded slot refuses */
    ray_release(t); ray_release(c); ray_release(v);
    PASS();
}

static test_result_t test_materialize_counts_once(void) {
    ray_t* v = make_col(RAY_I64, 5000, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/m"), 2), RAY_OK);
    ray_release(v);
    ray_enc_counters_reset();
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/m"));
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);                   /* load decodes nothing */
    TEST_ASSERT_EQ_U(k.decoded_bytes, 0);
    ray_t* a = ray_enccol_materialize(c); ray_t* b = ray_enccol_materialize(c);
    TEST_ASSERT_EQ_PTR(a, b);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    TEST_ASSERT_EQ_U(k.decoded_bytes, 5000 * 8);
    TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(c)->decoded_bytes), 5000 * 8);   /* per column */
    ray_release(c);
    PASS();
}

/* Cancellation is transient: reported, not cached, retried. */
static test_result_t test_cancel_retried(void) {
    ray_t* v = make_col(RAY_I32, 3 * 65536, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/cn"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/cn"));
    ray_request_interrupt();
    ray_t* r = ray_enccol_materialize(c);
    ray_clear_interrupt();
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "cancel");
    ray_error_free(r);
    r = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(c);
    PASS();
}

/* The per-chunk poll: the interrupt arrives after chunk 0 has decoded. */
static test_result_t test_cancel_mid_decode(void) {
    ray_t* v = make_col(RAY_I32, 128 * 65536, 1);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/cm"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/cm"));
    ray_enccol_test_interrupt_after(1);
    ray_t* r = ray_enccol_materialize(c);
    ray_enccol_test_interrupt_after(0);
    ray_clear_interrupt();
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "cancel");
    ray_error_free(r);
    TEST_ASSERT_TRUE(atomic_load(&ray_enccol_of(c)->decoded_bytes) < (uint64_t)128 * 65536 * 4);   /* stopped early */
    r = ray_enccol_materialize(c);                          /* retried */
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(c);
    PASS();
}

/* A waiter on a cancelled attempt receives the attempt's "cancel". */
typedef struct { ray_t* c; char code[8]; } cwait_t;   /* code: "" on success */
static void cancel_wait_thread(void* raw) {
    cwait_t* t = raw;
    ray_t* r = ray_enccol_materialize(t->c);
    t->code[0] = 0;
    if (r && RAY_IS_ERR(r)) { snprintf(t->code, sizeof t->code, "%s", ray_err_code(r)); ray_error_free(r); }
    ray_heap_abandon();
}
static test_result_t test_cancel_waiter(void) {
    ray_t* v = make_col(RAY_I32, 128 * 65536, 1);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/cw"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/cw"));
    ray_enccol_t* e = ray_enccol_of(c);
    cwait_t p = { c, "" }, w = { c, "" };
    ray_thread_t tp, tw;
    ray_thread_create(&tp, cancel_wait_thread, &p);
    while (atomic_load(&e->once.state) != RAY_ENC_ONCE_RUNNING) ray_enc_yield();
    ray_thread_create(&tw, cancel_wait_thread, &w);
    while (ray_enc_once_waiters(&e->once) < 1) ray_enc_yield();
    ray_request_interrupt();
    ray_thread_join(tp); ray_thread_join(tw);
    ray_clear_interrupt();
    TEST_ASSERT_STR_EQ(p.code, "cancel");
    TEST_ASSERT_STR_EQ(w.code, "cancel");
    ray_t* r = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(c);
    PASS();
}

/* ray_col_load_dom on a generation-2 SYM file: the loaded vector holds its own
 * reference on the symfile domain (owned branch) and outlives the object. */
static test_result_t test_load_dom_refcount(void) {
    const char* p = ray_test_tmp(ENCCOL_TMP "/ld");
    TEST_ASSERT_NULL(sym_col_save(p));
    ray_sym_domain_t* dom = ray_sym_domain_new();
    for (int i = 0; i < 400; i++) { char b[12]; int n = snprintf(b, sizeof b, "d%d", i); (void)ray_sym_domain_intern(dom, b, (size_t)n); }
    ray_t* b = ray_col_load_dom(p, dom);
    TEST_ASSERT_TRUE(b && !RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(b->type, RAY_SYM);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(b), dom);
    ray_sym_domain_release(dom);                            /* ours; the vector keeps the domain alive */
    ray_t* s = ray_sym_domain_str(ray_sym_vec_domain(b), 1);
    TEST_ASSERT_TRUE(s != NULL);
    ray_release(b);                                         /* last reference: frees the domain (ASan checks) */
    PASS();
}

/* Corrupt descriptors in chunks 1 and 3: the error names chunk 1 (the
 * lowest failing task), carries the path, and is cached. */
static test_result_t test_materialize_lowest_failure_cached(void) {
    ray_t* v = make_col(RAY_I32, 4 * 65536, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/f"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/f"));
    ray_enccol_t* e = ray_enccol_of(c);
    uint64_t off1, off3;
    memcpy(&off1, e->base + 96 + 64 * 1, 8);
    memcpy(&off3, e->base + 96 + 64 * 3, 8);
    ray_release(c);
    uint8_t one = 1;
    patch(ray_test_tmp(ENCCOL_TMP "/f"), (long)off1 + 6, &one, 1);        /* descriptor 0 reserved byte */
    patch(ray_test_tmp(ENCCOL_TMP "/f"), (long)off3 + 6, &one, 1);
    c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/f"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    uint64_t dec0 = 0;
    for (int i = 0; i < 3; i++) {
        ray_error_clear();
        ray_t* r = ray_enccol_materialize(c);
        if (i == 0) dec0 = atomic_load(&ray_enccol_of(c)->decoded_bytes);
        else TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(c)->decoded_bytes), dec0);   /* cached: no re-decode */
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), "chunk 1 ") != NULL);
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), ray_test_tmp(ENCCOL_TMP "/f")) != NULL);
        ray_error_free(r);
    }
    ray_release(c);
    PASS();
}

static test_result_t test_oom_then_retry(void) {
    ray_t* v = make_col(RAY_STR, 3000, 2);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/o"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/o"));
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    bool done = false;
    for (int nth = 1; nth <= 16 && !done; nth++) {          /* until an attempt passes every injection point */
        ray_enccol_test_fail_alloc(nth);
        ray_t* r = ray_enccol_materialize(c);
        ray_enccol_test_fail_alloc(0);
        if (r && RAY_IS_ERR(r)) {
            TEST_ASSERT_STR_EQ(ray_err_code(r), "oom");
            ray_error_free(r);
            ray_mem_stats(&m1);
            TEST_ASSERT_FMT(m1.bytes_allocated == m0.bytes_allocated, "nth %d: %llu vs %llu", nth, (unsigned long long)m1.bytes_allocated, (unsigned long long)m0.bytes_allocated);   /* a failed attempt freed everything */
        } else done = true;
    }
    TEST_ASSERT_TRUE(done);
    ray_t* r = ray_enccol_materialize(c);                   /* the success is cached */
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(c);
    PASS();
}

static test_result_t test_col_load_materializes(void) {
    ray_t* v = make_col(RAY_F64, 2000, 4);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/l"), 2), RAY_OK);
    ray_t* b = ray_col_load(ray_test_tmp(ENCCOL_TMP "/l"));
    TEST_ASSERT_TRUE(b && !RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(b->type, RAY_F64);
    TEST_ASSERT_TRUE(memcmp(ray_data(b), ray_data(v), 2000 * 8) == 0);
    ray_release(b); ray_release(v);
    PASS();
}

static ray_t* save_and_materialize(ray_t* v, const char* path, ray_t** keep) {
    if (ray_col_save_gen(v, path, 2) != RAY_OK) return ray_error("io", NULL);
    *keep = ray_col_mmap(path);
    if (!*keep || RAY_IS_ERR(*keep)) return *keep;
    return ray_enccol_materialize(*keep);
}

/* Overwrite rows [from, to) of an 8- or 4-byte element column with its null. */
static void null_rows(ray_t* v, int64_t from, int64_t to) {
    uint8_t* d = ray_data(v);
    for (int64_t i = from; i < to; i++)
        switch (v->type) {
        case RAY_I32: case RAY_DATE: case RAY_TIME: { int32_t x = NULL_I32; memcpy(d + i * 4, &x, 4); break; }
        case RAY_I64: case RAY_TIMESTAMP:           { int64_t x = NULL_I64; memcpy(d + i * 8, &x, 8); break; }
        case RAY_F64:                               { double x = NULL_F64;  memcpy(d + i * 8, &x, 8); break; }
        default: break;
        }
}

static test_result_t test_synth_zone(void) {
    static const int8_t types[] = { RAY_I32, RAY_I64, RAY_F64 };
    for (size_t t = 0; t < 3; t++) {
        int64_t n = 2 * 65536 + 5000;                      /* three chunks */
        ray_t* v = make_col(types[t], n, 4);
        if (types[t] == RAY_F64) {                         /* -0.0 / +0.0 tie across a chunk boundary */
            double* f = ray_data(v);
            for (int64_t i = 0; i < n; i++) if (f[i] == 0.0 || f[i] < 0.0) f[i] = 1.0;
            f[65535] = 0.0; f[65536] = -0.0; f[10] = 5.0;
            f[2 * 65536 - 1] = -0.0; f[2 * 65536] = 0.0;
            for (int64_t i = 0; i < n; i++) if (i != 65535 && i != 65536 && i != 2 * 65536 - 1 && i != 2 * 65536 && f[i] <= 0.0) f[i] = 1.0;
            for (int64_t i = 0; i < 65535; i += 7) f[i] = 2.0;
        }
        ray_t* z = ray_index_attach_zone(&v);
        TEST_ASSERT_TRUE(z && !RAY_IS_ERR(z));
        v = z;
        ray_t* keep;
        ray_t* m = save_and_materialize(v, ray_test_tmp(ENCCOL_TMP "/z"), &keep);
        TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
        TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_ZONE);
        ray_index_t* a = ray_index_payload(v->index);
        ray_index_t* b = ray_index_payload(m->index);
        if (types[t] == RAY_F64) {
            TEST_ASSERT_TRUE(memcmp(&b->u.zone.min_f, &a->u.zone.min_f, 8) == 0);   /* bit-equal: the sign of a tie */
            TEST_ASSERT_TRUE(memcmp(&b->u.zone.max_f, &a->u.zone.max_f, 8) == 0);
        } else {
            TEST_ASSERT_EQ_I(b->u.zone.min_i, a->u.zone.min_i);
            TEST_ASSERT_EQ_I(b->u.zone.max_i, a->u.zone.max_i);
        }
        TEST_ASSERT_EQ_I(b->u.zone.n_nulls, a->u.zone.n_nulls);
        TEST_ASSERT_EQ_I(b->built_for_len, n);
        ray_release(keep); ray_release(v);
    }
    PASS();
}

/* The synthesized chunk zone's arrays equal the builder's, aggs only when
 * the type is summable (NULL otherwise). */
static test_result_t chunk_zone_equal(ray_t* b_idx, ray_t* a_idx, bool summable) {
    ray_index_t* a = ray_index_payload(a_idx);
    ray_index_t* b = ray_index_payload(b_idx);
    uint32_t nc = a->u.chunk_zone.n_chunks;
    TEST_ASSERT_EQ_I(b->u.chunk_zone.n_chunks, nc);
    TEST_ASSERT_EQ_I(b->u.chunk_zone.chunk_log2, a->u.chunk_zone.chunk_log2);
    TEST_ASSERT_EQ_I(b->u.chunk_zone.is_f64, a->u.chunk_zone.is_f64);
    TEST_ASSERT_TRUE(memcmp(ray_data(b->u.chunk_zone.mins), ray_data(a->u.chunk_zone.mins), 8 * nc) == 0);
    TEST_ASSERT_TRUE(memcmp(ray_data(b->u.chunk_zone.maxs), ray_data(a->u.chunk_zone.maxs), 8 * nc) == 0);
    TEST_ASSERT_TRUE(memcmp(ray_data(b->u.chunk_zone.null_bits), ray_data(a->u.chunk_zone.null_bits), (nc + 7) / 8) == 0);
    if (summable) {
        TEST_ASSERT_TRUE(b->u.chunk_zone.aggs && a->u.chunk_zone.aggs);
        TEST_ASSERT_TRUE(memcmp(ray_data(b->u.chunk_zone.aggs), ray_data(a->u.chunk_zone.aggs), 8 * 3 * nc) == 0);
    } else {
        TEST_ASSERT_TRUE(b->u.chunk_zone.aggs == NULL);
    }
    PASS();
}

/* CHUNK_ZONE synthesized at 64K chunks equals the builder's on the
 * decoded vector, for integer, float and temporal columns with an all-null chunk. */
static test_result_t test_synth_chunk_zone(void) {
    static const int8_t types[] = { RAY_I64, RAY_F64, RAY_DATE, RAY_TIMESTAMP };
    for (size_t t = 0; t < 4; t++) {
        int64_t n = 3 * 65536 + 17;
        ray_t* v = make_col(types[t], n, 4);
        null_rows(v, 65536, 2 * 65536);                           /* chunk 1 all null */
        ray_t* z = ray_index_attach_chunk_zone(&v, 16);
        TEST_ASSERT_TRUE(z && !RAY_IS_ERR(z)); v = z;
        ray_t* keep;
        ray_t* m = save_and_materialize(v, ray_test_tmp(ENCCOL_TMP "/cz"), &keep);
        TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
        TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_CHUNK_ZONE);
        ray_t* ref = ray_index_chunk_zone_compute(m, 16);         /* builder on the decoded data */
        TEST_ASSERT_TRUE(ref && !RAY_IS_ERR(ref));
        test_result_t r = chunk_zone_equal(m->index, ref, ray_encw_type_summable(types[t]));
        if (r.status != TEST_PASS) return r;
        ray_release(ref); ray_release(keep); ray_release(v);
    }
    PASS();
}

static test_result_t test_unique_flag(void) {
    int64_t d[100]; for (int i = 0; i < 100; i++) d[i] = i * 3;
    ray_t* v = ray_vec_from_raw(RAY_I64, d, 100);
    ray_t* root = ray_index_new_root(RAY_IDX_NONE, RAY_I64, 100);
    ray_index_payload(root)->markers = RAY_MARK_UNIQUE;
    ray_t* a = ray_index_attach_persisted(&v, root);
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a)); v = a;
    ray_t* keep;
    ray_t* m = save_and_materialize(v, ray_test_tmp(ENCCOL_TMP "/u"), &keep);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    ray_enccol_t* e = ray_enccol_of(keep);
    TEST_ASSERT_EQ_I(e->hdr.col_flags, RAY_ENC_FLAG_UNIQUE);
    TEST_ASSERT_EQ_I(e->hdr.index_kind, 0);
    TEST_ASSERT_TRUE(ray_index_has(m));
    TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_NONE);
    TEST_ASSERT_TRUE(ray_index_payload(m->index)->markers & RAY_MARK_UNIQUE);
    ray_release(keep); ray_release(v);
    PASS();
}

/* The matrix refuses a pair: on a file (GUID claiming UNIQUE) at load; on
 * the attach itself the root is consumed exactly once (ASan would report a
 * double release or a leak). */
static test_result_t test_attach_refused(void) {
    ray_t* v = make_col(RAY_GUID, 50, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/gu"), 2), RAY_OK);
    uint8_t f = RAY_ENC_FLAG_UNIQUE;
    patch(ray_test_tmp(ENCCOL_TMP "/gu"), 32 + 7, &f, 1);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/gu"));
    TEST_ASSERT_TRUE(c && RAY_IS_ERR(c));
    TEST_ASSERT_STR_EQ(ray_err_code(c), "corrupt");
    ray_error_free(c);
    ray_t* root = ray_index_new_root(RAY_IDX_ZONE, RAY_GUID, v->len);
    ray_t* r = ray_index_attach_persisted(&v, root);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    TEST_ASSERT_FALSE(ray_index_has(v));
    ray_release(v);
    PASS();
}


/* A gen-2 load carries the same attachment a gen-0 load of the same vector
 * with the same index does: ZONE and CHUNK_ZONE (integer, float, temporal;
 * several chunks, nulls, an all-null chunk) plus UNIQUE on top.  The gen-0
 * side is mmap'd: ray_col_load drops the index. */
static test_result_t test_synth_matches_gen0(void) {
    static const int8_t types[] = { RAY_I64, RAY_F64, RAY_DATE, RAY_TIMESTAMP };
    for (size_t t = 0; t < 4; t++)
    for (int k = 0; k < 2; k++) {
        int64_t n = 2 * 65536 + 5;
        ray_t* v = make_col(types[t], n, 4);
        null_rows(v, 65536, 2 * 65536);
        ray_t* z = k ? ray_index_attach_chunk_zone(&v, 16) : ray_index_attach_zone(&v);
        TEST_ASSERT_TRUE(z && !RAY_IS_ERR(z)); v = z;
        ray_index_payload(v->index)->markers |= RAY_MARK_UNIQUE;
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/g0"), 0), RAY_OK);
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/g2"), 2), RAY_OK);
        ray_t* a = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/g0"));
        ray_t* b = ray_col_load(ray_test_tmp(ENCCOL_TMP "/g2"));
        TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a) && b && !RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(ray_index_kind(b), ray_index_kind(a));
        TEST_ASSERT_EQ_I(ray_index_kind(b), k ? RAY_IDX_CHUNK_ZONE : RAY_IDX_ZONE);
        ray_index_t* ia = ray_index_payload(a->index);
        ray_index_t* ib = ray_index_payload(b->index);
        TEST_ASSERT_EQ_I(ib->markers & RAY_MARK_UNIQUE, ia->markers & RAY_MARK_UNIQUE);   /* MMAP is residency */
        TEST_ASSERT_EQ_I(ib->built_for_len, ia->built_for_len);
        if (k) {
            /* gen-0 stores the builder's aggs for every integer type; gen 2 only where the directory has sums */
            bool sm = ray_encw_type_summable(types[t]);
            uint32_t nc = ia->u.chunk_zone.n_chunks;
            TEST_ASSERT_EQ_I(ib->u.chunk_zone.n_chunks, nc);
            TEST_ASSERT_TRUE(memcmp(ray_data(ib->u.chunk_zone.mins), ray_data(ia->u.chunk_zone.mins), 8 * nc) == 0);
            TEST_ASSERT_TRUE(memcmp(ray_data(ib->u.chunk_zone.maxs), ray_data(ia->u.chunk_zone.maxs), 8 * nc) == 0);
            TEST_ASSERT_TRUE(memcmp(ray_data(ib->u.chunk_zone.null_bits), ray_data(ia->u.chunk_zone.null_bits), (nc + 7) / 8) == 0);
            if (sm) TEST_ASSERT_TRUE(memcmp(ray_data(ib->u.chunk_zone.aggs), ray_data(ia->u.chunk_zone.aggs), 8 * 3 * nc) == 0);
            else    TEST_ASSERT_TRUE(ib->u.chunk_zone.aggs == NULL);
        } else if (types[t] == RAY_F64) {
            TEST_ASSERT_TRUE(memcmp(&ib->u.zone.min_f, &ia->u.zone.min_f, 8) == 0);
            TEST_ASSERT_TRUE(memcmp(&ib->u.zone.max_f, &ia->u.zone.max_f, 8) == 0);
        } else {
            TEST_ASSERT_EQ_I(ib->u.zone.min_i, ia->u.zone.min_i);
            TEST_ASSERT_EQ_I(ib->u.zone.max_i, ia->u.zone.max_i);
        }
        TEST_ASSERT_EQ_I(ib->u.zone.n_nulls * !k, ia->u.zone.n_nulls * !k);
        ray_release(a); ray_release(b); ray_release(v);
    }
    PASS();
}

/* The materialize-time refusal (open-time validation catches every file
 * the matrix rejects, so a hook feeds the predicate a mismatched root):
 * the attach releases the root once, the attempt is a cached corrupt and
 * a later materialize returns the same error with nothing leaked. */
static test_result_t test_materialize_refusal_cached(void) {
    ray_t* v = make_col(RAY_I64, 70000, 4);
    ray_t* z = ray_index_attach_zone(&v);
    TEST_ASSERT_TRUE(z && !RAY_IS_ERR(z)); v = z;
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(ENCCOL_TMP "/rf"), 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/rf"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_test_refuse_attach(1);
    ray_t* r = ray_enccol_materialize(c);
    ray_enccol_test_refuse_attach(0);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    ray_t* r2 = ray_enccol_materialize(c);                  /* cached: the hook is off and it still fails */
    TEST_ASSERT_TRUE(r2 && RAY_IS_ERR(r2));
    TEST_ASSERT_STR_EQ(ray_err_code(r2), "corrupt");
    ray_error_free(r2);
    ray_release(c);
    PASS();
}

/* Write a table as a legacy splayed directory whose columns are generation 2. */
static ray_err_t write_splayed_gen2(const char* dir, ray_t* tbl) {
    if (ray_test_mkdir_p(dir) != 0) return RAY_ERR_IO;
    int64_t nc = ray_table_ncols(tbl);
    ray_t* names = ray_vec_new(RAY_STR, nc);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));
        char p[512]; snprintf(p, sizeof p, "%s/%.*s", dir, (int)ray_str_len(nm), ray_str_ptr(nm));
        ray_err_t e = ray_col_save_gen(ray_table_get_col_idx(tbl, c), p, 2);
        if (e != RAY_OK) { ray_release(names); return e; }
        names = ray_str_vec_append(names, ray_str_ptr(nm), ray_str_len(nm));
    }
    char p[512]; snprintf(p, sizeof p, "%s/.d", dir);
    ray_err_t e = ray_col_save(names, p);
    ray_release(names);
    return e;
}

static ray_t* tbl2(int64_t n) {
    ray_t* t = ray_table_new(2);
    ray_t* a = make_col(RAY_I64, n, 0);
    ray_t* b = make_col(RAY_F64, n, 4);
    t = ray_table_add_col(t, ray_sym_intern("a", 1), a);
    t = ray_table_add_col(t, ray_sym_intern("b", 1), b);
    ray_release(a); ray_release(b);
    return t;
}

/* Tables are built directly from mmap'd generation-2 columns, then loaded
 * through the splayed loader, whose columns equal the generation-0 table. */
static test_result_t test_table_core_enccol(void) {
    ray_t* t = tbl2(70000);
    TEST_ASSERT_EQ_I(write_splayed_gen2(ray_test_tmp(ENCCOL_TMP "/t"), t), RAY_OK);   /* t: the generation-0 reference */
    ray_enc_counters_reset();
    ray_t* ca = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/t/a"));
    ray_t* cb = ray_col_mmap(ray_test_tmp(ENCCOL_TMP "/t/b"));
    TEST_ASSERT_FMT(ca && !RAY_IS_ERR(ca) && cb && !RAY_IS_ERR(cb), "mmap");
    TEST_ASSERT_EQ_I(ca->type, RAY_ENCCOL);
    TEST_ASSERT_EQ_I(ray_table_col_nrows(ca), 70000);
    ray_t* s = ray_table_new(2);
    s = ray_table_add_col(s, ray_sym_intern("a", 1), ca);
    s = ray_table_add_col(s, ray_sym_intern("b", 1), cb);
    TEST_ASSERT_FMT(s && !RAY_IS_ERR(s), "add_col: %s", s ? ray_err_code(s) : "null");
    ray_release(ca); ray_release(cb);
    TEST_ASSERT_EQ_I(ray_table_nrows(s), 70000);
    TEST_ASSERT_NULL(ray_table_validate_rectangular(s, "test"));
    TEST_ASSERT_TRUE(ray_table_get_col_enc_idx(s, 1) != NULL);
    /* a shorter plain column makes the table ragged */
    ray_t* shortc = make_col(RAY_I32, 10, 0);
    ray_retain(s);
    ray_t* r = ray_table_add_col(s, ray_sym_intern("c", 1), shortc);
    ray_t* e = ray_table_validate_rectangular(r, "test");
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    ray_error_free(e);
    ray_release(r); ray_release(shortc);
    /* copy-on-write shares the encoded object */
    ray_t* cp = ray_alloc_copy(s);
    TEST_ASSERT_EQ_I(ray_table_nrows(cp), 70000);
    for (int64_t c = 0; c < 2; c++) {
        ray_t* enc = ray_table_get_col_enc_idx(s, c);
        TEST_ASSERT_TRUE(enc != NULL);
        TEST_ASSERT_EQ_PTR(ray_table_get_col_enc_idx(cp, c), enc);
        TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(enc)->rc), 1);   /* one header: shared through the cols list, not duplicated */
    }
    ray_release(cp);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_I(ray_table_get_col_idx(s, 1)->type, RAY_F64);   /* legacy getter: plain */
    ray_release(s);
    /* the splayed loader returns the same values as the generation-0 table */
    ray_t* ref = t;
    ray_t* ld = ray_read_splayed(ray_test_tmp(ENCCOL_TMP "/t"), NULL);
    TEST_ASSERT_FMT(ld && !RAY_IS_ERR(ld), "read_splayed: %s", ld ? ray_err_code(ld) : "null");
    TEST_ASSERT_EQ_I(ray_table_nrows(ld), 70000);
    for (int64_t c = 0; c < 2; c++) {
        TEST_ASSERT_TRUE(ray_table_get_col_enc_idx(ld, c) != NULL);
        ray_t* want = ray_table_get_col_idx(ref, c);
        ray_t* got  = ray_table_col_idx(ld, c);
        TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got));
        TEST_ASSERT_EQ_I(got->type, want->type);
        TEST_ASSERT_EQ_I(got->len, want->len);
        TEST_ASSERT_TRUE(memcmp(ray_data(got), ray_data(want), (size_t)want->len * 8) == 0);
    }
    ray_release(ld); ray_release(ref);
    PASS();
}

/* The splayed loader reads logical rows through ray_table_col_nrows: a torn
 * encoded column is a corrupt table. */
static test_result_t test_table_torn_column(void) {
    ray_t* t = tbl2(5000);
    TEST_ASSERT_EQ_I(write_splayed_gen2(ray_test_tmp(ENCCOL_TMP "/torn"), t), RAY_OK);
    ray_release(t);
    ray_t* b = make_col(RAY_F64, 4999, 0);
    TEST_ASSERT_EQ_I(ray_col_save_gen(b, ray_test_tmp(ENCCOL_TMP "/torn/b"), 2), RAY_OK);
    ray_release(b);
    ray_t* s = ray_read_splayed(ray_test_tmp(ENCCOL_TMP "/torn"), NULL);
    TEST_ASSERT_TRUE(s && RAY_IS_ERR(s));
    TEST_ASSERT_STR_EQ(ray_err_code(s), "corrupt");
    TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), "4999 rows, expected 5000"));
    ray_error_free(s);
    PASS();
}

/* Row counts of the non-vector column shapes, without decoding. */
static test_result_t test_table_col_nrows_shapes(void) {
    ray_t* a = ray_alloc(0); a->type = -RAY_I64; a->i64 = 7;
    TEST_ASSERT_EQ_I(ray_table_col_nrows(a), 0);       /* an atom has no rows: its len */
    ray_release(a);
    ray_t* keys = ray_vec_new(RAY_I64, 2); keys->len = 2;
    ray_t* counts = ray_vec_new(RAY_I64, 2); counts->len = 2;
    ((int64_t*)ray_data(counts))[0] = 30; ((int64_t*)ray_data(counts))[1] = 12;
    ray_t* mc = ray_alloc(2 * sizeof(ray_t*));
    mc->type = RAY_MAPCOMMON; mc->len = 2;
    ((ray_t**)ray_data(mc))[0] = keys; ((ray_t**)ray_data(mc))[1] = counts;
    TEST_ASSERT_EQ_I(ray_table_col_nrows(mc), 42);
    ray_release(mc);
    PASS();
}

/* Three partitions of t(a I64, b F64); partition p is generation 2 when
 * gen2_mask has bit p set, generation 0 otherwise. */
static ray_err_t write_parted(const char* root, int gen2_mask, int64_t rows) {
    for (int p = 0; p < 3; p++) {
        char dir[512]; snprintf(dir, sizeof dir, "%s/2024.01.0%d/t", root, p + 1);
        ray_t* t = tbl2(rows);
        ray_err_t e;
        if (gen2_mask & (1 << p)) e = write_splayed_gen2(dir, t);
        else e = ray_splay_save(t, dir, NULL);
        ray_release(t);
        if (e != RAY_OK) return e;
    }
    return RAY_OK;
}

static test_result_t test_parted_mixed_load(void) {
    TEST_ASSERT_EQ_I(write_parted(ray_test_tmp(ENCCOL_TMP "/pdb"), 0x5, 4000), RAY_OK);   /* parts 1 and 3 encoded */
    ray_enc_counters_reset();
    ray_t* t = ray_read_parted(ray_test_tmp(ENCCOL_TMP "/pdb"), "t");
    TEST_ASSERT_FMT(t && !RAY_IS_ERR(t), "parted load: %s", t ? ray_err_code(t) : "null");
    TEST_ASSERT_EQ_I(ray_table_nrows(t), 12000);
    ray_t* slot = ray_table_get_col_enc(t, ray_sym_intern("a", 1));
    TEST_ASSERT_TRUE(slot && ray_enccol_is_parted(slot));
    TEST_ASSERT_EQ_I(ray_table_col_nrows(slot), 12000);          /* sum of segment lens, no decode */
    ray_col_meta_t m;
    TEST_ASSERT_TRUE(ray_table_col_meta(t, ray_sym_intern("a", 1), &m));
    TEST_ASSERT_EQ_I(m.type, RAY_PARTED_BASE + RAY_I64);
    TEST_ASSERT_EQ_I(m.n_segs, 3);
    TEST_ASSERT_TRUE(m.enc != NULL);
    ray_t* enc = m.enc;
    TEST_ASSERT_EQ_I(ray_enccol_seg(enc, 0)->type, RAY_ENCCOL);
    TEST_ASSERT_EQ_I(ray_enccol_seg(enc, 1)->type, RAY_I64);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_t* s2 = ray_enccol_seg_col(enc, 2);                 /* one segment only */
    TEST_ASSERT_TRUE(s2 && !RAY_IS_ERR(s2) && s2->type == RAY_I64);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    ray_t* w = ray_table_col(t, ray_sym_intern("a", 1));     /* plain wrapper */
    TEST_ASSERT_TRUE(w && !RAY_IS_ERR(w));
    TEST_ASSERT_EQ_I(w->type, RAY_PARTED_BASE + RAY_I64);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 2);                    /* segment 0 decoded, 2 cached */
    ray_t** segs = (ray_t**)ray_data(w);
    TEST_ASSERT_EQ_PTR(segs[2], s2);
    ray_retain(w);
    ray_release(t);                                          /* retained wrapper outlives the table */
    /* segment 0 decodes to what the same directory loads as on its own */
    ray_t* solo = ray_read_splayed(ray_test_tmp(ENCCOL_TMP "/pdb/2024.01.01/t"), NULL);
    TEST_ASSERT_TRUE(solo && !RAY_IS_ERR(solo));
    ray_t* sa = ray_table_col(solo, ray_sym_intern("a", 1));
    TEST_ASSERT_TRUE(sa && !RAY_IS_ERR(sa) && sa->len == 4000 && segs[0]->len == 4000);
    TEST_ASSERT_TRUE(memcmp(ray_data(sa), ray_data(segs[0]), 4000 * sizeof(int64_t)) == 0);
    ray_release(solo);
    ray_release(w);
    PASS();
}

static test_result_t test_parted_failure_lowest_segment(void) {
    TEST_ASSERT_EQ_I(write_parted(ray_test_tmp(ENCCOL_TMP "/pf"), 0x7, 4000), RAY_OK);
    uint8_t one = 1;
    patch(ray_test_tmp(ENCCOL_TMP "/pf/2024.01.01/t/a"), 160 + 6, &one, 1);    /* segment 0 corrupt */
    patch(ray_test_tmp(ENCCOL_TMP "/pf/2024.01.03/t/a"), 160 + 6, &one, 1);    /* segment 2 corrupt */
    ray_t* t = ray_read_parted(ray_test_tmp(ENCCOL_TMP "/pf"), "t");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_col_meta_t m; ray_table_col_meta(t, ray_sym_intern("a", 1), &m);
    /* a transient failure first, then the deterministic one */
    ray_enccol_test_fail_alloc(1);
    ray_t* r = ray_table_col(t, ray_sym_intern("a", 1));
    ray_enccol_test_fail_alloc(0);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    ray_error_free(r);
    for (int i = 0; i < 2; i++) {
        r = ray_table_col(t, ray_sym_intern("a", 1));
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), "2024.01.01") != NULL);
        ray_error_free(r);
    }
    /* the healthy segment keeps its own cached vector */
    ray_t* s1 = ray_enccol_seg_col(m.enc, 1);
    TEST_ASSERT_TRUE(s1 && !RAY_IS_ERR(s1));
    TEST_ASSERT_EQ_PTR(ray_enccol_of(ray_enccol_seg(m.enc, 1))->decoded, s1);
    ray_release(t);
    PASS();
}

/* Both failure orders, with the parted materialization taken on the
 * coordinator (mode 1) and as per-segment tasks (mode 2): every failure
 * happens inside a segment, and each segment keeps its own once-state. */
static test_result_t fanin_run(int mode) {
    char po[64], pq[64];
    snprintf(po, sizeof po, ray_test_tmp(ENCCOL_TMP "/po%d"), mode);
    snprintf(pq, sizeof pq, ray_test_tmp(ENCCOL_TMP "/pq%d"), mode);
    char f[600];
    ray_enccol_test_parted_mode(mode);
    TEST_ASSERT_EQ_I(write_parted(po, 0x7, 4000), RAY_OK);
    uint8_t one = 1;
    /* order 1: segment 0 corrupt, segment 2 OOM -> corrupt (lowest), cached */
    snprintf(f, sizeof f, "%s/2024.01.01/t/a", po);
    patch(f, 160 + 6, &one, 1);
    ray_t* t = ray_read_parted(po, "t");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_col_meta_t m; ray_table_col_meta(t, ray_sym_intern("a", 1), &m);
    ray_enccol_test_fail_path("2024.01.03");
    ray_t* r = ray_table_col(t, ray_sym_intern("a", 1));
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    ray_enccol_test_fail_path(NULL);
    TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(ray_enccol_seg(m.enc, 0))->once.state), RAY_ENC_ONCE_FAILED);
    TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(ray_enccol_seg(m.enc, 2))->once.state), RAY_ENC_ONCE_IDLE);
    r = ray_table_col(t, ray_sym_intern("a", 1));
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r) && strcmp(ray_err_code(r), "corrupt") == 0);
    ray_error_free(r);
    ray_release(t);
    /* order 2: segment 0 OOM, segment 2 corrupt -> oom first (segment 2's
     * corrupt is cached in its own state), then the retry meets corrupt */
    TEST_ASSERT_EQ_I(write_parted(pq, 0x7, 4000), RAY_OK);
    snprintf(f, sizeof f, "%s/2024.01.03/t/a", pq);
    patch(f, 160 + 6, &one, 1);
    t = ray_read_parted(pq, "t");
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_table_col_meta(t, ray_sym_intern("a", 1), &m);
    char sub[80]; snprintf(sub, sizeof sub, "pq%d/2024.01.01", mode);
    ray_enccol_test_fail_path(sub);
    r = ray_table_col(t, ray_sym_intern("a", 1));
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "oom");
    ray_error_free(r);
    ray_enccol_test_fail_path(NULL);
    TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(ray_enccol_seg(m.enc, 0))->once.state), RAY_ENC_ONCE_IDLE);
    TEST_ASSERT_EQ_U(atomic_load(&ray_enccol_of(ray_enccol_seg(m.enc, 2))->once.state), RAY_ENC_ONCE_FAILED);
    r = ray_table_col(t, ray_sym_intern("a", 1));
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    TEST_ASSERT_TRUE(strstr(ray_error_msg(), "2024.01.03") != NULL);
    ray_error_free(r);
    ray_release(t);
    ray_enccol_test_parted_mode(0);
    PASS();
}
static test_result_t test_parted_fanin_orders(void) {
    test_result_t r = fanin_run(1);
    if (r.status != TEST_PASS) { ray_enccol_test_parted_mode(0); return r; }
    return fanin_run(2);
}

/* A parted load of generation-2 segments materializes to exactly the wrapper
 * a generation-0 load of the same data produces, ragged partition included. */
static test_result_t test_parted_equals_gen0(void) {
    TEST_ASSERT_EQ_I(write_parted(ray_test_tmp(ENCCOL_TMP "/pg2"), 0x5, 3000), RAY_OK);
    TEST_ASSERT_EQ_I(write_parted(ray_test_tmp(ENCCOL_TMP "/pg0"), 0x0, 3000), RAY_OK);
    /* the same rows in both (rng is consumed per write): rewrite pg0 from pg2's columns */
    for (int p = 0; p < 3; p++) {
        char d2[512], d0[512];
        snprintf(d2, sizeof d2, ray_test_tmp(ENCCOL_TMP "/pg2/2024.01.0%d/t"), p + 1);
        snprintf(d0, sizeof d0, ray_test_tmp(ENCCOL_TMP "/pg0/2024.01.0%d/t"), p + 1);
        ray_t* src = ray_read_splayed(d2, NULL);
        TEST_ASSERT_TRUE(src && !RAY_IS_ERR(src));
        int64_t nc = ray_table_ncols(src);
        if (p == 2) nc = 1;                      /* ragged: partition 3 has no column b */
        ray_t* flat = ray_table_new(nc);
        for (int64_t c = 0; c < nc; c++)
            flat = ray_table_add_col(flat, ray_table_col_name(src, c), ray_table_col_idx(src, c));
        TEST_ASSERT_EQ_I(ray_splay_save(flat, d0, NULL), RAY_OK);
        ray_release(flat); ray_release(src);
    }
    remove(ray_test_tmp(ENCCOL_TMP "/pg2/2024.01.03/t/b"));
    {
        ray_t* names = ray_vec_new(RAY_STR, 1); names = ray_str_vec_append(names, "a", 1);
        TEST_ASSERT_EQ_I(ray_col_save(names, ray_test_tmp(ENCCOL_TMP "/pg2/2024.01.03/t/.d")), RAY_OK);
        ray_release(names);
    }
    ray_t* t2 = ray_read_parted(ray_test_tmp(ENCCOL_TMP "/pg2"), "t");
    ray_t* t0 = ray_read_parted(ray_test_tmp(ENCCOL_TMP "/pg0"), "t");
    TEST_ASSERT_TRUE(t2 && !RAY_IS_ERR(t2) && t0 && !RAY_IS_ERR(t0));
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(ray_table_ncols(t2), ray_table_ncols(t0));
    for (const char* nm = "a"; nm; nm = nm[0] == 'a' ? "b" : NULL) {
        int64_t id = ray_sym_intern(nm, 1);
        ray_t* w2 = ray_table_col(t2, id);
        ray_t* w0 = ray_table_col(t0, id);
        TEST_ASSERT_TRUE(w2 && !RAY_IS_ERR(w2) && w0 && !RAY_IS_ERR(w0));
        TEST_ASSERT_EQ_I(w2->type, w0->type);
        TEST_ASSERT_EQ_I(w2->attrs, w0->attrs);
        TEST_ASSERT_EQ_I(w2->len, w0->len);
        ray_t** s2 = (ray_t**)ray_data(w2); ray_t** s0 = (ray_t**)ray_data(w0);
        for (int64_t i = 0; i < w2->len; i++) {
            TEST_ASSERT_FMT((s2[i] == NULL) == (s0[i] == NULL), "col %s seg %lld: %p vs %p", nm, (long long)i, (void*)s2[i], (void*)s0[i]);
            if (!s2[i]) continue;
            TEST_ASSERT_EQ_I(s2[i]->type, s0[i]->type);
            TEST_ASSERT_EQ_I(s2[i]->len, s0[i]->len);
            TEST_ASSERT_TRUE(memcmp(ray_data(s2[i]), ray_data(s0[i]), (size_t)s2[i]->len * 8) == 0);
        }
    }
    ray_release(t2); ray_release(t0);
    PASS();
}

const test_entry_t enccol_entries[] = {
    { "enccol/open_returns_enccol", test_open_returns_enccol, enccol_setup, enccol_teardown },
    { "enccol/load_rejects",        test_load_rejects,        enccol_setup, enccol_teardown },
    { "enccol/sym_requires_domain", test_sym_requires_domain, enccol_setup, enccol_teardown },
    { "enccol/append_index_gen2",   test_append_index_refuses_gen2, enccol_setup, enccol_teardown },
    { "enccol/load_gen2_plain",      test_load_gen2_plain,       enccol_setup, enccol_teardown },
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
    { "enccol/fast_kernel_equal",      test_fast_kernel_equal,      enccol_setup, enccol_teardown },
    { "enccol/desc_validated_once",    test_desc_validated_once,    enccol_setup, enccol_teardown },
    { "enccol/desc_first_touch_concurrent", test_desc_first_touch_concurrent, enccol_setup, enccol_teardown },
    { "enccol/scratch_outlives_source", test_scratch_outlives_source, enccol_setup, enccol_teardown },
    { "enccol/decode_trailing_bytes",  test_decode_trailing_bytes,  enccol_setup, enccol_teardown },
    { "enccol/dict_null_dictionary",   test_dict_null_dictionary,   enccol_setup, enccol_teardown },
    { "enccol/vec_range_checks",       test_vec_range_checks,       enccol_setup, enccol_teardown },
    { "enccol/descriptor_null_sum",    test_descriptor_null_sum,    enccol_setup, enccol_teardown },
    { "enccol/str_stats_corrupt",      test_str_stats_corrupt,      enccol_setup, enccol_teardown },
    { "enccol/sym_width_and_bound",    test_sym_width_and_bound,    enccol_setup, enccol_teardown },
    { "enccol/decode_mutation_sweep",  test_decode_mutation_sweep,  enccol_setup, enccol_teardown },
    { "enccol/materialize_equivalence",        test_materialize_equivalence,        enccol_setup, enccol_teardown },
    { "enccol/materialize_lengths",            test_materialize_lengths,            enccol_setup, enccol_teardown },
    { "enccol/materialize_float_bits",         test_materialize_float_bits,         enccol_setup, enccol_teardown },
    { "enccol/decoded_peek",                   test_decoded_peek,                   enccol_setup, enccol_teardown },
    { "enccol/cols_inplace_encoded",           test_cols_inplace_encoded,           enccol_setup, enccol_teardown },
    { "enccol/materialize_counts_once",        test_materialize_counts_once,        enccol_setup, enccol_teardown },
    { "enccol/materialize_lowest_failure",     test_materialize_lowest_failure_cached, enccol_setup, enccol_teardown },
    { "enccol/oom_then_retry",                 test_oom_then_retry,                 enccol_setup, enccol_teardown },
    { "enccol/col_load_materializes",          test_col_load_materializes,          enccol_setup, enccol_teardown },
    { "enccol/cancel_retried",                 test_cancel_retried,                 enccol_setup, enccol_teardown },
    { "enccol/cancel_mid_decode",              test_cancel_mid_decode,              enccol_setup, enccol_teardown },
    { "enccol/cancel_waiter",                  test_cancel_waiter,                  enccol_setup, enccol_teardown },
    { "enccol/load_dom_refcount",              test_load_dom_refcount,              enccol_setup, enccol_teardown },
    { "enccol/synth_zone",            test_synth_zone,            enccol_setup, enccol_teardown },
    { "enccol/synth_chunk_zone",      test_synth_chunk_zone,      enccol_setup, enccol_teardown },
    { "enccol/unique_flag",           test_unique_flag,           enccol_setup, enccol_teardown },
    { "enccol/attach_refused",        test_attach_refused,        enccol_setup, enccol_teardown },
    { "enccol/materialize_refusal",   test_materialize_refusal_cached, enccol_setup, enccol_teardown },
    { "enccol/synth_matches_gen0",    test_synth_matches_gen0,    enccol_setup, enccol_teardown },
    { "enccol/table_core",            test_table_core_enccol,     enccol_setup, enccol_teardown },
    { "enccol/table_torn_column",     test_table_torn_column,     enccol_setup, enccol_teardown },
    { "enccol/table_col_nrows_shapes", test_table_col_nrows_shapes, enccol_setup, enccol_teardown },
    { "enccol/parted_fanin_orders",           test_parted_fanin_orders,           enccol_setup, enccol_teardown },
    { "enccol/parted_mixed_load",             test_parted_mixed_load,             enccol_setup, enccol_teardown },
    { "enccol/parted_failure_lowest_segment", test_parted_failure_lowest_segment, enccol_setup, enccol_teardown },
    { "enccol/parted_equals_gen0",            test_parted_equals_gen0,            enccol_setup, enccol_teardown },
    { NULL, NULL, NULL, NULL },
};
