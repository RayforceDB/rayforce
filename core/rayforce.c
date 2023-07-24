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
#include "vector.h"
#include "dict.h"
#include "util.h"
#include "string.h"
#include "runtime.h"

CASSERT(sizeof(struct obj_t) == 32, rayforce_h)

obj_t atom(type_t type)
{
    obj_t a = (obj_t)heap_malloc(sizeof(struct obj_t));

    a->type = -type;
    a->rc = 1;

    return a;
}

obj_t list(i64_t len, ...)
{
    i32_t i;
    obj_t l = (obj_t)heap_malloc(sizeof(struct obj_t));

    l->type = TYPE_LIST;
    l->rc = 1;
    l->ptr = heap_malloc(sizeof(obj_t) * len);

    va_list args;
    va_start(args, len);

    for (i = 0; i < len; i++)
        l->ptr[i] = va_arg(args, obj_t);

    va_end(args);

    return l;
}

obj_t error(i8_t code, str_t message)
{
    obj_t err = heap_malloc(sizeof(struct obj_t));

    err->type = TYPE_ERROR;
    err->len = strlen(message);
    err->ptr = message;

    return err;
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

obj_t table(obj_t keys, obj_t vals)
{
    obj_t t = list(2, keys, vals);
    t->type = TYPE_TABLE;

    return t;
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

bool_t is_null(obj_t obj)
{
    return (obj == NULL) ||
           (obj->type == -TYPE_I64 && obj->i64 == NULL_vector_i64) ||
           (obj->type == -TYPE_SYMBOL && obj->i64 == NULL_vector_i64) ||
           (obj->type == -TYPE_F64 && obj->f64 == NULL_vector_f64) ||
           (obj->type == -TYPE_TIMESTAMP && obj->i64 == NULL_vector_i64) ||
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

    i64_t i, l;
    u16_t slaves = runtime_get()->slaves;

    if (slaves)
        __atomic_fetch_add(&((obj)->rc), 1, __ATOMIC_RELAXED);
    else
        (obj)->rc += 1;

    switch (obj->type)
    {
    case TYPE_BOOL:
    case TYPE_I64:
    case TYPE_F64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_CHAR:
        return obj;
    case TYPE_LIST:
        l = obj->len;
        for (i = 0; i < l; i++)
            clone(&as_list(obj)[i]);
        return obj;
    case TYPE_DICT:
    case TYPE_TABLE:
        clone(&as_list(obj)[0]);
        clone(&as_list(obj)[1]);
        return obj;
    case TYPE_LAMBDA:
        return obj;
    case TYPE_ERROR:
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

    i64_t i, rc, l;
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
    case TYPE_BOOL:
    case TYPE_I64:
    case TYPE_F64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_CHAR:
        if (rc == 0)
            heap_free(obj->ptr);
        return;
    case TYPE_LIST:
        l = obj->len;
        for (i = 0; i < l; i++)
            drop(as_list(obj)[i]);
        if (rc == 0)
            heap_free(obj->ptr);
        return;
    case TYPE_TABLE:
    case TYPE_DICT:
        drop(as_list(obj)[0]);
        drop(as_list(obj)[1]);
        if (rc == 0)
            heap_free(obj->ptr);
        return;
    case TYPE_LAMBDA:
        if (rc == 0)
        {
            drop(as_lambda(obj)->constants);
            drop(as_lambda(obj)->args);
            drop(as_lambda(obj)->locals);
            drop(as_lambda(obj)->code);
            debuginfo_free(&as_lambda(obj)->debuginfo);
            heap_free(obj->ptr);
        }
        return;
    case TYPE_ERROR:
        if (rc == 0)
            heap_free(obj->ptr);
        return;
    default:
        panic(str_fmt(0, "free: invalid type: %d", obj->type));
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
i64_t rc(obj_t obj)
{
    i64_t rc;
    u16_t slaves = runtime_get()->slaves;

    if (slaves)
        rc = __atomic_load_n(&((obj)->rc), __ATOMIC_RELAXED);
    else
        rc = (obj)->rc;

    return rc;
}