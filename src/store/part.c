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
#include "core/runtime.h"
#include "mem/sys.h"
#include "ops/ops.h"
#include "store/splay.h"
#include "table/sym.h"
#include "table/domain.h"
#include "lang/cal.h"   /* MONTHDAYS, date_leap_year — calendar-date validation */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>

/* Validate YYYY.MM.DD format: exactly 10 chars, dots at pos 4/7, and a real
 * calendar date.  A blanket day 01-31 check let calendar-impossible names like
 * 2024.02.31 through; parse_date_dir then silently normalised them into a
 * different real day (2024.03.02) — a mislabelled partition key.  Validate the
 * day against the leap-aware length of its month instead. */
static bool is_date_dir(const char* name) {
    if (strlen(name) != 10) return false;
    if (name[4] != '.' || name[7] != '.') return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (name[i] < '0' || name[i] > '9') return false;
    }
    int year  = (name[0]-'0')*1000 + (name[1]-'0')*100 +
                (name[2]-'0')*10   + (name[3]-'0');
    int month = (name[5] - '0') * 10 + (name[6] - '0');
    int day   = (name[8] - '0') * 10 + (name[9] - '0');
    if (month < 1 || month > 12 || day < 1) return false;
    int leap = date_leap_year(year);
    int dim  = (int)(MONTHDAYS[leap][month] - MONTHDAYS[leap][month - 1]);
    return day <= dim;
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
 *   db_root/.sym             — global symbol intern table (dotfile so it
 *                              never collides with a splayed table dir)
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
 * The symfile (".sym") and its lock are dotfiles, and partition names are
 * digit/dot-only, so neither is ever picked up here.
 * Caller must free each entry with ray_sys_free and the array itself.
 * -------------------------------------------------------------------------- */

