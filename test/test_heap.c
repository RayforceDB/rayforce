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
 * (LIST / TABLE / DICT / parted / NULLMAP_EXT / SLICE / STR with str_pool).
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

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
    /* First, force a standard pool into existence so pool_count >= 2 even
     * after the oversized pool is reclaimed (the GC skips reclamation
     * when pool_count <= 1). */
    ray_t* small = ray_alloc(64);
    TEST_ASSERT_NOT_NULL(small);

    /* Now allocate a block that needs more than 32 MB — pool_order will
     * be order+1 of the request.  RAY_HEAP_POOL_ORDER is 25 (32 MB), so
     * a request of (1<<26) - 32 = 64 MB - 32 lands in order 26 and
     * triggers a pool_order 27 (128 MB) pool. */
    size_t big = (size_t)1 << 26;  /* 64 MB */
    ray_t* huge = ray_alloc(big);
    if (!huge) {
        /* Environment can't satisfy a 128 MB anon mmap.  Skip rather
         * than fail — this is environmental, not a code defect. */
        ray_free(small);
        SKIP("oversized pool alloc unavailable");
    }
    /* Verify it really did get an oversized pool. */
    TEST_ASSERT((ray_tl_heap->pool_count) >= (2), "pool_count >= 2");

    /* Release the huge block — its pool becomes empty.  GC reclaims it. */
    ray_free(huge);

    uint32_t pool_count_before = ray_tl_heap->pool_count;
    ray_heap_gc();

    /* Oversized pool must have been munmapped. */
    TEST_ASSERT((ray_tl_heap->pool_count) < (pool_count_before), "pool_count decreased");

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
    ray_free(blk);  /* enqueues onto heap_a->foreign */
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

/* ---- Owned-ref: NULLMAP_EXT child -------------------------------------- *
 *
 * A vec with RAY_ATTR_NULLMAP_EXT carries an owning ref to ext_nullmap.
 * ray_release_owned_refs must release that child.  Construct one
 * manually and free it. */

static test_result_t test_nullmap_ext_owned_ref(void) {
    ray_t* vec = ray_alloc(8 * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(vec);
    vec->type = RAY_I64;
    vec->len  = 8;

    ray_t* nm = ray_alloc(8);
    TEST_ASSERT_NOT_NULL(nm);
    nm->type = RAY_U8;
    nm->len  = 8;

    /* Attach extended nullmap.  vec now owns nm. */
    vec->ext_nullmap = nm;
    vec->attrs |= RAY_ATTR_NULLMAP_EXT;

    /* Drop vec — nm must be released as well via the NULLMAP_EXT branch. */
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

    uint32_t parent_rc = parent->rc;

    /* Copy the slice — ray_retain_owned_refs bumps parent->rc. */
    ray_t* copy = ray_alloc_copy(slice);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_TRUE(copy->attrs & RAY_ATTR_SLICE);
    TEST_ASSERT_EQ_PTR(copy->slice_parent, parent);
    TEST_ASSERT_EQ_U(parent->rc, parent_rc + 1);

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
    /* mc takes the only refs; we already own one each. */

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
    ray_free(fblk);  /* heap_b->foreign now contains fblk */
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
    /* tbl owns one ref each — we already have one ref each from alloc. */

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

/* ---- ray_alloc with order overflow ------------------------------------- *
 *
 * Asking for more bytes than RAY_HEAP_MAX_ORDER (256 GB) returns NULL
 * cleanly — exercises the overflow guard branch. */

static test_result_t test_alloc_too_large(void) {
    /* SIZE_MAX/2 will trigger ray_order_for_size > MAX_ORDER. */
    size_t huge = (size_t)1 << 40;  /* 1 TB */
    ray_t* v = ray_alloc(huge);
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
    { "heap/release_pages",            test_release_pages,               heap_setup, heap_teardown },
    { "heap/gc_reclaim_oversized",     test_gc_reclaim_oversized_pool,   heap_setup, heap_teardown },
    { "heap/gc_serial",                test_heap_gc_serial,              heap_setup, heap_teardown },
    { "heap/gc_parallel",              test_heap_gc_parallel,            heap_setup, heap_teardown },
    { "heap/flush_foreign_parallel",   test_flush_foreign_during_parallel, heap_setup, heap_teardown },
    { "heap/alloc_copy_list",          test_alloc_copy_list_retains,     heap_setup, heap_teardown },
    { "heap/str_pool_owned_ref",       test_str_pool_owned_ref,          heap_setup, heap_teardown },
    { "heap/nullmap_ext_owned_ref",    test_nullmap_ext_owned_ref,       heap_setup, heap_teardown },
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
    { NULL, NULL, NULL, NULL },
};
