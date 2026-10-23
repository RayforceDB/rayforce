#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  /* setenv / unsetenv: the import dictionary's trace flag */
#endif
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
#include "ipc_harness.h"   /* an IPC reply of corrupt symbols */
#include "app/repl.h"      /* ray_repl_run_file: the REPL display */
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "table/domain.h"
#include "table/symimp.h"   /* ray_symimp_stats_t: the import dictionary's counts */
#include "store/col.h"
#include "store/serde.h"
#include "core/pool.h"   /* the batch intern probes on the pool */
#include "core/platform.h"   /* ray_thread_create: lookups against a reservation */
#include "core/profile.h"    /* ray_profile_now_ns: their bounded wait */
#include "ops/hash.h"    /* ray_hash_bytes: batch intern takes prehashed entries */
#include "core/crc32.h"   /* the record index file's checksum */
#include "mem/sys.h"
#include "ops/ops.h"   /* RAY_PARTED_BASE (parted-flatten adoption test) */
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>      /* utimensat: a modification time set back */
#include <sys/stat.h>   /* stat: the symfile size behind the mapping counters */
#include <stdlib.h>   /* setenv */
#include <stdatomic.h>
#include <time.h>       /* nanosleep: a later modification time */

#define TMP_DOM_SYM_PATH "/tmp/rayforce_test_domain_sym"
#define TMP_DOM_COL_PATH "/tmp/rayforce_test_domain_col"

/* ---- Setup / Teardown -------------------------------------------------- */

static void domain_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

/* What an index test leaves when it fails early (its own cleanup is at its
 * end): the knob it set and its files, so the next test starts clean. */
static void domain_ix_cleanup(void) {
    unsetenv("RAY_SYM_INDEX_MIN");
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    unlink(TMP_DOM_SYM_PATH ".ix");
}

static void domain_teardown(void) {
    domain_ix_cleanup();
    ray_clear_interrupt();
    ray_sym_domain_batch_cancel_for_test(0);   /* disarm a hook a test left armed */
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
    unlink(ray_test_tmp(TMP_DOM_COL_PATH));

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

    TEST_ASSERT_EQ_I(ray_col_save(v, ray_test_tmp(TMP_DOM_COL_PATH)), RAY_OK);

    /* Domain pointer never serialized: scan the file bytes. */
    {
        ray_sym_domain_t* dom = v->sym_domain;
        FILE* f = fopen(ray_test_tmp(TMP_DOM_COL_PATH), "rb");
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

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_DOM_COL_PATH));
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_SYM);
    TEST_ASSERT_EQ_PTR(loaded->sym_domain, rt);
    ray_release(loaded);

    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_DOM_COL_PATH));
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_I(mapped->type, RAY_SYM);
    TEST_ASSERT_EQ_PTR(mapped->sym_domain, rt);
    TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(mapped), rt);
    ray_release(mapped);

    ray_release(v);
    unlink(ray_test_tmp(TMP_DOM_COL_PATH));
    PASS();
}

/* ---- FILE domains over a real symfile ----------------------------------- */

static test_result_t test_domain_open_basic(void) {
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));

    int64_t id_h = ray_sym_intern("file_hello", 10);
    int64_t id_w = ray_sym_intern("file_world", 10);
    uint32_t count_at_save = ray_sym_count();
    TEST_ASSERT_EQ_I(ray_sym_save(ray_test_tmp(TMP_DOM_SYM_PATH)), RAY_OK);

    ray_sym_domain_t* dom = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
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
    ray_sym_domain_t* re = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
    TEST_ASSERT_NOT_NULL(re);
    TEST_ASSERT_EQ_I(ray_sym_domain_count(re), (int64_t)count_at_save + 1);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(re, "file_new", 8), new_pos);
    ray_sym_domain_release(re);

    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    PASS();
}

/* ray_sym_domain_intern_batch: one batch with repeats spread over the hash
 * partitions, half the vocabulary already interned one by one.  Every
 * entry resolves to the position find() reports, repeats agree, the
 * pre-interned half keeps its positions, the count grows by exactly the
 * new distinct strings, a second identical batch changes nothing and ""
 * is position 0. */
static test_result_t test_domain_intern_batch(void) {
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    TEST_ASSERT_NOT_NULL(ray_pool_get());   /* parallel probe path */

    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(ray_test_tmp(TMP_DOM_SYM_PATH));
    TEST_ASSERT_NOT_NULL(dom);

    enum { NV = 20000, NB = 60000, SL = 16 };
    char*        vocab  = (char*)ray_sys_alloc((size_t)NV * SL);
    int64_t*     pre    = (int64_t*)ray_sys_alloc((size_t)NV * sizeof(int64_t));
    int64_t*     seen   = (int64_t*)ray_sys_alloc((size_t)NV * sizeof(int64_t));
    const char** strs   = (const char**)ray_sys_alloc((size_t)NB * sizeof(char*));
    size_t*      lens   = (size_t*)ray_sys_alloc((size_t)NB * sizeof(size_t));
    uint32_t*    hashes = (uint32_t*)ray_sys_alloc((size_t)NB * sizeof(uint32_t));
    int64_t*     pos    = (int64_t*)ray_sys_alloc((size_t)NB * sizeof(int64_t));
    int64_t*     pos2   = (int64_t*)ray_sys_alloc((size_t)NB * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(vocab); TEST_ASSERT_NOT_NULL(pre); TEST_ASSERT_NOT_NULL(seen);
    TEST_ASSERT_NOT_NULL(strs); TEST_ASSERT_NOT_NULL(lens); TEST_ASSERT_NOT_NULL(hashes);
    TEST_ASSERT_NOT_NULL(pos); TEST_ASSERT_NOT_NULL(pos2);

    for (int v = 0; v < NV; v++) {
        snprintf(vocab + (size_t)v * SL, SL, "v%05d_%c", v, 'a' + v % 26);
        seen[v] = -1;
        pre[v] = -1;
    }
    for (int v = 0; v < NV / 2; v++) {
        const char* sv = vocab + (size_t)v * SL;
        pre[v] = ray_sym_domain_intern(dom, sv, strlen(sv));
        TEST_ASSERT(pre[v] > 0, "pre-intern gets a position");
    }
    int64_t count_before = ray_sym_domain_count(dom);
    TEST_ASSERT_EQ_I(count_before, NV / 2 + 1);   /* + reserved "" */

    for (int j = 0; j < NB; j++) {
        int v = (int)(((int64_t)j * 7919) % NV);   /* every string ~3 times */
        const char* sv = vocab + (size_t)v * SL;
        strs[j] = sv;
        lens[j] = strlen(sv);
        hashes[j] = (uint32_t)ray_hash_bytes(sv, lens[j]);
        pos[j] = -7;
    }
    TEST_ASSERT_TRUE(ray_sym_domain_intern_batch(dom, NB, strs, lens, hashes, pos));
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), NV + 1);

    for (int j = 0; j < NB; j++) {
        int v = (int)(((int64_t)j * 7919) % NV);
        TEST_ASSERT(pos[j] > 0 && pos[j] <= NV, "position in range, never 0");
        if (seen[v] < 0) seen[v] = pos[j];
        TEST_ASSERT_EQ_I(pos[j], seen[v]);                        /* repeats agree */
        if (pre[v] >= 0) TEST_ASSERT_EQ_I(pos[j], pre[v]);        /* hits keep their position */
        TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, strs[j], lens[j]), pos[j]);
        ray_t* a = ray_sym_domain_str(dom, pos[j]);
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_EQ_U(ray_str_len(a), lens[j]);
        TEST_ASSERT_MEM_EQ(lens[j], ray_str_ptr(a), strs[j]);
    }
    /* distinct positions: every vocabulary entry got exactly one */
    for (int v = 0; v < NV; v++) TEST_ASSERT(seen[v] > 0, "every string appeared");

    /* the same batch again: all hits, nothing appended */
    for (int j = 0; j < NB; j++) pos2[j] = -7;
    TEST_ASSERT_TRUE(ray_sym_domain_intern_batch(dom, NB, strs, lens, hashes, pos2));
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), NV + 1);
    for (int j = 0; j < NB; j++) TEST_ASSERT_EQ_I(pos2[j], pos[j]);

    /* "" resolves to the reserved position 0; a fresh string still appends */
    {
        const char* two[2] = { "", "brand_new_entry" };
        size_t tl[2] = { 0, strlen("brand_new_entry") };
        uint32_t th[2] = { (uint32_t)ray_hash_bytes("", 0), (uint32_t)ray_hash_bytes(two[1], tl[1]) };
        int64_t tp[2] = { -7, -7 };
        TEST_ASSERT_TRUE(ray_sym_domain_intern_batch(dom, 2, two, tl, th, tp));
        TEST_ASSERT_EQ_I(tp[0], 0);
        TEST_ASSERT_EQ_I(tp[1], NV + 1);
        TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), NV + 2);
    }

    /* the flushed file reopens with the same vocabulary */
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, false), RAY_OK);
    ray_sym_domain_release(dom);
    ray_sym_domain_t* re = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
    TEST_ASSERT_NOT_NULL(re);
    TEST_ASSERT_EQ_I(ray_sym_domain_count(re), NV + 2);
    for (int j = 0; j < NB; j += 997)
        TEST_ASSERT_EQ_I(ray_sym_domain_find(re, strs[j], lens[j]), pos[j]);
    ray_sym_domain_release(re);

    ray_sys_free(vocab); ray_sys_free(pre); ray_sys_free(seen);
    ray_sys_free(strs); ray_sys_free(lens); ray_sys_free(hashes);
    ray_sys_free(pos); ray_sys_free(pos2);
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    PASS();
}

/* A cancelled pool dispatch can skip tasks while still returning normally.
 * The batch intern must not classify misses or commit dedupe output from
 * arrays those skipped tasks never filled. */
static test_result_t test_domain_intern_batch_cancel(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "seed", 4), 1);
    int64_t before = ray_sym_domain_count(dom);

    enum { N = 8192, SL = 24 };
    char* storage = ray_sys_alloc((size_t)N * SL);
    const char** strs = ray_sys_alloc((size_t)N * sizeof(*strs));
    size_t* lens = ray_sys_alloc((size_t)N * sizeof(*lens));
    uint32_t* hashes = ray_sys_alloc((size_t)N * sizeof(*hashes));
    int64_t* out = ray_sys_alloc((size_t)N * sizeof(*out));
    TEST_ASSERT_NOT_NULL(storage); TEST_ASSERT_NOT_NULL(strs);
    TEST_ASSERT_NOT_NULL(lens); TEST_ASSERT_NOT_NULL(hashes); TEST_ASSERT_NOT_NULL(out);
    for (int64_t i = 0; i < N; i++) {
        snprintf(storage + (size_t)i * SL, SL, "cancel-%07lld", (long long)i);
        strs[i] = storage + (size_t)i * SL;
        lens[i] = strlen(strs[i]);
        hashes[i] = (uint32_t)ray_hash_bytes(strs[i], lens[i]);
    }

    /* out is zero-filled: a skipped probe task then leaves "hits" at
     * position 0 behind, which the pre-fix code took as a complete probe
     * with no misses and returned true with those ids. */
    const int phases[] = {RAY_SYM_DOMAIN_CANCEL_PROBE, RAY_SYM_DOMAIN_CANCEL_DEDUPE};
    for (size_t p = 0; p < sizeof(phases) / sizeof(phases[0]); p++) {
        memset(out, 0, (size_t)N * sizeof(*out));
        ray_sym_domain_batch_cancel_for_test(phases[p]);
        TEST_ASSERT_FALSE(ray_sym_domain_intern_batch(dom, N, strs, lens, hashes, out));
        TEST_ASSERT_TRUE(ray_interrupted());
        ray_clear_interrupt();
        TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), before);
        for (int64_t i = 0; i < N; i += 257)
            TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, strs[i], lens[i]), -1);
    }

    /* A cancel that lands once the commit has begun is too late to stop
     * it: the parallel insert skips tasks, the caller completes them, and
     * the batch is published whole. */
    ray_sym_domain_batch_cancel_for_test(RAY_SYM_DOMAIN_CANCEL_INSERT);
    TEST_ASSERT_TRUE(ray_sym_domain_intern_batch(dom, N, strs, lens, hashes, out));
    TEST_ASSERT_TRUE(ray_interrupted());
    ray_clear_interrupt();
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), before + N);
    for (int64_t i = 0; i < N; i++)
        TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, strs[i], lens[i]), out[i]);
    for (int64_t i = 0; i < N; i += 257)
        TEST_ASSERT_EQ_I(out[i], before + i);

    /* And a second batch of new strings, uncancelled, lands after them. */
    for (int64_t i = 0; i < N; i++) {
        snprintf(storage + (size_t)i * SL, SL, "second-%07lld", (long long)i);
        lens[i] = strlen(strs[i]);
        hashes[i] = (uint32_t)ray_hash_bytes(strs[i], lens[i]);
    }
    TEST_ASSERT_TRUE(ray_sym_domain_intern_batch(dom, N, strs, lens, hashes, out));
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), before + 2 * N);
    for (int64_t i = 0; i < N; i += 257)
        TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, strs[i], lens[i]), out[i]);

    ray_sym_domain_release(dom);
    ray_sys_free(storage); ray_sys_free(strs); ray_sys_free(lens);
    ray_sys_free(hashes); ray_sys_free(out);
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

enum { DOM_CONCURRENT_TASKS = 32, DOM_CONCURRENT_BATCH = 8192,
       DOM_CONCURRENT_VOCAB = 32768, DOM_CONCURRENT_STRING = 32 };
typedef struct {
    ray_sym_domain_t* domain;
    char* vocabulary;
    int64_t* positions;
    bool ok[DOM_CONCURRENT_TASKS];
} domain_concurrent_batch_t;

static void domain_concurrent_batch_task(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    domain_concurrent_batch_t* ctx = raw;
    const char* strings[DOM_CONCURRENT_BATCH];
    size_t lengths[DOM_CONCURRENT_BATCH];
    uint32_t hashes[DOM_CONCURRENT_BATCH];
    for (int64_t task = start; task < end; task++) {
        for (int i = 0; i < DOM_CONCURRENT_BATCH; i++) {
            int v = (task*1024 + i%4096)%DOM_CONCURRENT_VOCAB;
            strings[i] = ctx->vocabulary + v*DOM_CONCURRENT_STRING;
            lengths[i] = strlen(strings[i]);
            hashes[i] = (uint32_t)ray_hash_bytes(strings[i], lengths[i]);
        }
        ctx->ok[task] = ray_sym_domain_intern_batch(ctx->domain, DOM_CONCURRENT_BATCH,
            strings, lengths, hashes, ctx->positions + task*DOM_CONCURRENT_BATCH);
    }
}

/* Overlapping batches grow the shared index while other workers probe it.
 * Every task has repeats, and adjacent tasks share most of their vocabulary.
 * Check all returned positions and the persisted domain, including a second
 * all-hit pass after the concurrent growth. */
static test_result_t test_domain_intern_batch_concurrent(void) {
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH)); unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    domain_concurrent_batch_t ctx = {0};
    ctx.domain = ray_sym_domain_open_or_create(ray_test_tmp(TMP_DOM_SYM_PATH));
    ctx.vocabulary = ray_sys_alloc(DOM_CONCURRENT_VOCAB*DOM_CONCURRENT_STRING);
    ctx.positions = ray_sys_alloc(DOM_CONCURRENT_TASKS*DOM_CONCURRENT_BATCH*sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(ctx.domain); TEST_ASSERT_NOT_NULL(ctx.vocabulary); TEST_ASSERT_NOT_NULL(ctx.positions);
    for (int v = 0; v < DOM_CONCURRENT_VOCAB; v++)
        snprintf(ctx.vocabulary + v*DOM_CONCURRENT_STRING, DOM_CONCURRENT_STRING,
                 "parallel-%05d-%08x", v, (unsigned)v*7919u);
    for (int pass = 0; pass < 2; pass++) {
        ray_pool_dispatch_n(ray_pool_get(), domain_concurrent_batch_task, &ctx, DOM_CONCURRENT_TASKS);
        TEST_ASSERT_EQ_I(ray_sym_domain_count(ctx.domain), DOM_CONCURRENT_VOCAB+1);
        for (int task = 0; task < DOM_CONCURRENT_TASKS; task++) {
            TEST_ASSERT_TRUE(ctx.ok[task]);
            for (int i = 0; i < DOM_CONCURRENT_BATCH; i++) {
                int v = (task*1024 + i%4096)%DOM_CONCURRENT_VOCAB;
                const char* expected = ctx.vocabulary + v*DOM_CONCURRENT_STRING;
                int64_t pos = ctx.positions[task*DOM_CONCURRENT_BATCH+i];
                TEST_ASSERT(pos > 0 && pos <= DOM_CONCURRENT_VOCAB, "concurrent position in range");
                ray_t* actual = ray_sym_domain_str(ctx.domain, pos);
                TEST_ASSERT_NOT_NULL(actual);
                TEST_ASSERT_EQ_U(ray_str_len(actual), strlen(expected));
                TEST_ASSERT_MEM_EQ(strlen(expected), ray_str_ptr(actual), expected);
                TEST_ASSERT_EQ_I(ray_sym_domain_find(ctx.domain, expected, strlen(expected)), pos);
            }
        }
        TEST_ASSERT_EQ_I(ray_sym_domain_flush(ctx.domain, false), RAY_OK);
        ray_sym_domain_release(ctx.domain);
        ctx.domain = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
        TEST_ASSERT_NOT_NULL(ctx.domain);
    }
    ray_sym_domain_release(ctx.domain);
    ray_sys_free(ctx.vocabulary); ray_sys_free(ctx.positions);
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH)); unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    PASS();
}

/* Task 7b: open_or_create on a missing file yields an empty writable
 * domain; "" is seeded at position 0 by the first intern; flush creates
 * the file; verify-base-unchanged makes a racing writer LOUD. */
