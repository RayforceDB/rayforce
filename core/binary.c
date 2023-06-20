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
#include "binary.h"
#include "dict.h"
#include "util.h"
#include "ops.h"
#include "util.h"
#include "format.h"
#include "vector.h"
#include "hash.h"

rf_object_t rf_dict(rf_object_t *x, rf_object_t *y)
{
    return dict(rf_object_clone(x), rf_object_clone(y));
}

rf_object_t rf_table(rf_object_t *x, rf_object_t *y)
{
    return table(rf_object_clone(x), rf_object_clone(y));
}

rf_object_t rf_add_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (i64(ADDI64(x->i64, y->i64)));
}

rf_object_t rf_add_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (f64(ADDF64(x->f64, y->f64)));
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

rf_object_t rf_add_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_i64(l);
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y), *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = ADDI64(iv1[i], iv2[i]);

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

rf_object_t rf_add_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_f64(l);
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y), *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = ADDF64(iv1[i], iv2[i]);

    return vec;
}

rf_object_t rf_sub_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (i64(SUBI64(x->i64, y->i64)));
}

rf_object_t rf_sub_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (f64(SUBF64(x->f64, y->f64)));
}

rf_object_t rf_sub_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_i64(l);
    i64_t *iv = as_vector_i64(x), *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = SUBI64(iv[i], y->i64);

    return vec;
}

rf_object_t rf_sub_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_i64(l);
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y), *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = SUBI64(iv1[i], iv2[i]);

    return vec;
}

rf_object_t rf_sub_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_f64(l);
    f64_t *iv = as_vector_f64(x), *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = SUBF64(iv[i], y->f64);

    return vec;
}

rf_object_t rf_sub_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_f64(l);
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y), *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = SUBF64(iv1[i], iv2[i]);

    return vec;
}

rf_object_t rf_mul_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (i64(MULI64(x->i64, y->i64)));
}

rf_object_t rf_mul_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (f64(MULF64(x->f64, y->f64)));
}

rf_object_t rf_mul_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_i64(l);
    i64_t *iv = as_vector_i64(x), *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = MULI64(iv[i], y->i64);

    return vec;
}

rf_object_t rf_mul_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_i64(l);
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y), *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = MULI64(iv1[i], iv2[i]);

    return vec;
}

rf_object_t rf_mul_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_f64(l);
    f64_t *iv = as_vector_f64(x), *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = MULF64(iv[i], y->f64);

    return vec;
}

rf_object_t rf_mul_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = vector_f64(l);
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y), *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = MULF64(iv1[i], iv2[i]);

    return vec;
}

rf_object_t rf_fdiv_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (f64(FDIVI64(x->i64, y->i64)));
}

rf_object_t rf_fdiv_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (f64(FDIVF64(x->f64, y->f64)));
}

rf_object_t rf_fdiv_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv = as_vector_i64(x);
    rf_object_t vec = vector_f64(l);
    f64_t *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = FDIVI64(iv[i], y->i64);

    return vec;
}

rf_object_t rf_fdiv_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y);
    rf_object_t vec = vector_f64(l);
    f64_t *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = FDIVI64(iv1[i], iv2[i]);

    return vec;
}

rf_object_t rf_fdiv_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv = as_vector_f64(x);
    rf_object_t vec = vector_f64(l);
    f64_t *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = FDIVF64(iv[i], y->f64);

    return vec;
}

rf_object_t rf_fdiv_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y);
    rf_object_t vec = vector_f64(l);
    f64_t *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = FDIVF64(iv1[i], iv2[i]);

    return vec;
}

rf_object_t rf_div_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (i64(DIVI64(x->i64, y->i64)));
}

rf_object_t rf_div_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (f64(DIVF64(x->f64, y->f64)));
}

rf_object_t rf_div_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv = as_vector_i64(x);
    rf_object_t vec = vector_i64(l);
    i64_t *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = DIVI64(iv[i], y->i64);

    return vec;
}

