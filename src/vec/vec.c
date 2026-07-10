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
#include "table/domain.h"
#include "vec/embedding.h"
#include "vec/str.h"
#include "ops/idxop.h"
#include "lang/format.h"  /* ray_type_name */
#include <string.h>
#include <stdlib.h>

/* qsort comparator for (idx, original_k) pairs in ray_vec_insert_many.
 * Sorts primarily by idx ascending; ties break by original k to preserve
 * stable-sort semantics (matches the previous insertion-sort behaviour). */
static int pair_cmp_idx_then_k(const void* a, const void* b) {
    const int64_t* pa = (const int64_t*)a;
    const int64_t* pb = (const int64_t*)b;
    if (pa[0] != pb[0]) return (pa[0] > pb[0]) - (pa[0] < pb[0]);
    return (pa[1] > pb[1]) - (pa[1] < pb[1]);
}

/* Sentinel-based per-element null test.  Caller guarantees v is a
 * non-slice vector (type > 0) and idx is in range.  Returns true iff
 * payload[idx] equals the type-correct NULL_* sentinel.  F64/F32 use
 * (x != x) to detect any NaN bit pattern.  BOOL/U8 are non-nullable
 * and return false. */
static inline bool sentinel_is_null(const ray_t* v, int64_t idx) {
    const void* p = ray_data((ray_t*)v);
    switch (v->type) {
        case RAY_F64: {
            double x = ((const double*)p)[idx];
            return x != x;
        }
        case RAY_F32: {
            float x = ((const float*)p)[idx];
            return x != x;
        }
        case RAY_I64:
        case RAY_TIMESTAMP:
            return ((const int64_t*)p)[idx] == NULL_I64;
        case RAY_I32:
        case RAY_DATE:
        case RAY_TIME:
            return ((const int32_t*)p)[idx] == NULL_I32;
        case RAY_I16:
            return ((const int16_t*)p)[idx] == NULL_I16;
        /* SYM has no null (sym id 0 is the empty value, not null) —
         * no arm here; ray_vec_is_null short-circuits SYM before this switch. */
        case RAY_STR:
            return ((const ray_str_t*)p)[idx].len == 0;
        case RAY_GUID: {
            /* GUID null = 16 all-zero bytes (canonical convention). */
            static const uint8_t Z[16] = {0};
            return memcmp((const uint8_t*)p + idx * 16, Z, 16) == 0;
        }
        case RAY_BOOL:
        case RAY_U8:
        default:
            return false;
    }
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
 * so it needs no restoration.
 *
 * Shared-index case: `v` may share its index ray_t with another vec
 * (e.g. after ray_cow followed by ray_retain_owned_refs, both copies
 * point at the same RAY_INDEX with rc==2).  We must NOT clobber the
 * saved-aux bytes inside a shared index — the other holder still
 * reads them.  Detect rc>1 and copy the saved pointers via
 * ray_index_retain_saved instead of moving them out. */
static inline void vec_drop_index_inplace(ray_t* v) {
    /* Any payload/length mutation invalidates the physical-order promise,
     * even when no accelerator index is attached. */
    v->attrs &= (uint8_t)~RAY_ATTR_SORTED;
    if (!(v->attrs & RAY_ATTR_HAS_INDEX)) return;
    ray_t* idx = v->index;
    ray_index_t* ix = ray_index_payload(idx);
    bool shared = ray_atomic_load(&idx->rc) > 1;

    if (shared) {
        /* Take our own retained references to the saved-pointer slots
         * (str_pool etc.) so the bytes we copy into v->aux
         * are validly owned by v.  Leave the index's snapshot intact for
         * the other holder. */
        ray_index_retain_saved(ix);
    }
    memcpy(v->aux, ix->saved_aux, 16);
    if (!shared) {
        /* Sole owner: about to release idx, so neutralize its snapshot
         * to prevent ray_index_release_saved from double-releasing the
         * pointers we just transferred to v. */
        memset(ix->saved_aux, 0, 16);
        ix->saved_attrs = 0;
    }
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_INDEX;
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
    /* ray_block_data_bytes handles both buddy blocks (1<<order - 32) and direct
     * mmap'd blocks (exact size - prefix - header); the latter carry a sentinel
     * order, so 1<<order would be garbage. */
    size_t data_space = ray_block_data_bytes(vec);
    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);
    if (esz == 0) return 0;
    return (int64_t)(data_space / esz);
}

