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

#include <time.h>
#include "runtime.h"
#include "unary.h"
#include "binary.h"
#include "dict.h"
#include "util.h"
#include "ops.h"
#include "util.h"
#include "format.h"
#include "vector.h"
#include "hash.h"
#include "set.h"

rf_object_t call_binary(binary_t f, rf_object_t *x, rf_object_t *y)
{
    rf_object_t cx, cy, res;

    // no need to cast
    if (x->type == y->type || x->type == -y->type)
        return f(x, y);

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_F64):
        cx = f64(x->i64);
        return f(&cx, y);
    case MTYPE2(-TYPE_F64, -TYPE_I64):
        cy = f64(y->i64);
        return f(x, &cy);
    case MTYPE2(-TYPE_I64, TYPE_F64):
        cx = f64(x->i64);
        return f(&cx, y);
    case MTYPE2(-TYPE_F64, TYPE_I64):
        cx = symbol("F64");
        cy = rf_cast(&cx, y);
        res = f(x, &cy);
        rf_object_free(&cy);
        return res;
    case MTYPE2(TYPE_I64, -TYPE_F64):
        cx = symbol("F64");
        cy = rf_cast(&cx, x);
        res = f(y, &cy);
        rf_object_free(&cy);
        return res;
    case MTYPE2(TYPE_F64, -TYPE_I64):
        cy = f64(y->i64);
        return f(x, &cy);
    case MTYPE2(TYPE_I64, TYPE_F64):
        cx = symbol("F64");
        cy = rf_cast(&cx, x);
        res = f(&cy, y);
        rf_object_free(&cy);
        return res;
    case MTYPE2(TYPE_F64, TYPE_I64):
        cx = symbol("F64");
        cy = rf_cast(&cx, y);
        res = f(&cy, x);
        rf_object_free(&cy);
        return res;
    default:
        return f(x, y);
    }
}

rf_object_t rf_call_binary_left_atomic(binary_t f, rf_object_t *x, rf_object_t *y)
{
    u64_t i, l;
    rf_object_t res, item, a;

    if (x->type == TYPE_LIST)
    {
        l = x->adt->len;
        a = vector_get(x, 0);
        item = rf_call_binary_left_atomic(f, &a, y);
        rf_object_free(&a);

        if (item.type == TYPE_ERROR)
            return item;

        res = list(l);

        vector_write(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = vector_get(x, i);
            item = rf_call_binary_left_atomic(f, &a, y);
            rf_object_free(&a);

            if (item.type == TYPE_ERROR)
            {
                res.adt->len = i;
                rf_object_free(&res);
                return item;
            }

            vector_write(&res, i, item);
        }

        return res;
    }

    return f(x, y);
}

rf_object_t rf_call_binary_right_atomic(binary_t f, rf_object_t *x, rf_object_t *y)
{
    u64_t i, l;
    rf_object_t res, item, b;

    if (y->type == TYPE_LIST)
    {
        l = y->adt->len;
        b = vector_get(y, 0);
        item = rf_call_binary_right_atomic(f, x, &b);
        rf_object_free(&b);

        if (item.type == TYPE_ERROR)
            return item;

        res = list(l);

        vector_write(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = vector_get(y, i);
            item = rf_call_binary_right_atomic(f, x, &b);
            rf_object_free(&b);

            if (item.type == TYPE_ERROR)
            {
                res.adt->len = i;
                rf_object_free(&res);
                return item;
            }

            vector_write(&res, i, item);
        }

        return res;
    }

    return f(x, y);
}

// Atomic binary functions (iterates through list of arguments down to atoms)
rf_object_t rf_call_binary_atomic(binary_t f, rf_object_t *x, rf_object_t *y)
{
    u64_t i, l;
    rf_object_t res, item, a, b;

    if ((x->type == TYPE_LIST && is_vector(y)) ||
        (y->type == TYPE_LIST && is_vector(x)))
    {
        l = x->adt->len;

        if (l != y->adt->len)
            return error(ERR_LENGTH, "binary: vectors must be of the same length");

        a = vector_get(x, 0);
        b = vector_get(y, 0);
        item = rf_call_binary_atomic(f, &a, &b);
        rf_object_free(&a);
        rf_object_free(&b);

        if (item.type == TYPE_ERROR)
            return item;

        res = list(l);

        vector_write(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = vector_get(x, i);
            b = vector_get(y, i);
            item = rf_call_binary_atomic(f, &a, &b);
            rf_object_free(&a);
            rf_object_free(&b);

            if (item.type == TYPE_ERROR)
            {
                res.adt->len = i;
                rf_object_free(&res);
                return item;
            }

            vector_write(&res, i, item);
        }

        return res;
    }
    else if (x->type == TYPE_LIST)
    {
        l = x->adt->len;
        a = vector_get(x, 0);
        item = rf_call_binary_atomic(f, &a, y);
        rf_object_free(&a);

        if (item.type == TYPE_ERROR)
            return item;

        res = list(l);

        vector_write(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = vector_get(x, i);
            item = rf_call_binary_atomic(f, &a, y);
            rf_object_free(&a);

            if (item.type == TYPE_ERROR)
            {
                res.adt->len = i;
                rf_object_free(&res);
                return item;
            }

            vector_write(&res, i, item);
        }

        return res;
    }
    else if (y->type == TYPE_LIST)
    {
        l = y->adt->len;
        b = vector_get(y, 0);
        item = rf_call_binary_atomic(f, x, &b);
        rf_object_free(&b);

        if (item.type == TYPE_ERROR)
            return item;

        res = list(l);

        vector_write(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = vector_get(y, i);
            item = rf_call_binary_atomic(f, x, &b);
            rf_object_free(&b);

            if (item.type == TYPE_ERROR)
            {
                res.adt->len = i;
                rf_object_free(&res);
                return item;
            }

            vector_write(&res, i, item);
        }

        return res;
    }

    return call_binary(f, x, y);
}

