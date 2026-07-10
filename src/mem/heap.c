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

#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#  define _GNU_SOURCE       /* ftruncate, MAP_SHARED, etc. */
#endif

#include "heap.h"
#include "cow.h"
#include "sys.h"
#include "core/platform.h"
#include "table/sym.h"
#include "table/domain.h"
#include "lang/eval.h"
#include "store/hnsw.h"
#include "store/csr.h"
#include "ops/idxop.h"
#include <string.h>
#include <stdlib.h>     /* getenv */
#include <stdio.h>      /* snprintf */
#include <unistd.h>     /* getpid, close, ftruncate, unlink */
#include <fcntl.h>      /* open, fcntl, F_PREALLOCATE on macOS */
#include <errno.h>
#include <sys/mman.h>   /* mmap, munmap */
#include <sys/stat.h>   /* O_*  modes */
#include <sys/types.h>
#include <stdatomic.h>

#ifdef DEBUG
/* =====================================================================
 * Debug-only stale-pointer detector (issue #240 investigation).
 *
 * Shadow set of block addresses that are currently FREE (sitting on a
 * freelist, in a slab cache, or queued on a foreign list).  Any
 * ray_free / ray_release / ray_retain that touches an address in this
 * set is a use-after-free; report with a backtrace and abort before
 * the allocator state is corrupted.
 *
 * Opt-in: set RAY_DFD=1 (debug builds only).  ASan cannot see
 * use-after-free inside the pool allocator, so this is the tool of
 * choice for chasing double releases (found while debugging #240).
 * RAY_DFD_NO_ABORT=1 reports without aborting.
 * ===================================================================== */
#include <execinfo.h>

#define DFD_CAP_BITS 20
#define DFD_CAP      (1u << DFD_CAP_BITS)
#define DFD_TOMB     ((const void*)(uintptr_t)1)

static const void*  dfd_slots[DFD_CAP];
static atomic_flag  dfd_lock_flag = ATOMIC_FLAG_INIT;

static bool dfd_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("RAY_DFD");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on == 1;
}

static void dfd_lock(void)   { while (atomic_flag_test_and_set_explicit(&dfd_lock_flag, memory_order_acquire)) {} }
static void dfd_unlock(void) { atomic_flag_clear_explicit(&dfd_lock_flag, memory_order_release); }

static inline uint32_t dfd_hash(const void* p) {
    uint64_t x = (uint64_t)(uintptr_t)p >> 5;
    x *= 0x9E3779B97F4A7C15ull;
    return (uint32_t)(x >> (64 - DFD_CAP_BITS));
}

static void dfd_add(const void* p) {
    if (!dfd_enabled()) return;
    dfd_lock();
    uint32_t i = dfd_hash(p);
    uint32_t first_tomb = UINT32_MAX;
    for (uint32_t n = 0; n < DFD_CAP; n++, i = (i + 1) & (DFD_CAP - 1)) {
        if (dfd_slots[i] == p) { dfd_unlock(); return; }       /* already present */
        if (dfd_slots[i] == DFD_TOMB && first_tomb == UINT32_MAX) first_tomb = i;
        if (dfd_slots[i] == NULL) {
            dfd_slots[first_tomb != UINT32_MAX ? first_tomb : i] = p;
            dfd_unlock();
            return;
        }
    }
    dfd_unlock();  /* table full — detector degrades, never blocks */
}

static void dfd_remove(const void* p) {
    if (!dfd_enabled()) return;
    dfd_lock();
    uint32_t i = dfd_hash(p);
    for (uint32_t n = 0; n < DFD_CAP; n++, i = (i + 1) & (DFD_CAP - 1)) {
        if (dfd_slots[i] == p) { dfd_slots[i] = DFD_TOMB; break; }
        if (dfd_slots[i] == NULL) break;
    }
    dfd_unlock();
}

static bool dfd_contains(const void* p) {
    if (!dfd_enabled()) return false;
    dfd_lock();
    uint32_t i = dfd_hash(p);
    bool found = false;
    for (uint32_t n = 0; n < DFD_CAP; n++, i = (i + 1) & (DFD_CAP - 1)) {
        if (dfd_slots[i] == p) { found = true; break; }
        if (dfd_slots[i] == NULL) break;
    }
    dfd_unlock();
    return found;
}

/* Forget every tracked address inside [lo, hi).  Must run whenever a
 * pool is munmapped (heap destroy, oversized-pool reclaim): tests
 * destroy and recreate heaps, and a new pool mapped at a recycled
 * address would otherwise alias stale "free" entries and report
 * phantom stale-retains on legitimately live blocks. */
static void dfd_purge_range(uintptr_t lo, uintptr_t hi) {
    if (!dfd_enabled()) return;
    dfd_lock();
    for (uint32_t i = 0; i < DFD_CAP; i++) {
        uintptr_t p = (uintptr_t)dfd_slots[i];
        if (p > (uintptr_t)DFD_TOMB && p >= lo && p < hi)
            dfd_slots[i] = DFD_TOMB;
    }
    dfd_unlock();
}

static void dfd_report(const char* who, const void* p) {
    void* frames[64];
    int n = backtrace(frames, 64);
    fprintf(stderr, "\n=== DFD: %s on FREED block %p ===\n", who, p);
    backtrace_symbols_fd(frames, n, 2);
    fflush(stderr);
    if (!getenv("RAY_DFD_NO_ABORT")) abort();
}

/* Called from cow.c (ray_release / ray_retain). */
void ray_dfd_check_live(const void* p, const char* who);
void ray_dfd_check_live(const void* p, const char* who) {
    if (p && dfd_contains(p)) dfd_report(who, p);
}

/* Walk every freelist of every registered heap; abort on NULL links or
 * cycles.  Called at GC entry so corruption is caught at the same point
 * the release build crashes (issue #240), but deterministically. */
static void dfd_validate_freelists(void);
#else
#define dfd_add(p)              ((void)0)
#define dfd_remove(p)           ((void)0)
#define dfd_purge_range(lo, hi) ((void)0)
#endif

/* Portable disk-block preallocation.  Returns 0 on success, errno-style
 * code on failure (matching posix_fallocate's contract).  Linux has
 * posix_fallocate natively.  macOS uses fcntl(F_PREALLOCATE) — try
 * contiguous first, fall back to non-contiguous, then ftruncate to
 * extend the file size if needed (F_PREALLOCATE doesn't grow the file
 * beyond its current size). */
static int heap_preallocate(int fd, off_t offset, off_t len) {
#if defined(__APPLE__)
    fstore_t fs = {
        .fst_flags    = F_ALLOCATECONTIG | F_ALLOCATEALL,
        .fst_posmode  = F_PEOFPOSMODE,
        .fst_offset   = 0,
        .fst_length   = offset + len,
        .fst_bytesalloc = 0,
    };
    if (fcntl(fd, F_PREALLOCATE, &fs) == -1) {
        /* Retry without contiguous-only constraint. */
        fs.fst_flags = F_ALLOCATEALL;
        if (fcntl(fd, F_PREALLOCATE, &fs) == -1) return errno ? errno : -1;
    }
    /* F_PREALLOCATE reserves blocks but doesn't grow the logical file
     * size — extend with ftruncate so mmap'd pages past the old size
     * can actually be written without SIGBUS. */
    if (ftruncate(fd, offset + len) != 0) return errno ? errno : -1;
    return 0;
#else
    return posix_fallocate(fd, offset, len);
#endif
}

/* --------------------------------------------------------------------------
 * Static asserts
 * -------------------------------------------------------------------------- */
_Static_assert(sizeof(ray_pool_hdr_t) <= 16,
               "ray_pool_hdr_t must fit in aux (16 bytes)");

/* --------------------------------------------------------------------------
 * Thread-local state
 * -------------------------------------------------------------------------- */
RAY_TLS ray_heap_t*     ray_tl_heap  = NULL;

/* Stats tracking — gated by RAY_MEM_STATS (1 in DEBUG builds, 0 in release).
 * All stats go through the per-heap struct (ray_tl_heap->stats) so that
 * heap merges keep bytes_allocated accurate.
 *
 * bytes_allocated is only modified by the owning thread (alloc/local-free)
 * or by the main thread during GC flush (return_to_owner=true, workers idle).
 * No atomics needed. */
#if RAY_MEM_STATS
#  define RAY_STAT(x) (x)
#else
#  define RAY_STAT(x) ((void)0)
#endif

/* --------------------------------------------------------------------------
 * Bitmap-based heap ID allocator (atomic CAS, reusable IDs)
 *
 * Each bit in the bitmap represents one heap ID. Acquiring sets a bit,
 * releasing clears it. IDs are reused after release (unlike a monotonic
 * counter). Cursor rotates to spread contention across words.
 * -------------------------------------------------------------------------- */
static _Atomic(uint64_t) g_heap_id_bitmap[RAY_HEAP_ID_WORDS] = { [0] = 1ULL };
static _Atomic(uint64_t) g_heap_id_cursor = 0;

/* Direct large-allocation counters (process-wide, exact).  Direct blocks are
 * standalone mmaps not owned by any per-thread heap, so their bytes live here
 * rather than in a per-thread stats slot; ray_mem_stats surfaces them. */
static _Atomic(int64_t) g_direct_bytes = 0;
static _Atomic(int64_t) g_direct_count = 0;

/* One process-wide, explicitly-scoped allocation trace for `.mem.ts`.
 * The inactive hot-path cost is one relaxed load plus a predicted-not-taken
 * branch in ray_alloc/ray_free.  While active, atomics make allocations from
 * the main and every worker heap contribute to the same query measurement. */
static _Atomic(uint32_t) g_mem_trace_active = 0;
static _Atomic(uint64_t) g_mem_trace_alloc_count = 0;
static _Atomic(uint64_t) g_mem_trace_free_count = 0;
static _Atomic(uint64_t) g_mem_trace_allocated = 0;
static _Atomic(uint64_t) g_mem_trace_freed = 0;
static _Atomic(int64_t)  g_mem_trace_live = 0;
static _Atomic(uint64_t) g_mem_trace_peak = 0;

static inline void mem_trace_note_alloc(size_t bytes) {
    if (RAY_LIKELY(atomic_load_explicit(&g_mem_trace_active,
                                        memory_order_relaxed) != 1)) return;
    atomic_fetch_add_explicit(&g_mem_trace_alloc_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_mem_trace_allocated, (uint64_t)bytes,
                              memory_order_relaxed);
    int64_t live = atomic_fetch_add_explicit(&g_mem_trace_live, (int64_t)bytes,
                                             memory_order_relaxed) + (int64_t)bytes;
    uint64_t candidate = live > 0 ? (uint64_t)live : 0;
    uint64_t peak = atomic_load_explicit(&g_mem_trace_peak, memory_order_relaxed);
    while (candidate > peak &&
           !atomic_compare_exchange_weak_explicit(&g_mem_trace_peak, &peak,
                                                   candidate,
                                                   memory_order_relaxed,
                                                   memory_order_relaxed)) {}
}

static inline void mem_trace_note_free(size_t bytes) {
    if (RAY_LIKELY(atomic_load_explicit(&g_mem_trace_active,
                                        memory_order_relaxed) != 1)) return;
    atomic_fetch_add_explicit(&g_mem_trace_free_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_mem_trace_freed, (uint64_t)bytes,
                              memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_mem_trace_live, (int64_t)bytes,
                              memory_order_relaxed);
}

bool ray_mem_trace_begin(void) {
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_mem_trace_active, &expected, 2,
                                                  memory_order_acq_rel,
                                                  memory_order_relaxed))
        return false;
    atomic_store_explicit(&g_mem_trace_alloc_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mem_trace_free_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mem_trace_allocated, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mem_trace_freed, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mem_trace_live, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mem_trace_peak, 0, memory_order_relaxed);
    atomic_store_explicit(&g_mem_trace_active, 1, memory_order_release);
    return true;
}

void ray_mem_trace_end(ray_mem_trace_t* out) {
    /* Freeze first: state 2 makes allocation hooks ignore the scope while
     * preventing another caller from resetting the counters mid-snapshot. */
    atomic_store_explicit(&g_mem_trace_active, 2, memory_order_release);
    if (out) {
        out->alloc_count = atomic_load_explicit(&g_mem_trace_alloc_count,
                                                memory_order_relaxed);
        out->free_count = atomic_load_explicit(&g_mem_trace_free_count,
                                               memory_order_relaxed);
        out->allocated_bytes = atomic_load_explicit(&g_mem_trace_allocated,
                                                    memory_order_relaxed);
        out->freed_bytes = atomic_load_explicit(&g_mem_trace_freed,
                                                memory_order_relaxed);
        out->net_bytes = atomic_load_explicit(&g_mem_trace_live,
                                              memory_order_relaxed);
        out->peak_live_bytes = atomic_load_explicit(&g_mem_trace_peak,
                                                    memory_order_relaxed);
    }
    atomic_store_explicit(&g_mem_trace_active, 0, memory_order_release);
}

