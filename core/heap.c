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
#include "string.h"

CASSERT(sizeof(struct block_t) == 2 * sizeof(struct obj_t), heap_h);

__thread heap_p __HEAP = NULL;

#define blocksize(s) (sizeof(struct obj_t) + (s))
#define bsizeof(o) (1ull << (u64_t)(o))
#define buddyof(b, o) ((block_p)((u64_t)(b)->pool + (((u64_t)(b) - (u64_t)(b)->pool) ^ bsizeof(o))))
#define orderof(s) (64ull - __builtin_clzll((s) - 1))
#define block2raw(b) ((raw_p)((u64_t)(b) + sizeof(struct obj_t)))
#define raw2block(r) ((block_p)((u64_t)(r) - sizeof(struct obj_t)))

#ifdef SYS_MALLOC

heap_p heap_init(u64_t) { return NULL; }
raw_p heap_alloc(u64_t size) { return malloc(size); }
raw_p heap_mmap(u64_t size) { return mmap_alloc(size); }
raw_p heap_stack(u64_t size) { return mmap_stack(size); }
nil_t heap_free(raw_p ptr) { free(ptr); }
raw_p heap_realloc(raw_p ptr, u64_t size) { return realloc(ptr, size); }
nil_t heap_unmap(raw_p ptr, u64_t size) { mmap_free(ptr, size); }
i64_t heap_gc(nil_t) { return 0; }
nil_t heap_cleanup(nil_t) {}
nil_t heap_borrow(heap_p heap) { unused(heap); }
nil_t heap_merge(heap_p heap) { unused(heap); }
memstat_t heap_memstat(nil_t) { return (memstat_t){0}; }

#else

heap_p heap_init(u64_t id)
{
    __HEAP = (heap_p)mmap_alloc(sizeof(struct heap_t));
    __HEAP->id = id;
    __HEAP->avail = 0;

    memset(__HEAP->freelist, 0, sizeof(__HEAP->freelist));

    return __HEAP;
}

block_p heap_add_pool(u64_t size)
{
    block_p block = (block_p)mmap_alloc(size);

    if (block == NULL)
        return NULL;

    block->pool = block;
    block->pool_order = orderof(size);

    __HEAP->memstat.system += size;
    __HEAP->memstat.heap += size;

    return block;
}

nil_t heap_remove_pool(block_p block, u64_t size)
{
    mmap_free(block, size);

    __HEAP->memstat.system -= size;
    __HEAP->memstat.heap -= size;
}

inline __attribute__((always_inline)) nil_t heap_insert_block(block_p block, u64_t order)
{
    u64_t size = bsizeof(order);

    block->prev = NULL;
    block->next = __HEAP->freelist[order];
    block->used = 0;
    block->order = order;

    if (__HEAP->freelist[order] != NULL)
        __HEAP->freelist[order]->prev = block;
    else
        __HEAP->avail |= size;

    __HEAP->freelist[order] = block;
}

inline __attribute__((always_inline)) nil_t heap_remove_block(block_p block, u64_t order)
{
    if (block->prev)
        block->prev->next = block->next;
    if (block->next)
        block->next->prev = block->prev;

    if (__HEAP->freelist[order] == block)
        __HEAP->freelist[order] = block->next;

    if (__HEAP->freelist[order] == NULL)
        __HEAP->avail &= ~bsizeof(order);
}

inline __attribute__((always_inline)) nil_t heap_split_block(block_p block, u64_t block_order, u64_t order)
{
    block_p buddy;

    while ((order--) > block_order)
    {
        buddy = (block_p)((u64_t)block + bsizeof(order));
        buddy->pool = block->pool;
        buddy->pool_order = block->pool_order;
        heap_insert_block(buddy, order);
    }
}

raw_p heap_mmap(u64_t size)
{
    raw_p ptr = mmap_alloc(size);

    if (ptr == NULL)
        return NULL;

    __HEAP->memstat.system += size;

    return ptr;
}

raw_p heap_stack(u64_t size)
{
    raw_p ptr = mmap_stack(size);

    if (ptr == NULL)
        return NULL;

    __HEAP->memstat.system += size;

    return ptr;
}

raw_p __attribute__((hot)) heap_alloc(u64_t size)
{
    u64_t i, order, block_size;
    block_p block;

    if (size == 0 || size > bsizeof(MAX_POOL_ORDER))
        return NULL;

    block_size = blocksize(size);

    // calculate minimal order for this size
    order = orderof(block_size);

    // find least order block that fits
    i = (AVAIL_MASK << order) & __HEAP->avail;

    // no free block found for this size, so mmap it directly if it is bigger than pool size or
    // add a new pool and split as well
    if (i == 0)
    {
        if (order >= MAX_BLOCK_ORDER)
        {
            size = bsizeof(order);
            block = heap_add_pool(size);

            if (block == NULL)
                return NULL;

            block->order = order;

            __HEAP->memstat.system += size;

            return block2raw(block);
        }

        block = heap_add_pool(bsizeof(MAX_BLOCK_ORDER));

        if (block == NULL)
            return NULL;

        i = MAX_BLOCK_ORDER;
        heap_insert_block(block, i);
    }
    else
        i = __builtin_ctzll(i);

    // remove the block out of list
    block = __HEAP->freelist[i];
    block->order = order;
    block->used = 1;

    __HEAP->freelist[i] = block->next;
    if (__HEAP->freelist[i] != NULL)
        __HEAP->freelist[i]->prev = NULL;
    else
        __HEAP->avail &= ~bsizeof(i);

    heap_split_block(block, order, i);

    return block2raw(block);
}

