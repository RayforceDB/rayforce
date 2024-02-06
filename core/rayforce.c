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

#include <stdio.h>
#include <stdatomic.h>
#include "rayforce.h"
#include "mmap.h"
#include "format.h"
#include "heap.h"
#include "string.h"
#include "util.h"
#include "binary.h"
#include "unary.h"
#include "string.h"
#include "runtime.h"
#include "items.h"
#include "ops.h"
#include "fs.h"
#include "serde.h"
#include "sock.h"
#include "eval.h"
#include "lambda.h"
#include "error.h"
#include "filter.h"

CASSERT(sizeof(struct obj_t) == 16, rayforce_h)

// Synchronization flag (use atomics on rc operations).
static i32_t __RC_SYNC = 0;

u8_t version()
{
    return RAYFORCE_VERSION;
}

nil_t zero_obj(obj_t obj)
{
    u64_t size = size_of(obj) - sizeof(struct obj_t);
    memset(obj->arr, 0, size);
}

obj_t atom(type_t type)
{
    obj_t a = (obj_t)heap_alloc(sizeof(struct obj_t));

    if (!a)
        panic("oom");

    a->mmod = MMOD_INTERNAL;
    a->type = -type;
    a->rc = 1;
    a->attrs = 0;

    return a;
}

obj_t null(type_t type)
{
    switch (type)
    {
    case TYPE_BOOL:
        return bool(false);
    case TYPE_I64:
        return i64(NULL_I64);
    case TYPE_F64:
        return f64(NULL_F64);
    case TYPE_CHAR:
        return vchar('\0');
    case TYPE_SYMBOL:
        return symboli64(NULL_I64);
    case TYPE_TIMESTAMP:
        return timestamp(NULL_I64);
    default:
        return NULL_OBJ;
    }
}

obj_t bool(bool_t val)
{
    obj_t b = atom(TYPE_BOOL);
    b->bool = val;
    return b;
}

obj_t u8(u8_t val)
{
    obj_t b = atom(TYPE_BYTE);
    b->u8 = val;
    return b;
}

obj_t i64(i64_t val)
{
    obj_t i = atom(TYPE_I64);
    i->i64 = val;
    return i;
}

obj_t f64(f64_t val)
{
    obj_t f = atom(TYPE_F64);
    f->f64 = val;
    return f;
}

obj_t symbol(str_t s)
{
    i64_t id = intern_symbol(s, strlen(s));
    obj_t a = atom(TYPE_SYMBOL);
    a->i64 = id;

    return a;
}

obj_t symboli64(i64_t id)
{
    obj_t a = atom(TYPE_SYMBOL);
    a->i64 = id;

    return a;
}

obj_t guid(u8_t buf[16])
{
    obj_t guid = vector(TYPE_I64, 2);
    guid->type = -TYPE_GUID;

    if (buf == NULL)
        memset(as_guid(guid), 0, 16);
    else
        memcpy(as_guid(guid)[0].buf, buf, 16);

    return guid;
}

obj_t vchar(char_t c)
{
    obj_t s = atom(TYPE_CHAR);
    s->vchar = c;

    return s;
}

obj_t timestamp(i64_t val)
{
    obj_t t = atom(TYPE_TIMESTAMP);
    t->i64 = val;

    return t;
}

obj_t vector(type_t type, u64_t len)
{
    type_t t;
    obj_t vec;

    if (type < 0)
        t = -type;
    else if (type == TYPE_CHAR)
    {
        len = (len > 0) ? len + 1 : len;
        t = TYPE_CHAR;
    }
    else if (type > 0 && type < TYPE_ENUM)
        t = type;
    else if (type == TYPE_ENUM)
        t = TYPE_SYMBOL;
    else
        t = TYPE_LIST;

    vec = (obj_t)heap_alloc(sizeof(struct obj_t) + len * size_of_type(t));

    if (!vec)
        panic("oom");

    vec->mmod = MMOD_INTERNAL;
    vec->refc = 1;
    vec->type = t;
    vec->rc = 1;
    vec->len = len;
    vec->attrs = 0;

    if ((type == TYPE_CHAR) && (len > 0))
        as_string(vec)[len - 1] = '\0';

    return vec;
}

obj_t vn_symbol(u64_t len, ...)
{
    u64_t i;
    i64_t sym, *syms;
    obj_t res;
    str_t s;
    va_list args;

    res = vector_symbol(len);
    syms = as_symbol(res);

    va_start(args, len);

    for (i = 0; i < len; i++)
    {
        s = va_arg(args, str_t);
        sym = intern_symbol(s, strlen(s));
        syms[i] = sym;
    }

    va_end(args);

    return res;
}

obj_t string(u64_t len)
{
    return vector(TYPE_CHAR, len);
}

obj_t list(u64_t len)
{
    return vector(TYPE_LIST, len);
}

obj_t vn_list(u64_t len, ...)
{
    u64_t i;
    obj_t l;
    va_list args;

    l = (obj_t)heap_alloc(sizeof(struct obj_t) + sizeof(obj_t) * len);

    l->mmod = MMOD_INTERNAL;
    l->refc = 1;
    l->type = TYPE_LIST;
    l->rc = 1;
    l->len = len;
    l->attrs = 0;

    va_start(args, len);

    for (i = 0; i < len; i++)
        as_list(l)[i] = va_arg(args, obj_t);

    va_end(args);

    return l;
}