rf_object_t rf_call_binary(u8_t flags, binary_t f, rf_object_t *x, rf_object_t *y)
{
    switch (flags)
    {
    case FLAG_ATOMIC:
        return rf_call_binary_atomic(f, x, y);
    case FLAG_LEFT_ATOMIC:
        return rf_call_binary_left_atomic(f, x, y);
    case FLAG_RIGHT_ATOMIC:
        return rf_call_binary_right_atomic(f, x, y);
    default:
        return call_binary(f, x, y);
    }
}

rf_object_t rf_set_variable(rf_object_t *key, rf_object_t *val)
{
    return dict_set(&runtime_get()->env.variables, key, rf_object_clone(val));
}

rf_object_t rf_dict(rf_object_t *x, rf_object_t *y)
{
    return dict(rf_object_clone(x), rf_object_clone(y));
}

rf_object_t rf_table(rf_object_t *x, rf_object_t *y)
{
    bool_t s = false;
    u64_t i, j, len, cl = 0;
    rf_object_t lst, c, l = null();

    if (x->type != TYPE_SYMBOL)
        return error(ERR_TYPE, "Keys must be a symbol vector");

    if (y->type != TYPE_LIST)
    {
        if (x->adt->len != 1)
            return error(ERR_LENGTH, "Keys and Values must have the same length");

        l = list(1);
        as_list(&l)[0] = rf_object_clone(y);
        y = &l;
    }

    if (x->adt->len != y->adt->len)
    {
        rf_object_free(&l);
        return error(ERR_LENGTH, "Keys and Values must have the same length");
    }

    len = y->adt->len;

    for (i = 0; i < len; i++)
    {
        switch (as_list(y)[i].type)
        {
        case TYPE_NULL:
        case -TYPE_BOOL:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_CHAR:
        case -TYPE_SYMBOL:
            s = true;
            break;
        case TYPE_BOOL:
        case TYPE_I64:
        case TYPE_F64:
        case TYPE_CHAR:
        case TYPE_SYMBOL:
        case TYPE_LIST:
            j = as_list(y)[i].adt->len;
            if (cl != 0 && j != cl)
                return error(ERR_LENGTH, "Values must be of the same length");

            cl = j;
            break;
        default:
            return error(ERR_TYPE, "unsupported type in a Values list");
        }
    }

    // there are no scalars and all columns are of the same length
    if (!s)
    {
        rf_object_free(&l);
        return table(rf_object_clone(x), rf_object_clone(y));
    }

    // otherwise we need to expand scalars to vectors
    lst = list(len);

    if (cl == 0)
        cl = 1;

    for (i = 0; i < len; i++)
    {
        switch (as_list(y)[i].type)
        {
        case TYPE_NULL:
        case -TYPE_BOOL:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_CHAR:
        case -TYPE_SYMBOL:
            c = i64(cl);
            as_list(&lst)[i] = rf_take(&c, &as_list(y)[i]);
            break;
        default:
            as_list(&lst)[i] = rf_object_clone(&as_list(y)[i]);
        }
    }

    rf_object_free(&l);

    return table(rf_object_clone(x), lst);
}

rf_object_t rf_rand(rf_object_t *x, rf_object_t *y)
{
    i64_t i, count;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        count = x->i64;
        vec = vector_i64(count);

        for (i = 0; i < count; i++)
            as_vector_i64(&vec)[i] = rfi_rand_u64() % y->i64;

        return vec;

    default:
        return error_type2(x->type, y->type, "rand: unsupported types");
    }
}

rf_object_t rf_add(rf_object_t *x, rf_object_t *y)
{
    u64_t i, l;
    rf_object_t vec, v;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (i64(ADDI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(ADDF64(x->f64, y->f64)));

    case MTYPE2(-TYPE_I64, TYPE_I64):
        l = y->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = ADDI64(x->i64, as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(-TYPE_F64, TYPE_F64):
        l = y->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = ADDF64(x->f64, as_vector_f64(y)[i]);

        return vec;

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = ADDI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        if (l != y->adt->len)
            return error(ERR_LENGTH, "add: vectors must be of the same length");
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = ADDI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = ADDF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        if (l != y->adt->len)
            return error(ERR_LENGTH, "add: vectors must be of the same length");
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = ADDF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    case MTYPE2(TYPE_LIST, TYPE_LIST):
        l = x->adt->len;
        if (l != y->adt->len)
            return error(ERR_LENGTH, "add: lists must be of the same length");

        vec = list(l);
        vec.adt->len = 0;

        for (i = 0; i < l; i++)
        {
            v = rf_add(&as_list(x)[i], &as_list(y)[i]);
            if (v.type == TYPE_ERROR)
            {
                rf_object_free(&vec);
                return v;
            }
            as_list(&vec)[vec.adt->len++] = v;
        }

        return vec;

    default:
        return error_type2(x->type, y->type, "add: unsupported types");
    }
}

rf_object_t rf_sub(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (i64(SUBI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(SUBF64(x->f64, y->f64)));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = SUBI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = SUBI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = SUBF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = SUBF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "sub: unsupported types");
    }
}

