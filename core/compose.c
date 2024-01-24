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

#include "compose.h"
#include "util.h"
#include "heap.h"
#include "ops.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "runtime.h"
#include "items.h"
#include "guid.h"
#include "error.h"

obj_t ray_as(obj_t x, obj_t y)
{
    type_t type;
    obj_t err;
    str_t fmt, msg;

    if (x->type != -TYPE_SYMBOL)
        throw(ERR_TYPE, "as: first argument must be a symbol");

    type = env_get_type_by_typename(&runtime_get()->env, x->i64);

    if (type == TYPE_ERROR)
    {
        fmt = obj_fmt(x);
        msg = str_fmt(0, "as: not a type: '%s", fmt);
        err = error_str(ERR_TYPE, msg);
        heap_free(fmt);
        heap_free(msg);
        return err;
    }

    return as(type, y);
}

obj_t ray_til(obj_t x)
{
    if (x->type != -TYPE_I64)
        return error_str(ERR_TYPE, "til: expected i64");

    i32_t i, l = (i32_t)x->i64;
    obj_t vec = NULL;

    vec = vector_i64(l);

    if (is_error(vec))
        return vec;

    for (i = 0; i < l; i++)
        as_i64(vec)[i] = i;

    vec->attrs = ATTR_ASC | ATTR_DISTINCT;

    return vec;
}

