/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
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
#include "mem/cow.h"
#include "vec/vec.h"
#include "table/sym.h"
#include "ops/idxop.h"
#include "ops/rowsel.h"
#include "store/col.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>

/* ─── Helpers ──────────────────────────────────────────────────────── */

static ray_t* make_i64_vec(const int64_t* xs, int64_t n) {
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &xs[i]);
    return v;
}

static ray_t* make_f64_vec(const double* xs, int64_t n) {
    ray_t* v = ray_vec_new(RAY_F64, n);
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &xs[i]);
    return v;
}

/* Snapshot the 16-byte aux union and attrs bits we care about. */
typedef struct {
    uint8_t bytes[16];
    uint8_t attrs;  /* HAS_NULLS */
} aux_snap_t;

static aux_snap_t snap_take(const ray_t* v) {
    aux_snap_t s;
    memcpy(s.bytes, v->aux, 16);
    s.attrs = v->attrs & RAY_ATTR_HAS_NULLS;
    return s;
}

static int snap_eq(const aux_snap_t* a, const aux_snap_t* b) {
    return memcmp(a->bytes, b->bytes, 16) == 0 && a->attrs == b->attrs;
}

/* ─── Tests ────────────────────────────────────────────────────────── */

static test_result_t test_index_attach_drop_no_nulls(void) {
    ray_heap_init();
    int64_t xs[] = { 5, 1, 9, 3, 7 };
    ray_t* v = make_i64_vec(xs, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_FALSE(v->attrs & RAY_ATTR_HAS_INDEX);

    aux_snap_t before = snap_take(v);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_NOT_NULL(w->index);
    TEST_ASSERT_TRUE(w->index->type == RAY_INDEX);

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)ix->kind, RAY_IDX_ZONE);
    TEST_ASSERT_EQ_I(ix->u.zone.min_i, 1);
    TEST_ASSERT_EQ_I(ix->u.zone.max_i, 9);
    TEST_ASSERT_EQ_I(ix->u.zone.n_nulls, 0);

    /* Drop and verify the aux union round-trips byte-for-byte. */
    ray_t* d = ray_index_drop(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_FALSE(w->attrs & RAY_ATTR_HAS_INDEX);

    aux_snap_t after = snap_take(w);
    TEST_ASSERT_TRUE(snap_eq(&before, &after));

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_attach_drop_with_inline_nulls(void) {
    ray_heap_init();
    int64_t xs[] = { 10, 20, 30, 40, 50 };
    ray_t* v = make_i64_vec(xs, 5);
    /* Mark element 1 and 3 as null using the public API. */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 3, true), RAY_OK);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);

    aux_snap_t before = snap_take(v);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ix->u.zone.min_i, 10);
    TEST_ASSERT_EQ_I(ix->u.zone.max_i, 50);
    TEST_ASSERT_EQ_I(ix->u.zone.n_nulls, 2);

    /* While the index is attached, ray_vec_is_null must still report
     * the original null state via the saved snapshot. */
    TEST_ASSERT_FALSE(ray_vec_is_null(w, 0));
    TEST_ASSERT_TRUE (ray_vec_is_null(w, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(w, 2));
    TEST_ASSERT_TRUE (ray_vec_is_null(w, 3));
    TEST_ASSERT_FALSE(ray_vec_is_null(w, 4));

    /* Drop and verify the snapshot is restored. */
    ray_index_drop(&w);
    aux_snap_t after = snap_take(w);
    TEST_ASSERT_TRUE(snap_eq(&before, &after));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_NULLS);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_attach_drop_large_sentinel_nulls(void) {
    /* Attach + drop on a vec with sentinel-encoded nulls past the
     * 128-element boundary.  Verifies null state survives the round-trip
     * via ray_vec_is_null. */
    ray_heap_init();
    int64_t n = 200;
    ray_t* v = ray_vec_new(RAY_I32, n);
    int32_t z = 0;
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &z);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 130, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 199, true), RAY_OK);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);

    /* is_null still returns true for the marked rows under HAS_INDEX. */
    TEST_ASSERT_TRUE (ray_vec_is_null(w, 130));
    TEST_ASSERT_TRUE (ray_vec_is_null(w, 199));
    TEST_ASSERT_FALSE(ray_vec_is_null(w, 0));

    ray_index_drop(&w);
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE (ray_vec_is_null(w, 130));
    TEST_ASSERT_TRUE (ray_vec_is_null(w, 199));
    TEST_ASSERT_FALSE(ray_vec_is_null(w, 0));

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_replace_existing(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3 };
    ray_t* v = make_i64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r1 = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    ray_t* idx1 = w->index;
    (void)idx1;

    ray_t* r2 = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    /* Should still have an index; the old one is gone. */
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_mutation_drops(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3 };
    ray_t* v = make_i64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);

    /* set_null mutates -> must drop the index. */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(w, 0, true), RAY_OK);
    TEST_ASSERT_FALSE(w->attrs & RAY_ATTR_HAS_INDEX);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_float_zone(void) {
    ray_heap_init();
    double xs[] = { 1.5, -3.25, 4.0, 0.0 };
    ray_t* v = make_f64_vec(xs, 4);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)ix->kind, RAY_IDX_ZONE);
    TEST_ASSERT_TRUE(ix->u.zone.min_f == -3.25);
    TEST_ASSERT_TRUE(ix->u.zone.max_f == 4.0);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_unsupported_type(void) {
    ray_heap_init();
    /* RAY_SYM is rejected by all index kinds in v1 (str_pool/sym_dict
     * displacement sweep deferred). */
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 4);
    int64_t s = ray_sym_intern("hello", 5);
    v = ray_vec_append(v, &s);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_FALSE(w->attrs & RAY_ATTR_HAS_INDEX);

    ray_release(w);
    if (RAY_IS_ERR(r)) ray_error_free(r);
    ray_heap_destroy();
    PASS();
}

/* ─── Hash index ──────────────────────────────────────────────────── */

static test_result_t test_index_hash_attach_drop(void) {
    ray_heap_init();
    int64_t xs[] = { 7, 3, 7, 9, 3, 1 };  /* duplicates and uniques */
    ray_t* v = make_i64_vec(xs, 6);
    aux_snap_t before = snap_take(v);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)ix->kind, RAY_IDX_HASH);
    TEST_ASSERT_EQ_I(ix->u.hash.n_keys, 6);
    TEST_ASSERT_EQ_I(ix->u.hash.n_groups, 4);  /* {7,3,9,1} */
    TEST_ASSERT_NOT_NULL(ix->u.hash.table);
    TEST_ASSERT_NOT_NULL(ix->u.hash.gkeys);
    TEST_ASSERT_NOT_NULL(ix->u.hash.offs);
    TEST_ASSERT_NOT_NULL(ix->u.hash.rows);
    TEST_ASSERT_EQ_I(ix->u.hash.rows->len, 6);
    TEST_ASSERT_EQ_I(ix->u.hash.offs->len, 5);

    /* CSR slice for key 7: rows 0 and 2 (both store 7), ascending. */
    const int64_t* grows = NULL;
    int64_t gn = 0;
    TEST_ASSERT_EQ_I(ray_index_hash_group(w, 7, &grows, &gn), 1);
    TEST_ASSERT_EQ_I(gn, 2);
    TEST_ASSERT_EQ_I(grows[0], 0);
    TEST_ASSERT_EQ_I(grows[1], 2);
    /* Absent key → provable miss. */
    TEST_ASSERT_EQ_I(ray_index_hash_group(w, 42, &grows, &gn), 0);

    ray_index_drop(&w);
    aux_snap_t after = snap_take(w);
    TEST_ASSERT_TRUE(snap_eq(&before, &after));

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_hash_with_nulls_preserved(void) {
    ray_heap_init();
    int64_t xs[] = { 10, 20, 30, 40 };
    ray_t* v = make_i64_vec(xs, 4);
    /* Mark row 1 null. */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);
    aux_snap_t before = snap_take(v);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ix->u.hash.n_keys, 3);  /* null row excluded */

    /* Null still readable through ray_vec_is_null. */
    TEST_ASSERT_TRUE(ray_vec_is_null(w, 1));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_NULLS);

    ray_index_drop(&w);
    aux_snap_t after = snap_take(w);
    TEST_ASSERT_TRUE(snap_eq(&before, &after));

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Sort index ──────────────────────────────────────────────────── */

static test_result_t test_index_sort_attach_drop(void) {
    ray_heap_init();
    int64_t xs[] = { 5, 1, 9, 3, 7 };
    ray_t* v = make_i64_vec(xs, 5);
    aux_snap_t before = snap_take(v);

    ray_t* w = v;
    ray_t* r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)ix->kind, RAY_IDX_SORT);
    TEST_ASSERT_NOT_NULL(ix->u.sort.perm);
    TEST_ASSERT_EQ_I(ix->u.sort.perm->len, 5);

    /* perm should rank values asc — smallest first.
     * xs = [5,1,9,3,7]; asc: 1@1, 3@3, 5@0, 7@4, 9@2. */
    int64_t* p = (int64_t*)ray_data(ix->u.sort.perm);
    int64_t expected[] = { 1, 3, 0, 4, 2 };
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQ_I(p[i], expected[i]);
    }

    ray_index_drop(&w);
    aux_snap_t after = snap_take(w);
    TEST_ASSERT_TRUE(snap_eq(&before, &after));

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Bloom filter ────────────────────────────────────────────────── */

static test_result_t test_index_bloom_attach_drop(void) {
    ray_heap_init();
    int64_t xs[] = { 11, 22, 33, 44, 55 };
    ray_t* v = make_i64_vec(xs, 5);
    aux_snap_t before = snap_take(v);

    ray_t* w = v;
    ray_t* r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)ix->kind, RAY_IDX_BLOOM);
    TEST_ASSERT_EQ_I(ix->u.bloom.n_keys, 5);
    TEST_ASSERT_EQ_I((int)ix->u.bloom.k, 3);
    TEST_ASSERT_NOT_NULL(ix->u.bloom.bits);

    /* Some bits must be set (5 keys * 3 = 15 bit-set ops). */
    uint8_t* bb = (uint8_t*)ray_data(ix->u.bloom.bits);
    int popcount = 0;
    for (int64_t i = 0; i < ix->u.bloom.bits->len; i++) {
        for (int b = 0; b < 8; b++) if (bb[i] & (1u << b)) popcount++;
    }
    TEST_ASSERT_TRUE(popcount > 0);

    ray_index_drop(&w);
    aux_snap_t after = snap_take(w);
    TEST_ASSERT_TRUE(snap_eq(&before, &after));

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Shared-COW: drop on one holder must not break the other ────── */

static test_result_t test_index_drop_under_shared_cow(void) {
    ray_heap_init();
    int64_t xs[] = { 100, 200, 300, 400, 500 };
    ray_t* a = make_i64_vec(xs, 5);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(a, 1, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(a, 3, true), RAY_OK);

    /* Attach a zone index. */
    ray_t* x = a;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_zone(&x)));
    TEST_ASSERT_TRUE(x->attrs & RAY_ATTR_HAS_INDEX);

    /* Force a COW share: retain x and ray_alloc_copy via ray_cow.
     * After this, both a' and b point at the same RAY_INDEX block (rc=2). */
    ray_retain(x);
    ray_retain(x);   /* simulate two outstanding references */
    ray_t* b = ray_cow(x);
    TEST_ASSERT_TRUE(b != x);
    TEST_ASSERT_TRUE(b->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_TRUE(b->index == x->index);
    /* Index ray_t now has rc>=2 (held by both x and b). */
    TEST_ASSERT_TRUE(ray_atomic_load(&x->index->rc) >= 2);

    /* Drop the index from x.  This must not corrupt b's view. */
    ray_t* x2 = x;
    ray_index_drop(&x2);
    TEST_ASSERT_FALSE(x2->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_TRUE(b->attrs & RAY_ATTR_HAS_INDEX);

    /* b must still report the original null state correctly. */
    TEST_ASSERT_FALSE(ray_vec_is_null(b, 0));
    TEST_ASSERT_TRUE (ray_vec_is_null(b, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(b, 2));
    TEST_ASSERT_TRUE (ray_vec_is_null(b, 3));
    TEST_ASSERT_FALSE(ray_vec_is_null(b, 4));

    /* And x2 (with index dropped) must also report correctly. */
    TEST_ASSERT_FALSE(ray_vec_is_null(x2, 0));
    TEST_ASSERT_TRUE (ray_vec_is_null(x2, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(x2, 2));
    TEST_ASSERT_TRUE (ray_vec_is_null(x2, 3));

    ray_release(x2);
    ray_release(b);
    ray_heap_destroy();
    PASS();
}

/* ─── Persistence round-trip on indexed vec ───────────────────────── */

static test_result_t test_index_persistence_roundtrip(void) {
    ray_heap_init();
    /* 200 elements is past the legacy 128-inline-bitmap boundary. */
    int64_t n = 200;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t x = i * 10;
        v = ray_vec_append(v, &x);
    }
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 7, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 150, true), RAY_OK);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);

    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_zone(&w)));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);

    /* Save through col.c — must NOT write the index pointer to disk. */
    char path[] = "/tmp/idx_persist_test_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);
    ray_err_t err = ray_col_save(w, path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load back and verify shape + null bits. */
    ray_t* loaded = ray_col_load(path);
    unlink(path);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_I64);
    TEST_ASSERT_EQ_I(loaded->len, n);
    /* HAS_INDEX must NOT survive serialization. */
    TEST_ASSERT_FALSE(loaded->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_TRUE(loaded->attrs & RAY_ATTR_HAS_NULLS);

    /* Null bits must round-trip. */
    TEST_ASSERT_FALSE(ray_vec_is_null(loaded, 0));
    TEST_ASSERT_TRUE (ray_vec_is_null(loaded, 7));
    TEST_ASSERT_FALSE(ray_vec_is_null(loaded, 100));
    TEST_ASSERT_TRUE (ray_vec_is_null(loaded, 150));
    TEST_ASSERT_FALSE(ray_vec_is_null(loaded, 199));

    /* Data must round-trip. */
    int64_t* d = (int64_t*)ray_data(loaded);
    TEST_ASSERT_EQ_I(d[0], 0);
    TEST_ASSERT_EQ_I(d[10], 100);
    TEST_ASSERT_EQ_I(d[199], 1990);

    ray_release(loaded);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Slice null detection on indexed/parent vec ───────────────────── */

static test_result_t test_index_aux_helper_slice(void) {
    /* Slice-relative null detection via ray_vec_is_null delegates to
     * the parent's sentinel payload at the translated index. */
    ray_heap_init();
    int64_t xs[] = { 100, 200, 300, 400, 500, 600 };
    ray_t* v = make_i64_vec(xs, 6);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 4, true), RAY_OK);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);

    ray_t* s = ray_vec_slice(v, 2, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_TRUE(s->attrs & RAY_ATTR_SLICE);
    /* A slice inherits the parent's HAS_NULLS hint (#495): the gates
     * that read the bit would otherwise treat the window as null-free. */
    TEST_ASSERT_TRUE(s->attrs & RAY_ATTR_HAS_NULLS);

    /* ray_vec_is_null still works correctly on the slice. */
    TEST_ASSERT_FALSE(ray_vec_is_null(s, 0));   /* parent row 2 — not null */
    TEST_ASSERT_FALSE(ray_vec_is_null(s, 1));   /* parent row 3 — not null */
    TEST_ASSERT_TRUE (ray_vec_is_null(s, 2));   /* parent row 4 — null */
    TEST_ASSERT_FALSE(ray_vec_is_null(s, 3));   /* parent row 5 — not null */

    ray_release(s);
    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* ─── Mutator invalidation (insert_at) ────────────────────────────── */

static test_result_t test_index_insert_at_drops_index(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3, 4, 5 };
    ray_t* v = make_i64_vec(xs, 5);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_zone(&w)));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);

    /* In-place insert at idx 2 — must drop the index before mutating. */
    int64_t v99 = 99;
    w = ray_vec_insert_at(w, 2, &v99);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w));
    TEST_ASSERT_FALSE(w->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_EQ_I(w->len, 6);

    /* The data must be intact after the index drop + insert. */
    int64_t* d = (int64_t*)ray_data(w);
    int64_t expected[] = { 1, 2, 99, 3, 4, 5 };
    for (int i = 0; i < 6; i++) TEST_ASSERT_EQ_I(d[i], expected[i]);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Null-aware reader correctness on indexed vec ─────────────────── */

static test_result_t test_index_null_readers_through_helper(void) {
    /* Verify the sentinel-based null reader invariant: ray_vec_is_null
     * returns the same answer before and after an index attach, even
     * though w->aux[0..7] holds the index pointer after attach. */
    ray_heap_init();
    int64_t xs[] = { 100, 200, 300, 400, 500 };
    ray_t* v = make_i64_vec(xs, 5);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 2, true), RAY_OK);

    TEST_ASSERT_TRUE (ray_vec_is_null(v, 2));
    TEST_ASSERT_FALSE(ray_vec_is_null(v, 0));

    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_zone(&w)));

    /* After attach the index pointer overlays bytes 0-7 of the union;
     * sentinel-based readers must still see the null at row 2. */
    TEST_ASSERT_TRUE (ray_vec_is_null(w, 2));
    TEST_ASSERT_FALSE(ray_vec_is_null(w, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(w, 4));

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Attach replaces (cross-kind) ────────────────────────────────── */

static test_result_t test_index_replace_cross_kind(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3, 4, 5 };
    ray_t* v = make_i64_vec(xs, 5);

    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_zone(&w)));
    TEST_ASSERT_EQ_I((int)ray_index_kind(w), RAY_IDX_ZONE);

    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));
    TEST_ASSERT_EQ_I((int)ray_index_kind(w), RAY_IDX_HASH);

    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_sort(&w)));
    TEST_ASSERT_EQ_I((int)ray_index_kind(w), RAY_IDX_SORT);

    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_bloom(&w)));
    TEST_ASSERT_EQ_I((int)ray_index_kind(w), RAY_IDX_BLOOM);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── BOOL/U8 zone + hash (covers numeric_elem_size case 1, zone_scan bool/u8,
 *     numeric_key_word case 1) ─────────────────────────────────────────── */

