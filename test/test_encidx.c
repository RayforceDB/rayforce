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
#include "store/encw.h"
#include "store/col.h"
#include "store/colrange.h"
#include "table/table.h"   /* ray_table_col_index */
#include "core/platform.h" /* ray_thread_create */
#include "store/splay.h"     /* ray_splay_resolve_dir */
#include "ops/ops.h"         /* ray_graph_new: the unsplit eq+range conjunction */
#include "lang/env.h"       /* ray_env_get */
#include "mem/cow.h"       /* ray_rc_sync */
#include "lang/format.h"    /* ray_fmt */
#include <math.h>
#include <stddef.h>
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
    /* the region holds the generation-1 layout of the index (I64 arrays,
     * groups by first occurrence, low-bits homes), whatever its builder */
    ray_t *lt, *lg, *lo, *lr;
    TEST_ASSERT_TRUE(ray_index_hash_legacy(ix, &lt, &lg, &lo, &lr));
    TEST_ASSERT_TRUE(memcmp(b + h.table, ray_data(lt), cap * 8) == 0);
    TEST_ASSERT_TRUE(memcmp(b + h.gkeys, ray_data(lg), 3 * 8) == 0);
    TEST_ASSERT_TRUE(memcmp(b + h.offs, ray_data(lo), 4 * 8) == 0);
    TEST_ASSERT_TRUE(memcmp(b + h.rows, ray_data(lr), 6 * 8) == 0);
    ray_release(lt); ray_release(lg); ray_release(lo); ray_release(lr);
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
    if (r[0]) ray_enc_index_view(r[0], n[0], RAY_IDX_HASH, RAY_I64, 0, N, &iv);
    bool valid = r[0] && iv.order_len == 2;
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

/* ===== Reading a region (ray_enc_index_view) ===== */

/* The view of the region of v's index. */
static void ix_view(const uint8_t* b, size_t n, ray_t* v, ray_enc_idx_view_t* out) {
    ray_index_t* ix = ray_index_payload(v->index);
    ray_enc_index_view(b, n, ix->kind, v->type, v->attrs, v->len, out);
}

/* The view of a written region names its fields. */
static test_result_t test_view_fields(void) {
    int64_t d[3000];
    for (int i = 0; i < 3000; i++) d[i] = (i * 31) % 997;
    ray_t* (*kinds[3])(ray_t**) = { ray_index_attach_hash, ray_index_attach_sort, ray_index_attach_bloom };
    for (int k = 0; k < 3; k++) {
        ray_t* v = with_index(i64_vec(d, 3000), kinds[k]);
        TEST_ASSERT_NOT_NULL(v);
        size_t n; ray_t* err; uint8_t* b = ix_region(v, NULL, 0, &n, &err);
        TEST_ASSERT_NOT_NULL(b);
        ray_enc_idx_view_t iv;
        ix_view(b, n, v, &iv);
        TEST_ASSERT_TRUE(iv.base == b && iv.size == n && iv.len == 3000 && iv.order_name == NULL);
        free(b); ray_release(v);
    }
    int64_t s[3000];
    for (int i = 0; i < 3000; i++) s[i] = i / 100;
    ray_t* p = with_index(i64_vec(s, 3000), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(p);
    size_t n; ray_t* err; uint8_t* b = ix_region(p, "ts", 2, &n, &err);
    ray_enc_idx_view_t iv;
    ix_view(b, n, p, &iv);
    TEST_ASSERT_EQ_I(iv.order_len, 2);
    TEST_ASSERT_TRUE(memcmp(iv.order_name, "ts", 2) == 0);
    TEST_ASSERT_EQ_I(iv.part.n_parts, 30);
    free(b); ray_release(p);
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
    ray_enc_index_view(b, n, RAY_IDX_HASH, RAY_STR, 0, 3, &iv);
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

/* Empty columns: every kind's region over zero rows is written and read. */
static test_result_t test_view_empty(void) {
    ray_t* (*kinds[4])(ray_t**) = { ray_index_attach_hash, ray_index_attach_sort,
                                    ray_index_attach_bloom, ray_index_attach_part };
    for (int k = 0; k < 4; k++) {
        ray_t* v = with_index(ray_vec_new(RAY_I64, 0), kinds[k]);
        TEST_ASSERT_NOT_NULL(v);
        size_t n; ray_t* err; uint8_t* b = ix_region(v, NULL, 0, &n, &err);
        TEST_ASSERT_NULL(err);
        TEST_ASSERT_NOT_NULL(b);
        ray_enc_idx_view_t iv;
        ix_view(b, n, v, &iv);
        free(b); ray_release(v);
    }
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
    for (size_t i = 0; i < n; i++)
        if (!used[i]) TEST_ASSERT_FMT(b[i] == 0, "byte %zu of %zu is %u", i, n, b[i]);
    free(used); free(b);
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
/* The index a region holds for source index `a`: itself, except a HASH,
 * which a region stores in the generation-1 layout (ray_index_hash_legacy:
 * I64 arrays, groups by first occurrence, low-bits homes).  *out is a copy
 * of `a` with those arrays (hold[] owns them; ix_region_view_free). */
static const ray_index_t* ix_region_view(const ray_index_t* a, ray_index_t* out, ray_t* hold[4]) {
    for (int i = 0; i < 4; i++) hold[i] = NULL;
    if (a->kind != RAY_IDX_HASH) return a;
    *out = *a;
    if (!ray_index_hash_legacy(a, &hold[0], &hold[1], &hold[2], &hold[3])) return a;
    out->u.hash.table = hold[0]; out->u.hash.gkeys = hold[1];
    out->u.hash.offs = hold[2]; out->u.hash.rows = hold[3];
    out->markers &= (uint8_t)~RAY_MARK_HASH_HIGH;
    return out;
}
static void ix_region_view_free(ray_t* hold[4]) {
    for (int i = 0; i < 4; i++) if (hold[i]) ray_release(hold[i]);
}
static test_result_t ix_parse_same(ray_t* v, const ray_enc_idx_view_t* iv, const char* name, const char* what) {
    ray_t* root = NULL;
    ray_t* e = ray_enc_index_parse(iv, v, &root);
    if (e) ray_error_free(e);
    TEST_ASSERT_FMT(e == NULL && root != NULL, "%s: parse failed", what);
    ray_index_t a_view; ray_t* a_hold[4];
    const ray_index_t* a = ix_region_view(ray_index_payload(v->index), &a_view, a_hold);
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
    ix_region_view_free(a_hold);
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
static test_result_t test_view_engine_matrix(void) {
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
                ix_view(b, n, v, &iv);
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
                ix_view(b, rn, v, &iv);
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
        ix_view(b, n, v, &iv);
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
        ix_view(b, n, v, &iv);
        PARSE_SAME(v, &iv, t == 2 ? NULL : "ts", "carried HASH type %d", t);
        free(b); ray_release(v); built++;
    }
    TEST_ASSERT_FMT(built >= 150, "only %d indexes built", built);
    PASS();
}

/* ===== Parse round trips ===== */

/* Region of v's index -> validate -> parse -> attach to a fresh copy of v. */
static ray_t* ix_roundtrip(ray_t* v, const char* name, uint32_t nlen) {
    size_t n; ray_t* err; uint8_t* b = ix_region(v, name, nlen, &n, &err);
    if (!b) { if (err) ray_error_free(err); return NULL; }
    ray_enc_idx_view_t iv;
    ray_t* fresh = ray_vec_new(v->type, v->len);
    if (v->type == RAY_SYM) { ray_release(fresh); fresh = ray_sym_vec_new(v->attrs & RAY_SYM_W_MASK, v->len); ray_sym_vec_adopt_domain(fresh, v); }
    fresh->len = v->len;
    memcpy(ray_data(fresh), ray_data(v), (size_t)v->len * ray_sym_elem_size(v->type, v->attrs));
    ix_view(b, n, v, &iv);
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
    ray_t* p = ix_roundtrip(v, NULL, 0);
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
    ray_t* p = ix_roundtrip(v, "ordcol_p4", 9);
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
    ray_t* q = ix_roundtrip(sp, NULL, 0);
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
    ray_t* fq = ix_roundtrip(fp, NULL, 0);
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
static test_result_t ix_parse_oom(ray_t* v, const char* name) {
    size_t n; ray_t* err; uint8_t* b = ix_region(v, name, name ? (uint32_t)strlen(name) : 0, &n, &err);
    if (err) ray_error_free(err);
    TEST_ASSERT_NOT_NULL(b);
    ray_enc_idx_view_t iv;
    ix_view(b, n, v, &iv);
    if (name) (void)ray_sym_intern(name, strlen(name));        /* interned before the baseline */
    test_result_t r = ix_parse_oom_run(v, &iv);
    free(b);
    return r;
}
#define PARSE_OOM(v, name) do { test_result_t r_ = ix_parse_oom((v), (name)); \
    if (r_.status != TEST_PASS) return r_; } while (0)

static test_result_t test_parse_oom_frees(void) {
    int64_t d[40];
    for (int i = 0; i < 40; i++) d[i] = (i * 7) % 11;
    ray_t* (*kinds[3])(ray_t**) = { ray_index_attach_hash, ray_index_attach_sort, ray_index_attach_bloom };
    for (int k = 0; k < 3; k++) {
        ray_t* v = with_index(i64_vec(d, 40), kinds[k]);
        TEST_ASSERT_NOT_NULL(v);
        PARSE_OOM(v, k == 0 ? "ts" : NULL);
        ray_release(v);
    }
    int64_t pd[6] = { 1, 1, 3, 3, 3, 8 };
    ray_t* p = with_index(i64_vec(pd, 6), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(p);
    PARSE_OOM(p, "ts");
    ray_release(p);
    ray_t* s = ray_sym_vec_new(RAY_SYM_W16, 5);
    s->len = 5;
    uint16_t ids[5] = { 4, 4, 9, 2, 2 };
    memcpy(ray_data(s), ids, sizeof ids);
    ray_t* sp = with_index(s, ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(sp);
    PARSE_OOM(sp, "ts");
    ray_release(sp);
    PASS();
}

/* ---- the column writer emits the region (Task 5) ---- */

/* The region of a written column file and its enc header, or NULL when the
 * file has none; *fn is the file's size (0 when unreadable), which callers
 * check against end_off. */
static uint8_t* ix_file_region(const char* path, size_t* rn, ray_enc_hdr_t* eh, uint64_t* fn) {
    size_t n = 0; uint8_t* b = ix_slurp(path, &n);
    memset(eh, 0, sizeof *eh);
    *fn = b ? n : 0;
    if (!b) return NULL;
    if (n < 96) { free(b); return NULL; }
    memcpy(eh, b + 32, 64);
    if (!eh->index_off || eh->index_off >= n) { free(b); return NULL; }
    *rn = n - eh->index_off;
    uint8_t* r = malloc(*rn);
    memcpy(r, b + eh->index_off, *rn);
    free(b);
    return r;
}

/* A HASH-indexed column is written as generation 2 with its region. */
static test_result_t test_file_has_region(void) {
    int64_t d[3000];
    for (int i = 0; i < 3000; i++) d[i] = (i * 13) % 211;
    ray_t* v = with_index(i64_vec(d, 3000), ray_index_attach_hash);
    TEST_ASSERT_NOT_NULL(v);
    const char* p = ray_test_tmp(IDX_TMP "/h");
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    TEST_ASSERT_EQ_U(ray_col_file_gen(p), 2);
    size_t rn = 0; ray_enc_hdr_t eh;
    uint64_t fn = 0;
    uint8_t* r = ix_file_region(p, &rn, &eh, &fn);
    TEST_ASSERT_TRUE(fn > 0 && eh.end_off == fn);              /* the header covers the whole file */
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_HASH);
    TEST_ASSERT_EQ_U(eh.index_off % 32, 0);
    size_t n2 = 0; ray_t* err = NULL; uint8_t* direct = ix_region(v, NULL, 0, &n2, &err);
    bool same = direct && n2 == rn && memcmp(direct, r, rn) == 0;
    free(direct); free(r); ray_release(v);
    TEST_ASSERT_FMT(same, "the file's region differs from the region writer's (%zu vs %zu bytes)", rn, n2);
    PASS();
}

/* UNIQUE is a header flag, never in the region; a stale root writes none. */
static test_result_t test_unique_and_stale(void) {
    int64_t d[100]; for (int i = 0; i < 100; i++) d[i] = i;
    ray_t* v = with_index(i64_vec(d, 100), ray_index_attach_hash);
    TEST_ASSERT_NOT_NULL(v);
    ray_index_payload(v->index)->markers |= RAY_MARK_UNIQUE;
    size_t rn = 0, dn = 0; ray_enc_hdr_t eh;
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(IDX_TMP "/u"), 2), RAY_OK);
    uint64_t fn = 0;
    uint8_t* r = ix_file_region(ray_test_tmp(IDX_TMP "/u"), &rn, &eh, &fn);
    TEST_ASSERT_TRUE(fn > 0 && eh.end_off == fn);              /* the header covers the whole file */
    ray_t* err = NULL; uint8_t* direct = ix_region(v, NULL, 0, &dn, &err);   /* markers are not on the wire */
    bool same = r && direct && rn == dn && memcmp(r, direct, rn) == 0;
    free(r); free(direct);
    TEST_ASSERT_TRUE(same);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_HASH);
    TEST_ASSERT_EQ_I(eh.col_flags, RAY_ENC_FLAG_UNIQUE);
    ray_index_payload(v->index)->built_for_len = 99;           /* stale */
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(IDX_TMP "/s"), 2), RAY_OK);
    r = ix_file_region(ray_test_tmp(IDX_TMP "/s"), &rn, &eh, &fn);
    TEST_ASSERT_TRUE(fn > 0 && eh.end_off == fn);              /* the header covers the whole file */
    ray_index_payload(v->index)->built_for_len = 100;
    ray_release(v);
    TEST_ASSERT_NULL(r);
    TEST_ASSERT_EQ_U(ray_col_file_gen(ray_test_tmp(IDX_TMP "/s")), 2);
    TEST_ASSERT_EQ_I(eh.index_kind, 0);
    TEST_ASSERT_EQ_I(eh.col_flags, 0);
    PASS();
}

/* Spec "Writing the index": a child of a type the slot does not allow fails
 * the SAVE with nyi naming the kind, the message reaches the caller's error
 * slot with the "column %s:" prefix, and nothing is published or left. */
static test_result_t test_save_nyi_names_kind(void) {
    int64_t d[3] = { 1, 2, 3 };
    ray_t* v = with_index(i64_vec(d, 3), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(v);
    ray_index_t* ix = ray_index_payload(v->index);
    ray_t* keys = ix->u.part.keys;
    ray_t* fake = ray_vec_new(RAY_STR, 3);
    for (int i = 0; i < 3; i++) fake = ray_str_vec_append(fake, "k", 1);
    ix->u.part.keys = fake;
    const char* p = ray_test_tmp(IDX_TMP "/nyi");
    ray_error_clear();
    ray_err_t e = ray_col_save_gen(v, p, 2);
    const char* m = ray_error_msg();
    char msg[256]; snprintf(msg, sizeof msg, "%s", m ? m : "");
    ix->u.part.keys = keys;
    ray_release(fake); ray_release(v);
    TEST_ASSERT_EQ_I(e, RAY_ERR_NYI);
    TEST_ASSERT_FMT(strstr(msg, "PART") != NULL, "message: %s", msg);
    char want[300]; snprintf(want, sizeof want, "column %s: index region:", p);
    TEST_ASSERT_FMT(strncmp(msg, want, strlen(want)) == 0, "message: %s", msg);
    FILE* f = fopen(p, "rb");                                 TEST_ASSERT_NULL(f);
    f = fopen(ray_test_tmp(IDX_TMP "/nyi.tmp"), "rb");        TEST_ASSERT_NULL(f);
    PASS();
}

/* An override kind never re-labels a region root: the file records the
 * override and carries no region. */
static test_result_t test_save_override_kind(void) {
    int64_t d[3000];
    for (int i = 0; i < 3000; i++) d[i] = (i * 13) % 211;
    ray_t* v = with_index(i64_vec(d, 3000), ray_index_attach_sort);
    TEST_ASSERT_NOT_NULL(v);
    ray_col_write_t w; memset(&w, 0, sizeof w);
    ray_t* err = ray_col_src_bind(v, &w.src);
    TEST_ASSERT_NULL(err);
    w.gen = 2; w.index_kind = RAY_IDX_CHUNK_ZONE;
    ray_err_t e = ray_col_save_ctx(&w, ray_test_tmp(IDX_TMP "/o"));
    ray_col_src_release(&w.src);
    size_t rn; ray_enc_hdr_t eh;
    uint64_t fn = 0;
    uint8_t* r = ix_file_region(ray_test_tmp(IDX_TMP "/o"), &rn, &eh, &fn);
    TEST_ASSERT_TRUE(fn > 0 && eh.end_off == fn);              /* the header covers the whole file */
    ray_release(v);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_NULL(r);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_CHUNK_ZONE);
    PASS();
}

/* Golden index regions (spec "Golden fixtures": one per persisted kind).
 * Compile with ENC_GOLDEN_KEEP to regenerate them into the fixture
 * directory (relative to the repository root, where the tests run). */
#define IDX_GOLDEN_DIR "test/data/enc"
#ifdef ENC_GOLDEN_KEEP
#  define IDX_GOLDEN_OUT IDX_GOLDEN_DIR
#else
#  define IDX_GOLDEN_OUT ray_test_tmp(IDX_TMP "/golden_emit")
#endif
static const char* const g_idx_golden[] = {
    "idx_hash_i64", "idx_hash_str", "idx_hash_sym", "idx_sort_i32", "idx_bloom_i64",
    "idx_part_i64", "idx_part_f64", "idx_part_sym", "idx_part_order", "idx_hash_unique",
    "idx_dict_str", NULL,
};

/* SYM fixtures go through the column writer directly, like P1's golden_sym,
 * so the saved count does not depend on the process symbol table. */
