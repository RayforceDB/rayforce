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
#include "store/col.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

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

/* Snapshot the 16-byte nullmap union and attrs bits we care about. */
typedef struct {
    uint8_t bytes[16];
    uint8_t attrs;  /* HAS_NULLS | NULLMAP_EXT */
} nullmap_snap_t;

static nullmap_snap_t snap_take(const ray_t* v) {
    nullmap_snap_t s;
    memcpy(s.bytes, v->nullmap, 16);
    s.attrs = v->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_NULLMAP_EXT);
    return s;
}

static int snap_eq(const nullmap_snap_t* a, const nullmap_snap_t* b) {
    return memcmp(a->bytes, b->bytes, 16) == 0 && a->attrs == b->attrs;
}

/* ─── Tests ────────────────────────────────────────────────────────── */

static test_result_t test_index_attach_drop_no_nulls(void) {
    ray_heap_init();
    int64_t xs[] = { 5, 1, 9, 3, 7 };
    ray_t* v = make_i64_vec(xs, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_FALSE(v->attrs & RAY_ATTR_HAS_INDEX);

    nullmap_snap_t before = snap_take(v);

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

    /* Drop and verify the nullmap union round-trips byte-for-byte. */
    ray_t* d = ray_index_drop(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    TEST_ASSERT_FALSE(w->attrs & RAY_ATTR_HAS_INDEX);

    nullmap_snap_t after = snap_take(w);
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

    nullmap_snap_t before = snap_take(v);

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
    nullmap_snap_t after = snap_take(w);
    TEST_ASSERT_TRUE(snap_eq(&before, &after));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_NULLS);

    ray_release(w);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_index_attach_drop_with_ext_nullmap(void) {
    ray_heap_init();
    int64_t n = 200;  /* > 128 forces external nullmap */
    ray_t* v = ray_vec_new(RAY_I32, n);
    int32_t z = 0;
    for (int64_t i = 0; i < n; i++) v = ray_vec_append(v, &z);
    /* Set a few nulls past the 128-element inline boundary. */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 130, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 199, true), RAY_OK);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_NULLMAP_EXT);

    nullmap_snap_t before = snap_take(v);

    ray_t* w = v;
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_FALSE(w->attrs & RAY_ATTR_NULLMAP_EXT);  /* moved into index */

    /* is_null still returns true for the marked rows. */
    TEST_ASSERT_TRUE (ray_vec_is_null(w, 130));
    TEST_ASSERT_TRUE (ray_vec_is_null(w, 199));
    TEST_ASSERT_FALSE(ray_vec_is_null(w, 0));

    ray_index_drop(&w);
    nullmap_snap_t after = snap_take(w);
    TEST_ASSERT_TRUE(snap_eq(&before, &after));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_NULLMAP_EXT);
    TEST_ASSERT_TRUE (ray_vec_is_null(w, 130));

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
    nullmap_snap_t before = snap_take(v);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_INDEX);

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I((int)ix->kind, RAY_IDX_HASH);
    TEST_ASSERT_EQ_I(ix->u.hash.n_keys, 6);
    TEST_ASSERT_NOT_NULL(ix->u.hash.table);
    TEST_ASSERT_NOT_NULL(ix->u.hash.chain);
    TEST_ASSERT_EQ_I(ix->u.hash.chain->len, 6);

    /* Walk the chain for key 7: must find rids 0 and 2 (both store 7). */
    int64_t mask = (int64_t)ix->u.hash.mask;
    int64_t* tbl = (int64_t*)ray_data(ix->u.hash.table);
    int64_t* chn = (int64_t*)ray_data(ix->u.hash.chain);
    /* Inline-recompute the same hash as the builder (numeric_key_word for I64
     * is the raw 64-bit value, then mix64). */
    uint64_t h = (uint64_t)7;
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    int64_t slot = (int64_t)(h & (uint64_t)mask);
    int found_at[8] = { 0 };
    int n_found = 0;
    for (int64_t rid = tbl[slot] - 1; rid >= 0 && n_found < 8;
         rid = chn[rid] - 1) {
        int64_t val;
        memcpy(&val, (char*)ray_data(w) + rid * 8, 8);
        if (val == 7) found_at[n_found++] = (int)rid;
    }
    TEST_ASSERT_EQ_I(n_found, 2);
    TEST_ASSERT_TRUE((found_at[0] == 0 && found_at[1] == 2) ||
                     (found_at[0] == 2 && found_at[1] == 0));

    ray_index_drop(&w);
    nullmap_snap_t after = snap_take(w);
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
    nullmap_snap_t before = snap_take(v);

    ray_t* w = v;
    ray_t* r = ray_index_attach_hash(&w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_EQ_I(ix->u.hash.n_keys, 3);  /* null row excluded */

    /* Null still readable through ray_vec_is_null. */
    TEST_ASSERT_TRUE(ray_vec_is_null(w, 1));
    TEST_ASSERT_TRUE(w->attrs & RAY_ATTR_HAS_NULLS);

    ray_index_drop(&w);
    nullmap_snap_t after = snap_take(w);
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
    nullmap_snap_t before = snap_take(v);

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
    nullmap_snap_t after = snap_take(w);
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
    nullmap_snap_t before = snap_take(v);

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
    nullmap_snap_t after = snap_take(w);
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
    /* 200 elements forces ext_nullmap. */
    int64_t n = 200;
    ray_t* v = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t x = i * 10;
        v = ray_vec_append(v, &x);
    }
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 7, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 150, true), RAY_OK);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_NULLMAP_EXT);

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

