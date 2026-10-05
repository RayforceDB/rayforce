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
#  define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#  define _GNU_SOURCE
#endif
#include "store/col.h"
#include "store/encw.h"
#include "store/fileio.h"
#include "mem/heap.h"
#include "ops/idxop.h"
#include "table/sym.h"
#include "table/domain.h"
#include <stdio.h>
#include <string.h>

/* tmp -> fsync (durable) -> rename -> dir sync; removes a stale .link
 * sidecar (generation-2 columns are never linked). */
static ray_err_t colw_publish(const char* tmp, const char* path, bool durable) {
    ray_err_t e;
    if (durable) {
        ray_fd_t fd = ray_file_open(tmp, RAY_OPEN_READ | RAY_OPEN_WRITE);
        if (fd == RAY_FD_INVALID) { remove(tmp); return RAY_ERR_IO; }
        e = ray_file_sync(fd);
        ray_file_close(fd);
        if (e != RAY_OK) { remove(tmp); return e; }
    }
    if ((e = ray_file_rename(tmp, path)) != RAY_OK) { remove(tmp); return e; }
    if (durable && (e = ray_file_sync_dir(path)) != RAY_OK) return e;
    char link[1100];
    if (snprintf(link, sizeof link, "%s.link", path) < (int)sizeof link) remove(link);
    return RAY_OK;
}

/* Generation 2 holds plain vectors of BOOL..STR without a link and without
 * a HASH, SORT, BLOOM or PART index (spec P1: those stay generation 0 until
 * the index region exists). */
bool ray_col_gen2_eligible(ray_t* col) {
    if (!col || RAY_IS_ERR(col)) return false;
    if (!ray_encw_type_supported(col->type)) return false;
    if (col->attrs & RAY_ATTR_HAS_LINK) return false;
    switch (ray_index_kind(col)) {
    case RAY_IDX_HASH: case RAY_IDX_SORT: case RAY_IDX_BLOOM: case RAY_IDX_PART: return false;
    default: return true;
    }
}

/* The index kind and UNIQUE flag a plain column records in the enc header:
 * ZONE and CHUNK_ZONE (synthesized at materialization), UNIQUE as a flag;
 * DICT and a marker-only root record none. */
void ray_col_gen2_index_meta(ray_t* col, uint8_t* index_kind, uint8_t* col_flags) {
    *index_kind = 0; *col_flags = 0;
    if (!ray_index_has(col)) return;
    ray_index_t* ix = ray_index_payload(col->index);
    if (ix->kind == RAY_IDX_ZONE || ix->kind == RAY_IDX_CHUNK_ZONE) *index_kind = ix->kind;
    if (ix->markers & RAY_MARK_UNIQUE) *col_flags |= RAY_ENC_FLAG_UNIQUE;
}

static ray_err_t colw_gen2_vec(ray_t* vec, const char* path, bool durable) {
    char tmp[1100];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return RAY_ERR_IO;
    FILE* f = fopen(tmp, "wb+");
    if (!f) return RAY_ERR_IO;
    uint8_t ik, cf;
    ray_col_gen2_index_meta(vec, &ik, &cf);
    /* the writer applies the same matrix as the validator (spec step 3) */
    if ((ik && !ray_index_kind_admits((ray_idx_kind_t)ik, vec->type)) ||
        ((cf & RAY_ENC_FLAG_UNIQUE) && !ray_index_kind_admits(RAY_IDX_NONE, vec->type))) {
        fclose(f); remove(tmp);
        return RAY_ERR_NYI;
    }
    uint32_t sc = 0;
    if (vec->type == RAY_SYM) {
        int64_t dc = ray_sym_domain_count(ray_sym_vec_domain(vec));
        sc = dc > UINT32_MAX ? UINT32_MAX : (uint32_t)dc;
    }
    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    const char* data = (const char*)ray_data(vec);          /* slice-safe */
    ray_t* owner = (vec->attrs & RAY_ATTR_SLICE) ? vec->slice_parent : vec;
    const char* pool = (vec->type == RAY_STR && owner->str_pool && !RAY_IS_ERR(owner->str_pool))
                       ? (const char*)ray_data(owner->str_pool) : NULL;
    ray_encw_t w;
    ray_err_t e = ray_encw_begin(&w, f, vec->type, vec->attrs, vec->len, sc, ik, cf);
    for (int64_t c = 0; e == RAY_OK && c < w.n_chunks; c++) {
        int64_t n = vec->len - c * RAY_ENC_CHUNK;
        if (n > RAY_ENC_CHUNK) n = RAY_ENC_CHUNK;
        e = ray_encw_chunk(&w, data + (size_t)c * RAY_ENC_CHUNK * esz, pool, n);
    }
    if (e == RAY_OK) e = ray_encw_finish(&w);
    ray_encw_free(&w);
    if (fclose(f) != 0 && e == RAY_OK) e = RAY_ERR_IO;
    if (e != RAY_OK) { remove(tmp); return e; }
    return colw_publish(tmp, path, durable);
}

static ray_err_t colw_save_gen(ray_t* vec, const char* path, uint8_t gen, bool durable) {
    if (!vec || RAY_IS_ERR(vec)) return RAY_ERR_TYPE;
    if (!path) return RAY_ERR_IO;
    if (gen == 2 && ray_col_gen2_eligible(vec)) return colw_gen2_vec(vec, path, durable);
    if (gen != 0 && gen != 2) return RAY_ERR_VERSION;
    return durable ? ray_col_save(vec, path) : ray_col_save_bulk(vec, path);
}

ray_err_t ray_col_save_gen(ray_t* vec, const char* path, uint8_t gen) {
    return colw_save_gen(vec, path, gen, true);
}
ray_err_t ray_col_save_bulk_gen(ray_t* vec, const char* path, uint8_t gen) {
    return colw_save_gen(vec, path, gen, false);
}
