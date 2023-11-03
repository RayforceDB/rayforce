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

obj_t ray_iasc(obj_t x)
{
    obj_t v, res;

    switch (x->type)
    {
    case TYPE_I64:
        return ray_sort_asc(x);

    case TYPE_VECMAP:
        v = ray_value(x);
        res = ray_iasc(v);
        drop(v);
        return res;

    default:
        throw(ERR_TYPE, "iasc: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_idesc(obj_t x)
{
    obj_t v, res;

    switch (x->type)
    {
    case TYPE_I64:
        return ray_sort_desc(x);

    case TYPE_VECMAP:
        v = ray_value(x);
        res = ray_idesc(v);
        drop(v);
        return res;

    default:
        throw(ERR_TYPE, "idesc: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_asc(obj_t x)
{
    obj_t idx, v, res;
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

    case TYPE_VECMAP:
        v = ray_value(x);
        res = ray_asc(v);
        drop(v);

        res->attrs |= ATTR_ASC;

        return res;

    default:
        throw(ERR_TYPE, "asc: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_desc(obj_t x)
{
    obj_t idx, v, res;
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

    case TYPE_VECMAP:
        v = ray_value(x);
        res = ray_desc(v);
        drop(v);

        res->attrs |= ATTR_DESC;

        return res;

    default:
        throw(ERR_TYPE, "desc: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_xasc(obj_t x, obj_t y)
{
    obj_t idx, col, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        col = at_obj(x, y);

        if (is_error(col))
            return col;

        idx = ray_iasc(col);
        drop(col);

        if (is_error(idx))
            return idx;

        res = ray_take(x, idx);

        drop(idx);

        return res;
    default:
        throw(ERR_TYPE, "xasc: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_xdesc(obj_t x, obj_t y)
{
    obj_t idx, col, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        col = at_obj(x, y);

        if (is_error(col))
            return col;

        idx = ray_idesc(col);
        drop(col);

        if (is_error(idx))
            return idx;

        res = ray_take(x, idx);

        drop(idx);

        return res;
    default:
        throw(ERR_TYPE, "xdesc: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_not(obj_t x)
{
    i32_t i;
    i64_t l;
    obj_t res;

    switch (x->type)
    {
    case -TYPE_BOOL:
        return bool(!x->bool);

    case TYPE_BOOL:
        l = x->len;
        res = vector_bool(l);
        for (i = 0; i < l; i++)
            as_bool(res)[i] = !as_bool(x)[i];

        return res;

    default:
        throw(ERR_TYPE, "not: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_neg(obj_t x)
{
    obj_t res;
    i64_t i, l;

    switch (x->type)
    {
    case -TYPE_BOOL:
        return i64(-x->bool);
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
        throw(ERR_TYPE, "neg: unsupported type: '%s", typename(x->type));
    }
}
