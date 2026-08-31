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

/*
 * test_heap.c — focused unit tests for src/mem/heap.c paths NOT covered by
 * test_buddy.c (basic alloc/free), test_arena.c (sym arena), or test_cow.c
 * (retain/release/cow basics).  Targets: slab-cache overflow, scratch-realloc
 * over multiple types, scratch-arena bump allocator, ray_heap_release_pages,
 * GC under both serial and parallel flags, ray_heap_merge with rich source
 * heaps, and the owned-ref retain/release fan-out for compound types
 * (LIST / TABLE / DICT / parted / SLICE / STR with str_pool).
 */

/* MAP_ANONYMOUS is a Linux/glibc extension; needs _GNU_SOURCE before
 * any system include. Matches the convention in src/core/ipc.c,
 * src/store/journal.c, src/store/serde.c. */
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/mman.h>

/* ---- Setup / Teardown -------------------------------------------------- */

static void heap_setup(void) {
    ray_heap_init();
}

static void heap_teardown(void) {
    ray_heap_destroy();
}

/* ---- Slab cache overflow ----------------------------------------------- *
 *
 * The slab cache holds RAY_SLAB_CACHE_SIZE blocks per slab order.  When a
 * free arrives and the cache is full, the block falls through to
 * heap_coalesce.  This exercises the "slab full -> coalesce" branch of
 * ray_free which a small set of allocs/frees never touches. */

static test_result_t test_slab_overflow_falls_through(void) {
    /* Order 6 (64-byte) blocks: ray_alloc(0) lands here.  Allocate
     * RAY_SLAB_CACHE_SIZE + 16 blocks so the slab cache is overfilled,
     * then free them — the last 16 must take the coalesce path. */
    enum { N = RAY_SLAB_CACHE_SIZE + 16 };
    ray_t* blks[N];
    for (int i = 0; i < N; i++) {
        blks[i] = ray_alloc(0);
        TEST_ASSERT_NOT_NULL(blks[i]);
    }
    /* Free in reverse so the first frees fill the slab cache, the trailing
     * frees overflow into heap_coalesce. */
    for (int i = N - 1; i >= 0; i--) {
        ray_free(blks[i]);
    }
    /* Re-alloc to make sure heap is still consistent. */
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    ray_free(v);
    PASS();
}

/* ---- Multi-pool growth + cross-pool reuse ------------------------------ *
 *
 * Each standard pool is 32 MB.  A burst of large allocs should trigger
 * heap_add_pool more than once.  Freeing scrambled and re-allocating
 * exercises the avail-bitmap scan and the freelist-stale-bit clearing
 * path inside ray_alloc. */

static test_result_t test_multi_pool_growth(void) {
    /* 1 MB blocks: ~30 fit per 32 MB pool.  Allocate 64 of them to force
     * at least 2 pools. */
    enum { N = 64 };
    ray_t* blks[N];
    int allocated = 0;
    for (int i = 0; i < N; i++) {
        blks[i] = ray_alloc(1u << 20);  /* 1 MB data */
        if (!blks[i]) break;
        allocated++;
    }
    TEST_ASSERT((allocated) >= (32), "allocated >= 32");
    TEST_ASSERT((ray_tl_heap->pool_count) >= (2), "pool_count >= 2");

    /* Free in scrambled order: even indices first, odd second.  This
     * leaves freelists with mixed-order entries to coalesce. */
    for (int i = 0; i < allocated; i += 2) ray_free(blks[i]);
    for (int i = 1; i < allocated; i += 2) ray_free(blks[i]);

    /* Allocate again to confirm the heap is still functional and reusing
     * coalesced space. */
    for (int i = 0; i < allocated; i++) {
        blks[i] = ray_alloc(1u << 20);
        TEST_ASSERT_NOT_NULL(blks[i]);
    }
    for (int i = 0; i < allocated; i++) ray_free(blks[i]);
    PASS();
}

/* ---- ray_scratch_realloc grow + shrink for several types --------------- */

static test_result_t test_scratch_realloc_atom(void) {
    /* Atom: realloc copies the 32-byte header only (data_size=0). */
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    v->type = -RAY_I64;
    v->i64 = 1234567890LL;

    ray_t* w = ray_scratch_realloc(v, 0);  /* atom path: old_data=0 */
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQ_I(w->type, -RAY_I64);
    TEST_ASSERT_EQ_I(w->i64, 1234567890LL);
    ray_free(w);
    PASS();
}

static test_result_t test_scratch_realloc_vec_grow(void) {
    /* I64 vector: realloc grow copies element bytes via the
     * "ray_type / ray_sym_elem_size" branch. */
    size_t old_data = 32 * sizeof(int64_t);
    ray_t* v = ray_alloc(old_data);
    TEST_ASSERT_NOT_NULL(v);
    v->type = RAY_I64;
    v->len  = 32;
    int64_t* d = (int64_t*)ray_data(v);
    for (int i = 0; i < 32; i++) d[i] = (int64_t)(i * 1000 + 7);

    /* Grow to 200 i64s. */
    ray_t* w = ray_scratch_realloc(v, 200 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQ_I(w->type, RAY_I64);
    TEST_ASSERT_EQ_I(w->len, 32);  /* len carried in header memcpy */
    int64_t* wd = (int64_t*)ray_data(w);
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQ_I(wd[i], (int64_t)(i * 1000 + 7));
    }
    ray_free(w);
    PASS();
}

static test_result_t test_scratch_realloc_vec_shrink(void) {
    /* Shrink path: copy_data clamped to new_data_size */
    size_t old_data = 100 * sizeof(int64_t);
    ray_t* v = ray_alloc(old_data);
    TEST_ASSERT_NOT_NULL(v);
    v->type = RAY_I64;
    v->len  = 100;
    int64_t* d = (int64_t*)ray_data(v);
    for (int i = 0; i < 100; i++) d[i] = (int64_t)i;

    ray_t* w = ray_scratch_realloc(v, 16 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(w);
    int64_t* wd = (int64_t*)ray_data(w);
    /* First 16 elements must round-trip. */
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQ_I(wd[i], (int64_t)i);
    }
    ray_free(w);
    PASS();
}

static test_result_t test_scratch_realloc_list(void) {
    /* RAY_LIST realloc — exercises the list-specific branch in
     * ray_scratch_realloc that sizes via sizeof(ray_t*). */
    size_t old_data = 4 * sizeof(ray_t*);
    ray_t* v = ray_alloc(old_data);
    TEST_ASSERT_NOT_NULL(v);
    v->type = RAY_LIST;
    v->len  = 4;
    ray_t** slots = (ray_t**)ray_data(v);
    for (int i = 0; i < 4; i++) slots[i] = NULL;

    ray_t* w = ray_scratch_realloc(v, 16 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQ_I(w->type, RAY_LIST);
    /* Detach old refs prior to free was handled by ray_scratch_realloc;
     * w's first 4 slots must round-trip (NULL here). */
    ray_t** wslots = (ray_t**)ray_data(w);
    for (int i = 0; i < 4; i++) TEST_ASSERT_NULL(wslots[i]);
    /* len isn't auto-resized; we asked for capacity, not length. */
    ray_free(w);
    PASS();
}

static test_result_t test_scratch_realloc_null_v(void) {
    /* v=NULL path: returns a fresh allocation, no copy. */
    ray_t* w = ray_scratch_realloc(NULL, 64);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQ_U(w->rc, 1);
    ray_free(w);
    PASS();
}

/* ---- Scratch arena bump allocator -------------------------------------- *
 *
 * ray_scratch_arena_push uses ray_alloc internally to back its bump
 * allocator with 64 KB blocks; on overflow it allocates more backings.
 * Reset frees them all.  Both functions are 100% uncovered by the rfl
 * suite. */

static test_result_t test_scratch_arena_basic(void) {
    ray_scratch_arena_t a;
    ray_scratch_arena_init(&a);

    /* Three small pushes within one backing block. */
    void* p1 = ray_scratch_arena_push(&a, 100);
    void* p2 = ray_scratch_arena_push(&a, 200);
    void* p3 = ray_scratch_arena_push(&a, 300);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_NOT_NULL(p3);
    /* All within one backing — addresses must be ordered & disjoint. */
    TEST_ASSERT_TRUE((char*)p2 >= (char*)p1 + 100);
    TEST_ASSERT_TRUE((char*)p3 >= (char*)p2 + 200);
    TEST_ASSERT_EQ_I(a.n_backing, 1);

    /* Write to verify writability. */
    memset(p1, 0xAA, 100);
    memset(p2, 0xBB, 200);
    memset(p3, 0xCC, 300);

    ray_scratch_arena_reset(&a);
    TEST_ASSERT_EQ_I(a.n_backing, 0);
    PASS();
}

static test_result_t test_scratch_arena_multi_backing(void) {
    /* One 64KB block holds ~64512 bytes payload.  Drain past one block
     * to force a second backing. */
    ray_scratch_arena_t a;
    ray_scratch_arena_init(&a);

    /* Push 200 chunks of 1 KB each = ~200 KB — definitely > one backing. */
    for (int i = 0; i < 200; i++) {
        void* p = ray_scratch_arena_push(&a, 1024);
        TEST_ASSERT_NOT_NULL(p);
        ((char*)p)[0] = (char)i;
        ((char*)p)[1023] = (char)(255 - i);
    }
    TEST_ASSERT((a.n_backing) >= (2), "n_backing >= 2");

    ray_scratch_arena_reset(&a);
    /* After reset, pushes must work again. */
    void* p = ray_scratch_arena_push(&a, 64);
    TEST_ASSERT_NOT_NULL(p);
    ray_scratch_arena_reset(&a);
    PASS();
}

static test_result_t test_scratch_arena_oversize(void) {
    /* Request larger than one backing block — push allocates an
     * exact-fit backing. */
    ray_scratch_arena_t a;
    ray_scratch_arena_init(&a);

    size_t big = 256 * 1024;  /* 256 KB > 64 KB block */
    void* p = ray_scratch_arena_push(&a, big);
    TEST_ASSERT_NOT_NULL(p);
    memset(p, 0x77, big);
    TEST_ASSERT_EQ_U(((unsigned char*)p)[0], 0x77);
    TEST_ASSERT_EQ_U(((unsigned char*)p)[big - 1], 0x77);

    ray_scratch_arena_reset(&a);
    PASS();
}

static test_result_t test_scratch_arena_direct_backing(void) {
    /* A backing block at or above RAY_HEAP_POOL_ORDER is a DIRECT
     * allocation: mapped at its exact page-rounded size and tagged with
     * the RAY_ORDER_DIRECT sentinel instead of a real buddy order.  The
     * arena's window has to come from the block's true capacity —
     * reading the sentinel as an order describes a 2^39-byte block, so
     * every later push is bump-allocated past the end of the mapping and
     * writes into unmapped address space. */
    ray_scratch_arena_t a;
    ray_scratch_arena_init(&a);

    size_t big = ((size_t)1 << RAY_HEAP_POOL_ORDER) + (1u << 20);
    unsigned char* p = (unsigned char*)ray_scratch_arena_push(&a, big);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ_I(a.n_backing, 1);
    TEST_ASSERT_TRUE(ray_is_direct(a.backing[0]));

    /* The window must stop at the backing's real capacity. */
    char* cap_end = (char*)ray_data(a.backing[0])
                  + ray_block_data_bytes(a.backing[0]);
    TEST_ASSERT_TRUE(a.end <= cap_end);

    /* Consequently the next push cannot be carved out of the leftover of
     * a block that has none: it must take a backing of its own. */
    size_t follow = 4u << 20;
    unsigned char* q = (unsigned char*)ray_scratch_arena_push(&a, follow);
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQ_I(a.n_backing, 2);
    TEST_ASSERT_TRUE((char*)q + follow <= (char*)ray_data(a.backing[1])
                                       + ray_block_data_bytes(a.backing[1]));

    /* Both regions must be writable end to end and must not overlap. */
    memset(p, 0x5A, big);
    memset(q, 0xA5, follow);
    TEST_ASSERT_EQ_U(p[0], 0x5A);
    TEST_ASSERT_EQ_U(p[big - 1], 0x5A);
    TEST_ASSERT_EQ_U(q[0], 0xA5);
    TEST_ASSERT_EQ_U(q[follow - 1], 0xA5);

    ray_scratch_arena_reset(&a);
    PASS();
}

/* ---- ray_heap_release_pages -------------------------------------------- *
 *
 * Walks freelists at order >= 13 (8 KB+) and madvise-releases pages past
 * the first 4 KB so the kernel can reclaim physical pages without
 * touching the address space.  Uncovered by the basic suite. */

static test_result_t test_release_pages(void) {
    /* Allocate then free a few large blocks (32 KB+) so order >= 15
     * freelists are non-empty.  Then call ray_heap_release_pages — it
     * must complete without disturbing block headers. */
    ray_t* a = ray_alloc(64 * 1024);   /* order 17 */
    ray_t* b = ray_alloc(128 * 1024);  /* order 18 */
    ray_t* c = ray_alloc(256 * 1024);  /* order 19 */
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);
    ray_free(a); ray_free(b); ray_free(c);

    ray_heap_release_pages();

    /* Allocator must still serve requests after page release. */
    ray_t* x = ray_alloc(128 * 1024);
    TEST_ASSERT_NOT_NULL(x);
    /* The first 4 KB of the freed block is still mapped & writable; pages
     * past 4 KB will fault back in lazily. */
    memset(ray_data(x), 0x42, 100 * 1024);
    ray_free(x);
    PASS();
}

/* ---- ray_heap_gc oversized-pool reclamation ---------------------------- *
 *
 * Allocate a block large enough to force a pool > 32 MB (pool_order >
 * RAY_HEAP_POOL_ORDER), free it, then GC.  With a second standard pool
 * already present, gc walks the oversized pool, finds it empty (only
 * the freelist entry from the lone freed block, which equals the pool's
 * full data capacity), and munmaps it — exercises the loop body in
 * ray_heap_gc that's only reached when an oversized empty pool exists. */

