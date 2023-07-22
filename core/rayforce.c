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

// clang-format off
CASSERT(sizeof(struct span_t     ) == 8,  debuginfo_h)
CASSERT(sizeof(struct header_t   ) == 32, rayforce_h )
CASSERT(sizeof(struct guid_t     ) == 16, rayforce_h )
CASSERT(sizeof(struct rf_object_t) == 16, rayforce_h )
// clang-format on

/*
 * Increment reference counter of the object
 */
#define rc_inc(object)                                                         \
    {                                                                          \
        if ((object)->type > 0)                                                \
        {                                                                      \
            u16_t slaves = runtime_get()->slaves;                              \
            if (slaves)                                                        \
                __atomic_fetch_add(&((object)->adt->rc), 1, __ATOMIC_RELAXED); \
            else                                                               \
                (object)->adt->rc += 1;                                        \
        }                                                                      \
    }

/*
 * Decrement reference counter of the object
 */
#define rc_dec(r, object)                                                          \
    {                                                                              \
        if ((object)->type > 0)                                                    \
        {                                                                          \
            u16_t slaves = runtime_get()->slaves;                                  \
            if (slaves)                                                            \
                r = __atomic_sub_fetch(&((object)->adt->rc), 1, __ATOMIC_RELAXED); \
            else                                                                   \
            {                                                                      \
                (object)->adt->rc -= 1;                                            \
                r = (object)->adt->rc;                                             \
            }                                                                      \
        }                                                                          \
    }

/*
 * Get reference counter of the object
 */
#define rc_get(r, object)                                                \
    {                                                                    \
        u16_t slaves = runtime_get()->slaves;                            \
        if (slaves)                                                      \
            r = __atomic_load_n(&((object)->adt->rc), __ATOMIC_RELAXED); \
        else                                                             \
            r = (object)->adt->rc;                                       \
    }

#define typeshift(t) (t - TYPE_BOOL)

rf_object_t error(i8_t code, str_t message)
{
    rf_object_t err = string_from_str(message, strlen(message));
    err.type = TYPE_ERROR;
    err.adt->code = code;
    return err;
}

rf_object_t null()
{
    rf_object_t obj = {
        .type = TYPE_NULL,
    };

    return obj;
}

rf_object_t bool(bool_t val)
{
    rf_object_t scalar = {
        .type = -TYPE_BOOL,
        .i64 = val,
    };

    return scalar;
}

rf_object_t i64(i64_t val)
{
    rf_object_t scalar = {
        .type = -TYPE_I64,
        .i64 = val,
    };

    return scalar;
}

rf_object_t f64(f64_t val)
{
    rf_object_t scalar = {
        .type = -TYPE_F64,
        .f64 = val,
    };

    return scalar;
}

rf_object_t symbol(str_t s)
{
    i64_t id = intern_symbol(s, strlen(s));
    rf_object_t sym = {
        .type = -TYPE_SYMBOL,
        .i64 = id,
    };

    return sym;
}

rf_object_t symboli64(i64_t id)
{
    rf_object_t sym = {
        .type = -TYPE_SYMBOL,
        .i64 = id,
    };

    return sym;
}

rf_object_t guid(u8_t data[])
{
    rf_object_t guid = {
        .type = -TYPE_GUID,
        .guid = NULL,
    };

    if (data == NULL)
        return guid;

    guid_t *g = (guid_t *)rf_malloc(sizeof(struct guid_t));
    memcpy(g->data, data, sizeof(guid_t));

    guid.guid = g;

    return guid;
}

rf_object_t schar(char_t c)
{
    rf_object_t ch = {
        .type = -TYPE_CHAR,
        .schar = c,
    };

    return ch;
}

rf_object_t timestamp(i64_t val)
{
    rf_object_t scalar = {
        .type = -TYPE_TIMESTAMP,
        .i64 = val,
    };

    return scalar;
}

rf_object_t table(rf_object_t keys, rf_object_t vals)
{
    rf_object_t t = list(2);

    as_list(&t)[0] = keys;
    as_list(&t)[1] = vals;

    t.type = TYPE_TABLE;

    return t;
}

bool_t is_null(rf_object_t *object)
{
    return (object->type == TYPE_NULL) ||
           (object->type == -TYPE_I64 && object->i64 == NULL_I64) ||
           (object->type == -TYPE_SYMBOL && object->i64 == NULL_I64) ||
           (object->type == -TYPE_F64 && object->f64 == NULL_F64) ||
           (object->type == -TYPE_TIMESTAMP && object->i64 == NULL_I64) ||
           (object->type == -TYPE_CHAR && object->schar == '\0');
}