static test_result_t test_domain_create_flush_concurrent(void) {
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));

    /* plain open of a missing file still fails */
    TEST_ASSERT_NULL(ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH)));

    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(ray_test_tmp(TMP_DOM_SYM_PATH));
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
        FILE* f = fopen(ray_test_tmp(TMP_DOM_SYM_PATH), "rb");
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
        FILE* f = fopen(ray_test_tmp(TMP_DOM_SYM_PATH), "wb");
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
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".tmp"));
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
    unlink(ray_test_tmp(TMP_DOM_RECYCLE_PATH));
    unlink(ray_test_tmp(TMP_DOM_RECYCLE_PATH ".lk"));

    /* --- heap A: open a domain with a SMALL vocabulary --- */
    ray_heap_init();
    (void)ray_sym_init();
    (void)ray_sym_intern("recyc_a", 7);
    TEST_ASSERT_EQ_I(ray_sym_save(ray_test_tmp(TMP_DOM_RECYCLE_PATH)), RAY_OK);
    ray_sym_domain_t* d1 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_RECYCLE_PATH));
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
    TEST_ASSERT_EQ_I(ray_sym_save(ray_test_tmp(TMP_DOM_RECYCLE_PATH)), RAY_OK);
    /* Re-open the SAME path.  Pre-fix: cache hit on the stale d1; the
     * size differs so dom_extend dereferences d1's atoms (freed, now
     * overwritten by the churn above) → SEGV or garbage.  Post-fix: the
     * stale entry is gone, so this builds a fresh domain. */
    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_RECYCLE_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_t* s0 = ray_sym_domain_str(d2, 0);  /* touch the atoms */
    TEST_ASSERT_NOT_NULL(s0);
    TEST_ASSERT_EQ_U(ray_str_len(s0), 0);   /* position 0 is "" */
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_RECYCLE_PATH));
    unlink(ray_test_tmp(TMP_DOM_RECYCLE_PATH ".lk"));
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_domain_open_identity_and_release(void) {
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));

    (void)ray_sym_intern("ident_x", 7);
    TEST_ASSERT_EQ_I(ray_sym_save(ray_test_tmp(TMP_DOM_SYM_PATH)), RAY_OK);

    /* Same resolved path => same object (pointer equality = identity). */
    ray_sym_domain_t* d1 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
    TEST_ASSERT_NOT_NULL(d1);
    TEST_ASSERT_EQ_PTR(d1, d2);

    /* retain/release are real on FILE domains */
    ray_sym_domain_retain(d1);
    ray_sym_domain_release(d1);

    ray_sym_domain_release(d1);
    ray_sym_domain_release(d2);

    /* Last release dropped the cache entry: a fresh open must build a
     * fresh, fully functional mapping (and not crash under ASan). */
    ray_sym_domain_t* d3 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
    TEST_ASSERT_NOT_NULL(d3);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d3, "ident_x", 7),
                     ray_sym_find("ident_x", 7));
    ray_sym_domain_release(d3);

    /* Missing file -> NULL, no crash. */
    TEST_ASSERT_NULL(ray_sym_domain_open(ray_test_tmp("/tmp/rayforce_test_domain_nonexistent")));

    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
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
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));

    (void)ray_sym_intern("adopt_x", 7);
    (void)ray_sym_intern("adopt_y", 7);
    TEST_ASSERT_EQ_I(ray_sym_save(ray_test_tmp(TMP_DOM_SYM_PATH)), RAY_OK);

    ray_sym_domain_t* dom = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
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
    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(d2, "adopt_x", 7), pos_x);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    PASS();
}

/* ---- Phase 2 Task 2: pivot output-vec adoption --------------------------- */

/* Pivot needs the full eval runtime, not just heap + sym table. */
static ray_runtime_t* pvt_rt;
static void domain_rt_setup(void)    { pvt_rt = ray_runtime_create(0, NULL); }
static void domain_rt_teardown(void) { domain_ix_cleanup(); ray_runtime_destroy(pvt_rt); }

/* A SYM index column carrying a FILE domain must hand that domain to the
 * pivot output's index column (the output raw-copies cell ids, so it must
 * resolve over the same dictionary).  Meaningful even pre-flip: col_vec_new
 * attaches the runtime singleton, and only the adopt call at the pivot
 * scatter site transfers the FILE-domain pointer. */
static test_result_t test_domain_pivot_index_adopts(void) {
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));

    int64_t id_A = ray_sym_intern("pd_A", 4);
    int64_t id_B = ray_sym_intern("pd_B", 4);
    int64_t id_x = ray_sym_intern("pd_x", 4);
    int64_t id_y = ray_sym_intern("pd_y", 4);
    TEST_ASSERT_EQ_I(ray_sym_save(ray_test_tmp(TMP_DOM_SYM_PATH)), RAY_OK);

    ray_sym_domain_t* dom = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
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

    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
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
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));

    (void)ray_sym_intern("dq_a", 4);
    (void)ray_sym_intern("dq_b", 4);
    if (ray_sym_save(ray_test_tmp(TMP_DOM_QSYM_PATH)) != RAY_OK) return 0;

    /* Restart: fresh global table, REVERSED intern order. */
    ray_runtime_destroy(pvt_rt);
    pvt_rt = ray_runtime_create(0, NULL);
    if (!pvt_rt) return 0;
    (void)ray_sym_intern("dq_b", 4);
    (void)ray_sym_intern("dq_a", 4);

    ray_sym_domain_t* dom = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
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
    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
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

    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
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
    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
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
    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
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
    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
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

    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
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

    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
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

    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
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

    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
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

    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
    PASS();
}

/* ---- Phase 2 Task 7a: flip preconditions ---------------------------------- */

/* Eager atom materialization: every position of a freshly opened FILE
 * domain resolves IMMEDIATELY (no lazy build dependency on find/first
 * touch), the borrowed atoms are pointer-stable across calls, and the
 * resolved bytes match the vocabulary — i.e. behavior is unchanged from
 * the lazy per-position cache, minus the per-call lock. */
static test_result_t test_domain_str_eager_lockfree(void) {
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));

    int64_t id_a = ray_sym_intern("eager_a", 7);
    int64_t id_b = ray_sym_intern("eager_b", 7);
    uint32_t count_at_save = ray_sym_count();
    TEST_ASSERT_EQ_I(ray_sym_save(ray_test_tmp(TMP_DOM_SYM_PATH)), RAY_OK);

    ray_sym_domain_t* dom = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
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
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    PASS();
}

/* Raw vocabulary access: a pinned FILE domain hands out each file entry's
 * bytes and length straight from the mapping, equal to the materialised
 * atom; the runtime singleton refuses; pins nest and release. */
static test_result_t test_domain_raw_pin(void) {
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    int64_t id_a = ray_sym_intern("raw_alpha", 9);
    int64_t id_b = ray_sym_intern("raw_beta_longer_entry", 21);
    (void)id_a; (void)id_b;
    uint32_t count_at_save = ray_sym_count();
    TEST_ASSERT_EQ_I(ray_sym_save(ray_test_tmp(TMP_DOM_SYM_PATH)), RAY_OK);
    ray_sym_domain_t* dom = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
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
    /* The file grows under another writer: the reopen extends the domain
     * (new mapping, new offsets, the old ones freed), and a pin taken after
     * it covers the grown prefix and still reads every old position.  A pin
     * taken before the reopen is not readable after it (domain.h). */
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    TEST_ASSERT_EQ_I(ray_sym_save(ray_test_tmp(TMP_DOM_SYM_PATH)), RAY_OK);
    ray_sym_domain_t* d1 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
    TEST_ASSERT_NOT_NULL(d1);
    ray_sym_domain_raw_t held;
    TEST_ASSERT(ray_sym_domain_raw_pin(d1, &held), "pin before growth");
    int64_t before = held.count;
    for (int i = 0; i < 64; i++) {
        char buf[32]; int n = snprintf(buf, sizeof buf, "raw_grow_%d", i);
        (void)ray_sym_intern(buf, (size_t)n);
    }
    TEST_ASSERT_EQ_I(ray_sym_save(ray_test_tmp(TMP_DOM_SYM_PATH)), RAY_OK);   /* grown image, same prefix */
    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));   /* cache hit → extend */
    TEST_ASSERT_EQ_PTR(d2, d1);
    TEST_ASSERT(ray_sym_domain_count(d1) > before, "reopen extended the domain");
    ray_sym_domain_raw_t fresh;
    TEST_ASSERT(ray_sym_domain_raw_pin(d1, &fresh), "pin after growth");
    TEST_ASSERT(fresh.count > before, "fresh snapshot covers the grown prefix");
    for (int64_t p = 0; p < before; p++) {
        size_t rl = 0;
        const char* rp = ray_sym_domain_raw_str(&fresh, p, &rl);
        ray_t* g = ray_sym_str(p);
        TEST_ASSERT_EQ_U(rl, ray_str_len(g));
        TEST_ASSERT_MEM_EQ(rl, rp, ray_str_ptr(g));
    }
    ray_sym_domain_raw_unpin(d1);
    ray_sym_domain_raw_unpin(d1);
    ray_sym_domain_release(d2);
    ray_sym_domain_release(d1);
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    PASS();
}

/* #717: a reader that reopens a symfile another process grew must keep
 * memory proportional to the vocabulary, however many times it reopened.
 * Each reopen is a cache hit that extends the domain; the arrays it
 * replaces and the mapping it supersedes used to be retired until the
 * domain died — and a replica never lets it die. */
static test_result_t test_domain_extend_reopen_flat(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    enum { VOCAB = 20000, WARM = 2, CYCLES = 30, PER_CYCLE = 10 };
    for (int i = 0; i < VOCAB; i++) {
        char b[32]; int n = snprintf(b, sizeof b, "flat_base_%d", i);
        (void)ray_sym_intern(b, (size_t)n);
    }
    TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);
    int64_t map_before_open = 0;
    ray_sys_get_mapped(&map_before_open, NULL);
    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    int64_t base = ray_sym_domain_count(dom);

    int64_t sys0 = 0, next = 0;
    for (int c = 0; c < WARM + CYCLES; c++) {
        if (c == WARM) ray_sys_get_stat(&sys0, NULL);
        for (int j = 0; j < PER_CYCLE; j++, next++) {
            char b[32]; int n = snprintf(b, sizeof b, "flat_new_%lld", (long long)next);
            (void)ray_sym_intern(b, (size_t)n);
        }
        TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);   /* tmp + rename: a new inode */
        ray_sym_domain_t* again = ray_sym_domain_open(TMP_DOM_SYM_PATH);
        TEST_ASSERT_EQ_PTR(again, dom);                              /* cache hit → extend */
        TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), base + next);
        /* the grown tail resolves both ways: lazily materialised atom and
         * reverse index (rebuilt over the grown vocabulary) */
        char last[32]; int ln = snprintf(last, sizeof last, "flat_new_%lld", (long long)(next - 1));
        ray_t* s = ray_sym_domain_str(dom, base + next - 1);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_EQ_U(ray_str_len(s), (size_t)ln);
        TEST_ASSERT_MEM_EQ((size_t)ln, ray_str_ptr(s), last);
        TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, last, (size_t)ln), base + next - 1);
        ray_sym_domain_release(again);
    }
    int64_t sys1 = 0, map1 = 0;
    ray_sys_get_stat(&sys1, NULL);
    ray_sys_get_mapped(&map1, NULL);
    struct stat st;
    TEST_ASSERT_EQ_I(stat(TMP_DOM_SYM_PATH, &st), 0);
    int64_t one_image = ((int64_t)st.st_size + 4095) & ~(int64_t)4095;
    /* a pin taken after the growth covers the whole grown prefix (a pin
     * taken before a reopen is not readable after it: see domain.h) */
    ray_sym_domain_raw_t fresh;
    TEST_ASSERT(ray_sym_domain_raw_pin(dom, &fresh), "pin after growth");
    TEST_ASSERT_EQ_I(fresh.count, base + next);
    for (int64_t p = 0; p < base + next; p += 997) {
        size_t rl = 0;
        const char* rp = ray_sym_domain_raw_str(&fresh, p, &rl);
        ray_t* g = ray_sym_str(p);
        TEST_ASSERT_EQ_U(rl, ray_str_len(g));
        TEST_ASSERT_MEM_EQ(rl, rp, ray_str_ptr(g));
    }
    ray_sym_domain_raw_unpin(dom);
    ray_sym_domain_release(dom);
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    /* committed RAM: nothing per reopen (slack for the atoms materialised
     * above and arena growth); file mappings: exactly one image of the
     * current file, whatever the number of reopens.  Asserted once the
     * domain is released, so a failure leaves none cached behind it. */
    /* The leak was one generation per reopen: two pointer-sized arrays
     * over the vocabulary, about 320 KB each cycle here, 9.6 MB over the
     * run.  The bound leaves room for arena and hash-table growth. */
    TEST_ASSERT_FMT(sys1 - sys0 <= 1024 * 1024,
                    "sys-current grew by %lld bytes over %d reopens",
                    (long long)(sys1 - sys0), CYCLES);
    TEST_ASSERT_FMT(map1 - map_before_open == one_image,
                    "sys-mapped holds %lld bytes for a %lld-byte symfile",
                    (long long)(map1 - map_before_open), (long long)one_image);
    PASS();
}
/* Write `words` as the symfile at `path` the way another process's writer
 * does: a tmp file renamed over it (a new inode). */
static bool write_symfile(const char* path, const char* const* words, int n) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.ext", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return false;
    uint32_t magic = 0x4C525453U;   /* "STRL" */
    int64_t count = n;
    bool ok = fwrite(&magic, 4, 1, f) == 1 && fwrite(&count, 8, 1, f) == 1;
    for (int i = 0; ok && i < n; i++) {
        uint32_t l = (uint32_t)strlen(words[i]);
        ok = fwrite(&l, 4, 1, f) == 1 && (l == 0 || fwrite(words[i], 1, l, f) == l);
    }
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, path) == 0;
}

/* Every position resolves to words[pos], by position and by string, and
 * a raw pin covers them all: the extend made them file positions. */
static bool dom_holds(ray_sym_domain_t* dom, const char* const* words, int n) {
    if (ray_sym_domain_count(dom) != n) return false;
    ray_sym_domain_raw_t raw;
    if (!ray_sym_domain_raw_pin(dom, &raw) || raw.count != n) return false;
    for (int p = 0; p < n; p++) {
        size_t l = strlen(words[p]), rl = 0;
        ray_t* s = ray_sym_domain_str(dom, p);
        const char* rp = ray_sym_domain_raw_str(&raw, p, &rl);
        if (!s || ray_str_len(s) != l || memcmp(ray_str_ptr(s), words[p], l) != 0 ||
            rl != l || memcmp(rp, words[p], l) != 0 ||
            ray_sym_domain_find(dom, words[p], l) != p)
            return false;
    }
    ray_sym_domain_raw_unpin(dom);
    return true;
}

/* A reopen after this process flushed its own appends: the positions the
 * flush wrote have atoms but no offset into the mapping (none at all when
 * the domain was created empty), yet the prefix check must still compare
 * them against the grown file — and the extend give them offsets. */
static test_result_t test_domain_extend_after_flush(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");

    /* (i) created empty, interned, flushed: nothing is mapped */
    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "fa", 2), 1);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "fb", 2), 2);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, false), RAY_OK);
    const char* grown1[] = { "", "fa", "fb", "fc", "fd" };
    TEST_ASSERT(write_symfile(TMP_DOM_SYM_PATH, grown1, 5), "external growth");
    ray_sym_domain_t* again = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_EQ_PTR(again, dom);
    TEST_ASSERT(dom_holds(dom, grown1, 5), "created-empty domain extended");
    ray_sym_domain_release(again);
    ray_sym_domain_release(dom);

    /* (ii) opened from 3 records, 2 interned and flushed: offsets cover 3 */
    const char* base[] = { "", "ga", "gb" };
    TEST_ASSERT(write_symfile(TMP_DOM_SYM_PATH, base, 3), "base file");
    dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "gc", 2), 3);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "gd", 2), 4);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, false), RAY_OK);
    /* a file that disagrees with the flushed tail is refused, and leaves
     * the domain as it was */
    const char* wrong[] = { "", "ga", "gb", "gc", "gX", "ge" };
    TEST_ASSERT(write_symfile(TMP_DOM_SYM_PATH, wrong, 6), "diverging file");
    TEST_ASSERT_NULL(ray_sym_domain_open(TMP_DOM_SYM_PATH));
    const char* grown2[] = { "", "ga", "gb", "gc", "gd", "ge", "gf" };
    TEST_ASSERT(write_symfile(TMP_DOM_SYM_PATH, grown2, 7), "external growth");
    again = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_EQ_PTR(again, dom);
    TEST_ASSERT(dom_holds(dom, grown2, 7), "flushed tail extended");
    ray_sym_domain_release(again);
    ray_sym_domain_release(dom);

    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    PASS();
}

/* A reopen that extends the domain while a batch intern probes outside
 * the lock drops the reverse index the batch then commits against.  The
 * batch interns exactly what the external writer appends, so however the
 * two interleave — extend first (all hits), during the probe (the misses
 * found again in the current index), after the commit (appended in batch
 * order; the reopen then refuses the dirty domain) — every string lands
 * at the position the file gives it. */
enum { XB_BASE = 64, XB_N = 50000, XB_ROUNDS = 10, XB_W = 24 };
typedef struct {
    ray_sym_domain_t* dom;
    const char* const* strs;
    const size_t*   lens;
    const uint32_t* hashes;
    int64_t*        out;
    _Atomic(bool)   started;
    bool            ok;
} xbatch_ctx_t;

static void xbatch_run(void* arg) {
    xbatch_ctx_t* c = (xbatch_ctx_t*)arg;
    atomic_store(&c->started, true);
    c->ok = ray_sym_domain_intern_batch(c->dom, XB_N, c->strs, c->lens, c->hashes, c->out);
}

static test_result_t test_domain_extend_under_batch(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    enum { ALL = XB_BASE + XB_N };
    char* buf = (char*)ray_sys_alloc((size_t)ALL * XB_W);
    const char** words = (const char**)ray_sys_alloc((size_t)ALL * sizeof(char*));
    size_t* lens = (size_t*)ray_sys_alloc((size_t)XB_N * sizeof(size_t));
    uint32_t* hashes = (uint32_t*)ray_sys_alloc((size_t)XB_N * sizeof(uint32_t));
    int64_t* out = (int64_t*)ray_sys_alloc((size_t)XB_N * sizeof(int64_t));
    TEST_ASSERT(buf && words && lens && hashes && out, "buffers");
    for (int r = 0; r < XB_ROUNDS; r++) {
        words[0] = "";
        for (int i = 1; i < ALL; i++) {
            snprintf(buf + (size_t)i * XB_W, XB_W, "xb%d_%d", r, i);
            words[i] = buf + (size_t)i * XB_W;
        }
        for (int i = 0; i < XB_N; i++) {
            lens[i] = strlen(words[XB_BASE + i]);
            hashes[i] = (uint32_t)ray_hash_bytes(words[XB_BASE + i], lens[i]);
        }
        TEST_ASSERT(write_symfile(TMP_DOM_SYM_PATH, words, XB_BASE), "base file");
        TEST_ASSERT(write_symfile(TMP_DOM_SYM_PATH ".grown", words, ALL), "grown file");
        ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
        TEST_ASSERT_NOT_NULL(dom);
        TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, words[1], strlen(words[1])), 1);   /* index built */

        xbatch_ctx_t c = { dom, words + XB_BASE, lens, hashes, out, false, false };
        ray_thread_t th;
        TEST_ASSERT_EQ_I(ray_thread_create(&th, xbatch_run, &c), RAY_OK);
        while (!atomic_load(&c.started)) {}
        int moved = rename(TMP_DOM_SYM_PATH ".grown", TMP_DOM_SYM_PATH);
        ray_sym_domain_t* again = ray_sym_domain_open(TMP_DOM_SYM_PATH);
        ray_thread_join(th);

        TEST_ASSERT_EQ_I(moved, 0);
        TEST_ASSERT(c.ok, "batch interned");
        for (int i = 0; i < XB_N; i++) TEST_ASSERT_EQ_I(out[i], XB_BASE + i);
        if (again) ray_sym_domain_release(again);   /* NULL: the batch committed first */
        ray_sym_domain_release(dom);
    }
    ray_sys_free(buf);
    ray_sys_free(words);
    ray_sys_free(lens);
    ray_sys_free(hashes);
    ray_sys_free(out);
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
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
    PASS();
}

