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

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "mem/heap.h"
#include "mem/arena.h"
#include <string.h>
#include <stdio.h>

static test_result_t test_arena_release_noop(void) {
    ray_heap_init();

    /* Allocate a block and mark it as arena-owned */
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    v->attrs |= RAY_ATTR_ARENA;

    /* ray_release should be a no-op — block should not be freed */
    ray_release(v);

    /* If ray_release freed it, accessing v->attrs would be UB / ASan error.
     * Since it's a no-op, we can still read it. */
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_ARENA);

    /* Clean up: remove flag and release properly */
    v->attrs &= (uint8_t)~RAY_ATTR_ARENA;
    ray_release(v);

    ray_heap_destroy();
    PASS();
}

static test_result_t test_arena_alloc_basic(void) {
    ray_heap_init();

    ray_arena_t* arena = ray_arena_new(4096);
    TEST_ASSERT_NOT_NULL(arena);

    /* Allocate a small block (header only, 0 data bytes) */
    ray_t* v1 = ray_arena_alloc(arena, 0);
    TEST_ASSERT_NOT_NULL(v1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v1));
    /* Must be 32-byte aligned */
    TEST_ASSERT_EQ_U((uintptr_t)v1 % 32, 0);
    /* Must have arena flag */
    TEST_ASSERT_TRUE(v1->attrs & RAY_ATTR_ARENA);
    /* Must have rc=1 */
    TEST_ASSERT_EQ_U(v1->rc, 1);

    /* Allocate a block with data */
    ray_t* v2 = ray_arena_alloc(arena, 100);
    TEST_ASSERT_NOT_NULL(v2);
    TEST_ASSERT_EQ_U((uintptr_t)v2 % 32, 0);
    /* v2 should be different from v1 */
    TEST_ASSERT_TRUE(v1 != v2);

    /* ray_data(v2) should be writable for 100 bytes */
    memset(ray_data(v2), 0xAB, 100);
    TEST_ASSERT_EQ_U(((uint8_t*)ray_data(v2))[0], 0xAB);
    TEST_ASSERT_EQ_U(((uint8_t*)ray_data(v2))[99], 0xAB);

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_arena_grows_across_chunks(void) {
    ray_heap_init();

    /* Tiny chunk size to force multiple chunks */
    ray_arena_t* arena = ray_arena_new(256);
    TEST_ASSERT_NOT_NULL(arena);

    /* Allocate many blocks — should span multiple chunks */
    for (int i = 0; i < 100; i++) {
        ray_t* v = ray_arena_alloc(arena, 64);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_ARENA);
    }

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_arena_reset(void) {
    ray_heap_init();

    ray_arena_t* arena = ray_arena_new(4096);

    ray_t* v1 = ray_arena_alloc(arena, 0);
    TEST_ASSERT_NOT_NULL(v1);

    ray_arena_reset(arena);

    /* After reset, arena should be usable with a fresh chunk */
    ray_t* v2 = ray_arena_alloc(arena, 0);
    TEST_ASSERT_NOT_NULL(v2);
    TEST_ASSERT_TRUE(v2->attrs & RAY_ATTR_ARENA);

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_arena_oversize(void) {
    ray_heap_init();

    /* Chunk size 256, but request 1024 bytes of data */
    ray_arena_t* arena = ray_arena_new(256);

    ray_t* v = ray_arena_alloc(arena, 1024);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_ARENA);
    memset(ray_data(v), 0xCD, 1024);

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_arena_destroy_null(void) {
    /* Must not crash */
    ray_arena_destroy(NULL);
    PASS();
}

static test_result_t test_arena_reset_multi_chunk(void) {
    ray_heap_init();

    /* Tiny chunk to force multiple chunks */
    ray_arena_t* arena = ray_arena_new(256);
    TEST_ASSERT_NOT_NULL(arena);

    /* Allocate enough to span several chunks */
    for (int i = 0; i < 50; i++) {
        ray_t* v = ray_arena_alloc(arena, 64);
        TEST_ASSERT_NOT_NULL(v);
    }

    /* Reset should free extra chunks */
    ray_arena_reset(arena);

    /* Arena should still be usable after reset */
    ray_t* v = ray_arena_alloc(arena, 64);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_ARENA);
    memset(ray_data(v), 0xAB, 64);
    TEST_ASSERT_EQ_U(((uint8_t*)ray_data(v))[0], 0xAB);

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_arena_retain_noop(void) {
    ray_heap_init();

    ray_arena_t* arena = ray_arena_new(4096);
    ray_t* v = ray_arena_alloc(arena, 0);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQ_U(v->rc, 1);

    /* ray_retain should be a no-op for arena blocks */
    ray_retain(v);
    TEST_ASSERT_EQ_U(v->rc, 1);

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_arena_cow_noop(void) {
    ray_heap_init();

    ray_arena_t* arena = ray_arena_new(4096);
    ray_t* v = ray_arena_alloc(arena, 0);
    TEST_ASSERT_NOT_NULL(v);

    /* ray_cow should return same pointer for arena blocks */
    ray_t* cow_result = ray_cow(v);
    TEST_ASSERT_EQ_PTR(v, cow_result);

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

static test_result_t test_arena_sym_intern(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Intern many strings — should use arena, not buddy allocator */
    for (int i = 0; i < 10000; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "sym_%d", i);
        int64_t id = ray_sym_intern(buf, (size_t)len);
        TEST_ASSERT((id) >= (0), "id >= 0");
    }

    /* Verify strings are accessible */
    ray_t* s = ray_sym_str(0);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->attrs & RAY_ATTR_ARENA);

    /* Verify roundtrip */
    int64_t id = ray_sym_find("sym_999", 7);
    TEST_ASSERT((id) >= (0), "id >= 0");
    ray_t* found = ray_sym_str(id);
    TEST_ASSERT_EQ_I(ray_str_len(found), 7);
    TEST_ASSERT_MEM_EQ(7, ray_str_ptr(found), "sym_999");

    /* Verify long strings (>=7 bytes) use the arena CHAR vector path */
    const char* long_str = "this_is_a_long_symbol_name_for_testing";
    size_t long_len = strlen(long_str);
    int64_t long_id = ray_sym_intern(long_str, long_len);
    TEST_ASSERT((long_id) >= (0), "long_id >= 0");
    ray_t* long_s = ray_sym_str(long_id);
    TEST_ASSERT_NOT_NULL(long_s);
    TEST_ASSERT_TRUE(long_s->attrs & RAY_ATTR_ARENA);
    TEST_ASSERT_EQ_I(ray_str_len(long_s), (int64_t)long_len);
    TEST_ASSERT_MEM_EQ(long_len, ray_str_ptr(long_s), long_str);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---- ray_arena_new with tiny chunk_size (<256) -------------------------- *
 *
 * When chunk_size < 256, ray_arena_new clamps it to 256.  Passing 0 (or any
 * value below 256) exercises the `if (chunk_size < 256) chunk_size = 256;`
 * branch that was previously uncovered. */

static test_result_t test_arena_new_tiny_chunk(void) {
    ray_heap_init();

    /* Pass chunk_size=0 — must be clamped to 256 internally. */
    ray_arena_t* arena = ray_arena_new(0);
    TEST_ASSERT_NOT_NULL(arena);

    /* Allocation must still work after clamping. */
    ray_t* v = ray_arena_alloc(arena, 0);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_ARENA);
    TEST_ASSERT_EQ_U(v->rc, 1);

    /* Also try chunk_size=1 to cover another sub-256 value. */
    ray_arena_destroy(arena);
    arena = ray_arena_new(1);
    TEST_ASSERT_NOT_NULL(arena);
    v = ray_arena_alloc(arena, 10);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_ARENA);

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

/* ---- ray_arena_alloc NULL arena guard ----------------------------------- *
 *
 * ray_arena_alloc(NULL, n) must return NULL immediately. */

static test_result_t test_arena_alloc_null_arena(void) {
    ray_t* v = ray_arena_alloc(NULL, 0);
    TEST_ASSERT_NULL(v);
    v = ray_arena_alloc(NULL, 64);
    TEST_ASSERT_NULL(v);
    PASS();
}

/* ---- ray_arena_alloc nbytes overflow guard ------------------------------ *
 *
 * When nbytes > SIZE_MAX - 32 - (ARENA_ALIGN-1), ray_arena_alloc returns NULL
 * to prevent integer overflow during block_size computation. */

static test_result_t test_arena_alloc_overflow_nbytes(void) {
    ray_heap_init();

    ray_arena_t* arena = ray_arena_new(4096);
    TEST_ASSERT_NOT_NULL(arena);

    /* SIZE_MAX - 32 - 31 = SIZE_MAX - 63; anything > that overflows. */
    size_t huge = SIZE_MAX - 30;
    ray_t* v = ray_arena_alloc(arena, huge);
    TEST_ASSERT_NULL(v);

    /* Arena must still be usable after the rejected request. */
    v = ray_arena_alloc(arena, 0);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_ARENA);

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

/* ---- ray_arena_reserve NULL arena guard --------------------------------- */

static test_result_t test_arena_reserve_null_arena(void) {
    /* Must return false immediately without crashing. */
    bool ok = ray_arena_reserve(NULL, 64);
    TEST_ASSERT_FALSE(ok);
    ok = ray_arena_reserve(NULL, 0);
    TEST_ASSERT_FALSE(ok);
    PASS();
}

/* ---- ray_arena_reserve zero bytes --------------------------------------- *
 *
 * Reserving 0 bytes is a no-op that must return true. */

static test_result_t test_arena_reserve_zero(void) {
    ray_heap_init();

    ray_arena_t* arena = ray_arena_new(4096);
    TEST_ASSERT_NOT_NULL(arena);

    bool ok = ray_arena_reserve(arena, 0);
    TEST_ASSERT_TRUE(ok);

    /* Arena still functional. */
    ray_t* v = ray_arena_alloc(arena, 0);
    TEST_ASSERT_NOT_NULL(v);

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

/* ---- ray_arena_reserve bytes > chunk_size (new_cap bump) --------------- *
 *
 * When the reservation request exceeds arena->chunk_size, the new chunk
 * capacity is bumped to ARENA_ALIGN_UP(bytes).  The `if (bytes > new_cap)`
 * branch inside ray_arena_reserve was previously uncovered. */

static test_result_t test_arena_reserve_oversize(void) {
    ray_heap_init();

    /* Small default chunk_size so a large reserve definitely exceeds it. */
    ray_arena_t* arena = ray_arena_new(256);
    TEST_ASSERT_NOT_NULL(arena);

    /* Reserve more than 256 bytes — triggers the bytes > new_cap path. */
    bool ok = ray_arena_reserve(arena, 8192);
    TEST_ASSERT_TRUE(ok);

    /* Subsequent allocation of up to 8192 bytes must fit without another
     * chunk allocation. */
    ray_t* v = ray_arena_alloc(arena, 4096);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_ARENA);
    memset(ray_data(v), 0x5A, 4096);
    TEST_ASSERT_EQ_U(((uint8_t*)ray_data(v))[0], 0x5A);
    TEST_ASSERT_EQ_U(((uint8_t*)ray_data(v))[4095], 0x5A);

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

/* ---- ray_arena_total_used NULL arena ------------------------------------ */

static test_result_t test_arena_total_used_null(void) {
    /* Must return 0 without crashing. */
    size_t used = ray_arena_total_used(NULL);
    TEST_ASSERT_EQ_U(used, 0);
    PASS();
}

/* ---- ray_arena_total_used multi-chunk accounting ----------------------- *
 *
 * After allocations that span multiple chunks, total_used must equal the
 * sum of used bytes across all chunks. */

static test_result_t test_arena_total_used_multi_chunk(void) {
    ray_heap_init();

    /* Tiny chunk so each block forces a new chunk. */
    ray_arena_t* arena = ray_arena_new(64);
    TEST_ASSERT_NOT_NULL(arena);

    /* Make several allocations that overflow the tiny chunk repeatedly. */
    size_t before = ray_arena_total_used(arena);
    TEST_ASSERT_EQ_U(before, 0);

    for (int i = 0; i < 20; i++) {
        ray_t* v = ray_arena_alloc(arena, 64);
        TEST_ASSERT_NOT_NULL(v);
    }

    size_t after = ray_arena_total_used(arena);
    /* Each 64-byte-data alloc is ARENA_ALIGN_UP(32+64)=128 bytes; 20 allocs
     * spread across chunks → total_used > 0 and spans multiple chunks. */
    TEST_ASSERT((after) > (0), "total_used > 0");

    ray_arena_destroy(arena);
    ray_heap_destroy();
    PASS();
}

/* ---- a large arena spills past the anonymous watermark (#712) ----------- *
 *
 * A domain's string arena starts with small chunks.  Grown to 64 MiB under a
 * watermark only 4 MiB above the current footprint, it must not hold that in
 * anonymous RAM: its chunks grow until they are large enough to be mapped
 * over spill files.  Every string still reads back. */
static test_result_t test_arena_spills_past_watermark(void) {
#if defined(_WIN32)
    SKIP("file-backed spill is POSIX-only");
#endif
    ray_heap_init();
    ray_heap_direct_cache_drain();
    int64_t base = ray_heap_anon_committed();
    int64_t previous = ray_heap_anon_watermark();
    ray_heap_set_anon_watermark(base + INT64_C(4) * 1024 * 1024);

    ray_arena_t* arena = ray_arena_new(64 * 1024);
    char buf[200];
    memset(buf, 'x', sizeof(buf));
    const int n = 300000;   /* 300K strings of 200 bytes: ~78 MiB of atoms */
    ray_t* first = NULL;
    ray_t* last = NULL;
    bool ok = arena != NULL;
    for (int i = 0; i < n && ok; i++) {
        int k = snprintf(buf, 16, "%d", i);
        buf[k] = 'x';
        ray_t* a = ray_arena_str(arena, buf, sizeof(buf));
        if (!a) ok = false;
        if (i == 0) first = a;
        last = a;
    }
    int64_t grown = ray_heap_anon_committed() - base;
    bool bounded = grown < INT64_C(8) * 1024 * 1024;
    bool readable = ok && first && last &&
                    memcmp(ray_str_ptr(first), "0x", 2) == 0 &&
                    memcmp(ray_str_ptr(last), "299999x", 7) == 0 &&
                    ray_str_len(last) == sizeof(buf);
    ray_arena_destroy(arena);
    bool back = ray_heap_anon_committed() == base;
    ray_heap_set_anon_watermark(previous);
    ray_heap_destroy();
    TEST_ASSERT_TRUE(ok && readable);
    TEST_ASSERT_TRUE(bounded);
    TEST_ASSERT_TRUE(back);
    PASS();
}

/* ---- ray_arena_reset NULL arena guard ----------------------------------- */

static test_result_t test_arena_reset_null(void) {
    /* Must not crash. */
    ray_arena_reset(NULL);
    PASS();
}

const test_entry_t arena_entries[] = {
    { "arena/release_noop", test_arena_release_noop, NULL, NULL },
    { "arena/alloc_basic", test_arena_alloc_basic, NULL, NULL },
    { "arena/grows_across_chunks", test_arena_grows_across_chunks, NULL, NULL },
    { "arena/reset", test_arena_reset, NULL, NULL },
    { "arena/oversize", test_arena_oversize, NULL, NULL },
    { "arena/destroy_null", test_arena_destroy_null, NULL, NULL },
    { "arena/reset_multi_chunk", test_arena_reset_multi_chunk, NULL, NULL },
    { "arena/retain_noop", test_arena_retain_noop, NULL, NULL },
    { "arena/cow_noop", test_arena_cow_noop, NULL, NULL },
    { "arena/sym_intern", test_arena_sym_intern, NULL, NULL },
    { "arena/new_tiny_chunk",           test_arena_new_tiny_chunk,           NULL, NULL },
    { "arena/alloc_null_arena",         test_arena_alloc_null_arena,         NULL, NULL },
    { "arena/alloc_overflow_nbytes",    test_arena_alloc_overflow_nbytes,    NULL, NULL },
    { "arena/reserve_null_arena",       test_arena_reserve_null_arena,       NULL, NULL },
    { "arena/reserve_zero",             test_arena_reserve_zero,             NULL, NULL },
    { "arena/reserve_oversize",         test_arena_reserve_oversize,         NULL, NULL },
    { "arena/total_used_null",          test_arena_total_used_null,          NULL, NULL },
    { "arena/total_used_multi_chunk",   test_arena_total_used_multi_chunk,   NULL, NULL },
    { "arena/reset_null",               test_arena_reset_null,               NULL, NULL },
    { "arena/spills_past_watermark",    test_arena_spills_past_watermark,    NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