bool_t rf_object_eq(rf_object_t *a, rf_object_t *b)
{
    i64_t i, l;

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
        if (a->adt->len != b->adt->len)
            return false;

        l = a->adt->len;
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
        if (a->adt->len != b->adt->len)
            return false;

        l = a->adt->len;
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
 * Increment the reference count of an rf_object_t
 */
rf_object_t __attribute__((hot)) rf_object_clone(rf_object_t *object)
{
    i64_t i, l;

    if (object->type == -TYPE_GUID)
    {
        if (object->guid == NULL)
            return guid(NULL);

        return guid(object->guid->data);
    }

    if (!is_rc(object))
        return *object;

    rc_inc(object);

    switch (object->type)
    {
    case TYPE_BOOL:
    case TYPE_I64:
    case TYPE_F64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_CHAR:
        return *object;
    case TYPE_LIST:
        l = object->adt->len;
        for (i = 0; i < l; i++)
            rf_object_clone(&as_list(object)[i]);
        return *object;
    case TYPE_DICT:
    case TYPE_TABLE:
        rf_object_clone(&as_list(object)[0]);
        rf_object_clone(&as_list(object)[1]);
        return *object;
    case TYPE_LAMBDA:
        return *object;
    case TYPE_ERROR:
        return *object;
    default:
        panic(str_fmt(0, "clone: invalid type: %d", object->type));
    }
}

/*
 * Free an rf_object_t
 */
null_t __attribute__((hot)) rf_object_free(rf_object_t *object)
{
    i64_t i, rc = 0, l;

    if (object->type == -TYPE_GUID)
    {
        if (object->guid == NULL)
            return;

        rf_free(object->guid);

        return;
    }

    if (!is_rc(object))
        return;

    rc_dec(rc, object);

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
        l = object->adt->len;
        for (i = 0; i < l; i++)
            rf_object_free(&as_list(object)[i]);
        if (rc == 0)
            vector_free(object);
        return;
    case TYPE_TABLE:
    case TYPE_DICT:
        rf_object_free(&as_list(object)[0]);
        rf_object_free(&as_list(object)[1]);
        if (rc == 0)
            rf_free(object->adt);
        return;
    case TYPE_LAMBDA:
        if (rc == 0)
        {
            rf_object_free(&as_lambda(object)->constants);
            rf_object_free(&as_lambda(object)->args);
            rf_object_free(&as_lambda(object)->locals);
            rf_object_free(&as_lambda(object)->code);
            debuginfo_free(&as_lambda(object)->debuginfo);
            rf_free(object->adt);
        }
        return;
    case TYPE_ERROR:
        if (rc == 0)
            rf_free(object->adt);
        return;
    default:
        panic(str_fmt(0, "free: invalid type: %d", object->type));
    }
}

/*
 * Copy on write rf_object_t
 */
rf_object_t rf_object_cow(rf_object_t *object)
{
    i64_t i, l;
    rf_object_t new;

    if (object->type == -TYPE_GUID)
    {
        if (object->guid == NULL)
            return guid(NULL);

        return guid(object->guid->data);
    }

    if (!is_rc(object))
        return *object;

    if (rf_object_rc(object) == 1)
        return rf_object_clone(object);

    switch (object->type)
    {
    case TYPE_BOOL:
        new = vector_bool(object->adt->len);
        new.adt->attrs = object->adt->attrs;
        memcpy(as_vector_bool(&new), as_vector_bool(object), object->adt->len);
        return new;
    case TYPE_I64:
        new = vector_i64(object->adt->len);
        new.adt->attrs = object->adt->attrs;
        memcpy(as_vector_i64(&new), as_vector_i64(object), object->adt->len * sizeof(i64_t));
        return new;
    case TYPE_F64:
        new = vector_f64(object->adt->len);
        new.adt->attrs = object->adt->attrs;
        memcpy(as_vector_f64(&new), as_vector_f64(object), object->adt->len * sizeof(f64_t));
        return new;
    case TYPE_SYMBOL:
        new = vector_symbol(object->adt->len);
        new.adt->attrs = object->adt->attrs;
        memcpy(as_vector_symbol(&new), as_vector_symbol(object), object->adt->len * sizeof(i64_t));
        return new;
    case TYPE_TIMESTAMP:
        new = vector_timestamp(object->adt->len);
        new.adt->attrs = object->adt->attrs;
        memcpy(as_vector_timestamp(&new), as_vector_timestamp(object), object->adt->len * sizeof(i64_t));
        return new;
    case TYPE_CHAR:
        new = string(object->adt->len);
        new.adt->attrs = object->adt->attrs;
        memcpy(as_string(&new), as_string(object), object->adt->len);
        return new;
    case TYPE_LIST:
        l = object->adt->len;
        new = list(l);
        new.adt->attrs = object->adt->attrs;
        for (i = 0; i < l; i++)
            as_list(&new)[i] = rf_object_cow(&as_list(object)[i]);
        return new;
    case TYPE_DICT:
        as_list(object)[0] = rf_object_cow(&as_list(object)[0]);
        as_list(object)[1] = rf_object_cow(&as_list(object)[1]);
        new.adt->attrs = object->adt->attrs;
        return new;
    case TYPE_TABLE:
        new = table(rf_object_cow(&as_list(object)[0]), rf_object_cow(&as_list(object)[1]));
        new.adt->attrs = object->adt->attrs;
        return new;
    case TYPE_LAMBDA:
        return *object;
    case TYPE_ERROR:
        return *object;
    default:
        panic(str_fmt(0, "cow: invalid type: %d", object->type));
    }
}

/*
 * Get the reference count of an rf_object_t
 */
i64_t rf_object_rc(rf_object_t *object)
{
    i64_t rc;

    if (object->type < 0)
        return 1;

    if (is_null(object))
        return 1;

    rc_get(rc, object);

    return rc;
}