static ray_err_t collect_part_dirs(const char* db_root, char*** out_dirs,
                                   int64_t* out_count) {
    DIR* d = opendir(db_root);
    if (!d) return RAY_ERR_IO;

    char** part_dirs = NULL;
    int64_t part_count = 0;
    int64_t part_cap = 0;
    ray_err_t err = RAY_OK;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

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
            char** tmp = (char**)ray_realloc_raw(part_dirs, (size_t)part_cap * sizeof(char*));
            if (!tmp) { err = RAY_ERR_OOM; break; }
            part_dirs = tmp;
        }
        size_t len = strlen(ent->d_name);
        char* dup = (char*)ray_alloc_raw(len + 1);
        if (!dup) { err = RAY_ERR_OOM; break; }
        memcpy(dup, ent->d_name, len + 1);
        part_dirs[part_count++] = dup;
    }
    closedir(d);

    if (err != RAY_OK) {
        for (int64_t i = 0; i < part_count; i++) ray_free_raw(part_dirs[i]);
        ray_free_raw(part_dirs);
        return err;
    }

    if (part_count == 0) {
        /* Directory opened fine but holds no partition directories — an
         * EMPTY (or non-parted) db, not an I/O failure.  Report success
         * with a zero count and let each caller decide what "empty" means
         * (tables/fill → empty result; get → error, nothing to read). */
        ray_free_raw(part_dirs);
        *out_dirs = NULL;
        *out_count = 0;
        return RAY_OK;
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
    bool trace = getenv("RAY_CSV_TRACE") != NULL;
    if (trace)
        fprintf(stderr, "parted.get: root=%s table=%s\n", db_root, table_name);

    /* Validate table_name: no path separators or traversal */
    if (strchr(table_name, '/') || strchr(table_name, '\\') ||
        strstr(table_name, "..") || table_name[0] == '.')
        return ray_error("io", NULL);

    /* Build sym_path. */
    char sym_path[1024];
    int sn = snprintf(sym_path, sizeof(sym_path), "%s/.sym", db_root);
    if (sn < 0 || (size_t)sn >= sizeof(sym_path))
        return ray_error("io", NULL);

    /* Open root/sym ONCE as the shared FILE domain for every partition's
     * SYM columns (sym-domain spec: partitions have no own symfiles; the
     * parted view is index-coherent by construction).  Tables without
     * RAY_SYM columns never produce a symfile, so a missing root-level
     * symfile is normal — the loud "sym" error fires only if a SYM
     * column is actually encountered (store/col.c). */
    struct ray_sym_domain_s* dom = NULL;
    struct stat sym_st;
    if (stat(sym_path, &sym_st) == 0) {
        dom = ray_sym_domain_open(sym_path);
        if (!dom)
            return ray_error("corrupt",
                "symfile %s: unreadable or invalid (bad magic, torn record, "
                "or missing \"\" at position 0)", sym_path);
    }

    /* Scan db_root for partition directories (the ".sym" dotfile is skipped) */
    char** part_dirs = NULL;
    int64_t part_count = 0;
    ray_err_t collect_err = collect_part_dirs(db_root, &part_dirs, &part_count);
    if (collect_err != RAY_OK) {
        if (trace)
            fprintf(stderr, "parted.get: collect dirs failed err=%s\n",
                    ray_err_code_str(collect_err));
        if (dom) ray_sym_domain_release(dom);
        return ray_error(ray_err_code_str(collect_err),
            "parted %s: cannot enumerate partition directories", db_root);
    }
    if (trace)
        fprintf(stderr, "parted.get: parts=%" PRId64 "\n", part_count);

    /* No partitions: there is no named table to read.  Unlike
     * .db.parted.tables (which lists nothing), reading data from an empty
     * or non-parted root is a genuine error. */
    if (part_count <= 0) {
        if (dom) ray_sym_domain_release(dom);
        return ray_error("io", "parted %s: no partition directories", db_root);
    }

    /* Open each partition via ray_read_splayed */
    ray_t* part_err = NULL;
    ray_t** part_tables = (ray_t**)ray_sys_alloc((size_t)part_count * sizeof(ray_t*));
    if (!part_tables) goto fail_dirs;
    memset(part_tables, 0, (size_t)part_count * sizeof(ray_t*));

    char path[1024];
    for (int64_t p = 0; p < part_count; p++) {
        /* Cancellation checkpoint (per partition — each loads a whole splayed
         * table).  part_tables[p] is still NULL; the fail_tables ladder
         * releases already-loaded partitions, part_dirs and the sym domain. */
        if (ray_interrupted()) {
            part_err = ray_error("cancel", "interrupted");
            goto fail_tables;
        }
        int pn = snprintf(path, sizeof(path), "%s/%s/%s", db_root, part_dirs[p], table_name);
        if (pn < 0 || (size_t)pn >= sizeof(path)) {
            part_tables[p] = NULL;
            part_err = ray_error("range", "parted %s: partition path %s/%s too long",
                                 db_root, part_dirs[p], table_name);
            goto fail_tables;
        }
        part_tables[p] = ray_read_splayed_dom(path, dom);
        if (!part_tables[p] || RAY_IS_ERR(part_tables[p])) {
            if (trace)
                fprintf(stderr, "parted.get: splayed load failed part=%" PRId64 " path=%s err=%s\n",
                        p, path,
                        part_tables[p] && RAY_IS_ERR(part_tables[p])
                            ? ray_err_code(part_tables[p]) : "io");
            /* Propagate the partition's error verbatim — the loud "sym"
             * error (SYM columns + no root/sym) must not degrade to a
             * generic "io". */
            part_err = part_tables[p]; /* error objects survive release */
            part_tables[p] = NULL;
            goto fail_tables;
        }
    }

    /* Get schema from first partition */
    int64_t ncols = ray_table_ncols(part_tables[0]);
    if (ncols <= 0) {
        if (trace)
            fprintf(stderr, "parted.get: empty first partition\n");
        part_err = ray_error("corrupt",
            "parted %s: first partition %s/%s has no columns",
            db_root, part_dirs[0], table_name);
        goto fail_tables;
    }
    if (trace)
        fprintf(stderr, "parted.get: ncols=%" PRId64 "\n", ncols);

    /* Cross-partition schema validation: column association is by INDEX
     * (see the data-column loop below), so every column a partition has
     * must match the first partition's name and type at the same index.
     * A partition MAY have fewer columns — the missing tail becomes NULL
     * segments (supported ragged mode) — but never more, and never a
     * renamed/retyped column (silent mis-association otherwise).  SYM
     * index width (attrs) may legally differ per partition — partitions
     * written at different dictionary sizes carry different index
     * widths; compare types only. */
    for (int64_t p = 1; p < part_count; p++) {
        int64_t ncols_p = ray_table_ncols(part_tables[p]);
        if (ncols_p > ncols) {
            part_err = ray_error("corrupt",
                "parted %s: partition %s/%s has %lld columns, "
                "expected at most %lld (from %s)",
                db_root, part_dirs[p], table_name,
                (long long)ncols_p, (long long)ncols, part_dirs[0]);
            goto fail_tables;
        }
        for (int64_t c = 0; c < ncols_p; c++) {
            int64_t n0 = ray_table_col_name(part_tables[0], c);
            int64_t np = ray_table_col_name(part_tables[p], c);
            ray_t* c0 = ray_table_get_col_idx(part_tables[0], c);
            ray_t* cp = ray_table_get_col_idx(part_tables[p], c);
            if (n0 != np || !c0 || !cp || c0->type != cp->type) {
                ray_t* na = ray_sym_str(np);
                part_err = ray_error("corrupt",
                    "parted %s: partition %s/%s column %lld ('%.*s') "
                    "does not match partition %s (name/type mismatch)",
                    db_root, part_dirs[p], table_name, (long long)c,
                    na ? (int)ray_str_len(na) : 1,
                    na ? ray_str_ptr(na) : "?", part_dirs[0]);
                goto fail_tables;
            }
        }
    }

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
        memset(mapcommon->aux, 0, 16);

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
            if (trace)
                fprintf(stderr, "parted.get: alloc failed col=%" PRId64 "\n", c);
            ray_release(result);
            goto fail_tables;
        }
        parted->type = RAY_PARTED_BASE + first_seg->type;
        parted->len = part_count;
        parted->attrs = 0;
        memset(parted->aux, 0, 16);

        ray_t** segs = (ray_t**)ray_data(parted);
        for (int64_t p = 0; p < part_count; p++) {
            ray_t* seg = ray_table_get_col_idx(part_tables[p], c);
            if (!seg) {
                segs[p] = NULL;
                continue;
            }
            ray_retain(seg);
            segs[p] = seg;
        }

        result = ray_table_add_col(result, name_id, parted);
        ray_release(parted);
        if (!result || RAY_IS_ERR(result)) goto fail_tables;
    }

    /* Release partition sub-tables (segment vectors survive via retain) */
    for (int64_t p = 0; p < part_count; p++) {
        if (part_tables[p]) ray_release(part_tables[p]);
        ray_free_raw(part_dirs[p]);
    }
    ray_sys_free(part_tables);
    ray_free_raw(part_dirs);
    if (dom) ray_sym_domain_release(dom); /* columns hold their own refs */

    return result;