static ray_err_t idx_golden_sym(const char* dir, const char* name, uint8_t kind) {
    ray_t* v = ray_sym_vec_new(RAY_SYM_W8, 600);
    if (!v || RAY_IS_ERR(v)) return RAY_ERR_OOM;
    v->len = 600;
    uint8_t* ids = (uint8_t*)ray_data(v);
    for (int k = 0; k < 600; k++)
        ids[k] = kind == RAY_IDX_PART ? (uint8_t)(1 + k / 100) : (uint8_t)(1 + (k * 7) % 40);
    ray_t* iv = with_index(v, kind == RAY_IDX_PART ? ray_index_attach_part : ray_index_attach_hash);
    if (!iv) return RAY_ERR_TYPE;
    char path[512]; snprintf(path, sizeof path, "%s/%s.bin", dir, name);
    FILE* f = fopen(path, "wb+");
    if (!f) { ray_release(iv); return RAY_ERR_IO; }
    ray_encw_t w;
    ray_err_t e = ray_encw_begin(&w, f, RAY_SYM, RAY_SYM_W8, 600, 64, 0, 0);
    ray_enc_idx_src_t s = { iv->index, RAY_SYM, RAY_SYM_W8, 600, NULL, 0 };
    if (e == RAY_OK) { ray_encw_set_index(&w, &s); e = ray_encw_chunk(&w, ray_data(iv), NULL, 600); }
    if (e == RAY_OK) e = ray_encw_finish(&w);
    ray_encw_free(&w); fclose(f); ray_release(iv);
    return e;
}

static const char* idx_golden_emit(const char* dir) {
    static char why[256];
    (void)ray_test_mkdir_p(dir);
    char path[512];
    #define GOLD(name, vec) do { ray_t* gv_ = (vec); snprintf(path, sizeof path, "%s/%s.bin", dir, name); \
        ray_err_t ge_ = gv_ ? ray_col_save_gen(gv_, path, 2) : RAY_ERR_TYPE; \
        if (gv_) ray_release(gv_); \
        if (ge_ != RAY_OK) { ray_encw_test_force(-1); snprintf(why, sizeof why, "%s: %d", name, (int)ge_); return why; } } while (0)
    int64_t a[1000]; for (int i = 0; i < 1000; i++) a[i] = (i * 7919) % 317;
    GOLD("idx_hash_i64", with_index(i64_vec(a, 1000), ray_index_attach_hash));
    ray_t* s = ray_vec_new(RAY_STR, 300);
    for (int i = 0; i < 300; i++) { char b[32]; int n = snprintf(b, sizeof b, "string key number %d", i % 23); s = ray_str_vec_append(s, b, (size_t)n); }
    ray_encw_test_force(RAY_ENC_RAW);                       /* P6: keep the P4 fixture's column bytes */
    GOLD("idx_hash_str", with_index(s, ray_index_attach_hash));
    ray_encw_test_force(-1);
    int32_t c[700]; for (int i = 0; i < 700; i++) c[i] = (int32_t)((i * 31) % 499) - 250;
    GOLD("idx_sort_i32", with_index(ray_vec_from_raw(RAY_I32, c, 700), ray_index_attach_sort));
    GOLD("idx_bloom_i64", with_index(i64_vec(a, 1000), ray_index_attach_bloom));
    int64_t p[1000]; for (int i = 0; i < 1000; i++) p[i] = i / 128 * 3;
    GOLD("idx_part_i64", with_index(i64_vec(p, 1000), ray_index_attach_part));
    double f[8] = { -2.5, -0.0, 0.0, 0.0, 1.5, 1.5, 3.0, 3.0 };
    ray_encw_test_force(RAY_ENC_RAW);                       /* P5: keep the P4 fixture's column bytes */
    GOLD("idx_part_f64", with_index(ray_vec_from_raw(RAY_F64, f, 8), ray_index_attach_part));
    ray_encw_test_force(-1);
    ray_t* o = with_index(i64_vec(p, 1000), ray_index_attach_part);
    if (o) ray_index_payload(o->index)->u.part.order_sym = ray_sym_intern("ts", 2);
    GOLD("idx_part_order", o);
    ray_t* u = with_index(i64_vec(a, 1000), ray_index_attach_hash);
    if (u) ray_index_payload(u->index)->markers |= RAY_MARK_UNIQUE;
    GOLD("idx_hash_unique", u);
    #undef GOLD
    if (idx_golden_sym(dir, "idx_hash_sym", RAY_IDX_HASH) != RAY_OK) return "idx_hash_sym";
    if (idx_golden_sym(dir, "idx_part_sym", RAY_IDX_PART) != RAY_OK) return "idx_part_sym";
    {   /* DICT (P6b): 300 rows through the column writer; the 65536-row floor
         * is the writers' decision, not a validity rule (D14) */
        ray_t* ds = ray_vec_new(RAY_STR, 300);
        for (int i = 0; i < 300 && ds && !RAY_IS_ERR(ds); i++) {
            char b[32]; int n = snprintf(b, sizeof b, "dict entry %d", (i * 7) % 23);
            ds = ray_str_vec_append(ds, b, (size_t)n);
        }
        if (!ds || RAY_IS_ERR(ds)) return "idx_dict_str";
        snprintf(path, sizeof path, "%s/idx_dict_str.bin", dir);
        FILE* f = fopen(path, "wb+");
        ray_encw_t w; memset(&w, 0, sizeof w);
        ray_encw_test_force_str(RAY_ENC_RAW);                  /* the subject is the region */
        ray_err_t e = f ? ray_encw_begin(&w, f, RAY_STR, 0, 300, 0, RAY_IDX_DICT, 0) : RAY_ERR_IO;
        const ray_t* own = ds;
        const char* pool = own->str_pool ? (const char*)ray_data(own->str_pool) : NULL;
        if (e == RAY_OK) e = ray_encw_chunk(&w, ray_data(ds), pool, 300);
        if (e == RAY_OK) e = ray_encw_finish(&w);
        ray_encw_test_force_str(-1);
        if (f) { ray_encw_free(&w); fclose(f); }
        ray_release(ds);
        if (e != RAY_OK) return "idx_dict_str";
    }
    return NULL;
}

static test_result_t test_idx_golden_emit(void) {
    const char* why = idx_golden_emit(IDX_GOLDEN_OUT);
    TEST_ASSERT_FMT(why == NULL, "%s", why);
    PASS();
}

static test_result_t test_idx_golden_match(void) {
    const char* why = idx_golden_emit(ray_test_tmp(IDX_TMP "/golden"));
    TEST_ASSERT_FMT(why == NULL, "%s", why);
    for (int i = 0; g_idx_golden[i]; i++) {
        char a[512], b[512];
        snprintf(a, sizeof a, "%s/%s.bin", IDX_GOLDEN_DIR, g_idx_golden[i]);
        snprintf(b, sizeof b, "%s/golden/%s.bin", ray_test_tmp(IDX_TMP), g_idx_golden[i]);
        size_t na = 0, nb = 0; uint8_t* pa = ix_slurp(a, &na); uint8_t* pb = ix_slurp(b, &nb);
        bool have = pa != NULL;
        bool same = pa && pb && na == nb && memcmp(pa, pb, na) == 0;
        free(pa); free(pb);
        TEST_ASSERT_FMT(have, "missing fixture %s", a);
        TEST_ASSERT_FMT(same, "%s differs from its fixture", g_idx_golden[i]);
    }
    PASS();
}

/* ---- materialization attaches the parsed index (Task 6) ---- */

static ray_t* ix_hash_col(int64_t n) {
    ray_t* v = ray_vec_new(RAY_I64, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = (i * 2654435761u) % 10007;
    return with_index(v, ray_index_attach_hash);
}
/* The enc header of a written column file. */
static ray_enc_hdr_t ix_hdr(const char* path) {
    ray_enc_hdr_t eh; memset(&eh, 0, sizeof eh);
    size_t n; uint8_t* b = ix_slurp(path, &n);
    if (b && n >= 96) memcpy(&eh, b + 32, 64);
    free(b);
    return eh;
}

/* Spec "Golden fixtures": the reader decodes them.  Each committed
 * test/data/enc/idx_*.bin validates and parses from its own header (type,
 * attrs, len, SYM bound) and loads as a column whose materialization
 * attaches the same index: kind, rows, order name, UNIQUE. */
static test_result_t test_idx_golden_read(void) {
    static const struct { const char* name; uint8_t kind; const char* order; bool unique; } fx[] = {
        { "idx_hash_i64",    RAY_IDX_HASH,  NULL, false }, { "idx_hash_str",  RAY_IDX_HASH,  NULL, false },
        { "idx_hash_sym",    RAY_IDX_HASH,  NULL, false }, { "idx_sort_i32",  RAY_IDX_SORT,  NULL, false },
        { "idx_bloom_i64",   RAY_IDX_BLOOM, NULL, false }, { "idx_part_i64",  RAY_IDX_PART,  NULL, false },
        { "idx_part_f64",    RAY_IDX_PART,  NULL, false }, { "idx_part_sym",  RAY_IDX_PART,  NULL, false },
        { "idx_part_order",  RAY_IDX_PART,  "ts", false }, { "idx_hash_unique", RAY_IDX_HASH, NULL, true },
        { "idx_dict_str",    RAY_IDX_DICT,  NULL, false },
    };
    size_t nfx = sizeof fx / sizeof fx[0];
    /* the SYM fixtures record 64 symbols: a process with fewer refuses them */
    for (int i = 0; i < 64; i++) { char nb[8]; int nn = snprintf(nb, sizeof nb, "g%d", i); (void)ray_sym_intern(nb, (size_t)nn); }
    for (int i = 0; g_idx_golden[i]; i++) TEST_ASSERT_TRUE((size_t)i < nfx && strcmp(fx[i].name, g_idx_golden[i]) == 0);
    for (size_t i = 0; i < nfx; i++) {
        char path[512]; snprintf(path, sizeof path, "%s/%s.bin", IDX_GOLDEN_DIR, fx[i].name);
        /* the column: loads, materializes, attaches */
        ray_t* c = ray_col_mmap(path);
        TEST_ASSERT_FMT(c && !RAY_IS_ERR(c) && c->type == RAY_ENCCOL, "%s: load", fx[i].name);
        ray_error_clear();
        ray_t* m = ray_enccol_materialize(c);
        TEST_ASSERT_FMT(m && !RAY_IS_ERR(m), "%s: %s", fx[i].name, ray_error_msg() ? ray_error_msg() : "?");
        TEST_ASSERT_FMT(ray_index_kind(m) == fx[i].kind, "%s: attached kind %d", fx[i].name, (int)ray_index_kind(m));
        ray_index_t* aix = ray_index_payload(m->index);
        TEST_ASSERT_EQ_I(aix->built_for_len, m->len);
        TEST_ASSERT_FALSE(aix->markers & RAY_MARK_MMAP);
        TEST_ASSERT_FMT(((aix->markers & RAY_MARK_UNIQUE) != 0) == fx[i].unique, "%s: UNIQUE", fx[i].name);
        /* the region alone, from the file's own header */
        size_t n = 0; uint8_t* b = ix_slurp(path, &n);
        TEST_ASSERT_TRUE(b && n >= 96);
        ray_t h; memcpy(&h, b, 32);
        uint32_t saved; memcpy(&saved, b + offsetof(ray_t, rc), 4);
        ray_enc_hdr_t eh; memcpy(&eh, b + 32, 64);
        TEST_ASSERT_TRUE(eh.end_off == n && eh.index_off > 0 && eh.index_off < n);
        TEST_ASSERT_EQ_I(eh.index_kind, fx[i].kind);
        TEST_ASSERT_EQ_I(h.type, m->type);
        TEST_ASSERT_EQ_I(h.len, m->len);
        if (fx[i].kind == RAY_IDX_DICT) {      /* its own reader, not ray_enc_index_view */
            const uint64_t rsz = eh.end_off - eh.index_off;
            uint8_t* rb = aligned_alloc(32, (size_t)((rsz + 31) & ~(uint64_t)31));
            TEST_ASSERT_NOT_NULL(rb);
            memcpy(rb, b + eh.index_off, (size_t)rsz);
            free(b);
            ray_enc_dict_view_t dv;
            ray_enc_dict_parse(rb, &dv);
            const int64_t nd = dv.n_distinct, dl = dv.len;
            free(rb);
            TEST_ASSERT_EQ_I(dl, 300);
            TEST_ASSERT_EQ_I(nd, 23);
            TEST_ASSERT_EQ_I(aix->u.dict.n_distinct, 23);
            ray_release(c);
            continue;
        }
        ray_enc_idx_view_t iv; ray_t* root = NULL;
        ray_enc_index_view(b + eh.index_off, n - eh.index_off, eh.index_kind, h.type, h.attrs, h.len, &iv);
        ray_t* e = ray_enc_index_parse(&iv, m, &root);
        free(b);
        TEST_ASSERT_FMT(e == NULL && root, "%s: parse", fx[i].name);
        ray_index_t* pix = ray_index_payload(root);
        bool ok = pix->kind == fx[i].kind && pix->built_for_len == h.len && pix->parent_type == h.type;
        int64_t osym = pix->kind == RAY_IDX_PART ? pix->u.part.order_sym
                     : pix->kind == RAY_IDX_HASH ? pix->u.hash.order_sym : -1;
        int64_t asym = aix->kind == RAY_IDX_PART ? aix->u.part.order_sym
                     : aix->kind == RAY_IDX_HASH ? aix->u.hash.order_sym : -1;
        ray_release(root);
        TEST_ASSERT_FMT(ok, "%s: parsed root", fx[i].name);
        TEST_ASSERT_EQ_I(osym, asym);
        if (fx[i].order) {
            TEST_ASSERT_TRUE(osym >= 0);
            ray_t* nm = ray_sym_str(osym);
            TEST_ASSERT_TRUE(nm && !RAY_IS_ERR(nm) && ray_str_len(nm) == strlen(fx[i].order) &&
                             memcmp(ray_str_ptr(nm), fx[i].order, ray_str_len(nm)) == 0);
        } else {
            TEST_ASSERT_FMT(osym < 0, "%s: order_sym %lld", fx[i].name, (long long)osym);
        }
        ray_release(c);
    }
    PASS();
}

static test_result_t test_materialize_attaches(void) {
    const char* p = ray_test_tmp(IDX_TMP "/m");
    ray_t* v = ix_hash_col(70000);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->type == RAY_ENCCOL);
    ray_enc_counters_reset();
    ray_t* m = ray_enccol_materialize(c);                           /* borrowed */
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_HASH);
    ray_t* sa = ray_index_hash_eq_rowsel(v, 4242);
    ray_t* sb = ray_index_hash_eq_rowsel(m, 4242);
    TEST_ASSERT_TRUE(sa && sb && ray_rowsel_meta(sa)->total_pass == ray_rowsel_meta(sb)->total_pass);
    ray_rowsel_release(sa); ray_rowsel_release(sb);
    TEST_ASSERT_FALSE(ray_index_payload(m->index)->markers & RAY_MARK_MMAP);
    TEST_ASSERT_FALSE(ray_index_payload(m->index)->markers & RAY_MARK_UNIQUE);
    ray_release(c); ray_release(v);
    PASS();
}

/* Every region kind round-trips through a file: the materialized column's
 * index writes the same region as the source's (the wire is normalized),
 * and SORT / PART / BLOOM probes agree. */
static test_result_t test_materialize_all_kinds(void) {
    ray_t* (*const kinds[])(ray_t**) = { ray_index_attach_hash, ray_index_attach_sort,
                                         ray_index_attach_bloom, ray_index_attach_part };
    const int want[] = { RAY_IDX_HASH, RAY_IDX_SORT, RAY_IDX_BLOOM, RAY_IDX_PART };
    static int64_t d[70000];
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 70000; i++)                 /* PART: ascending value-blocks */
            d[i] = want[k] == RAY_IDX_PART ? (i / 700) * 3 - 50 : (int64_t)((i * 2654435761u) % 10007) - 5000;
        const char* p = ray_test_tmp(IDX_TMP "/k");
        ray_t* v = with_index(i64_vec(d, 70000), kinds[k]);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
        TEST_ASSERT_EQ_I(ix_hdr(p).index_kind, want[k]);
        ray_t* c = ray_col_mmap(p);
        TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->type == RAY_ENCCOL);
        ray_t* m = ray_enccol_materialize(c);
        TEST_ASSERT_FMT(m && !RAY_IS_ERR(m), "kind %d: %s", want[k], ray_error_msg() ? ray_error_msg() : "?");
        TEST_ASSERT_EQ_I(ray_index_kind(m), want[k]);
        TEST_ASSERT_FALSE(ray_index_payload(m->index)->markers & RAY_MARK_MMAP);
        size_t na = 0, nb = 0; ray_t* ea = NULL; ray_t* eb = NULL;
        uint8_t* ra = ix_region(v, NULL, 0, &na, &ea);
        uint8_t* rb = ix_region(m, NULL, 0, &nb, &eb);
        bool same = ra && rb && na == nb && memcmp(ra, rb, na) == 0;
        free(ra); free(rb);
        if (ea) ray_error_free(ea);
        if (eb) ray_error_free(eb);
        TEST_ASSERT_FMT(same, "kind %d: the materialized index differs from the source's", want[k]);
        TEST_ASSERT_EQ_I(memcmp(ray_data(m), d, sizeof d), 0);
        ray_release(c); ray_release(v);
    }
    PASS();
}

/* Carry: UNIQUE comes from col_flags and is added by the materializer
 * after the parse (the parse leaves it to the caller). */
static test_result_t test_materialize_unique(void) {
    int64_t d[3000]; for (int i = 0; i < 3000; i++) d[i] = 3000 - i;
    ray_t* v = with_index(i64_vec(d, 3000), ray_index_attach_hash);
    TEST_ASSERT_NOT_NULL(v);
    ray_index_payload(v->index)->markers |= RAY_MARK_UNIQUE;
    const char* p = ray_test_tmp(IDX_TMP "/uq");
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    ray_enc_hdr_t eh = ix_hdr(p);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_HASH);
    TEST_ASSERT_EQ_I(eh.col_flags, RAY_ENC_FLAG_UNIQUE);
    /* the parse alone leaves UNIQUE clear */
    size_t n; uint8_t* b = ix_slurp(p, &n);
    TEST_ASSERT_NOT_NULL(b);
    ray_enc_idx_view_t iv; ray_t* root = NULL;
    ray_enc_index_view(b + eh.index_off, n - eh.index_off, RAY_IDX_HASH, RAY_I64, v->attrs, 3000, &iv);
    ray_t* e = ray_enc_index_parse(&iv, v, &root);
    free(b);
    TEST_ASSERT_TRUE(!e && root);
    TEST_ASSERT_FALSE(ray_index_payload(root)->markers & RAY_MARK_UNIQUE);
    ray_release(root);
    /* the materializer adds it */
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_HASH);
    TEST_ASSERT_TRUE(ray_index_payload(m->index)->markers & RAY_MARK_UNIQUE);
    TEST_ASSERT_FALSE(ray_index_payload(m->index)->markers & RAY_MARK_MMAP);
    ray_release(c); ray_release(v);
    PASS();
}

