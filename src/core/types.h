/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef RAY_TYPES_H
#define RAY_TYPES_H

/*
 * types.h — Internal types header.
 *
 * The canonical type definitions (ray_t, type constants, attribute flags)
 * live in <rayforce.h> (the public header).
 * Internal .c files can include either rayforce.h directly or types.h.
 */
#include <rayforce.h>
#include <float.h>

/* Number of types (positive range): must be > max type ID */
#define RAY_TYPE_COUNT 17

/* Type sizes lookup table (defined in types.c) */
extern const uint8_t ray_type_sizes[256];

/* Element size for a given type tag.  Cast to uint8_t so a negative (atom)
 * type tag indexes the upper, zero-filled half of the 256-entry table rather
 * than reading out of bounds — ray_type_sizes[(int8_t)-1] is an OOB read (UB).
 * Non-vector/atom tags legitimately have no element size and read back 0, and
 * this matches the (uint8_t) indexing already used in vec.c. */
#define ray_elem_size(t)  (ray_type_sizes[(uint8_t)(t)])

static inline int64_t ray_cast_f64_to_i64_null(double v) {
    if (v != v) return NULL_I64;
    if (v >= (double)INT64_MAX) return INT64_MAX;
    if (v <= (double)INT64_MIN) return INT64_MIN + 1;
    return (int64_t)v;
}

static inline int32_t ray_cast_f64_to_i32_null(double v) {
    if (v != v) return NULL_I32;
    if (v >= (double)INT32_MAX) return INT32_MAX;
    if (v <= (double)INT32_MIN) return INT32_MIN + 1;
    return (int32_t)v;
}

static inline int16_t ray_cast_f64_to_i16_null(double v) {
    if (v != v) return NULL_I16;
    if (v >= (double)INT16_MAX) return INT16_MAX;
    if (v <= (double)INT16_MIN) return INT16_MIN + 1;
    return (int16_t)v;
}

/* F32 has no infinities either: a value past FLT_MAX saturates, as the
 * integer narrowings above do.  NaN (the float null) stays null. */
static inline float ray_cast_f64_to_f32_null(double v) {
    if (v != v) return NULL_F32;
    if (v >= (double)FLT_MAX) return FLT_MAX;
    if (v <= -(double)FLT_MAX) return -FLT_MAX;
    return (float)v;
}

/* A computed F32 result (aggregate extreme, first/last) follows the single-null
 * float model instead: past FLT_MAX or non-finite is the null, never a
 * clamped value that is not in the column.  NaN fails the comparison. */
static inline float ray_f64_to_f32_fin(double v) {
    return (v <= (double)FLT_MAX && v >= -(double)FLT_MAX) ? (float)v : NULL_F32;
}

static inline uint8_t ray_cast_f64_to_u8_null(double v) {
    if (v != v || v <= 0.0) return 0;
    if (v >= (double)UINT8_MAX) return UINT8_MAX;
    return (uint8_t)v;
}

static inline uint8_t ray_cast_f64_to_bool_null(double v) {
    /* BOOL has no null: every non-zero value, including NaN, is true. */
    return v != 0.0 ? 1 : 0;
}

/* Two's-complement wraparound integer add/mul.  Signed overflow is UB in C;
 * aggregate reductions (sum / prod, and their parted / table-scan / affine
 * kernels) use these so an overflowing accumulator wraps deterministically
 * rather than trapping.  At -O2/-O3 they inline to a bare add/mul and do not
 * inhibit autovectorization (signed and unsigned 64-bit add are the same ISA
 * instruction). */
static inline int64_t wrap_add_i64(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a + (uint64_t)b);
}
static inline int64_t wrap_mul_i64(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a * (uint64_t)b);
}

/* The numeric promotion table (#972), the one rule for atoms, vectors, the
 * planner and the fused evaluator: B8 < U8 < I16 < I32 < I64 < F32 < F64 and
 * the wider type wins, two operands of one type keep it (an integer result
 * wraps in that width), and F32 beside any other type is F64.  Symmetric.
 * Takes and returns vector (positive) type codes; 0 when either operand is
 * not one of the seven numeric types.  The type codes are the ranks. */
_Static_assert(RAY_BOOL == 1 && RAY_U8 == 2 && RAY_I16 == 3 && RAY_I32 == 4 &&
               RAY_I64 == 5 && RAY_F32 == 6 && RAY_F64 == 7,
               "ray_num_type ranks numeric types by their codes");
static inline int8_t ray_num_type(int8_t a, int8_t b) {
    if ((uint8_t)(a - RAY_BOOL) > RAY_F64 - RAY_BOOL ||
        (uint8_t)(b - RAY_BOOL) > RAY_F64 - RAY_BOOL) return 0;
    if (a == b) return a;
    if (a == RAY_F32 || b == RAY_F32) return RAY_F64;
    return a > b ? a : b;
}

/* The table for + - * div %: as ray_num_type, but B8 with B8 counts as I64
 * (`(+ true true)` is 2).  min and max keep B8 (ray_num_type). */
static inline int8_t ray_arith_type(int8_t a, int8_t b) {
    int8_t t = ray_num_type(a, b);
    return t == RAY_BOOL ? RAY_I64 : t;
}

#endif /* RAY_TYPES_H */