rf_object_t rf_div_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y);
    rf_object_t vec = vector_i64(l);
    i64_t *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = DIVI64(iv1[i], iv2[i]);

    return vec;
}

rf_object_t rf_div_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv = as_vector_f64(x);
    rf_object_t vec = vector_f64(l);
    f64_t *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = (f64_t)DIVF64(iv[i], y->f64);

    return vec;
}

rf_object_t rf_div_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y);
    rf_object_t vec = vector_f64(l);
    f64_t *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = (f64_t)DIVF64(iv1[i], iv2[i]);

    return vec;
}

rf_object_t rf_mod_i64_i64(rf_object_t *x, rf_object_t *y)
{
    return (i64(MODI64(x->i64, y->i64)));
}

rf_object_t rf_mod_f64_f64(rf_object_t *x, rf_object_t *y)
{
    return (f64(MODF64(x->f64, y->f64)));
}

rf_object_t rf_mod_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv = as_vector_i64(x);
    rf_object_t vec = vector_i64(l);
    i64_t *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = MODI64(iv[i], y->i64);

    return vec;
}

rf_object_t rf_mod_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y);
    rf_object_t vec = vector_i64(l);
    i64_t *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = MODI64(iv1[i], iv2[i]);

    return vec;
}

rf_object_t rf_mod_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv = as_vector_f64(x);
    rf_object_t vec = vector_f64(l);
    f64_t *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = MODF64(iv[i], y->f64);

    return vec;
}

rf_object_t rf_mod_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y);
    rf_object_t vec = vector_f64(l);
    f64_t *ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = MODF64(iv1[i], iv2[i]);

    return vec;
}

rf_object_t rf_like_Char_Char(rf_object_t *x, rf_object_t *y)
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

rf_object_t rf_get_Bool_i64(rf_object_t *x, rf_object_t *y)
{
    return vector_get(x, y->i64);
}

rf_object_t rf_get_Bool_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t yl = y->adt->len, xl = x->adt->len;
    rf_object_t vec = vector_bool(yl);
    bool_t *iv1 = as_vector_bool(x), *iv2 = as_vector_bool(y), *ov = as_vector_bool(&vec);

    for (i = 0; i < yl; i++)
    {
        if (iv2[i] >= xl)
            ov[i] = false;
        else
            ov[i] = iv1[(i32_t)iv2[i]];
    }

    return vec;
}

rf_object_t rf_get_I64_i64(rf_object_t *x, rf_object_t *y)
{
    return vector_get(x, y->i64);
}

rf_object_t rf_get_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t yl = y->adt->len, xl = x->adt->len;
    rf_object_t vec = vector_i64(yl);
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y), *ov = as_vector_i64(&vec);

    for (i = 0; i < yl; i++)
    {
        if (iv2[i] >= xl)
            ov[i] = NULL_I64;
        else
            ov[i] = iv1[iv2[i]];
    }

    return (vec);
}

rf_object_t rf_get_Timestamp_i64(rf_object_t *x, rf_object_t *y)
{
    return vector_get(x, y->i64);
}

rf_object_t rf_get_Timestamp_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t yl = y->adt->len, xl = x->adt->len;
    rf_object_t vec = vector_timestamp(yl);
    i64_t *iv1 = as_vector_timestamp(x), *ov = as_vector_timestamp(&vec);
    i64_t *iv2 = as_vector_i64(y);

    for (i = 0; i < yl; i++)
    {
        if (iv2[i] >= xl)
            ov[i] = NULL_I64;
        else
            ov[i] = iv1[iv2[i]];
    }

    return (vec);
}

rf_object_t rf_get_Guid_i64(rf_object_t *x, rf_object_t *y)
{
    return vector_get(x, y->i64);
}