rf_object_t rf_mul(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (i64(MULI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(MULF64(x->f64, y->f64)));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = MULI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = MULI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = MULF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = MULF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "mul: unsupported types");
    }
}

rf_object_t rf_div(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (i64(DIVI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(DIVF64(x->f64, y->f64)));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = DIVI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = DIVI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = DIVF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = DIVF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "mul: unsupported types");
    }
}

rf_object_t rf_fdiv(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (f64(FDIVI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(FDIVF64(x->f64, y->f64)));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = FDIVI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = FDIVI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = FDIVF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = FDIVF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "fdiv: unsupported types");
    }
}

rf_object_t rf_mod(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (i64(MODI64(x->i64, y->i64)));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (f64(MODF64(x->f64, y->f64)));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = MODI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->adt->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&vec)[i] = MODI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = MODF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->adt->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&vec)[i] = MODF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "mod: unsupported types");
    }
}

rf_object_t rf_like(rf_object_t *x, rf_object_t *y)
{
    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_CHAR, TYPE_CHAR):
        return (bool(string_match(as_string(x), as_string(y))));

    default:
        return error_type2(x->type, y->type, "like: unsupported types");
    }
}

rf_object_t rf_eq(rf_object_t *x, rf_object_t *y)
{
    i64_t i, l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_BOOL, TYPE_BOOL):
        return (bool(x->bool == y->bool));

    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 == y->i64));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 == y->f64));

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->adt->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_vector_bool(&vec)[i] = as_vector_i64(x)[i] == y->i64;

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->adt->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_vector_bool(&vec)[i] = as_vector_f64(x)[i] == y->f64;

        return vec;

    case MTYPE2(-TYPE_I64, TYPE_I64):
        l = y->adt->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_vector_bool(&vec)[i] = x->i64 == as_vector_i64(y)[i];

        return vec;

    case MTYPE2(-TYPE_F64, TYPE_F64):
        l = y->adt->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_vector_bool(&vec)[i] = x->f64 == as_vector_f64(y)[i];

        return vec;

    case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
        l = x->adt->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_vector_bool(&vec)[i] = as_vector_symbol(x)[i] == y->i64;

        return vec;

    case MTYPE2(-TYPE_SYMBOL, TYPE_SYMBOL):
        l = y->adt->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_vector_bool(&vec)[i] = x->i64 == as_vector_symbol(y)[i];

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "eq: vectors of different length");

        l = x->adt->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_vector_bool(&vec)[i] = as_vector_i64(x)[i] == as_vector_i64(y)[i];

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "eq: vectors of different length");

        l = x->adt->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_vector_bool(&vec)[i] = as_vector_f64(x)[i] == as_vector_f64(y)[i];

        return vec;

    case MTYPE2(TYPE_LIST, TYPE_LIST):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "eq: lists of different length");

        l = x->adt->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_vector_bool(&vec)[i] = rf_eq(&as_list(x)[i], &as_list(y)[i]).bool;

        return vec;

    default:
        return error_type2(x->type, y->type, "eq: unsupported types");
    }
}

rf_object_t rf_ne(rf_object_t *x, rf_object_t *y)
{
    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_BOOL, TYPE_BOOL):
        return (bool(x->bool != y->bool));

    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 != y->i64));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 != y->f64));

    default:
        return error_type2(x->type, y->type, "ne: unsupported types");
    }
}

rf_object_t rf_lt(rf_object_t *x, rf_object_t *y)
{
    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 < y->i64));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 < y->f64));

    default:
        return error_type2(x->type, y->type, "lt: unsupported types");
    }
}

rf_object_t rf_le(rf_object_t *x, rf_object_t *y)
{
    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 <= y->i64));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 <= y->f64));

    default:
        return error_type2(x->type, y->type, "le: unsupported types");
    }
}

rf_object_t rf_gt(rf_object_t *x, rf_object_t *y)
{
    i64_t i, l;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 > y->i64));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 > y->f64));

    case MTYPE2(TYPE_I64, TYPE_I64):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "gt: vectors of different length");

        l = x->adt->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_vector_bool(&vec)[i] = as_vector_i64(x)[i] > as_vector_i64(y)[i];

        return vec;

    default:
        return error_type2(x->type, y->type, "gt: unsupported types");
    }
}

rf_object_t rf_ge(rf_object_t *x, rf_object_t *y)
{
    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 >= y->i64));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 >= y->f64));

    default:
        return error_type2(x->type, y->type, "ge: unsupported types");
    }
}

rf_object_t rf_and(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t res;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_BOOL, -TYPE_BOOL):
        return (bool(x->bool && y->bool));

    case MTYPE2(TYPE_BOOL, TYPE_BOOL):
        l = x->adt->len;
        res = vector_bool(x->adt->len);
        for (i = 0; i < l; i++)
            as_vector_bool(&res)[i] = as_vector_bool(x)[i] & as_vector_bool(y)[i];

        return res;

    default:
        return error_type2(x->type, y->type, "and: unsupported types");
    }
}

