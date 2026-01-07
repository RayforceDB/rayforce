/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef HEAP_H
#define HEAP_H

#include "rayforce.h"

#define AVAIL_MASK ((u64_t)0xffffffffffffffff)
#define MIN_BLOCK_ORDER 5   // 2^5 = 32B
#define MAX_BLOCK_ORDER 25  // 2^25 = 32MB
#define MAX_POOL_ORDER 38   // 2^38 = 256GB

// Small object cache (slab) for sizes 32, 64, 128, 256, 512 bytes
#define SLAB_CACHE_SIZE 64
#define SLAB_ORDERS 5  // orders 5, 6, 7, 8, 9

// Cache line size (typically 64 bytes on x86/ARM)
#define CACHE_LINE_SIZE 64

// Memory modes
#define MMOD_INTERNAL 0xff
#define MMOD_EXTERNAL_SIMPLE 0xfd
#define MMOD_EXTERNAL_COMPOUND 0xfe
#define MMOD_EXTERNAL_SERIALIZED 0xfa

typedef struct memstat_t {
    i64_t system;  // system memory used
    i64_t heap;    // total heap memory
    i64_t free;    // free heap memory
} memstat_t;

typedef struct block_t {
    u8_t order;
    u8_t used;
    u8_t pool_order;
    u8_t mode;
    u16_t heap_id;
    b8_t backed;  // backed by a file
    b8_t pad;
    struct block_t *pool;
    struct block_t *prev;
    struct block_t *next;
} *block_p;

// Small object slab cache for fast alloc/free of common sizes
// count is FIRST - checked before stack access, avoids cache miss
typedef struct slab_cache_t {
    i64_t count;                     // current stack depth (check first!)
    block_p stack[SLAB_CACHE_SIZE];  // LIFO stack of freed blocks
} slab_cache_t;

typedef struct heap_t {
    // ===== Cache line 1: hottest fields (allocation fast path) =====
    i64_t avail;               // 8B - bitmask checked every alloc
    i64_t id;                  // 8B - heap identity checked in free
    block_p foreign_blocks;    // 8B - checked in worker free path
    block_p backed_blocks;     // 8B - file-backed blocks
    i64_t _pad_cacheline1[4];  // 32B - pad to 64B cache line

    // ===== Slab caches (count field is first in each - fast path check) =====
    slab_cache_t slabs[SLAB_ORDERS];  // small object caches for orders 5-9

    // ===== Freelists (accessed for non-slab allocations) =====
    block_p freelist[MAX_POOL_ORDER + 2];  // free list of blocks by order

    // ===== Cold fields (rarely accessed) =====
    memstat_t memstat;   // statistics
    c8_t swap_path[64];  // swap directory path (init only)
} *heap_p;

heap_p heap_create(i64_t id);
nil_t heap_destroy(heap_p heap);
heap_p heap_get(nil_t);  // Get current heap via VM
raw_p heap_mmap(i64_t size);
raw_p heap_stack(i64_t size);
raw_p heap_alloc(i64_t size);
raw_p heap_realloc(raw_p ptr, i64_t size);
nil_t heap_free(raw_p ptr);
nil_t heap_unmap(raw_p ptr, i64_t size);
i64_t heap_gc(nil_t);
nil_t heap_borrow(heap_p heap);
nil_t heap_merge(heap_p heap);
memstat_t heap_memstat(nil_t);
nil_t heap_print_blocks(heap_p heap);

#endif  // HEAP_H