static test_result_t test_index_bool_zone_and_hash(void) {
    ray_heap_init();
    uint8_t xs[] = { 1, 0, 1, 1, 0 };
    ray_t* v = ray_vec_new(RAY_BOOL, 5);
    for (int i = 0; i < 5; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)iz->kind, RAY_IDX_ZONE);
    TEST_ASSERT_EQ_I(iz->u.zone.min_i, 0);
    TEST_ASSERT_EQ_I(iz->u.zone.max_i, 1);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 0);
    ray_index_drop(&w);

    /* BOOL hash */
    r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)ih->kind, RAY_IDX_HASH);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 5);
    ray_index_drop(&w);

    /* RAY_U8 */
    ray_t* uv = ray_vec_new(RAY_U8, 4);
    uint8_t us[] = { 10, 200, 10, 50 };
    for (int i = 0; i < 4; i++) uv = ray_vec_append(uv, &us[i]);
    ray_t* uw = uv;
    r = ray_index_attach_zone(&uw);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iuz = ray_index_payload(uw->index);
    TEST_ASSERT_EQ_I(iuz->u.zone.min_i, 10);
    TEST_ASSERT_EQ_I(iuz->u.zone.max_i, 200);
    ray_index_drop(&uw);

    r = ray_index_attach_hash(&uw);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iuh = ray_index_payload(uw->index);
    TEST_ASSERT_EQ_I(iuh->u.hash.n_keys, 4);

    ray_release(uw);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── I16 zone + hash (covers numeric_elem_size case 2, zone_scan i16,
 *     numeric_key_word case 2) ─────────────────────────────────────────── */

static test_result_t test_index_i16_zone_and_hash(void) {
    ray_heap_init();
    int16_t xs[] = { -100, 0, 200, -32768, 32767 };
    ray_t* v = ray_vec_new(RAY_I16, 5);
    for (int i = 0; i < 5; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(iz->u.zone.min_i, -32768);
    TEST_ASSERT_EQ_I(iz->u.zone.max_i, 32767);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 0);
    ray_index_drop(&w);

    r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── I32 hash (covers numeric_key_word case 4) ─────────────────────────── */

static test_result_t test_index_i32_hash(void) {
    ray_heap_init();
    int32_t xs[] = { 1000000, -1, 0, 2147483647, -2147483648 };
    ray_t* v = ray_vec_new(RAY_I32, 5);
    for (int i = 0; i < 5; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F32 zone + hash (covers zone_scan_float elem_size 4, zone_scan RAY_F32,
 *     numeric_key_word F32 path) ─────────────────────────────────────────── */

static test_result_t test_index_f32_zone_and_hash(void) {
    ray_heap_init();
    float xs[] = { 1.5f, -2.5f, 0.0f, 100.0f };
    ray_t* v = ray_vec_new(RAY_F32, 4);
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)iz->kind, RAY_IDX_ZONE);
    TEST_ASSERT_TRUE(iz->u.zone.min_f == -2.5);
    TEST_ASSERT_TRUE(iz->u.zone.max_f == 100.0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 0);
    /* Call ray_index_info on the F32 zone to cover the F32 branch
     * (ix->parent_type == RAY_F32) in ray_index_info, line 650. */
    r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_t* info = ray_index_info(w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(info));
    ray_release(info);
    ray_index_drop(&w);

    r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 4);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── TIME / TIMESTAMP zone (covers zone_scan RAY_TIME, RAY_TIMESTAMP) ───── */

static test_result_t test_index_time_timestamp_zone(void) {
    ray_heap_init();

    /* RAY_TIME: stored as int32_t (4 bytes); zone_scan routes via
     * zone_scan_int(v, ix, 4) matching the storage width, so min/max
     * are exact. */
    int32_t times[] = { 0, 3600, 86399, 1000 };
    ray_t* tv = ray_vec_new(RAY_TIME, 4);
    for (int i = 0; i < 4; i++) tv = ray_vec_append(tv, &times[i]);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tv));
    ray_t* tw = tv;
    ray_t* r = ray_index_attach_zone(&tw);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* itz = ray_index_payload(tw->index);
    TEST_ASSERT_EQ_I((int)itz->kind, RAY_IDX_ZONE);
    TEST_ASSERT_EQ_I(itz->u.zone.min_i, 0);
    TEST_ASSERT_EQ_I(itz->u.zone.max_i, 86399);
    TEST_ASSERT_EQ_I(itz->u.zone.n_nulls, 0);
    ray_release(tw);

    /* RAY_TIMESTAMP (int64_t, 8 bytes) */
    int64_t ts[] = { 1700000000000000000LL, 0LL, 1000000LL };
    ray_t* sv = ray_vec_new(RAY_TIMESTAMP, 3);
    for (int i = 0; i < 3; i++) sv = ray_vec_append(sv, &ts[i]);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sv));
    ray_t* sw = sv;
    r = ray_index_attach_zone(&sw);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* isz = ray_index_payload(sw->index);
    TEST_ASSERT_EQ_I(isz->u.zone.min_i, 0LL);
    TEST_ASSERT_EQ_I(isz->u.zone.max_i, 1700000000000000000LL);
    ray_release(sw);

    ray_heap_destroy();
    PASS();
}

/* ─── DATE zone (covers zone_scan RAY_DATE, elem_size 4) ─────────────────── */

static test_result_t test_index_date_zone(void) {
    ray_heap_init();
    int32_t dates[] = { 0, 18000, -365, 36500 };  /* days since epoch */
    ray_t* v = ray_vec_new(RAY_DATE, 4);
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &dates[i]);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)iz->kind, RAY_IDX_ZONE);
    TEST_ASSERT_EQ_I(iz->u.zone.min_i, -365);
    TEST_ASSERT_EQ_I(iz->u.zone.max_i, 36500);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 0);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Zone scan all-null (covers the !any_value branch: mn=0, mx=0) ───────── */

static test_result_t test_index_zone_all_null(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3 };
    ray_t* v = make_i64_vec(xs, 3);
    /* Mark every element null. */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 0, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 2, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    /* All null: min and max collapse to 0. */
    TEST_ASSERT_EQ_I(iz->u.zone.min_i, 0);
    TEST_ASSERT_EQ_I(iz->u.zone.max_i, 0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 3);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Zone scan float all-null (covers !any_value in zone_scan_float) ───── */

static test_result_t test_index_zone_float_all_null(void) {
    ray_heap_init();
    double xs[] = { 1.0, 2.0 };
    ray_t* v = make_f64_vec(xs, 2);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 0, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_TRUE(iz->u.zone.min_f == 0.0);
    TEST_ASSERT_TRUE(iz->u.zone.max_f == 0.0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 2);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Zone scan float with NaN (NaN skipped in float zone) ───────────────── */

static test_result_t test_index_zone_float_nan(void) {
    ray_heap_init();
    double xs[] = { 1.0, (double)NAN, 3.0, (double)NAN };
    ray_t* v = make_f64_vec(xs, 4);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    /* NaN rows are skipped, so min=1.0, max=3.0, n_nulls=0 */
    TEST_ASSERT_TRUE(iz->u.zone.min_f == 1.0);
    TEST_ASSERT_TRUE(iz->u.zone.max_f == 3.0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 0);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Hash index NaN key (covers numeric_key_word NaN branch) ───────────── */

static test_result_t test_index_hash_f64_nan(void) {
    ray_heap_init();
    double xs[] = { 1.0, (double)NAN, 2.0, (double)NAN };
    ray_t* v = make_f64_vec(xs, 4);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    /* All 4 rows are non-null so all 4 get indexed (NaN gets a per-row bucket). */
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 4);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Slice attach error (covers prepare_attach slice guard) ─────────────── */

static test_result_t test_index_attach_slice_error(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3, 4, 5 };
    ray_t* v = make_i64_vec(xs, 5);

    ray_t* s = ray_vec_slice(v, 1, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_TRUE(s->attrs & RAY_ATTR_SLICE);

    ray_t* sw = s;
    ray_t* r = ray_index_attach_zone(&sw);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_FALSE(sw->attrs & RAY_ATTR_HAS_INDEX);

    ray_release(sw);
    if (RAY_IS_ERR(r)) ray_error_free(r);
    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* ─── ray_index_drop: null guard (line 550 true branch) ──────────────────── */

static test_result_t test_index_drop_null_guard(void) {
    ray_heap_init();

    /* Pass vp pointing to NULL — triggers !*vp true branch in ray_index_drop. */
    ray_t* null_v = NULL;
    ray_t* r = ray_index_drop(&null_v);
    /* Returns *vp = NULL: safe no-op. */
    TEST_ASSERT_TRUE(r == NULL);

    /* Pass an error vec to ray_index_drop — covers RAY_IS_ERR(*vp) true branch. */
    ray_t* err_vec = ray_error("test", "synthetic error for coverage");
    r = ray_index_drop(&err_vec);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(err_vec);

    /* Also test that dropping a no-index vec returns it unchanged (line 552). */
    int64_t xs[] = { 1, 2, 3 };
    ray_t* v = make_i64_vec(xs, 3);
    TEST_ASSERT_FALSE(v->attrs & RAY_ATTR_HAS_INDEX);
    r = ray_index_drop(&v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_FALSE(v->attrs & RAY_ATTR_HAS_INDEX);

    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* ─── prepare_attach: null/error vector guard (line 354-355) ──────────────── */

static test_result_t test_index_attach_null_vec(void) {
    ray_heap_init();

    /* Pass vp pointing to NULL: !*vp branch triggers RAY_ERR. */
    ray_t* null_v = NULL;
    ray_t* r1 = ray_index_attach_zone(&null_v);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    if (RAY_IS_ERR(r1)) ray_error_free(r1);

    ray_t* null_v2 = NULL;
    ray_t* r2 = ray_index_attach_hash(&null_v2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    if (RAY_IS_ERR(r2)) ray_error_free(r2);

    ray_t* null_v3 = NULL;
    ray_t* r3 = ray_index_attach_sort(&null_v3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    if (RAY_IS_ERR(r3)) ray_error_free(r3);

    ray_t* null_v4 = NULL;
    ray_t* r4 = ray_index_attach_bloom(&null_v4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    if (RAY_IS_ERR(r4)) ray_error_free(r4);

    /* Pass vp pointing to a RAY_ERROR: RAY_IS_ERR(*vp) branch. */
    ray_t* err = ray_error("test", "synthetic");
    ray_t* err_copy = err;  /* save original for cleanup */
    ray_t* r5 = ray_index_attach_zone(&err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    /* prepare_attach returns a NEW error without touching *vp. */
    ray_error_free(r5);
    ray_error_free(err_copy);

    ray_heap_destroy();
    PASS();
}

/* ─── attach_finalize HAS_LINK branch (covers !HAS_LINK false path) ──────── */

static test_result_t test_index_attach_on_linked_vec(void) {
    ray_heap_init();

    /* We want a vector with RAY_ATTR_HAS_LINK set.  Setting it directly
     * on the block is valid because attach_finalize only reads the bit
     * without dereferencing link_target (it just preserves bytes 8-15). */
    int64_t xs[] = { 0, 1, 2, 0 };
    ray_t* v = make_i64_vec(xs, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* Set HAS_LINK manually — this simulates a linked column. */
    v->attrs |= RAY_ATTR_HAS_LINK;
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_LINK);

    /* Attach a zone index to the HAS_LINK vec — triggers the false branch of
     * `if (!(parent->attrs & RAY_ATTR_HAS_LINK))` in attach_finalize,
     * skipping the `parent->_idx_pad = NULL` assignment. */
    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_LINK);

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)ix->kind, RAY_IDX_ZONE);
    /* min/max should reflect actual data. */
    TEST_ASSERT_EQ_I(ix->u.zone.min_i, 0);
    TEST_ASSERT_EQ_I(ix->u.zone.max_i, 2);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── ray_index_retain_payload: direct call covering HASH/SORT/BLOOM/ZONE ── */

static test_result_t test_index_retain_payload_direct(void) {
    ray_heap_init();

    /* Build a hash index so we have valid table/chain pointers. */
    int64_t xs[] = { 10, 20, 30, 40 };
    ray_t* v = make_i64_vec(xs, 4);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));
    ray_index_t* ix_hash = ray_index_payload(w->index);

    /* Directly call ray_index_retain_payload with a HASH kind index
     * (retains all four CSR children). */
    ray_index_retain_payload(ix_hash);
    /* The children now have rc incremented by 1.
     * Decrement them back to avoid leaking. */
    ray_release(ix_hash->u.hash.table);
    ray_release(ix_hash->u.hash.gkeys);
    ray_release(ix_hash->u.hash.offs);
    ray_release(ix_hash->u.hash.rows);

    /* Drop the hash index, then attach sort and bloom for their retain paths. */
    ray_index_drop(&w);

    /* Sort index. */
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_sort(&w)));
    ray_index_t* ix_sort = ray_index_payload(w->index);
    ray_index_retain_payload(ix_sort);
    ray_release(ix_sort->u.sort.perm);
    ray_index_drop(&w);

    /* Bloom index. */
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_bloom(&w)));
    ray_index_t* ix_bloom = ray_index_payload(w->index);
    ray_index_retain_payload(ix_bloom);
    ray_release(ix_bloom->u.bloom.bits);
    ray_index_drop(&w);

    /* Zone index (ZONE case in retain_payload = fall-through to NONE). */
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_zone(&w)));
    ray_index_t* ix_zone = ray_index_payload(w->index);
    ray_index_retain_payload(ix_zone);  /* no-op for ZONE/NONE */

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── ray_index_release_saved / retain_saved are no-ops ────────────── *
 *
 * Index attachment is restricted to numeric vector types (see
 * prepare_attach), so saved_aux never carries owned ray_t* refs.
 * The functions are kept for call-site symmetry but do nothing.  These
 * tests verify the no-op contract: calling them on a fully populated
 * ix struct must not touch refcounts on whatever pointers happen to
 * sit in the saved bytes. */

