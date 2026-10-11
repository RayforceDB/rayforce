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
    /* single file, floats and integers (not GUID or STR): one byte per
     * chunk, set (release) once every RAW vector of the chunk without
     * nulls or trailer passed the checks its decode makes (floats: no NaN
     * outside the bitmap; integers: no sentinel, SYM ids in the domain and
     * the statistics; zero padding, no bytes after the body); never set
     * for a chunk that failed */
    _Atomic(uint8_t)* raw_ok;
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
 * payload of a RAW vector with no nulls and no trailer (F32, F64 and the
 * integer types, SYM included), whose bits equal its decode, once its
 * chunk's RAW checks passed (validated once per chunk, a failure never
 * cached).  NULL: decode it instead (the decode reports a corrupt vector).
 * A consumer guard (stats_guard) still checks the values it reads. */
const void* ray_enccol_raw_view(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d);
ray_t* ray_enccol_decode_into(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              void* out, char* pool_out, uint64_t pool_base, uint64_t pool_cap,
                              uint64_t* produced);
/* Vector j of chunk c straight into an expression register: RAY_I64 or
 * RAY_F64 elements (`wide`), nulls as NULL_I64 / NULL_F64, for a FOR_BP,
 * DELTA_BP, CONST or RLE vector of a narrow integer column (BOOL, U8, I16,
 * I32, DATE, TIME).  1 decoded (*produced as ray_enccol_decode_into), 0 declined: the
 * caller reads the vector by range, which decodes or reports it. */
int    ray_enccol_decode_wide(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              int8_t wide, void* out, uint64_t* produced);
/* Row `row` of a single-file integer column (not SYM), read from its packed
 * vector without decoding the vector: a FOR_BP vector, DICT over a FOR_BP
 * code child, CONST or RLE, with no nulls.  The chunk's descriptors are
 * validated (once per chunk); the row's body size, base, reserved fields,
 * code (below the dictionary size), the runs (every check of the run
 * kernel: headers, children, sizes, padding, ends ascending to the vector
 * length; the row's run found by binary search, no row expanded), the
 * sentinel and the value against the vector's statistics are checked; the
 * other rows of the vector are not read, so a
 * sampler may see a row of a vector the full decode later reports.  1
 * served (*out: the value widened to int64, as the decode would hold it),
 * 0 declined (read the vector by range: it decodes or reports it), -1 an
 * owned error in *err (descriptors, dictionary, or the value outside its
 * statistics: `corrupt`). */
int    ray_enccol_point_i64(ray_enccol_t* e, int64_t row, int64_t* out, ray_t** err);

/* RAY_ENC_VEC lane values at the kind's width T (the fast kernels' own
 * buffer: enccol.c's fk_vals_t is a typedef of this). */
typedef union {
    uint8_t  b[RAY_ENC_VEC]; uint16_t h[RAY_ENC_VEC];
    uint32_t s[RAY_ENC_VEC]; uint64_t q[RAY_ENC_VEC];
} ray_enc_lanes_t;

/* A parsed, validated top-level vector body; never expanded to values.
 * About 21 KB: a scan task keeps one on its stack (never the heap).
 * Floats (CONST only): k.T is the bit width (32 / 64), unsigned, and cbits
 * the value's F32 / F64 bit pattern. */
typedef struct ray_enc_vview_s {
    uint8_t         enc;        /* RAY_ENC_CONST, _FOR_BP, _RLE or _DICT (a view exists only for these) */
    int             n, n_nulls;
    const uint8_t*  nullmap;    /* the 128-byte bitmap (bitmap types with nulls), else NULL */
    ray_enc_kind_t  k;
    int8_t          type;
    uint64_t        cbits;      /* CONST: the value's T-bit pattern */
    uint64_t        base;       /* FOR_BP: the T-bit base */
    int             width;      /* FOR_BP */
    const uint8_t*  packed;     /* FOR_BP: the packed lanes in the mapping */
    uint32_t        n_runs;     /* RLE (CONST: 1) */
    ray_enc_lanes_t run_vals;   /* RLE / CONST: run values at width T */
    ray_enc_lanes_t run_ends;   /* RLE / CONST: .h[r] exclusive, strictly ascending, last == n */
    uint32_t        codes[RAY_ENC_VEC];   /* DICT */
    const uint64_t* dict;       /* DICT: VALUE_DICT entries (T-bit patterns, strictly ascending) */
    uint32_t        dict_n;
} ray_enc_vview_t;

