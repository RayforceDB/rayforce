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

#include "unary.h"
#include "runtime.h"
#include "heap.h"
#include "vm.h"
#include "ops.h"
#include "util.h"
#include "format.h"
#include "sort.h"
#include "guid.h"
#include "set.h"
#include "env.h"
#include "group.h"
#include "parse.h"
#include "fs.h"
#include "cc.h"
#include "util.h"

// Atomic unary functions (iterates through list of argumen items down to atoms)
obj_t rf_call_unary_atomic(unary_t f, obj_t x)
{
    u64_t i, l;
    obj_t res = NULL, item = NULL;

    // argument is a list, so iterate through it
    if (x->type == TYPE_LIST)
    {
        l = x->len;

        if (l == 0)
            return error(ERR_TYPE, "empty list");

        item = rf_call_unary_atomic(f, as_list(x)[0]); // call function with first item

        if (item->type == TYPE_ERROR)
            return item;

        // probably we can fold it in a vector if all other values will be of the same type
        if (is_atom(item))
            res = vector(-item->type, l);
        else
            res = list(l);

        write_obj(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            item = rf_call_unary_atomic(f, as_list(x)[i]);

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

    return f(x);
}

obj_t rf_call_unary(u8_t attrs, unary_t f, obj_t x)
{
    switch (attrs)
    {
    case FLAG_ATOMIC:
        return rf_call_unary_atomic(f, x);
    default:
        return f(x);
    }
}

obj_t rf_get(obj_t x)
{
    if (!x || x->type != -TYPE_SYMBOL)
        raise(ERR_TYPE, "get: expected symbol");

    obj_t val = at_obj(runtime_get()->env.variables, x);

    if (is_null(val))
        raise(ERR_NOT_EXIST, "variable '%s' does not exist", symbols_get(x->i64));

    return val;
}

obj_t rf_type(obj_t x)
{
    type_t type;
    if (!x)
        type = -TYPE_ERROR;
    else
        type = x->type;

    i64_t t = env_get_typename_by_type(&runtime_get()->env, type);
    return symboli64(t);
}

obj_t rf_count(obj_t x)
{
    if (!x)
        return i64(0);

    switch (x->type)
    {
    case TYPE_TABLE:
    case TYPE_DICT:
        return i64(as_list(as_list(x)[1])[0]->len);
    default:
        return i64(x->len);
    }
}

obj_t rf_til(obj_t x)
{
    if (mtype(x->type) != mtype(-TYPE_I64))
        return error(ERR_TYPE, "til: expected i64");

    i32_t i, l = (i32_t)x->i64;
    obj_t vec = NULL;

    vec = vector_i64(l);

    for (i = 0; i < l; i++)
        as_vector_i64(vec)[i] = i;

    vec->attrs = ATTR_ASC | ATTR_WITHOUT_NULLS | ATTR_DISTINCT;

    return vec;
}

obj_t rf_distinct(obj_t x)
{
    obj_t res = NULL;

    switch (mtype(x->type))
    {
    case mtype(TYPE_I64):
        return rf_distinct_vector_i64(x);
    case mtype(TYPE_SYMBOL):
        res = rf_distinct_vector_i64(x);
        res->type = TYPE_SYMBOL;
        return res;
    default:
        raise(ERR_TYPE, "distinct: expected vector_i64");
    }
}

obj_t rf_group(obj_t x)
{
    if (mtype(x->type) != mtype(TYPE_I64))
        raise(ERR_TYPE, "group: expected vector_i64");

    return rf_group_vector_i64(x);
}

obj_t rf_sum(obj_t x)
{
    i32_t i;
    i64_t l, isum = 0;
    f64_t fsum = 0.0;

    switch (mtype(x->type))
    {
    case mtype(-TYPE_I64):
        return clone(x);
    case mtype(-TYPE_F64):
        return clone(x);
    case mtype(TYPE_I64):
        l = x->len;
        if (x->attrs & ATTR_WITHOUT_NULLS)
        {
            for (i = 0; i < l; i++)
                isum += as_vector_i64(x)[i];
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                if (as_vector_i64(x)[i] ^ NULL_I64)
                    isum += as_vector_i64(x)[i];
            }
        }

        return i64(isum);

    case mtype(TYPE_F64):
        l = x->len;
        for (i = 0; i < l; i++)
            fsum += as_vector_f64(x)[i];

        return f64(fsum);

    default:
        raise(ERR_TYPE, "sum: unsupported type: %d", x->type);
    }
}

obj_t rf_avg(obj_t x)
{
    i32_t i;
    i64_t l, isum, *iv, n = 0;
    f64_t fsum, *fv;

    switch (mtype(x->type))
    {
    case mtype(TYPE_I64):
        l = x->len;
        iv = as_vector_i64(x);
        isum = 0;
        // vectorized version when we exactly know that there are no nulls
        if (x->attrs & ATTR_WITHOUT_NULLS)
        {
            for (i = 0; i < l; i++)
                isum += iv[i];
        }
        else
        {
            // atom version
            for (i = 0; i < l; i++)
            {
                if (iv[i] ^ NULL_I64)
                    isum += iv[i];
                else
                    n++;
            }
        }
        return f64((f64_t)isum / (l - n));

    case mtype(TYPE_F64):
        l = x->len;
        fv = as_vector_f64(x);
        fsum = 0;
        for (i = 0; i < l; i++)
            fsum += fv[i];

        return f64(fsum / l);

    default:
        raise(ERR_TYPE, "avg: unsupported type: %d", x->type);
    }
}

obj_t rf_min(obj_t x)
{
    i32_t i;
    i64_t l, imin, *iv;
    f64_t fmin, *fv;

    switch (mtype(x->type))
    {
    case mtype(TYPE_I64):
        l = x->len;

        if (!l)
            return i64(NULL_I64);

        iv = as_vector_i64(x);
        imin = iv[0];
        // vectorized version when we exactly know that there are no nulls
        if (x->attrs & ATTR_WITHOUT_NULLS)
        {
            if (x->attrs & ATTR_ASC)
                return i64(iv[0]);
            if (x->attrs & ATTR_DESC)
                return i64(iv[l - 1]);
            imin = iv[0];
            for (i = 1; i < l; i++)
                if (iv[i] < imin)
                    imin = iv[i];

            return i64(imin);
        }

        // atom version
        // find first nonnull value
        for (i = 0; i < l; i++)
            if (iv[i] ^ NULL_I64)
            {
                imin = iv[i];
                break;
            }

        for (i = 0; i < l; i++)
        {
            if (iv[i] ^ NULL_I64)
                imin = iv[i] < imin ? iv[i] : imin;
        }

        return i64(imin);

    case mtype(TYPE_F64):
        l = x->len;

        if (!l)
            return f64(NULL_F64);

        fv = as_vector_f64(x);
        fmin = fv[0];
        // vectorized version when we exactly know that there are no nulls
        if (x->attrs & ATTR_WITHOUT_NULLS)
        {
            if (x->attrs & ATTR_ASC)
                return f64(fv[0]);
            if (x->attrs & ATTR_DESC)
                return f64(fv[l - 1]);
            fmin = fv[0];
            for (i = 1; i < l; i++)
                if (fv[i] < fmin)
                    fmin = fv[i];

            return f64(fmin);
        }

        for (i = 0; i < l; i++)
            fmin = fv[i] < fmin ? fv[i] : fmin;

        return f64(fmin);

    default:
        raise(ERR_TYPE, "min: unsupported type: %d", x->type);
    }
}

obj_t rf_max(obj_t x)
{
    i32_t i;
    i64_t l, imax, *iv;

    switch (mtype(x->type))
    {
    case mtype(TYPE_I64):
        l = x->len;

        if (!l)
            return i64(NULL_I64);

        iv = as_vector_i64(x);
        imax = iv[0];
        // vectorized version when we exactly know that there are no nulls
        if (x->attrs & ATTR_WITHOUT_NULLS)
        {
            if (x->attrs & ATTR_ASC)
                return i64(iv[l - 1]);
            if (x->attrs & ATTR_DESC)
                return i64(iv[0]);
            imax = iv[0];
            for (i = 1; i < l; i++)
                if (iv[i] > imax)
                    imax = iv[i];

            return i64(imax);
        }

        // atom version
        // find first nonnull value
        for (i = 0; i < l; i++)
            if (iv[i] ^ NULL_I64)
            {
                imax = iv[i];
                break;
            }

        for (i = 0; i < l; i++)
        {
            if (iv[i] ^ NULL_I64)
                imax = iv[i] > imax ? iv[i] : imax;
        }

        return i64(imax);

    default:
        raise(ERR_TYPE, "max: unsupported type: %d", x->type);
    }
}

obj_t rf_not(obj_t x)
{
    i32_t i;
    i64_t l;
    obj_t res;

    switch (mtype(x->type))
    {
    case mtype(-TYPE_BOOL):
        return bool(!x->bool);

    case mtype(TYPE_BOOL):
        l = x->len;
        res = vector_bool(l);
        for (i = 0; i < l; i++)
            as_vector_bool(res)[i] = !as_vector_bool(x)[i];

        return res;

    default:
        raise(ERR_TYPE, "not: unsupported type: %d", x->type);
    }
}

obj_t rf_iasc(obj_t x)
{
    switch (mtype(x->type))
    {
    case mtype(TYPE_I64):
        return rf_sort_asc(x);

    default:
        raise(ERR_TYPE, "iasc: unsupported type: %d", x->type);
    }
}

obj_t rf_idesc(obj_t x)
{
    switch (mtype(x->type))
    {
    case mtype(TYPE_I64):
        return rf_sort_desc(x);

    default:
        raise(ERR_TYPE, "idesc: unsupported type: %d", x->type);
    }
}

obj_t rf_asc(obj_t x)
{
    obj_t idx = rf_sort_asc(x);
    i64_t l, i;

    switch (mtype(x->type))
    {
    case mtype(TYPE_I64):
        l = x->len;
        for (i = 0; i < l; i++)
            as_vector_i64(idx)[i] = as_vector_i64(x)[as_vector_i64(idx)[i]];

        idx->attrs |= ATTR_ASC;

        return idx;

    default:
        raise(ERR_TYPE, "asc: unsupported type: %d", x->type);
    }
}

obj_t rf_desc(obj_t x)
{
    obj_t idx = rf_sort_desc(x);
    i64_t l, i;

    switch (mtype(x->type))
    {
    case mtype(TYPE_I64):
        l = x->len;
        for (i = 0; i < l; i++)
            as_vector_i64(idx)[i] = as_vector_i64(x)[as_vector_i64(idx)[i]];

        idx->attrs |= ATTR_DESC;

        return idx;

    default:
        raise(ERR_TYPE, "desc: unsupported type: %d", x->type);
    }
}

obj_t rf_guid_generate(obj_t x)
{
    i64_t i, count;
    obj_t vec;
    guid_t *g;

    switch (mtype(x->type))
    {
    case mtype(-TYPE_I64):
        count = x->i64;
        vec = vector_guid(count);
        g = as_vector_guid(vec);

        for (i = 0; i < count; i++)
            guid_generate(g + i);

        return vec;

    default:
        raise(ERR_TYPE, "guid_generate: unsupported type: %d", x->type);
    }
}

obj_t rf_neg(obj_t x)
{
    obj_t res;
    i64_t i, l;

    switch (mtype(x->type))
    {
    case mtype(-TYPE_BOOL):
        return i64(-x->bool);
    case mtype(-TYPE_I64):
        return i64(-x->i64);
    case mtype(-TYPE_F64):
        return f64(-x->f64);
    case mtype(TYPE_I64):
        l = x->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(res)[i] = -as_vector_i64(x)[i];
        return res;
    case mtype(TYPE_F64):
        l = x->len;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(res)[i] = -as_vector_f64(x)[i];
        return res;

    default:
        raise(ERR_TYPE, "neg: unsupported type: %d", x->type);
    }
}

obj_t rf_where(obj_t x)
{
    i32_t i, j = 0;
    i64_t l, *ov;
    bool_t *iv;
    obj_t res;

    switch (mtype(x->type))
    {
    case mtype(TYPE_BOOL):
        l = x->len;
        iv = as_vector_bool(x);
        res = vector_i64(l);
        ov = as_vector_i64(res);
        for (i = 0; i < l; i++)
            if (iv[i])
                ov[j++] = i;

        resize(&res, j);

        return res;

    default:
        raise(ERR_TYPE, "where: unsupported type: %d", x->type);
    }
}

obj_t rf_key(obj_t x)
{
    switch (mtype(x->type))
    {
    case mtype(TYPE_TABLE):
    case mtype(TYPE_DICT):
        return clone(&as_list(x)[0]);
    default:
        return clone(x);
    }
}

obj_t rf_value(obj_t x)
{
    switch (mtype(x->type))
    {
    case mtype(TYPE_TABLE):
    case mtype(TYPE_DICT):
        return clone(&as_list(x)[1]);
    default:
        return clone(x);
    }
}

obj_t rf_fread(obj_t x)
{
    i64_t fd, size;
    str_t fmsg;
    obj_t res, err;

    switch (mtype(x->type))
    {
    case mtype(TYPE_CHAR):
        fd = fs_fopen(as_string(x), O_RDONLY);

        // error handling if file does not exist
        if (fd == -1)
        {
            fmsg = str_fmt(0, "file: '%s' does not exist", as_string(x));
            err = error(ERR_NOT_EXIST, fmsg);
            heap_free(fmsg);
            // err->id = x->id;
            return err;
        }

        size = fs_fsize(fd);

        res = string(size);

        if (read(fd, as_string(res), size) != size)
        {
            fmsg = str_fmt(0, "file: '%s' read error", as_string(x));
            err = error(ERR_IO, fmsg);
            heap_free(fmsg);
            close(fd);
            // err->id = x->id;
            return err;
        }

        fs_fclose(fd);

        return res;
    default:
        raise(ERR_TYPE, "fread: unsupported type: %d", x->type);
    }
}

obj_t rf_parse(obj_t x)
{
    parser_t parser;
    obj_t res;

    switch (mtype(x->type))
    {
    case mtype(TYPE_CHAR):
        parser = parser_new();
        res = parse(&parser, "top-level", as_string(x));
        parser_free(&parser);
        return res;
    default:
        raise(ERR_TYPE, "parse: unsupported type: %d", x->type);
    }
}

obj_t rf_read_parse_compile(obj_t x)
{
    parser_t parser;
    obj_t red, par, com;

    switch (mtype(x->type))
    {
    case mtype(TYPE_CHAR):
        red = rf_fread(x);
        if (red->type == TYPE_ERROR)
            return red;

        parser = parser_new();
        par = parse(&parser, as_string(x), as_string(red));
        drop(red);

        if (par->type == TYPE_ERROR)
        {
            print_error(par, as_string(x), as_string(red), red->len);
            parser_free(&parser);
            return par;
        }

        com = cc_compile_lambda(false, as_string(x), vector_symbol(0), par, &parser.nfo);
        drop(par);
        parser_free(&parser);

        return com;
    default:
        raise(ERR_TYPE, "read_parse_compile: unsupported type: %d", x->type);
    }
}
