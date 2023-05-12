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

#define MIN_ORDER 4
#define MAX_ORDER 26 // 64MB
#define MIN_ALLOC ((i64_t)1 << MIN_ORDER)
#define MAX_ALLOC ((i64_t)1 << MAX_ORDER)
#define POOL_SIZE (1 << MAX_ORDER)
#define MAX_POOL_ORDER 48

typedef struct node_t
{
    struct node_t *next;
    u64_t size;
} node_t;

CASSERT(sizeof(struct node_t) == 16, alloc_h)

typedef struct alloc_t
{
    node_t *freelist[MAX_POOL_ORDER + 2]; // free list of blocks by order
    node_t *pools;                        // list of pools
    u32_t avail;                          // mask of available blocks by order
    null_t *base;                         // base address of the pool
} __attribute__((aligned(PAGE_SIZE))) * alloc_t;

CASSERT(sizeof(struct alloc_t) % PAGE_SIZE == 0, alloc_h)

extern null_t *rf_malloc(i32_t size);
extern null_t *rf_realloc(null_t *ptr, i32_t size);
extern null_t rf_free(null_t *block);

extern alloc_t rf_alloc_init();
extern alloc_t rf_alloc_get();
extern null_t rf_alloc_cleanup();

#endif
