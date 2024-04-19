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
#define CASSERT(predicate, file) _IMPL_CASSERT_LINE(predicate, __LINE__, file)
#define _IMPL_PASTE(a, b) a##b
#define _IMPL_CASSERT_LINE(predicate, line, file) \
    typedef char _IMPL_PASTE(assertion_failed_##file##_, line)[2 * !!(predicate) - 1];

#define unused(x) (nil_t)(x)

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

#ifdef DEBUG

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

nil_t dump_stack(nil_t);

#define debug(fmt, ...)                      \
    do                                       \
    {                                        \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        fprintf(stderr, "\n");               \
        fflush(stderr);                      \
    } while (0)

#define debug_assert(x, fmt, ...)                      \
    {                                                  \
        if (!(x))                                      \
        {                                              \
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

#define debug_obj(o)                                  \
    {                                                 \
        obj_p _f = obj_fmt((o));                      \
        debug("%.*s", (i32_t)_f->len, as_string(_f)); \
        heap_free_obj(_f);                            \
    }

#else
#define debug(fmt, ...) (nil_t)0
#define debug_assert(x, fmt, ...) (nil_t)0
#endif

#define printbits_n(x, n)                                                \
    {                                                                    \
        for (u64_t __i = n; __i; __i--, putchar('0' | ((x >> __i) & 1))) \
            ;                                                            \
        printf("\n");                                                    \
    }
#define printbits_32(x) printbits_n(x, 32)
#define printbits_64(x) printbits_n(x, 64)

#define timeit(x)                                                           \
    {                                                                       \
        i64_t timer = clock();                                              \
        x;                                                                  \
        printf("%f\n", ((f64_t)(clock() - timer)) / CLOCKS_PER_SEC * 1000); \
    }

#define enum_key(x) (x->mmod == MMOD_INTERNAL ? strof_sym(as_list(x)[0]->i64) : as_string((obj_p)((str_p)x - PAGE_SIZE)))
#define enum_val(x) (x->mmod == MMOD_INTERNAL ? as_list(x)[1] : x)

#define anymap_key(x) (((obj_p)((str_p)x - PAGE_SIZE))->obj)
#define anymap_val(x) (x)

b8_t is_valid(obj_p obj);
u32_t next_power_of_two_u32(u32_t n);
u64_t next_power_of_two_u64(u64_t n);

#endif // UTIL_H
