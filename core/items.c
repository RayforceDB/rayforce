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

obj_t ray_at(obj_t x, obj_t y)
{
    u64_t i, j, yl, xl, n;
    obj_t res, k, s, v, c, cols;
    u8_t *buf;

    if (!x || !y)
        return null(0);

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_BOOL, -TYPE_I64):
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_F64, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
    case mtype2(TYPE_GUID, -TYPE_I64):
    case mtype2(TYPE_CHAR, -TYPE_I64):
    case mtype2(TYPE_LIST, -TYPE_I64):
        return at_idx(x, y->i64);

    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        return at_obj(x, y);

    case mtype2(TYPE_BOOL, TYPE_I64):
        yl = y->len;
        xl = x->len;
        res = vector_bool(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_bool(y)[i] >= (i64_t)xl)
                as_bool(res)[i] = false;
            else
                as_bool(res)[i] = as_bool(x)[(i32_t)as_bool(y)[i]];
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
                as_guid(res)[i] = (guid_t){0};
            else
                as_guid(res)[i] = as_guid(x)[as_i64(y)[i]];
        }

        return res;

    case mtype2(TYPE_CHAR, TYPE_I64):
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
                as_list(res)[i] = null(0);
            else
                as_list(res)[i] = clone(as_list(x)[(i32_t)as_i64(y)[i]]);
        }

        return res;

    case mtype2(TYPE_TABLE, TYPE_I64):
        xl = as_list(x)[0]->len;
        cols = vector(TYPE_LIST, xl);
        for (i = 0; i < xl; i++)
        {
            c = ray_at(as_list(as_list(x)[1])[i], y);

            if (is_atom(c))
                c = ray_enlist(&c, 1);

            if (is_error(c))
            {
                cols->len = i;
                drop(cols);
                return c;
            }

            ins_obj(&cols, i, c);
        }

        res = table(clone(as_list(x)[0]), cols);

        return res;

    case mtype2(TYPE_TABLE, TYPE_SYMBOL):
        xl = as_list(x)[1]->len;
        yl = y->len;
        if (yl == 0)
            return null(0);
        if (yl == 1)
        {
            for (j = 0; j < xl; j++)
            {
                if (as_symbol(as_list(x)[0])[j] == as_symbol(y)[0])
                    return clone(as_list(as_list(x)[1])[j]);
            }

            throw(ERR_INDEX, "at: column '%s' has not found in a table", symtostr(as_symbol(y)[0]));
        }

        cols = vector(TYPE_LIST, yl);
        for (i = 0; i < yl; i++)
        {
            for (j = 0; j < xl; j++)
            {
                if (as_symbol(as_list(x)[0])[j] == as_symbol(y)[i])
                {
                    as_list(cols)[i] = clone(as_list(as_list(x)[1])[j]);
                    break;
                }
            }

            if (j == xl)
            {
                cols->len = i;
                drop(cols);
                throw(ERR_INDEX, "at: column '%s' has not found in a table", symtostr(as_symbol(y)[i]));
            }
        }

        return cols;

    case mtype2(TYPE_ENUM, -TYPE_I64):
        k = ray_key(x);
        s = ray_get(k);
        drop(k);

        v = enum_val(x);

        if (y->i64 >= (i64_t)v->len)
        {
            drop(s);
            throw(ERR_INDEX, "at: enum can not be resolved: index out of range");
        }

        if (!s || is_error(s) || s->type != TYPE_SYMBOL)
        {
            drop(s);
            return i64(as_i64(v)[y->i64]);
        }

        if (as_i64(v)[y->i64] >= (i64_t)s->len)
        {
            drop(s);
            throw(ERR_INDEX, "at: enum can not be resolved: index out of range");
        }

        res = at_idx(s, as_i64(v)[y->i64]);

        drop(s);

        return res;

    case mtype2(TYPE_ENUM, TYPE_I64):
        k = ray_key(x);
        v = enum_val(x);

        s = ray_get(k);
        drop(k);

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
                    drop(s);
                    drop(res);
                    throw(ERR_INDEX, "at: enum can not be resolved: index out of range");
                }

                as_i64(res)[i] = as_i64(v)[as_i64(y)[i]];
            }

            drop(s);

            return res;
        }

        res = vector_symbol(yl);

        for (i = 0; i < yl; i++)
        {
            if (as_i64(v)[i] >= (i64_t)xl)
            {
                drop(s);
                drop(res);
                throw(ERR_INDEX, "at: enum can not be resolved: index out of range");
            }

            as_symbol(res)[i] = as_symbol(s)[as_i64(v)[as_i64(y)[i]]];
        }

        drop(s);

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
                drop(res);
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

