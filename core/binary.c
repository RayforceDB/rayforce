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
#include "util.h"
#include "ops.h"
#include "util.h"
#include "format.h"
#include "hash.h"
#include "set.h"

obj_t call_binary(binary_t f, obj_t x, obj_t y)
{
    obj_t cst, res;

    // no need to cast
    if (x->type == y->type || x->type == -y->type)
        return f(x, y);

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_F64):
        cst = f64(x->i64);
        res = f(cst, y);
        drop(cst);
        return res;
    case MTYPE2(-TYPE_F64, -TYPE_I64):
        cst = f64(y->i64);
        res = f(x, cst);
        drop(cst);
        return res;
    case MTYPE2(-TYPE_I64, TYPE_F64):
        cst = f64(x->i64);
        res = f(cst, y);
        drop(cst);
        return res;
    case MTYPE2(-TYPE_F64, TYPE_I64):
        cst = cast(TYPE_F64, y);
        res = f(x, cst);
        drop(cst);
        return res;
    case MTYPE2(TYPE_I64, -TYPE_F64):
        cst = cast(TYPE_I64, x);
        res = f(y, cst);
        drop(cst);
        return res;
    case MTYPE2(TYPE_F64, -TYPE_I64):
        cst = f64(y->i64);
        res = f(x, cst);
        drop(cst);
        return res;
    case MTYPE2(TYPE_I64, TYPE_F64):
        cst = cast(TYPE_F64, x);
        res = f(cst, y);
        drop(cst);
        return res;
    case MTYPE2(TYPE_F64, TYPE_I64):
        cst = cast(TYPE_F64, y);
        res = f(cst, x);
        drop(cst);
        return res;
    default:
        return f(x, y);
    }
}

obj_t rf_call_binary_left_atomic(binary_t f, obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res, item, a;

    if (x->type == TYPE_LIST)
    {
        l = x->len;
        a = at_idx(x, 0);
        item = rf_call_binary_left_atomic(f, a, y);
        drop(a);

        if (item->type == TYPE_ERROR)
            return item;

        res = list(l);

        write_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = at_idx(x, i);
            item = rf_call_binary_left_atomic(f, a, y);
            drop(a);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            write_obj(&res, i, item);
        }

        return res;
    }

    return f(x, y);
}

obj_t rf_call_binary_right_atomic(binary_t f, obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res, item, b;

    if (y->type == TYPE_LIST)
    {
        l = y->len;
        b = at_idx(y, 0);
        item = rf_call_binary_right_atomic(f, x, b);
        drop(b);

        if (item->type == TYPE_ERROR)
            return item;

        res = list(l);

        write_obj(res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = at_idx(y, i);
            item = rf_call_binary_right_atomic(f, x, b);
            drop(b);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            write_obj(&res, i, item);
        }

        return res;
    }

    return f(x, y);
}

// Atomic binary functions (iterates through list of arguments down to atoms)
obj_t rf_call_binary_atomic(binary_t f, obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res, item, a, b;

    if ((x->type == TYPE_LIST && is_vector(y)) ||
        (y->type == TYPE_LIST && is_vector(x)))
    {
        l = x->len;

        if (l != y->len)
            return error(ERR_LENGTH, "binary: vectors must be of the same length");

        a = at_idx(x, 0);
        b = at_idx(y, 0);
        item = rf_call_binary_atomic(f, a, b);
        drop(a);
        drop(b);

        if (item->type == TYPE_ERROR)
            return item;

        res = list(l);

        write_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = at_idx(x, i);
            b = at_idx(y, i);
            item = rf_call_binary_atomic(f, a, b);
            drop(a);
            drop(b);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            write_obj(&res, i, item);
        }

        return res;
    }
    else if (x->type == TYPE_LIST)
    {
        l = x->len;
        a = at_idx(x, 0);
        item = rf_call_binary_atomic(f, a, y);
        drop(a);

        if (item->type == TYPE_ERROR)
            return item;

        res = list(l);

        write_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            a = at_idx(x, i);
            item = rf_call_binary_atomic(f, a, y);
            drop(a);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            write_obj(&res, i, item);
        }

        return res;
    }
    else if (y->type == TYPE_LIST)
    {
        l = y->len;
        b = at_idx(y, 0);
        item = rf_call_binary_atomic(f, x, b);
        drop(b);

        if (item->type == TYPE_ERROR)
            return item;

        res = list(l);

        write_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            b = at_idx(y, i);
            item = rf_call_binary_atomic(f, x, b);
            drop(b);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            write_obj(&res, i, item);
        }

        return res;
    }

    return call_binary(f, x, y);
}

obj_t rf_call_binary(u8_t flags, binary_t f, obj_t x, obj_t y)
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
        return f(x, y);
    }
}

obj_t rf_set_variable(obj_t key, obj_t val)
{
    return set_obj(&runtime_get()->env.variables, key, val);
}

obj_t rf_dict(obj_t x, obj_t y)
{
    return dict(clone(x), clone(y));
}

obj_t rf_cast(obj_t x, obj_t y)
{
    type_t type;
    obj_t err;
    str_t fmt, msg;

    if (x->type != -TYPE_SYMBOL)
        raise(ERR_TYPE, "cast: first argument must be a symbol");

    type = env_get_type_by_typename(&runtime_get()->env, x->i64);

    if (type == TYPE_ERROR)
    {
        fmt = obj_fmt(x);
        msg = str_fmt(0, "cast: unknown type: '%s'", fmt);
        err = error(ERR_TYPE, msg);
        heap_free(fmt);
        heap_free(msg);
        return err;
    }

    return cast(type, y);
}

