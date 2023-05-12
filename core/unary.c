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

rf_object_t rf_til_i64(rf_object_t *x)
{
    i32_t i, l = (i32_t)x->i64;
    i64_t *v;
    rf_object_t vec;

    vec = vector_i64(l);
    v = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        v[i] = i;

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