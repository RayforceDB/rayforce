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

#include "block.h"
#include "core/platform.h"
#include "../mem/heap.h"
#include "../ops/ops.h"
#include "../table/sym.h"

/* Weak stub for ray_alloc — replaced by buddy allocator at link time.
 * Uses ray_vm_alloc (mmap) — page-aligned and zero-filled. */
__attribute__((weak))
ray_t* ray_alloc(size_t size) {
    if (size < 32) size = 32;
    size = (size + 4095) & ~(size_t)4095;
    void* p = ray_vm_alloc(size);
    if (!p) return ray_error("oom", NULL);
    return (ray_t*)p;
}

size_t ray_block_size(ray_t* v) {
    if (ray_is_atom(v)) return 32;
    /* LIST (type=0) stores child pointers */
    if (v->type == RAY_LIST) return 32 + (size_t)ray_len(v) * sizeof(ray_t*);
    /* TABLE / DICT: 2-pointer block [keys, vals] */
    if (v->type == RAY_TABLE || v->type == RAY_DICT) return 32 + 2 * sizeof(ray_t*);
    /* RAY_SEL: variable layout — meta + seg_flags + seg_popcnt + bits */
    if (v->type == RAY_SEL) {
        int64_t nrows = ray_len(v);
        if (nrows < 0) return 32;
        uint32_t n_segs = (uint32_t)((nrows + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);
        uint32_t n_words = (uint32_t)((nrows + 63) / 64);
        size_t dsz = sizeof(ray_sel_meta_t);
        dsz += (n_segs + 7u) & ~(size_t)7;           /* seg_flags, 8-aligned */
        dsz += ((size_t)n_segs * 2 + 7u) & ~(size_t)7; /* seg_popcnt, 8-aligned */
        dsz += (size_t)n_words * 8;                   /* bits */
        return 32 + dsz;
    }
    /* Vectors: header (32 bytes) + len * elem_size.
     * Use ray_sym_elem_size for SYM columns to respect narrow widths. */
    int8_t t = ray_type(v);
    if (t <= 0 || t >= RAY_TYPE_COUNT) return 32;
    return 32 + (size_t)ray_len(v) * ray_sym_elem_size(t, v->attrs);
}

ray_t* ray_block_copy(ray_t* src) {
    size_t sz = ray_block_size(src);
    ray_t* dst = ray_alloc(sz);
    if (!dst) return ray_error("oom", NULL);
    /* Save allocator metadata before memcpy overwrites the header */
    uint8_t new_mmod = dst->mmod;
    uint8_t new_order = dst->order;
    memcpy(dst, src, sz);
    dst->mmod = new_mmod;
    dst->order = new_order;
    ray_atomic_store(&dst->rc, 1);
    if (!ray_retain_owned_refs(dst)) {
        ray_free(dst);
        return ray_error("oom", NULL);
    }
    return dst;
}
