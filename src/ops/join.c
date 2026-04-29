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

/* ── Hash helper (shared by radix and chained HT join paths) ──────────── */

static uint64_t hash_row_keys(ray_t** key_vecs, uint8_t n_keys, int64_t row) {
    uint64_t h = 0;
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* col = key_vecs[k];
        if (!col) continue;
        /* NULL key — produce unique hash that won't match any other row */
        if (ray_vec_is_null(col, row))
            return h ^ ((uint64_t)row * 0x9E3779B97F4A7C15ULL);
        uint64_t kh;
        if (col->type == RAY_F64)
            kh = ray_hash_f64(((double*)ray_data(col))[row]);
        else
            kh = ray_hash_i64(read_col_i64(ray_data(col), row, col->type, col->attrs));
        h = (k == 0) ? kh : ray_hash_combine(h, kh);
    }
    return h;
}

/* ============================================================================
 * Radix-partitioned hash join
 *
 * Four-phase pipeline:
 *   Phase 1: Partition both sides by radix bits of hash (parallel)
 *   Phase 2: Per-partition build + probe with open-addressing HT (parallel)
 *   Phase 3: Gather output columns from matched pairs (parallel)
 *   Phase 4: Fallback to chained HT for small joins (< RAY_PARALLEL_THRESHOLD)
 * ============================================================================ */

/* Partition entry: row index + cached hash */
typedef struct {
    uint32_t row_idx;
    uint32_t hash;
} join_radix_entry_t;

/* Per-partition descriptor */
typedef struct {
    join_radix_entry_t* entries;     /* partition buffer (from ray_alloc) */
    ray_t*               entries_hdr; /* ray_alloc header for freeing */
    uint32_t            count;       /* number of entries in partition */
} join_radix_part_t;

/* Choose radix bits so each partition's HT working set fits in cache.
 * HT working set per partition ≈ 2x right entries × 8B = 16B per right row. */
static uint8_t radix_join_bits(int64_t right_rows) {
    /* HT working set: 2x capacity × 8B slot = 16B per right row */
    size_t right_bytes = (size_t)right_rows * 16;
    if (right_bytes <= RAY_JOIN_L2_TARGET)
        return RAY_JOIN_MIN_RADIX;

    /* R = ceil(log2(right_bytes / L2_TARGET)) */
    uint8_t r = 0;
    size_t target = RAY_JOIN_L2_TARGET;
    while (target < right_bytes && r < RAY_JOIN_MAX_RADIX) {
        target *= 2;
        r++;
    }
    if (r < RAY_JOIN_MIN_RADIX) r = RAY_JOIN_MIN_RADIX;
    return r;
}

/* Context for parallel hash pre-computation */
typedef struct {
    ray_t**    key_vecs;
    uint8_t   n_keys;
    uint32_t* hashes;    /* output: hash[row] */
} join_radix_hash_ctx_t;

static void join_radix_hash_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    join_radix_hash_ctx_t* c = (join_radix_hash_ctx_t*)raw;
    for (int64_t r = start; r < end; r++)
        c->hashes[r] = (uint32_t)hash_row_keys(c->key_vecs, c->n_keys, r);
}

/* Context for parallel partition histogram + scatter (pre-computed hashes).
 * Uses fixed row assignment: task i processes rows [i*chunk, (i+1)*chunk).
 * This ensures histogram and scatter see the same row ranges per task,
 * enabling non-atomic per-worker scatter offsets. */
typedef struct {
    uint32_t* hashes;
    uint32_t  radix_mask;
    uint8_t   radix_shift;
    uint32_t  n_parts;
    uint32_t  n_workers;
    int64_t   nrows;
    uint32_t* histograms;   /* [n_workers][n_parts] flat array */
} join_radix_hist_ctx_t;

static void join_radix_hist_fn(void* raw, uint32_t wid, int64_t task_start, int64_t task_end) {
    (void)wid; (void)task_end;
    join_radix_hist_ctx_t* c = (join_radix_hist_ctx_t*)raw;
    /* Fixed row range for this task */
    uint32_t tid = (uint32_t)task_start;
    int64_t chunk = (c->nrows + (int64_t)c->n_workers - 1) / (int64_t)c->n_workers;
    int64_t start = (int64_t)tid * chunk;
    int64_t end = start + chunk;
    if (end > c->nrows) end = c->nrows;
    if (start >= c->nrows) return;

    uint32_t* hist = c->histograms + tid * c->n_parts;
    uint32_t mask = c->radix_mask;
    uint8_t shift = c->radix_shift;

    for (int64_t r = start; r < end; r++) {
        uint32_t part = (c->hashes[r] >> shift) & mask;
        hist[part]++;
    }
}

/* Context for parallel partition scatter with write-combining buffers.
 * Each worker writes to small local buffers (one per partition). When
 * a buffer fills, it flushes to the partition in a burst memcpy.
 * This converts random writes into sequential bursts, dramatically
 * improving cache utilization.
 *
 * Uses fixed per-worker row assignments (dispatch_n with n_workers tasks)
 * to match histogram phase, eliminating atomic operations. */
#define WCB_SIZE 64  /* entries per write-combine buffer */
typedef struct {
    uint32_t*           hashes;
    uint32_t            radix_mask;
    uint8_t             radix_shift;
    uint32_t            n_parts;
    join_radix_part_t*  parts;
    uint32_t*           offsets;     /* [n_workers][n_parts] per-worker write positions */
    int64_t             nrows;
    uint32_t            n_workers;
    _Atomic(uint8_t)    had_error;   /* set by any worker on OOM */
} join_radix_scatter_ctx_t;

static void join_radix_scatter_fn(void* raw, uint32_t wid, int64_t task_start, int64_t task_end) {
    (void)wid; (void)task_end;
    join_radix_scatter_ctx_t* c = (join_radix_scatter_ctx_t*)raw;
    uint32_t mask = c->radix_mask;
    uint8_t shift = c->radix_shift;
    uint32_t n_parts = c->n_parts;

    /* Fixed row range for this task (matches histogram) */
    uint32_t tid = (uint32_t)task_start;
    int64_t chunk = (c->nrows + (int64_t)c->n_workers - 1) / (int64_t)c->n_workers;
    int64_t ws = (int64_t)tid * chunk;
    int64_t we = ws + chunk;
    if (we > c->nrows) we = c->nrows;
    if (ws >= c->nrows) return;

    uint32_t* off = c->offsets + tid * n_parts;

    /* Write-combining: per-partition local buffers, flushed in bursts */
    uint32_t wcb_cnt_stack[1024];
    uint32_t* wcb_cnt_p = wcb_cnt_stack;
    ray_t* wcb_cnt_hdr = NULL;
    if (n_parts > 1024) {
        wcb_cnt_p = (uint32_t*)scratch_calloc(&wcb_cnt_hdr, (size_t)n_parts * sizeof(uint32_t));
        if (!wcb_cnt_p) {
            atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
            return;
        }
    } else {
        memset(wcb_cnt_stack, 0, (size_t)n_parts * sizeof(uint32_t));
    }

    /* Allocate per-partition local buffers */
    ray_t* local_hdr = NULL;
    join_radix_entry_t* local_buf = (join_radix_entry_t*)scratch_alloc(&local_hdr,
        (size_t)n_parts * WCB_SIZE * sizeof(join_radix_entry_t));
    if (!local_buf) {
        /* Fallback: direct write without buffering */
        for (int64_t r = ws; r < we; r++) {
            uint32_t h = c->hashes[r];
            uint32_t part = (h >> shift) & mask;
            uint32_t pos = off[part]++;
            c->parts[part].entries[pos].row_idx = (uint32_t)r;
            c->parts[part].entries[pos].hash = h;
        }
        if (wcb_cnt_hdr) scratch_free(wcb_cnt_hdr);
        return;
    }

    for (int64_t r = ws; r < we; r++) {
        uint32_t h = c->hashes[r];
        uint32_t part = (h >> shift) & mask;
        uint32_t idx = wcb_cnt_p[part];
        local_buf[part * WCB_SIZE + idx].row_idx = (uint32_t)r;
        local_buf[part * WCB_SIZE + idx].hash = h;
        idx++;
        if (idx == WCB_SIZE) {
            /* Flush buffer to partition */
            memcpy(&c->parts[part].entries[off[part]],
                   &local_buf[part * WCB_SIZE],
                   WCB_SIZE * sizeof(join_radix_entry_t));
            off[part] += WCB_SIZE;
            idx = 0;
        }
        wcb_cnt_p[part] = idx;
    }

    /* Flush remaining entries */
    for (uint32_t p = 0; p < n_parts; p++) {
        uint32_t cnt = wcb_cnt_p[p];
        if (cnt > 0) {
            memcpy(&c->parts[p].entries[off[p]],
                   &local_buf[p * WCB_SIZE],
                   (size_t)cnt * sizeof(join_radix_entry_t));
            off[p] += cnt;
        }
    }

    scratch_free(local_hdr);
    if (wcb_cnt_hdr) scratch_free(wcb_cnt_hdr);
}

/* Partition one side of the join. Returns array of join_radix_part_t[n_parts].
 * Caller must free each partition's entries_hdr and the parts array itself. */
