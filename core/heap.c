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
#include "fs.h"
#include "util.h"
#include "string.h"
#include "os.h"
#include "log.h"
#include "eval.h"
#include "error.h"

// Slab cache helpers
#define SLAB_ORDER_MIN MIN_BLOCK_ORDER
#define SLAB_ORDER_MAX (MIN_BLOCK_ORDER + SLAB_ORDERS - 1)
#define IS_SLAB_ORDER(o) ((o) >= SLAB_ORDER_MIN && (o) <= SLAB_ORDER_MAX)
#define SLAB_INDEX(o) ((o) - SLAB_ORDER_MIN)

#ifndef __EMSCRIPTEN__
RAY_ASSERT(sizeof(struct block_t) == (2 * sizeof(struct obj_t)), "block_t must be 2x obj_t");
#endif

#define BLOCKSIZE(s) (sizeof(struct obj_t) + (s))
#define BSIZEOF(o) (1ll << (i64_t)(o))
#define BUDDYOF(b, o) ((block_p)((i64_t)(b)->pool + (((i64_t)(b) - (i64_t)(b)->pool) ^ BSIZEOF(o))))
#define ORDEROF(s) (64ll - __builtin_clzll((s) - 1))
#define BLOCK2RAW(b) ((raw_p)((i64_t)(b) + sizeof(struct obj_t)))
#define RAW2BLOCK(r) ((block_p)((i64_t)(r) - sizeof(struct obj_t)))
#define DEFAULT_HEAP_SWAP "./"

heap_p heap_create(i64_t id) {
    heap_p heap;

    LOG_INFO("Creating heap with id %lld", id);
    heap = (heap_p)mmap_alloc(sizeof(struct heap_t));

    if (heap == NULL) {
        LOG_ERROR("Failed to allocate heap: %s", strerror(errno));
        exit(1);
    }

    heap->id = id;
    heap->avail = 0;
    heap->foreign_blocks = NULL;

    memset(heap->freelist, 0, sizeof(heap->freelist));
    memset(heap->slabs, 0, sizeof(heap->slabs));

    // Initialize swap path from environment or use default
    if (os_get_var("HEAP_SWAP", heap->swap_path, sizeof(heap->swap_path)) == -1) {
        snprintf(heap->swap_path, sizeof(heap->swap_path), "%s", DEFAULT_HEAP_SWAP);
    } else {
        size_t len = strnlen(heap->swap_path, sizeof(heap->swap_path));

        // Treat empty or truncated values as unset
        if (len == 0 || len >= sizeof(heap->swap_path) - 1) {
            snprintf(heap->swap_path, sizeof(heap->swap_path), "%s", DEFAULT_HEAP_SWAP);
            len = strnlen(heap->swap_path, sizeof(heap->swap_path));
        }

        if (heap->swap_path[len - 1] != '/' && len < sizeof(heap->swap_path) - 1) {
            heap->swap_path[len++] = '/';
            heap->swap_path[len] = '\0';
        }
    }

    LOG_DEBUG("Heap created successfully with swap path: %s", heap->swap_path);
    return heap;
}

// Defined after #ifdef blocks to use heap_flush_slabs
nil_t heap_destroy(heap_p heap);

heap_p heap_get(nil_t) {
    LOG_TRACE("Getting heap instance");
    return VM->heap;
}

#ifdef SYS_MALLOC

static nil_t heap_flush_slabs(heap_p heap) { UNUSED(heap); }  // No-op for system malloc

raw_p heap_alloc(i64_t size) { return malloc(size); }
raw_p heap_mmap(i64_t size) { return mmap_alloc(size); }
raw_p heap_stack(i64_t size) { return mmap_stack(size); }
nil_t heap_free(raw_p ptr) {
    if (ptr != NULL && ptr != NULL_OBJ)
        free(ptr);
}
raw_p heap_realloc(raw_p ptr, i64_t size) { return realloc(ptr, size); }
nil_t heap_unmap(raw_p ptr, i64_t size) { mmap_free(ptr, size); }
i64_t heap_gc(nil_t) { return 0; }
nil_t heap_borrow(heap_p heap) { UNUSED(heap); }
nil_t heap_merge(heap_p heap) { UNUSED(heap); }
memstat_t heap_memstat(nil_t) { return (memstat_t){0}; }

