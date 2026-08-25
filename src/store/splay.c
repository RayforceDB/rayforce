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
#include "store/col.h"
#include "store/fileio.h"
#include "store/serde.h"
#include "table/sym.h"
#include "table/table.h"
#include "table/domain.h"
#include "ops/idxop.h"
#include "io/csv.h"      /* ray_csv_hash_upgrade_check — shared index policy */
#include "vec/str.h"
#include "lang/format.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

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

static ray_err_t splay_save_impl(ray_t* tbl, const char* dir, const char* sym_path,
                                 bool durable) {
    if (!tbl || RAY_IS_ERR(tbl)) return RAY_ERR_TYPE;
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
     * names are skipped below), so the default convention never collides —
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

        ray_err_t sym_err = ray_sym_domain_flush(dom, durable);
        if (sym_err != RAY_OK) {
            ray_sym_domain_release(dom);
            return sym_err;
        }
    }

    int64_t ncols = ray_table_ncols(tbl);

    /* 2. Column files (and the schema names they correspond to).
     * NOTE: overwriting an existing dir rewrites columns in place; a crash
     * mid-loop leaves old .d + a mix of old/new column files.  Ragged
     * lengths are caught at load (column-length check); equal-length
     * mixed-generation rows are inherent to in-place overwrite and would
     * need staged writes — out of scope (fresh-dir crashes degrade to a
     * visibly missing table via the .d-last commit marker). */
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
        if (!splay_col_name_safe(name, name_len)) {
            ray_release(schema);
            if (dom) ray_sym_domain_release(dom);
            return RAY_ERR_DOMAIN;
        }

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
            /* No new .d is written.  Preflight prevents deterministic format
             * failures here; an I/O error or crash can still leave an
             * existing directory with mixed-generation column files. */
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

ray_err_t ray_splay_save(ray_t* tbl, const char* dir, const char* sym_path) {
    return splay_save_impl(tbl, dir, sym_path, true);
}

ray_err_t ray_splay_save_bulk(ray_t* tbl, const char* dir, const char* sym_path) {
    return splay_save_impl(tbl, dir, sym_path, false);
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

/* Resolve sym_path to a FILE domain.  Missing file → NULL domain with
 * RAY_OK (only an error if a SYM column is later encountered — the
 * symbol-free-table exemption must hold for reads too); existing but
 * unopenable/invalid file → loud error. */
static ray_t* splay_load_impl(const char* dir, const char* sym_path,
                              bool use_mmap) {
    ray_sym_domain_t* dom = NULL;
    if (sym_path) {
        struct stat st;
        if (stat(sym_path, &st) == 0) {
            dom = ray_sym_domain_open(sym_path);
            if (!dom)
                return ray_error("corrupt",
                    "symfile %s: unreadable or invalid (bad magic, torn "
                    "record, or missing \"\" at position 0)", sym_path);
        }
    }
    ray_t* tbl = splay_load_dom_impl(dir, dom, use_mmap);
    if (dom) ray_sym_domain_release(dom); /* columns hold their own refs */
    return tbl;
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
void ray_splay_build_indexes(const char* dir, ray_t* tbl) {
    if (!dir || !tbl || RAY_IS_ERR(tbl) || tbl->type != RAY_TABLE) return;
    int64_t nc = ray_table_ncols(tbl);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (!col || RAY_IS_ERR(col)) continue;

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
            continue;
        }

        if (col->len < (1 << 16)) continue;

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
        if (!idx || RAY_IS_ERR(idx)) { if (idx) ray_error_free(idx); continue; }

        if (col->type != RAY_STR &&
            ray_csv_hash_upgrade_check(col->type, col->len,
                                       ray_index_payload(idx))) {
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
                continue;
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

ray_t* ray_read_splayed(const char* dir, const char* sym_path) {
    return splay_load_impl(dir, sym_path, true);
}

ray_t* ray_read_splayed_dom(const char* dir, struct ray_sym_domain_s* dom) {
    return splay_load_dom_impl(dir, dom, true);
}