obj_t rf_table(obj_t x, obj_t y)
{
    // bool_t s = false;
    // u64_t i, j, len, cl = 0;
    // obj_t lst, c, l = null();

    // if (x->type != TYPE_SYMBOL)
    //     return error(ERR_TYPE, "Keys must be a symbol vector");

    // if (y->type != TYPE_LIST)
    // {
    //     if (x->len != 1)
    //         return error(ERR_LENGTH, "Keys and Values must have the same length");

    //     l = list(1);
    //     as_list(&l)[0] = clone(y);
    //     y = &l;
    // }

    // if (x->len != y->len)
    // {
    //     drop(l);
    //     return error(ERR_LENGTH, "Keys and Values must have the same length");
    // }

    // len = y->len;

    // for (i = 0; i < len; i++)
    // {
    //     switch (as_list(y)[i].type)
    //     {
    //     // case TYPE_NULL:
    //     case -TYPE_BOOL:
    //     case -TYPE_I64:
    //     case -TYPE_F64:
    //     case -TYPE_CHAR:
    //     case -TYPE_SYMBOL:
    //     case -TYPE_TIMESTAMP:
    //         s = true;
    //         break;
    //     case TYPE_BOOL:
    //     case TYPE_I64:
    //     case TYPE_F64:
    //     case TYPE_TIMESTAMP:
    //     case TYPE_CHAR:
    //     case TYPE_SYMBOL:
    //     case TYPE_LIST:
    //         j = as_list(y)[i]->len;
    //         if (cl != 0 && j != cl)
    //             return error(ERR_LENGTH, "Values must be of the same length");

    //         cl = j;
    //         break;
    //     default:
    //         return error(ERR_TYPE, "unsupported type in a Values list");
    //     }
    // }

    // // there are no atoms and all columns are of the same length
    // if (!s)
    // {
    //     drop(l);
    //     return table(clone(x), clone(y));
    // }

    // // otherwise we need to expand atoms to vectors
    // lst = list(len);

    // if (cl == 0)
    //     cl = 1;

    // for (i = 0; i < len; i++)
    // {
    //     switch (as_list(y)[i].type)
    //     {
    //     // case TYPE_NULL:
    //     case -TYPE_BOOL:
    //     case -TYPE_I64:
    //     case -TYPE_F64:
    //     case -TYPE_CHAR:
    //     case -TYPE_SYMBOL:
    //         c = i64(cl);
    //         as_list(&lst)[i] = rf_take(&c, &as_list(y)[i]);
    //         break;
    //     default:
    //         as_list(&lst)[i] = clone(&as_list(y)[i]);
    //     }
    // }

    // drop(l);

    // return table(clone(x), lst);
    return null();
}

obj_t rf_rand(obj_t x, obj_t y)
{
    i64_t i, count;
    obj_t vec;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        count = x->i64;
        vec = vector_i64(count);

        for (i = 0; i < count; i++)
            as_vector_i64(vec)[i] = rfi_rand_u64() % y->i64;

        return vec;

    default:
        return error_type2(x->type, y->type, "rand: unsupported types");
    }
}

