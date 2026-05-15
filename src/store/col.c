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
#include "ops/idxop.h"
#include "vec/str.h"
#include <string.h>
#include <stdio.h>
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
 *   Bytes 0-15:  nullmap (inline) or zeroed (ext_nullmap / no nulls)
 *   Bytes 16-31: mmod=0, order=0, type, attrs, rc=0, len
 *   Bytes 32+:   raw element data
 *   (if RAY_ATTR_NULLMAP_EXT): appended (len+7)/8 bitmap bytes
 *
 * On-disk format IS the in-memory format (zero deserialization on load).
 * -------------------------------------------------------------------------- */

/* Explicit allowlist of types that are safe to serialize as raw bytes.
 * Fixed-size scalar types plus RAY_STR.  RAY_STR has a pointer-bearing pool
 * in memory, but the column format stores the fixed 16-byte descriptors and
 * the byte pool as adjacent raw regions so mmap can restore the pointer
 * without deserializing string contents. */
static bool is_serializable_type(int8_t t) {
    switch (t) {
    case RAY_BOOL: case RAY_U8:   case RAY_I16:
    case RAY_I32:  case RAY_I64:  case RAY_F64:
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
    if (remaining > (size_t)INT64_MAX) return ray_error("range", NULL);
    int64_t len = (int64_t)remaining;
    ray_t* result = ray_de_raw((uint8_t*)ptr, &len);
    if (!result || RAY_IS_ERR(result)) return result;
    if (result->type != RAY_STR) {
        ray_release(result);
        return ray_error("type", NULL);
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
 *     other:       [8B raw value]
 *   vectors with is_serializable_type: [8B len][raw data]
 *   RAY_LIST: [8B count][recursive elements...]
 *   RAY_TABLE: [8B ncols][8B nrows][for each col: 8B name_sym + recursive col]
 * -------------------------------------------------------------------------- */

static ray_err_t col_write_recursive(ray_t* obj, FILE* f);

static ray_err_t col_write_recursive(ray_t* obj, FILE* f) {
    if (!obj || RAY_IS_ERR(obj)) return RAY_ERR_TYPE;

    int8_t type = obj->type;
    if (fwrite(&type, 1, 1, f) != 1) return RAY_ERR_IO;

    if (type < 0) {
        /* Atom */
        if (type == -RAY_STR) {
            const char* sp = ray_str_ptr(obj);
            size_t slen = ray_str_len(obj);
            uint32_t len32 = (uint32_t)slen;
            if (fwrite(&len32, 4, 1, f) != 1) return RAY_ERR_IO;
            if (slen > 0 && fwrite(sp, 1, slen, f) != slen) return RAY_ERR_IO;
        } else {
            /* Fixed-size atom: write 8 bytes of the value union */
            if (fwrite(&obj->i64, 8, 1, f) != 1) return RAY_ERR_IO;
        }
        return RAY_OK;
    }

    if (is_serializable_type(type)) {
        /* Fixed-size vector: write len + raw data.
         * RAY_SYM: also write attrs byte (adaptive width W8/W16/W32/W64).
         * RAY_STR: also write attrs byte and the adjacent byte pool. */
        int64_t len = obj->len;
        if (fwrite(&len, 8, 1, f) != 1) return RAY_ERR_IO;
        if (type == RAY_SYM || type == RAY_STR) {
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
        if (type == -RAY_STR) {
            if (*remaining < 4) return ray_error("corrupt", NULL);
            uint32_t slen;
            memcpy(&slen, *pp, 4);
            *pp += 4; *remaining -= 4;
            if (slen > *remaining) return ray_error("corrupt", NULL);
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

    if (is_serializable_type(type)) {
        /* Fixed-size vector */
        if (*remaining < 8) return ray_error("corrupt", NULL);
        int64_t len;
        memcpy(&len, *pp, 8);
        *pp += 8; *remaining -= 8;
        if (len < 0) return ray_error("corrupt", NULL);

        /* RAY_SYM / RAY_STR: read attrs byte for adaptive width/nulls */
        uint8_t attrs = 0;
        if (type == RAY_SYM || type == RAY_STR) {
            if (*remaining < 1) return ray_error("corrupt", NULL);
            memcpy(&attrs, *pp, 1);
            *pp += 1; *remaining -= 1;
        }

        uint8_t esz = ray_sym_elem_size(type, attrs);
        if (esz > 0 && (uint64_t)len > SIZE_MAX / esz)
            return ray_error("corrupt", NULL);
        size_t data_size = (size_t)len * esz;
        if (data_size > *remaining) return ray_error("corrupt", NULL);

        ray_t* vec = (type == RAY_SYM)
            ? ray_sym_vec_new(attrs & RAY_SYM_W_MASK, len)
            : ray_vec_new(type, len);
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
            vec->attrs = attrs;
        }

        if (type == RAY_SYM) {
            uint32_t sc = ray_sym_count();
            ray_err_t ve = validate_sym_bounds(ray_data(vec), len, attrs, sc);
            if (ve != RAY_OK) { ray_release(vec); return ray_error(ray_err_code_str(ve), NULL); }
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
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, lf);
    fclose(lf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'
                  || buf[n-1] == ' '  || buf[n-1] == '\t'
                  || buf[n-1] == '\0')) n--;
    if (n == 0) return;
    int64_t target_sym = ray_sym_intern(buf, n);
    if (target_sym < 0) return;
    vec->link_target = target_sym;
    vec->attrs |= RAY_ATTR_HAS_LINK;
}

/* --------------------------------------------------------------------------
 * ray_col_save -- write a vector to a column file
 * -------------------------------------------------------------------------- */

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

    {
        FILE* f = fopen(tmp_path, "wb");
        if (!f) return RAY_ERR_IO;

        /* Write a clean header (mmod=0, rc=0) */
        ray_t header;
        memcpy(&header, vec, 32);
        header.mmod = 0;
        header.order = 0;
        /* For RAY_SYM: store sym count in rc field (always 0 on disk otherwise).
         * This serves as O(1) fast-reject metadata on load. */
        header.rc = (vec->type == RAY_SYM) ? ray_sym_count() : 0;

        /* HAS_INDEX rebase: an attached accelerator index displaces the
         * 16-byte nullmap union with an index pointer.  Persist the
         * pre-attach state instead — strip HAS_INDEX, restore the saved
         * NULLMAP_EXT bit, and copy the saved bitmap bytes back into the
         * on-disk header.  ext_for_append captures the saved ext-nullmap
         * pointer so the bitmap append at end-of-write reads from the
         * right place. */
        ray_t* ext_for_append = (vec->attrs & RAY_ATTR_NULLMAP_EXT)
                                ? vec->ext_nullmap : NULL;
        if (vec->attrs & RAY_ATTR_HAS_INDEX) {
            ray_index_t* ix = ray_index_payload(vec->index);
            header.attrs &= ~RAY_ATTR_HAS_INDEX;
            if (ix->saved_attrs & RAY_ATTR_NULLMAP_EXT) {
                header.attrs |= RAY_ATTR_NULLMAP_EXT;
                memcpy(&ext_for_append, &ix->saved_nullmap[0],
                       sizeof(ext_for_append));
            } else {
                header.attrs &= ~RAY_ATTR_NULLMAP_EXT;
                ext_for_append = NULL;
            }
            memcpy(header.nullmap, ix->saved_nullmap, 16);
        }

        /* HAS_LINK rebase: target sym ID lives at header.nullmap[8..15],
         * but sym IDs are process-local — the on-disk file would be
         * useless across runs.  Strip the bit and zero the slot; the
         * sidecar `.link` file (written below after rename) carries the
         * target table name in text form for portable restoration. */
        if (vec->attrs & RAY_ATTR_HAS_LINK) {
            header.attrs &= (uint8_t)~RAY_ATTR_HAS_LINK;
            memset(header.nullmap + 8, 0, 8);
        }

        /* Clear slice field; preserve ext_nullmap flag for bitmap append */
        header.attrs &= ~RAY_ATTR_SLICE;
        if (!(header.attrs & RAY_ATTR_HAS_NULLS)) {
            memset(header.nullmap, 0, 16);
            header.attrs &= ~RAY_ATTR_NULLMAP_EXT;
        } else if (header.attrs & RAY_ATTR_NULLMAP_EXT) {
            /* Ext bitmap appended after data; zero pointer bytes in header */
            memset(header.nullmap, 0, 16);
        }

        size_t written = fwrite(&header, 1, 32, f);
        if (written != 32) { fclose(f); remove(tmp_path); return RAY_ERR_IO; }

        /* Write data */
        if (vec->len < 0) { fclose(f); remove(tmp_path); return RAY_ERR_CORRUPT; }
        uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
        if (esz == 0 && vec->len > 0) { fclose(f); remove(tmp_path); return RAY_ERR_TYPE; }
        /* Overflow check: ensure len*esz fits in size_t with 32-byte header room */
        if ((uint64_t)vec->len > (SIZE_MAX - 32) / (esz ? esz : 1)) {
            fclose(f);
            remove(tmp_path);
            return RAY_ERR_IO;
        }
        size_t data_size = (size_t)vec->len * esz;

        void* data;
        if (vec->attrs & RAY_ATTR_SLICE) {
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
            if (written != data_size) { fclose(f); remove(tmp_path); return RAY_ERR_IO; }
        }

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

        /* Append external nullmap bitmap after data.  Use header.attrs
         * (rebased above for HAS_INDEX) and ext_for_append (the
         * effective ext_nullmap pointer, possibly extracted from the
         * index's saved snapshot). */
        if ((vec->attrs & RAY_ATTR_HAS_NULLS) &&
            (header.attrs & RAY_ATTR_NULLMAP_EXT) && ext_for_append) {
            size_t bitmap_len = ((size_t)vec->len + 7) / 8;
            written = fwrite(ray_data(ext_for_append), 1, bitmap_len, f);
            if (written != bitmap_len) { fclose(f); remove(tmp_path); return RAY_ERR_IO; }
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
                                ray_file_rename(tmp_link, link_path);
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
    size_t  bitmap_offset;
    bool    has_ext_nullmap;
    size_t  bitmap_len;
    uint32_t saved_sym_count;
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
    pool->attrs &= (uint8_t)~RAY_ATTR_SLICE;
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
    out->bitmap_offset = offset + 32 + pool_size;
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
        ray_vm_unmap_file(ptr, mapped_size);
        return ray_error("type", NULL);
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
    size_t bitmap_offset = 32 + data_size;
    if (hdr->type == RAY_STR) {
        ray_err_t se = col_validate_str_region(hdr, ptr, mapped_size, out);
        if (se != RAY_OK) {
            ray_vm_unmap_file(ptr, mapped_size);
            return ray_error(ray_err_code_str(se), NULL);
        }
        bitmap_offset = out->bitmap_offset;
    } else {
        out->has_str_pool = false;
        out->str_pool_offset = 0;
        out->str_pool_size = 0;
        out->bitmap_offset = bitmap_offset;
    }

    /* Check for appended ext_nullmap bitmap */
    bool has_ext_nullmap = (hdr->attrs & RAY_ATTR_HAS_NULLS) &&
                           (hdr->attrs & RAY_ATTR_NULLMAP_EXT);
    size_t bitmap_len = has_ext_nullmap ? ((size_t)hdr->len + 7) / 8 : 0;
    if (has_ext_nullmap && (bitmap_offset > mapped_size ||
                            bitmap_len > mapped_size - bitmap_offset)) {
        ray_vm_unmap_file(ptr, mapped_size);
        return ray_error("corrupt", NULL);
    }

    /* RAY_SYM: fast-reject via sym count in header rc field.
     * Use memcpy (not atomic_load) since file data is not atomic storage. */
    if (hdr->type == RAY_SYM) {
        uint32_t saved_sc;
        memcpy(&saved_sc, (const char*)ptr + offsetof(ray_t, rc), sizeof(saved_sc));
        out->saved_sym_count = saved_sc;
        uint32_t cur_sc = ray_sym_count();
        if (saved_sc > 0 && cur_sc > 0 && cur_sc < saved_sc) {
            ray_vm_unmap_file(ptr, mapped_size);
            return ray_error("corrupt", NULL);
        }
    }

    out->mapped          = ptr;
    out->mapped_size     = mapped_size;
    out->header          = hdr;
    out->esz             = esz;
    out->data_size       = data_size;
    out->bitmap_offset   = bitmap_offset;
    out->has_ext_nullmap = has_ext_nullmap;
    out->bitmap_len      = bitmap_len;
    return NULL;  /* success */
}

/* --------------------------------------------------------------------------
 * col_restore_ext_nullmap -- allocate buddy-backed copy of ext nullmap
 *
 * Shared by ray_col_load and ray_col_mmap. On success, sets vec->ext_nullmap.
 * Returns NULL on success, or an error string on failure.
 * -------------------------------------------------------------------------- */

static ray_t* col_restore_ext_nullmap(ray_t* vec, const col_mapped_t* cm) {
    ray_t* ext = ray_vec_new(RAY_U8, (int64_t)cm->bitmap_len);
    if (!ext || RAY_IS_ERR(ext)) return ray_error("oom", NULL);
    ext->len = (int64_t)cm->bitmap_len;
    memcpy(ray_data(ext), (char*)cm->mapped + cm->bitmap_offset, cm->bitmap_len);
    vec->ext_nullmap = ext;
    return NULL;  /* success */
}

/* --------------------------------------------------------------------------
 * ray_col_load -- load a column file via mmap (zero deserialization)
 * -------------------------------------------------------------------------- */

ray_t* ray_col_load(const char* path) {
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

    if (vec->type == RAY_STR) {
        ray_t* pool = col_copy_str_pool(&cm);
        if (pool && RAY_IS_ERR(pool)) {
            ray_vm_unmap_file(cm.mapped, cm.mapped_size);
            ray_free(vec);
            return pool;
        }
        vec->str_pool = pool;
    }

    /* Restore external nullmap if present */
    if (cm.has_ext_nullmap) {
        ray_t* ext_err = col_restore_ext_nullmap(vec, &cm);
        if (ext_err) {
            ray_vm_unmap_file(cm.mapped, cm.mapped_size);
            ray_free(vec);
            return ext_err;
        }
    }

    ray_vm_unmap_file(cm.mapped, cm.mapped_size);

    /* Fix up header for buddy-allocated block */
    vec->mmod = 0;
    vec->order = saved_order;
    vec->attrs &= ~RAY_ATTR_SLICE;
    if (!cm.has_ext_nullmap)
        vec->attrs &= ~RAY_ATTR_NULLMAP_EXT;
    ray_atomic_store(&vec->rc, 1);

    /* RAY_SYM: validate sym count footer + bounds check */
    if (vec->type == RAY_SYM) {
        ray_err_t sym_err = validate_sym_bounds(ray_data(vec), vec->len,
                                                vec->attrs, ray_sym_count());
        if (sym_err != RAY_OK) {
            ray_release(vec);
            return ray_error(ray_err_code_str(sym_err), NULL);
        }
    }

    try_load_link_sidecar(vec, path);

    return vec;
}

/* --------------------------------------------------------------------------
 * ray_col_mmap -- zero-copy column load via mmap (mmod=1)
 *
 * Returns a ray_t* backed directly by the file's mmap region.
 * MAP_PRIVATE gives COW semantics -- only the header page gets a private
 * copy when we write mmod/rc. All data pages stay shared with page cache.
 * ray_release -> ray_free -> munmap.
 * -------------------------------------------------------------------------- */

static ray_t* col_mmap_impl(const char* path, bool trust_splayed_sym_count) {
    if (!path) return ray_error("io", NULL);

    col_mapped_t cm = {0};
    ray_t* err = col_validate_mapped(path, &cm);
    if (err) return err;

    /* Validate that file size matches expected layout exactly.
     * ray_free() reconstructs the munmap size using the same formula. */
    size_t expected = cm.bitmap_offset + cm.bitmap_len;
    if (expected != cm.mapped_size) {
        ray_vm_unmap_file(cm.mapped, cm.mapped_size);
        return ray_error("io", NULL);
    }

    ray_t* vec = cm.header;

    /* RAY_SYM: generic mmap validates indices by scanning the data.  Splayed
     * reads are the trusted local table format, so opening them must only map
     * files and validate headers; walking every symbol column would turn mmap
     * open into an eager cold-disk scan.  New files carry a saved symbol count
     * for an O(1) reject.  Older files have zero there, so they rely on the
     * loaded global symfile contract instead. */
    if (vec->type == RAY_SYM) {
        uint32_t cur_sc = ray_sym_count();
        uint32_t trusted_sc = trust_splayed_sym_count ? ray_sym_persisted_count() : cur_sc;
        if (trust_splayed_sym_count && cm.saved_sym_count > trusted_sc) {
            ray_vm_unmap_file(cm.mapped, cm.mapped_size);
            return ray_error("corrupt", NULL);
        }
        bool skip_bounds = trust_splayed_sym_count && trusted_sc > 0;
        if (!skip_bounds) {
            ray_err_t sym_err = validate_sym_bounds(
                (const char*)cm.mapped + 32, vec->len, vec->attrs, cur_sc);
            if (sym_err != RAY_OK) {
                ray_vm_unmap_file(cm.mapped, cm.mapped_size);
                return ray_error(ray_err_code_str(sym_err), NULL);
            }
        }
    }

    /* Restore external nullmap: allocate buddy-backed copy
     * (ext_nullmap must be a proper ray_t for ref counting) */
    if (cm.has_ext_nullmap) {
        ray_t* ext_err = col_restore_ext_nullmap(vec, &cm);
        if (ext_err) {
            ray_vm_unmap_file(cm.mapped, cm.mapped_size);
            return ext_err;
        }
    }

    /* Patch header -- MAP_PRIVATE COW: only the header page gets copied */
    vec->mmod = 1;
    vec->order = 0;
    vec->attrs &= ~RAY_ATTR_SLICE;
    if (!cm.has_ext_nullmap)
        vec->attrs &= ~RAY_ATTR_NULLMAP_EXT;
    ray_atomic_store(&vec->rc, 1);

    if (vec->type == RAY_STR) {
        ray_t* pool = (ray_t*)((char*)cm.mapped + cm.str_pool_offset);
        pool->mmod = 2;
        pool->order = 0;
        pool->attrs &= (uint8_t)~RAY_ATTR_SLICE;
        ray_atomic_store(&pool->rc, 1);
        vec->str_pool = pool;
    }

    /* Reattach link sidecar if present.  Without this, linked columns
     * round-tripped through splay-mmap (splay.c:184) lose HAS_LINK
     * even though ray_col_load restores it. */
    try_load_link_sidecar(vec, path);

    return vec;
}

ray_t* ray_col_mmap(const char* path) {
    return col_mmap_impl(path, false);
}

ray_t* ray_col_mmap_splayed(const char* path) {
    return col_mmap_impl(path, true);
}
