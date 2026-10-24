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

#ifndef RAY_OPS_CDFUSE_H
#define RAY_OPS_CDFUSE_H

#include "rayforce.h"

/* Minimum row count for the fused kernel; below it the existing group +
 * count-distinct rewrite is already fast enough and the scatter buffers
 * are not worth their allocation. */
#define CDF_MIN_ROWS 262144

/* Fused grouped count-distinct: for each distinct value of key_col,
 * count the distinct values of val_col over rows [0, nrows) (all rows;
 * selection handling is the caller's job — Task 4 only wires the
 * unfiltered path and declines otherwise).
 * Returns a RAY_TABLE with three I64 columns in this exact order:
 *   k[out_n], u[out_n], _first[out_n]
 * (keys, distinct counts, first source row) in stable first-seen key
 * order, or NULL when the shape is unsupported (caller falls back to the
 * existing rewrite).  key_col/val_col must be flat vectors of
 * numeric/temporal/SYM types, including nulls. Float keys in `k` are
 * canonical bit encodings; callers restore the source logical type. */
ray_t* ray_cd_fused(ray_t* key_col, ray_t* val_col, int64_t nrows);

/* The route for a `(count (distinct X))` grouped by K_cols[0..n_K) over
 * the nrows rows of `rows` (a selection's survivors, sampled from
 * `population` rows when that is above nrows; NULL for all rows 0..nrows)
 * on `workers` workers (#756): true for the two-pass rewrite —
 * group by (K, X), then count per K — false for grouping by K and counting
 * distinct X over each group's slice.  Decided from sampled cardinality
 * estimates (agg_group_card_sample): the rewrite where its (K, X) grouping
 * is dense, where the largest group holds more than CD_SCATTER_SKEW
 * workers' shares of the rows (the slice dedup is serial over a group),
 * past a million groups, and between 256 K and a million groups when the
 * (K, X) tuples dedup below three quarters of the rows; the slices
 * otherwise.  A sampled group count the slices would rely on is confirmed
 * by an approximate distinct count over the whole key column (a key the
 * sample reads as singletons may hold far more values than the Chao1
 * says): past a million there, the rewrite. */
bool ray_cd_two_pass_preferred(ray_t** K_cols, int n_K, ray_t* X_col,
                               const int64_t* rows, int64_t nrows, int64_t population,
                               int64_t workers);

/* Grouped count-distinct in bounded memory (the post-group scatter of a
 * select whose `(count (distinct X))` sits beside a group by K).  One
 * sequential pass over the K and X columns per round: each row's group id
 * comes from K through a table of the n_groups result keys `grp_keys`
 * (rows whose key is not among them are skipped), or from `row_gid` when
 * the caller has the row map already (then `key_col` and `grp_keys` are
 * unused, and the map carries the selection as -1 — or, with `sel` given
 * too, holds one id for each row `sel` keeps, in order: sel->total_pass
 * ids, not nrows).  The (group, value)
 * pairs are hash-partitioned and deduplicated chunk by chunk into
 * partition-local sets; when the sets would outgrow the memory budget (a
 * share of what the anon watermark leaves), the partitions past it are
 * dropped and counted by a further pass over the input instead — up to
 * about CDB_MAX_ROUNDS passes, past which the sets may pass the budget.  No
 * per-row index, no per-group slices.
 *
 * key_col/val_col: flat vectors, parted columns (a missing partition's rows
 * read as nulls, sized by the partition counts of `tbl`) or MAPCOMMON.  Key
 * types: BOOL U8 I16 I32 I64 DATE TIME TIMESTAMP SYM; value types the same
 * and F64.  `sel` is a rowsel over the nrows rows or NULL.  A null value is
 * one distinct value, as on every other count-distinct path.
 *
 * Returns an I64 vector of n_groups counts (group i of grp_keys / row_gid
 * id i), an error (oom, cancel), or NULL when a column is not served (a type
 * it does not read, fewer rows than nrows). */
ray_t* ray_cd_bounded(ray_t* tbl, ray_t* key_col, ray_t* grp_keys,
                      const int64_t* row_gid, int64_t n_groups,
                      ray_t* val_col, ray_t* sel, int64_t nrows);

/* The count of distinct values of val_col's first nrows rows, in bounded
 * memory: ray_cd_bounded over one group (every row in it).  An I64 atom, an
 * error, or NULL when the column is not served. */
ray_t* ray_cd_bounded_all(ray_t* val_col, int64_t nrows);

/* Whether ray_cd_bounded serves `col` as a key (is_val false) or a value:
 * a flat, parted or MAPCOMMON column, or a single-file encoded one (a
 * RAY_ENCCOL slot, read by range, never decoded whole) from its metadata. */
bool ray_cd_bounded_col_ok(ray_t* col, bool is_val);

/* Whether the scatter of a count-distinct of `val` over nrows rows of `tbl`
 * (those `sel` keeps, when given) into n_groups groups of `key` takes
 * ray_cd_bounded rather than the per-group slices (or the global hash past
 * 50 K groups): when the slices' row buffers and the flat copies of parted
 * columns would not fit half of the room under the anon watermark; else,
 * from CDB_MIN_ROWS rows up to 50 K groups, for a column not resident, and
 * from 64 M rows for a dominant group or pairs that dedup to a quarter of
 * the rows (sampled; over one partition of a parted pair).  key/val NULL (a
 * composite key, a row map built anyway): memory alone. */
#define CDB_MIN_ROWS 65536
bool ray_cd_bounded_preferred(ray_t* tbl, ray_t* key, ray_t* val, ray_t* sel,
                              int64_t nrows, int64_t n_groups);

#endif /* RAY_OPS_CDFUSE_H */
