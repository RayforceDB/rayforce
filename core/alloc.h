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

#ifndef ALLOC_H
#define ALLOC_H

#include "rayforce.h"
#include "symbols.h"

// clang-format off
#define MIN_ORDER      4
#define MAX_ORDER      28 // 2^28 = 256MB
#define MAX_POOL_ORDER 36 // 2^36 = 64GB
#define MIN_ALLOC      (1ull << MIN_ORDER)
#define MAX_ALLOC      (1ull << MAX_ORDER)
#define POOL_SIZE      (1ull << MAX_ORDER)

typedef struct node_t
{
    null_t            *base; // base address of the root block (pool)
    union
    {
        struct node_t *next; // next block in the pool
        u64_t          size; // size of the block (splitted)
    };
} node_t;

CASSERT(sizeof(struct node_t) == 16, alloc_h)

typedef struct memstat_t
{
    u64_t total;
    u64_t used;
    u64_t free;
} memstat_t;

typedef struct alloc_t
{
    node_t *freelist[MAX_POOL_ORDER + 2]; // free list of blocks by order
    u64_t   avail;                        // mask of available blocks by order
} __attribute__((aligned(PAGE_SIZE))) * alloc_t;

CASSERT(sizeof(struct alloc_t) % PAGE_SIZE == 0, alloc_h)

extern null_t   *rf_malloc(u64_t size);
extern null_t   *rf_realloc(null_t *ptr, u64_t size);
extern null_t    rf_free(null_t *block);
extern alloc_t   rf_alloc_init();
extern alloc_t   rf_alloc_get();
extern null_t    rf_alloc_cleanup();
extern memstat_t rf_alloc_memstat();
// clang-format on

#endif
