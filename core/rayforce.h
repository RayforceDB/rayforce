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
#define ERR_HEAP 7
#define ERR_IO 8
#define ERR_NOT_FOUND 9
#define ERR_NOT_EXIST 10
#define ERR_NOT_IMPLEMENTED 11
#define ERR_STACK_OVERFLOW 12
#define ERR_THROW 13
#define ERR_UNKNOWN 127

#define NULL_vector_i64 ((i64_t)0x8000000000000000LL)
#define NULL_vector_f64 ((f64_t)(0 / 0.0))
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
typedef void nil_t;

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
typedef struct obj_t
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
} *obj_t;

// Constructors
extern obj_t null();                         // create null
extern obj_t atom(type_t type);              // create atom of type
extern obj_t list(i64_t len, ...);           // create list
extern obj_t vector(type_t type, i64_t len); // create vector of type
extern obj_t bool(bool_t val);               // bool scalar
extern obj_t i64(i64_t val);                 // i64 scalar
extern obj_t f64(f64_t val);                 // f64 scalar
extern obj_t symbol(str_t ptr);              // symbol
extern obj_t symboli64(i64_t id);            // symbol from i64
extern obj_t timestamp(i64_t val);           // timestamp
extern obj_t guid(u8_t data[]);              // GUID
extern obj_t schar(char_t c);                // char
extern obj_t string(i64_t len);              // string 

#define vector_bool(len)      (vector(TYPE_BOOL,      len)) // bool vector
#define vector_i64(len)       (vector(TYPE_I64,       len)) // i64 vector
#define vector_f64(len)       (vector(TYPE_F64,       len)) // f64 vector
#define vector_symbol(len)    (vector(TYPE_SYMBOL,    len)) // symbol vector
#define vector_timestamp(len) (vector(TYPE_TIMESTAMP, len)) // char vector
#define vector_guid(len)      (vector(TYPE_GUID,      len)) // GUID vector
         
extern obj_t table(obj_t keys, obj_t vals); // table
extern obj_t dict(obj_t keys,  obj_t vals); // dict
      
// Reference counting         
extern obj_t clone(obj_t obj); // clone
extern obj_t cow(obj_t   obj); // clone if refcount > 1
extern i64_t rc(obj_t    obj); // get refcount

// Error
extern obj_t error(i8_t code, str_t msg);

// Destructor
extern nil_t drop(obj_t obj);

// Accessors
#define as_string(obj)           ((obj)->ptr)
#define as_vector_bool(obj)      ((bool_t *)(as_string(obj)))
#define as_vector_i64(obj)       ((i64_t *)(as_string(obj)))
#define as_vector_f64(obj)       ((f64_t *)(as_string(obj)))
#define as_vector_symbol(obj)    ((i64_t *)(as_string(obj)))
#define as_vector_timestamp(obj) ((i64_t *)(as_string(obj)))
#define as_vector_guid(obj)      ((guid_t *)(as_string(obj)))
#define as_list(obj)             ((obj_t *)(as_string(obj)))

// Checkers
extern bool_t is_null(obj_t obj);
#define is_error(obj)  ((obj)->type == TYPE_ERROR)
#define is_scalar(obj) ((obj)->type < 0)
#define is_vector(obj) ((obj)->type > 0 && (obj)->type < TYPE_TABLE)

// Joins
extern obj_t join_raw(obj_t *obj, nil_t *val); // join raw value into a list
extern obj_t join_obj(obj_t *obj, obj_t  val); // join object to a list
extern obj_t join_sym(obj_t *obj, str_t  str); // join interned string to a symbol vector
extern obj_t join_lst(obj_t *obj, obj_t  val); // join two lists (must be of the same type)

#ifdef __cplusplus
}
#endif

#endif