/* Anonymous (RAM-resident) pool + direct bytes we have committed.  File-backed
 * spill mappings are NOT counted (they can be evicted to disk, never OOM-kill).
 * When a new anon mapping would push this past the watermark, it is routed to a
 * spill file instead — this never rejects work, it just picks disk over RAM. */
static _Atomic(int64_t) g_anon_committed = 0;
static _Atomic(int64_t) g_anon_watermark = 0;   /* 0 = default to physical RAM */
static _Atomic(int64_t) g_anon_peak      = 0;   /* high-water of g_anon_committed */

/* Commit `bytes` of anonymous (RAM-resident) footprint and advance the peak
 * high-water.  Called only from the two anon-commit sites; file-backed spill
 * mappings never call this (they must not count toward the RAM watermark). */
static void heap_anon_commit(int64_t bytes) {
    int64_t nv = atomic_fetch_add_explicit(&g_anon_committed, bytes,
                                            memory_order_relaxed) + bytes;
    int64_t pk = atomic_load_explicit(&g_anon_peak, memory_order_relaxed);
    while (nv > pk &&
           !atomic_compare_exchange_weak_explicit(&g_anon_peak, &pk, nv,
                                                   memory_order_relaxed,
                                                   memory_order_relaxed)) {
        /* pk reloaded with the current value on failure; retry. */
    }
}

/* Threshold above which anon allocations spill to disk.  Default keeps our
 * anon footprint within physical RAM (swap + page cache stay as headroom). */
static int64_t heap_anon_watermark(void) {
    int64_t wm = atomic_load_explicit(&g_anon_watermark, memory_order_relaxed);
    return wm > 0 ? wm : ray_sys_total_ram();
}
/* True if committing `bytes` more anonymous RAM would cross the watermark. */
static bool heap_anon_would_exceed(size_t bytes) {
    int64_t wm = heap_anon_watermark();
    if (wm <= 0) return false;   /* unknown RAM → keep the fast anon path */
    int64_t cur = atomic_load_explicit(&g_anon_committed, memory_order_relaxed);
    return cur + (int64_t)bytes > wm;
}

int64_t ray_heap_anon_committed(void) {
    return atomic_load_explicit(&g_anon_committed, memory_order_relaxed);
}
int64_t ray_heap_anon_peak(void) {
    return atomic_load_explicit(&g_anon_peak, memory_order_relaxed);
}
int64_t ray_heap_anon_watermark(void) {
    return heap_anon_watermark();
}
void ray_heap_set_anon_watermark(int64_t bytes) {
    atomic_store_explicit(&g_anon_watermark, bytes < 0 ? 0 : bytes,
                          memory_order_relaxed);
}

ray_heap_t* ray_heap_registry[RAY_HEAP_REGISTRY_SIZE];

/* Serializes the registry slot writes (register/unregister) and the
 * cross-heap foreign-list purges that walk EVERY other heap's ->foreign
 * during teardown.  ray_pool_free() joins workers one at a time, but all
 * workers wake on the shutdown signal together and run ray_heap_destroy()
 * CONCURRENTLY — two destroyers splicing the same neighbour's singly-linked
 * ->foreign list (or one walking a slot another is NULLing) tears the list
 * and SEGVs on the next ->fl_next deref.  Spinlock matches the dfd_lock
 * pattern above; held only across the short registry/foreign critical
 * sections, never across munmap or syscalls. */
static atomic_flag ray_registry_lock_flag = ATOMIC_FLAG_INIT;
static void ray_registry_lock(void)   { while (atomic_flag_test_and_set_explicit(&ray_registry_lock_flag, memory_order_acquire)) {} }
static void ray_registry_unlock(void) { atomic_flag_clear_explicit(&ray_registry_lock_flag, memory_order_release); }

/* Pending-merge queue head (lock-free LIFO) */
_Atomic(ray_heap_t*) ray_heap_pending_merge = NULL;

static int heap_id_acquire(void) {
    uint64_t start = atomic_fetch_add_explicit(&g_heap_id_cursor, 1,
                                                memory_order_relaxed);
    for (uint64_t off = 0; off < RAY_HEAP_ID_WORDS; off++) {
        uint64_t idx = (start + off) % RAY_HEAP_ID_WORDS;
        uint64_t word = atomic_load_explicit(&g_heap_id_bitmap[idx],
                                              memory_order_relaxed);
        while (~word != 0ULL) {
            uint64_t free_bits = ~word;
            uint64_t bit = (uint64_t)__builtin_ctzll(free_bits);
            uint64_t mask = 1ULL << bit;
            uint64_t new_word = word | mask;
            if (atomic_compare_exchange_weak_explicit(
                    &g_heap_id_bitmap[idx], &word, new_word,
                    memory_order_acq_rel, memory_order_relaxed)) {
                return (int)(idx * 64 + bit);
            }
            /* CAS failed — word updated, retry with new value */
        }
    }
    return -1;  /* pool exhausted */
}

static void heap_id_release(int id) {
    if (id < 0 || id >= (int)RAY_HEAP_ID_BITS) return;
    uint64_t idx = (uint64_t)id >> 6;
    uint64_t bit = (uint64_t)id & 63ULL;
    uint64_t mask = ~(1ULL << bit);
    atomic_fetch_and_explicit(&g_heap_id_bitmap[idx], mask,
                               memory_order_release);
}

/* --------------------------------------------------------------------------
 * Parallel flag
 * -------------------------------------------------------------------------- */
_Atomic(uint32_t) ray_parallel_flag = 0;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static uint8_t ceil_log2(size_t n) {
    if (n <= 1) return 0;
    return (uint8_t)(64 - __builtin_clzll(n - 1));
}

uint8_t ray_order_for_size(size_t data_size) {
    if (data_size > SIZE_MAX - 32) return RAY_HEAP_MAX_ORDER + 1;
    size_t total = data_size + 32;  /* 32B ray_t header (no prefix) */
    uint8_t k = ceil_log2(total);
    if (k < RAY_ORDER_MIN) k = RAY_ORDER_MIN;
    return k;
}

/* --------------------------------------------------------------------------
 * Pool management
 *
 * Self-aligned pools: pool base = ptr & ~(pool_size - 1).
 * First min-block (64B at offset 0) reserved for pool header.
 * Remaining space split via cascading buddy split.
 *
 * For oversized blocks (order > POOL_ORDER), pool_order = order + 1
 * so the cascading split produces a right-half block of the needed order.
 * -------------------------------------------------------------------------- */

static bool heap_add_pool(ray_heap_t* h, uint8_t order);

/* --------------------------------------------------------------------------
 * Freelist operations (circular sentinel via fl_prev/fl_next)
 *
 * Each freelist[order] is a ray_fl_head_t sentinel. fl_remove() unlinks a
 * block from ANY circular list without needing the head pointer — enabling
 * safe cross-heap buddy coalescing.
 * -------------------------------------------------------------------------- */

RAY_INLINE void heap_insert_block(ray_heap_t* h, ray_t* blk, uint8_t order) {
    dfd_add(blk);
    ray_fl_head_t* head = &h->freelist[order];
    ray_t* first = head->fl_next;
    blk->fl_prev = (ray_t*)head;
    blk->fl_next = first;
    first->fl_prev = blk;
    head->fl_next = blk;
    ray_atomic_store(&blk->rc, 0);  /* free marker */
    blk->order = order;
    /* Free-block AGE (page-release aging, see GC pass 5).  `attrs` is dead
     * state while a block is free — splits already leave garbage here and
     * every allocation rewrites the header — so it carries the number of
     * GC passes this block has survived on a freelist.  Reset on every
     * insert (fresh free, coalesce, split). */
    blk->attrs = 0;
    h->avail |= (1ULL << order);
}

RAY_INLINE void heap_split_block(ray_heap_t* h, ray_t* blk,
                                uint8_t target_order, uint8_t block_order) {
    while (block_order > target_order) {
        block_order--;
        ray_t* buddy = (ray_t*)((char*)blk + BSIZEOF(block_order));
        buddy->mmod  = 0;
        buddy->order = block_order;
        heap_insert_block(h, buddy, block_order);
    }
}

/* --------------------------------------------------------------------------
 * Coalescing: merge block with buddies up to pool_order
 *
 * Pool header at offset 0 has rc=1 and order=RAY_ORDER_MIN, so buddy
 * checks always fail before reaching the header. Safe sentinel.
 * -------------------------------------------------------------------------- */

static void heap_coalesce(ray_heap_t* h, ray_t* blk,
                          uintptr_t pool_base, uint8_t pool_order) {
    dfd_remove(blk);
    uint8_t order = blk->order;

    /* During parallel execution, coalesce only same-pool buddies owned by
     * THIS heap.  blk is local (this path runs only for local frees), and
     * buddies stay in-pool (ray_buddy_of XORs within the pool), so a free
     * (rc==0) buddy lives only on our own freelist — which only we touch
     * during parallel.  rc!=0 excludes in-flight cross-thread frees.  The
     * heap_id guard is belt-and-suspenders against unexpected aliasing. */
    if (atomic_load_explicit(&ray_parallel_flag, memory_order_relaxed) != 0) {
        for (;; order++) {
            if (order >= pool_order) break;
            ray_t* buddy = ray_buddy_of(blk, order, pool_base);
            if (ray_atomic_load(&buddy->rc) != 0 || buddy->order != order) break;
            ray_pool_hdr_t* bphdr = ray_pool_of(buddy);
            if (!bphdr || bphdr->heap_id != h->id) break;
            fl_remove(buddy);
            dfd_remove(buddy);
            if (fl_empty(&h->freelist[order]))
                h->avail &= ~(1ULL << order);
            blk = (buddy < blk) ? buddy : blk;
        }
        heap_insert_block(h, blk, order);
        return;
    }

    for (;; order++) {
        if (order >= pool_order) break;

        ray_t* buddy = ray_buddy_of(blk, order, pool_base);
        __builtin_prefetch(buddy, 0, 1);

        uint32_t buddy_rc = ray_atomic_load(&buddy->rc);
        if (buddy_rc != 0 || buddy->order != order) break;

        fl_remove(buddy);
        if (fl_empty(&h->freelist[order]))
            h->avail &= ~(1ULL << order);

        blk = (buddy < blk) ? buddy : blk;
    }

    heap_insert_block(h, blk, order);
}

/* --------------------------------------------------------------------------
 * heap_add_pool implementation
 * -------------------------------------------------------------------------- */

