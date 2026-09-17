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

/*
 * test_domain.c -- SYM domain objects (src/table/domain.c), Phase 1.
 *
 * Covers: runtime-singleton identity + delegation to ray_sym_*; the
 * non-NULL domain invariant across every SYM vec constructor path
 * (vec_new, append growth, COW, slice, concat, serde round-trip,
 * col_save/col_load, col_mmap); FILE domain open/find/str/count/release
 * semantics over a real symfile; and that the domain pointer never
 * reaches disk.
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "table/domain.h"
#include "store/col.h"
#include "store/serde.h"
#include "ops/ops.h"   /* RAY_PARTED_BASE (parted-flatten adoption test) */
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define TMP_DOM_SYM_PATH "/tmp/rayforce_test_domain_sym"
#define TMP_DOM_COL_PATH "/tmp/rayforce_test_domain_col"

/* ---- Setup / Teardown -------------------------------------------------- */

static void domain_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void domain_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- runtime singleton -------------------------------------------------- */

static test_result_t test_domain_runtime_identity(void) {
    ray_sym_domain_t* d1 = ray_sym_runtime_domain();
    ray_sym_domain_t* d2 = ray_sym_runtime_domain();
    TEST_ASSERT_NOT_NULL(d1);
    TEST_ASSERT_EQ_PTR(d1, d2);

    /* retain/release are no-ops on the singleton — must not crash and
     * must not change identity. */
    ray_sym_domain_retain(d1);
    ray_sym_domain_release(d1);
    ray_sym_domain_release(d1);
    TEST_ASSERT_EQ_PTR(ray_sym_runtime_domain(), d1);

    /* No backing file. */
    TEST_ASSERT_NULL(ray_sym_domain_path(d1));
    PASS();
}

static test_result_t test_domain_runtime_delegation(void) {
    ray_sym_domain_t* d = ray_sym_runtime_domain();

    int64_t id_a = ray_sym_intern("dom_alpha", 9);
    int64_t id_b = ray_sym_intern("dom_beta", 8);
    TEST_ASSERT(id_a >= 1 && id_b >= 1, "intern ids");

    /* count agrees */
    TEST_ASSERT_EQ_I(ray_sym_domain_count(d), (int64_t)ray_sym_count());

    /* find agrees: hit + miss */
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d, "dom_alpha", 9), id_a);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d, "dom_beta", 8), id_b);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d, "dom_missing", 11), -1);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d, "dom_missing", 11), ray_sym_find("dom_missing", 11));

    /* str agrees (borrowed atoms from the same table) */
    ray_t* s_dom = ray_sym_domain_str(d, id_a);
    ray_t* s_sym = ray_sym_str(id_a);
    TEST_ASSERT_EQ_PTR(s_dom, s_sym);

    /* intern delegates on the runtime domain */
    int64_t id_c = ray_sym_domain_intern(d, "dom_gamma", 9);
    TEST_ASSERT_EQ_I(id_c, ray_sym_find("dom_gamma", 9));

    /* flush is a no-op on the runtime domain (sym.c owns its own
     * persistence) — RAY_OK, never an error (post-flip semantics). */
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(d, false), RAY_OK);
    PASS();
}

/* ---- constructor paths all yield runtime-domain vecs -------------------- */

static test_result_t test_domain_vec_new_attach(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();

    ray_t* v8  = ray_sym_vec_new(RAY_SYM_W8, 4);
    ray_t* v64 = ray_sym_vec_new(RAY_SYM_W64, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v8));
    TEST_ASSERT_FALSE(RAY_IS_ERR(v64));
    TEST_ASSERT_EQ_PTR(v8->sym_domain, rt);
    TEST_ASSERT_EQ_PTR(v64->sym_domain, rt);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(v8), rt);

    /* ray_vec_new(RAY_SYM, …) delegates through the same chokepoint */
    ray_t* vg = ray_vec_new(RAY_SYM, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vg));
    TEST_ASSERT_EQ_PTR(vg->sym_domain, rt);

    ray_release(v8);
    ray_release(v64);
    ray_release(vg);
    PASS();
}

static test_result_t test_domain_append_growth_and_cow(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();

    /* Growth: start at capacity 0, append enough to force reallocs. */
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    for (int64_t i = 0; i < 100; i++) {
        int64_t id = ray_sym_intern("grow_x", 6);
        v = ray_vec_append(v, &id);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    }
    TEST_ASSERT_EQ_I(v->len, 100);
    TEST_ASSERT_EQ_PTR(v->sym_domain, rt);

    /* COW: share the vec, then append — the appended copy is a fresh
     * header copy via ray_alloc_copy and must carry the domain. */
    ray_retain(v);
    int64_t id = ray_sym_intern("cow_y", 5);
    ray_t* w = ray_vec_append(v, &id);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w));
    TEST_ASSERT(w != v, "COW must copy a shared vec");
    TEST_ASSERT_EQ_PTR(w->sym_domain, rt);
    TEST_ASSERT_EQ_PTR(v->sym_domain, rt);

    /* ray_vec_append COW'd the shared vec and already dropped the extra
     * retain via ray_cow's release — one release here drops our ref. */
    ray_release(w);
    ray_release(v);
    PASS();
}

static test_result_t test_domain_slice_accessor(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();

    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 8);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 8;
    int64_t* d = (int64_t*)ray_data(v);
    for (int i = 0; i < 8; i++) d[i] = 0;

    ray_t* s = ray_vec_slice(v, 2, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_TRUE(s->attrs & RAY_ATTR_SLICE);
    /* Slice header carries slice_parent/slice_offset in aux — the
     * accessor must resolve the domain through the parent. */
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(s), rt);

    /* Slice-of-slice resolves to the ultimate parent. */
    ray_t* s2 = ray_vec_slice(s, 1, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s2));
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(s2), rt);

    ray_release(s2);
    ray_release(s);
    ray_release(v);
    PASS();
}

static test_result_t test_domain_concat_attach(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();

    int64_t ida = ray_sym_intern("cc_a", 4);
    int64_t idb = ray_sym_intern("cc_b", 4);

    ray_t* a = ray_sym_vec_new(RAY_SYM_W64, 2);
    ray_t* b = ray_sym_vec_new(RAY_SYM_W64, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    a->len = 2; b->len = 2;
    ((int64_t*)ray_data(a))[0] = ida; ((int64_t*)ray_data(a))[1] = idb;
    ((int64_t*)ray_data(b))[0] = idb; ((int64_t*)ray_data(b))[1] = ida;

    ray_t* c = ray_vec_concat(a, b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->len, 4);
    TEST_ASSERT_EQ_PTR(c->sym_domain, rt);

    /* Mixed-width concat takes the widen loop — same contract. */
    ray_t* n8 = ray_sym_vec_new(RAY_SYM_W8, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(n8));
    n8->len = 2;
    ((uint8_t*)ray_data(n8))[0] = (uint8_t)ida;
    ((uint8_t*)ray_data(n8))[1] = (uint8_t)idb;
    ray_t* m = ray_vec_concat(n8, a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(m));
    TEST_ASSERT_EQ_PTR(m->sym_domain, rt);

    /* Concat with a slice input resolves through the parent. */
    ray_t* sl = ray_vec_slice(a, 0, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sl));
    ray_t* cs = ray_vec_concat(sl, b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(cs));
    TEST_ASSERT_EQ_PTR(cs->sym_domain, rt);

    ray_release(cs);
    ray_release(sl);
    ray_release(m);
    ray_release(n8);
    ray_release(c);
    ray_release(b);
    ray_release(a);
    PASS();
}

static test_result_t test_domain_serde_roundtrip(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();

    int64_t ida = ray_sym_intern("ser_a", 5);
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 3;
    int64_t* d = (int64_t*)ray_data(v);
    d[0] = ida; d[1] = ida; d[2] = 0;

    ray_t* bytes = ray_ser(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(bytes));

    /* The domain pointer must never appear in the wire bytes. */
    {
        ray_sym_domain_t* dom = v->sym_domain;
        const uint8_t* hay = (const uint8_t*)ray_data(bytes);
        int64_t n = bytes->len;
        int found = 0;
        for (int64_t i = 0; i + (int64_t)sizeof(dom) <= n; i++) {
            if (memcmp(hay + i, &dom, sizeof(dom)) == 0) { found = 1; break; }
        }
        TEST_ASSERT_FALSE(found);
    }

    ray_t* back = ray_de(bytes);
    TEST_ASSERT_FALSE(RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->type, RAY_SYM);
    TEST_ASSERT_EQ_I(back->len, 3);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(back), rt);
    TEST_ASSERT_EQ_PTR(back->sym_domain, rt);

    ray_release(back);
    ray_release(bytes);
    ray_release(v);
    PASS();
}

static test_result_t test_domain_col_save_load_mmap(void) {
    ray_sym_domain_t* rt = ray_sym_runtime_domain();
    unlink(TMP_DOM_COL_PATH);

    int64_t ida = ray_sym_intern("col_a", 5);
    int64_t idb = ray_sym_intern("col_b", 5);
    ray_t* v = ray_sym_vec_new(RAY_SYM_W16, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 4;
    uint16_t* d = (uint16_t*)ray_data(v);
    d[0] = (uint16_t)ida; d[1] = (uint16_t)idb;
    d[2] = (uint16_t)ida; d[3] = (uint16_t)idb;
    /* HAS_NULLS exercises the save branch that previously preserved aux
     * verbatim — the on-disk header must still be pointer-free. */
    v->attrs |= RAY_ATTR_HAS_NULLS;

    TEST_ASSERT_EQ_I(ray_col_save(v, TMP_DOM_COL_PATH), RAY_OK);

    /* Domain pointer never serialized: scan the file bytes. */
    {
        ray_sym_domain_t* dom = v->sym_domain;
        FILE* f = fopen(TMP_DOM_COL_PATH, "rb");
        TEST_ASSERT_NOT_NULL(f);
        uint8_t buf[4096];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        TEST_ASSERT(n >= 32, "column file too small");
        int found = 0;
        for (size_t i = 0; i + sizeof(dom) <= n; i++) {
            if (memcmp(buf + i, &dom, sizeof(dom)) == 0) { found = 1; break; }
        }
        TEST_ASSERT_FALSE(found);
    }

    ray_t* loaded = ray_col_load(TMP_DOM_COL_PATH);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_SYM);
    TEST_ASSERT_EQ_PTR(loaded->sym_domain, rt);
    ray_release(loaded);

    ray_t* mapped = ray_col_mmap(TMP_DOM_COL_PATH);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_I(mapped->type, RAY_SYM);
    TEST_ASSERT_EQ_PTR(mapped->sym_domain, rt);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(mapped), rt);
    ray_release(mapped);

    ray_release(v);
    unlink(TMP_DOM_COL_PATH);
    PASS();
}

