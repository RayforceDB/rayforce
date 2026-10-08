#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
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
#include "core/runtime.h"
#include "core/pool.h"
#include "mem/sys.h"
#include "store/col.h"
#include "store/fileio.h"
#include "store/serde.h"
#include "table/sym.h"
#include "table/table.h"
#include "table/domain.h"
#include "ops/idxop.h"
#include "core/profile.h"
#include "io/csv.h"      /* ray_csv_hash_upgrade_check — shared index policy */
#include "vec/str.h"
#include "lang/format.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>

/* --------------------------------------------------------------------------
 * Splayed table: directory of column files + .d schema file
 *
 * Format:
 *   dir/.d        — RAY_STR vector of column names (self-describing)
 *   dir/<colname> — column file per column; RAY_SYM column cells are
 *                   POSITIONS in the table's symfile (sym-domain spec)
 *
 * No symlink check: local-trust file format; path traversal checks
 * (rejecting '/', '\\', '..', leading '.') cover main attack vector.
 * -------------------------------------------------------------------------- */

/* True when the table has at least one top-level RAY_SYM column — the
 * data that encodes as symfile positions.  SYM data NESTED in list
 * columns serializes as self-contained strings (store/col.c recursive
 * format) and needs no symfile. */
static bool table_has_sym_cols(ray_t* tbl) {
    int64_t nc = ray_table_ncols(tbl);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col && !RAY_IS_ERR(col) && col->type == RAY_SYM) return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * ray_splay_save — save a table to a splayed table directory
 * -------------------------------------------------------------------------- */

/* True when `name` (len bytes) names a column of `tbl` that passes the
 * on-disk name-safety filter. */
static bool table_has_col_named(ray_t* tbl, const char* name, size_t len) {
    int64_t nc = ray_table_ncols(tbl);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* na = ray_sym_str(ray_table_col_name(tbl, c));
        if (!na) continue;
        if (ray_str_len(na) == len && memcmp(ray_str_ptr(na), name, len) == 0)
            return true;
    }
    return false;
}

static bool splay_col_name_safe(const char* name, size_t name_len) {
    return name_len > 0 && name[0] != '.' &&
           !memchr(name, '/', name_len) &&
           !memchr(name, '\\', name_len) &&
           !memchr(name, '\0', name_len);
}

static ray_err_t splay_validate_persisted_names(ray_t* tbl) {
    int64_t nc = ray_table_ncols(tbl);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* a = ray_sym_str(ray_table_col_name(tbl, c));
        if (!a || RAY_IS_ERR(a)) continue;
        const char* an = ray_str_ptr(a);
        size_t alen = ray_str_len(a);
        if (!splay_col_name_safe(an, alen)) return RAY_ERR_DOMAIN;
        for (int64_t j = c + 1; j < nc; j++) {
            ray_t* b = ray_sym_str(ray_table_col_name(tbl, j));
            if (!b || RAY_IS_ERR(b)) continue;
            const char* bn = ray_str_ptr(b);
            size_t blen = ray_str_len(b);
            if (!splay_col_name_safe(bn, blen)) return RAY_ERR_DOMAIN;
            if (alen == blen && memcmp(an, bn, alen) == 0)
                return RAY_ERR_DOMAIN;
        }
    }
    return RAY_OK;
}

/* Remove regular files in `dir` that are not part of the just-written
 * table: not a dotfile (".d", ".sym", ".sym.lk"), not a current column.
 * Runs after the .d commit so a stale wider-schema file can never shadow
 * a column (the historical "error: corrupt on re-set" bug).  The symfile
 * and its lock are dotfiles (".sym"/".sym.lk"), so the leading-'.' skip
 * already protects them — and a column legitimately named "sym" is now
 * swept like any other column when it leaves the schema. */
static void splay_sweep_stale(ray_t* tbl, const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        const char* n = ent->d_name;
        if (n[0] == '.') continue; /* ".", "..", ".d", ".sym", ".sym.lk" */
        size_t nlen = strlen(n);
        if (table_has_col_named(tbl, n, nlen)) continue;
        /* `<col>.link` sidecars (store/col.c) belong to their column: keep
         * them while the column is current; ray_col_save already removes a
         * stale sidecar when the column itself is rewritten without a link. */
        if (nlen > 5 && memcmp(n + nlen - 5, ".link", 5) == 0 &&
            table_has_col_named(tbl, n, nlen - 5))
            continue;
        char p[1024];
        int pl = snprintf(p, sizeof(p), "%s/%s", dir, n);
        if (pl <= 0 || (size_t)pl >= sizeof(p)) continue;
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        unlink(p);
    }
    closedir(d);
}

/* A published splayed table may contain more than one physical generation.
 * The small text manifest is the only mutable table-level pointer: data files
 * are written below .generations/<name> first, then .current is replaced with
 * one filesystem rename.  Readers that predate this format continue to use
 * the legacy directory when .current is absent. */
static _Atomic unsigned long splay_generation_seq;

static ray_err_t splay_current_dir(const char* dir, char* out, size_t out_sz,
                                   bool* active) {
    char manifest[1024];
    int n = snprintf(manifest, sizeof(manifest), "%s/.current", dir);
    if (n < 0 || (size_t)n >= sizeof(manifest) || !out || !active)
        return RAY_ERR_RANGE;
    *active = false;

    FILE* f = fopen(manifest, "rb");
    if (!f) {
        if (errno == ENOENT) return RAY_OK; /* legacy splayed directory */
        return RAY_ERR_IO;
    }

    char rel[512];
    size_t len = fread(rel, 1, sizeof(rel), f);
    bool failed = ferror(f) != 0;
    int closed = fclose(f);
    if (failed || closed != 0 || len == 0 || len >= sizeof(rel)) {
        return RAY_ERR_CORRUPT;
    }
    if (rel[len - 1] == '\n') len--;
    rel[len] = '\0';
    if (strncmp(rel, ".generations/", 13) != 0 ||
        len <= 13 || memchr(rel, '\0', len))
        return RAY_ERR_CORRUPT;
    for (size_t i = 13; i < len; i++)
        if (!((rel[i] >= '0' && rel[i] <= '9') || rel[i] == 'g' || rel[i] == '-'))
            return RAY_ERR_CORRUPT;

    n = snprintf(out, out_sz, "%s/%s", dir, rel);
    if (n < 0 || (size_t)n >= out_sz) return RAY_ERR_RANGE;
    *active = true;
    return RAY_OK;
}

ray_err_t ray_splay_resolve_dir(const char* dir, char* out, size_t out_sz) {
    if (!dir || !out || !out_sz) return RAY_ERR_IO;
    bool active;
    ray_err_t err = splay_current_dir(dir, out, out_sz, &active);
    if (err != RAY_OK || active) return err;
    int n = snprintf(out, out_sz, "%s", dir);
    return n < 0 || (size_t)n >= out_sz ? RAY_ERR_RANGE : RAY_OK;
}