static test_result_t test_domain_concat_text_nulls(void) {
    ray_sym_domain_t* dom = NULL;
    int64_t pos_a = -1, pos_b = -1;
    TEST_ASSERT_TRUE(build_divergent_qsym_fixture(&dom, &pos_a, &pos_b));
    /* Fixture includes the builtin vocabulary, so its positions need W16. */
    const uint8_t widths[] = {RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64};
    for (int w = 0; w < 3; w++) {
        ray_t* file = ray_sym_vec_new(widths[w], 4);
        ray_sym_domain_release(file->sym_domain);
        ray_sym_domain_retain(dom);
        file->sym_domain = dom;
        file->len = 4;
        const int64_t vals[] = {pos_b, pos_a, 0, pos_b};
        for (int i = 0; i < 4; i++) ray_write_sym(ray_data(file), i, vals[i], RAY_SYM, file->attrs);
        int64_t ids[] = {0, ray_sym_intern("dq_b", 4)};
        ray_t* runtime = ray_vec_from_raw(RAY_SYM, ids, 2);
        ray_t* slice = ray_vec_slice(file, 1, 2);
        for (int side = 0; side < 2; side++) {
            ray_t* out = ray_vec_concat(side ? runtime : slice, side ? slice : runtime);
            TEST_ASSERT_NOT_NULL(out);
            TEST_ASSERT_FALSE(RAY_IS_ERR(out));
            TEST_ASSERT_EQ_PTR(ray_sym_vec_domain(out), ray_sym_runtime_domain());
            TEST_ASSERT_EQ_I(out->attrs & RAY_SYM_W_MASK, RAY_SYM_W64);
            TEST_ASSERT_TRUE(out->attrs & RAY_ATTR_HAS_NULLS);
            TEST_ASSERT_FALSE(out->attrs & (RAY_ATTR_SLICE | RAY_ATTR_HAS_INDEX));
            int64_t expected[] = {ray_sym_intern("dq_a", 4), 0, 0, ids[1]};
            for (int i = 0; i < 4; i++) {
                int64_t value = expected[(i + (side ? 2 : 0)) % 4];
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(out))[i], value);
                TEST_ASSERT_EQ_I(ray_vec_is_null(out, i), value == 0);
            }
            ray_release(out);
        }
        ray_release(slice); ray_release(file); ray_release(runtime);
    }
    ray_sym_domain_release(dom);
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
    PASS();
}

#define TMP_DOM_BADSYM_PATH "/tmp/rayforce_test_domain_badsym"

/* Position-0 reservation: ray_sym_save-produced files carry "" at
 * position 0 (global id 0) and open fine; a crafted symfile whose first
 * entry is NOT "" is refused (NULL); an EMPTY vocabulary is fine. */
static test_result_t test_domain_open_position0_validation(void) {
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
    unlink(ray_test_tmp(TMP_DOM_BADSYM_PATH));

    /* (a) the save path today: "" at position 0 — opens. */
    (void)ray_sym_intern("p0_ok", 5);
    TEST_ASSERT_EQ_I(ray_sym_save(ray_test_tmp(TMP_DOM_SYM_PATH)), RAY_OK);
    ray_sym_domain_t* good = ray_sym_domain_open(ray_test_tmp(TMP_DOM_SYM_PATH));
    TEST_ASSERT_NOT_NULL(good);
    ray_t* s0 = ray_sym_domain_str(good, 0);
    TEST_ASSERT_NOT_NULL(s0);
    TEST_ASSERT_EQ_U(ray_str_len(s0), 0);
    ray_sym_domain_release(good);

    /* (b) crafted STRL whose position 0 is "x": refused. */
    {
        FILE* f = fopen(ray_test_tmp(TMP_DOM_BADSYM_PATH), "wb");
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
    TEST_ASSERT_NULL(ray_sym_domain_open(ray_test_tmp(TMP_DOM_BADSYM_PATH)));

    /* (c) empty vocabulary (count 0): structurally fine. */
    {
        FILE* f = fopen(ray_test_tmp(TMP_DOM_BADSYM_PATH), "wb");
        TEST_ASSERT_NOT_NULL(f);
        uint32_t magic = 0x4C525453U;
        int64_t  count = 0;
        fwrite(&magic, 4, 1, f);
        fwrite(&count, 8, 1, f);
        fclose(f);
    }
    ray_sym_domain_t* empty = ray_sym_domain_open(ray_test_tmp(TMP_DOM_BADSYM_PATH));
    TEST_ASSERT_NOT_NULL(empty);
    TEST_ASSERT_EQ_I(ray_sym_domain_count(empty), 0);
    TEST_ASSERT_NULL(ray_sym_domain_str(empty, 0));
    /* empty FILE domain still yields a non-NULL LUT marker (callers
     * branch on NULL == runtime domain) that is never indexable */
    TEST_ASSERT_NOT_NULL(ray_sym_domain_runtime_lut(empty));
    ray_sym_domain_release(empty);

    unlink(ray_test_tmp(TMP_DOM_BADSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_SYM_PATH ".lk"));
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
    ray_sym_domain_t* d2 = ray_sym_domain_open(ray_test_tmp(TMP_DOM_QSYM_PATH));
    TEST_ASSERT_NOT_NULL(d2);
    ray_sym_domain_release(d2);

    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH));
    unlink(ray_test_tmp(TMP_DOM_QSYM_PATH ".lk"));
    PASS();
}

/* ---- registration -------------------------------------------------------- */

static test_result_t test_domain_private(void) {
    ray_sym_domain_t* a = ray_sym_domain_new();
    ray_sym_domain_t* b = ray_sym_domain_new();
    TEST_ASSERT_NOT_NULL(a); TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(a != b);
    TEST_ASSERT_NULL(ray_sym_domain_path(a));
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(a,"private-a",9),1);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(b,"private-b",9),1);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(b,"private-a",9),-1);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(a,true),RAY_ERR_DOMAIN);
    ray_t* v = ray_sym_vec_new(RAY_SYM_W32,1);
    TEST_ASSERT_NOT_NULL(v);
    v->len = 1; v->sym_domain = a; ray_sym_domain_retain(a);
    ((uint32_t*)ray_data(v))[0] = 1;
    ray_sym_domain_release(a); ray_sym_domain_release(b);
    TEST_ASSERT_TRUE(!strcmp(ray_str_ptr(ray_sym_vec_cell(v,0)),"private-a"));
    ray_release(v);
    PASS();
}

/* ray_sym_domain_flush_append: a symfile grown in three appends (the first
 * creating it, the last with nothing new but syncing) holds exactly the
 * bytes one ray_sym_domain_flush of the same vocabulary writes, reopens
 * with every position, and a file changed behind the domain's back is
 * refused as another writer. */
static bool dom_file_bytes(const char* path, uint8_t** out, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    *out = (uint8_t*)ray_sys_alloc((size_t)*n + 1);
    bool ok = *out && fread(*out, 1, (size_t)*n, f) == (size_t)*n;
    fclose(f);
    return ok;
}
static test_result_t test_domain_flush_append(void) {
    const char* pa = TMP_DOM_SYM_PATH "_app";
    const char* pb = TMP_DOM_SYM_PATH "_one";
    char buf[64];
    for (int k = 0; k < 2; k++) {
        const char* p = k ? pb : pa;
        unlink(p);
        snprintf(buf, sizeof(buf), "%s.lk", p); unlink(buf);
    }
    ray_sym_domain_t* a = ray_sym_domain_open_or_create(pa);
    ray_sym_domain_t* b = ray_sym_domain_open_or_create(pb);
    TEST_ASSERT_NOT_NULL(a); TEST_ASSERT_NOT_NULL(b);
    for (int round = 0; round < 3; round++) {
        int n = round == 2 ? 0 : 500 + 300 * round;
        for (int i = 0; i < n; i++) {
            int l = snprintf(buf, sizeof(buf), "r%d-%d%s", round, i, i % 7 ? "" : "-a-longer-string");
            TEST_ASSERT_TRUE(ray_sym_domain_intern(a, buf, (size_t)l) >= 0);
            TEST_ASSERT_TRUE(ray_sym_domain_intern(b, buf, (size_t)l) >= 0);
        }
        TEST_ASSERT_EQ_I(ray_sym_domain_flush_append(a, round == 2), RAY_OK);
    }
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(b, true), RAY_OK);
    uint8_t *fa = NULL, *fb = NULL; long na = 0, nb = 0;
    TEST_ASSERT_TRUE(dom_file_bytes(pa, &fa, &na));
    TEST_ASSERT_TRUE(dom_file_bytes(pb, &fb, &nb));
    TEST_ASSERT_EQ_I(na, nb);
    TEST_ASSERT_EQ_I(memcmp(fa, fb, (size_t)na), 0);
    ray_sys_free(fa); ray_sys_free(fb);
    int64_t count = ray_sym_domain_count(a);

    /* another writer: the file is no longer the persisted prefix */
    FILE* f = fopen(pa, "ab");
    TEST_ASSERT_NOT_NULL(f);
    fputc(0, f); fclose(f);
    TEST_ASSERT_TRUE(ray_sym_domain_intern(a, "late", 4) >= 0);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush_append(a, false), RAY_ERR_CORRUPT);
    ray_sym_domain_release(a);
    ray_sym_domain_release(b);

    ray_sym_domain_t* re = ray_sym_domain_open(pb);
    TEST_ASSERT_NOT_NULL(re);
    TEST_ASSERT_EQ_I(ray_sym_domain_count(re), count);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(re, "r1-799", 6) >= 0, 1);
    ray_sym_domain_release(re);
    for (int k = 0; k < 2; k++) {
        const char* p = k ? pb : pa;
        unlink(p);
        snprintf(buf, sizeof(buf), "%s.lk", p); unlink(buf);
    }
    PASS();
}

/* ray_sym_domain_create_import: on an 8-worker pool, 32 tasks intern the
 * same 100K strings in the same order at once (every miss is raced), then
 * each its own slice of 800K more, so shard tables and the mapped file both
 * grow past their first size.  A string gets one position whoever adds it,
 * "" is position 0, the count is the distinct strings plus one, and after
 * the durable flush the file reopens as an ordinary domain with every
 * string at its position. */
#define IMP_SHARED 100000
#define IMP_OWN    800000
#define IMP_N      (IMP_SHARED + IMP_OWN)
#define IMP_TASKS  32
typedef struct { ray_sym_domain_t* dom; int64_t* pos; _Atomic(int) bad; } imp_ctx_t;
static int imp_str(int64_t i, char* buf) { return snprintf(buf, 32, "import-%lld-%c", (long long)i, (char)('a' + i % 26)); }
static void imp_range(imp_ctx_t* c, int64_t s, int64_t e) {
    enum { B = 512 };
    char bufs[B][32]; const char* strs[B]; size_t lens[B]; uint32_t hs[B]; int64_t out[B];
    for (int64_t i = s; i < e; i += B) {
        int64_t n = e - i < B ? e - i : B;
        for (int64_t j = 0; j < n; j++) {
            lens[j] = (size_t)imp_str(i + j, bufs[j]); strs[j] = bufs[j];
            hs[j] = (uint32_t)ray_hash_bytes(strs[j], lens[j]);
        }
        if (!ray_sym_domain_intern_batch(c->dom, n, strs, lens, hs, out)) { atomic_store(&c->bad, 1); return; }
        for (int64_t j = 0; j < n; j++) {
            int64_t prev = 0;
            if (!atomic_compare_exchange_strong((_Atomic(int64_t)*)&c->pos[i + j], &prev, out[j]) && prev != out[j])
                atomic_store(&c->bad, 1);
        }
    }
}
static void imp_task(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    imp_ctx_t* c = (imp_ctx_t*)raw;
    imp_range(c, 0, IMP_SHARED);
    int64_t per = IMP_OWN / IMP_TASKS;
    imp_range(c, IMP_SHARED + start * per, IMP_SHARED + (start + 1) * per);
}
static test_result_t test_domain_import_dict(void) {
    const char* p = TMP_DOM_SYM_PATH "_imp";
    unlink(p);
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(8), RAY_OK);
    ray_pool_t* pool = ray_pool_get();
    ray_sym_domain_t* dom = ray_sym_domain_create_import(p);
    if (!dom) { ray_pool_destroy(); SKIP("import dictionary unsupported here"); }
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), 1);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "", 0), 0);
    imp_ctx_t ctx = { dom, (int64_t*)ray_sys_alloc((size_t)IMP_N * sizeof(int64_t)), 0 };
    TEST_ASSERT_NOT_NULL(ctx.pos);
    memset(ctx.pos, 0, (size_t)IMP_N * sizeof(int64_t));
    if (pool && pool->n_workers > 0) ray_pool_dispatch_n(pool, imp_task, &ctx, IMP_TASKS);
    else for (int t = 0; t < IMP_TASKS; t++) imp_task(&ctx, 0, t, t + 1);
    TEST_ASSERT_EQ_I(atomic_load(&ctx.bad), 0);
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), IMP_N + 1);
    /* one more append and its single-string intern agree */
    int64_t z = ray_sym_domain_intern(dom, "late-one", 8);
    TEST_ASSERT_EQ_I(z, IMP_N + 1);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "late-one", 8), z);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, true), RAY_OK);
    ray_sym_domain_release(dom);

    ray_sym_domain_t* re = ray_sym_domain_open(p);
    TEST_ASSERT_NOT_NULL(re);
    TEST_ASSERT_EQ_I(ray_sym_domain_count(re), IMP_N + 2);
    int64_t bad = 0;
    char buf[32];
    for (int64_t i = 0; i < IMP_N; i += 997) {
        int l = imp_str(i, buf);
        ray_t* s = ray_sym_domain_str(re, ctx.pos[i]);
        if (!s || ray_str_len(s) != (size_t)l || memcmp(ray_str_ptr(s), buf, (size_t)l)) bad++;
        if (ray_sym_domain_find(re, buf, (size_t)l) != ctx.pos[i]) bad++;
    }
    TEST_ASSERT_EQ_I(bad, 0);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(re, "", 0), 0);
    TEST_ASSERT_EQ_I(ray_sym_domain_find(re, "late-one", 8), IMP_N + 1);
    ray_sym_domain_release(re);
    ray_sys_free(ctx.pos);
    ray_pool_destroy();
    unlink(p);
    unlink(TMP_DOM_SYM_PATH "_imp.lk");
    PASS();
}
/* The import dictionary frees a shard table it replaced once no lookup can
 * hold it.  Serially, 300K strings leave every one of the 1024 shard tables
 * at its first size, and 900K at twice that (a shard's share of 900K stays
 * under the next growth); between the two, committed memory grows by the
 * new tables less the old ones, not by the new tables whole.  A table is
 * cap 8-byte slots plus a header, counted in 4 KiB units as ray_sys_get_stat
 * counts every allocation whatever the page size. */
static int64_t imp_tab_bytes(int64_t cap) {
    return (cap * 8 + 64 + 4095) / 4096 * 4096;
}
static bool imp_intern_serial(ray_sym_domain_t* dom, int64_t lo, int64_t hi) {
    imp_ctx_t c = { dom, NULL, 0 };
    enum { B = 512 };
    char bufs[B][32]; const char* strs[B]; size_t lens[B]; uint32_t hs[B]; int64_t out[B];
    for (int64_t i = lo; i < hi; i += B) {
        int64_t n = hi - i < B ? hi - i : B;
        for (int64_t j = 0; j < n; j++) {
            lens[j] = (size_t)imp_str(i + j, bufs[j]); strs[j] = bufs[j];
            hs[j] = (uint32_t)ray_hash_bytes(strs[j], lens[j]);
        }
        if (!ray_sym_domain_intern_batch(c.dom, n, strs, lens, hs, out)) return false;
    }
    return true;
}
static test_result_t test_domain_import_dict_reclaim(void) {
    const char* p = TMP_DOM_SYM_PATH "_impr";
    unlink(p);
    ray_sym_domain_t* dom = ray_sym_domain_create_import(p);
    if (!dom) SKIP("import dictionary unsupported here");
    TEST_ASSERT_TRUE(imp_intern_serial(dom, 0, 300000));
    /* one more new string: the last batch's replaced tables are freed by
     * the next batch that adds */
    TEST_ASSERT_TRUE(ray_sym_domain_intern(dom, "flush-a", 7) > 0);
    int64_t a = 0, b = 0;
    ray_sys_get_stat(&a, NULL);
    TEST_ASSERT_TRUE(imp_intern_serial(dom, 300000, 900000));
    TEST_ASSERT_TRUE(ray_sym_domain_intern(dom, "flush-b", 7) > 0);
    ray_sys_get_stat(&b, NULL);
    int64_t grew = b - a, small = imp_tab_bytes(1024), big = imp_tab_bytes(2048);
    TEST_ASSERT_TRUE(grew >= 1024 * (big - small) - (1 << 20));   /* every shard grew */
    TEST_ASSERT_TRUE(grew < 1024 * (big - small / 2));            /* and gave its old table back */
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), 900003);
    ray_sym_domain_release(dom);
    unlink(p);
    unlink(TMP_DOM_SYM_PATH "_impr.lk");
    PASS();
}
/* The import dictionary takes the shard from the 64-bit hash's top bits and
 * keeps its low 32 as the slot tag.  Crafted hashes (the dictionary trusts
 * the caller's): 1024 strings whose hashes share the low 32 bits but not
 * the top 10 land in 1024 shards, and are added and found again without a
 * single tag matching another string; 64 strings sharing one whole hash all
 * get positions of their own and are found by their bytes.  Every position
 * survives a re-intern, and the 32-bit entry point, which hashes again,
 * finds strings the 64-bit one added under their real hashes.  The counts
 * are kept for a dictionary made under the converters' trace (RAY_CSV_TRACE
 * at create, set here for it); one made without it keeps none, its
 * positions the same. */
