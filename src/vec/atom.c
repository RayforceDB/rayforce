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

#include "atom.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Simple atom constructors
 *
 * Pattern: allocate 0-byte data block (just the 32B header), set type to
 * negative tag, store value in header union field.
 * -------------------------------------------------------------------------- */

ray_t* ray_bool(bool val) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_BOOL;
    v->b8 = val ? 1 : 0;
    return v;
}

ray_t* ray_u8(uint8_t val) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_U8;
    v->u8 = val;
    return v;
}

ray_t* ray_i16(int16_t val) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_I16;
    v->i16 = val;
    return v;
}

ray_t* ray_i32(int32_t val) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_I32;
    v->i32 = val;
    return v;
}

ray_t* ray_i64(int64_t val) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_I64;
    v->i64 = val;
    return v;
}

ray_t* ray_f64(double val) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_F64;
    v->f64 = val;
    return v;
}

/* F32 atoms reuse the f64 union slot — fmt_obj's RAY_F32 branch already
 * narrows back to float via `(float)obj->f64`.  Constructor mirrors
 * ray_f64; only the type tag differs.  Provided so RAY_F32 vectors can
 * box elements through the same atom-construction path used by I32/F64. */
ray_t* ray_f32(float val) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_F32;
    v->f64  = (double)val;
    return v;
}

/* --------------------------------------------------------------------------
 * String atom: SSO for <= 7 bytes, long string via U8 vector for > 7
 * -------------------------------------------------------------------------- */

ray_t* ray_str(const char* s, size_t len) {
    if (len < 7) {
        /* SSO path: store inline in header (< 7 leaves room for NUL).
         * Exactly 7 bytes would fill all of sdata[7] with no NUL terminator,
         * so 7-byte strings fall through to the long-string path. */
        ray_t* v = ray_alloc(0);
        if (RAY_IS_ERR(v)) return v;
        v->type = -RAY_STR;
        v->slen = (uint8_t)len;
        if (len > 0) memcpy(v->sdata, s, len);
        v->sdata[len] = '\0';
        return v;
    }
    /* Long string: allocate a U8 vector to hold the data, store pointer.
     * Allocate len+1 and null-terminate for C string compatibility — callers
     * (including ctypes c_char_p) may read until '\0'. */
    size_t data_size = len + 1;
    ray_t* chars = ray_alloc(data_size);
    if (!chars || RAY_IS_ERR(chars)) return chars;
    chars->type = RAY_U8;
    chars->len = (int64_t)len;
    memcpy(ray_data(chars), s, len);
    ((char*)ray_data(chars))[len] = '\0';

    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) {
        ray_free(chars);
        return v;
    }
    v->type = -RAY_STR;
    v->obj = chars;
    return v;
}

/* --------------------------------------------------------------------------
 * Symbol atom: intern ID stored as i64
 * -------------------------------------------------------------------------- */

ray_t* ray_sym(int64_t id) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_SYM;
    v->i64 = id;
    return v;
}

/* --------------------------------------------------------------------------
 * Date/Time/Timestamp atoms
 *
 * All atom constructors accept int64_t and store in the i64 union field
 * (atoms are scalar wrappers — always 8 bytes in the union). The vector
 * element sizes differ: DATE=4, TIME=4, TIMESTAMP=8. When broadcasting
 * an atom to a vector (materialize_broadcast_input), the value must be
 * narrowed to the correct element width.
 * -------------------------------------------------------------------------- */

ray_t* ray_date(int64_t val) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_DATE;
    v->i64 = val;
    return v;
}

ray_t* ray_time(int64_t val) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_TIME;
    v->i64 = val;
    return v;
}

ray_t* ray_timestamp(int64_t val) {
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = -RAY_TIMESTAMP;
    v->i64 = val;
    return v;
}

ray_t* ray_typed_null(int8_t type) {
    if (type >= 0) return ray_error("type", NULL);
    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) return v;
    v->type = type;
    v->i64 = 0;
    v->nullmap[0] |= 1;
    return v;
}

/* --------------------------------------------------------------------------
 * GUID atom: 16 bytes stored in a U8 vector, pointer in obj field
 * -------------------------------------------------------------------------- */

ray_t* ray_guid(const uint8_t* bytes) {
    /* Allocate U8 vector of length 16 */
    ray_t* vec = ray_alloc(16);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    vec->type = RAY_U8;
    vec->len = 16;
    memcpy(ray_data(vec), bytes, 16);

    ray_t* v = ray_alloc(0);
    if (RAY_IS_ERR(v)) {
        ray_free(vec);
        return v;
    }
    v->type = -RAY_GUID;
    v->obj = vec;
    return v;
}
