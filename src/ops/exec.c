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

#include "ops/internal.h"
#include "lang/internal.h"  /* sym_domain_rep (sym-domain Phase 2) */
#include "lang/format.h"    /* ray_type_name (error context) */
#include "ops/rowsel.h"
#include "ops/fused_group.h"
#include "ops/idxop.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "core/qstats.h"   /* per-worker parallelism stats for profile spans */
#include "core/runtime.h"  /* __VM — filter-compaction projection keep-set */

/* Global profiler instance (zero-initialized = inactive) */
ray_profile_t g_ray_profile;
ray_profile_t g_ray_profile_last;

/* --------------------------------------------------------------------------
 * Materialize a MAPCOMMON column into a flat RAY_SYM vector.
 * Expands key_values × row_counts into one SYM ID per row.
 * -------------------------------------------------------------------------- */
ray_t* materialize_mapcommon(ray_t* mc) {
    ray_t** mc_ptrs = (ray_t**)ray_data(mc);
    ray_t* kv = mc_ptrs[0];   /* key_values: typed vec (DATE/I64/SYM) */
    ray_t* rc = mc_ptrs[1];   /* row_counts: RAY_I64 vec of n_parts */
    int64_t n_parts = kv->len;
    int8_t kv_type = kv->type;
    size_t esz = (size_t)ray_sym_elem_size(kv_type, kv->attrs);
    const char* kdata = (const char*)ray_data(kv);
    const int64_t* counts = (const int64_t*)ray_data(rc);

    int64_t total = 0;
    for (int64_t p = 0; p < n_parts; p++) total += counts[p];

    ray_t* flat = ray_vec_new(kv_type, total);
    if (!flat || RAY_IS_ERR(flat)) return ray_error("oom", NULL);
    flat->len = total;
    /* SYM keys are raw cell ids of the MAPCOMMON keys carrier — the
     * flat output resolves over ITS dictionary (sym-domain Phase 2;
     * no-op while every domain is the runtime singleton). */
    if (kv_type == RAY_SYM) ray_sym_vec_adopt_domain(flat, kv);

    /* Pattern-fill: broadcast each partition's key value across its row range.
     * Typed fill avoids per-element memcpy overhead. */
    char* out = (char*)ray_data(flat);
    int64_t off = 0;
    for (int64_t p = 0; p < n_parts; p++) {
        int64_t cnt = counts[p];
        if (esz == 8) {
            uint64_t v;
            memcpy(&v, kdata + (size_t)p * 8, 8);
            uint64_t* dst = (uint64_t*)(out + off * 8);
            for (int64_t r = 0; r < cnt; r++) dst[r] = v;
        } else if (esz == 4) {
            uint32_t v;
            memcpy(&v, kdata + (size_t)p * 4, 4);
            uint32_t* dst = (uint32_t*)(out + off * 4);
            for (int64_t r = 0; r < cnt; r++) dst[r] = v;
        } else {
            for (int64_t r = 0; r < cnt; r++)
                memcpy(out + (off + r) * esz, kdata + (size_t)p * esz, esz);
        }
        off += cnt;
    }
    return flat;
}

/* Materialize first N rows of a MAPCOMMON column into a flat typed vector. */
ray_t* materialize_mapcommon_head(ray_t* mc, int64_t n) {
    ray_t** mc_ptrs = (ray_t**)ray_data(mc);
    ray_t* kv = mc_ptrs[0];
    ray_t* rc = mc_ptrs[1];
    int64_t n_parts = kv->len;
    int8_t kv_type = kv->type;
    size_t esz = (size_t)ray_sym_elem_size(kv_type, kv->attrs);
    const char* kdata = (const char*)ray_data(kv);
    const int64_t* counts = (const int64_t*)ray_data(rc);

    ray_t* flat = ray_vec_new(kv_type, n);
    if (!flat || RAY_IS_ERR(flat)) return ray_error("oom", NULL);
    flat->len = n;
    /* raw SYM cell ids — adopt the keys carrier's domain (see above) */
    if (kv_type == RAY_SYM) ray_sym_vec_adopt_domain(flat, kv);

    char* out = (char*)ray_data(flat);
    int64_t off = 0;
    for (int64_t p = 0; p < n_parts && off < n; p++) {
        int64_t take = counts[p];
        if (take > n - off) take = n - off;
        if (esz == 8) {
            uint64_t v;
            memcpy(&v, kdata + (size_t)p * 8, 8);
            uint64_t* dst = (uint64_t*)(out + off * 8);
            for (int64_t r = 0; r < take; r++) dst[r] = v;
        } else if (esz == 4) {
            uint32_t v;
            memcpy(&v, kdata + (size_t)p * 4, 4);
            uint32_t* dst = (uint32_t*)(out + off * 4);
            for (int64_t r = 0; r < take; r++) dst[r] = v;
        } else {
            for (int64_t r = 0; r < take; r++)
                memcpy(out + (off + r) * esz, kdata + (size_t)p * esz, esz);
        }
        off += take;
    }
    return flat;
}

/* Materialize MAPCOMMON through a boolean filter predicate. */
ray_t* materialize_mapcommon_filter(ray_t* mc, ray_t* pred, int64_t pass_count) {
    ray_t** mc_ptrs = (ray_t**)ray_data(mc);
    ray_t* kv = mc_ptrs[0];
    ray_t* rc = mc_ptrs[1];
    int64_t n_parts = kv->len;
    int8_t kv_type = kv->type;
    size_t esz = (size_t)ray_sym_elem_size(kv_type, kv->attrs);
    const char* kdata = (const char*)ray_data(kv);
    const int64_t* counts = (const int64_t*)ray_data(rc);

    ray_t* flat = ray_vec_new(kv_type, pass_count);
    if (!flat || RAY_IS_ERR(flat)) return ray_error("oom", NULL);
    flat->len = pass_count;
    /* raw SYM cell ids — adopt the keys carrier's domain (see above) */
    if (kv_type == RAY_SYM) ray_sym_vec_adopt_domain(flat, kv);

    char* out = (char*)ray_data(flat);
    int64_t out_idx = 0;
    int64_t row = 0;
    int64_t part_idx = 0;
    int64_t part_end = counts[0];

    ray_morsel_t mp;
    ray_morsel_init(&mp, pred);
    while (ray_morsel_next(&mp)) {
        const uint8_t* bits = (const uint8_t*)mp.morsel_ptr;
        for (int64_t i = 0; i < mp.morsel_len; i++, row++) {
            while (part_idx < n_parts - 1 && row >= part_end) {
                part_idx++;
                part_end += counts[part_idx];
            }
            if (bits[i])
                memcpy(out + (size_t)out_idx++ * esz,
                       kdata + (size_t)part_idx * esz, esz);
        }
    }
    return flat;
}


/* ============================================================================
 * Parallel index gather — used by filter, sort, and join
 * ============================================================================ */

void multi_gather_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    multi_gather_ctx_t* c = (multi_gather_ctx_t*)raw;
    const int64_t* restrict idx = c->idx;
    int64_t nc = c->ncols;

    /* Process one column at a time per batch of rows.
     * This focuses random reads on a single source array, giving the
     * hardware prefetcher only 1 stream to track (instead of ncols
     * concurrent streams, which overflows the L2 miss queue). */
#define MG_BATCH 512
#define MG_PF    32
    for (int64_t base = start; base < end; base += MG_BATCH) {
        int64_t bstart = base;
        int64_t bend = base + MG_BATCH;
        if (bend > end) bend = end;
        for (int64_t col = 0; col < nc; col++) {
            uint8_t e = c->esz[col];
            char* src = c->srcs[col];
            char* dst = c->dsts[col];
            if (e == 8) {
                const uint64_t* restrict s8 = (const uint64_t*)src;
                uint64_t* restrict d8 = (uint64_t*)dst;
                for (int64_t i = bstart; i < bend; i++) {
                    if (i + MG_PF < bend)
                        __builtin_prefetch(&s8[idx[i + MG_PF]], 0, 0);
                    d8[i] = s8[idx[i]];
                }
            } else if (e == 4) {
                const uint32_t* restrict s4 = (const uint32_t*)src;
                uint32_t* restrict d4 = (uint32_t*)dst;
                for (int64_t i = bstart; i < bend; i++) {
                    if (i + MG_PF < bend)
                        __builtin_prefetch(&s4[idx[i + MG_PF]], 0, 0);
                    d4[i] = s4[idx[i]];
                }
            } else {
                for (int64_t i = bstart; i < bend; i++) {
                    if (i + MG_PF < bend)
                        __builtin_prefetch(src + idx[i + MG_PF] * e, 0, 0);
                    memcpy(dst + i * e, src + idx[i] * e, e);
                }
            }
        }
    }
#undef MG_PF
#undef MG_BATCH
}

/* Parallel index gather — single column with prefetching */
void gather_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    gather_ctx_t* c = (gather_ctx_t*)raw;
    char* restrict src = (char*)ray_data(c->src_col);
    char* restrict dst = (char*)ray_data(c->dst_col);
    uint8_t esz = c->esz;
    const int64_t* restrict idx = c->idx;
#define GATHER_PF 16

    if (c->nullable) {
        for (int64_t i = start; i < end; i++) {
            if (i + GATHER_PF < end) {
                int64_t pf = idx[i + GATHER_PF];
                if (pf >= 0) __builtin_prefetch(src + pf * esz, 0, 0);
            }
            int64_t r = idx[i];
            if (r >= 0)
                memcpy(dst + i * esz, src + r * esz, esz);
            else
                memset(dst + i * esz, 0, esz);
        }
    } else {
        for (int64_t i = start; i < end; i++) {
            if (i + GATHER_PF < end)
                __builtin_prefetch(src + idx[i + GATHER_PF] * esz, 0, 0);
            memcpy(dst + i * esz, src + idx[i] * esz, esz);
        }
    }
#undef GATHER_PF
}

/* ============================================================================
 * Partitioned gather — cache-conscious column rearrangement
 *
 * Standard gather:  dst[i] = src[idx[i]] — sequential writes, random reads.
 * With 10M rows the source data (~hundreds of MB) far exceeds L2 cache, so
 * every read is a main-memory miss (~60ns even with prefetching).
 *
 * Partitioned gather groups work by source ranges: for each 16K-row source
 * block, process all indices that point into it.  The block fits in L2, so
 * reads become L2 hits (~5ns).  Output writes become random but the CPU's
 * store buffer absorbs them without stalling (~20ns effective).
 *
 * Three phases:
 *   1. Histogram  — count indices per source block           (parallel)
 *   2. Route      — scatter (dest, src) pairs into buckets   (parallel)
 *   3. Block-gather — per block, source in L2 → fast reads   (parallel)
 * ============================================================================ */

/* Block = 16K source rows.  16K × 16 cols × 8B = 2MB ≈ L2 cache per core. */
#define PG_BSHIFT 14
#define PG_BSIZE  (1 << PG_BSHIFT)   /* 16384 */
#define PG_MIN    (PG_BSIZE * 8)     /* 131072 — below this, routing overhead > benefit */

/* Pass 1+2 use dispatch_n with explicit task-to-range mapping so that
 * histogram and scatter have consistent per-task assignments regardless
 * of which worker picks up each task (work-stealing is non-deterministic). */

typedef struct {
    const int64_t* idx;
    int64_t*       hist;      /* n_tasks × n_parts, row-major */
    int64_t        n_parts;
    int64_t        n;         /* total rows */
    uint32_t       n_tasks;
} pg_hist_ctx_t;

static void pg_hist_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    pg_hist_ctx_t* c = (pg_hist_ctx_t*)arg;
    int64_t task = start;

    int64_t chunk = (c->n + c->n_tasks - 1) / c->n_tasks;
    int64_t lo = task * chunk;
    int64_t hi = lo + chunk;
    if (hi > c->n) hi = c->n;
    if (lo >= hi) { memset(c->hist + task * c->n_parts, 0,
                           (size_t)c->n_parts * sizeof(int64_t)); return; }

    int64_t* h = c->hist + task * c->n_parts;
    memset(h, 0, (size_t)c->n_parts * sizeof(int64_t));
    const int64_t* idx = c->idx;
    for (int64_t i = lo; i < hi; i++)
        h[idx[i] >> PG_BSHIFT]++;
}

typedef struct {
    const int64_t* idx;
    int32_t*       rdest;
    int32_t*       rsrc;
    int64_t*       offsets;   /* n_tasks × n_parts write cursors */
    int64_t        n_parts;
    int64_t        n;
    uint32_t       n_tasks;
} pg_route_ctx_t;

static void pg_route_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    pg_route_ctx_t* c = (pg_route_ctx_t*)arg;
    int64_t task = start;

    int64_t chunk = (c->n + c->n_tasks - 1) / c->n_tasks;
    int64_t lo = task * chunk;
    int64_t hi = lo + chunk;
    if (hi > c->n) hi = c->n;
    if (lo >= hi) return;

    int64_t* off = c->offsets + task * c->n_parts;
    const int64_t* idx = c->idx;
    int32_t* rd = c->rdest;
    int32_t* rs = c->rsrc;
    for (int64_t i = lo; i < hi; i++) {
        int64_t src = idx[i];
        int64_t pos = off[src >> PG_BSHIFT]++;
        rd[pos] = (int32_t)i;
        rs[pos] = (int32_t)src;
    }
}

/* Pass 3: per-block gather — one task per source block */
typedef struct {
    const int32_t* rdest;
    const int32_t* rsrc;
    const int64_t* part_off;  /* partition start offsets (n_parts + 1) */
    char**         srcs;
    char**         dsts;
    const uint8_t* esz;
    int64_t        ncols;
} pg_block_ctx_t;

static void pg_block_fn(void* arg, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    pg_block_ctx_t* c = (pg_block_ctx_t*)arg;
    int64_t blk = start;  /* dispatch_n: one task per call */

    int64_t lo = c->part_off[blk];
    int64_t hi = c->part_off[blk + 1];
    if (lo >= hi) return;

    const int32_t* rd = c->rdest + lo;
    const int32_t* rs = c->rsrc  + lo;
    int64_t cnt = hi - lo;

    /* Column-at-a-time: keeps the source block hot in L2.
     * After the first few reads, the entire 16K-row source slice
     * is cache-resident, so subsequent reads are L2 hits. */
    for (int64_t col = 0; col < c->ncols; col++) {
        uint8_t e = c->esz[col];
        const char* src = c->srcs[col];
        char* dst = c->dsts[col];
        if (e == 8) {
            const uint64_t* s8 = (const uint64_t*)src;
            uint64_t* d8 = (uint64_t*)dst;
            for (int64_t j = 0; j < cnt; j++)
                d8[rd[j]] = s8[rs[j]];
        } else if (e == 4) {
            const uint32_t* s4 = (const uint32_t*)src;
            uint32_t* d4 = (uint32_t*)dst;
            for (int64_t j = 0; j < cnt; j++)
                d4[rd[j]] = s4[rs[j]];
        } else if (e == 2) {
            const uint16_t* s2 = (const uint16_t*)src;
            uint16_t* d2 = (uint16_t*)dst;
            for (int64_t j = 0; j < cnt; j++)
                d2[rd[j]] = s2[rs[j]];
        } else if (e == 1) {
            for (int64_t j = 0; j < cnt; j++)
                dst[rd[j]] = src[rs[j]];
        } else {
            for (int64_t j = 0; j < cnt; j++)
                memcpy(dst + (int64_t)rd[j] * e,
                       src + (int64_t)rs[j] * e, e);
        }
    }
}

/* Public entry point: partitioned gather for n > PG_MIN, fallback otherwise.
 * n:        number of index entries (output rows)
 * src_rows: number of rows in the source columns (indices may reference [0, src_rows)) */
void partitioned_gather(ray_pool_t* pool, const int64_t* idx, int64_t n,
                        int64_t src_rows, char** srcs, char** dsts,
                        const uint8_t* esz, int64_t ncols) {
    /* Fallback for small arrays or no pool.  Chunked by MGATHER_MAX_COLS so
     * every column is gathered regardless of ncols (current callers batch to
     * <=MGATHER_MAX_COLS themselves, but a wider call must not silently
     * leave columns ungathered). */
    if (!pool || n < PG_MIN || n > INT32_MAX || src_rows > INT32_MAX) {
        for (int64_t base = 0; base < ncols; base += MGATHER_MAX_COLS) {
            multi_gather_ctx_t mg = { .idx = idx, .ncols = 0 };
            for (int64_t c = base; c < ncols && mg.ncols < MGATHER_MAX_COLS; c++) {
                mg.srcs[mg.ncols] = srcs[c]; mg.dsts[mg.ncols] = dsts[c];
                mg.esz[mg.ncols] = esz[c];
                mg.ncols++;
            }
            if (pool) ray_pool_dispatch(pool, multi_gather_fn, &mg, n);
            else      multi_gather_fn(&mg, 0, 0, n);
        }
        return;
    }

    /* Partition by SOURCE range — indices can reference any row in [0, src_rows) */
    int64_t n_parts = (src_rows + PG_BSIZE - 1) >> PG_BSHIFT;
    uint32_t nw = ray_pool_total_workers(pool);

    /* Allocate routing buffers */
    ray_t *hist_hdr = NULL, *off_hdr = NULL;
    ray_t *rdest_hdr = NULL, *rsrc_hdr = NULL, *poff_hdr = NULL;

    int64_t* hist    = (int64_t*)scratch_alloc(&hist_hdr,
                           (size_t)nw * (size_t)n_parts * sizeof(int64_t));
    int64_t* offsets = (int64_t*)scratch_alloc(&off_hdr,
                           (size_t)nw * (size_t)n_parts * sizeof(int64_t));
    int32_t* rdest   = (int32_t*)scratch_alloc(&rdest_hdr,
                           (size_t)n * sizeof(int32_t));
    int32_t* rsrc    = (int32_t*)scratch_alloc(&rsrc_hdr,
                           (size_t)n * sizeof(int32_t));
    int64_t* part_off = (int64_t*)scratch_alloc(&poff_hdr,
                            (size_t)(n_parts + 1) * sizeof(int64_t));

    if (!hist || !offsets || !rdest || !rsrc || !part_off) {
        scratch_free(hist_hdr); scratch_free(off_hdr);
        scratch_free(rdest_hdr); scratch_free(rsrc_hdr);
        scratch_free(poff_hdr);
        /* Fallback to regular gather on allocation failure (chunked, same
         * no-column-left-behind contract as the small-input fallback above) */
        for (int64_t base = 0; base < ncols; base += MGATHER_MAX_COLS) {
            multi_gather_ctx_t mg = { .idx = idx, .ncols = 0 };
            for (int64_t c = base; c < ncols && mg.ncols < MGATHER_MAX_COLS; c++) {
                mg.srcs[mg.ncols] = srcs[c]; mg.dsts[mg.ncols] = dsts[c];
                mg.esz[mg.ncols] = esz[c];
                mg.ncols++;
            }
            ray_pool_dispatch(pool, multi_gather_fn, &mg, n);
        }
        return;
    }

    /* Pass 1: parallel histogram (dispatch_n for deterministic task→range) */
    pg_hist_ctx_t hctx = {
        .idx = idx, .hist = hist, .n_parts = n_parts,
        .n = n, .n_tasks = nw,
    };
    ray_pool_dispatch_n(pool, pg_hist_fn, &hctx, nw);

    /* Pass 2: prefix sum → per-task scatter offsets + partition boundaries */
    int64_t running = 0;
    for (int64_t p = 0; p < n_parts; p++) {
        part_off[p] = running;
        for (uint32_t t = 0; t < nw; t++) {
            offsets[t * n_parts + p] = running;
            running += hist[t * n_parts + p];
        }
    }
    part_off[n_parts] = running;

    /* Pass 3: parallel route (same task→range mapping as histogram) */
    pg_route_ctx_t rctx = {
        .idx = idx, .rdest = rdest, .rsrc = rsrc,
        .offsets = offsets, .n_parts = n_parts,
        .n = n, .n_tasks = nw,
    };
    ray_pool_dispatch_n(pool, pg_route_fn, &rctx, nw);

    /* Pass 4: parallel per-block gather */
    pg_block_ctx_t bctx = {
        .rdest = rdest, .rsrc = rsrc, .part_off = part_off,
        .srcs = srcs, .dsts = dsts, .esz = esz, .ncols = ncols,
    };
    ray_pool_dispatch_n(pool, pg_block_fn, &bctx, (uint32_t)n_parts);

    scratch_free(hist_hdr);
    scratch_free(off_hdr);
    scratch_free(rdest_hdr);
    scratch_free(rsrc_hdr);
    scratch_free(poff_hdr);
}

/* (filter execution moved to filter.c) */


/* ============================================================================
 * Sort execution (simple insertion sort)
 * ============================================================================ */

