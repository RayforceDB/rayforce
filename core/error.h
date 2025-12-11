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

#ifndef ERROR_H
#define ERROR_H

#include "rayforce.h"
#include "nfo.h"
#include "heap.h"
#include "format.h"

#define AS_ERROR(obj) ((ray_error_p)(AS_C8(obj)))

/*
 * Create a new error object and return it
 */
#define THROW(t, ...)                        \
    {                                        \
        obj_p _m = str_fmt(-1, __VA_ARGS__); \
        obj_p _e = error_obj(t, _m);         \
        return _e;                           \
    }

// Static error - no formatting, uses string literal directly
#define THROW_S(t, msg) return error_str(t, msg)

// Common error messages (to avoid string duplication)
#define ERR_MSG_BUFFER_UNDERFLOW "de_raw: buffer underflow"
#define ERR_MSG_INVALID_TOKEN "Invalid token in vector"
#define ERR_MSG_VEC_SAME_LEN "vectors must have the same length"
#define ERR_MSG_ENUM_OUT_OF_RANGE "at: enum can not be resolved: index out of range"
#define ERR_MSG_TABLE_VALUES_LEN "table: values must be of the same length"
#define ERR_MSG_TABLE_KV_LEN "table: keys and values must have the same length"
#define ERR_MSG_SCAN_WRONG_ARGS "'scan': binary call with wrong arguments count"
#define ERR_MSG_SCAN_DIFF_LEN "'scan': arguments have different lengths"
#define ERR_MSG_SCAN_VEC_SAME_LEN "'scan': vectors must be of the same length"
#define ERR_MSG_FOLD_WRONG_ARGS "'fold': binary call with wrong arguments count"
#define ERR_MSG_FOLD_DIFF_LEN "'fold': arguments have different lengths"
#define ERR_MSG_FOLD_VEC_SAME_LEN "'fold': vectors must be of the same length"
#define ERR_MSG_BINARY_VEC_SAME_LEN "binary: vectors must be of the same length"
#define ERR_MSG_STACK_OVERFLOW "stack overflow"
#define ERR_MSG_MALFORMED_FMT "malformed format string"
#define ERR_MSG_INVALID_SYM_LEN "de_raw: invalid symbol length"
#define ERR_MSG_FIRST_ENUM "first: can not resolve an enum"

// Compact arity error messages (saves ~20 bytes each)
#define ERR_MSG_MAP_ARITY "map: bad arity"
#define ERR_MSG_PMAP_ARITY "pmap: bad arity"
#define ERR_MSG_MAP_L_ARITY "map-left: bad arity"
#define ERR_MSG_MAP_R_ARITY "map-right: bad arity"
#define ERR_MSG_FOLD_ARITY "fold: bad arity"
#define ERR_MSG_FOLD_L_ARITY "fold-left: bad arity"
#define ERR_MSG_FOLD_R_ARITY "fold-right: bad arity"
#define ERR_MSG_SCAN_ARITY "scan: bad arity"
#define ERR_MSG_SCAN_L_ARITY "scan-left: bad arity"
#define ERR_MSG_SCAN_R_ARITY "scan-right: bad arity"
#define ERR_MSG_APPLY_ARITY "apply: bad arity"
#define ERR_MSG_MAP_LEN "map: length mismatch"
#define ERR_MSG_PMAP_LEN "pmap: length mismatch"
#define ERR_MSG_VARY_LEN "vary: length mismatch"

// Join error messages (compact)
#define ERR_MSG_LJ_ARG1 "lj: arg1 must be symbol"
#define ERR_MSG_LJ_ARG2 "lj: arg2 must be table"
#define ERR_MSG_LJ_ARG3 "lj: arg3 must be table"
#define ERR_MSG_LJ_NO_COLS "lj: no columns to join"
#define ERR_MSG_IJ_ARG1 "ij: arg1 must be symbol"
#define ERR_MSG_IJ_ARG2 "ij: arg2 must be table"
#define ERR_MSG_IJ_ARG3 "ij: arg3 must be table"
#define ERR_MSG_IJ_NO_COLS "ij: no columns to join"
#define ERR_MSG_AJ_ARG1 "asof-join: arg1 must be symbol"
#define ERR_MSG_AJ_ARG2 "asof-join: arg2 must be table"
#define ERR_MSG_AJ_ARG3 "asof-join: arg3 must be table"
#define ERR_MSG_AJ_KEY "asof-join: key not found"
#define ERR_MSG_AJ_TYPES "asof-join: incompatible types"
#define ERR_MSG_WJ_ARG1 "wj: arg1 must be symbol"
#define ERR_MSG_WJ_ARG2 "wj: arg2 must be windows"
#define ERR_MSG_WJ_ARG3 "wj: arg3 must be table"
#define ERR_MSG_WJ_ARG4 "wj: arg4 must be table"
#define ERR_MSG_WJ_ARG5 "wj: arg5 must be dict"
#define ERR_MSG_WJ_KEY "wj: key not found"
#define ERR_MSG_WJ_TYPES "wj: incompatible types"

// Return error_str directly (for non-THROW contexts)
#define RETURN_ERR_S(t, msg) return error_str(t, msg)

// Consolidated error macros for common patterns
#define THROW_TYPE1(fn, t) THROW(ERR_TYPE, "%s: unsupported type: '%s", fn, type_name(t))
#define THROW_TYPE2(fn, t1, t2) THROW(ERR_TYPE, "%s: unsupported types: '%s, '%s", fn, type_name(t1), type_name(t2))

#define PANIC(...)                                                                             \
    do {                                                                                       \
        fprintf(stderr, "process panicked at: %s:%d in %s(): ", __FILE__, __LINE__, __func__); \
        fprintf(stderr, __VA_ARGS__);                                                          \
        fprintf(stderr, "\n");                                                                 \
        exit(1);                                                                               \
    } while (0)

typedef struct loc_t {
    span_t span;
    obj_p file;
    obj_p source;
} loc_t;

typedef struct ray_error_t {
    i64_t code;
    obj_p msg;
    obj_p locs;
} *ray_error_p;

obj_p error_obj(i8_t code, obj_p msg);
obj_p error_str(i8_t code, lit_p msg);
obj_p ray_error(i8_t code, lit_p fmt, ...);

#endif  // ERROR_H
