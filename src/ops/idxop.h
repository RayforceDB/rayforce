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
 * RAY_INDEX in its aux[0..7] slot.  The index ray_t holds:
 *   - the kind (hash / sort / zone / bloom)
 *   - kind-specific payload (keys vec, perm vec, min/max, bloom bits)
 *   - a snapshot of the parent's original 16-byte aux union plus
 *     the relevant attrs bits, so detach can restore the vector to its
 *     pre-attach state byte-for-byte.
 *
 * Attach precondition: parent must not be a slice, must not already
 * carry an index, must be COW'd to rc==1 by the caller's path.
 *
 * Mutation invalidates: any in-place write to the parent vector must
 * call ray_index_drop() first AND clear RAY_ATTR_SORTED — a stale index
 * or a stale sorted marker is a wrong-answer bug (e.g. an asof-join that
 * trusts a false ordering claim).  Operators that build a fresh result
 * vector get this for free (fresh vectors start at attrs==0, no index).
 */

#include <rayforce.h>
#include "mem/heap.h"  /* RAY_ATTR_HAS_INDEX */

/* Index kinds.  Stored in ray_index_t.kind. */
typedef enum {
    RAY_IDX_NONE       = 0,
    RAY_IDX_HASH       = 1,
    RAY_IDX_SORT       = 2,
    RAY_IDX_ZONE       = 3,
    RAY_IDX_BLOOM      = 4,
    /* Per-chunk min/max + null bit, one entry per (1 << chunk_log2) rows.
     * The whole-column zone is derivable as
     *   min(chunk_mins)/max(chunk_maxs) over the entries, so this
     *   subsumes RAY_IDX_ZONE wherever it's used in the reduce path.
     * Built at column ingest (csv.read); read by the min/max reduce
     * and by the predicate planner to skip chunks whose [min,max]
     * provably excludes/includes the constant.  See chunk_zone arm
     * of ray_index_t.u below. */
    RAY_IDX_CHUNK_ZONE = 5,
    RAY_IDX_PART       = 6,
    /* Per-column string dictionary: an int32 code per row + the distinct
     * string values (code -> string).  Lets group-by / distinct run on the
     * cheap integer-code path instead of hashing 16-byte ray_str_t descriptors
     * and chasing the string pool.  Built at column ingest / splayed save and
     * persisted inline like the chunk-zone index; the only accelerator index
     * permitted on RAY_STR (it stores codes alongside the descriptors, leaving
     * the column's own representation untouched). */
    RAY_IDX_DICT       = 7,
} ray_idx_kind_t;

/* Marker bits stored in ray_index_t.markers (block-resident attributes
 * that have no dedicated attrs bit).  sorted lives in attrs (RAY_ATTR_SORTED),
 * not here. */
#define RAY_MARK_UNIQUE  0x01
/* Set when this index and its child vecs are passengers in the parent column's
 * file mapping (restored in-place from the on-disk inline index region).  The
 * parent's single munmap frees them; ray_release_owned_refs must NOT free a
 * passenger index by pointer.  Clear = heap-resident index (freed normally),
 * including a runtime-built index attached to an mmap'd column. */
#define RAY_MARK_MMAP    0x02