obj_t rf_add(obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t vec, v;

    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(-TYPE_I64, -TYPE_I64):
        return i64(ADDI64(x->i64, y->i64));

    case MTYPE2(-TYPE_F64, -TYPE_F64):
        return f64(ADDF64(x->f64, y->f64));

    case MTYPE2(-TYPE_TIMESTAMP, -TYPE_I64):
        return timestamp(ADDI64(x->i64, y->i64));

    case MTYPE2(-TYPE_I64, TYPE_I64):
        l = y->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(vec)[i] = ADDI64(x->i64, as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(-TYPE_F64, TYPE_F64):
        l = y->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(vec)[i] = ADDF64(x->f64, as_vector_f64(y)[i]);

        return vec;

    case MTYPE2(-TYPE_TIMESTAMP, TYPE_I64):
        l = y->len;
        vec = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_vector_i64(vec)[i] = ADDI64(x->i64, as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
        l = x->len;
        vec = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_vector_i64(vec)[i] = ADDI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
        l = x->len;
        vec = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_vector_i64(vec)[i] = ADDI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(vec)[i] = ADDI64(as_vector_i64(x)[i], y->i64);

        return vec;

    case MTYPE2(TYPE_I64, TYPE_I64):
        l = x->len;
        if (l != y->len)
            return error(ERR_LENGTH, "add: vectors must be of the same length");
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(vec)[i] = ADDI64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

        return vec;

    case MTYPE2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(vec)[i] = ADDF64(as_vector_f64(x)[i], y->f64);

        return vec;

    case MTYPE2(TYPE_F64, TYPE_F64):
        l = x->len;
        if (l != y->len)
            return error(ERR_LENGTH, "add: vectors must be of the same length");
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(vec)[i] = ADDF64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

        return vec;

    default:
        return error_type2(x->type, y->type, "add: unsupported types");
    }
}

obj_t rf_sub(obj_t x, obj_t y)
{
    // i32_t i;
    // i64_t l;
    // obj_t vec;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(-TYPE_I64, -TYPE_I64):
    //     return (i64(SUBvector_i64(x->i64, y->i64)));

    // case MTYPE2(-TYPE_F64, -TYPE_F64):
    //     return (f64(SUBvector_f64(x->f64, y->f64)));

    // case MTYPE2(TYPE_I64, -TYPE_I64):
    //     l = x->len;
    //     vec = vector_i64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_i64(&vec)[i] = SUBvector_i64(as_vector_i64(x)[i], y->i64);

    //     return vec;

    // case MTYPE2(TYPE_I64, TYPE_I64):
    //     l = x->len;
    //     vec = vector_i64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_i64(&vec)[i] = SUBvector_i64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

    //     return vec;

    // case MTYPE2(TYPE_F64, -TYPE_F64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = SUBvector_f64(as_vector_f64(x)[i], y->f64);

    //     return vec;

    // case MTYPE2(TYPE_F64, TYPE_F64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = SUBvector_f64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

    //     return vec;

    // default:
    //     return error_type2(x->type, y->type, "sub: unsupported types");
    // }

    return null();
}

obj_t rf_mul(obj_t x, obj_t y)
{
    // i32_t i;
    // i64_t l;
    // obj_t vec;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(-TYPE_I64, -TYPE_I64):
    //     return (i64(MULvector_i64(x->i64, y->i64)));

    // case MTYPE2(-TYPE_F64, -TYPE_F64):
    //     return (f64(MULvector_f64(x->f64, y->f64)));

    // case MTYPE2(TYPE_I64, -TYPE_I64):
    //     l = x->len;
    //     vec = vector_i64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_i64(&vec)[i] = MULvector_i64(as_vector_i64(x)[i], y->i64);

    //     return vec;

    // case MTYPE2(TYPE_I64, TYPE_I64):
    //     l = x->len;
    //     vec = vector_i64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_i64(&vec)[i] = MULvector_i64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

    //     return vec;

    // case MTYPE2(TYPE_F64, -TYPE_F64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = MULvector_f64(as_vector_f64(x)[i], y->f64);

    //     return vec;

    // case MTYPE2(TYPE_F64, TYPE_F64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = MULvector_f64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

    //     return vec;

    // default:
    //     return error_type2(x->type, y->type, "mul: unsupported types");
    // }

    return null();
}

obj_t rf_div(obj_t x, obj_t y)
{
    // i32_t i;
    // i64_t l;
    // obj_t vec;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(-TYPE_I64, -TYPE_I64):
    //     return (i64(DIVvector_i64(x->i64, y->i64)));

    // case MTYPE2(-TYPE_F64, -TYPE_F64):
    //     return (f64(DIVvector_f64(x->f64, y->f64)));

    // case MTYPE2(TYPE_I64, -TYPE_I64):
    //     l = x->len;
    //     vec = vector_i64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_i64(&vec)[i] = DIVvector_i64(as_vector_i64(x)[i], y->i64);

    //     return vec;

    // case MTYPE2(TYPE_I64, TYPE_I64):
    //     l = x->len;
    //     vec = vector_i64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_i64(&vec)[i] = DIVvector_i64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

    //     return vec;

    // case MTYPE2(TYPE_F64, -TYPE_F64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = DIVvector_f64(as_vector_f64(x)[i], y->f64);

    //     return vec;

    // case MTYPE2(TYPE_F64, TYPE_F64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = DIVvector_f64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

    //     return vec;

    // default:
    //     return error_type2(x->type, y->type, "mul: unsupported types");
    // }

    return null();
}

obj_t rf_fdiv(obj_t x, obj_t y)
{
    // i32_t i;
    // i64_t l;
    // obj_t vec;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(-TYPE_I64, -TYPE_I64):
    //     return (f64(FDIVvector_i64(x->i64, y->i64)));

    // case MTYPE2(-TYPE_F64, -TYPE_F64):
    //     return (f64(FDIVvector_f64(x->f64, y->f64)));

    // case MTYPE2(TYPE_I64, -TYPE_I64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = FDIVvector_i64(as_vector_i64(x)[i], y->i64);

    //     return vec;

    // case MTYPE2(TYPE_I64, TYPE_I64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = FDIVvector_i64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

    //     return vec;

    // case MTYPE2(TYPE_F64, -TYPE_F64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = FDIVvector_f64(as_vector_f64(x)[i], y->f64);

    //     return vec;

    // case MTYPE2(TYPE_F64, TYPE_F64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = FDIVvector_f64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

    //     return vec;

    // default:
    //     return error_type2(x->type, y->type, "fdiv: unsupported types");
    // }

    return null();
}

obj_t rf_mod(obj_t x, obj_t y)
{
    // i32_t i;
    // i64_t l;
    // obj_t vec;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(-TYPE_I64, -TYPE_I64):
    //     return (i64(MODvector_i64(x->i64, y->i64)));

    // case MTYPE2(-TYPE_F64, -TYPE_F64):
    //     return (f64(MODvector_f64(x->f64, y->f64)));

    // case MTYPE2(TYPE_I64, -TYPE_I64):
    //     l = x->len;
    //     vec = vector_i64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_i64(&vec)[i] = MODvector_i64(as_vector_i64(x)[i], y->i64);

    //     return vec;

    // case MTYPE2(TYPE_I64, TYPE_I64):
    //     l = x->len;
    //     vec = vector_i64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_i64(&vec)[i] = MODvector_i64(as_vector_i64(x)[i], as_vector_i64(y)[i]);

    //     return vec;

    // case MTYPE2(TYPE_F64, -TYPE_F64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = MODvector_f64(as_vector_f64(x)[i], y->f64);

    //     return vec;

    // case MTYPE2(TYPE_F64, TYPE_F64):
    //     l = x->len;
    //     vec = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&vec)[i] = MODvector_f64(as_vector_f64(x)[i], as_vector_f64(y)[i]);

    //     return vec;

    // default:
    //     return error_type2(x->type, y->type, "mod: unsupported types");
    // }

    return null();
}

obj_t rf_like(obj_t x, obj_t y)
{
    switch (MTYPE2(x->type, y->type))
    {
    case MTYPE2(TYPE_CHAR, TYPE_CHAR):
        return (bool(string_match(as_string(x), as_string(y))));

    default:
        return error_type2(x->type, y->type, "like: unsupported types");
    }
}

obj_t rf_eq(obj_t x, obj_t y)
{
    // i64_t i, l;
    // obj_t vec;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(TYPE_BOOL, TYPE_BOOL):
    //     return (bool(x->bool == y->bool));

    // case MTYPE2(-TYPE_I64, -TYPE_I64):
    //     return (bool(x->i64 == y->i64));

    // case MTYPE2(-TYPE_F64, -TYPE_F64):
    //     return (bool(x->f64 == y->f64));

    // case MTYPE2(TYPE_I64, -TYPE_I64):
    //     l = x->len;
    //     vec = vector_bool(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&vec)[i] = as_vector_i64(x)[i] == y->i64;

    //     return vec;

    // case MTYPE2(TYPE_F64, -TYPE_F64):
    //     l = x->len;
    //     vec = vector_bool(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&vec)[i] = as_vector_f64(x)[i] == y->f64;

    //     return vec;

    // case MTYPE2(-TYPE_I64, TYPE_I64):
    //     l = y->len;
    //     vec = vector_bool(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&vec)[i] = x->i64 == as_vector_i64(y)[i];

    //     return vec;

    // case MTYPE2(-TYPE_F64, TYPE_F64):
    //     l = y->len;
    //     vec = vector_bool(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&vec)[i] = x->f64 == as_vector_f64(y)[i];

    //     return vec;

    // case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
    //     l = x->len;
    //     vec = vector_bool(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&vec)[i] = as_vector_symbol(x)[i] == y->i64;

    //     return vec;

    // case MTYPE2(-TYPE_SYMBOL, TYPE_SYMBOL):
    //     l = y->len;
    //     vec = vector_bool(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&vec)[i] = x->i64 == as_vector_symbol(y)[i];

    //     return vec;

    // case MTYPE2(TYPE_I64, TYPE_I64):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "eq: vectors of different length");

    //     l = x->len;
    //     vec = vector_bool(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&vec)[i] = as_vector_i64(x)[i] == as_vector_i64(y)[i];

    //     return vec;

    // case MTYPE2(TYPE_F64, TYPE_F64):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "eq: vectors of different length");

    //     l = x->len;
    //     vec = vector_bool(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&vec)[i] = as_vector_f64(x)[i] == as_vector_f64(y)[i];

    //     return vec;

    // case MTYPE2(TYPE_LIST, TYPE_LIST):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "eq: lists of different length");

    //     l = x->len;
    //     vec = vector_bool(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&vec)[i] = rf_eq(&as_list(x)[i], &as_list(y)[i]).bool;

    //     return vec;

    // default:
    //     return error_type2(x->type, y->type, "eq: unsupported types");
    // }

    return null();
}

obj_t rf_ne(obj_t x, obj_t y)
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

obj_t rf_lt(obj_t x, obj_t y)
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

obj_t rf_le(obj_t x, obj_t y)
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

obj_t rf_gt(obj_t x, obj_t y)
{
    // i64_t i, l;
    // obj_t vec;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(-TYPE_I64, -TYPE_I64):
    //     return (bool(x->i64 > y->i64));

    // case MTYPE2(-TYPE_F64, -TYPE_F64):
    //     return (bool(x->f64 > y->f64));

    // case MTYPE2(TYPE_I64, TYPE_I64):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "gt: vectors of different length");

    //     l = x->len;
    //     vec = vector_bool(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&vec)[i] = as_vector_i64(x)[i] > as_vector_i64(y)[i];

    //     return vec;

    // default:
    //     return error_type2(x->type, y->type, "gt: unsupported types");
    // }

    return null();
}

obj_t rf_ge(obj_t x, obj_t y)
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

obj_t rf_and(obj_t x, obj_t y)
{
    // i32_t i;
    // i64_t l;
    // obj_t res;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(-TYPE_BOOL, -TYPE_BOOL):
    //     return (bool(x->bool && y->bool));

    // case MTYPE2(TYPE_BOOL, TYPE_BOOL):
    //     l = x->len;
    //     res = vector_bool(x->len);
    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&res)[i] = as_vector_bool(x)[i] & as_vector_bool(y)[i];

    //     return res;

    // default:
    //     return error_type2(x->type, y->type, "and: unsupported types");
    // }

    return null();
}