rf_object_t rf_or(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l;
    rf_object_t res;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_BOOL, -TYPE_BOOL):
        return (bool(x->bool || y->bool));

    case MTYPE2(TYPE_BOOL, TYPE_BOOL):
        l = x->adt->len;
        res = vector_bool(x->adt->len);
        for (i = 0; i < l; i++)
            as_vector_bool(&res)[i] = as_vector_bool(x)[i] | as_vector_bool(y)[i];

        return res;

    default:
        return error_type2(x->type, y->type, "and: unsupported types");
    }
}

rf_object_t rf_get(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t yl, xl;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_BOOL, -TYPE_I64):
    case MTYPE2(TYPE_I64, -TYPE_I64):
    case MTYPE2(TYPE_F64, -TYPE_I64):
    case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
    case MTYPE2(TYPE_GUID, -TYPE_I64):
    case MTYPE2(TYPE_CHAR, -TYPE_I64):
    case MTYPE2(TYPE_LIST, -TYPE_I64):
        return vector_get(x, y->i64);

    case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
        return dict_get(x, y);

    case MTYPE2(TYPE_BOOL, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = vector_bool(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_bool(y)[i] >= xl)
                as_vector_bool(&vec)[i] = false;
            else
                as_vector_bool(&vec)[i] = as_vector_bool(x)[(i32_t)as_vector_bool(y)[i]];
        }

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = vector_i64(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_i64(y)[i] >= xl)
                as_vector_i64(&vec)[i] = NULL_I64;
            else
                as_vector_i64(&vec)[i] = as_vector_i64(x)[(i32_t)as_vector_i64(y)[i]];
        }

        return vec;

    case MTYPE2(TYPE_F64, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = vector_f64(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_i64(y)[i] >= xl)
                as_vector_f64(&vec)[i] = NULL_F64;
            else
                as_vector_f64(&vec)[i] = as_vector_f64(x)[(i32_t)as_vector_i64(y)[i]];
        }

        return vec;

    case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = vector_timestamp(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_i64(y)[i] >= xl)
                as_vector_timestamp(&vec)[i] = NULL_I64;
            else
                as_vector_timestamp(&vec)[i] = as_vector_timestamp(x)[(i32_t)as_vector_i64(y)[i]];
        }

        return vec;

        // case MTYPE2(TYPE_GUID, TYPE_I64):
        //     yl = y->adt->len;
        //     xl = x->adt->len;
        //     vec = vector_guid(yl);
        //     for (i = 0; i < yl; i++)
        //     {
        //         if (as_vector_i64(y)[i] >= xl)
        //             as_vector_guid(&vec)[i] = guid(NULL_GUID);
        //         else
        //             as_vector_guid(&vec)[i] = as_vector_guid(x)[(i32_t)as_vector_i64(y)[i]];
        //     }

        //     return vec;

    case MTYPE2(TYPE_CHAR, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = string(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_i64(y)[i] >= xl)
                as_string(&vec)[i] = ' ';
            else
                as_string(&vec)[i] = as_string(x)[(i32_t)as_vector_i64(y)[i]];
        }

        return vec;

    case MTYPE2(TYPE_LIST, TYPE_I64):
        yl = y->adt->len;
        xl = x->adt->len;
        vec = list(yl);
        for (i = 0; i < yl; i++)
        {
            if (as_vector_i64(y)[i] >= xl)
                as_list(&vec)[i] = null();
            else
                as_list(&vec)[i] = rf_object_clone(&as_list(x)[(i32_t)as_vector_i64(y)[i]]);
        }

        return vec;

    default:
        return error_type2(x->type, y->type, "get: unsupported types");
    }
}

rf_object_t rf_find_I64_I64(rf_object_t *x, rf_object_t *y)
{
    u64_t i, n, range, xl = x->adt->len, yl = y->adt->len;
    i64_t max = 0, min = 0;
    rf_object_t vec = vector_i64(yl), found;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y),
          *ov = as_vector_i64(&vec), *fv;
    ht_t *ht;

    for (i = 0; i < xl; i++)
    {
        if (iv1[i] > max)
            max = iv1[i];
        else if (iv1[i] < min)
            min = iv1[i];
    }

#define normalize(k) ((u64_t)(k - min))
    // if range fits in 64 mb, use vector positions instead of hash table
    range = max - min + 1;
    if (range < 1024 * 1024 * 64)
    {
        found = vector_i64(range);
        fv = as_vector_i64(&found);

        for (i = 0; i < xl; i++)
            fv[i] = NULL_I64;

        for (i = 0; i < xl; i++)
        {
            n = normalize(iv1[i]);
            if (fv[n] == NULL_I64)
                fv[n] = i;
        }

        for (i = 0; i < yl; i++)
        {
            n = normalize(iv2[i]);
            if (iv2[i] < min || iv2[i] > max)
                ov[i] = NULL_I64;
            else
                ov[i] = fv[n];
        }

        rf_object_free(&found);

        return vec;
    }

    // otherwise, use a hash table
    ht = ht_new(xl, &rfi_kmh_hash, &i64_cmp);

    for (i = 0; i < xl; i++)
        ht_insert(ht, iv1[i], i);

    for (i = 0; i < yl; i++)
        ov[i] = ht_get(ht, iv2[i]);

    ht_free(ht);

    return vec;
}

