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
_Static_assert(RAY_ENC_IDX_FIRST_CHILD == ((RAY_ENC_IDX_ROOT_BYTES + 31) & ~31),
               "children start at pad32 of the root");
ENCIDX_OFF(ray_enc_idx_root_t, parent_type, 0);
ENCIDX_OFF(ray_enc_idx_root_t, rsv, 1);
ENCIDX_OFF(ray_enc_idx_root_t, built_for_len, 8);
ENCIDX_OFF(ray_enc_idx_root_t, kind_area, 16);
_Static_assert(sizeof(((ray_enc_idx_root_t*)0)->kind_area) == 64, "kind area is 64 bytes");

_Static_assert(sizeof(ray_enc_idx_hash_t) == 64, "HASH fills the kind area");
ENCIDX_OFF(ray_enc_idx_hash_t, table, 0);
ENCIDX_OFF(ray_enc_idx_hash_t, gkeys, 8);
ENCIDX_OFF(ray_enc_idx_hash_t, offs, 16);
ENCIDX_OFF(ray_enc_idx_hash_t, rows, 24);
ENCIDX_OFF(ray_enc_idx_hash_t, mask, 32);
ENCIDX_OFF(ray_enc_idx_hash_t, n_keys, 40);
ENCIDX_OFF(ray_enc_idx_hash_t, n_groups, 48);
ENCIDX_OFF(ray_enc_idx_hash_t, order_name, 56);

_Static_assert(sizeof(ray_enc_idx_sort_t) == 8, "SORT kind fields");
ENCIDX_OFF(ray_enc_idx_sort_t, perm, 0);

_Static_assert(sizeof(ray_enc_idx_bloom_t) == 32, "BLOOM kind fields");
ENCIDX_OFF(ray_enc_idx_bloom_t, bits, 0);
ENCIDX_OFF(ray_enc_idx_bloom_t, m_mask, 8);
ENCIDX_OFF(ray_enc_idx_bloom_t, k, 16);
ENCIDX_OFF(ray_enc_idx_bloom_t, rsv, 20);
ENCIDX_OFF(ray_enc_idx_bloom_t, n_keys, 24);

_Static_assert(sizeof(ray_enc_idx_part_t) == 40, "PART kind fields");
ENCIDX_OFF(ray_enc_idx_part_t, keys, 0);
ENCIDX_OFF(ray_enc_idx_part_t, starts, 8);
ENCIDX_OFF(ray_enc_idx_part_t, lens, 16);
ENCIDX_OFF(ray_enc_idx_part_t, n_parts, 24);
ENCIDX_OFF(ray_enc_idx_part_t, order_name, 32);

#define ENCIDX_FITS(T) _Static_assert(sizeof(T) <= sizeof(((ray_enc_idx_root_t*)0)->kind_area), \
                                      #T " fits in the kind area")
ENCIDX_FITS(ray_enc_idx_hash_t);
ENCIDX_FITS(ray_enc_idx_sort_t);
ENCIDX_FITS(ray_enc_idx_bloom_t);
ENCIDX_FITS(ray_enc_idx_part_t);

/* The index kinds that carry a region: HASH, SORT, BLOOM, PART. */
static inline int ray_enc_idx_kind_has_region(uint8_t k) {
    return k == 1 || k == 2 || k == 4 || k == 6;
}

#ifndef RAY_ENC_IDX_WIRE_ONLY
#include <stdio.h>
#include <rayforce.h>

typedef struct {
    const ray_t* root;          /* RAY_INDEX block: HASH, SORT, BLOOM or PART */
    int8_t       parent_type;   /* the column's logical type */
    uint8_t      attrs;         /* the column's persisted attrs (width of SYM PART keys) */
    int64_t      len;           /* the column's rows */
    const char*  order_name;    /* NULL: order_name -1 */
    uint32_t     order_len;
} ray_enc_idx_src_t;

/* Stream the normalized region to f at its current position; *bytes is
 * the region size (a multiple of 32).  NULL, or an owned error: nyi for a
 * kind / type the admission matrix refuses or a child of the wrong type
 * or length (naming the kind), io on a short write. */
ray_t*  ray_enc_index_write(FILE* f, const ray_enc_idx_src_t* s, uint64_t* bytes);

/* A validated region: the root's fields by kind, the name into the region. */
typedef struct {
    uint8_t        kind;
    int8_t         parent_type;
    uint8_t        attrs;
    int64_t        len;
    const uint8_t* base;          /* the region */
    uint64_t       size;
    ray_enc_idx_hash_t  hash;     /* the kind's fields, by kind */
    ray_enc_idx_sort_t  sort;
    ray_enc_idx_bloom_t bloom;
    ray_enc_idx_part_t  part;
    const char*    order_name;    /* into the region, or NULL */
    uint32_t       order_len;
} ray_enc_idx_view_t;

/* Validate `size` bytes at `region` as an index of `kind` for a column of
 * (parent_type, attrs, len); SYM ids must be below sym_bound.  col_data is
 * the decoded column (PART float zero-sign rule) or NULL to skip that one
 * rule (writers re-checking a source).  NULL with *out filled, or an owned
 * corrupt error naming `path`. */
ray_t* ray_enc_index_validate(const uint8_t* region, uint64_t size, uint8_t kind,
                              int8_t parent_type, uint8_t attrs, int64_t len,
                              uint32_t sym_bound, const void* col_data,
                              const char* path, ray_enc_idx_view_t* out);

/* Children copied out of the region by the validation itself (a large
 * HASH: table, gkeys, offs, rows), for ray_enc_index_parse_copy; owned
 * heap vectors or NULL.  Every slot is NULL after a validation error. */
typedef struct { ray_t* hash[4]; } ray_enc_idx_copies_t;

/* ray_enc_index_validate, filling *copies when the checks copied the
 * children as they read them (the same verdict and errors either way). */
ray_t* ray_enc_index_validate_copy(const uint8_t* region, uint64_t size, uint8_t kind,
                                   int8_t parent_type, uint8_t attrs, int64_t len,
                                   uint32_t sym_bound, const void* col_data,
                                   const char* path, ray_enc_idx_view_t* out,
                                   ray_enc_idx_copies_t* copies);

/* A fresh heap RAY_INDEX root from a validated view, every child copied
 * into a heap vector of its slot's type.  SYM PART keys adopt the domain
 * of `vec` (the decoded column); order_name is interned in this process.
 * NULL with *root_out set (owned), or an owned oom error.  Nothing in the
 * root refers to the region, so it outlives the mapping.  A view whose
 * fields do not lie inside its region is corrupt, and a SYM PART without
 * a SYM `vec` is a type error: the view is re-tied to the region before
 * any count sizes an allocation. */
ray_t* ray_enc_index_parse(const ray_enc_idx_view_t* v, ray_t* vec, ray_t** root_out);
/* ray_enc_index_parse taking the children *copies holds for the view's
 * kind and counts in place of copying them; consumes *copies (every slot
 * is NULL on return, on any path). */
ray_t* ray_enc_index_parse_copy(const ray_enc_idx_view_t* v, ray_t* vec, ray_t** root_out,
                                ray_enc_idx_copies_t* copies);

/* order_sym a writer may convert to a name (spec "Writing the index"):
 * the root's HASH / PART order_sym when RAY_MARK_MMAP is clear, else -1. */
int64_t ray_enc_index_trusted_order(const ray_t* root);
#endif /* RAY_ENC_IDX_WIRE_ONLY */

#endif /* RAY_ENCIDX_H */
