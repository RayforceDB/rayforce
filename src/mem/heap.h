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

#ifndef RAY_HEAP_H
#define RAY_HEAP_H

/*
 * heap.h -- Rayforce-style per-thread heap allocator (zero-prefix layout).
 *
 * Each thread owns one ray_heap_t. Blocks are allocated from self-aligned
 * mmap'd pools via buddy splitting. ray_t IS the block — no prefix.
 *
 * Pool metadata (heap_id, pool_order) is stored in a pool header at
 * offset 0 of each self-aligned pool (first min-block reserved).
 * Pool base is derived in O(1): ptr & ~(pool_size - 1).
 *
 * Free-list prev/next overlay nullmap bytes 0-15 of ray_t (unused when free).
 * rc == 0 indicates a free block (replaces the old ray_blk_t.used flag).
 *
 * Cross-thread free uses a foreign_blocks list (checked via pool heap_id).
 */

#include <rayforce.h>
#include "core/platform.h"
#include "ops/ops.h"
#include <stdint.h>

/* ===== Attribute Flags =====
 *
 * The `attrs` byte in ray_t is type-namespaced: the same bit positions carry
 * different meanings depending on the object's type tag.
 *
 *   Bits 0x01-0x03  RAY_SYM vectors:  sym index width (RAY_SYM_W8/W16/W32/W64)
 *   Bits 0x01-0x10  function objects (RAY_UNARY/BINARY/VARY): RAY_FN_* flags
 *   Bit  0x04       -RAY_I64 atoms:  RAY_ATTR_HNSW (HNSW handle in .i64)
 *   Bit  0x08       vectors:         RAY_ATTR_HAS_INDEX (index ray_t* in nullmap[0..7])
 *   Bit  0x10       vectors:         RAY_ATTR_SLICE
 *   Bit  0x20       vectors:         RAY_ATTR_NULLMAP_EXT
 *   Bit  0x20       -RAY_SYM:        RAY_ATTR_NAME (variable reference)
 *   Bit  0x40       vectors:         RAY_ATTR_HAS_NULLS
 *   Bit  0x80       all types:       RAY_ATTR_ARENA (arena-allocated, no refcount)
 *
 * Overlapping bit values are safe because consumers always check the type tag
 * before interpreting attrs.
 */

#ifndef RAY_ATTR_SLICE
#define RAY_ATTR_SLICE        0x10
#endif
#define RAY_ATTR_NULLMAP_EXT  0x20
#define RAY_ATTR_HAS_NULLS    0x40
#define RAY_ATTR_ARENA        0x80

/* I64 atom carries an owning ray_hnsw_t* in its .i64 slot.
 * Checked by HNSW builtins before dereferencing.  User must (hnsw-free h). */
#define RAY_ATTR_HNSW         0x04

/* Vector is a linked column.  The 8 bytes of the nullmap union at offset
 * 8 (i.e. parent->_idx_pad / parent->slice_offset / parent->sym_dict /
 * parent->str_pool slot, depending on which arm is in use) hold an int64
 * sym ID naming the target table.  Resolved against the global env at
 * deref time.  Restricted to RAY_I32 / RAY_I64 vectors — STR/SYM/SLICE
 * already use bytes 8-15 for their own pointers/data so HAS_LINK on
 * those types would alias.
 *
 * Coexists with HAS_INDEX: bytes 0-7 carry the index pointer (or saved
 * nullmap), bytes 8-15 carry the link sym; both bits can be set on the
 * same column.  A linked vec with nulls is forced to RAY_ATTR_NULLMAP_EXT
 * because the inline 128-bit bitmap would alias the link-target slot.
 *
 * Same numeric value as RAY_ATTR_HNSW (HNSW handles are -RAY_I64 atoms,
 * the type tag disambiguates). */
#define RAY_ATTR_HAS_LINK     0x04

/* Vector carries an attached accelerator index in nullmap[0..7] (a ray_t*
 * of type RAY_INDEX).  The original 16-byte nullmap union content (inline
 * bitmap, ext_nullmap, str_ext_null/str_pool, sym_dict) is preserved inside
 * the index ray_t and restored on detach.
 *
 * Attribute-bit invariant when HAS_INDEX is set:
 *   - HAS_NULLS is *preserved* (not cleared).  Many call sites use it as a
 *     cheap "do I need null-aware logic?" gate; clearing it would silently
 *     break correctness for nullable columns.  The bit is authoritative.
 *   - NULLMAP_EXT is *cleared*.  The parent's ext_nullmap field is now the
 *     index pointer, not a U8 bitmap vec; readers that gate on NULLMAP_EXT
 *     and dereference ext_nullmap directly would otherwise read garbage.
 *     The displaced ext-nullmap pointer (if any) lives in
 *     ix->saved_nullmap[0..7]; ix->saved_attrs records the original
 *     NULLMAP_EXT bit for restoration on detach.
 *
 * Direct nullmap-byte readers (morsel iteration, ray_vec_is_null) MUST
 * check HAS_INDEX first and route through ix->saved_nullmap / saved_attrs.
 * See src/ops/idxop.h. */