fail_tables:
    if (trace)
        fprintf(stderr, "parted.get: failed\n");
    for (int64_t p = 0; p < part_count; p++) {
        if (part_tables[p] && !RAY_IS_ERR(part_tables[p]))
            ray_release(part_tables[p]);
    }
    ray_sys_free(part_tables);

fail_dirs:
    for (int64_t p = 0; p < part_count; p++)
        ray_free_raw(part_dirs[p]);
    ray_free_raw(part_dirs);
    if (dom) ray_sym_domain_release(dom);

    return part_err ? part_err : ray_error("io", NULL);
}

/* --------------------------------------------------------------------------
 * collect_table_dirs — list the splayed-table subdirectories (those carrying
 * a `.d` schema) of ONE partition directory `pdir`, as a sorted array of
 * malloc'd names.  Caller frees each name and the array.  Returns RAY_OK
 * (count may be 0), RAY_ERR_IO if pdir can't be opened, or RAY_ERR_OOM.
 * -------------------------------------------------------------------------- */
static ray_err_t collect_table_dirs(const char* pdir, char*** out_names,
                                    int64_t* out_count) {
    *out_names = NULL;
    *out_count = 0;
    DIR* d = opendir(pdir);
    if (!d) return RAY_ERR_IO;

    char** names = NULL;
    int64_t count = 0, cap = 0;
    ray_err_t oom = RAY_OK;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;   /* ".", "..", and the ".sym" dotfile */
        char dpath[1024];
        int dn = snprintf(dpath, sizeof(dpath), "%s/%s/.d", pdir, ent->d_name);
        if (dn < 0 || (size_t)dn >= sizeof(dpath)) continue;
        struct stat st;
        if (stat(dpath, &st) != 0 || !S_ISREG(st.st_mode)) continue; /* not a table */
        if (count >= cap) {
            int64_t ncap = cap == 0 ? 16 : cap * 2;
            char** tmp = (char**)ray_realloc_raw(names, (size_t)ncap * sizeof(char*));
            if (!tmp) { oom = RAY_ERR_OOM; break; }
            names = tmp; cap = ncap;
        }
        size_t len = strlen(ent->d_name);
        char* dup = (char*)ray_alloc_raw(len + 1);
        if (!dup) { oom = RAY_ERR_OOM; break; }
        memcpy(dup, ent->d_name, len + 1);
        names[count++] = dup;
    }
    closedir(d);

    if (oom != RAY_OK) {
        for (int64_t i = 0; i < count; i++) ray_free_raw(names[i]);
        ray_free_raw(names);
        return RAY_ERR_OOM;
    }

    /* Deterministic order. */
    for (int64_t i = 0; i + 1 < count; i++)
        for (int64_t j = i + 1; j < count; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char* t = names[i]; names[i] = names[j]; names[j] = t;
            }

    *out_names = names;
    *out_count = count;
    return RAY_OK;
}

