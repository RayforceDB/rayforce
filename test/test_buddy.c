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

#define _POSIX_C_SOURCE 200809L  /* setenv / unsetenv */
#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "mem/heap.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void buddy_setup(void) {
    ray_heap_init();
}

static void buddy_teardown(void) {
    ray_heap_destroy();
}

/* ---- Basic alloc/free -------------------------------------------------- */

static test_result_t test_alloc_basic(void) {
    ray_t* v = ray_alloc(0);  /* minimum: just a header */
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_U(v->mmod, 0);
    TEST_ASSERT((v->order) >= (RAY_ORDER_MIN), "v->order >= RAY_ORDER_MIN");
    TEST_ASSERT_EQ_U(v->rc, 1);

    ray_free(v);
    PASS();
}

static test_result_t test_alloc_small_atom(void) {
    /* Atom: 0 bytes of data beyond the 32-byte header */
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    /* Header should be zeroed (except mmod, order, rc set by allocator) */
    TEST_ASSERT_EQ_I(v->type, 0);
    TEST_ASSERT_EQ_U(v->attrs, 0);
    ray_free(v);
    PASS();
}

static test_result_t test_alloc_medium_vector(void) {
    /* Vector of 100 i64s: 100 * 8 = 800 bytes of data */
    size_t data_size = 100 * sizeof(int64_t);
    ray_t* v = ray_alloc(data_size);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* We should be able to write and read data */
    int64_t* data = (int64_t*)ray_data(v);
    for (int i = 0; i < 100; i++) {
        data[i] = (int64_t)(i * 42);
    }
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQ_I(data[i], (int64_t)(i * 42));
    }

    ray_free(v);
    PASS();
}

static test_result_t test_alloc_large(void) {
    /* Large allocation: 1 MiB */
    size_t data_size = 1024 * 1024;
    ray_t* v = ray_alloc(data_size);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_EQ_U(v->mmod, 0);  /* still within buddy range */

    /* Write pattern */
    uint8_t* data = (uint8_t*)ray_data(v);
    memset(data, 0xAB, data_size);
    TEST_ASSERT_EQ_U(data[0], 0xAB);
    TEST_ASSERT_EQ_U(data[data_size - 1], 0xAB);

    ray_free(v);
    PASS();
}

/* ---- Header zeroing ---------------------------------------------------- */

static test_result_t test_header_zeroed(void) {
    ray_t* v = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(v);

    /* Check nullmap region is zeroed */
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQ_U(v->nullmap[i], 0);
    }
    /* type and attrs should be 0 */
    TEST_ASSERT_EQ_I(v->type, 0);
    TEST_ASSERT_EQ_U(v->attrs, 0);
    /* mmod should be 0 (heap) */
    TEST_ASSERT_EQ_U(v->mmod, 0);
    /* rc should be 1 */
    TEST_ASSERT_EQ_U(v->rc, 1);

    ray_free(v);
    PASS();
}

/* ---- Slab cache -------------------------------------------------------- */

static test_result_t test_small_block_reuse(void) {
    /* Allocate and free many small blocks; verify they can be re-allocated */
    ray_t* blocks[128];
    for (int i = 0; i < 128; i++) {
        blocks[i] = ray_alloc(0);
        TEST_ASSERT_NOT_NULL(blocks[i]);
    }
    for (int i = 0; i < 128; i++) {
        ray_free(blocks[i]);
    }
    /* Re-allocate after freeing -- buddy should reuse freed blocks */
    for (int i = 0; i < 128; i++) {
        blocks[i] = ray_alloc(0);
        TEST_ASSERT_NOT_NULL(blocks[i]);
    }

    ray_mem_stats_t stats;
    ray_mem_stats(&stats);
    TEST_ASSERT((stats.alloc_count) >= (256), "stats.alloc_count >= 256");

    for (int i = 0; i < 128; i++) {
        ray_free(blocks[i]);
    }
    PASS();
}

