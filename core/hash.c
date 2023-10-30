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

nil_t rehash(obj_t *obj, hash_f hash, nil_t *seed)
{
    u64_t i, j, size, key, factor;
    obj_t new_obj;
    type_t type;
    i64_t *orig_keys, *new_keys, *orig_vals = NULL, *new_vals = NULL;

    size = as_list(*obj)[0]->len;
    orig_keys = as_i64(as_list(*obj)[0]);

    type = is_null(as_list(*obj)[1]) ? -1 : as_list(*obj)[1]->type;

    if (type > -1)
        orig_vals = as_i64(as_list(*obj)[1]);

    new_obj = ht_tab(size * 2, type);

    if (new_obj == NULL)
        throw("ht rehash: oom");

    factor = as_list(new_obj)[0]->len - 1;
    new_keys = as_i64(as_list(new_obj)[0]);

    if (type > -1)
        new_vals = as_i64(as_list(new_obj)[1]);

    for (i = 0; i < size; i++)
    {
        if (orig_keys[i] != NULL_I64)
        {
            key = orig_keys[i];

            // Recalculate the index for the new table
            j = hash ? hash(key, seed) & factor : (u64_t)key & factor;

            while (new_keys[j] != NULL_I64)
            {
                j++;

                if (j == size)
                    throw("ht is full");
            }

            new_keys[j] = key;

            if (type > -1)
                new_vals[j] = orig_vals[i];
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

        rehash(obj, NULL, NULL);
    }
}

i64_t ht_tab_next_with(obj_t *obj, i64_t key, hash_f hash, cmp_f cmp, nil_t *seed)
{
    u64_t i, size;

    while (true)
    {
        size = as_list(*obj)[0]->len;
        for (i = hash(key, seed) & (size - 1); i < size; i++)
        {
            if (as_i64(as_list(*obj)[0])[i] == NULL_I64 || cmp(as_i64(as_list(*obj)[0])[i], key, seed) == 0)
                return i;
        }

        rehash(obj, hash, seed);
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

i64_t ht_tab_get_with(obj_t obj, i64_t key, hash_f hash, cmp_f cmp, nil_t *seed)
{
    u64_t i, size = as_list(obj)[0]->len;

    for (i = hash(key, seed) & (size - 1); i < size; i++)
    {
        if (as_i64(as_list(obj)[0])[i] == NULL_I64)
            return NULL_I64;

        if (cmp(as_i64(as_list(obj)[0])[i], key, seed) == 0)
            return i;
    }

    return NULL_I64;
}