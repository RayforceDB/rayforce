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
 * header. Supports append, get, set, slice (zero-copy), concat, and nullable
 * bitmap (inline for <=128 elements, external for >128).
 */

#include <rayforce.h>

/* Copy null bitmap from src to dst (handles slices, inline, external).
 * dst and src must have the same length. Internal helper. */
ray_err_t ray_vec_copy_nulls(ray_t* dst, const ray_t* src);

/* Return a pointer to the effective null bitmap bytes for `v`, accounting
 * for slice / external / inline / HAS_INDEX storage forms.  Returns NULL
 * when `v` has no nulls (caller should gate on `v->attrs & RAY_ATTR_HAS_NULLS`
 * before calling for the cheap fast-path).
 *
 * On return:
 *   *bit_offset_out (if non-NULL): bit-offset within the returned buffer
 *      that corresponds to v's row 0.  Non-zero only for slices.
 *   *len_bits_out  (if non-NULL): total bits addressable in the buffer.
 *      For inline, this is 128.  For external, it's the ext->len * 8.
 *
 * The returned pointer is valid as long as `v` (and its ext_nullmap /
 * attached index ray_t, if any) are not released or mutated. */
const uint8_t* ray_vec_nullmap_bytes(const ray_t* v,
                                     int64_t* bit_offset_out,
                                     int64_t* len_bits_out);

#endif /* RAY_VEC_H */
