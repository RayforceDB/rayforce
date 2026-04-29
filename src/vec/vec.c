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

#include "vec.h"
#include "core/platform.h"
#include "mem/heap.h"
#include "table/sym.h"
#include "vec/embedding.h"
#include "vec/str.h"
#include "ops/idxop.h"
#include <string.h>

/* Public bitmap accessor — handles slice / ext / inline / HAS_INDEX
 * uniformly.  See vec.h for the contract. */
const uint8_t* ray_vec_nullmap_bytes(const ray_t* v,
                                     int64_t* bit_offset_out,
                                     int64_t* len_bits_out) {
    if (bit_offset_out) *bit_offset_out = 0;
    if (len_bits_out)   *len_bits_out   = 0;
    if (!v) return NULL;

    /* Slice: HAS_NULLS / HAS_INDEX live on the parent — redirect first,
     * THEN test for nulls.  Reading v->attrs & HAS_NULLS here would
     * incorrectly drop a sliced view of a nullable column. */
    const ray_t* target = v;
    int64_t off = 0;
    if (v->attrs & RAY_ATTR_SLICE) {
        target = v->slice_parent;
        off = v->slice_offset;
        if (!target) return NULL;
    }
    if (!(target->attrs & RAY_ATTR_HAS_NULLS)) return NULL;

    if (bit_offset_out) *bit_offset_out = off;

    if (target->attrs & RAY_ATTR_HAS_INDEX) {
        const ray_index_t* ix = ray_index_payload(target->index);
        if (ix->saved_attrs & RAY_ATTR_NULLMAP_EXT) {
            ray_t* ext;
            memcpy(&ext, &ix->saved_nullmap[0], sizeof(ext));
            if (len_bits_out) *len_bits_out = ext->len * 8;
            return (const uint8_t*)ray_data(ext);
        }
        if (len_bits_out) *len_bits_out = 128;
        return ix->saved_nullmap;
    }
    if (target->attrs & RAY_ATTR_NULLMAP_EXT) {
        if (len_bits_out) *len_bits_out = target->ext_nullmap->len * 8;
        return (const uint8_t*)ray_data(target->ext_nullmap);
    }
    /* Inline path: RAY_STR's bytes 0-15 hold str_pool/str_ext_null, not
     * bits — so RAY_STR with HAS_NULLS must always have NULLMAP_EXT. */
    if (target->type == RAY_STR) return NULL;
    if (len_bits_out) *len_bits_out = 128;
    return target->nullmap;
}

/* Internal compatibility wrapper for the older two-out-param form used
 * inside vec.c.  Returns the inline pointer (16-byte buffer) when nulls
 * live inline, or NULL when they live in *ext_out. */
static inline const uint8_t* vec_inline_nullmap(const ray_t* v, ray_t** ext_nullmap_ref) {
    *ext_nullmap_ref = NULL;
    if (v->attrs & RAY_ATTR_HAS_INDEX) {
        const ray_index_t* ix = ray_index_payload(v->index);
        if (ix->saved_attrs & RAY_ATTR_NULLMAP_EXT) {
            ray_t* ext;
            memcpy(&ext, &ix->saved_nullmap[0], sizeof(ext));
            *ext_nullmap_ref = ext;
            return NULL;
        }
        return ix->saved_nullmap;
    }
    if (v->attrs & RAY_ATTR_NULLMAP_EXT) {
        *ext_nullmap_ref = v->ext_nullmap;
        return NULL;
    }
    return v->nullmap;
}

/* True if v has any nulls.  HAS_NULLS is preserved on the parent across
 * index attach/detach (see attach_finalize), so this is the same one-bit
 * test in both indexed and non-indexed cases. */
static inline bool vec_any_nulls(const ray_t* v) {
    return (v->attrs & RAY_ATTR_HAS_NULLS) != 0;
}

/* In-place drop of attached index — caller must hold a unique ref (rc==1)
 * on `v` itself.  Used by mutation paths to invalidate the (now stale)
 * index before writing.  HAS_NULLS was preserved through the attachment
 * so it needs no restoration; only NULLMAP_EXT (cleared at attach time)
 * is reinstated from saved_attrs.
 *
 * Shared-index case: `v` may share its index ray_t with another vec
 * (e.g. after ray_cow followed by ray_retain_owned_refs, both copies
 * point at the same RAY_INDEX with rc==2).  We must NOT clobber the
 * saved-nullmap bytes inside a shared index — the other holder still
 * reads them.  Detect rc>1 and copy the saved pointers via
 * ray_index_retain_saved instead of moving them out. */
static inline void vec_drop_index_inplace(ray_t* v) {
    if (!(v->attrs & RAY_ATTR_HAS_INDEX)) return;
    ray_t* idx = v->index;
    ray_index_t* ix = ray_index_payload(idx);
    uint8_t saved = ix->saved_attrs;
    bool shared = ray_atomic_load(&idx->rc) > 1;

    if (shared) {
        /* Take our own retained references to the saved-pointer slots
         * (ext_nullmap / str_pool / sym_dict etc.) so the bytes we copy
         * into v->nullmap are validly owned by v.  Leave the index's
         * snapshot intact for the other holder. */
        ray_index_retain_saved(ix);
    }
    memcpy(v->nullmap, ix->saved_nullmap, 16);
    if (!shared) {
        /* Sole owner: about to release idx, so neutralize its snapshot
         * to prevent ray_index_release_saved from double-releasing the
         * pointers we just transferred to v. */
        memset(ix->saved_nullmap, 0, 16);
        ix->saved_attrs = 0;
    }
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_INDEX;
    if (saved & RAY_ATTR_NULLMAP_EXT) v->attrs |= RAY_ATTR_NULLMAP_EXT;
    ray_release(idx);
}

/* --------------------------------------------------------------------------
 * Capacity helpers
 *
 * A vector's capacity is determined by its buddy order:
 *   capacity = (2^order - 32) / elem_size
 * When len reaches capacity, realloc to next power-of-2 data size.
 * -------------------------------------------------------------------------- */

static int64_t vec_capacity(ray_t* vec) {
    size_t block_size = (size_t)1 << vec->order;
    size_t data_space = block_size - 32;  /* 32B ray_t header */
    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    if (esz == 0) return 0;
    return (int64_t)(data_space / esz);
}

/* --------------------------------------------------------------------------
 * ray_vec_new
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_new(int8_t type, int64_t capacity) {
    if (type <= 0 || type >= RAY_TYPE_COUNT)
        return ray_error("type", NULL);
    if (type == RAY_SYM)
        return ray_sym_vec_new(RAY_SYM_W64, capacity);  /* default: global sym IDs */
    if (capacity < 0) return ray_error("range", NULL);

    uint8_t esz = ray_elem_size(type);
    size_t data_size = (size_t)capacity * esz;
    if (esz > 1 && data_size / esz != (size_t)capacity)
        return ray_error("oom", NULL);

    ray_t* v = ray_alloc(data_size);
    if (!v) return ray_error("oom", "vec_new(type=%d, cap=%lld): %zu bytes",
                             (int)type, (long long)capacity, data_size);
    if (RAY_IS_ERR(v)) return v;

    v->type = type;
    v->len = 0;
    v->attrs = 0;
    memset(v->nullmap, 0, 16);
    if (type == RAY_STR) v->str_pool = NULL;

    return v;
}

