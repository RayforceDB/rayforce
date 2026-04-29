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

#ifndef RAY_IDXOP_H
#define RAY_IDXOP_H

/*
 * idxop.h -- Per-vector accelerator indices.
 *
 * A vector with RAY_ATTR_HAS_INDEX set carries a child ray_t of type
 * RAY_INDEX in its nullmap[0..7] slot.  The index ray_t holds:
 *   - the kind (hash / sort / zone / bloom)
 *   - kind-specific payload (keys vec, perm vec, min/max, bloom bits)
 *   - a snapshot of the parent's original 16-byte nullmap union plus
 *     the relevant attrs bits, so detach can restore the vector to its
 *     pre-attach state byte-for-byte.
 *
 * Attach precondition: parent must not be a slice, must not already
 * carry an index, must be COW'd to rc==1 by the caller's path.
 *
 * Mutation invalidates: any in-place write to the parent vector must
 * call ray_index_drop() first — a stale index is a wrong-answer bug.
 */

#include <rayforce.h>
#include "mem/heap.h"  /* RAY_ATTR_HAS_INDEX, RAY_ATTR_NULLMAP_EXT */

/* Index kinds.  Stored in ray_index_t.kind. */
typedef enum {
    RAY_IDX_NONE  = 0,
    RAY_IDX_HASH  = 1,
    RAY_IDX_SORT  = 2,
    RAY_IDX_ZONE  = 3,
    RAY_IDX_BLOOM = 4,
} ray_idx_kind_t;

/* The payload stored inside data[] of a RAY_INDEX ray_t. */
typedef struct {
    uint8_t  kind;            /* ray_idx_kind_t */
    uint8_t  saved_attrs;     /* parent attrs & (HAS_NULLS|NULLMAP_EXT) at attach */
    int8_t   parent_type;     /* parent->type (for restore-time pointer interp) */
    uint8_t  reserved;
    int64_t  built_for_len;   /* parent->len at attach (mismatch -> stale) */

    /* Raw 16-byte snapshot of parent->nullmap union at attach time.
     * Restored verbatim on detach.  When this contains pointers
     * (ext_nullmap, str_pool, sym_dict, str_ext_null) they are owned
     * by THIS ray_t for the duration of the attachment; release-side
     * of RAY_INDEX walks these based on (parent_type, saved_attrs). */
    uint8_t  saved_nullmap[16];

    /* Kind-specific payload.  All ray_t* fields are owning refs. */
    union {
        struct {                /* RAY_IDX_HASH */
            /* Chained open-addressing.  table[mask+1] holds the head rid+1
             * for each bucket (0 = empty bucket).  chain[parent->len] holds
             * the next rid+1 in the same bucket's chain (0 = end of chain).
             * Lookup: hash key, read table[hash & mask] for head, walk chain
             * until 0 comparing parent->data[rid] for equality. */
            ray_t*   table;     /* RAY_I64 vec, capacity entries */
            ray_t*   chain;     /* RAY_I64 vec, parent->len entries */
            uint64_t mask;      /* capacity - 1 (capacity is power of two) */
            int64_t  n_keys;    /* number of non-null rows indexed */
        } hash;
        struct {                /* RAY_IDX_SORT */
            ray_t* perm;        /* RAY_I64 vec, perm[i] = row id at sorted pos i */
        } sort;
        struct {                /* RAY_IDX_ZONE */
            int64_t min_i;      /* integer min (used when type is int/date/time) */
            int64_t max_i;      /* integer max */
            double  min_f;      /* float min (used when type is f32/f64) */
            double  max_f;      /* float max */
            int64_t n_nulls;    /* number of null rows (0 if no nulls) */
        } zone;
        struct {                /* RAY_IDX_BLOOM */
            ray_t*   bits;      /* RAY_U8 vec, m/8 bytes */
            uint64_t m_mask;    /* m - 1 (m is power of two, m bits total) */
            uint32_t k;         /* number of hash functions */
            uint32_t _pad;
            int64_t  n_keys;    /* number of non-null rows added */
        } bloom;
    } u;
} ray_index_t;

/* Inline accessor — returns ray_index_t* for a RAY_INDEX block. */
static inline ray_index_t* ray_index_payload(ray_t* idx) {
    return (ray_index_t*)idx->data;
}

/* ===== Attach / Detach ===== */

/* Build an accelerator and attach.  Numeric types only for v1
 * (BOOL/U8/I16/I32/I64/F32/F64/DATE/TIME/TIMESTAMP — RAY_STR/RAY_SYM/RAY_GUID
 * deferred until the str_pool/sym_dict displacement sweep is complete).
 * On success, *vp is the (possibly new) parent vector with HAS_INDEX set.
 * On failure, *vp is unchanged and a RAY_ERROR is returned. */
ray_t* ray_index_attach_zone (ray_t** vp);
ray_t* ray_index_attach_hash (ray_t** vp);
ray_t* ray_index_attach_sort (ray_t** vp);
ray_t* ray_index_attach_bloom(ray_t** vp);

/* Drop any attached index from *vp.  No-op if none.  Restores the
 * pre-attach nullmap state byte-for-byte.  Returns *vp. */
ray_t* ray_index_drop(ray_t** vp);

/* ===== Introspection ===== */

static inline bool ray_index_has(const ray_t* v) {
    return v && !RAY_IS_ERR((ray_t*)v) &&
           (v->attrs & RAY_ATTR_HAS_INDEX) &&
           v->index != NULL;
}

/* Returns RAY_IDX_NONE if no index is attached. */
static inline ray_idx_kind_t ray_index_kind(const ray_t* v) {
    if (!ray_index_has(v)) return RAY_IDX_NONE;
    return (ray_idx_kind_t)ray_index_payload(v->index)->kind;
}

/* Returns a fresh RAY_DICT with {kind, length, ...kind-specific...}
 * or RAY_NULL_OBJ when no index is attached. */
ray_t* ray_index_info(ray_t* v);

/* ===== Internal helpers (used by retain/release/detach in heap.c
 * and by mutation paths in vec.c) ===== */

/* Release the saved-nullmap pointers carried by a RAY_INDEX ray_t.
 * Invoked from ray_release_owned_refs when the index ray_t is freed. */
void ray_index_release_saved(ray_index_t* ix);

/* Retain the saved-nullmap pointers carried by a RAY_INDEX ray_t.
 * Invoked from ray_retain_owned_refs after a copy of the index ray_t. */
void ray_index_retain_saved(ray_index_t* ix);

/* Release per-kind payload children (keys/table/perm/bits...). */
void ray_index_release_payload(ray_index_t* ix);

/* Retain per-kind payload children. */
void ray_index_retain_payload(ray_index_t* ix);

/* ===== Rayfall builtin entry points (registered from src/lang/eval.c) ===== */

ray_t* ray_idx_zone_fn (ray_t* v);  /* (.idx.zone  v) -> v with zone  attached */
ray_t* ray_idx_hash_fn (ray_t* v);  /* (.idx.hash  v) -> v with hash  attached */
ray_t* ray_idx_sort_fn (ray_t* v);  /* (.idx.sort  v) -> v with sort  attached */
ray_t* ray_idx_bloom_fn(ray_t* v);  /* (.idx.bloom v) -> v with bloom attached */
ray_t* ray_idx_drop_fn (ray_t* v);  /* (.idx.drop  v) -> v with index removed */
ray_t* ray_idx_has_fn  (ray_t* v);  /* (.idx.has?  v) -> 0b/1b */
ray_t* ray_idx_info_fn (ray_t* v);  /* (.idx.info  v) -> dict of metadata */

#endif /* RAY_IDXOP_H */