#else

block_p heap_add_pool(i64_t size) {
    i64_t id;
    i64_t fd;
    block_p block;
    c8_t filename[128];
    heap_p heap = VM->heap;  // Cache heap pointer

    LOG_TRACE("Adding pool of size %lld", size);

    block = (block_p)mmap_alloc(size);

    if (block == NULL) {
        // Try to mmap with a file
        id = ops_rand_u64();
        snprintf(filename, sizeof(filename), "%svec_%llu.dat", heap->swap_path, id);
        fd = fs_fopen(filename, ATTR_RDWR | ATTR_CREAT);

        if (fd == -1) {
            perror("mmap:create");
            return NULL;
        }

        // Set initial file size if the file
        if (fs_file_extend(fd, size) == -1) {
            perror("mmap:trunc");
            fs_fclose(fd);
            return NULL;
        }

        block = (block_p)mmap_file_shared(fd, NULL, size, 0);

        if (block == NULL) {
            fs_fclose(fd);
            perror("mmap:map");
            return NULL;
        }

        block->pool = (block_p)fd;
        block->backed = B8_TRUE;
    } else {
        block->pool = block;
        block->backed = B8_FALSE;
    }

    block->pool_order = ORDEROF(size);

    heap->memstat.system += size;
    heap->memstat.heap += size;

    return block;
}

nil_t heap_remove_pool(block_p block, i64_t size) {
    heap_p heap = VM->heap;  // Cache heap pointer
    mmap_free(block, size);

    heap->memstat.system -= size;
    heap->memstat.heap -= size;
}

inline __attribute__((always_inline)) nil_t heap_insert_block(heap_p heap, block_p block, i64_t order) {
    i64_t size = BSIZEOF(order);

    block->prev = NULL;
    block->next = heap->freelist[order];
    block->used = 0;
    block->order = order;

    if (heap->freelist[order] != NULL)
        heap->freelist[order]->prev = block;
    else
        heap->avail |= size;

    heap->freelist[order] = block;
}

inline __attribute__((always_inline)) nil_t heap_remove_block(heap_p heap, block_p block, i64_t order) {
    if (block->prev)
        block->prev->next = block->next;
    if (block->next)
        block->next->prev = block->prev;

    if (heap->freelist[order] == block)
        heap->freelist[order] = block->next;

    if (heap->freelist[order] == NULL)
        heap->avail &= ~BSIZEOF(order);
}

inline __attribute__((always_inline)) nil_t heap_split_block(heap_p heap, block_p block, i64_t block_order,
                                                             i64_t order) {
    block_p buddy;

    while ((order--) > block_order) {
        buddy = (block_p)((i64_t)block + BSIZEOF(order));
        buddy->pool = block->pool;
        buddy->pool_order = block->pool_order;
        heap_insert_block(heap, buddy, order);
    }
}

// Flush slab caches back to freelists for coalescing
static nil_t heap_flush_slabs(heap_p heap) {
    i64_t i;
    block_p block;

    for (i = 0; i < SLAB_ORDERS; i++) {
        while (heap->slabs[i].count > 0) {
            block = heap->slabs[i].stack[--heap->slabs[i].count];
            // heap_insert_block will set used=0
            heap_insert_block(heap, block, SLAB_ORDER_MIN + i);
        }
    }
}

raw_p heap_mmap(i64_t size) {
    raw_p ptr = mmap_alloc(size);

    if (ptr == NULL)
        return NULL;

    // HEAP->memstat.system += size;

    return ptr;
}

raw_p heap_stack(i64_t size) {
    raw_p ptr = mmap_stack(size);
    heap_p heap = VM->heap;  // Cache heap pointer

    if (ptr == NULL)
        return NULL;

    heap->memstat.system += size;

    return ptr;
}

