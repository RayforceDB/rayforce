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
    bool     mm_partial;   /* a chunk with non-null rows carries no min / max: the extremes are incomplete */
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
    ray_enc_hdr_t   hdr;            /* the header's copy */
    int64_t         n_chunks;
    int64_t         null_count;     /* sum of the directory null counts */
    struct ray_sym_domain_s* dom;   /* SYM: the domain ids resolve over (retained unless runtime) */
    bool            dom_owned;
    bool            synthetic;      /* an in-memory stand-in (a ragged partition's absent
                                     * column), not data saved encoded */
    char            path[1024];
    uint64_t        vdict_off, vdict_len;  /* VALUE_DICT entry (file offset, bytes); 0 when absent */
    ray_enc_once_t  dict_once;      /* VALUE_DICT, read once (Task 7) */
    ray_t*          dict;           /* VALUE_DICT values as RAY_I64 bit patterns */
    /* STR side entries (P6): file offset and bytes, 0 when absent */
    uint64_t        sdict_off, sdict_len;  /* STR_DICT */
    uint64_t        fsst_off, fsst_len;    /* FSST_TABLE */
    ray_enc_once_t  sdict_once;     /* STR_DICT, read once */
    ray_enc_once_t  fsst_once;      /* FSST_TABLE, read once */
    ray_t*          sdict;          /* STR_DICT: a RAY_U8 blob holding a ray_enc_sdict_t */
    ray_t*          fsst;           /* FSST_TABLE: a RAY_U8 blob holding a ray_fsst_table_t */
    /* persisted DICT (P6b): the entry tier and the code tier, once each */
    ray_enc_once_t  idict_once;
    ray_t*          idict;          /* RAY_U8 blob: enc_idict_blob_t (the ray_enc_idict_t), then
                                     * the entry table (ray_enc_sdict_t) */
    ray_enc_once_t  icodes_once;
    ray_t*          icodes;         /* RAY_I32: a heap copy of the codes */
    /* parted (Task 12) */
    ray_t*          wrapper;        /* RAY_PARTED_BASE + type; segments are RAY_ENCCOL or plain */
    /* materialization (Task 8) */
    ray_enc_once_t  once;
    ray_t*          decoded;
    /* the statement-boundary trim (ray_enccol_cache_trim): columns whose
     * whole-column decode is published, linked while it is */
    struct ray_enccol_s* dec_prev;
    struct ray_enccol_s* dec_next;
    _Atomic(bool)   dec_listed;
    _Atomic(uint64_t) dec_used;     /* the statement that last asked for the decode */
    _Atomic(uint64_t) decoded_bytes;  /* bytes produced by this column's vector decodes */
    /* single file: ray_enccol_dir_totals' cache; totals_state 0 none,
     * 1 being stored, 2 stored (acquire before reading totals) */
    _Atomic(uint8_t) totals_state;
    ray_enc_col_totals_t totals;
} ray_enccol_t;

/* A STR_DICT side entry (heap blob; offs / bytes point into the
 * immutable mapping).  Entries are pairwise distinct; "" (the STR null, in
 * band) is an ordinary entry, null_code its code (UINT32_MAX when no entry
 * is empty).  slot[] is an open-addressing table over ray_hash_bytes of the
 * entry bytes: code + 1, 0 empty. */
typedef struct {
    uint32_t        n;
    uint32_t        null_code;
    const uint64_t* offs;       /* n + 1 */
    const uint8_t*  bytes;
    uint64_t        pooled;     /* sum of entry lengths over 12 */
    uint64_t        mask;       /* hash slots - 1 */
    uint32_t        slot[];     /* mask + 1 */
} ray_enc_sdict_t;
static inline void ray_enc_sdict_entry(const ray_enc_sdict_t* d, uint32_t k, const char** p, uint32_t* len) {
    *p = (const char*)d->bytes + d->offs[k]; *len = (uint32_t)(d->offs[k + 1] - d->offs[k]);
}

ray_enccol_t* ray_enccol_of(ray_t* v);
ray_t*  ray_enccol_open(void* mapped, size_t size, const char* path,
                        struct ray_sym_domain_s* dom, bool require_dom);
ray_t*  ray_enccol_wrap(ray_enccol_t* e);
void    ray_enccol_payload_retain(ray_t* v);
void    ray_enccol_payload_release(ray_t* v);
void    ray_enccol_payload_detach(ray_t* v);
size_t  ray_enccol_own_bytes(ray_t* v);
int     ray_enccol_children(ray_t* v, ray_t* out[2]);

