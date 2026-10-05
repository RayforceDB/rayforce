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
#ifndef RAY_ENCCOL_H
#define RAY_ENCCOL_H

#include <stdatomic.h>
#include <rayforce.h>
#include "mem/heap.h"          /* ray_file_map_t */
#include "ops/ops.h"           /* RAY_ENCCOL */
#include "store/encfmt.h"
#include "store/enc_once.h"

struct ray_sym_domain_s;

#define RAY_ENCCOL_SINGLE 1
#define RAY_ENCCOL_PARTED 2

typedef struct ray_enccol_s {
    _Atomic(uint32_t) rc;           /* shared by RAY_ENCCOL header copies */
    uint8_t  kind;                  /* RAY_ENCCOL_SINGLE / RAY_ENCCOL_PARTED */
    int8_t   type;                  /* logical element type (parted: base type) */
    uint8_t  attrs;                 /* persisted attrs (masked) */
    int64_t  len;                   /* logical rows (parted: sum over segments) */
    /* single file */
    ray_file_map_t* map;            /* one reference to the mapping */
    const uint8_t*  base;
    uint64_t        size;
    ray_enc_hdr_t   hdr;            /* validated copy */
    int64_t         n_chunks;
    int64_t         null_count;     /* sum of the directory null counts */
    uint32_t        sym_bound;      /* SYM: every id is below this */
    struct ray_sym_domain_s* dom;   /* SYM: the domain ids resolve over (retained unless runtime) */
    bool            dom_owned;
    char            path[1024];
    uint64_t        vdict_off, vdict_len;  /* VALUE_DICT entry (file offset, bytes); 0 when absent */
    ray_enc_once_t  dict_once;      /* VALUE_DICT validation (Task 7) */
    ray_t*          dict;           /* validated VALUE_DICT values as RAY_I64 bit patterns */
    /* parted (Task 12) */
    ray_t*          wrapper;        /* RAY_PARTED_BASE + type; segments are RAY_ENCCOL or plain */
    /* materialization (Task 8) */
    ray_enc_once_t  once;
    ray_t*          decoded;
    _Atomic(uint64_t) decoded_bytes;  /* bytes produced by this column's vector decodes */
} ray_enccol_t;

ray_enccol_t* ray_enccol_of(ray_t* v);
ray_t*  ray_enccol_open(void* mapped, size_t size, const char* path,
                        struct ray_sym_domain_s* dom, bool require_dom);
ray_t*  ray_enccol_wrap(ray_enccol_t* e);
void    ray_enccol_payload_retain(ray_t* v);
void    ray_enccol_payload_release(ray_t* v);
void    ray_enccol_payload_detach(ray_t* v);
size_t  ray_enccol_own_bytes(ray_t* v);
int     ray_enccol_children(ray_t* v, ray_t* out[2]);

#endif /* RAY_ENCCOL_H */
