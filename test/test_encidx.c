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
#include "ops/idxop.h"
#include "table/sym.h"
#include "table/domain.h"
#include "store/encidx.h"
#include "ops/hash.h"
#include "core/pool.h"
#include "ops/rowsel.h"
#include "store/enccol.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
/* v with an index attached by `attach`, or NULL (v released). */
static ray_t* with_index(ray_t* v, ray_t* (*attach)(ray_t**)) {
    if (!v) return NULL;
    ray_t* r = attach(&v);
    if (r && !RAY_IS_ERR(r)) return r;
    if (r) ray_error_free(r);
    ray_release(v);                 /* a failed attach leaves *vp owned */
    return NULL;
}

/* A fabricated HASH root: table/gkeys/offs/rows given by the test. */
static ray_t* fab_hash(int8_t type, int64_t len, const int64_t* tbl, uint64_t cap, const int64_t* gk,
                       int64_t ng, const int64_t* of, const int64_t* rw, int64_t nk) {
    ray_t* root = ray_index_new_root(RAY_IDX_HASH, type, len);
    ray_index_t* ix = ray_index_payload(root);
    ix->u.hash.table = ray_vec_from_raw(RAY_I64, tbl, (int64_t)cap);
    ix->u.hash.gkeys = ray_vec_from_raw(RAY_I64, gk, ng);
    ix->u.hash.offs  = ray_vec_from_raw(RAY_I64, of, ng + 1);
    ix->u.hash.rows  = ray_vec_from_raw(RAY_I64, rw, nk);
    ix->u.hash.mask = cap - 1; ix->u.hash.n_keys = nk; ix->u.hash.n_groups = ng;
    ix->u.hash.order_sym = -1;
    return root;
}

/* Write a source root; the error, or NULL with the region in *out. */
static ray_t* fab_write(ray_t* root, int8_t type, uint8_t attrs, int64_t len, uint8_t** out, size_t* n) {
    FILE* f = fopen(ray_test_tmp(IDX_TMP "/f"), "wb");
    ray_enc_idx_src_t s = { root, type, attrs, len, NULL, 0 };
    uint64_t bytes;
    ray_t* e = ray_enc_index_write(f, &s, &bytes);
    fclose(f);
    *out = NULL;
    if (e) return e;
    *out = ix_slurp(ray_test_tmp(IDX_TMP "/f"), n);
    return NULL;
}
static uint8_t* fab_region(ray_t* root, int8_t type, uint8_t attrs, int64_t len, size_t* n) {
    uint8_t* b;
    ray_t* e = fab_write(root, type, attrs, len, &b, n);
    if (e) { ray_error_free(e); return NULL; }
    return b;
}

static uint64_t slot_of(int64_t k, uint64_t cap) { return ray_index_mix64((uint64_t)k) & (cap - 1); }

/* The writer refuses `root` with nyi naming `why`, writing nothing. */
static test_result_t ix_write_refused(ray_t* root, int8_t type, int64_t len, const char* why) {
    uint8_t* b; size_t n = 0;
    ray_error_clear();
    ray_t* e = fab_write(root, type, 0, len, &b, &n);
    TEST_ASSERT_FMT(e != NULL, "written: %s", why);
    TEST_ASSERT_STR_EQ(ray_err_code(e), "nyi");
    TEST_ASSERT_FMT(strstr(ray_error_msg(), why) != NULL, "message '%s', want '%s'", ray_error_msg(), why);
    TEST_ASSERT_EQ_U(n, 0);
    ray_error_free(e);
    free(b);
    PASS();
}
#define WRITE_REFUSED(root, type, len, why) do { test_result_t r_ = ix_write_refused((root), (type), (len), (why)); \
    if (r_.status != TEST_PASS) return r_; } while (0)

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
    /* a name holding a NUL is written as -1 */
    b = ix_region(v, "t\0s", 3, &n, &err);
    TEST_ASSERT_NULL(err);
    memcpy(&p, b + 16, 40);
    TEST_ASSERT_EQ_I(p.order_name, -1);
    free(b);
    /* SORT and BLOOM have no order_name: a name given is not written */
    { int64_t sd[4] = { 3, 1, 2, 0 };
      ray_t* (*nk[2])(ray_t**) = { ray_index_attach_sort, ray_index_attach_bloom };
      for (int k = 0; k < 2; k++) {
          ray_t* s = with_index(i64_vec(sd, 4), nk[k]);
          TEST_ASSERT_NOT_NULL(s);
          size_t n0, n1; uint8_t* b0 = ix_region(s, NULL, 0, &n0, &err);
          uint8_t* b1 = ix_region(s, "ts", 2, &n1, &err);
          TEST_ASSERT_TRUE(b0 && b1 && n0 == n1 && memcmp(b0, b1, n0) == 0);
          free(b0); free(b1); ray_release(s);
      } }
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
    /* The scalar rules the validator enforces: the writer refuses a root
     * that breaks one even when every child is consistent with it. */
    int32_t bd[5] = { 9, 3, 7, 1, 3 };
    ray_t* bl = with_index(ray_vec_from_raw(RAY_I32, bd, 5), ray_index_attach_bloom);
    TEST_ASSERT_NOT_NULL(bl);
    ray_index_t* bx = ray_index_payload(bl->index);
    bx->u.bloom.k = 0;                       WRITE_REFUSED(bl->index, RAY_I32, 5, "BLOOM k");
    bx->u.bloom.k = RAY_ENC_IDX_BLOOM_K_MAX + 1; WRITE_REFUSED(bl->index, RAY_I32, 5, "BLOOM k");
    bx->u.bloom.k = 3;
    bx->u.bloom.n_keys = 6;                  WRITE_REFUSED(bl->index, RAY_I32, 5, "BLOOM n_keys");
    bx->u.bloom.n_keys = 5;
    ray_release(bl);
    int64_t t8[8] = { 0 }, gk3[3] = { 10, 20, 30 }, of4[4] = { 0, 1, 2, 2 }, rw2[2] = { 0, 1 };
    t8[slot_of(10, 8)] = 1;
    { uint64_t q = slot_of(20, 8); while (t8[q]) q = (q + 1) & 7; t8[q] = 2;
      q = slot_of(30, 8); while (t8[q]) q = (q + 1) & 7; t8[q] = 3; }
    /* three groups over two keys: n_groups > n_keys */
    root = fab_hash(RAY_I64, 4, t8, 8, gk3, 3, of4, rw2, 2);
    WRITE_REFUSED(root, RAY_I64, 4, "HASH n_groups");
    ray_release(root);
    /* five keys over four rows: n_keys > len */
    { int64_t of3[3] = { 0, 2, 5 }, rw5[5] = { 0, 1, 2, 3, 3 };
      root = fab_hash(RAY_I64, 4, t8, 8, gk3, 2, of3, rw5, 5);
      WRITE_REFUSED(root, RAY_I64, 4, "HASH n_keys");
      ray_release(root); }
    /* two buckets for two groups: no empty bucket */
    { int64_t t2[2] = { 1, 2 }, of3[3] = { 0, 1, 2 };
      root = fab_hash(RAY_I64, 4, t2, 2, gk3, 2, of3, rw2, 2);
      WRITE_REFUSED(root, RAY_I64, 4, "empty bucket");
      ray_release(root); }
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
    ray_enc_idx_view_t iv;
    ray_t* verr = r[0] ? ray_enc_index_validate(r[0], n[0], RAY_IDX_HASH, RAY_I64, 0, N, 0, NULL, "col x", &iv) : NULL;
    bool valid = r[0] && !verr && iv.order_len == 2;
    if (verr) ray_error_free(verr);
    for (int w = 0; w < 3; w++) free(r[w]);
    free(d);
    TEST_ASSERT_EQ_I(back, RAY_OK);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ_I(ng[0], 40000);
    TEST_ASSERT_EQ_I(ng[1], 40000);
    TEST_ASSERT_EQ_I(ng[2], 40000);
    TEST_ASSERT_TRUE(same);
    TEST_ASSERT_TRUE(valid);
    PASS();
}

/* ===== Validation (spec "Index validation") ===== */

/* Validate the region of v's index (sym_bound for SYM columns). */
static ray_t* ix_validate(const uint8_t* b, size_t n, ray_t* v, uint32_t bound, ray_enc_idx_view_t* out) {
    ray_index_t* ix = ray_index_payload(v->index);
    return ray_enc_index_validate(b, n, ix->kind, v->type, v->attrs, v->len, bound,
                                  ray_data(v), "col x", out);
}
/* Expect `corrupt` whose message contains `why`. */
static test_result_t ix_expect_corrupt(const uint8_t* b, size_t n, ray_t* v, uint32_t bound, const char* why) {
    ray_enc_idx_view_t iv;
    ray_error_clear();
    ray_t* e = ix_validate(b, n, v, bound, &iv);
    TEST_ASSERT_FMT(e != NULL, "accepted: %s", why);
    TEST_ASSERT_STR_EQ(ray_err_code(e), "corrupt");
    TEST_ASSERT_FMT(strstr(ray_error_msg(), why) != NULL, "message '%s', want '%s'", ray_error_msg(), why);
    TEST_ASSERT_TRUE(strstr(ray_error_msg(), "col x") != NULL);
    ray_error_free(e);
    PASS();
}
#define IX_CORRUPT(b, n, v, bound, why) do { test_result_t r_ = ix_expect_corrupt((b), (n), (v), (bound), (why)); \
    if (r_.status != TEST_PASS) return r_; } while (0)

