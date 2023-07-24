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

#ifndef OPS_H
#define OPS_H

#include "rayforce.h"

// internal types definitions:
//
// parser token type
#define TYPE_TOKEN (TYPE_ERROR - 1)
// vm context type
#define TYPE_CTX (TYPE_ERROR - 2)
// trap type
#define TYPE_TRAP (TYPE_ERROR - 3)
// trow type
#define TYPE_THROW (TYPE_ERROR - 4)
// none type
#define TYPE_NONE (TYPE_ERROR - 5)
// FLAGS
#define FLAG_NONE 0
#define FLAG_LEFT_ATOMIC 1
#define FLAG_RIGHT_ATOMIC 2
#define FLAG_ATOMIC 4

#define align8(x) ((str_t)(((u64_t)x + 7) & ~7))
// --

#define MTYPE(x) ((u8_t)(x))
#define MTYPE2(x, y) ((u8_t)(x) | ((u8_t)(y) << 8))
#define MTYPE3(x, y, z) ((u8_t)(x) | ((u8_t)(y) << 8) | ((u8_t)(z) << 16))
#define MTYPE4(x, y, z, w) ((u8_t)(x) | ((u8_t)(y) << 8) | ((u8_t)(z) << 16) | ((u8_t)(w) << 24))

#define IS_NULL_I64(x) (((x) >> 63) & 1)
#define ADDvector_i64(x, y) ((((x) + (y)) & ~((i64_t)IS_NULL_I64(x) | (i64_t)IS_NULL_I64(y))) | ((i64_t)IS_NULL_I64(x) | (i64_t)IS_NULL_I64(y)) << 63)
#define ADDvector_f64(x, y) (x + y)
#define SUBvector_i64(x, y) ((x == NULL_I64 || y == NULL_I64) ? NULL_I64 : (x - y))
#define SUBvector_f64(x, y) (x - y)
#define MULvector_i64(x, y) ((((x) * (y)) & ~((i64_t)IS_NULL_I64(x) | (i64_t)IS_NULL_I64(y))) | ((i64_t)IS_NULL_I64(x) | (i64_t)IS_NULL_I64(y)) << 63)
#define MULvector_f64(x, y) (x * y)
#define DIVvector_i64(x, y) ((x == NULL_I64 || y == NULL_I64) ? NULL_F64 : ((x) / (y)))
#define FDIVvector_i64(x, y) ((x == NULL_I64 || y == NULL_I64) ? NULL_F64 : ((f64_t)(x) / (f64_t)(y)))
#define DIVvector_f64(x, y) ((i64_t)(x / y))
#define FDIVvector_f64(x, y) (x / y)
#define MODvector_i64(x, y) ((x == NULL_I64 || y == NULL_I64) ? NULL_I64 : ((x) % (y)))
#define MODvector_f64(x, y) (x - y * ((i64_t)(x / y)))
#define MAXvector_i64(x, y) (x > y ? x : y)
#define MAXvector_f64(x, y) (x > y ? x : y)
#define MINvector_i64(x, y) (IS_NULL_I64(y) || (!IS_NULL_I64(x) && (x < y)) ? x : y)
#define MINvector_f64(x, y) (x < y ? x : y)
/*
 * Aligns x to the nearest multiple of a
 */
#define ALIGNUP(x, a) (((x) + (a)-1) & ~((a)-1))

bool_t rfi_is_nan(f64_t x);
bool_t rfi_eq(obj_t x, obj_t y);
bool_t rfi_lt(obj_t x, obj_t y);
bool_t rfi_as_vector_bool(obj_t x);

i64_t rfi_round_f64(f64_t x);
i64_t rfi_floor_f64(f64_t x);
i64_t rfi_ceil_f64(f64_t x);
u64_t rfi_rand_u64();
// Knuth's multiplicative hash
u64_t rfi_kmh_hash(i64_t key);
u64_t rfi_fnv1a_hash_64(i64_t key);
u64_t rfi_i64_hash(i64_t a);

#endif