static join_radix_part_t* join_radix_partition(ray_pool_t* pool, int64_t nrows,
                                      uint8_t radix_bits,
                                      uint32_t* hashes,
                                      ray_t** parts_hdr_out) {
    uint32_t n_parts = (uint32_t)1 << radix_bits;
    uint32_t mask = n_parts - 1;
    /* Use upper bits of hash for radix (lower bits used inside partition HT) */
    uint8_t shift = 32 - radix_bits;

    /* Allocate partition descriptor array */
    ray_t* parts_hdr;
    join_radix_part_t* parts = (join_radix_part_t*)scratch_calloc(&parts_hdr,
                            (size_t)n_parts * sizeof(join_radix_part_t));
    if (!parts) { *parts_hdr_out = NULL; return NULL; }
    *parts_hdr_out = parts_hdr;

    /* Step 1: Histogram — count rows per partition per worker.
     * n_workers must match dispatch: 1 when running serially so that the
     * single hist/scatter call covers all rows (chunk = nrows / 1). */
    uint32_t n_workers = (pool && nrows > RAY_PARALLEL_THRESHOLD) ? pool->n_workers + 1 : 1;
    ray_t* hist_hdr;
    uint32_t* histograms = (uint32_t*)scratch_calloc(&hist_hdr,
                             (size_t)n_workers * n_parts * sizeof(uint32_t));
    if (!histograms) { scratch_free(parts_hdr); *parts_hdr_out = NULL; return NULL; }

    join_radix_hist_ctx_t hctx = {
        .hashes = hashes,
        .radix_mask = mask, .radix_shift = shift,
        .n_parts = n_parts, .n_workers = n_workers,
        .nrows = nrows,
        .histograms = histograms,
    };
    if (pool && nrows > RAY_PARALLEL_THRESHOLD)
        ray_pool_dispatch_n(pool, join_radix_hist_fn, &hctx, n_workers);
    else
        join_radix_hist_fn(&hctx, 0, 0, 1);

    /* Compute partition sizes (sum across workers) */
    for (uint32_t p = 0; p < n_parts; p++) {
        uint32_t total = 0;
        for (uint32_t w = 0; w < n_workers; w++)
            total += histograms[w * n_parts + p];
        parts[p].count = total;
    }

    /* Allocate partition buffers */
    bool oom = false;
    for (uint32_t p = 0; p < n_parts; p++) {
        if (parts[p].count == 0) continue;
        parts[p].entries = (join_radix_entry_t*)scratch_alloc(&parts[p].entries_hdr,
                             (size_t)parts[p].count * sizeof(join_radix_entry_t));
        if (!parts[p].entries) {
            ray_heap_gc();
            ray_heap_release_pages();
            parts[p].entries = (join_radix_entry_t*)scratch_alloc(&parts[p].entries_hdr,
                                 (size_t)parts[p].count * sizeof(join_radix_entry_t));
            if (!parts[p].entries) { oom = true; break; }
        }
    }
    if (oom) {
        for (uint32_t p = 0; p < n_parts; p++)
            if (parts[p].entries_hdr) scratch_free(parts[p].entries_hdr);
        scratch_free(hist_hdr);
        scratch_free(parts_hdr);
        *parts_hdr_out = NULL;
        return NULL;
    }

    /* Step 2: Compute per-worker write offsets (prefix sum of histograms).
     * For each partition p, worker w's write offset =
     *   sum(histograms[0..w-1][p]) = global prefix for workers before w. */
    ray_t* off_hdr;
    uint32_t* offsets = (uint32_t*)scratch_alloc(&off_hdr,
                            (size_t)n_workers * n_parts * sizeof(uint32_t));
    if (!offsets) {
        for (uint32_t p = 0; p < n_parts; p++)
            if (parts[p].entries_hdr) scratch_free(parts[p].entries_hdr);
        scratch_free(hist_hdr);
        scratch_free(parts_hdr);
        *parts_hdr_out = NULL;
        return NULL;
    }
    for (uint32_t p = 0; p < n_parts; p++) {
        uint32_t running = 0;
        for (uint32_t w = 0; w < n_workers; w++) {
            offsets[w * n_parts + p] = running;
            running += histograms[w * n_parts + p];
        }
    }

    /* Step 3: Scatter rows into partition buffers (fixed row assignment, no atomics) */
    join_radix_scatter_ctx_t sctx = {
        .hashes = hashes,
        .radix_mask = mask, .radix_shift = shift,
        .n_parts = n_parts, .parts = parts,
        .offsets = offsets,
        .nrows = nrows, .n_workers = n_workers,
        .had_error = 0,
    };
    if (pool && nrows > RAY_PARALLEL_THRESHOLD)
        ray_pool_dispatch_n(pool, join_radix_scatter_fn, &sctx, n_workers);
    else
        join_radix_scatter_fn(&sctx, 0, 0, 1);

    scratch_free(off_hdr);
    scratch_free(hist_hdr);

    if (atomic_load_explicit(&sctx.had_error, memory_order_relaxed)) {
        for (uint32_t p = 0; p < n_parts; p++)
            if (parts[p].entries_hdr) scratch_free(parts[p].entries_hdr);
        scratch_free(parts_hdr);
        *parts_hdr_out = NULL;
        return NULL;
    }

    return parts;
}

/* ============================================================================
 * Join execution (parallel hash join)
 *
 * Three-phase pipeline:
 *   Phase 1 (sequential): Build chained hash table on right side
 *   Phase 2 (parallel):   Two-pass probe — count matches, prefix-sum, fill
 *   Phase 3 (parallel):   Column gather — assemble result columns
 * ============================================================================ */

/* Key equality helper — shared by count + fill phases */
static inline bool join_keys_eq(ray_t* const* l_vecs, ray_t* const* r_vecs, uint8_t n_keys,
                                 int64_t l, int64_t r) {
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* lc = l_vecs[k];
        ray_t* rc = r_vecs[k];
        if (!lc || !rc) return false;
        /* NULL != NULL in join predicates */
        if (ray_vec_is_null(lc, l) || ray_vec_is_null(rc, r)) return false;
        if (lc->type == RAY_F64) {
            if (((double*)ray_data(lc))[l] != ((double*)ray_data(rc))[r]) return false;
        } else {
            if (read_col_i64(ray_data(lc), l, lc->type, lc->attrs) !=
                read_col_i64(ray_data(rc), r, rc->type, rc->attrs)) return false;
        }
    }
    return true;
}

/* ── Per-partition open-addressing build + probe ─────────────────────── */

#define RADIX_HT_EMPTY UINT32_MAX

/* Per-partition single-pass build+probe context.
 * Each partition writes to its own local output buffer, then results
 * are consolidated into contiguous arrays afterward. */
typedef struct {
    join_radix_part_t*  l_parts;
    join_radix_part_t*  r_parts;
    ray_t**         l_key_vecs;
    ray_t**         r_key_vecs;
    uint8_t        n_keys;
    uint8_t        join_type;
    /* Per-partition output: pp_l[p], pp_r[p] are local buffers */
    int32_t**      pp_l;         /* per-partition left indices (int32_t) */
    int32_t**      pp_r;         /* per-partition right indices (int32_t) */
    ray_t**         pp_l_hdr;     /* allocation headers for freeing */
    ray_t**         pp_r_hdr;
    int64_t*       part_counts;  /* actual output count per partition */
    uint32_t*      pp_cap;       /* capacity per partition */
    _Atomic(uint8_t)* matched_right;
    _Atomic(uint8_t)  had_error;  /* set by any partition on OOM */
} join_radix_bp_ctx_t;

/* Grow per-partition output buffers (matched pair arrays).
 * Returns true on success, false on OOM (sets had_error). */
static inline bool bp_grow_bufs(join_radix_bp_ctx_t* c, uint32_t p,
                                 int32_t** pl, int32_t** pr,
                                 uint32_t* cap, uint32_t cnt) {
    if (cnt < *cap) return true;
    if (*cap > UINT32_MAX / 2) {
        atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
        return false;
    }
    uint32_t new_cap = *cap * 2;
    ray_t* nl_hdr; ray_t* nr_hdr;
    int32_t* nl = (int32_t*)scratch_alloc(&nl_hdr, (size_t)new_cap * sizeof(int32_t));
    int32_t* nr = (int32_t*)scratch_alloc(&nr_hdr, (size_t)new_cap * sizeof(int32_t));
    if (!nl || !nr) {
        if (nl_hdr) scratch_free(nl_hdr);
        if (nr_hdr) scratch_free(nr_hdr);
        atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
        return false;
    }
    memcpy(nl, *pl, (size_t)cnt * sizeof(int32_t));
    memcpy(nr, *pr, (size_t)cnt * sizeof(int32_t));
    scratch_free(c->pp_l_hdr[p]); scratch_free(c->pp_r_hdr[p]);
    *pl = nl; *pr = nr;
    c->pp_l_hdr[p] = nl_hdr; c->pp_r_hdr[p] = nr_hdr;
    *cap = new_cap;
    return true;
}

static void join_radix_build_probe_fn(void* raw, uint32_t wid, int64_t task_start, int64_t task_end) {
    (void)wid; (void)task_end;
    join_radix_bp_ctx_t* c = (join_radix_bp_ctx_t*)raw;
    uint32_t p = (uint32_t)task_start;

    join_radix_part_t* rp = &c->r_parts[p];
    join_radix_part_t* lp = &c->l_parts[p];

    if (rp->count == 0) {
        /* No right rows — emit unmatched left rows for LEFT/FULL */
        if (c->join_type >= 1 && lp->count > 0) {
            uint32_t cap = lp->count;
            int32_t* pl = (int32_t*)scratch_alloc(&c->pp_l_hdr[p], (size_t)cap * sizeof(int32_t));
            int32_t* pr = (int32_t*)scratch_alloc(&c->pp_r_hdr[p], (size_t)cap * sizeof(int32_t));
            if (pl && pr) {
                for (uint32_t i = 0; i < lp->count; i++) {
                    pl[i] = (int32_t)lp->entries[i].row_idx;
                    pr[i] = -1;
                }
                c->pp_l[p] = pl; c->pp_r[p] = pr;
                c->part_counts[p] = lp->count;
                c->pp_cap[p] = cap;
            } else {
                if (c->pp_l_hdr[p]) scratch_free(c->pp_l_hdr[p]);
                if (c->pp_r_hdr[p]) scratch_free(c->pp_r_hdr[p]);
                c->pp_l_hdr[p] = NULL; c->pp_r_hdr[p] = NULL;
                atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
            }
        }
        return;
    }

    /* Allocate per-partition output buffer.
     * Capacity = max(left, right) handles 1:1 and 1:N joins.
     * For N:M (overflow), we grow by re-allocating. */
    uint32_t init_cap = lp->count > rp->count ? lp->count : rp->count;
    if (init_cap < 64) init_cap = 64;
    int32_t* pl = (int32_t*)scratch_alloc(&c->pp_l_hdr[p], (size_t)init_cap * sizeof(int32_t));
    int32_t* pr = (int32_t*)scratch_alloc(&c->pp_r_hdr[p], (size_t)init_cap * sizeof(int32_t));
    if (!pl || !pr) {
        if (c->pp_l_hdr[p]) scratch_free(c->pp_l_hdr[p]);
        if (c->pp_r_hdr[p]) scratch_free(c->pp_r_hdr[p]);
        c->pp_l_hdr[p] = NULL; c->pp_r_hdr[p] = NULL;
        c->part_counts[p] = 0;
        atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
        return;
    }
    uint32_t cap = init_cap;
    uint32_t cnt = 0;

    /* Build open-addressing HT for right partition */
    uint32_t ht_cap = 256;
    uint64_t ht_target = (uint64_t)rp->count * 2;
    while ((uint64_t)ht_cap < ht_target && ht_cap <= (UINT32_MAX >> 1)) ht_cap *= 2;
    if ((uint64_t)ht_cap < ht_target) {
        /* Partition too large for open-addressing HT — signal error */
        atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
        c->part_counts[p] = 0;
        scratch_free(c->pp_l_hdr[p]); scratch_free(c->pp_r_hdr[p]);
        c->pp_l_hdr[p] = NULL; c->pp_r_hdr[p] = NULL;
        return;
    }
    uint32_t ht_mask = ht_cap - 1;

    ray_t* ht_hdr;
    uint32_t* ht = (uint32_t*)scratch_calloc(&ht_hdr, (size_t)ht_cap * 2 * sizeof(uint32_t));
    if (!ht) {
        atomic_store_explicit(&c->had_error, 1, memory_order_relaxed);
        scratch_free(c->pp_l_hdr[p]); scratch_free(c->pp_r_hdr[p]);
        c->pp_l_hdr[p] = NULL; c->pp_r_hdr[p] = NULL;
        c->part_counts[p] = 0;
        return;
    }
    for (uint32_t s = 0; s < ht_cap; s++)
        ht[s * 2 + 1] = RADIX_HT_EMPTY;

    for (uint32_t i = 0; i < rp->count; i++) {
        uint32_t h = rp->entries[i].hash;
        uint32_t slot = h & ht_mask;
        if (i + 4 < rp->count)
            __builtin_prefetch(&ht[(rp->entries[i + 4].hash & ht_mask) * 2], 1, 1);
        while (ht[slot * 2 + 1] != RADIX_HT_EMPTY)
            slot = (slot + 1) & ht_mask;
        ht[slot * 2] = h;
        ht[slot * 2 + 1] = rp->entries[i].row_idx;
    }

    /* Single-pass probe + fill */
    for (uint32_t i = 0; i < lp->count; i++) {
        uint32_t h = lp->entries[i].hash;
        uint32_t lr = lp->entries[i].row_idx;
        uint32_t slot = h & ht_mask;
        if (i + 4 < lp->count)
            __builtin_prefetch(&ht[(lp->entries[i + 4].hash & ht_mask) * 2], 0, 1);
        bool matched = false;
        while (ht[slot * 2 + 1] != RADIX_HT_EMPTY) {
            if (ht[slot * 2] == h) {
                uint32_t rr = ht[slot * 2 + 1];
                if (join_keys_eq(c->l_key_vecs, c->r_key_vecs, c->n_keys,
                                 (int64_t)lr, (int64_t)rr)) {
                    if (!bp_grow_bufs(c, p, &pl, &pr, &cap, cnt))
                        goto done;
                    pl[cnt] = (int32_t)lr;
                    pr[cnt] = (int32_t)rr;
                    cnt++;
                    matched = true;
                    if (c->matched_right)
                        atomic_store_explicit(&c->matched_right[rr], 1, memory_order_relaxed);
                }
            }
            slot = (slot + 1) & ht_mask;
        }
        if (!matched && c->join_type >= 1) {
            if (!bp_grow_bufs(c, p, &pl, &pr, &cap, cnt))
                goto done;
            pl[cnt] = (int32_t)lr;
            pr[cnt] = -1;
            cnt++;
        }
    }

done:
    scratch_free(ht_hdr);
    c->pp_l[p] = pl; c->pp_r[p] = pr;
    c->part_counts[p] = cnt;
    c->pp_cap[p] = cap;
}

