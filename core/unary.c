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
#include "dict.h"
#include "runtime.h"
#include "heap.h"
#include "vm.h"
#include "ops.h"
#include "util.h"
#include "format.h"
#include "sort.h"
#include "vector.h"
#include "guid.h"
#include "set.h"
#include "env.h"
#include "group.h"
#include "parse.h"
#include "fs.h"
#include "cc.h"

// Atomic unary functions (iterates through list of argumen items down to atoms)
obj_t rf_call_unary_atomic(unary_t f, obj_t x)
{
    i64_t i, l;
    obj_t res = NULL, item = NULL;

    // argument is a list, so iterate through it
    if (x->type == TYPE_LIST)
    {
        l = x->len;

        if (l == 0)
            return error(ERR_TYPE, "empty list");

        item = rf_call_unary_atomic(f, as_list(x)); // call function with first item

        if (item->type == TYPE_ERROR)
            return item;

        // probably we can fold it in a vector if all other values will be of the same type
        if (is_scalar(item))
            res = vector(-item->type, l);
        else
            res = list(l);

        vector_write(res, 0, item);
        drop(item);

        for (i = 1; i < l; i++)
        {
            item = rf_call_unary_atomic(f, &as_list(x)[i]);

            if (item->type == TYPE_ERROR)
            {
                res->len = i;
                drop(res);
                return item;
            }

            vector_write(&res, i, item);
        }

        return res;
    }

    return f(x);
}

obj_t rf_call_unary(u8_t flags, unary_t f, obj_t x)
{
    switch (flags)
    {
    case FLAG_ATOMIC:
        return rf_call_unary_atomic(f, x);
    default:
        return f(x);
    }
}

obj_t rf_get_variable(obj_t x)
{
    obj_t v = dict_get(&runtime_get()->env.variables, x);
    if (is_null(v))
        return error(ERR_NOT_FOUND, "symbol not found");

    return v;
}

obj_t rf_type(obj_t x)
{
    i64_t t = env_get_typename_by_type(&runtime_get()->env, x->type);
    return symboli64(t);
}

obj_t rf_count(obj_t x)
{
    if (is_vector(x))
        return i64(x->len);

    switch (x->type)
    {
    case TYPE_TABLE:
        return i64(as_list(as_list(x)[1])[0]->len);
    default:
        return i64(1);
    }
}

obj_t rf_til(obj_t x)
{
    if (MTYPE(x->type) != MTYPE(-TYPE_I64))
        return error(ERR_TYPE, "til: expected i64");

    i32_t i, l = (i32_t)x->i64;
    i64_t *v;
    obj_t vec = NULL;

    vec = vector_i64(l);

    v = as_vector_i64(vec);

    for (i = 0; i < l; i++)
        v[i] = i;

    vec->flags = VEC_ATTR_ASC | VEC_ATTR_WITHOUT_NULLS | VEC_ATTR_DISTINCT;
    return vec;
}

obj_t rf_distinct(obj_t x)
{
    obj_t res = NULL;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        return rf_distinct_vector_i64(x);
    case MTYPE(TYPE_SYMBOL):
        res = rf_distinct_vector_i64(x);
        res->type = TYPE_SYMBOL;
        return res;
    default:
        return error(ERR_TYPE, "distinct: expected vector_i64");
    }
}

obj_t rf_group(obj_t x)
{
    if (MTYPE(x->type) != MTYPE(TYPE_I64))
        return error(ERR_TYPE, "group: expected vector_i64");

    return rf_group_vector_i64(x);
}

obj_t rf_sum(obj_t x)
{
    i32_t i;
    i64_t l, isum = 0, *iv;
    f64_t fsum = 0.0, *fv;

    switch (MTYPE(x->type))
    {
    case MTYPE(-TYPE_I64):
        return clone(x);
    case MTYPE(-TYPE_F64):
        return clone(x);
    case MTYPE(TYPE_I64):
        l = x->len;
        iv = as_vector_i64(x);
        if (x->flags & VEC_ATTR_WITHOUT_NULLS)
        {
            for (i = 0; i < l; i++)
                isum += iv[i];
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                if (iv[i] ^ NULL_vector_i64)
                    isum += iv[i];
            }
        }

        return i64(isum);

    case MTYPE(TYPE_F64):
        l = x->len;
        fv = as_vector_f64(x);
        for (i = 0; i < l; i++)
            fsum += fv[i];

        return f64(fsum);

    default:
        return error_type1(x->type, "sum: unsupported type");
    }
}