#define RAY_ATTR_HAS_INDEX    0x08

/* ===== Internal Allocator Variants ===== */

ray_t*    ray_alloc_copy(ray_t* v);
ray_t*    ray_scratch_alloc(size_t data_size);
ray_t*    ray_scratch_realloc(ray_t* v, size_t new_data_size);

/* ===== COW (Copy-on-Write) ===== */

ray_t*    ray_cow(ray_t* v);

/* ===== Memory Statistics ===== */

typedef struct {
    size_t alloc_count;      /* ray_alloc calls */
    size_t free_count;       /* ray_free calls */
    size_t bytes_allocated;  /* currently allocated */
    size_t peak_bytes;       /* high-water mark */
    size_t slab_hits;        /* slab cache hits */
    size_t direct_count;     /* active direct mmaps */
    size_t direct_bytes;     /* bytes in direct mmaps */
    size_t sys_current;      /* sys allocator: current mmap'd bytes */
    size_t sys_peak;         /* sys allocator: peak mmap'd bytes */
} ray_mem_stats_t;

/* ===== Forward Declarations (internal types) ===== */

typedef struct ray_heap      ray_heap_t;
typedef struct ray_sym_table ray_sym_table_t;
typedef struct ray_sym_map   ray_sym_map_t;
typedef struct ray_task      ray_task_t;
typedef struct ray_dispatch  ray_dispatch_t;

/* ===== Heap Lifecycle ===== */

void     ray_heap_init(void);
void     ray_heap_destroy(void);
void     ray_heap_merge(ray_heap_t* src);
void     ray_heap_flush_foreign(void);
void     ray_heap_push_pending(ray_heap_t* heap);
void     ray_heap_drain_pending(void);
uint8_t  ray_order_for_size(size_t data_size);
void     ray_mem_stats(ray_mem_stats_t* out);

void ray_heap_gc(void);
void ray_heap_release_pages(void);

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define RAY_HEAP_POOL_ORDER  25      /* 32 MB standard pool */
#define RAY_HEAP_MAX_ORDER   38      /* 256 GB max pool */
#define RAY_HEAP_FL_SIZE     (RAY_HEAP_MAX_ORDER + 1)
#define RAY_MAX_POOLS        512

/* --------------------------------------------------------------------------
 * Block size helper
 * -------------------------------------------------------------------------- */
#define BSIZEOF(o)    ((size_t)1 << (o))

/* --------------------------------------------------------------------------
 * Pool header: first min-block (64B) of each self-aligned pool.
 *
 * Overlaid on bytes 0-15 of the ray_t at pool offset 0.
 * The ray_t at pool offset 0 has rc=1 (prevents coalescing) and
 * order=RAY_ORDER_MIN (correct for buddy math).
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t heap_id;     /* owning heap ID (for cross-thread free) */
    uint8_t  pool_order;  /* pool's top order */
    uint8_t  _pad[5];
    void*    vm_base;     /* original mmap base (for ray_vm_free on Windows) */
} ray_pool_hdr_t;

_Static_assert(sizeof(ray_pool_hdr_t) <= 16,
               "ray_pool_hdr_t must fit in ray_t nullmap (16 bytes)");

/* --------------------------------------------------------------------------
 * Circular sentinel freelist (Rayforce-style)
 *
 * Each freelist[order] is a sentinel node with prev/next pointers at
 * offsets 0/8 — same layout as ray_t.fl_prev/fl_next. This makes
 * fl_remove() work without knowing which freelist the block belongs to,
 * enabling safe cross-heap buddy coalescing.
 *
 * Empty list: sentinel.prev = sentinel.next = &sentinel.
 * -------------------------------------------------------------------------- */
typedef struct RAY_ALIGN(32) {
    ray_t* fl_prev;   /* offset 0 — same as ray_t.fl_prev */
    ray_t* fl_next;   /* offset 8 — same as ray_t.fl_next */
} ray_fl_head_t;

static inline void fl_init(ray_fl_head_t* h) {
    h->fl_prev = (ray_t*)h;
    h->fl_next = (ray_t*)h;
}

static inline bool fl_empty(const ray_fl_head_t* h) {
    return h->fl_next == (const ray_t*)h;
}

/* Unlink a block from whatever circular list it belongs to.
 * Works across heaps — no head pointer needed. */
