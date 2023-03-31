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

#include "rayforce.h"
#include "alloc.h"
#include "vector.h"
#include "util.h"
#include "ops.h"

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
#define reserve(v, t, n)                                            \
    {                                                               \
        i64_t occup = (v)->adt.len * sizeof(t);                     \
        i64_t cap = capacity(occup);                                \
        i64_t req_cap = n * sizeof(t);                              \
        i64_t new_cap = capacity(cap + req_cap);                    \
        if (cap >= req_cap + occup)                                 \
            ;                                                       \
        else if ((v)->adt.len == 0)                                 \
            (v)->adt.ptr = rayforce_malloc(new_cap);                \
        else                                                        \
            (v)->adt.ptr = rayforce_realloc((v)->adt.ptr, new_cap); \
    }

/*
 * Appends object to the end of vector (dynamically grows vector if needed)
 * v - vector to append to
 * t - type of object to append
 * x - object to append
 */
#define push(v, t, x)                              \
    {                                              \
        reserve(v, t, 1);                          \
        ((t *)((v)->adt.ptr))[(v)->adt.len++] = x; \
    }

#define pop(v, t) ((t *)((v)->adt.ptr))[--(v)->adt.len]

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
        for (i = 0; i < l.adt.len; i++)         \
        {                                       \
            member = &as_list(&l)[i];           \
                                                \
            if (member->type != type)           \
            {                                   \
                object_free(&vec);              \
                return list;                    \
            }                                   \
                                                \
            vector_##t##_push(&vec, member->t); \
        }                                       \
    }

/*
 * Creates new vector of type type
 */
extern rf_object_t vector(i8_t type, i8_t size_of_val, i64_t len)
{
    struct attrs_t *attrs = rayforce_malloc(sizeof(struct attrs_t));
    attrs->rc = 1;
    attrs->index = NULL;

    rf_object_t v = {
        .type = type,
        .adt = {
            .len = len,
            .ptr = NULL,
            .attrs = attrs,
        },
    };

    if (len == 0)
        return v;

    v.adt.ptr = rayforce_malloc(capacity(size_of_val * len));

    return v;
}

extern i64_t vector_i64_push(rf_object_t *vector, i64_t value)
{
    push(vector, i64_t, value);
    return vector->adt.len;
}

extern i64_t vector_i64_pop(rf_object_t *vector)
{
    return pop(vector, i64_t);
}

extern i64_t vector_f64_push(rf_object_t *vector, f64_t value)
{
    push(vector, f64_t, value);
    return vector->adt.len;
}

extern f64_t vector_f64_pop(rf_object_t *vector)
{
    return pop(vector, f64_t);
}

extern i64_t list_push(rf_object_t *list, rf_object_t object)
{
    push(list, rf_object_t, object);
    return list->adt.len;
}

extern rf_object_t list_pop(rf_object_t *list)
{
    rf_object_t object = pop(list, rf_object_t);
    return object_clone(&object);
}

extern i64_t vector_push(rf_object_t *vector, rf_object_t object)
{
    i8_t type = vector->type;

    if (type != 0 && type != -object.type)
        panic("vector_push: type mismatch");

    switch (type)
    {
    case TYPE_I64:
        vector_i64_push(vector, object.i64);
        break;
    case TYPE_F64:
        vector_f64_push(vector, object.f64);
        break;
    case TYPE_SYMBOL:
        vector_i64_push(vector, object.i64);
        break;
    case TYPE_LIST:
        list_push(vector, object);
        break;
    default:
        exit(1);
    }

    return vector->adt.len;
}

extern rf_object_t vector_pop(rf_object_t *vector)
{
    if (vector->adt.len == 0)
        return null();

    i8_t type = vector->type;
    rf_object_t obj;

    switch (type)
    {
    case TYPE_I64:
        return i64(vector_i64_pop(vector));
    case TYPE_F64:
        return f64(vector_f64_pop(vector));
    case TYPE_SYMBOL:
        obj = i64(vector_i64_pop(vector));
        obj.type = -TYPE_SYMBOL;
        return obj;
    case TYPE_LIST:
        return list_pop(vector);
    default:
        exit(1);
    }
}

null_t vector_reserve(rf_object_t *vector, u32_t len)
{
    switch (vector->type)
    {
    case TYPE_I64:
        reserve(vector, i64_t, len);
        break;
    case TYPE_F64:
        reserve(vector, f64_t, len);
        break;
    case TYPE_SYMBOL:
        reserve(vector, i64_t, len);
        break;
    case TYPE_LIST:
        reserve(vector, rf_object_t, len);
        break;
    case TYPE_STRING:
        reserve(vector, i8_t, len);
        break;
    default:
        exit(1);
    }
}

extern i64_t vector_i64_find(rf_object_t *vector, i64_t key)
{
    i64_t *ptr = as_vector_i64(vector);
    i32_t i;

    for (i = 0; i < vector->adt.len; i++)
    {
        if (ptr[i] == key)
            return i;
    }

    return vector->adt.len;
}

extern i64_t vector_f64_find(rf_object_t *vector, f64_t key)
{
    f64_t *ptr = as_vector_f64(vector);
    i32_t i;

    for (i = 0; i < vector->adt.len; i++)
    {
        if (ptr[i] == key)
            return i;
    }

    return vector->adt.len;
}

extern i64_t list_find(rf_object_t *list, rf_object_t key)
{
    rf_object_t *ptr = as_list(list);
    i32_t i;

    for (i = 0; i < list->adt.len; i++)
    {
        if (object_eq(&ptr[i], &key))
            return i;
    }

    return list->adt.len;
}

extern i64_t vector_find(rf_object_t *vector, rf_object_t key)
{
    i8_t type = vector->type;

    switch (type)
    {
    case TYPE_I64:
        return vector_i64_find(vector, key.i64);
    case TYPE_F64:
        return vector_f64_find(vector, key.f64);
    case TYPE_SYMBOL:
        return vector_i64_find(vector, key.i64);
    default:
        return list_find(vector, key);
    }
}

/*
 * Try to flatten list in a vector if all elements are of the same type
 */
extern rf_object_t list_flatten(rf_object_t list)
{
    if (list.type != TYPE_LIST)
        return list;

    i64_t len = list.adt.len;
    i8_t type;
    rf_object_t vec;

    if (len == 0)
        return list;

    type = as_list(&list)[0].type;

    // Only scalar types can be flattened
    if (type > -1)
        return list;

    switch (type)
    {
    case -TYPE_I64:
        flatten(list, vec, i64);
        break;
    case -TYPE_F64:
        flatten(list, vec, f64);
        break;
    case -TYPE_SYMBOL:
        flatten(list, vec, i64);
        if (vec.type == TYPE_I64)
            vec.type = TYPE_SYMBOL;
        break;
    default:
        return list;
    }

    object_free(&list);

    return vec;
}