/* --------------------------------------------------------------------------
 * ray_sym_vec_new — create a RAY_SYM vector with adaptive index width
 *
 * sym_width: RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, or RAY_SYM_W64
 * capacity:  number of elements (rows)
 * -------------------------------------------------------------------------- */

ray_t* ray_sym_vec_new(uint8_t sym_width, int64_t capacity) {
    if ((sym_width & ~RAY_SYM_W_MASK) != 0)
        return ray_error("type", NULL);
    if (capacity < 0) return ray_error("range", NULL);

    uint8_t esz = (uint8_t)RAY_SYM_ELEM(sym_width);
    size_t data_size = (size_t)capacity * esz;
    if (esz > 1 && data_size / esz != (size_t)capacity)
        return ray_error("oom", NULL);

    ray_t* v = ray_alloc(data_size);
    if (!v) return ray_error("oom", "sym_vec_new(width=%u, cap=%lld): %zu bytes",
                             (unsigned)sym_width, (long long)capacity, data_size);
    if (RAY_IS_ERR(v)) return v;

    v->type = RAY_SYM;
    v->len = 0;
    v->attrs = sym_width;  /* lower 2 bits encode width */
    memset(v->nullmap, 0, 16);

    return v;
}

/* --------------------------------------------------------------------------
 * ray_vec_append
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_append(ray_t* vec, const void* elem) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type <= 0 || vec->type >= RAY_TYPE_COUNT)
        return ray_error("type", NULL);
    if (vec->type == RAY_STR) return ray_error("type", NULL);

    /* COW: if shared, copy first */
    ray_t* original = vec;
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;

    /* Append changes len + writes data; any attached index is now stale. */
    vec_drop_index_inplace(vec);

    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    int64_t cap = vec_capacity(vec);

    /* Grow if needed */
    if (vec->len >= cap) {
        size_t new_data_size = (size_t)(vec->len + 1) * esz;
        /* Round up to next power of 2 block */
        if (new_data_size < 32) new_data_size = 32;
        else {
            size_t s = 32;
            while (s < new_data_size) {
                if (s > SIZE_MAX / 2) goto fail;
                s *= 2;
            }
            new_data_size = s;
        }
        ray_t* new_vec = ray_scratch_realloc(vec, new_data_size);
        if (!new_vec || RAY_IS_ERR(new_vec)) {
            if (vec != original) ray_release(vec);
            return new_vec ? new_vec : ray_error("oom", NULL);
        }
        vec = new_vec;
    }

    /* Append element */
    char* dst = (char*)ray_data(vec) + vec->len * esz;
    memcpy(dst, elem, esz);
    vec->len++;

    return vec;

fail:
    if (vec != original) ray_release(vec);
    return ray_error("oom", NULL);
}

/* --------------------------------------------------------------------------
 * ray_vec_set
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_set(ray_t* vec, int64_t idx, const void* elem) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type == RAY_STR) return ray_error("type", NULL);
    if (idx < 0 || idx >= vec->len)
        return ray_error("range", NULL);

    /* COW: if shared, copy first */
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;

    /* Writing a slot value invalidates any attached accelerator index. */
    vec_drop_index_inplace(vec);

    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    char* dst = (char*)ray_data(vec) + idx * esz;
    memcpy(dst, elem, esz);

    return vec;
}

/* --------------------------------------------------------------------------
 * ray_vec_get
 * -------------------------------------------------------------------------- */

void* ray_vec_get(ray_t* vec, int64_t idx) {
    if (!vec || RAY_IS_ERR(vec)) return NULL;
    if (vec->type == RAY_STR) return NULL;

    /* Slice path: redirect to parent */
    if (vec->attrs & RAY_ATTR_SLICE) {
        ray_t* parent = vec->slice_parent;
        int64_t offset = vec->slice_offset;
        if (idx < 0 || idx >= vec->len) return NULL;
        uint8_t esz = ray_sym_elem_size(parent->type, parent->attrs);
        return (char*)ray_data(parent) + (offset + idx) * esz;
    }

    if (idx < 0 || idx >= vec->len) return NULL;
    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    return (char*)ray_data(vec) + idx * esz;
}

/* --------------------------------------------------------------------------
 * ray_vec_slice  (zero-copy view)
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_slice(ray_t* vec, int64_t offset, int64_t len) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (offset < 0 || len < 0 || offset > vec->len || len > vec->len - offset)
        return ray_error("range", NULL);

    /* If input is already a slice, resolve to ultimate parent */
    ray_t* parent = vec;
    int64_t parent_offset = offset;
    if (vec->attrs & RAY_ATTR_SLICE) {
        parent = vec->slice_parent;
        parent_offset = vec->slice_offset + offset;
    }

    /* Allocate a header-only block for the slice view */
    ray_t* s = ray_alloc(0);
    if (!s || RAY_IS_ERR(s)) return s;

    s->type = parent->type;
    s->attrs = RAY_ATTR_SLICE | (parent->attrs & RAY_SYM_W_MASK);
    s->len = len;
    s->slice_parent = parent;
    s->slice_offset = parent_offset;

    /* Retain the parent so it stays alive */
    ray_retain(parent);

    return s;
}