rf_object_t rf_find(rf_object_t *x, rf_object_t *y)
{
    i64_t l, i;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_BOOL, -TYPE_BOOL):
    case MTYPE2(TYPE_I64, -TYPE_I64):
    case MTYPE2(TYPE_F64, -TYPE_F64):
    case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
    case MTYPE2(TYPE_GUID, -TYPE_GUID):
    case MTYPE2(TYPE_CHAR, -TYPE_CHAR):
    case MTYPE2(TYPE_LIST, -TYPE_LIST):
        l = x->adt->len;
        i = vector_find(x, y);

        if (i == l)
            return i64(NULL_I64);
        else
            return i64(i);

        return i64(vector_find(x, y));

    case MTYPE2(TYPE_I64, TYPE_I64):
        return rf_find_I64_I64(x, y);

    default:
        return error_type2(x->type, y->type, "find: unsupported types");
    }
}

rf_object_t rf_concat(rf_object_t *x, rf_object_t *y)
{
    i64_t i, xl, yl;
    rf_object_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_BOOL, -TYPE_BOOL):
        vec = vector_bool(2);
        as_vector_bool(&vec)[0] = x->bool;
        as_vector_bool(&vec)[1] = y->bool;
        return vec;

    case MTYPE2(-TYPE_I64, -TYPE_I64):
        vec = vector_i64(2);
        as_vector_i64(&vec)[0] = x->i64;
        as_vector_i64(&vec)[1] = y->i64;
        return vec;

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        vec = vector_f64(2);
        as_vector_f64(&vec)[0] = x->f64;
        as_vector_f64(&vec)[1] = y->f64;
        return vec;

    case MTYPE2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        vec = vector_timestamp(2);
        as_vector_timestamp(&vec)[0] = x->i64;
        as_vector_timestamp(&vec)[1] = y->i64;
        return vec;

    case MTYPE2(-TYPE_GUID, -TYPE_GUID):
        vec = vector_guid(2);
        memcpy(&as_vector_guid(&vec)[0], x->guid, sizeof(guid_t));
        memcpy(&as_vector_guid(&vec)[1], y->guid, sizeof(guid_t));
        return vec;

    case MTYPE2(-TYPE_CHAR, -TYPE_CHAR):
        vec = string(2);
        as_string(&vec)[0] = x->schar;
        as_string(&vec)[1] = y->schar;
        return vec;

    case MTYPE2(TYPE_BOOL, -TYPE_BOOL):
        xl = x->adt->len;
        vec = vector_bool(xl + 1);
        for (i = 0; i < xl; i++)
            as_vector_bool(&vec)[i] = as_vector_bool(x)[i];

        as_vector_bool(&vec)[xl] = y->bool;
        return vec;

    case MTYPE2(TYPE_I64, -TYPE_I64):
        xl = x->adt->len;
        vec = vector_i64(xl + 1);
        for (i = 0; i < xl; i++)
            as_vector_i64(&vec)[i] = as_vector_i64(x)[i];

        as_vector_i64(&vec)[xl] = y->i64;
        return vec;

    case MTYPE2(-TYPE_I64, TYPE_I64):
        yl = y->adt->len;
        vec = vector_i64(yl + 1);
        as_vector_i64(&vec)[0] = x->i64;
        for (i = 1; i <= yl; i++)
            as_vector_i64(&vec)[i] = as_vector_i64(y)[i - 1];

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        xl = x->adt->len;
        vec = vector_f64(xl + 1);
        for (i = 0; i < xl; i++)
            as_vector_f64(&vec)[i] = as_vector_f64(x)[i];

        as_vector_f64(&vec)[xl] = y->f64;
        return vec;

    case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        xl = x->adt->len;
        vec = vector_timestamp(xl + 1);
        for (i = 0; i < xl; i++)
            as_vector_timestamp(&vec)[i] = as_vector_timestamp(x)[i];

        as_vector_timestamp(&vec)[xl] = y->i64;
        return vec;

    case MTYPE2(TYPE_GUID, -TYPE_GUID):
        xl = x->adt->len;
        vec = vector_guid(xl + 1);
        for (i = 0; i < xl; i++)
            as_vector_guid(&vec)[i] = as_vector_guid(x)[i];

        memcpy(&as_vector_guid(&vec)[xl], y->guid, sizeof(guid_t));
        return vec;

    case MTYPE2(TYPE_BOOL, TYPE_BOOL):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = vector_bool(xl + yl);
        for (i = 0; i < xl; i++)
            as_vector_bool(&vec)[i] = as_vector_bool(x)[i];
        for (i = 0; i < yl; i++)
            as_vector_bool(&vec)[i + xl] = as_vector_bool(y)[i];
        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = vector_i64(xl + yl);
        for (i = 0; i < xl; i++)
            as_vector_i64(&vec)[i] = as_vector_i64(x)[i];
        for (i = 0; i < yl; i++)
            as_vector_i64(&vec)[i + xl] = as_vector_i64(y)[i];
        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = vector_f64(xl + yl);
        for (i = 0; i < xl; i++)
            as_vector_f64(&vec)[i] = as_vector_f64(x)[i];
        for (i = 0; i < yl; i++)
            as_vector_f64(&vec)[i + xl] = as_vector_f64(y)[i];
        return vec;

    case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = vector_timestamp(xl + yl);
        for (i = 0; i < xl; i++)
            as_vector_timestamp(&vec)[i] = as_vector_timestamp(x)[i];
        for (i = 0; i < yl; i++)
            as_vector_timestamp(&vec)[i + xl] = as_vector_timestamp(y)[i];
        return vec;

    case MTYPE2(TYPE_GUID, TYPE_GUID):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = vector_guid(xl + yl);
        for (i = 0; i < xl; i++)
            as_vector_guid(&vec)[i] = as_vector_guid(x)[i];
        for (i = 0; i < yl; i++)
            as_vector_guid(&vec)[i + xl] = as_vector_guid(y)[i];
        return vec;

    case MTYPE2(TYPE_CHAR, TYPE_CHAR):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = string(xl + yl);
        for (i = 0; i < xl; i++)
            as_string(&vec)[i] = as_string(x)[i];
        for (i = 0; i < yl; i++)
            as_string(&vec)[i + xl] = as_string(y)[i];
        return vec;

    case MTYPE2(TYPE_LIST, TYPE_LIST):
        xl = x->adt->len;
        yl = y->adt->len;
        vec = list(xl + yl);
        for (i = 0; i < xl; i++)
            as_list(&vec)[i] = rf_object_clone(&as_list(x)[i]);
        for (i = 0; i < yl; i++)
            as_list(&vec)[i + xl] = rf_object_clone(&as_list(y)[i]);
        return vec;

    default:
        if (x->type == TYPE_LIST)
        {
            xl = x->adt->len;
            vec = list(xl + 1);
            for (i = 0; i < xl; i++)
                as_list(&vec)[i] = rf_object_clone(&as_list(x)[i]);
            as_list(&vec)[xl] = rf_object_clone(y);

            return vec;
        }
        if (y->type == TYPE_LIST)
        {
            yl = y->adt->len;
            vec = list(yl + 1);
            as_list(&vec)[0] = rf_object_clone(x);
            for (i = 0; i < yl; i++)
                as_list(&vec)[i + 1] = rf_object_clone(&as_list(y)[i]);

            return vec;
        }

        return error_type2(x->type, y->type, "concat: unsupported types");
    }
}