/* Built indexes of every kind validate. */
static test_result_t test_validate_accepts(void) {
    int64_t d[3000];
    for (int i = 0; i < 3000; i++) d[i] = (i * 31) % 997;
    ray_t* (*kinds[3])(ray_t**) = { ray_index_attach_hash, ray_index_attach_sort, ray_index_attach_bloom };
    for (int k = 0; k < 3; k++) {
        ray_t* v = with_index(i64_vec(d, 3000), kinds[k]);
        TEST_ASSERT_NOT_NULL(v);
        size_t n; ray_t* err; uint8_t* b = ix_region(v, NULL, 0, &n, &err);
        TEST_ASSERT_NOT_NULL(b);
        ray_enc_idx_view_t iv;
        TEST_ASSERT_NULL(ix_validate(b, n, v, 0, &iv));
        TEST_ASSERT_TRUE(iv.base == b && iv.size == n && iv.len == 3000 && iv.order_name == NULL);
        free(b); ray_release(v);
    }
    int64_t s[3000];
    for (int i = 0; i < 3000; i++) s[i] = i / 100;
    ray_t* p = with_index(i64_vec(s, 3000), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(p);
    size_t n; ray_t* err; uint8_t* b = ix_region(p, "ts", 2, &n, &err);
    ray_enc_idx_view_t iv;
    TEST_ASSERT_NULL(ix_validate(b, n, p, 0, &iv));
    TEST_ASSERT_EQ_I(iv.order_len, 2);
    TEST_ASSERT_TRUE(memcmp(iv.order_name, "ts", 2) == 0);
    TEST_ASSERT_EQ_I(iv.part.n_parts, 30);
    free(b); ray_release(p);
    /* SYM HASH and SYM PART against the symbol count */
    ray_t* sv = ray_sym_vec_new(RAY_SYM_W8, 6);
    static const uint8_t ids[6] = { 3, 3, 5, 5, 7, 7 };
    memcpy(ray_data(sv), ids, 6); sv->len = 6;
    ray_retain(sv);
    ray_t* sh = with_index(sv, ray_index_attach_hash);
    ray_t* sp = with_index(sv, ray_index_attach_part);
    TEST_ASSERT_TRUE(sh && sp);
    b = ix_region(sh, NULL, 0, &n, &err);
    TEST_ASSERT_NULL(ix_validate(b, n, sh, 8, &iv));
    IX_CORRUPT(b, n, sh, 7, "SYM");
    free(b);
    b = ix_region(sp, NULL, 0, &n, &err);
    TEST_ASSERT_NULL(ix_validate(b, n, sp, 8, &iv));
    IX_CORRUPT(b, n, sp, 7, "SYM");
    { ray_enc_idx_part_t pp; memcpy(&pp, b + 16, 40);
      uint8_t* m = malloc(n); memcpy(m, b, n); m[pp.keys + 1] = 3;   /* keys 3, 3, 7 */
      IX_CORRUPT(m, n, sp, 8, "distinct"); free(m); }
    free(b); ray_release(sh); ray_release(sp);
    PASS();
}

/* Common rules: root, offsets, extents, padding, tail. */
static test_result_t test_reject_common(void) {
    int64_t d[6] = { 5, 7, 5, 9, 7, 5 };
    ray_t* v = with_index(i64_vec(d, 6), ray_index_attach_hash);
    size_t n; ray_t* err; uint8_t* b = ix_region(v, NULL, 0, &n, &err);
    TEST_ASSERT_NOT_NULL(b);
    uint8_t* m = malloc(n);
    IX_CORRUPT(b, 40, v, 0, "region shorter");                            /* truncated */
    IX_CORRUPT(b, 0, v, 0, "region shorter");
    IX_CORRUPT(b, n - 32, v, 0, "layout");                                /* truncated child */
    memcpy(m, b, n); m[3] = 1;                       IX_CORRUPT(m, n, v, 0, "reserved");
    memcpy(m, b, n); m[0] = RAY_I32;                 IX_CORRUPT(m, n, v, 0, "parent type");
    memcpy(m, b, n); m[8] = 7;                       IX_CORRUPT(m, n, v, 0, "built_for_len");
    memcpy(m, b, n); m[85] = 1;                      IX_CORRUPT(m, n, v, 0, "padding");
    memcpy(m, b, n); { int64_t o = 100; memcpy(m + 16, &o, 8); }   IX_CORRUPT(m, n, v, 0, "placement");
    memcpy(m, b, n); { int64_t o = 128; memcpy(m + 16, &o, 8); }   IX_CORRUPT(m, n, v, 0, "placement");
    /* gkeys moved onto the table (overlap), and past the region */
    memcpy(m, b, n); { int64_t o = 96; memcpy(m + 16 + 8, &o, 8); }   IX_CORRUPT(m, n, v, 0, "placement");
    memcpy(m, b, n); { int64_t o = (int64_t)n; memcpy(m + 16 + 24, &o, 8); } IX_CORRUPT(m, n, v, 0, "placement");
    memcpy(m, b, n); { int64_t o = -32; memcpy(m + 16 + 24, &o, 8); } IX_CORRUPT(m, n, v, 0, "placement");
    memcpy(m, b, n); { int64_t g = 1 << 30; memcpy(m + 16 + 48, &g, 8); } IX_CORRUPT(m, n, v, 0, "n_groups");
    memcpy(m, b, n); { int64_t g = -1; memcpy(m + 16 + 48, &g, 8); } IX_CORRUPT(m, n, v, 0, "n_groups");
    memcpy(m, b, n); { int64_t g = INT64_MAX; memcpy(m + 16 + 40, &g, 8); memcpy(m + 16 + 48, &g, 8); }
    IX_CORRUPT(m, n, v, 0, "n_keys");
    memcpy(m, b, n); { uint64_t k = UINT64_MAX; memcpy(m + 16 + 32, &k, 8); } IX_CORRUPT(m, n, v, 0, "power of two");
    memcpy(m, b, n); { uint64_t k = ((uint64_t)1 << 40) - 1; memcpy(m + 16 + 32, &k, 8); } IX_CORRUPT(m, n, v, 0, "builder's");
    /* the builder's table for 3 groups is 8 buckets; 16 is refused */
    memcpy(m, b, n); { uint64_t k = 15; memcpy(m + 16 + 32, &k, 8); } IX_CORRUPT(m, n, v, 0, "builder's");
    /* a name block where none fits */
    memcpy(m, b, n); { int64_t o = (int64_t)n - 32; memcpy(m + 16 + 56, &o, 8); } IX_CORRUPT(m, n, v, 0, "name");
    memcpy(m, b, n); { int64_t o = 0; memcpy(m + 16 + 56, &o, 8); } IX_CORRUPT(m, n, v, 0, "name");
    memcpy(m, b, n); m[n - 1] = 1;                   IX_CORRUPT(m, n, v, 0, "padding");
    /* a kind the matrix refuses: SORT claimed on a SYM column */
    ray_enc_idx_view_t iv;
    ray_t* e = ray_enc_index_validate(b, n, RAY_IDX_SORT, RAY_SYM, RAY_SYM_W8, 6, 100, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0); ray_error_free(e);
    e = ray_enc_index_validate(b, n, RAY_IDX_BLOOM, RAY_STR, 0, 6, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0); ray_error_free(e);
    e = ray_enc_index_validate(b, n, RAY_IDX_PART, RAY_STR, 0, 6, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0); ray_error_free(e);
    e = ray_enc_index_validate(b, n, RAY_IDX_ZONE, RAY_I64, 0, 6, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0 && strstr(ray_error_msg(), "no region"));
    ray_error_free(e);
    /* a region whose parent type matches, under a kind the type refuses */
    { memcpy(m, b, n); m[0] = (uint8_t)RAY_SYM;
      e = ray_enc_index_validate(m, n, RAY_IDX_SORT, RAY_SYM, RAY_SYM_W8, 6, 100, NULL, "col x", &iv);
      TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0 && strstr(ray_error_msg(), "not admitted"));
      ray_error_free(e); }
    /* the region read as another admitted kind of the same type */
    e = ray_enc_index_validate(b, n, RAY_IDX_BLOOM, RAY_I64, 0, 6, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0); ray_error_free(e);
    e = ray_enc_index_validate(b, n, RAY_IDX_SORT, RAY_I64, 0, 6, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0); ray_error_free(e);
    e = ray_enc_index_validate(b, n, RAY_IDX_PART, RAY_I64, 0, 6, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0); ray_error_free(e);
    /* a negative row count */
    e = ray_enc_index_validate(b, n, RAY_IDX_HASH, RAY_I64, 0, -1, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0); ray_error_free(e);
    free(m); free(b); ray_release(v);
    PASS();
}

static test_result_t ix_fab_corrupt(ray_t* root, int8_t type, int64_t len, uint32_t bound, const char* why) {
    size_t n; uint8_t* b = fab_region(root, type, RAY_SYM_W8, len, &n);
    ray_release(root);
    TEST_ASSERT_NOT_NULL(b);
    uint8_t attrs = type == RAY_SYM ? RAY_SYM_W8 : 0;
    ray_enc_idx_view_t iv;
    ray_error_clear();
    ray_t* e = ray_enc_index_validate(b, n, RAY_IDX_HASH, type, attrs, len, bound, NULL, "col x", &iv);
    free(b);
    TEST_ASSERT_FMT(e != NULL, "accepted: %s", why);
    TEST_ASSERT_STR_EQ(ray_err_code(e), "corrupt");
    TEST_ASSERT_FMT(strstr(ray_error_msg(), why) != NULL, "message '%s', want '%s'", ray_error_msg(), why);
    ray_error_free(e);
    PASS();
}
#define FAB_CORRUPT(root, type, len, bound, why) do { test_result_t r_ = ix_fab_corrupt((root), (type), (len), (bound), (why)); \
    if (r_.status != TEST_PASS) return r_; } while (0)

/* A fabricated region with the HASH kind-area field at byte `at` patched
 * to `val` after writing (roots the writer refuses). */
static test_result_t ix_patch_corrupt(ray_t* root, int at, uint64_t val, const char* why) {
    size_t n; uint8_t* b = fab_region(root, RAY_I64, 0, 4, &n);
    ray_release(root);
    TEST_ASSERT_NOT_NULL(b);
    memcpy(b + 16 + at, &val, 8);
    ray_enc_idx_view_t iv;
    ray_error_clear();
    ray_t* e = ray_enc_index_validate(b, n, RAY_IDX_HASH, RAY_I64, 0, 4, 0, NULL, "col x", &iv);
    free(b);
    TEST_ASSERT_FMT(e != NULL, "accepted: %s", why);
    TEST_ASSERT_STR_EQ(ray_err_code(e), "corrupt");
    TEST_ASSERT_FMT(strstr(ray_error_msg(), why) != NULL, "message '%s', want '%s'", ray_error_msg(), why);
    ray_error_free(e);
    PASS();
}
#define PATCH_CORRUPT(root, at, val, why) do { test_result_t r_ = ix_patch_corrupt((root), (at), (val), (why)); \
    if (r_.status != TEST_PASS) return r_; } while (0)