/* Forward declarations — exec_node wraps exec_node_inner with profiling */
/* exec_node declared extern in exec_internal.h */
static ray_t* exec_node_inner(ray_graph_t* g, ray_op_t* op);



/* Broadcast a scalar atom to a column vector of nrows elements.
 * Returns a new vector (caller owns).  On failure returns ray_error(). */
ray_t* broadcast_scalar(ray_t* atom, int64_t nrows) {
    if (!atom) return ray_error("domain", NULL);
    if (nrows <= 0) {
        /* Empty table: return an empty vector of the matching type */
        int8_t at = atom->type;
        int8_t vt;
        if      (at == -RAY_STR)  vt = RAY_STR;
        else if (at == -RAY_I64)  vt = RAY_I64;
        else if (at == -RAY_F64)  vt = RAY_F64;
        else if (at == -RAY_BOOL) vt = RAY_BOOL;
        else if (at == -RAY_SYM)  vt = RAY_SYM;
        else return ray_error("type", "broadcast: expected str/i64/f64/bool/sym atom, got %s", ray_type_name(at));
        return ray_vec_new(vt, 0);
    }
    int8_t at = atom->type;

    /* -RAY_STR → RAY_STR column */
    if (at == -RAY_STR) {
        const char* sp = ray_str_ptr(atom);
        size_t sl = ray_str_len(atom);
        ray_t* vec = ray_vec_new(RAY_STR, nrows);
        if (!vec || RAY_IS_ERR(vec)) return vec;
        for (int64_t r = 0; r < nrows; r++) {
            vec = ray_str_vec_append(vec, sp, sl);
            if (RAY_IS_ERR(vec)) return vec;
        }
        return vec;
    }

    /* Numeric / bool / sym scalars */
    int8_t vt;
    if      (at == -RAY_I64)  vt = RAY_I64;
    else if (at == -RAY_F64)  vt = RAY_F64;
    else if (at == -RAY_BOOL) vt = RAY_BOOL;
    else if (at == -RAY_SYM)  vt = RAY_SYM;
    else return ray_error("type", "broadcast: expected str/i64/f64/bool/sym atom, got %s", ray_type_name(at));

    size_t esz = (vt == RAY_BOOL) ? 1 : 8;
    ray_t* vec = ray_vec_new(vt, nrows);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    uint8_t elem[8] = {0};
    memcpy(elem, &atom->i64, esz);
    for (int64_t r = 0; r < nrows; r++) {
        vec = ray_vec_append(vec, elem);
        if (RAY_IS_ERR(vec)) return vec;
    }
    /* Propagate null state from the source atom.  A typed-null atom
     * (e.g. 0Nf returned by VAR/STDDEV with cnt<=1) carries its null
     * bit in atom->aux[0] & 1.  Without this propagation, the
     * scalar-select pipeline
     *   exec_reduction → ray_typed_null(-RAY_F64) → broadcast_scalar
     * would silently surface a stray 0.0 at (at result 0) instead of
     * the 0Nf the by-keyed path correctly reports.  Stamp every
     * output row null so downstream collection_elem / null-aware ops
     * preserve atom semantics. */
    if (RAY_ATOM_IS_NULL(atom)) {
        for (int64_t r = 0; r < nrows; r++)
            ray_vec_set_null(vec, r, true);
    }
    return vec;
}

/* OP_IN worker — process [start, end) of the BOOL output buffer.
 * Disjoint slices, no synchronization. */
typedef struct {
    ray_t*         col;
    const double*  svf;
    const int64_t* svi;
    int64_t        sv_len;
    uint8_t*       ob;       /* output base; element i writes ob[i - ob_base] */
    int64_t        ob_base;  /* 0 for full-vec output; morsel start for fused-sel */
    int8_t         ct;
    /* SYM columns: per-domain-position membership verdict (1 = in set),
     * built once over the VOCABULARY in in_build_worker_ctx.  Turns the
     * per-row linear set scan into one byte load — any set size, width-
     * specialized, vectorizable.  NULL when not applicable. */
    const uint8_t* symlut;
    bool           col_has_nulls;
    bool           col_atom_null;
    bool           col_is_atom;
    bool           use_double;
    bool           negate;
} in_worker_ctx_t;

static void exec_in_worker(void* vctx, uint32_t worker_id,
                           int64_t start, int64_t end) {
    (void)worker_id;
    in_worker_ctx_t* c = (in_worker_ctx_t*)vctx;
    ray_t* col = c->col;
    const void* cd = c->col_is_atom ? NULL : ray_data(col);
    int8_t ct = c->ct;
    uint8_t cattrs = c->col_is_atom ? 0 : col->attrs;
    /* Output index is relative to ob_base so a fused-selection caller can
     * point ob at a morsel-local scratch (ob_base = morsel start) while the
     * column is still read at the absolute row index i: element i writes
     * ob[i - ob_base], which lands at scratch[0..n).  Full-vec callers set
     * ob_base = 0, leaving the plain ob[i]. */
    uint8_t* ob = c->ob;
    int64_t ob_base = c->ob_base;
    int64_t sv_len = c->sv_len;
    int negate = c->negate ? 1 : 0;

    #define IN_READ_I64(dst, idx) do {                                      \
        switch (ct) {                                                       \
        case RAY_BOOL: case RAY_U8: (dst) = ((const uint8_t*)cd)[idx]; break; \
        case RAY_I16:  (dst) = ((const int16_t*)cd)[idx]; break;            \
        case RAY_I32:  case RAY_DATE: case RAY_TIME:                        \
                       (dst) = ((const int32_t*)cd)[idx]; break;            \
        case RAY_I64:  case RAY_TIMESTAMP:                                  \
                       (dst) = ((const int64_t*)cd)[idx]; break;            \
        case RAY_SYM:  (dst) = ray_read_sym(cd, (idx), ct, cattrs); break;  \
        default:       (dst) = 0; break;                                    \
        }                                                                   \
    } while (0)

    #define IN_READ_F64(dst, idx) do {                                      \
        switch (ct) {                                                       \
        case RAY_BOOL: case RAY_U8: (dst) = (double)((const uint8_t*)cd)[idx]; break; \
        case RAY_I16:  (dst) = (double)((const int16_t*)cd)[idx]; break;    \
        case RAY_I32:  case RAY_DATE: case RAY_TIME:                        \
                       (dst) = (double)((const int32_t*)cd)[idx]; break;    \
        case RAY_I64:  case RAY_TIMESTAMP:                                  \
                       (dst) = (double)((const int64_t*)cd)[idx]; break;    \
        case RAY_F32:  (dst) = (double)((const float*)cd)[idx]; break;      \
        case RAY_F64:  (dst) = ((const double*)cd)[idx]; break;             \
        default:       (dst) = 0.0; break;                                  \
        }                                                                   \
    } while (0)

    /* Hoist col_atom_null, col_is_atom, col_has_nulls outside the loops so
     * the per-element null check is a single ray_vec_is_null call when
     * nulls are present, or eliminated entirely when they are not. */
    bool vec_has_nulls = c->col_has_nulls && !c->col_is_atom;

    /* ---- SIMD-friendly fast path -----------------------------------------
     * Common case: non-atom fixed-width INTEGER column, integer set (not
     * float), small set (1..8 live elements).  Dispatch once to a loop
     * specialized per column type so the read is a direct typed pointer
     * access (no per-element `switch`) and the membership is an unrolled
     * OR-of-equality over 8 fixed slots (no `break`) — structured so
     * -O3 -march=native auto-vectorizes it.  Set elements that don't fit
     * the column type can match nothing and are dropped (kfit); unused
     * slots are padded with tv[0] (a real member, harmless under OR).
     * Null rows emit 0 regardless of negate (isn masks after the OR).
     * Bit-identical to the generic loops below; additive, chosen once. */
    /* SYM verdict-LUT fast path: one byte load per row, no set scan.
     * SYM has no null, so there is no per-element null branch either. */
    if (!c->col_is_atom && c->symlut && ct == RAY_SYM) {
        const uint8_t* lut = c->symlut;
        uint8_t neg = (uint8_t)negate;
        uint8_t w = (uint8_t)(cattrs & RAY_SYM_W_MASK);
        if (w == RAY_SYM_W8) {
            const uint8_t* dp = (const uint8_t*)cd;
            for (int64_t i = start; i < end; i++)
                ob[i - ob_base] = (uint8_t)(lut[dp[i]] ^ neg);
        } else if (w == RAY_SYM_W16) {
            const uint16_t* dp = (const uint16_t*)cd;
            for (int64_t i = start; i < end; i++)
                ob[i - ob_base] = (uint8_t)(lut[dp[i]] ^ neg);
        } else if (w == RAY_SYM_W32) {
            const uint32_t* dp = (const uint32_t*)cd;
            for (int64_t i = start; i < end; i++)
                ob[i - ob_base] = (uint8_t)(lut[dp[i]] ^ neg);
        } else {
            const int64_t* dp = (const int64_t*)cd;
            for (int64_t i = start; i < end; i++)
                ob[i - ob_base] = (uint8_t)(lut[dp[i]] ^ neg);
        }
        return;
    }

    if (!c->col_is_atom && !c->use_double && sv_len >= 1 && sv_len <= 8) {
        const int64_t* svi = c->svi;
        uint8_t neg = (uint8_t)negate;
        #define IN_FAST(CTYPE, FITS, SENT, HASNULL) do {                     \
            CTYPE tv[8];                                                     \
            int kfit = 0;                                                    \
            for (int64_t j = 0; j < sv_len; j++) {                          \
                int64_t sj = svi[j];                                         \
                if (FITS) tv[kfit++] = (CTYPE)sj;                            \
            }                                                                \
            if (kfit == 0) break; /* nothing representable → generic path */ \
            const CTYPE* dp = (const CTYPE*)cd;                              \
            /* pad unused slots with tv[0] (a real member; harmless under    \
             * OR) via ternaries so tv is never written/read past kfit. */   \
            CTYPE t0=tv[0],                                                  \
                  t1=(kfit>1?tv[1]:t0), t2=(kfit>2?tv[2]:t0),                \
                  t3=(kfit>3?tv[3]:t0), t4=(kfit>4?tv[4]:t0),                \
                  t5=(kfit>5?tv[5]:t0), t6=(kfit>6?tv[6]:t0),                \
                  t7=(kfit>7?tv[7]:t0);                                      \
            if ((HASNULL) && vec_has_nulls) {                                \
                CTYPE sent = (CTYPE)(SENT);                                  \
                for (int64_t i = start; i < end; i++) {                      \
                    CTYPE v = dp[i];                                         \
                    uint8_t f = (uint8_t)((v==t0)|(v==t1)|(v==t2)|(v==t3)|   \
                                          (v==t4)|(v==t5)|(v==t6)|(v==t7));  \
                    uint8_t isn = (uint8_t)(v == sent);                      \
                    ob[i - ob_base] = (uint8_t)(isn ? 0 : (f ^ neg));       \
                }                                                            \
            } else {                                                         \
                for (int64_t i = start; i < end; i++) {                      \
                    CTYPE v = dp[i];                                         \
                    uint8_t f = (uint8_t)((v==t0)|(v==t1)|(v==t2)|(v==t3)|   \
                                          (v==t4)|(v==t5)|(v==t6)|(v==t7));  \
                    ob[i - ob_base] = (uint8_t)(f ^ neg);                    \
                }                                                            \
            }                                                                \
            return;                                                          \
        } while (0)

        switch (ct) {
            case RAY_BOOL: case RAY_U8:  /* non-nullable → no isn path */
                IN_FAST(uint8_t, (sj >= 0 && sj <= UINT8_MAX), 0, 0);
                break;
            case RAY_I16:
                IN_FAST(int16_t, (sj >= INT16_MIN && sj <= INT16_MAX),
                        NULL_I16, 1);
                break;
            case RAY_I32: case RAY_DATE: case RAY_TIME:
                IN_FAST(int32_t, (sj >= INT32_MIN && sj <= INT32_MAX),
                        NULL_I32, 1);
                break;
            case RAY_I64: case RAY_TIMESTAMP:
                IN_FAST(int64_t, 1, NULL_I64, 1);  /* all i64 fit */
                break;
            default: break;  /* SYM / unsupported → generic below */
        }
        #undef IN_FAST
    }

    if (c->use_double) {
        const double* svf = c->svf;
        if (c->col_atom_null) {
            /* All elements are null — fill zeros */
            for (int64_t i = start; i < end; i++) ob[i - ob_base] = 0;
        } else if (vec_has_nulls) {
            for (int64_t i = start; i < end; i++) {
                if (ray_vec_is_null(col, i)) { ob[i - ob_base] = 0; continue; }
                double cv;
                if (c->col_is_atom) cv = (ct == RAY_F64) ? col->f64 : (double)col->i64;
                else IN_READ_F64(cv, i);
                int found = 0;
                for (int64_t j = 0; j < sv_len; j++)
                    if (cv == svf[j]) { found = 1; break; }
                ob[i - ob_base] = (uint8_t)(found ^ negate);
            }
        } else {
            for (int64_t i = start; i < end; i++) {
                double cv;
                if (c->col_is_atom) cv = (ct == RAY_F64) ? col->f64 : (double)col->i64;
                else IN_READ_F64(cv, i);
                int found = 0;
                for (int64_t j = 0; j < sv_len; j++)
                    if (cv == svf[j]) { found = 1; break; }
                ob[i - ob_base] = (uint8_t)(found ^ negate);
            }
        }
    } else {
        const int64_t* svi = c->svi;
        if (c->col_atom_null) {
            for (int64_t i = start; i < end; i++) ob[i - ob_base] = 0;
        } else if (vec_has_nulls) {
            for (int64_t i = start; i < end; i++) {
                if (ray_vec_is_null(col, i)) { ob[i - ob_base] = 0; continue; }
                int64_t cv;
                if (c->col_is_atom) cv = col->i64;
                else IN_READ_I64(cv, i);
                int found = 0;
                for (int64_t j = 0; j < sv_len; j++)
                    if (cv == svi[j]) { found = 1; break; }
                ob[i - ob_base] = (uint8_t)(found ^ negate);
            }
        } else {
            for (int64_t i = start; i < end; i++) {
                int64_t cv;
                if (c->col_is_atom) cv = col->i64;
                else IN_READ_I64(cv, i);
                int found = 0;
                for (int64_t j = 0; j < sv_len; j++)
                    if (cv == svi[j]) { found = 1; break; }
                ob[i - ob_base] = (uint8_t)(found ^ negate);
            }
        }
    }
    #undef IN_READ_I64
    #undef IN_READ_F64
}

/* ============================================================================
 * exec_in — membership test (col IN set_vec)
 *
 * Evaluates each element of `col` against `set`.  Returns a RAY_BOOL
 * vector of col->len.  For OP_NOT_IN the output is inverted.
 *
 * Type handling:
 *   - SYM ∈ SYM  → compare interned sym IDs as i64
 *   - Integer-family (BOOL/U8/I16/I32/I64/DATE/TIME/TIMESTAMP) on both
 *     sides → compare values as signed int64 (narrow types are
 *     sign-extended during read).
 *   - Any float on either side, mixed with each other or with
 *     integer family → promote both sides to double and compare with
 *     `==`.  This covers the common case `(in price [1 2 3])` where
 *     price is F64 and the set literal parses as I64.
 *   - SYM mixed with anything else → no matches (type-mismatch; we
 *     don't error because it's a legal Rayfall comparison that
 *     simply produces false).
 *   - RAY_STR: deferred (returns nyi).
 * ============================================================================ */
/* Build the membership probe buffer + in_worker_ctx_t shared by exec_in
 * (full BOOL output) and exec_in_to_selection (fused row-selection output).
 * Resolves the column read type, the int/float/sym classification, the
 * compacted probe buffer (null set elements dropped, cross-domain SYM ids
 * translated into the column's domain), and the per-column null flags —
 * everything in in_worker_ctx_t EXCEPT ob/ob_base, which each caller wires
 * to its own sink.  Assumes the column is non-empty (callers short-circuit
 * empty cols).  svf_stack/svi_stack are caller-owned 32-element buffers used
 * when the set has ≤32 live elements; otherwise *sv_hdr_out is heap-allocated
 * and must be ray_free'd after the dispatch.  Returns a tri-state status so
 * the bool path can surface nyi/oom while the fused path falls back. */
typedef enum { IN_CTX_UNSUPPORTED = 0, IN_CTX_OK = 1, IN_CTX_OOM = 2 } in_ctx_status_t;