obj_t rf_avg(obj_t x)
{
    i32_t i;
    i64_t l, isum, *iv, n = 0;
    f64_t fsum, *fv;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        l = x->len;
        iv = as_vector_i64(x);
        isum = 0;
        // vectorized version when we exactly know that there are no nulls
        if (x->flags & VEC_ATTR_WITHOUT_NULLS)
        {
            for (i = 0; i < l; i++)
                isum += iv[i];
        }
        else
        {
            // scalar version
            for (i = 0; i < l; i++)
            {
                if (iv[i] ^ NULL_vector_i64)
                    isum += iv[i];
                else
                    n++;
            }
        }
        return f64((f64_t)isum / (l - n));

    case MTYPE(TYPE_F64):
        l = x->len;
        fv = as_vector_f64(x);
        fsum = 0;
        for (i = 0; i < l; i++)
            fsum += fv[i];

        return f64(fsum / l);

    default:
        return error_type1(x->type, "avg: unsupported type");
    }
}

obj_t rf_min(obj_t x)
{
    i32_t i;
    i64_t l, imin, *iv;
    f64_t fmin, *fv;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        l = x->len;

        if (!l)
            return i64(NULL_vector_i64);

        iv = as_vector_i64(x);
        imin = iv[0];
        // vectorized version when we exactly know that there are no nulls
        if (x->flags & VEC_ATTR_WITHOUT_NULLS)
        {
            if (x->flags & VEC_ATTR_ASC)
                return i64(iv[0]);
            if (x->flags & VEC_ATTR_DESC)
                return i64(iv[l - 1]);
            imin = iv[0];
            for (i = 1; i < l; i++)
                if (iv[i] < imin)
                    imin = iv[i];

            return i64(imin);
        }

        // scalar version
        // find first nonnull value
        for (i = 0; i < l; i++)
            if (iv[i] ^ NULL_vector_i64)
            {
                imin = iv[i];
                break;
            }

        for (i = 0; i < l; i++)
        {
            if (iv[i] ^ NULL_vector_i64)
                imin = iv[i] < imin ? iv[i] : imin;
        }

        return i64(imin);

    case MTYPE(TYPE_F64):
        l = x->len;

        if (!l)
            return f64(NULL_vector_f64);

        fv = as_vector_f64(x);
        fmin = fv[0];
        // vectorized version when we exactly know that there are no nulls
        if (x->flags & VEC_ATTR_WITHOUT_NULLS)
        {
            if (x->flags & VEC_ATTR_ASC)
                return f64(fv[0]);
            if (x->flags & VEC_ATTR_DESC)
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
        return error_type1(x->type, "min: unsupported type");
    }
}

obj_t rf_max(obj_t x)
{
    i32_t i;
    i64_t l, imax, *iv;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        l = x->len;

        if (!l)
            return i64(NULL_vector_i64);

        iv = as_vector_i64(x);
        imax = iv[0];
        // vectorized version when we exactly know that there are no nulls
        if (x->flags & VEC_ATTR_WITHOUT_NULLS)
        {
            if (x->flags & VEC_ATTR_ASC)
                return i64(iv[l - 1]);
            if (x->flags & VEC_ATTR_DESC)
                return i64(iv[0]);
            imax = iv[0];
            for (i = 1; i < l; i++)
                if (iv[i] > imax)
                    imax = iv[i];

            return i64(imax);
        }

        // scalar version
        // find first nonnull value
        for (i = 0; i < l; i++)
            if (iv[i] ^ NULL_vector_i64)
            {
                imax = iv[i];
                break;
            }

        for (i = 0; i < l; i++)
        {
            if (iv[i] ^ NULL_vector_i64)
                imax = iv[i] > imax ? iv[i] : imax;
        }

        return i64(imax);

    default:
        return error_type1(x->type, "max: unsupported type");
    }
}

obj_t rf_not(obj_t x)
{
    i32_t i;
    i64_t l;
    obj_t res;

    switch (MTYPE(x->type))
    {
    case MTYPE(-TYPE_BOOL):
        return bool(!x->bool);

    case MTYPE(TYPE_BOOL):
        l = x->len;
        res = vector_bool(l);
        for (i = 0; i < l; i++)
            as_vector_bool(res)[i] = !as_vector_bool(x)[i];

        return res;

    default:
        return error_type1(x->type, "not: unsupported type");
    }
}

obj_t rf_iasc(obj_t x)
{
    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        return rf_sort_asc(x);

    default:
        return error_type1(x->type, "iasc: unsupported type");
    }
}

obj_trf_idesc(obj_t x)
{
    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        return rf_sort_desc(x);

    default:
        return error_type1(x->type, "idesc: unsupported type");
    }
}