/* --------------------------------------------------------------------------
 * ray_vec_new
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_new(int8_t type, int64_t capacity) {
    if (type <= 0 || type >= RAY_TYPE_COUNT)
        return ray_error("type", "vec_new: type must be a positive concrete vector type, got %s", ray_type_name(type));
    if (type == RAY_SYM)
        return ray_sym_vec_new(RAY_SYM_W64, capacity);  /* default: global sym IDs */
    if (capacity < 0) return ray_error("range", "vec_new: capacity must be non-negative, got %lld", (long long)capacity);

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
    memset(v->aux, 0, 16);
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
        return ray_error("type", "sym_vec_new: invalid sym width, expected one of RAY_SYM_W8/W16/W32/W64, got %u", (unsigned)sym_width);
    if (capacity < 0) return ray_error("range", "sym_vec_new: capacity must be non-negative, got %lld", (long long)capacity);

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
    memset(v->aux, 0, 16);
    /* Every SYM vec carries a non-NULL resolution domain.  This is the
     * single chokepoint all runtime SYM vec construction funnels through;
     * loaded columns are patched in src/store/col.c.  The singleton is
     * immortal — no retain needed. */
    v->sym_domain = ray_sym_runtime_domain();

    return v;
}

/* --------------------------------------------------------------------------
 * ray_sym_vec_adopt_domain
 *
 * An output SYM vec whose cells were COPIED from `in` must resolve over
 * the same dictionary.  Retain-before-release so adopting a vec's own
 * domain is safe; retain/release are no-ops on the runtime singleton, so
 * this is free on the hot (all-runtime) path.  Slice headers keep
 * parent/offset in the aux bytes — never write a domain into one (the
 * accessor resolves through slice_parent anyway).
 * -------------------------------------------------------------------------- */

void ray_sym_vec_adopt_domain(ray_t* out, ray_t* in) {
    if (!out || !in) return;
    if (out->type != RAY_SYM || in->type != RAY_SYM) return;
    if (out->attrs & RAY_ATTR_SLICE) return;

    struct ray_sym_domain_s* dom = ray_sym_vec_domain(in);
    if (out->sym_domain == dom) return;

    ray_sym_domain_retain(dom);
    ray_sym_domain_release(out->sym_domain);
    out->sym_domain = dom;
}

/* --------------------------------------------------------------------------
 * ray_vec_append
 * -------------------------------------------------------------------------- */

ray_t* ray_vec_append(ray_t* vec, const void* elem) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type <= 0 || vec->type >= RAY_TYPE_COUNT)
        return ray_error("type", "vec_append: expects a concrete vector type, got %s", ray_type_name(vec->type));
    if (vec->type == RAY_STR) return ray_error("type", "vec_append: str vectors use ray_str_vec_append, got %s", ray_type_name(vec->type));

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
    if (vec->type == RAY_STR) return ray_error("type", "vec_set: str vectors use ray_str_vec_set, got %s", ray_type_name(vec->type));
    if (idx < 0 || idx >= vec->len)
        return ray_error("range", "vec_set: index out of bounds [0,%lld), got %lld", (long long)vec->len, (long long)idx);

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

/* Out-of-line slice arm for ray_data_fn (declared in rayforce.h).  Kept
 * here so the single instantiation lives next to other slice handling
 * code, and llvm-cov sees the rare slice path once rather than as a
 * dead inline copy in every TU that includes the public header. */