rf_object_t rf_filter(rf_object_t *x, rf_object_t *y)
{
    i32_t i, j = 0;
    i64_t l, p = NULL_I64;
    rf_object_t res, *vals, col;

    switch (MTYPE2(x->type, y->type))
    {

    case MTYPE2(TYPE_BOOL, TYPE_BOOL):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->adt->len;
        res = vector_bool(l);
        for (i = 0; i < l; i++)
            if (as_vector_bool(y)[i])
                as_vector_bool(&res)[j++] = as_vector_bool(x)[i];

        vector_shrink(&res, j);

        return res;

    case MTYPE2(TYPE_I64, TYPE_BOOL):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->adt->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            if (as_vector_bool(y)[i])
                as_vector_i64(&res)[j++] = as_vector_i64(x)[i];

        vector_shrink(&res, j);

        return res;

    case MTYPE2(TYPE_SYMBOL, TYPE_BOOL):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->adt->len;
        res = vector_symbol(l);
        for (i = 0; i < l; i++)
            if (as_vector_bool(y)[i])
                as_vector_symbol(&res)[j++] = as_vector_symbol(x)[i];

        vector_shrink(&res, j);

        return res;

    case MTYPE2(TYPE_F64, TYPE_BOOL):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->adt->len;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            if (as_vector_bool(y)[i])
                as_vector_f64(&res)[j++] = as_vector_f64(x)[i];

        vector_shrink(&res, j);

        return res;

    case MTYPE2(TYPE_TIMESTAMP, TYPE_BOOL):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->adt->len;
        res = vector_timestamp(l);
        for (i = 0; i < l; i++)
            if (as_vector_bool(y)[i])
                as_vector_timestamp(&res)[j++] = as_vector_timestamp(x)[i];

        vector_shrink(&res, j);

        return res;

    case MTYPE2(TYPE_GUID, TYPE_BOOL):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->adt->len;
        res = vector_guid(l);
        for (i = 0; i < l; i++)
            if (as_vector_bool(y)[i])
                as_vector_guid(&res)[j++] = as_vector_guid(x)[i];

        vector_shrink(&res, j);

        return res;

    case MTYPE2(TYPE_CHAR, TYPE_BOOL):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->adt->len;
        res = string(l);
        for (i = 0; i < l; i++)
            if (as_vector_bool(y)[i])
                as_string(&res)[j++] = as_string(x)[i];

        vector_shrink(&res, j);

        return res;

    case MTYPE2(TYPE_LIST, TYPE_BOOL):
        if (x->adt->len != y->adt->len)
            return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

        l = x->adt->len;
        res = list(l);
        for (i = 0; i < l; i++)
            if (as_vector_bool(y)[i])
                as_list(&res)[j++] = rf_object_clone(&as_list(x)[i]);

        vector_shrink(&res, j);

        return res;

    case MTYPE2(TYPE_TABLE, TYPE_BOOL):
        vals = &as_list(x)[1];
        l = vals->adt->len;
        res = list(l);

        for (i = 0; i < l; i++)
        {
            col = vector_filter(&as_list(vals)[i], as_vector_bool(y), p);
            p = col.adt->len;
            as_list(&res)[i] = col;
        }

        return table(rf_object_clone(&as_list(x)[0]), res);

    default:
        return error_type2(x->type, y->type, "filter: unsupported types");
    }
}

