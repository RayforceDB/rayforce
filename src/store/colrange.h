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

struct ray_sym_domain_s;

/* A bound column source: the object a scan reads ranges of.  Binding
 * retains the object (and a non-runtime SYM domain) and never decodes. */
typedef struct {
    ray_t*  obj;                    /* retained: plain vector, slice, RAY_ENCCOL, or plain parted wrapper */
    struct ray_sym_domain_s* dom;   /* SYM: the domain ids resolve over */
    bool    dom_owned;              /* dom was retained */
    int8_t  type;                   /* logical element type (parted: base) */
    uint8_t attrs;                  /* persisted attrs */
    int64_t len;                    /* rows (parted: sum over segments) */
    bool    may_null;               /* HAS_NULLS, or directory null counts for generation 2 */
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
 * scratch or until the source is released.  A missing ragged segment of a
 * parted source holds no rows, as in a generation-0 parted flatten. */
ray_t* ray_col_range(ray_col_src_t* src, int64_t start, int64_t n,
                     ray_col_scratch_t* scratch, ray_col_range_t* out);

/* Bind a table column without decoding; an absent column leaves out->obj
 * NULL and returns NULL. */
ray_t* ray_table_col_src(ray_t* tbl, int64_t name_id, ray_col_src_t* out);

#endif /* RAY_COLRANGE_H */
