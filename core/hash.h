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

// GCC/Clang vector extensions - portable SIMD
// Compiler generates optimal code for target (AVX2, NEON, SVE, etc.)
typedef u64_t v4u64 __attribute__((vector_size(32)));  // 4 x u64 = 256 bits
typedef u64_t v2u64 __attribute__((vector_size(16)));  // 2 x u64 = 128 bits

#define U64_HASH_SEED 0x9ddfea08eb382d69ull

// Single threaded open addressing hash table
obj_p ht_oa_create(i64_t size, i8_t vals);
i64_t ht_oa_tab_next(obj_p *obj, i64_t key);
i64_t ht_oa_tab_next_with(obj_p *obj, i64_t key, hash_f hash, cmp_f cmp, raw_p seed);
i64_t ht_oa_tab_insert(obj_p *obj, i64_t key, i64_t val);
i64_t ht_oa_tab_insert_with(obj_p *obj, i64_t key, i64_t val, hash_f hash, cmp_f cmp, raw_p seed);
i64_t ht_oa_tab_get(obj_p obj, i64_t key);
i64_t ht_oa_tab_get_with(obj_p obj, i64_t key, hash_f hash, cmp_f cmp, raw_p seed);
nil_t ht_oa_rehash(obj_p *obj, hash_f hash, raw_p seed);

// Multithreaded lockfree hash table
typedef struct bucket_t {
    i64_t key;
    i64_t val;
    struct bucket_t *next;
} *bucket_p;

typedef struct ht_bk_t {
    i64_t size;
    i64_t count;
    bucket_p table[];
} *ht_bk_p;

ht_bk_p ht_bk_create(i64_t size);
nil_t ht_bk_destroy(ht_bk_p ht);
nil_t ht_bk_rehash(ht_bk_p *ht, i64_t new_size);
i64_t ht_bk_insert(ht_bk_p ht, i64_t key, i64_t val);
i64_t ht_bk_insert_par(ht_bk_p ht, i64_t key, i64_t val);
i64_t ht_bk_insert_with(ht_bk_p ht, i64_t key, i64_t val, hash_f hash, cmp_f cmp, raw_p seed);
i64_t ht_bk_insert_with_par(ht_bk_p ht, i64_t key, i64_t val, hash_f hash, cmp_f cmp, raw_p seed);
i64_t ht_bk_get(ht_bk_p ht, i64_t key);

// Hash functions
u64_t hash_kmh(i64_t key, raw_p seed);
u64_t hash_fnv1a(i64_t key, raw_p seed);
u64_t hash_murmur3(i64_t key, raw_p seed);

// Identity
u64_t hash_i64(i64_t a, raw_p seed);
u64_t hash_obj(i64_t a, raw_p seed);
u64_t hash_guid(i64_t a, raw_p seed);

// Compare
i64_t hash_cmp_obj(i64_t a, i64_t b, raw_p seed);
i64_t hash_cmp_guid(i64_t a, i64_t b, raw_p seed);
i64_t hash_cmp_i64(i64_t a, i64_t b, raw_p seed);

// Special hashes
u64_t hash_index_obj(obj_p obj);
inline __attribute__((always_inline)) u64_t hash_index_u64(u64_t h, u64_t k) {
    const u64_t s = U64_HASH_SEED;
    u64_t a, b;

    a = (h ^ k) * s;
    a ^= (a >> 47);
    b = (ROTI64(k, 31) ^ a) * s;
    b ^= (b >> 47);
    b *= s;

    return b;
}

// Vectorized hash computation using GCC/Clang vector extensions
// Processes 4 values at once - compiler generates optimal SIMD for target
static inline __attribute__((always_inline)) void hash_index_u64_vec4(u64_t *out, const u64_t *vals) {
    const v4u64 seed = {U64_HASH_SEED, U64_HASH_SEED, U64_HASH_SEED, U64_HASH_SEED};

    v4u64 h, k, a, b, k_rot;

    // Load 4 values
    __builtin_memcpy(&h, out, sizeof(v4u64));
    __builtin_memcpy(&k, vals, sizeof(v4u64));

    // a = (h ^ k) * s
    a = (h ^ k) * seed;
    // a ^= (a >> 47)
    a ^= (a >> 47);

    // k_rot = ROTI64(k, 31) = (k << 31) | (k >> 33)
    k_rot = (k << 31) | (k >> 33);

    // b = (k_rot ^ a) * s
    b = (k_rot ^ a) * seed;
    // b ^= (b >> 47)
    b ^= (b >> 47);
    // b *= s
    b *= seed;

    // Store result
    __builtin_memcpy(out, &b, sizeof(v4u64));
}

// Batch hash computation - uses vector extensions
static inline void hash_index_i64_batch(u64_t *out, const u64_t *vals, i64_t len) {
    i64_t i = 0;

    // Process 4 elements at a time with vector ops
    for (; i + 4 <= len; i += 4) {
        hash_index_u64_vec4(out + i, vals + i);
    }

    // Handle remainder with scalar
    // NOTE: hash_index_u64(h, k) takes seed h first, value k second - same as vec4
    for (; i < len; i++) {
        out[i] = hash_index_u64(out[i], vals[i]);
    }
}

#endif  // HASH_H