static in_ctx_status_t in_build_worker_ctx(ray_t* col, ray_t* set, bool negate,
                                           double* svf_stack, int64_t* svi_stack,
                                           in_worker_ctx_t* out_ctx,
                                           ray_t** sv_hdr_out,
                                           ray_t** lut_hdr_out) {
    *sv_hdr_out = NULL;
    *lut_hdr_out = NULL;
    int64_t set_len = ray_is_atom(set) ? 1 : set->len;

    int8_t ct = ray_is_atom(col) ? (int8_t)(-col->type) : col->type;
    int8_t st = ray_is_atom(set) ? (int8_t)(-set->type) : set->type;
    if (RAY_IS_PARTED(ct)) ct = (int8_t)RAY_PARTED_BASETYPE(ct);
    if (RAY_IS_PARTED(st)) st = (int8_t)RAY_PARTED_BASETYPE(st);

    if (ct == RAY_STR || st == RAY_STR) return IN_CTX_UNSUPPORTED;

    /* Classify each side: 0=int-family, 1=float-family, 2=sym. */
    #define CLASSIFY(t)                                                    \
        ((t) == RAY_SYM ? 2 :                                              \
         ((t) == RAY_F32 || (t) == RAY_F64) ? 1 : 0)

    int col_class = CLASSIFY(ct);
    int set_class = CLASSIFY(st);

    /* Mixed SYM vs non-SYM → treat as an empty probe.  A SYM set
     * containing resolved sym IDs has no meaning when compared to a
     * raw integer column, so nothing can match — but we still drop
     * through to the main loop so null rows are handled consistently
     * (they emit 0 regardless of negate). */
    if ((col_class == 2) != (set_class == 2)) {
        set_len = 0;
    }

    /* Float-promoted path: at least one side is float.  Read both as
     * double and compare. */
    int use_double = (col_class == 1 || set_class == 1);

    /* Null-aware: null rows in the column never pass either `in` or
     * `not-in`.  Mirrors SQL-style semantics where NULL IN (…) and
     * NULL NOT IN (…) both yield UNKNOWN / false in a boolean
     * context.  Also skip null elements when building the probe
     * buffer so a non-null col row doesn't accidentally match the
     * sentinel value of a null set element. */
    bool col_has_nulls = !ray_is_atom(col) && (col->attrs & RAY_ATTR_HAS_NULLS);
    bool col_atom_null = ray_is_atom(col) && RAY_ATOM_IS_NULL(col);
    bool set_has_nulls = !ray_is_atom(set) && (set->attrs & RAY_ATTR_HAS_NULLS);

    #define READ_I64(dst, vec, type, idx) do {                             \
        const void* _d = ray_data(vec);                                    \
        switch (type) {                                                    \
        case RAY_BOOL: case RAY_U8: (dst) = ((const uint8_t*)_d)[idx]; break; \
        case RAY_I16:  (dst) = ((const int16_t*)_d)[idx]; break;           \
        case RAY_I32:  case RAY_DATE: case RAY_TIME:                       \
                       (dst) = ((const int32_t*)_d)[idx]; break;           \
        case RAY_I64:  case RAY_TIMESTAMP:                                 \
                       (dst) = ((const int64_t*)_d)[idx]; break;           \
        case RAY_SYM:  (dst) = ray_read_sym(_d, (idx), (type),             \
                                            (vec)->attrs); break;          \
        default:       (dst) = 0; break;                                   \
        }                                                                  \
    } while (0)

    #define READ_F64(dst, vec, type, idx) do {                             \
        const void* _d = ray_data(vec);                                    \
        switch (type) {                                                    \
        case RAY_BOOL: case RAY_U8: (dst) = (double)((const uint8_t*)_d)[idx]; break; \
        case RAY_I16:  (dst) = (double)((const int16_t*)_d)[idx]; break;   \
        case RAY_I32:  case RAY_DATE: case RAY_TIME:                       \
                       (dst) = (double)((const int32_t*)_d)[idx]; break;   \
        case RAY_I64:  case RAY_TIMESTAMP:                                 \
                       (dst) = (double)((const int64_t*)_d)[idx]; break;   \
        case RAY_F32:  (dst) = (double)((const float*)_d)[idx]; break;     \
        case RAY_F64:  (dst) = ((const double*)_d)[idx]; break;            \
        default:       (dst) = 0.0; break;                                 \
        }                                                                  \
    } while (0)

    /* Compact probe buffer: drop null set elements up front so the
     * inner loop doesn't special-case them. */
    int64_t sv_len = 0;
    double* svf = svf_stack;
    int64_t* svi = svi_stack;
    ray_t* sv_hdr = NULL;
    if (set_len > 32) {
        size_t bytes = (size_t)set_len * (use_double ? sizeof(double) : sizeof(int64_t));
        sv_hdr = ray_alloc(bytes);
        if (!sv_hdr) { return IN_CTX_OOM; }
        if (use_double) svf = (double*)ray_data(sv_hdr);
        else            svi = (int64_t*)ray_data(sv_hdr);
    }

    /* sym-domain Phase 2 (SYM ∈ SYM): the worker compares RAW column
     * cell ids against the probe values, so the probe must be expressed
     * in the COLUMN's id space.  When the two sides resolve over
     * different domains, translate each set element through its strings
     * into the column's domain; elements absent from it are dropped —
     * they can match nothing.  Same-domain sides (the only pre-flip
     * case) keep the raw copy: byte-identical fast path. */
    struct ray_sym_domain_s* in_col_dom = NULL;  /* non-NULL ⇒ translate */
    struct ray_sym_domain_s* in_set_dom = NULL;
    if (col_class == 2 && set_class == 2 && set_len > 0) {
        ray_t* col_rep = ray_is_atom(col) ? NULL : sym_domain_rep(col);
        ray_t* set_rep = ray_is_atom(set) ? NULL : sym_domain_rep(set);
        struct ray_sym_domain_s* cd =
            col_rep ? ray_sym_vec_domain(col_rep) : ray_sym_runtime_domain();
        struct ray_sym_domain_s* sd =
            set_rep ? ray_sym_vec_domain(set_rep) : ray_sym_runtime_domain();
        if (cd != sd) { in_col_dom = cd; in_set_dom = sd; }
    }

    /* set_len is 0 when we want to suppress the set entirely
     * (SYM-vs-non-SYM type mismatch).  Respect it in BOTH the
     * atom and vec branches so the probe stays empty. */
    if (use_double) {
        if (set_len > 0 && ray_is_atom(set)) {
            if (!RAY_ATOM_IS_NULL(set)) {
                svf[0] = (st == RAY_F64) ? set->f64 : (double)set->i64;
                sv_len = 1;
            }
        } else if (set_len > 0) {
            for (int64_t i = 0; i < set_len; i++) {
                if (set_has_nulls && ray_vec_is_null(set, i)) continue;
                READ_F64(svf[sv_len], set, st, i);
                sv_len++;
            }
        }
    } else {
        if (set_len > 0 && ray_is_atom(set)) {
            if (!RAY_ATOM_IS_NULL(set)) {
                if (in_col_dom) {
                    ray_t* s = ray_sym_domain_str(in_set_dom, set->i64);
                    int64_t xid = s ? ray_sym_domain_find(in_col_dom,
                                          ray_str_ptr(s), ray_str_len(s)) : -1;
                    if (xid >= 0) { svi[0] = xid; sv_len = 1; }
                } else {
                    svi[0] = set->i64; sv_len = 1;
                }
            }
        } else if (set_len > 0) {
            for (int64_t i = 0; i < set_len; i++) {
                if (set_has_nulls && ray_vec_is_null(set, i)) continue;
                READ_I64(svi[sv_len], set, st, i);
                if (in_col_dom) {
                    /* cross-domain: re-express in the column's domain */
                    ray_t* s = ray_sym_domain_str(in_set_dom, svi[sv_len]);
                    int64_t xid = s ? ray_sym_domain_find(in_col_dom,
                                          ray_str_ptr(s), ray_str_len(s)) : -1;
                    if (xid < 0) continue;  /* absent: matches nothing */
                    svi[sv_len] = xid;
                }
                sv_len++;
            }
        }
    }

    #undef READ_I64
    #undef READ_F64
    #undef CLASSIFY

    /* SYM column: build the per-domain-position verdict LUT over the
     * VOCABULARY (svi is already expressed in the column's domain, so a
     * verdict is one flag per domain position).  O(D) build, then the
     * worker does one byte load per row regardless of set size. */
    const uint8_t* symlut = NULL;
    if (!ray_is_atom(col) && col_class == 2 && !use_double && sv_len > 0) {
        ray_t* col_rep2 = sym_domain_rep(col);
        struct ray_sym_domain_s* cdom = col_rep2
            ? ray_sym_vec_domain(col_rep2) : ray_sym_runtime_domain();
        int64_t d = ray_sym_domain_count(cdom);
        if (d > 0 && d <= (int64_t)1 << 22) {
            ray_t* lh = ray_alloc(d);
            if (lh) {
                uint8_t* lut = (uint8_t*)ray_data(lh);
                memset(lut, 0, (size_t)d);
                for (int64_t j = 0; j < sv_len; j++)
                    if (svi[j] >= 0 && svi[j] < d) lut[svi[j]] = 1;
                *lut_hdr_out = lh;
                symlut = lut;
            }
        }
    }

    *out_ctx = (in_worker_ctx_t){
        .col = col,
        .svf = svf, .svi = svi, .sv_len = sv_len,
        .symlut = symlut,
        .ob = NULL, .ob_base = 0, .ct = ct,
        .col_has_nulls = col_has_nulls,
        .col_atom_null = col_atom_null,
        .col_is_atom = ray_is_atom(col),
        .use_double = use_double,
        .negate = negate,
    };
    *sv_hdr_out = sv_hdr;
    return IN_CTX_OK;
}

/* Public entry for the typed membership kernel — the eval-level `in`
 * routes typed vec/set shapes here so a bare (in vec set) gets the same
 * verdict-LUT / SIMD / pool-parallel treatment as the fused WHERE path
 * instead of a serial per-row hashset probe.  Returns NULL when the shape
 * is unsupported (STR etc.) — caller falls back. */
ray_t* ray_in_vec_exec(ray_t* col, ray_t* set, bool negate) {
    int64_t col_len = ray_is_atom(col) ? 1 : col->len;
    if (col_len == 0) {
        ray_t* out = ray_vec_new(RAY_BOOL, 0);
        if (!out || RAY_IS_ERR(out)) return out;
        out->len = 0;
        return out;
    }
    double  svf_stack[32];
    int64_t svi_stack[32];
    in_worker_ctx_t in_ctx;
    ray_t* sv_hdr = NULL;
    ray_t* lut_hdr = NULL;
    in_ctx_status_t st = in_build_worker_ctx(col, set, negate,
                                             svf_stack, svi_stack,
                                             &in_ctx, &sv_hdr, &lut_hdr);
    if (st != IN_CTX_OK) return NULL;   /* unsupported / OOM: caller falls back */

    ray_t* out = ray_vec_new(RAY_BOOL, col_len);
    if (!out || RAY_IS_ERR(out)) {
        if (sv_hdr) ray_free(sv_hdr);
        if (lut_hdr) ray_free(lut_hdr);
        return out;
    }
    out->len = col_len;
    in_ctx.ob = (uint8_t*)ray_data(out);
    in_ctx.ob_base = 0;
    ray_pool_t* pool = ray_pool_get();
    if (pool && col_len >= RAY_PARALLEL_THRESHOLD && !ray_is_atom(col))
        ray_pool_dispatch(pool, exec_in_worker, &in_ctx, col_len);
    else
        exec_in_worker(&in_ctx, 0, 0, col_len);
    if (sv_hdr) ray_free(sv_hdr);
    if (lut_hdr) ray_free(lut_hdr);
    return out;
}

static ray_t* exec_in(ray_graph_t* g, ray_op_t* op, ray_t* col, ray_t* set) {
    (void)g;
    bool negate = (op->opcode == OP_NOT_IN);

    int64_t col_len = ray_is_atom(col) ? 1 : col->len;

    /* Empty col: the main loop produces an empty BOOL result
     * correctly, but there's nothing to iterate, so short-circuit. */
    if (col_len == 0) {
        ray_t* out = ray_vec_new(RAY_BOOL, 0);
        if (!out || RAY_IS_ERR(out)) return out;
        out->len = 0;
        return out;
    }

    /* NOTE: we intentionally do NOT short-circuit on set_len == 0.
     * Even for an empty probe, the main loop still needs to check
     * each col row's null flag so null rows never leak through as
     * true for `not-in` (the old memset bypass did exactly that). */

    double  svf_stack[32];
    int64_t svi_stack[32];
    in_worker_ctx_t in_ctx;
    ray_t* sv_hdr = NULL;
    ray_t* lut_hdr = NULL;
    in_ctx_status_t st = in_build_worker_ctx(col, set, negate,
                                             svf_stack, svi_stack,
                                             &in_ctx, &sv_hdr, &lut_hdr);
    if (st == IN_CTX_UNSUPPORTED)
        return ray_error("nyi", "OP_IN on RAY_STR not yet implemented");
    if (st == IN_CTX_OOM)
        return ray_error("oom", NULL);

    ray_t* out = ray_vec_new(RAY_BOOL, col_len);
    if (!out || RAY_IS_ERR(out)) {
        if (sv_hdr) ray_free(sv_hdr);
        if (lut_hdr) ray_free(lut_hdr);
        return out;
    }
    out->len = col_len;
    in_ctx.ob = (uint8_t*)ray_data(out);
    in_ctx.ob_base = 0;

    ray_pool_t* pool = ray_pool_get();
    if (pool && col_len >= RAY_PARALLEL_THRESHOLD && !ray_is_atom(col))
        ray_pool_dispatch(pool, exec_in_worker, &in_ctx, col_len);
    else
        exec_in_worker(&in_ctx, 0, 0, col_len);

    if (sv_hdr) ray_free(sv_hdr);
    if (lut_hdr) ray_free(lut_hdr);
    return out;
}

/* Fused `in`/`not-in` → row selection.  Decodes a root OP_IN/OP_NOT_IN of the
 * shape (SCAN flat-col) IN (CONST set) against g->table, builds the same probe
 * buffer + worker context as exec_in, then drives the per-task rowsel builders
 * — each morsel runs exec_in_worker into a morsel-local scratch (ob_base =
 * morsel start) and streams the verdict via rowsel_emit_segment.  Bit-identical
 * to exec_in's bool output (null rows → 0 → not-selected); only the sink
 * differs.  Returns NULL+all_pass=0 for any unsupported shape so the caller
 * falls back to exec_node→ray_rowsel_from_pred unchanged. */
static int idx_filter_in_decode(ray_graph_t* g, ray_op_t* pred_op,
                                ray_t** out_col, ray_t** out_set_lit);

typedef struct { in_worker_ctx_t* ic; } in_sel_fill_ctx_t;

static void in_sel_fill(void* vctx, int64_t start, int64_t n, uint8_t* out) {
    in_sel_fill_ctx_t* c = (in_sel_fill_ctx_t*)vctx;
    in_worker_ctx_t local = *c->ic;        /* per-call copy: morsel-local sink */
    local.ob      = out;                    /* element i writes out[i - start] */
    local.ob_base = start;
    exec_in_worker(&local, 0, start, start + n);
}

ray_t* exec_in_to_selection(ray_graph_t* g, ray_op_t* pred, int64_t nrows,
                            bool* all_pass) {
    *all_pass = false;
    if (pred->opcode != OP_IN && pred->opcode != OP_NOT_IN) return NULL;

    ray_t* col = NULL;
    ray_t* set_lit = NULL;
    /* idx_filter_in_decode matches OP_IN of shape (SCAN flat-col) IN (CONST
     * set) and rejects parted/MAPCOMMON columns + non-CONST sets.  OP_NOT_IN
     * isn't a decode shape (the shared decode is IN-only), so `not-in` falls
     * back to the bool path here — still correct, just unfused. */
    if (!idx_filter_in_decode(g, pred, &col, &set_lit)) return NULL;
    if (ray_is_atom(col) || col->len != nrows) return NULL;

    bool negate = (pred->opcode == OP_NOT_IN);
    double  svf_stack[32];
    int64_t svi_stack[32];
    in_worker_ctx_t ic;
    ray_t* sv_hdr = NULL;
    ray_t* lut_hdr = NULL;
    if (in_build_worker_ctx(col, set_lit, negate, svf_stack, svi_stack,
                            &ic, &sv_hdr, &lut_hdr) != IN_CTX_OK)
        return NULL;  /* STR / OOM → bool-path fallback */

    in_sel_fill_ctx_t fctx = { &ic };
    ray_t* sel = pred_sel_drive(nrows, in_sel_fill, &fctx, all_pass);

    if (sv_hdr) ray_free(sv_hdr);
    if (lut_hdr) ray_free(lut_hdr);
    return sel;
}

static int idx_filter_decode(ray_graph_t* g, ray_op_t* pred_op,
                             ray_t** out_col, uint16_t* out_cmp_op,
                             int64_t* out_key_i, double* out_key_f,
                             int* out_is_float);

/* ---- Slice-group fusion probe (FILTER(in/eq on c) + GROUP(by c)) ----
 *
 * Called from the select-by pipeline (query.c) INSTEAD of executing the
 * WHERE filter, after ray_optimize.  When the compiled filter's predicate
 * is exactly membership on the single bare group-key column — (in c SET)
 * or (== c K), no other conjuncts, no chained filter — and that column
 * carries a fresh CSR hash index, the surviving groups are exactly the
 * key set and each group's rows are its full CSR slice: resolve the
 * slices NOW (freshness verified inside the resolver) and arm the hint
 * on g.  exec_group consumes it — aggregating the slices directly, or
 * folding them into the selection the skipped filter would have
 * produced.  Returns true when armed (caller skips the filter exec). */
bool ray_slice_group_probe(ray_graph_t* g, ray_op_t* root, ray_t* by_col) {
    if (!g || !root || !by_col || g->selection || g->sg_col) return false;
    if (root->opcode != OP_FILTER) return false;
    ray_op_t* in0 = op_child(g, root, 0);
    /* A chained FILTER input means the optimizer split a compound WHERE —
     * this predicate is one conjunct of several, not the whole filter. */
    if (!in0 || in0->opcode == OP_FILTER) return false;
    if (by_col->type != RAY_SYM || ray_is_atom(by_col)) return false;
    if (by_col->attrs & RAY_ATTR_HAS_NULLS) return false;
    if (ray_index_kind(by_col) != RAY_IDX_HASH) return false;
    ray_op_t* pred = op_child(g, root, 1);
    if (!pred) return false;

    ray_t* keys = NULL;      /* borrowed (CONST literal) */
    ray_t* eq_atom = NULL;   /* owned (built for the eq shape) */
    if (pred->opcode == OP_IN) {   /* NOT_IN also decodes — reject it */
        ray_t* c = NULL; ray_t* set = NULL;
        if (idx_filter_in_decode(g, pred, &c, &set) && c == by_col)
            keys = set;
    }
    if (!keys) {
        ray_t* c = NULL; uint16_t cop = 0;
        int64_t ki = 0; double kf = 0.0; int isf = 0;
        if (idx_filter_decode(g, pred, &c, &cop, &ki, &kf, &isf) &&
            c == by_col && cop == OP_EQ && !isf) {
            eq_atom = ray_sym(ki);
            if (!eq_atom || RAY_IS_ERR(eq_atom)) {
                if (eq_atom) ray_release(eq_atom);
                return false;
            }
            keys = eq_atom;
        }
    }
    if (!keys) return false;

    ray_idx_consults[IDX_SITE_GROUP_SLICE]++;
    ray_idx_slice_t* sl = NULL;
    ray_t* shdr = NULL;
    int64_t k = ray_index_hash_sym_slices(by_col, keys, &sl, &shdr);
    if (eq_atom) ray_release(eq_atom);
    if (k < 0) return false;
    ray_idx_hits[IDX_SITE_GROUP_SLICE]++;
    g->sg_col = by_col;
    ray_retain(by_col);
    g->sg_slices_hdr = shdr;   /* NULL when k == 0 */
    g->sg_nslices = k;
    return true;
}

/* ============================================================================
 * Recursive executor
 * ============================================================================ */

/* Decode a comparison predicate `pred_op` against g->table.  When the
 * predicate has shape (cmp_op col_scan const) and `col_scan` resolves to
 * a column in g->table that is non-parted, non-MAPCOMMON, write the column
 * pointer to *out_col, the decoded opcode to *out_cmp_op, the integer key
 * to *out_key_i, the float key to *out_key_f, and *out_is_float to
 * distinguish the two families.  Returns 1 on success, 0 on any miss.
 *
 * Eligible opcodes: OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE.
 * The HAS_NULLS check is intentionally ABSENT here — per-consult gates
 * decide whether to exclude null-bearing columns (zone uses idx_fresh_nonull;
 * hash re-checks HAS_NULLS explicitly before calling ray_index_hash_eq_rowsel).
 *
 * Float literals (F32/F64) decode into *out_key_f; integer literals decode
 * into *out_key_i.  Hash-eq only uses integer keys; zone uses both. */
/* Flatten an OP_AND tree into its conjunct predicate nodes.  Returns the
 * count, or 0 when the root is not an AND or the tree exceeds max. */
static int idx_flatten_and(ray_graph_t* g, ray_op_t* op,
                           ray_op_t** out, int max) {
    if (!op || op->opcode != OP_AND) return 0;
    ray_op_t* stack[8];
    int sp = 0, n = 0;
    stack[sp++] = op;
    while (sp > 0) {
        ray_op_t* nd = stack[--sp];
        if (nd->opcode == OP_AND) {
            ray_op_t* l = op_child(g, nd, 0);
            ray_op_t* r = op_child(g, nd, 1);
            if (!l || !r || sp + 2 > 8) return 0;
            stack[sp++] = l;
            stack[sp++] = r;
        } else {
            if (n >= max) return 0;
            out[n++] = nd;
        }
    }
    return n;
}

static int idx_filter_decode(ray_graph_t* g, ray_op_t* pred_op,
                             ray_t** out_col, uint16_t* out_cmp_op,
                             int64_t* out_key_i, double* out_key_f,
                             int* out_is_float) {
    if (!pred_op || pred_op->arity != 2) return 0;
    uint16_t opc = pred_op->opcode;
    if (opc != OP_EQ && opc != OP_NE &&
        opc != OP_LT && opc != OP_LE &&
        opc != OP_GT && opc != OP_GE) return 0;

    ray_op_t* lhs = op_child(g, pred_op, 0);
    ray_op_t* rhs = op_child(g, pred_op, 1);
    if (!lhs || !rhs) return 0;
    if (lhs->opcode != OP_SCAN || rhs->opcode != OP_CONST) return 0;
    ray_op_ext_t* lext = find_ext(g, lhs->id);
    ray_op_ext_t* rext = find_ext(g, rhs->id);
    if (!lext || !rext || !rext->literal) return 0;
    uint16_t stored_table_id = 0;
    memcpy(&stored_table_id, lext->base.pad, sizeof(uint16_t));
    if (stored_table_id != 0) return 0;  /* non-default table — skip */
    ray_t* tbl = g->table;
    if (!tbl) return 0;
    ray_t* col = ray_table_get_col(tbl, lext->sym);
    if (!col || RAY_IS_ERR(col)) return 0;
    if (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON) return 0;

    ray_t* cv = rext->literal;
    if (!cv) return 0;
    int64_t key_i = 0;
    double  key_f = 0.0;
    int     is_float = 0;
    switch (cv->type) {
    case -RAY_I64:
    case -RAY_TIMESTAMP: key_i = cv->i64;                  break;
    case -RAY_I32:
    case -RAY_DATE:
    case -RAY_TIME:      key_i = (int64_t)cv->i32;         break;
    case -RAY_I16:       key_i = (int64_t)cv->i16;         break;
    case -RAY_BOOL:
    case -RAY_U8:        key_i = (int64_t)cv->b8;          break;
    case -RAY_F64:       key_f = cv->f64;    is_float = 1; break;
    case -RAY_F32:       key_f = cv->f64;    is_float = 1; break; /* F32 atoms use f64 slot */
    case -RAY_SYM:       key_i = cv->i64;               break; /* global intern id; domain-resolved at consult */
    default: return 0;  /* str / guid — not eligible */
    }

    /* Reconcile the literal's family with the COLUMN's family.  Index
     * consults (ray_index_zone_class) dispatch on the COLUMN type, so a
     * cross-type predicate would otherwise compare against the zero-filled
     * key slot — e.g. (> f 100) on an F64 column decoded key_f=0.0 and
     * misclassified ALL/NONE. */
    int col_is_float = (col->type == RAY_F32 || col->type == RAY_F64);
    if (col_is_float && !is_float) {
        /* Integer literal vs float column: coerce, but only when the
         * value round-trips exactly through double.  Doubles have a
         * 53-bit mantissa, so |key_i| <= 2^53 guarantees (double)key_i
         * is exact; beyond that the coercion could silently shift the
         * comparison boundary — reject and let the scan handle it. */
        if (key_i > (1LL << 53) || key_i < -(1LL << 53)) return 0;
        key_f = (double)key_i;
        is_float = 1;
    } else if (!col_is_float && is_float) {
        /* Float literal vs integer-family column: the scan fallback's
         * promotion semantics own this shape — reject. */
        return 0;
    }

    *out_col      = col;
    *out_cmp_op   = opc;
    *out_key_i    = key_i;
    *out_key_f    = key_f;
    *out_is_float = is_float;
    return 1;
}