/* Chunk c's 64 descriptors, in place in the mapping (valid while the
 * column is).  The file is ours: they are read as written. */
const ray_enc_desc_t* ray_enccol_descs(const ray_enccol_t* e, int64_t c);
/* Vector j of chunk c read in place: the mapped payload of a RAW vector
 * with no nulls and no trailer (F32, F64 and the integer types, SYM
 * included), whose bits equal its decode.  NULL: decode it instead. */
const void* ray_enccol_raw_view(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d);
/* Decode vector j of chunk c into `out` (vlen elements, generation-0
 * element width).  STR: pooled bytes are copied to pool_out[pool_base ...]
 * (sized by ray_enccol_desc_pool_bytes) and descriptors get pool_off =
 * pool_base + offset within the vector; hash32 is left 0.  The decoded
 * bytes are returned in *produced instead of counted; the caller counts
 * them with ray_enccol_note_decoded.  NULL or an owned error (oom of a
 * side entry, the FP environment). */
ray_t* ray_enccol_decode_into(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              void* out, char* pool_out, uint64_t pool_base, uint64_t* produced);
/* Vector j of chunk c straight into an expression register: RAY_I64 or
 * RAY_F64 elements (`wide`), nulls as NULL_I64 / NULL_F64, for a FOR_BP,
 * DELTA_BP, CONST or RLE vector of a narrow integer column (BOOL, U8, I16,
 * I32, DATE, TIME).  1 decoded (*produced as ray_enccol_decode_into), 0 declined: the
 * caller reads the vector by range. */
int    ray_enccol_decode_wide(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                              int8_t wide, void* out, uint64_t* produced);
/* Row `row` of a single-file integer column (not SYM), read from its packed
 * vector without decoding the vector: a FOR_BP vector, DICT over a FOR_BP
 * code child, CONST or RLE (the row's run found by binary search, no row
 * expanded), with no nulls.  1 served (*out: the value widened to int64,
 * as the decode would hold it), 0 declined (read the vector by range), -1
 * an owned error in *err (the dictionary's oom). */
int    ray_enccol_point_i64(ray_enccol_t* e, int64_t row, int64_t* out, ray_t** err);
/* ray_enccol_point_i64 of rows rows[q] - base, q in [0, n): ascending
 * rows of one vector, its body read once (CONST / RLE: the runs parsed
 * once, not once per row).  1: every row served into out; 0: declined (as
 * ray_enccol_point_i64; above open decision D2's limit for the codec,
 * RAY_ENC_VEC / 32 rows of a FOR_BP vector, RAY_ENC_VEC / 16 of an RLE,
 * CONST or DICT one; a RAW vector without nulls, which a view reads; a
 * vector with nulls; SYM; any other codec; or rows outside one vector or
 * not ascending), out partly written; -1: an owned error in *err. */
int    ray_enccol_points_i64(ray_enccol_t* e, const int64_t* rows, int64_t n, int64_t base, int64_t* out, ray_t** err);
/* ray_enccol_points_i64 for a gather, which takes nulls and SYM ids: also
 * serves a vector with a null bitmap (nul[q] 1 for a row in it, out[q]
 * then 0, and nothing else of the row read) and a SYM column's ids (id 0,
 * the in-band null, as itself).  nul[q] is 0 for every other row.  The
 * codecs and their per-vector row limits are ray_enccol_points_i64's:
 * FOR_BP, DICT over FOR_BP codes, CONST or RLE, and RAW with a bitmap.
 * 1 / 0 / -1 as there. */
int    ray_enccol_gather_points(ray_enccol_t* e, const int64_t* rows, int64_t n, int64_t base, int64_t* out,
                                uint8_t* nul, ray_t** err);

/* RAY_ENC_VEC lane values at the kind's width T (the fast kernels' own
 * buffer: enccol.c's fk_vals_t is a typedef of this). */
typedef union {
    uint8_t  b[RAY_ENC_VEC]; uint16_t h[RAY_ENC_VEC];
    uint32_t s[RAY_ENC_VEC]; uint64_t q[RAY_ENC_VEC];
} ray_enc_lanes_t;

/* A parsed top-level vector body; never expanded to values.
 * About 21 KB: a scan task keeps one on its stack (never the heap).
 * Floats (CONST, RLE, DICT): k is { 32 | 64, unsigned, float }; cbits, the
 * run values and the dictionary entries are F32 / F64 bit patterns. */