/* ── Parallel join HT build ─────────────────────────────────────────────
 * Workers hash right-side rows in parallel and insert into the shared
 * chain-linked hash table using atomic CAS on ht_heads[slot].
 * ht_next[r] is per-row (no contention). Load factor ~0.3 → negligible
 * CAS contention.
 * ──────────────────────────────────────────────────────────────────── */

/* ht_heads is accessed atomically from multiple workers during join build.
 * Using _Atomic(uint32_t)* for C11-compliant atomic access. */
#define JHT_EMPTY UINT32_MAX  /* sentinel for empty HT slot/chain end */

typedef struct {
    _Atomic(uint32_t)* ht_heads;  /* shared, protected by atomic CAS */
    uint32_t* ht_next;            /* per-row, no contention */
    uint32_t ht_mask;       /* ht_cap - 1 */
    ray_t**   r_key_vecs;
    uint8_t  n_keys;
    /* ASP-Join: semijoin filter from factorized left side (NULL if N/A) */
    uint64_t* asp_bits;
    int64_t   asp_key_max;
} join_build_ctx_t;

static void join_build_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    join_build_ctx_t* c = (join_build_ctx_t*)raw;
    _Atomic(uint32_t)* heads = c->ht_heads;
    uint32_t* restrict next  = c->ht_next;
    uint32_t mask  = c->ht_mask;

    /* ASP-Join: precompute pointer for right-side build filtering */
    uint64_t* asp_bits = c->asp_bits;
    int64_t asp_max = c->asp_key_max;
    int64_t* rk0 = (asp_bits && c->n_keys == 1) ? (int64_t*)ray_data(c->r_key_vecs[0]) : NULL;

    for (int64_t r = start; r < end; r++) {
        /* ASP-Join skip: if right key not in left-side bitmap, skip insert */
        if (rk0 && rk0[r] >= 0 && rk0[r] <= asp_max &&
            !RAY_SEL_BIT_TEST(asp_bits, rk0[r])) {
            next[(uint32_t)r] = JHT_EMPTY;  /* mark as unused */
            continue;
        }
        if (r + 8 < end) {
            uint64_t pf_h = hash_row_keys(c->r_key_vecs, c->n_keys, r + 8);
            __builtin_prefetch(&heads[(uint32_t)(pf_h & mask)], 1, 1);
        }
        uint64_t h = hash_row_keys(c->r_key_vecs, c->n_keys, r);
        uint32_t slot = (uint32_t)(h & mask);
        uint32_t row32 = (uint32_t)r;
        uint32_t old = atomic_load_explicit(&heads[slot], memory_order_relaxed);
        do {
            next[row32] = old;
        } while (!atomic_compare_exchange_weak_explicit(&heads[slot], &old, row32,
                    memory_order_release, memory_order_relaxed));
    }
}

#define JOIN_MORSEL 8192

typedef struct {
    _Atomic(uint32_t)* ht_heads;
    uint32_t*    ht_next;
    uint32_t     ht_cap;
    ray_t**       l_key_vecs;
    ray_t**       r_key_vecs;
    uint8_t      n_keys;
    uint8_t      join_type;
    int64_t      left_rows;
    /* Per-morsel counts/offsets (allocated by main thread) */
    int64_t*     morsel_counts;
    int64_t*     morsel_offsets;
    /* Shared output arrays (phase 2 fill) */
    int64_t*     l_idx;
    int64_t*     r_idx;
    /* FULL OUTER: track which right rows were matched (NULL if not full) */
    _Atomic(uint8_t)* matched_right;
    /* S-Join: semijoin filter bitmap (NULL if not applicable) */
    uint64_t*    sjoin_bits;
    int64_t      sjoin_key_max;
} join_probe_ctx_t;

/* Phase 2a: count matches per morsel */
static void join_count_fn(void* raw, uint32_t wid, int64_t task_start, int64_t task_end) {
    (void)wid; (void)task_end;
    join_probe_ctx_t* c = (join_probe_ctx_t*)raw;
    uint32_t tid = (uint32_t)task_start;
    int64_t row_start = (int64_t)tid * JOIN_MORSEL;
    int64_t row_end = row_start + JOIN_MORSEL;
    if (row_end > c->left_rows) row_end = c->left_rows;

    /* S-Join: precompute pointer for fast semijoin check */
    uint64_t* sjbits = c->sjoin_bits;
    int64_t sjmax = c->sjoin_key_max;
    int64_t* lk0 = (sjbits && c->n_keys == 1) ? (int64_t*)ray_data(c->l_key_vecs[0]) : NULL;

    int64_t count = 0;
    uint32_t ht_mask = c->ht_cap - 1;
    for (int64_t l = row_start; l < row_end; l++) {
        /* S-Join skip: if left key not in right-side bitmap, skip probe */
        if (lk0 && lk0[l] >= 0 && lk0[l] <= sjmax &&
            !RAY_SEL_BIT_TEST(sjbits, lk0[l])) {
            if (c->join_type >= 1) count++;  /* LEFT/FULL: emit unmatched */
            continue;
        }

        if (l + 8 < row_end) {
            uint64_t pf_h = hash_row_keys(c->l_key_vecs, c->n_keys, l + 8);
            __builtin_prefetch(&c->ht_heads[(uint32_t)(pf_h & ht_mask)], 0, 1);
        }
        uint64_t h = hash_row_keys(c->l_key_vecs, c->n_keys, l);
        uint32_t slot = (uint32_t)(h & ht_mask);
        bool matched = false;
        for (uint32_t r = c->ht_heads[slot]; r != JHT_EMPTY; r = c->ht_next[r]) {
            if (join_keys_eq(c->l_key_vecs, c->r_key_vecs, c->n_keys, l, (int64_t)r)) {
                count++;
                matched = true;
            }
        }
        if (!matched && c->join_type >= 1) count++;
    }
    c->morsel_counts[tid] = count;
}

/* Phase 2b: fill match pairs using pre-computed offsets */
static void join_fill_fn(void* raw, uint32_t wid, int64_t task_start, int64_t task_end) {
    (void)wid; (void)task_end;
    join_probe_ctx_t* c = (join_probe_ctx_t*)raw;
    uint32_t tid = (uint32_t)task_start;
    int64_t row_start = (int64_t)tid * JOIN_MORSEL;
    int64_t row_end = row_start + JOIN_MORSEL;
    if (row_end > c->left_rows) row_end = c->left_rows;

    int64_t off = c->morsel_offsets[tid];
    int64_t* restrict li = c->l_idx;
    int64_t* restrict ri = c->r_idx;

    /* S-Join: precompute pointer for fast semijoin check */
    uint64_t* sjbits = c->sjoin_bits;
    int64_t sjmax = c->sjoin_key_max;
    int64_t* lk0 = (sjbits && c->n_keys == 1) ? (int64_t*)ray_data(c->l_key_vecs[0]) : NULL;

    uint32_t ht_mask = c->ht_cap - 1;
    for (int64_t l = row_start; l < row_end; l++) {
        /* S-Join skip: if left key not in right-side bitmap, skip probe */
        if (lk0 && lk0[l] >= 0 && lk0[l] <= sjmax &&
            !RAY_SEL_BIT_TEST(sjbits, lk0[l])) {
            if (c->join_type >= 1) {
                li[off] = l;
                ri[off] = -1;
                off++;
            }
            continue;
        }

        if (l + 8 < row_end) {
            uint64_t pf_h = hash_row_keys(c->l_key_vecs, c->n_keys, l + 8);
            __builtin_prefetch(&c->ht_heads[(uint32_t)(pf_h & ht_mask)], 0, 1);
        }
        uint64_t h = hash_row_keys(c->l_key_vecs, c->n_keys, l);
        uint32_t slot = (uint32_t)(h & ht_mask);
        bool matched = false;
        for (uint32_t r = c->ht_heads[slot]; r != JHT_EMPTY; r = c->ht_next[r]) {
            if (join_keys_eq(c->l_key_vecs, c->r_key_vecs, c->n_keys, l, (int64_t)r)) {
                li[off] = l;
                ri[off] = (int64_t)r;
                off++;
                matched = true;
                /* Monotonic 0→1 store from multiple workers. */
                if (c->matched_right) atomic_store_explicit(&c->matched_right[r], 1, memory_order_relaxed);
            }
        }
        if (!matched && c->join_type >= 1) {
            li[off] = l;
            ri[off] = -1;
            off++;
        }
    }
}