static ray_err_t splay_publish_generation(const char* dir, const char* gen,
                                          bool durable) {
    char manifest[1024], tmp[1024];
    int n = snprintf(manifest, sizeof(manifest), "%s/.current", dir);
    if (n < 0 || (size_t)n >= sizeof(manifest)) return RAY_ERR_RANGE;
    /* The exclusively created generation owns this temporary file. */
    n = snprintf(tmp, sizeof(tmp), "%s/%s/.current.tmp", dir, gen);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return RAY_ERR_RANGE;

    FILE* f = fopen(tmp, "wb");
    if (!f) return RAY_ERR_IO;
    size_t len = strlen(gen);
    bool ok = fwrite(gen, 1, len, f) == len && fputc('\n', f) != EOF;
    if (ok && fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        (void)remove(tmp);
        return RAY_ERR_IO;
    }

    ray_fd_t fd = ray_file_open(tmp, RAY_OPEN_READ | RAY_OPEN_WRITE);
    if (fd == RAY_FD_INVALID) {
        (void)remove(tmp);
        return RAY_ERR_IO;
    }
    ray_err_t err = durable ? ray_file_sync(fd) : RAY_OK;
    ray_file_close(fd);
    if (err != RAY_OK || ray_file_rename(tmp, manifest) != RAY_OK) {
        (void)remove(tmp);
        return RAY_ERR_IO;
    }
    return durable ? ray_file_sync_dir(manifest) : RAY_OK;
}

static ray_err_t splay_has_file(const char* dir, const char* name, bool* exists) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return RAY_ERR_RANGE;
    struct stat st;
    *exists = stat(path, &st) == 0;
    return *exists || errno == ENOENT ? RAY_OK : RAY_ERR_IO;
}

static void splay_remove_tree_best_effort(const char* path) {
    DIR* d = opendir(path);
    if (!d) {
        (void)unlink(path);
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(d))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) continue;
        struct stat st;
#ifdef RAY_OS_WINDOWS
        if (stat(child, &st) != 0) continue;
#else
        if (lstat(child, &st) != 0) continue;
#endif
        if (S_ISDIR(st.st_mode)) splay_remove_tree_best_effort(child);
        else (void)unlink(child);
    }
    closedir(d);
    (void)rmdir(path);
}

static bool splay_generation_is_current(const ray_splay_write_t* write) {
    char current[1024];
    bool active = false;
    return write && write->staged && write->dir[0] &&
           splay_current_dir(write->root, current, sizeof(current), &active) == RAY_OK &&
           active && strcmp(current, write->dir) == 0;
}

/* The first write of a table is made in its directory; its columns are the
 * table until the first publish.  From then on readers resolve .current, so
 * the root schema is renamed out of the way (older binaries then fail loudly
 * instead of reading obsolete files) and the columns it names are removed,
 * with their .link sidecars.  Only what the schema names goes: the root also
 * holds the symfile, the lock file, the generations and, for a table written
 * into a database root, the partitions.  .legacy.d goes last, once every
 * file it names is gone, so a cleanup cut short is finished by the next
 * publish. */
static void splay_retire_legacy_schema(const char* root) {
    char schema[1024], retired[1024];
    int n = snprintf(schema, sizeof(schema), "%s/.d", root);
    int m = snprintf(retired, sizeof(retired), "%s/.legacy.d", root);
    if (n < 0 || (size_t)n >= sizeof(schema) ||
        m < 0 || (size_t)m >= sizeof(retired))
        return;
    if (access(schema, F_OK) == 0) {
        (void)unlink(retired);
        if (rename(schema, retired) != 0) {
            (void)unlink(schema);
            return;
        }
    }
    if (access(retired, F_OK) != 0) return;

    ray_t* names = ray_col_load(retired);
    if (!names || RAY_IS_ERR(names)) {
        if (names) ray_error_free(names);
        return;
    }
    bool all_gone = names->type == RAY_STR;
    for (int64_t c = 0; all_gone && c < names->len; c++) {
        size_t len = 0;
        const char* name = ray_str_vec_get(names, c, &len);
        if (!name || !splay_col_name_safe(name, len)) {
            all_gone = false;
            break;
        }
        static const char* const suffix[] = {"", ".link"};
        for (size_t k = 0; k < sizeof(suffix) / sizeof(suffix[0]); k++) {
            char path[1100];
            int pn = snprintf(path, sizeof(path), "%s/%.*s%s", root,
                              (int)len, name, suffix[k]);
            if (pn < 0 || (size_t)pn >= sizeof(path)) { all_gone = false; break; }
            struct stat st;
#ifdef RAY_OS_WINDOWS
            if (stat(path, &st) != 0) continue;
#else
            if (lstat(path, &st) != 0) continue;
#endif
            if (S_ISDIR(st.st_mode)) continue;  /* not a column file */
            if (unlink(path) != 0) all_gone = false;
        }
    }
    ray_release(names);
    if (all_gone) (void)unlink(retired);
}

/* Every generation but the selected one goes.  A reader that resolved an
 * older one and has not opened all its files yet finds one missing, resolves
 * again and reads the selected generation (see splay_load_impl); files it
 * already mapped stay readable until unmapped.  Removal is best-effort: where
 * an open file cannot be deleted (Windows), the next publish tries again. */
static void splay_prune_generations(const char* root, const char* current) {
    char generations[1024];
    int n = snprintf(generations, sizeof(generations), "%s/.generations", root);
    if (n < 0 || (size_t)n >= sizeof(generations)) return;
    DIR* d = opendir(generations);
    if (!d) return;

    struct dirent* entry;
    while ((entry = readdir(d))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char rel[256], full[1024];
        int rn = snprintf(rel, sizeof(rel), ".generations/%s", entry->d_name);
        int fn = snprintf(full, sizeof(full), "%s/%s", root, rel);
        if (rn < 0 || (size_t)rn >= sizeof(rel) ||
            fn < 0 || (size_t)fn >= sizeof(full))
            continue;
        if (current && strcmp(rel, current) == 0) continue;
        splay_remove_tree_best_effort(full);
    }
    closedir(d);
}

static ray_err_t splay_validate_save(ray_t* tbl, const char* dir,
                                     const char* sym_path) {
    if (!tbl || RAY_IS_ERR(tbl) || tbl->type != RAY_TABLE) return RAY_ERR_TYPE;
    if (!dir) return RAY_ERR_IO;

    ray_err_t name_err = splay_validate_persisted_names(tbl);
    if (name_err != RAY_OK) return name_err;

    /* Validate every column graph before mkdir, sym-domain growth, or the
     * first per-column atomic rename.  Without this pass, an unsupported
     * value in a later recursive LIST cell can return NYI after earlier
     * columns have already replaced an existing committed generation. */
    int64_t preflight_ncols = ray_table_ncols(tbl);
    for (int64_t c = 0; c < preflight_ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        ray_err_t err = ray_col_save_preflight(col);
        if (err != RAY_OK) return err;
    }

    /* Symfile/column collision guard.  A column is written as `dir/<name>`;
     * the symfile (and its `<sym>.lk` lock) is written at `sym_path`.  A
     * column whose file lands on the symfile path — or its lock path —
     * would clobber it, so reject loudly BEFORE writing anything
     * (MUST-prohibit, not silent skip).
     *
     * This can only happen when a symfile is actually written: the table
     * has SYM columns AND the symfile lives directly in `dir`.  The default
     * symfile is the dotfile ".sym", which no column can be named (dot-led
     * names are rejected up-front by splay_validate_persisted_names), so the
     * default convention never collides —
     * a plain column named "sym" round-trips fine (issue #280).  But an
     * explicit sym_path (3-arg .db.splayed.set) may name anything, so the
     * guard matches the resolved symfile path, not the literal "sym". */
    if (sym_path && table_has_sym_cols(tbl)) {
        size_t dlen = strlen(dir);
        while (dlen > 1 && dir[dlen - 1] == '/') dlen--;
        const char* slash = strrchr(sym_path, '/');
        const char* base  = slash ? slash + 1 : sym_path;
        size_t plen = slash ? (size_t)(slash - sym_path) : 0; /* parent dir */
        while (plen > 1 && sym_path[plen - 1] == '/') plen--;
        if (plen == dlen && memcmp(sym_path, dir, dlen) == 0) {
            size_t blen = strlen(base);
            int64_t nc = ray_table_ncols(tbl);
            for (int64_t c = 0; c < nc; c++) {
                ray_t* na = ray_sym_str(ray_table_col_name(tbl, c));
                if (!na || RAY_IS_ERR(na)) continue;
                const char* n = ray_str_ptr(na);
                size_t nlen = ray_str_len(na);
                if ((nlen == blen && memcmp(n, base, blen) == 0) ||
                    (nlen == blen + 3 && memcmp(n, base, blen) == 0 &&
                     memcmp(n + blen, ".lk", 3) == 0))
                    return RAY_ERR_RESERVED;
            }
        }
    }

    return RAY_OK;
}

