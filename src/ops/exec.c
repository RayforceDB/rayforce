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
#include "ops/colgather.h"   /* ray_col_gather_slot: the refinement over encoded columns */
#include "ops/colbind.h"
#include "lang/internal.h"  /* sym_domain_rep (sym-domain Phase 2) */
#include "lang/format.h"    /* ray_type_name (error context) */
#include "ops/rowsel.h"
#include "store/enccol.h"
#include "store/enceval.h"
#include "store/encidx.h"   /* ray_enc_idx_kind_has_region */
#include "table/table.h"
#include "ops/fused_group.h"
#include "ops/idxop.h"
#include "ops/hash.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "core/qstats.h"   /* per-worker parallelism stats for profile spans */
#include "core/qmeasure.h" /* shared worker aggregate */
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

/* Materialize the rows `idx[0..n)` (ascending) of a MAPCOMMON column. */
ray_t* materialize_mapcommon_gather(ray_t* mc, const int64_t* idx, int64_t n) {
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
    int64_t part_idx = 0;
    int64_t part_end = n_parts > 0 ? counts[0] : 0;
    for (int64_t i = 0; i < n; i++) {
        while (part_idx < n_parts - 1 && idx[i] >= part_end) {
            part_idx++;
            part_end += counts[part_idx];
        }
        memcpy(out + (size_t)i * esz, kdata + (size_t)part_idx * esz, esz);
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
/* Contiguous LIST slice with per-cell retain — the raw byte copy the HEAD/TAIL
 * trims use for flat columns would alias the source's ray_t* cells without a
 * refcount and double-free on release (issue #355; same reason exec_filter_head
 * gathers LIST columns separately).  col_vec_new/typed_vec_new also reject
 * RAY_LIST, so without this the column comes back NULL and ray_table_add_col
 * fails with "column must be a vector".  Caller owns the returned list. */
static ray_t* list_slice_retain(ray_t* col, int64_t start, int64_t n) {
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (!out || RAY_IS_ERR(out)) return out;
    out->len = n;
    ray_t** d = (ray_t**)ray_data(out);
    ray_t** s = (ray_t**)ray_data(col);
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = s[start + i];
        if (e) ray_retain(e);
        d[i] = e;
    }
    return out;
}

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
    ray_t*         col;      /* NULL for a range-read source (cd set) */
    /* Range-read view: element i of the column is cd[i - cd_base], with SYM
     * width class cattrs.  NULL for a plain column, read at ray_data(col). */
    const void*    cd;
    int64_t        cd_base;
    uint8_t        cattrs;
    const double*  svf;
    const int64_t* svi;
    int64_t        sv_len;
    uint8_t*       ob;       /* output base; element i writes ob[i - ob_base] */
    int64_t        ob_base;  /* 0 for full-vec output; morsel start for fused-sel */
    int8_t         ct;
    /* SYM columns: per-domain-position membership verdict (1 = in set;
     * null slot preadjusted so XOR with negate always yields 0),
     * built once over the VOCABULARY in in_build_worker_ctx.  Turns the
     * per-row linear set scan into one byte load — any set size, width-
     * specialized, vectorizable.  NULL when not applicable. */
    const uint8_t* symlut;
    /* Needle hash, built when the set outgrows the SIMD small-set path
     * (IN_SIMD_SET): open addressing over the live probe values, so a
     * row costs one probe instead of a scan of the whole set.  hti holds
     * int64 values (empty = INT64_MIN; a real INT64_MIN needle sets
     * ht_has_min), htf holds f64 bit patterns (empty = IN_HTF_EMPTY, a
     * NaN, which is never inserted).  Both NULL = linear scan. */
    const int64_t*  hti;
    const uint64_t* htf;
    int64_t        ht_mask;
    bool           ht_has_min;
    bool           col_has_nulls;
    bool           col_atom_null;
    bool           col_is_atom;
    bool           use_double;
    bool           negate;
} in_worker_ctx_t;

/* Sets up to this many live elements take the unrolled SIMD compare in
 * exec_in_worker; larger ones get the needle hash (in_build_worker_ctx). */
#define IN_SIMD_SET 8
#define IN_HTF_EMPTY UINT64_C(0x7ff8000000000000)

/* -0.0 and +0.0 compare equal, so they must share a slot. */
static inline uint64_t in_f64_key(double v) {
    uint64_t b;
    memcpy(&b, &v, sizeof(b));
    return b == UINT64_C(0x8000000000000000) ? 0 : b;
}

static inline int in_set_has_i(const in_worker_ctx_t* c, int64_t v) {
    if (c->hti) {
        if (v == INT64_MIN) return c->ht_has_min;
        int64_t s = (int64_t)(ray_hash_i64(v) & (uint64_t)c->ht_mask);
        for (;;) {
            int64_t k = c->hti[s];
            if (k == v) return 1;
            if (k == INT64_MIN) return 0;
            s = (s + 1) & c->ht_mask;
        }
    }
    for (int64_t j = 0; j < c->sv_len; j++)
        if (v == c->svi[j]) return 1;
    return 0;
}

static inline int in_set_has_f(const in_worker_ctx_t* c, double v) {
    if (c->htf) {
        if (v != v) return 0;  /* NaN equals nothing, as in the scan */
        uint64_t b = in_f64_key(v);
        int64_t s = (int64_t)(ray_hash_i64((int64_t)b) & (uint64_t)c->ht_mask);
        for (;;) {
            uint64_t k = c->htf[s];
            if (k == b) return 1;
            if (k == IN_HTF_EMPTY) return 0;
            s = (s + 1) & c->ht_mask;
        }
    }
    for (int64_t j = 0; j < c->sv_len; j++)
        if (v == c->svf[j]) return 1;
    return 0;
}

/* The worker body, specialized on `rng` (a range-read view in cd, else
 * the plain column): exec_in_worker instantiates it twice, so the plain
 * path compiles to loops with no view offset or view null test. */
RAY_INLINE void exec_in_body(const in_worker_ctx_t* c, int64_t start, int64_t end,
                             const bool rng) {
    ray_t* col = c->col;
    const void* cd = c->col_is_atom ? NULL : (rng ? c->cd : ray_data(col));
    const int64_t cd_base = rng ? c->cd_base : 0;
    int8_t ct = c->ct;
    uint8_t cattrs = c->col_is_atom ? 0 : (rng ? c->cattrs : col->attrs);
    /* Output index is relative to ob_base so a fused-selection caller can
     * point ob at a morsel-local scratch (ob_base = morsel start) while the
     * column is still read at the absolute row index i: element i writes
     * ob[i - ob_base], which lands at scratch[0..n).  Full-vec callers set
     * ob_base = 0, leaving the plain ob[i].  A range-read view is indexed
     * the same way: element i is cd[i - cd_base]. */
    uint8_t* ob = c->ob;
    int64_t ob_base = c->ob_base;
    int64_t sv_len = c->sv_len;
    int negate = c->negate ? 1 : 0;

    #define IN_READ_I64(dst, idx) do {                                      \
        int64_t _ix = (idx) - cd_base;                                      \
        switch (ct) {                                                       \
        case RAY_BOOL: case RAY_U8: (dst) = ((const uint8_t*)cd)[_ix]; break; \
        case RAY_I16:  (dst) = ((const int16_t*)cd)[_ix]; break;            \
        case RAY_I32:  case RAY_DATE: case RAY_TIME:                        \
                       (dst) = ((const int32_t*)cd)[_ix]; break;            \
        case RAY_I64:  case RAY_TIMESTAMP:                                  \
                       (dst) = ((const int64_t*)cd)[_ix]; break;            \
        case RAY_SYM:  (dst) = ray_read_sym(cd, _ix, ct, cattrs); break;    \
        default:       (dst) = 0; break;                                    \
        }                                                                   \
    } while (0)

    #define IN_READ_F64(dst, idx) do {                                      \
        int64_t _ix = (idx) - cd_base;                                      \
        switch (ct) {                                                       \
        case RAY_BOOL: case RAY_U8: (dst) = (double)((const uint8_t*)cd)[_ix]; break; \
        case RAY_I16:  (dst) = (double)((const int16_t*)cd)[_ix]; break;    \
        case RAY_I32:  case RAY_DATE: case RAY_TIME:                        \
                       (dst) = (double)((const int32_t*)cd)[_ix]; break;    \
        case RAY_I64:  case RAY_TIMESTAMP:                                  \
                       (dst) = (double)((const int64_t*)cd)[_ix]; break;    \
        case RAY_F32:  (dst) = (double)((const float*)cd)[_ix]; break;      \
        case RAY_F64:  (dst) = ((const double*)cd)[_ix]; break;             \
        default:       (dst) = 0.0; break;                                  \
        }                                                                   \
    } while (0)

    /* Hoist col_atom_null, col_is_atom, col_has_nulls outside the loops so
     * the per-element null check is a single ray_vec_is_null call when
     * nulls are present, or eliminated entirely when they are not. */
    bool vec_has_nulls = c->col_has_nulls && !c->col_is_atom;
    #define IN_IS_NULL(i) (rng ? ray_raw_is_null(ct, cattrs, cd, (i) - cd_base) \
                                 : ray_vec_is_null(col, (i)))

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
     * Canonical null is raw id 0 and is handled by the same LUT lookup. */
    if (!c->col_is_atom && c->symlut && ct == RAY_SYM) {
        const uint8_t* lut = c->symlut;
        uint8_t neg = (uint8_t)negate;
        uint8_t w = (uint8_t)(cattrs & RAY_SYM_W_MASK);
        if (w == RAY_SYM_W8) {
            const uint8_t* dp = (const uint8_t*)cd;
            for (int64_t i = start; i < end; i++)
                ob[i - ob_base] = (uint8_t)(lut[dp[i - cd_base]] ^ neg);
        } else if (w == RAY_SYM_W16) {
            const uint16_t* dp = (const uint16_t*)cd;
            for (int64_t i = start; i < end; i++)
                ob[i - ob_base] = (uint8_t)(lut[dp[i - cd_base]] ^ neg);
        } else if (w == RAY_SYM_W32) {
            const uint32_t* dp = (const uint32_t*)cd;
            for (int64_t i = start; i < end; i++)
                ob[i - ob_base] = (uint8_t)(lut[dp[i - cd_base]] ^ neg);
        } else {
            const int64_t* dp = (const int64_t*)cd;
            for (int64_t i = start; i < end; i++)
                ob[i - ob_base] = (uint8_t)(lut[dp[i - cd_base]] ^ neg);
        }
        return;
    }

    if (!c->col_is_atom && !c->use_double && sv_len >= 1 && sv_len <= IN_SIMD_SET) {
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
                    CTYPE v = dp[i - cd_base];                               \
                    uint8_t f = (uint8_t)((v==t0)|(v==t1)|(v==t2)|(v==t3)|   \
                                          (v==t4)|(v==t5)|(v==t6)|(v==t7));  \
                    uint8_t isn = (uint8_t)(v == sent);                      \
                    ob[i - ob_base] = (uint8_t)(isn ? 0 : (f ^ neg));       \
                }                                                            \
            } else {                                                         \
                for (int64_t i = start; i < end; i++) {                      \
                    CTYPE v = dp[i - cd_base];                               \
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
        if (c->col_atom_null) {
            /* All elements are null — fill zeros */
            for (int64_t i = start; i < end; i++) ob[i - ob_base] = 0;
        } else if (vec_has_nulls) {
            for (int64_t i = start; i < end; i++) {
                if (IN_IS_NULL(i)) { ob[i - ob_base] = 0; continue; }
                double cv;
                if (c->col_is_atom) cv = (ct == RAY_F64) ? col->f64 : (double)col->i64;
                else IN_READ_F64(cv, i);
                ob[i - ob_base] = (uint8_t)(in_set_has_f(c, cv) ^ negate);
            }
        } else {
            for (int64_t i = start; i < end; i++) {
                double cv;
                if (c->col_is_atom) cv = (ct == RAY_F64) ? col->f64 : (double)col->i64;
                else IN_READ_F64(cv, i);
                ob[i - ob_base] = (uint8_t)(in_set_has_f(c, cv) ^ negate);
            }
        }
    } else {
        if (c->col_atom_null) {
            for (int64_t i = start; i < end; i++) ob[i - ob_base] = 0;
        } else if (vec_has_nulls) {
            for (int64_t i = start; i < end; i++) {
                if (IN_IS_NULL(i)) { ob[i - ob_base] = 0; continue; }
                int64_t cv;
                if (c->col_is_atom) cv = col->i64;
                else IN_READ_I64(cv, i);
                ob[i - ob_base] = (uint8_t)(in_set_has_i(c, cv) ^ negate);
            }
        } else {
            for (int64_t i = start; i < end; i++) {
                int64_t cv;
                if (c->col_is_atom) cv = col->i64;
                else IN_READ_I64(cv, i);
                ob[i - ob_base] = (uint8_t)(in_set_has_i(c, cv) ^ negate);
            }
        }
    }
    #undef IN_READ_I64
    #undef IN_READ_F64
    #undef IN_IS_NULL
}

/* The range-read instance, out of line so the plain instance keeps the
 * worker's own code layout. */
static __attribute__((noinline)) void exec_in_worker_rng(const in_worker_ctx_t* c,
                                                         int64_t start, int64_t end) {
    exec_in_body(c, start, end, true);
}

static void exec_in_worker(void* vctx, uint32_t worker_id,
                           int64_t start, int64_t end) {
    (void)worker_id;
    const in_worker_ctx_t* c = (const in_worker_ctx_t*)vctx;
    if (c->cd) { exec_in_worker_rng(c, start, end); return; }
    exec_in_body(c, start, end, false);
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

/* The body shared by in_build_worker_ctx (a column vector or atom) and
 * in_build_worker_ctx_meta (a bound source read by range), over the
 * column's description: `atom` is the column when it is an atom (else
 * NULL), ct its element type, may_null its null flag, and dom (SYM) the
 * domain its raw ids resolve over.  out_ctx->col is left NULL. */
static in_ctx_status_t in_build_ctx_core(ray_t* atom, int8_t ct, bool may_null,
                                         struct ray_sym_domain_s* dom,
                                         ray_t* set, bool negate,
                                         double* svf_stack, int64_t* svi_stack,
                                         in_worker_ctx_t* out_ctx,
                                         ray_t** sv_hdr_out,
                                         ray_t** lut_hdr_out) {
    *sv_hdr_out = NULL;
    *lut_hdr_out = NULL;
    int64_t set_len = ray_is_atom(set) ? 1 : set->len;

    int8_t st = ray_is_atom(set) ? (int8_t)(-set->type) : set->type;
    if (RAY_IS_PARTED(st)) st = (int8_t)RAY_PARTED_BASETYPE(st);

    if (ct == RAY_STR || st == RAY_STR) return IN_CTX_UNSUPPORTED;
    /* GUID cells are 16-byte blobs; the typed kernel's READ_I64/IN_READ_I64
     * loops have no guid case and read them at the wrong width (every cell
     * decodes to 0, so membership silently matched everything).  Bail so the
     * caller routes to the guid-aware hashset path, mirroring how RAY_STR bails
     * above and the guid == fix. */
    if (ct == RAY_GUID || st == RAY_GUID) return IN_CTX_UNSUPPORTED;

    /* Classify each side: 0=int-family, 1=float-family, 2=sym. */
    #define CLASSIFY(t)                                                    \
        ((t) == RAY_SYM ? 2 :                                              \
         ((t) == RAY_F32 || (t) == RAY_F64) ? 1 : 0)

    int col_class = CLASSIFY(ct);
    int set_class = CLASSIFY(st);

    /* A temporal value equals only its own type: atom_eq — and so the
     * hashset behind find/except and the null-bearing `in` — never matches
     * a DATE to an int or to a TIMESTAMP.  Comparing raw payloads here
     * matched day 0 to 0i, and a DATE's day count to a TIMESTAMP's
     * nanoseconds.  Empty probe, exactly like SYM vs non-SYM below. */
    #define IS_TEMPORAL(t) \
        ((t) == RAY_DATE || (t) == RAY_TIME || (t) == RAY_TIMESTAMP)
    if ((IS_TEMPORAL(ct) || IS_TEMPORAL(st)) && ct != st)
        set_len = 0;
    #undef IS_TEMPORAL

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
    bool col_has_nulls = !atom && may_null;
    bool col_atom_null = atom && RAY_ATOM_IS_NULL(atom);
    bool set_has_nulls = !ray_is_atom(set) && ray_vec_may_have_nulls(set);

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
        ray_t* set_rep = ray_is_atom(set) ? NULL : sym_domain_rep(set);
        struct ray_sym_domain_s* cd = (!atom && dom) ? dom : ray_sym_runtime_domain();
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
    if (!atom && col_class == 2 && !use_double && sv_len > 0) {
        struct ray_sym_domain_s* cdom = dom ? dom : ray_sym_runtime_domain();
        int64_t d = ray_sym_domain_count(cdom);
        if (d > 0 && d <= (int64_t)1 << 22) {
            ray_t* lh = ray_alloc(d);
            if (lh) {
                uint8_t* lut = (uint8_t*)ray_data(lh);
                memset(lut, 0, (size_t)d);
                for (int64_t j = 0; j < sv_len; j++)
                    if (svi[j] >= 0 && svi[j] < d) lut[svi[j]] = 1;
                /* Null rows fail both in and not-in. The worker XORs every
                 * entry with negate, so preadjust ID 0 to keep it false. */
                lut[0] = (uint8_t)negate;
                *lut_hdr_out = lh;
                symlut = lut;
            }
        }
    }

    /* Large set on a non-LUT column: hash the live needles once so each row
     * costs one probe.  The linear scan was O(rows × needles) — a nullable
     * I64 column against 100k needles took over a second.  An allocation
     * failure keeps the scan: slower, same answer. */
    const int64_t* hti = NULL;
    const uint64_t* htf = NULL;
    int64_t ht_mask = 0;
    bool ht_has_min = false;
    if (!atom && !symlut && sv_len > IN_SIMD_SET) {
        int64_t cap = 16;
        while (cap < sv_len * 2) cap <<= 1;
        ray_t* hh = ray_alloc((size_t)cap * sizeof(int64_t));
        if (hh) {
            ht_mask = cap - 1;
            if (use_double) {
                uint64_t* t = (uint64_t*)ray_data(hh);
                for (int64_t k = 0; k < cap; k++) t[k] = IN_HTF_EMPTY;
                for (int64_t j = 0; j < sv_len; j++) {
                    if (svf[j] != svf[j]) continue;  /* NaN matches nothing */
                    uint64_t b = in_f64_key(svf[j]);
                    int64_t sl = (int64_t)(ray_hash_i64((int64_t)b) & (uint64_t)ht_mask);
                    while (t[sl] != IN_HTF_EMPTY && t[sl] != b) sl = (sl + 1) & ht_mask;
                    t[sl] = b;
                }
                htf = t;
            } else {
                int64_t* t = (int64_t*)ray_data(hh);
                for (int64_t k = 0; k < cap; k++) t[k] = INT64_MIN;
                for (int64_t j = 0; j < sv_len; j++) {
                    int64_t v = svi[j];
                    if (v == INT64_MIN) { ht_has_min = true; continue; }
                    int64_t sl = (int64_t)(ray_hash_i64(v) & (uint64_t)ht_mask);
                    while (t[sl] != INT64_MIN && t[sl] != v) sl = (sl + 1) & ht_mask;
                    t[sl] = v;
                }
                hti = t;
            }
            *lut_hdr_out = hh;  /* freed by every caller with the LUT */
        }
    }

    *out_ctx = (in_worker_ctx_t){
        .col = atom,
        .svf = svf, .svi = svi, .sv_len = sv_len,
        .symlut = symlut,
        .hti = hti, .htf = htf, .ht_mask = ht_mask, .ht_has_min = ht_has_min,
        .ob = NULL, .ob_base = 0, .ct = ct,
        .col_has_nulls = col_has_nulls,
        .col_atom_null = col_atom_null,
        .col_is_atom = atom != NULL,
        .use_double = use_double,
        .negate = negate,
    };
    *sv_hdr_out = sv_hdr;
    return IN_CTX_OK;
}

static in_ctx_status_t in_build_worker_ctx(ray_t* col, ray_t* set, bool negate,
                                           double* svf_stack, int64_t* svi_stack,
                                           in_worker_ctx_t* out_ctx,
                                           ray_t** sv_hdr_out,
                                           ray_t** lut_hdr_out) {
    bool atom = ray_is_atom(col);
    int8_t ct = atom ? (int8_t)(-col->type) : col->type;
    if (RAY_IS_PARTED(ct)) ct = (int8_t)RAY_PARTED_BASETYPE(ct);
    struct ray_sym_domain_s* dom = NULL;
    if (!atom && ct == RAY_SYM) {
        ray_t* rep = sym_domain_rep(col);
        dom = rep ? ray_sym_vec_domain(rep) : ray_sym_runtime_domain();
    }
    in_ctx_status_t st = in_build_ctx_core(atom ? col : NULL, ct,
                                           !atom && ray_vec_may_have_nulls(col), dom,
                                           set, negate, svf_stack, svi_stack,
                                           out_ctx, sv_hdr_out, lut_hdr_out);
    if (st == IN_CTX_OK) out_ctx->col = col;
    return st;
}

/* The context for a bound source read by range (exec_in_to_selection's
 * generation-2 path): the column described by its type, persisted attrs,
 * null flag and SYM domain.  ctx.col stays NULL; each fill sets cd /
 * cd_base / cattrs to its range view. */
static in_ctx_status_t in_build_worker_ctx_meta(int8_t ct, uint8_t attrs, bool may_null,
                                                struct ray_sym_domain_s* dom,
                                                ray_t* set, bool negate,
                                                double* svf_stack, int64_t* svi_stack,
                                                in_worker_ctx_t* out_ctx,
                                                ray_t** sv_hdr_out,
                                                ray_t** lut_hdr_out) {
    in_ctx_status_t st = in_build_ctx_core(NULL, ct, may_null, dom, set, negate,
                                           svf_stack, svi_stack, out_ctx,
                                           sv_hdr_out, lut_hdr_out);
    if (st == IN_CTX_OK) out_ctx->cattrs = attrs;
    return st;
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

static ray_t* flatten_parted_col(ray_t* col, ray_t* tbl);
static ray_t* flatten_parted_table(ray_t* tbl, ray_t* counts_src);
static int64_t parted_table_rows(ray_t* tbl, ray_t* counts_src, ray_t** err_out);
static ray_t* parted_result_validate(ray_t* tbl, ray_t* counts_src);

/* Debug builds check that a parted column flattened out of a table has the
 * table's row count, its MAPCOMMON total (#724: a short flat column was read
 * past its end).  Release builds compile the check out. */
#if defined(DEBUG) && !defined(NDEBUG)
#include <assert.h>
static int64_t dbg_mapcommon_rows(ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl) || tbl->type != RAY_TABLE) return -1;
    for (int64_t c = 0; c < ray_table_ncols(tbl); c++) {
        ray_col_meta_t m;
        if (ray_table_col_meta_idx(tbl, c, &m) && m.type == RAY_MAPCOMMON) return m.len;
    }
    return -1;
}
#define PARTED_FLAT_DCHECK(tbl, flat) \
    assert(!(flat) || RAY_IS_ERR(flat) || dbg_mapcommon_rows(tbl) < 0 || \
           (flat)->len == dbg_mapcommon_rows(tbl))
