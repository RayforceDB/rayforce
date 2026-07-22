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

#include "col.h"
#include "core/platform.h"
#include "mem/heap.h"
#include "store/serde.h"
#include "store/fileio.h"
#include "table/sym.h"
#include "table/domain.h"
#include "ops/idxop.h"
#include "vec/str.h"
#include "lang/format.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

/* --------------------------------------------------------------------------
 * validate_sym_bounds -- check all indices in a RAY_SYM column are < sym_count
 *
 * Width-dispatched scan for maximum index. Returns RAY_ERR_CORRUPT if any
 * index >= sym_count. Skipped when sym_count == 0 (allows raw column loads
 * in tests without a sym file).
 * -------------------------------------------------------------------------- */

static ray_err_t validate_sym_bounds(const void* data, int64_t len,
                                     uint8_t attrs, uint32_t sym_count) {
    if (sym_count == 0 || len == 0) return RAY_OK;

    uint64_t max_id = 0;
    switch (attrs & RAY_SYM_W_MASK) {
    case RAY_SYM_W8: {
        const uint8_t* p = (const uint8_t*)data;
        for (int64_t i = 0; i < len; i++)
            if (p[i] > max_id) max_id = p[i];
        break;
    }
    case RAY_SYM_W16: {
        const uint16_t* p = (const uint16_t*)data;
        for (int64_t i = 0; i < len; i++)
            if (p[i] > max_id) max_id = p[i];
        break;
    }
    case RAY_SYM_W32: {
        const uint32_t* p = (const uint32_t*)data;
        for (int64_t i = 0; i < len; i++)
            if (p[i] > max_id) max_id = p[i];
        break;
    }
    case RAY_SYM_W64: {
        const int64_t* p = (const int64_t*)data;
        for (int64_t i = 0; i < len; i++) {
            if (p[i] < 0) return RAY_ERR_CORRUPT;
            if ((uint64_t)p[i] > max_id) max_id = (uint64_t)p[i];
        }
        break;
    }
    default:
        return RAY_ERR_CORRUPT;
    }

    if (max_id >= sym_count) return RAY_ERR_CORRUPT;
    return RAY_OK;
}

/* Magic numbers for extended column formats */
#define STR_LIST_MAGIC  0x4C525453U  /* "STRL" */
#define STR_VEC_MAGIC   0x56525453U  /* "STRV" */
#define LIST_MAGIC      0x4754534CU  /* "LSTG" */
#define TABLE_MAGIC     0x4C425454U  /* "TTBL" */

static size_t col_str_pool_payload_len(const ray_t* vec);

/* --------------------------------------------------------------------------
 * Column file format:
 *   Bytes 0-15:  aux — RESERVED for postponed on-disk index persistence
 *                (min/max zone map); written ZERO today, NOT the version.
 *   Bytes 16-31: mmod=0, order=<format major version>, type, attrs, rc, len
 *   Bytes 32+:   raw element data
 *
 * On-disk format IS the in-memory format (zero deserialization on load):
 * file-offset 0 maps directly as the in-memory `ray_t`, payload at offset
 * 32.  The first 32 bytes ARE the allocator's object header; there is NO
 * separate envelope.  Of the header bytes, only mmod(16) and order(17) are
 * on-disk-free (written and recomputed on load).  The format MAJOR version
 * is a single byte carried in `order` (col.h); there is NO magic.  The
 * loaders validate the version, then reset the runtime `order` to its
 * correct value and reconstruct runtime aux in place (zero for plain
 * columns, symfile-domain attach for RAY_SYM via the COW header-page patch).
 *
 * Null state lives in the payload as a type-correct sentinel
 * (NULL_F64/NULL_I64/...).  There is no separate bitmap region.
 * -------------------------------------------------------------------------- */

/* The format-version constant and the stamp/check helpers
 * (ray_col_stamp_format / ray_col_check_format) live in col.h so the CSV
 * streaming splayed-column writer (src/io/csv.c) shares the exact same
 * on-disk identity. */

/* Allowlist of attr bits the save path legitimately persists (col_save_impl
 * strips HAS_INDEX / HAS_LINK / SLICE before writing the header; what
 * survives is the SYM width bits, SORTED, and HAS_NULLS).  Disk attrs are
 * untrusted: runtime-only bits like HAS_INDEX/HAS_LINK/SLICE route aux
 * bytes as owned pointers (ray_release_owned_refs would release an
 * attacker-controlled pointer), and ARENA would defeat refcounting.
 * Loaders must mask loaded attrs to this set immediately after header
 * materialization, BEFORE any error path can release the object.  Link
 * sidecar reattach (try_load_link_sidecar) runs after masking and sets
 * HAS_LINK with a freshly interned sym ID, so legit links are unaffected. */
#define COL_DISK_ATTRS_MASK \
    ((uint8_t)(RAY_SYM_W_MASK | RAY_ATTR_SORTED | RAY_ATTR_HAS_NULLS))

/* "Index present" marker written to the on-disk header's reserved aux[0..3]
 * (see col.h: aux is reserved for index persistence).  Existing files leave
 * aux=0, so the marker is absent → those files load through the exact current
 * path.  Only a file carrying this marker has an inline index region appended
 * at its 32-aligned tail. */
#define COL_IDX_AUX_MAGIC ((uint32_t)0x58444958)  /* "XIDX" */

/* Explicit allowlist of types that are safe to serialize as raw bytes.
 * Fixed-size scalar types plus RAY_STR.  RAY_STR has a pointer-bearing pool
 * in memory, but the column format stores the fixed 16-byte descriptors and
 * the byte pool as adjacent raw regions so mmap can restore the pointer
 * without deserializing string contents. */
static bool is_serializable_type(int8_t t) {
    switch (t) {
    case RAY_BOOL: case RAY_U8:   case RAY_I16:
    case RAY_I32:  case RAY_I64:  case RAY_F32: case RAY_F64:
    case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP: case RAY_GUID:
    case RAY_SYM:  case RAY_STR:
        return true;
    default:
        return false;
    }
}

/* --------------------------------------------------------------------------
 * String list detection: RAY_LIST whose elements are all -RAY_STR
 * -------------------------------------------------------------------------- */