static test_result_t test_index_release_saved_noop(void) {
    ray_heap_init();

    int64_t dummy[] = { 1 };
    ray_t* victim = make_i64_vec(dummy, 1);
    uint32_t rc_before = victim->rc;

    ray_index_t ix;
    memset(&ix, 0, sizeof(ix));
    ix.kind = RAY_IDX_ZONE;
    ix.parent_type = RAY_I64;
    ix.saved_attrs = 0;
    /* Put a real pointer into saved_aux[8..15] — if the function
     * were not a no-op it would try to release it and drop the rc. */
    memcpy(&ix.saved_aux[8], &victim, sizeof(victim));

    ray_index_release_saved(&ix);
    TEST_ASSERT_EQ_U(victim->rc, rc_before);

    ray_release(victim);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_retain_saved_noop(void) {
    ray_heap_init();

    int64_t dummy[] = { 1 };
    ray_t* victim = make_i64_vec(dummy, 1);
    uint32_t rc_before = victim->rc;

    ray_index_t ix;
    memset(&ix, 0, sizeof(ix));
    ix.kind = RAY_IDX_ZONE;
    ix.parent_type = RAY_I64;
    ix.saved_attrs = 0;
    memcpy(&ix.saved_aux[8], &victim, sizeof(victim));

    ray_index_retain_saved(&ix);
    TEST_ASSERT_EQ_U(victim->rc, rc_before);

    ray_release(victim);
    ray_heap_destroy();
    PASS();
}

/* ─── Shared-index drop preserves sentinel nulls across COW ─────────────── *
 *
 * When a vec with HAS_INDEX is shared (rc > 1) and then dropped, the
 * drop path takes the shared branch (ray_index_retain_saved + memcpy of
 * saved bytes).  This test verifies the round-trip on a >128-element
 * vec with sentinel-encoded nulls — both copies must still see the nulls
 * via ray_vec_is_null after the drop. */

static test_result_t test_index_drop_shared_with_large_nulls(void) {
    ray_heap_init();
    int64_t n = 150;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t x = i;
        v = ray_vec_append(v, &x);
    }
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 140, true), RAY_OK);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);

    /* Share the index (rc >= 2) so ray_index_drop hits the shared branch. */
    ray_retain(w);
    ray_retain(w);
    ray_t* b = ray_cow(w);
    TEST_ASSERT_TRUE(b != w);
    TEST_ASSERT_TRUE(b->index == w->index);

    /* Drop from w — shared path. */
    ray_t* w2 = w;
    ray_index_drop(&w2);
    TEST_ASSERT_FALSE(w2->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_TRUE(b->attrs & RAY_ATTR_HAS_INDEX);

    /* Both copies still see the null via the payload sentinel. */
    TEST_ASSERT_TRUE(ray_vec_is_null(w2, 140));
    TEST_ASSERT_TRUE(ray_vec_is_null(b, 140));

    ray_release(w2);
    ray_release(b);
    ray_heap_destroy();
    PASS();
}

/* ─── ray_index_info with no index attached ─────────────────────────────── */

static test_result_t test_index_info_no_index(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3 };
    ray_t* v = make_i64_vec(xs, 3);
    /* No index attached — should return RAY_NULL_OBJ. */
    TEST_ASSERT_FALSE(v->attrs & RAY_ATTR_HAS_INDEX);
    ray_t* info = ray_index_info(v);
    TEST_ASSERT_TRUE(info == RAY_NULL_OBJ);

    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* ─── Bloom filter with nulls (covers null-skip in bloom build) ──────────── */

static test_result_t test_index_bloom_with_nulls(void) {
    ray_heap_init();
    int64_t xs[] = { 10, 20, 30, 40, 50 };
    ray_t* v = make_i64_vec(xs, 5);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 3, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ix->u.bloom.n_keys, 3);  /* 5 - 2 nulls = 3 */
    TEST_ASSERT_NOT_NULL(ix->u.bloom.bits);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── GUID attach error (covers prepare_attach unsupported type for GUID) ── */

static test_result_t test_index_guid_unsupported(void) {
    ray_heap_init();
    /* RAY_GUID is not numeric, so attach_zone should fail. */
    ray_t* v = ray_vec_new(RAY_GUID, 4);
    /* GUID element is 16 bytes — append a zero GUID. */
    uint8_t guid[16] = {0};
    v = ray_vec_append(v, guid);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));

    if (RAY_IS_ERR(r)) ray_error_free(r);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Sort index with all-same values (stress the sort path) ─────────────── */

static test_result_t test_index_sort_all_same(void) {
    ray_heap_init();
    int64_t xs[] = { 7, 7, 7, 7, 7 };
    ray_t* v = make_i64_vec(xs, 5);

    ray_t* w = v;
    ray_t* r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)ix->kind, RAY_IDX_SORT);
    TEST_ASSERT_EQ_I(ix->u.sort.perm->len, 5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── ray_idx_*_fn builtins (covers attach_via, fn wrappers) ─────────────── */

static test_result_t test_index_builtin_fns(void) {
    ray_heap_init();
    int64_t xs[] = { 5, 3, 9, 1, 7 };
    ray_t* v = make_i64_vec(xs, 5);
    ray_retain(v);  /* keep a ref while the fn takes ownership */

    /* ray_idx_zone_fn */
    ray_t* r1 = ray_idx_zone_fn(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I((int)ray_index_kind(r1), RAY_IDX_ZONE);
    ray_release(r1);

    /* ray_idx_hash_fn */
    ray_retain(v);
    ray_t* r2 = ray_idx_hash_fn(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I((int)ray_index_kind(r2), RAY_IDX_HASH);

    /* ray_idx_has_fn */
    ray_t* has = ray_idx_has_fn(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(has));
    ray_release(has);

    /* ray_idx_info_fn */
    ray_t* info = ray_idx_info_fn(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(info));
    ray_release(info);

    /* ray_idx_drop_fn */
    ray_retain(r2);
    ray_t* r3 = ray_idx_drop_fn(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r3));
    TEST_ASSERT_FALSE(r3->attrs & RAY_ATTR_HAS_INDEX);
    ray_release(r3);
    ray_release(r2);

    /* ray_idx_sort_fn */
    ray_retain(v);
    ray_t* r4 = ray_idx_sort_fn(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r4));
    TEST_ASSERT_EQ_I((int)ray_index_kind(r4), RAY_IDX_SORT);
    ray_release(r4);

    /* ray_idx_bloom_fn */
    ray_retain(v);
    ray_t* r5 = ray_idx_bloom_fn(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r5));
    TEST_ASSERT_EQ_I((int)ray_index_kind(r5), RAY_IDX_BLOOM);
    ray_release(r5);

    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* ─── F32 hash with NaN (covers numeric_key_word F32 NaN branch) ────────── */

static test_result_t test_index_hash_f32_nan(void) {
    ray_heap_init();
    float xs[] = { 1.0f, (float)NAN, 2.0f, (float)NAN };
    ray_t* v = ray_vec_new(RAY_F32, 4);
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    /* All 4 rows are non-null; NaN rows get per-row bucket via numeric_key_word. */
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 4);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F32 zone with NaN (covers zone_scan_float es=4 NaN skip) ────────── */

static test_result_t test_index_zone_f32_nan(void) {
    ray_heap_init();
    float xs[] = { 1.0f, (float)NAN, 3.0f, (float)NAN };
    ray_t* v = ray_vec_new(RAY_F32, 4);
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &xs[i]);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    /* NaN rows are skipped: min=1.0, max=3.0 */
    TEST_ASSERT_TRUE(iz->u.zone.min_f == 1.0);
    TEST_ASSERT_TRUE(iz->u.zone.max_f == 3.0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 0);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F32 zone with nulls (covers zone_scan_float es=4 null path) ──────── */

static test_result_t test_index_zone_f32_nulls(void) {
    ray_heap_init();
    float xs[] = { 10.0f, 20.0f, 30.0f, 40.0f };
    ray_t* v = ray_vec_new(RAY_F32, 4);
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 3, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_TRUE(iz->u.zone.min_f == 10.0);
    TEST_ASSERT_TRUE(iz->u.zone.max_f == 30.0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 2);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F32 zone all-null (covers zone_scan_float es=4 !any_value) ───────── */

static test_result_t test_index_zone_f32_all_null(void) {
    ray_heap_init();
    float xs[] = { 1.0f, 2.0f };
    ray_t* v = ray_vec_new(RAY_F32, 2);
    for (int i = 0; i < 2; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 0, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_TRUE(iz->u.zone.min_f == 0.0);
    TEST_ASSERT_TRUE(iz->u.zone.max_f == 0.0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 2);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F64 hash with -0.0 (covers clear_neg_zero path in numeric_key_word) ─ */

static test_result_t test_index_hash_f64_neg_zero(void) {
    ray_heap_init();
    double xs[] = { -0.0, 0.0, 1.0, -0.0 };
    ray_t* v = make_f64_vec(xs, 4);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 4);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F32 hash with -0.0 (covers clear_neg_zero on F32 path) ──────────── */

static test_result_t test_index_hash_f32_neg_zero(void) {
    ray_heap_init();
    float xs[] = { -0.0f, 0.0f, 1.0f };
    ray_t* v = ray_vec_new(RAY_F32, 3);
    for (int i = 0; i < 3; i++) v = ray_vec_append(v, &xs[i]);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 3);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── TIME through all four index kinds ───────────────────────────────── */

static test_result_t test_index_time_all_kinds(void) {
    ray_heap_init();
    int64_t ts[] = { 0, 3600, 86399, 1000, 7200 };
    ray_t* v = ray_vec_new(RAY_TIME, 5);
    for (int i = 0; i < 5; i++) {
        int32_t t = (int32_t)ts[i];
        v = ray_vec_append(v, &t);
    }
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* hash */
    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 5);
    ray_index_drop(&w);

    /* sort */
    r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* is = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(is->u.sort.perm->len, 5);
    ray_index_drop(&w);

    /* bloom */
    r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── DATE through hash, sort, bloom ──────────────────────────────────── */

static test_result_t test_index_date_all_kinds(void) {
    ray_heap_init();
    int32_t dates[] = { 0, 18000, -365, 36500 };
    ray_t* v = ray_vec_new(RAY_DATE, 4);
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &dates[i]);

    /* hash */
    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 4);
    ray_index_drop(&w);

    /* sort */
    r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* is = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(is->u.sort.perm->len, 4);
    ray_index_drop(&w);

    /* bloom */
    r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 4);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── TIMESTAMP through hash, sort, bloom ─────────────────────────────── */

static test_result_t test_index_timestamp_all_kinds(void) {
    ray_heap_init();
    int64_t ts[] = { 1700000000000LL, 0LL, 1000000LL, 5LL };
    ray_t* v = ray_vec_new(RAY_TIMESTAMP, 4);
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &ts[i]);

    /* hash */
    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 4);
    ray_index_drop(&w);

    /* sort */
    r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* is = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(is->u.sort.perm->len, 4);
    ray_index_drop(&w);

    /* bloom */
    r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 4);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── I16 through sort and bloom ──────────────────────────────────────── */

static test_result_t test_index_i16_sort_and_bloom(void) {
    ray_heap_init();
    int16_t xs[] = { 300, -100, 0, 200, -32768 };
    ray_t* v = ray_vec_new(RAY_I16, 5);
    for (int i = 0; i < 5; i++) v = ray_vec_append(v, &xs[i]);

    /* sort */
    ray_t* w = v;
    ray_t* r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* is = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(is->u.sort.perm->len, 5);
    ray_index_drop(&w);

    /* bloom */
    r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── U8 through sort and bloom ───────────────────────────────────────── */

static test_result_t test_index_u8_sort_and_bloom(void) {
    ray_heap_init();
    uint8_t xs[] = { 50, 10, 200, 1, 255 };
    ray_t* v = ray_vec_new(RAY_U8, 5);
    for (int i = 0; i < 5; i++) v = ray_vec_append(v, &xs[i]);

    /* sort */
    ray_t* w = v;
    ray_t* r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* is = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(is->u.sort.perm->len, 5);
    ray_index_drop(&w);

    /* bloom */
    r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── BOOL through sort and bloom ─────────────────────────────────────── */

static test_result_t test_index_bool_sort_and_bloom(void) {
    ray_heap_init();
    uint8_t xs[] = { 1, 0, 1, 0, 1 };
    ray_t* v = ray_vec_new(RAY_BOOL, 5);
    for (int i = 0; i < 5; i++) v = ray_vec_append(v, &xs[i]);

    /* sort */
    ray_t* w = v;
    ray_t* r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* is = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(is->u.sort.perm->len, 5);
    ray_index_drop(&w);

    /* bloom */
    r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── I32 through zone, sort, bloom (zone with nulls) ─────────────────── */

static test_result_t test_index_i32_zone_sort_bloom(void) {
    ray_heap_init();
    int32_t xs[] = { 100, -50, 0, 999, -999 };
    ray_t* v = ray_vec_new(RAY_I32, 5);
    for (int i = 0; i < 5; i++) v = ray_vec_append(v, &xs[i]);

    /* zone with nulls */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 2, true), RAY_OK);
    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(iz->u.zone.min_i, -999);
    TEST_ASSERT_EQ_I(iz->u.zone.max_i, 999);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 1);
    ray_index_drop(&w);

    /* sort */
    r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* is = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(is->u.sort.perm->len, 5);
    ray_index_drop(&w);

    /* bloom */
    r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    /* row 2 is null, so n_keys = 4 */
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 4);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F64 sort and bloom ──────────────────────────────────────────────── */