static bool heap_add_pool(ray_heap_t* h, uint8_t order) {
    if (h->pool_count >= RAY_MAX_POOLS) return false;

    uint8_t pool_order;
    if (order >= RAY_HEAP_POOL_ORDER)
        pool_order = order + 1;  /* need one order larger for header + block */
    else
        pool_order = RAY_HEAP_POOL_ORDER;

    if (pool_order > RAY_HEAP_MAX_ORDER) return false;
    size_t pool_size = BSIZEOF(pool_order);

    void* mem = NULL;
    int   swap_fd  = -1;
    char* swap_path = NULL;

    /* Proactively back this pool with a disk spill file if an anon mapping
     * would push our RAM footprint past the watermark — a file-backed pool
     * can't be OOM-killed.  Otherwise take the fast anon path (and still fall
     * back to a spill file below if the kernel refuses the mapping). */
    if (!heap_anon_would_exceed(pool_size))
        mem = ray_vm_alloc_aligned(pool_size, pool_size);

    if (!mem) {
        /* Anonymous mmap refused — usually means RAM+swap can't satisfy
         * pool_size right now.  Fall back to file-backed mmap: create a
         * tempfile in h->swap_path, reserve `pool_size` bytes of disk
         * blocks (so writes won't SIGBUS later on disk-full), then map
         * the file at a self-aligned address using the anonymous-VM
         * reservation trick — no over-allocation of file or disk. */
        static _Atomic uint64_t swap_counter = 0;
        uint64_t cnt = atomic_fetch_add_explicit(&swap_counter, 1, memory_order_relaxed);

        size_t plen = strlen(h->swap_path);
        size_t need = plen + 64;  /* room for "rayheap_<pid>_<heap>_<cnt>.dat" */
        swap_path = (char*)ray_sys_alloc(need);
        if (!swap_path) return false;
        snprintf(swap_path, need, "%srayheap_%d_%u_%llu.dat",
                 h->swap_path, (int)getpid(), (unsigned)h->id,
                 (unsigned long long)cnt);

        swap_fd = open(swap_path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (swap_fd < 0) {
            ray_sys_free(swap_path);
            return false;
        }

        /* Reserve EXACTLY pool_size bytes of disk blocks AND grow the
         * file to pool_size.  Crucial that the file is empty (EOF=0)
         * when this runs: macOS F_PREALLOCATE with F_PEOFPOSMODE
         * extends past the current EOF, so doing this before any other
         * ftruncate keeps the reservation == pool_size, not 2x.  ENOSPC
         * here surfaces as a clean false return -> ray_alloc NULL ->
         * ray_error("oom") at the wrapper layer. */
        if (heap_preallocate(swap_fd, 0, (off_t)pool_size) != 0) {
            close(swap_fd);
            unlink(swap_path);
            ray_sys_free(swap_path);
            return false;
        }

        /* Reserve 2*pool_size of address space anonymously to guarantee
         * a self-aligned subrange exists.  PROT_NONE is enough — we
         * never read/write the anon mapping; it just holds the address
         * range so the kernel won't hand it out to a concurrent mmap.
         * After computing the aligned subrange, free the slack and
         * MAP_FIXED the file-backed mapping over the kept region. */
        size_t reserve_size = pool_size + pool_size;
        void* anon = mmap(NULL, reserve_size, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (anon == MAP_FAILED) {
            close(swap_fd);
            unlink(swap_path);
            ray_sys_free(swap_path);
            return false;
        }

        uintptr_t addr    = (uintptr_t)anon;
        uintptr_t aligned = (addr + pool_size - 1) & ~(pool_size - 1);
        if (aligned > addr)
            munmap(anon, aligned - addr);
        uintptr_t end         = addr + reserve_size;
        uintptr_t aligned_end = aligned + pool_size;
        if (end > aligned_end)
            munmap((void*)aligned_end, end - aligned_end);

        /* MAP_FIXED replaces the kept anon mapping atomically with the
         * file-backed one.  No address-space race since the kept range
         * is still anon-reserved at this point. */
        void* mapped = mmap((void*)aligned, pool_size,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_FIXED, swap_fd, 0);
        if (mapped == MAP_FAILED) {
            munmap((void*)aligned, pool_size);
            close(swap_fd);
            unlink(swap_path);
            ray_sys_free(swap_path);
            return false;
        }

        /* Count the swap-backed pool as committed working set — it is a RAM
         * substitute with preallocated blocks, not an evictable file cache.
         * The PROT_NONE reserve + slack trims above are transient and stay
         * uncounted.  Freed via ray_vm_free (which subtracts) at pool teardown. */
        ray_sys_track_add((int64_t)pool_size);
        mem = (void*)aligned;
    }

    /* Enable transparent huge pages on anon pools (Linux).  Self-aligned
     * 32MB pools are 2MB-aligned, hence THP-eligible.  Never on file-backed
     * swap mappings.  Env RAY_HEAP_HUGEPAGE=0 disables (THP-jitter-averse
     * deployments). */
    uint8_t pool_hugepage = 0;
    if (swap_fd < 0) {  /* anon pool only */
        const char* hp = getenv("RAY_HEAP_HUGEPAGE");
        if (!(hp && hp[0] == '0'))
            pool_hugepage = ray_vm_hugepage(mem, pool_size) ? 1 : 0;
    }

    /* --- Write pool header at offset 0 --- */
    ray_t* hdr_block = (ray_t*)mem;
    memset(hdr_block, 0, BSIZEOF(RAY_ORDER_MIN));
    hdr_block->mmod  = 0;
    hdr_block->order = RAY_ORDER_MIN;
    ray_atomic_store(&hdr_block->rc, 1);  /* sentinel: never free */

    ray_pool_hdr_t* hdr = (ray_pool_hdr_t*)hdr_block;  /* overlay on aux */
    hdr->heap_id    = h->id;
    hdr->pool_order = pool_order;
    hdr->vm_base    = mem;  /* on POSIX, same as aligned base */

    /* --- Cascading split: split from pool_order down to RAY_ORDER_MIN.
     *     Right half of each split → freelist.
     *     Leftmost min-block = pool header (already set, rc=1). --- */
    for (uint8_t o = pool_order; o > RAY_ORDER_MIN; o--) {
        ray_t* right = (ray_t*)((char*)mem + BSIZEOF(o - 1));
        right->mmod  = 0;
        right->order = (uint8_t)(o - 1);
        heap_insert_block(h, right, (uint8_t)(o - 1));
    }

    /* --- Track pool --- */
    h->pools[h->pool_count].base       = mem;
    h->pools[h->pool_count].pool_order = pool_order;
    h->pools[h->pool_count].backed     = (swap_fd >= 0) ? 1 : 0;
    h->pools[h->pool_count].hugepage   = pool_hugepage;
    h->pools[h->pool_count].swap_fd    = swap_fd;
    h->pools[h->pool_count].swap_path  = swap_path;  /* NULL when not backed */
    h->pool_count++;

    if (swap_fd < 0)   /* anonymous pool: counts toward the RAM watermark */
        heap_anon_commit((int64_t)pool_size);

    return true;
}

/* --------------------------------------------------------------------------
 * Slab cache flush (with coalescing for GC effectiveness)
 * -------------------------------------------------------------------------- */

static void heap_flush_slabs(ray_heap_t* h) {
    for (int i = 0; i < RAY_SLAB_ORDERS; i++) {
        while (h->slabs[i].count > 0) {
            ray_t* blk = h->slabs[i].stack[--h->slabs[i].count];
            dfd_remove(blk);
            int pidx = heap_find_pool(h, blk);
            uintptr_t pb;
            uint8_t po;
            if (pidx >= 0) {
                pb = (uintptr_t)h->pools[pidx].base;
                po = h->pools[pidx].pool_order;
            } else {
                ray_pool_hdr_t* phdr = ray_pool_of(blk);
                if (!phdr) continue;
                pb = (uintptr_t)phdr;
                po = phdr->pool_order;
            }
            heap_coalesce(h, blk, pb, po);
        }
    }
}

/* --------------------------------------------------------------------------
 * Foreign blocks flush
 *
 * When return_to_owner is true, returns each foreign block to its owning
 * heap (via pool header heap_id → global registry). This ensures workers
 * can reuse their pools across queries instead of allocating new ones.
 *
 * return_to_owner must only be true when workers are idle (on semaphore),
 * i.e. ray_parallel_flag == 0. Otherwise coalesce into current heap.
 * -------------------------------------------------------------------------- */

static void heap_flush_foreign(ray_heap_t* h, bool return_to_owner) {
    /* When workers are active (return_to_owner=false), skip entirely.
     * Foreign blocks stay queued until the proper GC flush after workers
     * finish. Absorbing foreign blocks locally would let them be re-
     * allocated under a different heap while pool ownership stays with
     * the original heap, corrupting bytes_allocated accounting. */
    if (!return_to_owner) return;

    ray_t* blk = h->foreign;
    while (blk) {
        ray_t* next = blk->fl_next;
        ray_pool_hdr_t* phdr = ray_pool_of(blk);
        if (!phdr) { blk = next; continue; }
        uint16_t owner_id = phdr->heap_id;
        ray_heap_t* owner = ray_heap_registry[owner_id % RAY_HEAP_REGISTRY_SIZE];
        if (owner && owner->id == owner_id && owner != h) {
            /* Return to owner and decrement owner's bytes_allocated.
             * Safe: workers are idle (return_to_owner=true implies
             * ray_parallel_flag==0). */
            int pidx = heap_find_pool(owner, blk);
            uintptr_t pb;
            uint8_t po;
            if (pidx >= 0) {
                pb = (uintptr_t)owner->pools[pidx].base;
                po = owner->pools[pidx].pool_order;
            } else {
                pb = (uintptr_t)phdr;
                po = phdr->pool_order;
            }
            RAY_STAT(owner->stats.bytes_allocated -= BSIZEOF(blk->order));
            heap_coalesce(owner, blk, pb, po);
        } else {
            /* Owner gone (destroyed/unregistered) — coalesce locally.
             * No stats adjustment: the owner's stats were destroyed
             * with the heap, and h never charged the alloc. */
            int pidx = heap_find_pool(h, blk);
            uintptr_t pb;
            uint8_t po;
            if (pidx >= 0) {
                pb = (uintptr_t)h->pools[pidx].base;
                po = h->pools[pidx].pool_order;
            } else {
                if (!phdr) { blk = next; continue; }
                pb = (uintptr_t)phdr;
                po = phdr->pool_order;
            }
            heap_coalesce(h, blk, pb, po);
        }
        blk = next;
    }
    h->foreign = NULL;
}

/* --------------------------------------------------------------------------
 * Owned-reference helpers
 * -------------------------------------------------------------------------- */

static bool ray_atom_str_is_sso(const ray_t* s) {
    if (s->slen >= 1 && s->slen <= 7) return true;
    if (s->slen == 0 && s->obj == NULL) return true;
    return false;
}

static bool ray_atom_owns_obj(const ray_t* v) {
    if (v->type == -RAY_GUID) return v->obj != NULL;
    if (v->type == -RAY_STR) return !ray_atom_str_is_sso(v);
    return false;
}

static void ray_release_owned_refs(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;

    if (ray_is_atom(v)) {
        if (v->type == RAY_LAMBDA) {
            /* Lambda stores [params, body, bytecode, constants, n_locals, nfo, dbg] in ray_data */
            ray_t** slots = (ray_t**)ray_data(v);
            for (int i = 0; i < 4; i++) {
                if (slots[i] && !RAY_IS_ERR(slots[i]))
                    ray_release(slots[i]);
            }
            /* Release optional debug info slots */
            if (LAMBDA_NFO(v)) ray_release(LAMBDA_NFO(v));
            if (LAMBDA_DBG(v)) ray_release(LAMBDA_DBG(v));
            return;
        }
        if (v->type == RAY_LAZY) {
            ray_graph_t* g = RAY_LAZY_GRAPH(v);
            if (g) {
                ray_graph_free(g);
                RAY_LAZY_GRAPH(v) = NULL;
            }
            return;
        }
        /* I64 atom tagged as an HNSW handle owns a ray_hnsw_t — free it
         * when the atom's rc drops to zero so rebindings and scope-exit
         * don't leak the (potentially large) index graph. */
        if (v->type == -RAY_I64 && (v->attrs & RAY_ATTR_HNSW)) {
            ray_hnsw_t* idx = (ray_hnsw_t*)(uintptr_t)v->i64;
            if (idx) ray_hnsw_free(idx);
            v->i64 = 0;
            v->attrs &= (uint8_t)~RAY_ATTR_HNSW;
            return;
        }
        /* I64 atom tagged as a graph handle owns a ray_rel_t (CSR) — free
         * it on rc→0 so users don't leak when rebinding or going out of
         * scope without an explicit (.graph.free h). */
        if (v->type == -RAY_I64 && (v->attrs & RAY_ATTR_GRAPH)) {
            ray_rel_t* rel = (ray_rel_t*)(uintptr_t)v->i64;
            if (rel) ray_rel_free(rel);
            v->i64 = 0;
            v->attrs &= (uint8_t)~RAY_ATTR_GRAPH;
            return;
        }
        if (ray_atom_owns_obj(v) && v->obj && !RAY_IS_ERR(v->obj))
            ray_release(v->obj);
        return;
    }

    if (v->attrs & RAY_ATTR_SLICE) {
        if (v->slice_parent && !RAY_IS_ERR(v->slice_parent))
            ray_release(v->slice_parent);
        return;
    }

    /* RAY_INDEX block: release per-kind payload children + saved-aux
     * pointers.  Must run before the LIST/TABLE compound checks below
     * (which would mistreat the data[] payload as child pointers). */
    if (v->type == RAY_INDEX) {
        ray_index_t* ix = ray_index_payload(v);
        ray_index_release_payload(ix);
        ray_index_release_saved(ix);
        return;
    }

    /* Vector with attached index: aux[0..7] holds an owning ref to
     * the index ray_t.  The index owns the displaced str_pool,
     * so we must NOT also try to release those off the parent — they
     * aren't there anymore.  Skip the STR_pool branch. */
    if (v->attrs & RAY_ATTR_HAS_INDEX) {
        /* A mmap-resident index (mmod==1) is a PASSENGER in this column's file
         * mapping — the column's single munmap frees it.  Releasing it here
         * would ray_free it and munmap a sub-region of the mapping.  Only a
         * heap-built index (mmod==0) is released by pointer. */
        if (v->index && !RAY_IS_ERR(v->index) && v->index->mmod != 1)
            ray_release(v->index);
        return;
    }

    if (v->type == RAY_STR && v->str_pool && !RAY_IS_ERR(v->str_pool))
        ray_release(v->str_pool);

    /* RAY_SYM vec: drop the resolution-domain ref (aux bytes 8-15).
     * No-op for the immortal runtime singleton — the common case. */
    if (v->type == RAY_SYM && v->sym_domain) {
        ray_sym_domain_release(v->sym_domain);
        return;
    }

    if (RAY_IS_PARTED(v->type)) {
        int64_t n_segs = v->len;
        ray_t** segs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < n_segs; i++) {
            if (segs[i] && !RAY_IS_ERR(segs[i]))
                ray_release(segs[i]);
        }
        return;
    }

    if (v->type == RAY_MAPCOMMON) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        if (ptrs[0] && !RAY_IS_ERR(ptrs[0])) ray_release(ptrs[0]);
        if (ptrs[1] && !RAY_IS_ERR(ptrs[1])) ray_release(ptrs[1]);
        return;
    }

    if (v->type == RAY_TABLE || v->type == RAY_DICT) {
        ray_t** slots = (ray_t**)ray_data(v);
        if (slots[0] && !RAY_IS_ERR(slots[0])) ray_release(slots[0]);
        if (slots[1] && !RAY_IS_ERR(slots[1])) ray_release(slots[1]);
        return;
    }

    if (v->type == RAY_LIST) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < v->len; i++) {
            ray_t* child = ptrs[i];
            if (child && !RAY_IS_ERR(child)) ray_release(child);
        }
    }
}