static bool is_str_list(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return false;
    if (v->type != RAY_LIST) return false;
    ray_t** slots = (ray_t**)ray_data(v);
    for (int64_t i = 0; i < v->len; i++) {
        ray_t* elem = slots[i];
        if (!elem || RAY_IS_ERR(elem)) return false;
        if (elem->type != -RAY_STR) return false;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * col_save_str_list -- serialize a list of string atoms
 *
 * Format: [4B magic "STRL"][8B count][for each: 4B len + data bytes]
 * -------------------------------------------------------------------------- */

static ray_err_t col_save_str_list(ray_t* list, FILE* f) {
    uint32_t magic = STR_LIST_MAGIC;
    if (fwrite(&magic, 4, 1, f) != 1) return RAY_ERR_IO;

    int64_t count = list->len;
    if (fwrite(&count, 8, 1, f) != 1) return RAY_ERR_IO;

    ray_t** slots = (ray_t**)ray_data(list);
    for (int64_t i = 0; i < count; i++) {
        ray_t* s = slots[i];
        const char* sp = ray_str_ptr(s);
        size_t slen = ray_str_len(s);
        uint32_t len32 = (uint32_t)slen;
        if (fwrite(&len32, 4, 1, f) != 1) return RAY_ERR_IO;
        if (slen > 0 && fwrite(sp, 1, slen, f) != slen) return RAY_ERR_IO;
    }
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * col_load_str_list -- deserialize a string list from mapped data
 *
 * ptr points past the 4B magic. remaining = bytes available.
 * -------------------------------------------------------------------------- */

static ray_t* col_load_str_list(const uint8_t* ptr, size_t remaining) {
    if (remaining < 8) return ray_error("corrupt", NULL);
    int64_t count;
    memcpy(&count, ptr, 8);
    ptr += 8; remaining -= 8;

    if (count < 0 || (uint64_t)count > remaining / 4)
        return ray_error("corrupt", NULL);

    ray_t* list = ray_list_new(count);
    if (!list || RAY_IS_ERR(list)) return list;

    for (int64_t i = 0; i < count; i++) {
        if (remaining < 4) { ray_release(list); return ray_error("corrupt", NULL); }
        uint32_t slen;
        memcpy(&slen, ptr, 4);
        ptr += 4; remaining -= 4;

        if (slen > remaining) { ray_release(list); return ray_error("corrupt", NULL); }
        ray_t* s = ray_str((const char*)ptr, (size_t)slen);
        if (!s || RAY_IS_ERR(s)) { ray_release(list); return s; }
        ptr += slen; remaining -= slen;

        list = ray_list_append(list, s);
        ray_release(s);  /* list_append retains */
        if (!list || RAY_IS_ERR(list)) return list;
    }
    return list;
}

static ray_t* col_load_str_vec(const uint8_t* ptr, size_t remaining) {
    if (remaining > (size_t)INT64_MAX) return ray_error("range", "col load str-vec: payload size exceeds int64 max, got %zu bytes", remaining);
    int64_t len = (int64_t)remaining;
    ray_t* result = ray_de_raw((uint8_t*)ptr, &len);
    if (!result || RAY_IS_ERR(result)) return result;
    if (result->type != RAY_STR) {
        const char* rt = ray_type_name(result->type);
        ray_release(result);
        return ray_error("type", "col load str-vec: decoded payload is not a str vector, got %s", rt);
    }
    return result;
}

/* --------------------------------------------------------------------------
 * Recursive element serialization for generic lists and tables
 *
 * Recursive element format:
 *   [1B type]
 *   atoms (type < 0):
 *     -RAY_STR: [4B len][data bytes]
 *     -RAY_SYM: [4B len][data bytes]   (resolved string; load re-interns)
 *     other:       [8B raw value]
 *   RAY_SYM vector: [8B len][per cell: 4B len + data bytes]
 *   other vectors with is_serializable_type: [8B len][raw data]
 *   RAY_LIST: [8B count][recursive elements...]
 *   RAY_TABLE: [8B ncols][8B nrows][for each col: 8B name_sym + recursive col]
 *
 * SYM data inside recursive payloads (list columns, nested tables) is
 * serialized as STRINGS, not ids (the flip, Task 7b): nested sym data
 * has no symfile of its own, and raw ids — global or positional — only
 * resolve against incidental process state.  Loads intern into the
 * global table, so nested SYM data is always runtime-domain.  Tables
 * with ONLY nested sym data therefore need no symfile at all. */

static ray_err_t col_write_recursive(ray_t* obj, FILE* f);

static ray_err_t col_write_recursive(ray_t* obj, FILE* f) {
    if (!obj || RAY_IS_ERR(obj)) return RAY_ERR_TYPE;

    int8_t type = obj->type;
    if (fwrite(&type, 1, 1, f) != 1) return RAY_ERR_IO;

    if (type < 0) {
        /* Atom */
        if (type == -RAY_STR || type == -RAY_SYM) {
            /* SYM atoms serialize as their resolved string (atoms are
             * runtime-domain by design; ids never reach disk). */
            ray_t* s = (type == -RAY_SYM) ? ray_sym_str(obj->i64) : obj;
            if (!s) return RAY_ERR_CORRUPT;
            const char* sp = ray_str_ptr(s);
            size_t slen = ray_str_len(s);
            uint32_t len32 = (uint32_t)slen;
            if (fwrite(&len32, 4, 1, f) != 1) return RAY_ERR_IO;
            if (slen > 0 && fwrite(sp, 1, slen, f) != slen) return RAY_ERR_IO;
        } else {
            /* Fixed-size atom: write 8 bytes of the value union */
            if (fwrite(&obj->i64, 8, 1, f) != 1) return RAY_ERR_IO;
        }
        return RAY_OK;
    }

    if (type == RAY_SYM) {
        /* SYM vector nested in a recursive payload: per-cell strings,
         * resolved through the VEC's domain. */
        int64_t len = obj->len;
        if (fwrite(&len, 8, 1, f) != 1) return RAY_ERR_IO;
        for (int64_t i = 0; i < len; i++) {
            ray_t* s = ray_sym_vec_cell(obj, i);
            if (!s) return RAY_ERR_CORRUPT;
            const char* sp = ray_str_ptr(s);
            size_t slen = ray_str_len(s);
            uint32_t len32 = (uint32_t)slen;
            if (fwrite(&len32, 4, 1, f) != 1) return RAY_ERR_IO;
            if (slen > 0 && fwrite(sp, 1, slen, f) != slen) return RAY_ERR_IO;
        }
        return RAY_OK;
    }

    if (is_serializable_type(type)) {
        /* Fixed-size vector: write len + raw data.
         * RAY_STR: also write attrs byte and the adjacent byte pool. */
        int64_t len = obj->len;
        if (fwrite(&len, 8, 1, f) != 1) return RAY_ERR_IO;
        if (type == RAY_STR) {
            uint8_t attrs = obj->attrs;
            if (fwrite(&attrs, 1, 1, f) != 1) return RAY_ERR_IO;
        }
        uint8_t esz = ray_sym_elem_size(type, obj->attrs);
        size_t data_size = (size_t)len * esz;
        if (data_size > 0 && fwrite(ray_data(obj), 1, data_size, f) != data_size)
            return RAY_ERR_IO;
        if (type == RAY_STR) {
            uint64_t pool_size = (uint64_t)col_str_pool_payload_len(obj);
            if (fwrite(&pool_size, 8, 1, f) != 1) return RAY_ERR_IO;
            if (pool_size > 0 &&
                fwrite(ray_data(obj->str_pool), 1, (size_t)pool_size, f) != pool_size)
                return RAY_ERR_IO;
        }
        return RAY_OK;
    }

    if (type == RAY_LIST) {
        int64_t count = obj->len;
        if (fwrite(&count, 8, 1, f) != 1) return RAY_ERR_IO;
        ray_t** slots = (ray_t**)ray_data(obj);
        for (int64_t i = 0; i < count; i++) {
            ray_err_t err = col_write_recursive(slots[i], f);
            if (err != RAY_OK) return err;
        }
        return RAY_OK;
    }

    if (type == RAY_TABLE) {
        int64_t ncols = ray_table_ncols(obj);
        int64_t nrows = ray_table_nrows(obj);
        if (fwrite(&ncols, 8, 1, f) != 1) return RAY_ERR_IO;
        if (fwrite(&nrows, 8, 1, f) != 1) return RAY_ERR_IO;
        for (int64_t c = 0; c < ncols; c++) {
            int64_t name_sym = ray_table_col_name(obj, c);
            if (fwrite(&name_sym, 8, 1, f) != 1) return RAY_ERR_IO;
            ray_t* col = ray_table_get_col_idx(obj, c);
            ray_err_t err = col_write_recursive(col, f);
            if (err != RAY_OK) return err;
        }
        return RAY_OK;
    }

    return RAY_ERR_NYI;
}

/* Read recursive element from mapped buffer */
static ray_t* col_read_recursive(const uint8_t** pp, size_t* remaining);

static ray_t* col_read_recursive(const uint8_t** pp, size_t* remaining) {
    if (*remaining < 1) return ray_error("corrupt", NULL);
    int8_t type;
    memcpy(&type, *pp, 1);
    *pp += 1; *remaining -= 1;

    if (type < 0) {
        /* Atom */
        if (type == -RAY_STR || type == -RAY_SYM) {
            if (*remaining < 4) return ray_error("corrupt", NULL);
            uint32_t slen;
            memcpy(&slen, *pp, 4);
            *pp += 4; *remaining -= 4;
            if (slen > *remaining) return ray_error("corrupt", NULL);
            if (type == -RAY_SYM) {
                /* Stored as a string; re-intern → runtime-domain atom. */
                int64_t id = ray_sym_intern((const char*)*pp, (size_t)slen);
                *pp += slen; *remaining -= slen;
                if (id < 0) return ray_error("oom", NULL);
                return ray_sym(id);
            }
            ray_t* s = ray_str((const char*)*pp, (size_t)slen);
            *pp += slen; *remaining -= slen;
            return s;
        } else {
            /* Fixed atom: 8 bytes */
            if (*remaining < 8) return ray_error("corrupt", NULL);
            int64_t val;
            memcpy(&val, *pp, 8);
            *pp += 8; *remaining -= 8;

            ray_t* atom = ray_alloc(0);
            if (!atom || RAY_IS_ERR(atom)) return atom;
            atom->type = type;
            atom->i64 = val;
            return atom;
        }
    }

    if (type == RAY_SYM) {
        /* SYM vector stored as per-cell strings: intern each into the
         * global table — runtime-domain output by construction. */
        if (*remaining < 8) return ray_error("corrupt", NULL);
        int64_t len;
        memcpy(&len, *pp, 8);
        *pp += 8; *remaining -= 8;
        if (len < 0 || (uint64_t)len > *remaining / 4)
            return ray_error("corrupt", NULL);

        ray_t* vec = ray_sym_vec_new(RAY_SYM_W64, len);
        if (!vec || RAY_IS_ERR(vec)) return vec;
        vec->len = len;
        int64_t* out = (int64_t*)ray_data(vec);
        for (int64_t i = 0; i < len; i++) {
            if (*remaining < 4) { ray_release(vec); return ray_error("corrupt", NULL); }
            uint32_t slen;
            memcpy(&slen, *pp, 4);
            *pp += 4; *remaining -= 4;
            if (slen > *remaining) { ray_release(vec); return ray_error("corrupt", NULL); }
            int64_t id = ray_sym_intern((const char*)*pp, (size_t)slen);
            *pp += slen; *remaining -= slen;
            if (id < 0) { ray_release(vec); return ray_error("oom", NULL); }
            out[i] = id;
        }
        return vec;
    }

    if (is_serializable_type(type)) {
        /* Fixed-size vector */
        if (*remaining < 8) return ray_error("corrupt", NULL);
        int64_t len;
        memcpy(&len, *pp, 8);
        *pp += 8; *remaining -= 8;
        if (len < 0) return ray_error("corrupt", NULL);

        /* RAY_STR: read attrs byte for nulls/pool layout */
        uint8_t attrs = 0;
        if (type == RAY_STR) {
            if (*remaining < 1) return ray_error("corrupt", NULL);
            memcpy(&attrs, *pp, 1);
            *pp += 1; *remaining -= 1;
        }

        uint8_t esz = ray_sym_elem_size(type, attrs);
        if (esz > 0 && (uint64_t)len > SIZE_MAX / esz)
            return ray_error("corrupt", NULL);
        size_t data_size = (size_t)len * esz;
        if (data_size > *remaining) return ray_error("corrupt", NULL);

        ray_t* vec = ray_vec_new(type, len);
        if (!vec || RAY_IS_ERR(vec)) return vec;
        vec->len = len;
        if (data_size > 0)
            memcpy(ray_data(vec), *pp, data_size);
        *pp += data_size; *remaining -= data_size;

        if (type == RAY_STR) {
            if (*remaining < 8) { ray_release(vec); return ray_error("corrupt", NULL); }
            uint64_t pool_size;
            memcpy(&pool_size, *pp, 8);
            *pp += 8; *remaining -= 8;
            if (pool_size > *remaining || pool_size > (uint64_t)INT64_MAX) {
                ray_release(vec);
                return ray_error("corrupt", NULL);
            }
            if (pool_size > 0) {
                vec->str_pool = ray_alloc((size_t)pool_size);
                if (!vec->str_pool || RAY_IS_ERR(vec->str_pool)) {
                    ray_t* err = vec->str_pool ? vec->str_pool : ray_error("oom", NULL);
                    vec->str_pool = NULL;
                    ray_release(vec);
                    return err;
                }
                vec->str_pool->type = RAY_U8;
                vec->str_pool->len = (int64_t)pool_size;
                memcpy(ray_data(vec->str_pool), *pp, (size_t)pool_size);
            }
            *pp += pool_size; *remaining -= (size_t)pool_size;
            /* Stream attrs byte is untrusted — same allowlist as the
             * column-file loaders (COL_DISK_ATTRS_MASK). */
            vec->attrs = attrs & COL_DISK_ATTRS_MASK;
        }

        return vec;
    }

    if (type == RAY_LIST) {
        if (*remaining < 8) return ray_error("corrupt", NULL);
        int64_t count;
        memcpy(&count, *pp, 8);
        *pp += 8; *remaining -= 8;
        if (count < 0) return ray_error("corrupt", NULL);

        ray_t* list = ray_list_new(count);
        if (!list || RAY_IS_ERR(list)) return list;
        for (int64_t i = 0; i < count; i++) {
            ray_t* elem = col_read_recursive(pp, remaining);
            if (!elem || RAY_IS_ERR(elem)) { ray_release(list); return elem; }
            list = ray_list_append(list, elem);
            ray_release(elem);
            if (!list || RAY_IS_ERR(list)) return list;
        }
        return list;
    }

    if (type == RAY_TABLE) {
        if (*remaining < 16) return ray_error("corrupt", NULL);
        int64_t ncols, nrows;
        memcpy(&ncols, *pp, 8);
        *pp += 8; *remaining -= 8;
        memcpy(&nrows, *pp, 8);
        *pp += 8; *remaining -= 8;
        (void)nrows;  /* nrows is reconstructed from columns */

        if (ncols < 0) return ray_error("corrupt", NULL);
        ray_t* tbl = ray_table_new(ncols);
        if (!tbl || RAY_IS_ERR(tbl)) return tbl;

        for (int64_t c = 0; c < ncols; c++) {
            if (*remaining < 8) { ray_release(tbl); return ray_error("corrupt", NULL); }
            int64_t name_sym;
            memcpy(&name_sym, *pp, 8);
            *pp += 8; *remaining -= 8;

            ray_t* col = col_read_recursive(pp, remaining);
            if (!col || RAY_IS_ERR(col)) { ray_release(tbl); return col; }
            tbl = ray_table_add_col(tbl, name_sym, col);
            ray_release(col);  /* table_add_col retains */
            if (!tbl || RAY_IS_ERR(tbl)) return tbl;
        }
        return tbl;
    }

    return ray_error("nyi", NULL);
}

/* --------------------------------------------------------------------------
 * col_save_list -- serialize a generic RAY_LIST
 * -------------------------------------------------------------------------- */

static ray_err_t col_save_list(ray_t* list, FILE* f) {
    uint32_t magic = LIST_MAGIC;
    if (fwrite(&magic, 4, 1, f) != 1) return RAY_ERR_IO;
    return col_write_recursive(list, f);
}

/* --------------------------------------------------------------------------
 * col_save_table -- serialize a RAY_TABLE
 * -------------------------------------------------------------------------- */

static ray_err_t col_save_table(ray_t* tbl, FILE* f) {
    uint32_t magic = TABLE_MAGIC;
    if (fwrite(&magic, 4, 1, f) != 1) return RAY_ERR_IO;
    return col_write_recursive(tbl, f);
}

/* --------------------------------------------------------------------------
 * try_load_link_sidecar -- attach HAS_LINK to vec from `<path>.link`
 *
 * Best-effort: missing sidecar, unreadable file, or empty contents leave
 * vec as a plain int column.  Only RAY_I32 / RAY_I64 columns are eligible.
 * The sidecar holds the target table sym name in plain text; we intern it
 * into the local sym table and write the resulting sym ID + HAS_LINK bit.
 * Used by both ray_col_load (buddy-copy path) and ray_col_mmap (zero-copy
 * path) so linked columns survive both load styles.
 * -------------------------------------------------------------------------- */
static void try_load_link_sidecar(ray_t* vec, const char* path) {
    if (!vec || (vec->type != RAY_I32 && vec->type != RAY_I64)) return;
    char link_path[1024];
    size_t plen = strlen(path);
    if (plen + 6 >= sizeof(link_path)) return;
    memcpy(link_path, path, plen);
    memcpy(link_path + plen, ".link", 6);
    FILE* lf = fopen(link_path, "rb");
    if (!lf) return;
    /* Read the whole sidecar.  The target sym name can be longer than any fixed
     * buffer, and a truncated read would intern a DIFFERENT symbol — silently
     * linking the column to the wrong table.  Size to the file, capped so a
     * corrupt/oversized sidecar can't force a huge allocation. */
    if (fseek(lf, 0, SEEK_END) != 0) { fclose(lf); return; }
    long fsz = ftell(lf);
    if (fsz <= 0 || fsz > (1L << 20)) { fclose(lf); return; }
    rewind(lf);
    char* buf = (char*)ray_alloc_raw((size_t)fsz);
    if (!buf) { fclose(lf); return; }
    size_t n = fread(buf, 1, (size_t)fsz, lf);
    fclose(lf);
    /* Reject a short read (I/O error, or the sidecar shrank after the size
     * check above): interning a partial name would link the WRONG symbol —
     * the very failure this reader guards against. */
    if (n != (size_t)fsz) { ray_free_raw(buf); return; }
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'
                  || buf[n-1] == ' '  || buf[n-1] == '\t'
                  || buf[n-1] == '\0')) n--;
    if (n == 0) { ray_free_raw(buf); return; }
    int64_t target_sym = ray_sym_intern(buf, n);
    ray_free_raw(buf);
    if (target_sym < 0) return;
    vec->link_target = target_sym;
    vec->attrs |= RAY_ATTR_HAS_LINK;
}

/* --------------------------------------------------------------------------
 * ray_col_save -- write a vector to a column file
 * -------------------------------------------------------------------------- */

/* Append an inline index region to an EXISTING column file (no data rewrite):
 * pad the payload to 32, write the region, then stamp the aux[0..3] marker.
 * Used by the streaming .csv.splayed builder, which writes raw columns first.
 * `col_len`/`col_type` describe the on-disk column (payload = 32 + len*esz). */
ray_err_t ray_col_append_index(const char* path, const void* ix_v,
                               int64_t col_len, int8_t col_type) {
    const ray_index_t* ix = (const ray_index_t*)ix_v;
    if (!path || !ix) return RAY_ERR_DOMAIN;
    (void)col_len; (void)col_type;

    FILE* f = fopen(path, "r+b");
    if (!f) return RAY_ERR_IO;
    /* Already-indexed guard: the marker is stamped into aux[0..3] LAST, so its
     * presence means a complete prior append. */
    uint32_t cur_mg = 0;
    if (fread(&cur_mg, 1, 4, f) == 4 && cur_mg == COL_IDX_AUX_MAGIC) {
        fclose(f); return RAY_ERR_CORRUPT;
    }
    /* Append at the actual payload end — generic across formats: numeric is
     * [header][data]; STR is [header][descriptors][pool].  Don't recompute it
     * from col_len*esz (that ignores the str_pool). */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return RAY_ERR_IO; }
    long fsz = ftell(f);
    if (fsz < 32) { fclose(f); return RAY_ERR_CORRUPT; }
    int64_t payload_end = fsz;

    int64_t region_off = (payload_end + 31) & ~(int64_t)31;
    int64_t pad = region_off - payload_end;
    static const uint8_t zeros[32] = {0};
    if (pad > 0 && fwrite(zeros, 1, (size_t)pad, f) != (size_t)pad) { fclose(f); return RAY_ERR_IO; }

    int64_t rsize = ray_index_inline_size(ix);
    uint8_t* rbuf = (uint8_t*)ray_calloc_raw((size_t)(1) * ((size_t)rsize));
    if (!rbuf) { fclose(f); return RAY_ERR_OOM; }
    ray_index_inline_write(rbuf, ix);
    size_t rw = fwrite(rbuf, 1, (size_t)rsize, f);
    ray_free_raw(rbuf);
    if (rw != (size_t)rsize) { fclose(f); return RAY_ERR_IO; }

    /* Stamp the marker into aux[0..3] (file offset 0). */
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return RAY_ERR_IO; }
    uint32_t mg = COL_IDX_AUX_MAGIC;
    if (fwrite(&mg, 1, 4, f) != 4) { fclose(f); return RAY_ERR_IO; }
    /* The index is a derived accelerator (rebuildable); flush is enough — no
     * fsync needed, and the marker is written LAST so a torn append never
     * leaves a half-region that looks indexed. */
    if (fclose(f) != 0) return RAY_ERR_IO;
    return RAY_OK;
}