/* Spec "Errors": injected allocation failure DURING INDEX PARSING is oom,
 * not cached, then succeeds; allocator accounting returns to baseline.  The
 * parse-only hook fails exactly the parse step (P1's nth-allocation hook
 * would trip at the start of materialization first). */
static test_result_t test_parse_oom_retry(void) {
    const char* p = ray_test_tmp(IDX_TMP "/o");
    ray_t* v = ix_hash_col(3000);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    ray_mem_stats_t m0, m1, m2; ray_mem_stats(&m0);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    for (int i = 0; i < 2; i++) {
        ray_enccol_test_fail_index_parse(true);
        ray_error_clear();
        ray_t* r = ray_enccol_materialize(c);
        ray_enccol_test_fail_index_parse(false);
        TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
        TEST_ASSERT_STR_EQ(ray_err_code(r), "oom");
        TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), "index parse") != NULL);
        TEST_ASSERT_TRUE(strstr(ray_error_msg(), p) != NULL);
        ray_error_free(r);
        /* The first attempt leaves caches owned by the column object (they
         * go with it, checked below); the second allocates nothing that
         * outlives it. */
        if (i == 0) ray_mem_stats(&m1);
        else { ray_mem_stats(&m2); TEST_ASSERT_EQ_U(m2.bytes_allocated, m1.bytes_allocated); }
    }
    ray_t* r = ray_enccol_materialize(c);                           /* not cached: the retry succeeds */
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && ray_index_kind(r) == RAY_IDX_HASH);
    ray_release(c);
    ray_mem_stats(&m2);
    TEST_ASSERT_EQ_U(m2.bytes_allocated, m0.bytes_allocated);       /* the column took everything with it */
    ray_release(v);
    PASS();
}

/* Every allocation of a HASH materialization, the parse's included, fails
 * in turn: each is oom, not cached, frees what it took; the parse's carry
 * the path (the nth hook reaches the parse through
 * ray_enccol_test_nth_alloc_fails).  The path hook fails the column at
 * "materialize", before the region. */
static test_result_t test_materialize_oom_sweep(void) {
    const char* p = ray_test_tmp(IDX_TMP "/sw");
    ray_t* v = ix_hash_col(3000);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    ray_mem_stats_t base, m0, m1; ray_mem_stats(&base);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_test_fail_index_parse(true);                          /* warm the column's caches */
    ray_t* w = ray_enccol_materialize(c);
    ray_enccol_test_fail_index_parse(false);
    TEST_ASSERT_TRUE(w && RAY_IS_ERR(w));
    ray_error_free(w);
    ray_mem_stats(&m0);
    bool parse_hit = false, done = false;
    for (int64_t nth = 1; nth <= 400 && !done; nth++) {
        ray_enccol_test_fail_alloc(nth);
        ray_error_clear();
        ray_t* r = ray_enccol_materialize(c);
        ray_enccol_test_fail_alloc(0);
        if (r && !RAY_IS_ERR(r)) { done = true; TEST_ASSERT_EQ_I(ray_index_kind(r), RAY_IDX_HASH); break; }
        TEST_ASSERT_TRUE(r != NULL);
        TEST_ASSERT_FMT(strcmp(ray_err_code(r), "oom") == 0, "nth %lld: %s", (long long)nth, ray_err_code(r));
        const char* msg = ray_error_msg();
        /* every parse failure names the column (the materializer's rewrap
         * of the parse's own path-less errors) */
        if (msg && strstr(msg, "index parse"))
            TEST_ASSERT_FMT(strstr(msg, p) != NULL, "nth %lld: %s names no path", (long long)nth, msg);
        if (msg && strstr(msg, "index parse") && strstr(msg, p)) parse_hit = true;
        ray_error_free(r);
        ray_mem_stats(&m1);
        TEST_ASSERT_FMT(m1.bytes_allocated == m0.bytes_allocated, "nth %lld leaked %lld bytes (%s)", (long long)nth,
                        (long long)m1.bytes_allocated - (long long)m0.bytes_allocated, msg ? msg : "?");
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(parse_hit);
    ray_release(c);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, base.bytes_allocated);
    /* the path hook fails this column's materialization as oom, not cached */
    c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enccol_test_fail_path("/sw");
    ray_t* r = ray_enccol_materialize(c);
    ray_enccol_test_fail_path(NULL);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "oom");
    ray_error_free(r);
    r = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && ray_index_kind(r) == RAY_IDX_HASH);
    ray_release(c);
    ray_release(v);
    PASS();
}

/* Spec "Lifetime": a column retained from ray_table_col survives the
 * table, index included. */
static test_result_t test_retained_index_outlives_table(void) {
    const char* p = ray_test_tmp(IDX_TMP "/rt");
    ray_t* v = ix_hash_col(70000);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    ray_t* sa = ray_index_hash_eq_rowsel(v, 4242);
    TEST_ASSERT_NOT_NULL(sa);
    int64_t want = ray_rowsel_meta(sa)->total_pass;
    ray_rowsel_release(sa);
    ray_release(v);
    int64_t sym = ray_sym_intern("k", 1);
    int64_t live = ray_enccol_test_live();
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->type == RAY_ENCCOL);
    ray_t* tbl = ray_table_add_col(ray_table_new(1), sym, c);
    ray_release(c);
    TEST_ASSERT_TRUE(tbl && !RAY_IS_ERR(tbl));
    ray_t* col = ray_table_col(tbl, sym);                            /* borrowed, materialized */
    TEST_ASSERT_TRUE(col && !RAY_IS_ERR(col));
    TEST_ASSERT_EQ_I(ray_index_kind(col), RAY_IDX_HASH);
    ray_retain(col);
    ray_release(tbl);                                                /* the table, the column object, the mapping */
    TEST_ASSERT_EQ_I(ray_enccol_test_live(), live);
    ray_t* sb = ray_index_hash_eq_rowsel(col, 4242);
    TEST_ASSERT_NOT_NULL(sb);
    TEST_ASSERT_EQ_I(ray_rowsel_meta(sb)->total_pass, want);
    ray_rowsel_release(sb);
    ray_release(col);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);
    PASS();
}

/* Spec "Concurrency": many threads on one fresh column, one materialization. */
typedef struct {
    ray_t* tbl; int64_t sym; ray_t* index; const char* code; int which;
    ray_sem_t* ready; ray_sem_t* go;         /* the start barrier */
    char msg[256];
} ix_thr_t;
/* Even threads ask for the index, odd ones for the plain column (spec
 * "Concurrency": ray_table_col_index and ray_table_col). */
static void ix_thr(void* p) {
    ix_thr_t* t = p;
    ray_rc_sync = true;                      /* thread-local: shared objects, atomic counts */
    ray_sem_signal(t->ready);                /* parked: every thread starts at once */
    ray_sem_wait(t->go);
    ray_t* e;
    if (t->which & 1) {
        ray_t* col = ray_table_col(t->tbl, t->sym);
        e = (col && RAY_IS_ERR(col)) ? col : NULL;
        t->index = (col && !RAY_IS_ERR(col)) ? col->index : NULL;
    } else {
        e = ray_table_col_index(t->tbl, t->sym, &t->index);
    }
    t->code = e ? (ray_err_code(e) && strcmp(ray_err_code(e), "corrupt") == 0 ? "corrupt" : "other") : "";
    snprintf(t->msg, sizeof t->msg, "%s", e && ray_error_msg() ? ray_error_msg() : "");   /* thread-local */
    if (e) ray_error_free(e);
    ray_heap_abandon();
}
static test_result_t ix_concurrent(void) {
    const char* p = ray_test_tmp(IDX_TMP "/t");
    ray_t* v = ix_hash_col(70000);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, p, 2), RAY_OK);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    int64_t sym = ray_sym_intern("k", 1);
    ray_t* tbl = ray_table_add_col(ray_table_new(1), sym, c);
    ray_release(c);
    ray_enc_counters_reset();
    ray_thread_t th[8]; ix_thr_t a[8];
    ray_sem_t ready, go;
    if (ray_sem_init(&ready, 0) != RAY_OK) { ray_release(tbl); ray_release(v); FAIL("sem"); }
    if (ray_sem_init(&go, 0) != RAY_OK) { ray_sem_destroy(&ready); ray_release(tbl); ray_release(v); FAIL("sem"); }
    int nt = 0;
    while (nt < 8) {
        a[nt] = (ix_thr_t){ .tbl = tbl, .sym = sym, .which = nt, .ready = &ready, .go = &go };
        if (ray_thread_create(&th[nt], ix_thr, &a[nt]) != RAY_OK) break;
        nt++;
    }
    for (int i = 0; i < nt; i++) ray_sem_wait(&ready);          /* every thread parked before any starts */
    for (int i = 0; i < nt; i++) ray_sem_signal(&go);
    for (int i = 0; i < nt; i++) ray_thread_join(th[i]);
    ray_sem_destroy(&ready); ray_sem_destroy(&go);
    if (nt < 8) { ray_release(tbl); ray_release(v); FAIL("thread create"); }
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_STR_EQ(a[i].code, "");
        TEST_ASSERT_TRUE(a[i].index == a[0].index && a[i].index != NULL);
    }
    ray_release(tbl); ray_release(v);
    PASS();
}
static test_result_t test_concurrent_index(void) {
    return ix_concurrent();
}


/* ---- generation-2 queries take the persisted index (Task 6b) ---- */

static ray_runtime_t* g_ixrt;
static void ixr_setup(void)    { g_ixrt = ray_runtime_create(0, NULL); (void)ray_test_rm_rf(ray_test_tmp(IDX_TMP)); (void)ray_test_mkdir_p(ray_test_tmp(IDX_TMP)); }
static void ixr_teardown(void) { ray_test_hooks_off(); (void)ray_test_rm_rf(ray_test_tmp(IDX_TMP)); ray_runtime_destroy(g_ixrt); }
static ray_t* ixr_eval(const char* src) {
    ray_t* r = ray_eval_str(ray_test_tmp(src));      /* every "/tmp" path moves under the process root */
    if (r && RAY_IS_ERR(r)) { fprintf(stderr, "%s: %s\n", src, ray_error_msg()); }
    return r;
}
static int64_t ixr_i64(const char* src) {
    ray_t* r = ixr_eval(src);
    int64_t x = (r && !RAY_IS_ERR(r)) ? r->i64 : INT64_MIN;
    if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
    return x;
}

/* The live file of column `col` in splayed directory `dir` (a "/tmp" path,
 * moved under the process root): splayed saves publish a generation
 * directory through .current (#649 / #713). */
static bool ix_live_path(const char* dir, const char* col, char* out, size_t cap) {
    char cur[1024];
    if (ray_splay_resolve_dir(ray_test_tmp(dir), cur, sizeof cur) != RAY_OK) return false;
    int n = snprintf(out, cap, "%s/%s", cur, col);
    return n > 0 && (size_t)n < cap;
}

/* FILTER(t, (== k key) AND (>= rc 10)) over the global table `t`, as one
 * unsplit conjunction (a graph run without ray_optimize, which would split
 * it; only the internal graph API reaches this arm): the eq+range arm
 * (test_store.c, lazy_eq_range).  rc "v" is the arm's shape, rc "k" one it
 * declines (the range on the equality column).  Owned result. */
static ray_t* ix_eqrange(const char* t, int64_t key, const char* rc) {
    ray_t* tbl = ray_env_get(ray_sym_intern(t, strlen(t)));         /* borrowed */
    if (!tbl || tbl->type != RAY_TABLE) return NULL;
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* pred = ray_and(g, ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, key)),
                             ray_ge(g, ray_scan(g, rc), ray_const_i64(g, 10)));
    ray_t* r = ray_execute(g, ray_filter(g, ray_const_table(g, tbl), pred));
    ray_graph_free(g);
    return r;
}

/* FILTER(t, (== col key)) as a bare graph (no ray_optimize): the
 * OP_FILTER single-compare arm itself.  Owned result. */
static ray_t* ix_filter_eq(const char* t, const char* col, int64_t key) {
    ray_t* tbl = ray_env_get(ray_sym_intern(t, strlen(t)));         /* borrowed */
    if (!tbl || tbl->type != RAY_TABLE) return NULL;
    ray_graph_t* g = ray_graph_new(tbl);
    ray_t* r = ray_execute(g, ray_filter(g, ray_const_table(g, tbl), ray_eq(g, ray_scan(g, col), ray_const_i64(g, key))));
    ray_graph_free(g);
    return r;
}

/* Generation 2 takes generation 0's index route for every recorded kind:
 * the same site counters, the same answers, and only the indexed column
 * materialized. */
