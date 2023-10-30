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

// Function's attributes
#define FN_NONE 0
#define FN_LEFT_ATOMIC 1
#define FN_RIGHT_ATOMIC 2
#define FN_ATOMIC 4

// Object's attributes
#define ATTR_DISTINCT 1
#define ATTR_ASC 2
#define ATTR_DESC 4
#define ATTR_QUOTED 8
#define ATTR_MULTIEXPR 16

// Memory modes
#define MMOD_INTERNAL 0xff
#define MMOD_EXTERNAL_SIMPLE 0xfd
#define MMOD_EXTERNAL_COMPOUND 0xfe
#define MMOD_EXTERNAL_SERIALIZED 0xfa

#define is_internal(x) ((((x)->mmod) & MMOD_INTERNAL) == (x)->mmod)
#define is_external_simple(x) ((((x)->mmod) & MMOD_EXTERNAL_SIMPLE) == (x)->mmod)
#define is_external_compound(x) ((((x)->mmod) & MMOD_EXTERNAL_COMPOUND) == (x)->mmod)
#define is_external_serialized(x) ((((x)->mmod) & MMOD_EXTERNAL_SERIALIZED) == (x)->mmod)

/*
 * Aligns x to the nearest multiple of a
 */
#define alignup(x, a) (((x) + (a)-1) & ~((a)-1))
#define align8(x) ((str_t)(((u64_t)x + 7) & ~7))

#define mtype2(x, y) ((u8_t)(x) | ((u8_t)(y) << 8))

#define absi64(x) ((x) == NULL_I64 ? 0 : (((x) < 0 ? -(x) : (x))))
#define addi64(x, y) (((x) == NULL_I64 || (y) == NULL_I64) ? NULL_I64 : (x) + (y))
#define addf64(x, y) ((x) + (y))
#define subi64(x, y) ((x == NULL_I64 || y == NULL_I64) ? NULL_I64 : (x) - (y))
#define subf64(x, y) ((x) - (y))
#define muli64(x, y) (((x) == NULL_I64 || (y) == NULL_I64) ? NULL_I64 : (x) * (y))
#define mulf64(x, y) ((x) * (y))
#define divi64(x, y) (((y) == 0) ? NULL_I64 : (((x) == NULL_I64 || (y) == NULL_I64) ? NULL_F64 : ((x) / (y))))
#define fdivi64(x, y) (((x) == NULL_I64 || (y) == NULL_I64) ? NULL_F64 : ((f64_t)(x) / (f64_t)(y)))
#define divf64(x, y) ((i64_t)((x) / (y)))
#define fdivf64(x, y) ((x) / (y))
#define modi64(x, y) (((y) == 0) ? NULL_I64 : (((x) == NULL_I64 || (y) == NULL_I64) ? NULL_I64 : (((i64_t)(x)) % ((i64_t)(y)))))
#define modf64(x, y) ((x) - (y) * ((i64_t)((x) / (y))))
#define maxi64(x, y) ((x) > (y) ? (x) : (y))
#define maxf64(x, y) ((x) > (y) ? (x) : (y))
#define mini64(x, y) (((y) == NULL_I64) || (((x) != NULL_I64) && ((x) < (y))) ? (x) : (y))
#define minf64(x, y) ((x) < (y) ? (x) : (y))
#define roti32(x, y) (((x) << (y)) | ((x) >> (32 - (y))))
#define roti64(x, y) (((x) << (y)) | ((x) >> (64 - (y))))

// Function types
typedef u64_t (*hash_f)(i64_t, nil_t *);
typedef i32_t (*cmp_f)(i64_t, i64_t, nil_t *);
typedef obj_t (*unary_f)(obj_t);
typedef obj_t (*binary_f)(obj_t, obj_t);
typedef obj_t (*vary_f)(obj_t *, i64_t n);

i32_t i64_cmp(i64_t a, i64_t b, nil_t *);

bool_t rfi_is_nan(f64_t x);
bool_t rfi_eq(obj_t x, obj_t y);
bool_t rfi_lt(obj_t x, obj_t y);
bool_t rfi_as_bool(obj_t x);

i64_t rfi_round_f64(f64_t x);
i64_t rfi_floor_f64(f64_t x);
i64_t rfi_ceil_f64(f64_t x);
u64_t rfi_rand_u64();

// Knuth's multiplicative hash
u64_t rfi_kmh_hash(i64_t key, nil_t *seed);
// FNV-1a hash
u64_t rfi_fnv1a_hash_64(i64_t key, nil_t *seed);
// Identity
u64_t rfi_i64_hash(i64_t a, nil_t *seed);
obj_t distinct(obj_t x);
obj_t group(i64_t values[], i64_t indices[], i64_t len);
u64_t count(obj_t x);
u64_t hash_obj(obj_t obj);

typedef enum
{
    ERROR_TYPE_OS,
    ERROR_TYPE_SYS,
    ERROR_TYPE_SOCK
} os_error_type_t;

obj_t sys_error(os_error_type_t, str_t msg);

#endif