static test_result_t test_index_f64_sort_and_bloom(void) {
    ray_heap_init();
    double xs[] = { 3.14, -2.5, 0.0, 100.0, 1.5 };
    ray_t* v = make_f64_vec(xs, 5);

    /* sort */
    ray_t* w = v;
    ray_t* r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* is = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(is->u.sort.perm->len, 5);
    ray_index_drop(&w);

    /* bloom */
    r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F32 sort and bloom ──────────────────────────────────────────────── */

static test_result_t test_index_f32_sort_and_bloom(void) {
    ray_heap_init();
    float xs[] = { 3.14f, -2.5f, 0.0f, 100.0f, 1.5f };
    ray_t* v = ray_vec_new(RAY_F32, 5);
    for (int i = 0; i < 5; i++) v = ray_vec_append(v, &xs[i]);

    /* sort */
    ray_t* w = v;
    ray_t* r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* is = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(is->u.sort.perm->len, 5);
    ray_index_drop(&w);

    /* bloom */
    r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── prepare_attach with vp == NULL (covers !vp true branch) ──────────── */

static test_result_t test_index_attach_null_vp(void) {
    ray_heap_init();

    /* Pass NULL pointer-to-pointer — triggers the !vp branch in prepare_attach. */
    ray_t* r1 = ray_index_attach_zone(NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    ray_error_free(r1);

    ray_t* r2 = ray_index_attach_hash(NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    ray_error_free(r2);

    ray_t* r3 = ray_index_attach_sort(NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    ray_error_free(r3);

    ray_t* r4 = ray_index_attach_bloom(NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    ray_error_free(r4);

    ray_heap_destroy();
    PASS();
}

/* ─── attach_via error propagation (NULL input to fn wrappers) ──────────── */

static test_result_t test_index_fn_null_input(void) {
    ray_heap_init();

    /* NULL input to all fn wrappers — covers attach_via !v branch. */
    ray_t* r1 = ray_idx_zone_fn(NULL);
    TEST_ASSERT_TRUE(r1 == NULL);

    ray_t* r2 = ray_idx_hash_fn(NULL);
    TEST_ASSERT_TRUE(r2 == NULL);

    ray_t* r3 = ray_idx_sort_fn(NULL);
    TEST_ASSERT_TRUE(r3 == NULL);

    ray_t* r4 = ray_idx_bloom_fn(NULL);
    TEST_ASSERT_TRUE(r4 == NULL);

    /* Error input to attach_via — covers RAY_IS_ERR(v) branch. */
    ray_t* err = ray_error("test", "synthetic");
    ray_t* r5 = ray_idx_zone_fn(err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));

    err = ray_error("test", "synthetic");
    ray_t* r6 = ray_idx_hash_fn(err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r6));

    err = ray_error("test", "synthetic");
    ray_t* r7 = ray_idx_sort_fn(err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r7));

    err = ray_error("test", "synthetic");
    ray_t* r8 = ray_idx_bloom_fn(err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r8));

    ray_heap_destroy();
    PASS();
}

/* ─── ray_idx_drop_fn with NULL and error ──────────────────────────────── */

static test_result_t test_index_drop_fn_null_error(void) {
    ray_heap_init();

    /* NULL input — covers !v branch in ray_idx_drop_fn. */
    ray_t* r1 = ray_idx_drop_fn(NULL);
    TEST_ASSERT_TRUE(r1 == NULL);

    /* Error input — covers RAY_IS_ERR(v) branch. */
    ray_t* err = ray_error("test", "synthetic");
    ray_t* r2 = ray_idx_drop_fn(err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    ray_error_free(r2);

    ray_heap_destroy();
    PASS();
}

/* ─── ray_idx_has_fn with NULL ─────────────────────────────────────────── */

static test_result_t test_index_has_fn_null(void) {
    ray_heap_init();

    /* ray_idx_has_fn on a vec without index returns false (0b). */
    int64_t xs[] = { 1, 2, 3 };
    ray_t* v = make_i64_vec(xs, 3);
    ray_t* r = ray_idx_has_fn(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    ray_release(v);

    ray_heap_destroy();
    PASS();
}

/* ─── release_payload with NULL pointers in hash/sort/bloom ────────────── *
 *
 * Exercises the false branches of the NULL checks in ray_index_release_payload
 * (lines 142-156): hash.table==NULL, hash.chain==NULL, sort.perm==NULL,
 * bloom.bits==NULL. */

static test_result_t test_index_release_payload_null_ptrs(void) {
    ray_heap_init();

    /* Hash with all-NULL CSR children. */
    ray_index_t ix_hash;
    memset(&ix_hash, 0, sizeof(ix_hash));
    ix_hash.kind = RAY_IDX_HASH;
    ray_index_release_payload(&ix_hash);  /* Should be safe no-op. */

    /* Sort with NULL perm. */
    ray_index_t ix_sort;
    memset(&ix_sort, 0, sizeof(ix_sort));
    ix_sort.kind = RAY_IDX_SORT;
    ix_sort.u.sort.perm = NULL;
    ray_index_release_payload(&ix_sort);

    /* Bloom with NULL bits. */
    ray_index_t ix_bloom;
    memset(&ix_bloom, 0, sizeof(ix_bloom));
    ix_bloom.kind = RAY_IDX_BLOOM;
    ix_bloom.u.bloom.bits = NULL;
    ray_index_release_payload(&ix_bloom);

    /* NONE kind. */
    ray_index_t ix_none;
    memset(&ix_none, 0, sizeof(ix_none));
    ix_none.kind = RAY_IDX_NONE;
    ray_index_release_payload(&ix_none);

    ray_heap_destroy();
    PASS();
}

/* ─── retain_payload with NULL pointers in hash/sort/bloom ─────────────── */

static test_result_t test_index_retain_payload_null_ptrs(void) {
    ray_heap_init();

    /* Hash with all-NULL CSR children — the if checks must skip retain. */
    ray_index_t ix_hash;
    memset(&ix_hash, 0, sizeof(ix_hash));
    ix_hash.kind = RAY_IDX_HASH;
    ray_index_retain_payload(&ix_hash);

    /* Sort with NULL perm. */
    ray_index_t ix_sort;
    memset(&ix_sort, 0, sizeof(ix_sort));
    ix_sort.kind = RAY_IDX_SORT;
    ix_sort.u.sort.perm = NULL;
    ray_index_retain_payload(&ix_sort);

    /* Bloom with NULL bits. */
    ray_index_t ix_bloom;
    memset(&ix_bloom, 0, sizeof(ix_bloom));
    ix_bloom.kind = RAY_IDX_BLOOM;
    ix_bloom.u.bloom.bits = NULL;
    ray_index_retain_payload(&ix_bloom);

    /* NONE kind. */
    ray_index_t ix_none;
    memset(&ix_none, 0, sizeof(ix_none));
    ix_none.kind = RAY_IDX_NONE;
    ray_index_retain_payload(&ix_none);

    ray_heap_destroy();
    PASS();
}

/* ─── Empty vec through all four kinds ─────────────────────────────────── */

static test_result_t test_index_empty_vec_all_kinds(void) {
    ray_heap_init();
    ray_t* v = ray_vec_new(RAY_I64, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* zone on empty */
    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(iz->u.zone.min_i, 0);
    TEST_ASSERT_EQ_I(iz->u.zone.max_i, 0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 0);
    ray_index_drop(&w);

    /* hash on empty (chain->len = 0) */
    r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 0);
    ray_index_drop(&w);

    /* sort on empty */
    r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* is = ray_index_payload(w->index);
    TEST_ASSERT_NOT_NULL(is->u.sort.perm);
    ray_index_drop(&w);

    /* bloom on empty (n_set=0, target_bits < 8 branch, floor 64) */
    r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 0);
    TEST_ASSERT_TRUE((ib->u.bloom.m_mask + 1) == 64);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Hash with nulls across multiple types ────────────────────────────── *
 *
 * Ensures the null-skip branch in hash build (line 380) fires for I16,
 * I32, F32, DATE types.  BOOL/U8 are non-nullable so excluded. */

static test_result_t test_index_hash_nulls_multi_type(void) {
    ray_heap_init();

    /* I16 with null */
    int16_t i16_xs[] = { 10, 20, 30 };
    ray_t* v16 = ray_vec_new(RAY_I16, 3);
    for (int i = 0; i < 3; i++) v16 = ray_vec_append(v16, &i16_xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v16, 1, true), RAY_OK);
    ray_t* w16 = v16;
    ray_t* r = ray_index_attach_hash(&w16);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_index_payload(w16->index)->u.hash.n_keys, 2);
    ray_release(w16);

    /* I32 with null */
    int32_t i32_xs[] = { 100, 200, 300 };
    ray_t* v32 = ray_vec_new(RAY_I32, 3);
    for (int i = 0; i < 3; i++) v32 = ray_vec_append(v32, &i32_xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v32, 0, true), RAY_OK);
    ray_t* w32 = v32;
    r = ray_index_attach_hash(&w32);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_index_payload(w32->index)->u.hash.n_keys, 2);
    ray_release(w32);

    /* F32 with null */
    float f32_xs[] = { 1.5f, 2.5f, 3.5f };
    ray_t* vf32 = ray_vec_new(RAY_F32, 3);
    for (int i = 0; i < 3; i++) vf32 = ray_vec_append(vf32, &f32_xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vf32, 1, true), RAY_OK);
    ray_t* wf32 = vf32;
    r = ray_index_attach_hash(&wf32);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_index_payload(wf32->index)->u.hash.n_keys, 2);
    ray_release(wf32);

    /* DATE with null */
    int32_t date_xs[] = { 18000, 19000, 20000 };
    ray_t* vd = ray_vec_new(RAY_DATE, 3);
    for (int i = 0; i < 3; i++) vd = ray_vec_append(vd, &date_xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vd, 0, true), RAY_OK);
    ray_t* wd = vd;
    r = ray_index_attach_hash(&wd);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_index_payload(wd->index)->u.hash.n_keys, 2);
    ray_release(wd);

    ray_heap_destroy();
    PASS();
}

/* ─── Bloom with nulls across multiple types ───────────────────────────── */

static test_result_t test_index_bloom_nulls_multi_type(void) {
    ray_heap_init();

    /* I16 with null */
    int16_t i16_xs[] = { 10, 20, 30, 40 };
    ray_t* v16 = ray_vec_new(RAY_I16, 4);
    for (int i = 0; i < 4; i++) v16 = ray_vec_append(v16, &i16_xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v16, 1, true), RAY_OK);
    ray_t* w16 = v16;
    ray_t* r = ray_index_attach_bloom(&w16);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_index_payload(w16->index)->u.bloom.n_keys, 3);
    ray_release(w16);

    /* F32 with null */
    float f32_xs[] = { 1.5f, 2.5f, 3.5f, 4.5f };
    ray_t* vf32 = ray_vec_new(RAY_F32, 4);
    for (int i = 0; i < 4; i++) vf32 = ray_vec_append(vf32, &f32_xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vf32, 0, true), RAY_OK);
    ray_t* wf32 = vf32;
    r = ray_index_attach_bloom(&wf32);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_index_payload(wf32->index)->u.bloom.n_keys, 3);
    ray_release(wf32);

    ray_heap_destroy();
    PASS();
}

/* ─── Sort with nulls ──────────────────────────────────────────────────── */

