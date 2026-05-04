/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_OS_WINDOWS
#  define _GNU_SOURCE   /* fileno() for fsync-after-fwrite below */
#endif

#include "serde.h"
#include "store/col.h"
#include "store/fileio.h"
#include "core/types.h"
#include "mem/heap.h"
#include "vec/str.h"
#include "vec/vec.h"
#include "ops/ops.h"

#ifndef RAY_OS_WINDOWS
#  include <unistd.h>
#endif
#include "table/sym.h"
#include "lang/env.h"
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Wire format:
 *
 *   byte 0:   type tag (int8_t — negative = atom, positive = vector/compound)
 *
 *   Atoms (type < 0):
 *     BOOL/U8:        1 byte value
 *     I16:            2 bytes
 *     I32/DATE/TIME:  4 bytes
 *     F32:            4 bytes
 *     I64/TIMESTAMP:  8 bytes
 *     F64:            8 bytes
 *     SYM:            null-terminated string (interned on deserialize)
 *     GUID:           16 bytes
 *     STR:            i64 length + raw bytes (no null terminator)
 *
 *   Vectors (type > 0):
 *     attrs byte + i64 length + element data
 *     SYM vector: each element as null-terminated string
 *     STR vector: each element as i64 length + raw bytes
 *     LIST: each element recursively serialized
 *
 *   TABLE/DICT: attrs byte + keys(recursive) + values(recursive)
 *   LAMBDA:     attrs byte + params(recursive) + body(recursive)
 *   UNARY/BINARY/VARY: function name as null-terminated string
 *   ERROR:      8-byte sdata (packed error code)
 *   NULL (type=0 with len=0): just the type byte
 * -------------------------------------------------------------------------- */

/* Helper: strlen with bounds */
static size_t safe_strlen(const uint8_t* buf, int64_t max) {
    for (int64_t i = 0; i < max; i++)
        if (buf[i] == 0) return (size_t)i;
    return (size_t)max;
}

/* Null bitmap size for a vector (0 if no nulls) */
static int64_t null_bitmap_size(ray_t* v) {
    if (!(v->attrs & RAY_ATTR_HAS_NULLS)) return 0;
    return (v->len + 7) / 8;
}

/* Write null bitmap bytes into buf. Returns bytes written.
 * Uses ray_vec_nullmap_bytes so HAS_INDEX, slice, ext, and inline storage
 * forms all serialize the correct bits.  bit_offset is non-zero only for
 * slices, which (per pre-existing serde behaviour) are saved as if they
 * had no nulls — null_bitmap_size returns 0 since the slice's own attrs
 * lack HAS_NULLS — so we never reach this with off>0. */
static int64_t ser_null_bitmap(uint8_t* buf, ray_t* v) {
    int64_t bsz = null_bitmap_size(v);
    if (bsz <= 0) return 0;

    int64_t bit_off = 0, len_bits = 0;
    const uint8_t* bits = ray_vec_nullmap_bytes(v, &bit_off, &len_bits);
    if (!bits || bit_off != 0) {
        memset(buf, 0, (size_t)bsz);
        return bsz;
    }
    int64_t avail_bytes = (len_bits + 7) / 8;
    int64_t copy = bsz < avail_bytes ? bsz : avail_bytes;
    memcpy(buf, bits, (size_t)copy);
    if (copy < bsz) memset(buf + copy, 0, (size_t)(bsz - copy));
    return bsz;
}

/* Restore null bitmap from buf into vector. Returns bytes consumed. */
static int64_t de_null_bitmap(const uint8_t* buf, int64_t avail, ray_t* v) {
    int64_t bsz = (v->len + 7) / 8;
    if (avail < bsz) return -1;

    v->attrs |= RAY_ATTR_HAS_NULLS;

    if (v->type == RAY_STR || v->len > 128) {
        /* Must use external nullmap (STR always, others when > 128 elements) */
        ray_t* ext = ray_vec_new(RAY_U8, bsz);
        if (!ext || RAY_IS_ERR(ext)) return -1;
        ext->len = bsz;
        memcpy(ray_data(ext), buf, (size_t)bsz);
        v->attrs |= RAY_ATTR_NULLMAP_EXT;
        v->ext_nullmap = ext;
    } else {
        /* Inline nullmap */
        memcpy(v->nullmap, buf, (size_t)bsz);
    }
    return bsz;
}

/* --------------------------------------------------------------------------
 * ray_serde_size — calculate serialized size (excluding IPC header)
 * -------------------------------------------------------------------------- */

