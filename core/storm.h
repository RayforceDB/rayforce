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

#ifndef STORM_H
#define STORM_H

#ifdef __cplusplus
extern "C"
{
#endif

// A compile time assertion check
#define CASSERT(predicate, file) _impl_CASSERT_LINE(predicate, __LINE__, file)
#define _impl_PASTE(a, b) a##b
#define _impl_CASSERT_LINE(predicate, line, file) \
    typedef char _impl_PASTE(assertion_failed_##file##_, line)[2 * !!(predicate)-1];

#define UNUSED(x) (void)(x)

// Type constants
#define TYPE_LIST 0
#define TYPE_I8 1
#define TYPE_I64 2
#define TYPE_F64 3
#define TYPE_STRING 4
#define TYPE_SYMBOL 5
#define TYPE_ERROR 127

// Result constants
#define OK 0
#define ERR_INIT 1
#define ERR_PARSE 2
#define ERR_FORMAT 3
#define ERR_INVALID_TYPE 4

    typedef char i8_t;
    typedef unsigned char u8_t;
    typedef char *str_t;
    typedef short i16_t;
    typedef unsigned short u16_t;
    typedef int i32_t;
    typedef unsigned int u32_t;
    typedef long long i64_t;
    typedef unsigned long long u64_t;
    typedef double f64_t;
    typedef void null_t;

    typedef struct error_t
    {
        i8_t code;
        str_t message;
    } error_t;

    typedef struct vector_t
    {
        u64_t len;
        null_t *ptr;
    } vector_t;

    // Generic type
    typedef struct value_t
    {
        i8_t type;

        union
        {
            i8_t i8;
            i64_t i64;
            f64_t f64;
            vector_t list;
            error_t error;
        };
    } __attribute__((aligned(16))) value_t;

    CASSERT(sizeof(struct value_t) == 32, storm_h)

    // Constructors
    extern value_t i64(i64_t value);
    extern value_t f64(f64_t value);
    extern value_t xi64(i64_t *ptr, i64_t len);
    extern value_t xf64(f64_t *ptr, i64_t len);
    extern value_t string(str_t ptr, i64_t len);
    extern value_t symbol(str_t ptr, i64_t len);
    extern value_t xsymbol(i64_t *ptr, i64_t len);
    extern value_t list(value_t *ptr, i64_t len);
    extern value_t null();

    // Error
    extern value_t error(i8_t code, str_t message);

    // Destructor
    extern null_t value_free(value_t *value);

    // Accessors
    extern i8_t is_null(value_t *value);
    extern i8_t is_error(value_t *value);

#ifdef __cplusplus
}
#endif

#endif
