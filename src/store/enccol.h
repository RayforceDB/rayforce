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

/* Whole-column totals of a single-file column's directory, in one pass
 * (what a loop of ray_enccol_chunk_stat over every chunk would add up):
 * the wrapping sum of the chunk low words, the exact 128-bit sum, the
 * non-null rows, and the extremes over the chunks with min/max (integer
 * types in min_i / max_i from INT64_MAX / INT64_MIN, float types in
 * min_f / max_f from +inf / -inf). */
typedef struct {
    uint64_t sum_wrapped;
    int64_t  sum_hi;
    uint64_t sum_lo;
    int64_t  nonnull;
    int64_t  min_i, max_i;
    double   min_f, max_f;
} ray_enc_col_totals_t;

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
    bool            synthetic;      /* an in-memory stand-in (a ragged partition's absent
                                     * column), not data saved encoded */
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
    /* single file: one byte per chunk, set (release) once the chunk's
     * descriptors passed validation; never set for a chunk that failed */
    _Atomic(uint8_t)* desc_ok;
    /* single-file F32 / F64: one byte per chunk, set (release) once every
     * RAW vector of the chunk without nulls or trailer passed the checks
     * dec_floats makes (no NaN outside the bitmap, zero padding, no bytes
     * after the body); never set for a chunk that failed */
    _Atomic(uint8_t)* fraw_ok;
    /* single file: ray_enccol_dir_totals' cache; totals_state 0 none,
     * 1 being stored, 2 stored (acquire before reading totals) */
    _Atomic(uint8_t) totals_state;
    ray_enc_col_totals_t totals;
    /* single file: set (release) once the published materialization's
     * values were found inside their chunks' statistics
     * (ray_enccol_check_dir_all); never set on a failure */
    _Atomic(uint8_t) mat_dir_ok;
    /* single file: per chunk, bit j set (release) once vector j's values
     * were found inside the chunk's statistics (ray_enccol_check_dir);
     * never set on a failure */
    _Atomic(uint64_t)* dir_vec_ok;
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
 * descriptors are used).  A chunk is validated once per column: later
 * calls trust the mapping, as generation 0 does. */
ray_t* ray_enccol_chunk_descs(ray_enccol_t* e, int64_t c, ray_enc_desc_t out[64]);
/* The same, read in place: *out points at the chunk's 64 descriptors in
 * the mapping (valid while the column is), NULL on an error. */
ray_t* ray_enccol_chunk_descs_ref(ray_enccol_t* e, int64_t c, const ray_enc_desc_t** out);
/* Decode vector j of chunk c (descriptor already validated) into `out`
 * (vlen elements, generation-0 element width).  STR: pooled bytes are
 * copied to pool_out[pool_base ...] and descriptors get pool_off =
 * pool_base + offset within the vector; hash32 is left 0.  `pool_cap` is
 * the size of pool_out.  Returns NULL or an owned error; on an error
 * `out` and `pool_out` are not written. */
ray_t* ray_enccol_decode_desc(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              void* out, char* pool_out, uint64_t pool_base, uint64_t pool_cap);
/* ray_enccol_decode_desc into a private destination: on an error `out`
 * may hold a partial decode (the caller never publishes it), and the
 * decoded bytes are returned in *produced instead of counted; the caller
 * counts them with ray_enccol_note_decoded. */
/* Vector j of chunk c (descriptors validated) read in place: the mapped
 * payload of a RAW F32 / F64 vector with no nulls and no trailer, whose
 * bits equal its decode, once its chunk's float checks passed (validated
 * once per chunk, a failure never cached).  NULL: decode it instead (the
 * decode reports a corrupt vector). */
const void* ray_enccol_raw_float_view(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d);
ray_t* ray_enccol_decode_into(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              void* out, char* pool_out, uint64_t pool_base, uint64_t pool_cap,
                              uint64_t* produced);
/* Vector j of chunk c straight into an expression register: RAY_I64 or
 * RAY_F64 elements (`wide`), nulls as NULL_I64 / NULL_F64, for a FOR_BP or
 * DELTA_BP vector of a narrow integer column (BOOL, U8, I16, I32, DATE,
 * TIME).  1 decoded (*produced as ray_enccol_decode_into), 0 declined: the
 * caller reads the vector by range, which decodes or reports it. */