/* The payload stored inside data[] of a RAY_INDEX ray_t. */
typedef struct {
    uint8_t  kind;            /* ray_idx_kind_t */
    uint8_t  saved_attrs;     /* parent attrs & HAS_NULLS at attach */
    int8_t   parent_type;     /* parent->type (recorded for diagnostics) */
    uint8_t  markers;         /* RAY_MARK_* bits (block-resident attr flags) */
    int64_t  built_for_len;   /* parent->len at attach (mismatch -> stale) */

    /* Raw 16-byte snapshot of parent->aux union at attach time,
     * restored verbatim on detach.  For the numeric vector types that
     * may attach an index (see prepare_attach) this snapshot holds no
     * owned ray_t* refs: bytes 0-7 are unused and bytes 8-15 carry the
     * link_target int64 when HAS_LINK is set. */
    uint8_t  saved_aux[16];

    /* Kind-specific payload.  All ray_t* fields are owning refs. */
    union {
        struct {                /* RAY_IDX_HASH — CSR grouped layout */
            /* Open-addressing bucket table over DISTINCT keys plus a CSR
             * (offsets + contiguous row lists) grouping of the rows:
             *   table[mask+1]  slot -> group id + 1 (0 = empty bucket)
             *   gkeys[n_groups]     key value per group (i64-canonical;
             *                       SYM stores the column-domain id)
             *   offs[n_groups+1]    rows[] slice bounds per group
             *   rows[n_keys]        row ids, ASCENDING within each group
             * Lookup `k`: slot-walk table comparing gkeys[gid] == k; a hit
             * yields the group's contiguous rows[offs[gid]..offs[gid+1])
             * slice — group size is O(1), first occurrence is rows[offs[gid]],
             * and the ascending order is what rowsel builders and the asof
             * binary search consume directly. */
            ray_t*   table;     /* RAY_I64 vec, capacity entries */
            ray_t*   gkeys;     /* RAY_I64 vec, n_groups entries */
            ray_t*   offs;      /* RAY_I64 vec, n_groups+1 entries */
            ray_t*   rows;      /* RAY_I64 vec, n_keys entries */
            uint64_t mask;      /* capacity - 1 (capacity is power of two) */
            int64_t  n_keys;    /* number of non-null rows indexed */
            int64_t  n_groups;  /* number of distinct keys */
            int64_t  order_sym; /* optional column symbol ordered within each group */
        } hash;
        struct {                /* RAY_IDX_SORT */
            ray_t* perm;        /* RAY_I64 vec, perm[i] = row id at sorted pos i */
        } sort;
        struct {                /* RAY_IDX_ZONE */
            /* A column is integer XOR float, so the int and float extrema share
             * storage (keeps ray_index_t a 32-byte multiple for mmap layout). */
            union { int64_t min_i; double min_f; }; /* min (int: date/time too) */
            union { int64_t max_i; double max_f; }; /* max */
            int64_t n_nulls;    /* number of null rows (0 if no nulls) */
        } zone;
        struct {                /* RAY_IDX_BLOOM */
            ray_t*   bits;      /* RAY_U8 vec, m/8 bytes */
            uint64_t m_mask;    /* m - 1 (m is power of two, m bits total) */
            uint32_t k;         /* number of hash functions */
            uint32_t _pad;
            int64_t  n_keys;    /* number of non-null rows added */
        } bloom;
        struct {                /* RAY_IDX_CHUNK_ZONE */
            /* mins / maxs hold n_chunks entries.  For integer / temporal
             * column types they are RAY_I64 vecs storing the per-chunk
             * extrema as int64; for RAY_F64 columns they are RAY_F64
             * vecs.  is_f64 disambiguates at read time. */
            ray_t*   mins;
            ray_t*   maxs;
            ray_t*   null_bits;   /* RAY_U8 vec, packed: bit i = chunk i has any null */
            uint32_t n_chunks;
            uint8_t  chunk_log2;  /* chunk size = 1 << chunk_log2 (default 16 → 64 K rows) */
            uint8_t  is_f64;
            uint8_t  _pad[2];
        } chunk_zone;
        struct {                /* RAY_IDX_PART */
            ray_t*  keys;       /* distinct partition values, in ascending block order */
            ray_t*  starts;     /* RAY_I64, row offset where each part begins */
            ray_t*  lens;       /* RAY_I64, row count of each part */
            int64_t n_parts;
            int64_t order_sym;  /* optional column symbol ordered within each part */
        } part;
        struct {                /* RAY_IDX_DICT */
            /* Both children are RAY_I32 (numeric — the inline persistence stores
             * them verbatim, no nested str_pool).  The distinct STRING values
             * are NOT duplicated: first_occ[c] is the parent-column row index of
             * code c's first occurrence, so code -> string resolves through the
             * parent column itself (ray_str_vec_get(col, first_occ[c])). */
            ray_t*   codes;     /* RAY_I32 vec, parent->len entries: code per row */
            ray_t*   first_occ; /* RAY_I32 vec, n_distinct entries: first-occ row  */
            int64_t  n_distinct;
        } dict;
    } u;
} ray_index_t;

