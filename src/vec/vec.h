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

#ifndef RAY_VEC_H
#define RAY_VEC_H

/*
 * vec.h -- Vector operations.
 *
 * Vectors are ray_t blocks with positive type tags. Data follows the 32-byte
 * header. Supports append, get, set, slice (zero-copy), concat. Null state
 * is encoded in-band via type-correct NULL_* sentinels (see vec.c).
 */

#include <rayforce.h>
#include "mem/heap.h"

/* Conservative null-check gate, not an exact count of nulls. SYM/STR encode
 * null as id 0 / length 0 independently of metadata, including old persisted
 * columns and raw gathers. Other vectors retain their HAS_NULLS fast path.
 * Resolve slices here so callers never interpret their view attrs as proof
 * that the underlying payload is null-free. This is a row-kernel gate, not
 * a reason to reject a text optimization: equality can compare canonical
 * payloads directly; paths requiring null-free data use has_nulls below. */
static inline bool ray_vec_may_have_nulls(const ray_t* v) {
    if (!v) return false;
    if (v->type == RAY_SYM || v->type == RAY_STR) return true;
    while ((v->attrs & RAY_ATTR_SLICE) && v->slice_parent)
        v = v->slice_parent;
    return (v->attrs & RAY_ATTR_HAS_NULLS) != 0;
}

/* Exact admission check for optimizations which require null-free input.
 * Text columns cannot prove that from attrs; inspect their payload instead.
 * Keep this out of per-row loops (use may_have_nulls + is_null there). */
static inline bool ray_vec_has_nulls(const ray_t* v) {
    if (!ray_vec_may_have_nulls(v)) return false;
    for (int64_t i = 0; i < v->len; i++)
        if (ray_vec_is_null((ray_t*)v, i)) return true;
    return false;
}

/* Copy null bits from src to dst (sentinel-based). dst and src must have
 * the same length. Internal helper. */
ray_err_t ray_vec_copy_nulls(ray_t* dst, const ray_t* src);

#endif /* RAY_VEC_H */