rf_object_t rf_get_Guid_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t yl = y->adt->len, xl = x->adt->len;
    rf_object_t vec = vector_guid(yl);
    guid_t *iv1 = as_vector_guid(x), *ov = as_vector_guid(&vec);
    i64_t *iv2 = as_vector_i64(y);

    for (i = 0; i < yl; i++)
    {
        if (iv2[i] >= xl)
            ov[i] = (guid_t){0};
        else
            ov[i] = iv1[iv2[i]];
    }

    return (vec);
}

rf_object_t rf_get_F64_i64(rf_object_t *x, rf_object_t *y)
{
    return vector_get(x, y->i64);
}

rf_object_t rf_get_F64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t yl = y->adt->len, xl = x->adt->len;
    rf_object_t vec = vector_f64(yl);
    f64_t *iv1 = as_vector_f64(x), *ov = as_vector_f64(&vec);
    i64_t *iv2 = as_vector_i64(y);

    for (i = 0; i < yl; i++)
    {
        if (iv2[i] >= xl)
            ov[i] = NULL_F64;
        else
            ov[i] = iv1[iv2[i]];
    }

    return (vec);
}

rf_object_t rf_get_Char_i64(rf_object_t *x, rf_object_t *y)
{
    return vector_get(x, y->i64);
}

rf_object_t rf_get_Char_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t yl = y->adt->len, xl = x->adt->len;
    rf_object_t vec = string(yl);
    str_t iv1 = as_string(x), ov = as_string(&vec);
    i64_t *iv2 = as_vector_i64(y);

    for (i = 0; i < yl; i++)
    {
        if (iv2[i] >= xl)
            ov[i] = ' ';
        else
            ov[i] = iv1[iv2[i]];
    }

    return vec;
}

rf_object_t rf_get_List_i64(rf_object_t *x, rf_object_t *y)
{
    return vector_get(x, y->i64);
}

rf_object_t rf_get_List_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t yl = y->adt->len, xl = x->adt->len;
    rf_object_t vec = list(yl);
    rf_object_t *iv1 = as_list(x), *ov = as_list(&vec);
    i64_t *iv2 = as_vector_i64(y);

    for (i = 0; i < yl; i++)
    {
        if (iv2[i] >= xl)
            ov[i] = null();
        else
            ov[i] = rf_object_clone(&iv1[iv2[i]]);
    }

    return vec;
}

rf_object_t rf_get_Table_symbol(rf_object_t *x, rf_object_t *y)
{
    return dict_get(x, y);
}

rf_object_t rf_find_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i64_t l = x->adt->len, i;

    i = vector_find(x, y);

    if (i == l)
        return i64(NULL_I64);
    else
        return i64(i);
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

#define mask -1ll
#define normalize(k) ((u64_t)(k - min))
    // if range fits in 64 mb, use vector positions instead of hash table
    range = max - min + 1;
    if (range < 1024 * 1024 * 64)
    {
        found = vector_i64(range);
        fv = as_vector_i64(&found);
        memset(fv, 255, sizeof(i64_t) * range);

        for (i = 0; i < xl; i++)
        {
            n = normalize(iv1[i]);
            if (fv[n] == mask)
                fv[n] = i;
        }

        for (i = 0; i < yl; i++)
        {
            n = normalize(iv2[i]);
            if (iv2[i] < min || iv2[i] > max || fv[n] == mask)
                ov[i] = NULL_I64;
            else
                ov[i] = fv[n];
        }

        rf_object_free(&found);

        return vec;
    }

    // otherwise, use a hash table
    ht = ht_new(xl, &kmh_hash, &i64_cmp);

    for (i = 0; i < xl; i++)
        ht_insert(ht, iv1[i], i);

    for (i = 0; i < yl; i++)
        ov[i] = ht_get(ht, iv2[i]);

    ht_free(ht);

    return vec;
}

rf_object_t rf_find_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i64_t l = x->adt->len, i;

    i = vector_find(x, y);

    if (i == l)
        return i64(NULL_I64);
    else
        return i64(i);
}

