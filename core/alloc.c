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

#include <stdlib.h>
#include <math.h>

static alloc_t _ALLOC = NULL;

#define MEMBASE ((i64_t)_ALLOC->pool)
#define offset(b) ((i64_t)b - MEMBASE)
#define _buddyof(b, i) (offset(b) ^ (1 << (i)))
#define buddyof(b, i) ((null_t *)(_buddyof(b, i) + MEMBASE))
#define blocksize(i) (1 << (i))

alloc_t rf_alloc_init()
{
    _ALLOC = (alloc_t)mmap(NULL, sizeof(struct alloc_t),
                           PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    memset(_ALLOC, 0, sizeof(struct alloc_t));
    _ALLOC->freelist[MAX_ORDER] = _ALLOC->pool;

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
            node = node->next;
        }
        printf("]\n");
    }
}

null_t *rf_malloc(i32_t size)
{
    i32_t i = 0, order;
    null_t *block, *buddy;
    node_t *node;

    // calculate minimal order for this size
    size += sizeof(node_t);
    order = 32 - __builtin_clz(size - 1);
    i = order;

    // debug("malloc: size: %d, order: %d", size, order);
    // debug_blocks();

    // level up until non-null list found
    for (;; i++)
    {
        if (i > MAX_ORDER)
            return NULL;
        if (_ALLOC->freelist[i])
            break;
    }

    // remove the block out of list
    block = _ALLOC->freelist[i];
    _ALLOC->freelist[i] = ((node_t *)block)->next;

    // split until i == order
    while (i-- > order)
    {
        buddy = buddyof(block, i); // get the buddy block
        ((node_t *)buddy)->next = _ALLOC->freelist[i];
        _ALLOC->freelist[i] = block;
        block = buddy;
    }

    return (null_t *)(((node_t *)block) + 1);
}

null_t rf_free(null_t *ptr)
{
}

null_t *rf_realloc(null_t *ptr, i32_t new_size)
{

    null_t *new_ptr = rf_malloc(new_size);
    if (ptr)
        memcpy(new_ptr, ptr, new_size);
    return new_ptr;
}
