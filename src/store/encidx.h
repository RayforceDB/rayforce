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

#ifndef RAY_ENCIDX_H
#define RAY_ENCIDX_H

/* Generation-2 index region (spec "Index region").  Little-endian and
 * ABI-independent: never a copy of ray_index_t, never an address.  The
 * part above RAY_ENC_IDX_WIRE_ONLY is freestanding (stdint / stddef only)
 * so the wasm32 layout check compiles it. */

#include <stddef.h>
#include <stdint.h>

#define RAY_ENC_IDX_ROOT_BYTES   80
#define RAY_ENC_IDX_FIRST_CHILD  96      /* pad32(80): children start here */
#define RAY_ENC_IDX_NAME_MAX     255     /* order_name: a column (file) name */
#define RAY_ENC_IDX_BLOOM_K_MAX  3       /* k of ray_index_attach_bloom */

typedef struct {
    int8_t   parent_type;      /*  0 the column's logical type */
    uint8_t  rsv[7];           /*  1 */
    int64_t  built_for_len;    /*  8 */
    uint8_t  kind_area[64];    /* 16 the kind's fields, unused tail zero */
} ray_enc_idx_root_t;

typedef struct {               /* HASH, in kind_area */
    int64_t  table, gkeys, offs, rows;
    uint64_t mask;
    int64_t  n_keys, n_groups, order_name;
} ray_enc_idx_hash_t;

typedef struct { int64_t perm; } ray_enc_idx_sort_t;

typedef struct {               /* BLOOM */
    int64_t  bits;
    uint64_t m_mask;
    uint32_t k;
    uint32_t rsv;
    int64_t  n_keys;
} ray_enc_idx_bloom_t;

typedef struct {               /* PART */
    int64_t  keys, starts, lens, n_parts, order_name;
} ray_enc_idx_part_t;

#define ENCIDX_OFF(T, f, o) _Static_assert(offsetof(T, f) == (o), #T "." #f " offset")
_Static_assert(sizeof(ray_enc_idx_root_t) == RAY_ENC_IDX_ROOT_BYTES, "wire root is 80 bytes");
ENCIDX_OFF(ray_enc_idx_root_t, parent_type, 0);
ENCIDX_OFF(ray_enc_idx_root_t, built_for_len, 8);
ENCIDX_OFF(ray_enc_idx_root_t, kind_area, 16);
_Static_assert(sizeof(ray_enc_idx_hash_t) == 64, "HASH fills the kind area");
ENCIDX_OFF(ray_enc_idx_hash_t, mask, 32);
ENCIDX_OFF(ray_enc_idx_hash_t, order_name, 56);
_Static_assert(sizeof(ray_enc_idx_sort_t) == 8, "SORT kind fields");
_Static_assert(sizeof(ray_enc_idx_bloom_t) == 32, "BLOOM kind fields");
ENCIDX_OFF(ray_enc_idx_bloom_t, k, 16);
ENCIDX_OFF(ray_enc_idx_bloom_t, n_keys, 24);
_Static_assert(sizeof(ray_enc_idx_part_t) == 40, "PART kind fields");
ENCIDX_OFF(ray_enc_idx_part_t, order_name, 32);

/* The index kinds that carry a region: HASH, SORT, BLOOM, PART. */
static inline int ray_enc_idx_kind_has_region(uint8_t k) {
    return k == 1 || k == 2 || k == 4 || k == 6;
}

#ifndef RAY_ENC_IDX_WIRE_ONLY
/* Functions are declared in Task 2 (writer), Task 3 (validator) and Task 4
 * (parse). */
#endif /* RAY_ENC_IDX_WIRE_ONLY */

#endif /* RAY_ENCIDX_H */