/* ---- FILE domains over a real symfile ----------------------------------- */

static test_result_t test_domain_open_basic(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");

    int64_t id_h = ray_sym_intern("file_hello", 10);
    int64_t id_w = ray_sym_intern("file_world", 10);
    uint32_t count_at_save = ray_sym_count();
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);

    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT(dom != ray_sym_runtime_domain(), "FILE domain is not the singleton");

    /* count matches the persisted dictionary */
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), (int64_t)count_at_save);

    /* find: hit + miss */
    TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, "file_hello", 10), id_h);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, "file_world", 10), id_w);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, "file_absent", 11), -1);

    /* str round-trips: position resolves to the same bytes */
    ray_t* s = ray_sym_domain_str(dom, id_h);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQ_U(ray_str_len(s), 10);
    TEST_ASSERT_MEM_EQ(10, ray_str_ptr(s), "file_hello");
    /* borrowed: a second call returns the same atom */
    TEST_ASSERT_EQ_PTR(ray_sym_domain_str(dom, id_h), s);
    /* position 0 is the reserved empty string */
    ray_t* s0 = ray_sym_domain_str(dom, 0);
    TEST_ASSERT_NOT_NULL(s0);
    TEST_ASSERT_EQ_U(ray_str_len(s0), 0);
    /* out of range -> NULL */
    TEST_ASSERT_NULL(ray_sym_domain_str(dom, (int64_t)count_at_save));
    TEST_ASSERT_NULL(ray_sym_domain_str(dom, -1));

    /* path is the resolved file path */
    TEST_ASSERT_NOT_NULL(ray_sym_domain_path(dom));
    TEST_ASSERT(strstr(ray_sym_domain_path(dom), "rayforce_test_domain_sym") != NULL,
                "path mentions the symfile");

    /* The flip (Task 7b): FILE domains are writable — find-or-append
     * into the in-memory tail, persisted by flush (append-only:
     * existing positions never move). */
    int64_t new_pos = ray_sym_domain_intern(dom, "file_new", 8);
    TEST_ASSERT_EQ_I(new_pos, (int64_t)count_at_save);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "file_new", 8), new_pos);
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), (int64_t)count_at_save + 1);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, "file_new", 8), new_pos);
    ray_t* ns = ray_sym_domain_str(dom, new_pos);
    TEST_ASSERT_NOT_NULL(ns);
    TEST_ASSERT_EQ_U(ray_str_len(ns), 8);
    TEST_ASSERT_MEM_EQ(8, ray_str_ptr(ns), "file_new");
    /* append-only: prior positions and their borrowed atoms unchanged */
    TEST_ASSERT_EQ_PTR(ray_sym_domain_str(dom, id_h), s);

    /* flush persists base+tail; a fresh open sees the grown vocabulary */
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, true), RAY_OK); /* idempotent */
    ray_sym_domain_release(dom);
    ray_sym_domain_t* re = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(re);
    TEST_ASSERT_EQ_I(ray_sym_domain_count(re), (int64_t)count_at_save + 1);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(re, "file_new", 8), new_pos);
    ray_sym_domain_release(re);

    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

/* Task 7b: open_or_create on a missing file yields an empty writable
 * domain; "" is seeded at position 0 by the first intern; flush creates
 * the file; verify-base-unchanged makes a racing writer LOUD. */
static test_result_t test_domain_create_flush_concurrent(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");

    /* plain open of a missing file still fails */
    TEST_ASSERT_NULL(ray_sym_domain_open(TMP_DOM_SYM_PATH));

    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), 0);

    /* first intern of a real symbol seeds "" at position 0 */
    int64_t p = ray_sym_domain_intern(dom, "cre_a", 5);
    TEST_ASSERT_EQ_I(p, 1);
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), 2);
    ray_t* s0 = ray_sym_domain_str(dom, 0);
    TEST_ASSERT_NOT_NULL(s0);
    TEST_ASSERT_EQ_U(ray_str_len(s0), 0);

    /* flush creates the file with "" first */
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, false), RAY_OK);
    {
        FILE* f = fopen(TMP_DOM_SYM_PATH, "rb");
        TEST_ASSERT_NOT_NULL(f);
        uint32_t magic = 0, len0 = 1;
        int64_t cnt = -1;
        TEST_ASSERT_EQ_I(fread(&magic, 4, 1, f), 1);
        TEST_ASSERT_EQ_I(fread(&cnt, 8, 1, f), 1);
        TEST_ASSERT_EQ_I(fread(&len0, 4, 1, f), 1);
        fclose(f);
        TEST_ASSERT_EQ_U(magic, 0x4C525453u);
        TEST_ASSERT_EQ_I(cnt, 2);
        TEST_ASSERT_EQ_U(len0, 0);
    }

    /* concurrent-writer detection: another writer replaces the file
     * under us; the next flush of OUR dirty tail must fail loudly. */
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "cre_b", 5), 2);
    {
        FILE* f = fopen(TMP_DOM_SYM_PATH, "wb");
        TEST_ASSERT_NOT_NULL(f);
        uint32_t magic = 0x4C525453u;
        int64_t cnt = 1;
        uint32_t len0 = 0;
        fwrite(&magic, 4, 1, f);
        fwrite(&cnt, 8, 1, f);
        fwrite(&len0, 4, 1, f);
        fclose(f);
    }
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, false), RAY_ERR_CORRUPT);

    ray_sym_domain_release(dom);
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    unlink(TMP_DOM_SYM_PATH ".tmp");
    PASS();
}

/* Heap-recycle isolation for the process-global FILE-domain cache.
 *
 * A domain's atoms are ray_str objects on the buddy heap, but the cache
 * (g_domains) is process-global and outlives a per-test
 * ray_heap_destroy.  Without the ray_sym_domain_drop_heap() hook the
 * cache keeps an entry whose atoms were freed with their heap, so the
 * next open of that path on a fresh heap reuses the stale domain — a
 * use-after-free that surfaces as the next open/extend reading freed
 * atoms (NULL/garbage domain, or a SEGV once the pages are reused).
 *
 * This exercises that path directly (own heap lifecycle, hence NULL
 * setup/teardown): open a FILE domain, leak it into the cache, recycle
 * the heap, churn allocations to reuse the freed region, then re-open
 * the same path with a divergent file.  Note: the UAF does not fault
 * deterministically (freed pages often stay mapped), so this is not a
 * hard pre/post oracle — it is an ASan-backed guard that the recycle +
 * reopen stays clean and the drop hook keeps the cache coherent. */
#define TMP_DOM_RECYCLE_PATH "/tmp/rayforce_test_domain_recycle_sym"
static test_result_t test_domain_survives_heap_recycle(void) {
    unlink(TMP_DOM_RECYCLE_PATH);
    unlink(TMP_DOM_RECYCLE_PATH ".lk");

    /* --- heap A: open a domain with a SMALL vocabulary --- */
    ray_heap_init();
    (void)ray_sym_init();
    (void)ray_sym_intern("recyc_a", 7);
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_RECYCLE_PATH), RAY_OK);
    ray_sym_domain_t* d1 = ray_sym_domain_open(TMP_DOM_RECYCLE_PATH);
    TEST_ASSERT_NOT_NULL(d1);          /* atoms now live on heap A */
    /* Deliberately DO NOT release d1 — it stays in the global cache,
     * exactly the leak that exposed the dangling bug. */
    ray_sym_destroy();
    ray_heap_destroy();                /* must drop d1 from the cache */

    /* --- heap B (fresh): churn allocations to reuse heap A's freed
     * pages, then write a DIVERGENT (larger) file at the same path --- */
    ray_heap_init();
    (void)ray_sym_init();
    for (int i = 0; i < 256; i++) {    /* overwrite heap A's freed region */
        char b[32]; int n = snprintf(b, sizeof b, "churn_%d", i);
        ray_t* v = ray_str(b, (size_t)n);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
    }
    (void)ray_sym_intern("recyc_a", 7);
    (void)ray_sym_intern("recyc_b", 7);     /* different vocabulary/size */
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_RECYCLE_PATH), RAY_OK);
    /* Re-open the SAME path.  Pre-fix: cache hit on the stale d1; the
     * size differs so dom_extend dereferences d1's atoms (freed, now
     * overwritten by the churn above) → SEGV or garbage.  Post-fix: the
     * stale entry is gone, so this builds a fresh domain. */
    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_RECYCLE_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_t* s0 = ray_sym_domain_str(d2, 0);  /* touch the atoms */
    TEST_ASSERT_NOT_NULL(s0);
    TEST_ASSERT_EQ_U(ray_str_len(s0), 0);   /* position 0 is "" */
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_RECYCLE_PATH);
    unlink(TMP_DOM_RECYCLE_PATH ".lk");
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_domain_open_identity_and_release(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");

    (void)ray_sym_intern("ident_x", 7);
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);

    /* Same resolved path => same object (pointer equality = identity). */
    ray_sym_domain_t* d1 = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(d1);
    TEST_ASSERT_EQ_PTR(d1, d2);

    /* retain/release are real on FILE domains */
    ray_sym_domain_retain(d1);
    ray_sym_domain_release(d1);

    ray_sym_domain_release(d1);
    ray_sym_domain_release(d2);

    /* Last release dropped the cache entry: a fresh open must build a
     * fresh, fully functional mapping (and not crash under ASan). */
    ray_sym_domain_t* d3 = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(d3);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d3, "ident_x", 7),
                     ray_sym_find("ident_x", 7));
    ray_sym_domain_release(d3);

    /* Missing file -> NULL, no crash. */
    TEST_ASSERT_NULL(ray_sym_domain_open("/tmp/rayforce_test_domain_nonexistent"));

    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

