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
#include "lang/eval.h"
#include "lang/format.h"
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

/* Attribute bits that may cross the wire and be reconstructed verbatim on
 * an object built by the deserializer.  Every OTHER attr bit — SLICE,
 * HAS_INDEX, HAS_LINK, GRAPH, HNSW, ARENA — implies a backing pointer or
 * an allocation arena that a freshly-deserialized object does not own.
 * Trusting those from an untrusted frame is a memory-safety hole: e.g. a
 * LIST payload with SLICE set makes ray_data() follow an uninitialised
 * slice_parent and segfault (a remotely reachable crash over IPC, and a
 * corrupt-journal crash on replay).  The scalar-vector paths already mask
 * down to HAS_NULLS; this covers the aggregate types (LIST/TABLE/DICT/
 * LAMBDA) which historically copied the raw byte.  SORTED is a pure,
 * self-describing marker and is safe to carry.  Applied on BOTH ends so
 * we never emit a dangerous bit and never trust one on input. */
#define RAY_SERDE_ATTR_WIRE_MASK ((uint8_t)(RAY_ATTR_HAS_NULLS | RAY_ATTR_SORTED))
/* Lambdas reuse the attr byte for function-internal flags: 0x40 is
 * RAY_FN_COMPILED, not "has nulls".  Only params/body cross the wire, so
 * compiled metadata must be stripped and rebuilt by the receiving runtime. */
#define RAY_SERDE_LAMBDA_HAS_CLOSURE ((uint8_t)0x01)
#define RAY_SERDE_LAMBDA_ATTR_WIRE_MASK RAY_SERDE_LAMBDA_HAS_CLOSURE

/* Helper: strlen with bounds */
static size_t safe_strlen(const uint8_t* buf, int64_t max) {
    for (int64_t i = 0; i < max; i++)
        if (buf[i] == 0) return (size_t)i;
    return (size_t)max;
}

/* Table schema slot: an I64 vector of column NAME ids — name-ids live in
 * the global table (sym-domain Phase 2: global resolution is correct). */
static int64_t schema_names_serde_size(ray_t* schema) {
    if (!schema || schema->type != RAY_I64) return 0;
    int64_t size = 1 + 1 + 8;
    int64_t* ids = (int64_t*)ray_data(schema);
    for (int64_t i = 0; i < schema->len; i++) {
        ray_t* s = ray_sym_str(ids[i]);
        size += (s ? (int64_t)ray_str_len(s) : 0) + 1;
    }
    return size;
}

