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

#ifndef RAY_BLOCK_H
#define RAY_BLOCK_H

/*
 * block.h — Internal block header utilities.
 *
 * Provides ray_block_size() and ray_block_copy(). The core ray_t struct and
 * accessor macros (ray_type, ray_is_atom, ray_is_vec, ray_len, ray_data,
 * ray_elem_size) are defined in <rayforce.h>.
 */
#include <rayforce.h>
#include <string.h>

/* Compute total block size in bytes (header + data) */
size_t ray_block_size(ray_t* v);

/* Allocate a new block and shallow-copy header + data from src.
 * Retains child refs (STR/LIST/TABLE pointers) via ray_retain_owned_refs.
 * Requires ray_alloc (declared in rayforce.h, provided by the buddy allocator). */
ray_t* ray_block_copy(ray_t* src);

#endif /* RAY_BLOCK_H */
