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

#include <stdio.h>
#include <assert.h>
#include "alloc.h"
#include "rayforce.h"
#include "ops.h"
#include "mmap.h"
#include "util.h"

static alloc_t _ALLOC = NULL;

#define MASK ((u64_t)0xFFFFFFFFFFFFFFFF)
#define MEMBASE ((i64_t)_ALLOC->pool)
#define offset(b) ((i64_t)b - MEMBASE)
#define _buddyof(b, i) (offset(b) ^ (1 << (i)))
#define buddyof(b, i) ((null_t *)(_buddyof(b, i) + MEMBASE))
#define blocksize(i) (1 << (i))
#define order_of(s) (32 - __builtin_clz(s + sizeof(struct node_t) - 1))

alloc_t rf_alloc_init()
{
    _ALLOC = (alloc_t)mmap(NULL, sizeof(struct alloc_t),
                           PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    memset(_ALLOC, 0, sizeof(struct alloc_t));
    _ALLOC->freelist[MAX_ORDER] = (node_t *)_ALLOC->pool;
    _ALLOC->blocks = 1 << MAX_ORDER;

    return _ALLOC;
}

alloc_t rf_alloc_get()
{
    return _ALLOC;
}

null_t rf_alloc_cleanup()
{
    munmap(_ALLOC, sizeof(struct alloc_t));
}

#ifdef SYS_MALLOC

null_t *rf_malloc(i32_t size)
{
    return malloc(size);
}

null_t rf_free(null_t *block)
{
    free(block);
}

null_t *rf_realloc(null_t *ptr, i32_t new_size)
{
    return realloc(ptr, new_size);
}

#else

null_t debug_blocks()
{
    i32_t i = 0;
    node_t *node;
    for (; i <= MAX_ORDER; i++)
    {
        node = _ALLOC->freelist[i];
        printf("-- order: %d [", i);
        while (node)
        {
            printf("%p, ", node);
            node = node->ptr;
        }
        printf("]\n");
    }
}

null_t *rf_malloc(i32_t size)
{
    i32_t i, order;
    null_t *block;
    node_t *node;

    // calculate minimal order for this size
    order = order_of(size);

    // find least order block that fits
    i = (MASK << order) & _ALLOC->blocks;
    if (!i)
        panic("OOM");
    i = __builtin_ctz(i);

    // remove the block out of list
    block = _ALLOC->freelist[i];
    _ALLOC->freelist[i] = _ALLOC->freelist[i]->ptr;
    ((node_t *)block)->order = order;
    if (_ALLOC->freelist[i] == NULL)
        _ALLOC->blocks &= ~(1 << i);

    // split until i == order
    while (i-- > order)
    {
        node = (node_t *)buddyof(block, i);
        node->order = order;
        node->ptr = _ALLOC->freelist[i];
        _ALLOC->freelist[i] = node;
        _ALLOC->blocks |= (1 << i);
    }

    block = (null_t *)((node_t *)block + 1);
    return block;
}

null_t rf_free(null_t *block)
{
    node_t *node = (node_t *)block - 1, *n;
    block = (null_t *)node;
    null_t *buddy;
    i32_t i = node->order;

    for (;; i++)
    {
        // calculate buddy
        buddy = buddyof(block, i);
        n = _ALLOC->freelist[i];

        // find buddy in list
        while (n && (n != buddy))
            n = n->ptr;

        // not found, insert into list
        if (n != buddy)
        {
            node->ptr = _ALLOC->freelist[i];
            _ALLOC->freelist[i] = node;
            _ALLOC->blocks |= (1 << i);
            return;
        }

        // found, merged block starts from the lower one
        block = (block < buddy) ? block : buddy;
        node = block;

        // remove buddy out of list
        _ALLOC->freelist[i] = _ALLOC->freelist[i]->ptr;
        _ALLOC->blocks &= ~(1 << i);
    }
}

null_t *rf_realloc(null_t *ptr, i32_t new_size)
{
    if (ptr == NULL)
        return rf_malloc(new_size);

    if (new_size == 0)
    {
        rf_free(ptr);
        panic("REALLOC: new size is 0");
        return NULL;
    }

    node_t *node = ((node_t *)ptr) - 1;
    i32_t size = 1 << node->order;
    i32_t adjusted_size = 1 << order_of(new_size);

    // If new size is smaller or equal to the current block size, no need to reallocate
    if (adjusted_size <= size)
        return ptr;

    null_t *new_ptr = rf_malloc(new_size);
    if (new_ptr)
    {
        // Take the minimum of size and new_size to prevent buffer overflow
        memcpy(new_ptr, ptr, size < new_size ? size : new_size);
        rf_free(ptr);
    }

    return new_ptr;
}

#endif
