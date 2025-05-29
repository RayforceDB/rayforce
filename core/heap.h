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

typedef struct heap_t {
    i64_t id;
    block_p freelist[MAX_POOL_ORDER + 2];  // free list of blocks by order
    i64_t avail;                           // mask of available blocks by order
    block_p foreign_blocks;                // foreign blocks (to be freed by the owner)
    block_p backed_blocks;                 // backed blocks (to be unmapped)
    memstat_t memstat;
} *heap_p;

heap_p heap_create(i64_t id);
nil_t heap_destroy(nil_t);
heap_p heap_get(nil_t);
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