static ray_err_t splay_write_table_impl(ray_t* tbl, const char* dir,
                                        const char* sym_path, bool durable,
                                        bool flush_sym, bool build_indexes) {
    ray_err_t validation = splay_validate_save(tbl, dir, sym_path);
    if (validation != RAY_OK) return validation;
    /* Create directory and any missing parents (mkdir -p semantics).
     * Required for partitioned layouts like "/db/2024.01.01/t/" where the
     * caller hasn't pre-created the date partition. */
    ray_err_t mkdir_err = ray_mkdir_p(dir);
    if (mkdir_err != RAY_OK) return mkdir_err;

    /* 1. Symfile FIRST — column data must never reference positions the
     *    symfile doesn't persist (crash ordering: sym → columns → .d).
     *
     *    The v1 algorithm, restored (sym-domain spec, "Save"): open or
     *    create the target symfile's domain, distinct-merge every SYM
     *    column's vocabulary into it (append-only — existing positions
     *    stay; the find-or-append rides the cell walk), flush, then
     *    write columns re-encoded as positions during the column pass.
     *
     *    Skipped entirely for symbol-free tables: nothing to enumerate
     *    (spec: no-symbol-columns exemption). */
    ray_sym_domain_t* dom = NULL;
    if (table_has_sym_cols(tbl)) {
        if (!sym_path) return RAY_ERR_DOMAIN; /* SYM columns need a symfile */
        dom = ray_sym_domain_open_or_create(sym_path);
        if (!dom) return RAY_ERR_IO;

        /* Empty-vocabulary seeding: a 0-row SYM table merges nothing
         * below, and ray_sym_domain_flush no-ops at count == disk_count
         * (0 == 0 for a freshly created domain) — no symfile would be
         * written and the table would be unloadable (loud "sym" on
         * load).  Seed the position-0 "" up front so the flush always
         * persists a count>=1 file whenever the table has SYM columns
         * (this also satisfies the position-0 "" invariant; for a
         * non-empty vocabulary the intern is a find hit, a no-op). */
        if (ray_sym_domain_count(dom) == 0 &&
            ray_sym_domain_intern(dom, "", 0) != 0) {
            ray_sym_domain_release(dom);
            return RAY_ERR_OOM;
        }

        int64_t nc = ray_table_ncols(tbl);
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(tbl, c);
            if (!col || RAY_IS_ERR(col) || col->type != RAY_SYM) continue;
            if (ray_sym_vec_domain(col) == dom) continue; /* already merged */
            for (int64_t i = 0; i < col->len; i++) {
                ray_t* s = ray_sym_vec_cell(col, i);
                if (!s) { ray_sym_domain_release(dom); return RAY_ERR_CORRUPT; }
                if (ray_sym_domain_intern(dom, ray_str_ptr(s),
                                          ray_str_len(s)) < 0) {
                    ray_sym_domain_release(dom);
                    return RAY_ERR_OOM;
                }
            }
        }

        ray_err_t sym_err = flush_sym ? ray_sym_domain_flush(dom, durable) : RAY_OK;
        if (sym_err != RAY_OK) {
            ray_sym_domain_release(dom);
            return sym_err;
        }
    }

    int64_t ncols = ray_table_ncols(tbl);

    /* 2. Write the column files in the caller's unpublished directory. */
    ray_t* schema = ray_vec_new(RAY_STR, ncols > 0 ? ncols : 1);
    if (!schema || RAY_IS_ERR(schema)) {
        if (schema) ray_release(schema);
        if (dom) ray_sym_domain_release(dom);
        return RAY_ERR_OOM;
    }
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (!col) continue;
        ray_t* name_atom = ray_sym_str(ray_table_col_name(tbl, c));
        if (!name_atom) continue;
        const char* name = ray_str_ptr(name_atom);
        size_t name_len  = ray_str_len(name_atom);
        /* Name safety (dot-led, '/', '\\', NUL) is enforced up-front by
         * splay_validate_persisted_names() at the top of this function, so
         * every name reaching this write loop is already safe. */

        char path[1024];
        int path_len = snprintf(path, sizeof(path), "%s/%.*s", dir,
                                (int)name_len, name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            ray_release(schema);
            if (dom) ray_sym_domain_release(dom);
            return RAY_ERR_RANGE;
        }
        ray_err_t err = (col->type == RAY_SYM)
            ? ray_col_save_sym_encoded(col, path, dom, durable)
            : (durable ? ray_col_save(col, path)
                       : ray_col_save_bulk(col, path));
        if (err != RAY_OK) {
            /* No new .d or .current is published on failure. */
            ray_release(schema);
            if (dom) ray_sym_domain_release(dom);
            return err;
        }
        schema = ray_str_vec_append(schema, name, name_len);
        if (!schema || RAY_IS_ERR(schema)) {
            if (schema) ray_release(schema);
            if (dom) ray_sym_domain_release(dom);
            return RAY_ERR_OOM;
        }
    }
    if (dom) ray_sym_domain_release(dom);

    /* Indexes belong to this generation and must precede publication. They are
     * rebuildable accelerators; ray_col_append_index writes the marker last and
     * intentionally does not fsync them again after ray_col_save fsyncs data. */
    if (build_indexes)
        ray_splay_build_indexes(dir, tbl);

    /* 3. .d LAST — the commit marker. */
    {
        char path[1024];
        int path_len = snprintf(path, sizeof(path), "%s/.d", dir);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            ray_release(schema);
            return RAY_ERR_RANGE;
        }
        ray_err_t err = durable ? ray_col_save(schema, path)
                                : ray_col_save_bulk(schema, path);
        ray_release(schema);
        if (err != RAY_OK) return err;
    }

    /* 4. Sweep files that are no longer part of the table. */
    splay_sweep_stale(tbl, dir);

    return RAY_OK;
}

ray_err_t ray_splay_write_table(ray_t* tbl, const char* dir,
                                 const char* sym_path, bool durable) {
    return splay_write_table_impl(tbl, dir, sym_path, durable, true, true);
}

#ifdef RAY_OS_WINDOWS
#define SPLAY_IS_SEP(c) ((c) == '/' || (c) == '\\')
#else
#define SPLAY_IS_SEP(c) ((c) == '/')
#endif

/* Record the shallowest component of `root` that does not exist yet, so a
 * failed first write can remove the directories it created.  Nothing is
 * recorded when a ".." follows it: root would then lie outside it. */