obj_t dict(obj_t keys, obj_t vals)
{
    obj_t d;

    d = vn_list(2, keys, vals);
    d->type = TYPE_DICT;

    return d;
}

obj_t table(obj_t keys, obj_t vals)
{
    obj_t t;

    t = vn_list(2, keys, vals);
    t->type = TYPE_TABLE;

    return t;
}

obj_t venum(obj_t sym, obj_t vec)
{
    obj_t e;

    e = vn_list(2, sym, vec);
    e->type = TYPE_ENUM;

    return e;
}

obj_t anymap(obj_t sym, obj_t vec)
{
    obj_t e;

    e = vn_list(2, sym, vec);
    e->type = TYPE_ANYMAP;

    return e;
}

obj_t resize(obj_t *obj, u64_t len)
{
    debug_assert(is_vector(*obj), "resize: invalid type: %d", (*obj)->type);

    if ((*obj)->len == len)
        return *obj;

    // calculate size of vector with new length
    i64_t new_size = sizeof(struct obj_t) + len * size_of_type((*obj)->type);

    *obj = heap_realloc(*obj, new_size);
    (*obj)->len = len;

    return *obj;
}

obj_t push_raw(obj_t *obj, raw_t val)
{
    i64_t off, occup, req;
    i32_t size = size_of_type((*obj)->type);

    off = (*obj)->len * size;
    occup = sizeof(struct obj_t) + off;
    req = occup + size;
    *obj = heap_realloc(*obj, req);
    memcpy((*obj)->arr + off, val, size);
    (*obj)->len++;

    return *obj;
}

obj_t push_obj(obj_t *obj, obj_t val)
{
    obj_t res, lst = NULL;
    u64_t i, l, size1, size2;
    type_t t = val ? val->type : TYPE_LIST;

    // change vector type to a list
    if ((*obj)->type != -t && (*obj)->type != TYPE_LIST)
    {
        l = (*obj)->len;
        lst = vn_list(l + 1);

        for (i = 0; i < l; i++)
            as_list(lst)[i] = at_idx(*obj, i);

        as_list(lst)[i] = val;

        drop(*obj);

        *obj = lst;

        return lst;
    }

    switch (mtype2((*obj)->type, t))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        res = push_raw(obj, &val->i64);
        drop(val);
        return res;
    case mtype2(TYPE_F64, -TYPE_F64):
        res = push_raw(obj, &val->f64);
        drop(val);
        return res;
    case mtype2(TYPE_CHAR, -TYPE_CHAR):
        res = push_raw(obj, &val->vchar);
        drop(val);
        return res;
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        size1 = size_of(*obj) - sizeof(struct obj_t);
        size2 = size_of(val) - sizeof(struct obj_t);
        res = resize(obj, (*obj)->len + val->len);
        memcpy((*obj)->arr + size1, as_i64(val), size2);
        return res;
    default:
        if ((*obj)->type == TYPE_LIST)
        {
            res = push_raw(obj, &val);
            return res;
        }

        throw(ERR_TYPE, "push_obj: invalid types: '%s, '%s", typename((*obj)->type), typename(val->type));
    }
}

obj_t push_sym(obj_t *obj, str_t str)
{
    i64_t sym = intern_symbol(str, strlen(str));
    return push_raw(obj, &sym);
}

obj_t ins_raw(obj_t *obj, i64_t idx, raw_t val)
{
    i32_t size = size_of_type((*obj)->type);
    memcpy((*obj)->arr + idx * size, val, size);
    return *obj;
}

obj_t ins_obj(obj_t *obj, i64_t idx, obj_t val)
{
    i64_t i;
    u64_t l;
    obj_t ret;

    if (!is_vector(*obj))
        return *obj;

    if (!val)
        val = null((*obj)->type);

    // we need to convert vector to list
    if ((*obj)->type - -val->type != 0 && (*obj)->type != TYPE_LIST)
    {
        l = ops_count(*obj);
        ret = list(l);

        for (i = 0; i < idx; i++)
            as_list(ret)[i] = at_idx(*obj, i);

        as_list(ret)[idx] = val;

        drop(*obj);

        *obj = ret;

        return ret;
    }

    switch ((*obj)->type)
    {
    case TYPE_BOOL:
        ret = ins_raw(obj, idx, &val->bool);
        drop(val);
        break;
    case TYPE_BYTE:
        ret = ins_raw(obj, idx, &val->u8);
        drop(val);
        break;
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        ret = ins_raw(obj, idx, &val->i64);
        drop(val);
        break;
    case TYPE_F64:
        ret = ins_raw(obj, idx, &val->f64);
        drop(val);
        break;
    case TYPE_CHAR:
        ret = ins_raw(obj, idx, &val->vchar);
        drop(val);
        break;
    case TYPE_LIST:
        ret = ins_raw(obj, idx, &val);
        break;
    default:
        throw(ERR_TYPE, "write_obj: invalid type: '%s", typename((*obj)->type));
    }

    return ret;
}

