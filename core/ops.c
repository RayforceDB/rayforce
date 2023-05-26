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

#include "string.h"
#include "ops.h"

/*
 * Incase of using -Ofast compiler flag, we can not just use x != x due to
 * compiler optimizations. So we need to use memcpy to get the bits of the x
 * and then separate check mantissa and exponent.
 */
bool_t rf_is_nan(f64_t x)
{
    u64_t bits;
    memcpy(&bits, &x, sizeof(x));
    return (bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (bits & 0x000FFFFFFFFFFFFFULL) != 0;
}

bool_t rf_eq(rf_object_t *x, rf_object_t *y)
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

bool_t rf_lt(rf_object_t *x, rf_object_t *y)
{
    if (x->type != y->type)
        return x->type - y->type;

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

i64_t round_f64(f64_t x)
{
    return x >= 0.0 ? (i64_t)(x + 0.5) : (i64_t)(x - 0.5);
}

i64_t floor_f64(f64_t x)
{
    return x >= 0.0 ? (i64_t)x : (i64_t)(x - 1.0);
}

i64_t ceil_f64(f64_t x)
{
    return x >= 0.0 ? (i64_t)(x + 1.0) : (i64_t)x;
}

i64_t rand_i64(i64_t *seed)
{
#define A 6364136223846793005LL
#define C 1442695040888963407LL
#define M (1LL << 63)
    *seed = (A * *seed + C) % M;
    return *seed;
}