static inline void fl_remove(ray_t* blk) {
    blk->fl_prev->fl_next = blk->fl_next;
    blk->fl_next->fl_prev = blk->fl_prev;
}

/* --------------------------------------------------------------------------
 * Pool tracking entry (in ray_heap_t)
 *
 * Pools are normally backed by anonymous mmap.  When anon mmap fails (the
 * OS refuses an N-byte allocation because RAM+swap can't satisfy it), the
 * allocator falls back to a file-backed mmap pointed at a tempfile in the
 * heap's swap directory — this lets fresh allocations exceed RAM, with
 * dirty pages flushed to disk by the kernel.
 *
 *   backed=0:  anonymous mmap (the common case).  swap_fd unused,
 *              swap_path NULL.
 *   backed=1:  file-backed mmap.  swap_fd holds the open fd and
 *              swap_path holds the absolute path; teardown closes the
 *              fd, unlinks the file, and ray_sys_frees the path string.
 * -------------------------------------------------------------------------- */
typedef struct {
    void*    base;         /* pool base address (self-aligned) */
    char*    swap_path;    /* tempfile path when backed=1; NULL otherwise (ray_sys_alloc'd) */
    int      swap_fd;      /* fd when backed=1; -1 otherwise */
    uint8_t  pool_order;   /* pool order for munmap sizing */
    uint8_t  backed;       /* 0 = anon mmap, 1 = file-backed swap */
    uint8_t  _pad[2];
} ray_pool_entry_t;

/* --------------------------------------------------------------------------
 * Pool derivation helpers
 *
 * ray_pool_of: derive pool header from any block pointer.
 *
 * All pools are self-aligned (pool base = multiple of pool_size). Standard
 * pools (32 MB) are derived in O(1) via a single AND mask. Oversized pools
 * (> 32 MB) use a downward walk at 32 MB stride to find the pool header.
 *
 * Pool header validation: order == RAY_ORDER_MIN, mmod == 0, rc == 1.
 * These conditions uniquely identify pool header blocks — cascade/split
 * blocks always have order > RAY_ORDER_MIN.
 * -------------------------------------------------------------------------- */

static inline ray_pool_hdr_t* ray_pool_of(ray_t* v) {
    /* Standard pools (32 MB, self-aligned): one AND gives the base.
     * Oversized pools need a downward walk but are rare. */
    uintptr_t stride = BSIZEOF(RAY_HEAP_POOL_ORDER);  /* 32 MB */
    uintptr_t base = (uintptr_t)v & ~(stride - 1);
    ray_pool_hdr_t* hdr = (ray_pool_hdr_t*)base;

    /* Fast path: standard pool header at 32 MB boundary (99%+ of calls) */
    if (RAY_LIKELY(hdr->pool_order == RAY_HEAP_POOL_ORDER))
        return hdr;

    /* Slow path: oversized pool — walk downward at 32 MB stride */
    if (hdr->pool_order > RAY_HEAP_POOL_ORDER &&
        hdr->pool_order <= RAY_HEAP_MAX_ORDER &&
        (uintptr_t)v < base + BSIZEOF(hdr->pool_order))
        return hdr;

    for (;;) {
        if (base < stride) break;
        base -= stride;
        hdr = (ray_pool_hdr_t*)base;
        ray_t* hdr_blk = (ray_t*)base;
        if (hdr_blk->order == RAY_ORDER_MIN &&
            hdr_blk->mmod == 0 &&
            ray_atomic_load(&hdr_blk->rc) == 1) {
            if (hdr->pool_order >= RAY_HEAP_POOL_ORDER &&
                hdr->pool_order <= RAY_HEAP_MAX_ORDER &&
                (uintptr_t)v < base + BSIZEOF(hdr->pool_order))
                return hdr;
        }
    }
    ray_pool_hdr_t* fallback = (ray_pool_hdr_t*)((uintptr_t)v & ~(stride - 1));
    if (fallback->pool_order >= RAY_HEAP_POOL_ORDER &&
        fallback->pool_order <= RAY_HEAP_MAX_ORDER)
        return fallback;
    return NULL;
}

/* --------------------------------------------------------------------------
 * Buddy derivation: uses self-aligned pool base
 * -------------------------------------------------------------------------- */
static inline ray_t* ray_buddy_of(ray_t* v, uint8_t order, uintptr_t pool_base) {
    return (ray_t*)(pool_base + (((uintptr_t)v - pool_base) ^ BSIZEOF(order)));
}