/* Decode a FILTER predicate of the shape OP_IN(SCAN(col), CONST(set_vec)).
 *
 * Same column eligibility as idx_filter_decode: default table, not parted,
 * not MAPCOMMON.
 *
 * On success, *out_col is the column vec and *out_set_lit is the CONST
 * literal vec (borrowed from the ext, valid for the lifetime of the graph).
 * Returns 1 on success, 0 if the shape is not matched. */
static int idx_filter_in_decode(ray_graph_t* g, ray_op_t* pred_op,
                                ray_t** out_col, ray_t** out_set_lit) {
    if (!pred_op || pred_op->arity != 2) return 0;
    if (pred_op->opcode != OP_IN) return 0;

    ray_op_t* lhs = op_child(g, pred_op, 0);  /* SCAN(col) */
    ray_op_t* rhs = op_child(g, pred_op, 1);  /* CONST(set_vec) */
    if (!lhs || !rhs) return 0;
    if (lhs->opcode != OP_SCAN || rhs->opcode != OP_CONST) return 0;

    ray_op_ext_t* lext = find_ext(g, lhs->id);
    ray_op_ext_t* rext = find_ext(g, rhs->id);
    if (!lext || !rext || !rext->literal) return 0;

    uint16_t stored_table_id = 0;
    memcpy(&stored_table_id, lext->base.pad, sizeof(uint16_t));
    if (stored_table_id != 0) return 0;  /* non-default table — skip */

    ray_t* tbl = g->table;
    if (!tbl) return 0;
    ray_t* col = ray_table_get_col(tbl, lext->sym);
    if (!col || RAY_IS_ERR(col)) return 0;
    if (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON) return 0;

    *out_col     = col;
    *out_set_lit = rext->literal;
    return 1;
}

/* Execute a pushed-down filter interposed as a GROUP's inputs[0]
 * (GROUP predicate pushdown, opt.c Task-3).
 *
 * Return values (three-way, check in this order):
 *   • RAY_IS_ERR(result) — propagate the error immediately.
 *   • result != NULL      — compacted table; caller owns the reference,
 *                           should group this table instead of g->table.
 *   • result == NULL      — lazy path: g->selection was installed by the
 *                           filter; caller groups g->table as-is. */
static ray_t* exec_pushed_group_filter(ray_graph_t* g, ray_op_t* filter_op) {
    ray_t* fres = exec_node(g, filter_op);
    /* NULL must not leak out: callers read NULL as "lazy, selection
     * installed" — a failed exec would silently group unfiltered rows. */
    if (!fres) return ray_error("nyi", "pushed group filter yielded no result");
    if (RAY_IS_ERR(fres)) return fres;
    if (fres->type == RAY_TABLE && fres != g->table) {
        return fres;  /* compacted table; caller owns ref */
    }
    ray_release(fres);  /* lazy: original table back, selection installed */
    return NULL;
}

/* Collect the distinct source-column syms referenced by the expression
 * subtrees under roots[] into syms[] (at most max), and report via
 * *has_expr whether any root is a computed expression rather than a plain
 * scan or literal.  Only scalar expression nodes (OP_SCAN/OP_CONST and the
 * element-wise range OP_ROUND..OP_IDIV) are admitted in the walk; any other
 * opcode, an oversized graph, or overflow returns -1 — callers must treat
 * that as "unknown" and skip the optimisation. */
static int collect_scan_syms(ray_graph_t* g, const uint32_t* roots, int nroots,
                             int64_t* syms, int max, bool* has_expr) {
    *has_expr = false;
    uint32_t nc = g->node_count;
    if (nc > 4096) return -1;

    uint32_t stack[64];
    int sp = 0, n = 0;
    bool visited[4096];
    memset(visited, 0, nc * sizeof(bool));

    for (int r = 0; r < nroots; r++) {
        if (roots[r] >= nc) return -1;
        uint16_t opc = g->nodes[roots[r]].opcode;
        if (opc != OP_SCAN && opc != OP_CONST) *has_expr = true;
        if (sp >= 64) return -1;
        stack[sp++] = roots[r];
    }

    while (sp > 0) {
        uint32_t nid = stack[--sp];
        if (nid >= nc || visited[nid]) continue;
        visited[nid] = true;
        ray_op_t* node = &g->nodes[nid];
        if (node->flags & OP_FLAG_DEAD) continue;

        if (node->opcode == OP_SCAN) {
            ray_op_ext_t* sx = find_ext(g, nid);
            if (!sx) return -1;
            bool dup = false;
            for (int j = 0; j < n; j++)
                if (syms[j] == sx->sym) { dup = true; break; }
            if (!dup) {
                if (n >= max) return -1;
                syms[n++] = sx->sym;
            }
            continue;
        }
        if (node->opcode == OP_CONST) continue;
        /* Admit only scalar element-wise expression nodes. */
        if (node->opcode < OP_ROUND || node->opcode > OP_IDIV) return -1;

        for (int i = 0; i < node->arity && i < 2; i++) {
            if (node->in_id[i] == RAY_OP_NONE) continue;
            if (sp >= 64) return -1;
            stack[sp++] = node->in_id[i];
        }
        /* 3-ary ops (OP_IF else / OP_SUBSTR len / OP_REPLACE repl) keep the
         * third operand in ext->third_in — missing it would compact away a
         * referenced column. */
        ray_op_ext_t* nx = find_ext(g, nid);
        if (nx && nx->third_in != RAY_OP_NONE && nx->third_in != 0) {
            if (sp >= 64) return -1;
            stack[sp++] = nx->third_in;
        }
    }
    return n;
}

/* collect_scan_syms over a GROUP's key + agg-input subtrees. */
static int group_input_scan_syms(ray_graph_t* g, ray_op_ext_t* gx,
                                 int64_t* syms, int max, bool* has_expr) {
    uint32_t roots[3 * 16];
    int nroots = 0;
    for (uint8_t k = 0; k < gx->n_keys && nroots < 48; k++)
        roots[nroots++] = gx->keys[k];
    for (uint8_t a = 0; a < gx->n_aggs && nroots < 48; a++) {
        if (gx->agg_ins && gx->agg_ins[a] != RAY_OP_NONE)
            roots[nroots++] = gx->agg_ins[a];
        if (gx->agg_ins2 && gx->agg_ins2[a] != RAY_OP_NONE && nroots < 48)
            roots[nroots++] = gx->agg_ins2[a];
    }
    return collect_scan_syms(g, roots, nroots, syms, max, has_expr);
}

/* Lowercase display label for profiler spans.  ray_opcode_name returns the
 * canonical UPPERCASE IR name — right for plan dumps and error messages, but
 * the profiler tree also carries lowercase phase and builtin labels (parse,
 * optimize, select, til), so an uppercase GROUP next to them reads as an
 * arbitrary mix.  Lowercase, and render '_' as a space to match the
 * space-separated pass labels (predicate pushdown, …).  Cached per opcode so
 * the returned pointer stays valid for the life of a span. */
