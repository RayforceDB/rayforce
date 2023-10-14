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

CASSERT(sizeof(struct node_t) ==            16, heap_h)
CASSERT(sizeof(struct heap_t) % PAGE_SIZE == 0,  heap_h)

__thread heap_t __HEAP = NULL;

#define AVAIL_MASK       ((u64_t)0xFFFFFFFFFFFFFFFF)
#define BLOCK_ADDR_MASK  ((u64_t)0x00FFFFFFFFFFFFFF)
#define BLOCK_ORDER_MASK (~BLOCK_ADDR_MASK)
#define blockorder(p)    ((u64_t)(p) >> 56)
#define blockaddr(p)     ((nil_t *)((u64_t)(p) & BLOCK_ADDR_MASK))
#define blocksize(i)     (1ull << (i))
#define buddyof(p, b, i) ((nil_t *)(((u64_t)(p - b) ^ blocksize(i)) + b))
#define orderof(s)       (64ull - __builtin_clzll(s - 1))
#define is16block(b)     ((b) >= __HEAP->blocks16 && (b) < __HEAP->blocks16 + NUM_16_BLOCKS * 16)

#ifdef SYS_MALLOC

nil_t  *heap_alloc(u64_t size)                   { return malloc(size);           }
nil_t   heap_free(nil_t *block)                  { free(block);                   }
nil_t  *heap_realloc(nil_t *ptr, u64_t new_size) { return realloc(ptr, new_size); }
i64_t   heap_gc()                                { return 0;                      }
nil_t   heap_cleanup()                           {                                }
heap_t  heap_init()                              { return NULL;                   }
memstat_t heap_memstat()                         { return (memstat_t){0};         }

#else

