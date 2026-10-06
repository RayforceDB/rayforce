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
#include "table/table.h"      /* ray_col_meta_t */

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

/* Chunk c's descriptors, validated (spec "Validation": before a chunk's
 * descriptors are used). */
ray_t* ray_enccol_chunk_descs(ray_enccol_t* e, int64_t c, ray_enc_desc_t out[64]);
/* Decode vector j of chunk c (descriptor already validated) into `out`
 * (vlen elements, generation-0 element width).  STR: pooled bytes are
 * copied to pool_out[pool_base ...] and descriptors get pool_off =
 * pool_base + offset within the vector; hash32 is left 0.  `pool_cap` is
 * the size of pool_out.  Returns NULL or an owned error; on an error
 * `out` and `pool_out` are not written. */
ray_t* ray_enccol_decode_desc(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              void* out, char* pool_out, uint64_t pool_base, uint64_t pool_cap);
/* Decoded-pool bytes of vector j of chunk c (STR; 0 otherwise). */
ray_t* ray_enccol_desc_pool_bytes(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                                  uint64_t* out);
/* Column-global vector: validates chunk descriptors, then decodes. */
ray_t* ray_enccol_decode_vec(ray_enccol_t* e, int64_t vec, void* out, char* pool_out,
                             uint64_t pool_base, uint64_t pool_cap);
ray_t* ray_enccol_vec_pool_bytes(ray_enccol_t* e, int64_t vec, uint64_t* out);
int    ray_enccol_vec_nulls(ray_enccol_t* e, int64_t vec, int* n_nulls);   /* 0 ok, -1 corrupt */
uint8_t ray_enccol_esz(const ray_enccol_t* e);   /* generation-0 element width */

/* The plain column (borrowed, cached for the object's lifetime) or an
 * owned RAY_ERROR.  Single and (Task 12) parted objects. */
ray_t* ray_enccol_materialize(ray_t* encv);

/* Counters (spec "Instrumentation"); reset by tests. */
typedef struct {
    uint64_t materialized;
    uint64_t decoded_bytes;
    uint64_t at_first_exec;   /* materialized when the first ray_execute since the last reset started; UINT64_MAX until then */
} ray_enc_counters_t;
void   ray_enc_note_exec_start(void);
void   ray_enc_counters_get(ray_enc_counters_t* out);
void   ray_enc_counters_reset(void);

/* Test hooks.  fail_alloc: the nth allocation inside generation-2 decode
 * or materialization fails (1 = the next one); 0 disables.  fail_path:
 * while set, every such allocation fails for a column whose path contains
 * `sub` (NULL disables); both report "oom" (transient). */
void   ray_enccol_test_fail_alloc(int64_t nth);
void   ray_enccol_test_fail_path(const char* sub);
/* interrupt_after: the nth chunk decoded successfully from now on calls
 * ray_request_interrupt() (0 disables), exercising the per-chunk poll. */
void   ray_enccol_test_interrupt_after(int64_t nth);
/* Parted materialization: 0 auto, 1 always on the coordinator, 2 always per-segment tasks. */
void   ray_enccol_test_parted_mode(int mode);
void   ray_enccol_test_refuse_attach(int on);        /* the index attach sees a mismatched root */

/* Wrap a plain-type parted wrapper (RAY_PARTED_BASE + base) whose segments
 * include at least one RAY_ENCCOL; retains the wrapper. */
ray_t* ray_enccol_parted_new(ray_t* wrapper);
/* Segment i: its RAY_ENCCOL or plain vector (borrowed; NULL for a missing
 * segment of a ragged partition). */
ray_t* ray_enccol_seg(ray_t* encv, int64_t i);
/* Segment i materialized (borrowed plain vector, NULL for a missing one)
 * or an owned error. */
ray_t* ray_enccol_seg_col(ray_t* encv, int64_t i);
bool   ray_enccol_is_parted(ray_t* encv);

void ray_enccol_meta(ray_t* encv, ray_col_meta_t* m);
/* Could materialization attach an index (recorded kind, UNIQUE, or for STR
 * a runtime DICT, possible only when every chunk has statistics)? */
bool ray_enccol_may_have_index(ray_t* encv);

#endif /* RAY_ENCCOL_H */
