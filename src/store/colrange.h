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

#ifndef RAY_COLRANGE_H
#define RAY_COLRANGE_H

#include <rayforce.h>
#include <stdatomic.h>
#include "store/enc_once.h"
#include "ops/ops.h"   /* RAY_DISPATCH_MORSELS, RAY_MORSEL_ELEMS: the pool's task grain */

struct ray_sym_domain_s;
struct ray_enc_chunk_stat_s;   /* store/enccol.h */
struct ray_enc_vview_s;        /* store/enccol.h */

/* A bound column source: the object a scan reads ranges of.  Binding
 * retains the object (and a non-runtime SYM domain) and never decodes.
 * The struct owns seg_off: never copy it by value (bind it in place, share
 * it by pointer, release it once). */
typedef struct {
    ray_t*  obj;                    /* retained: plain vector, slice, RAY_ENCCOL, or plain parted wrapper */
    struct ray_sym_domain_s* dom;   /* SYM: the domain ids resolve over */
    bool    dom_owned;              /* dom was retained */
    int8_t  type;                   /* logical element type (parted: base) */
    uint8_t attrs;                  /* persisted attrs; parted: not authoritative, see ray_col_range */
    int64_t len;                    /* rows (parted: sum over segments) */
    bool    may_null;               /* HAS_NULLS, or directory null counts for generation 2 */
    bool    parted;                 /* a parted source (per-range attrs are authoritative) */
    bool    stats_guard;            /* set by a consumer that sized memory from this source's
                                     * directory statistics (before its pass reads it): every
                                     * range read checks its values against their chunk's
                                     * statistics (ray_enccol_check_dir), a widened read
                                     * declines until its vector passed; `corrupt` on a
                                     * value outside them */
    _Atomic(int64_t*) seg_off;      /* parted: segment row offsets (n_segs + 1), built on the
                                     * first range read, published once, owned (a by-value
                                     * copy would free it twice), freed on release */
} ray_col_src_t;

/* A view of rows [start, start + n): elements in the generation-0
 * representation, nulls in-band. */
typedef struct {
    const void* data;
    bool        may_null;
    int8_t      type;
    uint8_t     attrs;
    int64_t     start, n;
    const char* str_pool;           /* STR: base of pooled ray_str_t offsets */
    struct ray_sym_domain_s* dom;   /* SYM: the domain the ids resolve over */
} ray_col_range_t;

/* Per-source scratch for generation-2 decodes: one fixed vector (1024
 * elements of the widest width, 16 KB) and a growable STR pool, both from
 * ray_alloc_raw.  Serves one source in one scan task.  The bytes it decodes
 * are counted (ray_enc_counters_get, the column's decoded_bytes) when the
 * scratch moves to another column or is freed, not per vector; the scratch
 * holds a reference on that column meanwhile, so it may outlive its source.
 * A RAW vector read in place (ray_enccol_raw_view) decodes nothing: an
 * integer one (SYM included) still counts the bytes it delivers, so a key's
 * read-once counts hold whatever its vectors' encodings; a float one
 * counts none. */
typedef struct {
    uint8_t* fixed;
    bool     fixed_lent;            /* fixed is the caller's (ray_col_scratch_lend): never freed here */
    char*    pool;
    size_t   pool_cap;
    struct ray_enccol_s* acc_e;     /* the column acc_bytes belong to (a reference) */
    uint64_t acc_bytes;             /* decoded, not yet counted */
    uint64_t wide_hits;             /* ray_col_range_wide decodes, not yet counted */
} ray_col_scratch_t;

/* The fixed vector a scan task lends its scratch from its own stack, so
 * a worker's first decode allocates nothing: a fresh worker heap maps and
 * faults in a whole pool on its first allocation, a fixed cost per worker
 * that generation 0, reading its columns in place, never pays.  One per
 * scratch, alive until the scratch is freed. */
#define RAY_COL_FIXED_BYTES ((size_t)1024 * 16)   /* one vector of the widest element (GUID) */
typedef struct { _Alignas(64) uint8_t b[RAY_COL_FIXED_BYTES]; } ray_col_fixed_t;

/* NULL or an owned error (ray_error_free). */
ray_t* ray_col_src_bind(ray_t* col, ray_col_src_t* out);
void   ray_col_src_release(ray_col_src_t* s);
void   ray_col_scratch_init(ray_col_scratch_t* s);
void   ray_col_scratch_free(ray_col_scratch_t* s);
/* An initialised scratch decodes into buf (the caller's, outliving the
 * scratch's use) instead of a fixed vector from the heap. */
