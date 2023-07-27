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
#include "format.h"
#include "heap.h"
#include "string.h"
#include "util.h"
#include "string.h"
#include "runtime.h"
#include "ops.h"

CASSERT(sizeof(struct obj_t) == 16, rayforce_h)

obj_t atom(type_t type)
{
    obj_t a = (obj_t)heap_malloc(sizeof(struct obj_t));

    a->type = -type;
    a->rc = 1;
    a->attrs = 0;
    a->len = 1;

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
        return schar('\0');
    case TYPE_SYMBOL:
        return symboli64(NULL_I64);
    case TYPE_TIMESTAMP:
        return timestamp(NULL_I64);
    default:
        return NULL;
    }
}

obj_t bool(bool_t val)
{
    obj_t b = atom(TYPE_BOOL);
    b->bool = val;
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

obj_t guid(u8_t data[])
{

    // if (data == NULL)
    //     return guid;

    // guid_t *g = (guid_t *)heap_malloc(sizeof(struct guid_t));
    // memcpy(g->data, data, sizeof(guid_t));

    // guid.guid = g;

    // return guid;

    return NULL;
}

obj_t schar(char_t c)
{
    obj_t s = atom(TYPE_CHAR);
    s->schar = c;
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
    obj_t vec = (obj_t)heap_malloc(sizeof(struct obj_t) + len * size_of(type));

    vec->type = type;
    vec->rc = 1;
    vec->len = len;
    vec->attrs = 0;

    return vec;
}

obj_t string(u64_t len)
{
    obj_t string = vector(TYPE_CHAR, len + 1);
    as_string(string)[len] = '\0';
    string->len = len;

    return string;
}

obj_t list(u64_t len, ...)
{
    i32_t i;
    obj_t l = (obj_t)heap_malloc(sizeof(struct obj_t) + sizeof(obj_t) * len);
    va_list args;

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
    obj_t dict;

    if (!is_vector(keys) || !is_vector(vals))
        return error(ERR_TYPE, "Keys and Values must be lists");

    if (keys->len != vals->len)
        return error(ERR_LENGTH, "Keys and Values must have the same length");

    dict = list(2, keys, vals);
    dict->type = TYPE_DICT;

    return dict;
}

obj_t table(obj_t keys, obj_t vals)
{
    obj_t t = list(2, keys, vals);
    t->type = TYPE_TABLE;

    return t;
}

obj_t error(i8_t code, str_t msg)
{
    obj_t obj = list(3, i64(code), string_from_str(msg, strlen(msg)), NULL);
    obj->type = TYPE_ERROR;

    return obj;
}

obj_t resize(obj_t *obj, u64_t len)
{
    debug_assert(is_vector(*obj));

    if ((*obj)->len == len)
        return *obj;

    // calculate size of vector with new length
    i64_t new_size = sizeof(struct obj_t) + len * size_of((*obj)->type);

    *obj = heap_realloc(*obj, new_size);
    (*obj)->len = len;

    return *obj;
}

obj_t join_raw(obj_t *obj, nil_t *val)
{
    i64_t off, occup, req;
    i32_t size = size_of((*obj)->type);

    off = (*obj)->len * size;
    occup = sizeof(struct obj_t) + off;
    req = occup + size;
    *obj = heap_realloc(*obj, req);
    memcpy((*obj)->arr + off, &val, size);
    (*obj)->len++;

    return *obj;
}

obj_t join_obj(obj_t *obj, obj_t val)
{
    obj_t res, lst = NULL;
    u64_t i, l;

    // change vector type to a list
    // if (obj->type && obj->type != -val->type)
    // {
    //     l = obj->len;
    //     lst = list(l + 1);

    //     for (i = 0; i < l; i++)
    //         as_list(lst)[i] = vector_get(vec, i);

    //     as_list(&lst)[index] = value;

    //     drop(vec);

    //     *vec = lst;

    //     return lst;
    // }

    switch (mtype2((*obj)->type, val->type))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        res = join_raw(obj, val->i64);
        drop(val);
        return res;
    case mtype2(TYPE_F64, -TYPE_F64):
        res = join_raw(obj, *(nil_t **)&val->f64);
        drop(val);
        return res;
    case mtype2(TYPE_CHAR, -TYPE_CHAR):
        res = join_raw(obj, *(nil_t **)&val->schar);
        drop(val);
        return res;
    default:
        if ((*obj)->type == TYPE_LIST)
        {
            res = join_raw(obj, val);
            return res;
        }

        raise(ERR_TYPE, "join: invalid types: %d, %d", (*obj)->type, val->type);
    }

    return join_raw(obj, val);
}

