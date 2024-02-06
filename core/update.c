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
#include "filter.h"
#include "binary.h"
#include "unary.h"
#include "vary.h"
#include "iter.h"

obj_t __fetch(obj_t obj, obj_t **val)
{
    if (obj->type == -TYPE_SYMBOL)
    {
        *val = deref(obj);
        if (*val == NULL)
            throw(ERR_NOT_FOUND, "fetch: symbol not found");

        obj = cow(**val);
    }
    else
        obj = cow(obj);

    return obj;
}

obj_t __update(obj_t *obj, obj_t *x, u64_t n)
{
    obj_t v, idx, res;

    // special case for set
    if (x[1]->i64 == (i64_t)ray_set)
    {
        if (n != 4)
            throw(ERR_LENGTH, "alter: set expected a value");

        return set_obj(obj, x[2], clone(x[3]));
    }

    // retrieve the object via indices
    v = at_obj(*obj, x[2]);

    if (is_error(v))
        return v;

    idx = x[2];
    x[2] = v;
    res = ray_map(x + 1, n - 1);
    x[2] = idx;
    drop(v);

    if (is_error(res))
        return res;

    return set_obj(obj, idx, res);
}

obj_t __commit(obj_t src, obj_t obj, obj_t *val)
{
    if (src->type == -TYPE_SYMBOL)
    {
        if ((val != NULL) && (*val != obj))
        {
            drop(*val);
            *val = obj;
        }

        return clone(src);
    }

    return obj;
}

obj_t ray_alter(obj_t *x, u64_t n)
{
    obj_t *val = NULL, obj, res;

    if (n < 3)
        throw(ERR_LENGTH, "alter: expected at least 3 arguments");

    if (x[1]->type < TYPE_LAMBDA || x[1]->type > TYPE_VARY)
        throw(ERR_TYPE, "alter: expected function as 3rd argument");

    obj = __fetch(x[0], &val);

    if (is_error(obj))
        return obj;

    res = __update(&obj, x, n);
    if (is_error(res))
    {
        if ((val != NULL) || (*val != obj))
            drop(obj);

        return res;
    }

    return __commit(x[0], obj, val);
}

obj_t ray_modify(obj_t *x, u64_t n)
{
    unused(x);
    switch (n)
    {
    default:
        throw(ERR_LENGTH, "modify: expected 3 or 4 arguments");
    }
}

/*
 * updates for tables
 */
obj_t ray_update(obj_t *x, u64_t n)
{
    unused(x);
    unused(n);
    throw(ERR_NOT_IMPLEMENTED, "update");
}

/*
 * update/inserts for tables
 */
obj_t ray_upsert(obj_t *x, u64_t n)
{
    unused(x);
    unused(n);
    throw(ERR_NOT_IMPLEMENTED, "upsert");
}