void   ray_col_scratch_lend(ray_col_scratch_t* s, ray_col_fixed_t* buf);
/* Test hook: fixed vectors range reads took from the heap since process
 * start (a lent scratch takes none). */
uint64_t ray_col_test_fixed_allocs(void);

/* Range of [start, start + n), clipped at the source end, at a parted
 * segment or slice boundary and (generation 2) at the next vector boundary;
 * `out->n` says how many rows were returned (0 only at the source end or
 * for n == 0).  Generation 0 is zero copy; generation 2 decodes the vector
 * into `scratch`.  The view is valid until the next call with the same
 * scratch or until the source is released.  A loaded parted source has no
 * missing segment (ray_read_parted gives a ragged partition's absent
 * column a segment of nulls, #724); a hand-built wrapper's missing segment holds no rows,
 * matching ray_parted_nrows, and the executor refuses a table whose parted
 * columns' row counts differ, so a scan never reads one.
 * For a parted source the SYM element width and SORTED are per range only
 * (segments may differ): read them from the view's `attrs`, never from the
 * source's. */
ray_t* ray_col_range(ray_col_src_t* src, int64_t start, int64_t n,
                     ray_col_scratch_t* scratch, ray_col_range_t* out);

/* Rows [start, start + n) decoded straight into dst as `wide` (RAY_I64 /
 * RAY_F64) elements, nulls as NULL_I64 / NULL_F64 (an expression
 * register), when they are exactly one whole vector of a single-file
 * generation-2 source that ray_enccol_decode_wide takes: *got = n.  Else
 * *got = 0 and nothing is decoded: read the rows with ray_col_range.  NULL
 * or an owned error. */
ray_t* ray_col_range_wide(ray_col_src_t* src, int64_t start, int64_t n, ray_col_scratch_t* scratch,
                          int8_t wide, void* dst, int64_t* got);

/* The vector of a generation-2 source (single file, or a parted source's
 * encoded segment, neither with a published materialization) that starts
 * at row `start`, decoded straight into dst at the generation-0 width,
 * when it ends inside [start, start + n) and is not STR or GUID (SYM: of
 * the width in `attrs`): *got = its rows (a RAW vector: with the RAW
 * vectors after it inside the range, copied at once).  Else *got = 0 and nothing is
 * read: read the rows with ray_col_range or a cursor.  A guarded source
 * (stats_guard) always declines: its range reads check each vector.  dst
 * may hold a partial vector on an error.  NULL or an owned error. */
ray_t* ray_col_range_into(ray_col_src_t* src, int64_t start, int64_t n, ray_col_scratch_t* sc,
                          uint8_t attrs, void* dst, int64_t* got);
/* Test hook: ray_col_range_into's deliveries and the rows they held
 * since process start. */
void ray_col_test_into(uint64_t* calls, uint64_t* rows);

/* Test hook: vectors ray_col_range_wide decoded since process start
 * (counted when their scratch is freed). */
uint64_t ray_col_test_wide_hits(void);

/* Bind a table column without decoding; an absent column leaves out->obj
 * NULL and returns NULL. */
ray_t* ray_table_col_src(ray_t* tbl, int64_t name_id, ray_col_src_t* out);

/* The bound source's plain column (borrowed; valid while the source is
 * bound): the object itself for generation 0, the cached materialization
 * for generation 2, checked against its chunks' statistics under
 * stats_guard (ray_enccol_check_dir_all: `corrupt` naming the column).
 * NULL or an owned error. */
ray_t*  ray_col_src_plain(ray_col_src_t* s, ray_t** out);
/* Literal translation into the source's SYM domain, without decoding:
 * the position of the bytes, -1 when absent. */
int64_t ray_col_src_sym_find(const ray_col_src_t* s, const char* str, size_t len);
/* STR dictionary code of a constant: >= 0 the code, -1 provably absent,
 * -2 no dictionary to resolve against (generation 2 in P1: STR_DICT
 * arrives in P6).  Reads only the generation-0 column's DICT index. */
int64_t ray_col_src_dict_code(const ray_col_src_t* s, const char* str, size_t len);

/* ---- Scan side: cursors, the piece walk, the failure envelope ---- */