/* ---- Pool growth -------------------------------------------------------- */

static test_result_t test_pool_growth(void) {
    /* Allocate blocks until we exhaust the first 32 MiB pool.
     * Each block is 2^15 = 32 KiB, so ~1024 blocks to fill 32 MiB. */
    size_t block_data_size = (1 << 15) - 32;  /* order 15 (32B ray_t header) */
    int count = 0;
    ray_t* blocks[4096];
    for (int i = 0; i < 4096; i++) {
        blocks[i] = ray_alloc(block_data_size);
        if (!blocks[i]) break;
        count++;
    }
    /* Should have allocated many blocks (some from first pool, some from second) */
    TEST_ASSERT((count) > (1000), "count > 1000");

    for (int i = 0; i < count; i++) {
        ray_free(blocks[i]);
    }
    PASS();
}

/* ---- Stats tracking ---------------------------------------------------- */

static test_result_t test_mem_stats(void) {
    ray_mem_stats_t stats0;
    ray_mem_stats(&stats0);
    size_t base_alloc = stats0.alloc_count;
    size_t base_free = stats0.free_count;

    ray_t* a = ray_alloc(64);
    ray_t* b = ray_alloc(64);
    ray_t* c = ray_alloc(64);

    ray_mem_stats_t stats1;
    ray_mem_stats(&stats1);
    TEST_ASSERT_EQ_U(stats1.alloc_count, base_alloc + 3);
    TEST_ASSERT((stats1.bytes_allocated) > (0), "stats1.bytes_allocated > 0");

    ray_free(a);
    ray_free(b);

    ray_mem_stats_t stats2;
    ray_mem_stats(&stats2);
    TEST_ASSERT_EQ_U(stats2.free_count, base_free + 2);
    TEST_ASSERT_EQ_U(stats2.alloc_count, base_alloc + 3);

    ray_free(c);
    PASS();
}

/* ---- Coalescing -------------------------------------------------------- */

static test_result_t test_coalescing(void) {
    /* Allocate two blocks of order 10 (1024 bytes each), then free them.
     * After freeing both, they should coalesce into a larger block.
     * Verify by allocating one block of order 11 (2048 bytes). */
    size_t data_size = (1 << 10) - 32;  /* order 10 (32B ray_t header) */
    ray_t* a = ray_alloc(data_size);
    ray_t* b = ray_alloc(data_size);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    ray_free(a);
    ray_free(b);

    /* Now allocate a block that requires order 11 */
    size_t bigger = (1 << 11) - 32;
    ray_t* c = ray_alloc(bigger);
    TEST_ASSERT_NOT_NULL(c);

    ray_free(c);
    PASS();
}

/* ---- ray_alloc_copy ----------------------------------------------------- */

static test_result_t test_alloc_copy(void) {
    /* Create a block with some data */
    size_t data_size = 100 * sizeof(int64_t);
    ray_t* orig = ray_alloc(data_size);
    TEST_ASSERT_NOT_NULL(orig);

    orig->type = RAY_I64;
    orig->len = 100;
    int64_t* data = (int64_t*)ray_data(orig);
    for (int i = 0; i < 100; i++) {
        data[i] = (int64_t)(i * 7 + 13);
    }

    /* Copy */
    ray_t* copy = ray_alloc_copy(orig);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_TRUE((void*)copy != (void*)orig);

    /* Verify copy has same content */
    TEST_ASSERT_EQ_I(copy->type, RAY_I64);
    TEST_ASSERT_EQ_I(copy->len, 100);
    int64_t* copy_data = (int64_t*)ray_data(copy);
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQ_I(copy_data[i], (int64_t)(i * 7 + 13));
    }

    /* Copy should have rc=1, independent of original */
    TEST_ASSERT_EQ_U(copy->rc, 1);

    ray_free(orig);
    ray_free(copy);
    PASS();
}

