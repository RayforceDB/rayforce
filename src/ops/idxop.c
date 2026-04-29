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

#include "idxop.h"
#include "mem/heap.h"
#include "mem/cow.h"
#include "vec/vec.h"
#include "table/table.h"
#include "table/sym.h"
#include "lang/eval.h"
#include "ops/ops.h"
#include <math.h>
#include <string.h>

/* Width of one element of a numeric vector type, or 0 if unsupported. */
static int numeric_elem_size(int8_t t) {
    switch (t) {
    case RAY_BOOL: case RAY_U8:                       return 1;
    case RAY_I16:                                     return 2;
    case RAY_I32: case RAY_DATE: case RAY_F32:        return 4;
    case RAY_I64: case RAY_TIME: case RAY_TIMESTAMP:
    case RAY_F64:                                     return 8;
    default:                                          return 0;
    }
}

/* Read row i of a numeric vector as a 64-bit hash-input word.  Mirrors the
 * canonical-equality semantics in the rest of the codebase: -0.0 / +0.0
 * collapse, NaNs route per-row (caller treats NaN as its own bucket). */
static uint64_t numeric_key_word(const uint8_t* base, int8_t type, int64_t i) {
    int es = numeric_elem_size(type);
    if (type == RAY_F32 || type == RAY_F64) {
        double v;
        if (es == 4) { float t; memcpy(&t, base + i*4, 4); v = (double)t; }
        else         {           memcpy(&v, base + i*8, 8);                }
        if (v == 0.0) v = 0.0;          /* canonicalise -0.0 -> +0.0 */
        if (v != v) {                   /* NaN: per-row bucket via row hash */
            return (uint64_t)i * 0x9E3779B97F4A7C15ULL;
        }
        uint64_t bits;
        memcpy(&bits, &v, 8);
        return bits;
    }
    int64_t k = 0;
    switch (es) {
    case 1: k = (int64_t)base[i]; break;
    case 2: { int16_t t; memcpy(&t, base + i*2, 2); k = (int64_t)t; break; }
    case 4: { int32_t t; memcpy(&t, base + i*4, 4); k = (int64_t)t; break; }
    case 8: { int64_t t; memcpy(&t, base + i*8, 8); k =          t; break; }
    }
    return (uint64_t)k;
}