int    ray_enccol_decode_wide(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              int8_t wide, void* out, uint64_t* produced);
/* Row `row` of a single-file integer column (not SYM), read from its packed
 * vector without decoding the vector: a FOR_BP vector, or DICT over a
 * FOR_BP code child, with no nulls.  The chunk's descriptors are validated
 * (once per chunk); the row's body size, base, reserved fields, code (below
 * the dictionary size), the sentinel and the value against the vector's
 * statistics are checked; the other rows of the vector are not read, so a
 * sampler may see a row of a vector the full decode later reports.  1
 * served (*out: the value widened to int64, as the decode would hold it),
 * 0 declined (read the vector by range: it decodes or reports it), -1 an
 * owned error in *err (descriptors, dictionary, or the value outside its
 * statistics: `corrupt`). */
int    ray_enccol_point_i64(ray_enccol_t* e, int64_t row, int64_t* out, ray_t** err);
/* A reference on the column's payload (the RAY_ENCCOL headers share it). */
void   ray_enccol_ref(ray_enccol_t* e);
void   ray_enccol_unref(ray_enccol_t* e);
/* Count decoded bytes (global and per column counters). */
void   ray_enccol_note_decoded(ray_enccol_t* e, uint64_t bytes);
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

/* The cached plain column when the materialization has completed, else NULL;
 * never decodes and is safe against a concurrent materialize. */
ray_t* ray_enccol_decoded_peek(ray_t* encv);

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
/* Encoded column objects alive (opened, not yet freed). */
int64_t  ray_enccol_test_live(void);
/* Chunk descriptor validations run since process start. */
uint64_t ray_enccol_test_desc_validations(void);
/* One top-level integer vector body (`enc`, `width`) of a column of `type`
 * (SYM: `attrs` width class, ids below `sym_bound`), vlen rows, `bm` the
 * null bitmap or NULL, n_nulls as its descriptor says.  fast: through the
 * fast kernel only, writing `wide` elements (0: the generation-0 width;
 * RAY_I64 / RAY_F64: an expression register); else through the reference
 * path (wide must be 0).  The descriptor's statistics are the non-null
 * range of the body's reference decode, as a writer gives them (absent
 * when that decode fails or every row is null).  1 decoded, 0 the fast
 * kernel declined, -1 the reference path reports it corrupt. */
int    ray_enccol_test_decode_body(int8_t type, uint8_t attrs, uint32_t sym_bound, uint8_t enc,
                                   uint8_t width, int n_nulls, const uint8_t* bm, const uint8_t* body,
                                   uint64_t avail, int vlen, bool fast, int8_t wide, void* out);
/* One top-level DICT vector body of a column of `type` whose VALUE_DICT
 * holds dict[0, n_dict) (T-bit patterns), the entry `trim` bytes short;
 * otherwise as ray_enccol_test_decode_body at the generation-0 width (fast:
 * the dictionary validated first, as a column's first decode leaves it). */
int    ray_enccol_test_decode_dict(int8_t type, const uint64_t* dict, uint32_t n_dict, int trim,
                                   int n_nulls, const uint8_t* bm, const uint8_t* body, uint64_t avail,
                                   int vlen, bool fast, void* out);
/* ray_enccol_point_i64's body read of every row [0, vlen) of one top-level
 * body (`enc`, `width`, n_nulls as its descriptor says; no statistics) of a
 * column of `type`
 * with the VALUE_DICT dict[0, n_dict) (NULL: none): res[i] is the point
 * read's 1 / 0 / -1 and out[i] its value when served.  0, or -1 on OOM. */
int    ray_enccol_test_point_body(int8_t type, const uint64_t* dict, uint32_t n_dict, uint8_t enc,
                                  uint8_t width, int n_nulls, const uint8_t* body, uint64_t avail,
                                  int vlen, int64_t* out, int8_t* res);

