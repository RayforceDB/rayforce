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
#include "alloc.h"
#include "string.h"
#include "vector.h"
#include "dict.h"
#include "util.h"
#include "string.h"
#include "runtime.h"

CASSERT(sizeof(struct rf_object_t) == 32, rayforce_h)

rf_object atom(type_t type)
{
    rf_object a = (rf_object)rf_malloc(sizeof(struct rf_object_t));

    a->type = -type;
    a->rc = 1;

    return a;
}

rf_object list(i64_t len, ...)
{
    i32_t i;
    rf_object l = (rf_object)rf_malloc(sizeof(struct rf_object_t));

    l->type = TYPE_LIST;
    l->rc = 1;
    l->ptr = rf_malloc(sizeof(rf_object) * len);

    va_list args;
    va_start(args, len);

    for (i = 0; i < len; i++)
        l->ptr[i] = va_arg(args, rf_object);

    va_end(args);

    return l;
}

rf_object error(i8_t code, str_t message)
{
    rf_object err = rf_malloc(sizeof(struct rf_object_t));

    err->type = TYPE_ERROR;
    err->len = strlen(message);
    err->ptr = message;

    return err;
}

rf_object null()
{
    return NULL;
}

rf_object bool(bool_t val)
{
    rf_object b = atom(TYPE_BOOL);
    b->bool = val;
    return b;
}

rf_object i64(i64_t val)
{
    rf_object i = atom(TYPE_I64);
    i->i64 = val;
    return i;
}

rf_object f64(f64_t val)
{
    rf_object f = atom(TYPE_F64);
    f->f64 = val;
    return f;
}

rf_object symbol(str_t s)
{
    i64_t id = intern_symbol(s, strlen(s));
    rf_object a = atom(TYPE_SYMBOL);
    a->i64 = id;

    return a;
}

rf_object symboli64(i64_t id)
{
    rf_object a = atom(TYPE_SYMBOL);
    a->i64 = id;

    return a;
}

rf_object guid(u8_t data[])
{

    // if (data == NULL)
    //     return guid;

    // guid_t *g = (guid_t *)rf_malloc(sizeof(struct guid_t));
    // memcpy(g->data, data, sizeof(guid_t));

    // guid.guid = g;

    // return guid;

    return NULL;
}

rf_object schar(char_t c)
{
    rf_object s = atom(TYPE_CHAR);
    s->schar = c;
    return s;
}

rf_object timestamp(i64_t val)
{
    rf_object t = atom(TYPE_TIMESTAMP);
    t->i64 = val;
    return t;
}

rf_object table(rf_object keys, rf_object vals)
{
    rf_object t = list(2, keys, vals);
    t->type = TYPE_TABLE;

    return t;
}

bool_t is_null(rf_object object)
{
    return (object->type == TYPE_LIST && object->ptr == NULL) ||
           (object->type == -TYPE_I64 && object->i64 == NULL_I64) ||
           (object->type == -TYPE_SYMBOL && object->i64 == NULL_I64) ||
           (object->type == -TYPE_F64 && object->f64 == NULL_F64) ||
           (object->type == -TYPE_TIMESTAMP && object->i64 == NULL_I64) ||
           (object->type == -TYPE_CHAR && object->schar == '\0');
}

bool_t rf_object_eq(rf_object a, rf_object b)
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
        if (as_I64(a) == as_I64(b))
            return true;
        if (a->len != b->len)
            return false;

        l = a->len;
        for (i = 0; i < l; i++)
        {
            if (as_I64(a)[i] != as_I64(b)[i])
                return false;
        }
        return 1;
    }
    else if (a->type == TYPE_F64)
    {
        if (as_F64(a) == as_F64(b))
            return 1;
        if (a->len != b->len)
            return false;

        l = a->len;
        for (i = 0; i < l; i++)
        {
            if (as_F64(a)[i] != as_F64(b)[i])
                return false;
        }
        return true;
    }

    return false;
}

/*
 * Increment the reference count of an rf_object
 */
rf_object __attribute__((hot)) clone(rf_object object)
{
    i64_t i, l;
    u16_t slaves = runtime_get()->slaves;

    if (slaves)
        __atomic_fetch_add(&((object)->rc), 1, __ATOMIC_RELAXED);
    else
        (object)->rc += 1;

    switch (object->type)
    {
    case TYPE_BOOL:
    case TYPE_I64:
    case TYPE_F64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_CHAR:
        return object;
    case TYPE_LIST:
        l = object->len;
        for (i = 0; i < l; i++)
            clone(&as_list(object)[i]);
        return object;
    case TYPE_DICT:
    case TYPE_TABLE:
        clone(&as_list(object)[0]);
        clone(&as_list(object)[1]);
        return object;
    case TYPE_LAMBDA:
        return object;
    case TYPE_ERROR:
        return object;
    default:
        panic(str_fmt(0, "clone: invalid type: %d", object->type));
    }
}