obj_t rf_or(obj_t x, obj_t y)
{
    // i32_t i;
    // i64_t l;
    // obj_t res;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(-TYPE_BOOL, -TYPE_BOOL):
    //     return (bool(x->bool || y->bool));

    // case MTYPE2(TYPE_BOOL, TYPE_BOOL):
    //     l = x->len;
    //     res = vector_bool(x->len);
    //     for (i = 0; i < l; i++)
    //         as_vector_bool(&res)[i] = as_vector_bool(x)[i] | as_vector_bool(y)[i];

    //     return res;

    // default:
    //     return error_type2(x->type, y->type, "and: unsupported types");
    // }

    return null();
}

obj_t rf_get(obj_t x, obj_t y)
{
    // i32_t i;
    // i64_t yl, xl;
    // obj_t vec;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(TYPE_BOOL, -TYPE_I64):
    // case MTYPE2(TYPE_I64, -TYPE_I64):
    // case MTYPE2(TYPE_F64, -TYPE_I64):
    // case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
    // case MTYPE2(TYPE_GUID, -TYPE_I64):
    // case MTYPE2(TYPE_CHAR, -TYPE_I64):
    // case MTYPE2(TYPE_LIST, -TYPE_I64):
    //     return vector_get(x, y->i64);

    // case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
    //     return dict_get(x, y);

    // case MTYPE2(TYPE_BOOL, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = vector_bool(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_vector_bool(y)[i] >= xl)
    //             as_vector_bool(&vec)[i] = false;
    //         else
    //             as_vector_bool(&vec)[i] = as_vector_bool(x)[(i32_t)as_vector_bool(y)[i]];
    //     }

    //     return vec;

    // case MTYPE2(TYPE_I64, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = vector_i64(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_vector_i64(y)[i] >= xl)
    //             as_vector_i64(&vec)[i] = NULL_I64;
    //         else
    //             as_vector_i64(&vec)[i] = as_vector_i64(x)[(i32_t)as_vector_i64(y)[i]];
    //     }

    //     return vec;

    // case MTYPE2(TYPE_F64, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = vector_f64(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_vector_i64(y)[i] >= xl)
    //             as_vector_f64(&vec)[i] = NULL_F64;
    //         else
    //             as_vector_f64(&vec)[i] = as_vector_f64(x)[(i32_t)as_vector_i64(y)[i]];
    //     }

    //     return vec;

    // case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = vector_timestamp(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_vector_i64(y)[i] >= xl)
    //             as_vector_timestamp(&vec)[i] = NULL_I64;
    //         else
    //             as_vector_timestamp(&vec)[i] = as_vector_timestamp(x)[(i32_t)as_vector_i64(y)[i]];
    //     }

    //     return vec;

    //     // case MTYPE2(TYPE_GUID, TYPE_I64):
    //     //     yl = y->len;
    //     //     xl = x->len;
    //     //     vec = vector_guid(yl);
    //     //     for (i = 0; i < yl; i++)
    //     //     {
    //     //         if (as_vector_i64(y)[i] >= xl)
    //     //             as_vector_guid(&vec)[i] = guid(NULL_GUID);
    //     //         else
    //     //             as_vector_guid(&vec)[i] = as_vector_guid(x)[(i32_t)as_vector_i64(y)[i]];
    //     //     }

    //     //     return vec;

    // case MTYPE2(TYPE_CHAR, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = string(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_vector_i64(y)[i] >= xl)
    //             as_string(&vec)[i] = ' ';
    //         else
    //             as_string(&vec)[i] = as_string(x)[(i32_t)as_vector_i64(y)[i]];
    //     }

    //     return vec;

    // case MTYPE2(TYPE_LIST, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = list(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_vector_i64(y)[i] >= xl)
    //             as_list(&vec)[i] = null();
    //         else
    //             as_list(&vec)[i] = clone(&as_list(x)[(i32_t)as_vector_i64(y)[i]]);
    //     }

    //     return vec;

    // default:
    //     return error_type2(x->type, y->type, "get: unsupported types");
    // }

    return null();
}