/* ---- Phase 2 Task 1: resolution helpers + adopt-domain ------------------- */

/* ray_sym_vec_cell on a runtime-domain vec must be exactly equivalent to
 * the legacy two-step (ray_read_sym + ray_sym_str) — same borrowed atom. */
static test_result_t test_domain_vec_cell_runtime_equiv(void) {
    int64_t ida = ray_sym_intern("cell_a", 6);
    int64_t idb = ray_sym_intern("cell_b", 6);

    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 3;
    int64_t* d = (int64_t*)ray_data(v);
    d[0] = ida; d[1] = idb; d[2] = ida;

    for (int64_t i = 0; i < 3; i++) {
        int64_t id = ray_read_sym(ray_data(v), i, RAY_SYM, v->attrs);
        TEST_ASSERT_EQ_PTR(ray_sym_vec_cell(v, i), ray_sym_str(id));
    }

    /* narrow width takes the same path through ray_read_sym */
    ray_t* v8 = ray_sym_vec_new(RAY_SYM_W8, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v8));
    v8->len = 2;
    ((uint8_t*)ray_data(v8))[0] = (uint8_t)ida;
    ((uint8_t*)ray_data(v8))[1] = (uint8_t)idb;
    TEST_ASSERT_EQ_PTR(ray_sym_vec_cell(v8, 0), ray_sym_str(ida));
    TEST_ASSERT_EQ_PTR(ray_sym_vec_cell(v8, 1), ray_sym_str(idb));

    /* slices resolve data AND domain through the parent */
    ray_t* s = ray_vec_slice(v, 1, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_EQ_PTR(ray_sym_vec_cell(s, 0), ray_sym_str(idb));
    TEST_ASSERT_EQ_PTR(ray_sym_vec_cell(s, 1), ray_sym_str(ida));

    ray_release(s);
    ray_release(v8);
    ray_release(v);
    PASS();
}

/* ray_sym_vec_lookup on a runtime vec delegates to the global find:
 * hit returns the interned id, miss returns -1 (matches nothing). */
static test_result_t test_domain_vec_lookup_hit_miss(void) {
    int64_t ida = ray_sym_intern("lkp_hit", 7);

    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 1;
    ((int64_t*)ray_data(v))[0] = ida;

    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(v, "lkp_hit", 7), ida);
    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(v, "lkp_hit", 7),
                     ray_sym_find("lkp_hit", 7));
    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(v, "lkp_missing", 11), -1);

    /* slice: domain through the parent */
    ray_t* s = ray_vec_slice(v, 0, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s));
    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(s, "lkp_hit", 7), ida);
    ray_release(s);
    ray_release(v);
    PASS();
}

/* adopt_domain: ref balance with a real FILE domain (ASan is the leak
 * oracle), plus cell/lookup resolution through the adopted domain. */
static test_result_t test_domain_adopt_domain_refcount(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");

    (void)ray_sym_intern("adopt_x", 7);
    (void)ray_sym_intern("adopt_y", 7);
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);

    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);

    int64_t pos_x = ray_sym_domain_find(dom, "adopt_x", 7);
    int64_t pos_y = ray_sym_domain_find(dom, "adopt_y", 7);
    TEST_ASSERT(pos_x >= 0 && pos_y >= 0, "positions resolve");

    /* a: heap vec manually attached to the FILE domain (simulates the
     * Task-7 load path; the vec holds its own ref, dropped on free). */
    ray_t* a = ray_sym_vec_new(RAY_SYM_W64, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    a->len = 2;
    ((int64_t*)ray_data(a))[0] = pos_x;
    ((int64_t*)ray_data(a))[1] = pos_y;
    ray_sym_domain_retain(dom);
    a->sym_domain = dom;

    /* helpers resolve through the FILE domain, not the global table */
    ray_t* sx = ray_sym_vec_cell(a, 0);
    TEST_ASSERT_NOT_NULL(sx);
    TEST_ASSERT_EQ_U(ray_str_len(sx), 7);
    TEST_ASSERT_MEM_EQ(7, ray_str_ptr(sx), "adopt_x");
    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(a, "adopt_y", 7), pos_y);
    TEST_ASSERT_EQ_I(ray_sym_vec_lookup(a, "adopt_absent", 12), -1);

    /* b adopts a's domain: out's runtime ref is a no-op release, the FILE
     * domain is retained. */
    ray_t* b = ray_sym_vec_new(RAY_SYM_W64, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    ray_sym_vec_adopt_domain(b, a);
    TEST_ASSERT_EQ_PTR(b->sym_domain, dom);
    TEST_ASSERT_EQ_PTR(a->sym_domain, dom);

    /* idempotent: adopting the same domain again must not unbalance */
    ray_sym_vec_adopt_domain(b, a);
    TEST_ASSERT_EQ_PTR(b->sym_domain, dom);

    /* adopting a runtime vec's domain over a FILE ref releases the FILE ref */
    ray_t* r = ray_sym_vec_new(RAY_SYM_W64, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_sym_vec_adopt_domain(b, r);
    TEST_ASSERT_EQ_PTR(b->sym_domain, ray_sym_runtime_domain());

    /* non-SYM pairs are a no-op */
    ray_t* i64v = ray_vec_new(RAY_I64, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(i64v));
    ray_sym_vec_adopt_domain(i64v, a);
    TEST_ASSERT(i64v->sym_domain != dom, "non-SYM out untouched");
    ray_sym_vec_adopt_domain(b, i64v);
    TEST_ASSERT_EQ_PTR(b->sym_domain, ray_sym_runtime_domain());
    ray_release(i64v);

    /* re-adopt the FILE domain into b so b's FREE path (heap.c owned-ref
     * release) carries one of the outstanding refs. */
    ray_sym_vec_adopt_domain(b, a);
    TEST_ASSERT_EQ_PTR(b->sym_domain, dom);

    ray_release(r);
    ray_release(b);              /* drops b's adopt ref */
    ray_release(a);              /* drops a's manual attach ref */
    ray_sym_domain_release(dom); /* drops the open ref — last one */

    /* Balance proof: the cache entry must be gone; a fresh open builds a
     * fully functional mapping (ASan flags any leak/double-free). */
    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d2, "adopt_x", 7), pos_x);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

/* ---- Phase 2 Task 2: pivot output-vec adoption --------------------------- */

/* Pivot needs the full eval runtime, not just heap + sym table. */
static ray_runtime_t* pvt_rt;
static void domain_rt_setup(void)    { pvt_rt = ray_runtime_create(0, NULL); }
static void domain_rt_teardown(void) { ray_runtime_destroy(pvt_rt); }

/* A SYM index column carrying a FILE domain must hand that domain to the
 * pivot output's index column (the output raw-copies cell ids, so it must
 * resolve over the same dictionary).  Meaningful even pre-flip: col_vec_new
 * attaches the runtime singleton, and only the adopt call at the pivot
 * scatter site transfers the FILE-domain pointer. */
static test_result_t test_domain_pivot_index_adopts(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");

    int64_t id_A = ray_sym_intern("pd_A", 4);
    int64_t id_B = ray_sym_intern("pd_B", 4);
    int64_t id_x = ray_sym_intern("pd_x", 4);
    int64_t id_y = ray_sym_intern("pd_y", 4);
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);

    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);

    /* Index col 'a': manually attached to the FILE domain (positions
     * coincide with global ids — the symfile was saved just above). */
    ray_t* a = ray_sym_vec_new(RAY_SYM_W64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    a->len = 4;
    int64_t* ad = (int64_t*)ray_data(a);
    ad[0] = id_A; ad[1] = id_A; ad[2] = id_B; ad[3] = id_B;
    ray_sym_domain_retain(dom);
    a->sym_domain = dom;

    /* Pivot col 'c' and value col 'v' stay runtime-domain. */
    ray_t* cv = ray_sym_vec_new(RAY_SYM_W64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(cv));
    cv->len = 4;
    int64_t* cd = (int64_t*)ray_data(cv);
    cd[0] = id_x; cd[1] = id_y; cd[2] = id_x; cd[3] = id_y;
    ray_t* v = ray_vec_new(RAY_I64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 4;
    int64_t* vd = (int64_t*)ray_data(v);
    vd[0] = 1; vd[1] = 2; vd[2] = 3; vd[3] = 4;

    ray_t* t = ray_table_new(3);
    t = ray_table_add_col(t, ray_sym_intern("a", 1), a);
    t = ray_table_add_col(t, ray_sym_intern("c", 1), cv);
    t = ray_table_add_col(t, ray_sym_intern("v", 1), v);
    ray_release(a); ray_release(cv); ray_release(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t));

    int64_t tname = ray_sym_intern("pvtdom_t", 8);
    TEST_ASSERT_EQ_I(ray_env_set(tname, t), RAY_OK);

    ray_t* r = ray_eval_str("(pivot pvtdom_t ['a] 'c 'v sum)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    ray_t* out_a = ray_table_get_col(r, ray_sym_intern("a", 1));
    TEST_ASSERT_NOT_NULL(out_a);
    TEST_ASSERT_EQ_I(out_a->type, RAY_SYM);
    /* The adoption under test: the output index col resolves over the
     * SOURCE column's FILE domain, not the runtime singleton. */
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_a), dom);

    /* And its cells resolve through that domain to the right strings. */
    for (int64_t i = 0; i < 2; i++) {
        ray_t* s = ray_sym_vec_cell(out_a, i);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_EQ_U(ray_str_len(s), 4);
        TEST_ASSERT_TRUE(memcmp(ray_str_ptr(s), "pd_A", 4) == 0 ||
                         memcmp(ray_str_ptr(s), "pd_B", 4) == 0);
    }

    ray_release(r);

    /* Drop the env binding, our table ref, then the open ref: every
     * FILE-domain ref must be back before teardown (fresh open proves the
     * cache entry died; ASan flags any imbalance). */
    ray_release(ray_eval_str("(set pvtdom_t 0)"));
    ray_release(t);
    ray_sym_domain_release(dom);

    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