/* HASH rules on fabricated roots (4 rows, keys 10 and 20). */
static test_result_t test_reject_hash(void) {
    int64_t of[3] = { 0, 2, 4 }, rw[4] = { 0, 2, 1, 3 }, gk[2] = { 10, 20 };
    int64_t t8[8] = { 0 };
    t8[slot_of(10, 8)] = 1;
    { uint64_t s2 = slot_of(20, 8); while (t8[s2]) s2 = (s2 + 1) & 7; t8[s2] = 2; }
    /* the fabrication itself is valid */
    { size_t n; uint8_t* b = fab_region(fab_hash(RAY_I64, 4, t8, 8, gk, 2, of, rw, 4), RAY_I64, 0, 4, &n);
      ray_enc_idx_view_t iv;
      TEST_ASSERT_NOT_NULL(b);
      TEST_ASSERT_NULL(ray_enc_index_validate(b, n, RAY_IDX_HASH, RAY_I64, 0, 4, 0, NULL, "col x", &iv));
      TEST_ASSERT_EQ_I(iv.hash.n_groups, 2);
      free(b); }
    /* roots the writer refuses, patched after writing */
    PATCH_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, of, rw, 4), 32, 1, "empty bucket");     /* 2 buckets */
    PATCH_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, of, rw, 4), 32, 5, "power of two");     /* 6 buckets */
    PATCH_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, of, rw, 4), 40, 5, "n_keys");           /* n_keys > len */
    PATCH_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, of, rw, 4), 40, 1, "n_groups");         /* n_keys < n_groups */
    { int64_t tb[8]; memcpy(tb, t8, sizeof tb); for (int i = 0; i < 8; i++) if (!tb[i]) { tb[i] = 3; break; }
      FAB_CORRUPT(fab_hash(RAY_I64, 4, tb, 8, gk, 2, of, rw, 4), RAY_I64, 4, 0, "holds 3"); }
    { int64_t tb[8]; memcpy(tb, t8, sizeof tb); for (int i = 0; i < 8; i++) if (!tb[i]) { tb[i] = -1; break; }
      FAB_CORRUPT(fab_hash(RAY_I64, 4, tb, 8, gk, 2, of, rw, 4), RAY_I64, 4, 0, "holds -1"); }
    { int64_t tb[8]; memcpy(tb, t8, sizeof tb); for (int i = 0; i < 8; i++) if (!tb[i]) { tb[i] = 1; break; }
      FAB_CORRUPT(fab_hash(RAY_I64, 4, tb, 8, gk, 2, of, rw, 4), RAY_I64, 4, 0, "more than one bucket"); }
    { int64_t tb[8]; memcpy(tb, t8, sizeof tb); for (int i = 0; i < 8; i++) if (tb[i] == 2) tb[i] = 0;
      FAB_CORRUPT(fab_hash(RAY_I64, 4, tb, 8, gk, 2, of, rw, 4), RAY_I64, 4, 0, "have a bucket"); }
    { int64_t dup[2] = { 10, 10 };
      FAB_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, dup, 2, of, rw, 4), RAY_I64, 4, 0, "lookup"); }
    { /* one group placed past the empty bucket its key hashes to */
      int64_t g1[1] = { 123456789 }, o1[2] = { 0, 4 };
      int64_t tb[8] = { 0 }; tb[(slot_of(g1[0], 8) + 1) & 7] = 1;
      FAB_CORRUPT(fab_hash(RAY_I64, 4, tb, 8, g1, 1, o1, rw, 4), RAY_I64, 4, 0, "lookup"); }
    { int64_t bo[3] = { 1, 2, 4 };
      FAB_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, bo, rw, 4), RAY_I64, 4, 0, "offs"); }
    { int64_t bo[3] = { 0, 2, 3 };
      FAB_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, bo, rw, 4), RAY_I64, 4, 0, "offs"); }
    { int64_t bo[3] = { 0, 4, 4 };
      FAB_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, bo, rw, 4), RAY_I64, 4, 0, "offs"); }
    { int64_t bo[3] = { 0, 9, 4 };
      FAB_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, bo, rw, 4), RAY_I64, 4, 0, "offs"); }
    { int64_t bo[3] = { 0, -5, 4 };
      FAB_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, bo, rw, 4), RAY_I64, 4, 0, "offs"); }
    { int64_t br[4] = { 0, 2, 1, 9 };
      FAB_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, of, br, 4), RAY_I64, 4, 0, "rows"); }
    { int64_t br[4] = { 0, 2, 1, 4 };          /* = len */
      FAB_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, of, br, 4), RAY_I64, 4, 0, "rows entry 4"); }
    { int64_t br[4] = { 0, 2, 1, -1 };
      FAB_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, of, br, 4), RAY_I64, 4, 0, "rows"); }
    { int64_t br[4] = { 2, 0, 1, 3 };
      FAB_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, of, br, 4), RAY_I64, 4, 0, "ascending"); }
    { int64_t br[4] = { 0, 2, 0, 3 };
      FAB_CORRUPT(fab_hash(RAY_I64, 4, t8, 8, gk, 2, of, br, 4), RAY_I64, 4, 0, "unique"); }
    /* SYM: gkeys must be in [1, bound) */
    { int64_t sg[2] = { 0, 20 };
      int64_t ts[8] = { 0 }; ts[slot_of(0, 8)] = 1; uint64_t s2 = slot_of(20, 8); while (ts[s2]) s2 = (s2 + 1) & 7; ts[s2] = 2;
      FAB_CORRUPT(fab_hash(RAY_SYM, 4, ts, 8, sg, 2, of, rw, 4), RAY_SYM, 4, 30, "SYM"); }
    { FAB_CORRUPT(fab_hash(RAY_SYM, 4, t8, 8, gk, 2, of, rw, 4), RAY_SYM, 4, 15, "SYM"); }
    PASS();
}

/* Sparse rows over a long column: the uniqueness check is bounded by the
 * region, not the claimed row count. */
static test_result_t test_validate_sparse_rows(void) {
    int64_t of[3] = { 0, 2, 4 }, gk[2] = { 10, 20 };
    int64_t big = (int64_t)1 << 40;
    int64_t t8[8] = { 0 };
    t8[slot_of(10, 8)] = 1;
    { uint64_t s2 = slot_of(20, 8); while (t8[s2]) s2 = (s2 + 1) & 7; t8[s2] = 2; }
    int64_t rw[4] = { 0, big - 1, 5, big - 2 };
    size_t n; uint8_t* b = fab_region(fab_hash(RAY_I64, big, t8, 8, gk, 2, of, rw, 4), RAY_I64, 0, big, &n);
    TEST_ASSERT_NOT_NULL(b);
    ray_enc_idx_view_t iv;
    TEST_ASSERT_NULL(ray_enc_index_validate(b, n, RAY_IDX_HASH, RAY_I64, 0, big, 0, NULL, "col x", &iv));
    ray_enc_idx_hash_t h; memcpy(&h, b + 16, 64);
    int64_t dup = big - 1; memcpy(b + h.rows + 24, &dup, 8);   /* rows { 0, big-1 }, { 5, big-1 } */
    ray_error_clear();
    ray_t* e = ray_enc_index_validate(b, n, RAY_IDX_HASH, RAY_I64, 0, big, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strstr(ray_error_msg(), "unique") != NULL);
    ray_error_free(e);
    free(b);
    PASS();
}

/* Spec reject fixture: "a STR group unreachable from its word". */
static test_result_t test_reject_str_unreachable(void) {
    int64_t gk[1] = { 123456789 }, of[2] = { 0, 2 }, rw[2] = { 0, 1 };
    int64_t t8[8] = { 0 };
    uint64_t h = slot_of(gk[0], 8);
    t8[(h + 1) & 7] = 1;            /* past the empty bucket at h */
    FAB_CORRUPT(fab_hash(RAY_STR, 2, t8, 8, gk, 1, of, rw, 2), RAY_STR, 2, 0, "unreachable");
    /* an empty bucket inside the probe path, not the walk's first: a run
     * of two (homes hw-1, hw), an empty bucket, then a second group of
     * word w (legal for STR) at hw+2; slot 0 is empty too */
    int64_t w = 1;
    while (slot_of(w, 8) < 2 || slot_of(w, 8) > 4) w++;
    uint64_t hw = slot_of(w, 8);
    int64_t w0 = 1;
    while (slot_of(w0, 8) != hw - 1) w0++;
    int64_t g3[3] = { w0, w, w }, o3[4] = { 0, 1, 2, 3 }, r3[3] = { 0, 1, 2 };
    int64_t tb[8] = { 0 };
    tb[hw - 1] = 1; tb[hw] = 2; tb[hw + 2] = 3;
    FAB_CORRUPT(fab_hash(RAY_STR, 3, tb, 8, g3, 3, o3, r3, 3), RAY_STR, 3, 0, "unreachable");
    tb[hw + 1] = 3; tb[hw + 2] = 0;                       /* adjacent: reachable */
    { size_t n; uint8_t* b = fab_region(fab_hash(RAY_STR, 3, tb, 8, g3, 3, o3, r3, 3), RAY_STR, 0, 3, &n);
      ray_enc_idx_view_t iv;
      TEST_ASSERT_NOT_NULL(b);
      TEST_ASSERT_NULL(ray_enc_index_validate(b, n, RAY_IDX_HASH, RAY_STR, 0, 3, 0, NULL, "col x", &iv));
      free(b); }
    PASS();
}

/* Review focus 1 (spec "STR indexes": forced word collisions).  Real rows
 * "alpha", "beta", "alpha"; a fabricated root gives both groups alpha's word
 * and puts beta's group FIRST on that chain, so a probe for "alpha" meets a
 * word hit whose bytes differ and must walk on (hash_probe_setup_str's
 * disambiguation).  The region validates, and the source root finds alpha
 * past the collision; beta (whose own word is in no bucket here) and gamma
 * are absent.  The parsed root's half of this comparison, answering exactly
 * as the source root, lands with the parse (Task 4). */
static ray_t* swc_col(void) {
    ray_t* v = ray_vec_new(RAY_STR, 3);
    v = ray_str_vec_append(v, "alpha", 5); v = ray_str_vec_append(v, "beta", 4); v = ray_str_vec_append(v, "alpha", 5);
    return v;
}
static test_result_t test_str_word_collision(void) {
    ray_t* v = swc_col();
    int64_t w = (int64_t)ray_hash_bytes("alpha", 5);
    int64_t gk[2] = { w, w }, of[3] = { 0, 1, 3 }, rw[3] = { 1, 0, 2 };   /* g0 = beta, g1 = alpha */
    int64_t t8[8] = { 0 };
    uint64_t h = slot_of(w, 8);
    t8[h] = 1; t8[(h + 1) & 7] = 2;
    ray_t* root = fab_hash(RAY_STR, 3, t8, 8, gk, 2, of, rw, 3);
    size_t n; uint8_t* b = fab_region(root, RAY_STR, 0, 3, &n);
    TEST_ASSERT_NOT_NULL(b);
    ray_enc_idx_view_t iv;
    TEST_ASSERT_NULL(ray_enc_index_validate(b, n, RAY_IDX_HASH, RAY_STR, 0, 3, 0, NULL, "col x", &iv));
    TEST_ASSERT_EQ_I(iv.hash.n_groups, 2);
    /* the parsed root: the source's fields and children, and nothing in
     * it refers to the region (freed before the probes) */
    ray_t* proot = NULL;
    TEST_ASSERT_NULL(ray_enc_index_parse(&iv, v, &proot));
    TEST_ASSERT_NOT_NULL(proot);
    free(b);
    { ray_index_t* sx = ray_index_payload(root); ray_index_t* px = ray_index_payload(proot);
      TEST_ASSERT_EQ_I(px->kind, RAY_IDX_HASH);
      TEST_ASSERT_EQ_U(px->u.hash.mask, 7);
      TEST_ASSERT_EQ_I(px->u.hash.n_groups, 2);
      TEST_ASSERT_EQ_I(px->u.hash.n_keys, 3);
      TEST_ASSERT_EQ_I(px->u.hash.order_sym, -1);
      TEST_ASSERT_TRUE(memcmp(ray_data(px->u.hash.table), ray_data(sx->u.hash.table), 64) == 0);
      TEST_ASSERT_TRUE(memcmp(ray_data(px->u.hash.gkeys), gk, 16) == 0);
      TEST_ASSERT_TRUE(memcmp(ray_data(px->u.hash.offs), of, 24) == 0);
      TEST_ASSERT_TRUE(memcmp(ray_data(px->u.hash.rows), rw, 24) == 0); }
    ray_t* src = v; ray_retain(src);
    ray_t* a = ray_index_attach_persisted(&src, root);                 /* consumes root */
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a)); src = a;
    ray_t* pv = swc_col();
    ray_t* pa = ray_index_attach_persisted(&pv, proot);                /* consumes proot */
    TEST_ASSERT_TRUE(pa && !RAY_IS_ERR(pa)); pv = pa;
    /* Both roots give alpha past the collision at row 0.  beta is absent
     * from both: the fabricated root gives beta alpha's word, so no bucket
     * holds beta's own word and its probe ends at an empty bucket. */
    static const char* const probes[3] = { "alpha", "beta", "gamma" };
    static const int64_t want[3] = { 0, -1, -1 };
    for (int i = 0; i < 3; i++) {
        ray_t* atom = ray_str(probes[i], strlen(probes[i]));
        int64_t r0 = ray_index_find_atom(src, atom);
        int64_t r1 = ray_index_find_atom(pv, atom);
        TEST_ASSERT_FMT(r0 == want[i], "source probe %s: %lld, want %lld", probes[i], (long long)r0, (long long)want[i]);
        TEST_ASSERT_FMT(r1 == r0, "parsed probe %s: %lld, source %lld", probes[i], (long long)r1, (long long)r0);
        ray_release(atom);
    }
    ray_release(pv); ray_release(src); ray_release(v);
    PASS();
}