/* On-disk index persistence stores the RAY_INDEX object and its child vecs as
 * contiguous 32-byte-aligned ray_t blocks mmap'd in place (no serialization).
 * For the trailing child blocks to stay 32-aligned, the index payload must be a
 * 32-byte multiple — enforce it so the layout invariant can't silently break. */
_Static_assert(sizeof(ray_index_t) % 32 == 0,
               "ray_index_t must be a 32-byte multiple for in-place mmap layout");

/* Inline accessor — returns ray_index_t* for a RAY_INDEX block. */
static inline ray_index_t* ray_index_payload(ray_t* idx) {
    return (ray_index_t*)idx->data;
}

/* ── Routing observability: per-site consult/hit counters ──
 * Diagnostic, unsynchronized (same caveat as ray_expr_bail_counts). */
typedef enum {
    IDX_SITE_FILTER_ZONE = 0, IDX_SITE_FILTER_BLOOM, IDX_SITE_FILTER_HASH,
    IDX_SITE_FILTER_PART, IDX_SITE_FILTER_RANGE, IDX_SITE_IN, IDX_SITE_FIND,
    IDX_SITE_SORT, IDX_SITE_DISTINCT, IDX_SITE_ASOF,
    IDX_SITE_FILTER_EQRANGE, IDX_SITE_GROUP_SLICE, IDX_SITE__N
} idx_site_t;
extern uint64_t ray_idx_consults[IDX_SITE__N];
extern uint64_t ray_idx_hits[IDX_SITE__N];
void ray_idx_stats_init(void);   /* atexit dump when RAY_IDX_STATS set */

/* ===== Attach / Detach ===== */

/* Build an accelerator and attach.  Numeric types are supported by every
 * index kind below; hash and part also support SYM domain ids.
 * On success, *vp is the (possibly new) parent vector with HAS_INDEX set.
 * On failure, *vp is unchanged and a RAY_ERROR is returned. */
ray_t* ray_index_attach_zone (ray_t** vp);
ray_t* ray_index_attach_hash (ray_t** vp);
ray_t* ray_index_attach_sort (ray_t** vp);
ray_t* ray_index_attach_bloom(ray_t** vp);
ray_t* ray_index_attach_part (ray_t** vp);
/* Build per-chunk min/max + null bit at chunk_size = 1 << chunk_log2.
 * Passing 0 picks the default (16 → 64 K rows / chunk).  Only valid on
 * numeric and temporal vectors; SYM/STR/GUID return RAY_ERR_NYI. */
ray_t* ray_index_attach_chunk_zone(ray_t** vp, uint8_t chunk_log2);

/* Build a chunk-zone index WITHOUT attaching it — returns a standalone
 * RAY_INDEX object (caller releases).  Used by the splayed-store builder to
 * compute an index for persistence without COWing a shared column. */
ray_t* ray_index_chunk_zone_compute(ray_t* v, uint8_t chunk_log2);

/* Build a RAY_IDX_DICT (codes + distinct values) for STR vector `v` WITHOUT
 * attaching it — standalone RAY_INDEX object (caller releases / attaches).
 * Returns RAY_ERR_NYI for non-STR.  Used at splayed save to persist the dict
 * and by ray_index_attach_dict for the runtime path. */
ray_t* ray_index_dict_compute(ray_t* v);
ray_t* ray_index_attach_dict(ray_t** vp);

/* Attach an already-built standalone RAY_INDEX object (zero-copy on rc=1). */
ray_t* ray_index_attach_built(ray_t** vp, ray_t* idx);

/* ── Inline on-disk index region (zero-copy mmap) ──
 * ray_index_inline_size: bytes the region occupies (32-aligned ray_t blocks).
 * ray_index_inline_write: serialize `ix` into dst (child ptrs → region offsets).
 * ray_index_inline_map: patch an mmap'd region's child offsets → absolute ptrs
 *   in place and return the RAY_INDEX object (flagged RAY_MARK_MMAP). */