ray_t* exec_join(ray_graph_t* g, ray_op_t* op, ray_t* left_table, ray_t* right_table) {
    if (!left_table || RAY_IS_ERR(left_table)) return left_table;
    if (!right_table || RAY_IS_ERR(right_table)) return right_table;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    int64_t left_rows = ray_table_nrows(left_table);
    int64_t right_rows = ray_table_nrows(right_table);
    /* Guard: radix path stores row indices as int32_t (widened to int64_t on gather).
     * Chained HT path uses uint32_t.  Cap at INT32_MAX for correctness. */
    if (right_rows > (int64_t)INT32_MAX || left_rows > (int64_t)INT32_MAX)
        return ray_error("nyi", NULL);
    uint8_t n_keys = ext->join.n_join_keys;
    uint8_t join_type = ext->join.join_type;

    /* VLA bound of zero is UB under -fsanitize=undefined.  Guarantee >=1
     * slot; iterations below are bounded by n_keys so the extra slot is
     * untouched when n_keys == 0. */
    size_t key_slots = n_keys ? n_keys : 1;
    ray_t* l_key_vecs[key_slots];
    ray_t* r_key_vecs[key_slots];
    memset(l_key_vecs, 0, key_slots * sizeof(ray_t*));
    memset(r_key_vecs, 0, key_slots * sizeof(ray_t*));

    for (uint8_t k = 0; k < n_keys; k++) {
        ray_op_ext_t* lk = find_ext(g, ext->join.left_keys[k]->id);
        ray_op_ext_t* rk = find_ext(g, ext->join.right_keys[k]->id);
        if (lk && lk->base.opcode == OP_SCAN)
            l_key_vecs[k] = ray_table_get_col(left_table, lk->sym);
        if (rk && rk->base.opcode == OP_SCAN)
            r_key_vecs[k] = ray_table_get_col(right_table, rk->sym);
        if (rk && rk->base.opcode == OP_CONST && rk->literal)
            r_key_vecs[k] = rk->literal;
    }

    /* RAY_STR keys not yet supported (16-byte elements vs 8-byte hash/eq slots) */
    for (uint8_t k = 0; k < n_keys; k++) {
        if ((l_key_vecs[k] && l_key_vecs[k]->type == RAY_STR) ||
            (r_key_vecs[k] && r_key_vecs[k]->type == RAY_STR))
            return ray_error("nyi", NULL);
    }

    ray_pool_t* pool = ray_pool_get();

    /* Shared output state — used by both radix and chained HT paths */
    ray_t* result = NULL;
    ray_t* counts_hdr = NULL;
    ray_t* l_idx_hdr = NULL;
    ray_t* r_idx_hdr = NULL;
    ray_t* matched_right_hdr = NULL;
    ray_t* sjoin_sel = NULL;
    ray_t* asp_sel = NULL;
    ray_t* ht_next_hdr = NULL;
    ray_t* ht_heads_hdr = NULL;
    int64_t* l_idx = NULL;
    int64_t* r_idx = NULL;
    int64_t pair_count = 0;
    _Atomic(uint8_t)* matched_right = NULL;

    /* ── Radix-partitioned path (large joins) ──────────────────────── */
    if (right_rows > RAY_PARALLEL_THRESHOLD) {
        uint8_t radix_bits = radix_join_bits(right_rows);
        uint32_t n_rparts = (uint32_t)1 << radix_bits;

        /* Pre-compute hashes for both sides (once, reused by histogram+scatter) */
        ray_t* r_hash_hdr = NULL;
        uint32_t* r_hashes = (uint32_t*)scratch_alloc(&r_hash_hdr,
                                (size_t)right_rows * sizeof(uint32_t));
        ray_t* l_hash_hdr = NULL;
        uint32_t* l_hashes = (uint32_t*)scratch_alloc(&l_hash_hdr,
                                (size_t)left_rows * sizeof(uint32_t));
        if (!r_hashes || !l_hashes) {
            if (r_hash_hdr) scratch_free(r_hash_hdr);
            if (l_hash_hdr) scratch_free(l_hash_hdr);
            goto chained_ht_fallback;
        }
        join_radix_hash_ctx_t rhctx = { .key_vecs = r_key_vecs, .n_keys = n_keys, .hashes = r_hashes };
        join_radix_hash_ctx_t lhctx = { .key_vecs = l_key_vecs, .n_keys = n_keys, .hashes = l_hashes };
        if (pool) {
            ray_pool_dispatch(pool, join_radix_hash_fn, &rhctx, right_rows);
            ray_pool_dispatch(pool, join_radix_hash_fn, &lhctx, left_rows);
        } else {
            join_radix_hash_fn(&rhctx, 0, 0, right_rows);
            join_radix_hash_fn(&lhctx, 0, 0, left_rows);
        }

        if (pool_cancelled(pool)) {
            scratch_free(r_hash_hdr); scratch_free(l_hash_hdr);
            return ray_error("cancel", NULL);
        }

        /* Partition both sides using cached hashes */
        ray_t* r_parts_hdr = NULL;
        join_radix_part_t* r_parts = join_radix_partition(pool, right_rows,
                                                          radix_bits, r_hashes, &r_parts_hdr);
        ray_t* l_parts_hdr = NULL;
        join_radix_part_t* l_parts = join_radix_partition(pool, left_rows,
                                                          radix_bits, l_hashes, &l_parts_hdr);
        scratch_free(r_hash_hdr);
        scratch_free(l_hash_hdr);
        if (!r_parts || !l_parts) {
            /* OOM during partitioning — fall through to chained HT path */
            if (r_parts) {
                for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++)
                    if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
                scratch_free(r_parts_hdr);
            }
            if (l_parts) {
                for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++)
                    if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
                scratch_free(l_parts_hdr);
            }
            goto chained_ht_fallback;
        }

        if (pool_cancelled(pool)) {
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
                if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
            }
            scratch_free(r_parts_hdr); scratch_free(l_parts_hdr);
            return ray_error("cancel", NULL);
        }

        /* FULL OUTER: allocate matched_right tracker */
        if (join_type == 2 && right_rows > 0) {
            matched_right = (_Atomic(uint8_t)*)scratch_calloc(&matched_right_hdr,
                                                               (size_t)right_rows);
            if (!matched_right) {
                for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                    if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
                    if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
                }
                scratch_free(r_parts_hdr); scratch_free(l_parts_hdr);
                matched_right_hdr = NULL;
                goto chained_ht_fallback;
            }
        }

        /* Single-pass per-partition build+probe with local output buffers */
        ray_t* pcounts_hdr = NULL;
        int64_t* part_counts = (int64_t*)scratch_calloc(&pcounts_hdr,
                                  (size_t)n_rparts * sizeof(int64_t));
        ray_t* pp_meta_hdr = NULL;
        /* Allocate per-partition pointer arrays */
        size_t pp_alloc_sz = (size_t)n_rparts * (2 * sizeof(int32_t*) + 2 * sizeof(ray_t*) + sizeof(uint32_t));
        char* pp_mem = (char*)scratch_calloc(&pp_meta_hdr, pp_alloc_sz);
        if (!part_counts || !pp_mem) {
            if (pcounts_hdr) scratch_free(pcounts_hdr);
            if (pp_meta_hdr) scratch_free(pp_meta_hdr);
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
                if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
            }
            scratch_free(r_parts_hdr); scratch_free(l_parts_hdr);
            if (matched_right_hdr) { scratch_free(matched_right_hdr); matched_right_hdr = NULL; }
            matched_right = NULL;
            goto chained_ht_fallback;
        }
        int32_t** pp_l = (int32_t**)pp_mem;
        int32_t** pp_r = (int32_t**)(pp_mem + (size_t)n_rparts * sizeof(int32_t*));
        ray_t** pp_l_hdr = (ray_t**)(pp_mem + (size_t)n_rparts * 2 * sizeof(int32_t*));
        ray_t** pp_r_hdr = (ray_t**)(pp_mem + (size_t)n_rparts * (2 * sizeof(int32_t*) + sizeof(ray_t*)));
        uint32_t* pp_cap = (uint32_t*)(pp_mem + (size_t)n_rparts * (2 * sizeof(int32_t*) + 2 * sizeof(ray_t*)));

        join_radix_bp_ctx_t bp_ctx = {
            .l_parts = l_parts, .r_parts = r_parts,
            .l_key_vecs = l_key_vecs, .r_key_vecs = r_key_vecs,
            .n_keys = n_keys, .join_type = join_type,
            .pp_l = pp_l, .pp_r = pp_r,
            .pp_l_hdr = pp_l_hdr, .pp_r_hdr = pp_r_hdr,
            .part_counts = part_counts, .pp_cap = pp_cap,
            .matched_right = matched_right,
            .had_error = 0,
        };
        if (pool && n_rparts > 1)
            ray_pool_dispatch_n(pool, join_radix_build_probe_fn, &bp_ctx, n_rparts);
        else
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++)
                join_radix_build_probe_fn(&bp_ctx, 0, rp2, rp2 + 1);

        /* Check cancellation and errors during build+probe */
        bool bp_cancelled = pool_cancelled(pool);
        bool bp_error = atomic_load_explicit(&bp_ctx.had_error, memory_order_relaxed);
        if (bp_cancelled || bp_error) {
            /* Free all per-partition buffers */
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
                if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
                if (pp_l_hdr[rp2]) scratch_free(pp_l_hdr[rp2]);
                if (pp_r_hdr[rp2]) scratch_free(pp_r_hdr[rp2]);
            }
            scratch_free(r_parts_hdr); scratch_free(l_parts_hdr);
            scratch_free(pp_meta_hdr); scratch_free(pcounts_hdr);
            if (matched_right_hdr) { scratch_free(matched_right_hdr); matched_right_hdr = NULL; }
            matched_right = NULL;
            if (bp_cancelled) return ray_error("cancel", NULL);
            goto chained_ht_fallback;
        }

        /* Free partition buffers — no longer needed */
        for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
            if (r_parts[rp2].entries_hdr) scratch_free(r_parts[rp2].entries_hdr);
            if (l_parts[rp2].entries_hdr) scratch_free(l_parts[rp2].entries_hdr);
        }
        scratch_free(r_parts_hdr);
        scratch_free(l_parts_hdr);

        /* Compute total output size and consolidate per-partition buffers */
        for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++)
            pair_count += part_counts[rp2];

        /* FULL OUTER: count unmatched right rows */
        int64_t unmatched_right = 0;
        if (join_type == 2 && matched_right) {
            for (int64_t r = 0; r < right_rows; r++)
                if (!matched_right[r]) unmatched_right++;
        }
        int64_t total_out = pair_count + unmatched_right;

        if (total_out > 0) {
            l_idx = (int64_t*)scratch_alloc(&l_idx_hdr, (size_t)total_out * sizeof(int64_t));
            r_idx = (int64_t*)scratch_alloc(&r_idx_hdr, (size_t)total_out * sizeof(int64_t));
            if (!l_idx || !r_idx) {
                scratch_free(l_idx_hdr); scratch_free(r_idx_hdr);
                l_idx_hdr = NULL; r_idx_hdr = NULL;
                for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                    if (pp_l_hdr[rp2]) scratch_free(pp_l_hdr[rp2]);
                    if (pp_r_hdr[rp2]) scratch_free(pp_r_hdr[rp2]);
                }
                scratch_free(pp_meta_hdr);
                scratch_free(pcounts_hdr);
                if (matched_right_hdr) scratch_free(matched_right_hdr);
                matched_right_hdr = NULL;
                return ray_error("oom", NULL);
            }

            /* Copy per-partition results into contiguous arrays (int32→int64 widen) */
            int64_t off = 0;
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                int64_t cnt = part_counts[rp2];
                if (cnt > 0 && pp_l[rp2] && pp_r[rp2]) {
                    for (int64_t j = 0; j < cnt; j++) {
                        l_idx[off + j] = (int64_t)pp_l[rp2][j];
                        r_idx[off + j] = (int64_t)pp_r[rp2][j];
                    }
                    off += cnt;
                }
            }

            /* FULL OUTER: append unmatched right rows */
            if (unmatched_right > 0) {
                for (int64_t r = 0; r < right_rows; r++) {
                    if (!matched_right[r]) {
                        l_idx[off] = -1;
                        r_idx[off] = r;
                        off++;
                    }
                }
            }
            pair_count = total_out;
        }

        /* Free per-partition buffers allocated by worker threads.
         * Safe: ray_pool_dispatch_n has completed (workers are back on semaphore),
         * ray_parallel_flag is 0, and ray_free handles cross-heap deallocation
         * via the foreign-block list flushed by ray_heap_gc at ray_parallel_end. */
        for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
            if (pp_l_hdr[rp2]) scratch_free(pp_l_hdr[rp2]);
            if (pp_r_hdr[rp2]) scratch_free(pp_r_hdr[rp2]);
        }
        scratch_free(pp_meta_hdr);
        scratch_free(pcounts_hdr);
        goto join_gather;
    }