static void splay_find_missing(const char* root, char* out, size_t out_sz) {
    out[0] = '\0';
    size_t len = strlen(root);
    if (len >= out_sz) return;
    char buf[1024];
    memcpy(buf, root, len + 1);
    struct stat st;
    size_t i = 1;
    for (; i <= len; i++) {
        if (i < len && !SPLAY_IS_SEP(buf[i])) continue;
        char saved = buf[i];
        buf[i] = '\0';
        bool missing = stat(buf, &st) != 0 && errno == ENOENT;
        buf[i] = saved;
        if (missing) break;
    }
    if (i > len) return;
    for (size_t j = i; j < len; j++)
        if (SPLAY_IS_SEP(root[j]) && root[j + 1] == '.' && root[j + 2] == '.' &&
            (root[j + 3] == '\0' || SPLAY_IS_SEP(root[j + 3])))
            return;
    memcpy(out, root, i);
    out[i] = '\0';
}

/* An in-place write goes to a directory that may have been there before this
 * writer: an operator pre-made it, or an earlier first write was killed and
 * left its lock file and partial columns.  Nothing says what else it holds
 * (a database root has no .d either), so a failed write removes only what
 * this writer produced: every entry whose inode was not in the directory
 * when the lock was taken.  Column files are renamed into place, so a
 * leftover this writer rewrote has a new inode and goes with the rest.
 * Taken under the lock, right after it. */
static int splay_ino_cmp(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

static void splay_snapshot_root(ray_splay_write_t* write) {
    write->before = NULL;
    write->nbefore = 0;
    write->before_known = false;
    DIR* d = opendir(write->root);
    if (!d) return;
    size_t cap = 0;
    bool ok = true;
    struct dirent* entry;
    while ((entry = readdir(d))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[1100];
        int n = snprintf(child, sizeof(child), "%s/%s", write->root, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) { ok = false; break; }
        struct stat st;
        if (stat(child, &st) != 0) {
            if (errno == ENOENT) continue;      /* removed while listing */
            ok = false;
            break;
        }
        if (write->nbefore == cap) {
            cap = cap ? cap * 2 : 16;
            uint64_t* grown = ray_realloc_raw(write->before, cap * sizeof(uint64_t));
            if (!grown) { ok = false; break; }
            write->before = grown;
        }
        write->before[write->nbefore++] = (uint64_t)st.st_ino;
    }
    closedir(d);
    if (ok && write->nbefore > 1)
        qsort(write->before, write->nbefore, sizeof(uint64_t), splay_ino_cmp);
    write->before_known = ok;
}

static bool splay_was_there(const ray_splay_write_t* write, uint64_t ino) {
    size_t lo = 0, hi = write->nbefore;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (write->before[mid] < ino) lo = mid + 1;
        else hi = mid;
    }
    return lo < write->nbefore && write->before[lo] == ino;
}

/* A failed first write removes the table directory and every directory begin
 * created above it, so no partition without a schema is left behind.  The
 * lock holder first empties the table directory of what it produced, all
 * but its lock file: everything, when begin created the directory; else
 * what the snapshot does not know.  Without a snapshot nothing goes. */
static void splay_clear_created(const ray_splay_write_t* write) {
    bool all = write->created[0];
    if (!all && !write->before_known) return;
    DIR* d = opendir(write->root);
    if (!d) return;
    struct dirent* entry;
    while ((entry = readdir(d))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, ".write.lock") == 0)
            continue;
        char child[1100];
        int n = snprintf(child, sizeof(child), "%s/%s", write->root, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) continue;
        if (!all) {
            struct stat st;
            if (stat(child, &st) != 0 || splay_was_there(write, (uint64_t)st.st_ino))
                continue;
        }
        splay_remove_tree_best_effort(child);
    }
    closedir(d);
}

/* Nothing but the lock file left? */
static bool splay_root_is_bare(const ray_splay_write_t* write) {
    DIR* d = opendir(write->root);
    if (!d) return false;
    bool bare = true;
    struct dirent* entry;
    while (bare && (entry = readdir(d))) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
            strcmp(entry->d_name, ".write.lock") != 0)
            bare = false;
    }
    closedir(d);
    return bare;
}

/* Then the lock file goes, last: a waiter on it finds it unlinked and starts
 * over, and a writer that creates a new one keeps the directory, which is
 * then removed only while empty, like every directory above it.  A directory
 * that still holds anything else keeps its lock file too.  Without the lock
 * (begin failed before taking it) nothing but empty directories goes, and
 * the lock file only when nobody can hold it.  Directories above a table
 * directory that was there before the write are not this writer's. */
static void splay_remove_created(const ray_splay_write_t* write, bool unlink_lock) {
    if (!splay_root_is_bare(write)) return;
    if (unlink_lock) {
        char lock[1100];
        int n = snprintf(lock, sizeof(lock), "%s/.write.lock", write->root);
        if (n > 0 && (size_t)n < sizeof(lock)) (void)unlink(lock);
    }
    if (rmdir(write->root) != 0 || !write->created[0]) return;
    size_t stop = strlen(write->created);
    char path[1024];
    memcpy(path, write->root, strlen(write->root) + 1);
    for (;;) {
        size_t k = strlen(path);
        while (k > 0 && !SPLAY_IS_SEP(path[k - 1])) k--;
        if (k == 0 || k - 1 < stop) break;
        path[k - 1] = '\0';
        if (rmdir(path) != 0) break;
    }
}

ray_err_t ray_splay_write_finish(ray_splay_write_t* write, ray_err_t result,
                                  bool durable) {
    if (result == RAY_OK && write->staged) {
        /* ray_file_sync_dir syncs the PARENT of its argument. */
        char schema[1100];
        snprintf(schema, sizeof(schema), "%s/.d", write->dir);
        if (durable && (ray_file_sync_dir(schema) != RAY_OK ||
                        ray_file_sync_dir(write->dir) != RAY_OK))
            result = RAY_ERR_IO;
        if (result == RAY_OK)
            result = splay_publish_generation(write->root, write->generation, durable);
        if (result == RAY_OK) {
            splay_retire_legacy_schema(write->root);
            splay_prune_generations(write->root, write->generation);
        }
    }
    if (result != RAY_OK && write->staged && write->dir[0] &&
        !splay_generation_is_current(write)) {
        splay_remove_tree_best_effort(write->dir);
    }
    /* The lock holder clears what it produced in the table directory;
     * without the lock only the empty directories begin created go.  The
     * schema check is repeated here, under the lock: `created` was recorded
     * before the lock was taken, and a stat error in begin must not clear a
     * table another writer published meanwhile. */
    bool locked = write->locked;
    bool drop_created = result != RAY_OK && !write->staged &&
                        (locked || write->created[0]);
    if (drop_created && locked) {
        bool schema = false, current = false;
        if (splay_has_file(write->root, ".d", &schema) != RAY_OK ||
            splay_has_file(write->root, ".current", &current) != RAY_OK ||
            schema || current)
            drop_created = false;
        else
            splay_clear_created(write);
    }
    if (write->before) {
        ray_free_raw(write->before);
        write->before = NULL;
        write->nbefore = 0;
    }
    bool unlink_lock = locked || write->unlink_lock;
#ifndef RAY_OS_WINDOWS
    /* Under the lock; a writer waiting on it then finds its lock file
     * unlinked and starts over (see ray_splay_write_begin). */
    if (drop_created) splay_remove_created(write, unlink_lock);
#endif
    if (write->lock != RAY_FD_INVALID) {
        if (write->locked) (void)ray_file_unlock(write->lock);
        ray_file_close(write->lock);
        write->lock = RAY_FD_INVALID;
        write->locked = false;
    }
#ifdef RAY_OS_WINDOWS
    /* Windows cannot delete the open lock file. */
    if (drop_created) splay_remove_created(write, unlink_lock);
#endif
    return result;
}

