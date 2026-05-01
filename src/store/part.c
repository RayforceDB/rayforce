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

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(RAY_OS_WINDOWS)
#define _GNU_SOURCE
#endif
#include "part.h"
#include "core/platform.h"
#include "mem/sys.h"
#include "ops/ops.h"
#include "store/splay.h"
#include "table/sym.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

/* Validate YYYY.MM.DD format: exactly 10 chars, dots at pos 4/7,
 * month 01-12, day 01-31. */
static bool is_date_dir(const char* name) {
    if (strlen(name) != 10) return false;
    if (name[4] != '.' || name[7] != '.') return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (name[i] < '0' || name[i] > '9') return false;
    }
    int month = (name[5] - '0') * 10 + (name[6] - '0');
    int day   = (name[8] - '0') * 10 + (name[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

/* Check if string is a pure integer (digits only, possibly with leading minus). */
static bool is_integer_str(const char* s) {
    if (!*s) return false;
    if (*s == '-') s++;
    if (!*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return false;
    return true;
}

/* Infer MAPCOMMON sub-type from partition directory names. */
static uint8_t infer_mc_type(char** part_dirs, int64_t part_count) {
    bool all_date = true, all_int = true;
    for (int64_t i = 0; i < part_count; i++) {
        if (all_date && !is_date_dir(part_dirs[i])) all_date = false;
        if (all_int && !is_integer_str(part_dirs[i])) all_int = false;
        if (!all_date && !all_int) break;
    }
    if (all_date) return RAY_MC_DATE;
    if (all_int) return RAY_MC_I64;
    return RAY_MC_SYM;
}

/* Parse "YYYY.MM.DD" → days since 2000-01-01 (Rayforce epoch).
 * Uses inverse of Hinnant's civil_from_days algorithm (same as exec.c). */
static int32_t parse_date_dir(const char* name) {
    int64_t y = (name[0]-'0')*1000 + (name[1]-'0')*100 +
                (name[2]-'0')*10   + (name[3]-'0');
    int64_t m = (name[5]-'0')*10 + (name[6]-'0');
    int64_t d = (name[8]-'0')*10 + (name[9]-'0');
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = (uint64_t)(y - era * 400);
    uint64_t doy = (153 * (m > 2 ? (uint64_t)m-3 : (uint64_t)m+9) + 2)/5 + (uint64_t)d - 1;
    uint64_t doe = yoe*365 + yoe/4 - yoe/100 + doy;
    return (int32_t)(era * 146097 + (int64_t)doe - 719468 - 10957);
}

/* Parse integer string → int64_t. Caller guarantees is_integer_str(). */
static int64_t parse_int_dir(const char* s) {
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    int64_t v = 0;
    for (; *s; s++) v = v * 10 + (*s - '0');
    return neg ? -v : v;
}

/* --------------------------------------------------------------------------
 * Partitioned table: date-partitioned directory of splayed tables
 *
 * Format:
 *   db_root/sym              — global symbol intern table
 *   db_root/YYYY.MM.DD/      — partition directories
 *   db_root/YYYY.MM.DD/table — splayed table per partition
 *
 * No symlink check: local-trust file format; path traversal checks
 * cover main attack vector.
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * collect_part_dirs — scan db_root for partition directories
 *
 * Collects directory names that match digit/dot pattern, bubble-sorts them.
 * If skip_sym is true, entries named "sym" are skipped.
 * Caller must free each entry with ray_sys_free and the array itself.
 * -------------------------------------------------------------------------- */

static ray_err_t collect_part_dirs(const char* db_root, char*** out_dirs,
                                   int64_t* out_count, bool skip_sym) {
    DIR* d = opendir(db_root);
    if (!d) return RAY_ERR_IO;

    char** part_dirs = NULL;
    int64_t part_count = 0;
    int64_t part_cap = 0;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (skip_sym && strcmp(ent->d_name, "sym") == 0) continue;

        /* Partition directory name format validation is intentionally loose:
         * accepts any sequence of digits and dots (e.g. "2024.01.15").
         * Invalid entries fail during splay load and are caught there. */
        bool valid = (ent->d_name[0] != '\0');
        for (const char* c = ent->d_name; *c; c++) {
            if (*c == '.' || (*c >= '0' && *c <= '9')) continue;
            valid = false; break;
        }
        if (!valid) continue;

        if (part_count >= part_cap) {
            part_cap = part_cap == 0 ? 16 : part_cap * 2;
            char** tmp = (char**)ray_sys_realloc(part_dirs, (size_t)part_cap * sizeof(char*));
            if (!tmp) break;
            part_dirs = tmp;
        }
        char* dup = ray_sys_strdup(ent->d_name);
        if (!dup) break;
        part_dirs[part_count++] = dup;
    }
    closedir(d);

    if (part_count == 0) {
        ray_sys_free(part_dirs);
        return RAY_ERR_IO;
    }

    /* Sort partition names for deterministic order.
     * O(n^2) but partition count is typically small (< 1000 daily partitions). */
    for (int64_t i = 0; i < part_count - 1; i++) {
        for (int64_t j = i + 1; j < part_count; j++) {
            if (strcmp(part_dirs[i], part_dirs[j]) > 0) {
                char* tmp = part_dirs[i];
                part_dirs[i] = part_dirs[j];
                part_dirs[j] = tmp;
            }
        }
    }

    *out_dirs = part_dirs;
    *out_count = part_count;
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * ray_read_parted — zero-copy open of a partitioned table
 *
 * Builds parted columns (RAY_PARTED_BASE + base_type) where each segment
 * is an mmap'd vector from ray_read_splayed. Also builds a MAPCOMMON column
 * with partition key names and row counts.
 * -------------------------------------------------------------------------- */

ray_t* ray_read_parted(const char* db_root, const char* table_name) {
    if (!db_root || !table_name) return ray_error("io", NULL);

    /* Validate table_name: no path separators or traversal */
    if (strchr(table_name, '/') || strchr(table_name, '\\') ||
        strstr(table_name, "..") || table_name[0] == '.')
        return ray_error("io", NULL);

    /* Build sym_path. */
    char sym_path[1024];
    int sn = snprintf(sym_path, sizeof(sym_path), "%s/sym", db_root);
    if (sn < 0 || (size_t)sn >= sizeof(sym_path))
        return ray_error("io", NULL);

    /* Load global symfile if present.  Tables without RAY_SYM columns
     * never produce a global symfile (.db.splayed.set only writes per-table
     * sym files inside the leaf splayed dir), so a missing root-level
     * symfile is normal — not an error. */
    struct stat sym_st;
    if (stat(sym_path, &sym_st) == 0) {
        ray_err_t sym_err = ray_sym_load(sym_path);
        if (sym_err != RAY_OK) return ray_error(ray_err_code_str(sym_err), NULL);
    }

    /* Scan db_root for partition directories (skip "sym" entry) */
    char** part_dirs = NULL;
    int64_t part_count = 0;
    ray_err_t collect_err = collect_part_dirs(db_root, &part_dirs, &part_count, true);
    if (collect_err != RAY_OK) return ray_error("io", NULL);

    /* Open each partition via ray_read_splayed */
    ray_t** part_tables = (ray_t**)ray_sys_alloc((size_t)part_count * sizeof(ray_t*));
    if (!part_tables) goto fail_dirs;
    memset(part_tables, 0, (size_t)part_count * sizeof(ray_t*));

    char path[1024];
    for (int64_t p = 0; p < part_count; p++) {
        int pn = snprintf(path, sizeof(path), "%s/%s/%s", db_root, part_dirs[p], table_name);
        if (pn < 0 || (size_t)pn >= sizeof(path)) {
            part_tables[p] = NULL;
            goto fail_tables;
        }
        part_tables[p] = ray_read_splayed(path, NULL);
        if (!part_tables[p] || RAY_IS_ERR(part_tables[p])) {
            part_tables[p] = NULL;
            goto fail_tables;
        }
    }

    /* Get schema from first partition */
    int64_t ncols = ray_table_ncols(part_tables[0]);
    if (ncols <= 0) goto fail_tables;

    /* Infer MAPCOMMON sub-type from partition directory names */
    uint8_t mc_type = infer_mc_type(part_dirs, part_count);

    /* Build result table: 1 MAPCOMMON + ncols data columns */
    ray_t* result = ray_table_new(ncols + 2);
    if (!result || RAY_IS_ERR(result)) goto fail_tables;

    /* ---- MAPCOMMON column (first) ---- */
    {
        /* key_values type matches inferred partition key type */
        int8_t kv_type = (mc_type == RAY_MC_DATE) ? RAY_DATE
                       : (mc_type == RAY_MC_I64)  ? RAY_I64
                       :                           RAY_SYM;
        ray_t* key_values = ray_vec_new(kv_type, part_count);
        ray_t* row_counts = ray_vec_new(RAY_I64, part_count);
        if (!key_values || RAY_IS_ERR(key_values) ||
            !row_counts || RAY_IS_ERR(row_counts)) {
            if (key_values && !RAY_IS_ERR(key_values)) ray_release(key_values);
            if (row_counts && !RAY_IS_ERR(row_counts)) ray_release(row_counts);
            ray_release(result);
            goto fail_tables;
        }

        int64_t* rc_data = (int64_t*)ray_data(row_counts);
        if (mc_type == RAY_MC_DATE) {
            int32_t* kv_data = (int32_t*)ray_data(key_values);
            for (int64_t p = 0; p < part_count; p++) {
                kv_data[p] = parse_date_dir(part_dirs[p]);
                rc_data[p] = ray_table_nrows(part_tables[p]);
            }
        } else if (mc_type == RAY_MC_I64) {
            int64_t* kv_data = (int64_t*)ray_data(key_values);
            for (int64_t p = 0; p < part_count; p++) {
                kv_data[p] = parse_int_dir(part_dirs[p]);
                rc_data[p] = ray_table_nrows(part_tables[p]);
            }
        } else {
            int64_t* kv_data = (int64_t*)ray_data(key_values);
            for (int64_t p = 0; p < part_count; p++) {
                kv_data[p] = ray_sym_intern(part_dirs[p], strlen(part_dirs[p]));
                rc_data[p] = ray_table_nrows(part_tables[p]);
            }
        }
        key_values->len = part_count;
        row_counts->len = part_count;

        ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
        if (!mapcommon || RAY_IS_ERR(mapcommon)) {
            ray_release(key_values);
            ray_release(row_counts);
            ray_release(result);
            goto fail_tables;
        }
        mapcommon->type = RAY_MAPCOMMON;
        mapcommon->len = 2;
        mapcommon->attrs = mc_type;
        memset(mapcommon->nullmap, 0, 16);

        ray_t** mc_ptrs = (ray_t**)ray_data(mapcommon);
        mc_ptrs[0] = key_values;  ray_retain(key_values);
        mc_ptrs[1] = row_counts;  ray_retain(row_counts);

        const char* mc_name = (mc_type == RAY_MC_DATE) ? "date" : "part";
        int64_t part_name_id = ray_sym_intern(mc_name, strlen(mc_name));
        result = ray_table_add_col(result, part_name_id, mapcommon);
        if (!result || RAY_IS_ERR(result)) {
            ray_release(mapcommon);
            ray_release(key_values);
            ray_release(row_counts);
            goto fail_tables;
        }

        ray_release(mapcommon);
        ray_release(key_values);
        ray_release(row_counts);
    }

    /* ---- Data columns (after MAPCOMMON) ---- */
    for (int64_t c = 0; c < ncols; c++) {
        int64_t name_id = ray_table_col_name(part_tables[0], c);
        ray_t* first_seg = ray_table_get_col_idx(part_tables[0], c);
        if (!first_seg) continue;

        ray_t* parted = ray_alloc((size_t)part_count * sizeof(ray_t*));
        if (!parted || RAY_IS_ERR(parted)) {
            ray_release(result);
            goto fail_tables;
        }
        parted->type = RAY_PARTED_BASE + first_seg->type;
        parted->len = part_count;
        parted->attrs = 0;
        memset(parted->nullmap, 0, 16);

        ray_t** segs = (ray_t**)ray_data(parted);
        for (int64_t p = 0; p < part_count; p++) {
            ray_t* seg = ray_table_get_col_idx(part_tables[p], c);
            if (!seg) {
                segs[p] = NULL;
                continue;
            }
            ray_retain(seg);
            segs[p] = seg;
            ray_vm_advise_willneed(ray_data(seg),
                                  (size_t)seg->len * ray_sym_elem_size(seg->type, seg->attrs));
        }

        result = ray_table_add_col(result, name_id, parted);
        ray_release(parted);
        if (!result || RAY_IS_ERR(result)) goto fail_tables;
    }

    /* Release partition sub-tables (segment vectors survive via retain) */
    for (int64_t p = 0; p < part_count; p++) {
        if (part_tables[p]) ray_release(part_tables[p]);
        ray_sys_free(part_dirs[p]);
    }
    ray_sys_free(part_tables);
    ray_sys_free(part_dirs);

    return result;

fail_tables:
    for (int64_t p = 0; p < part_count; p++) {
        if (part_tables[p] && !RAY_IS_ERR(part_tables[p]))
            ray_release(part_tables[p]);
    }
    ray_sys_free(part_tables);

fail_dirs:
    for (int64_t p = 0; p < part_count; p++)
        ray_sys_free(part_dirs[p]);
    ray_sys_free(part_dirs);

    return ray_error("io", NULL);
}