obj_t rf_find_vector_i64_vector_i64(obj_t x, obj_t y)
{
    u64_t i, n, range, xl = x->len, yl = y->len;
    i64_t max = 0, min = 0;
    obj_t vec = vector_i64(yl), found;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y),
          *ov = as_vector_i64(vec), *fv;
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
        fv = as_vector_i64(found);

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

        drop(found);

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

obj_t rf_find(obj_t x, obj_t y)
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
        // case MTYPE2(TYPE_LIST, -TYPE_LIST):
        //     l = x->len;
        //     i = vector_find(x, y);

        //     if (i == l)
        //         return i64(NULL_I64);
        //     else
        //         return i64(i);

        //     return i64(vector_find(x, y));

        // case MTYPE2(TYPE_I64, TYPE_I64):
        //     return rf_find_vector_i64_vector_i64(x, y);

    default:
        return error_type2(x->type, y->type, "find: unsupported types");
    }
}

obj_t rf_concat(obj_t x, obj_t y)
{
    // i64_t i, xl, yl;
    // obj_t vec;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(-TYPE_BOOL, -TYPE_BOOL):
    //     vec = vector_bool(2);
    //     as_vector_bool(&vec)[0] = x->bool;
    //     as_vector_bool(&vec)[1] = y->bool;
    //     return vec;

    // case MTYPE2(-TYPE_I64, -TYPE_I64):
    //     vec = vector_i64(2);
    //     as_vector_i64(&vec)[0] = x->i64;
    //     as_vector_i64(&vec)[1] = y->i64;
    //     return vec;

    // case MTYPE2(-TYPE_F64, -TYPE_F64):
    //     vec = vector_f64(2);
    //     as_vector_f64(&vec)[0] = x->f64;
    //     as_vector_f64(&vec)[1] = y->f64;
    //     return vec;

    // case MTYPE2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
    //     vec = vector_timestamp(2);
    //     as_vector_timestamp(&vec)[0] = x->i64;
    //     as_vector_timestamp(&vec)[1] = y->i64;
    //     return vec;

    // case MTYPE2(-TYPE_GUID, -TYPE_GUID):
    //     vec = vector_guid(2);
    //     memcpy(&as_vector_guid(&vec)[0], x->guid, sizeof(guid_t));
    //     memcpy(&as_vector_guid(&vec)[1], y->guid, sizeof(guid_t));
    //     return vec;

    // case MTYPE2(-TYPE_CHAR, -TYPE_CHAR):
    //     vec = string(2);
    //     as_string(&vec)[0] = x->schar;
    //     as_string(&vec)[1] = y->schar;
    //     return vec;

    // case MTYPE2(TYPE_BOOL, -TYPE_BOOL):
    //     xl = x->len;
    //     vec = vector_bool(xl + 1);
    //     for (i = 0; i < xl; i++)
    //         as_vector_bool(&vec)[i] = as_vector_bool(x)[i];

    //     as_vector_bool(&vec)[xl] = y->bool;
    //     return vec;

    // case MTYPE2(TYPE_I64, -TYPE_I64):
    //     xl = x->len;
    //     vec = vector_i64(xl + 1);
    //     for (i = 0; i < xl; i++)
    //         as_vector_i64(&vec)[i] = as_vector_i64(x)[i];

    //     as_vector_i64(&vec)[xl] = y->i64;
    //     return vec;

    // case MTYPE2(-TYPE_I64, TYPE_I64):
    //     yl = y->len;
    //     vec = vector_i64(yl + 1);
    //     as_vector_i64(&vec)[0] = x->i64;
    //     for (i = 1; i <= yl; i++)
    //         as_vector_i64(&vec)[i] = as_vector_i64(y)[i - 1];

    //     return vec;

    // case MTYPE2(TYPE_F64, -TYPE_F64):
    //     xl = x->len;
    //     vec = vector_f64(xl + 1);
    //     for (i = 0; i < xl; i++)
    //         as_vector_f64(&vec)[i] = as_vector_f64(x)[i];

    //     as_vector_f64(&vec)[xl] = y->f64;
    //     return vec;

    // case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
    //     xl = x->len;
    //     vec = vector_timestamp(xl + 1);
    //     for (i = 0; i < xl; i++)
    //         as_vector_timestamp(&vec)[i] = as_vector_timestamp(x)[i];

    //     as_vector_timestamp(&vec)[xl] = y->i64;
    //     return vec;

    // case MTYPE2(TYPE_GUID, -TYPE_GUID):
    //     xl = x->len;
    //     vec = vector_guid(xl + 1);
    //     for (i = 0; i < xl; i++)
    //         as_vector_guid(&vec)[i] = as_vector_guid(x)[i];

    //     memcpy(&as_vector_guid(&vec)[xl], y->guid, sizeof(guid_t));
    //     return vec;

    // case MTYPE2(TYPE_BOOL, TYPE_BOOL):
    //     xl = x->len;
    //     yl = y->len;
    //     vec = vector_bool(xl + yl);
    //     for (i = 0; i < xl; i++)
    //         as_vector_bool(&vec)[i] = as_vector_bool(x)[i];
    //     for (i = 0; i < yl; i++)
    //         as_vector_bool(&vec)[i + xl] = as_vector_bool(y)[i];
    //     return vec;

    // case MTYPE2(TYPE_I64, TYPE_I64):
    //     xl = x->len;
    //     yl = y->len;
    //     vec = vector_i64(xl + yl);
    //     for (i = 0; i < xl; i++)
    //         as_vector_i64(&vec)[i] = as_vector_i64(x)[i];
    //     for (i = 0; i < yl; i++)
    //         as_vector_i64(&vec)[i + xl] = as_vector_i64(y)[i];
    //     return vec;

    // case MTYPE2(TYPE_F64, TYPE_F64):
    //     xl = x->len;
    //     yl = y->len;
    //     vec = vector_f64(xl + yl);
    //     for (i = 0; i < xl; i++)
    //         as_vector_f64(&vec)[i] = as_vector_f64(x)[i];
    //     for (i = 0; i < yl; i++)
    //         as_vector_f64(&vec)[i + xl] = as_vector_f64(y)[i];
    //     return vec;

    // case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
    //     xl = x->len;
    //     yl = y->len;
    //     vec = vector_timestamp(xl + yl);
    //     for (i = 0; i < xl; i++)
    //         as_vector_timestamp(&vec)[i] = as_vector_timestamp(x)[i];
    //     for (i = 0; i < yl; i++)
    //         as_vector_timestamp(&vec)[i + xl] = as_vector_timestamp(y)[i];
    //     return vec;

    // case MTYPE2(TYPE_GUID, TYPE_GUID):
    //     xl = x->len;
    //     yl = y->len;
    //     vec = vector_guid(xl + yl);
    //     for (i = 0; i < xl; i++)
    //         as_vector_guid(&vec)[i] = as_vector_guid(x)[i];
    //     for (i = 0; i < yl; i++)
    //         as_vector_guid(&vec)[i + xl] = as_vector_guid(y)[i];
    //     return vec;

    // case MTYPE2(TYPE_CHAR, TYPE_CHAR):
    //     xl = x->len;
    //     yl = y->len;
    //     vec = string(xl + yl);
    //     for (i = 0; i < xl; i++)
    //         as_string(&vec)[i] = as_string(x)[i];
    //     for (i = 0; i < yl; i++)
    //         as_string(&vec)[i + xl] = as_string(y)[i];
    //     return vec;

    // case MTYPE2(TYPE_LIST, TYPE_LIST):
    //     xl = x->len;
    //     yl = y->len;
    //     vec = list(xl + yl);
    //     for (i = 0; i < xl; i++)
    //         as_list(&vec)[i] = clone(&as_list(x)[i]);
    //     for (i = 0; i < yl; i++)
    //         as_list(&vec)[i + xl] = clone(&as_list(y)[i]);
    //     return vec;

    // default:
    //     if (x->type == TYPE_LIST)
    //     {
    //         xl = x->len;
    //         vec = list(xl + 1);
    //         for (i = 0; i < xl; i++)
    //             as_list(&vec)[i] = clone(&as_list(x)[i]);
    //         as_list(&vec)[xl] = clone(y);

    //         return vec;
    //     }
    //     if (y->type == TYPE_LIST)
    //     {
    //         yl = y->len;
    //         vec = list(yl + 1);
    //         as_list(&vec)[0] = clone(x);
    //         for (i = 0; i < yl; i++)
    //             as_list(&vec)[i + 1] = clone(&as_list(y)[i]);

    //         return vec;
    //     }

    //     return error_type2(x->type, y->type, "concat: unsupported types");
    // }

    return null();
}