int64_t ray_index_inline_size(const ray_index_t* ix);
void    ray_index_inline_write(uint8_t* dst, const ray_index_t* ix);
ray_t*  ray_index_inline_map(uint8_t* region);

/* Drop any attached index from *vp.  No-op if none.  Restores the
 * pre-attach aux state byte-for-byte.  Returns *vp. */
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

/* --- Semantic attribute markers (see mem/heap.h: RAY_ATTR_SORTED) --- */

/* True iff v is a vector flagged sorted (non-descending). */
static inline bool ray_attr_is_sorted(const ray_t* v) {
    return v && !RAY_IS_ERR((ray_t*)v) && ray_is_vec(v)
        && (v->attrs & RAY_ATTR_SORTED);
}

/* Returns a fresh RAY_DICT with {kind, length, ...kind-specific...}
 * or RAY_NULL_OBJ when no index is attached. */
ray_t* ray_index_info(ray_t* v);

/* ===== Zone-index all/none classification =====
 *
 * O(1) pre-check: given the zone index on `col` and a comparison
 * (col cmp_op key), decide whether the predicate is definitely true for
 * ALL rows, definitely false for ALL rows (NONE), or UNKNOWN (need scan).
 *
 * cmp_op is the OP_EQ..OP_GE opcode.  key_i is used for integer/temporal
 * column families; key_f for float families.  NaN key_f → UNKNOWN.
 * Returns UNKNOWN when the column is not eligible (no zone index, stale,
 * null-bearing, unsupported type). */
typedef enum { RAY_ZONE_UNKNOWN = 0, RAY_ZONE_ALL = 1, RAY_ZONE_NONE = 2 } ray_zone_class_t;
ray_zone_class_t ray_index_zone_class(ray_t* col, uint16_t cmp_op,
                                      int64_t key_i, double key_f);

/* Build an all-NONE rowsel of `n` rows.  Wraps rowsel_from_sorted_ids(n, NULL, 0).
 * Returns NULL on OOM; returns a valid zero-match rowsel on success. */
ray_t* ray_index_empty_rowsel(int64_t n);

/* ===== Bloom-index definite-absent probe =====
 *
 * Returns true ONLY when the bloom filter PROVES the key is absent
 * (all k probe bits are clear).  Returns false when the key may be
 * present OR when the column is not eligible — caller proceeds to
 * other consults / the scan either way.
 *
 * Integer-family columns only in v1 (mirrors the hash path's
 * conservatism on float equality: F32/F64 NaN/-0 semantics).
 * No range validation is performed: an out-of-range key hashes to
 * some bit positions; if they are set we fall through — the absent
 * proof is still sound (no false absent is possible). */
bool ray_index_bloom_absent(ray_t* col, int64_t key);

/* ===== Hash-index point-lookup probe =====
 *
 * Build a ray_rowsel directly from a hash probe on `col`'s
 * RAY_IDX_HASH for rows where the payload equals `key`.  Bypasses
 * the intermediate BOOL pred vec entirely — touches O(matches)
 * memory instead of O(rows), which is the whole reason to ship
 * this fast path.
 *
 * Returns:
 *   - A fresh rowsel block (rc=1) on success — install on
 *     g->selection.  The block carries per-segment NONE/MIX/ALL
 *     flags and the morsel-local indices for matching rows.
 *     Pure NONE blocks (no matches) are returned as a valid empty
 *     rowsel rather than NULL — NULL is the "all-pass" sentinel
 *     in the consumer and would let every row through.
 *   - NULL when the column is not eligible: no index, wrong kind,
 *     built_for_len mismatch (stale), type mismatch, or out-of-
 *     range key.  Caller must fall back to the full scan path.
 *
 * Eligibility (and the canonical hashing used) match
 * ray_index_attach_hash: BOOL/U8/I16/I32/I64/DATE/TIME/TIMESTAMP.
 * Floats are intentionally not supported — equality on F32/F64
 * has NaN / -0 semantics the unfused compare kernel handles. */
