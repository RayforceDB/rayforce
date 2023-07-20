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
#define AVAIL_MASK       ((u64_t)0xFFFFFFFFFFFFFFFF)
#define BLOCK_ORDER_MASK (1ull << 56)        // 1 byte
#define BLOCK_ADDR_MASK  (~BLOCK_ORDER_MASK) // 7 bytes
#define blockorder(p)    ((u64_t)(p) >> 56)
#define blockaddr(p)     ((null_t *)((u64_t)(p) & BLOCK_ADDR_MASK))
#define blocksize(i)     (1ull << (i))
#define buddyof(p, b, i) ((null_t *)(((u64_t)(p - b) ^ blocksize(i)) + b))
#define orderof(s)       (64ull - __builtin_clzl(s - 1))
#define is32block(b)     ((b) >= _ALLOC->blocks32 && (b) < _ALLOC->blocks32 + NUM_32_BLOCKS * 32)
#define is64block(b)     ((b) >= _ALLOC->blocks64 && (b) < _ALLOC->blocks64 + NUM_64_BLOCKS * 64)
// clang-format on

null_t print_blocks()
{
    i32_t i = 0;
    node_t *node;
    for (; i <= MAX_POOL_ORDER; i++)
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

null_t *rf_alloc_add_pool(u64_t size)
{
    null_t *pool = mmap_malloc(size);

    if (pool == NULL)
        return NULL;

    node_t *node = (node_t *)pool;

    assert((i64_t)node % 16 == 0);

    node->base = (null_t *)((orderof(size) << 56) | (u64_t)pool);
    node->size = size;

    return (null_t *)node;
}

alloc_t rf_alloc_init()
{
    i32_t i;

    _ALLOC = (alloc_t)mmap_malloc(sizeof(struct alloc_t));
    _ALLOC->avail = 0;
    _ALLOC->blocks32 = mmap_malloc(NUM_32_BLOCKS * 32);
    _ALLOC->freelist32 = NULL;
    _ALLOC->blocks64 = mmap_malloc(NUM_64_BLOCKS * 64);
    _ALLOC->freelist64 = NULL;

    // fill linked list of 32 bytes blocks
    for (i = NUM_32_BLOCKS - 1; i >= 0; i--)
    {
        null_t *block32 = _ALLOC->blocks32 + i * 32;
        *(null_t **)block32 = _ALLOC->freelist32;
        _ALLOC->freelist32 = block32;
    }

    // // fill linked list of 64 bytes blocks
    for (i = NUM_64_BLOCKS - 1; i >= 0; i--)
    {
        null_t *block64 = _ALLOC->blocks64 + i * 64;
        *(null_t **)block64 = _ALLOC->freelist64;
        _ALLOC->freelist64 = block64;
    }

    node_t *node = (node_t *)rf_alloc_add_pool(1ull << MAX_ORDER);
    node->next = NULL;
    _ALLOC->freelist[MAX_ORDER] = node;
    _ALLOC->avail = 1ull << MAX_ORDER;

    // print_blocks();

    return _ALLOC;
}

alloc_t rf_alloc_get()
{
    return _ALLOC;
}

null_t rf_alloc_cleanup()
{
    print_blocks();

    i32_t i;

    // All the nodes remains are pools, so just munmap them
    for (i = 0; i <= MAX_POOL_ORDER; i++)
    {
        node_t *node = _ALLOC->freelist[i], *next;
        while (node)
        {
            next = node->next;
            if (blockaddr(node->base) != node)
            {
                debug("node->base: %p\n", node->base);
                return;
            }
            mmap_free(node->base, node->size);
            node = next;
        }
    }

    mmap_free(_ALLOC->blocks32, NUM_32_BLOCKS * 32);
    mmap_free(_ALLOC->blocks64, NUM_64_BLOCKS * 64);
    mmap_free(_ALLOC, sizeof(struct alloc_t));
}

memstat_t rf_alloc_memstat()
{
    memstat_t stat = {0};
    i32_t i = 0;
    node_t *node;

    for (; i <= MAX_POOL_ORDER; i++)
    {
        node = _ALLOC->freelist[i];
        while (node)
        {
            stat.total += node->size;
            node = node->next;
        }
    }

    stat.used = stat.total - stat.free;

    return stat;
}

#ifdef SYS_MALLOC

null_t *rf_malloc(u64_t size)
{
    return malloc(size);
}

null_t rf_free(null_t *block)
{
    free(block);
}

null_t *rf_realloc(null_t *ptr, u64_t new_size)
{
    return realloc(ptr, new_size);
}

null_t rf_alloc_mrequest(u64_t size) {}

#else

null_t *rf_malloc(u64_t size)
{
    u32_t i, order;
    null_t *block, *base;
    node_t *node;
    u64_t capacity;

    if (size == 0)
        return NULL;

    // block is a 32 bytes block
    if (size <= 32 && _ALLOC->freelist32)
    {
        block = _ALLOC->freelist32;
        _ALLOC->freelist32 = *(null_t **)block;
        return block;
    }

    // block is a 64 bytes block
    if (size <= 64 && _ALLOC->freelist64)
    {
        block = _ALLOC->freelist64;
        _ALLOC->freelist64 = *(null_t **)block;
        return block;
    }

    capacity = size + sizeof(struct node_t);

    // calculate minimal order for this size
    order = orderof(capacity);

    if (order > MAX_POOL_ORDER)
        return NULL;

    // find least order block that fits
    i = (AVAIL_MASK << order) & _ALLOC->avail;

    // no free block found for this size, so mmap it directly if it is bigger than pool size or
    // add a new pool and split as well
    if (i == 0)
    {
        if (capacity >= POOL_SIZE)
        {
            capacity = 1ull << order;
            block = rf_alloc_add_pool(capacity);
            return (null_t *)((node_t *)block + 1);
        }

        node_t *node = (node_t *)rf_alloc_add_pool(POOL_SIZE);

        node->next = NULL;
        _ALLOC->freelist[MAX_ORDER] = node;
        _ALLOC->avail |= 1 << MAX_ORDER;

        i = MAX_ORDER;
    }
    else
        i = __builtin_ctzl(i);

    // remove the block out of list
    block = _ALLOC->freelist[i];
    _ALLOC->freelist[i] = _ALLOC->freelist[i]->next;

    if (_ALLOC->freelist[i] == NULL)
        _ALLOC->avail &= ~blocksize(i);

    // split until i == order
    while ((i--) > order)
    {
        base = ((node_t *)block)->base;
        node = (node_t *)buddyof(block, blockaddr(base), i);
        node->next = _ALLOC->freelist[i];
        node->base = base;
        _ALLOC->freelist[i] = node;
        _ALLOC->avail |= blocksize(i);
    }

    ((node_t *)block)->size = capacity;
    return (null_t *)((node_t *)block + 1);
}

null_t rf_free(null_t *block)
{
    null_t *buddy;
    node_t *node, **n;
    u32_t order;

    // block is a 32 bytes block
    if is32block (block)
    {
        *(null_t **)block = _ALLOC->freelist32;
        _ALLOC->freelist32 = block;

        return;
    }

    // block is a 64 bytes block
    if is64block (block)
    {
        *(null_t **)block = _ALLOC->freelist64;
        _ALLOC->freelist64 = block;

        return;
    }

    node = (node_t *)block - 1;
    block = (null_t *)node;
    order = orderof(node->size);

    for (;; order++)
    {
        // node is the root block, so just insert it into list
        if (order == blockorder(node->base))
        {
            node->next = _ALLOC->freelist[order];
            _ALLOC->freelist[order] = node;
            _ALLOC->avail |= blocksize(order);

            return;
        }

        // calculate buddy
        buddy = buddyof(block, blockaddr(node->base), order);

        n = &_ALLOC->freelist[order];

        // then continue to find the buddy of the block in the freelist.
        while ((*n != NULL) && (*n != buddy))
            n = &((*n)->next);

        // not found, insert into freelist
        if (*n == NULL)
        {
            node->next = _ALLOC->freelist[order];
            _ALLOC->freelist[order] = node;
            _ALLOC->avail |= blocksize(order);

            return;
        }

        // remove buddy out of list
        *n = (*n)->next;
        if (_ALLOC->freelist[order] == NULL)
            _ALLOC->avail &= ~blocksize(order);

        // check if buddy is lower address than block (means it is of higher order), if so, swap them
        block = (buddy > block) ? block : buddy;
        node = block;
    }
}

null_t *rf_realloc(null_t *block, u64_t new_size)
{
    node_t *node, *buddy;
    u64_t i, capacity, size, order;
    null_t *new_block, *base;

    if (block == NULL)
        return rf_malloc(new_size);

    if (new_size == 0)
    {
        rf_free(block);
        return NULL;
    }

    // block is a 32 bytes block
    if is32block (block)
    {
        if (new_size <= 32)
            return block;

        new_block = rf_malloc(new_size);
        if (new_block)
            memcpy(new_block, block, 32);

        *(null_t **)block = _ALLOC->freelist32;
        _ALLOC->freelist32 = block;

        return new_block;
    }

    // block is a 64 bytes block
    if is64block (block)
    {
        if (new_size <= 64)
            return block;

        new_block = rf_malloc(new_size);
        if (new_block)
            memcpy(new_block, block, 64);

        *(null_t **)block = _ALLOC->freelist64;
        _ALLOC->freelist64 = block;

        return new_block;
    }

    node = ((node_t *)block) - 1;
    capacity = node->size;
    size = capacity - sizeof(struct node_t);

    if (new_size == size)
        return block;

    // grow
    if (new_size > size)
    {
        new_block = rf_malloc(new_size);

        if (new_block)
            memcpy(new_block, block, size);

        rf_free(block);

        return new_block;
    }

    // shrink
    i = orderof(capacity);
    order = orderof(new_size + sizeof(struct node_t));

    // split until i == order
    while ((i--) > order)
    {
        size = blocksize(i);
        base = (node_t *)node->base;
        node->size = size;
        buddy = (node_t *)buddyof((null_t *)node, blockaddr(base), i);
        buddy->next = _ALLOC->freelist[i];
        buddy->base = base;
        _ALLOC->freelist[i] = buddy;
        _ALLOC->avail |= size;
    }

    return block;
}

null_t rf_alloc_mrequest(u64_t size)
{
    u32_t order;
    node_t *node;
    u64_t capacity;

    if (size <= 64)
        return;

    capacity = size + sizeof(struct node_t);

    // calculate minimal order for this size
    order = orderof(capacity);

    if (order > MAX_POOL_ORDER)
        return;

    if (_ALLOC->avail & (AVAIL_MASK << order))
        return;

    // add a new pool of requested size
    node = (node_t *)rf_alloc_add_pool(capacity);

    if (node == NULL)
        return;

    node->next = NULL;
    _ALLOC->freelist[order] = node;
    _ALLOC->avail |= 1 << order;
}

#endif