obj_t ray_reverse(obj_t x)
{
    i64_t i, l;
    obj_t res;

    switch (x->type)
    {
    case TYPE_CHAR:
    case TYPE_BYTE:
    case TYPE_BOOL:
        l = x->len;
        res = string(l);

        for (i = l - 1; i >= 0; i--)
            as_string(res)[l - i] = as_string(x)[i];

        return res;

    case TYPE_I64:
    case TYPE_TIMESTAMP:
    case TYPE_SYMBOL:
        l = x->len;
        res = vector(x->type, l);

        for (i = 0; i < l; i++)
            as_i64(res)[i] = as_i64(x)[l - i - 1];

        return res;

    case TYPE_F64:
        l = x->len;
        res = vector_f64(l);

        for (i = 0; i < l; i++)
            as_f64(res)[i] = as_f64(x)[l - i - 1];

        return res;

    case TYPE_LIST:
        l = x->len;
        res = vector(TYPE_LIST, l);

        for (i = 0; i < l; i++)
            as_list(res)[i] = clone(as_list(x)[l - i - 1]);

        return res;

    default:
        throw(ERR_TYPE, "reverse: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_dict(obj_t x, obj_t y)
{
    if (!is_vector(x) || !is_vector(y))
        return error_str(ERR_TYPE, "Keys and Values must be lists");

    if (ops_count(x) != ops_count(y))
        return error_str(ERR_LENGTH, "Keys and Values must have the same length");

    return dict(clone(x), clone(y));
}

obj_t ray_table(obj_t x, obj_t y)
{
    bool_t synergy = true;
    u64_t i, j, len, cl = 0;
    obj_t lst, c, l = null(0);

    if (x->type != TYPE_SYMBOL)
        return error_str(ERR_TYPE, "table: keys must be a symbol vector");

    if (y->type != TYPE_LIST)
    {
        if (x->len != 1)
            return error_str(ERR_LENGTH, "table: keys and values must have the same length");

        l = vn_list(1);
        as_list(l)[0] = clone(y);
        y = l;
    }

    if (x->len != y->len && y->len > 0)
    {
        drop(l);
        return error_str(ERR_LENGTH, "table: keys and values must have the same length");
    }

    len = y->len;

    for (i = 0; i < len; i++)
    {
        switch (as_list(y)[i]->type)
        {
        case -TYPE_BOOL:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_CHAR:
        case -TYPE_SYMBOL:
        case -TYPE_TIMESTAMP:
        case TYPE_LAMBDA:
        case TYPE_DICT:
        case TYPE_TABLE:
            synergy = false;
            break;
        case TYPE_BOOL:
        case TYPE_I64:
        case TYPE_F64:
        case TYPE_TIMESTAMP:
        case TYPE_CHAR:
        case TYPE_SYMBOL:
        case TYPE_LIST:
        case TYPE_GUID:
            j = as_list(y)[i]->len;
            if (cl != 0 && j != cl)
                return error_str(ERR_LENGTH, "table: values must be of the same length");

            cl = j;
            break;
        case TYPE_ENUM:
            synergy = false;
            j = as_list(as_list(y)[i])[1]->len;
            if (cl != 0 && j != cl)
                return error_str(ERR_LENGTH, "table: values must be of the same length");

            cl = j;
            break;
        default:
            return error_str(ERR_TYPE, "table: unsupported type in a values list");
        }
    }

    // there are no atoms, no lazytypes and all columns are of the same length
    if (synergy)
    {
        drop(l);
        return table(clone(x), clone(y));
    }

    // otherwise we need to expand atoms to vectors
    lst = vector(TYPE_LIST, len);

    if (cl == 0)
        cl = 1;

    for (i = 0; i < len; i++)
    {
        switch (as_list(y)[i]->type)
        {
        case -TYPE_BOOL:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_CHAR:
        case -TYPE_SYMBOL:
            c = i64(cl);
            as_list(lst)[i] = ray_take(c, as_list(y)[i]);
            drop(c);
            break;
        case TYPE_FILTERMAP:
        case TYPE_ENUM:
        case TYPE_GROUPMAP:
            as_list(lst)[i] = ray_value(as_list(y)[i]);
            break;
        default:
            as_list(lst)[i] = clone(as_list(y)[i]);
        }
    }

    drop(l);

    return table(clone(x), lst);
}

obj_t ray_guid(obj_t x)
{
    i64_t i, count;
    obj_t vec;
    guid_t *g;

    switch (x->type)
    {
    case -TYPE_I64:
        count = x->i64;
        vec = vector_guid(count);
        g = as_guid(vec);

        for (i = 0; i < count; i++)
            guid_generate(g + i);

        return vec;

    default:
        throw(ERR_TYPE, "guid: unsupported type: '%s", typename(x->type));
    }
}

obj_t ray_list(obj_t *x, u64_t n)
{
    u64_t i;
    obj_t lst = vector(TYPE_LIST, n);

    for (i = 0; i < n; i++)
        as_list(lst)[i] = clone(x[i]);

    return lst;
}

obj_t ray_enlist(obj_t *x, u64_t n)
{
    u64_t i;
    obj_t lst;

    if (n == 0)
        return list(0);

    lst = vector(x[0]->type, n);

    for (i = 0; i < n; i++)
        ins_obj(&lst, i, clone(x[i]));

    return lst;
}

obj_t ray_enum(obj_t x, obj_t y)
{
    obj_t s, v;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_SYMBOL, TYPE_SYMBOL):
        s = ray_get(x);

        if (is_error(s))
            return s;

        if (!s || s->type != TYPE_SYMBOL)
        {
            drop(s);
            throw(ERR_TYPE, "enum: expected vector symbol");
        }

        v = ops_find(as_i64(s), s->len, as_i64(y), y->len);
        drop(s);

        if (is_error(v))
        {
            drop(v);
            throw(ERR_TYPE, "enum: can not be fully indexed");
        }

        return venum(clone(x), v);
    default:
        throw(ERR_TYPE, "enum: unsupported types: '%s '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_filtermap(obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res;

    switch (x->type)
    {
    case TYPE_TABLE:
        l = as_list(x)[1]->len;
        res = list(l);
        for (i = 0; i < l; i++)
            as_list(res)[i] = ray_filtermap(as_list(as_list(x)[1])[i], y);

        return table(clone(as_list(x)[0]), res);

    default:
        res = vn_list(3, clone(x), clone(y), NULL);
        res->type = TYPE_FILTERMAP;
        return res;
    }
}

obj_t ray_rand(obj_t x, obj_t y)
{
    i64_t i, count;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        count = x->i64;
        vec = vector_i64(count);

        for (i = 0; i < count; i++)
            as_i64(vec)[i] = ops_rand_u64() % y->i64;

        return vec;

    default:
        throw(ERR_TYPE, "rand: unsupported types: '%s '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_concat(obj_t x, obj_t y)
{
    i64_t i, xl, yl;
    obj_t vec;

    if (!x || !y)
        return vn_list(2, clone(x), clone(y));

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_BOOL, -TYPE_BOOL):
        vec = vector_bool(2);
        as_bool(vec)[0] = x->bool;
        as_bool(vec)[1] = y->bool;
        return vec;

    case mtype2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
    case mtype2(-TYPE_I64, -TYPE_I64):
    case mtype2(-TYPE_SYMBOL, -TYPE_SYMBOL):
        vec = vector(-x->type, 2);
        as_i64(vec)[0] = x->i64;
        as_i64(vec)[1] = y->i64;
        return vec;

    case mtype2(-TYPE_F64, -TYPE_F64):
        vec = vector_f64(2);
        as_f64(vec)[0] = x->f64;
        as_f64(vec)[1] = y->f64;
        return vec;

    case mtype2(-TYPE_GUID, -TYPE_GUID):
        vec = vector_guid(2);
        memcpy(&as_guid(vec)[0], as_guid(x), sizeof(guid_t));
        memcpy(&as_guid(vec)[1], as_guid(y), sizeof(guid_t));
        return vec;

    case mtype2(-TYPE_CHAR, -TYPE_CHAR):
        vec = string(2);
        as_string(vec)[0] = x->vchar;
        as_string(vec)[1] = y->vchar;
        return vec;

    case mtype2(TYPE_BOOL, -TYPE_BOOL):
        xl = x->len;
        vec = vector_bool(xl + 1);
        for (i = 0; i < xl; i++)
            as_bool(vec)[i] = as_bool(x)[i];

        as_bool(vec)[xl] = y->bool;
        return vec;

    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        xl = x->len;
        vec = vector_i64(xl + 1);
        for (i = 0; i < xl; i++)
            as_i64(vec)[i] = as_i64(x)[i];

        as_i64(vec)[xl] = y->i64;
        return vec;

    case mtype2(-TYPE_I64, TYPE_I64):
    case mtype2(-TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(-TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        yl = y->len;
        vec = vector(x->type, yl + 1);
        as_i64(vec)[0] = x->i64;
        for (i = 1; i <= yl; i++)
            as_i64(vec)[i] = as_i64(y)[i - 1];

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        xl = x->len;
        vec = vector_f64(xl + 1);
        for (i = 0; i < xl; i++)
            as_f64(vec)[i] = as_f64(x)[i];

        as_f64(vec)[xl] = y->f64;
        return vec;

    case mtype2(TYPE_GUID, -TYPE_GUID):
        xl = x->len;
        vec = vector_guid(xl + 1);
        for (i = 0; i < xl; i++)
            as_guid(vec)[i] = as_guid(x)[i];

        memcpy(&as_guid(vec)[xl], as_guid(y), sizeof(guid_t));
        return vec;

    case mtype2(-TYPE_GUID, TYPE_GUID):
        yl = y->len + 1;
        vec = vector_guid(yl);
        memcpy(&as_guid(vec)[0], as_guid(x), sizeof(guid_t));
        for (i = 1; i < yl; i++)
            as_guid(vec)[i] = as_guid(y)[i];

        return vec;

    case mtype2(TYPE_BOOL, TYPE_BOOL):
        xl = x->len;
        yl = y->len;
        vec = vector_bool(xl + yl);
        for (i = 0; i < xl; i++)
            as_bool(vec)[i] = as_bool(x)[i];
        for (i = 0; i < yl; i++)
            as_bool(vec)[i + xl] = as_bool(y)[i];
        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        xl = x->len;
        yl = y->len;
        vec = vector(x->type, xl + yl);
        for (i = 0; i < xl; i++)
            as_i64(vec)[i] = as_i64(x)[i];
        for (i = 0; i < yl; i++)
            as_i64(vec)[i + xl] = as_i64(y)[i];
        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        xl = x->len;
        yl = y->len;
        vec = vector_f64(xl + yl);
        for (i = 0; i < xl; i++)
            as_f64(vec)[i] = as_f64(x)[i];
        for (i = 0; i < yl; i++)
            as_f64(vec)[i + xl] = as_f64(y)[i];
        return vec;

    case mtype2(TYPE_GUID, TYPE_GUID):
        xl = x->len;
        yl = y->len;
        vec = vector_guid(xl + yl);
        for (i = 0; i < xl; i++)
            memcpy(as_guid(vec)[i].buf, as_guid(x)[i].buf, sizeof(guid_t));
        for (i = 0; i < yl; i++)
            memcpy(as_guid(vec)[i + xl].buf, as_guid(y)[i].buf, sizeof(guid_t));
        return vec;

    case mtype2(TYPE_CHAR, TYPE_CHAR):
        xl = ops_count(x);
        yl = ops_count(y);
        vec = string(xl + yl);
        for (i = 0; i < xl; i++)
            as_string(vec)[i] = as_string(x)[i];
        for (i = 0; i < yl; i++)
            as_string(vec)[i + xl] = as_string(y)[i];
        as_string(vec)[xl + yl] = '\0';
        return vec;

    case mtype2(TYPE_LIST, TYPE_LIST):
        xl = x->len;
        yl = y->len;
        vec = list(xl + yl);
        for (i = 0; i < xl; i++)
            as_list(vec)[i] = clone(as_list(x)[i]);
        for (i = 0; i < yl; i++)
            as_list(vec)[i + xl] = clone(as_list(y)[i]);
        return vec;

    default:
        if (x->type == TYPE_LIST)
        {
            xl = x->len;
            vec = list(xl + 1);
            for (i = 0; i < xl; i++)
                as_list(vec)[i] = clone(as_list(x)[i]);
            as_list(vec)[xl] = clone(y);

            return vec;
        }
        if (y->type == TYPE_LIST)
        {
            yl = y->len;
            vec = list(yl + 1);
            as_list(vec)[0] = clone(x);
            for (i = 0; i < yl; i++)
                as_list(vec)[i + 1] = clone(as_list(y)[i]);

            return vec;
        }

        throw(ERR_TYPE, "concat: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}