/* ---- Multiple alloc/free cycles ---------------------------------------- */

static test_result_t test_alloc_free_cycles(void) {
    /* Repeated alloc/free should not leak or crash */
    for (int round = 0; round < 10; round++) {
        ray_t* blocks[64];
        for (int i = 0; i < 64; i++) {
            blocks[i] = ray_alloc((size_t)(i * 8));
            TEST_ASSERT_NOT_NULL(blocks[i]);
        }
        for (int i = 63; i >= 0; i--) {
            ray_free(blocks[i]);
        }
    }
    PASS();
}

/* ---- Various sizes ----------------------------------------------------- */

static test_result_t test_various_sizes(void) {
    /* Test a range of allocation sizes */
    size_t sizes[] = { 0, 1, 7, 8, 16, 31, 32, 33, 64, 100, 255, 256,
                       512, 1000, 1024, 4096, 8192, 65536, 1048576 };
    int n = (int)(sizeof(sizes) / sizeof(sizes[0]));

    ray_t* blocks[20];
    for (int i = 0; i < n; i++) {
        blocks[i] = ray_alloc(sizes[i]);
        TEST_ASSERT_NOT_NULL(blocks[i]);
        TEST_ASSERT_FALSE(RAY_IS_ERR(blocks[i]));
        /* Write some data */
        if (sizes[i] > 0) {
            memset(ray_data(blocks[i]), 0xFF, sizes[i]);
        }
    }
    for (int i = 0; i < n; i++) {
        ray_free(blocks[i]);
    }
    PASS();
}

/* ---- Order computation ------------------------------------------------- */

static test_result_t test_order_for_size(void) {
    /* 0 data bytes -> need 32 bytes total (32B ray_t header) -> order 6 (2^6=64) */
    TEST_ASSERT_EQ_U(ray_order_for_size(0), 6);

    /* 1 data byte -> 33 bytes -> order 6 (2^6=64) */
    TEST_ASSERT_EQ_U(ray_order_for_size(1), 6);

    /* 32 data bytes -> 64 bytes -> order 6 (exact fit) */
    TEST_ASSERT_EQ_U(ray_order_for_size(32), 6);

    /* 33 data bytes -> 65 bytes -> order 7 (2^7=128) */
    TEST_ASSERT_EQ_U(ray_order_for_size(33), 7);

    /* 800 data bytes (100 i64s) -> 832 bytes -> order 10 (2^10=1024) */
    TEST_ASSERT_EQ_U(ray_order_for_size(800), 10);

    PASS();
}

/* ---- Pool alignment ---------------------------------------------------- */

static test_result_t test_pool_alignment(void) {
    /* Allocate a block and verify self-aligned pool derivation */
    ray_t* v = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(v);

    /* Block must be inside a self-aligned pool */
    uintptr_t addr = (uintptr_t)v;
    size_t pool_size = (size_t)1 << 25;  /* 32 MB standard pool */
    uintptr_t pool_base = addr & ~(pool_size - 1);

    /* Pool base must be self-aligned */
    TEST_ASSERT_EQ_U(pool_base % pool_size, 0);
    /* Block must be within pool */
    TEST_ASSERT_TRUE(addr >= pool_base && addr < pool_base + pool_size);

    ray_free(v);
    PASS();
}

/* ---- Heap ID derivation ------------------------------------------------ */

static test_result_t test_heap_id_derivation(void) {
    /* Allocate blocks and verify pool header heap_id matches current heap */
    ray_t* v1 = ray_alloc(0);
    ray_t* v2 = ray_alloc(1024);
    ray_t* v3 = ray_alloc(65536);
    TEST_ASSERT_NOT_NULL(v1);
    TEST_ASSERT_NOT_NULL(v2);
    TEST_ASSERT_NOT_NULL(v3);

    ray_heap_t* h = ray_tl_heap;

    /* Pool header heap_id should match current heap for all blocks */
    ray_pool_hdr_t* phdr1 = ray_pool_of(v1);
    ray_pool_hdr_t* phdr2 = ray_pool_of(v2);
    ray_pool_hdr_t* phdr3 = ray_pool_of(v3);
    TEST_ASSERT_EQ_U(phdr1->heap_id, h->id);
    TEST_ASSERT_EQ_U(phdr2->heap_id, h->id);
    TEST_ASSERT_EQ_U(phdr3->heap_id, h->id);

    ray_free(v1);
    ray_free(v2);
    ray_free(v3);
    PASS();
}

