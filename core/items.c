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

#include "items.h"
#include "util.h"
#include "heap.h"
#include "ops.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "serde.h"
#include "runtime.h"
#include "compose.h"
#include "order.h"
#include "misc.h"
#include "error.h"
#include "aggr.h"
#include "index.h"
#include "string.h"
#include "pool.h"

obj_p ray_at(obj_p x, obj_p y)
{
    u64_t i, j, yl, xl, n;
    obj_p res, k, s, v, cols;
    u8_t *buf;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_B8, -TYPE_I64):
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_F64, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
    case mtype2(TYPE_GUID, -TYPE_I64):
    case mtype2(TYPE_C8, -TYPE_I64):
    case mtype2(TYPE_LIST, -TYPE_I64):
        return at_idx(x, y->i64);

    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        return at_obj(x, y);

    case mtype2(TYPE_B8, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = vector_b8(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_b8(y)[i] >= (i64_t)xl)
                as_b8(res)[i] = B8_FALSE;
            else
                as_b8(res)[i] = as_b8(x)[(i32_t)as_b8(y)[i]];
        }

        return res;

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = vector(x->type, yl);
        for (i = 0; i < yl; i++)
        {
            if (as_i64(y)[i] >= (i64_t)xl)
                as_i64(res)[i] = NULL_I64;
            else
                as_i64(res)[i] = as_i64(x)[as_i64(y)[i]];
        }

        return res;

    case mtype2(TYPE_F64, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = vector_f64(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_i64(y)[i] >= (i64_t)xl)
                as_f64(res)[i] = NULL_F64;
            else
                as_f64(res)[i] = as_f64(x)[as_i64(y)[i]];
        }

        return res;

    case mtype2(TYPE_GUID, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = vector_guid(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_i64(y)[i] >= (i64_t)xl)
                memset(as_guid(res)[i], 0, sizeof(guid_t));
            else
                memcpy(as_guid(res)[i], as_guid(x)[as_i64(y)[i]], sizeof(guid_t));
        }

        return res;

    case mtype2(TYPE_C8, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = string(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_i64(y)[i] >= (i64_t)xl)
                as_string(res)[i] = ' ';
            else
                as_string(res)[i] = as_string(x)[(i32_t)as_i64(y)[i]];
        }

        return res;

    case mtype2(TYPE_LIST, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = vector(TYPE_LIST, yl);
        for (i = 0; i < yl; i++)
        {
            if (as_i64(y)[i] >= (i64_t)xl)
                as_list(res)[i] = NULL_OBJ;
            else
                as_list(res)[i] = clone_obj(as_list(x)[(i32_t)as_i64(y)[i]]);
        }

        return res;

    case mtype2(TYPE_TABLE, TYPE_SYMBOL):
        xl = as_list(x)[1]->len;
        yl = y->len;
        if (yl == 0)
            return NULL_OBJ;
        if (yl == 1)
        {
            for (j = 0; j < xl; j++)
            {
                if (as_symbol(as_list(x)[0])[j] == as_symbol(y)[0])
                    return clone_obj(as_list(as_list(x)[1])[j]);
            }

            throw(ERR_INDEX, "at: column '%s' has not found in a table", str_from_symbol(as_symbol(y)[0]));
        }

        cols = vector(TYPE_LIST, yl);
        for (i = 0; i < yl; i++)
        {
            for (j = 0; j < xl; j++)
            {
                if (as_symbol(as_list(x)[0])[j] == as_symbol(y)[i])
                {
                    as_list(cols)[i] = clone_obj(as_list(as_list(x)[1])[j]);
                    break;
                }
            }

            if (j == xl)
            {
                cols->len = i;
                drop_obj(cols);
                throw(ERR_INDEX, "at: column '%s' has not found in a table", str_from_symbol(as_symbol(y)[i]));
            }
        }

        return cols;

    case mtype2(TYPE_ENUM, -TYPE_I64):
        k = ray_key(x);
        s = ray_get(k);
        drop_obj(k);

        v = enum_val(x);

        if (y->i64 >= (i64_t)v->len)
        {
            drop_obj(s);
            throw(ERR_INDEX, "at: enum can not be resolved: index out of range");
        }

        if (!s || is_error(s) || s->type != TYPE_SYMBOL)
        {
            drop_obj(s);
            return i64(as_i64(v)[y->i64]);
        }

        if (as_i64(v)[y->i64] >= (i64_t)s->len)
        {
            drop_obj(s);
            throw(ERR_INDEX, "at: enum can not be resolved: index out of range");
        }

        res = at_idx(s, as_i64(v)[y->i64]);

        drop_obj(s);

        return res;

    case mtype2(TYPE_ENUM, TYPE_I64):
        k = ray_key(x);
        v = enum_val(x);

        s = ray_get(k);
        drop_obj(k);

        if (is_error(s))
            return s;

        xl = s->len;
        yl = y->len;
        n = v->len;

        if (!s || s->type != TYPE_SYMBOL)
        {
            res = vector_i64(yl);

            for (i = 0; i < yl; i++)
            {

                if (as_i64(y)[i] >= (i64_t)n)
                {
                    drop_obj(s);
                    drop_obj(res);
                    throw(ERR_INDEX, "at: enum can not be resolved: index out of range");
                }

                as_i64(res)[i] = as_i64(v)[as_i64(y)[i]];
            }

            drop_obj(s);

            return res;
        }

        res = vector_symbol(yl);

        for (i = 0; i < yl; i++)
        {
            if (as_i64(v)[i] >= (i64_t)xl)
            {
                drop_obj(s);
                drop_obj(res);
                throw(ERR_INDEX, "at: enum can not be resolved: index out of range");
            }

            as_symbol(res)[i] = as_symbol(s)[as_i64(v)[as_i64(y)[i]]];
        }

        drop_obj(s);

        return res;

    case mtype2(TYPE_ANYMAP, -TYPE_I64):
        k = anymap_key(x);
        v = anymap_val(x);

        xl = k->len;
        yl = v->len;

        if (y->i64 >= (i64_t)v->len)
            throw(ERR_INDEX, "at: anymap can not be resolved: index out of range");

        buf = as_u8(k) + as_i64(v)[y->i64];

        return load_obj(&buf, xl);

    case mtype2(TYPE_ANYMAP, TYPE_I64):
        k = anymap_key(x);
        v = anymap_val(x);

        n = v->len;
        yl = y->len;

        res = vector(TYPE_LIST, yl);

        for (i = 0; i < yl; i++)
        {
            if (as_i64(y)[i] >= (i64_t)n)
            {
                res->len = i;
                drop_obj(res);
                throw(ERR_INDEX, "at: anymap can not be resolved: index out of range");
            }

            buf = as_u8(k) + as_i64(v)[as_i64(y)[i]];
            as_list(res)[i] = load_obj(&buf, k->len);
        }

        return res;

    default:
        return at_obj(x, y);
    }
}