static int64_t ser_schema_names(uint8_t* buf, ray_t* schema) {
    if (!schema || schema->type != RAY_I64) return 0;
    buf[0] = (uint8_t)RAY_SYM;
    buf[1] = RAY_SYM_W64;
    memcpy(buf + 2, &schema->len, 8);
    int64_t c = 10;
    int64_t* ids = (int64_t*)ray_data(schema);
    for (int64_t i = 0; i < schema->len; i++) {
        ray_t* s = ray_sym_str(ids[i]);
        if (s) {
            size_t slen = ray_str_len(s);
            memcpy(buf + c, ray_str_ptr(s), slen);
            c += (int64_t)slen;
        }
        buf[c++] = '\0';
    }
    return c;
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
            /* SYM ATOM: atoms are runtime-domain by design — global
             * resolution is correct (sym-domain Phase 2). */
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

    /* Vectors — format: type(1) + attrs(1) + len(8) + data.
     * Null state is sentinel-encoded in the payload — no bitmap region. */

    /* Overflow guard: worst case is GUID at 16 bytes/elem */
    if (obj->len > (INT64_MAX - 32) / 16) return -1;

    switch (type) {
    case RAY_BOOL:
    case RAY_U8:        return 1 + 1 + 8 + obj->len;
    case RAY_I16:       return 1 + 1 + 8 + obj->len * 2;
    case RAY_I32:
    case RAY_DATE:
    case RAY_TIME:
    case RAY_F32:       return 1 + 1 + 8 + obj->len * 4;
    case RAY_I64:
    case RAY_TIMESTAMP:
    case RAY_F64:       return 1 + 1 + 8 + obj->len * 8;
    case RAY_GUID:      return 1 + 1 + 8 + obj->len * 16;
    case RAY_SYM: {
        /* SYM VECTOR cells are positions in THE VEC's domain — resolve
         * each through ray_sym_vec_cell, not the global table (sym-domain
         * Phase 2; identical strings while the domain is the runtime
         * singleton).  Also honors narrow W8/16/32 index widths.  Must
         * stay in lockstep with the ray_ser_raw RAY_SYM loop below. */
        int64_t size = 1 + 1 + 8;
        for (int64_t i = 0; i < obj->len; i++) {
            ray_t* s = ray_sym_vec_cell(obj, i);
            size += (s ? (int64_t)ray_str_len(s) : 0) + 1;
        }
        return size;
    }
    case RAY_STR: {
        int64_t size = 1 + 1 + 8;
        ray_str_t* elems = (ray_str_t*)ray_data(obj);
        for (int64_t i = 0; i < obj->len; i++)
            size += 8 + elems[i].len; /* i64 length + raw bytes */
        return size;
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
        return 1 + 1 + schema_names_serde_size(slots[0]) + ray_serde_size(slots[1]);
    }
    case RAY_DICT: {
        /* type + attrs + keys(recursive) + vals(recursive) */
        ray_t** slots = (ray_t**)ray_data(obj);
        return 1 + 1 + ray_serde_size(slots[0]) + ray_serde_size(slots[1]);
    }
    case RAY_LAMBDA: {
        ray_t** slots = (ray_t**)ray_data(obj);
        int64_t size = 1 + 1 + ray_serde_size(slots[0]) + ray_serde_size(slots[1]);
        if (LAMBDA_CLOSURE(obj)) size += ray_serde_size(LAMBDA_CLOSURE(obj));
        return size;
    }
    case RAY_UNARY:
    case RAY_BINARY:
    case RAY_VARY: {
        /* Serialize by name (null-terminated string in aux) */
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
     * carries the typed-null marker (aux[0] & 1 on the source atom)
     * so (de (ser 0Nl)) roundtrips instead of decoding as plain 0. */
    if (type < 0) {
        uint8_t aflags = (uint8_t)(obj->aux[0] & 1);
        if (type == -RAY_SYM && (obj->attrs & ATTR_QUOTED))
            aflags |= ATTR_QUOTED;
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
            /* SYM ATOM — runtime-domain by design, global resolution. */
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

    /* Attrs byte: preserve HAS_NULLS; clear SLICE / ARENA (internal flags). */
    uint8_t wire_attrs = obj->attrs & (RAY_ATTR_HAS_NULLS);

    switch (type) {
    case RAY_BOOL:
    case RAY_U8: {
        buf[0] = wire_attrs; buf++;
        memcpy(buf, &obj->len, 8); buf += 8;
        memcpy(buf, ray_data(obj), obj->len);
        c = 1 + 1 + 8 + obj->len;
        return c;
    }
    case RAY_I16: {
        buf[0] = wire_attrs; buf++;
        memcpy(buf, &obj->len, 8); buf += 8;
        int64_t dsz = obj->len * 2;
        memcpy(buf, ray_data(obj), dsz);
        c = 1 + 1 + 8 + dsz;
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
        return c;
    }
    case RAY_GUID: {
        buf[0] = wire_attrs; buf++;
        memcpy(buf, &obj->len, 8); buf += 8;
        int64_t dsz = obj->len * 16;
        memcpy(buf, ray_data(obj), dsz);
        c = 1 + 1 + 8 + dsz;
        return c;
    }
    case RAY_SYM: {
        /* Cells resolve through THE VEC's domain (sym-domain Phase 2);
         * the wire format is unchanged — only the resolution source.
         * Must stay in lockstep with the ray_serde_size RAY_SYM loop. */
        buf[0] = wire_attrs; buf++;
        memcpy(buf, &obj->len, 8); buf += 8;
        c = 0;
        for (int64_t i = 0; i < obj->len; i++) {
            ray_t* s = ray_sym_vec_cell(obj, i);
            if (s) {
                size_t slen = ray_str_len(s);
                memcpy(buf + c, ray_str_ptr(s), slen);
                c += (int64_t)slen;
            }
            buf[c] = '\0';
            c++;
        }
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
        return 1 + 1 + 8 + c;
    }

    case RAY_LIST: {
        buf[0] = obj->attrs & RAY_SERDE_ATTR_WIRE_MASK;
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
        buf[0] = obj->attrs & RAY_SERDE_ATTR_WIRE_MASK;
        buf++;
        ray_t** slots = (ray_t**)ray_data(obj);
        c = ser_schema_names(buf, slots[0]);     /* schema names as RAY_SYM vector */
        c += ray_ser_raw(buf + c, slots[1]);     /* cols (RAY_LIST) */
        return 1 + 1 + c;
    }

    case RAY_DICT: {
        buf[0] = obj->attrs & RAY_SERDE_ATTR_WIRE_MASK;
        buf++;
        ray_t** slots = (ray_t**)ray_data(obj);
        c = ray_ser_raw(buf, slots[0]);
        c += ray_ser_raw(buf + c, slots[1]);
        return 1 + 1 + c;
    }

    case RAY_LAMBDA: {
        buf[0] = LAMBDA_CLOSURE(obj) ? RAY_SERDE_LAMBDA_HAS_CLOSURE : 0;
        buf++;
        ray_t** slots = (ray_t**)ray_data(obj);
        c = ray_ser_raw(buf, slots[0]);     /* params */
        c += ray_ser_raw(buf + c, slots[1]); /* body */
        if (LAMBDA_CLOSURE(obj))
            c += ray_ser_raw(buf + c, LAMBDA_CLOSURE(obj));
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

/* Bound the recursion depth so a corrupted record encoding pathologically
 * deep nesting returns an error instead of overflowing the C stack.  Mirrors
 * the eval-depth ceiling (RAY_EVAL_MAX_DEPTH).  Thread-local: ray_de_raw may
 * run on worker-pool threads. */
#define RAY_DE_MAX_DEPTH 512
static _Thread_local int g_de_depth = 0;

static ray_t* de_raw_inner(uint8_t* buf, int64_t* len);

ray_t* ray_de_raw(uint8_t* buf, int64_t* len) {
    if (g_de_depth >= RAY_DE_MAX_DEPTH)
        return ray_error("domain", "deserialize: nesting exceeds max depth %lld", (long long)RAY_DE_MAX_DEPTH);
    g_de_depth++;
    ray_t* r = de_raw_inner(buf, len);
    g_de_depth--;
    return r;
}

static ray_t* de_raw_inner(uint8_t* buf, int64_t* len) {
    if (*len < 1) return NULL;

    int8_t type = (int8_t)buf[0];
    buf++;
    (*len)--;

    /* Null */
    if ((uint8_t)type == RAY_SERDE_NULL) return RAY_NULL_OBJ;

    /* Atoms — read 1-byte flags (typed-null bit) before the value.  If
     * the null bit is set we always return ray_typed_null(type) regardless
     * of the value bytes, which are still read/skipped to keep the buffer
     * position in sync with the serialized length. */
    if (type < 0) {
        if (*len < 1) return ray_error("domain", "deserialize atom: truncated buffer reading flags byte for %s", ray_type_name(type));
        uint8_t aflags = buf[0];
        buf++; (*len)--;
        bool is_null = (aflags & 1) != 0;
        int8_t base = -type;
        switch (base) {
        case RAY_BOOL:
            if (*len < 1) return ray_error("domain", "deserialize atom: truncated bool, need 1 byte");
            (*len)--;
            return is_null ? ray_typed_null(type) : ray_bool(buf[0]);
        case RAY_U8:
            if (*len < 1) return ray_error("domain", "deserialize atom: truncated u8, need 1 byte");
            (*len)--;
            return is_null ? ray_typed_null(type) : ray_u8(buf[0]);
        case RAY_I16:
            if (*len < 2) return ray_error("domain", "deserialize atom: truncated i16, need 2 bytes");
            { int16_t v; memcpy(&v, buf, 2); *len -= 2;
              return is_null ? ray_typed_null(type) : ray_i16(v); }
        case RAY_I32:
            if (*len < 4) return ray_error("domain", "deserialize atom: truncated i32, need 4 bytes");
            { int32_t v; memcpy(&v, buf, 4); *len -= 4;
              return is_null ? ray_typed_null(type) : ray_i32(v); }
        case RAY_DATE:
            if (*len < 4) return ray_error("domain", "deserialize atom: truncated date, need 4 bytes");
            { int32_t v; memcpy(&v, buf, 4); *len -= 4;
              return is_null ? ray_typed_null(type) : ray_date((int64_t)v); }
        case RAY_TIME:
            if (*len < 4) return ray_error("domain", "deserialize atom: truncated time, need 4 bytes");
            { int32_t v; memcpy(&v, buf, 4); *len -= 4;
              return is_null ? ray_typed_null(type) : ray_time((int64_t)v); }
        case RAY_F32:
            if (*len < 4) return ray_error("domain", "deserialize atom: truncated f32, need 4 bytes");
            { float v; memcpy(&v, buf, 4); *len -= 4;
              return is_null ? ray_typed_null(-RAY_F32)
                             : ray_f32(v); }
        case RAY_I64:
            if (*len < 8) return ray_error("domain", "deserialize atom: truncated i64, need 8 bytes");
            { int64_t v; memcpy(&v, buf, 8); *len -= 8;
              return is_null ? ray_typed_null(type) : ray_i64(v); }
        case RAY_TIMESTAMP:
            if (*len < 8) return ray_error("domain", "deserialize atom: truncated timestamp, need 8 bytes");
            { int64_t v; memcpy(&v, buf, 8); *len -= 8;
              return is_null ? ray_typed_null(type) : ray_timestamp(v); }
        case RAY_F64:
            if (*len < 8) return ray_error("domain", "deserialize atom: truncated f64, need 8 bytes");
            { double v; memcpy(&v, buf, 8); *len -= 8;
              return is_null ? ray_typed_null(type) : ray_f64(v); }
        case RAY_GUID:
            if (*len < 16) return ray_error("domain", "deserialize atom: truncated guid, need 16 bytes");
            *len -= 16;
            return is_null ? ray_typed_null(type) : ray_guid(buf);
        case RAY_SYM: {
            size_t slen = safe_strlen(buf, *len);
            if ((int64_t)slen >= *len) return ray_error("domain", "deserialize atom: unterminated sym, no NUL within %lld bytes", (long long)*len);
            *len -= (int64_t)slen + 1;
            if (is_null) return ray_typed_null(type);
            /* Decode interns into the GLOBAL table: atoms are
             * runtime-domain by design (sym-domain Phase 2 — correct). */
            int64_t id = ray_sym_intern((const char*)buf, slen);
            ray_t* s = ray_sym(id);
            if (s && !RAY_IS_ERR(s) && (aflags & ATTR_QUOTED))
                s->attrs |= ATTR_QUOTED;
            return s;
        }
        case RAY_STR: {
            if (*len < 8) return ray_error("domain", "deserialize atom: truncated str length prefix, need 8 bytes");
            int64_t slen; memcpy(&slen, buf, 8);
            buf += 8; *len -= 8;
            if (*len < slen || slen < 0) return ray_error("domain", "deserialize atom: str length %lld out of range for %lld remaining bytes", (long long)slen, (long long)*len);
            *len -= slen;
            if (is_null) return ray_typed_null(type);
            return ray_str((const char*)buf, (size_t)slen);
        }
        default:
            return ray_error("type", "deserialize atom: unknown atom type %lld", (long long)type);
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
        if (*len < 9) return ray_error("domain", "deserialize vector: truncated %s header, need 9 bytes (attr+len)", ray_type_name(type));
        uint8_t attrs = buf[0];
        buf++;
        memcpy(&l, buf, 8);
        buf += 8;
        *len -= 9;

        if (l < 0 || l > 1000000000) return ray_error("domain", "deserialize vector: %s length %lld out of range", ray_type_name(type), (long long)l);

        uint8_t esz = ray_type_sizes[type];
        int64_t data_bytes = l * esz;
        if (*len < data_bytes) return ray_error("domain", "deserialize vector: truncated %s data, need %lld bytes, have %lld", ray_type_name(type), (long long)data_bytes, (long long)*len);

        ray_t* vec = ray_vec_from_raw(type, buf, l);
        if (!vec || RAY_IS_ERR(vec)) return vec;
        buf += data_bytes;
        *len -= data_bytes;

        if (attrs & RAY_ATTR_HAS_NULLS) vec->attrs |= RAY_ATTR_HAS_NULLS;
        return vec;
    }

    case RAY_SYM: {
        if (*len < 9) return ray_error("domain", "deserialize sym vector: truncated header, need 9 bytes (attr+len)");
        uint8_t attrs = buf[0];
        buf++;
        memcpy(&l, buf, 8);
        buf += 8;
        *len -= 9;

        if (l < 0 || l > 1000000000) return ray_error("domain", "deserialize sym vector: length %lld out of range", (long long)l);
        /* Every element occupies at least one wire byte (its NUL), so a
         * declared count larger than the bytes left cannot be real — reject
         * it BEFORE allocating so a tiny frame can't force a huge alloc. */
        if (l > *len) return ray_error("domain", "deserialize sym vector: length %lld exceeds %lld remaining bytes", (long long)l, (long long)*len);

        /* Decode interns each string into the GLOBAL table and builds a
         * W64 runtime-domain vec — correct: a freshly materialized wire
         * object lives in the runtime's id space (sym-domain Phase 2). */
        ray_t* vec = ray_vec_new(RAY_SYM, l);
        if (!vec || RAY_IS_ERR(vec)) return vec;
        vec->len = l;
        int64_t* ids = (int64_t*)ray_data(vec);
        for (int64_t i = 0; i < l; i++) {
            size_t slen = safe_strlen(buf, *len);
            if ((int64_t)slen >= *len) {
                vec->len = i;
                ray_release(vec);
                return ray_error("domain", "deserialize sym vector: unterminated sym at index %lld, no NUL within %lld bytes", (long long)i, (long long)*len);
            }
            ids[i] = ray_sym_intern((const char*)buf, slen);
            buf += slen + 1;
            *len -= (int64_t)slen + 1;
        }

        if (attrs & RAY_ATTR_HAS_NULLS) vec->attrs |= RAY_ATTR_HAS_NULLS;
        return vec;
    }

    case RAY_STR: {
        if (*len < 9) return ray_error("domain", "deserialize str vector: truncated header, need 9 bytes (attr+len)");
        uint8_t attrs = buf[0];
        buf++;
        memcpy(&l, buf, 8);
        buf += 8;
        *len -= 9;

        if (l < 0 || l > 1000000000) return ray_error("domain", "deserialize str vector: length %lld out of range", (long long)l);
        /* Each element carries at least an 8-byte length prefix, so more
         * elements than remaining bytes is impossible — bound the up-front
         * allocation to the actual input size. */
        if (l > *len) return ray_error("domain", "deserialize str vector: length %lld exceeds %lld remaining bytes", (long long)l, (long long)*len);

        /* Build STR vector by appending each string via ray_str_vec_append */
        ray_t* vec = ray_vec_new(RAY_STR, l);
        if (!vec || RAY_IS_ERR(vec)) return vec;
        vec->len = 0;
        for (int64_t i = 0; i < l; i++) {
            if (*len < 8) { ray_release(vec); return ray_error("domain", "deserialize str vector: truncated element length prefix at index %lld, need 8 bytes", (long long)i); }
            int64_t slen; memcpy(&slen, buf, 8);
            buf += 8; *len -= 8;
            if (*len < slen || slen < 0) { ray_release(vec); return ray_error("domain", "deserialize str vector: element length %lld at index %lld out of range for %lld remaining bytes", (long long)slen, (long long)i, (long long)*len); }
            ray_t* nv = ray_str_vec_append(vec, (const char*)buf, (size_t)slen);
            if (!nv || RAY_IS_ERR(nv)) { ray_release(vec); return nv ? nv : ray_error("oom", NULL); }
            vec = nv;
            buf += slen;
            *len -= slen;
        }

        if (attrs & RAY_ATTR_HAS_NULLS) vec->attrs |= RAY_ATTR_HAS_NULLS;
        return vec;
    }

    case RAY_LIST: {
        if (*len < 9) return ray_error("domain", "deserialize list: truncated header, need 9 bytes (attr+len)");
        uint8_t list_attrs = buf[0];
        buf++;
        memcpy(&l, buf, 8);
        buf += 8;
        *len -= 9;

        if (l < 0 || l > 1000000000) return ray_error("domain", "deserialize list: length %lld out of range", (long long)l);
        /* Each element serializes to at least one wire byte (its type tag),
         * so a declared count above the remaining byte count is impossible —
         * reject before allocating l pointers. */
        if (l > *len) return ray_error("domain", "deserialize list: length %lld exceeds %lld remaining bytes", (long long)l, (long long)*len);

        ray_t* list = ray_alloc(l * sizeof(ray_t*));
        if (!list || RAY_IS_ERR(list)) return list;
        list->type = RAY_LIST;
        list->attrs = list_attrs & RAY_SERDE_ATTR_WIRE_MASK;
        list->len = l;
        ray_t** elems = (ray_t**)ray_data(list);

        int64_t saved = *len;
        for (int64_t i = 0; i < l; i++) {
            elems[i] = ray_de_raw(buf + (saved - *len), len);
            /* The SERDE_NULL wire marker now deserializes to RAY_NULL_OBJ
             * directly (de_raw_inner returns the singleton), so the guard
             * below only catches a genuine buffer-underrun NULL.  Either way
             * we substitute the singleton so lists round-trip nulls and
             * downstream code (ray_lang_print, etc.) always sees a valid
             * pointer — e.g. an IPC VERBOSE response is `[captured_str,
             * result]` where `result` is RAY_NULL_OBJ for any (println ...) /
             * (set ...) eval; rejecting NULL would error the whole frame as
             * "domain". */
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
        if (*len < 1) return ray_error("domain", "deserialize table: truncated buffer reading attr byte");
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
        if (cols->type != RAY_LIST ||
            (schema->type != RAY_I64 && schema->type != RAY_SYM)) {
            ray_t* e = ray_error("domain", "deserialize table: expected list columns and i64/sym schema, got cols %s schema %s", ray_type_name(cols->type), ray_type_name(schema->type));
            ray_release(schema);
            ray_release(cols);
            return e;
        }

        /* Schema names and columns must be 1:1.  The build loop below is
         * bounded by min(cols->len, schema->len), so a crafted frame with a
         * shorter schema (or more columns) would otherwise be accepted as a
         * silently truncated table instead of being rejected. */
        if (schema->len != cols->len) {
            ray_t* e = ray_error("domain", "deserialize table: schema/column count mismatch (%lld names, %lld columns)", (long long)schema->len, (long long)cols->len);
            ray_release(schema);
            ray_release(cols);
            return e;
        }

        /* Every column must have the same length (the table's row count).  A
         * frame with ragged columns would leave ray_table_nrows reporting
         * column 0's length while row-wise ops read past the shorter ones. */
        ray_t** cps = (ray_t**)ray_data(cols);
        int64_t nrows = (cols->len > 0 && cps[0]) ? cps[0]->len : 0;
        for (int64_t i = 1; i < cols->len; i++) {
            if (!cps[i] || cps[i]->len != nrows) {
                ray_t* e = ray_error("domain", "deserialize table: ragged columns (column %lld has %lld rows, expected %lld)", (long long)i, (long long)(cps[i] ? cps[i]->len : -1), (long long)nrows);
                ray_release(schema);
                ray_release(cols);
                return e;
            }
        }

        int64_t ncols = cols->len;
        ray_t* tbl = ray_table_new(ncols);
        if (!tbl || RAY_IS_ERR(tbl)) {
            ray_release(schema);
            ray_release(cols);
            return tbl;
        }

        void* name_data = ray_data(schema);
        ray_t** col_ptrs = (ray_t**)ray_data(cols);
        for (int64_t i = 0; i < ncols; i++) {   /* ncols == schema->len (guarded above) */
            int64_t name_id = (schema->type == RAY_I64)
                ? ((int64_t*)name_data)[i]
                : ray_read_sym(name_data, i, RAY_SYM, schema->attrs);
            ray_t* new_tbl = ray_table_add_col(tbl, name_id, col_ptrs[i]);
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
        if (*len < 1) return ray_error("domain", "deserialize dict: truncated buffer reading attr byte");
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

        /* One value per key: a dict probe finds a key index in [0, keys->len)
         * and reads the value at that index, so a crafted dict with more keys
         * than values would index the value block out of bounds (OOB
         * read/release).  vals may be a LIST of column vectors or a flat value
         * vector — either way its length must equal keys->len. */
        if (keys->len != vals->len) {
            ray_t* e = ray_error("domain", "deserialize dict: key/value count mismatch (%lld keys, %lld values)", (long long)keys->len, (long long)vals->len);
            ray_release(keys);
            ray_release(vals);
            return e;
        }

        /* Build dict: alloc with 2 slots */
        ray_t* dict = ray_alloc(2 * sizeof(ray_t*));
        if (!dict || RAY_IS_ERR(dict)) {
            ray_release(keys);
            ray_release(vals);
            return dict;
        }
        dict->type = RAY_DICT;
        dict->attrs = dict_attrs & RAY_SERDE_ATTR_WIRE_MASK;
        dict->len = 2;
        ((ray_t**)ray_data(dict))[0] = keys;
        ((ray_t**)ray_data(dict))[1] = vals;
        return dict;
    }

    case RAY_LAMBDA: {
        if (*len < 1) return ray_error("domain", "deserialize lambda: truncated buffer reading attr byte");
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

        ray_t* closure = NULL;
        if (lam_attrs & RAY_SERDE_LAMBDA_HAS_CLOSURE) {
            closure = ray_de_raw(buf + (saved - *len), len);
            if (!closure || RAY_IS_ERR(closure)) {
                ray_release(params);
                ray_release(body);
                return closure;
            }
            if (closure->type != RAY_DICT) {
                ray_release(params);
                ray_release(body);
                ray_release(closure);
                return ray_error("type", "deserialize lambda: closure must be a dict");
            }
        }

        /* Build lambda: allocate with 8 slots (same as eval.c). */
        ray_t* lambda = ray_alloc(8 * sizeof(ray_t*));
        if (!lambda || RAY_IS_ERR(lambda)) {
            ray_release(params);
            ray_release(body);
            ray_release(closure);
            return lambda;
        }
        lambda->type = RAY_LAMBDA;
        lambda->attrs = 0;
        lambda->len = 0;
        memset(ray_data(lambda), 0, 8 * sizeof(ray_t*));
        ((ray_t**)ray_data(lambda))[0] = params;
        ((ray_t**)ray_data(lambda))[1] = body;
        LAMBDA_CLOSURE(lambda) = closure;
        return lambda;
    }

    case RAY_UNARY:
    case RAY_BINARY:
    case RAY_VARY: {
        /* Deserialize builtin by name: read null-terminated string,
         * look up in the global environment. */
        size_t nlen = safe_strlen(buf, *len);
        if ((int64_t)nlen >= *len) return ray_error("domain", "deserialize builtin: unterminated name, no NUL within %lld bytes", (long long)*len);
        int64_t sym = ray_sym_intern((const char*)buf, nlen);
        ray_t* fn = ray_env_get(sym);
        if (!fn) return ray_error("name", "deserialize builtin: '%s' not in global environment", (const char*)buf);
        *len -= (int64_t)nlen + 1;
        ray_retain(fn);
        return fn;
    }

    case RAY_ERROR: {
        if (*len < 8) return ray_error("domain", "deserialize error: truncated error code, need 8 bytes");
        /* A well-formed writer NUL-terminates the 8-byte code field (byte
         * 7 == 0), but a corrupt or hostile frame may fill all 8 bytes.
         * Copy into a bounded, always-terminated buffer so ray_error's
         * strlen cannot read past the field. */
        char code[9];
        memcpy(code, buf, 8);
        code[8] = '\0';
        ray_t* err = ray_error(code, NULL);
        *len -= 8;
        return err;
    }

    default:
        return ray_error("type", "deserialize: unknown wire type %lld", (long long)type);
    }
}

/* --------------------------------------------------------------------------
 * ray_ser — top-level: serialize with IPC header
 * -------------------------------------------------------------------------- */

ray_t* ray_ser(ray_t* obj) {
    bool owned = false;
    if (ray_is_lazy(obj)) {
        ray_retain(obj);
        obj = ray_lazy_materialize(obj); /* consumes the retain */
        if (RAY_IS_ERR(obj)) return obj;
        owned = true;
    }

    int64_t payload = ray_serde_size(obj);
    if (payload <= 0) {
        ray_t* e = ray_error("domain", payload < 0
            ? "serialize: payload size overflow"
            : "serialize: zero serialized size for %s", ray_type_name(obj->type));
        if (owned) ray_release(obj);
        return e;
    }

    int64_t total = (int64_t)sizeof(ray_ipc_header_t) + payload;
    ray_t* buf = ray_vec_new(RAY_U8, total);
    if (!buf || RAY_IS_ERR(buf)) {
        if (owned) ray_release(obj);
        return buf;
    }
    buf->len = total;

    ray_ipc_header_t* hdr = (ray_ipc_header_t*)ray_data(buf);
    hdr->prefix  = RAY_SERDE_PREFIX;
    hdr->version = RAY_SERDE_WIRE_VERSION;
    hdr->flags   = 0;
    hdr->endian  = RAY_SERDE_ENDIAN;
    hdr->msgtype = 0;
    hdr->size    = payload;

    int64_t written = ray_ser_raw((uint8_t*)ray_data(buf) + sizeof(ray_ipc_header_t), obj);
    if (written == 0) {
        ray_t* e = ray_error("domain", "serialize: ray_ser_raw wrote 0 bytes for %s", ray_type_name(obj->type));
        ray_release(buf);
        if (owned) ray_release(obj);
        return e;
    }

    if (owned) ray_release(obj);
    return buf;
}

/* --------------------------------------------------------------------------
 * ray_de — top-level: deserialize from U8 vector
 * -------------------------------------------------------------------------- */

ray_t* ray_de(ray_t* bytes) {
    if (!bytes || RAY_IS_ERR(bytes)) return ray_error("type", "deserialize: input must be a u8 byte buffer, got %s", bytes ? ray_type_name(bytes->type) : "null");
    if (bytes->type != RAY_U8 && bytes->type != -RAY_U8)
        return ray_error("type", "deserialize: input must be a u8 byte buffer, got %s", ray_type_name(bytes->type));

    int64_t total = bytes->len;
    uint8_t* buf = (uint8_t*)ray_data(bytes);

    if (total < (int64_t)sizeof(ray_ipc_header_t))
        return ray_error("domain", "deserialize: buffer too small for ipc header, got %lld bytes", (long long)total);

    ray_ipc_header_t* hdr = (ray_ipc_header_t*)buf;
    if (hdr->prefix != RAY_SERDE_PREFIX)
        return ray_error("domain", "deserialize: bad ipc header magic prefix");
    if (hdr->version != RAY_SERDE_WIRE_VERSION)
        return ray_error("version", "serde wire version mismatch");
    if (hdr->endian != RAY_SERDE_ENDIAN)
        return ray_error("domain", "deserialize: byte-order mismatch (frame endian %d, host %d)",
                         (int)hdr->endian, (int)RAY_SERDE_ENDIAN);
    if (hdr->size < 0 || hdr->size > 1000000000)
        return ray_error("domain", "deserialize: ipc header payload size %lld out of range", (long long)hdr->size);
    if (hdr->size + (int64_t)sizeof(ray_ipc_header_t) != total)
        return ray_error("domain", "deserialize: ipc header size %lld + header != buffer length %lld", (long long)hdr->size, (long long)total);

    int64_t len = hdr->size;
    return ray_de_raw(buf + sizeof(ray_ipc_header_t), &len);
}

/* --------------------------------------------------------------------------
 * File I/O: save/load any object in binary format
 * -------------------------------------------------------------------------- */

ray_err_t ray_obj_save(ray_t* obj, const char* path) {
    bool owned = false;
    if (ray_is_lazy(obj)) {
        ray_retain(obj);
        obj = ray_lazy_materialize(obj); /* consumes the retain */
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