#else
#define PARTED_FLAT_DCHECK(tbl, flat) ((void)0)
#endif

/* General membership fallback: the typed `in` kernel bails on guid operands
 * (16-byte cells it can't compare) and on string columns (variable-length
 * cells), so route them through the general ray_in_fn path, which builds a
 * guid- and string-aware hashset.  `col` is a materialized column vector, so
 * ray_in_fn returns a fresh RAY_BOOL vector we own and may invert in place
 * for not-in. */
static ray_t* exec_in_general_fallback(ray_t* col, ray_t* set, bool negate,
                                       ray_t* tbl) {
    ray_t* input = col;
    bool owns_input = false;
    if (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON) {
        input = flatten_parted_col(col, tbl);
        if (!input || RAY_IS_ERR(input)) return input ? input : ray_error("oom", NULL);
        owns_input = true;
    }

    ray_t* r = ray_in_fn(input, set);
    if (!r || RAY_IS_ERR(r)) {
        if (owns_input) ray_release(input);
        return r;
    }

    /* ray_in_fn returns a scalar for a scalar GUID subject.  Normalize it to
     * the vector contract of exec_in before applying not-in. */
    if (ray_is_atom(r) && r->type == -RAY_BOOL) {
        ray_t* out = ray_vec_new(RAY_BOOL, 1);
        if (!out || RAY_IS_ERR(out)) {
            ray_release(r);
            return out ? out : ray_error("oom", NULL);
        }
        out->len = 1;
        ((uint8_t*)ray_data(out))[0] = r->b8;
        ray_release(r);
        r = out;
    }

    /* The caller indexes the answer as one bool per row; anything else is
     * not a membership verdict for this column. */
    if (r->type != RAY_BOOL || r->len != (ray_is_atom(col) ? 1 : ray_len(input))) {
        int8_t rt = r->type;
        ray_release(r);
        if (owns_input) ray_release(input);
        return ray_error("type", "in: membership of a %s column answered %s, not a bool per row",
                         ray_type_name(ray_is_atom(col) ? (int8_t)-col->type : input->type),
                         ray_type_name(rt));
    }
    {
        uint8_t* b = (uint8_t*)ray_data(r);
        if (negate)
            for (int64_t i = 0; i < r->len; i++) b[i] = (uint8_t)!b[i];

        /* Mask after inversion: null rows must remain false for both IN and
         * NOT_IN, while the underlying membership helper may match null. */
        if (ray_is_atom(col)) {
            if (RAY_ATOM_IS_NULL(col)) b[0] = 0;
        } else if (input->type == RAY_GUID) {
            /* GUID nulls are all-zero cells; older table construction paths
             * may omit HAS_NULLS even though the sentinel is present. */
            static const uint8_t null_guid[16] = {0};
            const uint8_t* data = (const uint8_t*)ray_data(input);
            for (int64_t i = 0; i < r->len; i++)
                if (memcmp(data + i * 16, null_guid, 16) == 0) b[i] = 0;
        } else if (ray_vec_may_have_nulls(input)) {
            for (int64_t i = 0; i < r->len; i++)
                if (ray_vec_is_null(input, i)) b[i] = 0;
        }
    }
    if (owns_input) ray_release(input);
    return r;
}

/* True when the typed kernel's refusal is a shape the general path answers:
 * a guid on either side, or a string on either side (parted base included)
 * — a string column, or any column against a string set, which matches no
 * row of another type exactly as the bare `in` does.  A string ATOM subject
 * has one verdict only against a column of cells; against a lone string or
 * a list, ray_in_fn answers it per character, which is not a row verdict,
 * so those shapes stay out. */