obj_t ins_sym(obj_t *obj, i64_t idx, str_t str)
{
    i64_t sym = intern_symbol(str, strlen(str));
    return ins_raw(obj, idx, &sym);
}

obj_t at_idx(obj_t obj, i64_t idx)
{
    u64_t i, n, l;
    obj_t k, v, res;
    u8_t *buf;

    switch (obj->type)
    {
    case TYPE_I64:
        if (idx < 0)
            idx = obj->len + idx + 1;
        if (idx >= 0 && idx < (i64_t)obj->len)
            return i64(as_i64(obj)[idx]);
        return i64(NULL_I64);
    case TYPE_SYMBOL:
        if (idx < 0)
            idx = obj->len + idx + 1;
        if (idx >= 0 && idx < (i64_t)obj->len)
            return symboli64(as_symbol(obj)[idx]);
        return symboli64(NULL_I64);
    case TYPE_TIMESTAMP:
        if (idx < 0)
            idx = obj->len + idx + 1;
        if (idx >= 0 && idx < (i64_t)obj->len)
            return timestamp(as_timestamp(obj)[idx]);
        return timestamp(NULL_I64);
    case TYPE_F64:
        if (idx < 0)
            idx = obj->len + idx + 1;
        if (idx >= 0 && idx < (i64_t)obj->len)
            return f64(as_f64(obj)[idx]);
        return f64(NULL_F64);
    case TYPE_CHAR:
        if (idx < 0)
            idx = obj->len + idx + 1;
        if (idx >= 0 && idx < (i64_t)obj->len)
            return vchar(as_string(obj)[idx]);
        return vchar('\0');
    case TYPE_LIST:
        if (idx < 0)
            idx = obj->len + idx + 1;
        if (idx >= 0 && idx < (i64_t)obj->len)
            return clone(as_list(obj)[idx]);
        return NULL_OBJ;
    case TYPE_GUID:
        if (idx < 0)
            idx = obj->len + idx + 1;
        if (idx >= 0 && idx < (i64_t)obj->len)
            return guid(as_guid(obj)[idx].buf);
        return NULL_OBJ;
    case TYPE_ENUM:
        if (idx < 0)
            idx = obj->len + idx + 1;
        if (idx >= 0 && idx < (i64_t)enum_val(obj)->len)
        {
            k = ray_key(obj);
            if (is_error(k))
                return k;
            v = ray_get(k);
            drop(k);
            if (is_error(v))
                return v;
            idx = as_i64(enum_val(obj))[idx];
            res = at_idx(v, idx);
            drop(v);
            return res;
        }
        return symboli64(NULL_I64);

    case TYPE_ANYMAP:
        k = anymap_key(obj);
        v = anymap_val(obj);
        if (idx < 0)
            idx = obj->len + idx + 1;
        if (idx >= 0 && idx < (i64_t)v->len)
        {
            buf = as_u8(k) + as_i64(v)[idx];
            return load_obj(&buf, k->len);
        }

        return NULL_OBJ;

    case TYPE_TABLE:
        n = as_list(obj)[0]->len;
        v = list(n);
        l = ops_count(obj);
        if (idx < 0)
            idx = obj->len + idx + 1;
        if (idx >= 0 && idx < (i64_t)l)
        {
            for (i = 0; i < n; i++)
            {
                k = at_idx(as_list(as_list(obj)[1])[i], idx);
                if (is_error(k))
                {
                    v->len = i;
                    drop(v);
                    return k;
                }
                ins_obj(&v, i, k);
            }
            return dict(clone(as_list(obj)[0]), v);
        }

        for (i = 0; i < n; i++)
        {
            k = null(as_list(as_list(obj)[1])[i]->type);
            ins_obj(&v, i, k);
        }

        return dict(clone(as_list(obj)[0]), v);

    default:
        return clone(obj);
    }
}

obj_t at_ids(obj_t obj, i64_t ids[], u64_t len)
{
    u64_t i, xl;
    i64_t *iinp, *iout;
    f64_t *finp, *fout;
    obj_t k, v, cols, res;

    switch (obj->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        res = vector(obj->type, len);
        iinp = as_i64(obj);
        iout = as_i64(res);
        for (i = 0; i < len; i++)
            iout[i] = iinp[ids[i]];

        return res;
    case TYPE_F64:
        res = vector_f64(len);
        finp = as_f64(obj);
        fout = as_f64(res);
        for (i = 0; i < len; i++)
            fout[i] = finp[ids[i]];

        return res;
    case TYPE_GUID:
        res = vector(TYPE_GUID, len);
        for (i = 0; i < len; i++)
            as_guid(res)[i] = as_guid(obj)[ids[i]];

        return res;
    case TYPE_LIST:
        res = list(len);
        for (i = 0; i < len; i++)
            as_list(res)[i] = clone(as_list(obj)[ids[i]]);

        return res;
    case TYPE_ENUM:
        k = ray_key(obj);
        if (is_error(k))
            return k;

        v = ray_get(k);
        drop(k);

        if (is_error(v))
            return v;

        if (v->type != TYPE_SYMBOL)
            return error(ERR_TYPE, "enum: '%s' is not a 'Symbol'", typename(v->type));

        res = vector_symbol(len);
        for (i = 0; i < len; i++)
            as_i64(res)[i] = as_i64(v)[as_i64(enum_val(obj))[ids[i]]];

        drop(v);
        return res;
    case TYPE_TABLE:
        xl = as_list(obj)[0]->len;
        cols = list(xl);
        for (i = 0; i < xl; i++)
        {
            k = at_ids(as_list(as_list(obj)[1])[i], ids, len);

            // if (is_atom(c))
            //     c = ray_enlist(&c, 1);

            if (is_error(k))
            {
                cols->len = i;
                drop(cols);
                return k;
            }

            ins_obj(&cols, i, k);
        }

        return table(clone(as_list(obj)[0]), cols);
    default:
        res = vector(TYPE_LIST, len);
        for (i = 0; i < len; i++)
            ins_obj(&res, i, at_idx(obj, ids[i]));

        return res;
    }
}