/* 64-bit avalanche mix (splittable hash from Stafford / xxhash). */
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* Smallest power of two >= n, clamped to >= 1. */
static uint64_t next_pow2(uint64_t n) {
    if (n <= 1) return 1;
    uint64_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* --------------------------------------------------------------------------
 * Index ray_t allocation / destruction helpers
 *
 * The block layout: 32-byte ray_t header + ray_index_t payload in data[].
 * type = RAY_INDEX, attrs = 0 (the index itself is never sliced or aliased),
 * len = sizeof(ray_index_t) (so callers can sanity-check the payload size).
 * -------------------------------------------------------------------------- */

static ray_t* ray_index_alloc(ray_idx_kind_t kind, int8_t parent_type, int64_t parent_len) {
    ray_t* idx = ray_alloc(sizeof(ray_index_t));
    if (!idx || RAY_IS_ERR(idx)) return idx;
    idx->type  = RAY_INDEX;
    idx->attrs = 0;
    idx->len   = (int64_t)sizeof(ray_index_t);
    memset(idx->data, 0, sizeof(ray_index_t));
    ray_index_t* ix = ray_index_payload(idx);
    ix->kind         = (uint8_t)kind;
    ix->parent_type  = parent_type;
    ix->built_for_len = parent_len;
    return idx;
}

/* Reading saved-nullmap pointers: typed views into the 16-byte snapshot. */
static inline ray_t* saved_lo_ptr(ray_index_t* ix) {
    ray_t* p; memcpy(&p, &ix->saved_nullmap[0], sizeof(p)); return p;
}
static inline ray_t* saved_hi_ptr(ray_index_t* ix) {
    ray_t* p; memcpy(&p, &ix->saved_nullmap[8], sizeof(p)); return p;
}
static inline void saved_lo_clear(ray_index_t* ix) {
    memset(&ix->saved_nullmap[0], 0, 8);
}
static inline void saved_hi_clear(ray_index_t* ix) {
    memset(&ix->saved_nullmap[8], 0, 8);
}

/* --------------------------------------------------------------------------
 * Saved-nullmap retain / release
 *
 * The saved 16 bytes hold pointers iff (parent_type, saved_attrs) say so:
 *   - saved_attrs & NULLMAP_EXT  => low 8 bytes are an owning ray_t* (ext nullmap)
 *                                   *except* RAY_STR uses the same slot for
 *                                   str_ext_null (also an owning ref) — same
 *                                   semantics, same ownership.
 *   - parent_type == RAY_STR     => high 8 bytes are str_pool (owning ref)
 *   - parent_type == RAY_SYM and saved_attrs & NULLMAP_EXT
 *                                => high 8 bytes are sym_dict (owning ref)
 *
 * For all other type/attr combos the bytes are inline bitmap data, not
 * pointers, and we leave them alone.
 * -------------------------------------------------------------------------- */

void ray_index_release_saved(ray_index_t* ix) {
    if (ix->saved_attrs & RAY_ATTR_NULLMAP_EXT) {
        ray_t* lo = saved_lo_ptr(ix);
        if (lo && !RAY_IS_ERR(lo)) ray_release(lo);
        saved_lo_clear(ix);
    }
    if (ix->parent_type == RAY_STR) {
        ray_t* hi = saved_hi_ptr(ix);
        if (hi && !RAY_IS_ERR(hi)) ray_release(hi);
        saved_hi_clear(ix);
    } else if (ix->parent_type == RAY_SYM &&
               (ix->saved_attrs & RAY_ATTR_NULLMAP_EXT)) {
        /* RAY_SYM stores sym_dict at high 8 bytes only when an ext nullmap
         * is present (otherwise the inline bitmap occupies both halves and
         * sym_dict isn't materialized in the union slot). */
        ray_t* hi = saved_hi_ptr(ix);
        if (hi && !RAY_IS_ERR(hi)) ray_release(hi);
        saved_hi_clear(ix);
    }
}

void ray_index_retain_saved(ray_index_t* ix) {
    if (ix->saved_attrs & RAY_ATTR_NULLMAP_EXT) {
        ray_t* lo = saved_lo_ptr(ix);
        if (lo && !RAY_IS_ERR(lo)) ray_retain(lo);
    }
    if (ix->parent_type == RAY_STR) {
        ray_t* hi = saved_hi_ptr(ix);
        if (hi && !RAY_IS_ERR(hi)) ray_retain(hi);
    } else if (ix->parent_type == RAY_SYM &&
               (ix->saved_attrs & RAY_ATTR_NULLMAP_EXT)) {
        ray_t* hi = saved_hi_ptr(ix);
        if (hi && !RAY_IS_ERR(hi)) ray_retain(hi);
    }
}

/* --------------------------------------------------------------------------
 * Per-kind payload retain / release
 * -------------------------------------------------------------------------- */

void ray_index_release_payload(ray_index_t* ix) {
    switch ((ray_idx_kind_t)ix->kind) {
    case RAY_IDX_HASH:
        if (ix->u.hash.table && !RAY_IS_ERR(ix->u.hash.table))
            ray_release(ix->u.hash.table);
        if (ix->u.hash.chain && !RAY_IS_ERR(ix->u.hash.chain))
            ray_release(ix->u.hash.chain);
        ix->u.hash.table = ix->u.hash.chain = NULL;
        break;
    case RAY_IDX_SORT:
        if (ix->u.sort.perm && !RAY_IS_ERR(ix->u.sort.perm))
            ray_release(ix->u.sort.perm);
        ix->u.sort.perm = NULL;
        break;
    case RAY_IDX_BLOOM:
        if (ix->u.bloom.bits && !RAY_IS_ERR(ix->u.bloom.bits))
            ray_release(ix->u.bloom.bits);
        ix->u.bloom.bits = NULL;
        break;
    case RAY_IDX_ZONE:
    case RAY_IDX_NONE:
        break;
    }
}

void ray_index_retain_payload(ray_index_t* ix) {
    switch ((ray_idx_kind_t)ix->kind) {
    case RAY_IDX_HASH:
        if (ix->u.hash.table && !RAY_IS_ERR(ix->u.hash.table))
            ray_retain(ix->u.hash.table);
        if (ix->u.hash.chain && !RAY_IS_ERR(ix->u.hash.chain))
            ray_retain(ix->u.hash.chain);
        break;
    case RAY_IDX_SORT:
        if (ix->u.sort.perm && !RAY_IS_ERR(ix->u.sort.perm))
            ray_retain(ix->u.sort.perm);
        break;
    case RAY_IDX_BLOOM:
        if (ix->u.bloom.bits && !RAY_IS_ERR(ix->u.bloom.bits))
            ray_retain(ix->u.bloom.bits);
        break;
    case RAY_IDX_ZONE:
    case RAY_IDX_NONE:
        break;
    }
}

/* --------------------------------------------------------------------------
 * Zone scan -- compute min/max + null count
 *
 * Reads the parent vector before the nullmap is displaced.  Integer paths
 * cover BOOL/U8/I16/I32/I64/DATE/TIME/TIMESTAMP (all stored in int slots);
 * float paths cover F32/F64.  RAY_SYM/STR/GUID return RAY_ERR_NYI for now;
 * those types will get string-aware min/max in the P4 zone work.
 * -------------------------------------------------------------------------- */

static ray_err_t zone_scan_int(ray_t* v, ray_index_t* ix, int elem_size) {
    int64_t n = v->len;
    int64_t mn = INT64_MAX, mx = INT64_MIN;
    int64_t nn = 0;
    bool any_value = false;
    const uint8_t* base = (const uint8_t*)ray_data(v);

    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(v, i)) { nn++; continue; }
        int64_t val = 0;
        switch (elem_size) {
        case 1: val = (int64_t)base[i]; break;
        case 2: { int16_t t; memcpy(&t, base + i*2, 2); val = (int64_t)t; break; }
        case 4: { int32_t t; memcpy(&t, base + i*4, 4); val = (int64_t)t; break; }
        case 8: { int64_t t; memcpy(&t, base + i*8, 8); val = t;          break; }
        default: return RAY_ERR_TYPE;
        }
        if (val < mn) mn = val;
        if (val > mx) mx = val;
        any_value = true;
    }
    if (!any_value) { mn = 0; mx = 0; }
    ix->u.zone.min_i  = mn;
    ix->u.zone.max_i  = mx;
    ix->u.zone.n_nulls = nn;
    return RAY_OK;
}

