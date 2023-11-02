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

    switch (x->type)
    {
    case TYPE_I64:
        return ops_distinct(x);
    case TYPE_SYMBOL:
        res = ops_distinct(x);
        res->type = TYPE_SYMBOL;
        return res;
    default:
        emit(ERR_TYPE, "distinct: invalid type: %d", x->type);
    }
}

obj_t ray_group(obj_t x)
{
    obj_t g, k, v, vals, *vi, res;
    u64_t i, l;
    i64_t j, *offsets, *indices = NULL;

dispatch:
    switch (x->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        l = indices == NULL ? x->len : l;
        g = ops_group(as_i64(x), indices, l);
        as_list(g)[0]->type = x->type;
        break;
    case TYPE_ENUM:
        l = indices == NULL ? ops_count(x) : l;
        k = ray_key(x);
        v = ray_get(k);
        drop(k);
        if (is_error(v))
            return v;

        g = ops_group(as_i64(enum_val(x)), indices, l);
        drop(v);
        l = as_list(g)[0]->len;
        res = vector(TYPE_SYMBOL, l);
        for (i = 0; i < l; i++)
            as_symbol(res)[i] = as_symbol(v)[as_i64(as_list(g)[0])[i]];
        drop(as_list(g)[0]);
        as_list(g)[0] = res;
        break;
    case TYPE_VECMAP:
        l = as_list(x)[1]->len;
        indices = as_i64(as_list(x)[1]);
        x = as_list(x)[0];
        goto dispatch;
    default:
        emit(ERR_TYPE, "group: invalid type: %d", x->type);
    }

    l = as_list(g)[0]->len;
    offsets = as_i64(as_list(g)[1]);
    indices = as_i64(as_list(g)[2]);

    vals = vector(TYPE_LIST, l);
    vi = as_list(vals);

    for (i = 0, j = 0; i < l; i++)
    {
        vi[i] = vector_i64(offsets[i] - j);
        memcpy(as_u8(vi[i]), indices + j, (offsets[i] - j) * sizeof(i64_t));
        j = offsets[i];
    }

    res = dict(clone(as_list(g)[0]), vals);
    drop(g);

    return res;
}

obj_t ray_parse(obj_t x)
{
    parser_t parser;
    obj_t res;

    switch (x->type)
    {
    case TYPE_CHAR:
        parser = parser_new();
        res = parse(&parser, "top-level", as_string(x));
        parser_free(&parser);
        return res;
    default:
        emit(ERR_TYPE, "parse: unsupported type: %d", x->type);
    }
}

obj_t ray_read_parse_compile(obj_t x)
{
    // parser_t parser;
    // obj_t red, par, com;

    switch (x->type)
    {
    // case TYPE_CHAR:
    //     red = ray_read(x);
    //     if (red->type == TYPE_ERROR)
    //         return red;

    //     parser = parser_new();
    //     par = parse(&parser, as_string(x), as_string(red));
    //     drop(red);

    //     if (par->type == TYPE_ERROR)
    //     {
    //         print_error(par, as_string(x), as_string(red), red->len);
    //         parser_free(&parser);
    //         return par;
    //     }

    //     com = cc_compile_lambda(as_string(x), vector_symbol(0), &par, 1, &parser.nfo);
    //     drop(par);
    //     parser_free(&parser);

    //     return com;
    default:
        emit(ERR_TYPE, "read_parse_compile: unsupported type: %d", x->type);
    }
}

obj_t ray_rc(obj_t x)
{
    // substract 1 to skip the our reference
    return i64(rc(x) - 1);
}