bool ray_retain_owned_refs(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return true;

    if (ray_is_atom(v)) {
        if (v->type == RAY_LAMBDA) {
            ray_t** slots = (ray_t**)ray_data(v);
            for (int i = 0; i < 4; i++) {
                if (slots[i] && !RAY_IS_ERR(slots[i]))
                    ray_retain(slots[i]);
            }
            if (LAMBDA_NFO(v)) ray_retain(LAMBDA_NFO(v));
            if (LAMBDA_DBG(v)) ray_retain(LAMBDA_DBG(v));
            return true;
        }
        /* Lazy handles own their graph uniquely — no retain on copy */
        if (v->type == RAY_LAZY) return true;
        /* HNSW handle owns its ray_hnsw_t uniquely.  Deep-clone the index
         * so the copy is an independent owner with the same semantics as
         * the source.  On clone-OOM, detach the copy (so caller can free
         * it cleanly) and signal failure — the caller must not treat the
         * copy as a valid handle. */
        if (v->type == -RAY_I64 && (v->attrs & RAY_ATTR_HNSW)) {
            ray_hnsw_t* src = (ray_hnsw_t*)(uintptr_t)v->i64;
            if (src) {
                ray_hnsw_t* dup = ray_hnsw_clone(src);
                if (!dup) {
                    v->i64 = 0;
                    v->attrs &= (uint8_t)~RAY_ATTR_HNSW;
                    return false;
                }
                v->i64 = (int64_t)(uintptr_t)dup;
            }
            return true;
        }
        /* Graph handle (ray_rel_t*).  No deep-clone API exists for CSR
         * (could be GBs), and the user's mental model is "the handle is
         * the graph" — like a file descriptor.  On block-copy we detach
         * the COPY (the source keeps ownership) so a later free of the
         * duplicate doesn't touch the source's CSR.  This matches the
         * lazy-graph (RAY_LAZY) policy above. */
        if (v->type == -RAY_I64 && (v->attrs & RAY_ATTR_GRAPH)) {
            v->i64 = 0;
            v->attrs &= (uint8_t)~RAY_ATTR_GRAPH;
            return true;
        }
        if (ray_atom_owns_obj(v) && v->obj && !RAY_IS_ERR(v->obj))
            ray_retain(v->obj);
        return true;
    }

    if (v->attrs & RAY_ATTR_SLICE) {
        if (v->slice_parent && !RAY_IS_ERR(v->slice_parent))
            ray_retain(v->slice_parent);
        return true;
    }

    if (v->type == RAY_INDEX) {
        ray_index_t* ix = ray_index_payload(v);
        ray_index_retain_payload(ix);
        ray_index_retain_saved(ix);
        return true;
    }

    if (v->attrs & RAY_ATTR_HAS_INDEX) {
        /* Mirror ray_release_owned_refs: a mmap-resident passenger index
         * (mmod==1) is owned by the column's mapping, not refcounted here. */
        if (v->index && !RAY_IS_ERR(v->index) && v->index->mmod != 1)
            ray_retain(v->index);
        return true;
    }

    if (v->type == RAY_STR && v->str_pool && !RAY_IS_ERR(v->str_pool))
        ray_retain(v->str_pool);

    /* RAY_SYM vec: the header copy carried the domain pointer — take a
     * ref.  No-op for the immortal runtime singleton. */
    if (v->type == RAY_SYM && v->sym_domain) {
        ray_sym_domain_retain(v->sym_domain);
        return true;
    }

    if (RAY_IS_PARTED(v->type)) {
        int64_t n_segs = v->len;
        ray_t** segs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < n_segs; i++) {
            if (segs[i] && !RAY_IS_ERR(segs[i]))
                ray_retain(segs[i]);
        }
        return true;
    }

    if (v->type == RAY_MAPCOMMON) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        if (ptrs[0] && !RAY_IS_ERR(ptrs[0])) ray_retain(ptrs[0]);
        if (ptrs[1] && !RAY_IS_ERR(ptrs[1])) ray_retain(ptrs[1]);
        return true;
    }

    if (v->type == RAY_TABLE || v->type == RAY_DICT) {
        ray_t** slots = (ray_t**)ray_data(v);
        if (slots[0] && !RAY_IS_ERR(slots[0])) ray_retain(slots[0]);
        if (slots[1] && !RAY_IS_ERR(slots[1])) ray_retain(slots[1]);
        return true;
    }

    if (v->type == RAY_LIST) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < v->len; i++) {
            ray_t* child = ptrs[i];
            if (child && !RAY_IS_ERR(child)) ray_retain(child);
        }
    }
    return true;
}

static void ray_detach_owned_refs(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;

    if (ray_is_atom(v)) {
        if (v->type == RAY_LAMBDA) {
            ray_t** slots = (ray_t**)ray_data(v);
            for (int i = 0; i < 4; i++) slots[i] = NULL;
            LAMBDA_NFO(v) = NULL;
            LAMBDA_DBG(v) = NULL;
            return;
        }
        if (v->type == RAY_LAZY) {
            RAY_LAZY_GRAPH(v) = NULL;
            RAY_LAZY_OP(v)    = NULL;
            return;
        }
        /* HNSW handle: ownership has been transferred elsewhere; stop the
         * rc→0 cleanup hook from freeing the (now-foreign) index. */
        if (v->type == -RAY_I64 && (v->attrs & RAY_ATTR_HNSW)) {
            v->i64 = 0;
            v->attrs &= (uint8_t)~RAY_ATTR_HNSW;
            return;
        }
        /* Graph handle: same semantics as HNSW — ownership has moved,
         * clear the bit so rc→0 doesn't ray_rel_free a foreign pointer. */
        if (v->type == -RAY_I64 && (v->attrs & RAY_ATTR_GRAPH)) {
            v->i64 = 0;
            v->attrs &= (uint8_t)~RAY_ATTR_GRAPH;
            return;
        }
        if (ray_atom_owns_obj(v)) v->obj = NULL;
        return;
    }

    if (v->attrs & RAY_ATTR_SLICE) {
        v->slice_parent = NULL;
        v->slice_offset = 0;
        v->attrs &= (uint8_t)~RAY_ATTR_SLICE;
        return;
    }

    if (v->type == RAY_INDEX) {
        ray_index_t* ix = ray_index_payload(v);
        switch ((ray_idx_kind_t)ix->kind) {
        case RAY_IDX_HASH:
            ix->u.hash.table = ix->u.hash.gkeys = NULL;
            ix->u.hash.offs  = ix->u.hash.rows  = NULL;
            break;
        case RAY_IDX_SORT:  ix->u.sort.perm = NULL; break;
        case RAY_IDX_BLOOM: ix->u.bloom.bits = NULL; break;
        default: break;
        }
        memset(ix->saved_aux, 0, 16);
        ix->saved_attrs = 0;
        return;
    }

    if (v->attrs & RAY_ATTR_HAS_INDEX) {
        v->index    = NULL;
        v->_idx_pad = NULL;
        v->attrs   &= (uint8_t)~RAY_ATTR_HAS_INDEX;
        return;
    }

    if (v->type == RAY_STR) {
        v->str_pool = NULL;
    }

    /* RAY_SYM vec: domain ref transferred elsewhere (e.g. the header
     * memcpy in ray_scratch_realloc) — clear so ray_free won't drop it.
     * The defensive ray_sym_vec_domain fallback covers any reader that
     * sees the detached header. */
    if (v->type == RAY_SYM) {
        v->sym_domain = NULL;
        return;
    }

    if (RAY_IS_PARTED(v->type)) {
        int64_t n_segs = v->len;
        ray_t** segs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < n_segs; i++)
            segs[i] = NULL;
        return;
    }

    if (v->type == RAY_MAPCOMMON) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        ptrs[0] = NULL;
        ptrs[1] = NULL;
        return;
    }

    if (v->type == RAY_TABLE || v->type == RAY_DICT) {
        ray_t** slots = (ray_t**)ray_data(v);
        slots[0] = NULL;
        slots[1] = NULL;
        v->len = 0;
        return;
    }

    if (v->type == RAY_LIST) {
        v->len = 0;
    }
}

/* --------------------------------------------------------------------------
 * ray_alloc
 * -------------------------------------------------------------------------- */

/* File-backed direct mapping: a disk-spilled standalone allocation, used when
 * an allocation cannot be RAM-resident (larger than physical RAM) or when the
 * anonymous mmap was refused.  Mirrors heap_add_pool's swap fallback but at the
 * exact size — a direct block needs no pool alignment (it is located by its
 * stored map_size, not by pool-base masking), so a plain mmap suffices.
 * Returns the mapped base and the fd/path to close+unlink at free; NULL on
 * failure. */
static void* heap_direct_map_file(ray_heap_t* h, size_t map_size,
                                  int* out_fd, char** out_path) {
    static _Atomic uint64_t direct_swap_counter = 0;
    uint64_t cnt = atomic_fetch_add_explicit(&direct_swap_counter, 1,
                                             memory_order_relaxed);
    size_t plen = strlen(h->swap_path);
    size_t need = plen + 64;  /* room for "raydirect_<pid>_<heap>_<cnt>.dat" */
    char*  path = (char*)ray_sys_alloc(need);
    if (!path) return NULL;
    snprintf(path, need, "%sraydirect_%d_%u_%llu.dat", h->swap_path,
             (int)getpid(), (unsigned)h->id, (unsigned long long)cnt);

    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { ray_sys_free(path); return NULL; }

    /* Reserve real disk blocks up front so later stores can't SIGBUS on a
     * disk-full condition.  ENOSPC here surfaces as a clean NULL -> "oom". */
    if (heap_preallocate(fd, 0, (off_t)map_size) != 0) {
        close(fd); unlink(path); ray_sys_free(path); return NULL;
    }
    void* mapped = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd); unlink(path); ray_sys_free(path); return NULL;
    }
    /* Count as committed working set — a RAM substitute with preallocated
     * blocks (matches heap_add_pool's swap-pool accounting). */
    ray_sys_track_add((int64_t)map_size);
    *out_fd   = fd;
    *out_path = path;
    return mapped;
}

/* Direct large allocation: mmap the exact page-rounded size instead of a
 * power-of-2 oversized buddy pool.  Returns a marked ray_t or NULL on failure. */
static ray_t* heap_alloc_direct(ray_heap_t* h, size_t data_size) {
    if (data_size > SIZE_MAX - RAY_DIRECT_HDR - 32 - 4095) return NULL;
    size_t map_size = (RAY_DIRECT_HDR + 32 + data_size + 4095) & ~(size_t)4095;

    void* base      = NULL;
    int   swap_fd   = -1;
    char* swap_path = NULL;

    /* If keeping this in anonymous RAM would push our footprint past the
     * watermark (default: physical RAM), back it with a disk file from the
     * start so it spills instead of being OOM-killed as its pages fault in.
     * (Under lenient overcommit an anon mmap the kernel can't actually back
     * SUCCEEDS, then kills us at fault time — so a fallback-on-failure alone
     * would not catch it.)  Otherwise use anonymous RAM, and fall back to a
     * spill file only if the kernel refuses the mapping. */
    bool force_file = heap_anon_would_exceed(map_size);
    if (!force_file)
        base = ray_vm_alloc(map_size);   /* anon RW, page-aligned, counted */
    if (!base) {
        base = heap_direct_map_file(h, map_size, &swap_fd, &swap_path);
        if (!base) return NULL;
    }
    if (swap_fd < 0)   /* anonymous: counts toward the RAM watermark */
        heap_anon_commit((int64_t)map_size);

    ray_direct_hdr_t* hdr = (ray_direct_hdr_t*)base;
    hdr->map_size  = map_size;
    hdr->swap_fd   = swap_fd;
    hdr->swap_path = swap_path;

    ray_t* v = (ray_t*)((char*)base + RAY_DIRECT_HDR);
    memset(v, 0, 32);
    v->mmod  = 0;
    v->order = RAY_ORDER_DIRECT;
    if (RAY_UNLIKELY(ray_rc_sync))
        ray_atomic_store(&v->rc, 1);
    else
        v->rc = 1;

    atomic_fetch_add_explicit(&g_direct_bytes, (int64_t)map_size, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_direct_count, 1, memory_order_relaxed);
    RAY_STAT(h->stats.alloc_count++);
    return v;
}