static ray_err_t col_save_impl(ray_t* vec, const char* path, bool durable) {
    if (!vec || RAY_IS_ERR(vec)) return RAY_ERR_TYPE;
    if (!path) return RAY_ERR_IO;

    /* Build temp path for crash-safe write: write tmp, fsync, atomic rename */
    char tmp_path[1024];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path))
        return RAY_ERR_IO;

    /* String list: RAY_LIST of -RAY_STR atoms */
    if (is_str_list(vec)) {
        FILE* f = fopen(tmp_path, "wb");
        if (!f) return RAY_ERR_IO;
        ray_err_t err = col_save_str_list(vec, f);
        fclose(f);
        if (err != RAY_OK) { remove(tmp_path); return err; }
        goto fsync_and_rename;
    }

    /* Generic list */
    if (vec->type == RAY_LIST) {
        FILE* f = fopen(tmp_path, "wb");
        if (!f) return RAY_ERR_IO;
        ray_err_t err = col_save_list(vec, f);
        fclose(f);
        if (err != RAY_OK) { remove(tmp_path); return err; }
        goto fsync_and_rename;
    }

    /* Table */
    if (vec->type == RAY_TABLE) {
        FILE* f = fopen(tmp_path, "wb");
        if (!f) return RAY_ERR_IO;
        ray_err_t err = col_save_table(vec, f);
        fclose(f);
        if (err != RAY_OK) { remove(tmp_path); return err; }
        goto fsync_and_rename;
    }

    /* Explicit allowlist of serializable types */
    if (!is_serializable_type(vec->type))
        return RAY_ERR_NYI;

    /* Bare column files keep PROCESS-LOCAL semantics for RAY_SYM: cells
     * are runtime intern ids (header rc = global count for the fast
     * reject).  A FILE-domain vec (a loaded splayed column saved through
     * this entry point) stores positions in ITS symfile — re-express
     * them as runtime ids via the domain LUT so the written file means
     * the same thing every bare-save file means.  Splayed table saves
     * never come here for SYM columns (ray_col_save_sym_encoded). */
    void*   sym_xlate = NULL;
    uint8_t sym_xlate_attrs = 0;
    if (vec->type == RAY_SYM &&
        ray_sym_vec_domain(vec) != ray_sym_runtime_domain()) {
        struct ray_sym_domain_s* dom = ray_sym_vec_domain(vec);
        const int64_t* lut = ray_sym_domain_runtime_lut(dom);
        if (!lut) return RAY_ERR_OOM;
        int64_t dcount = ray_sym_domain_count(dom);
        uint8_t w = ray_sym_dict_width((int64_t)ray_sym_count());
        uint8_t wesz = (uint8_t)(1u << w);
        if (vec->len > 0 && (uint64_t)vec->len > SIZE_MAX / wesz)
            return RAY_ERR_IO;
        sym_xlate = ray_alloc_raw(vec->len > 0 ? (size_t)vec->len * wesz : 1);
        if (!sym_xlate) return RAY_ERR_OOM;
        const void* src = ray_data(vec); /* slice-safe */
        for (int64_t i = 0; i < vec->len; i++) {
            int64_t pos = ray_read_sym(src, i, RAY_SYM, vec->attrs);
            if (pos < 0 || pos >= dcount) {
                ray_free_raw(sym_xlate);
                return RAY_ERR_CORRUPT;
            }
            ray_write_sym(sym_xlate, i, (uint64_t)lut[pos], RAY_SYM, w);
        }
        /* Translation reorders ids — SORTED no longer holds. */
        sym_xlate_attrs = w;
    }

    {
        FILE* f = fopen(tmp_path, "wb");
        if (!f) { ray_free_raw(sym_xlate); return RAY_ERR_IO; }

        /* Write a clean header (mmod=0, rc=0) */
        ray_t header;
        memcpy(&header, vec, 32);
        header.mmod = 0;
        /* Stamp the format major version into `order` (replaces the prior
         * `header.order = 0`); see col.h.  aux is left to the
         * index/null/SYM stripping below — reserved for postponed index
         * persistence, never the version. */
        ray_col_stamp_format(&header);
        /* For RAY_SYM: store sym count in rc field (always 0 on disk otherwise).
         * This serves as O(1) fast-reject metadata on load. */
        header.rc = (vec->type == RAY_SYM) ? ray_sym_count() : 0;

        /* HAS_INDEX rebase: an attached accelerator index displaces the
         * 16-byte aux union with an index pointer.  Strip HAS_INDEX from the
         * on-disk attrs and restore the pre-attach aux bytes; the persisted
         * index (if any) is signaled by the aux marker stamped after all aux
         * manipulation below, and its data is appended at the 32-aligned tail. */
        bool persist_index = false;
        if (vec->attrs & RAY_ATTR_HAS_INDEX) {
            ray_index_t* ix = ray_index_payload(vec->index);
            header.attrs &= ~RAY_ATTR_HAS_INDEX;
            memcpy(header.aux, ix->saved_aux, 16);
            persist_index = (vec->index != NULL);
        }

        /* HAS_LINK rebase: target sym ID lives at header.aux[8..15],
         * but sym IDs are process-local — the on-disk file would be
         * useless across runs.  Strip the bit and zero the slot; the
         * sidecar `.link` file (written below after rename) carries the
         * target table name in text form for portable restoration. */
        if (vec->attrs & RAY_ATTR_HAS_LINK) {
            header.attrs &= (uint8_t)~RAY_ATTR_HAS_LINK;
            memset(header.aux + 8, 0, 8);
        }

        /* Clear slice flag — slices are materialized on save. */
        header.attrs &= (uint8_t)~RAY_ATTR_SLICE;
        if (!(header.attrs & RAY_ATTR_HAS_NULLS))
            memset(header.aux, 0, 16);

        /* RAY_SYM: aux bytes 8-15 hold the runtime-only domain pointer
         * (rayforce.h), which must never reach disk — zero the slot
         * unconditionally (SYM aux carried no on-disk state before the
         * domain pointer either, so the file bytes are unchanged).
         * Load/mmap reattach the domain below. */
        if (vec->type == RAY_SYM)
            memset(header.aux, 0, 16);
        /* FILE-domain SYM re-expressed to runtime ids: width follows the
         * global table, SORTED dropped (id order changed). */
        if (sym_xlate)
            header.attrs = (uint8_t)((header.attrs &
                ~(uint8_t)(RAY_SYM_W_MASK | RAY_ATTR_SORTED)) | sym_xlate_attrs);

        /* Stamp the "index present" marker into the reserved aux (after all
         * other aux manipulation, which zeroes it for the common cases). */
        if (persist_index) {
            memset(header.aux, 0, 16);
            uint32_t mg = COL_IDX_AUX_MAGIC;
            memcpy(header.aux, &mg, 4);
        }

        size_t written = fwrite(&header, 1, 32, f);
        if (written != 32) { fclose(f); remove(tmp_path); ray_free_raw(sym_xlate); return RAY_ERR_IO; }

        /* Write data */
        if (vec->len < 0) { fclose(f); remove(tmp_path); ray_free_raw(sym_xlate); return RAY_ERR_CORRUPT; }
        uint8_t esz = ray_sym_elem_size(vec->type,
                                        sym_xlate ? sym_xlate_attrs : vec->attrs);
        if (esz == 0 && vec->len > 0) { fclose(f); remove(tmp_path); ray_free_raw(sym_xlate); return RAY_ERR_TYPE; }
        /* Overflow check: ensure len*esz fits in size_t with 32-byte header room */
        if ((uint64_t)vec->len > (SIZE_MAX - 32) / (esz ? esz : 1)) {
            fclose(f);
            remove(tmp_path);
            ray_free_raw(sym_xlate);
            return RAY_ERR_IO;
        }
        size_t data_size = (size_t)vec->len * esz;

        void* data;
        if (sym_xlate) {
            data = sym_xlate;
        } else if (vec->attrs & RAY_ATTR_SLICE) {
            /* Validate slice bounds before computing data pointer */
            ray_t* parent = vec->slice_parent;
            if (!parent || vec->slice_offset < 0 ||
                vec->slice_offset + vec->len > parent->len) {
                fclose(f);
                remove(tmp_path);
                return RAY_ERR_IO;
            }
            data = (char*)ray_data(parent) + vec->slice_offset * esz;
        } else {
            data = ray_data(vec);
        }

        if (data_size > 0) {
            written = fwrite(data, 1, data_size, f);
            if (written != data_size) { fclose(f); remove(tmp_path); ray_free_raw(sym_xlate); return RAY_ERR_IO; }
        }
        ray_free_raw(sym_xlate);
        sym_xlate = NULL;

        if (vec->type == RAY_STR) {
            size_t pool_size = col_str_pool_payload_len(vec);
            ray_t pool_header;
            memset(&pool_header, 0, sizeof(pool_header));
            if (vec->str_pool && !RAY_IS_ERR(vec->str_pool)) {
                memcpy(&pool_header, vec->str_pool, 32);
            }
            pool_header.mmod = 0;
            pool_header.order = 0;
            pool_header.type = RAY_U8;
            pool_header.attrs = 0;
            pool_header.rc = 0;
            pool_header.len = (int64_t)pool_size;
            written = fwrite(&pool_header, 1, 32, f);
            if (written != 32) { fclose(f); remove(tmp_path); return RAY_ERR_IO; }
            if (pool_size > 0) {
                if (!vec->str_pool || RAY_IS_ERR(vec->str_pool)) {
                    fclose(f);
                    remove(tmp_path);
                    return RAY_ERR_CORRUPT;
                }
                written = fwrite(ray_data(vec->str_pool), 1, pool_size, f);
                if (written != pool_size) { fclose(f); remove(tmp_path); return RAY_ERR_IO; }
            }
        }

        /* Inline index region (numeric and SYM columns; STR routes through dict
         * instead).  SYM carries RAY_IDX_HASH when the grouped attr is set;
         * the existing `persist_index && vec->type != RAY_STR` guard already
         * admits it.  Pad the payload to a 32-byte boundary, then append the
         * raw 32-aligned ray_t blocks (RAY_INDEX object + child vecs) the
         * loader mmaps in place.  Payload end here is 32 + data_size (no
         * str_pool on an indexed numeric/SYM column). */
        if (persist_index && vec->type != RAY_STR) {
            int64_t payload_end = 32 + (int64_t)data_size;
            int64_t region_off  = (payload_end + 31) & ~(int64_t)31;
            int64_t pad         = region_off - payload_end;
            static const uint8_t zeros[32] = {0};
            if (pad > 0 && fwrite(zeros, 1, (size_t)pad, f) != (size_t)pad) {
                fclose(f); remove(tmp_path); return RAY_ERR_IO;
            }
            ray_index_t* ix = ray_index_payload(vec->index);
            int64_t rsize = ray_index_inline_size(ix);
            uint8_t* rbuf = (uint8_t*)ray_calloc_raw((size_t)(1) * ((size_t)rsize));
            if (!rbuf) { fclose(f); remove(tmp_path); return RAY_ERR_OOM; }
            ray_index_inline_write(rbuf, ix);
            size_t rw = fwrite(rbuf, 1, (size_t)rsize, f);
            ray_free_raw(rbuf);
            if (rw != (size_t)rsize) { fclose(f); remove(tmp_path); return RAY_ERR_IO; }
        }

        fclose(f);
    }