obj_t join_sym(obj_t *obj, str_t str)
{
    i64_t sym = intern_symbol(str, strlen(str));
    return join_raw(obj, sym);
}

obj_t write_raw(obj_t *obj, u64_t idx, nil_t *val)
{
    i32_t size = size_of((*obj)->type);
    memcpy((*obj)->arr + idx * size, &val, size);
    return obj;
}

obj_t write_obj(obj_t *obj, u64_t idx, obj_t val)
{
    obj_t ret;

    if (*obj == NULL || !is_vector(*obj))
        return *obj;

    switch ((*obj)->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        ret = write_raw(obj, idx, val->i64);
        drop(val);
        break;
    case TYPE_F64:
        ret = write_raw(obj, idx, *(nil_t **)&val->f64);
        drop(val);
        break;
    case TYPE_CHAR:
        ret = write_raw(obj, idx, *(nil_t **)&val->schar);
        drop(val);
        break;
    case TYPE_LIST:
        ret = write_raw(obj, idx, val);
        break;
    default:
        panic(str_fmt(0, "write obj: invalid type: %d", (*obj)->type));
    }

    return ret;
}

obj_t write_sym(obj_t *obj, u64_t idx, str_t str)
{
    i64_t sym = intern_symbol(str, strlen(str));
    return write_raw(obj, idx, sym);
}

obj_t at_idx(obj_t obj, u64_t idx)
{
    if (obj == NULL || !is_vector(obj))
        return null(0);

    switch (obj->type)
    {
    case TYPE_I64:
        if (idx < obj->len)
            return i64(as_vector_i64(obj)[idx]);
        return i64(NULL_I64);
    case TYPE_SYMBOL:
        if (idx < obj->len)
            return symboli64(as_vector_symbol(obj)[idx]);
        return symboli64(NULL_I64);
    case TYPE_TIMESTAMP:
        if (idx < obj->len)
            return timestamp(as_vector_timestamp(obj)[idx]);
        return timestamp(NULL_I64);
    case TYPE_F64:
        if (idx < obj->len)
            return f64(as_vector_f64(obj)[idx]);
        return f64(NULL_F64);
    case TYPE_CHAR:
        if (idx < obj->len)
            return schar(as_string(obj)[idx]);
        return schar('\0');
    case TYPE_LIST:
        if (idx < obj->len)
            return clone(as_list(obj)[idx]);
        return null(0);
    default:
        panic(str_fmt(0, "at_idx: invalid type: %d", obj->type));
    }
}

obj_t at_obj(obj_t obj, obj_t idx)
{
    u64_t i;

    if (obj == NULL)
        return null(0);

    switch (mtype2(obj->type, idx->type))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
    case mtype2(TYPE_F64, -TYPE_I64):
    case mtype2(TYPE_CHAR, -TYPE_I64):
    case mtype2(TYPE_LIST, -TYPE_I64):
        return at_idx(obj, idx->i64);
    default:
        if (obj->type == TYPE_DICT)
        {
            i = find_obj(as_list(obj)[0], idx);
            if (i == as_list(obj)[0]->len)
                return null(as_list(obj)[1]->type);

            return at_idx(as_list(obj)[1], i);
        }

        panic(str_fmt(0, "at_obj: invalid type: %d", obj->type));
    }
}

obj_t set_idx(obj_t *obj, u64_t idx, obj_t val)
{
    switch (mtype2((*obj)->type, val->type))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
        as_vector_i64(*obj)[idx] = val->i64;
        drop(val);
        return *obj;
    case mtype2(TYPE_F64, -TYPE_F64):
        as_vector_f64(*obj)[idx] = val->f64;
        drop(val);
        return *obj;
    case mtype2(TYPE_CHAR, -TYPE_CHAR):
        as_string(*obj)[idx] = val->schar;
        drop(val);
        return *obj;
    default:
        if ((*obj)->type == TYPE_LIST)
        {
            drop(as_list(*obj)[idx]);
            as_list(*obj)[idx] = val;
            return *obj;
        }

        raise(ERR_TYPE, "set_idx: invalid types: %d, %d", (*obj)->type, val->type);
    }
}

