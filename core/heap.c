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

#ifndef __EMSCRIPTEN__
RAYASSERT(sizeof(struct block_t) == (2 * sizeof(struct obj_t)), heap_h);
#endif

#define BLOCKSIZE(s) (sizeof(struct obj_t) + (s))
#define BSIZEOF(o) (1ll << (i64_t)(o))
#define BUDDYOF(b, o) ((block_p)((i64_t)(b)->pool + (((i64_t)(b) - (i64_t)(b)->pool) ^ BSIZEOF(o))))
#define ORDEROF(s) (64ll - __builtin_clzll((s) - 1))
#define BLOCK2RAW(b) ((raw_p)((i64_t)(b) + sizeof(struct obj_t)))
#define RAW2BLOCK(r) ((block_p)((i64_t)(r) - sizeof(struct obj_t)))
#define DEFAULT_HEAP_SWAP "./"

// Access heap through current VM - all heap operations use this macro
#define __HEAP (__VM->heap)

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

nil_t heap_destroy(heap_p heap) {
    i64_t i;
    block_p block, next;

    if (heap == NULL)
        return;

    LOG_INFO("Destroying heap");

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

heap_p heap_get(nil_t) {
    LOG_TRACE("Getting heap instance");
    return __HEAP;
}

#ifdef SYS_MALLOC

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

    LOG_TRACE("Adding pool of size %lld", size);

    block = (block_p)mmap_alloc(size);

    if (block == NULL) {
        // Try to mmap with a file
        id = ops_rand_u64();
        snprintf(filename, sizeof(filename), "%svec_%llu.dat", __HEAP->swap_path, id);
        fd = fs_fopen(filename, ATTR_RDWR | ATTR_CREAT);

        if (fd == -1) {
            perror("can't create mmap backed file");
            return NULL;
        }

        // Set initial file size if the file
        if (fs_file_extend(fd, size) == -1) {
            perror("can't truncate mmap backed file");
            fs_fclose(fd);
            return NULL;
        }

        block = (block_p)mmap_file_shared(fd, NULL, size, 0);

        if (block == NULL) {
            fs_fclose(fd);
            perror("can't mmap file");
            return NULL;
        }

        block->pool = (block_p)fd;
        block->backed = B8_TRUE;
    } else {
        block->pool = block;
        block->backed = B8_FALSE;
    }

    block->pool_order = ORDEROF(size);

    __HEAP->memstat.system += size;
    __HEAP->memstat.heap += size;

    return block;
}

nil_t heap_remove_pool(block_p block, i64_t size) {
    mmap_free(block, size);

    __HEAP->memstat.system -= size;
    __HEAP->memstat.heap -= size;
}