/* Does partition `part` contain splayed table `tname` (i.e. a `.d` schema)? */
static bool partition_has_table(const char* db_root, const char* part,
                                const char* tname) {
    char dpath[1024];
    int n = snprintf(dpath, sizeof(dpath), "%s/%s/%s/.d", db_root, part, tname);
    if (n < 0 || (size_t)n >= sizeof(dpath)) return false;
    struct stat st;
    return stat(dpath, &st) == 0 && S_ISREG(st.st_mode);
}

/* Build a 0-row table with the same column names and types as `tmpl`. */
static ray_t* empty_table_like(ray_t* tmpl) {
    int64_t ncols = ray_table_ncols(tmpl);
    ray_t* out = ray_table_new(ncols);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tmpl, c);
        if (!col) { ray_release(out); return ray_error("type", "empty_table_like: null column"); }
        ray_t* ecol = NULL;
        if (col->type == RAY_LIST) {
            ecol = ray_list_new(0);
        } else if (col->type == RAY_TABLE || col->type == RAY_DICT) {
            int64_t name_id = ray_table_col_name(tmpl, c);
            ray_t* name = ray_sym_str(name_id);
            const char* name_ptr = name ? ray_str_ptr(name) : "?";
            int name_len = name ? (int)ray_str_len(name) : 1;
            const char* type_name = col->type == RAY_TABLE ? "TABLE" : "DICT";
            ray_release(out);
            return ray_error("nyi", "empty_table_like: column '%.*s' has unsupported nested type %s",
                             name_len, name_ptr, type_name);
        } else {
            ecol = ray_vec_new(col->type, 0);
        }
        if (!ecol || RAY_IS_ERR(ecol)) { ray_release(out); return ecol ? ecol : ray_error("oom", NULL); }
        ray_t* nout = ray_table_add_col(out, ray_table_col_name(tmpl, c), ecol);
        ray_release(ecol);
        if (!nout || RAY_IS_ERR(nout)) { ray_release(out); return nout ? nout : ray_error("oom", NULL); }
        out = nout;
    }
    return out;
}

/* --------------------------------------------------------------------------
 * ray_parted_tables — list the table names under a parted db root
 *
 * Table names are the splayed-table subdirectories of a partition (those
 * carrying a `.d` schema).  We read them from the LAST (sorted) partition:
 * partition dirs sort ascending (e.g. by date), so the last is the most
 * recent and reflects the current table set — a table added in a later
 * partition would be invisible if we only looked at the first/oldest.
 * Returns a sorted RAY_SYM vector suitable for ray_read_parted, or an error.
 * -------------------------------------------------------------------------- */