fsync_and_rename:;
    ray_err_t err = RAY_OK;
    if (durable) {
        /* Fsync temp file for durability */
        ray_fd_t tmp_fd = ray_file_open(tmp_path, RAY_OPEN_READ | RAY_OPEN_WRITE);
        if (tmp_fd == RAY_FD_INVALID) { remove(tmp_path); return RAY_ERR_IO; }
        err = ray_file_sync(tmp_fd);
        ray_file_close(tmp_fd);
        if (err != RAY_OK) { remove(tmp_path); return err; }
    }

    /* Atomic rename: tmp -> final path */
    err = ray_file_rename(tmp_path, path);
    if (err != RAY_OK) { remove(tmp_path); return err; }
    /* Durable: fsync the parent dir so the rename (the new dir entry) is
     * persisted, not just the file contents. */
    if (durable) {
        err = ray_file_sync_dir(path);
        if (err != RAY_OK) return err;
    }

    /* Linked-column sidecar: write `<path>.link` containing the target
     * table's sym name (text form) so it survives the per-process
     * sym-ID re-assignment.  Remove any stale `.link` from a previous
     * save when the current vec is unlinked. */
    {
        char link_path[1024];
        size_t plen = strlen(path);
        if (plen + 6 < sizeof(link_path)) {
            memcpy(link_path, path, plen);
            memcpy(link_path + plen, ".link", 6);
            if (vec->attrs & RAY_ATTR_HAS_LINK) {
                ray_t* sym_str = ray_sym_str(vec->link_target);
                const char* sp = sym_str ? ray_str_ptr(sym_str) : NULL;
                size_t slen = sym_str ? ray_str_len(sym_str) : 0;
                if (sp && slen > 0) {
                    char tmp_link[1024];
                    memcpy(tmp_link, link_path, plen + 6);
                    if (plen + 10 < sizeof(tmp_link)) {
                        memcpy(tmp_link + plen + 5, ".tmp", 5);
                        FILE* lf = fopen(tmp_link, "wb");
                        if (lf) {
                            size_t wrote = fwrite(sp, 1, slen, lf);
                            fclose(lf);
                            if (wrote == slen) {
                                if (ray_file_rename(tmp_link, link_path) == RAY_OK
                                    && durable)
                                    (void)ray_file_sync_dir(link_path);
                            } else {
                                remove(tmp_link);
                            }
                        }
                    }
                }
            } else {
                /* No link on this column — clean stale sidecar if any. */
                remove(link_path);
            }
        }
    }

    return RAY_OK;
}

