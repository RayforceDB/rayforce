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

obj_t dict_get(obj_t dict, obj_t key)
{
    // obj_t keys = as_list(dict)[0], vals = as_list(dict)[1];
    // i64_t i;

    // i = vector_find(keys, key);

    // return vector_get(vals, i);

    return null();
}

obj_t dict_set(obj_t dict, obj_t key, obj_t val)
{
    // obj_t keys = as_list(dict)[0];
    // obj_t vals = as_list(dict)[1];
    // i64_t index = vector_find(keys, key);

    // if (index == (i64_t)keys->len)
    // {
    //     vector_push(keys, clone(key));
    //     vector_push(vals, clone(val));
    //     return val;
    // }

    // vector_set(vals, index, clone(val));

    return val;
}

nil_t dict_clear(obj_t dict)
{
    obj_t keys = as_list(dict)[0];
    obj_t vals = as_list(dict)[1];

    if (keys->len == 0)
        return;

    vector_clear(keys);
    vector_clear(vals);
}
