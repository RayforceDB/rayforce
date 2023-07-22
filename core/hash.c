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
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "hash.h"
#include "rayforce.h"
#include "alloc.h"
#include "util.h"

CASSERT(sizeof(bucket_t) == 16, hash_h)

ht_t *ht_new(i64_t size, u64_t (*hasher)(i64_t a), i32_t (*compare)(i64_t a, i64_t b))
{
    size = next_power_of_two_u64(size);
    i64_t i;
    ht_t *table = (ht_t *)rf_malloc(sizeof(struct ht_t));
    bucket_t *buckets = (bucket_t *)rf_malloc(size * sizeof(bucket_t));

    table->buckets = buckets;
    table->size = size;
    table->count = 0;
    table->hasher = hasher;
    table->compare = compare;

    for (i = 0; i < size; i++)
        buckets[i].key = NULL_I64;

    return table;
}

null_t ht_free(ht_t *table)
{
    rf_free(table->buckets);
    rf_free(table);
}

null_t ht_rehash(ht_t *table)
{
    i64_t i, old_size = table->size, key, val, factor, index;
    bucket_t *old_buckets = table->buckets, *new_buckets;

    // Double the table size.
    table->size *= 2;
    table->buckets = (bucket_t *)rf_malloc(table->size * sizeof(bucket_t));
    factor = table->size - 1;

    new_buckets = table->buckets;

    for (i = 0; i < table->size; i++)
        new_buckets[i].key = NULL_I64;

    for (i = 0; i < old_size; i++)
    {
        if (old_buckets[i].key != NULL_I64)
        {
            key = old_buckets[i].key;
            val = old_buckets[i].val;
            index = table->hasher(key) & factor;

            // Linear probing.
            while (new_buckets[index].key != NULL_I64)
            {
                if (index == table->size)
                    panic("ht is full!!");

                index = index + 1;
            }

            new_buckets[index].key = key;
            new_buckets[index].val = val;
        }
    }

    rf_free(old_buckets);
}

/*
 * Inserts new node or returns existing node.
 * Does not update existing node.
 */
i64_t ht_insert(ht_t *table, i64_t key, i64_t val)
{
    while (true)
    {
        i64_t i = 0, size = table->size, factor = table->size - 1,
              index = table->hasher(key) & factor;
        bucket_t *buckets = table->buckets;

        for (i = index; i < size; i++)
        {
            if (buckets[i].key != NULL_I64)
            {
                if (table->compare(buckets[i].key, key) == 0)
                    return buckets[i].val;
            }
            else
            {
                buckets[i].key = key;
                buckets[i].val = val;
                table->count++;

                // Check if ht_rehash is necessary.
                if ((f64_t)table->count / table->size > 0.7)
                    ht_rehash(table);

                return val;
            }
        }

        ht_rehash(table);
    }
}

/*
 * Does the same as ht_insert, but uses a lambda to set the rf_object of the bucket.
 */
i64_t ht_insert_with(ht_t *table, i64_t key, i64_t val, null_t *seed,
                     i64_t (*func)(i64_t key, i64_t val, null_t *seed, i64_t *tkey, i64_t *tval))
{
    while (true)
    {
        i64_t i = 0, size = table->size, factor = table->size - 1,
              index = table->hasher(key) & factor;
        bucket_t *buckets = table->buckets;

        for (i = index; i < size; i++)
        {
            if (buckets[i].key != NULL_I64)
            {
                if (table->compare(buckets[i].key, key) == 0)
                    return buckets[i].val;
            }
            else
            {
                table->count++;

                // Check if ht_rehash is necessary.
                if ((f64_t)table->count / table->size > 0.7)
                    ht_rehash(table);

                return func(key, val, seed, &buckets[i].key, &buckets[i].val);
            }
        }

        ht_rehash(table);
    }
}

/*
 * Inserts new node or updates existing one.
 * Returns true if the node was updated, false if it was inserted.
 */
bool_t ht_upsert(ht_t *table, i64_t key, i64_t val)
{
    while (true)
    {
        i64_t i = 0, size = table->size, factor = table->size - 1,
              index = table->hasher(key) & factor;
        bucket_t *buckets = table->buckets;

        for (i = index; i < size; i++)
        {
            if (buckets[i].key != NULL_I64)
            {
                if (table->compare(buckets[i].key, key) == 0)
                {
                    buckets[i].val = val;
                    return true;
                }
            }
            else
            {
                buckets[i].key = key;
                buckets[i].val = val;
                table->count++;

                // Check if ht_rehash is necessary.
                if ((f64_t)table->count / table->size > 0.7)
                    ht_rehash(table);

                return false;
            }
        }

        ht_rehash(table);
    }
}

/*
 * Does the same as ht_upsert, but uses a lambda to set the val of the bucket.
 */
bool_t ht_upsert_with(ht_t *table, i64_t key, i64_t val, null_t *seed,
                      bool_t (*func)(i64_t key, i64_t val, null_t *seed, i64_t *tkey, i64_t *tval))
{
    while (true)
    {
        i64_t i = 0, size = table->size, factor = table->size - 1,
              index = table->hasher(key) & factor;
        bucket_t *buckets = table->buckets;

        for (i = index; i < size; i++)
        {
            if (buckets[i].key != NULL_I64)
            {
                if (table->compare(buckets[i].key, key) == 0)
                    return func(key, val, seed, &buckets[i].key, &buckets[i].val);
            }
            else
            {
                buckets[i].key = key;
                buckets[i].val = val;
                table->count++;

                // Check if ht_rehash is necessary.
                if ((f64_t)table->count / table->size > 0.7)
                    ht_rehash(table);

                return false;
            }
        }

        ht_rehash(table);
    }
}

/*
 * Returns the rf_object of the node with the given key.
 * Returns -1 if the key does not exist.
 */
i64_t ht_get(ht_t *table, i64_t key)
{
    i64_t i = 0, size = table->size, factor = table->size - 1,
          index = table->hasher(key) & factor;
    bucket_t *buckets = table->buckets;

    for (i = index; i < size; i++)
    {
        if (buckets[i].key != NULL_I64)
        {
            if (table->compare(buckets[i].key, key) == 0)
                return buckets[i].val;
        }
        else
            return NULL_I64;
    }

    return NULL_I64;
}