rf_object_t rf_take(rf_object_t *x, rf_object_t *y)
{
    u64_t i, l, m;
    rf_object_t res, cols, sym, c;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_BOOL, -TYPE_I64):
    case MTYPE2(TYPE_I64, -TYPE_I64):
    case MTYPE2(TYPE_F64, -TYPE_I64):
    case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
    case MTYPE2(TYPE_GUID, -TYPE_I64):
    case MTYPE2(TYPE_SYMBOL, -TYPE_I64):
        return vector_get(x, y->i64);

    case MTYPE2(-TYPE_I64, TYPE_I64):
        l = y->adt->len;
        m = x->i64;
        res = vector_i64(m);

        for (i = 0; i < m; i++)
            as_vector_i64(&res)[i] = as_vector_i64(y)[i % l];

        return res;
    case MTYPE2(-TYPE_I64, TYPE_NULL):
        l = x->i64;
        res = list(l);
        for (i = 0; i < l; i++)
            as_list(&res)[i] = *y;

        return res;
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        l = x->i64;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&res)[i] = y->i64;

        return res;

    case MTYPE2(-TYPE_I64, -TYPE_F64):
        l = x->i64;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&res)[i] = y->f64;

        return res;

    case MTYPE2(-TYPE_I64, -TYPE_TIMESTAMP):
        l = x->i64;
        res = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_vector_timestamp(&res)[i] = y->i64;

        return res;

    case MTYPE2(TYPE_TABLE, -TYPE_I64):
    case MTYPE2(TYPE_TABLE, TYPE_I64):
        l = as_list(x)[0].adt->len;
        cols = list(l);
        for (i = 0; i < l; i++)
        {
            c = rf_take(&as_list(&as_list(x)[1])[i], y);

            if (is_scalar(&c))
                c = rf_enlist(&c, 1);

            if (c.type == TYPE_ERROR)
            {
                res.adt->len = i;
                rf_object_free(&res);
                return c;
            }

            vector_write(&cols, i, c);
        }

        res = rf_table(&as_list(x)[0], &cols);
        rf_object_free(&cols);

        return res;

    case MTYPE2(TYPE_TABLE, TYPE_SYMBOL):
        l = y->adt->len;
        cols = list(l);
        for (i = 0; i < l; i++)
        {
            sym = symboli64(as_vector_symbol(y)[i]);
            as_list(&cols)[i] = dict_get(x, &sym);
        }

        res = rf_table(y, &cols);
        rf_object_free(&cols);

        return res;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = y->adt->len;
        res = vector_i64(l);

        for (i = 0; i < l; i++)
            as_vector_i64(&res)[i] = vector_get(x, as_vector_i64(y)[i]).i64;

        return res;

    case MTYPE2(TYPE_SYMBOL, TYPE_I64):
        l = y->adt->len;
        res = vector_symbol(l);

        for (i = 0; i < l; i++)
            as_vector_symbol(&res)[i] = vector_get(x, as_vector_i64(y)[i]).i64;

        return res;

    case MTYPE2(TYPE_F64, TYPE_I64):
        l = y->adt->len;
        res = vector_f64(l);

        for (i = 0; i < l; i++)
            as_vector_f64(&res)[i] = vector_get(x, as_vector_i64(y)[i]).f64;

        return res;

    default:
        return error_type2(x->type, y->type, "take: unsupported types");
    }
}

rf_object_t rf_in(rf_object_t *x, rf_object_t *y)
{
    i64_t i, xl, yl;
    rf_object_t vec;
    set_t *set;

    switch
        MTYPE2(x->type, y->type)
        {
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
            xl = x->adt->len;
            yl = y->adt->len;
            set = set_new(yl, &rfi_i64_hash, &i64_cmp);

            for (i = 0; i < yl; i++)
                set_insert(set, as_vector_i64(y)[i]);

            vec = vector_bool(xl);

            for (i = 0; i < xl; i++)
                as_vector_bool(&vec)[i] = set_contains(set, as_vector_i64(x)[i]);

            set_free(set);

            return vec;

        default:
            return error_type2(x->type, y->type, "in: unsupported types");
        }
}

rf_object_t rf_sect(rf_object_t *x, rf_object_t *y)
{
    rf_object_t mask, res;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_I64, TYPE_I64):
    case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
        mask = rf_in(x, y);
        res = rf_filter(x, &mask);
        rf_object_free(&mask);
        return res;
    default:
        return error_type2(x->type, y->type, "sect: unsupported types");
    }
}

rf_object_t rf_except(rf_object_t *x, rf_object_t *y)
{
    i64_t i, j = 0, l;
    rf_object_t mask, res;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_I64, -TYPE_I64):
    case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
        l = x->adt->len;
        res = vector(x->type, l);

        for (i = 0; i < l; i++)
        {
            if (as_vector_i64(x)[i] != y->i64)
                as_vector_i64(&res)[j++] = as_vector_i64(x)[i];
        }

        vector_shrink(&res, j);

        return res;
    case MTYPE2(TYPE_I64, TYPE_I64):
    case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
        mask = rf_in(x, y);
        mask = rf_not(&mask);
        res = rf_filter(x, &mask);
        rf_object_free(&mask);
        return res;
    default:
        return error_type2(x->type, y->type, "except: unsupported types");
    }
}

/*
 * Casts the object to the specified type.
 * If the cast is not possible, returns an error object.
 * If object is already of the specified type, returns the object itself (cloned).
 */