ray_t* ray_index_hash_eq_rowsel(ray_t* col, int64_t key);

/* Direct equality selection for a fresh part index.  `key` is a numeric
 * value or a column-domain SYM id.  Dense matches remain eligible because
 * the part payload already stores one contiguous physical span. */
ray_t* ray_index_part_eq_rowsel(ray_t* col, int64_t key);

/* ===== Hash/part-index IN probe =====
 *
 * Build a rowsel of all rows whose value is in set_vec via per-element hash
 * probes or direct physical spans from a part index.  Integer-family and SYM
 * columns (same conservatism as the hash-eq path: F32/F64 NaN/-0 semantics
 * belong to the scan kernel).
 *
 * set_vec must match the column family: an integer-family typed vec for an
 * integer column, a SYM vec for a SYM column (each set symbol is resolved
 * through the COLUMN's domain; domain-absent symbols match no rows).
 * Other combinations → NULL.
 *
 * Returns:
 *   - A fresh rowsel block (rc=1) on success — install on g->selection.
 *     An empty set yields a valid all-NONE rowsel (NOT NULL).
 *   - NULL when the column is not eligible (no hash/part index, stale,
 *     float-family column, unsupported set type, OOM) or a hash-index union
 *     is too dense.  Part-index unions retain dense full morsels efficiently.
 *     Caller falls back to the scan path. */
ray_t* ray_index_in_rowsel(ray_t* col, ray_t* set_vec);

/* ===== Hash-index find (point lookup) =====
 *
 * Returns the minimum row id where the column value equals `key`, or
 * -1 when the index proves the key is absent (provably-no-match).
 * Returns -2 when the column is not eligible: no hash index, stale,
 * float-family column, or out-of-eligibility condition — caller must
 * fall back to the linear scan.
 *
 * Contract:
 *   >= 0  → key found; this is the FIRST (minimum) row id match.
 *   -1    → key provably absent; caller returns find's miss value.
 *   -2    → not eligible; caller falls back to the scan.
 *
 * Uses idx_fresh_nonull: null-bearing columns fall back (-2) so the
 * scan correctly surfaces null-equality searches. */
int64_t ray_index_find_row(ray_t* col, int64_t key);

/* ===== Hash-index group slice (CSR accessor) =====
 *
 * Resolve `key` to its group's contiguous ascending row-id slice.
 * SYM columns take the COLUMN-DOMAIN id as key; numeric columns take the
 * value (canonicalized through the storage width like the eq probe).
 *
 * Returns:
 *    1 → hit: *rows_out points at the ascending row ids, *n_out the size.
 *        The slice borrows the index payload — valid while the index is.
 *    0 → key provably absent (*rows_out NULL, *n_out 0).
 *   -1 → not eligible (no fresh hash index / bad key) — caller falls back. */
int ray_index_hash_group(ray_t* col, int64_t key,
                         const int64_t** rows_out, int64_t* n_out);

/* Build a rowsel from ASCENDING row ids (empty n=0 is valid).  NULL on OOM. */
ray_t* ray_index_rowsel_from_ids(int64_t nrows, const int64_t* ids, int64_t n);

/* ===== Slice-group resolution (FILTER + GROUP-by-key fusion) =====
 *
 * One surviving group of the fused shape: the key's column-domain id and
 * its CSR row slice (ascending row ids, borrowed from the index payload —
 * valid while the index is). */
typedef struct {
    int64_t        dom;   /* column-domain key id */
    const int64_t* rows;  /* ascending row ids (borrowed); NULL if contiguous */
    int64_t        first; /* first physical row when rows == NULL */
    int64_t        n;     /* slice length (> 0) */
} ray_idx_slice_t;

