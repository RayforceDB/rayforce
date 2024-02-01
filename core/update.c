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

#include "update.h"
#include "heap.h"
#include "util.h"
#include "runtime.h"
#include "error.h"
#include "eval.h"

obj_t __fetch(obj_t x, obj_t **out)
{
    u64_t i;
    obj_t *val, *env;

    switch (x->type)
    {
    case -TYPE_SYMBOL:
        val = deref(x);
        if (val == NULL)
            throw(ERR_NOT_FOUND, "fetch: symbol not found");
        *out = val;
        return cow(*val);
    default:
        return cow(x);
    }
}

obj_t __update(obj_t *obj, obj_t idx, obj_t fun, obj_t val)
{
    // switch (x[2]->type)
    // {
    //     case TYPE_DYAD:
    //         obj = call(x[2], obj, x[3]);
    //         break;
    // }

    return set_obj(obj, idx, val);
}

obj_t __commit(obj_t src, obj_t *out, obj_t obj)
{
    if (src->type == -TYPE_SYMBOL)
    {
        if (out && (*out != obj))
        {
            drop(*out);
            *out = obj;
        }

        return clone(src);
    }

    return obj;
}

/*
 * modifies an object in place (in a row).
 * It is a dyad that takes 4 arguments:
 * [0] - object to modify
 * [1] - indexes
 * [2] - function to apply
 * [3] - arguments to function
 */
obj_t __upwidth(obj_t src, obj_t idx, obj_t fun, obj_t val)
{
    obj_t *out = NULL, obj, res;

    obj = __fetch(src, &out);
    if (is_error(obj))
        return obj;

    res = __update(&obj, idx, fun, clone(val));
    if (is_error(res))
    {
        if (!out || (*out != obj))
            drop(obj);

        return res;
    }

    return __commit(src, out, res);
}

obj_t ray_upwidth(obj_t *x, u64_t n)
{
    if (n != 4)
        throw(ERR_LENGTH, "upwidth: expected 4 arguments");

    return __upwidth(x[0], x[1], x[2], x[3]);
}

/*
 * recursive for upwidth
 */
obj_t __updepth(obj_t src, obj_t idx, obj_t fun, obj_t val)
{
    u64_t i, l;
    obj_t *out = NULL, obj, res;

    switch (idx->type)
    {
    case -TYPE_I64:
        return __upwidth(src, idx, fun, val);
        // case TYPE_I64:
        // if ()
        // l = idx->len;
        // for (i = 0; i < l; i++)
        // {
        //     obj =
        //     if ()
        // }
    default:
        throw(ERR_NOT_IMPLEMENTED, "updepth");
    }
}

obj_t ray_updepth(obj_t *x, u64_t n)
{
    if (n != 4)
        throw(ERR_LENGTH, "updepth: expected 4 arguments");

    return __updepth(x[0], x[1], x[2], x[3]);
}

/*
 * updates for tables
 */
obj_t ray_update(obj_t *x, u64_t n)
{
    throw(ERR_NOT_IMPLEMENTED, "update");
}

/*
 * update/inserts for tables
 */
obj_t ray_upsert(obj_t *x, u64_t n)
{
    throw(ERR_NOT_IMPLEMENTED, "upsert");
}