raw_p __attribute__((hot)) heap_alloc(i64_t size) {
    i64_t i, order, block_size;
    block_p block;
    heap_p heap = VM->heap;  // Cache heap pointer to avoid repeated VM calls

    if (UNLIKELY(size == 0 || size > BSIZEOF(MAX_POOL_ORDER)))
        return NULL;

    block_size = BLOCKSIZE(size);

    // calculate minimal order for this size
    order = ORDEROF(block_size);

    // Fast path: check slab cache for small allocations
    if (LIKELY(IS_SLAB_ORDER(order))) {
        i64_t idx = SLAB_INDEX(order);
        if (LIKELY(heap->slabs[idx].count > 0)) {
            block = heap->slabs[idx].stack[--heap->slabs[idx].count];
            // Note: block->order, pool_order, and backed should already be valid
            // from when the block was first allocated. Just update used and heap_id.
            block->used = 1;
            block->heap_id = heap->id;
            return BLOCK2RAW(block);
        }
    }

    // find least order block that fits
    i = (AVAIL_MASK << order) & heap->avail;

    // no free block found for this size, so mmap it directly if it is bigger than pool size or
    // add a new pool and split as well
    if (UNLIKELY(i == 0)) {
        if (order >= MAX_BLOCK_ORDER) {
            LOG_TRACE("Adding pool of size %lld requested size %lld", BSIZEOF(order), size);
            size = BSIZEOF(order);
            block = heap_add_pool(size);

            if (UNLIKELY(block == NULL))
                return NULL;

            block->order = order;
            block->used = 1;
            block->heap_id = heap->id;

            heap->memstat.system += size;

            return BLOCK2RAW(block);
        }

        block = heap_add_pool(BSIZEOF(MAX_BLOCK_ORDER));

        if (UNLIKELY(block == NULL))
            return NULL;

        i = MAX_BLOCK_ORDER;
        heap_insert_block(heap, block, i);
    } else
        i = __builtin_ctzll(i);

    // remove the block out of list
    block = heap->freelist[i];

    heap->freelist[i] = block->next;
    if (heap->freelist[i] != NULL)
        heap->freelist[i]->prev = NULL;
    else
        heap->avail &= ~BSIZEOF(i);

    heap_split_block(heap, block, order, i);

    block->order = order;
    block->used = 1;
    block->heap_id = heap->id;
    block->backed = B8_FALSE;

    return BLOCK2RAW(block);
}

__attribute__((hot)) nil_t heap_free(raw_p ptr) {
    block_p block, buddy;
    i64_t fd, res;
    i64_t order;
    c8_t filename[64];
    heap_p heap = VM->heap;  // Cache heap pointer

    if (UNLIKELY(ptr == NULL || ptr == NULL_OBJ))
        return;

    block = RAW2BLOCK(ptr);
    order = block->order;

    // Validate block metadata - detect memory corruption or invalid pointers
    // backed should only be 0 or 1, order should be in valid range
    if (UNLIKELY(block->backed != B8_FALSE && block->backed != B8_TRUE)) {
        obj_p obj = (obj_p)ptr;
        PANIC("block: b=%d o=%d p=%p t=%d", block->backed, block->order, ptr, obj->type);
    }

    // Validate order is in valid range (detect corruption or external objects)
    // External/mmap'd objects shouldn't be freed via heap_free
    if (UNLIKELY(order < MIN_BLOCK_ORDER || order > MAX_POOL_ORDER))
        return;

    // Return block to the system and close file if it is file-backed
    if (UNLIKELY(block->backed)) {
        fd = (i64_t)block->pool;
        heap_remove_pool(block, BSIZEOF(order));
        // Get filename before closing - ignore errors as file may already be gone
        res = fs_get_fname_by_fd(fd, filename, sizeof(filename));
        fs_fclose(fd);
        if (res == 0)
            fs_fdelete(filename);

        return;
    }

    // Fast path: push to slab cache for small blocks (same heap only)
    if (heap != NULL && order >= MIN_BLOCK_ORDER && IS_SLAB_ORDER(order) &&
        (heap->id == 0 || block->heap_id == heap->id)) {
        i64_t idx = SLAB_INDEX(order);
        if (heap->slabs[idx].count < SLAB_CACHE_SIZE) {
            heap->slabs[idx].stack[heap->slabs[idx].count++] = block;
            return;
        }
    }

    if (UNLIKELY(heap->id != 0 && block->heap_id != heap->id)) {
        block->next = heap->foreign_blocks;
        heap->foreign_blocks = block;
        return;
    }

    for (;; order++) {
        // check if we are at the root block (no buddies left)
        if (block->pool_order == order)
            return heap_insert_block(heap, block, order);

        // calculate buddy and prefetch its metadata
        buddy = BUDDYOF(block, order);
        __builtin_prefetch(buddy, 0, 1);  // read, low temporal locality

        // buddy is used, or buddy is of different order, so we can't merge
        if (buddy->used || buddy->order != order)
            return heap_insert_block(heap, block, order);

        // merge blocks: remove buddy from its freelist.
        heap_remove_block(heap, buddy, order);

        // check if buddy is lower address than block (means it is of higher order), if so, swap them
        block = (buddy < block) ? buddy : block;
    }
}

