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

#ifndef RAY_TABLE_H
#define RAY_TABLE_H

/*
 * table.h -- Table operations.
 *
 * A table has type = RAY_TABLE (98), len = current column count.
 * Data region: first sizeof(ray_t*) bytes = pointer to schema (I64 vector
 * of column name symbol IDs), then ncols * sizeof(ray_t*) = column vector
 * pointers.
 */

#include <rayforce.h>
#include <stdbool.h>

int64_t ray_parted_nrows(ray_t* parted_col);

/* Logical rows of any column object (plain, parted wrapper, MAPCOMMON,
 * RAY_ENCCOL) without decoding. */
int64_t ray_table_col_nrows(ray_t* col);

typedef struct {
    int8_t   type;        /* logical type; parted: RAY_PARTED_BASE + base */
    uint8_t  attrs;       /* generation 0: the vector's attrs; generation 2: persisted attrs */
    int64_t  len;         /* rows (parted: sum over segments) */
    int64_t  n_segs;      /* parted: segment count; else 0 */
    uint8_t  index_kind;  /* attached (gen 0) or recorded (gen 2) ray_idx_kind_t */
    bool     unique;
    int64_t  null_count;  /* generation 2: exact, from the directory; -1 when unknown */
    ray_t*   enc;         /* borrowed RAY_ENCCOL, or NULL */
} ray_col_meta_t;

bool    ray_col_meta_of(ray_t* col, ray_col_meta_t* m);          /* any slot object */
bool    ray_table_col_meta(ray_t* tbl, int64_t name_id, ray_col_meta_t* m);
bool    ray_table_col_meta_idx(ray_t* tbl, int64_t idx, ray_col_meta_t* m);

/* Index of a column without decoding when it cannot carry one: NULL with
 * *index set (borrowed RAY_INDEX block or NULL), or an owned error. */
ray_t*  ray_table_col_index(ray_t* tbl, int64_t name_id, ray_t** index);
ray_t*  ray_table_col_index_idx(ray_t* tbl, int64_t idx, ray_t** index);

/* The borrowed RAY_ENCCOL object of a slot, or NULL (absent, plain). */
ray_t*  ray_table_get_col_enc(ray_t* tbl, int64_t name_id);
ray_t*  ray_table_get_col_enc_idx(ray_t* tbl, int64_t idx);

/* Null presence from metadata; never materializes. */
bool    ray_table_col_has_nulls(ray_t* tbl, int64_t name_id);

/* The table's column list for an in-place value write, or NULL when the
 * table is not a table or any slot holds a RAY_ENCCOL (callers then take
 * their copy route).  Ownership checks (rc) stay with the caller. */
ray_t*  ray_table_cols_inplace(ray_t* tbl);

/* The column-list container is canonical: a RAY_LIST with zero attrs whose
 * length is the schema's.  Reads only the container header, never a slot. */
bool    ray_table_cols_canonical(ray_t* tbl);

#endif /* RAY_TABLE_H */
