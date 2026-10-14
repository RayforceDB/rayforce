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
#ifndef RAY_ENCW_H
#define RAY_ENCW_H

#include <rayforce.h>
#include <stdio.h>
#include "store/enc.h"
#include "store/encfmt.h"
#include "store/encidx.h"

typedef struct {
    ray_enc_desc_t desc;        /* payload_off is set by the column writer */
    ray_enc_buf_t  payload;
    int            n_nulls;
    bool           has_stats;
    int64_t        sum_hi;
    uint64_t       sum_lo;
    uint64_t       str_pooled;
} ray_encw_vec_t;

void      ray_encw_vec_init(ray_encw_vec_t* v);
void      ray_encw_vec_free(ray_encw_vec_t* v);
ray_err_t ray_encw_vec(int8_t type, uint8_t attrs, const void* data, const char* str_pool,
                       int64_t start, int n, const ray_enc_dict_t* dict, ray_encw_vec_t* out);
bool      ray_encw_type_has_dict(int8_t type);
bool      ray_encw_type_summable(int8_t type);
bool      ray_encw_type_supported(int8_t type);
/* Test hook: every following vector uses top-level encoding `enc` where it
 * is legal for the type and applicable to the values (all-null vectors stay
 * CONST); -1 restores selection.  A forced encoding that is not applicable
 * fails the vector with RAY_ERR_NYI. */
void      ray_encw_test_force(int enc);

/* Column assembly (spec "Format"): begin, one chunk per 65536 rows, finish. */
typedef struct {
    FILE*    f;
    int8_t   type;
    uint8_t  attrs;            /* source attrs; masked + HAS_NULLS derived at finish */
    int64_t  len;
    uint32_t sym_count;        /* outer header rc (SYM) */
    uint8_t  index_kind;       /* the recorded kind; a region kind needs a region (set_index*) */
    uint8_t  col_flags;        /* RAY_ENC_FLAG_UNIQUE */
    int64_t  n_chunks, chunk_i;
    ray_enc_dirent_t* dir;
    uint64_t pos;              /* bytes written == current file offset */
    uint64_t str_pool_bytes;
    bool     any_bitmap_null;
    uint64_t* dict_v; uint32_t dict_n; bool dict_used;
    ray_enc_buf_t  chunk;
    ray_encw_vec_t vec;
    ray_enc_idx_src_t idx; bool has_idx;              /* region from a heap root */
    const uint8_t* idx_copy; uint64_t idx_copy_len;   /* region copied verbatim */
} ray_encw_t;
ray_err_t ray_encw_begin(ray_encw_t* w, FILE* f, int8_t type, uint8_t attrs, int64_t len,
                         uint32_t sym_count, uint8_t index_kind, uint8_t col_flags);
ray_err_t ray_encw_chunk(ray_encw_t* w, const void* data, const char* str_pool, int64_t n);
ray_err_t ray_encw_finish(ray_encw_t* w);
void      ray_encw_free(ray_encw_t* w);
/* Write s's index as the region (index_kind = the root's kind).  The root
 * must stay alive until ray_encw_finish; s->attrs is replaced by the
 * column's final persisted attrs at finish.  A region writer failure leaves
 * its "index region: ..." message in ray_error_msg(). */
void      ray_encw_set_index(ray_encw_t* w, const ray_enc_idx_src_t* s);
/* Copy `len` bytes of an already validated region of `kind` verbatim. */
void      ray_encw_set_index_copy(ray_encw_t* w, uint8_t kind, const uint8_t* region, uint64_t len);

/* An all-null generation-2 column image of `len` rows in an anonymous
 * mapping (*out, *out_size; ray_vm_unmap_file releases it).  `fill`: min(len,
 * 65536) null cells of `type` (generation-0 width; SYM at `attrs` width). */
ray_err_t ray_encw_null_image(int8_t type, uint8_t attrs, int64_t len, uint32_t sym_count,
                              const void* fill, void** out, size_t* out_size);

#endif /* RAY_ENCW_H */