static test_result_t test_index_sort_with_nulls(void) {
    ray_heap_init();
    int64_t xs[] = { 50, 10, 30, 20, 40 };
    ray_t* v = make_i64_vec(xs, 5);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 2, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* is = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(is->u.sort.perm->len, 5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Zone scan I64 with single value (min == max) ─────────────────────── */

static test_result_t test_index_zone_single_value(void) {
    ray_heap_init();
    int64_t xs[] = { 42 };
    ray_t* v = make_i64_vec(xs, 1);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(iz->u.zone.min_i, 42);
    TEST_ASSERT_EQ_I(iz->u.zone.max_i, 42);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 0);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F64 bloom with NaN (covers numeric_key_word F64 NaN in bloom) ────── */

static test_result_t test_index_bloom_f64_nan(void) {
    ray_heap_init();
    double xs[] = { 1.0, (double)NAN, 3.0, (double)NAN, 5.0 };
    ray_t* v = make_f64_vec(xs, 5);

    ray_t* w = v;
    ray_t* r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    /* All 5 rows are non-null (NaN is not null), all 5 get hashed. */
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F32 bloom with NaN (covers numeric_key_word F32 NaN in bloom) ────── */

static test_result_t test_index_bloom_f32_nan(void) {
    ray_heap_init();
    float xs[] = { 1.0f, (float)NAN, 3.0f, (float)NAN };
    ray_t* v = ray_vec_new(RAY_F32, 4);
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &xs[i]);

    ray_t* w = v;
    ray_t* r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 4);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F64 bloom with -0.0 ─────────────────────────────────────────────── */

static test_result_t test_index_bloom_f64_neg_zero(void) {
    ray_heap_init();
    double xs[] = { -0.0, 0.0, 1.0 };
    ray_t* v = make_f64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 3);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Zone scan F64 with -0.0 (clear_neg_zero in zone_scan_float) ──────── */

static test_result_t test_index_zone_f64_neg_zero(void) {
    ray_heap_init();
    double xs[] = { -0.0, 1.0, -1.0 };
    ray_t* v = make_f64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_TRUE(iz->u.zone.min_f == -1.0);
    TEST_ASSERT_TRUE(iz->u.zone.max_f == 1.0);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── info on sort with NULL perm (covers sort.perm ? perm->len : 0) ───── */

static test_result_t test_index_info_sort(void) {
    ray_heap_init();
    int64_t xs[] = { 3, 1, 2 };
    ray_t* v = make_i64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r = ray_index_attach_sort(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    /* Get info dict — covers RAY_IDX_SORT branch in ray_index_info. */
    ray_t* info = ray_index_info(w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(info));
    ray_release(info);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── info on bloom (covers RAY_IDX_BLOOM branch in ray_index_info) ────── */

static test_result_t test_index_info_bloom(void) {
    ray_heap_init();
    int64_t xs[] = { 10, 20, 30 };
    ray_t* v = make_i64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_t* info = ray_index_info(w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(info));
    ray_release(info);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── info on hash (covers RAY_IDX_HASH branch in ray_index_info) ──────── */

static test_result_t test_index_info_hash(void) {
    ray_heap_init();
    int64_t xs[] = { 10, 20, 30 };
    ray_t* v = make_i64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_t* info = ray_index_info(w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(info));
    ray_release(info);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── info on zone with int parent (covers else branch in zone info) ───── */

static test_result_t test_index_info_zone_int(void) {
    ray_heap_init();
    int64_t xs[] = { 5, 1, 9 };
    ray_t* v = make_i64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_t* info = ray_index_info(w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(info));
    ray_release(info);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── info on zone with F64 parent (covers F32/F64 branch in zone info) ── */

static test_result_t test_index_info_zone_f64(void) {
    ray_heap_init();
    double xs[] = { 1.5, -2.5, 3.14 };
    ray_t* v = make_f64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_t* info = ray_index_info(w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(info));
    ray_release(info);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Hash on n=1 (n < 4 branch, capacity=8) ──────────────────────────── */

static test_result_t test_index_hash_single_elem(void) {
    ray_heap_init();
    int64_t xs[] = { 42 };
    ray_t* v = make_i64_vec(xs, 1);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 1);
    /* n=1 < 4, so cap = next_pow2(8) = 8 */
    TEST_ASSERT_TRUE((ih->u.hash.mask + 1) == 8);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── I64 zone with nulls where min/max update on non-null ─────────────── */

static test_result_t test_index_zone_i64_mixed_nulls(void) {
    ray_heap_init();
    int64_t xs[] = { 100, 200, 50, 300, 75 };
    ray_t* v = make_i64_vec(xs, 5);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 0, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 3, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    /* Non-null: 200, 50, 75 -> min=50, max=200 */
    TEST_ASSERT_EQ_I(iz->u.zone.min_i, 50);
    TEST_ASSERT_EQ_I(iz->u.zone.max_i, 200);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 2);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F64 zone with explicit null via set_null_checked ──────────────────── *
 *
 * Note: NaN IS the F64 null sentinel.  Once HAS_NULLS is set via
 * set_null_checked, any NaN row will also be detected as null.
 * This test uses only non-NaN data and marks one row null. */

static test_result_t test_index_zone_f64_nan_and_null(void) {
    ray_heap_init();
    double xs[] = { 1.0, 5.0, 3.0, 7.0 };
    ray_t* v = make_f64_vec(xs, 4);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 2, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    /* Row 2 is null (sentinel NaN), non-null rows: 1.0, 5.0, 7.0 */
    TEST_ASSERT_TRUE(iz->u.zone.min_f == 1.0);
    TEST_ASSERT_TRUE(iz->u.zone.max_f == 7.0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 1);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── attach_via error path via unsupported type ───────────────────────── *
 *
 * Uses a SYM vec through the fn wrappers to hit the error branch in
 * attach_via (line 654: RAY_IS_ERR(r) -> release w, return r). */

static test_result_t test_index_fn_error_propagation(void) {
    ray_heap_init();
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 4);
    int64_t s = ray_sym_intern("test", 4);
    v = ray_vec_append(v, &s);

    /* zone/sort/bloom reject SYM; hash now accepts SYM (domain-id hash index). */
    ray_retain(v);
    ray_t* r1 = ray_idx_zone_fn(v);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    ray_error_free(r1);

    /* hash on SYM now succeeds — verify the result carries an index. */
    ray_retain(v);
    ray_t* r2 = ray_idx_hash_fn(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_TRUE(ray_index_has(r2));
    ray_release(r2);

    ray_retain(v);
    ray_t* r3 = ray_idx_sort_fn(v);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    ray_error_free(r3);

    ray_retain(v);
    ray_t* r4 = ray_idx_bloom_fn(v);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    ray_error_free(r4);

    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* ─── Hash on large n (n >= 4, 2*n path in capacity calc) ─────────────── */

static test_result_t test_index_hash_large_n(void) {
    ray_heap_init();
    int64_t n = 100;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &i);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 100);
    /* n=100 >= 4, cap = next_pow2(200) = 256 */
    TEST_ASSERT_TRUE((ih->u.hash.mask + 1) == 256);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Bloom on large vec (n_set >= 8, 8*n_set path in sizing) ──────────── */

static test_result_t test_index_bloom_large_n(void) {
    ray_heap_init();
    int64_t n = 50;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &i);

    ray_t* w = v;
    ray_t* r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 50);
    /* n_set=50, 8*50=400, next_pow2(400)=512 */
    TEST_ASSERT_TRUE((ib->u.bloom.m_mask + 1) == 512);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── LIST vec unsupported through all four kinds ──────────────────────── *
 *
 * RAY_LIST is type 0 which fails ray_is_vec, so prepare_attach returns
 * "type" error.  This covers the !ray_is_vec branch for all four kinds. */

static test_result_t test_index_list_unsupported(void) {
    ray_heap_init();
    ray_t* v = ray_list_new(4);
    ray_t* elem = ray_i64(1);
    v = ray_list_append(v, elem);
    ray_release(elem);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* w = v;
    ray_t* r1 = ray_index_attach_zone(&w);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    ray_error_free(r1);

    w = v;
    ray_t* r2 = ray_index_attach_hash(&w);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    ray_error_free(r2);

    w = v;
    ray_t* r3 = ray_index_attach_sort(&w);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    ray_error_free(r3);

    w = v;
    ray_t* r4 = ray_index_attach_bloom(&w);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    ray_error_free(r4);

    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* ─── Non-vec (atom) through all kinds (covers !ray_is_vec branch) ──────── */

static test_result_t test_index_attach_atom_error(void) {
    ray_heap_init();
    ray_t* a = ray_i64(42);

    /* Zone */
    ray_t* wa = a;
    ray_t* r1 = ray_index_attach_zone(&wa);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    ray_error_free(r1);

    /* Hash */
    wa = a;
    ray_t* r2 = ray_index_attach_hash(&wa);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    ray_error_free(r2);

    /* Sort */
    wa = a;
    ray_t* r3 = ray_index_attach_sort(&wa);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    ray_error_free(r3);

    /* Bloom */
    wa = a;
    ray_t* r4 = ray_index_attach_bloom(&wa);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    ray_error_free(r4);

    ray_release(a);
    ray_heap_destroy();
    PASS();
}

/* ─── I64 hash with all-same values (one group holding every row) ─────── */

static test_result_t test_index_hash_collisions(void) {
    ray_heap_init();
    int64_t xs[] = { 5, 5, 5, 5, 5 };
    ray_t* v = make_i64_vec(xs, 5);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ih->u.hash.n_keys, 5);
    TEST_ASSERT_EQ_I(ih->u.hash.n_groups, 1);

    /* One CSR group holding rows 0..4 in ascending order. */
    const int64_t* grows = NULL;
    int64_t gn = 0;
    TEST_ASSERT_EQ_I(ray_index_hash_group(w, 5, &grows, &gn), 1);
    TEST_ASSERT_EQ_I(gn, 5);
    for (int64_t i = 0; i < 5; i++)
        TEST_ASSERT_EQ_I(grows[i], i);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Bloom n_set in [1,7] range (n_set < 8, target_bits=64 floor) ────── */

static test_result_t test_index_bloom_small_n_set(void) {
    ray_heap_init();
    int64_t xs[] = { 42 };
    ray_t* v = make_i64_vec(xs, 1);

    ray_t* w = v;
    ray_t* r = ray_index_attach_bloom(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ib = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ib->u.bloom.n_keys, 1);
    /* n_set=1 < 8 -> target_bits=64 -> m=64 */
    TEST_ASSERT_TRUE((ib->u.bloom.m_mask + 1) == 64);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Drop on vec whose index was already dropped (double-drop) ────────── */

static test_result_t test_index_double_drop(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3 };
    ray_t* v = make_i64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_index_drop(&w);
    TEST_ASSERT_FALSE(w->attrs & RAY_ATTR_HAS_INDEX);

    /* Second drop is a no-op (covers !(v->attrs & RAY_ATTR_HAS_INDEX) branch). */
    ray_t* r2 = ray_index_drop(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_FALSE(w->attrs & RAY_ATTR_HAS_INDEX);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F64 zone only-NaN rows (all NaN, no null: !any_value path) ───────── */

static test_result_t test_index_zone_f64_only_nan(void) {
    ray_heap_init();
    double xs[] = { (double)NAN, (double)NAN, (double)NAN };
    ray_t* v = make_f64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    /* All NaN, none null: NaN skipped in min/max, !any_value -> 0.0/0.0 */
    TEST_ASSERT_TRUE(iz->u.zone.min_f == 0.0);
    TEST_ASSERT_TRUE(iz->u.zone.max_f == 0.0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 0);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F32 zone only-NaN rows ──────────────────────────────────────────── */

static test_result_t test_index_zone_f32_only_nan(void) {
    ray_heap_init();
    float xs[] = { (float)NAN, (float)NAN };
    ray_t* v = ray_vec_new(RAY_F32, 2);
    for (int i = 0; i < 2; i++) v = ray_vec_append(v, &xs[i]);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_TRUE(iz->u.zone.min_f == 0.0);
    TEST_ASSERT_TRUE(iz->u.zone.max_f == 0.0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 0);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── I16 zone with nulls (covers zone_scan_int es=2 null path) ────────── */

static test_result_t test_index_zone_i16_nulls(void) {
    ray_heap_init();
    int16_t xs[] = { 100, -200, 300, 0, 50 };
    ray_t* v = ray_vec_new(RAY_I16, 5);
    for (int i = 0; i < 5; i++) v = ray_vec_append(v, &xs[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 3, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(iz->u.zone.min_i, 50);
    TEST_ASSERT_EQ_I(iz->u.zone.max_i, 300);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 2);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── U8 / BOOL are non-nullable: set_null_checked rejects them ──────────
 *
 * BOOL/U8 have no null sentinel, so ray_vec_set_null_checked returns
 * RAY_ERR_TYPE.  This is NOT an idxop branch — but it documents the
 * constraint.  Zone-scan on BOOL/U8 never enters the null-skip branch.
 * The zone_scan_int es=1 path is already covered by the non-null BOOL
 * and U8 zone tests above. */

static test_result_t test_index_u8_bool_non_nullable(void) {
    ray_heap_init();

    /* U8: set_null_checked must reject. */
    uint8_t ux[] = { 10, 20 };
    ray_t* vu = ray_vec_new(RAY_U8, 2);
    for (int i = 0; i < 2; i++) vu = ray_vec_append(vu, &ux[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vu, 0, true), RAY_ERR_TYPE);
    ray_release(vu);

    /* BOOL: set_null_checked must reject. */
    uint8_t bx[] = { 1, 0 };
    ray_t* vb = ray_vec_new(RAY_BOOL, 2);
    for (int i = 0; i < 2; i++) vb = ray_vec_append(vb, &bx[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vb, 0, true), RAY_ERR_TYPE);
    ray_release(vb);

    ray_heap_destroy();
    PASS();
}

/* ─── DATE zone with nulls (covers zone_scan_int es=4 null) ────────────── */

static test_result_t test_index_zone_date_nulls(void) {
    ray_heap_init();
    int32_t dates[] = { 18000, 19000, 20000, 21000 };
    ray_t* v = ray_vec_new(RAY_DATE, 4);
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &dates[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 0, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 1);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── TIME zone with nulls (covers zone_scan_int es=8 null for TIME) ──── */

static test_result_t test_index_zone_time_nulls(void) {
    ray_heap_init();
    int32_t times[] = { 0, 3600, 86399, 1000 };
    ray_t* v = ray_vec_new(RAY_TIME, 4);
    for (int i = 0; i < 4; i++) v = ray_vec_append(v, &times[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 1);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── TIMESTAMP zone with nulls ───────────────────────────────────────── */

static test_result_t test_index_zone_timestamp_nulls(void) {
    ray_heap_init();
    int64_t ts[] = { 1700000000000LL, 0LL, 1000000LL };
    ray_t* v = ray_vec_new(RAY_TIMESTAMP, 3);
    for (int i = 0; i < 3; i++) v = ray_vec_append(v, &ts[i]);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 2, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 1);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── F64 zone all-null via set_null_checked (all rows become NaN) ──────── *
 *
 * NaN IS the F64 null sentinel.  Once HAS_NULLS is set, ALL NaN values
 * are detected as null by ray_vec_is_null.  This test marks all rows
 * null to trigger the !any_value branch in zone_scan_float. */

static test_result_t test_index_zone_f64_null_and_nan(void) {
    ray_heap_init();
    double xs[] = { 1.0, 2.0, 3.0 };
    ray_t* v = make_f64_vec(xs, 3);
    /* Mark all rows null. */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 0, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 2, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* iz = ray_index_payload(w->index);
    /* All null: !any_value -> min=0.0, max=0.0 */
    TEST_ASSERT_TRUE(iz->u.zone.min_f == 0.0);
    TEST_ASSERT_TRUE(iz->u.zone.max_f == 0.0);
    TEST_ASSERT_EQ_I(iz->u.zone.n_nulls, 3);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Hash n=3 (n < 4 boundary, cap=8) ─────────────────────────────────── */

static test_result_t test_index_hash_n3(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3 };
    ray_t* v = make_i64_vec(xs, 3);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    /* n=3 < 4 -> cap = next_pow2(8) = 8 */
    TEST_ASSERT_TRUE((ih->u.hash.mask + 1) == 8);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Hash n=4 (exactly at boundary, cap=next_pow2(8)=8) ──────────────── */

static test_result_t test_index_hash_n4(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3, 4 };
    ray_t* v = make_i64_vec(xs, 4);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    /* n=4 >= 4 -> 2*4=8, next_pow2(8)=8 */
    TEST_ASSERT_TRUE((ih->u.hash.mask + 1) == 8);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── Hash n=5 (2*5=10, next_pow2(10)=16) ─────────────────────────────── */

static test_result_t test_index_hash_n5(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3, 4, 5 };
    ray_t* v = make_i64_vec(xs, 5);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ih = ray_index_payload(w->index);
    /* n=5 >= 4 -> 2*5=10, next_pow2(10)=16 */
    TEST_ASSERT_TRUE((ih->u.hash.mask + 1) == 16);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── chunk_zone: I64 basic attach + payload checks ───────────────────────
 *
 * Smallest legal chunk_log2 is 8 → chunk_size=256.  Build a 1000-row I64
 * column so we get ceil(1000/256)=4 chunks with the last chunk short. */

static test_result_t test_index_chunk_zone_i64_basic(void) {
    ray_heap_init();
    int64_t n = 1000;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t x = i;            /* monotone */
        v = ray_vec_append(v, &x);
    }
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* w = v;
    ray_t* r = ray_index_attach_chunk_zone(&w, 8);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)ix->kind, RAY_IDX_CHUNK_ZONE);
    TEST_ASSERT_EQ_I((int64_t)ix->u.chunk_zone.chunk_log2, 8);
    TEST_ASSERT_EQ_I((int64_t)ix->u.chunk_zone.n_chunks, 4);
    TEST_ASSERT_EQ_I((int64_t)ix->u.chunk_zone.is_f64,   0);
    TEST_ASSERT_NOT_NULL(ix->u.chunk_zone.mins);
    TEST_ASSERT_NOT_NULL(ix->u.chunk_zone.maxs);
    TEST_ASSERT_NOT_NULL(ix->u.chunk_zone.null_bits);

    int64_t* mins = (int64_t*)ray_data(ix->u.chunk_zone.mins);
    int64_t* maxs = (int64_t*)ray_data(ix->u.chunk_zone.maxs);
    /* Chunks: [0,256), [256,512), [512,768), [768,1000). */
    TEST_ASSERT_EQ_I(mins[0], 0);    TEST_ASSERT_EQ_I(maxs[0], 255);
    TEST_ASSERT_EQ_I(mins[1], 256);  TEST_ASSERT_EQ_I(maxs[1], 511);
    TEST_ASSERT_EQ_I(mins[2], 512);  TEST_ASSERT_EQ_I(maxs[2], 767);
    TEST_ASSERT_EQ_I(mins[3], 768);  TEST_ASSERT_EQ_I(maxs[3], 999);
    /* No nulls → null_bits all zero. */
    uint8_t* nb = (uint8_t*)ray_data(ix->u.chunk_zone.null_bits);
    TEST_ASSERT_EQ_I((int64_t)nb[0], 0);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── chunk_zone: I64 with nulls in some chunks ────────────────────────────
 * One chunk gets a null → its bit gets set; whole-column null detection. */

static test_result_t test_index_chunk_zone_i64_with_nulls(void) {
    ray_heap_init();
    int64_t n = 600;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t x = i * 2;
        v = ray_vec_append(v, &x);
    }
    /* Place nulls in chunk 0 (row 5) and chunk 2 (row 520). */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 5,   true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 520, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_chunk_zone(&w, 8);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ix = ray_index_payload(w->index);
    /* ceil(600/256) = 3 chunks */
    TEST_ASSERT_EQ_I((int64_t)ix->u.chunk_zone.n_chunks, 3);

    uint8_t* nb = (uint8_t*)ray_data(ix->u.chunk_zone.null_bits);
    TEST_ASSERT_TRUE((nb[0] & 0x01) != 0);   /* chunk 0 has null */
    TEST_ASSERT_FALSE((nb[0] & 0x02) != 0);  /* chunk 1 no null  */
    TEST_ASSERT_TRUE((nb[0] & 0x04) != 0);   /* chunk 2 has null */

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── chunk_zone: type matrix int paths (BOOL/U8/I16/I32/DATE/TIME/TS) ──── */

static test_result_t test_index_chunk_zone_u8_bool(void) {
    ray_heap_init();
    int64_t n = 500;
    /* U8 (es=1) */
    ray_t* vu = ray_vec_new(RAY_U8, n);
    for (int64_t i = 0; i < n; i++) {
        uint8_t x = (uint8_t)(i & 0xff);
        vu = ray_vec_append(vu, &x);
    }
    ray_t* wu = vu;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&wu, 8)));
    ray_index_t* ix = ray_index_payload(wu->index);
    TEST_ASSERT_EQ_I((int64_t)ix->u.chunk_zone.n_chunks, 2);
    ray_release(wu);

    /* BOOL (es=1) */
    ray_t* vb = ray_vec_new(RAY_BOOL, n);
    for (int64_t i = 0; i < n; i++) {
        uint8_t x = (uint8_t)(i & 1);
        vb = ray_vec_append(vb, &x);
    }
    ray_t* wb = vb;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&wb, 8)));
    int64_t* mins = (int64_t*)ray_data(ray_index_payload(wb->index)->u.chunk_zone.mins);
    int64_t* maxs = (int64_t*)ray_data(ray_index_payload(wb->index)->u.chunk_zone.maxs);
    TEST_ASSERT_EQ_I(mins[0], 0);  TEST_ASSERT_EQ_I(maxs[0], 1);
    ray_release(wb);

    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_chunk_zone_i16(void) {
    ray_heap_init();
    int64_t n = 300;
    ray_t* v = ray_vec_new(RAY_I16, n);
    for (int64_t i = 0; i < n; i++) {
        int16_t x = (int16_t)(i - 100);
        v = ray_vec_append(v, &x);
    }
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&w, 8)));
    ray_index_t* ix = ray_index_payload(w->index);
    int64_t* mins = (int64_t*)ray_data(ix->u.chunk_zone.mins);
    int64_t* maxs = (int64_t*)ray_data(ix->u.chunk_zone.maxs);
    /* Chunk 0 covers rows 0..255, values -100..155 */
    TEST_ASSERT_EQ_I(mins[0], -100);
    TEST_ASSERT_EQ_I(maxs[0], 155);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_chunk_zone_i32_date(void) {
    ray_heap_init();
    int64_t n = 400;
    /* I32 (es=4) */
    ray_t* v = ray_vec_new(RAY_I32, n);
    for (int64_t i = 0; i < n; i++) {
        int32_t x = (int32_t)(i * 7);
        v = ray_vec_append(v, &x);
    }
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&w, 8)));
    ray_release(w);

    /* DATE (es=4) */
    ray_t* vd = ray_vec_new(RAY_DATE, n);
    for (int64_t i = 0; i < n; i++) {
        int32_t d = (int32_t)(18000 + i);
        vd = ray_vec_append(vd, &d);
    }
    ray_t* wd = vd;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&wd, 8)));
    int64_t* mins = (int64_t*)ray_data(ray_index_payload(wd->index)->u.chunk_zone.mins);
    TEST_ASSERT_EQ_I(mins[0], 18000);
    ray_release(wd);

    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_chunk_zone_time_timestamp(void) {
    ray_heap_init();
    int64_t n = 300;
    /* TIME (es=8, stored in int slot) */
    ray_t* vt = ray_vec_new(RAY_TIME, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t x = i * 1000LL;
        vt = ray_vec_append(vt, &x);
    }
    ray_t* wt = vt;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&wt, 8)));
    ray_release(wt);

    /* TIMESTAMP (es=8) */
    ray_t* vs = ray_vec_new(RAY_TIMESTAMP, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t x = 1700000000000LL + i;
        vs = ray_vec_append(vs, &x);
    }
    ray_t* ws = vs;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&ws, 8)));
    ray_release(ws);

    ray_heap_destroy();
    PASS();
}

