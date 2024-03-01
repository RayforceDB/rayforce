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
#include "eval.h"
#include "error.h"

obj_p ht_tab(u64_t size, i8_t vals)
{
    u64_t i;
    obj_p k, v;

    size = next_power_of_two_u64(size);
    k = vector(TYPE_I64, size);

    if (vals >= 0)
        v = vector(vals, size);
    else
        v = NULL_OBJ;

    for (i = 0; i < size; i++)
        as_i64(k)[i] = NULL_I64;

    return dict(k, v);
}

nil_t rehash(obj_p *obj, hash_f hash, nil_t *seed)
{
    u64_t i, j, size, key, factor;
    obj_p new_obj;
    i8_t type;
    i64_t *orig_keys, *new_keys, *orig_vals = NULL, *new_vals = NULL;

    size = as_list(*obj)[0]->len;
    orig_keys = as_i64(as_list(*obj)[0]);

    type = is_null(as_list(*obj)[1]) ? -1 : as_list(*obj)[1]->type;

    if (type > -1)
        orig_vals = as_i64(as_list(*obj)[1]);

    new_obj = ht_tab(size * 2, type);

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
                    panic("ht is full");
            }

            new_keys[j] = key;

            if (type > -1)
                new_vals[j] = orig_vals[i];
        }
    }

    drop_obj(*obj);
    *obj = new_obj;
}

i64_t ht_tab_next(obj_p *obj, i64_t key)
{
    u64_t i, size;
    i64_t *keys;

    size = as_list(*obj)[0]->len;
    keys = as_i64(as_list(*obj)[0]);

    while (B8_TRUE)
    {
        for (i = (u64_t)key & (size - 1); i < size; i++)
        {
            if ((keys[i] == NULL_I64) || (keys[i] == key))
                return i;
        }

        rehash(obj, NULL, NULL);
        size = as_list(*obj)[0]->len;
        keys = as_i64(as_list(*obj)[0]);
    }
}

i64_t ht_tab_next_with(obj_p *obj, i64_t key, hash_f hash, cmp_f cmp, nil_t *seed)
{
    u64_t i, size;
    i64_t *keys;

    size = as_list(*obj)[0]->len;
    keys = as_i64(as_list(*obj)[0]);

    while (B8_TRUE)
    {
        for (i = hash(key, seed) & (size - 1); i < size; i++)
        {
            if (keys[i] == NULL_I64 || cmp(keys[i], key, seed) == 0)
                return i;
        }

        rehash(obj, hash, seed);
        size = as_list(*obj)[0]->len;
        keys = as_i64(as_list(*obj)[0]);
    }
}

i64_t ht_tab_get(obj_p obj, i64_t key)
{
    u64_t i, size;
    i64_t *keys;

    size = as_list(obj)[0]->len;
    keys = as_i64(as_list(obj)[0]);

    for (i = (u64_t)key & (size - 1); i < size; i++)
    {
        if (keys[i] == NULL_I64)
            return NULL_I64;
        else if (keys[i] == key)
            return i;
    }

    return NULL_I64;
}

i64_t ht_tab_get_with(obj_p obj, i64_t key, hash_f hash, cmp_f cmp, nil_t *seed)
{
    u64_t i, size;
    i64_t *keys;

    size = as_list(obj)[0]->len;
    keys = as_i64(as_list(obj)[0]);

    for (i = hash(key, seed) & (size - 1); i < size; i++)
    {
        if (keys[i] == NULL_I64)
            return NULL_I64;
        else if (cmp(keys[i], key, seed) == 0)
            return i;
    }

    return NULL_I64;
}

u64_t hash_index_obj(obj_p obj)
{
    u64_t hash, len, i, c;
    str_p str;

    switch (obj->type)
    {
    case -TYPE_I64:
    case -TYPE_SYMBOL:
    case -TYPE_TIMESTAMP:
        return (u64_t)obj->i64;
    case -TYPE_F64:
        return (u64_t)obj->f64;
    case -TYPE_GUID:
        return hash_index_u64(*(u64_t *)as_guid(obj), *((u64_t *)as_guid(obj) + 1));
    case TYPE_CHAR:
        str = as_string(obj);
        hash = 5381;
        while ((c = *str++))
            hash = ((hash << 5) + hash) + c;

        return hash;
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        len = obj->len;
        for (i = 0, hash = 0xcbf29ce484222325ull; i < len; i++)
            hash = hash_index_u64((u64_t)as_i64(obj)[i], hash);
        return hash;
    default:
        panic("hash: unsupported type: %d", obj->type);
    }
}

u64_t hash_kmh(i64_t key, nil_t *seed)
{
    unused(seed);
    return (key * 6364136223846793005ull) >> 32;
}

u64_t hash_fnv1a(i64_t key, nil_t *seed)
{
    unused(seed);
    u64_t hash = 14695981039346656037ull;
    i32_t i;

    for (i = 0; i < 8; i++)
    {
        u8_t byte = (key >> (i * 8)) & 0xff;
        hash ^= byte;
        hash *= 1099511628211ull;
    }

    return hash;
}

u64_t hash_guid(i64_t a, nil_t *seed)
{
    unused(seed);
    guid_t *g = (guid_t *)a;
    u64_t upper_part, lower_part;

    // Cast the first and second halves of the GUID to u64_t
    memcpy(&upper_part, g->buf, sizeof(u64_t));
    memcpy(&lower_part, g->buf + 8, sizeof(u64_t));

    // Combine the two parts
    return upper_part ^ lower_part;
}

u64_t hash_obj(i64_t a, nil_t *seed)
{
    unused(seed);
    return hash_index_obj((obj_p)a);
}

i64_t hash_cmp_i64(i64_t a, i64_t b, nil_t *seed)
{
    unused(seed);
    return a - b;
}

i64_t hash_cmp_obj(i64_t a, i64_t b, nil_t *seed)
{
    unused(seed);
    return cmp_obj((obj_p)a, (obj_p)b);
}

i64_t hash_cmp_guid(i64_t a, i64_t b, nil_t *seed)
{
    unused(seed);
    guid_t *g1 = (guid_t *)a, *g2 = (guid_t *)b;
    i64_t i;

    for (i = 0; i < 16; i++)
    {
        if (g1->buf[i] < g2->buf[i])
            return -1;
        if (g1->buf[i] > g2->buf[i])
            return 1;
    }

    return 0;
}