chained_ht_fallback:;
    /* ── Chained HT path (small joins / radix OOM fallback) ────────── */
    uint64_t ht_cap64 = 256;
    uint64_t target = (uint64_t)right_rows * 2;
    while (ht_cap64 < target) ht_cap64 *= 2;
    if (ht_cap64 > UINT32_MAX) ht_cap64 = (uint64_t)1 << 31;
    uint32_t ht_cap = (uint32_t)ht_cap64;

    uint32_t* ht_next = (uint32_t*)scratch_alloc(&ht_next_hdr, (size_t)right_rows * sizeof(uint32_t));
    // cppcheck-suppress internalAstError
    // Valid C11/C17 _Atomic(T)* declaration; cppcheck parser may mis-handle this syntax.
    _Atomic(uint32_t)* ht_heads = (_Atomic(uint32_t)*)scratch_alloc(&ht_heads_hdr, ht_cap * sizeof(uint32_t));
    if (!ht_next || !ht_heads) {
        scratch_free(ht_next_hdr); scratch_free(ht_heads_hdr);
        return ray_error("oom", NULL);
    }
    memset(ht_heads, 0xFF, ht_cap * sizeof(uint32_t));  /* JHT_EMPTY = 0xFFFFFFFF */

    /* Phase 0.5: ASP-Join — extract semijoin filter from factorized left side.
     * When the left input comes from a factorized expand (_count column present),
     * build a RAY_SEL bitmap of left-side key values to skip right-side rows
     * during hash-build whose keys can't match any left-side row. */
    uint64_t* asp_bits = NULL;
    int64_t asp_key_max = 0;
    if (n_keys == 1 && join_type == 0 && l_key_vecs[0] &&
        l_key_vecs[0]->type == RAY_I64 && right_rows > left_rows * 2) {
        int64_t cnt_sym = ray_sym_intern("_count", 6);
        ray_t* cnt_col = ray_table_get_col(left_table, cnt_sym);
        if (cnt_col) {  /* left is factorized */
            int64_t* lk = (int64_t*)ray_data(l_key_vecs[0]);
            int64_t lk_max = 0;
            for (int64_t i = 0; i < left_rows; i++)
                if (lk[i] > lk_max) lk_max = lk[i];

            if (lk_max < (int64_t)1 << 24) {
                asp_sel = ray_sel_new(lk_max + 1);
                if (asp_sel && !RAY_IS_ERR(asp_sel)) {
                    asp_bits = ray_sel_bits(asp_sel);
                    asp_key_max = lk_max;
                    for (int64_t i = 0; i < left_rows; i++) {
                        int64_t k = lk[i];
                        if (k >= 0 && k <= lk_max)
                            RAY_SEL_BIT_SET(asp_bits, k);
                    }
                }
            }
        }
    }

    {
        join_build_ctx_t bctx = {
            .ht_heads   = ht_heads,
            .ht_next    = ht_next,
            .ht_mask    = ht_cap - 1,
            .r_key_vecs = r_key_vecs,
            .n_keys     = n_keys,
            .asp_bits   = asp_bits,
            .asp_key_max = asp_key_max,
        };
        if (pool && right_rows > RAY_PARALLEL_THRESHOLD)
            ray_pool_dispatch(pool, join_build_fn, &bctx, right_rows);
        else
            join_build_fn(&bctx, 0, 0, right_rows);
    }
    CHECK_CANCEL_GOTO(pool, join_cleanup);

    /* Phase 1.5: S-Join semijoin filter extraction.
     * Build a RAY_SEL bitmap of all distinct right-side key values that
     * appear in the hash table. This can be used to skip left-side rows
     * whose key cannot match any right-side row.
     *
     * Applied when: single I64 key, inner join, left side is large enough
     * to benefit from filtering (> 2x right side). */
    if (n_keys == 1 && join_type == 0 && l_key_vecs[0] && r_key_vecs[0] &&
        l_key_vecs[0]->type == RAY_I64 && r_key_vecs[0]->type == RAY_I64 &&
        left_rows > right_rows * 2) {
        /* Determine key range to size the bitmap */
        int64_t* rk = (int64_t*)ray_data(r_key_vecs[0]);
        int64_t key_max = 0;
        for (int64_t i = 0; i < right_rows; i++)
            if (rk[i] > key_max) key_max = rk[i];

        if (key_max < (int64_t)1 << 24) {  /* only for reasonably bounded keys */
            sjoin_sel = ray_sel_new(key_max + 1);
            if (sjoin_sel && !RAY_IS_ERR(sjoin_sel)) {
                uint64_t* bits = ray_sel_bits(sjoin_sel);
                for (int64_t i = 0; i < right_rows; i++) {
                    int64_t k = rk[i];
                    if (k >= 0 && k <= key_max)
                        RAY_SEL_BIT_SET(bits, k);
                }
            }
        }
    }

    /* Phase 2: Parallel probe (two-pass: count → prefix-sum → fill) */
    uint32_t n_tasks = (uint32_t)((left_rows + JOIN_MORSEL - 1) / JOIN_MORSEL);
    if (n_tasks == 0) n_tasks = 1;

    int64_t* morsel_counts = (int64_t*)scratch_calloc(&counts_hdr,
                              (size_t)(n_tasks + 1) * sizeof(int64_t));
    if (!morsel_counts) {
        scratch_free(ht_next_hdr); scratch_free(ht_heads_hdr);
        return ray_error("oom", NULL);
    }

    /* For FULL OUTER JOIN, allocate matched_right tracker */
    if (join_type == 2 && right_rows > 0) {
        matched_right = (_Atomic(uint8_t)*)scratch_calloc(&matched_right_hdr,
                                                           (size_t)right_rows);
        if (!matched_right) goto join_cleanup;
    }

    /* Prepare S-Join fields for probe context */
    uint64_t* sjoin_bits = NULL;
    int64_t sjoin_key_max = 0;
    if (sjoin_sel && !RAY_IS_ERR(sjoin_sel)) {
        sjoin_bits = ray_sel_bits(sjoin_sel);
        sjoin_key_max = sjoin_sel->len - 1;
    }

    join_probe_ctx_t probe_ctx = {
        .ht_heads    = ht_heads,
        .ht_next     = ht_next,
        .ht_cap      = ht_cap,
        .l_key_vecs  = l_key_vecs,
        .r_key_vecs  = r_key_vecs,
        .n_keys      = n_keys,
        .join_type   = join_type,
        .left_rows   = left_rows,
        .morsel_counts = morsel_counts,
        .matched_right = matched_right,
        .sjoin_bits  = sjoin_bits,
        .sjoin_key_max = sjoin_key_max,
    };

    /* 2a: Count matches per morsel */
    if (pool && n_tasks > 1)
        ray_pool_dispatch_n(pool, join_count_fn, &probe_ctx, n_tasks);
    else
        for (uint32_t t = 0; t < n_tasks; t++)
            join_count_fn(&probe_ctx, 0, t, t + 1);

    /* Prefix sum → morsel_offsets (reuse counts array as offsets) */
    pair_count = 0;
    for (uint32_t t = 0; t < n_tasks; t++) {
        int64_t cnt = morsel_counts[t];
        morsel_counts[t] = pair_count;
        pair_count += cnt;
    }

    /* Allocate output pair arrays */
    if (pair_count > 0) {
        l_idx = (int64_t*)scratch_alloc(&l_idx_hdr, (size_t)pair_count * sizeof(int64_t));
        r_idx = (int64_t*)scratch_alloc(&r_idx_hdr, (size_t)pair_count * sizeof(int64_t));
        if (!l_idx || !r_idx) goto join_cleanup;
    }

    /* 2b: Fill match pairs */
    probe_ctx.morsel_offsets = morsel_counts;  /* now holds prefix sums */
    probe_ctx.l_idx = l_idx;
    probe_ctx.r_idx = r_idx;

    if (pair_count > 0) {
        if (pool && n_tasks > 1)
            ray_pool_dispatch_n(pool, join_fill_fn, &probe_ctx, n_tasks);
        else
            for (uint32_t t = 0; t < n_tasks; t++)
                join_fill_fn(&probe_ctx, 0, t, t + 1);
    }

    CHECK_CANCEL_GOTO(pool, join_cleanup);

    /* FULL OUTER: append unmatched right rows (l_idx=-1, r_idx=r) */
    if (join_type == 2 && matched_right) {
        int64_t unmatched_right = 0;
        for (int64_t r = 0; r < right_rows; r++)
            if (!matched_right[r]) unmatched_right++;

        if (unmatched_right > 0) {
            int64_t total = pair_count + unmatched_right;
            ray_t* new_l_hdr;
            ray_t* new_r_hdr;
            int64_t* new_l = (int64_t*)scratch_alloc(&new_l_hdr,
                                (size_t)total * sizeof(int64_t));
            int64_t* new_r = (int64_t*)scratch_alloc(&new_r_hdr,
                                (size_t)total * sizeof(int64_t));
            if (!new_l || !new_r) {
                scratch_free(new_l_hdr); scratch_free(new_r_hdr);
                goto join_cleanup;
            }
            if (pair_count > 0) {
                memcpy(new_l, l_idx, (size_t)pair_count * sizeof(int64_t));
                memcpy(new_r, r_idx, (size_t)pair_count * sizeof(int64_t));
            }
            scratch_free(l_idx_hdr);
            scratch_free(r_idx_hdr);
            int64_t off = pair_count;
            for (int64_t r = 0; r < right_rows; r++) {
                if (!matched_right[r]) {
                    new_l[off] = -1;
                    new_r[off] = r;
                    off++;
                }
            }
            l_idx = new_l;  r_idx = new_r;
            l_idx_hdr = new_l_hdr;  r_idx_hdr = new_r_hdr;
            pair_count = total;
        }
    }