/* ---- Phase 2 Task 3: query.c resolution / re-expression ------------------ */

#define TMP_DOM_QSYM_PATH "/tmp/rayforce_test_domain_qsym"

/* Build a DIVERGENT file-domain fixture: a symfile in which the
 * positions of "dq_a"/"dq_b" are SWAPPED relative to the current
 * runtime's global ids.  Technique: intern a→b, save the whole global
 * table, then restart the runtime (fresh sym table, identical builtin
 * prefix) and intern b→a.  Now resolving a cell id through the global
 * table yields the WRONG symbol — only domain-aware code stays correct.
 *
 * On return: pvt_rt is a FRESH runtime, *out_dom is the opened FILE
 * domain (caller releases), pos_a / pos_b receive the FILE positions
 * of dq_a/dq_b.  Returns false if the fixture could not be built. */
static int build_divergent_qsym_fixture(ray_sym_domain_t** out_dom,
                                        int64_t* pos_a, int64_t* pos_b) {
    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");

    (void)ray_sym_intern("dq_a", 4);
    (void)ray_sym_intern("dq_b", 4);
    if (ray_sym_save(TMP_DOM_QSYM_PATH) != RAY_OK) return 0;

    /* Restart: fresh global table, REVERSED intern order. */
    ray_runtime_destroy(pvt_rt);
    pvt_rt = ray_runtime_create(0, NULL);
    if (!pvt_rt) return 0;
    (void)ray_sym_intern("dq_b", 4);
    (void)ray_sym_intern("dq_a", 4);

    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    if (!dom) return 0;
    *pos_a = ray_sym_domain_find(dom, "dq_a", 4);
    *pos_b = ray_sym_domain_find(dom, "dq_b", 4);
    *out_dom = dom;
    return *pos_a >= 0 && *pos_b >= 0;
}

/* Table {k: SYM FILE-domain cells [pos_a pos_b], v: I64 [v0 v1]} bound
 * to env name `tname`.  Returns the owned table (caller releases). */
static ray_t* build_qsym_table(ray_sym_domain_t* dom, int64_t pos_a,
                               int64_t pos_b, int64_t v0, int64_t v1,
                               const char* tname) {
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, 2);
    if (!k || RAY_IS_ERR(k)) return NULL;
    k->len = 2;
    ((int64_t*)ray_data(k))[0] = pos_a;
    ((int64_t*)ray_data(k))[1] = pos_b;
    ray_sym_domain_retain(dom);
    k->sym_domain = dom;

    ray_t* v = ray_vec_new(RAY_I64, 2);
    if (!v || RAY_IS_ERR(v)) { ray_release(k); return NULL; }
    v->len = 2;
    ((int64_t*)ray_data(v))[0] = v0;
    ((int64_t*)ray_data(v))[1] = v1;

    ray_t* t = ray_table_new(2);
    t = ray_table_add_col(t, ray_sym_intern("k", 1), k);
    t = ray_table_add_col(t, ray_sym_intern("v", 1), v);
    ray_release(k); ray_release(v);
    if (!t || RAY_IS_ERR(t)) return NULL;
    if (ray_env_set(ray_sym_intern(tname, strlen(tname)), t) != RAY_OK) {
        ray_release(t);
        return NULL;
    }
    return t;
}

static int sym_cell_is(ray_t* col, int64_t row, const char* want) {
    ray_t* s = ray_sym_vec_cell(col, row);
    size_t n = strlen(want);
    return s && ray_str_len(s) == n && memcmp(ray_str_ptr(s), want, n) == 0;
}

/* Upsert's literal-vs-column key match (query.c): the runtime literal
 * 'dq_a must be resolved into the KEY COLUMN'S domain before the raw
 * row scan, and the rebuilt SYM column must re-express the untouched
 * cells through that domain.  With the divergent fixture, the legacy
 * global-id compare matches the WRONG row (silent wrong-symbol bug). */
static test_result_t test_domain_query_upsert_key_lookup(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    /* discrimination precondition: file positions != runtime ids */
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    ray_t* t = build_qsym_table(dom, pos_a, pos_b, 10, 20, "dmq_t");
    TEST_ASSERT_NOT_NULL(t);

    ray_t* r = ray_eval_str("(upsert dmq_t 'k (list 'dq_a 99))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* match found in the file domain → UPDATE, not insert */
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    /* …and the row whose file-domain string is "dq_a" (row 0) got the
     * new value.  The legacy global compare matched row 1 instead. */
    ray_t* out_v = ray_table_get_col(r, ray_sym_intern("v", 1));
    TEST_ASSERT_NOT_NULL(out_v);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_v))[0], 99);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_v))[1], 20);

    /* The rebuilt SYM column is runtime-domain (it mixes the replacement
     * atom with re-expressed cells) and resolves to the right strings. */
    ray_t* out_k = ray_table_get_col(r, ray_sym_intern("k", 1));
    TEST_ASSERT_NOT_NULL(out_k);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_k), ray_sym_runtime_domain());
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 0, "dq_a"));
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 1, "dq_b"));

    ray_release(r);
    ray_release(ray_eval_str("(set dmq_t 0)"));
    ray_release(t);
    ray_sym_domain_release(dom);

    /* ref balance: the cache entry must be gone (fresh open rebuilds) */
    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* Insert's cell-to-output materialization (query.c): the rebuilt SYM
 * column re-expresses every copied cell through the SOURCE column's
 * FILE domain before mixing in the appended runtime atom.  The legacy
 * raw copy produced swapped strings under the divergent fixture. */
static test_result_t test_domain_query_insert_cell_reexpress(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    ray_t* t = build_qsym_table(dom, pos_a, pos_b, 1, 2, "dmi_t");
    TEST_ASSERT_NOT_NULL(t);

    ray_t* r = ray_eval_str("(insert dmi_t (list 'dq_a 3))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);

    ray_t* out_k = ray_table_get_col(r, ray_sym_intern("k", 1));
    TEST_ASSERT_NOT_NULL(out_k);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_k), ray_sym_runtime_domain());
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 0, "dq_a"));   /* was "dq_b" raw */
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 1, "dq_b"));   /* was "dq_a" raw */
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 2, "dq_a"));   /* appended atom  */

    ray_t* out_v = ray_table_get_col(r, ray_sym_intern("v", 1));
    TEST_ASSERT_NOT_NULL(out_v);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_v))[2], 3);

    ray_release(r);
    ray_release(ray_eval_str("(set dmi_t 0)"));
    ray_release(t);
    ray_sym_domain_release(dom);

    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* ---- Phase 2 Task 3 follow-up: gather + atom-materialization chokepoints - */

/* gather_by_idx (lang/eval.c) raw-copies SYM cell ids from ONE source vec
 * into a fresh output vec, so the output must adopt the source's domain.
 * Driven through the select-by per-group gather: `s: k` collects each
 * group's slice of the FILE-domain column via gather_by_idx.  Under the
 * divergent fixture the legacy runtime-domain output resolved to SWAPPED
 * strings.  Second assertion: collection_elem (lang/internal.h) — the
 * `(at col i)` atom materialization must re-express the cell id through
 * the column's domain into a runtime id. */
static test_result_t test_domain_gather_adopts_domain(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    /* discrimination precondition: file positions != runtime ids */
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    /* Table {k: SYM FILE-domain [a b a b], g: I64 [1 1 2 2]}. */
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(k));
    k->len = 4;
    int64_t* kd = (int64_t*)ray_data(k);
    kd[0] = pos_a; kd[1] = pos_b; kd[2] = pos_a; kd[3] = pos_b;
    ray_sym_domain_retain(dom);
    k->sym_domain = dom;

    ray_t* g = ray_vec_new(RAY_I64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(g));
    g->len = 4;
    int64_t* gd = (int64_t*)ray_data(g);
    gd[0] = 1; gd[1] = 1; gd[2] = 2; gd[3] = 2;

    ray_t* t = ray_table_new(2);
    t = ray_table_add_col(t, ray_sym_intern("k", 1), k);
    t = ray_table_add_col(t, ray_sym_intern("g", 1), g);
    ray_release(k); ray_release(g);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t));
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("dmg_t", 5), t), RAY_OK);

    /* Per-group gather: each group's cell is gather_by_idx(k, group_rows)
     * — a fresh SYM vec that must resolve over the FILE domain. */
    ray_t* r = ray_eval_str("(select {from: dmg_t by: g s: k})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    ray_t* out_s = ray_table_get_col(r, ray_sym_intern("s", 1));
    TEST_ASSERT_NOT_NULL(out_s);
    TEST_ASSERT_EQ_I(out_s->type, RAY_LIST);
    ray_t** cells = (ray_t**)ray_data(out_s);
    for (int64_t gi = 0; gi < 2; gi++) {
        ray_t* cell = cells[gi];
        TEST_ASSERT_NOT_NULL(cell);
        TEST_ASSERT_EQ_I(cell->type, RAY_SYM);
        TEST_ASSERT_EQ_I(cell->len, 2);
        /* The adoption under test: the gathered vec resolves over the
         * SOURCE column's FILE domain, not the runtime singleton. */
        TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(cell), dom);
        TEST_ASSERT_TRUE(sym_cell_is(cell, 0, "dq_a"));  /* was "dq_b" raw */
        TEST_ASSERT_TRUE(sym_cell_is(cell, 1, "dq_b"));  /* was "dq_a" raw */
    }
    ray_release(r);

    /* collection_elem atom rule: (at k 1) reads cell id pos_b and must
     * return the RUNTIME atom for "dq_b" (raw copy yielded "dq_a"). */
    ray_t* a = ray_eval_str("(at (at dmg_t 'k) 1)");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    TEST_ASSERT_EQ_I(a->type, -RAY_SYM);
    TEST_ASSERT_EQ_I(a->i64, ray_sym_intern("dq_b", 4));
    ray_release(a);

    ray_release(ray_eval_str("(set dmg_t 0)"));
    ray_release(t);
    ray_sym_domain_release(dom);

    /* ref balance: the cache entry must be gone (fresh open rebuilds) */
    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* ---- Phase 2 Task 4: group/agg sweep -------------------------------------- */