/* --------------------------------------------------------------------------
 * ray_vec_concat
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_concat(ray_t* a, ray_t* b) {
    if (!a || RAY_IS_ERR(a)) return a;
    if (!b || RAY_IS_ERR(b)) return b;
    if (a->type != b->type)
        return ray_error("type", NULL);

    if (a->type == RAY_STR) {
        int64_t total_len = a->len + b->len;
        if (total_len < a->len) return ray_error("oom", NULL);

        ray_t* result = ray_vec_new(RAY_STR, total_len);
        if (!result || RAY_IS_ERR(result)) return result;
        result->len = total_len;

        ray_str_t* dst = (ray_str_t*)ray_data(result);

        /* Resolve a's data (may be a slice) */
        const ray_str_t* a_elems = (a->attrs & RAY_ATTR_SLICE)
            ? &((const ray_str_t*)ray_data(a->slice_parent))[a->slice_offset]
            : (const ray_str_t*)ray_data(a);
        ray_t* a_pool_owner = (a->attrs & RAY_ATTR_SLICE) ? a->slice_parent : a;

        /* Resolve b's data (may be a slice) */
        const ray_str_t* b_elems = (b->attrs & RAY_ATTR_SLICE)
            ? &((const ray_str_t*)ray_data(b->slice_parent))[b->slice_offset]
            : (const ray_str_t*)ray_data(b);
        ray_t* b_pool_owner = (b->attrs & RAY_ATTR_SLICE) ? b->slice_parent : b;

        /* Copy a's elements as-is */
        memcpy(dst, a_elems, (size_t)a->len * sizeof(ray_str_t));

        /* Merge pools: a's pool + b's pool */
        int64_t a_pool_size = (a_pool_owner->str_pool) ? a_pool_owner->str_pool->len : 0;
        int64_t b_pool_size = (b_pool_owner->str_pool) ? b_pool_owner->str_pool->len : 0;
        int64_t total_pool = a_pool_size + b_pool_size;

        /* Guard: total pool must fit in uint32_t for pool_off rebasing */
        if (total_pool > (int64_t)UINT32_MAX) {
            ray_release(result);
            return ray_error("range", NULL);
        }

        if (total_pool > 0) {
            result->str_pool = ray_alloc((size_t)total_pool);
            if (!result->str_pool || RAY_IS_ERR(result->str_pool)) {
                result->str_pool = NULL;
                ray_release(result);
                return ray_error("oom", NULL);
            }
            result->str_pool->type = RAY_U8;
            result->str_pool->len = total_pool;
            char* pool_dst = (char*)ray_data(result->str_pool);
            if (a_pool_size > 0)
                memcpy(pool_dst, ray_data(a_pool_owner->str_pool), (size_t)a_pool_size);
            if (b_pool_size > 0)
                memcpy(pool_dst + a_pool_size, ray_data(b_pool_owner->str_pool), (size_t)b_pool_size);
        }

        /* Copy b's elements, rebasing pool offsets */
        for (int64_t i = 0; i < b->len; i++) {
            dst[a->len + i] = b_elems[i];
            if (!ray_str_is_inline(&b_elems[i]) && b_elems[i].len > 0) {
                dst[a->len + i].pool_off += (uint32_t)a_pool_size;
            }
        }

        /* Propagate null bitmaps from a and b.
         * Slices don't carry RAY_ATTR_HAS_NULLS — check RAY_ATTR_SLICE too. */
        if ((a->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE)) ||
            (b->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE))) {
            for (int64_t i = 0; i < a->len; i++) {
                if (ray_vec_is_null((ray_t*)a, i)) {
                    ray_err_t err = ray_vec_set_null_checked(result, i, true);
                    if (err != RAY_OK) { ray_release(result); return ray_error(ray_err_code_str(err), NULL); }
                }
            }
            for (int64_t i = 0; i < b->len; i++) {
                if (ray_vec_is_null((ray_t*)b, i)) {
                    ray_err_t err = ray_vec_set_null_checked(result, a->len + i, true);
                    if (err != RAY_OK) { ray_release(result); return ray_error(ray_err_code_str(err), NULL); }
                }
            }
        }

        return result;
    }

    uint8_t a_esz = ray_sym_elem_size(a->type, a->attrs);
    uint8_t b_esz = ray_sym_elem_size(b->type, b->attrs);
    /* Use the wider of the two widths for SYM columns — carry only width bits,
     * not flags like RAY_ATTR_SLICE or RAY_ATTR_HAS_NULLS from inputs. */
    uint8_t out_attrs = (a_esz >= b_esz) ? (a->attrs & RAY_SYM_W_MASK) : (b->attrs & RAY_SYM_W_MASK);
    uint8_t esz = (a_esz >= b_esz) ? a_esz : b_esz;

    int64_t total_len = a->len + b->len;
    if (total_len < a->len) return ray_error("oom", NULL); /* overflow */
    size_t data_size = (size_t)total_len * esz;
    if (esz > 1 && data_size / esz != (size_t)total_len)
        return ray_error("oom", NULL); /* multiplication overflow */

    ray_t* result = ray_alloc(data_size);
    if (!result || RAY_IS_ERR(result)) return result;

    result->type = a->type;
    result->len = total_len;
    result->attrs = out_attrs;
    memset(result->nullmap, 0, 16);

    /* For SYM with mismatched widths, widen element-by-element */
    if (a->type == RAY_SYM && a_esz != b_esz) {
        void* dst = ray_data(result);
        for (int64_t i = 0; i < a->len; i++) {
            int64_t val = ray_read_sym(ray_data(a), i, a->type, a->attrs);
            ray_write_sym(dst, i, (uint64_t)val, result->type, result->attrs);
        }
        for (int64_t i = 0; i < b->len; i++) {
            int64_t val = ray_read_sym(ray_data(b), i, b->type, b->attrs);
            ray_write_sym(dst, a->len + i, (uint64_t)val, result->type, result->attrs);
        }
    } else {
        /* Same width: fast memcpy path */
        void* a_data = (a->attrs & RAY_ATTR_SLICE) ?
            ((char*)ray_data(a->slice_parent) + a->slice_offset * esz) :
            ray_data(a);
        memcpy(ray_data(result), a_data, (size_t)a->len * esz);

        void* b_data = (b->attrs & RAY_ATTR_SLICE) ?
            ((char*)ray_data(b->slice_parent) + b->slice_offset * esz) :
            ray_data(b);
        memcpy((char*)ray_data(result) + (size_t)a->len * esz, b_data,
               (size_t)b->len * esz);
    }

    /* Propagate null bitmaps from a and b.
     * Slices don't carry RAY_ATTR_HAS_NULLS — check RAY_ATTR_SLICE too. */
    if ((a->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE)) ||
        (b->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE))) {
        for (int64_t i = 0; i < a->len; i++) {
            if (ray_vec_is_null((ray_t*)a, i)) {
                ray_err_t err = ray_vec_set_null_checked(result, i, true);
                if (err != RAY_OK) { ray_release(result); return ray_error(ray_err_code_str(err), NULL); }
            }
        }
        for (int64_t i = 0; i < b->len; i++) {
            if (ray_vec_is_null((ray_t*)b, i)) {
                ray_err_t err = ray_vec_set_null_checked(result, a->len + i, true);
                if (err != RAY_OK) { ray_release(result); return ray_error(ray_err_code_str(err), NULL); }
            }
        }
    }

    /* LIST/TABLE columns hold child pointers — retain them */
    if (a->type == RAY_LIST || a->type == RAY_TABLE) {
        ray_t** ptrs = (ray_t**)ray_data(result);
        for (int64_t i = 0; i < total_len; i++) {
            if (ptrs[i]) ray_retain(ptrs[i]);
        }
    }

    return result;
}