static ray_err_t zone_scan_float(ray_t* v, ray_index_t* ix, int elem_size) {
    int64_t n = v->len;
    double mn = INFINITY, mx = -INFINITY;
    int64_t nn = 0;
    bool any_value = false;
    const uint8_t* base = (const uint8_t*)ray_data(v);

    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(v, i)) { nn++; continue; }
        double val = 0.0;
        if (elem_size == 4) {
            float t; memcpy(&t, base + i*4, 4); val = (double)t;
        } else {
            memcpy(&val, base + i*8, 8);
        }
        if (isnan(val)) continue;  /* NaNs don't participate in min/max */
        if (val < mn) mn = val;
        if (val > mx) mx = val;
        any_value = true;
    }
    if (!any_value) { mn = 0.0; mx = 0.0; }
    ix->u.zone.min_f  = mn;
    ix->u.zone.max_f  = mx;
    ix->u.zone.n_nulls = nn;
    return RAY_OK;
}

static ray_err_t zone_scan(ray_t* v, ray_index_t* ix) {
    switch (v->type) {
    case RAY_BOOL:
    case RAY_U8:        return zone_scan_int(v, ix, 1);
    case RAY_I16:       return zone_scan_int(v, ix, 2);
    case RAY_I32:
    case RAY_DATE:      return zone_scan_int(v, ix, 4);
    case RAY_I64:
    case RAY_TIME:
    case RAY_TIMESTAMP: return zone_scan_int(v, ix, 8);
    case RAY_F32:       return zone_scan_float(v, ix, 4);
    case RAY_F64:       return zone_scan_float(v, ix, 8);
    default:            return RAY_ERR_NYI;
    }
}

