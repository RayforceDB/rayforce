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

#include <stdio.h>
#include "dict.h"
#include "vector.h"
#include "format.h"
#include "util.h"

rf_object_t dict(rf_object_t keys, rf_object_t vals)
{
    if (keys.type < 0 || vals.type < 0)
        return error(ERR_TYPE, "Keys and rf_objects must be lists");

    if (keys.adt->len != vals.adt->len)
        return error(ERR_LENGTH, "Keys and rf_objects must have the same length");

    rf_object_t dict = list(2);

    as_list(&dict)[0] = keys;
    as_list(&dict)[1] = vals;

    dict.type = TYPE_DICT;

    return dict;
}

rf_object_t dict_get(rf_object_t *dict, rf_object_t *key)
{
    rf_object_t *keys = &as_list(dict)[0], *vals = &as_list(dict)[1];
    i64_t i;

    i = vector_find(keys, key);

    return vector_get(vals, i);
}

rf_object_t dict_set(rf_object_t *dict, rf_object_t *key, rf_object_t val)
{
    rf_object_t *keys = &as_list(dict)[0];
    rf_object_t *vals = &as_list(dict)[1];
    i64_t index = vector_find(keys, key);

    if (index == keys->adt->len)
    {
        vector_push(keys, rf_object_clone(key));
        vector_push(vals, rf_object_clone(&val));
        return val;
    }

    switch (vals->type)
    {
    case TYPE_I64:
        as_vector_i64(vals)[index] = val.i64;
        break;
    case TYPE_F64:
        as_vector_f64(vals)[index] = val.f64;
        break;
    case TYPE_SYMBOL:
        as_vector_i64(vals)[index] = val.i64;
        break;
    case TYPE_LIST:
        as_list(vals)[index] = rf_object_clone(&val);
        break;
    }

    return val;
}