rf_object_t rf_find_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i64_t xl = x->adt->len, yl = y->adt->len, i, j;
    rf_object_t vec = vector_i64(yl);
    f64_t *iv1 = as_vector_f64(x);
    i64_t *iv2 = as_vector_i64(y);

    for (i = 0; i < yl; i++)
    {
        for (j = 0; j < xl; j++)
            if (iv1[j] == y->f64)
                break;
        if (j == xl)
            iv2[i] = NULL_I64;
        else
            iv2[i] = j;
    }

    return vec;
}

rf_object_t rf_concat_bool_bool(rf_object_t *x, rf_object_t *y)
{
    rf_object_t vec = vector_bool(2);
    as_vector_bool(&vec)[0] = x->bool;
    as_vector_bool(&vec)[1] = y->bool;

    return vec;
}

rf_object_t rf_concat_Bool_bool(rf_object_t *x, rf_object_t *y)
{
    rf_object_t vec = vector_bool(x->adt->len + 1);
    as_vector_bool(&vec)[x->adt->len] = y->bool;

    return vec;
}

rf_object_t rf_concat_Bool_Bool(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t xl = x->adt->len, yl = y->adt->len;
    rf_object_t vec = vector_bool(xl + yl);
    bool_t *iv1 = as_vector_bool(x), *iv2 = as_vector_bool(y), *ov = as_vector_bool(&vec);

    for (i = 0; i < xl; i++)
        ov[i] = iv1[i];
    for (i = 0; i < yl; i++)
        ov[i + xl] = iv2[i];

    return vec;
}

rf_object_t rf_concat_bool_Bool(rf_object_t *x, rf_object_t *y)
{
    rf_object_t vec = vector_bool(2);
    as_vector_bool(&vec)[0] = x->bool;
    as_vector_bool(&vec)[1] = y->bool;

    return vec;
}

rf_object_t rf_concat_i64_i64(rf_object_t *x, rf_object_t *y)
{
    rf_object_t vec = vector_i64(2);
    as_vector_i64(&vec)[0] = x->i64;
    as_vector_i64(&vec)[1] = y->i64;

    return vec;
}

rf_object_t rf_concat_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i64_t i, l = x->adt->len, *iv, *ov;
    rf_object_t vec = vector_i64(l + 1);

    iv = as_vector_i64(x);
    ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = iv[i];

    ov[l] = y->i64;

    return vec;
}

rf_object_t rf_concat_I64_I64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t xl = x->adt->len, yl = y->adt->len;
    rf_object_t vec = vector_i64(xl + yl);
    i64_t *iv1 = as_vector_i64(x), *iv2 = as_vector_i64(y), *ov = as_vector_i64(&vec);

    for (i = 0; i < xl; i++)
        ov[i] = iv1[i];
    for (i = 0; i < yl; i++)
        ov[i + xl] = iv2[i];

    return vec;
}

rf_object_t rf_concat_i64_I64(rf_object_t *x, rf_object_t *y)
{
    i64_t i, l = y->adt->len, *iv, *ov;
    rf_object_t vec = vector_i64(l + 1);

    iv = as_vector_i64(y);
    ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i + 1] = iv[i];

    ov[0] = x->i64;

    return vec;
}

rf_object_t rf_concat_f64_f64(rf_object_t *x, rf_object_t *y)
{
    rf_object_t vec = vector_f64(2);
    as_vector_i64(&vec)[0] = x->f64;
    as_vector_i64(&vec)[1] = y->f64;

    return vec;
}

rf_object_t rf_concat_F64_f64(rf_object_t *x, rf_object_t *y)
{
    i64_t i, l = x->adt->len;
    f64_t *iv, *ov;
    rf_object_t vec = vector_f64(l + 1);

    iv = as_vector_f64(x);
    ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = iv[i];

    ov[l] = y->f64;

    return vec;
}