/* --------------------------------------------------------------------------
 * ray_vec_insert_at — insert a single element at position idx.
 *
 * idx is a pre-insertion position in [0, vec->len]. idx == vec->len is
 * equivalent to append. Does not support RAY_STR (use ray_str_vec_insert_at).
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_insert_at(ray_t* vec, int64_t idx, const void* elem) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type <= 0 || vec->type >= RAY_TYPE_COUNT)
        return ray_error("type", NULL);
    if (vec->type == RAY_STR) return ray_error("type", NULL);
    if (idx < 0 || idx > vec->len) return ray_error("range", NULL);

    /* COW: if shared, copy first */
    ray_t* original = vec;
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;

    /* In-place insert mutates len + data + nullmap; any attached
     * accelerator index is now stale. */
    vec_drop_index_inplace(vec);

    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    int64_t cap = vec_capacity(vec);

    /* Grow if needed */
    if (vec->len >= cap) {
        size_t new_data_size = (size_t)(vec->len + 1) * esz;
        if (new_data_size < 32) new_data_size = 32;
        else {
            size_t s = 32;
            while (s < new_data_size) {
                if (s > SIZE_MAX / 2) goto fail_oom;
                s *= 2;
            }
            new_data_size = s;
        }
        ray_t* new_vec = ray_scratch_realloc(vec, new_data_size);
        if (!new_vec || RAY_IS_ERR(new_vec)) {
            if (vec != original) ray_release(vec);
            return new_vec ? new_vec : ray_error("oom", NULL);
        }
        vec = new_vec;
    }

    int64_t old_len = vec->len;
    char* base = (char*)ray_data(vec);

    /* Shift elements [idx..old_len) → [idx+1..old_len+1) */
    if (idx < old_len) {
        memmove(base + (size_t)(idx + 1) * esz,
                base + (size_t)idx * esz,
                (size_t)(old_len - idx) * esz);
    }

    /* Write the new element */
    memcpy(base + (size_t)idx * esz, elem, esz);

    vec->len = old_len + 1;

    /* Shift null bitmap bits [idx..old_len) up by one; clear bit at idx.
     * Walk from tail backward so we don't overwrite unread bits. */
    if (vec->attrs & RAY_ATTR_HAS_NULLS) {
        for (int64_t i = old_len - 1; i >= idx; i--) {
            bool was_null = ray_vec_is_null(vec, i);
            if (was_null) {
                ray_err_t err = ray_vec_set_null_checked(vec, i + 1, true);
                if (err != RAY_OK) goto fail_oom;
            } else {
                ray_err_t err = ray_vec_set_null_checked(vec, i + 1, false);
                if (err != RAY_OK) goto fail_oom;
            }
        }
        /* New element is not null */
        ray_err_t err = ray_vec_set_null_checked(vec, idx, false);
        if (err != RAY_OK) goto fail_oom;
    }

    return vec;

fail_oom:
    if (vec != original) ray_release(vec);
    return ray_error("oom", NULL);
}

/* --------------------------------------------------------------------------
 * ray_vec_insert_vec_at — splice src into vec at position idx.
 *
 * Shares SYM-width widening, RAY_STR pool merge, and null-bit propagation
 * with ray_vec_concat via the slice→concat→concat pattern. Always returns
 * a fresh block; caller should release the input if no longer needed.
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_insert_vec_at(ray_t* vec, int64_t idx, ray_t* src) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (!src || RAY_IS_ERR(src)) return src;
    if (vec->type != src->type) return ray_error("type", NULL);
    if (idx < 0 || idx > vec->len) return ray_error("range", NULL);

    /* Fast path: idx == len is plain concat */
    if (idx == vec->len) return ray_vec_concat(vec, src);
    /* Fast path: idx == 0 is reversed concat */
    if (idx == 0) return ray_vec_concat(src, vec);

    ray_t* head = ray_vec_slice(vec, 0, idx);
    if (!head || RAY_IS_ERR(head)) return head;

    ray_t* tail = ray_vec_slice(vec, idx, vec->len - idx);
    if (!tail || RAY_IS_ERR(tail)) { ray_release(head); return tail; }

    ray_t* mid = ray_vec_concat(head, src);
    ray_release(head);
    if (!mid || RAY_IS_ERR(mid)) { ray_release(tail); return mid; }

    ray_t* result = ray_vec_concat(mid, tail);
    ray_release(mid);
    ray_release(tail);
    return result;
}

