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
#include "store/enccol.h"   /* ray_enc_chunk_stat_t */
#include "ops/ops.h"   /* RAY_DISPATCH_MORSELS, RAY_MORSEL_ELEMS: the pool's task grain */

struct ray_sym_domain_s;

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
 * ray_alloc_raw.  Serves one source in one scan task. */
typedef struct {
    uint8_t* fixed;
    char*    pool;
    size_t   pool_cap;
} ray_col_scratch_t;

/* NULL or an owned error (ray_error_free). */
ray_t* ray_col_src_bind(ray_t* col, ray_col_src_t* out);
void   ray_col_src_release(ray_col_src_t* s);
void   ray_col_scratch_init(ray_col_scratch_t* s);
void   ray_col_scratch_free(ray_col_scratch_t* s);

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

/* Bind a table column without decoding; an absent column leaves out->obj
 * NULL and returns NULL. */
ray_t* ray_table_col_src(ray_t* tbl, int64_t name_id, ray_col_src_t* out);

/* The bound source's plain column (borrowed; valid while the source is
 * bound): the object itself for generation 0, the cached materialization
 * for generation 2.  NULL or an owned error. */
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
 * published materialization; NULL when reading it would decode.  Borrowed
 * from the source; never decodes. */
ray_t*  ray_col_src_ready(const ray_col_src_t* s);
/* The base pointer (row 0) a scan reads the source at in place: the ready
 * plain vector's data; NULL when it must be read by range (not ready, or a
 * parted wrapper, which has no single base).  Never decodes. */
const void* ray_col_src_base(const ray_col_src_t* s);
/* Generation-0 element width of the source (SYM: its persisted width). */
uint8_t ray_col_src_esz(const ray_col_src_t* s);

/* Chunk c's statistics when the source is a single-file generation-2
 * column; false otherwise (plain, parted, slice). */
bool ray_col_src_chunk_stat(const ray_col_src_t* s, int64_t c, ray_enc_chunk_stat_t* out);
/* One sequential read-ahead hint for rows [lo, hi): plain mmapped
 * columns hint their element bytes, generation 2 the chunk bytes from the
 * directory, parted sources each touched segment; heap vectors nothing. */
void ray_col_src_advise(const ray_col_src_t* s, int64_t lo, int64_t hi);

/* One source in one task: P1's scratch plus the current view. */
typedef struct {
    ray_col_src_t*    src;     /* borrowed; bound for the execution */
    ray_col_scratch_t sc;
    ray_col_range_t   v;       /* valid when have */
    bool              have;
} ray_col_cursor_t;

void   ray_col_cursor_init(ray_col_cursor_t* c, ray_col_src_t* src);
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