static void imp_trace_env(bool on) {
#if defined(_WIN32)
    _putenv_s("RAY_CSV_TRACE", on ? "1" : "");
#else
    if (on) setenv("RAY_CSV_TRACE", "1", 1); else unsetenv("RAY_CSV_TRACE");
#endif
}
#define IMP_SPREAD 1024
#define IMP_SAME   64
static test_result_t test_domain_import_dict_tags(void) {
    const char* p = TMP_DOM_SYM_PATH "_impt";
    unlink(p);
    imp_trace_env(true);
    ray_sym_domain_t* dom = ray_sym_domain_create_import(p);
    imp_trace_env(false);
    if (!dom) SKIP("import dictionary unsupported here");
    enum { N = IMP_SPREAD + IMP_SAME };
    static char bufs[N][32];
    const char* strs[N]; size_t lens[N]; uint64_t hs[N]; int64_t pos[N], again[N];
    for (int i = 0; i < N; i++) {
        lens[i] = (size_t)snprintf(bufs[i], sizeof(bufs[i]), "tag-%d", i); strs[i] = bufs[i];
        hs[i] = i < IMP_SPREAD ? ((uint64_t)i << 54) | UINT64_C(0x5a5a5a5a)
                               : UINT64_C(0x0123456789abcdef);
    }
    ray_symimp_stats_t st;
    ray_sym_domain_import_stats(dom, &st);   /* from zero */
    TEST_ASSERT_TRUE(ray_sym_domain_intern_batch64(dom, IMP_SPREAD, strs, lens, hs, pos));
    TEST_ASSERT_TRUE(ray_sym_domain_intern_batch64(dom, IMP_SPREAD, strs, lens, hs, again));
    TEST_ASSERT_TRUE(ray_sym_domain_import_stats(dom, &st));
    TEST_ASSERT_EQ_I(st.added, IMP_SPREAD);
    TEST_ASSERT_EQ_I(st.hits, IMP_SPREAD);
    TEST_ASSERT_EQ_I(st.false_tags, 0);
    /* one at a time, so each is looked up in the shard the others fill */
    for (int i = IMP_SPREAD; i < N; i++)
        TEST_ASSERT_TRUE(ray_sym_domain_intern_batch64(dom, 1, strs + i, lens + i, hs + i, pos + i));
    for (int i = IMP_SPREAD; i < N; i++)
        TEST_ASSERT_TRUE(ray_sym_domain_intern_batch64(dom, 1, strs + i, lens + i, hs + i, again + i));
    TEST_ASSERT_TRUE(ray_sym_domain_import_stats(dom, &st));
    TEST_ASSERT_EQ_I(st.added, IMP_SAME);
    TEST_ASSERT_TRUE(st.false_tags > 0);
    for (int i = 0; i < N; i++) {
        TEST_ASSERT_EQ_I(pos[i], i + 1);   /* "" is position 0 */
        TEST_ASSERT_EQ_I(again[i], pos[i]);
    }
    TEST_ASSERT_EQ_I(ray_sym_domain_count(dom), N + 1);
    /* real hashes: added through the 64-bit entry, found through the 32-bit */
    const char* real = "a real hash";
    size_t rl = strlen(real);
    uint64_t rh = ray_hash_bytes(real, rl);
    uint32_t rh32 = (uint32_t)rh;
    int64_t r64 = -1, r32 = -2;
    TEST_ASSERT_TRUE(ray_sym_domain_intern_batch64(dom, 1, &real, &rl, &rh, &r64));
    TEST_ASSERT_TRUE(ray_sym_domain_intern_batch(dom, 1, &real, &rl, &rh32, &r32));
    TEST_ASSERT_EQ_I(r64, N + 1);
    TEST_ASSERT_EQ_I(r32, r64);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, real, rl), r64);
    ray_sym_domain_release(dom);
    unlink(p);
    unlink(TMP_DOM_SYM_PATH "_impt.lk");
    /* made without the trace: the same positions, no counts */
    dom = ray_sym_domain_create_import(p);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_TRUE(ray_sym_domain_intern_batch64(dom, IMP_SPREAD, strs, lens, hs, again));
    for (int i = IMP_SPREAD; i < N; i++)
        TEST_ASSERT_TRUE(ray_sym_domain_intern_batch64(dom, 1, strs + i, lens + i, hs + i, again + i));
    TEST_ASSERT_TRUE(ray_sym_domain_intern_batch64(dom, N, strs, lens, hs, again));
    for (int i = 0; i < N; i++) TEST_ASSERT_EQ_I(again[i], pos[i]);
    TEST_ASSERT_TRUE(ray_sym_domain_import_stats(dom, &st));
    TEST_ASSERT_EQ_I(st.strings + st.probes + st.hits + st.added + st.slots + st.false_tags + st.grows, 0);
    TEST_ASSERT_EQ_I(st.count, N + 1);
    ray_sym_domain_release(dom);
    unlink(p);
    unlink(TMP_DOM_SYM_PATH "_impt.lk");
    PASS();
}
/* The first reservation (ray_symimp_reserve: the grouped import's records)
 * ends the dictionary's interning: from it on a lookup is refused and no
 * string is added, and the shard tables go back at once unless a lookup is
 * in flight (which keeps them to the end).  Serially: the tables go, the
 * counts and the index bytes read none, the reserved positions follow the
 * interned ones, and a lookup is refused.  Then rounds of lookup threads
 * against a reservation the main thread makes once every thread has got a
 * batch through: 64K strings already there, found batch after batch (eight
 * chunks, between which a thread holds no table), on one thread and on
 * four; and on two, the same with one new string a chunk (a lookup of the
 * chunk then adds it under the lock).  Every batch gets every position
 * right or is refused; none reads a table freed under it (a reservation
 * that frees regardless of a lookup in flight, or a lookup that reads the
 * reservation before it registers, faults here) and none adds a string past
 * the reservation (as an add that does not check it under the lock does). */
#define IMPR_N     65536
#define IMPR_EVERY 8192   /* the new strings' spacing: one a chunk (SI_BATCH) */
typedef struct {
    ray_symimp_t* imp;
    const char* const* strs; const size_t* lens; const uint64_t* hs; const int64_t* want;
    int fresh;                  /* 0: the known strings; else this thread's tag for its new ones */
    char (*buf)[32]; const char** fs; size_t* fl; uint64_t* fh;   /* fresh: the batch */
    int64_t* out;
    _Atomic(int64_t) found;     /* batches got through */
    _Atomic(int) done, bad;
} impr_ctx_t;
static void impr_lookups(void* raw) {
    impr_ctx_t* c = (impr_ctx_t*)raw;
    const char* const* s = c->strs; const size_t* l = c->lens; const uint64_t* h = c->hs;
    if (c->fresh) {
        memcpy(c->fs, c->strs, (size_t)IMPR_N * sizeof(char*));
        memcpy(c->fl, c->lens, (size_t)IMPR_N * sizeof(size_t));
        memcpy(c->fh, c->hs, (size_t)IMPR_N * sizeof(uint64_t));
        s = c->fs; l = c->fl; h = c->fh;
    }
    for (int64_t k = 0;; k++) {
        for (int64_t i = 0; c->fresh && i < IMPR_N; i += IMPR_EVERY) {
            char* b = c->buf[i / IMPR_EVERY];
            c->fl[i] = (size_t)snprintf(b, 32, "fresh-%d-%lld", c->fresh, (long long)(k * IMPR_N + i));
            c->fs[i] = b;
            c->fh[i] = ray_hash_bytes(b, c->fl[i]);
        }
        if (!ray_symimp_intern_batch(c->imp, IMPR_N, s, l, h, c->out)) break;
        for (int64_t i = 0; i < IMPR_N; i++) {
            bool ok = c->out[i] == c->want[i];
            if (c->fresh && i % IMPR_EVERY == 0) {
                uint32_t len = 0;
                const char* r = ray_symimp_get(c->imp, c->out[i], &len);
                ok = len == l[i] && !memcmp(r, s[i], len);
            }
            if (!ok) { atomic_store(&c->bad, 1); break; }
        }
        atomic_fetch_add(&c->found, 1);
    }
    atomic_store(&c->done, 1);
}
static test_result_t test_domain_import_dict_reserve(void) {
    const char* p = TMP_DOM_SYM_PATH "_imps";
    char (*bufs)[32] = (char (*)[32])ray_sys_alloc((size_t)IMPR_N * 32);
    const char** strs = (const char**)ray_sys_alloc((size_t)IMPR_N * sizeof(char*));
    size_t* lens = (size_t*)ray_sys_alloc((size_t)IMPR_N * sizeof(size_t));
    uint64_t* hs = (uint64_t*)ray_sys_alloc((size_t)IMPR_N * sizeof(uint64_t));
    int64_t* want = (int64_t*)ray_sys_alloc((size_t)IMPR_N * sizeof(int64_t));
    int64_t* outs = (int64_t*)ray_sys_alloc((size_t)4 * IMPR_N * sizeof(int64_t));
    char (*fbuf)[32] = (char (*)[32])ray_sys_alloc((size_t)2 * (IMPR_N / IMPR_EVERY) * 32);
    const char** fs = (const char**)ray_sys_alloc((size_t)2 * IMPR_N * sizeof(char*));
    size_t* fl = (size_t*)ray_sys_alloc((size_t)2 * IMPR_N * sizeof(size_t));
    uint64_t* fh = (uint64_t*)ray_sys_alloc((size_t)2 * IMPR_N * sizeof(uint64_t));
    TEST_ASSERT_TRUE(bufs && strs && lens && hs && want && outs && fbuf && fs && fl && fh);
    for (int64_t i = 0; i < IMPR_N; i++) {
        lens[i] = (size_t)imp_str(i, bufs[i]); strs[i] = bufs[i];
        hs[i] = ray_hash_bytes(strs[i], lens[i]);
    }
    /* round 0 serial; 1-8 one thread finding, 9-12 four, 13-16 two adding too */
    int timeouts = 0, bad = 0;
    for (int round = 0; round <= 16; round++) {
        unlink(p);
        ray_sym_domain_t* dom = ray_sym_domain_create_import(p);
        if (!dom) {
            ray_sys_free(bufs); ray_sys_free(strs); ray_sys_free(lens); ray_sys_free(hs); ray_sys_free(want);
            ray_sys_free(outs); ray_sys_free(fbuf); ray_sys_free(fs); ray_sys_free(fl); ray_sys_free(fh);
            SKIP("import dictionary unsupported here");
        }
        ray_symimp_t* imp = ray_sym_domain_import(dom);
        TEST_ASSERT_NOT_NULL(imp);
        TEST_ASSERT_TRUE(ray_symimp_intern_batch(imp, IMPR_N, strs, lens, hs, want));
        int64_t count = ray_symimp_count(imp), p0 = -1, o0 = -1, again = -1;
        TEST_ASSERT_EQ_I(count, IMPR_N + 1);   /* "" is position 0 */
        if (round == 0) {
            TEST_ASSERT_TRUE(ray_symimp_index_bytes(imp) > 0);
            TEST_ASSERT_TRUE(ray_symimp_reserve(imp, 2, 16, &p0, &o0));
            TEST_ASSERT_EQ_I(p0, count);
            TEST_ASSERT_EQ_I(ray_symimp_count(imp), count + 2);
            TEST_ASSERT_EQ_I(ray_symimp_index_bytes(imp), 0);
            ray_symimp_stats_t st;
            ray_symimp_stats(imp, &st);
            TEST_ASSERT_EQ_I(st.tab_bytes, 0);
            TEST_ASSERT_EQ_I(st.count, count + 2);
            TEST_ASSERT_FALSE(ray_symimp_intern_batch(imp, 1, strs, lens, hs, &again));
            ray_symimp_put(imp, p0, o0, "res0", 4); ray_symimp_put(imp, p0 + 1, o0 + 8, "res1", 4);
            TEST_ASSERT_TRUE(ray_symimp_reserve(imp, 1, 8, &p0, &o0));   /* a later one: nothing more to free */
            TEST_ASSERT_EQ_I(p0, count + 2);
            ray_symimp_put(imp, p0, o0, "res2", 4);
        } else {
            int nt = round <= 8 ? 1 : round <= 12 ? 4 : 2, fresh = round > 12;
            impr_ctx_t ctx[4];
            ray_thread_t th[4];
            int started = 0;
            for (int t = 0; t < nt; t++) {
                ctx[t] = (impr_ctx_t){ imp, strs, lens, hs, want, fresh ? t + 1 : 0,
                                       fbuf + t * (IMPR_N / IMPR_EVERY), fs + (int64_t)t * IMPR_N, fl + (int64_t)t * IMPR_N, fh + (int64_t)t * IMPR_N,
                                       outs + (int64_t)t * IMPR_N, 0, 0, 0 };
                if (ray_thread_create(&th[t], impr_lookups, &ctx[t]) == RAY_OK) started++;
                else break;
            }
            /* every thread through a batch (or out), within a bound */
            int64_t until = ray_profile_now_ns() + (int64_t)30 * 1000000000;
            for (int t = 0; t < started; t++)
                while (!atomic_load(&ctx[t].found) && !atomic_load(&ctx[t].done)) {
                    if (ray_profile_now_ns() > until) { timeouts++; break; }
                    RAY_CPU_RELAX();
                }
            bool reserved = ray_symimp_reserve(imp, 1, 8, &p0, &o0);
            for (int t = 0; t < started; t++) ray_thread_join(th[t]);
            TEST_ASSERT_EQ_I(started, nt);
            TEST_ASSERT_TRUE(reserved);
            for (int t = 0; t < nt; t++) {
                if (atomic_load(&ctx[t].bad) || !atomic_load(&ctx[t].found)) bad++;
            }
            /* nothing added past the reservation */
            if (ray_symimp_count(imp) != p0 + 1) {
                fprintf(stderr, "  reserve round %d: %lld strings, the reservation at %lld\n", round,
                        (long long)ray_symimp_count(imp), (long long)p0);
                bad++;
            }
            TEST_ASSERT_FALSE(ray_symimp_intern_batch(imp, 1, strs, lens, hs, &again));
            ray_symimp_put(imp, p0, o0, "res0", 4);
        }
        ray_sym_domain_release(dom);
        unlink(p);
        unlink(TMP_DOM_SYM_PATH "_imps.lk");
    }
    ray_sys_free(bufs); ray_sys_free(strs); ray_sys_free(lens); ray_sys_free(hs); ray_sys_free(want);
    ray_sys_free(outs); ray_sys_free(fbuf); ray_sys_free(fs); ray_sys_free(fl); ray_sys_free(fh);
    TEST_ASSERT_EQ_I(timeouts, 0);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
}
/* Skeptic probe (pqio round 4).  Rounds of the reservation against lookups
 * in flight (two threads finding the 64K strings batch after batch), as in
 * import_dict_reserve: how often the reservation kept the shard tables (a
 * lookup was in flight, so ray_symimp_index_bytes still reads them right
 * after it), and that a kept set goes back with the dictionary: the
 * committed memory (ray_heap_anon_committed, which every ray_sys_alloc
 * counts) the same after a round as before it.  The tables are 1024
 * mappings of 12 KB each: a kept set never freed shows as 12 MB. */
static test_result_t test_domain_sk4_reserve_kept_freed(void) {
    const char* p = TMP_DOM_SYM_PATH "_imk";
    char (*bufs)[32] = (char (*)[32])ray_sys_alloc((size_t)IMPR_N * 32);
    const char** strs = (const char**)ray_sys_alloc((size_t)IMPR_N * sizeof(char*));
    size_t* lens = (size_t*)ray_sys_alloc((size_t)IMPR_N * sizeof(size_t));
    uint64_t* hs = (uint64_t*)ray_sys_alloc((size_t)IMPR_N * sizeof(uint64_t));
    int64_t* want = (int64_t*)ray_sys_alloc((size_t)IMPR_N * sizeof(int64_t));
    int64_t* outs = (int64_t*)ray_sys_alloc((size_t)2 * IMPR_N * sizeof(int64_t));
    TEST_ASSERT_TRUE(bufs && strs && lens && hs && want && outs);
    for (int64_t i = 0; i < IMPR_N; i++) {
        lens[i] = (size_t)imp_str(i, bufs[i]); strs[i] = bufs[i];
        hs[i] = ray_hash_bytes(strs[i], lens[i]);
    }
    int kept = 0, freed = 0, leaks = 0, timeouts = 0, bad = 0, measured = 0;
    for (int round = 0; round < 17; round++) {
        unlink(p);
        int64_t c0 = ray_heap_anon_committed();
        ray_sym_domain_t* dom = ray_sym_domain_create_import(p);
        if (!dom) {
            ray_sys_free(bufs); ray_sys_free(strs); ray_sys_free(lens); ray_sys_free(hs); ray_sys_free(want); ray_sys_free(outs);
            SKIP("import dictionary unsupported here");
        }
        ray_symimp_t* imp = ray_sym_domain_import(dom);
        TEST_ASSERT_NOT_NULL(imp);
        TEST_ASSERT_TRUE(ray_symimp_intern_batch(imp, IMPR_N, strs, lens, hs, want));
        impr_ctx_t ctx[2];
        ray_thread_t th[2];
        int started = 0;
        for (int t = 0; t < 2; t++) {
            ctx[t] = (impr_ctx_t){ imp, strs, lens, hs, want, 0, NULL, NULL, NULL, NULL, outs + (int64_t)t * IMPR_N, 0, 0, 0 };
            if (ray_thread_create(&th[t], impr_lookups, &ctx[t]) == RAY_OK) started++;
            else break;
        }
        int64_t until = ray_profile_now_ns() + (int64_t)30 * 1000000000;
        for (int t = 0; t < started; t++)
            while (!atomic_load(&ctx[t].found) && !atomic_load(&ctx[t].done)) {
                if (ray_profile_now_ns() > until) { timeouts++; break; }
                RAY_CPU_RELAX();
            }
        int64_t p0 = -1, o0 = -1;
        bool reserved = ray_symimp_reserve(imp, 1, 8, &p0, &o0);
        int64_t ib = ray_symimp_index_bytes(imp);
        for (int t = 0; t < started; t++) ray_thread_join(th[t]);
        TEST_ASSERT_EQ_I(started, 2);
        TEST_ASSERT_TRUE(reserved);
        for (int t = 0; t < 2; t++) if (atomic_load(&ctx[t].bad) || !atomic_load(&ctx[t].found)) bad++;
        if (ib > 0) kept++; else freed++;
        ray_symimp_put(imp, p0, o0, "res0", 4);
        ray_sym_domain_release(dom);
        unlink(p);
        unlink(TMP_DOM_SYM_PATH "_imk.lk");
        int64_t c1 = ray_heap_anon_committed();
        if (round > 0) {   /* the first warms whatever stays */
            measured++;
            if (c1 - c0 >= ((int64_t)8 << 20)) {
                leaks++;
                fprintf(stderr, "  sk4 reserve round %d (%s): %lld bytes more committed after the dictionary went\n", round,
                        ib > 0 ? "kept" : "freed", (long long)(c1 - c0));
            }
        }
    }
    fprintf(stderr, "  sk4 reserve: tables kept (a lookup in flight) in %d rounds, freed at once in %d; %d of %d measured rounds leaked\n",
            kept, freed, leaks, measured);
    ray_sys_free(bufs); ray_sys_free(strs); ray_sys_free(lens); ray_sys_free(hs); ray_sys_free(want); ray_sys_free(outs);
    TEST_ASSERT_EQ_I(timeouts, 0);
    TEST_ASSERT_EQ_I(bad, 0);
    TEST_ASSERT_EQ_I(leaks, 0);
    PASS();
}
/* ---- the symfile's record index (<symfile>.ix) --------------------------- */

#define TMP_IX_PATH TMP_DOM_SYM_PATH ".ix"
#define IX_MAXN 4096

/* A test vocabulary: "" at 0, then strings of varying length, so the
 * checkpoints fall at irregular offsets; entries may be replaced. */
typedef struct {
    int64_t n;
    char    s[IX_MAXN][48];
    uint8_t l[IX_MAXN];
} ix_vocab_t;
static ix_vocab_t ix_va, ix_vb;

static void ix_gen(ix_vocab_t* v, const char* tag, int64_t n) {
    v->n = n;
    for (int64_t i = 0; i < n; i++) {
        int l = i == 0 ? 0 : snprintf(v->s[i], sizeof v->s[i], "%s_%lld_%.*s", tag,
                                       (long long)i, (int)(i % 11), "xxxxxxxxxxx");
        if (i == 0) v->s[i][0] = 0;
        v->l[i] = (uint8_t)l;
    }
}
static void ix_set(ix_vocab_t* v, int64_t i, const char* s) {
    size_t l = strlen(s);
    memcpy(v->s[i], s, l + 1);
    v->l[i] = (uint8_t)l;
}