ray_err_t ray_splay_write_begin(const char* dir, ray_splay_write_t* write) {
    memset(write, 0, sizeof(*write));
    write->lock = RAY_FD_INVALID;
    if (!dir || !*dir) return RAY_ERR_IO;
    int n = snprintf(write->root, sizeof(write->root), "%s", dir);
    if (n < 0 || (size_t)n >= sizeof(write->root)) return RAY_ERR_RANGE;
    size_t len = strlen(write->root);
    while (len > 1 && write->root[len - 1] == '/') write->root[--len] = '\0';

    char path[1024];
    n = snprintf(path, sizeof(path), "%s/.write.lock", write->root);
    if (n < 0 || (size_t)n >= sizeof(path)) return RAY_ERR_RANGE;
    ray_err_t err = RAY_ERR_IO;
    for (int attempt = 0; attempt < 8 && !write->locked; attempt++) {
        splay_find_missing(write->root, write->created, sizeof(write->created));
        err = ray_mkdir_p(write->root);
        if (err != RAY_OK) {
#ifndef RAY_OS_WINDOWS
            /* A failed first write of a sibling removed a new parent
             * between our mkdirs: create it again. */
            if (errno == ENOENT) continue;
#endif
            return ray_splay_write_finish(write, err, false);
        }
        write->lock = ray_file_open(path, RAY_OPEN_READ | RAY_OPEN_WRITE | RAY_OPEN_CREATE);
        if (write->lock == RAY_FD_INVALID) {
            err = RAY_ERR_IO;
            if (errno == ENOENT) continue;
            return ray_splay_write_finish(write, err, false);
        }
        err = ray_file_lock_ex(write->lock);
        if (err != RAY_OK) {
            /* A wait cut short (EINTR) is retried without waiting.  When
             * that fails for any reason but another holder, locking itself
             * is broken, so nobody holds this lock file either and a
             * directory created for it can go with it (see finish);
             * otherwise it stays, as it would admit a second writer. */
            if (ray_file_trylock_ex(write->lock) == RAY_OK) {
                err = RAY_OK;
            } else {
#ifndef RAY_OS_WINDOWS
                write->unlink_lock = errno != EWOULDBLOCK && errno != EAGAIN &&
                                     write->created[0];
#endif
                return ray_splay_write_finish(write, err, false);
            }
        }
        write->locked = true;
#ifndef RAY_OS_WINDOWS
        /* A failed first write removes the directory with its lock file
         * while holding the lock; a waiter then holds a lock on a file
         * nobody else can find.  Drop it and start over. */
        struct stat held, named;
        if (fstat(write->lock, &held) != 0 || stat(path, &named) != 0 ||
            held.st_ino != named.st_ino || held.st_dev != named.st_dev) {
            (void)ray_file_unlock(write->lock);
            ray_file_close(write->lock);
            write->lock = RAY_FD_INVALID;
            write->locked = false;
            err = RAY_ERR_IO;
        }
#endif
    }
    if (!write->locked) return ray_splay_write_finish(write, err, false);

    bool schema, current;
    err = splay_has_file(write->root, ".d", &schema);
    if (err == RAY_OK) err = splay_has_file(write->root, ".current", &current);
    if (err != RAY_OK) return ray_splay_write_finish(write, err, false);
    write->staged = schema || current;
    if (!write->staged) {
        splay_snapshot_root(write);
        memcpy(write->dir, write->root, len + 1);
        return RAY_OK;
    }
    n = snprintf(path, sizeof(path), "%s/.generations", write->root);
    if (n < 0 || (size_t)n >= sizeof(path))
        return ray_splay_write_finish(write, RAY_ERR_RANGE, false);
    err = ray_mkdir_p(path);
    if (err != RAY_OK) return ray_splay_write_finish(write, err, false);

    for (;;) {
        unsigned long seq = atomic_fetch_add(&splay_generation_seq, 1);
        snprintf(write->generation, sizeof(write->generation), ".generations/g-%lu-%lu-%lu",
                 (unsigned long)time(NULL), (unsigned long)getpid(), seq);
        n = snprintf(write->dir, sizeof(write->dir), "%s/%s", write->root, write->generation);
        if (n < 0 || (size_t)n >= sizeof(write->dir))
            return ray_splay_write_finish(write, RAY_ERR_RANGE, false);
#ifdef RAY_OS_WINDOWS
        if (CreateDirectoryA(write->dir, NULL)) break;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
#else
        if (mkdir(write->dir, 0755) == 0) break;
        if (errno != EEXIST)
#endif
            return ray_splay_write_finish(write, RAY_ERR_IO, false);
        /* Never reuse an existing generation, including after PID reuse. */
    }
    return RAY_OK;
}

static ray_err_t splay_save_impl(ray_t* tbl, const char* dir,
                                 const char* sym_path, bool durable) {
    ray_err_t err = splay_validate_save(tbl, dir, sym_path);
    if (err != RAY_OK) return err;
    ray_splay_write_t write;
    err = ray_splay_write_begin(dir, &write);
    if (err != RAY_OK) return err;
    err = ray_splay_write_table(tbl, write.dir, sym_path, durable);
    return ray_splay_write_finish(&write, err, durable);
}

ray_err_t ray_splay_save(ray_t* tbl, const char* dir, const char* sym_path) {
    return splay_save_impl(tbl, dir, sym_path, true);
}

ray_err_t ray_splay_save_bulk(ray_t* tbl, const char* dir, const char* sym_path) {
    return splay_save_impl(tbl, dir, sym_path, false);
}

ray_err_t ray_splay_save_staged_bulk(ray_t* tbl, const char* dir, const char* sym_path) {
    return splay_write_table_impl(tbl, dir, sym_path, false, false, false);
}

/* --------------------------------------------------------------------------
 * splay_load_impl — shared implementation for ray_splay_load / ray_read_splayed
 *
 * When use_mmap is false, columns are loaded via ray_col_load (buddy copy).
 * When use_mmap is true, columns are loaded via ray_col_mmap (zero-copy).
 * The .d schema is always loaded via ray_col_load (small, buddy copy).
 * -------------------------------------------------------------------------- */

