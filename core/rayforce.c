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

CASSERT(sizeof(struct obj_t) == 16, rayforce_h)

obj_t atom(type_t type)
{
    obj_t a = (obj_t)heap_malloc(sizeof(struct obj_t));

    a->type = -type;
    a->rc = 1;

    return a;
}

obj_t null()
{
    return NULL;
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

obj_t shrink(obj_t *obj, u64_t len)
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

    return obj;
}

obj_t join_obj(obj_t *obj, obj_t val)
{
    obj_t lst = NULL;
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

    return join_raw(obj, val);
}

obj_t join_sym(obj_t *obj, str_t str)
{
    i64_t sym = intern_symbol(str, strlen(str));
    return join_raw(obj, sym);
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

bool_t obj_t_eq(obj_t a, obj_t b)
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

/*
 * Increment the reference count of an obj_t
 */
obj_t __attribute__((hot)) clone(obj_t obj)
{
    if (obj == NULL)
        return NULL;

    u64_t i, l;
    u16_t slaves = runtime_get()->slaves;

    if (slaves)
        __atomic_fetch_add(&((obj)->rc), 1, __ATOMIC_RELAXED);
    else
        (obj)->rc += 1;

    switch (obj->type)
    {
    case -TYPE_BOOL:
    case -TYPE_I64:
    case -TYPE_F64:
    case -TYPE_SYMBOL:
    case -TYPE_TIMESTAMP:
    case -TYPE_CHAR:
    case TYPE_BOOL:
    case TYPE_I64:
    case TYPE_F64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_CHAR:
    case TYPE_LAMBDA:
    case TYPE_UNARY:
    case TYPE_BINARY:
    case TYPE_VARY:
    case TYPE_ERROR:
        return obj;
    case TYPE_LIST:
        l = obj->len;
        for (i = 0; i < l; i++)
            clone(as_list(obj)[i]);
        return obj;
    case TYPE_DICT:
    case TYPE_TABLE:
        clone(as_list(obj)[0]);
        clone(as_list(obj)[1]);
        return obj;
    default:
        panic(str_fmt(0, "clone: invalid type: %d", obj->type));
    }
}

/*
 * Free an obj_t
 */
nil_t __attribute__((hot)) drop(obj_t obj)
{
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
    case -TYPE_BOOL:
    case -TYPE_I64:
    case -TYPE_F64:
    case -TYPE_SYMBOL:
    case -TYPE_TIMESTAMP:
    case -TYPE_CHAR:
    case TYPE_BOOL:
    case TYPE_I64:
    case TYPE_F64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_CHAR:
    case TYPE_UNARY:
    case TYPE_BINARY:
    case TYPE_VARY:
        if (rc == 0)
            heap_free(obj);
        return;
    case TYPE_LIST:
        l = obj->len;
        for (i = 0; i < l; i++)
            drop(as_list(obj)[i]);
        if (rc == 0)
            heap_free(obj);
        return;
    case TYPE_TABLE:
    case TYPE_DICT:
        drop(as_list(obj)[0]);
        drop(as_list(obj)[1]);
        if (rc == 0)
            heap_free(obj);
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
        panic(str_fmt(0, "drop: invalid type: %d", obj->type));
    }
}

/*
 * Copy on write rf_
 */
obj_t cow(obj_t obj)
{
    i64_t i, l;
    obj_t new = NULL;

    // TODO: implement copy on write
    return obj;

    // if (obj_t_rc(obj) == 1)
    //     return clone(obj);

    // switch (obj->type)
    // {
    // case TYPE_BOOL:
    //     new = vector_bool(obj->len);
    //     new.adt->attrs = obj->attrs;
    //     memcpy(as_vector_bool(&new), as_vector_bool(obj), obj->len);
    //     return new;
    // case TYPE_I64:
    //     new = vector_i64(obj->len);
    //     new.adt->attrs = obj->attrs;
    //     memcpy(as_vector_i64(&new), as_vector_i64(obj), obj->len * sizeof(i64_t));
    //     return new;
    // case TYPE_F64:
    //     new = vector_f64(obj->len);
    //     new.adt->attrs = obj->attrs;
    //     memcpy(as_vector_f64(&new), as_vector_f64(obj), obj->len * sizeof(f64_t));
    //     return new;
    // case TYPE_SYMBOL:
    //     new = vector_symbol(obj->len);
    //     new.adt->attrs = obj->attrs;
    //     memcpy(as_vector_symbol(&new), as_vector_symbol(obj), obj->len * sizeof(i64_t));
    //     return new;
    // case TYPE_TIMESTAMP:
    //     new = vector_timestamp(obj->len);
    //     new.adt->attrs = obj->attrs;
    //     memcpy(as_vector_timestamp(&new), as_vector_timestamp(obj), obj->len * sizeof(i64_t));
    //     return new;
    // case TYPE_CHAR:
    //     new = string(obj->len);
    //     new.adt->attrs = obj->attrs;
    //     memcpy(as_string(&new), as_string(obj), obj->len);
    //     return new;
    // case TYPE_LIST:
    //     l = obj->len;
    //     new = list(l);
    //     new.adt->attrs = obj->attrs;
    //     for (i = 0; i < l; i++)
    //         as_list(&new)[i] = cow(&as_list(obj)[i]);
    //     return new;
    // case TYPE_DICT:
    //     as_list(obj)[0] = cow(&as_list(obj)[0]);
    //     as_list(obj)[1] = cow(&as_list(obj)[1]);
    //     new.adt->attrs = obj->attrs;
    //     return new;
    // case TYPE_TABLE:
    //     new = table(cow(&as_list(obj)[0]), cow(&as_list(obj)[1]));
    //     new.adt->attrs = obj->attrs;
    //     return new;
    // case TYPE_LAMBDA:
    //     return *obj;
    // case TYPE_ERROR:
    //     return *obj;
    // default:
    //     panic(str_fmt(0, "cow: invalid type: %d", obj->type));
    // }
}

/*
 * Get the reference count of an rf_
 */
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