/* SORT, BLOOM, PART and name-block rules. */
static test_result_t test_reject_sort_bloom_part(void) {
    int64_t d[6] = { 4, 2, 9, 1, 3, 3 };
    ray_t* s = with_index(i64_vec(d, 6), ray_index_attach_sort);
    size_t n; ray_t* err; uint8_t* b = ix_region(s, NULL, 0, &n, &err);
    TEST_ASSERT_NOT_NULL(b);
    uint8_t* m = malloc(n + 64);
    memcpy(m, b, n); { int64_t x = 0; memcpy(m + 96 + 8, &x, 8); memcpy(m + 96, &x, 8); } IX_CORRUPT(m, n, s, 0, "permutation");
    memcpy(m, b, n); { int64_t x = 6; memcpy(m + 96, &x, 8); } IX_CORRUPT(m, n, s, 0, "permutation");
    memcpy(m, b, n); { int64_t x = -1; memcpy(m + 96, &x, 8); } IX_CORRUPT(m, n, s, 0, "permutation");
    memcpy(m, b, n); m[24] = 1;                      IX_CORRUPT(m, n, s, 0, "reserved");   /* kind-area tail */
    IX_CORRUPT(b, 96, s, 0, "layout");                                                    /* perm cut off */
    free(b); ray_release(s);
    ray_t* bl = with_index(i64_vec(d, 6), ray_index_attach_bloom);
    b = ix_region(bl, NULL, 0, &n, &err);
    TEST_ASSERT_NOT_NULL(b);
    memcpy(m, b, n); m[16 + 16] = 0;                 IX_CORRUPT(m, n, bl, 0, "BLOOM k");  /* k = 0 */
    memcpy(m, b, n); m[16 + 16] = 4;                 IX_CORRUPT(m, n, bl, 0, "BLOOM k");  /* k > 3 */
    memcpy(m, b, n); { int64_t x = 7; memcpy(m + 16 + 24, &x, 8); } IX_CORRUPT(m, n, bl, 0, "n_keys");
    memcpy(m, b, n); { int64_t x = -1; memcpy(m + 16 + 24, &x, 8); } IX_CORRUPT(m, n, bl, 0, "n_keys");
    memcpy(m, b, n); m[16 + 20] = 1;                 IX_CORRUPT(m, n, bl, 0, "reserved");
    memcpy(m, b, n); { uint64_t x = 5; memcpy(m + 16 + 8, &x, 8); } IX_CORRUPT(m, n, bl, 0, "power of two");
    memcpy(m, b, n); { uint64_t x = 3; memcpy(m + 16 + 8, &x, 8); } IX_CORRUPT(m, n, bl, 0, "power of two");
    memcpy(m, b, n); { uint64_t x = 11; memcpy(m + 16 + 8, &x, 8); } IX_CORRUPT(m, n, bl, 0, "power of two");  /* 12 bits */
    memcpy(m, b, n); { uint64_t x = UINT64_MAX; memcpy(m + 16 + 8, &x, 8); } IX_CORRUPT(m, n, bl, 0, "power of two");
    memcpy(m, b, n); { uint64_t x = ((uint64_t)1 << 40) - 1; memcpy(m + 16 + 8, &x, 8); } IX_CORRUPT(m, n, bl, 0, "builder's");
    /* 6 keys: the builder's 64 bits; 128 (the bits child fits either way) */
    memcpy(m, b, n); { uint64_t x = 127; memcpy(m + 16 + 8, &x, 8); } IX_CORRUPT(m, n, bl, 0, "builder's");
    memcpy(m, b, n); { int64_t x = 0; memcpy(m + 16 + 24, &x, 8); }
    TEST_ASSERT_NULL(ix_validate(m, n, bl, 0, &(ray_enc_idx_view_t){ 0 }));        /* 0 keys: 64 bits too */
    free(b); ray_release(bl);
    int64_t pd[6] = { 1, 1, 3, 3, 3, 8 };
    ray_t* p = with_index(i64_vec(pd, 6), ray_index_attach_part);
    b = ix_region(p, "ts", 2, &n, &err);
    TEST_ASSERT_NOT_NULL(b);
    ray_enc_idx_part_t pp; memcpy(&pp, b + 16, 40);
    memcpy(m, b, n); { int64_t x = 1; memcpy(m + pp.starts, &x, 8); }      IX_CORRUPT(m, n, p, 0, "starts");
    memcpy(m, b, n); { int64_t x = 0; memcpy(m + pp.lens + 8, &x, 8); }    IX_CORRUPT(m, n, p, 0, "lens");
    memcpy(m, b, n); { int64_t x = -4; memcpy(m + pp.lens + 8, &x, 8); }   IX_CORRUPT(m, n, p, 0, "lens");
    memcpy(m, b, n); { int64_t x = 3; memcpy(m + pp.lens + 16, &x, 8); }   IX_CORRUPT(m, n, p, 0, "sum");
    memcpy(m, b, n); { int64_t x = INT64_MAX; memcpy(m + pp.lens + 16, &x, 8); } IX_CORRUPT(m, n, p, 0, "sum");
    memcpy(m, b, n); { int64_t x = 0; memcpy(m + pp.lens + 16, &x, 8); }   IX_CORRUPT(m, n, p, 0, "lens");
    memcpy(m, b, n); { int64_t x = 0; memcpy(m + pp.keys + 8, &x, 8); }    IX_CORRUPT(m, n, p, 0, "ascend");
    memcpy(m, b, n); { int64_t x = 1; memcpy(m + pp.keys + 8, &x, 8); }    IX_CORRUPT(m, n, p, 0, "ascend");
    memcpy(m, b, n); { int64_t x = 7; memcpy(m + 16 + 24, &x, 8); }        IX_CORRUPT(m, n, p, 0, "n_parts");
    memcpy(m, b, n); { int64_t x = 0; memcpy(m + 16 + 24, &x, 8); }        IX_CORRUPT(m, n, p, 0, "n_parts");
    memcpy(m, b, n); { int64_t x = -1; memcpy(m + 16 + 24, &x, 8); }       IX_CORRUPT(m, n, p, 0, "n_parts");
    memcpy(m, b, n); { uint32_t x = 0; memcpy(m + pp.order_name, &x, 4); } IX_CORRUPT(m, n, p, 0, "name");
    memcpy(m, b, n); { uint32_t x = 256; memcpy(m + pp.order_name, &x, 4); } IX_CORRUPT(m, n, p, 0, "name");
    memcpy(m, b, n); { uint32_t x = 30; memcpy(m + pp.order_name, &x, 4); } IX_CORRUPT(m, n, p, 0, "name");
    memcpy(m, b, n); { uint32_t x = UINT32_MAX; memcpy(m + pp.order_name, &x, 4); } IX_CORRUPT(m, n, p, 0, "name");
    memcpy(m, b, n); m[pp.order_name + 4] = 1;                              IX_CORRUPT(m, n, p, 0, "name");
    memcpy(m, b, n); m[pp.order_name + 12] = 1;                             IX_CORRUPT(m, n, p, 0, "padding");
    memcpy(m, b, n); m[pp.order_name + 9] = 0;                              IX_CORRUPT(m, n, p, 0, "NUL");
    { uint8_t* x = malloc(n); memcpy(x, b, n);                            /* exact size: no slack past the end */
      uint32_t l = 30; memcpy(x + pp.order_name, &l, 4);                  IX_CORRUPT(x, n, p, 0, "name block outside");
      free(x); }
    memcpy(m, b, n); { int64_t x = pp.order_name + 32; memcpy(m + 16 + 32, &x, 8); } IX_CORRUPT(m, n, p, 0, "name");
    memcpy(m, b, n); { int64_t x = pp.lens; memcpy(m + 16 + 32, &x, 8); }  IX_CORRUPT(m, n, p, 0, "name");  /* on a child */
    memcpy(m, b, n); { int64_t x = -1; memcpy(m + 16 + 32, &x, 8); }       IX_CORRUPT(m, n, p, 0, "layout");
    IX_CORRUPT(b, (size_t)pp.order_name + 4, p, 0, "name");                /* name block cut off */
    free(b); ray_release(p);
    /* lens short of the rows with every start consistent: blocks {2,3,2}, last 1 */
    int64_t pd7[7] = { 1, 1, 3, 3, 3, 8, 8 };
    p = with_index(i64_vec(pd7, 7), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(p);
    b = ix_region(p, NULL, 0, &n, &err);
    TEST_ASSERT_NOT_NULL(b);
    memcpy(&pp, b + 16, 40);
    memcpy(m, b, n); { int64_t x = 1; memcpy(m + pp.lens + 16, &x, 8); }   IX_CORRUPT(m, n, p, 0, "lens sum 6");
    free(b); ray_release(p);
    free(m);
    PASS();
}

/* Counts whose byte size would wrap 64 bits (8 * 2^61) over a column that
 * claims that many rows: rejected on the count against the region size,
 * before a child is placed or a byte past the region is read. */
static test_result_t test_reject_wrapping_counts(void) {
    int64_t huge = (int64_t)1 << 61;
    ray_enc_idx_view_t iv;
    uint8_t* r = calloc(1, 4096);
    TEST_ASSERT_NOT_NULL(r);
    /* SORT: perm of 2^61 rows in a 96-byte region */
    r[0] = (uint8_t)RAY_I64; memcpy(r + 8, &huge, 8); { int64_t o = 96; memcpy(r + 16, &o, 8); }
    ray_error_clear();
    ray_t* e = ray_enc_index_validate(r, 96, RAY_IDX_SORT, RAY_I64, 0, huge, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0 && strstr(ray_error_msg(), "outside the region"));
    ray_error_free(e);
    /* PART: 2^61 parts, keys / starts / lens all at 96 */
    memset(r, 0, 4096);
    r[0] = (uint8_t)RAY_I64; memcpy(r + 8, &huge, 8);
    { ray_enc_idx_part_t pp = { 96, 96, 96, huge, -1 }; memcpy(r + 16, &pp, sizeof pp); }
    e = ray_enc_index_validate(r, 96, RAY_IDX_PART, RAY_I64, 0, huge, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0 && strstr(ray_error_msg(), "outside the region"));
    ray_error_free(e);
    /* HASH: n_keys 2^61 (offs ends there too), the region cut where rows starts */
    int64_t of[3] = { 0, 2, 4 }, gk[2] = { 10, 20 }, rw[4] = { 0, 1, 2, 3 };
    int64_t t8[8] = { 0 };
    t8[slot_of(10, 8)] = 1;
    { uint64_t s2 = slot_of(20, 8); while (t8[s2]) s2 = (s2 + 1) & 7; t8[s2] = 2; }
    int64_t len = huge * 2;
    size_t n; uint8_t* b = fab_region(fab_hash(RAY_I64, len, t8, 8, gk, 2, of, rw, 4), RAY_I64, 0, len, &n);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NULL(ray_enc_index_validate(b, n, RAY_IDX_HASH, RAY_I64, 0, len, 0, NULL, "col x", &iv));
    ray_enc_idx_hash_t h; memcpy(&h, b + 16, 64);
    memcpy(b + 16 + 40, &huge, 8); memcpy(b + h.offs + 16, &huge, 8);
    e = ray_enc_index_validate(b, (uint64_t)h.rows, RAY_IDX_HASH, RAY_I64, 0, len, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0 && strstr(ray_error_msg(), "outside the region"));
    ray_error_free(e);
    /* canonical sizes for 2^20 groups and keys, the region far smaller */
    { int64_t g20 = (int64_t)1 << 20; uint64_t m21 = ((uint64_t)1 << 21) - 1;
      uint8_t* c = malloc(n); memcpy(c, b, n);
      { int64_t four = 4; memcpy(c + 16 + 40, &four, 8); memcpy(c + h.offs + 16, &four, 8); }
      memcpy(c + 16 + 32, &m21, 8); memcpy(c + 16 + 40, &g20, 8); memcpy(c + 16 + 48, &g20, 8);
      e = ray_enc_index_validate(c, n, RAY_IDX_HASH, RAY_I64, 0, len, 0, NULL, "col x", &iv);
      TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0 && strstr(ray_error_msg(), "HASH table outside the region"));
      ray_error_free(e); free(c); }
    { memset(r, 0, 4096);
      int64_t k40 = (int64_t)1 << 40; uint64_t mm = ((uint64_t)1 << 43) - 1;
      r[0] = (uint8_t)RAY_I64; memcpy(r + 8, &k40, 8);
      ray_enc_idx_bloom_t bb = { 96, mm, 3, 0, k40 }; memcpy(r + 16, &bb, sizeof bb);
      e = ray_enc_index_validate(r, 4096, RAY_IDX_BLOOM, RAY_I64, 0, k40, 0, NULL, "col x", &iv);
      TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0 && strstr(ray_error_msg(), "BLOOM bits outside the region"));
      ray_error_free(e); }
    /* a negative row count claimed by root and column alike, every kind */
    static const uint8_t ks[4] = { RAY_IDX_HASH, RAY_IDX_SORT, RAY_IDX_BLOOM, RAY_IDX_PART };
    for (int k = 0; k < 4; k++) {
        memset(r, 0, 4096);
        r[0] = (uint8_t)RAY_I64; { int64_t m1 = -1; memcpy(r + 8, &m1, 8); }
        e = ray_enc_index_validate(r, 4096, ks[k], RAY_I64, 0, -1, 0, NULL, "col x", &iv);
        TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "corrupt") == 0);
        ray_error_free(e);
    }
    free(b); free(r);
    PASS();
}

