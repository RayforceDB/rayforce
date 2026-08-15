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
 * phase 1 scatters compact [khash][k][v][row] records into per-(worker,
 * partition) buffers, partitioned by fmix64(hash(k)) LOW bits so every
 * row of one key lands in exactly one partition; phase 2 walks each
 * partition once with two partition-local open-addressing tables — a
 * (k,v) dedupe table and a k count table — and emits (k, distinct,
 * first_row) directly.  No intermediate pairs table, no second group
 * pipeline.  Slot indices for both local tables use hash bits ABOVE the
 * partition bits (the agg_engine hash-bit-overlap lesson). */

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
 * static there).  Partition selection consumes the LOW log2(n_parts) bits
 * and both partition-local slot indices the bits ABOVE them, so the raw
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

#define CDF_REC 32 /* [khash 8][k 8][v 8][row 8] */

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
} cdf_p1_ctx_t;

static void cdf_p1_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    cdf_p1_ctx_t* c = (cdf_p1_ctx_t*)vctx;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    cdf_buf_t* my = &c->bufs[(size_t)(wid % c->nw) * c->n_parts];
    for (int64_t r = start; r < end; r++) {
        int64_t k = read_col_i64(c->kdata, r, c->ktype, c->kattrs);
        int64_t v = read_col_i64(c->vdata, r, c->vtype, c->vattrs);
        uint64_t h = cdf_fmix64(ray_hash_i64(k));
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
}

/* ══════════════════════════════════════════
 * Phase 2 — per-partition dedupe + count
 * ══════════════════════════════════════════ */

/* One emitted group.  Kept as a struct-of-three (not three parallel slabs
 * pre-sized to the partition's record count) so the output memory tracks
 * the group count instead of the row count — a 100M-row / 1K-group run
 * then pays 24 bytes per GROUP, not 24 bytes per ROW. */
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
        if (total == 0) continue;

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
                uint64_t kh = ((const uint64_t*)rec)[0];
                int64_t k = ((const int64_t*)rec)[1];
                int64_t v = ((const int64_t*)rec)[2];
                int64_t row = ((const int64_t*)rec)[3];

                /* k table probe for EVERY record: first_row must be the MIN
                 * over all rows of the key, not only over rows that open a
                 * fresh (k,v) pair. */
                uint64_t t = (kh >> c->part_bits) & dmask;
                int64_t gi = 0;
                for (;;) {
                    if (kidx[t] == -1) {
                        if (ng == cap) {
                            int64_t nc = cap * 2 > total ? total : cap * 2;
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

                /* Combined (k,v) hash for the dedupe table; slot bits above
                 * the partition bits. */
                uint64_t vh = cdf_fmix64(kh ^ ray_hash_i64(v));
                uint64_t s = (vh >> c->part_bits) & dmask;
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
    }
}

/* ══════════════════════════════════════════
 * Assembly
 * ══════════════════════════════════════════ */

/* (first_row, partition, group index) triple used to restore stable
 * first-seen order across partitions.  first_row values are unique — each
 * key lives in exactly one partition and contributes one group — so the
 * sort is total and the comparator needs no tiebreak. */
typedef struct {
    int64_t first;
    int32_t part, idx;
} cdf_ord_t;

static int cdf_ord_cmp(const void* a, const void* b) {
    int64_t x = ((const cdf_ord_t*)a)->first, y = ((const cdf_ord_t*)b)->first;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Same sqrt-style sizing as agg_radix_part_count: at least one partition per
 * worker, and enough partitions that each holds ~sqrt(nrows) records. */
static uint32_t cdf_part_count(uint32_t nworkers, int64_t nrows) {
    uint32_t n = 1;
    uint64_t rows = nrows > 0 ? (uint64_t)nrows : 1;
    while ((n < nworkers || (uint64_t)n < rows / n + (rows % n != 0)) &&
           n < (1u << 14))
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
        .oom = 0,
    };
    ray_pool_dispatch(pool, cdf_p1_fn, &p1, nrows);
    if (atomic_load_explicit(&p1.oom, memory_order_relaxed)) {
        cdf_free_all(bufs, nbuf, parts, n_parts);
        return NULL; /* fallback, not an error */
    }

    cdf_p2_ctx_t p2 = {
        .bufs = bufs, .parts = parts, .nw = nw, .n_parts = n_parts,
        .part_bits = (uint32_t)__builtin_ctz(n_parts), .oom = 0,
    };
    ray_pool_dispatch_n(pool, cdf_p2_fn, &p2, n_parts);
    if (atomic_load_explicit(&p2.oom, memory_order_relaxed)) {
        cdf_free_all(bufs, nbuf, parts, n_parts);
        return NULL;
    }

    int64_t ng = 0;
    for (uint32_t p = 0; p < n_parts; p++) ng += parts[p].ng;

    cdf_ord_t* ord = (cdf_ord_t*)ray_alloc_raw((size_t)(ng > 0 ? ng : 1) * sizeof(cdf_ord_t));
    if (!ord) {
        cdf_free_all(bufs, nbuf, parts, n_parts);
        return NULL;
    }
    int64_t o = 0;
    for (uint32_t p = 0; p < n_parts; p++)
        for (int64_t i = 0; i < parts[p].ng; i++) {
            ord[o].first = parts[p].g[i].first;
            ord[o].part = (int32_t)p;
            ord[o].idx = (int32_t)i;
            o++;
        }
    qsort(ord, (size_t)ng, sizeof(cdf_ord_t), cdf_ord_cmp);

    ray_t* keys = ray_vec_new(RAY_I64, ng);
    ray_t* cnts = ray_vec_new(RAY_I64, ng);
    ray_t* firsts = ray_vec_new(RAY_I64, ng);
    ray_t* tbl = ray_table_new(3);
    if (!keys || RAY_IS_ERR(keys) || !cnts || RAY_IS_ERR(cnts) ||
        !firsts || RAY_IS_ERR(firsts) || !tbl || RAY_IS_ERR(tbl)) {
        ray_release(keys); ray_release(cnts); ray_release(firsts); ray_release(tbl);
        ray_free_raw(ord);
        cdf_free_all(bufs, nbuf, parts, n_parts);
        return NULL;
    }
    keys->len = cnts->len = firsts->len = ng;
    int64_t* kd = (int64_t*)ray_data(keys);
    int64_t* cd = (int64_t*)ray_data(cnts);
    int64_t* fd = (int64_t*)ray_data(firsts);
    for (int64_t i = 0; i < ng; i++) {
        const cdf_grp_t* g = &parts[ord[i].part].g[ord[i].idx];
        kd[i] = g->key;
        cd[i] = g->cnt;
        fd[i] = g->first;
    }
    ray_free_raw(ord);
    cdf_free_all(bufs, nbuf, parts, n_parts);

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