obj_t at_obj(obj_t obj, obj_t idx)
{
    u64_t i, j, n, l;
    i64_t *ids;
    obj_t v;

    switch (mtype2(obj->type, idx->type))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
    case mtype2(TYPE_F64, -TYPE_I64):
    case mtype2(TYPE_CHAR, -TYPE_I64):
    case mtype2(TYPE_LIST, -TYPE_I64):
    case mtype2(TYPE_ENUM, -TYPE_I64):
    case mtype2(TYPE_ANYMAP, -TYPE_I64):
    case mtype2(TYPE_TABLE, -TYPE_I64):
        return at_idx(obj, idx->i64);
    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        j = find_raw(as_list(obj)[0], &idx->i64);
        if (j == as_list(obj)[0]->len)
            return null(as_list(obj)[1]->type);
        return at_idx(as_list(obj)[1], j);
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, TYPE_I64):
    case mtype2(TYPE_F64, TYPE_I64):
    case mtype2(TYPE_GUID, TYPE_I64):
    case mtype2(TYPE_LIST, TYPE_I64):
    case mtype2(TYPE_ENUM, TYPE_I64):
    case mtype2(TYPE_TABLE, TYPE_I64):
        ids = as_i64(idx);
        n = idx->len;
        l = ops_count(obj);
        for (i = 0; i < n; i++)
            if (ids[i] < 0 || ids[i] >= (i64_t)l)
                throw(ERR_TYPE, "at_obj: '%lld' is out of range '0..%lld'", ids[i], l - 1);
        return at_ids(obj, as_i64(idx), idx->len);
    case mtype2(TYPE_TABLE, TYPE_SYMBOL):
        l = ops_count(idx);
        v = list(l);

        for (i = 0; i < l; i++)
        {
            j = find_raw(as_list(obj)[0], &as_symbol(idx)[i]);
            if (j == as_list(obj)[0]->len)
                as_list(v)[i] = null(0);
            else
                as_list(v)[i] = at_idx(as_list(obj)[1], j);
        }
        return v;
    default:
        if (obj->type == TYPE_DICT)
        {
            i = find_obj(as_list(obj)[0], idx);
            if (i == as_list(obj)[0]->len)
                return null(as_list(obj)[1]->type);

            return at_idx(as_list(obj)[1], i);
        }

        throw(ERR_TYPE, "at_obj: invalid type: '%s", typename(obj->type));
    }
}

obj_t set_idx(obj_t *obj, i64_t idx, obj_t val)
{
    switch (mtype2((*obj)->type, val->type))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
        as_i64(*obj)[idx] = val->i64;
        drop(val);
        return *obj;
    case mtype2(TYPE_F64, -TYPE_F64):
        as_f64(*obj)[idx] = val->f64;
        drop(val);
        return *obj;
    case mtype2(TYPE_CHAR, -TYPE_CHAR):
        as_string(*obj)[idx] = val->vchar;
        drop(val);
        return *obj;
    default:
        if ((*obj)->type == TYPE_LIST)
        {
            drop(as_list(*obj)[idx]);
            as_list(*obj)[idx] = val;
            return *obj;
        }

        throw(ERR_TYPE, "set_idx: invalid types: '%s, '%s", typename((*obj)->type), typename(val->type));
    }
}

