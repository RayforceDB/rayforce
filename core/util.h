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

#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include "rayforce.h"
#include "format.h"

// A compile time assertion check
#define RAYASSERT(predicate, file) _IMPL_CASSERT_LINE(predicate, __LINE__, file)
#define _IMPL_PASTE(a, b) a##b
#define _IMPL_CASSERT_LINE(predicate, line, file) \
    typedef char _IMPL_PASTE(assertion_failed_##file##_, line)[2 * !!(predicate) - 1];

#define UNUSED(x) (nil_t)(x)

#define LIKELY(x) __builtin_expect((x), 1)
#define UNLIKELY(x) __builtin_expect((x), 0)

#ifdef DEBUG

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

nil_t dump_stack(nil_t);

#define DEBUG_PRINT(fmt, ...)                \
    do {                                     \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        fprintf(stderr, "\n");               \
        fflush(stderr);                      \
    } while (0)

#define DEBUG_ASSERT(x, fmt, ...)                      \
    {                                                  \
        if (!(x)) {                                    \
            fprintf(stderr, "-- ASSERTION FAILED:\n"); \
            fprintf(stderr, fmt, ##__VA_ARGS__);       \
            fprintf(stderr, "\n--\n");                 \
            fflush(stderr);                            \
            dump_stack();                              \
            assert(x);                                 \
        }                                              \
    }

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#define DEBUG_OBJ(o)                                    \
    {                                                   \
        obj_p _f = obj_fmt((o), B8_TRUE);               \
        DEBUG_PRINT("%.*s", (i32_t)_f->len, AS_C8(_f)); \
        drop_obj(_f);                                   \
    }

#else
#define DEBUG_PRINT(fmt, ...) (nil_t)0
#define DEBUG_ASSERT(x, fmt, ...) (nil_t)0
#define DEBUG_OBJ(o) (nil_t)0
#endif

#define PRINTBITS_N(x, n)                                                \
    {                                                                    \
        for (i64_t __i = n; __i; __i--, putchar('0' | ((x >> __i) & 1))) \
            ;                                                            \
        printf("\n");                                                    \
    }
#define PRINTBITS_32(x) PRINTBITS_N(x, 32)
#define PRINTBITS_64(x) PRINTBITS_N(x, 64)

#define TIMEIT(x)                                                           \
    {                                                                       \
        i64_t timer = clock();                                              \
        x;                                                                  \
        printf("%f\n", ((f64_t)(clock() - timer)) / CLOCKS_PER_SEC * 1000); \
    }

#define ENUM_KEY(x) \
    (x->mmod == MMOD_INTERNAL ? str_from_symbol(AS_LIST(x)[0]->i64) : AS_C8((obj_p)((str_p)x - RAY_PAGE_SIZE)))
#define ENUM_VAL(x) (x->mmod == MMOD_INTERNAL ? AS_LIST(x)[1] : x)

#define MAPLIST_KEY(x) (((obj_p)((str_p)x - RAY_PAGE_SIZE))->obj)
#define MAPLIST_VAL(x) (x)

#define __TYPE_u8 TYPE_U8
#define __TYPE_b8 TYPE_B8
#define __TYPE_c8 TYPE_C8
#define __TYPE_i32 TYPE_I32
#define __TYPE_date TYPE_DATE
#define __TYPE_time TYPE_TIME
#define __TYPE_timestamp TYPE_TIMESTAMP
#define __TYPE_i64 TYPE_I64
#define __TYPE_f64 TYPE_F64
#define __TYPE_guid TYPE_GUID
#define __TYPE_symbol TYPE_SYMBOL
#define __TYPE_list TYPE_LIST

// Base type for use in pointer declarations
#define __BASE_u8 u8
#define __BASE_b8 b8
#define __BASE_c8 c8
#define __BASE_i16 i16
#define __BASE_i32 i32
#define __BASE_date i32
#define __BASE_time i32
#define __BASE_timestamp i64
#define __BASE_i64 i64
#define __BASE_f64 f64
#define __BASE_guid guid
#define __BASE_symbol i64

// Base type with _t suffix for use in pointer declarations
#define __BASE_u8_t u8_t
#define __BASE_b8_t b8_t
#define __BASE_c8_t c8_t
#define __BASE_i16_t i16_t
#define __BASE_i32_t i32_t
#define __BASE_date_t i32_t
#define __BASE_time_t i32_t
#define __BASE_timestamp_t i64_t
#define __BASE_i64_t i64_t
#define __BASE_f64_t f64_t
#define __BASE_guid_t guid_t
#define __BASE_symbol_t i64_t
#define __BASE_list_t obj_p

#define __v_i8(x) I8(x)
#define __v_u8(x) U8(x)
#define __v_b8(x) B8(x)
#define __v_c8(x) C8(x)
#define __v_symbol(x) SYMBOL(x)
#define __v_i64(x) I64(x)
#define __v_time(x) TIME(x)
#define __v_date(x) DATE(x)
#define __v_timestamp(x) TIMESTAMP(x)
#define __v_f64(x) F64(x)
#define __v_guid(x) GUID(x)
#define __v_list(x) LIST(x)

#define __AS_i8(x) AS_I8(x)
#define __AS_u8(x) AS_U8(x)
#define __AS_b8(x) AS_B8(x)
#define __AS_c8(x) AS_C8(x)
#define __AS_i16(x) AS_I16(x)
#define __AS_i32(x) AS_I32(x)
#define __AS_symbol(x) AS_SYMBOL(x)
#define __AS_i64(x) AS_I64(x)
#define __AS_time(x) AS_TIME(x)
#define __AS_date(x) AS_DATE(x)
#define __AS_timestamp(x) AS_TIMESTAMP(x)
#define __AS_f64(x) AS_F64(x)
#define __AS_guid(x) AS_GUID(x)
#define __AS_list(x) AS_LIST(x)

#define __SIZE_OF_i8 sizeof(i8_t)
#define __SIZE_OF_u8 sizeof(u8_t)
#define __SIZE_OF_b8 sizeof(b8_t)
#define __SIZE_OF_c8 sizeof(c8_t)
#define __SIZE_OF_i16 sizeof(i16_t)
#define __SIZE_OF_i32 sizeof(i32_t)
#define __SIZE_OF_i64 sizeof(i64_t)
#define __SIZE_OF_time sizeof(i32_t)
#define __SIZE_OF_date sizeof(i32_t)
#define __SIZE_OF_timestamp sizeof(i64_t)
#define __SIZE_OF_f64 sizeof(f64_t)
#define __SIZE_OF_guid sizeof(guid_t)
#define __SIZE_OF_list sizeof(obj_p)

#define __NULL_X(x) NULL_##x
#define __NULL_b8 __NULL_X(b8)
#define __NULL_i8() 0
#define __NULL_u8 0
#define __NULL_c8 ""
#define __NULL_i32 NULL_I32
#define __NULL_date NULL_I32
#define __NULL_time NULL_I32
#define __NULL_i64 __NULL_X(I64)
#define __NULL_timestamp NULL_I64
#define __NULL_f64 NULL_F64

#define XFIRST(x, p) ((x->len == 0) ? __NULL_##p : __AS_##p(x)[0])

#ifdef __cplusplus
// C++: Use empty braces {} for value initialization
#define ZERO_INIT_STRUCT \
    {}
#else
// C: Use {0} for aggregate zero-initialization
#define ZERO_INIT_STRUCT {0}
#endif

b8_t is_valid(obj_p obj);
u32_t next_power_of_two_u32(u32_t n);
i64_t next_power_of_two_u64(i64_t n);

#endif  // UTIL_H
