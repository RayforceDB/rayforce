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

// clang-format off
#ifndef RAYFORCE_H
#define RAYFORCE_H

#ifdef __cplusplus
extern "C"
{
#endif

// Type constants
#define TYPE_LIST 0
#define TYPE_B8 1
#define TYPE_U8 2
#define TYPE_I64 3
#define TYPE_SYMBOL 4
#define TYPE_TIMESTAMP 5
#define TYPE_F64 6
#define TYPE_GUID 7
#define TYPE_C8 8
#define TYPE_ENUM 20
#define TYPE_ANYMAP 77
#define TYPE_FILTERMAP 78
#define TYPE_GROUPMAP 79
#define TYPE_TABLE 98
#define TYPE_DICT 99
#define TYPE_LAMBDA 100
#define TYPE_UNARY 101
#define TYPE_BINARY 102
#define TYPE_VARY 103
#define TYPE_NULL 126
#define TYPE_ERROR 127

// Result constants
#define OK 0
#define ERR_INIT 1
#define ERR_PARSE 2
#define ERR_EVAL 3
#define ERR_FORMAT 4
#define ERR_TYPE 5
#define ERR_LENGTH 6
#define ERR_ARITY 7
#define ERR_INDEX 8
#define ERR_HEAP 9
#define ERR_IO 10
#define ERR_SYS 11
#define ERR_NOT_FOUND 12
#define ERR_NOT_EXIST 13
#define ERR_NOT_IMPLEMENTED 14
#define ERR_NOT_SUPPORTED 15
#define ERR_STACK_OVERFLOW 16
#define ERR_RAISE 17
#define ERR_UNKNOWN 127

typedef char i8_t;
typedef char c8_t;
typedef char b8_t;
typedef char *str_p;
typedef unsigned char u8_t;
typedef short i16_t;
typedef unsigned short u16_t;
typedef int i32_t;
typedef unsigned int u32_t;
typedef long long i64_t;
typedef void* raw_p;
typedef unsigned long long u64_t;
typedef double f64_t;
typedef void nil_t;

#define NULL_I64  ((i64_t)0x8000000000000000LL)
#define NULL_F64  ((f64_t)(0 / 0.0))
#define MAX_I64   ((i64_t)0x7FFFFFFFFFFFFFFFLL)
#define B8_TRUE   (char)1
#define B8_FALSE  (char)0


// GUID (Globally Unique Identifier)
typedef struct guid_t {
    u8_t buf[16];
} guid_t;

// Object (generic type)
typedef struct obj_t
{
    u8_t mmod;  // memory model (internal, external, etc.)
    u8_t resv;  // reserved
    i8_t type;  // type
    u8_t attrs; // attributes
    u32_t rc;   // reference count
    union
    {
        b8_t b8;
        u8_t u8;
        c8_t c8;
        i64_t i64;
        f64_t f64;
        struct obj_t *obj;
        struct {
            u64_t len;
            i8_t arr[];
        };
    };
} *obj_p;

// Version
extern u8_t version(nil_t); // get version as u8_t (major - 5 bits, minor - 3 bits)

// Constructors
extern obj_p null(i8_t type);                 // create null atom of type
extern obj_p nullv(i8_t type, u64_t len);     // create null list of type and length
extern obj_p atom(i8_t type);                 // create atom of type
extern obj_p list(u64_t len);                 // create list
extern obj_p vn_list(u64_t len, ...);         // create list from values
extern obj_p vector(i8_t type, u64_t len);    // create vector of type
extern obj_p vn_symbol(u64_t len, ...);       // create vector symbols from strings
extern obj_p b8(b8_t val);                    // bool atom
extern obj_p u8(u8_t val);                    // byte atom
extern obj_p c8(c8_t c);                      // char
extern obj_p i64(i64_t val);                  // i64 atom
extern obj_p f64(f64_t val);                  // f64 atom
extern obj_p symbol(str_p ptr);               // symbol
extern obj_p symboli64(i64_t id);             // symbol from i64
extern obj_p timestamp(i64_t val);            // timestamp
extern obj_p guid(u8_t buf[16]);              // GUID
extern obj_p string(u64_t len);               // string 
extern obj_p vn_string(str_p fmt, ...);       // string from format
extern obj_p enumerate(obj_p sym, obj_p vec); // enum
extern obj_p anymap(obj_p sym, obj_p vec);    // anymap

#define vector_b8(len)        (vector(TYPE_B8,        len)) // bool vector
#define vector_u8(len)        (vector(TYPE_U8,        len)) // byte vector
#define vector_i64(len)       (vector(TYPE_I64,       len)) // i64 vector
#define vector_f64(len)       (vector(TYPE_F64,       len)) // f64 vector
#define vector_symbol(len)    (vector(TYPE_SYMBOL,    len)) // symbol vector
#define vector_timestamp(len) (vector(TYPE_TIMESTAMP, len)) // char vector
#define vector_guid(len)      (vector(TYPE_GUID,      len)) // GUID vector
         
extern obj_p table(obj_p keys, obj_p vals); // table
extern obj_p dict(obj_p keys,  obj_p vals); // dict
      
// Reference counting         
extern obj_p clone_obj(obj_p obj); // clone_obj
extern obj_p copy_obj(obj_p obj);  // copy
extern obj_p cow_obj(obj_p obj);   // clone_obj if refcount > 1
extern u32_t rc_obj(obj_p obj);    // get refcount

// Errors
extern obj_p error(i8_t code, str_p fmt, ...); // Creates an error object

// Destructors
extern nil_t drop_obj(obj_p obj); // Free an object
extern nil_t drop_raw(raw_p ptr); // Free a raw pointer

// Accessors
#define as_string(obj)    ((str_p)__builtin_assume_aligned((obj + 1), sizeof(struct obj_t)))
#define as_b8(obj)        ((b8_t *)(as_string(obj)))
#define as_u8(obj)        ((u8_t *)(as_string(obj)))
#define as_i64(obj)       ((i64_t *)(as_string(obj)))
#define as_f64(obj)       ((f64_t *)(as_string(obj)))
#define as_symbol(obj)    ((i64_t *)(as_string(obj)))
#define as_timestamp(obj) ((i64_t *)(as_string(obj)))
#define as_guid(obj)      ((guid_t *)(as_string(obj)))
#define as_list(obj)      ((obj_p *)(as_string(obj)))

// Checkers
extern b8_t is_null(obj_p obj);
extern str_p type_name(i8_t tp);
#define is_error(obj)  ((obj)->type == TYPE_ERROR)
#define is_atom(obj)   ((obj)->type < 0)
#define is_vector(obj) ((obj)->type >= 0 && (obj)->type <= TYPE_C8)

// Push a value to the end of a list
extern obj_p push_raw(obj_p *obj, raw_p val); // push raw value into a list
extern obj_p push_obj(obj_p *obj, obj_p val); // push object to a list
extern obj_p push_sym(obj_p *obj, str_p str); // push interned string to a symbol vector

// Append list to a list
extern obj_p append_list(obj_p *obj, obj_p vals);  

// Pop a value from the end of a list
extern obj_p pop_obj(obj_p *obj); // pop object from a list

// Insert a value into a list by an index (doesn't call a drop)
extern obj_p ins_raw(obj_p *obj, i64_t idx, raw_p val); // write raw value into a list
extern obj_p ins_obj(obj_p *obj, i64_t idx, obj_p val); // write object to a list
extern obj_p ins_sym(obj_p *obj, i64_t idx, str_p str); // write interned string to a symbol vector

// Read
extern obj_p at_idx(obj_p obj, i64_t idx);              // read value from an obj at index
extern obj_p at_ids(obj_p obj, i64_t ids[], u64_t len); // read values from an obj at indexes
extern obj_p at_obj(obj_p obj, obj_p idx);              // read from obj indexed by obj
extern obj_p at_sym(obj_p obj, str_p str);              // read value indexed by symbol created from str

// Format
extern str_p strof_sym(i64_t id);                       // return interned string by interned id
extern str_p strof_obj(obj_p obj);                      // return string representation of an object

// Initialize obj with zeroed memory
extern nil_t zero_obj(obj_p obj);

// Set
extern obj_p set_idx(obj_p *obj, i64_t idx, obj_p val);               // set obj at index
extern obj_p set_ids(obj_p *obj, i64_t ids[], u64_t len, obj_p vals); // set obj at indexes
extern obj_p set_obj(obj_p *obj, obj_p idx, obj_p val);               // set obj indexed by obj

// Resize
extern obj_p resize_obj(obj_p *obj, u64_t len);

// Search
extern i64_t find_raw(obj_p obj, raw_p val); // find raw value in a list, return index (obj->len if not found)
extern i64_t find_obj(obj_p obj, obj_p val); // find object in a list, return index (obj->len if not found)
extern i64_t find_sym(obj_p obj, str_p str); // find interned string in a symbol vector, return index (obj->len if not found)

// Cast
extern obj_p cast_obj(i8_t type, obj_p obj);

// Comparison
extern i32_t cmp_obj(obj_p a, obj_p b);

// Serialization
extern obj_p ser_obj(obj_p obj);
extern obj_p de_obj(obj_p buf);

// Parse
extern obj_p parse_str(str_p str);

// Evaluate
extern obj_p eval_str(str_p str);
extern obj_p eval_obj(obj_p obj); // Moves ownership of obj to the eval function
extern obj_p try_obj(obj_p obj, obj_p ctch);

// Initialize runtime
extern i32_t ray_init();

// Cleanup runtime
extern nil_t ray_clean();

#ifdef __cplusplus
}
#endif

#endif // RAYFORCE_H