/* The symfile bytes of a vocabulary (the format every writer leaves) and
 * the offset of each record. */
static size_t ix_strl(const ix_vocab_t* v, int64_t n, uint8_t* out, uint64_t* offs) {
    uint32_t magic = 0x4C525453U;
    memcpy(out, &magic, 4);
    memcpy(out + 4, &n, 8);
    size_t o = 12;
    for (int64_t i = 0; i < n; i++) {
        if (offs) offs[i] = o;
        uint32_t l = v->l[i];
        memcpy(out + o, &l, 4);
        memcpy(out + o + 4, v->s[i], l);
        o += 4 + l;
    }
    return o;
}

static bool ix_write(const char* path, const uint8_t* buf, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(buf, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static long ix_read(const char* path, uint8_t* buf, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}
static bool ix_exists(const char* path) { struct stat st; return stat(path, &st) == 0; }

static uint8_t ix_sym[1 << 18], ix_sym2[1 << 18], ix_buf[1 << 16], ix_buf2[1 << 16];
static uint64_t ix_offs[IX_MAXN];

static bool ix_write_vocab(const char* path, const ix_vocab_t* v, int64_t n) {
    size_t sz = ix_strl(v, n, ix_sym, ix_offs);
    return ix_write(path, ix_sym, sz);
}

/* An index (the format of domain.c: a 128-byte header, the blocks in
 * `cap` 64-byte blocks of an offset and 28 u16 lengths) of `count` entries of the image
 * `img` (record offsets `offs`) whose records end at `end`; claimed for the
 * file at `path` as it is now (its inode, modification and status-change
 * times), or for none (NULL).  Returns its size. */
#define IXH 128
#define IXS 28   /* entries in a block of 64 bytes */
static uint8_t ix_sym3[4 << 20];   /* a symfile of long strings */
static inline int64_t ix_nblk(int64_t count) { return (count + IXS - 1) / IXS; }
static inline int64_t ix_cap(int64_t count) { int64_t n = ix_nblk(count); return n + n / 4 + 64; }

static inline size_t ix_bytes(int64_t count) { return IXH + 64 * (size_t)ix_cap(count); }
static void ix_fix(uint8_t* ix) {
    uint32_t w = ray_crc32(0, ix + 16, IXH - 16);
    memcpy(ix + 12, &w, 4);
}
/* A file's modification and status-change times in nanoseconds. */
static int64_t ix_mtime_ns(const struct stat* st) {
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
    /* Darwin's POSIX namespace exposes seconds/nanoseconds separately. */
    return (int64_t)st->st_mtime * 1000000000 + st->st_mtimensec;
#elif defined(__APPLE__)
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000 + st->st_mtimespec.tv_nsec;
#else
    return (int64_t)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec;
#endif
}
static int64_t ix_ctime_ns(const struct stat* st) {
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
    return (int64_t)st->st_ctime * 1000000000 + st->st_ctimensec;
#elif defined(__APPLE__)
    return (int64_t)st->st_ctimespec.tv_sec * 1000000000 + st->st_ctimespec.tv_nsec;
#else
    return (int64_t)st->st_ctim.tv_sec * 1000000000 + st->st_ctim.tv_nsec;
#endif
}
/* Claim an index for a file's current identity, as one written for that
 * very file is. */
static void ix_stamp(uint8_t* ix, const char* path) {
    struct stat st;
    uint64_t ino = 0, stamp = 0;
    if (path && stat(path, &st) == 0) {
        ino = (uint64_t)st.st_ino;
        int64_t mt = ix_mtime_ns(&st);
        int64_t ct = ix_ctime_ns(&st);
        stamp = (uint64_t)mt ^ ((uint64_t)ct * UINT64_C(0x9E3779B97F4A7C15));
    }
    memcpy(ix + 32, &ino, 8);
    memcpy(ix + 40, &stamp, 8);
    ix_fix(ix);
}
static size_t ix_make(uint8_t* out, const uint8_t* img, const uint64_t* offs, int64_t count,
                      uint64_t end, const char* path) {
    int64_t n = ix_nblk(count), cap = ix_cap(count);
    size_t bytes = ix_bytes(count);
    memset(out, 0, bytes);
    uint32_t w = 0x58525453U; memcpy(out, &w, 4);
    w = 5; memcpy(out + 4, &w, 4);
    w = IXS; memcpy(out + 8, &w, 4);
    memcpy(out + 16, &count, 8);
    memcpy(out + 24, &end, 8);
    uint64_t last = offs[IXS * (n - 1)];
    memcpy(out + 48, &last, 8);
    w = ray_crc32(0, img + last, (size_t)(end - last)); memcpy(out + 56, &w, 4);
    uint64_t he = end - 12 < 4096 ? end : 12 + 4096;
    w = ray_crc32(0, img + 12, (size_t)(he - 12)); memcpy(out + 60, &w, 4);
    uint64_t c64 = (uint64_t)cap; memcpy(out + 72, &c64, 8);
    for (int64_t k = 0; k < n; k++) memcpy(out + IXH + 64 * (size_t)k, &offs[IXS * k], 8);
    for (int64_t i = 0; i < count; i++) {
        uint64_t l = (i + 1 < count ? offs[i + 1] : end) - offs[i] - 4;
        uint16_t l16 = l < 65535 ? (uint16_t)l : 65535;
        memcpy(out + IXH + 64 * (size_t)(i / IXS) + 8 + 2 * (size_t)(i % IXS), &l16, 2);
    }
    ix_stamp(out, path);
    return bytes;
}
/* The checkpoint of block k, and the length byte of entry i, in an index of
 * `count` entries. */
static uint8_t* ix_ck_at(uint8_t* ix, int64_t k) { return ix + IXH + 64 * (size_t)k; }
static uint16_t* ix_len_at(uint8_t* ix, int64_t count, int64_t i) {
    (void)count;
    return (uint16_t*)(ix + IXH + 64 * (size_t)(i / IXS) + 8 + 2 * (size_t)(i % IXS));
}

/* Wait until the file system's clock is past `path`'s change time: a file
 * written now gets a later one.  Some kernels move timestamps only by the
 * scheduler tick, and a rewrite in the same tick keeps them (domain.h). */
static void ix_wait_fs_tick(const char* path) {
    struct stat st, pr;
    if (stat(path, &st) != 0) return;
    const char* probe = TMP_DOM_SYM_PATH ".tick";
    for (int i = 0; i < 5000; i++) {
        FILE* f = fopen(probe, "wb");
        if (f) { fputc('t', f); fclose(f); }
        if (stat(probe, &pr) == 0 &&
            ix_ctime_ns(&pr) > ix_ctime_ns(&st))
            break;
        struct timespec ts = { 0, 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    unlink(probe);
}

/* Every entry of `dom` is the vocabulary's: first through the raw snapshot
 * (the mapped prefix; lock-free, so it checks blocks itself), then the
 * atoms, then the reverse index (a pass over the whole vocabulary). */
static bool ix_check_all(ray_sym_domain_t* dom, const ix_vocab_t* v, int64_t n) {
    if (ray_sym_domain_count(dom) != n) return false;
    ray_sym_domain_raw_t raw;
    if (!ray_sym_domain_raw_pin(dom, &raw) || raw.count > n) return false;
    for (int64_t i = 0; i < raw.count; i++) {
        size_t l = v->l[i], rl = 0;
        const char* rp = ray_sym_domain_raw_str(&raw, i, &rl);
        if (!rp || rl != l || (l && memcmp(rp, v->s[i], l) != 0)) return false;
    }
    for (int64_t i = 0; i < n; i++) {
        size_t l = v->l[i];
        ray_t* s = ray_sym_domain_str(dom, i);
        if (!s || ray_str_len(s) != l || (l && memcmp(ray_str_ptr(s), v->s[i], l) != 0))
            return false;
    }
    for (int64_t i = 1; i < n; i += 97)
        if (ray_sym_domain_find(dom, v->s[i], v->l[i]) != i) return false;
    return true;
}

static ray_sym_domain_ix_info_t ix_info(ray_sym_domain_t* dom) {
    ray_sym_domain_ix_info_t in;
    memset(&in, 0, sizeof in);
    (void)ray_sym_domain_ix_info(dom, &in);
    return in;
}

static void ix_unlink_all(void) {
    unlink(TMP_DOM_SYM_PATH);
    unlink(TMP_DOM_SYM_PATH ".lk");
    unlink(TMP_IX_PATH);
}

/* Open, check every entry, report what the index did, release. */
static bool ix_open_check(const ix_vocab_t* v, int64_t n, ray_sym_domain_ix_info_t* out) {
    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    if (!dom) return false;
    bool ok = ix_check_all(dom, v, n);
    *out = ix_info(dom);
    ray_sym_domain_release(dom);
    return ok;
}

/* The length of every entry, through the raw snapshot and the domain. */
static bool ix_check_lens(ray_sym_domain_t* dom, const ix_vocab_t* v, int64_t n) {
    ray_sym_domain_raw_t raw;
    if (!ray_sym_domain_raw_pin(dom, &raw)) return false;
    for (int64_t i = 0; i < n; i++) {
        if (i < raw.count && ray_sym_domain_raw_len(&raw, i) != v->l[i]) return false;
        if (ray_sym_domain_strlen(dom, i) != v->l[i]) return false;
    }
    return true;
}

/* An old symfile (no index) opens with one walk and leaves an index; the
 * next open takes the table from it — the lengths mapped from it — checks
 * each block on first read, and does not rewrite it. */
static test_result_t test_domain_index_build_and_use(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 1000 };
    ix_gen(&ix_va, "ix", N);
    TEST_ASSERT(ix_write_vocab(TMP_DOM_SYM_PATH, &ix_va, N), "write symfile");
    ray_sym_domain_ix_info_t in;
    TEST_ASSERT(ix_open_check(&ix_va, N, &in), "old symfile resolves");
    TEST_ASSERT_FALSE(in.from_index);
    long n1 = ix_read(TMP_IX_PATH, ix_buf, sizeof ix_buf);
    TEST_ASSERT_EQ_I(n1, (long)ix_bytes(N));
    /* what the writer wrote is what the format says, for this very file */
    size_t ib = ix_make(ix_buf2, ix_sym, ix_offs, N, ix_strl(&ix_va, N, ix_sym, ix_offs),
                        TMP_DOM_SYM_PATH);
    TEST_ASSERT_EQ_U(ib, (size_t)n1);
    TEST_ASSERT_MEM_EQ(ib, ix_buf, ix_buf2);
    TEST_ASSERT(ix_open_check(&ix_va, N, &in), "indexed open resolves");
    TEST_ASSERT_TRUE(in.from_index);
    TEST_ASSERT_TRUE(in.lens_mapped);
    TEST_ASSERT_EQ_I(in.covered, N);
    TEST_ASSERT(in.checked > 0, "blocks checked on first read");
    TEST_ASSERT_EQ_I(in.rescued, 0);
    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT(ix_check_lens(dom, &ix_va, N), "lengths from the index");
    ray_sym_domain_release(dom);
    TEST_ASSERT_EQ_I(ix_read(TMP_IX_PATH, ix_buf2, sizeof ix_buf2), n1);
    TEST_ASSERT_MEM_EQ((size_t)n1, ix_buf2, ix_buf);   /* used, not rewritten */
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* Strings of 255 bytes and longer, before the entries read in their block:
 * their length is in the index (two bytes) and no record is read for it, and
 * those of 65535 bytes and longer (the escape) take it from the record; every
 * entry and every length is the file's. */
static test_result_t test_domain_index_long_strings(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 200 };
    static const int64_t lens[] = { 254, 255, 256, 300, 1000, 4095, 65534, 65535, 70000 };
    /* the records by hand: the vocabulary type holds short strings only */
    uint64_t offs[N];
    int64_t want[N];
    size_t o = 12;
    for (int64_t i = 0; i < N; i++) {
        int64_t l = i == 0 ? 0 : (i % IXS >= 3 && i % IXS < 10) ? lens[(i / IXS + i) % 9] : i % 23;
        want[i] = l;
        offs[i] = o;
        uint32_t l32 = (uint32_t)l;
        memcpy(ix_sym3 + o, &l32, 4);
        for (int64_t c = 0; c < l; c++) ix_sym3[o + 4 + c] = (uint8_t)('a' + (i + c) % 26);
        o += 4 + (size_t)l;
    }
    uint32_t magic = 0x4C525453U;
    int64_t cnt = N;
    memcpy(ix_sym3, &magic, 4);
    memcpy(ix_sym3 + 4, &cnt, 8);
    TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH, ix_sym3, o), "symfile");
    for (int pass = 0; pass < 2; pass++) {   /* walked, then from the index */
        ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
        TEST_ASSERT_NOT_NULL(dom);
        TEST_ASSERT_EQ_I(ix_info(dom).from_index, pass == 1);
        ray_sym_domain_raw_t raw;
        TEST_ASSERT(ray_sym_domain_raw_pin(dom, &raw), "pin");
        for (int64_t i = N - 1; i >= 0; i--) {   /* the later entries of a block first */
            size_t rl = 0;
            const char* rp = ray_sym_domain_raw_str(&raw, i, &rl);
            if (!rp || (int64_t)rl != want[i] || memcmp(rp, ix_sym3 + offs[i] + 4, rl) != 0)
                FAILF("pass %d entry %lld", pass, (long long)i);
            if (ray_sym_domain_raw_len(&raw, i) != want[i] || ray_sym_domain_strlen(dom, i) != want[i])
                FAILF("pass %d length of %lld", pass, (long long)i);
        }
        TEST_ASSERT_EQ_I(ix_info(dom).rescued, 0);
        ray_sym_domain_release(dom);
    }
    long nb = ix_read(TMP_IX_PATH, ix_buf, sizeof ix_buf);
    TEST_ASSERT_EQ_I(nb, (long)ix_bytes(N));
    for (int64_t i = 0; i < N; i++)
        if (*ix_len_at(ix_buf, N, i) != (want[i] < 65535 ? want[i] : 65535)) FAILF("length %lld", (long long)i);
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* The index is taken for the very file it was written for only: the same
 * bytes written again (a new modification and status-change time), a chmod
 * (the status-change time alone), another vocabulary of the same shape —
 * each opens by one walk, every entry the file's, and the index is written
 * again for it, so the open after takes it. */
static test_result_t test_domain_index_identity(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 900 };
    ix_gen(&ix_va, "id", N);
    TEST_ASSERT(ix_write_vocab(TMP_DOM_SYM_PATH, &ix_va, N), "write symfile");
    ray_sym_domain_ix_info_t in;
    TEST_ASSERT(ix_open_check(&ix_va, N, &in), "first open");
    TEST_ASSERT(ix_open_check(&ix_va, N, &in), "indexed open");
    TEST_ASSERT_TRUE(in.from_index);
    ix_vb = ix_va;
    for (int64_t i = 400; i < 420; i++) ix_vb.s[i][0] = 'J';
    for (int v = 0; v < 3; v++) {
        ix_wait_fs_tick(TMP_IX_PATH);
        const ix_vocab_t* now = v == 2 ? &ix_vb : &ix_va;
        if (v == 0) TEST_ASSERT(ix_write_vocab(TMP_DOM_SYM_PATH, &ix_va, N), "the same bytes again");
        if (v == 1) TEST_ASSERT_EQ_I(chmod(TMP_DOM_SYM_PATH, 0640), 0);
        if (v == 2) TEST_ASSERT(ix_write_vocab(TMP_DOM_SYM_PATH, &ix_vb, N), "same-shape vocabulary");
        if (!ix_open_check(now, N, &in)) FAILF("case %d: does not resolve", v);
        if (in.from_index) FAILF("case %d: the index was taken", v);
        if (!ix_open_check(now, N, &in) || !in.from_index) FAILF("case %d: not rewritten", v);
        TEST_ASSERT_EQ_I(in.rescued, 0);
    }
    TEST_ASSERT_EQ_I(chmod(TMP_DOM_SYM_PATH, 0644), 0);
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* An index that fails a check made at open is not used: the records are
 * walked and the index written again for the file.  One per check. */
static test_result_t test_domain_index_rejects_bad(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 777, CASES = 13 };
    ix_gen(&ix_va, "rj", N);
    size_t sz = ix_strl(&ix_va, N, ix_sym, ix_offs);
    static uint8_t good[1 << 14];
    for (int v = 0; v < CASES; v++) {
        TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH, ix_sym, sz), "write symfile");
        size_t gb = ix_make(good, ix_sym, ix_offs, N, sz, TMP_DOM_SYM_PATH);
        size_t len = gb;
        memcpy(ix_buf, good, gb);
        uint64_t u;
        int64_t c;
        switch (v) {
        case 0: ix_buf[20] ^= 1; break;                                              /* header crc */
        case 1: ix_buf[4] = 3; ix_fix(ix_buf); break;                                /* earlier format */
        case 2: u = ix_offs[IXS * 24 + 1]; memcpy(ix_buf + 48, &u, 8); ix_fix(ix_buf); break;     /* last block moved */
        case 3: ix_buf[56] ^= 1; ix_fix(ix_buf); break;                              /* last block's crc */
        case 4: ix_buf[60] ^= 1; ix_fix(ix_buf); break;                              /* first records' crc */
        case 5: len = gb - 5; break;                                                 /* lengths cut short */
        case 6: c = N - 1; memcpy(ix_buf + 16, &c, 8); ix_fix(ix_buf); break;        /* count */
        case 7: u = sz - 1; memcpy(ix_buf + 24, &u, 8); ix_fix(ix_buf); break;      /* end */
        case 8: ix_buf[32] ^= 1; ix_fix(ix_buf); break;                             /* inode */
        case 9: ix_buf[40] ^= 1; ix_fix(ix_buf); break;                             /* times */
        case 10: memset(ix_buf + 32, 0, 16); ix_fix(ix_buf); break;                  /* claimed for none */
        case 11: u = 3; memcpy(ix_buf + 72, &u, 8); ix_fix(ix_buf); break;           /* fewer slots than blocks */
        case 12: len = 40; break;                                                    /* the header cut short */
        }
        TEST_ASSERT(ix_write(TMP_IX_PATH, ix_buf, len), "write bad index");
        ray_sym_domain_ix_info_t in;
        if (!ix_open_check(&ix_va, N, &in)) FAILF("case %d: does not resolve", v);
        if (in.from_index) FAILF("case %d: the index was used", v);
        if (ix_read(TMP_IX_PATH, ix_buf2, sizeof ix_buf2) != (long)gb ||
            memcmp(ix_buf2, good, gb) != 0)
            FAILF("case %d: not written again", v);
    }
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* Damage to the checkpoint section is not looked for at open (it is not
 * read): the index is taken, a block away from the damage reads as before,
 * and the first read of a block it touches finds that the block does not
 * check — the domain walks the records, answers the entry from the file,
 * and writes the index again.  Every symbol read is the vocabulary's. */
