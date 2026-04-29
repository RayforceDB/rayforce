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

#include "table.h"
#include "mem/heap.h"
#include "ops/ops.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Data layout — same shape as RAY_DICT.
 *
 * Block header (32 B):  type = RAY_TABLE, len = 2
 *   slot[0] = ray_t* schema    — RAY_I64 vector of column name sym IDs
 *   slot[1] = ray_t* cols      — RAY_LIST of column vectors
 *
 * `tbl->len` is the slot count (always 2).  Use ray_table_ncols() to get
 * the column count, ray_table_nrows() for the row count.
 *
 * The schema vector stays RAY_I64 (rather than RAY_SYM) because the rest
 * of the codebase reads it as `int64_t* ids = ray_data(schema)` in
 * dozens of hot loops; RAY_SYM's adaptive widths (W8/W16/W32/W64) would
 * silently truncate those reads.  RAY_DICT is free to use any keys type;
 * TABLE deliberately pins schema to I64 for that interop.
 * -------------------------------------------------------------------------- */

#define TBL_DATA_SIZE  (2 * sizeof(ray_t*))

static inline ray_t** tbl_slots(ray_t* tbl) {
    return (ray_t**)ray_data(tbl);
}

static inline ray_t* tbl_schema(ray_t* tbl) {
    return tbl_slots(tbl)[0];
}

static inline ray_t* tbl_cols(ray_t* tbl) {
    return tbl_slots(tbl)[1];
}

/* --------------------------------------------------------------------------
 * ray_table_new — allocates an empty table with capacity for `ncols`.
 *
 * The schema vector and cols list are pre-sized to avoid early grows.
 * Callers append columns via ray_table_add_col.
 * -------------------------------------------------------------------------- */

ray_t* ray_table_new(int64_t ncols) {
    if (ncols < 0) return ray_error("range", NULL);

    ray_t* tbl = ray_alloc(TBL_DATA_SIZE);
    if (!tbl) return ray_error("oom", "table_new(ncols=%lld)", (long long)ncols);
    if (RAY_IS_ERR(tbl)) return tbl;
    tbl->type  = RAY_TABLE;
    tbl->attrs = 0;
    tbl->len   = 2;
    memset(tbl->nullmap, 0, 16);
    memset(ray_data(tbl), 0, TBL_DATA_SIZE);

    ray_t* schema = ray_vec_new(RAY_I64, ncols);
    if (!schema || RAY_IS_ERR(schema)) {
        ray_free(tbl);
        return schema ? schema : ray_error("oom", NULL);
    }
    ray_t* cols = ray_list_new(ncols);
    if (!cols || RAY_IS_ERR(cols)) {
        ray_release(schema);
        ray_free(tbl);
        return cols ? cols : ray_error("oom", NULL);
    }
    tbl_slots(tbl)[0] = schema;
    tbl_slots(tbl)[1] = cols;
    return tbl;
}

/* --------------------------------------------------------------------------
 * ray_table_add_col — append `col_vec` under name `name_id`.
 *
 * Consumes one ref of the input table; on success returns an owned ref to
 * the (possibly COW'd) result.  Retains `col_vec` internally so the caller
 * keeps its own ref.
 * -------------------------------------------------------------------------- */

ray_t* ray_table_add_col(ray_t* tbl, int64_t name_id, ray_t* col_vec) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    if (!col_vec || RAY_IS_ERR(col_vec)) return ray_error("type", NULL);

    tbl = ray_cow(tbl);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    ray_t** slots = tbl_slots(tbl);
    ray_t* schema = slots[0];
    ray_t* cols   = slots[1];

    /* schema and cols may themselves be shared after cow — append helpers
     * COW them as needed and return the (possibly new) owned ref. */
    ray_t* new_schema = ray_vec_append(schema, &name_id);
    if (!new_schema || RAY_IS_ERR(new_schema)) { ray_release(tbl); return new_schema ? new_schema : ray_error("oom", NULL); }
    slots[0] = new_schema;

    ray_retain(col_vec);
    ray_t* new_cols = ray_list_append(cols, col_vec);
    ray_release(col_vec);
    if (!new_cols || RAY_IS_ERR(new_cols)) { ray_release(tbl); return new_cols ? new_cols : ray_error("oom", NULL); }
    slots[1] = new_cols;

    return tbl;
}