/* --------------------------------------------------------------------------
 * Attach
 *
 * The 16-byte snapshot must be taken AFTER the scan (so the scan reads the
 * parent's normal nullmap) but BEFORE we overwrite parent->nullmap with the
 * index pointer.  Ownership transfer: pointers in the snapshot (ext_nullmap,
 * str_pool, sym_dict) move from parent to ix.  We do NOT retain them here —
 * the existing refs simply move.  Symmetrically, when we install the index
 * pointer in parent->nullmap, we transfer that single ref to the parent
 * (no extra retain).
 * -------------------------------------------------------------------------- */

static ray_t* attach_finalize(ray_t* parent, ray_t* idx) {
    ray_index_t* ix = ray_index_payload(idx);
    /* Snapshot the parent's 16 raw bytes verbatim. */
    memcpy(ix->saved_nullmap, parent->nullmap, 16);
    ix->saved_attrs = parent->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_NULLMAP_EXT);

    /* Install the index pointer — overwrites bytes 0-7 with the index ptr.
     * Bytes 8-15 carry link_target when HAS_LINK is set; preserve them.
     * Otherwise zero _idx_pad as a tidy default. */
    parent->index    = idx;
    if (!(parent->attrs & RAY_ATTR_HAS_LINK)) parent->_idx_pad = NULL;
    parent->attrs   |= RAY_ATTR_HAS_INDEX;
    /* Clear NULLMAP_EXT on the parent: vec->ext_nullmap is now the index
     * pointer, not a U8 nullmap vec, so naive readers that gate on
     * NULLMAP_EXT and dereference ext_nullmap would read garbage.  The
     * displaced ext-nullmap pointer is preserved inside ix->saved_nullmap[0..7]
     * and accessed via the HAS_INDEX-aware helpers in vec.c / morsel.c.
     *
     * IMPORTANT: HAS_NULLS is *preserved* on the parent so the many call
     * sites that use it as a cheap "do I need null logic at all?" gate
     * continue to give correct answers.  The actual null bits are read
     * via ray_vec_is_null / ray_morsel_next, both of which check
     * HAS_INDEX first and route through the saved snapshot. */
    parent->attrs   &= (uint8_t)~RAY_ATTR_NULLMAP_EXT;
    return parent;
}

/* Validate + COW + drop existing index.  Returns the (possibly new) parent
 * pointer and updates *vp.  On error returns a RAY_ERROR; caller must
 * propagate without further modifying *vp. */
static ray_t* prepare_attach(ray_t** vp, const char* what) {
    if (!vp || !*vp || RAY_IS_ERR(*vp))
        return ray_error("type", "%s: null/error vector", what);
    ray_t* v = *vp;
    if (!ray_is_vec(v))
        return ray_error("type", "%s: index can only attach to a vector", what);
    if (v->attrs & RAY_ATTR_SLICE)
        return ray_error("type", "%s: cannot index a slice; materialize first", what);
    if (v->attrs & RAY_ATTR_HAS_INDEX) {
        ray_index_drop(&v);
        if (RAY_IS_ERR(v)) return v;
        *vp = v;
    }
    v = ray_cow(v);
    if (!v || RAY_IS_ERR(v)) return v;
    *vp = v;
    if (numeric_elem_size(v->type) == 0) {
        return ray_error("nyi", "%s: only numeric vectors supported in v1 (got type %d)",
                         what, (int)v->type);
    }
    return v;
}