static test_result_t test_domain_index_checkpoint_damage(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 777, CASES = 5, CLEAN = 321 };
    static uint8_t good[1 << 14];
    static const int64_t first_read[CASES] = { 100, 100, 5, 100, 70 };
    for (int v = 0; v < CASES; v++) {
        ix_gen(&ix_va, "ck", N);
        if (v >= 3) {   /* every record the same width: a shifted checkpoint is a record boundary */
            for (int64_t i = 1; i < N; i++) {
                char w[8];
                snprintf(w, sizeof w, "s%05lld", (long long)i);
                ix_set(&ix_va, i, w);
            }
        }
        size_t sz = ix_strl(&ix_va, N, ix_sym, ix_offs);
        TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH, ix_sym, sz), "write symfile");
        size_t gb = ix_make(good, ix_sym, ix_offs, N, sz, TMP_DOM_SYM_PATH);
        memcpy(ix_buf, good, gb);
        uint64_t u = 16;
        switch (v) {
        case 0: ix_ck_at(ix_buf, 3)[0] ^= 0x10; break;                               /* a byte flipped */
        case 1: memcpy(ix_ck_at(ix_buf, 3), ix_ck_at(ix_buf, 2), 8); break;         /* not increasing */
        case 2: memcpy(ix_ck_at(ix_buf, 0), &u, 8); break;                           /* not at the first record */
        case 3: case 4:   /* one record on: block 3 alone, then blocks 3 and 4 together */
            for (int k = 3; k <= v; k++) {
                memcpy(&u, ix_ck_at(ix_buf, k), 8);
                u += 10;
                memcpy(ix_ck_at(ix_buf, k), &u, 8);
            }
            break;
        }
        TEST_ASSERT(ix_write(TMP_IX_PATH, ix_buf, gb), "write damaged index");
        ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
        TEST_ASSERT_NOT_NULL(dom);
        ray_sym_domain_ix_info_t in = ix_info(dom);
        if (!in.from_index || in.rescued != 0 || in.checked != 0)
            FAILF("case %d: not taken as is at open", v);
        ray_t* s = ray_sym_domain_str(dom, CLEAN);
        if (!s || ray_str_len(s) != (size_t)ix_va.l[CLEAN] ||
            memcmp(ray_str_ptr(s), ix_va.s[CLEAN], ix_va.l[CLEAN]) != 0)
            FAILF("case %d: block away from the damage", v);
        in = ix_info(dom);
        if (in.rescued != 0) FAILF("case %d: rescued before the damaged block was read", v);
        if (ix_read(TMP_IX_PATH, ix_buf2, sizeof ix_buf2) != (long)gb ||
            memcmp(ix_buf2, ix_buf, gb) != 0)
            FAILF("case %d: index rewritten early", v);
        int64_t p = first_read[v];
        s = ray_sym_domain_str(dom, p);
        if (!s || ray_str_len(s) != (size_t)ix_va.l[p] ||
            memcmp(ray_str_ptr(s), ix_va.s[p], ix_va.l[p]) != 0)
            FAILF("case %d: wrong symbol at the damaged block", v);
        in = ix_info(dom);
        if (in.rescued != 1) FAILF("case %d: rescued %lld at first read", v, (long long)in.rescued);
        TEST_ASSERT_FALSE(in.corrupt);
        if (ix_read(TMP_IX_PATH, ix_buf2, sizeof ix_buf2) != (long)gb ||
            memcmp(ix_buf2, good, gb) != 0)
            FAILF("case %d: index not rewritten", v);
        if (!ix_check_all(dom, &ix_va, N)) FAILF("case %d: a wrong symbol", v);
        ray_sym_domain_release(dom);
        if (!ix_open_check(&ix_va, N, &in) || !in.from_index || in.rescued != 0)
            FAILF("case %d: rewritten index not used", v);
    }
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* An index claimed for the file that nevertheless does not describe it — a
 * checkpoint moved, a length byte one short, two length bytes changed by +7
 * and -7 (their block still adds up) — is found out on the first read of a
 * block or record it gets wrong: the domain walks the records once and
 * answers every entry correctly; the index is written again. */
static test_result_t test_domain_index_stale_caught(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 2000 };
    ix_gen(&ix_va, "st", N);
    ix_set(&ix_va, 645, "LONGNAME_qqqqqqqqqqqqqqqqqqqq");
    ix_set(&ix_va, 650, "mmmmm");
    size_t sz = ix_strl(&ix_va, N, ix_sym, ix_offs);
    for (int v = 0; v < 3; v++) {
        TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH, ix_sym, sz), "write symfile");
        size_t ib = ix_make(ix_buf, ix_sym, ix_offs, N, sz, TMP_DOM_SYM_PATH);
        if (v == 0) {   /* checkpoint 20 moved to entry 641 */
            memcpy(ix_ck_at(ix_buf, 20), &ix_offs[641], 8);
            ix_fix(ix_buf);
        }
        if (v == 1) *ix_len_at(ix_buf, N, 645) -= 1;   /* its block falls short of the next */
        if (v == 2) { *ix_len_at(ix_buf, N, 645) -= 7; *ix_len_at(ix_buf, N, 650) += 7; }
        TEST_ASSERT(ix_write(TMP_IX_PATH, ix_buf, ib), "write index");
        ray_sym_domain_ix_info_t in;
        if (v == 1) {
            /* the length of the entry whose byte is off, read first and
             * without its record: its block does not check, so the length
             * is the file's */
            ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
            TEST_ASSERT_NOT_NULL(dom);
            TEST_ASSERT_EQ_I(ray_sym_domain_strlen(dom, 645), ix_va.l[645]);
            ray_sym_domain_release(dom);
            TEST_ASSERT(ix_write(TMP_IX_PATH, ix_buf, ib), "write index again");
        }
        if (!ix_open_check(&ix_va, N, &in)) FAILF("case %d: does not resolve", v);
        TEST_ASSERT_TRUE(in.from_index);
        if (in.rescued != 1) FAILF("case %d: rescued %lld", v, (long long)in.rescued);
        TEST_ASSERT_FALSE(in.corrupt);
        TEST_ASSERT(ix_read(TMP_IX_PATH, ix_buf2, sizeof ix_buf2) == (long)ib &&
                    memcmp(ix_buf2, ix_buf, ib) != 0, "index rewritten");
        TEST_ASSERT(ix_open_check(&ix_va, N, &in), "rewritten index resolves");
        TEST_ASSERT_TRUE(in.from_index);
        TEST_ASSERT_EQ_I(in.rescued, 0);
    }
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* A symfile of IXL_N records in `img` — a quarter of them 254 to 70000 bytes
 * (those of 65535 and longer the escape), one a multi-byte UTF-8 string —
 * with their lengths in `want` and offsets in `offs`; returns its size. */
enum { IXL_N = 200 };
static size_t ix_long_image(uint8_t* img, uint64_t* offs, int64_t* want) {
    static const int64_t lens[] = { 254, 255, 256, 300, 1000, 4095, 65534, 65535, 70000 };
    size_t o = 12;
    for (int64_t i = 0; i < IXL_N; i++) {
        int64_t l = i == 0 ? 0 : (i % 8 >= 2 && i % 8 < 4) ? lens[(i / 8) % 9] : i % 23;
        want[i] = l;
        offs[i] = o;
        uint32_t l32 = (uint32_t)l;
        memcpy(img + o, &l32, 4);
        for (int64_t c = 0; c < l; c++)
            img[o + 4 + c] = (uint8_t)(i == 77 ? (c & 1 ? 0xA9 : 0xC3) : 'a' + (i + c) % 26);
        o += 4 + (size_t)l;
    }
    uint32_t magic = 0x4C525453U;
    int64_t cnt = IXL_N;
    memcpy(img, &magic, 4);
    memcpy(img + 4, &cnt, 8);
    return o;
}

/* The length of every id through the domain (any order), against `want`;
 * with `only_right`, an id may also be refused (-1) but never wrong. */
static bool ix_long_lens_ok(ray_sym_domain_t* dom, const int64_t* want, bool only_right) {
    for (int64_t j = 0; j < IXL_N; j++) {
        int64_t i = (j * 37) % IXL_N, l = ray_sym_domain_strlen(dom, i);
        if (l != want[i] && !(only_right && l == -1)) return false;
    }
    return true;
}

/* On a domain just opened, every record's header overwritten with 0xFF —
 * except the empty first record, and the escape entries' (65535 bytes and
 * longer: their header is their length) — and every string's bytes: each
 * length, of the short entries and of those of 255 to 65534 bytes alike,
 * comes out exact, in any order, from the index alone, and no read is
 * counted corrupt.  Reading the string of such an entry finds the record
 * gone and counts. */
static test_result_t test_domain_index_cold_long_no_headers(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    int64_t want[IXL_N];
    size_t sz = ix_long_image(ix_sym3, ix_offs, want);
    int64_t nlong = 0, nesc = 0, lg = -1;
    for (int64_t i = 1; i < IXL_N; i++) {
        nlong += want[i] >= 255 && want[i] < 65535;
        nesc += want[i] >= 65535;
        if (lg < 0 && want[i] >= 255 && want[i] < 65535) lg = i;
        if (want[i] >= 65535) memset(ix_sym3 + ix_offs[i] + 4, 0xFF, (size_t)want[i]);   /* the escape keeps its header */
        else memset(ix_sym3 + ix_offs[i], 0xFF, 4 + (size_t)want[i]);
    }
    TEST_ASSERT(nlong > 15 && nesc > 3, "long and escape entries");
    TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH, ix_sym3, sz), "write symfile");
    size_t ib = ix_make(ix_buf, ix_sym3, ix_offs, IXL_N, sz, TMP_DOM_SYM_PATH);
    TEST_ASSERT(ix_write(TMP_IX_PATH, ix_buf, ib), "write index");
    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_TRUE(ix_info(dom).from_index);
    TEST_ASSERT_EQ_I(ix_info(dom).checked, 0);
    uint64_t c0 = ray_sym_domain_corruptions();
    TEST_ASSERT(ix_long_lens_ok(dom, want, false), "exact lengths");
    TEST_ASSERT_EQ_U(ray_sym_domain_corruptions(), c0);
    TEST_ASSERT_FALSE(ix_info(dom).corrupt);
    TEST_ASSERT_EQ_I(ix_info(dom).rescued, 0);
    TEST_ASSERT(ix_info(dom).checked > 0, "blocks checked by the length reads");
    ray_sym_domain_raw_t raw;
    TEST_ASSERT(ray_sym_domain_raw_pin(dom, &raw), "pin");
    size_t rl = 7;
    TEST_ASSERT_NULL(ray_sym_domain_raw_str(&raw, lg, &rl));
    TEST_ASSERT(ray_sym_domain_corruptions() > c0, "string read counted");
    ray_sym_domain_release(dom);
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* The escape: an entry of 65535 bytes or longer has its length from its record
 * header — with it intact its block checks and the length is exact (the
 * strings are read intact too); with it damaged (read in the block's check)
 * the block does not check, the walk fails, and the domain is corrupt: no
 * length is ever wrong.  A length of a 255-or-longer entry off in the index
 * by one is found out by its block's sum, the domain walks, and every
 * length is the file's. */
static test_result_t test_domain_index_escape_and_long_damage(void) {
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    int64_t want[IXL_N];
    size_t sz = ix_long_image(ix_sym3, ix_offs, want);
    int64_t esc = 0, lg = 0;
    while (want[esc] < 65535) esc++;
    while (want[lg] < 255 || want[lg] >= 65535) lg++;
    for (int v = 0; v < 4; v++) {
        ix_unlink_all();
        uint32_t keep, bad = 100;
        memcpy(&keep, ix_sym3 + ix_offs[esc], 4);
        if (v == 1) memcpy(ix_sym3 + ix_offs[esc], &bad, 4);   /* the escape's header: gone */
        TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH, ix_sym3, sz), "write symfile");
        size_t ib = ix_make(ix_buf, ix_sym3, ix_offs, IXL_N, sz, TMP_DOM_SYM_PATH);
        if (v == 1) memcpy(ix_sym3 + ix_offs[esc], &keep, 4);
        if (v == 2) *ix_len_at(ix_buf, IXL_N, lg) -= 1;         /* a long length off by one */
        if (v == 3) *ix_len_at(ix_buf, IXL_N, esc) = 300;       /* the escape not marked */
        TEST_ASSERT(ix_write(TMP_IX_PATH, ix_buf, ib), "write index");
        uint64_t c0 = ray_sym_domain_corruptions();
        ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
        TEST_ASSERT_NOT_NULL(dom);
        if (!ix_long_lens_ok(dom, want, v == 1)) FAILF("case %d: a wrong length", v);
        ray_sym_domain_ix_info_t in = ix_info(dom);
        if (v == 0) {
            TEST_ASSERT_EQ_U(ray_sym_domain_corruptions(), c0);
            TEST_ASSERT_EQ_I(in.rescued, 0);
            TEST_ASSERT_FALSE(in.corrupt);
        } else if (v == 1) {
            TEST_ASSERT_TRUE(in.corrupt);
            TEST_ASSERT_EQ_I(ray_sym_domain_strlen(dom, esc), -1);
        } else {
            if (in.rescued < 1) FAILF("case %d: not found out", v);
            TEST_ASSERT_FALSE(in.corrupt);
        }
        ray_sym_domain_release(dom);
    }
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* A page of the file lost behind an index claimed for it (a torn write, a
 * bad sector): the open does not read it, but the first read of an entry
 * there finds its record does not say its length and the walk does not fit
 * — the entry is not answered, the domain is corrupt and the count of such
 * reads grows; an entry of a block read before still is.  A pass over the
 * whole vocabulary (the runtime LUT) fails the same way. */
static test_result_t test_domain_index_torn_file(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 3000 };
    ix_gen(&ix_va, "tf", N);
    size_t sz = ix_strl(&ix_va, N, ix_sym, ix_offs);
    size_t pg = (sz / 2) & ~(size_t)4095;
    int64_t mid = 0;
    while (ix_offs[mid] < pg + 100) mid++;
    for (int pass = 0; pass < 2; pass++) {
        memcpy(ix_sym2, ix_sym, sz);
        memset(ix_sym2 + pg, 0, 4096);
        TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH, ix_sym2, sz), "write torn symfile");
        /* the index of the intact layout, claimed for the torn file (its first
         * records and last block are intact) */
        size_t ib = ix_make(ix_buf, ix_sym2, ix_offs, N, sz, TMP_DOM_SYM_PATH);
        TEST_ASSERT(ix_write(TMP_IX_PATH, ix_buf, ib), "write index");
        ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
        TEST_ASSERT_NOT_NULL(dom);
        TEST_ASSERT_TRUE(ix_info(dom).from_index);
        ray_t* s1 = ray_sym_domain_str(dom, 1);
        TEST_ASSERT_NOT_NULL(s1);
        uint64_t c0 = ray_sym_domain_corruptions();
        if (pass == 0) {
            TEST_ASSERT_NULL(ray_sym_domain_str(dom, mid));
            ray_sym_domain_raw_t raw;
            TEST_ASSERT(ray_sym_domain_raw_pin(dom, &raw), "pin");
            size_t rl = 7;
            TEST_ASSERT_NULL(ray_sym_domain_raw_str(&raw, mid, &rl));
            TEST_ASSERT_EQ_U(rl, 0);
            TEST_ASSERT_EQ_I(ray_sym_domain_strlen(dom, mid), -1);
        } else {
            TEST_ASSERT_NULL(ray_sym_domain_runtime_lut(dom));
        }
        TEST_ASSERT(ray_sym_domain_corruptions() > c0, "counted");
        TEST_ASSERT_TRUE(ix_info(dom).corrupt);
        TEST_ASSERT_EQ_PTR(ray_sym_domain_str(dom, 1), s1);
        ray_sym_domain_release(dom);
    }
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* A length is read from the index and the record is left alone: with the
 * record header of one entry overwritten (the index byte intact), its
 * length still comes out of the index, no read is counted corrupt, and no
 * other entry is affected; reading the same entry's string finds the record
 * does not say what the index does — it is not answered and counts.  The
 * bytes of another entry's string overwritten in place change what its
 * string reads as and nothing about its length. */
static test_result_t test_domain_index_len_skips_record(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 3000 };
    ix_gen(&ix_va, "ls", N);
    size_t sz = ix_strl(&ix_va, N, ix_sym, ix_offs);
    const int64_t hd = 1500, by = 2200;
    TEST_ASSERT(ix_va.l[by] > 0, "entry with bytes");
    memcpy(ix_sym2, ix_sym, sz);
    uint32_t bad = 4000;
    memcpy(ix_sym2 + ix_offs[hd], &bad, 4);                      /* its header only */
    memset(ix_sym2 + ix_offs[by] + 4, '#', (size_t)ix_va.l[by]); /* its bytes only */
    TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH, ix_sym2, sz), "write symfile");
    size_t ib = ix_make(ix_buf, ix_sym, ix_offs, N, sz, TMP_DOM_SYM_PATH);
    TEST_ASSERT(ix_write(TMP_IX_PATH, ix_buf, ib), "write index");
    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_TRUE(ix_info(dom).from_index);
    ray_sym_domain_raw_t raw;
    TEST_ASSERT(ray_sym_domain_raw_pin(dom, &raw), "pin");
    uint64_t c0 = ray_sym_domain_corruptions();
    TEST_ASSERT_EQ_I(ray_sym_domain_raw_len(&raw, hd), ix_va.l[hd]);
    TEST_ASSERT_EQ_I(ray_sym_domain_raw_len(&raw, by), ix_va.l[by]);
    TEST_ASSERT_EQ_I(ray_sym_domain_strlen(dom, hd), ix_va.l[hd]);
    TEST_ASSERT_EQ_U(ray_sym_domain_corruptions(), c0);
    TEST_ASSERT_FALSE(ix_info(dom).corrupt);
    size_t rl = 0;
    const char* rp = ray_sym_domain_raw_str(&raw, by, &rl);
    TEST_ASSERT_NOT_NULL(rp);
    TEST_ASSERT_EQ_U(rl, (size_t)ix_va.l[by]);
    for (size_t c = 0; c < rl; c++) TEST_ASSERT(rp[c] == '#', "bytes are the file's");
    TEST_ASSERT_EQ_U(ray_sym_domain_corruptions(), c0);
    TEST_ASSERT_NULL(ray_sym_domain_raw_str(&raw, hd, &rl));
    TEST_ASSERT(ray_sym_domain_corruptions() > c0, "string read counted");
    TEST_ASSERT_EQ_I(ray_sym_domain_strlen(dom, hd), -1);   /* once corrupt, no length either */
    ray_sym_domain_release(dom);
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* On a domain just opened (no earlier read), the string bytes of every record
 * overwritten, and the headers of the odd short ones (the index intact): the
 * length of every id comes
 * out right, in any order, and no read is counted corrupt — the lengths
 * come from the index and its blocks' own check, which reads no string
 * bytes.  The strings read at the same ids are the overwritten ones. */