ray_err_t ray_col_save(ray_t* vec, const char* path) {
    return col_save_impl(vec, path, true);
}

ray_err_t ray_col_save_bulk(ray_t* vec, const char* path) {
    return col_save_impl(vec, path, false);
}

/* --------------------------------------------------------------------------
 * ray_col_save_sym_encoded -- write a RAY_SYM column as positions in the
 * table's symfile domain (the flip, Task 7b).
 *
 * Width = ray_sym_dict_width(|domain|) — the table's vocabulary, not the
 * process dictionary.  Header rc = domain count at save time (the O(1)
 * fast-reject the loader checks against the FILE domain's count).
 * Caller contract: every distinct symbol of the column was interned into
 * `target` and the domain was FLUSHED before this call (crash ordering:
 * sym → columns → .d) — an absent symbol here is RAY_ERR_CORRUPT.
 * -------------------------------------------------------------------------- */

ray_err_t ray_col_save_sym_encoded(ray_t* vec, const char* path,
                                   struct ray_sym_domain_s* target,
                                   bool durable) {
    if (!vec || RAY_IS_ERR(vec) || vec->type != RAY_SYM) return RAY_ERR_TYPE;
    if (!path || !target) return RAY_ERR_IO;
    if (vec->len < 0) return RAY_ERR_CORRUPT;

    struct ray_sym_domain_s* src = ray_sym_vec_domain(vec);
    int64_t dcount = ray_sym_domain_count(target);
    if (dcount > UINT32_MAX) return RAY_ERR_RANGE;
    uint8_t w = ray_sym_dict_width(dcount);
    uint8_t esz = (uint8_t)(1u << w);

    char tmp_path[1024];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path))
        return RAY_ERR_IO;

    FILE* f = fopen(tmp_path, "wb");
    if (!f) return RAY_ERR_IO;

    ray_err_t err = RAY_OK;
    {
        ray_t header;
        memcpy(&header, vec, 32);
        header.mmod = 0;
        /* Stamp the format major version into `order` (replaces the prior
         * `header.order = 0`); see col.h. */
        ray_col_stamp_format(&header);
        header.rc = (uint32_t)dcount;
        /* Same-domain saves preserve positions, hence order; re-encoding
         * from another domain reorders ids — drop SORTED then. */
        uint8_t keep_sorted = (src == target) ? (vec->attrs & RAY_ATTR_SORTED) : 0;
        header.attrs = (uint8_t)(w | keep_sorted);
        memset(header.aux, 0, 16); /* domain ptr is runtime-only state;
                                    * aux reserved (postponed index), never
                                    * the version (lives in `order` above). */
        if (fwrite(&header, 1, 32, f) != 32) err = RAY_ERR_IO;
    }

    if (err == RAY_OK) {
        uint8_t buf[8192];
        int64_t per = (int64_t)(sizeof(buf) / esz);
        const void* sdata = ray_data(vec); /* slice-safe */
        for (int64_t off = 0; err == RAY_OK && off < vec->len; off += per) {
            int64_t cnt = vec->len - off;
            if (cnt > per) cnt = per;
            for (int64_t i = 0; i < cnt; i++) {
                int64_t pos = ray_read_sym(sdata, off + i, RAY_SYM, vec->attrs);
                int64_t tpos;
                if (src == target) {
                    tpos = pos; /* already positions in the target */
                    if (tpos < 0 || tpos >= dcount) { err = RAY_ERR_CORRUPT; break; }
                } else {
                    ray_t* s = ray_sym_domain_str(src, pos);
                    if (!s) { err = RAY_ERR_CORRUPT; break; }
                    tpos = ray_sym_domain_find(target, ray_str_ptr(s),
                                               ray_str_len(s));
                    if (tpos < 0) { err = RAY_ERR_CORRUPT; break; }
                }
                ray_write_sym(buf, i, (uint64_t)tpos, RAY_SYM, w);
            }
            if (err == RAY_OK && cnt > 0 &&
                fwrite(buf, esz, (size_t)cnt, f) != (size_t)cnt)
                err = RAY_ERR_IO;
        }
    }

    if (fclose(f) != 0 && err == RAY_OK) err = RAY_ERR_IO;
    if (err != RAY_OK) { remove(tmp_path); return err; }

    if (durable) {
        ray_fd_t tmp_fd = ray_file_open(tmp_path, RAY_OPEN_READ | RAY_OPEN_WRITE);
        if (tmp_fd == RAY_FD_INVALID) { remove(tmp_path); return RAY_ERR_IO; }
        err = ray_file_sync(tmp_fd);
        ray_file_close(tmp_fd);
        if (err != RAY_OK) { remove(tmp_path); return err; }
    }

    err = ray_file_rename(tmp_path, path);
    if (err != RAY_OK) { remove(tmp_path); return err; }
    /* Durable: persist the new dir entry created by the rename. */
    if (durable) {
        err = ray_file_sync_dir(path);
        if (err != RAY_OK) return err;
    }

    /* SYM columns never carry links — clean any stale sidecar. */
    {
        char link_path[1024];
        size_t plen = strlen(path);
        if (plen + 6 < sizeof(link_path)) {
            memcpy(link_path, path, plen);
            memcpy(link_path + plen, ".link", 6);
            remove(link_path);
        }
    }
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * col_validate_mapped -- shared validation for ray_col_load / ray_col_mmap
 *
 * Maps the file, validates header/type/bounds, and returns parsed metadata.
 * On success, the mapping remains open (caller must unmap on error paths).
 * Returns NULL on success, or an error ray_t* on failure (mapping already
 * cleaned up in that case).
 * -------------------------------------------------------------------------- */