/* --------------------------------------------------------------------------
 * ray_vec_insert_many — insert N values at N pre-insertion positions.
 *
 * idxs: I64 vec of length N, each idx in [0, vec->len].
 * vals: either a matching atom (broadcast) or same-type vec of length N
 *       (parallel) or length 1 (broadcast).
 *
 * For ties in idxs, the original input order is preserved (stable sort).
 * Returns a fresh block; caller releases vec if no longer needed.
 * RAY_STR targets are rejected — use ray_vec_insert_vec_at in a loop instead.
 * For RAY_SYM, the source width must match the destination width.
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_insert_many(ray_t* vec, ray_t* idxs, ray_t* vals) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (!idxs || RAY_IS_ERR(idxs)) return idxs;
    if (!vals || RAY_IS_ERR(vals)) return vals;
    if (vec->type <= 0 || vec->type >= RAY_TYPE_COUNT) return ray_error("type", NULL);
    if (vec->type == RAY_STR) return ray_error("type", NULL);
    if (idxs->type != RAY_I64) return ray_error("type", NULL);

    int64_t N = idxs->len;
    int64_t old_len = vec->len;
    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);

    /* Fast path: N == 0 returns a fresh retain */
    if (N == 0) { ray_retain(vec); return vec; }

    /* Validate indices */
    const int64_t* idx_arr = (const int64_t*)ray_data(idxs);
    for (int64_t k = 0; k < N; k++) {
        if (idx_arr[k] < 0 || idx_arr[k] > old_len)
            return ray_error("range", NULL);
    }

    /* Classify vals: atom (broadcast) vs vec (parallel or singleton broadcast) */
    int broadcast;
    if (vals->type < 0) {
        if (vals->type != -vec->type) return ray_error("type", NULL);
        broadcast = 1;
    } else if (vals->type == vec->type) {
        /* SYM width must match — dispatcher should widen upstream */
        if (vec->type == RAY_SYM &&
            (vals->attrs & RAY_SYM_W_MASK) != (vec->attrs & RAY_SYM_W_MASK))
            return ray_error("type", NULL);
        if (vals->len == 1) broadcast = 1;
        else if (vals->len == N) broadcast = 0;
        else return ray_error("range", NULL);
    } else {
        return ray_error("type", NULL);
    }

    /* Build sort buffer as I64 vec of 2*N slots: [idx0, src0, idx1, src1, ...] */
    ray_t* pair_vec = ray_vec_new(RAY_I64, 2 * N);
    if (!pair_vec || RAY_IS_ERR(pair_vec)) return ray_error("oom", NULL);
    pair_vec->len = 2 * N;
    int64_t* pairs = (int64_t*)ray_data(pair_vec);
    for (int64_t k = 0; k < N; k++) {
        pairs[2 * k]     = idx_arr[k];
        pairs[2 * k + 1] = k;
    }

    /* Stable insertion sort by idx */
    for (int64_t i = 1; i < N; i++) {
        int64_t ki = pairs[2 * i];
        int64_t ks = pairs[2 * i + 1];
        int64_t j = i - 1;
        while (j >= 0 && pairs[2 * j] > ki) {
            pairs[2 * (j + 1)]     = pairs[2 * j];
            pairs[2 * (j + 1) + 1] = pairs[2 * j + 1];
            j--;
        }
        pairs[2 * (j + 1)]     = ki;
        pairs[2 * (j + 1) + 1] = ks;
    }

    /* Allocate result */
    int64_t new_len = old_len + N;
    if (new_len < old_len) { ray_release(pair_vec); return ray_error("oom", NULL); }
    size_t data_size = (size_t)new_len * esz;
    if (esz > 1 && data_size / esz != (size_t)new_len) {
        ray_release(pair_vec);
        return ray_error("oom", NULL);
    }

    ray_t* result = ray_alloc(data_size);
    if (!result || RAY_IS_ERR(result)) { ray_release(pair_vec); return result ? result : ray_error("oom", NULL); }
    result->type = vec->type;
    result->len = new_len;
    result->attrs = vec->attrs & RAY_SYM_W_MASK;
    memset(result->nullmap, 0, 16);

    /* Source pointers */
    const char* src_base = (vec->attrs & RAY_ATTR_SLICE)
        ? ((const char*)ray_data(vec->slice_parent) + (size_t)vec->slice_offset * esz)
        : (const char*)ray_data(vec);

    /* Value source: atom bytes or vec row bytes.
     * GUID atoms keep their 16-byte payload in vals->obj, not inline; typed
     * nulls carry obj==NULL and fall through to a zero buffer (null bit is
     * then set below via RAY_ATOM_IS_NULL). */
    static const uint8_t zero_guid[16] = {0};
    const char* val_atom_bytes = NULL;
    if (vals->type < 0) {
        if (vec->type == RAY_GUID) {
            val_atom_bytes = vals->obj
                ? (const char*)ray_data(vals->obj)
                : (const char*)zero_guid;
        } else {
            val_atom_bytes = (const char*)&vals->u8;
        }
    }
    const char* val_vec_base = NULL;
    if (val_atom_bytes == NULL) {
        val_vec_base = (vals->attrs & RAY_ATTR_SLICE)
            ? ((const char*)ray_data(vals->slice_parent) + (size_t)vals->slice_offset * esz)
            : (const char*)ray_data(vals);
    }

    char* dst_base = (char*)ray_data(result);

    /* Walk: merge sorted inserts with original */
    int64_t w = 0;   /* write cursor */
    int64_t p = 0;   /* pair cursor */
    for (int64_t r = 0; r <= old_len; r++) {
        while (p < N && pairs[2 * p] == r) {
            int64_t src_pos = pairs[2 * p + 1];
            if (val_atom_bytes) {
                /* Broadcast atom */
                memcpy(dst_base + (size_t)w * esz, val_atom_bytes, esz);
                /* Atom-level null propagation */
                if (RAY_ATOM_IS_NULL(vals)) {
                    ray_err_t e = ray_vec_set_null_checked(result, w, true);
                    if (e != RAY_OK) { ray_release(result); ray_release(pair_vec); return ray_error("oom", NULL); }
                }
            } else if (broadcast) {
                /* Single-element vec broadcast — always row 0 */
                memcpy(dst_base + (size_t)w * esz, val_vec_base, esz);
                if (ray_vec_is_null(vals, 0)) {
                    ray_err_t e = ray_vec_set_null_checked(result, w, true);
                    if (e != RAY_OK) { ray_release(result); ray_release(pair_vec); return ray_error("oom", NULL); }
                }
            } else {
                /* Parallel: use src_pos into vals */
                memcpy(dst_base + (size_t)w * esz,
                       val_vec_base + (size_t)src_pos * esz, esz);
                if (ray_vec_is_null(vals, src_pos)) {
                    ray_err_t e = ray_vec_set_null_checked(result, w, true);
                    if (e != RAY_OK) { ray_release(result); ray_release(pair_vec); return ray_error("oom", NULL); }
                }
            }
            w++;
            p++;
        }
        if (r < old_len) {
            memcpy(dst_base + (size_t)w * esz, src_base + (size_t)r * esz, esz);
            if (ray_vec_is_null(vec, r)) {
                ray_err_t e = ray_vec_set_null_checked(result, w, true);
                if (e != RAY_OK) { ray_release(result); ray_release(pair_vec); return ray_error("oom", NULL); }
            }
            w++;
        }
    }

    ray_release(pair_vec);
    return result;
}

/* --------------------------------------------------------------------------
 * ray_vec_from_raw
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_from_raw(int8_t type, const void* data, int64_t count) {
    if (type <= 0 || type >= RAY_TYPE_COUNT)
        return ray_error("type", NULL);
    if (type == RAY_STR) return ray_error("type", NULL);
    if (count < 0) return ray_error("range", NULL);

    /* RAY_SYM defaults to W64 (global sym IDs) */
    uint8_t sym_w = (type == RAY_SYM) ? RAY_SYM_W64 : 0;
    uint8_t esz = ray_sym_elem_size(type, sym_w);
    size_t data_size = (size_t)count * esz;

    ray_t* v = ray_alloc(data_size);
    if (!v || RAY_IS_ERR(v)) return v;

    v->type = type;
    v->len = count;
    v->attrs = sym_w;
    memset(v->nullmap, 0, 16);

    memcpy(ray_data(v), data, data_size);

    /* LIST/TABLE elements are child pointers — retain them */
    if (type == RAY_LIST || type == RAY_TABLE) {
        ray_t** ptrs = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < count; i++) {
            if (ptrs[i]) ray_retain(ptrs[i]);
        }
    }

    return v;
}

/* --------------------------------------------------------------------------
 * Null bitmap operations
 *
 * Inline: for vectors with <=128 elements, bits stored in nullmap[16] (128 bits).
 * External: for >128 elements, allocate a U8 vector bitmap via ext_nullmap.
 * -------------------------------------------------------------------------- */

