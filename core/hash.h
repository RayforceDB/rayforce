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

#include "rayforce.h"

typedef struct bucket_t
{
    i64_t key;
    i64_t val;
} bucket_t;

typedef struct ht_t
{
    u64_t (*hasher)(i64_t a);
    i32_t (*compare)(i64_t a, i64_t b);
    i64_t size;
    i64_t count;
    bucket_t *buckets;
} ht_t;

// clang-format off
ht_t   *ht_new(i64_t size, u64_t (*hasher)(i64_t a), i32_t (*compare)(i64_t a, i64_t b));
null_t  ht_free(ht_t *table);
i64_t   ht_insert(ht_t *table, i64_t key, i64_t val);
i64_t   ht_insert_with(ht_t *table, i64_t key, i64_t val, null_t *seed,
                  i64_t (*func)(i64_t key, i64_t val, null_t *seed, i64_t *tkey, i64_t *tval));
bool_t  ht_upsert(ht_t *table, i64_t key, i64_t val);
bool_t  ht_upsert_with(ht_t *table, i64_t key, i64_t val, null_t *seed,
                  bool_t (*func)(i64_t key, i64_t val, null_t *seed, i64_t *tkey, i64_t *tval));
i64_t   ht_get(ht_t *table, i64_t key);
// clang-format on

#endif