/* The contiguous-mode block bound: the pool's task grain (8192 rows). */
#define RAY_COL_BLOCK_ROWS (RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS)

/* The source's object is a RAY_ENCCOL (generation 2, or a parted column
 * with an encoded or synthetic segment). */
static inline bool ray_col_src_encoded(const ray_col_src_t* s) {
    return s && s->obj && s->obj->type == RAY_ENCCOL;
}

/* The plain column a scan can read in place, without decoding: a plain
 * object (vector, slice, plain parted wrapper) itself, or a RAY_ENCCOL's
 * published materialization; NULL when reading it would decode.  Under
 * stats_guard a materialization is ready only once its values lie inside
 * their chunks' statistics (ray_enccol_check_dir_all); otherwise NULL, and
 * the range reads report `corrupt`.  Borrowed from the source; never
 * decodes. */
ray_t*  ray_col_src_ready(const ray_col_src_t* s);
/* The base pointer (row 0) a scan reads the source at in place: the ready
 * plain vector's data; NULL when it must be read by range (not ready, or a
 * parted wrapper, which has no single base).  Never decodes. */
const void* ray_col_src_base(const ray_col_src_t* s);
/* Generation-0 element width of the source (SYM: its persisted width). */
uint8_t ray_col_src_esz(const ray_col_src_t* s);

/* Chunk c's statistics when the source is a single-file generation-2
 * column; false otherwise (plain, parted, slice). */
bool ray_col_src_chunk_stat(const ray_col_src_t* s, int64_t c, struct ray_enc_chunk_stat_s* out);
/* Vector v's statistics (rows [v * RAY_ENC_VEC, + RAY_ENC_VEC), the
 * vector's descriptor, validated with its chunk once per column and nested
 * inside the chunk's statistics) when the source is a single-file
 * generation-2 column and the vector carries them: min / max are
 * logical_T values as ray_enc_chunk_stat_t's, n_nulls the vector's null
 * count.  False otherwise: not such a source, v outside it, statistics
 * absent (every row null, or a type without them), or descriptors that
 * fail validation (the read of the vector then reports them).  For
 * answer-only decisions (skip or fill a vector); nothing may be sized from
 * them without stats_guard. */
bool ray_col_src_vec_stat(const ray_col_src_t* s, int64_t v, int64_t* min, int64_t* max, uint32_t* n_nulls);
/* Vector v of a single-file generation-2 source (not synthetic, not
 * materialized) as a view: ray_enccol_vview's 1 / 0 / -1.  0 for any
 * other source (read it by range; a published materialization is read in
 * place). */
int  ray_col_src_vview(const ray_col_src_t* s, int64_t v, struct ray_enc_vview_s* out, ray_t** err);
/* The statistics of a parted source, chunk by chunk in row order: fn is
 * called for rows [lo, hi) of every RAY_ENC_CHUNK-row chunk of every
 * segment, with the chunk's directory statistics when the segment is a
 * single-file generation-2 column, else NULL (a plain segment).  False
 * when the source is not parted, or with an owned *err (OOM).  A consumer
 * that sizes memory from these statistics sets stats_guard: the range
 * reads then check every segment's values against its own chunks'. */
typedef void (*ray_col_chunk_fn)(void* ctx, int64_t lo, int64_t hi, const struct ray_enc_chunk_stat_s* st);
bool ray_col_src_parted_chunks(ray_col_src_t* s, void* ctx, ray_col_chunk_fn fn, ray_t** err);
/* The segments of a parted source: *n of them in row order (borrowed
 * RAY_ENCCOL or plain vectors, NULL for a missing one), segment i holding
 * rows [off[i], off[i + 1]).  False when the source is not parted, or with
 * an owned *err (OOM).  Publishes the segment offsets on first use. */
bool ray_col_src_parted_segs(ray_col_src_t* s, int64_t* n, ray_t*** segs, const int64_t** off, ray_t** err);
/* Row `row` of a single-file or parted generation-2 integer source (not
 * SYM) read without decoding its vector (ray_enccol_point_i64: FOR_BP,
 * DICT over FOR_BP, CONST or RLE, without nulls; a parted row is its
 * segment's): 1
 * served into *out (the value widened to int64), 0 declined (read the row
 * through a cursor, which decodes or reports the vector; always for a
 * plain or materialized source or segment), -1 an owned error in *err.
 * For samplers: a sampled row costs no vector decode.  A parted source
 * publishes its segment offsets on first use (so the source is not const). */
