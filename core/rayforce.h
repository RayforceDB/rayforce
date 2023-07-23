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

// Type constants
#define TYPE_LIST 0
#define TYPE_BOOL 1
#define TYPE_I64 2
#define TYPE_F64 3
#define TYPE_SYMBOL 4
#define TYPE_TIMESTAMP 5
#define TYPE_GUID 6
#define TYPE_CHAR 7
#define TYPE_TABLE 98
#define TYPE_DICT 99
#define TYPE_LAMBDA 100
#define TYPE_UNARY 101
#define TYPE_BINARY 102
#define TYPE_VARY 103
#define TYPE_INSTRUCTION 111
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
#define ERR_NOT_EXIST 10
#define ERR_NOT_IMPLEMENTED 11
#define ERR_STACK_OVERFLOW 12
#define ERR_THROW 13
#define ERR_UNKNOWN 127

#define NULL_I64 ((i64_t)0x8000000000000000LL)
#define NULL_F64 ((f64_t)(0 / 0.0))
#define true (char)1
#define false (char)0

typedef char type_t;
typedef char i8_t;
typedef char char_t;
typedef char bool_t;
typedef char *str_t;
typedef unsigned char u8_t;
typedef short i16_t;
typedef unsigned short u16_t;
typedef int i32_t;
typedef unsigned int u32_t;
typedef long long i64_t;
typedef unsigned long long u64_t;
typedef double f64_t;
typedef void null_t;

/*
 * GUID (Globally Unique Identifier)
 */
typedef struct guid_t
{
    u8_t data[16];
} guid_t;

/*
* Generic type
*/ 
typedef struct rf_object_t
{
    type_t type;
    u8_t flags;
    u8_t code;
    u32_t id;
    u32_t rc;
    union
    {
        bool_t bool;
        char_t schar;
        i64_t i64;
        f64_t f64;
        struct {
            u64_t len;
            str_t ptr;
        };
    };
} *rf_object;

// Constructors
extern rf_object null();                                                     // create null
extern rf_object atom(type_t type);                                          // create atom of type
extern rf_object list(i64_t len, ...);                                       // create list
extern rf_object vector(type_t type, i64_t len);                             // create vector of type
extern rf_object bool(bool_t val);                                           // bool scalar
extern rf_object i64(i64_t val);                                             // i64 scalar
extern rf_object f64(f64_t val);                                             // f64 scalar
extern rf_object symbol(str_t ptr);                                          // symbol
extern rf_object symboli64(i64_t id);                                        // symbol from i64
extern rf_object timestamp(i64_t val);                                       // timestamp
extern rf_object guid(u8_t data[]);                                          // GUID
extern rf_object schar(char_t c);                                            // char
extern rf_object string(i64_t len);                                          // string 

#define Bool(len)      (vector(TYPE_BOOL,       len ))  // bool vector
#define I64(len)       (vector(TYPE_I64,        len ))  // i64 vector
#define F64(len)       (vector(TYPE_F64,        len ))  // f64 vector
#define Symbol(len)    (vector(TYPE_SYMBOL,     len ))  // symbol vector
#define Timestamp(len) (vector(TYPE_TIMESTAMP,  len ))  // char vector
#define Guid(len)      (vector(TYPE_GUID,       len ))  // GUID vector

extern rf_object table(rf_object keys, rf_object vals);                  // table
extern rf_object dict(rf_object keys,  rf_object vals);                  // dict

// Reference counting   
extern rf_object clone(rf_object object);                    // clone
extern rf_object cow(rf_object   object);                    // clone if refcount > 1
extern i64_t     rc(rf_object    object);                    // get refcount

// Error
extern rf_object error(i8_t code, str_t message);

// Destructor
extern null_t drop(rf_object   object);

// Accessors
#define as_string(object)    ((object)->ptr)
#define as_Bool(object)      ((bool_t *)(as_string(object)))
#define as_I64(object)       ((i64_t *)(as_string(object)))
#define as_F64(object)       ((f64_t *)(as_string(object)))
#define as_Symbol(object)    ((i64_t *)(as_string(object)))
#define as_Timestamp(object) ((i64_t *)(as_string(object)))
#define as_Guid(object)      ((guid_t *)(as_string(object)))
#define as_list(object)      ((rf_object *)(as_string(object)))

// Checkers
extern bool_t is_null(rf_object object);
#define is_error(object)  ((object)->type == TYPE_ERROR)
#define is_scalar(object) ((object)->type < 0)
#define is_vector(object) ((object)->type > 0 && (object)->type < TYPE_TABLE)

// Mutators
extern rf_object vector_push(rf_object vector, rf_object object);
extern rf_object vector_pop(rf_object  vector);

// Compare
extern bool_t rf_object_eq(rf_object a, rf_object b);

#ifdef __cplusplus
}
#endif

#endif
