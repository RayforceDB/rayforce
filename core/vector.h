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

/*
 * Each vector capacity is always factor of 8
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
#define push(v, t, x)                               \
    {                                               \
        reserve(v, t, 1);                           \
        ((t *)(as_string(v)))[(v)->adt->len++] = x; \
    }

#define pop(v, t) ((t *)(as_string(v)))[--(v)->adt->len]

/*
 * Attemts to make vector from list if all elements are of the same type
 * l - list
 * v - vector
 * f - function to push element to vector
 * t - member type of element to push
 */
#define flatten(l, v, t)                        \
    {                                           \
        rf_object_t *member;                    \
        i64_t i;                                \
        i8_t type = as_list(&l)[0].type;        \
        v = vector_##t(0);                      \
                                                \
        for (i = 0; i < l.adt->len; i++)        \
        {                                       \
            member = &as_list(&l)[i];           \
                                                \
            if (member->type != type)           \
            {                                   \
                rf_object_free(&vec);           \
                return list;                    \
            }                                   \
                                                \
            vector_##t##_push(&vec, member->t); \
        }                                       \
    }

extern rf_object_t list_flatten(rf_object_t object);
extern i64_t vector_i64_find(rf_object_t *vector, i64_t key);
extern i64_t vector_f64_find(rf_object_t *vector, f64_t key);
extern i64_t list_find(rf_object_t *vector, rf_object_t key);
extern i64_t vector_find(rf_object_t *vector, rf_object_t key);
extern rf_object_t vector_get(rf_object_t *vector, rf_object_t key);
extern i64_t vector_push(rf_object_t *vector, rf_object_t object);
extern i64_t vector_bool_push(rf_object_t *vector, bool_t value);
extern i64_t vector_i64_push(rf_object_t *vector, i64_t value);
extern i64_t vector_f64_push(rf_object_t *vector, f64_t value);
extern i64_t vector_symbol_push(rf_object_t *vector, rf_object_t object);
extern i64_t list_push(rf_object_t *vector, rf_object_t object);
extern i64_t vector_i64_pop(rf_object_t *vector);
extern f64_t vector_f64_pop(rf_object_t *vector);
extern rf_object_t list_pop(rf_object_t *vector);
null_t vector_reserve(rf_object_t *vector, u32_t len);
#endif