ray_t* ray_index_attach_zone(ray_t** vp) {
    ray_t* v = prepare_attach(vp, "zone");
    if (RAY_IS_ERR(v)) return v;

    ray_t* idx = ray_index_alloc(RAY_IDX_ZONE, v->type, v->len);
    if (!idx || RAY_IS_ERR(idx)) return idx;

    ray_err_t err = zone_scan(v, ray_index_payload(idx));
    if (err != RAY_OK) {
        ray_release(idx);
        return ray_error(ray_err_code_str(err), "zone scan failed for type %d", (int)v->type);
    }
    return attach_finalize(v, idx);
}

/* --------------------------------------------------------------------------
 * Hash index — chained open addressing
 *
 * table[capacity]: each slot is rid+1 of the most recent row that hashed
 *   into the bucket (0 = empty bucket).
 * chain[parent->len]: each slot is rid+1 of the next-older row in the same
 *   bucket's chain (0 = end of chain).
 *
 * Lookup `k`: rid = table[hash(k) & mask] - 1; while rid >= 0 compare
 * parent->data[rid] == k, on miss step rid = chain[rid] - 1.
 * -------------------------------------------------------------------------- */

ray_t* ray_index_attach_hash(ray_t** vp) {
    ray_t* v = prepare_attach(vp, "hash");
    if (RAY_IS_ERR(v)) return v;

    int64_t n = v->len;
    /* Capacity: at least 8, at most 2*n.  Power of two for cheap masking. */
    uint64_t cap = next_pow2((uint64_t)(n < 4 ? 8 : 2 * n));
    if (cap < 8) cap = 8;

    ray_t* table = ray_vec_new(RAY_I64, (int64_t)cap);
    if (!table || RAY_IS_ERR(table)) return table ? table : ray_error("oom", NULL);
    table->len = (int64_t)cap;
    memset(ray_data(table), 0, (size_t)cap * sizeof(int64_t));

    ray_t* chain = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (!chain || RAY_IS_ERR(chain)) {
        ray_release(table);
        return chain ? chain : ray_error("oom", NULL);
    }
    chain->len = n;
    if (n > 0) memset(ray_data(chain), 0, (size_t)n * sizeof(int64_t));

    int64_t* tbl = (int64_t*)ray_data(table);
    int64_t* chn = (int64_t*)ray_data(chain);
    const uint8_t* base = (const uint8_t*)ray_data(v);
    int64_t n_keys = 0;
    uint64_t mask = cap - 1;

    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(v, i)) continue;
        uint64_t h = mix64(numeric_key_word(base, v->type, i));
        uint64_t slot = h & mask;
        chn[i] = tbl[slot];     /* link previous head into chain */
        tbl[slot] = i + 1;      /* this row becomes new head */
        n_keys++;
    }

    ray_t* idx = ray_index_alloc(RAY_IDX_HASH, v->type, n);
    if (!idx || RAY_IS_ERR(idx)) {
        ray_release(table);
        ray_release(chain);
        return idx ? idx : ray_error("oom", NULL);
    }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.hash.table  = table;
    ix->u.hash.chain  = chain;
    ix->u.hash.mask   = mask;
    ix->u.hash.n_keys = n_keys;

    return attach_finalize(v, idx);
}

/* --------------------------------------------------------------------------
 * Sort index — ascending permutation of row ids
 *
 * Delegates to the existing parallel sort builder.  Result is an I64 vec of
 * length parent->len with PostgreSQL null-handling (nulls last for asc).
 * -------------------------------------------------------------------------- */