ray_t* ray_alloc(size_t data_size) {
    ray_heap_t* h = ray_tl_heap;
    if (RAY_UNLIKELY(!h)) {
        ray_heap_init();
        h = ray_tl_heap;
        if (!h) return NULL;
    }

    uint8_t order = ray_order_for_size(data_size);

    /* Anything that would need a dedicated oversized pool goes direct — exact
     * size, no power-of-2 rounding, no order+1 pool.  This includes sizes
     * above the largest buddy order: direct blocks carry their exact map_size
     * in a prefix header, so they are not bounded by RAY_HEAP_MAX_ORDER.  On
     * mmap failure, fall through to the buddy pool only when the buddy model
     * can still represent the request. */
    if (RAY_UNLIKELY(order >= RAY_HEAP_POOL_ORDER)) {
        ray_t* v = heap_alloc_direct(h, data_size);
        if (v) {
            ray_direct_hdr_t* hdr = (ray_direct_hdr_t*)((char*)v - RAY_DIRECT_HDR);
            mem_trace_note_alloc(hdr->map_size);
            return v;
        }
        if (order > RAY_HEAP_MAX_ORDER) return NULL;
    }

    if (order > RAY_HEAP_MAX_ORDER) return NULL;

    /* Slab fast path */
    if (RAY_LIKELY(IS_SLAB_ORDER(order))) {
        int idx = SLAB_INDEX(order);
        if (RAY_LIKELY(h->slabs[idx].count > 0)) {
            ray_t* v = h->slabs[idx].stack[--h->slabs[idx].count];
            dfd_remove(v);

            /* Zero full 32-byte header (hot path).
             * Null bitmap (bytes 0-15) must be cleared for null-bit correctness. */
            memset(v, 0, 32);
            v->order = order;
            if (RAY_UNLIKELY(ray_rc_sync))
                ray_atomic_store(&v->rc, 1);
            else
                v->rc = 1;

            RAY_STAT(h->stats.alloc_count++);
            RAY_STAT(h->stats.slab_hits++);
            RAY_STAT(h->stats.bytes_allocated += BSIZEOF(order));
            RAY_STAT(h->stats.peak_bytes = h->stats.bytes_allocated > h->stats.peak_bytes
                ? h->stats.bytes_allocated : h->stats.peak_bytes);
            mem_trace_note_alloc(BSIZEOF(order));
            return v;
        }
    }

    /* Find free block via avail bitmask.
     * Avail bits can be stale from cross-heap fl_remove, so we loop
     * to find a genuinely non-empty freelist. */
    uint64_t candidates = h->avail & (UINT64_MAX << order);

    if (RAY_UNLIKELY(candidates == 0)) {
        heap_flush_foreign(h, false);  /* always local in ray_alloc */

        candidates = h->avail & (UINT64_MAX << order);

        if (candidates == 0) {
            if (!heap_add_pool(h, order)) return NULL;
            candidates = h->avail & (UINT64_MAX << order);
            if (candidates == 0) return NULL;
        }
    }

    /* Scan past stale avail bits (cross-heap fl_remove may have emptied lists) */
    uint8_t found_order;
    for (;;) {
        if (candidates == 0) {
            if (!heap_add_pool(h, order)) return NULL;
            candidates = h->avail & (UINT64_MAX << order);
            if (candidates == 0) return NULL;
        }
        found_order = (uint8_t)__builtin_ctzll(candidates);
        if (!fl_empty(&h->freelist[found_order])) break;
        /* Clear stale bit and try next */
        h->avail &= ~(1ULL << found_order);
        candidates &= ~(1ULL << found_order);
    }

    /* Pop from circular sentinel freelist */
    ray_fl_head_t* head = &h->freelist[found_order];
    ray_t* blk = head->fl_next;
    fl_remove(blk);
    dfd_remove(blk);
    if (fl_empty(head))
        h->avail &= ~(1ULL << found_order);

    /* Split down to requested order */
    heap_split_block(h, blk, order, found_order);

    /* Zero ray_t header and set metadata */
    memset(blk, 0, 32);
    blk->mmod  = 0;
    blk->order = order;
    if (RAY_UNLIKELY(ray_rc_sync))
        ray_atomic_store(&blk->rc, 1);
    else
        blk->rc = 1;

    RAY_STAT(h->stats.alloc_count++);
    RAY_STAT(h->stats.bytes_allocated += BSIZEOF(order));
    RAY_STAT(h->stats.peak_bytes = h->stats.bytes_allocated > h->stats.peak_bytes
        ? h->stats.bytes_allocated : h->stats.peak_bytes);

    mem_trace_note_alloc(BSIZEOF(order));

    return blk;
}

/* --------------------------------------------------------------------------
 * ray_free
 * -------------------------------------------------------------------------- */

void ray_free(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;
    if (v->attrs & RAY_ATTR_ARENA) return;  /* arena-owned, bulk-freed */
#ifdef DEBUG
    if (dfd_contains(v)) dfd_report("ray_free (double free)", v);
#endif

    /* Guard: keep rc=1 while releasing children so buddy coalescing
     * won't merge this block prematurely (it checks buddy_rc==0). */
    ray_atomic_store(&v->rc, 1);

    ray_release_owned_refs(v);

    ray_heap_t* h = ray_tl_heap;

    /* File-mapped: munmap */
    if (v->mmod == 1) {
        if (v->type == RAY_TABLE || v->type == RAY_DICT || v->type == RAY_LIST) return;
        if (v->type > 0 && v->type < RAY_TYPE_COUNT) {
            uint8_t esz = ray_sym_elem_size(v->type, v->attrs);
            size_t data_size = 32 + (size_t)v->len * esz;
            if (v->type == RAY_STR) {
                size_t pool_len = 0;
                if (v->str_pool && !RAY_IS_ERR(v->str_pool) && v->str_pool->len > 0)
                    pool_len = (size_t)v->str_pool->len;
                data_size += 32 + pool_len;
            }
            /* Inline index region: a mmap-resident (passenger) index extends the
             * mapping past the payload by its 32-aligned inline region.  Derive
             * that size from the index itself (ray_index_inline_size) rather than
             * stashing it in aux — str_pool occupies _idx_pad on STR columns, and
             * the payload size above already accounts for descriptors + pool.
             * Heap-resident indexes (RAY_MARK_MMAP clear) keep the payload-only
             * formula. */
            if ((v->attrs & RAY_ATTR_HAS_INDEX) && v->index && !RAY_IS_ERR(v->index)) {
                ray_index_t* ix = ray_index_payload(v->index);
                if (ix->markers & RAY_MARK_MMAP) {
                    int64_t region_off = ((int64_t)data_size + 31) & ~(int64_t)31;
                    data_size = (size_t)(region_off + ray_index_inline_size(ix));
                }
            }
            size_t mapped_size = (data_size + 4095) & ~(size_t)4095;
            ray_vm_unmap_file(v, mapped_size);
        } else {
            ray_vm_unmap_file(v, 4096);
        }
        if (h) RAY_STAT(h->stats.free_count++);
        return;
    }

    /* Legacy mmod==2 guard */
    if (v->mmod == 2) return;

    /* Direct large allocation: standalone mmap, freed by munmap.  Checked
     * BEFORE any pool masking (a direct block is not a pool member).  Any
     * thread may free it; the counter is a global atomic. */
    if (RAY_UNLIKELY(v->order > RAY_HEAP_MAX_ORDER)) {
        char*             base = (char*)v - RAY_DIRECT_HDR;
        ray_direct_hdr_t* hdr  = (ray_direct_hdr_t*)base;
        size_t map_size  = hdr->map_size;
        int    swap_fd   = hdr->swap_fd;
        char*  swap_path = hdr->swap_path;
        mem_trace_note_free(map_size);
        if (h) RAY_STAT(h->stats.free_count++);
        atomic_fetch_sub_explicit(&g_direct_bytes, (int64_t)map_size, memory_order_relaxed);
        atomic_fetch_sub_explicit(&g_direct_count, 1, memory_order_relaxed);
        if (swap_fd >= 0) {
            /* File-backed spill: mapped directly (not via ray_vm_alloc), so
             * unmap + uncount by hand, then close and unlink the spill file. */
            munmap(base, map_size);
            ray_sys_track_sub((int64_t)map_size);
            close(swap_fd);
            if (swap_path) { unlink(swap_path); ray_sys_free(swap_path); }
        } else {
            atomic_fetch_sub_explicit(&g_anon_committed, (int64_t)map_size,
                                      memory_order_relaxed);
            ray_vm_free(base, map_size);   /* munmap + committed-RAM counter -= */
        }
        return;
    }

    if (!h) return;

    uint8_t order = v->order;

    if (order < RAY_ORDER_MIN || order > RAY_HEAP_MAX_ORDER) return;

    mem_trace_note_free(BSIZEOF(order));

#if RAY_MEM_STATS
    size_t block_size = BSIZEOF(order);
#endif

    /* Warm-first ownership check: probe the MRU pool, then scan the warm
     * h->pools[] array.  A hit proves the block is local and yields
     * pool base+order WITHOUT reading the cold pool header 32MB away.
     * Only a full miss falls through to the foreign path.  heap_find_pool
     * returning >=0 is equivalent to the old phdr->heap_id == h->id check. */
    int pidx = -1;
    if (h->last_pool_idx < h->pool_count) {
        uintptr_t pb = (uintptr_t)h->pools[h->last_pool_idx].base;
        uintptr_t pe = pb + BSIZEOF(h->pools[h->last_pool_idx].pool_order);
        if ((uintptr_t)v >= pb && (uintptr_t)v < pe)
            pidx = (int)h->last_pool_idx;
    }
    if (pidx < 0) pidx = heap_find_pool(h, v);

    if (pidx >= 0) {
        /* Local block — base+order from the warm pools array. */
        h->last_pool_idx = (uint32_t)pidx;
        uintptr_t pool_base  = (uintptr_t)h->pools[pidx].base;
        uint8_t   pool_order = h->pools[pidx].pool_order;

        /* Slab fast path */
        if (IS_SLAB_ORDER(order)) {
            int idx = SLAB_INDEX(order);
            if (h->slabs[idx].count < h->slab_cap[idx]) {
                /* rc=1 so buddy coalescing skips slab-cached blocks (a buddy
                 * freed concurrently reads rc; rc==0 would wrongly merge). */
                ray_atomic_store(&v->rc, 1);
                dfd_add(v);
                h->slabs[idx].stack[h->slabs[idx].count++] = v;
                RAY_STAT(h->stats.free_count++);
                RAY_STAT(h->stats.bytes_allocated -= block_size);
                return;
            }
        }

        heap_coalesce(h, v, pool_base, pool_order);
        RAY_STAT(h->stats.free_count++);
        RAY_STAT(h->stats.bytes_allocated -= block_size);
        return;
    }

    /* Foreign: not in any of our pools.  Enqueue to the foreign list for
     * return to the owner during GC.  Do NOT adjust bytes_allocated: the
     * block stays counted on the owning heap until returned and coalesced. */
    dfd_add(v);
    v->fl_next = h->foreign;
    h->foreign = v;
    RAY_STAT(h->stats.free_count++);
}

/* --------------------------------------------------------------------------
 * Raw buffer allocator: malloc/calloc/realloc/free for plain byte buffers that
 * are NOT ray_t values, backed by the buddy heap (no libc malloc).  The block
 * is a real ray_t header + data; we stamp type=RAY_U8 (no owned children) so
 * ray_free reclaims it without walking elements, and hand back ray_data().  The
 * header sits 32 bytes before the returned pointer; ray_free_raw recovers it.
 * -------------------------------------------------------------------------- */