static test_result_t test_domain_index_cold_len_no_bytes(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 3000 };
    ix_gen(&ix_va, "cl", N);
    size_t sz = ix_strl(&ix_va, N, ix_sym, ix_offs);
    memcpy(ix_sym2, ix_sym, sz);
    for (int64_t i = 0; i < N; i++)
    {
        memset(ix_sym2 + ix_offs[i] + 4, '#', (size_t)ix_va.l[i]);
        if (i % 2) memset(ix_sym2 + ix_offs[i], 0xFF, 4);   /* odd short records: header too */
    }
    TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH, ix_sym2, sz), "write symfile");
    size_t ib = ix_make(ix_buf, ix_sym2, ix_offs, N, sz, TMP_DOM_SYM_PATH);
    TEST_ASSERT(ix_write(TMP_IX_PATH, ix_buf, ib), "write index");
    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_TRUE(ix_info(dom).from_index);
    TEST_ASSERT_EQ_I(ix_info(dom).checked, 0);   /* cold: no block checked yet */
    uint64_t c0 = ray_sym_domain_corruptions();
    for (int64_t j = 0; j < N; j++) {
        int64_t i = (j * 1237) % N;
        if (ray_sym_domain_strlen(dom, i) != ix_va.l[i]) FAILF("length of %lld", (long long)i);
    }
    TEST_ASSERT_EQ_U(ray_sym_domain_corruptions(), c0);
    TEST_ASSERT_FALSE(ix_info(dom).corrupt);
    TEST_ASSERT(ix_info(dom).checked > 0, "blocks checked by the length reads");
    ray_sym_domain_raw_t raw;
    TEST_ASSERT(ray_sym_domain_raw_pin(dom, &raw), "pin");
    size_t rl = 0;
    const char* rp = ray_sym_domain_raw_str(&raw, 1000, &rl);
    TEST_ASSERT_NOT_NULL(rp);
    TEST_ASSERT_EQ_U(rl, (size_t)ix_va.l[1000]);
    for (size_t c = 0; c < rl; c++) TEST_ASSERT(rp[c] == '#', "string read is the file's bytes");
    TEST_ASSERT_NULL(ray_sym_domain_raw_str(&raw, 1001, &rl));   /* header overwritten: a string read detects it */
    TEST_ASSERT(ray_sym_domain_corruptions() > c0, "string read counted");
    ray_sym_domain_release(dom);
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* A file whose records do not fill it, with no index of its own — torn, cut
 * short, bytes after the last record: the open walks it and opens the
 * domain corrupt — no entry is read (the walk cannot tell which of the
 * records before the failure are good: a lost page reads as empty ones),
 * every read is counted — and writes no index for it; a flush of it is
 * refused. */
static test_result_t test_domain_index_torn_open(void) {
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 3000 };
    ix_gen(&ix_va, "to", N + 1);
    size_t sz = ix_strl(&ix_va, N, ix_sym, ix_offs);
    size_t pg = (sz / 2) & ~(size_t)4095;
    int64_t mid = 0;
    while (ix_offs[mid] < pg + 100) mid++;
    for (int v = 0; v < 3; v++) {
        ix_unlink_all();
        memcpy(ix_sym2, ix_sym, sz);
        size_t len = sz;
        if (v == 0) memset(ix_sym2 + pg, 0, 4096);
        if (v == 1) len = sz - 3;
        if (v == 2) { memset(ix_sym2 + sz, 0, 5); len = sz + 5; }
        TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH, ix_sym2, len), "write the file");
        ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
        if (!dom) FAILF("case %d: not opened", v);
        ray_sym_domain_ix_info_t in = ix_info(dom);
        TEST_ASSERT_FALSE(in.from_index);
        TEST_ASSERT_TRUE(in.corrupt);
        uint64_t c0 = ray_sym_domain_corruptions();
        bool answered = ray_sym_domain_str(dom, 1) || ray_sym_domain_str(dom, v == 0 ? mid : N - 1) ||
                        ray_sym_domain_strlen(dom, 1) >= 0;
        TEST_ASSERT(ray_sym_domain_corruptions() >= c0 + 3, "every read counted");
        ray_sym_domain_release(dom);
        if (answered) FAILF("case %d: an entry of a torn file answered", v);
        dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
        TEST_ASSERT_NOT_NULL(dom);
        TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, false), RAY_ERR_CORRUPT);
        TEST_ASSERT_EQ_I(ray_sym_domain_flush_append(dom, false), RAY_ERR_CORRUPT);
        ray_sym_domain_release(dom);
        if (ix_exists(TMP_IX_PATH)) FAILF("case %d: an index written for a torn file", v);
    }
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* Two torn symfiles: each is reported on stderr by one line, the first time
 * a read of it fails, with its path last; every later failed read of either
 * leaves stderr alone, and the error names the file that read was of. */
static test_result_t test_domain_corrupt_names_file(void) {
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 3000 };
    static const char* const P[2] = { TMP_DOM_SYM_PATH, "/tmp/rayforce_test_domain_other" };
    static const char* const ERRF = TMP_DOM_SYM_PATH ".err";
    ix_gen(&ix_va, "nm", N + 1);
    size_t sz = ix_strl(&ix_va, N, ix_sym, ix_offs);
    size_t pg = (sz / 2) & ~(size_t)4095;
    memset(ix_sym + pg, 0, 4096);
    ix_unlink_all();
    unlink(P[1]); unlink(ERRF);
    TEST_ASSERT(ix_write(P[0], ix_sym, sz), "write torn file a");
    TEST_ASSERT(ix_write(P[1], ix_sym, sz), "write torn file b");
    fflush(stderr);
    int keep = dup(2);
    int ef = open(ERRF, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    TEST_ASSERT(keep >= 0 && ef >= 0, "capture stderr");
    dup2(ef, 2);
    close(ef);
    ray_sym_domain_t* dom[2] = { ray_sym_domain_open(P[0]), ray_sym_domain_open(P[1]) };
    bool opened = dom[0] && dom[1];
    bool named[6] = { false };
    char msg[6][512];
    memset(msg, 0, sizeof msg);
    for (int r = 0; opened && r < 6; r++) {   /* a, b, b, a, a, b */
        int w = (r == 0 || r == 3 || r == 4) ? 0 : 1;
        (void)ray_sym_domain_str(dom[w], 1);
        (void)ray_sym_domain_strlen(dom[w], 2);
        ray_t* e = ray_sym_domain_corrupt_error();
        const char* m = ray_error_msg();
        named[r] = m && strstr(m, P[w]) != NULL && strstr(m, P[1 - w]) == NULL;
        if (m) snprintf(msg[r], sizeof msg[r], "%s", m);
        ray_error_free(e);
    }
    fflush(stderr);
    dup2(keep, 2);
    close(keep);
    long n = ix_read(ERRF, ix_buf2, sizeof ix_buf2 - 1);
    if (dom[0]) ray_sym_domain_release(dom[0]);
    if (dom[1]) ray_sym_domain_release(dom[1]);
    unlink(P[1]);
    unlink(ERRF);
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    TEST_ASSERT(opened, "both opened");
    for (int r = 0; r < 6; r++)
        if (!named[r]) FAILF("read %d: the error names the wrong file (%s)", r, msg[r]);
    TEST_ASSERT(n > 0, "stderr read");
    ix_buf2[n] = 0;
    int lines = 0;
    for (long i = 0; i < n; i++) lines += ix_buf2[i] == '\n';
    if (lines != 2) FAILF("%d stderr lines for 2 torn files", lines);
    for (int w = 0; w < 2; w++) {
        char tail[128];
        snprintf(tail, sizeof tail, ": %s\n", P[w]);
        if (!strstr((const char*)ix_buf2, tail)) FAILF("file %d: no line ends with its path", w);
    }
    PASS();
}

/* Records appended by a writer that left the index as it was (an older
 * binary, another tool): the file is not the one the index was written for
 * (its size and times moved), so the open walks it and writes the index
 * again; the open after takes it whole. */
static test_result_t test_domain_index_append_unindexed(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 500, M = 300 };
    ix_gen(&ix_va, "au", N + M);
    TEST_ASSERT(ix_write_vocab(TMP_DOM_SYM_PATH, &ix_va, N), "write symfile");
    ray_sym_domain_ix_info_t in;
    TEST_ASSERT(ix_open_check(&ix_va, N, &in), "writes the index");
    ix_wait_fs_tick(TMP_IX_PATH);
    TEST_ASSERT(ix_write_vocab(TMP_DOM_SYM_PATH, &ix_va, N + M), "appended behind the index");
    TEST_ASSERT(ix_open_check(&ix_va, N + M, &in), "walked");
    TEST_ASSERT_FALSE(in.from_index);
    TEST_ASSERT(ix_open_check(&ix_va, N + M, &in), "the new index taken");
    TEST_ASSERT_TRUE(in.from_index);
    TEST_ASSERT_EQ_I(in.covered, N + M);
    TEST_ASSERT_EQ_I(in.rescued, 0);
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* The writers' indexes describe the files they wrote: a full flush, appends
 * (some durable) and the import dictionary's final sync each leave an index
 * the next open takes whole — its lengths those of the strings — with no
 * block failing its check. */
static test_result_t test_domain_index_writers(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 700, M = 333 };
    ix_gen(&ix_va, "wr", N + M);
    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    for (int64_t i = 1; i < N; i++)
        TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, ix_va.s[i], ix_va.l[i]), i);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, true), RAY_OK);
    ray_sym_domain_release(dom);
    ray_sym_domain_ix_info_t in;
    TEST_ASSERT(ix_open_check(&ix_va, N, &in), "after a full flush");
    TEST_ASSERT_TRUE(in.from_index);
    TEST_ASSERT_EQ_I(in.covered, N);
    TEST_ASSERT_EQ_I(in.rescued, 0);

    dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    for (int64_t i = N; i < N + M; i++) {
        TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, ix_va.s[i], ix_va.l[i]), i);
        if (i % 50 == 0) TEST_ASSERT_EQ_I(ray_sym_domain_flush_append(dom, false), RAY_OK);
    }
    TEST_ASSERT_EQ_I(ray_sym_domain_flush_append(dom, true), RAY_OK);
    ray_sym_domain_release(dom);
    TEST_ASSERT(ix_open_check(&ix_va, N + M, &in), "after appends");
    TEST_ASSERT_TRUE(in.from_index);
    TEST_ASSERT_EQ_I(in.covered, N + M);
    TEST_ASSERT_EQ_I(in.rescued, 0);
    dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT(ix_check_lens(dom, &ix_va, N + M), "the lengths the appends wrote");
    ray_sym_domain_release(dom);
    ix_unlink_all();

    const char* p = TMP_DOM_SYM_PATH "_ixi";
    char pix[256];
    snprintf(pix, sizeof pix, "%s.ix", p);
    unlink(p);
    unlink(pix);
    dom = ray_sym_domain_create_import(p);
    if (dom) {
        for (int64_t i = 1; i < N; i++)
            TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, ix_va.s[i], ix_va.l[i]), i);
        TEST_ASSERT_EQ_I(ray_sym_domain_flush(dom, true), RAY_OK);
        ray_sym_domain_release(dom);
        ray_sym_domain_t* re = ray_sym_domain_open(p);
        TEST_ASSERT_NOT_NULL(re);
        TEST_ASSERT(ix_check_all(re, &ix_va, N), "after the import's sync");
        TEST_ASSERT(ix_check_lens(re, &ix_va, N), "the import's lengths");
        in = ix_info(re);
        TEST_ASSERT_TRUE(in.from_index);
        TEST_ASSERT_EQ_I(in.covered, N);
        TEST_ASSERT_EQ_I(in.rescued, 0);
        ray_sym_domain_release(re);
    }
    unlink(p);
    unlink(pix);
    char plk[256];
    snprintf(plk, sizeof plk, "%s.lk", p);
    unlink(plk);
    unsetenv("RAY_SYM_INDEX_MIN");
    PASS();
}

/* Every flush brings the index up to the file, in place while its slots
 * last: the new blocks' checkpoints and the new entries' lengths are
 * written into it (the index keeps its inode), so the next open walks
 * nothing; past its slots it is written whole, with room again. */
static test_result_t test_domain_index_refresh_in_place(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 1000, M = 100, L = 2990 };   /* 36 blocks, 109 slots; then 146 blocks */
    ix_gen(&ix_va, "rp", N + M + L);
    TEST_ASSERT(ix_write_vocab(TMP_DOM_SYM_PATH, &ix_va, N), "write symfile");
    ray_sym_domain_ix_info_t in;
    TEST_ASSERT(ix_open_check(&ix_va, N, &in), "first open writes the index");
    struct stat s0, s1;
    TEST_ASSERT_EQ_I(stat(TMP_IX_PATH, &s0), 0);
    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    for (int64_t i = N; i < N + M; i++)
        TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, ix_va.s[i], ix_va.l[i]), i);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush_append(dom, false), RAY_OK);
    TEST_ASSERT_EQ_I(stat(TMP_IX_PATH, &s1), 0);
    TEST_ASSERT_EQ_U(s1.st_ino, s0.st_ino);   /* written into, not rewritten */
    TEST_ASSERT_EQ_I(s1.st_size, (long)ix_bytes(N));
    for (int64_t i = N + M; i < N + M + L; i++)
        TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, ix_va.s[i], ix_va.l[i]), i);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush_append(dom, false), RAY_OK);
    ray_sym_domain_release(dom);
    TEST_ASSERT_EQ_I(stat(TMP_IX_PATH, &s1), 0);
    TEST_ASSERT_EQ_I(s1.st_size, (long)ix_bytes(N + M + L));   /* past its slots: whole */
    TEST_ASSERT(ix_open_check(&ix_va, N + M + L, &in), "after the appends");
    TEST_ASSERT_TRUE(in.from_index);
    TEST_ASSERT_EQ_I(in.covered, N + M + L);
    TEST_ASSERT_EQ_I(in.rescued, 0);
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* Every writer that makes a new file under a name first removes the index
 * a file of that name left; and an index write sweeps the temporary files
 * of writers that no longer run. */
static test_result_t test_domain_index_new_file_removes(void) {
    ix_unlink_all();
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    ix_gen(&ix_va, "nf", 64);
    size_t sz = ix_strl(&ix_va, 64, ix_sym, ix_offs);
    size_t ib = ix_make(ix_buf, ix_sym, ix_offs, 64, sz, NULL);
    for (int w = 0; w < 4; w++) {
        ix_unlink_all();
        TEST_ASSERT(ix_write(TMP_IX_PATH, ix_buf, ib), "an index left behind");
        if (w <= 1) {
            ray_sym_domain_t* dom = ray_sym_domain_open_or_create(TMP_DOM_SYM_PATH);
            TEST_ASSERT_NOT_NULL(dom);
            TEST_ASSERT(ray_sym_domain_intern(dom, "only", 4) > 0, "intern");
            TEST_ASSERT_EQ_I(w == 0 ? ray_sym_domain_flush(dom, false)
                                    : ray_sym_domain_flush_append(dom, false), RAY_OK);
            ray_sym_domain_release(dom);
        } else if (w == 2) {
            ray_sym_domain_t* dom = ray_sym_domain_create_import(TMP_DOM_SYM_PATH);
            if (!dom) continue;
            ray_sym_domain_release(dom);
        } else {
            TEST_ASSERT_EQ_I(ray_sym_save(TMP_DOM_SYM_PATH), RAY_OK);
        }
        if (w <= 1) {
            /* a flush writes the new file's index: never the old one */
            int64_t cnt = 0;
            TEST_ASSERT(ix_read(TMP_IX_PATH, ix_buf2, sizeof ix_buf2) >= IXH, "index written");
            memcpy(&cnt, ix_buf2 + 16, 8);
            if (cnt != 2) FAILF("writer %d left the old index", w);
        } else if (ix_exists(TMP_IX_PATH)) FAILF("writer %d left the old index", w);
    }
    /* a dead writer's temporary file */
    ix_unlink_all();
    const char* tmp = TMP_DOM_SYM_PATH ".ix.999999999.tmp";
    TEST_ASSERT(ix_write(tmp, ix_buf, 16), "temporary file");
    TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH, ix_sym, sz), "symfile");
    ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
    TEST_ASSERT_NOT_NULL(dom);
    ray_sym_domain_release(dom);
    TEST_ASSERT_TRUE(ix_exists(TMP_IX_PATH));
    TEST_ASSERT_FALSE(ix_exists(tmp));
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* A reopen after another process changed the file: growth that keeps every
 * published byte extends the domain — through the new file's index, or by
 * a walk — and a change inside the old records is refused, with the new
 * file's index or without: a length that moves the blocks after it, two in
 * one block that keep them, a byte in the last, partial block or in the
 * first records. */
static test_result_t test_domain_index_extend_prefix(void) {
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    enum { N = 1000, M = 50 };
    for (int v = 0; v < 10; v++) {
        ix_unlink_all();
        ix_gen(&ix_va, "ex", N + M);
        TEST_ASSERT(ix_write_vocab(TMP_DOM_SYM_PATH, &ix_va, N), "symfile");
        ray_sym_domain_t* dom = ray_sym_domain_open(TMP_DOM_SYM_PATH);
        TEST_ASSERT_NOT_NULL(dom);
        TEST_ASSERT_NOT_NULL(ray_sym_domain_str(dom, 5 * IXS + 1));
        /* the grown file, by another process: tmp + rename, as a flush */
        ix_vb = ix_va;
        if (v == 2 || v == 3) ix_set(&ix_vb, 5 * IXS + 1, "a_longer_string_in_a_whole_block");
        if (v == 4 || v == 5) ix_vb.s[N - 2][3] ^= 0x20;
        if (v == 6 || v == 7) {   /* one block's lengths: +3 and -3, its checkpoints kept */
            ix_set(&ix_vb, 335, "ex_335_xxxxxabc");
            ix_set(&ix_vb, 340, "ex_340_xxxxxxx");
        }
        if (v == 8 || v == 9) ix_vb.s[2][1] ^= 0x20;   /* a byte of the first records */
        size_t s2 = ix_strl(&ix_vb, N + M, ix_sym, ix_offs);
        TEST_ASSERT(ix_write(TMP_DOM_SYM_PATH ".tmpx", ix_sym, s2), "grown file");
        TEST_ASSERT_EQ_I(rename(TMP_DOM_SYM_PATH ".tmpx", TMP_DOM_SYM_PATH), 0);
        if (v % 2 == 0) {
            size_t ib = ix_make(ix_buf, ix_sym, ix_offs, N + M, s2, TMP_DOM_SYM_PATH);
            TEST_ASSERT(ix_write(TMP_IX_PATH, ix_buf, ib), "its index");
        } else {
            unlink(TMP_IX_PATH);
        }
        ray_sym_domain_t* again = ray_sym_domain_open(TMP_DOM_SYM_PATH);
        if (v <= 1) {
            TEST_ASSERT_EQ_PTR(again, dom);
            TEST_ASSERT(ix_check_all(dom, &ix_va, N + M), "extended");
            ray_sym_domain_ix_info_t in = ix_info(dom);
            if (in.from_index != (v == 0)) FAILF("case %d: from_index %d", v, (int)in.from_index);
            ray_sym_domain_release(again);
        } else if (again) {
            ray_sym_domain_release(again);
            ray_sym_domain_release(dom);
            FAILF("case %d: a changed prefix was taken for growth", v);
        }
        ray_sym_domain_release(dom);
    }
    unsetenv("RAY_SYM_INDEX_MIN");
    ix_unlink_all();
    PASS();
}

/* A query over a symfile found torn reports `corrupt` instead of reading
 * the lost entries as "". */
