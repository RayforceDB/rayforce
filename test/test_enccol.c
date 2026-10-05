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
 * #if !RAY_ENC_LE branch is compiled out.  What is testable portably: the
 * gate predicate tracks RAY_ENC_LE, and the discriminator recognises "XENC"
 * bytewise, so on any host a generation-2 file reaches ray_enccol_open
 * instead of being rejected as corrupt before the gate. */
static test_result_t test_be_gate_logic(void) {
    TEST_ASSERT_EQ_I(ray_enc_gen2_supported(), RAY_ENC_LE);
    uint8_t h[32] = {0};
    memcpy(h, "XENC", 4); h[17] = RAY_ENC_GEN;
    TEST_ASSERT_EQ_I(ray_col_gen_of(h), 2);
    memcpy(h, "CNEX", 4);                       /* byte-reversed marker is not XENC */
    TEST_ASSERT_EQ_I(ray_col_gen_of(h), RAY_ENC_GEN_CORRUPT);
    PASS();
}

const test_entry_t enccol_entries[] = {
    { "enccol/open_returns_enccol", test_open_returns_enccol, enccol_setup, enccol_teardown },
    { "enccol/load_rejects",        test_load_rejects,        enccol_setup, enccol_teardown },
    { "enccol/sym_requires_domain", test_sym_requires_domain, enccol_setup, enccol_teardown },
    { "enccol/append_index_gen2",   test_append_index_refuses_gen2, enccol_setup, enccol_teardown },
    { "enccol/load_gen2_nyi",       test_load_gen2_nyi,       enccol_setup, enccol_teardown },
    { "enccol/no_mapping_leak",     test_no_mapping_leak,     enccol_setup, enccol_teardown },
    { "enccol/be_gate_logic",       test_be_gate_logic,       enccol_setup, enccol_teardown },
    { NULL, NULL, NULL, NULL },
};