join_gather:;
    /* Phase 3: Build result table with parallel column gather.
     * Use multi_gather for batched column access when possible (non-nullable
     * indices), falling back to per-column gather for nullable RIGHT columns. */
    int64_t left_ncols = ray_table_ncols(left_table);
    int64_t right_ncols = ray_table_ncols(right_table);
    result = ray_table_new(left_ncols + right_ncols);
    if (!result || RAY_IS_ERR(result)) goto join_cleanup;

    /* Allocate all output columns upfront for batched gather */
    ray_t* l_out_cols[MGATHER_MAX_COLS];
    int64_t l_out_names[MGATHER_MAX_COLS];
    int64_t l_out_count = 0;
    for (int64_t c = 0; c < left_ncols && l_out_count < MGATHER_MAX_COLS; c++) {
        ray_t* col = ray_table_get_col_idx(left_table, c);
        if (!col) continue;
        ray_t* new_col = col_vec_new(col, pair_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = pair_count;
        l_out_cols[l_out_count] = new_col;
        l_out_names[l_out_count] = ray_table_col_name(left_table, c);
        l_out_count++;
    }

    ray_t* r_out_cols[MGATHER_MAX_COLS];
    ray_t* r_src_cols[MGATHER_MAX_COLS];
    int64_t r_out_names[MGATHER_MAX_COLS];
    int64_t r_out_count = 0;
    for (int64_t c = 0; c < right_ncols; c++) {
        ray_t* col = ray_table_get_col_idx(right_table, c);
        int64_t name_id = ray_table_col_name(right_table, c);
        if (!col) continue;
        bool is_key = false;
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_op_ext_t* rk = find_ext(g, ext->join.right_keys[k]->id);
            if (rk && rk->base.opcode == OP_SCAN && rk->sym == name_id) {
                is_key = true; break;
            }
        }
        if (is_key) continue;
        if (r_out_count >= MGATHER_MAX_COLS) continue;
        ray_t* new_col = col_vec_new(col, pair_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = pair_count;
        r_out_cols[r_out_count] = new_col;
        r_src_cols[r_out_count] = col;
        r_out_names[r_out_count] = name_id;
        r_out_count++;
    }

    if (pair_count > 0) {
        /* Left columns: multi_gather (non-nullable for INNER/LEFT) */
        bool l_nullable = (join_type == 2);  /* only FULL OUTER */
        if (!l_nullable && l_out_count > 1 && l_out_count <= MGATHER_MAX_COLS) {
            multi_gather_ctx_t mgctx = { .idx = l_idx, .ncols = l_out_count };
            int64_t si = 0;
            for (int64_t c = 0; c < left_ncols && si < l_out_count; c++) {
                ray_t* col = ray_table_get_col_idx(left_table, c);
                if (!col) continue;
                mgctx.srcs[si] = (char*)ray_data(col);
                mgctx.dsts[si] = (char*)ray_data(l_out_cols[si]);
                mgctx.esz[si] = col_esz(col);
                si++;
            }
            if (pool && pair_count > RAY_PARALLEL_THRESHOLD)
                ray_pool_dispatch(pool, multi_gather_fn, &mgctx, pair_count);
            else
                multi_gather_fn(&mgctx, 0, 0, pair_count);
        } else {
            /* Fall back to per-column gather for nullable or single column */
            int64_t si = 0;
            for (int64_t c = 0; c < left_ncols && si < l_out_count; c++) {
                ray_t* col = ray_table_get_col_idx(left_table, c);
                if (!col) continue;
                gather_ctx_t gctx = {
                    .idx = l_idx, .src_col = col, .dst_col = l_out_cols[si],
                    .esz = col_esz(col), .nullable = l_nullable,
                };
                if (pool && pair_count > RAY_PARALLEL_THRESHOLD)
                    ray_pool_dispatch(pool, gather_fn, &gctx, pair_count);
                else
                    gather_fn(&gctx, 0, 0, pair_count);
                si++;
            }
        }

        /* Right columns: per-column gather (nullable for LEFT/FULL OUTER) */
        bool r_nullable = (join_type >= 1);
        if (!r_nullable && r_out_count > 1 && r_out_count <= MGATHER_MAX_COLS) {
            multi_gather_ctx_t mgctx = { .idx = r_idx, .ncols = r_out_count };
            for (int64_t i = 0; i < r_out_count; i++) {
                mgctx.srcs[i] = (char*)ray_data(r_src_cols[i]);
                mgctx.dsts[i] = (char*)ray_data(r_out_cols[i]);
                mgctx.esz[i] = col_esz(r_out_cols[i]);
            }
            if (pool && pair_count > RAY_PARALLEL_THRESHOLD)
                ray_pool_dispatch(pool, multi_gather_fn, &mgctx, pair_count);
            else
                multi_gather_fn(&mgctx, 0, 0, pair_count);
        } else {
            for (int64_t i = 0; i < r_out_count; i++) {
                gather_ctx_t gctx = {
                    .idx = r_idx, .src_col = r_src_cols[i], .dst_col = r_out_cols[i],
                    .esz = col_esz(r_src_cols[i]), .nullable = r_nullable,
                };
                if (pool && pair_count > RAY_PARALLEL_THRESHOLD)
                    ray_pool_dispatch(pool, gather_fn, &gctx, pair_count);
                else
                    gather_fn(&gctx, 0, 0, pair_count);
            }
        }
    }

    /* Propagate RAY_STR string pools and null bitmaps from source columns */
    {
        int64_t si = 0;
        for (int64_t c = 0; c < left_ncols && si < l_out_count; c++) {
            ray_t* col = ray_table_get_col_idx(left_table, c);
            if (!col) continue;
            col_propagate_str_pool(l_out_cols[si], col);
            col_propagate_nulls_gather(l_out_cols[si], col, l_idx, pair_count);
            si++;
        }
    }
    for (int64_t i = 0; i < r_out_count; i++) {
        col_propagate_str_pool(r_out_cols[i], r_src_cols[i]);
        col_propagate_nulls_gather(r_out_cols[i], r_src_cols[i], r_idx, pair_count);
    }

    /* Add columns to result */
    for (int64_t i = 0; i < l_out_count; i++) {
        result = ray_table_add_col(result, l_out_names[i], l_out_cols[i]);
        ray_release(l_out_cols[i]);
    }
    for (int64_t i = 0; i < r_out_count; i++) {
        result = ray_table_add_col(result, r_out_names[i], r_out_cols[i]);
        ray_release(r_out_cols[i]);
    }

join_cleanup:
    if (ht_next_hdr) scratch_free(ht_next_hdr);
    if (ht_heads_hdr) scratch_free(ht_heads_hdr);
    scratch_free(l_idx_hdr);
    scratch_free(r_idx_hdr);
    if (counts_hdr) scratch_free(counts_hdr);
    scratch_free(matched_right_hdr);
    if (sjoin_sel) ray_release(sjoin_sel);
    if (asp_sel) ray_release(asp_sel);

    return result;
}

/* ============================================================================
 * OP_ANTIJOIN: anti-semi-join — keep left rows with NO matching right row
 * Build hash set from right keys, probe left, emit non-matching left rows.
 * ============================================================================ */

ray_t* exec_antijoin(ray_graph_t* g, ray_op_t* op,
                            ray_t* left_table, ray_t* right_table) {
    if (!left_table || RAY_IS_ERR(left_table)) return left_table;
    if (!right_table || RAY_IS_ERR(right_table)) return right_table;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    int64_t left_rows  = ray_table_nrows(left_table);
    int64_t right_rows = ray_table_nrows(right_table);

    if (right_rows > (int64_t)INT32_MAX || left_rows > (int64_t)INT32_MAX)
        return ray_error("nyi", NULL);

    uint8_t n_keys = ext->join.n_join_keys;

    /* Trivial case: empty right → all left rows pass */
    if (right_rows == 0) {
        ray_retain(left_table);
        return left_table;
    }
    /* Trivial case: empty left → empty result */
    if (left_rows == 0) {
        ray_retain(left_table);
        return left_table;
    }

    ray_t* l_key_vecs[16];
    ray_t* r_key_vecs[16];
    memset(l_key_vecs, 0, n_keys * sizeof(ray_t*));
    memset(r_key_vecs, 0, n_keys * sizeof(ray_t*));

    for (uint8_t k = 0; k < n_keys; k++) {
        ray_op_ext_t* lk = find_ext(g, ext->join.left_keys[k]->id);
        ray_op_ext_t* rk = find_ext(g, ext->join.right_keys[k]->id);
        if (lk && lk->base.opcode == OP_SCAN)
            l_key_vecs[k] = ray_table_get_col(left_table, lk->sym);
        if (rk && rk->base.opcode == OP_SCAN)
            r_key_vecs[k] = ray_table_get_col(right_table, rk->sym);
        if (rk && rk->base.opcode == OP_CONST && rk->literal)
            r_key_vecs[k] = rk->literal;
    }

    /* RAY_STR keys not yet supported */
    for (uint8_t k = 0; k < n_keys; k++) {
        if ((l_key_vecs[k] && l_key_vecs[k]->type == RAY_STR) ||
            (r_key_vecs[k] && r_key_vecs[k]->type == RAY_STR))
            return ray_error("nyi", NULL);
    }

    /* Build chained hash table from right side */
    ray_t* ht_next_hdr = NULL;
    ray_t* ht_heads_hdr = NULL;

    uint64_t ht_cap64 = 256;
    uint64_t target = (uint64_t)right_rows * 2;
    while (ht_cap64 < target) ht_cap64 *= 2;
    if (ht_cap64 > UINT32_MAX) ht_cap64 = (uint64_t)1 << 31;
    uint32_t ht_cap = (uint32_t)ht_cap64;

    uint32_t* ht_next = (uint32_t*)scratch_alloc(&ht_next_hdr,
                            (size_t)right_rows * sizeof(uint32_t));
    _Atomic(uint32_t)* ht_heads = (_Atomic(uint32_t)*)scratch_alloc(&ht_heads_hdr,
                            ht_cap * sizeof(uint32_t));
    if (!ht_next || !ht_heads) {
        if (ht_next_hdr) scratch_free(ht_next_hdr);
        if (ht_heads_hdr) scratch_free(ht_heads_hdr);
        return ray_error("oom", NULL);
    }
    memset(ht_heads, 0xFF, ht_cap * sizeof(uint32_t));  /* JHT_EMPTY */

    /* Build: insert right rows into HT */
    ray_pool_t* pool = ray_pool_get();
    {
        join_build_ctx_t bctx = {
            .ht_heads   = ht_heads,
            .ht_next    = ht_next,
            .ht_mask    = ht_cap - 1,
            .r_key_vecs = r_key_vecs,
            .n_keys     = n_keys,
            .asp_bits   = NULL,
            .asp_key_max = 0,
        };
        if (pool && right_rows > RAY_PARALLEL_THRESHOLD)
            ray_pool_dispatch(pool, join_build_fn, &bctx, right_rows);
        else
            join_build_fn(&bctx, 0, 0, right_rows);
    }

    if (pool_cancelled(pool)) {
        scratch_free(ht_next_hdr);
        scratch_free(ht_heads_hdr);
        return ray_error("cancel", NULL);
    }

    /* Probe: scan left rows, collect indices of those with NO match */
    ray_t* out_idx_hdr = NULL;
    int64_t* out_idx = (int64_t*)scratch_alloc(&out_idx_hdr,
                            (size_t)left_rows * sizeof(int64_t));
    if (!out_idx) {
        scratch_free(ht_next_hdr);
        scratch_free(ht_heads_hdr);
        return ray_error("oom", NULL);
    }

    uint32_t ht_mask = ht_cap - 1;
    int64_t out_count = 0;
    for (int64_t l = 0; l < left_rows; l++) {
        uint64_t h = hash_row_keys(l_key_vecs, n_keys, l);
        uint32_t slot = (uint32_t)(h & ht_mask);
        bool matched = false;
        for (uint32_t r = ht_heads[slot]; r != JHT_EMPTY; r = ht_next[r]) {
            if (join_keys_eq(l_key_vecs, r_key_vecs, n_keys, l, (int64_t)r)) {
                matched = true;
                break;  /* anti-join: one match is enough to exclude */
            }
        }
        if (!matched) {
            out_idx[out_count++] = l;
        }
    }

    scratch_free(ht_next_hdr);
    scratch_free(ht_heads_hdr);

    /* Gather: build result table with only left columns */
    int64_t left_ncols = ray_table_ncols(left_table);
    ray_t* result = ray_table_new(left_ncols);
    if (!result || RAY_IS_ERR(result)) {
        scratch_free(out_idx_hdr);
        return result;
    }

    if (out_count > 0) {
        for (int64_t c = 0; c < left_ncols; c++) {
            ray_t* col = ray_table_get_col_idx(left_table, c);
            if (!col) continue;
            ray_t* new_col = col_vec_new(col, out_count);
            if (!new_col || RAY_IS_ERR(new_col)) continue;
            new_col->len = out_count;

            gather_ctx_t gctx = {
                .idx = out_idx, .src_col = col, .dst_col = new_col,
                .esz = col_esz(col), .nullable = false,
            };
            if (pool && out_count > RAY_PARALLEL_THRESHOLD)
                ray_pool_dispatch(pool, gather_fn, &gctx, out_count);
            else
                gather_fn(&gctx, 0, 0, out_count);

            col_propagate_str_pool(new_col, col);

            int64_t name_id = ray_table_col_name(left_table, c);
            result = ray_table_add_col(result, name_id, new_col);
            ray_release(new_col);
        }
    }

    scratch_free(out_idx_hdr);
    return result;
}

