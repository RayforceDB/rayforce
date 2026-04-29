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
    { NULL, NULL, NULL, NULL },
};