obj_p ray_find(obj_p x, obj_p y)
{
    u64_t i, l;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_B8, -TYPE_B8):
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(TYPE_F64, -TYPE_F64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
    case mtype2(TYPE_GUID, -TYPE_GUID):
    case mtype2(TYPE_C8, -TYPE_C8):
        l = x->len;
        i = find_obj(x, y);

        if (i == l)
            return i64(NULL_I64);
        else
            return i64(i);
    case mtype2(TYPE_B8, TYPE_B8):
    case mtype2(TYPE_U8, TYPE_U8):
    case mtype2(TYPE_C8, TYPE_C8):
        return index_find_i8((i8_t *)as_u8(x), x->len, (i8_t *)as_u8(y), y->len);
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        return index_find_i64(as_i64(x), x->len, as_i64(y), y->len);
    case mtype2(TYPE_F64, TYPE_F64):
        return index_find_i64((i64_t *)as_f64(x), x->len, (i64_t *)as_f64(y), y->len);
    case mtype2(TYPE_GUID, TYPE_GUID):
        return index_find_guid(as_guid(x), x->len, as_guid(y), y->len);
    case mtype2(TYPE_LIST, TYPE_LIST):
        return index_find_obj(as_list(x), x->len, as_list(y), y->len);
    default:
        throw(ERR_TYPE, "find: unsupported types: '%s '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_filter(obj_p x, obj_p y)
{
    u64_t i, j = 0, l;
    obj_p res, vals, col;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_B8, TYPE_B8):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_b8(l);
        for (i = 0; i < l; i++)
            if (as_b8(y)[i])
                as_b8(res)[j++] = as_b8(x)[i];

        resize_obj(&res, j);

        return res;

    case mtype2(TYPE_I64, TYPE_B8):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            if (as_b8(y)[i])
                as_i64(res)[j++] = as_i64(x)[i];

        resize_obj(&res, j);

        return res;

    case mtype2(TYPE_SYMBOL, TYPE_B8):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_symbol(l);
        for (i = 0; i < l; i++)
            if (as_b8(y)[i])
                as_symbol(res)[j++] = as_symbol(x)[i];

        resize_obj(&res, j);

        return res;

    case mtype2(TYPE_F64, TYPE_B8):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            if (as_b8(y)[i])
                as_f64(res)[j++] = as_f64(x)[i];

        resize_obj(&res, j);

        return res;

    case mtype2(TYPE_TIMESTAMP, TYPE_B8):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_timestamp(l);
        for (i = 0; i < l; i++)
            if (as_b8(y)[i])
                as_timestamp(res)[j++] = as_timestamp(x)[i];

        resize_obj(&res, j);

        return res;

    case mtype2(TYPE_GUID, TYPE_B8):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_guid(l);
        for (i = 0; i < l; i++)
            if (as_b8(y)[i])
                memcpy(as_guid(res)[j++], as_guid(x)[i], sizeof(guid_t));

        resize_obj(&res, j);

        return res;

    case mtype2(TYPE_C8, TYPE_B8):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = string(l);
        for (i = 0; i < l; i++)
            if (as_b8(y)[i])
                as_string(res)[j++] = as_string(x)[i];

        resize_obj(&res, j);

        return res;

    case mtype2(TYPE_LIST, TYPE_B8):
        if (x->len != y->len)
            return error_str(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = list(l);
        for (i = 0; i < l; i++)
            if (as_b8(y)[i])
                as_list(res)[j++] = clone_obj(as_list(x)[i]);

        resize_obj(&res, j);

        return res;

    case mtype2(TYPE_TABLE, TYPE_B8):
        vals = as_list(x)[1];
        l = vals->len;
        res = list(l);

        for (i = 0; i < l; i++)
        {
            col = ray_filter(as_list(vals)[i], y);
            as_list(res)[i] = col;
        }

        return table(clone_obj(as_list(x)[0]), res);

    default:
        throw(ERR_TYPE, "filter: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_take(obj_p x, obj_p y)
{
    u64_t i, l, m, n;
    obj_p k, s, v, res;
    u8_t *buf;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, TYPE_I64):
    case mtype2(-TYPE_I64, TYPE_SYMBOL):
    case mtype2(-TYPE_I64, TYPE_TIMESTAMP):
        l = y->len;
        m = absi64(x->i64);
        res = vector(y->type, m);

        if (x->i64 >= 0)
        {
            for (i = 0; i < m; i++)
                as_i64(res)[i] = as_i64(y)[i % l];
        }
        else
        {
            for (i = 0; i < m; i++)
                as_i64(res)[i] = as_i64(y)[l - 1 - (i % l)];
        }

        return res;

    case mtype2(-TYPE_I64, TYPE_F64):
        l = y->len;
        m = absi64(x->i64);
        res = vector_f64(m);

        if (x->i64 >= 0)
        {
            for (i = 0; i < m; i++)
                as_f64(res)[i] = as_f64(y)[i % l];
        }
        else
        {
            for (i = 0; i < m; i++)
                as_f64(res)[i] = as_f64(y)[l - 1 - (i % l)];
        }

        return res;

    case mtype2(-TYPE_I64, -TYPE_I64):
    case mtype2(-TYPE_I64, -TYPE_SYMBOL):
    case mtype2(-TYPE_I64, -TYPE_TIMESTAMP):
        l = absi64(x->i64);
        res = vector_i64(l);

        for (i = 0; i < l; i++)
            as_i64(res)[i] = y->i64;

        return res;

    case mtype2(-TYPE_I64, -TYPE_F64):
        l = absi64(x->i64);
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(res)[i] = y->f64;

        return res;

    case mtype2(-TYPE_I64, -TYPE_GUID):
        l = absi64(x->i64);
        res = vector_guid(l);
        for (i = 0; i < l; i++)
            memcpy(as_guid(res)[i], as_guid(y)[0], sizeof(guid_t));

        return res;

    case mtype2(-TYPE_I64, TYPE_ENUM):
        k = ray_key(y);
        s = ray_get(k);
        drop_obj(k);

        if (is_error(s))
            return s;

        v = enum_val(y);

        l = absi64(x->i64);
        m = v->len;

        if (!s || s->type != TYPE_SYMBOL)
        {
            res = vector_i64(l);

            if (x->i64 >= 0)
            {
                for (i = 0; i < l; i++)
                    as_i64(res)[i] = as_i64(v)[i % m];
            }
            else
            {
                for (i = 0; i < l; i++)
                    as_i64(res)[i] = as_i64(v)[m - 1 - (i % m)];
            }

            drop_obj(s);

            return res;
        }

        res = vector_symbol(l);

        if (x->i64 >= 0)
        {
            for (i = 0; i < l; i++)
            {
                if (as_i64(v)[i % m] >= (i64_t)s->len)
                {
                    drop_obj(s);
                    drop_obj(res);
                    throw(ERR_INDEX, "take: enum can not be resolved: index out of range");
                }

                as_symbol(res)[i] = as_symbol(s)[as_i64(v)[i % m]];
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                if (as_i64(v)[m - 1 - (i % m)] >= (i64_t)s->len)
                {
                    drop_obj(s);
                    drop_obj(res);
                    throw(ERR_INDEX, "take: enum can not be resolved: index out of range");
                }

                as_symbol(res)[i] = as_symbol(s)[as_i64(v)[m - 1 - (i % m)]];
            }
        }

        drop_obj(s);

        return res;

    case mtype2(-TYPE_I64, TYPE_ANYMAP):
        l = absi64(x->i64);
        res = vector(TYPE_LIST, l);

        k = anymap_key(y);
        s = anymap_val(y);

        m = k->len;
        n = s->len;

        if (x->i64 >= 0)
        {
            for (i = 0; i < l; i++)
            {
                if (as_i64(s)[i % n] >= (i64_t)m)
                {
                    buf = as_u8(k) + as_i64(s)[i % n];
                    v = load_obj(&buf, l);

                    if (is_error(v))
                    {
                        res->len = i;
                        drop_obj(res);
                        return v;
                    }

                    as_list(res)[i] = v;
                }
                else
                {
                    res->len = i;
                    drop_obj(res);
                    throw(ERR_INDEX, "anymap value: index out of range: %d", as_i64(s)[i % n]);
                }
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                if (as_i64(s)[n - 1 - (i % n)] >= (i64_t)m)
                {
                    buf = as_u8(k) + as_i64(s)[n - 1 - (i % n)];
                    v = load_obj(&buf, l);

                    if (is_error(v))
                    {
                        res->len = i;
                        drop_obj(res);
                        return v;
                    }

                    as_list(res)[i] = v;
                }
                else
                {
                    res->len = i;
                    drop_obj(res);
                    throw(ERR_INDEX, "anymap value: index out of range: %d", as_i64(s)[n - 1 - (i % n)]);
                }
            }
        }

        return res;

    case mtype2(-TYPE_I64, TYPE_C8):
        m = absi64(x->i64);
        n = y->len;
        res = string(m);

        if (x->i64 >= 0)
        {
            for (i = 0; i < m; i++)
                as_string(res)[i] = as_string(y)[i % n];
        }
        else
        {
            for (i = 0; i < m; i++)
                as_string(res)[i] = as_string(y)[n - 1 - (i % n)];
        }

        return res;

    case mtype2(-TYPE_I64, TYPE_LIST):
        m = absi64(x->i64);
        n = y->len;
        res = vector(TYPE_LIST, m);

        if (x->i64 >= 0)
        {
            for (i = 0; i < m; i++)
                as_list(res)[i] = clone_obj(as_list(y)[i % n]);
        }
        else
        {
            for (i = 0; i < m; i++)
                as_list(res)[i] = clone_obj(as_list(y)[n - 1 - (i % n)]);
        }

        return res;

    case mtype2(-TYPE_I64, TYPE_GUID):
        m = absi64(x->i64);
        n = y->len;
        res = vector_guid(m);

        if (x->i64 >= 0)
        {
            for (i = 0; i < m; i++)
                memcpy(as_guid(res)[i], as_guid(y)[i % n], sizeof(guid_t));
        }
        else
        {
            for (i = 0; i < m; i++)
                memcpy(as_guid(res)[i], as_guid(y)[n - 1 - (i % n)], sizeof(guid_t));
        }

        return res;

    case mtype2(-TYPE_I64, TYPE_TABLE):
        l = as_list(y)[1]->len;
        res = vector(TYPE_LIST, l);
        for (i = 0; i < l; i++)
        {
            v = ray_take(x, as_list(as_list(y)[1])[i]);

            if (is_error(v))
            {
                res->len = i;
                drop_obj(v);
                drop_obj(res);
                return v;
            }

            as_list(res)[i] = v;
        }

        return table(clone_obj(as_list(y)[0]), res);

    default:
        throw(ERR_TYPE, "take: unsupported types: '%s, %s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_in(obj_p x, obj_p y)
{
    i64_t i, xl, yl, p;
    obj_p vec, set;

    switch
        mtype2(x->type, y->type)
        {
        case mtype2(TYPE_I64, TYPE_I64):
        case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
            xl = x->len;
            yl = y->len;
            set = ht_oa_create(yl, -1);

            for (i = 0; i < yl; i++)
            {
                // p = ht_oa_tab_next_with(&set, as_i64(y)[i], &hash_i64, &cmp_i64);
                p = ht_oa_tab_next(&set, as_i64(y)[i]);
                if (as_i64(as_list(set)[0])[p] == NULL_I64)
                    as_i64(as_list(set)[0])[p] = as_i64(y)[i];
            }

            vec = vector_b8(xl);

            for (i = 0; i < xl; i++)
            {
                // p = ht_oa_tab_next_with(&set, as_i64(x)[i], &hash_i64, &cmp_i64);
                p = ht_oa_tab_get(set, as_i64(x)[i]);
                as_b8(vec)[i] = (p != NULL_I64);
            }

            drop_obj(set);

            return vec;

        default:
            throw(ERR_TYPE, "in: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
        }

    return NULL_OBJ;
}

obj_p ray_sect(obj_p x, obj_p y)
{
    obj_p mask, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
        mask = ray_in(x, y);
        res = ray_filter(x, mask);
        drop_obj(mask);
        return res;

    default:
        throw(ERR_TYPE, "sect: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }

    return NULL_OBJ;
}

obj_p ray_except(obj_p x, obj_p y)
{
    i64_t i, j = 0, l;
    obj_p mask, nmask, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
        l = x->len;
        res = vector(x->type, l);

        for (i = 0; i < l; i++)
        {
            if (as_i64(x)[i] != y->i64)
                as_i64(res)[j++] = as_i64(x)[i];
        }

        resize_obj(&res, j);

        return res;
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
        mask = ray_in(x, y);
        nmask = ray_not(mask);
        drop_obj(mask);
        res = ray_filter(x, nmask);
        drop_obj(nmask);
        return res;
    default:
        throw(ERR_TYPE, "except: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

// TODO: implement
obj_p ray_union(obj_p x, obj_p y)
{
    obj_p c, res;

    c = ray_concat(x, y);
    res = ray_distinct(c);
    drop_obj(c);
    return res;
}

obj_p ray_first(obj_p x)
{
    return at_idx(x, 0);
}

obj_p ray_last(obj_p x)
{
    u64_t l = ops_count(x);

    return at_idx(x, l ? l - 1 : l);
}

obj_p ray_key(obj_p x)
{
    switch (x->type)
    {
    case TYPE_TABLE:
    case TYPE_DICT:
        return clone_obj(as_list(x)[0]);
    case TYPE_ENUM:
        return symbol(enum_key(x), enum_key_len(x));
    case TYPE_ANYMAP:
        return clone_obj(anymap_key(x));
    default:
        return clone_obj(x);
    }
}

obj_p ray_value(obj_p x)
{
    obj_p sym, k, v, res, e;
    i64_t i, l, sl, xl;
    u8_t *buf;

    switch (x->type)
    {
    case TYPE_ENUM:
        k = ray_key(x);
        sym = at_obj(runtime_get()->env.variables, k);
        drop_obj(k);

        e = enum_val(x);
        xl = e->len;

        if (is_null(sym) || sym->type != TYPE_SYMBOL)
        {
            res = vector_i64(xl);

            for (i = 0; i < xl; i++)
                as_i64(res)[i] = as_i64(e)[i];

            drop_obj(sym);

            return res;
        }

        sl = sym->len;

        res = vector_symbol(xl);

        for (i = 0; i < xl; i++)
        {
            if (as_i64(e)[i] < sl)
                as_symbol(res)[i] = as_symbol(sym)[as_i64(e)[i]];
            else
                as_symbol(res)[i] = NULL_I64;
        }

        drop_obj(sym);

        return res;

    case TYPE_ANYMAP:
        k = anymap_key(x);
        e = anymap_val(x);

        xl = e->len;
        sl = k->len;

        res = vector(TYPE_LIST, xl);

        for (i = 0; i < xl; i++)
        {
            if (as_i64(e)[i] < sl)
            {
                buf = as_u8(k) + as_i64(e)[i];
                v = load_obj(&buf, sl);

                if (is_error(v))
                {
                    res->len = i;
                    drop_obj(res);
                    return v;
                }

                as_list(res)[i] = v;
            }
            else
            {
                res->len = i;
                drop_obj(res);
                throw(ERR_INDEX, "anymap value: index out of range: %d", as_i64(e)[i]);
            }
        }

        return res;

    case TYPE_TABLE:
        l = as_list(x)[1]->len;
        v = vector(TYPE_LIST, l);
        for (i = 0; i < l; i++)
        {
            res = ray_value(as_list(as_list(x)[1])[i]);
            if (is_error(res))
            {
                drop_obj(v);
                return res;
            }

            as_list(v)[i] = res;
        }
        return table(clone_obj(as_list(x)[0]), v);

    case TYPE_DICT:
        return clone_obj(as_list(x)[1]);

    default:
        return clone_obj(x);
    }
}

obj_p ray_where(obj_p x)
{
    u64_t i, j, l, ones;
    i64_t *cur;
    b8_t *bcur;
    obj_p res;

    switch (x->type)
    {
    case TYPE_B8:
        l = x->len;
        res = vector_i64(l);
        cur = as_i64(res);
        bcur = as_b8(x);

        for (i = 0, j = 0; i < l; i++)
            if (bcur[i])
                cur[j++] = i;

        resize_obj(&res, j);

        return res;
    case TYPE_NULL:
        return vector_i64(0);
    default:
        throw(ERR_TYPE, "where: unsupported type: '%s", type_name(x->type));
    }
}