static const char* prof_op_label(uint16_t opc) {
#define PROF_OP_LABEL_MAX OP_WAVG
    if (opc > PROF_OP_LABEL_MAX) return ray_opcode_name(opc);
    static char lc[PROF_OP_LABEL_MAX + 1][28];
    if (!lc[opc][0]) {
        const char* up = ray_opcode_name(opc);
        if (!up) return NULL;
        size_t i = 0;
        for (; i < sizeof(lc[0]) - 1 && up[i]; i++) {
            char c = up[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            else if (c == '_')        c = ' ';
            lc[opc][i] = c;
        }
        lc[opc][i] = '\0';
    }
    return lc[opc];
#undef PROF_OP_LABEL_MAX
}

/* Is this opcode a "heavy" pipeline breaker worth profiling? */
static inline bool op_is_heavy(uint16_t opc) {
    return opc == OP_FILTER || opc == OP_SORT || opc == OP_GROUP ||
           opc == OP_JOIN   || opc == OP_WINDOW_JOIN || opc == OP_SELECT ||
           opc == OP_HEAD   || opc == OP_TAIL || opc == OP_WINDOW ||
           opc == OP_PIVOT  ||
           opc == OP_LAG    || opc == OP_LEAD || opc == OP_DELTAS ||
           opc == OP_RATIOS || opc == OP_FILLS || opc == OP_SUMS ||
           opc == OP_AVGS   || opc == OP_MINS  || opc == OP_MAXS ||
           opc == OP_PRDS   || opc == OP_DIFFER ||
           opc == OP_MSUM   || opc == OP_MAVG || opc == OP_MMIN ||
           opc == OP_MMAX   || opc == OP_MCOUNT || opc == OP_MVAR ||
           opc == OP_MDEV   ||
           (opc >= OP_EXPAND && opc <= OP_KNN_RERANK);
}

/* ── Deep-DAG stack discipline ──────────────────────────────────────
 * exec_node/exec_node_inner recurse per child, and exec_node_inner's
 * frame is the union of every case's locals (~35 KB under ASan), so a
 * deep op chain overflows the 8 MB main stack around depth ~220 —
 * worker/IPC threads with 512 KB stacks die far sooner.  Two defenses:
 *
 *  1. Elementwise chains — the one shape that gets arbitrarily deep in
 *     practice (machine-generated arithmetic, the >1024-node streaming
 *     DAG test) — are evaluated iteratively by exec_elementwise_tree
 *     and consume no recursion depth.
 *  2. Any other graph nesting past RAY_EXEC_MAX_DEPTH is pathological:
 *     fail with a clean "limit" error instead of a stack overflow
 *     (mirrors RAY_EVAL_MAX_DEPTH in lang/eval.c).  The counter is
 *     per-thread; 64 levels of non-elementwise nesting stays within
 *     ~2.5 MB even at ASan frame sizes. */
#define RAY_EXEC_MAX_DEPTH 64
static _Thread_local int32_t tl_exec_depth = 0;

/* Opcodes handled by the elementwise case of exec_node_inner — must
 * match its case labels (ROUND sits at 9, outside NEG..CAST; IDIV at
 * 49, outside ADD..MAX2). */
static inline bool op_is_elementwise(uint16_t o) {
    return o == OP_ROUND || (o >= OP_NEG && o <= OP_CAST)
        || (o >= OP_ADD && o <= OP_MAX2) || o == OP_IDIV;
}

/* Iterative post-order evaluation of an elementwise subtree.
 *
 * Replaces the recursive per-child fallback, which burned one
 * exec_node+exec_node_inner frame pair per chain link.  The walk uses
 * an explicit heap stack:
 *
 *  - elementwise children are evaluated exactly once (vals[] memo,
 *    released when their last in-tree consumer has run);
 *  - non-elementwise children (SCAN/CONST/aggregates/...) remain
 *    exec_node calls, evaluated per reference like the recursion did,
 *    and bounded by the depth guard;
 *  - each node still gets an expr_compile attempt on descent, so
 *    fuseable sub-windows (within the compiler's depth-64 DFS cap)
 *    run the fused kernels exactly as before — only the spine above
 *    them is interpreted node-by-node. */
static ray_t* exec_elementwise_tree(ray_graph_t* g, ray_op_t* root) {
    uint32_t nc = g->node_count;
    int64_t  nr = g->table ? ray_table_nrows(g->table) : 0;

    typedef struct { uint32_t id; uint8_t phase; } ew_ent_t;
    enum { EW_NEW = 0, EW_PENDING = 1, EW_DONE = 2, EW_SEEN = 3 };

    /* One carve for the id-indexed maps plus the walk stack.  Stack
     * bound: one discover push per edge (≤ 2·nc) + one emit push per
     * node (≤ nc).  The byte-wide state map goes last so every wider
     * field stays naturally aligned. */
    size_t vals_sz = (size_t)nc * sizeof(ray_t*);
    size_t uses_sz = (size_t)nc * sizeof(uint32_t);
    size_t stk_sz  = ((size_t)3 * nc + 2) * sizeof(ew_ent_t);
    ray_t* hdr = NULL;
    char* mem = (char*)scratch_calloc(&hdr, vals_sz + uses_sz + stk_sz + (size_t)nc);
    if (!mem) return ray_error("oom", NULL);
    ray_t**   vals  = (ray_t**)mem;
    uint32_t* uses  = (uint32_t*)(mem + vals_sz);
    ew_ent_t* stk   = (ew_ent_t*)(mem + vals_sz + uses_sz);
    uint8_t*  state = (uint8_t*)(mem + vals_sz + uses_sz + stk_sz);

    ray_t* result = NULL;
    int64_t sp = 0;

    /* Prepass: count each node's in-tree consumers (parent edges from
     * elementwise nodes, +1 for the root's return reference) BEFORE
     * evaluating anything.  Counting lazily during the walk is wrong
     * for diamond sharing: the first parent to emit would see the
     * shared child at 1 use, drop it to 0 and free the value while a
     * later parent still needs it.  Marks nodes EW_SEEN, which the
     * evaluation walk below treats as unvisited. */
    uses[root->id] = 1;
    stk[sp++] = (ew_ent_t){root->id, 0};
    while (sp > 0) {
        uint32_t id = stk[--sp].id;
        if (state[id] != EW_NEW) continue;
        state[id] = EW_SEEN;
        ray_op_t* n = op_node(g, id);
        if (!n) { result = ray_error("nyi", NULL); goto done; }
        for (int k = 0; k < n->arity; k++) {
            ray_op_t* c = op_child(g, n, k);
            if (c && op_is_elementwise(c->opcode)) {
                uses[c->id]++;                    /* one per edge */
                stk[sp++] = (ew_ent_t){c->id, 0};
            }
        }
    }

    stk[sp++] = (ew_ent_t){root->id, 0};

    while (sp > 0) {
        ew_ent_t e = stk[--sp];
        ray_op_t* n = op_node(g, e.id);
        if (!n) { result = ray_error("nyi", NULL); goto done; }

        if (e.phase == 0) {                       /* discover */
            if (state[e.id] == EW_DONE)           /* shared node, evaluated */
                continue;
            if (state[e.id] == EW_PENDING) {
                /* Re-discovering a node whose emit is still pending means
                 * the node is its own ancestor — a cyclic graph.  Refuse
                 * rather than read a missing value. */
                result = ray_error("nyi", NULL);
                goto done;
            }
            state[e.id] = EW_PENDING;

            /* Fusion attempt on descent (root's was just done by the
             * caller).  The recursion re-tried expr_compile at every
             * level; keep that so the deepest fuseable window still
             * runs compiled. */
            if (n != root && g->table && nr > 0) {
                ray_expr_t ex;
                if (expr_compile(g, g->table, n, &ex)) {
                    ray_t* vec = expr_eval_full(&ex, nr);
                    if (vec && !RAY_IS_ERR(vec)) {
                        vals[e.id]  = vec;
                        state[e.id] = EW_DONE;
                        continue;
                    }
                    ray_error_free(vec);   /* fall through to per-node eval */
                }
            }

            stk[sp++] = (ew_ent_t){e.id, 1};
            for (int k = 0; k < n->arity; k++) {
                ray_op_t* c = op_child(g, n, k);
                if (c && op_is_elementwise(c->opcode))
                    stk[sp++] = (ew_ent_t){c->id, 0};
                /* non-elementwise children evaluate at emit time */
            }
        } else {                                  /* emit */
            ray_t* in[2]    = {NULL, NULL};
            bool   owned[2] = {false, false};     /* boundary refs to release */
            for (int k = 0; k < n->arity; k++) {
                ray_op_t* c = op_child(g, n, k);
                if (c && op_is_elementwise(c->opcode)) {
                    in[k] = vals[c->id];
                    if (!in[k]) { result = ray_error("nyi", NULL); goto emit_fail; }
                } else {
                    in[k] = exec_node(g, c);      /* may recurse; depth-guarded */
                    owned[k] = true;
                    if (!in[k] || RAY_IS_ERR(in[k])) {
                        result = in[k];
                        in[k] = NULL;
                        goto emit_fail;
                    }
                }
            }

            ray_t* out = (n->arity == 1)
                       ? exec_elementwise_unary(g, n, in[0])
                       : exec_elementwise_binary(g, n, in[0], in[1]);

            for (int k = 0; k < n->arity; k++) {
                if (owned[k]) {
                    ray_release(in[k]);
                } else {
                    uint32_t cid = op_child(g, n, k)->id;
                    if (--uses[cid] == 0) {
                        ray_release(vals[cid]);
                        vals[cid] = NULL;
                    }
                }
            }
            if (!out || RAY_IS_ERR(out)) { result = out; goto done; }
            vals[e.id]  = out;
            state[e.id] = EW_DONE;
            continue;

        emit_fail:
            for (int k = 0; k < n->arity; k++)
                if (owned[k] && in[k]) ray_release(in[k]);
            goto done;
        }
    }

    result = vals[root->id];
    vals[root->id] = NULL;

done:
    /* Release stragglers: partially-evaluated memo entries on the error
     * path (the success path has drained all consumers already). */
    for (uint32_t i = 0; i < nc; i++)
        if (vals[i]) ray_release(vals[i]);
    scratch_free(hdr);
    return result;
}

ray_t* exec_node(ray_graph_t* g, ray_op_t* op) {
    if (!op) return ray_error("nyi", NULL);

    /* Per-op cancellation checkpoint. Long fused pipelines iterate
     * exec_node many times; this catches Ctrl-C between operators
     * without adding cost to the per-row hot path. */
    if (ray_interrupted()) return ray_error("cancel", "interrupted");

    /* Recursion depth guard — see RAY_EXEC_MAX_DEPTH above. */
    if (++tl_exec_depth > RAY_EXEC_MAX_DEPTH) {
        tl_exec_depth--;
        return ray_error("limit", "exec depth exceeded");
    }

    bool heavy = op_is_heavy(op->opcode);
    bool profiling = g_ray_profile.active && heavy;
    const char* oname = NULL;
    if (heavy) {
        oname = prof_op_label(op->opcode);
        /* Relabel progress without touching counters — leaf ops that
         * drive their own rows_done/rows_total still work; ops that
         * don't get a spinner-style indeterminate bar until they
         * either finish or emit their own update. */
        ray_progress_label(oname, NULL);
        if (profiling) {
            ray_prof_span_t* sp = ray_profile_span_start(oname);
            if (sp) {
                int64_t cur = 0; ray_sys_get_stat(&cur, NULL);
                sp->sys_cur    = cur;
                sp->qs_rows    = ray_qstats_sum_rows();
                uint64_t sum = 0, mx = 0; uint32_t used = 0;
                ray_qstats_agg(&used, &sum, &mx);
                sp->qs_busy_ns = sum;
            }
        }
    }

    ray_t* _prof_result = exec_node_inner(g, op);
    tl_exec_depth--;

    if (profiling) {
        ray_prof_span_t* ep = ray_profile_span_end(oname);
        if (ep) {
            int64_t cur = 0; ray_sys_get_stat(&cur, NULL);
            ep->sys_cur = cur;
            ep->qs_rows = ray_qstats_sum_rows();
            uint64_t sum = 0, mx = 0; uint32_t used = 0;
            ray_qstats_agg(&used, &sum, &mx);
            ep->qs_busy_ns = sum;
            ep->qs_workers = used;
            /* Result footprint — rows this operator produced. */
            if (_prof_result && !RAY_IS_ERR(_prof_result) && !RAY_IS_NULL(_prof_result)) {
                /* Scalar atoms alias their value into the len field, so
                 * ray_len would report the value, not a row count. */
                ep->rows_out  = (_prof_result->type == RAY_TABLE) ? ray_table_nrows(_prof_result)
                              : ray_is_atom(_prof_result)         ? 1
                              :                                      (int64_t)ray_len(_prof_result);
            }
        }
    }

    return _prof_result;
}

static ray_t* exec_node_inner(ray_graph_t* g, ray_op_t* op) {
    if (!op) return ray_error("nyi", NULL);

    switch (op->opcode) {
        case OP_SCAN: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            if (!ext) return ray_error("nyi", NULL);

            /* Resolve table: pad[0..1] stores table_id+1 (0 = default g->table) */
            uint16_t stored_table_id = 0;
            memcpy(&stored_table_id, ext->base.pad, sizeof(uint16_t));
            ray_t* scan_tbl;
            if (stored_table_id > 0 && g->tables && (stored_table_id - 1) < g->n_tables) {
                scan_tbl = g->tables[stored_table_id - 1];
            } else {
                scan_tbl = g->table;
            }
            if (!scan_tbl) return ray_error("schema", NULL);
            ray_t* col = ray_table_get_col(scan_tbl, ext->sym);
            if (!col) return ray_error("schema", NULL);
            if (col->type == RAY_MAPCOMMON)
                return materialize_mapcommon(col);
            if (RAY_IS_PARTED(col->type)) {
                /* Concat parted segments into flat vector (cold path) */
                int8_t base = (int8_t)RAY_PARTED_BASETYPE(col->type);
                ray_t** sps = (ray_t**)ray_data(col);
                int64_t total = ray_parted_nrows(col);

                /* RAY_STR: deep-copy to handle multi-pool segments */
                if (base == RAY_STR)
                    return parted_flatten_str(sps, col->len, total);

                uint8_t sba = (base == RAY_SYM)
                            ? parted_sym_max_attrs(sps, col->len) : 0;
                ray_t* flat = typed_vec_new(base, sba, total);
                if (!flat || RAY_IS_ERR(flat)) return ray_error("oom", NULL);
                flat->len = total;
                ray_t** segs = sps;
                int64_t off = 0;
                uint8_t seg_nulls = 0;
                for (int64_t s = 0; s < col->len; s++) {
                    if (!segs[s] || segs[s]->len <= 0) continue;
                    parted_copy_cells(ray_data(flat), base, sba, off,
                                      segs[s], 0, segs[s]->len);
                    seg_nulls |= segs[s]->attrs & RAY_ATTR_HAS_NULLS;
                    off += segs[s]->len;
                }
                /* Invariant 16.4: propagate HAS_NULLS across segments. */
                flat->attrs |= seg_nulls;
                /* Raw-copied SYM cell ids resolve over the partitions'
                 * shared domain (first SYM segment is the rep). */
                if (flat->type == RAY_SYM)
                    ray_sym_vec_adopt_domain(flat, sym_domain_rep(col));
                return flat;
            }
            ray_retain(col);
            return col;
        }

        case OP_CONST: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            if (!ext || !ext->literal) return ray_error("nyi", NULL);
            ray_retain(ext->literal);
            return ext->literal;
        }

        /* Membership: col IN set_vec */
        case OP_IN: case OP_NOT_IN: {
            ray_t* col = exec_node(g, op_child(g, op, 0));
            if (!col || RAY_IS_ERR(col)) return col;
            ray_t* set = exec_node(g, op_child(g, op, 1));
            if (!set || RAY_IS_ERR(set)) { ray_release(col); return set; }
            ray_t* result = exec_in(g, op, col, set);
            ray_release(col);
            ray_release(set);
            return result;
        }

        /* Unary element-wise */
        case OP_NEG: case OP_ABS: case OP_NOT: case OP_SQRT:
        case OP_LOG: case OP_EXP: case OP_CEIL: case OP_FLOOR: case OP_ROUND:
        case OP_ISNULL: case OP_CAST:
        /* Binary element-wise */
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_IDIV: case OP_MOD:
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
        case OP_GT: case OP_GE: case OP_AND: case OP_OR:
        case OP_MIN2: case OP_MAX2: {
            /* Try compiled expression first (fuses entire subtree) */
            if (g->table) {
                int64_t nr = ray_table_nrows(g->table);
                if (nr > 0) {
                    ray_expr_t ex;
                    if (expr_compile(g, g->table, op, &ex)) {
                        ray_t* vec = expr_eval_full(&ex, nr);
                        if (vec && !RAY_IS_ERR(vec)) return vec;
                    }
                }
            }
            /* Fallback: iterative per-node evaluation.  Deep chains
             * (>1024-node NEG spines, machine-generated arithmetic)
             * previously recursed one exec_node frame pair per link
             * and overflowed the stack — see exec_elementwise_tree. */
            return exec_elementwise_tree(g, op);
        }

        /* Reductions */
        case OP_SUM: case OP_PROD: case OP_ALL: case OP_ANY:
        case OP_MIN: case OP_MAX:
        case OP_COUNT: case OP_AVG: case OP_FIRST: case OP_LAST:
        case OP_STDDEV: case OP_STDDEV_POP: case OP_VAR: case OP_VAR_POP: {
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            /* Compact lazy selection before reducing — filters may have
             * set g->selection without materializing a compacted table. */
            bool own_input = (input != g->table);
            if (g->selection && input->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, input, g->selection, NULL, 0);
                if (own_input) ray_release(input);
                ray_release(g->selection);
                g->selection = NULL;
                input = compacted;
                own_input = true;
            }
            /* Canonical aggregand type-admission — reject non-admitted element
             * types (SYM/STR/GUID everywhere; DATE/TIMESTAMP for sum) so the
             * DAG reduction matches the scalar builtins instead of aggregating
             * raw ids / bytes / date counts. */
            if (input && input->type != RAY_TABLE) {
                int8_t et = RAY_IS_PARTED(input->type)
                          ? (int8_t)RAY_PARTED_BASETYPE(input->type) : input->type;
                if (et > 0 && !agg_type_admitted(op->opcode, et)) {
                    if (own_input) ray_release(input);
                    return ray_error("type", "aggregate: element type not admitted for this aggregation, got %s", ray_type_name(et));
                }
            }
            ray_t* result = exec_reduction(g, op, input);
            if (own_input) ray_release(input);
            return result;
        }

        case OP_COUNT_DISTINCT: {
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            ray_t* result = exec_count_distinct(g, op, input);
            ray_release(input);
            return result;
        }

        case OP_DISTINCT: {
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            if (!ray_is_vec(input)) {
                ray_release(input);
                return ray_error("type", "OP_DISTINCT expects a vector input");
            }
            ray_t* result = distinct_vec_eager(input);
            ray_release(input);
            return result;
        }

        case OP_ASC: {
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            if (!ray_is_vec(input)) {
                ray_release(input);
                return ray_error("type", "OP_ASC expects a vector input");
            }
            ray_t* result = asc_vec_eager(input);
            ray_release(input);
            return result;
        }

        case OP_DESC: {
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            if (!ray_is_vec(input)) {
                ray_release(input);
                return ray_error("type", "OP_DESC expects a vector input");
            }
            ray_t* result = desc_vec_eager(input);
            ray_release(input);
            return result;
        }

        case OP_REVERSE: {
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            if (!ray_is_vec(input)) {
                ray_release(input);
                return ray_error("type", "OP_REVERSE expects a vector input");
            }
            ray_t* result = reverse_vec_eager(input);
            ray_release(input);
            return result;
        }

        case OP_LAG: case OP_LEAD: case OP_DELTAS: case OP_RATIOS:
        case OP_FILLS: case OP_SUMS: case OP_AVGS: case OP_MINS:
        case OP_MAXS: case OP_PRDS: case OP_DIFFER:
        case OP_MSUM: case OP_MAVG: case OP_MMIN: case OP_MMAX:
        case OP_MCOUNT: case OP_MVAR: case OP_MDEV: {
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            if (!ray_is_vec(input)) {
                ray_release(input);
                return ray_error("type", "%s expects a vector input", ray_opcode_name(op->opcode));
            }
            ray_t* result = NULL;
            switch (op->opcode) {
                case OP_LAG:    result = lag_vec_eager(input); break;
                case OP_LEAD:   result = lead_vec_eager(input); break;
                case OP_DELTAS: result = deltas_vec_eager(input); break;
                case OP_RATIOS: result = ratios_vec_eager(input); break;
                case OP_FILLS:  result = fills_vec_eager(input); break;
                case OP_SUMS:   result = running_vec_eager(input, OP_SUMS); break;
                case OP_AVGS:   result = running_vec_eager(input, OP_AVGS); break;
                case OP_MINS:   result = running_vec_eager(input, OP_MINS); break;
                case OP_MAXS:   result = running_vec_eager(input, OP_MAXS); break;
                case OP_PRDS:   result = running_vec_eager(input, OP_PRDS); break;
                case OP_DIFFER: result = differ_vec_eager(input); break;
                default: {
                    ray_op_ext_t* ext = find_ext(g, op->id);
                    int64_t window = ext ? ext->param : 0;
                    result = moving_vec_eager(input, op->opcode, window);
                    break;
                }
            }
            ray_release(input);
            return result;
        }

        case OP_FILTER: {
            /* HAVING fusion: FILTER(GROUP) — evaluate the predicate against
             * the GROUP result rather than the original input table.
             * SCAN nodes in the predicate tree resolve column names via
             * g->table, so we temporarily swap it to the GROUP output. */
            ray_op_t* filter_child = op_child(g, op, 0);
            if (filter_child && filter_child->opcode == OP_GROUP) {
                ray_t* group_result = exec_node(g, filter_child);
                if (!group_result || RAY_IS_ERR(group_result))
                    return group_result;

                ray_t* saved_table = g->table;
                ray_t* saved_sel   = g->selection;
                g->table     = group_result;
                g->selection = NULL;

                ray_t* pred = exec_node(g, op_child(g, op, 1));

                g->table     = saved_table;
                g->selection = saved_sel;

                if (!pred || RAY_IS_ERR(pred)) {
                    ray_release(group_result);
                    return pred;
                }

                ray_t* result = exec_filter(g, op, group_result, pred);
                ray_release(pred);
                ray_release(group_result);
                return result;
            }

            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            /* Hash-index point-lookup fast path: when the predicate is
             * `col == K` on a column with RAY_IDX_HASH attached and
             * built for the column's current length, install the
             * matching rowsel on g->selection directly — bypasses
             * both the O(rows) compare AND the O(rows) BOOL→rowsel
             * scan.  Only fires for the lazy TABLE-input case with no
             * pre-existing selection (the entry shape downstream
             * group-by / sort already expects). */
            if (input->type == RAY_TABLE && !g->selection) {
                ray_t*   col      = NULL;
                uint16_t cmp_op   = 0;
                int64_t  key_i    = 0;
                double   key_f    = 0.0;
                int      is_float = 0;
                if (idx_filter_decode(g, op_child(g, op, 1), &col, &cmp_op,
                                      &key_i, &key_f, &is_float)) {
                    /* 1. Zone index: O(1) all/none short-circuit. */
                    if (ray_index_kind(col) == RAY_IDX_ZONE)
                        ray_idx_consults[IDX_SITE_FILTER_ZONE]++;
                    switch (ray_index_zone_class(col, cmp_op, key_i, key_f)) {
                    case RAY_ZONE_ALL:
                        ray_idx_hits[IDX_SITE_FILTER_ZONE]++;
                        /* All rows pass — mirror the lazy path: leave
                         * g->selection NULL (no filter) and return the
                         * table.  Same ownership as the hash path below. */
                        return input;
                    case RAY_ZONE_NONE: {
                        ray_idx_hits[IDX_SITE_FILTER_ZONE]++;
                        int64_t nrows = ray_table_nrows(input);
                        ray_t* empty = ray_index_empty_rowsel(nrows);
                        if (empty) { g->selection = empty; return input; }
                        break;  /* OOM — fall through to scan */
                    }
                    default: break;
                    }

                    /* 2. Bloom definite-absent: EQ + integer-family only. */
                    if (cmp_op == OP_EQ && !is_float &&
                        ray_index_kind(col) == RAY_IDX_BLOOM) {
                        ray_idx_consults[IDX_SITE_FILTER_BLOOM]++;
                        if (ray_index_bloom_absent(col, key_i)) {
                            int64_t nrows = ray_table_nrows(input);
                            ray_t* empty = ray_index_empty_rowsel(nrows);
                            if (empty) {
                                ray_idx_hits[IDX_SITE_FILTER_BLOOM]++;
                                g->selection = empty;
                                return input;
                            }
                            /* OOM — fall through to scan */
                        }
                    }

                    /* 3. Hash-eq: integer or SYM EQ; re-check HAS_NULLS. */
                    if (cmp_op == OP_EQ && !is_float &&
                        !(col->attrs & RAY_ATTR_HAS_NULLS)) {
                        /* SYM equality: idx_filter_decode passes the global intern id;
                         * the hash was built over per-column domain ids.  Resolve here. */
                        bool sym_probe_skip = false;
                        if (col->type == RAY_SYM) {
                            ray_t* cs = ray_sym_str(key_i);
                            int64_t dom_id = cs
                                ? ray_sym_domain_find(ray_sym_vec_domain(col),
                                                      ray_str_ptr(cs), ray_str_len(cs))
                                : -1;
                            if (dom_id < 0) {
                                /* Symbol absent from this column's domain → no rows. */
                                int64_t nrows = ray_table_nrows(input);
                                ray_t* empty = ray_index_empty_rowsel(nrows);
                                if (empty) { g->selection = empty; return input; }
                                /* OOM — fall through to scan.  key_i here is
                                 * still the GLOBAL intern id, not dom_id; under
                                 * the current singleton domain these are equal
                                 * so the scan yields the correct zero matches.
                                 * If domains ever diverge, this degraded OOM
                                 * path would need key_i re-mapped. */
                                sym_probe_skip = true;
                            } else {
                                key_i = dom_id;
                            }
                        }
                        if (!sym_probe_skip) {
                            if (ray_index_kind(col) == RAY_IDX_HASH)
                                ray_idx_consults[IDX_SITE_FILTER_HASH]++;
                            ray_t* sel = ray_index_hash_eq_rowsel(col, key_i);
                            if (sel) {
                                ray_idx_hits[IDX_SITE_FILTER_HASH]++;
                                g->selection = sel;
                                return input;
                            }
                            /* sel == NULL: ineligible (wrong/stale index kind,
                             * key out of range) or OOM — fall through to scan. */
                        }
                    }

                    /* 4. Sort-index range: EQ/LT/LE/GT/GE (NE returns NULL). */
                    if (cmp_op != OP_NE) {
                        if (ray_index_kind(col) == RAY_IDX_SORT) {
                            ray_idx_consults[IDX_SITE_FILTER_RANGE]++;
                            ray_t* sel = ray_index_range_rowsel(col, cmp_op,
                                                                key_i, key_f,
                                                                (bool)is_float);
                            if (sel) {
                                ray_idx_hits[IDX_SITE_FILTER_RANGE]++;
                                g->selection = sel;
                                return input;
                            }
                        }
                    }
                }

                /* 4.5 Eq+range conjunction: (col0 == K) AND (range cmps on
                 * ONE other column).  The eq conjunct's CSR index slice
                 * gives the candidate rows; when the range column is
                 * non-descending ALONG that slice — verified with a
                 * bail-early scan, free on a parted (key,range)-sorted
                 * layout where the slice is contiguous and the range
                 * column is the secondary sort key — the bounds binary-
                 * search to a sub-slice and the whole compound WHERE
                 * collapses to O(slice-verify + log slice). */
                {
                    ray_op_t* conj[4];
                    int ncj = idx_flatten_and(g, op_child(g, op, 1), conj, 4);
                    ray_t* eq_col = NULL;
                    ray_t* rg_col = NULL;
                    int64_t eq_key = 0;
                    int64_t lo = INT64_MIN, hi = INT64_MAX;
                    bool shape_ok = (ncj >= 2);
                    for (int ci = 0; ci < ncj && shape_ok; ci++) {
                        ray_t* c = NULL; uint16_t cop = 0;
                        int64_t ki = 0; double kf = 0.0; int isf = 0;
                        if (!idx_filter_decode(g, conj[ci], &c, &cop,
                                               &ki, &kf, &isf) || isf) {
                            shape_ok = false; break;
                        }
                        if (cop == OP_EQ && !eq_col &&
                            ray_index_kind(c) == RAY_IDX_HASH &&
                            !(c->attrs & RAY_ATTR_HAS_NULLS)) {
                            eq_col = c; eq_key = ki;
                        } else if (cop == OP_GE || cop == OP_GT ||
                                   cop == OP_LE || cop == OP_LT) {
                            if (c->attrs & RAY_ATTR_HAS_NULLS) { shape_ok = false; break; }
                            if (!rg_col) rg_col = c;
                            else if (rg_col != c) { shape_ok = false; break; }
                            if (cop == OP_GE)      { if (ki > lo) lo = ki; }
                            else if (cop == OP_GT) { if (ki == INT64_MAX) { lo = 1; hi = 0; } else if (ki + 1 > lo) lo = ki + 1; }
                            else if (cop == OP_LE) { if (ki < hi) hi = ki; }
                            else                   { if (ki == INT64_MIN) { lo = 1; hi = 0; } else if (ki - 1 < hi) hi = ki - 1; }
                        } else { shape_ok = false; break; }
                    }
                    if (shape_ok && eq_col && rg_col && rg_col != eq_col) {
                        ray_idx_consults[IDX_SITE_FILTER_EQRANGE]++;
                        /* SYM eq: global intern id -> this column's domain
                         * (mirrors the single-eq consult above). */
                        int64_t probe = eq_key;
                        bool absent = false;
                        if (eq_col->type == RAY_SYM) {
                            ray_t* cs = ray_sym_str(eq_key);
                            int64_t dom_id = cs
                                ? ray_sym_domain_find(ray_sym_vec_domain(eq_col),
                                                      ray_str_ptr(cs),
                                                      ray_str_len(cs))
                                : -1;
                            if (dom_id < 0) absent = true;
                            else probe = dom_id;
                        }
                        const int64_t* grows = NULL;
                        int64_t gn = 0;
                        int hit = absent ? 0
                            : ray_index_hash_group(eq_col, probe, &grows, &gn);
                        if (hit == 0 || (hit > 0 && lo > hi)) {
                            /* provably empty: absent key or empty range */
                            int64_t nrows = ray_table_nrows(input);
                            ray_t* sel = ray_index_rowsel_from_ids(nrows, NULL, 0);
                            if (sel) {
                                ray_idx_hits[IDX_SITE_FILTER_EQRANGE]++;
                                g->selection = sel;
                                return input;
                            }
                        } else if (hit > 0) {
                            const void* rgd = ray_data(rg_col);
                            int8_t  rt = rg_col->type;
                            uint8_t ra = rg_col->attrs;
                            bool mono = true;
                            int64_t prevv = INT64_MIN;
                            for (int64_t pp = 0; pp < gn; pp++) {
                                int64_t v = read_col_i64(rgd, grows[pp], rt, ra);
                                if (v < prevv) { mono = false; break; }
                                prevv = v;
                            }
                            if (mono) {
                                int64_t a = 0, b = gn;   /* first pos >= lo */
                                while (a < b) {
                                    int64_t m = a + (b - a) / 2;
                                    if (read_col_i64(rgd, grows[m], rt, ra) < lo) a = m + 1;
                                    else b = m;
                                }
                                int64_t startp = a;
                                b = gn;                   /* first pos > hi */
                                while (a < b) {
                                    int64_t m = a + (b - a) / 2;
                                    if (read_col_i64(rgd, grows[m], rt, ra) <= hi) a = m + 1;
                                    else b = m;
                                }
                                int64_t nrows = ray_table_nrows(input);
                                ray_t* sel = ray_index_rowsel_from_ids(
                                    nrows, grows + startp, a - startp);
                                if (sel) {
                                    ray_idx_hits[IDX_SITE_FILTER_EQRANGE]++;
                                    g->selection = sel;
                                    return input;
                                }
                            }
                            /* non-monotone slice / OOM: fall through */
                        }
                        /* hit < 0 (stale/ineligible index): fall through */
                    }
                }

                /* 5. Hash-index IN probe: FILTER(IN(SCAN col, CONST set_vec))
                 * on an integer-family column with a hash index.  Probes the
                 * hash chain for each unique set element; builds one rowsel
                 * over the union of matches. */
                {
                    ray_t* in_col     = NULL;
                    ray_t* in_set_lit = NULL;
                    if (idx_filter_in_decode(g, op_child(g, op, 1), &in_col, &in_set_lit)) {
                        bool in_col_is_float = (in_col->type == RAY_F32 ||
                                               in_col->type == RAY_F64);
                        if (ray_index_kind(in_col) == RAY_IDX_HASH &&
                            !in_col_is_float) {
                            ray_idx_consults[IDX_SITE_IN]++;
                            ray_t* sel = ray_index_in_rowsel(in_col, in_set_lit);
                            if (sel) {
                                ray_idx_hits[IDX_SITE_IN]++;
                                g->selection = sel;
                                return input;
                            }
                            /* NULL: guard fired, ineligible, or OOM — fall through. */
                        }
                    }
                }

                /* 6. Fused predicate→selection: when the WHERE predicate is an
                 * expr-evaluable BOOL subtree, stream its per-morsel bools
                 * straight into a rowsel — skipping both the full BOOL vec and
                 * the second BOOL→rowsel scan.  Any unsupported shape (parted,
                 * >RAY_POOL_MAX_TASKS, OOM) returns NULL+all_pass=false,
                 * falling through to the bool path. */
                ray_op_t* pred_root = op_child(g, op, 1);
                {
                    int64_t fnrows = ray_table_nrows(input);
                    bool all_pass = false;
                    ray_t* fsel = exec_pred_to_selection(g, pred_root, fnrows,
                                                         &all_pass);
                    if (fsel || all_pass) {
                        g->selection = fsel;  /* NULL when all rows pass */
                        return input;
                    }
                    /* unsupported / OOM at runtime — fall through. */
                }
            }

            /* Sparse chained-filter refine: the optimizer splits compound
             * WHEREs selectivity-first, so a second FILTER arrives here
             * with the first filter's selection installed — and evaluating
             * its predicate over ALL rows to refine a few survivors wastes
             * O(rows).  Gather only the predicate's columns for the
             * surviving rows, evaluate the predicate over that dense
             * subset, and rebuild the selection from the passing
             * survivors — O(survivors).  Any ineligibility (non-scalar
             * predicate shape, OOM, unexpected result) falls through to
             * the full-width path unchanged. */
            if (input->type == RAY_TABLE && g->selection) {
                ray_rowsel_t* smeta = ray_rowsel_meta(g->selection);
                int64_t fr_nrows = ray_table_nrows(input);
                int64_t nsel = smeta ? smeta->total_pass : -1;
                if (nsel >= 0 && fr_nrows > 0 && nsel * 4 <= fr_nrows) {
                    ray_op_t* pred_root = op_child(g, op, 1);
                    int64_t keep[32];
                    bool has_expr = false;
                    uint32_t roots[1] = { pred_root->id };
                    int nkeep = collect_scan_syms(g, roots, 1, keep, 32,
                                                  &has_expr);
                    if (nkeep > 0) {
                        ray_t* sub = sel_compact(g, input, g->selection,
                                                 keep, nkeep);
                        if (sub && !RAY_IS_ERR(sub) && sub != input) {
                            ray_t* saved_tbl = g->table;
                            ray_t* saved_sel = g->selection;
                            g->table = sub;
                            g->selection = NULL;
                            ray_t* pv = exec_node(g, pred_root);
                            g->table = saved_tbl;
                            g->selection = saved_sel;
                            bool done_refine = false;
                            if (pv && !RAY_IS_ERR(pv) &&
                                pv->type == RAY_BOOL && pv->len == nsel) {
                                ray_t* ids_hdr = NULL;
                                int64_t* ids = (int64_t*)scratch_alloc(
                                    &ids_hdr,
                                    (size_t)(nsel > 0 ? nsel : 1) * 8);
                                if (ids) {
                                    const uint8_t*  fl  = ray_rowsel_flags(g->selection);
                                    const uint32_t* off = ray_rowsel_offsets(g->selection);
                                    const uint16_t* ix  = ray_rowsel_idx(g->selection);
                                    const uint8_t*  bv  = (const uint8_t*)ray_data(pv);
                                    uint32_t n_segs = smeta->n_segs;
                                    int64_t j = 0, kept = 0;
                                    for (uint32_t seg = 0; seg < n_segs; seg++) {
                                        uint8_t f = fl[seg];
                                        if (f == RAY_SEL_NONE) continue;
                                        int64_t ss = (int64_t)seg * RAY_MORSEL_ELEMS;
                                        int64_t se = ss + RAY_MORSEL_ELEMS;
                                        if (se > fr_nrows) se = fr_nrows;
                                        if (f == RAY_SEL_ALL) {
                                            for (int64_t r = ss; r < se; r++, j++)
                                                if (bv[j]) ids[kept++] = r;
                                        } else {
                                            const uint16_t* sl = ix + off[seg];
                                            uint32_t sn = off[seg + 1] - off[seg];
                                            for (uint32_t i2 = 0; i2 < sn; i2++, j++)
                                                if (bv[j]) ids[kept++] = ss + sl[i2];
                                        }
                                    }
                                    ray_t* nsel_blk = ray_index_rowsel_from_ids(
                                        fr_nrows, ids, kept);
                                    scratch_free(ids_hdr);
                                    if (nsel_blk) {
                                        ray_release(g->selection);
                                        g->selection = nsel_blk;
                                        done_refine = true;
                                    }
                                }
                            }
                            if (pv && !RAY_IS_ERR(pv)) ray_release(pv);
                            else if (pv) ray_error_free(pv);
                            ray_release(sub);
                            if (done_refine) return input;
                        } else if (sub && sub != input) {
                            ray_release(sub);
                        } else if (sub == input) {
                            ray_release(sub);   /* all-pass retain */
                        }
                    }
                }
            }

            ray_t* pred = exec_node(g, op_child(g, op, 1));
            if (!pred || RAY_IS_ERR(pred)) { ray_release(input); return pred; }

            /* Lazy filter: convert predicate to a rowsel (morsel-local
             * index list) and install on g->selection instead of
             * materializing a compacted table.  Only for TABLE inputs —
             * downstream ops (group-by) walk the rowsel directly,
             * boundary ops (sort/join/window) compact on demand via
             * sel_compact.  Vector inputs must still materialize
             * immediately since downstream ops like COUNT rely on
             * compacted length. */
            if (pred->type == RAY_BOOL && input->type == RAY_TABLE) {
                if (g->selection) {
                    /* Chained filter: refine the existing selection
                     * with this predicate in one walk. */
                    ray_t* merged = ray_rowsel_refine(g->selection, pred);
                    ray_release(pred);
                    ray_release(g->selection);
                    g->selection = merged;  /* may be NULL if all-pass */
                } else {
                    ray_t* new_sel = ray_rowsel_from_pred(pred);
                    ray_release(pred);
                    g->selection = new_sel;  /* may be NULL if all-pass */
                }
                return input;  /* original table, not compacted */
            }

            /* Eager filter for vector inputs and non-BOOL predicates */
            ray_t* result = exec_filter(g, op, input, pred);
            ray_release(input);
            ray_release(pred);
            return result;
        }

        case OP_SORT: {
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            ray_t* tbl = (input->type == RAY_TABLE) ? input : g->table;
            /* Compact lazy selection before sort (needs dense data) */
            if (g->selection && tbl && !RAY_IS_ERR(tbl) && tbl->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, tbl, g->selection, NULL, 0);
                if (input != g->table) ray_release(input);
                ray_release(g->selection);
                g->selection = NULL;
                input = compacted;
                tbl = compacted;
            }
            ray_t* result = exec_sort(g, op, tbl, 0);
            if (input != g->table) ray_release(input);
            return result;
        }

        case OP_GROUP: {
            ray_t* tbl = g->table;
            ray_t* owned_tbl = NULL;

            /* Factorized pipeline: detect OP_EXPAND (factorized) → OP_GROUP.
             * When the group key is _src and there's a factorized expand node
             * in the graph, execute the expand first and pipe its output as
             * the group input table.  This connects the expand→group pipeline
             * that would otherwise disconnect since GROUP reads g->table. */
            {
                ray_op_ext_t* gext = find_ext(g, op->id);
                if (gext && gext->n_keys == 1) {
                    ray_op_ext_t* kext = find_ext(g, gext->keys[0]);
                    int64_t src_sym = ray_sym_intern("_src", 4);
                    if (kext && kext->base.opcode == OP_SCAN && kext->sym == src_sym) {
                        /* Find the factorized OP_EXPAND connected to this GROUP.
                         * The expand must be the one whose output the GROUP
                         * is scanning (connected via OP_SCAN inputs). */
                        for (uint32_t ei = 0; ei < g->ext_count; ei++) {
                            ray_op_ext_t* ex = g->ext_nodes[ei];
                            if (ex && ex->base.id < g->node_count
                                && g->nodes[ex->base.id].opcode == OP_EXPAND
                                && ex->graph.factorized) {
                                ray_op_t* expand_op = &g->nodes[ex->base.id];
                                ray_t* expand_result = exec_node(g, expand_op);
                                if (!expand_result || RAY_IS_ERR(expand_result))
                                    return expand_result;
                                if (expand_result->type == RAY_TABLE) {
                                    ray_t* saved = g->table;
                                    g->table = expand_result;
                                    ray_t* result = exec_group(g, op, expand_result, 0);
                                    g->table = saved;
                                    ray_release(expand_result);
                                    return result;
                                }
                                ray_release(expand_result);
                                break;
                            }
                        }
                    }
                }
            }

            /* Pushed-down predicate (GROUP pushdown, opt.c): the optimizer
             * may interpose an OP_FILTER as inputs[0] (normally the first
             * key op).  Execute it first via the shared helper. */
            ray_op_t* group_in0 = op_child(g, op, 0);
            if (group_in0 && group_in0->opcode == OP_FILTER) {
                ray_t* res = exec_pushed_group_filter(g, group_in0);
                if (res && RAY_IS_ERR(res)) return res;
                if (res) { owned_tbl = res; tbl = res; }
                /* NULL → lazy path; g->selection installed, tbl unchanged */
            }

            /* Slice-group hint settle: an armed hint whose group shape is
             * ineligible for the slice kernel folds into the equivalent
             * selection HERE — before the sparse pre-compaction below —
             * so ineligible fused shapes still get compacted instead of
             * evaluating expression aggs over all rows. */
            if (g->sg_col) {
                ray_t* serr = ray_group_slice_hint_settle(g, op, tbl, 0);
                if (serr) {
                    if (owned_tbl) ray_release(owned_tbl);
                    return serr;
                }
            }

            /* Sparse-selection pre-compaction: when the WHERE selection
             * admits far fewer rows than the table AND some group input is
             * a computed expression, exec_group would still materialize the
             * expression over ALL rows (expr_eval_full) and only then skip
             * non-selected entries.  Gather the referenced columns once
             * (sel_compact keep-set) and group the dense survivors with no
             * selection instead.  Plain-scan-only groups stay on the lazy
             * path — their selection-aware loops are already optimal. */
            if (g->selection && tbl && tbl->type == RAY_TABLE) {
                ray_op_ext_t* gx = find_ext(g, op->id);
                ray_rowsel_t* meta = ray_rowsel_meta(g->selection);
                int64_t nrows = ray_table_nrows(tbl);
                /* Contiguity-aware admission: when most surviving rows sit
                 * in ALL-flagged rowsel segments (contiguous runs — the
                 * parted-layout shape), sel_compact's gather is nearly
                 * sequential memcpy, so compaction pays off at much higher
                 * densities than the scattered-row case. */
                int64_t all_rows = 0;
                if (meta) {
                    const uint8_t* fl = ray_rowsel_flags(g->selection);
                    for (uint32_t sg = 0; sg < meta->n_segs; sg++)
                        if (fl[sg] == RAY_SEL_ALL) all_rows += RAY_MORSEL_ELEMS;
                }
                bool contig = meta && meta->total_pass > 0 &&
                              all_rows * 4 >= meta->total_pass * 3;
                /* Admission: compaction only ever fires when an expression
                 * agg input exists (has_expr below) — the alternative is
                 * materializing that expression over ALL rows — so the
                 * scattered gate matches the projection-side compaction
                 * factor (4x), not the plain-gather tradeoff (16x). */
                if (gx && meta && nrows > 0 &&
                    meta->total_pass * (contig ? 2 : 4) <= nrows) {
                    int64_t keep[32];
                    bool has_expr = false;
                    int nkeep = group_input_scan_syms(g, gx, keep, 32, &has_expr);
                    if (nkeep > 0 && has_expr) {
                        ray_t* compacted = sel_compact(g, tbl, g->selection,
                                                       keep, nkeep);
                        if (compacted && !RAY_IS_ERR(compacted) &&
                            compacted != tbl) {
                            if (owned_tbl) ray_release(owned_tbl);
                            owned_tbl = compacted;
                            tbl = compacted;
                            ray_release(g->selection);
                            g->selection = NULL;
                        } else if (compacted) {
                            /* all-pass / alloc-fallback returned tbl retained,
                             * or an error — drop it and keep the lazy path. */
                            ray_release(compacted);
                        }
                    }
                }
            }

            /* Lazy selection is consumed by exec_group itself — all
             * paths (sequential, DA, radix-parallel) honour the
             * bitmap via group_rows_range / radix scan loops.  We
             * must still clear g->selection *after* group runs so
             * downstream ops (SORT etc.) don't try to sel_compact the
             * aggregated output with a mismatched-length bitmap. */
            ray_t* result = exec_group(g, op, tbl, 0);
            if (owned_tbl) ray_release(owned_tbl);
            if (g->selection) {
                ray_release(g->selection);
                g->selection = NULL;
            }
            return result;
        }

        case OP_PIVOT: {
            ray_t* tbl = g->table;
            ray_t* owned_tbl = NULL;
            if (g->selection) {
                ray_t* compacted = sel_compact(g, tbl, g->selection, NULL, 0);
                if (!compacted || RAY_IS_ERR(compacted)) return compacted;
                ray_release(g->selection);
                g->selection = NULL;
                owned_tbl = compacted;
                tbl = compacted;
            }
            ray_t* result = exec_pivot(g, op, tbl);
            if (owned_tbl) ray_release(owned_tbl);
            return result;
        }

        case OP_JOIN: {
            ray_t* left = exec_node(g, op_child(g, op, 0));
            ray_t* right = exec_node(g, op_child(g, op, 1));
            if (!left || RAY_IS_ERR(left)) { if (right && !RAY_IS_ERR(right)) ray_release(right); return left; }
            if (!right || RAY_IS_ERR(right)) { ray_release(left); return right; }
            /* Compact lazy selection before join (needs dense data) */
            if (g->selection && left && !RAY_IS_ERR(left) && left->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, left, g->selection, NULL, 0);
                ray_release(left);
                ray_release(g->selection);
                g->selection = NULL;
                left = compacted;
            }
            ray_t* result = exec_join(g, op, left, right);
            ray_release(left);
            ray_release(right);
            return result;
        }

        case OP_ANTIJOIN: {
            ray_t* left = exec_node(g, op_child(g, op, 0));
            ray_t* right = exec_node(g, op_child(g, op, 1));
            if (!left || RAY_IS_ERR(left)) { if (right && !RAY_IS_ERR(right)) ray_release(right); return left; }
            if (!right || RAY_IS_ERR(right)) { ray_release(left); return right; }
            if (g->selection && left && !RAY_IS_ERR(left) && left->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, left, g->selection, NULL, 0);
                ray_release(left);
                ray_release(g->selection);
                g->selection = NULL;
                left = compacted;
            }
            ray_t* result = exec_antijoin(g, op, left, right);
            ray_release(left);
            ray_release(right);
            return result;
        }

        case OP_WINDOW_JOIN: {
            ray_t* left = exec_node(g, op_child(g, op, 0));
            ray_t* right = exec_node(g, op_child(g, op, 1));
            if (!left || RAY_IS_ERR(left)) { if (right && !RAY_IS_ERR(right)) ray_release(right); return left; }
            if (!right || RAY_IS_ERR(right)) { ray_release(left); return right; }
            if (g->selection && left && !RAY_IS_ERR(left) && left->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, left, g->selection, NULL, 0);
                ray_release(left);
                ray_release(g->selection);
                g->selection = NULL;
                left = compacted;
            }
            ray_t* result = exec_window_join(g, op, left, right);
            ray_release(left);
            ray_release(right);
            return result;
        }

        case OP_WINDOW: {
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            ray_t* wdf = (input->type == RAY_TABLE) ? input : g->table;
            /* Compact lazy selection before window (needs dense data) */
            if (g->selection && wdf && !RAY_IS_ERR(wdf) && wdf->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, wdf, g->selection, NULL, 0);
                if (input != g->table) ray_release(input);
                ray_release(g->selection);
                g->selection = NULL;
                input = compacted;
                wdf = compacted;
            }
            ray_t* result = exec_window(g, op, wdf);
            if (input != g->table) ray_release(input);
            return result;
        }

        case OP_HEAD: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            int64_t n = ext ? ext->sym : 10;

            /* Fused sort+limit: detect SORT child → only gather N rows */
            ray_op_t* child_op = op_child(g, op, 0);
            if (child_op && child_op->opcode == OP_SORT) {
                ray_t* sort_input = exec_node(g, op_child(g, child_op, 0));
                if (!sort_input || RAY_IS_ERR(sort_input)) return sort_input;
                ray_t* tbl = (sort_input->type == RAY_TABLE) ? sort_input : g->table;
                /* Compact lazy selection before sort */
                if (g->selection && tbl && !RAY_IS_ERR(tbl) && tbl->type == RAY_TABLE) {
                    ray_t* compacted = sel_compact(g, tbl, g->selection, NULL, 0);
                    if (sort_input != g->table) ray_release(sort_input);
                    ray_release(g->selection);
                    g->selection = NULL;
                    sort_input = compacted;
                    tbl = compacted;
                }
                ray_t* result = exec_sort(g, child_op, tbl, n);
                if (sort_input != g->table) ray_release(sort_input);
                /* Top-level statement GC catches intermediates. */
                return result;
            }

            /* HEAD(GROUP) optimization: pass limit hint to exec_group
             * so it can short-circuit the per-partition loop when all
             * GROUP BY keys are MAPCOMMON.  The normal HEAD logic below
             * still trims the result to N rows regardless.
             *
             * After GROUP predicate pushdown (opt.c Task-3) the shape
             * becomes HEAD(GROUP(FILTER(...))).  The fast-path must
             * execute the interposed filter before calling exec_group,
             * mirroring the Task-2 hook in the generic OP_GROUP dispatch. */
            ray_t* input;
            if (child_op && child_op->opcode == OP_GROUP) {
                ray_t* tbl = g->table;
                if (!tbl || RAY_IS_ERR(tbl)) return tbl;
                ray_t* owned_tbl = NULL;
                /* Pushed-down filter: execute it first via the shared helper. */
                ray_op_t* child_in0 = op_child(g, child_op, 0);
                if (child_in0 && child_in0->opcode == OP_FILTER) {
                    ray_t* res = exec_pushed_group_filter(g, child_in0);
                    if (res && RAY_IS_ERR(res)) return res;
                    if (res) { owned_tbl = res; tbl = res; }
                    /* NULL → lazy path; g->selection installed, tbl unchanged */
                } else if (g->selection && tbl->type == RAY_TABLE) {
                    int needs = 0;
                    int64_t nc = ray_table_ncols(tbl);
                    for (int64_t c = 0; c < nc; c++) {
                        ray_t* col = ray_table_get_col_idx(tbl, c);
                        if (col && !RAY_IS_PARTED(col->type)
                            && col->type != RAY_MAPCOMMON) {
                            needs = 1; break;
                        }
                    }
                    if (needs) {
                        ray_t* compacted = sel_compact(g, tbl, g->selection, NULL, 0);
                        if (!compacted || RAY_IS_ERR(compacted)) return compacted;
                        ray_release(g->selection);
                        g->selection = NULL;
                        owned_tbl = compacted;
                        tbl = compacted;
                    }
                }
                input = exec_group(g, child_op, tbl, n);
                if (owned_tbl) ray_release(owned_tbl);
                if (g->selection) {
                    ray_release(g->selection);
                    g->selection = NULL;
                }
            } else if (child_op && child_op->opcode == OP_FILTER) {
                /* HEAD(FILTER): early-termination filter — gather only
                 * the first N matching rows instead of all matches. */
                ray_t* filter_input = exec_node(g, op_child(g, child_op, 0));
                if (!filter_input || RAY_IS_ERR(filter_input))
                    return filter_input;

                /* Compact lazy selection before filter evaluation */
                ray_t* ftbl = (filter_input->type == RAY_TABLE)
                           ? filter_input : g->table;
                if (g->selection && ftbl && ftbl->type == RAY_TABLE) {
                    ray_t* compacted = sel_compact(g, ftbl, g->selection, NULL, 0);
                    if (filter_input != g->table) ray_release(filter_input);
                    ray_release(g->selection);
                    g->selection = NULL;
                    filter_input = compacted;
                    ftbl = compacted;
                }

                /* Swap table for predicate evaluation */
                ray_t* saved_table = g->table;
                g->table = ftbl;
                ray_t* pred = exec_node(g, op_child(g, child_op, 1));
                g->table = saved_table;

                if (!pred || RAY_IS_ERR(pred)) {
                    if (filter_input != saved_table)
                        ray_release(filter_input);
                    return pred;
                }

                ray_t* result = exec_filter_head(ftbl, pred, n);
                ray_release(pred);
                if (filter_input != saved_table)
                    ray_release(filter_input);
                /* Top-level statement GC catches intermediates. */
                return result;
            } else {
                input = exec_node(g, op_child(g, op, 0));
            }
            if (!input || RAY_IS_ERR(input)) return input;
            if (input->type == RAY_TABLE) {
                int64_t ncols = ray_table_ncols(input);
                int64_t nrows = ray_table_nrows(input);
                if (n > nrows) n = nrows;
                ray_t* result = ray_table_new(ncols);
                for (int64_t c = 0; c < ncols; c++) {
                    ray_t* col = ray_table_get_col_idx(input, c);
                    int64_t name_id = ray_table_col_name(input, c);
                    if (!col) continue;
                    if (col->type == RAY_MAPCOMMON) {
                        ray_t* mc_head = materialize_mapcommon_head(col, n);
                        if (mc_head && !RAY_IS_ERR(mc_head)) {
                            result = ray_table_add_col(result, name_id, mc_head);
                            ray_release(mc_head);
                        }
                        continue;
                    }
                    if (RAY_IS_PARTED(col->type)) {
                        /* Copy first n rows from parted segments */
                        int8_t base = (int8_t)RAY_PARTED_BASETYPE(col->type);
                        ray_t** sp = (ray_t**)ray_data(col);
                        ray_t* head_vec;
                        if (base == RAY_STR) {
                            head_vec = parted_head_str(sp, col->len, n);
                        } else {
                            uint8_t ba = (base == RAY_SYM)
                                       ? parted_sym_max_attrs(sp, col->len) : 0;
                            head_vec = typed_vec_new(base, ba, n);
                            if (head_vec && !RAY_IS_ERR(head_vec)) {
                                head_vec->len = n;
                                ray_t** segs = (ray_t**)ray_data(col);
                                int64_t remaining = n;
                                int64_t dst_off = 0;
                                uint8_t seg_nulls = 0;
                                for (int64_t s = 0; s < col->len && remaining > 0; s++) {
                                    if (!segs[s]) continue;
                                    int64_t take = segs[s]->len;
                                    if (take > remaining) take = remaining;
                                    parted_copy_cells(ray_data(head_vec), base,
                                                      ba, dst_off, segs[s], 0,
                                                      take);
                                    seg_nulls |= segs[s]->attrs & RAY_ATTR_HAS_NULLS;
                                    dst_off += take;
                                    remaining -= take;
                                }
                                /* Invariant 16.4: conservatively propagate
                                 * HAS_NULLS from the touched segments. */
                                head_vec->attrs |= seg_nulls;
                                /* raw SYM cell-id copy from the segments —
                                 * adopt the partitions' shared domain */
                                if (head_vec->type == RAY_SYM)
                                    ray_sym_vec_adopt_domain(head_vec,
                                                             sym_domain_rep(col));
                            }
                        }
                        result = ray_table_add_col(result, name_id, head_vec);
                        ray_release(head_vec);
                    } else {
                        /* Flat column: direct copy */
                        uint8_t esz = col_esz(col);
                        ray_t* head_vec = col_vec_new(col, n);
                        if (head_vec && !RAY_IS_ERR(head_vec)) {
                            head_vec->len = n;
                            memcpy(ray_data(head_vec), ray_data(col),
                                   (size_t)n * esz);
                            col_propagate_nulls_range(head_vec, 0, col, 0, n);
                            /* raw SYM cell-id copy — adopt the source's
                             * dictionary (sym-domain Phase 2) */
                            if (col->type == RAY_SYM)
                                ray_sym_vec_adopt_domain(head_vec, col);
                        }
                        result = ray_table_add_col(result, name_id, head_vec);
                        ray_release(head_vec);
                    }
                }
                ray_release(input);
                return result;
            }
            if (n > input->len) n = input->len;
            /* Materialized copy for vector head */
            uint8_t esz = col_esz(input);
            ray_t* result = col_vec_new(input, n);
            if (result && !RAY_IS_ERR(result)) {
                result->len = n;
                memcpy(ray_data(result), ray_data(input), (size_t)n * esz);
                col_propagate_nulls_range(result, 0, input, 0, n);
                if (input->type == RAY_SYM)
                    ray_sym_vec_adopt_domain(result, input);
            }
            ray_release(input);
            return result;
        }

        case OP_TAIL: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            int64_t n = ext ? ext->sym : 10;
            if (input->type == RAY_TABLE) {
                int64_t ncols = ray_table_ncols(input);
                int64_t nrows = ray_table_nrows(input);
                if (n > nrows) n = nrows;
                int64_t skip = nrows - n;
                ray_t* result = ray_table_new(ncols);
                for (int64_t c = 0; c < ncols; c++) {
                    ray_t* col = ray_table_get_col_idx(input, c);
                    int64_t name_id = ray_table_col_name(input, c);
                    if (!col) continue;
                    if (col->type == RAY_MAPCOMMON) {
                        /* Materialize last N rows from MAPCOMMON partitions */
                        ray_t** mc_ptrs = (ray_t**)ray_data(col);
                        ray_t* kv = mc_ptrs[0];
                        ray_t* rc = mc_ptrs[1];
                        int64_t n_parts = kv->len;
                        size_t esz = (size_t)col_esz(kv);
                        const char* kdata = (const char*)ray_data(kv);
                        const int64_t* counts = (const int64_t*)ray_data(rc);
                        ray_t* flat = col_vec_new(kv, n);
                        if (flat && !RAY_IS_ERR(flat)) {
                            flat->len = n;
                            /* raw SYM keys of the MAPCOMMON carrier */
                            if (kv->type == RAY_SYM)
                                ray_sym_vec_adopt_domain(flat, kv);
                            char* out = (char*)ray_data(flat);
                            /* Walk partitions from end, fill output from end */
                            int64_t remaining = n;
                            int64_t dst = n;
                            for (int64_t p = n_parts - 1; p >= 0 && remaining > 0; p--) {
                                int64_t take = counts[p];
                                if (take > remaining) take = remaining;
                                dst -= take;
                                for (int64_t r = 0; r < take; r++)
                                    memcpy(out + (dst + r) * esz, kdata + (size_t)p * esz, esz);
                                remaining -= take;
                            }
                        }
                        result = ray_table_add_col(result, name_id, flat);
                        ray_release(flat);
                        continue;
                    }
                    if (RAY_IS_PARTED(col->type)) {
                        /* Copy last N rows from parted segments */
                        int8_t base = (int8_t)RAY_PARTED_BASETYPE(col->type);
                        ray_t** tsp = (ray_t**)ray_data(col);
                        ray_t* tail_vec;
                        if (base == RAY_STR) {
                            tail_vec = parted_tail_str(tsp, col->len, n);
                        } else {
                            uint8_t tba = (base == RAY_SYM)
                                        ? parted_sym_max_attrs(tsp, col->len) : 0;
                            tail_vec = typed_vec_new(base, tba, n);
                            if (tail_vec && !RAY_IS_ERR(tail_vec)) {
                                tail_vec->len = n;
                                ray_t** segs = (ray_t**)ray_data(col);
                                int64_t remaining = n;
                                int64_t dst = n;
                                uint8_t seg_nulls = 0;
                                for (int64_t s = col->len - 1; s >= 0 && remaining > 0; s--) {
                                    if (!segs[s]) continue;
                                    int64_t take = segs[s]->len;
                                    if (take > remaining) take = remaining;
                                    dst -= take;
                                    parted_copy_cells(ray_data(tail_vec), base,
                                                      tba, dst, segs[s],
                                                      segs[s]->len - take, take);
                                    seg_nulls |= segs[s]->attrs & RAY_ATTR_HAS_NULLS;
                                    remaining -= take;
                                }
                                /* Invariant 16.4: conservatively propagate
                                 * HAS_NULLS from the touched segments. */
                                tail_vec->attrs |= seg_nulls;
                                /* raw SYM cell-id copy from the segments —
                                 * adopt the partitions' shared domain */
                                if (tail_vec->type == RAY_SYM)
                                    ray_sym_vec_adopt_domain(tail_vec,
                                                             sym_domain_rep(col));
                            }
                        }
                        result = ray_table_add_col(result, name_id, tail_vec);
                        ray_release(tail_vec);
                    } else {
                        /* Flat column: direct copy */
                        uint8_t esz = col_esz(col);
                        ray_t* tail_vec = col_vec_new(col, n);
                        if (tail_vec && !RAY_IS_ERR(tail_vec)) {
                            tail_vec->len = n;
                            memcpy(ray_data(tail_vec),
                                   (char*)ray_data(col) + (size_t)skip * esz,
                                   (size_t)n * esz);
                            col_propagate_nulls_range(tail_vec, 0, col, skip, n);
                            /* raw SYM cell-id copy — adopt source dict */
                            if (col->type == RAY_SYM)
                                ray_sym_vec_adopt_domain(tail_vec, col);
                        }
                        result = ray_table_add_col(result, name_id, tail_vec);
                        ray_release(tail_vec);
                    }
                }
                ray_release(input);
                return result;
            }
            if (n > input->len) n = input->len;
            int64_t skip = input->len - n;
            uint8_t esz = col_esz(input);
            ray_t* result = col_vec_new(input, n);
            if (result && !RAY_IS_ERR(result)) {
                result->len = n;
                memcpy(ray_data(result),
                       (char*)ray_data(input) + (size_t)skip * esz,
                       (size_t)n * esz);
                col_propagate_nulls_range(result, 0, input, skip, n);
                if (input->type == RAY_SYM)
                    ray_sym_vec_adopt_domain(result, input);
            }
            ray_release(input);
            return result;
        }

        case OP_IF: {
            return exec_if(g, op);
        }

        case OP_LIKE: {
            return exec_like(g, op);
        }

        case OP_ILIKE: {
            return exec_ilike(g, op);
        }

        case OP_UPPER: case OP_LOWER: case OP_TRIM: {
            return exec_string_unary(g, op);
        }
        case OP_STRLEN: {
            return exec_strlen(g, op);
        }
        case OP_SUBSTR: {
            return exec_substr(g, op);
        }
        case OP_REPLACE: {
            return exec_replace(g, op);
        }
        case OP_CONCAT: {
            return exec_concat(g, op);
        }

        case OP_EXTRACT: {
            return exec_extract(g, op);
        }

        case OP_DATE_TRUNC: {
            return exec_date_trunc(g, op);
        }

        case OP_ALIAS: {
            return exec_node(g, op_child(g, op, 0));
        }

        case OP_MATERIALIZE: {
            return exec_node(g, op_child(g, op, 0));
        }

        case OP_SELECT: {
            /* Column projection: select/compute columns from input table */
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            if (input->type != RAY_TABLE) {
                ray_release(input);
                return ray_error("nyi", NULL);
            }
            ray_op_ext_t* ext = find_ext(g, op->id);
            if (!ext) { ray_release(input); return ray_error("nyi", NULL); }
            uint32_t n_cols = ext->sort.n_cols;
            uint32_t* columns = ext->sort.columns;

            /* Sparse-selection pre-compaction (mirrors OP_GROUP): expression
             * columns would otherwise be evaluated over ALL rows, with the
             * selection applied only when ray_execute_inner gathers the full-
             * length result.  Gather the referenced input columns once and
             * project the dense survivors instead — the final gather then
             * disappears (the selection is consumed here).  Plain-scan-only
             * projections stay lazy: they share column refs and the single
             * final gather is already optimal. */
            if (g->selection && n_cols > 0) {
                ray_rowsel_t* smeta = ray_rowsel_meta(g->selection);
                int64_t in_rows = ray_table_nrows(input);
                if (smeta && in_rows > 0 && smeta->total_pass * 4 <= in_rows) {
                    int64_t keep[32];
                    bool has_expr = false;
                    int nkeep = collect_scan_syms(g, columns, n_cols,
                                                  keep, 32, &has_expr);
                    if (nkeep > 0 && has_expr) {
                        ray_t* compacted = sel_compact(g, input, g->selection,
                                                       keep, nkeep);
                        if (compacted && !RAY_IS_ERR(compacted) &&
                            compacted != input) {
                            ray_release(input);
                            input = compacted;
                            ray_release(g->selection);
                            g->selection = NULL;
                        } else if (compacted) {
                            /* all-pass / alloc-fallback returned input
                             * retained, or an error — drop it and keep the
                             * lazy final-gather path. */
                            ray_release(compacted);
                        }
                    }
                }
            }

            ray_t* result = ray_table_new(n_cols);

            /* Set g->table so SCAN nodes inside expressions resolve correctly */
            ray_t* saved_table = g->table;
            g->table = input;

            for (uint32_t c = 0; c < n_cols; c++) {
                ray_op_t* col_op = op_node(g, columns[c]);
                if (col_op->opcode == OP_SCAN) {
                    /* Direct column reference — copy from input table */
                    ray_op_ext_t* col_ext = find_ext(g, columns[c]);
                    if (!col_ext) continue;
                    int64_t name_id = col_ext->sym;
                    ray_t* src_col = ray_table_get_col(input, name_id);
                    if (src_col) {
                        ray_retain(src_col);
                        result = ray_table_add_col(result, name_id, src_col);
                        ray_release(src_col);
                    }
                } else {
                    /* Expression column — evaluate against input table */
                    ray_t* vec = exec_node(g, col_op);
                    if (!vec || RAY_IS_ERR(vec)) {
                        ray_release(result);
                        g->table = saved_table;
                        ray_release(input);
                        return vec ? vec : ray_error("nyi", NULL);
                    }
                    /* Broadcast scalar atoms to full column vectors */
                    if (vec->type < 0) {
                        int64_t nr = ray_table_nrows(input);
                        ray_t* col = broadcast_scalar(vec, nr);
                        ray_release(vec);
                        vec = col;
                        if (!vec || RAY_IS_ERR(vec)) {
                            ray_release(result);
                            g->table = saved_table;
                            ray_release(input);
                            return vec ? vec : ray_error("nyi", NULL);
                        }
                    }
                    /* Synthetic name: _expr_0, _expr_1, ... */
                    char name_buf[16];
                    int n = 0;
                    name_buf[n++] = '_'; name_buf[n++] = 'e';
                    if (c >= 100) name_buf[n++] = '0' + (c / 100);
                    if (c >= 10)  name_buf[n++] = '0' + ((c / 10) % 10);
                    name_buf[n++] = '0' + (c % 10);
                    int64_t name_id = ray_sym_intern(name_buf, (size_t)n);
                    result = ray_table_add_col(result, name_id, vec);
                    ray_release(vec);
                }
            }

            g->table = saved_table;
            ray_release(input);
            return result;
        }

        case OP_EXPAND: {
            ray_t* src = exec_node(g, op_child(g, op, 0));
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* result = exec_expand(g, op, src);
            ray_release(src);
            return result;
        }

        case OP_VAR_EXPAND: {
            ray_t* start = exec_node(g, op_child(g, op, 0));
            if (!start || RAY_IS_ERR(start)) return start;
            ray_t* result = exec_var_expand(g, op, start);
            ray_release(start);
            return result;
        }

        case OP_SHORTEST_PATH: {
            ray_t* src = exec_node(g, op_child(g, op, 0));
            ray_t* dst = exec_node(g, op_child(g, op, 1));
            if (!src || RAY_IS_ERR(src)) {
                if (dst && !RAY_IS_ERR(dst)) ray_release(dst);
                return src;
            }
            if (!dst || RAY_IS_ERR(dst)) { ray_release(src); return dst; }
            ray_t* result = exec_shortest_path(g, op, src, dst);
            ray_release(src);
            ray_release(dst);
            return result;
        }

        case OP_WCO_JOIN: {
            return exec_wco_join(g, op);
        }

        case OP_PAGERANK: {
            return exec_pagerank(g, op);
        }

        case OP_CONNECTED_COMP: {
            return exec_connected_comp(g, op);
        }

        case OP_DIJKSTRA: {
            ray_t* src = exec_node(g, op_child(g, op, 0));
            if (!src || RAY_IS_ERR(src)) return src;
            ray_op_t* dij_in1 = op_child(g, op, 1);
            ray_t* dst = dij_in1 ? exec_node(g, dij_in1) : NULL;
            if (dst && RAY_IS_ERR(dst)) { ray_release(src); return dst; }
            ray_t* result = exec_dijkstra(g, op, src, dst);
            ray_release(src);
            if (dst) ray_release(dst);
            return result;
        }

        case OP_LOUVAIN: {
            return exec_louvain(g, op);
        }

        case OP_DEGREE_CENT: {
            return exec_degree_cent(g, op);
        }

        case OP_TOPSORT: {
            return exec_topsort(g, op);
        }

        case OP_DFS: {
            ray_t* src = exec_node(g, op_child(g, op, 0));
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* result = exec_dfs(g, op, src);
            ray_release(src);
            return result;
        }

        case OP_CLUSTER_COEFF: {
            return exec_cluster_coeff(g, op);
        }

        case OP_BETWEENNESS: {
            return exec_betweenness(g, op);
        }

        case OP_CLOSENESS: {
            return exec_closeness(g, op);
        }

        case OP_MST: {
            return exec_mst(g, op);
        }

        case OP_RANDOM_WALK: {
            ray_t* src = exec_node(g, op_child(g, op, 0));
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* result = exec_random_walk(g, op, src);
            ray_release(src);
            return result;
        }

        case OP_ASTAR: {
            ray_t* src = exec_node(g, op_child(g, op, 0));
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* dst = exec_node(g, op_child(g, op, 1));
            if (!dst || RAY_IS_ERR(dst)) { ray_release(src); return dst; }
            ray_t* result = exec_astar(g, op, src, dst);
            ray_release(src); ray_release(dst);
            return result;
        }

        case OP_K_SHORTEST: {
            ray_t* src = exec_node(g, op_child(g, op, 0));
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* dst = exec_node(g, op_child(g, op, 1));
            if (!dst || RAY_IS_ERR(dst)) { ray_release(src); return dst; }
            ray_t* result = exec_k_shortest(g, op, src, dst);
            ray_release(src); ray_release(dst);
            return result;
        }

        case OP_ANN_RERANK: {
            ray_t* src = exec_node(g, op_child(g, op, 0));
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* result = exec_ann_rerank(g, op, src);
            ray_release(src);
            return result;
        }
        case OP_KNN_RERANK: {
            ray_t* src = exec_node(g, op_child(g, op, 0));
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* result = exec_knn_rerank(g, op, src);
            ray_release(src);
            return result;
        }

        default:
            return ray_error("nyi", NULL);
    }
}

