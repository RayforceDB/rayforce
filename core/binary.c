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
#include <errno.h>
#include "runtime.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "util.h"
#include "util.h"
#include "format.h"
#include "hash.h"
#include "fs.h"
#include "serde.h"

obj_t call_binary(binary_f f, obj_t x, obj_t y)
{
    obj_t cst, res;
    bool_t dropx = false, dropy = false;

    if (x->type == TYPE_ENUM)
    {
        x = rf_value(x);
        if (is_error(x))
            return x;

        dropx = true;
    }

    if (y->type == TYPE_ENUM)
    {
        y = rf_value(y);
        if (is_error(y))
        {
            if (dropx)
                drop(x);
            return y;
        }

        dropy = true;
    }

    // no need to cast
    if (x->type == y->type || x->type == -y->type)
    {
        res = f(x, y);
        goto call;
    }

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_F64):
        cst = f64(x->i64);
        res = f(cst, y);
        drop(cst);
        goto call;
    case mtype2(-TYPE_F64, -TYPE_I64):
        cst = f64(y->i64);
        res = f(x, cst);
        drop(cst);
        goto call;
    case mtype2(-TYPE_I64, TYPE_F64):
        cst = f64(x->i64);
        res = f(cst, y);
        drop(cst);
        goto call;
    case mtype2(-TYPE_F64, TYPE_I64):
        cst = cast(TYPE_F64, y);
        res = f(x, cst);
        drop(cst);
        goto call;
    case mtype2(TYPE_I64, -TYPE_F64):
        cst = cast(TYPE_I64, x);
        res = f(y, cst);
        drop(cst);
        goto call;
    case mtype2(TYPE_F64, -TYPE_I64):
        cst = f64(y->i64);
        res = f(x, cst);
        drop(cst);
        goto call;
    case mtype2(TYPE_I64, TYPE_F64):
        cst = cast(TYPE_F64, x);
        res = f(cst, y);
        drop(cst);
        goto call;
    case mtype2(TYPE_F64, TYPE_I64):
        cst = cast(TYPE_F64, y);
        res = f(cst, x);
        drop(cst);
        goto call;
    default:
        res = f(x, y);
    }

call:
    if (dropx)
        drop(x);
    if (dropy)
        drop(y);

    return res;
}

obj_t rf_call_binary_left_atomic(binary_f f, obj_t x, obj_t y)
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

        res = vector(item->type, l);

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

obj_t rf_call_binary_right_atomic(binary_f f, obj_t x, obj_t y)
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

        res = vector(item->type, l);

        write_obj(&res, 0, item);

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
obj_t rf_call_binary_atomic(binary_f f, obj_t x, obj_t y)
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

        if (is_error(item))
            return item;

        res = vector(item->type, l);

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

        res = vector(item->type, l);

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

        res = vector(item->type, l);

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

