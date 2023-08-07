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

#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "string.h"
#include "hash.h"
#include "rayforce.h"
#include "heap.h"
#include "util.h"

obj_t ht_tab(u64_t size, type_t vals)
{
    u64_t i;
    obj_t k, v;

    size = next_power_of_two_u64(size);
    k = vector(TYPE_I64, size);

    if (vals >= 0)
        v = vector(vals, size);
    else
        v = NULL;

    for (i = 0; i < size; i++)
        as_i64(k)[i] = NULL_I64;

    return list(2, k, v);
}

nil_t rehash(obj_t *obj, hash_f hash)
{
    u64_t i, size, key, val, factor, index;
    obj_t new_obj;
    type_t type;

    size = as_list(*obj)[0]->len;
    type = is_null(as_list(*obj)[1]) ? -1 : as_list(*obj)[1]->type;
    new_obj = ht_tab(size * 2, type);
    factor = new_obj->len - 1;

    for (i = 0; i < size; i++)
    {
        if (as_i64(as_list(*obj)[0])[i] != NULL_I64)
        {
            key = as_i64(as_list(*obj)[0])[i];

            if (type > -1)
                val = (i64_t)at_idx(as_list(*obj)[1], i);

            index = hash ? hash(key) & factor : key & factor;

            while (as_i64(as_list(new_obj)[0])[i] != NULL_I64)
            {
                if (index == size)
                    panic("hash tab is full!!");

                index = index + 1;
            }

            as_i64(as_list(new_obj)[0])[index] = key;

            if (type > -1)
                set_idx(&as_list(new_obj)[1], i, (obj_t)val);
        }
    }

    drop(*obj);

    *obj = new_obj;
}

i64_t ht_tab_next(obj_t *obj, i64_t key)
{
    u64_t i, size;

    while (true)
    {
        size = as_list(*obj)[0]->len;

        for (i = (u64_t)key & (size - 1); i < size; i++)
            if ((as_i64(as_list(*obj)[0])[i] == NULL_I64) || (as_i64(as_list(*obj)[0])[i] == key))
                return i;

        rehash(obj, NULL);
    }
}

i64_t ht_tab_next_with(obj_t *obj, i64_t key, hash_f hash, cmp_f cmp)
{
    u64_t i, size;

    while (true)
    {
        size = as_list(*obj)[0]->len;

        for (i = hash(key) & (size - 1); i < size; i++)
            if (as_i64(as_list(*obj)[0])[i] == NULL_I64 || cmp(as_i64(as_list(*obj)[0])[i], key) == 0)
                return i;

        rehash(obj, hash);
    }
}

i64_t ht_tab_get(obj_t obj, i64_t key)
{
    u64_t i, size = as_list(obj)[0]->len;

    for (i = (u64_t)key & (size - 1); i < size; i++)
    {
        if (as_i64(as_list(obj)[0])[i] == NULL_I64)
            return NULL_I64;

        if (as_i64(as_list(obj)[0])[i] == key)
            return i;
    }

    return NULL_I64;
}

i64_t ht_tab_get_with(obj_t obj, i64_t key, hash_f hash, cmp_f cmp)
{
    u64_t i, size = as_list(obj)[0]->len;

    for (i = hash(key) & (size - 1); i < size; i++)
    {
        if (as_i64(as_list(obj)[0])[i] == NULL_I64)
            return NULL_I64;

        if (cmp(as_i64(as_list(obj)[0])[i], key) == 0)
            return i;
    }

    return NULL_I64;
}