obj_t rf_filter(obj_t x, obj_t y)
{
    // i32_t i, j = 0;
    // i64_t l, p = NULL_I64;
    // obj_t res, *vals, col;

    // switch (MTYPE2(x->type, y->type))
    // {

    // case MTYPE2(TYPE_BOOL, TYPE_BOOL):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

    //     l = x->len;
    //     res = vector_bool(l);
    //     for (i = 0; i < l; i++)
    //         if (as_vector_bool(y)[i])
    //             as_vector_bool(&res)[j++] = as_vector_bool(x)[i];

    //     resize(&res, j);

    //     return res;

    // case MTYPE2(TYPE_I64, TYPE_BOOL):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

    //     l = x->len;
    //     res = vector_i64(l);
    //     for (i = 0; i < l; i++)
    //         if (as_vector_bool(y)[i])
    //             as_vector_i64(&res)[j++] = as_vector_i64(x)[i];

    //     resize(&res, j);

    //     return res;

    // case MTYPE2(TYPE_SYMBOL, TYPE_BOOL):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

    //     l = x->len;
    //     res = vector_symbol(l);
    //     for (i = 0; i < l; i++)
    //         if (as_vector_bool(y)[i])
    //             as_vector_symbol(&res)[j++] = as_vector_symbol(x)[i];

    //     resize(&res, j);

    //     return res;

    // case MTYPE2(TYPE_F64, TYPE_BOOL):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

    //     l = x->len;
    //     res = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         if (as_vector_bool(y)[i])
    //             as_vector_f64(&res)[j++] = as_vector_f64(x)[i];

    //     resize(&res, j);

    //     return res;

    // case MTYPE2(TYPE_TIMESTAMP, TYPE_BOOL):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

    //     l = x->len;
    //     res = vector_timestamp(l);
    //     for (i = 0; i < l; i++)
    //         if (as_vector_bool(y)[i])
    //             as_vector_timestamp(&res)[j++] = as_vector_timestamp(x)[i];

    //     resize(&res, j);

    //     return res;

    // case MTYPE2(TYPE_GUID, TYPE_BOOL):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

    //     l = x->len;
    //     res = vector_guid(l);
    //     for (i = 0; i < l; i++)
    //         if (as_vector_bool(y)[i])
    //             as_vector_guid(&res)[j++] = as_vector_guid(x)[i];

    //     resize(&res, j);

    //     return res;

    // case MTYPE2(TYPE_CHAR, TYPE_BOOL):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

    //     l = x->len;
    //     res = string(l);
    //     for (i = 0; i < l; i++)
    //         if (as_vector_bool(y)[i])
    //             as_string(&res)[j++] = as_string(x)[i];

    //     resize(&res, j);

    //     return res;

    // case MTYPE2(TYPE_LIST, TYPE_BOOL):
    //     if (x->len != y->len)
    //         return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

    //     l = x->len;
    //     res = list(l);
    //     for (i = 0; i < l; i++)
    //         if (as_vector_bool(y)[i])
    //             as_list(&res)[j++] = clone(&as_list(x)[i]);

    //     resize(&res, j);

    //     return res;

    // case MTYPE2(TYPE_TABLE, TYPE_BOOL):
    //     vals = &as_list(x)[1];
    //     l = vals->len;
    //     res = list(l);

    //     for (i = 0; i < l; i++)
    //     {
    //         col = vector_filter(&as_list(vals)[i], as_vector_bool(y), p);
    //         p = col->len;
    //         as_list(&res)[i] = col;
    //     }

    //     return table(clone(&as_list(x)[0]), res);

    // default:
    //     return error_type2(x->type, y->type, "filter: unsupported types");
    // }

    return null();
}

