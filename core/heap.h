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
#include "symbols.h"

#define MIN_ORDER 6                   // 2^6  = 64 bytes
#define MAX_ORDER 25                  // 2^25 = 32MB
#define MAX_POOL_ORDER 36             // 2^36 = 64GB
#define POOL_SIZE (1ull << MAX_ORDER) // 32MB
#define NUM_16_BLOCKS 1024            // 1K blocks

typedef struct node_t
{
    nil_t *base; // base address of the root block (pool) + original order in 7's byte
    union
    {
        struct node_t *next; // next block in the pool
        u64_t size;          // size of the block (splitted)
    };
} node_t;

typedef struct memstat_t
{
    u64_t system; // system memory used
    u64_t heap;   // total heap memory
    u64_t free;   // free heap memory
} memstat_t;

typedef struct heap_t
{
    nil_t *blocks16;                      // pool of 16 bytes blocks
    nil_t *freelist16;                    // blocks of 16 bytes
    node_t *freelist[MAX_POOL_ORDER + 2]; // free list of blocks by order
    u64_t avail;                          // mask of available blocks by order
    memstat_t memstat;
} __attribute__((aligned(PAGE_SIZE))) * heap_p;

nil_t *heap_alloc(u64_t size);
nil_t *heap_realloc(nil_t *block, u64_t size);
nil_t heap_free(nil_t *block);
heap_p heap_init(nil_t);
heap_p heap_get(nil_t);
i64_t heap_gc(nil_t);
nil_t heap_cleanup(nil_t);
memstat_t heap_memstat(nil_t);

#endif // HEAP_H
