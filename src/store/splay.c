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

#include "splay.h"
#include "store/col.h"
#include "store/fileio.h"
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Splayed table: directory of column files + .d schema file
 *
 * Format:
 *   dir/.d        — I64 vector of column name symbol IDs
 *   dir/<colname> — column file per column
 *
 * No symlink check: local-trust file format; path traversal checks
 * (rejecting '/', '\\', '..', leading '.') cover main attack vector.
 * -------------------------------------------------------------------------- */

/* Post-load validation: reject if sym table is empty but table has RAY_SYM
 * columns, or if schema expected columns but none could be loaded. */
static ray_err_t validate_sym_columns(ray_t* tbl, int64_t schema_ncols) {
    if (ray_sym_count() != 0) return RAY_OK;

    int64_t nc = ray_table_ncols(tbl);
    if (schema_ncols > 0 && nc == 0) return RAY_ERR_CORRUPT;

    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col && col->type == RAY_SYM) return RAY_ERR_CORRUPT;
    }
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * ray_splay_save — save a table to a splayed table directory
 * -------------------------------------------------------------------------- */

ray_err_t ray_splay_save(ray_t* tbl, const char* dir, const char* sym_path) {
    if (!tbl || RAY_IS_ERR(tbl)) return RAY_ERR_TYPE;
    if (!dir) return RAY_ERR_IO;

    /* Create directory and any missing parents (mkdir -p semantics).
     * Required for partitioned layouts like "/db/2024.01.01/t/" where the
     * caller hasn't pre-created the date partition. */
    ray_err_t mkdir_err = ray_mkdir_p(dir);
    if (mkdir_err != RAY_OK) return mkdir_err;

    /* Save symbol table if sym_path provided */
    if (sym_path) {
        ray_err_t sym_err = ray_sym_save(sym_path);
        if (sym_err != RAY_OK) return sym_err;
    }

    int64_t ncols = ray_table_ncols(tbl);

    /* Save .d schema file */
    ray_t* schema = ray_table_schema(tbl);
    if (schema) {
        char path[1024];
        int path_len = snprintf(path, sizeof(path), "%s/.d", dir);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) return RAY_ERR_RANGE;
        ray_err_t err = ray_col_save(schema, path);
        if (err != RAY_OK) return err;
    }

    /* Save each column */
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        int64_t name_id = ray_table_col_name(tbl, c);
        if (!col) continue;

        /* Get column name string */
        ray_t* name_atom = ray_sym_str(name_id);
        if (!name_atom) continue;

        const char* name = ray_str_ptr(name_atom);
        size_t name_len = ray_str_len(name_atom);

        /* Reject names with path separators, traversal, or starting with '.' */
        if (name_len == 0 || name[0] == '.' ||
            memchr(name, '/', name_len) || memchr(name, '\\', name_len) ||
            memchr(name, '\0', name_len))
            continue;

        char path[1024];
        int path_len = snprintf(path, sizeof(path), "%s/%.*s", dir, (int)name_len, name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) return RAY_ERR_RANGE;

        ray_err_t err = ray_col_save(col, path);
        /* On partial failure, columns 0..c-1 remain on disk.
         * Caller should clean up or use atomic rename for safe writes. */
        if (err != RAY_OK) return err;
    }

    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * splay_load_impl — shared implementation for ray_splay_load / ray_read_splayed
 *
 * When use_mmap is false, columns are loaded via ray_col_load (buddy copy).
 * When use_mmap is true, columns are loaded via ray_col_mmap (zero-copy).
 * The .d schema is always loaded via ray_col_load (small, buddy copy).
 * -------------------------------------------------------------------------- */

static ray_t* splay_load_impl(const char* dir, const char* sym_path, bool use_mmap) {
    if (!dir) return ray_error("io", NULL);

    /* Load symbol table if sym_path provided */
    if (sym_path) {
        ray_err_t sym_err = ray_sym_load(sym_path);
        if (sym_err != RAY_OK) return ray_error(ray_err_code_str(sym_err), NULL);
    }

    /* Load .d schema */
    char path[1024];
    int path_len = snprintf(path, sizeof(path), "%s/.d", dir);
    if (path_len < 0 || (size_t)path_len >= sizeof(path))
        return ray_error("range", NULL);
    ray_t* schema = ray_col_load(path);
    if (!schema || RAY_IS_ERR(schema)) return schema;

    int64_t ncols = schema->len;
    int64_t* name_ids = (int64_t*)ray_data(schema);

    ray_t* tbl = ray_table_new(ncols);
    if (!tbl || RAY_IS_ERR(tbl)) {
        ray_release(schema);
        return tbl;
    }

    /* Load each column */
    for (int64_t c = 0; c < ncols; c++) {
        int64_t name_id = name_ids[c];
        ray_t* name_atom = ray_sym_str(name_id);
        if (!name_atom) {
            /* Schema references a sym ID that doesn't exist — sym table
             * is stale or wrong for this data. */
            ray_release(schema);
            ray_release(tbl);
            return ray_error("corrupt", NULL);
        }

        const char* name = ray_str_ptr(name_atom);
        size_t name_len = ray_str_len(name_atom);

        /* Reject names with path separators, traversal, or starting with '.'
         * — these indicate a stale/wrong sym file, not a column to skip. */
        if (name_len == 0 || name[0] == '.' ||
            memchr(name, '/', name_len) || memchr(name, '\\', name_len) ||
            memchr(name, '\0', name_len)) {
            ray_release(schema);
            ray_release(tbl);
            return ray_error("corrupt", NULL);
        }

        path_len = snprintf(path, sizeof(path), "%s/%.*s", dir, (int)name_len, name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            ray_release(schema);
            ray_release(tbl);
            return ray_error("range", NULL);
        }

        ray_t* col = use_mmap ? ray_col_mmap(path) : ray_col_load(path);
        if (!col || RAY_IS_ERR(col)) {
            ray_release(schema);
            ray_release(tbl);
            return col ? col : ray_error("io", NULL);
        }

        ray_t* new_df = ray_table_add_col(tbl, name_id, col);
        if (!new_df || RAY_IS_ERR(new_df)) {
            ray_release(col);
            ray_release(schema);
            ray_release(tbl);
            return new_df ? new_df : ray_error("oom", NULL);
        }
        ray_release(col); /* table_add_col retains; drop our ref */
        tbl = new_df;
    }

    ray_release(schema);

    ray_err_t sym_check = validate_sym_columns(tbl, ncols);
    if (sym_check != RAY_OK) {
        ray_release(tbl);
        return ray_error(ray_err_code_str(sym_check), NULL);
    }

    return tbl;
}

ray_t* ray_splay_load(const char* dir, const char* sym_path) {
    return splay_load_impl(dir, sym_path, false);
}

ray_t* ray_read_splayed(const char* dir, const char* sym_path) {
    return splay_load_impl(dir, sym_path, true);
}