obj_t set_ids(obj_t *obj, i64_t ids[], u64_t len, obj_t vals)
{
    u64_t i;

    switch (mtype2((*obj)->type, vals->type))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
        for (i = 0; i < len; i++)
            as_i64(*obj)[ids[i]] = vals->i64;
        drop(vals);
        return *obj;
    case mtype2(TYPE_F64, -TYPE_F64):
        for (i = 0; i < len; i++)
            as_f64(*obj)[ids[i]] = vals->f64;
        drop(vals);
        return *obj;
    case mtype2(TYPE_CHAR, -TYPE_CHAR):
        for (i = 0; i < len; i++)
            as_string(*obj)[ids[i]] = vals->vchar;
        drop(vals);
        return *obj;
    case mtype2(TYPE_GUID, -TYPE_GUID):
        for (i = 0; i < len; i++)
            as_guid(*obj)[ids[i]] = as_guid(vals)[i];
        drop(vals);
        return *obj;
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, TYPE_I64):
        for (i = 0; i < len; i++)
            as_i64(*obj)[ids[i]] = as_i64(vals)[i];
        drop(vals);
        return *obj;
    case mtype2(TYPE_F64, TYPE_F64):
        for (i = 0; i < len; i++)
            as_f64(*obj)[ids[i]] = as_f64(vals)[i];
        drop(vals);
        return *obj;
    case mtype2(TYPE_CHAR, TYPE_CHAR):
        for (i = 0; i < len; i++)
            as_string(*obj)[ids[i]] = as_string(vals)[i];
        drop(vals);
        return *obj;
    case mtype2(TYPE_GUID, TYPE_GUID):
        for (i = 0; i < len; i++)
            as_guid(*obj)[ids[i]] = as_guid(vals)[i];
        drop(vals);
        return *obj;
    default:
        if ((*obj)->type == TYPE_LIST)
        {
            for (i = 0; i < len; i++)
            {
                drop(as_list(*obj)[ids[i]]);
                as_list(*obj)[ids[i]] = at_idx(vals, i);
            }
            drop(vals);
            return *obj;
        }

        throw(ERR_TYPE, "set_ids: types mismatch/unsupported: '%s, '%s", typename((*obj)->type), typename(vals->type));
    }
}

obj_t __expand(obj_t obj, u64_t len)
{
    u64_t i;
    obj_t res;

    switch (obj->type)
    {
    case -TYPE_BOOL:
    case -TYPE_BYTE:
    case -TYPE_CHAR:
        res = vector(obj->type, len);
        for (i = 0; i < len; i++)
            as_u8(res)[i] = obj->u8;

        drop(obj);

        return res;
    case -TYPE_I64:
    case -TYPE_SYMBOL:
    case -TYPE_TIMESTAMP:
        res = vector(obj->type, len);
        for (i = 0; i < len; i++)
            as_i64(res)[i] = obj->i64;

        drop(obj);

        return res;
    case -TYPE_F64:
        res = vector_f64(len);
        for (i = 0; i < len; i++)
            as_f64(res)[i] = obj->f64;

        drop(obj);

        return res;
    case -TYPE_GUID:
        res = vector(TYPE_GUID, len);
        for (i = 0; i < len; i++)
            memcpy(&as_guid(res)[i], as_guid(obj)->buf, sizeof(struct guid_t));

        drop(obj);

        return res;
    default:
        if (ops_count(obj) != len)
        {
            drop(obj);
            throw(ERR_LENGTH, "set: invalid length: '%lld' != '%lld'", ops_count(obj), len);
        }

        return obj;
    }
}

obj_t set_obj(obj_t *obj, obj_t idx, obj_t val)
{
    obj_t k, v, res;
    u64_t i, n, l;
    i64_t id = NULL_I64, *ids = NULL;

    // dispatch:
    switch (mtype2((*obj)->type, idx->type))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
    case mtype2(TYPE_F64, -TYPE_I64):
    case mtype2(TYPE_CHAR, -TYPE_I64):
    case mtype2(TYPE_LIST, -TYPE_I64):
    case mtype2(TYPE_GUID, -TYPE_I64):
        if (idx->i64 < 0 || idx->i64 >= (i64_t)(*obj)->len)
        {
            drop(val);
            throw(ERR_TYPE, "set_obj: '%lld' is out of range '0..%lld'", idx->i64, (*obj)->len - 1);
        }
        return set_idx(obj, idx->i64, val);
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, TYPE_I64):
    case mtype2(TYPE_F64, TYPE_I64):
    case mtype2(TYPE_CHAR, TYPE_I64):
    case mtype2(TYPE_GUID, TYPE_I64):
    case mtype2(TYPE_LIST, TYPE_I64):
        if (is_vector(val) && idx->len != val->len)
        {
            drop(val);
            throw(ERR_LENGTH, "set_obj: idx and vals length mismatch: '%lld' != '%lld'", idx->len, val->len);
        }
        ids = as_i64(idx);
        n = idx->len;
        l = ops_count(*obj);
        for (i = 0; i < n; i++)
        {
            if (ids[i] < 0 || ids[i] >= (i64_t)l)
            {
                drop(val);
                throw(ERR_TYPE, "set_obj: '%lld' is out of range '0..%lld'", ids[i], l - 1);
            }
        }
        return set_ids(obj, ids, n, val);
    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        val = __expand(val, ops_count(*obj));
        if (is_error(val))
            return val;
        i = find_obj(as_list(*obj)[0], idx);
        if (i == as_list(*obj)[0]->len)
        {
            res = push_obj(&as_list(*obj)[0], clone(idx));
            if (is_error(res))
                return res;

            res = push_obj(&as_list(*obj)[1], val);
            if (is_error(res))
                panic("set_obj: inconsistent update");

            return *obj;
        }

        set_idx(&as_list(*obj)[1], i, val);

        return *obj;
    case mtype2(TYPE_TABLE, TYPE_SYMBOL):
        if (val->type != TYPE_LIST)
        {
            drop(val);
            throw(ERR_TYPE, "set_obj: 'Table indexed via vector expects 'List in a values, found: '%s", typename(val->type));
        }

        l = ops_count(idx);
        if (l != ops_count(val))
        {
            drop(val);
            throw(ERR_LENGTH, "set_obj: idx and vals length mismatch: '%lld' != '%lld'", ops_count(*obj), ops_count(val));
        }

        n = ops_count(*obj);
        v = list(l);
        for (i = 0; i < l; i++)
        {
            k = __expand(clone(as_list(val)[i]), n);

            if (is_error(k))
            {
                v->len = i;
                drop(v);
                drop(val);
                return k;
            }

            as_list(v)[i] = k;
        }

        drop(val);
        val = v;

        for (i = 0; i < l; i++)
        {
            id = find_raw(as_list(*obj)[0], &as_symbol(idx)[i]);
            if (id == (i64_t)as_list(*obj)[0]->len)
            {
                push_raw(&as_list(*obj)[0], &as_symbol(idx)[i]);
                push_obj(&as_list(*obj)[1], clone(as_list(val)[i]));
            }
            else
            {
                set_idx(&as_list(*obj)[1], id, clone(as_list(val)[i]));
            }
        }

        drop(val);

        return *obj;
    default:
        if ((*obj)->type == TYPE_DICT)
        {
            i = find_obj(as_list(*obj)[0], idx);
            if (i == as_list(*obj)[0]->len)
            {
                res = push_obj(&as_list(*obj)[0], clone(idx));
                if (res->type == TYPE_ERROR)
                    return res;

                res = push_obj(&as_list(*obj)[1], val);

                if (res->type == TYPE_ERROR)
                    panic("set_obj: inconsistent update");

                return *obj;
            }

            set_idx(&as_list(*obj)[1], i, val);

            return *obj;
        }

        throw(ERR_TYPE, "set_obj: invalid types: '%s, '%s", typename((*obj)->type), typename(val->type));
    }
}

