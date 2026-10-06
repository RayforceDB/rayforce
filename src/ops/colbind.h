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

#ifndef RAY_COLBIND_H
#define RAY_COLBIND_H

#include "ops/ops.h"
#include "store/colrange.h"

/* Bind column `sym` of `tbl` for this execution (deduplicated per table
 * and name; no decode).  NULL with *err NULL when absent; NULL with an
 * owned *err on failure (OOM).  Binds are made on the thread executing the
 * graph, before any pool dispatch that reads them; the returned source is
 * borrowed from the graph and valid until ray_graph_free.  `tbl` must
 * outlive the graph: the dedup key is its raw pointer. */
ray_col_src_t* ray_graph_bind_col(ray_graph_t* g, ray_t* tbl, int64_t sym, ray_t** err);
void           ray_graph_binds_free(ray_graph_t* g);
uint32_t       ray_graph_binds_count(const ray_graph_t* g);   /* tests */
void           ray_graph_bind_test_fail_alloc(int64_t nth);   /* tests: fail the nth bind (0 = off) */

/* True when the column is present but its shape is not a bindable source
 * (a partition key column, a slice of a parted or encoded column): a
 * compile site declines these as a shape, the way it declined them before
 * the bind stage existed, instead of treating the bind's error as a failure. */
bool ray_col_bind_declines(ray_t* tbl, int64_t sym);

#endif /* RAY_COLBIND_H */
