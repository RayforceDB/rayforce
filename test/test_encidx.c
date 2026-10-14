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
#include "ops/idxop.h"
#include "table/sym.h"
#include "core/runtime.h"
#include "store/col.h"
#include "store/encfmt.h"
#include "store/encidx.h"
#include "ops/hash.h"
#include "core/pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDX_TMP "/tmp/rayforce_encidx"

static void ix_setup(void)    { ray_heap_init(); (void)ray_sym_init(); (void)ray_test_rm_rf(ray_test_tmp(IDX_TMP)); (void)ray_test_mkdir_p(ray_test_tmp(IDX_TMP)); }
static void ix_teardown(void) { ray_test_hooks_off(); (void)ray_test_rm_rf(ray_test_tmp(IDX_TMP)); ray_sym_destroy(); ray_heap_destroy(); }

static uint8_t* ix_slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)sz + 1);
    *n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    return b;
}

/* The region of `v`'s attached index, written to a file and read back. */
static uint8_t* ix_region(ray_t* v, const char* name, uint32_t nlen, size_t* n, ray_t** err) {
    FILE* f = fopen(ray_test_tmp(IDX_TMP "/r"), "wb");
    ray_enc_idx_src_t s = { v->index, v->type, v->attrs, v->len, name, nlen };
    uint64_t bytes = 0;
    *err = ray_enc_index_write(f, &s, &bytes);
    fclose(f);
    if (*err) return NULL;
    uint8_t* b = ix_slurp(ray_test_tmp(IDX_TMP "/r"), n);
    if (b && *n != bytes) { free(b); return NULL; }
    return b;
}

static int64_t rd64(const uint8_t* p) { int64_t x; memcpy(&x, p, 8); return x; }
static uint64_t ix_p32(uint64_t x) { return (x + 31) & ~(uint64_t)31; }

static ray_t* i64_vec(const int64_t* d, int64_t n) { return ray_vec_from_raw(RAY_I64, d, n); }
static ray_t* with_index(ray_t* v, ray_t* (*attach)(ray_t**)) {
    ray_t* r = attach(&v);
    return (r && !RAY_IS_ERR(r)) ? r : NULL;
}

/* HASH on I64: root, kind area, normalized child placement, child bytes. */
static test_result_t test_write_hash_layout(void) {
    int64_t d[6] = { 5, 7, 5, 9, 7, 5 };
    ray_t* v = with_index(i64_vec(d, 6), ray_index_attach_hash);
    TEST_ASSERT_NOT_NULL(v);
    size_t n; ray_t* err;
    uint8_t* b = ix_region(v, NULL, 0, &n, &err);
    TEST_ASSERT_NULL(err);
    ray_index_t* ix = ray_index_payload(v->index);
    TEST_ASSERT_EQ_I((int8_t)b[0], RAY_I64);
    for (int i = 1; i < 8; i++) TEST_ASSERT_EQ_I(b[i], 0);
    TEST_ASSERT_EQ_I(rd64(b + 8), 6);
    ray_enc_idx_hash_t h; memcpy(&h, b + 16, 64);
    uint64_t cap = ix->u.hash.mask + 1;
    TEST_ASSERT_EQ_I(h.table, 96);
    TEST_ASSERT_EQ_I(h.gkeys, (int64_t)ix_p32(96 + cap * 8));
    TEST_ASSERT_EQ_I(h.offs, (int64_t)ix_p32((uint64_t)h.gkeys + 3 * 8));
    TEST_ASSERT_EQ_I(h.rows, (int64_t)ix_p32((uint64_t)h.offs + 4 * 8));
    TEST_ASSERT_EQ_U(h.mask, ix->u.hash.mask);
    TEST_ASSERT_EQ_I(h.n_keys, 6);
    TEST_ASSERT_EQ_I(h.n_groups, 3);
    TEST_ASSERT_EQ_I(h.order_name, -1);
    TEST_ASSERT_EQ_U(n, ix_p32((uint64_t)h.rows + 6 * 8));
    TEST_ASSERT_TRUE(memcmp(b + h.table, ray_data(ix->u.hash.table), cap * 8) == 0);
    TEST_ASSERT_TRUE(memcmp(b + h.gkeys, ray_data(ix->u.hash.gkeys), 3 * 8) == 0);
    TEST_ASSERT_TRUE(memcmp(b + h.rows, ray_data(ix->u.hash.rows), 6 * 8) == 0);
    for (size_t i = 80; i < 96; i++) TEST_ASSERT_EQ_I(b[i], 0);
    free(b); ray_release(v);
    PASS();
}

