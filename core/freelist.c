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

#include "freelist.h"
#include "heap.h"

freelist_p freelist_create(i64_t size) {
    freelist_p freelist;

    freelist = (freelist_p)heap_alloc(sizeof(struct freelist_t));
    freelist->data = (i64_t *)heap_alloc(size * sizeof(i64_t));
    freelist->data_pos = 0;
    freelist->free = (i64_t *)heap_alloc(size * sizeof(i64_t));
    freelist->data_size = size;
    freelist->free_size = size;
    freelist->free_pos = 0;

    return freelist;
}

nil_t freelist_free(freelist_p freelist) {
    heap_free(freelist->data);
    heap_free(freelist->free);
    heap_free(freelist);
}

i64_t freelist_push(freelist_p freelist, i64_t val) {
    i64_t pos;

    if (freelist->free_pos > 0) {
        pos = freelist->free[--freelist->free_pos];
        freelist->data[pos] = val;
        return pos;
    }

    if (freelist->data_pos == freelist->data_size) {
        freelist->data_size *= 2;
        freelist->data = (i64_t *)heap_realloc(freelist->data, freelist->data_size * sizeof(i64_t));
    }

    pos = freelist->data_pos;
    freelist->data[pos] = val;
    freelist->data_pos++;

    return pos;
}

i64_t freelist_pop(freelist_p freelist, i64_t pos) {
    i64_t val;

    if (pos < 0 || pos >= freelist->data_pos)
        return NULL_I64;

    if (freelist->free_pos == freelist->free_size) {
        freelist->free_size *= 2;
        freelist->free = (i64_t *)heap_realloc(freelist->free, freelist->free_size * sizeof(i64_t));
    }

    val = freelist->data[pos];
    freelist->free[freelist->free_pos++] = pos;
    freelist->data[pos] = NULL_I64;

    return val;
}

i64_t freelist_get(freelist_p freelist, i64_t idx) {
    if (idx < 0 || idx >= freelist->data_pos)
        return NULL_I64;

    return freelist->data[idx];
}
