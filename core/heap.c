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

CASSERT(sizeof(struct block_t) == sizeof(struct obj_t), heap_h);

__thread heap_p __HEAP = NULL;

#define blocksize(s) (sizeof(struct block_t) + (s))
#define bsizeof(o) (1ull << (u64_t)(o))
#define buddyof(b, o) ((block_p)((u64_t)__HEAP->memory + (((u64_t)(b) - (u64_t)__HEAP->memory) ^ bsizeof(o))))
#define orderof(s) (64ull - __builtin_clzll((s) - 1))
#define blockprev(b) ((block_p)((u64_t)(b->prev) & ~ORDER_MASK))
#define blockorder(b) ((u64_t)(b->prev) >> ORDER_SHIFT)
#define blockaddr(p, o) ((block_p)((u64_t)(p) | ((u64_t)(o) << ORDER_SHIFT)))
#define obj2raw(o) ((raw_p)(o)->arr)
#define raw2obj(r) ((obj_p)((i64_t)(r) - sizeof(struct obj_t)))

#ifdef SYS_MALLOC

obj_p heap_alloc_obj(u64_t size) { return malloc(sizeof(struct obj_t) + size); }
nil_t heap_free_obj(obj_p obj) { free(obj); }
obj_p heap_realloc_obj(obj_p ptr, u64_t new_size) { return realloc(ptr, sizeof(struct obj_t) + new_size); }
raw_p heap_alloc_raw(u64_t size) { return malloc(size); }
nil_t heap_free_raw(raw_p ptr) { free(ptr); }
raw_p heap_realloc_raw(raw_p ptr, u64_t size) { return realloc(ptr, size); }
i64_t heap_gc(nil_t) { return 0; }
nil_t heap_cleanup(nil_t) {}
nil_t heap_borrow(heap_p) {}
nil_t heap_merge(heap_p) {}
heap_p heap_init(u64_t) { return NULL; }
memstat_t heap_memstat(nil_t) { return (memstat_t){0}; }

#else

heap_p heap_init(u64_t id)
{
    __HEAP = (heap_p)mmap_alloc(sizeof(struct heap_t));
    __HEAP->id = id;
    __HEAP->avail = 0;
    __HEAP->memoffset = 0;
    __HEAP->memory = (block_p)mmap_reserve(POOL_SIZE);

    memset(__HEAP->freelist, 0, sizeof(__HEAP->freelist));

    return __HEAP;
}

block_p heap_add_pool(u64_t size)
{
    block_p block = (block_p)mmap_commit(__HEAP->memory + __HEAP->memoffset, size);

    debug_assert(block != NULL, "Failed to add pool");
    debug_assert((u64_t)block % PAGE_SIZE == 0, "Pool is not page aligned");

    __HEAP->memoffset += bsizeof(MAX_ORDER);
    __HEAP->memstat.system += size;

    return block;
}

nil_t heap_remove_pool(block_p block)
{
    mmap_free(block, bsizeof(MAX_ORDER));
    __HEAP->memstat.system -= bsizeof(MAX_ORDER);
}

// inline __attribute__((always_inline))
nil_t insert_block(block_p block, u64_t order)
{
    u64_t size = bsizeof(order);

    block->prev = blockaddr(NULL, order);
    block->next = __HEAP->freelist[order];

    if (__HEAP->freelist[order] != NULL)
        __HEAP->freelist[order]->prev = blockaddr(block, order);
    else
        __HEAP->avail |= size;

    __HEAP->freelist[order] = block;
}

nil_t remove_block(block_p block, u64_t order)
{
    block_p prev = blockprev(block);

    if (prev)
        prev->next = block->next;
    if (block->next)
        block->next->prev = block->prev;

    if (__HEAP->freelist[order] == block)
        __HEAP->freelist[order] = block->next;

    if (__HEAP->freelist[order] == NULL)
        __HEAP->avail &= ~bsizeof(order);
}

nil_t split_block(block_p block, u64_t block_order, u64_t order)
{
    while ((order--) > block_order)
        insert_block((block_p)((u64_t)block + bsizeof(order)), order);
}