static test_result_t test_consult_routes(void) {
    ray_t* r = ixr_eval("(set T (table [k s b p v] (list (.idx.hash (% (* (til 100000) 7919) 100003)) "
                        "(.idx.sort (as 'I32 (- (% (* (til 100000) 31) 9973) 4000))) (.idx.bloom (% (* (til 100000) 17) 50021)) "
                        "(.attr.set 'parted (div (til 100000) 1000)) (til 100000))))");
    if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/cr0/\" T {compress: false})"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/cr2/\" T {compress: true})"); if (r) ray_release(r);
    /* cn: a HASH key with nulls: generation 0's EQ probe needs a null-free
     * column (so generation 2 must not materialize for it), its `in` probe
     * does not */
    r = ixr_eval("(set TN (table [k v] (list (.idx.hash (as 'I64 (map (fn [j] (if (== 0 (% j 100)) 0N (% (* j 7) 10007))) (til 70000)))) "
                 "(til 70000))))");
    if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/cn0/\" TN {compress: false})"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/cn2/\" TN {compress: true})"); if (r) ray_release(r);
    /* cf: F64 HASH and BLOOM: generation 0 consults neither for a float
     * key, EQ or `in` (so generation 2 materializes nothing) */
    r = ixr_eval("(set TF (table [h b v] (list (.idx.hash (as 'F64 (% (til 70000) 1000))) "
                 "(.idx.bloom (as 'F64 (% (* (til 70000) 7) 5000))) (til 70000))))");
    if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/cf0/\" TF {compress: false})"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/cf2/\" TF {compress: true})"); if (r) ray_release(r);
    /* cs: a SYM HASH key with nulls, generation 2 since the rebuild: the
     * null key (id 0) consults nothing on generation 0, a nonzero key and
     * `in` consult the index */
    r = ixr_eval("(set SK (as 'SYM (map (fn [i] (if (== i 0) \"\" (concat \"k\" (as 'STR i)))) (til 5000))))");
    if (r) ray_release(r);
    r = ixr_eval("(set TS (table [k v] (list (.idx.hash (at SK (% (* (til 70000) 7) 5000))) (til 70000))))");
    if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/cs0/\" TS {compress: false})"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/cs2/\" TS {compress: true})"); if (r) ray_release(r);
    {
        char kp[1200];
        TEST_ASSERT_TRUE(ix_live_path(IDX_TMP "/cs2/", "k", kp, sizeof kp));
        TEST_ASSERT_EQ_U(ray_col_file_gen(kp), 2);
        TEST_ASSERT_EQ_I(ix_hdr(kp).index_kind, RAY_IDX_HASH);
    }
    /* the eq+range conjunction: the HASH key's index slice, the same rows;
     * a declined shape (the range on k itself) consults nothing and
     * materializes nothing */
    for (int sh = 0; sh < 2; sh++) {
        uint64_t ec[2], eh[2]; int64_t en[2];
        for (int g = 0; g < 2; g++) {
            char q[200];
            snprintf(q, sizeof q, "(set X (.db.splayed.get \"" IDX_TMP "/cr%d/\"))", g * 2);
            r = ixr_eval(q); if (r) ray_release(r);
            uint64_t c0 = ray_idx_consults[IDX_SITE_FILTER_EQRANGE], h0 = ray_idx_hits[IDX_SITE_FILTER_EQRANGE];
            ray_enc_counters_reset();
            r = ix_eqrange("X", 4242, sh ? "k" : "v");
            TEST_ASSERT_FMT(r && !RAY_IS_ERR(r) && r->type == RAY_TABLE, "eq+range gen %d: %s", g * 2,
                            r && RAY_IS_ERR(r) ? ray_error_msg() : "no table");
            en[g] = ray_table_nrows(r);
            ray_release(r);
            ec[g] = ray_idx_consults[IDX_SITE_FILTER_EQRANGE] - c0; eh[g] = ray_idx_hits[IDX_SITE_FILTER_EQRANGE] - h0;
            if (g) {                                                  /* k and v; nothing when declined */
                ray_enc_counters_t k; ray_enc_counters_get(&k);
                TEST_ASSERT_FMT(k.materialized == (sh ? 0u : 2u), "eq+range shape %d: %llu materializations", sh,
                                (unsigned long long)k.materialized);
            }
        }
        TEST_ASSERT_TRUE(en[0] == en[1] && en[0] >= 1);
        TEST_ASSERT_TRUE(ec[0] == (sh ? 0u : 1u) && eh[0] == ec[0]);   /* generation 0 takes the arm, or not */
        TEST_ASSERT_TRUE(ec[1] == ec[0] && eh[1] == eh[0]);
    }
    /* the bare single compare on the nullable HASH key: generation 0's EQ
     * probe declines a column with nulls, so generation 2 reads by range */
    {
        uint64_t c[2][IDX_SITE__N]; int64_t n[2];
        for (int g = 0; g < 2; g++) {
            char q[200];
            snprintf(q, sizeof q, "(set X (.db.splayed.get \"" IDX_TMP "/cn%d/\"))", g * 2);
            r = ixr_eval(q); if (r) ray_release(r);
            uint64_t c0[IDX_SITE__N]; memcpy(c0, ray_idx_consults, sizeof c0);
            ray_enc_counters_reset();
            r = ix_filter_eq("X", "k", 42);
            TEST_ASSERT_FMT(r && !RAY_IS_ERR(r) && r->type == RAY_TABLE, "bare eq gen %d: %s", g * 2,
                            r && RAY_IS_ERR(r) ? ray_error_msg() : "no table");
            n[g] = ray_table_nrows(r);
            ray_release(r);
            for (int s = 0; s < IDX_SITE__N; s++) c[g][s] = ray_idx_consults[s] - c0[s];
            if (g) { ray_enc_counters_t k; ray_enc_counters_get(&k); TEST_ASSERT_EQ_U(k.materialized, 0); }
        }
        TEST_ASSERT_TRUE(n[0] == n[1] && n[0] >= 1);
        TEST_ASSERT_TRUE(c[0][IDX_SITE_FILTER_HASH] == 0 && memcmp(c[0], c[1], sizeof c[0]) == 0);
    }
    /* sum-of-a-projection shapes: the select gathers v by the selection, so
     * each also pins that v is never materialized.  `uses`: generation 0
     * consults the column's region index (its materialization on generation
     * 2 is that index use); every other shape takes P3's encoded route on
     * generation 2 and materializes nothing. */
    /* `probe`: an equality on the HASH, answered by probing the region where
     * it is mapped (ray_enc_hash_eq_rowsel): the index is used, nothing is
     * materialized */
    static const struct { const char* q; bool uses; const char* d; bool probe; } qs[] = {
        { "(sum (at (select {from: X where: (== k 4242)}) 'v))", true, NULL, true },
        { "(sum (at (select {from: X where: (in k [1 2 3 4242])}) 'v))", true, NULL, false },
        { "(sum (at (select {from: X where: (> s 3000)}) 'v))", true, NULL, false },
        { "(sum (at (select {from: X where: (== b 50020)}) 'v))", true, NULL, false },     /* present: consulted, no prune */
        { "(sum (at (select {from: X where: (== b 3)}) 'v))", true, NULL, false },
        { "(sum (at (select {from: X where: (== p 77)}) 'v))", true, NULL, false },
        { "(sum (at (select {from: X where: (in p [3 77])}) 'v))", true, NULL, false },
        { "(sum (at (select {from: X where: (!= k 4242)}) 'v))", false, NULL, false },
        { "(sum (at (select {from: X where: (> k 50000)}) 'v))", false, NULL, false },
        { "(sum (at (select {from: X where: (< k 500)}) 'v))", false, NULL, false },
        { "(sum (at (select {from: X where: (> k 999999)}) 'v))", false, NULL, false },
        { "(sum (at (select {from: X where: (!= s 3000)}) 'v))", false, NULL, false },
        { "(sum (at (select {from: X where: (in s [1 2 3000])}) 'v))", false, NULL, false },
        { "(sum (at (select {from: X where: (!= b 5)}) 'v))", false, NULL, false },
        { "(sum (at (select {from: X where: (> b 40000)}) 'v))", false, NULL, false },
        { "(sum (at (select {from: X where: (in b [3 50020])}) 'v))", false, NULL, false },
        { "(sum (at (select {from: X where: (!= p 77)}) 'v))", false, NULL, false },
        { "(sum (at (select {from: X where: (> p 90)}) 'v))", false, NULL, false },
        { "(sum (at (select {from: X where: (== k 42)}) 'v))", false, "cn", false },
        { "(sum (at (select {from: X where: (in k [42 43])}) 'v))", true, "cn", false },
        /* SORT: every compare but NE consults the range probe */
        { "(sum (at (select {from: X where: (== s 3000)}) 'v))", true, NULL, false },
        { "(sum (at (select {from: X where: (< s -3000)}) 'v))", true, NULL, false },
        { "(sum (at (select {from: X where: (<= s -3000)}) 'v))", true, NULL, false },
        { "(sum (at (select {from: X where: (>= s 3000)}) 'v))", true, NULL, false },
        /* float keys: neither HASH nor BLOOM, EQ or `in` */
        { "(sum (at (select {from: X where: (== h 42.0)}) 'v))", false, "cf", false },
        { "(sum (at (select {from: X where: (in h [42.0 43.0])}) 'v))", false, "cf", false },
        { "(sum (at (select {from: X where: (== b 42.0)}) 'v))", false, "cf", false },
        { "(sum (at (select {from: X where: (in b [42.0 43.0])}) 'v))", false, "cf", false },
        /* SYM HASH with nulls: key 0, a nonzero key, `in` */
        /* the null key: decided on ids on the encoded route (id 0) */
        { "(sum (at (select {from: X where: (== k ')}) 'v))", false, "cs", false },
        { "(sum (at (select {from: X where: (!= k ')}) 'v))", false, "cs", false },
        { "(sum (at (select {from: X where: (== k 'k42)}) 'v))", true, "cs", true },
        { "(sum (at (select {from: X where: (in k ['k42 'k43])}) 'v))", true, "cs", false },
        { NULL, false, NULL, false },
    };
    for (int i = 0; qs[i].q; i++) {
        uint64_t c[2][IDX_SITE__N], h[2][IDX_SITE__N]; int64_t n[2];
        for (int g = 0; g < 2; g++) {
            char q[200];
            snprintf(q, sizeof q, "(set X (.db.splayed.get \"" IDX_TMP "/%s%d/\"))", qs[i].d ? qs[i].d : "cr", g * 2);
            r = ixr_eval(q); if (r) ray_release(r);
            uint64_t c0[IDX_SITE__N], h0[IDX_SITE__N];
            memcpy(c0, ray_idx_consults, sizeof c0); memcpy(h0, ray_idx_hits, sizeof h0);
            ray_enc_counters_reset();
            n[g] = ixr_i64(qs[i].q);
            for (int s = 0; s < IDX_SITE__N; s++) { c[g][s] = ray_idx_consults[s] - c0[s]; h[g][s] = ray_idx_hits[s] - h0[s]; }
            if (g) {
                ray_enc_counters_t k; ray_enc_counters_get(&k);
                TEST_ASSERT_FMT(k.materialized == (qs[i].uses && !qs[i].probe ? 1u : 0u), "%s: %llu materializations", qs[i].q,
                                (unsigned long long)k.materialized);
                TEST_ASSERT_EQ_I(ixr_i64("(if (at (.idx.info X 'v) 'materialized) 1 0)"), 0);
            }
        }
        TEST_ASSERT_FMT(n[0] == n[1] && n[0] != INT64_MIN, "%s: %lld vs %lld", qs[i].q, (long long)n[0], (long long)n[1]);
        uint64_t any = 0; for (int s = 0; s < IDX_SITE__N; s++) any += c[0][s];
        if (qs[i].uses)
            TEST_ASSERT_FMT(any >= 1, "%s: generation 0 consults no index (the shape does not discriminate)", qs[i].q);
        TEST_ASSERT_FMT(memcmp(c[0], c[1], sizeof c[0]) == 0 && memcmp(h[0], h[1], sizeof h[0]) == 0,
                        "%s: index sites differ between generations", qs[i].q);
    }
    PASS();
}

/* An unindexed (chunk-zone) generation-2 column keeps P2's directory
 * consult: nothing is materialized for an out-of-range key. */
static test_result_t test_consult_dir_unchanged(void) {
    ray_t* r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/cz/\" (table [h] (list (til 200000))) {compress: true})");
    if (r) ray_release(r);
    r = ixr_eval("(set Z (.db.splayed.get \"" IDX_TMP "/cz/\"))"); if (r) ray_release(r);
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info Z 'h) 'index)"), ray_sym_intern("chunk_zone", 10));
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(ixr_i64("(count (select {from: Z where: (== h 999999)}))"), 0);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    PASS();
}

/* Spec "Errors": a transient failure on the index route (the parse's oom)
 * is the query's error, not a scan, and not cached: the next query, with
 * the hook off, answers from the index.  The `in` probe parses the region
 * (it materializes the column); the equality probes the region where it is
 * mapped (ray_enc_hash_eq_rowsel), parses nothing and answers under the
 * hook. */
static test_result_t test_index_route_oom(void) {
    ray_t* r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/co/\" (table [k v] (list (.idx.hash (% (* (til 70000) 7) 10007)) (til 70000))) {compress: true})");
    if (r) ray_release(r);
    r = ixr_eval("(set CO (.db.splayed.get \"" IDX_TMP "/co/\"))"); if (r) ray_release(r);
    const char* q = "(count (at (select {from: CO where: (in k [42 43])}) 'v))";
    const char* qe = "(count (at (select {from: CO where: (== k 42)}) 'v))";
    int64_t want = ixr_i64("(count (where (in (% (* (til 70000) 7) 10007) [42 43])))");
    ray_enccol_test_fail_index_parse(true);
    uint64_t e0 = ray_idx_hits[IDX_SITE_FILTER_HASH];
    TEST_ASSERT_EQ_I(ixr_i64(qe), 7);
    TEST_ASSERT_EQ_U(ray_idx_hits[IDX_SITE_FILTER_HASH], e0 + 1);    /* the region probe */
    ray_error_clear();
    r = ray_eval_str(q);
    ray_enccol_test_fail_index_parse(false);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "oom");
    TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), "index parse") != NULL);
    ray_error_free(r);
    uint64_t h0 = ray_idx_hits[IDX_SITE_IN];
    TEST_ASSERT_EQ_I(ixr_i64(q), want);
    TEST_ASSERT_EQ_U(ray_idx_hits[IDX_SITE_IN], h0 + 1);    /* from the index */
    PASS();
}

/* An equality on a recorded HASH probes the region where it is mapped
 * (ray_enc_hash_eq_rowsel): generation 0's answers and index counters,
 * nothing materialized, the column not decoded.  k: about one row per key;
 * g: 100 rows per key; z: key 0 on half the rows (dense: the probe
 * declines and the column materializes, as before); w: an I32 key; y: a
 * SYM key.  An absent key is an empty selection. */
static test_result_t test_hash_region_probe(void) {
    ray_t* r = ixr_eval("(set SY (as 'SYM (map (fn [i] (concat \"y\" (as 'STR i))) (til 2000))))");
    if (r) ray_release(r);
    r = ixr_eval("(set TP (table [k g z w y v] (list (.idx.hash (% (* (til 100000) 7919) 100003)) "
                 "(.idx.hash (% (til 100000) 1000)) (.idx.hash (* (>= (til 100000) 50000) (til 100000))) "
                 "(.idx.hash (as 'I32 (- (% (* (til 100000) 31) 9973) 4000))) (.idx.hash (at SY (% (* (til 100000) 7) 2000))) "
                 "(til 100000))))");
    if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/hp0/\" TP {compress: false})"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/hp2/\" TP {compress: true})"); if (r) ray_release(r);
    static const struct { const char* q; int probes; uint64_t mat; } qs[] = {
        { "(sum (at (select {from: X where: (== k 4242)}) 'v))", 1, 0 },
        { "(sum (at (select {from: X where: (== k 999999)}) 'v))", 1, 0 },   /* absent */
        { "(sum (at (select {from: X where: (== g 77)}) 'v))", 1, 0 },
        { "(sum (at (select {from: X where: (== g 999)}) 'v))", 1, 0 },      /* the last group */
        { "(sum (at (select {from: X where: (== z 0)}) 'v))", 0, 1 },        /* dense: declined */
        { "(sum (at (select {from: X where: (== z 77777)}) 'v))", 1, 0 },
        { "(sum (at (select {from: X where: (== w -4000)}) 'v))", 1, 0 },
        { "(sum (at (select {from: X where: (== w 5969)}) 'v))", 1, 0 },
        { "(sum (at (select {from: X where: (== y 'y42)}) 'v))", 1, 0 },
        { "(sum (at (select {from: X where: (== y 'nope)}) 'v))", 0, 0 },    /* not in the domain */
        { NULL, 0, 0 },
    };
    for (int i = 0; qs[i].q; i++) {
        int64_t n[2]; uint64_t hc[2], hh[2];
        for (int gen = 0; gen < 2; gen++) {
            r = ixr_eval(gen ? "(set X (.db.splayed.get \"" IDX_TMP "/hp2/\"))" : "(set X (.db.splayed.get \"" IDX_TMP "/hp0/\"))");
            if (r) ray_release(r);
            uint64_t c0 = ray_idx_consults[IDX_SITE_FILTER_HASH], h0 = ray_idx_hits[IDX_SITE_FILTER_HASH];
            uint64_t p0 = ray_enc_hash_test_probes();
            ray_enc_counters_reset();
            n[gen] = ixr_i64(qs[i].q);
            hc[gen] = ray_idx_consults[IDX_SITE_FILTER_HASH] - c0; hh[gen] = ray_idx_hits[IDX_SITE_FILTER_HASH] - h0;
            if (gen) {
                ray_enc_counters_t k; ray_enc_counters_get(&k);
                TEST_ASSERT_FMT(ray_enc_hash_test_probes() - p0 == (uint64_t)qs[i].probes, "%s: %llu region probes", qs[i].q,
                                (unsigned long long)(ray_enc_hash_test_probes() - p0));
                TEST_ASSERT_FMT(k.materialized == qs[i].mat, "%s: %llu materializations", qs[i].q,
                                (unsigned long long)k.materialized);
                /* the probed column: its group's rows read back, never the
                 * column (100000 rows, at most 800000 bytes) */
                if (!qs[i].mat) {
                    const char* cn = strstr(qs[i].q, "(== ") + 4;
                    ray_t* xt = ray_eval_str("X");
                    ray_t* xe = xt && !RAY_IS_ERR(xt) ? ray_table_get_col_enc(xt, ray_sym_intern(cn, 1)) : NULL;
                    uint64_t cb = xe ? atomic_load(&ray_enccol_of(xe)->decoded_bytes) : UINT64_MAX;
                    if (xt && !RAY_IS_ERR(xt)) ray_release(xt);
                    TEST_ASSERT_FMT(cb <= 800000 / 2, "%s: the key column decoded %llu bytes", qs[i].q,
                                    (unsigned long long)cb);
                }
            }
        }
        TEST_ASSERT_FMT(n[0] == n[1] && n[0] != INT64_MIN, "%s: %lld vs %lld", qs[i].q, (long long)n[0], (long long)n[1]);
        if (qs[i].probes) TEST_ASSERT_FMT(hc[1] >= 1 && hh[1] == hh[0], "%s: hash hits %llu vs %llu", qs[i].q,
                                          (unsigned long long)hh[1], (unsigned long long)hh[0]);
    }
    PASS();
}

/* ---- index decisions in the write context; the SYM rebuild (Task 7) ---- */

static ray_err_t ix_save_ctx(ray_t* col, const char* path, struct ray_sym_domain_s* dom, uint8_t decision) {
    ray_col_write_t w; memset(&w, 0, sizeof w);
    ray_t* e = ray_col_src_bind(col, &w.src);
    if (e) { ray_error_free(e); return RAY_ERR_TYPE; }
    w.name = "k"; w.gen = 2; w.target_dom = dom; w.durable = false; w.index_decision = decision;
    ray_err_t r = ray_col_save_ctx(&w, path);
    ray_col_src_release(&w.src);
    return r;
}

/* Review focus 2: a SYM HASH saved into a target domain with another
 * insertion order and width is rebuilt over the target ids.  1000 words x 5
 * rows: every group is sparse, so the eq probe answers by index
 * (ray_index_hash_eq_rowsel returns NULL for dense groups, idxop.c:2404). */
static test_result_t test_sym_rebuild_remap(void) {
    char wb[16];
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 5000); v->len = 5000;
    for (int i = 0; i < 5000; i++) { int n = snprintf(wb, sizeof wb, "w%d", (i * 7) % 1000); ((int64_t*)ray_data(v))[i] = ray_sym_intern(wb, (size_t)n); }
    v = with_index(v, ray_index_attach_hash);
    TEST_ASSERT_NOT_NULL(v);
    ray_index_payload(v->index)->markers |= RAY_MARK_UNIQUE;    /* flag only: the builder does not check it */
    int64_t ts = ray_sym_intern("ts", 2);
    ray_index_payload(v->index)->u.hash.order_sym = ts;          /* a heap root of this process: trusted */
    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(ray_test_tmp(IDX_TMP "/sym"));
    TEST_ASSERT_NOT_NULL(dom);
    for (int i = 0; i < 300; i++) { int n = snprintf(wb, sizeof wb, "pre%d", i); (void)ray_sym_domain_intern(dom, wb, (size_t)n); }
    for (int i = 999; i >= 0; i--) { int n = snprintf(wb, sizeof wb, "w%d", i); (void)ray_sym_domain_intern(dom, wb, (size_t)n); }  /* reversed */
    const char* ps = ray_test_tmp(IDX_TMP "/s");
    TEST_ASSERT_EQ_I(ix_save_ctx(v, ps, dom, RAY_COL_IDX_KEEP), RAY_OK);
    ray_t* c = ray_col_mmap_splayed_dom(ps, dom);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->type == RAY_ENCCOL);
    TEST_ASSERT_EQ_I(ray_enccol_of(c)->hdr.index_kind, RAY_IDX_HASH);
    TEST_ASSERT_EQ_I(ray_enccol_of(c)->hdr.col_flags, RAY_ENC_FLAG_UNIQUE);   /* the source's UNIQUE */
    TEST_ASSERT_EQ_I(ray_enccol_of(c)->attrs & RAY_SYM_W_MASK, RAY_SYM_W16);   /* 1301 entries: a wider class */
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_HASH);
    TEST_ASSERT_TRUE(ray_index_payload(m->index)->markers & RAY_MARK_UNIQUE);
    TEST_ASSERT_EQ_I(ray_index_payload(m->index)->u.hash.order_sym, ts);   /* the source's order name */
    for (int w = 0; w < 1000; w += 111) {
        int n = snprintf(wb, sizeof wb, "w%d", w);
        int64_t pos = ray_sym_domain_find(dom, wb, (size_t)n);
        TEST_ASSERT_TRUE(pos > 300);
        ray_t* sel = ray_index_hash_eq_rowsel(m, pos);
        TEST_ASSERT_FMT(sel && ray_rowsel_meta(sel)->total_pass == 5, "word %s", wb);
        ray_rowsel_release(sel);
    }
    /* encoded -> 2 from a fresh mapping: the rebuild materializes it exactly once */
    ray_release(c);
    c = ray_col_mmap_splayed_dom(ps, dom);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(ix_save_ctx(c, ray_test_tmp(IDX_TMP "/s2"), dom, RAY_COL_IDX_KEEP), RAY_OK);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    ray_release(c);
    c = ray_col_mmap_splayed_dom(ray_test_tmp(IDX_TMP "/s2"), dom);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->type == RAY_ENCCOL);
    TEST_ASSERT_EQ_I(ray_enccol_of(c)->hdr.index_kind, RAY_IDX_HASH);
    TEST_ASSERT_EQ_I(ray_enccol_of(c)->hdr.col_flags, RAY_ENC_FLAG_UNIQUE);
    m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    TEST_ASSERT_TRUE(ray_index_payload(m->index)->markers & RAY_MARK_UNIQUE);
    TEST_ASSERT_EQ_I(ray_index_payload(m->index)->u.hash.order_sym, ts);   /* kept through the encoded re-save */
    ray_t* sel = ray_index_hash_eq_rowsel(m, ray_sym_domain_find(dom, "w7", 2));
    TEST_ASSERT_TRUE(sel && ray_rowsel_meta(sel)->total_pass == 5);
    ray_rowsel_release(sel);
    ray_release(c);
    ray_sym_domain_release(dom); ray_release(v);
    PASS();
}

