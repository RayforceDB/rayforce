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

#include "order.h"
#include "util.h"
#include "ops.h"
#include "items.h"
#include "heap.h"
#include "sort.h"
#include "error.h"

obj_p ray_iasc(obj_p x)
{
    switch (x->type)
    {
    case TYPE_I64:
        return ray_sort_asc(x);

    default:
        throw(ERR_TYPE, "iasc: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_idesc(obj_p x)
{
    switch (x->type)
    {
    case TYPE_I64:
        return ray_sort_desc(x);

    default:
        throw(ERR_TYPE, "idesc: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_asc(obj_p x)
{
    obj_p idx;
    i64_t l, i;

    switch (x->type)
    {
    case TYPE_I64:
        idx = ray_sort_asc(x);
        l = x->len;
        for (i = 0; i < l; i++)
            as_i64(idx)[i] = as_i64(x)[as_i64(idx)[i]];

        idx->attrs |= ATTR_ASC;

        return idx;

    default:
        throw(ERR_TYPE, "asc: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_desc(obj_p x)
{
    obj_p idx;
    i64_t l, i;

    switch (x->type)
    {
    case TYPE_I64:
        idx = ray_sort_desc(x);
        l = x->len;
        for (i = 0; i < l; i++)
            as_i64(idx)[i] = as_i64(x)[as_i64(idx)[i]];

        idx->attrs |= ATTR_DESC;

        return idx;

    default:
        throw(ERR_TYPE, "desc: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_xasc(obj_p x, obj_p y)
{
    obj_p idx, col, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        col = at_obj(x, y);

        if (is_error(col))
            return col;

        idx = ray_iasc(col);
        drop_obj(col);

        if (is_error(idx))
            return idx;

        res = ray_take(x, idx);

        drop_obj(idx);

        return res;
    default:
        throw(ERR_TYPE, "xasc: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_xdesc(obj_p x, obj_p y)
{
    obj_p idx, col, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        col = at_obj(x, y);

        if (is_error(col))
            return col;

        idx = ray_idesc(col);
        drop_obj(col);

        if (is_error(idx))
            return idx;

        res = ray_take(x, idx);

        drop_obj(idx);

        return res;
    default:
        throw(ERR_TYPE, "xdesc: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_not(obj_p x)
{
    i32_t i;
    i64_t l;
    obj_p res;

    switch (x->type)
    {
    case -TYPE_B8:
        return b8(!x->b8);

    case TYPE_B8:
        l = x->len;
        res = vector_b8(l);
        for (i = 0; i < l; i++)
            as_b8(res)[i] = !as_b8(x)[i];

        return res;

    default:
        throw(ERR_TYPE, "not: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_neg(obj_p x)
{
    obj_p res;
    i64_t i, l;

    switch (x->type)
    {
    case -TYPE_B8:
        return i64(-x->b8);
    case -TYPE_I64:
        return i64(-x->i64);
    case -TYPE_F64:
        return f64(-x->f64);
    case TYPE_I64:
        l = x->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(res)[i] = -as_i64(x)[i];
        return res;
    case TYPE_F64:
        l = x->len;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(res)[i] = -as_f64(x)[i];
        return res;

    default:
        throw(ERR_TYPE, "neg: unsupported type: '%s", type_name(x->type));
    }
}
