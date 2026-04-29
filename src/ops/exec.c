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
#include "ops/rowsel.h"
#include "mem/sys.h"

/* Global profiler instance (zero-initialized = inactive) */
ray_profile_t g_ray_profile;

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

/* Phase 1+2 use dispatch_n with explicit task-to-range mapping so that
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

/* Phase 3: per-block gather — one task per source block */
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
    /* Fallback for small arrays or no pool */
    if (!pool || n < PG_MIN || n > INT32_MAX || src_rows > INT32_MAX) {
        multi_gather_ctx_t mg = { .idx = idx, .ncols = 0 };
        for (int64_t c = 0; c < ncols && c < MGATHER_MAX_COLS; c++) {
            mg.srcs[c] = srcs[c]; mg.dsts[c] = dsts[c]; mg.esz[c] = esz[c];
            mg.ncols++;
        }
        if (pool) ray_pool_dispatch(pool, multi_gather_fn, &mg, n);
        else      multi_gather_fn(&mg, 0, 0, n);
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
        /* Fallback to regular gather on allocation failure */
        multi_gather_ctx_t mg = { .idx = idx, .ncols = 0 };
        for (int64_t c = 0; c < ncols && c < MGATHER_MAX_COLS; c++) {
            mg.srcs[c] = srcs[c]; mg.dsts[c] = dsts[c]; mg.esz[c] = esz[c];
            mg.ncols++;
        }
        ray_pool_dispatch(pool, multi_gather_fn, &mg, n);
        return;
    }

    /* Phase 1: parallel histogram (dispatch_n for deterministic task→range) */
    pg_hist_ctx_t hctx = {
        .idx = idx, .hist = hist, .n_parts = n_parts,
        .n = n, .n_tasks = nw,
    };
    ray_pool_dispatch_n(pool, pg_hist_fn, &hctx, nw);

    /* Phase 2: prefix sum → per-task scatter offsets + partition boundaries */
    int64_t running = 0;
    for (int64_t p = 0; p < n_parts; p++) {
        part_off[p] = running;
        for (uint32_t t = 0; t < nw; t++) {
            offsets[t * n_parts + p] = running;
            running += hist[t * n_parts + p];
        }
    }
    part_off[n_parts] = running;

    /* Phase 3: parallel route (same task→range mapping as histogram) */
    pg_route_ctx_t rctx = {
        .idx = idx, .rdest = rdest, .rsrc = rsrc,
        .offsets = offsets, .n_parts = n_parts,
        .n = n, .n_tasks = nw,
    };
    ray_pool_dispatch_n(pool, pg_route_fn, &rctx, nw);

    /* Phase 4: parallel per-block gather */
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
        else return ray_error("type", NULL);
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
    else return ray_error("type", NULL);

    size_t esz = (vt == RAY_BOOL) ? 1 : 8;
    ray_t* vec = ray_vec_new(vt, nrows);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    uint8_t elem[8] = {0};
    memcpy(elem, &atom->i64, esz);
    for (int64_t r = 0; r < nrows; r++) {
        vec = ray_vec_append(vec, elem);
        if (RAY_IS_ERR(vec)) return vec;
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
    uint8_t*       ob;
    int8_t         ct;
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
    uint8_t* ob = c->ob;
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

    if (c->use_double) {
        const double* svf = c->svf;
        for (int64_t i = start; i < end; i++) {
            bool row_null = c->col_atom_null ||
                            (c->col_has_nulls && !c->col_is_atom &&
                             ray_vec_is_null(col, i));
            if (row_null) { ob[i] = 0; continue; }
            double cv;
            if (c->col_is_atom) cv = (ct == RAY_F64) ? col->f64 : (double)col->i64;
            else IN_READ_F64(cv, i);
            int found = 0;
            for (int64_t j = 0; j < sv_len; j++)
                if (cv == svf[j]) { found = 1; break; }
            ob[i] = (uint8_t)(found ^ negate);
        }
    } else {
        const int64_t* svi = c->svi;
        for (int64_t i = start; i < end; i++) {
            bool row_null = c->col_atom_null ||
                            (c->col_has_nulls && !c->col_is_atom &&
                             ray_vec_is_null(col, i));
            if (row_null) { ob[i] = 0; continue; }
            int64_t cv;
            if (c->col_is_atom) cv = col->i64;
            else IN_READ_I64(cv, i);
            int found = 0;
            for (int64_t j = 0; j < sv_len; j++)
                if (cv == svi[j]) { found = 1; break; }
            ob[i] = (uint8_t)(found ^ negate);
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
static ray_t* exec_in(ray_graph_t* g, ray_op_t* op, ray_t* col, ray_t* set) {
    (void)g;
    bool negate = (op->opcode == OP_NOT_IN);

    int64_t col_len = ray_is_atom(col) ? 1 : col->len;
    int64_t set_len = ray_is_atom(set) ? 1 : set->len;

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

    int8_t ct = ray_is_atom(col) ? (int8_t)(-col->type) : col->type;
    int8_t st = ray_is_atom(set) ? (int8_t)(-set->type) : set->type;
    if (RAY_IS_PARTED(ct)) ct = (int8_t)RAY_PARTED_BASETYPE(ct);
    if (RAY_IS_PARTED(st)) st = (int8_t)RAY_PARTED_BASETYPE(st);

    if (ct == RAY_STR || st == RAY_STR)
        return ray_error("nyi", "OP_IN on RAY_STR not yet implemented");

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

    ray_t* out = ray_vec_new(RAY_BOOL, col_len);
    if (!out || RAY_IS_ERR(out)) return out;
    out->len = col_len;
    uint8_t* ob = (uint8_t*)ray_data(out);

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
    double  svf_stack[32];
    int64_t svi_stack[32];
    double* svf = svf_stack;
    int64_t* svi = svi_stack;
    ray_t* sv_hdr = NULL;
    if (set_len > 32) {
        size_t bytes = (size_t)set_len * (use_double ? sizeof(double) : sizeof(int64_t));
        sv_hdr = ray_alloc(bytes);
        if (!sv_hdr) { ray_release(out); return ray_error("oom", NULL); }
        if (use_double) svf = (double*)ray_data(sv_hdr);
        else            svi = (int64_t*)ray_data(sv_hdr);
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
            if (!RAY_ATOM_IS_NULL(set)) { svi[0] = set->i64; sv_len = 1; }
        } else if (set_len > 0) {
            for (int64_t i = 0; i < set_len; i++) {
                if (set_has_nulls && ray_vec_is_null(set, i)) continue;
                READ_I64(svi[sv_len], set, st, i);
                sv_len++;
            }
        }
    }

    in_worker_ctx_t in_ctx = {
        .col = col,
        .svf = svf, .svi = svi, .sv_len = sv_len,
        .ob = ob, .ct = ct,
        .col_has_nulls = col_has_nulls,
        .col_atom_null = col_atom_null,
        .col_is_atom = ray_is_atom(col),
        .use_double = use_double,
        .negate = negate,
    };

    ray_pool_t* pool = ray_pool_get();
    if (pool && col_len >= RAY_PARALLEL_THRESHOLD && !ray_is_atom(col))
        ray_pool_dispatch(pool, exec_in_worker, &in_ctx, col_len);
    else
        exec_in_worker(&in_ctx, 0, 0, col_len);

    if (sv_hdr) ray_free(sv_hdr);

    #undef READ_I64
    #undef READ_F64
    #undef CLASSIFY
    return out;
}

/* ============================================================================
 * Recursive executor
 * ============================================================================ */

/* Is this opcode a "heavy" pipeline breaker worth profiling? */
static inline bool op_is_heavy(uint16_t opc) {
    return opc == OP_FILTER || opc == OP_SORT || opc == OP_GROUP ||
           opc == OP_JOIN   || opc == OP_WINDOW_JOIN || opc == OP_SELECT ||
           opc == OP_HEAD   || opc == OP_TAIL || opc == OP_WINDOW ||
           opc == OP_PIVOT  ||
           (opc >= OP_EXPAND && opc <= OP_KNN_RERANK);
}

ray_t* exec_node(ray_graph_t* g, ray_op_t* op) {
    if (!op) return ray_error("nyi", NULL);

    /* Per-op cancellation checkpoint. Long fused pipelines iterate
     * exec_node many times; this catches Ctrl-C between operators
     * without adding cost to the per-row hot path. */
    if (ray_interrupted()) return ray_error("cancel", "interrupted");

    bool heavy = op_is_heavy(op->opcode);
    bool profiling = g_ray_profile.active && heavy;
    const char* oname = NULL;
    if (heavy) {
        oname = ray_opcode_name(op->opcode);
        /* Relabel progress without touching counters — leaf ops that
         * drive their own rows_done/rows_total still work; ops that
         * don't get a spinner-style indeterminate bar until they
         * either finish or emit their own update. */
        ray_progress_label(oname, NULL);
        if (profiling) ray_profile_span_start(oname);
    }

    ray_t* _prof_result = exec_node_inner(g, op);

    if (profiling)
        ray_profile_span_end(oname);

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
                            ? parted_first_attrs(sps, col->len) : 0;
                ray_t* flat = typed_vec_new(base, sba, total);
                if (!flat || RAY_IS_ERR(flat)) return ray_error("oom", NULL);
                flat->len = total;
                ray_t** segs = sps;
                size_t esz = (size_t)ray_sym_elem_size(base, sba);
                int64_t off = 0;
                for (int64_t s = 0; s < col->len; s++) {
                    if (segs[s] && segs[s]->len > 0 &&
                        parted_seg_esz_ok(segs[s], base, (uint8_t)esz)) {
                        memcpy((char*)ray_data(flat) + off * esz,
                               ray_data(segs[s]), (size_t)segs[s]->len * esz);
                        off += segs[s]->len;
                    } else if (segs[s] && segs[s]->len > 0) {
                        memset((char*)ray_data(flat) + off * esz, 0,
                               (size_t)segs[s]->len * esz);
                        off += segs[s]->len;
                    }
                }
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

        case OP_TIL: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            if (!ext || !ext->literal) return ray_error("nyi", NULL);
            int64_t n = ext->literal->i64;
            if (n <= 0) return ray_vec_new(RAY_I64, 0);
            ray_t* vec = ray_vec_new(RAY_I64, n);
            if (!vec || RAY_IS_ERR(vec)) return vec;
            vec->len = n;
            int64_t* d = (int64_t*)ray_data(vec);
            for (int64_t i = 0; i < n; i++) d[i] = i;
            return vec;
        }

        /* Membership: col IN set_vec */
        case OP_IN: case OP_NOT_IN: {
            ray_t* col = exec_node(g, op->inputs[0]);
            if (!col || RAY_IS_ERR(col)) return col;
            ray_t* set = exec_node(g, op->inputs[1]);
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
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
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
            /* Fallback: recursive per-node evaluation */
            if (op->arity == 1) {
                ray_t* input = exec_node(g, op->inputs[0]);
                if (!input || RAY_IS_ERR(input)) return input;
                ray_t* result = exec_elementwise_unary(g, op, input);
                ray_release(input);
                return result;
            } else {
                ray_t* lhs = exec_node(g, op->inputs[0]);
                ray_t* rhs = exec_node(g, op->inputs[1]);
                if (!lhs || RAY_IS_ERR(lhs)) { if (rhs && !RAY_IS_ERR(rhs)) ray_release(rhs); return lhs; }
                if (!rhs || RAY_IS_ERR(rhs)) { ray_release(lhs); return rhs; }
                ray_t* result = exec_elementwise_binary(g, op, lhs, rhs);
                ray_release(lhs);
                ray_release(rhs);
                return result;
            }
        }

        /* Reductions */
        case OP_SUM: case OP_PROD: case OP_MIN: case OP_MAX:
        case OP_COUNT: case OP_AVG: case OP_FIRST: case OP_LAST:
        case OP_STDDEV: case OP_STDDEV_POP: case OP_VAR: case OP_VAR_POP: {
            ray_t* input = exec_node(g, op->inputs[0]);
            if (!input || RAY_IS_ERR(input)) return input;
            /* Compact lazy selection before reducing — filters may have
             * set g->selection without materializing a compacted table. */
            bool own_input = (input != g->table);
            if (g->selection && input->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, input, g->selection);
                if (own_input) ray_release(input);
                ray_release(g->selection);
                g->selection = NULL;
                input = compacted;
                own_input = true;
            }
            ray_t* result = exec_reduction(g, op, input);
            if (own_input) ray_release(input);
            return result;
        }

        case OP_COUNT_DISTINCT: {
            ray_t* input = exec_node(g, op->inputs[0]);
            if (!input || RAY_IS_ERR(input)) return input;
            ray_t* result = exec_count_distinct(g, op, input);
            ray_release(input);
            return result;
        }

        case OP_FILTER: {
            /* HAVING fusion: FILTER(GROUP) — evaluate the predicate against
             * the GROUP result rather than the original input table.
             * SCAN nodes in the predicate tree resolve column names via
             * g->table, so we temporarily swap it to the GROUP output. */
            ray_op_t* filter_child = op->inputs[0];
            if (filter_child && filter_child->opcode == OP_GROUP) {
                ray_t* group_result = exec_node(g, filter_child);
                if (!group_result || RAY_IS_ERR(group_result))
                    return group_result;

                ray_t* saved_table = g->table;
                ray_t* saved_sel   = g->selection;
                g->table     = group_result;
                g->selection = NULL;

                ray_t* pred = exec_node(g, op->inputs[1]);

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

            ray_t* input = exec_node(g, op->inputs[0]);
            ray_t* pred  = exec_node(g, op->inputs[1]);
            if (!input || RAY_IS_ERR(input)) { if (pred && !RAY_IS_ERR(pred)) ray_release(pred); return input; }
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
            ray_t* input = exec_node(g, op->inputs[0]);
            if (!input || RAY_IS_ERR(input)) return input;
            ray_t* tbl = (input->type == RAY_TABLE) ? input : g->table;
            /* Compact lazy selection before sort (needs dense data) */
            if (g->selection && tbl && !RAY_IS_ERR(tbl) && tbl->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, tbl, g->selection);
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
                    ray_op_ext_t* kx = find_ext(g, gext->keys[0]->id);
                    int64_t src_sym = ray_sym_intern("_src", 4);
                    if (kx && kx->base.opcode == OP_SCAN && kx->sym == src_sym) {
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
                ray_t* compacted = sel_compact(g, tbl, g->selection);
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
            ray_t* left = exec_node(g, op->inputs[0]);
            ray_t* right = exec_node(g, op->inputs[1]);
            if (!left || RAY_IS_ERR(left)) { if (right && !RAY_IS_ERR(right)) ray_release(right); return left; }
            if (!right || RAY_IS_ERR(right)) { ray_release(left); return right; }
            /* Compact lazy selection before join (needs dense data) */
            if (g->selection && left && !RAY_IS_ERR(left) && left->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, left, g->selection);
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
            ray_t* left = exec_node(g, op->inputs[0]);
            ray_t* right = exec_node(g, op->inputs[1]);
            if (!left || RAY_IS_ERR(left)) { if (right && !RAY_IS_ERR(right)) ray_release(right); return left; }
            if (!right || RAY_IS_ERR(right)) { ray_release(left); return right; }
            if (g->selection && left && !RAY_IS_ERR(left) && left->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, left, g->selection);
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
            ray_t* left = exec_node(g, op->inputs[0]);
            ray_t* right = exec_node(g, op->inputs[1]);
            if (!left || RAY_IS_ERR(left)) { if (right && !RAY_IS_ERR(right)) ray_release(right); return left; }
            if (!right || RAY_IS_ERR(right)) { ray_release(left); return right; }
            if (g->selection && left && !RAY_IS_ERR(left) && left->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, left, g->selection);
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
            ray_t* input = exec_node(g, op->inputs[0]);
            if (!input || RAY_IS_ERR(input)) return input;
            ray_t* wdf = (input->type == RAY_TABLE) ? input : g->table;
            /* Compact lazy selection before window (needs dense data) */
            if (g->selection && wdf && !RAY_IS_ERR(wdf) && wdf->type == RAY_TABLE) {
                ray_t* compacted = sel_compact(g, wdf, g->selection);
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
            ray_op_t* child_op = op->inputs[0];
            if (child_op && child_op->opcode == OP_SORT) {
                ray_t* sort_input = exec_node(g, child_op->inputs[0]);
                if (!sort_input || RAY_IS_ERR(sort_input)) return sort_input;
                ray_t* tbl = (sort_input->type == RAY_TABLE) ? sort_input : g->table;
                /* Compact lazy selection before sort */
                if (g->selection && tbl && !RAY_IS_ERR(tbl) && tbl->type == RAY_TABLE) {
                    ray_t* compacted = sel_compact(g, tbl, g->selection);
                    if (sort_input != g->table) ray_release(sort_input);
                    ray_release(g->selection);
                    g->selection = NULL;
                    sort_input = compacted;
                    tbl = compacted;
                }
                ray_t* result = exec_sort(g, child_op, tbl, n);
                if (sort_input != g->table) ray_release(sort_input);
                return result;
            }

            /* HEAD(GROUP) optimization: pass limit hint to exec_group
             * so it can short-circuit the per-partition loop when all
             * GROUP BY keys are MAPCOMMON.  The normal HEAD logic below
             * still trims the result to N rows regardless. */
            ray_t* input;
            if (child_op && child_op->opcode == OP_GROUP) {
                ray_t* tbl = g->table;
                if (!tbl || RAY_IS_ERR(tbl)) return tbl;
                ray_t* owned_tbl = NULL;
                if (g->selection && tbl->type == RAY_TABLE) {
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
                        ray_t* compacted = sel_compact(g, tbl, g->selection);
                        if (!compacted || RAY_IS_ERR(compacted)) return compacted;
                        ray_release(g->selection);
                        g->selection = NULL;
                        owned_tbl = compacted;
                        tbl = compacted;
                    }
                }
                input = exec_group(g, child_op, tbl, n);
                if (owned_tbl) ray_release(owned_tbl);
            } else if (child_op && child_op->opcode == OP_FILTER) {
                /* HEAD(FILTER): early-termination filter — gather only
                 * the first N matching rows instead of all matches. */
                ray_t* filter_input = exec_node(g, child_op->inputs[0]);
                if (!filter_input || RAY_IS_ERR(filter_input))
                    return filter_input;

                /* Compact lazy selection before filter evaluation */
                ray_t* ftbl = (filter_input->type == RAY_TABLE)
                           ? filter_input : g->table;
                if (g->selection && ftbl && ftbl->type == RAY_TABLE) {
                    ray_t* compacted = sel_compact(g, ftbl, g->selection);
                    if (filter_input != g->table) ray_release(filter_input);
                    ray_release(g->selection);
                    g->selection = NULL;
                    filter_input = compacted;
                    ftbl = compacted;
                }

                /* Swap table for predicate evaluation */
                ray_t* saved_table = g->table;
                g->table = ftbl;
                ray_t* pred = exec_node(g, child_op->inputs[1]);
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
                return result;
            } else {
                input = exec_node(g, op->inputs[0]);
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
                                       ? parted_first_attrs(sp, col->len) : 0;
                            uint8_t esz = ray_sym_elem_size(base, ba);
                            head_vec = typed_vec_new(base, ba, n);
                            if (head_vec && !RAY_IS_ERR(head_vec)) {
                                head_vec->len = n;
                                ray_t** segs = (ray_t**)ray_data(col);
                                int64_t remaining = n;
                                int64_t dst_off = 0;
                                for (int64_t s = 0; s < col->len && remaining > 0; s++) {
                                    if (!segs[s]) continue;
                                    int64_t take = segs[s]->len;
                                    if (take > remaining) take = remaining;
                                    if (parted_seg_esz_ok(segs[s], base, esz)) {
                                        memcpy((char*)ray_data(head_vec) + dst_off * esz,
                                               ray_data(segs[s]), (size_t)take * esz);
                                    } else {
                                        memset((char*)ray_data(head_vec) + dst_off * esz,
                                               0, (size_t)take * esz);
                                    }
                                    dst_off += take;
                                    remaining -= take;
                                }
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
            }
            ray_release(input);
            return result;
        }

        case OP_TAIL: {
            ray_op_ext_t* ext = find_ext(g, op->id);
            ray_t* input = exec_node(g, op->inputs[0]);
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
                                        ? parted_first_attrs(tsp, col->len) : 0;
                            uint8_t esz = ray_sym_elem_size(base, tba);
                            tail_vec = typed_vec_new(base, tba, n);
                            if (tail_vec && !RAY_IS_ERR(tail_vec)) {
                                tail_vec->len = n;
                                ray_t** segs = (ray_t**)ray_data(col);
                                int64_t remaining = n;
                                int64_t dst = n;
                                for (int64_t s = col->len - 1; s >= 0 && remaining > 0; s--) {
                                    if (!segs[s]) continue;
                                    int64_t take = segs[s]->len;
                                    if (take > remaining) take = remaining;
                                    dst -= take;
                                    if (parted_seg_esz_ok(segs[s], base, esz)) {
                                        memcpy((char*)ray_data(tail_vec) + (size_t)dst * esz,
                                               (char*)ray_data(segs[s]) + (size_t)(segs[s]->len - take) * esz,
                                               (size_t)take * esz);
                                    } else {
                                        memset((char*)ray_data(tail_vec) + (size_t)dst * esz,
                                               0, (size_t)take * esz);
                                    }
                                    remaining -= take;
                                }
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
            return exec_node(g, op->inputs[0]);
        }

        case OP_MATERIALIZE: {
            return exec_node(g, op->inputs[0]);
        }

        case OP_SELECT: {
            /* Column projection: select/compute columns from input table */
            ray_t* input = exec_node(g, op->inputs[0]);
            if (!input || RAY_IS_ERR(input)) return input;
            if (input->type != RAY_TABLE) {
                ray_release(input);
                return ray_error("nyi", NULL);
            }
            ray_op_ext_t* ext = find_ext(g, op->id);
            if (!ext) { ray_release(input); return ray_error("nyi", NULL); }
            uint8_t n_cols = ext->sort.n_cols;
            ray_op_t** columns = ext->sort.columns;
            ray_t* result = ray_table_new(n_cols);

            /* Set g->table so SCAN nodes inside expressions resolve correctly */
            ray_t* saved_table = g->table;
            g->table = input;

            for (uint8_t c = 0; c < n_cols; c++) {
                if (columns[c]->opcode == OP_SCAN) {
                    /* Direct column reference — copy from input table */
                    ray_op_ext_t* col_ext = find_ext(g, columns[c]->id);
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
                    ray_t* vec = exec_node(g, columns[c]);
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
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* result = exec_expand(g, op, src);
            ray_release(src);
            return result;
        }

        case OP_VAR_EXPAND: {
            ray_t* start = exec_node(g, op->inputs[0]);
            if (!start || RAY_IS_ERR(start)) return start;
            ray_t* result = exec_var_expand(g, op, start);
            ray_release(start);
            return result;
        }

        case OP_SHORTEST_PATH: {
            ray_t* src = exec_node(g, op->inputs[0]);
            ray_t* dst = exec_node(g, op->inputs[1]);
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
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* dst = op->inputs[1] ? exec_node(g, op->inputs[1]) : NULL;
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
            ray_t* src = exec_node(g, op->inputs[0]);
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
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* result = exec_random_walk(g, op, src);
            ray_release(src);
            return result;
        }

        case OP_ASTAR: {
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* dst = exec_node(g, op->inputs[1]);
            if (!dst || RAY_IS_ERR(dst)) { ray_release(src); return dst; }
            ray_t* result = exec_astar(g, op, src, dst);
            ray_release(src); ray_release(dst);
            return result;
        }

        case OP_K_SHORTEST: {
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* dst = exec_node(g, op->inputs[1]);
            if (!dst || RAY_IS_ERR(dst)) { ray_release(src); return dst; }
            ray_t* result = exec_k_shortest(g, op, src, dst);
            ray_release(src); ray_release(dst);
            return result;
        }

        case OP_COSINE_SIM: {
            ray_t* emb = exec_node(g, op->inputs[0]);
            if (!emb || RAY_IS_ERR(emb)) return emb;
            ray_t* result = exec_cosine_sim(g, op, emb);
            ray_release(emb);
            return result;
        }
        case OP_EUCLIDEAN_DIST: {
            ray_t* emb = exec_node(g, op->inputs[0]);
            if (!emb || RAY_IS_ERR(emb)) return emb;
            ray_t* result = exec_euclidean_dist(g, op, emb);
            ray_release(emb);
            return result;
        }
        case OP_KNN: {
            ray_t* emb = exec_node(g, op->inputs[0]);
            if (!emb || RAY_IS_ERR(emb)) return emb;
            ray_t* result = exec_knn(g, op, emb);
            ray_release(emb);
            return result;
        }
        case OP_HNSW_KNN: {
            return exec_hnsw_knn(g, op);
        }
        case OP_ANN_RERANK: {
            ray_t* src = exec_node(g, op->inputs[0]);
            if (!src || RAY_IS_ERR(src)) return src;
            ray_t* result = exec_ann_rerank(g, op, src);
            ray_release(src);
            return result;
        }
        case OP_KNN_RERANK: {
            ray_t* src = exec_node(g, op->inputs[0]);
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
static ray_t* ray_result_merge(ray_t* accum, ray_t* partial) {
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

    return ray_error("type", NULL);
}

/* Build a flat table containing one segment's columns from a parted table.
 * For each parted column, extracts segs[seg_idx] as a flat vector.
 * MAPCOMMON columns are materialized for segment seg_idx: the partition key
 * value is broadcast to fill seg_rows elements.
 * Non-parted columns are retained as-is. */
static ray_t* build_segment_table(ray_t* parted_tbl, int32_t seg_idx) {
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
                return ray_error("type", NULL);
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
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
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
 *   OP_IF      — else branch: g->nodes[(uint32_t)(uintptr_t)ext->literal]
 *   OP_SUBSTR  — length arg:  g->nodes[(uint32_t)(uintptr_t)ext->literal]
 *   OP_REPLACE — replacement: g->nodes[(uint32_t)(uintptr_t)ext->literal]
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
        found |= subtree_has_default_scan(g, op->inputs[i], ok, visited);

    /* Walk hidden operands stored in ext nodes */
    if (opc == OP_SELECT) {
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (ext) {
            for (uint8_t c = 0; c < ext->sort.n_cols && *ok; c++)
                found |= subtree_has_default_scan(g, ext->sort.columns[c], ok, visited);
        }
    } else if (opc == OP_IF || opc == OP_SUBSTR || opc == OP_REPLACE) {
        /* 3rd operand stored as node index in ext->literal */
        ray_op_ext_t* ext = find_ext(g, op->id);
        if (ext) {
            uint32_t child_id = (uint32_t)(uintptr_t)ext->literal;
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
    ray_t* r = ray_execute_inner(g, root);
    /* End the current progress tracking session. A no-op when no
     * callback is registered; otherwise emits the final "100% done"
     * tick (only if the bar was actually shown). */
    ray_progress_end();
    return r;
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
            ray_t* compacted = sel_compact(g, result, g->selection);
            ray_release(result);
            ray_release(g->selection);
            g->selection = NULL;
            result = compacted;
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
            ray_t* compacted = sel_compact(g, partial, g->selection);
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