/* A SYM PART saved at a wider target width writes its keys at the saved
 * width (encw: the final attrs, not the caller's), keeps a trustworthy
 * order name, and answers by the target ids. */
static test_result_t test_sym_part_rebuild(void) {
    char wb[16];
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 600); v->len = 600;
    for (int i = 0; i < 600; i++) { int n = snprintf(wb, sizeof wb, "pk%d", i / 100); ((int64_t*)ray_data(v))[i] = ray_sym_intern(wb, (size_t)n); }
    v = with_index(v, ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(v);
    ray_index_payload(v->index)->u.part.order_sym = ray_sym_intern("ts", 2);
    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(ray_test_tmp(IDX_TMP "/symp"));
    TEST_ASSERT_NOT_NULL(dom);
    for (int i = 0; i < 400; i++) { int n = snprintf(wb, sizeof wb, "pre%d", i); (void)ray_sym_domain_intern(dom, wb, (size_t)n); }
    for (int i = 5; i >= 0; i--) { int n = snprintf(wb, sizeof wb, "pk%d", i); (void)ray_sym_domain_intern(dom, wb, (size_t)n); }
    const char* p = ray_test_tmp(IDX_TMP "/sp");
    TEST_ASSERT_EQ_I(ix_save_ctx(v, p, dom, RAY_COL_IDX_KEEP), RAY_OK);
    ray_t* c = ray_col_mmap_splayed_dom(p, dom);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->type == RAY_ENCCOL);
    TEST_ASSERT_EQ_I(ray_enccol_of(c)->hdr.index_kind, RAY_IDX_PART);
    TEST_ASSERT_EQ_I(ray_enccol_of(c)->attrs & RAY_SYM_W_MASK, RAY_SYM_W16);
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_FMT(m && !RAY_IS_ERR(m), "materialize: %s", m && RAY_IS_ERR(m) ? ray_error_msg() : "null");
    TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_PART);
    ray_index_t* ix = ray_index_payload(m->index);
    TEST_ASSERT_EQ_I(ix->u.part.order_sym, ray_sym_intern("ts", 2));      /* the source's order name */
    TEST_ASSERT_EQ_I(ix->u.part.n_parts, 6);
    for (int i = 0; i < 6; i++) {
        int n = snprintf(wb, sizeof wb, "pk%d", i);
        ray_t* sel = ray_index_part_eq_rowsel(m, ray_sym_domain_find(dom, wb, (size_t)n));
        TEST_ASSERT_FMT(sel && ray_rowsel_meta(sel)->total_pass == 100, "key %s", wb);
        ray_rowsel_release(sel);
    }
    ray_release(c);
    ray_sym_domain_release(dom); ray_release(v);
    PASS();
}

/* An encoded non-SYM source keeps its region byte for byte, without
 * materializing. */
static test_result_t test_encoded_keep_copies(void) {
    ray_t* v = ix_hash_col(70000);
    const char* pa = ray_test_tmp(IDX_TMP "/a");
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, pa, 2), RAY_OK);
    ray_t* c = ray_col_mmap(pa);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->type == RAY_ENCCOL);
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(ix_save_ctx(c, ray_test_tmp(IDX_TMP "/b"), NULL, RAY_COL_IDX_KEEP), RAY_OK);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    size_t na = 0, nb = 0; ray_enc_hdr_t ea, eb; uint64_t fa, fb;
    uint8_t* ra = ix_file_region(pa, &na, &ea, &fa);
    uint8_t* rb = ix_file_region(ray_test_tmp(IDX_TMP "/b"), &nb, &eb, &fb);
    bool same = ra && rb && na == nb && memcmp(ra, rb, na) == 0;
    free(ra); free(rb);
    TEST_ASSERT_TRUE(same);
    TEST_ASSERT_EQ_I(eb.index_kind, RAY_IDX_HASH);
    TEST_ASSERT_TRUE(eb.end_off == fb);
    /* DROP: no region; REBUILD: materializes once and equals the copy */
    TEST_ASSERT_EQ_I(ix_save_ctx(c, ray_test_tmp(IDX_TMP "/d"), NULL, RAY_COL_IDX_DROP), RAY_OK);
    TEST_ASSERT_NULL(ix_file_region(ray_test_tmp(IDX_TMP "/d"), &nb, &eb, &fb));
    TEST_ASSERT_EQ_I(eb.index_kind, 0);
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(ix_save_ctx(c, ray_test_tmp(IDX_TMP "/r"), NULL, RAY_COL_IDX_REBUILD), RAY_OK);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    ra = ix_file_region(pa, &na, &ea, &fa);
    rb = ix_file_region(ray_test_tmp(IDX_TMP "/r"), &nb, &eb, &fb);
    same = ra && rb && na == nb && memcmp(ra, rb, na) == 0;        /* the same builder, the same column */
    free(ra); free(rb);
    TEST_ASSERT_TRUE(same && eb.index_kind == RAY_IDX_HASH);
    ray_release(c);
    /* a plain source on REBUILD goes by range and writes the same region */
    TEST_ASSERT_EQ_I(ix_save_ctx(v, ray_test_tmp(IDX_TMP "/rp"), NULL, RAY_COL_IDX_REBUILD), RAY_OK);
    ra = ix_file_region(pa, &na, &ea, &fa);
    rb = ix_file_region(ray_test_tmp(IDX_TMP "/rp"), &nb, &eb, &fb);
    same = ra && rb && na == nb && memcmp(ra, rb, na) == 0;
    free(ra); free(rb);
    TEST_ASSERT_TRUE(same);
    /* an unknown decision is a type error; generation 0 takes KEEP only */
    TEST_ASSERT_EQ_I(ix_save_ctx(v, ray_test_tmp(IDX_TMP "/x"), NULL, RAY_COL_IDX_DROP + 1), RAY_ERR_TYPE);
    ray_release(v);
    PASS();
}

/* Review minor 1: a stale SYM HASH / PART is rebuilt from the column on
 * generation 2 (every SYM index is rebuilt over the target ids), without
 * the stale root's UNIQUE or order name (colw_region_prepare's `!stale`):
 * after a splayed save and reload, generation 2 keeps at least what
 * generation 0 keeps (a rebuilt HASH; no PART). */
static test_result_t test_sym_stale_rebuild(void) {
    char wb[16];
    ray_t* g = ray_sym_vec_new(RAY_SYM_W64, 4000); g->len = 4000;
    ray_t* p = ray_sym_vec_new(RAY_SYM_W64, 4000); p->len = 4000;
    for (int i = 0; i < 4000; i++) {
        int n = snprintf(wb, sizeof wb, "sg%d", (i * 7) % 800);
        ((int64_t*)ray_data(g))[i] = ray_sym_intern(wb, (size_t)n);
        n = snprintf(wb, sizeof wb, "sp%d", i / 500);
        ((int64_t*)ray_data(p))[i] = ray_sym_intern(wb, (size_t)n);
    }
    g = with_index(g, ray_index_attach_hash);
    p = with_index(p, ray_index_attach_part);
    TEST_ASSERT_TRUE(g && p);
    ray_index_payload(g->index)->markers |= RAY_MARK_UNIQUE;
    ray_index_payload(g->index)->u.hash.order_sym = ray_sym_intern("ts", 2);   /* heap roots: trusted unless stale */
    ray_index_payload(p->index)->u.part.order_sym = ray_sym_intern("ts", 2);
    ray_index_payload(g->index)->built_for_len = 3999;              /* stale */
    ray_index_payload(p->index)->built_for_len = 3999;
    ray_t* t = ray_table_new(2);
    t = ray_table_add_col(t, ray_sym_intern("g", 1), g);
    t = ray_table_add_col(t, ray_sym_intern("p", 1), p);
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    for (int gen = 0; gen <= 2; gen += 2) {
        char d[64], y[64];
        snprintf(d, sizeof d, IDX_TMP "/st%d/", gen); snprintf(y, sizeof y, IDX_TMP "/st%d.sym", gen);
        TEST_ASSERT_EQ_I(ray_splay_save_mode(t, ray_test_tmp(d), ray_test_tmp(y), gen ? RAY_COMPRESS_ON : RAY_COMPRESS_OFF),
                         RAY_OK);
    }
    ray_index_payload(g->index)->built_for_len = 4000;
    ray_index_payload(p->index)->built_for_len = 4000;
    ray_t* l0 = ray_read_splayed(ray_test_tmp(IDX_TMP "/st0/"), ray_test_tmp(IDX_TMP "/st0.sym"));
    ray_t* l2 = ray_read_splayed(ray_test_tmp(IDX_TMP "/st2/"), ray_test_tmp(IDX_TMP "/st2.sym"));
    TEST_ASSERT_TRUE(l0 && !RAY_IS_ERR(l0) && l2 && !RAY_IS_ERR(l2));
    ray_t* g0 = ray_table_get_col(l0, ray_sym_intern("g", 1));
    ray_t* p0 = ray_table_get_col(l0, ray_sym_intern("p", 1));
    ray_t* g2 = ray_table_col(l2, ray_sym_intern("g", 1));             /* materialized: attaches the region */
    ray_t* p2 = ray_table_col(l2, ray_sym_intern("p", 1));
    TEST_ASSERT_TRUE(g2 && !RAY_IS_ERR(g2) && p2 && !RAY_IS_ERR(p2));
    TEST_ASSERT_EQ_I(ray_index_kind(g0), RAY_IDX_HASH);              /* generation 0 rebuilds the HASH */
    TEST_ASSERT_TRUE(ray_index_kind(p0) != RAY_IDX_PART);            /* and drops the PART */
    TEST_ASSERT_EQ_I(ray_index_kind(g2), RAY_IDX_HASH);
    TEST_ASSERT_EQ_I(ray_index_kind(p2), RAY_IDX_PART);
    TEST_ASSERT_FALSE(ray_index_payload(g2->index)->markers & RAY_MARK_UNIQUE);   /* a stale flag is not kept */
    TEST_ASSERT_EQ_I(ray_index_payload(g2->index)->u.hash.order_sym, -1);         /* nor a stale order name */
    TEST_ASSERT_EQ_I(ray_index_payload(p2->index)->u.part.order_sym, -1);
    TEST_ASSERT_EQ_I(ray_index_payload(g2->index)->built_for_len, 4000);
    /* the rebuilt indexes answer by the file's ids */
    ray_t* cell = ray_sym_vec_cell(g2, 7);
    int64_t key = ray_read_sym(ray_data(g2), 7, RAY_SYM, g2->attrs);
    ray_t* sel = ray_index_hash_eq_rowsel(g2, key);
    TEST_ASSERT_TRUE(cell && sel && ray_rowsel_meta(sel)->total_pass == 5);
    ray_rowsel_release(sel);
    key = ray_read_sym(ray_data(p2), 1000, RAY_SYM, p2->attrs);
    sel = ray_index_part_eq_rowsel(p2, key);
    TEST_ASSERT_TRUE(sel && ray_rowsel_meta(sel)->total_pass == 500);
    ray_rowsel_release(sel);
    ray_release(l0); ray_release(l2); ray_release(t); ray_release(g); ray_release(p);
    PASS();
}

/* A stale SYM PART over values that are no longer in contiguous blocks has
 * no rebuild: the save still succeeds and records no index. */
static test_result_t test_sym_stale_part_unbuildable(void) {
    char wb[16];
    ray_t* p = ray_sym_vec_new(RAY_SYM_W64, 600); p->len = 600;
    for (int i = 0; i < 600; i++) { int n = snprintf(wb, sizeof wb, "q%d", i / 100); ((int64_t*)ray_data(p))[i] = ray_sym_intern(wb, (size_t)n); }
    p = with_index(p, ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(p);
    ray_index_payload(p->index)->built_for_len = 599;              /* stale */
    ((int64_t*)ray_data(p))[599] = ray_sym_intern("q0", 2);        /* q0 again after q5: not contiguous */
    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(ray_test_tmp(IDX_TMP "/symq"));
    for (int i = 0; i < 6; i++) { int n = snprintf(wb, sizeof wb, "q%d", i); (void)ray_sym_domain_intern(dom, wb, (size_t)n); }
    const char* pa = ray_test_tmp(IDX_TMP "/sq");
    TEST_ASSERT_EQ_I(ix_save_ctx(p, pa, dom, RAY_COL_IDX_KEEP), RAY_OK);
    size_t n; ray_enc_hdr_t eh; uint64_t fn;
    TEST_ASSERT_NULL(ix_file_region(pa, &n, &eh, &fn));
    TEST_ASSERT_EQ_I(eh.index_kind, 0);
    ray_t* c = ray_col_mmap_splayed_dom(pa, dom);
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_release(c);
    ray_index_payload(p->index)->built_for_len = 600;
    ray_sym_domain_release(dom); ray_release(p);
    PASS();
}

/* Review important 1: the null-key compare on a SYM HASH uses no index
 * (generation 0 consults none for key 0): generation 2 answers on ids as
 * generation 0 does, and so does an index-using key. */
static test_result_t test_sym_hash_null_key(void) {
    ray_t* r = ixr_eval("(set SK (as 'SYM (map (fn [i] (if (== i 0) \"\" (concat \"k\" (as 'STR i)))) (til 5000))))");
    if (r) ray_release(r);
    r = ixr_eval("(set TS (table [k v] (list (.idx.hash (at SK (% (* (til 70000) 7) 5000))) (til 70000))))");
    if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/zn0/\" TS {compress: false})"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/zn2/\" TS {compress: true})"); if (r) ray_release(r);
    char kp[1200];
    TEST_ASSERT_TRUE(ix_live_path(IDX_TMP "/zn2/", "k", kp, sizeof kp));
    ray_enc_hdr_t eh = ix_hdr(kp);
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_HASH);
    static const char* const qs[] = {
        "(sum (at (select {from: X where: (== k ')}) 'v))",
        "(sum (at (select {from: X where: (!= k ')}) 'v))",
        "(count (select {from: X where: (== k ')}))",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        r = ixr_eval("(set X (.db.splayed.get \"" IDX_TMP "/zn0/\"))"); if (r) ray_release(r);
        int64_t want = ixr_i64(qs[i]);
        TEST_ASSERT_TRUE(want != INT64_MIN);
        r = ixr_eval("(set X (.db.splayed.get \"" IDX_TMP "/zn2/\"))"); if (r) ray_release(r);
        ray_enc_counters_reset();
        int64_t got = ixr_i64(qs[i]);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_FMT(got == want, "%s: %lld vs generation 0 %lld", qs[i], (long long)got, (long long)want);
        TEST_ASSERT_EQ_U(k.materialized, 0);
    }
    TEST_ASSERT_EQ_I(ixr_i64("(count (select {from: X where: (== k ')}))"), 14);
    int64_t got = ixr_i64("(sum (at (select {from: X where: (== k 'k42)}) 'v))");
    r = ixr_eval("(set X (.db.splayed.get \"" IDX_TMP "/zn0/\"))"); if (r) ray_release(r);
    int64_t want = ixr_i64("(sum (at (select {from: X where: (== k 'k42)}) 'v))");
    TEST_ASSERT_TRUE(want != INT64_MIN);
    TEST_ASSERT_EQ_I(got, want);
    PASS();
}

/* DROP records no kind on either writer: a plain column (direct pointers),
 * a SYM column (range reads, the rebuild skipped).  Each file opens and
 * materializes without an index; UNIQUE, a header flag, stays. */