obj_t rf_call_binary(u8_t attrs, binary_f f, obj_t x, obj_t y)
{
    switch (attrs)
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

obj_t rf_distinctn(obj_t *x, u64_t n)
{
    i64_t r, p, min, max, k, w, b;
    u64_t i, j, h, l, range;
    obj_t mask, vec, set, a;

    if (n == 0 || !*x)
        return vector_i64(0);

    a = *x;

    // check all types (must be the same)
    if (a->type != TYPE_I64 && a->type != TYPE_SYMBOL && a->type != TYPE_F64 && a->type != TYPE_TIMESTAMP)
        raise(ERR_TYPE, "'distinct': unsupported type: %d", a->type);

    for (i = 0; i < n; i++)
        if (a->type != x[i]->type)
            raise(ERR_TYPE, "'distinct': all arguments must be of the same type");

    if (n == 1 && (a->attrs & ATTR_DISTINCT))
        return clone(a);

    min = max = as_i64(a)[0];

    // find min and max of all vectors
    for (i = 0; i < n; i++)
    {
        a = *(x + i);

        l = a->len;

        for (j = 0; j < l; j++)
        {
            if (as_i64(a)[j] < min)
                min = as_i64(a)[j];
            else if (as_i64(a)[j] > max)
                max = as_i64(a)[j];
        }
    }

    range = max - min + 1;

    if (range <= l)
    {
        r = alignup(range / 8, 8);
        if (!r)
            r = 1;

        mask = vector_bool(r);
        memset(as_bool(mask), 0, r);
        vec = vector_i64(range);

        for (i = 0; i < n; i++)
        {
            for (j = 0, h = 0; j < l; j++)
            {
                k = as_i64(a)[j] - min;
                w = k >> 3; // k / 8
                b = k & 7;  // k % 8

                if (as_bool(mask)[w] & (1 << b))
                    continue;

                as_bool(mask)[w] |= (1 << b);
                as_i64(vec)[h++] = as_i64(a)[j];
            }
        }

        drop(mask);

        resize(&vec, h);

        return vec;
    }

    set = ht_tab(l, -1);
    vec = vector_i64(l);

    for (i = 0; i < n; i++)
    {
        for (j = 0, h = 0; j < l; j++)
        {
            p = ht_tab_next(&set, as_i64(a)[j] - min);
            if (as_i64(as_list(set)[0])[p] == NULL_I64)
            {
                as_i64(as_list(set)[0])[p] = as_i64(a)[j] - min;
                as_i64(vec)[h++] = as_i64(a)[j];
            }
        }
    }

    vec->attrs |= ATTR_DISTINCT;

    resize(&vec, h);
    drop(set);

    return vec;
}

obj_t rf_set(obj_t x, obj_t y)
{
    obj_t res, col, s;
    i64_t fd, c = 0;
    u64_t i, l, size;

    switch (x->type)
    {
    case -TYPE_SYMBOL:
        res = set_obj(&runtime_get()->env.variables, x, clone(y));

        if (res->type == TYPE_ERROR)
            return res;

        return clone(y);

    case TYPE_CHAR:
        if (x->len == 0)
            raise(ERR_LENGTH, "set: empty string path");

        switch (y->type)
        {
        case TYPE_TABLE:
            if (as_string(x)[x->len - 1] != '/')
                raise(ERR_TYPE, "set: table path must be a directory");

            // save columns schema
            s = string_from_str(".d", 2);
            col = rf_concat(x, s);
            res = rf_save(col, as_list(y)[0]);

            drop(s);
            drop(col);

            if (is_error(res))
                return res;

            drop(res);

            // save columns data
            for (i = 0, l = as_list(y)[0]->len; i < l; i++)
            {
                s = cast(TYPE_CHAR, at_idx(as_list(y)[0], i));
                col = rf_concat(x, s);
                res = rf_set(col, at_idx(as_list(y)[1], i));
                drop(s);
                drop(col);

                if (is_error(res))
                    return res;
            }

            return clone(y);

        default:
            fd = fs_fopen(as_string(x), ATTR_WRONLY | ATTR_CREAT | ATTR_TRUNC);

            if (fd == -1)
                raise(ERR_IO, "set: failed to open file '%s': %s", as_string(x), get_os_error());

            size = size_of(y);

            c = fs_fwrite(fd, (str_t)y, size);
            fs_fclose(fd);

            if (c == -1)
                raise(ERR_IO, "set: failed to write to file '%s': %s", as_string(x), get_os_error());

            return clone(y);
        }

    default:
        raise(ERR_TYPE, "set: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_save(obj_t x, obj_t y)
{
    i64_t fd, c;
    obj_t buf;

    fd = fs_fopen(as_string(x), ATTR_RDWR | ATTR_CREAT | ATTR_TRUNC);

    if (fd == -1)
        raise(ERR_IO, "write: failed to open file '%s': %s", as_string(x), get_os_error());

    buf = ser(y);

    if (is_error(buf))
    {
        fs_fclose(fd);
        return buf;
    }

    c = fs_fwrite(fd, as_byte(buf), buf->len);
    fs_fclose(fd);
    drop(buf);

    if (c == -1)
        raise(ERR_IO, "write: failed to write to file '%s': %s", as_string(x), get_os_error());

    return clone(y);
}

obj_t rf_write(obj_t x, obj_t y)
{
    unused(x);
    unused(y);
    raise(ERR_NOT_IMPLEMENTED, "write: not implemented");
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

obj_t rf_dict(obj_t x, obj_t y)
{
    if (!is_vector(x) || !is_vector(y))
        return error(ERR_TYPE, "Keys and Values must be lists");

    if (x->len != y->len)
        return error(ERR_LENGTH, "Keys and Values must have the same length");

    return dict(clone(x), clone(y));
}

obj_t rf_table(obj_t x, obj_t y)
{
    bool_t s = false;
    u64_t i, j, len, cl = 0;
    obj_t lst, c, l = null(0);

    if (x->type != TYPE_SYMBOL)
        return error(ERR_TYPE, "Keys must be a symbol vector");

    if (y->type != TYPE_LIST)
    {
        if (x->len != 1)
            return error(ERR_LENGTH, "Keys and Values must have the same length");

        l = list(1);
        as_list(l)[0] = clone(y);
        y = l;
    }

    if (x->len != y->len)
    {
        drop(l);
        return error(ERR_LENGTH, "Keys and Values must have the same length");
    }

    len = y->len;

    for (i = 0; i < len; i++)
    {
        switch (as_list(y)[i]->type)
        {
        // case TYPE_NULL:
        case -TYPE_BOOL:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_CHAR:
        case -TYPE_SYMBOL:
        case -TYPE_TIMESTAMP:
            s = true;
            break;
        case TYPE_BOOL:
        case TYPE_I64:
        case TYPE_F64:
        case TYPE_TIMESTAMP:
        case TYPE_CHAR:
        case TYPE_SYMBOL:
        case TYPE_LIST:
            j = as_list(y)[i]->len;
            if (cl != 0 && j != cl)
                return error(ERR_LENGTH, "Values must be of the same length");

            cl = j;
            break;
        case TYPE_ENUM:
            j = as_list(as_list(y)[i])[1]->len;
            if (cl != 0 && j != cl)
                return error(ERR_LENGTH, "Values must be of the same length");

            cl = j;
            break;
        default:
            return error(ERR_TYPE, "Unsupported type in a Values list");
        }
    }

    // there are no atoms and all columns are of the same length
    if (!s)
    {
        drop(l);
        return table(clone(x), clone(y));
    }

    // otherwise we need to expand atoms to vectors
    lst = list(len);

    if (cl == 0)
        cl = 1;

    for (i = 0; i < len; i++)
    {
        switch (as_list(y)[i]->type)
        {
        // case TYPE_NULL:
        case -TYPE_BOOL:
        case -TYPE_I64:
        case -TYPE_F64:
        case -TYPE_CHAR:
        case -TYPE_SYMBOL:
            c = i64(cl);
            as_list(lst)[i] = rf_take(c, as_list(y)[i]);
            break;
        default:
            as_list(lst)[i] = clone(as_list(y)[i]);
        }
    }

    drop(l);

    return table(clone(x), lst);
}

obj_t rf_rand(obj_t x, obj_t y)
{
    i64_t i, count;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        count = x->i64;
        vec = vector_i64(count);

        for (i = 0; i < count; i++)
            as_i64(vec)[i] = rfi_rand_u64() % y->i64;

        return vec;

    default:
        raise(ERR_TYPE, "rand: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_add(obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return i64(addi64(x->i64, y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return f64(addf64(x->f64, y->f64));

    case mtype2(-TYPE_TIMESTAMP, -TYPE_I64):
        return timestamp(addi64(x->i64, y->i64));

    case mtype2(-TYPE_I64, TYPE_I64):
        l = y->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(x->i64, as_i64(y)[i]);

        return vec;

    case mtype2(-TYPE_F64, TYPE_F64):
        l = y->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = addf64(x->f64, as_f64(y)[i]);

        return vec;

    case mtype2(-TYPE_TIMESTAMP, TYPE_I64):
        l = y->len;
        vec = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(x->i64, as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
        l = x->len;
        vec = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_TIMESTAMP, TYPE_I64):
        l = x->len;
        vec = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        if (l != y->len)
            return error(ERR_LENGTH, "add: vectors must be of the same length");
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = addi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = addf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        if (l != y->len)
            return error(ERR_LENGTH, "add: vectors must be of the same length");
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = addf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "add: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_sub(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (i64(subi64(x->i64, y->i64)));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (f64(subf64(x->f64, y->f64)));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = subi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = subi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = subf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = subf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "sub: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_mul(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (i64(muli64(x->i64, y->i64)));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (f64(mulf64(x->f64, y->f64)));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = muli64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = muli64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = mulf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = mulf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "mul: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_div(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (i64(divi64(x->i64, y->i64)));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (f64(divf64(x->f64, y->f64)));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = divi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = divi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = divf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = divf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "div: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_fdiv(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (f64(fdivi64(x->i64, y->i64)));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (f64(fdivf64(x->f64, y->f64)));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = fdivf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "fdiv: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_mod(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (i64(modi64(x->i64, y->i64)));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (f64(modf64(x->f64, y->f64)));

    case mtype2(TYPE_I64, -TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = modi64(as_i64(x)[i], y->i64);

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        l = x->len;
        vec = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(vec)[i] = modi64(as_i64(x)[i], as_i64(y)[i]);

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = modf64(as_f64(x)[i], y->f64);

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        l = x->len;
        vec = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(vec)[i] = modf64(as_f64(x)[i], as_f64(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "mod: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_like(obj_t x, obj_t y)
{
    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_CHAR, TYPE_CHAR):
        return (bool(str_match(as_string(x), as_string(y))));

    default:
        raise(ERR_TYPE, "like: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_eq(obj_t x, obj_t y)
{
    i64_t i, l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_BOOL, TYPE_BOOL):
        return (bool(x->bool == y->bool));

    case mtype2(-TYPE_I64, -TYPE_I64):
    case mtype2(-TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        return (bool(x->i64 == y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 == y->f64));

    case mtype2(TYPE_I64, -TYPE_I64):
    case mtype2(TYPE_SYMBOL, -TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = as_i64(x)[i] == y->i64;

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = as_f64(x)[i] == y->f64;

        return vec;

    case mtype2(-TYPE_I64, TYPE_I64):
    case mtype2(-TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(-TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        l = y->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = x->i64 == as_i64(y)[i];

        return vec;

    case mtype2(-TYPE_F64, TYPE_F64):
        l = y->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = x->f64 == as_f64(y)[i];

        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        if (x->len != y->len)
            return error(ERR_LENGTH, "eq: vectors of different length");

        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = as_i64(x)[i] == as_i64(y)[i];

        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        if (x->len != y->len)
            return error(ERR_LENGTH, "eq: vectors of different length");

        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = as_f64(x)[i] == as_f64(y)[i];

        return vec;

    case mtype2(TYPE_LIST, TYPE_LIST):
        if (x->len != y->len)
            return error(ERR_LENGTH, "eq: lists of different length");

        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = equal(as_list(x)[i], as_list(y)[i]);

        return vec;

    default:
        raise(ERR_TYPE, "eq: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_ne(obj_t x, obj_t y)
{
    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_BOOL, TYPE_BOOL):
        return (bool(x->bool != y->bool));

    case mtype2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 != y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 != y->f64));

    default:
        raise(ERR_TYPE, "ne: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_lt(obj_t x, obj_t y)
{
    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 < y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 < y->f64));

    default:
        raise(ERR_TYPE, "lt: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_le(obj_t x, obj_t y)
{
    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 <= y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 <= y->f64));

    default:
        raise(ERR_TYPE, "le: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_gt(obj_t x, obj_t y)
{
    i64_t i, l;
    obj_t vec;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 > y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 > y->f64));

    case mtype2(TYPE_I64, TYPE_I64):
        if (x->len != y->len)
            return error(ERR_LENGTH, "gt: vectors of different length");

        l = x->len;
        vec = vector_bool(l);

        for (i = 0; i < l; i++)
            as_bool(vec)[i] = as_i64(x)[i] > as_i64(y)[i];

        return vec;

    default:
        raise(ERR_TYPE, "gt: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_ge(obj_t x, obj_t y)
{
    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_I64, -TYPE_I64):
        return (bool(x->i64 >= y->i64));

    case mtype2(-TYPE_F64, -TYPE_F64):
        return (bool(x->f64 >= y->f64));

    default:
        raise(ERR_TYPE, "ge: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_and(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_BOOL, -TYPE_BOOL):
        return (bool(x->bool && y->bool));

    case mtype2(TYPE_BOOL, TYPE_BOOL):
        l = x->len;
        res = vector_bool(x->len);
        for (i = 0; i < l; i++)
            as_bool(res)[i] = as_bool(x)[i] & as_bool(y)[i];

        return res;

    default:
        raise(ERR_TYPE, "and: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_or(obj_t x, obj_t y)
{
    i32_t i;
    i64_t l;
    obj_t res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_BOOL, -TYPE_BOOL):
        return (bool(x->bool || y->bool));

    case mtype2(TYPE_BOOL, TYPE_BOOL):
        l = x->len;
        res = vector_bool(x->len);
        for (i = 0; i < l; i++)
            as_bool(res)[i] = as_bool(x)[i] | as_bool(y)[i];

        return res;

    default:
        raise(ERR_TYPE, "or: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_at(obj_t x, obj_t y)
{
    unused(x);
    unused(y);
    // i32_t i;
    // i64_t yl, xl;
    // obj_t vec;

    // switch (mtype2(x->type, y->type))
    // {
    // case mtype2(TYPE_BOOL, -TYPE_I64):
    // case mtype2(TYPE_I64, -TYPE_I64):
    // case mtype2(TYPE_F64, -TYPE_I64):
    // case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
    // case mtype2(TYPE_GUID, -TYPE_I64):
    // case mtype2(TYPE_CHAR, -TYPE_I64):
    // case mtype2(TYPE_LIST, -TYPE_I64):
    //     return vector_get(x, y->i64);

    // case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
    //     return dict_get(x, y);

    // case mtype2(TYPE_BOOL, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = vector_bool(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_bool(y)[i] >= xl)
    //             as_bool(vec)[i] = false;
    //         else
    //             as_bool(vec)[i] = as_bool(x)[(i32_t)as_bool(y)[i]];
    //     }

    //     return vec;

    // case mtype2(TYPE_I64, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = vector_i64(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_i64(y)[i] >= xl)
    //             as_i64(vec)[i] = NULL_I64;
    //         else
    //             as_i64(vec)[i] = as_i64(x)[(i32_t)as_i64(y)[i]];
    //     }

    //     return vec;

    // case mtype2(TYPE_F64, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = vector_f64(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_i64(y)[i] >= xl)
    //             as_f64(vec)[i] = NULL_F64;
    //         else
    //             as_f64(vec)[i] = as_f64(x)[(i32_t)as_i64(y)[i]];
    //     }

    //     return vec;

    // case mtype2(TYPE_TIMESTAMP, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = vector_timestamp(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_i64(y)[i] >= xl)
    //             as_timestamp(vec)[i] = NULL_I64;
    //         else
    //             as_timestamp(vec)[i] = as_timestamp(x)[(i32_t)as_i64(y)[i]];
    //     }

    //     return vec;

    //     // case mtype2(TYPE_GUID, TYPE_I64):
    //     //     yl = y->len;
    //     //     xl = x->len;
    //     //     vec = vector_guid(yl);
    //     //     for (i = 0; i < yl; i++)
    //     //     {
    //     //         if (as_i64(y)[i] >= xl)
    //     //             as_guid(vec)[i] = guid(NULL_GUID);
    //     //         else
    //     //             as_guid(vec)[i] = as_guid(x)[(i32_t)as_i64(y)[i]];
    //     //     }

    //     //     return vec;

    // case mtype2(TYPE_CHAR, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = string(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_i64(y)[i] >= xl)
    //             as_string(vec)[i] = ' ';
    //         else
    //             as_string(vec)[i] = as_string(x)[(i32_t)as_i64(y)[i]];
    //     }

    //     return vec;

    // case mtype2(TYPE_LIST, TYPE_I64):
    //     yl = y->len;
    //     xl = x->len;
    //     vec = list(yl);
    //     for (i = 0; i < yl; i++)
    //     {
    //         if (as_i64(y)[i] >= xl)
    //             as_list(vec)[i] = null(0);
    //         else
    //             as_list(vec)[i] = clone(as_list(x)[(i32_t)as_i64(y)[i]]);
    //     }

    //     return vec;

    // default:
    //     return error_type2(x->type, y->type, "get: unsupported types");
    // }

    return null(0);
}

obj_t rf_find_vector_i64_vector_i64(obj_t x, obj_t y)
{
    u64_t i, n, range, xl = x->len, yl = y->len;
    i64_t max = 0, min = 0, p;
    obj_t vec, found, ht;

    for (i = 0; i < xl; i++)
    {
        if (as_i64(x)[i] > max)
            max = as_i64(x)[i];
        else if (as_i64(x)[i] < min)
            min = as_i64(x)[i];
    }

    vec = vector_i64(yl);

    range = max - min + 1;

    if (range <= yl)
    {
        found = vector_i64(range);

        for (i = 0; i < xl; i++)
            as_i64(found)[i] = NULL_I64;

        for (i = 0; i < xl; i++)
        {
            n = as_i64(x)[i] - min;
            if (as_i64(found)[n] == NULL_I64)
                as_i64(found)[n] = i;
        }

        for (i = 0; i < yl; i++)
        {
            n = as_i64(y)[i] - min;
            if (as_i64(y)[i] < min || as_i64(y)[i] > max)
                as_i64(vec)[i] = NULL_I64;
            else
                as_i64(vec)[i] = as_i64(found)[n];
        }

        drop(found);

        return vec;
    }

    // otherwise, use a hash table
    ht = ht_tab(xl, TYPE_I64);

    for (i = 0; i < xl; i++)
    {
        p = ht_tab_next(&ht, as_i64(x)[i] - min);
        if (as_i64(as_list(ht)[0])[p] == NULL_I64)
        {
            as_i64(as_list(ht)[0])[p] = as_i64(x)[i] - min;
            as_i64(as_list(ht)[1])[p] = i;
        }
    }

    for (i = 0; i < yl; i++)
    {
        p = ht_tab_next(&ht, as_i64(y)[i] - min);
        if (as_i64(as_list(ht)[0])[p] == NULL_I64)
            as_i64(vec)[i] = NULL_I64;
        else
            as_i64(vec)[i] = as_i64(as_list(ht)[1])[p] + min;
    }

    drop(ht);

    return vec;
}

obj_t rf_find(obj_t x, obj_t y)
{
    u64_t l, i;

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
        return rf_find_vector_i64_vector_i64(x, y);

    default:
        raise(ERR_TYPE, "find: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_concat(obj_t x, obj_t y)
{
    i64_t i, xl, yl;
    obj_t vec;

    if (!x || !y)
        return list(2, clone(x), clone(y));

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_BOOL, -TYPE_BOOL):
        vec = vector_bool(2);
        as_bool(vec)[0] = x->bool;
        as_bool(vec)[1] = y->bool;
        return vec;

    case mtype2(-TYPE_I64, -TYPE_I64):
        vec = vector_i64(2);
        as_i64(vec)[0] = x->i64;
        as_i64(vec)[1] = y->i64;
        return vec;

    case mtype2(-TYPE_F64, -TYPE_F64):
        vec = vector_f64(2);
        as_f64(vec)[0] = x->f64;
        as_f64(vec)[1] = y->f64;
        return vec;

    case mtype2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        vec = vector_timestamp(2);
        as_timestamp(vec)[0] = x->i64;
        as_timestamp(vec)[1] = y->i64;
        return vec;

        // case mtype2(-TYPE_GUID, -TYPE_GUID):
        //     vec = vector_guid(2);
        //     memcpy(&as_guid(vec)[0], x->guid, sizeof(guid_t));
        //     memcpy(&as_guid(vec)[1], y->guid, sizeof(guid_t));
        //     return vec;

    case mtype2(-TYPE_CHAR, -TYPE_CHAR):
        vec = string(2);
        as_string(vec)[0] = x->vchar;
        as_string(vec)[1] = y->vchar;
        return vec;

    case mtype2(TYPE_BOOL, -TYPE_BOOL):
        xl = x->len;
        vec = vector_bool(xl + 1);
        for (i = 0; i < xl; i++)
            as_bool(vec)[i] = as_bool(x)[i];

        as_bool(vec)[xl] = y->bool;
        return vec;

    case mtype2(TYPE_I64, -TYPE_I64):
        xl = x->len;
        vec = vector_i64(xl + 1);
        for (i = 0; i < xl; i++)
            as_i64(vec)[i] = as_i64(x)[i];

        as_i64(vec)[xl] = y->i64;
        return vec;

    case mtype2(-TYPE_I64, TYPE_I64):
        yl = y->len;
        vec = vector_i64(yl + 1);
        as_i64(vec)[0] = x->i64;
        for (i = 1; i <= yl; i++)
            as_i64(vec)[i] = as_i64(y)[i - 1];

        return vec;

    case mtype2(TYPE_F64, -TYPE_F64):
        xl = x->len;
        vec = vector_f64(xl + 1);
        for (i = 0; i < xl; i++)
            as_f64(vec)[i] = as_f64(x)[i];

        as_f64(vec)[xl] = y->f64;
        return vec;

    case mtype2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        xl = x->len;
        vec = vector_timestamp(xl + 1);
        for (i = 0; i < xl; i++)
            as_timestamp(vec)[i] = as_timestamp(x)[i];

        as_timestamp(vec)[xl] = y->i64;
        return vec;

        // case mtype2(TYPE_GUID, -TYPE_GUID):
        //     xl = x->len;
        //     vec = vector_guid(xl + 1);
        //     for (i = 0; i < xl; i++)
        //         as_guid(vec)[i] = as_guid(x)[i];

        //     memcpy(&as_guid(vec)[xl], y->guid, sizeof(guid_t));
        //     return vec;

    case mtype2(TYPE_BOOL, TYPE_BOOL):
        xl = x->len;
        yl = y->len;
        vec = vector_bool(xl + yl);
        for (i = 0; i < xl; i++)
            as_bool(vec)[i] = as_bool(x)[i];
        for (i = 0; i < yl; i++)
            as_bool(vec)[i + xl] = as_bool(y)[i];
        return vec;

    case mtype2(TYPE_I64, TYPE_I64):
        xl = x->len;
        yl = y->len;
        vec = vector_i64(xl + yl);
        for (i = 0; i < xl; i++)
            as_i64(vec)[i] = as_i64(x)[i];
        for (i = 0; i < yl; i++)
            as_i64(vec)[i + xl] = as_i64(y)[i];
        return vec;

    case mtype2(TYPE_F64, TYPE_F64):
        xl = x->len;
        yl = y->len;
        vec = vector_f64(xl + yl);
        for (i = 0; i < xl; i++)
            as_f64(vec)[i] = as_f64(x)[i];
        for (i = 0; i < yl; i++)
            as_f64(vec)[i + xl] = as_f64(y)[i];
        return vec;

    case mtype2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
        xl = x->len;
        yl = y->len;
        vec = vector_timestamp(xl + yl);
        for (i = 0; i < xl; i++)
            as_timestamp(vec)[i] = as_timestamp(x)[i];
        for (i = 0; i < yl; i++)
            as_timestamp(vec)[i + xl] = as_timestamp(y)[i];
        return vec;

    case mtype2(TYPE_GUID, TYPE_GUID):
        xl = x->len;
        yl = y->len;
        vec = vector_guid(xl + yl);
        for (i = 0; i < xl; i++)
            as_guid(vec)[i] = as_guid(x)[i];
        for (i = 0; i < yl; i++)
            as_guid(vec)[i + xl] = as_guid(y)[i];
        return vec;

    case mtype2(TYPE_CHAR, TYPE_CHAR):
        xl = x->len;
        yl = y->len;
        vec = string(xl + yl);
        for (i = 0; i < xl; i++)
            as_string(vec)[i] = as_string(x)[i];
        for (i = 0; i < yl; i++)
            as_string(vec)[i + xl] = as_string(y)[i];
        return vec;

    case mtype2(TYPE_LIST, TYPE_LIST):
        xl = x->len;
        yl = y->len;
        vec = list(xl + yl);
        for (i = 0; i < xl; i++)
            as_list(vec)[i] = clone(as_list(x)[i]);
        for (i = 0; i < yl; i++)
            as_list(vec)[i + xl] = clone(as_list(y)[i]);
        return vec;

    default:
        if (x->type == TYPE_LIST)
        {
            xl = x->len;
            vec = list(xl + 1);
            for (i = 0; i < xl; i++)
                as_list(vec)[i] = clone(as_list(x)[i]);
            as_list(vec)[xl] = clone(y);

            return vec;
        }
        if (y->type == TYPE_LIST)
        {
            yl = y->len;
            vec = list(yl + 1);
            as_list(vec)[0] = clone(x);
            for (i = 0; i < yl; i++)
                as_list(vec)[i + 1] = clone(as_list(y)[i]);

            return vec;
        }

        raise(ERR_TYPE, "join: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_filter(obj_t x, obj_t y)
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
                as_guid(res)[j++] = as_guid(x)[i];

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
            col = rf_filter(as_list(vals)[i], y);
            as_list(res)[i] = col;
        }

        return table(clone(as_list(x)[0]), res);

    default:
        raise(ERR_TYPE, "filter: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_take(obj_t x, obj_t y)
{
    u64_t i, l, m;
    obj_t res;

    switch (mtype2(x->type, y->type))
    {
        // case mtype2(TYPE_BOOL, -TYPE_I64):
        // case mtype2(TYPE_I64, -TYPE_I64):
        // case mtype2(TYPE_F64, -TYPE_I64):
        // case mtype2(TYPE_TIMESTAMP, -TYPE_I64):
        // case mtype2(TYPE_GUID, -TYPE_I64):
        // case mtype2(TYPE_SYMBOL, -TYPE_I64):
        //     return vector_get(x, y->i64);

    case mtype2(-TYPE_I64, TYPE_I64):
        l = y->len;
        m = x->i64;
        res = vector_i64(m);

        for (i = 0; i < m; i++)
            as_i64(res)[i] = as_i64(y)[i % l];

        return res;
        // case mtype2(-TYPE_I64, TYPE_NULL):
        //     l = x->i64;
        //     res = list(l);
        //     for (i = 0; i < l; i++)
        //         as_list(res)[i] = *y;

        return res;
    case mtype2(-TYPE_I64, -TYPE_I64):
        l = x->i64;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_i64(res)[i] = y->i64;

        return res;

    case mtype2(-TYPE_I64, -TYPE_F64):
        l = x->i64;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            as_f64(res)[i] = y->f64;

        return res;

    case mtype2(-TYPE_I64, -TYPE_TIMESTAMP):
        l = x->i64;
        res = vector_timestamp(l);
        for (i = 0; i < l; i++)
            as_timestamp(res)[i] = y->i64;

        return res;

    case mtype2(TYPE_TABLE, -TYPE_I64):
        // case mtype2(TYPE_TABLE, TYPE_I64):
        //     l = as_list(x)[0]->len;
        //     cols = list(l);
        //     for (i = 0; i < l; i++)
        //     {
        //         c = rf_take(as_list(as_list(x)[1])[i], y);

        //         if (is_atom(c))
        //             c = rf_enlist(c, 1);

        //         if (is_error(c))
        //         {
        //             res->len = i;
        //             drop(res);
        //             return c;
        //         }

        //         write_obj(&cols, i, c);
        //     }

        //     res = rf_table(as_list(x)[0], cols);

        //     return res;

        // case mtype2(TYPE_TABLE, TYPE_SYMBOL):
        //     l = y->len;
        //     cols = list(l);
        //     for (i = 0; i < l; i++)
        //     {
        //         sym = symboli64(as_symbol(y)[i]);
        //         as_list(&cols)[i] = at_obj(x, sym);
        //     }

        //     res = rf_table(y, &cols);
        //     drop(cols);

        //     return res;

        // case mtype2(TYPE_I64, TYPE_I64):
        //     l = y->len;
        //     res = vector_i64(l);

        //     for (i = 0; i < l; i++)
        //         as_i64(res)[i] = vector_get(x, as_i64(y)[i]).i64;

        //     return res;

        // case mtype2(TYPE_SYMBOL, TYPE_I64):
        //     l = y->len;
        //     res = vector_symbol(l);

        //     for (i = 0; i < l; i++)
        //         as_symbol(res)[i] = vector_get(x, as_i64(y)[i]).i64;

        //     return res;

        // case mtype2(TYPE_F64, TYPE_I64):
        //     l = y->len;
        //     res = vector_f64(l);

        //     for (i = 0; i < l; i++)
        //         as_f64(res)[i] = vector_get(x, as_i64(y)[i]).f64;

        //     return res;

        // case mtype2(TYPE_TIMESTAMP, TYPE_I64):
        //     l = y->len;
        //     res = vector_timestamp(l);

        //     for (i = 0; i < l; i++)
        //         as_timestamp(res)[i] = vector_get(x, as_i64(y)[i]).i64;

        //     return res;

    case mtype2(TYPE_LIST, TYPE_I64):
        l = y->len;
        res = list(l);

        for (i = 0; i < l; i++)
            as_list(res)[i] = clone(as_list(x)[as_i64(y)[i]]);

        return res;

    default:
        raise(ERR_TYPE, "take: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_in(obj_t x, obj_t y)
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
                // p = ht_tab_next_with(&set, as_i64(y)[i], &rfi_i64_hash, &i64_cmp);
                p = ht_tab_next(&set, as_i64(y)[i]);
                if (as_i64(as_list(set)[0])[p] == NULL_I64)
                    as_i64(as_list(set)[0])[p] = as_i64(y)[i];
            }

            vec = vector_bool(xl);

            for (i = 0; i < xl; i++)
            {
                // p = ht_tab_next_with(&set, as_i64(x)[i], &rfi_i64_hash, &i64_cmp);
                p = ht_tab_get(set, as_i64(x)[i]);
                as_bool(vec)[i] = (p != NULL_I64);
            }

            drop(set);

            return vec;

        default:
            raise(ERR_TYPE, "in: unsupported types: %d %d", x->type, y->type);
        }

    return null(0);
}

obj_t rf_sect(obj_t x, obj_t y)
{
    obj_t mask, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_I64, TYPE_I64):
    case mtype2(TYPE_SYMBOL, TYPE_SYMBOL):
        mask = rf_in(x, y);
        res = rf_filter(x, mask);
        drop(mask);
        return res;

    default:
        raise(ERR_TYPE, "sect: unsupported types: %d %d", x->type, y->type);
    }

    return null(0);
}

obj_t rf_except(obj_t x, obj_t y)
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
        mask = rf_in(x, y);
        nmask = rf_not(mask);
        drop(mask);
        res = rf_filter(x, nmask);
        drop(nmask);
        return res;
    default:
        raise(ERR_TYPE, "except: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_group_Table(obj_t x, obj_t y)
{
    i64_t i, l;
    obj_t res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_TABLE, TYPE_LIST):
        l = as_list(x)[1]->len;
        res = list(l);
        for (i = 0; i < l; i++)
            as_list(res)[i] = rf_call_binary_right_atomic(rf_take, as_list(as_list(x)[1])[i], y);

        return table(clone(as_list(x)[0]), res);

    default:
        raise(ERR_TYPE, "group: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_xasc(obj_t x, obj_t y)
{
    obj_t idx, col, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        col = at_obj(x, y);

        if (is_error(col))
            return col;

        idx = rf_iasc(col);
        drop(col);

        if (is_error(idx))
            return idx;

        res = rf_take(x, idx);

        drop(idx);

        return res;
    default:
        raise(ERR_TYPE, "xasc: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_xdesc(obj_t x, obj_t y)
{
    obj_t idx, col, res;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(TYPE_TABLE, -TYPE_SYMBOL):
        col = at_obj(x, y);

        if (is_error(col))
            return col;

        idx = rf_idesc(col);
        drop(col);

        if (is_error(idx))
            return idx;

        res = rf_take(x, idx);

        drop(idx);

        return res;
    default:
        raise(ERR_TYPE, "xdesc: unsupported types: %d %d", x->type, y->type);
    }
}

obj_t rf_enum(obj_t x, obj_t y)
{
    obj_t s, v;

    switch (mtype2(x->type, y->type))
    {
    case mtype2(-TYPE_SYMBOL, TYPE_SYMBOL):
        s = rf_get(x);

        if (is_error(s))
            return s;

        if (!s || s->type != TYPE_SYMBOL)
            raise(ERR_TYPE, "enum: expected vector symbol");

        v = rf_find(s, y);
        drop(s);

        return aenum(clone(x), v);
    default:
        raise(ERR_TYPE, "enum: unsupported types: %d %d", x->type, y->type);
    }
}