typedef struct ray_enc_vview_s {
    uint8_t         enc;        /* RAY_ENC_CONST, _FOR_BP, _RLE, _DICT or _STR_DICT (a view exists only for these) */
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
    uint32_t        codes[RAY_ENC_VEC];   /* DICT, STR_DICT */
    const uint64_t* dict;       /* DICT: VALUE_DICT entries (T-bit patterns, strictly ascending in dictionary order) */
    uint32_t        dict_n;
} ray_enc_vview_t;

/* Column-global vector `vec` of a single-file column as a view.  1: a
 * view of a CONST, RLE, FOR_BP or DICT (over a U32 FOR_BP code child)
 * integer body, a CONST, RLE or DICT float body (no trailer), or a STR
 * STR_DICT vector as its codes (k U32, type RAY_STR, dict NULL, dict_n the
 * entry count, nullmap NULL: the null is the empty entry's code, in band).
 * A FOR_BP body's base is the vector minimum (the writer's), so its
 * residuals compare in logical order.  0: declined (RAW, DELTA_BP, another
 * code child, a float trailer, STR other than STR_DICT): read the vector
 * by range; -1: an owned error in *err (a side entry's oom).  Never counts
 * decoded bytes.  A declined view may be partly written; the caller
 * ignores it. */
int  ray_enccol_vview(ray_enccol_t* e, int64_t vec, ray_enc_vview_t* out, ray_t** err);

/* The FOR_BP residuals of a view (no base added) at width T into out
 * (n elements of T bits), through the lane kernels (fk_forT, base 0).
 * FOR_BP views only (any other view has packed NULL); out is aligned to
 * T / 8 bytes and holds n elements. */
void ray_enccol_unpack_resid(const ray_enc_vview_t* v, void* out);
/* The residuals of a FOR_BP view comparing true (op: OP_EQ .. OP_GE, at
 * the lane width, unsigned) against t, unpacked and compared in one pass
 * of the lane kernels without storing them (perf fix 6's count). */
uint32_t ray_enccol_count_resid(const ray_enc_vview_t* v, uint16_t op, uint64_t t);
/* A reference on the column's payload (the RAY_ENCCOL headers share it). */
void   ray_enccol_ref(ray_enccol_t* e);
void   ray_enccol_unref(ray_enccol_t* e);
/* Count decoded bytes (global and per column counters). */
void   ray_enccol_note_decoded(ray_enccol_t* e, uint64_t bytes);
/* Decoded-pool bytes of vector j of chunk c (STR; 0 otherwise). */
ray_t* ray_enccol_desc_pool_bytes(ray_enccol_t* e, int64_t c, int j, const ray_enc_desc_t* d,
                                  uint64_t* out);
/* Column-global vector decoded into out (its bytes counted); STR pooled
 * bytes at pool_out[pool_base ...], sized by ray_enccol_vec_pool_bytes. */
ray_t* ray_enccol_decode_vec(ray_enccol_t* e, int64_t vec, void* out, char* pool_out, uint64_t pool_base);
ray_t* ray_enccol_vec_pool_bytes(ray_enccol_t* e, int64_t vec, uint64_t* out);
uint8_t ray_enccol_esz(const ray_enccol_t* e);   /* generation-0 element width */

/* The plain column (borrowed, cached for the object's lifetime) or an
 * owned RAY_ERROR.  Single and (Task 12) parted objects. */
ray_t* ray_enccol_materialize(ray_t* encv);

/* The cached plain column when the materialization has completed, else NULL;
 * never decodes and is safe against a concurrent materialize. */
ray_t* ray_enccol_decoded_peek(ray_t* encv);

/* Whether a whole decode of the column would stay cached from one
 * statement to the next: its generation-0 bytes fit the budget
 * ray_enccol_cache_trim keeps (a quarter of the anonymous watermark; no
 * watermark keeps everything).  A consumer that can read the column by
 * range does so when it would not: such a decode takes the column's whole
 * size at once and is dropped at the statement's end, so every query pays
 * it again.  Metadata only. */
bool   ray_enccol_decode_kept(ray_t* encv);
/* Test hook: a positive budget replaces the quarter of the watermark in
 * ray_enccol_decode_kept until reset to 0. */
void   ray_enccol_test_kept_limit(int64_t bytes);

/* At a statement boundary: drop the whole-column decodes, least recently
 * asked for first, until those still kept fit a quarter of the anonymous
 * watermark.  A decode something else retains is neither counted nor
 * dropped (dropping it would free nothing and the next read would decode a
 * second copy).  Does nothing while an evaluation is on this thread's stack
 * (a request served from inside a statement's sync send) or a dispatch is in
 * flight, and without a watermark. */
