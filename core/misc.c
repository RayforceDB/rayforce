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
#include "error.h"
#include "index.h"

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
        res = index_distinct_i64(as_i64(x), indices, l);
        res->type = x->type;
        return res;
    case TYPE_ENUM:
        l = indices == NULL ? ops_count(x) : l;
        res = index_distinct_i64(as_i64(enum_val(x)), indices, l);
        res = venum(ray_key(x), res);
        return res;
    case TYPE_LIST:
        l = indices == NULL ? ops_count(x) : l;
        res = index_distinct_obj(as_list(x), indices, l);
        return res;
    case TYPE_GUID:
        l = indices == NULL ? x->len : l;
        res = index_distinct_guid(as_guid(x), indices, l);
        return res;
    case TYPE_FILTERMAP:
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
    obj_t c, g, k, v;
    u64_t i, m, n, l;

    switch (x->type)
    {
    case TYPE_BYTE:
    case TYPE_BOOL:
    case TYPE_CHAR:
        g = index_group_i8((i8_t *)as_u8(x), NULL, ops_count(x));
        l = as_list(g)[0]->i64;
        k = vector_u8(l);
        v = list(l);

        c = index_group_cnts(g);

        // Allocate vectors for each group indices
        for (i = 0; i < l; i++)
        {
            as_list(v)[i] = vector_i64(as_i64(c)[i]);
            as_list(v)[i]->len = 0;
        }

        l = as_list(g)[1]->len;

        // Fill vectors with indices
        for (i = 0; i < l; i++)
        {
            n = as_i64(as_list(g)[1])[i];
            as_i64(as_list(v)[n])[as_list(v)[n]->len++] = i;
            as_u8(k)[n] = as_u8(x)[i];
        }

        drop(c);
        drop(g);

        return dict(k, v);
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        g = index_group_i64(as_i64(x), NULL, ops_count(x));
        l = as_list(g)[0]->i64;
        k = vector(x->type, l);
        v = list(l);

        c = index_group_cnts(g);

        // Allocate vectors for each group indices
        for (i = 0; i < l; i++)
        {
            as_list(v)[i] = vector_i64(as_i64(c)[i]);
            as_list(v)[i]->len = 0;
        }

        l = as_list(g)[1]->len;

        // Fill vectors with indices
        for (i = 0; i < l; i++)
        {
            n = as_i64(as_list(g)[1])[i];
            as_i64(as_list(v)[n])[as_list(v)[n]->len++] = i;
            as_i64(k)[n] = as_i64(x)[i];
        }

        drop(c);
        drop(g);

        return dict(k, v);

    case TYPE_F64:
        g = index_group_i64((i64_t *)as_f64(x), NULL, ops_count(x));
        l = as_list(g)[0]->i64;
        k = vector_f64(l);
        v = list(l);

        c = index_group_cnts(g);

        // Allocate vectors for each group indices
        for (i = 0; i < l; i++)
        {
            as_list(v)[i] = vector_i64(as_i64(c)[i]);
            as_list(v)[i]->len = 0;
        }

        l = as_list(g)[1]->len;

        // Fill vectors with indices
        for (i = 0; i < l; i++)
        {
            n = as_i64(as_list(g)[1])[i];
            as_i64(as_list(v)[n])[as_list(v)[n]->len++] = i;
            as_f64(k)[n] = as_f64(x)[i];
        }

        drop(g);

        return dict(k, v);
    case TYPE_ENUM:
        g = index_group_i64(as_i64(enum_val(x)), NULL, ops_count(x));
        l = as_list(g)[0]->i64;
        k = vector(x->type, l);
        v = list(l);

        c = index_group_cnts(g);

        // Allocate vectors for each group indices
        for (i = 0; i < l; i++)
        {
            as_list(v)[i] = vector_i64(as_i64(c)[i]);
            as_list(v)[i]->len = 0;
        }

        l = as_list(g)[1]->len;

        // Fill vectors with indices
        for (i = 0; i < l; i++)
        {
            n = as_i64(as_list(g)[1])[i];
            as_i64(as_list(v)[n])[as_list(v)[n]->len++] = i;
            as_i64(k)[n] = as_i64(enum_val(x))[i];
        }

        drop(c);
        drop(g);

        return dict(k, v);
    case TYPE_GUID:
        g = index_group_guid(as_guid(x), NULL, ops_count(x));
        l = as_list(g)[0]->i64;
        k = vector_guid(l);
        v = list(l);

        c = index_group_cnts(g);

        // Allocate vectors for each group indices
        for (i = 0; i < l; i++)
        {
            as_list(v)[i] = vector_i64(as_i64(c)[i]);
            as_list(v)[i]->len = 0;
        }

        l = as_list(g)[1]->len;

        // Fill vectors with indices
        for (i = 0; i < l; i++)
        {
            n = as_i64(as_list(g)[1])[i];
            as_i64(as_list(v)[n])[as_list(v)[n]->len++] = i;
            as_guid(k)[n] = as_guid(x)[i];
        }

        drop(c);
        drop(g);

        return dict(k, v);

    case TYPE_LIST:
        g = index_group_obj(as_list(x), NULL, ops_count(x));
        l = as_list(g)[0]->i64;
        k = vector(TYPE_LIST, l);
        v = list(l);

        c = index_group_cnts(g);

        // Allocate vectors for each group indices
        for (i = 0; i < l; i++)
        {
            as_list(v)[i] = vector_i64(as_i64(c)[i]);
            as_list(v)[i]->len = 0;
        }

        l = as_list(g)[1]->len;

        // Fill vectors with indices
        for (i = 0; i < l; i++)
        {
            n = as_i64(as_list(g)[1])[i];
            m = as_list(v)[n]->len;
            as_i64(as_list(v)[n])[m] = i;
            if (m == 0)
                as_list(k)[n] = clone(as_list(x)[i]);

            as_list(v)[n]->len++;
        }

        drop(c);
        drop(g);

        return dict(k, v);
    default:
        throw(ERR_TYPE, "group: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_rc(obj_t x)
{
    // substract 1 to skip the our reference
    return i64(rc(x) - 1);
}