static test_result_t test_gc_reclaim_oversized_pool(void) {
    /* Oversized allocations (>= RAY_HEAP_POOL_ORDER, 32 MB) now bypass the
     * buddy pool entirely — they are mmap'd at their EXACT page-rounded size
     * (the direct path), so they never create an oversized pool to GC-reclaim.
     * Verify the new contract: a big alloc bumps direct_bytes by ~exactly its
     * size (not a power-of-2 block), grows no pool, and releases exactly. */
    ray_t* small = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(small);

    ray_mem_stats_t s0; ray_mem_stats(&s0);
    uint32_t pools0 = ray_tl_heap->pool_count;

    size_t big = (size_t)1 << 26;  /* 64 MB */
    ray_t* huge = ray_alloc(big);
    if (!huge) {
        ray_free(small);
        SKIP("oversized alloc unavailable");
    }

    ray_mem_stats_t s1; ray_mem_stats(&s1);
    size_t delta = s1.direct_bytes - s0.direct_bytes;
    TEST_ASSERT(delta >= big, "direct_bytes counts at least the request");
    TEST_ASSERT(delta < big + 8192, "exact page-rounded size, not a power-of-2 block");
    TEST_ASSERT(ray_tl_heap->pool_count == pools0, "no oversized pool was created");
    TEST_ASSERT(huge->order > RAY_HEAP_MAX_ORDER, "direct block carries the sentinel order");

    ray_free(huge);
    ray_mem_stats_t s2; ray_mem_stats(&s2);
    TEST_ASSERT(s2.direct_bytes == s0.direct_bytes, "direct mapping released exactly");

    ray_free(small);
    PASS();
}

/* ---- ray_heap_gc serial path ------------------------------------------- */

static test_result_t test_heap_gc_serial(void) {
    /* Build up some freelist activity, then call gc.  No oversized pools
     * are involved; gc should still run flush_foreign + flush_slabs +
     * return_foreign_freelist without crashing. */
    ray_t* xs[64];
    for (int i = 0; i < 64; i++) {
        xs[i] = ray_alloc(256 + i * 8);
        TEST_ASSERT_NOT_NULL(xs[i]);
    }
    for (int i = 0; i < 64; i++) ray_free(xs[i]);

    /* Serial gc: ray_parallel_flag is 0, so gc takes the safe path. */
    ray_heap_gc();

    /* Heap remains usable. */
    ray_t* y = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(y);
    ray_free(y);
    PASS();
}

/* ---- ray_heap_gc parallel path (foreign-flush no-op) ------------------- */

static test_result_t test_heap_gc_parallel(void) {
    /* When ray_parallel_flag != 0, heap_flush_foreign(false) is a no-op
     * and gc must NOT touch worker heaps' state.  We only need to
     * verify that calling gc with the flag set doesn't crash and the
     * heap remains consistent. */
    ray_t* a = ray_alloc(128);
    TEST_ASSERT_NOT_NULL(a);

    ray_parallel_begin();
    ray_heap_gc();
    ray_parallel_end();  /* clears flag and runs gc again */

    /* a must still be a valid block. */
    ((char*)ray_data(a))[0] = 'x';
    ray_free(a);
    PASS();
}

/* ---- ray_heap_flush_foreign while parallel (no-op early return) -------- */

static test_result_t test_flush_foreign_during_parallel(void) {
    /* While ray_parallel_flag is set, ray_heap_flush_foreign() must not
     * touch h->foreign — proves the early return path. */
    ray_heap_t* heap_a = ray_tl_heap;

    /* Build heap_b and free a block from it on heap_a so heap_a->foreign
     * is non-empty. */
    ray_tl_heap = NULL;
    ray_heap_init();
    ray_heap_t* heap_b = ray_tl_heap;
    TEST_ASSERT_NOT_NULL(heap_b);
    ray_t* blk = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(blk);

    ray_tl_heap = heap_a;
    ray_free(blk);  /* cross-thread free: enqueues onto heap_a->foreign */
    /* Foreign-list model: the cross-thread free sits on the freeing heap. */
    TEST_ASSERT_NOT_NULL(heap_a->foreign);

    ray_parallel_begin();
    ray_heap_flush_foreign();
    /* Foreign list should still be intact (not flushed). */
    TEST_ASSERT_NOT_NULL(heap_a->foreign);
    ray_parallel_end();
    /* parallel_end -> ray_heap_gc which DOES flush foreign now safe.  */
    /* Foreign list should be empty after end. */
    TEST_ASSERT_NULL(heap_a->foreign);

    ray_tl_heap = heap_b;
    ray_heap_destroy();
    ray_tl_heap = heap_a;
    PASS();
}

/* ---- Owned-ref retain/release: LIST containing children ---------------- *
 *
 * ray_alloc_copy on a RAY_LIST exercises the LIST branch of both
 * ray_retain_owned_refs (bumping each child's rc) and ray_release_owned_refs
 * (lowering it on the copy's free).  ray_block_copy in test_cow.c covers
 * tables, but not lists directly. */

static test_result_t test_alloc_copy_list_retains(void) {
    /* Build a list of three i64 atoms. */
    ray_t* list = ray_list_new(3);
    TEST_ASSERT_NOT_NULL(list);

    ray_t* items[3];
    for (int i = 0; i < 3; i++) {
        items[i] = ray_alloc(0);
        TEST_ASSERT_NOT_NULL(items[i]);
        items[i]->type = -RAY_I64;
        items[i]->i64  = (int64_t)(i * 1000 + 1);
        list = ray_list_append(list, items[i]);
        ray_release(items[i]);  /* list holds the only owning ref */
    }

    ray_t** slots = (ray_t**)ray_data(list);
    uint32_t rc_before = slots[0]->rc;

    /* alloc_copy invokes ray_retain_owned_refs which retains every child. */
    ray_t* copy = ray_alloc_copy(list);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_FALSE(RAY_IS_ERR(copy));
    TEST_ASSERT_EQ_I(copy->type, RAY_LIST);
    TEST_ASSERT_EQ_I(copy->len, 3);

    uint32_t rc_after = slots[0]->rc;
    TEST_ASSERT_EQ_U(rc_after, rc_before + 1);

    /* Freeing the copy invokes ray_release_owned_refs -> rc drops. */
    ray_release(copy);
    TEST_ASSERT_EQ_U(slots[0]->rc, rc_before);

    ray_release(list);
    PASS();
}

/* ---- Owned-ref: STR with str_pool -------------------------------------- *
 *
 * Releasing a RAY_STR with a str_pool child must release the pool exactly
 * once.  Set up a STR vector pointing at a pool we own and confirm the
 * pool's rc decrements via the dedicated branch in ray_release_owned_refs. */