ray_t* ray_parted_tables(const char* db_root) {
    if (!db_root || !*db_root) return ray_error("io", NULL);

    char** part_dirs = NULL;
    int64_t part_count = 0;
    ray_err_t e = collect_part_dirs(db_root, &part_dirs, &part_count);
    if (e != RAY_OK)
        return ray_error(ray_err_code_str(e),
            "parted %s: cannot enumerate partition directories", db_root);
    if (part_count <= 0) {
        /* Existing-but-empty (or non-parted) root → no tables.  Return an
         * empty SYM vector rather than an error: a freshly-created db root
         * has zero tables, and that's a friendlier answer than failing. */
        ray_free_raw(part_dirs);
        ray_t* empty = ray_vec_new(RAY_SYM, 0);
        return empty ? empty : ray_error("oom", NULL);
    }

    /* Only the last (most recent) partition is needed; release the rest. */
    char pdir[1024];
    int pn = snprintf(pdir, sizeof(pdir), "%s/%s", db_root, part_dirs[part_count - 1]);
    for (int64_t p = 0; p < part_count; p++) ray_free_raw(part_dirs[p]);
    ray_free_raw(part_dirs);
    if (pn < 0 || (size_t)pn >= sizeof(pdir))
        return ray_error("range", "parted %s: partition path too long", db_root);

    char** names = NULL;
    int64_t count = 0;
    ray_err_t te = collect_table_dirs(pdir, &names, &count);
    if (te != RAY_OK)
        return ray_error(ray_err_code_str(te),
            "parted %s: cannot read partition %s", db_root, pdir);

    ray_t* result = ray_vec_new(RAY_SYM, count);
    if (!result || RAY_IS_ERR(result)) {
        for (int64_t i = 0; i < count; i++) ray_free_raw(names[i]);
        ray_free_raw(names);
        return result ? result : ray_error("oom", NULL);
    }
    result->len = count;
    int64_t* out = (int64_t*)ray_data(result);
    for (int64_t i = 0; i < count; i++) {
        out[i] = ray_sym_intern(names[i], strlen(names[i]));
        ray_free_raw(names[i]);
    }
    ray_free_raw(names);
    return result;
}

/* --------------------------------------------------------------------------
 * ray_parted_fill — fill missing tables across a parted db's partitions.
 *
 * For every table that appears in ANY partition, ensure every partition has
 * it: a partition missing the table gets an EMPTY copy whose schema is taken
 * from the most recent partition that does have it (the same canonical-schema
 * convention ray_parted_tables / ray_read_parted use).  This keeps queries
 * that span partitions from failing on a partition where a table is absent.
 *
 * Returns a sorted RAY_SYM vector of the partition names that were filled
 * (empty vector when nothing needed fixing), or an error.
 * -------------------------------------------------------------------------- */