ray_err_t ray_vec_set_null_checked(ray_t* vec, int64_t idx, bool is_null) {
    if (!vec || RAY_IS_ERR(vec)) return RAY_ERR_TYPE;
    if (vec->attrs & RAY_ATTR_SLICE) return RAY_ERR_TYPE; /* cannot set null on slice — COW first */
    if (idx < 0 || idx >= vec->len) return RAY_ERR_RANGE;

    /* Mutation invalidates any attached accelerator index — drop it inline.
     * Caller must already hold a unique ref (set-null on a shared vec is a
     * bug regardless of indexing). */
    vec_drop_index_inplace(vec);

    /* Mark HAS_NULLS if setting a null (defer for RAY_STR until ext alloc succeeds) */
    if (is_null && vec->type != RAY_STR) vec->attrs |= RAY_ATTR_HAS_NULLS;

    if (!(vec->attrs & RAY_ATTR_NULLMAP_EXT)) {
        /* RAY_STR uses bytes 8-15 for str_pool, HAS_LINK uses bytes 8-15 for
         * link_target — both must skip the inline-128 path to avoid
         * aliasing corruption.  Otherwise <=128 elements go inline. */
        bool can_inline = (vec->type != RAY_STR) && idx < 128 &&
                          !(vec->attrs & RAY_ATTR_HAS_LINK);
        if (can_inline) {
            /* Inline nullmap path (<=128 elements, non-STR, non-linked) */
            int byte_idx = (int)(idx / 8);
            int bit_idx = (int)(idx % 8);
            if (is_null)
                vec->nullmap[byte_idx] |= (uint8_t)(1u << bit_idx);
            else
                vec->nullmap[byte_idx] &= (uint8_t)~(1u << bit_idx);
            return RAY_OK;
        }
        /* Need to promote to external nullmap */
        int64_t bitmap_len = (vec->len + 7) / 8;
        ray_t* ext = ray_vec_new(RAY_U8, bitmap_len);
        if (!ext || RAY_IS_ERR(ext)) return RAY_ERR_OOM;
        ext->len = bitmap_len;
        if (vec->type == RAY_STR || (vec->attrs & RAY_ATTR_HAS_LINK)) {
            /* Bytes 0-15 contain pointers/sym, not bits — start ext zeroed.
             * (Linked vecs reach here only when adding their first null,
             *  since promote_inline_to_ext in linkop.c covers the
             *  pre-existing-nulls case at attach time.) */
            memset(ray_data(ext), 0, (size_t)bitmap_len);
        } else {
            /* Copy existing inline bits */
            memcpy(ray_data(ext), vec->nullmap, 16);
            /* Zero remaining bytes */
            if (bitmap_len > 16)
                memset((char*)ray_data(ext) + 16, 0, (size_t)(bitmap_len - 16));
        }
        vec->attrs |= RAY_ATTR_NULLMAP_EXT;
        if (is_null) vec->attrs |= RAY_ATTR_HAS_NULLS;
        vec->ext_nullmap = ext;
    }

    /* External nullmap path */
    ray_t* ext = vec->ext_nullmap;
    /* Grow external bitmap if needed */
    int64_t needed_bytes = (idx / 8) + 1;
    if (needed_bytes > ext->len) {
        int64_t new_len = (vec->len + 7) / 8;
        if (new_len < needed_bytes) new_len = needed_bytes;
        size_t new_data_size = (size_t)new_len;
        int64_t old_len = ext->len;
        ray_t* new_ext = ray_scratch_realloc(ext, new_data_size);
        if (!new_ext || RAY_IS_ERR(new_ext)) return RAY_ERR_OOM;
        /* Zero new bytes */
        if (new_len > old_len)
            memset((char*)ray_data(new_ext) + old_len, 0,
                   (size_t)(new_len - old_len));
        new_ext->len = new_len;
        vec->ext_nullmap = new_ext;
        ext = new_ext;
    }

    uint8_t* bits = (uint8_t*)ray_data(ext);
    int byte_idx = (int)(idx / 8);
    int bit_idx = (int)(idx % 8);
    if (is_null)
        bits[byte_idx] |= (uint8_t)(1u << bit_idx);
    else
        bits[byte_idx] &= (uint8_t)~(1u << bit_idx);
    return RAY_OK;
}

void ray_vec_set_null(ray_t* vec, int64_t idx, bool is_null) {
    (void)ray_vec_set_null_checked(vec, idx, is_null);
}

/* --------------------------------------------------------------------------
 * str_pool_cow — ensure pool is privately owned after ray_cow()
 *
 * After ray_cow(), the copy shares the same str_pool as the original.
 * ray_retain_owned_refs bumps pool rc, so direct mutation would corrupt
 * the original's pool data (or ray_scratch_realloc would ray_free a
 * shared block).  Deep-copy the pool when rc > 1.
 * -------------------------------------------------------------------------- */

static ray_t* str_pool_cow(ray_t* vec) {
    if (!vec->str_pool || RAY_IS_ERR(vec->str_pool)) return vec;
    uint32_t pool_rc = ray_atomic_load(&vec->str_pool->rc);
    if (pool_rc <= 1) return vec;

    size_t pool_data_size = ((size_t)1 << vec->str_pool->order) - 32;
    ray_t* new_pool = ray_alloc(pool_data_size);
    if (!new_pool || RAY_IS_ERR(new_pool)) return NULL;

    size_t copy_bytes = (size_t)vec->str_pool->len;
    if (copy_bytes > pool_data_size) copy_bytes = pool_data_size;

    uint8_t saved_order = new_pool->order;
    uint8_t saved_mmod  = new_pool->mmod;
    memcpy(new_pool, vec->str_pool, 32 + copy_bytes);
    new_pool->order = saved_order;
    new_pool->mmod  = saved_mmod;
    ray_atomic_store(&new_pool->rc, 1);

    ray_release(vec->str_pool);
    vec->str_pool = new_pool;
    return vec;
}

/* --------------------------------------------------------------------------
 * String pool dead-byte tracking
 *
 * Dead bytes are stored as a uint32_t in the pool block's nullmap[0..3],
 * which is otherwise unused (the pool is a raw CHAR vector).
 * -------------------------------------------------------------------------- */

static inline uint32_t str_pool_dead(ray_t* vec) {
    if (!vec->str_pool) return 0;
    uint32_t d;
    memcpy(&d, vec->str_pool->nullmap, 4);
    return d;
}

static inline void str_pool_add_dead(ray_t* vec, uint32_t bytes) {
    uint32_t d = str_pool_dead(vec);
    d = (d > UINT32_MAX - bytes) ? UINT32_MAX : d + bytes;
    memcpy(vec->str_pool->nullmap, &d, 4);
}

/* --------------------------------------------------------------------------
 * ray_str_vec_append — append a string to a RAY_STR vector
 *
 * Strings <= 12 bytes are inlined in the ray_str_t element.
 * Strings > 12 bytes store a 4-byte prefix + offset into a growable pool.
 * -------------------------------------------------------------------------- */

