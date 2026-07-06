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
#include "ops/hash.h"
#include "ops/idxop.h"
#include "lang/internal.h"  /* sym_id_runtime, sym_domain_rep (sym-domain Phase 2) */

/* Test knob: force the legacy build-on-right behavior so the differential
 * harness can compare swap vs no-swap in one binary. */
bool ray_join_no_build_swap = false;
/* Diagnostic: how many radix inner-joins built on the smaller (left) side. */
uint64_t ray_join_build_swaps = 0;
/* Test knob: force every radix join to fall back to the chained path, so the
 * differential harness can compare radix-build vs chained-build on ordinary data. */
bool ray_join_force_dup_fallback = false;
/* Perf-gate bypass: disable the auto dup-fallback so the differential harness
 * can measure the pre-fix O(dup²) build in the same binary.  Independent of
 * ray_join_force_dup_fallback (which forces the fallback); this disables the
 * auto-trip only. */
bool ray_join_no_dup_fallback = false;
/* Diagnostic: radix joins that fell back due to pathological key duplication. */
uint64_t ray_join_dup_fallbacks = 0;

/* ── Hash helper (shared by radix and chained HT join paths) ──────────── */

static uint64_t hash_row_keys(ray_t** key_vecs, uint32_t n_keys, int64_t row) {
    uint64_t h = 0;
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* col = key_vecs[k];
        if (!col) continue;
        /* NULL key — produce unique hash that won't match any other row */
        if (ray_vec_is_null(col, row))
            return h ^ ((uint64_t)row * 0x9E3779B97F4A7C15ULL);
        uint64_t kh;
        if (col->type == RAY_F64) {
            kh = ray_hash_f64(((double*)ray_data(col))[row]);
        } else {
            int64_t kv = read_col_i64(ray_data(col), row, col->type, col->attrs);
            /* sym-domain Phase 2: a SYM cell id is a position in the
             * COLUMN's domain — build and probe sides must hash into ONE
             * id space, so canonicalize to the runtime id.  Runtime-domain
             * columns (the only pre-flip case) pass through unchanged. */
            if (col->type == RAY_SYM) kv = sym_id_runtime(col, kv);
            kh = ray_hash_i64(kv);
        }
        h = (k == 0) ? kh : ray_hash_combine(h, kh);
    }
    return h;
}

/* hash_row_keys / join_keys_eq translate FILE-domain SYM cells through
 * the per-domain runtime-id LUT and run inside ray_pool_dispatch
 * workers.  The FIRST LUT request interns the domain's vocabulary into
 * the global table — a sequential one-time cost that must be paid HERE,
 * in join setup, never inside a worker (sym.c's frozen-table rule; a
 * worker-side intern would also leak the joined vocabulary build into
 * parallel execution).  No-op while every key is runtime-domain (LUT is
 * NULL for the singleton).
 *
 * Returns false on LUT-build OOM for a FILE domain — the caller MUST
 * abort the join: silent -1 translations downstream would make
 * cross-domain keys spuriously equal (7a-review hardening). */
static bool join_warm_sym_luts(ray_t* const* l_vecs, ray_t* const* r_vecs,
                               uint32_t n_keys) {
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* sides[2] = { l_vecs[k], r_vecs[k] };
        for (int s = 0; s < 2; s++) {
            ray_t* v = sides[s];
            if (!v || v->type != RAY_SYM) continue;
            struct ray_sym_domain_s* dom = ray_sym_vec_domain(v);
            if (dom == ray_sym_runtime_domain()) continue;
            if (!ray_sym_domain_runtime_lut(dom)) return false;
        }
    }
    return true;
}

/* ============================================================================
 * Radix-partitioned hash join
 *
 * Four-phase pipeline:
 *   Pass 1: Partition both sides by radix bits of hash (parallel)
 *   Pass 2: Per-partition build + probe with open-addressing HT (parallel)
 *   Pass 3: Gather output columns from matched pairs (parallel)
 *   Pass 4: Fallback to chained HT for small joins (< RAY_PARALLEL_THRESHOLD)
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
    uint32_t  n_keys;
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
 *   Pass 1 (sequential): Build chained hash table on right side
 *   Pass 2 (parallel):   Two-pass probe — count matches, prefix-sum, fill
 *   Pass 3 (parallel):   Column gather — assemble result columns
 * ============================================================================ */

/* Key equality helper — shared by count + fill phases */
static inline bool join_keys_eq(ray_t* const* l_vecs, ray_t* const* r_vecs, uint32_t n_keys,
                                 int64_t l, int64_t r) {
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* lc = l_vecs[k];
        ray_t* rc = r_vecs[k];
        if (!lc || !rc) return false;
        /* NULL != NULL in join predicates */
        if (ray_vec_is_null(lc, l) || ray_vec_is_null(rc, r)) return false;
        if (lc->type == RAY_F64) {
            if (((double*)ray_data(lc))[l] != ((double*)ray_data(rc))[r]) return false;
        } else {
            int64_t lv = read_col_i64(ray_data(lc), l, lc->type, lc->attrs);
            int64_t rv = read_col_i64(ray_data(rc), r, rc->type, rc->attrs);
            /* Raw SYM ids only compare within ONE domain — gate on domain
             * identity and translate both sides to runtime ids on mismatch
             * (collection.c hashset precedent; matches hash_row_keys'
             * canonical hashing).  Same-domain compares stay raw: per-domain
             * translation is injective, so raw equality ⇔ runtime equality.
             * Pre-flip both sides are the runtime singleton: byte-identical
             * fast path. */
            if (lc->type == RAY_SYM && rc->type == RAY_SYM &&
                ray_sym_vec_domain(lc) != ray_sym_vec_domain(rc)) {
                lv = sym_id_runtime(lc, lv);
                rv = sym_id_runtime(rc, rv);
            }
            if (lv != rv) return false;
        }
    }
    return true;
}

/* ── Per-partition open-addressing build + probe ─────────────────────── */

#define RADIX_HT_EMPTY UINT32_MAX

/* A build key with more than this many duplicate rows is pathological
 * (O(dup²) build); abort to the chained path.  Counts same-hash slots, so
 * dense moderate keys whose clusters merge into a long run don't trip. */
#define RADIX_DUP_RUN_MAX 512

/* Per-partition single-pass build+probe context.
 * Each partition writes to its own local output buffer, then results
 * are consolidated into contiguous arrays afterward. */
