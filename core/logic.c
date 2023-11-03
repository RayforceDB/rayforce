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

#include "logic.h"
#include "util.h"
#include "heap.h"
#include "ops.h"

obj_t ray_and(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_BOOL, -TYPE_BOOL):
        return (bool(x->bool && y->bool));

    case mtype2(TYPE_BOOL, TYPE_BOOL):
        l = x->len;
        res = vector_bool(x->len);
        for (i = 0; i < l; i++)
            as_bool(res)[i] = as_bool(x)[i] & as_bool(y)[i];

        return res;

    default:
        throw(ERR_TYPE, "and: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_or(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_BOOL, -TYPE_BOOL):
        return (bool(x->bool || y->bool));

    case mtype2(TYPE_BOOL, TYPE_BOOL):
        l = x->len;
        res = vector_bool(x->len);
        for (i = 0; i < l; i++)
            as_bool(res)[i] = as_bool(x)[i] | as_bool(y)[i];

        return res;

    default:
        throw(ERR_TYPE, "or: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_like(obj_t x, obj_t y)
{
    i64_t i, l;
    obj_t res, e;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_CHAR, TYPE_CHAR):
        return (bool(str_match(as_string(x), as_string(y))));
    case mtype2(TYPE_LIST, TYPE_CHAR):
        l = x->len;
        res = vector_bool(l);
        for (i = 0; i < l; i++)
        {
            e = as_list(x)[i];
            if (!e || e->type != TYPE_CHAR)
            {
                res->len = i;
                drop(res);
                throw(ERR_TYPE, "like: unsupported types: '%s, %s", typename(e->type), typename(y->type));
            }

            as_bool(res)[i] = str_match(as_string(e), as_string(y));
        }

        return res;

    case mtype2(TYPE_ANYMAP, TYPE_CHAR):
        l = x->len;
        res = vector_bool(l);
        for (i = 0; i < l; i++)
        {
            e = at_idx(x, i);
            if (!e || e->type != TYPE_CHAR)
            {
                res->len = i;
                drop(res);
                drop(e);
                throw(ERR_TYPE, "like: unsupported types: '%s, '%s", typename(e->type), typename(y->type));
            }

            as_bool(res)[i] = str_match(as_string(e), as_string(y));
            drop(e);
        }

        return res;

    default:
        throw(ERR_TYPE, "like: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}