void* ray_data_slice_path(ray_t* v) {
    return (char*)v->slice_parent->data
           + v->slice_offset * ray_type_sizes[(uint8_t)v->type];
}

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
        return ray_error("range", "vec_slice: offset+len must stay within [0,%lld], got offset %lld len %lld", (long long)vec->len, (long long)offset, (long long)len);

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
        return ray_error("type", "concat: operands must have matching type, got %s and %s", ray_type_name(a->type), ray_type_name(b->type));

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
            return ray_error("range", "concat: merged str pool exceeds %lld bytes, got %lld", (long long)UINT32_MAX, (long long)total_pool);
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

    /* SYM cross-domain concat (post-flip reachable: a loaded FILE-domain
     * column concatenated with a runtime one): positions in different
     * dictionaries must not be raw-mixed.  Materialize as RUNTIME-domain
     * W64 ids, translating each side through its domain's runtime-id LUT
     * (NULL LUT = runtime side, ids pass through) — invariant 5: the
     * translation rides this pass, which touches every row anyway. */
    struct ray_sym_domain_s* concat_dom = NULL;
    const int64_t* sym_lut_a = NULL;
    const int64_t* sym_lut_b = NULL;
    bool sym_translate = false;
    if (a->type == RAY_SYM) {
        struct ray_sym_domain_s* da = ray_sym_vec_domain(a);
        struct ray_sym_domain_s* db = ray_sym_vec_domain(b);
        if (da == db) {
            concat_dom = da;
        } else {
            concat_dom = ray_sym_runtime_domain();
            sym_translate = true;
            sym_lut_a = ray_sym_domain_runtime_lut(da);
            sym_lut_b = ray_sym_domain_runtime_lut(db);
            if ((da != concat_dom && !sym_lut_a) ||
                (db != concat_dom && !sym_lut_b))
                return ray_error("oom", "concat: sym domain LUT build failed");
            out_attrs = RAY_SYM_W64;
            esz = 8;
        }
    }

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
    memset(result->aux, 0, 16);

    /* SYM: propagate the resolution domain — inputs agree → that domain;
     * mixed → runtime (cells re-expressed below). */
    if (result->type == RAY_SYM) {
        ray_sym_domain_retain(concat_dom);
        result->sym_domain = concat_dom;
    }

    if (sym_translate) {
        /* Cross-domain: re-express both sides as runtime ids. */
        int64_t* dst = (int64_t*)ray_data(result);
        int64_t cnt_a = ray_sym_domain_count(ray_sym_vec_domain(a));
        int64_t cnt_b = ray_sym_domain_count(ray_sym_vec_domain(b));
        /* An out-of-range position means corrupt input — never silently
         * map it to 0 (the SYM null ''): loud error instead. */
        for (int64_t i = 0; i < a->len; i++) {
            int64_t val = ray_read_sym(ray_data(a), i, a->type, a->attrs);
            if (sym_lut_a) {
                if (val < 0 || val >= cnt_a) {
                    ray_release(result);
                    return ray_error("corrupt",
                        "sym position %lld out of domain range %lld in concat",
                        (long long)val, (long long)cnt_a);
                }
                val = sym_lut_a[val];
            }
            dst[i] = val;
        }
        for (int64_t i = 0; i < b->len; i++) {
            int64_t val = ray_read_sym(ray_data(b), i, b->type, b->attrs);
            if (sym_lut_b) {
                if (val < 0 || val >= cnt_b) {
                    ray_release(result);
                    return ray_error("corrupt",
                        "sym position %lld out of domain range %lld in concat",
                        (long long)val, (long long)cnt_b);
                }
                val = sym_lut_b[val];
            }
            dst[a->len + i] = val;
        }
    } else if (a->type == RAY_SYM && a_esz != b_esz) {
        /* Same domain, mismatched widths: widen element-by-element */
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
        return ray_error("type", "vec_insert_at: expects a concrete vector type, got %s", ray_type_name(vec->type));
    if (vec->type == RAY_STR) return ray_error("type", "vec_insert_at: str vectors use ray_str_vec_insert_at, got %s", ray_type_name(vec->type));
    if (idx < 0 || idx > vec->len) return ray_error("range", "vec_insert_at: index out of bounds [0,%lld], got %lld", (long long)vec->len, (long long)idx);

    /* COW: if shared, copy first */
    ray_t* original = vec;
    vec = ray_cow(vec);
    if (!vec || RAY_IS_ERR(vec)) return vec;

    /* In-place insert mutates len + data + aux; any attached
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

    /* Null info for every type that accepts HAS_NULLS is sentinel-encoded
     * in the payload (see ray_vec_is_null + ray_vec_set_null_checked).
     * The memmove above moved the data — including any null sentinels —
     * to their new slots, so no separate bitmap shift is needed.  The
     * caller-supplied `elem` lands at idx; if it carries a NULL_*
     * sentinel the HAS_NULLS bit is already set on `vec` (we don't clear
     * it — we have no cheap way to detect "this insert removed the last
     * null"; HAS_NULLS being a strict over-approximation is harmless). */

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
    if (vec->type != src->type) return ray_error("type", "vec_insert_vec_at: dest and src must have matching type, got %s and %s", ray_type_name(vec->type), ray_type_name(src->type));
    if (idx < 0 || idx > vec->len) return ray_error("range", "vec_insert_vec_at: index out of bounds [0,%lld], got %lld", (long long)vec->len, (long long)idx);

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
    if (vec->type <= 0 || vec->type >= RAY_TYPE_COUNT) return ray_error("type", "vec_insert_many: dest expects a concrete vector type, got %s", ray_type_name(vec->type));
    if (vec->type == RAY_STR) return ray_error("type", "vec_insert_many: str vectors are unsupported, use ray_vec_insert_vec_at in a loop, got %s", ray_type_name(vec->type));
    if (idxs->type != RAY_I64) return ray_error("type", "vec_insert_many: indices must be an i64 vector, got %s", ray_type_name(idxs->type));

    int64_t N = idxs->len;
    int64_t old_len = vec->len;
    uint8_t esz = ray_sym_elem_size(vec->type, vec->attrs);

    /* Fast path: N == 0 returns a fresh retain */
    if (N == 0) { ray_retain(vec); return vec; }

    /* Validate indices */
    const int64_t* idx_arr = (const int64_t*)ray_data(idxs);
    for (int64_t k = 0; k < N; k++) {
        if (idx_arr[k] < 0 || idx_arr[k] > old_len)
            return ray_error("range", "vec_insert_many: index out of bounds [0,%lld], got %lld", (long long)old_len, (long long)idx_arr[k]);
    }

    /* Classify vals: atom (broadcast) vs vec (parallel or singleton broadcast) */
    int broadcast;
    if (vals->type < 0) {
        if (vals->type != -vec->type) return ray_error("type", "vec_insert_many: scalar value must be an atom of the dest type %s, got %s", ray_type_name(vec->type), ray_type_name(vals->type));
        broadcast = 1;
    } else if (vals->type == vec->type) {
        /* SYM width must match — dispatcher should widen upstream */
        if (vec->type == RAY_SYM &&
            (vals->attrs & RAY_SYM_W_MASK) != (vec->attrs & RAY_SYM_W_MASK))
            return ray_error("type", "vec_insert_many: sym value width must match dest width, got vals width %u dest width %u", (unsigned)(vals->attrs & RAY_SYM_W_MASK), (unsigned)(vec->attrs & RAY_SYM_W_MASK));
        if (vals->len == 1) broadcast = 1;
        else if (vals->len == N) broadcast = 0;
        else return ray_error("range", "vec_insert_many: value count must be 1 or match index count %lld, got %lld", (long long)N, (long long)vals->len);
    } else {
        return ray_error("type", "vec_insert_many: values must be an atom or vector of the dest type %s, got %s", ray_type_name(vec->type), ray_type_name(vals->type));
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

    /* Stable sort the (idx, original_k) pairs by idx.  qsort isn't
     * inherently stable, but a compound comparator on (idx, k) — where
     * k is the original position — gives the same total order as a
     * stable sort by idx alone.  Replaces an O(N^2) insertion sort
     * that hangs for bulk-set updates with thousands+ of indices. */
    qsort(pairs, (size_t)N, 2 * sizeof(int64_t), pair_cmp_idx_then_k);

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
    memset(result->aux, 0, 16);
    if (result->type == RAY_SYM) {
        /* Fresh SYM result inherits the source vec's domain. */
        struct ray_sym_domain_s* dom = ray_sym_vec_domain(vec);
        ray_sym_domain_retain(dom);
        result->sym_domain = dom;
    }

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
        return ray_error("type", "vec_from_raw: type must be a positive concrete vector type, got %s", ray_type_name(type));
    if (type == RAY_STR) return ray_error("type", "vec_from_raw: str vectors are unsupported (no pool), got %s", ray_type_name(type));
    if (count < 0) return ray_error("range", "vec_from_raw: count must be non-negative, got %lld", (long long)count);

    /* RAY_SYM defaults to W64 (global sym IDs) */
    uint8_t sym_w = (type == RAY_SYM) ? RAY_SYM_W64 : 0;
    uint8_t esz = ray_sym_elem_size(type, sym_w);
    size_t data_size = (size_t)count * esz;

    ray_t* v = ray_alloc(data_size);
    if (!v || RAY_IS_ERR(v)) return v;

    v->type = type;
    v->len = count;
    v->attrs = sym_w;
    memset(v->aux, 0, 16);
    if (type == RAY_SYM)
        v->sym_domain = ray_sym_runtime_domain();  /* immortal — no retain */

    if (data_size) {
        if (!data) { ray_release(v); return ray_error("domain", "vec_from_raw: data pointer is null but count is %lld", (long long)count); }
        memcpy(ray_data(v), data, data_size);
    }

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
 * Null state operations
 *
 * Null state is encoded in-band via the type-correct NULL_* sentinel in
 * the payload (F64/F32 NaN, NULL_I64 / NULL_I32 / NULL_I16, ray_str_t{0,0},
 * 16 zero bytes for GUID).  A vec-level RAY_ATTR_HAS_NULLS flag is a
 * cheap fast-path gate; ray_vec_is_null reads the payload as source of
 * truth.  BOOL/U8/SYM are non-nullable.
 * -------------------------------------------------------------------------- */

ray_err_t ray_vec_set_null_checked(ray_t* vec, int64_t idx, bool is_null) {
    if (!vec || RAY_IS_ERR(vec)) return RAY_ERR_TYPE;
    if (vec->attrs & RAY_ATTR_SLICE) return RAY_ERR_TYPE; /* cannot set null on slice — COW first */
    if (idx < 0 || idx >= vec->len) return RAY_ERR_RANGE;

    /* Types that don't accept set-null:
     *   - SYM: sym ID 0 (interned empty string, reserved by
     *     ray_sym_init) is the canonical "missing" value; callers
     *     write 0 directly.
     *   - BOOL / U8: non-nullable; they have nowhere to store a
     *     null, so reject to keep the producer surface clean. */
    if (vec->type == RAY_SYM ||
        vec->type == RAY_BOOL ||
        vec->type == RAY_U8) return RAY_ERR_TYPE;

    /* Mutation invalidates any attached accelerator index — drop it inline.
     * Caller must already hold a unique ref (set-null on a shared vec is a
     * bug regardless of indexing). */
    vec_drop_index_inplace(vec);

    /* Every remaining vec type uses a sentinel: F64/F32/I64/TIMESTAMP/
     * I32/DATE/TIME/I16/STR/GUID.  Write the type-correct NULL_* into
     * the payload and set HAS_NULLS.  ray_vec_is_null (the sole reader)
     * recovers null state from the payload.  Caller owns the payload on
     * is_null=false (we have no way to know the prior real value); the
     * clear path is a no-op. */
    if (is_null) {
        void* p = ray_data(vec);
        switch (vec->type) {
            case RAY_F64:                          ((double*)p)[idx] = NULL_F64; break;
            case RAY_F32:                          ((float*)p)[idx]  = NULL_F32; break;
            case RAY_I64: case RAY_TIMESTAMP:      ((int64_t*)p)[idx] = NULL_I64; break;
            case RAY_I32: case RAY_DATE: case RAY_TIME: ((int32_t*)p)[idx] = NULL_I32; break;
            case RAY_I16:                          ((int16_t*)p)[idx] = NULL_I16; break;
            case RAY_STR:
                /* STR has no null distinct from "": write the
                 * empty string but do NOT mark the column nullable. */
                memset(&((ray_str_t*)p)[idx], 0, sizeof(ray_str_t));
                return RAY_OK;
            case RAY_GUID:
                memset((uint8_t*)p + idx * 16, 0, 16);
                break;
            default: return RAY_ERR_TYPE;
        }
        vec->attrs |= RAY_ATTR_HAS_NULLS;
    }
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
    if (pool_rc <= 1 && vec->str_pool->mmod == 0) return vec;

    size_t pool_data_size = vec->str_pool->mmod == 0
        ? ray_block_data_bytes(vec->str_pool)
        : (vec->str_pool->len > 64 ? (size_t)vec->str_pool->len : 64);
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
 * Dead bytes are stored as a uint32_t in the pool block's aux[0..3],
 * which is otherwise unused (the pool is a raw CHAR vector).
 * -------------------------------------------------------------------------- */

static inline uint32_t str_pool_dead(ray_t* vec) {
    if (!vec->str_pool) return 0;
    uint32_t d;
    memcpy(&d, vec->str_pool->aux, 4);
    return d;
}

static inline void str_pool_add_dead(ray_t* vec, uint32_t bytes) {
    uint32_t d = str_pool_dead(vec);
    d = (d > UINT32_MAX - bytes) ? UINT32_MAX : d + bytes;
    memcpy(vec->str_pool->aux, &d, 4);
}

/* --------------------------------------------------------------------------
 * ray_str_vec_append — append a string to a RAY_STR vector
 *
 * Strings <= 12 bytes are inlined in the ray_str_t element.
 * Strings > 12 bytes store a 4-byte prefix + offset into a growable pool.
 * -------------------------------------------------------------------------- */

ray_t* ray_str_vec_append(ray_t* vec, const char* s, size_t len) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type != RAY_STR) return ray_error("type", "str_vec_append: expects a str vector, got %s", ray_type_name(vec->type));
    if (len > UINT32_MAX) return ray_error("range", "str_vec_append: string length exceeds %lld bytes, got %lld", (long long)UINT32_MAX, (long long)len);

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
        if (pool_used < 0 || (uint64_t)pool_used > UINT32_MAX ||
            (uint64_t)len > UINT32_MAX - (uint64_t)pool_used)
            goto fail_range;
        size_t pool_cap = ray_block_data_bytes(vec->str_pool);
        size_t need = (size_t)pool_used + len;
        if (need > pool_cap) {
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
    return ray_error("range", "str_vec_append: pool offset exceeds %lld bytes", (long long)UINT32_MAX);
}

/* --------------------------------------------------------------------------
 * ray_str_vec_from_parts — bulk-build a RAY_STR vec in two passes (no COW)
 *
 * Allocates the string pool once (not per-element like ray_str_vec_append).
 * Pass 1 sums pooled bytes; pass 2 fills descriptors and pool in one sweep.
 * ptrs[i] may be NULL when lens[i]==0.  nulls may be NULL (no nulls); when
 * non-NULL, nulls[i]!=0 marks element i null (STR null = empty string —
 * len==0, no RAY_ATTR_HAS_NULLS set).
 * -------------------------------------------------------------------------- */

ray_t* ray_str_vec_from_parts(const char* const* ptrs, const uint32_t* lens,
                               const uint8_t* nulls, int64_t n) {
    if (n < 0) return ray_error("range", "str_vec_from_parts: n must be non-negative, got %lld", (long long)n);

    ray_t* v = ray_vec_new(RAY_STR, n);
    if (!v || RAY_IS_ERR(v)) return v;

    /* Pass 1: count pool bytes needed for elements that don't inline */
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        if ((!nulls || !nulls[i]) && lens[i] > RAY_STR_INLINE_MAX) {
            if (total > UINT32_MAX - lens[i]) {
                ray_release(v);
                return ray_error("range", "str_vec_from_parts: pool offset exceeds %lld bytes", (long long)UINT32_MAX);
            }
            total += lens[i];
        }
    }

    /* Allocate pool once — mirrors the setup in ray_str_vec_append */
    if (total > 0) {
        v->str_pool = ray_alloc(total + 32);
        if (!v->str_pool || RAY_IS_ERR(v->str_pool)) {
            v->str_pool = NULL;
            ray_release(v);
            return ray_error("oom", NULL);
        }
        v->str_pool->type = RAY_U8;
        v->str_pool->len  = 0;
    }

    /* Pass 2: fill descriptors and pool */
    ray_str_t* elems     = (ray_str_t*)ray_data(v);
    char*      pool_base = v->str_pool ? (char*)ray_data(v->str_pool) : NULL;
    int64_t    pool_used = 0;

    for (int64_t i = 0; i < n; i++) {
        ray_str_t* d = &elems[i];
        memset(d, 0, sizeof(ray_str_t));
        if (nulls && nulls[i]) {
            /* STR null = empty string: d is already zeroed (len=0) */
        } else if (lens[i] <= RAY_STR_INLINE_MAX) {
            d->len = lens[i];
            if (lens[i] > 0) memcpy(d->data, ptrs[i], lens[i]);
        } else {
            if ((uint64_t)pool_used > UINT32_MAX ||
                (uint64_t)lens[i] > UINT32_MAX - (uint64_t)pool_used) {
                ray_release(v);
                return ray_error("range", "str_vec_from_parts: pool offset exceeds %lld bytes", (long long)UINT32_MAX);
            }
            memcpy(pool_base + pool_used, ptrs[i], lens[i]);
            d->len      = lens[i];
            d->pool_off = (uint32_t)pool_used;
            memcpy(d->prefix, ptrs[i], 4);
            pool_used  += (int64_t)lens[i];
        }
    }

    if (v->str_pool) v->str_pool->len = pool_used;
    v->len = n;

    return v;
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
    if (vec->type != RAY_STR) return ray_error("type", "str_vec_set: expects a str vector, got %s", ray_type_name(vec->type));
    if (idx < 0 || idx >= vec->len) return ray_error("range", "str_vec_set: index out of bounds [0,%lld), got %lld", (long long)vec->len, (long long)idx);
    if (len > UINT32_MAX) return ray_error("range", "str_vec_set: string length exceeds %lld bytes, got %lld", (long long)UINT32_MAX, (long long)len);

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
        if (pool_used < 0 || (uint64_t)pool_used > UINT32_MAX ||
            (uint64_t)len > UINT32_MAX - (uint64_t)pool_used)
            goto fail_range;
        size_t pool_cap = ray_block_data_bytes(vec->str_pool);
        size_t need = (size_t)pool_used + len;
        if (need > pool_cap) {
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
    return ray_error("range", "str_vec_set: pool offset exceeds %lld bytes", (long long)UINT32_MAX);
}

/* --------------------------------------------------------------------------
 * ray_str_vec_insert_at — insert a single string at position idx.
 *
 * Wraps (s, len) into a 1-element RAY_STR vector and delegates to
 * ray_vec_insert_vec_at, which handles pool merging via ray_vec_concat.
 * -------------------------------------------------------------------------- */

ray_t* ray_str_vec_insert_at(ray_t* vec, int64_t idx, const char* s, size_t len) {
    if (!vec || RAY_IS_ERR(vec)) return vec;
    if (vec->type != RAY_STR) return ray_error("type", "str_vec_insert_at: expects a str vector, got %s", ray_type_name(vec->type));
    if (idx < 0 || idx > vec->len) return ray_error("range", "str_vec_insert_at: index out of bounds [0,%lld], got %lld", (long long)vec->len, (long long)idx);

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
    if (vec->type != RAY_STR) return ray_error("type", "str_vec_compact: expects a str vector, got %s", ray_type_name(vec->type));
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
    memset(new_pool->aux, 0, 16);

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

    /* SYM and STR columns are no-null by design (empty "" / sym 0 is the
     * convention — there is no null distinct from empty).  Consumers
     * that need empty detection test the value (sym id 0 / str len 0) directly. */
    if (vec->type == RAY_SYM || vec->type == RAY_STR) return false;

    /* Slice: delegate to parent with adjusted index */
    if (vec->attrs & RAY_ATTR_SLICE) {
        ray_t* parent = vec->slice_parent;
        int64_t pidx = vec->slice_offset + idx;
        return ray_vec_is_null(parent, pidx);
    }

    /* Vec-level fast-path gate: HAS_NULLS clear means no null anywhere. */
    if (!vec_any_nulls(vec)) return false;

    /* Sentinels are the sole source of truth.  BOOL/U8 are non-nullable
     * (rejected at the producer) so they can never reach here with
     * HAS_NULLS set; the default arm is unreachable in practice. */
    switch (vec->type) {
        case RAY_F64:
        case RAY_F32:
        case RAY_I64: case RAY_TIMESTAMP:
        case RAY_I32: case RAY_DATE: case RAY_TIME:
        case RAY_I16:
        case RAY_STR:
        case RAY_GUID:
            return sentinel_is_null(vec, idx);
        default:
            return false;
    }
}

/* --------------------------------------------------------------------------
 * ray_vec_copy_nulls — copy null state from src to dst
 *
 * dst must have the same len as src (or at least as many elements).
 * Handles direct and slice sources.
 * -------------------------------------------------------------------------- */

ray_err_t ray_vec_copy_nulls(ray_t* dst, const ray_t* src) {
    if (!dst || !src) return RAY_ERR_TYPE;

    /* Use ray_vec_is_null which handles slices and sentinel reads
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