static test_result_t test_drop_records_none(void) {
    ray_t* v = ix_hash_col(3000);
    ray_index_payload(v->index)->markers |= RAY_MARK_UNIQUE;
    const char* p = ray_test_tmp(IDX_TMP "/dp");
    TEST_ASSERT_EQ_I(ix_save_ctx(v, p, NULL, RAY_COL_IDX_DROP), RAY_OK);
    size_t n; ray_enc_hdr_t eh; uint64_t fn;
    TEST_ASSERT_NULL(ix_file_region(p, &n, &eh, &fn));
    TEST_ASSERT_EQ_I(eh.index_kind, 0);
    TEST_ASSERT_EQ_I(eh.col_flags, RAY_ENC_FLAG_UNIQUE);
    ray_t* c = ray_col_mmap(p);
    TEST_ASSERT_FMT(c && !RAY_IS_ERR(c), "open: %s", c && RAY_IS_ERR(c) ? ray_error_msg() : "null");
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m) && ray_index_kind(m) != RAY_IDX_HASH);
    ray_release(c); ray_release(v);
    char wb[16];
    ray_t* s = ray_sym_vec_new(RAY_SYM_W64, 400); s->len = 400;
    for (int i = 0; i < 400; i++) { int k = snprintf(wb, sizeof wb, "d%d", i % 37); ((int64_t*)ray_data(s))[i] = ray_sym_intern(wb, (size_t)k); }
    s = with_index(s, ray_index_attach_hash);
    TEST_ASSERT_NOT_NULL(s);
    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(ray_test_tmp(IDX_TMP "/symd"));
    for (int i = 0; i < 37; i++) { int k = snprintf(wb, sizeof wb, "d%d", i); (void)ray_sym_domain_intern(dom, wb, (size_t)k); }
    p = ray_test_tmp(IDX_TMP "/ds");
    TEST_ASSERT_EQ_I(ix_save_ctx(s, p, dom, RAY_COL_IDX_DROP), RAY_OK);
    TEST_ASSERT_NULL(ix_file_region(p, &n, &eh, &fn));
    TEST_ASSERT_EQ_I(eh.index_kind, 0);
    c = ray_col_mmap_splayed_dom(p, dom);
    TEST_ASSERT_FMT(c && !RAY_IS_ERR(c), "open: %s", c && RAY_IS_ERR(c) ? ray_error_msg() : "null");
    m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m) && !ray_index_has(m));
    ray_release(c);
    ray_sym_domain_release(dom); ray_release(s);
    PASS();
}

/* A loaded HASH / SORT / BLOOM / PART column re-saved compressed keeps its
 * kind and its region, byte for byte, without materializing. */
static test_result_t test_reload_keeps_kind(void) {
    static ray_t* (*const att[4])(ray_t**) = { ray_index_attach_hash, ray_index_attach_sort,
                                              ray_index_attach_bloom, ray_index_attach_part };
    static const uint8_t kinds[4] = { RAY_IDX_HASH, RAY_IDX_SORT, RAY_IDX_BLOOM, RAY_IDX_PART };
    for (int k = 0; k < 4; k++) {
        ray_t* v = ray_vec_new(RAY_I64, 70000); v->len = 70000;
        for (int64_t i = 0; i < 70000; i++) ((int64_t*)ray_data(v))[i] = i / 700 * 3 - 50;   /* blocks: PART admits it */
        v = with_index(v, att[k]);
        TEST_ASSERT_FMT(v != NULL, "kind %u", kinds[k]);
        char a[64], b[64];
        snprintf(a, sizeof a, IDX_TMP "/rk%d_a", k); snprintf(b, sizeof b, IDX_TMP "/rk%d_b", k);
        TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(a), 2), RAY_OK);
        ray_t* c = ray_col_mmap(ray_test_tmp(a));
        TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c) && c->type == RAY_ENCCOL);
        ray_enc_counters_reset();
        TEST_ASSERT_EQ_I(ray_col_save_gen(c, ray_test_tmp(b), 2), RAY_OK);
        ray_enc_counters_t kc; ray_enc_counters_get(&kc);
        TEST_ASSERT_FMT(kc.materialized == 0, "kind %u: %llu materializations", kinds[k], (unsigned long long)kc.materialized);
        size_t na = 0, nb = 0; ray_enc_hdr_t ea, eb; uint64_t fa, fb;
        uint8_t* ra = ix_file_region(ray_test_tmp(a), &na, &ea, &fa);
        uint8_t* rb = ix_file_region(ray_test_tmp(b), &nb, &eb, &fb);
        bool same = ra && rb && na == nb && memcmp(ra, rb, na) == 0;
        free(ra); free(rb);
        TEST_ASSERT_FMT(same && eb.index_kind == kinds[k] && eb.end_off == fb, "kind %u: recorded %u", kinds[k], eb.index_kind);
        ray_release(c);
        c = ray_col_mmap(ray_test_tmp(b));
        ray_t* m = c && !RAY_IS_ERR(c) ? ray_enccol_materialize(c) : NULL;
        TEST_ASSERT_FMT(m && !RAY_IS_ERR(m) && ray_index_kind(m) == kinds[k], "kind %u: the re-saved column", kinds[k]);
        ray_release(c); ray_release(v);
    }
    PASS();
}

/* Review focus 3: a generation-0 passenger root's order_sym is written -1. */
static test_result_t test_passenger_order_absent(void) {
    int64_t d[8] = { 1, 1, 2, 2, 3, 3, 4, 4 };
    ray_t* v = with_index(i64_vec(d, 8), ray_index_attach_part);
    TEST_ASSERT_NOT_NULL(v);
    ray_index_payload(v->index)->u.part.order_sym = ray_sym_intern("ts", 2);
    TEST_ASSERT_EQ_I(ray_col_save(v, ray_test_tmp(IDX_TMP "/g0")), RAY_OK);          /* generation 0 */
    ray_t* g0 = ray_col_mmap(ray_test_tmp(IDX_TMP "/g0"));
    TEST_ASSERT_TRUE(g0 && !RAY_IS_ERR(g0) && ray_index_kind(g0) == RAY_IDX_PART);
    TEST_ASSERT_TRUE(ray_index_payload(g0->index)->markers & RAY_MARK_MMAP);
    TEST_ASSERT_EQ_I(ray_col_save_gen(g0, ray_test_tmp(IDX_TMP "/g2"), 2), RAY_OK);
    size_t n; ray_enc_hdr_t eh; uint64_t fn;
    uint8_t* r = ix_file_region(ray_test_tmp(IDX_TMP "/g2"), &n, &eh, &fn);
    TEST_ASSERT_NOT_NULL(r);
    ray_enc_idx_part_t p; memcpy(&p, r + 16, 40);
    TEST_ASSERT_EQ_I(p.order_name, -1);
    free(r);
    /* the heap root of this process converts */
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(IDX_TMP "/h2"), 2), RAY_OK);
    r = ix_file_region(ray_test_tmp(IDX_TMP "/h2"), &n, &eh, &fn);
    TEST_ASSERT_NOT_NULL(r);
    memcpy(&p, r + 16, 40);
    TEST_ASSERT_TRUE(p.order_name > 0 && (size_t)p.order_name + 10 <= n && memcmp(r + p.order_name + 8, "ts", 2) == 0);
    free(r); ray_release(g0); ray_release(v);
    PASS();
}

/* Every ADMITTED (kind, parent type) pair goes builder -> write -> validate
 * -> parse -> attach, and the parsed children equal the builder's byte for
 * byte (spec "Completion additions"; "PART key types: save, load,
 * validation ... on BOOL, U8, I16, I32, I64, F32, F64, DATE, TIME,
 * TIMESTAMP and SYM").  PART equality probes then agree on every key. */
static int ix_children_equal_raw(const ray_index_t* a, const ray_index_t* b);
/* the builder's index `a` as a region holds it, against the parsed `b` */
static int ix_children_equal(const ray_index_t* a, const ray_index_t* b) {
    ray_index_t av; ray_t* hold[4];
    int r = ix_children_equal_raw(ix_region_view(a, &av, hold), b);
    ix_region_view_free(hold);
    return r;
}
static int ix_children_equal_raw(const ray_index_t* a, const ray_index_t* b) {
    ray_t* ca[4]; ray_t* cb[4];
    int na = ray_index_child_blocks(a, ca, 4), nb = ray_index_child_blocks(b, cb, 4);
    if (na != nb) return 0;
    for (int i = 0; i < na; i++) {
        if (ca[i]->type != cb[i]->type || ca[i]->len != cb[i]->len) return 0;
        if (ca[i]->type == RAY_SYM) {
            for (int64_t k = 0; k < ca[i]->len; k++)
                if (ray_read_sym(ray_data(ca[i]), k, RAY_SYM, ca[i]->attrs) != ray_read_sym(ray_data(cb[i]), k, RAY_SYM, cb[i]->attrs)) return 0;
        } else if (memcmp(ray_data(ca[i]), ray_data(cb[i]), (size_t)ca[i]->len * ray_sym_elem_size(ca[i]->type, ca[i]->attrs)) != 0) {
            return 0;
        }
    }
    return 1;
}
/* Row r of a numeric or SYM column as the int64 key the probes take. */
static int64_t ix_key_at(ray_t* v, int64_t r) {
    if (v->type == RAY_SYM) return ray_read_sym(ray_data(v), r, RAY_SYM, v->attrs);
    const uint8_t* d = ray_data(v);
    switch (ray_sym_elem_size(v->type, 0)) {
    case 1: return d[r];
    case 2: { int16_t x; memcpy(&x, d + r * 2, 2); return x; }
    case 4: { int32_t x; memcpy(&x, d + r * 4, 4); return x; }
    default: { int64_t x; memcpy(&x, d + r * 8, 8); return x; }
    }
}
/* A column of `type`, 4096 rows, ascending value blocks of 64 (so PART
 * admits it), no nulls; SYM ids 1..64 of the runtime domain. */
static ray_t* ix_typed_col(int8_t type) {
    for (int i = 0; i < 70; i++) { char b[8]; int n = snprintf(b, sizeof b, "t%d", i); (void)ray_sym_intern(b, (size_t)n); }
    ray_t* v = type == RAY_SYM ? ray_sym_vec_new(RAY_SYM_W8, 4096) : ray_vec_new(type, 4096);
    v->len = 4096;
    uint8_t* d = ray_data(v);
    for (int64_t i = 0; i < 4096; i++) {
        int64_t k = i / 64;                       /* 0..63 */
        switch (type) {
        case RAY_BOOL: d[i] = (uint8_t)(k >= 32); break;
        case RAY_U8:   d[i] = (uint8_t)k; break;
        case RAY_I16:  { int16_t x = (int16_t)(k - 32); memcpy(d + i * 2, &x, 2); break; }
        case RAY_I32: case RAY_DATE: case RAY_TIME: { int32_t x = (int32_t)(k * 1000 - 5000); memcpy(d + i * 4, &x, 4); break; }
        case RAY_F32:  { float x = (float)k * 0.5f - 3.0f; memcpy(d + i * 4, &x, 4); break; }
        case RAY_F64:  { double x = (double)k * 0.25 - 2.0; memcpy(d + i * 8, &x, 8); break; }
        case RAY_SYM:  d[i] = (uint8_t)(k + 1); break;
        default:       { int64_t x = k * 1000003 - 7; memcpy(d + i * 8, &x, 8); break; }   /* I64, TIMESTAMP */
        }
    }
    return v;
}
static test_result_t test_admitted_roundtrip(void) {
    static const int8_t types[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_F32, RAY_F64,
                                    RAY_DATE, RAY_TIME, RAY_TIMESTAMP, RAY_SYM };
    static ray_t* (*const att[4])(ray_t**) = { ray_index_attach_hash, ray_index_attach_sort,
                                              ray_index_attach_bloom, ray_index_attach_part };
    static const uint8_t kinds[4] = { RAY_IDX_HASH, RAY_IDX_SORT, RAY_IDX_BLOOM, RAY_IDX_PART };
    int done = 0;
    for (size_t t = 0; t < sizeof types; t++)
        for (int k = 0; k < 4; k++) {
            if (!ray_index_kind_admits((ray_idx_kind_t)kinds[k], types[t])) continue;
            ray_t* v = with_index(ix_typed_col(types[t]), att[k]);
            TEST_ASSERT_FMT(v != NULL, "builder kind %u type %d", kinds[k], types[t]);
            ray_t* p = ix_roundtrip(v, NULL, 0);
            TEST_ASSERT_FMT(p != NULL, "round trip kind %u type %d", kinds[k], types[t]);
            ray_index_t* a = ray_index_payload(v->index);
            ray_index_t* b = ray_index_payload(p->index);
            TEST_ASSERT_FMT(a->kind == b->kind && a->built_for_len == b->built_for_len && ix_children_equal(a, b),
                            "children kind %u type %d", kinds[k], types[t]);
            if (kinds[k] == RAY_IDX_PART && types[t] != RAY_F32 && types[t] != RAY_F64)
                for (int64_t r = 0; r < 4096; r += 640) {
                    int64_t key = ix_key_at(v, r);
                    ray_t* sa = ray_index_part_eq_rowsel(v, key);
                    ray_t* sb = ray_index_part_eq_rowsel(p, key);
                    TEST_ASSERT_FMT((sa == NULL) == (sb == NULL), "part probe type %d", types[t]);
                    if (sa) { TEST_ASSERT_EQ_I(ray_rowsel_meta(sa)->total_pass, ray_rowsel_meta(sb)->total_pass);
                              ray_rowsel_release(sa); ray_rowsel_release(sb); }
                }
            ray_release(p); ray_release(v);
            done++;
        }
    ray_t* s = ray_vec_new(RAY_STR, 600);
    for (int i = 0; i < 600; i++) { char b[32]; int n = snprintf(b, sizeof b, "string key %d long", i % 23); s = ray_str_vec_append(s, b, (size_t)n); }
    ray_t* sv = with_index(s, ray_index_attach_hash);
    ray_t* sp = ix_roundtrip(sv, NULL, 0);
    TEST_ASSERT_TRUE(sp && ix_children_equal(ray_index_payload(sv->index), ray_index_payload(sp->index)));
    ray_release(sp); ray_release(sv);
    TEST_ASSERT_EQ_I(done, 10 * 4 + 2);          /* ten numeric types x four kinds, SYM HASH and PART */
    PASS();
}

/* Spec "Completion additions": every admitted and every rejected (kind,
 * parent type) pair through the writer, the validator and the attach. */
static test_result_t test_admission_through_all(void) {
    static const int8_t types[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_F32, RAY_F64,
                                    RAY_DATE, RAY_TIME, RAY_TIMESTAMP, RAY_SYM, RAY_STR, RAY_GUID };
    static const uint8_t kinds[] = { RAY_IDX_HASH, RAY_IDX_SORT, RAY_IDX_BLOOM, RAY_IDX_PART };
    int rejected = 0;
    for (size_t t = 0; t < sizeof types; t++)
        for (size_t k = 0; k < sizeof kinds; k++) {
            bool adm = ray_index_kind_admits((ray_idx_kind_t)kinds[k], types[t]);
            ray_t* root = ray_index_new_root((ray_idx_kind_t)kinds[k], types[t], 0);
            ray_enc_idx_src_t s = { root, types[t], 0, 0, NULL, 0 };
            FILE* f = fopen(ray_test_tmp(IDX_TMP "/adm"), "wb");
            uint64_t bytes;
            ray_t* we = ray_enc_index_write(f, &s, &bytes);
            fclose(f);
            if (!adm) {
                rejected++;
                TEST_ASSERT_FMT(we && strcmp(ray_err_code(we), "nyi") == 0, "writer kind %u type %d", kinds[k], types[t]);
                ray_t* col = types[t] == RAY_SYM ? ray_sym_vec_new(RAY_SYM_W8, 0) : ray_vec_new(types[t], 0);
                ray_retain(root);
                ray_t* ae = ray_index_attach_persisted(&col, root);
                TEST_ASSERT_FMT(ae && RAY_IS_ERR(ae) && strcmp(ray_err_code(ae), "corrupt") == 0, "attach kind %u type %d", kinds[k], types[t]);
                ray_error_free(ae); ray_release(col);
            }
            if (we) ray_error_free(we);
            ray_release(root);
        }
    TEST_ASSERT_EQ_I(rejected, 13 * 4 - (10 * 4 + 3));     /* every pair test_admitted_roundtrip does not build */
    PASS();
}

/* Carry from Task 5 (review minor 4): a stale region root on a numeric
 * column of at least one chunk records no kind of its own, so a splayed
 * save records the chunk zone generation 0 builds for such a column. */
static test_result_t test_stale_root_chunk_zone(void) {
    ray_t* v = ix_hash_col(70000);
    TEST_ASSERT_NOT_NULL(v);
    ray_index_payload(v->index)->built_for_len = 69999;                 /* stale */
    ray_t* t = ray_table_new(1);
    t = ray_table_add_col(t, ray_sym_intern("v", 1), v);
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, ray_test_tmp(IDX_TMP "/stz/"), NULL, RAY_COMPRESS_ON), RAY_OK);
    char p[1200];
    TEST_ASSERT_TRUE(ix_live_path(IDX_TMP "/stz/", "v", p, sizeof p));
    TEST_ASSERT_EQ_U(ray_col_file_gen(p), 2);
    size_t n; ray_enc_hdr_t eh; uint64_t fn;
    TEST_ASSERT_NULL(ix_file_region(p, &n, &eh, &fn));
    TEST_ASSERT_EQ_I(eh.index_kind, RAY_IDX_CHUNK_ZONE);
    ray_index_payload(v->index)->built_for_len = 70000;
    ray_release(t); ray_release(v);
    PASS();
}

/* D1: the SYM HASH / PART a splayed save writes as generation 2 keep the
 * source's UNIQUE (checked against the in-memory source), and every query
 * answers as on generation 0.  Generation 0's own save drops the SYM PART
 * and UNIQUE (splay_build_index_col rebuilds a bare HASH), the attribute
 * difference D1 accepts. */