/*
 * Free an rf_object
 */
null_t __attribute__((hot)) drop(rf_object object)
{
    i64_t i, rc, l;
    u16_t slaves = runtime_get()->slaves;

    if (slaves)
        rc = __atomic_sub_fetch(&((object)->rc), 1, __ATOMIC_RELAXED);
    else
    {
        (object)->rc -= 1;
        rc = (object)->rc;
    }

    switch (object->type)
    {
    case TYPE_BOOL:
    case TYPE_I64:
    case TYPE_F64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_CHAR:
        if (rc == 0)
            vector_free(object);
        return;
    case TYPE_LIST:
        l = object->len;
        for (i = 0; i < l; i++)
            drop(&as_list(object)[i]);
        if (rc == 0)
            vector_free(object);
        return;
    case TYPE_TABLE:
    case TYPE_DICT:
        drop(&as_list(object)[0]);
        drop(&as_list(object)[1]);
        if (rc == 0)
            rf_free(object->ptr);
        return;
    case TYPE_LAMBDA:
        if (rc == 0)
        {
            drop(&as_lambda(object)->constants);
            drop(&as_lambda(object)->args);
            drop(&as_lambda(object)->locals);
            drop(&as_lambda(object)->code);
            debuginfo_free(&as_lambda(object)->debuginfo);
            rf_free(object->ptr);
        }
        return;
    case TYPE_ERROR:
        if (rc == 0)
            rf_free(object->ptr);
        return;
    default:
        panic(str_fmt(0, "free: invalid type: %d", object->type));
    }
}

/*
 * Copy on write rf_object_t
 */
rf_object cow(rf_object object)
{
    i64_t i, l;
    rf_object new = NULL;

    // TODO: implement copy on write
    return object;

    // if (rf_object_rc(object) == 1)
    //     return clone(object);

    // switch (object->type)
    // {
    // case TYPE_BOOL:
    //     new = Bool(object->adt->len);
    //     new.adt->attrs = object->adt->attrs;
    //     memcpy(as_Bool(&new), as_Bool(object), object->adt->len);
    //     return new;
    // case TYPE_I64:
    //     new = I64(object->adt->len);
    //     new.adt->attrs = object->adt->attrs;
    //     memcpy(as_I64(&new), as_I64(object), object->adt->len * sizeof(i64_t));
    //     return new;
    // case TYPE_F64:
    //     new = F64(object->adt->len);
    //     new.adt->attrs = object->adt->attrs;
    //     memcpy(as_F64(&new), as_F64(object), object->adt->len * sizeof(f64_t));
    //     return new;
    // case TYPE_SYMBOL:
    //     new = Symbol(object->adt->len);
    //     new.adt->attrs = object->adt->attrs;
    //     memcpy(as_Symbol(&new), as_Symbol(object), object->adt->len * sizeof(i64_t));
    //     return new;
    // case TYPE_TIMESTAMP:
    //     new = Timestamp(object->adt->len);
    //     new.adt->attrs = object->adt->attrs;
    //     memcpy(as_Timestamp(&new), as_Timestamp(object), object->adt->len * sizeof(i64_t));
    //     return new;
    // case TYPE_CHAR:
    //     new = string(object->adt->len);
    //     new.adt->attrs = object->adt->attrs;
    //     memcpy(as_string(&new), as_string(object), object->adt->len);
    //     return new;
    // case TYPE_LIST:
    //     l = object->adt->len;
    //     new = list(l);
    //     new.adt->attrs = object->adt->attrs;
    //     for (i = 0; i < l; i++)
    //         as_list(&new)[i] = cow(&as_list(object)[i]);
    //     return new;
    // case TYPE_DICT:
    //     as_list(object)[0] = cow(&as_list(object)[0]);
    //     as_list(object)[1] = cow(&as_list(object)[1]);
    //     new.adt->attrs = object->adt->attrs;
    //     return new;
    // case TYPE_TABLE:
    //     new = table(cow(&as_list(object)[0]), cow(&as_list(object)[1]));
    //     new.adt->attrs = object->adt->attrs;
    //     return new;
    // case TYPE_LAMBDA:
    //     return *object;
    // case TYPE_ERROR:
    //     return *object;
    // default:
    //     panic(str_fmt(0, "cow: invalid type: %d", object->type));
    // }
}

/*
 * Get the reference count of an rf_object_t
 */
i64_t rc(rf_object object)
{
    i64_t rc;
    u16_t slaves = runtime_get()->slaves;

    if (slaves)
        rc = __atomic_load_n(&((object)->rc), __ATOMIC_RELAXED);
    else
        rc = (object)->rc;

    return rc;
}