inline __attribute__((always_inline)) nil_t heap_insert_block(block_p block, i64_t order) {
    i64_t size = BSIZEOF(order);

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

inline __attribute__((always_inline)) nil_t heap_remove_block(block_p block, i64_t order) {
    if (block->prev)
        block->prev->next = block->next;
    if (block->next)
        block->next->prev = block->prev;

    if (__HEAP->freelist[order] == block)
        __HEAP->freelist[order] = block->next;

    if (__HEAP->freelist[order] == NULL)
        __HEAP->avail &= ~BSIZEOF(order);
}

inline __attribute__((always_inline)) nil_t heap_split_block(block_p block, i64_t block_order, i64_t order) {
    block_p buddy;

    while ((order--) > block_order) {
        buddy = (block_p)((i64_t)block + BSIZEOF(order));
        buddy->pool = block->pool;
        buddy->pool_order = block->pool_order;
        heap_insert_block(buddy, order);
    }
}

raw_p heap_mmap(i64_t size) {
    raw_p ptr = mmap_alloc(size);

    if (ptr == NULL)
        return NULL;

    // __HEAP->memstat.system += size;

    return ptr;
}

raw_p heap_stack(i64_t size) {
    raw_p ptr = mmap_stack(size);

    if (ptr == NULL)
        return NULL;

    __HEAP->memstat.system += size;

    return ptr;
}

raw_p __attribute__((hot)) heap_alloc(i64_t size) {
    i64_t i, order, block_size;
    block_p block;

    if (size == 0 || size > BSIZEOF(MAX_POOL_ORDER))
        return NULL;

    block_size = BLOCKSIZE(size);

    // calculate minimal order for this size
    order = ORDEROF(block_size);

    // find least order block that fits
    i = (AVAIL_MASK << order) & __HEAP->avail;

    // no free block found for this size, so mmap it directly if it is bigger than pool size or
    // add a new pool and split as well
    if (i == 0) {
        if (order >= MAX_BLOCK_ORDER) {
            LOG_TRACE("Adding pool of size %lld requested size %lld", BSIZEOF(order), size);
            size = BSIZEOF(order);
            block = heap_add_pool(size);

            if (block == NULL)
                return NULL;

            block->order = order;
            block->used = 1;
            block->heap_id = __HEAP->id;

            __HEAP->memstat.system += size;

            return BLOCK2RAW(block);
        }

        block = heap_add_pool(BSIZEOF(MAX_BLOCK_ORDER));

        if (block == NULL)
            return NULL;

        i = MAX_BLOCK_ORDER;
        heap_insert_block(block, i);
    } else
        i = __builtin_ctzll(i);

    // remove the block out of list
    block = __HEAP->freelist[i];

    __HEAP->freelist[i] = block->next;
    if (__HEAP->freelist[i] != NULL)
        __HEAP->freelist[i]->prev = NULL;
    else
        __HEAP->avail &= ~BSIZEOF(i);

    heap_split_block(block, order, i);

    block->order = order;
    block->used = 1;
    block->heap_id = __HEAP->id;
    block->backed = B8_FALSE;

    return BLOCK2RAW(block);
}

__attribute__((hot)) nil_t heap_free(raw_p ptr) {
    block_p block, buddy;
    i64_t fd, res;
    i64_t order;
    c8_t filename[64];

    if (ptr == NULL || ptr == NULL_OBJ)
        return;

    block = RAW2BLOCK(ptr);
    order = block->order;

    // Validate block metadata - detect memory corruption or invalid pointers
    // backed should only be 0 or 1, order should be >= MIN_BLOCK_ORDER for heap blocks
    if (block->backed != B8_FALSE && block->backed != B8_TRUE) {
        obj_p obj = (obj_p)ptr;
        fprintf(stderr, "heap_free: corrupted block header (backed=%d, order=%d) at ptr=%p, type=%d\n", block->backed,
                block->order, ptr, obj->type);
        fprintf(stderr, "  This usually indicates a buffer overflow or use-after-free bug.\n");
        assert(0 && "heap_free: corrupted block header");
    }

    // Return block to the system and close file if it is file-backed
    if (block->backed) {
        fd = (i64_t)block->pool;
        heap_remove_pool(block, BSIZEOF(order));
        // Get filename before closing - ignore errors as file may already be gone
        res = fs_get_fname_by_fd(fd, filename, sizeof(filename));
        fs_fclose(fd);
        if (res == 0)
            fs_fdelete(filename);

        return;
    }

    if (__HEAP->id != 0 && block->heap_id != __HEAP->id) {
        block->next = __HEAP->foreign_blocks;
        __HEAP->foreign_blocks = block;
        return;
    }

    for (;; order++) {
        // check if we are at the root block (no buddies left)
        if (block->pool_order == order)
            return heap_insert_block(block, order);

        // calculate buddy
        buddy = BUDDYOF(block, order);

        // buddy is used, or buddy is of different order, so we can't merge
        if (buddy->used || buddy->order != order)
            return heap_insert_block(block, order);

        // merge blocks: remove buddy from its freelist.
        heap_remove_block(buddy, order);

        // check if buddy is lower address than block (means it is of higher order), if so, swap them
        block = (buddy < block) ? buddy : block;
    }
}

__attribute__((hot)) raw_p heap_realloc(raw_p ptr, i64_t new_size) {
    block_p block;
    i64_t i, old_size, cap, order;
    raw_p new_ptr;

    if (ptr == NULL)
        return heap_alloc(new_size);

    block = RAW2BLOCK(ptr);
    old_size = BSIZEOF(block->order);
    cap = BLOCKSIZE(new_size);
    order = ORDEROF(cap);

    if (block->order == order)
        return ptr;

    // grow or block is not in the same heap
    if (order > block->order || (__HEAP->id != 0 && block->heap_id != __HEAP->id) || block->backed) {
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
    heap_split_block(block, order, i);

    return ptr;
}

nil_t heap_unmap(raw_p ptr, i64_t size) {
    mmap_free(ptr, size);
    __HEAP->memstat.system -= size;
}

i64_t heap_gc(nil_t) {
    i64_t i, size, total = 0;
    block_p block, next;

    for (i = MAX_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++) {
        block = __HEAP->freelist[i];
        size = BSIZEOF(i);

        while (block) {
            next = block->next;

            if (i == block->pool_order) {
                heap_remove_block(block, i);
                heap_remove_pool(block, size);
                total += size;
            }

            block = next;
        }
    }

    return total;
}

nil_t heap_borrow(heap_p heap) {
    i64_t i;

    for (i = MAX_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++) {
        // Only borrow if the source heap has a freelist[i] and it has more than one node and it is the pool (not a
        // splitted block)
        if (__HEAP->freelist[i] == NULL || __HEAP->freelist[i]->next == NULL || __HEAP->freelist[i]->pool_order != i)
            continue;

        heap->freelist[i] = __HEAP->freelist[i];
        __HEAP->freelist[i] = __HEAP->freelist[i]->next;
        __HEAP->freelist[i]->prev = NULL;

        heap->freelist[i]->next = NULL;
        heap->freelist[i]->prev = NULL;
        heap->avail |= BSIZEOF(i);
    }
}

nil_t heap_merge(heap_p heap) {
    i64_t i;
    block_p block, last;

    // First traverse foreign blocks and free them
    block = heap->foreign_blocks;
    while (block != NULL) {
        last = block;
        block = block->next;
        last->heap_id = __HEAP->id;
        heap_free(BLOCK2RAW(last));
    }

    heap->foreign_blocks = NULL;

    for (i = MIN_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++) {
        block = heap->freelist[i];
        last = NULL;

        while (block != NULL) {
            last = block;
            block = block->next;
        }

        if (last != NULL) {
            last->next = __HEAP->freelist[i];

            if (__HEAP->freelist[i] != NULL)
                __HEAP->freelist[i]->prev = last;

            __HEAP->freelist[i] = heap->freelist[i];

            heap->freelist[i] = NULL;
        }
    }

    __HEAP->avail |= heap->avail;
    heap->avail = 0;
}

memstat_t heap_memstat(nil_t) {
    i64_t i;
    block_p block;

    __HEAP->memstat.free = 0;

    // calculate free blocks
    for (i = MIN_BLOCK_ORDER; i <= MAX_POOL_ORDER; i++) {
        block = __HEAP->freelist[i];
        while (block) {
            __HEAP->memstat.free += BLOCKSIZE(i);
            block = block->next;
        }
    }

    return __HEAP->memstat;
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