typedef struct {
    join_radix_part_t*  l_parts;
    join_radix_part_t*  r_parts;
    ray_t**         l_key_vecs;
    ray_t**         r_key_vecs;
    uint32_t       n_keys;
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
    _Atomic(uint8_t)  pathological;  /* set on long-run duplication or forced */
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

/* NOTE: l_xx/r_xx (l_parts/r_parts/l_key_vecs/r_key_vecs) and lr/rr/pl/pr denote
 * PROBE/BUILD roles, not logical left/right.  Under the build-side swap the
 * physical right becomes the build side and physical left the probe side.
 * Logical left/right is restored at the consolidation remap in exec_join. */
static void join_radix_build_probe_fn(void* raw, uint32_t wid, int64_t task_start, int64_t task_end) {
    (void)wid; (void)task_end;
    join_radix_bp_ctx_t* c = (join_radix_bp_ctx_t*)raw;
    uint32_t p = (uint32_t)task_start;

    join_radix_part_t* rp = &c->r_parts[p];
    join_radix_part_t* lp = &c->l_parts[p];

    /* Test knob: force the chained-path fallback.  Bail before allocating
     * anything (pp headers are still NULL → cleanup-safe). */
    if (ray_join_force_dup_fallback) {
        atomic_store_explicit(&c->pathological, 1, memory_order_relaxed);
        return;
    }

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
        /* Count rows of THIS key (same hash) already inserted — the true
         * per-key duplication.  Total run length would conflate one giant
         * key (pathological O(dup²) build) with many moderate keys whose
         * dense clusters merge into a long run (fine); counting same-hash
         * slots is immune to that collision-merge.  Accumulate `same`
         * branchlessly with NO global read / NO goto in the loop body: an
         * in-loop trip check makes the compiler clone the probe loop on the
         * (loop-invariant) bypass knob and pessimise the production variant
         * (~55% regression at moderate dup, measured).  Trip once, after. */
        uint32_t same = 0;
        while (ht[slot * 2 + 1] != RADIX_HT_EMPTY) {
            same += (ht[slot * 2] == h);
            slot = (slot + 1) & ht_mask;
        }
        if (same > RADIX_DUP_RUN_MAX && !ray_join_no_dup_fallback) {
            /* Pathological duplication — abort to the chained path.
             * `done:` frees ht_hdr and leaves pp buffers cleanup-safe. */
            atomic_store_explicit(&c->pathological, 1, memory_order_relaxed);
            goto done;
        }
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
    uint32_t n_keys;
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
    uint32_t     n_keys;
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

/* Pass 2a: count matches per morsel */
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

/* Pass 2b: fill match pairs using pre-computed offsets */
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
    uint32_t n_keys = ext->join.n_join_keys;
    uint8_t join_type = ext->join.join_type;

    /* n_keys is unbounded post-lift (no fixed key-count ceiling) — a stack
     * VLA here would be a stack blow for a wide join.  One zero-inited
     * carve holds both l_key_vecs and r_key_vecs (key_slots each); guard
     * >=1 slot so the carve is never zero-sized when n_keys == 0.  Freed
     * on every return between here and the end of the function (join_cleanup
     * covers the fall-through paths; direct returns free it explicitly). */
    size_t key_slots = n_keys ? n_keys : 1;
    ray_t* key_vecs_hdr = NULL;
    ray_t** key_vecs = (ray_t**)scratch_calloc(&key_vecs_hdr, key_slots * 2 * sizeof(ray_t*));
    if (!key_vecs) return ray_error("oom", NULL);
    ray_t** l_key_vecs = key_vecs;
    ray_t** r_key_vecs = key_vecs + key_slots;

    for (uint32_t k = 0; k < n_keys; k++) {
        ray_op_ext_t* lk = find_ext(g, ext->join.left_keys[k]);
        ray_op_ext_t* rk = find_ext(g, ext->join.right_keys[k]);
        if (lk && lk->base.opcode == OP_SCAN)
            l_key_vecs[k] = ray_table_get_col(left_table, lk->sym);
        if (rk && rk->base.opcode == OP_SCAN)
            r_key_vecs[k] = ray_table_get_col(right_table, rk->sym);
        if (rk && rk->base.opcode == OP_CONST && rk->literal)
            r_key_vecs[k] = rk->literal;
    }

    /* RAY_STR keys not yet supported (16-byte elements vs 8-byte hash/eq slots) */
    for (uint32_t k = 0; k < n_keys; k++) {
        if ((l_key_vecs[k] && l_key_vecs[k]->type == RAY_STR) ||
            (r_key_vecs[k] && r_key_vecs[k]->type == RAY_STR)) {
            scratch_free(key_vecs_hdr);
            return ray_error("nyi", NULL);
        }
    }

    /* Sequential LUT warm-up BEFORE any dispatch (see join_warm_sym_luts). */
    if (!join_warm_sym_luts(l_key_vecs, r_key_vecs, n_keys)) {
        scratch_free(key_vecs_hdr);
        return ray_error("oom", "join: sym domain runtime-id LUT build failed");
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
    ray_t* out_cols_hdr = NULL;
    int64_t* l_idx = NULL;
    int64_t* r_idx = NULL;
    int64_t pair_count = 0;
    _Atomic(uint8_t)* matched_right = NULL;

    /* ── Radix-partitioned path (large joins) ──────────────────────── */
    if (right_rows > RAY_PARALLEL_THRESHOLD) {
        /* Build on the smaller side for INNER joins (radix path).  Other
         * join types stay build-on-right (LEFT/FULL/ANTI are asymmetric — a
         * swap would change their result).  SWAP_MARGIN ≥ 1: require LEFT
         * (×margin) strictly smaller; default 1.  Knob forces legacy. */
        #define JOIN_SWAP_MARGIN 1
        bool swap = (join_type == 0) && !ray_join_no_build_swap &&
                    (left_rows * (int64_t)JOIN_SWAP_MARGIN < right_rows);
        if (swap) ray_join_build_swaps++;
        int64_t  build_rows = swap ? left_rows : right_rows;
        int64_t  probe_rows = swap ? right_rows : left_rows;
        ray_t**  build_keys = swap ? l_key_vecs : r_key_vecs;
        ray_t**  probe_keys = swap ? r_key_vecs : l_key_vecs;
        uint8_t radix_bits = radix_join_bits(build_rows);
        uint32_t n_rparts = (uint32_t)1 << radix_bits;

        /* Pre-compute hashes for both sides (once, reused by histogram+scatter) */
        ray_t* r_hash_hdr = NULL;
        uint32_t* r_hashes = (uint32_t*)scratch_alloc(&r_hash_hdr,
                                (size_t)build_rows * sizeof(uint32_t));
        ray_t* l_hash_hdr = NULL;
        uint32_t* l_hashes = (uint32_t*)scratch_alloc(&l_hash_hdr,
                                (size_t)probe_rows * sizeof(uint32_t));
        if (!r_hashes || !l_hashes) {
            if (r_hash_hdr) scratch_free(r_hash_hdr);
            if (l_hash_hdr) scratch_free(l_hash_hdr);
            goto chained_ht_fallback;
        }
        join_radix_hash_ctx_t rhctx = { .key_vecs = build_keys, .n_keys = n_keys, .hashes = r_hashes };
        join_radix_hash_ctx_t lhctx = { .key_vecs = probe_keys, .n_keys = n_keys, .hashes = l_hashes };
        if (pool) {
            ray_pool_dispatch(pool, join_radix_hash_fn, &rhctx, build_rows);
            ray_pool_dispatch(pool, join_radix_hash_fn, &lhctx, probe_rows);
        } else {
            join_radix_hash_fn(&rhctx, 0, 0, build_rows);
            join_radix_hash_fn(&lhctx, 0, 0, probe_rows);
        }

        if (pool_cancelled(pool)) {
            scratch_free(r_hash_hdr); scratch_free(l_hash_hdr);
            scratch_free(key_vecs_hdr);
            return ray_error("cancel", NULL);
        }

        /* Partition both sides using cached hashes */
        ray_t* r_parts_hdr = NULL;
        join_radix_part_t* r_parts = join_radix_partition(pool, build_rows,
                                                          radix_bits, r_hashes, &r_parts_hdr);
        ray_t* l_parts_hdr = NULL;
        join_radix_part_t* l_parts = join_radix_partition(pool, probe_rows,
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
            scratch_free(key_vecs_hdr);
            return ray_error("cancel", NULL);
        }

        /* FULL OUTER: allocate matched_right tracker */
        if (join_type == 2 && right_rows > 0) {
            // cppcheck-suppress internalAstError // _Atomic(T)* cast breaks cppcheck parser (see :1167)
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
            .l_key_vecs = probe_keys, .r_key_vecs = build_keys,
            .n_keys = n_keys, .join_type = join_type,
            .pp_l = pp_l, .pp_r = pp_r,
            .pp_l_hdr = pp_l_hdr, .pp_r_hdr = pp_r_hdr,
            .part_counts = part_counts, .pp_cap = pp_cap,
            .matched_right = matched_right,
            .had_error = 0,
            .pathological = 0,
        };
        if (pool && n_rparts > 1)
            ray_pool_dispatch_n(pool, join_radix_build_probe_fn, &bp_ctx, n_rparts);
        else
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++)
                join_radix_build_probe_fn(&bp_ctx, 0, rp2, rp2 + 1);

        /* Check cancellation and errors during build+probe */
        bool bp_cancelled = pool_cancelled(pool);
        bool bp_error = atomic_load_explicit(&bp_ctx.had_error, memory_order_relaxed);
        bool bp_pathological = atomic_load_explicit(&bp_ctx.pathological, memory_order_relaxed);
        if (bp_cancelled || bp_error || bp_pathological) {
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
            if (bp_cancelled) { scratch_free(key_vecs_hdr); return ray_error("cancel", NULL); }
            if (bp_pathological) ray_join_dup_fallbacks++;
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
                scratch_free(key_vecs_hdr);
                return ray_error("oom", NULL);
            }

            /* Copy per-partition results into contiguous arrays (int32→int64 widen) */
            int64_t off = 0;
            for (uint32_t rp2 = 0; rp2 < n_rparts; rp2++) {
                int64_t cnt = part_counts[rp2];
                if (cnt > 0 && pp_l[rp2] && pp_r[rp2]) {
                    for (int64_t j = 0; j < cnt; j++) {
                        int32_t probe_row = pp_l[rp2][j];   /* PROBE side row */
                        int32_t build_row = pp_r[rp2][j];   /* BUILD side row */
                        l_idx[off + j] = (int64_t)(swap ? build_row : probe_row);
                        r_idx[off + j] = (int64_t)(swap ? probe_row : build_row);
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
        scratch_free(key_vecs_hdr);
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

    /* Pass 1.5: S-Join semijoin filter extraction.
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

    /* Pass 2: Parallel probe (two-pass: count → prefix-sum → fill) */
    uint32_t n_tasks = (uint32_t)((left_rows + JOIN_MORSEL - 1) / JOIN_MORSEL);
    if (n_tasks == 0) n_tasks = 1;

    int64_t* morsel_counts = (int64_t*)scratch_calloc(&counts_hdr,
                              (size_t)(n_tasks + 1) * sizeof(int64_t));
    if (!morsel_counts) {
        scratch_free(ht_next_hdr); scratch_free(ht_heads_hdr);
        scratch_free(key_vecs_hdr);
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
    /* Pass 3: Build result table with parallel column gather.
     * Use multi_gather for batched column access when possible (non-nullable
     * indices), falling back to per-column gather for nullable RIGHT columns. */
    int64_t left_ncols = ray_table_ncols(left_table);
    int64_t right_ncols = ray_table_ncols(right_table);
    result = ray_table_new(left_ncols + right_ncols);
    if (!result || RAY_IS_ERR(result)) goto join_cleanup;

    /* Output-column arrays sized to the actual table widths — the gather
     * stage below batches through multi_gather only when a side fits
     * MGATHER_MAX_COLS and falls back to per-column gather otherwise, so
     * no column may be dropped here (a fixed [16] cap used to silently
     * truncate wide-table join results). */
    {
        size_t slots = (size_t)(left_ncols * 2 + right_ncols * 3);
        void* out_mem = scratch_alloc(&out_cols_hdr,
                                      (slots ? slots : 1) * sizeof(void*));
        if (!out_mem) {
            ray_release(result);
            result = ray_error("oom", NULL);
            goto join_cleanup;
        }
    }
    ray_t** l_out_cols  = (ray_t**)ray_data(out_cols_hdr);
    int64_t* l_out_names = (int64_t*)(l_out_cols + left_ncols);
    ray_t** r_out_cols  = (ray_t**)(l_out_names + left_ncols);
    ray_t** r_src_cols  = r_out_cols + right_ncols;
    int64_t* r_out_names = (int64_t*)(r_src_cols + right_ncols);

    /* Allocate all output columns upfront for batched gather */
    int64_t l_out_count = 0;
    for (int64_t c = 0; c < left_ncols; c++) {
        ray_t* col = ray_table_get_col_idx(left_table, c);
        if (!col) continue;
        ray_t* new_col = col_vec_new(col, pair_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = pair_count;
        l_out_cols[l_out_count] = new_col;
        l_out_names[l_out_count] = ray_table_col_name(left_table, c);
        l_out_count++;
    }

    int64_t r_out_count = 0;
    for (int64_t c = 0; c < right_ncols; c++) {
        ray_t* col = ray_table_get_col_idx(right_table, c);
        int64_t name_id = ray_table_col_name(right_table, c);
        if (!col) continue;
        bool is_key = false;
        for (uint32_t k = 0; k < n_keys; k++) {
            ray_op_ext_t* rk = find_ext(g, ext->join.right_keys[k]);
            if (rk && rk->base.opcode == OP_SCAN && rk->sym == name_id) {
                is_key = true; break;
            }
        }
        if (is_key) continue;
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

    /* Propagate RAY_STR string pools and null bitmaps from source columns.
     * SYM outputs gather raw cell ids from exactly one side's column —
     * adopt that source's domain (sym-domain Phase 2; no-op while all
     * domains are the runtime singleton). */
    {
        int64_t si = 0;
        for (int64_t c = 0; c < left_ncols && si < l_out_count; c++) {
            ray_t* col = ray_table_get_col_idx(left_table, c);
            if (!col) continue;
            col_propagate_str_pool(l_out_cols[si], col);
            col_propagate_nulls_gather(l_out_cols[si], col, l_idx, pair_count);
            if (l_out_cols[si]->type == RAY_SYM)
                ray_sym_vec_adopt_domain(l_out_cols[si], col);
            si++;
        }
    }
    for (int64_t i = 0; i < r_out_count; i++) {
        col_propagate_str_pool(r_out_cols[i], r_src_cols[i]);
        col_propagate_nulls_gather(r_out_cols[i], r_src_cols[i], r_idx, pair_count);
        if (r_out_cols[i]->type == RAY_SYM)
            ray_sym_vec_adopt_domain(r_out_cols[i], r_src_cols[i]);
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
    if (out_cols_hdr) scratch_free(out_cols_hdr);
    if (ht_next_hdr) scratch_free(ht_next_hdr);
    if (ht_heads_hdr) scratch_free(ht_heads_hdr);
    scratch_free(l_idx_hdr);
    scratch_free(r_idx_hdr);
    if (counts_hdr) scratch_free(counts_hdr);
    scratch_free(matched_right_hdr);
    scratch_free(key_vecs_hdr);
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

    uint32_t n_keys = ext->join.n_join_keys;

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

    /* n_keys is unbounded post-lift (query.c join_impl/antijoin_impl no
     * longer cap at 16; datalog.c callers stay bounded by DL_MAX_ARITY,
     * a separate cap on a different call path) — carve instead of a fixed
     * [16] or a stack VLA (both wrong for an unbounded key count).  One
     * zero-inited carve holds both l_key_vecs and r_key_vecs. */
    ray_t* key_vecs_hdr = NULL;
    ray_t** key_vecs = (ray_t**)scratch_calloc(&key_vecs_hdr,
                            (size_t)(n_keys ? n_keys : 1) * 2 * sizeof(ray_t*));
    if (!key_vecs) return ray_error("oom", NULL);
    ray_t** l_key_vecs = key_vecs;
    ray_t** r_key_vecs = key_vecs + n_keys;

    for (uint32_t k = 0; k < n_keys; k++) {
        ray_op_ext_t* lk = find_ext(g, ext->join.left_keys[k]);
        ray_op_ext_t* rk = find_ext(g, ext->join.right_keys[k]);
        if (lk && lk->base.opcode == OP_SCAN)
            l_key_vecs[k] = ray_table_get_col(left_table, lk->sym);
        if (rk && rk->base.opcode == OP_SCAN)
            r_key_vecs[k] = ray_table_get_col(right_table, rk->sym);
        if (rk && rk->base.opcode == OP_CONST && rk->literal)
            r_key_vecs[k] = rk->literal;
    }

    /* RAY_STR keys not yet supported */
    for (uint32_t k = 0; k < n_keys; k++) {
        if ((l_key_vecs[k] && l_key_vecs[k]->type == RAY_STR) ||
            (r_key_vecs[k] && r_key_vecs[k]->type == RAY_STR)) {
            scratch_free(key_vecs_hdr);
            return ray_error("nyi", NULL);
        }
    }

    /* Sequential LUT warm-up BEFORE the parallel build dispatch. */
    if (!join_warm_sym_luts(l_key_vecs, r_key_vecs, n_keys)) {
        scratch_free(key_vecs_hdr);
        return ray_error("oom", "join: sym domain runtime-id LUT build failed");
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
        scratch_free(key_vecs_hdr);
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
        scratch_free(key_vecs_hdr);
        return ray_error("cancel", NULL);
    }

    /* Probe: scan left rows, collect indices of those with NO match */
    ray_t* out_idx_hdr = NULL;
    int64_t* out_idx = (int64_t*)scratch_alloc(&out_idx_hdr,
                            (size_t)left_rows * sizeof(int64_t));
    if (!out_idx) {
        scratch_free(ht_next_hdr);
        scratch_free(ht_heads_hdr);
        scratch_free(key_vecs_hdr);
        return ray_error("oom", NULL);
    }

    uint32_t ht_mask = ht_cap - 1;
    int64_t out_count = 0;
    for (int64_t l = 0; l < left_rows; l++) {
        /* Cancellation checkpoint — this probe is fully serial (not pool
         * dispatched), so it needs its own check.  Masked to stay off the
         * per-row hot path; frees the live scratch on a cancel. */
        if ((l & 0xFFFF) == 0 && ray_interrupted()) {
            scratch_free(out_idx_hdr);
            scratch_free(ht_next_hdr);
            scratch_free(ht_heads_hdr);
            scratch_free(key_vecs_hdr);
            return ray_error("cancel", NULL);
        }
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
    scratch_free(key_vecs_hdr);

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
            /* Null-model invariant 16.4: the gather copies sentinel cells
             * verbatim (e.g. an anti-join preserves a null-key left row) but
             * gather_fn does not flag HAS_NULLS — propagate it from the
             * source, mirroring the inner/outer join gather path. */
            col_propagate_nulls_gather(new_col, col, out_idx, out_count);
            /* SYM output gathers raw cell ids from `col` — resolve over
             * the same dictionary (sym-domain Phase 2). */
            if (new_col->type == RAY_SYM)
                ray_sym_vec_adopt_domain(new_col, col);

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

/* True iff `tvals` (a side's time values, already expanded to int64) is
 * non-descending within every part described by keycol's RAY_IDX_PART index.
 * The part index guarantees rows are already grouped into contiguous,
 * ascending key value-blocks; if time is also non-descending inside each
 * block, the natural row order already equals the (key, time) order the
 * merge expects, so that side's sort can be skipped.  Caller must have
 * checked ray_index_kind(keycol)==RAY_IDX_PART, and must separately ensure
 * the time column has no nulls (null-keyed rows must sort last, which the
 * identity order would not do).  Caller must also confirm the index's
 * built_for_len matches the current column length, so the part ranges
 * (starts/lens) are in-bounds for the time array indexed here. */
/* Left-side eq-key reader for the asof sort-merge.  Eq keys are MATCHED
 * across the two tables, and raw SYM ids only order/compare within ONE
 * domain (sym-domain Phase 2): when a SYM eq-key pair spans two domains,
 * re-express the LEFT cell in the RIGHT side's id space (absent symbol →
 * -1: sorts first, matches nothing — right ids are ≥ 0).  Every left-side
 * eq read below goes through this reader, so the left sort, the partition
 * rewind and the merge walk all order by the right side's id space, while
 * right-side reads stay raw (which is also why the per-domain RUNTIME-id
 * LUT doesn't fit here: the target space is the RIGHT domain, not the
 * global table).  `xlut` is the per-key left-position → right-position
 * table built ONCE at setup (NULL for same-domain / non-SYM keys —
 * pre-flip always, byte-identical raw read); `xn` is its entry count.
 * No per-read hash lookups: the old str+find per read degenerated the
 * absent-key rewind into O(L·R) hash probes. */
static inline int64_t asof_eq_lread(ray_t* lc, const int64_t* xlut, int64_t xn,
                                    int64_t row) {
    int64_t v = read_col_i64(ray_data(lc), row, lc->type, lc->attrs);
    if (xlut) v = (v >= 0 && v < xn) ? xlut[v] : -1;
    return v;
}

/* Cross-domain SYM eq key with an EMPTY left vocabulary: nothing can
 * translate, but the reader still needs a non-NULL marker so raw left
 * ids never leak into the right id space. */
static const int64_t g_asof_empty_xlut[1] = { -1 };

static bool asof_time_sorted_within_parts(const ray_t* keycol, const int64_t* tvals) {
    ray_index_t* ix = ray_index_payload(((ray_t*)keycol)->index);
    const int64_t* st = (const int64_t*)ray_data(ix->u.part.starts);
    const int64_t* ln = (const int64_t*)ray_data(ix->u.part.lens);
    for (int64_t p = 0; p < ix->u.part.n_parts; p++) {
        int64_t s = st[p], e = st[p] + ln[p];
        for (int64_t i = s + 1; i < e; i++)
            if (tvals[i] < tvals[i - 1]) return false;
    }
    return true;
}

/* ── Hash-group asof strategy ────────────────────────────────────────────
 * Build the set of DISTINCT LEFT key tuples, collect the right rows of
 * those (and only those) groups in one sequential right pass, then answer
 * each left row with a binary search over its group's (row-ordered)
 * right-row list for the last right time <= left time.  O(L + R + L·log G)
 * with NO sorting of either side and no work on unprobed right keys — the
 * sort-merge below pays O((L+R)·log(L+R)) per query, which for a small
 * probe against a large right table is almost entirely wasted work.
 * (Rayforce v1's asof/window joins used this hash-group + binary-search
 * structure; the v2 sort-merge rewrite is what regressed them.)
 *
 * Precondition — VERIFIED, not assumed: right time must be non-descending
 * within each key group over its non-null rows.  True whenever the right
 * table is time-sorted (the canonical asof input).  On violation, on OOM,
 * or for >2^31-row sides this returns false with match_orig untouched and
 * the caller runs the sort-merge, which is always correct.
 *
 * Id spaces mirror the merge exactly: right cells are read raw,
 * left cells through asof_eq_lread (cross-domain SYM translated into the
 * right domain), so a hash-probe equality holds iff the merge would place
 * the rows in the same partition.  Tie times match the merge too: the
 * binary search takes the LAST group row with time <= t, which is the
 * merge's final best_ri carry (its sort is stable across equal times). */
/* Comparator context-free sort element for the cursor-merge variant. */
typedef struct { int64_t t; int32_t li; } asof_lslot_t;
static int asof_lslot_cmp(const void* a, const void* b) {
    int64_t x = ((const asof_lslot_t*)a)->t;
    int64_t y = ((const asof_lslot_t*)b)->t;
    return (x > y) - (x < y);
}

/* Per-slice ascending-time verify over a collected probe set (see the
 * single-eq index variant below).  gsl holds n_groups (grows, gn) pairs.
 * Contiguous slices verify with a sequential scan over rt_time; scattered
 * ones fall back to the gathered walk.  Above the parallel threshold the
 * pairs are chunked across the pool; each chunk covers the adjacent
 * pairs (i-1, i) for i in [max(start,1), end) within its slice, so
 * chunk boundaries re-check the crossing pair and no pair is skipped.
 * Returns false at the first violation (shared bail flag). */
typedef struct { const int64_t* grows; int64_t lo, hi; } asof_vchunk_t;
typedef struct {
    const asof_vchunk_t* chunks;
    const int64_t* rt_time;
    volatile int* ok;
} asof_vctx_t;

static void asof_verify_fn(void* raw, uint32_t wid, int64_t cstart,
                           int64_t cend) {
    (void)wid;
    asof_vctx_t* c = (asof_vctx_t*)raw;
    const int64_t* restrict t = c->rt_time;
    for (int64_t ci = cstart; ci < cend; ci++) {
        if (!*c->ok) return;
        const asof_vchunk_t* ch = &c->chunks[ci];
        const int64_t* restrict rows = ch->grows;
        int64_t lo = ch->lo, hi = ch->hi;
        if (lo < 1) lo = 1;
        bool contig = (hi > lo) &&
                      (rows[hi - 1] - rows[lo - 1] == hi - lo);
        if (contig) {
            /* Branchless block accumulation: `bad += (next < cur)` maps to
             * packed i64 compares (the early-exit form stays scalar), with
             * a per-block bail so a violation still stops the scan early. */
            const int64_t* restrict tv = t + rows[lo - 1];
            int64_t n = hi - lo;
            int64_t bad = 0;
            for (int64_t b = 0; b < n; b += 4096) {
                int64_t e = b + 4096 < n ? b + 4096 : n;
                for (int64_t i = b; i < e; i++)
                    bad += (int64_t)(tv[i + 1] < tv[i]);
                if (bad) { *c->ok = 0; return; }
            }
        } else {
            for (int64_t i = lo; i < hi; i++)
                if (t[rows[i]] < t[rows[i - 1]]) { *c->ok = 0; return; }
        }
    }
}

#define ASOF_VERIFY_CHUNK 65536
static bool asof_verify_slices(const int64_t* gsl, int32_t n_groups,
                               const int64_t* rt_time, int64_t total_rows) {
    int64_t n_chunks = 0;
    for (int32_t g = 0; g < n_groups; g++)
        n_chunks += (gsl[2 * g + 1] + ASOF_VERIFY_CHUNK - 1)
                    / ASOF_VERIFY_CHUNK;
    if (n_chunks <= 0) return true;
    ray_t* ch_hdr = NULL;
    asof_vchunk_t* chunks = (asof_vchunk_t*)scratch_alloc(&ch_hdr,
                                (size_t)n_chunks * sizeof(asof_vchunk_t));
    if (!chunks) {
        /* OOM — serial gathered walk, no allocation. */
        for (int32_t g = 0; g < n_groups; g++) {
            const int64_t* rows = (const int64_t*)(intptr_t)gsl[2 * g];
            int64_t gn = gsl[2 * g + 1];
            for (int64_t p = 1; p < gn; p++)
                if (rt_time[rows[p]] < rt_time[rows[p - 1]]) return false;
        }
        return true;
    }
    int64_t w = 0;
    for (int32_t g = 0; g < n_groups; g++) {
        const int64_t* rows = (const int64_t*)(intptr_t)gsl[2 * g];
        int64_t gn = gsl[2 * g + 1];
        for (int64_t lo = 0; lo < gn; lo += ASOF_VERIFY_CHUNK) {
            int64_t hi = lo + ASOF_VERIFY_CHUNK;
            if (hi > gn) hi = gn;
            chunks[w].grows = rows;
            chunks[w].lo = lo;
            chunks[w].hi = hi;
            w++;
        }
    }
    volatile int vok = 1;
    asof_vctx_t ctx = { chunks, rt_time, &vok };
    ray_pool_t* pool = ray_pool_get();
    /* dispatch_n: the element count here is TASKS (a few dozen), far
     * below ray_pool_dispatch's element grain — the plain dispatch would
     * collapse them into one serial task. */
    if (pool && n_chunks > 1 && total_rows >= RAY_PARALLEL_THRESHOLD) {
        uint32_t nt = (n_chunks > 4096) ? 4096u : (uint32_t)n_chunks;
        /* fn receives a task-index range; map [ti_start, ti_end) over
         * chunks directly — dispatch_n hands out [i, i+1) per task. */
        (void)nt;
        ray_pool_dispatch_n(pool, asof_verify_fn, &ctx, (uint32_t)n_chunks);
    } else
        asof_verify_fn(&ctx, 0, 0, n_chunks);
    scratch_free(ch_hdr);
    return vok != 0;
}

static bool asof_hash_group_match(uint32_t n_eq,
                                  ray_t* const* lt_eq, ray_t* const* rt_eq,
                                  const int64_t* const* eq_xlut,
                                  const int64_t* eq_xn,
                                  const int64_t* lt_time,
                                  const int64_t* rt_time,
                                  const uint8_t* lt_null,
                                  const uint8_t* rt_null,
                                  int64_t left_n, int64_t right_n,
                                  bool rt_time_sorted, bool rt_no_nulls,
                                  int64_t* match_orig) {
    if (left_n <= 0) return true;
    if (right_n >= INT32_MAX - 1 || left_n >= INT32_MAX - 1) return false;
    if (right_n == 0) {
        for (int64_t i = 0; i < left_n; i++) match_orig[i] = -1;
        return true;
    }

    /* Groups are the DISTINCT LEFT key tuples — right rows whose keys no
     * left row probes are never touched beyond one read+miss, so a small
     * probe against a huge right table costs one sequential right pass,
     * not a full grouping of it.  Key values are stored translated
     * (asof_eq_lread), i.e. in the right side's id space; right rows probe
     * with raw reads — equality holds iff the merge would pair the rows. */
    uint64_t cap = 16;
    while (cap < (uint64_t)left_n * 2u) cap <<= 1;
    uint64_t mask = cap - 1;

    ray_t *tab_hdr = NULL, *keys_hdr = NULL, *gid_hdr = NULL,
          *cnt_hdr = NULL, *off_hdr = NULL, *rows_hdr = NULL,
          *lgid_hdr = NULL, *rowkey_hdr = NULL;
    int32_t* tab      = NULL;   /* slot -> group id + 1 (0 = empty) */
    int64_t* gkeys    = NULL;   /* n_groups × n_eq translated key values */
    int32_t* left_gid = NULL;   /* per left row: its group id (or -1) */
    int32_t* row_gid  = NULL;   /* per right row: matched group id (or -1) */
    int32_t* cnt      = NULL;
    int32_t* off      = NULL;
    int32_t* rows     = NULL;
    /* Single per-row eq-key scratch buffer, reused sequentially by Pass 1
     * below, the Pass 2 hash-scan fallback, and the ASOF_RESOLVE_GID macro's
     * OA-probe branch — none of those three loops is ever live at the same
     * time as another, so one buffer covers all of them.  For the common
     * bounded case (n_eq <= 16) stage into a STACK array (matches the
     * pre-cut-3 per-loop `int64_t lv[256]`/`rv[256]`/`_rv[256]` provenance a
     * unified heap carve gave up); wide keys (>16, unbounded — no fixed
     * ceiling) fall back to the exact-sized heap carve.  The pointer is chosen
     * ONCE here (no per-row branch). */
    int64_t  rowkey_stk[16];
    int64_t* rowkey_heap = NULL;

    left_gid = (int32_t*)scratch_alloc(&lgid_hdr, (size_t)left_n * 4);
    row_gid  = (int32_t*)scratch_alloc(&gid_hdr, (size_t)right_n * 4);
    if (n_eq > 0) {
        tab   = (int32_t*)scratch_alloc(&tab_hdr, (size_t)cap * 4);
        gkeys = (int64_t*)scratch_alloc(&keys_hdr,
                                        (size_t)left_n * n_eq * 8);
        if (n_eq > 16)
            rowkey_heap = (int64_t*)scratch_alloc(&rowkey_hdr, (size_t)n_eq * 8);
        if (tab) memset(tab, 0, (size_t)cap * 4);
    }
    int64_t* rowkey = (n_eq <= 16) ? rowkey_stk : rowkey_heap;
    bool ok = left_gid && row_gid &&
              (n_eq == 0 || (tab && gkeys && (n_eq <= 16 || rowkey_heap)));

    /* Pass 1 — distinct left keys.  n_groups <= left_n. */
    int32_t n_groups = 0;
    if (ok && n_eq == 0) {
        for (int64_t li = 0; li < left_n; li++)
            left_gid[li] = lt_null[li] ? -1 : 0;
        n_groups = 1;
    } else if (ok) {
        for (int64_t li = 0; li < left_n; li++) {
            if (lt_null[li]) { left_gid[li] = -1; continue; }
            /* rowkey: carved once above this loop, sized n_eq — see the
             * declaration comment. */
            uint64_t h = 0;
            for (uint32_t k = 0; k < n_eq; k++) {
                rowkey[k] = asof_eq_lread(lt_eq[k], eq_xlut[k], eq_xn[k], li);
                uint64_t kh = ray_hash_i64(rowkey[k]);
                h = (k == 0) ? kh : ray_hash_combine(h, kh);
            }
            uint64_t slot = h & mask;
            for (;;) {
                int32_t gp1 = tab[slot];
                if (gp1 == 0) {
                    tab[slot] = n_groups + 1;
                    for (uint32_t k = 0; k < n_eq; k++)
                        gkeys[(int64_t)n_groups * n_eq + k] = rowkey[k];
                    left_gid[li] = n_groups;
                    n_groups++;
                    break;
                }
                const int64_t* gk = gkeys + (int64_t)(gp1 - 1) * n_eq;
                int eq = 1;
                for (uint32_t k = 0; k < n_eq; k++)
                    if (gk[k] != rowkey[k]) { eq = 0; break; }
                if (eq) { left_gid[li] = gp1 - 1; break; }
                slot = (slot + 1) & mask;
            }
        }
    }

    /* Declared before the index variant's `goto done` so the shared frees
     * see initialized handles on every route. */
    ray_t *lut_hdr = NULL, *gnx_hdr = NULL;
    int32_t* sym_lut  = NULL;
    int32_t* gid_next = NULL;
    int64_t  lut_d    = 0;

    /* The caller's signal is the verified `sorted` attr, but the canonical
     * asof right side is time-sorted whether or not anyone stamped the
     * attr (xasc does not) — check directly, bailing at the first
     * violation, so the sequential scan is only paid when it's true and
     * the cursor merge it enables repays it many times over. */
    if (ok && !rt_time_sorted && right_n > 1) {
        rt_time_sorted = true;
        for (int64_t i = 1; i < right_n; i++)
            if (rt_time[i] < rt_time[i - 1]) { rt_time_sorted = false; break; }
    }

    /* Index variant — single eq key carrying a fresh CSR grouped index,
     * no null-marked right rows: every group is answered straight from its
     * index slice (ascending row ids).  Ascending TIME along the slice is
     * either implied by globally sorted right time, or verified per probed
     * slice with a bail-early scan — on a (sym,time)-sorted (parted)
     * layout the slice is one contiguous range, so the verify reads
     * sequential memory and costs a fraction of the binary searches it
     * unlocks.  NO pass over the right table at all — this is the
     * parted-layout-parity path: cost is per-probed-key, not per-row. */
    if (ok && n_eq == 1 && rt_no_nulls &&
        ray_index_kind(rt_eq[0]) == RAY_IDX_HASH) {
        ray_idx_consults[IDX_SITE_ASOF]++;
        bool served = true;
        ray_t* gsl_hdr = NULL;
        int64_t* gsl = (int64_t*)scratch_alloc(&gsl_hdr,
                           (size_t)(n_groups > 0 ? n_groups : 1) * 2 *
                           sizeof(int64_t));
        if (gsl) {
            /* Resolve every probed group's slice first; the per-slice
             * time-order verify runs over the collected set afterwards
             * so it can go wide.  The verify IS the honest-engine cost
             * that trusting a sorted-attribute stamp would skip — when a
             * query probes the
             * liquid names it walks millions of rows, so it must run at
             * memory speed: contiguous slices (the parted layout) are
             * checked with a sequential compare instead of a gathered
             * one, and the whole probe set is chunked across the pool
             * with a shared bail flag (chunks re-check their boundary
             * pair, so every adjacent pair is covered exactly once). */
            int64_t verify_rows = 0;
            for (int32_t g = 0; g < n_groups && served; g++) {
                const int64_t* grows = NULL;
                int64_t gn = 0;
                int hit = ray_index_hash_group(rt_eq[0], gkeys[g],
                                               &grows, &gn);
                if (hit < 0) { served = false; break; }  /* ineligible */
                gsl[2 * g]     = (int64_t)(intptr_t)grows;
                gsl[2 * g + 1] = gn;
                verify_rows += gn;
            }
            if (served && !rt_time_sorted && verify_rows > 0) {
                served = asof_verify_slices(gsl, n_groups, rt_time,
                                            verify_rows);
            }
            if (served) {
                ray_idx_hits[IDX_SITE_ASOF]++;
                for (int64_t li = 0; li < left_n; li++) {
                    int32_t g = left_gid[li];
                    if (g < 0) { match_orig[li] = -1; continue; }
                    const int64_t* grows =
                        (const int64_t*)(intptr_t)gsl[2 * g];
                    int64_t gn = gsl[2 * g + 1];
                    int64_t t = lt_time[li];
                    int64_t lo = 0, hi = gn, best = -1;
                    while (lo < hi) {
                        int64_t mid = lo + (hi - lo) / 2;
                        if (rt_time[grows[mid]] <= t) { best = mid; lo = mid + 1; }
                        else                          { hi = mid; }
                    }
                    match_orig[li] = (best >= 0) ? grows[best] : -1;
                }
                scratch_free(gsl_hdr);
                goto done;
            }
        }
        scratch_free(gsl_hdr);
        /* ineligible / OOM — fall through to the scan variants */
    }

    /* Multi-key index variant — n_eq >= 2 with a fresh CSR index on the
     * FIRST key (same sorted/no-null preconditions): walk only the probed
     * first keys' slices, resolving each slice row to its full
     * (k0,k1,...) group by comparing the remaining keys inline, and drive
     * the same per-group cursor merge the full-pass variant uses (a slice
     * is ascending row ids, hence ascending times — every group under one
     * first key is a subsequence of that one ordered walk).  Cost is
     * proportional to the PROBED slices; when those cover most of the
     * table anyway, the sequential full pass is cheaper per row, so bail
     * upfront on the (known!) slice-length sum. */
    if (ok && n_eq >= 2 && rt_no_nulls && n_groups > 0 &&
        ray_index_kind(rt_eq[0]) == RAY_IDX_HASH) {
        ray_idx_consults[IDX_SITE_ASOF]++;
        bool served = true;

        /* Distinct FIRST keys among the groups: fk_of[g] = distinct-fk
         * ordinal, chained via fkg_next[g] so one slice walk serves every
         * group sharing its first key.  Dedup through a small OA table. */
        uint64_t fcap = 16;
        while (fcap < (uint64_t)n_groups * 2u) fcap <<= 1;
        uint64_t fmask = fcap - 1;
        ray_t *ftab_hdr = NULL, *fhead_hdr = NULL, *fnext_hdr = NULL,
              *fsl_hdr = NULL;
        ray_t *lcnt_hdr = NULL, *ls_hdr = NULL, *cur_hdr = NULL,
              *prev_hdr = NULL;
        int32_t* ftab    = (int32_t*)scratch_alloc(&ftab_hdr, fcap * 4);
        int32_t* fk_head = (int32_t*)scratch_alloc(&fhead_hdr,
                                                   (size_t)n_groups * 4);
        int32_t* fkg_next = (int32_t*)scratch_alloc(&fnext_hdr,
                                                    (size_t)n_groups * 4);
        int64_t* fsl = (int64_t*)scratch_alloc(&fsl_hdr,
                                               (size_t)n_groups * 2 *
                                               sizeof(int64_t));
        int32_t* lcnt = (int32_t*)scratch_alloc(&lcnt_hdr,
                                                ((size_t)n_groups + 1) * 4);
        int32_t* cur  = (int32_t*)scratch_alloc(&cur_hdr,
                                                ((size_t)n_groups + 1) * 4);
        int32_t* prev = (int32_t*)scratch_alloc(&prev_hdr,
                                                ((size_t)n_groups + 1) * 4);
        asof_lslot_t* ls = (asof_lslot_t*)scratch_alloc(&ls_hdr,
                              (size_t)left_n * sizeof(asof_lslot_t));
        int32_t n_fk = 0;
        if (ftab && fk_head && fkg_next && fsl &&
            lcnt && cur && prev && ls) {
            memset(ftab, 0, fcap * 4);
            for (int32_t g = 0; g < n_groups; g++) {
                int64_t k0 = gkeys[(int64_t)g * n_eq];
                uint64_t slot = (uint64_t)ray_hash_i64(k0) & fmask;
                int32_t fid = -1;
                for (;;) {
                    int32_t fp1 = ftab[slot];
                    if (fp1 == 0) {
                        ftab[slot] = n_fk + 1;
                        fk_head[n_fk] = -1;
                        fid = n_fk++;
                        break;
                    }
                    int32_t rep = fk_head[fp1 - 1];
                    /* compare against any chained group's first key */
                    if (rep >= 0 &&
                        gkeys[(int64_t)rep * n_eq] == k0) { fid = fp1 - 1; break; }
                    if (rep < 0) {
                        /* empty chain can't happen after first insert below,
                         * but stay defensive: treat as fresh slot walk */
                    }
                    slot = (slot + 1) & fmask;
                }
                fkg_next[g] = fk_head[fid];
                fk_head[fid] = g;
            }

            /* Probe each distinct first key once; sum the slice lengths
             * for the density bail. */
            int64_t total_slice = 0;
            for (int32_t f = 0; f < n_fk && served; f++) {
                int32_t rep = fk_head[f];
                const int64_t* grows = NULL;
                int64_t gn = 0;
                int hit = ray_index_hash_group(rt_eq[0],
                                               gkeys[(int64_t)rep * n_eq],
                                               &grows, &gn);
                if (hit < 0) { served = false; break; }   /* ineligible */
                /* The cursor merge needs ascending time along the walk;
                 * globally sorted right time implies it, otherwise verify
                 * this slice (sequential reads on a parted layout). */
                if (!rt_time_sorted)
                    for (int64_t p = 1; p < gn; p++)
                        if (rt_time[grows[p]] < rt_time[grows[p - 1]]) {
                            served = false;
                            break;
                        }
                if (!served) break;
                fsl[2 * f]     = (int64_t)(intptr_t)grows;
                fsl[2 * f + 1] = gn;
                total_slice += gn;
            }
            /* Density bail: the sequential full pass reads every row once
             * with better locality than slice-driven scattered reads. */
            if (served && total_slice > right_n / 2) served = false;

            if (served) {
                ray_idx_hits[IDX_SITE_ASOF]++;
                /* Left cursor structures — identical to the full-pass
                 * cursor merge. */
                memset(lcnt, 0, ((size_t)n_groups + 1) * 4);
                for (int64_t li = 0; li < left_n; li++) {
                    if (left_gid[li] >= 0) lcnt[left_gid[li]]++;
                    else match_orig[li] = -1;
                }
                int32_t acc = 0;
                for (int32_t g = 0; g < n_groups; g++) {
                    int32_t c = lcnt[g];
                    lcnt[g] = acc;
                    cur[g] = acc;
                    acc += c;
                }
                lcnt[n_groups] = acc;
                for (int64_t li = 0; li < left_n; li++) {
                    int32_t g = left_gid[li];
                    if (g < 0) continue;
                    ls[cur[g]].t  = lt_time[li];
                    ls[cur[g]].li = (int32_t)li;
                    cur[g]++;
                }
                for (int32_t g = 0; g < n_groups; g++) {
                    int32_t sz = lcnt[g + 1] - lcnt[g];
                    if (sz > 1)
                        qsort(ls + lcnt[g], (size_t)sz,
                              sizeof(asof_lslot_t), asof_lslot_cmp);
                    cur[g]  = lcnt[g];
                    prev[g] = -1;
                }

                /* One ordered walk per probed first key; resolve each
                 * slice row to its full group by the remaining keys. */
                for (int32_t f = 0; f < n_fk; f++) {
                    const int64_t* grows =
                        (const int64_t*)(intptr_t)fsl[2 * f];
                    int64_t gn = fsl[2 * f + 1];
                    int32_t head = fk_head[f];
                    for (int64_t p = 0; p < gn; p++) {
                        int64_t i = grows[p];
                        int32_t gid = head;
                        while (gid >= 0) {
                            const int64_t* gk =
                                gkeys + (int64_t)gid * n_eq;
                            int eq = 1;
                            for (uint32_t k = 1; k < n_eq; k++) {
                                int64_t rv = read_col_i64(
                                    ray_data(rt_eq[k]), i,
                                    rt_eq[k]->type, rt_eq[k]->attrs);
                                if (gk[k] != rv) { eq = 0; break; }
                            }
                            if (eq) break;
                            gid = fkg_next[gid];
                        }
                        if (gid < 0) continue;
                        int64_t rt = rt_time[i];
                        while (cur[gid] < lcnt[gid + 1] &&
                               ls[cur[gid]].t < rt) {
                            match_orig[ls[cur[gid]].li] = prev[gid];
                            cur[gid]++;
                        }
                        prev[gid] = (int32_t)i;
                    }
                }
                for (int32_t g = 0; g < n_groups; g++)
                    while (cur[g] < lcnt[g + 1])
                        match_orig[ls[cur[g]++].li] = prev[g];

                scratch_free(ftab_hdr);  scratch_free(fhead_hdr);
                scratch_free(fnext_hdr); scratch_free(fsl_hdr);
                scratch_free(lcnt_hdr);  scratch_free(cur_hdr);
                scratch_free(prev_hdr);  scratch_free(ls_hdr);
                goto done;
            }
        }
        scratch_free(ftab_hdr);  scratch_free(fhead_hdr);
        scratch_free(fnext_hdr); scratch_free(fsl_hdr);
        scratch_free(lcnt_hdr);  scratch_free(cur_hdr);
        scratch_free(prev_hdr);  scratch_free(ls_hdr);
        /* ineligible / dense / OOM — fall through to the scan variants */
    }

    if (ok) {
        cnt = (int32_t*)scratch_alloc(&cnt_hdr, ((size_t)n_groups + 1) * 4);
        off = (int32_t*)scratch_alloc(&off_hdr, ((size_t)n_groups + 1) * 4);
        ok = cnt && off;
        if (ok) memset(cnt, 0, ((size_t)n_groups + 1) * 4);
    }

    /* First-key SYM direct index: gid by domain position, chained across
     * groups that share the first key.  Replaces the per-row hash+probe in
     * the right pass with one array load — the pass is a sequential scan
     * of the key column, so this is what its speed is made of. */
    if (ok && n_eq >= 1 && rt_eq[0]->type == RAY_SYM) {
        int64_t d = ray_sym_domain_count(ray_sym_vec_domain(rt_eq[0]));
        if (d > 0 && d <= (int64_t)1 << 22) {
            sym_lut  = (int32_t*)scratch_alloc(&lut_hdr, (size_t)d * 4);
            gid_next = (int32_t*)scratch_alloc(&gnx_hdr,
                           (size_t)(n_groups > 0 ? n_groups : 1) * 4);
            if (sym_lut && gid_next) {
                lut_d = d;
                memset(sym_lut, 0xFF, (size_t)d * 4);   /* all -1 */
                for (int32_t gid = 0; gid < n_groups; gid++) {
                    int64_t k0 = gkeys[(int64_t)gid * n_eq];
                    gid_next[gid] = -1;
                    /* -1 = absent cross-domain translation: matches nothing */
                    if (k0 < 0 || k0 >= d) continue;
                    gid_next[gid] = sym_lut[k0];
                    sym_lut[k0] = gid;
                }
            } else {
                scratch_free(lut_hdr);  lut_hdr = NULL;  sym_lut = NULL;
                scratch_free(gnx_hdr);  gnx_hdr = NULL;  gid_next = NULL;
            }
        }
    }

    /* Right-row group resolution, shared by both variants below: direct
     * LUT for a SYM first key, open-addressing probe otherwise. */
    #define ASOF_RESOLVE_GID(i, gid_out) do {                                  \
        int32_t _g = -1;                                                       \
        if (n_eq == 0) {                                                       \
            _g = 0;                                                            \
        } else if (sym_lut) {                                                  \
            int64_t _c0 = ray_read_sym(r0_base, (i), RAY_SYM, r0_attrs);       \
            _g = (_c0 >= 0 && _c0 < lut_d) ? sym_lut[_c0] : -1;                \
            while (_g >= 0) {                                                  \
                const int64_t* _gk = gkeys + (int64_t)_g * n_eq;               \
                int _eq = 1;                                                   \
                for (uint32_t _k = 1; _k < n_eq; _k++) {                        \
                    int64_t _rv = read_col_i64(ray_data(rt_eq[_k]), (i),       \
                                               rt_eq[_k]->type,                \
                                               rt_eq[_k]->attrs);              \
                    if (_gk[_k] != _rv) { _eq = 0; break; }                    \
                }                                                              \
                if (_eq) break;                                                \
                _g = gid_next[_g];                                             \
            }                                                                  \
        } else {                                                               \
            /* rowkey: the same per-row scratch buffer Pass 1 uses above,     \
             * carved once at function entry, sized n_eq — no per-row alloc. \
             */                                                                \
            uint64_t _h = 0;                                                   \
            for (uint32_t _k = 0; _k < n_eq; _k++) {                            \
                rowkey[_k] = read_col_i64(ray_data(rt_eq[_k]), (i),            \
                                       rt_eq[_k]->type, rt_eq[_k]->attrs);     \
                uint64_t _kh = ray_hash_i64(rowkey[_k]);                       \
                _h = (_k == 0) ? _kh : ray_hash_combine(_h, _kh);              \
            }                                                                  \
            uint64_t _slot = _h & mask;                                        \
            for (;;) {                                                         \
                int32_t _gp1 = tab[_slot];                                     \
                if (_gp1 == 0) break;                                          \
                const int64_t* _gk = gkeys + (int64_t)(_gp1 - 1) * n_eq;       \
                int _eq = 1;                                                   \
                for (uint32_t _k = 0; _k < n_eq; _k++)                          \
                    if (_gk[_k] != rowkey[_k]) { _eq = 0; break; }             \
                if (_eq) { _g = _gp1 - 1; break; }                             \
                _slot = (_slot + 1) & mask;                                    \
            }                                                                  \
        }                                                                      \
        (gid_out) = _g;                                                        \
    } while (0)

    /* Hoisted first-key access for ASOF_RESOLVE_GID (loop-invariant). */
    const uint8_t* r0_base  = (n_eq > 0) ? (const uint8_t*)ray_data(rt_eq[0]) : NULL;
    uint8_t        r0_attrs = (n_eq > 0) ? rt_eq[0]->attrs : 0;
    (void)r0_base; (void)r0_attrs;

    /* Cursor-merge variant — right time GLOBALLY sorted (verified attr):
     * sort the left rows of each group by time, then a single right pass
     * advances per-group cursors, assigning each left row the previous
     * right row of its group once right time passes it.  No row-list
     * collection, no per-group monotone sweep, no binary searches. */
    if (ok && rt_time_sorted) {
        ray_t *lcnt_hdr = NULL, *ls_hdr = NULL, *cur_hdr = NULL,
              *prev_hdr = NULL;
        int32_t* lcnt = (int32_t*)scratch_alloc(&lcnt_hdr,
                                                ((size_t)n_groups + 1) * 4);
        int32_t* cur  = (int32_t*)scratch_alloc(&cur_hdr,
                                                ((size_t)n_groups + 1) * 4);
        int32_t* prev = (int32_t*)scratch_alloc(&prev_hdr,
                                                ((size_t)n_groups + 1) * 4);
        asof_lslot_t* ls = (asof_lslot_t*)scratch_alloc(&ls_hdr,
                              (size_t)left_n * sizeof(asof_lslot_t));
        if (lcnt && cur && prev && ls) {
            memset(lcnt, 0, ((size_t)n_groups + 1) * 4);
            for (int64_t li = 0; li < left_n; li++) {
                if (left_gid[li] >= 0) lcnt[left_gid[li]]++;
                else match_orig[li] = -1;
            }
            /* loff lives in lcnt after the prefix pass; cur[] is the fill
             * cursor and then the merge cursor. */
            int32_t acc = 0;
            for (int32_t g = 0; g < n_groups; g++) {
                int32_t c = lcnt[g];
                lcnt[g] = acc;
                cur[g] = acc;
                acc += c;
            }
            lcnt[n_groups] = acc;
            for (int64_t li = 0; li < left_n; li++) {
                int32_t g = left_gid[li];
                if (g < 0) continue;
                ls[cur[g]].t  = lt_time[li];
                ls[cur[g]].li = (int32_t)li;
                cur[g]++;
            }
            for (int32_t g = 0; g < n_groups; g++) {
                int32_t sz = lcnt[g + 1] - lcnt[g];
                if (sz > 1)
                    qsort(ls + lcnt[g], (size_t)sz, sizeof(asof_lslot_t),
                          asof_lslot_cmp);
                cur[g]  = lcnt[g];
                prev[g] = -1;
            }

            for (int64_t i = 0; i < right_n; i++) {
                if (rt_null[i]) continue;
                int32_t gid;
                ASOF_RESOLVE_GID(i, gid);
                if (gid < 0) continue;
                int64_t rt = rt_time[i];
                while (cur[gid] < lcnt[gid + 1] && ls[cur[gid]].t < rt) {
                    match_orig[ls[cur[gid]].li] = prev[gid];
                    cur[gid]++;
                }
                prev[gid] = (int32_t)i;
            }
            for (int32_t g = 0; g < n_groups; g++)
                while (cur[g] < lcnt[g + 1])
                    match_orig[ls[cur[g]++].li] = prev[g];

            scratch_free(lcnt_hdr);
            scratch_free(cur_hdr);
            scratch_free(prev_hdr);
            scratch_free(ls_hdr);
            goto done;
        }
        /* scratch OOM — fall through to the collect variant */
        scratch_free(lcnt_hdr);
        scratch_free(cur_hdr);
        scratch_free(prev_hdr);
        scratch_free(ls_hdr);
    }

    /* Pass 2 — one sequential right pass: rows whose key tuple matches a
     * probed group are counted; everything else is a miss.  n_eq==0
     * admits every non-null row into the single group. */
    int64_t n_matched = 0;
    if (ok && n_eq == 0) {
        for (int64_t i = 0; i < right_n; i++) {
            if (rt_null[i]) { row_gid[i] = -1; continue; }
            row_gid[i] = 0;
            cnt[0]++; n_matched++;
        }
    } else if (ok && sym_lut) {
        const uint8_t* base0  = (const uint8_t*)ray_data(rt_eq[0]);
        uint8_t        attrs0 = rt_eq[0]->attrs;
        for (int64_t i = 0; i < right_n; i++) {
            if (rt_null[i]) { row_gid[i] = -1; continue; }
            int64_t c0 = ray_read_sym(base0, i, RAY_SYM, attrs0);
            int32_t gid = (c0 >= 0 && c0 < lut_d) ? sym_lut[c0] : -1;
            /* Chain across groups sharing the first key; compare the rest. */
            while (gid >= 0) {
                const int64_t* gk = gkeys + (int64_t)gid * n_eq;
                int eq = 1;
                for (uint32_t k = 1; k < n_eq; k++) {
                    int64_t rv = read_col_i64(ray_data(rt_eq[k]), i,
                                              rt_eq[k]->type, rt_eq[k]->attrs);
                    if (gk[k] != rv) { eq = 0; break; }
                }
                if (eq) break;
                gid = gid_next[gid];
            }
            row_gid[i] = gid;
            if (gid >= 0) { cnt[gid]++; n_matched++; }
        }
    } else if (ok) {
        /* rowkey: same carve as Pass 1 (function-entry, sized n_eq) — Pass 1
         * over left rows has already finished by the time this right-row
         * pass runs, so reusing it here adds no aliasing. */
        for (int64_t i = 0; i < right_n; i++) {
            if (rt_null[i]) { row_gid[i] = -1; continue; }
            int32_t gid = -1;
            uint64_t h = 0;
            for (uint32_t k = 0; k < n_eq; k++) {
                rowkey[k] = read_col_i64(ray_data(rt_eq[k]), i,
                                     rt_eq[k]->type, rt_eq[k]->attrs);
                uint64_t kh = ray_hash_i64(rowkey[k]);
                h = (k == 0) ? kh : ray_hash_combine(h, kh);
            }
            uint64_t slot = h & mask;
            for (;;) {
                int32_t gp1 = tab[slot];
                if (gp1 == 0) break;   /* key not probed by any left row */
                const int64_t* gk = gkeys + (int64_t)(gp1 - 1) * n_eq;
                int eq = 1;
                for (uint32_t k = 0; k < n_eq; k++)
                    if (gk[k] != rowkey[k]) { eq = 0; break; }
                if (eq) { gid = gp1 - 1; break; }
                slot = (slot + 1) & mask;
            }
            row_gid[i] = gid;
            if (gid >= 0) { cnt[gid]++; n_matched++; }
        }
    }

    if (ok) {
        rows = (int32_t*)scratch_alloc(&rows_hdr,
                                       (size_t)(n_matched > 0 ? n_matched : 1) * 4);
        ok = rows != NULL;
    }

    if (ok) {
        /* Prefix sums -> group slices; fill rows[] in ascending row order
         * (reusing cnt[] as the per-group fill cursor). */
        int32_t acc = 0;
        for (int32_t gid = 0; gid < n_groups; gid++) {
            off[gid] = acc;
            acc += cnt[gid];
            cnt[gid] = off[gid];
        }
        off[n_groups] = acc;
        for (int64_t i = 0; i < right_n; i++) {
            int32_t gid = row_gid[i];
            if (gid >= 0) rows[cnt[gid]++] = (int32_t)i;
        }

        /* Verify right time is non-descending within every probed group —
         * the binary search below is only valid then. */
        for (int32_t gid = 0; ok && gid < n_groups; gid++) {
            for (int32_t p = off[gid] + 1; p < off[gid + 1]; p++)
                if (rt_time[rows[p]] < rt_time[rows[p - 1]]) { ok = false; break; }
        }
    }

    if (ok) {
        /* Answer each left row: last row of its group with time <= t. */
        for (int64_t li = 0; li < left_n; li++) {
            int32_t gid = left_gid[li];
            if (gid < 0) { match_orig[li] = -1; continue; }
            int64_t t = lt_time[li];
            int32_t lo = off[gid], hi = off[gid + 1], best = -1;
            while (lo < hi) {
                int32_t mid = lo + (hi - lo) / 2;
                if (rt_time[rows[mid]] <= t) { best = mid; lo = mid + 1; }
                else                         { hi = mid; }
            }
            match_orig[li] = (best >= 0) ? (int64_t)rows[best] : -1;
        }
    }

done:
    #undef ASOF_RESOLVE_GID
    scratch_free(tab_hdr);
    scratch_free(keys_hdr);
    scratch_free(lgid_hdr);
    scratch_free(gid_hdr);
    scratch_free(cnt_hdr);
    scratch_free(off_hdr);
    scratch_free(rows_hdr);
    scratch_free(lut_hdr);
    scratch_free(gnx_hdr);
    scratch_free(rowkey_hdr);
    return ok;
}

ray_t* exec_window_join(ray_graph_t* g, ray_op_t* op,
                               ray_t* left_table, ray_t* right_table) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    uint32_t n_eq     = ext->asof.n_eq_keys;
    uint8_t join_type = ext->asof.join_type;

    int64_t left_n  = ray_table_nrows(left_table);
    int64_t right_n = ray_table_nrows(right_table);

    /* Resolve time key */
    ray_op_ext_t* time_ext = find_ext(g, ext->asof.time_key);
    if (!time_ext || time_ext->base.opcode != OP_SCAN)
        return ray_error("nyi", NULL);
    int64_t time_sym = time_ext->sym;

    /* Resolve equality keys.
     * eq_syms/lt_eq/rt_eq/eq_xlut/eq_xn/xcnt: one exact-size carve, all
     * 8-byte elements (int64 or pointer) so one n_eq-wide (?: 1) block —
     * naturally 8-byte-aligned — covers all six arrays by offset.  This
     * replaces six fixed [256] arrays that were sized to query.c's
     * uint8_t eq-count narrowing (removed in this same commit); n_eq is
     * unbounded from here down.  Freed on every return below. */
    ray_t* eqbuf_hdr = NULL;
    size_t eq_cap = (size_t)(n_eq ? n_eq : 1);
    int64_t* eq_syms = (int64_t*)scratch_alloc(&eqbuf_hdr, eq_cap * 6 * sizeof(int64_t));
    if (!eq_syms) return ray_error("oom", NULL);
    ray_t**          lt_eq   = (ray_t**)(eq_syms + eq_cap);
    ray_t**          rt_eq   = (ray_t**)(lt_eq + eq_cap);
    const int64_t** eq_xlut  = (const int64_t**)(rt_eq + eq_cap);
    int64_t*         eq_xn   = (int64_t*)(eq_xlut + eq_cap);
    int64_t*         xcnt    = eq_xn + eq_cap;
    for (uint32_t k = 0; k < n_eq; k++) {
        ray_op_ext_t* ek = find_ext(g, ext->asof.eq_keys[k]);
        if (!ek || ek->base.opcode != OP_SCAN) {
            scratch_free(eqbuf_hdr);
            return ray_error("nyi", NULL);
        }
        eq_syms[k] = ek->sym;
    }

    /* Get time vectors — use int64 representation for comparison.
     * TIME uses 4-byte i32 (ms), TIMESTAMP uses 8-byte i64 (ns).
     * We expand to a temporary i64 array for uniform comparison. */
    ray_t* lt_time_vec = ray_table_get_col(left_table, time_sym);
    ray_t* rt_time_vec = ray_table_get_col(right_table, time_sym);
    if (!lt_time_vec || !rt_time_vec) {
        scratch_free(eqbuf_hdr);
        return ray_error("schema", NULL);
    }
    int8_t time_type = lt_time_vec->type;

    /* Helper macro to read time value as int64_t regardless of storage type */
    #define READ_TIME(vec, idx) \
        ((time_type == RAY_TIME || time_type == RAY_DATE) \
            ? (int64_t)((int32_t*)ray_data(vec))[(idx)] \
            : ((int64_t*)ray_data(vec))[(idx)])

    /* Time values as int64 for uniform comparison.  8-byte storage
     * (TIMESTAMP / I64) aliases the column data directly; 4-byte storage
     * (TIME / DATE) widens into a scratch copy. */
    bool time_is_i64 = !(time_type == RAY_TIME || time_type == RAY_DATE);
    ray_t* lt_time_hdr = NULL, *rt_time_hdr = NULL;
    int64_t* lt_time;
    int64_t* rt_time;
    if (time_is_i64) {
        lt_time = (int64_t*)ray_data(lt_time_vec);
        rt_time = (int64_t*)ray_data(rt_time_vec);
    } else {
        lt_time = (int64_t*)scratch_alloc(&lt_time_hdr, (size_t)left_n * sizeof(int64_t));
        rt_time = (int64_t*)scratch_alloc(&rt_time_hdr, (size_t)right_n * sizeof(int64_t));
        if ((!lt_time && left_n > 0) || (!rt_time && right_n > 0)) {
            if (lt_time_hdr) scratch_free(lt_time_hdr);
            if (rt_time_hdr) scratch_free(rt_time_hdr);
            scratch_free(eqbuf_hdr);
            return ray_error("oom", NULL);
        }
        for (int64_t i = 0; i < left_n; i++) lt_time[i] = READ_TIME(lt_time_vec, i);
        for (int64_t i = 0; i < right_n; i++) rt_time[i] = READ_TIME(rt_time_vec, i);
    }
    #undef READ_TIME

    /* Get eq key vectors — stored as ray_t* for type-safe access.
     * lt_eq/rt_eq point into the eqbuf carve above (no separate alloc). */
    for (uint32_t k = 0; k < n_eq; k++) {
        ray_t* lv = ray_table_get_col(left_table, eq_syms[k]);
        ray_t* rv = ray_table_get_col(right_table, eq_syms[k]);
        if (!lv || !rv) {
            if (lt_time_hdr) scratch_free(lt_time_hdr);
            if (rt_time_hdr) scratch_free(rt_time_hdr);
            scratch_free(eqbuf_hdr);
            return ray_error("schema", NULL);
        }
        lt_eq[k] = lv;
        rt_eq[k] = rv;
    }

    /* Cross-domain SYM eq keys: per-key LEFT→RIGHT position translation
     * tables, built sequentially HERE — one pass over the LEFT
     * vocabulary (never the rows) replacing the old per-read str+find
     * hash lookups in asof_eq_lread.  Pre-flip both sides are the
     * runtime singleton: no table is built, the reader's raw fast path
     * is byte-identical.  eq_xlut/eq_xn/xcnt point into the eqbuf carve
     * above (no separate alloc). */
    ray_t* eq_xl_hdr = NULL;
    {
        size_t  xtotal = 0;
        for (uint32_t k = 0; k < n_eq; k++) {
            eq_xlut[k] = NULL;
            eq_xn[k]   = 0;
            xcnt[k]    = -1; /* -1 = same domain / non-SYM: raw reads */
            if (lt_eq[k]->type == RAY_SYM && rt_eq[k]->type == RAY_SYM) {
                struct ray_sym_domain_s* ld = ray_sym_vec_domain(lt_eq[k]);
                struct ray_sym_domain_s* rd = ray_sym_vec_domain(rt_eq[k]);
                if (ld != rd) {
                    xcnt[k] = ray_sym_domain_count(ld);
                    xtotal += (size_t)xcnt[k];
                }
            }
        }
        int64_t* blk = NULL;
        if (xtotal > 0) {
            blk = (int64_t*)scratch_alloc(&eq_xl_hdr, xtotal * sizeof(int64_t));
            if (!blk) {
                if (lt_time_hdr) scratch_free(lt_time_hdr);
                if (rt_time_hdr) scratch_free(rt_time_hdr);
                scratch_free(eqbuf_hdr);
                return ray_error("oom", NULL);
            }
        }
        size_t xoff = 0;
        for (uint32_t k = 0; k < n_eq; k++) {
            if (xcnt[k] < 0) continue;       /* raw reads */
            if (xcnt[k] == 0) {              /* empty left vocabulary */
                eq_xlut[k] = g_asof_empty_xlut;
                continue;
            }
            struct ray_sym_domain_s* ld = ray_sym_vec_domain(lt_eq[k]);
            struct ray_sym_domain_s* rd = ray_sym_vec_domain(rt_eq[k]);
            int64_t* lut = blk + xoff;
            xoff += (size_t)xcnt[k];
            for (int64_t p = 0; p < xcnt[k]; p++) {
                ray_t* s = ray_sym_domain_str(ld, p);
                lut[p] = s ? ray_sym_domain_find(rd, ray_str_ptr(s),
                                                 ray_str_len(s))
                           : -1;
            }
            eq_xlut[k] = lut;
            eq_xn[k]   = xcnt[k];
        }
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
        if (eq_xl_hdr) scratch_free(eq_xl_hdr);
        scratch_free(eqbuf_hdr);
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
    for (uint32_t k = 0; k < n_eq; k++) {
        if (lt_eq[k]->attrs & RAY_ATTR_HAS_NULLS)
            for (int64_t i = 0; i < left_n; i++)
                if (ray_vec_is_null(lt_eq[k], i)) lt_null[i] = 1;
        if (rt_eq[k]->attrs & RAY_ATTR_HAS_NULLS)
            for (int64_t i = 0; i < right_n; i++)
                if (ray_vec_is_null(rt_eq[k], i)) rt_null[i] = 1;
    }

    /* match_orig[li] = best right row for ORIGINAL left row li.  Allocated
     * up front: the hash-group strategy fills it directly; the sort-merge
     * fallback fills it through its sorted-order remap. */
    ray_t* mo_hdr = NULL;
    int64_t* match_orig = left_n > 0
        ? (int64_t*)scratch_alloc(&mo_hdr, (size_t)left_n * sizeof(int64_t))
        : NULL;
    if (!match_orig && left_n > 0) {
        if (lt_null_hdr) scratch_free(lt_null_hdr);
        if (rt_null_hdr) scratch_free(rt_null_hdr);
        if (lt_time_hdr) scratch_free(lt_time_hdr);
        if (rt_time_hdr) scratch_free(rt_time_hdr);
        if (eq_xl_hdr) scratch_free(eq_xl_hdr);
        scratch_free(eqbuf_hdr);
        return ray_error("oom", NULL);
    }

    /* Slow-path scratch, hoisted above the fast-path skip so the shared
     * cleanup below stays valid on both routes. */
    ray_t* li_hdr = NULL, *ri_hdr = NULL, *match_hdr = NULL;
    int64_t* li_idx = NULL;
    int64_t* ri_idx = NULL;
    int64_t* match  = NULL;

    /* Primary strategy: hash-group the right side + per-left binary search
     * (no sorting of either side; a fresh CSR grouped index on a single eq
     * key answers groups with no right pass at all).  Falls back to the
     * sort-merge when the per-group right-time monotonicity precondition
     * fails. */
    bool rt_no_nulls = !(rt_time_vec->attrs & RAY_ATTR_HAS_NULLS);
    for (uint32_t k = 0; k < n_eq && rt_no_nulls; k++)
        if (rt_eq[k]->attrs & RAY_ATTR_HAS_NULLS) rt_no_nulls = false;
    if (asof_hash_group_match(n_eq, lt_eq, rt_eq, eq_xlut, eq_xn,
                              lt_time, rt_time, lt_null, rt_null,
                              left_n, right_n,
                              ray_attr_is_sorted(rt_time_vec), rt_no_nulls,
                              match_orig))
        goto build_output;

    /* Sort both tables by (eq_keys, time_key) using index arrays.  Rows
     * with any null key sort LAST (NULLS LAST) so the merge walk reaches
     * them once all real candidates are consumed and can skip them
     * cheaply. */
    li_idx = (int64_t*)scratch_alloc(&li_hdr, (size_t)left_n * sizeof(int64_t));
    ri_idx = (int64_t*)scratch_alloc(&ri_hdr, (size_t)right_n * sizeof(int64_t));
    if ((!li_idx && left_n > 0) || (!ri_idx && right_n > 0)) {
        if (li_hdr) scratch_free(li_hdr);
        if (ri_hdr) scratch_free(ri_hdr);
        if (mo_hdr) scratch_free(mo_hdr);
        if (lt_null_hdr) scratch_free(lt_null_hdr);
        if (rt_null_hdr) scratch_free(rt_null_hdr);
        if (lt_time_hdr) scratch_free(lt_time_hdr);
        if (rt_time_hdr) scratch_free(rt_time_hdr);
        if (eq_xl_hdr) scratch_free(eq_xl_hdr);
        scratch_free(eqbuf_hdr);
        return ray_error("oom", NULL);
    }
    for (int64_t i = 0; i < left_n; i++) li_idx[i] = i;
    for (int64_t i = 0; i < right_n; i++) ri_idx[i] = i;

    /* asof fast-path: a side can skip its mergesort when its rows already sit
     * in the (eq-key, time) order the merge expects.  Two cases:
     *   - un-partitioned (n_eq==0): the time column carries the verified
     *     `sorted` marker (non-descending, no nulls), so identity order == sorted.
     *   - partitioned (n_eq==1): the single equality key is `parted` (rows
     *     grouped into contiguous, ascending value-blocks) AND the time column
     *     has no nulls AND is non-descending within each part — so identity
     *     order already equals (key, time) order.
     * Either way we fill the index array with the identity permutation and skip
     * the O(n log n) sort.  Falls back to sort-merge otherwise; never wrong. */
    /* Cross-domain SYM eq key?  The left side then sorts/merges in the
     * RIGHT side's id space (asof_eq_lread), so left identity order in
     * its OWN id space is not usable — disable the left presort shortcut.
     * Right-side reads stay raw: its shortcut is unaffected. */
    bool eq_xdomain = false;
    for (uint32_t k = 0; k < n_eq; k++)
        if (eq_xlut[k]) eq_xdomain = true;

    bool l_presorted = !eq_xdomain
        && ((n_eq == 0 && ray_attr_is_sorted(lt_time_vec))
        || (n_eq == 1
            && !(lt_time_vec->attrs & RAY_ATTR_HAS_NULLS)
            && ray_index_kind(lt_eq[0]) == RAY_IDX_PART
            /* reject a stale part index (parent length changed since build) */
            && ray_index_payload(lt_eq[0]->index)->built_for_len == lt_eq[0]->len
            && asof_time_sorted_within_parts(lt_eq[0], lt_time)));
    bool r_presorted = (n_eq == 0 && ray_attr_is_sorted(rt_time_vec))
        || (n_eq == 1
            && !(rt_time_vec->attrs & RAY_ATTR_HAS_NULLS)
            && ray_index_kind(rt_eq[0]) == RAY_IDX_PART
            /* reject a stale part index (parent length changed since build) */
            && ray_index_payload(rt_eq[0]->index)->built_for_len == rt_eq[0]->len
            && asof_time_sorted_within_parts(rt_eq[0], rt_time));

    /* Bottom-up mergesort on index arrays — O(N log N) */
    {
        int64_t max_n = left_n > right_n ? left_n : right_n;
        bool need_tmp = (!l_presorted || !r_presorted) && max_n > 0;
        ray_t* tmp_hdr = NULL;
        int64_t* tmp = need_tmp
            ? (int64_t*)scratch_alloc(&tmp_hdr, (size_t)max_n * sizeof(int64_t))
            : NULL;
        if (!tmp && need_tmp) {
            scratch_free(li_hdr); scratch_free(ri_hdr);
            if (mo_hdr) scratch_free(mo_hdr);
            if (lt_null_hdr) scratch_free(lt_null_hdr);
            if (rt_null_hdr) scratch_free(rt_null_hdr);
            if (lt_time_hdr) scratch_free(lt_time_hdr);
            if (rt_time_hdr) scratch_free(rt_time_hdr);
            if (eq_xl_hdr) scratch_free(eq_xl_hdr);
            scratch_free(eqbuf_hdr);
            return ray_error("oom", NULL);
        }

        /* Sort left indices by (nulls-last, eq_keys, time) */
        if (!l_presorted) {
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
                    for (uint32_t k2 = 0; k2 < n_eq && cmp == 0; k2++) {
                        /* asof_eq_lread: left sorts in the RIGHT side's
                         * id space when a SYM key pair spans domains. */
                        int64_t va = asof_eq_lread(lt_eq[k2], eq_xlut[k2], eq_xn[k2], ai);
                        int64_t vb = asof_eq_lread(lt_eq[k2], eq_xlut[k2], eq_xn[k2], bi);
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
        }

        /* Sort right indices by (nulls-last, eq_keys, time) */
        if (!r_presorted) {
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
                    for (uint32_t k2 = 0; k2 < n_eq && cmp == 0; k2++) {
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
        }

        if (tmp_hdr) scratch_free(tmp_hdr);
    }

    /* Build match array: for each left row (sorted), find best right match */
    match = (int64_t*)scratch_alloc(&match_hdr, (size_t)left_n * sizeof(int64_t));
    if (!match && left_n > 0) {
        scratch_free(li_hdr); scratch_free(ri_hdr);
        if (mo_hdr) scratch_free(mo_hdr);
        if (lt_null_hdr) scratch_free(lt_null_hdr);
        if (rt_null_hdr) scratch_free(rt_null_hdr);
        if (lt_time_hdr) scratch_free(lt_time_hdr);
        if (rt_time_hdr) scratch_free(rt_time_hdr);
        if (eq_xl_hdr) scratch_free(eq_xl_hdr);
        scratch_free(eqbuf_hdr);
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

        /* Detect partition change — reset best match and rewind rp.
         * Within-column equality of two LEFT rows: raw ids compare
         * correctly in any single domain — no translation needed. */
        if (prev_non_null_li >= 0) {
            int changed = 0;
            for (uint32_t k = 0; k < n_eq; k++) {
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
                    for (uint32_t k = 0; k < n_eq; k++) {
                        int64_t rv = read_col_i64(ray_data(rt_eq[k]), ri_prev, rt_eq[k]->type, rt_eq[k]->attrs);
                        int64_t lv = asof_eq_lread(lt_eq[k], eq_xlut[k], eq_xn[k], li);
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
            for (uint32_t k = 0; k < n_eq && eq_cmp == 0; k++) {
                int64_t rv = read_col_i64(ray_data(rt_eq[k]), ri, rt_eq[k]->type, rt_eq[k]->attrs);
                int64_t lv = asof_eq_lread(lt_eq[k], eq_xlut[k], eq_xn[k], li);
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
    for (int64_t lp = 0; lp < left_n; lp++)
        match_orig[li_idx[lp]] = match[lp];

build_output:;
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
    ray_t* right_out_idx_hdr = NULL;
    int64_t* right_out_idx = right_ncols > 0
        ? (int64_t*)scratch_alloc(&right_out_idx_hdr, (size_t)right_ncols * sizeof(int64_t))
        : NULL;
    if (right_ncols > 0 && !right_out_idx) {
        scratch_free(mo_hdr);
        scratch_free(match_hdr);
        scratch_free(li_hdr);
        scratch_free(ri_hdr);
        if (lt_null_hdr) scratch_free(lt_null_hdr);
        if (rt_null_hdr) scratch_free(rt_null_hdr);
        if (lt_time_hdr) scratch_free(lt_time_hdr);
        if (rt_time_hdr) scratch_free(rt_time_hdr);
        if (eq_xl_hdr) scratch_free(eq_xl_hdr);
        scratch_free(eqbuf_hdr);
        return ray_error("oom", NULL);
    }
    int64_t right_out_count = 0;
    for (int64_t c = 0; c < right_ncols; c++) {
        int64_t rname = ray_table_col_name(right_table, c);
        int skip = 0;
        if (rname == time_sym) skip = 1;
        for (uint32_t k = 0; k < n_eq && !skip; k++)
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
        if (right_out_idx_hdr) scratch_free(right_out_idx_hdr);
        scratch_free(mo_hdr);
        scratch_free(match_hdr);
        scratch_free(li_hdr);
        scratch_free(ri_hdr);
        if (lt_null_hdr) scratch_free(lt_null_hdr);
        if (rt_null_hdr) scratch_free(rt_null_hdr);
        if (lt_time_hdr) scratch_free(lt_time_hdr);
        if (rt_time_hdr) scratch_free(rt_time_hdr);
        if (eq_xl_hdr) scratch_free(eq_xl_hdr);
        scratch_free(eqbuf_hdr);
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
        /* SYM is adaptive-width (W8/W16/W32/W64): a splayed/loaded source is
         * typically W32 while ray_vec_new(RAY_SYM,…) yields a W64 dst.  Copy at
         * the SOURCE element size into a dst of the SAME width — otherwise the
         * ids are read at the wrong stride and decode to the empty symbol. */
        uint8_t esz = (ctype == RAY_SYM) ? col_esz(src_col) : ray_type_sizes[ctype];
        ray_t* dst_col = (ctype == RAY_SYM)
            ? ray_sym_vec_new((uint8_t)(src_col->attrs & RAY_SYM_W_MASK), out_n)
            : ray_vec_new(ctype, out_n);
        char* src = (char*)ray_data(src_col);
        char* dst = (char*)ray_data(dst_col);
        for (int64_t wi = 0; wi < out_n; wi++)
            memcpy(dst + wi * esz, src + lidx[wi] * esz, esz);
        dst_col->len = out_n;
        col_propagate_str_pool(dst_col, src_col);
        col_propagate_nulls_gather(dst_col, src_col, lidx, out_n);
        /* SYM output raw-copies cell ids from one source column —
         * adopt its domain (sym-domain Phase 2). */
        if (dst_col->type == RAY_SYM)
            ray_sym_vec_adopt_domain(dst_col, src_col);
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
        /* SYM adaptive-width: match the source width (see left-gather note). */
        uint8_t esz = (ctype == RAY_SYM) ? col_esz(src_col) : ray_type_sizes[ctype];
        ray_t* dst_col = (ctype == RAY_SYM)
            ? ray_sym_vec_new((uint8_t)(src_col->attrs & RAY_SYM_W_MASK), out_n)
            : ray_vec_new(ctype, out_n);
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
        if (dst_col->type == RAY_SYM)
            ray_sym_vec_adopt_domain(dst_col, src_col);
        out = ray_table_add_col(out, col_name, dst_col);
        ray_release(dst_col);
    }

    if (lidx_hdr) scratch_free(lidx_hdr);
    if (ridx_hdr) scratch_free(ridx_hdr);
    if (right_out_idx_hdr) scratch_free(right_out_idx_hdr);
    scratch_free(mo_hdr);
    scratch_free(match_hdr);
    scratch_free(li_hdr);
    scratch_free(ri_hdr);
    if (lt_null_hdr) scratch_free(lt_null_hdr);
    if (rt_null_hdr) scratch_free(rt_null_hdr);
    if (lt_time_hdr) scratch_free(lt_time_hdr);
    if (rt_time_hdr) scratch_free(rt_time_hdr);
    if (eq_xl_hdr) scratch_free(eq_xl_hdr);
    scratch_free(eqbuf_hdr);
    return out;
}
