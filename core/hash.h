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

#ifndef HASH_H
#define HASH_H

#include "storm.h"

#define DEFAULT_SIZE 4096 * 1024

typedef struct bucket_t
{
    null_t *key;
    null_t *val;
    struct bucket_t *next;
} bucket_t;

typedef struct hash_table_t
{
    u64_t cap;  // Total capacity of the table (always a multiplier of DEFAULT_SIZE)
    u64_t size; // Actual size of the table in elements
    bucket_t **buckets;
    u64_t (*hasher)(null_t *a);
    i32_t (*compare)(null_t *a, null_t *b);
} hash_table_t;

hash_table_t *ht_create(u64_t (*hasher)(null_t *a), i32_t (*compare)(null_t *a, null_t *b));
null_t ht_free(hash_table_t *table);

null_t *ht_insert(hash_table_t *table, null_t *key, null_t *val);
null_t *ht_insert_with(hash_table_t *table, null_t *key, null_t *val, null_t *(*func)(null_t *key, null_t *val, bucket_t *bucket));
null_t *ht_get(hash_table_t *table, null_t *key);

#endif
