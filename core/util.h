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
#include "rayforce.h"

// A compile time assertion check
#define CASSERT(predicate, file) _IMPL_CASSERT_LINE(predicate, __LINE__, file)
#define _IMPL_PASTE(a, b) a##b
#define _IMPL_CASSERT_LINE(predicate, line, file) \
    typedef char _IMPL_PASTE(assertion_failed_##file##_, line)[2 * !!(predicate)-1];

#define UNUSED(x) (void)(x)

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

#ifdef DEBUG
#define debug(fmt, ...)                      \
    do                                       \
    {                                        \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        fprintf(stderr, "\n");               \
        fflush(stderr);                      \
    } while (0)

#define debug_assert(x) (assert(x))

#define debug_obj(o)                \
    {                                  \
        str_t _f = obj_fmt((o)); \
        debug("%s", _f);               \
        heap_free(_f);                   \
    }

#else
#define debug(fmt, ...) (nil_t)0
#define debug_assert(x) (nil_t)0
#endif

#define panic(x)                                                   \
    {                                                              \
        fprintf(stderr, "Process panicked with message: '%s'", x); \
        exit(1);                                                   \
    }

#define printbits_n(x, n)                                        \
    {                                                            \
        for (i32_t i = n; i; i--, putchar('0' | ((x >> i) & 1))) \
            ;                                                    \
        printf("\n");                                            \
    }
#define printbits_32(x) printbits_n(x, 32)

#define timeit(x)                                                           \
    {                                                                       \
        i64_t timer = clock();                                              \
        x;                                                                  \
        printf("%f\n", ((f64_t)(clock() - timer)) / CLOCKS_PER_SEC * 1000); \
    }

u32_t next_power_of_two_u32(u32_t n);
u64_t next_power_of_two_u64(u64_t n);

obj_t error_type1(type_t type, str_t msg);
obj_t error_type2(type_t type1, type_t type2, str_t msg);

#endif