int64_t ray_serde_size(ray_t* obj) {
    if (!obj) return 1; /* RAY_SERDE_NULL marker */
    if (RAY_IS_ERR(obj)) return 1 + 8; /* type + sdata */
    if (RAY_IS_NULL(obj)) return 1; /* just the null type byte */

    int8_t type = obj->type;

    /* Atoms (negative type).  Format: type(1) + flags(1) + value-bytes.
     * `flags` carries the typed-null bit so a deserialize round-trip
     * restores 0Nl/0Nf/0Nd/0Nt etc. instead of decoding the zero-value
     * payload as a plain atom (see ray_typed_null / RAY_ATOM_IS_NULL). */
    if (type < 0) {
        int8_t base = -type;
        switch (base) {
        case RAY_BOOL:
        case RAY_U8:        return 1 + 1 + 1;
        case RAY_I16:       return 1 + 1 + 2;
        case RAY_I32:
        case RAY_DATE:
        case RAY_TIME:
        case RAY_F32:       return 1 + 1 + 4;
        case RAY_I64:
        case RAY_TIMESTAMP:
        case RAY_F64:       return 1 + 1 + 8;
        case RAY_GUID:      return 1 + 1 + 16;
        case RAY_SYM: {
            ray_t* s = ray_sym_str(obj->i64);
            return 1 + 1 + (s ? (int64_t)ray_str_len(s) : 0) + 1; /* +1 for null terminator */
        }
        case RAY_STR: {
            return 1 + 1 + 8 + (int64_t)ray_str_len(obj);
        }
        default: return 0;
        }
    }

    /* NULL object: type=LIST with len=0, but we check for actual NULL semantics */

    /* Vectors — format: type(1) + attrs(1) + len(8) + data + nullmap */
    int64_t nbm = null_bitmap_size(obj);

    /* Overflow guard: worst case is GUID at 16 bytes/elem */
    if (obj->len > (INT64_MAX - 32) / 16) return -1;

    switch (type) {
    case RAY_BOOL:
    case RAY_U8:        return 1 + 1 + 8 + obj->len + nbm;
    case RAY_I16:       return 1 + 1 + 8 + obj->len * 2 + nbm;
    case RAY_I32:
    case RAY_DATE:
    case RAY_TIME:
    case RAY_F32:       return 1 + 1 + 8 + obj->len * 4 + nbm;
    case RAY_I64:
    case RAY_TIMESTAMP:
    case RAY_F64:       return 1 + 1 + 8 + obj->len * 8 + nbm;
    case RAY_GUID:      return 1 + 1 + 8 + obj->len * 16 + nbm;
    case RAY_SYM: {
        int64_t size = 1 + 1 + 8;
        int64_t* ids = (int64_t*)ray_data(obj);
        for (int64_t i = 0; i < obj->len; i++) {
            ray_t* s = ray_sym_str(ids[i]);
            size += (s ? (int64_t)ray_str_len(s) : 0) + 1;
        }
        return size + nbm;
    }
    case RAY_STR: {
        int64_t size = 1 + 1 + 8;
        ray_str_t* elems = (ray_str_t*)ray_data(obj);
        for (int64_t i = 0; i < obj->len; i++)
            size += 8 + elems[i].len; /* i64 length + raw bytes */
        return size + nbm;
    }
    case RAY_LIST: {
        int64_t size = 1 + 1 + 8;
        ray_t** elems = (ray_t**)ray_data(obj);
        for (int64_t i = 0; i < obj->len; i++)
            size += ray_serde_size(elems[i]);
        return size;
    }
    case RAY_TABLE: {
        /* type + attrs + schema(recursive) + cols(recursive RAY_LIST) */
        ray_t** slots = (ray_t**)ray_data(obj);
        return 1 + 1 + ray_serde_size(slots[0]) + ray_serde_size(slots[1]);
    }
    case RAY_DICT: {
        /* type + attrs + keys(recursive) + vals(recursive) */
        ray_t** slots = (ray_t**)ray_data(obj);
        return 1 + 1 + ray_serde_size(slots[0]) + ray_serde_size(slots[1]);
    }
    case RAY_LAMBDA: {
        ray_t** slots = (ray_t**)ray_data(obj);
        return 1 + 1 + ray_serde_size(slots[0]) + ray_serde_size(slots[1]);
    }
    case RAY_UNARY:
    case RAY_BINARY:
    case RAY_VARY: {
        /* Serialize by name (null-terminated string in nullmap) */
        const char* name = ray_fn_name(obj);
        size_t nlen = strlen(name); if (nlen > 15) nlen = 15;
        return 1 + (int64_t)nlen + 1; /* type + name + null terminator */
    }
    case RAY_ERROR:
        return 1 + 8; /* sdata */
    default:
        return 0;
    }
}

/* --------------------------------------------------------------------------
 * ray_ser_raw — serialize into buffer, returns bytes written
 * -------------------------------------------------------------------------- */