ray_t* ray_str_vec_append(ray_t* vec, const char* s, size_t len) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type != RAY_STR) return ray_error("type", NULL);
    if (len > UINT32_MAX) return ray_error("range", NULL);

    ray_t* original = vec;
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (!str_pool_cow(vec)) goto fail_oom;

    int64_t pool_off = 0;
    if (len > RAY_STR_INLINE_MAX) {
        if (!vec->str_pool) {
            size_t init_pool = len < 256 ? 256 : len * 2;
            vec->str_pool = ray_alloc(init_pool);
            if (!vec->str_pool || RAY_IS_ERR(vec->str_pool)) {
                vec->str_pool = NULL;
                goto fail_oom;
            }
            vec->str_pool->type = RAY_U8;
            vec->str_pool->len = 0;
        }

        int64_t pool_used = vec->str_pool->len;
        size_t pool_cap = ((size_t)1 << vec->str_pool->order) - 32;
        if ((size_t)pool_used + len > pool_cap) {
            size_t need = (size_t)pool_used + len;
            size_t new_cap = pool_cap;
            if (new_cap == 0) new_cap = 256;
            while (new_cap < need) {
                if (new_cap > SIZE_MAX / 2) goto fail_oom;
                new_cap *= 2;
            }
            ray_t* np = ray_scratch_realloc(vec->str_pool, new_cap);
            if (!np || RAY_IS_ERR(np)) goto fail_oom;
            vec->str_pool = np;
        }

        if ((uint64_t)pool_used > UINT32_MAX) goto fail_range;
        pool_off = pool_used;
    }

    /* Grow element array if needed — pool is already ready */
    int64_t cap = vec_capacity(vec);
    if (vec->len >= cap) {
        size_t new_data_size = (size_t)(vec->len + 1) * sizeof(ray_str_t);
        if (new_data_size < 32) new_data_size = 32;
        else {
            size_t s2 = 32;
            while (s2 < new_data_size) {
                if (s2 > SIZE_MAX / 2) goto fail_oom;
                s2 *= 2;
            }
            new_data_size = s2;
        }
        ray_t* nv = ray_scratch_realloc(vec, new_data_size);
        if (!nv || RAY_IS_ERR(nv)) goto fail_oom;
        vec = nv;
    }

    ray_str_t* elem = &((ray_str_t*)ray_data(vec))[vec->len];
    memset(elem, 0, sizeof(ray_str_t));
    elem->len = (uint32_t)len;

    if (len <= RAY_STR_INLINE_MAX) {
        if (len > 0) memcpy(elem->data, s, len);
    } else {
        /* Copy string into pool (already allocated above) */
        char* pool_base = (char*)ray_data(vec->str_pool);
        memcpy(pool_base + pool_off, s, len);

        memcpy(elem->prefix, s, 4);
        elem->pool_off = (uint32_t)pool_off;
        vec->str_pool->len = pool_off + (int64_t)len;
    }

    vec->len++;
    return vec;

fail_oom:
    if (vec != original) ray_release(vec);
    return ray_error("oom", NULL);
fail_range:
    if (vec != original) ray_release(vec);
    return ray_error("range", NULL);
}

/* --------------------------------------------------------------------------
 * ray_str_vec_get — read a string from a RAY_STR vector by index
 *
 * Returns a pointer to the string data (inline or pool) and sets *out_len.
 * Returns NULL for invalid input or out-of-bounds index.
 * -------------------------------------------------------------------------- */

const char* ray_str_vec_get(ray_t* vec, int64_t idx, size_t* out_len) {
    if (out_len) *out_len = 0;
    if (!vec || RAY_IS_ERR(vec) || vec->type != RAY_STR) return NULL;
    if (idx < 0 || idx >= vec->len) return NULL;

    /* Slice: redirect to parent */
    ray_t* data_owner = vec;
    int64_t data_idx = idx;
    if (vec->attrs & RAY_ATTR_SLICE) {
        data_owner = vec->slice_parent;
        data_idx = vec->slice_offset + idx;
    }

    const ray_str_t* elem = &((const ray_str_t*)ray_data(data_owner))[data_idx];
    if (out_len) *out_len = elem->len;

    if (elem->len == 0) return "";
    if (ray_str_is_inline(elem)) return elem->data;

    /* Pooled: resolve via pool */
    if (!data_owner->str_pool) return NULL;
    return (const char*)ray_data(data_owner->str_pool) + elem->pool_off;
}

/* --------------------------------------------------------------------------
 * ray_str_vec_set — update string at index in a RAY_STR vector
 *
 * Overwrites element at idx. Old pooled bytes become dead space (reclaimed
 * by ray_str_vec_compact). New pooled strings are appended to the pool.
 * -------------------------------------------------------------------------- */

ray_t* ray_str_vec_set(ray_t* vec, int64_t idx, const char* s, size_t len) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type != RAY_STR) return ray_error("type", NULL);
    if (idx < 0 || idx >= vec->len) return ray_error("range", NULL);
    if (len > UINT32_MAX) return ray_error("range", NULL);

    ray_t* original = vec;
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (!str_pool_cow(vec)) goto fail_oom;

    ray_str_t* elem = &((ray_str_t*)ray_data(vec))[idx];

    if (len <= RAY_STR_INLINE_MAX) {
        /* Track dead bytes if old string was pooled */
        if (!ray_str_is_inline(elem) && elem->len > 0 && vec->str_pool) {
            str_pool_add_dead(vec, elem->len);
        }
        memset(elem, 0, sizeof(ray_str_t));
        elem->len = (uint32_t)len;
        if (len > 0) memcpy(elem->data, s, len);
    } else {
        if (!vec->str_pool) {
            size_t init_pool = len < 256 ? 256 : len * 2;
            vec->str_pool = ray_alloc(init_pool);
            if (!vec->str_pool || RAY_IS_ERR(vec->str_pool)) {
                vec->str_pool = NULL;
                goto fail_oom;
            }
            vec->str_pool->type = RAY_U8;
            vec->str_pool->len = 0;
        }

        /* Grow pool if needed */
        int64_t pool_used = vec->str_pool->len;
        size_t pool_cap = ((size_t)1 << vec->str_pool->order) - 32;
        if ((size_t)pool_used + len > pool_cap) {
            size_t need = (size_t)pool_used + len;
            size_t new_cap = pool_cap;
            if (new_cap == 0) new_cap = 256;
            while (new_cap < need) {
                if (new_cap > SIZE_MAX / 2) goto fail_oom;
                new_cap *= 2;
            }
            ray_t* np = ray_scratch_realloc(vec->str_pool, new_cap);
            if (!np || RAY_IS_ERR(np)) goto fail_oom;
            vec->str_pool = np;
        }

        if ((uint64_t)pool_used > UINT32_MAX) goto fail_range;

        /* Pool alloc succeeded — now safe to modify the element */
        if (!ray_str_is_inline(elem) && elem->len > 0 && vec->str_pool) {
            str_pool_add_dead(vec, elem->len);
        }

        char* pool_base = (char*)ray_data(vec->str_pool);
        memcpy(pool_base + pool_used, s, len);
        memset(elem, 0, sizeof(ray_str_t));
        elem->len = (uint32_t)len;
        memcpy(elem->prefix, s, 4);
        elem->pool_off = (uint32_t)pool_used;
        vec->str_pool->len = pool_used + (int64_t)len;
    }

    return vec;