nil_t heap_print_blocks()
{
    i32_t i = 0;
    node_t *node;
    for (; i <= MAX_POOL_ORDER; i++)
    {
        node = __HEAP->freelist[i];
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

    debug_assert((i64_t)pool % 16 == 0, "pool is not aligned");

    node_t *node = (node_t *)pool;
    node->base = (nil_t *)(order << 56 | (u64_t)pool);
    node->size = size;

    return (nil_t *)node;
}

heap_t heap_init()
{
    i32_t i;

    __HEAP = (heap_t)mmap_malloc(sizeof(struct heap_t));
    __HEAP->avail = 0;
    __HEAP->blocks16 = mmap_malloc(NUM_16_BLOCKS * 16);
    __HEAP->freelist16 = NULL;

    // fill linked list of 16 bytes blocks
    for (i = NUM_16_BLOCKS - 1; i >= 0; i--)
    {
        nil_t *block16 = __HEAP->blocks16 + i * 16;
        *(nil_t **)block16 = __HEAP->freelist16;
        __HEAP->freelist16 = block16;
    }

    return __HEAP;
}

nil_t heap_cleanup()
{
    i32_t i, order;
    node_t *node, *next;

    // check if all small blocks are freed
    for (i = 0; i < NUM_16_BLOCKS; i++)
    {
        if (__HEAP->freelist16 == NULL)
        {
            debug("blocks16 leak\n");
            return;
        }

        __HEAP->freelist16 = *(nil_t **)__HEAP->freelist16;
    }

    // All the nodes remains are pools, so just munmap them
    for (i = 0; i <= MAX_POOL_ORDER; i++)
    {
        node = __HEAP->freelist[i];
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

    mmap_free(__HEAP->blocks16, NUM_16_BLOCKS * 16);
    mmap_free(__HEAP, sizeof(struct heap_t));
}

heap_t heap_get()
{
    return __HEAP;
}

memstat_t heap_memstat()
{
    memstat_t stat = {0};
    i32_t i = 0;
    node_t *node;

    for (; i <= MAX_POOL_ORDER; i++)
    {
        node = __HEAP->freelist[i];
        while (node)
        {
            stat.total += node->size;
            node = node->next;
        }
    }

    stat.used = stat.total - stat.free;

    return stat;
}

nil_t __attribute__((hot)) * heap_alloc(u64_t size)
{
    u32_t i, order;
    nil_t *block, *base;
    node_t *node;
    u64_t capacity;

    if (size == 0)
        return NULL;

    // block is a 16 bytes block
    if (size <= 16 && __HEAP->freelist16)
    {
        block = __HEAP->freelist16;
        __HEAP->freelist16 = *(nil_t **)block;
        return block;
    }

    capacity = size + sizeof(struct node_t);

    // calculate minimal order for this size
    order = orderof(capacity);

    if (order > MAX_POOL_ORDER)
        return NULL;

    // find least order block that fits
    i = (AVAIL_MASK << order) & __HEAP->avail;

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
        __HEAP->freelist[MAX_ORDER] = node;
        __HEAP->avail |= 1 << MAX_ORDER;

        i = MAX_ORDER;
    }
    else
        i = __builtin_ctzl(i);

    // remove the block out of list
    block = __HEAP->freelist[i];
    __HEAP->freelist[i] = __HEAP->freelist[i]->next;

    if (__HEAP->freelist[i] == NULL)
        __HEAP->avail &= ~blocksize(i);

    // split until i == order
    while ((i--) > order)
    {
        base = ((node_t *)block)->base;
        node = (node_t *)buddyof(block, blockaddr(base), i);
        node->next = __HEAP->freelist[i];
        node->base = base;
        __HEAP->freelist[i] = node;
        __HEAP->avail |= blocksize(i);
    }

    ((node_t *)block)->size = capacity;
    return (nil_t *)((node_t *)block + 1);
}

nil_t __attribute__((hot)) heap_free(nil_t *block)
{
    nil_t *buddy;
    node_t *node, **n;
    u32_t order;

    if (block == NULL)
        return;

    // block is a 16 bytes block
    if is16block (block)
    {
        *(nil_t **)block = __HEAP->freelist16;
        __HEAP->freelist16 = block;

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
            node->next = __HEAP->freelist[order];
            __HEAP->freelist[order] = node;
            __HEAP->avail |= blocksize(order);

            return;
        }

        // calculate buddy
        buddy = buddyof(block, blockaddr(node->base), order);

        n = &__HEAP->freelist[order];

        // then continue to find the buddy of the block in the freelist.
        while ((*n != NULL) && (*n != buddy))
            n = &((*n)->next);

        // not found, insert into freelist
        if (*n == NULL)
        {
            node->next = __HEAP->freelist[order];
            __HEAP->freelist[order] = node;
            __HEAP->avail |= blocksize(order);

            return;
        }

        // remove buddy out of list
        *n = (*n)->next;
        if (__HEAP->freelist[order] == NULL)
            __HEAP->avail &= ~blocksize(order);

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
        return heap_alloc(new_size);

    if (new_size == 0)
    {
        heap_free(block);
        return NULL;
    }

    // block is a 16 bytes block
    if is16block (block)
    {
        if (new_size <= 16)
            return block;

        new_block = heap_alloc(new_size);
        if (new_block)
            memcpy(new_block, block, 16);

        *(nil_t **)block = __HEAP->freelist16;
        __HEAP->freelist16 = block;

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
        new_block = heap_alloc(new_size);

        if (new_block)
            memcpy(new_block, block, size);

        heap_free(block);

        return new_block;
    }

    // resize
    i = orderof(capacity);
    order = orderof(new_size + sizeof(struct node_t));

    // split until i == order
    while ((i--) > order)
    {
        size = blocksize(i);
        base = (node_t *)node->base;
        node->size = size;
        buddy = (node_t *)buddyof((nil_t *)node, blockaddr(base), i);
        buddy->next = __HEAP->freelist[i];
        buddy->base = base;
        __HEAP->freelist[i] = buddy;
        __HEAP->avail |= size;
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
        node = __HEAP->freelist[i];
        while (node)
        {
            next = node->next;
            order = blockorder(node->base);
            if (order == i)
            {
                if (prev != NULL)
                    prev->next = next;
                else
                    __HEAP->freelist[i] = next;

                mmap_free(node, blocksize(order));
                total += blocksize(order);

                if (__HEAP->freelist[i] == NULL)
                    __HEAP->avail &= ~blocksize(order);

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

#endif