/* ============================================================================
 * ray_execute -- top-level entry point (lazy pool init)
 * ============================================================================ */

/* Merge two partial results from partition-streamed execution.
 * Concatenates table columns or vectors across segments. */
ray_t* ray_result_merge(ray_t* accum, ray_t* partial) {
    if (!accum || RAY_IS_ERR(accum)) {
        if (partial && !RAY_IS_ERR(partial)) ray_retain(partial);
        return partial;
    }
    if (!partial || RAY_IS_ERR(partial)) {
        ray_retain(accum);
        return accum;
    }

    /* Table merge: concatenate each column */
    if (accum->type == RAY_TABLE && partial->type == RAY_TABLE) {
        int64_t ncols = ray_table_ncols(accum);
        ray_t* merged = ray_table_new(ncols);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t name_id = ray_table_col_name(accum, c);
            ray_t* a_col = ray_table_get_col_idx(accum, c);
            ray_t* p_col = ray_table_get_col_idx(partial, c);
            if (!a_col || !p_col) {
                ray_release(merged);
                return ray_error("schema", NULL);
            }
            ray_t* combined = ray_vec_concat(a_col, p_col);
            if (!combined || RAY_IS_ERR(combined)) {
                ray_release(merged);
                return combined;
            }
            merged = ray_table_add_col(merged, name_id, combined);
            ray_release(combined);
        }
        return merged;
    }

    /* Vector merge: concatenate directly */
    if (accum->type != RAY_TABLE && partial->type != RAY_TABLE) {
        return ray_vec_concat(accum, partial);
    }

    return ray_error("type", "result merge: cannot combine mismatched partial results, got %s and %s",
                     ray_type_name(accum->type), ray_type_name(partial->type));
}