static test_result_t test_sym_splay_kept(void) {
    ray_t* r = ixr_eval("(set SY (as 'SYM (map (fn [i] (concat \"k\" (as 'STR i))) (til 6000))))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r)); ray_release(r);
    r = ixr_eval("(set TQ (table [p g v] (list (.attr.set 'parted (asc (take ['a 'b 'c 'd] 6000))) "
                 "(.attr.set 'grouped (.attr.set 'unique SY)) (til 6000))))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r)); ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/sq0/\" TQ {compress: false})"); TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r)); ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/sq2/\" TQ {compress: true})"); TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r)); ray_release(r);
    r = ixr_eval("(set X0 (.db.splayed.get \"" IDX_TMP "/sq0/\"))"); TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r)); ray_release(r);
    r = ixr_eval("(set X2 (.db.splayed.get \"" IDX_TMP "/sq2/\"))"); TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r)); ray_release(r);
    char pp[1200], pg[1200];
    TEST_ASSERT_TRUE(ix_live_path(IDX_TMP "/sq2/", "p", pp, sizeof pp) && ix_live_path(IDX_TMP "/sq2/", "g", pg, sizeof pg));
    ray_enc_hdr_t hp = ix_hdr(pp), hg = ix_hdr(pg);
    TEST_ASSERT_EQ_U(ray_col_file_gen(pp), 2);
    TEST_ASSERT_EQ_U(ray_col_file_gen(pg), 2);
    TEST_ASSERT_EQ_I(hp.index_kind, RAY_IDX_PART);
    TEST_ASSERT_EQ_I(hg.index_kind, RAY_IDX_HASH);
    TEST_ASSERT_EQ_I(hg.col_flags, RAY_ENC_FLAG_UNIQUE);
    /* the attributes of the in-memory source, on generation 2 */
    static const char* const kept[] = {
        "(if (== (in 'parted (.attr.get (at TQ 'p))) (in 'parted (.attr.get (at X2 'p)))) 1 0)",
        "(if (== (in 'unique (.attr.get (at TQ 'g))) (in 'unique (.attr.get (at X2 'g)))) 1 0)",
        "(if (== (in 'grouped (.attr.get (at TQ 'g))) (in 'grouped (.attr.get (at X2 'g)))) 1 0)",
        "(if (in 'parted (.attr.get (at TQ 'p))) 1 0)",
        "(if (in 'unique (.attr.get (at TQ 'g))) 1 0)",
        NULL,
    };
    for (int i = 0; kept[i]; i++) TEST_ASSERT_FMT(ixr_i64(kept[i]) == 1, "%s", kept[i]);
    /* generation 0's loss, as D1 records it (dev's splay_build_index_col) */
    TEST_ASSERT_EQ_I(ixr_i64("(if (in 'parted (.attr.get (at X0 'p))) 1 0)"), 0);
    TEST_ASSERT_EQ_I(ixr_i64("(if (in 'unique (.attr.get (at X0 'g))) 1 0)"), 0);
    static const char* const qs[] = {
        "(sum (at (select {from: %s where: (== p 'b)}) 'v))",
        "(sum (at (select {from: %s where: (in p ['a 'd])}) 'v))",
        "(sum (at (select {from: %s where: (== p 'zz)}) 'v))",
        "(sum (at (select {from: %s where: (== g 'k4242)}) 'v))",
        "(sum (at (select {from: %s where: (in g ['k1 'k5999 'nope])}) 'v))",
        "(sum (at (select {from: %s where: (!= g 'k17)}) 'v))",
        "(count (select {from: %s where: (and (== p 'c) (> v 4000))}))",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        char q0[200], q2[200];
        snprintf(q0, sizeof q0, qs[i], "X0"); snprintf(q2, sizeof q2, qs[i], "X2");
        int64_t a = ixr_i64(q0), b = ixr_i64(q2);
        TEST_ASSERT_FMT(a == b && a != INT64_MIN, "%s: %lld vs %lld", qs[i], (long long)a, (long long)b);
    }
    PASS();
}

/* ---- end to end: save paths, converter parity (Task 8) ---- */

/* The formatted value of `src` (owned string), or NULL on an error. */
static ray_t* ixr_fmt(const char* src) {
    ray_t* r = ixr_eval(src);
    if (!r || RAY_IS_ERR(r)) { if (r) ray_error_free(r); return NULL; }
    ray_t* f = ray_fmt(r, 0);
    ray_release(r);
    return f;
}
static bool ixr_same_fmt(ray_t* a, ray_t* b) {
    return a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b) &&           /* an error never matches */
           ray_str_len(a) == ray_str_len(b) && memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) == 0;
}
/* Two tables with the same column names and, column by column, the same
 * formatted values (a table's own format is only its shape). */
static bool ixr_same_table(ray_t* a, ray_t* b) {
    if (!a || !b || a->type != RAY_TABLE || b->type != RAY_TABLE || ray_table_ncols(a) != ray_table_ncols(b)) return false;
    bool same = true;
    for (int64_t c = 0; same && c < ray_table_ncols(a); c++) {
        if (ray_table_col_name(a, c) != ray_table_col_name(b, c)) return false;
        ray_t* ca = ray_table_col_idx(a, c); ray_t* cb = ray_table_col_idx(b, c);
        if (!ca || !cb || RAY_IS_ERR(ca) || RAY_IS_ERR(cb)) return false;
        ray_t* fa = ray_fmt(ca, 0); ray_t* fb = ray_fmt(cb, 0);
        same = ixr_same_fmt(fa, fb);
        if (fa) { if (RAY_IS_ERR(fa)) ray_error_free(fa); else ray_release(fa); }
        if (fb) { if (RAY_IS_ERR(fb)) ray_error_free(fb); else ray_release(fb); }
    }
    return same;
}

/* Every index the save path persists on generation 0 is on the generation-2
 * copy, and no auto pass touched a gen-2 file.  STR DICT (P6b): the
 * converter persists generation 0's DICT on generation 2 (over 65536 rows,
 * no HASH), recorded in the header before materialization and attached to
 * the materialized column after it. */
static test_result_t test_converter_parity(void) {
    FILE* f = fopen(ray_test_tmp(IDX_TMP "/in.csv"), "w");
    TEST_ASSERT_NOT_NULL(f);
    fprintf(f, "u,c,s\n");
    for (int i = 0; i < 200000; i++) fprintf(f, "%d,%d,name%d\n", (i * 7919) % 1000003, i / 10, i % 50);
    fclose(f);
    ray_t* r;
    r = ixr_eval("(.csv.splayed {compress: false} [I64 I64 STR] \"" IDX_TMP "/in.csv\" \"" IDX_TMP "/c0/\")"); if (r) ray_release(r);
    r = ixr_eval("(.csv.splayed {compress: true} [I64 I64 STR] \"" IDX_TMP "/in.csv\" \"" IDX_TMP "/c2/\")"); if (r) ray_release(r);
    r = ixr_eval("(set A (.db.splayed.get \"" IDX_TMP "/c0/\"))"); if (r) ray_release(r);
    r = ixr_eval("(set C (.db.splayed.get \"" IDX_TMP "/c2/\"))"); if (r) ray_release(r);
    static const char* const cols[] = { "u", "c", NULL };
    for (int i = 0; cols[i]; i++) {
        char q0[256], q2[256], g2[256];
        snprintf(q0, sizeof q0, "(at (.idx.info (at A '%s)) 'kind)", cols[i]);
        snprintf(q2, sizeof q2, "(at (.idx.info C '%s) 'index)", cols[i]);
        snprintf(g2, sizeof g2, "(at (.idx.info C '%s) 'gen)", cols[i]);
        TEST_ASSERT_EQ_I(ixr_i64(g2), 2);
        int64_t k0 = ixr_i64(q0);
        TEST_ASSERT_TRUE(k0 != INT64_MIN);
        TEST_ASSERT_EQ_I(k0, ray_sym_intern("chunk_zone", 10));       /* the kind is there */
        TEST_ASSERT_EQ_I(k0, ixr_i64(q2));                             /* chunk_zone on both */
    }
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info C 's) 'gen)"), 2);
    /* P6b: generation 2 persists the DICT (spec "STR dictionary index"),
     * recorded before materialization and attached after it */
    TEST_ASSERT_EQ_I(ixr_i64("(if (at (.idx.info C 's) 'materialized) 1 0)"), 0);
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info C 's) 'index)"), ray_sym_intern("dict", 4));   /* recorded */
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info (at A 's)) 'kind)"), ray_sym_intern("dict", 4));
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info (at C 's)) 'kind)"), ray_sym_intern("dict", 4));   /* attached */
    TEST_ASSERT_EQ_I(ixr_i64("(if (at (.idx.info C 's) 'materialized) 1 0)"), 1);
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info C 's) 'index)"), ray_sym_intern("dict", 4));
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info (at C 's)) 'n_distinct)"), ixr_i64("(at (.idx.info (at A 's)) 'n_distinct)"));
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info (at C 's)) 'n_distinct)"), 50);
    /* explicit indexes, then .db.splayed.set: generation 2 keeps the kind */
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/e2/\" (table [u s] (list (.idx.hash (at A 'u)) (.idx.hash (at A 's)))) {compress: true})"); if (r) ray_release(r);
    r = ixr_eval("(set E (.db.splayed.get \"" IDX_TMP "/e2/\"))"); if (r) ray_release(r);
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info E 'u) 'gen)"), 2);
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info E 'u) 'index)"), ray_sym_intern("hash", 4));
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info E 's) 'gen)"), 2);
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info E 's) 'index)"), ray_sym_intern("hash", 4));
    TEST_ASSERT_EQ_I(ixr_i64("(count (select {from: E where: (== s \"name7\")}))"), 4000);
    PASS();
}

/* Task 2 review Minor 2: the converter's DICT floor is generation 0's,
 * 65536 rows.  At 65535 rows neither generation has a DICT (kind 0 recorded);
 * at 65536 both do (kind 7 recorded, read from the header, nothing
 * materialized). */
static test_result_t test_converter_dict_boundary(void) {
    static const int rows[2] = { 65535, 65536 };
    for (int b = 0; b < 2; b++) {
        char csv[128], q[600];
        snprintf(csv, sizeof csv, IDX_TMP "/b%d.csv", rows[b]);
        FILE* f = fopen(ray_test_tmp(csv), "w");
        TEST_ASSERT_NOT_NULL(f);
        fprintf(f, "v,s\n");
        for (int i = 0; i < rows[b]; i++) fprintf(f, "%d,name%d\n", i, (i * 7919) % 300);
        fclose(f);
        for (int g = 0; g <= 2; g += 2) {
            snprintf(q, sizeof q, "(.csv.splayed {compress: %s} [I64 STR] \"%s\" \"" IDX_TMP "/b%d_%d/\")",
                     g ? "true" : "false", csv, rows[b], g);
            ray_t* r = ixr_eval(q); if (r) ray_release(r);
        }
        snprintf(q, sizeof q, "(.db.splayed.get \"" IDX_TMP "/b%d_2/\")", rows[b]);
        ray_t* t = ixr_eval(q);
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_col_meta_t m;
        ray_enc_counters_reset();
        const bool got = ray_table_col_meta(t, ray_sym_intern("s", 1), &m);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        ray_release(t);
        TEST_ASSERT_TRUE(got);
        TEST_ASSERT_FMT(m.enc != NULL, "%d rows: generation 2", rows[b]);
        TEST_ASSERT_FMT(m.index_kind == (b ? RAY_IDX_DICT : RAY_IDX_NONE), "%d rows: kind %u", rows[b], m.index_kind);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        /* generation 0 over the same file: the same decision */
        if (b) snprintf(q, sizeof q, "(at (.idx.info (at (.db.splayed.get \"" IDX_TMP "/b%d_0/\") 's)) 'kind)", rows[b]);
        else   snprintf(q, sizeof q, "(if (.idx.has? (at (.db.splayed.get \"" IDX_TMP "/b%d_0/\") 's)) 1 0)", rows[b]);
        TEST_ASSERT_EQ_I(ixr_i64(q), b ? ray_sym_intern("dict", 4) : 0);
    }
    PASS();
}

/* Spec "Index use materializes once": a point lookup on an indexed column
 * materializes at most that column.  An equality on a recorded HASH now
 * probes the region where it is mapped (ray_enc_hash_eq_rowsel) and
 * materializes nothing; an `in` on it still materializes the column for
 * its index, and only that column. */
static test_result_t test_use_materializes_once(void) {
    ray_t* r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/m2/\" (table [k v w] (list (.idx.hash (% (* (til 200000) 7919) 100003)) (til 200000) (map (fn [j] (format \"t%\" j)) (til 200000)))) {compress: true})");
    if (r) ray_release(r);
    r = ixr_eval("(set M (.db.splayed.get \"" IDX_TMP "/m2/\"))"); if (r) ray_release(r);
    ray_enc_counters_reset();
    int64_t got = ixr_i64("(count (at (select {v: v w: w from: M where: (== k 4242)}) 'v))");   /* the select runs the filter; count of its column, not count(select) */
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_I(got, ixr_i64("(count (where (== (% (* (til 200000) 7919) 100003) 4242)))"));
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_I(ixr_i64("(if (at (.idx.info M 'v) 'materialized) 1 0)"), 0);
    TEST_ASSERT_EQ_I(ixr_i64("(if (at (.idx.info M 'k) 'materialized) 1 0)"), 0);
    ray_enc_counters_reset();
    got = ixr_i64("(count (at (select {v: v w: w from: M where: (in k [4242 4243])}) 'v))");
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_I(got, ixr_i64("(count (where (in (% (* (til 200000) 7919) 100003) [4242 4243])))"));
    TEST_ASSERT_EQ_U(k.materialized, 1);
    TEST_ASSERT_EQ_I(ixr_i64("(if (at (.idx.info M 'v) 'materialized) 1 0)"), 0);
    TEST_ASSERT_EQ_I(ixr_i64("(if (at (.idx.info M 'k) 'materialized) 1 0)"), 1);
    PASS();
}

/* Parted (Task 6b minor 5): a parted table consults its per-segment indexes
 * on generation 2 exactly as on generation 0: every site's consults and
 * hits, the answers, and one materialization per segment HASH consult on
 * generation 0 (the consult-set rule), so a segment that is pruned or not
 * consulted is never decoded.  At this HEAD no parted shape reaches a
 * segment's index on generation 0 (the parted select answers by its scan;
 * a bare FILTER graph over a constant table does not stream), so the
 * counters are zero on both and generation 2 materializes nothing; when
 * generation 0 starts consulting a segment, the same assertions require
 * generation 2 to follow. */
static test_result_t test_parted_segment_index(void) {
    for (int gen = 0; gen <= 2; gen += 2)
        for (int d = 1; d <= 3; d++) {
            char q[600];
            snprintf(q, sizeof q, "(.db.splayed.set \"" IDX_TMP "/pt%d/2024.01.0%d/t/\" (table [k v] (list (.idx.hash (%% (* (til 70000) %d) 10007)) (til 70000))) {compress: %s})",
                     gen, d, d + 6, gen ? "true" : "false");
            ray_t* r = ixr_eval(q); if (r) ray_release(r);
        }
    /* a segment of each generation, loaded alone, records / attaches its HASH */
    TEST_ASSERT_EQ_I(ixr_i64("(if (== (at (.idx.info (at (.db.splayed.get \"" IDX_TMP "/pt0/2024.01.02/t/\") 'k)) 'kind) 'hash) 1 0)"), 1);
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info (.db.splayed.get \"" IDX_TMP "/pt2/2024.01.02/t/\") 'k) 'index)"), ray_sym_intern("hash", 4));
    static const char* const qs[] = {
        "(sum (at (select {from: PT where: (and (== date 2024.01.01) (== k 42))}) 'v))",
        "(sum (at (select {from: PT where: (== k 42)}) 'v))",
        "(sum (at (select {from: PT where: (in k [1 42 77])}) 'v))",
        "(sum (at (select {v: v from: PT where: (and (== date 2024.01.02) (== k 7))}) 'v))",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        uint64_t c[2][IDX_SITE__N], h[2][IDX_SITE__N], mat[2];
        int64_t cnt[2];
        for (int g = 0; g < 2; g++) {
            char q[300];
            snprintf(q, sizeof q, "(set PT (.db.parted.get \"" IDX_TMP "/pt%d\" 't))", g * 2);
            ray_t* r = ixr_eval(q); if (r) ray_release(r);
            uint64_t c0[IDX_SITE__N], h0[IDX_SITE__N];
            memcpy(c0, ray_idx_consults, sizeof c0); memcpy(h0, ray_idx_hits, sizeof h0);
            ray_enc_counters_reset();
            cnt[g] = ixr_i64(qs[i]);
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            mat[g] = k.materialized;
            for (int s = 0; s < IDX_SITE__N; s++) { c[g][s] = ray_idx_consults[s] - c0[s]; h[g][s] = ray_idx_hits[s] - h0[s]; }
        }
        TEST_ASSERT_FMT(cnt[0] != INT64_MIN && cnt[0] == cnt[1], "%s: %lld vs generation 0 %lld", qs[i], (long long)cnt[1], (long long)cnt[0]);
        for (int s = 0; s < IDX_SITE__N; s++)
            TEST_ASSERT_FMT(c[0][s] == c[1][s] && h[0][s] == h[1][s], "%s: site %d consults %llu/%llu hits %llu/%llu", qs[i], s,
                            (unsigned long long)c[0][s], (unsigned long long)c[1][s], (unsigned long long)h[0][s], (unsigned long long)h[1][s]);
        TEST_ASSERT_FMT(mat[0] == 0 && mat[1] == c[0][IDX_SITE_FILTER_HASH], "%s: generation 2 materialized %llu, generation 0 consulted %llu segment HASHes",
                        qs[i], (unsigned long long)mat[1], (unsigned long long)c[0][IDX_SITE_FILTER_HASH]);
    }
    PASS();
}

/* Spec "Indexes / UNIQUE": unique-only and HASH+UNIQUE numeric columns from a
 * heap source and from a generation-0 passenger source: the flag is known
 * before materialization (metadata, nothing decoded) and .attr.get matches
 * generation 0 after it. */
static test_result_t test_unique_attr_get(void) {
    ray_t* r = ixr_eval("(set U (table [u h] (list (.attr.set 'unique (til 70000)) (.attr.set 'unique (.idx.hash (* (til 70000) 3))))))");
    if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/u0/\" U {compress: false})"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/u2/\" U {compress: true})"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/up/\" (.db.splayed.get \"" IDX_TMP "/u0/\") {compress: true})"); if (r) ray_release(r);  /* passenger source */
    static const char* const dirs[2] = { IDX_TMP "/u2/", IDX_TMP "/up/" };
    for (int d = 0; d < 2; d++)
        for (int c = 0; c < 2; c++) {
            const char* col = c ? "h" : "u";
            char q[300];
            snprintf(q, sizeof q, "(.db.splayed.get \"%s\")", dirs[d]);
            ray_t* t = ixr_eval(q);
            TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
            ray_col_meta_t m;
            ray_enc_counters_reset();
            TEST_ASSERT_TRUE(ray_table_col_meta(t, ray_sym_intern(col, 1), &m));
            TEST_ASSERT_EQ_I(m.enc != NULL, 1);                          /* generation 2, not decoded */
            TEST_ASSERT_FMT(m.unique, "%s %s: UNIQUE known from the header", dirs[d], col);
            ray_enc_counters_t k; ray_enc_counters_get(&k);
            TEST_ASSERT_EQ_U(k.materialized, 0);
            ray_release(t);
            char q0[300], q2[300];
            snprintf(q0, sizeof q0, "(.attr.get (at (.db.splayed.get \"" IDX_TMP "/u0/\") '%s))", col);
            snprintf(q2, sizeof q2, "(.attr.get (at (.db.splayed.get \"%s\") '%s))", dirs[d], col);
            ray_t* fa = ixr_fmt(q0); ray_t* fb = ixr_fmt(q2);
            TEST_ASSERT_FMT(ixr_same_fmt(fa, fb), "%s %s: .attr.get differs (%.*s vs %.*s)", dirs[d], col,
                            fa ? (int)ray_str_len(fa) : 5, fa ? ray_str_ptr(fa) : "error",
                            fb ? (int)ray_str_len(fb) : 5, fb ? ray_str_ptr(fb) : "error");
            if (fa) ray_release(fa);
            if (fb) ray_release(fb);
        }
    PASS();
}