int  ray_col_src_point_i64(ray_col_src_t* s, int64_t row, int64_t* out, ray_t** err);
/* ray_col_src_point_i64 of the ascending rows[0, n), all inside one
 * vector (ray_col_src_vec_span), the vector's body read once
 * (ray_enccol_points_i64): 1 every row served into out, 0 declined (read
 * the vector through a cursor; out partly written), -1 an owned *err. */
int  ray_col_src_points_i64(ray_col_src_t* s, const int64_t* rows, int64_t n, int64_t* out, ray_t** err);
/* The rows [*lo, *hi) of the vector holding row `row` (segment-local
 * vectors for a parted source) when ray_col_src_point_i64 may serve it (a
 * single-file encoded column or segment, not materialized): true.  False
 * for any other row, which a cursor reads.  For a caller choosing between
 * point reads and one decode by how many of a vector's rows it needs. */
bool ray_col_src_vec_span(ray_col_src_t* s, int64_t row, int64_t* lo, int64_t* hi);
/* The vector holding row `row` as a run view: a single-file generation-2
 * integer or SYM source, or a parted source's single-file encoded segment
 * (segment-local vectors), neither materialized, whose descriptor says
 * CONST or RLE (no other codec builds a view) and which ray_enccol_vview
 * vouches for, without a bitmap of nulls (SYM nulls are in-band ids, run
 * values like the others).  Under stats_guard the run values, which are
 * the vector's values, are checked as a range read checks them: their
 * [min, max] inside the chunk's directory statistics
 * (ray_enccol_check_dir_range), `corrupt` naming the column otherwise.
 * *lo / *hi are set in every case to rows of the source holding row and
 * read alike: the vector, or a plain, materialized or stand-in segment
 * (the source) whole.  1: the view of the vector [*lo, *hi); 0 declined
 * (read the rows through a cursor, which decodes or reports the vector);
 * -1 an owned *err (the descriptors', the payload's or the guard's). */
int  ray_col_src_runview(ray_col_src_t* s, int64_t row, struct ray_enc_vview_s* out, int64_t* lo, int64_t* hi, ray_t** err);
/* Open decision D2 (ruled per codec at the measured crossovers of a
 * batched point read against one decode): a vector's requested rows are
 * point-read when there are at most RAY_ENC_VEC / 32 of them in a FOR_BP
 * vector, RAY_ENC_VEC / 16 in an RLE or CONST one; any other codec
 * declines (ray_enccol_points_i64 decides).  RAY_COL_POINT_ROWS is the
 * largest of them, the most rows a caller hands one call (RAY_ENC_VEC =
 * RAY_MORSEL_ELEMS values).  Structural ratios, not knobs. */
#define RAY_COL_POINT_ROWS (RAY_MORSEL_ELEMS / 16)
/* One sequential read-ahead hint for rows [lo, hi): plain mmapped
 * columns hint their element bytes, generation 2 the chunk bytes from the
 * directory, parted sources each touched segment; heap vectors nothing. */
void ray_col_src_advise(const ray_col_src_t* s, int64_t lo, int64_t hi);
/* Test hook: the spans ray_col_src_advise would hint (page-aligned start,
 * bytes), the first `cap` stored; returns how many there are. */
int  ray_col_src_test_advise_spans(const ray_col_src_t* s, int64_t lo, int64_t hi,
                                   uintptr_t* start, size_t* len, int cap);

/* One source in one task: P1's scratch plus the current view. */
typedef struct {
    ray_col_src_t*    src;     /* borrowed; bound for the execution */
    ray_col_scratch_t sc;
    ray_col_range_t   v;       /* valid when have */
    bool              have;
} ray_col_cursor_t;

void   ray_col_cursor_init(ray_col_cursor_t* c, ray_col_src_t* src);
/* ray_col_cursor_init with its scratch lent buf (ray_col_scratch_lend). */
void   ray_col_cursor_init_lent(ray_col_cursor_t* c, ray_col_src_t* src, ray_col_fixed_t* buf);
void   ray_col_cursor_free(ray_col_cursor_t* c);
/* The view covering row r (0 <= r < len): the current one when it covers
 * r, else a range read of [r, len) (generation 2: up to the end of r's
 * vector; plain: to the source, segment or slice end).  A RAY_ENCCOL whose
 * materialization is published is read from that vector.  NULL or an
 * owned error; *out is valid until the next call or the cursor's free. */
