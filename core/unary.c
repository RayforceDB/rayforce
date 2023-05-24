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

#include "rayforce.h"
#include "alloc.h"
#include "vm.h"
#include "ops.h"
#include "util.h"
#include "format.h"
#include "sort.h"
#include "vector.h"

rf_object_t rf_til_i64(rf_object_t *x)
{
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

rf_object_t rf_distinct_i64(rf_object_t *x)
{
    i32_t i, j = 0, mask_size;
    i64_t min, max, l = x->adt->len, *xi = as_vector_i64(x), *vi;
    rf_object_t mask, vec;
    bool_t *m;

    if (l == 0)
        return vector_i64(0);

    if (x->adt->attrs & VEC_ATTR_DISTINCT)
        return rf_object_clone(x);

    max = min = xi[0];

    for (i = 0; i < l; i++)
    {
        if (xi[i] < min)
            min = xi[i];
        else if (xi[i] > max)
            max = xi[i];
    }

    mask_size = max - min + 1;

    mask = vector_bool(mask_size);
    m = as_vector_bool(&mask);
    memset(m, 0, mask_size);

    vec = vector_i64(l);
    vi = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
    {
        if (!m[xi[i]])
        {
            vi[j++] = xi[i];
            m[xi[i]] = true;
        }
    }

    rf_object_free(&mask);
    vec.adt->len = j;

    return vec;
}

rf_object_t rf_sum_I64(rf_object_t *x)
{
    i32_t i;
    i64_t l = x->adt->len, sum = 0, *v = as_vector_i64(x);

    for (i = 0; i < l; i++)
        sum += v[i];

    return i64(sum);
}

rf_object_t rf_avg_I64(rf_object_t *x)
{
    i32_t i;
    i64_t l = x->adt->len, sum = 0,
          *v = as_vector_i64(x), n = 0;

    // vectorized version when we exactly know that there are no nulls
    if (x->adt->attrs & VEC_ATTR_WITHOUT_NULLS)
    {
        for (i = 0; i < l; i++)
            sum += v[i];

        return i64(sum / l);
    }

    // scalar version
    for (i = 0; i < l; i++)
    {
        if (v[i] ^ NULL_I64)
            sum += v[i];
        else
            n++;
    }

    return f64((f64_t)sum / (l - n));
}

rf_object_t rf_min_I64(rf_object_t *x)
{
    i32_t i;
    i64_t l = x->adt->len, min = 0,
          *v = as_vector_i64(x);

    if (!l)
        return i64(NULL_I64);

    // vectorized version when we exactly know that there are no nulls
    if (x->adt->attrs & VEC_ATTR_WITHOUT_NULLS)
    {
        if (x->adt->attrs & VEC_ATTR_ASC)
            return i64(v[0]);
        if (x->adt->attrs & VEC_ATTR_DESC)
            return i64(v[l - 1]);
        min = v[0];
        for (i = 1; i < l; i++)
            if (v[i] < min)
                min = v[i];

        return i64(min);
    }

    // scalar version
    // find first nonnull value
    for (i = 0; i < l; i++)
        if (v[i] ^ NULL_I64)
        {
            min = v[i];
            break;
        }

    for (i = 0; i < l; i++)
    {
        if (v[i] ^ NULL_I64)
            min = v[i] < min ? v[i] : min;
    }

    return i64(min);
}

rf_object_t rf_max_I64(rf_object_t *x)
{
    i32_t i;
    i64_t l = x->adt->len, max = x->adt->len ? as_vector_i64(x)[0] : 0, *v = as_vector_i64(x);

    if (x->adt->attrs & VEC_ATTR_ASC)
        return i64(v[l - 1]);
    if (x->adt->attrs & VEC_ATTR_DESC)
        return i64(v[0]);

    for (i = 0; i < l; i++)
        if (v[i] > max)
            max = v[i];

    return i64(max);
}

rf_object_t rf_sum_F64(rf_object_t *x)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t sum = 0, *v = as_vector_f64(x);

    for (i = 0; i < l; i++)
        sum += v[i];

    return f64(sum);
}

rf_object_t rf_avg_F64(rf_object_t *x)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t sum = 0, *v = as_vector_f64(x);

    for (i = 0; i < l; i++)
        sum += v[i];

    return f64(sum / l);
}

rf_object_t rf_min_F64(rf_object_t *x)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t min = x->adt->len ? as_vector_f64(x)[0] : 0, *v = as_vector_f64(x);

    for (i = 0; i < l; i++)
        if (v[i] < min)
            min = v[i];

    return f64(min);
}

rf_object_t rf_max_F64(rf_object_t *x)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t max = x->adt->len ? as_vector_f64(x)[0] : 0, *v = as_vector_f64(x);

    for (i = 0; i < l; i++)
        if (v[i] > max)
            max = v[i];

    return f64(max);
}

rf_object_t rf_count(rf_object_t *x)
{
    if (x->type < TYPE_NULL)
        return i64(1);

    switch (x->type)
    {
    case TYPE_FUNCTION:
        return i64(1);
    default:
        return i64(x->adt->len);
    }
}

rf_object_t rf_not_bool(rf_object_t *x)
{
    return bool(!x->bool);
}

rf_object_t rf_not_Bool(rf_object_t *x)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t res = vector_bool(l);
    bool_t *iv = as_vector_bool(x), *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = !iv[i];

    return res;
}

rf_object_t rf_iasc_I64(rf_object_t *x)
{
    return rf_sort_asc(x);
}

rf_object_t rf_idesc_I64(rf_object_t *x)
{
    return rf_sort_desc(x);
}

rf_object_t rf_asc_I64(rf_object_t *x)
{
    rf_object_t idx = rf_sort_asc(x);
    i64_t len = x->adt->len, i,
          *iv = as_vector_i64(x), *ov = as_vector_i64(&idx);

    for (i = 0; i < len; i++)
        ov[i] = iv[ov[i]];

    idx.adt->attrs |= VEC_ATTR_ASC;

    return idx;
}

rf_object_t rf_desc_I64(rf_object_t *x)
{
    rf_object_t idx = rf_sort_desc(x);
    i64_t len = x->adt->len, i,
          *iv = as_vector_i64(x), *ov = as_vector_i64(&idx);

    for (i = 0; i < len; i++)
        ov[i] = iv[ov[i]];

    idx.adt->attrs |= VEC_ATTR_DESC;

    return idx;
}

rf_object_t rf_flatten_List(rf_object_t *x)
{
    return list_flatten(x);
}