int64_t ray_ser_raw(uint8_t* buf, ray_t* obj) {
    if (!obj) {
        buf[0] = RAY_SERDE_NULL;
        return 1;
    }
    if (RAY_IS_ERR(obj)) {
        buf[0] = (uint8_t)RAY_ERROR;
        memcpy(buf + 1, obj->sdata, 7);
        buf[8] = 0;
        return 1 + 8;
    }
    /* RAY_NULL_OBJ — ray_serde_size mirrors this with `return 1`.
     * Without this branch, ser_raw fell through to the vec path,
     * wrote `obj->type` (= 126 = RAY_SERDE_NULL on the wire) plus
     * vector-shape garbage, corrupting any frame that contained a
     * null result (e.g. a list response from a `(println ...)`
     * eval whose second element is the bare null sentinel). */
    if (RAY_IS_NULL(obj)) {
        buf[0] = RAY_SERDE_NULL;
        return 1;
    }

    int8_t type = obj->type;
    buf[0] = (uint8_t)type;
    buf++;

    /* Atoms — format: type(1) + flags(1) + value-bytes.  `flags` bit 0
     * carries the typed-null marker (nullmap[0] & 1 on the source atom)
     * so (de (ser 0Nl)) roundtrips instead of decoding as plain 0. */
    if (type < 0) {
        uint8_t aflags = (uint8_t)(obj->nullmap[0] & 1);
        buf[0] = aflags;
        buf++;
        int8_t base = -type;
        switch (base) {
        case RAY_BOOL:
        case RAY_U8:
            buf[0] = obj->u8;
            return 1 + 1 + 1;
        case RAY_I16:
            memcpy(buf, &obj->i16, 2);
            return 1 + 1 + 2;
        case RAY_I32:
        case RAY_DATE:
        case RAY_TIME:
            memcpy(buf, &obj->i32, 4);
            return 1 + 1 + 4;
        case RAY_F32: {
            /* F32 atoms store the value in obj->f64 (see ray_f32 in
             * src/vec/atom.c).  Earlier code read &obj->i32 hoping
             * those bytes aliased the float — but f64 is 8 bytes, so
             * the low half is just the lsb of the double bit pattern,
             * not the float value.  Narrow explicitly. */
            float f = (float)obj->f64;
            memcpy(buf, &f, 4);
            return 1 + 1 + 4;
        }
        case RAY_I64:
        case RAY_TIMESTAMP:
            memcpy(buf, &obj->i64, 8);
            return 1 + 1 + 8;
        case RAY_F64:
            memcpy(buf, &obj->f64, 8);
            return 1 + 1 + 8;
        case RAY_GUID: {
            /* GUID atom stored via obj pointer to 16-byte data */
            ray_t* gv = obj->obj;
            if (gv) memcpy(buf, ray_data(gv), 16);
            else    memset(buf, 0, 16);
            return 1 + 1 + 16;
        }
        case RAY_SYM: {
            ray_t* s = ray_sym_str(obj->i64);
            if (s) {
                size_t slen = ray_str_len(s);
                memcpy(buf, ray_str_ptr(s), slen);
                buf[slen] = '\0';
                return 1 + 1 + (int64_t)slen + 1;
            }
            buf[0] = '\0';
            return 1 + 1 + 1;
        }
        case RAY_STR: {
            size_t slen = ray_str_len(obj);
            const char* p = ray_str_ptr(obj);
            if (!p) { p = ""; slen = 0; }
            int64_t n = (int64_t)slen;
            memcpy(buf, &n, 8);
            memcpy(buf + 8, p, slen);
            return 1 + 1 + 8 + (int64_t)slen;
        }
        default: return 0;
        }
    }

    /* Vectors and compound types */
    int64_t c;

    /* Attrs byte: preserve HAS_NULLS, clear SLICE/NULLMAP_EXT/ARENA (internal flags) */
    uint8_t wire_attrs = obj->attrs & (RAY_ATTR_HAS_NULLS);

    switch (type) {
    case RAY_BOOL:
    case RAY_U8: {
        buf[0] = wire_attrs; buf++;
        memcpy(buf, &obj->len, 8); buf += 8;
        memcpy(buf, ray_data(obj), obj->len);
        c = 1 + 1 + 8 + obj->len;
        c += ser_null_bitmap(buf + obj->len, obj);
        return c;
    }
    case RAY_I16: {
        buf[0] = wire_attrs; buf++;
        memcpy(buf, &obj->len, 8); buf += 8;
        int64_t dsz = obj->len * 2;
        memcpy(buf, ray_data(obj), dsz);
        c = 1 + 1 + 8 + dsz;
        c += ser_null_bitmap(buf + dsz, obj);
        return c;
    }
    case RAY_I32:
    case RAY_DATE:
    case RAY_TIME:
    case RAY_F32: {
        buf[0] = wire_attrs; buf++;
        memcpy(buf, &obj->len, 8); buf += 8;
        int64_t dsz = obj->len * 4;
        memcpy(buf, ray_data(obj), dsz);
        c = 1 + 1 + 8 + dsz;
        c += ser_null_bitmap(buf + dsz, obj);
        return c;
    }
    case RAY_I64:
    case RAY_TIMESTAMP:
    case RAY_F64: {
        buf[0] = wire_attrs; buf++;
        memcpy(buf, &obj->len, 8); buf += 8;
        int64_t dsz = obj->len * 8;
        memcpy(buf, ray_data(obj), dsz);
        c = 1 + 1 + 8 + dsz;
        c += ser_null_bitmap(buf + dsz, obj);
        return c;
    }
    case RAY_GUID: {
        buf[0] = wire_attrs; buf++;
        memcpy(buf, &obj->len, 8); buf += 8;
        int64_t dsz = obj->len * 16;
        memcpy(buf, ray_data(obj), dsz);
        c = 1 + 1 + 8 + dsz;
        c += ser_null_bitmap(buf + dsz, obj);
        return c;
    }
    case RAY_SYM: {
        buf[0] = wire_attrs; buf++;
        memcpy(buf, &obj->len, 8); buf += 8;
        int64_t* ids = (int64_t*)ray_data(obj);
        c = 0;
        for (int64_t i = 0; i < obj->len; i++) {
            ray_t* s = ray_sym_str(ids[i]);
            if (s) {
                size_t slen = ray_str_len(s);
                memcpy(buf + c, ray_str_ptr(s), slen);
                c += (int64_t)slen;
            }
            buf[c] = '\0';
            c++;
        }
        c += ser_null_bitmap(buf + c, obj);
        return 1 + 1 + 8 + c;
    }

    case RAY_STR: {
        buf[0] = wire_attrs; buf++;
        memcpy(buf, &obj->len, 8); buf += 8;
        ray_str_t* elems = (ray_str_t*)ray_data(obj);
        const char* pool = obj->str_pool ? (const char*)ray_data(obj->str_pool) : NULL;
        c = 0;
        for (int64_t i = 0; i < obj->len; i++) {
            int64_t slen = (int64_t)elems[i].len;
            memcpy(buf + c, &slen, 8);
            c += 8;
            const char* p = ray_str_t_ptr(&elems[i], pool);
            memcpy(buf + c, p, (size_t)slen);
            c += slen;
        }
        c += ser_null_bitmap(buf + c, obj);
        return 1 + 1 + 8 + c;
    }

    case RAY_LIST: {
        buf[0] = obj->attrs;
        buf++;
        memcpy(buf, &obj->len, 8);
        buf += 8;
        ray_t** elems = (ray_t**)ray_data(obj);
        c = 0;
        for (int64_t i = 0; i < obj->len; i++)
            c += ray_ser_raw(buf + c, elems[i]);
        return 1 + 1 + 8 + c;
    }

    case RAY_TABLE: {
        /* Layout: type + attrs + schema(recursive) + cols(recursive RAY_LIST) */
        buf[0] = obj->attrs;
        buf++;
        ray_t** slots = (ray_t**)ray_data(obj);
        c = ray_ser_raw(buf, slots[0]);          /* schema (RAY_I64 vector) */
        c += ray_ser_raw(buf + c, slots[1]);     /* cols (RAY_LIST) */
        return 1 + 1 + c;
    }

    case RAY_DICT: {
        buf[0] = obj->attrs;
        buf++;
        ray_t** slots = (ray_t**)ray_data(obj);
        c = ray_ser_raw(buf, slots[0]);
        c += ray_ser_raw(buf + c, slots[1]);
        return 1 + 1 + c;
    }

    case RAY_LAMBDA: {
        buf[0] = obj->attrs;
        buf++;
        ray_t** slots = (ray_t**)ray_data(obj);
        c = ray_ser_raw(buf, slots[0]);     /* params */
        c += ray_ser_raw(buf + c, slots[1]); /* body */
        return 1 + 1 + c;
    }

    case RAY_UNARY:
    case RAY_BINARY:
    case RAY_VARY: {
        /* Serialize builtin by name (null-terminated) */
        const char* name = ray_fn_name(obj);
        size_t nlen = strlen(name); if (nlen > 15) nlen = 15;
        memcpy(buf, name, nlen);
        buf[nlen] = 0;
        return 1 + (int64_t)nlen + 1;
    }

    case RAY_ERROR:
        memcpy(buf, obj->sdata, 7);
        buf[7] = 0;
        return 1 + 8;

    default:
        return 0;
    }
}