ray_t* ray_col_cursor_at(ray_col_cursor_t* c, int64_t r, const ray_col_range_t** out);

/* One source's view of a piece: data points at the piece's first row. */
typedef struct {
    const void* data;
    uint8_t     attrs;          /* SYM: the width class of data */
    bool        may_null;
    const char* str_pool;       /* STR */
    struct ray_sym_domain_s* dom;
} ray_col_piece_t;

typedef void (*ray_col_piece_fn)(void* ctx, uint32_t wid, int64_t b, int64_t e,
                                 const ray_col_piece_t* p);

/* Walk [lo, hi) of n sources.  plain[i] is the base pointer of source i
 * (its row 0) when it can be read in place, or NULL to read it through a
 * cursor (generation 2, RAY_ENCCOL_PARTED, a plain parted wrapper); a NULL
 * srcs[i] is skipped (COUNT inputs).  Each call of fn covers a piece
 * [b, e) with one ray_col_piece_t per source.  Plain sources point at
 * plain + b * esz; cursor sources point into their view, so a piece ends
 * at the nearest view end.  contiguous: one piece [lo, hi)
 * (hi - lo <= RAY_COL_BLOCK_ROWS), every cursor F32/F64 source copied into
 * a buffer of the call (any other cursor type is `nyi` in this mode).
 * Cursors and buffers live for the call: allocate at its start, free at its
 * end.  NULL or an owned error (fn is not called again after an error). */
ray_t* ray_col_walk(ray_col_src_t* const* srcs, const void* const* plain, uint32_t n,
                    int64_t lo, int64_t hi, bool contiguous,
                    void* ctx, uint32_t wid, ray_col_piece_fn fn);

/* ray_col_walk's non-contiguous mode through the caller's cursors: cur[i]
 * is source i's cursor (ray_col_cursor_init'd for every srcs[i] with a NULL
 * plain[i]; other entries unused), kept by the caller across calls.  A task
 * walking consecutive blocks with one cursor set decodes the vector one
 * block ends inside once, not once per block.  NULL or an owned error. */
ray_t* ray_col_walk_cur(ray_col_src_t* const* srcs, const void* const* plain, uint32_t n,
                        ray_col_cursor_t* cur, int64_t lo, int64_t hi,
                        void* ctx, uint32_t wid, ray_col_piece_fn fn);

/* A dispatch's failure envelope (R-FAIL): a worker notes a range error
 * keyed by its task's first row and stops reading.  The answer is the
 * lowest-keyed failure, independent of scheduling, so a task may return
 * early only when ray_scan_fail_skip says a LOWER key already failed (a
 * task below the lowest failure still runs: it may fail lower).  After the
 * join, every coordinator calls ray_scan_fail_finish, which also turns a
 * cancelled dispatch (tasks the pool skipped note nothing) into `cancel`. */
typedef struct {
    _Atomic(uint32_t) lock;
    _Atomic(int64_t)  key;     /* lowest failing key; INT64_MAX when none */
    ray_enc_snap_t    snap;
} ray_scan_fail_t;

void   ray_scan_fail_init(ray_scan_fail_t* f);
/* Consumes err; snapshots its code and this thread's message when key is
 * the lowest so far. */
void   ray_scan_fail_note(ray_scan_fail_t* f, int64_t key, ray_t* err);
static inline bool ray_scan_failed(ray_scan_fail_t* f) {
    return atomic_load_explicit(&f->key, memory_order_acquire) != INT64_MAX;
}
/* True only when a failure with a key lower than `key` is already noted:
 * the task of `key` cannot change the answer and may skip its work. */
static inline bool ray_scan_fail_skip(ray_scan_fail_t* f, int64_t key) {
    return atomic_load_explicit(&f->key, memory_order_acquire) < key;
}
/* NULL, or a fresh error rebuilt on the calling thread. */
ray_t* ray_scan_fail_error(ray_scan_fail_t* f);
/* The coordinator's verdict after the join: the lowest noted failure; else
 * `cancel` when the dispatch was cancelled (ray_interrupted(), or `pool`'s
 * cancel flag; pool may be NULL); else NULL.  A noted failure stands over a
 * cancel: it is a real fault of the data, a cancel only an absence of work. */
ray_t* ray_scan_fail_finish(ray_scan_fail_t* f, ray_pool_t* pool);

#endif /* RAY_COLRANGE_H */