/* ---- Cross-heap free --------------------------------------------------- */

static test_result_t test_cross_heap_free(void) {
    /* heap_a is the current heap (from buddy_setup) */
    ray_heap_t* heap_a = ray_tl_heap;
    uint16_t heap_a_id = heap_a->id;

    /* Create heap_b by switching thread-local pointer */
    ray_tl_heap = NULL;
    ray_heap_init();
    ray_heap_t* heap_b = ray_tl_heap;
    TEST_ASSERT_NOT_NULL(heap_b);
    TEST_ASSERT((heap_b->id) != (heap_a_id), "heap_b->id != heap_a_id");

    /* Allocate blocks on heap_b */
    ray_t* blk1 = ray_alloc(0);
    ray_t* blk2 = ray_alloc(128);
    ray_t* blk3 = ray_alloc(4096);
    TEST_ASSERT_NOT_NULL(blk1);
    TEST_ASSERT_NOT_NULL(blk2);
    TEST_ASSERT_NOT_NULL(blk3);

    /* Pool headers should carry heap_b's ID */
    TEST_ASSERT_EQ_U(ray_pool_of(blk1)->heap_id, heap_b->id);
    TEST_ASSERT_EQ_U(ray_pool_of(blk2)->heap_id, heap_b->id);

    /* Switch to heap_a and free blocks from the wrong heap */
    ray_tl_heap = heap_a;
    ray_free(blk1);  /* should go to heap_a->foreign */
    ray_free(blk2);
    ray_free(blk3);

    /* Flush foreign blocks back to their owning heap */
    ray_heap_flush_foreign();

    /* Cleanup: destroy heap_b */
    ray_tl_heap = heap_b;
    ray_heap_destroy();

    /* Restore heap_a for teardown */
    ray_tl_heap = heap_a;
    PASS();
}

/* ---- Pending merge ------------------------------------------------------ */

static test_result_t test_heap_pending_merge(void) {
    /* heap_a is the current heap (from buddy_setup) */
    ray_heap_t* heap_a = ray_tl_heap;
    uint32_t pools_before = heap_a->pool_count;

    /* Create heap_b */
    ray_tl_heap = NULL;
    ray_heap_init();
    ray_heap_t* heap_b = ray_tl_heap;
    TEST_ASSERT_NOT_NULL(heap_b);

    /* Allocate on heap_b to force pool creation */
    ray_t* blk = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(blk);
    uint32_t heap_b_pools = heap_b->pool_count;
    TEST_ASSERT((heap_b_pools) > (0), "heap_b_pools > 0");

    /* Push heap_b onto pending queue (simulating worker teardown) */
    ray_tl_heap = heap_a;
    ray_heap_push_pending(heap_b);

    /* Drain pending — merges heap_b into heap_a, destroys heap_b */
    ray_heap_drain_pending();

    /* heap_a should have gained heap_b's pools */
    TEST_ASSERT_EQ_U(heap_a->pool_count, pools_before + heap_b_pools);

    /* The block should still be accessible (pool transferred) */
    TEST_ASSERT_EQ_U(blk->mmod, 0);

    /* Free the block — goes via foreign path (heap_id mismatch)
     * then flush reclaims it locally */
    ray_free(blk);
    ray_heap_flush_foreign();

    PASS();
}

