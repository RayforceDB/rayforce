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
#include <rayforce.h>
#include "mem/heap.h"
#include "mem/arena.h"
#include "table/sym.h"
#include "store/col.h"
#include "lang/internal.h"
#include "ops/hash.h"
#include <string.h>
#include <stdio.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void sym_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void sym_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- sym_init_destroy -------------------------------------------------- */

static test_result_t test_sym_init_destroy(void) {
    /* After init, count should be 0 */
    TEST_ASSERT_EQ_U(ray_sym_count(), 0);

    PASS();
}

/* ---- sym_intern_basic -------------------------------------------------- */

static test_result_t test_sym_intern_basic(void) {
    int64_t id = ray_sym_intern("hello", 5);
    TEST_ASSERT((id) >= (0), "id >= 0");
    TEST_ASSERT_EQ_U(ray_sym_count(), 1);

    PASS();
}

/* ---- sym_intern_duplicate ---------------------------------------------- */

static test_result_t test_sym_intern_duplicate(void) {
    int64_t id1 = ray_sym_intern("hello", 5);
    int64_t id2 = ray_sym_intern("hello", 5);
    TEST_ASSERT_EQ_I(id1, id2);
    TEST_ASSERT_EQ_U(ray_sym_count(), 1);

    PASS();
}

/* ---- sym_find_existing ------------------------------------------------- */

static test_result_t test_sym_find_existing(void) {
    int64_t id = ray_sym_intern("world", 5);
    int64_t found = ray_sym_find("world", 5);
    TEST_ASSERT_EQ_I(found, id);

    PASS();
}

/* ---- sym_find_missing -------------------------------------------------- */

static test_result_t test_sym_find_missing(void) {
    int64_t found = ray_sym_find("nonexistent", 11);
    TEST_ASSERT_EQ_I(found, -1);

    PASS();
}

/* ---- sym_str_roundtrip ------------------------------------------------- */

static test_result_t test_sym_str_roundtrip(void) {
    int64_t id = ray_sym_intern("roundtrip", 9);
    TEST_ASSERT((id) >= (0), "id >= 0");

    ray_t* s = ray_sym_str(id);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQ_U(ray_str_len(s), 9);
    TEST_ASSERT_MEM_EQ(9, ray_str_ptr(s), "roundtrip");

    PASS();
}

/* ---- sym_count --------------------------------------------------------- */

static test_result_t test_sym_count(void) {
    TEST_ASSERT_EQ_U(ray_sym_count(), 0);

    ray_sym_intern("a", 1);
    TEST_ASSERT_EQ_U(ray_sym_count(), 1);

    ray_sym_intern("b", 1);
    TEST_ASSERT_EQ_U(ray_sym_count(), 2);

    ray_sym_intern("c", 1);
    TEST_ASSERT_EQ_U(ray_sym_count(), 3);

    /* Duplicate should not increase count */
    ray_sym_intern("a", 1);
    TEST_ASSERT_EQ_U(ray_sym_count(), 3);

    PASS();
}

/* ---- sym_many ---------------------------------------------------------- */

static test_result_t test_sym_many(void) {
    /* Intern 1000 unique symbols */
    int64_t ids[1000];
    char buf[32];
    for (int i = 0; i < 1000; i++) {
        int len = snprintf(buf, sizeof(buf), "sym_%d", i);
        ids[i] = ray_sym_intern(buf, (size_t)len);
        TEST_ASSERT((ids[i]) >= (0), "ids[i] >= 0");
    }

    TEST_ASSERT_EQ_U(ray_sym_count(), 1000);

    /* Verify all are distinct IDs */
    for (int i = 0; i < 1000; i++) {
        for (int j = i + 1; j < 1000; j++) {
            TEST_ASSERT((ids[i]) != (ids[j]), "ids[i] != ids[j]");
        }
    }

    /* Verify all are retrievable with correct strings */
    for (int i = 0; i < 1000; i++) {
        int len = snprintf(buf, sizeof(buf), "sym_%d", i);
        ray_t* s = ray_sym_str(ids[i]);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_EQ_U(ray_str_len(s), (size_t)len);
        TEST_ASSERT_MEM_EQ((size_t)len, ray_str_ptr(s), buf);
    }

    /* Re-interning should return same IDs */
    for (int i = 0; i < 1000; i++) {
        int len = snprintf(buf, sizeof(buf), "sym_%d", i);
        int64_t id2 = ray_sym_intern(buf, (size_t)len);
        TEST_ASSERT_EQ_I(id2, ids[i]);
    }

    TEST_ASSERT_EQ_U(ray_sym_count(), 1000);

    PASS();
}

/* ---- sym_bulk: intern 100K symbols and verify none are lost ------------ */

static test_result_t test_sym_bulk(void) {
    #define BULK_N 100000

    /* Pre-reserve capacity (tests ray_sym_ensure_cap) */
    bool cap_ok = ray_sym_ensure_cap(BULK_N);
    TEST_ASSERT_TRUE(cap_ok);

    /* Intern 100K unique symbols */
    char buf[32];
    for (int i = 0; i < BULK_N; i++) {
        int len = snprintf(buf, sizeof(buf), "bulk_%06d", i);
        int64_t id = ray_sym_intern(buf, (size_t)len);
        TEST_ASSERT((id) >= (0), "id >= 0");
    }

    TEST_ASSERT_EQ_U(ray_sym_count(), BULK_N);

    /* Verify every symbol is retrievable with correct string */
    for (int i = 0; i < BULK_N; i++) {
        int len = snprintf(buf, sizeof(buf), "bulk_%06d", i);
        int64_t id = ray_sym_find(buf, (size_t)len);
        TEST_ASSERT((id) >= (0), "id >= 0");
        ray_t* s = ray_sym_str(id);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_EQ_U(ray_str_len(s), (size_t)len);
        TEST_ASSERT_MEM_EQ((size_t)len, ray_str_ptr(s), buf);
    }

    /* Re-interning must return same IDs (idempotent) */
    for (int i = 0; i < BULK_N; i++) {
        int len = snprintf(buf, sizeof(buf), "bulk_%06d", i);
        int64_t id1 = ray_sym_find(buf, (size_t)len);
        int64_t id2 = ray_sym_intern(buf, (size_t)len);
        TEST_ASSERT_EQ_I(id1, id2);
    }

    TEST_ASSERT_EQ_U(ray_sym_count(), BULK_N);

    #undef BULK_N
    PASS();
}

/* ---- sym_save_load_roundtrip ------------------------------------------- */