typedef struct {
    void*   mapped;
    size_t  mapped_size;
    ray_t*  header;       /* pointer into mapped region */
    uint8_t esz;
    size_t  data_size;
    bool    has_str_pool;
    size_t  str_pool_offset;
    size_t  str_pool_size;
    size_t  tail_offset;       /* end of payload — file size must match (unless has_index) */
    uint32_t saved_sym_count;
    bool    has_index;         /* aux marker present → inline index region at tail */
    size_t  index_offset;      /* 32-aligned start of the index region (has_index) */
} col_mapped_t;

static size_t col_str_pool_payload_len(const ray_t* vec) {
    if (!vec || vec->type != RAY_STR || !vec->str_pool || RAY_IS_ERR(vec->str_pool))
        return 0;
    return vec->str_pool->len > 0 ? (size_t)vec->str_pool->len : 0;
}

static ray_t* col_copy_str_pool(const col_mapped_t* cm) {
    if (!cm->has_str_pool) return NULL;

    const ray_t* src = (const ray_t*)((const char*)cm->mapped + cm->str_pool_offset);
    if (cm->str_pool_size == 0) return NULL;

    ray_t* pool = ray_alloc(cm->str_pool_size);
    if (!pool || RAY_IS_ERR(pool)) return pool ? pool : ray_error("oom", NULL);

    uint8_t saved_order = pool->order;
    uint8_t saved_mmod = pool->mmod;
    memcpy(pool, src, 32 + cm->str_pool_size);
    pool->order = saved_order;
    pool->mmod = saved_mmod;
    /* Pool header attrs are untrusted disk bytes (save writes 0) — mask
     * to the persisted allowlist, see COL_DISK_ATTRS_MASK. */
    pool->attrs &= COL_DISK_ATTRS_MASK;
    ray_atomic_store(&pool->rc, 1);
    return pool;
}

static ray_err_t col_validate_str_region(ray_t* hdr, const void* ptr,
                                         size_t mapped_size, col_mapped_t* out) {
    size_t offset = 32 + out->data_size;
    if (offset > mapped_size || mapped_size - offset < 32)
        return RAY_ERR_CORRUPT;

    ray_t* pool = (ray_t*)((char*)ptr + offset);
    if (pool->type != RAY_U8 || pool->len < 0)
        return RAY_ERR_CORRUPT;

    size_t pool_size = (size_t)pool->len;
    if (pool_size > mapped_size - offset - 32)
        return RAY_ERR_CORRUPT;

    const ray_str_t* elems = (const ray_str_t*)((const char*)ptr + 32);
    for (int64_t i = 0; i < hdr->len; i++) {
        uint32_t len = elems[i].len;
        if (len <= RAY_STR_INLINE_MAX) continue;
        if (pool_size == 0 || elems[i].pool_off > pool_size ||
            len > pool_size - elems[i].pool_off)
            return RAY_ERR_CORRUPT;
        if (len >= 4) {
            const char* p = (const char*)ptr + offset + 32 + elems[i].pool_off;
            if (memcmp(elems[i].prefix, p, 4) != 0)
                return RAY_ERR_CORRUPT;
        }
    }

    out->has_str_pool = true;
    out->str_pool_offset = offset;
    out->str_pool_size = pool_size;
    out->tail_offset = offset + 32 + pool_size;
    return RAY_OK;
}