obj_t set_obj(obj_t *obj, obj_t idx, obj_t val)
{
    obj_t res;
    u64_t i;

    switch (mtype2((*obj)->type, idx->type))
    {
    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_I64):
    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
    case mtype2(TYPE_F64, -TYPE_I64):
    case mtype2(TYPE_CHAR, -TYPE_I64):
    case mtype2(TYPE_LIST, -TYPE_I64):
        return set_idx(obj, idx->i64, val);
    default:
        if ((*obj)->type == TYPE_DICT)
        {
            i = find_obj(as_list(*obj)[0], idx);
            if (i == as_list(*obj)[0]->len)
            {
                res = join_obj(&as_list(*obj)[0], clone(idx));
                if (res->type == TYPE_ERROR)
                    return res;

                res = join_obj(&as_list(*obj)[1], val);

                if (res->type == TYPE_ERROR)
                    panic("set_obj: inconsistent update");

                return *obj;
            }

            set_idx(&as_list(*obj)[1], i, val);

            return *obj;
        }

        raise(ERR_TYPE, "set_obj: invalid types: %d, %d", (*obj)->type, val->type);
    }
}

bool_t is_null(obj_t obj)
{
    return (obj == NULL) ||
           (obj->type == -TYPE_I64 && obj->i64 == NULL_I64) ||
           (obj->type == -TYPE_SYMBOL && obj->i64 == NULL_I64) ||
           (obj->type == -TYPE_F64 && obj->f64 == NULL_F64) ||
           (obj->type == -TYPE_TIMESTAMP && obj->i64 == NULL_I64) ||
           (obj->type == -TYPE_CHAR && obj->schar == '\0');
}

bool_t equal(obj_t a, obj_t b)
{
    i64_t i, l;

    if (a == b)
        return true;

    if (a->type != b->type)
        return 0;

    if (a->type == -TYPE_I64 || a->type == -TYPE_SYMBOL || a->type == TYPE_UNARY || a->type == TYPE_BINARY || a->type == TYPE_VARY)
        return a->i64 == b->i64;
    else if (a->type == -TYPE_F64)
        return a->f64 == b->f64;
    else if (a->type == TYPE_I64 || a->type == TYPE_SYMBOL)
    {
        if (as_vector_i64(a) == as_vector_i64(b))
            return true;
        if (a->len != b->len)
            return false;

        l = a->len;
        for (i = 0; i < l; i++)
        {
            if (as_vector_i64(a)[i] != as_vector_i64(b)[i])
                return false;
        }
        return 1;
    }
    else if (a->type == TYPE_F64)
    {
        if (as_vector_f64(a) == as_vector_f64(b))
            return 1;
        if (a->len != b->len)
            return false;

        l = a->len;
        for (i = 0; i < l; i++)
        {
            if (as_vector_f64(a)[i] != as_vector_f64(b)[i])
                return false;
        }
        return true;
    }

    return false;
}

i64_t find_raw(obj_t obj, nil_t *val)
{
    i64_t i, l;

    if (obj == NULL || !is_vector(obj))
        return NULL_I64;

    switch (obj->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        l = obj->len;
        for (i = 0; i < l; i++)
            if (as_vector_i64(obj)[i] == val)
                return i;
        return l;
    case TYPE_F64:
        l = obj->len;
        for (i = 0; i < l; i++)
            if (as_vector_f64(obj)[i] == *(f64_t *)&val)
                return i;
        return l;
    case TYPE_CHAR:
        l = obj->len;
        for (i = 0; i < l; i++)
            if (as_string(obj)[i] == *(char_t *)&val)
                return i;
        return l;
    case TYPE_LIST:
        l = obj->len;
        for (i = 0; i < l; i++)
            if (equal(as_list(obj)[i], val))
                return i;
        return l;
    default:
        panic(str_fmt(0, "find: invalid type: %d", obj->type));
    }
}

i64_t find_obj(obj_t obj, obj_t val)
{
    i64_t i, l;

    if (obj == NULL || !is_vector(obj))
        return NULL_I64;

    switch (obj->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        return find_raw(obj, val->i64);
    case TYPE_F64:
        return find_raw(obj, *(nil_t **)&val->f64);
    case TYPE_CHAR:
        return find_raw(obj, *(nil_t **)&val->schar);
    case TYPE_LIST:
        return find_raw(obj, val);
    default:
        panic(str_fmt(0, "find: invalid type: %d", obj->type));
    }
}

i64_t find_sym(obj_t obj, str_t str)
{
    i64_t n = intern_symbol(str, strlen(str));
    return find_raw(obj, n);
}

