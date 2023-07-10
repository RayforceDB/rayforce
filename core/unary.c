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
#include "alloc.h"
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

// Atomic unary functions (iterates through list of argumen items down to atoms)
rf_object_t rf_call_unary_atomic(unary_t f, rf_object_t *x)
{
    i64_t i, l;
    rf_object_t res, item;

    // argument is a list, so iterate through it
    if (x->type == TYPE_LIST)
    {
        l = x->adt->len;
        if (l == 0)
            return error(ERR_TYPE, "empty list");

        item = rf_call_unary_atomic(f, as_list(x)); // call function with first item

        if (item.type == TYPE_ERROR)
            return item;

        // probably we can fold it in a vector if all other values will be of the same type
        if (item.type < 0)
            res = vector(-item.type, l);
        else
            res = list(l);

        vector_set(&res, 0, item);

        for (i = 1; i < l; i++)
        {
            item = rf_call_unary_atomic(f, &as_list(x)[i]);
            if (item.type == TYPE_ERROR)
            {
                rf_object_free(&res);
                return item;
            }
            vector_set(&res, i, item);
        }

        return res;
    }

    return f(x);
}

rf_object_t rf_get_variable(rf_object_t *x)
{
    rf_object_t v = dict_get(&runtime_get()->env.variables, x);
    if (is_null(&v))
        return error(ERR_NOT_FOUND, "not found");

    return v;
}

rf_object_t rf_type(rf_object_t *x)
{
    i64_t t = env_get_typename_by_type(&runtime_get()->env, x->type);
    return symboli64(t);
}

rf_object_t rf_count(rf_object_t *x)
{
    if (!is_vector(x))
        return i64(1);

    switch (x->type)
    {
    case TYPE_LAMBDA:
        return i64(1);
    case TYPE_TABLE:
        return i64((&as_list(&as_list(x)[1])[0])->adt->len);
    default:
        return i64(x->adt->len);
    }
}

rf_object_t rf_til(rf_object_t *x)
{
    if (MTYPE(x->type) != MTYPE(-TYPE_I64))
        return error(ERR_TYPE, "til: expected i64");

    i32_t i, l = (i32_t)x->i64;
    i64_t *v;
    rf_object_t vec;

    vec = vector_i64(l);

    v = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        v[i] = i;

    vec.adt->attrs = VEC_ATTR_ASC | VEC_ATTR_WITHOUT_NULLS | VEC_ATTR_DISTINCT;
    return vec;
}

rf_object_t rf_distinct(rf_object_t *x)
{
    if (MTYPE(x->type) != MTYPE(TYPE_I64))
        return error(ERR_TYPE, "distinct: expected I64");

    return rf_distinct_I64(x);
}

rf_object_t rf_group(rf_object_t *x)
{
    if (MTYPE(x->type) != MTYPE(TYPE_I64))
        return error(ERR_TYPE, "group: expected I64");

    return rf_group_I64(x);
}

rf_object_t rf_sum(rf_object_t *x)
{
    i32_t i;
    i64_t l, isum = 0, *iv;
    f64_t fsum = 0.0, *fv;
    rf_object_t res, v;

    switch (MTYPE(x->type))
    {
    case MTYPE(-TYPE_I64):
        return *x;
    case MTYPE(-TYPE_F64):
        return *x;
    case MTYPE(TYPE_I64):
        l = x->adt->len;
        iv = as_vector_i64(x);
        if (x->adt->attrs & VEC_ATTR_WITHOUT_NULLS)
        {
            for (i = 0; i < l; i++)
                isum += iv[i];
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                if (iv[i] ^ NULL_I64)
                    isum += iv[i];
            }
        }

        return i64(isum);

    case MTYPE(TYPE_F64):
        l = x->adt->len;
        fv = as_vector_f64(x);
        for (i = 0; i < l; i++)
            fsum += fv[i];

        return f64(fsum);

    default:
        return error_type1(x->type, "sum: unsupported type");
    }
}

rf_object_t rf_avg(rf_object_t *x)
{
    i32_t i;
    i64_t l, isum, *iv, n = 0;
    f64_t fsum, *fv;
    rf_object_t res, v;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        l = x->adt->len;
        iv = as_vector_i64(x);
        isum = 0;
        // vectorized version when we exactly know that there are no nulls
        if (x->adt->attrs & VEC_ATTR_WITHOUT_NULLS)
        {
            for (i = 0; i < l; i++)
                isum += iv[i];
        }
        else
        {
            // scalar version
            for (i = 0; i < l; i++)
            {
                if (iv[i] ^ NULL_I64)
                    isum += iv[i];
                else
                    n++;
            }
        }
        return f64((f64_t)isum / (l - n));

    case MTYPE(TYPE_F64):
        l = x->adt->len;
        fv = as_vector_f64(x);
        fsum = 0;
        for (i = 0; i < l; i++)
            fsum += fv[i];

        return f64(fsum / l);

    default:
        return error_type1(x->type, "avg: unsupported type");
    }
}

