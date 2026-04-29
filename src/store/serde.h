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

#ifndef RAY_SERDE_H
#define RAY_SERDE_H

#include <rayforce.h>

/* Wire format prefix */
#define RAY_SERDE_PREFIX 0xcefadefa

/* Wire format version.  Bumped whenever the on-the-wire layout of any
 * serialized value changes (e.g. a new field is added to the atom
 * record) so a peer running older code detects the mismatch and
 * rejects the payload instead of silently mis-parsing.  Decoupled from
 * RAY_VERSION_MAJOR on purpose: API version and wire version evolve
 * independently.
 *
 *   Version 2 — atoms: type(1) + value-bytes.
 *   Version 3 — atoms: type(1) + flags(1) + value-bytes.  `flags` bit 0
 *               carries the typed-null marker so (de (ser 0Nl)) round-
 *               trips (previously decoded as ray_i64(0) and dropped the
 *               null bit). */
#define RAY_SERDE_WIRE_VERSION 3

/* Wire-only null marker (not a valid ray_t type) */
#define RAY_SERDE_NULL 126

typedef struct ray_ipc_header_t {
    uint32_t prefix;     /* RAY_SERDE_PREFIX */
    uint8_t  version;    /* RAY_VERSION_MAJOR */
    uint8_t  flags;      /* 0 */
    uint8_t  endian;     /* 0 = little */
    uint8_t  msgtype;    /* 0 = async, 1 = sync, 2 = response */
    int64_t  size;       /* payload size in bytes */
} ray_ipc_header_t;

_Static_assert(sizeof(ray_ipc_header_t) == 16, "ipc header must be 16 bytes");

/* Calculate serialized size of an object (excluding IPC header) */
int64_t ray_serde_size(ray_t* obj);

/* Serialize object into buffer. Returns bytes written, 0 on error.
 * Buffer must have at least ray_serde_size(obj) bytes. */
int64_t ray_ser_raw(uint8_t* buf, ray_t* obj);

/* Deserialize object from buffer. Returns reconstructed ray_t*.
 * *len is updated to reflect bytes consumed. */
ray_t*  ray_de_raw(uint8_t* buf, int64_t* len);

/* Top-level: serialize to U8 vector with IPC header */
ray_t*  ray_ser(ray_t* obj);

/* Top-level: deserialize from U8 vector (validates IPC header) */
ray_t*  ray_de(ray_t* bytes);

/* File I/O: save/load any object in binary format */
ray_err_t ray_obj_save(ray_t* obj, const char* path);
ray_t*    ray_obj_load(const char* path);

#endif /* RAY_SERDE_H */