obj_t rf_take(obj_t x, obj_t y)
{
    u64_t i, l, m;
    obj_t res, cols, sym, c;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(TYPE_BOOL, -TYPE_I64):
    // case MTYPE2(TYPE_I64, -TYPE_I64):
    // case MTYPE2(TYPE_F64, -TYPE_I64):
    // case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
    // case MTYPE2(TYPE_GUID, -TYPE_I64):
    // case MTYPE2(TYPE_SYMBOL, -TYPE_I64):
    //     return vector_get(x, y->i64);

    // case MTYPE2(-TYPE_I64, TYPE_I64):
    //     l = y->len;
    //     m = x->i64;
    //     res = vector_i64(m);

    //     for (i = 0; i < m; i++)
    //         as_vector_i64(&res)[i] = as_vector_i64(y)[i % l];

    //     return res;
    // case MTYPE2(-TYPE_I64, TYPE_NULL):
    //     l = x->i64;
    //     res = list(l);
    //     for (i = 0; i < l; i++)
    //         as_list(&res)[i] = *y;

    //     return res;
    // case MTYPE2(-TYPE_I64, -TYPE_I64):
    //     l = x->i64;
    //     res = vector_i64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_i64(&res)[i] = y->i64;

    //     return res;

    // case MTYPE2(-TYPE_I64, -TYPE_F64):
    //     l = x->i64;
    //     res = vector_f64(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&res)[i] = y->f64;

    //     return res;

    // case MTYPE2(-TYPE_I64, -TYPE_TIMESTAMP):
    //     l = x->i64;
    //     res = vector_timestamp(l);
    //     for (i = 0; i < l; i++)
    //         as_vector_timestamp(&res)[i] = y->i64;

    //     return res;

    // case MTYPE2(TYPE_TABLE, -TYPE_I64):
    // case MTYPE2(TYPE_TABLE, TYPE_I64):
    //     l = as_list(x)[0]->len;
    //     cols = list(l);
    //     for (i = 0; i < l; i++)
    //     {
    //         c = rf_take(&as_list(&as_list(x)[1])[i], y);

    //         if (is_atom(&c))
    //             c = rf_enlist(&c, 1);

    //         if (c.type == TYPE_ERROR)
    //         {
    //             res->len = i;
    //             drop(res);
    //             return c;
    //         }

    //         vector_write(&cols, i, c);
    //     }

    //     res = rf_table(&as_list(x)[0], &cols);
    //     drop(cols);

    //     return res;

    // case MTYPE2(TYPE_TABLE, TYPE_SYMBOL):
    //     l = y->len;
    //     cols = list(l);
    //     for (i = 0; i < l; i++)
    //     {
    //         sym = symboli64(as_vector_symbol(y)[i]);
    //         as_list(&cols)[i] = dict_get(x, &sym);
    //     }

    //     res = rf_table(y, &cols);
    //     drop(cols);

    //     return res;

    // case MTYPE2(TYPE_I64, TYPE_I64):
    //     l = y->len;
    //     res = vector_i64(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_i64(&res)[i] = vector_get(x, as_vector_i64(y)[i]).i64;

    //     return res;

    // case MTYPE2(TYPE_SYMBOL, TYPE_I64):
    //     l = y->len;
    //     res = vector_symbol(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_symbol(&res)[i] = vector_get(x, as_vector_i64(y)[i]).i64;

    //     return res;

    // case MTYPE2(TYPE_F64, TYPE_I64):
    //     l = y->len;
    //     res = vector_f64(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_f64(&res)[i] = vector_get(x, as_vector_i64(y)[i]).f64;

    //     return res;

    // case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
    //     l = y->len;
    //     res = vector_timestamp(l);

    //     for (i = 0; i < l; i++)
    //         as_vector_timestamp(&res)[i] = vector_get(x, as_vector_i64(y)[i]).i64;

    //     return res;

    // case MTYPE2(TYPE_LIST, TYPE_I64):
    //     l = y->len;
    //     res = list(l);

    //     for (i = 0; i < l; i++)
    //         as_list(&res)[i] = clone(&as_list(x)[as_vector_i64(y)[i]]);

    //     return res;

    // default:
    //     return error_type2(x->type, y->type, "take: unsupported types");
    // }

    return null();
}