/* Group-by over FILE-domain SYM columns (ops/group.c): the key output
 * copies raw cell ids from the source column and must adopt its domain
 * (query.c's KEY_READ raw compares depend on this invariant); FIRST/MIN
 * SYM aggregates emit raw cell ids of the value column and must both
 * resolve lex MIN through the COLUMN's domain and adopt it on the
 * output.  Built on the divergent fixture, every partial conversion
 * fails: no adoption ⇒ swapped strings; adoption without the lex-domain
 * fix ⇒ MIN picks the wrong cell; legacy ⇒ runtime-domain outputs. */
static test_result_t test_domain_group_by_adopts_domain(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    /* discrimination precondition: file positions != runtime ids */
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    /* Table {k: SYM FILE [a a b b], v: SYM FILE [b a a b], n: I64 [1 2 3 4]}.
     * Group dq_a: v = {dq_b, dq_a} → first dq_b, min dq_a, sum 3.
     * Group dq_b: v = {dq_a, dq_b} → first dq_a, min dq_a, sum 7. */
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(k));
    k->len = 4;
    int64_t* kd = (int64_t*)ray_data(k);
    kd[0] = pos_a; kd[1] = pos_a; kd[2] = pos_b; kd[3] = pos_b;
    ray_sym_domain_retain(dom);
    k->sym_domain = dom;

    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 4;
    int64_t* vd = (int64_t*)ray_data(v);
    vd[0] = pos_b; vd[1] = pos_a; vd[2] = pos_a; vd[3] = pos_b;
    ray_sym_domain_retain(dom);
    v->sym_domain = dom;

    ray_t* nv = ray_vec_new(RAY_I64, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(nv));
    nv->len = 4;
    int64_t* nd = (int64_t*)ray_data(nv);
    nd[0] = 1; nd[1] = 2; nd[2] = 3; nd[3] = 4;

    ray_t* t = ray_table_new(3);
    t = ray_table_add_col(t, ray_sym_intern("k", 1), k);
    t = ray_table_add_col(t, ray_sym_intern("v", 1), v);
    t = ray_table_add_col(t, ray_sym_intern("n", 1), nv);
    ray_release(k); ray_release(v); ray_release(nv);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t));
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("dmgb_t", 6), t), RAY_OK);

    ray_t* r = ray_eval_str(
        "(select {from: dmgb_t by: k f: (first v) mn: (min v) s: (sum n)})");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    /* (a) the key output adopts the source column's FILE domain and
     *     resolves to the right strings */
    ray_t* out_k = ray_table_get_col(r, ray_sym_intern("k", 1));
    TEST_ASSERT_NOT_NULL(out_k);
    TEST_ASSERT_EQ_I(out_k->type, RAY_SYM);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_k), dom);

    /* locate each group's row by the key's domain-resolved string */
    int64_t row_a = sym_cell_is(out_k, 0, "dq_a") ? 0
                  : sym_cell_is(out_k, 1, "dq_a") ? 1 : -1;
    TEST_ASSERT(row_a >= 0, "group dq_a present");
    int64_t row_b = 1 - row_a;
    TEST_ASSERT_TRUE(sym_cell_is(out_k, row_b, "dq_b"));

    /* (b) FIRST on a SYM column: raw cell ids of v — adopted + resolved */
    ray_t* out_f = ray_table_get_col(r, ray_sym_intern("f", 1));
    TEST_ASSERT_NOT_NULL(out_f);
    TEST_ASSERT_EQ_I(out_f->type, RAY_SYM);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_f), dom);
    TEST_ASSERT_TRUE(sym_cell_is(out_f, row_a, "dq_b"));  /* swapped raw */
    TEST_ASSERT_TRUE(sym_cell_is(out_f, row_b, "dq_a"));

    /* (c) MIN on a SYM column: lex order comes from the COLUMN's domain
     *     ("dq_a" < "dq_b" by FILE strings) and the output adopts it.
     *     Adoption without the lex-domain fix resolves "dq_b" here. */
    ray_t* out_mn = ray_table_get_col(r, ray_sym_intern("mn", 2));
    TEST_ASSERT_NOT_NULL(out_mn);
    TEST_ASSERT_EQ_I(out_mn->type, RAY_SYM);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_mn), dom);
    TEST_ASSERT_TRUE(sym_cell_is(out_mn, row_a, "dq_a"));
    TEST_ASSERT_TRUE(sym_cell_is(out_mn, row_b, "dq_a"));

    /* numeric agg sanity (group identity not scrambled by the sweep) */
    ray_t* out_s = ray_table_get_col(r, ray_sym_intern("s", 1));
    TEST_ASSERT_NOT_NULL(out_s);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_s))[row_a], 3);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_s))[row_b], 7);
    ray_release(r);

    /* Scalar (no-by) reduction: LAST over the FILE-domain column must
     * surface "dq_b" whichever emit path runs (atom re-expression or
     * column adoption).  Legacy raw handling surfaced "dq_a". */
    ray_t* r2 = ray_eval_str("(select {from: dmgb_t l: (last v)})");
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(ray_table_nrows(r2), 1);
    ray_t* out_l = ray_table_get_col(r2, ray_sym_intern("l", 1));
    TEST_ASSERT_NOT_NULL(out_l);
    TEST_ASSERT_EQ_I(out_l->type, RAY_SYM);
    TEST_ASSERT_TRUE(sym_cell_is(out_l, 0, "dq_b"));
    ray_release(r2);

    ray_release(ray_eval_str("(set dmgb_t 0)"));
    ray_release(t);
    ray_sym_domain_release(dom);

    /* ref balance: the cache entry must be gone (fresh open rebuilds) */
    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* ---- Phase 2 Task 5: kernel sweep ----------------------------------------- */

/* The R7 SYM eq/ne fast path (lang/eval.c): `(== col 'lit)` compares the
 * literal atom's id RAW against the column's cell ids in tight per-width
 * loops.  Over a FILE-domain column the literal must first be re-expressed
 * as a position in the COLUMN's domain; a literal absent from the domain
 * matches nothing (== all-false, != all-true).  With the divergent fixture
 * the legacy raw runtime-id compare selects the WRONG rows. */
static test_result_t test_domain_r7_eq_fast_path(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    /* discrimination precondition: file positions != runtime ids */
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = 3;
    int64_t* vd = (int64_t*)ray_data(v);
    vd[0] = pos_a; vd[1] = pos_a; vd[2] = pos_b;
    ray_sym_domain_retain(dom);
    v->sym_domain = dom;
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("dmr7v", 5), v), RAY_OK);

    /* present literal, == : exactly the dq_a cells */
    ray_t* r = ray_eval_str("(== dmr7v 'dq_a)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(r->len, 3);
    {
        const bool* b = (const bool*)ray_data(r);
        TEST_ASSERT_TRUE(b[0]);
        TEST_ASSERT_TRUE(b[1]);
        TEST_ASSERT_FALSE(b[2]);
    }
    ray_release(r);

    /* present literal, != : complement */
    r = ray_eval_str("(!= dmr7v 'dq_b)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->len, 3);
    {
        const bool* b = (const bool*)ray_data(r);
        TEST_ASSERT_TRUE(b[0]);
        TEST_ASSERT_TRUE(b[1]);
        TEST_ASSERT_FALSE(b[2]);
    }
    ray_release(r);

    /* absent literal ('dq_zz interns into the RUNTIME at parse but is
     * not in the FILE domain): == matches nothing, != matches all */
    r = ray_eval_str("(== dmr7v 'dq_zz)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->len, 3);
    {
        const bool* b = (const bool*)ray_data(r);
        TEST_ASSERT_FALSE(b[0]);
        TEST_ASSERT_FALSE(b[1]);
        TEST_ASSERT_FALSE(b[2]);
    }
    ray_release(r);

    r = ray_eval_str("(!= dmr7v 'dq_zz)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->len, 3);
    {
        const bool* b = (const bool*)ray_data(r);
        TEST_ASSERT_TRUE(b[0]);
        TEST_ASSERT_TRUE(b[1]);
        TEST_ASSERT_TRUE(b[2]);
    }
    ray_release(r);

    ray_release(ray_eval_str("(set dmr7v 0)"));
    ray_release(v);
    ray_sym_domain_release(dom);

    /* ref balance: the cache entry must be gone (fresh open rebuilds) */
    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* Sort over a FILE-domain SYM key (ops/sort.c): both the lex rank LUT
 * (build_enum_rank) and the multi-key comparator materialize strings,
 * which must come from the COLUMN's domain.  With the divergent fixture
 * the legacy global resolution ranks the two symbols in the WRONG order
 * (file cells resolve to the swapped global strings), leaving the rows
 * unsorted; the converted path reorders them lexicographically.  Also
 * proves the sorted output adopts the key column's domain, and that
 * `(upper k)` (sym_elem in ops/internal.h) materializes through it. */
static test_result_t test_domain_sort_and_upper_resolve_domain(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    /* k = ["dq_b" "dq_a"] by FILE strings; v = [1 2] tracks the rows */
    ray_t* t = build_qsym_table(dom, pos_b, pos_a, 1, 2, "dmsrt_t");
    TEST_ASSERT_NOT_NULL(t);

    ray_t* r = ray_eval_str("(xasc dmsrt_t 'k)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    ray_t* out_k = ray_table_get_col(r, ray_sym_intern("k", 1));
    TEST_ASSERT_NOT_NULL(out_k);
    TEST_ASSERT_EQ_I(out_k->type, RAY_SYM);
    /* lex order by the FILE domain's strings */
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 0, "dq_a"));
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 1, "dq_b"));
    /* sorted key output resolves over the key column's dictionary */
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_k), dom);

    /* the payload moved with its key row */
    ray_t* out_v = ray_table_get_col(r, ray_sym_intern("v", 1));
    TEST_ASSERT_NOT_NULL(out_v);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_v))[0], 2);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_v))[1], 1);
    ray_release(r);

    /* (upper k): exec_string_unary resolves each cell via sym_elem —
     * the strings must come from the FILE domain (legacy: swapped). */
    ray_t* r2 = ray_eval_str("(select {r: (upper k) from: dmsrt_t})");
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(ray_table_nrows(r2), 2);
    ray_t* out_r = ray_table_get_col(r2, ray_sym_intern("r", 1));
    TEST_ASSERT_NOT_NULL(out_r);
    TEST_ASSERT_EQ_I(out_r->type, RAY_SYM);
    TEST_ASSERT_TRUE(sym_cell_is(out_r, 0, "DQ_B"));  /* row order of t */
    TEST_ASSERT_TRUE(sym_cell_is(out_r, 1, "DQ_A"));
    ray_release(r2);

    ray_release(ray_eval_str("(set dmsrt_t 0)"));
    ray_release(t);
    ray_sym_domain_release(dom);

    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* ---- Phase 2 Task 6: serde encode + join matching across domains --------- */

