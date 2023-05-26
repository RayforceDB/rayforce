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

// clang-format off
#ifdef __cplusplus
extern "C"
{
#endif

// A compile time assertion check
#define CASSERT(predicate, file) _IMPL_CASSERT_LINE(predicate, __LINE__, file)
#define _IMPL_PASTE(a, b) a##b
#define _IMPL_CASSERT_LINE(predicate, line, file) \
    typedef char _IMPL_PASTE(assertion_failed_##file##_, line)[2 * !!(predicate)-1];

#define UNUSED(x) (void)(x)

// Type constants
#define TYPE_NULL 0
#define TYPE_BOOL 1
#define TYPE_I64 2
#define TYPE_F64 3
#define TYPE_SYMBOL 4
#define TYPE_TIMESTAMP 5
#define TYPE_GUID 6
#define TYPE_CHAR 7
#define TYPE_LIST 8
#define TYPE_DICT 9
#define TYPE_TABLE 10
#define TYPE_FUNCTION 11
#define TYPE_ERROR 12
#define TYPE_ANY 127

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
#define ERR_STACK_OVERFLOW 11
#define ERR_THROW 12
#define ERR_UNKNOWN 127

#define NULL_I64 ((i64_t)1 << 63)
#define NULL_F64 ((f64_t)(0 / 0.0))
#define true (char)1
#define false (char)0

typedef char i8_t;
typedef unsigned char u8_t;
typedef char char_t;
typedef char bool_t;
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
 * Points to a actual error position in a source code
 */
typedef struct span_t
{
    u16_t start_line;
    u16_t end_line;
    u16_t start_column;
    u16_t end_column;
} span_t;

CASSERT(sizeof(struct span_t) == 8, debuginfo_h)

/*
 * ADT header
 */
typedef struct header_t
{
    i64_t len;
    i64_t rc;
    span_t span;
    union
    {
        i8_t code;
        i64_t attrs;
    };
} header_t;

CASSERT(sizeof(struct header_t) == 32, rayforce_h)

/*
 * GUID (Globally Unique Identifier)
 */
typedef struct guid_t
{
    u8_t data[16];
} guid_t;

CASSERT(sizeof(struct guid_t) == 16, rayforce_h)

// Generic type
typedef struct rf_object_t
{
    union
    {
        bool_t bool;
        char_t schar;
        i64_t i64;
        f64_t f64;
        guid_t *guid;
        header_t *adt;
    };

    u32_t id;
    u16_t tt;
    i8_t type;
    i8_t flags;

} rf_object_t;

CASSERT(sizeof(struct rf_object_t) == 16, rayforce_h)

// Constructors
extern rf_object_t bool(bool_t val);                                        // bool scalar
extern rf_object_t i64(i64_t val);                                          // i64 scalar
extern rf_object_t f64(f64_t val);                                          // f64 scalar
extern rf_object_t symbol(str_t ptr);                                       // symbol
extern rf_object_t symboli64(i64_t id);                                     // symbol from i64
extern rf_object_t timestamp(i64_t val);                                    // timestamp
extern rf_object_t guid(u8_t data[]);                                       // GUID
extern rf_object_t schar(char_t c);                                         // char
extern rf_object_t vector(i8_t type, i8_t size_of_val, i64_t len);          // vector of type
extern rf_object_t string(i64_t len);                                       // string 

#define null()                ((rf_object_t){.type = TYPE_NULL})            // null
#define vector_bool(len)      (vector(TYPE_BOOL, sizeof(bool_t), len))      // bool vector
#define vector_i64(len)       (vector(TYPE_I64, sizeof(i64_t), len))        // i64 vector
#define vector_f64(len)       (vector(TYPE_F64, sizeof(f64_t), len))        // f64 vector
#define vector_symbol(len)    (vector(TYPE_SYMBOL, sizeof(i64_t), len))     // symbol vector
#define vector_timestamp(len) (vector(TYPE_TIMESTAMP, sizeof(i64_t), len))  // char vector
#define vector_guid(len)      (vector(TYPE_GUID, sizeof(guid_t), len))      // GUID vector
#define list(len)             (vector(TYPE_LIST, sizeof(rf_object_t), len)) // list

extern rf_object_t table(rf_object_t keys, rf_object_t vals);               // table
extern rf_object_t dict(rf_object_t keys, rf_object_t vals);                // dict

// Reference counting   
extern rf_object_t rf_object_clone(rf_object_t *rf_object);                 // clone
extern rf_object_t rf_object_cow(rf_object_t *rf_object);                   // clone if refcount > 1
extern i64_t       rf_object_rc(rf_object_t *rf_object);                    // get refcount

// Error
extern rf_object_t error(i8_t code, str_t message);

// Destructor
extern null_t rf_object_free(rf_object_t *rf_object);

// Accessors
#define as_string(object)           ((str_t)((object)->adt + 1))
#define as_vector_bool(object)      ((bool_t *)(as_string(object)))
#define as_vector_i64(object)       ((i64_t *)(as_string(object)))
#define as_vector_f64(object)       ((f64_t *)(as_string(object)))
#define as_vector_symbol(object)    ((i64_t *)(as_string(object)))
#define as_vector_timestamp(object) ((i64_t *)(as_string(object)))
#define as_vector_guid(object)      ((guid_t *)(as_string(object)))
#define as_list(object)             ((rf_object_t *)(as_string(object)))

// Checkers
#define is_null(object)   ((object)->type == TYPE_NULL)
#define is_error(object)  ((object)->type == TYPE_ERROR)
#define is_scalar(object) ((object)->type < 0)

// Mutators
extern i64_t       vector_push(rf_object_t *vector, rf_object_t object);
extern rf_object_t vector_pop(rf_object_t *vector);

// Compare
extern i8_t rf_object_eq(rf_object_t *a, rf_object_t *b);

#ifdef __cplusplus
}
#endif

#endif