/* Build a flat table containing one segment's columns from a parted table.
 * For each parted column, extracts segs[seg_idx] as a flat vector.
 * MAPCOMMON columns are materialized for segment seg_idx: the partition key
 * value is broadcast to fill seg_rows elements.
 * Non-parted columns are retained as-is. */
ray_t* build_segment_table(ray_t* parted_tbl, int32_t seg_idx) {
    int64_t ncols = ray_table_ncols(parted_tbl);
    ray_t* seg_tbl = ray_table_new(ncols);
    if (!seg_tbl || RAY_IS_ERR(seg_tbl)) return seg_tbl;

    /* Find segment row count from first parted column */
    int64_t seg_rows = 0;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(parted_tbl, c);
        if (col && RAY_IS_PARTED(col->type)) {
            ray_t** segs = (ray_t**)ray_data(col);
            if (seg_idx < col->len && segs[seg_idx])
                seg_rows = segs[seg_idx]->len;
            break;
        }
    }

    for (int64_t c = 0; c < ncols; c++) {
        int64_t name_id = ray_table_col_name(parted_tbl, c);
        ray_t* col = ray_table_get_col_idx(parted_tbl, c);
        if (!col) continue;

        if (col->type == RAY_MAPCOMMON) {
            /* Materialize partition key for this segment: broadcast key
             * value across seg_rows elements. */
            if (col->len < 2) {
                ray_release(seg_tbl);
                return ray_error("schema", NULL);
            }
            ray_t** mc_ptrs = (ray_t**)ray_data(col);
            ray_t* kv = mc_ptrs[0];  /* key_values */
            if (!kv || seg_idx >= kv->len) {
                ray_release(seg_tbl);
                return ray_error("schema", NULL);
            }
            int8_t kv_type = kv->type;
            size_t esz = (size_t)ray_sym_elem_size(kv_type, kv->attrs);
            if (esz == 0) {
                ray_release(seg_tbl);
                return ray_error("type", "segment build: partition key has no element size, got %s", ray_type_name(kv_type));
            }
            ray_t* flat = ray_vec_new(kv_type, seg_rows);
            if (!flat || RAY_IS_ERR(flat)) {
                ray_release(seg_tbl);
                return ray_error("oom", NULL);
            }
            flat->len = seg_rows;
            const char* src = (const char*)ray_data(kv) + (size_t)seg_idx * esz;
            char* dst = (char*)ray_data(flat);
            if (esz == 8) {
                uint64_t v; memcpy(&v, src, 8);
                for (int64_t r = 0; r < seg_rows; r++)
                    ((uint64_t*)dst)[r] = v;
            } else if (esz == 4) {
                uint32_t v; memcpy(&v, src, 4);
                for (int64_t r = 0; r < seg_rows; r++)
                    ((uint32_t*)dst)[r] = v;
            } else {
                for (int64_t r = 0; r < seg_rows; r++)
                    memcpy(dst + r * esz, src, esz);
            }
            /* raw SYM key id broadcast — adopt the keys carrier's domain */
            if (flat->type == RAY_SYM) ray_sym_vec_adopt_domain(flat, kv);
            seg_tbl = ray_table_add_col(seg_tbl, name_id, flat);
            ray_release(flat);
        } else if (RAY_IS_PARTED(col->type)) {
            ray_t** segs = (ray_t**)ray_data(col);
            if (seg_idx >= col->len || !segs[seg_idx]) {
                ray_release(seg_tbl);
                return ray_error("schema", NULL);
            }
            ray_retain(segs[seg_idx]);
            seg_tbl = ray_table_add_col(seg_tbl, name_id, segs[seg_idx]);
            ray_release(segs[seg_idx]);
        } else {
            /* Non-parted, non-MAPCOMMON column in a parted table:
             * streaming should have been rejected by ray_execute().
             * Error here as defense-in-depth to avoid silent duplication. */
            ray_release(seg_tbl);
            return ray_error("schema", NULL);
        }
    }
    return seg_tbl;
}

