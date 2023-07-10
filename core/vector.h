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
#include "alloc.h"
#include "ops.h"
#include "util.h"

#define VEC_ATTR_WITHOUT_NULLS 1Ull << 63
#define VEC_ATTR_ASC (1Ull << 62)
#define VEC_ATTR_DESC (1Ull << 61)
#define VEC_ATTR_DISTINCT (1Ull << 60)
#define VEC_ATTR_MASK 0xFF << 55

/*
 * Each vector capacity is always factor of 16
 * This allows to avoid storing capacity in vector
 */
#define CAPACITY_FACTOR 16

/*
 * Calculates capacity for vector of length x
 */
#define capacity(x) (ALIGNUP(x, CAPACITY_FACTOR))

/*
 * Reserves memory for n elements
 * v - vector to reserve memory for
 * t - type of element
 * n - number of elements
 */
#define reserve(v, t, n)                                                                                                \
    {                                                                                                                   \
        i64_t occup = (v)->adt->len * sizeof(t) + sizeof(header_t);                                                     \
        i64_t cap = capacity(occup);                                                                                    \
        i64_t req_cap = n * sizeof(t);                                                                                  \
        if (cap < req_cap + occup)                                                                                      \
        {                                                                                                               \
            i64_t new_cap = capacity(cap + req_cap);                                                                    \
            /*debug("realloc: len %lld n %lld from %lld to %lld occup: %lld", (v)->adt->len, n, cap, new_cap, occup);*/ \
            (v)->adt = rf_realloc((v)->adt, new_cap);                                                                   \
        }                                                                                                               \
    }

/*
 * Appends rf_objectect to the end of vector (dynamically grows vector if needed)
 * v - vector to append to
 * t - type of rf_objectect to append
 * x - rf_objectect to append
 */
#define push(v, t, x)                                                      \
    {                                                                      \
        reserve(v, t, 1);                                                  \
        memcpy(as_string(v) + ((v)->adt->len * sizeof(t)), &x, sizeof(t)); \
        (v)->adt->len++;                                                   \
    }

#define pop(v, t) ((t *)(as_string(v)))[--(v)->adt->len]

i64_t size_of_val(type_t type);
i64_t vector_find(rf_object_t *vec, rf_object_t *key);

rf_object_t vector_flatten(rf_object_t *object);
rf_object_t vector_get(rf_object_t *vec, i64_t index);
rf_object_t vector_filter(rf_object_t *vec, bool_t mask[], i64_t len);
rf_object_t vector_set(rf_object_t *vec, i64_t index, rf_object_t value);
rf_object_t vector_push(rf_object_t *vec, rf_object_t object);
rf_object_t rf_list(rf_object_t *x, u32_t n);

null_t vector_reserve(rf_object_t *vec, u32_t len);
null_t vector_grow(rf_object_t *vec, u32_t len);
null_t vector_shrink(rf_object_t *vec, u32_t len);
null_t vector_free(rf_object_t *vec);
null_t vector_clear(rf_object_t *vec);

#endif