/* --------------------------------------------------------------------------
 * Slab cache for small blocks (orders 6-10, i.e., 64B-1024B)
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t count;
    ray_t*    stack[RAY_SLAB_CACHE_SIZE];
} ray_slab_t;

#define RAY_SLAB_MIN       RAY_ORDER_MIN
#define RAY_SLAB_MAX       (RAY_ORDER_MIN + RAY_SLAB_ORDERS - 1)
#define IS_SLAB_ORDER(o)  ((o) >= RAY_SLAB_MIN && (o) <= RAY_SLAB_MAX)
#define SLAB_INDEX(o)     ((o) - RAY_SLAB_MIN)

/* --------------------------------------------------------------------------
 * Per-thread heap
 * -------------------------------------------------------------------------- */
typedef struct ray_heap {
    uint64_t        avail;                       /* bitmask: bit N set = freelist[N] non-empty */
    uint16_t        id;                          /* heap identity (for cross-thread free) */
    ray_t*           foreign;                     /* cross-heap freed blocks (lock-free LIFO via fl_next) */
    ray_slab_t       slabs[RAY_SLAB_ORDERS];       /* small-block slab caches */
    ray_fl_head_t    freelist[RAY_HEAP_FL_SIZE];   /* circular sentinel per order */
    ray_mem_stats_t  stats;
    uint32_t        pool_count;                  /* number of tracked pools */
    ray_pool_entry_t pools[RAY_MAX_POOLS];         /* pool tracking for destroy/merge */
    struct ray_heap* pending_next;                /* link for pending-merge LIFO queue */
    char            swap_path[256];              /* dir for file-backed pool fallback (RAY_HEAP_SWAP env, default "./") */
} ray_heap_t;

/* --------------------------------------------------------------------------
 * Bitmap-based heap ID allocator (atomic CAS, reusable IDs)
 * -------------------------------------------------------------------------- */
#define RAY_HEAP_ID_WORDS  16   /* 16 * 64 = 1024 IDs (matches registry size) */
#define RAY_HEAP_ID_BITS   (RAY_HEAP_ID_WORDS * 64)

/* Global pending-merge queue head (lock-free LIFO) */
extern _Atomic(ray_heap_t*) ray_heap_pending_merge;

/* --------------------------------------------------------------------------
 * Pool-list scan: find which pool a block belongs to without reading the
 * remote pool header (avoids cold cache line 32MB away on hot path).
 * Returns pool index in h->pools[], or -1 if block is foreign.
 * -------------------------------------------------------------------------- */
static inline int heap_find_pool(const ray_heap_t* h, const void* ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    for (uint32_t i = 0; i < h->pool_count; i++) {
        uintptr_t pb = (uintptr_t)h->pools[i].base;
        if (addr >= pb && addr < pb + BSIZEOF(h->pools[i].pool_order))
            return (int)i;
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * Thread-local state
 * -------------------------------------------------------------------------- */
extern RAY_TLS ray_heap_t*     ray_tl_heap;

/* --------------------------------------------------------------------------
 * Global heap registry: look up any heap by ID so foreign blocks can be
 * returned to their owning heap instead of accumulating on the freeing heap.
 * -------------------------------------------------------------------------- */
#define RAY_HEAP_REGISTRY_SIZE 1024
extern ray_heap_t* ray_heap_registry[RAY_HEAP_REGISTRY_SIZE];

/* --------------------------------------------------------------------------
 * Scratch arena: bump-allocator backed by buddy-allocated pages.
 * O(1) push (pointer bump), O(n_backing) reset (free all backing blocks).
 * -------------------------------------------------------------------------- */
#define RAY_ARENA_MAX_BACKING  64
#define RAY_ARENA_BLOCK_ORDER  16   /* 64 KB backing blocks */

typedef struct {
    ray_t*   backing[RAY_ARENA_MAX_BACKING];
    int     n_backing;
    char*   ptr;
    char*   end;
} ray_scratch_arena_t;

static inline void ray_scratch_arena_init(ray_scratch_arena_t* a) {
    a->n_backing = 0;
    a->ptr = NULL;
    a->end = NULL;
}

/* Retain all child/owned refs inside a compound block (STR/LIST/TABLE/etc.).
 * Used by ray_block_copy and ray_alloc_copy after shallow-copying a block.
 *
 * Returns true on success, false if a deep-clone of a uniquely-owned
 * resource (e.g. an HNSW index) failed.  On failure, any owned state that
 * was memcpy'd into the copy has been neutralized (attr flags cleared,
 * pointers zeroed) so the caller may safely ray_free(v) without leaks or
 * double-frees. */
bool ray_retain_owned_refs(ray_t* v);

void* ray_scratch_arena_push(ray_scratch_arena_t* a, size_t nbytes);
void  ray_scratch_arena_reset(ray_scratch_arena_t* a);

#endif /* RAY_HEAP_H */
