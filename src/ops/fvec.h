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

#ifndef RAY_FVEC_H
#define RAY_FVEC_H

#include "ops.h"

/* Factorization state -- pipeline concept, NOT added to ray_t.
 *
 * Lives in the pipeline context. ray_t itself remains unchanged.
 */
typedef struct ray_fvec {
    ray_t*    vec;            /* underlying ray_t vector (I64, SYM, etc.) */
    int64_t  cur_idx;        /* >= 0: flat (single value at index)      */
                             /* -1: unflat (full vector is active)      */
    int64_t  cardinality;    /* for flat: how many rows this represents */
} ray_fvec_t;

/* Factorized Table -- accumulation buffer for ASP-Join */
typedef struct ray_ftable {
    ray_fvec_t*  columns;     /* array of factorized vectors   */
    uint16_t    n_cols;
    int64_t     n_tuples;    /* factorized tuple count        */
    ray_t*       semijoin;    /* RAY_SEL bitmap of qualifying keys */
} ray_ftable_t;

ray_ftable_t* ray_ftable_new(uint16_t n_cols);
void         ray_ftable_free(ray_ftable_t* ft);
ray_t*        ray_ftable_materialize(ray_ftable_t* ft);

#endif /* RAY_FVEC_H */
