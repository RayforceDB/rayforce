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

/* Fused grouped count-distinct (spec Part B).  Single pass over rows:
 * phase 1 scatters compact [pairhash][k][v][row] records into per-(worker,
 * partition) buffers; phase 2 walks each partition once with two
 * partition-local open-addressing tables — a (k,v) dedupe table and a k
 * table — producing PARTIAL per-key (distinct count, first_row); phase 3
 * merges the partials into global totals and emits (k, distinct,
 * first_row).  No intermediate pairs table, no second group pipeline.
 *
 * Partitioning is by the PAIR hash fmix64(hash(k) ^ hash(v)), NOT by the
 * key hash.  Key-hash partitioning makes each key's ENTIRE row set land in
 * one partition, so a single skewed key (e.g. a region owning half the
 * table) serializes phase 2 behind one core — measured 35% SLOWER than the
 * unfused path on a 100M-row / 9K-key / heavy-skew query.  The pair hash is
 * uniform regardless of key skew, at the price of every partition seeing
 * (nearly) every key — hence the additive merge in phase 3 and the 1024
 * partition cap that bounds the merge input.
 *
 * The dedupe table's slot index uses pair-hash bits ABOVE the partition
 * bits (the agg_engine hash-bit-overlap lesson); the k table indexes on an
 * independent hash of k alone, which takes no part in partition selection. */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "rayforce.h"
#include "core/pool.h"
#include "ops/internal.h" /* scratch_*, read_col_i64 */
#include "ops/hash.h"     /* ray_hash_i64 */
#include "ops/cdfuse.h"
#include "table/sym.h"    /* RAY_IS_SYM */

/* ══════════════════════════════════════════
 * Shared helpers
 * ══════════════════════════════════════════ */

/* Avalanche finalizer — identical body to agg_radix_fmix64 (agg_engine.c,
 * static there).  Partition selection consumes the LOW log2(n_parts) bits of
 * the pair hash and the dedupe slot index the bits ABOVE them, so the raw
 * hash must be finalized for the shift-out split to hold. */
static inline uint64_t cdf_fmix64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

/* Growable per-(worker, partition) record buffer. */
typedef struct {
    char* buf;
    uint32_t n, cap;
} cdf_buf_t;

#define CDF_REC 32 /* [pairhash 8][k 8][v 8][row 8] */

/* Upper bound on partitions.  Phase 3's merge input is
 * n_parts × distinct-keys-per-partition and pair-hash partitioning lets every
 * partition see nearly every key, so the cap is what keeps the merge bounded;
 * 1024 still gives ample parallelism (and equals RAY_POOL_INIT_TASKS, so
 * ray_pool_dispatch_n never needs to grow its ring). */
#define CDF_MAX_PARTS 1024u

/* Reserve room for one more record; returns dest ptr or NULL on OOM. */
static char* cdf_reserve(cdf_buf_t* b, uint32_t prime) {
    if (b->n == b->cap) {
        if (b->cap > UINT32_MAX / 2) return NULL;
        uint32_t nc = b->cap ? b->cap * 2 : (prime ? prime : 64);
        char* nb = (char*)ray_realloc_raw(b->buf, (size_t)nc * CDF_REC);
        if (!nb) return NULL;
        b->buf = nb;
        b->cap = nc;
    }
    return b->buf + (size_t)b->n++ * CDF_REC;
}

/* ══════════════════════════════════════════
 * Phase 1 — scatter
 * ══════════════════════════════════════════ */

typedef struct {
    const void* kdata;
    int8_t ktype;
    uint8_t kattrs;
    const void* vdata;
    int8_t vtype;
    uint8_t vattrs;
    uint32_t n_parts, nw;
    cdf_buf_t* bufs; /* [nw * n_parts] */
    uint32_t prime;  /* first-allocation capacity per buf */
    _Atomic(int) oom;
    /* Rows actually scattered.  ray_pool_dispatch clamps its task count to
     * the ring capacity but RECOMPUTES the grain, so no row is dropped by
     * clamping — however a cancelled pool (pool->cancelled) skips claimed
     * tasks outright.  Comparing this against nrows turns any such silent
     * row loss into a NULL decline instead of a short answer. */
    _Atomic(int64_t) rows_done;
} cdf_p1_ctx_t;