/* Column-global vector `vec` of a single-file column as a view.  1: a
 * view, after every check the vector's decode makes (the payload split
 * and bitmap; body sizes, headers and padding; run ends; the sentinel
 * outside the bitmap; SYM ids against the bound, the statistics and the
 * null count; DICT codes below the dictionary size with their values
 * inside the statistics; float CONST: no NaN outside the bitmap, the value
 * inside the statistics), and a FOR_BP body whose residuals do not wrap
 * (base + each residual at most the kind's largest value, checked on the
 * actual residuals: a writer's base is the minimum, so a sound vector
 * passes; its residuals then compare in logical order); 0: declined (RAW, DELTA_BP,
 * floats other than CONST, a trailer, a DICT code child other than FOR_BP,
 * a body that could wrap, or anything decode would reject): read the
 * vector by range, which decodes or reports it; -1: an owned error in
 * *err (the descriptors', the payload split's or the VALUE_DICT's
 * validation, as the decode would report it).  Never counts decoded
 * bytes.  A declined view may be partly written; the caller ignores it. */
int  ray_enccol_vview(ray_enccol_t* e, int64_t vec, ray_enc_vview_t* out, ray_t** err);

/* The FOR_BP residuals of a view (no base added) at width T into out
 * (n elements of T bits), through the lane kernels (fk_forT, base 0).
 * FOR_BP views only (any other view has packed NULL); out is aligned to
 * T / 8 bytes and holds n elements. */
void ray_enccol_unpack_resid(const ray_enc_vview_t* v, void* out);
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

/* Counters (spec "Instrumentation"); reset by tests.  decoded_bytes is
 * the bytes vector decodes produce, plus the bytes an integer RAW vector
 * read in place delivers (ray_enccol_raw_view through a range read); a
 * float RAW view delivers its bytes uncounted. */
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
/* The same with the descriptor's statistics [mn, mx] when `stats`, so the
 * point read's value check runs: a value outside them is res[i] -1. */
int    ray_enccol_test_point_body_stats(int8_t type, const uint64_t* dict, uint32_t n_dict, uint8_t enc,
                                        uint8_t width, int n_nulls, const uint8_t* body, uint64_t avail,
                                        int vlen, bool stats, int64_t mn, int64_t mx, int64_t* out, int8_t* res);

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

/* A column's format, for .idx.info: vectors per encoding code (RAW ..
 * FSST), file bytes, generation-0 bytes, whether the plain column exists.
 * Reads descriptors only (statistics are reported, never trusted to size
 * memory) and never decodes a payload. */
typedef struct {
    int64_t  vectors[10];
    uint64_t encoded_bytes;   /* file bytes (end_off) of real encoded files; a plain parted segment
                               * counts its raw bytes, a synthetic stand-in counts 0 */
    uint64_t raw_bytes;       /* generation-0 payload: len * width (+ STR pool) */
    bool     materialized;
    bool     synthetic;       /* every encoded segment is a synthetic stand-in */
} ray_enccol_info_t;
/* Generation-0 payload bytes of a plain vector: len * width (+ STR pool). */
uint64_t ray_enccol_plain_bytes(ray_t* v);
/* NULL, or an owned error from a chunk's descriptor validation. */
ray_t* ray_enccol_info(ray_t* encv, ray_enccol_info_t* out);

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
 * once per column (a success is cached; the mapping is immutable); a
 * parted column checks each encoded segment against its own chunks.  NULL
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
