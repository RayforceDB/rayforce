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

#ifdef __cplusplus
extern "C"
{
#endif

// Type constants
#define TYPE_LIST 0
#define TYPE_BOOL 1
#define TYPE_BYTE 2
#define TYPE_I64 3
#define TYPE_F64 4
#define TYPE_SYMBOL 5
#define TYPE_TIMESTAMP 6
#define TYPE_GUID 7
#define TYPE_CHAR 8
#define TYPE_ENUM 20
#define TYPE_ANYMAP 77
#define TYPE_VECMAP 78
#define TYPE_LISTMAP 79
#define TYPE_TABLE 98
#define TYPE_DICT 99
#define TYPE_LAMBDA 100
#define TYPE_UNARY 101
#define TYPE_BINARY 102
#define TYPE_VARY 103
#define TYPE_ERROR 127

// Result constants
#define OK 0
#define ERR_INIT 1
#define ERR_PARSE 2
#define ERR_FORMAT 3
#define ERR_TYPE 4
#define ERR_LENGTH 5
#define ERR_INDEX 6
#define ERR__HEAP 7
#define ERR_IO 8
#define ERR_NOT_FOUND 9
#define ERR_NOT_EXIST 10
#define ERR_NOT_IMPLEMENTED 11
#define ERR_NOT_SUPPORTED 12
#define ERR_STACK_OVERFLOW 13
#define ERR_THROW 14
#define ERR_UNKNOWN 127

#define NULL_I64  ((i64_t)0x8000000000000000LL)
#define NULL_F64  ((f64_t)(0 / 0.0))
#define NULL_GUID ((u8_t[16]){0, 0, 0, 0})
#define MAX_I64   ((i64_t)0x7FFFFFFFFFFFFFFFLL)
#define true      (char)1
#define false     (char)0

typedef signed char type_t;
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
typedef void* raw_t;
typedef unsigned long long u64_t;
typedef double f64_t;
typedef void nil_t;

/*
 * GUID (Globally Unique Identifier)
 */
typedef struct guid_t {
    u8_t buf[16];
} guid_t;

/*
* Generic type
*/ 
typedef struct obj_t
{
    u8_t mmod;       // memory model (0 - internal, 1 - memmapped)
    u8_t refc;       // is reference counted
    type_t type;     // type
    u8_t attrs;      // attributes
    u32_t rc;        // reference count
    union
    {
        bool_t bool;
        u8_t u8;
        char_t vchar;
        i64_t i64;
        f64_t f64;
        struct obj_t *obj;
        struct {
            u64_t len;
            i8_t arr[];
        };
    };
} *obj_t;

// Version
extern u8_t version(); // get version as u8_t (major - 5 bits, minor - 3 bits)

// Constructors
extern obj_t null(type_t type);                             // create null of type
extern obj_t atom(type_t type);                             // create atom of type
extern obj_t list(u64_t len, ...);                          // create list
extern obj_t vector(type_t type, u64_t len);                // create vector of type
extern obj_t bool(bool_t val);                              // bool atom
extern obj_t u8(u8_t val);                                  // byte atom
extern obj_t i64(i64_t val);                                // i64 atom
extern obj_t f64(f64_t val);                                // f64 atom
extern obj_t symbol(str_t ptr);                             // symbol
extern obj_t symboli64(i64_t id);                           // symbol from i64
extern obj_t timestamp(i64_t val);                          // timestamp
extern obj_t guid(u8_t buf[16]);                            // GUID
extern obj_t vchar(char_t c);                               // char
extern obj_t string(u64_t len);                             // string 
extern obj_t venum(obj_t sym, obj_t vec);                   // enum
extern obj_t anymap(obj_t sym, obj_t vec);                  // anymap

#define vector_bool(len)      (vector(TYPE_BOOL,      len)) // bool vector
#define vector_u8(len)        (vector(TYPE_BYTE,      len)) // byte vector
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
extern u32_t rc(obj_t    obj); // get refcount

// Error
extern obj_t error(i8_t code, str_t msg);

// Destructor
extern nil_t drop(obj_t obj);
extern nil_t dropn(u64_t n, ...);

// Accessors
#define as_string(obj)    ((str_t)__builtin_assume_aligned((obj + 1), sizeof(struct obj_t)))
#define as_bool(obj)      ((bool_t *)(as_string(obj)))
#define as_u8(obj)        ((u8_t *)(as_string(obj)))
#define as_i64(obj)       ((i64_t *)(as_string(obj)))
#define as_f64(obj)       ((f64_t *)(as_string(obj)))
#define as_symbol(obj)    ((i64_t *)(as_string(obj)))
#define as_timestamp(obj) ((i64_t *)(as_string(obj)))
#define as_guid(obj)      ((guid_t *)(as_string(obj)))
#define as_list(obj)      ((obj_t *)(as_string(obj)))

// Checkers
extern bool_t is_null(obj_t obj);
#define is_error(obj)  (obj && (obj)->type == TYPE_ERROR)
#define is_atom(obj)   (obj && (obj)->type < 0)
#define is_vector(obj) (obj && (obj)->type >= 0 && (obj)->type <= TYPE_CHAR)

// Push a value to the end of a list
extern obj_t push_raw(obj_t *obj, raw_t val); // push raw value into a list
extern obj_t push_obj(obj_t *obj, obj_t val); // push object to a list
extern obj_t push_sym(obj_t *obj, str_t str); // push interned string to a symbol vector

// Pop a value from the end of a list
extern obj_t pop_obj(obj_t *obj); // pop object from a list

// Insert a value into a list by an index (doesn't call a drop)
extern obj_t ins_raw(obj_t *obj, i64_t idx, raw_t val); // write raw value into a list
extern obj_t ins_obj(obj_t *obj, i64_t idx, obj_t val); // write object to a list
extern obj_t ins_sym(obj_t *obj, i64_t idx, str_t str); // write interned string to a symbol vector

// Read
extern obj_t at_idx(obj_t obj, i64_t idx); // read raw value from an obj at index
extern obj_t at_obj(obj_t obj, obj_t idx); // read from obj indexed by obj
extern str_t symtostr(i64_t id);           // return interned string by interned id

// Set
extern obj_t set_idx(obj_t *obj, i64_t idx, obj_t val); // set obj at index
extern obj_t set_obj(obj_t *obj, obj_t idx, obj_t val); // set obj indexed by obj

// Resize
extern obj_t resize(obj_t *obj, u64_t len);

// Search
extern i64_t find_raw(obj_t obj, raw_t val); // find raw value in a list, return index (obj->len if not found)
extern i64_t find_obj(obj_t obj, obj_t val); // find object in a list, return index (obj->len if not found)
extern i64_t find_sym(obj_t obj, str_t str); // find interned string in a symbol vector, return index (obj->len if not found)

// Cast
extern obj_t cast(type_t type, obj_t obj);   // cast object x o a type

// Comparison
extern bool_t equal(obj_t a, obj_t b);

// Serialization
extern obj_t ser(obj_t obj);
extern obj_t de(obj_t buf);

/* 
* Evaluate into an instance, drived by handle fd:
* = 0: self process
* > 0: sync evaluate string str into an instance, drived by handle fd
* < 0: async evaluate string str into an instance, drived by handle -fd
* Moves ownership of an obj, so don't call drop() on it after
*/
extern obj_t eval_str(i64_t fd, str_t name, str_t str);
extern obj_t eval_obj(i64_t fd, str_t name, obj_t obj);


#ifdef __cplusplus
}
#endif

#endif
