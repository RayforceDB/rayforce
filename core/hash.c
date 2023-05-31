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

ht_t *ht_new(u64_t size, u64_t (*hasher)(null_t *a), i32_t (*compare)(null_t *a, null_t *b))
{
    size = next_power_of_two_u64(size);
    ht_t *table = (ht_t *)rf_malloc(sizeof(struct ht_t));

    table->buckets = (bucket_t *)rf_malloc(sizeof(bucket_t) * size);
    table->size = size;
    table->count = 0;
    table->hasher = hasher;
    table->compare = compare;

    memset(table->buckets, 0, sizeof(bucket_t) * size);

    return table;
}

null_t ht_free(ht_t *table)
{
    rf_free(table->buckets);
    rf_free(table);
}

null_t rehash(ht_t *table)
{
    u64_t i, old_size = table->size;
    bucket_t *old_buckets = table->buckets;

    // Double the table size.
    table->size *= 2;
    table->buckets = (bucket_t *)rf_malloc(table->size * sizeof(struct bucket_t));

    for (i = 0; i < old_size; i++)
    {
        if (old_buckets[i].state == STATE_OCCUPIED)
            ht_insert(table, old_buckets[i].key, old_buckets[i].val);
    }

    rf_free(old_buckets);
}

null_t rehash_with(ht_t *table, null_t *(*func)(null_t *key, null_t *val, bucket_t *bucket))
{
    u64_t i, old_size = table->size;
    bucket_t *old_buckets = table->buckets;

    // Double the table size.
    table->size *= 2;
    table->buckets = (bucket_t *)rf_malloc(table->size * sizeof(struct bucket_t));

    for (i = 0; i < old_size; i++)
    {
        if (old_buckets[i].state == STATE_OCCUPIED)
            ht_insert_with(table, old_buckets[i].key, old_buckets[i].val, func);
    }

    rf_free(old_buckets);
}

/*
 * Inserts new node or returns existing node.
 * Does not update existing node.
 */
null_t *ht_insert(ht_t *table, null_t *key, null_t *val)
{
    // Table's size is always a power of 2
    u64_t factor = table->size - 1,
          index = table->hasher(key) & factor;
    u32_t distance = 0, temp_distance;
    null_t *temp_key, *temp_val;

    while (distance < table->size)
    {
        if (table->buckets[index].state == STATE_OCCUPIED)
        {
            if (table->compare(table->buckets[index].key, key) == 0)
                return table->buckets[index].val;
        }

        if (table->buckets[index].state == STATE_EMPTY)
        {
            table->buckets[index].key = key;
            table->buckets[index].val = val;
            table->buckets[index].state = STATE_OCCUPIED;
            table->buckets[index].distance = distance;
            table->count++;
            return val;
        }

        if (table->buckets[index].distance < distance)
        {
            // Swap places
            temp_key = table->buckets[index].key;
            temp_val = table->buckets[index].val;
            temp_distance = table->buckets[index].distance;

            table->buckets[index].key = key;
            table->buckets[index].val = val;
            table->buckets[index].distance = distance;

            key = temp_key;
            val = temp_val;
            distance = temp_distance;
        }

        index = (index + 1) & factor;
        distance++;

        // Rehash if the load factor is above 0.7
        if (distance > table->size * 0.7)
        {
            rehash(table);
            // Start the insert operation from scratch, since the table has been resized and rehashed.
            return ht_insert(table, key, val);
        }
    }

    panic("Hash table is full");
}

/*
 * Does the same as ht_insert, but uses a function to set the rf_object of the bucket.
 */
null_t *ht_insert_with(ht_t *table, null_t *key, null_t *val,
                       null_t *(*func)(null_t *key, null_t *val, bucket_t *bucket))
{
    // Table's size is always a power of 2
    u64_t factor = table->size - 1,
          index = table->hasher(key) & factor;
    u32_t distance = 0;

    while (distance < table->size)
    {
        if (table->buckets[index].state == STATE_OCCUPIED)
        {
            if (table->compare(table->buckets[index].key, key) == 0)
                return table->buckets[index].val;
        }

        if (table->buckets[index].state == STATE_EMPTY)
        {
            // debug("INSERT INTO %d: hash: %lld, %s %lld", index, table->hasher(key), *(str_t *)key, (i64_t)val);
            table->buckets[index].state = STATE_OCCUPIED;
            table->count++;
            return func(key, val, &table->buckets[index]);
        }

        if (table->buckets[index].distance < distance)
        {
            // Swap places
            null_t *temp_key = table->buckets[index].key;
            null_t *temp_val = table->buckets[index].val;
            u64_t temp_distance = table->buckets[index].distance;

            table->buckets[index].key = key;
            table->buckets[index].val = val;
            table->buckets[index].distance = distance;

            key = temp_key;
            val = temp_val;
            distance = temp_distance;
        }

        index = (index + 1) & factor;
        distance++;

        // Rehash if the load factor is above 0.7
        if (distance > table->size * 0.7)
        {
            rehash_with(table, func);
            // Start the insert operation from scratch, since the table has been resized and rehashed.
            return ht_insert_with(table, key, val, func);
        }
    }

    panic("Hash table is full");
}

/*
 * Inserts new node or updates existing one.
 * Returns true if the node was updated, false if it was inserted.
 */
bool_t ht_update(ht_t *table, null_t *key, null_t *val)
{
    // Table's size is always a power of 2
    u64_t factor = table->size - 1,
          index = table->hasher(key) & factor;
    u32_t distance = 0;

    while (distance < table->size)
    {
        if (table->buckets[index].state == STATE_OCCUPIED)
        {
            if (table->compare(table->buckets[index].key, key) == 0)
            {
                table->buckets[index].key = key;
                table->buckets[index].val = val;
                return true;
            }
        }
        else
        {
            table->buckets[index].key = key;
            table->buckets[index].val = val;
            table->buckets[index].state = STATE_OCCUPIED;
            table->count++;
            return false;
        }

        if (table->buckets[index].distance < distance)
        {
            // Swap places
            null_t *temp_key = table->buckets[index].key;
            null_t *temp_val = table->buckets[index].val;
            u64_t temp_distance = table->buckets[index].distance;

            table->buckets[index].key = key;
            table->buckets[index].val = val;
            table->buckets[index].distance = distance;

            key = temp_key;
            val = temp_val;
            distance = temp_distance;
        }

        index = (index + 1) & factor;
        distance++;

        // Rehash if the load factor is above 0.7
        if (distance > table->size * 0.7)
        {
            rehash(table);
            // Start the insert operation from scratch, since the table has been resized and rehashed.
            return ht_update(table, key, val);
        }
    }

    panic("Hash table is full");
}

/*
 * Returns the rf_object of the node with the given key.
 * Returns -1 if the key does not exist.
 */
null_t *ht_get(ht_t *table, null_t *key)
{
    // Table's size is always a power of 2
    u64_t factor = table->size - 1,
          index = table->hasher(key) & factor;
    u32_t distance = 0;

    while (table->buckets[index].state == STATE_OCCUPIED &&
           distance <= table->buckets[index].distance)
    {
        if (table->compare(table->buckets[index].key, key) == 0)
            return table->buckets[index].val;

        index = (index + 1) & factor;
        distance++;
    }

    return (null_t *)NULL_I64;
}

bucket_t *ht_next_bucket(ht_t *table, u64_t *index)
{
    bucket_t *bucket;

    while (*index < table->size)
    {
        if (table->buckets[*index].state == STATE_OCCUPIED)
        {
            bucket = &table->buckets[*index];
            (*index)++;
            return bucket;
        }

        (*index)++;
    }

    return NULL;
}