/* --------------------------------------------------------------------------
 * ray_de_raw — deserialize from buffer
 * -------------------------------------------------------------------------- */

ray_t* ray_de_raw(uint8_t* buf, int64_t* len) {
    if (*len < 1) return NULL;

    int8_t type = (int8_t)buf[0];
    buf++;
    (*len)--;

    /* Null */
    if ((uint8_t)type == RAY_SERDE_NULL) return NULL;

    /* Atoms — read 1-byte flags (typed-null bit) before the value.  If
     * the null bit is set we always return ray_typed_null(type) regardless
     * of the value bytes, which are still read/skipped to keep the buffer
     * position in sync with the serialized length. */
    if (type < 0) {
        if (*len < 1) return ray_error("domain", NULL);
        uint8_t aflags = buf[0];
        buf++; (*len)--;
        bool is_null = (aflags & 1) != 0;
        int8_t base = -type;
        switch (base) {
        case RAY_BOOL:
            if (*len < 1) return ray_error("domain", NULL);
            (*len)--;
            return is_null ? ray_typed_null(type) : ray_bool(buf[0]);
        case RAY_U8:
            if (*len < 1) return ray_error("domain", NULL);
            (*len)--;
            return is_null ? ray_typed_null(type) : ray_u8(buf[0]);
        case RAY_I16:
            if (*len < 2) return ray_error("domain", NULL);
            { int16_t v; memcpy(&v, buf, 2); *len -= 2;
              return is_null ? ray_typed_null(type) : ray_i16(v); }
        case RAY_I32:
            if (*len < 4) return ray_error("domain", NULL);
            { int32_t v; memcpy(&v, buf, 4); *len -= 4;
              return is_null ? ray_typed_null(type) : ray_i32(v); }
        case RAY_DATE:
            if (*len < 4) return ray_error("domain", NULL);
            { int32_t v; memcpy(&v, buf, 4); *len -= 4;
              return is_null ? ray_typed_null(type) : ray_date((int64_t)v); }
        case RAY_TIME:
            if (*len < 4) return ray_error("domain", NULL);
            { int32_t v; memcpy(&v, buf, 4); *len -= 4;
              return is_null ? ray_typed_null(type) : ray_time((int64_t)v); }
        case RAY_F32:
            if (*len < 4) return ray_error("domain", NULL);
            { float v; memcpy(&v, buf, 4); *len -= 4;
              return is_null ? ray_typed_null(-RAY_F32)
                             : ray_f32(v); }
        case RAY_I64:
            if (*len < 8) return ray_error("domain", NULL);
            { int64_t v; memcpy(&v, buf, 8); *len -= 8;
              return is_null ? ray_typed_null(type) : ray_i64(v); }
        case RAY_TIMESTAMP:
            if (*len < 8) return ray_error("domain", NULL);
            { int64_t v; memcpy(&v, buf, 8); *len -= 8;
              return is_null ? ray_typed_null(type) : ray_timestamp(v); }
        case RAY_F64:
            if (*len < 8) return ray_error("domain", NULL);
            { double v; memcpy(&v, buf, 8); *len -= 8;
              return is_null ? ray_typed_null(type) : ray_f64(v); }
        case RAY_GUID:
            if (*len < 16) return ray_error("domain", NULL);
            *len -= 16;
            return is_null ? ray_typed_null(type) : ray_guid(buf);
        case RAY_SYM: {
            size_t slen = safe_strlen(buf, *len);
            if ((int64_t)slen >= *len) return ray_error("domain", NULL);
            *len -= (int64_t)slen + 1;
            if (is_null) return ray_typed_null(type);
            int64_t id = ray_sym_intern((const char*)buf, slen);
            return ray_sym(id);
        }
        case RAY_STR: {
            if (*len < 8) return ray_error("domain", NULL);
            int64_t slen; memcpy(&slen, buf, 8);
            buf += 8; *len -= 8;
            if (*len < slen || slen < 0) return ray_error("domain", NULL);
            *len -= slen;
            if (is_null) return ray_typed_null(type);
            return ray_str((const char*)buf, (size_t)slen);
        }
        default:
            return ray_error("type", NULL);
        }
    }

    /* Vectors and compounds */
    int64_t l;

    switch (type) {
    case RAY_BOOL:
    case RAY_U8:
    case RAY_I16:
    case RAY_I32:
    case RAY_DATE:
    case RAY_TIME:
    case RAY_F32:
    case RAY_I64:
    case RAY_TIMESTAMP:
    case RAY_F64:
    case RAY_GUID: {
        if (*len < 9) return ray_error("domain", NULL);
        uint8_t attrs = buf[0];
        buf++;
        memcpy(&l, buf, 8);
        buf += 8;
        *len -= 9;

        if (l < 0 || l > 1000000000) return ray_error("domain", NULL);

        uint8_t esz = ray_type_sizes[type];
        int64_t data_bytes = l * esz;
        if (*len < data_bytes) return ray_error("domain", NULL);

        ray_t* vec = ray_vec_from_raw(type, buf, l);
        if (!vec || RAY_IS_ERR(vec)) return vec;
        buf += data_bytes;
        *len -= data_bytes;

        /* Restore null bitmap if present */
        if (attrs & RAY_ATTR_HAS_NULLS) {
            int64_t consumed = de_null_bitmap(buf, *len, vec);
            if (consumed < 0) { ray_release(vec); return ray_error("domain", NULL); }
            buf += consumed;
            *len -= consumed;
        }
        return vec;
    }

    case RAY_SYM: {
        if (*len < 9) return ray_error("domain", NULL);
        uint8_t attrs = buf[0];
        buf++;
        memcpy(&l, buf, 8);
        buf += 8;
        *len -= 9;

        if (l < 0 || l > 1000000000) return ray_error("domain", NULL);

        ray_t* vec = ray_vec_new(RAY_SYM, l);
        if (!vec || RAY_IS_ERR(vec)) return vec;
        vec->len = l;
        int64_t* ids = (int64_t*)ray_data(vec);
        for (int64_t i = 0; i < l; i++) {
            size_t slen = safe_strlen(buf, *len);
            if ((int64_t)slen >= *len) {
                vec->len = i;
                ray_release(vec);
                return ray_error("domain", NULL);
            }
            ids[i] = ray_sym_intern((const char*)buf, slen);
            buf += slen + 1;
            *len -= (int64_t)slen + 1;
        }

        if (attrs & RAY_ATTR_HAS_NULLS) {
            int64_t consumed = de_null_bitmap(buf, *len, vec);
            if (consumed < 0) { ray_release(vec); return ray_error("domain", NULL); }
            buf += consumed;
            *len -= consumed;
        }
        return vec;
    }

    case RAY_STR: {
        if (*len < 9) return ray_error("domain", NULL);
        uint8_t attrs = buf[0];
        buf++;
        memcpy(&l, buf, 8);
        buf += 8;
        *len -= 9;

        if (l < 0 || l > 1000000000) return ray_error("domain", NULL);

        /* Build STR vector by appending each string via ray_str_vec_append */
        ray_t* vec = ray_vec_new(RAY_STR, l);
        if (!vec || RAY_IS_ERR(vec)) return vec;
        vec->len = 0;
        for (int64_t i = 0; i < l; i++) {
            if (*len < 8) { ray_release(vec); return ray_error("domain", NULL); }
            int64_t slen; memcpy(&slen, buf, 8);
            buf += 8; *len -= 8;
            if (*len < slen || slen < 0) { ray_release(vec); return ray_error("domain", NULL); }
            ray_t* nv = ray_str_vec_append(vec, (const char*)buf, (size_t)slen);
            if (!nv || RAY_IS_ERR(nv)) { ray_release(vec); return nv ? nv : ray_error("oom", NULL); }
            vec = nv;
            buf += slen;
            *len -= slen;
        }

        if (attrs & RAY_ATTR_HAS_NULLS) {
            int64_t consumed = de_null_bitmap(buf, *len, vec);
            if (consumed < 0) { ray_release(vec); return ray_error("domain", NULL); }
            buf += consumed;
            *len -= consumed;
        }
        return vec;
    }

    case RAY_LIST: {
        if (*len < 9) return ray_error("domain", NULL);
        uint8_t list_attrs = buf[0];
        buf++;
        memcpy(&l, buf, 8);
        buf += 8;
        *len -= 9;

        if (l < 0 || l > 1000000000) return ray_error("domain", NULL);

        ray_t* list = ray_alloc(l * sizeof(ray_t*));
        if (!list || RAY_IS_ERR(list)) return list;
        list->type = RAY_LIST;
        list->attrs = list_attrs;
        list->len = l;
        ray_t** elems = (ray_t**)ray_data(list);

        int64_t saved = *len;
        for (int64_t i = 0; i < l; i++) {
            elems[i] = ray_de_raw(buf + (saved - *len), len);
            /* A NULL element is the in-band representation of
             * RAY_NULL_OBJ on the wire (ray_ser_raw writes SERDE_NULL
             * for the null singleton; ray_de_raw returns C NULL for
             * that marker).  Lists must round-trip them — e.g. an IPC
             * VERBOSE response is `[captured_str, result]` where
             * `result` is RAY_NULL_OBJ for any (println ...) /
             * (set ...) eval.  Rejecting NULL here would error the
             * whole frame as "domain".  Substitute the singleton so
             * downstream code (ray_lang_print, etc.) sees a valid
             * pointer instead of a raw NULL. */
            if (!elems[i]) {
                elems[i] = RAY_NULL_OBJ;
            } else if (RAY_IS_ERR(elems[i])) {
                /* Clean up already-deserialized elements */
                for (int64_t j = 0; j < i; j++) ray_release(elems[j]);
                list->len = 0;
                ray_release(list);
                return elems[i];
            }
        }
        return list;
    }

    case RAY_TABLE: {
        if (*len < 1) return ray_error("domain", NULL);
        /* uint8_t tbl_attrs = buf[0]; — tables rebuild attrs via ray_table_add_col */
        buf++;
        *len -= 1;

        int64_t saved = *len;
        /* Deserialize schema (I64 vector of sym IDs) */
        ray_t* schema = ray_de_raw(buf, len);
        if (!schema || RAY_IS_ERR(schema)) return schema;

        /* Deserialize columns (as LIST) */
        ray_t* cols = ray_de_raw(buf + (saved - *len), len);
        if (!cols || RAY_IS_ERR(cols)) {
            ray_release(schema);
            return cols;
        }

        /* Reconstruct table */
        if (cols->type != RAY_LIST || schema->type != RAY_I64) {
            ray_release(schema);
            ray_release(cols);
            return ray_error("domain", NULL);
        }

        int64_t ncols = cols->len;
        ray_t* tbl = ray_table_new(ncols);
        if (!tbl || RAY_IS_ERR(tbl)) {
            ray_release(schema);
            ray_release(cols);
            return tbl;
        }

        int64_t* name_ids = (int64_t*)ray_data(schema);
        ray_t** col_ptrs = (ray_t**)ray_data(cols);
        for (int64_t i = 0; i < ncols && i < schema->len; i++) {
            ray_t* new_tbl = ray_table_add_col(tbl, name_ids[i], col_ptrs[i]);
            if (!new_tbl || RAY_IS_ERR(new_tbl)) {
                ray_release(tbl);
                ray_release(schema);
                ray_release(cols);
                return new_tbl;
            }
            tbl = new_tbl;
        }

        ray_release(schema);
        ray_release(cols);
        return tbl;
    }

    case RAY_DICT: {
        if (*len < 1) return ray_error("domain", NULL);
        uint8_t dict_attrs = buf[0];
        buf++;
        *len -= 1;

        int64_t saved = *len;
        ray_t* keys = ray_de_raw(buf, len);
        if (!keys || RAY_IS_ERR(keys)) return keys;

        ray_t* vals = ray_de_raw(buf + (saved - *len), len);
        if (!vals || RAY_IS_ERR(vals)) {
            ray_release(keys);
            return vals;
        }

        /* Build dict: alloc with 2 slots */
        ray_t* dict = ray_alloc(2 * sizeof(ray_t*));
        if (!dict || RAY_IS_ERR(dict)) {
            ray_release(keys);
            ray_release(vals);
            return dict;
        }
        dict->type = RAY_DICT;
        dict->attrs = dict_attrs;
        dict->len = 2;
        ((ray_t**)ray_data(dict))[0] = keys;
        ((ray_t**)ray_data(dict))[1] = vals;
        return dict;
    }

    case RAY_LAMBDA: {
        if (*len < 1) return ray_error("domain", NULL);
        uint8_t lam_attrs = buf[0];
        buf++;
        *len -= 1;

        int64_t saved = *len;
        ray_t* params = ray_de_raw(buf, len);
        if (!params || RAY_IS_ERR(params)) return params;

        ray_t* body = ray_de_raw(buf + (saved - *len), len);
        if (!body || RAY_IS_ERR(body)) {
            ray_release(params);
            return body;
        }

        /* Build lambda: allocate with 7 slots (same as eval.c) */
        ray_t* lambda = ray_alloc(7 * sizeof(ray_t*));
        if (!lambda || RAY_IS_ERR(lambda)) {
            ray_release(params);
            ray_release(body);
            return lambda;
        }
        lambda->type = RAY_LAMBDA;
        lambda->attrs = lam_attrs;
        lambda->len = 0;
        memset(ray_data(lambda), 0, 7 * sizeof(ray_t*));
        ((ray_t**)ray_data(lambda))[0] = params;
        ((ray_t**)ray_data(lambda))[1] = body;
        return lambda;
    }

    case RAY_UNARY:
    case RAY_BINARY:
    case RAY_VARY: {
        /* Deserialize builtin by name: read null-terminated string,
         * look up in the global environment. */
        size_t nlen = safe_strlen(buf, *len);
        if ((int64_t)nlen >= *len) return ray_error("domain", NULL);
        int64_t sym = ray_sym_intern((const char*)buf, nlen);
        *len -= (int64_t)nlen + 1;
        ray_t* fn = ray_env_get(sym);
        if (!fn) return ray_error("name", NULL);
        ray_retain(fn);
        return fn;
    }

    case RAY_ERROR: {
        if (*len < 8) return ray_error("domain", NULL);
        ray_t* err = ray_error((const char*)buf, NULL);
        *len -= 8;
        return err;
    }

    default:
        return ray_error("type", NULL);
    }
}