static test_result_t test_domain_index_query_corrupt(void) {
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    ray_release(ray_eval_str("(.sys.exec \"rm -rf /tmp/rf_ixq\")"));
    ray_t* r = ray_eval_str(
        "(.db.splayed.set \"/tmp/rf_ixq/t/\" (table [s] (list (as 'SYM (map (fn [x] "
        "(concat \"sym_number_\" (as 'STR x))) (til 3000))))) \"/tmp/rf_ixq/sym\")");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    const char* q = "(count (distinct (as 'STR (at (.db.splayed.get \"/tmp/rf_ixq/t/\" "
                    "\"/tmp/rf_ixq/sym\") 's))))";
    r = ray_eval_str(q);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    TEST_ASSERT_TRUE(ix_exists("/tmp/rf_ixq/sym.ix"));
    long n = ix_read("/tmp/rf_ixq/sym", ix_sym, sizeof ix_sym);
    TEST_ASSERT(n > 3 * 4096, "symfile read");
    memset(ix_sym + 8192, 0, 4096);
    TEST_ASSERT(ix_write("/tmp/rf_ixq/sym", ix_sym, (size_t)n), "torn symfile");
    r = ray_eval_str(q);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    ray_release(ray_eval_str("(.sys.exec \"rm -rf /tmp/rf_ixq\")"));
    unsetenv("RAY_SYM_INDEX_MIN");
    PASS();
}

/* An expression that reads symbols of a torn symfile fails `corrupt` where
 * it reads them: a `set` of it binds nothing, a .csv.write of it writes no
 * file, and a graph run through the C API (ray_execute) fails too. */
static test_result_t test_domain_index_corrupt_stops(void) {
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    ray_release(ray_eval_str("(.sys.exec \"rm -rf /tmp/rf_ixs\")"));
    ray_t* r = ray_eval_str(
        "(.db.splayed.set \"/tmp/rf_ixs/t/\" (table [s] (list (as 'SYM (map (fn [x] "
        "(concat \"sym_number_\" (as 'STR x))) (til 3000))))) \"/tmp/rf_ixs/sym\")");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    r = ray_eval_str("(count (.db.splayed.get \"/tmp/rf_ixs/t/\" \"/tmp/rf_ixs/sym\"))");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    long n = ix_read("/tmp/rf_ixs/sym", ix_sym, sizeof ix_sym);
    TEST_ASSERT(n > 3 * 4096, "symfile read");
    memset(ix_sym + 8192, 0, 4096);
    TEST_ASSERT(ix_write("/tmp/rf_ixs/sym", ix_sym, (size_t)n), "torn symfile");

    r = ray_eval_str("(set ixsU (.db.splayed.get \"/tmp/rf_ixs/t/\" \"/tmp/rf_ixs/sym\"))");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    r = ray_eval_str("(set ixsX (select {from: ixsU where: (== s (as 'SYM \"sym_number_1700\"))}))");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    r = ray_eval_str("ixsX");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));               /* never bound */
    ray_error_free(r);
    r = ray_eval_str("(.csv.write ixsU \"/tmp/rf_ixs/out.csv\")");
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_error_free(r);
    TEST_ASSERT_FALSE(ix_exists("/tmp/rf_ixs/out.csv"));

    /* a print of the column prints nothing: the symbols are read first */
    fflush(stdout);
    int keep = dup(1);
    int fd = open("/tmp/rf_ixs/print.out", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    TEST_ASSERT(keep >= 0 && fd >= 0, "redirect");
    dup2(fd, 1);
    close(fd);
    r = ray_eval_str("(println (at ixsU 's))");
    fflush(stdout);
    dup2(keep, 1);
    close(keep);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    TEST_ASSERT_EQ_I(ix_read("/tmp/rf_ixs/print.out", ix_buf, sizeof ix_buf), 0);

    /* the C API: a LIKE over the column through ray_execute */
    ray_t* u = ray_eval_str("ixsU");
    TEST_ASSERT_FALSE(RAY_IS_ERR(u));
    ray_graph_t* g = ray_graph_new(u);
    ray_op_t* col = ray_scan(g, "s");
    ray_op_t* lk = ray_like(g, col, ray_const_str(g, "*1700*", 6));
    ray_t* res = ray_execute(g, ray_count(g, ray_filter(g, col, lk)));
    TEST_ASSERT_TRUE(RAY_IS_ERR(res));
    TEST_ASSERT_STR_EQ(ray_err_code(res), "corrupt");
    ray_error_free(res);
    ray_graph_free(g);
    ray_release(u);
    ray_release(ray_eval_str("(set ixsU 0)"));
    ray_release(ray_eval_str("(.sys.exec \"rm -rf /tmp/rf_ixs\")"));
    unsetenv("RAY_SYM_INDEX_MIN");
    PASS();
}

static bool ix_has(const uint8_t* b, long n, const char* str) {
    size_t l = strlen(str);
    for (long i = 0; i + (long)l <= n; i++)
        if (memcmp(b + i, str, l) == 0) return true;
    return false;
}

/* Run `expr` with stdout (and stderr) captured into files; returns the
 * result and the bytes each received. */
static ray_t* ix_eval_captured(const char* expr, long* out_n, long* err_n, bool repl) {
    fflush(stdout); fflush(stderr);
    int k1 = dup(1), k2 = dup(2);
    int f1 = open("/tmp/rf_ixs/cap.out", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    int f2 = open("/tmp/rf_ixs/cap.err", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    dup2(f1, 1); dup2(f2, 2); close(f1); close(f2);
    ray_t* r = NULL;
    if (repl) {
        FILE* f = fopen("/tmp/rf_ixs/cap.rfl", "wb");
        if (f) { fputs(expr, f); fclose(f); }
        int rc = ray_repl_run_file("/tmp/rf_ixs/cap.rfl");
        r = ray_i64(rc);
    } else {
        r = ray_eval_str(expr);
    }
    fflush(stdout); fflush(stderr);
    dup2(k1, 1); dup2(k2, 2); close(k1); close(k2);
    *out_n = ix_read("/tmp/rf_ixs/cap.out", ix_buf, sizeof ix_buf);
    *err_n = ix_read("/tmp/rf_ixs/cap.err", ix_buf2, sizeof ix_buf2);
    return r;
}

/* What prints or replies with symbols of a torn symfile: nothing of them
 * goes out — a dict, a long list, deep nesting, the REPL's display, an IPC
 * reply. */
static test_result_t test_domain_index_corrupt_output(void) {
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    ray_release(ray_eval_str("(.sys.exec \"rm -rf /tmp/rf_ixs\")"));
    ray_t* r = ray_eval_str(
        "(.db.splayed.set \"/tmp/rf_ixs/t/\" (table [s] (list (as 'SYM (map (fn [x] "
        "(concat \"sym_number_\" (as 'STR x))) (til 3000))))) \"/tmp/rf_ixs/sym\")");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    r = ray_eval_str("(count (.db.splayed.get \"/tmp/rf_ixs/t/\" \"/tmp/rf_ixs/sym\"))");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    long n = ix_read("/tmp/rf_ixs/sym", ix_sym, sizeof ix_sym);
    TEST_ASSERT(n > 3 * 4096, "symfile read");
    memset(ix_sym + 8192, 0, 4096);
    TEST_ASSERT(ix_write("/tmp/rf_ixs/sym", ix_sym, (size_t)n), "torn symfile");
    r = ray_eval_str("(set ixoU (.db.splayed.get \"/tmp/rf_ixs/t/\" \"/tmp/rf_ixs/sym\"))");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    r = ray_eval_str("(set ixoV (at ixoU 's))");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);

    const char* bad[] = {
        "(println \"dict:\" (dict [k] (list ixoV)))",
        "(println (concat (til 2000) (list ixoV)))",
        "(println (list (list (list (list (list (list ixoV)))))))",
        "(show (dict [k] (list ixoV)))",
    };
    long on = 0, en = 0;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        r = ix_eval_captured(bad[i], &on, &en, false);
        if (!RAY_IS_ERR(r) || strcmp(ray_err_code(r), "corrupt") != 0) FAILF("%s: not corrupt", bad[i]);
        ray_error_free(r);
        if (on != 0) FAILF("%s: printed %ld bytes", bad[i], on);
    }
    /* the REPL's display of a value holding them (its first rows are in
     * the lost page) */
    r = ix_eval_captured("(dict [k] (list (drop ixoV 500)))", &on, &en, true);
    ray_release(r);
    TEST_ASSERT(!ix_has(ix_buf, on, "sym_number_"), "no symbol displayed");
    TEST_ASSERT(ix_has(ix_buf, on, "corrupt") || ix_has(ix_buf2, en, "corrupt"),
                "the REPL says corrupt");
    /* ... and of the rows before the lost page: the walk of a torn file
     * cannot tell them good, so they are not shown either */
    r = ix_eval_captured("(take ixoU 5)", &on, &en, true);
    ray_release(r);
    TEST_ASSERT(!ix_has(ix_buf, on, "sym_number_"), "no symbol displayed");
    TEST_ASSERT(ix_has(ix_buf, on, "corrupt") || ix_has(ix_buf2, en, "corrupt"),
                "the REPL says corrupt");

    /* an IPC reply */
    ray_poll_t* cp = ray_test_client_poll();
    ray_test_server_t srv;
    RAY_TEST_SERVER_START(srv);
    int64_t h = ray_ipc_connect("127.0.0.1", srv.port, NULL, NULL, 0);
    TEST_ASSERT(h >= 0, "connected");
    ray_t* msg = ray_str("ixoV", 4);
    ray_t* resp = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_TRUE(RAY_IS_ERR(resp));
    TEST_ASSERT_STR_EQ(ray_err_code(resp), "corrupt");
    ray_error_free(resp);
    ray_ipc_close(h);
    ray_test_server_stop(&srv);
    if (cp) ray_test_client_poll_done();

    ray_release(ray_eval_str("(set ixoV 0)"));
    ray_release(ray_eval_str("(set ixoU 0)"));
    ray_release(ray_eval_str("(.sys.exec \"rm -rf /tmp/rf_ixs\")"));
    unsetenv("RAY_SYM_INDEX_MIN");
    PASS();
}

/* Zero the bytes of records [from, to) of a symfile in place (a torn write
 * across them). */
static bool ix_zero_records(const char* path, int64_t from, int64_t to) {
    long n = ix_read(path, ix_sym, sizeof ix_sym);
    if (n < 12) return false;
    size_t o = 12, a = 0, b = 0;
    for (int64_t i = 0; o + 4 <= (size_t)n; i++) {
        if (i == from) a = o;
        if (i == to) { b = o; break; }
        uint32_t l;
        memcpy(&l, ix_sym + o, 4);
        o += 4 + l;
    }
    if (!a || b <= a) return false;
    memset(ix_sym + a, 0, b - a);
    return ix_write(path, ix_sym, (size_t)n);
}

/* A torn symfile (records 101..110 zeroed in place after its index was
 * written) opens corrupt, and nothing reads its symbols: not a concat with
 * runtime symbols (the runtime LUT), not a join on them, not a save of
 * them — each fails `corrupt`, naming the file — and the C API's LUT is
 * NULL. */
static test_result_t test_domain_index_torn_readers(void) {
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    ray_release(ray_eval_str("(.sys.exec \"rm -rf /tmp/rf_ixr\")"));
    ray_t* r = ray_eval_str(
        "(.db.splayed.set \"/tmp/rf_ixr/t/\" (table [s] (list (as 'SYM (map (fn [x] "
        "(concat \"sym_number_\" (as 'STR x))) (til 3000))))) \"/tmp/rf_ixr/sym\")");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    r = ray_eval_str("(count (.db.splayed.get \"/tmp/rf_ixr/t/\" \"/tmp/rf_ixr/sym\"))");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    TEST_ASSERT_TRUE(ix_exists("/tmp/rf_ixr/sym.ix"));
    /* 10 records of 18 bytes: the zeros read as 45 empty records and the walk
     * comes back in step at record 111 — every count record is walked, the
     * records end short of the file's end: the walk's checkpoints all agree
     * with themselves */
    TEST_ASSERT(ix_zero_records("/tmp/rf_ixr/sym", 101, 111), "torn symfile");
    r = ray_eval_str("(set ixrU (.db.splayed.get \"/tmp/rf_ixr/t/\" \"/tmp/rf_ixr/sym\"))");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    const char* bad[] = {
        "(at (concat (at ixrU 's) (as 'SYM [\"zz\"])) [150 2999])",
        "(inner-join [s] ixrU (table [s v] (list (as 'SYM [\"sym_number_5\" \"sym_number_2000\"]) [1 2])))",
        "(left-join [s] ixrU (table [s v] (list (as 'SYM [\"sym_number_5\" \"sym_number_2000\"]) [1 2])))",
        "(.db.splayed.set \"/tmp/rf_ixr/t2/\" (table [s] (list (concat (at ixrU 's) (as 'SYM [\"zz\"])))))",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        r = ray_eval_str(bad[i]);
        if (!RAY_IS_ERR(r) || strcmp(ray_err_code(r), "corrupt") != 0) {
            if (!RAY_IS_ERR(r)) ray_release(r); else ray_error_free(r);
            FAILF("%s: not corrupt", bad[i]);
        }
        const char* m = ray_error_msg();
        if (!m || !strstr(m, "/tmp/rf_ixr/sym")) {
            ray_error_free(r);
            FAILF("%s: the error does not name the file (%s)", bad[i], m ? m : "none");
        }
        ray_error_free(r);
    }
    ray_sym_domain_t* dom = ray_sym_domain_open("/tmp/rf_ixr/sym");
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_TRUE(ix_info(dom).corrupt);
    uint64_t c0 = ray_sym_domain_corruptions();
    TEST_ASSERT_NULL(ray_sym_domain_runtime_lut(dom));
    TEST_ASSERT(ray_sym_domain_corruptions() > c0, "counted");
    TEST_ASSERT_EQ_I(ray_sym_domain_find(dom, "sym_number_2000", 15), -1);
    ray_sym_domain_release(dom);
    ray_release(ray_eval_str("(set ixrU 0)"));
    ray_release(ray_eval_str("(.sys.exec \"rm -rf /tmp/rf_ixr\")"));
    unsetenv("RAY_SYM_INDEX_MIN");
    PASS();
}

/* The domain scenarios that reopen, extend, flush and build lookups, run
 * once more with every symfile indexed. */
static test_result_t test_domain_index_scenarios(void) {
    setenv("RAY_SYM_INDEX_MIN", "0", 1);
    test_result_t (*const runs[])(void) = {
        test_domain_open_basic, test_domain_raw_pin, test_domain_extend_reopen_flat,
        test_domain_extend_after_flush, test_domain_flush_append,
        test_domain_open_position0_validation, test_domain_intern_batch,
    };
    for (size_t k = 0; k < sizeof runs / sizeof runs[0]; k++) {
        test_result_t r = runs[k]();
        if (r.status != TEST_PASS) { unsetenv("RAY_SYM_INDEX_MIN"); unlink(TMP_IX_PATH); return r; }
    }
    unsetenv("RAY_SYM_INDEX_MIN");
    unlink(TMP_IX_PATH);
    PASS();
}


const test_entry_t domain_entries[] = {
    { "domain/index_build_and_use",     test_domain_index_build_and_use,     domain_setup, domain_teardown },
    { "domain/index_long_strings",      test_domain_index_long_strings,      domain_setup, domain_teardown },
    { "domain/index_identity",          test_domain_index_identity,          domain_setup, domain_teardown },
    { "domain/index_rejects_bad",       test_domain_index_rejects_bad,       domain_setup, domain_teardown },
    { "domain/index_checkpoint_damage", test_domain_index_checkpoint_damage, domain_setup, domain_teardown },
    { "domain/index_stale_caught",      test_domain_index_stale_caught,      domain_setup, domain_teardown },
    { "domain/index_torn_file",         test_domain_index_torn_file,         domain_setup, domain_teardown },
    { "domain/index_len_skips_record",   test_domain_index_len_skips_record,   domain_setup, domain_teardown },
    { "domain/index_cold_long_no_headers", test_domain_index_cold_long_no_headers, domain_setup, domain_teardown },
    { "domain/index_escape_long_damage", test_domain_index_escape_and_long_damage, domain_setup, domain_teardown },
    { "domain/index_cold_len_no_bytes",  test_domain_index_cold_len_no_bytes,  domain_setup, domain_teardown },
    { "domain/index_torn_open",         test_domain_index_torn_open,         domain_setup, domain_teardown },
    { "domain/index_corrupt_names_file", test_domain_corrupt_names_file, domain_setup, domain_teardown },
    { "domain/index_append_unindexed",  test_domain_index_append_unindexed,  domain_setup, domain_teardown },
    { "domain/index_writers",           test_domain_index_writers,           domain_setup, domain_teardown },
    { "domain/index_refresh_in_place",  test_domain_index_refresh_in_place,  domain_setup, domain_teardown },
    { "domain/index_new_file_removes",  test_domain_index_new_file_removes,  domain_setup, domain_teardown },
    { "domain/index_extend_prefix",     test_domain_index_extend_prefix,     domain_setup, domain_teardown },
    { "domain/index_query_corrupt",     test_domain_index_query_corrupt,     domain_rt_setup, domain_rt_teardown },
    { "domain/index_corrupt_stops",     test_domain_index_corrupt_stops,     domain_rt_setup, domain_rt_teardown },
    { "domain/index_corrupt_output",    test_domain_index_corrupt_output,    domain_rt_setup, domain_rt_teardown },
    { "domain/index_torn_readers",      test_domain_index_torn_readers,      domain_rt_setup, domain_rt_teardown },
    { "domain/index_scenarios",         test_domain_index_scenarios,         domain_setup, domain_teardown },
    { "domain/private", test_domain_private, domain_setup, domain_teardown },
    { "domain/flush_append", test_domain_flush_append, domain_setup, domain_teardown },
    { "domain/import_dict", test_domain_import_dict, domain_setup, domain_teardown },
    { "domain/import_dict_reclaim", test_domain_import_dict_reclaim, domain_setup, domain_teardown },
    { "domain/import_dict_tags", test_domain_import_dict_tags, domain_setup, domain_teardown },
    { "domain/import_dict_reserve", test_domain_import_dict_reserve, domain_setup, domain_teardown },
    { "domain/sk4_reserve_kept_freed", test_domain_sk4_reserve_kept_freed, domain_setup, domain_teardown },
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
    { "domain/extend_reopen_flat",      test_domain_extend_reopen_flat,      domain_setup, domain_teardown },
    { "domain/extend_after_flush",      test_domain_extend_after_flush,      domain_setup, domain_teardown },
    { "domain/extend_under_batch",      test_domain_extend_under_batch,      domain_setup, domain_teardown },
    { "domain/concat_text_nulls",       test_domain_concat_text_nulls,       domain_rt_setup, domain_rt_teardown },
    { "domain/runtime_lut",             test_domain_runtime_lut,             domain_rt_setup, domain_rt_teardown },
    { "domain/open_position0_validation", test_domain_open_position0_validation, domain_setup, domain_teardown },
    { "domain/intern_batch",            test_domain_intern_batch,            domain_setup, domain_teardown },
    { "domain/intern_batch_cancel",     test_domain_intern_batch_cancel,     domain_setup, domain_teardown },
    { "domain/intern_batch_concurrent", test_domain_intern_batch_concurrent, domain_setup, domain_teardown },
    { "domain/dict_upsert_file_keys",   test_domain_dict_upsert_file_keys,   domain_rt_setup, domain_rt_teardown },
    { NULL, NULL, NULL, NULL },
};
