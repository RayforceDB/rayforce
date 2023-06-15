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

#include "set.h"
#include "alloc.h"
#include "util.h"

set_t *set_new(i64_t size, u64_t (*hasher)(i64_t a), i32_t (*compare)(i64_t a, i64_t b))
{
    size = next_power_of_two_u64(size);
    i64_t i, *kv;
    set_t *set = (set_t *)rf_malloc(sizeof(struct set_t));

    set->keys = (i64_t *)rf_malloc(size * sizeof(i64_t));
    set->size = size;
    set->count = 0;
    set->hasher = hasher;
    set->compare = compare;

    kv = set->keys;

    for (i = 0; i < size; i++)
        kv[i] = NULL_I64;

    return set;
}

null_t set_free(set_t *set)
{
    rf_free(set->keys);
    rf_free(set);
}

null_t set_rehash(set_t *set)
{
    i64_t i, old_size = set->size, key,
             *old_keys = set->keys, *new_keys;
    u64_t index, factor;

    // Double the table size.
    set->size *= 2;
    set->keys = (i64_t *)rf_malloc(set->size * sizeof(i64_t));

    new_keys = set->keys;

    for (i = 0; i < set->size; i++)
        new_keys[i] = NULL_I64;

    for (i = 0; i < old_size; i++)
    {
        if (old_keys[i] != NULL_I64)
        {
            key = old_keys[i];
            factor = set->size - 1,
            index = set->hasher(key) & factor;

            // Linear probing.
            while (new_keys[index] != NULL_I64)
                index = (index + 1) & factor;

            new_keys[index] = key;
        }
    }

    rf_free(old_keys);
}

bool_t set_insert(set_t *set, i64_t key)
{
    while (true)
    {
        i32_t i, size = set->size;
        u64_t factor = set->size - 1, index = set->hasher(key) & factor;
        i64_t *keys = set->keys;

        for (i = index; i < size; i++)
        {
            if (keys[i] == NULL_I64)
            {
                keys[i] = key;
                set->count++;

                // Check if rehash is necessary.
                if ((f64_t)set->count / set->size > 0.7)
                    set_rehash(set);

                return true;
            }

            if (set->compare(keys[i], key) == 0)
                return false;
        }

        set_rehash(set);
    }
}

bool_t set_contains(set_t *set, i64_t key)
{
    i32_t i, size = set->size;
    u64_t factor = set->size - 1, index = set->hasher(key) & factor;
    i64_t *keys = set->keys;

    for (i = index; i < size; i++)
    {
        if (keys[i] != NULL_I64)
        {
            if (set->compare(keys[i], key) == 0)
                return true;
        }
        else
            return false;
    }

    return false;
}

i64_t set_next(set_t *set, i64_t *index)
{
    i64_t *keys = set->keys;

    for (; *index < set->size; (*index)++)
    {
        if (keys[*index] != NULL_I64)
            return keys[(*index)++];
    }

    return NULL_I64;
}
