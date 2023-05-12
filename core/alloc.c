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

// clang-format off
#define AVAIL_MASK    ((u64_t)0xFFFFFFFFFFFFFFFF)
#define MEMBASE       ((i64_t)_ALLOC->base)
#define offset(b)     ((i64_t)b - MEMBASE)
#define blocksize(i)  (1 << (i))
#define buddyof(b, i) ((null_t *)((offset(b) ^ blocksize(i)) + MEMBASE))
#define realsize(s)   (s + sizeof(struct node_t))
#define orderof(s)    (32 - __builtin_clz(s - 1))
// clang-format on

null_t *rf_alloc_add_pool(u32_t size)
{
    null_t *pool = (null_t *)mmap(NULL, size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(pool, 0, size);

    node_t *base = (node_t *)pool;
    base->size = size;
    base->next = _ALLOC->pools;
    _ALLOC->pools = base;

    return (null_t *)(base + 1);
}

alloc_t rf_alloc_init()
{
    _ALLOC = (alloc_t)mmap(NULL, sizeof(struct alloc_t),
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    _ALLOC->pools = NULL;
    _ALLOC->base = rf_alloc_add_pool(realsize(POOL_SIZE));
    _ALLOC->freelist[MAX_ORDER] = (node_t *)_ALLOC->base;
    _ALLOC->avail = 1 << MAX_ORDER;

    debug_assert(_ALLOC->base != NULL);
    debug_assert((i64_t)_ALLOC->base % sizeof(struct node_t) == 0);

    return _ALLOC;
}

alloc_t rf_alloc_get()
{
    return _ALLOC;
}

null_t rf_alloc_cleanup()
{
    node_t *node = _ALLOC->pools, *next;
    while (node)
    {
        next = node->next;
        munmap(node, node->size);
        node = next;
    }

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

// null_t bin(u32_t i)
// {
//     u32_t mask = 1 << 31;

//     while (mask)
//     {
//         printf("%s ", (i & mask) ? "++" : "--");
//         mask = mask >> 1;
//     }

//     printf("\n");

//     for (i32_t i = 0; i < 32; i++)
//         printf("%0.2d ", 31 - i);

//     printf("\n");
// }

null_t print_blocks()
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
            node = node->next;
        }
        printf("]\n");
    }
}

null_t *rf_malloc(i32_t size)
{
    i32_t i, order;
    null_t *block;
    node_t *node;

    size = realsize(size);

    // calculate minimal order for this size
    order = orderof(size);

    // find least order block that fits
    i = (AVAIL_MASK << order) & _ALLOC->avail;

    // no free block found for this size, so mmap it directly
    if (i == 0)
        return rf_alloc_add_pool(size);

    i = __builtin_ctz(i);

    // remove the block out of list
    block = _ALLOC->freelist[i];
    _ALLOC->freelist[i] = _ALLOC->freelist[i]->next;
    ((node_t *)block)->size = size;
    if (_ALLOC->freelist[i] == NULL)
        _ALLOC->avail &= ~blocksize(i);

    // split until i == order
    while (i-- > order)
    {
        node = (node_t *)buddyof(block, i);
        node->size = size;
        node->next = _ALLOC->freelist[i];
        _ALLOC->freelist[i] = node;
        _ALLOC->avail |= blocksize(i);
    }

    block = (null_t *)((node_t *)block + 1);
    return block;
}

null_t rf_free(null_t *block)
{
    node_t *node = (node_t *)block - 1, **n;
    block = (null_t *)node;
    null_t *buddy;
    i32_t i = orderof(node->size);

    for (;; i++)
    {
        // calculate buddy
        buddy = buddyof(block, i);
        n = &_ALLOC->freelist[i];

        // find buddy in list
        while ((*n != NULL) && (*n != buddy))
            n = &((*n)->next);

        // not found, insert into list
        if (*n != buddy)
        {
            node->next = _ALLOC->freelist[i];
            _ALLOC->freelist[i] = node;
            _ALLOC->avail |= blocksize(i);
            return;
        }

        // found, merged block starts from the lower one
        block = (block < buddy) ? block : buddy;
        node = block;

        // remove buddy out of list
        *n = (*n)->next;
        if (_ALLOC->freelist[i] == NULL)
            _ALLOC->avail &= ~blocksize(i);
    }
}

null_t *rf_realloc(null_t *ptr, i32_t new_size)
{
    if (ptr == NULL)
        return rf_malloc(new_size);

    if (new_size == 0)
    {
        rf_free(ptr);
        return NULL;
    }

    new_size = realsize(new_size);

    node_t *node = ((node_t *)ptr) - 1;
    i32_t size = node->size;
    i32_t adjusted_size = blocksize(orderof(new_size));

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