fail_oom:
    if (vec != original) ray_release(vec);
    return ray_error("oom", NULL);
fail_range:
    if (vec != original) ray_release(vec);
    return ray_error("range", NULL);
}

/* --------------------------------------------------------------------------
 * ray_str_vec_insert_at — insert a single string at position idx.
 *
 * Wraps (s, len) into a 1-element RAY_STR vector and delegates to
 * ray_vec_insert_vec_at, which handles pool merging via ray_vec_concat.
 * -------------------------------------------------------------------------- */

ray_t* ray_str_vec_insert_at(ray_t* vec, int64_t idx, const char* s, size_t len) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type != RAY_STR) return ray_error("type", NULL);
    if (idx < 0 || idx > vec->len) return ray_error("range", NULL);

    ray_t* tmp = ray_vec_new(RAY_STR, 1);
    if (!tmp || RAY_IS_ERR(tmp)) return tmp ? tmp : ray_error("oom", NULL);

    ray_t* tmp2 = ray_str_vec_append(tmp, s, len);
    if (!tmp2 || RAY_IS_ERR(tmp2)) { ray_release(tmp); return tmp2 ? tmp2 : ray_error("oom", NULL); }

    ray_t* result = ray_vec_insert_vec_at(vec, idx, tmp2);
    ray_release(tmp2);
    return result;
}

/* --------------------------------------------------------------------------
 * ray_str_vec_compact — reclaim dead pool space
 *
 * Allocates a fresh pool containing only live pooled strings, updates
 * element offsets, and releases the old pool.
 * -------------------------------------------------------------------------- */

ray_t* ray_str_vec_compact(ray_t* vec) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type != RAY_STR) return ray_error("type", NULL);
    if (!vec->str_pool || str_pool_dead(vec) == 0) return vec;

    ray_t* original = vec;
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (!str_pool_cow(vec)) {
        if (vec != original) ray_release(vec);
        return ray_error("oom", NULL);
    }

    /* Compute true live size by scanning elements — avoids overflow when
     * the dead-byte counter (uint32_t) has saturated at UINT32_MAX. */
    ray_str_t* elems = (ray_str_t*)ray_data(vec);
    size_t live_size = 0;
    for (int64_t i = 0; i < vec->len; i++) {
        if (ray_vec_is_null(vec, i) || ray_str_is_inline(&elems[i]) || elems[i].len == 0) continue;
        live_size += elems[i].len;
    }

    if (live_size == 0) {
        ray_release(vec->str_pool);
        vec->str_pool = NULL;
        return vec;
    }

    ray_t* new_pool = ray_alloc(live_size);
    if (!new_pool || RAY_IS_ERR(new_pool)) return vec;
    new_pool->type = RAY_U8;
    new_pool->len = 0;
    memset(new_pool->nullmap, 0, 16);

    char* old_base = (char*)ray_data(vec->str_pool);
    char* new_base = (char*)ray_data(new_pool);
    uint32_t write_off = 0;

    for (int64_t i = 0; i < vec->len; i++) {
        if (ray_vec_is_null(vec, i) || ray_str_is_inline(&elems[i]) || elems[i].len == 0) continue;

        uint32_t slen = elems[i].len;
        memcpy(new_base + write_off, old_base + elems[i].pool_off, slen);
        elems[i].pool_off = write_off;
        write_off += slen;
    }

    new_pool->len = (int64_t)write_off;
    ray_release(vec->str_pool);
    vec->str_pool = new_pool;

    return vec;
}

/* --------------------------------------------------------------------------
 * ray_embedding_new — create a flat F32 vector for N*D embedding storage
 * -------------------------------------------------------------------------- */

ray_t* ray_embedding_new(int64_t nrows, int32_t dim) {
    int64_t total = nrows * (int64_t)dim;
    ray_t* v = ray_vec_new(RAY_F32, total);
    if (!v || RAY_IS_ERR(v)) return v;
    v->len = total;
    return v;
}

bool ray_vec_is_null(ray_t* vec, int64_t idx) {
    if (!vec || RAY_IS_ERR(vec)) return false;
    if (idx < 0 || idx >= vec->len) return false;

    /* Slice: delegate to parent with adjusted index */
    if (vec->attrs & RAY_ATTR_SLICE) {
        ray_t* parent = vec->slice_parent;
        int64_t pidx = vec->slice_offset + idx;
        return ray_vec_is_null(parent, pidx);
    }

    if (!vec_any_nulls(vec)) return false;

    ray_t* ext = NULL;
    const uint8_t* inline_bits = vec_inline_nullmap(vec, &ext);
    if (ext) {
        int64_t byte_idx = idx / 8;
        if (byte_idx >= ext->len) return false;
        const uint8_t* bits = (const uint8_t*)ray_data(ext);
        return (bits[byte_idx] >> (idx % 8)) & 1;
    }

    /* Inline nullmap path.  RAY_STR's inline 16 bytes hold str_pool/str_ext_null
     * (or, when an index is attached, were the same and are now in the index
     * snapshot).  Either way, RAY_STR uses ext nullmap exclusively for its
     * null bits, which is handled above; if the inline path is taken for
     * RAY_STR, no nulls are present. */
    if (vec->type == RAY_STR) return false;
    if (idx >= 128) return false;
    int byte_idx = (int)(idx / 8);
    int bit_idx = (int)(idx % 8);
    return (inline_bits[byte_idx] >> bit_idx) & 1;
}

/* --------------------------------------------------------------------------
 * ray_vec_copy_nulls — bulk-copy null bitmap from src to dst
 *
 * dst must have the same len as src (or at least as many elements).
 * Handles inline, external, and slice source bitmaps.
 * -------------------------------------------------------------------------- */

ray_err_t ray_vec_copy_nulls(ray_t* dst, const ray_t* src) {
    if (!dst || !src) return RAY_ERR_TYPE;

    /* Use ray_vec_is_null which handles slices, inline, and external bitmaps
     * transparently. For non-null sources this returns immediately. */
    bool has_any = false;
    if (src->attrs & RAY_ATTR_SLICE) {
        const ray_t* parent = src->slice_parent;
        if (parent && (parent->attrs & RAY_ATTR_HAS_NULLS)) has_any = true;
    } else {
        if (src->attrs & RAY_ATTR_HAS_NULLS) has_any = true;
    }
    if (!has_any) return RAY_OK;

    for (int64_t i = 0; i < dst->len && i < src->len; i++) {
        if (ray_vec_is_null((ray_t*)src, i)) {
            ray_err_t err = ray_vec_set_null_checked(dst, i, true);
            if (err != RAY_OK) return err;
        }
    }
    return RAY_OK;
}