/* Wrap a plain-type parted wrapper (RAY_PARTED_BASE + base) whose segments
 * include at least one RAY_ENCCOL; retains the wrapper. */
ray_t* ray_enccol_parted_new(ray_t* wrapper);
/* Segment i: its RAY_ENCCOL or plain vector (borrowed; NULL for a missing
 * segment, which ray_read_parted never builds: a ragged partition's absent
 * column is a lazy all-null RAY_ENCCOL segment, or for LIST a heap fill). */
ray_t* ray_enccol_seg(ray_t* encv, int64_t i);
/* Segment i materialized (borrowed plain vector, NULL for a missing one)
 * or an owned error. */
ray_t* ray_enccol_seg_col(ray_t* encv, int64_t i);
bool   ray_enccol_is_parted(ray_t* encv);
/* A synthetic column stands in for data that was never saved encoded (a
 * ragged partition's absent column, ray_read_parted): generation
 * inheritance treats it as plain.  Parted: every encoded segment is
 * synthetic. */
void   ray_enccol_set_synthetic(ray_t* encv);
bool   ray_enccol_is_synthetic(ray_t* encv);

/* One chunk's directory statistics. */
typedef struct ray_enc_chunk_stat_s {
    int64_t  rows;           /* rows in the chunk */
    int64_t  min, max;       /* logical_T values (float types: the double's bits); valid when has_minmax */
    bool     has_minmax;     /* directory flag bit 0 clear */
    uint32_t null_count;
    uint64_t sum_lo;         /* exact 128-bit sum (BOOL, U8, I16, I32, I64, TIME: ray_encw_type_summable) */
    int64_t  sum_hi;
} ray_enc_chunk_stat_t;
/* Chunk c of a single-file encoded column; the directory was verified at
 * load, so this cannot fail.  c must be below e->n_chunks. */
void ray_enccol_chunk_stat(const ray_enccol_t* e, int64_t c, ray_enc_chunk_stat_t* out);
/* The totals below, computed once per column (the directory is immutable). */
void ray_enccol_dir_totals(ray_enccol_t* e, ray_enc_col_totals_t* out);
/* File byte range holding rows [lo, hi): from the first touched chunk's
 * descriptors to the end of the last touched chunk.  Precondition:
 * 0 <= lo < hi <= e->len (a row past the end would read a directory entry
 * past the last chunk). */
void ray_enccol_byte_range(const ray_enccol_t* e, int64_t lo, int64_t hi,
                           uint64_t* off, uint64_t* len);

/* The consumer guard (ray_col_src_t.stats_guard): the non-null values of
 * vector j of chunk c, decoded at data (vlen elements at the generation-0
 * width, nulls in band; may_null: the vector has nulls), lie inside the
 * chunk's directory statistics.  A consumer that sized memory from those
 * statistics reads only values that fit it.  Integer types other than SYM
 * (whose ids decode checks); a chunk without statistics passes.  A vector
 * that passed is recorded and not scanned again (a failure is not).  NULL,
 * or `corrupt` naming the column. */
ray_t* ray_enccol_check_dir(ray_enccol_t* e, int64_t c, int j, const void* data, int vlen, bool may_null);
/* Vector j of chunk c already passed ray_enccol_check_dir. */
bool   ray_enccol_dir_vec_ok(const ray_enccol_t* e, int64_t c, int j);
/* The same over every chunk of the column's published materialization,
 * once per column (a success is cached; the mapping is immutable).  NULL
 * when nothing is published. */
ray_t* ray_enccol_check_dir_all(ray_t* encv);

void ray_enccol_meta(ray_t* encv, ray_col_meta_t* m);
/* Could materialization attach an index (recorded kind, UNIQUE, or for STR
 * a runtime DICT, possible only when every chunk has statistics)? */
bool ray_enccol_may_have_index(ray_t* encv);
/* A fresh SYM vector `out` (runtime domain) takes the domain the column's ids
 * resolve over, as its materialization would; no decode. */
void ray_enccol_adopt_domain(ray_t* out, ray_t* encv);

#endif /* RAY_ENCCOL_H */