rf_object_t rf_concat_F64_F64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t xl = x->adt->len, yl = y->adt->len;
    rf_object_t vec = vector_f64(xl + yl);
    f64_t *iv1 = as_vector_f64(x), *iv2 = as_vector_f64(y), *ov = as_vector_f64(&vec);

    for (i = 0; i < xl; i++)
        ov[i] = iv1[i];
    for (i = 0; i < yl; i++)
        ov[i + xl] = iv2[i];

    return vec;
}

rf_object_t rf_concat_f64_F64(rf_object_t *x, rf_object_t *y)
{
    i64_t i, l = y->adt->len;
    f64_t *iv, *ov;
    rf_object_t vec = vector_f64(l + 1);

    iv = as_vector_f64(y);
    ov = as_vector_f64(&vec);

    for (i = 0; i < l; i++)
        ov[i + 1] = iv[i];

    ov[0] = x->f64;

    return vec;
}

rf_object_t rf_concat_char_char(rf_object_t *x, rf_object_t *y)
{
    rf_object_t vec = string(2);
    as_string(&vec)[0] = x->schar;
    as_string(&vec)[1] = y->schar;

    return vec;
}

rf_object_t rf_concat_Char_char(rf_object_t *x, rf_object_t *y)
{
    i64_t i, l = x->adt->len;
    str_t iv, ov;
    rf_object_t vec = string(l + 1);

    iv = as_string(x);
    ov = as_string(&vec);

    for (i = 0; i < l; i++)
        ov[i] = iv[i];

    ov[l] = y->schar;

    return vec;
}

rf_object_t rf_concat_Char_Char(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t xl = x->adt->len, yl = y->adt->len;
    rf_object_t vec = string(xl + yl);
    str_t iv1 = as_string(x), iv2 = as_string(y), ov = as_string(&vec);

    for (i = 0; i < xl; i++)
        ov[i] = iv1[i];
    for (i = 0; i < yl; i++)
        ov[i + xl] = iv2[i];

    return vec;
}

rf_object_t rf_concat_char_Char(rf_object_t *x, rf_object_t *y)
{
    i64_t i, l = y->adt->len;
    str_t iv, ov;
    rf_object_t vec = string(l + 1);

    iv = as_string(y);
    ov = as_string(&vec);

    for (i = 0; i < l; i++)
        ov[i + 1] = iv[i];

    ov[0] = x->schar;

    return vec;
}

rf_object_t rf_concat_List_List(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t xl = x->adt->len, yl = y->adt->len;
    rf_object_t vec = list(xl + yl);
    rf_object_t *iv1 = as_list(x), *iv2 = as_list(y), *ov = as_list(&vec);

    for (i = 0; i < xl; i++)
        ov[i] = rf_object_clone(&iv1[i]);
    for (i = 0; i < yl; i++)
        ov[i + xl] = rf_object_clone(&iv2[i]);

    return vec;
}

rf_object_t rf_filter_I64_Bool(rf_object_t *x, rf_object_t *y)
{
    if (x->adt->len != y->adt->len)
        return error(ERR_LENGTH, "filter: vector and filter vector must be of same length");

    i32_t i, j = 0;
    i64_t l = x->adt->len;
    i64_t *iv1 = as_vector_i64(x);
    bool_t *iv2 = as_vector_bool(y);
    rf_object_t res = vector_i64(l);
    i64_t *ov = as_vector_i64(&res);

    for (i = 0; i < l; i++)
        if (iv2[i])
            ov[j++] = iv1[i];

    vector_shrink(&res, j);

    return res;
}

rf_object_t rf_take_i64_i64(rf_object_t *x, rf_object_t *y)
{
    i64_t i, l = x->i64, n = y->i64;
    rf_object_t vec = vector_i64(l);
    i64_t *ov = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        ov[i] = n;

    return vec;
}
