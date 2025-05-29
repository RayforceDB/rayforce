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
extern "C" {
#endif

//=============================================================================
// Type Definitions
//=============================================================================

// Basic Types
typedef char i8_t;
typedef char c8_t;
typedef char b8_t;
typedef char *str_p;
typedef const char *lit_p;
typedef unsigned char u8_t;
typedef short i16_t;
typedef unsigned short u16_t;
typedef int i32_t;
typedef unsigned int u32_t;
typedef long long i64_t;
typedef void *raw_p;
typedef unsigned long long u64_t;
typedef double f64_t;
typedef void nil_t;
typedef u8_t guid_t[16];

//=============================================================================
// Type Constants
//=============================================================================

// Simple Types
#define TYPE_LIST 0
#define TYPE_B8 1
#define TYPE_U8 2
#define TYPE_I16 3
#define TYPE_I32 4
#define TYPE_I64 5
#define TYPE_SYMBOL 6
#define TYPE_DATE 7
#define TYPE_TIME 8
#define TYPE_TIMESTAMP 9
#define TYPE_F64 10
#define TYPE_GUID 11
#define TYPE_C8 12
#define TYPE_ENUM 20
#define TYPE_MAPFILTER 71
#define TYPE_MAPGROUP 72
#define TYPE_MAPFD 73
#define TYPE_MAPCOMMON 74
#define TYPE_MAPLIST 75

// Parted Types (TYPE_PARTEDLIST 77 + type)
#define TYPE_PARTEDLIST 77
#define TYPE_PARTEDB8 (TYPE_PARTEDLIST + TYPE_B8)
#define TYPE_PARTEDU8 (TYPE_PARTEDLIST + TYPE_U8)
#define TYPE_PARTEDI16 (TYPE_PARTEDLIST + TYPE_I16)
#define TYPE_PARTEDI32 (TYPE_PARTEDLIST + TYPE_I32)
#define TYPE_PARTEDI64 (TYPE_PARTEDLIST + TYPE_I64)
#define TYPE_PARTEDDATE (TYPE_PARTEDLIST + TYPE_DATE)
#define TYPE_PARTEDTIME (TYPE_PARTEDLIST + TYPE_TIME)
#define TYPE_PARTEDTIMESTAMP (TYPE_PARTEDLIST + TYPE_TIMESTAMP)
#define TYPE_PARTEDF64 (TYPE_PARTEDLIST + TYPE_F64)
#define TYPE_PARTEDGUID (TYPE_PARTEDLIST + TYPE_GUID)
#define TYPE_PARTEDENUM (TYPE_PARTEDLIST + TYPE_ENUM)

// Other Types
#define TYPE_TABLE 98
#define TYPE_DICT 99
#define TYPE_LAMBDA 100
#define TYPE_UNARY 101
#define TYPE_BINARY 102
#define TYPE_VARY 103
#define TYPE_TOKEN 125

// Special Types
#define TYPE_NULL 126
#define TYPE_ERR 127

//=============================================================================
// Result Constants
//=============================================================================

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
#define ERR_OS 12
#define ERR_NOT_FOUND 13
#define ERR_NOT_EXIST 14
#define ERR_NOT_IMPLEMENTED 15
#define ERR_NOT_SUPPORTED 16
#define ERR_STACK_OVERFLOW 17
#define ERR_RAISE 18
#define ERR_UNKNOWN 127

//=============================================================================
// Special Values
//=============================================================================

#define NULL_I16 ((i16_t)0x8000)
#define NULL_I32 ((i32_t)0x80000000)
#define NULL_I64 ((i64_t)0x8000000000000000LL)
#define NULL_F64 ((f64_t)(0 / 0.0))
#define INF_I32 ((i32_t)0x7FFFFFFF)
#define INF_I64 ((i64_t)0x7FFFFFFFFFFFFFFFLL)
#define INF_U64 ((u64_t)0xFFFFFFFFFFFFFFFFULL)
#define B8_TRUE (char)1
#define B8_FALSE (char)0

static const guid_t NULL_GUID = {0};

//=============================================================================
// Core Data Structures
//=============================================================================

// Object (generic type)
typedef struct obj_t {
    u8_t mmod;   // memory model (internal, external, etc.) | block used flag (inside the heap)
    u8_t order;  // block order in the heap
    i8_t type;   // type
    u8_t attrs;  // attributes
    u32_t rc;    // reference count
    union {
        b8_t b8;
        u8_t u8;
        c8_t c8;
        i16_t i16;
        i32_t i32;
        i64_t i64;
        f64_t f64;
        struct obj_t *obj;
        struct {
            i64_t len;
            i8_t raw[];
        };
    };
} *obj_p;

typedef obj_p list_t;

//=============================================================================
// Function Declarations
//=============================================================================

// Version
extern u8_t version(nil_t);  // get version as u8_t (major - 5 bits, minor - 3 bits)

// Constructors
extern obj_p null(i8_t type);                  // create null atom of type
extern obj_p nullv(i8_t type, i64_t len);      // create null list of type and length
extern obj_p atom(i8_t type);                  // create atom of type
extern obj_p vn_list(i64_t len, ...);          // create list from values
extern obj_p vector(i8_t type, i64_t len);     // create vector of type
extern obj_p vn_symbol(i64_t len, ...);        // create vector symbols from strings
extern obj_p b8(b8_t val);                     // bool atom
extern obj_p u8(u8_t val);                     // byte atom
extern obj_p c8(c8_t c);                       // char
extern obj_p i16(i16_t val);                   // i16 atom
extern obj_p i32(i32_t val);                   // i32 atom
extern obj_p i64(i64_t val);                   // i64 atom
extern obj_p f64(f64_t val);                   // f64 atom
extern obj_p symbol(lit_p ptr, i64_t len);     // symbol
extern obj_p symboli64(i64_t id);              // symbol from i64
extern obj_p adate(i32_t val);                 // date
extern obj_p atime(i32_t val);                 // time
extern obj_p timestamp(i64_t val);             // timestamp
extern obj_p guid(const guid_t buf);           // GUID
extern obj_p vn_c8(lit_p fmt, ...);            // string from format
extern obj_p enumerate(obj_p sym, obj_p vec);  // enum
extern obj_p anymap(obj_p sym, obj_p vec);     // anymap

#define B8(len) (vector(TYPE_B8, len))                // bool vector
#define U8(len) (vector(TYPE_U8, len))                // byte vector
#define C8(len) (vector(TYPE_C8, len))                // string
#define I16(len) (vector(TYPE_I16, len))              // i16 vector
#define I32(len) (vector(TYPE_I32, len))              // i32 vector
#define I64(len) (vector(TYPE_I64, len))              // i64 vector
#define F64(len) (vector(TYPE_F64, len))              // f64 vector
#define SYMBOL(len) (vector(TYPE_SYMBOL, len))        // symbol vector
#define DATE(len) (vector(TYPE_DATE, len))            // date vector
#define TIME(len) (vector(TYPE_TIME, len))            // time vector
#define TIMESTAMP(len) (vector(TYPE_TIMESTAMP, len))  // char vector
#define GUID(len) (vector(TYPE_GUID, len))            // GUID vector
#define LIST(len) (vector(TYPE_LIST, len))            // list

extern obj_p table(obj_p keys, obj_p vals);  // table
extern obj_p dict(obj_p keys, obj_p vals);   // dict

// Reference counting
extern obj_p clone_obj(obj_p obj);  // clone_obj
extern obj_p copy_obj(obj_p obj);   // copy
extern obj_p cow_obj(obj_p obj);    // copy-on-write
extern u32_t rc_obj(obj_p obj);     // get refcount

// Errors
extern obj_p error(i8_t code, lit_p fmt, ...);  // Creates an error object

// Destructors
extern nil_t drop_obj(obj_p obj);  // Free an object
extern nil_t drop_raw(raw_p ptr);  // Free a raw pointer

// Accessors
#define AS_C8(obj) ((str_p)(obj + 1))
#define AS_B8(obj) ((b8_t *)(AS_C8(obj)))
#define AS_I8(obj) ((i8_t *)(AS_C8(obj)))
#define AS_U8(obj) ((u8_t *)(AS_C8(obj)))
#define AS_I16(obj) ((i16_t *)(AS_C8(obj)))
#define AS_I32(obj) ((i32_t *)(AS_C8(obj)))
#define AS_I64(obj) ((i64_t *)(AS_C8(obj)))
#define AS_F64(obj) ((f64_t *)(AS_C8(obj)))
#define AS_SYMBOL(obj) ((i64_t *)(AS_C8(obj)))
#define AS_DATE(obj) ((i32_t *)(AS_C8(obj)))
#define AS_TIME(obj) ((i32_t *)(AS_C8(obj)))
#define AS_TIMESTAMP(obj) ((i64_t *)(AS_C8(obj)))
#define AS_GUID(obj) ((guid_t *)(AS_C8(obj)))
#define AS_LIST(obj) ((obj_p *)(AS_C8(obj)))

// Checkers
extern b8_t is_null(obj_p obj);
extern str_p type_name(i8_t tp);
#define IS_ERR(obj) ((obj)->type == TYPE_ERR)
#define IS_ATOM(obj) ((obj)->type < 0)
#define IS_VECTOR(obj) ((obj)->type >= 0 && (obj)->type <= TYPE_ENUM)

// Push a value to the end of a list
extern obj_p push_raw(obj_p *obj, raw_p val);  // push raw value into a list
extern obj_p push_obj(obj_p *obj, obj_p val);  // push object to a list
extern obj_p push_sym(obj_p *obj, lit_p str);  // push interned string to a symbol vector

// Append list to a list
extern obj_p append_list(obj_p *obj, obj_p vals);

extern obj_p unify_list(obj_p *obj);   // Unify list to a vector (if possible)
extern obj_p diverse_obj(obj_p *obj);  // Diverse object to a list

// Pop a value from the end of a list
extern obj_p pop_obj(obj_p *obj);                             // pop object from a list
extern obj_p remove_idx(obj_p *obj, i64_t idx);               // remove value from a obj by index
extern obj_p remove_ids(obj_p *obj, i64_t ids[], i64_t len);  // remove value from a obj by indices
extern obj_p remove_obj(obj_p *obj, obj_p idx);               // remove value from a obj by obj

// Insert a value into a list by an index (doesn't call a drop)
extern obj_p ins_raw(obj_p *obj, i64_t idx, raw_p val);  // write raw value into a list
extern obj_p ins_obj(obj_p *obj, i64_t idx, obj_p val);  // write object to a list
extern obj_p ins_sym(obj_p *obj, i64_t idx, lit_p str);  // write interned string to a symbol vector

// Read
extern obj_p at_idx(obj_p obj, i64_t idx);               // read value from an obj at index
extern obj_p at_ids(obj_p obj, i64_t ids[], i64_t len);  // read values from an obj at indexes
extern obj_p at_obj(obj_p obj, obj_p idx);               // read from obj indexed by obj
extern obj_p at_sym(obj_p obj, lit_p str, i64_t len);    // read value indexed by symbol created from str

// Format
extern str_p str_from_symbol(i64_t id);  // return interned string by interned id

// Initialize obj with zeroed memory
extern nil_t zero_obj(obj_p obj);

// Set
extern obj_p set_idx(obj_p *obj, i64_t idx, obj_p val);                // set obj at index
extern obj_p set_ids(obj_p *obj, i64_t ids[], i64_t len, obj_p vals);  // set obj at indexes
extern obj_p set_obj(obj_p *obj, obj_p idx, obj_p val);                // set obj indexed by obj

// Sync
extern b8_t rc_sync_get();          // get reference counting synchronization state
extern nil_t rc_sync_set(b8_t on);  // turn on/off reference counting synchronization

// Resize
extern obj_p resize_obj(obj_p *obj, i64_t len);

// Search
extern i64_t find_raw(obj_p obj, raw_p val);      // find raw value in a list, return index (obj->len if not found)
extern i64_t find_obj_idx(obj_p obj, obj_p val);  // find object in a list, return index (obj->len if not found)
extern obj_p find_obj_ids(obj_p obj, obj_p val);  // find object in a list, return indexes (NULL_OBJ if not found)
extern i64_t find_sym(obj_p obj, lit_p str);      // find interned string in a symbol vector

// Cast
extern obj_p cast_obj(i8_t type, obj_p obj);

// Comparison
extern i64_t cmp_obj(obj_p a, obj_p b);

// Serialization
extern obj_p ser_obj(obj_p obj);
extern obj_p de_obj(obj_p buf);

// Parse
extern obj_p parse_str(lit_p str);

// Evaluate
extern obj_p eval_str(lit_p str);
extern obj_p eval_obj(obj_p obj);  // Moves ownership of obj to the eval function
extern obj_p try_obj(obj_p obj, obj_p ctch);

// Initialize runtime
extern i32_t ray_init(nil_t);

// Cleanup runtime
extern nil_t ray_clean(nil_t);

#ifdef __cplusplus
}
#endif

#endif  // RAYFORCE_H