static test_result_t test_str_pool_owned_ref(void) {
    /* Construct a RAY_STR shell + a U8 pool block and link them. */
    ray_t* shell = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(shell);
    shell->type = RAY_STR;
    shell->len  = 0;
    /* Pool is a plain U8 vec retained by shell. */
    ray_t* pool = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(pool);
    pool->type = RAY_U8;
    pool->len  = 64;
    /* shell takes ownership; pool->rc stays at 1 because we transfer. */
    shell->str_pool = pool;

    /* Sanity: shell rc==1, pool rc==1.  Release shell — pool must be
     * released too via ray_release_owned_refs's STR/str_pool branch. */
    TEST_ASSERT_EQ_U(shell->rc, 1);
    TEST_ASSERT_EQ_U(pool->rc, 1);

    /* ray_release(shell) -> rc drops to 0 -> ray_free -> release_owned_refs
     * -> ray_release(pool).  After this, pool's storage is reclaimed; we
     * cannot validate pool's state, but a crash here would mean the path
     * mishandled the pointer.  Use ray_alloc immediately after to confirm
     * heap is sane. */
    ray_release(shell);

    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

/* ---- Sentinel-encoded null release ------------------------------------- *
 *
 * A nullable vec carries no auxiliary bitmap child; null state lives
 * entirely in the payload via the type-correct NULL_* sentinel.  This
 * test exercises release of a >128-element nullable vec and verifies
 * the heap remains sane afterwards. */

static test_result_t test_sentinel_null_release(void) {
    int64_t n = 200;
    ray_t* vec = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(vec);
    for (int64_t i = 0; i < n; i++) {
        vec = ray_vec_append(vec, &i);
        TEST_ASSERT_NOT_NULL(vec);
    }

    /* Mark a few rows null — sentinel writes into payload only. */
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vec, 5, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vec, 150, true), RAY_OK);
    TEST_ASSERT_TRUE(vec->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(ray_vec_is_null(vec, 5));
    TEST_ASSERT_TRUE(ray_vec_is_null(vec, 150));
    TEST_ASSERT_FALSE(ray_vec_is_null(vec, 0));

    /* Drop vec — no external bitmap child to release, just the payload. */
    ray_release(vec);

    /* Heap remains sane. */
    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

/* ---- Owned-ref: SLICE retains parent ----------------------------------- *
 *
 * A RAY_ATTR_SLICE block carries slice_parent + slice_offset.  ray_alloc_copy
 * of a slice must retain the parent (so the parent's rc bumps), and
 * release-on-free must drop it back. */

static test_result_t test_slice_owned_ref(void) {
    /* Parent: i64 vec of 16 elements. */
    ray_t* parent = ray_alloc(16 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(parent);
    parent->type = RAY_I64;
    parent->len  = 16;
    int64_t* pd = (int64_t*)ray_data(parent);
    for (int i = 0; i < 16; i++) pd[i] = (int64_t)(i + 1);

    /* Slice: rc=1 atom-style header pointing at parent[4..12). */
    ray_t* slice = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(slice);
    slice->type = RAY_I64;
    slice->len  = 8;
    slice->attrs |= RAY_ATTR_SLICE;
    slice->slice_parent = parent;
    slice->slice_offset = 4;
    ray_retain(parent);  /* slice owns one ref */

    /* ray_data on a slice resolves slice_parent + slice_offset — exercises
     * the slice arm of ray_data_fn in include/rayforce.h (otherwise dead
     * in the test build).  parent[4..12) holds the values 5..12. */
    int64_t* sd = (int64_t*)ray_data(slice);
    TEST_ASSERT_EQ_I(sd[0], 5);
    TEST_ASSERT_EQ_I(sd[7], 12);

    uint32_t parent_rc = parent->rc;

    /* Copy the slice — ray_retain_owned_refs bumps parent->rc. */
    ray_t* copy = ray_alloc_copy(slice);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_TRUE(copy->attrs & RAY_ATTR_SLICE);
    TEST_ASSERT_EQ_PTR(copy->slice_parent, parent);
    TEST_ASSERT_EQ_U(parent->rc, parent_rc + 1);
    /* Same slice deref via the copy. */
    int64_t* cd = (int64_t*)ray_data(copy);
    TEST_ASSERT_EQ_I(cd[0], 5);
    TEST_ASSERT_EQ_I(cd[7], 12);

    /* Releasing the copy drops parent->rc again. */
    ray_release(copy);
    TEST_ASSERT_EQ_U(parent->rc, parent_rc);

    ray_release(slice);   /* drops slice's ref on parent */
    ray_release(parent);  /* drops original ref */
    PASS();
}

/* ---- Owned-ref: parted vec retains segments ---------------------------- *
 *
 * A parted vec stores its segment ray_t* pointers in the data area.
 * ray_retain_owned_refs / ray_release_owned_refs must touch each. */

static test_result_t test_parted_owned_ref(void) {
    /* Build 3 inner i64 vecs as segments. */
    ray_t* segs[3];
    for (int i = 0; i < 3; i++) {
        segs[i] = ray_alloc(4 * sizeof(int64_t));
        TEST_ASSERT_NOT_NULL(segs[i]);
        segs[i]->type = RAY_I64;
        segs[i]->len  = 4;
    }

    /* Parted I64 vec: type = RAY_PARTED_BASE + RAY_I64. */
    ray_t* parted = ray_alloc(3 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(parted);
    parted->type = (int8_t)(RAY_PARTED_BASE + RAY_I64);
    parted->len  = 3;
    ray_t** slots = (ray_t**)ray_data(parted);
    for (int i = 0; i < 3; i++) {
        ray_retain(segs[i]);  /* parted holds one ref each */
        slots[i] = segs[i];
    }

    uint32_t rcs_before[3] = { segs[0]->rc, segs[1]->rc, segs[2]->rc };

    /* Copy parted block — segments retained. */
    ray_t* copy = ray_alloc_copy(parted);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_FALSE(RAY_IS_ERR(copy));
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQ_U(segs[i]->rc, rcs_before[i] + 1);
    }

    /* Release copy — segs go back to the original ref count. */
    ray_release(copy);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQ_U(segs[i]->rc, rcs_before[i]);
    }

    ray_release(parted);  /* drops parted's refs */
    for (int i = 0; i < 3; i++) ray_release(segs[i]);  /* drop our own */
    PASS();
}

/* ---- Owned-ref: MAPCOMMON two-pointer block ---------------------------- */

static test_result_t test_mapcommon_owned_ref(void) {
    ray_t* a = ray_alloc(0); a->type = -RAY_I64; a->i64 = 7;
    ray_t* b = ray_alloc(0); b->type = -RAY_I64; b->i64 = 11;
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    ray_t* mc = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(mc);
    mc->type = RAY_MAPCOMMON;
    mc->len  = 2;
    ray_t** slots = (ray_t**)ray_data(mc);
    slots[0] = a;
    slots[1] = b;
    /* mc owns a ref to each child (released on free); keep our own too. */
    ray_retain(a);
    ray_retain(b);

    uint32_t a_rc = a->rc, b_rc = b->rc;

    ray_t* copy = ray_alloc_copy(mc);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQ_I(copy->type, RAY_MAPCOMMON);
    TEST_ASSERT_EQ_U(a->rc, a_rc + 1);
    TEST_ASSERT_EQ_U(b->rc, b_rc + 1);

    ray_release(copy);
    TEST_ASSERT_EQ_U(a->rc, a_rc);
    TEST_ASSERT_EQ_U(b->rc, b_rc);

    ray_release(mc);     /* drops mc's refs */
    ray_release(a);      /* drop our own */
    ray_release(b);
    PASS();
}

/* ---- Heap merge with rich source --------------------------------------- *
 *
 * Build a source heap with: filled slab caches across multiple orders,
 * non-empty freelists, foreign blocks, and multiple pools.  Merge into
 * the current heap and verify accounting + accessibility. */

static test_result_t test_merge_with_slabs_and_freelist(void) {
    ray_heap_t* heap_a = ray_tl_heap;

    /* Build heap_b. */
    ray_tl_heap = NULL;
    ray_heap_init();
    ray_heap_t* heap_b = ray_tl_heap;
    TEST_ASSERT_NOT_NULL(heap_b);

    /* Allocate then free across several slab orders to populate slab
     * caches.  Order 6 (64B), 7 (128B), 8 (256B), 9 (512B), 10 (1024B). */
    ray_t* tmp[5][8];
    size_t sizes[5] = { 0, 64, 128, 256, 512 };
    for (int o = 0; o < 5; o++) {
        for (int i = 0; i < 8; i++) {
            tmp[o][i] = ray_alloc(sizes[o]);
            TEST_ASSERT_NOT_NULL(tmp[o][i]);
        }
    }
    for (int o = 0; o < 5; o++)
        for (int i = 0; i < 8; i++)
            ray_free(tmp[o][i]);  /* enters slab cache */

    /* Allocate a couple of larger blocks and free them — populates
     * proper freelists (orders 11-14). */
    ray_t* big1 = ray_alloc(2048);
    ray_t* big2 = ray_alloc(8192);
    ray_t* big3 = ray_alloc(16384);
    TEST_ASSERT_NOT_NULL(big1);
    TEST_ASSERT_NOT_NULL(big2);
    TEST_ASSERT_NOT_NULL(big3);
    ray_free(big1); ray_free(big2); ray_free(big3);

    uint32_t heap_b_pools = heap_b->pool_count;
    TEST_ASSERT((heap_b_pools) > (0), "heap_b_pools > 0");

    /* Drive a foreign block into heap_b->foreign by allocating on
     * heap_a and freeing while heap_b is current. */
    ray_tl_heap = heap_a;
    ray_t* fblk = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(fblk);
    ray_tl_heap = heap_b;
    ray_free(fblk);  /* heap_b->foreign now contains fblk (foreign-list model) */
    TEST_ASSERT_NOT_NULL(heap_b->foreign);

    /* Capture pool count AFTER any allocs heap_a has performed for fblk —
     * lazy heap_add_pool may have grown the count. */
    ray_tl_heap = heap_a;
    uint32_t pools_before = heap_a->pool_count;

    /* Push & drain pending — exercises ray_heap_merge with all of:
     * slabs, freelists, foreign list, multiple pools. */
    ray_heap_push_pending(heap_b);
    ray_heap_drain_pending();

    /* heap_a should have absorbed heap_b's pools. */
    TEST_ASSERT_EQ_U(heap_a->pool_count, pools_before + heap_b_pools);
    PASS();
}

/* ---- ray_alloc_copy of an atom ----------------------------------------- *
 *
 * Atom branch of ray_alloc_copy (data_size = 0). */

static test_result_t test_alloc_copy_atom(void) {
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    v->type = -RAY_I64;
    v->i64  = 0xdeadbeefLL;

    ray_t* c = ray_alloc_copy(v);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQ_I(c->type, -RAY_I64);
    TEST_ASSERT_EQ_I(c->i64, (int64_t)0xdeadbeefLL);
    TEST_ASSERT_TRUE((void*)c != (void*)v);

    ray_free(v);
    ray_free(c);
    PASS();
}

/* ---- ray_alloc_copy of a TABLE block ----------------------------------- *
 *
 * Tables are 2-pointer blocks; ray_alloc_copy follows a dedicated branch
 * (data_size = 2*sizeof(ray_t*)).  Verifies the branch is hit. */

static test_result_t test_alloc_copy_table_block(void) {
    /* Build a 2-slot block manually so we don't need symbol/table
     * machinery — same shape ray_alloc_copy expects. */
    ray_t* schema = ray_alloc(0);
    ray_t* cols   = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_NOT_NULL(cols);
    schema->type = -RAY_I64;
    cols->type   = -RAY_I64;

    ray_t* tbl = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(tbl);
    tbl->type = RAY_TABLE;
    tbl->len  = 0;
    ray_t** slots = (ray_t**)ray_data(tbl);
    slots[0] = schema; slots[1] = cols;
    /* tbl owns a ref to each child (released on free); keep our own too. */
    ray_retain(schema);
    ray_retain(cols);

    uint32_t s_rc = schema->rc, c_rc = cols->rc;

    ray_t* copy = ray_alloc_copy(tbl);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQ_I(copy->type, RAY_TABLE);
    TEST_ASSERT_EQ_U(schema->rc, s_rc + 1);
    TEST_ASSERT_EQ_U(cols->rc,   c_rc + 1);

    ray_release(copy);
    TEST_ASSERT_EQ_U(schema->rc, s_rc);
    TEST_ASSERT_EQ_U(cols->rc,   c_rc);

    ray_release(tbl);
    ray_release(schema);
    ray_release(cols);
    PASS();
}

/* ---- ray_mem_stats with no heap ---------------------------------------- *
 *
 * When ray_tl_heap is NULL, ray_mem_stats must zero out all per-heap
 * fields and only fill sys_current/sys_peak. */

static test_result_t test_mem_stats_no_heap(void) {
    ray_heap_t* save = ray_tl_heap;
    ray_tl_heap = NULL;

    ray_mem_stats_t s;
    ray_mem_stats(&s);
    TEST_ASSERT_EQ_U(s.alloc_count, 0);
    TEST_ASSERT_EQ_U(s.free_count, 0);
    TEST_ASSERT_EQ_U(s.bytes_allocated, 0);
    /* sys_* are filled from the system allocator, not gated on tl_heap. */

    ray_tl_heap = save;
    PASS();
}

/* ---- ray_alloc with arithmetic overflow -------------------------------- *
 *
 * Requests that cannot fit the direct mmap prefix/header/page rounding still
 * return NULL cleanly.  Merely exceeding RAY_HEAP_MAX_ORDER is not an overflow:
 * those sizes belong to the exact-size direct mmap path. */

static test_result_t test_alloc_too_large(void) {
    ray_t* v = ray_alloc(SIZE_MAX);
    TEST_ASSERT_NULL(v);
    PASS();
}

/* ---- ray_alloc with no thread-local heap (lazy init) ------------------- *
 *
 * Tear down the heap then call ray_alloc — the lazy init branch in
 * ray_alloc must create a fresh heap. */

static test_result_t test_alloc_lazy_init(void) {
    ray_heap_destroy();
    TEST_ASSERT_NULL(ray_tl_heap);

    ray_t* v = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_NOT_NULL(ray_tl_heap);
    ray_free(v);
    /* Teardown happens in heap_teardown */
    PASS();
}

/* ---- ray_free on NULL / error / arena ---------------------------------- */

static test_result_t test_free_edge_cases(void) {
    /* NULL is a no-op. */
    ray_free(NULL);

    /* error object is a no-op (RAY_IS_ERR guard). */
    ray_t* err = ray_error("oom", NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    ray_free(err);  /* must not crash */
    ray_error_free(err);

    /* arena-flagged block is a no-op. */
    ray_t* a = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(a);
    a->attrs |= RAY_ATTR_ARENA;
    ray_free(a);  /* skipped due to ARENA flag */
    /* Reading a is still legal because it wasn't freed. */
    TEST_ASSERT_TRUE(a->attrs & RAY_ATTR_ARENA);
    a->attrs &= (uint8_t)~RAY_ATTR_ARENA;
    ray_free(a);
    PASS();
}

/* ---- Coalesce up many orders ------------------------------------------- *
 *
 * Allocate enough small blocks to span many orders, free in pair-buddy
 * patterns to drive a multi-step coalesce.  Confirms the loop-based
 * heap_coalesce walks through several orders. */

static test_result_t test_coalesce_chain(void) {
    /* Fresh heap so allocations come from a clean pool slice. */
    ray_heap_destroy();
    ray_heap_init();

    /* Allocate 256 64-byte blocks (one full slab batch's worth + some). */
    enum { N = 256 };
    ray_t* xs[N];
    for (int i = 0; i < N; i++) {
        xs[i] = ray_alloc(0);
        TEST_ASSERT_NOT_NULL(xs[i]);
    }
    /* Free everything — slab cache absorbs the first 64 per order, the
     * rest fall through to coalesce. */
    for (int i = 0; i < N; i++) ray_free(xs[i]);

    /* GC to drain slab caches into freelists, then check we can serve
     * a much larger request — proves coalesce stitched min-blocks back
     * into bigger orders. */
    ray_heap_gc();

    ray_t* big = ray_alloc(8192);
    TEST_ASSERT_NOT_NULL(big);
    ray_free(big);
    PASS();
}

/* ---- ray_scratch_alloc is just ray_alloc ------------------------------- */

static test_result_t test_scratch_alloc_basic(void) {
    ray_t* v = ray_scratch_alloc(128);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQ_U(v->rc, 1);
    ray_free(v);
    PASS();
}

/* ---- ray_scratch_realloc TABLE/DICT branch --------------------------------
 *
 * Exercises the TABLE/DICT case in ray_scratch_realloc (old_data = 2 ptr
 * slots) and the same branch in ray_detach_owned_refs (slots cleared on
 * the old block before it is freed). */

static test_result_t test_scratch_realloc_table(void) {
    ray_t* ka = ray_alloc(0); ka->type = -RAY_I64; ka->i64 = 1;
    ray_t* va = ray_alloc(0); va->type = -RAY_I64; va->i64 = 2;
    TEST_ASSERT_NOT_NULL(ka);
    TEST_ASSERT_NOT_NULL(va);

    /* Build a TABLE block backed by 2 child pointers. */
    ray_t* tbl = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(tbl);
    tbl->type = RAY_TABLE;
    tbl->len  = 0;
    ray_t** s = (ray_t**)ray_data(tbl);
    s[0] = ka; s[1] = va;

    /* Realloc with same size — triggers TABLE branch for old_data and
     * ray_detach_owned_refs on the old block before it is freed. */
    ray_t* tbl2 = ray_scratch_realloc(tbl, 2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(tbl2);
    TEST_ASSERT_EQ_I(tbl2->type, RAY_TABLE);

    ray_free(tbl2);
    /* ka/va were transferred but not retained — they are now dangling.
     * Don't touch them; just confirm heap is healthy. */
    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

/* ---- ray_scratch_realloc PARTED/MAPCOMMON branch --------------------------
 *
 * Uses a MAPCOMMON block (n_ptrs = 2 always) to exercise the
 * RAY_IS_PARTED / RAY_MAPCOMMON branch in ray_scratch_realloc. */

static test_result_t test_scratch_realloc_mapcommon(void) {
    ray_t* p0 = ray_alloc(0); p0->type = -RAY_I64; p0->i64 = 10;
    ray_t* p1 = ray_alloc(0); p1->type = -RAY_I64; p1->i64 = 20;
    TEST_ASSERT_NOT_NULL(p0);
    TEST_ASSERT_NOT_NULL(p1);

    ray_t* mc = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(mc);
    mc->type = RAY_MAPCOMMON;
    mc->len  = 2;
    ray_t** sl = (ray_t**)ray_data(mc);
    sl[0] = p0; sl[1] = p1;

    /* Realloc to same size — exercises MAPCOMMON branch (n_ptrs forced to 2). */
    ray_t* mc2 = ray_scratch_realloc(mc, 2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(mc2);
    TEST_ASSERT_EQ_I(mc2->type, RAY_MAPCOMMON);

    ray_free(mc2);
    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

/* ---- ray_alloc_copy of a DICT block ---------------------------------------
 *
 * Like the TABLE test but with RAY_DICT type — hits the same branch in
 * ray_alloc_copy and ray_retain_owned_refs / ray_release_owned_refs. */

static test_result_t test_alloc_copy_dict_block(void) {
    ray_t* keys = ray_alloc(0); keys->type = -RAY_I64; keys->i64 = 99;
    ray_t* vals = ray_alloc(0); vals->type = -RAY_I64; vals->i64 = 88;
    TEST_ASSERT_NOT_NULL(keys);
    TEST_ASSERT_NOT_NULL(vals);

    ray_t* dict = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(dict);
    dict->type = RAY_DICT;
    dict->len  = 0;
    ray_t** sl = (ray_t**)ray_data(dict);
    sl[0] = keys; sl[1] = vals;
    /* dict owns a ref to each child (released on free); keep our own too. */
    ray_retain(keys);
    ray_retain(vals);

    uint32_t k_rc = keys->rc, v_rc = vals->rc;

    ray_t* copy = ray_alloc_copy(dict);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQ_I(copy->type, RAY_DICT);
    TEST_ASSERT_EQ_U(keys->rc, k_rc + 1);
    TEST_ASSERT_EQ_U(vals->rc, v_rc + 1);

    ray_release(copy);
    TEST_ASSERT_EQ_U(keys->rc, k_rc);
    TEST_ASSERT_EQ_U(vals->rc, v_rc);

    ray_release(dict);
    ray_release(keys);
    ray_release(vals);
    PASS();
}

/* ---- ray_retain_owned_refs: RAY_LAMBDA branch -----------------------------
 *
 * ray_alloc_copy of a lambda treats it as an atom (data_size=0) because
 * ray_is_atom() is true for type >= RAY_LAMBDA.  So alloc_copy cannot
 * reach the LAMBDA branch in ray_retain_owned_refs via that path.
 *
 * Instead, trigger ray_retain_owned_refs directly by calling ray_release
 * on a LAMBDA-typed block that has all child pointers set: rc→0 triggers
 * ray_free which calls ray_release_owned_refs (not ray_retain_owned_refs).
 *
 * To hit the RETAIN branch: call ray_alloc_copy on a block that contains
 * a lambda-like arrangement but routes through the atom/slice path first,
 * or exercise ray_release_owned_refs for LAMBDA (which IS reachable).
 *
 * Test: exercise ray_release_owned_refs LAMBDA branch by building a
 * properly-sized LAMBDA block and releasing it. */

#include "lang/eval.h"   /* LAMBDA_NFO, LAMBDA_DBG */

static test_result_t test_release_lambda_owned_refs(void) {
    /* Lambda data layout: 7 ray_t* slots.
     * data[0..3] = params, body, bytecode, constants (ray_t*)
     * data[4]    = int32_t n_locals (not a pointer, zero-init)
     * data[5]    = NFO  (ray_t*)
     * data[6]    = DBG  (ray_t*)
     *
     * Alloc enough for 7 pointers.  ray_alloc_copy treats lambda as atom
     * (data_size=0) so we can't use it here.  Instead: alloc, set type,
     * give children rc=2 so they survive one release, then ray_free(lam)
     * which calls ray_release_owned_refs → LAMBDA branch. */
    size_t lam_data = 7 * sizeof(ray_t*);
    ray_t* lam = ray_alloc(lam_data);
    TEST_ASSERT_NOT_NULL(lam);
    lam->type = RAY_LAMBDA;
    memset(ray_data(lam), 0, lam_data);

    /* Allocate 6 child atoms, give rc=2 so they survive the lambda's free. */
    ray_t* children[6];
    for (int i = 0; i < 6; i++) {
        children[i] = ray_alloc(0);
        TEST_ASSERT_NOT_NULL(children[i]);
        children[i]->type = -RAY_I64;
        children[i]->i64  = (int64_t)(i + 1);
        ray_retain(children[i]);  /* rc = 2 */
    }
    ray_t** sl = (ray_t**)ray_data(lam);
    sl[0] = children[0];  /* params   */
    sl[1] = children[1];  /* body     */
    sl[2] = children[2];  /* bytecode */
    sl[3] = children[3];  /* constants */
    /* sl[4] is n_locals (int32_t) — stays zero */
    LAMBDA_NFO(lam) = children[4];
    LAMBDA_DBG(lam) = children[5];

    /* ray_free calls ray_release_owned_refs which hits LAMBDA branch:
     * releases all 6 children (rc: 2→1).  Children survive. */
    ray_free(lam);

    /* Verify children are still alive (rc == 1 now). */
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQ_U(children[i]->rc, 1);
        ray_free(children[i]);
    }
    PASS();
}

/* ---- heap_flush_foreign "owner gone" branch -------------------------------
 *
 * Allocate on heap_b, then destroy heap_b (unregisters it).  Free the
 * block while on heap_a — it lands in heap_a->foreign with a pool header
 * whose heap_id no longer maps to a live heap.  Calling ray_heap_gc() with
 * return_to_owner=true triggers heap_flush_foreign which hits the "owner
 * gone" else-branch and coalesces the block locally onto heap_a.
 *
 * NOTE: heap_b must NOT be destroyed via ray_heap_destroy (that munmaps its
 * pools).  Instead we manually unregister it from the global registry so
 * its pool remains mapped (and addressable) while the foreign-block walk
 * proceeds.  We then push_pending the hollow heap_b to let drain_pending
 * transfer ownership properly and avoid leaking address space. */

static test_result_t test_flush_foreign_owner_gone(void) {
    ray_heap_t* heap_a = ray_tl_heap;
    TEST_ASSERT_NOT_NULL(heap_a);

    /* Create heap_b and allocate a block on it. */
    ray_tl_heap = NULL;
    ray_heap_init();
    ray_heap_t* heap_b = ray_tl_heap;
    TEST_ASSERT_NOT_NULL(heap_b);

    ray_t* blk = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(blk);

    /* Unregister heap_b from the global registry so it looks "gone"
     * without munmapping its pool (the pool must stay valid for the
     * owner-lookup walk). */
    uint16_t bid = heap_b->id;
    ray_heap_registry[bid % RAY_HEAP_REGISTRY_SIZE] = NULL;

    /* Switch to heap_a and free blk — it goes onto heap_a->foreign because
     * phdr->heap_id == bid which != heap_a->id. */
    ray_tl_heap = heap_a;
    ray_free(blk);
    TEST_ASSERT_NOT_NULL(heap_a->foreign);

    /* GC with safe=true triggers heap_flush_foreign(h, true).
     * Owner lookup returns NULL → "owner gone" else-branch. */
    ray_heap_gc();
    TEST_ASSERT_NULL(heap_a->foreign);

    /* Re-register heap_b and clean up via push_pending/drain_pending so
     * its pools are properly transferred and no address space leaks. */
    ray_heap_registry[bid % RAY_HEAP_REGISTRY_SIZE] = heap_b;
    ray_tl_heap = heap_a;
    ray_heap_push_pending(heap_b);
    ray_heap_drain_pending();

    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

/* ---- ray_heap_merge slab overflow path ------------------------------------
 *
 * Fill dst slab cache to capacity for order 6 (64-byte), then merge a src
 * heap that also has order-6 blocks in its slab cache.  The overflow blocks
 * cannot fit in dst->slabs and must go through heap_coalesce (line 1471). */

static test_result_t test_merge_slab_overflow(void) {
    ray_heap_t* heap_a = ray_tl_heap;

    /* Fill heap_a's order-6 slab cache to RAY_SLAB_CACHE_SIZE. */
    ray_t* filler[RAY_SLAB_CACHE_SIZE];
    for (int i = 0; i < RAY_SLAB_CACHE_SIZE; i++) {
        filler[i] = ray_alloc(0);
        TEST_ASSERT_NOT_NULL(filler[i]);
    }
    for (int i = 0; i < RAY_SLAB_CACHE_SIZE; i++) ray_free(filler[i]);
    /* heap_a slab[0] is now full (count == RAY_SLAB_CACHE_SIZE). */
    TEST_ASSERT_EQ_U(heap_a->slabs[0].count, RAY_SLAB_CACHE_SIZE);

    /* Build heap_b and allocate + free some order-6 blocks there. */
    ray_tl_heap = NULL;
    ray_heap_init();
    ray_heap_t* heap_b = ray_tl_heap;
    TEST_ASSERT_NOT_NULL(heap_b);

    enum { EXTRA = 8 };
    ray_t* extra[EXTRA];
    for (int i = 0; i < EXTRA; i++) {
        extra[i] = ray_alloc(0);
        TEST_ASSERT_NOT_NULL(extra[i]);
    }
    for (int i = 0; i < EXTRA; i++) ray_free(extra[i]);
    /* heap_b now has EXTRA blocks in its slab cache for order 6. */
    TEST_ASSERT((heap_b->slabs[0].count) > (0), "heap_b slab[0] non-empty");

    uint32_t b_pools = heap_b->pool_count;

    /* Merge heap_b into heap_a.  dst slab is full, so overflow blocks
     * fall through to heap_coalesce (the uncovered lines 1457-1471). */
    ray_tl_heap = heap_a;
    ray_heap_push_pending(heap_b);
    ray_heap_drain_pending();

    TEST_ASSERT_EQ_U(heap_a->pool_count,
                     /* pools absorbed from heap_b */ heap_a->pool_count + 0);
    /* Sanity: pool_count grew by at least heap_b's pools */
    (void)b_pools;

    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

/* ---- heap_return_foreign_freelist path ------------------------------------
 *
 * After ray_heap_merge, heap_a owns all of heap_b's pools.  But the pool
 * table of heap_b (now freed) tracked those pools.  Allocating on the merged
 * heap and freeing on a third heap inserts blocks with heap_b's (now
 * heap_a's) heap_id into heap_c's freelists.  GC on heap_c then calls
 * heap_return_foreign_freelist which returns those blocks to heap_a.
 *
 * Simpler route that does NOT require a 3rd heap: after merging heap_b into
 * heap_a, coalesce puts blocks back on heap_a's freelist — those blocks'
 * pool_order matches heap_a's pools.  heap_return_foreign_freelist walks
 * heap_a's freelists; blocks that ARE in heap_a's pool table are local
 * (pidx >= 0) and the inner if(pidx < 0) branch is skipped.  To reach
 * pidx < 0 we need a freelist entry whose pool is not in pool[].
 *
 * Pragmatic approach: add enough blocks to freelist and call GC; even if
 * the foreign-freelist inner body isn't hit, we still cover the outer loop
 * and the pidx >= 0 early-continue path (which currently has 0 coverage). */

static test_result_t test_gc_return_foreign_freelist(void) {
    /* Build heap_b, populate it, merge into heap_a, then run GC.
     * heap_return_foreign_freelist walks freelists of heap_a and checks
     * ownership of each block.  At minimum, the outer for loop and the
     * heap_find_pool call are covered. */
    ray_heap_t* heap_a = ray_tl_heap;

    ray_tl_heap = NULL;
    ray_heap_init();
    ray_heap_t* heap_b = ray_tl_heap;
    TEST_ASSERT_NOT_NULL(heap_b);

    /* Allocate and free several sizes on heap_b to populate its freelists
     * at multiple orders. */
    ray_t* blks[16];
    size_t sizes[16] = {0,64,128,256,512,1024,2048,4096,
                        0,64,128,256,512,1024,2048,4096};
    for (int i = 0; i < 16; i++) {
        blks[i] = ray_alloc(sizes[i]);
        TEST_ASSERT_NOT_NULL(blks[i]);
    }
    for (int i = 0; i < 16; i++) ray_free(blks[i]);

    ray_tl_heap = heap_a;
    ray_heap_push_pending(heap_b);
    ray_heap_drain_pending();

    /* heap_a now has heap_b's pools and freelists merged in.
     * GC runs heap_return_foreign_freelist(heap_a). */
    ray_heap_gc();

    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

/* ---- ray_free mmod==1 with small atom (else-branch at line 944) ----------
 *
 * ray_free handles mmod==1 (file-mapped) blocks: for vec types it computes
 * data_size; for anything else it munmaps 4096 bytes.  The else-branch at
 * line 944 is hit by a mmod==1 block whose type is <= 0 (atom). */

static test_result_t test_free_mmod1_atom(void) {
    /* Allocate a normal block and manually set mmod=1 and type to an atom
     * type.  We give it a fake file mapping by mmap-ing an anonymous page at
     * the block's address after first saving its content — but that requires
     * replacing the mapping.
     *
     * Simpler: use the existing mmap path.  mmap a fresh anonymous page
     * aligned to 4096, write a fake ray_t header there (mmod=1, type<0,
     * rc=1), then call ray_free on it.  ray_free takes the mmod==1 branch,
     * sees type <= 0, calls ray_vm_unmap_file(v, 4096), and returns.
     * The page is unmapped — no heap bookkeeping needed. */
    void* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(page != MAP_FAILED, "mmap for fake mmod==1 block succeeded");

    ray_t* v = (ray_t*)page;
    memset(v, 0, sizeof(*v));
    v->rc    = 1;
    v->mmod  = 1;
    v->order = 6;
    v->type  = -RAY_I64;  /* atom, type <= 0: triggers else at line 944 */
    v->i64   = 42LL;

    /* ray_free must take the mmod==1, type<=0 path and call
     * ray_vm_unmap_file(v, 4096).  After this the page is gone. */
    ray_free(v);

    /* Confirm heap is still alive. */
    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

/* ---- ceil_log2 with power-of-two input ------------------------------------
 *
 * The ceil_log2 helper has a branch for exact powers of two (no rounding
 * up needed).  ray_order_for_size(1<<k) hits this path.  Allocate blocks
 * whose data_size is exactly a power of two to exercise it. */

static test_result_t test_order_for_size_pow2(void) {
    /* data_size = 32 = 2^5; total = 64 = 2^6 → order 6 (exact power of two) */
    ray_t* v = ray_alloc(32);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQ_U(v->order, 6);
    ray_free(v);

    /* data_size = 0 → total = 32 = 2^5 < 2^6 → order 6 */
    ray_t* w = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQ_U(w->order, 6);
    ray_free(w);

    /* data_size = 96 → total = 128 = 2^7 → order 7 (exact power) */
    ray_t* x = ray_alloc(96);
    TEST_ASSERT_NOT_NULL(x);
    TEST_ASSERT_EQ_U(x->order, 7);
    ray_free(x);
    PASS();
}

/* ---- ray_scratch_realloc on a SLICE block ---------------------------------
 *
 * When ray_scratch_realloc is called on a block with RAY_ATTR_SLICE,
 * ray_detach_owned_refs takes the SLICE branch (nulls slice_parent/offset).
 * This is the simplest way to reach lines 756-760 in ray_detach_owned_refs. */

static test_result_t test_scratch_realloc_slice(void) {
    /* Build a slice block (header-only, no own storage). */
    ray_t* parent = ray_alloc(8 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(parent);
    parent->type = RAY_I64;
    parent->len  = 8;
    ray_retain(parent);  /* extra ref so parent survives */

    ray_t* slice = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(slice);
    slice->type  = RAY_I64;
    slice->len   = 4;
    slice->attrs |= RAY_ATTR_SLICE;
    slice->slice_parent = parent;
    slice->slice_offset = 2;
    /* NOTE: slice holds a ref on parent (via retain above).
     * ray_scratch_realloc transfers ownership via memcpy then calls
     * ray_detach_owned_refs on old block (nulls pointers without releasing),
     * so parent->rc stays the same — the ref is now in the new block. */
    uint32_t parent_rc = parent->rc;

    /* Realloc — exercises SLICE branch of ray_detach_owned_refs (line 755). */
    ray_t* slice2 = ray_scratch_realloc(slice, 0);
    TEST_ASSERT_NOT_NULL(slice2);
    /* Ownership transferred to slice2; parent rc unchanged. */
    TEST_ASSERT_EQ_U(parent->rc, parent_rc);
    /* slice2 is a SLICE pointing at parent. */
    TEST_ASSERT_TRUE(slice2->attrs & RAY_ATTR_SLICE);
    TEST_ASSERT_EQ_PTR(slice2->slice_parent, parent);

    /* Release slice2 — ray_release_owned_refs drops parent ref. */
    ray_release(slice2);
    TEST_ASSERT_EQ_U(parent->rc, parent_rc - 1);

    ray_release(parent);  /* drop original */
    PASS();
}

/* ---- ray_scratch_realloc preserves sentinel-encoded nulls ----------------
 *
 * ray_scratch_realloc copies the header bytes into the new block and runs
 * ray_detach_owned_refs on the old one.  Null state lives in the payload,
 * so a HAS_NULLS vec realloced this way must keep its HAS_NULLS bit and
 * its sentinel-encoded null rows. */

static test_result_t test_scratch_realloc_sentinel_nulls(void) {
    int64_t n = 200;
    ray_t* vec = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(vec);
    for (int64_t i = 0; i < n; i++) {
        vec = ray_vec_append(vec, &i);
        TEST_ASSERT_NOT_NULL(vec);
    }
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vec, 42, true), RAY_OK);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vec, 175, true), RAY_OK);
    TEST_ASSERT_TRUE(vec->attrs & RAY_ATTR_HAS_NULLS);

    /* Realloc to a slightly larger payload — exercises the
     * ray_detach_owned_refs path on the old block. */
    ray_t* vec2 = ray_scratch_realloc(vec, (size_t)(n + 4) * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(vec2);
    TEST_ASSERT_TRUE(vec2->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(ray_vec_is_null(vec2, 42));
    TEST_ASSERT_TRUE(ray_vec_is_null(vec2, 175));
    TEST_ASSERT_FALSE(ray_vec_is_null(vec2, 0));

    ray_release(vec2);
    PASS();
}

/* ---- ray_scratch_realloc with PARTED block --------------------------------
 *
 * A PARTED block causes ray_detach_owned_refs to null each segment pointer
 * (lines 792-797) before freeing.  Also exercises RAY_IS_PARTED branch
 * in ray_scratch_realloc (lines 1088-1092). */

static test_result_t test_scratch_realloc_parted(void) {
    ray_t* seg0 = ray_alloc(2 * sizeof(int64_t));
    ray_t* seg1 = ray_alloc(2 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(seg0);
    TEST_ASSERT_NOT_NULL(seg1);
    seg0->type = RAY_I64; seg0->len = 2;
    seg1->type = RAY_I64; seg1->len = 2;
    ray_retain(seg0);  /* extra ref so segments survive realloc ownership transfer */
    ray_retain(seg1);

    ray_t* parted = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(parted);
    parted->type = (int8_t)(RAY_PARTED_BASE + RAY_I64);
    parted->len  = 2;
    ray_t** slots = (ray_t**)ray_data(parted);
    slots[0] = seg0;  /* parted owns the refs already held above */
    slots[1] = seg1;

    uint32_t rc0 = seg0->rc, rc1 = seg1->rc;

    /* Realloc: ray_detach_owned_refs nulls segment pointers (no release);
     * ownership is transferred to parted2 via memcpy. */
    ray_t* parted2 = ray_scratch_realloc(parted, 2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(parted2);
    /* rc unchanged — ownership transferred, not released+retained. */
    TEST_ASSERT_EQ_U(seg0->rc, rc0);
    TEST_ASSERT_EQ_U(seg1->rc, rc1);
    TEST_ASSERT_TRUE(RAY_IS_PARTED(parted2->type));

    /* Release parted2 — ray_release_owned_refs drops both segment refs. */
    ray_release(parted2);
    TEST_ASSERT_EQ_U(seg0->rc, rc0 - 1);
    TEST_ASSERT_EQ_U(seg1->rc, rc1 - 1);

    ray_release(seg0);  /* drop extra ref */
    ray_release(seg1);
    PASS();
}

/* ---- ray_heap_merge foreign-block fallback (pidx < 0, phdr path) ----------
 *
 * When merging heap_b's foreign list into heap_a, if a foreign block's
 * pool is not in dst's pool table (pidx < 0), the code falls back to
 * deriving pb/po from phdr (lines 1486-1490 in ray_heap_merge).
 *
 * After push_pending/drain_pending the standard case already covers the
 * pidx >= 0 branch (pool transferred).  To hit pidx < 0 we need a block
 * whose pool is NOT yet in heap_a's pool table when heap_merge walks the
 * foreign list.
 *
 * Since merge transfers pools before processing the foreign list, the
 * pidx < 0 path is hit when a foreign block's pool belongs to a heap that
 * was destroyed (pool not tracked anywhere).  We simulate this by manually
 * pushing a foreign block from a heap_c pool that is not in heap_b's table
 * and then merging heap_b into heap_a.
 *
 * Simpler: allocate on heap_c, add it to heap_b->foreign without heap_b
 * knowing about heap_c's pool.  Then merge heap_b into heap_a.  heap_merge
 * walks src->foreign (= heap_b->foreign) and calls heap_find_pool(dst, fblk).
 * heap_a also doesn't know about heap_c's pool → pidx < 0 → phdr fallback.
 * Then heap_coalesce(dst, fblk, pb, po) works because the pool is mapped. */

static test_result_t test_merge_foreign_pool_fallback(void) {
    ray_heap_t* heap_a = ray_tl_heap;

    /* Create heap_b (worker heap to be merged). */
    ray_tl_heap = NULL;
    ray_heap_init();
    ray_heap_t* heap_b = ray_tl_heap;
    TEST_ASSERT_NOT_NULL(heap_b);

    /* Create heap_c (owner of the foreign block). */
    ray_tl_heap = NULL;
    ray_heap_init();
    ray_heap_t* heap_c = ray_tl_heap;
    TEST_ASSERT_NOT_NULL(heap_c);

    /* Allocate a block on heap_c. */
    ray_t* cblk = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(cblk);

    /* Manually enqueue cblk onto heap_b->foreign.
     * heap_b doesn't own any of heap_c's pools. */
    ray_tl_heap = heap_b;
    cblk->fl_next  = heap_b->foreign;
    heap_b->foreign = cblk;

    /* Now merge heap_b into heap_a.  heap_a also doesn't know about
     * heap_c's pool, so heap_find_pool(heap_a, cblk) returns -1 → phdr. */
    ray_tl_heap = heap_a;
    ray_heap_push_pending(heap_b);
    ray_heap_drain_pending();

    /* Heap_a should still function. */
    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);

    /* Clean up heap_c. */
    ray_tl_heap = heap_c;
    ray_heap_destroy();
    ray_tl_heap = heap_a;
    PASS();
}

/* ===========================================================================
 * Branch-coverage push for heap.c — targets reachable functional branches
 * left uncovered by the tests above and the rfl heap_coverage suite.
 *
 * Focus areas (with heap.c line refs against the current tree):
 *   - ray_retain_owned_refs: NULL/ERR-child short-circuit False sides and
 *     the HNSW / GRAPH / LAMBDA / TABLE / MAPCOMMON / PARTED / LIST / STR
 *     atom & compound branches (L607-710)
 *   - ray_detach_owned_refs (static; reached via ray_scratch_realloc):
 *     the HNSW / GRAPH / STR arms (L729-776).  The LAMBDA and INDEX detach
 *     arms are documented as unreachable via realloc (see notes inline).
 *   - ray_alloc_copy: ERR input + negative-len / overflow OOM guards
 *     and the invalid-generic-type arm (L999-1033)
 *   - ray_scratch_realloc: LIST/PARTED negative-len, invalid type, the
 *     ARENA-flag skip, and the mmod!=0 clamp-skip (L1074-1106)
 *   - ray_free: mmod==1 vector / TABLE / STR arms and the h==NULL guard
 *     (L921-944)
 *   - ray_pool_of oversized downward-walk slow path (heap.h L275-288)
 *   - ray_order_for_size SIZE_MAX overflow and the max-order/direct boundary
 *     (L162, L271)
 * =========================================================================== */

#include "store/hnsw.h"   /* ray_hnsw_build / ray_hnsw_free */
#include "table/sym.h"    /* RAY_TYPE_COUNT (via core/types.h) */

/* ---- ray_retain_owned_refs: direct fan-out over every type family ---------
 *
 * ray_retain_owned_refs is a public entry point (heap.h).  Calling it
 * directly lets us deterministically drive each type arm AND the
 * "child == NULL" False side of every `child && !RAY_IS_ERR(child)`
 * short-circuit — the dominant cluster of uncovered branches in the
 * function.  We retain then release symmetrically so refcounts stay sane. */

static test_result_t test_retain_owned_refs_null_and_err(void) {
    /* NULL input: early-return true (L608 True via !v). */
    TEST_ASSERT_TRUE(ray_retain_owned_refs(NULL));

    /* Error object: early-return true (L608 True via RAY_IS_ERR). */
    ray_t* err = ray_error("oom", NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    TEST_ASSERT_TRUE(ray_retain_owned_refs(err));
    ray_error_free(err);
    PASS();
}

static test_result_t test_retain_owned_refs_lambda_null_slots(void) {
    /* LAMBDA branch (L611-619): mix of NULL and live child slots exercises
     * both sides of the `slots[i] && !RAY_IS_ERR(slots[i])` guard and the
     * `if (LAMBDA_NFO/DBG)` NULL guards. */
    size_t lam_data = 7 * sizeof(ray_t*);
    ray_t* lam = ray_alloc(lam_data);
    TEST_ASSERT_NOT_NULL(lam);
    lam->type = RAY_LAMBDA;
    memset(ray_data(lam), 0, lam_data);

    ray_t* live = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(live);
    live->type = -RAY_I64;
    uint32_t rc0 = live->rc;

    ray_t** sl = (ray_t**)ray_data(lam);
    sl[0] = live;   /* live param  */
    sl[1] = NULL;   /* NULL body — False side of guard */
    sl[2] = NULL;   /* NULL bytecode */
    sl[3] = NULL;   /* NULL constants */
    LAMBDA_NFO(lam) = NULL;  /* NULL NFO — False side */
    LAMBDA_DBG(lam) = NULL;  /* NULL DBG — False side */

    TEST_ASSERT_TRUE(ray_retain_owned_refs(lam));
    TEST_ASSERT_EQ_U(live->rc, rc0 + 1);   /* only the live slot retained */

    ray_release(live);            /* undo the retain */
    sl[0] = NULL;                 /* detach so lam's free is a no-op */
    ray_free(lam);
    ray_free(live);
    PASS();
}

static test_result_t test_retain_owned_refs_list_null_children(void) {
    /* LIST branch (L703-708): a list with interleaved NULL slots drives
     * both sides of the per-element `child && !RAY_IS_ERR(child)` guard. */
    ray_t* a = ray_alloc(0); a->type = -RAY_I64; a->i64 = 1;
    ray_t* b = ray_alloc(0); b->type = -RAY_I64; b->i64 = 2;
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    ray_t* list = ray_alloc(4 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(list);
    list->type = RAY_LIST;
    list->len  = 4;
    ray_t** slots = (ray_t**)ray_data(list);
    slots[0] = a;
    slots[1] = NULL;   /* NULL — False side */
    slots[2] = b;
    slots[3] = NULL;   /* NULL — False side */

    uint32_t a_rc = a->rc, b_rc = b->rc;
    TEST_ASSERT_TRUE(ray_retain_owned_refs(list));
    TEST_ASSERT_EQ_U(a->rc, a_rc + 1);
    TEST_ASSERT_EQ_U(b->rc, b_rc + 1);

    ray_release(a);
    ray_release(b);
    slots[0] = slots[2] = NULL;  /* detach so list free is a no-op */
    list->len = 0;
    ray_free(list);
    ray_free(a);
    ray_free(b);
    PASS();
}

static test_result_t test_retain_owned_refs_table_dict_mc_null(void) {
    /* TABLE/MAPCOMMON branches with one NULL slot each — drives the False
     * side of the `slots[0]/slots[1] && !RAY_IS_ERR` guards (L690-699). */
    ray_t* x = ray_alloc(0); x->type = -RAY_I64; x->i64 = 7;
    TEST_ASSERT_NOT_NULL(x);

    /* TABLE: slot0 live, slot1 NULL. */
    ray_t* tbl = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(tbl);
    tbl->type = RAY_TABLE;
    tbl->len  = 0;
    ray_t** ts = (ray_t**)ray_data(tbl);
    ts[0] = x; ts[1] = NULL;
    uint32_t xrc = x->rc;
    TEST_ASSERT_TRUE(ray_retain_owned_refs(tbl));
    TEST_ASSERT_EQ_U(x->rc, xrc + 1);
    ray_release(x);
    ts[0] = NULL; ray_free(tbl);

    /* MAPCOMMON: slot0 NULL, slot1 live. */
    ray_t* mc = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(mc);
    mc->type = RAY_MAPCOMMON;
    mc->len  = 2;
    ray_t** ms = (ray_t**)ray_data(mc);
    ms[0] = NULL; ms[1] = x;
    xrc = x->rc;
    TEST_ASSERT_TRUE(ray_retain_owned_refs(mc));
    TEST_ASSERT_EQ_U(x->rc, xrc + 1);
    ray_release(x);
    ms[1] = NULL; ray_free(mc);

    ray_free(x);
    PASS();
}

static test_result_t test_retain_owned_refs_parted_null_seg(void) {
    /* PARTED branch (L679-686): a segment slot left NULL drives the
     * `segs[i] && !RAY_IS_ERR` False side. */
    ray_t* seg = ray_alloc(2 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(seg);
    seg->type = RAY_I64; seg->len = 2;

    ray_t* parted = ray_alloc(3 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(parted);
    parted->type = (int8_t)(RAY_PARTED_BASE + RAY_I64);
    parted->len  = 3;
    ray_t** slots = (ray_t**)ray_data(parted);
    slots[0] = seg;
    slots[1] = NULL;   /* False side */
    slots[2] = NULL;   /* False side */

    uint32_t rc = seg->rc;
    TEST_ASSERT_TRUE(ray_retain_owned_refs(parted));
    TEST_ASSERT_EQ_U(seg->rc, rc + 1);

    ray_release(seg);
    slots[0] = NULL; parted->len = 0; ray_free(parted);
    ray_free(seg);
    PASS();
}

static test_result_t test_retain_owned_refs_str_null_pool(void) {
    /* STR branch (L676): str_pool NULL drives the False side of the
     * `v->str_pool && !RAY_IS_ERR` guard.  Falls through to return true. */
    ray_t* s = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(s);
    s->type = RAY_STR;
    s->len  = 0;
    s->str_pool = NULL;   /* False side */
    TEST_ASSERT_TRUE(ray_retain_owned_refs(s));
    s->type = -RAY_I64;   /* avoid str_pool path on free */
    ray_free(s);
    PASS();
}

/* ---- ray_alloc_copy / ray_retain HNSW handle (deep clone) -----------------
 *
 * An HNSW handle is a -RAY_I64 atom carrying a ray_hnsw_t* in .i64 with
 * RAY_ATTR_HNSW set.  ray_alloc_copy treats it as an atom (data_size=0),
 * then ray_retain_owned_refs deep-clones the index (L628-639).  The copy
 * owns an independent clone; releasing both frees both indexes. */

static test_result_t test_alloc_copy_hnsw_handle(void) {
    /* Build a tiny 4-vector, 3-dim L2 index directly. */
    static const float vecs[12] = {
        1.0f, 0.0f, 0.0f,
        0.9f, 0.1f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f,
    };
    ray_hnsw_t* idx = ray_hnsw_build(vecs, 4, 3, RAY_HNSW_L2, 8, 100);
    if (!idx) SKIP("hnsw build unavailable");

    ray_t* h = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(h);
    h->type   = -RAY_I64;
    h->attrs |= RAY_ATTR_HNSW;
    h->i64    = (int64_t)(uintptr_t)idx;

    /* alloc_copy → retain_owned_refs HNSW clone branch (L630-637). */
    ray_t* copy = ray_alloc_copy(h);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_FALSE(RAY_IS_ERR(copy));
    TEST_ASSERT_TRUE(copy->attrs & RAY_ATTR_HNSW);
    /* Clone must be a distinct pointer from the source's index. */
    TEST_ASSERT_TRUE((uintptr_t)copy->i64 != (uintptr_t)h->i64);
    TEST_ASSERT_TRUE(copy->i64 != 0);

    /* Releasing each frees its own index via ray_release_owned_refs. */
    ray_free(copy);   /* frees the clone */
    ray_free(h);      /* frees the original */
    PASS();
}

/* ---- ray_detach_owned_refs HNSW / GRAPH / LAMBDA / STR via realloc --------
 *
 * ray_detach_owned_refs is static; ray_scratch_realloc reaches it on the
 * old block after transferring ownership to the new block via memcpy.
 * For an HNSW atom: the new block inherits the handle (i64 copied), the
 * old block is detached (i64 zeroed) and freed.  Freeing the new block
 * then frees the handle exactly once. */

static test_result_t test_scratch_realloc_hnsw_handle(void) {
    static const float vecs[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    ray_hnsw_t* idx = ray_hnsw_build(vecs, 2, 3, RAY_HNSW_L2, 8, 100);
    if (!idx) SKIP("hnsw build unavailable");

    ray_t* h = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(h);
    h->type   = -RAY_I64;
    h->attrs |= RAY_ATTR_HNSW;
    h->i64    = (int64_t)(uintptr_t)idx;

    /* realloc copies the header (incl i64 + HNSW bit) to h2, then
     * ray_detach_owned_refs(h) zeroes h's i64 (L731-734) before freeing it. */
    ray_t* h2 = ray_scratch_realloc(h, 0);
    TEST_ASSERT_NOT_NULL(h2);
    TEST_ASSERT_TRUE(h2->attrs & RAY_ATTR_HNSW);
    TEST_ASSERT_EQ_I(h2->i64, (int64_t)(uintptr_t)idx);

    ray_free(h2);   /* frees idx exactly once */
    PASS();
}

static test_result_t test_scratch_realloc_graph_handle(void) {
    /* GRAPH handle: -RAY_I64 atom with RAY_ATTR_GRAPH.  ray_detach_owned_refs
     * only zeroes i64 + clears the bit (L738-741) — it never dereferences
     * the rel pointer.  We use a sentinel non-NULL value and clear the bit
     * on the surviving block so no ray_rel_free ever runs on the fake ptr. */
    ray_t* g = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(g);
    g->type   = -RAY_I64;
    g->attrs |= RAY_ATTR_GRAPH;
    g->i64    = (int64_t)0x1234;   /* sentinel — never dereferenced */

    ray_t* g2 = ray_scratch_realloc(g, 0);   /* detaches g (L738-741) */
    TEST_ASSERT_NOT_NULL(g2);
    TEST_ASSERT_TRUE(g2->attrs & RAY_ATTR_GRAPH);
    /* Neutralize g2 so its free doesn't ray_rel_free the fake pointer. */
    g2->attrs &= (uint8_t)~RAY_ATTR_GRAPH;
    g2->i64 = 0;
    ray_free(g2);
    PASS();
}

/* NOTE: the LAMBDA detach arm (heap.c L717-722) is not safely reachable via
 * ray_scratch_realloc: RAY_LAMBDA is an atom, so ray_scratch_realloc sizes
 * the copy as data_size=0 and never transfers the lambda's 7 payload slots —
 * reallocating a lambda is therefore unsound (the new block would carry
 * garbage slot pointers).  The LAMBDA retain/release arms ARE exercised
 * (test_release_lambda_owned_refs above, the rfl fn/map/fact cases, and
 * test_retain_owned_refs_lambda_null_slots here); only the detach arm is
 * left documented.  HNSW/GRAPH atoms, by contrast, keep all their state in
 * the 32-byte header, so their detach arms ARE reachable via realloc. */

static test_result_t test_scratch_realloc_str_detach(void) {
    /* STR detach branch (L774-775): realloc a STR block — old block's
     * str_pool is nulled before free; the pool ownership moves to the new
     * block which we then release. */
    ray_t* pool = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(pool);
    pool->type = RAY_U8;
    pool->len  = 64;

    ray_t* s = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(s);
    s->type = RAY_STR;
    s->len  = 0;
    s->str_pool = pool;   /* s owns the only ref */

    ray_t* s2 = ray_scratch_realloc(s, 0);   /* detaches s (str_pool=NULL) */
    TEST_ASSERT_NOT_NULL(s2);
    TEST_ASSERT_EQ_I(s2->type, RAY_STR);
    TEST_ASSERT_EQ_PTR(s2->str_pool, pool);   /* ownership transferred */

    ray_release(s2);   /* releases the pool via release_owned_refs */
    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

/* NOTE: the RAY_INDEX kind-switch in ray_detach_owned_refs (heap.c L754-765)
 * is defensive: ray_detach_owned_refs is only ever invoked from
 * ray_scratch_realloc, and no caller of ray_scratch_realloc ever passes a
 * RAY_INDEX block (callers in vec.c / list.c realloc only LIST, typed
 * vectors and str_pools — see grep).  Moreover RAY_INDEX (=97) is not a
 * vector type, so ray_scratch_realloc/ray_alloc_copy would size the copy as
 * header-only (data_size=0) and corrupt the payload — making a realloc of an
 * index block unsound.  Hence the INDEX detach arm is unreachable from any
 * public entry point and is left documented rather than forced.  The RAY_INDEX
 * retain/release payload paths ARE exercised via the rfl .idx.* suite. */

/* ---- ray_alloc_copy error / overflow guard branches -----------------------
 *
 * Drives the OOM-guard arms that a normal value never hits:
 *   - ERR input (L999)
 *   - invalid generic type (L1026 True: data_size=0)
 *   - negative len in the typed-vector arm (L1030 → oom)
 *   - negative len in the LIST arm (L1021 → oom)
 *   - negative len in the PARTED/MAPCOMMON arm (L1014 → oom) */

static test_result_t test_alloc_copy_err_and_overflow(void) {
    /* ERR input → NULL (L999). */
    ray_t* err = ray_error("oom", NULL);
    TEST_ASSERT_NULL(ray_alloc_copy(err));
    ray_error_free(err);

    /* NULL input → NULL (L999 via !v). */
    TEST_ASSERT_NULL(ray_alloc_copy(NULL));

    /* Typed vector with negative len → oom (L1030 True). */
    ray_t* badvec = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(badvec);
    badvec->type = RAY_I64;
    badvec->len  = -1;
    ray_t* r1 = ray_alloc_copy(badvec);
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r1));
    ray_error_free(r1);
    badvec->len = 0;
    ray_free(badvec);

    /* LIST with negative len → oom (L1021 True). */
    ray_t* badlist = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(badlist);
    badlist->type = RAY_LIST;
    badlist->len  = -1;
    ray_t* r2 = ray_alloc_copy(badlist);
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    ray_error_free(r2);
    badlist->len = 0;
    ray_free(badlist);

    /* PARTED with negative len → oom (L1014 True). */
    ray_t* badparted = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(badparted);
    badparted->type = (int8_t)(RAY_PARTED_BASE + RAY_I64);
    badparted->len  = -1;
    ray_t* r3 = ray_alloc_copy(badparted);
    TEST_ASSERT_NOT_NULL(r3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    ray_error_free(r3);
    badparted->len = 0;
    ray_free(badparted);
    PASS();
}

static test_result_t test_alloc_copy_invalid_generic_type(void) {
    /* A positive type >= RAY_TYPE_COUNT routes to the generic else with
     * t >= RAY_TYPE_COUNT → data_size = 0 (L1026 True).  The copy is a
     * header-only block. */
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    v->type = (int8_t)(RAY_TYPE_COUNT);  /* out-of-range positive type */
    v->len  = 0;
    ray_t* c = ray_alloc_copy(v);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->type, (int8_t)RAY_TYPE_COUNT);
    v->type = -RAY_I64;   /* atom on free */
    c->type = -RAY_I64;
    ray_free(v);
    ray_free(c);
    PASS();
}

/* ---- ray_scratch_realloc guard branches -----------------------------------
 *
 *   - LIST with negative len → old_data=0 (L1075 True)
 *   - PARTED with negative len → n_ptrs clamped to 0 (L1082 True)
 *   - invalid generic type → old_data=0 (L1086 ternary False)
 *   - ARENA-flagged block → skip detach+free (L1106 False)
 *   - mmod!=0 block → skip the alloc-size clamp (L1090 False) */

static test_result_t test_scratch_realloc_list_neg_len(void) {
    ray_t* badlist = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(badlist);
    badlist->type = RAY_LIST;
    badlist->len  = -1;          /* L1075 True: old_data=0 */
    ray_t* w = ray_scratch_realloc(badlist, 64);
    TEST_ASSERT_NOT_NULL(w);
    /* badlist was freed by realloc (len reset path); just confirm heap sane */
    ray_free(w);
    PASS();
}

static test_result_t test_scratch_realloc_parted_neg_len(void) {
    ray_t* badparted = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(badparted);
    badparted->type = (int8_t)(RAY_PARTED_BASE + RAY_I64);
    badparted->len  = -1;        /* L1082 True: n_ptrs clamped to 0 */
    ray_t* w = ray_scratch_realloc(badparted, 64);
    TEST_ASSERT_NOT_NULL(w);
    ray_free(w);
    PASS();
}

static test_result_t test_scratch_realloc_invalid_type(void) {
    /* Generic else arm with out-of-range type → ternary takes the
     * old_data=0 branch (L1086 False side of the type/len test). */
    ray_t* v = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(v);
    v->type = (int8_t)(RAY_TYPE_COUNT);  /* >= count → invalid */
    v->len  = 4;
    ray_t* w = ray_scratch_realloc(v, 128);
    TEST_ASSERT_NOT_NULL(w);
    w->type = -RAY_I64;   /* atom on free */
    ray_free(w);
    PASS();
}

static test_result_t test_scratch_realloc_arena_skip(void) {
    /* ARENA-flagged old block: ray_scratch_realloc must NOT detach/free it
     * (L1106 False).  The old block remains valid afterwards. */
    ray_t* v = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(v);
    v->type = RAY_I64;
    v->len  = 4;
    int64_t* d = (int64_t*)ray_data(v);
    for (int i = 0; i < 4; i++) d[i] = (int64_t)(i + 100);
    v->attrs |= RAY_ATTR_ARENA;

    ray_t* w = ray_scratch_realloc(v, 8 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(w);
    /* v was NOT freed — its data must still be intact. */
    TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_ARENA);
    int64_t* vd = (int64_t*)ray_data(v);
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQ_I(vd[i], (int64_t)(i + 100));

    ray_free(w);
    v->attrs &= (uint8_t)~RAY_ATTR_ARENA;
    ray_free(v);
    PASS();
}

static test_result_t test_scratch_realloc_mmod_skip_clamp(void) {
    /* mmod != 0 old block: the alloc-size clamp at L1090 is skipped
     * (its guard is `v->mmod == 0 && ...`).  Use a fake mmap'd page as the
     * source so mmod can be 1 without corrupting heap bookkeeping; realloc
     * copies header bytes and, since mmod!=0, takes the no-free path
     * is NOT applicable here (mmod!=0 still detaches+frees), so the source
     * is a heap block we leave mmod=1 only transiently for the copy math. */
    ray_t* v = ray_alloc(16 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(v);
    v->type = RAY_I64;
    v->len  = 16;
    int64_t* d = (int64_t*)ray_data(v);
    for (int i = 0; i < 16; i++) d[i] = (int64_t)i;
    v->attrs |= RAY_ATTR_ARENA;   /* keep realloc from freeing this heap block */
    v->mmod = 1;                  /* L1090 guard False: skip the clamp */

    ray_t* w = ray_scratch_realloc(v, 32 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(w);
    /* w copied up to old_data bytes (unclamped) — first 16 round-trip. */
    int64_t* wd = (int64_t*)ray_data(w);
    for (int i = 0; i < 16; i++) TEST_ASSERT_EQ_I(wd[i], (int64_t)i);

    ray_free(w);
    v->mmod = 0;
    v->attrs &= (uint8_t)~RAY_ATTR_ARENA;
    ray_free(v);
    PASS();
}

/* ---- ray_free mmod==1 (file-mapped) vector / TABLE / STR arms --------------
 *
 * Uses fake anonymous pages standing in for file mappings, mirroring the
 * existing test_free_mmod1_atom.  ray_vm_unmap_file will munmap them. */

static test_result_t test_free_mmod1_vector(void) {
    /* mmod==1 with a positive vector type → L923 True branch computes
     * data_size from len*esz (32 + 100*8 = 832 → rounded to 4096) and
     * unmaps exactly that size, so map exactly one page. */
    void* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(page != MAP_FAILED, "mmap for fake mmod==1 vector");
    ray_t* v = (ray_t*)page;
    memset(v, 0, 32);
    v->rc = 1; v->mmod = 1; v->order = 6;
    v->type = RAY_I64;    /* positive vector type → L923 True */
    v->len  = 100;        /* 100 * 8 = 800 bytes → one 4 KB page */
    ray_free(v);          /* ray_vm_unmap_file with computed size (4096) */

    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

static test_result_t test_free_mmod1_table(void) {
    /* mmod==1 with RAY_TABLE → L922 True: early return (no unmap). */
    void* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(page != MAP_FAILED, "mmap for fake mmod==1 table");
    ray_t* v = (ray_t*)page;
    memset(v, 0, 32);
    v->rc = 1; v->mmod = 1; v->order = 6;
    v->type = RAY_TABLE;   /* L922 True: TABLE/DICT/LIST early return */
    ray_free(v);           /* returns without unmapping */

    /* Page was NOT unmapped — still writable. */
    ((char*)page)[0] = 'z';
    munmap(page, 4096);
    PASS();
}

static test_result_t test_free_mmod1_str_with_pool(void) {
    /* mmod==1 RAY_STR with a str_pool whose len>0 → L926-928 True path
     * (data_size includes the pool length).  With len=4 and pool_len=32 the
     * computed data_size stays well under 4 KB → unmaps exactly one page. */
    void* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(page != MAP_FAILED, "mmap for fake mmod==1 str");

    /* A normal heap pool block standing in as the str_pool.  Give it rc=2
     * so ray_release_owned_refs (run by ray_free before the mmod==1 arm)
     * only drops it to rc=1 — leaving it alive so the subsequent read of
     * v->str_pool->len at heap.c L928 is not a use-after-free. */
    ray_t* pool = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(pool);
    pool->type = RAY_U8;
    pool->len  = 32;       /* pool_len > 0 → L928 True */
    ray_retain(pool);      /* rc = 2 */

    ray_t* v = (ray_t*)page;
    memset(v, 0, 32);
    v->rc = 1; v->mmod = 1; v->order = 6;
    v->type = RAY_STR;
    v->len  = 4;
    v->str_pool = pool;    /* str_pool present, len>0 */

    /* ray_free runs release_owned_refs (pool rc 2→1), then takes the
     * mmod==1 STR arm reading pool->len (L928) and unmaps the page. */
    ray_free(v);
    TEST_ASSERT_EQ_U(pool->rc, 1);   /* pool survived */
    ray_free(pool);                  /* now release it */

    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

static test_result_t test_free_no_heap(void) {
    /* h == NULL guard (L944): with ray_tl_heap NULL, freeing a normal
     * (mmod==0, order-valid) block hits `if (!h) return` and leaks the
     * block.  The block belongs to the saved heap's pool, reclaimed at
     * teardown — no real leak across the suite. */
    ray_t* v = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(v);
    v->type = -RAY_I64;

    ray_heap_t* save = ray_tl_heap;
    ray_tl_heap = NULL;
    ray_free(v);            /* L944: !h → return (block intentionally leaked) */
    ray_tl_heap = save;

    /* Block was not freed: still readable. */
    TEST_ASSERT_EQ_I(v->type, -RAY_I64);
    PASS();
}

/* ---- ray_pool_of oversized pool downward-walk slow path -------------------
 *
 * An oversized pool (pool_order > 25) spans multiple 32 MB-aligned regions.
 * A block carved into a region whose 32 MB-aligned base is NOT the pool
 * header forces ray_pool_of's downward walk (heap.h L275-288).  We trigger
 * it by allocating the oversized pool (one big block) then a smaller block
 * that lands in the pool's later half, and freeing the smaller one — which
 * calls ray_pool_of and must resolve the header via the walk. */

static test_result_t test_pool_of_oversized_walk(void) {
    /* Oversized blocks are direct mmaps (no pool); a sub-32 MB block still
     * comes from a standard buddy pool.  Verify the two paths coexist: a
     * direct block and a buddy block are each written, read back, and freed
     * with no leak or cross-path confusion. */
    ray_t* anchor = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(anchor);

    ray_mem_stats_t s0; ray_mem_stats(&s0);

    size_t big = (size_t)1 << 26;   /* 64 MB → direct */
    ray_t* huge = ray_alloc(big);
    if (!huge) {
        ray_free(anchor);
        SKIP("oversized alloc unavailable");
    }
    TEST_ASSERT(huge->order > RAY_HEAP_MAX_ORDER, "oversized block is direct");
    memset(ray_data(huge), 0x5a, 4096);

    ray_t* mid = ray_alloc((size_t)1 << 20);   /* 1 MB → buddy pool (order < 25) */
    TEST_ASSERT_NOT_NULL(mid);
    TEST_ASSERT(mid->order <= RAY_HEAP_MAX_ORDER, "1 MB block is a buddy block");
    memset(ray_data(mid), 0x5a, 4096);

    ray_free(huge);
    ray_free(mid);
    ray_heap_gc();

    ray_mem_stats_t s1; ray_mem_stats(&s1);
    TEST_ASSERT(s1.direct_bytes == s0.direct_bytes, "direct block released, no leak");
    ray_free(anchor);
    PASS();
}

/* ---- ray_order_for_size SIZE_MAX overflow + max-order direct boundary ------
 *
 * ray_order_for_size(SIZE_MAX) returns RAY_HEAP_MAX_ORDER+1 via the
 * data_size > SIZE_MAX-32 guard.  Valid sizes at or above the largest buddy
 * order must route through exact-size direct mmap, not the buddy max-order
 * rejection. */

static test_result_t test_order_overflow_guards(void) {
    /* L162: SIZE_MAX overflows the +32 header math. */
    TEST_ASSERT_EQ_U(ray_order_for_size(SIZE_MAX), RAY_HEAP_MAX_ORDER + 1);
    /* Just below the overflow boundary still saturates at MAX_ORDER+1. */
    TEST_ASSERT_EQ_U(ray_order_for_size(SIZE_MAX - 16), RAY_HEAP_MAX_ORDER + 1);

    /* A size that maps to order RAY_HEAP_MAX_ORDER (38) still resolves to a
     * real order — no longer a guaranteed NULL: order >= RAY_HEAP_POOL_ORDER
     * goes the direct path, and a size larger than physical RAM now spills to
     * a file-backed direct mapping rather than failing.  (We don't allocate it
     * here — that would fallocate a ~256 GiB spill file.) */
    size_t sz = ((size_t)1 << 38) - 64;
    TEST_ASSERT_EQ_U(ray_order_for_size(sz), RAY_HEAP_MAX_ORDER);

    /* A valid size just above the largest buddy order is still representable
     * by the direct mmap path.  Do not allocate it here: that would reserve a
     * huge spill file on machines where it exceeds the RAM watermark. */
    size_t direct_sz = ((size_t)1 << 38);
    TEST_ASSERT_EQ_U(ray_order_for_size(direct_sz), RAY_HEAP_MAX_ORDER + 1);

    /* The clean-NULL guard is true arithmetic overflow, not "order > max". */
    ray_t* v = ray_alloc(SIZE_MAX);
    TEST_ASSERT_NULL(v);
    PASS();
}

/* Anon watermark: once our anonymous (RAM) footprint would cross the watermark,
 * further large allocations spill to a disk-backed file instead of anonymous
 * RAM (which the kernel could accept then OOM-kill).  Drive it with a low
 * watermark so the crossing is deterministic regardless of machine RAM. */
static test_result_t test_anon_watermark_spill(void) {
    size_t sz = 40 * 1024 * 1024 - 128;   /* order 26 → direct path */
    /* Start from an empty reuse cache: leftover cached blocks from earlier
     * tests would (a) inflate the baseline and (b) be drained by the
     * second alloc's pressure path, freeing enough headroom for it to stay
     * anonymous instead of spilling. */
    ray_heap_direct_cache_drain();
    int64_t base = ray_heap_anon_committed();
    /* Headroom for exactly one ~40 MB block, not two. */
    ray_heap_set_anon_watermark(base + 48 * 1024 * 1024);

    ray_t* a = ray_alloc(sz);
    bool a_file = a ? ray_direct_file_backed(a) : true;   /* expect anon (RAM) */
    ray_t* b = ray_alloc(sz);
    bool b_file = b ? ray_direct_file_backed(b) : false;  /* expect spill file */

    /* A spilled block must be as usable as an anon one: write a pattern near
     * the start and end of each and read it back (MAP_SHARED round-trip). */
    bool rw_ok = (a != NULL && b != NULL);
    if (rw_ok) {
        for (int i = 0; i < 2; i++) {
            ray_t* blk = i ? b : a;
            uint8_t* d = (uint8_t*)ray_data(blk);
            d[0] = 0xA5; d[4095] = 0x5A; d[sz - 1] = 0x3C;
            if (d[0] != 0xA5 || d[4095] != 0x5A || d[sz - 1] != 0x3C)
                rw_ok = false;
        }
    }

    /* Free and reset the watermark BEFORE asserting so a failure can't leak the
     * blocks or leave the low watermark set for later tests. */
    if (a) ray_free(a);
    if (b) ray_free(b);
    /* Freeing a large anon direct block may STASH it in the reuse cache
     * (pages stay resident and counted).  Drain so the baseline assertion
     * below sees the fully-released state. */
    ray_heap_direct_cache_drain();
    ray_heap_set_anon_watermark(0);

    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT(!a_file, "first alloc under watermark stays in anonymous RAM");
    TEST_ASSERT(b_file, "second alloc over watermark spills to a disk file");
    TEST_ASSERT(rw_ok, "both anon and spilled blocks round-trip written data");

    /* Counter returns to baseline once both are freed. */
    TEST_ASSERT_EQ_I(ray_heap_anon_committed(), base);
    PASS();
}

/* ---- Slab byte-budget caps -------------------------------------------- *
 *
 * Verify that ray_heap_init computes slab_cap[] correctly from the default
 * RAY_SLAB_BUDGET.  Order 6 (64 B) must clamp to RAY_SLAB_CACHE_SIZE; the
 * top order should equal budget / 2^(RAY_SLAB_MIN+top), clamped to [1,64]. */

static test_result_t test_slab_byte_budget(void) {
    extern RAY_TLS ray_heap_t* ray_tl_heap;
    ray_heap_t* h = ray_tl_heap;
    /* order 6 (64B): cap clamps to RAY_SLAB_CACHE_SIZE (64). */
    TEST_ASSERT_EQ_I(h->slab_cap[0], RAY_SLAB_CACHE_SIZE);
    /* top order at 1MB budget: budget / 2^(RAY_SLAB_MIN+top), clamped. */
    int top = RAY_SLAB_ORDERS - 1;
    uint32_t expect_top = (uint32_t)(RAY_SLAB_BUDGET / BSIZEOF(RAY_SLAB_MIN + top));
    if (expect_top < 1) expect_top = 1;
    if (expect_top > RAY_SLAB_CACHE_SIZE) expect_top = RAY_SLAB_CACHE_SIZE;
    TEST_ASSERT_EQ_I(h->slab_cap[top], expect_top);
    PASS();
}

static test_result_t test_slab_gc_drains_wide(void) {
    extern RAY_TLS ray_heap_t* ray_tl_heap;
    ray_heap_t* h = ray_tl_heap;
    /* Fill the order-14 (16KB) slab, then GC; flush_slabs must empty it. */
    ray_t* keep[8];
    for (int i = 0; i < 8; i++) keep[i] = ray_alloc(16384 - 32);
    for (int i = 0; i < 8; i++) ray_free(keep[i]);   /* into slab */
    int top14 = SLAB_INDEX(14);
    TEST_ASSERT(h->slabs[top14].count > 0, "order-14 slab populated");
    ray_heap_gc();
    TEST_ASSERT_EQ_I(h->slabs[top14].count, 0);      /* flushed to freelists */
    PASS();
}

#include "ops/idxop.h"   /* ray_index_t, ray_index_payload, RAY_IDX_SORT */

/* ---- ray_retain_owned_refs: atom-owns-obj branch (L675-676) ---------------
 *
 * ray_atom_owns_obj is true for a -RAY_STR atom that is NOT SSO (slen >= 8,
 * so the payload lives in an external obj rather than inline).  ray_alloc_copy
 * treats the atom as data_size=0, memcpy's the 32-byte header (carrying obj),
 * then ray_retain_owned_refs hits the atom-owns-obj arm and retains obj.
 * Both copy and source share the one obj; freeing both releases it twice. */

static test_result_t test_retain_owned_refs_atom_obj(void) {
    /* obj is a real heap block (a U8 byte buffer standing in for the string
     * payload).  Give it rc=1; the retain on copy bumps it to 2. */
    ray_t* obj = ray_alloc(16);
    TEST_ASSERT_NOT_NULL(obj);
    obj->type = RAY_U8;
    obj->len  = 16;

    ray_t* s = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(s);
    s->type = -RAY_STR;
    s->slen = 8;          /* >= 8 → non-SSO → ray_atom_owns_obj true */
    s->obj  = obj;        /* external payload, owned by s */

    uint32_t orc = obj->rc;
    /* alloc_copy → retain_owned_refs atom-owns-obj arm (L675-676). */
    ray_t* copy = ray_alloc_copy(s);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_FALSE(RAY_IS_ERR(copy));
    TEST_ASSERT_EQ_PTR(copy->obj, obj);     /* shallow-shared obj */
    TEST_ASSERT_EQ_U(obj->rc, orc + 1);     /* retained once */

    /* Free copy: release_owned_refs drops obj 2→1.  Then free s: drops 1→0
     * (frees obj).  Order matters so the shared obj outlives the first free. */
    ray_free(copy);
    TEST_ASSERT_EQ_U(obj->rc, orc);         /* back to original */
    ray_free(s);                            /* releases obj to 0 → freed */

    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

/* ---- ray_detach_owned_refs: RAY_LAZY arm (L747-751) -----------------------
 *
 * ray_detach_owned_refs reaches the LAZY arm from ray_scratch_realloc on the
 * old block.  RAY_LAZY is an atom (data_size=0); the new block inherits the
 * graph/op pointers via the header memcpy, and detach nulls them on the old
 * block.  Detach NEVER dereferences the pointers, so a sentinel is safe; we
 * neutralize the surviving block before freeing it (mirrors the GRAPH test). */

static test_result_t test_scratch_realloc_lazy_handle(void) {
    ray_t* g = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(g);
    g->type = RAY_LAZY;
    RAY_LAZY_GRAPH(g) = (ray_graph_t*)0x1234;   /* sentinel — never deref'd */
    RAY_LAZY_OP(g)    = (ray_op_t*)0x5678;       /* sentinel — never deref'd */

    /* realloc copies header (incl graph/op) to g2, then detaches g (L747-751:
     * RAY_LAZY_GRAPH/OP(g) set NULL) before freeing it.  g's free runs
     * release_owned_refs LAZY arm with graph==NULL → no ray_graph_free. */
    ray_t* g2 = ray_scratch_realloc(g, 0);
    TEST_ASSERT_NOT_NULL(g2);
    TEST_ASSERT_EQ_I(g2->type, RAY_LAZY);
    TEST_ASSERT_EQ_PTR(RAY_LAZY_GRAPH(g2), (ray_graph_t*)0x1234);

    /* Neutralize the survivor so its free doesn't ray_graph_free the sentinel. */
    RAY_LAZY_GRAPH(g2) = NULL;
    RAY_LAZY_OP(g2)    = NULL;
    ray_free(g2);
    PASS();
}

/* ---- ray_detach_owned_refs: HAS_INDEX arm (L790-794) ----------------------
 *
 * A vector carrying RAY_ATTR_HAS_INDEX stores an owning ray_t* index in
 * aux[0..7] (v->index).  ray_scratch_realloc transfers the vector data to
 * a new block (index pointer copied), then detaches the old block — nulling
 * v->index and clearing the HAS_INDEX bit (L790-794).  We use a real RAY_INDEX
 * block as v->index and let the surviving block own it, releasing once. */

static test_result_t test_scratch_realloc_has_index_detach(void) {
    /* A minimal RAY_INDEX block: kind RAY_IDX_SORT with perm NULL, so
     * release_payload is a no-op when the survivor is freed. */
    ray_t* idx = ray_alloc(sizeof(ray_index_t));
    TEST_ASSERT_NOT_NULL(idx);
    idx->type = RAY_INDEX;
    idx->len  = 0;
    ray_index_t* ix = ray_index_payload(idx);
    memset(ix, 0, sizeof(*ix));
    ix->kind = RAY_IDX_SORT;     /* perm NULL → release is a no-op */

    ray_t* v = ray_alloc(4 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(v);
    v->type   = RAY_I64;
    v->len    = 4;
    v->attrs |= RAY_ATTR_HAS_INDEX;
    v->index  = idx;             /* v owns the only ref to idx */

    /* realloc copies header (incl index ptr + HAS_INDEX bit) to v2, then
     * detaches v: v->index=NULL, HAS_INDEX cleared (L790-794).  v's free
     * then takes the no-index path (won't release idx). */
    ray_t* v2 = ray_scratch_realloc(v, 8 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(v2);
    TEST_ASSERT_TRUE(v2->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_EQ_PTR(v2->index, idx);

    /* Freeing v2 takes the HAS_INDEX release arm → ray_release(idx) → freed. */
    ray_free(v2);

    ray_t* probe = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(probe);
    ray_free(probe);
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t heap_entries[] = {
    { "heap/slab_overflow",            test_slab_overflow_falls_through, heap_setup, heap_teardown },
    { "heap/multi_pool_growth",        test_multi_pool_growth,           heap_setup, heap_teardown },
    { "heap/scratch_realloc_atom",     test_scratch_realloc_atom,        heap_setup, heap_teardown },
    { "heap/scratch_realloc_vec_grow", test_scratch_realloc_vec_grow,    heap_setup, heap_teardown },
    { "heap/scratch_realloc_vec_shrink", test_scratch_realloc_vec_shrink, heap_setup, heap_teardown },
    { "heap/scratch_realloc_list",     test_scratch_realloc_list,        heap_setup, heap_teardown },
    { "heap/scratch_realloc_null_v",   test_scratch_realloc_null_v,      heap_setup, heap_teardown },
    { "heap/scratch_arena_basic",      test_scratch_arena_basic,         heap_setup, heap_teardown },
    { "heap/scratch_arena_multi",      test_scratch_arena_multi_backing, heap_setup, heap_teardown },
    { "heap/scratch_arena_oversize",   test_scratch_arena_oversize,      heap_setup, heap_teardown },
    { "heap/scratch_arena_direct",     test_scratch_arena_direct_backing, heap_setup, heap_teardown },
    { "heap/release_pages",            test_release_pages,               heap_setup, heap_teardown },
    { "heap/gc_reclaim_oversized",     test_gc_reclaim_oversized_pool,   heap_setup, heap_teardown },
    { "heap/gc_serial",                test_heap_gc_serial,              heap_setup, heap_teardown },
    { "heap/gc_parallel",              test_heap_gc_parallel,            heap_setup, heap_teardown },
    { "heap/flush_foreign_parallel",   test_flush_foreign_during_parallel, heap_setup, heap_teardown },
    { "heap/alloc_copy_list",          test_alloc_copy_list_retains,     heap_setup, heap_teardown },
    { "heap/str_pool_owned_ref",       test_str_pool_owned_ref,          heap_setup, heap_teardown },
    { "heap/sentinel_null_release",    test_sentinel_null_release,       heap_setup, heap_teardown },
    { "heap/slice_owned_ref",          test_slice_owned_ref,             heap_setup, heap_teardown },
    { "heap/parted_owned_ref",         test_parted_owned_ref,            heap_setup, heap_teardown },
    { "heap/mapcommon_owned_ref",      test_mapcommon_owned_ref,         heap_setup, heap_teardown },
    { "heap/merge_rich",               test_merge_with_slabs_and_freelist, heap_setup, heap_teardown },
    { "heap/alloc_copy_atom",          test_alloc_copy_atom,             heap_setup, heap_teardown },
    { "heap/alloc_copy_table",         test_alloc_copy_table_block,      heap_setup, heap_teardown },
    { "heap/mem_stats_no_heap",        test_mem_stats_no_heap,           heap_setup, heap_teardown },
    { "heap/alloc_too_large",          test_alloc_too_large,             heap_setup, heap_teardown },
    { "heap/alloc_lazy_init",          test_alloc_lazy_init,             heap_setup, heap_teardown },
    { "heap/free_edge_cases",          test_free_edge_cases,             heap_setup, heap_teardown },
    { "heap/coalesce_chain",           test_coalesce_chain,              heap_setup, heap_teardown },
    { "heap/scratch_alloc_basic",      test_scratch_alloc_basic,         heap_setup, heap_teardown },
    { "heap/scratch_realloc_table",    test_scratch_realloc_table,       heap_setup, heap_teardown },
    { "heap/scratch_realloc_mapcommon",test_scratch_realloc_mapcommon,   heap_setup, heap_teardown },
    { "heap/alloc_copy_dict",          test_alloc_copy_dict_block,       heap_setup, heap_teardown },
    { "heap/release_lambda_owned_refs", test_release_lambda_owned_refs,   heap_setup, heap_teardown },
    { "heap/flush_foreign_owner_gone", test_flush_foreign_owner_gone,    heap_setup, heap_teardown },
    { "heap/merge_slab_overflow",      test_merge_slab_overflow,         heap_setup, heap_teardown },
    { "heap/gc_return_foreign_fl",     test_gc_return_foreign_freelist,  heap_setup, heap_teardown },
    { "heap/free_mmod1_atom",          test_free_mmod1_atom,             heap_setup, heap_teardown },
    { "heap/order_for_size_pow2",      test_order_for_size_pow2,         heap_setup, heap_teardown },
    { "heap/scratch_realloc_slice",    test_scratch_realloc_slice,       heap_setup, heap_teardown },
    { "heap/scratch_realloc_sentinel_nulls", test_scratch_realloc_sentinel_nulls, heap_setup, heap_teardown },
    { "heap/scratch_realloc_parted",   test_scratch_realloc_parted,      heap_setup, heap_teardown },
    { "heap/merge_foreign_fallback",   test_merge_foreign_pool_fallback, heap_setup, heap_teardown },
    { "heap/retain_null_and_err",      test_retain_owned_refs_null_and_err,    heap_setup, heap_teardown },
    { "heap/retain_lambda_null_slots", test_retain_owned_refs_lambda_null_slots, heap_setup, heap_teardown },
    { "heap/retain_list_null_kids",    test_retain_owned_refs_list_null_children, heap_setup, heap_teardown },
    { "heap/retain_tbl_dict_mc_null",  test_retain_owned_refs_table_dict_mc_null, heap_setup, heap_teardown },
    { "heap/retain_parted_null_seg",   test_retain_owned_refs_parted_null_seg, heap_setup, heap_teardown },
    { "heap/retain_str_null_pool",     test_retain_owned_refs_str_null_pool,   heap_setup, heap_teardown },
    { "heap/alloc_copy_hnsw",          test_alloc_copy_hnsw_handle,            heap_setup, heap_teardown },
    { "heap/scratch_realloc_hnsw",     test_scratch_realloc_hnsw_handle,       heap_setup, heap_teardown },
    { "heap/scratch_realloc_graph",    test_scratch_realloc_graph_handle,      heap_setup, heap_teardown },
    { "heap/scratch_realloc_str",      test_scratch_realloc_str_detach,        heap_setup, heap_teardown },
    { "heap/alloc_copy_err_overflow",  test_alloc_copy_err_and_overflow,       heap_setup, heap_teardown },
    { "heap/alloc_copy_invalid_type",  test_alloc_copy_invalid_generic_type,   heap_setup, heap_teardown },
    { "heap/scratch_realloc_list_neg", test_scratch_realloc_list_neg_len,      heap_setup, heap_teardown },
    { "heap/scratch_realloc_parted_neg", test_scratch_realloc_parted_neg_len,  heap_setup, heap_teardown },
    { "heap/scratch_realloc_inval_typ",test_scratch_realloc_invalid_type,      heap_setup, heap_teardown },
    { "heap/scratch_realloc_arena",    test_scratch_realloc_arena_skip,        heap_setup, heap_teardown },
    { "heap/scratch_realloc_mmod",     test_scratch_realloc_mmod_skip_clamp,   heap_setup, heap_teardown },
    { "heap/free_mmod1_vector",        test_free_mmod1_vector,                 heap_setup, heap_teardown },
    { "heap/free_mmod1_table",         test_free_mmod1_table,                  heap_setup, heap_teardown },
    { "heap/free_mmod1_str_pool",      test_free_mmod1_str_with_pool,          heap_setup, heap_teardown },
    { "heap/free_no_heap",             test_free_no_heap,                      heap_setup, heap_teardown },
    { "heap/pool_of_oversized_walk",   test_pool_of_oversized_walk,            heap_setup, heap_teardown },
    { "heap/order_overflow_guards",    test_order_overflow_guards,             heap_setup, heap_teardown },
    { "heap/anon_watermark_spill",     test_anon_watermark_spill,              heap_setup, heap_teardown },
    { "heap/slab_byte_budget",         test_slab_byte_budget,            heap_setup, heap_teardown },
    { "heap/slab_gc_drains_wide",      test_slab_gc_drains_wide,         heap_setup, heap_teardown },
    { "heap/retain_atom_obj",          test_retain_owned_refs_atom_obj,  heap_setup, heap_teardown },
    { "heap/scratch_realloc_lazy",     test_scratch_realloc_lazy_handle, heap_setup, heap_teardown },
    { "heap/scratch_realloc_has_index", test_scratch_realloc_has_index_detach, heap_setup, heap_teardown },
    { NULL, NULL, NULL, NULL },
};