static test_result_t test_sym_save_load_roundtrip(void) {
    const char* sym_path = "/tmp/test_sym_roundtrip.sym";

    /* Intern some symbols */
    int64_t id_hello = ray_sym_intern("hello", 5);
    int64_t id_world = ray_sym_intern("world", 5);
    int64_t id_foo   = ray_sym_intern("foo", 3);
    TEST_ASSERT((id_hello) >= (0), "id_hello >= 0");
    TEST_ASSERT((id_world) >= (0), "id_world >= 0");
    TEST_ASSERT((id_foo) >= (0), "id_foo >= 0");
    TEST_ASSERT_EQ_U(ray_sym_count(), 3);

    /* Save */
    ray_err_t err = ray_sym_save(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Destroy and re-init sym table */
    ray_sym_destroy();
    (void)ray_sym_init();
    TEST_ASSERT_EQ_U(ray_sym_count(), 0);

    /* Load */
    err = ray_sym_load(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    TEST_ASSERT_EQ_U(ray_sym_count(), 3);

    /* Verify all strings match */
    ray_t* s0 = ray_sym_str(id_hello);
    TEST_ASSERT_NOT_NULL(s0);
    TEST_ASSERT_EQ_U(ray_str_len(s0), 5);
    TEST_ASSERT_MEM_EQ(5, ray_str_ptr(s0), "hello");

    ray_t* s1 = ray_sym_str(id_world);
    TEST_ASSERT_NOT_NULL(s1);
    TEST_ASSERT_EQ_U(ray_str_len(s1), 5);
    TEST_ASSERT_MEM_EQ(5, ray_str_ptr(s1), "world");

    ray_t* s2 = ray_sym_str(id_foo);
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_EQ_U(ray_str_len(s2), 3);
    TEST_ASSERT_MEM_EQ(3, ray_str_ptr(s2), "foo");

    /* Cleanup */
    remove(sym_path);
    char lk_path[4096];
    snprintf(lk_path, sizeof(lk_path), "%s.lk", sym_path);
    remove(lk_path);

    PASS();
}

/* ---- sym_save_append_only --------------------------------------------- */

static test_result_t test_sym_save_append_only(void) {
    const char* sym_path = "/tmp/test_sym_append.sym";

    /* Intern initial batch */
    int64_t id_a = ray_sym_intern("alpha", 5);
    int64_t id_b = ray_sym_intern("beta", 4);
    TEST_ASSERT((id_a) >= (0), "id_a >= 0");
    TEST_ASSERT((id_b) >= (0), "id_b >= 0");

    /* First save */
    ray_err_t err = ray_sym_save(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Second save with no changes -> should be no-op */
    err = ray_sym_save(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Intern more symbols */
    int64_t id_c = ray_sym_intern("gamma", 5);
    int64_t id_d = ray_sym_intern("delta", 5);
    TEST_ASSERT((id_c) >= (0), "id_c >= 0");
    TEST_ASSERT((id_d) >= (0), "id_d >= 0");

    /* Save again (append-only: new entries added) */
    err = ray_sym_save(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Destroy and reload */
    ray_sym_destroy();
    (void)ray_sym_init();
    err = ray_sym_load(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    TEST_ASSERT_EQ_U(ray_sym_count(), 4);

    /* Verify old IDs are stable */
    ray_t* sa = ray_sym_str(id_a);
    TEST_ASSERT_NOT_NULL(sa);
    TEST_ASSERT_MEM_EQ(5, ray_str_ptr(sa), "alpha");

    ray_t* sb = ray_sym_str(id_b);
    TEST_ASSERT_NOT_NULL(sb);
    TEST_ASSERT_MEM_EQ(4, ray_str_ptr(sb), "beta");

    /* Verify new IDs are present */
    ray_t* sc = ray_sym_str(id_c);
    TEST_ASSERT_NOT_NULL(sc);
    TEST_ASSERT_MEM_EQ(5, ray_str_ptr(sc), "gamma");

    ray_t* sd = ray_sym_str(id_d);
    TEST_ASSERT_NOT_NULL(sd);
    TEST_ASSERT_MEM_EQ(5, ray_str_ptr(sd), "delta");

    /* Cleanup */
    remove(sym_path);
    char lk_path[4096];
    snprintf(lk_path, sizeof(lk_path), "%s.lk", sym_path);
    remove(lk_path);

    PASS();
}

/* ---- sym_load_corrupt -------------------------------------------------- */

static test_result_t test_sym_load_corrupt(void) {
    const char* sym_path = "/tmp/test_sym_corrupt.sym";

    /* Write garbage data */
    FILE* f = fopen(sym_path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    const char garbage[] = "this is not a valid sym file";
    fwrite(garbage, 1, sizeof(garbage), f);
    fclose(f);

    /* Load should fail with corrupt */
    ray_err_t err = ray_sym_load(sym_path);
    TEST_ASSERT((err) != (RAY_OK), "err != RAY_OK");

    /* Count unchanged */
    TEST_ASSERT_EQ_U(ray_sym_count(), 0);

    /* Cleanup */
    remove(sym_path);
    char lk_path[4096];
    snprintf(lk_path, sizeof(lk_path), "%s.lk", sym_path);
    remove(lk_path);

    PASS();
}

/* ---- sym_load_truncated ------------------------------------------------ */

static test_result_t test_sym_load_truncated(void) {
    const char* sym_path = "/tmp/test_sym_trunc.sym";

    /* Intern and save valid sym file */
    ray_sym_intern("abc", 3);
    ray_sym_intern("def", 3);
    ray_err_t err = ray_sym_save(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Truncate the file to 2 bytes */
    FILE* f = fopen(sym_path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite("AB", 1, 2, f);
    fclose(f);

    /* Destroy and re-init */
    ray_sym_destroy();
    (void)ray_sym_init();

    /* Load should fail */
    err = ray_sym_load(sym_path);
    TEST_ASSERT((err) != (RAY_OK), "err != RAY_OK");
    TEST_ASSERT_EQ_U(ray_sym_count(), 0);

    /* Cleanup */
    remove(sym_path);
    char lk_path[4096];
    snprintf(lk_path, sizeof(lk_path), "%s.lk", sym_path);
    remove(lk_path);

    PASS();
}

/* ---- sym_dotted -------------------------------------------------------- */

static test_result_t test_sym_dotted_detect(void) {
    int64_t flat_id = ray_sym_intern("foo", 3);
    TEST_ASSERT((flat_id) >= (0), "flat_id >= 0");
    TEST_ASSERT_FALSE(ray_sym_is_dotted(flat_id));

    int64_t dotted_id = ray_sym_intern("foo.bar", 7);
    TEST_ASSERT((dotted_id) >= (0), "dotted_id >= 0");
    TEST_ASSERT_TRUE(ray_sym_is_dotted(dotted_id));

    /* foo.bar is a distinct sym_id from foo */
    TEST_ASSERT((dotted_id) != (flat_id), "dotted_id != flat_id");

    PASS();
}

static test_result_t test_sym_dotted_segments(void) {
    int64_t id = ray_sym_intern("math.pi", 7);
    TEST_ASSERT((id) >= (0), "id >= 0");
    TEST_ASSERT_TRUE(ray_sym_is_dotted(id));

    const int64_t* segs = NULL;
    int n = ray_sym_segs(id, &segs);
    TEST_ASSERT_EQ_I(n, 2);
    TEST_ASSERT_NOT_NULL(segs);

    /* Each segment must itself be interned and resolvable */
    int64_t math_id = ray_sym_find("math", 4);
    int64_t pi_id   = ray_sym_find("pi",   2);
    TEST_ASSERT_EQ_I(math_id, segs[0]);
    TEST_ASSERT_EQ_I(pi_id, segs[1]);

    /* Segments themselves are plain names (not dotted) */
    TEST_ASSERT_FALSE(ray_sym_is_dotted(segs[0]));
    TEST_ASSERT_FALSE(ray_sym_is_dotted(segs[1]));

    PASS();
}

static test_result_t test_sym_dotted_triple(void) {
    int64_t id = ray_sym_intern("a.b.c", 5);
    TEST_ASSERT((id) >= (0), "id >= 0");
    TEST_ASSERT_TRUE(ray_sym_is_dotted(id));

    const int64_t* segs = NULL;
    int n = ray_sym_segs(id, &segs);
    TEST_ASSERT_EQ_I(n, 3);
    TEST_ASSERT_EQ_I(ray_sym_find("a", 1), segs[0]);
    TEST_ASSERT_EQ_I(ray_sym_find("b", 1), segs[1]);
    TEST_ASSERT_EQ_I(ray_sym_find("c", 1), segs[2]);

    PASS();
}

static test_result_t test_sym_dotted_idempotent(void) {
    int64_t id1 = ray_sym_intern("lib.util", 8);
    int64_t id2 = ray_sym_intern("lib.util", 8);
    TEST_ASSERT_EQ_I(id1, id2);
    TEST_ASSERT_TRUE(ray_sym_is_dotted(id1));

    const int64_t* segs1 = NULL;
    const int64_t* segs2 = NULL;
    int n1 = ray_sym_segs(id1, &segs1);
    int n2 = ray_sym_segs(id2, &segs2);
    TEST_ASSERT_EQ_I(n1, n2);
    /* Same symbol so the cached segment pointer should be identical too */
    TEST_ASSERT_EQ_PTR((void*)segs1, (void*)segs2);

    PASS();
}

static test_result_t test_sym_dotted_rejects_empty_segment(void) {
    /* Leading / trailing dot, or double-dot: the name still interns (a plain
     * symbol with '.' in it), but is_dotted stays false so env-lookup
     * doesn't try to split it. */
    int64_t id1 = ray_sym_intern(".foo", 4);
    int64_t id2 = ray_sym_intern("foo.", 4);
    int64_t id3 = ray_sym_intern("a..b", 4);
    TEST_ASSERT((id1) >= (0), "id1 >= 0");
    TEST_ASSERT((id2) >= (0), "id2 >= 0");
    TEST_ASSERT((id3) >= (0), "id3 >= 0");
    TEST_ASSERT_FALSE(ray_sym_is_dotted(id1));
    TEST_ASSERT_FALSE(ray_sym_is_dotted(id2));
    TEST_ASSERT_FALSE(ray_sym_is_dotted(id3));

    PASS();
}

/* ---- sym_bytes_upper_covers_sym_str_arena ------------------------------ */

/* Directly validates ray_sym_bytes_upper against the actual arena
 * consumption of sym_str_arena's allocation pattern for every length
 * in a wide range.  The previous formula returned `chars_block` for
 * len>=7 but sym_str_arena's ray_arena_alloc actually charges
 * 32 + chars_block — an off-by-32 under-reservation that could still
 * cause the atomic-intern commit phase to trip a new-chunk allocation
 * and fail.  A regression would cause this test to fail for the first
 * long length (len=7) where actual > predicted.
 *
 * The test mirrors sym_str_arena's allocation pattern from sym.c:
 * short names (len<7) call ray_arena_alloc(_, 0); long names call
 * ray_arena_alloc(_, chars_block) where chars_block = ALIGN(32+len+1). */
static test_result_t test_sym_bytes_upper_covers_arena(void) {
    /* Local arena so we're isolated from g_sym's state. */
    ray_arena_t* a = ray_arena_new(1024 * 1024);
    TEST_ASSERT_NOT_NULL(a);

    for (size_t len = 0; len <= 128; len++) {
        size_t before = ray_arena_total_used(a);
        ray_t* v;
        if (len < 7) {
            v = ray_arena_alloc(a, 0);
        } else {
            size_t chars_block = ((size_t)32 + len + 1 + 31) & ~(size_t)31;
            v = ray_arena_alloc(a, chars_block);
        }
        TEST_ASSERT_NOT_NULL(v);
        size_t after = ray_arena_total_used(a);

        size_t actual    = after - before;
        size_t predicted = ray_sym_bytes_upper(len);
        /* Invariant: predicted must be a safe upper bound. */
        if (actual > predicted) {
            fprintf(stderr,
                "ray_sym_bytes_upper(%zu) = %zu but arena consumed %zu bytes\n",
                len, predicted, actual);
            FAIL("explicit MUNIT_FAIL");
        }
    }

    ray_arena_destroy(a);
    PASS();
}

/* ---- sym_intern_long_segments ------------------------------------------ */

/* Exercises the long-string arena path (segment name length >= 7) for
 * every commit inside a dotted intern.  sym_str_arena takes a different
 * allocation branch for names >= 7 bytes (fused CHAR+STR block) and the
 * pre-reservation formula has to account for the ARENA_ALIGN_UP(32 +
 * nbytes) overhead that ray_arena_alloc charges for each block —
 * previously under-reserved by 32 bytes per long string, so phase C
 * could trigger a new-chunk allocation and fail mid-commit.  Many deep
 * paths with long segments stress that repeatedly. */
static test_result_t test_sym_intern_long_segments(void) {
    /* 5-segment path, every segment >=7 bytes: forces the long-string
     * path for main + all segments in one atomic commit. */
    const char* name =
        "configuration.database.primary.connection.timeout_seconds";
    size_t len = strlen(name);
    int64_t id = ray_sym_intern(name, len);
    TEST_ASSERT((id) >= (0), "id >= 0");
    TEST_ASSERT_TRUE(ray_sym_is_dotted(id));

    const int64_t* segs = NULL;
    int n = ray_sym_segs(id, &segs);
    TEST_ASSERT_EQ_I(n, 5);
    TEST_ASSERT_EQ_I(ray_sym_find("configuration",    13), segs[0]);
    TEST_ASSERT_EQ_I(ray_sym_find("database",          8), segs[1]);
    TEST_ASSERT_EQ_I(ray_sym_find("primary",           7), segs[2]);
    TEST_ASSERT_EQ_I(ray_sym_find("connection",       10), segs[3]);
    TEST_ASSERT_EQ_I(ray_sym_find("timeout_seconds",  15), segs[4]);

    /* Repeat with a second long path that overlaps the first on one
     * segment to confirm re-use works under the atomic commit. */
    int64_t id2 = ray_sym_intern("configuration.logging.destination", 33);
    TEST_ASSERT((id2) >= (0), "id2 >= 0");
    TEST_ASSERT_TRUE(ray_sym_is_dotted(id2));
    segs = NULL;
    n = ray_sym_segs(id2, &segs);
    TEST_ASSERT_EQ_I(n, 3);
    TEST_ASSERT_EQ_I(segs[0], ray_sym_find("configuration",  13));

    PASS();
}

/* ---- sym_intern_atomic_no_orphans -------------------------------------- */

/* Interning a dotted name must commit the main sym BEFORE (or at least
 * in lockstep with) its segment syms — we must never observe a state
 * where the segments exist but the main name does not.  Previously,
 * prep sub-interned segments eagerly and only then attempted the main
 * commit; a failure between those steps would leave the segments as
 * orphans on the sym table.  The three-phase intern closes that gap:
 * all commits happen under a reservation that guarantees success. */
static test_result_t test_sym_intern_atomic_no_orphans(void) {
    /* Fresh table: no segments or main name yet. */
    TEST_ASSERT_EQ_I(ray_sym_find("cfg",      3), -1);
    TEST_ASSERT_EQ_I(ray_sym_find("host",     4), -1);
    TEST_ASSERT_EQ_I(ray_sym_find("cfg.host", 8), -1);

    /* Intern the dotted name.  After success, main + both segments exist
     * AND are linked through the segments cache. */
    int64_t id = ray_sym_intern("cfg.host", 8);
    TEST_ASSERT((id) >= (0), "id >= 0");

    int64_t host_id = ray_sym_find("host", 4);
    int64_t cfg_id  = ray_sym_find("cfg",  3);
    TEST_ASSERT((host_id) >= (0), "host_id >= 0");
    TEST_ASSERT((cfg_id) >= (0), "cfg_id >= 0");

    TEST_ASSERT_TRUE(ray_sym_is_dotted(id));
    const int64_t* segs = NULL;
    int n = ray_sym_segs(id, &segs);
    TEST_ASSERT_EQ_I(n, 2);
    TEST_ASSERT_EQ_I(segs[0], cfg_id);
    TEST_ASSERT_EQ_I(segs[1], host_id);

    /* Segments are marked as plain (scanned bit set, dotted bit clear). */
    TEST_ASSERT_FALSE(ray_sym_is_dotted(cfg_id));
    TEST_ASSERT_FALSE(ray_sym_is_dotted(host_id));

    /* Re-interning reuses the same ids — no duplicate commits. */
    TEST_ASSERT_EQ_I(ray_sym_intern("cfg.host", 8), id);
    TEST_ASSERT_EQ_I(ray_sym_intern("cfg",      3), cfg_id);
    TEST_ASSERT_EQ_I(ray_sym_intern("host",     4), host_id);

    PASS();
}

/* ---- sym_intern_atomic_cache ------------------------------------------- */

/* Invariant: if ray_sym_intern returns a valid id, the dotted/scanned
 * metadata for that id is already consistent with the name's structure.
 * If we ever regress to committing the sym first and caching after (a la
 * the previous "sym_intern_nolock_noseg then sym_cache_segments" split),
 * env_set/env_get would silently take the flat path for a dotted name
 * during the window before caching ran.  This test locks that shut. */
static test_result_t test_sym_intern_atomic_cache(void) {
    /* Dotted name: is_dotted + segments must be populated on return. */
    int64_t id = ray_sym_intern("ns.leaf", 7);
    TEST_ASSERT((id) >= (0), "id >= 0");
    TEST_ASSERT_TRUE(ray_sym_is_dotted(id));
    const int64_t* segs = NULL;
    int n = ray_sym_segs(id, &segs);
    TEST_ASSERT_EQ_I(n, 2);
    TEST_ASSERT_EQ_I(ray_sym_find("ns",   2), segs[0]);
    TEST_ASSERT_EQ_I(ray_sym_find("leaf", 4), segs[1]);

    /* Plain name: is_dotted is false immediately, segments empty. */
    int64_t pid = ray_sym_intern("plain_thing", 11);
    TEST_ASSERT((pid) >= (0), "pid >= 0");
    TEST_ASSERT_FALSE(ray_sym_is_dotted(pid));
    segs = NULL;
    TEST_ASSERT_EQ_I(ray_sym_segs(pid, &segs), 0);

    /* Deep path: all segments interned, all visible. */
    int64_t d = ray_sym_intern("a.b.c.d.e", 9);
    TEST_ASSERT((d) >= (0), "d >= 0");
    TEST_ASSERT_TRUE(ray_sym_is_dotted(d));
    segs = NULL;
    TEST_ASSERT_EQ_I(ray_sym_segs(d, &segs), 5);

    PASS();
}

/* ---- sym_dotted_reintern_retries_cache --------------------------------- */

/* If the very first intern of a dotted name fails to cache its segments
 * (e.g. transient arena OOM), the next intern of the same name must
 * retry the cache — otherwise a momentary OOM would permanently strip
 * the dotted semantics from that sym.  We simulate "first intern didn't
 * cache" with ray_sym_intern_no_split, then verify the normal intern
 * path picks up the missing cache. */
static test_result_t test_sym_dotted_reintern_retries_cache(void) {
    /* Seed the table with a dotted sym via the no-split path — this is
     * exactly the in-memory state after a transient cache failure:
     * the sym exists but its dotted bit is clear. */
    int64_t id = ray_sym_intern_no_split("math.pi", 7);
    TEST_ASSERT((id) >= (0), "id >= 0");
    TEST_ASSERT_FALSE(ray_sym_is_dotted(id));

    /* Re-intern through the normal path.  Must return the same id AND
     * retroactively populate the segment cache. */
    int64_t id2 = ray_sym_intern("math.pi", 7);
    TEST_ASSERT_EQ_I(id2, id);
    TEST_ASSERT_TRUE(ray_sym_is_dotted(id));

    const int64_t* segs = NULL;
    int n = ray_sym_segs(id, &segs);
    TEST_ASSERT_EQ_I(n, 2);
    TEST_ASSERT_EQ_I(ray_sym_find("math", 4), segs[0]);
    TEST_ASSERT_EQ_I(ray_sym_find("pi",   2), segs[1]);

    PASS();
}

/* ---- sym_rebuild_segments_contract ------------------------------------- */

/* Direct-call contract for ray_sym_rebuild_segments: returns RAY_OK on
 * success, is idempotent, and populates the dotted cache for entries
 * created via the no-split variant (which is what the persistence paths
 * use internally). */
static test_result_t test_sym_rebuild_segments_contract(void) {
    /* Empty table: rebuild is a no-op but still OK. */
    TEST_ASSERT_EQ_I(ray_sym_rebuild_segments(), RAY_OK);

    /* Insert a dotted name via the no-split path: segments are NOT cached
     * until rebuild runs. */
    int64_t id = ray_sym_intern_no_split("alpha.beta", 10);
    TEST_ASSERT((id) >= (0), "id >= 0");
    TEST_ASSERT_FALSE(ray_sym_is_dotted(id));   /* not yet cached */

    /* Rebuild: cache populated, succeeds. */
    TEST_ASSERT_EQ_I(ray_sym_rebuild_segments(), RAY_OK);
    TEST_ASSERT_TRUE(ray_sym_is_dotted(id));

    /* Idempotent: second call is still OK, doesn't re-cache. */
    const int64_t* segs1 = NULL;
    int n1 = ray_sym_segs(id, &segs1);
    TEST_ASSERT_EQ_I(ray_sym_rebuild_segments(), RAY_OK);
    const int64_t* segs2 = NULL;
    int n2 = ray_sym_segs(id, &segs2);
    TEST_ASSERT_EQ_I(n1, n2);
    TEST_ASSERT_EQ_PTR((void*)segs1, (void*)segs2);  /* same cached array */

    PASS();
}

/* ---- sym_load_legacy_dotted -------------------------------------------- */

/* Pre-feature sym files can contain dotted names like "user.name" (a CSV
 * column, a sym literal, etc.) *without* accompanying segment entries
 * because the old interner didn't split on '.'.  Loading must still
 * succeed: segment sub-interning during load would otherwise shift
 * subsequent disk positions and trip the id==pos check. */
static test_result_t test_sym_load_legacy_dotted(void) {
    const char* sym_path = "/tmp/test_sym_legacy_dotted.sym";
    remove(sym_path);
    char lk_path[4096];
    snprintf(lk_path, sizeof(lk_path), "%s.lk", sym_path);
    remove(lk_path);

    /* Build a file-on-disk that only contains ["alice", "user.name",
     * "charlie"] — no "user" or "name" entries following the dotted name.
     * Temporarily disable segment caching by interning the dotted name
     * with a fake name first, then renaming via direct RAY_LIST build. */
    ray_t* list = ray_list_new(3);
    TEST_ASSERT_NOT_NULL(list);
    ray_t* s0 = ray_str("alice", 5);
    ray_t* s1 = ray_str("user.name", 9);
    ray_t* s2 = ray_str("charlie", 7);
    list = ray_list_append(list, s0); ray_release(s0);
    list = ray_list_append(list, s1); ray_release(s1);
    list = ray_list_append(list, s2); ray_release(s2);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    ray_err_t err = ray_col_save(list, sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    ray_release(list);

    /* Load must succeed even though the file has a dotted name but no
     * segment entries.  The load should re-intern the three disk entries
     * at ids 0,1,2; then separately cache segment info for "user.name",
     * placing "user" and "name" at whatever transient ids follow. */
    err = ray_sym_load(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    TEST_ASSERT_EQ_I(ray_sym_find("alice",     5), 0);
    TEST_ASSERT_EQ_I(ray_sym_find("user.name", 9), 1);
    TEST_ASSERT_EQ_I(ray_sym_find("charlie",   7), 2);
    TEST_ASSERT_TRUE(ray_sym_is_dotted(1));

    const int64_t* segs = NULL;
    int n = ray_sym_segs(1, &segs);
    TEST_ASSERT_EQ_I(n, 2);
    TEST_ASSERT_EQ_I(ray_sym_find("user", 4), segs[0]);
    TEST_ASSERT_EQ_I(ray_sym_find("name", 4), segs[1]);

    remove(sym_path);
    remove(lk_path);
    PASS();
}

/* ---- sym_save_load_dotted ---------------------------------------------- */

/* Persistence must be stable across dotted names.  This exercises the
 * interaction between segment sub-interning and the disk-position ==
 * sym_id invariant that ray_sym_load/ray_sym_save rely on. */
static test_result_t test_sym_save_load_dotted(void) {
    const char* sym_path = "/tmp/test_sym_dotted.sym";
    remove(sym_path);
    char lk_path[4096];
    snprintf(lk_path, sizeof(lk_path), "%s.lk", sym_path);
    remove(lk_path);

    /* Intern a mix of plain and dotted names.  The dotted name triggers
     * segment sub-interning, appending "alpha" + "beta" at further ids. */
    int64_t id_plain      = ray_sym_intern("plain_one", 9);
    int64_t id_dotted     = ray_sym_intern("alpha.beta", 10);
    int64_t id_after      = ray_sym_intern("plain_two", 9);
    TEST_ASSERT((id_plain) >= (0), "id_plain >= 0");
    TEST_ASSERT((id_dotted) >= (0), "id_dotted >= 0");
    TEST_ASSERT((id_after) >= (0), "id_after >= 0");
    TEST_ASSERT_TRUE(ray_sym_is_dotted(id_dotted));

    /* Persist */
    ray_err_t err = ray_sym_save(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Tear down and reload from disk */
    ray_sym_destroy();
    (void)ray_sym_init();
    TEST_ASSERT_EQ_U(ray_sym_count(), 0);

    err = ray_sym_load(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Every pre-save sym_id must still resolve to its original string.
     * If load's segment sub-intern shifted ids, this is where it breaks. */
    ray_t* sp = ray_sym_str(id_plain);
    TEST_ASSERT_NOT_NULL(sp);
    TEST_ASSERT_EQ_U(ray_str_len(sp), 9);
    TEST_ASSERT_MEM_EQ(9, ray_str_ptr(sp), "plain_one");

    ray_t* sd = ray_sym_str(id_dotted);
    TEST_ASSERT_NOT_NULL(sd);
    TEST_ASSERT_EQ_U(ray_str_len(sd), 10);
    TEST_ASSERT_MEM_EQ(10, ray_str_ptr(sd), "alpha.beta");

    ray_t* sa = ray_sym_str(id_after);
    TEST_ASSERT_NOT_NULL(sa);
    TEST_ASSERT_EQ_U(ray_str_len(sa), 9);
    TEST_ASSERT_MEM_EQ(9, ray_str_ptr(sa), "plain_two");

    /* And the dotted metadata must be reconstructed — otherwise env lookup
     * of a dotted name restored from disk would silently fall back to a
     * flat scan and fail. */
    TEST_ASSERT_TRUE(ray_sym_is_dotted(id_dotted));
    const int64_t* segs = NULL;
    int n = ray_sym_segs(id_dotted, &segs);
    TEST_ASSERT_EQ_I(n, 2);
    TEST_ASSERT_NOT_NULL(segs);
    TEST_ASSERT_EQ_I(ray_sym_find("alpha", 5), segs[0]);
    TEST_ASSERT_EQ_I(ray_sym_find("beta", 4), segs[1]);

    /* Cleanup */
    remove(sym_path);
    remove(lk_path);
    PASS();
}

/* ---- sym_load_missing_file --------------------------------------------- */

static test_result_t test_sym_load_missing(void) {
    ray_err_t err = ray_sym_load("/tmp/nonexistent_sym_file_xyz.sym");
    TEST_ASSERT((err) != (RAY_OK), "err != RAY_OK");
    TEST_ASSERT_EQ_U(ray_sym_count(), 0);

    PASS();
}

/* ---- ray_sym_name_fn (src/ops/strop.c) -------------------------------- */

/* Atom RAY_I64 (sym ID) → RAY_SYM atom: line 254-258 of strop.c. */
static test_result_t test_sym_name_fn_atom_i64(void) {
    int64_t id = (int64_t)ray_sym_intern("hello", 5);
    ray_t* in  = ray_i64(id);
    ray_t* out = ray_sym_name_fn(in);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I(out->type, -RAY_SYM);
    TEST_ASSERT_EQ_I(out->i64, id);
    ray_release(out);
    ray_release(in);
    PASS();
}

/* Atom RAY_I64 with negative ID → domain error (line 255). */
static test_result_t test_sym_name_fn_atom_negative_id(void) {
    ray_t* in  = ray_i64(-1);
    ray_t* out = ray_sym_name_fn(in);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(RAY_IS_ERR(out));
    ray_release(out);
    ray_release(in);
    PASS();
}

/* Atom RAY_I64 with out-of-range ID → domain error (line 255). */
static test_result_t test_sym_name_fn_atom_unknown_id(void) {
    /* sym IDs start at 0 — pick a large id that hasn't been interned. */
    ray_t* in  = ray_i64(99999);
    ray_t* out = ray_sym_name_fn(in);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(RAY_IS_ERR(out));
    ray_release(out);
    ray_release(in);
    PASS();
}

/* Vector RAY_I64 of valid IDs → RAY_SYM vector (lines 259-273). */
static test_result_t test_sym_name_fn_vec_i64(void) {
    int64_t a = (int64_t)ray_sym_intern("foo", 3);
    int64_t b = (int64_t)ray_sym_intern("bar", 3);
    int64_t c = (int64_t)ray_sym_intern("baz", 3);
    int64_t ids[3] = { a, b, c };
    ray_t* in  = ray_vec_from_raw(RAY_I64, ids, 3);
    ray_t* out = ray_sym_name_fn(in);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I(out->type, RAY_SYM);
    TEST_ASSERT_EQ_I(out->len, 3);
    ray_release(out);
    ray_release(in);
    PASS();
}

/* Vector RAY_I64 with one invalid ID → domain error (lines 263-265). */
static test_result_t test_sym_name_fn_vec_invalid_id(void) {
    int64_t a = (int64_t)ray_sym_intern("ok", 2);
    int64_t ids[2] = { a, -7 };
    ray_t* in  = ray_vec_from_raw(RAY_I64, ids, 2);
    ray_t* out = ray_sym_name_fn(in);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(RAY_IS_ERR(out));
    ray_release(out);
    ray_release(in);
    PASS();
}

/* Already a -RAY_SYM atom → passthrough with retain (line 276-278). */
static test_result_t test_sym_name_fn_passthrough_atom(void) {
    int64_t id = (int64_t)ray_sym_intern("alpha", 5);
    ray_t* in  = ray_sym(id);
    TEST_ASSERT_EQ_I(in->type, -RAY_SYM);
    ray_t* out = ray_sym_name_fn(in);
    TEST_ASSERT_EQ_PTR(out, in);
    ray_release(out);
    ray_release(in);
    PASS();
}

/* RAY_SYM vector → passthrough (line 276-278). */
static test_result_t test_sym_name_fn_passthrough_vec(void) {
    int64_t a = (int64_t)ray_sym_intern("p", 1);
    int64_t b = (int64_t)ray_sym_intern("q", 2);
    int64_t ids[2] = { a, b };
    ray_t* in = ray_vec_from_raw(RAY_SYM, ids, 2);
    ray_t* out = ray_sym_name_fn(in);
    TEST_ASSERT_EQ_PTR(out, in);
    ray_release(out);
    ray_release(in);
    PASS();
}

/* Empty RAY_I64 vector → fresh empty RAY_SYM vector (the second-`if` body
 * runs the validation loop zero times then ray_vec_new(RAY_SYM, 0)).
 * Note: the `x->len == 0` arm in the third `if` is dead code for
 * RAY_I64 because the RAY_I64 branch above already matched. */
static test_result_t test_sym_name_fn_empty_i64_vec(void) {
    ray_t* in  = ray_vec_new(RAY_I64, 0);
    ray_t* out = ray_sym_name_fn(in);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I(out->type, RAY_SYM);
    TEST_ASSERT_EQ_I(out->len, 0);
    ray_release(out);
    ray_release(in);
    PASS();
}

/* Empty RAY_SYM vector → passthrough (the third `if` matches RAY_SYM). */
static test_result_t test_sym_name_fn_empty_sym_vec(void) {
    ray_t* in  = ray_vec_new(RAY_SYM, 0);
    ray_t* out = ray_sym_name_fn(in);
    TEST_ASSERT_EQ_PTR(out, in);
    ray_release(out);
    ray_release(in);
    PASS();
}

/* Wrong-type atom → type error (line 280). */
static test_result_t test_sym_name_fn_wrong_type(void) {
    /* Build an F64 atom — neither I64 nor SYM. */
    double v = 3.14;
    ray_t* in = ray_vec_from_raw(RAY_F64, &v, 1);
    in->type = -RAY_F64;
    ray_t* out = ray_sym_name_fn(in);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(RAY_IS_ERR(out));
    ray_release(out);
    ray_release(in);
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

/* ─── src/table/sym.h inline-fn coverage ───────────────────────── */

static test_result_t test_sym_dict_width_w32_w64(void) {
    /* W8 boundary */
    TEST_ASSERT_EQ_U(ray_sym_dict_width(0),   RAY_SYM_W8);
    TEST_ASSERT_EQ_U(ray_sym_dict_width(255),  RAY_SYM_W8);
    /* W16 boundary */
    TEST_ASSERT_EQ_U(ray_sym_dict_width(256),    RAY_SYM_W16);
    TEST_ASSERT_EQ_U(ray_sym_dict_width(65535),  RAY_SYM_W16);
    /* W32 branch (line 57 — previously never hit) */
    TEST_ASSERT_EQ_U(ray_sym_dict_width(65536),      RAY_SYM_W32);
    TEST_ASSERT_EQ_U(ray_sym_dict_width(4294967295LL), RAY_SYM_W32);
    /* W64 fallthrough (line 58 — previously never hit) */
    TEST_ASSERT_EQ_U(ray_sym_dict_width(4294967296LL), RAY_SYM_W64);
    TEST_ASSERT_EQ_U(ray_sym_dict_width(INT64_MAX),    RAY_SYM_W64);

    PASS();
}

/* ---- sym_elem_size_non_sym -------------------------------------------- */

/* ray_sym_elem_size: non-RAY_SYM type must fall through to ray_elem_size
 * (line 64 in test_sym.c's instantiation — always 0 in that TU). */
static test_result_t test_sym_elem_size_non_sym(void) {
    /* RAY_BOOL = 1 byte, RAY_I32 = 4 bytes, RAY_I64 = 8 bytes, RAY_F64 = 8 */
    TEST_ASSERT_EQ_U(ray_sym_elem_size(RAY_BOOL, 0), 1);
    TEST_ASSERT_EQ_U(ray_sym_elem_size(RAY_I32,  0), 4);
    TEST_ASSERT_EQ_U(ray_sym_elem_size(RAY_I64,  0), 8);
    TEST_ASSERT_EQ_U(ray_sym_elem_size(RAY_F64,  0), 8);
    /* RAY_SYM path still works for completeness */
    TEST_ASSERT_EQ_U(ray_sym_elem_size(RAY_SYM, RAY_SYM_W8),  1);
    TEST_ASSERT_EQ_U(ray_sym_elem_size(RAY_SYM, RAY_SYM_W16), 2);
    TEST_ASSERT_EQ_U(ray_sym_elem_size(RAY_SYM, RAY_SYM_W32), 4);
    TEST_ASSERT_EQ_U(ray_sym_elem_size(RAY_SYM, RAY_SYM_W64), 8);

    PASS();
}

/* ---- sym_read_write_w32 ----------------------------------------------- */

/* ray_read_sym / ray_write_sym W32 case (lines 73/85 in test_sym.c TU).
 * Also exercises the W8/W16/W64 paths to keep the switch fully covered. */
static test_result_t test_sym_read_write_all_widths(void) {
    /* Buffers large enough for 4 elements at the widest (W64 = 8 bytes each) */
    uint8_t  buf8[4]  = {0};
    uint16_t buf16[4] = {0};
    uint32_t buf32[4] = {0};
    int64_t  buf64[4] = {0};

    /* W8 */
    ray_write_sym(buf8,  0, 42,  RAY_SYM, RAY_SYM_W8);
    ray_write_sym(buf8,  1, 200, RAY_SYM, RAY_SYM_W8);
    TEST_ASSERT_EQ_I(ray_read_sym(buf8, 0, RAY_SYM, RAY_SYM_W8), 42);
    TEST_ASSERT_EQ_I(ray_read_sym(buf8, 1, RAY_SYM, RAY_SYM_W8), 200);

    /* W16 */
    ray_write_sym(buf16, 0, 1000,  RAY_SYM, RAY_SYM_W16);
    ray_write_sym(buf16, 2, 65000, RAY_SYM, RAY_SYM_W16);
    TEST_ASSERT_EQ_I(ray_read_sym(buf16, 0, RAY_SYM, RAY_SYM_W16), 1000);
    TEST_ASSERT_EQ_I(ray_read_sym(buf16, 2, RAY_SYM, RAY_SYM_W16), 65000);

    /* W32 — previously uncovered in test_sym.c TU */
    ray_write_sym(buf32, 0, 70000,      RAY_SYM, RAY_SYM_W32);
    ray_write_sym(buf32, 3, 4000000000ULL, RAY_SYM, RAY_SYM_W32);
    TEST_ASSERT_EQ_I(ray_read_sym(buf32, 0, RAY_SYM, RAY_SYM_W32), 70000);
    TEST_ASSERT_EQ_I(ray_read_sym(buf32, 3, RAY_SYM, RAY_SYM_W32), (int64_t)4000000000ULL);

    /* W64 */
    ray_write_sym(buf64, 0, (uint64_t)5000000000LL, RAY_SYM, RAY_SYM_W64);
    ray_write_sym(buf64, 1, 7,                      RAY_SYM, RAY_SYM_W64);
    TEST_ASSERT_EQ_I(ray_read_sym(buf64, 0, RAY_SYM, RAY_SYM_W64), 5000000000LL);
    TEST_ASSERT_EQ_I(ray_read_sym(buf64, 1, RAY_SYM, RAY_SYM_W64), 7);

    PASS();
}

/* ---- Suite definition -------------------------------------------------- */


/* ─── src/table/sym.c body coverage ────────────────────────────── */

static test_result_t test_sym_cache_segs_trailing_dot(void) {
    /* Insert trailing-dot name without segment processing. */
    int64_t id = ray_sym_intern_no_split("foo.", 4);
    TEST_ASSERT((id) >= (0), "id >= 0");
    /* Not yet scanned. */
    TEST_ASSERT_FALSE(ray_sym_is_dotted(id));

    /* Rebuild must succeed and must NOT mark the trailing-dot sym as dotted. */
    TEST_ASSERT_EQ_I(ray_sym_rebuild_segments(), RAY_OK);
    TEST_ASSERT_FALSE(ray_sym_is_dotted(id));

    /* A normal intern of the same name also sees it as plain. */
    int64_t id2 = ray_sym_intern("foo.", 4);
    TEST_ASSERT_EQ_I(id2, id);
    TEST_ASSERT_FALSE(ray_sym_is_dotted(id2));

    PASS();
}

/* ---- sym_null_path ---------------------------------------------------- */

static test_result_t test_sym_save_null_path(void) {
    ray_err_t err = ray_sym_save(NULL);
    TEST_ASSERT((err) != (RAY_OK), "save(NULL) should fail");
    PASS();
}

static test_result_t test_sym_load_null_path(void) {
    ray_err_t err = ray_sym_load(NULL);
    TEST_ASSERT((err) != (RAY_OK), "load(NULL) should fail");
    PASS();
}

/* ---- sym_load_non_list ------------------------------------------------- */

/* ray_sym_load rejects a valid STRL file that contains something other than
 * a RAY_LIST (e.g. a RAY_I64 vector). */
static test_result_t test_sym_load_non_list(void) {
    const char* sym_path = "/tmp/test_sym_nonlist.sym";
    remove(sym_path);

    /* Write a RAY_I64 vector instead of a RAY_LIST. */
    ray_t* vec = ray_vec_new(RAY_I64, 3);
    TEST_ASSERT_NOT_NULL(vec);
    int64_t v0 = 1, v1 = 2, v2 = 3;
    vec = ray_vec_append(vec, &v0);
    vec = ray_vec_append(vec, &v1);
    vec = ray_vec_append(vec, &v2);
    TEST_ASSERT_NOT_NULL(vec);
    ray_err_t err = ray_col_save(vec, sym_path);
    ray_release(vec);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Loading must fail because type != RAY_LIST. */
    err = ray_sym_load(sym_path);
    TEST_ASSERT((err) != (RAY_OK), "load non-list should fail");
    TEST_ASSERT_EQ_U(ray_sym_count(), 0);

    remove(sym_path);
    char lk_path[4096];
    snprintf(lk_path, sizeof(lk_path), "%s.lk", sym_path);
    remove(lk_path);
    PASS();
}

/* ---- sym_load_stale_prefix -------------------------------------------- */

/* ray_sym_load rejects a file that has fewer entries than what was
 * previously persisted (stale / truncated on disk). */
static test_result_t test_sym_load_stale_prefix(void) {
    const char* sym_path = "/tmp/test_sym_stale.sym";
    remove(sym_path);
    char lk_path[4096];
    snprintf(lk_path, sizeof(lk_path), "%s.lk", sym_path);
    remove(lk_path);

    /* Intern and save 3 symbols so persisted_count == 3. */
    ray_sym_intern("aaa", 3);
    ray_sym_intern("bbb", 3);
    ray_sym_intern("ccc", 3);
    ray_err_t err = ray_sym_save(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Reload from the same file so persisted_count stays 3. */
    err = ray_sym_load(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Now overwrite the sym file on disk with only 2 entries (stale). */
    ray_t* short_list = ray_list_new(2);
    TEST_ASSERT_NOT_NULL(short_list);
    ray_t* s0 = ray_str("aaa", 3);
    ray_t* s1 = ray_str("bbb", 3);
    short_list = ray_list_append(short_list, s0); ray_release(s0);
    short_list = ray_list_append(short_list, s1); ray_release(s1);
    TEST_ASSERT_NOT_NULL(short_list);
    err = ray_col_save(short_list, sym_path);
    ray_release(short_list);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load must fail: disk has 2 entries but persisted_count==3. */
    err = ray_sym_load(sym_path);
    TEST_ASSERT((err) != (RAY_OK), "stale file should be rejected");

    remove(sym_path);
    remove(lk_path);
    PASS();
}

/* ---- sym_load_prefix_mismatch ----------------------------------------- */

/* ray_sym_load rejects a reload where the first (already-loaded) entry
 * has a different string than what is in memory. */
static test_result_t test_sym_load_prefix_mismatch(void) {
    const char* sym_path = "/tmp/test_sym_mismatch.sym";
    remove(sym_path);
    char lk_path[4096];
    snprintf(lk_path, sizeof(lk_path), "%s.lk", sym_path);
    remove(lk_path);

    /* Intern and save 2 symbols. */
    ray_sym_intern("dog", 3);
    ray_sym_intern("cat", 3);
    ray_err_t err = ray_sym_save(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Reload so persisted_count == 2. */
    err = ray_sym_load(sym_path);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Overwrite sym file with different strings at same positions. */
    ray_t* bad_list = ray_list_new(2);
    TEST_ASSERT_NOT_NULL(bad_list);
    ray_t* s0 = ray_str("fox", 3);   /* was "dog" */
    ray_t* s1 = ray_str("cat", 3);
    bad_list = ray_list_append(bad_list, s0); ray_release(s0);
    bad_list = ray_list_append(bad_list, s1); ray_release(s1);
    TEST_ASSERT_NOT_NULL(bad_list);
    err = ray_col_save(bad_list, sym_path);
    ray_release(bad_list);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load must fail: prefix entry 0 has "fox" on disk but "dog" in memory. */
    err = ray_sym_load(sym_path);
    TEST_ASSERT((err) != (RAY_OK), "mismatched prefix should be rejected");

    remove(sym_path);
    remove(lk_path);
    PASS();
}

/* ---- sym_load_id_mismatch --------------------------------------------- */

/* ray_sym_load rejects a file when a disk entry would be assigned an
 * in-memory id != its disk position.  This happens when a transient
 * symbol already occupies the slot. */
static test_result_t test_sym_load_id_mismatch(void) {
    const char* sym_path = "/tmp/test_sym_idmismatch.sym";
    remove(sym_path);
    char lk_path[4096];
    snprintf(lk_path, sizeof(lk_path), "%s.lk", sym_path);
    remove(lk_path);

    /* Write a file that contains just one entry: "zebra". */
    ray_t* file_list = ray_list_new(1);
    TEST_ASSERT_NOT_NULL(file_list);
    ray_t* s0 = ray_str("zebra", 5);
    file_list = ray_list_append(file_list, s0); ray_release(s0);
    TEST_ASSERT_NOT_NULL(file_list);
    ray_err_t err = ray_col_save(file_list, sym_path);
    ray_release(file_list);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Intern a different symbol first — it occupies id=0. */
    int64_t transient_id = ray_sym_intern("apple", 5);
    TEST_ASSERT_EQ_I(transient_id, 0);

    /* Now load the file: "zebra" would need id=0 but "apple" is already there. */
    err = ray_sym_load(sym_path);
    TEST_ASSERT((err) != (RAY_OK), "id mismatch should be rejected");

    remove(sym_path);
    remove(lk_path);
    PASS();
}

/* ---- sym_save_existing_not_list --------------------------------------- */

/* ray_sym_save reads the existing file at the path before writing.
 * If the file is readable but its contents are not a RAY_LIST, it should
 * return RAY_ERR_CORRUPT rather than overwriting. */
static test_result_t test_sym_save_existing_not_list(void) {
    const char* sym_path = "/tmp/test_sym_save_notlist.sym";
    remove(sym_path);
    char lk_path[4096];
    snprintf(lk_path, sizeof(lk_path), "%s.lk", sym_path);
    remove(lk_path);

    /* Write a RAY_I64 vector at the sym path. */
    ray_t* vec = ray_vec_new(RAY_I64, 2);
    TEST_ASSERT_NOT_NULL(vec);
    int64_t v0 = 10, v1 = 20;
    vec = ray_vec_append(vec, &v0);
    vec = ray_vec_append(vec, &v1);
    TEST_ASSERT_NOT_NULL(vec);
    ray_err_t err = ray_col_save(vec, sym_path);
    ray_release(vec);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Intern a symbol so there is something to save. */
    ray_sym_intern("hello", 5);

    /* ray_sym_save must fail because existing file is not a RAY_LIST. */
    err = ray_sym_save(sym_path);
    TEST_ASSERT((err) != (RAY_OK), "save over non-list file should fail");

    remove(sym_path);
    remove(lk_path);
    PASS();
}

/* ---- sym_intern_prehashed_basic --------------------------------------- */

/* Verify that ray_sym_intern_prehashed works and is consistent with
 * ray_sym_intern. */
static test_result_t test_sym_intern_prehashed_basic(void) {
    int64_t id1 = ray_sym_intern("pretest", 7);
    TEST_ASSERT((id1) >= (0), "id1 >= 0");

    /* Using prehashed with the same string returns the same id. */
    uint32_t h = (uint32_t)ray_hash_bytes("pretest", 7);
    int64_t id2 = ray_sym_intern_prehashed(h, "pretest", 7);
    TEST_ASSERT_EQ_I(id1, id2);

    /* Prehashed with a new name creates it. */
    uint32_t h2 = (uint32_t)ray_hash_bytes("newpre", 6);
    int64_t id3 = ray_sym_intern_prehashed(h2, "newpre", 6);
    TEST_ASSERT((id3) >= (0), "id3 >= 0");
    TEST_ASSERT((id3) != (id1), "id3 != id1");

    PASS();
}

/* ---- sym_str_invalid_id ----------------------------------------------- */

/* ray_sym_str with out-of-range id should return NULL. */
static test_result_t test_sym_str_invalid_id(void) {
    /* No syms interned yet. */
    ray_t* s = ray_sym_str(-1);
    TEST_ASSERT_NULL(s);

    ray_t* s2 = ray_sym_str(9999);
    TEST_ASSERT_NULL(s2);

    /* After one intern, id=0 is valid but id=1 is not. */
    ray_sym_intern("x", 1);
    ray_t* s3 = ray_sym_str(0);
    TEST_ASSERT_NOT_NULL(s3);
    ray_t* s4 = ray_sym_str(1);
    TEST_ASSERT_NULL(s4);

    PASS();
}

/* ---- sym_is_dotted_invalid_id ----------------------------------------- */

/* ray_sym_is_dotted with out-of-range ids returns false, not a crash. */
static test_result_t test_sym_is_dotted_invalid_id(void) {
    TEST_ASSERT_FALSE(ray_sym_is_dotted(-1));
    TEST_ASSERT_FALSE(ray_sym_is_dotted(9999));
    PASS();
}

/* ---- sym_segs_invalid_id ---------------------------------------------- */

/* ray_sym_segs with out-of-range id returns 0. */
static test_result_t test_sym_segs_invalid_id(void) {
    const int64_t* segs = NULL;
    TEST_ASSERT_EQ_I(ray_sym_segs(-1, &segs), 0);
    TEST_ASSERT_EQ_I(ray_sym_segs(9999, &segs), 0);
    PASS();
}

/* ---- sym_find_after_many ---------------------------------------------- */

/* Ensure that hash table linear probing works after many collisions:
 * intern 512 unique names (forces ht_grow) then verify all are findable. */
static test_result_t test_sym_find_after_grow(void) {
    char buf[32];
    for (int i = 0; i < 512; i++) {
        int len = snprintf(buf, sizeof(buf), "grow_%03d", i);
        int64_t id = ray_sym_intern(buf, (size_t)len);
        TEST_ASSERT((id) >= (0), "id >= 0");
    }
    /* Verify all 512 are findable. */
    for (int i = 0; i < 512; i++) {
        int len = snprintf(buf, sizeof(buf), "grow_%03d", i);
        int64_t id = ray_sym_find(buf, (size_t)len);
        TEST_ASSERT((id) >= (0), "found grow sym");
    }
    PASS();
}

/* ---- sym_ensure_cap_zero ---------------------------------------------- */

/* Calling ray_sym_ensure_cap(0) is a no-op that returns true. */
static test_result_t test_sym_ensure_cap_zero(void) {
    TEST_ASSERT_TRUE(ray_sym_ensure_cap(0));
    PASS();
}

/* ---- sym_ensure_cap_large --------------------------------------------- */

/* Pre-grow to a large capacity, then intern up to that capacity. */
static test_result_t test_sym_ensure_cap_large(void) {
    bool ok = ray_sym_ensure_cap(2000);
    TEST_ASSERT_TRUE(ok);
    /* After ensure_cap, str_cap >= 2000 — just verify we can intern that many. */
    char buf[32];
    for (int i = 0; i < 2000; i++) {
        int len = snprintf(buf, sizeof(buf), "ecap_%04d", i);
        int64_t id = ray_sym_intern(buf, (size_t)len);
        TEST_ASSERT((id) >= (0), "id >= 0");
    }
    TEST_ASSERT_EQ_U(ray_sym_count(), 2000);
    PASS();
}

/* ---- sym_dotted_leading_dot_with_second_dot ---------------------------- */

/* Leading dot followed by a second dot (`.sys.gc`) should be treated as
 * dotted, with segment 0 being `.sys` (including the leading dot). */
static test_result_t test_sym_dotted_leading_dot(void) {
    int64_t id = ray_sym_intern(".sys.gc", 7);
    TEST_ASSERT((id) >= (0), "id >= 0");
    TEST_ASSERT_TRUE(ray_sym_is_dotted(id));

    const int64_t* segs = NULL;
    int n = ray_sym_segs(id, &segs);
    TEST_ASSERT_EQ_I(n, 2);
    /* Segment 0 is `.sys` (4 bytes), segment 1 is `gc` (2 bytes). */
    int64_t seg0_id = ray_sym_find(".sys", 4);
    int64_t seg1_id = ray_sym_find("gc",   2);
    TEST_ASSERT((seg0_id) >= (0), "seg0_id >= 0");
    TEST_ASSERT((seg1_id) >= (0), "seg1_id >= 0");
    TEST_ASSERT_EQ_I(segs[0], seg0_id);
    TEST_ASSERT_EQ_I(segs[1], seg1_id);

    PASS();
}

/* ---- Suite definition -------------------------------------------------- */


const test_entry_t sym_entries[] = {
    { "sym/init_destroy", test_sym_init_destroy, sym_setup, sym_teardown },
    { "sym/intern_basic", test_sym_intern_basic, sym_setup, sym_teardown },
    { "sym/intern_duplicate", test_sym_intern_duplicate, sym_setup, sym_teardown },
    { "sym/find_existing", test_sym_find_existing, sym_setup, sym_teardown },
    { "sym/find_missing", test_sym_find_missing, sym_setup, sym_teardown },
    { "sym/str_roundtrip", test_sym_str_roundtrip, sym_setup, sym_teardown },
    { "sym/count", test_sym_count, sym_setup, sym_teardown },
    { "sym/many", test_sym_many, sym_setup, sym_teardown },
    { "sym/bulk", test_sym_bulk, sym_setup, sym_teardown },
    { "sym/save_load_roundtrip", test_sym_save_load_roundtrip, sym_setup, sym_teardown },
    { "sym/save_append_only", test_sym_save_append_only, sym_setup, sym_teardown },
    { "sym/load_corrupt", test_sym_load_corrupt, sym_setup, sym_teardown },
    { "sym/load_truncated", test_sym_load_truncated, sym_setup, sym_teardown },
    { "sym/load_missing", test_sym_load_missing, sym_setup, sym_teardown },
    { "sym/dotted_detect", test_sym_dotted_detect, sym_setup, sym_teardown },
    { "sym/dotted_segments", test_sym_dotted_segments, sym_setup, sym_teardown },
    { "sym/dotted_triple", test_sym_dotted_triple, sym_setup, sym_teardown },
    { "sym/dotted_idempotent", test_sym_dotted_idempotent, sym_setup, sym_teardown },
    { "sym/dotted_empty_segment", test_sym_dotted_rejects_empty_segment, sym_setup, sym_teardown },
    { "sym/save_load_dotted", test_sym_save_load_dotted, sym_setup, sym_teardown },
    { "sym/load_legacy_dotted", test_sym_load_legacy_dotted, sym_setup, sym_teardown },
    { "sym/rebuild_segments_contract", test_sym_rebuild_segments_contract, sym_setup, sym_teardown },
    { "sym/dotted_reintern_retries_cache", test_sym_dotted_reintern_retries_cache, sym_setup, sym_teardown },
    { "sym/intern_atomic_cache", test_sym_intern_atomic_cache, sym_setup, sym_teardown },
    { "sym/intern_atomic_no_orphans", test_sym_intern_atomic_no_orphans, sym_setup, sym_teardown },
    { "sym/intern_long_segments", test_sym_intern_long_segments, sym_setup, sym_teardown },
    { "sym/bytes_upper_covers_arena", test_sym_bytes_upper_covers_arena, sym_setup, sym_teardown },

    /* ray_sym_name_fn (src/ops/strop.c) */
    { "sym/name_fn/atom_i64",           test_sym_name_fn_atom_i64,         sym_setup, sym_teardown },
    { "sym/name_fn/atom_negative_id",   test_sym_name_fn_atom_negative_id, sym_setup, sym_teardown },
    { "sym/name_fn/atom_unknown_id",    test_sym_name_fn_atom_unknown_id,  sym_setup, sym_teardown },
    { "sym/name_fn/vec_i64",            test_sym_name_fn_vec_i64,          sym_setup, sym_teardown },
    { "sym/name_fn/vec_invalid_id",     test_sym_name_fn_vec_invalid_id,   sym_setup, sym_teardown },
    { "sym/name_fn/passthrough_atom",   test_sym_name_fn_passthrough_atom, sym_setup, sym_teardown },
    { "sym/name_fn/passthrough_vec",    test_sym_name_fn_passthrough_vec,  sym_setup, sym_teardown },
    { "sym/name_fn/empty_i64_vec",      test_sym_name_fn_empty_i64_vec,    sym_setup, sym_teardown },
    { "sym/name_fn/empty_sym_vec",      test_sym_name_fn_empty_sym_vec,    sym_setup, sym_teardown },
    { "sym/name_fn/wrong_type",         test_sym_name_fn_wrong_type,       sym_setup, sym_teardown },

    /* src/table/sym.h inline-fn coverage */
    { "sym/dict_width_w32_w64",         test_sym_dict_width_w32_w64,       sym_setup, sym_teardown },
    { "sym/elem_size_non_sym",          test_sym_elem_size_non_sym,        sym_setup, sym_teardown },
    { "sym/read_write_all_widths",      test_sym_read_write_all_widths,    sym_setup, sym_teardown },

    /* src/table/sym.c body coverage */
    { "sym/cache_segs_trailing_dot",    test_sym_cache_segs_trailing_dot,  sym_setup, sym_teardown },
    { "sym/save_null_path",             test_sym_save_null_path,           sym_setup, sym_teardown },
    { "sym/load_null_path",             test_sym_load_null_path,           sym_setup, sym_teardown },
    { "sym/load_non_list",              test_sym_load_non_list,            sym_setup, sym_teardown },
    { "sym/load_stale_prefix",          test_sym_load_stale_prefix,        sym_setup, sym_teardown },
    { "sym/load_prefix_mismatch",       test_sym_load_prefix_mismatch,     sym_setup, sym_teardown },
    { "sym/load_id_mismatch",           test_sym_load_id_mismatch,         sym_setup, sym_teardown },
    { "sym/save_existing_not_list",     test_sym_save_existing_not_list,   sym_setup, sym_teardown },
    { "sym/intern_prehashed_basic",     test_sym_intern_prehashed_basic,   sym_setup, sym_teardown },
    { "sym/str_invalid_id",             test_sym_str_invalid_id,           sym_setup, sym_teardown },
    { "sym/is_dotted_invalid_id",       test_sym_is_dotted_invalid_id,     sym_setup, sym_teardown },
    { "sym/segs_invalid_id",            test_sym_segs_invalid_id,          sym_setup, sym_teardown },
    { "sym/find_after_grow",            test_sym_find_after_grow,          sym_setup, sym_teardown },
    { "sym/ensure_cap_zero",            test_sym_ensure_cap_zero,          sym_setup, sym_teardown },
    { "sym/ensure_cap_large",           test_sym_ensure_cap_large,         sym_setup, sym_teardown },
    { "sym/dotted_leading_dot",         test_sym_dotted_leading_dot,       sym_setup, sym_teardown },

    { NULL, NULL, NULL, NULL },
};


