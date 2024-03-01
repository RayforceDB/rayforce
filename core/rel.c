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

#include "rel.h"
#include "util.h"
#include "heap.h"
#include "ops.h"
#include "error.h"

obj_p ray_eq(obj_p x, obj_p y)
{
    i64_t i, l;
    obj_p vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_B8, TYPE_B8):
        return (b8(x->b8 == y->b8));

    case mtype2(-TYPE_I64, -TYPE_I64):
    case mtype2(-TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        return (b8(x->i64 == y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (b8(x->f64 == y->f64));

    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] == y->i64;

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] == y->f64;

        return vec;

    case mtype2(-TYPE_I64, TYPE_I64):
    case mtype2(-TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(-TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        l = y->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = x->i64 == as_i64(y)[i];

        return vec;

    case mtype2(-TYPE_F64, TYPE_F64):
        l = y->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = x->f64 == as_f64(y)[i];

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "eq: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] == as_i64(y)[i];

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "eq: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] == as_f64(y)[i];

        return vec;

    case mtype2(TYPE_LIST, TYPE_LIST):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "eq: lists of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = (cmp_obj(as_list(x)[i], as_list(y)[i]) == 0);

        return vec;

    default:
        throw(ERR_TYPE, "eq: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_ne(obj_p x, obj_p y)
{
    i64_t i, l;
    obj_p vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_B8, TYPE_B8):
        return (b8(x->b8 != y->b8));

    case mtype2(-TYPE_I64, -TYPE_I64):
        return (b8(x->i64 != y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (b8(x->f64 != y->f64));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] != y->i64;

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "ne: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] != as_i64(y)[i];

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] != y->f64;

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "ne: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] != as_f64(y)[i];

        return vec;

    case mtype2(TYPE_LIST, TYPE_LIST):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "ne: lists of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] =
                (cmp_obj(as_list(x)[i], as_list(y)[i]) != 0);

        return vec;

    default:
        throw(ERR_TYPE, "ne: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_lt(obj_p x, obj_p y)
{
    i64_t i, l;
    obj_p vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (b8(x->i64 < y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (b8(x->f64 < y->f64));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] < y->i64;

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "lt: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] < as_i64(y)[i];

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] < y->f64;

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "lt: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] < as_f64(y)[i];

        return vec;

    case mtype2(TYPE_LIST, TYPE_LIST):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "ne: lists of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] =
                (cmp_obj(as_list(x)[i], as_list(y)[i]) < 0);

        return vec;

    default:
        throw(ERR_TYPE, "lt: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_le(obj_p x, obj_p y)
{
    i64_t i, l;
    obj_p vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (b8(x->i64 <= y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (b8(x->f64 <= y->f64));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] <= y->i64;

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "le: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] <= as_i64(y)[i];

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] <= y->f64;

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "le: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] <= as_f64(y)[i];

        return vec;

    case mtype2(TYPE_LIST, TYPE_LIST):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "ne: lists of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] =
                (cmp_obj(as_list(x)[i], as_list(y)[i]) <= 0);

        return vec;

    default:
        throw(ERR_TYPE, "le: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_gt(obj_p x, obj_p y)
{
    i64_t i, l;
    obj_p vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (b8(x->i64 > y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (b8(x->f64 > y->f64));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] > y->i64;

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "gt: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] > as_i64(y)[i];

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] > y->f64;

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "gt: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] > as_f64(y)[i];

        return vec;

    case mtype2(TYPE_LIST, TYPE_LIST):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "ne: lists of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] =
                (cmp_obj(as_list(x)[i], as_list(y)[i]) > 0);

        return vec;

    default:
        throw(ERR_TYPE, "gt: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_ge(obj_p x, obj_p y)
{
    i64_t i, l;
    obj_p vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (b8(x->i64 >= y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (b8(x->f64 >= y->f64));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] >= y->i64;

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "ge: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] >= as_i64(y)[i];

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] >= y->f64;

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "ge: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] >= as_f64(y)[i];

        return vec;

    case mtype2(TYPE_LIST, TYPE_LIST):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "ne: lists of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] =
                (cmp_obj(as_list(x)[i], as_list(y)[i]) >= 0);

        return vec;

    default:
        throw(ERR_TYPE, "ge: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}