/* ─── chunk_zone: F64 / F32 paths (chunk_zone_scan_float) ──────────────── */

static test_result_t test_index_chunk_zone_f64_basic(void) {
    ray_heap_init();
    int64_t n = 600;
    ray_t* v = ray_vec_new(RAY_F64, n);
    for (int64_t i = 0; i < n; i++) {
        double x = (double)i + 0.5;
        v = ray_vec_append(v, &x);
    }
    ray_t* w = v;
    ray_t* r = ray_index_attach_chunk_zone(&w, 8);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int64_t)ix->u.chunk_zone.is_f64, 1);
    TEST_ASSERT_EQ_I((int64_t)ix->u.chunk_zone.n_chunks, 3);

    double* mins = (double*)ray_data(ix->u.chunk_zone.mins);
    double* maxs = (double*)ray_data(ix->u.chunk_zone.maxs);
    /* Chunk 0 rows 0..255: values 0.5..255.5 */
    TEST_ASSERT_TRUE(mins[0] == 0.5);
    TEST_ASSERT_TRUE(maxs[0] == 255.5);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_chunk_zone_f32_basic(void) {
    ray_heap_init();
    int64_t n = 300;
    ray_t* v = ray_vec_new(RAY_F32, n);
    for (int64_t i = 0; i < n; i++) {
        float x = (float)i * 0.25f;
        v = ray_vec_append(v, &x);
    }
    ray_t* w = v;
    ray_t* r = ray_index_attach_chunk_zone(&w, 8);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int64_t)ix->u.chunk_zone.is_f64, 1);
    double* mins = (double*)ray_data(ix->u.chunk_zone.mins);
    TEST_ASSERT_TRUE(mins[0] == 0.0);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* F64 chunk_zone with bare NaN values (no HAS_NULLS attr) — covers the
 * isnan(val) → any_null branch in chunk_zone_scan_float. */

static test_result_t test_index_chunk_zone_f64_bare_nan(void) {
    ray_heap_init();
    int64_t n = 300;
    ray_t* v = ray_vec_new(RAY_F64, n);
    for (int64_t i = 0; i < n; i++) {
        double x = (i % 50 == 0) ? (double)NAN : ((double)i);
        v = ray_vec_append(v, &x);
    }
    /* Do NOT call set_null_checked → HAS_NULLS stays off; ray_vec_is_null
     * returns false even for NaN, so the isnan(val) branch fires. */
    ray_t* w = v;
    ray_t* r = ray_index_attach_chunk_zone(&w, 8);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ix = ray_index_payload(w->index);
    uint8_t* nb = (uint8_t*)ray_data(ix->u.chunk_zone.null_bits);
    /* Chunk 0 (rows 0..255) contains NaN at row 0, 50, 100, 150, 200, 250 →
     * any_null is set, even though HAS_NULLS isn't. */
    TEST_ASSERT_TRUE((nb[0] & 0x01) != 0);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* F64 chunk_zone with proper nulls (HAS_NULLS set) — covers
 * ray_vec_is_null branch in chunk_zone_scan_float. */

static test_result_t test_index_chunk_zone_f64_with_nulls(void) {
    ray_heap_init();
    int64_t n = 400;
    ray_t* v = ray_vec_new(RAY_F64, n);
    for (int64_t i = 0; i < n; i++) {
        double x = (double)i;
        v = ray_vec_append(v, &x);
    }
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 10,  true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 300, true), RAY_OK);

    ray_t* w = v;
    ray_t* r = ray_index_attach_chunk_zone(&w, 8);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_index_t* ix = ray_index_payload(w->index);
    uint8_t* nb = (uint8_t*)ray_data(ix->u.chunk_zone.null_bits);
    TEST_ASSERT_TRUE((nb[0] & 0x01) != 0);   /* chunk 0 (rows 0..255) */
    TEST_ASSERT_TRUE((nb[0] & 0x02) != 0);   /* chunk 1 (rows 256..400) */

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── chunk_zone: error / edge cases ─────────────────────────────────────── */

/* chunk_log2 == 0 → defaults to 16 (64K rows/chunk) → too small ≤ 1000
 * rows triggers `v->len < csz` domain error. */
static test_result_t test_index_chunk_zone_log2_zero_default(void) {
    ray_heap_init();
    int64_t n = 500;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &i);
    ray_t* w = v;
    ray_t* r = ray_index_attach_chunk_zone(&w, 0);
    /* 0 → default 16 → csz=65536 > 500 → domain error */
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);
    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* chunk_log2 < 8 (e.g. 7) → domain error. */
static test_result_t test_index_chunk_zone_log2_too_small(void) {
    ray_heap_init();
    int64_t n = 500;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &i);
    ray_t* w = v;
    ray_t* r = ray_index_attach_chunk_zone(&w, 7);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);
    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* chunk_log2 > 22 → domain error. */
static test_result_t test_index_chunk_zone_log2_too_large(void) {
    ray_heap_init();
    int64_t n = 500;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &i);
    ray_t* w = v;
    ray_t* r = ray_index_attach_chunk_zone(&w, 23);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);
    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* column < one chunk → domain error. */
static test_result_t test_index_chunk_zone_column_too_small(void) {
    ray_heap_init();
    int64_t n = 100;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &i);
    ray_t* w = v;
    /* chunk_log2=8 → csz=256, but n=100 < 256 */
    ray_t* r = ray_index_attach_chunk_zone(&w, 8);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);
    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* SYM column → prepare_attach rejects via numeric_elem_size==0. */
static test_result_t test_index_chunk_zone_unsupported_type(void) {
    ray_heap_init();
    int64_t n = 300;
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t kid = ray_sym_intern("k", 1);
        v = ray_vec_append(v, &kid);
    }
    ray_t* w = v;
    ray_t* r = ray_index_attach_chunk_zone(&w, 8);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);
    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* Null/error vp guards on chunk_zone. */
static test_result_t test_index_chunk_zone_null_vp(void) {
    ray_heap_init();
    ray_t* r = ray_index_attach_chunk_zone(NULL, 8);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);
    ray_t* nv = NULL;
    r = ray_index_attach_chunk_zone(&nv, 8);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);
    ray_heap_destroy();
    PASS();
}

/* atom (non-vec) → prepare_attach rejects with "type" error. */
static test_result_t test_index_chunk_zone_atom_error(void) {
    ray_heap_init();
    ray_t* a = ray_i64(99);
    ray_t* wa = a;
    ray_t* r = ray_index_attach_chunk_zone(&wa, 8);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);
    ray_release(a);
    ray_heap_destroy();
    PASS();
}

/* Replace existing index with chunk_zone: prepare_attach drops the prior
 * index first.  Also exercises the `attrs & RAY_ATTR_HAS_INDEX` branch
 * in prepare_attach for the new kind. */
static test_result_t test_index_chunk_zone_replace_existing(void) {
    ray_heap_init();
    int64_t n = 500;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &i);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_zone(&w)));
    TEST_ASSERT_EQ_I((int)ray_index_kind(w), RAY_IDX_ZONE);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&w, 8)));
    TEST_ASSERT_EQ_I((int)ray_index_kind(w), RAY_IDX_CHUNK_ZONE);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* chunk_zone drop + info coverage (kind_name "chunk_zone" + info dict). */
static test_result_t test_index_chunk_zone_info_and_drop(void) {
    ray_heap_init();
    int64_t n = 600;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &i);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&w, 8)));
    /* info exercises kind_name "chunk_zone" + the RAY_IDX_CHUNK_ZONE case
     * in ray_index_info's switch. */
    ray_t* info = ray_index_info(w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(info));
    TEST_ASSERT_TRUE(info != RAY_NULL_OBJ);
    ray_release(info);

    /* Drop covers the RAY_IDX_CHUNK_ZONE branch of ray_index_release_payload. */
    ray_index_drop(&w);
    TEST_ASSERT_FALSE(w->attrs & RAY_ATTR_HAS_INDEX);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* chunk_zone retain_payload: drives the RAY_IDX_CHUNK_ZONE branch in
 * ray_index_retain_payload directly.  The retain helper is exposed via
 * idxop.h for the heap.c / vec.c symmetry it's meant to maintain; calling
 * it manually after an attach mimics the heap.c ray_alloc_copy code-path
 * where a shared RAY_INDEX block gets duplicated and per-kind children
 * need retaining.  Pairs with an explicit release to balance refcounts. */
static test_result_t test_index_chunk_zone_retain_payload(void) {
    ray_heap_init();
    int64_t n = 600;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &i);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&w, 8)));

    ray_index_t* ix = ray_index_payload(w->index);
    /* Bump rc on each child via retain_payload — direct exercise of the
     * RAY_IDX_CHUNK_ZONE arm. */
    ray_index_retain_payload(ix);
    /* Balance the bump so destroy doesn't leak. */
    if (ix->u.chunk_zone.mins      && !RAY_IS_ERR(ix->u.chunk_zone.mins))
        ray_release(ix->u.chunk_zone.mins);
    if (ix->u.chunk_zone.maxs      && !RAY_IS_ERR(ix->u.chunk_zone.maxs))
        ray_release(ix->u.chunk_zone.maxs);
    if (ix->u.chunk_zone.null_bits && !RAY_IS_ERR(ix->u.chunk_zone.null_bits))
        ray_release(ix->u.chunk_zone.null_bits);

    /* retain_saved is also exposed; call to drive coverage on its no-op body. */
    ray_index_retain_saved(ix);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* ─── hash_eq_rowsel: point-lookup fast path ─────────────────────────────── */

/* Helper: count total passing rows summed across all segments of a rowsel. */
static int64_t rowsel_count_pass(ray_t* sel) {
    if (!sel) return -1;
    return ray_rowsel_meta(sel)->total_pass;
}