rf_object_t rf_min(rf_object_t *x)
{
    i32_t i;
    i64_t l, imin, *iv;
    f64_t fmin, *fv;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        l = x->adt->len;

        if (!l)
            return i64(NULL_I64);

        iv = as_vector_i64(x);
        imin = iv[0];
        // vectorized version when we exactly know that there are no nulls
        if (x->adt->attrs & VEC_ATTR_WITHOUT_NULLS)
        {
            if (x->adt->attrs & VEC_ATTR_ASC)
                return i64(iv[0]);
            if (x->adt->attrs & VEC_ATTR_DESC)
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

    case MTYPE(TYPE_F64):
        l = x->adt->len;

        if (!l)
            return f64(NULL_F64);

        fv = as_vector_f64(x);
        fmin = fv[0];
        // vectorized version when we exactly know that there are no nulls
        if (x->adt->attrs & VEC_ATTR_WITHOUT_NULLS)
        {
            if (x->adt->attrs & VEC_ATTR_ASC)
                return f64(fv[0]);
            if (x->adt->attrs & VEC_ATTR_DESC)
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

rf_object_t rf_max(rf_object_t *x)
{
    i32_t i;
    i64_t l, imax, *iv;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        l = x->adt->len;

        if (!l)
            return i64(NULL_I64);

        iv = as_vector_i64(x);
        imax = iv[0];
        // vectorized version when we exactly know that there are no nulls
        if (x->adt->attrs & VEC_ATTR_WITHOUT_NULLS)
        {
            if (x->adt->attrs & VEC_ATTR_ASC)
                return i64(iv[l - 1]);
            if (x->adt->attrs & VEC_ATTR_DESC)
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
        return error_type1(x->type, "max: unsupported type");
    }
}

rf_object_t rf_not(rf_object_t *x)
{
    i32_t i;
    i64_t l;
    rf_object_t res;

    switch (MTYPE(x->type))
    {
    case MTYPE(-TYPE_BOOL):
        return bool(!x->bool);

    case MTYPE(TYPE_BOOL):
        l = x->adt->len;
        res = vector_bool(l);
        for (i = 0; i < l; i++)
            as_vector_bool(&res)[i] = !as_vector_bool(x)[i];

        return res;

    default:
        return error_type1(x->type, "not: unsupported type");
    }
}

rf_object_t rf_iasc(rf_object_t *x)
{
    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        return rf_sort_asc(x);

    default:
        return error_type1(x->type, "iasc: unsupported type");
    }
}

rf_object_t rf_idesc(rf_object_t *x)
{
    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        return rf_sort_desc(x);

    default:
        return error_type1(x->type, "idesc: unsupported type");
    }
}

rf_object_t rf_asc(rf_object_t *x)
{
    rf_object_t idx = rf_sort_asc(x);
    i64_t l, i;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        l = x->adt->len;
        for (i = 0; i < l; i++)
            as_vector_i64(&idx)[i] = as_vector_i64(x)[as_vector_i64(&idx)[i]];

        idx.adt->attrs |= VEC_ATTR_ASC;

        return idx;

    default:
        return error_type1(x->type, "asc: unsupported type");
    }
}

rf_object_t rf_desc(rf_object_t *x)
{
    rf_object_t idx = rf_sort_desc(x);
    i64_t l, i;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_I64):
        l = x->adt->len;
        for (i = 0; i < l; i++)
            as_vector_i64(&idx)[i] = as_vector_i64(x)[as_vector_i64(&idx)[i]];

        idx.adt->attrs |= VEC_ATTR_DESC;

        return idx;

    default:
        return error_type1(x->type, "desc: unsupported type");
    }
}

rf_object_t rf_guid_generate(rf_object_t *x)
{
    i64_t i, count;
    rf_object_t vec;
    guid_t *g;

    switch (MTYPE(x->type))
    {
    case MTYPE(-TYPE_I64):
        count = x->i64;
        vec = vector_guid(count);
        g = as_vector_guid(&vec);

        for (i = 0; i < count; i++)
            guid_generate(g + i);

        return vec;

    default:
        return error_type1(x->type, "guid_generate: unsupported type");
    }
}

rf_object_t rf_neg(rf_object_t *x)
{
    rf_object_t res;
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
        l = x->adt->len;
        res = vector_i64(l);
        for (i = 0; i < l; i++)
            as_vector_i64(&res)[i] = -as_vector_i64(x)[i];
        return res;
    case MTYPE(TYPE_F64):
        l = x->adt->len;
        res = vector_f64(l);
        for (i = 0; i < l; i++)
            as_vector_f64(&res)[i] = -as_vector_f64(x)[i];
        return res;

    default:
        return error_type1(x->type, "neg: unsupported type");
    }
}

rf_object_t rf_where(rf_object_t *x)
{
    i32_t i, j = 0;
    i64_t l, *ov;
    bool_t *iv;
    rf_object_t res;

    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_BOOL):
        l = x->adt->len;
        iv = as_vector_bool(x);
        res = vector_i64(l);
        ov = as_vector_i64(&res);
        for (i = 0; i < l; i++)
            if (iv[i])
                ov[j++] = i;

        vector_shrink(&res, j);

        return res;

    default:
        return error_type1(x->type, "where: unsupported type");
    }
}

rf_object_t rf_value(rf_object_t *x)
{
    switch (MTYPE(x->type))
    {
    case MTYPE(TYPE_DICT):
        return rf_object_clone(&as_list(x)[1]);
    default:
        return rf_object_clone(x);
    }
}