/* --------------------------------------------------------------------------
 * ray_ser — top-level: serialize with IPC header
 * -------------------------------------------------------------------------- */

ray_t* ray_ser(ray_t* obj) {
    int64_t payload = ray_serde_size(obj);
    if (payload <= 0) return ray_error("domain", payload < 0 ? "serialization overflow" : NULL);

    int64_t total = (int64_t)sizeof(ray_ipc_header_t) + payload;
    ray_t* buf = ray_vec_new(RAY_U8, total);
    if (!buf || RAY_IS_ERR(buf)) return buf;
    buf->len = total;

    ray_ipc_header_t* hdr = (ray_ipc_header_t*)ray_data(buf);
    hdr->prefix  = RAY_SERDE_PREFIX;
    hdr->version = RAY_SERDE_WIRE_VERSION;
    hdr->flags   = 0;
    hdr->endian  = 0;
    hdr->msgtype = 0;
    hdr->size    = payload;

    int64_t written = ray_ser_raw((uint8_t*)ray_data(buf) + sizeof(ray_ipc_header_t), obj);
    if (written == 0) {
        ray_release(buf);
        return ray_error("domain", NULL);
    }

    return buf;
}

/* --------------------------------------------------------------------------
 * ray_de — top-level: deserialize from U8 vector
 * -------------------------------------------------------------------------- */

