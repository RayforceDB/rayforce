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
#include "heap.h"
#include "rayforce.h"
#include "ops.h"
#include "mmap.h"
#include "util.h"

// clang-format off
CASSERT(sizeof(struct node_t     ) ==            16, heap_h)
CASSERT(sizeof(struct heap_t    ) % PAGE_SIZE == 0,  heap_h)

static heap_t _HEAP = NULL;

#define AVAIL_MASK       ((u64_t)0xFFFFFFFFFFFFFFFF)
#define BLOCK_ADDR_MASK  ((u64_t)0x00FFFFFFFFFFFFFF)
#define BLOCK_ORDER_MASK (~BLOCK_ADDR_MASK)
#define blockorder(p)    ((u64_t)(p) >> 56)
#define blockaddr(p)     ((nil_t *)((u64_t)(p) & BLOCK_ADDR_MASK))
#define blocksize(i)     (1ull << (i))
#define buddyof(p, b, i) ((nil_t *)(((u64_t)(p - b) ^ blocksize(i)) + b))
#define orderof(s)       (64ull - __builtin_clzl(s - 1))
#define is32block(b)     ((b) >= _HEAP->blocks32 && (b) < _HEAP->blocks32 + NUM_32_BLOCKS * 32)
#define is64block(b)     ((b) >= _HEAP->blocks64 && (b) < _HEAP->blocks64 + NUM_64_BLOCKS * 64)

#ifdef SYS_MALLOC

nil_t  *heap_malloc(u64_t size)                  { return malloc(size);           }
nil_t   heap_free(nil_t *block)                  { free(block);                   }
nil_t  *heap_realloc(nil_t *ptr, u64_t new_size) { return realloc(ptr, new_size); }
i64_t   heap_gc()                                { return 0;                      }
nil_t   heap_mrequest(u64_t size)                {                                }
nil_t   heap_cleanup()                           {                                }
heap_t  heap_init()                              { return NULL;                   }

#else

// clang-format on
nil_t heap_print_blocks()
{
    i32_t i = 0;
    node_t *node;
    for (; i <= MAX_POOL_ORDER; i++)
    {
        node = _HEAP->freelist[i];
        printf("-- order: %d [", i);
        while (node)
        {
            printf("%p, ", node);
            node = node->next;
        }
        printf("]\n");
    }
}

nil_t *heap_add_pool(u64_t order)
{
    u64_t size = blocksize(order);
    nil_t *pool = mmap_malloc(size);

    if (pool == NULL)
        return NULL;

    debug_assert((i64_t)pool % 16 == 0);

    node_t *node = (node_t *)pool;
    node->base = (nil_t *)(order << 56 | (u64_t)pool);
    node->size = size;

    return (nil_t *)node;
}

heap_t heap_init()
{
    i32_t i;

    _HEAP = (heap_t)mmap_malloc(sizeof(struct heap_t));
    _HEAP->avail = 0;
    _HEAP->blocks32 = mmap_malloc(NUM_32_BLOCKS * 32);
    _HEAP->freelist32 = NULL;
    _HEAP->blocks64 = mmap_malloc(NUM_64_BLOCKS * 64);
    _HEAP->freelist64 = NULL;

    // fill linked list of 32 bytes blocks
    for (i = NUM_32_BLOCKS - 1; i >= 0; i--)
    {
        nil_t *block32 = _HEAP->blocks32 + i * 32;
        *(nil_t **)block32 = _HEAP->freelist32;
        _HEAP->freelist32 = block32;
    }

    // fill linked list of 64 bytes blocks
    for (i = NUM_64_BLOCKS - 1; i >= 0; i--)
    {
        nil_t *block64 = _HEAP->blocks64 + i * 64;
        *(nil_t **)block64 = _HEAP->freelist64;
        _HEAP->freelist64 = block64;
    }

    return _HEAP;
}

heap_t heap_get()
{
    return _HEAP;
}

nil_t heap_cleanup()
{
    i32_t i, order;
    node_t *node, *next;

    // All the nodes remains are pools, so just munmap them
    for (i = 0; i <= MAX_POOL_ORDER; i++)
    {
        node = _HEAP->freelist[i];
        while (node)
        {
            next = node->next;
            order = blockorder(node->base);
            if (order != i)
            {
                debug("order: %d node: %p\n", i, node);
                return;
            }

            mmap_free(node, blocksize(order));
            node = next;
        }
    }

    mmap_free(_HEAP->blocks32, NUM_32_BLOCKS * 32);
    mmap_free(_HEAP->blocks64, NUM_64_BLOCKS * 64);
    mmap_free(_HEAP, sizeof(struct heap_t));
}

memstat_t heap_memstat()
{
    memstat_t stat = {0};
    i32_t i = 0;
    node_t *node;

    for (; i <= MAX_POOL_ORDER; i++)
    {
        node = _HEAP->freelist[i];
        while (node)
        {
            stat.total += node->size;
            node = node->next;
        }
    }

    stat.used = stat.total - stat.free;

    return stat;
}

