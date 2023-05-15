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
#include "vector.h"
#include "util.h"
#include "format.h"

/*
 * Creates new vector of type type
 */
rf_object_t vector(i8_t type, i8_t size_of_val, i64_t len)
{
    i64_t size = size_of_val * len + sizeof(header_t);
    header_t *adt = rf_malloc(size);

    if (adt == NULL)
        panic("OOM");

    adt->len = len;
    adt->rc = 1;

    rf_object_t v = {
        .type = type,
        .adt = adt,
    };

    return v;
}

i64_t vector_bool_push(rf_object_t *vector, bool_t value)
{
    push(vector, bool_t, value);
    return vector->adt->len;
}

i64_t vector_i64_push(rf_object_t *vector, i64_t value)
{
    push(vector, i64_t, value);
    return vector->adt->len;
}

i64_t vector_i64_pop(rf_object_t *vector)
{
    return pop(vector, i64_t);
}

i64_t vector_f64_push(rf_object_t *vector, f64_t value)
{
    push(vector, f64_t, value);
    return vector->adt->len;
}

f64_t vector_f64_pop(rf_object_t *vector)
{
    return pop(vector, f64_t);
}

i64_t list_push(rf_object_t *list, rf_object_t object)
{
    push(list, rf_object_t, object);
    return list->adt->len;
}

rf_object_t list_pop(rf_object_t *list)
{
    rf_object_t object = pop(list, rf_object_t);
    return rf_object_clone(&object);
}

i64_t vector_push(rf_object_t *vector, rf_object_t object)
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

    return vector->adt->len;
}

rf_object_t vector_pop(rf_object_t *vector)
{
    if (vector->adt->len == 0)
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
    case TYPE_CHAR:
        reserve(vector, i8_t, len);
        break;
    default:
        exit(1);
    }
}

i64_t vector_i64_find(rf_object_t *vector, i64_t key)
{
    i64_t *ptr = as_vector_i64(vector), l;
    i32_t i;

    l = vector->adt->len;
    for (i = 0; i < l; i++)
    {
        if (ptr[i] == key)
            return i;
    }

    return l;
}

i64_t vector_f64_find(rf_object_t *vector, f64_t key)
{
    f64_t *ptr = as_vector_f64(vector);
    i32_t i;
    i64_t l;

    l = vector->adt->len;
    for (i = 0; i < l; i++)
    {
        if (ptr[i] == key)
            return i;
    }

    return l;
}

i64_t list_find(rf_object_t *list, rf_object_t key)
{
    rf_object_t *ptr = as_list(list);
    i32_t i;
    i64_t l = list->adt->len;

    for (i = 0; i < l; i++)
    {
        if (rf_object_eq(&ptr[i], &key))
            return i;
    }

    return l;
}

i64_t vector_find(rf_object_t *vector, rf_object_t key)
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

rf_object_t vector_get(rf_object_t *vector, rf_object_t key)
{
    i64_t l = vector->adt->len, i = key.i64;
    i8_t type = vector->type - TYPE_BOOL;

    static null_t *types_table[] = {&&type_bool, &&type_i64, &&type_f64, &&type_symbol,
                                    &&type_char, &&type_list};

    if (type > TYPE_LIST)
        panic("vector_get: non-gettable type");

    goto *types_table[(i32_t)type];

type_bool:
    if (l < i)
        return bool(false);
    return bool(as_vector_bool(vector)[i]);
type_i64:
    if (l < i)
        return i64(NULL_I64);
    return i64(as_vector_i64(vector)[i]);
type_f64:
    if (l < i)
        return f64(NULL_F64);
    return f64(as_vector_f64(vector)[i]);
type_symbol:
    if (l < i)
        return i64(NULL_I64);
    return i64(as_vector_i64(vector)[i]);
type_char:
    if (l < i)
        return schar(0);
    return schar(as_string(vector)[i]);
type_list:
    if (l < i)
        return null();
    return rf_object_clone(&as_list(vector)[i]);
}

/*
 * Try to flatten list in a vector if all elements are of the same type
 */
rf_object_t list_flatten(rf_object_t list)
{
    if (list.type != TYPE_LIST)
        return list;

    i64_t len = list.adt->len;
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

    rf_object_free(&list);

    return vec;
}

null_t vector_free(rf_object_t *vector)
{
    // i64_t size_of_val;
    // i64_t size;

    // switch (vector->type)
    // {
    // case TYPE_BOOL:
    //     size_of_val = sizeof(i8_t);
    //     break;
    // case TYPE_I64:
    //     size_of_val = sizeof(i64_t);
    //     break;
    // case TYPE_F64:
    //     size_of_val = sizeof(f64_t);
    //     break;
    // case TYPE_SYMBOL:
    //     size_of_val = sizeof(i64_t);
    //     break;
    // case TYPE_LIST:
    //     size_of_val = sizeof(rf_object_t);
    //     break;
    // case TYPE_CHAR:
    //     size_of_val = sizeof(i8_t);
    //     break;
    // default:
    //     panic(str_fmt(0, "unknown type: %d", vector->type));
    // }

    rf_free(vector->adt);
}