ray_t* ray_parted_fill(const char* db_root) {
    if (!db_root || !*db_root) return ray_error("io", NULL);

    char** part_dirs = NULL;
    int64_t part_count = 0;
    ray_err_t e = collect_part_dirs(db_root, &part_dirs, &part_count);
    if (e != RAY_OK)
        return ray_error(ray_err_code_str(e),
            "parted %s: cannot enumerate partition directories", db_root);
    if (part_count <= 0) {
        /* Empty (or non-parted) root → nothing to fill.  Matches the
         * "empty vector when nothing needed fixing" contract, so a fill on
         * a fresh db root is a friendly no-op rather than an error. */
        ray_free_raw(part_dirs);
        ray_t* empty = ray_vec_new(RAY_SYM, 0);
        return empty ? empty : ray_error("oom", NULL);
    }

    /* Shared root symfile — pass to load/save so SYM columns intern against
     * the one vocabulary.  NULL when the DB is symbol-free (no <root>/.sym). */
    char sym_buf[1024];
    const char* sym_path = NULL;
    int sn = snprintf(sym_buf, sizeof(sym_buf), "%s/.sym", db_root);
    if (sn > 0 && (size_t)sn < sizeof(sym_buf)) {
        struct stat sst;
        if (stat(sym_buf, &sst) == 0) sym_path = sym_buf;
    }

    /* Union of table names across all partitions. */
    char** all = NULL;
    int64_t all_count = 0, all_cap = 0;
    uint8_t* fixed = (uint8_t*)ray_calloc_raw((size_t)((size_t)part_count) * (1));
    ray_err_t err = fixed ? RAY_OK : RAY_ERR_OOM;
    char cause[256] = {0};

    for (int64_t p = 0; p < part_count && err == RAY_OK; p++) {
        char pdir[1024];
        int pn = snprintf(pdir, sizeof(pdir), "%s/%s", db_root, part_dirs[p]);
        if (pn < 0 || (size_t)pn >= sizeof(pdir)) { err = RAY_ERR_RANGE; break; }
        char** names = NULL;
        int64_t cnt = 0;
        ray_err_t te = collect_table_dirs(pdir, &names, &cnt);
        if (te != RAY_OK) { err = te; break; }
        for (int64_t i = 0; i < cnt; i++) {
            bool seen = false;
            for (int64_t k = 0; k < all_count; k++)
                if (strcmp(all[k], names[i]) == 0) { seen = true; break; }
            if (seen) { ray_free_raw(names[i]); continue; }
            if (all_count >= all_cap) {
                int64_t nc = all_cap == 0 ? 16 : all_cap * 2;
                char** tmp = (char**)ray_realloc_raw(all, (size_t)nc * sizeof(char*));
                if (!tmp) { err = RAY_ERR_OOM; ray_free_raw(names[i]); continue; }
                all = tmp; all_cap = nc;
            }
            all[all_count++] = names[i];   /* transfer ownership */
        }
        ray_free_raw(names);
    }

    /* For each table: template = last partition with it; fill the rest. */
    for (int64_t t = 0; t < all_count && err == RAY_OK; t++) {
        const char* tname = all[t];
        int64_t templ = -1;
        for (int64_t p = part_count - 1; p >= 0; p--)
            if (partition_has_table(db_root, part_dirs[p], tname)) { templ = p; break; }
        if (templ < 0) continue;            /* unreachable (it's in the union) */

        ray_t* empty_tbl = NULL;            /* built lazily on first miss */
        for (int64_t p = 0; p < part_count && err == RAY_OK; p++) {
            if (partition_has_table(db_root, part_dirs[p], tname)) continue;

            if (!empty_tbl) {
                char tdir[1024];
                int tn = snprintf(tdir, sizeof(tdir), "%s/%s/%s",
                                  db_root, part_dirs[templ], tname);
                if (tn < 0 || (size_t)tn >= sizeof(tdir)) { err = RAY_ERR_RANGE; break; }
                ray_t* full = ray_read_splayed(tdir, sym_path);
                if (!full || RAY_IS_ERR(full)) {
                    err = full ? ray_err_from_obj(full) : RAY_ERR_OOM;
                    const char* detail = ray_error_msg();
                    snprintf(cause, sizeof(cause), "table %s (partition %s): %s",
                             tname, part_dirs[templ],
                             detail && *detail ? detail : "template read failed");
                    if (full) ray_error_free(full);
                    break;
                }
                empty_tbl = empty_table_like(full);
                ray_release(full);
                if (!empty_tbl || RAY_IS_ERR(empty_tbl)) {
                    err = empty_tbl ? ray_err_from_obj(empty_tbl) : RAY_ERR_OOM;
                    const char* detail = ray_error_msg();
                    snprintf(cause, sizeof(cause), "table %s: %s", tname,
                             detail && *detail ? detail : "empty table construction failed");
                    if (empty_tbl) ray_error_free(empty_tbl);
                    empty_tbl = NULL;
                    break;
                }
            }

            char ptdir[1024];
            int ptn = snprintf(ptdir, sizeof(ptdir), "%s/%s/%s",
                               db_root, part_dirs[p], tname);
            if (ptn < 0 || (size_t)ptn >= sizeof(ptdir)) { err = RAY_ERR_RANGE; break; }
            ray_err_t se = ray_splay_save(empty_tbl, ptdir, sym_path);
            if (se != RAY_OK) {
                err = se;
                snprintf(cause, sizeof(cause), "table %s (partition %s): save failed",
                         tname, part_dirs[p]);
                break;
            }
            fixed[p] = 1;
        }
        if (empty_tbl) ray_release(empty_tbl);
    }

    /* Build the sorted SYM result of fixed partition names. */
    ray_t* result = NULL;
    if (err == RAY_OK) {
        int64_t nfixed = 0;
        for (int64_t p = 0; p < part_count; p++) if (fixed[p]) nfixed++;
        result = ray_vec_new(RAY_SYM, nfixed);
        if (!result || RAY_IS_ERR(result)) { err = RAY_ERR_OOM; }
        else {
            result->len = nfixed;
            int64_t* out = (int64_t*)ray_data(result);
            int64_t w = 0;
            for (int64_t p = 0; p < part_count; p++)
                if (fixed[p]) out[w++] = ray_sym_intern(part_dirs[p], strlen(part_dirs[p]));
        }
    }

    for (int64_t i = 0; i < all_count; i++) ray_free_raw(all[i]);
    ray_free_raw(all);
    for (int64_t p = 0; p < part_count; p++) ray_free_raw(part_dirs[p]);
    ray_free_raw(part_dirs);
    ray_free_raw(fixed);

    if (err != RAY_OK) {
        if (result && !RAY_IS_ERR(result)) ray_release(result);
        if (cause[0])
            return ray_error(ray_err_code_str(err), "parted %s: fill failed: %s",
                             db_root, cause);
        return ray_error(ray_err_code_str(err), "parted %s: fill failed", db_root);
    }
    return result;
}