obj_t pop_obj(obj_t *obj)
{
    if ((*obj)->len == 0)
        return NULL_OBJ;

    switch ((*obj)->type)
    {
    case TYPE_I64:
        return i64(as_i64(*obj)[--(*obj)->len]);
    case TYPE_SYMBOL:
        return symboli64(as_symbol(*obj)[--(*obj)->len]);
    case TYPE_TIMESTAMP:
        return timestamp(as_timestamp(*obj)[--(*obj)->len]);
    case TYPE_F64:
        return f64(as_f64(*obj)[--(*obj)->len]);
    case TYPE_CHAR:
        return vchar(as_string(*obj)[--(*obj)->len]);
    case TYPE_LIST:
        return as_list(*obj)[--(*obj)->len];

    default:
        panic("pop_obj: invalid type: %d", (*obj)->type);
    }
}

obj_t remove_idx(obj_t *obj, i64_t idx)
{
    unused(idx);

    return *obj;
}

obj_t remove_obj(obj_t *obj, obj_t idx)
{
    unused(idx);

    return *obj;
}

bool_t is_null(obj_t obj)
{
    return (obj->type == TYPE_NULL) ||
           (obj->type == -TYPE_I64 && obj->i64 == NULL_I64) ||
           (obj->type == -TYPE_SYMBOL && obj->i64 == NULL_I64) ||
           (obj->type == -TYPE_F64 && obj->f64 == NULL_F64) ||
           (obj->type == -TYPE_TIMESTAMP && obj->i64 == NULL_I64) ||
           (obj->type == -TYPE_CHAR && obj->vchar == '\0');
}

i32_t objcmp(obj_t a, obj_t b)
{
    i64_t i, l, d;

    if (a == b)
        return 0;

    if (a->type != b->type)
        return a->type - b->type;

    switch (a->type)
    {
    case -TYPE_BOOL:
        return a->bool - b->bool;
    case -TYPE_BYTE:
    case -TYPE_CHAR:
        return (i32_t)a->u8 - (i32_t)b->u8;
    case -TYPE_I64:
    case -TYPE_SYMBOL:
    case -TYPE_TIMESTAMP:
    case TYPE_UNARY:
    case TYPE_BINARY:
    case TYPE_VARY:
        return a->i64 - b->i64;
    case -TYPE_F64:
        return a->f64 - b->f64;
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        if (a->len != b->len)
            return a->len - b->len;

        l = a->len;
        for (i = 0; i < l; i++)
        {
            d = as_i64(a)[i] - as_i64(b)[i];
            if (d != 0)
                return d;
        }
        return 0;
    case TYPE_F64:
        if (a->len != b->len)
            return a->len - b->len;

        l = a->len;
        for (i = 0; i < l; i++)
        {
            d = as_f64(a)[i] - as_f64(b)[i];
            if (d != 0)
                return d;
        }

        return 0;
    case TYPE_CHAR:
        if (a->len != b->len)
            return a->len - b->len;

        l = a->len;
        for (i = 0; i < l; i++)
        {
            d = as_string(a)[i] - as_string(b)[i];
            if (d != 0)
                return d;
        }

        return 0;

    case TYPE_LIST:
        if (a->len != b->len)
            return a->len - b->len;

        l = a->len;
        for (i = 0; i < l; i++)
        {
            d = objcmp(as_list(a)[i], as_list(b)[i]);
            if (d != 0)
                return d;
        }

        return 0;

    default:
        return -1;
    }
}

