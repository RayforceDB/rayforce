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

#include "binary.h"
#include "dict.h"
#include "util.h"
#include "ops.h"
#include "util.h"
#include "format.h"

rf_object_t rf_dict(rf_object_t *x, rf_object_t *y)
{
    return dict(list_flatten(rf_object_clone(x)), list_flatten(rf_object_clone(y)));
}

rf_object_t rf_add_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_i64(l);
    i64_t *iv = as_vector_i64(x), *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = ADDI64(iv[i], y->i64);

    return vec;
}

rf_object_t rf_add_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_f64(l);
    f64_t *iv = as_vector_f64(x), *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = ADDF64(iv[i], y->f64);

    return vec;
}

rf_object_t rf_like_String_String(rf_object_t *x, rf_object_t *y)
{
    return (bool(string_match(as_string(x), as_string(y))));
}

rf_object_t rf_eq_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->i64 == y->i64));
}

rf_object_t rf_eq_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->f64 == y->f64));
}

rf_object_t rf_eq_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv = as_vector_i64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] == y->i64;

    return res;
}

rf_object_t rf_eq_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] == iv2[i];

    return res;
}

rf_object_t rf_eq_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv = as_vector_f64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] == y->f64;

    return res;
}

rf_object_t rf_eq_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] == iv2[i];

    return res;
}

rf_object_t rf_eq_Bool_Bool(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->bool == y->bool));
}

rf_object_t rf_ne_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->i64 != y->i64));
}

rf_object_t rf_ne_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->f64 != y->f64));
}

rf_object_t rf_ne_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv = as_vector_i64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] != y->i64;

    return res;
}

rf_object_t rf_ne_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] != iv2[i];

    return res;
}

rf_object_t rf_ne_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv = as_vector_f64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] != y->f64;

    return res;
}

rf_object_t rf_ne_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] != iv2[i];

    return res;
}

rf_object_t rf_ne_Bool_Bool(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *iv1 = as_vector_bool(x), *iv2 = as_vector_bool(y), *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] != iv2[i];

    return res;
}

rf_object_t rf_lt_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->i64 < y->i64));
}

rf_object_t rf_lt_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->f64 < y->f64));
}

rf_object_t rf_lt_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv = as_vector_i64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] < y->i64;

    return res;
}

rf_object_t rf_lt_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] < iv2[i];

    return res;
}

rf_object_t rf_lt_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv = as_vector_f64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] < y->f64;

    return res;
}

rf_object_t rf_lt_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] < iv2[i];

    return res;
}

rf_object_t rf_le_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->i64 <= y->i64));
}

rf_object_t rf_le_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->f64 <= y->f64));
}

rf_object_t rf_le_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv = as_vector_i64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] <= y->i64;

    return res;
}

rf_object_t rf_le_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] <= iv2[i];

    return res;
}

rf_object_t rf_le_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv = as_vector_f64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] <= y->f64;

    return res;
}

rf_object_t rf_le_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] <= iv2[i];

    return res;
}

rf_object_t rf_gt_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->i64 > y->i64));
}

rf_object_t rf_gt_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->f64 > y->f64));
}

rf_object_t rf_gt_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv = as_vector_i64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] > y->i64;

    return res;
}

rf_object_t rf_gt_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] > iv2[i];

    return res;
}

rf_object_t rf_gt_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv = as_vector_f64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] > y->f64;

    return res;
}

rf_object_t rf_gt_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] > iv2[i];

    return res;
}

rf_object_t rf_ge_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->i64 >= y->i64));
}

rf_object_t rf_ge_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->f64 >= y->f64));
}

rf_object_t rf_ge_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv = as_vector_i64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] >= y->i64;

    return res;
}

rf_object_t rf_ge_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] >= iv2[i];

    return res;
}

rf_object_t rf_ge_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv = as_vector_f64(x);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv[i] >= y->f64;

    return res;
}

rf_object_t rf_ge_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y);
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] >= iv2[i];

    return res;
}

rf_object_t rf_and_bool_bool(rf_object_t *x, rf_object_t *y)
{
    return (bool(x->bool && y->bool));
}

rf_object_t rf_and_Bool_Bool(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *iv1 = as_vector_bool(x), *iv2 = as_vector_bool(y), *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] & iv2[i];

    return res;
}

rf_object_t rf_or_Bool_Bool(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t res = vector_bool(x->adt->len);
    bool_t *iv1 = as_vector_bool(x), *iv2 = as_vector_bool(y), *ov = as_vector_bool(&res);

    for (i = 0; i < l; i++)
        ov[i] = iv1[i] | iv2[i];

    return res;
}
