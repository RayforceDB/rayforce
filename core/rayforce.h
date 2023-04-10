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

#ifndef RAYFORCE_H
#define RAYFORCE_H

// #ifdef __cplusplus
// extern "C"
// {
// #endif

// A compile time assertion check
#define CASSERT(predicate, file) _IMPL_CASSERT_LINE(predicate, __LINE__, file)
#define _IMPL_PASTE(a, b) a##b
#define _IMPL_CASSERT_LINE(predicate, line, file) \
    typedef char _IMPL_PASTE(assertion_failed_##file##_, line)[2 * !!(predicate)-1];

#define UNUSED(x) (void)(x)

// Type constants
#define TYPE_ANY 0
#define TYPE_I8 1
#define TYPE_I64 2
#define TYPE_F64 3
#define TYPE_SYMBOL 4
#define TYPE_STRING 5
#define TYPE_LIST 97
#define TYPE_DICT 98
#define TYPE_TABLE 99
#define TYPE_ERROR 127

// Result constants
#define OK 0
#define ERR_INIT 1
#define ERR_PARSE 2
#define ERR_FORMAT 3
#define ERR_TYPE 4
#define ERR_LENGTH 5
#define ERR_INDEX 6
#define ERR_ALLOC 7
#define ERR_IO 8
#define ERR_NOT_FOUND 9
#define ERR_NOT_IMPLEMENTED 10
#define ERR_UNKNOWN 127

#define NULL_I64 ((i64_t)1 << 63)
#define NULL_F64 ((f64_t)(0 / 0.0))

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

/*
 * Vector header
 */
typedef struct header_t
{
    i64_t len;
    i64_t rc;
    i64_t attrs;
    union
    {
        i8_t code;
        i64_t pad;
    };
} header_t;

CASSERT(sizeof(struct header_t) == 32, vector_c)

// Generic type
typedef struct rf_object_t
{
    i8_t type, flags;
    u32_t id;

    union
    {
        i8_t i8;
        i64_t i64;
        f64_t f64;
        header_t *adt;
    };

} rf_object_t;

CASSERT(sizeof(struct rf_object_t) == 16, rf_h)

// Constructors
extern rf_object_t i64(i64_t rf_object);                           // i64 scalar
extern rf_object_t f64(f64_t rf_object);                           // f64 scalar
extern rf_object_t symbol(str_t ptr);                              // symbol
extern rf_object_t vector(i8_t type, i8_t size_of_val, i64_t len); // vector of type
extern rf_object_t string(i64_t len);                              // string (allocates len + 1 for \0 but sets len to a 'len')

#define vector_i64(len) (vector(TYPE_I64, sizeof(i64_t), len))       // i64 vector
#define vector_f64(len) (vector(TYPE_F64, sizeof(f64_t), len))       // f64 vector
#define vector_symbol(len) (vector(TYPE_SYMBOL, sizeof(i64_t), len)) // symbol vector
#define list(len) (vector(TYPE_LIST, sizeof(rf_object_t), len))      // list

extern rf_object_t null();                                    // null (as null list)
extern rf_object_t table(rf_object_t keys, rf_object_t vals); // table
extern rf_object_t dict(rf_object_t keys, rf_object_t vals);  // dict

// Reference counting
extern rf_object_t rf_object_clone(rf_object_t *rf_object); // clone
extern rf_object_t rf_object_cow(rf_object_t *rf_object);   // clone if refcount > 1

// Error
extern rf_object_t error(i8_t code, str_t message);

// Destructor
extern null_t rf_object_free(rf_object_t *rf_object);

// Accessors
#define as_string(rf_object) ((str_t)((rf_object)->adt + 1))
#define as_vector_i64(rf_object) ((i64_t *)(as_string(rf_object)))
#define as_vector_f64(rf_object) ((f64_t *)(as_string(rf_object)))
#define as_vector_symbol(rf_object) ((i64_t *)(as_string(rf_object)))
#define as_list(rf_object) ((rf_object_t *)(as_string(rf_object)))

// Checkers
#define is_null(rf_object) ((rf_object)->type == TYPE_LIST && (rf_object)->adt == NULL)
#define is_error(rf_object) ((rf_object)->type == TYPE_ERROR)
#define is_scalar(rf_object) ((rf_object)->type < 0)

// Mutators
extern i64_t vector_push(rf_object_t *vector, rf_object_t rf_object);
extern rf_object_t vector_pop(rf_object_t *vector);

// Compare
extern i8_t rf_object_eq(rf_object_t *a, rf_object_t *b);

// #ifdef __cplusplus
// }
// #endif

#endif