i64_t find_raw(obj_t obj, raw_t val)
{
    i64_t i, l;

    if (!is_vector(obj))
        return 1;

    switch (obj->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        l = obj->len;
        for (i = 0; i < l; i++)
            if (as_i64(obj)[i] == *(i64_t *)val)
                return i;
        return l;
    case TYPE_F64:
        l = obj->len;
        for (i = 0; i < l; i++)
            if (as_f64(obj)[i] == *(f64_t *)val)
                return i;
        return l;
    case TYPE_CHAR:
        l = obj->len;
        for (i = 0; i < l; i++)
            if (as_string(obj)[i] == *(char_t *)val)
                return i;
        return l;
    case TYPE_LIST:
        l = obj->len;
        for (i = 0; i < l; i++)
            if (objcmp(as_list(obj)[i], *(obj_t *)val) == 0)
                return i;
        return l;
    default:
        panic("find: invalid type: %d", obj->type);
    }
}

i64_t find_obj(obj_t obj, obj_t val)
{
    if (!is_vector(obj))
        return NULL_I64;

    switch (obj->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        return find_raw(obj, &val->i64);
    case TYPE_F64:
        return find_raw(obj, &val->f64);
    case TYPE_CHAR:
        return find_raw(obj, &val->vchar);
    case TYPE_LIST:
        return find_raw(obj, &val);
    default:
        panic("find: invalid type: %d", obj->type);
    }
}

i64_t find_sym(obj_t obj, str_t str)
{
    i64_t n = intern_symbol(str, strlen(str));
    return find_raw(obj, &n);
}

obj_t as(type_t type, obj_t obj)
{
    obj_t res, err;
    u64_t i, l;
    str_t msg;

    // Do nothing if the type is the same
    if (type == obj->type)
        return clone(obj);

    if (type == TYPE_CHAR)
        return obj_stringify(obj);

    switch (mtype2(type, obj->type))
    {
    case mtype2(-TYPE_I64, -TYPE_F64):
        return i64((i64_t)obj->f64);
    case mtype2(-TYPE_F64, -TYPE_I64):
        return f64((f64_t)obj->i64);
    case mtype2(-TYPE_I64, -TYPE_TIMESTAMP):
        return i64(obj->i64);
    case mtype2(-TYPE_TIMESTAMP, -TYPE_I64):
        return timestamp(obj->i64);
    case mtype2(-TYPE_SYMBOL, TYPE_CHAR):
        return symbol(as_string(obj));
    case mtype2(-TYPE_I64, TYPE_CHAR):
        return i64(strtol(as_string(obj), NULL, 10));
    case mtype2(-TYPE_F64, TYPE_CHAR):
        return f64(strtod(as_string(obj), NULL));
    case mtype2(TYPE_TABLE, TYPE_DICT):
        return table(clone(as_list(obj)[0]), clone(as_list(obj)[1]));
    case mtype2(TYPE_DICT, TYPE_TABLE):
        return dict(clone(as_list(obj)[0]), clone(as_list(obj)[1]));
    case mtype2(TYPE_F64, TYPE_I64):
        l = obj->len;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(res)[i] = (f64_t)as_i64(obj)[i];
        return res;
    case mtype2(TYPE_I64, TYPE_F64):
        l = obj->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(res)[i] = (i64_t)as_f64(obj)[i];
        return res;
    case mtype2(TYPE_BOOL, TYPE_I64):
        l = obj->len;
        res = vector_bool(l);
        for (i = 0; i < l; i++)
            as_bool(res)[i] = (bool_t)as_i64(obj)[i];
        return res;
    case mtype2(-TYPE_GUID, TYPE_CHAR):
        res = guid(NULL);

        if (obj->len != 36)
            break;

        i = sscanf(as_string(obj),
                   "%02hhx%02hhx%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
                   &as_guid(res)->buf[0], &as_guid(res)->buf[1], &as_guid(res)->buf[2], &as_guid(res)->buf[3],
                   &as_guid(res)->buf[4], &as_guid(res)->buf[5],
                   &as_guid(res)->buf[6], &as_guid(res)->buf[7],
                   &as_guid(res)->buf[8], &as_guid(res)->buf[9],
                   &as_guid(res)->buf[10], &as_guid(res)->buf[11], &as_guid(res)->buf[12],
                   &as_guid(res)->buf[13], &as_guid(res)->buf[14], &as_guid(res)->buf[15]);

        if (i != 16)
            memset(as_guid(res)->buf, 0, 16);

        return res;
    case mtype2(TYPE_I64, TYPE_TIMESTAMP):
        l = obj->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(res)[i] = as_timestamp(obj)[i];
        return res;
    case mtype2(TYPE_TIMESTAMP, TYPE_I64):
        l = obj->len;
        res = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_timestamp(res)[i] = as_i64(obj)[i];
        return res;
    default:
        msg = str_fmt(0, "invalid conversion from '%s to '%s", typename(obj->type), typename(type));
        err = error_str(ERR_TYPE, msg);
        heap_free(msg);
        return err;
    }

    return res;
}

