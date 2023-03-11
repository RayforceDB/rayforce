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
#include "storm.h"

// Global allocator reference
alloc_t GLOBAL_A0 = NULL;

extern null_t *storm_malloc(i32_t size)
{
    return malloc(size);
}

extern null_t storm_free(null_t *block)
{
    free(block);
}

extern null_t *storm_realloc(null_t *ptr, i32_t size)
{
    return realloc(ptr, size);
}

extern null_t storm_alloc_init()
{
    alloc_t alloc;

    alloc = (alloc_t)mmap(NULL, sizeof(struct alloc_t),
                          PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    alloc->symbols = symbols_create();

    GLOBAL_A0 = alloc;
}

extern null_t storm_alloc_deinit()
{
    alloc_t alloc = alloc_get();
    symbols_free(alloc->symbols);
    // munmap(GLOBAL_A0, sizeof(struct alloc_t));
}

extern alloc_t alloc_get()
{
    return GLOBAL_A0;
}