obj_t rf_in(obj_t x, obj_t y)
{
    i64_t i, xl, yl;
    obj_t vec;
    set_t *set;

    // switch
    //     MTYPE2(x->type, y->type)
    //     {
    //     case MTYPE2(TYPE_I64, TYPE_I64):
    //     case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
    //         xl = x->len;
    //         yl = y->len;
    //         set = set_new(yl, &rfi_i64_hash, &i64_cmp);

    //         for (i = 0; i < yl; i++)
    //             set_insert(set, as_vector_i64(y)[i]);

    //         vec = vector_bool(xl);

    //         for (i = 0; i < xl; i++)
    //             as_vector_bool(&vec)[i] = set_contains(set, as_vector_i64(x)[i]);

    //         set_free(set);

    //         return vec;

    //     default:
    //         return error_type2(x->type, y->type, "in: unsupported types");
    //     }

    return null();
}

obj_t rf_sect(obj_t x, obj_t y)
{
    obj_t mask, res;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(TYPE_I64, TYPE_I64):
    // case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
    //     mask = rf_in(x, y);
    //     res = rf_filter(x, &mask);
    //     drop(mask);
    //     return res;
    // default:
    //     return error_type2(x->type, y->type, "sect: unsupported types");
    // }

    return null();
}

obj_t rf_except(obj_t x, obj_t y)
{
    i64_t i, j = 0, l;
    obj_t mask, res;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(TYPE_I64, -TYPE_I64):
    // case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
    //     l = x->len;
    //     res = vector(x->type, l);

    //     for (i = 0; i < l; i++)
    //     {
    //         if (as_vector_i64(x)[i] != y->i64)
    //             as_vector_i64(&res)[j++] = as_vector_i64(x)[i];
    //     }

    //     resize(&res, j);

    //     return res;
    // case MTYPE2(TYPE_I64, TYPE_I64):
    // case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
    //     mask = rf_in(x, y);
    //     mask = rf_not(&mask);
    //     res = rf_filter(x, &mask);
    //     drop(mask);
    //     return res;
    // default:
    //     return error_type2(x->type, y->type, "except: unsupported types");
    // }

    return null();
}

obj_t rf_group_Table(obj_t x, obj_t y)
{
    // i64_t i, l;
    // obj_t res;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(TYPE_TABLE, TYPE_LIST):
    //     l = as_list(x)[1]->len;
    //     res = list(l);
    //     for (i = 0; i < l; i++)
    //         as_list(&res)[i] = rf_call_binary_right_atomic(rf_take, &as_list(&as_list(x)[1])[i], y);

    //     return table(clone(&as_list(x)[0]), res);

    // default:
    //     return error_type2(x->type, y->type, "group: unsupported types");
    // }

    return null();
}

obj_t rf_xasc(obj_t x, obj_t y)
{
    // obj_t idx, col, res;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
    //     col = dict_get(x, y);

    //     if (col.type == TYPE_ERROR)
    //         return col;

    //     idx = rf_iasc(&col);
    //     drop(col);

    //     if (idx.type == TYPE_ERROR)
    //         return idx;

    //     res = rf_take(x, &idx);

    //     drop(idx);

    //     return res;
    // default:
    //     return error_type2(x->type, y->type, "xasc: unsupported types");
    // }

    return null();
}

obj_t rf_xdesc(obj_t x, obj_t y)
{
    // obj_t idx, col, res;

    // switch (MTYPE2(x->type, y->type))
    // {
    // case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
    //     col = dict_get(x, y);

    //     if (col.type == TYPE_ERROR)
    //         return col;

    //     idx = rf_idesc(&col);
    //     drop(col);

    //     if (idx.type == TYPE_ERROR)
    //         return idx;

    //     res = rf_take(x, &idx);

    //     drop(idx);

    //     return res;
    // default:
    //     return error_type2(x->type, y->type, "xdesc: unsupported types");
    // }

    return null();
}