/* Spec "Normalized bytes: the same table saved from different allocations
 * and domains writes byte-identical regions": one SYM HASH / PART table,
 * once from the runtime domain and once from a FILE domain with another
 * insertion order, saved into one target symfile. */
static test_result_t test_normalized_domains(void) {
    ray_t* r;
    r = ixr_eval("(set N (table [k p] (list (.idx.hash (at ['zeta 'alpha 'mu 'omega] (% (* (til 6000) 7) 4))) (.attr.set 'parted (at ['b 'a 'c] (div (til 6000) 2000))))))");
    if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/nd1/\" N {compress: false})"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/ndA/\" N \"" IDX_TMP "/target.sym\" {compress: true})"); if (r) ray_release(r);
    /* the generation-0 save drops the SYM PART (D1, #838): the FILE-domain
     * copy re-attaches it over its own ids */
    r = ixr_eval("(set F (.db.splayed.get \"" IDX_TMP "/nd1/\"))"); if (r) ray_release(r);
    TEST_ASSERT_TRUE(ixr_i64("(if (== (at (.idx.info (at F 'k)) 'kind) 'hash) 1 0)") == 1);
    r = ixr_eval("(set F (table [k p] (list (at F 'k) (.attr.set 'parted (at F 'p)))))"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/ndB/\" F \"" IDX_TMP "/target.sym\" {compress: true})");
    if (r) ray_release(r);
    static const char* const cols[2] = { "k", "p" };
    for (int c = 0; c < 2; c++) {
        char pa[1200], pb[1200];
        TEST_ASSERT_TRUE(ix_live_path(IDX_TMP "/ndA/", cols[c], pa, sizeof pa));   /* the published generation's file */
        TEST_ASSERT_TRUE(ix_live_path(IDX_TMP "/ndB/", cols[c], pb, sizeof pb));
        size_t na = 0, nb = 0; ray_enc_hdr_t ea, eb; uint64_t fa, fb;
        uint8_t* ra = ix_file_region(pa, &na, &ea, &fa); uint8_t* rb = ix_file_region(pb, &nb, &eb, &fb);
        bool same = ra && rb && na == nb && memcmp(ra, rb, na) == 0;
        free(ra); free(rb);
        TEST_ASSERT_FMT(same, "column %s: regions differ", cols[c]);
        TEST_ASSERT_TRUE(ea.index_kind == (c ? RAY_IDX_PART : RAY_IDX_HASH) && eb.index_kind == ea.index_kind);
    }
    PASS();
}

/* Spec "Indexes / order_sym": a PART ordered by `ts`, loaded from generation
 * 2, takes the index-ordered as-of route (the IDX_SITE_ASOF_ORDERED counter);
 * a name naming no column disables it and the results stay generation 0's.
 * The key is I64: the as-of index variant needs a right side proven
 * null-free, which a SYM column never is (ray_vec_may_have_nulls), so a SYM
 * key takes no index route on either generation (#842). */
static test_result_t test_order_sym_fast_path(void) {
    ray_t* r;
    r = ixr_eval("(set Q (xasc (xasc (table [id ts bid] (list (% (til 3000) 3) (til 3000) (as 'F64 (til 3000)))) ['id 'ts]) ['id 'ts]))");
    if (r) ray_release(r);
    r = ixr_eval("(set L (table [id ts] (list [0 1 2] [100 200 2999])))"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/os0/\" Q {compress: false})"); if (r) ray_release(r);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/os2/\" Q {compress: true})"); if (r) ray_release(r);
    /* the same table with order_sym naming no column */
    ray_t* q = ray_eval_str("Q");
    TEST_ASSERT_TRUE(q && !RAY_IS_ERR(q));
    ray_t* qc = ray_table_col(q, ray_sym_intern("id", 2));
    TEST_ASSERT_EQ_I(ray_index_kind(qc), RAY_IDX_PART);
    int64_t keep = ray_index_payload(qc->index)->u.part.order_sym;
    TEST_ASSERT_EQ_I(keep, ray_sym_intern("ts", 2));
    ray_index_payload(qc->index)->u.part.order_sym = ray_sym_intern("no_such_col", 11);
    r = ixr_eval("(.db.splayed.set \"" IDX_TMP "/osn/\" Q {compress: true})"); if (r) ray_release(r);
    ray_index_payload(qc->index)->u.part.order_sym = keep;
    ray_release(q);
    /* os2 and osn are generation-2 files that record the PART */
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info (.db.splayed.get \"" IDX_TMP "/os2/\") 'id) 'gen)"), 2);
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info (.db.splayed.get \"" IDX_TMP "/os2/\") 'id) 'index)"), ray_sym_intern("part", 4));
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info (.db.splayed.get \"" IDX_TMP "/osn/\") 'id) 'gen)"), 2);
    TEST_ASSERT_EQ_I(ixr_i64("(at (.idx.info (.db.splayed.get \"" IDX_TMP "/osn/\") 'id) 'index)"), ray_sym_intern("part", 4));
    /* 0: the in-memory Q (heap root, order_sym ts: the ordered route in this
     * process); 1: the generation-0 load (the answer reference); 2: the
     * generation-2 load; 3: the generation-2 load whose name block names no
     * column. */
    static const char* const srcs[4] = { "Q", "(.db.splayed.get \"" IDX_TMP "/os0/\")",
                                         "(.db.splayed.get \"" IDX_TMP "/os2/\")", "(.db.splayed.get \"" IDX_TMP "/osn/\")" };
    ray_t* out[4]; uint64_t ordered[4];
    for (int d = 0; d < 4; d++) {
        char qs[400];
        snprintf(qs, sizeof qs, "(asof-join [id ts] L %s)", srcs[d]);
        uint64_t c0 = ray_idx_consults[IDX_SITE_ASOF_ORDERED];
        out[d] = ixr_eval(qs);
        TEST_ASSERT_TRUE(out[d] && !RAY_IS_ERR(out[d]));
        ordered[d] = ray_idx_consults[IDX_SITE_ASOF_ORDERED] - c0;
    }
    TEST_ASSERT_TRUE(ordered[0] >= 1);            /* the heap root: the ordered route */
    TEST_ASSERT_EQ_U(ordered[2], ordered[0]);     /* generation 2: the same route through the name block */
    TEST_ASSERT_EQ_U(ordered[3], 0);              /* no such column: off */
    for (int d = 0; d < 4; d++) {
        if (d == 1) continue;
        TEST_ASSERT_FMT(ixr_same_table(out[1], out[d]), "as-of %d differs from generation 0", d);
    }
    for (int d = 0; d < 4; d++) ray_release(out[d]);
    PASS();
}


/* ===== HASH distinctness in the reachability walk (Task 10b) =====
 *
 * The validator checks distinct gkeys in its reachability walk: a key equal
 * to group g's has g's home, so it lies in [home, p) of g's bucket p.  The
 * fabrications below insert groups in order by linear probing (as a builder
 * does), with a repeated key placed where only that walk, its long-distance
 * re-read, its sorted fallback or the pool pass can find it. */

/* ---- P6b: the DICT region (spec "Index region", "Index validation") ---- */

/* A DICT source over 8 rows: codes 0 1 0 2 1 0 2 3, first_occ 0 1 3 7,
 * entries "a", "bb", "", "dddd" (the empty entry is the in-band null). */
static const int32_t  dx_codes[8] = { 0, 1, 0, 2, 1, 0, 2, 3 };
static const int32_t  dx_focc[4]  = { 0, 1, 3, 7 };
static const uint64_t dx_offs[4]  = { 0, 1, 3, 3 };
static const char     dx_bytes[]  = "abbdddd";      /* 7 bytes */
static ray_enc_dict_src_t dx_src(void) {
    return (ray_enc_dict_src_t){ .len = 8, .n_distinct = 4, .codes = dx_codes, .first_occ = dx_focc,
                                 .offs = dx_offs, .bytes = dx_bytes, .bytes_len = 7 };
}
/* The region of s, written to a file and read back into a 32-aligned
 * buffer (*n bytes; the caller frees). */
static uint8_t* dx_region(const ray_enc_dict_src_t* s, size_t* n, ray_t** err) {
    FILE* f = fopen(ray_test_tmp(IDX_TMP "/dr"), "wb");
    uint64_t nb = 0;
    *err = f ? ray_enc_dict_write(f, s, &nb) : ray_error("io", NULL);
    if (f) fclose(f);
    if (*err) return NULL;
    size_t got = 0;
    uint8_t* raw = ix_slurp(ray_test_tmp(IDX_TMP "/dr"), &got);
    if (!raw || got != nb) { free(raw); return NULL; }
    uint8_t* b = aligned_alloc(32, (got + 31) & ~(size_t)31);
    memcpy(b, raw, got); free(raw);
    *n = got;
    return b;
}
/* Layout: root fields, children at pad32 offsets from 96, region end. */
static test_result_t test_dict_write_layout(void) {
    ray_enc_dict_src_t s = dx_src();
    size_t n; ray_t* err;
    uint8_t* b = dx_region(&s, &n, &err);
    TEST_ASSERT_NULL(err);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQ_I((int8_t)b[0], RAY_STR);
    TEST_ASSERT_EQ_I(rd64(b + 8), 8);
    ray_enc_idx_dict_t d; memcpy(&d, b + 16, sizeof d);
    TEST_ASSERT_EQ_I(d.codes, 96);
    TEST_ASSERT_EQ_I(d.first_occ, ix_p32(96 + 8 * 4));
    TEST_ASSERT_EQ_I(d.offs, ix_p32(d.first_occ + 4 * 4));
    TEST_ASSERT_EQ_I(d.bytes, ix_p32(d.offs + 5 * 8));
    TEST_ASSERT_EQ_I(d.n_distinct, 4);
    TEST_ASSERT_EQ_U(d.bytes_len, 7);
    for (int i = 16 + 48; i < 96; i++) TEST_ASSERT_EQ_I(b[i], 0);   /* kind-area tail, root padding */
    TEST_ASSERT_EQ_U(n, ix_p32((uint64_t)d.bytes + 7));
    uint64_t last; memcpy(&last, b + d.offs + 4 * 8, 8);
    TEST_ASSERT_EQ_U(last, 7);                                      /* the terminal offset */
    TEST_ASSERT_TRUE(memcmp(b + d.bytes, "abbdddd", 7) == 0);
    free(b);
    PASS();
}

/* The region validates; the view points at its children. */
static test_result_t test_dict_view_fields(void) {
    ray_enc_dict_src_t s = dx_src();
    size_t n; ray_t* err;
    uint8_t* b = dx_region(&s, &n, &err);
    TEST_ASSERT_NULL(err);
    ray_enc_dict_view_t v;
    ray_enc_dict_parse(b, &v);
    TEST_ASSERT_EQ_I(v.n_distinct, 4);
    TEST_ASSERT_EQ_I(v.len, 8);
    TEST_ASSERT_TRUE(memcmp(v.codes, dx_codes, sizeof dx_codes) == 0);
    TEST_ASSERT_TRUE(memcmp(v.first_occ, dx_focc, sizeof dx_focc) == 0);
    TEST_ASSERT_EQ_U(v.offs[4], 7);
    free(b);
    PASS();
}

/* The writer refuses a source no validator would accept: nyi naming DICT. */
static test_result_t test_dict_write_rejects(void) {
    ray_enc_dict_src_t s = dx_src();
    FILE* f = fopen(ray_test_tmp(IDX_TMP "/dw"), "wb");
    uint64_t nb;
    s.n_distinct = 0;
    ray_t* e = ray_enc_dict_write(f, &s, &nb);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "nyi") == 0 && strstr(ray_error_msg(), "DICT"));
    ray_error_free(e);
    s = dx_src(); s.n_distinct = 9;                                    /* above len */
    e = ray_enc_dict_write(f, &s, &nb);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "nyi") == 0); ray_error_free(e);
    s = dx_src(); s.bytes_len = 2;                                     /* the last entry starts past the bytes */
    e = ray_enc_dict_write(f, &s, &nb);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "nyi") == 0); ray_error_free(e);
    s = dx_src(); s.len = (int64_t)INT32_MAX + 1;
    e = ray_enc_dict_write(f, &s, &nb);
    TEST_ASSERT_TRUE(e && strcmp(ray_err_code(e), "nyi") == 0); ray_error_free(e);
    fclose(f);
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
    { "idxval/view_fields",        test_view_fields,        ix_setup, ix_teardown },
    { "idxval/str_word_collision",      test_str_word_collision,      ix_setup, ix_teardown },
    { "idxval/view_empty",          test_view_empty,          ix_setup, ix_teardown },
    { "idxval/view_engine_matrix",  test_view_engine_matrix,  ix_setup, ix_teardown },
    { "idxval/parse_hash_probes",       test_parse_hash_probes,       ix_setup, ix_teardown },
    { "idxval/parse_part_order_sym",    test_parse_part_order_sym,    ix_setup, ix_teardown },
    { "idxval/parse_oom_frees",         test_parse_oom_frees,         ix_setup, ix_teardown },
    { "idxval/file_has_region",         test_file_has_region,         ix_setup, ix_teardown },
    { "idxval/unique_and_stale",        test_unique_and_stale,        ix_setup, ix_teardown },
    { "idxval/save_nyi_names_kind",     test_save_nyi_names_kind,     ix_setup, ix_teardown },
    { "idxval/save_override_kind",      test_save_override_kind,      ix_setup, ix_teardown },
    { "idxval/golden_emit",             test_idx_golden_emit,         ix_setup, ix_teardown },
    { "idxval/golden_match",            test_idx_golden_match,        ix_setup, ix_teardown },
    { "idxval/materialize_attaches",    test_materialize_attaches,    ix_setup, ix_teardown },
    { "idxval/materialize_all_kinds",   test_materialize_all_kinds,   ix_setup, ix_teardown },
    { "idxval/materialize_unique",      test_materialize_unique,      ix_setup, ix_teardown },
    { "idxval/parse_oom_retry",         test_parse_oom_retry,         ix_setup, ix_teardown },
    { "idxval/materialize_oom_sweep",   test_materialize_oom_sweep,   ix_setup, ix_teardown },
    { "idxval/retained_index_outlives_table", test_retained_index_outlives_table, ix_setup, ix_teardown },
    { "idxval/concurrent_index",        test_concurrent_index,        ix_setup, ix_teardown },
    { "idxval/golden_read",             test_idx_golden_read,         ix_setup, ix_teardown },
    { "idxval/consult_routes",        test_consult_routes,        ixr_setup, ixr_teardown },
    { "idxval/consult_dir_unchanged", test_consult_dir_unchanged, ixr_setup, ixr_teardown },
    { "idxval/index_route_oom",       test_index_route_oom,       ixr_setup, ixr_teardown },
    { "idxval/hash_region_probe",     test_hash_region_probe,     ixr_setup, ixr_teardown },
    { "idxval/sym_rebuild_remap",      test_sym_rebuild_remap,      ix_setup, ix_teardown },
    { "idxval/sym_part_rebuild",       test_sym_part_rebuild,       ix_setup, ix_teardown },
    { "idxval/encoded_keep_copies",    test_encoded_keep_copies,    ix_setup, ix_teardown },
    { "idxval/drop_records_none",      test_drop_records_none,      ix_setup, ix_teardown },
    { "idxval/reload_keeps_kind",      test_reload_keeps_kind,      ix_setup, ix_teardown },
    { "idxval/passenger_order_absent", test_passenger_order_absent, ix_setup, ix_teardown },
    { "idxval/admitted_roundtrip",     test_admitted_roundtrip,     ix_setup, ix_teardown },
    { "idxval/admission_through_all",  test_admission_through_all,  ix_setup, ix_teardown },
    { "idxval/stale_root_chunk_zone",  test_stale_root_chunk_zone,  ixr_setup, ixr_teardown },
    { "idxval/sym_splay_kept",         test_sym_splay_kept,         ixr_setup, ixr_teardown },
    { "idxval/sym_stale_rebuild",      test_sym_stale_rebuild,      ixr_setup, ixr_teardown },
    { "idxval/sym_stale_part_unbuildable", test_sym_stale_part_unbuildable, ix_setup, ix_teardown },
    { "idxval/sym_hash_null_key",  test_sym_hash_null_key,  ixr_setup, ixr_teardown },
    { "idxval/converter_parity",      test_converter_parity,      ixr_setup, ixr_teardown },
    { "idxval/converter_dict_boundary", test_converter_dict_boundary, ixr_setup, ixr_teardown },
    { "idxval/use_materializes_once", test_use_materializes_once, ixr_setup, ixr_teardown },
    { "idxval/parted_segment_index",  test_parted_segment_index,  ixr_setup, ixr_teardown },
    { "idxval/unique_attr_get",       test_unique_attr_get,       ixr_setup, ixr_teardown },
    { "idxval/normalized_domains",    test_normalized_domains,    ixr_setup, ixr_teardown },
    { "idxval/order_sym_fast_path",   test_order_sym_fast_path,   ixr_setup, ixr_teardown },
    { "idxval/dict_write_layout",     test_dict_write_layout,     ix_setup, ix_teardown },
    { "idxval/dict_view_fields", test_dict_view_fields, ix_setup, ix_teardown },
    { "idxval/dict_write_rejects",    test_dict_write_rejects,    ix_setup, ix_teardown },
    { NULL, NULL, NULL, NULL },
};