/* ---- Swap path resolution -------------------------------------------------
 *
 * These two tests verify the wiring for the file-backed-mmap fallback's
 * swap directory.  RAY_HEAP_SWAP overrides the default ("./"), and either
 * way the resolved path always carries a trailing slash so heap_add_pool
 * can concatenate `<swap_path><filename>` directly.  The end-to-end
 * behaviour (anon mmap fails -> tempfile created -> pool usable -> file
 * unlinked on heap_destroy) is verified manually via REPL — it requires
 * an environment where anon mmap of pool-size actually fails. */
static test_result_t test_swap_path_env(void) {
    /* buddy_setup ran ray_heap_init.  Tear it down, set the env, re-init,
     * and check the env value flowed through. */
    ray_heap_destroy();
    setenv("RAY_HEAP_SWAP", "/tmp/rayforce_swap_test", 1);
    ray_heap_init();
    extern RAY_TLS ray_heap_t* ray_tl_heap;
    TEST_ASSERT_NOT_NULL(ray_tl_heap);
    /* Path must end with a trailing slash regardless of the env value. */
    size_t n = strlen(ray_tl_heap->swap_path);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQ_I(ray_tl_heap->swap_path[n - 1], '/');
    /* Env value should be the prefix. */
    TEST_ASSERT_EQ_I(strncmp(ray_tl_heap->swap_path,
                             "/tmp/rayforce_swap_test/", 24), 0);
    unsetenv("RAY_HEAP_SWAP");
    PASS();
}

static test_result_t test_swap_path_default(void) {
    /* Manage runtime explicitly: with no env set, the default "./" must
     * apply.  This test owns its own heap lifecycle (entries registered
     * with NULL setup/teardown). */
    unsetenv("RAY_HEAP_SWAP");
    ray_heap_init();
    extern RAY_TLS ray_heap_t* ray_tl_heap;
    TEST_ASSERT_NOT_NULL(ray_tl_heap);
    TEST_ASSERT_EQ_I(strcmp(ray_tl_heap->swap_path, "./"), 0);
    ray_heap_destroy();
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t buddy_entries[] = {
    { "buddy/alloc_basic", test_alloc_basic, buddy_setup, buddy_teardown },
    { "buddy/alloc_small_atom", test_alloc_small_atom, buddy_setup, buddy_teardown },
    { "buddy/alloc_medium_vec", test_alloc_medium_vector, buddy_setup, buddy_teardown },
    { "buddy/alloc_large", test_alloc_large, buddy_setup, buddy_teardown },
    { "buddy/header_zeroed", test_header_zeroed, buddy_setup, buddy_teardown },
    { "buddy/small_reuse", test_small_block_reuse, buddy_setup, buddy_teardown },
    { "buddy/pool_growth", test_pool_growth, buddy_setup, buddy_teardown },
    { "buddy/mem_stats", test_mem_stats, buddy_setup, buddy_teardown },
    { "buddy/coalescing", test_coalescing, buddy_setup, buddy_teardown },
    { "buddy/alloc_copy", test_alloc_copy, buddy_setup, buddy_teardown },
    { "buddy/alloc_free_cycles", test_alloc_free_cycles, buddy_setup, buddy_teardown },
    { "buddy/various_sizes", test_various_sizes, buddy_setup, buddy_teardown },
    { "buddy/order_for_size", test_order_for_size, buddy_setup, buddy_teardown },
    { "buddy/pool_alignment", test_pool_alignment, buddy_setup, buddy_teardown },
    { "buddy/heap_id_derivation", test_heap_id_derivation, buddy_setup, buddy_teardown },
    { "buddy/cross_heap_free", test_cross_heap_free, buddy_setup, buddy_teardown },
    { "buddy/pending_merge", test_heap_pending_merge, buddy_setup, buddy_teardown },
    { "buddy/swap_path_env", test_swap_path_env, buddy_setup, buddy_teardown },
    { "buddy/swap_path_default", test_swap_path_default, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