/* I64 column, key present once. */
static test_result_t test_index_hash_eq_rowsel_i64_one_match(void) {
    ray_heap_init();
    int64_t xs[] = { 10, 20, 30, 40, 50 };
    ray_t* v = make_i64_vec(xs, 5);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));

    ray_t* sel = ray_index_hash_eq_rowsel(w, 30);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQ_I(rowsel_count_pass(sel), 1);

    /* Check the matching row is in segment 0 at offset 2. */
    ray_rowsel_t* m = ray_rowsel_meta(sel);
    TEST_ASSERT_EQ_I((int64_t)m->n_segs, 1);
    uint8_t* fl = ray_rowsel_flags(sel);
    uint32_t* off = ray_rowsel_offsets(sel);
    uint16_t* ia = ray_rowsel_idx(sel);
    TEST_ASSERT_EQ_I((int)fl[0], RAY_SEL_MIX);
    TEST_ASSERT_EQ_I((int64_t)off[1], 1);
    TEST_ASSERT_EQ_I((int64_t)ia[0], 2);

    ray_rowsel_release(sel);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* I64 column, key absent → empty rowsel (NOT NULL; NULL would be all-pass). */
static test_result_t test_index_hash_eq_rowsel_i64_no_match(void) {
    ray_heap_init();
    int64_t xs[] = { 10, 20, 30 };
    ray_t* v = make_i64_vec(xs, 3);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));

    ray_t* sel = ray_index_hash_eq_rowsel(w, 9999);
    TEST_ASSERT_NOT_NULL(sel);  /* must not collapse to NULL (= all-pass) */
    TEST_ASSERT_EQ_I(rowsel_count_pass(sel), 0);
    uint8_t* fl = ray_rowsel_flags(sel);
    TEST_ASSERT_EQ_I((int)fl[0], RAY_SEL_NONE);

    ray_rowsel_release(sel);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* Multiple matches in same column. */
static test_result_t test_index_hash_eq_rowsel_multiple(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 7, 2, 7, 3, 7, 4 };
    ray_t* v = make_i64_vec(xs, 7);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));

    ray_t* sel = ray_index_hash_eq_rowsel(w, 7);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQ_I(rowsel_count_pass(sel), 3);

    /* All matches in segment 0; offsets[1] = 3; idx[] contains 1,3,5 sorted. */
    uint16_t* ia = ray_rowsel_idx(sel);
    TEST_ASSERT_EQ_I((int64_t)ia[0], 1);
    TEST_ASSERT_EQ_I((int64_t)ia[1], 3);
    TEST_ASSERT_EQ_I((int64_t)ia[2], 5);

    ray_rowsel_release(sel);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* No index attached → NULL (caller falls back to scan). */
static test_result_t test_index_hash_eq_rowsel_no_index(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3 };
    ray_t* v = make_i64_vec(xs, 3);
    ray_t* sel = ray_index_hash_eq_rowsel(v, 1);
    TEST_ASSERT_NULL(sel);
    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* Wrong kind (zone) → NULL. */
static test_result_t test_index_hash_eq_rowsel_wrong_kind(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3 };
    ray_t* v = make_i64_vec(xs, 3);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_zone(&w)));
    ray_t* sel = ray_index_hash_eq_rowsel(w, 1);
    TEST_ASSERT_NULL(sel);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* NULL col / RAY_ERR / non-vec input → NULL. */
static test_result_t test_index_hash_eq_rowsel_null_input(void) {
    ray_heap_init();
    TEST_ASSERT_NULL(ray_index_hash_eq_rowsel(NULL, 0));

    /* atom (non-vec) */
    ray_t* a = ray_i64(7);
    TEST_ASSERT_NULL(ray_index_hash_eq_rowsel(a, 7));
    ray_release(a);

    /* RAY_ERROR */
    ray_t* err = ray_error("test", "x");
    TEST_ASSERT_NULL(ray_index_hash_eq_rowsel(err, 0));
    ray_error_free(err);

    ray_heap_destroy();
    PASS();
}

/* Float column → not eligible (NULL). */
static test_result_t test_index_hash_eq_rowsel_float_rejected(void) {
    ray_heap_init();
    double xs[] = { 1.0, 2.0, 3.0 };
    ray_t* v = make_f64_vec(xs, 3);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));
    /* float column, hash index built — but hash_key_in_range returns 0 for
     * float types, so we get NULL. */
    ray_t* sel = ray_index_hash_eq_rowsel(w, 1);
    TEST_ASSERT_NULL(sel);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* Key out of range for the column type. */
static test_result_t test_index_hash_eq_rowsel_out_of_range(void) {
    ray_heap_init();

    /* U8 column, key=300 > 255 → NULL */
    uint8_t ux[] = { 10, 20, 30 };
    ray_t* vu = ray_vec_new(RAY_U8, 3);
    for (int i = 0; i < 3; i++) vu = ray_vec_append(vu, &ux[i]);
    ray_t* wu = vu;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&wu)));
    TEST_ASSERT_NULL(ray_index_hash_eq_rowsel(wu, 300));
    TEST_ASSERT_NULL(ray_index_hash_eq_rowsel(wu, -1));
    ray_release(wu);

    /* I16 column, key out of range */
    int16_t sx[] = { 0, 1, 2 };
    ray_t* vs = ray_vec_new(RAY_I16, 3);
    for (int i = 0; i < 3; i++) vs = ray_vec_append(vs, &sx[i]);
    ray_t* ws = vs;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&ws)));
    TEST_ASSERT_NULL(ray_index_hash_eq_rowsel(ws, 100000));
    TEST_ASSERT_NULL(ray_index_hash_eq_rowsel(ws, -100000));
    ray_release(ws);

    /* I32 column, key out of i32 range */
    int32_t ix[] = { 0, 1, 2 };
    ray_t* vi = ray_vec_new(RAY_I32, 3);
    for (int i = 0; i < 3; i++) vi = ray_vec_append(vi, &ix[i]);
    ray_t* wi = vi;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&wi)));
    TEST_ASSERT_NULL(ray_index_hash_eq_rowsel(wi, (int64_t)INT32_MAX + 1));
    TEST_ASSERT_NULL(ray_index_hash_eq_rowsel(wi, (int64_t)INT32_MIN - 1));
    ray_release(wi);

    /* TIME column (int32 storage), key out of i32 range → NULL */
    int32_t tx[] = { 0, 3600, 86399 };
    ray_t* vt = ray_vec_new(RAY_TIME, 3);
    for (int i = 0; i < 3; i++) vt = ray_vec_append(vt, &tx[i]);
    ray_t* wt = vt;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&wt)));
    TEST_ASSERT_NULL(ray_index_hash_eq_rowsel(wt, (int64_t)INT32_MAX + 1));
    TEST_ASSERT_NULL(ray_index_hash_eq_rowsel(wt, (int64_t)INT32_MIN - 1));
    ray_release(wt);

    ray_heap_destroy();
    PASS();
}

/* Numeric type matrix that IS eligible: BOOL/U8/I16/I32/DATE/I64/TIME/TS.
 * Hits each switch arm of hash_key_in_range / hash_col_read_i64 / mix64
 * dispatch in hash_probe_setup. */
static test_result_t test_index_hash_eq_rowsel_type_matrix(void) {
    ray_heap_init();

    /* BOOL */
    uint8_t bx[] = { 1, 0, 1, 0 };
    ray_t* vb = ray_vec_new(RAY_BOOL, 4);
    for (int i = 0; i < 4; i++) vb = ray_vec_append(vb, &bx[i]);
    ray_t* wb = vb;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&wb)));
    ray_t* sb = ray_index_hash_eq_rowsel(wb, 1);
    TEST_ASSERT_NOT_NULL(sb);
    TEST_ASSERT_EQ_I(rowsel_count_pass(sb), 2);
    ray_rowsel_release(sb);
    ray_release(wb);

    /* U8 */
    uint8_t ux[] = { 5, 10, 5, 20 };
    ray_t* vu = ray_vec_new(RAY_U8, 4);
    for (int i = 0; i < 4; i++) vu = ray_vec_append(vu, &ux[i]);
    ray_t* wu = vu;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&wu)));
    ray_t* su = ray_index_hash_eq_rowsel(wu, 5);
    TEST_ASSERT_NOT_NULL(su);
    TEST_ASSERT_EQ_I(rowsel_count_pass(su), 2);
    ray_rowsel_release(su);
    ray_release(wu);

    /* I16 */
    int16_t sx[] = { -100, 200, -100, 300 };
    ray_t* vs = ray_vec_new(RAY_I16, 4);
    for (int i = 0; i < 4; i++) vs = ray_vec_append(vs, &sx[i]);
    ray_t* ws = vs;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&ws)));
    ray_t* ss = ray_index_hash_eq_rowsel(ws, -100);
    TEST_ASSERT_NOT_NULL(ss);
    TEST_ASSERT_EQ_I(rowsel_count_pass(ss), 2);
    ray_rowsel_release(ss);
    ray_release(ws);

    /* I32 */
    int32_t ix[] = { 1000, 2000, 1000 };
    ray_t* vi = ray_vec_new(RAY_I32, 3);
    for (int i = 0; i < 3; i++) vi = ray_vec_append(vi, &ix[i]);
    ray_t* wi = vi;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&wi)));
    ray_t* si = ray_index_hash_eq_rowsel(wi, 1000);
    TEST_ASSERT_NOT_NULL(si);
    TEST_ASSERT_EQ_I(rowsel_count_pass(si), 2);
    ray_rowsel_release(si);
    ray_release(wi);

    /* DATE (es=4) */
    int32_t dx[] = { 18000, 19000, 18000 };
    ray_t* vd = ray_vec_new(RAY_DATE, 3);
    for (int i = 0; i < 3; i++) vd = ray_vec_append(vd, &dx[i]);
    ray_t* wd = vd;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&wd)));
    ray_t* sd = ray_index_hash_eq_rowsel(wd, 18000);
    TEST_ASSERT_NOT_NULL(sd);
    TEST_ASSERT_EQ_I(rowsel_count_pass(sd), 2);
    ray_rowsel_release(sd);
    ray_release(wd);

    /* TIME (es=4, int32 storage with NULL_I32 sentinel).  All idxop
     * readers now agree with the builder on the 4-byte width, so the
     * hash-eq fast path is exact on TIME columns. */
    int32_t tx[] = { 1000, 2000, 1000 };
    ray_t* vt = ray_vec_new(RAY_TIME, 3);
    for (int i = 0; i < 3; i++) vt = ray_vec_append(vt, &tx[i]);
    ray_t* wt = vt;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&wt)));
    ray_t* st = ray_index_hash_eq_rowsel(wt, 1000);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_EQ_I(rowsel_count_pass(st), 2);
    ray_rowsel_release(st);
    ray_release(wt);

    /* TIMESTAMP (es=8, storage matches) */
    int64_t tsx[] = { 1700000000000LL, 1700000000001LL, 1700000000000LL };
    ray_t* vts = ray_vec_new(RAY_TIMESTAMP, 3);
    for (int i = 0; i < 3; i++) vts = ray_vec_append(vts, &tsx[i]);
    ray_t* wts = vts;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&wts)));
    ray_t* sts = ray_index_hash_eq_rowsel(wts, 1700000000000LL);
    TEST_ASSERT_NOT_NULL(sts);
    TEST_ASSERT_EQ_I(rowsel_count_pass(sts), 2);
    ray_rowsel_release(sts);
    ray_release(wts);

    ray_heap_destroy();
    PASS();
}

/* Spread matches across multiple morsel segments (n > RAY_MORSEL_ELEMS).
 * Exercises seg_offsets / per-seg flag flipping for MIX and NONE. */
static test_result_t test_index_hash_eq_rowsel_multi_segment(void) {
    ray_heap_init();
    /* 3000 rows → 3 segments of 1024 (last short).  Plant the target value
     * in seg 0 (row 5) and seg 2 (row 2100); seg 1 has none.  Filler values
     * are negative so they can't collide with the positive target. */
    int64_t n = 3000;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t x = (i == 5 || i == 2100) ? 999 : -(i + 1);
        v = ray_vec_append(v, &x);
    }
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));

    ray_t* sel = ray_index_hash_eq_rowsel(w, 999);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQ_I(rowsel_count_pass(sel), 2);
    ray_rowsel_t* m = ray_rowsel_meta(sel);
    TEST_ASSERT_EQ_I((int64_t)m->n_segs, 3);
    uint8_t* fl = ray_rowsel_flags(sel);
    TEST_ASSERT_EQ_I((int)fl[0], RAY_SEL_MIX);
    TEST_ASSERT_EQ_I((int)fl[1], RAY_SEL_NONE);
    TEST_ASSERT_EQ_I((int)fl[2], RAY_SEL_MIX);

    ray_rowsel_release(sel);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* Many matches (> 16) → triggers the dynamic-grow path in the match-buffer
 * collect loop (mcnt == mcap → realloc). */
static test_result_t test_index_hash_eq_rowsel_grow_buffer(void) {
    ray_heap_init();
    int64_t n = 100;
    ray_t* v = ray_vec_new(RAY_I64, n);
    /* Plant 50 occurrences of the target key — well above initial mcap=16. */
    for (int64_t i = 0; i < n; i++) {
        int64_t x = (i < 50) ? 42 : (1000 + i);
        v = ray_vec_append(v, &x);
    }
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));

    ray_t* sel = ray_index_hash_eq_rowsel(w, 42);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQ_I(rowsel_count_pass(sel), 50);

    ray_rowsel_release(sel);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* All rows in one morsel match → that segment gets the ALL flag, no idx[]
 * entries.  The key must stay SPARSE overall (a dense key aborts to the
 * scan path — see the dense_aborts test below), so the matching morsel
 * sits inside a much larger column of distinct values: segment 0 is ALL,
 * the rest are NONE. */
static test_result_t test_index_hash_eq_rowsel_all_segment(void) {
    ray_heap_init();
    int64_t m = RAY_MORSEL_ELEMS;  /* 1024 */
    int64_t n = 128 * m;           /* keep the key well under the dense-abort budget (n/64) */
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t x = (i < m) ? 7 : 1000 + i;
        v = ray_vec_append(v, &x);
    }
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));

    ray_t* sel = ray_index_hash_eq_rowsel(w, 7);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQ_I(rowsel_count_pass(sel), m);
    uint8_t* fl = ray_rowsel_flags(sel);
    TEST_ASSERT_EQ_I((int)fl[0], RAY_SEL_ALL);
    TEST_ASSERT_EQ_I((int)fl[1], RAY_SEL_NONE);

    ray_rowsel_release(sel);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* Dense key: when one key covers a large fraction of the column the probe
 * must ABORT (return NULL) so the caller falls back to the SIMD scan — the
 * scattered chain walk loses to the scan at that density. */
static test_result_t test_index_hash_eq_rowsel_dense_aborts(void) {
    ray_heap_init();
    int64_t n = RAY_MORSEL_ELEMS;  /* 1024, all one value → maximally dense */
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t x = 7;
        v = ray_vec_append(v, &x);
    }
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));

    ray_t* sel = ray_index_hash_eq_rowsel(w, 7);
    TEST_ASSERT(sel == NULL, "dense key must abort to the scan path");

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* Hash index built over a column with nulls — nulls are skipped during
 * insertion AND during probe (because rid->chain steps through inserted
 * rows only).  Verifies hash_eq_rowsel returns the non-null match count. */