typedef struct __items_find_ctx_t
{
    obj_t lobj;
    obj_t robj;
    u64_t *hashes;
} __items_find_ctx_t;

u64_t __items_hash_get(i64_t row, nil_t *seed)
{
    __items_find_ctx_t *ctx = (__items_find_ctx_t *)seed;
    return ctx->hashes[row];
}

i32_t __items_cmp_idx(i64_t row1, i64_t row2, nil_t *seed)
{
    __items_find_ctx_t *ctx = (__items_find_ctx_t *)seed;
    return ops_eq_idx(ctx->lobj, row1, ctx->robj, row2) == 0;
}

obj_t ray_find(obj_t x, obj_t y)
{
    u64_t i, l, xl, yl;
    obj_t ht, res;
    i64_t idx;
    __items_find_ctx_t ctx;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_BOOL, -TYPE_BOOL):
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_F64, -TYPE_F64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
    case mtype2(TYPE_GUID, -TYPE_GUID):
    case mtype2(TYPE_CHAR, -TYPE_CHAR):
        l = x->len;
        i = find_obj(x, y);

        if (i == l)
            return i64(NULL_I64);
        else
            return i64(i);

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "find: vectors must be of same length");
        return ops_find(as_i64(x), x->len, as_i64(y), y->len);
    case mtype2(TYPE_LIST, TYPE_LIST):
        xl = x->len;
        yl = y->len;

        // calc hashes
        res = vector_i64(maxi64(xl, yl));
        ht = ht_tab(maxi64(xl, yl) * 2, -1);

        // initialize hashes
        for (i = 0; i < xl; i++)
            as_i64(res)[i] = 0xa5b6c7d8e9f01234ull;

        ops_hash_list(x, (u64_t *)as_i64(res), xl);
        ctx = (__items_find_ctx_t){.lobj = x, .robj = x, .hashes = (u64_t *)as_i64(res)};
        for (i = 0; i < xl; i++)
        {
            idx = ht_tab_next_with(&ht, i, &__items_hash_get, &__items_cmp_idx, &ctx);
            if (as_i64(as_list(ht)[0])[idx] == NULL_I64)
                as_i64(as_list(ht)[0])[idx] = i;
        }

        // (re)initialize hashes
        for (i = 0; i < yl; i++)
            as_i64(res)[i] = 0xa5b6c7d8e9f01234ull;

        ops_hash_list(y, (u64_t *)as_i64(res), yl);
        ctx = (__items_find_ctx_t){.lobj = x, .robj = y, .hashes = (u64_t *)as_i64(res)};
        for (i = 0; i < yl; i++)
        {
            idx = ht_tab_get_with(ht, i, &__items_hash_get, &__items_cmp_idx, &ctx);
            as_i64(res)[i] = (idx == NULL_I64) ? NULL_I64 : as_i64(as_list(ht)[0])[idx];
        }

        drop(ht);
        resize(&res, yl);

        return res;
    default:
        throw(ERR_TYPE, "find: unsupported types: '%s '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_filter(obj_t x, obj_t y)
{
    u64_t i, j = 0, l;
    obj_t res, vals, col;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_BOOL, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_bool(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_bool(res)[j++] = as_bool(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_I64, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_i64(res)[j++] = as_i64(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_SYMBOL, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_symbol(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_symbol(res)[j++] = as_symbol(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_F64, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_f64(res)[j++] = as_f64(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_TIMESTAMP, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_timestamp(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_timestamp(res)[j++] = as_timestamp(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_GUID, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = vector_guid(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                memcpy(as_guid(res)[j++].buf, as_guid(x)[i].buf, sizeof(guid_t));

        resize(&res, j);

        return res;

    case mtype2(TYPE_CHAR, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = string(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_string(res)[j++] = as_string(x)[i];

        resize(&res, j);

        return res;

    case mtype2(TYPE_LIST, TYPE_BOOL):
        if (x->len != y->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->len;
        res = list(l);
        for (i = 0; i < l; i++)
            if (as_bool(y)[i])
                as_list(res)[j++] = clone(as_list(x)[i]);

        resize(&res, j);

        return res;

    case mtype2(TYPE_TABLE, TYPE_BOOL):
        vals = as_list(x)[1];
        l = vals->len;
        res = list(l);

        for (i = 0; i < l; i++)
        {
            col = ray_filter(as_list(vals)[i], y);
            as_list(res)[i] = col;
        }

        return table(clone(as_list(x)[0]), res);

    default:
        throw(ERR_TYPE, "filter: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

obj_t ray_take(obj_t x, obj_t y)
{
    i64_t *idxs;
    u64_t i, l, m, n;
    obj_t k, s, v, res;
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

    case mtype2(-TYPE_I64, TYPE_ENUM):
        k = ray_key(y);
        s = ray_get(k);
        drop(k);

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

            drop(s);

            return res;
        }

        res = vector_symbol(l);

        if (x->i64 >= 0)
        {
            for (i = 0; i < l; i++)
            {
                if (as_i64(v)[i % m] >= (i64_t)s->len)
                {
                    drop(s);
                    drop(res);
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
                    drop(s);
                    drop(res);
                    throw(ERR_INDEX, "take: enum can not be resolved: index out of range");
                }

                as_symbol(res)[i] = as_symbol(s)[as_i64(v)[m - 1 - (i % m)]];
            }
        }

        drop(s);

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
                        drop(res);
                        return v;
                    }

                    as_list(res)[i] = v;
                }
                else
                {
                    res->len = i;
                    drop(res);
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
                        drop(res);
                        return v;
                    }

                    as_list(res)[i] = v;
                }
                else
                {
                    res->len = i;
                    drop(res);
                    throw(ERR_INDEX, "anymap value: index out of range: %d", as_i64(s)[n - 1 - (i % n)]);
                }
            }
        }

        return res;

    case mtype2(-TYPE_I64, TYPE_CHAR):
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
                as_list(res)[i] = clone(as_list(y)[i % n]);
        }
        else
        {
            for (i = 0; i < m; i++)
                as_list(res)[i] = clone(as_list(y)[n - 1 - (i % n)]);
        }

        return res;

    case mtype2(-TYPE_I64, TYPE_GUID):
        m = absi64(x->i64);
        n = y->len;
        res = vector_guid(m);

        if (x->i64 >= 0)
        {
            for (i = 0; i < m; i++)
                memcpy(as_guid(res)[i].buf, as_guid(y)[i % n].buf, sizeof(guid_t));
        }
        else
        {
            for (i = 0; i < m; i++)
                memcpy(as_guid(res)[i].buf, as_guid(y)[n - 1 - (i % n)].buf, sizeof(guid_t));
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
                drop(v);
                drop(res);
                return v;
            }

            as_list(res)[i] = v;
        }

        return table(clone(as_list(y)[0]), res);

    case mtype2(-TYPE_I64, TYPE_VECMAP):
        idxs = as_i64(as_list(y)[1]); // idxs
        l = as_list(y)[1]->len;       // idxs len
        v = as_list(y)[0];            // vals
        n = absi64(x->i64);           // take len

        res = vector(v->type, n);

        if (x->i64 >= 0)
        {
            for (i = 0; i < n; i++)
                ins_obj(&res, i, at_idx(v, idxs[i % l]));
        }
        else
        {
            for (i = 0; i < n; i++)
                ins_obj(&res, i, at_idx(v, idxs[l - 1 - (i % l)]));
        }

        return res;

    default:
        throw(ERR_TYPE, "take: unsupported types: '%s, %s", typename(x->type), typename(y->type));
    }
}

obj_t ray_in(obj_t x, obj_t y)
{
    i64_t i, xl, yl, p;
    obj_t vec, set;

    switch
        mtype2(x->type, y->type)
        {
        case mtype2(TYPE_I64, TYPE_I64):
        case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
            xl = x->len;
            yl = y->len;
            set = ht_tab(yl, -1);

            for (i = 0; i < yl; i++)
            {
                // p = ht_tab_next_with(&set, as_i64(y)[i], &i64_hash, &i64_cmp);
                p = ht_tab_next(&set, as_i64(y)[i]);
                if (as_i64(as_list(set)[0])[p] == NULL_I64)
                    as_i64(as_list(set)[0])[p] = as_i64(y)[i];
            }

            vec = vector_bool(xl);

            for (i = 0; i < xl; i++)
            {
                // p = ht_tab_next_with(&set, as_i64(x)[i], &i64_hash, &i64_cmp);
                p = ht_tab_get(set, as_i64(x)[i]);
                as_bool(vec)[i] = (p != NULL_I64);
            }

            drop(set);

            return vec;

        default:
            throw(ERR_TYPE, "in: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
        }

    return null(0);
}

obj_t ray_sect(obj_t x, obj_t y)
{
    obj_t mask, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
        mask = ray_in(x, y);
        res = ray_filter(x, mask);
        drop(mask);
        return res;

    default:
        throw(ERR_TYPE, "sect: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }

    return null(0);
}

obj_t ray_except(obj_t x, obj_t y)
{
    i64_t i, j = 0, l;
    obj_t mask, nmask, res;

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

        resize(&res, j);

        return res;
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
        mask = ray_in(x, y);
        nmask = ray_not(mask);
        drop(mask);
        res = ray_filter(x, nmask);
        drop(nmask);
        return res;
    default:
        throw(ERR_TYPE, "except: unsupported types: '%s, '%s", typename(x->type), typename(y->type));
    }
}

// TODO: implement
obj_t ray_union(obj_t x, obj_t y)
{
    obj_t c, res;

    c = ray_concat(x, y);
    res = ray_distinct(c);
    drop(c);
    return res;
}

obj_t ray_first(obj_t x)
{
    if (!x)
        return null(0);
    if (x->type < 0)
        return clone(x);
    if (is_vector(x))
        return at_idx(x, 0);
    if (x->type == TYPE_ANYMAP || x->type == TYPE_VECMAP || x->type == TYPE_LISTMAP)
        return at_idx(x, 0);

    return clone(x);
}

obj_t ray_last(obj_t x)
{
    if (!x)
        return null(0);
    if (x->type < 0)
        return clone(x);
    if (is_vector(x))
        return at_idx(x, -1);
    if (x->type == TYPE_ANYMAP || x->type == TYPE_VECMAP || x->type == TYPE_LISTMAP)
        return at_idx(x, -1);

    return clone(x);
}

obj_t ray_key(obj_t x)
{
    switch (x->type)
    {
    case TYPE_TABLE:
    case TYPE_DICT:
        return clone(as_list(x)[0]);
    case TYPE_ENUM:
        return symbol(enum_key(x));
    case TYPE_ANYMAP:
        return clone(anymap_key(x));
    case TYPE_VECMAP:
    case TYPE_LISTMAP:
        return clone(as_list(x)[0]);
    default:
        return clone(x);
    }
}

obj_t ray_value(obj_t x)
{
    obj_t sym, k, v, res, e;
    i64_t i, l, sl, xl, *ids;
    u8_t *buf;

    switch (x->type)
    {
    case TYPE_ENUM:
        k = ray_key(x);
        sym = at_obj(runtime_get()->env.variables, k);
        drop(k);

        e = enum_val(x);
        xl = e->len;

        if (is_null(sym) || sym->type != TYPE_SYMBOL)
        {
            res = vector_i64(xl);

            for (i = 0; i < xl; i++)
                as_i64(res)[i] = as_i64(e)[i];

            drop(sym);

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

        drop(sym);

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
                    drop(res);
                    return v;
                }

                as_list(res)[i] = v;
            }
            else
            {
                res->len = i;
                drop(res);
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
                drop(v);
                return res;
            }

            as_list(v)[i] = res;
        }
        return table(clone(as_list(x)[0]), v);

    case TYPE_DICT:
        return clone(as_list(x)[1]);

    case TYPE_VECMAP:
        xl = as_list(x)[1]->len;
        if (xl == 0)
            return null(0);

        ids = as_i64(as_list(x)[1]);
        v = at_idx(as_list(x)[0], ids[0]);
        res = v->type < 0 ? vector(v->type, xl) : vector(TYPE_LIST, xl);
        ins_obj(&res, 0, v);

        for (i = 1; i < xl; i++)
        {
            v = at_idx(as_list(x)[0], ids[i]);
            ins_obj(&res, i, v);
        }

        return res;

    case TYPE_LISTMAP:
        xl = as_list(x)[1]->len;
        res = vector(TYPE_LIST, xl);

        for (i = 0; i < xl; i++)
        {
            k = ray_vecmap(as_list(x)[0], as_list(as_list(x)[1])[i]);
            v = ray_value(k);
            drop(k);
            ins_obj(&res, i, v);
        }

        return res;

    default:
        return clone(x);
    }
}

obj_t ray_where(obj_t x)
{
    u64_t i, j, l;
    obj_t res;
    i64_t *cur;

    switch (x->type)
    {
    case TYPE_BOOL:
        l = x->len;
        for (i = 0, j = 0; i < l; i++)
            j += as_bool(x)[i];

        res = vector_i64(j);
        cur = as_i64(res);

        for (i = 0; i < l; i++)
        {
            *cur = i;             // Always assign the value
            cur += as_bool(x)[i]; // Move the pointer only if value is 1
        }

        return res;

    default:
        throw(ERR_TYPE, "where: unsupported type: '%s", typename(x->type));
    }
}