__attribute__((hot)) nil_t heap_free(raw_p ptr)
{
    block_p block, buddy;
    u64_t order;

    if (ptr == NULL)
        return;

    block = raw2block(ptr);
    order = block->order;

    for (;; order++)
    {
        // check if we are at the root block (no buddies left)
        if (block->pool_order == order)
            return heap_insert_block(block, order);

        // calculate buddy
        buddy = buddyof(block, order);

        // buddy is used, or buddy is of different order, so we can't merge
        if (buddy->used || buddy->order != order)
            return heap_insert_block(block, order);

        // merge blocks: remove buddy from its freelist.
        heap_remove_block(buddy, order);

        // check if buddy is lower address than block (means it is of higher order), if so, swap them
        block = (buddy < block) ? buddy : block;
    }
}

__attribute__((hot)) raw_p heap_realloc(raw_p ptr, u64_t new_size)
{
    block_p block;
    u64_t i, old_size, cap, order;
    raw_p new_ptr;

    block = raw2block(ptr);
    old_size = bsizeof(block->order);
    cap = blocksize(new_size);
    order = orderof(cap);

    if (block->order == order)
        return ptr;

    // grow
    if (order > block->order)
    {
        new_ptr = heap_alloc(new_size);

        if (new_ptr == NULL)
        {
            heap_free(ptr);
            return NULL;
        }

        memcpy(new_ptr, ptr, old_size - sizeof(struct obj_t));
        heap_free(ptr);

        return new_ptr;
    }

    // shrink
    i = block->order;
    block->order = order;

    heap_split_block(block, order, i);

    return ptr;
}

nil_t heap_unmap(raw_p ptr, u64_t size)
{
    mmap_free(ptr, size);
    __HEAP->memstat.system -= size;
}

i64_t heap_gc(nil_t)
{
    u64_t i, size, total = 0;
    block_p block, next;

    for (i = MAX_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++)
    {
        block = __HEAP->freelist[i];
        size = bsizeof(i);

        while (block)
        {
            next = block->next;

            if (i == block->pool_order)
            {
                heap_remove_block(block, i);
                heap_remove_pool(block, size);
                total += size;
            }

            block = next;
        }
    }

    return total;
}

nil_t heap_borrow(heap_p heap)
{
    u64_t i, size, avail_mask = __HEAP->avail;

    while (avail_mask != 0)
    {
        i = __builtin_ctzll(avail_mask);
        size = bsizeof(i);
        avail_mask &= ~size;

        // Only borrow if the source heap has a freelist[i] and it has more than one node
        if (__HEAP->freelist[i]->next == NULL)
            continue;

        heap->freelist[i] = __HEAP->freelist[i];
        __HEAP->freelist[i] = __HEAP->freelist[i]->next;
        __HEAP->freelist[i]->prev = NULL;

        heap->freelist[i]->next = NULL;
        heap->freelist[i]->prev = NULL;
        heap->avail |= size;
    }
}

nil_t heap_merge(heap_p heap)
{
    u64_t i, avail_mask = heap->avail;
    block_p block, next;

    while (avail_mask != 0)
    {
        i = __builtin_ctzll(avail_mask);
        avail_mask &= ~bsizeof(i);

        block = heap->freelist[i];

        while (block)
        {
            next = block;
            block = block->next;
            heap_free(next);
        }

        heap->freelist[i] = NULL;
    }

    // Update memory statistics
    __HEAP->memstat.heap += heap->memstat.heap;
    __HEAP->memstat.system += heap->memstat.system;

    // Clear borrowed heap
    heap->memstat.heap = 0;
    heap->memstat.system = 0;
    heap->avail = 0;
}

memstat_t heap_memstat(nil_t)
{
    u64_t i;
    block_p block;

    __HEAP->memstat.free = 0;

    // calculate free blocks
    for (i = MIN_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++)
    {
        block = __HEAP->freelist[i];
        while (block)
        {
            __HEAP->memstat.free += blocksize(i);
            block = block->next;
        }
    }

    return __HEAP->memstat;
}

nil_t heap_cleanup(nil_t)
{
    u64_t i;
    block_p block, next;

    // All the nodes remains are pools, so just munmap them
    for (i = MIN_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++)
    {
        block = __HEAP->freelist[i];
        while (block)
        {
            next = block->next;
            if (i != block->pool_order)
            {
                debug("%s-- HEAP[%lld]: leak order: %lld block: %p%s", RED, __HEAP->id, i, block, RESET);
                return;
            }

            mmap_free(block, bsizeof(i));
            block = next;
        }
    }

    mmap_free(__HEAP, sizeof(struct heap_t));

    __HEAP = NULL;
}

nil_t heap_print_blocks(heap_p heap)
{
    u64_t i;
    block_p block;

    printf("-- HEAP[%lld]: BLOCKS:\n", heap->id);
    for (i = 0; i <= MAX_POOL_ORDER; i++)
    {
        block = heap->freelist[i];
        printf("-- order: %lld [", i);
        while (block)
        {
            printf("%p, ", block);
            block = block->next;
        }
        printf("]\n");
    }
}

#endif
