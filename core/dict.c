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

#include "dict.h"
#include "vector.h"

extern value_t dict(value_t keys, value_t vals)
{
    if (keys.type < 0 || vals.type < 0)
        return error(ERR_TYPE, "Keys and values must be lists");

    if (keys.list.len != vals.list.len)
        return error(ERR_LENGTH, "Keys and values must have the same length");

    value_t dict = list(2);

    as_list(&dict)[0] = keys;
    as_list(&dict)[1] = vals;

    dict.type = TYPE_DICT;

    return dict;
}

extern value_t dict_get(value_t *dict, value_t key)
{
    if (dict->type != TYPE_DICT && dict->type != TYPE_ERROR)
        return error(ERR_TYPE, "Expected dict");

    value_t *keys = &as_list(dict)[0];
    value_t *vals = &as_list(dict)[1];
    u64_t index = vector_find(keys, key);

    if (index == keys->list.len)
        return null();

    return value_clone(&as_list(vals)[index]);
}

extern value_t dict_set(value_t *dict, value_t key, value_t val)
{
    if (dict->type != TYPE_DICT)
        return error(ERR_TYPE, "Expected dict");

    value_t *keys = &as_list(dict)[0];
    value_t *vals = &as_list(dict)[1];
    u64_t index = vector_find(keys, key);

    if (index == keys->list.len)
    {
        vector_push(keys, key);
        vector_push(vals, val);
    }

    else
        as_list(vals)[index] = val;

    return null();
}