ray_t* ray_de(ray_t* bytes) {
    if (!bytes || RAY_IS_ERR(bytes)) return ray_error("type", NULL);
    if (bytes->type != RAY_U8 && bytes->type != -RAY_U8)
        return ray_error("type", NULL);

    int64_t total = bytes->len;
    uint8_t* buf = (uint8_t*)ray_data(bytes);

    if (total < (int64_t)sizeof(ray_ipc_header_t))
        return ray_error("domain", NULL);

    ray_ipc_header_t* hdr = (ray_ipc_header_t*)buf;
    if (hdr->prefix != RAY_SERDE_PREFIX)
        return ray_error("domain", NULL);
    if (hdr->version != RAY_SERDE_WIRE_VERSION)
        return ray_error("version", "serde wire version mismatch");
    if (hdr->size < 0 || hdr->size > 1000000000)
        return ray_error("domain", NULL);
    if (hdr->size + (int64_t)sizeof(ray_ipc_header_t) != total)
        return ray_error("domain", NULL);

    int64_t len = hdr->size;
    return ray_de_raw(buf + sizeof(ray_ipc_header_t), &len);
}

/* --------------------------------------------------------------------------
 * File I/O: save/load any object in binary format
 * -------------------------------------------------------------------------- */

ray_err_t ray_obj_save(ray_t* obj, const char* path) {
    bool owned = false;
    if (ray_is_lazy(obj)) {
        obj = ray_lazy_materialize(obj);
        if (RAY_IS_ERR(obj)) {
            ray_err_t code = ray_err_from_obj(obj);
            ray_error_free(obj);
            return code;
        }
        owned = true;
    }

    ray_t* bytes = ray_ser(obj);
    if (!bytes || RAY_IS_ERR(bytes)) {
        if (bytes && RAY_IS_ERR(bytes)) ray_error_free(bytes);
        if (owned) ray_release(obj);
        return RAY_ERR_DOMAIN;
    }

    FILE* f = fopen(path, "wb");
    if (!f) { ray_release(bytes); if (owned) ray_release(obj); return RAY_ERR_IO; }

    size_t total = (size_t)bytes->len;
    size_t n = fwrite(ray_data(bytes), 1, total, f);
    if (n != total) {
        fclose(f); ray_release(bytes);
        if (owned) ray_release(obj);
        return RAY_ERR_IO;
    }

    /* Durability: fflush + fsync BEFORE fclose so a buffered write
     * hitting ENOSPC inside fclose doesn't slip through silently.
     * Callers (esp. ray_journal_snapshot) write to a .tmp then rename
     * — without this fsync the .tmp may be empty/partial on disk
     * when the rename atomically swaps it in. */
    if (fflush(f) != 0) {
        fclose(f); ray_release(bytes);
        if (owned) ray_release(obj);
        return RAY_ERR_IO;
    }
#ifndef RAY_OS_WINDOWS
    if (fsync(fileno(f)) != 0) {
        fclose(f); ray_release(bytes);
        if (owned) ray_release(obj);
        return RAY_ERR_IO;
    }
#endif
    /* fclose itself can fail (final flush of any platform-level
     * buffer).  Check it. */
    int close_rc = fclose(f);
    ray_release(bytes);
    if (owned) ray_release(obj);
    return close_rc == 0 ? RAY_OK : RAY_ERR_IO;
}

ray_t* ray_obj_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return ray_error("io", NULL);

    /* Check fseek/ftell return values — silent failures here let a
     * truncated read through as "valid empty file" or worse. */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ray_error("io", "fseek end"); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return ray_error("io", "ftell"); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ray_error("io", "fseek set"); }

    if (sz == 0) { fclose(f); return ray_error("io", "empty file"); }

    ray_t* buf = ray_vec_new(RAY_U8, sz);
    if (!buf || RAY_IS_ERR(buf)) { fclose(f); return buf; }
    buf->len = sz;

    size_t n = fread(ray_data(buf), 1, (size_t)sz, f);
    fclose(f);

    if ((long)n != sz) { ray_release(buf); return ray_error("io", "short read"); }

    ray_t* result = ray_de(buf);
    ray_release(buf);
    return result;
}
