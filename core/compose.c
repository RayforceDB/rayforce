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
#include "index.h"
#include "error.h"

obj_p ray_cast_obj(obj_p x, obj_p y)
{
    i8_t type;
    obj_p err;
    obj_p fmt, msg;

    if (x->type != -TYPE_SYMBOL)
        throw(ERR_TYPE, "as: first argument must be a symbol");

    type = env_get_type_by_type_name(&runtime_get()->env, x->i64);

    if (type == TYPE_ERROR)
    {
        fmt = obj_fmt(x);
        msg = str_fmt(-1, "as: not a type: '%s", fmt);
        err = error_obj(ERR_TYPE, msg);
        heap_free(fmt);
        heap_free(msg);
        return err;
    }

    return cast_obj(type, y);
}

obj_p ray_til(obj_p x)
{
    if (x->type != -TYPE_I64)
        return error_str(ERR_TYPE, "til: expected i64");

    i32_t i, l = (i32_t)x->i64;
    obj_p vec = NULL;

    vec = vector_i64(l);

    if (is_error(vec))
        return vec;

    for (i = 0; i < l; i++)
        as_i64(vec)[i] = i;

    vec->attrs = ATTR_ASC | ATTR_DISTINCT;

    return vec;
}

obj_p ray_reverse(obj_p x)
{
    i64_t i, l;
    obj_p res;

    switch (x->type)
    {
    case TYPE_C8:
    case TYPE_U8:
    case TYPE_B8:
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
            as_list(res)[i] = clone_obj(as_list(x)[l - i - 1]);

        return res;

    default:
        throw(ERR_TYPE, "reverse: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_dict(obj_p x, obj_p y)
{
    if (!is_vector(x) || !is_vector(y))
        return error_str(ERR_TYPE, "Keys and Values must be lists");

    if (ops_count(x) != ops_count(y))
        return error_str(ERR_LENGTH, "Keys and Values must have the same length");

    return dict(clone_obj(x), clone_obj(y));
}

obj_p ray_table(obj_p x, obj_p y)
{
    b8_t synergy = B8_TRUE;
    u64_t i, j, len, cl = 0;
    obj_p lst, c, l = NULL_OBJ;

    if (x->type != TYPE_SYMBOL)
        return error_str(ERR_TYPE, "table: keys must be a symbol vector");

    if (y->type != TYPE_LIST)
    {
        if (x->len != 1)
            return error_str(ERR_LENGTH, "table: keys and values must have the same length");

        l = list(1);
        as_list(l)[0] = clone_obj(y);
        y = l;
    }

    if (x->len != y->len && y->len > 0)
    {
        drop_obj(l);
        return error_str(ERR_LENGTH, "table: keys and values must have the same length");
    }

    len = y->len;

    for (i = 0; i < len; i++)
    {
        switch (as_list(y)[i]->type)
        {
        case -TYPE_B8:
        case -TYPE_U8:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_C8:
        case -TYPE_SYMBOL:
        case -TYPE_TIMESTAMP:
        case -TYPE_GUID:
        case TYPE_LAMBDA:
        case TYPE_DICT:
        case TYPE_TABLE:
            synergy = B8_FALSE;
            break;
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_I64:
        case TYPE_F64:
        case TYPE_TIMESTAMP:
        case TYPE_C8:
        case TYPE_SYMBOL:
        case TYPE_LIST:
        case TYPE_GUID:
            j = as_list(y)[i]->len;
            if (cl != 0 && j != cl)
                return error(ERR_LENGTH, "table: values must be of the same length");

            cl = j;
            break;
        case TYPE_ENUM:
            synergy = B8_FALSE;
            j = as_list(as_list(y)[i])[1]->len;
            if (cl != 0 && j != cl)
                return error(ERR_LENGTH, "table: values must be of the same length");

            cl = j;
            break;
        default:
            return error(ERR_TYPE, "table: unsupported type: '%s' in a values list", type_name(as_list(y)[i]->type));
        }
    }

    // there are no atoms, no lazytypes and all columns are of the same length
    if (synergy)
    {
        drop_obj(l);
        return table(clone_obj(x), clone_obj(y));
    }

    // otherwise we need to expand atoms to vectors
    lst = vector(TYPE_LIST, len);

    if (cl == 0)
        cl = 1;

    for (i = 0; i < len; i++)
    {
        switch (as_list(y)[i]->type)
        {
        case -TYPE_B8:
        case -TYPE_U8:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_C8:
        case -TYPE_SYMBOL:
        case -TYPE_TIMESTAMP:
        case -TYPE_GUID:
            c = i64(cl);
            as_list(lst)[i] = ray_take(c, as_list(y)[i]);
            drop_obj(c);
            break;
        case TYPE_ENUM:
            as_list(lst)[i] = ray_value(as_list(y)[i]);
            break;
        default:
            as_list(lst)[i] = clone_obj(as_list(y)[i]);
        }
    }

    drop_obj(l);

    return table(clone_obj(x), lst);
}

obj_p ray_guid(obj_p x)
{
    i64_t i, count;
    obj_p vec;
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
        throw(ERR_TYPE, "guid: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_list(obj_p *x, u64_t n)
{
    u64_t i;
    obj_p lst = list(n);

    for (i = 0; i < n; i++)
        as_list(lst)[i] = clone_obj(x[i]);

    return lst;
}

obj_p ray_enlist(obj_p *x, u64_t n)
{
    u64_t i;
    obj_p lst;

    if (n == 0)
        return list(0);

    lst = vector(x[0]->type, n);

    for (i = 0; i < n; i++)
        ins_obj(&lst, i, clone_obj(x[i]));

    return lst;
}

obj_p ray_enum(obj_p x, obj_p y)
{
    obj_p s, v;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_SYMBOL, TYPE_SYMBOL):
        s = ray_get(x);

        if (is_error(s))
            return s;

        if (!s || s->type != TYPE_SYMBOL)
        {
            drop_obj(s);
            throw(ERR_TYPE, "enum: expected vector symbol");
        }

        v = index_find_i64(as_i64(s), s->len, as_i64(y), y->len);
        drop_obj(s);

        if (is_error(v))
        {
            drop_obj(v);
            throw(ERR_TYPE, "enum: can not be fully indexed");
        }

        return enumerate(clone_obj(x), v);
    default:
        throw(ERR_TYPE, "enum: unsupported types: '%s '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_rand(obj_p x, obj_p y)
{
    i64_t i, count;
    obj_p vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        count = x->i64;
        vec = vector_i64(count);

        for (i = 0; i < count; i++)
            as_i64(vec)[i] = ops_rand_u64() % y->i64;

        return vec;

    default:
        throw(ERR_TYPE, "rand: unsupported types: '%s '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_concat(obj_p x, obj_p y)
{
    i64_t i, xl, yl;
    obj_p vec;

    if (!x || !y)
        return vn_list(2, clone_obj(x), clone_obj(y));

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_B8, -TYPE_B8):
        vec = vector_b8(2);
        as_b8(vec)[0] = x->b8;
        as_b8(vec)[1] = y->b8;
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

    case mtype2(-TYPE_C8, -TYPE_C8):
        vec = string(2);
        as_string(vec)[0] = x->c8;
        as_string(vec)[1] = y->c8;
        return vec;

    case mtype2(TYPE_B8, -TYPE_B8):
        xl = x->len;
        vec = vector_b8(xl + 1);
        for (i = 0; i < xl; i++)
            as_b8(vec)[i] = as_b8(x)[i];

        as_b8(vec)[xl] = y->b8;
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

    case mtype2(TYPE_B8, TYPE_B8):
        xl = x->len;
        yl = y->len;
        vec = vector_b8(xl + yl);
        for (i = 0; i < xl; i++)
            as_b8(vec)[i] = as_b8(x)[i];
        for (i = 0; i < yl; i++)
            as_b8(vec)[i + xl] = as_b8(y)[i];
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

    case mtype2(TYPE_C8, TYPE_C8):
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
            as_list(vec)[i] = clone_obj(as_list(x)[i]);
        for (i = 0; i < yl; i++)
            as_list(vec)[i + xl] = clone_obj(as_list(y)[i]);
        return vec;

    default:
        if (x->type == TYPE_LIST)
        {
            xl = x->len;
            vec = list(xl + 1);
            for (i = 0; i < xl; i++)
                as_list(vec)[i] = clone_obj(as_list(x)[i]);
            as_list(vec)[xl] = clone_obj(y);

            return vec;
        }
        if (y->type == TYPE_LIST)
        {
            yl = y->len;
            vec = list(yl + 1);
            as_list(vec)[0] = clone_obj(x);
            for (i = 0; i < yl; i++)
                as_list(vec)[i + 1] = clone_obj(as_list(y)[i]);

            return vec;
        }

        throw(ERR_TYPE, "concat: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_distinct(obj_p x)
{
    obj_p res = NULL;
    u64_t l;

    switch (x->type)
    {
    case TYPE_B8:
    case TYPE_U8:
    case TYPE_C8:
        l = ops_count(x);
        res = index_distinct_i8((i8_t *)as_u8(x), l, x->type == TYPE_C8);
        res->type = x->type;
        return res;
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        l = x->len;
        res = index_distinct_i64(as_i64(x), l);
        res->type = x->type;
        return res;
    case TYPE_ENUM:
        l = ops_count(x);
        res = index_distinct_i64(as_i64(enum_val(x)), l);
        res = enumerate(ray_key(x), res);
        return res;
    case TYPE_LIST:
        l = ops_count(x);
        res = index_distinct_obj(as_list(x), l);
        return res;
    case TYPE_GUID:
        l = x->len;
        res = index_distinct_guid(as_guid(x), l);
        return res;
    default:
        throw(ERR_TYPE, "distinct: invalid type: '%s", type_name(x->type));
    }
}

obj_p ray_group(obj_p x)
{
    obj_p c, g, k, v;
    u64_t i, m, n, l;

    switch (x->type)
    {
    case TYPE_U8:
    case TYPE_B8:
    case TYPE_C8:
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

        drop_obj(c);
        drop_obj(g);

        return dict(k, v);
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        g = index_group_i64(as_i64(x), NULL, ops_count(x));
        l = as_list(g)[0]->i64;
        c = index_group_cnts(g);

        k = vector(x->type, l);
        v = list(l);

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

        drop_obj(c);
        drop_obj(g);

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

        drop_obj(g);

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

        drop_obj(c);
        drop_obj(g);

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

        drop_obj(c);
        drop_obj(g);

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
                as_list(k)[n] = clone_obj(as_list(x)[i]);

            as_list(v)[n]->len++;
        }

        drop_obj(c);
        drop_obj(g);

        return dict(k, v);
    default:
        throw(ERR_TYPE, "group: unsupported type: '%s", type_name(x->type));
    }
}