__attribute__((hot)) raw_p heap_realloc(raw_p ptr, i64_t new_size) {
    block_p block;
    i64_t i, old_size, cap, order;
    raw_p new_ptr;
    heap_p heap = VM->heap;  // Cache heap pointer

    if (ptr == NULL)
        return heap_alloc(new_size);

    block = RAW2BLOCK(ptr);
    old_size = BSIZEOF(block->order);
    cap = BLOCKSIZE(new_size);
    order = ORDEROF(cap);

    if (block->order == order)
        return ptr;

    // grow or block is not in the same heap
    if (order > block->order || (heap->id != 0 && block->heap_id != heap->id) || block->backed) {
        new_ptr = heap_alloc(new_size);

        if (new_ptr == NULL) {
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
    heap_split_block(heap, block, order, i);

    return ptr;
}

nil_t heap_unmap(raw_p ptr, i64_t size) {
    heap_p heap = VM->heap;  // Cache heap pointer
    mmap_free(ptr, size);
    heap->memstat.system -= size;
}

i64_t heap_gc(nil_t) {
    i64_t i, size, total = 0;
    block_p block, next;
    heap_p h = VM->heap;  // Cache heap pointer

    // Flush slab caches to allow coalescing
    heap_flush_slabs(h);

    for (i = MAX_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++) {
        block = h->freelist[i];
        size = BSIZEOF(i);

        while (block) {
            next = block->next;

            if (i == block->pool_order) {
                heap_remove_block(h, block, i);
                heap_remove_pool(block, size);
                total += size;
            }

            block = next;
        }
    }

    return total;
}

nil_t heap_borrow(heap_p heap) {
    i64_t i, j, half;
    heap_p h = VM->heap;  // Cache heap pointer (source heap)

    // Transfer half of slab cache entries to worker (improves small object alloc)
    for (i = 0; i < SLAB_ORDERS; i++) {
        half = h->slabs[i].count / 2;
        for (j = 0; j < half; j++) {
            heap->slabs[i].stack[heap->slabs[i].count++] = h->slabs[i].stack[--h->slabs[i].count];
        }
    }

    // Borrow medium blocks (orders 20-24: 1MB-16MB) for common allocations
    for (i = 20; i < MAX_BLOCK_ORDER; i++) {
        // Only borrow if source has 2+ blocks at this order
        if (h->freelist[i] == NULL || h->freelist[i]->next == NULL)
            continue;

        heap->freelist[i] = h->freelist[i];
        h->freelist[i] = h->freelist[i]->next;
        h->freelist[i]->prev = NULL;

        heap->freelist[i]->next = NULL;
        heap->freelist[i]->prev = NULL;
        heap->avail |= BSIZEOF(i);
    }

    // Borrow large pool blocks (>=32MB) for big allocations
    for (i = MAX_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++) {
        // Only borrow if source has freelist[i] with >1 node and it's a full pool
        if (h->freelist[i] == NULL || h->freelist[i]->next == NULL || h->freelist[i]->pool_order != i)
            continue;

        heap->freelist[i] = h->freelist[i];
        h->freelist[i] = h->freelist[i]->next;
        h->freelist[i]->prev = NULL;

        heap->freelist[i]->next = NULL;
        heap->freelist[i]->prev = NULL;
        heap->avail |= BSIZEOF(i);
    }
}

nil_t heap_merge(heap_p heap) {
    i64_t i;
    block_p block, next, last;
    heap_p h = VM->heap;  // Cache heap pointer (destination heap)

    // Transfer slab caches back to main heap (if room), else flush to freelist
    for (i = 0; i < SLAB_ORDERS; i++) {
        // Transfer as many as fit into main's slab cache
        while (heap->slabs[i].count > 0 && h->slabs[i].count < SLAB_CACHE_SIZE) {
            h->slabs[i].stack[h->slabs[i].count++] = heap->slabs[i].stack[--heap->slabs[i].count];
        }
        // Flush remaining to main heap's freelist
        while (heap->slabs[i].count > 0) {
            block = heap->slabs[i].stack[--heap->slabs[i].count];
            heap_insert_block(h, block, SLAB_ORDER_MIN + i);
        }
    }

    // Return foreign blocks via normal free path (includes coalescing)
    block = heap->foreign_blocks;
    while (block != NULL) {
        next = block->next;
        block->heap_id = h->id;
        heap_free(BLOCK2RAW(block));
        block = next;
    }
    heap->foreign_blocks = NULL;

    // Merge freelists: O(1) prepend by finding tail, linking to main head
    for (i = MIN_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++) {
        if (heap->freelist[i] == NULL)
            continue;

        // Find tail of worker's freelist
        last = heap->freelist[i];
        while (last->next != NULL)
            last = last->next;

        // Link: worker_tail -> main_head, main_head = worker_head
        last->next = h->freelist[i];
        if (h->freelist[i] != NULL)
            h->freelist[i]->prev = last;

        h->freelist[i] = heap->freelist[i];
        heap->freelist[i] = NULL;
    }

    h->avail |= heap->avail;
    heap->avail = 0;
}

memstat_t heap_memstat(nil_t) {
    i64_t i;
    block_p block;
    heap_p h = VM->heap;  // Cache heap pointer

    h->memstat.free = 0;

    // calculate free blocks
    for (i = MIN_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++) {
        block = h->freelist[i];
        while (block) {
            h->memstat.free += BLOCKSIZE(i);
            block = block->next;
        }
    }

    return h->memstat;
}

nil_t heap_print_blocks(heap_p heap) {
    i64_t i;
    block_p block;

    printf("-- HEAP[%lld]: BLOCKS:\n", heap->id);
    for (i = 0; i <= MAX_POOL_ORDER; i++) {
        block = heap->freelist[i];
        printf("-- order: %lld [", i);
        while (block) {
            printf("%p, ", block);
            block = block->next;
        }
        printf("]\n");
    }
}

#endif

// heap_destroy defined after #ifdef blocks to use heap_flush_slabs
nil_t heap_destroy(heap_p heap) {
    i64_t i;
    block_p block, next;

    if (heap == NULL)
        return;

    LOG_INFO("Destroying heap");

    // Flush slab caches first
    heap_flush_slabs(heap);

    // Ensure foreign blocks are freed
    if (heap->foreign_blocks != NULL)
        LOG_WARN("Heap[%lld]: foreign blocks not freed", heap->id);

    // All the nodes remains are pools, so just munmap them
    for (i = MIN_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++) {
        block = heap->freelist[i];

        while (block) {
            next = block->next;
            if (i != block->pool_order) {
                LOG_ERROR("Heap[%lld]: leak order: %lld block: %p", heap->id, i, block);
                return;
            }

            mmap_free(block, BSIZEOF(i));
            block = next;
        }
    }

    // munmap heap
    mmap_free(heap, sizeof(struct heap_t));

    LOG_DEBUG("Heap destroyed successfully");
}
