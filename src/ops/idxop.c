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
#include "ops/internal.h"
#include "mem/heap.h"
#include "mem/cow.h"
#include "vec/vec.h"
#include "table/table.h"
#include "table/sym.h"
#include "lang/eval.h"
#include "ops/ops.h"
#include "ops/rowsel.h"
#include "ops/hash.h"     /* ray_hash_bytes: STR hash-index key word */
#include "core/pool.h"    /* parallel hash-index build */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#if defined(__linux__)
#include <sys/resource.h>   /* getrusage: the hash build trace */
#endif

/* ── Routing observability counters (diagnostic, unsynchronized) ── */
uint64_t ray_idx_consults[IDX_SITE__N];
uint64_t ray_idx_hits[IDX_SITE__N];

static void idx_stats_dump(void) {
    static const char* names[IDX_SITE__N] = {
        "filter-zone", "filter-bloom", "filter-hash", "filter-part",
        "filter-range", "in", "find", "sort", "distinct", "asof",
        "filter-eq-range", "group-slice",
    };
    for (int i = 0; i < IDX_SITE__N; i++)
        if (ray_idx_consults[i] || ray_idx_hits[i])
            fprintf(stderr, "idx_route %-12s consults=%llu hits=%llu\n",
                    names[i],
                    (unsigned long long)ray_idx_consults[i],
                    (unsigned long long)ray_idx_hits[i]);
}

void ray_idx_stats_init(void) {
    if (getenv("RAY_IDX_STATS")) atexit(idx_stats_dump);
}

