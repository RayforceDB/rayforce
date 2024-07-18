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

#include "cmp.h"
#include "util.h"
#include "heap.h"
#include "ops.h"
#include "error.h"
#include "runtime.h"
#include "pool.h"

obj_p cmp_map(raw_p cmp, obj_p lhs, obj_p rhs)
{
    pool_p pool = runtime_get()->pool;
    u64_t i, l, n, chunk;
    obj_p res, parts;
    raw_p argv[6];

    n = pool_executors_count(pool);
    res = vector_b8(lhs->len);
    l = lhs->len;

    if (n == 1)
    {
        argv[0] = (raw_p)l;
        argv[1] = (raw_p)0;
        argv[2] = lhs;
        argv[3] = rhs;
        argv[4] = res;
        pool_call_task_fn(cmp, 5, argv);

        return res;
    }

    pool_prepare(pool);

    chunk = l / n;

    for (i = 0; i < n - 1; i++)
        pool_add_task(pool, cmp, 5, chunk, i * chunk, lhs, rhs, res);

    pool_add_task(pool, cmp, 5, l - i * chunk, i * chunk, lhs, rhs, res);

    parts = pool_run(pool);
    unwrap_list(parts);
    drop_obj(parts);

    return res;
}

obj_p ray_eq_partial(u64_t len, u64_t offset, obj_p lhs, obj_p rhs, obj_p res)
{
    u64_t i;
    i64_t *xi, *yi, si;
    b8_t *out;

    out = as_b8(res) + offset;

    switch (mtype2(lhs->type, rhs->type))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        xi = as_i64(lhs) + offset;
        si = rhs->i64;
        for (i = 0; i < len; i++)
            out[i] = xi[i] == si;
        break;
    default:
        break;
    }

    return NULL_OBJ;
}

obj_p ray_eq(obj_p x, obj_p y)
{
    i64_t i, l;
    obj_p vec, parts;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_B8, -TYPE_B8):
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
        return cmp_map(ray_eq_partial, x, y);
    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] == y->f64;

        return vec;

    case mtype2(TYPE_F64, -TYPE_I64):
        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] == (f64_t)y->i64;

        return vec;

    case mtype2(-TYPE_I64, TYPE_I64):
    case mtype2(-TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(-TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        return cmp_map(ray_eq_partial, y, x);
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
        // case mtype2(TYPE_SYMBOL, TYPE_ENUM):
        //     if (x->len != y->len)
        //         return error_str(ERR_LENGTH, "eq: vectors of different length");

        //     l = x->len;
        //     vec = vector_b8(l);

        //     for (i = 0; i < l; i++)
        //         as_b8(vec)[i] = as_i64(x)[i] == as_i64(y)[i];

        //     return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "eq: vectors of different length");

        l = x->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] == as_f64(y)[i];

        return vec;

    case mtype2(TYPE_F64, TYPE_I64):
        l = y->len;
        vec = vector_b8(l);

        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_f64(x)[i] == (f64_t)as_i64(y)[i];

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
    case mtype2(-TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        return (b8(x->i64 != y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (b8(x->f64 != y->f64));

    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] != y->i64;

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
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
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] < y->i64;

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
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
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] <= y->i64;

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
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
    case mtype2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        return (b8(x->i64 > y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (b8(x->f64 > y->f64));

    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] > y->i64;

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
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
    case mtype2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        return (b8(x->i64 >= y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (b8(x->f64 >= y->f64));

    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        l = x->len;
        vec = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(vec)[i] = as_i64(x)[i] >= y->i64;

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
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
