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

#include <stdio.h>
#include <stdatomic.h>
#include "rayforce.h"
#include "format.h"
#include "alloc.h"
#include "string.h"
#include "vector.h"
#include "dict.h"
#include "util.h"
#include "string.h"

rf_object_t error(i8_t code, str_t message)
{
    rf_object_t err = string_from_str(message, strlen(message));
    err.type = TYPE_ERROR;
    err.adt->code = code;

    return err;
}

rf_object_t bool(bool_t val)
{
    rf_object_t scalar = {
        .type = -TYPE_BOOL,
        .i64 = val,
    };

    return scalar;
}

rf_object_t i64(i64_t val)
{
    rf_object_t scalar = {
        .type = -TYPE_I64,
        .i64 = val,
    };

    return scalar;
}

rf_object_t f64(f64_t val)
{
    rf_object_t scalar = {
        .type = -TYPE_F64,
        .f64 = val,
    };

    return scalar;
}

rf_object_t symbol(str_t s)
{
    i64_t id = symbols_intern(s, strlen(s));
    rf_object_t sym = {
        .type = -TYPE_SYMBOL,
        .i64 = id,
    };

    return sym;
}

rf_object_t null()
{
    rf_object_t list = {
        .type = TYPE_LIST,
        .adt = NULL,
    };

    return list;
}

rf_object_t table(rf_object_t keys, rf_object_t vals)
{
    if (keys.type != TYPE_SYMBOL || vals.type != 0)
        return error(ERR_TYPE, "Keys must be a symbol vector and rf_objects must be list");

    if (keys.adt->len != vals.adt->len)
        return error(ERR_LENGTH, "Keys and rf_objects must have the same length");

    // rf_object_t *v = as_list(&vals);
    // i64_t len = 0;

    // for (i64_t i = 0; i < v.adt.len; i++)
    // {
    //     if (v[i].type < 0)
    //         return error(ERR_TYPE, "Values must be scalars");
    // }

    rf_object_t table = list(2);

    as_list(&table)[0] = keys;
    as_list(&table)[1] = vals;

    table.type = TYPE_TABLE;

    return table;
}

i8_t rf_object_eq(rf_object_t *a, rf_object_t *b)
{
    i64_t i;

    if (a->type != b->type)
        return 0;

    if (a->type == -TYPE_I64 || a->type == -TYPE_SYMBOL)
        return a->i64 == b->i64;
    else if (a->type == -TYPE_F64)
        return a->f64 == b->f64;
    else if (a->type == TYPE_I64 || a->type == TYPE_SYMBOL)
    {
        if (as_vector_i64(a) == as_vector_i64(b))
            return 1;
        if (a->adt->len != b->adt->len)
            return 0;
        for (i = 0; i < a->adt->len; i++)
        {
            if (as_vector_i64(a)[i] != as_vector_i64(b)[i])
                return 0;
        }
        return 1;
    }
    else if (a->type == TYPE_F64)
    {
        if (as_vector_f64(a) == as_vector_f64(b))
            return 1;
        if (a->adt->len != b->adt->len)
            return 0;
        for (i = 0; i < a->adt->len; i++)
        {
            if (as_vector_f64(a)[i] != as_vector_f64(b)[i])
                return 0;
        }
        return 1;
    }

    // printf("** Eq: Invalid type\n");
    return 0;
}

/*
 * Increment the reference count of an rf_object_t
 */
rf_object_t rf_object_clone(rf_object_t *object)
{
    if (object->type < 0)
        return *object;

    if (object->adt == NULL)
        return *object;

    if (object->type == TYPE_ERROR)
    {
        return *object;
    }

    __atomic_fetch_add(&object->adt->rc, 1, __ATOMIC_RELAXED);

    return *object;
}

/*
 * Free an rf_object_t
 */
null_t rf_object_free(rf_object_t *object)
{
    if (object->type < 0)
        return;

    if (object->adt == NULL)
        return;

    if (object->type == TYPE_ERROR)
    {
        rf_free(object->adt);
        return;
    }

    i64_t rc = __atomic_sub_fetch(&object->adt->rc, 1, __ATOMIC_RELAXED);

    if (object->type == TYPE_LIST)
    {
        for (i64_t i = 0; i < object->adt->len; i++)
            rf_object_free(&as_list(object)[i]);
    }

    if (rc == 0)
        rf_free(object->adt);
}

/*
 * Copy on write rf_object_t
 */
rf_object_t rf_object_cow(rf_object_t *object)
{
    rf_object_t new;

    if (object->type < 0)
        return *object;

    if (object->adt == NULL)
        return *object;

    if (object->type == TYPE_ERROR)
    {
        return *object;
    }

    if (object->adt->rc == 1)
        return *object;

    switch (object->type)
    {
    case TYPE_LIST:
        new = list(object->adt->len);
        for (i64_t i = 0; i < object->adt->len; i++)
            as_list(&new)[i] = rf_object_cow(&as_list(object)[i]);
        break;
    case TYPE_I64:
        new = vector_i64(object->adt->len);
        memcpy(as_vector_i64(&new), as_vector_i64(object), object->adt->len * sizeof(i64_t));
        break;
    case TYPE_F64:
        new = vector_i64(object->adt->len);
        memcpy(as_vector_f64(&new), as_vector_f64(object), object->adt->len * sizeof(f64_t));
        break;
    case TYPE_SYMBOL:
        new = vector_symbol(object->adt->len);
        memcpy(as_vector_symbol(&new), as_vector_symbol(object), object->adt->len * sizeof(i64_t));
        break;
    default:
        panic("cow: invalid type");
    }

    return new;
}

/*
 * Get the reference count of an rf_object_t
 */
i64_t rf_object_rc(rf_object_t *object)
{
    if (object->type < 0)
        return 1;

    if (object->adt == NULL)
        return 1;

    if (object->type == TYPE_ERROR)
        return 1;

    return __atomic_load_n(&object->adt->rc, __ATOMIC_RELAXED);
}