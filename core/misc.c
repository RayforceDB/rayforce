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

#include "misc.h"
#include "ops.h"
#include "util.h"
#include "runtime.h"
#include "sock.h"
#include "fs.h"
#include "items.h"
#include "unary.h"

obj_t ray_type(obj_t x)
{
    type_t type;
    if (!x)
        type = -TYPE_ERROR;
    else
        type = x->type;

    i64_t t = env_get_typename_by_type(&runtime_get()->env, type);
    return symboli64(t);
}

obj_t ray_count(obj_t x)
{
    return i64(ops_count(x));
}

obj_t ray_distinct(obj_t x)
{
    obj_t res = NULL;
    u64_t l;
    i64_t *indices = NULL;

dispatch:
    switch (x->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        l = indices == NULL ? x->len : l;
        res = ops_distinct_raw(as_i64(x), indices, l);
        res->type = x->type;
        return res;
    case TYPE_ENUM:
        l = indices == NULL ? ops_count(x) : l;
        res = ops_distinct_raw(as_i64(enum_val(x)), indices, l);
        res = venum(ray_key(x), res);
        return res;
    case TYPE_LIST:
        l = indices == NULL ? ops_count(x) : l;
        res = ops_distinct_obj(as_list(x), indices, l);
        return res;
    case TYPE_VECMAP:
        l = as_list(x)[1]->len;
        indices = as_i64(as_list(x)[1]);
        x = as_list(x)[0];
        goto dispatch;
    default:
        throw(ERR_TYPE, "distinct: invalid type: '%s", typename(x->type));
    }
}

obj_t ray_group(obj_t x)
{
    obj_t res;
    u64_t l;
    i64_t *indices = NULL;

dispatch:
    switch (x->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        l = indices == NULL ? x->len : l;
        res = ops_group(as_i64(x), indices, l);
        as_list(res)[0]->type = x->type;
        return res;
    case TYPE_ENUM:
        l = indices == NULL ? ops_count(x) : l;
        res = ops_group(as_i64(enum_val(x)), indices, l);
        as_list(res)[0] = venum(ray_key(x), as_list(res)[0]);
        return res;
    case TYPE_VECMAP:
        l = as_list(x)[1]->len;
        indices = as_i64(as_list(x)[1]);
        x = as_list(x)[0];
        goto dispatch;
    default:
        throw(ERR_TYPE, "group: invalid type: '%s", typename(x->type));
    }
}

obj_t ray_rc(obj_t x)
{
    // substract 1 to skip the our reference
    return i64(rc(x) - 1);
}