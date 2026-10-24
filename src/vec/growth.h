/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef RAY_CELL_GROWTH_H
#define RAY_CELL_GROWTH_H

#include "mem/heap.h"

/* A direct mapping includes a prefix and object header and is page-rounded.
 * Keep a single allocation's mapped size representable in the allocator's
 * signed byte counters as well as size_t. This is a ceiling, not a request. */
static inline size_t ray_cell_growth_limit(void) {
    size_t limit = SIZE_MAX;
    if ((uint64_t)limit > (uint64_t)INT64_MAX) limit = (size_t)INT64_MAX;
    return (limit & ~(size_t)4095) - RAY_DIRECT_HDR - 32;
}

/* Check actual growth requests; no signed sum or byte product may wrap.
 * Bulk APIs preflight externally requested counts before COW/index changes.
 * Physical width matters (notably for narrow SYM). Zero-count raw appends
 * retain their earlier no-op path and need not call this helper. */
static inline bool ray_cell_growth_bytes(int64_t len, int64_t count,
                                         size_t width, size_t* required) {
    if (len < 0 || count < 0 || width == 0 || count > INT64_MAX - len)
        return false;
    uint64_t n = (uint64_t)(len + count);
    if (n > (uint64_t)SIZE_MAX) return false;
    size_t bytes;
    if (__builtin_mul_overflow((size_t)n, width, &bytes) ||
        bytes > ray_cell_growth_limit()) return false;
    *required = bytes;
    return true;
}

/* Called only when required exceeds owned usable room. Buddy rounding of
 * payload+header already supplies geometric growth: do not pre-round payload.
 * Direct mappings instead round to pages, so grow from their actual capacity
 * explicitly. Larger cached mappings and allocator-owned spill use the same
 * capacity rule. Nothing here changes allocation/free/cache policy. */
static inline bool ray_cell_growth_request(size_t room, size_t required,
                                           size_t* request) {
    size_t limit = ray_cell_growth_limit();
    if (required > limit || room > limit) return false;
    size_t bytes = required;
    if (ray_order_for_size(required) >= RAY_HEAP_POOL_ORDER) {
        size_t doubled = room > limit / 2 ? limit : room * 2;
        if (doubled > bytes) bytes = doubled;
    }
    *request = bytes;
    return true;
}

/* Rare allocation geometry must not inflate the register frame of a
 * reserved-capacity single append. Bulk callers preflight their count. */
static __attribute__((cold, noinline, unused))
ray_t* ray_cell_grow_one(ray_t* value, size_t width) {
    size_t required, request;
    if (!ray_cell_growth_bytes(value->len, 1, width, &required) ||
        !ray_cell_growth_request(ray_block_data_bytes(value), required, &request))
        return NULL;
    return ray_scratch_realloc(value, request);
}

#endif /* RAY_CELL_GROWTH_H */