/* ============================================================================
 * OP_WINDOW_JOIN: ASOF join (sort-merge)
 * For each left row, find the most recent right row where right.time <= left.time,
 * optionally partitioned by equality keys. O(N+M) after sorting.
 * ============================================================================ */

ray_t* exec_window_join(ray_graph_t* g, ray_op_t* op,
                               ray_t* left_table, ray_t* right_table) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    uint8_t n_eq      = ext->asof.n_eq_keys;
    uint8_t join_type = ext->asof.join_type;

    int64_t left_n  = ray_table_nrows(left_table);
    int64_t right_n = ray_table_nrows(right_table);

    /* Resolve time key */
    ray_op_ext_t* time_ext = find_ext(g, ext->asof.time_key->id);
    if (!time_ext || time_ext->base.opcode != OP_SCAN)
        return ray_error("nyi", NULL);
    int64_t time_sym = time_ext->sym;

    /* Resolve equality keys */
    int64_t eq_syms[256];
    for (uint8_t k = 0; k < n_eq; k++) {
        ray_op_ext_t* ek = find_ext(g, ext->asof.eq_keys[k]->id);
        if (!ek || ek->base.opcode != OP_SCAN)
            return ray_error("nyi", NULL);
        eq_syms[k] = ek->sym;
    }

    /* Get time vectors — use int64 representation for comparison.
     * TIME uses 4-byte i32 (ms), TIMESTAMP uses 8-byte i64 (ns).
     * We expand to a temporary i64 array for uniform comparison. */
    ray_t* lt_time_vec = ray_table_get_col(left_table, time_sym);
    ray_t* rt_time_vec = ray_table_get_col(right_table, time_sym);
    if (!lt_time_vec || !rt_time_vec) return ray_error("schema", NULL);
    int8_t time_type = lt_time_vec->type;

    /* Helper macro to read time value as int64_t regardless of storage type */
    #define READ_TIME(vec, idx) \
        ((time_type == RAY_TIME || time_type == RAY_DATE) \
            ? (int64_t)((int32_t*)ray_data(vec))[(idx)] \
            : ((int64_t*)ray_data(vec))[(idx)])

    /* Build i64 time arrays for efficient comparison */
    ray_t* lt_time_hdr = NULL, *rt_time_hdr = NULL;
    int64_t* lt_time = (int64_t*)scratch_alloc(&lt_time_hdr, (size_t)left_n * sizeof(int64_t));
    int64_t* rt_time = (int64_t*)scratch_alloc(&rt_time_hdr, (size_t)right_n * sizeof(int64_t));
    if ((!lt_time && left_n > 0) || (!rt_time && right_n > 0)) {
        if (lt_time_hdr) scratch_free(lt_time_hdr);
        if (rt_time_hdr) scratch_free(rt_time_hdr);
        return ray_error("oom", NULL);
    }
    for (int64_t i = 0; i < left_n; i++) lt_time[i] = READ_TIME(lt_time_vec, i);
    for (int64_t i = 0; i < right_n; i++) rt_time[i] = READ_TIME(rt_time_vec, i);
    #undef READ_TIME

    /* Get eq key vectors — stored as ray_t* for type-safe access */
    ray_t* lt_eq[256], *rt_eq[256];
    for (uint8_t k = 0; k < n_eq; k++) {
        ray_t* lv = ray_table_get_col(left_table, eq_syms[k]);
        ray_t* rv = ray_table_get_col(right_table, eq_syms[k]);
        if (!lv || !rv) {
            if (lt_time_hdr) scratch_free(lt_time_hdr);
            if (rt_time_hdr) scratch_free(rt_time_hdr);
            return ray_error("schema", NULL);
        }
        lt_eq[k] = lv;
        rt_eq[k] = rv;
    }

    /* Precompute per-row "any key is null" bitsets.  Null-keyed rows must
     * not match — left rows fall through to the left-outer null fill,
     * right rows are skipped entirely during the merge walk.  SQL-style
     * NULLs-never-match semantics. */
    ray_t* lt_null_hdr = NULL, *rt_null_hdr = NULL;
    uint8_t* lt_null = left_n > 0
        ? (uint8_t*)scratch_alloc(&lt_null_hdr, (size_t)left_n)
        : NULL;
    uint8_t* rt_null = right_n > 0
        ? (uint8_t*)scratch_alloc(&rt_null_hdr, (size_t)right_n)
        : NULL;
    if ((!lt_null && left_n > 0) || (!rt_null && right_n > 0)) {
        if (lt_null_hdr) scratch_free(lt_null_hdr);
        if (rt_null_hdr) scratch_free(rt_null_hdr);
        if (lt_time_hdr) scratch_free(lt_time_hdr);
        if (rt_time_hdr) scratch_free(rt_time_hdr);
        return ray_error("oom", NULL);
    }
    if (left_n > 0) memset(lt_null, 0, (size_t)left_n);
    if (right_n > 0) memset(rt_null, 0, (size_t)right_n);
    if (lt_time_vec->attrs & RAY_ATTR_HAS_NULLS)
        for (int64_t i = 0; i < left_n; i++)
            if (ray_vec_is_null(lt_time_vec, i)) lt_null[i] = 1;
    if (rt_time_vec->attrs & RAY_ATTR_HAS_NULLS)
        for (int64_t i = 0; i < right_n; i++)
            if (ray_vec_is_null(rt_time_vec, i)) rt_null[i] = 1;
    for (uint8_t k = 0; k < n_eq; k++) {
        if (lt_eq[k]->attrs & RAY_ATTR_HAS_NULLS)
            for (int64_t i = 0; i < left_n; i++)
                if (ray_vec_is_null(lt_eq[k], i)) lt_null[i] = 1;
        if (rt_eq[k]->attrs & RAY_ATTR_HAS_NULLS)
            for (int64_t i = 0; i < right_n; i++)
                if (ray_vec_is_null(rt_eq[k], i)) rt_null[i] = 1;
    }

    /* Sort both tables by (eq_keys, time_key) using index arrays.  Rows
     * with any null key sort LAST (NULLS LAST) so the merge walk reaches
     * them once all real candidates are consumed and can skip them
     * cheaply. */
    ray_t* li_hdr = NULL, *ri_hdr = NULL;
    int64_t* li_idx = (int64_t*)scratch_alloc(&li_hdr, (size_t)left_n * sizeof(int64_t));
    int64_t* ri_idx = (int64_t*)scratch_alloc(&ri_hdr, (size_t)right_n * sizeof(int64_t));
    if ((!li_idx && left_n > 0) || (!ri_idx && right_n > 0)) {
        if (li_hdr) scratch_free(li_hdr);
        if (ri_hdr) scratch_free(ri_hdr);
        if (lt_null_hdr) scratch_free(lt_null_hdr);
        if (rt_null_hdr) scratch_free(rt_null_hdr);
        if (lt_time_hdr) scratch_free(lt_time_hdr);
        if (rt_time_hdr) scratch_free(rt_time_hdr);
        return ray_error("oom", NULL);
    }
    for (int64_t i = 0; i < left_n; i++) li_idx[i] = i;
    for (int64_t i = 0; i < right_n; i++) ri_idx[i] = i;

    /* Bottom-up mergesort on index arrays — O(N log N) */
    {
        int64_t max_n = left_n > right_n ? left_n : right_n;
        ray_t* tmp_hdr = NULL;
        int64_t* tmp = max_n > 0
            ? (int64_t*)scratch_alloc(&tmp_hdr, (size_t)max_n * sizeof(int64_t))
            : NULL;
        if (!tmp && max_n > 0) {
            scratch_free(li_hdr); scratch_free(ri_hdr);
            if (lt_null_hdr) scratch_free(lt_null_hdr);
            if (rt_null_hdr) scratch_free(rt_null_hdr);
            if (lt_time_hdr) scratch_free(lt_time_hdr);
            if (rt_time_hdr) scratch_free(rt_time_hdr);
            return ray_error("oom", NULL);
        }

        /* Sort left indices by (nulls-last, eq_keys, time) */
        for (int64_t width = 1; width < left_n; width *= 2) {
            for (int64_t lo = 0; lo < left_n; lo += 2 * width) {
                int64_t mid = lo + width;
                int64_t hi = lo + 2 * width;
                if (mid > left_n) mid = left_n;
                if (hi > left_n) hi = left_n;
                int64_t a = lo, b = mid, t = lo;
                while (a < mid && b < hi) {
                    int64_t ai = li_idx[a], bi = li_idx[b];
                    int cmp = 0;
                    if (lt_null[ai] != lt_null[bi])
                        cmp = lt_null[ai] - lt_null[bi]; /* 1 > 0 → nulls last */
                    for (uint8_t k2 = 0; k2 < n_eq && cmp == 0; k2++) {
                        int64_t va = read_col_i64(ray_data(lt_eq[k2]), ai, lt_eq[k2]->type, lt_eq[k2]->attrs);
                        int64_t vb = read_col_i64(ray_data(lt_eq[k2]), bi, lt_eq[k2]->type, lt_eq[k2]->attrs);
                        if (va < vb) cmp = -1;
                        else if (va > vb) cmp = 1;
                    }
                    if (cmp == 0) {
                        if (lt_time[ai] < lt_time[bi]) cmp = -1;
                        else if (lt_time[ai] > lt_time[bi]) cmp = 1;
                    }
                    tmp[t++] = (cmp <= 0) ? li_idx[a++] : li_idx[b++];
                }
                while (a < mid) tmp[t++] = li_idx[a++];
                while (b < hi) tmp[t++] = li_idx[b++];
                for (int64_t c = lo; c < hi; c++) li_idx[c] = tmp[c];
            }
        }

        /* Sort right indices by (nulls-last, eq_keys, time) */
        for (int64_t width = 1; width < right_n; width *= 2) {
            for (int64_t lo = 0; lo < right_n; lo += 2 * width) {
                int64_t mid = lo + width;
                int64_t hi = lo + 2 * width;
                if (mid > right_n) mid = right_n;
                if (hi > right_n) hi = right_n;
                int64_t a = lo, b = mid, t = lo;
                while (a < mid && b < hi) {
                    int64_t ai = ri_idx[a], bi = ri_idx[b];
                    int cmp = 0;
                    if (rt_null[ai] != rt_null[bi])
                        cmp = rt_null[ai] - rt_null[bi];
                    for (uint8_t k2 = 0; k2 < n_eq && cmp == 0; k2++) {
                        int64_t va = read_col_i64(ray_data(rt_eq[k2]), ai, rt_eq[k2]->type, rt_eq[k2]->attrs);
                        int64_t vb = read_col_i64(ray_data(rt_eq[k2]), bi, rt_eq[k2]->type, rt_eq[k2]->attrs);
                        if (va < vb) cmp = -1;
                        else if (va > vb) cmp = 1;
                    }
                    if (cmp == 0) {
                        if (rt_time[ai] < rt_time[bi]) cmp = -1;
                        else if (rt_time[ai] > rt_time[bi]) cmp = 1;
                    }
                    tmp[t++] = (cmp <= 0) ? ri_idx[a++] : ri_idx[b++];
                }
                while (a < mid) tmp[t++] = ri_idx[a++];
                while (b < hi) tmp[t++] = ri_idx[b++];
                for (int64_t c = lo; c < hi; c++) ri_idx[c] = tmp[c];
            }
        }

        if (tmp_hdr) scratch_free(tmp_hdr);
    }

    /* Build match array: for each left row (sorted), find best right match */
    ray_t* match_hdr = NULL;
    int64_t* match = (int64_t*)scratch_alloc(&match_hdr, (size_t)left_n * sizeof(int64_t));
    if (!match && left_n > 0) {
        scratch_free(li_hdr); scratch_free(ri_hdr);
        if (lt_null_hdr) scratch_free(lt_null_hdr);
        if (rt_null_hdr) scratch_free(rt_null_hdr);
        if (lt_time_hdr) scratch_free(lt_time_hdr);
        if (rt_time_hdr) scratch_free(rt_time_hdr);
        return ray_error("oom", NULL);
    }

    /* Two-pointer merge with best-match carry-forward.  Because the sort
     * pins null-keyed rows to the end, skipping them is just an early
     * "no match" for left and a plain `rp++` for right. */
    int64_t rp = 0;        /* right pointer (only advances) */
    int64_t best_ri = -1;  /* best right match in current partition */
    /* Track the previous *non-null* left row for partition-change detection
     * so a null-keyed left row doesn't force an incorrect partition reset
     * (and so its own null keys aren't read through read_col_i64). */
    int64_t prev_non_null_li = -1;
    for (int64_t lp = 0; lp < left_n; lp++) {
        int64_t li = li_idx[lp];

        if (lt_null[li]) {
            /* Null-keyed left row cannot match; in left-outer mode it
             * still appears in the result with all right cols null. */
            match[lp] = -1;
            continue;
        }

        /* Detect partition change — reset best match and rewind rp */
        if (prev_non_null_li >= 0) {
            int changed = 0;
            for (uint8_t k = 0; k < n_eq; k++) {
                int64_t cv = read_col_i64(ray_data(lt_eq[k]), li, lt_eq[k]->type, lt_eq[k]->attrs);
                int64_t pv = read_col_i64(ray_data(lt_eq[k]), prev_non_null_li, lt_eq[k]->type, lt_eq[k]->attrs);
                if (cv != pv) { changed = 1; break; }
            }
            if (changed) {
                best_ri = -1;
                /* Rewind rp to find start of new partition in right table */
                while (rp > 0) {
                    int64_t ri_prev = ri_idx[rp - 1];
                    if (rt_null[ri_prev]) break;
                    int eq_match = 1;
                    for (uint8_t k = 0; k < n_eq; k++) {
                        int64_t rv = read_col_i64(ray_data(rt_eq[k]), ri_prev, rt_eq[k]->type, rt_eq[k]->attrs);
                        int64_t lv = read_col_i64(ray_data(lt_eq[k]), li, lt_eq[k]->type, lt_eq[k]->attrs);
                        if (rv < lv) { eq_match = 0; break; }
                    }
                    if (!eq_match) break;
                    rp--;
                }
            }
        }

        /* Advance right pointer, accumulating best match */
        while (rp < right_n) {
            int64_t ri = ri_idx[rp];
            if (rt_null[ri]) { rp++; continue; }  /* null keys never match */
            int eq_cmp = 0;
            for (uint8_t k = 0; k < n_eq && eq_cmp == 0; k++) {
                int64_t rv = read_col_i64(ray_data(rt_eq[k]), ri, rt_eq[k]->type, rt_eq[k]->attrs);
                int64_t lv = read_col_i64(ray_data(lt_eq[k]), li, lt_eq[k]->type, lt_eq[k]->attrs);
                if (rv < lv) eq_cmp = -1;
                else if (rv > lv) eq_cmp = 1;
            }
            if (eq_cmp > 0) break;  /* right partition past left */
            if (eq_cmp == 0) {
                if (rt_time[ri] <= lt_time[li])
                    best_ri = ri;  /* valid candidate */
                else
                    break;  /* right time past left time */
            }
            rp++;
        }
        match[lp] = best_ri;
        prev_non_null_li = li;
    }

    /* Remap match[] from sorted order to original left-row order.
     * match[lp] gives the best right row for sorted left position lp.
     * We need match_orig[li] = best right row for original left row li. */
    ray_t* mo_hdr = NULL;
    int64_t* match_orig = (int64_t*)scratch_alloc(&mo_hdr, (size_t)left_n * sizeof(int64_t));
    if (!match_orig && left_n > 0) {
        scratch_free(match_hdr); scratch_free(li_hdr); scratch_free(ri_hdr);
        return ray_error("oom", NULL);
    }
    for (int64_t lp = 0; lp < left_n; lp++)
        match_orig[li_idx[lp]] = match[lp];

    /* Count output rows */
    int64_t out_n = 0;
    if (join_type == 1) {
        out_n = left_n;  /* left outer: all left rows */
    } else {
        for (int64_t i = 0; i < left_n; i++)
            if (match_orig[i] >= 0) out_n++;
    }

    /* Build output table */
    int64_t left_ncols  = ray_table_ncols(left_table);
    int64_t right_ncols = ray_table_ncols(right_table);

    /* Collect right column indices, excluding duplicate key columns */
    int64_t right_out_idx[256];
    int64_t right_out_count = 0;
    for (int64_t c = 0; c < right_ncols; c++) {
        int64_t rname = ray_table_col_name(right_table, c);
        int skip = 0;
        if (rname == time_sym) skip = 1;
        for (uint8_t k = 0; k < n_eq && !skip; k++)
            if (rname == eq_syms[k]) skip = 1;
        if (!skip) right_out_idx[right_out_count++] = c;
    }

    ray_t* out = ray_table_new(left_ncols + right_out_count);

    /* Build index arrays for gather so col_propagate_nulls_gather can
     * copy the null bitmap correctly (null bit in source → null bit in
     * output, plus explicit null for match_orig == -1 on the right side). */
    ray_t* lidx_hdr = NULL, *ridx_hdr = NULL;
    int64_t* lidx = out_n > 0
        ? (int64_t*)scratch_alloc(&lidx_hdr, (size_t)out_n * sizeof(int64_t))
        : NULL;
    int64_t* ridx = out_n > 0
        ? (int64_t*)scratch_alloc(&ridx_hdr, (size_t)out_n * sizeof(int64_t))
        : NULL;
    if (out_n > 0 && (!lidx || !ridx)) {
        if (lidx_hdr) scratch_free(lidx_hdr);
        if (ridx_hdr) scratch_free(ridx_hdr);
        scratch_free(mo_hdr);
        scratch_free(match_hdr);
        scratch_free(li_hdr);
        scratch_free(ri_hdr);
        if (lt_null_hdr) scratch_free(lt_null_hdr);
        if (rt_null_hdr) scratch_free(rt_null_hdr);
        if (lt_time_hdr) scratch_free(lt_time_hdr);
        if (rt_time_hdr) scratch_free(rt_time_hdr);
        return ray_error("oom", NULL);
    }
    {
        int64_t wi = 0;
        for (int64_t li = 0; li < left_n; li++) {
            if (join_type == 0 && match_orig[li] < 0) continue;
            lidx[wi] = li;
            ridx[wi] = match_orig[li];
            wi++;
        }
    }

    /* Gather left columns — iterate in original row order, preserve nulls */
    for (int64_t c = 0; c < left_ncols; c++) {
        int64_t col_name = ray_table_col_name(left_table, c);
        ray_t* src_col = ray_table_get_col_idx(left_table, c);
        int8_t ctype = src_col->type;
        ray_t* dst_col = ray_vec_new(ctype, out_n);

        uint8_t esz = ray_type_sizes[ctype];
        char* src = (char*)ray_data(src_col);
        char* dst = (char*)ray_data(dst_col);
        for (int64_t wi = 0; wi < out_n; wi++)
            memcpy(dst + wi * esz, src + lidx[wi] * esz, esz);
        dst_col->len = out_n;
        col_propagate_str_pool(dst_col, src_col);
        col_propagate_nulls_gather(dst_col, src_col, lidx, out_n);
        out = ray_table_add_col(out, col_name, dst_col);
        ray_release(dst_col);
    }

    /* Gather right columns (excluding key duplicates) — original left-row order.
     * For unmatched rows (ridx[wi] == -1) we memset 0 for the value and
     * rely on col_propagate_nulls_gather to set the null bit; the zero
     * bytes keep the vector well-formed when consumers ignore the null
     * bit. */
    for (int64_t rc = 0; rc < right_out_count; rc++) {
        int64_t cidx = right_out_idx[rc];
        int64_t col_name = ray_table_col_name(right_table, cidx);
        ray_t* src_col = ray_table_get_col_idx(right_table, cidx);
        int8_t ctype = src_col->type;
        ray_t* dst_col = ray_vec_new(ctype, out_n);

        uint8_t esz = ray_type_sizes[ctype];
        char* src = (char*)ray_data(src_col);
        char* dst = (char*)ray_data(dst_col);
        for (int64_t wi = 0; wi < out_n; wi++) {
            int64_t ri = ridx[wi];
            if (ri >= 0) memcpy(dst + wi * esz, src + ri * esz, esz);
            else         memset(dst + wi * esz, 0, esz);
        }
        dst_col->len = out_n;
        col_propagate_str_pool(dst_col, src_col);
        col_propagate_nulls_gather(dst_col, src_col, ridx, out_n);
        out = ray_table_add_col(out, col_name, dst_col);
        ray_release(dst_col);
    }

    if (lidx_hdr) scratch_free(lidx_hdr);
    if (ridx_hdr) scratch_free(ridx_hdr);
    scratch_free(mo_hdr);
    scratch_free(match_hdr);
    scratch_free(li_hdr);
    scratch_free(ri_hdr);
    if (lt_null_hdr) scratch_free(lt_null_hdr);
    if (rt_null_hdr) scratch_free(rt_null_hdr);
    if (lt_time_hdr) scratch_free(lt_time_hdr);
    if (rt_time_hdr) scratch_free(rt_time_hdr);
    return out;
}