void   ray_enccol_cache_trim(void);
/* Bytes of the whole-column decodes currently kept (test hook). */
int64_t ray_enccol_cache_bytes(void);

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
 * or materialization (and a string-domain build's per-segment code remap,
 * strdom.c) fails (1 = the next one); 0 disables.  fail_path:
 * while set, every such allocation fails for a column whose path contains
 * `sub` (NULL disables); both report "oom" (transient). */
void   ray_enccol_test_fail_alloc(int64_t nth);
void   ray_enccol_test_fail_path(const char* sub);
/* The fail_alloc counter, for a materialization step outside this file
 * (the index-region parse): true when this allocation is the nth.  It is
 * the counter only (ray_enc_index_parse takes no path).  fail_path never
 * reaches the region: it fails the column's first materialization
 * allocation ("materialize"), before the region is parsed;
 * the parse is reached by the nth counter (the materializer's own check
 * before the parse, then this one inside it) and by fail_index_parse. */
bool   ray_enccol_test_nth_alloc_fails(void);
/* Test hook: while set, the index parse of every materialization fails
 * with "oom" ("column <path>: index parse"); transient, so not cached. */
void   ray_enccol_test_fail_index_parse(bool on);
/* interrupt_after: the nth chunk decoded successfully from now on calls
 * ray_request_interrupt() (0 disables), exercising the per-chunk poll. */
void   ray_enccol_test_interrupt_after(int64_t nth);
/* Parted materialization: 0 auto, 1 always on the coordinator, 2 always per-segment tasks. */
void   ray_enccol_test_parted_mode(int mode);
/* Encoded column objects alive (opened, not yet freed). */
int64_t  ray_enccol_test_live(void);
/* One top-level integer vector body (`enc`, `width`) of a column of `type`
 * (SYM: `attrs` width class), vlen rows, `bm` the null bitmap or NULL,
 * n_nulls as its descriptor says.  fast: through the fast kernel only,
 * writing `wide` elements (0: the generation-0 width; RAY_I64 / RAY_F64:
 * an expression register); else through the reference path (wide must be
 * 0).  1 decoded, 0 the fast kernel declined, -1 the reference path
 * failed (oom). */
int    ray_enccol_test_decode_body(int8_t type, uint8_t attrs, uint8_t enc,
                                   uint8_t width, int n_nulls, const uint8_t* bm, const uint8_t* body,
                                   uint64_t avail, int vlen, bool fast, int8_t wide, void* out);
/* One top-level DICT vector body of a column of `type` whose VALUE_DICT
 * holds dict[0, n_dict) (T-bit patterns); otherwise as
 * ray_enccol_test_decode_body at the generation-0 width (fast: the
 * dictionary published first, as a column's first decode leaves it). */
int    ray_enccol_test_decode_dict(int8_t type, const uint64_t* dict, uint32_t n_dict,
                                   int n_nulls, const uint8_t* bm, const uint8_t* body, uint64_t avail,
                                   int vlen, bool fast, void* out);
/* One top-level float vector body (`enc`, any float encoding; F32 / F64)
 * of a column with the VALUE_DICT dict[0, n_dict) (NULL: none), n_nulls
 * as its descriptor says, no trailer.  fast: through the fast kernel only
 * (the dictionary published first); else through dec_floats.  1 decoded,
 * 0 the fast kernel declined, -1 the reference path failed. */
int    ray_enccol_test_decode_float(int8_t type, const uint64_t* dict, uint32_t n_dict, uint8_t enc, int n_nulls,
                                    const uint8_t* bm, const uint8_t* body, uint64_t avail, int vlen,
                                    bool fast, void* out);
/* ray_enccol_point_i64's body read of every row [0, vlen) of one top-level
 * body (`enc`, `width`, n_nulls as its descriptor says) of a column of
 * `type` with the VALUE_DICT dict[0, n_dict) (NULL: none): res[i] is the
 * point read's 1 / 0 / -1 and out[i] its value when served.  0, or -1 on
 * OOM. */
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

/* A column's format, for .idx.info: vectors per encoding code (RAW ..
 * FSST), file bytes, generation-0 bytes, whether the plain column exists.
 * Reads descriptors only (statistics are reported, never trusted to size
 * memory) and never decodes a payload. */