/* serde (store/serde.c): the wire format carries SYM cells as STRINGS, so
 * the encoder must resolve each cell through THE VEC's domain; the decoder
 * interns into the global table (runtime-domain output).  With the
 * divergent fixture the legacy global-resolution encoder wrote SWAPPED
 * strings onto the wire — the round-trip must preserve the FILE domain's
 * strings, re-expressed as runtime-domain output ids. */
static test_result_t test_domain_serde_sym_file_domain(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    ray_t* t = build_qsym_table(dom, pos_a, pos_b, 1, 2, "dser_t");
    TEST_ASSERT_NOT_NULL(t);

    ray_t* bytes = ray_ser(t);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_FALSE(RAY_IS_ERR(bytes));

    ray_t* back = ray_de(bytes);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_FALSE(RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(back), 2);

    ray_t* out_k = ray_table_get_col(back, ray_sym_intern("k", 1));
    TEST_ASSERT_NOT_NULL(out_k);
    TEST_ASSERT_EQ_I(out_k->type, RAY_SYM);
    /* decode interns globally: the output is runtime-domain... */
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_k), ray_sym_runtime_domain());
    /* ...and carries the SAME STRINGS the file domain held — proving the
     * encoder resolved through the vec's domain, not the global table. */
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 0, "dq_a"));
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 1, "dq_b"));

    ray_t* out_v = ray_table_get_col(back, ray_sym_intern("v", 1));
    TEST_ASSERT_NOT_NULL(out_v);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_v))[0], 1);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_v))[1], 2);

    ray_release(back);
    ray_release(bytes);
    ray_release(ray_eval_str("(set dser_t 0)"));
    ray_release(t);
    ray_sym_domain_release(dom);

    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* Hash join key MATCHING (ops/join.c): cells of the two key columns are
 * compared across the tables, and raw ids only compare within ONE domain.
 * hash_row_keys canonicalizes SYM cells to runtime ids and join_keys_eq
 * gates the raw compare on domain identity.  Under the divergent fixture
 * the runtime ids of dq_a/dq_b are exactly the SWAPPED file positions, so
 * the legacy raw compare paired every left row with the WRONG right row
 * (silent wrong-symbol bug); domain-aware matching pairs by string.  Also
 * pins the joined left SYM output adopting its source's FILE domain. */
static test_result_t test_domain_join_across_domains(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    /* Left: FILE-domain k ["dq_a" "dq_b"], v [1 2]. */
    ray_t* lt = build_qsym_table(dom, pos_a, pos_b, 1, 2, "djl_t");
    TEST_ASSERT_NOT_NULL(lt);

    /* Right: runtime-domain k ['dq_a 'dq_b], w [10 20]. */
    ray_t* rt = ray_eval_str(
        "(set djr_t (table [k w] (list [dq_a dq_b] [10 20])))");
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rt));

    ray_t* r = ray_eval_str("(inner-join [k] djl_t djr_t)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    ray_t* out_k = ray_table_get_col(r, ray_sym_intern("k", 1));
    ray_t* out_v = ray_table_get_col(r, ray_sym_intern("v", 1));
    ray_t* out_w = ray_table_get_col(r, ray_sym_intern("w", 1));
    TEST_ASSERT_NOT_NULL(out_k);
    TEST_ASSERT_NOT_NULL(out_v);
    TEST_ASSERT_NOT_NULL(out_w);
    TEST_ASSERT_EQ_I(out_k->type, RAY_SYM);
    /* left gather output resolves over the left key column's dictionary */
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out_k), dom);

    /* match by STRING, order-independent: dq_a↔(v 1, w 10), dq_b↔(2, 20).
     * The legacy raw compare produced (1, 20) / (2, 10). */
    for (int64_t i = 0; i < 2; i++) {
        int64_t v = ((int64_t*)ray_data(out_v))[i];
        int64_t w = ((int64_t*)ray_data(out_w))[i];
        if (sym_cell_is(out_k, i, "dq_a")) {
            TEST_ASSERT_EQ_I(v, 1);
            TEST_ASSERT_EQ_I(w, 10);
        } else {
            TEST_ASSERT_TRUE(sym_cell_is(out_k, i, "dq_b"));
            TEST_ASSERT_EQ_I(v, 2);
            TEST_ASSERT_EQ_I(w, 20);
        }
    }

    ray_release(r);
    ray_release(rt);
    ray_release(ray_eval_str("(set djl_t 0)"));
    ray_release(ray_eval_str("(set djr_t 0)"));
    ray_release(lt);
    ray_sym_domain_release(dom);

    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* asof sort-merge eq-key matching (exec_window_join): when a SYM eq-key
 * pair spans two domains, the left side sorts and merges in the RIGHT
 * side's id space (asof_eq_lread).  Divergent fixture: the legacy raw
 * compare carried each left row to the WRONG right partition. */
static test_result_t test_domain_asof_join_across_domains(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    /* Left: FILE-domain k ["dq_a" "dq_b"], t TIME [2 2], p [1 2].
     * build_qsym_table gives {k v}; build the time column by hand. */
    ray_t* lt = build_qsym_table(dom, pos_a, pos_b, 1, 2, "dajl_t");
    TEST_ASSERT_NOT_NULL(lt);
    {
        ray_t* tv = ray_vec_new(RAY_TIME, 2);
        TEST_ASSERT_FALSE(RAY_IS_ERR(tv));
        tv->len = 2;
        ((int32_t*)ray_data(tv))[0] = 2;
        ((int32_t*)ray_data(tv))[1] = 2;
        ray_t* lt2 = ray_table_add_col(lt, ray_sym_intern("t", 1), tv);
        ray_release(tv);
        TEST_ASSERT_NOT_NULL(lt2);
        TEST_ASSERT_FALSE(RAY_IS_ERR(lt2));
        lt = lt2;
        TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("dajl_t", 6), lt), RAY_OK);
    }

    /* Right: runtime-domain k ['dq_a 'dq_b], t TIME [1 1], bid [10 20]. */
    ray_t* rt = ray_eval_str(
        "(set dajr_t (table [k t bid] "
        "(list [dq_a dq_b] [00:00:00.001 00:00:00.001] [10 20])))");
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rt));

    ray_t* r = ray_eval_str("(asof-join [k t] dajl_t dajr_t)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    ray_t* out_k = ray_table_get_col(r, ray_sym_intern("k", 1));
    ray_t* out_b = ray_table_get_col(r, ray_sym_intern("bid", 3));
    TEST_ASSERT_NOT_NULL(out_k);
    TEST_ASSERT_NOT_NULL(out_b);
    /* output preserves left row order: ["dq_a" "dq_b"] by FILE strings */
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 0, "dq_a"));
    TEST_ASSERT_TRUE(sym_cell_is(out_k, 1, "dq_b"));
    /* …each carrying the right row matched BY STRING (legacy: swapped) */
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_b))[0], 10);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_b))[1], 20);

    ray_release(r);
    ray_release(rt);
    ray_release(ray_eval_str("(set dajl_t 0)"));
    ray_release(ray_eval_str("(set dajr_t 0)"));
    ray_release(lt);
    ray_sym_domain_release(dom);

    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* Parted-flatten / filter-compact materializers (ops/exec.c OP_SCAN cold
 * path + flatten_parted_col, ops/query.c query_materialize_parted_col,
 * ops/filter.c sel_compact): every one of them memcpy's SYM cell ids
 * verbatim out of the partition segments, so the flat output must adopt
 * the partitions' shared FILE domain.  Divergent fixture: without the
 * adopt, the raw ids resolve through the (swapped) runtime table and the
 * output names come back WRONG. */