obj_t cast(type_t type, obj_t obj)
{
    obj_t res, err;
    u64_t i, l;
    str_t s, msg;

    // Do nothing if the type is the same
    if (type == obj->type)
        return clone(obj);

    if (type == TYPE_CHAR)
    {
        s = obj_fmt(obj);
        if (s == NULL)
            panic("obj_fmt() returned NULL");
        res = string_from_str(s, strlen(s));
        heap_free(s);
        return res;
    }

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
            as_vector_f64(res)[i] = (f64_t)as_vector_i64(obj)[i];
        return res;
    case mtype2(TYPE_I64, TYPE_F64):
        l = obj->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(res)[i] = (i64_t)as_vector_f64(obj)[i];
        return res;
    case mtype2(TYPE_BOOL, TYPE_I64):
        l = obj->len;
        res = vector_bool(l);
        for (i = 0; i < l; i++)
            as_vector_bool(res)[i] = (bool_t)as_vector_i64(obj)[i];
        return res;
        // case mtype2(-TYPE_GUID, TYPE_CHAR):
        //     res = guid(NULL);

        //     if (strlen(as_string(obj)) != 36)
        //         break;

        //     res->guid = heap_malloc(sizeof(guid_t));

        //     i = sscanf(as_string(y),
        //                "%02hhx%02hhx%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
        //                &res.guid->data[0], &res.guid->data[1], &res.guid->data[2], &res.guid->data[3],
        //                &res.guid->data[4], &res.guid->data[5],
        //                &res.guid->data[6], &res.guid->data[7],
        //                &res.guid->data[8], &res.guid->data[9],
        //                &res.guid->data[10], &res.guid->data[11], &res.guid->data[12],
        //                &res.guid->data[13], &res.guid->data[14], &res.guid->data[15]);

        //     if (i != 16)
        //     {
        //         heap_free(res.guid);
        //         res = guid(NULL);
        //     }

        //     break;

    case mtype2(TYPE_I64, TYPE_TIMESTAMP):
        l = obj->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(res)[i] = as_vector_timestamp(obj)[i];
        return res;
    case mtype2(TYPE_TIMESTAMP, TYPE_I64):
        l = obj->len;
        res = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_vector_timestamp(res)[i] = as_vector_i64(obj)[i];
        return res;
    default:
        msg = str_fmt(0, "invalid conversion from '%s' to '%s'",
                      symbols_get(env_get_typename_by_type(&runtime_get()->env, obj->type)),
                      symbols_get(env_get_typename_by_type(&runtime_get()->env, type)));
        err = error(ERR_TYPE, msg);
        heap_free(msg);
        return err;
    }

    return res;
}

obj_t __attribute__((hot)) clone(obj_t obj)
{
    debug_assert(is_valid(obj));

    if (obj == NULL)
        return NULL;

    u64_t i, l;
    u16_t slaves = runtime_get()->slaves;

    if (slaves)
        __atomic_fetch_add(&((obj)->rc), 1, __ATOMIC_RELAXED);
    else
        (obj)->rc += 1;

    return obj;
}

nil_t __attribute__((hot)) drop(obj_t obj)
{
    debug_assert(is_valid(obj));

    if (obj == NULL)
        return;

    u32_t rc;
    u64_t i, l;
    u16_t slaves = runtime_get()->slaves;

    if (slaves)
        rc = __atomic_sub_fetch(&((obj)->rc), 1, __ATOMIC_RELAXED);
    else
    {
        (obj)->rc -= 1;
        rc = (obj)->rc;
    }

    switch (obj->type)
    {
    case TYPE_LIST:
        if (rc == 0)
        {
            l = obj->len;
            for (i = 0; i < l; i++)
                drop(as_list(obj)[i]);
            heap_free(obj);
        }
        return;
    case TYPE_TABLE:
    case TYPE_DICT:
        if (rc == 0)
        {
            drop(as_list(obj)[0]);
            drop(as_list(obj)[1]);
            heap_free(obj);
        }
        return;
    case TYPE_LAMBDA:
        if (rc == 0)
        {
            drop(as_lambda(obj)->constants);
            drop(as_lambda(obj)->args);
            drop(as_lambda(obj)->locals);
            drop(as_lambda(obj)->code);
            nfo_free(&as_lambda(obj)->nfo);
            heap_free(obj);
        }
        return;
    case TYPE_ERROR:
        if (rc == 0)
        {
            drop(as_list(obj)[0]);
            drop(as_list(obj)[1]);
            heap_free(obj);
        }
        return;
    default:
        if (rc == 0)
            heap_free(obj);
    }
}

obj_t cow(obj_t obj)
{
    i64_t i, l;
    obj_t new = NULL;

    // TODO: implement copy on write
    return obj;
}

u32_t rc(obj_t obj)
{
    u32_t rc;
    u16_t slaves = runtime_get()->slaves;

    if (slaves)
        rc = __atomic_load_n(&((obj)->rc), __ATOMIC_RELAXED);
    else
        rc = (obj)->rc;

    return rc;
}