typedef struct {
    int64_t  vectors[10];
    uint64_t encoded_bytes;   /* file bytes (end_off) of real encoded files; a plain parted segment
                               * counts its raw bytes, a synthetic stand-in counts 0 */
    uint64_t raw_bytes;       /* materialized payload: len * width (+ the decoded STR pool, which
                               * holds each STR_DICT entry once) */
    bool     materialized;
    bool     synthetic;       /* every encoded segment is a synthetic stand-in */
} ray_enccol_info_t;
/* Generation-0 payload bytes of a plain vector: len * width (+ STR pool). */
uint64_t ray_enccol_plain_bytes(ray_t* v);
/* NULL, or an owned type error (not an encoded column). */
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
/* Chunk c of a single-file encoded column.  c must be below e->n_chunks. */
void ray_enccol_chunk_stat(const ray_enccol_t* e, int64_t c, ray_enc_chunk_stat_t* out);
/* STR code bounds (membership bounds, spec "Statistics") of chunk c (j < 0)
 * or of vector j of chunk c: true with [*lo, *hi] when present.  The only
 * reader of STR statistics as statistics; every generic accessor reports
 * none for STR (chunk_stat with min / max 0, dir_totals, vec_stat), and the
 * ZONE / CHUNK_ZONE synthesis treats STR chunks as without them. */
bool ray_enccol_code_bounds(const ray_enccol_t* e, int64_t c, int j, int64_t* lo, int64_t* hi);
/* The totals below, computed once per column (the directory is immutable). */
void ray_enccol_dir_totals(ray_enccol_t* e, ray_enc_col_totals_t* out);
/* File byte range holding rows [lo, hi): from the first touched chunk's
 * descriptors to the end of the last touched chunk.  Precondition:
 * 0 <= lo < hi <= e->len (a row past the end would read a directory entry
 * past the last chunk). */
void ray_enccol_byte_range(const ray_enccol_t* e, int64_t lo, int64_t hi,
                           uint64_t* off, uint64_t* len);

void ray_enccol_meta(ray_t* encv, ray_col_meta_t* m);
/* Could materialization attach an index: a recorded kind or UNIQUE
 * (single-file)?  Known without decoding. */
bool ray_enccol_may_have_index(ray_t* encv);

/* The column's STR dictionary (*out NULL when it has none, or the column
 * is not STR), or an owned oom error (read once). */
ray_t* ray_enccol_strdict(ray_enccol_t* e, const ray_enc_sdict_t** out);
/* Code of `s` in the column's dictionary: *code >= 0, -1 absent, -2 no
 * dictionary.  NULL, or an owned oom error (*code untouched). */
ray_t* ray_enccol_strdict_code(ray_enccol_t* e, const char* s, size_t len, int64_t* code);

/* A persisted DICT (spec "STR dictionary index"), parsed.  Pointers into
 * the immutable mapping; ent is a STR_DICT-form view of the entries (offs
 * / bytes in the mapping, slots in the blob). */
typedef struct {
    int64_t                len, n_distinct;
    const int32_t*         codes;       /* len: read through ray_enccol_idict_codes */
    const int32_t*         first_occ;   /* n_distinct */
    const ray_enc_sdict_t* ent;         /* ent->n == n_distinct */
} ray_enc_idict_t;
/* The column records a persisted DICT (P6b): a single file, not synthetic,
 * its header naming kind 7.  The one gate of every DICT reader. */
bool ray_enccol_has_idict(const ray_enccol_t* e);
/* The column's persisted DICT, parsed once (*out NULL: the column
 * records none, or is parted / synthetic), or an owned error (once). */
ray_t* ray_enccol_idict(ray_enccol_t* e, const ray_enc_idict_t** out);
/* Code of s: *code >= 0, -1 absent, -2 no persisted DICT. */
ray_t* ray_enccol_idict_code(ray_enccol_t* e, const char* s, size_t len, int64_t* code);
/* Codes of rows [start, start + n) in place (vector scope); no decode, no
 * decoded bytes.  The column must record a DICT (ray_enccol_idict gave
 * one). */
ray_t* ray_enccol_idict_codes(ray_enccol_t* e, int64_t start, int64_t n, const int32_t** out);
/* The whole codes as a heap RAY_I32 vector,
 * made once and kept for the object's lifetime (borrowed: retain to keep). */
ray_t* ray_enccol_idict_codes_vec(ray_enccol_t* e, ray_t** out);
/* Test hook: how many STR pools materialization has allocated so far. */
uint64_t ray_enccol_test_str_pool_allocs(void);
/* A fresh SYM vector `out` (runtime domain) takes the domain the column's ids
 * resolve over, as its materialization would; no decode. */
void ray_enccol_adopt_domain(ray_t* out, ray_t* encv);

#endif /* RAY_ENCCOL_H */