static bool in_has_general_fallback(ray_t* col, ray_t* set) {
    int8_t ct = ray_is_atom(col) ? (int8_t)(-col->type) : col->type;
    int8_t st = ray_is_atom(set) ? (int8_t)(-set->type) : set->type;
    if (RAY_IS_PARTED(ct)) ct = (int8_t)RAY_PARTED_BASETYPE(ct);
    if (RAY_IS_PARTED(st)) st = (int8_t)RAY_PARTED_BASETYPE(st);
    if (ct == RAY_GUID || st == RAY_GUID) return true;
    if (ct != RAY_STR && st != RAY_STR) return false;
    /* Only a column of cells answers one verdict per row: a LIST column's
     * cell may be a vector, and its answer a list no mask can use. */
    if (!ray_is_atom(col) && !(ct >= RAY_BOOL && ct <= RAY_STR)) return false;
    if (ct == RAY_STR && ray_is_atom(col))
        return !ray_is_atom(set) && st != RAY_LIST;
    return true;
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
    if (st == IN_CTX_UNSUPPORTED) {
        if (in_has_general_fallback(col, set))
            return exec_in_general_fallback(col, set, negate, g ? g->table : NULL);
        return ray_error("nyi", "OP_IN on RAY_STR not yet implemented");
    }
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

/* Fused `in`/`not-in` → row selection (exec_in_to_selection).  Binds the
 * column of a root OP_IN/OP_NOT_IN of the shape (SCAN flat-col) IN (CONST
 * set) against g->table (a generation-2 column is read by range, never
 * decoded whole), builds the same probe buffer + worker context as exec_in,
 * then drives the per-task rowsel builders — each morsel runs exec_in_worker
 * into a morsel-local scratch (ob_base = morsel start) and streams the
 * verdict via rowsel_emit_segment.  Bit-identical to exec_in's bool output
 * (null rows → 0 → not-selected); only the sink differs.  Returns
 * NULL+all_pass=0 for any unsupported shape so the caller falls back to
 * exec_node→ray_rowsel_from_pred unchanged. */
static int idx_filter_in_decode(ray_graph_t* g, ray_op_t* pred_op, unsigned want,
                                ray_t** out_col, ray_t** out_set_lit, ray_col_meta_t* out_meta);

/* What a filter-index consult needs from a column, decided from metadata so
 * an encoded column without it is never decoded to find that out: any index
 * (IDXW_INDEX), a sorted attribute (IDXW_SORTED), or neither (the caller
 * reads the column's values). */
enum { IDXW_NONE = 0, IDXW_INDEX = 1, IDXW_SORTED = 2 };

static bool idx_want_ok(const ray_col_meta_t* m, unsigned want) {
    if (!want) return true;
    return ((want & IDXW_INDEX) && m->index_kind != RAY_IDX_NONE) ||
           ((want & IDXW_SORTED) && (m->attrs & RAY_ATTR_SORTED));
}

/* Metadata admits the lazy in-memory hash (ray_idx_lazy_hash): an 8-byte
 * integer or timestamp column, no nulls, a chunk zone the hash may replace.  The column's own
 * shape test runs on the decoded vector after this. */
static bool idx_lazy_meta_ok(const ray_col_meta_t* m) {
    if (m->index_kind != RAY_IDX_CHUNK_ZONE) return false;
    /* the lazy cost test admits 8-byte columns only */
    if (m->type != RAY_I64 && m->type != RAY_TIMESTAMP) return false;
    return m->null_count >= 0 ? m->null_count == 0 : !(m->attrs & RAY_ATTR_HAS_NULLS);
}

/* The (kind, op, type) pairs the generation-0 single-compare block consults
 * a persisted region index for, decided from metadata (cmp_op, key_i and
 * is_float as idx_filter_decode reconciled them with the column):
 *   BLOOM        EQ, integer key (the definite-absent probe, step 2);
 *   HASH / PART  EQ, integer key, a nonzero SYM key or a column without
 *                HAS_NULLS (ray_vec_may_have_nulls of the materialization,
 *                whose attrs are the persisted ones; step 3);
 *   SORT         every compare but NE (the range probe, step 4).
 * Any other shape consults no region index on generation 0. */
static bool idx_enc_cmp_uses_index(const ray_col_meta_t* m, uint16_t cmp_op, int64_t key_i, int is_float) {
    switch (m->index_kind) {
    case RAY_IDX_BLOOM: return cmp_op == OP_EQ && !is_float;
    case RAY_IDX_HASH:
    case RAY_IDX_PART:
        if (cmp_op != OP_EQ || is_float) return false;
        if (m->type == RAY_SYM) return key_i != 0;
        return m->type != RAY_STR && !(m->attrs & RAY_ATTR_HAS_NULLS);
    case RAY_IDX_SORT:  return cmp_op != OP_NE;
    default:            return false;
    }
}

/* The generation-0 `in` block's region-index pairs: HASH or PART over a
 * non-float column (the IN probe, step 5). */
static bool idx_enc_in_uses_index(const ray_col_meta_t* m) {
    return (m->index_kind == RAY_IDX_HASH || m->index_kind == RAY_IDX_PART) &&
           m->type != RAY_F32 && m->type != RAY_F64;
}

/* An unpublished generation-2 column is consulted from its directory (P3's
 * encoded route) unless the shape uses its recorded region index (`uses`,
 * from idx_enc_cmp_uses_index / idx_enc_in_uses_index): then a recorded
 * HASH, SORT, BLOOM or PART answers as on generation 0, from the index its
 * materialization attaches (the "index an executor chose to use" exception
 * of spec "Instrumentation").  Every other shape materializes nothing, and
 * a malformed region fails only an index-using query. */
static bool idx_enc_from_dir(const ray_col_meta_t* m, bool uses) {
    return m->enc && !ray_enccol_decoded_peek(m->enc) &&
           !(uses && ray_enc_idx_kind_has_region(m->index_kind));
}

/* Materialize a region-kind generation-2 column for its index (called only
 * for a shape that uses it): NULL when the column is plain, published, or
 * materialized now; an owned error otherwise (spec "Accessors": an error
 * never reads as "no index"). */
static ray_t* idx_enc_index_ready(ray_graph_t* g, const ray_col_meta_t* m, int64_t sym) {
    if (!m->enc || !ray_enc_idx_kind_has_region(m->index_kind) || ray_enccol_decoded_peek(m->enc)) return NULL;
    ray_t* c = ray_table_col(g->table, sym);
    return (c && RAY_IS_ERR(c)) ? c : NULL;
}

/* A lazy-hash failure is best effort (the scan answers), except an
 * interrupted build: that is the query's cancellation, handed back. */
static ray_t* idx_lazy_err(ray_t* le) {
    if (!le) return NULL;
    if (strcmp(ray_err_code(le), "cancel") == 0) return le;
    ray_error_free(le);
    return NULL;
}

/* An `in` set that is a point lookup for the lazy hash: an integer-family
 * vector no larger than an eighth of the rows (the dense cutoff the hash
 * equality probe applies to one key's rows). */
static bool idx_lazy_set_ok_len(int64_t len, const ray_t* set) {
    if (!set || RAY_IS_ERR(set) || ray_is_atom(set)) return false;
    switch (set->type) {
    case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
    case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP: break;
    default: return false;
    }
    return set->len <= (len >> 3);
}
static bool idx_lazy_set_ok(const ray_t* col, const ray_t* set) {
    return idx_lazy_set_ok_len(col->len, set);
}

/* The plain column for a decode that declines (the scan fallback re-reads the
 * column through the accessors and reports a failure with its message).  A
 * region-kind generation-2 column's failure never reaches here: the arms
 * materialize it first (idx_enc_index_ready) and return that error. */
static ray_t* idx_decode_col(ray_t* tbl, int64_t sym) {
    ray_t* col = ray_table_col(tbl, sym);
    if (col && RAY_IS_ERR(col)) { ray_error_free(col); return NULL; }
    return col;
}

/* ---- STR equality / inequality as a selection (P6, spec Path 1) ----
 *
 * (== s "c") / (!= s "c") over a generation-2 STR column, single file or
 * parted.  Byte equality, nulls included: "" is the in-band null, so a ""
 * constant selects the null rows under ==, and a null row passes != of a
 * non-empty constant.  Per segment the constant is looked up once in the
 * column's STR_DICT entry (its code, -1 absent, -2 no entry).  A vector is
 * decided whole when the constant is non-empty and its code lies outside
 * the chunk's or the vector's code bounds (membership bounds over the
 * non-null rows: "" is never inside them, D4); a STR_DICT vector otherwise
 * compares its codes as a view; any other vector, a plain or materialized
 * segment and a synthetic all-null one are read by range and compared on
 * bytes.  A segment recording a persisted DICT (P6b), materialized or not,
 * compares its codes by range instead, with no bounds: its constant is
 * looked up once in the DICT's entries ("" is an entry like any other, so
 * the null rows are selected exactly). */
typedef struct {
    ray_col_src_t  src;        /* parted: the segment's own source, bound here */
    ray_col_src_t* rs;         /* the source its rows are read from by range */
    ray_enccol_t*  e;          /* single-file encoded, not materialized: codes and bounds; else NULL */
    int64_t        code;       /* the constant's code in e's entry: >= 0, -1 absent, -2 no entry */
    ray_enccol_t*  de;         /* single-file encoded with a persisted DICT (materialized or not); else NULL */
    int64_t        dcode;      /* the constant's DICT code: >= 0, -1 absent (-2, no DICT: de stays NULL) */
    bool           bound;
} str_seg_sel_t;

typedef struct {
    str_seg_sel_t*   segs;
    const int64_t*   off;      /* segment i: rows [off[i], off[i + 1]) */
    int64_t          n_segs;
    const char*      cs;
    uint32_t         clen;
    bool             ne;
    const char*      cname;    /* the column's name (error texts), cname_len bytes */
    int              cname_len;
    ray_scan_fail_t* fail;
    /* an installed selection the result is ANDed with (a later conjunct), or NULL */
    const uint8_t*   wflags;
    const uint32_t*  woff;
    const uint16_t*  widx;
} str_sel_ctx_t;

/* Rows [lr, lr + n) of segment s (segment-local rows) into out; false
 * with the failure noted under `key` (the rest of out is the caller's). */
static bool str_seg_fill(const str_sel_ctx_t* x, str_seg_sel_t* s, ray_col_scratch_t* sc, int64_t key,
                         int64_t lr, int64_t n, uint8_t* out) {
    const uint8_t miss = (uint8_t)x->ne;           /* a row that does not equal c */
    int64_t at = 0;
    while (at < n) {
        const int64_t row = lr + at;
        uint8_t* o = out + at;
        if (s->de) {
            /* the persisted DICT (spec path 1, P6b): codes by range, no bounds */
            const int64_t v = row / RAY_ENC_VEC;
            int64_t piece = (v + 1) * RAY_ENC_VEC - row;
            if (piece > n - at) piece = n - at;
            if (s->dcode < 0) { memset(o, miss, (size_t)piece); at += piece; continue; }
            const int32_t* cd = NULL;
            ray_t* err = ray_enccol_idict_codes(s->de, row, piece, &cd);
            if (err) { ray_scan_fail_note(x->fail, key, err); return false; }
            const int32_t cc = (int32_t)s->dcode;
            for (int64_t k = 0; k < piece; k++) o[k] = (uint8_t)((cd[k] == cc) ^ x->ne);
            at += piece;
            continue;
        }
        if (s->e) {
            const int64_t v = row / RAY_ENC_VEC;
            int64_t piece = (v + 1) * RAY_ENC_VEC - row;
            if (piece > n - at) piece = n - at;
            const int64_t c = v / RAY_ENC_VECS_PER_CHUNK;
            const int j = (int)(v % RAY_ENC_VECS_PER_CHUNK);
            ray_t* err = NULL;
            int64_t lo, hi;
            bool decided = false;
            if (x->clen > 0 && s->code >= -1) {     /* "" is never inside the bounds */
                if (ray_enccol_code_bounds(s->e, c, -1, &lo, &hi, &err))
                    decided = s->code < lo || s->code > hi;
                if (!decided && !err && ray_enccol_code_bounds(s->e, c, j, &lo, &hi, &err))
                    decided = s->code < lo || s->code > hi;
                if (err) { ray_scan_fail_note(x->fail, key, err); return false; }
            }
            if (decided) { memset(o, miss, (size_t)piece); at += piece; continue; }
            if (s->code >= -1) {
                const ray_enc_desc_t* d;
                if ((err = ray_enccol_chunk_descs_ref(s->e, c, &d)) != NULL) {
                    ray_scan_fail_note(x->fail, key, err); return false;
                }
                if (d[j].encoding == RAY_ENC_STR_DICT) {
                    ray_enc_vview_t vv;                 /* on this task's stack */
                    int vr = ray_enccol_vview(s->e, v, &vv, &err);
                    if (vr < 0) { ray_scan_fail_note(x->fail, key, err); return false; }
                    if (vr > 0 && vv.enc == RAY_ENC_STR_DICT) {
                        const int64_t k0 = row - v * RAY_ENC_VEC;
                        if (k0 + piece > vv.n) {
                            ray_scan_fail_note(x->fail, key, ray_error("corrupt",
                                "column %s: STR selection: vector %lld holds %d rows", s->e->path, (long long)v, vv.n));
                            return false;
                        }
                        const uint32_t* cd = vv.codes + k0;
                        if (s->code < 0) memset(o, miss, (size_t)piece);     /* absent: no row equals c */
                        else {
                            const uint32_t cc = (uint32_t)s->code;
                            for (int64_t k = 0; k < piece; k++) o[k] = (uint8_t)((cd[k] == cc) ^ x->ne);
                        }
                        at += piece;
                        continue;
                    }
                    /* declined: the range read decodes or reports it */
                }
            }
        }
        ray_col_range_t rv;
        ray_t* err = ray_col_range(s->rs, row, n - at, sc, &rv);
        if (!err && rv.n <= 0)
            err = s->e ? ray_error("range", "column %s: STR selection: empty range read at row %lld", s->e->path, (long long)row)
                       : ray_error("range", "column %.*s: STR selection: empty range read at row %lld",
                                   x->cname_len, x->cname, (long long)row);
        if (err) { ray_scan_fail_note(x->fail, key, err); return false; }
        const ray_str_t* sd = (const ray_str_t*)rv.data;
        const uint32_t cl = x->clen;
        for (int64_t k = 0; k < rv.n; k++) {
            const uint32_t len = sd[k].len;
            const bool eq = len == cl && (len == 0 || memcmp(ray_str_t_ptr(&sd[k], rv.str_pool), x->cs, len) == 0);
            o[k] = (uint8_t)(eq ^ x->ne);
        }
        at += rv.n;
    }
    return true;
}

/* One morsel [start, start + n) (pred_sel_drive fills morsel by morsel):
 * each piece inside one segment at segment-local rows; under an installed
 * selection a morsel it drops is not read and a mixed one keeps only its
 * own rows.  A failure is noted keyed by the morsel's first row and the
 * morsel passes no row. */
static void str_sel_fill(void* vctx, ray_col_scratch_t* sc, int64_t start, int64_t n, uint8_t* out) {
    const str_sel_ctx_t* x = (const str_sel_ctx_t*)vctx;
    const int64_t ms = start / RAY_MORSEL_ELEMS;
    if (x->wflags && x->wflags[ms] == RAY_SEL_NONE) { memset(out, 0, (size_t)n); return; }
    if (ray_scan_fail_skip(x->fail, start)) { memset(out, 0, (size_t)n); return; }
    int64_t r = start;
    const int64_t end = start + n;
    while (r < end) {
        int64_t lo = 0, hi = x->n_segs - 1;          /* the last i with off[i] <= r */
        while (lo < hi) { int64_t mid = lo + (hi - lo + 1) / 2; if (x->off[mid] <= r) lo = mid; else hi = mid - 1; }
        int64_t e = x->off[lo + 1] < end ? x->off[lo + 1] : end;
        if (!str_seg_fill(x, &x->segs[lo], sc, start, r - x->off[lo], e - r, out + (r - start))) {
            memset(out, 0, (size_t)n);
            return;
        }
        r = e;
    }
    if (x->wflags && x->wflags[ms] == RAY_SEL_MIX) {
        uint8_t keep[RAY_MORSEL_ELEMS];
        memset(keep, 0, sizeof keep);
        for (uint32_t j = x->woff[ms]; j < x->woff[ms + 1]; j++)
            if (x->widx[j] < n) keep[x->widx[j]] = 1;
        for (int64_t i = 0; i < n; i++) out[i] &= keep[i];
    }
}

static _Atomic(uint64_t) g_str_sel_runs[2];   /* test hook: [0] first conjunct, [1] refinement */
uint64_t ray_exec_test_str_sel(int within) { return atomic_load(&g_str_sel_runs[within ? 1 : 0]); }

/* A segment's codes path: a single-file, non-synthetic encoded column
 * without a published materialization (read by range otherwise). */
static ray_enccol_t* str_sel_codes_col(ray_t* o) {
    if (!o || o->type != RAY_ENCCOL || ray_enccol_decoded_peek(o)) return NULL;
    ray_enccol_t* e = ray_enccol_of(o);
    return e && e->kind == RAY_ENCCOL_SINGLE && !e->synthetic ? e : NULL;
}

/* A segment's persisted DICT (P6b): a single-file, non-synthetic encoded
 * column recording one, materialized or not (the codes read the mapping). */
static ray_enccol_t* str_sel_dict_col(ray_t* o) {
    if (!o || o->type != RAY_ENCCOL) return NULL;
    ray_enccol_t* e = ray_enccol_of(o);
    return ray_enccol_has_idict(e) ? e : NULL;
}

static ray_t* str_sel_run(ray_graph_t* g, ray_op_t* pred, int64_t nrows, ray_t* within, bool* all_pass) {
    *all_pass = false;
    if (!g || !g->table || !pred || nrows <= 0) return NULL;
    if ((pred->opcode != OP_EQ && pred->opcode != OP_NE) || pred->arity != 2) return NULL;
    ray_op_t* lhs = op_child(g, pred, 0);
    ray_op_t* rhs = op_child(g, pred, 1);
    if (!lhs || !rhs) return NULL;
    if (lhs->opcode == OP_CONST && rhs->opcode == OP_SCAN) { ray_op_t* t = lhs; lhs = rhs; rhs = t; }
    if (lhs->opcode != OP_SCAN || rhs->opcode != OP_CONST) return NULL;
    ray_op_ext_t* le = find_ext(g, lhs->id);
    ray_op_ext_t* re = find_ext(g, rhs->id);
    if (!le || !re || !re->literal || re->literal->type != -RAY_STR) return NULL;
    uint16_t stored_table_id = 0;
    memcpy(&stored_table_id, le->base.pad, sizeof(uint16_t));
    if (stored_table_id != 0) return NULL;                 /* default table only */
    ray_col_meta_t m;
    /* generation 2 only: a generation-0 column keeps its path */
    if (!ray_table_col_meta(g->table, le->sym, &m) || !m.enc) return NULL;
    const bool parted = RAY_IS_PARTED(m.type);
    if ((parted ? RAY_PARTED_BASETYPE(m.type) : m.type) != RAY_STR) return NULL;
    if (within) {
        ray_rowsel_t* wm = ray_rowsel_meta(within);
        if (!wm || wm->nrows != nrows) return NULL;
    }
    if (ray_col_bind_declines(g->table, le->sym)) return NULL;
    ray_t* berr = NULL;
    ray_col_src_t* src = ray_graph_bind_col(g, g->table, le->sym, &berr);
    if (berr) return berr;
    if (!src || src->type != RAY_STR || src->len != nrows || src->parted != parted) return NULL;

    str_sel_ctx_t x = { .cs = ray_str_ptr(re->literal), .clen = (uint32_t)ray_str_len(re->literal),
                        .ne = pred->opcode == OP_NE };
    int64_t one_off[2] = { 0, nrows };
    str_seg_sel_t one;
    ray_t* ret = NULL;
    bool any_codes = false;
    ray_t* cn = ray_sym_str(le->sym);                      /* arena-owned: the release at out is a no-op */
    x.cname = cn && !RAY_IS_ERR(cn) ? ray_str_ptr(cn) : "?";
    x.cname_len = cn && !RAY_IS_ERR(cn) ? (int)ray_str_len(cn) : 1;
    ray_scan_fail_t fail; ray_scan_fail_init(&fail);
    x.fail = &fail;

    if (!parted) {
        ray_enccol_t* de = str_sel_dict_col(src->obj);
        int64_t dc = -2;
        if (de && (ret = ray_enccol_idict_code(de, x.cs, x.clen, &dc)) != NULL) goto out;
        if (dc >= -1) {                            /* -2: no DICT after all, the route below */
            one = (str_seg_sel_t){ .rs = src, .de = de, .code = -2, .dcode = dc };
        } else {
            ray_enccol_t* e = str_sel_codes_col(src->obj);
            if (!e) goto decline;                  /* materialized: read in place on its path */
            one = (str_seg_sel_t){ .rs = src, .e = e, .code = -2 };
            if ((ret = ray_enccol_strdict_code(e, x.cs, x.clen, &one.code)) != NULL) goto out;
        }
        x.segs = &one; x.off = one_off; x.n_segs = 1;
        ray_col_src_advise(src, 0, nrows);
    } else {
        int64_t ns = 0; ray_t** segv = NULL; const int64_t* off = NULL;
        if (!ray_col_src_parted_segs(src, &ns, &segv, &off, &berr)) { ret = berr; goto out; }
        if (ns <= 0 || off[ns] != nrows) goto decline;
        x.segs = (str_seg_sel_t*)ray_calloc_raw((size_t)ns * sizeof(str_seg_sel_t));
        if (!x.segs) goto decline;
        x.off = off; x.n_segs = ns;
        for (int64_t i = 0; i < ns; i++) {
            str_seg_sel_t* s = &x.segs[i];
            if (off[i + 1] == off[i]) continue;    /* empty: never filled */
            ray_t* be = ray_col_src_bind(segv[i], &s->src);
            if (be) { ray_error_free(be); goto decline; }
            s->bound = true;
            if (s->src.parted || s->src.type != RAY_STR || s->src.len != off[i + 1] - off[i]) goto decline;
            s->rs = &s->src;
            s->code = -2;
            ray_enccol_t* de = str_sel_dict_col(segv[i]);
            int64_t dc = -2;
            if (de && (ret = ray_enccol_idict_code(de, x.cs, x.clen, &dc)) != NULL) goto out;
            if (dc >= -1) {                        /* -2: no DICT after all, the route below */
                s->de = de; s->dcode = dc;
                any_codes = true;
                continue;
            }
            s->e = str_sel_codes_col(segv[i]);
            if (s->e && (ret = ray_enccol_strdict_code(s->e, x.cs, x.clen, &s->code)) != NULL) goto out;
            any_codes |= s->e != NULL;
        }
        /* no encoded segment (a generation-0 store whose ragged partitions
         * stand in as synthetic segments): its path is unchanged */
        if (!any_codes) goto decline;
        for (int64_t i = 0; i < ns; i++)
            if (x.segs[i].bound) ray_col_src_advise(&x.segs[i].src, 0, x.segs[i].src.len);
    }
    if (within) {
        x.wflags = ray_rowsel_flags(within);
        x.woff = ray_rowsel_offsets(within);
        x.widx = ray_rowsel_idx(within);
    }
    ret = pred_sel_drive(nrows, str_sel_fill, &x, &fail, all_pass);
    if ((ret && !RAY_IS_ERR(ret)) || (!ret && *all_pass)) atomic_fetch_add(&g_str_sel_runs[within ? 1 : 0], 1);
    goto out;
decline:
    ret = NULL;
out:
    if (parted && x.segs) {
        for (int64_t i = 0; i < x.n_segs; i++)
            if (x.segs[i].bound) ray_col_src_release(&x.segs[i].src);
        ray_free_raw(x.segs);
    }
    if (cn && !RAY_IS_ERR(cn)) ray_release(cn);
    else if (cn) ray_error_free(cn);
    return ret;
}

ray_t* exec_str_to_selection(ray_graph_t* g, ray_op_t* pred, int64_t nrows, bool* all_pass) {
    return str_sel_run(g, pred, nrows, NULL, all_pass);
}

ray_t* exec_str_refine_selection(ray_graph_t* g, ray_op_t* pred, ray_t* sel, bool* all_pass) {
    *all_pass = false;
    if (!sel) return NULL;
    ray_rowsel_t* m = ray_rowsel_meta(sel);
    return m ? str_sel_run(g, pred, m->nrows, sel, all_pass) : NULL;
}

static int idx_filter_decode(ray_graph_t* g, ray_op_t* pred_op, unsigned want,
                             ray_t** out_col, uint16_t* out_cmp_op,
                             int64_t* out_key_i, double* out_key_f,
                             int* out_is_float, ray_col_meta_t* out_meta);

/* The scanned column of a conjunct idx_filter_decode accepted. */
static int64_t idx_conj_sym(ray_graph_t* g, ray_op_t* pred) {
    ray_op_t* l = op_child(g, pred, 0);
    ray_op_ext_t* x = l ? find_ext(g, l->id) : NULL;
    return x ? x->sym : -1;
}

/* The eq+range conjunction's one null rule, for the metadata pass and the
 * decode loop alike: a generation-2 column (m->enc, published or not) by its
 * exact directory null count, a text column always (ray_vec_may_have_nulls),
 * a plain column by ray_vec_may_have_nulls on its vector (borrowed from the
 * table: reading it decodes nothing). */
static bool idx_conj_may_null(ray_graph_t* g, const ray_col_meta_t* m, int64_t sym) {
    if (m->type == RAY_SYM || m->type == RAY_STR) return true;
    if (m->enc) return m->null_count != 0;
    ray_t* c = g->table ? idx_decode_col(g->table, sym) : NULL;
    return !c || ray_vec_may_have_nulls(c);
}

/* The eq+range conjunction's decode loop decided from metadata: no float
 * literal, one equality on a hash-eligible column, and ordered compares on
 * one other null-free column.  Lets a generation-2 equality column probe
 * its lazy hash only for a shape that would use it. */
static bool idx_eqrange_meta_ok(ray_graph_t* g, ray_op_t** conj, int ncj) {
    int64_t eq_sym = -1, rg_sym = -1;
    for (int ci = 0; ci < ncj; ci++) {
        uint16_t cop = 0; ray_col_meta_t cm;
        int64_t ki = 0; double kf = 0.0; int isf = 0;
        if (!idx_filter_decode(g, conj[ci], IDXW_NONE, NULL, &cop, &ki, &kf, &isf, &cm) || isf)
            return false;
        int64_t sym = idx_conj_sym(g, conj[ci]);
        if (sym < 0) return false;
        if (cop == OP_EQ && eq_sym < 0 &&
            (cm.index_kind == RAY_IDX_HASH || idx_lazy_meta_ok(&cm)) &&
            (cm.type == RAY_SYM ? ki != 0 : !idx_conj_may_null(g, &cm, sym))) {
            eq_sym = sym;
        } else if (cop == OP_GE || cop == OP_GT || cop == OP_LE || cop == OP_LT) {
            if (idx_conj_may_null(g, &cm, sym)) return false;
            if (rg_sym < 0) rg_sym = sym;
            else if (rg_sym != sym) return false;
        } else return false;
    }
    return eq_sym >= 0 && rg_sym >= 0 && rg_sym != eq_sym;
}

/* The single-compare consult on a generation-2 column without a published
 * materialization (finding C1), from its directory: the zone class (recorded
 * ZONE), an EQ key outside the chunk zone's range, then the PI lazy HASH,
 * which materializes the column only when the accumulated probes pay for
 * the build.  Only a column idx_enc_from_dir admits comes here: recorded
 * kind none, ZONE or CHUNK_ZONE, or a recorded HASH, SORT, BLOOM or PART
 * under a shape that does not use it (idx_enc_cmp_uses_index); a shape
 * that uses it is materialized and answered by the generation-0 block from
 * its index.
 * 1: every row passes; 2: *sel is the selection; 0: undecided (the fused
 * scan answers, by range); -1: *err owned.  Mirrors the generation-0 steps
 * of the OP_FILTER arm: the same counters in the same order, the same null
 * and float conditions (m->null_count is exact for generation 2). */
static int idx_filter_enc_consult(ray_t* enc, const ray_col_meta_t* m, uint16_t cmp_op,
                                  int64_t key_i, double key_f, int is_float,
                                  int64_t nrows, ray_t** sel, ray_t** err) {
    *sel = NULL; *err = NULL;
    if (m->index_kind == RAY_IDX_ZONE) ray_idx_consults[IDX_SITE_FILTER_ZONE]++;
    switch (ray_enc_zone_class(enc, cmp_op, key_i, key_f)) {
    case RAY_ZONE_ALL:
        ray_idx_hits[IDX_SITE_FILTER_ZONE]++;
        ray_idx_note_use_enc(enc);
        return 1;
    case RAY_ZONE_NONE:
        ray_idx_hits[IDX_SITE_FILTER_ZONE]++;
        ray_idx_note_use_enc(enc);
        *sel = ray_index_empty_rowsel(nrows);
        return *sel ? 2 : 0;
    default: break;
    }
    if (cmp_op != OP_EQ || is_float || m->type == RAY_SYM || m->null_count != 0) return 0;
    if (ray_enc_zone_excludes(enc, key_i)) {
        ray_idx_consults[IDX_SITE_FILTER_ZONE]++;
        *sel = ray_index_empty_rowsel(nrows);
        if (*sel) { ray_idx_hits[IDX_SITE_FILTER_ZONE]++; ray_idx_note_use_enc(enc); return 2; }
    }
    ray_t* lazy = NULL;
    ray_t* le = idx_lazy_err(ray_idx_lazy_hash_enc(enc, &lazy));
    if (le) { *err = le; return -1; }
    if (!lazy) return 0;
    ray_idx_consults[IDX_SITE_FILTER_HASH]++;
    *sel = ray_index_hash_eq_rowsel(lazy, key_i);
    if (*sel) { ray_idx_hits[IDX_SITE_FILTER_HASH]++; ray_idx_note_use(lazy); }
    ray_release(lazy);
    return *sel ? 2 : 0;
}

/* The IN consult's twin: every element outside the chunk zone's range, then
 * the lazy HASH for a small integer set. */
static int idx_filter_enc_in_consult(ray_t* enc, const ray_col_meta_t* m, ray_t* set,
                                     int64_t nrows, ray_t** sel, ray_t** err) {
    *sel = NULL; *err = NULL;
    if (m->type == RAY_F32 || m->type == RAY_F64 || m->null_count != 0) return 0;
    if (ray_enc_zone_excludes_set(enc, set)) {
        ray_idx_consults[IDX_SITE_FILTER_ZONE]++;
        *sel = ray_index_empty_rowsel(nrows);
        if (*sel) { ray_idx_hits[IDX_SITE_FILTER_ZONE]++; ray_idx_note_use_enc(enc); return 2; }
    }
    if (!idx_lazy_set_ok_len(m->len, set)) return 0;
    ray_t* lazy = NULL;
    ray_t* le = idx_lazy_err(ray_idx_lazy_hash_enc(enc, &lazy));
    if (le) { *err = le; return -1; }
    if (!lazy) return 0;
    ray_idx_consults[IDX_SITE_IN]++;
    *sel = ray_index_in_rowsel(lazy, set);
    if (*sel) { ray_idx_hits[IDX_SITE_IN]++; ray_idx_note_use(lazy); }
    ray_release(lazy);
    return *sel ? 2 : 0;
}

typedef struct { in_worker_ctx_t* ic; } in_sel_fill_ctx_t;

static void in_sel_fill(void* vctx, ray_col_scratch_t* sc, int64_t start, int64_t n, uint8_t* out) {
    (void)sc;
    in_sel_fill_ctx_t* c = (in_sel_fill_ctx_t*)vctx;
    in_worker_ctx_t local = *c->ic;        /* per-call copy: morsel-local sink */
    local.ob      = out;                    /* element i writes out[i - start] */
    local.ob_base = start;
    exec_in_worker(&local, 0, start, start + n);
}

/* The range-reading fill: rows [start, start + n) of the bound source,
 * one range view at a time (a view ends at a generation-2 vector
 * boundary), each through exec_in_worker with cd / cd_base at the view.
 * A failed read is noted keyed by the morsel's first row (a global row:
 * the lowest failure wins) and the rest of the morsel is not-selected.
 * Today a morsel is exactly one generation-2 vector (both RAY_MORSEL_ELEMS =
 * RAY_ENC_VEC rows, both aligned at row 0), so the loop runs once; it stays
 * general for a source whose views end inside a morsel: each view is read
 * at its own offset (cd_base = ob_base = start + at), and the failure key
 * stays the morsel's first row, the key every view of the morsel shares. */
typedef struct {
    in_worker_ctx_t* ic;
    ray_col_src_t*   src;
    ray_scan_fail_t* fail;
    int64_t          key_base;         /* added to failure keys: a parted segment's first row */
    /* Membership bounds (integer and SYM columns): the probe values, sorted
     * ascending and distinct (keys == NULL: no bounds, every morsel read),
     * and whether "" (SYM id 0) is one of them. */
    const int64_t*   keys;
    int              nkeys;
    bool             has_zero;
    bool             negate;
    bool             sym_zero_fails;   /* SYM: id-0 rows fail both in and not in, as in the kernel */
    /* The float twin (a float column or set: the kernel compares doubles
     * with ==, so -0.0 matches +0.0 and a NaN matches nothing): the probe
     * as doubles, sorted, NaN dropped (fkeys == NULL: no float bounds);
     * col_float: the statistics are F32 / F64 bit patterns, else integer
     * values widened to double (monotone, so a value's double stays inside
     * the widened [min, max]). */
    const double*    fkeys;
    int              nfkeys;
    bool             col_float;
} in_sel_rfill_ctx_t;

/* The membership rule over a range from its statistics (min / max over its
 * non-null values, SYM ids nonzero; nn its null rows): when no probe value
 * lies in [min, max], `in` passes no row (a null row passes neither) and
 * `not in` passes every row, but only without nulls (a null row is not
 * selected by `not in` here).  A SYM "" member is decided by the null
 * count: any id-0 row leaves the range undecided.  1 / 0 / -1. */
static int in_bounds_decide(const in_sel_rfill_ctx_t* c, int64_t mn, int64_t mx, bool has_mm, int64_t nn) {
    if (!has_mm || mn > mx) return -1;
    if (c->has_zero && nn > 0) return -1;
    int lo = 0, hi = c->nkeys;
    while (lo < hi) { int mid = lo + (hi - lo) / 2; if (c->keys[mid] < mn) lo = mid + 1; else hi = mid; }
    if (lo < c->nkeys && c->keys[lo] <= mx) return -1;
    if (!c->negate) return 0;
    return nn == 0 ? 1 : -1;
}

/* in_bounds_decide over double bounds (the float twin's probe).  A NaN
 * bound is undecided; -0.0 and +0.0 compare equal, as in the kernel. */
static int in_bounds_decide_f(const in_sel_rfill_ctx_t* c, double mn, double mx, bool has_mm, int64_t nn) {
    if (!has_mm || !(mn <= mx)) return -1;
    int lo = 0, hi = c->nfkeys;
    while (lo < hi) { int mid = lo + (hi - lo) / 2; if (c->fkeys[mid] < mn) lo = mid + 1; else hi = mid; }
    if (lo < c->nfkeys && c->fkeys[lo] <= mx) return -1;
    if (!c->negate) return 0;
    return nn == 0 ? 1 : -1;
}
static inline double in_stat_f(const in_sel_rfill_ctx_t* c, int64_t v) {
    if (!c->col_float) return (double)v;
    double d; memcpy(&d, &v, 8);
    return d;
}

/* Morsel [start, start + n) answered without a range read: from the chunk's
 * directory, then the vector's descriptor, then the vector's view
 * (ray_enc_eval_in on ids / values, inverted for `not in` with null rows
 * cleared).  1 answered into out, 0 read it, -1 an owned *err (the view's
 * validation, as the decode would report it). */
static int in_sel_bounds(const in_sel_rfill_ctx_t* c, int64_t start, int64_t n, uint8_t* out, ray_t** err) {
    if (c->fkeys) {                                     /* floats: bounds only, no view evaluator */
        int d = -1;
        if ((start >> 16) == ((start + n - 1) >> 16)) {
            ray_enc_chunk_stat_t st;
            if (ray_col_src_chunk_stat(c->src, start >> 16, &st))
                d = in_bounds_decide_f(c, in_stat_f(c, st.min), in_stat_f(c, st.max), st.has_minmax,
                                       (int64_t)st.null_count);
        }
        if (d < 0 && start / RAY_ENC_VEC == (start + n - 1) / RAY_ENC_VEC) {
            int64_t mn, mx; uint32_t nn;
            if (ray_col_src_vec_stat(c->src, start / RAY_ENC_VEC, &mn, &mx, &nn))
                d = in_bounds_decide_f(c, in_stat_f(c, mn), in_stat_f(c, mx), true, nn);
        }
        if (d >= 0) { memset(out, d, (size_t)n); return 1; }
        return 0;
    }
    if (!c->keys) return 0;
    int d = -1;
    if ((start >> 16) == ((start + n - 1) >> 16)) {
        ray_enc_chunk_stat_t st;
        if (ray_col_src_chunk_stat(c->src, start >> 16, &st))
            d = in_bounds_decide(c, st.min, st.max, st.has_minmax, (int64_t)st.null_count);
    }
    if (d < 0 && start / RAY_ENC_VEC == (start + n - 1) / RAY_ENC_VEC) {
        int64_t mn, mx; uint32_t nn;
        if (ray_col_src_vec_stat(c->src, start / RAY_ENC_VEC, &mn, &mx, &nn))
            d = in_bounds_decide(c, mn, mx, true, nn);
    }
    if (d >= 0) { memset(out, d, (size_t)n); return 1; }
    if (start % RAY_ENC_VEC) return 0;
    ray_enc_vview_t v;                                  /* on this task's stack */
    int vr = ray_col_src_vview(c->src, start / RAY_ENC_VEC, &v, err);
    if (vr <= 0 || v.n != n) return vr < 0 ? -1 : 0;
    if (!ray_enc_eval_in(&v, c->keys, c->nkeys, 0, out)) return 0;
    if (c->negate) {
        for (int64_t i = 0; i < n; i++) out[i] ^= 1;
        ray_enc_mask_nulls(&v, 0, out);
    }
    if (v.type == RAY_SYM && c->sym_zero_fails && v.n_nulls > 0) {
        uint8_t zero[RAY_ENC_VEC];                      /* the view's id-0 rows */
        const int64_t z = 0;
        if (!ray_enc_eval_in(&v, &z, 1, 0, zero)) return 0;
        for (int64_t i = 0; i < n; i++) out[i] &= (uint8_t)(zero[i] ^ 1);
    }
    return 1;
}

static int in_key_cmp(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

static int in_fkey_cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static void in_sel_rfill(void* vctx, ray_col_scratch_t* sc, int64_t start, int64_t n, uint8_t* out) {
    in_sel_rfill_ctx_t* c = (in_sel_rfill_ctx_t*)vctx;
    const int64_t key = c->key_base + start;
    if (ray_scan_fail_skip(c->fail, key)) { memset(out, 0, (size_t)n); return; }
    ray_t* berr = NULL;
    int b = in_sel_bounds(c, start, n, out, &berr);
    if (b > 0) return;
    if (b < 0) { ray_scan_fail_note(c->fail, key, berr); memset(out, 0, (size_t)n); return; }
    int64_t at = 0;
    while (at < n) {
        ray_col_range_t v;
        ray_t* e = ray_col_range(c->src, start + at, n - at, sc, &v);
        if (!e && v.n <= 0)
            e = ray_error("range", "in selection: empty range read at row %lld", (long long)(start + at));
        if (e) {
            ray_scan_fail_note(c->fail, key, e);
            memset(out + at, 0, (size_t)(n - at));
            return;
        }
        in_worker_ctx_t local = *c->ic;
        local.ob = out + at;   local.ob_base = start + at;
        local.cd = v.data;     local.cd_base = start + at;
        local.cattrs = v.attrs;
        exec_in_worker(&local, 0, start + at, start + at + v.n);
        at += v.n;
    }
}

/* The range-reading fill's context over bound source `src` with probe ic:
 * the membership bounds, the probe values sorted once (integer and SYM
 * columns as int64; a float column or set as doubles), in kstack / fstack
 * when they hold 32 values or fewer.  A failed allocation leaves those
 * bounds off (every morsel read).  in_rfill_free releases them. */
static void in_rfill_setup(in_sel_rfill_ctx_t* f, in_worker_ctx_t* ic, ray_col_src_t* src,
                           ray_scan_fail_t* fail, bool negate, int64_t* kstack, double* fstack) {
    *f = (in_sel_rfill_ctx_t){ .ic = ic, .src = src, .fail = fail, .negate = negate };
    if (!ic->use_double && ic->sv_len <= INT32_MAX) {
        int64_t* keys = ic->sv_len <= 32 ? kstack : (int64_t*)ray_alloc_raw((size_t)ic->sv_len * sizeof(int64_t));
        if (keys) {
            if (ic->sv_len) memcpy(keys, ic->svi, (size_t)ic->sv_len * sizeof(int64_t));
            qsort(keys, (size_t)ic->sv_len, sizeof(int64_t), in_key_cmp);
            int64_t nk = 0;
            for (int64_t j = 0; j < ic->sv_len; j++)
                if (nk == 0 || keys[nk - 1] != keys[j]) keys[nk++] = keys[j];
            f->keys = keys;
            f->nkeys = (int)nk;
            f->has_zero = src->type == RAY_SYM && nk > 0 && keys[0] == 0;
            /* a SYM view's id-0 rows answer as the kernel's: 0 under the
             * verdict LUT and the null-checked loop (null rows fail both
             * in and not in, #739), (0 in set) ^ negate under the
             * unchecked loop */
            f->sym_zero_fails = ic->symlut || ic->col_has_nulls;
        }
    }
    if (ic->use_double && ic->sv_len <= INT32_MAX) {
        double* fkeys = ic->sv_len <= 32 ? fstack : (double*)ray_alloc_raw((size_t)ic->sv_len * sizeof(double));
        if (fkeys) {
            int64_t nk = 0;
            for (int64_t j = 0; j < ic->sv_len; j++)
                if (ic->svf[j] == ic->svf[j]) fkeys[nk++] = ic->svf[j];   /* a NaN member matches nothing */
            qsort(fkeys, (size_t)nk, sizeof(double), in_fkey_cmp);
            f->fkeys = fkeys;
            f->nfkeys = (int)nk;
            f->col_float = src->type == RAY_F32 || src->type == RAY_F64;
        }
    }
}

static void in_rfill_free(in_sel_rfill_ctx_t* f, int64_t* kstack, double* fstack) {
    if (f->keys && f->keys != kstack) ray_free_raw((void*)f->keys);
    if (f->fkeys && f->fkeys != fstack) ray_free_raw((void*)f->fkeys);
    f->keys = NULL; f->fkeys = NULL;
}

/* One segment of a parted `in` selection: its own bound source and probe
 * (a SYM segment translates the set into its own domain, an integer DICT
 * segment's views decide on its own dictionary), read in place when ready
 * (a plain segment, or a published materialization) and by range
 * otherwise. */
typedef struct {
    ray_col_src_t      src;
    bool               bound, ready;
    in_worker_ctx_t    ic;
    ray_t*             sv_hdr;
    ray_t*             lut_hdr;
    double             svf_stack[32];
    int64_t            svi_stack[32];
    int64_t            kstack[32];
    double             fstack[32];
    in_sel_fill_ctx_t  f;
    in_sel_rfill_ctx_t rf;
} in_seg_sel_t;

typedef struct {
    in_seg_sel_t*  segs;
    const int64_t* off;      /* segment i: rows [off[i], off[i + 1]) */
    int64_t        n_segs;
    /* An installed selection the result is ANDed with (a later conjunct),
     * or NULL: its morsel flags, offsets and ids. */
    const uint8_t*  wflags;
    const uint32_t* woff;
    const uint16_t* widx;
} in_sel_parted_ctx_t;

/* Rows [start, start + n) of a parted table (one morsel: pred_sel_drive
 * fills morsel by morsel): each piece inside one segment goes to that
 * segment's fill at segment-local rows (a morsel straddling a segment
 * boundary is cut there).  Under an installed selection a morsel it drops
 * is not read, and a mixed one keeps only its own rows. */
static void in_sel_parted_fill(void* vctx, ray_col_scratch_t* sc, int64_t start, int64_t n, uint8_t* out) {
    const in_sel_parted_ctx_t* c = (const in_sel_parted_ctx_t*)vctx;
    const int64_t ms = start / RAY_MORSEL_ELEMS;
    if (c->wflags && c->wflags[ms] == RAY_SEL_NONE) { memset(out, 0, (size_t)n); return; }
    int64_t r = start;
    const int64_t end = start + n;
    while (r < end) {
        int64_t lo = 0, hi = c->n_segs - 1;          /* the last i with off[i] <= r: never empty */
        while (lo < hi) { int64_t mid = lo + (hi - lo + 1) / 2; if (c->off[mid] <= r) lo = mid; else hi = mid - 1; }
        in_seg_sel_t* s = &c->segs[lo];
        int64_t e = c->off[lo + 1] < end ? c->off[lo + 1] : end;
        if (s->ready) in_sel_fill(&s->f, sc, r - c->off[lo], e - r, out + (r - start));
        else          in_sel_rfill(&s->rf, sc, r - c->off[lo], e - r, out + (r - start));
        r = e;
    }
    if (c->wflags && c->wflags[ms] == RAY_SEL_MIX) {
        uint8_t keep[RAY_MORSEL_ELEMS];
        memset(keep, 0, sizeof keep);
        for (uint32_t j = c->woff[ms]; j < c->woff[ms + 1]; j++)
            if (c->widx[j] < n) keep[c->widx[j]] = 1;
        for (int64_t i = 0; i < n; i++) out[i] &= keep[i];
    }
}

static void in_seg_sel_free(in_seg_sel_t* segs, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        in_seg_sel_t* s = &segs[i];
        if (!s->ready) in_rfill_free(&s->rf, s->kstack, s->fstack);
        if (s->sv_hdr) ray_free(s->sv_hdr);
        if (s->lut_hdr) ray_free(s->lut_hdr);
        if (s->bound) ray_col_src_release(&s->src);
    }
    ray_free_raw(segs);
}

/* exec_in_to_selection over a generation-2 parted column (src, bound over
 * the RAY_ENCCOL parted object): the segments evaluated each by its own
 * source and probe, as the single-file path reads one, into one selection
 * over the table's rows; the column is never materialized.  A generation-0
 * parted wrapper is not taken (its route is unchanged).  With `within` (a
 * selection over the same rows, borrowed) the result is its AND with that
 * one.  NULL (the caller's fallback) for a shape a segment declines (STR,
 * GUID), an owned error, or the selection. */
static ray_t* in_sel_parted(ray_col_src_t* src, ray_t* set_lit, bool negate, int64_t nrows, ray_t* within,
                            bool* all_pass) {
    *all_pass = false;
    if (!src->obj || src->obj->type != RAY_ENCCOL) return NULL;
    int64_t ns = 0; ray_t** segv = NULL; const int64_t* off = NULL; ray_t* err = NULL;
    if (!ray_col_src_parted_segs(src, &ns, &segv, &off, &err)) return err;
    if (ns <= 0 || off[ns] != nrows) return NULL;
    in_seg_sel_t* segs = (in_seg_sel_t*)ray_calloc_raw((size_t)ns * sizeof(in_seg_sel_t));
    if (!segs) return NULL;
    ray_scan_fail_t fail; ray_scan_fail_init(&fail);
    for (int64_t i = 0; i < ns; i++) {
        in_seg_sel_t* s = &segs[i];
        if (off[i + 1] == off[i]) { s->ready = true; continue; }   /* empty: never filled */
        ray_t* be = ray_col_src_bind(segv[i], &s->src);
        if (be) { ray_error_free(be); in_seg_sel_free(segs, ns); return NULL; }
        s->bound = true;
        if (s->src.parted || s->src.len != off[i + 1] - off[i]) { in_seg_sel_free(segs, ns); return NULL; }
        ray_t* col = ray_col_src_ready(&s->src);
        in_ctx_status_t st;
        if (col) {
            if (ray_is_atom(col) || RAY_IS_PARTED(col->type)) { in_seg_sel_free(segs, ns); return NULL; }
            s->ready = true;
            st = in_build_worker_ctx(col, set_lit, negate, s->svf_stack, s->svi_stack, &s->ic, &s->sv_hdr, &s->lut_hdr);
            s->f.ic = &s->ic;
        } else {
            st = in_build_worker_ctx_meta(s->src.type, s->src.attrs, s->src.may_null, s->src.dom, set_lit, negate,
                                          s->svf_stack, s->svi_stack, &s->ic, &s->sv_hdr, &s->lut_hdr);
            if (st == IN_CTX_OK) {
                in_rfill_setup(&s->rf, &s->ic, &s->src, &fail, negate, s->kstack, s->fstack);
                s->rf.key_base = off[i];
                ray_col_src_advise(&s->src, 0, s->src.len);
            }
        }
        if (st != IN_CTX_OK) { s->ready = true; in_seg_sel_free(segs, ns); return NULL; }   /* STR / GUID / OOM */
    }
    in_sel_parted_ctx_t pc = { segs, off, ns, NULL, NULL, NULL };
    if (within) {
        ray_rowsel_t* wm = ray_rowsel_meta(within);
        if (!wm || wm->nrows != nrows) { in_seg_sel_free(segs, ns); return NULL; }
        pc.wflags = ray_rowsel_flags(within);
        pc.woff = ray_rowsel_offsets(within);
        pc.widx = ray_rowsel_idx(within);
    }
    ray_t* sel = pred_sel_drive(nrows, in_sel_parted_fill, &pc, &fail, all_pass);
    in_seg_sel_free(segs, ns);
    return sel;
}

/* A later `in` conjunct over a single-file generation-2 column read by
 * range (exec_in_to_selection's range fill), ANDed with the installed
 * selection `within` inside the fill (in_sel_parted_fill over one
 * segment: a morsel `within` drops is not read).  NULL when the column is
 * read in place (a published materialization: the bool path, as before)
 * or the shape declines (STR, GUID, OOM); an owned error, or the
 * selection. */
static _Atomic(uint64_t) g_in_single_within;   /* test hook */
uint64_t ray_exec_test_in_single_within(void) { return atomic_load(&g_in_single_within); }
static ray_t* in_sel_single_within(ray_col_src_t* src, ray_t* set_lit, bool negate, int64_t nrows,
                                   ray_t* within, bool* all_pass) {
    *all_pass = false;
    if (ray_col_src_ready(src)) return NULL;
    ray_rowsel_t* wm = ray_rowsel_meta(within);
    if (!wm || wm->nrows != nrows) return NULL;
    in_seg_sel_t* seg = (in_seg_sel_t*)ray_calloc_raw(sizeof(in_seg_sel_t));
    if (!seg) return NULL;
    ray_scan_fail_t fail; ray_scan_fail_init(&fail);
    if (in_build_worker_ctx_meta(src->type, src->attrs, src->may_null, src->dom, set_lit, negate,
                                 seg->svf_stack, seg->svi_stack, &seg->ic, &seg->sv_hdr, &seg->lut_hdr) != IN_CTX_OK) {
        seg->ready = true;                         /* nothing set up to free but the headers */
        in_seg_sel_free(seg, 1);
        return NULL;
    }
    in_rfill_setup(&seg->rf, &seg->ic, src, &fail, negate, seg->kstack, seg->fstack);
    ray_col_src_advise(src, 0, nrows);
    const int64_t off[2] = { 0, nrows };
    in_sel_parted_ctx_t pc = { seg, off, 1, ray_rowsel_flags(within), ray_rowsel_offsets(within),
                               ray_rowsel_idx(within) };
    ray_t* sel = pred_sel_drive(nrows, in_sel_parted_fill, &pc, &fail, all_pass);
    in_seg_sel_free(seg, 1);                       /* src is the graph's: not bound here */
    if ((sel && !RAY_IS_ERR(sel)) || (!sel && *all_pass)) atomic_fetch_add(&g_in_single_within, 1);
    return sel;
}

/* A parted `in` / `not in` filter's selection, for a FILTER that already
 * holds one (a later conjunct): the generation-2 route above, or NULL when
 * it is not taken (any other column keeps the bool path). */
static ray_t* exec_in_parted_selection(ray_graph_t* g, ray_op_t* pred, int64_t nrows, ray_t* within,
                                       bool* all_pass);

ray_t* exec_in_to_selection(ray_graph_t* g, ray_op_t* pred, int64_t nrows,
                            bool* all_pass) {
    *all_pass = false;
    if (pred->opcode != OP_IN && pred->opcode != OP_NOT_IN) return NULL;
    if (pred->arity != 2) return NULL;

    /* Shape: (SCAN flat-col) IN / NOT-IN (CONST set) over the default
     * table.  The column is bound, never decoded: a plain column (or a
     * generation-2 one whose materialization is published) is read in
     * place, a generation-2 one by range. */
    ray_op_t* lhs = op_child(g, pred, 0);
    ray_op_t* rhs = op_child(g, pred, 1);
    if (!lhs || lhs->opcode != OP_SCAN || !rhs || rhs->opcode != OP_CONST) return NULL;
    ray_op_ext_t* le = find_ext(g, lhs->id);
    ray_op_ext_t* re = find_ext(g, rhs->id);
    if (!le || !re || !re->literal) return NULL;
    uint16_t stored_table_id = 0;
    memcpy(&stored_table_id, le->base.pad, sizeof(uint16_t));
    if (stored_table_id != 0 || !g->table) return NULL;   /* default table only, as idx_filter_in_decode */
    ray_col_meta_t m;
    if (!ray_table_col_meta(g->table, le->sym, &m) || m.type == RAY_MAPCOMMON) return NULL;
    if (RAY_IS_PARTED(m.type)) return exec_in_parted_selection(g, pred, nrows, NULL, all_pass);
    if (ray_col_bind_declines(g->table, le->sym)) return NULL;
    ray_t* berr = NULL;
    ray_col_src_t* src = ray_graph_bind_col(g, g->table, le->sym, &berr);
    if (berr) return berr;
    if (!src || src->parted || src->len != nrows) return NULL;
    ray_t* set_lit = re->literal;

    bool negate = (pred->opcode == OP_NOT_IN);
    double  svf_stack[32];
    int64_t svi_stack[32];
    in_worker_ctx_t ic;
    ray_t* sv_hdr = NULL;
    ray_t* lut_hdr = NULL;
    ray_scan_fail_t fail; ray_scan_fail_init(&fail);
    ray_t* sel;
    ray_t* col = ray_col_src_ready(src);
    if (col) {
        if (ray_is_atom(col) || RAY_IS_PARTED(col->type)) return NULL;
        if (in_build_worker_ctx(col, set_lit, negate, svf_stack, svi_stack,
                                &ic, &sv_hdr, &lut_hdr) != IN_CTX_OK)
            return NULL;  /* STR / OOM → bool-path fallback */
        in_sel_fill_ctx_t fctx = { &ic };
        sel = pred_sel_drive(nrows, in_sel_fill, &fctx, &fail, all_pass);
    } else {
        if (in_build_worker_ctx_meta(src->type, src->attrs, src->may_null, src->dom,
                                     set_lit, negate, svf_stack, svi_stack,
                                     &ic, &sv_hdr, &lut_hdr) != IN_CTX_OK)
            return NULL;  /* STR / GUID / OOM → bool-path fallback */
        ray_col_src_advise(src, 0, nrows);
        in_sel_rfill_ctx_t fctx;
        int64_t kstack[32];
        double fstack[32];
        in_rfill_setup(&fctx, &ic, src, &fail, negate, kstack, fstack);
        sel = pred_sel_drive(nrows, in_sel_rfill, &fctx, &fail, all_pass);
        in_rfill_free(&fctx, kstack, fstack);
    }

    if (sv_hdr) ray_free(sv_hdr);
    if (lut_hdr) ray_free(lut_hdr);
    return sel;
}

static ray_t* exec_in_parted_selection(ray_graph_t* g, ray_op_t* pred, int64_t nrows, ray_t* within,
                                       bool* all_pass) {
    *all_pass = false;
    if (!g || !g->table || !pred || nrows <= 0) return NULL;
    if ((pred->opcode != OP_IN && pred->opcode != OP_NOT_IN) || pred->arity != 2) return NULL;
    ray_op_t* lhs = op_child(g, pred, 0);
    ray_op_t* rhs = op_child(g, pred, 1);
    if (!lhs || lhs->opcode != OP_SCAN || !rhs || rhs->opcode != OP_CONST) return NULL;
    ray_op_ext_t* le = find_ext(g, lhs->id);
    ray_op_ext_t* re = find_ext(g, rhs->id);
    if (!le || !re || !re->literal) return NULL;
    uint16_t stored_table_id = 0;
    memcpy(&stored_table_id, le->base.pad, sizeof(uint16_t));
    if (stored_table_id != 0) return NULL;
    ray_col_meta_t m;
    /* generation 2 only: a generation-0 column is never bound here; a
     * single-file one only as a later conjunct (within: the first one
     * takes exec_in_to_selection's range fill) */
    if (!ray_table_col_meta(g->table, le->sym, &m) || !m.enc) return NULL;
    if (!RAY_IS_PARTED(m.type) && (!within || m.type == RAY_MAPCOMMON)) return NULL;
    if (ray_col_bind_declines(g->table, le->sym)) return NULL;
    ray_t* berr = NULL;
    ray_col_src_t* src = ray_graph_bind_col(g, g->table, le->sym, &berr);
    if (berr) return berr;
    if (!src || src->len != nrows) return NULL;
    if (!src->parted) return in_sel_single_within(src, re->literal, pred->opcode == OP_NOT_IN, nrows, within, all_pass);
    return in_sel_parted(src, re->literal, pred->opcode == OP_NOT_IN, nrows, within, all_pass);
}

static int idx_filter_decode(ray_graph_t* g, ray_op_t* pred_op, unsigned want,
                             ray_t** out_col, uint16_t* out_cmp_op,
                             int64_t* out_key_i, double* out_key_f,
                             int* out_is_float, ray_col_meta_t* out_meta);

/* ---- Slice-group fusion probe (FILTER(in/eq on c) + GROUP(by c)) ----
 *
 * Called from the select-by pipeline (query.c) INSTEAD of executing the
 * WHERE filter, after ray_optimize.  When the compiled filter's predicate
 * is exactly membership on the single bare group-key column — (in c SET)
 * or (== c K), no other conjuncts, no chained filter — and that column
 * carries a fresh hash or part index, the surviving groups are exactly the
 * key set and each group's rows are its full index slice: resolve the
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
    ray_idx_kind_t kind = ray_index_kind(by_col);
    if (kind != RAY_IDX_HASH && kind != RAY_IDX_PART) return false;
    ray_op_t* pred = op_child(g, root, 1);
    if (!pred) return false;

    ray_t* keys = NULL;      /* borrowed (CONST literal) */
    ray_t* eq_atom = NULL;   /* owned (built for the eq shape) */
    if (pred->opcode == OP_IN) {   /* NOT_IN also decodes — reject it */
        ray_t* c = NULL; ray_t* set = NULL;
        if (idx_filter_in_decode(g, pred, IDXW_INDEX, &c, &set, NULL) && c == by_col)
            keys = set;
    }
    if (!keys) {
        ray_t* c = NULL; uint16_t cop = 0;
        int64_t ki = 0; double kf = 0.0; int isf = 0;
        if (idx_filter_decode(g, pred, IDXW_INDEX, &c, &cop, &ki, &kf, &isf, NULL) &&
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
    int64_t k = ray_index_sym_slices(by_col, keys, &sl, &shdr);
    if (eq_atom) ray_release(eq_atom);
    if (k < 0) return false;
    ray_idx_hits[IDX_SITE_GROUP_SLICE]++;
    ray_idx_note_use(by_col);
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

static int idx_filter_decode(ray_graph_t* g, ray_op_t* pred_op, unsigned want,
                             ray_t** out_col, uint16_t* out_cmp_op,
                             int64_t* out_key_i, double* out_key_f,
                             int* out_is_float, ray_col_meta_t* out_meta) {
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
    ray_col_meta_t m;
    if (!ray_table_col_meta(tbl, lext->sym, &m)) return 0;
    if (RAY_IS_PARTED(m.type) || m.type == RAY_MAPCOMMON) return 0;
    if (!idx_want_ok(&m, want)) return 0;

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
    int col_is_float = (m.type == RAY_F32 || m.type == RAY_F64);
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

    if (out_meta) *out_meta = m;
    if (out_col) {
        ray_t* col = idx_decode_col(tbl, lext->sym);
        if (!col) return 0;
        *out_col  = col;
    }
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
 * On success, *out_col (when non-NULL; NULL decodes nothing) is the column
 * vec, *out_meta (when non-NULL) its metadata, and *out_set_lit is the CONST
 * literal vec (borrowed from the ext, valid for the lifetime of the graph).
 * Returns 1 on success, 0 if the shape is not matched. */
static int idx_filter_in_decode(ray_graph_t* g, ray_op_t* pred_op, unsigned want,
                                ray_t** out_col, ray_t** out_set_lit, ray_col_meta_t* out_meta) {
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
    ray_col_meta_t m;
    if (!ray_table_col_meta(tbl, lext->sym, &m)) return 0;
    if (RAY_IS_PARTED(m.type) || m.type == RAY_MAPCOMMON) return 0;
    if (!idx_want_ok(&m, want)) return 0;
    if (out_meta) *out_meta = m;
    if (out_col) {
        ray_t* col = idx_decode_col(tbl, lext->sym);
        if (!col) return 0;
        *out_col = col;
    }
    *out_set_lit = rext->literal;
    return 1;
}

/* True when a GROUP node groups by at least one key. */
static inline bool group_has_keys(ray_graph_t* g, ray_op_t* group_op) {
    ray_op_ext_t* gx = find_ext(g, group_op->id);
    return gx && gx->n_keys > 0;
}

/* True when every leaf of the op tree under `id` is a constant or, with
 * `reduced`, sits under a reduction. */
static bool op_tree_leaves(ray_graph_t* g, uint32_t id, bool reduced, bool atoms_only, int depth) {
    if (id >= g->node_count || depth > 64) return false;
    ray_op_t* n = &g->nodes[id];
    switch (n->opcode) {
    case OP_CONST: {
        ray_op_ext_t* x = find_ext(g, id);
        return !atoms_only || (x && x->literal && ray_is_atom(x->literal));
    }
    case OP_SCAN:  return false;
    case OP_SUM: case OP_PROD: case OP_ALL: case OP_ANY:
    case OP_MIN: case OP_MAX:
    case OP_COUNT: case OP_AVG: case OP_FIRST: case OP_LAST:
    case OP_STDDEV: case OP_STDDEV_POP: case OP_VAR: case OP_VAR_POP:
        if (reduced) return true;
        break;
    default: break;
    }
    if (n->arity == 0) return false;
    for (uint32_t i = 0; i < n->arity && i < 2; i++)
        if (!op_tree_leaves(g, n->in_id[i], reduced, atoms_only, depth + 1)) return false;
    if (n->opcode == OP_IF) {           /* the else branch rides in the ext */
        ray_op_ext_t* x = find_ext(g, id);
        if (!x || !op_tree_leaves(g, x->third_in, reduced, atoms_only, depth + 1)) return false;
    }
    if (n->opcode == OP_CONCAT) {
        ray_op_ext_t* x = find_ext(g, id);
        if (!x) return false;
        uint32_t* extra = (uint32_t*)(x + 1);
        for (int64_t i = 2; i < x->sym; i++)
            if (!op_tree_leaves(g, extra[i - 2], reduced, atoms_only, depth + 1)) return false;
    }
    return true;
}

/* True when the op tree under `id` carries none of the input's rows: every
 * leaf is a constant or sits under a reduction.  Tells a scalar-only SELECT
 * column (`(+ (sum x) 0)`) from a one-row one, which the column lengths
 * cannot on a one-row table (#675). */
bool op_tree_is_scalar(ray_graph_t* g, uint32_t id) {
    return op_tree_leaves(g, id, true, false, 0);
}

/* As above, but vector constants do not count as scalar atoms. */
bool op_tree_is_atom(ray_graph_t* g, uint32_t id) {
    return op_tree_leaves(g, id, true, true, 0);
}

/* True when the op tree under `id` reads no column at all, not even under a
 * reduction: a literal, or an expression over literals. */
static bool op_tree_is_const(ray_graph_t* g, uint32_t id) {
    return op_tree_leaves(g, id, false, false, 0);
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
 * that as "unknown" and skip the optimisation.  So does a constant holding
 * several values: a vector of the table's length stands for a column, row
 * aligned with the table, and the compacted survivors would read it from
 * its first cell on (one of another length is the group's length error,
 * which measures it against the table). */
/* Opcodes handled by the elementwise case of exec_node_inner — must
 * match its case labels (ROUND sits at 9, outside NEG..CAST; IDIV at
 * 49, outside ADD..MAX2; extra unary math starts at 137). */
static inline bool op_is_elementwise(uint16_t o) {
    return o == OP_ROUND || (o >= OP_NEG && o <= OP_CAST)
        || (o >= OP_ADD && o <= OP_MAX2) || o == OP_POW || o == OP_IDIV
        || (o >= OP_SIN && o <= OP_SIGNUM);
}

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
        if (node->opcode == OP_CONST) {
            ray_op_ext_t* cx = find_ext(g, nid);
            ray_t* lit = cx ? cx->literal : NULL;
            if (lit && !ray_is_atom(lit) && ray_len(lit) != 1) return -1;
            continue;
        }
        /* Admit only scalar element-wise expression nodes. */
        if (!op_is_elementwise(node->opcode)) return -1;

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

/* The sparse refinement's keep set for a membership test the element-wise
 * walk declines: `in` / `not in` of one generation-2 column (not SYM: its
 * ids resolve over the file's domain, which exec_in does not translate)
 * against a constant set.  The refinement then gathers that column's
 * survivors by range and runs exec_in over them, instead of evaluating
 * the test over every row, which reads the column whole.  Generation 0
 * keeps the full-width path (0: not admitted). */
static ray_t* scan_table_of(ray_graph_t* g, ray_op_ext_t* ext);
static _Atomic(uint64_t) g_refine_in;
uint64_t ray_exec_test_refine_in(void) { return atomic_load(&g_refine_in); }
static int refine_in_keep(ray_graph_t* g, ray_op_t* pred, ray_t* tbl, int64_t* keep) {
    if (!pred || (pred->opcode != OP_IN && pred->opcode != OP_NOT_IN)) return 0;
    ray_op_t* c0 = op_child(g, pred, 0);
    ray_op_t* c1 = op_child(g, pred, 1);
    if (!c0 || !c1 || c0->opcode != OP_SCAN || c1->opcode != OP_CONST) return 0;
    ray_op_ext_t* x = find_ext(g, c0->id);
    if (!x || scan_table_of(g, x) != tbl) return 0;
    ray_col_meta_t m;
    if (!ray_table_col_meta(tbl, x->sym, &m) || !m.enc) return 0;
    int8_t bt = RAY_IS_PARTED(m.type) ? (int8_t)RAY_PARTED_BASETYPE(m.type) : m.type;
    if (bt == RAY_SYM || bt == RAY_LIST) return 0;
    keep[0] = x->sym;
    atomic_fetch_add(&g_refine_in, 1);
    return 1;
}

/* Does a GROUP read a constant of several values as rows: a vector
 * literal under its key or aggregate inputs, as one or as an operand of an
 * element-wise node?  Such a vector stands for a column of the table it was
 * written against, so compacting the table under a where: would leave it
 * longer than the survivors.  An operand of any other node (the set of an
 * `in`, say) is no row input and does not count. */
static bool group_reads_vector_const(ray_graph_t* g, ray_op_ext_t* gx) {
    if (!gx) return false;
    uint32_t nc = g->node_count;
    if (nc > 4096) return true;
    bool visited[4096];
    memset(visited, 0, nc * sizeof(bool));
    uint32_t stack[64];
    bool rows[64];
    int sp = 0;
    for (uint8_t k = 0; k < gx->n_keys; k++) {
        if (sp >= 64) return true;
        stack[sp] = gx->keys[k]; rows[sp++] = true;
    }
    for (uint8_t a = 0; a < gx->n_aggs; a++) {
        uint32_t ins[2] = { gx->agg_ins ? gx->agg_ins[a] : RAY_OP_NONE,
                            gx->agg_ins2 ? gx->agg_ins2[a] : RAY_OP_NONE };
        for (int i = 0; i < 2; i++) {
            if (ins[i] == RAY_OP_NONE) continue;
            if (sp >= 64) return true;
            stack[sp] = ins[i]; rows[sp++] = true;
        }
    }
    while (sp > 0) {
        sp--;
        uint32_t nid = stack[sp];
        bool as_rows = rows[sp];
        if (nid >= nc) return true;
        ray_op_t* node = &g->nodes[nid];
        if (node->flags & OP_FLAG_DEAD) continue;
        if (node->opcode == OP_CONST) {
            ray_op_ext_t* cx = find_ext(g, nid);
            ray_t* lit = cx ? cx->literal : NULL;
            if (as_rows && lit && !ray_is_atom(lit) && ray_len(lit) != 1) return true;
            continue;
        }
        if (visited[nid]) continue;
        visited[nid] = true;
        bool child_rows = op_is_elementwise(node->opcode);
        ray_op_ext_t* nx = find_ext(g, nid);
        uint32_t kids[3] = { node->arity > 0 ? node->in_id[0] : RAY_OP_NONE,
                             node->arity > 1 ? node->in_id[1] : RAY_OP_NONE,
                             nx && nx->third_in != 0 ? nx->third_in : RAY_OP_NONE };
        for (int i = 0; i < 3; i++) {
            if (kids[i] == RAY_OP_NONE) continue;
            if (sp >= 64) return true;
            stack[sp] = kids[i]; rows[sp++] = child_rows;
        }
    }
    return false;
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
                ray_t* cerr = NULL;
                if (expr_compile(g, g->table, n, &ex, &cerr)) {
                    ray_t* vec = expr_eval_full(&ex, nr);
                    if (vec && !RAY_IS_ERR(vec)) {
                        vals[e.id]  = vec;
                        state[e.id] = EW_DONE;
                        continue;
                    }
                    if (expr_eval_err_fatal(vec)) { result = vec; goto done; }
                    ray_error_free(vec);   /* fall through to per-node eval */
                } else if (cerr) {         /* a failed column bind is the query's error */
                    result = cerr;
                    goto done;
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

/* Nodes whose result may be shared between consumers: pure, vector-valued
 * operators.  Structural ops (scan, filter, group, sort, join …) either
 * carry side state (g->selection) or return their input, and stay out. */
static inline bool op_memoizable(uint16_t o) {
    switch (o) {
        case OP_IF: case OP_LIKE: case OP_ILIKE: case OP_UPPER: case OP_LOWER:
        case OP_STRLEN: case OP_SUBSTR: case OP_REPLACE: case OP_TRIM:
        case OP_CONCAT: case OP_STR_FIND: case OP_EXTRACT: case OP_DATE_TRUNC:
        case OP_IN: case OP_NOT_IN:
            return true;
        default:
            return op_is_elementwise(o);
    }
}

/* Count each node's consumers over the in_id edges (plus one for the
 * root's return) and arm the memo when some memoizable node has more than
 * one.  The memo keeps its own ref on every stored value until
 * exec_memo_end: a consumer that receives an input with rc == 1 may reuse
 * the buffer in place, and a fused window or a sibling still holding a raw
 * pointer into that buffer would then read the overwritten values — so no
 * shared value is ever handed out as the sole reference.  Shared
 * intermediates therefore live to the end of the execution; before this a
 * shared node was recomputed per consumer instead.  Values belong to the
 * table the memo was armed over (memo_table): while g->table is swapped
 * for a sub-table the memo is inert.  Returns whether it armed; not
 * re-entrant on purpose — a nested execution of the same graph leaves the
 * outer memo alone and must not tear it down. */
static bool exec_memo_begin(ray_graph_t* g, ray_op_t* root) {
    if (!g || g->memo_uses || g->node_count == 0) return false;
    uint32_t nc = g->node_count;
    ray_t* hdr = NULL;
    char* mem = (char*)scratch_calloc(&hdr, (size_t)nc * (sizeof(ray_t*) + sizeof(uint32_t)));
    if (!mem) return false;
    ray_t**   vals = (ray_t**)mem;
    uint32_t* uses = (uint32_t*)(mem + (size_t)nc * sizeof(ray_t*));
    for (uint32_t i = 0; i < nc; i++) {
        ray_op_t* n = &g->nodes[i];
        if (n->flags & OP_FLAG_DEAD) continue;
        for (uint8_t k = 0; k < n->arity && k < 2; k++)
            if (n->in_id[k] != RAY_OP_NONE && n->in_id[k] < nc) uses[n->in_id[k]]++;
        /* Operands kept in the ext node: the third input of if / substr /
         * replace, the trailing arguments of concat. */
        if (n->opcode == OP_IF || n->opcode == OP_SUBSTR || n->opcode == OP_REPLACE) {
            ray_op_ext_t* e = find_ext(g, n->id);
            if (e && e->third_in < nc) uses[e->third_in]++;
        } else if (n->opcode == OP_CONCAT) {
            ray_op_ext_t* e = find_ext(g, n->id);
            if (e) {
                int n_args = (int)e->sym;
                const uint32_t* trail = (const uint32_t*)((const char*)(e + 1));
                for (int a = 2; a < n_args; a++)
                    if (trail[a - 2] < nc) uses[trail[a - 2]]++;
            }
        }
    }
    if (root && root->id < nc) uses[root->id]++;
    bool any = false;
    for (uint32_t i = 0; i < nc && !any; i++)
        any = uses[i] > 1 && op_memoizable(g->nodes[i].opcode);
    if (!any) { scratch_free(hdr); return false; }
    g->memo_vals = vals; g->memo_uses = uses; g->memo_n = nc; g->memo_hdr = hdr;
    g->memo_table = g->table;
    return true;
}

static void exec_memo_end(ray_graph_t* g) {
    if (!g || !g->memo_uses) return;
    for (uint32_t i = 0; i < g->memo_n; i++)
        if (g->memo_vals[i]) ray_release(g->memo_vals[i]);
    scratch_free(g->memo_hdr);
    g->memo_vals = NULL; g->memo_uses = NULL; g->memo_n = 0; g->memo_hdr = NULL;
    g->memo_table = NULL;
}

/* A sub-evaluation over another table (an `if` branch over its compacted
 * rows) gets a memo of its own: the outer memo is set aside — its values
 * belong to the outer table — and a fresh one is armed over the current
 * g->table with the sub-root's census.  Pop tears the inner memo down and
 * puts the outer one back. */
void ray_exec_memo_push(ray_graph_t* g, ray_op_t* root, ray_exec_memo_save_t* save) {
    save->vals = g->memo_vals; save->uses = g->memo_uses; save->n = g->memo_n;
    save->hdr = g->memo_hdr; save->table = g->memo_table;
    g->memo_vals = NULL; g->memo_uses = NULL; g->memo_n = 0; g->memo_hdr = NULL;
    g->memo_table = NULL;
    (void)exec_memo_begin(g, root);
}

void ray_exec_memo_pop(ray_graph_t* g, const ray_exec_memo_save_t* save) {
    exec_memo_end(g);
    g->memo_vals = save->vals; g->memo_uses = save->uses; g->memo_n = save->n;
    g->memo_hdr = save->hdr; g->memo_table = save->table;
}

/* Row filters being executed on this thread, the outermost first. */
static RAY_TLS int tl_filter_depth;

ray_t* exec_node(ray_graph_t* g, ray_op_t* op) {
    if (!op) return ray_error("nyi", NULL);

    /* Shared node already computed: hand out a ref (the memo's own ref goes
     * at exec_memo_end). */
    bool memo_on = g->memo_uses && g->table == g->memo_table &&
                   op->id < g->memo_n && op_memoizable(op->opcode);
    if (memo_on && g->memo_vals[op->id]) {
        ray_t* v = g->memo_vals[op->id];
        ray_retain(v);
        /* The memo keeps its own ref until exec_memo_end: a consumer that
         * reaches its input with rc == 1 may reuse the buffer in place, and
         * another consumer may still hold a raw pointer into it. */
        return v;
    }
    bool memo_shared = memo_on && g->memo_uses[op->id] > 1;

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
                uint64_t sum = 0; uint32_t used = 0;
                ray_query_workers_snapshot(&used, &sum);
                sp->qs_busy_ns = sum;
            }
        }
    }

    /* A row filter (not a HAVING over a group); an `and` runs as a chain of
     * them, each executing the next one in as its input. */
    bool row_filter = false;
    if (op->opcode == OP_FILTER) {
        ray_op_t* fc = op_child(g, op, 0);
        row_filter = !fc || fc->opcode != OP_GROUP;
    }
    if (row_filter) tl_filter_depth++;
    ray_t* _prof_result = exec_node_inner(g, op);
    tl_exec_depth--;
    /* The outermost filter of a chain has the final selection: the
     * select's columns held back for it can be requested now. */
    if (row_filter && --tl_filter_depth == 0 && _prof_result && !RAY_IS_ERR(_prof_result))
        ray_select_scan_filtered(g->selection,
            _prof_result->type == RAY_TABLE ? ray_table_nrows(_prof_result) : 0);

    /* First consumer of a shared node: keep a ref for the others (a lazy
     * value is not kept — materialising it is the consumer's business). */
    if (memo_shared && _prof_result && !RAY_IS_ERR(_prof_result) &&
        !ray_is_lazy(_prof_result) && g->memo_uses && g->table == g->memo_table &&
        !g->memo_vals[op->id]) {
        ray_retain(_prof_result);
        g->memo_vals[op->id] = _prof_result;
    }

    if (profiling) {
        ray_prof_span_t* ep = ray_profile_span_end(oname);
        if (ep) {
            int64_t cur = 0; ray_sys_get_stat(&cur, NULL);
            ep->sys_cur = cur;
            ep->qs_rows = ray_qstats_sum_rows();
            uint64_t sum = 0; uint32_t used = 0;
            ray_query_workers_snapshot(&used, &sum);
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

/* Release a table an operator consumed (a selection-compacted copy, a
 * child's result) after unbinding it: kernels compiled over it bound its
 * columns on `g`, and each bind retains its column until ray_graph_free, so
 * a released temporary would stay alive for the rest of the graph.  The
 * query table itself keeps its binds. */
static void exec_release_tmp(ray_graph_t* g, ray_t* t) {
    if (t && !RAY_IS_ERR(t) && t->type == RAY_TABLE && t != g->table)
        ray_graph_unbind_table(g, t);
    ray_release(t);
}

/* The table an OP_SCAN reads: pad[0..1] holds table_id + 1, 0 = g->table. */
static ray_t* scan_table_of(ray_graph_t* g, ray_op_ext_t* ext) {
    uint16_t stored_table_id = 0;
    memcpy(&stored_table_id, ext->base.pad, sizeof(uint16_t));
    if (stored_table_id > 0 && g->tables && (stored_table_id - 1) < g->n_tables)
        return g->tables[stored_table_id - 1];
    return g->table;
}

/* A chained filter's refinement over a generation-2 predicate column: the
 * predicate is evaluated on the segments the selection keeps, through the
 * fused path's encoded leaves (views and vector statistics), and ANDed with
 * the selection (exec_pred_refine_selection), so no predicate value is
 * gathered.  input (the selection replaced), an owned error (input
 * released), or NULL: not such a predicate, or unsupported (the caller's
 * gather runs).  Out of line: exec_node_inner keeps its code. */
static __attribute__((noinline)) ray_t* exec_filter_refine_enc(ray_graph_t* g, ray_op_t* op, ray_t* input) {
    ray_op_t* pred_root = op_child(g, op, 1);
    int64_t ks[32];
    bool has_expr = false;
    uint32_t roots[1] = { pred_root->id };
    int nks = collect_scan_syms(g, roots, 1, ks, 32, &has_expr);
    bool enc = false;
    for (int i = 0; i < nks && !enc; i++)
        enc = ray_col_gather_slot(ray_table_get_col_enc(input, ks[i])) != NULL;
    if (!enc) return NULL;
    bool ap = false;
    ray_t* rs = exec_pred_refine_selection(g, pred_root, g->selection, &ap);
    if (rs && RAY_IS_ERR(rs)) { ray_release(input); return rs; }
    if (!rs && !ap) return NULL;
    ray_release(g->selection);
    g->selection = rs;                 /* NULL: every row passes */
    return input;
}

static ray_t* exec_node_inner(ray_graph_t* g, ray_op_t* op) {
    if (!op) return ray_error("nyi", NULL);

    switch (op->opcode) {
        case OP_SCAN: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            if (!ext) return ray_error("nyi", NULL);

            ray_t* scan_tbl = scan_table_of(g, ext);
            if (!scan_tbl) return ray_error("schema", NULL);
            ray_t* col = ray_table_col(scan_tbl, ext->sym);
            if (!col) return ray_error("schema", NULL);
            if (RAY_IS_ERR(col)) return col;     /* a failed decode keeps its own code and message */
            if (col->type == RAY_MAPCOMMON)
                return materialize_mapcommon(col);
            if (RAY_IS_PARTED(col->type)) {
                /* Concat parted segments into a flat vector (cold path);
                 * the table's partition counts give a missing segment its
                 * rows as typed nulls (ragged parted table). */
                {
                    ray_t* f = flatten_parted_col(col, scan_tbl);
                    PARTED_FLAT_DCHECK(scan_tbl, f);
                    return f;
                }
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
        case OP_SIN: case OP_ASIN: case OP_COS: case OP_ACOS:
        case OP_TAN: case OP_ATAN: case OP_RECIPROCAL: case OP_SIGNUM:
        case OP_ISNULL: case OP_CAST:
        /* Binary element-wise */
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_IDIV: case OP_MOD:
        case OP_POW:
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
        case OP_GT: case OP_GE: case OP_AND: case OP_OR:
        case OP_MIN2: case OP_MAX2: {
            /* Try compiled expression first (fuses entire subtree) */
            if (g->table) {
                int64_t nr = ray_table_nrows(g->table);
                if (nr > 0) {
                    ray_expr_t ex;
                    ray_t* cerr = NULL;
                    if (expr_compile(g, g->table, op, &ex, &cerr)) {
                        ray_t* vec = expr_eval_full(&ex, nr);
                        if (vec && !RAY_IS_ERR(vec)) return vec;
                        if (expr_eval_err_fatal(vec)) return vec;   /* a range read failed */
                        if (vec) ray_error_free(vec);               /* oom: per-node eval */
                    } else if (cerr) {
                        return cerr;
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
            /* A reduction of an encoded column's scan, no selection: reduce
             * the bound source (directory answer or range reads) instead of
             * materializing it.  A table without an encoded slot never
             * enters (sm.enc is NULL).  With a selection installed the
             * reduction must read only the selected rows (exec_reduction's
             * selection branch); no language shape reaches this arm with one
             * today (a select with a where takes the group path), so that
             * case keeps the plain path rather than going untested. */
            ray_op_t* child = op_child(g, op, 0);
            if (child && child->opcode == OP_SCAN && !g->selection) {
                ray_op_ext_t* se = find_ext(g, child->id);
                ray_t* scan_tbl = se ? scan_table_of(g, se) : NULL;
                ray_col_meta_t sm;
                if (scan_tbl && ray_table_col_meta(scan_tbl, se->sym, &sm) && sm.enc &&
                    !ray_col_bind_declines(scan_tbl, se->sym)) {
                    ray_t* berr = NULL;
                    ray_col_src_t* src = ray_graph_bind_col(g, scan_tbl, se->sym, &berr);
                    if (berr) return berr;
                    if (src) {
                        if (src->type > 0 && !agg_type_admitted(op->opcode, src->type))
                            return ray_error("type", "aggregate: element type not admitted for this aggregation, got %s", ray_type_name(src->type));
                        ray_t* r = exec_reduction_src(g, op, src);
                        if (r) return r;            /* NULL: type not served, plain path below */
                    }
                }
            }
            ray_t* input = exec_node(g, child);
            if (!input || RAY_IS_ERR(input)) return input;
            /* Compact lazy selection before reducing — filters may have
             * set g->selection without materializing a compacted table. */
            /* Owned, like every exec_node result (see OP_SORT); no child
             * evaluates to a borrowed query table. */
            bool own_input = true;
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
            if (own_input) exec_release_tmp(g, input);
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
                if (group_result->type == RAY_TABLE) {
                    pred = ray_where_mask_coerce(pred, ray_table_nrows(group_result), "where");
                    if (!pred || RAY_IS_ERR(pred)) {
                        ray_release(group_result);
                        return pred ? pred : ray_error("oom", NULL);
                    }
                }

                ray_t* result = exec_filter(g, op, group_result, pred);
                ray_release(pred);
                ray_release(group_result);
                return result;
            }

            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            /* A verified physically-sorted column remains routable even when
             * this is the outer half of a split AND predicate.  Intersect the
             * new contiguous range rowsel with the existing selection instead
             * of evaluating the second bound over surviving rows. */
            if (input->type == RAY_TABLE) {
                ray_t* scol = NULL;
                uint16_t sop = 0;
                int64_t ski = 0;
                double skf = 0.0;
                int sisf = 0;
                if (idx_filter_decode(g, op_child(g, op, 1), IDXW_SORTED,
                                      &scol, &sop, &ski, &skf, &sisf, NULL) &&
                    sop != OP_NE && ray_attr_is_sorted(scol)) {
                    ray_idx_consults[IDX_SITE_FILTER_RANGE]++;
                    ray_t* ssel = ray_sorted_range_rowsel(scol, sop, ski, skf,
                                                          (bool)sisf);
                    if (ssel) {
                        if (g->selection) {
                            ray_t* merged = ray_rowsel_intersect(g->selection,
                                                                 ssel);
                            ray_rowsel_release(ssel);
                            if (merged) {
                                ray_rowsel_release(g->selection);
                                g->selection = merged;
                                ray_idx_hits[IDX_SITE_FILTER_RANGE]++;
                                return input;
                            }
                        } else {
                            g->selection = ssel;
                            ray_idx_hits[IDX_SITE_FILTER_RANGE]++;
                            return input;
                        }
                    }
                }
            }
            /* Hash-index point-lookup fast path: when the predicate is
             * `col == K` on a column with RAY_IDX_HASH attached and
             * built for the column's current length, install the
             * matching rowsel on g->selection directly — bypasses
             * both the O(rows) compare AND the O(rows) BOOL→rowsel
             * scan.  Only fires for the lazy TABLE-input case with no
             * pre-existing selection (the entry shape downstream
             * group-by / sort already expects). */
            if (input->type == RAY_TABLE && !g->selection) {
                /* A symbol missing from the column's domain selects no rows,
                 * indexed or not.  Decided from the domain alone: a generation-2
                 * column is never decoded to learn that. */
                {
                    uint16_t pc = 0; int64_t pk = 0; double pf = 0.0; int pif = 0;
                    ray_col_meta_t pm;
                    if (idx_filter_decode(g, op_child(g, op, 1), IDXW_NONE, NULL, &pc,
                                          &pk, &pf, &pif, &pm) &&
                        pm.type == RAY_SYM && pc == OP_EQ && !pif && pk != 0) {
                        ray_op_t* sl = op_child(g, op_child(g, op, 1), 0);
                        ray_op_ext_t* sx = sl ? find_ext(g, sl->id) : NULL;
                        struct ray_sym_domain_s* dom = NULL;
                        if (pm.enc) {
                            ray_enccol_t* ee = ray_enccol_of(pm.enc);
                            if (ee) dom = ee->dom;
                        } else if (sx) {
                            ray_t* pcol = ray_table_col(g->table, sx->sym);
                            if (pcol && RAY_IS_ERR(pcol)) ray_error_free(pcol);
                            else if (pcol) dom = ray_sym_vec_domain(pcol);
                        }
                        ray_t* ps = dom ? ray_sym_str(pk) : NULL;
                        if (ps && ray_sym_domain_find(dom, ray_str_ptr(ps), ray_str_len(ps)) < 0) {
                            ray_t* empty = ray_index_empty_rowsel(ray_table_nrows(input));
                            if (empty) { g->selection = empty; return input; }
                        }
                    }
                }
                ray_t*   col      = NULL;
                uint16_t cmp_op   = 0;
                int64_t  key_i    = 0;
                double   key_f    = 0.0;
                int      is_float = 0;
                /* A generation-2 column without a published materialization
                 * is consulted from its directory (finding C1) and never
                 * decoded here, unless the shape uses its recorded HASH /
                 * SORT / BLOOM / PART (idx_enc_cmp_uses_index): that column
                 * is materialized now (its failure is the query's) and, like
                 * a published one, takes the block below on its
                 * materialization, which carries the attached index. */
                ray_col_meta_t fm;
                bool enc_done = false;
                bool fm_ok = idx_filter_decode(g, op_child(g, op, 1), IDXW_INDEX, NULL, &cmp_op,
                                               &key_i, &key_f, &is_float, &fm);
                bool fm_uses = fm_ok && fm.enc && idx_enc_cmp_uses_index(&fm, cmp_op, key_i, is_float);
                if (fm_ok && idx_enc_from_dir(&fm, fm_uses)) {
                    enc_done = true;
                    ray_t* esel = NULL; ray_t* eerr = NULL;
                    int rc = idx_filter_enc_consult(fm.enc, &fm, cmp_op, key_i, key_f, is_float,
                                                    ray_table_nrows(input), &esel, &eerr);
                    if (rc < 0) { ray_release(input); return eerr; }
                    if (rc == 1) return input;
                    if (rc == 2) { g->selection = esel; return input; }
                } else if (fm_uses) {
                    ray_t* merr = idx_enc_index_ready(g, &fm, idx_conj_sym(g, op_child(g, op, 1)));
                    if (merr) { ray_release(input); return merr; }
                }
                if (!enc_done && idx_filter_decode(g, op_child(g, op, 1), IDXW_INDEX, &col, &cmp_op,
                                                   &key_i, &key_f, &is_float, NULL)) {
                    /* 1. Zone index: O(1) all/none short-circuit. */
                    if (ray_index_kind(col) == RAY_IDX_ZONE)
                        ray_idx_consults[IDX_SITE_FILTER_ZONE]++;
                    switch (ray_index_zone_class(col, cmp_op, key_i, key_f)) {
                    case RAY_ZONE_ALL:
                        ray_idx_hits[IDX_SITE_FILTER_ZONE]++;
                        ray_idx_note_use(col);
                        /* All rows pass — mirror the lazy path: leave
                         * g->selection NULL (no filter) and return the
                         * table.  Same ownership as the hash path below. */
                        return input;
                    case RAY_ZONE_NONE: {
                        ray_idx_hits[IDX_SITE_FILTER_ZONE]++;
                        ray_idx_note_use(col);
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
                                ray_idx_note_use(col);
                                g->selection = empty;
                                return input;
                            }
                            /* OOM — fall through to scan */
                        }
                    }

                    /* 3. Hash/part EQ: nonzero SYM keys remain eligible
                     * even when other rows are null; indexes omit null rows.
                     * A part hit is already one contiguous physical span, so
                     * it stays eligible at any density. */
                    if (cmp_op == OP_EQ && !is_float &&
                        (col->type == RAY_SYM ? key_i != 0 :
                         !ray_vec_may_have_nulls(col))) {
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
                            ray_idx_kind_t eq_kind = ray_index_kind(col);
                            /* No persisted hash: an unprunable integer zone
                             * gets the lazy in-memory one (built on this first
                             * point lookup, reused after).  The indexed copy
                             * has the table column's rows. */
                            /* A key outside the chunk zone's whole range
                             * matches nothing: answered here, nothing built. */
                            if (eq_kind != RAY_IDX_HASH && eq_kind != RAY_IDX_PART &&
                                ray_idx_zone_excludes(col, key_i)) {
                                ray_idx_consults[IDX_SITE_FILTER_ZONE]++;
                                ray_t* empty = ray_index_empty_rowsel(ray_table_nrows(input));
                                if (empty) {
                                    ray_idx_hits[IDX_SITE_FILTER_ZONE]++;
                                    ray_idx_note_use(col);
                                    g->selection = empty;
                                    return input;
                                }
                            }
                            ray_t* lazy_col = NULL;
                            if (eq_kind != RAY_IDX_HASH && eq_kind != RAY_IDX_PART) {
                                ray_t* le = idx_lazy_err(ray_idx_lazy_hash(col, &lazy_col));
                                if (le) { ray_release(input); return le; }
                                if (lazy_col) eq_kind = RAY_IDX_HASH;
                            }
                            ray_t* probe_col = lazy_col ? lazy_col : col;
                            if (eq_kind == RAY_IDX_HASH ||
                                eq_kind == RAY_IDX_PART) {
                                idx_site_t site = eq_kind == RAY_IDX_HASH
                                    ? IDX_SITE_FILTER_HASH
                                    : IDX_SITE_FILTER_PART;
                                ray_idx_consults[site]++;
                                ray_t* sel = eq_kind == RAY_IDX_HASH
                                    ? ray_index_hash_eq_rowsel(probe_col, key_i)
                                    : ray_index_part_eq_rowsel(probe_col, key_i);
                                if (sel) {
                                    ray_idx_hits[site]++;
                                    ray_idx_note_use(probe_col);
                                    if (lazy_col) ray_release(lazy_col);
                                    g->selection = sel;
                                    return input;
                                }
                            }
                            if (lazy_col) ray_release(lazy_col);
                            /* sel == NULL: ineligible (wrong/stale index kind,
                             * key out of range) or OOM — fall through to scan. */
                        }
                    }

                    /* 4. Sort-index range: EQ/LT/LE/GT/GE (NE returns NULL).
                     * The physical-sorted route runs before this block so it
                     * can also intersect an existing chained selection. */
                    if (cmp_op != OP_NE) {
                        /* Shuffled physical rows need the
                         * permutation probe and its sparse-span guard. */
                        if (ray_index_kind(col) == RAY_IDX_SORT) {
                            ray_idx_consults[IDX_SITE_FILTER_RANGE]++;
                            ray_t* sel = ray_index_range_rowsel(col, cmp_op,
                                                                key_i, key_f,
                                                                (bool)is_float);
                            if (sel) {
                                ray_idx_hits[IDX_SITE_FILTER_RANGE]++;
                                ray_idx_note_use(col);
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
                    /* Metadata first: a conjunct list without an equality on a
                     * hash-indexed column decodes nothing. */
                    bool has_hash_eq = false;
                    ray_t* eq_enc = NULL;
                    ray_t* eq_lazy = NULL;
                    for (int ci = 0; ci < ncj && shape_ok && !has_hash_eq; ci++) {
                        uint16_t cop = 0; ray_col_meta_t cm;
                        int64_t ki = 0; double kf = 0.0; int isf = 0;
                        has_hash_eq = idx_filter_decode(g, conj[ci], IDXW_NONE, NULL, &cop,
                                                        &ki, &kf, &isf, &cm) &&
                                      cop == OP_EQ && (cm.index_kind == RAY_IDX_HASH ||
                                                       idx_lazy_meta_ok(&cm));
                        if (has_hash_eq && cm.enc && !ray_enccol_decoded_peek(cm.enc) &&
                            cm.index_kind != RAY_IDX_HASH)
                            eq_enc = cm.enc;
                        /* a recorded HASH materializes for its index only
                         * when the whole shape is the one the decode below
                         * takes (else nothing is decoded); its failure is
                         * the query's, and the decode loop reads it */
                        if (has_hash_eq && cm.enc && cm.index_kind == RAY_IDX_HASH &&
                            !ray_enccol_decoded_peek(cm.enc)) {
                            if (!idx_eqrange_meta_ok(g, conj, ncj)) { shape_ok = false; break; }
                            ray_t* merr = idx_enc_index_ready(g, &cm, idx_conj_sym(g, conj[ci]));
                            if (merr) { ray_release(input); return merr; }
                        }
                    }
                    shape_ok = shape_ok && has_hash_eq;
                    /* Finding C1: a generation-2 equality column is probed
                     * (and its lazy HASH built, materializing it) before any
                     * conjunct column is decoded, once the metadata shows
                     * the shape the decode below accepts; while the probe is
                     * still pending the scan answers and nothing is decoded. */
                    if (shape_ok && eq_enc) {
                        if (!idx_eqrange_meta_ok(g, conj, ncj)) shape_ok = false;
                        else {
                            ray_t* le = idx_lazy_err(ray_idx_lazy_hash_enc(eq_enc, &eq_lazy));
                            if (le) { ray_release(input); return le; }
                            if (!eq_lazy) shape_ok = false;
                        }
                    }
                    for (int ci = 0; ci < ncj && shape_ok; ci++) {
                        ray_t* c = NULL; uint16_t cop = 0; ray_col_meta_t dm;
                        int64_t ki = 0; double kf = 0.0; int isf = 0;
                        if (!idx_filter_decode(g, conj[ci], IDXW_NONE, &c, &cop,
                                               &ki, &kf, &isf, &dm) || isf) {
                            shape_ok = false; break;
                        }
                        bool c_nulls = idx_conj_may_null(g, &dm, idx_conj_sym(g, conj[ci]));
                        if (cop == OP_EQ && !eq_col &&
                            (ray_index_kind(c) == RAY_IDX_HASH || ray_idx_lazy_hash_wanted(c)) &&
                            (c->type == RAY_SYM ? ki != 0 : !c_nulls)) {
                            eq_col = c; eq_key = ki;
                        } else if (cop == OP_GE || cop == OP_GT ||
                                   cop == OP_LE || cop == OP_LT) {
                            if (c_nulls) { shape_ok = false; break; }
                            if (!rg_col) rg_col = c;
                            else if (rg_col != c) { shape_ok = false; break; }
                            if (cop == OP_GE)      { if (ki > lo) lo = ki; }
                            else if (cop == OP_GT) { if (ki == INT64_MAX) { lo = 1; hi = 0; } else if (ki + 1 > lo) lo = ki + 1; }
                            else if (cop == OP_LE) { if (ki < hi) hi = ki; }
                            else                   { if (ki == INT64_MIN) { lo = 1; hi = 0; } else if (ki - 1 < hi) hi = ki - 1; }
                        } else { shape_ok = false; break; }
                    }
                    /* The equality column without a persisted hash probes
                     * its lazy one, built only now that the whole shape
                     * matched (a generation-2 one was probed above, and its
                     * build published the materialization the decode read);
                     * the range column stays the table's. */
                    if (shape_ok && eq_col && rg_col && rg_col != eq_col && !eq_lazy &&
                        ray_index_kind(eq_col) != RAY_IDX_HASH) {
                        ray_t* le = idx_lazy_err(ray_idx_lazy_hash(eq_col, &eq_lazy));
                        if (le) { ray_release(input); return le; }
                        if (!eq_lazy) shape_ok = false;
                    }
                    if (eq_lazy) eq_col = eq_lazy;
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
                        ray_idx_rows_t grows = { NULL, false };
                        int64_t gn = 0;
                        int hit = absent ? 0
                            : ray_index_hash_group(eq_col, probe, &grows, &gn);
                        if (hit == 0 || (hit > 0 && lo > hi)) {
                            /* provably empty: absent key or empty range */
                            int64_t nrows = ray_table_nrows(input);
                            ray_t* sel = ray_index_rowsel_from_ids(nrows, NULL, 0);
                            if (sel) {
                                ray_idx_hits[IDX_SITE_FILTER_EQRANGE]++;
                                ray_idx_note_use(eq_col);
                                if (eq_lazy) ray_release(eq_lazy);
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
                                int64_t v = read_col_i64(rgd, ray_idx_rows_at(grows, pp), rt, ra);
                                if (v < prevv) { mono = false; break; }
                                prevv = v;
                            }
                            if (mono) {
                                int64_t a = 0, b = gn;   /* first pos >= lo */
                                while (a < b) {
                                    int64_t m = a + (b - a) / 2;
                                    if (read_col_i64(rgd, ray_idx_rows_at(grows, m), rt, ra) < lo) a = m + 1;
                                    else b = m;
                                }
                                int64_t startp = a;
                                b = gn;                   /* first pos > hi */
                                while (a < b) {
                                    int64_t m = a + (b - a) / 2;
                                    if (read_col_i64(rgd, ray_idx_rows_at(grows, m), rt, ra) <= hi) a = m + 1;
                                    else b = m;
                                }
                                int64_t nrows = ray_table_nrows(input);
                                ray_t* sel = ray_index_rowsel_from_rows(
                                    nrows, ray_idx_rows_from(grows, startp), a - startp);
                                if (sel) {
                                    ray_idx_hits[IDX_SITE_FILTER_EQRANGE]++;
                                    ray_idx_note_use(eq_col);
                                    if (eq_lazy) ray_release(eq_lazy);
                                    g->selection = sel;
                                    return input;
                                }
                            }
                            /* non-monotone slice / OOM: fall through */
                        }
                        /* hit < 0 (stale/ineligible index): fall through */
                    }
                    if (eq_lazy) ray_release(eq_lazy);
                }

                /* 5. Hash/part-index IN probe:
                 * FILTER(IN(SCAN col, CONST set_vec)).  Hash indexes probe
                 * each unique set element; part indexes select their
                 * contiguous physical spans directly. */
                {
                    ray_t* in_col     = NULL;
                    ray_t* in_set_lit = NULL;
                    /* generation 2 without a published materialization:
                     * from the directory, never decoded here, unless the
                     * `in` uses its recorded HASH / PART
                     * (idx_enc_in_uses_index): that column materializes now
                     * (its failure the query's) and takes the block below on
                     * its index */
                    ray_col_meta_t im;
                    bool in_enc_done = false;
                    bool im_ok = idx_filter_in_decode(g, op_child(g, op, 1), IDXW_INDEX, NULL, &in_set_lit, &im);
                    bool im_uses = im_ok && im.enc && idx_enc_in_uses_index(&im);
                    if (im_ok && idx_enc_from_dir(&im, im_uses)) {
                        in_enc_done = true;
                        ray_t* isel = NULL; ray_t* ierr = NULL;
                        int rc = idx_filter_enc_in_consult(im.enc, &im, in_set_lit,
                                                           ray_table_nrows(input), &isel, &ierr);
                        if (rc < 0) { ray_release(input); return ierr; }
                        if (rc == 2) { g->selection = isel; return input; }
                    } else if (im_uses) {
                        ray_t* merr = idx_enc_index_ready(g, &im, idx_conj_sym(g, op_child(g, op, 1)));
                        if (merr) { ray_release(input); return merr; }
                    }
                    if (!in_enc_done &&
                        idx_filter_in_decode(g, op_child(g, op, 1), IDXW_INDEX, &in_col, &in_set_lit, NULL)) {
                        bool in_col_is_float = (in_col->type == RAY_F32 ||
                                               in_col->type == RAY_F64);
                        ray_idx_kind_t in_kind = ray_index_kind(in_col);
                        /* No persisted index: a small integer set on an
                         * unprunable zone probes the lazy in-memory hash. */
                        if (in_kind != RAY_IDX_HASH && in_kind != RAY_IDX_PART &&
                            !in_col_is_float && !ray_vec_may_have_nulls(in_col) &&
                            ray_idx_zone_excludes_set(in_col, in_set_lit)) {
                            /* every element outside the zone's range */
                            ray_idx_consults[IDX_SITE_FILTER_ZONE]++;
                            ray_t* empty = ray_index_empty_rowsel(ray_table_nrows(input));
                            if (empty) {
                                ray_idx_hits[IDX_SITE_FILTER_ZONE]++;
                                ray_idx_note_use(in_col);
                                g->selection = empty;
                                return input;
                            }
                        }
                        ray_t* in_lazy = NULL;
                        if (in_kind != RAY_IDX_HASH && in_kind != RAY_IDX_PART &&
                            !in_col_is_float && !ray_vec_may_have_nulls(in_col) &&
                            idx_lazy_set_ok(in_col, in_set_lit)) {
                            ray_t* le = idx_lazy_err(ray_idx_lazy_hash(in_col, &in_lazy));
                            if (le) { ray_release(input); return le; }
                            if (in_lazy) in_kind = RAY_IDX_HASH;
                        }
                        ray_t* in_probe = in_lazy ? in_lazy : in_col;
                        if ((in_kind == RAY_IDX_HASH || in_kind == RAY_IDX_PART) &&
                            !in_col_is_float) {
                            ray_idx_consults[IDX_SITE_IN]++;
                            ray_t* sel = ray_index_in_rowsel(in_probe, in_set_lit);
                            if (sel) {
                                ray_idx_hits[IDX_SITE_IN]++;
                                ray_idx_note_use(in_probe);
                                if (in_lazy) ray_release(in_lazy);
                                g->selection = sel;
                                return input;
                            }
                            /* NULL: guard fired, ineligible, or OOM — fall through. */
                        }
                        if (in_lazy) ray_release(in_lazy);
                    }
                }

                /* 6. Fused predicate→selection: when the WHERE predicate is an
                 * expr-evaluable BOOL subtree, stream its per-morsel bools
                 * straight into a rowsel — skipping both the full BOOL vec and
                 * the second BOOL→rowsel scan.  Any unsupported shape (parted,
                 * OOM) returns NULL+all_pass=false, falling through to the
                 * bool path. */
                ray_op_t* pred_root = op_child(g, op, 1);
                {
                    int64_t fnrows = ray_table_nrows(input);
                    bool all_pass = false;
                    ray_t* fsel = exec_pred_to_selection(g, pred_root, fnrows,
                                                         &all_pass);
                    if (fsel && RAY_IS_ERR(fsel)) {       /* a failed bind or range read, an oom, a cancel */
                        ray_release(input);
                        return fsel;
                    }
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
            /* Over a generation-2 predicate column the refinement reads no
             * gathered values (exec_filter_refine_enc); generation 0 keeps
             * the gather below. */
            if (input->type == RAY_TABLE && g->selection && input == g->table) {
                ray_t* rs = exec_filter_refine_enc(g, op, input);
                if (rs) return rs;             /* input (refined), or an owned error */
            }
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
                    if (nkeep <= 0) nkeep = refine_in_keep(g, pred_root, input, keep);
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
                            ray_graph_unbind_table(g, sub);
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

            /* A later `in` conjunct over a generation-2 column (parted:
             * segment by segment; single-file: by range) read over the
             * morsels the installed selection keeps, ANDed with it
             * (exec_in_parted_selection), instead of a bool vector over the
             * column read whole. */
            if (input->type == RAY_TABLE && g->selection && input == g->table) {
                bool in_all = false;
                ray_t* isel = exec_in_parted_selection(g, op_child(g, op, 1), ray_table_nrows(input),
                                                       g->selection, &in_all);
                if (isel && RAY_IS_ERR(isel)) { ray_release(input); return isel; }
                if (isel || in_all) {
                    ray_rowsel_release(g->selection);
                    g->selection = isel;               /* NULL: every row passes both */
                    return input;
                }
            }

            ray_t* pred = exec_node(g, op_child(g, op, 1));
            if (!pred || RAY_IS_ERR(pred)) { ray_release(input); return pred; }

            /* Over a table every predicate value goes through the one
             * where: rule (ray_where_mask_coerce): a scalar (`(== (count x)
             * 5)`, a scalar conjunct of an `and`) or a one-element BOOL is
             * spread over the rows; any other vector is a type error.
             * Before this, an I64 column fell through to the eager
             * exec_filter, which read it as one byte per row (#678). */
            if (input->type == RAY_TABLE &&
                !(pred->type == RAY_BOOL && pred->len == ray_table_nrows(input))) {
                int64_t tn = ray_table_nrows(input);
                if (ray_is_atom(pred) && ray_pred_atom_type_ok(pred->type) &&
                    is_truthy(pred)) {
                    ray_release(pred);  /* every row passes: selection unchanged */
                    return input;
                }
                pred = ray_where_mask_coerce(pred, tn, "where");
                if (!pred || RAY_IS_ERR(pred)) {
                    ray_release(input);
                    return pred ? pred : ray_error("oom", NULL);
                }
            }

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
            /* exec_node hands back an OWNED reference for every node,
             * the constant table node included (it retains its literal).
             * A `!= g->table` guard here treated a child that evaluates
             * to the query table as borrowed and leaked one table per
             * call — the ordinary shape, since a select's root is a
             * constant node over that very table. */
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            ray_t* tbl = (input->type == RAY_TABLE) ? input : g->table;
            /* Compact lazy selection before sort (needs dense data) */
            if (g->selection && tbl && !RAY_IS_ERR(tbl) && tbl->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, tbl, g->selection, NULL, 0);
                /* sel_compact's alloc fallback hands back the table itself:
                 * sorting it would answer from rows the selection dropped */
                if (compacted && !RAY_IS_ERR(compacted) &&
                    ray_table_nrows(compacted) != ray_rowsel_meta(g->selection)->total_pass) {
                    ray_release(compacted);
                    compacted = ray_error("oom", NULL);
                }
                ray_release(input);
                ray_release(g->selection);
                g->selection = NULL;
                input = compacted;
                tbl = compacted;
            }
            /* An all-pass where: leaves no selection and hands over the
             * source itself; a parted source still has segmented columns,
             * which the sort cannot gather.  Sort a flattened copy, as the
             * partial-where path above does. */
            if (!g->selection && tbl && tbl->type == RAY_TABLE) {
                ray_t* flat = flatten_parted_table(tbl, g->table);
                if (flat) {
                    ray_release(input);
                    if (RAY_IS_ERR(flat)) return flat;
                    input = flat;
                    tbl = flat;
                }
            }
            ray_t* result = exec_sort(g, op, tbl, 0);
            exec_release_tmp(g, input);
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
                    if (owned_tbl) { ray_graph_unbind_table(g, owned_tbl); ray_release(owned_tbl); }
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
                /* A keyless group (an all-aggregate select) is one row and
                 * its aggregates read the selection themselves; compacted,
                 * a no-match where: handed exec_group an empty table and it
                 * came back with no row (#679, the HEAD arm likewise). */
                if (gx && gx->n_keys > 0 && meta && nrows > 0 &&
                    meta->total_pass * (contig ? 2 : 4) <= nrows) {
                    int64_t keep[32];
                    bool has_expr = false;
                    int nkeep = group_input_scan_syms(g, gx, keep, 32, &has_expr);
                    if (nkeep > 0 && has_expr) {
                        ray_t* compacted = sel_compact(g, tbl, g->selection,
                                                       keep, nkeep);
                        if (compacted && !RAY_IS_ERR(compacted) &&
                            compacted != tbl) {
                            if (owned_tbl) { ray_graph_unbind_table(g, owned_tbl); ray_release(owned_tbl); }
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
            if (owned_tbl) { ray_graph_unbind_table(g, owned_tbl); ray_release(owned_tbl); }
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
            if (owned_tbl) { ray_graph_unbind_table(g, owned_tbl); ray_release(owned_tbl); }
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
            exec_release_tmp(g, left);
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
            exec_release_tmp(g, left);
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
            exec_release_tmp(g, left);
            ray_release(right);
            return result;
        }

        case OP_WINDOW: {
            /* Owned input, released unconditionally: see OP_SORT. */
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            ray_t* wdf = (input->type == RAY_TABLE) ? input : g->table;
            /* Compact lazy selection before window (needs dense data) */
            if (g->selection && wdf && !RAY_IS_ERR(wdf) && wdf->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, wdf, g->selection, NULL, 0);
                ray_release(input);
                ray_release(g->selection);
                g->selection = NULL;
                input = compacted;
                wdf = compacted;
            }
            ray_t* result = exec_window(g, op, wdf);
            exec_release_tmp(g, input);
            return result;
        }

        case OP_HEAD: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            int64_t n = ext ? ext->sym : 10;

            /* Fused sort+limit: detect SORT child → only gather N rows.  Not
             * for N = 0: exec_sort reads a 0 limit as none (every row); the
             * plain HEAD below trims the sort to no rows. */
            ray_op_t* child_op = op_child(g, op, 0);
            if (child_op && child_op->opcode == OP_SORT && n > 0) {
                ray_t* sort_input = exec_node(g, op_child(g, child_op, 0));
                if (!sort_input || RAY_IS_ERR(sort_input)) return sort_input;
                ray_t* tbl = (sort_input->type == RAY_TABLE) ? sort_input : g->table;
                /* Under a lazy selection, the keys first and only the
                 * winners' rows of the other columns (late
                 * materialization); else compact the table and sort */
                if (g->selection && tbl && !RAY_IS_ERR(tbl) && tbl->type == RAY_TABLE) {
                    ray_t* late = exec_sort_head_sel(g, child_op, tbl, g->selection, n);
                    if (late) {
                        ray_release(sort_input);      /* owned: see OP_SORT */
                        ray_release(g->selection);
                        g->selection = NULL;
                        return late;
                    }
                }
                if (g->selection && tbl && !RAY_IS_ERR(tbl) && tbl->type == RAY_TABLE) {
                    ray_t* compacted = sel_compact(g, tbl, g->selection, NULL, 0);
                    if (compacted && !RAY_IS_ERR(compacted) &&   /* the alloc fallback: see OP_SORT */
                        ray_table_nrows(compacted) != ray_rowsel_meta(g->selection)->total_pass) {
                        ray_release(compacted);
                        compacted = ray_error("oom", NULL);
                    }
                    ray_release(sort_input);          /* owned: see OP_SORT */
                    ray_release(g->selection);
                    g->selection = NULL;
                    sort_input = compacted;
                    tbl = compacted;
                }
                /* All-pass where: over a parted source: see OP_SORT */
                if (!g->selection && tbl && tbl->type == RAY_TABLE) {
                    ray_t* flat = flatten_parted_table(tbl, g->table);
                    if (flat) {
                        ray_release(sort_input);
                        if (RAY_IS_ERR(flat)) return flat;
                        sort_input = flat;
                        tbl = flat;
                    }
                }
                ray_t* result = exec_sort(g, child_op, tbl, n);
                exec_release_tmp(g, sort_input);
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
                } else if (g->selection && tbl->type == RAY_TABLE &&
                           group_has_keys(g, child_op) &&
                           !group_reads_vector_const(g, find_ext(g, child_op->id))) {
                    /* A keyless group (an all-aggregate select) is one row
                     * whatever the limit, and its aggregates read the
                     * selection themselves: compacting a no-match where:
                     * handed exec_group an empty table, which has no rows to
                     * reduce and came back with no row at all (#679). */
                    int needs = 0;
                    int64_t nc = ray_table_ncols(tbl);
                    for (int64_t c = 0; c < nc; c++) {
                        ray_col_meta_t cm;
                        if (ray_table_col_meta_idx(tbl, c, &cm) && !RAY_IS_PARTED(cm.type)
                            && cm.type != RAY_MAPCOMMON) {
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
                if (owned_tbl) { ray_graph_unbind_table(g, owned_tbl); ray_release(owned_tbl); }
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
                    ray_release(filter_input);        /* owned: see OP_SORT */
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
                    ray_release(filter_input);
                    return pred;
                }

                ray_t* result = exec_filter_head(ftbl, pred, n);
                ray_release(pred);
                exec_release_tmp(g, filter_input);
                /* Top-level statement GC catches intermediates. */
                return result;
            } else {
                input = exec_node(g, op_child(g, op, 0));
            }
            if (!input || RAY_IS_ERR(input)) return input;
            if (g->selection && input->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, input, g->selection, NULL, 0);
                ray_release(input);
                ray_release(g->selection);
                g->selection = NULL;
                input = compacted;
                if (!input || RAY_IS_ERR(input)) return input;
            }
            if (input->type == RAY_TABLE) {
                int64_t ncols = ray_table_ncols(input);
                int64_t nrows = ray_table_nrows(input);
                if (n > nrows) n = nrows;
                ray_t* result = ray_table_new(ncols);
                for (int64_t c = 0; c < ncols; c++) {
                    ray_t* col = ray_table_col_idx(input, c);
                    int64_t name_id = ray_table_col_name(input, c);
                    if (col && RAY_IS_ERR(col)) {
                        ray_release(result);
                        ray_release(input);
                        return col;
                    }
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
                        int64_t hcn = 0;
                        const int64_t* hcnt = parted_table_counts(input, &hcn);
                        if (base == RAY_STR) {
                            head_vec = parted_head_str_c(sp, col->len, n, hcnt, hcn);
                        } else if (base == RAY_LIST) {
                            head_vec = parted_head_list_c(sp, col->len, n, hcnt, hcn);
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
                                    int64_t take = parted_seg_rows(segs, s, hcnt, hcn);
                                    if (take > remaining) take = remaining;
                                    if (!segs[s]) {
                                        /* a partition without the column */
                                        parted_fill_nulls(head_vec, dst_off, take);
                                        dst_off += take;
                                        remaining -= take;
                                        continue;
                                    }
                                    parted_copy_cells(ray_data(head_vec), base,
                                                      ba, dst_off, segs[s], 0,
                                                      take);
                                    seg_nulls |= segs[s]->attrs & RAY_ATTR_HAS_NULLS;
                                    dst_off += take;
                                    remaining -= take;
                                }
                                /* Null propagation: conservatively propagate
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
                    } else if (col->type == RAY_LIST) {
                        ray_t* lcol = list_slice_retain(col, 0, n);
                        result = ray_table_add_col(result, name_id, lcol);
                        ray_release(lcol);
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
                            /* copied ray_str_t descriptors still reference
                             * the source's pool by offset (issue #404) */
                            if (col->type == RAY_STR)
                                col_propagate_str_pool(head_vec, col);
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
            if (input->type == RAY_LIST) {
                ray_t* lres = list_slice_retain(input, 0, n);
                ray_release(input);
                return lres;
            }
            uint8_t esz = col_esz(input);
            ray_t* result = col_vec_new(input, n);
            if (result && !RAY_IS_ERR(result)) {
                result->len = n;
                memcpy(ray_data(result), ray_data(input), (size_t)n * esz);
                col_propagate_nulls_range(result, 0, input, 0, n);
                if (input->type == RAY_SYM)
                    ray_sym_vec_adopt_domain(result, input);
                if (input->type == RAY_STR)
                    col_propagate_str_pool(result, input);
            }
            ray_release(input);
            return result;
        }

        case OP_TAIL: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            ray_t* input = exec_node(g, op_child(g, op, 0));
            if (!input || RAY_IS_ERR(input)) return input;
            int64_t n = ext ? ext->sym : 10;
            if (g->selection && input->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, input, g->selection, NULL, 0);
                ray_release(input);
                ray_release(g->selection);
                g->selection = NULL;
                input = compacted;
                if (!input || RAY_IS_ERR(input)) return input;
            }
            if (input->type == RAY_TABLE) {
                int64_t ncols = ray_table_ncols(input);
                int64_t nrows = ray_table_nrows(input);
                if (n > nrows) n = nrows;
                int64_t skip = nrows - n;
                ray_t* result = ray_table_new(ncols);
                for (int64_t c = 0; c < ncols; c++) {
                    ray_t* col = ray_table_col_idx(input, c);
                    int64_t name_id = ray_table_col_name(input, c);
                    if (col && RAY_IS_ERR(col)) {
                        ray_release(result);
                        ray_release(input);
                        return col;
                    }
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
                        int64_t tcn = 0;
                        const int64_t* tcnt = parted_table_counts(input, &tcn);
                        if (base == RAY_STR) {
                            tail_vec = parted_tail_str_c(tsp, col->len, n, tcnt, tcn);
                        } else if (base == RAY_LIST) {
                            tail_vec = parted_tail_list_c(tsp, col->len, n, tcnt, tcn);
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
                                    int64_t take = parted_seg_rows(segs, s, tcnt, tcn);
                                    if (take > remaining) take = remaining;
                                    dst -= take;
                                    if (!segs[s]) {
                                        /* a partition without the column */
                                        parted_fill_nulls(tail_vec, dst, take);
                                        remaining -= take;
                                        continue;
                                    }
                                    parted_copy_cells(ray_data(tail_vec), base,
                                                      tba, dst, segs[s],
                                                      segs[s]->len - take, take);
                                    seg_nulls |= segs[s]->attrs & RAY_ATTR_HAS_NULLS;
                                    remaining -= take;
                                }
                                /* Null propagation: conservatively propagate
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
                    } else if (col->type == RAY_LIST) {
                        ray_t* lcol = list_slice_retain(col, skip, n);
                        result = ray_table_add_col(result, name_id, lcol);
                        ray_release(lcol);
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
                            /* copied ray_str_t descriptors still reference
                             * the source's pool by offset (issue #404) */
                            if (col->type == RAY_STR)
                                col_propagate_str_pool(tail_vec, col);
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
            if (input->type == RAY_LIST) {
                ray_t* lres = list_slice_retain(input, skip, n);
                ray_release(input);
                return lres;
            }
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
                if (input->type == RAY_STR)
                    col_propagate_str_pool(result, input);
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
        case OP_STR_FIND: {
            return exec_str_find(g, op);
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
                    ray_t* src_col = ray_table_slot(input, name_id);
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
                    /* A scalar atom becomes a one-element column, widened
                     * below only if another column carries the input's
                     * rows.  Broadcast here, it posed as such a column and
                     * widened the scalars beside it (#698). */
                    if (vec->type < 0) {
                        ray_t* col = ray_enlist_fn(&vec, 1);
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

            /* A scalar column, an atom enlisted above or one computed as a
             * one-element vector (`(+ (sum x) 0)`, an `if` whose parts are
             * all scalar), is broadcast when another column has the input's
             * rows.  Left alone, the table took its row count from whichever
             * column came first and its columns disagreed in length.  When
             * every column is a scalar the one-row result stays. */
            int64_t nr_in = ray_table_nrows(input);
            if (result && !RAY_IS_ERR(result)) {
                int64_t rc = ray_table_ncols(result);
                /* A collection projection may change the row count. Use
                 * its length for scalar broadcasting; unequal non-scalar
                 * lengths are rejected below, independently of column order. */
                int64_t broadcast_len = nr_in;
                bool has_collection = false;
                for (int64_t c = 0; c < rc; c++) {
                    ray_col_meta_t lm;   /* metadata: an encoded column stays encoded */
                    if (!ray_table_col_meta_idx(result, c, &lm) || lm.type < 0) continue;
                    int64_t len = lm.len;   /* a parted column: its rows across segments */
                    if (len == 1) continue;
                    if (!has_collection) broadcast_len = len;
                    has_collection = true;
                }
                bool has_full = false;
                if (nr_in != 1) {
                    for (int64_t c = 0; c < rc; c++) {
                        ray_col_meta_t cm;
                        /* a parted column counts its rows across segments */
                        if (ray_table_col_meta_idx(result, c, &cm) && cm.type > 0 && cm.len == nr_in) { has_full = true; break; }
                    }
                } else {
                    /* On a one-row input a scalar and a row column have the
                     * same length: read the ops instead, so a scalar-only
                     * select under a failing where: is still one row. */
                    for (uint32_t c = 0; c < n_cols && !has_full; c++)
                        if (!op_tree_is_scalar(g, columns[c])) has_full = true;
                }
                /* A select of literals alone (`{s: 1}`) reduces nothing and
                 * keeps the input's rows.  Beside an aggregate a literal is
                 * one more scalar, as in a keyless group. */
                if (!has_full) {
                    bool all_const = true;
                    for (uint32_t c = 0; c < n_cols && all_const; c++)
                        if (!op_tree_is_const(g, columns[c])) all_const = false;
                    has_full = all_const;
                }
                if (has_collection) has_full = true;
                /* Every column a scalar: the reductions inside them already
                 * walked the where: selection (exec_reduction), so the
                 * one-row result is final.  Left installed, the selection
                 * (built for the input's rows) met the one-row result in
                 * ray_execute_inner's final gather (#675). */
                if (!has_full && g->selection) {
                    ray_release(g->selection);
                    g->selection = NULL;
                }
                /* A LIST here is an atom enlist has no vector for (F32). */
                for (int64_t c = 0; broadcast_len != 1 && has_full && c < rc; c++) {
                    ray_col_meta_t cm;
                    if (!ray_table_col_meta_idx(result, c, &cm) || cm.type < 0 ||
                        RAY_IS_PARTED(cm.type) || cm.type == RAY_MAPCOMMON ||
                        cm.len != 1) continue;
                    /* One input row does not turn a row-aligned expression
                     * into a scalar. Only constants/reductions may expand. */
                    if (nr_in == 1 && !op_tree_is_scalar(g, columns[c])) continue;
                    ray_t* cv = ray_table_col_idx(result, c);
                    if (cv && RAY_IS_ERR(cv)) {
                        ray_release(result);
                        g->table = saved_table;
                        ray_release(input);
                        return cv;
                    }
                    ray_t* n_obj = make_i64(broadcast_len);
                    ray_t* wide = n_obj ? ray_take_fn(cv, n_obj) : ray_error("oom", NULL);
                    if (n_obj) ray_release(n_obj);
                    if (!wide || RAY_IS_ERR(wide)) {
                        ray_release(result);
                        g->table = saved_table;
                        ray_release(input);
                        return wide ? wide : ray_error("oom", NULL);
                    }
                    ray_table_set_col_idx(result, c, wide);
                    ray_release(wide);
                }
                /* Length-changing constant projections must not silently
                 * truncate or pad the other columns during materialization.
                 * A parted column with missing segments (ragged parted
                 * table) is measured through the source's partition counts,
                 * which the generic check does not know. */
                ray_t* shape_err = parted_result_validate(result, input);
                if (shape_err) {
                    ray_release(result);
                    g->table = saved_table;
                    ray_release(input);
                    return shape_err;
                }
                if (g->selection) {
                    ray_t* rows_err = NULL;
                    int64_t proj_rows = parted_table_rows(result, input, &rows_err);
                    if (rows_err || proj_rows != nr_in) {
                        ray_t* err = rows_err ? rows_err : ray_error("length",
                            "select: collection projection has %lld rows; where requires %lld input-aligned rows",
                            (long long)proj_rows, (long long)nr_in);
                        ray_release(result);
                        g->table = saved_table;
                        ray_release(input);
                        return err;
                    }
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
            ray_t* a_col = ray_table_col_idx(accum, c);
            ray_t* p_col = ray_table_col_idx(partial, c);
            if ((a_col && RAY_IS_ERR(a_col)) || (p_col && RAY_IS_ERR(p_col))) {
                bool a_bad = a_col && RAY_IS_ERR(a_col);
                if (a_bad && p_col && RAY_IS_ERR(p_col)) ray_error_free(p_col);
                ray_release(merged);
                return a_bad ? a_col : p_col;
            }
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
 * For each parted column, extracts segs[seg_idx] without decoding it (a
 * generation-2 segment stays a RAY_ENCCOL until an operator needs it).
 * MAPCOMMON columns are materialized for segment seg_idx: the partition key
 * value is broadcast to fill seg_rows elements.
 * Non-parted columns are retained as-is. */
ray_t* build_segment_table(ray_t* parted_tbl, int32_t seg_idx) {
    int64_t ncols = ray_table_ncols(parted_tbl);
    ray_t* seg_tbl = ray_table_new(ncols);
    if (!seg_tbl || RAY_IS_ERR(seg_tbl)) return seg_tbl;

    /* The segment's row count: the partition-key counts, else the first
     * parted column's segment (a ragged table may lack a column in this
     * partition; its counts still give the rows). */
    int64_t seg_rows = 0;
    {
        int64_t ncnt = 0;
        const int64_t* cnt = parted_table_counts(parted_tbl, &ncnt);
        if (cnt && seg_idx < ncnt) seg_rows = cnt[seg_idx];
        else for (int64_t c = 0; c < ncols; c++) {
            ray_col_meta_t m;
            if (ray_table_col_meta_idx(parted_tbl, c, &m) && RAY_IS_PARTED(m.type)) {
                ray_t* seg = seg_idx < m.n_segs ? ray_table_parted_seg_idx(parted_tbl, c, seg_idx) : NULL;
                if (seg) seg_rows = ray_table_col_nrows(seg);
                break;
            }
        }
    }

    for (int64_t c = 0; c < ncols; c++) {
        int64_t name_id = ray_table_col_name(parted_tbl, c);
        ray_col_meta_t m;
        if (!ray_table_col_meta_idx(parted_tbl, c, &m)) continue;

        if (m.type == RAY_MAPCOMMON) {
            ray_t* col = ray_table_col_idx(parted_tbl, c);  /* MAPCOMMON: plain */
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
        } else if (RAY_IS_PARTED(m.type)) {
            if (seg_idx >= m.n_segs) {
                ray_release(seg_tbl);
                return ray_error("schema", NULL);
            }
            ray_t* seg = ray_table_parted_seg_idx(parted_tbl, c, seg_idx);
            if (!seg) {
                /* A partition without this column (a ragged parted table
                 * built elsewhere than ray_read_parted, whose missing
                 * segments are lazy all-null segments): its rows are the
                 * type's null. */
                int8_t base = (int8_t)RAY_PARTED_BASETYPE(m.type);
                ray_t* nulls;
                if (base == RAY_STR) {
                    nulls = ray_vec_new(RAY_STR, seg_rows);
                    for (int64_t r = 0; nulls && !RAY_IS_ERR(nulls) && r < seg_rows; r++) {
                        nulls = ray_str_vec_append(nulls, "", 0);
                        if (nulls && !RAY_IS_ERR(nulls)) ray_vec_set_null(nulls, r, true);
                    }
                } else if (base == RAY_LIST) {
                    nulls = ray_list_new(seg_rows);
                    if (nulls && !RAY_IS_ERR(nulls)) {
                        ray_t** d = (ray_t**)ray_data(nulls);
                        for (int64_t r = 0; r < seg_rows; r++) { ray_retain(RAY_NULL_OBJ); d[r] = RAY_NULL_OBJ; }
                        nulls->len = seg_rows;
                    }
                } else {
                    nulls = typed_vec_new(base, 0, seg_rows);
                    if (nulls && !RAY_IS_ERR(nulls)) {
                        nulls->len = seg_rows;
                        parted_fill_nulls(nulls, 0, seg_rows);
                        /* the domain of the first present segment */
                        for (int64_t s = 0; base == RAY_SYM && s < m.n_segs; s++) {
                            ray_t* rep = ray_table_parted_seg_idx(parted_tbl, c, s);
                            if (!rep) continue;
                            if (rep->type == RAY_ENCCOL) ray_enccol_adopt_domain(nulls, rep);
                            else ray_sym_vec_adopt_domain(nulls, rep);
                            break;
                        }
                    }
                }
                if (!nulls || RAY_IS_ERR(nulls)) {
                    ray_release(seg_tbl);
                    return nulls ? nulls : ray_error("oom", NULL);
                }
                seg_tbl = ray_table_add_col(seg_tbl, name_id, nulls);
                ray_release(nulls);
                continue;
            }
            if (ray_table_col_nrows(seg) != seg_rows) {        /* segments of one partition align */
                ray_release(seg_tbl);
                return ray_error("length", "parted table: segment %d of column %lld has %lld rows, expected %lld",
                                 (int)seg_idx, (long long)c, (long long)ray_table_col_nrows(seg), (long long)seg_rows);
            }
            seg_tbl = ray_table_add_col(seg_tbl, name_id, seg);   /* encoded or plain, not decoded */
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

/* Resolved pointwise layouts shared by streaming admission and the stricter
 * row-subset proof.  This is NOT op_is_elementwise's executor-dispatch set. */
enum { EXPR_NONE, EXPR_UNARY, EXPR_BINARY, EXPR_THIRD, EXPR_CONCAT, EXPR_SET };
static unsigned pointwise_layout(uint16_t opc) {
    switch (opc) {
        case OP_NEG: case OP_ABS: case OP_NOT: case OP_SQRT:
        case OP_LOG: case OP_EXP: case OP_CEIL: case OP_FLOOR: case OP_ROUND:
        case OP_SIN: case OP_ASIN: case OP_COS: case OP_ACOS:
        case OP_TAN: case OP_ATAN: case OP_RECIPROCAL: case OP_SIGNUM:
        case OP_ISNULL: case OP_CAST:
        case OP_UPPER: case OP_LOWER: case OP_STRLEN: case OP_TRIM:
        case OP_EXTRACT: case OP_DATE_TRUNC:
            return EXPR_UNARY;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_IDIV: case OP_MOD:
        case OP_POW: case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
        case OP_GT: case OP_GE: case OP_AND: case OP_OR: case OP_MIN2: case OP_MAX2:
        case OP_LIKE: case OP_ILIKE: case OP_STR_FIND:
            return EXPR_BINARY;
        case OP_IF: case OP_SUBSTR: case OP_REPLACE:
            return EXPR_THIRD;
        case OP_CONCAT:
            return EXPR_CONCAT;
        case OP_IN: case OP_NOT_IN:
            return EXPR_SET;
        default:
            return EXPR_NONE;
    }
}

/* Streaming keeps its historical structural permissions and root/constant
 * policies.  Sharing the classification does not widen its admission. */
static bool op_streamable(uint16_t opc) {
    if (pointwise_layout(opc)) return true;
    switch (opc) {
        case OP_SCAN: case OP_FILTER: case OP_SELECT: case OP_ALIAS:
        case OP_MATERIALIZE:
            return true;
        default:
            return false;
    }
}

/* A bounded rewrite-safety proof, not an execution validator for arbitrary
 * malformed graphs.  Shapes: 0 unproved, 1 scalar, 2 default-source rows.
 * Set literals are checked on their edge and NEVER cached as value shapes. */
#define SUBSET_NODE_LIMIT 4096
#define SUBSET_DEPTH_LIMIT 64
typedef struct {
    ray_graph_t* g;
    uint8_t state[SUBSET_NODE_LIMIT]; /* 0 unseen, 1 active, 2 done */
    uint8_t shape[SUBSET_NODE_LIMIT];
} subset_proof_t;

static bool subset_data_atom(ray_t* v) {
    return v && v->type <= -RAY_BOOL && v->type >= -RAY_STR;
}

static ray_op_t* subset_node(subset_proof_t* p, uint32_t id) {
    if (id >= p->g->node_count) return NULL;
    ray_op_t* n = &p->g->nodes[id];
    if (n->id != id || (n->flags & (OP_FLAG_DEAD | OP_FLAG_INVALID_SCAN))) return NULL;
    return n;
}

static ray_op_ext_t* subset_ext(subset_proof_t* p, ray_op_t* n) {
    ray_op_ext_t* e = find_ext(p->g, n->id);
    return e && e->base.opcode == n->opcode && e->base.arity == n->arity ? e : NULL;
}

static bool subset_set(subset_proof_t* p, uint32_t id) {
    ray_op_t* n = subset_node(p, id);
    if (!n || n->opcode != OP_CONST || n->arity != 0) return false;
    ray_op_ext_t* e = subset_ext(p, n);
    /* Flat typed vectors only: excludes LIST, callable, lazy and table values.
     * No data reads or mutation; the held literal is fixed during the rewrite. */
    return e && e->literal && e->literal->type >= RAY_BOOL &&
           e->literal->type <= RAY_STR && e->literal->len >= 0;
}

static uint8_t subset_visit(subset_proof_t* p, uint32_t id, unsigned depth) {
    if (depth >= SUBSET_DEPTH_LIMIT) return 0;
    ray_op_t* n = subset_node(p, id);
    if (!n || p->state[id] == 1) return 0;
    if (p->state[id] == 2) return p->shape[id];
    p->state[id] = 1;
    uint8_t shape = 0;
    if (n->opcode == OP_CONST || n->opcode == OP_SCAN) {
        ray_op_ext_t* e = subset_ext(p, n);
        if (n->arity != 0 || !e) return 0;
        if (n->opcode == OP_CONST) {
            if (!subset_data_atom(e->literal)) return 0;
            shape = 1;
        } else {
            uint16_t source;
            memcpy(&source, e->base.pad, sizeof(source));
            if (source != 0) return 0;
            shape = 2;
        }
    } else {
        unsigned layout = pointwise_layout(n->opcode);
        if (!layout || n->arity != (layout == EXPR_UNARY ? 1 : 2)) return 0;
        ray_op_ext_t* e = NULL;
        if (layout == EXPR_THIRD || layout == EXPR_CONCAT ||
            n->opcode == OP_EXTRACT || n->opcode == OP_DATE_TRUNC) {
            e = subset_ext(p, n);
            if (!e) return 0;
        }
        /* Tail allocation capacity is not recorded: do not silently omit
         * hidden arguments or read an unverifiable variadic tail. */
        if (layout == EXPR_CONCAT && e->sym != 2) return 0;
        if (n->opcode == OP_EXTRACT &&
            (e->sym < RAY_EXTRACT_YEAR || e->sym > RAY_EXTRACT_EPOCH)) return 0;
        if (n->opcode == OP_DATE_TRUNC &&
            e->sym != RAY_EXTRACT_DAY && e->sym != RAY_EXTRACT_SECOND) return 0;
        if (n->opcode == OP_CAST &&
            (n->out_type < RAY_BOOL || n->out_type > RAY_STR)) return 0;
        uint8_t a = subset_visit(p, n->in_id[0], depth + 1);
        if (!a) return 0;
        /* These special executors own row shape through their first input;
         * do not infer row output merely from a parameter/unchosen branch. */
        if ((layout == EXPR_THIRD || n->opcode == OP_LIKE ||
             n->opcode == OP_ILIKE || n->opcode == OP_STR_FIND) && a != 2) return 0;
        shape = a;
        if (layout == EXPR_SET) {
            /* Atom-left IN returns a one-cell vector, not scalar broadcast. */
            if (a != 2 || !subset_set(p, n->in_id[1])) return 0;
        } else if (n->arity == 2) {
            uint8_t b = subset_visit(p, n->in_id[1], depth + 1);
            if (!b) return 0;
            if ((n->opcode == OP_LIKE || n->opcode == OP_ILIKE ||
                 n->opcode == OP_STR_FIND || n->opcode == OP_REPLACE) && b != 1) return 0;
            if (b == 2) shape = 2;
        }
        if (layout == EXPR_THIRD) {
            uint8_t c = subset_visit(p, e->third_in, depth + 1); /* ID zero is valid */
            if (!c || (n->opcode == OP_REPLACE && c != 1)) return 0;
            if (c == 2) shape = 2;
        }
        /* Numeric primitive scalar dispatch is established.  Decline other
         * scan-free compositions unless their atom return is explicit. */
        if (shape == 1 && !op_is_elementwise(n->opcode) &&
            n->opcode != OP_UPPER && n->opcode != OP_LOWER &&
            n->opcode != OP_TRIM && n->opcode != OP_STRLEN &&
            n->opcode != OP_EXTRACT && n->opcode != OP_DATE_TRUNC) return 0;
    }
    p->state[id] = 2;
    return p->shape[id] = shape;
}

bool ray_expr_subset_invariant(ray_graph_t* g, uint32_t root_id) {
    if (!g || !g->nodes || !g->node_count || g->node_count > SUBSET_NODE_LIMIT ||
        g->node_count > g->node_cap || g->ext_count > g->ext_cap ||
        (g->ext_count && !g->ext_nodes)) return false;
    subset_proof_t p = { .g = g };
    return subset_visit(&p, root_id, 0) != 0;
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
bool dag_can_stream(ray_graph_t* g, ray_op_t* root) {
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

static ray_t* ray_execute_inner(ray_graph_t* g, ray_op_t* root, bool reset_cancel);
static _Thread_local int exec_depth;   /* nesting of ray_execute on this thread */

/* Validate compiler-marked missing scans before execution.  Optimizer rewrites
 * may replace a root or eliminate such a scan from the live DAG (for example
 * count/missing-column), but that must not turn an invalid reference into a
 * result.  Do not revalidate every OP_SCAN against g->table here: graph APIs
 * legitimately scan derived columns produced by upstream operators. */
static ray_t* validate_scan_columns(ray_graph_t* g) {
    if (!g) return ray_error("nyi", NULL);

    for (uint32_t i = 0; i < g->node_count; i++) {
        ray_op_t* op = &g->nodes[i];
        if (op->opcode != OP_SCAN ||
            !(op->flags & OP_FLAG_INVALID_SCAN)) continue;

        ray_op_ext_t* ext = find_ext(g, op->id);
        if (!ext) return ray_error("schema", "scan metadata missing");
        ray_t* name = ray_sym_str(ext->sym);
        if (name)
            return ray_error("schema", "column '%.*s' not found",
                             (int)ray_str_len(name), ray_str_ptr(name));
        return ray_error("schema", "column not found");
    }
    return NULL;
}

void exec_scan_init(ray_pool_scan_t* s) {
    s->n = 0;
    s->rows = -1;
    s->gate = NULL;
    s->gate_log2 = 0;
}

void exec_scan_add_col(ray_pool_scan_t* s, ray_t* col) {
    if (!col || col->mmod != 1 || col->type <= 0 || RAY_IS_PARTED(col->type) ||
        col->type == RAY_MAPCOMMON || col->type == RAY_LIST || col->type == RAY_STR) return;
    if (col->len <= 0 || (s->rows >= 0 && col->len != s->rows)) return;
    size_t esz = ray_sym_elem_size(col->type, col->attrs);
    if (esz == 0) return;
    s->rows = col->len;
    const uint8_t* base = (const uint8_t*)ray_data(col);
    for (uint32_t k = 0; k < s->n; k++) if (s->base[k] == base) return;
    size_t bytes = (size_t)col->len * esz;
    if (s->n >= RAY_POOL_SCAN_MAX || ray_vm_resident(base, bytes)) return;
    /* The pages at its start that are resident already, up to 4 MiB: read
     * ahead with the header when the column was opened (see warm).  They
     * run on from the first, so halving finds where they end. */
    size_t lo = 0, hi = (bytes < ((size_t)4 << 20) ? bytes : ((size_t)4 << 20)) / 4096;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ray_vm_resident(base + mid * 4096, 1)) lo = mid + 1; else hi = mid;
    }
    s->base[s->n] = base;
    s->esz[s->n] = (uint32_t)esz;
    s->warm[s->n] = (int64_t)(lo * 4096 / esz);
    s->n++;
}

/* The graph's scanned columns, when they are mapped (see exec_scan_add_col);
 * NULL when no scan reads a mapped column. */
static const ray_pool_scan_t* exec_scan_columns(ray_graph_t* g, ray_pool_scan_t* s) {
    exec_scan_init(s);
    for (uint32_t i = 0; i < g->node_count; i++) {
        ray_op_t* op = &g->nodes[i];
        if (op->opcode != OP_SCAN) continue;
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (!ext) continue;
        uint16_t tid = 0;
        memcpy(&tid, ext->base.pad, sizeof(tid));
        ray_t* tbl = (tid > 0 && g->tables && (uint32_t)(tid - 1) < g->n_tables)
                   ? g->tables[tid - 1] : g->table;
        if (!tbl || tbl->type != RAY_TABLE) continue;
        if (!ray_table_get_col_enc(tbl, ext->sym))   /* generation 2 reads by range */
            exec_scan_add_col(s, ray_table_col(tbl, ext->sym));   /* a plain slot: no decode, no error */
    }
    return s->rows >= 0 ? s : NULL;
}

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
    ray_t* scan_err = validate_scan_columns(g);
    if (scan_err) return scan_err;
    /* Scan read-ahead: a registration over mapped columns made by the
     * caller (a select, which also knows its filter) stands; otherwise the
     * graph's own mapped scans are registered for its dispatches. */
    ray_pool_scan_t scan;
    const ray_pool_scan_t* prev_scan = ray_pool_scan_get();
    bool own_scan = !prev_scan || prev_scan->rows < 0;
    if (own_scan) ray_pool_scan_set(exec_scan_columns(g, &scan));
    /* Never NULL: callers test RAY_IS_ERR, which is false for NULL, and then
     * dereference.  The inner paths hand NULL back unchanged when a node,
     * a compaction or a merge fails to allocate (the flat path's
     * `return result`, the streaming path's seg_tbl / partial / merged
     * returns); `(times <lazy> ...)` would then crash in loop_count. */
    /* The cancel flag is reset at the outermost call only.  A node's own
     * path may execute a sub-graph (the grouped expression bridge
     * materializing an input the element-wise compiler declines, #756);
     * were that call to reset the flag, a cancel that arrived before it
     * would be dropped and the workers would run the remaining tickets. */
    bool outermost = exec_depth++ == 0;
    ray_t* result = ray_execute_inner(g, root, outermost);
    exec_depth--;
    if (own_scan) ray_pool_scan_set(prev_scan);
    return result ? result : ray_error("oom", NULL);
}

/* Flatten one parted/mapcommon column into a dense vector (mirrors the
 * OP_SCAN cold-path flatten at the top of this file).  Returns an owned
 * flat vector or a RAY_ERR.  Precondition: col is RAY_MAPCOMMON or
 * RAY_IS_PARTED.  `tbl` is the column's table, or NULL: its partition
 * counts give a NULL segment (a partition without the column, see
 * store/part.c) its rows, filled with the type's null. */
static ray_t* flatten_parted_col(ray_t* col, ray_t* tbl) {
    if (col->type == RAY_MAPCOMMON)
        return materialize_mapcommon(col);
    int8_t base = (int8_t)RAY_PARTED_BASETYPE(col->type);
    ray_t** segs = (ray_t**)ray_data(col);
    int64_t ncnt = 0;
    const int64_t* cnt = parted_table_counts(tbl, &ncnt);
    if (base == RAY_STR)
        return parted_flatten_str_c(segs, col->len, cnt, ncnt);
    if (base == RAY_LIST)
        return parted_flatten_list_c(segs, col->len, cnt, ncnt);
    int64_t total = parted_rows_counted(col, cnt, ncnt);
    uint8_t sba = (base == RAY_SYM) ? parted_sym_max_attrs(segs, col->len) : 0;
    ray_t* flat = typed_vec_new(base, sba, total);
    if (!flat || RAY_IS_ERR(flat)) return flat ? flat : ray_error("oom", NULL);
    flat->len = total;
    int64_t off = 0;
    uint8_t seg_nulls = 0;
    for (int64_t s = 0; s < col->len; s++) {
        if (!segs[s] || RAY_IS_ERR(segs[s])) {
            int64_t n = parted_seg_rows(segs, s, cnt, ncnt);
            parted_fill_nulls(flat, off, n);
            off += n;
            continue;
        }
        if (segs[s]->len <= 0) continue;
        parted_copy_cells(ray_data(flat), base, sba, off,
                          segs[s], 0, segs[s]->len);
        seg_nulls |= segs[s]->attrs & RAY_ATTR_HAS_NULLS;
        off += segs[s]->len;
    }
    /* Null propagation: parted_copy_cells copies sentinel
     * bit-patterns but not HAS_NULLS — OR it across segments. */
    flat->attrs |= seg_nulls;
    /* Raw-copied SYM cell ids resolve over the partitions' shared
     * domain (first SYM segment is the rep). */
    if (flat->type == RAY_SYM)
        ray_sym_vec_adopt_domain(flat, sym_domain_rep(col));
    return flat;
}

/* The keep-set a select published for this ray_execute's filter
 * finalization (__VM->filt_keep: the columns its group reads),
 * or none (keep every column).  Depth-gated so a nested subquery's own
 * finalization does not consume it. */
static void exec_filt_keep(const int64_t** keep, int* keep_n) {
    *keep = NULL; *keep_n = 0;
    if (__VM && __VM->filt_keep && __VM->filt_keep_n > 0
        && __VM->eval_depth == __VM->filt_depth) {
        *keep = __VM->filt_keep; *keep_n = __VM->filt_keep_n;
    }
}

/* The partition-count source for `tbl`: itself when it carries the
 * partition-key column, else `counts_src` (the source table a projection
 * of parted columns was taken from, whose partitions it shares). */
static ray_t* parted_counts_table(ray_t* tbl, ray_t* counts_src) {
    int64_t n = 0;
    if (parted_table_counts(tbl, &n)) return tbl;
    return counts_src;
}

/* Rows of column c of `tbl`, a parted one counted through the partition
 * counts; reads metadata only, except a parted column (its segments).
 * -1 with *err set when the column fails to load. */
static int64_t parted_col_rows(ray_t* tbl, int64_t c, const int64_t* cnt, int64_t ncnt, ray_t** err) {
    *err = NULL;
    ray_col_meta_t m;
    if (!ray_table_col_meta_idx(tbl, c, &m)) return -2;   /* no column */
    if (!RAY_IS_PARTED(m.type) || m.enc) return m.len;   /* an encoded parted column has every segment */
    ray_t* col = ray_table_col_idx(tbl, c);
    if (col && RAY_IS_ERR(col)) { *err = col; return -1; }
    if (!col) return -2;
    return parted_rows_counted(col, cnt, ncnt);
}

/* ray_table_validate_rectangular for a result that may hold parted columns
 * with missing segments: every column must span the rows the partition
 * counts (of `tbl`, else `counts_src`) give a parted column. */
static ray_t* parted_result_validate(ray_t* tbl, ray_t* counts_src) {
    int64_t ncols = ray_table_ncols(tbl);
    bool any_parted = false;
    for (int64_t c = 0; c < ncols && !any_parted; c++) {
        ray_col_meta_t m;
        if (ray_table_col_meta_idx(tbl, c, &m) && RAY_IS_PARTED(m.type)) any_parted = true;
    }
    if (!any_parted) return ray_table_validate_rectangular(tbl, "select");
    int64_t ncnt = 0;
    const int64_t* cnt = parted_table_counts(parted_counts_table(tbl, counts_src), &ncnt);
    int64_t want = -1;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* err = NULL;
        int64_t n = parted_col_rows(tbl, c, cnt, ncnt, &err);
        if (err) return err;
        if (n < 0) continue;
        if (want < 0) want = n;
        else if (n != want)
            return ray_error("domain", "select: ragged columns (column %lld has %lld rows, expected %lld)",
                             (long long)c, (long long)n, (long long)want);
    }
    return NULL;
}

/* Rows of a table whose first column may be a parted column with missing
 * segments: counted through the partition counts. */
static int64_t parted_table_rows(ray_t* tbl, ray_t* counts_src, ray_t** err_out) {
    ray_col_meta_t m;
    if (ray_table_ncols(tbl) <= 0 || !ray_table_col_meta_idx(tbl, 0, &m) || !RAY_IS_PARTED(m.type))
        return ray_table_nrows(tbl);
    int64_t ncnt = 0;
    const int64_t* cnt = parted_table_counts(parted_counts_table(tbl, counts_src), &ncnt);
    ray_t* err = NULL;
    int64_t n = parted_col_rows(tbl, 0, cnt, ncnt, &err);
    if (err) { *err_out = err; return -1; }   /* the column's own load error, owned by the caller */
    return n;
}

/* A flattened copy of `tbl` when any column is parted / mapcommon, built
 * without mutating the (possibly stored) source; NULL when every column is
 * already dense.  Returns an owned table or a RAY_ERR.  A ragged table (a
 * partition without some column) flattens to the table's row count in
 * every column, the missing rows as typed nulls; the counts come from
 * `tbl`, or from `counts_src` when `tbl` is a projection without the
 * partition-key column.  Only the parted columns are read (an encoded one
 * through its materialization); the others are carried as they are. */
static ray_t* flatten_parted_table(ray_t* tbl, ray_t* counts_src) {
    int64_t ncols = ray_table_ncols(tbl);
    ray_t* ctbl = parted_counts_table(tbl, counts_src);
    bool any_parted = false;
    for (int64_t c = 0; c < ncols; c++) {
        ray_col_meta_t m;
        if (ray_table_col_meta_idx(tbl, c, &m) && (RAY_IS_PARTED(m.type) || m.type == RAY_MAPCOMMON)) {
            any_parted = true; break;
        }
    }
    if (!any_parted) return NULL;
    ray_t* out = ray_table_new(ncols);
    for (int64_t c = 0; out && !RAY_IS_ERR(out) && c < ncols; c++) {
        ray_col_meta_t m;
        if (!ray_table_col_meta_idx(tbl, c, &m)) continue;
        int64_t name = ray_table_col_name(tbl, c);
        if (RAY_IS_PARTED(m.type) || m.type == RAY_MAPCOMMON) {
            ray_t* col = ray_table_col_idx(tbl, c);
            if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : ray_error("oom", NULL); }
            ray_t* flat = flatten_parted_col(col, ctbl);
            PARTED_FLAT_DCHECK(ctbl, flat);
            if (!flat || RAY_IS_ERR(flat)) {
                ray_release(out);
                return flat ? flat : ray_error("oom", NULL);
            }
            out = ray_table_add_col(out, name, flat);
            ray_release(flat);
        } else {
            ray_t* slot = ray_table_slot_idx(tbl, c);   /* encoded or plain, not decoded */
            if (slot) out = ray_table_add_col(out, name, slot);
        }
    }
    return out ? out : ray_error("oom", NULL);
}

static ray_t* ray_execute_inner(ray_graph_t* g, ray_op_t* root, bool reset_cancel) {
    ray_enc_note_exec_start();
    if (!g || !root) return ray_error("nyi", NULL);

    /* Lazy-init the global thread pool on first call */
    ray_pool_t* pool = ray_pool_get();

    /* Reset cancellation flag at the start of each query (not of a nested
     * execution — see ray_execute) */
    if (pool && reset_cancel)
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
        /* Every parted and MAPCOMMON column covers the same rows: a
         * flattened column is sized by its own row count and consumers
         * index up to the table's, so a shorter one (#724) is refused here
         * rather than read past its end.  A missing (NULL) segment of a
         * hand-built wrapper counts its partition's rows, from the
         * partition-key counts: the segment table and every flatten fill
         * them with the type's null (ray_read_parted builds none). */
        int64_t parted_rows = -1;
        int64_t pncnt = 0;
        const int64_t* pcnt = parted_table_counts(g->table, &pncnt);
        for (int64_t c = 0; c < ray_table_ncols(g->table); c++) {
            ray_col_meta_t m;
            if (!ray_table_col_meta_idx(g->table, c, &m)) continue;
            if (RAY_IS_PARTED(m.type) || m.type == RAY_MAPCOMMON) {
                int64_t rows = m.len;
                if (RAY_IS_PARTED(m.type) && !m.enc && pcnt) {
                    ray_t* pc = ray_table_col_idx(g->table, c);   /* a plain wrapper: no decode */
                    if (pc && !RAY_IS_ERR(pc)) rows = parted_rows_counted(pc, pcnt, pncnt);
                }
                if (parted_rows < 0) parted_rows = rows;
                else if (rows != parted_rows)
                    return ray_error("length", "parted table: column %lld has %lld rows, expected %lld",
                                     (long long)c, (long long)rows, (long long)parted_rows);
            }
            if (RAY_IS_PARTED(m.type)) {
                if (seg_count == 0)
                    seg_count = (int32_t)m.n_segs;
                else if ((int32_t)m.n_segs != seg_count)
                    return ray_error("schema", NULL);
            } else if (m.type != RAY_MAPCOMMON) {
                has_flat = true;
            }
        }
        if (has_flat)
            seg_count = 0;  /* fall back to flat materialization */
    }

    if (seg_count == 0 || !dag_can_stream(g, root)) {
        /* Non-parted table or DAG contains ops that need specialized merge:
         * use existing flat-materialization path. */
        /* Only the call that armed the memo tears it down: a nested
         * execution of the same graph leaves the outer memo alone. */
        bool memo_armed = exec_memo_begin(g, root);
        ray_t* result = exec_node(g, root);
        if (memo_armed) exec_memo_end(g);
        if (g->selection && result && !RAY_IS_ERR(result)
            && result->type == RAY_TABLE) {
            /* Projection-aware compaction: a select publishes the columns
             * its group reads via __VM->filt_keep before this
             * ray_execute; gather only those columns. */
            const int64_t* keep = NULL; int keep_n = 0;
            exec_filt_keep(&keep, &keep_n);
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
                ray_col_meta_t m;
                if (ray_table_col_meta_idx(result, c, &m)
                    && (RAY_IS_PARTED(m.type) || m.type == RAY_MAPCOMMON)) {
                    any_parted = true; break;
                }
            }
            if (any_parted) {
                ray_t* ctbl = parted_counts_table(result, g->table);
                /* the same keep-set as the partial filter's sel_compact */
                const int64_t* keep = NULL; int keep_n = 0;
                exec_filt_keep(&keep, &keep_n);
                ray_t* out = ray_table_new(ncols);
                for (int64_t c = 0; out && !RAY_IS_ERR(out) && c < ncols; c++) {
                    int64_t name = ray_table_col_name(result, c);
                    if (keep_n > 0) {
                        bool kept = false;
                        for (int j = 0; j < keep_n && !kept; j++) kept = keep[j] == name;
                        if (!kept) continue;
                    }
                    ray_t* col = ray_table_col_idx(result, c);
                    if (col && RAY_IS_ERR(col)) { ray_release(out); ray_release(result); return col; }
                    if (col && (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON)) {
                        ray_t* flat = flatten_parted_col(col, ctbl);
                        PARTED_FLAT_DCHECK(ctbl, flat);
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
            const int64_t* keep = NULL; int keep_n = 0;
            exec_filt_keep(&keep, &keep_n);
            ray_t* compacted = sel_compact(g, partial, g->selection, keep, keep_n);
            ray_release(partial);
            ray_release(g->selection);
            g->selection = NULL;
            partial = compacted;
        }

        g->table = saved_table;
        ray_graph_unbind_table(g, seg_tbl);
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
                ray_col_meta_t m;
                if (!ray_table_col_meta_idx(saved_table, c, &m)) continue;
                int8_t base = m.type;
                if (m.type == RAY_MAPCOMMON) {
                    ray_t* col = ray_table_col_idx(saved_table, c);
                    ray_t** mc = (ray_t**)ray_data(col);
                    base = mc[0] ? mc[0]->type : RAY_I64;
                } else if (RAY_IS_PARTED(m.type)) {
                    base = (int8_t)RAY_PARTED_BASETYPE(m.type);
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
            ray_graph_unbind_table(g, empty_tbl);
            ray_release(empty_tbl);
        }
    }

    if (!result) return ray_error("oom", NULL);
    return result;
}