static void cdf_p1_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    cdf_p1_ctx_t* c = (cdf_p1_ctx_t*)vctx;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    cdf_buf_t* my = &c->bufs[(size_t)(wid % c->nw) * c->n_parts];
    for (int64_t r = start; r < end; r++) {
        int64_t k = read_col_i64(c->kdata, r, c->ktype, c->kattrs);
        int64_t v = read_col_i64(c->vdata, r, c->vtype, c->vattrs);
        /* PAIR hash: uniform even when one key owns most of the table. */
        uint64_t h = cdf_fmix64(ray_hash_i64(k) ^ ray_hash_i64(v));
        uint32_t p = (uint32_t)(h & (c->n_parts - 1));
        char* rec = cdf_reserve(&my[p], c->prime);
        if (!rec) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        ((uint64_t*)rec)[0] = h;
        ((int64_t*)rec)[1] = k;
        ((int64_t*)rec)[2] = v;
        ((int64_t*)rec)[3] = r;
    }
    atomic_fetch_add_explicit(&c->rows_done, end - start, memory_order_relaxed);
}

/* ══════════════════════════════════════════
 * Phase 2 — per-partition dedupe + count
 * ══════════════════════════════════════════ */

/* One group's PARTIAL result inside one partition: this partition's share of
 * the key's distinct-value count and the minimum row index it saw for the key.
 * Phase 3 sums the counts and takes the MIN of the firsts.  Kept as a
 * struct-of-three (not three parallel slabs pre-sized to the partition's
 * record count) so the output memory tracks the group count instead of the row
 * count — a 100M-row / 1K-group run pays 24 bytes per GROUP, not per ROW. */
typedef struct {
    int64_t key, cnt, first;
} cdf_grp_t;

typedef struct {
    cdf_grp_t* g;
    int64_t ng, cap;
} cdf_part_t;

typedef struct {
    cdf_buf_t* bufs;
    cdf_part_t* parts;
    uint32_t nw, n_parts, part_bits;
    _Atomic(int) oom;
    /* Partitions actually processed.  ray_pool_dispatch_n CLAMPS its task
     * count to the ring capacity when the growth realloc fails (see
     * RAY_POOL_INIT_TASKS in core/pool.h) and a cancelled pool skips claimed
     * tasks — either way the tail partitions never run, keep ng == 0, and
     * their groups would silently vanish from the result.  The wrapper
     * compares this against n_parts and declines (NULL) on mismatch. */
    _Atomic(int64_t) done;
} cdf_p2_ctx_t;

