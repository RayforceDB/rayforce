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

#ifndef RAY_TYPES_H
#define RAY_TYPES_H

/*
 * types.h — Internal types header.
 *
 * The canonical type definitions (ray_t, type constants, attribute flags)
 * live in <rayforce.h> (the public header).
 * Internal .c files can include either rayforce.h directly or types.h.
 */
#include <rayforce.h>

/* Number of types (positive range): must be > max type ID */
#define RAY_TYPE_COUNT 15

/* Type sizes lookup table (defined in types.c) */
extern const uint8_t ray_type_sizes[256];

/* Element size for a given type tag */
#define ray_elem_size(t)  (ray_type_sizes[(t)])

#endif /* RAY_TYPES_H */