nil_t *heap_malloc(u64_t size)
{
    u32_t i, order;
    nil_t *block, *base;
    node_t *node;
    u64_t capacity;

    if (size == 0)
        return NULL;

    // block is a 32 bytes block
    if (size <= 32 && _HEAP->freelist32)
    {
        block = _HEAP->freelist32;
        _HEAP->freelist32 = *(nil_t **)block;
        return block;
    }

    // block is a 64 bytes block
    if (size <= 64 && _HEAP->freelist64)
    {
        block = _HEAP->freelist64;
        _HEAP->freelist64 = *(nil_t **)block;
        return block;
    }

    capacity = size + sizeof(struct node_t);

    // calculate minimal order for this size
    order = orderof(capacity);

    if (order > MAX_POOL_ORDER)
        return NULL;

    // find least order block that fits
    i = (AVAIL_MASK << order) & _HEAP->avail;

    // no free block found for this size, so mmap it directly if it is bigger than pool size or
    // add a new pool and split as well
    if (i == 0)
    {
        if (capacity >= POOL_SIZE)
        {
            block = heap_add_pool(order);
            return (nil_t *)((node_t *)block + 1);
        }

        node_t *node = (node_t *)heap_add_pool(MAX_ORDER);

        node->next = NULL;
        _HEAP->freelist[MAX_ORDER] = node;
        _HEAP->avail |= 1 << MAX_ORDER;

        i = MAX_ORDER;
    }
    else
        i = __builtin_ctzl(i);

    // remove the block out of list
    block = _HEAP->freelist[i];
    _HEAP->freelist[i] = _HEAP->freelist[i]->next;

    if (_HEAP->freelist[i] == NULL)
        _HEAP->avail &= ~blocksize(i);

    // split until i == order
    while ((i--) > order)
    {
        base = ((node_t *)block)->base;
        node = (node_t *)buddyof(block, blockaddr(base), i);
        node->next = _HEAP->freelist[i];
        node->base = base;
        _HEAP->freelist[i] = node;
        _HEAP->avail |= blocksize(i);
    }

    ((node_t *)block)->size = capacity;
    return (nil_t *)((node_t *)block + 1);
}

nil_t heap_free(nil_t *block)
{
    nil_t *buddy;
    node_t *node, **n;
    u32_t order;

    // block is a 32 bytes block
    if is32block (block)
    {
        *(nil_t **)block = _HEAP->freelist32;
        _HEAP->freelist32 = block;

        return;
    }

    // block is a 64 bytes block
    if is64block (block)
    {
        *(nil_t **)block = _HEAP->freelist64;
        _HEAP->freelist64 = block;

        return;
    }

    node = (node_t *)block - 1;
    block = (nil_t *)node;
    order = orderof(node->size);

    for (;; order++)
    {
        // node is the root block, so just insert it into list
        if (order == blockorder(node->base))
        {
            node->next = _HEAP->freelist[order];
            _HEAP->freelist[order] = node;
            _HEAP->avail |= blocksize(order);

            return;
        }

        // calculate buddy
        buddy = buddyof(block, blockaddr(node->base), order);

        n = &_HEAP->freelist[order];

        // then continue to find the buddy of the block in the freelist.
        while ((*n != NULL) && (*n != buddy))
            n = &((*n)->next);

        // not found, insert into freelist
        if (*n == NULL)
        {
            node->next = _HEAP->freelist[order];
            _HEAP->freelist[order] = node;
            _HEAP->avail |= blocksize(order);

            return;
        }

        // remove buddy out of list
        *n = (*n)->next;
        if (_HEAP->freelist[order] == NULL)
            _HEAP->avail &= ~blocksize(order);

        // check if buddy is lower address than block (means it is of higher order), if so, swap them
        block = (buddy > block) ? block : buddy;
        node = block;
    }
}

nil_t *heap_realloc(nil_t *block, u64_t new_size)
{
    node_t *node, *buddy;
    u64_t i, capacity, size, order;
    nil_t *new_block, *base;

    if (block == NULL)
        return heap_malloc(new_size);

    if (new_size == 0)
    {
        heap_free(block);
        return NULL;
    }

    // block is a 32 bytes block
    if is32block (block)
    {
        if (new_size <= 32)
            return block;

        new_block = heap_malloc(new_size);
        if (new_block)
            memcpy(new_block, block, 32);

        *(nil_t **)block = _HEAP->freelist32;
        _HEAP->freelist32 = block;

        return new_block;
    }

    // block is a 64 bytes block
    if is64block (block)
    {
        if (new_size <= 64)
            return block;

        new_block = heap_malloc(new_size);
        if (new_block)
            memcpy(new_block, block, 64);

        *(nil_t **)block = _HEAP->freelist64;
        _HEAP->freelist64 = block;

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
        new_block = heap_malloc(new_size);

        if (new_block)
            memcpy(new_block, block, size);

        heap_free(block);

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
        buddy = (node_t *)buddyof((nil_t *)node, blockaddr(base), i);
        buddy->next = _HEAP->freelist[i];
        buddy->base = base;
        _HEAP->freelist[i] = buddy;
        _HEAP->avail |= size;
    }

    return block;
}

i64_t heap_gc()
{
    i64_t i, order, total = 0;
    node_t *node, *prev, *next;

    for (i = 0; i <= MAX_POOL_ORDER; i++)
    {
        prev = NULL;
        node = _HEAP->freelist[i];
        while (node)
        {
            next = node->next;
            order = blockorder(node->base);
            if (order == i)
            {
                if (prev != NULL)
                    prev->next = next;
                else
                    _HEAP->freelist[i] = next;

                mmap_free(node, blocksize(order));
                total += blocksize(order);

                if (_HEAP->freelist[i] == NULL)
                    _HEAP->avail &= ~blocksize(order);

                // As node is now freed, move onto the next node.
                node = next;
            }
            else
            {
                prev = node;
                node = next;
            }
        }
    }

    return total;
}

nil_t heap_mrequest(u64_t size)
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

    if (_HEAP->avail & (AVAIL_MASK << order))
        return;

    // add a new pool of requested size
    node = (node_t *)heap_add_pool(order);

    if (node == NULL)
        return;

    node->next = NULL;
    _HEAP->freelist[order] = node;
    _HEAP->avail |= 1 << order;
}

#endif