/* ─── Slice handling in ray_vec_nullmap_bytes ─────────────────────── */

static test_result_t test_index_nullmap_helper_slice(void) {
    ray_heap_init();
    /* Build a parent with nulls at row 1 and row 4. */
    int64_t xs[] = { 100, 200, 300, 400, 500, 600 };
    ray_t* v = make_i64_vec(xs, 6);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 4, true), RAY_OK);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);

    /* Slice [2..6) — rows 2,3,4,5 in the parent, with row 4 (parent
     * index) being null — slice-local index 2. */
    ray_t* s = ray_vec_slice(v, 2, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_TRUE(s->attrs & RAY_ATTR_SLICE);
    /* Slice itself does NOT carry HAS_NULLS — that's the codebase invariant. */
    TEST_ASSERT_FALSE(s->attrs & RAY_ATTR_HAS_NULLS);

    /* The helper must still resolve to the parent's bitmap and return
     * the correct bit_offset (= slice_offset = 2). */
    int64_t off = -1, lb = -1;
    const uint8_t* bits = ray_vec_nullmap_bytes(s, &off, &lb);
    TEST_ASSERT_NOT_NULL(bits);
    TEST_ASSERT_EQ_I(off, 2);
    TEST_ASSERT_TRUE(lb >= 8);
    /* Parent bit 4 must be set in the buffer (bit 4 = byte 0 bit 4). */
    TEST_ASSERT_TRUE((bits[(off + 2) / 8] >> ((off + 2) % 8)) & 1);
    /* And parent bit 1 must also be set (parent has it). */
    TEST_ASSERT_TRUE((bits[1 / 8] >> (1 % 8)) & 1);

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
    ray_heap_init();
    /* 5-element vec with one null in the middle. */
    int64_t xs[] = { 100, 200, 300, 400, 500 };
    ray_t* v = make_i64_vec(xs, 5);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 2, true), RAY_OK);

    /* Snapshot the bitmap pointer/contents before attach. */
    int64_t pre_off = -1, pre_len = -1;
    const uint8_t* pre = ray_vec_nullmap_bytes(v, &pre_off, &pre_len);
    TEST_ASSERT_NOT_NULL(pre);
    TEST_ASSERT_EQ_I(pre_off, 0);
    TEST_ASSERT_TRUE(pre_len >= 8);
    /* Bit 2 must be set in the pre-snapshot. */
    TEST_ASSERT_TRUE((pre[0] >> 2) & 1);

    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_zone(&w)));

    /* After attach, the helper must still report bit 2 as set, even
     * though w->nullmap[] is now the index pointer. */
    int64_t post_off = -1, post_len = -1;
    const uint8_t* post = ray_vec_nullmap_bytes(w, &post_off, &post_len);
    TEST_ASSERT_NOT_NULL(post);
    TEST_ASSERT_EQ_I(post_off, 0);
    TEST_ASSERT_TRUE((post[0] >> 2) & 1);

    /* The helper must NOT return the parent's now-clobbered nullmap[]
     * (which holds an index pointer in its first 8 bytes). */
    TEST_ASSERT_TRUE(post != w->nullmap);

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

const test_entry_t index_entries[] = {
    { "index/attach_drop_no_nulls",          test_index_attach_drop_no_nulls,          NULL, NULL },
    { "index/attach_drop_with_inline_nulls", test_index_attach_drop_with_inline_nulls, NULL, NULL },
    { "index/attach_drop_with_ext_nullmap",  test_index_attach_drop_with_ext_nullmap,  NULL, NULL },
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
    { "index/nullmap_helper_slice",          test_index_nullmap_helper_slice,          NULL, NULL },
    { "index/drop_under_shared_cow",         test_index_drop_under_shared_cow,         NULL, NULL },
    { "index/persistence_roundtrip",         test_index_persistence_roundtrip,         NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