static ray_t* splay_load_dom_impl(const char* dir, ray_sym_domain_t* dom,
                                  bool use_mmap) {
    if (!dir) return ray_error("io", NULL);
    bool trace = getenv("RAY_CSV_TRACE") != NULL;
    if (trace)
        fprintf(stderr, "splayed.get: dir=%s mmap=%d\n", dir, use_mmap ? 1 : 0);

    /* Load .d schema */
    char path[1024];
    int path_len = snprintf(path, sizeof(path), "%s/.d", dir);
    if (path_len < 0 || (size_t)path_len >= sizeof(path))
        return ray_error("range", "splayed %s: .d schema path exceeds %zu-byte buffer", dir, sizeof(path));
    ray_t* schema = ray_col_load(path);
    if (!schema || RAY_IS_ERR(schema)) {
        if (trace)
            fprintf(stderr, "splayed.get: schema load failed path=%s err=%s\n",
                    path, schema && RAY_IS_ERR(schema) ? ray_err_code(schema) : "io");
        /* .d failures from ray_col_load are bare (code, no message) —
         * name the directory so a missing/corrupt table is locatable. */
        char codebuf[8];
        const char* code = schema && RAY_IS_ERR(schema) ? ray_err_code(schema) : "io";
        snprintf(codebuf, sizeof(codebuf), "%s", code);
        ray_error_free(schema);
        return ray_error(codebuf, "splayed %s: cannot read .d schema", dir);
    }

    if (schema->type != RAY_STR) {
        ray_release(schema);
        return ray_error("corrupt",
            "splayed %s: .d is not a string vector (pre-cleanup format?)", dir);
    }

    int64_t ncols = schema->len;

    ray_t* tbl = ray_table_new(ncols);
    if (!tbl || RAY_IS_ERR(tbl)) {
        ray_release(schema);
        return tbl;
    }

    /* Load each column */
    for (int64_t c = 0; c < ncols; c++) {
        size_t name_len = 0;
        const char* name = ray_str_vec_get(schema, c, &name_len);
        if (!name) {
            ray_release(schema);
            ray_release(tbl);
            return ray_error("corrupt", "splayed %s: unreadable .d entry %lld",
                             dir, (long long)c);
        }

        /* Reject names with path separators, traversal, or starting with '.'
         * — these indicate a corrupt/hand-tampered .d. */
        if (!splay_col_name_safe(name, name_len)) {
            ray_release(schema);
            ray_release(tbl);
            return ray_error("corrupt",
                "splayed %s: invalid column name in .d entry %lld",
                dir, (long long)c);
        }

        int64_t name_id = ray_sym_intern(name, name_len);

        path_len = snprintf(path, sizeof(path), "%s/%.*s", dir, (int)name_len, name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            ray_release(schema);
            ray_release(tbl);
            return ray_error("range", "splayed %s: column path for entry %lld exceeds %zu-byte buffer",
                             dir, (long long)c, sizeof(path));
        }

        /* Domain-attaching loaders: a SYM column resolves over the
         * table's symfile (dom); dom == NULL + SYM column is the loud
         * "sym" error from the col loader (spec: never silent
         * resolution against incidental state). */
        ray_t* col = use_mmap ? ray_col_mmap_splayed_dom(path, dom)
                              : ray_col_load_dom(path, dom);
        if (use_mmap && col && RAY_IS_ERR(col) &&
            strcmp(ray_err_code(col), "nyi") == 0) {
            /* ray_release on an error object is a no-op (rayforce.h:180);
             * must use ray_error_free to actually reclaim the error
             * before retrying with the non-mmap loader. */
            ray_error_free(col);
            col = ray_col_load_dom(path, dom);
        }
        if (!col || RAY_IS_ERR(col)) {
            if (trace)
                fprintf(stderr, "splayed.get: col load failed path=%s err=%s\n",
                        path, col && RAY_IS_ERR(col) ? ray_err_code(col) : "io");
            ray_release(schema);
            ray_release(tbl);
            /* Rich loader errors (the loud "sym", domain bounds, …) name
             * the path already and propagate verbatim; only a bare code
             * (e.g. plain "io" from a missing column file) gains the
             * dir + column context here. */
            if (col && ray_error_msg() != NULL) return col;
            char codebuf[8];
            snprintf(codebuf, sizeof(codebuf), "%s",
                     col && RAY_IS_ERR(col) ? ray_err_code(col) : "io");
            ray_error_free(col);
            return ray_error(codebuf, "splayed %s: column '%.*s' failed to load",
                             dir, (int)name_len, name);
        }

        if (c > 0 && col->len != ray_table_nrows(tbl)) {
            ray_t* err = ray_error("corrupt",
                "splayed %s: column '%.*s' has %lld rows, expected %lld "
                "(torn overwrite?)", dir, (int)name_len, name,
                (long long)col->len, (long long)ray_table_nrows(tbl));
            ray_release(col);
            ray_release(schema);
            ray_release(tbl);
            return err;
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
    return tbl;
}

/* Only the selected generation is kept, so one a reader resolved can be
 * removed before the reader has opened all its files.  A read that fails is
 * tried again while the table keeps resolving to a different generation;
 * a failure on the generation still selected is the table's own.  Each retry
 * follows a completed publish, so the reader is not counted out: a reader
 * that is slow to open its files (many columns, slow storage, a busy CPU)
 * would otherwise fail after a fixed number of saves.  It gives up once the
 * table has kept changing under it for SPLAY_RESOLVE_WAIT_NS (after at
 * least SPLAY_RESOLVE_ATTEMPTS attempts), or when interrupted. */
#define SPLAY_RESOLVE_WAIT_NS  (INT64_C(60) * 1000000000)
#define SPLAY_RESOLVE_ATTEMPTS 8

static bool splay_moved_on(const char* dir, const char* resolved) {
    char again[1024];
    return ray_splay_resolve_dir(dir, again, sizeof(again)) == RAY_OK &&
           strcmp(again, resolved) != 0;
}

/* Whether a read of `resolved` that failed (attempt `attempt` of a loop
 * started at `t0`) is tried again: 1 yes; 0 no, the failure is the table's
 * own; -1 interrupted; -2 the table kept changing for too long. */
static int splay_retry(const char* dir, const char* resolved, int attempt, int64_t t0) {
    if (!resolved[0] || !splay_moved_on(dir, resolved)) return 0;
    if (ray_interrupted()) return -1;
    if (attempt >= SPLAY_RESOLVE_ATTEMPTS &&
        ray_profile_now_ns() - t0 >= SPLAY_RESOLVE_WAIT_NS) return -2;
    return 1;
}

/* The error a reader that stopped retrying (splay_retry < 0) returns in
 * place of its last attempt's, which only reflects the race. */
static ray_t* splay_retry_error(int why, const char* dir, ray_t* last) {
    if (last && RAY_IS_ERR(last)) ray_error_free(last);
    if (why == -1) return ray_error("cancel", "interrupted");
    return ray_error("io", "splayed %s: the table kept changing while it was read", dir);
}

/* Resolve sym_path to a FILE domain.  Missing file → NULL domain with
 * RAY_OK (only an error if a SYM column is later encountered — the
 * symbol-free-table exemption must hold for reads too); existing but
 * unopenable/invalid file → loud error. */
static ray_t* splay_load_once(const char* dir, const char* sym_path,
                              bool use_mmap, char* resolved, size_t resolved_sz,
                              ray_sym_domain_t** held) {
    ray_err_t err = ray_splay_resolve_dir(dir, resolved, resolved_sz);
    if (err != RAY_OK)
        return ray_error(ray_err_code_str(err), "cannot resolve splayed generation");
    /* Resolve first: a newly published generation can reference symbols
     * appended since a previous domain open. Opening afterwards refreshes
     * the cached domain before any of those column codes are validated. */
    ray_sym_domain_t* dom = NULL;
    if (sym_path) {
        struct stat st;
        if (stat(sym_path, &st) == 0) {
            dom = ray_sym_domain_open(sym_path);
            /* The previous attempt's domain stays open until here, so a
             * retry extends it instead of reading the symfile again. */
            if (*held) { ray_sym_domain_release(*held); *held = NULL; }
            if (!dom)
                return ray_error("corrupt",
                    "symfile %s: unreadable or invalid (bad magic, torn "
                    "record, or missing \"\" at position 0)", sym_path);
        }
    }
    /* An earlier attempt's domain not replaced above (no symfile this
     * time) is let go here, before *held is overwritten. */
    if (*held) ray_sym_domain_release(*held);
    ray_t* tbl = splay_load_dom_impl(resolved, dom, use_mmap);
    *held = dom;   /* released by the caller; columns hold their own refs */
    return tbl;
}

static ray_t* splay_load_impl(const char* dir, const char* sym_path,
                              bool use_mmap) {
    char resolved[1024];
    ray_sym_domain_t* held = NULL;
    int64_t t0 = ray_profile_now_ns();
    ray_t* tbl;
    for (int attempt = 1;; attempt++) {
        resolved[0] = '\0';
        tbl = splay_load_once(dir, sym_path, use_mmap, resolved, sizeof(resolved), &held);
        if (tbl && !RAY_IS_ERR(tbl)) break;
        int why = splay_retry(dir, resolved, attempt, t0);
        if (why < 0) { tbl = splay_retry_error(why, dir, tbl); break; }
        if (!why) break;
        ray_error_free(tbl);
    }
    if (held) ray_sym_domain_release(held);
    return tbl;
}

bool ray_splay_has_schema(const char* dir) {
    char resolved[1024], schema[1100];
    int64_t t0 = ray_profile_now_ns();
    for (int attempt = 1;; attempt++) {
        if (ray_splay_resolve_dir(dir, resolved, sizeof(resolved)) != RAY_OK)
            return false;
        int n = snprintf(schema, sizeof(schema), "%s/.d", resolved);
        if (n < 0 || (size_t)n >= sizeof(schema)) return false;
        struct stat st;
        if (stat(schema, &st) == 0) return S_ISREG(st.st_mode);
        if (splay_retry(dir, resolved, attempt, t0) != 1) return false;
    }
}

ray_t* ray_splay_load(const char* dir, const char* sym_path) {
    return splay_load_impl(dir, sym_path, false);
}

/* Build + persist accelerator indexes for a freshly-streamed splayed store.
 * The .csv.splayed writer emits raw columns; this scans each eligible numeric
 * column once and APPENDS a chunk-zone index region to its file (no data
 * rewrite), so a later mmap load gets the same block-skip an in-memory build
 * has.  Best-effort and idempotent-ish: ray_col_append_index refuses a file
 * that is not exactly payload-sized (already indexed), so re-runs are no-ops. */
/* Index one column of a just-written splayed table (see
 * ray_splay_build_indexes).  Columns are independent — each reads and
 * appends to its own file — so the caller runs one task per column. */
/* deferred: when non-NULL and the column qualifies for a hash index, the
 * computed zone is stored there instead of persisted and nothing is written;
 * splay_persist_hash_or_zone finishes the column. */
static void splay_build_index_col(const char* dir, ray_t* tbl, int64_t c, ray_t** deferred) {
    {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (!col || RAY_IS_ERR(col)) return;

        /* Explicit SYM index: a SYM column carrying a grouped (hash) index in
         * memory gets a hash index persisted inline — regardless of length (the
         * grouped attr is deliberate intent, unlike the size-gated auto
         * dict/chunk-zone below).
         *
         * The in-memory index (col->index) is keyed by this column's runtime
         * domain ids, but the raw column was written re-encoded to FILE-LOCAL
         * domain positions (ray_col_save_sym_encoded).  Persisting the runtime-
         * keyed index verbatim would make every reload-time equality probe miss:
         * the probe resolves the query symbol to a file-local position, which
         * lands in a different bucket than the runtime id the builder hashed.
         * So rebuild the hash over the just-written file-local column (loaded
         * back index-stripped) — that index is keyed in the same space the
         * probe uses.  The raw file is payload-sized here, so append adds only
         * the region. */
        if (col->type == RAY_SYM && ray_index_kind(col) == RAY_IDX_HASH) {
            ray_t* nstr = ray_sym_str(ray_table_col_name(tbl, c));
            if (nstr && !RAY_IS_ERR(nstr)) {
                char path[1024];
                int n = snprintf(path, sizeof(path), "%s/%.*s", dir,
                                 (int)ray_str_len(nstr), ray_str_ptr(nstr));
                if (n > 0 && n < (int)sizeof(path)) {
                    ray_t* fc = ray_col_load(path);  /* file-local codes */
                    if (fc && !RAY_IS_ERR(fc)) {
                        ray_t* fi = ray_idx_hash_fn(fc);  /* hash over file-local */
                        if (fi && !RAY_IS_ERR(fi)) {
                            (void)ray_col_append_index(path,
                                ray_index_payload(fi->index), fi->len, RAY_SYM);
                            ray_release(fi);
                        } else if (fi) {
                            ray_error_free(fi);
                        }
                        ray_release(fc);
                    } else if (fc) {
                        ray_error_free(fc);
                    }
                }
            }
            return;
        }

        /* Explicit STR index: a grouped / unique hash on a STR column is keyed
         * on byte hashes and row ids — neither depends on a domain — so the
         * in-memory index persists verbatim, regardless of length.  It takes
         * the column's single index slot: the size-gated auto dictionary
         * below is not built for it. */
        if (col->type == RAY_STR && ray_index_kind(col) == RAY_IDX_HASH) {
            ray_t* nstr = ray_sym_str(ray_table_col_name(tbl, c));
            if (nstr && !RAY_IS_ERR(nstr)) {
                char path[1024];
                int n = snprintf(path, sizeof(path), "%s/%.*s", dir,
                                 (int)ray_str_len(nstr), ray_str_ptr(nstr));
                if (n > 0 && n < (int)sizeof(path))
                    (void)ray_col_append_index(path, ray_index_payload(col->index),
                                               col->len, RAY_STR);
            }
            return;
        }

        if (col->len < (1 << 16)) return;

        /* STR columns get a dictionary (group on int codes); numeric/temporal
         * get the per-chunk min/max for block-skip.
         *
         * High-entropy numeric columns are then upgraded to a persisted HASH
         * index using the same chunk-zone entropy heuristic the in-memory
         * .csv.read load applies (ray_csv_hash_upgrade_check): the index
         * decision is made ONCE at conversion time, so a `.db.splayed.get`
         * reload serves equality probes exactly as fast as a fresh CSV load
         * (previously the reloaded store had only the zone map and e.g.
         * ClickBench q10 ran 5x slower on-disk than in-memory). */
        ray_t* idx = (col->type == RAY_STR)
                     ? ray_index_dict_compute(col)
                     : ray_index_chunk_zone_compute(col, 16);
        if (!idx || RAY_IS_ERR(idx)) { if (idx) ray_error_free(idx); return; }

        if (col->type != RAY_STR &&
            ray_csv_hash_upgrade_check(col->type, col->len,
                                       ray_index_payload(idx))) {
            if (deferred) {
                /* Inside a per-column task: the hash build has its own
                 * parallel path that needs the pool, so hand the zone
                 * back and let the caller build the hash afterwards. */
                *deferred = idx;
                return;
            }
            ray_t* hi = ray_idx_hash_fn(col);
            if (hi && !RAY_IS_ERR(hi) && (hi->attrs & RAY_ATTR_HAS_INDEX)) {
                ray_release(idx);          /* zone sacrificed for the hash */
                idx = NULL;
                ray_t* nstr = ray_sym_str(ray_table_col_name(tbl, c));
                if (nstr && !RAY_IS_ERR(nstr)) {
                    char path[1100];
                    int n = snprintf(path, sizeof(path), "%s/%.*s", dir,
                                     (int)ray_str_len(nstr), ray_str_ptr(nstr));
                    if (n > 0 && n < (int)sizeof(path))
                        (void)ray_col_append_index(path,
                            ray_index_payload(hi->index), hi->len, hi->type);
                }
                ray_release(hi);
                return;
            }
            if (hi) { if (RAY_IS_ERR(hi)) ray_error_free(hi); else ray_release(hi); }
            /* Hash build failed — fall through and persist the zone. */
        }

        ray_t* nstr = ray_sym_str(ray_table_col_name(tbl, c));
        if (nstr && !RAY_IS_ERR(nstr)) {
            char path[1100];
            int n = snprintf(path, sizeof(path), "%s/%.*s", dir,
                             (int)ray_str_len(nstr), ray_str_ptr(nstr));
            if (n > 0 && n < (int)sizeof(path))
                (void)ray_col_append_index(path, ray_index_payload(idx),
                                           col->len, col->type);
        }
        ray_release(idx);
    }
}

/* Persist a deferred column: its hash index when the build succeeded
 * (hashed[c]), else the zone it was computed with (deferred[c]). */
typedef struct { const char* dir; ray_t* tbl; ray_t** deferred; ray_t** hashed; } splay_index_ctx_t;

static void splay_persist_deferred(splay_index_ctx_t* x, int64_t c) {
    ray_t* col = ray_table_get_col_idx(x->tbl, c);
    ray_t* nstr = ray_sym_str(ray_table_col_name(x->tbl, c));
    char path[1100];
    int n = (nstr && !RAY_IS_ERR(nstr))
        ? snprintf(path, sizeof(path), "%s/%.*s", x->dir, (int)ray_str_len(nstr), ray_str_ptr(nstr))
        : -1;
    bool have_path = n > 0 && n < (int)sizeof(path);
    ray_t* hi = x->hashed[c];
    if (hi) {
        if (have_path)
            (void)ray_col_append_index(path, ray_index_payload(hi->index), hi->len, hi->type);
        ray_release(hi);
    } else if (have_path) {
        (void)ray_col_append_index(path, ray_index_payload(x->deferred[c]), col->len, col->type);
    }
    ray_release(x->deferred[c]);
    x->hashed[c] = NULL; x->deferred[c] = NULL;
}

static void splay_persist_task(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    splay_index_ctx_t* x = (splay_index_ctx_t*)raw;
    if (x->deferred[start]) splay_persist_deferred(x, start);
}
static void splay_build_index_task(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    splay_index_ctx_t* c = (splay_index_ctx_t*)raw;
    splay_build_index_col(c->dir, c->tbl, start, &c->deferred[start]);
}

void ray_splay_build_indexes(const char* dir, ray_t* tbl) {

    if (!dir || !tbl || RAY_IS_ERR(tbl) || tbl->type != RAY_TABLE) return;
    int64_t nc = ray_table_ncols(tbl);
    if (nc <= 0) return;
    /* One task per column: the zone / dictionary / hash builds are per-row
     * scans of each column and used to run one after another on the
     * calling thread — the longest serial stretch of a CSV → splayed load. */
    ray_pool_t* pool = ray_pool_get();
    if (ray_pool_par_dispatch_ok(pool, nc, 2)) {
        /* Zones / dictionaries per column in parallel; the columns that
         * qualify for a hash index come back deferred and are built one
         * after another on this thread, each hash build parallel inside. */
        ray_t** deferred = (ray_t**)ray_sys_alloc((size_t)nc * 2 * sizeof(ray_t*));
        if (!deferred) {
            for (int64_t c = 0; c < nc; c++) splay_build_index_col(dir, tbl, c, NULL);
            return;
        }
        memset(deferred, 0, (size_t)nc * 2 * sizeof(ray_t*));
        splay_index_ctx_t ctx = { .dir = dir, .tbl = tbl, .deferred = deferred, .hashed = deferred + nc };
        ray_pool_dispatch_n(pool, splay_build_index_task, &ctx, (uint32_t)nc);
        /* Hash builds one after another (each parallel inside), then the
         * writes of all deferred columns together. */
        for (int64_t c = 0; c < nc; c++) {
            if (!deferred[c]) continue;
            ray_t* hi = ray_idx_hash_fn(ray_table_get_col_idx(tbl, c));
            if (hi && !RAY_IS_ERR(hi) && (hi->attrs & RAY_ATTR_HAS_INDEX)) ctx.hashed[c] = hi;
            else if (hi) { if (RAY_IS_ERR(hi)) ray_error_free(hi); else ray_release(hi); }
        }
        ray_pool_dispatch_n(pool, splay_persist_task, &ctx, (uint32_t)nc);
        ray_sys_free(deferred);
    } else {
        for (int64_t c = 0; c < nc; c++) splay_build_index_col(dir, tbl, c, NULL);
    }
}

ray_t* ray_read_splayed(const char* dir, const char* sym_path) {
    return splay_load_impl(dir, sym_path, true);
}

static ray_t* splay_read_dom_once(const char* dir, ray_sym_domain_t* dom,
                                  char* resolved, size_t resolved_sz) {
    ray_err_t err = ray_splay_resolve_dir(dir, resolved, resolved_sz);
    if (err != RAY_OK)
        return ray_error(ray_err_code_str(err), "cannot resolve splayed generation");
    ray_sym_domain_t* fresh = NULL;
    const char* path = dom ? ray_sym_domain_path(dom) : NULL;
    if (path) {
        fresh = ray_sym_domain_open(path);
        if (!fresh) return ray_error("corrupt", "cannot refresh splayed symfile");
        dom = fresh;
    }
    ray_t* tbl = splay_load_dom_impl(resolved, dom, true);
    if (fresh) ray_sym_domain_release(fresh);
    return tbl;
}

ray_t* ray_read_splayed_dom(const char* dir, struct ray_sym_domain_s* dom) {
    char resolved[1024];
    int64_t t0 = ray_profile_now_ns();
    for (int attempt = 1;; attempt++) {
        resolved[0] = '\0';
        ray_t* tbl = splay_read_dom_once(dir, dom, resolved, sizeof(resolved));
        if (tbl && !RAY_IS_ERR(tbl)) return tbl;
        int why = splay_retry(dir, resolved, attempt, t0);
        if (why < 0) return splay_retry_error(why, dir, tbl);
        if (!why) return tbl;
        ray_error_free(tbl);
    }
}

ray_err_t ray_splay_hash_column(const char* path, ray_t* zone) {
    if (!path) return RAY_ERR_DOMAIN;
    bool trace = getenv("RAY_CSV_TRACE") != NULL;   /* the converters' phase trace */
    int64_t t0 = trace ? ray_profile_now_ns() : 0;
    ray_t* col = ray_col_mmap(path);
    if (!col || RAY_IS_ERR(col)) { if (col) ray_error_free(col); return RAY_ERR_IO; }
    ray_err_t err = RAY_ERR_IO;
    int64_t t1 = trace ? ray_profile_now_ns() : 0;
    ray_t* hi = ray_idx_hash_fn(col);
    int64_t t2 = trace ? ray_profile_now_ns() : 0;
    if (hi && !RAY_IS_ERR(hi) && (hi->attrs & RAY_ATTR_HAS_INDEX)) {
        err = ray_col_append_index(path, ray_index_payload(hi->index), hi->len, hi->type);
        if (trace) {
            const ray_index_t* hx = ray_index_payload(hi->index);
            fprintf(stderr, "splayed hash: file=%s rows=%" PRId64 " keys=%" PRId64 " groups=%" PRId64
                            " mmap=%.1fms build=%.1fms write=%.1fms\n",
                    path, hi->len, hx->u.hash.n_keys, hx->u.hash.n_groups,
                    (double)(t1 - t0) / 1e6, (double)(t2 - t1) / 1e6, (double)(ray_profile_now_ns() - t2) / 1e6);
        }
        ray_release(hi);
    } else if (hi) {
        if (RAY_IS_ERR(hi)) ray_error_free(hi); else ray_release(hi);
    }
    if (err != RAY_OK && zone)
        err = ray_col_append_index(path, ray_index_payload(zone), col->len, col->type);
    ray_release(col);
    return err;
}
