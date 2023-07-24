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

#include <time.h>
#include "string.h"
#include "ops.h"

static u64_t __RND_SEED__ = 0;

/*
 * Incase of using -Ofast compiler flag, we can not just use x != x due to
 * compiler optimizations. So we need to use memcpy to get the bits of the x
 * and then separate check mantissa and exponent.
 */
bool_t rfi_is_nan(f64_t x)
{
    u64_t bits;
    memcpy(&bits, &x, sizeof(x));
    return (bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (bits & 0x000FFFFFFFFFFFFFULL) != 0;
}

bool_t rfi_eq(obj_t x, obj_t y)
{
    if (x->type != y->type)
        return false;

    switch (x->type)
    {
    case -TYPE_BOOL:
        return x->bool == y->bool;
    case -TYPE_I64:
        return x->i64 == y->i64;
    case -TYPE_F64:
        return x->f64 == y->f64;
    case TYPE_CHAR:
        return strcmp(as_string(x), as_string(y)) == 0;
    default:
        return false;
    }
}

bool_t rfi_lt(obj_t x, obj_t y)
{
    if (x->type != y->type)
        return false;

    switch (x->type)
    {
    case -TYPE_BOOL:
        return x->bool - y->bool;
    case -TYPE_I64:
        return x->i64 < y->i64;
    case -TYPE_F64:
        return x->f64 < y->f64;
    case TYPE_CHAR:
        return strcmp(as_string(x), as_string(y)) < 0;
    default:
        return 0;
    }
}

i64_t rfi_round_f64(f64_t x)
{
    return x >= 0.0 ? (i64_t)(x + 0.5) : (i64_t)(x - 0.5);
}

i64_t rfi_floor_f64(f64_t x)
{
    return x >= 0.0 ? (i64_t)x : (i64_t)(x - 1.0);
}

i64_t rfi_ceil_f64(f64_t x)
{
    return x >= 0.0 ? (i64_t)(x + 1.0) : (i64_t)x;
}

u64_t rfi_rand_u64()
{
#define A 6364136223846793005ULL
#define C 1442695040888963407ULL
#define M (1ULL << 63)
    __RND_SEED__ += time(0);
    __RND_SEED__ = (A * __RND_SEED__ + C) % M;
    return __RND_SEED__;
}

u64_t rfi_kmh_hash(i64_t key)
{
#define LARGE_PRIME 6364136223846793005ULL
    return (key * LARGE_PRIME) >> 32;
}

u64_t rfi_fnv1a_hash_64(i64_t key)
{
#define FNV_OFFSET_64 14695981039346656037ULL
#define FNV_PRIME_64 1099511628211ULL
    u64_t hash = FNV_OFFSET_64;
    i32_t i;
    for (i = 0; i < 8; i++)
    {
        u8_t byte = (key >> (i * 8)) & 0xff;
        hash ^= byte;
        hash *= FNV_PRIME_64;
    }

    return hash;
}

u64_t rfi_i64_hash(i64_t key)
{
    return (u64_t)key;
}

bool_t rfi_as_vector_bool(obj_t x)
{
    switch (x->type)
    {
    case -TYPE_BOOL:
        return x->bool;
    case -TYPE_I64:
        return x->i64 != 0;
    case -TYPE_F64:
        return x->f64 != 0.0;
    case -TYPE_CHAR:
        return x->schar != 0;
    case TYPE_BOOL:
    case TYPE_I64:
    case TYPE_F64:
    case TYPE_CHAR:
    case TYPE_LIST:
        if (x->ptr == NULL)
            return false;
        return x->len != 0;
    default:
        return false;
    }
}