ray_t* ray_index_attach_sort(ray_t** vp) {
    ray_t* v = prepare_attach(vp, "sort");
    if (RAY_IS_ERR(v)) return v;

    ray_t* col = v;
    ray_t* perm = ray_sort_indices(&col, NULL, NULL, 1, v->len);
    if (!perm || RAY_IS_ERR(perm)) return perm ? perm : ray_error("oom", NULL);

    ray_t* idx = ray_index_alloc(RAY_IDX_SORT, v->type, v->len);
    if (!idx || RAY_IS_ERR(idx)) {
        ray_release(perm);
        return idx ? idx : ray_error("oom", NULL);
    }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.sort.perm = perm;

    return attach_finalize(v, idx);
}

/* --------------------------------------------------------------------------
 * Bloom filter — m bits, k=3 hashes via double-hashing
 *
 * Layout: m is rounded to the next power of two >= max(64, 8*n_non_null).
 * Each row sets bits at positions (h1 + i*h2) mod m for i in [0..k).
 * h1, h2 are derived from a single 64-bit mix of the key word.
 * -------------------------------------------------------------------------- */

ray_t* ray_index_attach_bloom(ray_t** vp) {
    ray_t* v = prepare_attach(vp, "bloom");
    if (RAY_IS_ERR(v)) return v;

    int64_t n = v->len;
    /* Count non-null rows for sizing. */
    int64_t n_set = 0;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_vec_is_null(v, i)) n_set++;
    }
    uint64_t target_bits = (uint64_t)(n_set < 8 ? 64 : 8 * n_set);
    uint64_t m = next_pow2(target_bits);
    if (m < 64) m = 64;
    uint64_t mbytes = m / 8;
    uint32_t k = 3;

    ray_t* bits = ray_vec_new(RAY_U8, (int64_t)mbytes);
    if (!bits || RAY_IS_ERR(bits)) return bits ? bits : ray_error("oom", NULL);
    bits->len = (int64_t)mbytes;
    memset(ray_data(bits), 0, (size_t)mbytes);

    uint8_t* bbuf = (uint8_t*)ray_data(bits);
    uint64_t mask = m - 1;
    const uint8_t* base = (const uint8_t*)ray_data(v);

    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(v, i)) continue;
        uint64_t h = mix64(numeric_key_word(base, v->type, i));
        uint64_t h1 = h;
        uint64_t h2 = mix64(h ^ 0xc6a4a7935bd1e995ULL) | 1ULL;  /* ensure odd */
        for (uint32_t kk = 0; kk < k; kk++) {
            uint64_t pos = (h1 + (uint64_t)kk * h2) & mask;
            bbuf[pos >> 3] |= (uint8_t)(1u << (pos & 7));
        }
    }

    ray_t* idx = ray_index_alloc(RAY_IDX_BLOOM, v->type, n);
    if (!idx || RAY_IS_ERR(idx)) {
        ray_release(bits);
        return idx ? idx : ray_error("oom", NULL);
    }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.bloom.bits   = bits;
    ix->u.bloom.m_mask = mask;
    ix->u.bloom.k      = k;
    ix->u.bloom.n_keys = n_set;

    return attach_finalize(v, idx);
}

/* --------------------------------------------------------------------------
 * Detach (drop)
 *
 * Restore the parent's 16-byte nullmap union from the saved snapshot, then
 * release the index ray_t.  The release path of RAY_INDEX would otherwise
 * also try to release the saved-nullmap pointers, so we clear the saved
 * snapshot and saved_attrs first to neutralize that — ownership is moving
 * back to the parent.
 * -------------------------------------------------------------------------- */