rf_object_t rf_cast(rf_object_t *x, rf_object_t *y)
{
    if (x->type != -TYPE_SYMBOL)
        return error_type2(x->type, y->type, "cast: unsupported types");

    env_t *env = &runtime_get()->env;
    type_t type = env_get_type_by_typename(env, *x);

    rf_object_t res, err;
    i64_t i, l;
    str_t s, msg;

    // Do nothing if the type is the same
    if (type == y->type)
        return rf_object_clone(y);

    if (type == TYPE_CHAR)
    {
        s = rf_object_fmt(y);
        if (s == NULL)
            panic("rf_object_fmt() returned NULL");
        return string_from_str(s, strlen(s));
    }

    switch (MTYPE2(type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        res = *y;
        res.type = type;
        break;
    case MTYPE2(-TYPE_I64, -TYPE_F64):
        res = i64((i64_t)y->f64);
        res.type = type;
        break;
    case MTYPE2(-TYPE_F64, -TYPE_I64):
        res = f64((f64_t)y->i64);
        res.type = type;
        break;
    case MTYPE2(-TYPE_SYMBOL, TYPE_CHAR):
        res = symbol(as_string(y));
        break;
    case MTYPE2(-TYPE_I64, TYPE_CHAR):
        res = i64(strtol(as_string(y), NULL, 10));
        break;
    case MTYPE2(-TYPE_F64, TYPE_CHAR):
        res = f64(strtod(as_string(y), NULL));
        break;
    case MTYPE2(TYPE_TABLE, TYPE_DICT):
        res = rf_object_clone(y);
        res.type = type;
        break;
    case MTYPE2(TYPE_DICT, TYPE_TABLE):
        res = rf_object_clone(y);
        res.type = type;
        break;
    case MTYPE2(TYPE_I64, TYPE_LIST):
        res = vector_i64(y->adt->len);
        l = (i64_t)y->adt->len;
        for (i = 0; i < l; i++)
        {
            if (as_list(y)[i].type != -TYPE_I64)
            {
                rf_object_free(&res);
                msg = str_fmt(0, "invalid conversion from '%s' to 'i64'",
                              symbols_get(env_get_typename_by_type(env, as_list(y)[i].type)));
                err = error(ERR_TYPE, msg);
                rf_free(msg);
                return err;
            }

            as_vector_i64(&res)[i] = as_list(y)[i].i64;
        }
        break;
    case MTYPE2(TYPE_F64, TYPE_I64):
        res = vector_f64(y->adt->len);
        l = (i64_t)y->adt->len;
        for (i = 0; i < l; i++)
            as_vector_f64(&res)[i] = (f64_t)as_vector_i64(y)[i];
        break;
    case MTYPE2(TYPE_F64, TYPE_LIST):
        res = vector_f64(y->adt->len);
        l = (i64_t)y->adt->len;
        for (i = 0; i < l; i++)
        {
            if (as_list(y)[i].type != -TYPE_F64)
            {
                rf_object_free(&res);
                msg = str_fmt(0, "invalid conversion from '%s' to 'f64'",
                              symbols_get(env_get_typename_by_type(env, as_list(y)[i].type)));
                err = error(ERR_TYPE, msg);
                rf_free(msg);
                return err;
            }

            as_vector_f64(&res)[i] = as_list(y)[i].f64;
        }
        break;
    case MTYPE2(TYPE_BOOL, TYPE_I64):
        res = vector_bool(y->adt->len);
        l = (i64_t)y->adt->len;
        for (i = 0; i < l; i++)
            as_vector_bool(&res)[i] = as_list(y)[i].i64 != 0;
        break;
    case MTYPE2(-TYPE_GUID, TYPE_CHAR):
        res = guid(NULL);

        if (strlen(as_string(y)) != 36)
            break;

        res.guid = rf_malloc(sizeof(guid_t));

        i = sscanf(as_string(y),
                   "%02hhx%02hhx%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
                   &res.guid->data[0], &res.guid->data[1], &res.guid->data[2], &res.guid->data[3],
                   &res.guid->data[4], &res.guid->data[5],
                   &res.guid->data[6], &res.guid->data[7],
                   &res.guid->data[8], &res.guid->data[9],
                   &res.guid->data[10], &res.guid->data[11], &res.guid->data[12],
                   &res.guid->data[13], &res.guid->data[14], &res.guid->data[15]);

        if (i != 16)
        {
            rf_free(res.guid);
            res = guid(NULL);
        }

        break;
    case MTYPE2(-TYPE_I64, -TYPE_TIMESTAMP):
        res = timestamp(y->i64);
        break;
    case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I64):
        res = i64(y->i64);
        break;
    case MTYPE2(TYPE_I64, TYPE_TIMESTAMP):
        l = y->adt->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&res)[i] = as_vector_timestamp(y)[i];
        break;
    case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
        l = y->adt->len;
        res = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_vector_timestamp(&res)[i] = as_vector_i64(y)[i];
        break;
    default:
        msg = str_fmt(0, "invalid conversion from '%s' to '%s'",
                      symbols_get(env_get_typename_by_type(&runtime_get()->env, y->type)),
                      symbols_get(x->i64));
        err = error(ERR_TYPE, msg);
        rf_free(msg);
        return err;
    }

    return res;
}

rf_object_t rf_group_Table(rf_object_t *x, rf_object_t *y)
{
    i64_t i, l;
    rf_object_t res;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_TABLE, TYPE_LIST):
        l = as_list(x)[1].adt->len;
        res = list(l);
        for (i = 0; i < l; i++)
            as_list(&res)[i] = rf_call_binary_right_atomic(rf_take, &as_list(&as_list(x)[1])[i], y);

        return table(rf_object_clone(&as_list(x)[0]), res);

    default:
        return error_type2(x->type, y->type, "group: unsupported types");
    }
}