/* Resolve a SYM eq key (sym ATOM, global intern id) or IN set (SYM vec,
 * any domain) against `col`'s fresh hash or part index into per-key slices,
 * canonicalized ascending by column-domain id (duplicates collapsed;
 * absent / rowless keys dropped). Hash slices borrow CSR row ids; part
 * slices use rows==NULL plus their contiguous physical first row. On success
 * returns K >= 0 and, for
 * K > 0, *out points into a fresh alloc block returned via *hdr_out
 * (release with ray_free).  K == 0 → provably no surviving rows (*out
 * and *hdr_out NULL).  Returns -1 when ineligible: stale or missing
 * hash/part index, null-bearing or non-SYM column, wrong key shape. */
int64_t ray_index_sym_slices(ray_t* col, ray_t* keys,
                             ray_idx_slice_t** out, ray_t** hdr_out);

/* ===== Sort-index range probe =====
 *
 * Build a rowsel from a binary search over the sort-index permutation for
 * rows satisfying (col cmp_op key).  Supports EQ/LT/LE/GT/GE.  NE returns
 * NULL (two disjoint spans — unsupported).
 *
 * Returns:
 *   - A fresh rowsel block on success — install on g->selection.
 *     span == 0 yields a valid all-NONE rowsel (NOT NULL).
 *   - NULL when not eligible: no sort index, stale, type mismatch,
 *     span > col->len / IDX_RANGE_MAX_FRAC (selectivity guard), OOM,
 *     or NE operator.  Caller falls back to the scan.
 *
 * key_i is used for integer-family columns; key_f for float-family.
 * is_float must match the column family (mirror of idx_filter_decode
 * contract); mismatch returns NULL defensively. */
ray_t* ray_index_range_rowsel(ray_t* col, uint16_t cmp_op,
                              int64_t key_i, double key_f, bool is_float);

/* Build a rowsel by binary-searching a physically non-descending column.
 * Unlike the sort-index route above, matching row ids are already one
 * contiguous interval, so dense spans are eligible and the rowsel stores
 * only the at-most-two partial boundary morsels.  Requires the verified
 * RAY_ATTR_SORTED marker and a null-free numeric column. */
ray_t* ray_sorted_range_rowsel(ray_t* col, uint16_t cmp_op,
                               int64_t key_i, double key_f, bool is_float);

/* ===== Sort-index ORDER BY permutation probe =====
 *
 * Returns a BORROWED pointer to the ascending permutation vector stored in
 * the sort index on `col` when the column carries a fresh, null-free sort
 * index; NULL otherwise.  The caller MUST NOT release the returned pointer
 * (it is owned by the index payload).  To use it as an owned reference,
 * call ray_retain() immediately after. */
ray_t* ray_index_sort_perm_fresh(ray_t* col);

/* ===== Sort-index distinct fast path =====
 *
 * Walk the sort permutation once to collect the first-occurrence row id of
 * each distinct value (the first element of each equal-value run, which is
 * the minimum row id of the run because the sort is stable with iota
 * tie-breaking).  Returns an OWNED I64 vector of those row ids, sorted
 * ascending by VALUE (matching distinct_vec_eager's qsort-of-indices
 * contract), and sets *out_count to the number of distinct values.
 *
 * Returns NULL if not eligible (no sort index, stale, null-bearing) or on
 * OOM.  The caller is responsible for releasing the returned vector. */
ray_t* ray_index_distinct_ids(ray_t* col, int64_t* out_count);

/* ===== Internal helpers (used by retain/release/detach in heap.c
 * and by mutation paths in vec.c) ===== */

/* Release the saved-aux pointers carried by a RAY_INDEX ray_t.
 * Invoked from ray_release_owned_refs when the index ray_t is freed. */
void ray_index_release_saved(ray_index_t* ix);

/* Retain the saved-aux pointers carried by a RAY_INDEX ray_t.
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

ray_t* ray_attr_set_fn (ray_t* name, ray_t* v); /* (.attr.set 'name v) */
ray_t* ray_attr_get_fn (ray_t* v);              /* (.attr.get v) -> sym vec */
ray_t* ray_attr_drop_fn(ray_t* v);              /* (.attr.drop v) -> v cleared */

#endif /* RAY_IDXOP_H */