ray_t* ray_index_drop(ray_t** vp) {
    if (!vp || !*vp || RAY_IS_ERR(*vp)) return *vp;
    ray_t* v = *vp;
    if (!(v->attrs & RAY_ATTR_HAS_INDEX)) return v;

    /* Detach mutates the parent in place; require sole ownership. */
    v = ray_cow(v);
    if (!v || RAY_IS_ERR(v)) { *vp = v; return v; }
    *vp = v;

    /* After ray_cow, *vp may be a freshly copied block.  In ray_alloc_copy,
     * the index pointer was retained by ray_retain_owned_refs (via the
     * RAY_ATTR_HAS_INDEX branch we add in heap.c), so v->index here is
     * still the live, owned index ray_t. */
    ray_t* idx = v->index;
    ray_index_t* ix = ray_index_payload(idx);

    /* Shared-index case: another vec may share this RAY_INDEX block via
     * ray_alloc_copy (rc>1).  Don't clobber the snapshot in that case —
     * the other holder still reads it.  Copy our own retained refs to
     * the saved-pointer slots so the bytes we move into v->nullmap are
     * owned by v.  See vec_drop_index_inplace for the same pattern. */
    uint8_t saved = ix->saved_attrs;
    bool shared = ray_atomic_load(&idx->rc) > 1;
    if (shared) {
        ray_index_retain_saved(ix);
    }
    memcpy(v->nullmap, ix->saved_nullmap, 16);
    if (!shared) {
        memset(ix->saved_nullmap, 0, 16);
        ix->saved_attrs = 0;
    }

    /* Restore parent attrs.  HAS_NULLS was preserved through the attachment
     * so we don't need to OR it back in; only NULLMAP_EXT (which we cleared
     * at attach time) needs to be reinstated from saved_attrs. */
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_INDEX;
    if (saved & RAY_ATTR_NULLMAP_EXT) v->attrs |= RAY_ATTR_NULLMAP_EXT;

    /* Release the index.  Per-kind children are released by the RAY_INDEX
     * branch of ray_release_owned_refs (added in heap.c). */
    ray_release(idx);
    return v;
}

/* --------------------------------------------------------------------------
 * Info
 * -------------------------------------------------------------------------- */

static const char* kind_name(ray_idx_kind_t k) {
    switch (k) {
    case RAY_IDX_HASH:  return "hash";
    case RAY_IDX_SORT:  return "sort";
    case RAY_IDX_ZONE:  return "zone";
    case RAY_IDX_BLOOM: return "bloom";
    default:            return "none";
    }
}

static ray_t* dict_append_sym_i64(ray_t** keys, ray_t** vals, const char* k, int64_t n) {
    int64_t kid = ray_sym_intern(k, strlen(k));
    *keys = ray_vec_append(*keys, &kid);
    if (RAY_IS_ERR(*keys)) return *keys;
    ray_t* nv = ray_i64(n);
    *vals = ray_list_append(*vals, nv);
    ray_release(nv);
    return *vals;
}

static ray_t* dict_append_sym_sym(ray_t** keys, ray_t** vals, const char* k, const char* s) {
    int64_t kid = ray_sym_intern(k, strlen(k));
    *keys = ray_vec_append(*keys, &kid);
    if (RAY_IS_ERR(*keys)) return *keys;
    int64_t sid = ray_sym_intern(s, strlen(s));
    ray_t* sv = ray_sym(sid);
    *vals = ray_list_append(*vals, sv);
    ray_release(sv);
    return *vals;
}

