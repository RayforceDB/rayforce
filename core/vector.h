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

#ifndef VECTOR_H
#define VECTOR_H

#include "rayforce.h"
#include "heap.h"
#include "ops.h"
#include "util.h"

#define VEC_ATTR_WITHOUT_NULLS 1 << 31
#define VEC_ATTR_ASC (1 << 30)
#define VEC_ATTR_DESC (1 << 29)
#define VEC_ATTR_DISTINCT (1 << 28)

/*
 * Each vector capacity is always factor of 16
 * This allows to avoid storing capacity in vector
 */
#define CAPACITY_FACTOR 16

i64_t vector_find(obj_t vec, obj_t key);

obj_t vector_get(obj_t vec, i64_t index);
obj_t vector_filter(obj_t vec, bool_t mask[], i64_t len);
obj_t vector_set(obj_t vec, i64_t index, obj_t value);
nil_t vector_write(obj_t vec, i64_t index, obj_t value);
obj_t vector_push(obj_t vec, obj_t obj);
obj_t list_push(obj_t vec, obj_t obj);
obj_t rf_list(obj_t x, u32_t n);
obj_t rf_enlist(obj_t x, u32_t n);

nil_t vector_reserve(obj_t vec, u32_t len);
nil_t vector_grow(obj_t vec, u32_t len);
nil_t shrink(obj_t vec, u32_t len);
nil_t vector_free(obj_t vec);
nil_t vector_clear(obj_t vec);

#endif