static ray_t* col_validate_mapped(const char* path, col_mapped_t* out) {
    size_t mapped_size = 0;
    void* ptr = ray_vm_map_file(path, &mapped_size);
    if (!ptr) return ray_error("io", NULL);

    /* Reject extended-format files early.  STRV / STRL / LIST / TABLE
     * encode their payload and cannot satisfy the raw 32-byte ray_t
     * header contract that the rest of this validator assumes;
     * ray_col_load handles them via the magic-dispatch fast path
     * higher up.  Returning "nyi" here lets ray_col_mmap callers
     * fall back to ray_col_load (see splay_load_impl).  This must
     * come before the mapped_size < 32 check: 0-row STRV files are
     * 14 bytes, 1-row STRV files with content < 10 bytes also fall
     * under 32, and either would otherwise be rejected as "corrupt"
     * with no fallback path. */
    if (mapped_size >= 4) {
        uint32_t magic;
        memcpy(&magic, ptr, 4);
        if (magic == STR_LIST_MAGIC || magic == STR_VEC_MAGIC ||
            magic == LIST_MAGIC     || magic == TABLE_MAGIC) {
            ray_vm_unmap_file(ptr, mapped_size);
            return ray_error("nyi", NULL);
        }
    }

    if (mapped_size < 32) {
        ray_vm_unmap_file(ptr, mapped_size);
        return ray_error("corrupt", NULL);
    }

    ray_t* hdr = (ray_t*)ptr;

    /* Validate the on-disk format major version (the `order` byte) BEFORE
     * interpreting type/len.  FRESH SWAP, NO LEGACY: a file whose major
     * does not match the reader is rejected here.  The loaders below reset
     * the runtime `order` to its correct value, so reading the version out
     * of `order` here does not leak into the live block order. */
    if (ray_col_check_format(hdr) != RAY_OK) {
        ray_vm_unmap_file(ptr, mapped_size);
        return ray_error("version", "col %s: bad format version", path);
    }

    /* Validate type from untrusted file data -- allowlist only */
    if (!is_serializable_type(hdr->type)) {
        ray_vm_unmap_file(ptr, mapped_size);
        return ray_error("nyi", NULL);
    }
    if (hdr->len < 0) {
        ray_vm_unmap_file(ptr, mapped_size);
        return ray_error("corrupt", NULL);
    }

    uint8_t esz = ray_sym_elem_size(hdr->type, hdr->attrs);
    if (esz == 0 && hdr->len > 0) {
        const char* ht = ray_type_name(hdr->type);
        ray_vm_unmap_file(ptr, mapped_size);
        return ray_error("type", "col mmap %s: on-disk type %s has no fixed element size for %lld rows",
                         path, ht, (long long)hdr->len);
    }
    /* Overflow check: ensure len*esz fits in size_t with 32-byte header room */
    if ((uint64_t)hdr->len > (SIZE_MAX - 32) / (esz ? esz : 1)) {
        ray_vm_unmap_file(ptr, mapped_size);
        return ray_error("io", NULL);
    }
    size_t data_size = (size_t)hdr->len * esz;
    if (32 + data_size > mapped_size) {
        ray_vm_unmap_file(ptr, mapped_size);
        return ray_error("corrupt", NULL);
    }

    out->data_size = data_size;
    if (hdr->type == RAY_STR) {
        ray_err_t se = col_validate_str_region(hdr, ptr, mapped_size, out);
        if (se != RAY_OK) {
            ray_vm_unmap_file(ptr, mapped_size);
            return ray_error(ray_err_code_str(se), NULL);
        }
    } else {
        out->has_str_pool = false;
        out->str_pool_offset = 0;
        out->str_pool_size = 0;
        out->tail_offset = 32 + data_size;
    }

    /* RAY_SYM: record the saved dictionary count from the header rc
     * field (runtime-domain files: global count at save; domain-encoded
     * splayed columns: the symfile's count at save).  The fast-reject
     * against the live dictionary happens in the loaders, which know
     * which dictionary applies.  Use memcpy (not atomic_load) since
     * file data is not atomic storage. */
    if (hdr->type == RAY_SYM) {
        uint32_t saved_sc;
        memcpy(&saved_sc, (const char*)ptr + offsetof(ray_t, rc), sizeof(saved_sc));
        out->saved_sym_count = saved_sc;
    }

    /* Inline index region: the reserved aux[0..3] marker (read from the header's
     * first 4 bytes) distinguishes a file with an appended index from a plain,
     * current-format column.  Absent (the default, all existing data) → strict
     * payload-only layout; present → a 32-aligned index region follows. */
    out->has_index = false;
    out->index_offset = 0;
    {
        /* The marker lives in the header's aux[0..3] (first 4 file bytes) for
         * every column type — numeric and STR alike.  tail_offset already
         * accounts for the str_pool on STR, so region_off lands after it. */
        uint32_t idxmg;
        memcpy(&idxmg, ptr, 4);
        if (idxmg == COL_IDX_AUX_MAGIC) {
            size_t region_off = (out->tail_offset + 31) & ~(size_t)31;
            /* Need at least one RAY_INDEX block (32 hdr + sizeof payload). */
            if (region_off + 32 + sizeof(ray_index_t) <= mapped_size) {
                out->has_index = true;
                out->index_offset = region_off;
            }
        }
    }

    out->mapped          = ptr;
    out->mapped_size     = mapped_size;
    out->header          = hdr;
    out->esz             = esz;
    out->data_size       = data_size;
    return NULL;  /* success */
}

/* --------------------------------------------------------------------------
 * ray_col_load -- load a column file via mmap (zero deserialization)
 * -------------------------------------------------------------------------- */

static ray_t* col_load_impl(const char* path, struct ray_sym_domain_s* dom,
                            bool require_dom) {
    if (!path) return ray_error("io", NULL);

    /* Read file into temp mmap for validation, then copy to buddy block.
     * This avoids the mmap lifecycle problem (mmod=1 blocks are never freed). */
    size_t mapped_size = 0;
    void* ptr = ray_vm_map_file(path, &mapped_size);
    if (!ptr) return ray_error("io", NULL);

    /* Check for extended format magic numbers (first 4 bytes) */
    if (mapped_size >= 4) {
        uint32_t magic;
        memcpy(&magic, ptr, 4);

        if (magic == STR_LIST_MAGIC) {
            ray_t* result = col_load_str_list((const uint8_t*)ptr + 4, mapped_size - 4);
            ray_vm_unmap_file(ptr, mapped_size);
            return result;
        }
        if (magic == STR_VEC_MAGIC) {
            ray_t* result = col_load_str_vec((const uint8_t*)ptr + 4, mapped_size - 4);
            ray_vm_unmap_file(ptr, mapped_size);
            return result;
        }
        if (magic == LIST_MAGIC || magic == TABLE_MAGIC) {
            const uint8_t* p = (const uint8_t*)ptr + 4;
            size_t rem = mapped_size - 4;
            ray_t* result = col_read_recursive(&p, &rem);
            ray_vm_unmap_file(ptr, mapped_size);
            return result;
        }
    }
    /* Unmap the initial mapping; col_validate_mapped will re-map for validation */
    ray_vm_unmap_file(ptr, mapped_size);

    col_mapped_t cm = {0};
    ray_t* err = col_validate_mapped(path, &cm);
    if (err) return err;

    /* Allocate buddy block and copy file data */
    ray_t* vec = ray_alloc(cm.data_size);
    if (!vec || RAY_IS_ERR(vec)) {
        ray_vm_unmap_file(cm.mapped, cm.mapped_size);
        return vec ? vec : ray_error("oom", NULL);
    }
    uint8_t saved_order = vec->order;  /* preserve buddy order */
    memcpy(vec, cm.mapped, 32 + cm.data_size);
    /* The on-disk aux is reserved (postponed index persistence) — written
     * zero today, but foreign files may carry untrusted bytes.  Reconstruct
     * runtime aux by zeroing it BEFORE attaching str_pool / sym_domain
     * (which write aux[8..15]).  The on-disk `order` held the format
     * version; it is reset to saved_order below.  Plain columns want
     * all-zero aux. */
    memset(vec->aux, 0, 16);

    if (vec->type == RAY_STR) {
        ray_t* pool = col_copy_str_pool(&cm);
        if (pool && RAY_IS_ERR(pool)) {
            ray_vm_unmap_file(cm.mapped, cm.mapped_size);
            ray_free(vec);
            return pool;
        }
        vec->str_pool = pool;
    }

    ray_vm_unmap_file(cm.mapped, cm.mapped_size);

    /* Fix up header for buddy-allocated block.  Mask attrs to the
     * persisted allowlist (covers the old SLICE strip) — see
     * COL_DISK_ATTRS_MASK.  Must precede the SYM release path below. */
    vec->mmod = 0;
    vec->order = saved_order;
    vec->attrs &= COL_DISK_ATTRS_MASK;
    ray_atomic_store(&vec->rc, 1);

    /* RAY_SYM: attach the resolution domain + bounds check.  The masked
     * attrs + zeroed aux above mean no release path can run owned-ref
     * handling over a garbage pointer; attach the domain now. */
    if (vec->type == RAY_SYM) {
        if (dom) {
            /* Splayed/parted load: on-disk cells are positions in the
             * table's symfile — attach ITS domain (one ref per column). */
            vec->sym_domain = dom;
            ray_sym_domain_retain(dom);
            int64_t dcount = ray_sym_domain_count(dom);
            if (cm.saved_sym_count > 0 && (int64_t)cm.saved_sym_count > dcount) {
                ray_release(vec);
                return ray_error("corrupt",
                    "column %s: saved sym count %u exceeds symfile count %lld",
                    path, cm.saved_sym_count, (long long)dcount);
            }
            uint32_t bound = dcount > UINT32_MAX ? UINT32_MAX : (uint32_t)dcount;
            ray_err_t sym_err = validate_sym_bounds(ray_data(vec), vec->len,
                                                    vec->attrs, bound);
            if (sym_err != RAY_OK) {
                ray_release(vec);
                return ray_error(ray_err_code_str(sym_err), NULL);
            }
        } else if (require_dom) {
            /* A stored SYM column without a resolvable symfile must
             * never silently resolve against incidental state. */
            vec->sym_domain = ray_sym_runtime_domain();
            ray_release(vec);
            return ray_error("sym",
                "column %s: SYM data but no symfile resolved "
                "(missing <dir>/sym or explicit sym argument)", path);
        } else {
            /* Bare load: process-local runtime-id semantics. */
            vec->sym_domain = ray_sym_runtime_domain();
            uint32_t cur_sc = ray_sym_count();
            if (cm.saved_sym_count > 0 && cur_sc > 0 && cur_sc < cm.saved_sym_count) {
                ray_release(vec);
                return ray_error("corrupt", NULL);
            }
            ray_err_t sym_err = validate_sym_bounds(ray_data(vec), vec->len,
                                                    vec->attrs, cur_sc);
            if (sym_err != RAY_OK) {
                ray_release(vec);
                return ray_error(ray_err_code_str(sym_err), NULL);
            }
        }
    }

    try_load_link_sidecar(vec, path);

    return vec;
}