static test_result_t test_domain_parted_flatten_adopts(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    /* PARTED SYM column k: 2 segments over the FILE domain.
     *   seg0 = ["dq_a" "dq_b"], seg1 = ["dq_b"]  (3 rows total) */
    ray_t* seg0 = ray_sym_vec_new(RAY_SYM_W64, 2);
    ray_t* seg1 = ray_sym_vec_new(RAY_SYM_W64, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(seg0));
    TEST_ASSERT_FALSE(RAY_IS_ERR(seg1));
    seg0->len = 2; seg1->len = 1;
    ((int64_t*)ray_data(seg0))[0] = pos_a;
    ((int64_t*)ray_data(seg0))[1] = pos_b;
    ((int64_t*)ray_data(seg1))[0] = pos_b;
    ray_sym_domain_retain(dom); seg0->sym_domain = dom;
    ray_sym_domain_retain(dom); seg1->sym_domain = dom;

    ray_t* k = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(k);
    k->type = (int8_t)(RAY_PARTED_BASE + RAY_SYM);
    k->len = 2;
    k->attrs = 0;
    memset(k->aux, 0, 16);
    ((ray_t**)ray_data(k))[0] = seg0;   /* ownership moves to the carrier */
    ((ray_t**)ray_data(k))[1] = seg1;

    /* PARTED I64 column v: segments [1 2] and [3]. */
    ray_t* vs0 = ray_vec_new(RAY_I64, 2);
    ray_t* vs1 = ray_vec_new(RAY_I64, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vs0));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vs1));
    vs0->len = 2; vs1->len = 1;
    ((int64_t*)ray_data(vs0))[0] = 1;
    ((int64_t*)ray_data(vs0))[1] = 2;
    ((int64_t*)ray_data(vs1))[0] = 3;
    ray_t* v = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(v);
    v->type = (int8_t)(RAY_PARTED_BASE + RAY_I64);
    v->len = 2;
    v->attrs = 0;
    memset(v->aux, 0, 16);
    ((ray_t**)ray_data(v))[0] = vs0;
    ((ray_t**)ray_data(v))[1] = vs1;

    ray_t* t = ray_table_new(2);
    t = ray_table_add_col(t, ray_sym_intern("k", 1), k);
    t = ray_table_add_col(t, ray_sym_intern("v", 1), v);
    ray_release(k); ray_release(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t));
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("dpf_t", 5), t), RAY_OK);

    /* (a) Projection over the parted column: the scan-side flatten
     * memcpy's segment cells into one flat vec.  All 3 rows pass. */
    ray_t* r1 = ray_eval_str("(select {k: k v: v from: dpf_t})");
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 3);
    ray_t* k1 = ray_table_get_col(r1, ray_sym_intern("k", 1));
    TEST_ASSERT_NOT_NULL(k1);
    TEST_ASSERT_EQ_I(k1->type, RAY_SYM);
    TEST_ASSERT_TRUE(sym_cell_is(k1, 0, "dq_a"));
    TEST_ASSERT_TRUE(sym_cell_is(k1, 1, "dq_b"));
    TEST_ASSERT_TRUE(sym_cell_is(k1, 2, "dq_b"));
    ray_release(r1);

    /* (b) Partial WHERE over the parted table: the filter installs a
     * rowsel and the boundary materializer (sel_compact / the parted
     * gather) compacts the SYM column row-by-row. */
    ray_t* r2 = ray_eval_str("(select {k: k v: v from: dpf_t where: (> v 1)})");
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(ray_table_nrows(r2), 2);
    ray_t* k2 = ray_table_get_col(r2, ray_sym_intern("k", 1));
    ray_t* v2 = ray_table_get_col(r2, ray_sym_intern("v", 1));
    TEST_ASSERT_NOT_NULL(k2);
    TEST_ASSERT_NOT_NULL(v2);
    TEST_ASSERT_EQ_I(k2->type, RAY_SYM);
    TEST_ASSERT_TRUE(sym_cell_is(k2, 0, "dq_b"));
    TEST_ASSERT_TRUE(sym_cell_is(k2, 1, "dq_b"));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(v2))[0], 2);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(v2))[1], 3);
    ray_release(r2);

    ray_release(ray_eval_str("(set dpf_t 0)"));
    ray_release(t);
    ray_sym_domain_release(dom);

    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* ---- Phase 2 Task 7a: flip preconditions ---------------------------------- */

/* Eager atom materialization: every position of a freshly opened FILE
 * domain resolves IMMEDIATELY (no lazy build dependency on find/first
 * touch), the borrowed atoms are pointer-stable across calls, and the
 * resolved bytes match the vocabulary — i.e. behavior is unchanged from
 * the lazy per-position cache, minus the per-call lock. */
static test_result_t test_domain_str_eager_lockfree(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");

    int64_t id_a = ray_sym_intern("eager_a", 7);
    int64_t id_b = ray_sym_intern("eager_b", 7);
    uint32_t count_at_save = ray_sym_count();
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);

    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);

    /* First touch after open: walk EVERY position before any find/LUT
     * call — all atoms must already be there and match the global
     * table's strings (positions coincide with ids in this fixture). */
    ray_t* first[2] = { NULL, NULL };
    for (int64_t p = 0; p < (int64_t)count_at_save; p++) {
        ray_t* s = ray_sym_domain_str(dom, p);
        TEST_ASSERT_NOT_NULL(s);
        ray_t* g = ray_sym_str(p);
        TEST_ASSERT_EQ_U(ray_str_len(s), ray_str_len(g));
        TEST_ASSERT_MEM_EQ(ray_str_len(s), ray_str_ptr(s), ray_str_ptr(g));
        if (p == id_a) first[0] = s;
        if (p == id_b) first[1] = s;
    }
    /* Pointer-stable borrowed atoms: identical on re-resolution. */
    TEST_ASSERT_EQ_PTR(ray_sym_domain_str(dom, id_a), first[0]);
    TEST_ASSERT_EQ_PTR(ray_sym_domain_str(dom, id_b), first[1]);
    /* Out of range still NULL. */
    TEST_ASSERT_NULL(ray_sym_domain_str(dom, (int64_t)count_at_save));
    TEST_ASSERT_NULL(ray_sym_domain_str(dom, -1));

    ray_sym_domain_release(dom);
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

/* Raw vocabulary access: a pinned FILE domain hands out each file entry's
 * bytes and length straight from the mapping, equal to the materialised
 * atom; the runtime singleton refuses; pins nest and release. */
static test_result_t test_domain_raw_pin(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    int64_t id_a = ray_sym_intern("raw_alpha", 9);
    int64_t id_b = ray_sym_intern("raw_beta_longer_entry", 21);
    (void)id_a; (void)id_b;
    uint32_t count_at_save = ray_sym_count();
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);
    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    ray_sym_domain_raw_t raw;
    TEST_ASSERT_FALSE(ray_sym_domain_raw_pin(ray_sym_runtime_domain(), &raw));
    TEST_ASSERT(ray_sym_domain_raw_pin(dom, &raw), "pin a file domain");
    TEST_ASSERT_EQ_I(raw.count, (int64_t)count_at_save);
    /* Nested pin from another reader is fine. */
    ray_sym_domain_raw_t raw2;
    TEST_ASSERT(ray_sym_domain_raw_pin(dom, &raw2), "second pin");
    for (int64_t p = 0; p < raw.count; p++) {
        size_t rl = 0;
        const char* rp = ray_sym_domain_raw_str(&raw, p, &rl);
        ray_t* s = ray_sym_domain_str(dom, p);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_EQ_U(rl, ray_str_len(s));
        TEST_ASSERT_MEM_EQ(rl, rp, ray_str_ptr(s));
    }
    ray_sym_domain_raw_unpin(dom);
    ray_sym_domain_raw_unpin(dom);
    /* An entry appended after open is past the file prefix: not raw. */
    int64_t pos_new = ray_sym_domain_intern(dom, "raw_appended", 12);
    TEST_ASSERT(pos_new >= raw.count, "appended entry sits past the mapped prefix");
    TEST_ASSERT(ray_sym_domain_raw_pin(dom, &raw), "pin again");
    TEST_ASSERT_EQ_I(raw.count, (int64_t)count_at_save);
    ray_sym_domain_raw_unpin(dom);
    ray_sym_domain_release(dom);
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}
/* Runtime-id LUT: NULL for the runtime singleton; for a FILE domain a
 * position-indexed array of runtime intern ids (round-trips strings),
 * built once (idempotent: same pointer).  Divergent fixture proves the
 * entries are real translations, not identity copies. */
