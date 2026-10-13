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

#ifndef RAY_OPS_STRDOM_H
#define RAY_OPS_STRDOM_H

/* A query-wide string domain: the distinct strings of a STR column, interned
 * while it is read by range, each with a dense id.  A group-by keys a
 * generation-2 STR column on the ids (an I64 key column built for the query)
 * and emits the groups' strings from the domain, so the column is never
 * materialized.  Ids are assigned in first-occurrence order over the rows:
 * a route emitting groups in first-occurrence order or in id order emits
 * them in the order generation 0's STR key gives.  "" is the STR null (its
 * canonical empty payload): null and "" rows share one id. */

#include <rayforce.h>
#include "store/colrange.h"

typedef struct ray_strdom_s ray_strdom_t;
/* Intern every value of a STR source into a new query-wide domain, writing
 * each row's id (nulls: the id of the empty string) into an I64 vector.
 * Reads the source by range; never materializes it.  The domain grows only
 * from the bytes read (its arena is bounded by UINT32_MAX: `range` past it);
 * the id vector is sized by the source's row count.  NULL with *dom_out and
 * *ids_out owned, or an owned error with both NULL. */
ray_t* ray_strdom_build(ray_col_src_t* src, ray_strdom_t** dom_out, ray_t** ids_out);
/* The strings of ids[0..n) as a new STR vector (the group keys' output); an
 * id outside the domain (none a group emits) reads as "".  NULL with *out
 * owned, or an owned error with *out NULL. */
ray_t* ray_strdom_emit(const ray_strdom_t* d, const int64_t* ids, int64_t n, ray_t** out);
int64_t ray_strdom_count(const ray_strdom_t* d);
void    ray_strdom_free(ray_strdom_t* d);

#endif /* RAY_OPS_STRDOM_H */