ray_t* ray_col_load(const char* path) {
    return col_load_impl(path, NULL, false);
}

ray_t* ray_col_load_dom(const char* path, struct ray_sym_domain_s* dom) {
    return col_load_impl(path, dom, true);
}

/* --------------------------------------------------------------------------
 * ray_col_mmap -- zero-copy column load via mmap (mmod=1)
 *
 * Returns a ray_t* backed directly by the file's mmap region.
 * MAP_PRIVATE gives COW semantics -- only the header page gets a private
 * copy when we write mmod/rc. All data pages stay shared with page cache.
 * ray_release -> ray_free -> munmap.
 * -------------------------------------------------------------------------- */

static ray_t* col_mmap_impl(const char* path, struct ray_sym_domain_s* dom,
                            bool require_dom) {
    if (!path) return ray_error("io", NULL);

    col_mapped_t cm = {0};
    ray_t* err = col_validate_mapped(path, &cm);
    if (err) return err;

    /* Validate that the file size matches the expected layout.  Without an
     * inline index the payload is the whole file (ray_free reconstructs the
     * munmap size from the same formula).  With an index, the file extends past
     * the payload by the 32-aligned index region — that's expected, not a tear. */
    if (!cm.has_index && cm.tail_offset != cm.mapped_size) {
        ray_vm_unmap_file(cm.mapped, cm.mapped_size);
        return ray_error("io", NULL);
    }

    ray_t* vec = cm.header;

    /* RAY_SYM: generic mmap validates indices by scanning the data.  Splayed
     * reads are the trusted local table format, so opening them must only map
     * files and validate headers; walking every symbol column would turn mmap
     * open into an eager cold-disk scan.  Files carry a saved dictionary
     * count for an O(1) reject — post-flip that is the SYMFILE's count at
     * save time, checked against the attached FILE domain. */
    if (vec->type == RAY_SYM) {
        if (dom) {
            int64_t dcount = ray_sym_domain_count(dom);
            if (cm.saved_sym_count > 0 && (int64_t)cm.saved_sym_count > dcount) {
                ray_vm_unmap_file(cm.mapped, cm.mapped_size);
                return ray_error("corrupt",
                    "column %s: saved sym count %u exceeds symfile count %lld",
                    path, cm.saved_sym_count, (long long)dcount);
            }
            if (cm.saved_sym_count == 0) {
                uint32_t bound = dcount > UINT32_MAX ? UINT32_MAX
                                                     : (uint32_t)dcount;
                ray_err_t sym_err = validate_sym_bounds(
                    (const char*)cm.mapped + 32, vec->len, vec->attrs, bound);
                if (sym_err != RAY_OK) {
                    ray_vm_unmap_file(cm.mapped, cm.mapped_size);
                    return ray_error(ray_err_code_str(sym_err), NULL);
                }
            }
        } else if (require_dom) {
            ray_vm_unmap_file(cm.mapped, cm.mapped_size);
            return ray_error("sym",
                "column %s: SYM data but no symfile resolved "
                "(missing <dir>/sym or explicit sym argument)", path);
        } else {
            /* Bare load: process-local runtime-id semantics.  Fast-reject a
             * file whose saved dictionary count exceeds the live table — it
             * cannot have been written by this process lineage. */
            uint32_t cur_sc = ray_sym_count();
            if (cm.saved_sym_count > 0 && cur_sc > 0 &&
                cur_sc < cm.saved_sym_count) {
                ray_vm_unmap_file(cm.mapped, cm.mapped_size);
                return ray_error("corrupt", NULL);
            }
            ray_err_t sym_err = validate_sym_bounds(
                (const char*)cm.mapped + 32, vec->len, vec->attrs, cur_sc);
            if (sym_err != RAY_OK) {
                ray_vm_unmap_file(cm.mapped, cm.mapped_size);
                return ray_error(ray_err_code_str(sym_err), NULL);
            }
        }
    }

    /* Patch header -- MAP_PRIVATE COW: only the header page gets copied.
     * Mask attrs to the persisted allowlist (covers the old SLICE strip)
     * — see COL_DISK_ATTRS_MASK.  Must precede the link-sidecar reattach
     * below and any release of the loaded header. */
    vec->mmod = 1;
    vec->order = 0;  /* on-disk `order` held the format version; the mmap
                      * free path uses mapped_size, not order — reset it to
                      * the prior runtime value (0) so nothing reads the
                      * version as a live block order. */
    vec->attrs &= COL_DISK_ATTRS_MASK;
    ray_atomic_store(&vec->rc, 1);
    /* COW header page: the mapped aux is reserved (postponed index) and
     * written zero today; reconstruct runtime aux by zeroing it.  The
     * str_pool / RAY_SYM domain patches below overwrite aux[8..15]. */
    memset(vec->aux, 0, 16);

    if (vec->type == RAY_STR) {
        ray_t* pool = (ray_t*)((char*)cm.mapped + cm.str_pool_offset);
        pool->mmod = 2;
        pool->order = 0;
        /* Untrusted disk attrs — mask, see COL_DISK_ATTRS_MASK. */
        pool->attrs &= COL_DISK_ATTRS_MASK;
        ray_atomic_store(&pool->rc, 1);
        vec->str_pool = pool;
    }

    /* RAY_SYM: attach the resolution domain (header page is MAP_PRIVATE
     * COW, same mechanism as the str_pool patch above).  Splayed loads
     * attach the table's symfile domain (cells are positions in it);
     * bare loads attach the runtime singleton (cells are runtime ids). */
    if (vec->type == RAY_SYM) {
        if (dom) {
            vec->sym_domain = dom;
            ray_sym_domain_retain(dom);
        } else {
            vec->sym_domain = ray_sym_runtime_domain();
        }
    }

    /* Reattach link sidecar if present.  Without this, linked columns
     * round-tripped through splay-mmap (splay.c:184) lose HAS_LINK
     * even though ray_col_load restores it. */
    try_load_link_sidecar(vec, path);

    /* Attach the inline index region: mapped in place (zero-copy), patched to
     * absolute child pointers, and flagged RAY_MARK_MMAP so it rides the
     * column's single mapping.  ray_free reads the full mapping size from the
     * reserved _idx_pad slot to munmap the whole region (payload + index). */
    if (cm.has_index) {
        ray_t* idx = ray_index_inline_map((uint8_t*)cm.mapped + cm.index_offset);
        if (idx) {   /* NULL = stale index layout generation: load unindexed */
            ray_t* r = ray_index_attach_built(&vec, idx);
            if (r && !RAY_IS_ERR(r)) vec = r;
        }
        /* The munmap size is derived from the column + index at free time
         * (ray_free), so no aux slot is needed — leaving str_pool intact on STR. */
        /* Attach failure → column loads unindexed; correctness unaffected. */
    }

    return vec;
}

ray_t* ray_col_mmap(const char* path) {
    return col_mmap_impl(path, NULL, false);
}

ray_t* ray_col_mmap_splayed_dom(const char* path, struct ray_sym_domain_s* dom) {
    return col_mmap_impl(path, dom, true);
}