obj_p __attribute__((hot)) heap_alloc_obj(u64_t size)
{
    u64_t i, order, block_size;
    block_p block;
    obj_p obj;

    block_size = blocksize(size);

    // calculate minimal order for this size
    order = orderof(block_size);

    if (order > MAX_POOL_ORDER)
        return NULL;

    // find least order block that fits
    i = (AVAIL_MASK << order) & __HEAP->avail;

    // no free block found for this size, so mmap it directly if it is bigger than pool size or
    // add a new pool and split as well
    if (i == 0)
    {
        if (order >= MAX_ORDER)
        {
            size = bsizeof(order);
            block = heap_add_pool(size);

            if (block == NULL)
                return NULL;

            goto complete;
        }

        block = heap_add_pool(bsizeof(MAX_ORDER));
        i = MAX_ORDER;

        if (block == NULL)
            return NULL;

        insert_block(block, i);
    }
    else
        i = __builtin_ctzll(i);

    // remove the block out of list
    block = __HEAP->freelist[i];

    __HEAP->freelist[i] = block->next;
    if (__HEAP->freelist[i] != NULL)
        __HEAP->freelist[i]->prev = blockaddr(NULL, i);
    else
        __HEAP->avail &= ~bsizeof(i);

    split_block(block, order, i);

complete:
    obj = (obj_p)block;
    obj->mmod = MMOD_INTERNAL;
    obj->order = order;

    return obj;
}

__attribute__((hot)) nil_t heap_free_obj(obj_p obj)
{
    block_p block, buddy;
    obj_p buddy_obj;
    u64_t order;

    if (obj == NULL)
        return;

    order = obj->order;
    block = (block_p)obj;

    // blocks over MAX_ORDER are directly munmaped
    if (order > MAX_ORDER)
        return heap_remove_pool(block);

    for (;; order++)
    {
        // check if we are at the root block (no buddies left)
        if (order == MAX_ORDER)
            return insert_block(block, order);

        // calculate buddy
        buddy = buddyof(block, order);
        buddy_obj = (obj_p)buddy;

        // buddy is used, or buddy is of different order, so we can't merge
        if (buddy_obj->mmod == MMOD_INTERNAL || blockorder(buddy) != order)
            return insert_block(block, order);

        // merge blocks: remove buddy from its freelist.
        remove_block(buddy, order);

        // check if buddy is lower address than block (means it is of higher order), if so, swap them
        block = (buddy < block) ? buddy : block;
    }
}

__attribute__((hot)) obj_p heap_realloc_obj(obj_p obj, u64_t new_size)
{
    block_p block;
    obj_p new_obj;
    u64_t i, old_size, cap, order;

    if (obj == NULL)
        return heap_alloc_obj(new_size);

    if (new_size == 0)
    {
        heap_free_obj(obj);
        return NULL;
    }

    old_size = bsizeof(obj->order);
    block = (block_p)obj;
    cap = blocksize(new_size);
    order = orderof(cap);

    // grow
    if (cap > old_size)
    {
        new_obj = heap_alloc_obj(new_size);

        if (new_obj == NULL)
        {
            heap_free_obj(obj);
            return NULL;
        }

        memcpy(new_obj, obj, old_size);
        new_obj->order = order;

        heap_free_obj(obj);

        return new_obj;
    }

    // resize
    i = obj->order;
    obj->order = order;

    split_block(block, order, i);

    return obj;
}

raw_p heap_alloc_raw(u64_t size)
{
    obj_p obj;

    if (size == 0)
        return NULL;

    obj = heap_alloc_obj(size);

    if (obj == NULL)
        return NULL;

    return obj2raw(obj);
}

raw_p heap_realloc_raw(raw_p ptr, u64_t size)
{
    obj_p obj, new_obj;

    obj = raw2obj(ptr);
    new_obj = heap_realloc_obj(obj, size + sizeof(struct obj_t));

    if (new_obj == NULL)
        return NULL;

    return obj2raw(new_obj);
}

nil_t heap_free_raw(raw_p ptr)
{
    if (ptr == NULL)
        return;

    heap_free_obj(raw2obj(ptr));
}

i64_t heap_gc(nil_t)
{
    u64_t size, total = 0;
    block_p block, next;

    block = __HEAP->freelist[MAX_ORDER];
    size = bsizeof(MAX_ORDER);

    while (block)
    {
        next = block->next;
        mmap_free(block, size);
        total += size;
        block = next;
    }

    __HEAP->freelist[MAX_ORDER] = NULL;
    __HEAP->avail &= ~size;

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
            heap_free_raw(next);
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

    // heap_print_blocks(__HEAP);

    __HEAP->memstat.free = 0;

    // calculate free blocks
    for (i = MIN_ORDER; i <= MAX_POOL_ORDER; i++)
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

    // heap_print_blocks(__HEAP);

    // All the nodes remains are pools, so just munmap them
    for (i = 0; i <= MAX_POOL_ORDER; i++)
    {
        block = __HEAP->freelist[i];
        while (block)
        {
            next = block->next;
            if (i != MAX_ORDER)
            {
                debug("%s-- HEAP[%lld]: leak order: %lld block: %p%s", RED, __HEAP->id, i, block, RESET);
                return;
            }

            mmap_free(block, bsizeof(i));
            block = next;
        }
    }

    mmap_free(__HEAP->memory, POOL_SIZE);
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