static void cdf_p2_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    cdf_p2_ctx_t* c = (cdf_p2_ctx_t*)vctx;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    for (int64_t p = start; p < end; p++) {
        int64_t total = 0;
        for (uint32_t w = 0; w < c->nw; w++)
            total += c->bufs[(size_t)w * c->n_parts + p].n;
        c->parts[p].ng = 0;
        if (total == 0) {
            atomic_fetch_add_explicit(&c->done, 1, memory_order_relaxed);
            continue;
        }

        /* Both partition-local tables are sized to 2× the record count:
         * groups and distinct (k,v) pairs are each bounded by the record
         * count, so load stays ≤ 0.5 and linear probing always finds an
         * empty slot. */
        uint64_t dcap = 8;
        while (dcap < (uint64_t)total * 2) dcap <<= 1;
        uint64_t dmask = dcap - 1;

        /* (k,v) dedupe table: 16B key slot + int32 occupancy (an in-band
         * sentinel is unsafe — every int64 is a valid value). */
        ray_t* dh_hdr = NULL;
        int64_t* dkv = (int64_t*)scratch_alloc(
            &dh_hdr, (size_t)dcap * 2 * sizeof(int64_t) + (size_t)dcap * 4);
        if (!dkv) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        int32_t* docc = (int32_t*)(dkv + (size_t)dcap * 2);
        memset(docc, 0xFF, (size_t)dcap * 4);

        /* k table: slots of (k, group index), both written on first insert. */
        ray_t* kh_hdr = NULL;
        int64_t* kk = (int64_t*)scratch_alloc(
            &kh_hdr, (size_t)dcap * sizeof(int64_t) + (size_t)dcap * 4);
        if (!kk) {
            scratch_free(dh_hdr);
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        int32_t* kidx = (int32_t*)(kk + dcap);
        memset(kidx, 0xFF, (size_t)dcap * 4);

        int64_t cap = total < 256 ? total : 256;
        cdf_grp_t* grps = (cdf_grp_t*)ray_alloc_raw((size_t)cap * sizeof(cdf_grp_t));
        if (!grps) {
            scratch_free(dh_hdr);
            scratch_free(kh_hdr);
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        int64_t ng = 0;
        int failed = 0;

        for (uint32_t w = 0; w < c->nw && !failed; w++) {
            cdf_buf_t* b = &c->bufs[(size_t)w * c->n_parts + p];
            const char* rec = b->buf;
            for (uint32_t i = 0; i < b->n; i++, rec += CDF_REC) {
                uint64_t ph = ((const uint64_t*)rec)[0];
                int64_t k = ((const int64_t*)rec)[1];
                int64_t v = ((const int64_t*)rec)[2];
                int64_t row = ((const int64_t*)rec)[3];

                /* k table probe for EVERY record: this partition's first_row
                 * must be the MIN over all its rows of the key, not only over
                 * rows that open a fresh (k,v) pair.  The slot index comes
                 * from a hash of k ALONE — recomputed here rather than stored,
                 * to keep the record at 32B — and that hash never selects a
                 * partition, so it needs no bit shift-out. */
                uint64_t t = cdf_fmix64(ray_hash_i64(k)) & dmask;
                int64_t gi = 0;
                for (;;) {
                    if (kidx[t] == -1) {
                        if (ng >= cap) {
                            /* Invariant: ng <= total — every group is opened
                             * by a distinct record of this partition, so the
                             * record count bounds the group count and `total`
                             * is a legitimate clamp.  The growth must never
                             * be a no-op while ng == cap (that would write one
                             * past the end), so nc is always > cap: clamp to
                             * total only while cap is still below it. */
                            int64_t nc = cap * 2;
                            if (nc > total && cap < total) nc = total;
                            cdf_grp_t* ngp = (cdf_grp_t*)ray_realloc_raw(
                                grps, (size_t)nc * sizeof(cdf_grp_t));
                            if (!ngp) { failed = 1; break; }
                            grps = ngp;
                            cap = nc;
                        }
                        if (ng > INT32_MAX) { failed = 1; break; }
                        kk[t] = k;
                        kidx[t] = (int32_t)ng;
                        grps[ng].key = k;
                        grps[ng].cnt = 0;
                        grps[ng].first = row;
                        gi = ng++;
                        break;
                    }
                    if (kk[t] == k) {
                        gi = kidx[t];
                        if (row < grps[gi].first) grps[gi].first = row;
                        break;
                    }
                    t = (t + 1) & dmask;
                }
                if (failed) break;

                /* Dedupe on the pair hash already in the record; slot bits
                 * above the partition bits. */
                uint64_t s = (ph >> c->part_bits) & dmask;
                for (;;) {
                    if (docc[s] == -1) {
                        dkv[s * 2] = k;
                        dkv[s * 2 + 1] = v;
                        docc[s] = 1;
                        grps[gi].cnt++;
                        break;
                    }
                    if (dkv[s * 2] == k && dkv[s * 2 + 1] == v) break;
                    s = (s + 1) & dmask;
                }
            }
        }

        scratch_free(dh_hdr);
        scratch_free(kh_hdr);
        if (failed) {
            ray_free_raw(grps);
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        c->parts[p].g = grps;
        c->parts[p].cap = cap;
        c->parts[p].ng = ng;
        atomic_fetch_add_explicit(&c->done, 1, memory_order_relaxed);
    }
}

/* ══════════════════════════════════════════
 * Assembly
 * ══════════════════════════════════════════ */

/* Merged group.  first_row values are unique across merged groups — one entry
 * per distinct key, each carrying that key's global minimum row — so the
 * ordering sort is total and the comparator needs no tiebreak. */
static int cdf_grp_cmp(const void* a, const void* b) {
    int64_t x = ((const cdf_grp_t*)a)->first, y = ((const cdf_grp_t*)b)->first;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Same sqrt-style sizing as agg_radix_part_count: at least one partition per
 * worker, and enough partitions that each holds ~sqrt(nrows) records — capped
 * at CDF_MAX_PARTS to bound phase 3's merge input. */
static uint32_t cdf_part_count(uint32_t nworkers, int64_t nrows) {
    uint32_t n = 1;
    uint64_t rows = nrows > 0 ? (uint64_t)nrows : 1;
    while ((n < nworkers || (uint64_t)n < rows / n + (rows % n != 0)) &&
           n < CDF_MAX_PARTS)
        n <<= 1;
    return n;
}

static int cdf_type_ok(int8_t t) {
    return t == RAY_I64 || t == RAY_I32 || t == RAY_I16 || RAY_IS_SYM(t);
}

/* Free every phase-1/2 buffer; used by both the fallback and success paths. */
static void cdf_free_all(cdf_buf_t* bufs, size_t nbuf, cdf_part_t* parts,
                         uint32_t n_parts) {
    if (bufs)
        for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
    ray_free_raw(bufs);
    if (parts)
        for (uint32_t p = 0; p < n_parts; p++) ray_free_raw(parts[p].g);
    ray_free_raw(parts);
}

ray_t* ray_cd_fused(ray_t* key_col, ray_t* val_col, int64_t nrows) {
    if (!key_col || !val_col || nrows <= 0) return NULL;
    if (!ray_is_vec(key_col) || !ray_is_vec(val_col)) return NULL;
    if (!cdf_type_ok(key_col->type) || !cdf_type_ok(val_col->type)) return NULL;
    if ((key_col->attrs & RAY_ATTR_HAS_NULLS) ||
        (val_col->attrs & RAY_ATTR_HAS_NULLS))
        return NULL;
    if (key_col->len < nrows || val_col->len < nrows) return NULL;
    if (nrows < CDF_MIN_ROWS) return NULL; /* small: existing path fine */

    ray_pool_t* pool = ray_pool_get();
    if (!pool) return NULL;
    uint32_t nw = ray_pool_total_workers(pool);

    uint32_t n_parts = cdf_part_count(nw, nrows);
    size_t nbuf = (size_t)nw * n_parts;
    cdf_buf_t* bufs = (cdf_buf_t*)ray_calloc_raw(nbuf * sizeof(cdf_buf_t));
    cdf_part_t* parts = (cdf_part_t*)ray_calloc_raw((size_t)n_parts * sizeof(cdf_part_t));
    if (!bufs || !parts) {
        cdf_free_all(bufs, nbuf, parts, n_parts);
        return NULL;
    }

    cdf_p1_ctx_t p1 = {
        .kdata = ray_data(key_col), .ktype = key_col->type, .kattrs = key_col->attrs,
        .vdata = ray_data(val_col), .vtype = val_col->type, .vattrs = val_col->attrs,
        .n_parts = n_parts, .nw = nw, .bufs = bufs,
        /* Uniform-hash expectation with 25% slack; ≥8 so tiny buffers don't
         * immediately re-double. */
        .prime = (uint32_t)((uint64_t)nrows / ((uint64_t)nw * n_parts) * 5 / 4 + 8),
        .oom = 0, .rows_done = 0,
    };
    ray_pool_dispatch(pool, cdf_p1_fn, &p1, nrows);
    if (atomic_load_explicit(&p1.oom, memory_order_relaxed) ||
        atomic_load_explicit(&p1.rows_done, memory_order_relaxed) != nrows) {
        cdf_free_all(bufs, nbuf, parts, n_parts);
        return NULL; /* fallback, not an error */
    }

    cdf_p2_ctx_t p2 = {
        .bufs = bufs, .parts = parts, .nw = nw, .n_parts = n_parts,
        .part_bits = (uint32_t)__builtin_ctz(n_parts), .oom = 0, .done = 0,
    };
    ray_pool_dispatch_n(pool, cdf_p2_fn, &p2, n_parts);
    if (atomic_load_explicit(&p2.oom, memory_order_relaxed) ||
        atomic_load_explicit(&p2.done, memory_order_relaxed) != (int64_t)n_parts) {
        cdf_free_all(bufs, nbuf, parts, n_parts);
        return NULL;
    }

    /* Phase 3: merge the per-partition partials into global per-key totals.
     * A (k,v) pair hashes to exactly ONE partition, so the partitions' distinct
     * sets for a key are disjoint and their counts simply ADD; first_row is a
     * MIN, which composes the same way.  The merge input is bounded by
     * n_parts × distinct keys and the table grows from the actual key count, so
     * this serial hash accumulate stays small even at 100M rows. */
    cdf_grp_t* merged = NULL;
    int32_t* mslot = NULL;
    int64_t mcap = 1024, mn = 0;
    uint64_t scap = 4096, smask = scap - 1;
    merged = (cdf_grp_t*)ray_alloc_raw((size_t)mcap * sizeof(cdf_grp_t));
    mslot = (int32_t*)ray_alloc_raw((size_t)scap * sizeof(int32_t));
    if (!merged || !mslot) {
        ray_free_raw(merged); ray_free_raw(mslot);
        cdf_free_all(bufs, nbuf, parts, n_parts);
        return NULL;
    }
    memset(mslot, 0xFF, (size_t)scap * sizeof(int32_t));

    for (uint32_t p = 0; p < n_parts; p++) {
        for (int64_t i = 0; i < parts[p].ng; i++) {
            const cdf_grp_t* src = &parts[p].g[i];
            uint64_t h = cdf_fmix64(ray_hash_i64(src->key));
            uint64_t s = h & smask;
            while (mslot[s] != -1 && merged[mslot[s]].key != src->key)
                s = (s + 1) & smask;
            if (mslot[s] != -1) {
                cdf_grp_t* dst = &merged[mslot[s]];
                dst->cnt += src->cnt;
                if (src->first < dst->first) dst->first = src->first;
                continue;
            }
            if (mn == mcap || mn > INT32_MAX) {
                if (mn > INT32_MAX) { /* int32 slot payload exhausted */
                    ray_free_raw(merged); ray_free_raw(mslot);
                    cdf_free_all(bufs, nbuf, parts, n_parts);
                    return NULL;
                }
                cdf_grp_t* nm = (cdf_grp_t*)ray_realloc_raw(
                    merged, (size_t)(mcap * 2) * sizeof(cdf_grp_t));
                if (!nm) {
                    ray_free_raw(merged); ray_free_raw(mslot);
                    cdf_free_all(bufs, nbuf, parts, n_parts);
                    return NULL;
                }
                merged = nm;
                mcap *= 2;
            }
            merged[mn] = *src;
            mslot[s] = (int32_t)mn;
            mn++;
            /* Keep the slot table under 50% load; rehash from merged[]. */
            if ((uint64_t)mn * 2 >= scap) {
                uint64_t ns = scap * 2;
                int32_t* nsl = (int32_t*)ray_realloc_raw(mslot,
                                                         (size_t)ns * sizeof(int32_t));
                if (!nsl) {
                    ray_free_raw(merged); ray_free_raw(mslot);
                    cdf_free_all(bufs, nbuf, parts, n_parts);
                    return NULL;
                }
                mslot = nsl;
                scap = ns;
                smask = scap - 1;
                memset(mslot, 0xFF, (size_t)scap * sizeof(int32_t));
                for (int64_t m = 0; m < mn; m++) {
                    uint64_t hh = cdf_fmix64(ray_hash_i64(merged[m].key)) & smask;
                    while (mslot[hh] != -1) hh = (hh + 1) & smask;
                    mslot[hh] = (int32_t)m;
                }
            }
        }
    }
    ray_free_raw(mslot);
    cdf_free_all(bufs, nbuf, parts, n_parts);

    int64_t ng = mn;
    /* Global stable first-seen order. */
    qsort(merged, (size_t)ng, sizeof(cdf_grp_t), cdf_grp_cmp);

    ray_t* keys = ray_vec_new(RAY_I64, ng);
    ray_t* cnts = ray_vec_new(RAY_I64, ng);
    ray_t* firsts = ray_vec_new(RAY_I64, ng);
    ray_t* tbl = ray_table_new(3);
    if (!keys || RAY_IS_ERR(keys) || !cnts || RAY_IS_ERR(cnts) ||
        !firsts || RAY_IS_ERR(firsts) || !tbl || RAY_IS_ERR(tbl)) {
        ray_release(keys); ray_release(cnts); ray_release(firsts); ray_release(tbl);
        ray_free_raw(merged);
        return NULL;
    }
    keys->len = cnts->len = firsts->len = ng;
    int64_t* kd = (int64_t*)ray_data(keys);
    int64_t* cd = (int64_t*)ray_data(cnts);
    int64_t* fd = (int64_t*)ray_data(firsts);
    for (int64_t i = 0; i < ng; i++) {
        kd[i] = merged[i].key;
        cd[i] = merged[i].cnt;
        fd[i] = merged[i].first;
    }
    ray_free_raw(merged);

    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), keys);
    tbl = ray_table_add_col(tbl, ray_sym_intern("u", 1), cnts);
    tbl = ray_table_add_col(tbl, ray_sym_intern("_first", 6), firsts);
    ray_release(keys);
    ray_release(cnts);
    ray_release(firsts);
    if (!tbl || RAY_IS_ERR(tbl)) {
        ray_release(tbl);
        return NULL;
    }
    return tbl;
}
