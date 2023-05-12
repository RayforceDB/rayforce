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

rf_object_t rf_dict(rf_object_t *x, rf_object_t *y)
{

    return dict(list_flatten(rf_object_clone(x)), list_flatten(rf_object_clone(y)));
}

rf_object_t rf_add_I64_i64(rf_object_t *x, rf_object_t *y)
{
    i32_t i;
    i64_t l = x->adt->len;
    rf_object_t vec = rf_object_cow(x);
    i64_t *v = as_vector_i64(&vec);

    for (i = 0; i < l; i++)
        v[i] = ADDI64(v[i], y->i64);

    return vec;
}

rf_object_t rf_like_String_String(rf_object_t *x, rf_object_t *y)
{
    return (bool(string_match(as_string(x), as_string(y))));
}