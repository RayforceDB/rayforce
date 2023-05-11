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

hash_table_t *ht_new(i32_t size, i64_t (*hasher)(null_t *a), i32_t (*compare)(null_t *a, null_t *b))
{
    hash_table_t *table = (hash_table_t *)mmap(NULL, sizeof(hash_table_t),
                                               PROT_READ | PROT_WRITE,
                                               MAP_ANONYMOUS | MAP_PRIVATE,
                                               -1, 0);

    bucket_t **buckets = (bucket_t **)mmap(NULL, sizeof(bucket_t *) * size,
                                           PROT_READ | PROT_WRITE,
                                           MAP_ANONYMOUS | MAP_PRIVATE,
                                           -1, 0);

    memset(buckets, 0, sizeof(bucket_t *) * size);

    table->cap = size;
    table->buckets = buckets;
    table->size = 0;
    table->hasher = hasher;
    table->compare = compare;

    return table;
}

null_t ht_free(hash_table_t *table)
{
    i32_t i;
    bucket_t *bucket, *next;

    for (i = 0; i < table->cap; i++)
    {
        bucket = table->buckets[i];

        while (bucket)
        {
            next = bucket->next;
            rf_free(bucket);
            bucket = next;
        }
    }

    munmap(table->buckets, sizeof(bucket_t *) * table->cap);
}

/*
 * Inserts new node or returns existing node.
 * Does not update existing node.
 */
null_t *ht_insert(hash_table_t *table, null_t *key, null_t *val)
{
    // Table's size is always a power of 2
    i32_t index = table->hasher(key) & (table->cap - 1);
    bucket_t **bucket = &table->buckets[index];

    while (*bucket)
    {
        // Key already exists, return it
        if (table->compare((*bucket)->key, key) == 0)
            return (*bucket)->val;

        bucket = &((*bucket)->next);
    }

    // Add new bucket to the end of the list
    (*bucket) = (bucket_t *)rf_malloc(sizeof(bucket_t));
    (*bucket)->key = key;
    (*bucket)->val = val;
    (*bucket)->next = NULL;

    table->size++;

    return val;
}

/*
 * Does the same as ht_insert, but uses a function to set the rf_object of the bucket.
 */
null_t *ht_insert_with(hash_table_t *table, null_t *key, null_t *val, null_t *(*func)(null_t *key, null_t *val, bucket_t *bucket))
{
    // Table's size is always a power of 2
    i32_t index = table->hasher(key) & (table->cap - 1);
    bucket_t **bucket = &table->buckets[index];

    while (*bucket)
    {
        // Key already exists, return it
        if (table->compare((*bucket)->key, key) == 0)
            return (*bucket)->val;

        bucket = &((*bucket)->next);
    }

    // Add new bucket to the end of the list
    (*bucket) = (bucket_t *)rf_malloc(sizeof(bucket_t));
    (*bucket)->next = NULL;

    table->size++;

    return func(key, val, *bucket);
}

/*
 * Inserts new node or updates existing one.
 * Returns true if the node was updated, false if it was inserted.
 */
bool_t ht_update(hash_table_t *table, null_t *key, null_t *val)
{
    // Table's size is always a power of 2
    i32_t index = table->hasher(key) & (table->cap - 1);
    bucket_t **bucket = &table->buckets[index];

    while (*bucket)
    {
        // Key already exists, update it
        if (table->compare((*bucket)->key, key) == 0)
        {
            (*bucket)->val = val;
            return true;
        }

        bucket = &((*bucket)->next);
    }

    // Add new bucket to the end of the list
    (*bucket) = (bucket_t *)rf_malloc(sizeof(bucket_t));
    (*bucket)->key = key;
    (*bucket)->val = val;
    (*bucket)->next = NULL;

    table->size++;

    return false;
}

/*
 * Returns the rf_object of the node with the given key.
 * Returns NULL if the key does not exist.
 */
null_t *ht_get(hash_table_t *table, null_t *key)
{
    i32_t index = table->hasher(key) & (table->cap - 1);
    bucket_t *bucket = table->buckets[index];

    while (bucket)
    {
        if (table->compare(bucket->key, key) == 0)
            return bucket->val;

        bucket = bucket->next;
    }

    return NULL;
}
