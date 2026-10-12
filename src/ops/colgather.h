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

#ifndef RAY_COLGATHER_H
#define RAY_COLGATHER_H

/*
 * Gathers from a bound column source (generation 0 or 2, single file or
 * parted) under the spec's gather output contract: the result is a new
 * plain vector that owns everything it references.  A STR result holds one
 * pool built for it (each pooled value is copied out of the read's view
 * before the next read reuses the scratch); a SYM result retains the
 * source's domain at the widest contributing width class, ids widened as
 * logical values; HAS_NULLS and null payloads are left as generation 0's
 * gather leaves them (the type's canonical sentinel).  A LIST source is
 * `nyi` (lists are never encoded; callers keep their LIST code).  On any
 * error the partial result, its pool and its domain are released and
 * nothing is published.
 */

#include "store/colrange.h"

/* Rows of `src` kept by rowsel `sel` (NULL: every row), in row order, as a
 * new plain vector.  Only vectors whose segment is not RAY_SEL_NONE are
 * read.  A large selection is gathered by pool tasks over consecutive
 * segments; a failure is the lowest-row one (the scan-fail envelope), a
 * cancelled dispatch `cancel`.  NULL with *out set, or an owned error. */
ray_t* ray_col_gather_sel(ray_col_src_t* src, ray_t* sel, ray_t** out);
/* The operators' entry for one table slot.  ray_col_gather_slot: the
 * RAY_ENCCOL a row gather reads by range (slot from ray_table_get_col_enc /
 * _idx), or NULL for none or a slice of an encoded column, which a source
 * does not bind: the caller keeps its plain gather.  ray_col_gather_enc:
 * that slot bound, gathered under `sel` (by_rows false) or at ids[0..n)
 * (by_rows true), released.  NULL with *out set, or an owned error. */
ray_t* ray_col_gather_slot(ray_t* enc);
ray_t* ray_col_gather_enc(ray_t* enc, ray_t* sel, bool by_rows, const int64_t* ids, int64_t n, ray_t** out);
/* Test hook: selection gathers run as several pool tasks since process
 * start (a selection of at least RAY_PARALLEL_THRESHOLD rows with a
 * pool: each task gathers its segments into the output range the
 * selection's counts give it, STR pools appended in row order). */
uint64_t ray_col_test_gather_par(void);
/* Rows ids[0..n) of `src`, in that order, repeats included, as a new plain
 * vector.  Ids are bucketed by vector; each touched vector is range-read
 * once and its values scattered to their output positions. */
ray_t* ray_col_gather_rows(ray_col_src_t* src, const int64_t* ids, int64_t n, ray_t** out);

#endif /* RAY_COLGATHER_H */