static test_result_t test_domain_runtime_lut(void) {
    /* runtime singleton: ids are already runtime ids — no LUT */
    TEST_ASSERT_NULL(ray_sym_domain_runtime_lut(ray_sym_runtime_domain()));

    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    const int64_t* lut = ray_sym_domain_runtime_lut(dom);
    TEST_ASSERT_NOT_NULL(lut);

    /* Idempotence: built once, second request returns the same array. */
    TEST_ASSERT_EQ_PTR((const void*)ray_sym_domain_runtime_lut(dom),
                       (const void*)lut);

    /* Round-trip: every position's runtime id resolves to the same bytes
     * as the domain's own vocabulary entry. */
    int64_t cnt = ray_sym_domain_count(dom);
    TEST_ASSERT(cnt > 0, "non-empty vocabulary");
    for (int64_t p = 0; p < cnt; p++) {
        TEST_ASSERT(lut[p] >= 0, "translated id valid");
        ray_t* ds = ray_sym_domain_str(dom, p);
        ray_t* rs = ray_sym_str(lut[p]);
        TEST_ASSERT_NOT_NULL(ds);
        TEST_ASSERT_NOT_NULL(rs);
        TEST_ASSERT_EQ_U(ray_str_len(ds), ray_str_len(rs));
        TEST_ASSERT_MEM_EQ(ray_str_len(ds), ray_str_ptr(ds), ray_str_ptr(rs));
    }

    /* Real translation under divergence + the position-0 "" anchor. */
    TEST_ASSERT_EQ_I(lut[pos_a], ray_sym_intern("dq_a", 4));
    TEST_ASSERT_EQ_I(lut[pos_b], ray_sym_intern("dq_b", 4));
    TEST_ASSERT(lut[pos_a] != pos_a, "LUT translates, not identity");
    TEST_ASSERT_EQ_I(lut[0], 0);  /* "" reserves position 0 AND global id 0 */

    ray_sym_domain_release(dom);
    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

#define TMP_DOM_BADSYM_PATH "/tmp/rayforce_test_domain_badsym"

/* Position-0 reservation: ray_sym_save-produced files carry "" at
 * position 0 (global id 0) and open fine; a crafted symfile whose first
 * entry is NOT "" is refused (NULL); an EMPTY vocabulary is fine. */
static test_result_t test_domain_open_position0_validation(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    unlink(TMP_DOM_BADSYM_PATH);

    /* (a) the save path today: "" at position 0 — opens. */
    (void)ray_sym_intern("p0_ok", 5);
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);
    ray_sym_domain_t* good = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(good);
    ray_t* s0 = ray_sym_domain_str(good, 0);
    TEST_ASSERT_NOT_NULL(s0);
    TEST_ASSERT_EQ_U(ray_str_len(s0), 0);
    ray_sym_domain_release(good);

    /* (b) crafted STRL whose position 0 is "x": refused. */
    {
        FILE* f = fopen(TMP_DOM_BADSYM_PATH, "wb");
        TEST_ASSERT_NOT_NULL(f);
        uint32_t magic = 0x4C525453U; /* "STRL" */
        int64_t  count = 2;
        uint32_t len_x = 1, len_e = 0;
        fwrite(&magic, 4, 1, f);
        fwrite(&count, 8, 1, f);
        fwrite(&len_x, 4, 1, f);
        fwrite("x", 1, 1, f);
        fwrite(&len_e, 4, 1, f);
        fclose(f);
    }
    TEST_ASSERT_NULL(ray_sym_domain_open(TMP_DOM_BADSYM_PATH));

    /* (c) empty vocabulary (count 0): structurally fine. */
    {
        FILE* f = fopen(TMP_DOM_BADSYM_PATH, "wb");
        TEST_ASSERT_NOT_NULL(f);
        uint32_t magic = 0x4C525453U;
        int64_t  count = 0;
        fwrite(&magic, 4, 1, f);
        fwrite(&count, 8, 1, f);
        fclose(f);
    }
    ray_sym_domain_t* empty = ray_sym_domain_open(TMP_DOM_BADSYM_PATH);
    TEST_ASSERT_NOT_NULL(empty);
    TEST_ASSERT_EQ_I(ray_sym_domain_count(empty), 0);
    TEST_ASSERT_NULL(ray_sym_domain_str(empty, 0));
    /* empty FILE domain still yields a non-NULL LUT marker (callers
     * branch on NULL == runtime domain) that is never indexable */
    TEST_ASSERT_NOT_NULL(ray_sym_domain_runtime_lut(empty));
    ray_sym_domain_release(empty);

    unlink(TMP_DOM_BADSYM_PATH);
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

/* dict upsert mutation boundary: a dict whose SYM keys carry an injected
 * FILE domain re-expresses the keys to the runtime domain on upsert —
 * the existing-key probe matches by STRING (under the divergent fixture
 * the raw id compare hits the WRONG row), and the appended key is a
 * runtime id in a runtime-domain keys vec. */
static test_result_t test_domain_dict_upsert_file_keys(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    TEST_ASSERT(pos_a != ray_sym_intern("dq_a", 4), "fixture diverges");

    /* keys: FILE-domain [dq_a dq_b]; vals: (10 20) */
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(k));
    k->len = 2;
    ((int64_t*)ray_data(k))[0] = pos_a;
    ((int64_t*)ray_data(k))[1] = pos_b;
    ray_sym_domain_retain(dom);
    k->sym_domain = dom;

    ray_t* vals = ray_list_new(2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vals));
    ray_t* v10 = ray_i64(10);
    ray_t* v20 = ray_i64(20);
    vals = ray_list_append(vals, v10);
    vals = ray_list_append(vals, v20);
    ray_release(v10);
    ray_release(v20);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vals));

    ray_t* d = ray_dict_new(k, vals);  /* consumes both */
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));

    /* (a) upsert an EXISTING key: 'dq_a must UPDATE row 0 (string match
     * through the FILE domain; the raw runtime-id compare matched the
     * dq_b row under divergence). */
    {
        ray_t* ka = ray_sym(ray_sym_intern("dq_a", 4));
        ray_t* nv = ray_i64(99);
        d = ray_dict_upsert(d, ka, nv);
        ray_release(ka);
        ray_release(nv);
        TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    }
    ray_t* keys = ray_dict_keys(d);
    ray_t* outv = ray_dict_vals(d);
    TEST_ASSERT_NOT_NULL(keys);
    TEST_ASSERT_NOT_NULL(outv);
    TEST_ASSERT_EQ_I(keys->len, 2);  /* update, not insert */
    /* keys were re-expressed to the runtime domain */
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(keys), ray_sym_runtime_domain());
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(keys))[0], ray_sym_intern("dq_a", 4));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(keys))[1], ray_sym_intern("dq_b", 4));
    TEST_ASSERT_TRUE(sym_cell_is(keys, 0, "dq_a"));
    TEST_ASSERT_TRUE(sym_cell_is(keys, 1, "dq_b"));
    TEST_ASSERT_EQ_I(((ray_t**)ray_data(outv))[0]->i64, 99);
    TEST_ASSERT_EQ_I(((ray_t**)ray_data(outv))[1]->i64, 20);

    /* (b) upsert a NEW key: appended as a runtime id. */
    {
        ray_t* kc = ray_sym(ray_sym_intern("dq_c", 4));
        ray_t* nv = ray_i64(7);
        d = ray_dict_upsert(d, kc, nv);
        ray_release(kc);
        ray_release(nv);
        TEST_ASSERT_FALSE(RAY_IS_ERR(d));
    }
    keys = ray_dict_keys(d);
    outv = ray_dict_vals(d);
    TEST_ASSERT_EQ_I(keys->len, 3);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(keys), ray_sym_runtime_domain());
    TEST_ASSERT_TRUE(sym_cell_is(keys, 0, "dq_a"));
    TEST_ASSERT_TRUE(sym_cell_is(keys, 1, "dq_b"));
    TEST_ASSERT_TRUE(sym_cell_is(keys, 2, "dq_c"));
    TEST_ASSERT_EQ_I(((ray_t**)ray_data(outv))[2]->i64, 7);

    ray_release(d);
    ray_sym_domain_release(dom);

    /* ref balance: the dict's converted keys dropped the FILE-domain ref
     * (fresh open rebuilds the cache entry). */
    ray_sym_domain_t* d2 = ray_sym_domain_open(TMP_DOM_QSYM_PATH);
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(TMP_DOM_QSYM_PATH);
    unlink(TMP_DOM_QSYM_PATH ".lk");
    PASS();
}

/* ---- registration -------------------------------------------------------- */

const test_entry_t domain_entries[] = {
    { "domain/runtime_identity",        test_domain_runtime_identity,        domain_setup, domain_teardown },
    { "domain/runtime_delegation",      test_domain_runtime_delegation,      domain_setup, domain_teardown },
    { "domain/vec_new_attach",          test_domain_vec_new_attach,          domain_setup, domain_teardown },
    { "domain/append_growth_cow",       test_domain_append_growth_and_cow,   domain_setup, domain_teardown },
    { "domain/slice_accessor",          test_domain_slice_accessor,          domain_setup, domain_teardown },
    { "domain/concat_attach",           test_domain_concat_attach,           domain_setup, domain_teardown },
    { "domain/serde_roundtrip",         test_domain_serde_roundtrip,         domain_setup, domain_teardown },
    { "domain/col_save_load_mmap",      test_domain_col_save_load_mmap,      domain_setup, domain_teardown },
    { "domain/open_basic",              test_domain_open_basic,              domain_setup, domain_teardown },
    { "domain/create_flush_concurrent", test_domain_create_flush_concurrent, domain_setup, domain_teardown },
    { "domain/survives_heap_recycle",   test_domain_survives_heap_recycle,   NULL, NULL },
    { "domain/open_identity_release",   test_domain_open_identity_and_release, domain_setup, domain_teardown },
    { "domain/vec_cell_runtime_equiv",  test_domain_vec_cell_runtime_equiv,  domain_setup, domain_teardown },
    { "domain/vec_lookup_hit_miss",     test_domain_vec_lookup_hit_miss,     domain_setup, domain_teardown },
    { "domain/adopt_domain_refcount",   test_domain_adopt_domain_refcount,   domain_setup, domain_teardown },
    { "domain/pivot_index_adopts",      test_domain_pivot_index_adopts,      domain_rt_setup, domain_rt_teardown },
    { "domain/query_upsert_key_lookup", test_domain_query_upsert_key_lookup, domain_rt_setup, domain_rt_teardown },
    { "domain/query_insert_reexpress",  test_domain_query_insert_cell_reexpress, domain_rt_setup, domain_rt_teardown },
    { "domain/gather_adopts_domain",    test_domain_gather_adopts_domain,    domain_rt_setup, domain_rt_teardown },
    { "domain/group_by_adopts_domain",  test_domain_group_by_adopts_domain,  domain_rt_setup, domain_rt_teardown },
    { "domain/r7_eq_fast_path",         test_domain_r7_eq_fast_path,         domain_rt_setup, domain_rt_teardown },
    { "domain/sort_upper_resolve",      test_domain_sort_and_upper_resolve_domain, domain_rt_setup, domain_rt_teardown },
    { "domain/serde_sym_file_domain",   test_domain_serde_sym_file_domain,   domain_rt_setup, domain_rt_teardown },
    { "domain/join_across_domains",     test_domain_join_across_domains,     domain_rt_setup, domain_rt_teardown },
    { "domain/asof_join_across_domains", test_domain_asof_join_across_domains, domain_rt_setup, domain_rt_teardown },
    { "domain/parted_flatten_adopts",   test_domain_parted_flatten_adopts,   domain_rt_setup, domain_rt_teardown },
    { "domain/str_eager_lockfree",      test_domain_str_eager_lockfree,      domain_setup, domain_teardown },
    { "domain/raw_pin",                 test_domain_raw_pin,                 domain_setup, domain_teardown },
    { "domain/runtime_lut",             test_domain_runtime_lut,             domain_rt_setup, domain_rt_teardown },
    { "domain/open_position0_validation", test_domain_open_position0_validation, domain_setup, domain_teardown },
    { "domain/dict_upsert_file_keys",   test_domain_dict_upsert_file_keys,   domain_rt_setup, domain_rt_teardown },
    { NULL, NULL, NULL, NULL },
};