obj_t rf_asc(obj_t x)
{
    obj_t idx = rf_sort_asc(x);
    i64_t l, i;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        l = x->len;
        for (i = 0; i < l; i++)
            as_vector_i64(idx)[i] = as_vector_i64(x)[as_vector_i64(idx)[i]];

        idx->flags |= VEC_ATTR_ASC;

        return idx;

    default:
        return error_type1(x->type, "asc: unsupported type");
    }
}

obj_t rf_desc(obj_t x)
{
    obj_t idx = rf_sort_desc(x);
    i64_t l, i;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        l = x->len;
        for (i = 0; i < l; i++)
            as_vector_i64(idx)[i] = as_vector_i64(x)[as_vector_i64(idx)[i]];

        idx->flags |= VEC_ATTR_DESC;

        return idx;

    default:
        return error_type1(x->type, "desc: unsupported type");
    }
}

obj_t rf_guid_generate(obj_t x)
{
    i64_t i, count;
    obj_t vec;
    guid_t *g;

    switch (MTYPE(x->type))
    {
    case MTYPE(-TYPE_I64):
        count = x->i64;
        vec = vector_guid(count);
        g = as_vector_guid(vec);

        for (i = 0; i < count; i++)
            guid_generate(g + i);

        return vec;

    default:
        return error_type1(x->type, "guid_generate: unsupported type");
    }
}

obj_t rf_neg(obj_t x)
{
    obj_t res;
    i64_t i, l;

    switch (MTYPE(x->type))
    {
    case MTYPE(-TYPE_BOOL):
        return i64(-x->bool);
    case MTYPE(-TYPE_I64):
        return i64(-x->i64);
    case MTYPE(-TYPE_F64):
        return f64(-x->f64);
    case MTYPE(TYPE_I64):
        l = x->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(res)[i] = -as_vector_i64(x)[i];
        return res;
    case MTYPE(TYPE_F64):
        l = x->len;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(res)[i] = -as_vector_f64(x)[i];
        return res;

    default:
        return error_type1(x->type, "neg: unsupported type");
    }
}

obj_trf_where(obj_t x)
{
    i32_t i, j = 0;
    i64_t l, *ov;
    bool_t *iv;
    obj_t res;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_BOOL):
        l = x->len;
        iv = as_vector_bool(x);
        res = vector_i64(l);
        ov = as_vector_i64(res);
        for (i = 0; i < l; i++)
            if (iv[i])
                ov[j++] = i;

        vector_shrink(&res, j);

        return res;

    default:
        return error_type1(x->type, "where: unsupported type");
    }
}

obj_t rf_key(obj_t x)
{
    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_TABLE):
    case MTYPE(TYPE_DICT):
        return clone(&as_list(x)[0]);
    default:
        return clone(x);
    }
}

obj_trf_value(obj_t x)
{
    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_TABLE):
    case MTYPE(TYPE_DICT):
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

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_CHAR):
        fd = fs_fopen(as_string(x), O_RDONLY);

        // error handling if file does not exist
        if (fd == -1)
        {
            fmsg = str_fmt(0, "file: '%s' does not exist", as_string(x));
            err = error(ERR_NOT_EXIST, fmsg);
            heap_free(fmsg);
            err->id = x->id;
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
            err->id = x->id;
            return err;
        }

        fs_fclose(fd);

        return res;
    default:
        return error_type1(x->type, "fread: unsupported type");
    }
}

obj_t rf_parse(obj_t x)
{
    parser_t parser;
    obj_t res;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_CHAR):
        parser = parser_new();
        res = parse(&parser, "top-level", as_string(x));
        parser_free(&parser);
        return res;
    default:
        return error_type1(x->type, "parse: unsupported type");
    }
}

obj_t rf_read_parse_compile(obj_t x)
{
    parser_t parser;
    obj_t red, par, com;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_CHAR):
        red = rf_fread(x);
        if (red->type == TYPE_ERROR)
            return red;

        parser = parser_new();
        par = parse(&parser, as_string(x), as_string(red));
        drop(&red);

        if (par->type == TYPE_ERROR)
        {
            print_error(&par, as_string(x), as_string(red), red->len);
            parser_free(&parser);
            return par;
        }

        com = cc_compile_lambda(false, as_string(x), vector_symbol(0),
                                as_list(par), par->id, par->len, &parser.debuginfo);
        drop(&par);
        parser_free(&parser);

        return com;
    default:
        return error_type1(x->type, "read parse compile: unsupported type");
    }
}