obj_t __attribute__((hot)) clone(obj_t obj)
{
    debug_assert(is_valid(obj), "invalid object type: %d", obj->type);

    if (!__RC_SYNC)
        (obj)->rc += 1;
    else
        __atomic_fetch_add(&obj->rc, 1, __ATOMIC_RELAXED);

    return obj;
}

nil_t __attribute__((hot)) drop(obj_t obj)
{
    debug_assert(is_valid(obj), "invalid object type: %d", obj->type);

    u32_t rc;
    u64_t i, l;
    obj_t id, k;

    if (!__RC_SYNC)
    {
        (obj)->rc -= 1;
        rc = (obj)->rc;
    }
    else
        rc = __atomic_sub_fetch(&obj->rc, 1, __ATOMIC_RELAXED);

    if (rc)
        return;

    switch (obj->type)
    {
    case TYPE_LIST:
    case TYPE_FILTERMAP:
    case TYPE_GROUPMAP:
        l = obj->len;
        for (i = 0; i < l; i++)
            drop(as_list(obj)[i]);

        if (is_external_simple(obj))
            mmap_free(obj, size_of(obj));
        else
            heap_free(obj);
        return;
    case TYPE_ENUM:
        if (is_external_compound(obj))
            mmap_free((str_t)obj - PAGE_SIZE, size_of(obj) + PAGE_SIZE);
        else
        {
            drop(as_list(obj)[0]);
            drop(as_list(obj)[1]);
            heap_free(obj);
        }
        return;
    case TYPE_ANYMAP:
        mmap_free(anymap_key(obj), size_of(obj));
        mmap_free((str_t)obj - PAGE_SIZE, size_of(obj) + PAGE_SIZE);
        return;
    case TYPE_TABLE:
    case TYPE_DICT:
        drop(as_list(obj)[0]);
        drop(as_list(obj)[1]);
        heap_free(obj);
        return;
    case TYPE_LAMBDA:
        drop(as_lambda(obj)->name);
        drop(as_lambda(obj)->args);
        drop(as_lambda(obj)->body);
        drop(as_lambda(obj)->nfo);
        heap_free(obj);
        return;
    case TYPE_NULL:
        return;
    case TYPE_ERROR:
        drop(as_error(obj)->msg);
        drop(as_error(obj)->locs);
        heap_free(obj);
        return;
    default:
        if (is_external_simple(obj))
        {
            mmap_free(obj, size_of(obj));
            id = i64((i64_t)obj);
            k = at_obj(runtime_get()->fds, id);
            if (!is_null(k))
            {
                fs_fclose(k->i64);
                drop(k);
                set_obj(&runtime_get()->fds, id, null(TYPE_I64));
            }

            drop(id);
        }
        else
            heap_free(obj);
    }
}

obj_t cow(obj_t obj)
{
    u32_t rc;
    u64_t i, l, size;
    obj_t id, fd, res;

    if (!is_internal(obj))
    {
        id = i64((i64_t)obj);
        fd = at_obj(runtime_get()->fds, id);
        if (is_null(fd))
        {
            printf("cow: invalid fd");
            exit(1);
        }

        res = mmap_file(fd->i64, size_of(obj));
        res->rc = 2;
        drop(id);
        id = i64((i64_t)res);

        // insert fd into runtime fds
        set_obj(&runtime_get()->fds, id, fd);

        drop(id);

        return res;
    }

    /*
    Since it is forbidden to modify globals from several threads simultenously,
    we can just check for rc == 1 and if it is the case, we can just return the object
    */
    if (!__RC_SYNC)
        rc = (obj)->rc;
    else
        rc = __atomic_load_n(&obj->rc, __ATOMIC_RELAXED);

    // we only owns the reference, so we can freely modify it
    if (rc == 1)
        return obj;

    // we don't own the reference, so we need to copy object
    switch (obj->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_F64:
    case TYPE_CHAR:
    case TYPE_GUID:
        size = size_of(obj);
        res = heap_alloc(size);
        memcpy(res, obj, size);
        res->rc = 1;
        return res;
    case TYPE_LIST:
        l = obj->len;
        res = list(l);
        res->rc = 1;
        for (i = 0; i < l; i++)
            as_list(res)[i] = clone(as_list(obj)[i]);
        return res;
    case TYPE_TABLE:
        return table(clone(as_list(obj)[0]), clone(as_list(obj)[1]));
    case TYPE_DICT:
        return dict(clone(as_list(obj)[0]), clone(as_list(obj)[1]));
    default:
        throw(ERR_NOT_IMPLEMENTED, "cow: not implemented for type: '%s", typename(obj->type));
    }
}

u32_t rc(obj_t obj)
{
    u32_t rc;

    if (!__RC_SYNC)
        rc = (obj)->rc;
    else
        rc = __atomic_load_n(&obj->rc, __ATOMIC_RELAXED);

    return rc;
}

str_t typename(type_t type)
{
    return symtostr(env_get_typename_by_type(&runtime_get()->env, type));
}
