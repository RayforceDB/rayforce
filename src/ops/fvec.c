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

#include "fvec.h"
#include "mem/sys.h"
#include "table/sym.h"
#include <string.h>
#include <stdio.h>

ray_ftable_t* ray_ftable_new(uint16_t n_cols) {
    ray_ftable_t* ft = (ray_ftable_t*)ray_sys_alloc(sizeof(ray_ftable_t));
    if (!ft) return NULL;
    memset(ft, 0, sizeof(ray_ftable_t));

    ft->columns = (ray_fvec_t*)ray_sys_alloc((size_t)n_cols * sizeof(ray_fvec_t));
    if (!ft->columns) {
        ray_sys_free(ft);
        return NULL;
    }
    memset(ft->columns, 0, (size_t)n_cols * sizeof(ray_fvec_t));
    ft->n_cols = n_cols;

    return ft;
}

void ray_ftable_free(ray_ftable_t* ft) {
    if (!ft) return;

    if (ft->columns) {
        for (uint16_t i = 0; i < ft->n_cols; i++) {
            if (ft->columns[i].vec) ray_release(ft->columns[i].vec);
        }
        ray_sys_free(ft->columns);
    }
    if (ft->semijoin) ray_release(ft->semijoin);
    ray_sys_free(ft);
}

ray_t* ray_ftable_materialize(ray_ftable_t* ft) {
    if (!ft || ft->n_cols == 0) return ray_error("type", NULL);

    ray_t* tbl = ray_table_new(ft->n_cols);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    for (uint16_t c = 0; c < ft->n_cols; c++) {
        ray_fvec_t* fv = &ft->columns[c];
        if (!fv->vec) continue;

        ray_t* col;
        if (fv->cur_idx >= 0) {
            /* Flat: replicate single value */
            if (fv->cardinality <= 0) { ray_release(tbl); return ray_error("range", NULL); }
            col = ray_vec_new(fv->vec->type, fv->cardinality);
            if (!col || RAY_IS_ERR(col)) { ray_release(tbl); return col ? col : ray_error("oom", NULL); }
            col->len = fv->cardinality;
            void* val = ray_vec_get(fv->vec, fv->cur_idx);
            if (!val) { ray_release(col); ray_release(tbl); return ray_error("range", NULL); }
            uint8_t esz = ray_sym_elem_size(fv->vec->type, fv->vec->attrs);
            char* dst = (char*)ray_data(col);
            for (int64_t r = 0; r < fv->cardinality; r++)
                memcpy(dst + r * esz, val, esz);
        } else {
            /* Unflat: use as-is */
            col = fv->vec;
            ray_retain(col);
        }

        char name_buf[12];
        int n = snprintf(name_buf, sizeof(name_buf), "_c%d", c);
        int64_t name_id = ray_sym_intern(name_buf, (size_t)n);
        ray_t* new_tbl = ray_table_add_col(tbl, name_id, col);
        ray_release(col);
        if (!new_tbl || RAY_IS_ERR(new_tbl)) {
            if (new_tbl != tbl) ray_release(tbl);
            return new_tbl ? new_tbl : ray_error("oom", NULL);
        }
        tbl = new_tbl;
    }

    return tbl;
}