ray_t* ray_index_info(ray_t* v) {
    if (!ray_index_has(v)) return RAY_NULL_OBJ;
    ray_index_t* ix = ray_index_payload(v->index);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 8);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(8);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    ray_t* r;
    r = dict_append_sym_sym(&keys, &vals, "kind", kind_name((ray_idx_kind_t)ix->kind));
    if (RAY_IS_ERR(r)) goto fail;
    r = dict_append_sym_i64(&keys, &vals, "length", ix->built_for_len);
    if (RAY_IS_ERR(r)) goto fail;
    r = dict_append_sym_i64(&keys, &vals, "parent_type", (int64_t)ix->parent_type);
    if (RAY_IS_ERR(r)) goto fail;
    r = dict_append_sym_i64(&keys, &vals, "saved_attrs", (int64_t)ix->saved_attrs);
    if (RAY_IS_ERR(r)) goto fail;

    switch ((ray_idx_kind_t)ix->kind) {
    case RAY_IDX_ZONE:
        if (ix->parent_type == RAY_F32 || ix->parent_type == RAY_F64) {
            int64_t kmin = ray_sym_intern("min", 3);
            keys = ray_vec_append(keys, &kmin);
            ray_t* mn = ray_f64(ix->u.zone.min_f);
            vals = ray_list_append(vals, mn); ray_release(mn);
            int64_t kmax = ray_sym_intern("max", 3);
            keys = ray_vec_append(keys, &kmax);
            ray_t* mx = ray_f64(ix->u.zone.max_f);
            vals = ray_list_append(vals, mx); ray_release(mx);
        } else {
            r = dict_append_sym_i64(&keys, &vals, "min", ix->u.zone.min_i);
            if (RAY_IS_ERR(r)) goto fail;
            r = dict_append_sym_i64(&keys, &vals, "max", ix->u.zone.max_i);
            if (RAY_IS_ERR(r)) goto fail;
        }
        r = dict_append_sym_i64(&keys, &vals, "n_nulls", ix->u.zone.n_nulls);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_HASH:
        r = dict_append_sym_i64(&keys, &vals, "capacity", (int64_t)(ix->u.hash.mask + 1));
        if (RAY_IS_ERR(r)) goto fail;
        r = dict_append_sym_i64(&keys, &vals, "n_keys",   ix->u.hash.n_keys);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_SORT:
        r = dict_append_sym_i64(&keys, &vals, "perm_len",
                                ix->u.sort.perm ? ix->u.sort.perm->len : 0);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_BLOOM:
        r = dict_append_sym_i64(&keys, &vals, "m_bits", (int64_t)(ix->u.bloom.m_mask + 1));
        if (RAY_IS_ERR(r)) goto fail;
        r = dict_append_sym_i64(&keys, &vals, "k", (int64_t)ix->u.bloom.k);
        if (RAY_IS_ERR(r)) goto fail;
        r = dict_append_sym_i64(&keys, &vals, "n_keys", ix->u.bloom.n_keys);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_NONE:
        break;
    }

    return ray_dict_new(keys, vals);

fail:
    if (!RAY_IS_ERR(keys)) ray_release(keys);
    if (!RAY_IS_ERR(vals)) ray_release(vals);
    return r;
}

/* --------------------------------------------------------------------------
 * Rayfall builtins (registered from src/lang/eval.c)
 * -------------------------------------------------------------------------- */

/* Common entry shape: take a borrowed ref, return an owning ref of the
 * (possibly COW-copied) parent.  See heap.c:ray_release on rc transfer. */
static ray_t* attach_via(ray_t* v, ray_t* (*fn)(ray_t**)) {
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* w = v;
    ray_retain(w);
    ray_t* r = fn(&w);
    if (RAY_IS_ERR(r)) { ray_release(w); return r; }
    return w;
}

ray_t* ray_idx_zone_fn (ray_t* v) { return attach_via(v, ray_index_attach_zone);  }
ray_t* ray_idx_hash_fn (ray_t* v) { return attach_via(v, ray_index_attach_hash);  }
ray_t* ray_idx_sort_fn (ray_t* v) { return attach_via(v, ray_index_attach_sort);  }
ray_t* ray_idx_bloom_fn(ray_t* v) { return attach_via(v, ray_index_attach_bloom); }

ray_t* ray_idx_drop_fn(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* w = v;
    ray_retain(w);
    ray_t* r = ray_index_drop(&w);
    if (RAY_IS_ERR(r)) { ray_release(w); return r; }
    return w;
}

ray_t* ray_idx_has_fn(ray_t* v) {
    return ray_bool(ray_index_has(v) ? 1 : 0);
}

ray_t* ray_idx_info_fn(ray_t* v) {
    return ray_index_info(v);
}