/* Is this opcode safe for segment streaming with concatenation merge?
 * Only element-wise, scan, filter, project, and alias ops produce
 * results that can be correctly concatenated across segments.
 * Everything else (joins, aggregations, sorts, graph ops, etc.)
 * requires specialized merge or global state. */
static bool op_streamable(uint16_t opc) {
    switch (opc) {
        /* Data access (OP_CONST excluded: vector constants have total-row
         * length and produce length mismatches with per-segment data.
         * Scalar constants are checked separately in dag_can_stream.) */
        case OP_SCAN:
        /* Element-wise unary */
        case OP_NEG: case OP_ABS: case OP_NOT: case OP_SQRT:
        case OP_LOG: case OP_EXP: case OP_CEIL: case OP_FLOOR: case OP_ROUND:
        case OP_ISNULL: case OP_CAST:
        /* Element-wise binary */
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_IDIV: case OP_MOD:
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
        case OP_GT: case OP_GE: case OP_AND: case OP_OR:
        case OP_MIN2: case OP_MAX2: case OP_IF: case OP_IN: case OP_NOT_IN:
        /* String element-wise */
        case OP_LIKE: case OP_ILIKE: case OP_UPPER: case OP_LOWER:
        case OP_STRLEN: case OP_SUBSTR: case OP_REPLACE: case OP_TRIM:
        case OP_CONCAT:
        /* Temporal element-wise */
        case OP_EXTRACT: case OP_DATE_TRUNC:
        /* Structure */
        case OP_FILTER: case OP_SELECT: case OP_ALIAS:
        case OP_MATERIALIZE:
            return true;
        default:
            return false;
    }
}

/* Walk the root's input subtree to check if it reaches a default-table
 * OP_SCAN.  Returns true if found, false otherwise.  Also rejects the
 * subtree (sets *ok = false) on vector constants or secondary-table scans.
 *
 * Several streamable ops store extra operands in ext nodes rather than in
 * the standard inputs[] array.  These hidden children must be walked too:
 *   OP_SELECT  — ext->sort.columns[0..n_cols-1]
 *   OP_IF      — else branch: g->nodes[ext->third_in]
 *   OP_SUBSTR  — length arg:  g->nodes[ext->third_in]
 *   OP_REPLACE — replacement: g->nodes[ext->third_in]
 *   OP_CONCAT  — args 2+:    g->nodes[trail[i-2]] (uint32_t[] after ext) */
static bool subtree_has_default_scan(ray_graph_t* g, ray_op_t* op, bool* ok,
                                     uint64_t* visited) {
    if (!op || !*ok) return false;
    /* Skip already-visited nodes (DAGs may share subexpressions). */
    uint32_t nid = op->id;
    if (nid < g->node_count) {
        if (visited[nid / 64] & (1ULL << (nid % 64))) return false;
        visited[nid / 64] |= (1ULL << (nid % 64));
    }
    uint16_t opc = op->opcode;
    if (opc == OP_CONST) {
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (ext && ext->literal && !ray_is_atom(ext->literal))
            *ok = false;           /* vector constant — can't stream */
        return false;
    }
    if (opc == OP_SCAN) {
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (ext) {
            uint16_t stored_id = 0;
            memcpy(&stored_id, ext->base.pad, sizeof(uint16_t));
            if (stored_id > 0) { *ok = false; return false; }
            return true;           /* default-table scan */
        }
        return false;
    }
    if (!op_streamable(opc)) { *ok = false; return false; }
    bool found = false;
    for (uint8_t i = 0; i < op->arity && i < 2; i++)
        found |= subtree_has_default_scan(g, op_child(g, op, i), ok, visited);

    /* Walk hidden operands stored in ext nodes */
    if (opc == OP_SELECT) {
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (ext) {
            for (uint32_t c = 0; c < ext->sort.n_cols && *ok; c++)
                found |= subtree_has_default_scan(g, op_node(g, ext->sort.columns[c]), ok, visited);
        }
    } else if (opc == OP_IF || opc == OP_SUBSTR || opc == OP_REPLACE) {
        /* 3rd operand stored as node index in ext->third_in */
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (ext) {
            uint32_t child_id = ext->third_in;
            if (child_id < g->node_count)
                found |= subtree_has_default_scan(g, &g->nodes[child_id], ok, visited);
        }
    } else if (opc == OP_CONCAT) {
        /* n_args in ext->sym, args 2+ as uint32_t[] trailing after ext */
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (ext) {
            int n_args = (int)ext->sym;
            uint32_t* trail = (uint32_t*)((char*)(ext + 1));
            for (int i = 2; i < n_args && *ok; i++) {
                if (trail[i - 2] < g->node_count)
                    found |= subtree_has_default_scan(g, &g->nodes[trail[i - 2]], ok, visited);
            }
        }
    }
    return found;
}

/* Check whether a DAG rooted at `root` can be correctly executed via
 * segment streaming with simple concatenation merge.
 * Every node in the root's subtree must be streamable, and at least one
 * OP_SCAN must read from the default table (stored_table_id == 0).
 * OP_CONST is allowed only for scalar (atom) literals — vector constants
 * have total-row length and would mismatch per-segment data.
 * OP_SCAN nodes referencing secondary tables (stored_table_id > 0)
 * disqualify streaming, since the loop only swaps g->table.
 * DAGs that never scan the default table (e.g. a bare OP_CONST behind
 * passthrough ops) are rejected to avoid duplicating table-independent
 * results across partitions. */
static bool dag_can_stream(ray_graph_t* g, ray_op_t* root) {
    uint32_t n_words = (g->node_count + 63) / 64;
    uint64_t  stack_buf[16];                  /* covers DAGs up to 1024 nodes */
    ray_t* visited_hdr = NULL;
    uint64_t* visited;
    if (n_words <= 16) {
        visited = stack_buf;
    } else {
        visited = (uint64_t*)scratch_alloc(&visited_hdr, n_words * 8);
        if (!visited) return false;
    }
    memset(visited, 0, n_words * 8);
    bool ok = true;
    bool has_default_scan = subtree_has_default_scan(g, root, &ok, visited);
    if (visited_hdr) scratch_free(visited_hdr);
    return ok && has_default_scan;
}

static ray_t* ray_execute_inner(ray_graph_t* g, ray_op_t* root);

ray_t* ray_execute(ray_graph_t* g, ray_op_t* root) {
    /* The qstats capture mode is armed once per query at the eval boundary
     * (ray_eval, eval_depth==0) — covering PROF (profiler/query-log) AND the
     * PROGRESS bit (progress pump).  Do NOT overwrite it here: a bare
     * set_mode would drop the PROGRESS bit (killing the pump) and the qlog's
     * PROF bit on a profiling-off query.  Only OR PROF in defensively, so a
     * direct ray_execute call outside an eval boundary still profiles. */
    if (g_ray_profile.active)
        ray_qstats_set_mode(ray_qstats_mode() | RAY_QS_PROF);
    /* Progress ends at the eval boundary (the true query end), not here —
     * a query can drive several ray_execute calls, and ending after each
     * would reset the elapsed clock and fire premature "final" ticks. */
    return ray_execute_inner(g, root);
}

/* Flatten one parted/mapcommon column into a dense vector (mirrors the
 * OP_SCAN cold-path flatten at the top of this file).  Returns an owned
 * flat vector or a RAY_ERR.  Precondition: col is RAY_MAPCOMMON or
 * RAY_IS_PARTED. */
static ray_t* flatten_parted_col(ray_t* col) {
    if (col->type == RAY_MAPCOMMON)
        return materialize_mapcommon(col);
    int8_t base = (int8_t)RAY_PARTED_BASETYPE(col->type);
    ray_t** segs = (ray_t**)ray_data(col);
    int64_t total = ray_parted_nrows(col);
    if (base == RAY_STR)
        return parted_flatten_str(segs, col->len, total);
    uint8_t sba = (base == RAY_SYM) ? parted_sym_max_attrs(segs, col->len) : 0;
    ray_t* flat = typed_vec_new(base, sba, total);
    if (!flat || RAY_IS_ERR(flat)) return ray_error("oom", NULL);
    flat->len = total;
    int64_t off = 0;
    uint8_t seg_nulls = 0;
    for (int64_t s = 0; s < col->len; s++) {
        if (!segs[s] || segs[s]->len <= 0) continue;
        parted_copy_cells(ray_data(flat), base, sba, off,
                          segs[s], 0, segs[s]->len);
        seg_nulls |= segs[s]->attrs & RAY_ATTR_HAS_NULLS;
        off += segs[s]->len;
    }
    /* Null-model invariant 16.4: parted_copy_cells copies sentinel
     * bit-patterns but not HAS_NULLS — OR it across segments. */
    flat->attrs |= seg_nulls;
    /* Raw-copied SYM cell ids resolve over the partitions' shared
     * domain (first SYM segment is the rep). */
    if (flat->type == RAY_SYM)
        ray_sym_vec_adopt_domain(flat, sym_domain_rep(col));
    return flat;
}

static ray_t* ray_execute_inner(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root) return ray_error("nyi", NULL);

    /* Lazy-init the global thread pool on first call */
    ray_pool_t* pool = ray_pool_get();

    /* Reset cancellation flag at the start of each query */
    if (pool)
        atomic_store_explicit(&pool->cancelled, 0, memory_order_relaxed);

    /* Detect streaming mode: check if g->table has parted columns.
     * All non-MAPCOMMON columns must be parted; a flat (non-parted)
     * column would be duplicated across every segment table, producing
     * wrong results after concatenation merge.
     * All parted columns must agree on segment count — a mismatch is
     * a malformed table and is rejected upfront. */
    int32_t seg_count = 0;
    if (g->table) {
        bool has_flat = false;
        for (int64_t c = 0; c < ray_table_ncols(g->table); c++) {
            ray_t* col = ray_table_get_col_idx(g->table, c);
            if (!col) continue;
            if (RAY_IS_PARTED(col->type)) {
                if (seg_count == 0)
                    seg_count = (int32_t)col->len;
                else if ((int32_t)col->len != seg_count)
                    return ray_error("schema", NULL);
            } else if (col->type != RAY_MAPCOMMON) {
                has_flat = true;
            }
        }
        if (has_flat)
            seg_count = 0;  /* fall back to flat materialization */
    }

    if (seg_count == 0 || !dag_can_stream(g, root)) {
        /* Non-parted table or DAG contains ops that need specialized merge:
         * use existing flat-materialization path. */
        ray_t* result = exec_node(g, root);
        if (g->selection && result && !RAY_IS_ERR(result)
            && result->type == RAY_TABLE) {
            /* Projection-aware compaction: a select publishes the keys-only
             * keep-set of its distinct downstream via __VM->filt_keep before
             * this ray_execute; gather only those columns.  Depth-gated so a
             * nested subquery's own finalization does not consume it. */
            const int64_t* keep = NULL; int keep_n = 0;
            if (__VM && __VM->filt_keep && __VM->filt_keep_n > 0
                && __VM->eval_depth == __VM->filt_depth) {
                keep = __VM->filt_keep; keep_n = __VM->filt_keep_n;
            }
            ray_t* compacted = sel_compact(g, result, g->selection, keep, keep_n);
            ray_release(result);
            ray_release(g->selection);
            g->selection = NULL;
            result = compacted;
        } else if (result && !RAY_IS_ERR(result) && result->type == RAY_TABLE) {
            /* All-pass / no-selection finalization: an OP_FILTER whose
             * predicate matched every row (or a select with no where:)
             * returns the SOURCE table verbatim.  A parted/splayed source
             * still holds segmented (RAY_IS_PARTED) / RAY_MAPCOMMON columns
             * that the partial-filter path would have flattened via
             * sel_compact.  Build a flattened copy (never mutating the
             * possibly-stored source) so downstream at/count/first see dense
             * columns, matching the partial-filter path. */
            int64_t ncols = ray_table_ncols(result);
            bool any_parted = false;
            for (int64_t c = 0; c < ncols; c++) {
                ray_t* col = ray_table_get_col_idx(result, c);
                if (col && (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON)) {
                    any_parted = true; break;
                }
            }
            if (any_parted) {
                ray_t* out = ray_table_new(ncols);
                for (int64_t c = 0; out && !RAY_IS_ERR(out) && c < ncols; c++) {
                    ray_t* col = ray_table_get_col_idx(result, c);
                    int64_t name = ray_table_col_name(result, c);
                    if (col && (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON)) {
                        ray_t* flat = flatten_parted_col(col);
                        if (!flat || RAY_IS_ERR(flat)) {
                            ray_release(out); ray_release(result);
                            return flat ? flat : ray_error("oom", NULL);
                        }
                        out = ray_table_add_col(out, name, flat);
                        ray_release(flat);
                    } else if (col) {
                        out = ray_table_add_col(out, name, col);
                    }
                }
                ray_release(result);
                result = out;
            }
        }
        return result;
    }

    /* Streaming mode: find seg_mask from optimizer (if any) */
    uint64_t* seg_mask = NULL;
    int64_t   seg_mask_count = 0;
    for (uint32_t e = 0; e < g->ext_count; e++) {
        if (g->ext_nodes[e] && g->ext_nodes[e]->seg_mask) {
            seg_mask = g->ext_nodes[e]->seg_mask;
            seg_mask_count = g->ext_nodes[e]->seg_mask_count;
            break;
        }
    }

    /* Validate mask covers all segments — a mismatch means the
     * MAPCOMMON key count disagrees with the parted column segment
     * count, which is a schema error.  Surface it rather than
     * silently dropping data. */
    if (seg_mask && seg_mask_count != (int64_t)seg_count)
        return ray_error("schema", NULL);

    ray_t* saved_table = g->table;
    ray_t* result = NULL;

    for (int32_t s = 0; s < seg_count; s++) {
        /* Check pruning mask */
        if (seg_mask && !(seg_mask[s / 64] & (1ULL << (s % 64))))
            continue;

        /* Check cancellation */
        if (pool && atomic_load_explicit(&pool->cancelled, memory_order_relaxed)) {
            g->table = saved_table;
            if (g->selection) { ray_release(g->selection); g->selection = NULL; }
            ray_release(result);
            return ray_error("cancel", NULL);
        }

        /* Build flat table for this segment and swap g->table.
         * All operators (OP_SCAN, GROUP, expr_compile, etc.) see flat
         * columns via g->table, so no special-casing is needed. */
        ray_t* seg_tbl = build_segment_table(saved_table, s);
        if (!seg_tbl || RAY_IS_ERR(seg_tbl)) {
            g->table = saved_table;
            if (g->selection) { ray_release(g->selection); g->selection = NULL; }
            ray_release(result);
            return seg_tbl;
        }
        g->table = seg_tbl;
        if (g->selection) ray_release(g->selection);
        g->selection = NULL;

        ray_t* partial = exec_node(g, root);

        /* Compact lazy selection for this segment */
        if (g->selection && partial && !RAY_IS_ERR(partial)
            && partial->type == RAY_TABLE) {
            ray_t* compacted = sel_compact(g, partial, g->selection, NULL, 0);
            ray_release(partial);
            ray_release(g->selection);
            g->selection = NULL;
            partial = compacted;
        }

        g->table = saved_table;
        ray_release(seg_tbl);

        if (!partial || RAY_IS_ERR(partial)) {
            if (g->selection) { ray_release(g->selection); g->selection = NULL; }
            ray_release(result);
            return partial;
        }

        /* Merge partial into accumulator */
        ray_t* merged = ray_result_merge(result, partial);
        ray_release(result);
        ray_release(partial);
        if (!merged || RAY_IS_ERR(merged)) {
            if (g->selection) { ray_release(g->selection); g->selection = NULL; }
            return merged;
        }
        result = merged;
    }

    /* Clean up any lingering selection from the last segment iteration */
    if (g->selection) { ray_release(g->selection); g->selection = NULL; }

    /* All segments pruned: execute DAG on empty table to get correct
     * output schema (handles SELECT/PROJECT that reshape columns).
     * Build a fresh 0-row table — do not mutate shared source vectors. */
    if (!result) {
        int64_t ncols = ray_table_ncols(saved_table);
        ray_t* empty_tbl = ray_table_new(ncols);
        if (empty_tbl && !RAY_IS_ERR(empty_tbl)) {
            for (int64_t c = 0; c < ncols; c++) {
                int64_t name_id = ray_table_col_name(saved_table, c);
                ray_t* col = ray_table_get_col_idx(saved_table, c);
                if (!col) continue;
                int8_t base = col->type;
                if (col->type == RAY_MAPCOMMON) {
                    ray_t** mc = (ray_t**)ray_data(col);
                    base = mc[0] ? mc[0]->type : RAY_I64;
                } else if (RAY_IS_PARTED(col->type)) {
                    base = (int8_t)RAY_PARTED_BASETYPE(col->type);
                }
                ray_t* ecol = ray_vec_new(base, 0);
                if (!ecol || RAY_IS_ERR(ecol)) {
                    /* ray_vec_new rejects RAY_LIST (type 0) and other
                     * non-standard types; fall back to a raw 0-length
                     * block with the correct type tag. */
                    ecol = ray_alloc(0);
                    if (!ecol || RAY_IS_ERR(ecol)) continue;
                    ecol->type = base;
                    ecol->len = 0;
                }
                empty_tbl = ray_table_add_col(empty_tbl, name_id, ecol);
                ray_release(ecol);
            }
            g->table = empty_tbl;
            if (g->selection) ray_release(g->selection);
            g->selection = NULL;
            result = exec_node(g, root);
            if (g->selection) {
                ray_release(g->selection);
                g->selection = NULL;
            }
            g->table = saved_table;
            ray_release(empty_tbl);
        }
    }

    if (!result) return ray_error("oom", NULL);
    return result;
}