/* Empty columns: every kind's region over zero rows validates. */
static test_result_t test_validate_empty(void) {
    ray_t* (*kinds[4])(ray_t**) = { ray_index_attach_hash, ray_index_attach_sort,
                                    ray_index_attach_bloom, ray_index_attach_part };
    for (int k = 0; k < 4; k++) {
        ray_t* v = with_index(ray_vec_new(RAY_I64, 0), kinds[k]);
        TEST_ASSERT_NOT_NULL(v);
        size_t n; ray_t* err; uint8_t* b = ix_region(v, NULL, 0, &n, &err);
        TEST_ASSERT_NULL(err);
        TEST_ASSERT_NOT_NULL(b);
        ray_enc_idx_view_t iv;
        ray_t* e = ix_validate(b, n, v, 0, &iv);
        TEST_ASSERT_FMT(e == NULL, "kind %d: %s", k, e ? ray_error_msg() : "");
        free(b); ray_release(v);
    }
    PASS();
}

/* PART float keys: NaN, two zeros, a zero whose sign is not its block's
 * first row's. */
static test_result_t test_reject_part_zero_sign(void) {
    double d[4] = { -1.0, -0.0, 0.0, 2.0 };          /* blocks: [-1], [-0, +0], [2] */
    ray_t* p = with_index(ray_vec_from_raw(RAY_F64, d, 4), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ_I(ray_index_payload(p->index)->u.part.n_parts, 3);
    size_t n; ray_t* err; uint8_t* b = ix_region(p, NULL, 0, &n, &err);
    ray_enc_idx_view_t iv;
    TEST_ASSERT_NULL(ix_validate(b, n, p, 0, &iv));
    ray_enc_idx_part_t pp; memcpy(&pp, b + 16, 40);
    uint8_t* m = malloc(n);
    memcpy(m, b, n); { double z = 0.0; memcpy(m + pp.keys + 8, &z, 8); }   IX_CORRUPT(m, n, p, 0, "sign");
    /* without the column the sign rule is skipped */
    TEST_ASSERT_NULL(ray_enc_index_validate(m, n, RAY_IDX_PART, RAY_F64, 0, 4, 0, NULL, "col x", &iv));
    memcpy(m, b, n); { double z = NAN; memcpy(m + pp.keys + 16, &z, 8); }  IX_CORRUPT(m, n, p, 0, "NaN");
    memcpy(m, b, n); { double z = -0.0; memcpy(m + pp.keys, &z, 8); }      IX_CORRUPT(m, n, p, 0, "increase");
    free(m); free(b); ray_release(p);
    /* infinities are keys, not NaN */
    { double di[3] = { -INFINITY, 1.0, INFINITY };
      ray_t* q = with_index(ray_vec_from_raw(RAY_F64, di, 3), ray_index_attach_part);
      TEST_ASSERT_NOT_NULL(q);
      b = ix_region(q, NULL, 0, &n, &err);
      TEST_ASSERT_NOT_NULL(b);
      ray_t* e = ix_validate(b, n, q, 0, &iv);
      TEST_ASSERT_FMT(e == NULL, "%s", ray_error_msg());
      free(b); ray_release(q); }
    /* F32 */
    float f[3] = { 1.0f, 2.0f, 2.0f };
    ray_t* q = with_index(ray_vec_from_raw(RAY_F32, f, 3), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(q);
    b = ix_region(q, NULL, 0, &n, &err);
    TEST_ASSERT_NULL(ix_validate(b, n, q, 0, &iv));
    memcpy(&pp, b + 16, 40);
    m = malloc(n);
    memcpy(m, b, n); { float z = 0.5f; memcpy(m + pp.keys + 4, &z, 4); }  IX_CORRUPT(m, n, q, 0, "increase");
    free(m); free(b); ray_release(q);
    PASS();
}

/* Signaling NaNs are NaN too: a quiet-only check would accept a positive
 * sNaN as the largest key and a negative sNaN as the smallest. */
static test_result_t test_reject_part_snan(void) {
    double d[3] = { -1.0, 1.0, 2.0 };
    ray_t* p = with_index(ray_vec_from_raw(RAY_F64, d, 3), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(p);
    size_t n; ray_t* err; uint8_t* b = ix_region(p, NULL, 0, &n, &err);
    TEST_ASSERT_NOT_NULL(b);
    ray_enc_idx_view_t iv;
    TEST_ASSERT_NULL(ix_validate(b, n, p, 0, &iv));
    ray_enc_idx_part_t pp; memcpy(&pp, b + 16, 40);
    uint8_t* m = malloc(n);
    static const uint64_t s64[2] = { 0x7FF0000000000001ull, 0x7FF4000000000000ull };   /* quiet bit clear */
    for (int i = 0; i < 2; i++) {
        uint64_t neg = s64[i] | 0x8000000000000000ull;
        memcpy(m, b, n); memcpy(m + pp.keys + 16, &s64[i], 8); IX_CORRUPT(m, n, p, 0, "NaN");   /* last */
        memcpy(m, b, n); memcpy(m + pp.keys, &neg, 8);         IX_CORRUPT(m, n, p, 0, "NaN");   /* first */
    }
    free(m); free(b); ray_release(p);
    float f[3] = { -1.0f, 1.0f, 2.0f };
    ray_t* q = with_index(ray_vec_from_raw(RAY_F32, f, 3), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(q);
    b = ix_region(q, NULL, 0, &n, &err);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NULL(ix_validate(b, n, q, 0, &iv));
    memcpy(&pp, b + 16, 40);
    m = malloc(n);
    static const uint32_t s32[2] = { 0x7F800001u, 0x7FA00000u };
    for (int i = 0; i < 2; i++) {
        uint32_t neg = s32[i] | 0x80000000u;
        memcpy(m, b, n); memcpy(m + pp.keys + 8, &s32[i], 4); IX_CORRUPT(m, n, q, 0, "NaN");
        memcpy(m, b, n); memcpy(m + pp.keys, &neg, 4);        IX_CORRUPT(m, n, q, 0, "NaN");
    }
    free(m); free(b); ray_release(q);
    PASS();
}

/* Review carry (Task 2): every byte outside the root fields, the children
 * and the name bytes is zero in the writer's output (root reserved, kind
 * tail, inter-child padding, the name block's reserved u32, BLOOM rsv, the
 * tail), and setting any one of them is corrupt. */
static test_result_t ix_zero_fill(ray_t* v, const char* name, uint32_t nlen) {
    size_t n; ray_t* err;
    uint8_t* b = ix_region(v, name, nlen, &n, &err);
    TEST_ASSERT_NOT_NULL(b);
    uint8_t* used = calloc(n, 1);
    #define USE(o, k) do { for (uint64_t u_ = 0; u_ < (uint64_t)(k); u_++) used[(o) + u_] = 1; } while (0)
    USE(0, 1); USE(8, 8);
    ray_index_t* ix = ray_index_payload(v->index);
    int64_t name_at = -1;
    if (ix->kind == RAY_IDX_HASH) {
        ray_enc_idx_hash_t h; memcpy(&h, b + 16, 64);
        USE(16, 64);
        USE(h.table, (h.mask + 1) * 8); USE(h.gkeys, h.n_groups * 8);
        USE(h.offs, (h.n_groups + 1) * 8); USE(h.rows, h.n_keys * 8);
        name_at = h.order_name;
    } else {
        ray_enc_idx_bloom_t bb; memcpy(&bb, b + 16, 32);
        USE(16, 20); USE(16 + 24, 8);           /* bits, m_mask, k; n_keys (rsv stays zero) */
        USE(bb.bits, (bb.m_mask + 1) / 8);
    }
    if (nlen) {
        TEST_ASSERT_TRUE(name_at > 0);
        USE(name_at, 4); USE(name_at + 8, nlen);
    }
    #undef USE
    ray_enc_idx_view_t iv;
    TEST_ASSERT_NULL(ix_validate(b, n, v, 0, &iv));
    uint8_t* m = malloc(n);
    for (size_t i = 0; i < n; i++) {
        if (used[i]) continue;
        TEST_ASSERT_FMT(b[i] == 0, "byte %zu of %zu is %u", i, n, b[i]);
        memcpy(m, b, n); m[i] = 0x40;
        ray_t* e = ix_validate(m, n, v, 0, &iv);
        TEST_ASSERT_FMT(e != NULL, "byte %zu of %zu set, accepted", i, n);
        TEST_ASSERT_STR_EQ(ray_err_code(e), "corrupt");
        ray_error_free(e);
    }
    free(m); free(used); free(b);
    PASS();
}
static test_result_t test_write_zero_fill(void) {
    int64_t d[7] = { 5, 7, 5, 9, 7, 5, 11 };
    ray_t* h = with_index(i64_vec(d, 7), ray_index_attach_hash);
    ray_t* bl = with_index(i64_vec(d, 7), ray_index_attach_bloom);
    TEST_ASSERT_TRUE(h && bl);
    test_result_t r = ix_zero_fill(h, "ts", 2);
    if (r.status == TEST_PASS) r = ix_zero_fill(h, NULL, 0);
    if (r.status == TEST_PASS) r = ix_zero_fill(bl, NULL, 0);
    ray_release(h); ray_release(bl);
    return r;
}

/* ===== Parse ===== */

static bool same_child(const ray_t* a, const ray_t* b, size_t esz) {
    return a && b && a->type == b->type && a->len == b->len &&
           memcmp(ray_data((ray_t*)a), ray_data((ray_t*)b), (size_t)a->len * esz) == 0;
}

/* Parse the validated view of v's index and compare the heap root with
 * the source root field by field; name: the order_name written. */
static test_result_t ix_parse_same(ray_t* v, const ray_enc_idx_view_t* iv, const char* name, const char* what) {
    ray_t* root = NULL;
    ray_t* e = ray_enc_index_parse(iv, v, &root);
    if (e) ray_error_free(e);
    TEST_ASSERT_FMT(e == NULL && root != NULL, "%s: parse failed", what);
    const ray_index_t* a = ray_index_payload(v->index);
    const ray_index_t* p = ray_index_payload(root);
    bool hdr = p->kind == a->kind && p->parent_type == a->parent_type && p->built_for_len == a->built_for_len;
    bool mapped = (p->markers & RAY_MARK_MMAP) != 0;
    int64_t want_sym = name ? ray_sym_intern(name, strlen(name)) : -1;
    bool ok = false;
    switch (a->kind) {
    case RAY_IDX_HASH:
        ok = p->u.hash.mask == a->u.hash.mask && p->u.hash.n_keys == a->u.hash.n_keys &&
             p->u.hash.n_groups == a->u.hash.n_groups && p->u.hash.order_sym == want_sym &&
             same_child(p->u.hash.table, a->u.hash.table, 8) && same_child(p->u.hash.gkeys, a->u.hash.gkeys, 8) &&
             same_child(p->u.hash.offs, a->u.hash.offs, 8) && same_child(p->u.hash.rows, a->u.hash.rows, 8);
        break;
    case RAY_IDX_SORT:
        ok = same_child(p->u.sort.perm, a->u.sort.perm, 8);
        break;
    case RAY_IDX_BLOOM:
        ok = p->u.bloom.m_mask == a->u.bloom.m_mask && p->u.bloom.k == a->u.bloom.k &&
             p->u.bloom.n_keys == a->u.bloom.n_keys && same_child(p->u.bloom.bits, a->u.bloom.bits, 1);
        break;
    default: {
        const ray_t* pk = p->u.part.keys; const ray_t* ak = a->u.part.keys;
        ok = p->u.part.n_parts == a->u.part.n_parts && p->u.part.order_sym == want_sym &&
             same_child(p->u.part.starts, a->u.part.starts, 8) && same_child(p->u.part.lens, a->u.part.lens, 8) &&
             pk && pk->type == ak->type && pk->len == ak->len;
        if (ok && v->type == RAY_SYM) {
            ok = ray_sym_vec_domain((ray_t*)pk) == ray_sym_vec_domain(v) &&
                 ray_sym_elem_size(RAY_SYM, pk->attrs) == ray_sym_elem_size(RAY_SYM, v->attrs);
            for (int64_t i = 0; ok && i < pk->len; i++)
                ok = ray_read_sym(ray_data((ray_t*)pk), i, RAY_SYM, pk->attrs) ==
                     ray_read_sym(ray_data((ray_t*)ak), i, RAY_SYM, ak->attrs);
        } else if (ok) {
            ok = memcmp(ray_data((ray_t*)pk), ray_data((ray_t*)ak), (size_t)pk->len * ray_sym_elem_size(pk->type, pk->attrs)) == 0;
        }
        break;
    }
    }
    ray_release(root);                                  /* before any assertion can return */
    TEST_ASSERT_FMT(hdr, "%s: root header", what);
    TEST_ASSERT_FMT(!mapped, "%s: parsed root marked mapped", what);
    TEST_ASSERT_FMT(ok, "%s: parsed root differs from the source", what);
    PASS();
}
#define PARSE_SAME(v, iv, name, ...) do { char w_[96]; snprintf(w_, sizeof w_, __VA_ARGS__); \
    test_result_t r_ = ix_parse_same((v), (iv), (name), w_); if (r_.status != TEST_PASS) return r_; } while (0)

/* A column of type t over n rows for the engine matrix: ascending
 * value-blocks when `sorted` (PART), else mixed values with nulls, both
 * zeros and NaN payloads for floats. */
static ray_t* mx_col(int8_t t, int64_t n, bool sorted) {
    ray_t* v = ray_vec_new(t, n > 0 ? n : 1);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = n;
    uint8_t* p = ray_data(v);
    for (int64_t i = 0; i < n; i++) {
        int64_t x = sorted ? (i * 5) / n - 2 : ((i * 37) % 13) - 6;
        if (t == RAY_BOOL) x = sorted ? (i * 2 >= n) : (i & 1);
        if (t == RAY_U8) x = sorted ? (i * 5) / n : (i * 37) % 251;
        switch (t) {
        case RAY_BOOL: case RAY_U8: p[i] = (uint8_t)x; break;
        case RAY_I16: { int16_t y = (int16_t)x; memcpy(p + i * 2, &y, 2); break; }
        case RAY_I32: case RAY_DATE: case RAY_TIME: { int32_t y = (int32_t)x; memcpy(p + i * 4, &y, 4); break; }
        case RAY_F32: {
            float y = (float)x;
            if (x == 0 && (sorted ? n > 0 : (i & 1))) y = -0.0f;
            memcpy(p + i * 4, &y, 4);
            if (!sorted && i % 17 == 5) { uint32_t q = 0x7FC00000u | (uint32_t)i; memcpy(p + i * 4, &q, 4); }
            break;
        }
        case RAY_F64: {
            double y = (double)x;
            if (x == 0 && (sorted ? n > 0 : (i & 1))) y = -0.0;
            memcpy(p + i * 8, &y, 8);
            if (!sorted && i % 17 == 5) { uint64_t q = 0x7FF8000000000000ull | (uint64_t)i; memcpy(p + i * 8, &q, 8); }
            break;
        }
        default: memcpy(p + i * 8, &x, 8); break;
        }
    }
    if (!sorted)
        for (int64_t i = 3; i < n; i += 11) ray_vec_set_null(v, i, true);
    return v;
}

/* Review probe, recreated: every engine-built HASH / SORT / BLOOM / PART
 * over every admitted type (nulls, both zeros, NaN payloads), SYM at
 * W8 / W16 / W32 and STR with "" validates. */
static test_result_t test_validate_engine_matrix(void) {
    static const int8_t types[10] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_F32, RAY_F64,
                                      RAY_DATE, RAY_TIME, RAY_TIMESTAMP };
    static const int64_t lens[5] = { 0, 1, 7, 100, 3000 };
    ray_t* (*kinds[4])(ray_t**) = { ray_index_attach_hash, ray_index_attach_sort,
                                    ray_index_attach_bloom, ray_index_attach_part };
    int built = 0;
    ray_enc_idx_view_t iv;
    for (int ti = 0; ti < 10; ti++)
        for (int li = 0; li < 5; li++)
            for (int k = 0; k < 4; k++) {
                ray_t* v = with_index(mx_col(types[ti], lens[li], k == 3), kinds[k]);
                if (!v) continue;
                size_t n; ray_t* err; uint8_t* b = ix_region(v, k == 3 || k == 0 ? "ts" : NULL, 2, &n, &err);
                TEST_ASSERT_FMT(b != NULL, "type %d len %lld kind %d: not written", types[ti], (long long)lens[li], k);
                ray_t* e = ix_validate(b, n, v, 0, &iv);
                TEST_ASSERT_FMT(e == NULL, "type %d len %lld kind %d: %s", types[ti], (long long)lens[li], k, ray_error_msg());
                PARSE_SAME(v, &iv, k == 3 || k == 0 ? "ts" : NULL, "type %d len %lld kind %d", types[ti], (long long)lens[li], k);
                free(b); ray_release(v); built++;
            }
    static const uint8_t ws[3] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32 };
    static const int64_t base[3] = { 0, 300, 70000 };
    for (int w = 0; w < 3; w++)
        for (int li = 1; li < 5; li++)
            for (int k = 0; k < 2; k++) {
                int64_t n = lens[li];
                ray_t* v = ray_sym_vec_new(ws[w], n);
                TEST_ASSERT_NOT_NULL(v);
                v->len = n;
                for (int64_t i = 0; i < n; i++) {
                    int64_t id = k ? base[w] + 1 + (i * 5) / n : (i % 11 == 3 ? 0 : base[w] + 1 + (i * 37) % 13);
                    ray_write_sym(ray_data(v), i, (uint64_t)id, RAY_SYM, ws[w]);
                }
                v = with_index(v, k ? ray_index_attach_part : ray_index_attach_hash);
                TEST_ASSERT_FMT(v != NULL, "SYM width %d len %lld kind %d: not built", w, (long long)n, k);
                size_t rn; ray_t* err; uint8_t* b = ix_region(v, "ts", 2, &rn, &err);
                TEST_ASSERT_NOT_NULL(b);
                ray_t* e = ix_validate(b, rn, v, (uint32_t)(base[w] + 20), &iv);
                TEST_ASSERT_FMT(e == NULL, "SYM width %d len %lld kind %d: %s", w, (long long)n, k, ray_error_msg());
                PARSE_SAME(v, &iv, "ts", "SYM width %d len %lld kind %d", w, (long long)n, k);
                free(b); ray_release(v); built++;
            }
    for (int li = 0; li < 5; li++) {
        static const char* const strs[5] = { "", "a", "bb", "", "ccc" };
        ray_t* v = ray_vec_new(RAY_STR, lens[li] > 0 ? lens[li] : 1);
        for (int64_t i = 0; i < lens[li]; i++) v = ray_str_vec_append(v, strs[(i * 7) % 5], strlen(strs[(i * 7) % 5]));
        v = with_index(v, ray_index_attach_hash);
        TEST_ASSERT_NOT_NULL(v);
        size_t n; ray_t* err; uint8_t* b = ix_region(v, NULL, 0, &n, &err);
        TEST_ASSERT_NOT_NULL(b);
        ray_t* e = ix_validate(b, n, v, 0, &iv);
        TEST_ASSERT_FMT(e == NULL, "STR len %lld: %s", (long long)lens[li], ray_error_msg());
        PARSE_SAME(v, &iv, NULL, "STR len %lld", (long long)lens[li]);
        free(b); ray_release(v); built++;
    }
    /* A HASH carried through concat (ray_index_carry_append) over several
     * appends of new keys, its bucket table regrown past 8: written, then
     * validated and parsed like a built one. */
    for (int t = 0; t < 3; t++) {
        ray_t* v = NULL;
        int64_t rows = 0;
        for (int step = 0; step < 5; step++) {
            int64_t m = step ? 3 + step : 6;         /* rows: 6, then 4, 5, 6, 7 new keys */
            ray_t* nv;
            if (t == 2) {
                nv = ray_vec_new(RAY_STR, rows + m);
                for (int64_t i = 0; i < rows + m; i++) {
                    char sb[16]; int sl = snprintf(sb, sizeof sb, "k%lld", (long long)(i < 6 ? i % 3 : i));
                    nv = ray_str_vec_append(nv, sb, (size_t)sl);
                }
            } else {
                nv = t == 0 ? ray_vec_new(RAY_I64, rows + m) : ray_sym_vec_new(RAY_SYM_W16, rows + m);
                nv->len = rows + m;
                for (int64_t i = 0; i < rows + m; i++) {
                    int64_t k = i < 6 ? i % 3 : i;           /* the first 6 rows repeat; appends are new */
                    if (t == 0) ((int64_t*)ray_data(nv))[i] = k * 1000 - 7;
                    else ray_write_sym(ray_data(nv), i, (uint64_t)(k + 1), RAY_SYM, RAY_SYM_W16);
                }
            }
            if (!step) {
                v = with_index(nv, ray_index_attach_hash);
            } else {
                ray_index_carry_append(v, nv);
                ray_release(v);
                v = nv;
            }
            TEST_ASSERT_FMT(v && ray_index_kind(v) == RAY_IDX_HASH, "carried HASH type %d step %d: no index", t, step);
            rows += m;
        }
        TEST_ASSERT_TRUE(ray_index_payload(v->index)->u.hash.mask + 1 >= 32);
        size_t n; ray_t* err; uint8_t* b = ix_region(v, t == 2 ? NULL : "ts", 2, &n, &err);
        TEST_ASSERT_FMT(b != NULL, "carried HASH type %d: not written", t);
        ray_t* e = ix_validate(b, n, v, 64, &iv);
        TEST_ASSERT_FMT(e == NULL, "carried HASH type %d: %s", t, ray_error_msg());
        PARSE_SAME(v, &iv, t == 2 ? NULL : "ts", "carried HASH type %d", t);
        free(b); ray_release(v); built++;
    }
    TEST_ASSERT_FMT(built >= 150, "only %d indexes built", built);
    PASS();
}

/* A forged HASH of one long cluster: 2^17 distinct keys whose homes lie in
 * the first sixteenth of a 2^18-bucket table, linearly probed in.  A
 * per-group probe walk is quadratic here (about 2^33 bucket reads); the
 * validator's single walk is O(cap + ng log ng). */
static int lc_cmp_home(const void* a, const void* b) {
    uint64_t x = ((const uint64_t*)a)[0], y = ((const uint64_t*)b)[0];
    return (x > y) - (x < y);
}
static uint8_t* long_cluster_region(int64_t ng, size_t* n) {
    uint64_t cap = 1;
    while (cap < 2 * (uint64_t)ng) cap <<= 1;
    int64_t* tbl = calloc(cap, 8);
    int64_t* gk = malloc((size_t)ng * 8);
    int64_t* of = malloc((size_t)(ng + 1) * 8);
    int64_t* rw = malloc((size_t)ng * 8);
    uint64_t* hk = malloc((size_t)ng * 16);              /* (home, key) */
    int64_t key = 1;
    for (int64_t g = 0; g < ng; g++) {
        while (slot_of(key, cap) >= cap / 16) key++;
        hk[2 * g] = slot_of(key, cap); hk[2 * g + 1] = (uint64_t)key++;
    }
    /* inserting in home order makes linear probing a running max */
    qsort(hk, (size_t)ng, 16, lc_cmp_home);
    uint64_t next = 0;
    for (int64_t g = 0; g < ng; g++) {
        uint64_t s = hk[2 * g] > next ? hk[2 * g] : next;
        gk[g] = (int64_t)hk[2 * g + 1];
        tbl[s] = g + 1; next = s + 1;
        of[g] = g; rw[g] = g;
    }
    of[ng] = ng;
    uint8_t* b = NULL;
    if (next < cap) {
        ray_t* root = fab_hash(RAY_I64, ng, tbl, cap, gk, ng, of, rw, ng);
        b = fab_region(root, RAY_I64, 0, ng, n);
        ray_release(root);
    }
    free(tbl); free(gk); free(of); free(rw); free(hk);
    return b;
}
static test_result_t test_validate_long_cluster(void) {
    int64_t ng = (int64_t)1 << 17;
    size_t n; uint8_t* b = long_cluster_region(ng, &n);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(n > ((size_t)4 << 20));
    ray_enc_idx_view_t iv;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ray_t* e = ray_enc_index_validate(b, n, RAY_IDX_HASH, RAY_I64, 0, ng, 0, NULL, "col x", &iv);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    TEST_ASSERT_FMT(e == NULL, "%s", ray_error_msg());
    TEST_ASSERT_FMT(s < 1.0, "validation took %.3f s", s);
    /* the same cluster with one group moved before its home: rejected */
    ray_enc_idx_hash_t h; memcpy(&h, b + 16, 64);
    uint64_t cap = h.mask + 1, e0 = cap, at = cap;
    for (uint64_t p = 0; p < cap && e0 == cap; p++) {        /* an empty bucket before a cluster */
        int64_t t, tp;
        memcpy(&t, b + h.table + p * 8, 8); memcpy(&tp, b + h.table + ((p - 1) & h.mask) * 8, 8);
        if (t && !tp) { e0 = (p - 1) & h.mask; at = p; }
    }
    TEST_ASSERT_TRUE(e0 < cap);
    int64_t t; memcpy(&t, b + h.table + at * 8, 8);
    memcpy(b + h.table + e0 * 8, &t, 8);
    memset(b + h.table + at * 8, 0, 8);                       /* the group now sits before its home */
    e = ray_enc_index_validate(b, n, RAY_IDX_HASH, RAY_I64, 0, ng, 0, NULL, "col x", &iv);
    TEST_ASSERT_TRUE(e && strstr(ray_error_msg(), "lookup") != NULL);
    ray_error_free(e);
    free(b);
    PASS();
}

/* ===== Parse round trips ===== */

/* Region of v's index -> validate -> parse -> attach to a fresh copy of v. */
static ray_t* ix_roundtrip(ray_t* v, const char* name, uint32_t nlen, uint32_t bound) {
    size_t n; ray_t* err; uint8_t* b = ix_region(v, name, nlen, &n, &err);
    if (!b) { if (err) ray_error_free(err); return NULL; }
    ray_enc_idx_view_t iv;
    ray_t* fresh = ray_vec_new(v->type, v->len);
    if (v->type == RAY_SYM) { ray_release(fresh); fresh = ray_sym_vec_new(v->attrs & RAY_SYM_W_MASK, v->len); ray_sym_vec_adopt_domain(fresh, v); }
    fresh->len = v->len;
    memcpy(ray_data(fresh), ray_data(v), (size_t)v->len * ray_sym_elem_size(v->type, v->attrs));
    ray_t* ve = ix_validate(b, n, v, bound, &iv);
    if (ve) { ray_error_free(ve); free(b); ray_release(fresh); return NULL; }
    ray_t* root = NULL;
    ray_t* pe = ray_enc_index_parse(&iv, fresh, &root);
    free(b);
    if (pe) { ray_error_free(pe); ray_release(fresh); return NULL; }
    ray_t* r = ray_index_attach_persisted(&fresh, root);
    if (!r || RAY_IS_ERR(r)) { if (r) ray_error_free(r); ray_release(fresh); return NULL; }
    return r;
}

static test_result_t test_parse_hash_probes(void) {
    int64_t d[5000];
    for (int i = 0; i < 5000; i++) d[i] = (i * 7919) % 1499;
    ray_t* v = with_index(i64_vec(d, 5000), ray_index_attach_hash);
    ray_t* p = ix_roundtrip(v, NULL, 0, 0);
    TEST_ASSERT_NOT_NULL(p);
    ray_index_t* a = ray_index_payload(v->index);
    ray_index_t* b = ray_index_payload(p->index);
    TEST_ASSERT_EQ_I(b->kind, RAY_IDX_HASH);
    TEST_ASSERT_EQ_U(b->u.hash.mask, a->u.hash.mask);
    TEST_ASSERT_EQ_I(b->u.hash.n_groups, a->u.hash.n_groups);
    TEST_ASSERT_EQ_I(b->u.hash.order_sym, -1);
    TEST_ASSERT_FALSE(b->markers & RAY_MARK_MMAP);
    for (int64_t key = 0; key < 1499; key += 37) {
        ray_t* sa = ray_index_hash_eq_rowsel(v, key);
        ray_t* sb = ray_index_hash_eq_rowsel(p, key);
        TEST_ASSERT_TRUE(sa && sb);
        TEST_ASSERT_EQ_I(ray_rowsel_meta(sb)->total_pass, ray_rowsel_meta(sa)->total_pass);
        ray_rowsel_release(sa); ray_rowsel_release(sb);
    }
    ray_release(p); ray_release(v);
    PASS();
}

/* PART with an order name: the name interns to the same id; SYM keys keep
 * the column's domain. */
static test_result_t test_parse_part_order_sym(void) {
    int64_t d[8] = { 1, 1, 1, 4, 4, 9, 9, 9 };
    ray_t* v = with_index(i64_vec(d, 8), ray_index_attach_part);
    ray_t* p = ix_roundtrip(v, "ordcol_p4", 9, 0);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ_I(ray_index_payload(p->index)->u.part.order_sym, ray_sym_intern("ordcol_p4", 9));
    ray_release(p); ray_release(v);
    for (int i = 0; i < 20; i++) { char b[8]; int n = snprintf(b, sizeof b, "q%d", i); (void)ray_sym_intern(b, (size_t)n); }
    ray_t* s = ray_sym_vec_new(RAY_SYM_W8, 6);
    s->len = 6;
    uint8_t ids[6] = { 3, 3, 7, 7, 7, 5 };
    memcpy(ray_data(s), ids, 6);
    ray_t* sp = with_index(s, ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(sp);
    ray_t* q = ix_roundtrip(sp, NULL, 0, 21);
    TEST_ASSERT_NOT_NULL(q);
    ray_t* keys = ray_index_payload(q->index)->u.part.keys;
    TEST_ASSERT_EQ_I(keys->type, RAY_SYM);
    TEST_ASSERT_EQ_I(keys->len, 3);
    TEST_ASSERT_EQ_I(ray_read_sym(ray_data(keys), 1, RAY_SYM, keys->attrs), 7);
    TEST_ASSERT_TRUE(ray_sym_vec_domain(keys) == ray_sym_vec_domain(q));
    ray_release(q); ray_release(sp);
    /* a column over a private domain: the keys resolve over that domain */
    ray_sym_domain_t* dom = ray_sym_domain_new();
    TEST_ASSERT_NOT_NULL(dom);
    static const char* const words[4] = { "pd_zero", "pd_one", "pd_two", "pd_three" };
    int64_t pos[4];
    for (int i = 0; i < 4; i++) pos[i] = ray_sym_domain_intern(dom, words[i], strlen(words[i]));
    ray_t* f = ray_sym_vec_new(RAY_SYM_W8, 5);
    f->len = 5;
    uint8_t fid[5] = { (uint8_t)pos[2], (uint8_t)pos[2], (uint8_t)pos[1], (uint8_t)pos[3], (uint8_t)pos[3] };
    memcpy(ray_data(f), fid, 5);
    f->sym_domain = dom;                                        /* the vector holds the reference */
    ray_t* fp = with_index(f, ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(fp);
    ray_t* fq = ix_roundtrip(fp, NULL, 0, (uint32_t)ray_sym_domain_count(dom));
    TEST_ASSERT_NOT_NULL(fq);
    keys = ray_index_payload(fq->index)->u.part.keys;
    TEST_ASSERT_TRUE(ray_sym_vec_domain(keys) == dom);
    TEST_ASSERT_TRUE(ray_sym_vec_domain(keys) != ray_sym_runtime_domain());
    TEST_ASSERT_EQ_I(keys->len, 3);
    ray_t* w1 = ray_sym_domain_str(ray_sym_vec_domain(keys), ray_read_sym(ray_data(keys), 1, RAY_SYM, keys->attrs));
    TEST_ASSERT_TRUE(w1 && ray_str_len(w1) == 6 && memcmp(ray_str_ptr(w1), "pd_one", 6) == 0);
    ray_release(fq); ray_release(fp);
    PASS();
}

/* Every allocation of the parse failing in turn: oom, no root, and
 * nothing left allocated (the error path releases what it set). */
static test_result_t ix_parse_oom_run(ray_t* v, const ray_enc_idx_view_t* iv) {
    int fails = 0;
    for (int64_t nth = 1; ; nth++) {
        ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
        ray_enccol_test_fail_alloc(nth);
        ray_t* root = (ray_t*)1;
        ray_t* e = ray_enc_index_parse(iv, v, &root);
        ray_enccol_test_fail_alloc(0);
        if (!e) {
            bool got = root != NULL && root != (ray_t*)1;
            if (got) ray_release(root);
            ray_mem_stats(&m1);
            TEST_ASSERT_TRUE(got);
            TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);
            break;
        }
        bool oom = strcmp(ray_err_code(e), "oom") == 0;
        ray_error_free(e);
        if (root && root != (ray_t*)1) { ray_release(root); FAILF("allocation %lld failing: a root with the error", (long long)nth); }
        TEST_ASSERT_FMT(oom, "allocation %lld failing: not oom", (long long)nth);
        TEST_ASSERT_NULL(root);
        ray_mem_stats(&m1);
        TEST_ASSERT_FMT(m1.bytes_allocated == m0.bytes_allocated, "allocation %lld failing leaks %lld bytes",
                        (long long)nth, (long long)(m1.bytes_allocated - m0.bytes_allocated));
        fails++;
        TEST_ASSERT_TRUE(nth < 16);
    }
    TEST_ASSERT_TRUE(fails >= 2);                    /* the root and at least one child */
    PASS();
}
static test_result_t ix_parse_oom(ray_t* v, const char* name, uint32_t bound) {
    size_t n; ray_t* err; uint8_t* b = ix_region(v, name, name ? (uint32_t)strlen(name) : 0, &n, &err);
    if (err) ray_error_free(err);
    TEST_ASSERT_NOT_NULL(b);
    ray_enc_idx_view_t iv;
    ray_t* ve = ix_validate(b, n, v, bound, &iv);
    if (ve) { ray_error_free(ve); free(b); FAIL("region not valid"); }
    if (name) (void)ray_sym_intern(name, strlen(name));        /* interned before the baseline */
    test_result_t r = ix_parse_oom_run(v, &iv);
    free(b);
    return r;
}
#define PARSE_OOM(v, name, bound) do { test_result_t r_ = ix_parse_oom((v), (name), (bound)); \
    if (r_.status != TEST_PASS) return r_; } while (0)

static test_result_t test_parse_oom_frees(void) {
    int64_t d[40];
    for (int i = 0; i < 40; i++) d[i] = (i * 7) % 11;
    ray_t* (*kinds[3])(ray_t**) = { ray_index_attach_hash, ray_index_attach_sort, ray_index_attach_bloom };
    for (int k = 0; k < 3; k++) {
        ray_t* v = with_index(i64_vec(d, 40), kinds[k]);
        TEST_ASSERT_NOT_NULL(v);
        PARSE_OOM(v, k == 0 ? "ts" : NULL, 0);
        ray_release(v);
    }
    int64_t pd[6] = { 1, 1, 3, 3, 3, 8 };
    ray_t* p = with_index(i64_vec(pd, 6), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(p);
    PARSE_OOM(p, "ts", 0);
    ray_release(p);
    ray_t* s = ray_sym_vec_new(RAY_SYM_W16, 5);
    s->len = 5;
    uint16_t ids[5] = { 4, 4, 9, 2, 2 };
    memcpy(ray_data(s), ids, sizeof ids);
    ray_t* sp = with_index(s, ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(sp);
    PARSE_OOM(sp, "ts", 64);
    ray_release(sp);
    PASS();
}

/* A view the region does not hold is refused before any allocation, and
 * a SYM PART needs the decoded SYM column for its domain. */
static test_result_t test_parse_rejects_view(void) {
    int64_t d[6] = { 5, 7, 5, 9, 7, 5 };
    ray_t* v = with_index(i64_vec(d, 6), ray_index_attach_hash);
    TEST_ASSERT_NOT_NULL(v);
    size_t n; ray_t* err; uint8_t* b = ix_region(v, "ts", 2, &n, &err);
    TEST_ASSERT_NOT_NULL(b);
    ray_enc_idx_view_t iv, w;
    TEST_ASSERT_NULL(ix_validate(b, n, v, 0, &iv));
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    #define VIEW_REFUSED(code, mut) do { w = iv; mut; ray_t* r_ = (ray_t*)1; ray_t* e_ = ray_enc_index_parse(&w, v, &r_); \
        TEST_ASSERT_TRUE(e_ != NULL); TEST_ASSERT_STR_EQ(ray_err_code(e_), code); TEST_ASSERT_NULL(r_); \
        ray_error_free(e_); } while (0)
    VIEW_REFUSED("corrupt", w.hash.rows = (int64_t)n);
    VIEW_REFUSED("corrupt", w.hash.rows = 64);                              /* inside the root */
    VIEW_REFUSED("corrupt", w.hash.n_keys = (int64_t)n);
    VIEW_REFUSED("corrupt", w.hash.n_keys = -1);
    VIEW_REFUSED("corrupt", w.hash.n_groups = INT64_MAX);
    VIEW_REFUSED("corrupt", w.hash.mask = UINT64_MAX);
    VIEW_REFUSED("corrupt", w.hash.mask = ((uint64_t)1 << 40) - 1);
    VIEW_REFUSED("corrupt", w.size = 64);
    VIEW_REFUSED("corrupt", w.base = NULL);
    VIEW_REFUSED("corrupt", w.order_len = (uint32_t)n);
    VIEW_REFUSED("corrupt", w.order_name = (const char*)b + n);
    VIEW_REFUSED("corrupt", w.kind = RAY_IDX_ZONE);
    VIEW_REFUSED("corrupt", (w.kind = RAY_IDX_SORT, w.len = (int64_t)n));
    VIEW_REFUSED("corrupt", (w.kind = RAY_IDX_BLOOM, w.bloom.bits = 96, w.bloom.m_mask = UINT64_MAX));
    /* boundaries: one element past the region, a name in the root */
    VIEW_REFUSED("corrupt", w.hash.n_keys = ((int64_t)n - iv.hash.rows) / 8 + 1);
    VIEW_REFUSED("corrupt", w.order_name = (const char*)b + 8);
    VIEW_REFUSED("corrupt", w.order_name = (const char*)b + 95);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);
    free(b); ray_release(v);
    /* a 256-byte name inside a region long enough to hold it: refused by
     * the name limit, not by the span */
    { int64_t d2[40];
      for (int i = 0; i < 40; i++) d2[i] = i % 11;
      ray_t* v2 = with_index(i64_vec(d2, 40), ray_index_attach_hash);
      TEST_ASSERT_NOT_NULL(v2);
      b = ix_region(v2, "ts", 2, &n, &err);
      TEST_ASSERT_NOT_NULL(b);
      TEST_ASSERT_TRUE(n >= RAY_ENC_IDX_FIRST_CHILD + 256);
      TEST_ASSERT_NULL(ix_validate(b, n, v2, 0, &iv));
      ray_t* vsave = v; v = v2;
      VIEW_REFUSED("corrupt", (w.order_name = (const char*)b + RAY_ENC_IDX_FIRST_CHILD, w.order_len = 256));
      v = vsave;
      free(b); ray_release(v2); }
    ray_t* s = ray_sym_vec_new(RAY_SYM_W8, 4);
    s->len = 4;
    uint8_t ids[4] = { 3, 3, 6, 6 };
    memcpy(ray_data(s), ids, 4);
    ray_t* sp = with_index(s, ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(sp);
    b = ix_region(sp, NULL, 0, &n, &err);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NULL(ix_validate(b, n, sp, 64, &iv));
    ray_t* r = (ray_t*)1;
    ray_t* e = ray_enc_index_parse(&iv, NULL, &r);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "type") == 0 && r == NULL);
    ray_error_free(e);
    ray_t* i64v = i64_vec(d, 4);
    e = ray_enc_index_parse(&iv, i64v, &r);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "type") == 0 && r == NULL);
    ray_error_free(e); ray_release(i64v);
    #undef VIEW_REFUSED
    free(b); ray_release(sp);
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
    { "idxval/write_zero_fill",         test_write_zero_fill,         ix_setup, ix_teardown },
    { "idxval/validate_accepts",        test_validate_accepts,        ix_setup, ix_teardown },
    { "idxval/reject_common",           test_reject_common,           ix_setup, ix_teardown },
    { "idxval/reject_hash",             test_reject_hash,             ix_setup, ix_teardown },
    { "idxval/validate_sparse_rows",    test_validate_sparse_rows,    ix_setup, ix_teardown },
    { "idxval/reject_str_unreachable",  test_reject_str_unreachable,  ix_setup, ix_teardown },
    { "idxval/str_word_collision",      test_str_word_collision,      ix_setup, ix_teardown },
    { "idxval/reject_sort_bloom_part",  test_reject_sort_bloom_part,  ix_setup, ix_teardown },
    { "idxval/reject_part_zero_sign",   test_reject_part_zero_sign,   ix_setup, ix_teardown },
    { "idxval/reject_wrapping_counts",  test_reject_wrapping_counts,  ix_setup, ix_teardown },
    { "idxval/validate_empty",          test_validate_empty,          ix_setup, ix_teardown },
    { "idxval/validate_engine_matrix",  test_validate_engine_matrix,  ix_setup, ix_teardown },
    { "idxval/validate_long_cluster",   test_validate_long_cluster,   ix_setup, ix_teardown },
    { "idxval/reject_part_snan",        test_reject_part_snan,        ix_setup, ix_teardown },
    { "idxval/parse_hash_probes",       test_parse_hash_probes,       ix_setup, ix_teardown },
    { "idxval/parse_part_order_sym",    test_parse_part_order_sym,    ix_setup, ix_teardown },
    { "idxval/parse_oom_frees",         test_parse_oom_frees,         ix_setup, ix_teardown },
    { "idxval/parse_rejects_view",      test_parse_rejects_view,      ix_setup, ix_teardown },
    { NULL, NULL, NULL, NULL },
};