void* ray_alloc_raw(size_t n) {
    ray_t* v = ray_alloc(n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->type = RAY_U8;          /* plain byte vec: ray_free reclaims, no child walk */
    v->len  = (int64_t)n;
    return ray_data(v);
}

void* ray_calloc_raw(size_t n) {
    void* p = ray_alloc_raw(n);
    if (p && n) memset(p, 0, n);
    return p;
}

void ray_free_raw(void* p) {
    if (!p) return;
    ray_free((ray_t*)((char*)p - 32));   /* 32 = ray_t header before the data */
}

void* ray_realloc_raw(void* p, size_t n) {
    if (!p) return ray_alloc_raw(n);
    ray_t* v = (ray_t*)((char*)p - 32);
    size_t cur = ray_block_data_bytes(v);   /* data capacity (buddy or direct) */
    if (n <= cur) { v->len = (int64_t)n; return p; }
    void* np = ray_alloc_raw(n);
    if (!np) return NULL;
    memcpy(np, p, cur);                     /* copy the whole old block (>= valid bytes) */
    ray_free_raw(p);
    return np;
}

/* --------------------------------------------------------------------------
 * ray_alloc_copy
 * -------------------------------------------------------------------------- */

ray_t* ray_alloc_copy(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return NULL;
    size_t data_size;
    if (v->attrs & RAY_ATTR_SLICE) {
        /* Slice blocks are header-only views; their .len reflects the
         * slice extent on the parent, not their own storage.  A naive
         * len*esz would overrun the 32-byte header.  ray_retain_owned_refs
         * then bumps slice_parent's rc, keeping the view valid. */
        data_size = 0;
    } else if (ray_is_atom(v)) {
        data_size = 0;
    } else if (v->type == RAY_TABLE || v->type == RAY_DICT) {
        data_size = 2 * sizeof(ray_t*);
    } else if (RAY_IS_PARTED(v->type) || v->type == RAY_MAPCOMMON) {
        int64_t n_ptrs = v->len;
        if (v->type == RAY_MAPCOMMON) n_ptrs = 2;
        if (n_ptrs < 0) return ray_error("oom", NULL);
        data_size = (size_t)n_ptrs * sizeof(ray_t*);
    } else if (v->type == RAY_LIST) {
        /* RAY_LIST has type==0, which the generic branch below (t <= 0)
         * would route to data_size=0, silently producing a header-only copy
         * whose item-pointer area is uninitialised — a shallow COW of a
         * shared list would then lose every element.  Handle explicitly. */
        if (v->len < 0 || (uint64_t)v->len > SIZE_MAX / sizeof(ray_t*))
            return ray_error("oom", NULL);
        data_size = (size_t)ray_len(v) * sizeof(ray_t*);
    } else {
        int8_t t = ray_type(v);
        if (t <= 0 || t >= RAY_TYPE_COUNT)
            data_size = 0;
        else {
            uint8_t esz = ray_sym_elem_size(t, v->attrs);
            if (v->len < 0 || (esz > 0 && (uint64_t)v->len > SIZE_MAX / esz))
                return ray_error("oom", NULL);
            data_size = (size_t)ray_len(v) * esz;
        }
    }
    ray_t* copy = ray_alloc(data_size);
    if (!copy) return NULL;

    uint8_t new_order = copy->order;
    uint8_t new_mmod  = copy->mmod;
    memcpy(copy, v, 32 + data_size);
    copy->mmod  = new_mmod;
    copy->order = new_order;
    if (RAY_UNLIKELY(ray_rc_sync))
        ray_atomic_store(&copy->rc, 1);
    else
        copy->rc = 1;
    if (!ray_retain_owned_refs(copy)) {
        /* Deep-clone of an owned resource failed (e.g. HNSW index OOM).
         * The copy's owned state has already been neutralized, so a plain
         * ray_free won't touch the source's resources. */
        ray_free(copy);
        return ray_error("oom", NULL);
    }
    return copy;
}

/* --------------------------------------------------------------------------
 * ray_scratch_alloc / ray_scratch_realloc
 * -------------------------------------------------------------------------- */

ray_t* ray_scratch_alloc(size_t data_size) {
    return ray_alloc(data_size);
}

ray_t* ray_scratch_realloc(ray_t* v, size_t new_data_size) {
    ray_t* new_v = ray_alloc(new_data_size);
    if (!new_v) return NULL;
    if (v && !RAY_IS_ERR(v)) {
        size_t old_data;
        if (v->attrs & RAY_ATTR_SLICE)
            old_data = 0;  /* slice blocks are header-only — no own storage */
        else if (ray_is_atom(v))
            old_data = 0;
        else if (v->type == RAY_LIST) {
            if (v->len < 0) { old_data = 0; }
            else old_data = (size_t)ray_len(v) * sizeof(ray_t*);
        } else if (v->type == RAY_TABLE || v->type == RAY_DICT) {
            old_data = 2 * sizeof(ray_t*);
        } else if (RAY_IS_PARTED(v->type) || v->type == RAY_MAPCOMMON) {
            int64_t n_ptrs = v->len;
            if (v->type == RAY_MAPCOMMON) n_ptrs = 2;
            if (n_ptrs < 0) n_ptrs = 0;
            old_data = (size_t)n_ptrs * sizeof(ray_t*);
        } else {
            int8_t t = ray_type(v);
            old_data = (t > 0 && t < RAY_TYPE_COUNT && v->len >= 0) ?
                       (size_t)ray_len(v) * ray_sym_elem_size(t, v->attrs) : 0;
        }
        /* Clamp old_data to actual allocation size */
        if (v->mmod == 0 && v->order >= RAY_ORDER_MIN) {
            size_t alloc_data = ray_block_data_bytes(v);  /* buddy or direct */
            if (old_data > alloc_data) old_data = alloc_data;
        }
        size_t copy_data = old_data < new_data_size ? old_data : new_data_size;
        uint8_t new_mmod = new_v->mmod;
        uint8_t new_order = new_v->order;
        memcpy(new_v, v, 32 + copy_data);
        new_v->mmod = new_mmod;
        new_v->order = new_order;
        if (RAY_UNLIKELY(ray_rc_sync))
            ray_atomic_store(&new_v->rc, 1);
        else
            new_v->rc = 1;
        /* Ownership transfers via memcpy — no retain needed on new_v.
         * Detach nulls old pointers so ray_free won't double-release. */
        if (!(v->attrs & RAY_ATTR_ARENA)) {
            ray_detach_owned_refs(v);
            ray_free(v);
        }
    }
    return new_v;
}

/* --------------------------------------------------------------------------
 * ray_mem_stats
 * -------------------------------------------------------------------------- */

void ray_mem_stats(ray_mem_stats_t* out) {
    if (ray_tl_heap)
        *out = ray_tl_heap->stats;
    else
        memset(out, 0, sizeof(*out));
    int64_t sc = 0, sp = 0;
    ray_sys_get_stat(&sc, &sp);
    out->sys_current = (size_t)sc;
    out->sys_peak    = (size_t)sp;
    int64_t mc = 0, mp = 0;
    ray_sys_get_mapped(&mc, &mp);
    out->sys_mapped      = (size_t)mc;
    out->sys_mapped_peak = (size_t)mp;
    /* Direct large allocations are process-wide (not per-thread) — overlay the
     * global counters so bytes_allocated + direct_bytes is the true live
     * object footprint (buddy blocks + exact direct mmaps). */
    out->direct_bytes = (size_t)atomic_load_explicit(&g_direct_bytes, memory_order_relaxed);
    out->direct_count = (size_t)atomic_load_explicit(&g_direct_count, memory_order_relaxed);
}

/* --------------------------------------------------------------------------
 * Heap lifecycle
 * -------------------------------------------------------------------------- */


void ray_heap_init(void) {
    if (ray_tl_heap) return;

    size_t heap_sz = (sizeof(ray_heap_t) + 4095) & ~(size_t)4095;
    ray_heap_t* h = (ray_heap_t*)ray_vm_alloc(heap_sz);
    /* A silent return here leaves ray_tl_heap NULL, so every later
     * ray_alloc on this thread fails and the caller sees a baffling
     * downstream error (e.g. a save returning IO) with no hint that the
     * heap never came up.  Make both init failure modes loud. */
    if (!h) {
        fprintf(stderr, "ray_heap_init: ray_vm_alloc(%zu) failed (errno=%d %s) "
                        "— heap not initialized\n",
                heap_sz, errno, strerror(errno));
        return;
    }
    memset(h, 0, heap_sz);

    /* Bitmap-based ID: acquire reusable ID via atomic CAS */
    int id = heap_id_acquire();
    if (id < 0) {
        fprintf(stderr, "ray_heap_init: heap-ID pool exhausted "
                        "— heap not initialized\n");
        ray_vm_free(h, heap_sz);
        return;  /* ID pool exhausted */
    }
    h->id = (uint16_t)id;

    /* Register in global heap registry (lock: a concurrent destroyer may be
     * walking the array in its foreign-purge loop). */
    ray_registry_lock();
    ray_heap_registry[h->id % RAY_HEAP_REGISTRY_SIZE] = h;
    ray_registry_unlock();

    /* Initialize circular sentinel freelists */
    for (int i = 0; i < RAY_HEAP_FL_SIZE; i++)
        fl_init(&h->freelist[i]);

    /* Per-order slab push caps from the byte budget (env-overridable).
     * cap[o] = clamp(budget / 2^o, 1, RAY_SLAB_CACHE_SIZE): large orders
     * keep fewer entries, giving a predictable per-thread RSS ceiling. */
    {
        size_t budget = RAY_SLAB_BUDGET;
        const char* b = getenv("RAY_SLAB_BUDGET");
        if (b && *b) {
            char* end = NULL;
            unsigned long long bv = strtoull(b, &end, 10);
            if (end && *end == '\0' && bv > 0) budget = (size_t)bv;
        }
        for (int i = 0; i < RAY_SLAB_ORDERS; i++) {
            size_t bsize = BSIZEOF(RAY_SLAB_MIN + i);
            size_t cap = budget / bsize;
            if (cap < 1) cap = 1;
            if (cap > RAY_SLAB_CACHE_SIZE) cap = RAY_SLAB_CACHE_SIZE;
            h->slab_cap[i] = (uint32_t)cap;
        }
    }

    /* Resolve swap directory for the file-backed pool fallback.  Order:
     * RAY_HEAP_SWAP override → TMPDIR → /tmp.  /tmp is the default rather
     * than the working directory, which may be read-only or shared (the old
     * "./" default littered CWD with large sparse files or failed outright);
     * swap files carry the pid so they never collide.  A trailing slash is
     * always ensured so heap_add_pool can concatenate `<swap_path><filename>`
     * unconditionally.  An empty / over-long value is rejected in favour of
     * the next option. */
    const char* env = getenv("RAY_HEAP_SWAP");
    if (!(env && *env)) env = getenv("TMPDIR");
    const char* sp = (env && *env && strlen(env) < sizeof(h->swap_path) - 16) ? env : "/tmp";
    size_t sp_len = strlen(sp);
    memcpy(h->swap_path, sp, sp_len);
    h->swap_path[sp_len] = '\0';
    if (sp_len > 0 && h->swap_path[sp_len - 1] != '/' && sp_len < sizeof(h->swap_path) - 1) {
        h->swap_path[sp_len] = '/';
        h->swap_path[sp_len + 1] = '\0';
    }

    ray_tl_heap = h;
}

/* Forward decl — released before this heap's pools are munmap'd so a
 * stale thread-local trace doesn't survive the destroy and crash a
 * subsequent ray_eval_str → ray_clear_error_trace call when the next
 * heap takes over.  Lives in src/lang/eval.c. */
extern void ray_clear_error_trace(void);

void ray_heap_destroy(void) {
    ray_heap_t* h = ray_tl_heap;
    if (!h) return;

    /* Drop any thread-local refs into THIS heap's memory before we
     * unmap its pools.  Ordering matters: release while the backing
     * memory is still mapped; once the for-loop below munmap's the
     * pools, dereferencing g_error_trace becomes UB. */
    ray_clear_error_trace();

    /* FILE sym-domain string atoms used to live on the per-thread buddy
     * heap, requiring a drop here so the process-global cache never handed
     * out a domain backed by this heap's freed memory.  They now live in a
     * per-domain malloc arena (RAY_ATTR_ARENA, thread-independent), so the
     * cache safely outlives any heap teardown — no drop needed. */

    uint16_t saved_id = h->id;

    /* Workers tear down CONCURRENTLY during ray_pool_free() (all woken by the
     * same shutdown signal), so the unregister + the cross-heap foreign-list
     * purge below must be serialized: otherwise two destroyers splice the same
     * neighbour's ->foreign list at once and the ->fl_next walk SEGVs.  Held
     * only across the registry critical section, released before munmap. */
    ray_registry_lock();

    /* Unregister from global heap registry */
    ray_heap_registry[h->id % RAY_HEAP_REGISTRY_SIZE] = NULL;

    /* Skip flush_slabs and flush_foreign — all pools are about to be
     * munmap'd. Flushing would coalesce blocks and fl_remove buddies
     * from other heaps' freelists, which races with concurrent worker
     * destruction during ray_pool_free(). */

    /* Purge any of h's blocks from every other heap's foreign list
     * BEFORE we munmap.  Without this, foreign lists outlive h with
     * dangling pointers into unmapped memory and crash on the next
     * ray_heap_gc has_foreign walk or heap_flush_foreign. */
    for (int fh_id = 0; fh_id < RAY_HEAP_REGISTRY_SIZE; fh_id++) {
        ray_heap_t* fh_heap = ray_heap_registry[fh_id];
        if (!fh_heap || fh_heap == h) continue;
        ray_t** pp = &fh_heap->foreign;
        ray_t* curr = *pp;
        while (curr) {
            ray_t* next = curr->fl_next;
            bool in_h = false;
            for (uint32_t i = 0; i < h->pool_count; i++) {
                uintptr_t pb = (uintptr_t)h->pools[i].base;
                uintptr_t pe = pb + BSIZEOF(h->pools[i].pool_order);
                if ((uintptr_t)curr >= pb && (uintptr_t)curr < pe) {
                    in_h = true;
                    break;
                }
            }
            if (in_h) {
                *pp = next;
            } else {
                pp = &curr->fl_next;
            }
            curr = next;
        }
    }

    ray_registry_unlock();

    /* Munmap all tracked pools.  File-backed pools also need their fd
     * closed and their tempfile unlinked so the swap directory doesn't
     * accumulate orphans. */
    for (uint32_t i = 0; i < h->pool_count; i++) {
        ray_pool_hdr_t* hdr = (ray_pool_hdr_t*)h->pools[i].base;
        dfd_purge_range((uintptr_t)h->pools[i].base,
                        (uintptr_t)h->pools[i].base + BSIZEOF(h->pools[i].pool_order));
        ray_vm_free(hdr->vm_base, BSIZEOF(h->pools[i].pool_order));
        if (h->pools[i].backed) {
            if (h->pools[i].swap_fd >= 0) close(h->pools[i].swap_fd);
            if (h->pools[i].swap_path) {
                unlink(h->pools[i].swap_path);
                ray_sys_free(h->pools[i].swap_path);
            }
        } else {
            atomic_fetch_sub_explicit(&g_anon_committed,
                (int64_t)BSIZEOF(h->pools[i].pool_order), memory_order_relaxed);
        }
    }

    size_t heap_sz = (sizeof(ray_heap_t) + 4095) & ~(size_t)4095;
    ray_vm_free(h, heap_sz);
    ray_tl_heap = NULL;

    /* Release bitmap ID after all memory is freed */
    heap_id_release(saved_id);
}

/* --------------------------------------------------------------------------
 * Return worker-pool blocks from this heap's freelists to their owners.
 *
 * After ray_alloc flushes foreign blocks locally (coalesce + madvise),
 * worker-pool blocks sit on main's freelists with released physical pages.
 * This function walks the freelists, finds blocks whose pool header
 * heap_id != ours, removes them, and inserts into the owning worker heap.
 * Workers can then reuse their pools without allocating new ones.
 *
 * ONLY safe when workers are idle (on semaphore, ray_parallel_flag == 0).
 * -------------------------------------------------------------------------- */

static void heap_return_foreign_freelist(ray_heap_t* h) {
    /* avail bit (set on insert, cleared on remove) tells us which
     * freelist orders have any blocks at all — skip the empty ones. */
    if (!h->avail) return;
    for (int order = RAY_ORDER_MIN; order < RAY_HEAP_FL_SIZE; order++) {
        if (!(h->avail & (1ULL << order))) continue;
        ray_fl_head_t* head = &h->freelist[order];
        ray_t* blk = head->fl_next;
        while (blk != (ray_t*)head) {
            ray_t* next = blk->fl_next;
            /* Use heap_find_pool on h first — if found, block is local */
            int pidx = heap_find_pool(h, blk);
            if (pidx < 0) {
                /* Foreign block — find owner via pool header (GC path) */
                ray_pool_hdr_t* phdr = ray_pool_of(blk);
                if (!phdr) { blk = next; continue; }
                ray_heap_t* owner = ray_heap_registry[phdr->heap_id % RAY_HEAP_REGISTRY_SIZE];
                if (owner && owner->id == phdr->heap_id) {
                    fl_remove(blk);
                    dfd_remove(blk);
                    if (fl_empty(head))
                        h->avail &= ~(1ULL << order);
                    /* Coalesce on owner for defragmentation */
                    int opidx = heap_find_pool(owner, blk);
                    uintptr_t pb;
                    uint8_t po;
                    if (opidx >= 0) {
                        pb = (uintptr_t)owner->pools[opidx].base;
                        po = owner->pools[opidx].pool_order;
                    } else {
                        pb = (uintptr_t)phdr;
                        po = phdr->pool_order;
                    }
                    heap_coalesce(owner, blk, pb, po);
                }
            }
            blk = next;
        }
    }
}

#ifdef DEBUG
static void dfd_validate_freelists(void) {
    if (!dfd_enabled()) return;
    for (int hid = 0; hid < RAY_HEAP_REGISTRY_SIZE; hid++) {
        ray_heap_t* gh = ray_heap_registry[hid];
        if (!gh) continue;
        for (int ord = RAY_ORDER_MIN; ord < RAY_HEAP_FL_SIZE; ord++) {
            ray_fl_head_t* fh = &gh->freelist[ord];
            ray_t* blk = fh->fl_next;
            int64_t steps = 0;
            while (blk != (ray_t*)fh) {
                if (!blk) {
                    fprintf(stderr, "\n=== DFD: NULL link in heap %u freelist order %d ===\n",
                            gh->id, ord);
                    dfd_report("freelist NULL link", NULL);
                }
                if (++steps > (int64_t)1e8) {
                    fprintf(stderr, "\n=== DFD: cycle in heap %u freelist order %d ===\n",
                            gh->id, ord);
                    dfd_report("freelist cycle", blk);
                }
                blk = blk->fl_next;
            }
        }
    }
}
#endif

void ray_heap_gc(void) {
    ray_heap_t* h = ray_tl_heap;
    if (!h) return;
#ifdef DEBUG
    dfd_validate_freelists();
#endif

    bool safe = (atomic_load_explicit(&ray_parallel_flag, memory_order_relaxed) == 0);

    /* Pass 1: Flush main heap's foreign blocks and slab caches.
     * When safe (workers idle), return foreign blocks to their owners
     * so worker pools become reusable. */
    heap_flush_foreign(h, safe);
    heap_flush_slabs(h);

    if (safe) {
        /* Pass 2: Return foreign blocks absorbed onto our freelists
         * back to their owning worker heaps. */
        heap_return_foreign_freelist(h);

        /* Pass 3: Skip worker heaps — we cannot safely touch their
         * foreign lists or slab caches because workers may still be
         * between pending-- and sem_wait, calling ray_free which
         * modifies wh->foreign and wh->slabs.  Workers flush their
         * own foreign/slabs on their next dispatch entry.
         * TODO: full cross-heap reclamation requires a worker
         * quiescence barrier. */

        /* Pass 4: Reclaim OVERSIZED empty pools.
         * Standard pools (pool_order == RAY_HEAP_POOL_ORDER) are never
         * munmapped — physical pages released via madvise (phase 5)
         * re-fault cheaply on next query.  (On THP-backed pools, phase 5
         * only releases the 2MB-aligned interior of a free block, so
         * sub-2MB free blocks keep their pages resident — the deliberate
         * no-shatter tradeoff; disable per-deployment with RAY_HEAP_HUGEPAGE=0.)
         * Only oversized pools (pool_order > RAY_HEAP_POOL_ORDER) are
         * candidates — these are one-off large allocations.
         *
         * Emptiness is computed by walking all heaps' freelists and slab
         * caches to sum free capacity within the pool. This avoids atomic
         * live_count operations on the alloc/free hot path. */
        /* Pass 4: Reclaim oversized empty pools.
         *
         * For each candidate pool (owned by heap gh), count free bytes from:
         *   (a) gh's own freelist + slab cache — safe, only gh modifies these
         *   (b) ALL heaps' foreign lists (read-only) — foreign lists are
         *       prepend-only during the race window, so a read-only walk
         *       sees a consistent suffix. A concurrent prepend may be
         *       missed, making us undercount — which is conservative.
         *
         * On removal, only unlink from gh's freelist/slabs. Blocks still
         * in other heaps' foreign lists will be discovered as dangling on
         * their next flush (foreign block with unmapped pool → ray_pool_of
         * returns NULL → skipped by the NULL guard). */
        for (int hid = 0; hid < RAY_HEAP_REGISTRY_SIZE; hid++) {
            ray_heap_t* gh = ray_heap_registry[hid];
            if (!gh) continue;

            for (uint32_t p = 0; p < gh->pool_count; ) {
                ray_pool_hdr_t* phdr = (ray_pool_hdr_t*)gh->pools[p].base;

                /* Skip standard pools and last-remaining pool */
                if (phdr->pool_order <= RAY_HEAP_POOL_ORDER
                    || gh->pool_count <= 1) {
                    p++;
                    continue;
                }

                uint8_t po = phdr->pool_order;
                uintptr_t pb = (uintptr_t)phdr;
                uintptr_t pe = pb + BSIZEOF(po);
                size_t pool_capacity = BSIZEOF(po) - BSIZEOF(RAY_ORDER_MIN);

                /* (a) Sum free bytes from owning heap's freelist + slabs */
                size_t free_bytes = 0;
                for (int ord = RAY_ORDER_MIN; ord < RAY_HEAP_FL_SIZE; ord++) {
                    ray_fl_head_t* fh = &gh->freelist[ord];
                    ray_t* blk = fh->fl_next;
                    while (blk != (ray_t*)fh) {
                        if ((uintptr_t)blk >= pb && (uintptr_t)blk < pe)
                            free_bytes += BSIZEOF(ord);
                        blk = blk->fl_next;
                    }
                }
                for (int si = 0; si < RAY_SLAB_ORDERS; si++) {
                    for (uint32_t j = 0; j < gh->slabs[si].count; j++) {
                        ray_t* sb = gh->slabs[si].stack[j];
                        if ((uintptr_t)sb >= pb && (uintptr_t)sb < pe)
                            free_bytes += BSIZEOF(RAY_SLAB_MIN + si);
                    }
                }

                /* (b) Check if ANY blocks from this pool are still in other
                 *     heaps' foreign lists.  If so, we cannot munmap —
                 *     those blocks are threaded into the foreign list and
                 *     dereferencing them after munmap would crash.
                 *     They'll be flushed to the owner on the next GC. */
                /* Lock the foreign-list scan + purge: keeps the registry-wide
                 * foreign-splice invariant uniform with ray_heap_destroy.
                 * Workers are idle here (ray_parallel_flag == 0), so this is
                 * defensive — never contended in the live GC path. */
                ray_registry_lock();
                bool has_foreign = false;
                for (int fh_id = 0; fh_id < RAY_HEAP_REGISTRY_SIZE && !has_foreign; fh_id++) {
                    ray_heap_t* fh_heap = ray_heap_registry[fh_id];
                    if (!fh_heap || fh_heap == gh) continue;
                    ray_t* fb = fh_heap->foreign;
                    while (fb) {
                        if ((uintptr_t)fb >= pb && (uintptr_t)fb < pe) {
                            has_foreign = true;
                            break;
                        }
                        fb = fb->fl_next;
                    }
                }

                if (free_bytes < pool_capacity || has_foreign) {
                    ray_registry_unlock();
                    p++;
                    continue;  /* pool has live allocations or dangling foreign refs */
                }

                /* Pool is empty and no foreign-list refs — safe to munmap.
                 * Remove blocks from owning heap's freelists and slab caches. */
                for (int ord = RAY_ORDER_MIN; ord < RAY_HEAP_FL_SIZE; ord++) {
                    ray_fl_head_t* fh = &gh->freelist[ord];
                    ray_t* blk = fh->fl_next;
                    while (blk != (ray_t*)fh) {
                        ray_t* next = blk->fl_next;
                        if ((uintptr_t)blk >= pb && (uintptr_t)blk < pe) {
                            fl_remove(blk);
                            dfd_remove(blk);
                            if (fl_empty(fh))
                                gh->avail &= ~(1ULL << ord);
                        }
                        blk = next;
                    }
                }
                for (int si = 0; si < RAY_SLAB_ORDERS; si++) {
                    uint32_t dst = 0;
                    for (uint32_t j = 0; j < gh->slabs[si].count; j++) {
                        ray_t* sb = gh->slabs[si].stack[j];
                        if ((uintptr_t)sb >= pb && (uintptr_t)sb < pe) {
                            dfd_remove(sb);
                            continue;
                        }
                        gh->slabs[si].stack[dst++] = sb;
                    }
                    gh->slabs[si].count = dst;
                }

                /* Purge any [pb, pe) blocks from every other heap's foreign
                 * list BEFORE munmap.  The has_foreign check above is racy
                 * vs concurrent ray_free, and any block left here becomes
                 * a dangling pointer that crashes subsequent Pass 4 walks
                 * at fb->fl_next.  Reading fl_next is safe here because
                 * the pool is still mapped. */
                for (int fh_id = 0; fh_id < RAY_HEAP_REGISTRY_SIZE; fh_id++) {
                    ray_heap_t* fh_heap = ray_heap_registry[fh_id];
                    if (!fh_heap || fh_heap == gh) continue;
                    ray_t** pp = &fh_heap->foreign;
                    ray_t* curr = *pp;
                    while (curr) {
                        ray_t* next = curr->fl_next;
                        if ((uintptr_t)curr >= pb && (uintptr_t)curr < pe) {
                            *pp = next;
                        } else {
                            pp = &curr->fl_next;
                        }
                        curr = next;
                    }
                }
                ray_registry_unlock();

                dfd_purge_range(pb, pe);
                ray_vm_free(phdr->vm_base, BSIZEOF(po));
                /* File-backed pools also need their fd closed and tempfile
                 * unlinked, mirroring the heap_destroy path. */
                if (gh->pools[p].backed) {
                    if (gh->pools[p].swap_fd >= 0) close(gh->pools[p].swap_fd);
                    if (gh->pools[p].swap_path) {
                        unlink(gh->pools[p].swap_path);
                        ray_sys_free(gh->pools[p].swap_path);
                    }
                } else {
                    atomic_fetch_sub_explicit(&g_anon_committed,
                        (int64_t)BSIZEOF(po), memory_order_relaxed);
                }
                gh->pools[p] = gh->pools[--gh->pool_count];
                /* Don't increment p — check swapped entry */
            }
        }

        /* Pass 5: Release physical pages from free blocks in every
         * idle heap.  Pass 2 may have returned blocks to worker-owned
         * freelists; releasing only the caller heap leaves those worker
         * pages resident across large query repetitions.
         *
         * Use each heap's avail bitmap (set on insert, cleared on
         * remove) to skip the entire walk when no order >= 13 has any
         * free block.  Tiny-query workloads — where the per-statement
         * GC fires before any large allocation has been freed —
         * complete pass 5 without entering the body. */
        /* Aging: pages are released only after a block SURVIVES several
         * pass-5 VISITS on a freelist.  This routine runs per statement
         * and per parallel_end, so the per-query temporaries of a
         * repeated query used to be MADV_DONTNEEDed between every two
         * executions and refaulted from scratch — kernel page-fault
         * frames dominated repeated-query profiles while the freed block
         * was reused within milliseconds.  A block re-allocated before
         * reaching RAY_FREE_AGE_RELEASE keeps its pages; genuinely idle
         * memory still returns to the OS after a few maintenance points.
         * The released marker also stops re-madvising the same idle
         * block on every subsequent pass.
         *
         * INCREMENTAL CURSOR — bounded work at every maintenance point
         * (the design contract: heap maintenance must never turn into an
         * unpredictable pause).  The walk resumes at the (heap, order)
         * slot where the previous pass stopped and is capped by BOTH a
         * block-visit budget and a release budget: each madvise batch
         * costs cross-core TLB-shootdown IPIs, so releases are the
         * expensive unit, not visits.  The cursor advances BEFORE a slot
         * is processed, so a list interrupted mid-walk simply waits one
         * full cycle instead of being double-aged; the cursor never
         * stores a block pointer — blocks may be reallocated, split or
         * coalesced between passes, only (heap, order) is stable. */
        #define RAY_FREE_AGE_RELEASE   3
        #define RAY_P5_VISIT_BUDGET    2048
        #define RAY_P5_RELEASE_BUDGET  64
        static int s_p5_hid = 0;
        static int s_p5_ord = 13;
        uint64_t large_orders_mask = ~((1ULL << 13) - 1);
        int64_t p5_visited = 0, p5_released = 0;
        int p5_slots = RAY_HEAP_REGISTRY_SIZE * (RAY_HEAP_FL_SIZE - 13);
        for (int step = 0; step < p5_slots; step++) {
            if (p5_visited >= RAY_P5_VISIT_BUDGET ||
                p5_released >= RAY_P5_RELEASE_BUDGET) break;
            int hid = s_p5_hid;
            int ord = s_p5_ord;
            if (++s_p5_ord >= RAY_HEAP_FL_SIZE) {
                s_p5_ord = 13;
                s_p5_hid = (s_p5_hid + 1) % RAY_HEAP_REGISTRY_SIZE;
            }
            ray_heap_t* gh = ray_heap_registry[hid];
            if (!gh) continue;
            if (!((gh->avail & large_orders_mask) & (1ULL << ord))) continue;
            ray_fl_head_t* head = &gh->freelist[ord];
            for (ray_t* blk = head->fl_next; blk != (ray_t*)head;
                 blk = blk->fl_next) {
                if (p5_visited++ >= RAY_P5_VISIT_BUDGET) break;
                if (blk->attrs < RAY_FREE_AGE_RELEASE) {
                    blk->attrs++;
                } else if (blk->attrs == RAY_FREE_AGE_RELEASE) {
                    size_t bsize = BSIZEOF(ord);
                    int rpidx = heap_find_pool(gh, blk);
                    bool hp = (rpidx >= 0) ? (gh->pools[rpidx].hugepage != 0)
                                           : false;
                    ray_vm_release_block(blk, bsize, hp);
                    blk->attrs = RAY_FREE_AGE_RELEASE + 1;  /* released */
                    if (++p5_released >= RAY_P5_RELEASE_BUDGET) break;
                }
            }
        }
    }

}

void ray_heap_release_pages(void) {
    /* Explicit release: callers asking for pages back get them NOW —
     * no aging — but still skip blocks already released. */
    ray_heap_t* h = ray_tl_heap;
    if (!h) return;
    for (int i = 13; i < RAY_HEAP_FL_SIZE; i++) {
        ray_fl_head_t* head = &h->freelist[i];
        ray_t* blk = head->fl_next;
        while (blk != (ray_t*)head) {
            if (blk->attrs <= RAY_FREE_AGE_RELEASE) {
                size_t bsize = BSIZEOF(i);
                int rpidx = heap_find_pool(h, blk);
                bool hp = (rpidx >= 0) ? (h->pools[rpidx].hugepage != 0) : false;
                ray_vm_release_block(blk, bsize, hp);
                blk->attrs = RAY_FREE_AGE_RELEASE + 1;
            }
            blk = blk->fl_next;
        }
    }
}

void ray_heap_merge(ray_heap_t* src) {
    ray_heap_t* dst = ray_tl_heap;
    if (!dst || !src) return;

    /* Merge stats: dst inherits src's outstanding allocations so that
     * future local frees of those blocks correctly decrement dst. */
    dst->stats.alloc_count     += src->stats.alloc_count;
    dst->stats.free_count      += src->stats.free_count;
    dst->stats.bytes_allocated += src->stats.bytes_allocated;
    dst->stats.slab_hits       += src->stats.slab_hits;
    dst->stats.direct_count    += src->stats.direct_count;
    dst->stats.direct_bytes    += src->stats.direct_bytes;
    if (src->stats.peak_bytes > dst->stats.peak_bytes)
        dst->stats.peak_bytes = src->stats.peak_bytes;

    /* Transfer slabs: fit into dst cache, coalesce overflow */
    for (int i = 0; i < RAY_SLAB_ORDERS; i++) {
        while (src->slabs[i].count > 0 && dst->slabs[i].count < dst->slab_cap[i])
            dst->slabs[i].stack[dst->slabs[i].count++] =
                src->slabs[i].stack[--src->slabs[i].count];
        while (src->slabs[i].count > 0) {
            ray_t* blk = src->slabs[i].stack[--src->slabs[i].count];
            int pidx = heap_find_pool(dst, blk);
            uintptr_t pb;
            uint8_t po;
            if (pidx >= 0) {
                pb = (uintptr_t)dst->pools[pidx].base;
                po = dst->pools[pidx].pool_order;
            } else {
                ray_pool_hdr_t* phdr = ray_pool_of(blk);
                if (!phdr) continue;
                pb = (uintptr_t)phdr;
                po = phdr->pool_order;
            }
            heap_coalesce(dst, blk, pb, po);
        }
    }

    /* Free foreign blocks via coalescing */
    ray_t* fblk = src->foreign;
    while (fblk) {
        ray_t* next = fblk->fl_next;
        int pidx = heap_find_pool(dst, fblk);
        uintptr_t pb;
        uint8_t po;
        if (pidx >= 0) {
            pb = (uintptr_t)dst->pools[pidx].base;
            po = dst->pools[pidx].pool_order;
        } else {
            ray_pool_hdr_t* phdr = ray_pool_of(fblk);
            if (!phdr) { fblk = next; continue; }
            pb = (uintptr_t)phdr;
            po = phdr->pool_order;
        }
        heap_coalesce(dst, fblk, pb, po);
        fblk = next;
    }
    src->foreign = NULL;

    /* Merge freelists: circular list splice (src chain into dst chain) */
    for (int i = RAY_ORDER_MIN; i < RAY_HEAP_FL_SIZE; i++) {
        if (fl_empty(&src->freelist[i])) continue;

        ray_fl_head_t* src_head = &src->freelist[i];
        ray_fl_head_t* dst_head = &dst->freelist[i];

        /* Splice: src's chain [src_first...src_last] into dst after sentinel */
        ray_t* src_first = src_head->fl_next;
        ray_t* src_last  = src_head->fl_prev;
        ray_t* dst_first = dst_head->fl_next;

        /* src_first goes after dst sentinel */
        dst_head->fl_next = src_first;
        src_first->fl_prev = (ray_t*)dst_head;

        /* src_last connects to old dst_first */
        src_last->fl_next = dst_first;
        dst_first->fl_prev = src_last;

        dst->avail |= (1ULL << i);

        /* Reset src sentinel to empty */
        fl_init(src_head);
    }

    src->avail = 0;

    /* Update pool headers: set heap_id to dst, transfer pool entries.
     * Do NOT rewrite heap_id for pools that can't be tracked — that would
     * make coalescing reference a pool not in dst's pool table. */
    for (uint32_t i = 0; i < src->pool_count; i++) {
        if (dst->pool_count < RAY_MAX_POOLS) {
            ray_pool_hdr_t* hdr = (ray_pool_hdr_t*)src->pools[i].base;
            hdr->heap_id = dst->id;
            dst->pools[dst->pool_count++] = src->pools[i];
        } else {
            /* Pool overflow: only triggers at RAY_MAX_POOLS (512 pools = 16GB+).
             * Fix ownership so blocks free to the correct heap. */
            ray_pool_hdr_t* hdr = (ray_pool_hdr_t*)src->pools[i].base;
            hdr->heap_id = dst->id;
            assert(0 && "ray_heap_merge: pool overflow at RAY_MAX_POOLS");
        }
    }
    src->pool_count = 0;
}

/* --------------------------------------------------------------------------
 * Public foreign-blocks flush
 * -------------------------------------------------------------------------- */

void ray_heap_flush_foreign(void) {
    ray_heap_t* h = ray_tl_heap;
    if (!h) return;
    bool safe = (atomic_load_explicit(&ray_parallel_flag,
                                       memory_order_relaxed) == 0);
    heap_flush_foreign(h, safe);
}

/* --------------------------------------------------------------------------
 * Pending-merge queue (lock-free LIFO)
 *
 * Workers that are torn down push their heap onto this queue instead of
 * destroying it immediately. The main thread drains the queue, merging
 * each pending heap into its own and then destroying it.
 * -------------------------------------------------------------------------- */

void ray_heap_push_pending(ray_heap_t* heap) {
    if (!heap) return;
    /* Unregister so no new foreign blocks target this heap (lock: serialize
     * against concurrent destroyers walking the registry). */
    ray_registry_lock();
    ray_heap_registry[heap->id % RAY_HEAP_REGISTRY_SIZE] = NULL;
    ray_registry_unlock();
    /* Lock-free push: CAS loop on global LIFO head */
    heap->pending_next = atomic_load_explicit(&ray_heap_pending_merge, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(
            &ray_heap_pending_merge,
            &heap->pending_next, heap,
            memory_order_release, memory_order_relaxed))
        ;
}

void ray_heap_drain_pending(void) {
    /* Atomically steal the entire pending list */
    ray_heap_t* pending = atomic_exchange_explicit(
        &ray_heap_pending_merge, NULL,
        memory_order_acquire);
    while (pending) {
        ray_heap_t* next = pending->pending_next;
        ray_heap_merge(pending);
        /* Free the heap struct (pools already transferred by merge) */
        uint16_t saved_id = pending->id;
        size_t heap_sz = (sizeof(ray_heap_t) + 4095) & ~(size_t)4095;
        ray_vm_free(pending, heap_sz);
        heap_id_release(saved_id);
        pending = next;
    }
}

/* --------------------------------------------------------------------------
 * Scratch arena: bump allocator backed by buddy-allocated 64KB blocks
 * -------------------------------------------------------------------------- */

void* ray_scratch_arena_push(ray_scratch_arena_t* a, size_t nbytes) {
    /* 16-byte alignment */
    nbytes = (nbytes + 15) & ~(size_t)15;

    if (RAY_LIKELY(a->ptr != NULL && a->ptr + nbytes <= a->end))
        goto bump;

    /* Need a new backing block */
    if (a->n_backing >= RAY_ARENA_MAX_BACKING) return NULL;

    size_t block_data = BSIZEOF(RAY_ARENA_BLOCK_ORDER) - 32;
    /* If request exceeds standard block, allocate exact-fit */
    size_t alloc_size = nbytes > block_data ? nbytes : block_data;
    ray_t* blk = ray_alloc(alloc_size);
    if (!blk) return NULL;
    a->backing[a->n_backing++] = blk;
    a->ptr = (char*)ray_data(blk);
    a->end = (char*)blk + BSIZEOF(blk->order);

bump:;
    void* ret = a->ptr;
    a->ptr += nbytes;
    return ret;
}

void ray_scratch_arena_reset(ray_scratch_arena_t* a) {
    for (int i = 0; i < a->n_backing; i++)
        ray_free(a->backing[i]);
    a->n_backing = 0;
    a->ptr = NULL;
    a->end = NULL;
}

/* --------------------------------------------------------------------------
 * Parallel begin / end
 * -------------------------------------------------------------------------- */

void ray_parallel_begin(void) { atomic_store(&ray_parallel_flag, 1); }
void ray_parallel_end(void) {
    atomic_store(&ray_parallel_flag, 0);
    ray_heap_gc();
}