/* Width of one element of a numeric vector type, or 0 if unsupported. */
static int numeric_elem_size(int8_t t) {
    switch (t) {
    case RAY_BOOL: case RAY_U8:                       return 1;
    case RAY_I16:                                     return 2;
    case RAY_I32: case RAY_DATE: case RAY_TIME: case RAY_F32:  return 4;
    case RAY_I64: case RAY_TIMESTAMP: case RAY_F64:            return 8;
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
        v = clear_neg_zero(v);
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

/* STR hash-index key word: a 64-bit hash of the bytes.  Two distinct
 * strings may share a word, so every STR key compare in the builder and
 * the probes ALSO compares the payload (str_rows_eq / str_row_eq_bytes)
 * and keeps walking on a payload mismatch — the bucket table may hold two
 * groups with the same word. */
static inline uint64_t str_key_word(const char* p, size_t l) {
    return p ? ray_hash_bytes(p, l) : 0;
}

/* Row i of `v` as the hash-index key word: numeric → numeric_key_word,
 * SYM → the column-domain id (width-aware), STR → str_key_word. */
static uint64_t hash_row_key_word(ray_t* v, const uint8_t* base, int64_t i) {
    if (v->type == RAY_SYM) return (uint64_t)ray_read_sym(base, i, RAY_SYM, v->attrs);
    if (v->type == RAY_STR) {
        size_t l = 0;
        const char* p = ray_str_vec_get(v, i, &l);
        return str_key_word(p, l);
    }
    return numeric_key_word(base, v->type, i);
}

static bool str_row_eq_bytes(ray_t* v, int64_t i, const char* p, size_t l) {
    size_t il = 0;
    const char* ip = ray_str_vec_get(v, i, &il);
    if (!ip) ip = "";
    if (!p)  p  = "";
    return il == l && memcmp(ip, p, l) == 0;
}

static bool str_rows_eq(ray_t* v, int64_t i, int64_t j) {
    size_t jl = 0;
    const char* jp = ray_str_vec_get(v, j, &jl);
    return str_row_eq_bytes(v, i, jp, jl);
}

/* Returns true iff numeric vector v is non-descending.  v1 scope: rejects
 * (returns false) if any null or NaN is present — callers turn false into a
 * verify error.  Caller has already ensured numeric_elem_size(v->type) > 0. */
static bool vec_is_ascending(const ray_t* v) {
    int64_t n = v->len;
    if (n < 2) return true;
    if (ray_vec_may_have_nulls(v)) {
        for (int64_t i = 0; i < n; i++)
            if (ray_vec_is_null((ray_t*)v, i)) return false;
    }
    const void* b = ray_data((ray_t*)v);
    switch (v->type) {
    case RAY_BOOL: case RAY_U8: {
        const uint8_t* p = b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_I16: {
        const int16_t* p = (const int16_t*)b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_I32: case RAY_DATE: case RAY_TIME: {  /* TIME is 4-byte int32 */
        const int32_t* p = (const int32_t*)b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_I64: case RAY_TIMESTAMP: {
        const int64_t* p = (const int64_t*)b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_F32: {
        const float* p = (const float*)b;
        for (int64_t i = 0; i < n; i++) if (p[i] != p[i]) return false; /* NaN */
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_F64: {
        const double* p = (const double*)b;
        for (int64_t i = 0; i < n; i++) if (p[i] != p[i]) return false; /* NaN */
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    default: return false;
    }
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

/* True iff all non-null rows are distinct.  Open-addressing probe over a
 * power-of-two table sized ~2x the row count.  v1: numeric vectors only.
 * Returns false on OOM so the caller raises a verify error (conservative). */
static bool vec_all_distinct(const ray_t* v) {
    int64_t n = v->len;
    if (n < 2) return true;
    uint64_t cap = next_pow2((uint64_t)n * 2 + 1);
    if (cap < 16) cap = 16;
    uint64_t mask = cap - 1;
    /* Transient scratch table, deliberately off-heap (freed before return);
     * it is never an owned ray_t, unlike the persistent hash-index table. */
    int64_t* slot = (int64_t*)ray_calloc_raw((size_t)((size_t)cap) * (sizeof(int64_t))); /* 0 = empty; store i+1 */
    if (!slot) return false;
    const uint8_t* base = (const uint8_t*)ray_data((ray_t*)v);
    bool ok = true;
    bool is_str = (v->type == RAY_STR);
    for (int64_t i = 0; i < n && ok; i++) {
        if (ray_vec_is_null((ray_t*)v, i)) continue;
        uint64_t hi = hash_row_key_word((ray_t*)v, base, i);
        uint64_t h = mix64(hi) & mask;
        for (;;) {
            int64_t cur = slot[h];
            if (cur == 0) { slot[h] = i + 1; break; }
            if (hash_row_key_word((ray_t*)v, base, cur - 1) == hi &&
                (!is_str || str_rows_eq((ray_t*)v, cur - 1, i))) { ok = false; break; } /* dup */
            h = (h + 1) & mask;
        }
    }
    ray_free_raw(slot);
    return ok;
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

/* --------------------------------------------------------------------------
 * Table-resident key index (RAY_IDX_UKEY)
 *
 * A table's aux is zero-init, so unlike a vector there is nothing to snapshot
 * and restore — attach is a pointer write plus the attrs bit, detach clears
 * both.  Refcounting rides the type-agnostic RAY_ATTR_HAS_INDEX arms of
 * ray_retain_owned_refs / ray_release_owned_refs, so the index dies with the
 * table and a copied table is handled by the caller (a fresh table starts
 * without one).
 * -------------------------------------------------------------------------- */

ray_t* ray_index_build_ukey(const int64_t* kci, int64_t nk, int64_t entries) {
    if (nk <= 0 || nk > RAY_UKEY_MAX_COLS) return NULL;
    for (int64_t k = 0; k < nk; k++)
        if (kci[k] < 0 || kci[k] > INT16_MAX) return NULL;  /* kci is int16 */
    uint64_t cap = 16;
    while (cap < (uint64_t)entries * 2) {
        if (cap > (UINT64_MAX >> 1)) return ray_error("oom", NULL);
        cap <<= 1;
    }
    ray_t* slots = ray_vec_new(RAY_I64, (int64_t)cap);
    if (!slots || RAY_IS_ERR(slots)) return slots ? slots : ray_error("oom", NULL);
    slots->len = (int64_t)cap;
    memset(ray_data(slots), 0, (size_t)cap * sizeof(int64_t));

    ray_t* idx = ray_index_alloc(RAY_IDX_UKEY, RAY_TABLE, 0);
    if (!idx || RAY_IS_ERR(idx)) { ray_release(slots); return idx ? idx : ray_error("oom", NULL); }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.ukey.slots = slots;
    ix->u.ukey.mask  = cap - 1;
    ix->u.ukey.nrows = 0;
    ix->u.ukey.n_tomb = 0;
    ix->u.ukey.nk    = nk;
    for (int64_t k = 0; k < nk; k++) ix->u.ukey.kci[k] = (int16_t)kci[k];
    return idx;
}

bool ray_table_ukey_attach(ray_t* tbl, ray_t* idx) {
    if (!tbl || tbl->type != RAY_TABLE || !idx || RAY_IS_ERR(idx)) return false;
    if (tbl->attrs & RAY_ATTR_HAS_INDEX) return false;
    tbl->index    = idx;
    tbl->_idx_pad = NULL;
    tbl->attrs   |= RAY_ATTR_HAS_INDEX;
    return true;
}

ray_index_t* ray_table_ukey_get(ray_t* tbl) {
    if (!tbl || tbl->type != RAY_TABLE) return NULL;
    if (!(tbl->attrs & RAY_ATTR_HAS_INDEX)) return NULL;
    ray_t* idx = tbl->index;
    if (!idx || RAY_IS_ERR(idx) || idx->type != RAY_INDEX) return NULL;
    ray_index_t* ix = ray_index_payload(idx);
    return ix->kind == RAY_IDX_UKEY ? ix : NULL;
}

void ray_table_ukey_drop(ray_t* tbl) {
    if (!tbl || tbl->type != RAY_TABLE) return;
    if (!(tbl->attrs & RAY_ATTR_HAS_INDEX)) return;
    ray_t* idx = tbl->index;
    tbl->index    = NULL;
    tbl->_idx_pad = NULL;
    tbl->attrs   &= (uint8_t)~RAY_ATTR_HAS_INDEX;
    if (idx && !RAY_IS_ERR(idx)) ray_release(idx);
}

/* --------------------------------------------------------------------------
 * Saved-aux retain / release
 *
 * The 16 byte snapshot preserves the parent's original aux-union bytes
 * across attach/detach.  Since index attach is restricted to numeric
 * types (see prepare_attach), the snapshot contains either:
 *   - all-zero bytes (no link, no nulls), or
 *   - bytes 8-15 hold an int64 link_target (HAS_LINK on I32/I64 cols).
 * Neither case carries an owning ray_t* reference, so retain/release
 * are no-ops.  The functions remain to preserve the heap.c / vec.c
 * call sites symmetric with the pre-migration layout. */

void ray_index_release_saved(ray_index_t* ix) {
    (void)ix;
}

void ray_index_retain_saved(ray_index_t* ix) {
    (void)ix;
}

/* --------------------------------------------------------------------------
 * Per-kind payload retain / release
 * -------------------------------------------------------------------------- */

void ray_index_release_payload(ray_index_t* ix) {
    switch ((ray_idx_kind_t)ix->kind) {
    case RAY_IDX_UKEY:
        if (ix->u.ukey.slots && !RAY_IS_ERR(ix->u.ukey.slots))
            ray_release(ix->u.ukey.slots);
        break;
    case RAY_IDX_HASH:
        if (ix->u.hash.table && !RAY_IS_ERR(ix->u.hash.table))
            ray_release(ix->u.hash.table);
        if (ix->u.hash.gkeys && !RAY_IS_ERR(ix->u.hash.gkeys))
            ray_release(ix->u.hash.gkeys);
        if (ix->u.hash.offs && !RAY_IS_ERR(ix->u.hash.offs))
            ray_release(ix->u.hash.offs);
        if (ix->u.hash.rows && !RAY_IS_ERR(ix->u.hash.rows))
            ray_release(ix->u.hash.rows);
        ix->u.hash.table = ix->u.hash.gkeys = NULL;
        ix->u.hash.offs  = ix->u.hash.rows  = NULL;
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
    case RAY_IDX_CHUNK_ZONE:
        if (ix->u.chunk_zone.mins && !RAY_IS_ERR(ix->u.chunk_zone.mins))
            ray_release(ix->u.chunk_zone.mins);
        if (ix->u.chunk_zone.maxs && !RAY_IS_ERR(ix->u.chunk_zone.maxs))
            ray_release(ix->u.chunk_zone.maxs);
        if (ix->u.chunk_zone.null_bits && !RAY_IS_ERR(ix->u.chunk_zone.null_bits))
            ray_release(ix->u.chunk_zone.null_bits);
        if (ix->u.chunk_zone.aggs && !RAY_IS_ERR(ix->u.chunk_zone.aggs))
            ray_release(ix->u.chunk_zone.aggs);
        ix->u.chunk_zone.mins = NULL;
        ix->u.chunk_zone.maxs = NULL;
        ix->u.chunk_zone.null_bits = NULL;
        ix->u.chunk_zone.aggs = NULL;
        break;
    case RAY_IDX_PART:
        if (ix->u.part.keys   && !RAY_IS_ERR(ix->u.part.keys))   ray_release(ix->u.part.keys);
        if (ix->u.part.starts && !RAY_IS_ERR(ix->u.part.starts)) ray_release(ix->u.part.starts);
        if (ix->u.part.lens   && !RAY_IS_ERR(ix->u.part.lens))   ray_release(ix->u.part.lens);
        ix->u.part.keys = ix->u.part.starts = ix->u.part.lens = NULL;
        break;
    case RAY_IDX_DICT:
        if (ix->u.dict.codes  && !RAY_IS_ERR(ix->u.dict.codes))  ray_release(ix->u.dict.codes);
        if (ix->u.dict.first_occ && !RAY_IS_ERR(ix->u.dict.first_occ)) ray_release(ix->u.dict.first_occ);
        ix->u.dict.codes = ix->u.dict.first_occ = NULL;
        break;
    case RAY_IDX_ZONE:
    case RAY_IDX_NONE:
        break;
    }
}

int ray_index_child_blocks(const ray_index_t* ix, ray_t** out, int cap) {
    ray_t* c[4] = { NULL, NULL, NULL, NULL };
    switch ((ray_idx_kind_t)ix->kind) {
    case RAY_IDX_UKEY:       c[0] = ix->u.ukey.slots; break;
    case RAY_IDX_HASH:
        c[0] = ix->u.hash.table; c[1] = ix->u.hash.gkeys;
        c[2] = ix->u.hash.offs;  c[3] = ix->u.hash.rows;
        break;
    case RAY_IDX_SORT:       c[0] = ix->u.sort.perm; break;
    case RAY_IDX_BLOOM:      c[0] = ix->u.bloom.bits; break;
    case RAY_IDX_CHUNK_ZONE:
        c[0] = ix->u.chunk_zone.mins; c[1] = ix->u.chunk_zone.maxs;
        c[2] = ix->u.chunk_zone.null_bits; c[3] = ix->u.chunk_zone.aggs;
        break;
    case RAY_IDX_PART:
        c[0] = ix->u.part.keys; c[1] = ix->u.part.starts; c[2] = ix->u.part.lens;
        break;
    case RAY_IDX_DICT:
        c[0] = ix->u.dict.codes; c[1] = ix->u.dict.first_occ;
        break;
    case RAY_IDX_ZONE:
    case RAY_IDX_NONE:
        break;
    }
    int n = 0;
    for (int i = 0; i < 4 && n < cap; i++)
        if (c[i] && !RAY_IS_ERR(c[i])) out[n++] = c[i];
    return n;
}

void ray_index_retain_payload(ray_index_t* ix) {
    switch ((ray_idx_kind_t)ix->kind) {
    case RAY_IDX_UKEY:
        if (ix->u.ukey.slots && !RAY_IS_ERR(ix->u.ukey.slots))
            ray_retain(ix->u.ukey.slots);
        break;
    case RAY_IDX_HASH:
        if (ix->u.hash.table && !RAY_IS_ERR(ix->u.hash.table))
            ray_retain(ix->u.hash.table);
        if (ix->u.hash.gkeys && !RAY_IS_ERR(ix->u.hash.gkeys))
            ray_retain(ix->u.hash.gkeys);
        if (ix->u.hash.offs && !RAY_IS_ERR(ix->u.hash.offs))
            ray_retain(ix->u.hash.offs);
        if (ix->u.hash.rows && !RAY_IS_ERR(ix->u.hash.rows))
            ray_retain(ix->u.hash.rows);
        break;
    case RAY_IDX_SORT:
        if (ix->u.sort.perm && !RAY_IS_ERR(ix->u.sort.perm))
            ray_retain(ix->u.sort.perm);
        break;
    case RAY_IDX_BLOOM:
        if (ix->u.bloom.bits && !RAY_IS_ERR(ix->u.bloom.bits))
            ray_retain(ix->u.bloom.bits);
        break;
    case RAY_IDX_CHUNK_ZONE:
        if (ix->u.chunk_zone.mins && !RAY_IS_ERR(ix->u.chunk_zone.mins))
            ray_retain(ix->u.chunk_zone.mins);
        if (ix->u.chunk_zone.maxs && !RAY_IS_ERR(ix->u.chunk_zone.maxs))
            ray_retain(ix->u.chunk_zone.maxs);
        if (ix->u.chunk_zone.null_bits && !RAY_IS_ERR(ix->u.chunk_zone.null_bits))
            ray_retain(ix->u.chunk_zone.null_bits);
        if (ix->u.chunk_zone.aggs && !RAY_IS_ERR(ix->u.chunk_zone.aggs))
            ray_retain(ix->u.chunk_zone.aggs);
        break;
    case RAY_IDX_PART:
        if (ix->u.part.keys   && !RAY_IS_ERR(ix->u.part.keys))   ray_retain(ix->u.part.keys);
        if (ix->u.part.starts && !RAY_IS_ERR(ix->u.part.starts)) ray_retain(ix->u.part.starts);
        if (ix->u.part.lens   && !RAY_IS_ERR(ix->u.part.lens))   ray_retain(ix->u.part.lens);
        break;
    case RAY_IDX_DICT:
        if (ix->u.dict.codes  && !RAY_IS_ERR(ix->u.dict.codes))  ray_retain(ix->u.dict.codes);
        if (ix->u.dict.first_occ && !RAY_IS_ERR(ix->u.dict.first_occ)) ray_retain(ix->u.dict.first_occ);
        break;
    case RAY_IDX_ZONE:
    case RAY_IDX_NONE:
        break;
    }
}

/* Deep-clone a RAY_INDEX block, sharing (retaining) its payload child vectors.
 * ray_alloc_copy CANNOT be used here: a RAY_INDEX block's type (97) is outside
 * the vector-type range, so ray_alloc_copy computes data_size==0 and copies
 * only the 32-byte header, silently dropping the ray_index_t payload (kind,
 * markers, child pointers).  Used when a marker must be set on an index block
 * that is shared after copy-on-write. */
static ray_t* clone_index_block(ray_t* blk) {
    ray_index_t* src = ray_index_payload(blk);
    ray_t* nb = ray_index_alloc((ray_idx_kind_t)src->kind, src->parent_type,
                                src->built_for_len);
    if (!nb || RAY_IS_ERR(nb)) return nb ? nb : ray_error("oom", NULL);
    ray_index_t* dst = ray_index_payload(nb);
    memcpy(dst, src, sizeof(ray_index_t));   /* kind, markers, saved_aux, union */
    ray_index_retain_payload(dst);           /* child vectors now referenced twice */
    ray_index_retain_saved(dst);             /* no-op for numeric, kept symmetric */
    return nb;
}

/* --------------------------------------------------------------------------
 * Zone scan -- compute min/max + null count
 *
 * Reads the parent vector before the aux is displaced.  Integer paths
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
    case RAY_DATE:
    case RAY_TIME:      return zone_scan_int(v, ix, 4);  /* TIME is 4-byte int32 */
    case RAY_I64:
    case RAY_TIMESTAMP: return zone_scan_int(v, ix, 8);
    case RAY_F32:       return zone_scan_float(v, ix, 4);
    case RAY_F64:       return zone_scan_float(v, ix, 8);
    default:            return RAY_ERR_NYI;
    }
}

/* --------------------------------------------------------------------------
 * Chunk-zone scan -- per-(1<<chunk_log2)-row min/max + null flag
 *
 * For each chunk g in [0, n_chunks) the scan computes the chunk's min and
 * max value across its row range and sets the chunk's null-bit if any row
 * in that chunk is a null sentinel.  Whole-column extrema fall out as
 * min(mins[*]) / max(maxs[*]) so the reduce min/max path can consume this
 * index without needing a separate column-wide zone.
 * -------------------------------------------------------------------------- */

static ray_err_t chunk_zone_scan_int(ray_t* v, ray_index_t* ix,
                                     int elem_size) {
    uint32_t n_chunks = ix->u.chunk_zone.n_chunks;
    uint8_t  log2     = ix->u.chunk_zone.chunk_log2;
    int64_t  csz      = 1LL << log2;
    int64_t  n        = v->len;
    int64_t* mins     = (int64_t*)ray_data(ix->u.chunk_zone.mins);
    int64_t* maxs     = (int64_t*)ray_data(ix->u.chunk_zone.maxs);
    uint8_t* nbits    = (uint8_t*)ray_data(ix->u.chunk_zone.null_bits);
    const uint8_t* base = (const uint8_t*)ray_data(v);

    for (uint32_t g = 0; g < n_chunks; g++) {
        if (RAY_UNLIKELY(ray_interrupted())) return RAY_ERR_CANCEL;
        int64_t s = (int64_t)g * csz;
        int64_t e = s + csz; if (e > n) e = n;
        int64_t mn = INT64_MAX, mx = INT64_MIN;
        uint64_t sum = 0;          /* low word: wraps like the engine's int64 sum */
        int64_t  hi  = 0;          /* high word of the exact 128-bit sum */
        int64_t nn = 0;
        bool any_null = false;
        for (int64_t i = s; i < e; i++) {
            if (ray_vec_is_null(v, i)) { any_null = true; continue; }
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
            ray_i128_add(&hi, &sum, val);
            nn++;
        }
        if (ix->u.chunk_zone.aggs) {
            int64_t* ag = (int64_t*)ray_data(ix->u.chunk_zone.aggs);
            ag[g] = (int64_t)sum;
            ag[n_chunks + g] = nn;
            if (ix->u.chunk_zone.aggs->len >= 3 * (int64_t)n_chunks)
                ag[2 * (int64_t)n_chunks + g] = hi;
        }
        /* Empty (all-null) chunks keep mn=INT64_MAX / mx=INT64_MIN so
         * the reduce path's min(mins[*]) / max(maxs[*]) ignores them. */
        mins[g] = mn;
        maxs[g] = mx;
        if (any_null) nbits[g >> 3] |= (uint8_t)(1u << (g & 7));
    }
    return RAY_OK;
}

static ray_err_t chunk_zone_scan_float(ray_t* v, ray_index_t* ix,
                                       int elem_size) {
    uint32_t n_chunks = ix->u.chunk_zone.n_chunks;
    uint8_t  log2     = ix->u.chunk_zone.chunk_log2;
    int64_t  csz      = 1LL << log2;
    int64_t  n        = v->len;
    double*  mins     = (double*)ray_data(ix->u.chunk_zone.mins);
    double*  maxs     = (double*)ray_data(ix->u.chunk_zone.maxs);
    uint8_t* nbits    = (uint8_t*)ray_data(ix->u.chunk_zone.null_bits);
    const uint8_t* base = (const uint8_t*)ray_data(v);

    for (uint32_t g = 0; g < n_chunks; g++) {
        if (RAY_UNLIKELY(ray_interrupted())) return RAY_ERR_CANCEL;
        int64_t s = (int64_t)g * csz;
        int64_t e = s + csz; if (e > n) e = n;
        double mn = INFINITY, mx = -INFINITY;
        bool any_null = false;
        for (int64_t i = s; i < e; i++) {
            if (ray_vec_is_null(v, i)) { any_null = true; continue; }
            double val = 0.0;
            if (elem_size == 4) {
                float t; memcpy(&t, base + i*4, 4); val = (double)t;
            } else {
                memcpy(&val, base + i*8, 8);
            }
            if (isnan(val)) { any_null = true; continue; }
            if (val < mn) mn = val;
            if (val > mx) mx = val;
        }
        /* Empty (all-null) chunks keep mn=+inf / mx=-inf so reduce
         * (min/max across mins[]/maxs[]) ignores them. */
        mins[g] = mn;
        maxs[g] = mx;
        if (any_null) nbits[g >> 3] |= (uint8_t)(1u << (g & 7));
    }
    return RAY_OK;
}

static ray_err_t chunk_zone_scan(ray_t* v, ray_index_t* ix) {
    switch (v->type) {
    case RAY_BOOL:
    case RAY_U8:        return chunk_zone_scan_int(v, ix, 1);
    case RAY_I16:       return chunk_zone_scan_int(v, ix, 2);
    case RAY_I32:
    case RAY_DATE:
    case RAY_TIME:      return chunk_zone_scan_int(v, ix, 4);  /* TIME is 4-byte int32 */
    case RAY_I64:
    case RAY_TIMESTAMP: return chunk_zone_scan_int(v, ix, 8);
    case RAY_F32:       return chunk_zone_scan_float(v, ix, 4);
    case RAY_F64:       return chunk_zone_scan_float(v, ix, 8);
    default:            return RAY_ERR_NYI;
    }
}

/* --------------------------------------------------------------------------
 * Attach
 *
 * The 16-byte snapshot preserves the parent's aux-union bytes across
 * the attachment so detach can restore them byte-for-byte.  For numeric
 * vectors (the only types that may attach) bytes 0-7 are unused and
 * bytes 8-15 carry link_target when HAS_LINK is set — no owned pointers
 * either way.  We do NOT retain anything here; the index pointer install
 * at bytes 0-7 transfers a single ref to the parent (no extra retain).
 * -------------------------------------------------------------------------- */

static ray_t* attach_finalize(ray_t* parent, ray_t* idx) {
    ray_index_t* ix = ray_index_payload(idx);
    /* Snapshot the parent's 16 raw bytes verbatim. */
    memcpy(ix->saved_aux, parent->aux, 16);
    ix->saved_attrs = parent->attrs & RAY_ATTR_HAS_NULLS;

    /* Install the index pointer — overwrites bytes 0-7 with the index ptr.
     * Bytes 8-15 carry link_target when HAS_LINK is set; preserve them.
     * Otherwise zero _idx_pad as a tidy default.
     *
     * IMPORTANT: HAS_NULLS is *preserved* on the parent so the many call
     * sites that use it as a cheap "do I need null logic at all?" gate
     * continue to give correct answers.  The actual null state is read
     * via ray_vec_is_null (sentinel-based), which is unaffected by the
     * index pointer overlay at bytes 0-7. */
    parent->index    = idx;
    /* _idx_pad (bytes 8-15) aliases str_pool on a RAY_STR parent and
     * sym_domain on a RAY_SYM parent — NEVER clear it for either, or
     * we'd null the column's pool/domain.  HAS_LINK uses it too. */
    if (!(parent->attrs & RAY_ATTR_HAS_LINK) &&
        parent->type != RAY_STR && parent->type != RAY_SYM)
        parent->_idx_pad = NULL;
    parent->attrs   |= RAY_ATTR_HAS_INDEX;
    return parent;
}

/* Validate + COW + drop existing index.  Returns the (possibly new) parent
 * pointer and updates *vp.  On error returns a RAY_ERROR; caller must
 * propagate without further modifying *vp. */
static ray_t* prepare_attach_ex(ray_t** vp, const char* what,
                                bool allow_str, bool allow_sym) {
    if (!vp || !*vp || RAY_IS_ERR(*vp))
        return ray_error("type", "%s: null/error vector", what);
    ray_t* v = *vp;
    if (!ray_is_vec(v))
        return ray_error("type", "%s: index can only attach to a vector", what);
    if (v->attrs & RAY_ATTR_SLICE)
        return ray_error("type", "%s: cannot index a slice; materialize first", what);
    if (v->attrs & RAY_ATTR_HAS_INDEX) {
        ray_index_drop(&v);
        if (!v) return ray_error("oom", NULL);  /* keep *vp intact */
        if (RAY_IS_ERR(v)) return v;
        *vp = v;
    }
    v = ray_cow(v);
    /* Never hand back NULL: every caller tests only RAY_IS_ERR and then
     * dereferences, so a failed copy must surface as an error. */
    if (!v) return ray_error("oom", NULL);
    if (RAY_IS_ERR(v)) return v;
    *vp = v;
    /* Numeric vectors carry any index kind; STR carries only RAY_IDX_DICT
     * (codes live alongside the descriptors — the column representation is
     * untouched).  allow_str gates that one exception.  RAY_SYM carries
     * RAY_IDX_HASH: id values are numeric ints of adaptive width;
     * allow_sym gates that exception (hash attach only). */
    if (numeric_elem_size(v->type) == 0 &&
        !(allow_str && v->type == RAY_STR) &&
        !(allow_sym && v->type == RAY_SYM)) {
        return ray_error("nyi", "%s: only numeric/sym vectors supported (got type %d)",
                         what, (int)v->type);
    }
    return v;
}
static ray_t* prepare_attach(ray_t** vp, const char* what) {
    return prepare_attach_ex(vp, what, false, false);
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

ray_t* ray_index_attach_chunk_zone(ray_t** vp, uint8_t chunk_log2) {
    ray_t* v = prepare_attach(vp, "chunk_zone");
    if (RAY_IS_ERR(v)) return v;

    if (chunk_log2 == 0) chunk_log2 = 16;          /* default 64 K rows / chunk */
    if (chunk_log2 < 8 || chunk_log2 > 22)
        return ray_error("domain", "chunk_zone: chunk_log2 out of range [8, 22]");
    int64_t csz = 1LL << chunk_log2;
    /* No point indexing a column smaller than one chunk — fall back to
     * the column-wide zone (or no index at all) at that size. */
    if (v->len < csz)
        return ray_error("domain", "chunk_zone: column has fewer rows than one chunk");

    uint32_t n_chunks = (uint32_t)((v->len + csz - 1) / csz);

    ray_t* idx = ray_index_alloc(RAY_IDX_CHUNK_ZONE, v->type, v->len);
    if (!idx || RAY_IS_ERR(idx)) return idx;
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.chunk_zone.n_chunks   = n_chunks;
    ix->u.chunk_zone.chunk_log2 = chunk_log2;
    ix->u.chunk_zone.is_f64     = (v->type == RAY_F64 || v->type == RAY_F32) ? 1 : 0;

    int8_t arr_type = ix->u.chunk_zone.is_f64 ? RAY_F64 : RAY_I64;
    ray_t* mins = ray_vec_new(arr_type, (int64_t)n_chunks);
    ray_t* maxs = ray_vec_new(arr_type, (int64_t)n_chunks);
    int64_t nb_len = (int64_t)((n_chunks + 7) / 8);
    ray_t* nbits = ray_vec_new(RAY_U8, nb_len);
    if (!mins || RAY_IS_ERR(mins) || !maxs || RAY_IS_ERR(maxs) ||
        !nbits || RAY_IS_ERR(nbits))
    {
        if (mins && !RAY_IS_ERR(mins)) ray_release(mins);
        if (maxs && !RAY_IS_ERR(maxs)) ray_release(maxs);
        if (nbits && !RAY_IS_ERR(nbits)) ray_release(nbits);
        ray_release(idx);
        return ray_error("oom", "chunk_zone: arrays alloc");
    }
    mins->len  = (int64_t)n_chunks;
    maxs->len  = (int64_t)n_chunks;
    nbits->len = nb_len;
    memset(ray_data(nbits), 0, (size_t)nb_len);
    ix->u.chunk_zone.mins      = mins;
    ix->u.chunk_zone.maxs      = maxs;
    ix->u.chunk_zone.null_bits = nbits;
    if (!ix->u.chunk_zone.is_f64) {
        /* [sum low words | non-null counts | sum high words], one per chunk */
        ray_t* aggs = ray_vec_new(RAY_I64, 3 * (int64_t)n_chunks);
        if (!aggs || RAY_IS_ERR(aggs)) { ray_release(idx); return ray_error("oom", "chunk_zone: aggs alloc"); }
        aggs->len = 3 * (int64_t)n_chunks;
        ix->u.chunk_zone.aggs = aggs;
    }

    ray_err_t err = chunk_zone_scan(v, ix);
    if (err != RAY_OK) {
        ray_release(idx);   /* releases mins/maxs/nbits via release_payload */
        return ray_error(ray_err_code_str(err),
                         "chunk_zone scan failed for type %d", (int)v->type);
    }
    return attach_finalize(v, idx);
}

ray_t* ray_index_chunk_zone_compute(ray_t* v, uint8_t chunk_log2) {
    if (!v || RAY_IS_ERR(v) || !ray_is_vec(v))
        return ray_error("type", "chunk_zone: compute needs a vector");
    if (chunk_log2 == 0) chunk_log2 = 16;
    if (chunk_log2 < 8 || chunk_log2 > 22)
        return ray_error("domain", "chunk_zone: chunk_log2 out of range [8, 22]");
    if (numeric_elem_size(v->type) == 0)
        return ray_error("nyi", "chunk_zone: only numeric vectors supported");
    int64_t csz = 1LL << chunk_log2;
    if (v->len < csz)
        return ray_error("domain", "chunk_zone: column has fewer rows than one chunk");

    uint32_t n_chunks = (uint32_t)((v->len + csz - 1) / csz);
    ray_t* idx = ray_index_alloc(RAY_IDX_CHUNK_ZONE, v->type, v->len);
    if (!idx || RAY_IS_ERR(idx)) return idx;
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.chunk_zone.n_chunks   = n_chunks;
    ix->u.chunk_zone.chunk_log2 = chunk_log2;
    ix->u.chunk_zone.is_f64     = (v->type == RAY_F64 || v->type == RAY_F32) ? 1 : 0;

    int8_t arr_type = ix->u.chunk_zone.is_f64 ? RAY_F64 : RAY_I64;
    ray_t* mins = ray_vec_new(arr_type, (int64_t)n_chunks);
    ray_t* maxs = ray_vec_new(arr_type, (int64_t)n_chunks);
    int64_t nb_len = (int64_t)((n_chunks + 7) / 8);
    ray_t* nbits = ray_vec_new(RAY_U8, nb_len);
    if (!mins || RAY_IS_ERR(mins) || !maxs || RAY_IS_ERR(maxs) ||
        !nbits || RAY_IS_ERR(nbits)) {
        if (mins && !RAY_IS_ERR(mins)) ray_release(mins);
        if (maxs && !RAY_IS_ERR(maxs)) ray_release(maxs);
        if (nbits && !RAY_IS_ERR(nbits)) ray_release(nbits);
        ray_release(idx);
        return ray_error("oom", "chunk_zone: arrays alloc");
    }
    mins->len = (int64_t)n_chunks;
    maxs->len = (int64_t)n_chunks;
    nbits->len = nb_len;
    memset(ray_data(nbits), 0, (size_t)nb_len);
    ix->u.chunk_zone.mins      = mins;
    ix->u.chunk_zone.maxs      = maxs;
    ix->u.chunk_zone.null_bits = nbits;
    if (!ix->u.chunk_zone.is_f64) {
        /* [sum low words | non-null counts | sum high words], one per chunk */
        ray_t* aggs = ray_vec_new(RAY_I64, 3 * (int64_t)n_chunks);
        if (!aggs || RAY_IS_ERR(aggs)) { ray_release(idx); return ray_error("oom", "chunk_zone: aggs alloc"); }
        aggs->len = 3 * (int64_t)n_chunks;
        ix->u.chunk_zone.aggs = aggs;
    }

    ray_err_t err = chunk_zone_scan(v, ix);
    if (err != RAY_OK) {
        ray_release(idx);
        return ray_error(ray_err_code_str(err),
                         "chunk_zone scan failed for type %d", (int)v->type);
    }
    return idx;   /* standalone RAY_INDEX object — caller releases */
}

/* ── Incremental chunk zone ─────────────────────────────────────────────────
 * The stream writers see a column in slices; per chunk the zone needs only
 * running min / max, a null flag and (integers) the exact 128-bit sum and
 * non-null count, so any slice size and any starting row work, and two
 * accumulators over disjoint row ranges merge chunk by chunk.  Arrays are
 * indexed by GLOBAL chunk (row >> 16) so a Parquet task that starts at row
 * 5 M carries a few empty leading chunks and merges by index. */
#define ZONE_ACC_LOG2 16

bool ray_zone_acc_supported(int8_t type) { return numeric_elem_size(type) != 0; }

static ray_err_t zone_acc_grow(ray_zone_acc_t* a, uint32_t want) {
    if (want <= a->n_chunks) return RAY_OK;
    if (want <= a->cap) { a->n_chunks = want; return RAY_OK; }
    uint32_t cap = a->cap ? a->cap : 16;
    while (cap < want) cap *= 2;
    size_t old = a->cap;
#define ZONE_GROW(field, T, init) do {                                        \
        T* p = (T*)ray_realloc_raw(a->field, (size_t)cap * sizeof(T));        \
        if (!p) return RAY_ERR_OOM;                                           \
        for (size_t i = old; i < cap; i++) p[i] = (init);                     \
        a->field = p; } while (0)
    if (a->is_f64) { ZONE_GROW(fmins, double, INFINITY); ZONE_GROW(fmaxs, double, -INFINITY); }
    else {
        ZONE_GROW(mins, int64_t, INT64_MAX); ZONE_GROW(maxs, int64_t, INT64_MIN);
        ZONE_GROW(sum_lo, uint64_t, 0); ZONE_GROW(sum_hi, int64_t, 0); ZONE_GROW(nn, int64_t, 0);
    }
    ZONE_GROW(nulls, uint8_t, 0);
#undef ZONE_GROW
    a->cap = cap;
    a->n_chunks = want;
    return RAY_OK;
}

ray_err_t ray_zone_acc_init(ray_zone_acc_t* a, int8_t type, int64_t start_row) {
    memset(a, 0, sizeof(*a));
    a->esz = numeric_elem_size(type);
    if (a->esz == 0 || start_row < 0) return RAY_ERR_TYPE;
    a->type = type;
    a->is_f64 = (type == RAY_F64 || type == RAY_F32);
    a->next_row = start_row;
    return RAY_OK;
}

void ray_zone_acc_free(ray_zone_acc_t* a) {
    ray_free_raw(a->mins); ray_free_raw(a->maxs); ray_free_raw(a->fmins); ray_free_raw(a->fmaxs);
    ray_free_raw(a->nulls); ray_free_raw(a->sum_lo); ray_free_raw(a->sum_hi); ray_free_raw(a->nn);
    memset(a, 0, sizeof(*a));
}

ray_err_t ray_zone_acc_add(ray_zone_acc_t* a, ray_t* v) {
    if (!a->type || !v || RAY_IS_ERR(v) || v->type != a->type) return RAY_ERR_TYPE;
    int64_t n = v->len;
    if (n <= 0) return RAY_OK;
    int64_t csz = 1LL << ZONE_ACC_LOG2;
    int64_t last = a->next_row + n - 1;
    ray_err_t err = zone_acc_grow(a, (uint32_t)(last >> ZONE_ACC_LOG2) + 1);
    if (err != RAY_OK) return err;
    const uint8_t* base = (const uint8_t*)ray_data(v);
    int64_t i = 0;
    while (i < n) {
        if (RAY_UNLIKELY(ray_interrupted())) return RAY_ERR_CANCEL;
        int64_t row = a->next_row + i;
        uint32_t g = (uint32_t)(row >> ZONE_ACC_LOG2);
        int64_t e = ((int64_t)g + 1) * csz - a->next_row; if (e > n) e = n;
        bool any_null = false;
        if (a->is_f64) {
            double mn = a->fmins[g], mx = a->fmaxs[g];
            for (; i < e; i++) {
                double val;
                if (a->esz == 4) { float t; memcpy(&t, base + i * 4, 4); val = (double)t; }
                else memcpy(&val, base + i * 8, 8);
                if (isnan(val)) { any_null = true; continue; }
                if (val < mn) mn = val;
                if (val > mx) mx = val;
            }
            a->fmins[g] = mn; a->fmaxs[g] = mx;
        } else {
            int64_t mn = a->mins[g], mx = a->maxs[g], nn = a->nn[g], hi = a->sum_hi[g];
            uint64_t lo = a->sum_lo[g];
            for (; i < e; i++) {
                /* Null = the type's sentinel VALUE, whatever the slice header
                 * says (col.c #495: the persisted bit derives from the payload). */
                int64_t val = 0;
                bool is_null = false;
                switch (a->esz) {
                case 1: val = (int64_t)base[i]; break;
                case 2: { int16_t t; memcpy(&t, base + i * 2, 2); val = t; is_null = (t == NULL_I16); break; }
                case 4: { int32_t t; memcpy(&t, base + i * 4, 4); val = t; is_null = (t == NULL_I32); break; }
                default: { int64_t t; memcpy(&t, base + i * 8, 8); val = t; is_null = (t == NULL_I64); break; }
                }
                if (is_null) { any_null = true; continue; }
                if (val < mn) mn = val;
                if (val > mx) mx = val;
                ray_i128_add(&hi, &lo, val);
                nn++;
            }
            a->mins[g] = mn; a->maxs[g] = mx; a->nn[g] = nn; a->sum_hi[g] = hi; a->sum_lo[g] = lo;
        }
        if (any_null) { a->nulls[g] = 1; a->saw_null = true; }
    }
    a->next_row += n;
    return RAY_OK;
}

ray_err_t ray_zone_acc_merge(ray_zone_acc_t* dst, const ray_zone_acc_t* src) {
    if (!dst->type || dst->type != src->type) return RAY_ERR_TYPE;
    ray_err_t err = zone_acc_grow(dst, src->n_chunks);
    if (err != RAY_OK) return err;
    for (uint32_t g = 0; g < src->n_chunks; g++) {
        if (dst->is_f64) {
            if (src->fmins[g] < dst->fmins[g]) dst->fmins[g] = src->fmins[g];
            if (src->fmaxs[g] > dst->fmaxs[g]) dst->fmaxs[g] = src->fmaxs[g];
        } else {
            if (src->mins[g] < dst->mins[g]) dst->mins[g] = src->mins[g];
            if (src->maxs[g] > dst->maxs[g]) dst->maxs[g] = src->maxs[g];
            dst->nn[g] += src->nn[g];
            ray_i128_add128(&dst->sum_hi[g], &dst->sum_lo[g], src->sum_hi[g], src->sum_lo[g]);
        }
        dst->nulls[g] |= src->nulls[g];
    }
    if (src->saw_null) dst->saw_null = true;
    if (src->next_row > dst->next_row) dst->next_row = src->next_row;
    return RAY_OK;
}

ray_t* ray_zone_acc_finish(ray_zone_acc_t* a, int64_t len) {
    int64_t csz = 1LL << ZONE_ACC_LOG2;
    if (!a->type || len < csz) { ray_zone_acc_free(a); return NULL; }
    uint32_t n_chunks = (uint32_t)((len + csz - 1) / csz);
    ray_err_t err = zone_acc_grow(a, n_chunks);   /* chunks never touched stay empty */
    if (err != RAY_OK) { ray_zone_acc_free(a); return ray_error("oom", "chunk_zone: arrays alloc"); }
    ray_t* idx = ray_index_alloc(RAY_IDX_CHUNK_ZONE, a->type, len);
    if (!idx || RAY_IS_ERR(idx)) { ray_zone_acc_free(a); return idx; }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.chunk_zone.n_chunks = n_chunks;
    ix->u.chunk_zone.chunk_log2 = ZONE_ACC_LOG2;
    ix->u.chunk_zone.is_f64 = a->is_f64;
    int8_t arr_type = a->is_f64 ? RAY_F64 : RAY_I64;
    ray_t* mins = ray_vec_new(arr_type, n_chunks);
    ray_t* maxs = ray_vec_new(arr_type, n_chunks);
    int64_t nb_len = (n_chunks + 7) / 8;
    ray_t* nbits = ray_vec_new(RAY_U8, nb_len);
    ray_t* aggs = a->is_f64 ? NULL : ray_vec_new(RAY_I64, 3 * (int64_t)n_chunks);
    if (!mins || RAY_IS_ERR(mins) || !maxs || RAY_IS_ERR(maxs) || !nbits || RAY_IS_ERR(nbits) ||
        (!a->is_f64 && (!aggs || RAY_IS_ERR(aggs)))) {
        if (mins && !RAY_IS_ERR(mins)) ray_release(mins);
        if (maxs && !RAY_IS_ERR(maxs)) ray_release(maxs);
        if (nbits && !RAY_IS_ERR(nbits)) ray_release(nbits);
        if (aggs && !RAY_IS_ERR(aggs)) ray_release(aggs);
        ray_release(idx); ray_zone_acc_free(a);
        return ray_error("oom", "chunk_zone: arrays alloc");
    }
    mins->len = n_chunks; maxs->len = n_chunks; nbits->len = nb_len;
    memset(ray_data(nbits), 0, (size_t)nb_len);
    if (a->is_f64) {
        memcpy(ray_data(mins), a->fmins, (size_t)n_chunks * 8);
        memcpy(ray_data(maxs), a->fmaxs, (size_t)n_chunks * 8);
    } else {
        memcpy(ray_data(mins), a->mins, (size_t)n_chunks * 8);
        memcpy(ray_data(maxs), a->maxs, (size_t)n_chunks * 8);
        aggs->len = 3 * (int64_t)n_chunks;
        int64_t* ag = (int64_t*)ray_data(aggs);
        for (uint32_t g = 0; g < n_chunks; g++) {
            ag[g] = (int64_t)a->sum_lo[g];
            ag[n_chunks + g] = a->nn[g];
            ag[2 * (int64_t)n_chunks + g] = a->sum_hi[g];
        }
    }
    uint8_t* nb = (uint8_t*)ray_data(nbits);
    for (uint32_t g = 0; g < n_chunks; g++) if (a->nulls[g]) nb[g >> 3] |= (uint8_t)(1u << (g & 7));
    ix->u.chunk_zone.mins = mins; ix->u.chunk_zone.maxs = maxs;
    ix->u.chunk_zone.null_bits = nbits; ix->u.chunk_zone.aggs = aggs;
    ray_zone_acc_free(a);
    return idx;
}

/* ── RAY_IDX_DICT: per-column string dictionary (codes + first-occ rows) ──
 * Deduplicates `v`'s strings into dense int32 codes; first_occ[c] is the row of
 * code c's first occurrence (so code -> string resolves through `v` itself).
 * Standalone RAY_INDEX object (caller releases / attaches).  STR only. */
ray_t* ray_index_dict_compute(ray_t* v) {
    if (!v || RAY_IS_ERR(v) || !ray_is_vec(v))
        return ray_error("type", "dict: compute needs a vector");
    if (v->type != RAY_STR)
        return ray_error("nyi", "dict: only RAY_STR vectors supported");
    int64_t n = v->len;
    if (n <= 0 || n > INT32_MAX)
        return ray_error("domain", "dict: row count out of int32 code range");

    ray_t* owner = (v->attrs & RAY_ATTR_SLICE) ? v->slice_parent : v;
    const ray_str_t* desc = (const ray_str_t*)ray_data(v);
    const char* pool = (owner && owner->str_pool && !RAY_IS_ERR(owner->str_pool))
                       ? (const char*)ray_data(owner->str_pool) : NULL;

    ray_t* codes = ray_vec_new(RAY_I32, n);
    if (!codes || RAY_IS_ERR(codes)) return codes ? codes : ray_error("oom", NULL);
    codes->len = n;
    int32_t* cd = (int32_t*)ray_data(codes);

    uint64_t cap = 32; while (cap < (uint64_t)n * 2) cap <<= 1;
    uint64_t mask = cap - 1;
    int32_t* slot = (int32_t*)ray_calloc_raw((size_t)((size_t)cap) * (sizeof(int32_t)));   /* code+1 */
    int32_t* focc = (int32_t*)ray_alloc_raw((size_t)n * sizeof(int32_t));    /* first-occ row */
    if (!slot || !focc) { ray_free_raw(slot); ray_free_raw(focc); ray_release(codes); return ray_error("oom", NULL); }

    int64_t ndist = 0;
    for (int64_t i = 0; i < n; i++) {
        const ray_str_t* d = &desc[i];
        uint64_t s = ray_str_t_hash(d, pool) & mask;
        for (;;) {
            int32_t cp1 = slot[s];
            if (cp1 == 0) {
                int32_t code = (int32_t)ndist++;
                focc[code] = (int32_t)i; slot[s] = code + 1; cd[i] = code; break;
            }
            int32_t code = cp1 - 1;
            if (ray_str_t_eq(&desc[focc[code]], pool, d, pool)) { cd[i] = code; break; }
            s = (s + 1) & mask;
        }
    }
    ray_free_raw(slot);

    ray_t* first_occ = ray_vec_new(RAY_I32, ndist > 0 ? ndist : 1);
    if (!first_occ || RAY_IS_ERR(first_occ)) { ray_free_raw(focc); ray_release(codes); return ray_error("oom", NULL); }
    first_occ->len = ndist;
    memcpy(ray_data(first_occ), focc, (size_t)ndist * sizeof(int32_t));
    ray_free_raw(focc);

    ray_t* idx = ray_index_alloc(RAY_IDX_DICT, v->type, n);
    if (!idx || RAY_IS_ERR(idx)) { ray_release(codes); ray_release(first_occ); return idx; }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.dict.codes      = codes;
    ix->u.dict.first_occ  = first_occ;
    ix->u.dict.n_distinct = ndist;
    return idx;
}

/* Attach an already-built standalone RAY_INDEX object to a column.  Takes
 * ownership of idx on success.  Zero-copy on a fresh rc=1 column. */
ray_t* ray_index_attach_built(ray_t** vp, ray_t* idx) {
    if (!idx || RAY_IS_ERR(idx) || idx->type != RAY_INDEX)
        return ray_error("type", "attach_built: not an index object");
    bool is_dict = (ray_index_payload(idx)->kind == RAY_IDX_DICT);
    bool is_hash = (ray_index_payload(idx)->kind == RAY_IDX_HASH);
    /* STR carries the dict and (since the vector-needle find work) the hash;
     * SYM carries the hash.  rc=1 → ray_cow no-op. */
    ray_t* v = prepare_attach_ex(vp, "index", is_dict || is_hash, is_hash);
    if (RAY_IS_ERR(v)) return v;
    return attach_finalize(v, idx);
}

ray_t* ray_index_attach_dict(ray_t** vp) {
    ray_t* v = prepare_attach_ex(vp, "dict", true, false);
    if (RAY_IS_ERR(v)) return v;
    ray_t* idx = ray_index_dict_compute(v);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("oom", NULL);
    return attach_finalize(v, idx);
}

/* ── Incremental STR dictionary ─────────────────────────────────────────────
 * Codes are assigned in first-occurrence order, exactly as
 * ray_index_dict_compute assigns them over the whole column, so the result
 * is the same index.  Distinct strings are copied once into a private pool;
 * the column itself is on disk by then.  There is no cardinality cap: the
 * accumulator holds 4 B/row of codes plus one copy of each distinct string,
 * less than the whole-column ray_index_dict_compute (codes + slots +
 * first_occ), and a high-cardinality dictionary still serves distinct. */
ray_err_t ray_dict_acc_init(ray_dict_acc_t* a) {
    memset(a, 0, sizeof(*a));
    a->mask = 1023;
    a->slot = (uint32_t*)ray_calloc_raw((size_t)(a->mask + 1) * sizeof(uint32_t));
    return a->slot ? RAY_OK : RAY_ERR_OOM;
}

void ray_dict_acc_free(ray_dict_acc_t* a) {
    ray_free_raw(a->codes); ray_free_raw(a->first_occ); ray_free_raw(a->offs);
    ray_free_raw(a->lens); ray_free_raw(a->pool); ray_free_raw(a->slot);
    bool dead = a->dead;
    memset(a, 0, sizeof(*a));
    a->dead = dead;
}

static ray_err_t dict_acc_rehash(ray_dict_acc_t* a) {
    uint64_t ncap = (a->mask + 1) * 2;
    uint32_t* ns = (uint32_t*)ray_calloc_raw((size_t)ncap * sizeof(uint32_t));
    if (!ns) return RAY_ERR_OOM;
    for (int64_t c = 0; c < a->n_distinct; c++) {
        uint64_t s = ray_hash_bytes(a->lens[c] ? a->pool + a->offs[c] : "", a->lens[c]) & (ncap - 1);
        while (ns[s]) s = (s + 1) & (ncap - 1);
        ns[s] = (uint32_t)c + 1;
    }
    ray_free_raw(a->slot); a->slot = ns; a->mask = ncap - 1;
    return RAY_OK;
}

ray_err_t ray_dict_acc_add(ray_dict_acc_t* a, ray_t* v) {
    if (a->dead) return RAY_OK;
    if (!v || RAY_IS_ERR(v) || v->type != RAY_STR) return RAY_ERR_TYPE;
    int64_t n = v->len;
    if (n <= 0) return RAY_OK;
    if (a->n_rows + n > INT32_MAX) { a->dead = true; ray_dict_acc_free(a); return RAY_OK; }
    if (a->n_rows + n > a->cap_rows) {
        int64_t cap = a->cap_rows ? a->cap_rows : 65536;
        while (cap < a->n_rows + n) cap *= 2;
        int32_t* p = (int32_t*)ray_realloc_raw(a->codes, (size_t)cap * sizeof(int32_t));
        if (!p) return RAY_ERR_OOM;
        a->codes = p; a->cap_rows = cap;
    }
    for (int64_t i = 0; i < n; i++) {
        if (RAY_UNLIKELY((i & 4095) == 0 && ray_interrupted())) return RAY_ERR_CANCEL;
        size_t len; const char* p = ray_str_vec_get(v, i, &len);
        uint64_t h = ray_hash_bytes(p, len);
        uint64_t s = h & a->mask;
        int32_t code = -1;
        for (;;) {
            uint32_t cp1 = a->slot[s];
            if (cp1 == 0) break;
            int32_t c = (int32_t)cp1 - 1;
            if (a->lens[c] == len && (len == 0 || memcmp(a->pool + a->offs[c], p, len) == 0)) { code = c; break; }
            s = (s + 1) & a->mask;
        }
        if (code < 0) {
            if (a->n_distinct == a->cap_distinct) {
                int64_t cap = a->cap_distinct ? a->cap_distinct * 2 : 1024;
                int32_t* f = (int32_t*)ray_realloc_raw(a->first_occ, (size_t)cap * sizeof(int32_t));
                uint64_t* o = f ? (uint64_t*)ray_realloc_raw(a->offs, (size_t)cap * sizeof(uint64_t)) : NULL;
                uint32_t* l = o ? (uint32_t*)ray_realloc_raw(a->lens, (size_t)cap * sizeof(uint32_t)) : NULL;
                if (f) a->first_occ = f;
                if (o) a->offs = o;
                if (l) a->lens = l;
                if (!f || !o || !l) return RAY_ERR_OOM;   /* grown parts stay owned by a */
                a->cap_distinct = cap;
            }
            if (len > 0) {
                if (a->pool_len + len > a->pool_cap) {
                    uint64_t cap = a->pool_cap ? a->pool_cap * 2 : 65536;
                    while (cap < a->pool_len + len) cap *= 2;
                    char* np = (char*)ray_realloc_raw(a->pool, (size_t)cap);
                    if (!np) return RAY_ERR_OOM;
                    a->pool = np; a->pool_cap = cap;
                }
                memcpy(a->pool + a->pool_len, p, len);
            }
            code = (int32_t)a->n_distinct++;
            a->offs[code] = a->pool_len; a->lens[code] = (uint32_t)len;
            a->pool_len += len;
            a->first_occ[code] = (int32_t)(a->n_rows + i);
            a->slot[s] = (uint32_t)code + 1;
            if ((uint64_t)a->n_distinct * 2 > a->mask + 1) {
                ray_err_t err = dict_acc_rehash(a);
                if (err != RAY_OK) return err;
            }
        }
        a->codes[a->n_rows + i] = code;
    }
    a->n_rows += n;
    return RAY_OK;
}

ray_t* ray_dict_acc_finish(ray_dict_acc_t* a, int64_t len) {
    if (a->dead || len < 65536 || a->n_rows != len) { ray_dict_acc_free(a); return NULL; }
    ray_t* codes = ray_vec_new(RAY_I32, len);
    ray_t* first_occ = ray_vec_new(RAY_I32, a->n_distinct > 0 ? a->n_distinct : 1);
    ray_t* idx = ray_index_alloc(RAY_IDX_DICT, RAY_STR, len);
    if (!codes || RAY_IS_ERR(codes) || !first_occ || RAY_IS_ERR(first_occ) || !idx || RAY_IS_ERR(idx)) {
        if (codes && !RAY_IS_ERR(codes)) ray_release(codes);
        if (first_occ && !RAY_IS_ERR(first_occ)) ray_release(first_occ);
        if (idx && !RAY_IS_ERR(idx)) ray_release(idx);
        ray_dict_acc_free(a);
        return ray_error("oom", NULL);
    }
    codes->len = len; memcpy(ray_data(codes), a->codes, (size_t)len * 4);
    first_occ->len = a->n_distinct; memcpy(ray_data(first_occ), a->first_occ, (size_t)a->n_distinct * 4);
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.dict.codes = codes; ix->u.dict.first_occ = first_occ; ix->u.dict.n_distinct = a->n_distinct;
    ray_dict_acc_free(a);
    return idx;
}

/* ── Inline on-disk index region (zero-copy mmap) ───────────────────────────
 * An attached index is persisted at the (32-aligned) tail of its column file as
 * a run of contiguous 32-byte-aligned ray_t blocks:
 *   [RAY_INDEX block: 32B hdr + ray_index_t payload][child vec block]…
 * The ray_index_t's child-pointer fields hold REGION-RELATIVE byte offsets on
 * disk; ray_index_inline_map patches them to absolute pointers in place after
 * mmap (one COW'd header page) and flags the index RAY_MARK_MMAP so the parent
 * column frees the whole region with one munmap and never frees children by
 * pointer.  All blocks are 32-aligned so each is a directly-usable ray_t. */
#define IDX_ALIGN32(n) (((n) + 31) & ~(int64_t)31)
#define IDX_HEAD_BYTES IDX_ALIGN32(32 + (int64_t)sizeof(ray_index_t))   /* the RAY_INDEX block */

/* Addresses of this kind's child ray_t* fields (so write/map read+patch them
 * uniformly).  Returns count 0..4. */
static int idx_child_slots(ray_index_t* ix, ray_t** slots[4]) {
    int n = 0;
    switch (ix->kind) {
    /* RAY_IDX_UKEY is runtime-only and lives on a table, which is never
     * written as a column, so it has nothing to persist.  Named rather
     * than left to the default so the omission reads as deliberate. */
    case RAY_IDX_UKEY: break;
    case RAY_IDX_HASH:
        slots[n++] = &ix->u.hash.table; slots[n++] = &ix->u.hash.gkeys;
        slots[n++] = &ix->u.hash.offs;  slots[n++] = &ix->u.hash.rows;  break;
    case RAY_IDX_SORT:
        slots[n++] = &ix->u.sort.perm; break;
    case RAY_IDX_BLOOM:
        slots[n++] = &ix->u.bloom.bits; break;
    case RAY_IDX_CHUNK_ZONE:
        slots[n++] = &ix->u.chunk_zone.mins;
        slots[n++] = &ix->u.chunk_zone.maxs;
        slots[n++] = &ix->u.chunk_zone.null_bits;
        /* 4th slot added after the first on-disk generation: older regions
         * hold zero there (the payload is zeroed at alloc), which maps to
         * NULL — no aggregates, nothing else changes. */
        slots[n++] = &ix->u.chunk_zone.aggs; break;
    case RAY_IDX_PART:
        slots[n++] = &ix->u.part.keys; slots[n++] = &ix->u.part.starts;
        slots[n++] = &ix->u.part.lens; break;
    case RAY_IDX_DICT:
        slots[n++] = &ix->u.dict.codes; slots[n++] = &ix->u.dict.first_occ; break;
    default: break;  /* RAY_IDX_ZONE: scalars only, no child vecs */
    }
    return n;
}

static int64_t idx_blk_bytes(const ray_t* v) {
    return IDX_ALIGN32(32 + (int64_t)v->len * ray_elem_size(v->type));
}

/* Total byte size of the inline region for `ix` (32-aligned blocks). */
int64_t ray_index_inline_size(const ray_index_t* ix) {
    int64_t total = IDX_ALIGN32(32 + (int64_t)sizeof(ray_index_t));  /* RAY_INDEX block */
    ray_t** slots[4];
    int nch = idx_child_slots((ray_index_t*)ix, slots);
    for (int i = 0; i < nch; i++) {
        ray_t* c = *slots[i];
        if (c && !RAY_IS_ERR(c)) total += idx_blk_bytes(c);
    }
    return total;
}

/* Persisted-index layout generation, carried in the RAY_INDEX block's
 * on-disk-free `order` byte (mirrors the column-header scheme in col.h).
 * The index region is a rebuildable ACCELERATOR: a mismatched generation is
 * simply ignored (column loads unindexed) rather than mis-decoded.
 * Generation 1 = the CSR grouped hash layout (table/gkeys/offs/rows);
 * generation-0 regions carried the retired chain layout. */
#define RAY_IDX_FORMAT_MAJOR ((uint8_t)1)
/* Generation 2: the same layout with the hash `table`, `offs` and `rows`
 * stored narrow (see hx_get).  Written only for indexes that carry a narrow
 * array, so a binary that knows generation 1 alone loads such a column
 * unindexed instead of misreading it; generation-1 regions read as before. */
#define RAY_IDX_FORMAT_NARROW ((uint8_t)2)
/* Generation 3: a hash table whose home slots are the top hash bits
 * (RAY_MARK_HASH_HIGH, groups in hash order); arrays narrow or I64 as in
 * generation 2.  Older binaries load such a column unindexed. */
#define RAY_IDX_FORMAT_HASH_HIGH ((uint8_t)3)

/* Hash index arrays whose values are group ids, positions or row ids
 * (`table`, `offs`, `rows`) fit 32 bits on any column below 2^32 rows; they
 * are stored as unsigned 32-bit values in an I32 vector when they do,
 * halving them.  Indexes built before, or too large, keep I64.  Read either
 * with hx_get; hand row-id slices out as ray_idx_rows_t (hx_rows). */
static inline int64_t hx_get(const ray_t* v, int64_t i) {
    const void* d = ray_data((ray_t*)v);
    return v->type == RAY_I32 ? (int64_t)((const uint32_t*)d)[i]
                              : ((const int64_t*)d)[i];
}
static inline void hx_set(ray_t* v, int64_t i, int64_t x) {
    void* d = ray_data(v);
    if (v->type == RAY_I32) ((uint32_t*)d)[i] = (uint32_t)x;
    else                    ((int64_t*)d)[i] = x;
}
/* Copy `n` entries of `src` into `dst`, converting between widths. */
static void hx_copy_into(ray_t* dst, const ray_t* src, int64_t n) {
    if (dst->type == src->type) {
        memcpy(ray_data(dst), ray_data((ray_t*)src), (size_t)n * (size_t)ray_elem_size(src->type));
        return;
    }
    for (int64_t i = 0; i < n; i++) hx_set(dst, i, hx_get(src, i));
}
static inline ray_idx_rows_t hx_rows(const ray_t* rows, int64_t off) {
    bool nw = rows->type == RAY_I32;
    const uint8_t* d = (const uint8_t*)ray_data((ray_t*)rows);
    return (ray_idx_rows_t){ d + off * (nw ? 4 : 8), nw };
}
/* Copy `n` row ids from position `off` as int64. */
static void hx_copy_rows(int64_t* dst, const ray_t* rows, int64_t off, int64_t n) {
    if (rows->type != RAY_I32) {
        memcpy(dst, (const int64_t*)ray_data((ray_t*)rows) + off, (size_t)n * sizeof(int64_t));
        return;
    }
    const uint32_t* src = (const uint32_t*)ray_data((ray_t*)rows) + off;
    for (int64_t i = 0; i < n; i++) dst[i] = (int64_t)src[i];
}
/* Home slot of mixed hash `h` in a table of `mask + 1` slots: its top bits
 * when the table is RAY_MARK_HASH_HIGH, else its low bits. */
static inline uint64_t hx_home_m(uint8_t markers, uint64_t mask, uint64_t h) {
    return (markers & RAY_MARK_HASH_HIGH) ? h >> __builtin_clzll(mask) : h & mask;
}
static inline uint64_t hx_home(const ray_index_t* ix, uint64_t h) {
    return hx_home_m(ix->markers, ix->u.hash.mask, h);
}
static inline bool hx_narrow_ok(const ray_t* v) {
    return v && !RAY_IS_ERR(v) && (v->type == RAY_I64 || v->type == RAY_I32);
}
typedef struct { const int64_t* src; uint32_t* dst; } hx_narrow_ctx_t;
static void hx_narrow_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    hx_narrow_ctx_t* c = (hx_narrow_ctx_t*)raw;
    for (int64_t i = start; i < end; i++) c->dst[i] = (uint32_t)c->src[i];
}
/* The narrow copy of an I64 array whose values all lie in [0, bound]
 * (bound below 2^32, known to the caller), consuming `v`; `v` itself when it
 * cannot be narrowed or the copy cannot be allocated. */
static ray_t* hx_narrow(ray_t* v, int64_t bound) {
    if (!v || RAY_IS_ERR(v) || v->type != RAY_I64 || bound < 0 || bound > (int64_t)UINT32_MAX) return v;
    ray_t* n = ray_vec_new(RAY_I32, v->len > 0 ? v->len : 1);
    if (!n || RAY_IS_ERR(n)) { if (n) ray_error_free(n); return v; }
    n->len = v->len;
    hx_narrow_ctx_t c = { (const int64_t*)ray_data(v), (uint32_t*)ray_data(n) };
    ray_pool_t* pool = ray_pool_get();
    if (ray_pool_par_dispatch_ok(pool, v->len, RAY_PARALLEL_THRESHOLD)) {
        ray_pool_dispatch(pool, hx_narrow_fn, &c, v->len);
        /* An interrupt makes the dispatch skip its tasks, leaving the copy
         * unfilled: keep the wide array. */
        if (atomic_load_explicit(&pool->cancelled, memory_order_acquire)) {
            ray_release(n);
            return v;
        }
    } else {
        hx_narrow_fn(&c, 0, 0, v->len);
    }
    ray_release(v);
    return n;
}
/* Narrow a freshly built hash payload in place. */
static void hx_narrow_payload(ray_index_t* ix) {
    ix->u.hash.table = hx_narrow(ix->u.hash.table, ix->u.hash.n_groups);
    ix->u.hash.offs  = hx_narrow(ix->u.hash.offs,  ix->u.hash.n_keys);
    ix->u.hash.rows  = hx_narrow(ix->u.hash.rows,  ix->built_for_len);
}
static uint8_t idx_format_of(const ray_index_t* ix) {
    if (ix->kind == RAY_IDX_HASH && (ix->markers & RAY_MARK_HASH_HIGH))
        return RAY_IDX_FORMAT_HASH_HIGH;
    if (ix->kind == RAY_IDX_HASH) {
        const ray_t* a[3] = { ix->u.hash.table, ix->u.hash.offs, ix->u.hash.rows };
        for (int i = 0; i < 3; i++)
            if (a[i] && !RAY_IS_ERR(a[i]) && a[i]->type == RAY_I32)
                return RAY_IDX_FORMAT_NARROW;
    }
    return RAY_IDX_FORMAT_MAJOR;
}

/* Serialize `ix` into `dst` (>= ray_index_inline_size bytes, pre-zeroed by the
 * caller for clean 32-pad).  Child pointers become region-relative offsets. */
void ray_index_inline_write(uint8_t* dst, const ray_index_t* ix) {
    ray_t blkhdr;
    memset(&blkhdr, 0, 32);
    blkhdr.type = RAY_INDEX; blkhdr.len = (int64_t)sizeof(ray_index_t);
    blkhdr.mmod = 1; blkhdr.rc = 1;
    blkhdr.order = idx_format_of(ix);
    memcpy(dst, &blkhdr, 32);

    ray_index_t* on = (ray_index_t*)(dst + 32);
    memcpy(on, ix, sizeof(ray_index_t));   /* scalars + child ptrs (overwritten below) */
    on->markers |= RAY_MARK_MMAP;           /* on-disk indexes are always mmap-resident */

    ray_t** src_slots[4];
    ray_t** on_slots[4];
    int nch = idx_child_slots((ray_index_t*)ix, src_slots);
    idx_child_slots(on, on_slots);
    int64_t off = IDX_ALIGN32(32 + (int64_t)sizeof(ray_index_t));
    for (int i = 0; i < nch; i++) {
        ray_t* c = *src_slots[i];
        if (!c || RAY_IS_ERR(c)) { *on_slots[i] = NULL; continue; }
        ray_t chdr;
        memcpy(&chdr, c, 32);
        chdr.mmod = 1; chdr.rc = 1;
        memcpy(dst + off, &chdr, 32);
        size_t dbytes = (size_t)c->len * ray_elem_size(c->type);
        if (dbytes) memcpy(dst + off + 32, ray_data(c), dbytes);
        *on_slots[i] = (ray_t*)(intptr_t)off;   /* region-relative offset */
        off += idx_blk_bytes(c);
    }
}

/* The head block of `ix`'s region (IDX_HEAD_BYTES at `head`): the RAY_INDEX
 * block with the child pointers turned into region offsets, the children
 * following it in slot order. */
static void idx_inline_head(uint8_t* head, const ray_index_t* ix) {
    memset(head, 0, (size_t)IDX_HEAD_BYTES);
    ray_t blkhdr;
    memset(&blkhdr, 0, 32);
    blkhdr.type = RAY_INDEX; blkhdr.len = (int64_t)sizeof(ray_index_t);
    blkhdr.mmod = 1; blkhdr.rc = 1;
    blkhdr.order = idx_format_of(ix);
    memcpy(head, &blkhdr, 32);
    ray_index_t* on = (ray_index_t*)(head + 32);
    memcpy(on, ix, sizeof(ray_index_t));
    on->markers |= RAY_MARK_MMAP;

    ray_t** src_slots[4];
    ray_t** on_slots[4];
    int nch = idx_child_slots((ray_index_t*)ix, src_slots);
    idx_child_slots(on, on_slots);
    int64_t off = IDX_HEAD_BYTES;
    for (int i = 0; i < nch; i++) {
        ray_t* c = *src_slots[i];
        if (!c || RAY_IS_ERR(c)) { *on_slots[i] = NULL; continue; }
        *on_slots[i] = (ray_t*)(intptr_t)off;   /* region-relative offset */
        off += idx_blk_bytes(c);
    }
}

bool ray_index_inline_write_file(FILE* f, const ray_index_t* ix) {
    static const uint8_t zeros[32] = {0};
    _Alignas(32) uint8_t head[IDX_HEAD_BYTES];
    idx_inline_head(head, ix);
    ray_t** src_slots[4];
    int nch = idx_child_slots((ray_index_t*)ix, src_slots);
    if (fwrite(head, 1, sizeof(head), f) != sizeof(head)) return false;
    for (int i = 0; i < nch; i++) {
        ray_t* c = *src_slots[i];
        if (!c || RAY_IS_ERR(c)) continue;
        ray_t chdr;
        memcpy(&chdr, c, 32);
        chdr.mmod = 1; chdr.rc = 1;
        if (fwrite(&chdr, 1, 32, f) != 32) return false;
        size_t dbytes = (size_t)c->len * ray_elem_size(c->type);
        if (dbytes && fwrite(ray_data(c), 1, dbytes, f) != dbytes) return false;
        size_t pad = (size_t)idx_blk_bytes(c) - 32 - dbytes;
        if (pad && fwrite(zeros, 1, pad, f) != pad) return false;
    }
    return true;
}

/* Map an mmap'd inline region in place: patch child offsets to absolute
 * pointers and return the RAY_INDEX object (already RAY_MARK_MMAP).  `region`
 * points at the start of the index region within the column's file mapping.
 * Returns NULL for a stale layout generation or a payload-size mismatch —
 * the caller loads the column unindexed (the index is rebuildable). */
ray_t* ray_index_inline_map(uint8_t* region, int64_t region_size) {
    int64_t head = IDX_ALIGN32(32 + (int64_t)sizeof(ray_index_t));
    if (region_size < head) return NULL;
    ray_t* idx = (ray_t*)region;
    if (idx->order != RAY_IDX_FORMAT_MAJOR && idx->order != RAY_IDX_FORMAT_NARROW &&
        idx->order != RAY_IDX_FORMAT_HASH_HIGH) return NULL;
    if (idx->len != (int64_t)sizeof(ray_index_t)) return NULL;
    ray_index_t* ix = ray_index_payload(idx);
    ray_t** slots[4];
    int nch = idx_child_slots(ix, slots);
    for (int i = 0; i < nch; i++) {
        int64_t o = (int64_t)(intptr_t)(*slots[i]);
        ray_t* c = NULL;
        /* A child must lie inside the region: a region re-saved by a
         * binary that knows fewer child slots keeps a stale offset in a
         * slot it did not write. */
        if (o >= head && o <= region_size - 32) {
            ray_t* cand = (ray_t*)(region + o);
            int64_t esz = ray_elem_size(cand->type);
            if (cand->len >= 0 && esz > 0 &&
                cand->len <= (region_size - o - 32) / esz)
                c = cand;
        }
        if (o && !c) {
            /* The chunk-zone aggregates are optional; any other child out
             * of bounds means the region cannot be trusted. */
            if (ix->kind == RAY_IDX_CHUNK_ZONE && slots[i] == &ix->u.chunk_zone.aggs) {
                *slots[i] = NULL;
                continue;
            }
            return NULL;
        }
        *slots[i] = c;
    }
    /* Hash arrays: I64 (generation 1), or I32 for the narrow ones. */
    if (ix->kind == RAY_IDX_HASH &&
        ((ix->u.hash.table && !hx_narrow_ok(ix->u.hash.table)) ||
         (ix->u.hash.offs  && !hx_narrow_ok(ix->u.hash.offs))  ||
         (ix->u.hash.rows  && !hx_narrow_ok(ix->u.hash.rows))  ||
         (ix->u.hash.gkeys && ix->u.hash.gkeys->type != RAY_I64) ||
         (idx->order == RAY_IDX_FORMAT_MAJOR &&
          ((ix->u.hash.table && ix->u.hash.table->type != RAY_I64) ||
           (ix->u.hash.offs  && ix->u.hash.offs->type  != RAY_I64) ||
           (ix->u.hash.rows  && ix->u.hash.rows->type  != RAY_I64)))))
        return NULL;
    /* Generation 3 is exactly the hash tables with top-bit homes; their
     * home shift comes from the mask, so it must match the table. */
    bool high = ix->kind == RAY_IDX_HASH && (ix->markers & RAY_MARK_HASH_HIGH);
    if (high != (idx->order == RAY_IDX_FORMAT_HASH_HIGH)) return NULL;
    if (high && (ix->u.hash.mask < 7 || (ix->u.hash.mask & (ix->u.hash.mask + 1)) ||
                 !ix->u.hash.table ||
                 (uint64_t)ix->u.hash.table->len != ix->u.hash.mask + 1))
        return NULL;
    /* two layouts: [lo | nn] (2 per chunk) and [lo | nn | hi] (3 per chunk) */
    if (ix->kind == RAY_IDX_CHUNK_ZONE && ix->u.chunk_zone.aggs &&
        (ix->u.chunk_zone.aggs->type != RAY_I64 || ix->u.chunk_zone.is_f64 ||
         (ix->u.chunk_zone.aggs->len != 2 * (int64_t)ix->u.chunk_zone.n_chunks &&
          ix->u.chunk_zone.aggs->len != 3 * (int64_t)ix->u.chunk_zone.n_chunks)))
        ix->u.chunk_zone.aggs = NULL;
    ix->markers |= RAY_MARK_MMAP;
    idx->mmod = 1;
    return idx;
}

/* --------------------------------------------------------------------------
 * Hash index — CSR grouped layout
 *
 * Build: every non-null row gets a group id from an open-addressing probe on
 * its i64-canonical key (SYM: column-domain id), and the row ids are laid out
 * as contiguous ascending slices (CSR: offs[] + rows[]).  Numeric and SYM
 * columns use the partitioned builder below; STR keeps a serial walk.  The
 * persisted table is sized by DISTINCT keys — it indexes groups, not rows,
 * so a high-duplication column (the whole point of a grouped index) keeps a
 * tiny footprint.
 *
 * Lookup `k`: slot-walk table from hx_home(mix64(k)) comparing
 * gkeys[gid] == k; a hit yields the contiguous ascending slice
 * rows[offs[gid]..offs[gid+1]).
 * -------------------------------------------------------------------------- */

/* ── Hash-index build trace ── */
ray_hash_mark_t ray_hash_trace_mark(void) {
    ray_hash_mark_t m = { ray_profile_now_ns(), 0, 0, 0, 0 };
#if defined(__linux__)
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) m.majflt = ru.ru_majflt;
    FILE* f = fopen("/proc/self/io", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            long long x;
            if (sscanf(line, "read_bytes: %lld", &x) == 1) m.rd = x;
            else if (sscanf(line, "write_bytes: %lld", &x) == 1) m.wr = x;
            else if (sscanf(line, "cancelled_write_bytes: %lld", &x) == 1) m.cwr = x;
        }
        fclose(f);
    }
#endif
    return m;
}

void ray_hash_trace_add(ray_hash_trace_t* t, int ph, ray_hash_mark_t since, int64_t moved) {
    if (!t || ph < 0 || ph >= RAY_HXT_N) return;
    ray_hash_mark_t now = ray_hash_trace_mark();
    t->ns[ph] += now.ns - since.ns;
    t->majflt[ph] += now.majflt - since.majflt;
    t->rd[ph] += now.rd - since.rd;
    t->wr[ph] += now.wr - since.wr;
    t->cwr[ph] += now.cwr - since.cwr;
    t->moved[ph] += moved;
}

void ray_hash_trace_print(FILE* f, const char* what, const ray_hash_trace_t* t, int64_t col_bytes) {
    static const char* names[RAY_HXT_N] = {
        "count", "bucket", "distinct", "groups", "table", "append" };
    int64_t moved = 0;
    fprintf(f, "%s:", what);
    for (int p = 0; p < RAY_HXT_N; p++) {
        if (!t->ns[p] && !t->moved[p]) continue;
        moved += t->moved[p];
        fprintf(f, " %s=%.1fms/%lldflt/%.1fMBr/%.1fMBw/%.1fMBcw/moved=%.1fMB(%.2fx)", names[p],
                (double)t->ns[p] / 1e6, (long long)t->majflt[p],
                (double)t->rd[p] / 1048576.0, (double)t->wr[p] / 1048576.0,
                (double)t->cwr[p] / 1048576.0, (double)t->moved[p] / 1048576.0,
                col_bytes > 0 ? (double)t->moved[p] / (double)col_bytes : 0.0);
    }
    fprintf(f, " total_moved=%.1fMB(%.2fx)\n", (double)moved / 1048576.0,
            col_bytes > 0 ? (double)moved / (double)col_bytes : 0.0);
}

/* Partitioned build of the CSR hash layout for numeric / SYM keys.
 *
 * Groups are numbered in ascending order of mix64(key word) — for these key
 * types distinct keys have distinct words, so the order is strict — and a
 * group's home slot is the TOP bits of that hash (RAY_MARK_HASH_HIGH).  Both
 * follow from the data alone, so the arrays are the same bytes whatever the
 * partition count, memory budget or core count, and every output array is
 * written front to back:
 *
 *   1  row ranges: key words and row ids bucketed by the top hash bits into
 *      contiguous partitions (rows stay ascending inside each);
 *   2  the number of groups, when the arrays are to be laid out at their
 *      final size before they are written (one batch: its own dedupe;
 *      several, into a persisted region: a counting pass per partition);
 *   3  partitions in batches that fit the memory budget: per partition a
 *      local dedupe, its groups sorted by hash, then its keys, row offsets
 *      and row slices stored at the partition's place in gkeys/offs/rows —
 *      a partition's groups follow every group of the partitions before it —
 *      and the batch's key words and row ids punched out of their spill
 *      files when they spilled;
 *   4  the slot table: groups in order, each in the first free slot from its
 *      home.  Homes ascend with the group number, so this is one forward
 *      sweep (a run past the last slot continues from slot 0) and gives
 *      exactly the table that inserting the groups in order would; with the
 *      size known it runs batch by batch in step 3.
 *
 * Only a batch of partitions is accessed at random; the bucketed input and
 * the outputs are streamed, so past the spill watermark they page through
 * their files sequentially instead of thrashing.  Returns 1 on success,
 * -1 when out of memory or interrupted (nothing left allocated; a region's
 * memory holds no complete index). */
#define HB_ROW_BYTES   128         /* batch working set per row, worst case in heap blocks */
#define HB_PART_ROWS   (1 << 17)   /* rows per partition: its dedupe table stays cache-sized */
#define HB_MAX_PARTS   4096
#define HB_MAX_TASKS   256

typedef struct {
    uint32_t* tab;      /* slot -> local group + 1 */
    uint64_t  tmask;
    uint64_t* gk;       /* [cap] key word per local group */
    int64_t*  gc;       /* [cap] rows per local group, then its row cursor */
    uint32_t* ord;      /* [ng] local groups in ascending hash order */
    uint32_t* lg;       /* [rows] local group of each of the partition's rows */
    int64_t   ng, cap;
    int64_t   g0;       /* number of the partition's first group */
} hb_part_t;

typedef struct {
    ray_t*          v;
    const uint8_t*  base;
    int64_t         n;
    int64_t         n_tasks;
    int64_t         n_part;
    int             part_shift;    /* partition = mix64(key) >> part_shift */
    bool            wide;          /* row ids past 32 bits */
    int64_t*        cnt;           /* [n_tasks * n_part] rows per (task, partition), then cursors */
    int64_t*        part_off;      /* [n_part + 1] */
    uint64_t*       kw;            /* [n_keys] key words, bucketed */
    void*           rid;           /* [n_keys] row ids, bucketed (u32 or i64) */
    int64_t*        gk;            /* gkeys data */
    ray_t*          offs;
    ray_t*          rows;
    int64_t         b_lo;          /* first partition of the batch */
    int64_t         part_cap0;     /* rows of a batch: a larger partition is alone in its batch */
    hb_part_t*      part;          /* [batch] */
    int64_t*        part_ng;       /* [n_part] groups per partition, once counted; else NULL */
    bool            counting;      /* the dedupe only counts its groups into part_ng */
    _Atomic(bool)   oom;
} hash_bld_t;

static inline int64_t hb_task_lo(const hash_bld_t* h, int64_t t) { return h->n * t / h->n_tasks; }
static inline int64_t hb_rid(const hash_bld_t* h, int64_t k) {
    return h->wide ? ((const int64_t*)h->rid)[k] : (int64_t)((const uint32_t*)h->rid)[k];
}

static void hb_count(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    hash_bld_t* h = (hash_bld_t*)raw;
    int64_t lo = hb_task_lo(h, start), hi = hb_task_lo(h, start + 1);
    int64_t* cnt = h->cnt + start * h->n_part;
    for (int64_t i = lo; i < hi; i++) {
        if (ray_vec_is_null(h->v, i)) continue;
        cnt[mix64(hash_row_key_word(h->v, h->base, i)) >> h->part_shift]++;
    }
}

static void hb_bucket(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    hash_bld_t* h = (hash_bld_t*)raw;
    int64_t lo = hb_task_lo(h, start), hi = hb_task_lo(h, start + 1);
    int64_t* cur = h->cnt + start * h->n_part;
    for (int64_t i = lo; i < hi; i++) {
        if (ray_vec_is_null(h->v, i)) continue;
        uint64_t k = hash_row_key_word(h->v, h->base, i);
        int64_t j = cur[mix64(k) >> h->part_shift]++;
        h->kw[j] = k;
        if (h->wide) ((int64_t*)h->rid)[j] = i;
        else         ((uint32_t*)h->rid)[j] = (uint32_t)i;
    }
}

static void hb_part_free(hb_part_t* s) {
    ray_free_raw(s->tab); ray_free_raw(s->gk); ray_free_raw(s->gc); ray_free_raw(s->ord);
    ray_free_raw(s->lg);
    memset(s, 0, sizeof(*s));
}

/* Size the group arrays for `cap` groups and re-place the groups so far. */
static bool hb_part_reserve(hb_part_t* s, int64_t cap) {
    uint64_t tcap = next_pow2((uint64_t)cap * 2);
    uint64_t* gk = (uint64_t*)ray_alloc_raw((size_t)cap * sizeof(uint64_t));
    int64_t*  gc = (int64_t*)ray_alloc_raw((size_t)cap * sizeof(int64_t));
    uint32_t* tab = (uint32_t*)ray_alloc_raw((size_t)tcap * sizeof(uint32_t));
    if (!gk || !gc || !tab) { ray_free_raw(gk); ray_free_raw(gc); ray_free_raw(tab); return false; }
    memset(tab, 0, (size_t)tcap * sizeof(uint32_t));
    if (s->ng) {
        memcpy(gk, s->gk, (size_t)s->ng * sizeof(uint64_t));
        memcpy(gc, s->gc, (size_t)s->ng * sizeof(int64_t));
    }
    for (int64_t g = 0; g < s->ng; g++) {
        uint64_t slot = mix64(gk[g]) & (tcap - 1);
        while (tab[slot]) slot = (slot + 1) & (tcap - 1);
        tab[slot] = (uint32_t)(g + 1);
    }
    ray_free_raw(s->gk); ray_free_raw(s->gc); ray_free_raw(s->tab);
    s->gk = gk; s->gc = gc; s->tab = tab; s->tmask = tcap - 1; s->cap = cap;
    return true;
}

static inline int64_t hb_part_find(const hb_part_t* s, uint64_t k) {
    uint64_t slot = mix64(k) & s->tmask;
    for (;;) {
        uint32_t g1 = s->tab[slot];
        if (s->gk[g1 - 1] == k) return g1 - 1;   /* every probed key is present */
        slot = (slot + 1) & s->tmask;
    }
}

/* LSD radix sort of `n` hashes with their group payload; constant digits
 * (the partition's own top bits among them) are skipped. */
static void hb_sort(uint64_t* k, uint32_t* v, uint64_t* tk, uint32_t* tv, int64_t n) {
    if (n < 64) {
        for (int64_t i = 1; i < n; i++) {
            uint64_t x = k[i]; uint32_t y = v[i]; int64_t j = i;
            for (; j > 0 && k[j - 1] > x; j--) { k[j] = k[j - 1]; v[j] = v[j - 1]; }
            k[j] = x; v[j] = y;
        }
        return;
    }
    int64_t hist[8][256];
    memset(hist, 0, sizeof(hist));
    for (int64_t i = 0; i < n; i++)
        for (int d = 0; d < 8; d++) hist[d][(k[i] >> (8 * d)) & 0xFF]++;
    uint64_t *sk = k, *dk = tk;
    uint32_t *sv = v, *dv = tv;
    for (int d = 0; d < 8; d++) {
        int64_t* hd = hist[d];
        if (hd[(sk[0] >> (8 * d)) & 0xFF] == n) continue;
        int64_t run = 0;
        for (int b = 0; b < 256; b++) { int64_t c = hd[b]; hd[b] = run; run += c; }
        for (int64_t i = 0; i < n; i++) {
            int64_t j = hd[(sk[i] >> (8 * d)) & 0xFF]++;
            dk[j] = sk[i]; dv[j] = sv[i];
        }
        uint64_t* t1 = sk; sk = dk; dk = t1;
        uint32_t* t2 = sv; sv = dv; dv = t2;
    }
    if (sk != k) {
        memcpy(k, sk, (size_t)n * sizeof(uint64_t));
        memcpy(v, sv, (size_t)n * sizeof(uint32_t));
    }
}

/* Batch task: dedupe partition b_lo + start, then order its groups by hash
 * (counting: only record how many groups it has). */
static void hb_dedupe(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    hash_bld_t* h = (hash_bld_t*)raw;
    hb_part_t* s = &h->part[start];
    int64_t p = h->b_lo + start;
    int64_t lo = h->part_off[p], hi = h->part_off[p + 1];
    if (hi == lo) return;
    /* groups are at most the rows; a partition larger than a batch (a hot
     * key's) starts at the batch size and grows.  Counted before, it is
     * sized for its groups exactly. */
    int64_t cap0 = hi - lo < h->part_cap0 ? hi - lo : h->part_cap0;
    if (h->part_ng && !h->counting) cap0 = h->part_ng[p] > 0 ? h->part_ng[p] : 1;
    /* each row's group, kept for the row pass when the partition fits a
     * batch; past it the row pass probes again */
    if (!h->counting && hi - lo <= h->part_cap0) {
        s->lg = (uint32_t*)ray_alloc_raw((size_t)(hi - lo) * sizeof(uint32_t));
        if (!s->lg) goto oom;
    }
    if (!hb_part_reserve(s, cap0)) goto oom;
    for (int64_t j = lo; j < hi; j++) {
        if (RAY_UNLIKELY(((j - lo) & 0xFFFF) == 0xFFFF && ray_interrupted())) return;
        uint64_t k = h->kw[j];
        uint64_t slot = mix64(k) & s->tmask;
        for (;;) {
            uint32_t g1 = s->tab[slot];
            if (g1 == 0) {
                if (s->ng == s->cap) {
                    if (s->cap >= (int64_t)UINT32_MAX / 2 || !hb_part_reserve(s, s->cap * 2)) goto oom;
                    slot = mix64(k) & s->tmask;
                    continue;
                }
                s->tab[slot] = (uint32_t)(s->ng + 1);
                s->gk[s->ng] = k;
                s->gc[s->ng] = 1;
                if (s->lg) s->lg[j - lo] = (uint32_t)s->ng;
                s->ng++;
                break;
            }
            if (s->gk[g1 - 1] == k) {
                s->gc[g1 - 1]++;
                if (s->lg) s->lg[j - lo] = g1 - 1;
                break;
            }
            slot = (slot + 1) & s->tmask;
        }
    }
    if (h->counting) {
        h->part_ng[p] = s->ng;
        hb_part_free(s);
        return;
    }
    {
        int64_t ng = s->ng;
        uint64_t* hs = (uint64_t*)ray_alloc_raw((size_t)ng * sizeof(uint64_t));
        uint64_t* th = (uint64_t*)ray_alloc_raw((size_t)ng * sizeof(uint64_t));
        uint32_t* tv = (uint32_t*)ray_alloc_raw((size_t)ng * sizeof(uint32_t));
        s->ord = (uint32_t*)ray_alloc_raw((size_t)ng * sizeof(uint32_t));
        if (!hs || !th || !tv || !s->ord) {
            ray_free_raw(hs); ray_free_raw(th); ray_free_raw(tv);
            goto oom;
        }
        for (int64_t g = 0; g < ng; g++) { hs[g] = mix64(s->gk[g]); s->ord[g] = (uint32_t)g; }
        hb_sort(hs, s->ord, th, tv, ng);
        ray_free_raw(hs); ray_free_raw(th); ray_free_raw(tv);
    }
    return;
oom:
    atomic_store_explicit(&h->oom, true, memory_order_relaxed);
}

/* Batch task: store partition b_lo + start's keys, row offsets and rows. */
static void hb_emit(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    hash_bld_t* h = (hash_bld_t*)raw;
    hb_part_t* s = &h->part[start];
    int64_t p = h->b_lo + start;
    int64_t lo = h->part_off[p], hi = h->part_off[p + 1];
    int64_t r = lo;
    for (int64_t j = 0; j < s->ng; j++) {
        uint32_t g = s->ord[j];
        h->gk[s->g0 + j] = (int64_t)s->gk[g];
        hx_set(h->offs, s->g0 + j, r);
        int64_t c = s->gc[g];
        s->gc[g] = r;
        r += c;
    }
    /* rows ascend inside the partition, so inside each group too */
    for (int64_t j = lo; j < hi; j++)
        hx_set(h->rows, s->gc[s->lg ? (int64_t)s->lg[j - lo] : hb_part_find(s, h->kw[j])]++,
               hb_rid(h, j));
    hb_part_free(s);
}

/* An interrupt makes a dispatch skip its tasks, so the outputs may be
 * incomplete: stop on either signal. */
static bool hb_stopped(ray_pool_t* pool) {
    return ray_interrupted() ||
           (pool && atomic_load_explicit(&pool->cancelled, memory_order_acquire));
}

/* Is an array of `len` entries of `es` bytes, allocated for `cap`, worth
 * cutting to size? */
static bool hb_cut(int64_t len, int64_t cap, size_t es) {
    size_t slack = (size_t)(cap - len) * es;
    return !(slack < ((size_t)1 << 20) || (2 * len > cap && slack < ((size_t)64 << 20)));
}

/* `v` (room for `cap` entries, `len` used) cut to size when the slack is
 * worth a copy; `v` itself when it is not or the copy cannot be made. */
static ray_t* hb_trim(ray_t* v, int64_t len, int64_t cap) {
    size_t es = (size_t)ray_elem_size(v->type);
    if (!hb_cut(len, cap, es)) return v;
    ray_t* t = ray_vec_new(v->type, len > 0 ? len : 1);
    if (!t || RAY_IS_ERR(t)) { if (t) ray_error_free(t); return v; }
    t->len = len;
    memcpy(ray_data(t), ray_data(v), (size_t)len * es);
    ray_release(v);
    return t;
}

static void hb_zero(ray_t* v, int64_t lo, int64_t hi) {
    if (hi <= lo) return;
    size_t es = (size_t)ray_elem_size(v->type);
    memset((uint8_t*)ray_data(v) + (size_t)lo * es, 0, (size_t)(hi - lo) * es);
}

/* Where the build puts its four arrays: heap vectors (`map` NULL), or the
 * persisted region itself — `map(ctx, bytes)` hands back the region's
 * zero-filled memory once its size is known, laid out as
 * ray_index_inline_write lays an index out (the head block, then the
 * table, gkeys, offs and rows blocks), and the arrays are the block bodies. */
typedef struct {
    ray_index_region_fn map;
    void*     ctx;
    uint8_t*  base;      /* the region: map's result */
    bool      zeroed;    /* the arrays start zero-filled */
    ray_err_t err;       /* why the build failed when map did */
    ray_t    *table, *gkeys, *offs, *rows;
} hb_dst_t;

/* The order a heap block for `bytes` of vector data gets.  A vector's
 * header carries it, and the region keeps each array's header. */
static uint8_t hb_alloc_order(size_t bytes) {
    uint8_t o = ray_order_for_size(bytes);
    return o >= RAY_HEAP_POOL_ORDER ? (uint8_t)RAY_ORDER_DIRECT : o;
}

/* The block header at `at`: what ray_index_inline_write stores for a heap
 * vector of `type` holding `len` entries in room for `cap`. */
static ray_t* hb_region_vec(uint8_t* at, int8_t type, int64_t len, int64_t cap) {
    ray_t* b = (ray_t*)at;
    memset(b, 0, 32);
    b->mmod = 1; b->rc = 1;
    b->order = hb_alloc_order((size_t)cap * (size_t)ray_elem_size(type));
    b->type = type;
    b->len = len;
    return b;
}

static void hb_dst_drop(hb_dst_t* d) {
    ray_t** a[4] = { &d->table, &d->gkeys, &d->offs, &d->rows };
    for (int i = 0; i < 4; i++) {
        if (!d->map && *a[i]) {
            if (RAY_IS_ERR(*a[i])) ray_error_free(*a[i]); else ray_release(*a[i]);
        }
        *a[i] = NULL;
    }
}

/* The arrays of `n_keys` keys in `n_groups` groups and a table of `cap`
 * slots.  Each is allocated at the capacity the in-memory build ends with
 * (gkeys and offs sized for the keys and cut to the groups when that pays,
 * hb_trim), so a region written in place holds the bytes a heap-built
 * index persists. */
static bool hb_dst_open(hb_dst_t* d, int8_t aw, int64_t n_keys, int64_t n_groups, uint64_t cap) {
    size_t es = (size_t)ray_elem_size(aw);
    int64_t gcap = hb_cut(n_groups, n_keys, 8) ? n_groups : n_keys;
    int64_t ocap = hb_cut(n_groups + 1, n_keys + 1, es) ? n_groups + 1 : n_keys + 1;
    int64_t rcap = n_keys > 0 ? n_keys : 1;
    if (gcap < 1) gcap = 1;
    if (!d->map) {
        d->table = ray_vec_new(aw, (int64_t)cap);
        d->gkeys = ray_vec_new(RAY_I64, gcap);
        d->offs  = ray_vec_new(aw, ocap);
        d->rows  = ray_vec_new(aw, rcap);
        if (!d->table || RAY_IS_ERR(d->table) || !d->gkeys || RAY_IS_ERR(d->gkeys) ||
            !d->offs || RAY_IS_ERR(d->offs) || !d->rows || RAY_IS_ERR(d->rows)) {
            hb_dst_drop(d);
            return false;
        }
        d->table->len = (int64_t)cap;
        d->gkeys->len = n_groups;
        d->offs->len  = n_groups + 1;
        d->rows->len  = n_keys;
        return true;
    }
    int64_t tb = IDX_ALIGN32(32 + (int64_t)cap * (int64_t)es);
    int64_t gb = IDX_ALIGN32(32 + n_groups * 8);
    int64_t ob = IDX_ALIGN32(32 + (n_groups + 1) * (int64_t)es);
    int64_t rb = IDX_ALIGN32(32 + n_keys * (int64_t)es);
    d->base = d->map(d->ctx, IDX_HEAD_BYTES + tb + gb + ob + rb);
    if (!d->base) { d->err = RAY_ERR_IO; return false; }
    d->zeroed = true;
    uint8_t* at = d->base + IDX_HEAD_BYTES;
    d->table = hb_region_vec(at, aw, (int64_t)cap, (int64_t)cap); at += tb;
    d->gkeys = hb_region_vec(at, RAY_I64, n_groups, gcap);        at += gb;
    d->offs  = hb_region_vec(at, aw, n_groups + 1, ocap);         at += ob;
    d->rows  = hb_region_vec(at, aw, n_keys, rcap);
    return true;
}

/* The slot table, filled as the groups come in group order: each group in
 * the first free slot from its home.  Homes ascend with the group number,
 * so this is one forward sweep; the run that passes the last slot goes on
 * from slot 0 once every group is in (hb_sweep_finish). */
typedef struct { uint64_t cap; int shift; int64_t next, wrap; bool zeroed; } hb_sweep_t;

static void hb_sweep_init(hb_sweep_t* w, uint64_t cap, bool zeroed) {
    w->cap = cap; w->shift = __builtin_clzll(cap - 1);
    w->next = 0; w->wrap = -1; w->zeroed = zeroed;
}

/* Groups [g_lo, g_hi); false when interrupted. */
static bool hb_sweep(hb_sweep_t* w, ray_t* table, const int64_t* gk, int64_t g_lo, int64_t g_hi,
                     ray_pool_t* pool) {
    for (int64_t g = g_lo; g < g_hi && w->wrap < 0; g++) {
        if (RAY_UNLIKELY(((g - g_lo) & 0xFFFFF) == 0xFFFFF && hb_stopped(pool))) return false;
        int64_t home = (int64_t)(mix64((uint64_t)gk[g]) >> w->shift);
        int64_t s = home > w->next ? home : w->next;
        if (s >= (int64_t)w->cap) { w->wrap = g; break; }
        if (!w->zeroed) hb_zero(table, w->next, s);
        hx_set(table, s, g + 1);
        w->next = s + 1;
    }
    return true;
}

static void hb_sweep_finish(hb_sweep_t* w, ray_t* table, int64_t n_groups) {
    if (!w->zeroed) hb_zero(table, w->next, (int64_t)w->cap);
    if (w->wrap < 0) return;
    for (int64_t s = 0, g = w->wrap; g < n_groups; g++) {     /* the run that wrapped */
        while (hx_get(table, s) != 0) s++;
        hx_set(table, s, g + 1);
    }
}

/* The end of the batch of partitions that starts at `p`: as many as fit
 * `batch_rows` (at least one) and the batch's task slots. */
static int64_t hb_batch_end(const hash_bld_t* h, int64_t p, int64_t max_batch, int64_t batch_rows) {
    int64_t q = p, br = 0;
    do { br += h->part_off[q + 1] - h->part_off[q]; q++; }
    while (q < h->n_part && q - p < max_batch &&
           br + h->part_off[q + 1] - h->part_off[q] <= batch_rows);
    return q;
}

/* Dedupe (or count) the partitions [p, q); false when interrupted or out of
 * memory. */
static bool hb_dedupe_batch(hash_bld_t* h, ray_pool_t* pool, bool par, int64_t p, int64_t q) {
    h->b_lo = p;
    if (par && q - p > 1) ray_pool_dispatch_n(pool, hb_dedupe, h, (uint32_t)(q - p));
    else for (int64_t i = 0; i < q - p; i++) hb_dedupe(h, 0, i, i + 1);
    return !hb_stopped(pool) && !atomic_load_explicit(&h->oom, memory_order_relaxed);
}

/* Debug builds: the row count past which row ids are 64-bit, lowered by
 * RAY_HASH_WIDE_ROWS so the wide layout runs on small columns in tests. */
static int64_t hb_wide_rows(void) {
#if defined(DEBUG)
    const char* e = getenv("RAY_HASH_WIDE_ROWS");
    if (e && *e) { long long x = strtoll(e, NULL, 10); if (x >= 0) return x; }
#endif
    return (int64_t)UINT32_MAX;
}

static int hash_build_part(ray_t* v, hb_dst_t* d, uint64_t* mask_out,
                           int64_t* n_keys_out, int64_t* n_groups_out, ray_hash_trace_t* tr) {
    int64_t n = v->len;
    ray_pool_t* pool = ray_pool_get();
    bool par = ray_pool_par_dispatch_ok(pool, n, 1 << 16);
    int64_t workers = par ? (int64_t)ray_pool_total_workers(pool) : 1;

    /* Rows of a batch: the random-access working set within the budget. */
    int64_t budget = ray_heap_anon_watermark() / 4;
    if (budget < (INT64_C(16) << 20)) budget = INT64_C(16) << 20;
    int64_t batch_rows = budget / HB_ROW_BYTES;
    int64_t part_rows = batch_rows / workers;
    if (part_rows > HB_PART_ROWS) part_rows = HB_PART_ROWS;
    if (part_rows < 4096) part_rows = 4096;

    hash_bld_t h;
    memset(&h, 0, sizeof(h));
    h.v = v; h.base = (const uint8_t*)ray_data(v); h.n = n;
    h.wide = n > hb_wide_rows();
    h.n_tasks = par ? workers * 4 : 1;
    if (h.n_tasks > HB_MAX_TASKS) h.n_tasks = HB_MAX_TASKS;
    h.n_part = 2;
    while (((par && h.n_part < workers * 4) || h.n_part * part_rows < n) &&
           h.n_part < HB_MAX_PARTS)
        h.n_part <<= 1;
    h.part_shift = 64;
    for (int64_t q = h.n_part; q > 1; q >>= 1) h.part_shift--;

    int8_t aw = h.wide ? RAY_I64 : RAY_I32;
    int64_t es = h.wide ? 8 : 4;
    size_t cnt_b = (size_t)h.n_tasks * (size_t)h.n_part * sizeof(int64_t);
    int64_t n_keys = 0, n_groups = 0, ng_run = 0, kw_done = 0;
    int64_t max_batch = h.n_part < workers * 4 ? h.n_part : workers * 4;
    h.part_cap0 = batch_rows;
    bool ok = false, known = false, one = false;
    uint64_t cap = 0;
    hb_sweep_t sw;
    memset(&sw, 0, sizeof(sw));
    ray_hash_mark_t tm = tr ? ray_hash_trace_mark() : (ray_hash_mark_t){0};
    int64_t colb = n * (int64_t)ray_sym_elem_size(v->type, v->attrs);
    h.cnt      = (int64_t*)ray_alloc_raw(cnt_b);
    h.part_off = (int64_t*)ray_alloc_raw((size_t)(h.n_part + 1) * sizeof(int64_t));
    h.part     = (hb_part_t*)ray_calloc_raw((size_t)max_batch * sizeof(hb_part_t));
    if (!h.cnt || !h.part_off || !h.part) goto done;
    memset(h.cnt, 0, cnt_b);

    /* 1: bucket the key words and row ids by partition */
    if (par) ray_pool_dispatch_n(pool, hb_count, &h, (uint32_t)h.n_tasks);
    else     hb_count(&h, 0, 0, 1);
    if (tr) { ray_hash_trace_add(tr, RAY_HXT_COUNT, tm, colb); tm = ray_hash_trace_mark(); }
    if (hb_stopped(pool)) goto done;
    {
        int64_t run = 0;
        for (int64_t p = 0; p < h.n_part; p++) {
            h.part_off[p] = run;
            for (int64_t t = 0; t < h.n_tasks; t++) {
                int64_t c = h.cnt[t * h.n_part + p];
                h.cnt[t * h.n_part + p] = run;
                run += c;
            }
        }
        h.part_off[h.n_part] = run;
        n_keys = run;
    }
    h.kw  = (uint64_t*)ray_alloc_raw((size_t)(n_keys > 0 ? n_keys : 1) * sizeof(uint64_t));
    h.rid = ray_alloc_raw((size_t)(n_keys > 0 ? n_keys : 1) * (size_t)es);
    if (!h.kw || !h.rid) goto done;
    if (par) ray_pool_dispatch_n(pool, hb_bucket, &h, (uint32_t)h.n_tasks);
    else     hb_bucket(&h, 0, 0, 1);
    if (tr) { ray_hash_trace_add(tr, RAY_HXT_BUCKET, tm, colb + n_keys * (8 + es)); tm = ray_hash_trace_mark(); }
    if (hb_stopped(pool)) goto done;

    /* 2: the group count before any array, so each is allocated — or laid
     * out in the persisted region — once, at its final size, and written
     * once, front to back.  One batch: its dedupe gives the count.  Several,
     * into a region: each partition's groups are counted first (one more
     * read of the key words, where the arrays would otherwise go through
     * memory at the key count's size and be copied into the file).  Several,
     * in memory: the group arrays are sized for the keys and cut after. */
    one = hb_batch_end(&h, 0, max_batch, batch_rows) >= h.n_part;
    if (one) {
        if (!hb_dedupe_batch(&h, pool, par, 0, h.n_part)) goto done;
        for (int64_t i = 0; i < h.n_part; i++) n_groups += h.part[i].ng;
        known = true;
    } else if (d->map) {
        h.part_ng = (int64_t*)ray_calloc_raw((size_t)h.n_part * sizeof(int64_t));
        if (!h.part_ng) goto done;
        h.counting = true;
        for (int64_t p = 0, q; p < h.n_part; p = q) {
            q = hb_batch_end(&h, p, max_batch, batch_rows);
            if (!hb_dedupe_batch(&h, pool, par, p, q)) goto done;
        }
        h.counting = false;
        for (int64_t p = 0; p < h.n_part; p++) n_groups += h.part_ng[p];
        known = true;
        if (tr) { ray_hash_trace_add(tr, RAY_HXT_DISTINCT, tm, n_keys * 8); tm = ray_hash_trace_mark(); }
    }
    if (known) {
        cap = next_pow2((uint64_t)(n_groups < 4 ? 8 : 2 * n_groups));
        if (cap < 8) cap = 8;
        if (!hb_dst_open(d, aw, n_keys, n_groups, cap)) goto done;
        hb_sweep_init(&sw, cap, d->zeroed);
    } else {
        d->gkeys = ray_vec_new(RAY_I64, n_keys > 0 ? n_keys : 1);
        d->offs  = ray_vec_new(aw, n_keys + 1);
        d->rows  = ray_vec_new(aw, n_keys > 0 ? n_keys : 1);
        if (!d->gkeys || RAY_IS_ERR(d->gkeys) || !d->offs || RAY_IS_ERR(d->offs) ||
            !d->rows || RAY_IS_ERR(d->rows)) goto done;
    }
    h.gk = (int64_t*)ray_data(d->gkeys); h.offs = d->offs; h.rows = d->rows;

    /* 3: partitions in batches: dedupe, store, and the batch's groups into
     * the slot table while they are at hand */
    for (int64_t p = 0, q; p < h.n_part; p = q) {
        q = one ? h.n_part : hb_batch_end(&h, p, max_batch, batch_rows);
        if (!one && !hb_dedupe_batch(&h, pool, par, p, q)) goto done;
        int64_t g_lo = ng_run;
        for (int64_t i = 0; i < q - p; i++) { h.part[i].g0 = ng_run; ng_run += h.part[i].ng; }
        if (par && q - p > 1) ray_pool_dispatch_n(pool, hb_emit, &h, (uint32_t)(q - p));
        else for (int64_t i = 0; i < q - p; i++) hb_emit(&h, 0, i, i + 1);
        /* an interrupted dispatch skips tasks: their partitions are still
         * held and their outputs unwritten */
        for (int64_t i = 0; i < q - p; i++) hb_part_free(&h.part[i]);
        if (hb_stopped(pool)) goto done;
        if (known && !hb_sweep(&sw, d->table, h.gk, g_lo, ng_run, pool)) goto done;
        /* The batch's key words and row ids are not read again: past the
         * watermark their pages leave the spill files now instead of being
         * written back, and make room for the arrays still to come. */
        ray_raw_discard(h.kw, (size_t)kw_done * 8, (size_t)h.part_off[q] * 8);
        ray_raw_discard(h.rid, (size_t)(kw_done * es), (size_t)(h.part_off[q] * es));
        kw_done = h.part_off[q];
    }
    if (known && ng_run != n_groups) goto done;   /* the counts disagree: never */
    n_groups = ng_run;
    hx_set(d->offs, n_groups, n_keys);
    d->gkeys->len = n_groups; d->offs->len = n_groups + 1; d->rows->len = n_keys;
    ray_free_raw(h.kw);  h.kw = NULL;
    ray_free_raw(h.rid); h.rid = NULL;
    if (tr) {
        ray_hash_trace_add(tr, RAY_HXT_GROUPS, tm, n_keys * (8 + es) + n_groups * 8 +
                           (n_groups + 1) * es + n_keys * es + (known ? (int64_t)cap * es : 0));
        tm = ray_hash_trace_mark();
    }

    /* 4: the slot table, unless swept batch by batch above */
    if (!known) {
        cap = next_pow2((uint64_t)(n_groups < 4 ? 8 : 2 * n_groups));
        if (cap < 8) cap = 8;
        d->table = ray_vec_new(aw, (int64_t)cap);
        if (!d->table || RAY_IS_ERR(d->table)) goto done;
        d->table->len = (int64_t)cap;
        hb_sweep_init(&sw, cap, false);
        if (!hb_sweep(&sw, d->table, h.gk, 0, n_groups, pool)) goto done;
    }
    hb_sweep_finish(&sw, d->table, n_groups);
    *mask_out = cap - 1;
    ok = true;

done:
    if (h.part)
        for (int64_t i = 0; i < max_batch; i++) hb_part_free(&h.part[i]);
    ray_free_raw(h.cnt); ray_free_raw(h.part_off); ray_free_raw(h.part);
    ray_free_raw(h.kw);  ray_free_raw(h.rid); ray_free_raw(h.part_ng);
    if (!ok) {
        hb_dst_drop(d);
        return -1;
    }
    if (!known) {
        ray_t *g0 = d->gkeys, *o0 = d->offs;
        d->gkeys = hb_trim(d->gkeys, n_groups, n_keys);
        d->offs  = hb_trim(d->offs, n_groups + 1, n_keys + 1);
        if (tr) {
            int64_t mv = n_groups * 8 + (int64_t)cap * es;   /* the sweep */
            if (d->gkeys != g0) mv += 2 * n_groups * 8;
            if (d->offs != o0)  mv += 2 * (n_groups + 1) * es;
            ray_hash_trace_add(tr, RAY_HXT_TABLE, tm, mv);
        }
    }
    if (tr) { tr->n_keys = n_keys; tr->n_groups = n_groups; }
    *n_keys_out = n_keys; *n_groups_out = n_groups;
    return 1;
}

ray_t* ray_index_attach_hash(ray_t** vp) {
    /* allow_str: keyed on a byte hash with payload-verified compares;
     * allow_sym: RAY_SYM uses domain ids. */
    ray_t* v = prepare_attach_ex(vp, "hash", true, true);
    if (RAY_IS_ERR(v)) return v;
    bool is_str = (v->type == RAY_STR);

    int64_t n = v->len;
    ray_t* table = NULL;
    uint64_t mask = 0;
    if (!is_str) {
        hb_dst_t d;
        memset(&d, 0, sizeof(d));
        uint64_t pm = 0;
        int64_t pk = 0, pn = 0;
        if (hash_build_part(v, &d, &pm, &pk, &pn, NULL) < 0)
            return hb_stopped(ray_pool_get()) ? ray_error("cancel", "interrupted")
                                              : ray_error("oom", NULL);
        ray_t* idx = ray_index_alloc(RAY_IDX_HASH, v->type, n);
        if (!idx || RAY_IS_ERR(idx)) {
            hb_dst_drop(&d);
            return idx ? idx : ray_error("oom", NULL);
        }
        ray_index_t* ix = ray_index_payload(idx);
        ix->u.hash.table    = d.table;
        ix->u.hash.gkeys    = d.gkeys;
        ix->u.hash.offs     = d.offs;
        ix->u.hash.rows     = d.rows;
        ix->u.hash.mask     = pm;
        ix->u.hash.n_keys   = pk;
        ix->u.hash.n_groups = pn;
        ix->u.hash.order_sym = -1;
        ix->markers |= RAY_MARK_HASH_HIGH;
        return attach_finalize(v, idx);
    }
    /* STR: a serial walk, groups numbered by first occurrence.  Build-time
     * capacity: sized by rows for O(1) inserts. */
    uint64_t bcap = next_pow2((uint64_t)(n < 4 ? 8 : 2 * n));
    if (bcap < 8) bcap = 8;
    uint64_t bmask = bcap - 1;

    ray_t* btab_hdr = NULL;
    ray_t* rgid_hdr = NULL;
    ray_t* gfirst_hdr = NULL;
    int64_t* btab = (int64_t*)scratch_alloc(&btab_hdr,
                                            (size_t)bcap * sizeof(int64_t));
    int64_t* rgid = (int64_t*)scratch_alloc(&rgid_hdr,
                        (size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    /* STR: first row of each group, for the payload compare on a word hit. */
    int64_t* gfirst = is_str ? (int64_t*)scratch_alloc(&gfirst_hdr,
                        (size_t)(n > 0 ? n : 1) * sizeof(int64_t)) : NULL;
    ray_t* gkeys = ray_vec_new(RAY_I64, n > 0 ? n : 1);   /* worst case: all distinct */
    if (!btab || !rgid || (is_str && !gfirst) || !gkeys || RAY_IS_ERR(gkeys)) {
        scratch_free(btab_hdr);
        scratch_free(rgid_hdr);
        scratch_free(gfirst_hdr);
        if (gkeys && !RAY_IS_ERR(gkeys)) ray_release(gkeys);
        return ray_error("oom", NULL);
    }
    memset(btab, 0, (size_t)bcap * sizeof(int64_t));
    int64_t* gk = (int64_t*)ray_data(gkeys);

    const uint8_t* base = (const uint8_t*)ray_data(v);
    int64_t n_keys = 0, n_groups = 0;

    for (int64_t i = 0; i < n; i++) {
        if (RAY_UNLIKELY((i & 0xFFFF) == 0 && ray_interrupted())) {
            scratch_free(btab_hdr);
            scratch_free(rgid_hdr);
            scratch_free(gfirst_hdr);
            ray_release(gkeys);
            return ray_error("cancel", "interrupted");
        }
        if (ray_vec_is_null(v, i)) { rgid[i] = -1; continue; }
        int64_t kw = (int64_t)hash_row_key_word(v, base, i);
        uint64_t slot = mix64((uint64_t)kw) & bmask;
        for (;;) {
            int64_t gp1 = btab[slot];
            if (gp1 == 0) {
                btab[slot] = n_groups + 1;
                gk[n_groups] = kw;
                if (gfirst) gfirst[n_groups] = i;
                rgid[i] = n_groups;
                n_groups++;
                break;
            }
            /* STR: a word hit is only a group hit when the bytes agree. */
            if (gk[gp1 - 1] == kw &&
                (!is_str || str_rows_eq(v, gfirst[gp1 - 1], i))) { rgid[i] = gp1 - 1; break; }
            slot = (slot + 1) & bmask;
        }
        n_keys++;
    }
    scratch_free(btab_hdr);
    scratch_free(gfirst_hdr);
    gkeys->len = n_groups;

    /* CSR slices: counting pass over rgid.  offs doubles as the fill
     * cursor; the shift-back restores the slice bounds. */
    ray_t* offs = ray_vec_new(RAY_I64, n_groups + 1);
    ray_t* rows = ray_vec_new(RAY_I64, n_keys > 0 ? n_keys : 1);
    if (!offs || RAY_IS_ERR(offs) || !rows || RAY_IS_ERR(rows)) {
        scratch_free(rgid_hdr);
        ray_release(gkeys);
        if (offs && !RAY_IS_ERR(offs)) ray_release(offs);
        if (rows && !RAY_IS_ERR(rows)) ray_release(rows);
        return ray_error("oom", NULL);
    }
    offs->len = n_groups + 1;
    rows->len = n_keys;
    int64_t* of = (int64_t*)ray_data(offs);
    int64_t* rw = (int64_t*)ray_data(rows);
    memset(of, 0, (size_t)(n_groups + 1) * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) {
        if (RAY_UNLIKELY((i & 0xFFFF) == 0 && ray_interrupted()))
            goto hash_cancel_csr;
        if (rgid[i] >= 0) of[rgid[i] + 1]++;
    }
    for (int64_t g = 0; g < n_groups; g++) {
        if (RAY_UNLIKELY((g & 0xFFFF) == 0 && ray_interrupted()))
            goto hash_cancel_csr;
        of[g + 1] += of[g];
    }
    for (int64_t i = 0; i < n; i++) {
        if (RAY_UNLIKELY((i & 0xFFFF) == 0 && ray_interrupted()))
            goto hash_cancel_csr;
        if (rgid[i] >= 0) rw[of[rgid[i]]++] = i;
    }
    for (int64_t g = n_groups; g > 0; g--) {
        if (RAY_UNLIKELY((g & 0xFFFF) == 0 && ray_interrupted()))
            goto hash_cancel_csr;
        of[g] = of[g - 1];
    }
    of[0] = 0;
    scratch_free(rgid_hdr);

    /* Attached/persisted bucket table: sized by DISTINCT keys. */
    uint64_t cap = next_pow2((uint64_t)(n_groups < 4 ? 8 : 2 * n_groups));
    if (cap < 8) cap = 8;
    mask = cap - 1;
    table = ray_vec_new(RAY_I64, (int64_t)cap);
    if (!table || RAY_IS_ERR(table)) {
        ray_release(gkeys); ray_release(offs); ray_release(rows);
        return table ? table : ray_error("oom", NULL);
    }
    table->len = (int64_t)cap;
    int64_t* tbl = (int64_t*)ray_data(table);
    memset(tbl, 0, (size_t)cap * sizeof(int64_t));
    for (int64_t g = 0; g < n_groups; g++) {
        if (RAY_UNLIKELY((g & 0xFFFF) == 0 && ray_interrupted())) {
            ray_release(table); ray_release(gkeys);
            ray_release(offs);  ray_release(rows);
            return ray_error("cancel", "interrupted");
        }
        uint64_t slot = mix64((uint64_t)gk[g]) & mask;
        while (tbl[slot] != 0) slot = (slot + 1) & mask;
        tbl[slot] = g + 1;
    }

    ray_t* idx = ray_index_alloc(RAY_IDX_HASH, v->type, n);
    if (!idx || RAY_IS_ERR(idx)) {
        ray_release(table); ray_release(gkeys);
        ray_release(offs);  ray_release(rows);
        return idx ? idx : ray_error("oom", NULL);
    }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.hash.table    = table;
    ix->u.hash.gkeys    = gkeys;
    ix->u.hash.offs     = offs;
    ix->u.hash.rows     = rows;
    ix->u.hash.mask     = mask;
    ix->u.hash.n_keys   = n_keys;
    ix->u.hash.n_groups = n_groups;
    ix->u.hash.order_sym = -1;
    hx_narrow_payload(ix);

    return attach_finalize(v, idx);

hash_cancel_csr:
    scratch_free(rgid_hdr);
    ray_release(gkeys);
    ray_release(offs);
    ray_release(rows);
    return ray_error("cancel", "interrupted");
}

ray_err_t ray_index_hash_build_region(ray_t* v, ray_index_region_fn map, void* ctx,
                                      ray_hash_trace_t* tr) {
    if (!v || RAY_IS_ERR(v) || !map) return RAY_ERR_DOMAIN;
    if (!ray_is_vec(v) || (v->attrs & RAY_ATTR_SLICE) ||
        (numeric_elem_size(v->type) == 0 && v->type != RAY_SYM))
        return RAY_ERR_NYI;
    hb_dst_t d;
    memset(&d, 0, sizeof(d));
    d.map = map; d.ctx = ctx;
    uint64_t mask = 0;
    int64_t n_keys = 0, n_groups = 0;
    if (hash_build_part(v, &d, &mask, &n_keys, &n_groups, tr) < 0)
        return d.err != RAY_OK ? d.err
             : hb_stopped(ray_pool_get()) ? RAY_ERR_CANCEL : RAY_ERR_OOM;
    /* The head as ray_index_attach_hash fills it: the parent's aux and
     * HAS_NULLS snapshot — the aux it had before any index of its own, which
     * an attach drops first. */
    ray_index_t ix;
    memset(&ix, 0, sizeof(ix));
    ix.kind = RAY_IDX_HASH;
    ix.parent_type = v->type;
    ix.built_for_len = v->len;
    ix.markers = RAY_MARK_HASH_HIGH;
    bool has_ix = (v->attrs & RAY_ATTR_HAS_INDEX) && v->index && !RAY_IS_ERR(v->index);
    memcpy(ix.saved_aux, has_ix ? ray_index_payload(v->index)->saved_aux : v->aux, 16);
    ix.saved_attrs = v->attrs & RAY_ATTR_HAS_NULLS;
    ix.u.hash.table     = d.table;
    ix.u.hash.gkeys     = d.gkeys;
    ix.u.hash.offs      = d.offs;
    ix.u.hash.rows      = d.rows;
    ix.u.hash.mask      = mask;
    ix.u.hash.n_keys    = n_keys;
    ix.u.hash.n_groups  = n_groups;
    ix.u.hash.order_sym = -1;
    idx_inline_head(d.base, &ix);
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * Hash-index point-lookup probe — public entry point for the eq-filter
 * fast path (ray_index_hash_eq_rowsel).
 *
 * Callers present the index with an int64 key; we mix64 it with the
 * same hash the builder used, walk the bucket chain, collect matches,
 * and emit a ray_rowsel sized for O(matches) memory (no intermediate
 * row-wide BOOL pred vec).
 *
 * Type matrix.  An index built on column type T accepts a key only
 * when T's storage width covers it without truncation — i.e. asking
 * for `u8_col == 300` would never match, so we fail eligibility and
 * the caller falls back to the scan (which folds out-of-range via
 * fp_fold_t).  Float keys are not supported here — equality on
 * F32/F64 has NaN / -0 semantics the unfused engine handles. */

static int hash_key_in_range(int8_t t, int64_t k) {
    switch (t) {
    case RAY_BOOL: case RAY_U8:        return k >= 0 && k <= UINT8_MAX;
    case RAY_I16:                      return k >= INT16_MIN && k <= INT16_MAX;
    case RAY_I32: case RAY_DATE:
    case RAY_TIME:                     return k >= INT32_MIN && k <= INT32_MAX;
    case RAY_I64:
    case RAY_TIMESTAMP:                return 1;
    case RAY_SYM:                      return k >= 0; /* domain id; non-neg guaranteed by resolve */
    default:                           return 0;
    }
}

/* Read row `i` of a numeric column as int64 for equality compare. */
static int64_t hash_col_read_i64(const uint8_t* base, int8_t t, int64_t i) {
    int es;
    switch (t) {
    case RAY_BOOL: case RAY_U8:        es = 1; break;
    case RAY_I16:                      es = 2; break;
    case RAY_I32: case RAY_DATE:
    case RAY_TIME:                     es = 4; break;  /* TIME is 4-byte int32 */
    case RAY_I64:
    case RAY_TIMESTAMP:                es = 8; break;
    default:                           return 0;
    }
    switch (es) {
    case 1:  return (int64_t)base[i];
    case 2:  { int16_t v; memcpy(&v, base + i*2, 2); return (int64_t)v; }
    case 4:  { int32_t v; memcpy(&v, base + i*4, 4); return (int64_t)v; }
    default: { int64_t v; memcpy(&v, base + i*8, 8); return v;          }
    }
}

/* Mirror numeric_key_word for an int64 key probed against a column of
 * element size `es`: the canonical hash input is the raw bit pattern of
 * the storage width.  We zero-extend U8/BOOL and sign-extend others up
 * to int64; mix64 then folds them — the builder did the same per row. */
static uint64_t hash_key_bits(int es, int64_t key) {
    switch (es) {
    case 1:  return (uint64_t)(uint8_t)key;
    case 2:  return (uint64_t)(int64_t)(int16_t)key;
    case 4:  return (uint64_t)(int64_t)(int32_t)key;
    default: return (uint64_t)key;
    }
}

/* Validate eligibility and resolve `key` to its GROUP id.
 * On a valid index with no such key leaves *gid = -1 (provably absent) so
 * the caller can short-circuit; returns NULL only on ineligibility. */
static ray_index_t* hash_probe_setup(ray_t* col, int64_t key,
                                     int64_t* gid) {
    *gid = -1;
    if (!col || RAY_IS_ERR(col) || !ray_is_vec(col)) return NULL;
    if (!(col->attrs & RAY_ATTR_HAS_INDEX) || !col->index) return NULL;
    ray_index_t* ix = ray_index_payload(col->index);
    if (ix->kind != RAY_IDX_HASH) return NULL;
    if (ix->built_for_len != col->len) return NULL;
    if (!hash_key_in_range(col->type, key)) return NULL;
    /* Hash builders omit null rows. A zero-symbol probe must use the scan
     * path, which also handles indexes persisted before this convention. */
    if (col->type == RAY_SYM && key == 0) return NULL;
    /* SYM ids are valid probe keys (non-negative domain ids); all other
     * non-numeric types are rejected by hash_key_in_range above. */
    if (col->type != RAY_SYM && numeric_elem_size(col->type) == 0) return NULL;
    if (!ix->u.hash.table || !ix->u.hash.gkeys ||
        !ix->u.hash.offs  || !ix->u.hash.rows) return NULL;

    /* SYM: hash the domain id directly (mirrors the builder's cast to uint64_t).
     * Numeric: fold through the storage width to match the builder's
     * numeric_key_word.  gkeys stores exactly these canonical values, so the
     * slot walk compares i64s — no column reads. */
    int64_t kw = (col->type == RAY_SYM)
        ? key
        : (int64_t)hash_key_bits(numeric_elem_size(col->type), key);
    uint64_t slot = hx_home(ix, mix64((uint64_t)kw));
    const ray_t* tab = ix->u.hash.table;
    const int64_t* gk  = (const int64_t*)ray_data(ix->u.hash.gkeys);
    for (;;) {
        int64_t gp1 = hx_get(tab, slot);
        if (gp1 == 0) break;                 /* key absent */
        if (gk[gp1 - 1] == kw) { *gid = gp1 - 1; break; }
        slot = (slot + 1) & ix->u.hash.mask;
    }
    return ix;
}

/* STR twin of hash_probe_setup: the key is the needle's bytes.  Walks the
 * bucket table on the byte-hash word and confirms each word hit against the
 * payload of the group's first row (rows[offs[gid]]) — a distinct string with
 * the same word is a different group further along the chain. */
static ray_index_t* hash_probe_setup_str(ray_t* col, const char* p, size_t l,
                                         int64_t* gid) {
    *gid = -1;
    if (!col || RAY_IS_ERR(col) || !ray_is_vec(col) || col->type != RAY_STR) return NULL;
    if (!(col->attrs & RAY_ATTR_HAS_INDEX) || !col->index) return NULL;
    ray_index_t* ix = ray_index_payload(col->index);
    if (ix->kind != RAY_IDX_HASH) return NULL;
    if (ix->built_for_len != col->len) return NULL;
    if (!ix->u.hash.table || !ix->u.hash.gkeys ||
        !ix->u.hash.offs  || !ix->u.hash.rows) return NULL;

    int64_t kw = (int64_t)str_key_word(p, l);
    uint64_t slot = hx_home(ix, mix64((uint64_t)kw));
    const ray_t* tab = ix->u.hash.table;
    const int64_t* gk  = (const int64_t*)ray_data(ix->u.hash.gkeys);
    const ray_t* ofv = ix->u.hash.offs;
    const ray_t* rwv = ix->u.hash.rows;
    for (;;) {
        int64_t gp1 = hx_get(tab, slot);
        if (gp1 == 0) break;                 /* key absent */
        if (gk[gp1 - 1] == kw &&
            str_row_eq_bytes(col, hx_get(rwv, hx_get(ofv, gp1 - 1)), p, l)) { *gid = gp1 - 1; break; }
        slot = (slot + 1) & ix->u.hash.mask;
    }
    return ix;
}

/* v1 routing eligibility — structural gate shared by every consult function.
 * Checks that col is a flat (non-parted, non-MAPCOMMON) vector carrying a
 * fresh index of the requested kind.  Does NOT gate on HAS_NULLS: hash and
 * bloom indexes skip null rows during build, so probing a null-bearing column
 * is correct (the chain simply won't surface null-row matches).  Callers that
 * must exclude null-bearing columns for semantic reasons (sort perm, range
 * scan) should add their own HAS_NULLS check. */
static bool idx_fresh(ray_t* col, ray_idx_kind_t kind) {
    if (!col || RAY_IS_ERR(col)) return false;
    if (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON) return false;
    if (!ray_index_has(col)) return false;
    ray_index_t* ix = ray_index_payload(col->index);
    return ix->kind == (uint8_t)kind && ix->built_for_len == col->len;
}

/* v1 routing gate for the NEW consult paths: like idx_fresh but also
 * excludes null-bearing columns.  A sort perm now orders nulls FIRST on
 * ascending, agreeing with null-as-minimum rather than contradicting it
 * — but a perm built before that and persisted still carries nulls last,
 * and none of the new consults model either placement in v1.
 * (The pre-existing hash-eq probe keeps bare idx_fresh: its builder
 * skips null rows, making null-bearing probes structurally correct.) */
static bool idx_fresh_nonull(ray_t* col, ray_idx_kind_t kind) {
    if (!idx_fresh(col, kind)) return false;
    if (col->type == RAY_SYM) {
        ray_index_t* ix = ray_index_payload(col->index);
        /* Builders already counted non-null rows. Keep admission O(1)
         * instead of rescanning the column on each indexed lookup. */
        if (kind == RAY_IDX_HASH)
            return ix->u.hash.rows && ix->u.hash.rows->len == col->len;
        if (kind == RAY_IDX_BLOOM) return ix->u.bloom.n_keys == col->len;
        if (kind == RAY_IDX_ZONE) return ix->u.zone.n_nulls == 0;
        if (kind == RAY_IDX_PART) return true; /* builder rejects nulls */
    }
    return !ray_vec_has_nulls(col);
}

/* --------------------------------------------------------------------------
 * Zone-index all/none classification
 * -------------------------------------------------------------------------- */

/* Classify (col cmp_op key) using the zone index min/max for O(1) decision.
 * Integer family (BOOL/U8/I16/I32/I64/DATE/TIME/TIMESTAMP) uses key_i and
 * the zone's min_i/max_i; float family (F32/F64) uses key_f and min_f/max_f.
 * NaN key_f → UNKNOWN immediately (NaN comparison is the null-aware kernel's
 * business).  Returns UNKNOWN when not eligible (no zone, stale, null-bearing,
 * or unsupported type). */
ray_zone_class_t ray_index_zone_class(ray_t* col, uint16_t cmp_op,
                                      int64_t key_i, double key_f) {
    if (!idx_fresh_nonull(col, RAY_IDX_ZONE)) return RAY_ZONE_UNKNOWN;
    ray_index_t* ix = ray_index_payload(col->index);

    /* Dispatch to integer or float path based on column type. */
    bool is_float = (col->type == RAY_F32 || col->type == RAY_F64);
    bool is_int   = !is_float && (numeric_elem_size(col->type) > 0);
    if (!is_float && !is_int) return RAY_ZONE_UNKNOWN;

    if (is_float && isnan(key_f)) return RAY_ZONE_UNKNOWN;

#define ZONE_CLASSIFY(mn, mx, key) do {                                \
    switch (cmp_op) {                                                  \
    case OP_EQ:                                                        \
        if ((key) < (mn) || (key) > (mx)) return RAY_ZONE_NONE;       \
        if ((mn) == (mx) && (mn) == (key)) return RAY_ZONE_ALL;       \
        break;                                                         \
    case OP_NE:                                                        \
        if ((mn) == (mx) && (mn) == (key)) return RAY_ZONE_NONE;      \
        if ((key) < (mn) || (key) > (mx)) return RAY_ZONE_ALL;        \
        break;                                                         \
    case OP_LT:                                                        \
        if ((mx) <  (key)) return RAY_ZONE_ALL;                        \
        if ((mn) >= (key)) return RAY_ZONE_NONE;                       \
        break;                                                         \
    case OP_LE:                                                        \
        if ((mx) <= (key)) return RAY_ZONE_ALL;                        \
        if ((mn) >  (key)) return RAY_ZONE_NONE;                       \
        break;                                                         \
    case OP_GT:                                                        \
        if ((mn) >  (key)) return RAY_ZONE_ALL;                        \
        if ((mx) <= (key)) return RAY_ZONE_NONE;                       \
        break;                                                         \
    case OP_GE:                                                        \
        if ((mn) >= (key)) return RAY_ZONE_ALL;                        \
        if ((mx) <  (key)) return RAY_ZONE_NONE;                       \
        break;                                                         \
    }                                                                  \
} while (0)

    if (is_int)   { ZONE_CLASSIFY(ix->u.zone.min_i, ix->u.zone.max_i, key_i); }
    else          { ZONE_CLASSIFY(ix->u.zone.min_f, ix->u.zone.max_f, key_f); }

#undef ZONE_CLASSIFY

    return RAY_ZONE_UNKNOWN;
}

/* qsort comparator: ascending int64 row ids, used by the rowsel
 * builder to put matches into per-segment order. */
static int hash_match_cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a;
    int64_t y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

/* Build a rowsel block from a SORTED ascending array of matching row ids
 * over a column of n rows.  Returns fresh rowsel (rc=1) or NULL on OOM.
 * mcnt==0 yields a valid all-NONE rowsel, NOT NULL — NULL means "no fast
 * path" to every consumer (idxop.h contract). */
static inline __attribute__((always_inline))
ray_t* rowsel_sorted_body(int64_t n, ray_idx_rows_t ids, int64_t mcnt) {
    ray_t* block = ray_rowsel_new(n, mcnt, mcnt);
    if (!block) return NULL;

    uint32_t n_segs = ray_rowsel_meta(block)->n_segs;
    uint8_t*  seg_flags   = ray_rowsel_flags(block);
    uint32_t* seg_offsets = ray_rowsel_offsets(block);
    uint16_t* idx_arr     = ray_rowsel_idx(block);

    /* All segments default to NONE; the loop below flips MIX where
     * a match lands.  ray_alloc does NOT zero the data area
     * (only the 32-byte header), so explicit init is required. */
    memset(seg_flags, RAY_SEL_NONE, (size_t)n_segs);
    /* seg_offsets is built by linear sweep below — initialize to a
     * sentinel that the sweep will overwrite. */
    /* (no memset needed; the sweep writes every entry [0..n_segs]) */

    /* Classify-then-emit sweep over the sorted matches.  For each
     * segment, first COUNT its matches by advancing mi without writing
     * anything, then classify and emit:
     *   - NONE: no matches — nothing stored.
     *   - ALL:  whole segment matched — no idx[] entries, cum unchanged,
     *           so seg_offsets[s+1] == seg_offsets[s] (rowsel.h contract;
     *           consumers iterate ALL segments densely and never read
     *           idx[] for them).
     *   - MIX:  re-walk [mi0, mi) and write morsel-local indices at
     *           idx_arr[cum..], then advance cum.
     * The previous sweep wrote idx entries while counting and tried to
     * "roll back" ALL segments with `cum -= pc` — but cum had never been
     * advanced for the segment, so the uint32 cursor wrapped and every
     * subsequent idx_arr write landed out of bounds. */
    int64_t mi = 0;
    uint32_t cum = 0;
    for (uint32_t s = 0; s < n_segs; s++) {
        seg_offsets[s] = cum;
        int64_t seg_start = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t seg_end   = seg_start + RAY_MORSEL_ELEMS;
        if (seg_end > n) seg_end = n;
        int64_t mi0 = mi;
        while (mi < mcnt && ray_idx_rows_at(ids, mi) < seg_end) mi++;
        int64_t pc = mi - mi0;
        if (pc == 0) continue;                 /* NONE — preset by memset */
        if (pc == seg_end - seg_start) {
            seg_flags[s] = RAY_SEL_ALL;        /* zero idx[] entries */
        } else {
            seg_flags[s] = RAY_SEL_MIX;
            for (int64_t i = mi0; i < mi; i++)
                idx_arr[cum + (uint32_t)(i - mi0)] =
                    (uint16_t)(ray_idx_rows_at(ids, i) - seg_start);
            cum += (uint32_t)pc;
        }
    }
    seg_offsets[n_segs] = cum;
    /* meta->total_pass is already mcnt (set by ray_rowsel_new): ALL-segment
     * rows still count as passing.  idx space was sized for mcnt entries,
     * so when ALL segments exist the tail [cum, mcnt) is unused slack —
     * harmless; ray_rowsel_new only uses idx_count for allocation sizing. */

    return block;
}
static ray_t* rowsel_from_sorted_ids(int64_t n, const int64_t* ids, int64_t mcnt) {
    return rowsel_sorted_body(n, (ray_idx_rows_t){ ids, false }, mcnt);
}
static ray_t* rowsel_from_sorted_rows(int64_t n, ray_idx_rows_t ids, int64_t mcnt) {
    return ids.narrow ? rowsel_sorted_body(n, (ray_idx_rows_t){ ids.p, true }, mcnt)
                      : rowsel_sorted_body(n, (ray_idx_rows_t){ ids.p, false }, mcnt);
}

/* Public wrapper: build an all-NONE rowsel for n rows.  Returns NULL on OOM. */
ray_t* ray_index_empty_rowsel(int64_t n) {
    return rowsel_from_sorted_ids(n, NULL, 0);
}

ray_t* ray_index_hash_eq_rowsel(ray_t* col, int64_t key) {
    /* Sanity precheck — idx_fresh validates parted/nulls/kind/staleness;
     * hash_probe_setup below also validates key-range, elem-size, and
     * payload pointers, so these checks are complementary. */
    if (!idx_fresh(col, RAY_IDX_HASH)) return NULL;
    int64_t gid = -1;
    ray_index_t* ix = hash_probe_setup(col, key, &gid);
    if (!ix) return NULL;

    int64_t n = col->len;
    if (gid < 0)   /* provably absent → O(1) all-NONE rowsel */
        return rowsel_from_sorted_ids(n, NULL, 0);

    /* CSR: the group's row ids are one contiguous ascending slice — feed
     * it to the rowsel builder directly.  Group size is known up front;
     * a dense key (where the SIMD scan would win) returns NULL with zero
     * wasted work instead of the chain-walk budget the old layout needed. */
    const ray_t* ofv = ix->u.hash.offs;
    int64_t gsz = hx_get(ofv, gid + 1) - hx_get(ofv, gid);
    if (gsz > 64 && gsz > (n >> 3))
        return NULL;   /* dense: fall through to the scan path */

    return rowsel_from_sorted_rows(n, hx_rows(ix->u.hash.rows, hx_get(ofv, gid)), gsz);
}

/* --------------------------------------------------------------------------
 * Hash/part-index IN probe
 *
 * For each unique in-range element of set_vec, walk the hash chain and
 * collect matching row ids into a single shared buffer, then build a
 * rowsel in one pass.
 * -------------------------------------------------------------------------- */

/* Read element i of a set vec (integer-family only) as int64.  Mirrors
 * hash_col_read_i64 but operates on the set_vec type, not the column type. */
static int64_t set_vec_read_i64(const uint8_t* base, int8_t t, int64_t i) {
    switch (t) {
    case RAY_BOOL: case RAY_U8:        return (int64_t)base[i];
    case RAY_I16:  { int16_t v; memcpy(&v, base + i*2, 2); return (int64_t)v; }
    case RAY_I32: case RAY_DATE: case RAY_TIME:
                   { int32_t v; memcpy(&v, base + i*4, 4); return (int64_t)v; }
    case RAY_I64: case RAY_TIMESTAMP:
                   { int64_t v; memcpy(&v, base + i*8, 8); return v; }
    default:       return 0;
    }
}

static int cmp_i64_plain(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a;
    int64_t y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

typedef struct {
    int64_t start;
    int64_t len;
} idx_span_t;

static bool sorted_i64_contains(const int64_t* values, int64_t n,
                                int64_t needle) {
    int64_t lo = 0, hi = n;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (values[mid] < needle) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && values[lo] == needle;
}

/* Build a rowsel from sorted, disjoint physical-row spans.  Part indexes
 * already describe matches this way, so keep full morsels as ALL and store
 * row ids only for partial morsels instead of expanding every matching row. */
static ray_t* rowsel_from_sorted_spans(int64_t n, const idx_span_t* spans,
                                       int64_t nspans, int64_t total) {
    if (n < 0 || nspans < 0 || total < 0 || total > n) return NULL;
    if (nspans == 0)
        return rowsel_from_sorted_ids(n, NULL, 0);

    uint32_t nsegs = n > 0
        ? (uint32_t)((n + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS)
        : 0;
    ray_t* pop_hdr = ray_alloc((int64_t)nsegs * (int64_t)sizeof(uint32_t));
    if (!pop_hdr) return NULL;
    uint32_t* pop = (uint32_t*)ray_data(pop_hdr);
    memset(pop, 0, (size_t)nsegs * sizeof(uint32_t));

    int64_t counted = 0;
    int64_t prev_end = 0;
    for (int64_t i = 0; i < nspans; i++) {
        int64_t start = spans[i].start;
        int64_t len = spans[i].len;
        if (start < prev_end || len <= 0 || start < 0 || start > n - len) {
            ray_release(pop_hdr);
            return NULL;
        }
        int64_t end = start + len;
        counted += len;
        prev_end = end;
        uint32_t first = (uint32_t)(start / RAY_MORSEL_ELEMS);
        uint32_t last = (uint32_t)((end - 1) / RAY_MORSEL_ELEMS);
        for (uint32_t s = first; s <= last; s++) {
            int64_t ss = (int64_t)s * RAY_MORSEL_ELEMS;
            int64_t se = ss + RAY_MORSEL_ELEMS;
            if (se > n) se = n;
            int64_t a = start > ss ? start : ss;
            int64_t b = end < se ? end : se;
            pop[s] += (uint32_t)(b - a);
        }
    }
    if (counted != total) {
        ray_release(pop_hdr);
        return NULL;
    }

    int64_t idx_count = 0;
    for (uint32_t s = 0; s < nsegs; s++) {
        int64_t ss = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t se = ss + RAY_MORSEL_ELEMS;
        if (se > n) se = n;
        if (pop[s] != 0 && pop[s] != (uint32_t)(se - ss))
            idx_count += pop[s];
    }

    ray_t* block = ray_rowsel_new(n, total, idx_count);
    if (!block) {
        ray_release(pop_hdr);
        return NULL;
    }
    uint8_t* flags = ray_rowsel_flags(block);
    uint32_t* offsets = ray_rowsel_offsets(block);
    uint16_t* ids = ray_rowsel_idx(block);
    uint32_t cum = 0;
    for (uint32_t s = 0; s < nsegs; s++) {
        offsets[s] = cum;
        int64_t ss = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t se = ss + RAY_MORSEL_ELEMS;
        if (se > n) se = n;
        if (pop[s] == 0) flags[s] = RAY_SEL_NONE;
        else if (pop[s] == (uint32_t)(se - ss)) flags[s] = RAY_SEL_ALL;
        else {
            flags[s] = RAY_SEL_MIX;
            cum += pop[s];
        }
    }
    offsets[nsegs] = cum;

    memset(pop, 0, (size_t)nsegs * sizeof(uint32_t));
    for (int64_t i = 0; i < nspans; i++) {
        int64_t start = spans[i].start;
        int64_t end = start + spans[i].len;
        uint32_t first = (uint32_t)(start / RAY_MORSEL_ELEMS);
        uint32_t last = (uint32_t)((end - 1) / RAY_MORSEL_ELEMS);
        for (uint32_t s = first; s <= last; s++) {
            if (flags[s] != RAY_SEL_MIX) continue;
            int64_t ss = (int64_t)s * RAY_MORSEL_ELEMS;
            int64_t se = ss + RAY_MORSEL_ELEMS;
            if (se > n) se = n;
            int64_t a = start > ss ? start : ss;
            int64_t b = end < se ? end : se;
            uint32_t out = offsets[s] + pop[s];
            for (int64_t r = a; r < b; r++)
                ids[out++] = (uint16_t)(r - ss);
            pop[s] += (uint32_t)(b - a);
        }
    }
    ray_release(pop_hdr);
    return block;
}

ray_t* ray_index_part_eq_rowsel(ray_t* col, int64_t key) {
    if (!idx_fresh_nonull(col, RAY_IDX_PART)) return NULL;
    if (col->type == RAY_F32 || col->type == RAY_F64) return NULL;
    if (col->type != RAY_SYM && !hash_key_in_range(col->type, key))
        return rowsel_from_sorted_ids(col->len, NULL, 0);

    ray_index_t* ix = ray_index_payload(col->index);
    ray_t* keys = ix->u.part.keys;
    ray_t* starts = ix->u.part.starts;
    ray_t* lens = ix->u.part.lens;
    int64_t nparts = ix->u.part.n_parts;
    if (!keys || !starts || !lens || keys->len != nparts ||
        starts->len != nparts || lens->len != nparts)
        return NULL;

    const uint8_t* kb = (const uint8_t*)ray_data(keys);
    const int64_t* st = (const int64_t*)ray_data(starts);
    const int64_t* ln = (const int64_t*)ray_data(lens);
    for (int64_t p = 0; p < nparts; p++) {
        int64_t pkey = keys->type == RAY_SYM
            ? ray_read_sym(kb, p, RAY_SYM, keys->attrs)
            : set_vec_read_i64(kb, keys->type, p);
        if (pkey != key) continue;
        /* A direct FILTER rowsel still feeds the generic table compactor.
         * For a dense partition that O(selected rows × columns) gather is
         * slower than the normal predicate scan.  Keep dense partitions on
         * the scan path; the dedicated GROUP BY slice path remains dense-safe
         * because it streams the range directly into its aggregates. */
        if (ln[p] > 64 && ln[p] > (col->len >> 1))
            return NULL;
        idx_span_t span = { .start = st[p], .len = ln[p] };
        return rowsel_from_sorted_spans(col->len, &span, 1, ln[p]);
    }
    return rowsel_from_sorted_ids(col->len, NULL, 0);
}

ray_t* ray_index_in_rowsel(ray_t* col, ray_t* set_vec) {
    /* Gate: integer-family or SYM column with a fresh hash/part index. */
    ray_idx_kind_t kind = ray_index_kind(col);
    if ((kind != RAY_IDX_HASH && kind != RAY_IDX_PART) ||
        !(col && col->type == RAY_SYM ? idx_fresh(col, kind)
                                      : idx_fresh_nonull(col, kind))) return NULL;
    bool col_is_float = (col->type == RAY_F32 || col->type == RAY_F64);
    if (col_is_float) return NULL;
    /* A STR hash index is keyed on byte hashes — the int64 probes below do
     * not apply (the eval-level `in` consults it via ray_index_find_vec). */
    if (col->type == RAY_STR) return NULL;
    bool col_is_sym = (col->type == RAY_SYM);

    /* set_vec must be a non-atom vec of the matching family: SYM set for a
     * SYM column (each element resolved through the COLUMN's domain — the
     * index is keyed on column-domain ids, and the set may live in a
     * different domain), integer-family set for an integer column. */
    if (!set_vec || RAY_IS_ERR(set_vec) || ray_is_atom(set_vec)) return NULL;
    int8_t st = set_vec->type;
    if (col_is_sym) {
        if (st != RAY_SYM) return NULL;
    } else {
        bool set_is_float = (st == RAY_F32 || st == RAY_F64);
        if (set_is_float) return NULL;
        /* Check set type is integer-family (numeric_elem_size covers all int types) */
        if (numeric_elem_size(st) == 0) return NULL;
    }

    int64_t set_len = set_vec->len;
    int64_t n       = col->len;

    /* Canonicalize set: copy to int64 scratch, sort, unique, drop
     * out-of-range / domain-absent.  SYM: translate each set symbol to the
     * column's domain id (absent symbol matches no rows — drop it). */
    int64_t* set_scratch = NULL;
    ray_t*   set_hdr     = NULL;
    if (set_len > 0) {
        set_hdr = ray_alloc(set_len * (int64_t)sizeof(int64_t));
        if (!set_hdr) return NULL;
        set_scratch = (int64_t*)ray_data(set_hdr);
        const uint8_t* sb = (const uint8_t*)ray_data(set_vec);
        int64_t ulen = 0;
        for (int64_t i = 0; i < set_len; i++) {
            if (col_is_sym) {
                ray_t* s = ray_sym_vec_cell(set_vec, i);
                if (!s) continue;
                int64_t dom_id = ray_sym_vec_lookup(col, ray_str_ptr(s),
                                                    ray_str_len(s));
                if (dom_id == 0 && kind == RAY_IDX_HASH) {
                    ray_release(set_hdr);
                    return NULL; /* null membership needs the scan path */
                }
                if (dom_id >= 0)
                    set_scratch[ulen++] = dom_id;
            } else {
                int64_t v = set_vec_read_i64(sb, st, i);
                if (hash_key_in_range(col->type, v))
                    set_scratch[ulen++] = v;
            }
        }
        if (ulen == 0) {
            /* All elements out of range / absent — no possible matches. */
            ray_release(set_hdr);
            return rowsel_from_sorted_ids(n, NULL, 0);
        }
        /* Sort and deduplicate. */
        qsort(set_scratch, (size_t)ulen, sizeof(int64_t), cmp_i64_plain);
        int64_t wlen = 1;
        for (int64_t i = 1; i < ulen; i++)
            if (set_scratch[i] != set_scratch[i-1])
                set_scratch[wlen++] = set_scratch[i];
        set_len = wlen;
    } else {
        /* Empty set → all-NONE rowsel. */
        return rowsel_from_sorted_ids(n, NULL, 0);
    }

    if (kind == RAY_IDX_PART) {
        ray_index_t* ix = ray_index_payload(col->index);
        ray_t* keys = ix->u.part.keys;
        ray_t* starts = ix->u.part.starts;
        ray_t* lens = ix->u.part.lens;
        int64_t nparts = ix->u.part.n_parts;
        if (!keys || !starts || !lens || keys->len != nparts ||
            starts->len != nparts || lens->len != nparts) {
            ray_release(set_hdr);
            return NULL;
        }

        ray_t* spans_hdr = ray_alloc(set_len * (int64_t)sizeof(idx_span_t));
        if (!spans_hdr) {
            ray_release(set_hdr);
            return NULL;
        }
        idx_span_t* spans = (idx_span_t*)ray_data(spans_hdr);
        const int64_t* stv = (const int64_t*)ray_data(starts);
        const int64_t* lnv = (const int64_t*)ray_data(lens);
        const uint8_t* kb = (const uint8_t*)ray_data(keys);
        int64_t nspans = 0;
        int64_t total = 0;
        for (int64_t p = 0; p < nparts; p++) {
            int64_t key = col_is_sym
                ? ray_read_sym(kb, p, RAY_SYM, keys->attrs)
                : set_vec_read_i64(kb, keys->type, p);
            if (!sorted_i64_contains(set_scratch, set_len, key)) continue;
            spans[nspans].start = stv[p];
            spans[nspans].len = lnv[p];
            total += lnv[p];
            nspans++;
        }
        ray_release(set_hdr);
        ray_t* block = rowsel_from_sorted_spans(n, spans, nspans, total);
        ray_release(spans_hdr);
        return block;
    }

    /* Resolve every set element to its GROUP up front (i64 compares against
     * gkeys — no column reads), so the union size is known EXACTLY before
     * any collection work: a dense union (where the SIMD membership scan
     * wins) returns NULL with zero waste — no chain-walk budget needed. */
    ray_t* gid_hdr = NULL;
    int64_t* gids = (int64_t*)scratch_alloc(&gid_hdr,
                                            (size_t)set_len * sizeof(int64_t));
    if (!gids) { ray_release(set_hdr); return NULL; }

    int64_t total = 0;
    const ray_t* ofv = NULL;
    const ray_t* rwv = NULL;
    {
        ray_index_t* ix = ray_index_payload(col->index);
        if (!ix->u.hash.table || !ix->u.hash.gkeys ||
            !ix->u.hash.offs  || !ix->u.hash.rows) {
            scratch_free(gid_hdr);
            ray_release(set_hdr);
            return NULL;
        }
        ofv = ix->u.hash.offs;
        rwv = ix->u.hash.rows;
        int es = numeric_elem_size(col->type);
        int64_t ngid = 0;
        for (int64_t si = 0; si < set_len; si++) {
            int64_t key = set_scratch[si];
            int64_t kw = col_is_sym ? key
                                    : (int64_t)hash_key_bits(es, key);
            int64_t gid = -1;
            uint64_t slot = hx_home(ix, mix64((uint64_t)kw));
            const ray_t* tab = ix->u.hash.table;
            const int64_t* gk  = (const int64_t*)ray_data(ix->u.hash.gkeys);
            for (;;) {
                int64_t gp1 = hx_get(tab, slot);
                if (gp1 == 0) break;
                if (gk[gp1 - 1] == kw) { gid = gp1 - 1; break; }
                slot = (slot + 1) & ix->u.hash.mask;
            }
            if (gid >= 0) {
                gids[ngid++] = gid;
                total += hx_get(ofv, gid + 1) - hx_get(ofv, gid);
            }
        }
        set_len = ngid;
    }
    ray_release(set_hdr);

    /* Dense union: the SIMD membership scan wins — bail with zero waste.
     * Tighter than the eq probe's n/8: a multi-key union pays an
     * O(m log m) qsort the single-slice path doesn't. */
    if (total > 64 && total > (n >> 4)) {
        scratch_free(gid_hdr);
        return NULL;
    }
    if (total == 0) {
        scratch_free(gid_hdr);
        return rowsel_from_sorted_ids(n, NULL, 0);
    }

    /* Concatenate the groups' ascending slices and sort once — distinct
     * keys → disjoint slices, no duplicate row ids. */
    ray_t* match_hdr = ray_alloc(total * (int64_t)sizeof(int64_t));
    if (!match_hdr) { scratch_free(gid_hdr); return NULL; }
    int64_t* matches = (int64_t*)ray_data(match_hdr);
    int64_t mcnt = 0;
    for (int64_t si = 0; si < set_len; si++) {
        int64_t gid = gids[si];
        int64_t gsz = hx_get(ofv, gid + 1) - hx_get(ofv, gid);
        hx_copy_rows(matches + mcnt, rwv, hx_get(ofv, gid), gsz);
        mcnt += gsz;
    }
    scratch_free(gid_hdr);
    if (set_len > 1 && mcnt > 1)
        qsort(matches, (size_t)mcnt, sizeof(int64_t), hash_match_cmp_i64);

    ray_t* block = rowsel_from_sorted_ids(n, matches, mcnt);
    ray_release(match_hdr);
    return block;
}

/* --------------------------------------------------------------------------
 * Sort-index range probe
 *
 * Binary search over the sort permutation.  Two typed helpers — one for
 * integer-family columns, one for float-family — avoid branching inside the
 * hot search loop.
 *
 * Guard: O(m log m) row-id sort + rowsel build must stay under O(n).
 * Measured break-even on shuffled data is ~0.5-1% selectivity (loses +1.9ms
 * at 1%, wins -7.8ms at 0.1% on 10M rows).  1/128 (~0.78%) keeps the 0.1%
 * win region and rejects the loss region.  Sorted-layout 1% wins are forfeited
 * because the guard is layout-blind (no RAY_ATTR_SORTED signal yet).
 * See bench/bottleneck/idx_route_compare.md ROUND 2 Q1 for the curve.
 * -------------------------------------------------------------------------- */

/* Bail when the qualifying span exceeds len/IDX_RANGE_MAX_FRAC — the
 * O(m log m) row-id sort below must stay under the scan's O(n) cost.
 * 128 (~0.78%) sits just below the ~0.5-1% shuffled-data break-even measured
 * in bench/bottleneck/idx_route_compare.md ROUND 2 Q1. */
#define IDX_RANGE_MAX_FRAC 128

/* Read row rid of an integer-family column as int64. */
static int64_t sort_read_i64(const uint8_t* base, int8_t t, int64_t rid) {
    return hash_col_read_i64(base, t, rid);
}

/* Read row rid of a float-family column as double. */
static double sort_read_f64(const uint8_t* base, int8_t t, int64_t rid) {
    if (t == RAY_F32) {
        float v; memcpy(&v, base + rid * 4, 4); return (double)v;
    }
    double v; memcpy(&v, base + rid * 8, 8); return v;
}

/* Build a rowsel for the contiguous physical-row interval [lo, hi).  Full
 * morsels are represented by an ALL flag and consume no idx[] space; only
 * the two possible boundary morsels contribute local row ids. */
static ray_t* rowsel_from_contiguous_span(int64_t n, int64_t lo, int64_t hi) {
    if (n < 0 || lo < 0 || hi < lo || hi > n) return NULL;

    int64_t total = hi - lo;
    uint32_t n_segs = n > 0
        ? (uint32_t)((n + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS)
        : 0;
    int64_t idx_count = 0;
    for (uint32_t s = 0; s < n_segs; s++) {
        int64_t ss = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t se = ss + RAY_MORSEL_ELEMS;
        if (se > n) se = n;
        int64_t a = lo > ss ? lo : ss;
        int64_t b = hi < se ? hi : se;
        int64_t pass = b > a ? b - a : 0;
        if (pass > 0 && pass != se - ss) idx_count += pass;
    }

    ray_t* block = ray_rowsel_new(n, total, idx_count);
    if (!block) return NULL;
    uint8_t* flags = ray_rowsel_flags(block);
    uint32_t* offsets = ray_rowsel_offsets(block);
    uint16_t* ids = ray_rowsel_idx(block);
    uint32_t cum = 0;
    for (uint32_t s = 0; s < n_segs; s++) {
        offsets[s] = cum;
        int64_t ss = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t se = ss + RAY_MORSEL_ELEMS;
        if (se > n) se = n;
        int64_t a = lo > ss ? lo : ss;
        int64_t b = hi < se ? hi : se;
        int64_t pass = b > a ? b - a : 0;
        if (pass == 0) {
            flags[s] = RAY_SEL_NONE;
        } else if (pass == se - ss) {
            flags[s] = RAY_SEL_ALL;
        } else {
            flags[s] = RAY_SEL_MIX;
            for (int64_t r = a; r < b; r++)
                ids[cum++] = (uint16_t)(r - ss);
        }
    }
    offsets[n_segs] = cum;
    return block;
}

/* lower_bound_i: first sorted position pos where value_at(perm[pos]) >= key.
 * Positions in [0, n) are searched; perm is the sort permutation. */
static int64_t sort_lower_i(const int64_t* perm, int64_t n,
                            const uint8_t* base, int8_t t, int64_t key) {
    int64_t lo = 0, hi = n;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (sort_read_i64(base, t, perm[mid]) < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* upper_bound_i: first sorted position pos where value_at(perm[pos]) > key. */
static int64_t sort_upper_i(const int64_t* perm, int64_t n,
                            const uint8_t* base, int8_t t, int64_t key) {
    int64_t lo = 0, hi = n;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (sort_read_i64(base, t, perm[mid]) <= key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* lower_bound_f: first position where value >= key (NaN-safe: NaN is > any). */
static int64_t sort_lower_f(const int64_t* perm, int64_t n,
                            const uint8_t* base, int8_t t, double key) {
    int64_t lo = 0, hi = n;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        double v = sort_read_f64(base, t, perm[mid]);
        if (!isnan(v) && v < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* upper_bound_f: first position where value > key. */
static int64_t sort_upper_f(const int64_t* perm, int64_t n,
                            const uint8_t* base, int8_t t, double key) {
    int64_t lo = 0, hi = n;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        double v = sort_read_f64(base, t, perm[mid]);
        if (!isnan(v) && v <= key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

ray_t* ray_index_range_rowsel(ray_t* col, uint16_t cmp_op,
                              int64_t key_i, double key_f, bool is_float) {
    /* NE = two spans — unsupported. */
    if (cmp_op == OP_NE) return NULL;

    /* Freshness + no-null gate. */
    if (!idx_fresh_nonull(col, RAY_IDX_SORT)) return NULL;

    /* Consistency check: is_float must agree with the column's type family. */
    bool col_is_float = (col->type == RAY_F32 || col->type == RAY_F64);
    if ((bool)is_float != col_is_float) return NULL;

    ray_index_t* ix = ray_index_payload(col->index);
    if (!ix->u.sort.perm) return NULL;

    int64_t n = col->len;
    const int64_t* perm = (const int64_t*)ray_data(ix->u.sort.perm);
    const uint8_t* base = (const uint8_t*)ray_data(col);
    int8_t t = col->type;

    /* Compute [lo, hi) span in sorted order. */
    int64_t lo, hi;
    if (!is_float) {
        int64_t lower = sort_lower_i(perm, n, base, t, key_i);
        int64_t upper = sort_upper_i(perm, n, base, t, key_i);
        switch (cmp_op) {
        case OP_LT: lo = 0;     hi = lower; break;
        case OP_LE: lo = 0;     hi = upper; break;
        case OP_GT: lo = upper; hi = n;     break;
        case OP_GE: lo = lower; hi = n;     break;
        case OP_EQ: lo = lower; hi = upper; break;
        default:    return NULL;
        }
    } else {
        int64_t lower = sort_lower_f(perm, n, base, t, key_f);
        int64_t upper = sort_upper_f(perm, n, base, t, key_f);
        switch (cmp_op) {
        case OP_LT: lo = 0;     hi = lower; break;
        case OP_LE: lo = 0;     hi = upper; break;
        case OP_GT: lo = upper; hi = n;     break;
        case OP_GE: lo = lower; hi = n;     break;
        case OP_EQ: lo = lower; hi = upper; break;
        default:    return NULL;
        }
    }

    int64_t span = hi - lo;
    if (span <= 0) return ray_index_empty_rowsel(n);
    /* Selectivity guard: only worth paying the O(m log m) sort when the
     * span is small relative to the column AND the column is large enough
     * that a scan would be expensive.  Below 64 rows the scan is trivial
     * regardless of selectivity, so skip the guard. */
    if (n >= 64 && span > n / IDX_RANGE_MAX_FRAC) return NULL;

    /* Copy perm[lo..hi) into a scratch buffer, sort ascending, build rowsel. */
    ray_t* scratch = ray_alloc(span * (int64_t)sizeof(int64_t));
    if (!scratch) return NULL;
    int64_t* ids = (int64_t*)ray_data(scratch);
    for (int64_t i = 0; i < span; i++) ids[i] = perm[lo + i];

    if (span > 1)
        qsort(ids, (size_t)span, sizeof(int64_t), hash_match_cmp_i64);

    ray_t* block = rowsel_from_sorted_ids(n, ids, span);
    ray_release(scratch);
    return block;
}

ray_t* ray_sorted_range_rowsel(ray_t* col, uint16_t cmp_op,
                               int64_t key_i, double key_f, bool is_float) {
    if (!col || RAY_IS_ERR(col) || cmp_op == OP_NE) return NULL;
    if (!ray_attr_is_sorted(col) || ray_vec_may_have_nulls(col))
        return NULL;

    bool col_is_float = (col->type == RAY_F32 || col->type == RAY_F64);
    bool col_is_int = col->type == RAY_BOOL || col->type == RAY_U8 ||
                      col->type == RAY_I16 || col->type == RAY_I32 ||
                      col->type == RAY_I64 || col->type == RAY_DATE ||
                      col->type == RAY_TIME || col->type == RAY_TIMESTAMP;
    if (!col_is_float && !col_is_int) return NULL;
    if ((bool)is_float != col_is_float) return NULL;

    int64_t n = col->len;
    const uint8_t* base = (const uint8_t*)ray_data(col);
    int8_t t = col->type;
    int64_t lower = 0, upper = 0;
    if (!is_float) {
        int64_t hi = n;
        while (lower < hi) {
            int64_t mid = lower + (hi - lower) / 2;
            if (sort_read_i64(base, t, mid) < key_i) lower = mid + 1;
            else hi = mid;
        }
        upper = lower;
        hi = n;
        while (upper < hi) {
            int64_t mid = upper + (hi - upper) / 2;
            if (sort_read_i64(base, t, mid) <= key_i) upper = mid + 1;
            else hi = mid;
        }
    } else {
        int64_t hi = n;
        while (lower < hi) {
            int64_t mid = lower + (hi - lower) / 2;
            if (sort_read_f64(base, t, mid) < key_f) lower = mid + 1;
            else hi = mid;
        }
        upper = lower;
        hi = n;
        while (upper < hi) {
            int64_t mid = upper + (hi - upper) / 2;
            if (sort_read_f64(base, t, mid) <= key_f) upper = mid + 1;
            else hi = mid;
        }
    }

    int64_t lo, hi;
    switch (cmp_op) {
    case OP_LT: lo = 0;     hi = lower; break;
    case OP_LE: lo = 0;     hi = upper; break;
    case OP_GT: lo = upper; hi = n;     break;
    case OP_GE: lo = lower; hi = n;     break;
    case OP_EQ: lo = lower; hi = upper; break;
    default: return NULL;
    }
    return rowsel_from_contiguous_span(n, lo, hi);
}

/* --------------------------------------------------------------------------
 * Bloom-index definite-absent probe
 *
 * Builder formula (ray_index_attach_bloom above):
 *   h  = mix64(numeric_key_word(base, type, row))
 *   h1 = h
 *   h2 = mix64(h ^ 0xc6a4a7935bd1e995ULL) | 1ULL   -- ensure odd (double-hashing)
 *   for kk in 0..k-1:
 *       pos = (h1 + kk * h2) & m_mask
 *       bits[pos >> 3] |= 1 << (pos & 7)
 *
 * For an integer key the raw bit representation used by numeric_key_word is:
 *   zero-extend  for U8/BOOL (1-byte storage)
 *   sign-extend  for I16 (2-byte storage)
 *   sign-extend  for I32/DATE/TIME (4-byte storage)
 *   identity     for I64/TIMESTAMP (8-byte storage)
 * which is exactly (uint64_t)(int64_t)key after the appropriate truncation.
 *
 * We derive kbits the same way hash_probe_setup does for the HASH index
 * (see hash_probe_setup, ~line 773), then run the same double-hash probe.
 * -------------------------------------------------------------------------- */

bool ray_index_bloom_absent(ray_t* col, int64_t key) {
    /* idx_fresh_nonull: freshness + kind + no-null gate. */
    if (!idx_fresh_nonull(col, RAY_IDX_BLOOM)) return false;

    /* Integer-family only in v1.  F32/F64 equality has NaN/-0 semantics
     * that the unfused kernel handles; skip bloom for float columns. */
    int8_t t = col->type;
    if (t == RAY_F32 || t == RAY_F64) return false;

    /* Derive kbits: mirror numeric_key_word for an integer scalar key.
     * numeric_key_word uses the raw bit pattern of the storage width; for
     * integer types that means zero-extension (U8/BOOL) or sign-extension
     * (I16/I32/I64 family) to uint64.  No range check needed: an out-of-
     * range key hashes to some bit positions; if all happen to be set we
     * fall through harmlessly — the absent proof is still sound. */
    int es = numeric_elem_size(t);
    uint64_t kbits;
    switch (es) {
    case 1:  kbits = (uint64_t)(uint8_t)key;                break;
    case 2:  kbits = (uint64_t)(int64_t)(int16_t)key;       break;
    case 4:  kbits = (uint64_t)(int64_t)(int32_t)key;       break;
    default: kbits = (uint64_t)key;                         break;
    }

    ray_index_t* ix    = ray_index_payload(col->index);
    uint64_t     mask  = ix->u.bloom.m_mask;
    uint32_t     k     = ix->u.bloom.k;
    const uint8_t* bbuf = (const uint8_t*)ray_data(ix->u.bloom.bits);

    /* Double-hashing probe — exact mirror of the builder loop. */
    uint64_t h  = mix64(kbits);
    uint64_t h1 = h;
    uint64_t h2 = mix64(h ^ 0xc6a4a7935bd1e995ULL) | 1ULL;

    for (uint32_t kk = 0; kk < k; kk++) {
        uint64_t pos = (h1 + (uint64_t)kk * h2) & mask;
        if (!(bbuf[pos >> 3] & (uint8_t)(1u << (pos & 7))))
            return true;   /* bit clear → key provably absent */
    }
    return false;  /* all bits set → maybe present, fall through */
}

/* --------------------------------------------------------------------------
 * Sort index — ascending permutation of row ids
 *
 * Delegates to the existing parallel sort builder.  Result is an I64 vec of
 * length parent->len with the default null placement — a null is the
 * smallest value, so nulls come first on ascending.  An index persisted
 * before that rule changed carries the old order; idx_fresh_nonull keeps
 * such an index off the consult paths, which is what makes the difference
 * unobservable rather than merely unlikely.
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
 * Sort-index ORDER BY permutation probe
 * -------------------------------------------------------------------------- */

ray_t* ray_index_sort_perm_fresh(ray_t* col) {
    if (!idx_fresh_nonull(col, RAY_IDX_SORT)) return NULL;
    ray_index_t* ix = ray_index_payload(col->index);
    return ix->u.sort.perm;
}

/* --------------------------------------------------------------------------
 * Sort-index distinct fast path
 *
 * Walk the sort permutation once.  The perm is a stable ascending sort with
 * iota tie-breaking, so within each equal-value run the row ids appear in
 * ascending order; the FIRST element of each run is already the minimum row
 * id (= first occurrence) for that value.
 *
 * After collecting the first-in-run ids we sort them by VALUE (not by row)
 * to match distinct_vec_eager's distinct_sort_indices contract: output is
 * numerically sorted, not first-occurrence ordered.  Because the perm
 * already provides one representative per value in ascending value order, we
 * can emit them directly without a second sort — the perm walk order is
 * already value-ascending, which is exactly the required output order.
 * -------------------------------------------------------------------------- */

ray_t* ray_index_distinct_ids(ray_t* col, int64_t* out_count) {
    *out_count = 0;
    if (!idx_fresh_nonull(col, RAY_IDX_SORT)) return NULL;

    ray_index_t* ix  = ray_index_payload(col->index);
    ray_t*       perm = ix->u.sort.perm;
    int64_t      n    = perm->len;
    if (n == 0) {
        ray_t* empty = ray_vec_new(RAY_I64, 0);
        return empty;
    }

    /* Allocate output array (worst case: all values distinct). */
    ray_t* out = ray_vec_new(RAY_I64, n);
    if (!out || RAY_IS_ERR(out)) return NULL;

    const int64_t* p    = (const int64_t*)ray_data(perm);
    const uint8_t* base = (const uint8_t*)ray_data(col);
    int64_t* ids        = (int64_t*)ray_data(out);
    int64_t  count      = 0;

    /* First element of the perm always starts the first run. */
    ids[count++] = p[0];
    for (int64_t i = 1; i < n; i++) {
        /* Compare value at current perm position to previous one.
         * The perm is sorted ascending, so equal values form contiguous runs;
         * we emit one id per run (the first element, which is the min row id
         * because the sort is stable with iota tie-breaking). */
        if (numeric_key_word(base, col->type, p[i]) !=
            numeric_key_word(base, col->type, p[i-1])) {
            ids[count++] = p[i];
        }
    }

    /* The perm walk produces ids in VALUE-ascending order (perm is sorted
     * ascending).  distinct_vec_eager's contract (from distinct_sort_indices)
     * is also value-ascending, so no second sort is needed. */
    out->len   = count;
    *out_count = count;
    return out;
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
 * Part index — contiguous value-blocks
 *
 * Build a RAY_IDX_PART index. Numeric columns must be laid out as ascending
 * value-blocks. SYM columns must be laid out as contiguous value-blocks in the
 * column's domain id space; the block order itself is allowed to follow another
 * comparator such as a string lexical sort.
 * -------------------------------------------------------------------------- */

ray_t* ray_index_attach_part(ray_t** vp) {
    ray_t* v = prepare_attach_ex(vp, "part", false, true);
    if (RAY_IS_ERR(v)) return v;
    int64_t n = v->len;

    if (v->type != RAY_SYM && !vec_is_ascending(v))
        return ray_error("domain", "parted: column is not laid out as ascending value-blocks");
    if (v->type == RAY_SYM && ray_vec_may_have_nulls(v)) {
        for (int64_t i = 0; i < n; i++)
            if (ray_vec_is_null(v, i))
                return ray_error("domain", "parted: column contains nulls");
    }

    const uint8_t* base = (const uint8_t*)ray_data(v);
    int es = numeric_elem_size(v->type);

    /* Row i starts a new value-block iff it differs from i-1. */
    #define PART_NEW_BLOCK(i) \
        ((v->type == RAY_SYM) \
            ? (ray_read_sym(base, (i), RAY_SYM, v->attrs) != \
               ray_read_sym(base, (i)-1, RAY_SYM, v->attrs)) \
            : (numeric_key_word(base, v->type, (i)) != \
               numeric_key_word(base, v->type, (i)-1)))

    /* Count runs of equal values. */
    int64_t nparts = (n > 0) ? 1 : 0;
    for (int64_t i = 1; i < n; i++)
        if (PART_NEW_BLOCK(i))
            nparts++;

    ray_t* seen_hdr = NULL;
    int64_t* seen = NULL;
    uint64_t seen_mask = 0;
    if (v->type == RAY_SYM && nparts > 1) {
        uint64_t cap = next_pow2((uint64_t)nparts * 2u);
        if (cap < 8) cap = 8;
        seen = (int64_t*)scratch_alloc(&seen_hdr, (size_t)cap * sizeof(int64_t));
        if (!seen) return ray_error("oom", NULL);
        for (uint64_t i = 0; i < cap; i++) seen[i] = -1;
        seen_mask = cap - 1;
    }

    int64_t cap = nparts > 0 ? nparts : 1;
    ray_t* starts = ray_vec_new(RAY_I64, cap);
    ray_t* lens   = ray_vec_new(RAY_I64, cap);
    ray_t* keys   = ray_vec_new(v->type, cap);
    if (RAY_IS_ERR(starts) || RAY_IS_ERR(lens) || RAY_IS_ERR(keys)) {
        scratch_free(seen_hdr);
        if (!RAY_IS_ERR(starts)) ray_release(starts);
        if (!RAY_IS_ERR(lens))   ray_release(lens);
        if (!RAY_IS_ERR(keys))   ray_release(keys);
        return ray_error("oom", NULL);
    }
    if (v->type == RAY_SYM)
        ray_sym_vec_adopt_domain(keys, v);
    starts->len = lens->len = keys->len = nparts;
    int64_t* st = (int64_t*)ray_data(starts);
    int64_t* ln = (int64_t*)ray_data(lens);
    uint8_t* kb = (uint8_t*)ray_data(keys);

    int64_t p = 0, run_start = 0;
    for (int64_t i = 1; i <= n; i++) {
        bool boundary = (i == n) || PART_NEW_BLOCK(i);
        if (boundary && n > 0) {
            if (v->type == RAY_SYM && seen) {
                int64_t kv = ray_read_sym(base, run_start, RAY_SYM, v->attrs);
                uint64_t slot = mix64((uint64_t)kv) & seen_mask;
                for (;;) {
                    if (seen[slot] == -1) { seen[slot] = kv; break; }
                    if (seen[slot] == kv) {
                        scratch_free(seen_hdr);
                        ray_release(starts); ray_release(lens); ray_release(keys);
                        return ray_error("domain", "parted: column has repeated non-contiguous blocks");
                    }
                    slot = (slot + 1) & seen_mask;
                }
            }
            st[p] = run_start;
            ln[p] = i - run_start;
            if (v->type == RAY_SYM) {
                int64_t kv = ray_read_sym(base, run_start, RAY_SYM, v->attrs);
                write_col_i64(kb, p, kv, RAY_SYM, keys->attrs);
            } else {
                memcpy(kb + (size_t)p*es, base + (size_t)run_start*es, (size_t)es);
            }
            p++;
            run_start = i;
        }
    }
    #undef PART_NEW_BLOCK
    scratch_free(seen_hdr);

    ray_t* idx = ray_index_alloc(RAY_IDX_PART, v->type, n);
    if (!idx || RAY_IS_ERR(idx)) {
        ray_release(starts); ray_release(lens); ray_release(keys);
        return idx ? idx : ray_error("oom", NULL);
    }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.part.keys    = keys;
    ix->u.part.starts  = starts;
    ix->u.part.lens    = lens;
    ix->u.part.n_parts = nparts;
    ix->u.part.order_sym = -1;
    return attach_finalize(v, idx);
}

/* --------------------------------------------------------------------------
 * Detach (drop)
 *
 * Restore the parent's 16-byte aux union from the saved snapshot, then
 * release the index ray_t.  The release path of RAY_INDEX would otherwise
 * also try to release the saved-aux pointers, so we clear the saved
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
     * still the live, owned index ray_t — EXCEPT for a table, whose key map
     * ray_alloc_copy deliberately does not carry to the copy.  The copy has
     * no index left to detach, which is the state this function exists to
     * reach, so it is already done. */
    if (!(v->attrs & RAY_ATTR_HAS_INDEX) || !v->index) return v;
    ray_t* idx = v->index;
    ray_index_t* ix = ray_index_payload(idx);

    /* Shared-index case: another vec may share this RAY_INDEX block via
     * ray_alloc_copy (rc>1).  Don't clobber the snapshot in that case —
     * the other holder still reads it.  See vec_drop_index_inplace for
     * the same pattern. */
    /* A mapped index (mmod 1) rides the column file's mapping: copies of
     * the column borrow it without a reference, and the mapping's owner
     * unmaps it.  Dropping it from a vector only detaches it — the
     * snapshot stays for the other holders and nothing is released. */
    bool mapped = idx->mmod == 1;
    bool shared = mapped || ray_atomic_load(&idx->rc) > 1;
    if (shared) {
        ray_index_retain_saved(ix);
    }
    memcpy(v->aux, ix->saved_aux, 16);
    if (!shared) {
        memset(ix->saved_aux, 0, 16);
        ix->saved_attrs = 0;
    }

    /* Restore parent attrs.  HAS_NULLS was preserved through the
     * attachment so it needs no restoration. */
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_INDEX;

    /* Release the index.  Per-kind children are released by the RAY_INDEX
     * branch of ray_release_owned_refs (added in heap.c). */
    if (!mapped) ray_release(idx);
    return v;
}

/* --------------------------------------------------------------------------
 * Info
 * -------------------------------------------------------------------------- */

static const char* kind_name(ray_idx_kind_t k) {
    switch (k) {
    case RAY_IDX_HASH:       return "hash";
    case RAY_IDX_SORT:       return "sort";
    case RAY_IDX_ZONE:       return "zone";
    case RAY_IDX_BLOOM:      return "bloom";
    case RAY_IDX_CHUNK_ZONE: return "chunk_zone";
    case RAY_IDX_PART:       return "part";
    case RAY_IDX_DICT:       return "dict";
    default:                 return "none";
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
    case RAY_IDX_UKEY:
        r = dict_append_sym_i64(&keys, &vals, "n_keys", ix->u.ukey.nrows);
        if (RAY_IS_ERR(r)) goto fail;
        r = dict_append_sym_i64(&keys, &vals, "capacity", (int64_t)(ix->u.ukey.mask + 1));
        if (RAY_IS_ERR(r)) goto fail;
        r = dict_append_sym_i64(&keys, &vals, "n_key_cols", ix->u.ukey.nk);
        if (RAY_IS_ERR(r)) goto fail;
        r = dict_append_sym_i64(&keys, &vals, "n_tombstones", ix->u.ukey.n_tomb);
        if (RAY_IS_ERR(r)) goto fail;
        break;
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
        r = dict_append_sym_i64(&keys, &vals, "n_groups", ix->u.hash.n_groups);
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
    case RAY_IDX_CHUNK_ZONE:
        r = dict_append_sym_i64(&keys, &vals, "n_chunks",
                                (int64_t)ix->u.chunk_zone.n_chunks);
        if (RAY_IS_ERR(r)) goto fail;
        r = dict_append_sym_i64(&keys, &vals, "chunk_log2",
                                (int64_t)ix->u.chunk_zone.chunk_log2);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_PART:
        r = dict_append_sym_i64(&keys, &vals, "n_parts", ix->u.part.n_parts);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_DICT:
        r = dict_append_sym_i64(&keys, &vals, "n_distinct", ix->u.dict.n_distinct);
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
    /* fn() receives &w only to REWRITE w's heap value (COW); it never
     * returns the local's address — cppcheck 2.13 infers r could alias
     * &w through the callback and misfires returnDanglingLifetime. */
    ray_t* r = fn(&w);
    // cppcheck-suppress returnDanglingLifetime
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

/* --------------------------------------------------------------------------
 * Semantic attributes — (.attr.*) family.  See docs spec
 * 2026-06-01-column-attributes-design.md.
 * -------------------------------------------------------------------------- */

/* unique: verify distinctness, then set a block-resident marker bit.  If the
 * column already carries an index block, clone it (it is shared post-cow) and
 * mark the clone; otherwise attach a marker-only block (kind NONE). */
static ray_t* attr_set_unique(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("type", "attr: null");
    if (!ray_is_vec(v))
        return ray_error("type", "unique: attribute applies to vectors only");
    if (numeric_elem_size(v->type) == 0 && v->type != RAY_SYM && v->type != RAY_STR)
        return ray_error("nyi", "unique: only numeric/sym/str vectors supported (type %d)", (int)v->type);
    if (v->attrs & RAY_ATTR_SLICE)
        return ray_error("type", "unique: cannot attribute a slice; materialize first");
    if (!vec_all_distinct(v))
        return ray_error("domain", "unique: column contains duplicate values");

    ray_retain(v);
    ray_t* w = ray_cow(v);
    if (!w || RAY_IS_ERR(w)) return w ? w : ray_error("oom", NULL);

    if (w->attrs & RAY_ATTR_HAS_INDEX) {
        /* Post-cow w->index is SHARED with the original (ray_cow retains the
         * block, it does not deep-copy it).  Clone it before mutating markers
         * so the original's block is untouched.  Then the clone is sole-owned. */
        ray_t* nb = clone_index_block(w->index);
        if (!nb || RAY_IS_ERR(nb)) { ray_release(w); return nb ? nb : ray_error("oom", NULL); }
        ray_release(w->index);
        w->index = nb;
        ray_index_payload(nb)->markers |= RAY_MARK_UNIQUE;
        return w;
    }
    /* No backing index: attach a marker-only block (kind NONE). */
    ray_t* idx = ray_index_alloc(RAY_IDX_NONE, w->type, w->len);
    if (!idx || RAY_IS_ERR(idx)) { ray_release(w); return idx ? idx : ray_error("oom", NULL); }
    ray_index_payload(idx)->markers = RAY_MARK_UNIQUE;
    return attach_finalize(w, idx);
}

/* Build a backing index via `fn` (drop-and-rebuild through prepare_attach),
 * carrying block-resident metadata that remains true for the unchanged row
 * order.  Used by grouped and parted, which replace the backing index but must
 * preserve markers such as unique and the optional within-group order marker.
 * The sorted marker lives in attrs (not the block) and survives attach. */
static int64_t index_order_sym(ray_t* v) {
    if (!v || RAY_IS_ERR(v) || !ray_index_has(v))
        return -1;
    ray_index_t* ix = ray_index_payload(v->index);
    if (!ix || ix->built_for_len != v->len)
        return -1;
    if (ix->kind == RAY_IDX_HASH)
        return ix->u.hash.order_sym;
    if (ix->kind == RAY_IDX_PART)
        return ix->u.part.order_sym;
    return -1;
}

static void index_set_order_sym(ray_t* v, int64_t order_sym) {
    if (!v || RAY_IS_ERR(v) || order_sym < 0 || !ray_index_has(v))
        return;
    ray_index_t* ix = ray_index_payload(v->index);
    if (!ix || ix->built_for_len != v->len)
        return;
    if (ix->kind == RAY_IDX_HASH)
        ix->u.hash.order_sym = order_sym;
    else if (ix->kind == RAY_IDX_PART)
        ix->u.part.order_sym = order_sym;
}

static ray_t* attach_backing_index_carry_markers(ray_t* v, ray_t* (*fn)(ray_t**)) {
    /* The rebuilt table keeps its own home rule. */
    uint8_t carry = (v && !RAY_IS_ERR(v) && ray_index_has(v))
                    ? (uint8_t)(ray_index_payload(v->index)->markers & ~RAY_MARK_HASH_HIGH) : 0;
    int64_t order_sym = index_order_sym(v);
    ray_t* w = attach_via(v, fn);
    if (w && !RAY_IS_ERR(w) && carry && (w->attrs & RAY_ATTR_HAS_INDEX))
        ray_index_payload(w->index)->markers |= carry;
    index_set_order_sym(w, order_sym);
    return w;
}

/* grouped == hash index. */
static ray_t* attr_set_grouped(ray_t* v) {
    return attach_backing_index_carry_markers(v, ray_index_attach_hash);
}

/* parted == contiguous ascending value-block index. */
static ray_t* attr_set_parted(ray_t* v) {
    return attach_backing_index_carry_markers(v, ray_index_attach_part);
}

/* Set the sorted marker after verifying.  Borrowed v in, owning ref out. */
static ray_t* attr_set_sorted(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("type", "attr: null");
    if (!ray_is_vec(v))
        return ray_error("type", "sorted: attribute applies to vectors only");
    if (numeric_elem_size(v->type) == 0)
        return ray_error("nyi", "sorted: only numeric vectors supported in v1 (type %d)",
                         (int)v->type);
    if (!vec_is_ascending(v))
        return ray_error("domain", "sorted: column is not in non-descending order");
    ray_retain(v);
    ray_t* w = ray_cow(v);
    if (!w || RAY_IS_ERR(w)) return w ? w : ray_error("oom", NULL);
    w->attrs |= RAY_ATTR_SORTED;
    return w;
}

/* (.attr.get v) -> symbol vector of attributes held (sorted, unique, grouped,
 * parted), or the empty symbol vector. */
ray_t* ray_attr_get_fn(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* syms = ray_vec_new(RAY_SYM, 4);
    if (!syms || RAY_IS_ERR(syms)) return syms ? syms : ray_error("oom", NULL);
    syms->len = 0;
    int64_t* out = (int64_t*)ray_data(syms);
    if (ray_attr_is_sorted(v))
        out[syms->len++] = ray_sym_intern_runtime("sorted", 6);
    if (ray_index_has(v)) {
        ray_index_t* ix = ray_index_payload(v->index);
        if (ix->markers & RAY_MARK_UNIQUE)
            out[syms->len++] = ray_sym_intern_runtime("unique", 6);
        if (ix->kind == RAY_IDX_HASH)
            out[syms->len++] = ray_sym_intern_runtime("grouped", 7);
        else if (ix->kind == RAY_IDX_PART)
            out[syms->len++] = ray_sym_intern_runtime("parted", 6);
    }
    return syms;
}

/* (.attr.drop v) -> v with all attributes and any backing index removed. */
ray_t* ray_attr_drop_fn(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* w = v;
    ray_retain(w);
    if (w->attrs & RAY_ATTR_HAS_INDEX) {
        ray_t* r = ray_index_drop(&w);
        if (RAY_IS_ERR(r)) { ray_release(w); return r; }
    }
    if (w->attrs & RAY_ATTR_SORTED) {
        ray_t* c = ray_cow(w);
        if (!c || RAY_IS_ERR(c)) return c ? c : ray_error("oom", NULL);
        c->attrs &= ~RAY_ATTR_SORTED;
        w = c;
    }
    return w;
}

/* (.attr.set 'name v) — dispatch on the symbol name. */
ray_t* ray_attr_set_fn(ray_t* name, ray_t* v) {
    if (RAY_IS_ERR(name)) return name;
    if (!name || name->type != -RAY_SYM)
        return ray_error("type", "attr.set: first arg must be a symbol");
    int64_t id = name->i64;
    if (id == ray_sym_intern_runtime("sorted", 6))  return attr_set_sorted(v);
    if (id == ray_sym_intern_runtime("unique", 6))   return attr_set_unique(v);
    if (id == ray_sym_intern_runtime("grouped", 7))  return attr_set_grouped(v);
    if (id == ray_sym_intern_runtime("parted", 6))   return attr_set_parted(v);
    return ray_error("domain", "attr.set: unknown attribute (want sorted/unique/grouped/parted)");
}

/* --------------------------------------------------------------------------
 * Hash-index find — minimum row id whose value equals key, or -1 / -2.
 * -------------------------------------------------------------------------- */

int64_t ray_index_find_row(ray_t* col, int64_t key) {
    /* Float-family: equality has NaN/-0 semantics owned by the scan kernel. */
    if (!col || RAY_IS_ERR(col)) return -2;
    int8_t t = col->type;
    if (t == RAY_F32 || t == RAY_F64) return -2;
    if (t == RAY_STR) return -2;   /* byte-keyed: see ray_index_find_atom */

    /* Nonzero symbol equality is safe even when other rows are null. */
    if (t == RAY_SYM) {
        if (key == 0 || !idx_fresh(col, RAY_IDX_HASH)) return -2;
    } else if (!idx_fresh_nonull(col, RAY_IDX_HASH)) return -2;

    /* Out-of-range key cannot equal any stored value of this type. */
    if (!hash_key_in_range(t, key)) return -1;

    int64_t gid = -1;
    ray_index_t* ix = hash_probe_setup(col, key, &gid);
    if (!ix) return -2;   /* unexpected failure after eligibility passed */
    if (gid < 0) return -1;  /* key provably absent */

    /* CSR: rows are ascending within the group — first slice entry IS the
     * minimum matching row id.  O(1). */
    return hx_get(ix->u.hash.rows, hx_get(ix->u.hash.offs, gid));
}

/* Public wrapper over the internal sorted-ids rowsel builder — consult
 * sites outside this file (the eq+range conjunction in exec.c) construct
 * selections from index slices. */

int64_t ray_index_sym_slices(ray_t* col, ray_t* keys,
                             ray_idx_slice_t** out, ray_t** hdr_out) {
    *out = NULL; *hdr_out = NULL;
    if (!col || !keys) return -1;
    if (col->type != RAY_SYM) return -1;
    ray_idx_kind_t kind = ray_index_kind(col);
    if ((kind != RAY_IDX_HASH && kind != RAY_IDX_PART) ||
        !idx_fresh(col, kind)) return -1;

    bool atom = (keys->type == -RAY_SYM);
    int64_t nkeys;
    if (atom) nkeys = 1;
    else if (keys->type == RAY_SYM && !ray_is_atom(keys)) nkeys = keys->len;
    else return -1;
    if (nkeys <= 0) return 0;

    /* Canonicalize: translate to column-domain ids (absent → drop),
     * sort ascending, collapse duplicates — the DA path's emit order. */
    ray_t* dhdr = ray_alloc((size_t)nkeys * (int64_t)sizeof(int64_t));
    if (!dhdr) return -1;
    int64_t* doms = (int64_t*)ray_data(dhdr);
    int64_t nd = 0;
    for (int64_t i = 0; i < nkeys; i++) {
        ray_t* str = atom ? ray_sym_str(keys->i64) : ray_sym_vec_cell(keys, i);
        if (!str) continue;
        int64_t dom = ray_sym_vec_lookup(col, ray_str_ptr(str), ray_str_len(str));
        if (dom == 0 && kind == RAY_IDX_HASH) { ray_free(dhdr); return -1; }
        if (dom >= 0) doms[nd++] = dom;
    }
    if (nd == 0) { ray_free(dhdr); return 0; }
    qsort(doms, (size_t)nd, sizeof(int64_t), cmp_i64_plain);
    int64_t w = 1;
    for (int64_t i = 1; i < nd; i++)
        if (doms[i] != doms[i - 1]) doms[w++] = doms[i];
    nd = w;

    ray_t* shdr = ray_alloc((size_t)nd * (int64_t)sizeof(ray_idx_slice_t));
    if (!shdr) { ray_free(dhdr); return -1; }
    ray_idx_slice_t* sl = (ray_idx_slice_t*)ray_data(shdr);
    int64_t k = 0;
    if (kind == RAY_IDX_HASH) {
        for (int64_t i = 0; i < nd; i++) {
            ray_idx_rows_t rows = { NULL, false };
            int64_t n = 0;
            int hit = ray_index_hash_group(col, doms[i], &rows, &n);
            if (hit < 0) { ray_free(dhdr); ray_free(shdr); return -1; }
            if (hit == 0 || n == 0) continue;
            sl[k].dom = doms[i]; sl[k].rows = rows;
            sl[k].first = 0; sl[k].n = n; k++;
        }
    } else {
        ray_index_t* ix = ray_index_payload(col->index);
        ray_t* pkeys = ix->u.part.keys;
        ray_t* starts = ix->u.part.starts;
        ray_t* lens = ix->u.part.lens;
        int64_t np = ix->u.part.n_parts;
        if (!pkeys || !starts || !lens || pkeys->len != np ||
            starts->len != np || lens->len != np) {
            ray_free(dhdr); ray_free(shdr); return -1;
        }
        const uint8_t* pk = (const uint8_t*)ray_data(pkeys);
        const int64_t* ps = (const int64_t*)ray_data(starts);
        const int64_t* pl = (const int64_t*)ray_data(lens);
        for (int64_t i = 0; i < nd; i++) {
            for (int64_t p = 0; p < np; p++) {
                int64_t pdom = ray_read_sym(pk, p, RAY_SYM, pkeys->attrs);
                if (pdom != doms[i]) continue;
                if (pl[p] > 0) {
                    sl[k].dom = doms[i]; sl[k].rows = (ray_idx_rows_t){ NULL, false };
                    sl[k].first = ps[p]; sl[k].n = pl[p]; k++;
                }
                break;
            }
        }
    }
    ray_free(dhdr);
    if (k == 0) { ray_free(shdr); return 0; }
    *out = sl; *hdr_out = shdr;
    return k;
}

ray_t* ray_index_rowsel_from_ids(int64_t nrows, const int64_t* ids,
                                 int64_t n) {
    return rowsel_from_sorted_ids(nrows, ids, n);
}

ray_t* ray_index_rowsel_from_rows(int64_t nrows, ray_idx_rows_t ids, int64_t n) {
    return rowsel_from_sorted_rows(nrows, ids, n);
}

int ray_index_hash_group(ray_t* col, int64_t key,
                         ray_idx_rows_t* rows_out, int64_t* n_out) {
    *rows_out = (ray_idx_rows_t){ NULL, false };
    *n_out = 0;
    if (!idx_fresh(col, RAY_IDX_HASH)) return -1;
    /* A STR hash is byte-keyed: an int64 key is not a probe of it, and
     * "out of range" below would wrongly read as provably absent. */
    if (col->type == RAY_STR) return -1;
    int64_t gid = -1;
    ray_index_t* ix = hash_probe_setup(col, key, &gid);
    if (!ix) {
        /* Out-of-range key on an otherwise-fresh index cannot match any
         * stored value — that is a provable absence, not ineligibility. */
        return hash_key_in_range(col->type, key) ? -1 : 0;
    }
    if (gid < 0) return 0;
    const ray_t* ofv = ix->u.hash.offs;
    *rows_out = hx_rows(ix->u.hash.rows, hx_get(ofv, gid));
    *n_out = hx_get(ofv, gid + 1) - hx_get(ofv, gid);
    return 1;
}

/* --------------------------------------------------------------------------
 * Hash-index find for one atom / a vector of needles (the eval-level `find`,
 * `in` and dict lookup) and the append carry.  Keyed per column type:
 * integer family → value (ray_index_find_row), SYM → the needle re-expressed
 * in the COLUMN's domain, STR → the needle's bytes.
 * -------------------------------------------------------------------------- */

/* Integer-family atom → int64 key.  Returns false for any other atom kind
 * (float and cross-family needles keep the scan's promotion semantics). */
static bool int_atom_key(const ray_t* a, int64_t* k) {
    switch (a->type) {
    case -RAY_I64:
    case -RAY_TIMESTAMP: *k = a->i64;            return true;
    case -RAY_I32:
    case -RAY_DATE:
    case -RAY_TIME:      *k = (int64_t)a->i32;   return true;
    case -RAY_I16:       *k = (int64_t)a->i16;   return true;
    case -RAY_BOOL:
    case -RAY_U8:        *k = (int64_t)a->b8;    return true;
    default:             return false;
    }
}

/* A runtime-domain symbol id → this column's domain id, or -1 when the
 * column's domain does not hold the symbol (provably absent). */
static int64_t sym_id_in_col_domain(ray_t* col, int64_t runtime_id) {
    if (ray_sym_vec_domain(col) == ray_sym_runtime_domain()) return runtime_id;
    ray_t* s = ray_sym_str(runtime_id);
    if (!s) return -1;
    return ray_sym_vec_lookup(col, ray_str_ptr(s), ray_str_len(s));
}

int64_t ray_index_find_atom(ray_t* col, ray_t* a) {
    if (!col || RAY_IS_ERR(col) || !a || RAY_IS_ERR(a)) return -2;
    if (!ray_is_atom(a) || RAY_ATOM_IS_NULL(a)) return -2;
    /* SYM mirrors ray_index_find_row: a nonzero symbol probe is sound even
     * when other rows are null, so only freshness is required there. */
    if (col->type == RAY_SYM ? !idx_fresh(col, RAY_IDX_HASH)
                             : !idx_fresh_nonull(col, RAY_IDX_HASH)) return -2;
    switch (col->type) {
    case RAY_SYM: {
        if (a->type != -RAY_SYM) return -2;
        int64_t id = sym_id_in_col_domain(col, a->i64);
        if (id < 0) return -1;
        return ray_index_find_row(col, id);   /* id 0 (SYM null) → -2 */
    }
    case RAY_STR: {
        if (a->type != -RAY_STR) return -2;
        int64_t gid = -1;
        ray_index_t* ix = hash_probe_setup_str(col, ray_str_ptr(a), ray_str_len(a), &gid);
        if (!ix) return -2;
        if (gid < 0) return -1;
        return hx_get(ix->u.hash.rows, hx_get(ix->u.hash.offs, gid));
    }
    default: {
        int64_t k = 0;
        if (!int_atom_key(a, &k)) return -2;
        return ray_index_find_row(col, k);
    }
    }
}

int ray_index_find_vec(ray_t* col, ray_t* nd, int64_t* out, bool* any_miss) {
    if (!col || RAY_IS_ERR(col) || !nd || RAY_IS_ERR(nd) || !out) return 0;
    if (!ray_is_vec(nd) || ray_is_atom(nd)) return 0;
    if (col->type == RAY_SYM ? !idx_fresh(col, RAY_IDX_HASH)
                             : !idx_fresh_nonull(col, RAY_IDX_HASH)) return 0;
    int64_t m = nd->len;
    bool miss = false;
    const uint8_t* nb = (const uint8_t*)ray_data(nd);
    bool nd_nulls = (nd->attrs & RAY_ATTR_HAS_NULLS) != 0;

    if (col->type == RAY_SYM) {
        if (nd->type != RAY_SYM) return 0;
        bool same_dom = (ray_sym_vec_domain(col) == ray_sym_vec_domain(nd));
        for (int64_t i = 0; i < m; i++) {
            int64_t id;
            if (same_dom) {
                id = ray_read_sym(nb, i, RAY_SYM, nd->attrs);
            } else {
                ray_t* s = ray_sym_vec_cell(nd, i);
                id = s ? ray_sym_vec_lookup(col, ray_str_ptr(s), ray_str_len(s)) : -1;
            }
            /* id 0 is the SYM null: the scan / hashset own null equality. */
            int64_t r = (id < 0) ? -1 : ray_index_find_row(col, id);
            if (r == -2) return 0;
            if (r < 0) { out[i] = NULL_I64; miss = true; } else out[i] = r;
        }
    } else if (col->type == RAY_STR) {
        if (nd->type != RAY_STR) return 0;
        for (int64_t i = 0; i < m; i++) {
            if (nd_nulls && ray_vec_is_null(nd, i)) { out[i] = NULL_I64; miss = true; continue; }
            size_t l = 0;
            const char* p = ray_str_vec_get(nd, i, &l);
            int64_t gid = -1;
            ray_index_t* ix = hash_probe_setup_str(col, p, l, &gid);
            if (!ix) return 0;
            if (gid < 0) { out[i] = NULL_I64; miss = true; continue; }
            out[i] = hx_get(ix->u.hash.rows, hx_get(ix->u.hash.offs, gid));
        }
    } else {
        /* Integer-family column and needles only: float equality (NaN, -0)
         * and cross-family promotion belong to the scan / hashset paths. */
        if (col->type == RAY_F32 || col->type == RAY_F64) return 0;
        if (nd->type == RAY_F32 || nd->type == RAY_F64 || numeric_elem_size(nd->type) == 0) return 0;
        for (int64_t i = 0; i < m; i++) {
            if (nd_nulls && ray_vec_is_null(nd, i)) { out[i] = NULL_I64; miss = true; continue; }
            int64_t r = ray_index_find_row(col, set_vec_read_i64(nb, nd->type, i));
            if (r == -2) return 0;
            if (r < 0) { out[i] = NULL_I64; miss = true; } else out[i] = r;
        }
    }
    if (any_miss) *any_miss = miss;
    return 1;
}

/* Carry `src`'s hash index onto `dst`, a fresh vector holding src's rows
 * followed by appended rows.  Rebuilds nothing over the old rows: the group
 * tables are copied and each appended row is probed once and added as a new
 * single-row group.  An appended row that repeats a key is left alone — the
 * CSR row slices would need re-laying (O(n), the re-attach cost the carry
 * exists to avoid) and a `unique` marker would stop being true — so dst
 * simply stays unindexed, as every concat result did before. */
void ray_index_carry_append(ray_t* src, ray_t* dst) {
    if (!src || RAY_IS_ERR(src) || !dst || RAY_IS_ERR(dst)) return;
    if (!ray_is_vec(dst) || dst->type != src->type || dst->len < src->len) return;
    if (dst->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SLICE | RAY_ATTR_HAS_INDEX)) return;
    if (!idx_fresh_nonull(src, RAY_IDX_HASH)) return;
    int8_t t = src->type;
    if (t == RAY_F32 || t == RAY_F64) return;
    if (t == RAY_SYM && ray_sym_vec_domain(src) != ray_sym_vec_domain(dst)) return;
    ray_index_t* sx = ray_index_payload(src->index);
    if (!sx->u.hash.table || !sx->u.hash.gkeys || !sx->u.hash.offs || !sx->u.hash.rows) return;

    int64_t n0 = src->len, n1 = dst->len, add = n1 - n0;
    int64_t og = sx->u.hash.n_groups, ok = sx->u.hash.n_keys;
    if (add > INT64_MAX - og || add > INT64_MAX - ok) return;   /* no signed overflow below */
    bool is_str = (t == RAY_STR);

    /* Built in the width they end in: narrow while every value (at most
     * n1) fits 32 bits, as hx_narrow would leave them. */
    int8_t aw = n1 <= (int64_t)UINT32_MAX ? RAY_I32 : RAY_I64;
    ray_t* gkeys = ray_vec_new(RAY_I64, og + add > 0 ? og + add : 1);
    ray_t* offs  = ray_vec_new(aw, og + add + 1);
    ray_t* rows  = ray_vec_new(aw, ok + add > 0 ? ok + add : 1);
    uint64_t cap = next_pow2((uint64_t)(og + add < 4 ? 8 : 2 * (og + add)));
    if (cap < 8) cap = 8;
    ray_t* table = ray_vec_new(aw, (int64_t)cap);
    if (!gkeys || RAY_IS_ERR(gkeys) || !offs || RAY_IS_ERR(offs) ||
        !rows || RAY_IS_ERR(rows) || !table || RAY_IS_ERR(table)) {
        if (gkeys && !RAY_IS_ERR(gkeys)) ray_release(gkeys);
        if (offs  && !RAY_IS_ERR(offs))  ray_release(offs);
        if (rows  && !RAY_IS_ERR(rows))  ray_release(rows);
        if (table && !RAY_IS_ERR(table)) ray_release(table);
        return;
    }
    int64_t* gk  = (int64_t*)ray_data(gkeys);
    uint64_t mask = cap - 1;
    memcpy(gk, ray_data(sx->u.hash.gkeys), (size_t)og * sizeof(int64_t));
    hx_copy_into(offs, sx->u.hash.offs, og + 1);
    hx_copy_into(rows, sx->u.hash.rows, ok);
    if (cap == sx->u.hash.mask + 1) {
        /* Same capacity: the old slots are valid as they are. */
        hx_copy_into(table, sx->u.hash.table, (int64_t)cap);
    } else {
        /* Re-placed under src's home rule: its markers carry over. */
        memset(ray_data(table), 0, (size_t)cap * (size_t)ray_elem_size(aw));
        for (int64_t g = 0; g < og; g++) {
            uint64_t slot = hx_home_m(sx->markers, mask, mix64((uint64_t)gk[g]));
            while (hx_get(table, (int64_t)slot) != 0) slot = (slot + 1) & mask;
            hx_set(table, (int64_t)slot, g + 1);
        }
    }

    /* Appended rows: probe with equality; a hit means a repeated key. */
    const uint8_t* base = (const uint8_t*)ray_data(dst);
    int64_t ng = og, nk = ok;
    for (int64_t r = n0; r < n1; r++) {
        int64_t kw = (int64_t)hash_row_key_word(dst, base, r);
        uint64_t slot = hx_home_m(sx->markers, mask, mix64((uint64_t)kw));
        for (;;) {
            int64_t gp1 = hx_get(table, (int64_t)slot);
            if (gp1 == 0) break;
            if (gk[gp1 - 1] == kw &&
                (!is_str || str_rows_eq(dst, hx_get(rows, hx_get(offs, gp1 - 1)), r))) {
                ray_release(gkeys); ray_release(offs);
                ray_release(rows);  ray_release(table);
                return;                       /* repeated key: not carried */
            }
            slot = (slot + 1) & mask;
        }
        hx_set(table, (int64_t)slot, ng + 1);
        gk[ng] = kw;
        hx_set(rows, nk, r);
        hx_set(offs, ng + 1, nk + 1);
        ng++; nk++;
    }
    gkeys->len = ng; offs->len = ng + 1; rows->len = nk; table->len = (int64_t)cap;

    ray_t* idx = ray_index_alloc(RAY_IDX_HASH, t, n1);
    if (!idx || RAY_IS_ERR(idx)) {
        ray_release(gkeys); ray_release(offs); ray_release(rows); ray_release(table);
        return;
    }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.hash.table    = table;
    ix->u.hash.gkeys    = gkeys;
    ix->u.hash.offs     = offs;
    ix->u.hash.rows     = rows;
    ix->u.hash.mask     = mask;
    ix->u.hash.n_keys   = nk;
    ix->u.hash.n_groups = ng;
    /* Every appended key is new, so `unique` stays true and a single-row
     * group is trivially ordered by any column: the markers carry over. */
    ix->u.hash.order_sym = sx->u.hash.order_sym;
    ix->markers = sx->markers;
    attach_finalize(dst, idx);
}
