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
#include "rayforce.h"
#include "format.h"
#include "alloc.h"
#include "string.h"
#include "vector.h"

extern value_t error(i8_t code, str_t message)
{
    value_t keys = vector_symbol(2), vals = list(2), c, error;
    c = string_from_const_str("code");
    as_vector_symbol(&keys)[0] = symbols_intern(&c);
    c = string_from_const_str("message");
    as_vector_symbol(&keys)[1] = symbols_intern(&c);
    as_list(&vals)[0] = i64(code);
    c = string_from_str(message);
    as_list(&vals)[1] = c;

    error = dict(keys, vals);
    error.type = TYPE_ERROR;

    return error;
}

extern value_t i64(i64_t value)
{
    value_t scalar = {
        .type = -TYPE_I64,
        .i64 = value,
    };

    return scalar;
}

extern value_t f64(f64_t value)
{
    value_t scalar = {
        .type = -TYPE_F64,
        .f64 = value,
    };

    return scalar;
}

extern value_t symbol(str_t ptr)
{
    // Do not allocate new string - it would be done by symbols_intern (if needed)
    value_t string = str(ptr, strlen(ptr));
    string.list.ptr = ptr;
    i64_t id = symbols_intern(&string);
    value_t list = {
        .type = -TYPE_SYMBOL,
        .i64 = id,
    };

    return list;
}

extern value_t null()
{
    value_t list = {
        .type = TYPE_LIST,
        .list = {
            .ptr = NULL,
            .len = 0,
        },
    };

    return list;
}

extern value_t table(value_t keys, value_t vals)
{
    if (keys.type != TYPE_SYMBOL || vals.type != 0)
        return error(ERR_TYPE, "Keys must be a symbol vector and values must be list");

    if (keys.list.len != vals.list.len)
        return error(ERR_LENGTH, "Keys and values must have the same length");

    // value_t *v = as_list(&vals);
    // i64_t len = 0;

    // for (i64_t i = 0; i < v.list.len; i++)
    // {
    //     if (v[i].type < 0)
    //         return error(ERR_TYPE, "Values must be scalars");
    // }

    value_t table = list(2);

    as_list(&table)[0] = keys;
    as_list(&table)[1] = vals;

    table.type = TYPE_TABLE;

    return table;
}

extern value_t value_clone(value_t *value)
{
    switch (value->type)
    {
    case TYPE_I64:
        return *value;
    case TYPE_F64:
        return *value;
    default:
    {
        // printf("** Clone: Invalid type\n");
        return *value;
    }
    }
}

extern i8_t value_eq(value_t *a, value_t *b)
{
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
        if (a->list.len != b->list.len)
            return 0;
        for (i64_t i = 0; i < a->list.len; i++)
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
        if (a->list.len != b->list.len)
            return 0;
        for (i64_t i = 0; i < a->list.len; i++)
        {
            if (as_vector_f64(a)[i] != as_vector_f64(b)[i])
                return 0;
        }
        return 1;
    }

    // printf("** Eq: Invalid type\n");
    return 0;
}

extern null_t value_free(value_t *value)
{
    switch (value->type)
    {
    case TYPE_I64:
    {
        rayforce_free(value->list.ptr);
        break;
    }
    case TYPE_F64:
    {
        rayforce_free(value->list.ptr);
        break;
    }
    default:
    {
        // printf("** Free: Invalid type\n");
        break;
    }
    }
}
