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

rf_object_t error(i8_t code, str_t message)
{
    rf_object_t err = string_from_str(message, strlen(message));
    err.type = TYPE_ERROR;
    err.adt->code = code;
    return err;
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
    i64_t id = symbols_intern(s, strlen(s));
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

rf_object_t schar(char_t c)
{
    rf_object_t ch = {
        .type = -TYPE_CHAR,
        .schar = c,
    };

    return ch;
}

rf_object_t table(rf_object_t keys, rf_object_t vals)
{
    if (keys.type != TYPE_SYMBOL || vals.type != 0)
        return error(ERR_TYPE, "Keys must be a symbol vector and rf_objects must be list");

    if (keys.adt->len != vals.adt->len)
        return error(ERR_LENGTH, "Keys and rf_objects must have the same length");

    // rf_object_t *v = as_list(&vals);
    // i64_t len = 0;

    // for (i64_t i = 0; i < v.adt.len; i++)
    // {
    //     if (v[i].type < 0)
    //         return error(ERR_TYPE, "Values must be scalars");
    // }

    rf_object_t table = list(2);

    as_list(&table)[0] = keys;
    as_list(&table)[1] = vals;

    table.type = TYPE_TABLE;

    return table;
}

i8_t rf_object_eq(rf_object_t *a, rf_object_t *b)
{
    i64_t i, l;

    if (a->type != b->type)
        return 0;

    if (a->type == -TYPE_I64 || a->type == -TYPE_SYMBOL)
        return a->i64 == b->i64;
    else if (a->type == -TYPE_F64)
        return a->f64 == b->f64;
    else if (a->type == TYPE_I64 || a->type == TYPE_SYMBOL)
    {
        if (as_vector_i64(a) == as_vector_i64(b))
            return 1;
        if (a->adt->len != b->adt->len)
            return 0;

        l = a->adt->len;
        for (i = 0; i < l; i++)
        {
            if (as_vector_i64(a)[i] != as_vector_i64(b)[i])
                return 0;
        }
        return 1;
    }
    else if (a->type == TYPE_F64)
    {
        if (as_vector_f64(a) == as_vector_f64(b))
            return 1;
        if (a->adt->len != b->adt->len)
            return 0;

        l = a->adt->len;
        for (i = 0; i < l; i++)
        {
            if (as_vector_f64(a)[i] != as_vector_f64(b)[i])
                return 0;
        }
        return 1;
    }

    // printf("** Eq: Invalid type\n");
    return 0;
}

/*
 * Increment the reference count of an rf_object_t
 */
rf_object_t rf_object_clone(rf_object_t *object)
{
    i64_t i, l;

    if (object->type < TYPE_NULL)
        return *object;

    rc_inc(object);

    static null_t *types_table[] = {
        &&type_any, &&type_bool, &&type_i64, &&type_f64, &&type_symbol, &&type_char,
        &&type_list, &&type_dict, &&type_table, &&type_function, &&type_error};

    goto *types_table[(i32_t)object->type];

type_any:
    return *object;
type_bool:
    return *object;
type_i64:
    return *object;
type_f64:
    return *object;
type_symbol:
    return *object;
type_char:
    return *object;
type_list:
    l = object->adt->len;
    for (i = 0; i < l; i++)
        rc_inc(&as_list(object)[i]);
    return *object;
type_dict:
    rf_object_clone(&as_list(object)[0]);
    rf_object_clone(&as_list(object)[1]);
    return *object;
type_table:
    rf_object_clone(&as_list(object)[0]);
    rf_object_clone(&as_list(object)[1]);
    return *object;
type_function:
    return *object;
type_error:
    return *object;
}

/*
 * Free an rf_object_t
 */
null_t rf_object_free(rf_object_t *object)
{
    i64_t i, rc, l, *addrs;
    str_t code;

    if (object->type < 0)
        return;

    rc_dec(rc, object);

    static null_t *types_table[] = {
        &&type_any, &&type_bool, &&type_i64, &&type_f64, &&type_symbol, &&type_char,
        &&type_list, &&type_dict, &&type_table, &&type_function, &&type_error};

    goto *types_table[(i32_t)object->type];

type_any:
    return;
type_bool:
    if (rc == 0)
        vector_free(object);
    return;
type_i64:
    if (rc == 0)
        vector_free(object);
    return;
type_f64:
    if (rc == 0)
        vector_free(object);
    return;
type_symbol:
    if (rc == 0)
        vector_free(object);
    return;
type_char:
    if (rc == 0)
        vector_free(object);
    return;
type_list:
    l = object->adt->len;
    for (i = 0; i < l; i++)
        rf_object_free(&as_list(object)[i]);
    if (rc == 0)
        vector_free(object);
    return;
type_dict:
    rf_object_free(&as_list(object)[0]);
    rf_object_free(&as_list(object)[1]);
    if (rc == 0)
        rf_free(object->adt);
    return;
type_table:
    rf_object_free(&as_list(object)[0]);
    rf_object_free(&as_list(object)[1]);
    if (rc == 0)
        rf_free(object->adt);
    return;
type_function:
    if (rc == 0)
    {
        code = as_string(&as_function(object)->code);
        l = as_function(object)->const_addrs.adt->len;
        addrs = as_vector_i64(&as_function(object)->const_addrs);
        for (i = 0; i < l; i++)
            rf_object_free((rf_object_t *)(code + addrs[i]));
        rf_object_free(&as_function(object)->const_addrs);
        rf_object_free(&as_function(object)->args);
        rf_object_free(&as_function(object)->locals);
        rf_object_free(&as_function(object)->code);
        debuginfo_free(&as_function(object)->debuginfo);
        rf_free(object->adt);
    }
    return;
type_error:
    if (rc == 0)
        rf_free(object->adt);
    return;
}

/*
 * Copy on write rf_object_t
 */
rf_object_t rf_object_cow(rf_object_t *object)
{
    i64_t i, l;
    rf_object_t new;

    if (object->type < 0)
        return *object;

    if (object->adt == NULL)
        return *object;

    if (rf_object_rc(object) == 1)
        return rf_object_clone(object);

    static null_t *types_table[] = {
        &&type_any, &&type_bool, &&type_i64, &&type_f64, &&type_symbol, &&type_char,
        &&type_list, &&type_dict, &&type_table, &&type_function, &&type_error};

    goto *types_table[(i32_t)object->type];

type_any:
    return *object;
type_bool:
    new = vector_bool(object->adt->len);
    memcpy(as_vector_bool(&new), as_vector_bool(object), object->adt->len);
    return new;
type_i64:
    new = vector_i64(object->adt->len);
    memcpy(as_vector_i64(&new), as_vector_i64(object), object->adt->len * sizeof(i64_t));
    return new;
type_f64:
    new = vector_i64(object->adt->len);
    memcpy(as_vector_f64(&new), as_vector_f64(object), object->adt->len * sizeof(f64_t));
    return new;
type_symbol:
    new = vector_symbol(object->adt->len);
    memcpy(as_vector_symbol(&new), as_vector_symbol(object), object->adt->len * sizeof(i64_t));
    return new;
type_char:
    new = string(object->adt->len);
    memcpy(as_string(&new), as_string(object), object->adt->len);
    return new;
type_list:
    l = object->adt->len;
    new = list(l);
    for (i = 0; i < l; i++)
        as_list(&new)[i] = rf_object_cow(&as_list(object)[i]);
    return new;
type_dict:
    new = dict(rf_object_cow(&as_list(object)[0]), rf_object_cow(&as_list(object)[1]));
    return new;
type_table:
    new = table(rf_object_cow(&as_list(object)[0]), rf_object_cow(&as_list(object)[1]));
    return new;
type_function:
    return *object;
type_error:
    return *object;
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