/* --------------------------------------------------------------------------
 * ray_table_get_col — lookup column by sym id; borrowed pointer or NULL.
 * -------------------------------------------------------------------------- */

ray_t* ray_table_get_col(ray_t* tbl, int64_t name_id) {
    if (!tbl || RAY_IS_ERR(tbl)) return NULL;
    ray_t* schema = tbl_schema(tbl);
    ray_t* cols   = tbl_cols(tbl);
    if (!schema || !cols) return NULL;
    int64_t* ids = (int64_t*)ray_data(schema);
    int64_t ncols = schema->len;
    ray_t** col_ptrs = (ray_t**)ray_data(cols);
    for (int64_t i = 0; i < ncols; i++)
        if (ids[i] == name_id) return col_ptrs[i];
    return NULL;
}

/* --------------------------------------------------------------------------
 * ray_table_get_col_idx — borrowed pointer at slot `idx`, or NULL.
 * -------------------------------------------------------------------------- */

ray_t* ray_table_get_col_idx(ray_t* tbl, int64_t idx) {
    if (!tbl || RAY_IS_ERR(tbl)) return NULL;
    ray_t* cols = tbl_cols(tbl);
    if (!cols) return NULL;
    if (idx < 0 || idx >= cols->len) return NULL;
    return ((ray_t**)ray_data(cols))[idx];
}

/* --------------------------------------------------------------------------
 * ray_table_col_name — sym id at slot `idx`, -1 on out-of-range.
 * -------------------------------------------------------------------------- */

int64_t ray_table_col_name(ray_t* tbl, int64_t idx) {
    if (!tbl || RAY_IS_ERR(tbl)) return -1;
    ray_t* schema = tbl_schema(tbl);
    if (!schema) return -1;
    if (idx < 0 || idx >= schema->len) return -1;
    return ((int64_t*)ray_data(schema))[idx];
}

/* --------------------------------------------------------------------------
 * ray_table_set_col_name — overwrite name at `idx`.  Caller must ensure
 * exclusive ownership (rc==1) before calling.
 * -------------------------------------------------------------------------- */

void ray_table_set_col_name(ray_t* tbl, int64_t idx, int64_t name_id) {
    if (!tbl || RAY_IS_ERR(tbl)) return;
    ray_t** slots = tbl_slots(tbl);
    ray_t* schema = slots[0];
    if (!schema || RAY_IS_ERR(schema)) return;
    if (idx < 0 || idx >= schema->len) return;
    schema = ray_cow(schema);
    if (!schema || RAY_IS_ERR(schema)) return;
    slots[0] = schema;
    ((int64_t*)ray_data(schema))[idx] = name_id;
}

/* --------------------------------------------------------------------------
 * ray_table_ncols / ray_table_nrows / ray_table_schema
 * -------------------------------------------------------------------------- */

int64_t ray_table_ncols(ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return 0;
    ray_t* schema = tbl_schema(tbl);
    return schema ? schema->len : 0;
}

int64_t ray_table_nrows(ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return 0;
    ray_t* cols = tbl_cols(tbl);
    if (!cols || cols->len <= 0) return 0;
    ray_t* first_col = ((ray_t**)ray_data(cols))[0];
    if (!first_col || RAY_IS_ERR(first_col)) return 0;

    if (RAY_IS_PARTED(first_col->type) || first_col->type == RAY_MAPCOMMON)
        return ray_parted_nrows(first_col);

    return first_col->len;
}

int64_t ray_parted_nrows(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return 0;
    if (!RAY_IS_PARTED(v->type) && v->type != RAY_MAPCOMMON) return v->len;

    if (v->type == RAY_MAPCOMMON) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        ray_t* counts = ptrs[1];
        if (!counts || RAY_IS_ERR(counts)) return 0;
        int64_t total = 0;
        int64_t* cdata = (int64_t*)ray_data(counts);
        for (int64_t i = 0; i < counts->len; i++)
            total += cdata[i];
        return total;
    }

    int64_t n_segs = v->len;
    ray_t** segs = (ray_t**)ray_data(v);
    int64_t total = 0;
    for (int64_t i = 0; i < n_segs; i++) {
        if (segs[i] && !RAY_IS_ERR(segs[i]))
            total += segs[i]->len;
    }
    return total;
}

ray_t* ray_table_schema(ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return NULL;
    return tbl_schema(tbl);
}
