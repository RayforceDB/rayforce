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
#include "ops.h"

obj_p ht_tab(u64_t size, i8_t vals);
obj_p ht_set(u64_t size);
i64_t ht_tab_next(obj_p *obj, i64_t key);
i64_t ht_tab_next_with(obj_p *obj, i64_t key, hash_f hash, cmp_f cmp, raw_p seed);
i64_t ht_tab_get(obj_p obj, i64_t key);
i64_t ht_tab_get_with(obj_p obj, i64_t key, hash_f hash, cmp_f cmp, raw_p seed);
nil_t rehash(obj_p *obj, hash_f hash, raw_p seed);

// Knuth's multiplicative hash
u64_t hash_kmh(i64_t key, raw_p seed);
// FNV-1a hash
u64_t hash_fnv1a(i64_t key, raw_p seed);
// Identity
u64_t hash_i64(i64_t a, raw_p seed);
u64_t hash_obj(i64_t a, raw_p seed);
u64_t hash_guid(i64_t a, raw_p seed);

i64_t hash_cmp_obj(i64_t a, i64_t b, raw_p seed);
i64_t hash_cmp_guid(i64_t a, i64_t b, raw_p seed);
i64_t hash_cmp_i64(i64_t a, i64_t b, raw_p seed);

// Special hashes
u64_t hash_index_obj(obj_p obj);
inline __attribute__((always_inline)) u64_t hash_index_u64(u64_t h, u64_t k)
{
    u64_t a, b, s = 0x9ddfea08eb382d69ull;

    a = (h ^ k) * s;
    a ^= (a >> 47);
    b = (roti64(k, 31) ^ a) * s;
    b ^= (b >> 47);
    b *= s;

    return b;
}

#endif // HASH_H