/* The name block: u32 len, u32 reserved, bytes, zero pad to 32, last. */
static test_result_t test_write_name_block(void) {
    int64_t d[4] = { 1, 1, 2, 2 };
    ray_t* v = with_index(i64_vec(d, 4), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(v);
    size_t n; ray_t* err;
    uint8_t* b = ix_region(v, "ts", 2, &n, &err);
    TEST_ASSERT_NULL(err);
    ray_enc_idx_part_t p; memcpy(&p, b + 16, 40);
    TEST_ASSERT_EQ_I(p.n_parts, 2);
    TEST_ASSERT_EQ_I(p.keys, 96);
    TEST_ASSERT_EQ_I(p.starts, (int64_t)ix_p32(96 + 2 * 8));
    TEST_ASSERT_EQ_I(p.order_name, (int64_t)ix_p32((uint64_t)p.lens + 2 * 8));
    uint32_t nl; memcpy(&nl, b + p.order_name, 4);
    TEST_ASSERT_EQ_I(nl, 2);
    TEST_ASSERT_TRUE(memcmp(b + p.order_name + 8, "ts", 2) == 0);
    TEST_ASSERT_EQ_U(n, ix_p32((uint64_t)p.order_name + 10));
    for (size_t i = (size_t)p.order_name + 10; i < n; i++) TEST_ASSERT_EQ_I(b[i], 0);
    free(b);
    /* a name over the limit is written as -1 */
    char longn[300]; memset(longn, 'x', sizeof longn);
    b = ix_region(v, longn, 300, &n, &err);
    TEST_ASSERT_NULL(err);
    memcpy(&p, b + 16, 40);
    TEST_ASSERT_EQ_I(p.order_name, -1);
    free(b); ray_release(v);
    PASS();
}

/* SORT and BLOOM layouts; unused kind-area tails are zero. */
static test_result_t test_write_sort_bloom(void) {
    int32_t d[5] = { 9, 3, 7, 1, 3 };
    ray_t* s = with_index(ray_vec_from_raw(RAY_I32, d, 5), ray_index_attach_sort);
    ray_t* bl = with_index(ray_vec_from_raw(RAY_I32, d, 5), ray_index_attach_bloom);
    TEST_ASSERT_TRUE(s && bl);
    size_t n; ray_t* err;
    uint8_t* b = ix_region(s, NULL, 0, &n, &err);
    TEST_ASSERT_NULL(err);
    TEST_ASSERT_EQ_I(rd64(b + 16), 96);
    for (int i = 24; i < 80; i++) TEST_ASSERT_EQ_I(b[i], 0);
    TEST_ASSERT_EQ_U(n, ix_p32(96 + 5 * 8));
    free(b);
    b = ix_region(bl, NULL, 0, &n, &err);
    TEST_ASSERT_NULL(err);
    ray_enc_idx_bloom_t bb; memcpy(&bb, b + 16, 32);
    ray_index_t* ix = ray_index_payload(bl->index);
    TEST_ASSERT_EQ_I(bb.bits, 96);
    TEST_ASSERT_EQ_U(bb.m_mask, ix->u.bloom.m_mask);
    TEST_ASSERT_EQ_I(bb.k, 3);
    TEST_ASSERT_EQ_I(bb.n_keys, 5);
    for (int i = 48; i < 80; i++) TEST_ASSERT_EQ_I(b[i], 0);
    TEST_ASSERT_EQ_U(n, ix_p32(96 + (bb.m_mask + 1) / 8));
    free(b); ray_release(s); ray_release(bl);
    PASS();
}

/* A fabricated root with a STR child, or a kind the matrix refuses, fails
 * with nyi naming the kind. */
static test_result_t test_write_rejects(void) {
    int64_t d[3] = { 1, 2, 3 };
    ray_t* v = with_index(i64_vec(d, 3), ray_index_attach_part);
    ray_index_t* ix = ray_index_payload(v->index);
    ray_t* keys = ix->u.part.keys;
    ray_t* fake = ray_vec_new(RAY_STR, 3);
    for (int i = 0; i < 3; i++) fake = ray_str_vec_append(fake, "k", 1);
    ix->u.part.keys = fake;
    size_t n; ray_t* err;
    uint8_t* b = ix_region(v, NULL, 0, &n, &err);
    TEST_ASSERT_NULL(b);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_STR_EQ(ray_err_code(err), "nyi");
    TEST_ASSERT_TRUE(strstr(ray_error_msg(), "PART") != NULL);
    ray_error_free(err);
    ix->u.part.keys = keys;
    ray_release(fake);
    /* SORT on SYM is not admitted */
    ray_t* root = ray_index_new_root(RAY_IDX_SORT, RAY_SYM, 3);
    ray_enc_idx_src_t s = { root, RAY_SYM, RAY_SYM_W8, 3, NULL, 0 };
    FILE* f = fopen(ray_test_tmp(IDX_TMP "/r"), "wb");
    uint64_t bytes;
    err = ray_enc_index_write(f, &s, &bytes);
    fclose(f);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_STR_EQ(ray_err_code(err), "nyi");
    ray_error_free(err);
    ray_release(root); ray_release(v);
    PASS();
}

/* Spec "Normalized bytes": the same index built from different allocations
 * writes the same region. */
static test_result_t test_write_normalized(void) {
    int64_t d[2000];
    for (int i = 0; i < 2000; i++) d[i] = (i * 7919) % 613;
    ray_t* a = with_index(i64_vec(d, 2000), ray_index_attach_hash);
    ray_t* junk[64];
    for (int i = 0; i < 64; i++) junk[i] = ray_vec_new(RAY_I64, 100 + i);
    ray_t* c = with_index(i64_vec(d, 2000), ray_index_attach_hash);
    size_t na, nc; ray_t* err;
    uint8_t* ra = ix_region(a, NULL, 0, &na, &err);
    uint8_t* rc = ix_region(c, NULL, 0, &nc, &err);
    TEST_ASSERT_TRUE(ra && rc && na == nc && memcmp(ra, rc, na) == 0);
    free(ra); free(rc);
    for (int i = 0; i < 64; i++) ray_release(junk[i]);
    ray_release(a); ray_release(c);
    PASS();
}

/* Trust: a heap root's order_sym converts, a passenger root's does not. */
static test_result_t test_trusted_order(void) {
    int64_t d[4] = { 1, 1, 2, 2 };
    ray_t* v = with_index(i64_vec(d, 4), ray_index_attach_part);
    ray_index_t* ix = ray_index_payload(v->index);
    ix->u.part.order_sym = ray_sym_intern("ts", 2);
    TEST_ASSERT_EQ_I(ray_enc_index_trusted_order(v->index), ray_sym_intern("ts", 2));
    ix->markers |= RAY_MARK_MMAP;
    TEST_ASSERT_EQ_I(ray_enc_index_trusted_order(v->index), -1);
    ix->markers &= (uint8_t)~RAY_MARK_MMAP;
    ray_release(v);
    PASS();
}

/* SYM PART keys are written at the width class the source names; a key id
 * past that class fails before any byte is written. */
static test_result_t test_write_sym_part(void) {
    ray_t* v = ray_sym_vec_new(RAY_SYM_W16, 4);
    TEST_ASSERT_NOT_NULL(v);
    static const uint16_t ids[4] = { 300, 300, 700, 700 };
    memcpy(ray_data(v), ids, sizeof ids);
    v->len = 4;
    v = with_index(v, ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(v);
    size_t n; ray_t* err;
    uint8_t* b = ix_region(v, NULL, 0, &n, &err);
    TEST_ASSERT_NULL(err);
    ray_enc_idx_part_t p; memcpy(&p, b + 16, 40);
    TEST_ASSERT_EQ_I(p.n_parts, 2);
    uint16_t k16[2]; memcpy(k16, b + p.keys, 4);
    TEST_ASSERT_EQ_I(k16[0], 300);
    TEST_ASSERT_EQ_I(k16[1], 700);
    TEST_ASSERT_EQ_I(p.starts, (int64_t)ix_p32(96 + 2 * 2));
    free(b);
    /* a wider class widens the keys */
    ray_enc_idx_src_t s = { v->index, RAY_SYM, RAY_SYM_W64, 4, NULL, 0 };
    FILE* f = fopen(ray_test_tmp(IDX_TMP "/r"), "wb");
    uint64_t bytes = 0;
    err = ray_enc_index_write(f, &s, &bytes);
    fclose(f);
    TEST_ASSERT_NULL(err);
    b = ix_slurp(ray_test_tmp(IDX_TMP "/r"), &n);
    TEST_ASSERT_TRUE(b && n == bytes);
    memcpy(&p, b + 16, 40);
    TEST_ASSERT_EQ_I(rd64(b + p.keys), 300);
    TEST_ASSERT_EQ_I(rd64(b + p.keys + 8), 700);
    free(b);
    /* a narrower class would truncate: nyi, nothing written */
    s.attrs = RAY_SYM_W8;
    f = fopen(ray_test_tmp(IDX_TMP "/r"), "wb");
    err = ray_enc_index_write(f, &s, &bytes);
    long at = ftell(f);
    fclose(f);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_STR_EQ(ray_err_code(err), "nyi");
    TEST_ASSERT_TRUE(strstr(ray_error_msg(), "PART") != NULL);
    TEST_ASSERT_EQ_I(at, 0);
    TEST_ASSERT_EQ_I(bytes, 0);
    ray_error_free(err);
    ray_release(v);
    PASS();
}

/* Region bytes do not depend on the core count: the persisted HASH builder
 * is canonical, so the serial build (1 worker) and the parallel build
 * (4 and 8 workers, 70000 rows is past the dispatch threshold) write the
 * same region. */
static test_result_t test_write_core_count(void) {
    enum { N = 70000 };
    int64_t* d = malloc(N * sizeof *d);
    TEST_ASSERT_NOT_NULL(d);
    for (int64_t i = 0; i < N; i++) d[i] = (int64_t)(((uint64_t)i * 2654435761u) % 40000);
    uint32_t cores = ray_pool_total_workers(ray_pool_get());
    static const uint32_t ws[3] = { 1, 4, 8 };
    uint8_t* r[3] = { NULL, NULL, NULL };
    size_t n[3] = { 0, 0, 0 };
    int64_t ng[3] = { -1, -1, -1 };
    bool ok = true;
    for (int w = 0; w < 3 && ok; w++) {
        ray_pool_destroy();
        if (ray_pool_init_total(ws[w]) != RAY_OK) { ok = false; break; }
        ray_t* v = with_index(i64_vec(d, N), ray_index_attach_hash);
        if (!v) { ok = false; break; }
        ng[w] = ray_index_payload(v->index)->u.hash.n_groups;
        ray_t* err = NULL;
        r[w] = ix_region(v, "ts", 2, &n[w], &err);
        if (err) { ray_error_free(err); ok = false; }
        ray_release(v);
    }
    ray_pool_destroy();
    ray_err_t back = ray_pool_init_total(cores);
    bool same = ok && r[0] && r[1] && r[2] && n[1] == n[0] && n[2] == n[0] &&
                memcmp(r[1], r[0], n[0]) == 0 && memcmp(r[2], r[0], n[0]) == 0;
    for (int w = 0; w < 3; w++) free(r[w]);
    free(d);
    TEST_ASSERT_EQ_I(back, RAY_OK);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ_I(ng[0], 40000);
    TEST_ASSERT_EQ_I(ng[1], 40000);
    TEST_ASSERT_EQ_I(ng[2], 40000);
    TEST_ASSERT_TRUE(same);
    PASS();
}

const test_entry_t encidx_entries[] = {
    { "idxval/write_hash_layout", test_write_hash_layout, ix_setup, ix_teardown },
    { "idxval/write_name_block",  test_write_name_block,  ix_setup, ix_teardown },
    { "idxval/write_sort_bloom",  test_write_sort_bloom,  ix_setup, ix_teardown },
    { "idxval/write_rejects",     test_write_rejects,     ix_setup, ix_teardown },
    { "idxval/write_normalized",  test_write_normalized,  ix_setup, ix_teardown },
    { "idxval/trusted_order",     test_trusted_order,     ix_setup, ix_teardown },
    { "idxval/write_sym_part",    test_write_sym_part,    ix_setup, ix_teardown },
    { "idxval/write_core_count",  test_write_core_count,  ix_setup, ix_teardown },
    { NULL, NULL, NULL, NULL },
};
