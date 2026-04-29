/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

#include "core/types.h"

/* Element sizes indexed by type tag.  Only types 0-14 (vectors) have
 * non-zero entries; remaining indices are zero (safe for non-vector types). */
const uint8_t ray_type_sizes[256] = {
    /* [RAY_LIST]      =  0 */ 8,   /* pointer-sized (ray_t*) */
    /* [RAY_BOOL]      =  1 */ 1,
    /* [RAY_U8]        =  2 */ 1,
    /* [RAY_I16]       =  3 */ 2,
    /* [RAY_I32]       =  4 */ 4,
    /* [RAY_I64]       =  5 */ 8,
    /* [RAY_F32]       =  6 */ 4,
    /* [RAY_F64]       =  7 */ 8,
    /* [RAY_DATE]      =  8 */ 4,
    /* [RAY_TIME]      =  9 */ 4,
    /* [RAY_TIMESTAMP] = 10 */ 8,
    /* [RAY_GUID]      = 11 */ 16,
    /* [RAY_SYM]       = 12 */ 8,   /* W64 default; narrow widths use ray_sym_elem_size */
    /* [RAY_STR]       = 13 */ 16,  /* sizeof(ray_str_t) */
    /* [RAY_SEL]       = 14 */ 0,   /* variable-size layout, no elem_size */
};

/* ===== Semantic Version API ===== */

/* Stringify helpers to build version string from header macros */
#define RAY_VER_STR_(x) #x
#define RAY_VER_STR(x)  RAY_VER_STR_(x)
#define RAY_VERSION_STRING_ \
    RAY_VER_STR(RAY_VERSION_MAJOR) "." RAY_VER_STR(RAY_VERSION_MINOR) "." RAY_VER_STR(RAY_VERSION_PATCH)

int  ray_version_major(void)         { return RAY_VERSION_MAJOR; }
int  ray_version_minor(void)         { return RAY_VERSION_MINOR; }
int  ray_version_patch(void)         { return RAY_VERSION_PATCH; }
const char* ray_version_string(void) { return RAY_VERSION_STRING_; }