static test_result_t test_index_hash_eq_rowsel_with_nulls(void) {
    ray_heap_init();
    int64_t xs[] = { 7, 100, 7, 200, 7 };
    ray_t* v = make_i64_vec(xs, 5);
    /* Mark row 4 (value 7) null → only rows 0 and 2 should match. */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 4, true), RAY_OK);

    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));

    ray_t* sel = ray_index_hash_eq_rowsel(w, 7);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQ_I(rowsel_count_pass(sel), 2);

    ray_rowsel_release(sel);
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

/* Stale index: change parent->len without rebuilding → built_for_len
 * mismatch → hash_probe_setup returns NULL. */
static test_result_t test_index_hash_eq_rowsel_stale(void) {
    ray_heap_init();
    int64_t xs[] = { 1, 2, 3, 4 };
    ray_t* v = make_i64_vec(xs, 4);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&w)));

    /* Tamper: shrink the recorded length so built_for_len != col->len. */
    ray_index_t* ix = ray_index_payload(w->index);
    ix->built_for_len = 99;

    ray_t* sel = ray_index_hash_eq_rowsel(w, 1);
    TEST_ASSERT_NULL(sel);

    /* Restore so destroy can clean up. */
    ix->built_for_len = w->len;
    ray_release(w);
    ray_heap_destroy();
    PASS();
}

const test_entry_t index_entries[] = {
    { "index/attach_drop_no_nulls",          test_index_attach_drop_no_nulls,          NULL, NULL },
    { "index/attach_drop_with_inline_nulls", test_index_attach_drop_with_inline_nulls, NULL, NULL },
    { "index/attach_drop_large_sentinel_nulls", test_index_attach_drop_large_sentinel_nulls, NULL, NULL },
    { "index/replace_existing",              test_index_replace_existing,              NULL, NULL },
    { "index/mutation_drops",                test_index_mutation_drops,                NULL, NULL },
    { "index/float_zone",                    test_index_float_zone,                    NULL, NULL },
    { "index/unsupported_type",              test_index_unsupported_type,              NULL, NULL },
    { "index/hash_attach_drop",              test_index_hash_attach_drop,              NULL, NULL },
    { "index/hash_with_nulls_preserved",     test_index_hash_with_nulls_preserved,     NULL, NULL },
    { "index/sort_attach_drop",              test_index_sort_attach_drop,              NULL, NULL },
    { "index/bloom_attach_drop",             test_index_bloom_attach_drop,             NULL, NULL },
    { "index/replace_cross_kind",            test_index_replace_cross_kind,            NULL, NULL },
    { "index/insert_at_drops_index",         test_index_insert_at_drops_index,         NULL, NULL },
    { "index/null_readers_through_helper",   test_index_null_readers_through_helper,   NULL, NULL },
    { "index/aux_helper_slice",          test_index_aux_helper_slice,          NULL, NULL },
    { "index/drop_under_shared_cow",         test_index_drop_under_shared_cow,         NULL, NULL },
    { "index/persistence_roundtrip",         test_index_persistence_roundtrip,         NULL, NULL },
    { "index/bool_zone_and_hash",            test_index_bool_zone_and_hash,            NULL, NULL },
    { "index/i16_zone_and_hash",             test_index_i16_zone_and_hash,             NULL, NULL },
    { "index/i32_hash",                      test_index_i32_hash,                      NULL, NULL },
    { "index/f32_zone_and_hash",             test_index_f32_zone_and_hash,             NULL, NULL },
    { "index/time_timestamp_zone",           test_index_time_timestamp_zone,           NULL, NULL },
    { "index/date_zone",                     test_index_date_zone,                     NULL, NULL },
    { "index/zone_all_null",                 test_index_zone_all_null,                 NULL, NULL },
    { "index/zone_float_all_null",           test_index_zone_float_all_null,           NULL, NULL },
    { "index/zone_float_nan",                test_index_zone_float_nan,                NULL, NULL },
    { "index/hash_f64_nan",                  test_index_hash_f64_nan,                  NULL, NULL },
    { "index/attach_slice_error",            test_index_attach_slice_error,            NULL, NULL },
    { "index/retain_payload_direct",         test_index_retain_payload_direct,         NULL, NULL },
    { "index/release_saved_noop",            test_index_release_saved_noop,            NULL, NULL },
    { "index/retain_saved_noop",             test_index_retain_saved_noop,             NULL, NULL },
    { "index/drop_shared_with_large_nulls",  test_index_drop_shared_with_large_nulls,  NULL, NULL },
    { "index/info_no_index",                 test_index_info_no_index,                 NULL, NULL },
    { "index/bloom_with_nulls",              test_index_bloom_with_nulls,              NULL, NULL },
    { "index/guid_unsupported",              test_index_guid_unsupported,              NULL, NULL },
    { "index/sort_all_same",                 test_index_sort_all_same,                 NULL, NULL },
    { "index/builtin_fns",                   test_index_builtin_fns,                   NULL, NULL },
    { "index/attach_null_vec",               test_index_attach_null_vec,               NULL, NULL },
    { "index/attach_on_linked_vec",          test_index_attach_on_linked_vec,          NULL, NULL },
    { "index/drop_null_guard",               test_index_drop_null_guard,               NULL, NULL },
    { "index/hash_f32_nan",                  test_index_hash_f32_nan,                  NULL, NULL },
    { "index/zone_f32_nan",                  test_index_zone_f32_nan,                  NULL, NULL },
    { "index/zone_f32_nulls",                test_index_zone_f32_nulls,                NULL, NULL },
    { "index/zone_f32_all_null",             test_index_zone_f32_all_null,             NULL, NULL },
    { "index/hash_f64_neg_zero",             test_index_hash_f64_neg_zero,             NULL, NULL },
    { "index/hash_f32_neg_zero",             test_index_hash_f32_neg_zero,             NULL, NULL },
    { "index/time_all_kinds",                test_index_time_all_kinds,                NULL, NULL },
    { "index/date_all_kinds",                test_index_date_all_kinds,                NULL, NULL },
    { "index/timestamp_all_kinds",           test_index_timestamp_all_kinds,           NULL, NULL },
    { "index/i16_sort_and_bloom",            test_index_i16_sort_and_bloom,            NULL, NULL },
    { "index/u8_sort_and_bloom",             test_index_u8_sort_and_bloom,             NULL, NULL },
    { "index/bool_sort_and_bloom",           test_index_bool_sort_and_bloom,           NULL, NULL },
    { "index/i32_zone_sort_bloom",           test_index_i32_zone_sort_bloom,           NULL, NULL },
    { "index/f64_sort_and_bloom",            test_index_f64_sort_and_bloom,            NULL, NULL },
    { "index/f32_sort_and_bloom",            test_index_f32_sort_and_bloom,            NULL, NULL },
    { "index/attach_null_vp",                test_index_attach_null_vp,                NULL, NULL },
    { "index/fn_null_input",                 test_index_fn_null_input,                 NULL, NULL },
    { "index/drop_fn_null_error",            test_index_drop_fn_null_error,            NULL, NULL },
    { "index/has_fn_null",                   test_index_has_fn_null,                   NULL, NULL },
    { "index/release_payload_null_ptrs",     test_index_release_payload_null_ptrs,     NULL, NULL },
    { "index/retain_payload_null_ptrs",      test_index_retain_payload_null_ptrs,      NULL, NULL },
    { "index/empty_vec_all_kinds",           test_index_empty_vec_all_kinds,           NULL, NULL },
    { "index/hash_nulls_multi_type",         test_index_hash_nulls_multi_type,         NULL, NULL },
    { "index/bloom_nulls_multi_type",        test_index_bloom_nulls_multi_type,        NULL, NULL },
    { "index/sort_with_nulls",               test_index_sort_with_nulls,               NULL, NULL },
    { "index/zone_single_value",             test_index_zone_single_value,             NULL, NULL },
    { "index/bloom_f64_nan",                 test_index_bloom_f64_nan,                 NULL, NULL },
    { "index/bloom_f32_nan",                 test_index_bloom_f32_nan,                 NULL, NULL },
    { "index/bloom_f64_neg_zero",            test_index_bloom_f64_neg_zero,            NULL, NULL },
    { "index/zone_f64_neg_zero",             test_index_zone_f64_neg_zero,             NULL, NULL },
    { "index/info_sort",                     test_index_info_sort,                     NULL, NULL },
    { "index/info_bloom",                    test_index_info_bloom,                    NULL, NULL },
    { "index/info_hash",                     test_index_info_hash,                     NULL, NULL },
    { "index/info_zone_int",                 test_index_info_zone_int,                 NULL, NULL },
    { "index/info_zone_f64",                 test_index_info_zone_f64,                 NULL, NULL },
    { "index/hash_single_elem",              test_index_hash_single_elem,              NULL, NULL },
    { "index/zone_i64_mixed_nulls",          test_index_zone_i64_mixed_nulls,          NULL, NULL },
    { "index/zone_f64_nan_and_null",         test_index_zone_f64_nan_and_null,         NULL, NULL },
    { "index/fn_error_propagation",          test_index_fn_error_propagation,          NULL, NULL },
    { "index/hash_large_n",                  test_index_hash_large_n,                  NULL, NULL },
    { "index/bloom_large_n",                 test_index_bloom_large_n,                 NULL, NULL },
    { "index/list_unsupported",              test_index_list_unsupported,              NULL, NULL },
    { "index/attach_atom_error",             test_index_attach_atom_error,             NULL, NULL },
    { "index/hash_collisions",               test_index_hash_collisions,               NULL, NULL },
    { "index/bloom_small_n_set",             test_index_bloom_small_n_set,             NULL, NULL },
    { "index/double_drop",                   test_index_double_drop,                   NULL, NULL },
    { "index/zone_f64_only_nan",             test_index_zone_f64_only_nan,             NULL, NULL },
    { "index/zone_f32_only_nan",             test_index_zone_f32_only_nan,             NULL, NULL },
    { "index/zone_i16_nulls",                test_index_zone_i16_nulls,               NULL, NULL },
    { "index/u8_bool_non_nullable",           test_index_u8_bool_non_nullable,         NULL, NULL },
    { "index/zone_date_nulls",               test_index_zone_date_nulls,              NULL, NULL },
    { "index/zone_time_nulls",               test_index_zone_time_nulls,              NULL, NULL },
    { "index/zone_timestamp_nulls",          test_index_zone_timestamp_nulls,         NULL, NULL },
    { "index/zone_f64_null_and_nan",         test_index_zone_f64_null_and_nan,        NULL, NULL },
    { "index/hash_n3",                       test_index_hash_n3,                      NULL, NULL },
    { "index/hash_n4",                       test_index_hash_n4,                      NULL, NULL },
    { "index/hash_n5",                       test_index_hash_n5,                      NULL, NULL },
    { "index/chunk_zone_i64_basic",          test_index_chunk_zone_i64_basic,         NULL, NULL },
    { "index/chunk_zone_i64_with_nulls",     test_index_chunk_zone_i64_with_nulls,    NULL, NULL },
    { "index/chunk_zone_u8_bool",            test_index_chunk_zone_u8_bool,           NULL, NULL },
    { "index/chunk_zone_i16",                test_index_chunk_zone_i16,               NULL, NULL },
    { "index/chunk_zone_i32_date",           test_index_chunk_zone_i32_date,          NULL, NULL },
    { "index/chunk_zone_time_timestamp",     test_index_chunk_zone_time_timestamp,    NULL, NULL },
    { "index/chunk_zone_f64_basic",          test_index_chunk_zone_f64_basic,         NULL, NULL },
    { "index/chunk_zone_f32_basic",          test_index_chunk_zone_f32_basic,         NULL, NULL },
    { "index/chunk_zone_f64_bare_nan",       test_index_chunk_zone_f64_bare_nan,      NULL, NULL },
    { "index/chunk_zone_f64_with_nulls",     test_index_chunk_zone_f64_with_nulls,    NULL, NULL },
    { "index/chunk_zone_log2_zero_default",  test_index_chunk_zone_log2_zero_default, NULL, NULL },
    { "index/chunk_zone_log2_too_small",     test_index_chunk_zone_log2_too_small,    NULL, NULL },
    { "index/chunk_zone_log2_too_large",     test_index_chunk_zone_log2_too_large,    NULL, NULL },
    { "index/chunk_zone_column_too_small",   test_index_chunk_zone_column_too_small,  NULL, NULL },
    { "index/chunk_zone_unsupported_type",   test_index_chunk_zone_unsupported_type,  NULL, NULL },
    { "index/chunk_zone_null_vp",            test_index_chunk_zone_null_vp,           NULL, NULL },
    { "index/chunk_zone_atom_error",         test_index_chunk_zone_atom_error,        NULL, NULL },
    { "index/chunk_zone_replace_existing",   test_index_chunk_zone_replace_existing,  NULL, NULL },
    { "index/chunk_zone_info_and_drop",      test_index_chunk_zone_info_and_drop,     NULL, NULL },
    { "index/chunk_zone_retain_payload",     test_index_chunk_zone_retain_payload,    NULL, NULL },
    { "index/hash_eq_rowsel_i64_one_match",  test_index_hash_eq_rowsel_i64_one_match, NULL, NULL },
    { "index/hash_eq_rowsel_i64_no_match",   test_index_hash_eq_rowsel_i64_no_match,  NULL, NULL },
    { "index/hash_eq_rowsel_multiple",       test_index_hash_eq_rowsel_multiple,      NULL, NULL },
    { "index/hash_eq_rowsel_no_index",       test_index_hash_eq_rowsel_no_index,      NULL, NULL },
    { "index/hash_eq_rowsel_wrong_kind",     test_index_hash_eq_rowsel_wrong_kind,    NULL, NULL },
    { "index/hash_eq_rowsel_null_input",     test_index_hash_eq_rowsel_null_input,    NULL, NULL },
    { "index/hash_eq_rowsel_float_rejected", test_index_hash_eq_rowsel_float_rejected,NULL, NULL },
    { "index/hash_eq_rowsel_out_of_range",   test_index_hash_eq_rowsel_out_of_range,  NULL, NULL },
    { "index/hash_eq_rowsel_type_matrix",    test_index_hash_eq_rowsel_type_matrix,   NULL, NULL },
    { "index/hash_eq_rowsel_multi_segment",  test_index_hash_eq_rowsel_multi_segment, NULL, NULL },
    { "index/hash_eq_rowsel_grow_buffer",    test_index_hash_eq_rowsel_grow_buffer,   NULL, NULL },
    { "index/hash_eq_rowsel_all_segment",    test_index_hash_eq_rowsel_all_segment,   NULL, NULL },
    { "index/hash_eq_rowsel_dense_aborts",   test_index_hash_eq_rowsel_dense_aborts,  NULL, NULL },
    { "index/hash_eq_rowsel_with_nulls",     test_index_hash_eq_rowsel_with_nulls,    NULL, NULL },
    { "index/hash_eq_rowsel_stale",          test_index_hash_eq_rowsel_stale,         NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
