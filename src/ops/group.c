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
#include "lang/internal.h"  /* for ray_median_dbl_inplace */

/* ============================================================================
 * Reduction execution
 * ============================================================================ */

typedef struct {
    double sum_f, min_f, max_f, prod_f, first_f, last_f, sum_sq_f;
    int64_t sum_i, min_i, max_i, prod_i, first_i, last_i, sum_sq_i;
    int64_t cnt;
    int64_t null_count;
    bool has_first;
} reduce_acc_t;

static void reduce_acc_init(reduce_acc_t* acc) {
    acc->sum_f = 0; acc->min_f = DBL_MAX; acc->max_f = -DBL_MAX;
    acc->prod_f = 1.0; acc->first_f = 0; acc->last_f = 0; acc->sum_sq_f = 0;
    acc->sum_i = 0; acc->min_i = INT64_MAX; acc->max_i = INT64_MIN;
    acc->prod_i = 1; acc->first_i = 0; acc->last_i = 0; acc->sum_sq_i = 0;
    acc->cnt = 0; acc->null_count = 0; acc->has_first = false;
}

/* Integer reduction loop — reads native type T, accumulates as i64.
 * HAS_NULLS and HAS_IDX must be integer literal constants (0 or 1) so the
 * compiler dead-code-eliminates the corresponding branches in every
 * specialisation.  reduce_range dispatches to the right combination before
 * calling this macro so the hot path (no nulls, no idx) contains zero
 * per-element runtime branches. */
#define REDUCE_LOOP_I(T, base, start, end, acc, HAS_NULLS, null_bm, HAS_IDX, idx) \
    do { \
        const T* d = (const T*)(base); \
        for (int64_t i = start; i < end; i++) { \
            int64_t row = (HAS_IDX) ? (idx)[i] : i; \
            if ((HAS_NULLS) && (null_bm[row/8] >> (row%8)) & 1) { (acc)->null_count++; continue; } \
            int64_t v = (int64_t)d[row]; \
            /* sum/sum_sq may overflow on signed arithmetic — use defined \
             * unsigned wrap (same semantic, no UBSan whine). */ \
            (acc)->sum_i    = (int64_t)((uint64_t)(acc)->sum_i    + (uint64_t)v); \
            (acc)->sum_sq_i = (int64_t)((uint64_t)(acc)->sum_sq_i + (uint64_t)v * (uint64_t)v); \
            (acc)->prod_i   = (int64_t)((uint64_t)(acc)->prod_i   * (uint64_t)v); \
            if (v < (acc)->min_i) (acc)->min_i = v; \
            if (v > (acc)->max_i) (acc)->max_i = v; \
            if (!(acc)->has_first) { (acc)->first_i = v; (acc)->has_first = true; } \
            (acc)->last_i = v; (acc)->cnt++; \
        } \
    } while (0)

/* Float reduction loop — see REDUCE_LOOP_I for HAS_NULLS/HAS_IDX semantics. */
#define REDUCE_LOOP_F(base, start, end, acc, HAS_NULLS, null_bm, HAS_IDX, idx) \
    do { \
        const double* d = (const double*)(base); \
        for (int64_t i = start; i < end; i++) { \
            int64_t row = (HAS_IDX) ? (idx)[i] : i; \
            if ((HAS_NULLS) && (null_bm[row/8] >> (row%8)) & 1) { (acc)->null_count++; continue; } \
            double v = d[row]; \
            (acc)->sum_f += v; (acc)->sum_sq_f += v * v; (acc)->prod_f *= v; \
            if (v < (acc)->min_f) (acc)->min_f = v; \
            if (v > (acc)->max_f) (acc)->max_f = v; \
            if (!(acc)->has_first) { (acc)->first_f = v; (acc)->has_first = true; } \
            (acc)->last_f = v; (acc)->cnt++; \
        } \
    } while (0)

/* Dispatch helper: expand REDUCE_LOOP_I/F with compile-time 0/1 constants for
 * HAS_NULLS and HAS_IDX based on the runtime pointers so the compiler can
 * dead-code-eliminate the branches inside each specialisation. */
#define DISPATCH_I(T, base, start, end, acc, has_nulls, null_bm, idx) \
    do { \
        if (!(has_nulls) && !(idx)) \
            REDUCE_LOOP_I(T, base, start, end, acc, 0, null_bm, 0, idx); \
        else if (!(has_nulls)) \
            REDUCE_LOOP_I(T, base, start, end, acc, 0, null_bm, 1, idx); \
        else if (!(idx)) \
            REDUCE_LOOP_I(T, base, start, end, acc, 1, null_bm, 0, idx); \
        else \
            REDUCE_LOOP_I(T, base, start, end, acc, 1, null_bm, 1, idx); \
    } while (0)

#define DISPATCH_F(base, start, end, acc, has_nulls, null_bm, idx) \
    do { \
        if (!(has_nulls) && !(idx)) \
            REDUCE_LOOP_F(base, start, end, acc, 0, null_bm, 0, idx); \
        else if (!(has_nulls)) \
            REDUCE_LOOP_F(base, start, end, acc, 0, null_bm, 1, idx); \
        else if (!(idx)) \
            REDUCE_LOOP_F(base, start, end, acc, 1, null_bm, 0, idx); \
        else \
            REDUCE_LOOP_F(base, start, end, acc, 1, null_bm, 1, idx); \
    } while (0)

static void reduce_range(ray_t* input, int64_t start, int64_t end,
                         reduce_acc_t* acc, bool has_nulls,
                         const uint8_t* null_bm, const int64_t* idx) {
    void* base = ray_data(input);
    switch (input->type) {
    case RAY_BOOL: case RAY_U8:
        DISPATCH_I(uint8_t, base, start, end, acc, has_nulls, null_bm, idx); break;
    case RAY_I16:
        DISPATCH_I(int16_t, base, start, end, acc, has_nulls, null_bm, idx); break;
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        DISPATCH_I(int32_t, base, start, end, acc, has_nulls, null_bm, idx); break;
    case RAY_I64: case RAY_TIMESTAMP:
        DISPATCH_I(int64_t, base, start, end, acc, has_nulls, null_bm, idx); break;
    case RAY_F64:
        DISPATCH_F(base, start, end, acc, has_nulls, null_bm, idx); break;
    case RAY_SYM: {
        /* Adaptive-width SYM columns — use read_col_i64.  Same 4-way dispatch
         * to eliminate the per-element null/idx branches. */
        if (!has_nulls && !idx) {
            for (int64_t i = start; i < end; i++) {
                int64_t v = read_col_i64(base, i, input->type, input->attrs);
                acc->sum_i += v; acc->sum_sq_i += v * v;
                acc->prod_i = (int64_t)((uint64_t)acc->prod_i * (uint64_t)v);
                if (v < acc->min_i) acc->min_i = v;
                if (v > acc->max_i) acc->max_i = v;
                if (!acc->has_first) { acc->first_i = v; acc->has_first = true; }
                acc->last_i = v; acc->cnt++;
            }
        } else if (!has_nulls) {
            for (int64_t i = start; i < end; i++) {
                int64_t row = idx[i];
                int64_t v = read_col_i64(base, row, input->type, input->attrs);
                acc->sum_i += v; acc->sum_sq_i += v * v;
                acc->prod_i = (int64_t)((uint64_t)acc->prod_i * (uint64_t)v);
                if (v < acc->min_i) acc->min_i = v;
                if (v > acc->max_i) acc->max_i = v;
                if (!acc->has_first) { acc->first_i = v; acc->has_first = true; }
                acc->last_i = v; acc->cnt++;
            }
        } else if (!idx) {
            for (int64_t i = start; i < end; i++) {
                if ((null_bm[i/8] >> (i%8)) & 1) { acc->null_count++; continue; }
                int64_t v = read_col_i64(base, i, input->type, input->attrs);
                acc->sum_i += v; acc->sum_sq_i += v * v;
                acc->prod_i = (int64_t)((uint64_t)acc->prod_i * (uint64_t)v);
                if (v < acc->min_i) acc->min_i = v;
                if (v > acc->max_i) acc->max_i = v;
                if (!acc->has_first) { acc->first_i = v; acc->has_first = true; }
                acc->last_i = v; acc->cnt++;
            }
        } else {
            for (int64_t i = start; i < end; i++) {
                int64_t row = idx[i];
                if ((null_bm[row/8] >> (row%8)) & 1) { acc->null_count++; continue; }
                int64_t v = read_col_i64(base, row, input->type, input->attrs);
                acc->sum_i += v; acc->sum_sq_i += v * v;
                acc->prod_i = (int64_t)((uint64_t)acc->prod_i * (uint64_t)v);
                if (v < acc->min_i) acc->min_i = v;
                if (v > acc->max_i) acc->max_i = v;
                if (!acc->has_first) { acc->first_i = v; acc->has_first = true; }
                acc->last_i = v; acc->cnt++;
            }
        }
        break;
    }
    default: break;
    }
}

/* Context for parallel reduction */
typedef struct {
    ray_t*         input;
    reduce_acc_t*  accs;   /* one per worker */
    bool           has_nulls;
    const uint8_t* null_bm;
    const int64_t* idx;    /* NULL = no selection; else int64[total_pass] */
} par_reduce_ctx_t;

static void par_reduce_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    par_reduce_ctx_t* c = (par_reduce_ctx_t*)ctx;
    reduce_range(c->input, start, end, &c->accs[worker_id],
                 c->has_nulls, c->null_bm, c->idx);
}

static void reduce_merge(reduce_acc_t* dst, const reduce_acc_t* src, int8_t in_type) {
    if (in_type == RAY_F64) {
        dst->sum_f += src->sum_f;
        dst->sum_sq_f += src->sum_sq_f;
        dst->prod_f *= src->prod_f;
        if (src->min_f < dst->min_f) dst->min_f = src->min_f;
        if (src->max_f > dst->max_f) dst->max_f = src->max_f;
    } else {
        /* Defined unsigned wrap — matches REDUCE_LOOP_I's per-row path. */
        dst->sum_i    = (int64_t)((uint64_t)dst->sum_i    + (uint64_t)src->sum_i);
        dst->sum_sq_i = (int64_t)((uint64_t)dst->sum_sq_i + (uint64_t)src->sum_sq_i);
        dst->prod_i   = (int64_t)((uint64_t)dst->prod_i   * (uint64_t)src->prod_i);
        if (src->min_i < dst->min_i) dst->min_i = src->min_i;
        if (src->max_i > dst->max_i) dst->max_i = src->max_i;
    }
    dst->cnt += src->cnt;
    dst->null_count += src->null_count;
    /* reduce_merge does not merge first/last; caller handles these separately.
     * Since workers process sequential ranges, worker 0's first is the global first,
     * and the last worker's last is the global last. */
}

/* Hash mixing constants used by the count-distinct kernel and helpers. */
#define CD_HASH_K1 0x9E3779B97F4A7C15ULL
#define CD_HASH_K2 0xBF58476D1CE4E5B9ULL

/* Per-partition hash-distinct.  Each worker is given a contiguous slice
 * of partition payloads (already grouped by hash high bits) and counts
 * distinct values within.  Since distinct values are guaranteed to fall
 * into the same partition, the global distinct count is the sum of
 * per-partition counts. */
typedef struct {
    int64_t* values;       /* concatenated partition payloads */
    int64_t* part_off;     /* P+1 prefix sums, partition boundaries */
    int64_t* part_count;   /* OUT: per-partition distinct count */
} cd_part_ctx_t;

static void cd_part_dedup_fn(void* ctx, uint32_t worker_id,
                             int64_t start, int64_t end) {
    (void)worker_id;
    cd_part_ctx_t* x = (cd_part_ctx_t*)ctx;
    for (int64_t p = start; p < end; p++) {
        int64_t off = x->part_off[p];
        int64_t cnt = x->part_off[p + 1] - off;
        if (cnt == 0) { x->part_count[p] = 0; continue; }

        uint64_t cap = (uint64_t)cnt * 2;
        if (cap < 32) cap = 32;
        uint64_t c = 1;
        while (c && c < cap) c <<= 1;
        if (!c) { x->part_count[p] = -1; continue; }
        cap = c;
        uint64_t mask = cap - 1;

        ray_t* set_hdr  = NULL;
        ray_t* used_hdr = NULL;
        int64_t* set    = (int64_t*)scratch_alloc (&set_hdr,
                                                   (size_t)cap * sizeof(int64_t));
        uint8_t* used   = (uint8_t*)scratch_calloc(&used_hdr,
                                                   (size_t)cap * sizeof(uint8_t));
        if (!set || !used) {
            if (set_hdr)  scratch_free(set_hdr);
            if (used_hdr) scratch_free(used_hdr);
            x->part_count[p] = -1;
            continue;
        }

        int64_t* base = x->values + off;
        int64_t distinct = 0;
        for (int64_t i = 0; i < cnt; i++) {
            int64_t v = base[i];
            uint64_t h = (uint64_t)v * CD_HASH_K1;
            h ^= h >> 33;
            uint64_t slot = h & mask;
            while (used[slot]) {
                if (set[slot] == v) goto cd_next;
                slot = (slot + 1) & mask;
            }
            set[slot]  = v;
            used[slot] = 1;
            distinct++;
            cd_next:;
        }
        scratch_free(set_hdr);
        scratch_free(used_hdr);
        x->part_count[p] = distinct;
    }
}

/* Width-specialised value extraction for the partition pass.  Reading
 * row-by-row through read_col_i64 was the dispatch overhead in the
 * sequential path; specialising on the column width lets the autovec
 * pass tighten the loop.
 *
 * Indexing note: histograms and cursors are keyed by *task index*, not
 * worker id.  ray_pool_dispatch's ring is work-stealing — the same
 * worker_id can claim different tasks across two consecutive
 * dispatches, so the row range processed by worker w in pass 1
 * (histogram) need not match the range processed by worker w in pass 2
 * (scatter).  Using worker_id as the cursor key would let pass 2
 * scatter writes overshoot the slot reserved by pass 1, mangle the
 * partition layout, and over- or under-count distinct values
 * non-deterministically.  Task index is stable across passes because
 * the row range tied to task t is fixed at dispatch-fill time. */
typedef struct {
    const void* base;
    int64_t*    counts;        /* P per-partition row counts (per task) */
    uint32_t    p_bits;
    uint64_t    p_mask;
    int64_t     grain;         /* rows per task (last task may have fewer) */
    int64_t     total;         /* total row count */
    uint8_t     stride_log2;   /* log2(elem size) for plain int paths */
    uint8_t     is_f64;
    int8_t      type;
    uint8_t     attrs;
} cd_count_ctx_t;

/* Count rows per partition (per task, into task-local slot).  Two
 * passes: this one fills the histograms; the next does the scatter.
 * Dispatched via ray_pool_dispatch_n with start=task_idx so the
 * cursor key is stable across the histogram and scatter passes. */
static void cd_hist_fn(void* ctx, uint32_t worker_id,
                       int64_t start, int64_t end) {
    (void)worker_id;
    (void)end;
    cd_count_ctx_t* x = (cd_count_ctx_t*)ctx;
    int64_t task_idx = start;
    int64_t row_start = task_idx * x->grain;
    int64_t row_end = row_start + x->grain;
    if (row_end > x->total) row_end = x->total;
    int64_t* hist = x->counts + (size_t)task_idx * (x->p_mask + 1);
    /* Reuse the existing tight loops by aliasing the local names. */
    start = row_start;
    end = row_end;
    const void* base = x->base;
    int8_t in_type = x->type;
    uint8_t in_attrs = x->attrs;
    uint64_t p_mask = x->p_mask;
    if (x->is_f64) {
        const double* d = (const double*)base;
        for (int64_t i = start; i < end; i++) {
            double fv = d[i];
            if (fv != fv) fv = (double)NAN;
            else if (fv == 0.0) fv = 0.0;
            int64_t val;
            memcpy(&val, &fv, sizeof(int64_t));
            uint64_t h = (uint64_t)val * CD_HASH_K1;
            h ^= h >> 33;
            uint64_t p = (h ^ (h >> 33)) & p_mask;
            hist[p]++;
        }
    } else if (in_type == RAY_I64 || in_type == RAY_TIMESTAMP) {
        const int64_t* d = (const int64_t*)base;
        for (int64_t i = start; i < end; i++) {
            int64_t val = d[i];
            uint64_t h = (uint64_t)val * CD_HASH_K1;
            h ^= h >> 33;
            uint64_t p = (h ^ (h >> 33)) & p_mask;
            hist[p]++;
        }
    } else if (in_type == RAY_I32 || in_type == RAY_DATE || in_type == RAY_TIME) {
        const int32_t* d = (const int32_t*)base;
        for (int64_t i = start; i < end; i++) {
            int64_t val = d[i];
            uint64_t h = (uint64_t)val * CD_HASH_K1;
            h ^= h >> 33;
            uint64_t p = (h ^ (h >> 33)) & p_mask;
            hist[p]++;
        }
    } else if (in_type == RAY_I16) {
        const int16_t* d = (const int16_t*)base;
        for (int64_t i = start; i < end; i++) {
            int64_t val = d[i];
            uint64_t h = (uint64_t)val * CD_HASH_K1;
            h ^= h >> 33;
            uint64_t p = (h ^ (h >> 33)) & p_mask;
            hist[p]++;
        }
    } else if (in_type == RAY_BOOL || in_type == RAY_U8) {
        const uint8_t* d = (const uint8_t*)base;
        for (int64_t i = start; i < end; i++) {
            int64_t val = d[i];
            uint64_t h = (uint64_t)val * CD_HASH_K1;
            h ^= h >> 33;
            uint64_t p = (h ^ (h >> 33)) & p_mask;
            hist[p]++;
        }
    } else if (in_type == RAY_SYM) {
        for (int64_t i = start; i < end; i++) {
            int64_t val = read_col_i64(base, i, in_type, in_attrs);
            uint64_t h = (uint64_t)val * CD_HASH_K1;
            h ^= h >> 33;
            uint64_t p = (h ^ (h >> 33)) & p_mask;
            hist[p]++;
        }
    }
}

typedef struct {
    const void* base;
    int64_t*    out_buf;       /* concatenated payloads (output) */
    int64_t*    cursor;        /* per-task × P; advances per scatter */
    uint32_t    p_bits;
    uint64_t    p_mask;
    int64_t     grain;         /* rows per task (last task may have fewer) */
    int64_t     total;         /* total row count */
    uint8_t     is_f64;
    int8_t      type;
    uint8_t     attrs;
} cd_scatter_ctx_t;

static void cd_scatter_fn(void* ctx, uint32_t worker_id,
                          int64_t start, int64_t end) {
    (void)worker_id;
    (void)end;
    cd_scatter_ctx_t* x = (cd_scatter_ctx_t*)ctx;
    int64_t task_idx = start;
    int64_t row_start = task_idx * x->grain;
    int64_t row_end = row_start + x->grain;
    if (row_end > x->total) row_end = x->total;
    int64_t* cur = x->cursor + (size_t)task_idx * (x->p_mask + 1);
    /* Reuse the existing tight loops by aliasing the local names. */
    start = row_start;
    end = row_end;
    int64_t* out = x->out_buf;
    const void* base = x->base;
    int8_t in_type = x->type;
    uint8_t in_attrs = x->attrs;
    uint64_t p_mask = x->p_mask;
    #define SCATTER_BODY(LOAD)                                                \
        for (int64_t i = start; i < end; i++) {                               \
            int64_t val = (LOAD);                                             \
            uint64_t h = (uint64_t)val * CD_HASH_K1;                          \
            h ^= h >> 33;                                                     \
            uint64_t p = (h ^ (h >> 33)) & p_mask;                            \
            out[cur[p]++] = val;                                              \
        }
    if (x->is_f64) {
        const double* d = (const double*)base;
        for (int64_t i = start; i < end; i++) {
            double fv = d[i];
            if (fv != fv) fv = (double)NAN;
            else if (fv == 0.0) fv = 0.0;
            int64_t val;
            memcpy(&val, &fv, sizeof(int64_t));
            uint64_t h = (uint64_t)val * CD_HASH_K1;
            h ^= h >> 33;
            uint64_t p = (h ^ (h >> 33)) & p_mask;
            out[cur[p]++] = val;
        }
    } else if (in_type == RAY_I64 || in_type == RAY_TIMESTAMP) {
        const int64_t* d = (const int64_t*)base;
        SCATTER_BODY(d[i])
    } else if (in_type == RAY_I32 || in_type == RAY_DATE || in_type == RAY_TIME) {
        const int32_t* d = (const int32_t*)base;
        SCATTER_BODY(d[i])
    } else if (in_type == RAY_I16) {
        const int16_t* d = (const int16_t*)base;
        SCATTER_BODY(d[i])
    } else if (in_type == RAY_BOOL || in_type == RAY_U8) {
        const uint8_t* d = (const uint8_t*)base;
        SCATTER_BODY(d[i])
    } else { /* RAY_SYM */
        SCATTER_BODY(read_col_i64(base, i, in_type, in_attrs))
    }
    #undef SCATTER_BODY
}

/* Sequential fallback for small inputs / when the pool isn't available.
 * Same algorithm as the original: open-addressing hash set, single pass. */
static int64_t cd_seq_count(int8_t in_type, uint8_t in_attrs,
                            const void* base, int64_t len) {
    uint64_t cap = (uint64_t)(len < 16 ? 32 : len) * 2;
    uint64_t c = 1;
    while (c && c < cap) c <<= 1;
    if (!c) return -1;
    cap = c;
    uint64_t mask = cap - 1;

    ray_t* set_hdr  = NULL;
    ray_t* used_hdr = NULL;
    int64_t* set    = (int64_t*)scratch_alloc (&set_hdr,  (size_t)cap * sizeof(int64_t));
    uint8_t* used   = (uint8_t*)scratch_calloc(&used_hdr, (size_t)cap * sizeof(uint8_t));
    if (!set || !used) {
        if (set_hdr) scratch_free(set_hdr);
        if (used_hdr) scratch_free(used_hdr);
        return -1;
    }
    int64_t count = 0;
    for (int64_t i = 0; i < len; i++) {
        int64_t val;
        if (in_type == RAY_F64) {
            double fv = ((const double*)base)[i];
            if (fv != fv) fv = (double)NAN;
            else if (fv == 0.0) fv = 0.0;
            memcpy(&val, &fv, sizeof(int64_t));
        } else {
            val = read_col_i64(base, i, in_type, in_attrs);
        }
        uint64_t h = (uint64_t)val * CD_HASH_K1;
        uint64_t slot = h & mask;
        while (used[slot]) {
            if (set[slot] == val) goto cd_seq_next;
            slot = (slot + 1) & mask;
        }
        set[slot]  = val;
        used[slot] = 1;
        count++;
        cd_seq_next:;
    }
    scratch_free(set_hdr);
    scratch_free(used_hdr);
    return count;
}

/* Hash-based count distinct for integer/float columns.
 *
 * Strategy:
 *  - small inputs            → sequential single-pass hash set (low overhead).
 *  - large inputs            → radix-partition by hash high bits across the
 *                              worker pool, then dedup each partition in
 *                              parallel.  Each partition fits L2, eliminating
 *                              the cache-miss-per-probe pattern of one giant
 *                              global set.  Distinct values land in the same
 *                              partition, so the global count is the sum of
 *                              per-partition counts. */
ray_t* exec_count_distinct(ray_graph_t* g, ray_op_t* op, ray_t* input) {
    (void)g; (void)op;
    if (!input || RAY_IS_ERR(input)) return input;

    int8_t in_type = input->type;
    int64_t len = input->len;

    if (len == 0) return ray_i64(0);

    switch (in_type) {
    case RAY_BOOL: case RAY_U8:
    case RAY_I16: case RAY_I32: case RAY_I64:
    case RAY_F64: case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
    case RAY_SYM:
        break;
    case RAY_STR:
    case RAY_GUID:
    case RAY_LIST: {
        /* The hash kernel only handles fixed-width scalar types.  For
         * STR / GUID / LIST the rewrite-aware path is to delegate to
         * distinct_vec_eager (which uses the row-aware hashset_t) and
         * count its result.  Slower than the radix kernel but correct. */
        ray_t* dist = distinct_vec_eager(input);
        if (!dist || RAY_IS_ERR(dist)) return dist ? dist : ray_error("oom", NULL);
        int64_t cnt = ray_len(dist);
        ray_release(dist);
        return ray_i64(cnt);
    }
    default:
        return ray_error("type", NULL);
    }

    void* base = ray_data(input);
    ray_pool_t* pool = ray_pool_get();

    /* Small-input fast path: per-row dispatch overhead would dwarf the
     * actual work. */
    if (!pool || len < (1 << 16)) {
        int64_t cnt = cd_seq_count(in_type, input->attrs, base, len);
        if (cnt < 0) return ray_error("oom", NULL);
        return ray_i64(cnt);
    }

    uint32_t nw = ray_pool_total_workers(pool);

    /* Partition count: a small power of two ≥ nw, capped so per-partition
     * sets stay in L2.  16 works well for nw=28; 32 for >32 workers.  */
    uint32_t p_bits;
    if (nw <= 8) p_bits = 4;       /* 16 partitions */
    else if (nw <= 32) p_bits = 5;  /* 32 partitions */
    else p_bits = 6;                /* 64 partitions */
    uint64_t P = (uint64_t)1 << p_bits;
    uint64_t p_mask = P - 1;

    /* Histograms and cursors are keyed by *task* index, not worker id, so
     * pass-2 scatter writes land in the slot that pass-1 histogram
     * reserved.  A worker may execute different tasks in the two passes
     * (the dispatch ring is work-stealing); the row range tied to a task
     * is fixed when ray_pool_dispatch_n fills the ring. */
    int64_t grain = (int64_t)RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS;
    if (grain <= 0) grain = 8192;
    int64_t n_tasks_64 = (len + grain - 1) / grain;
    if (n_tasks_64 <= 0) n_tasks_64 = 1;
    /* MAX_RING_CAP guards against pathological len; if we'd exceed it,
     * fall back to the sequential kernel — the cap is high enough that
     * this only fires on absurd inputs. */
    if (n_tasks_64 > (1u << 16)) {
        int64_t cnt = cd_seq_count(in_type, input->attrs, base, len);
        if (cnt < 0) return ray_error("oom", NULL);
        return ray_i64(cnt);
    }
    uint32_t n_tasks = (uint32_t)n_tasks_64;

    /* Pass 1: per-task histogram (P × n_tasks int64 cells). */
    ray_t* hist_hdr = NULL;
    int64_t* hist = (int64_t*)scratch_calloc(&hist_hdr,
                                             (size_t)P * n_tasks * sizeof(int64_t));
    if (!hist) {
        return ray_error("oom", NULL);
    }
    cd_count_ctx_t hctx = {
        .base = base, .counts = hist,
        .p_bits = p_bits, .p_mask = p_mask,
        .grain = grain, .total = len,
        .stride_log2 = 0, .is_f64 = (in_type == RAY_F64),
        .type = in_type, .attrs = input->attrs,
    };
    ray_pool_dispatch_n(pool, cd_hist_fn, &hctx, n_tasks);

    /* Convert per-task histograms into a global prefix sum.  Order:
     * partition_0_task_0, partition_0_task_1, …, partition_1_task_0, …
     * so each (task, partition) range is a contiguous slice of out_buf. */
    ray_t* off_hdr = NULL;
    int64_t* part_off = (int64_t*)scratch_alloc(&off_hdr,
                                                (size_t)(P + 1) * sizeof(int64_t));
    if (!part_off) { scratch_free(hist_hdr); return ray_error("oom", NULL); }
    ray_t* cur_hdr = NULL;
    int64_t* cursor = (int64_t*)scratch_alloc(&cur_hdr,
                                              (size_t)P * n_tasks * sizeof(int64_t));
    if (!cursor) {
        scratch_free(off_hdr); scratch_free(hist_hdr);
        return ray_error("oom", NULL);
    }

    int64_t total = 0;
    for (uint64_t p = 0; p < P; p++) {
        part_off[p] = total;
        for (uint32_t t = 0; t < n_tasks; t++) {
            cursor[(size_t)t * P + p] = total;
            total += hist[(size_t)t * P + p];
        }
    }
    part_off[P] = total;

    /* Sanity: total must equal len. */
    if (total != len) {
        scratch_free(cur_hdr); scratch_free(off_hdr); scratch_free(hist_hdr);
        return ray_error("nyi", "count_distinct: histogram mismatch");
    }

    /* Pass 2: scatter values into out_buf. */
    ray_t* buf_hdr = NULL;
    int64_t* out_buf = (int64_t*)scratch_alloc(&buf_hdr,
                                               (size_t)len * sizeof(int64_t));
    if (!out_buf) {
        scratch_free(cur_hdr); scratch_free(off_hdr); scratch_free(hist_hdr);
        return ray_error("oom", NULL);
    }
    cd_scatter_ctx_t sctx = {
        .base = base, .out_buf = out_buf, .cursor = cursor,
        .p_bits = p_bits, .p_mask = p_mask,
        .grain = grain, .total = len,
        .is_f64 = (in_type == RAY_F64),
        .type = in_type, .attrs = input->attrs,
    };
    ray_pool_dispatch_n(pool, cd_scatter_fn, &sctx, n_tasks);

    /* Pass 3: dedup each partition in parallel.  Each partition gets one
     * task — distinct values land in the same partition, so per-partition
     * sums give the global distinct count. */
    ray_t* pcnt_hdr = NULL;
    int64_t* part_count = (int64_t*)scratch_alloc(&pcnt_hdr,
                                                  (size_t)P * sizeof(int64_t));
    if (!part_count) {
        scratch_free(buf_hdr); scratch_free(cur_hdr);
        scratch_free(off_hdr); scratch_free(hist_hdr);
        return ray_error("oom", NULL);
    }
    cd_part_ctx_t dctx = {
        .values = out_buf, .part_off = part_off, .part_count = part_count,
    };
    ray_pool_dispatch_n(pool, cd_part_dedup_fn, &dctx, (uint32_t)P);

    int64_t total_distinct = 0;
    for (uint64_t p = 0; p < P; p++) {
        if (part_count[p] < 0) {
            scratch_free(pcnt_hdr); scratch_free(buf_hdr); scratch_free(cur_hdr);
            scratch_free(off_hdr); scratch_free(hist_hdr);
            return ray_error("oom", NULL);
        }
        total_distinct += part_count[p];
    }

    scratch_free(pcnt_hdr); scratch_free(buf_hdr); scratch_free(cur_hdr);
    scratch_free(off_hdr); scratch_free(hist_hdr);
    return ray_i64(total_distinct);
}

/* ════════════════════════════════════════════════════════════════════
 * Parallel partitioned grouped count(distinct).
 *
 * The serial kernel further down uses a single global hash keyed by
 * (gid, val).  At high (n_rows × n_groups) the hash exceeds L3 and
 * every probe is a cache miss — Q14 (937 K rows × 611 K groups) lands
 * at ~200 ms even though the per-row work is microscopic.
 *
 * Strategy: radix-partition (gid, val) pairs into P buckets by the high
 * bits of the composite hash, dispatch dedup of each bucket to the
 * worker pool.  Each bucket is sized to fit in L2, so hash probes hit
 * cache.  The dedup writes per-group distinct counts into the shared
 * `odata` via atomic increment.
 *
 * Three passes:
 *   1. cdpg_hist_fn  – per-worker histogram of partition counts.
 *   2. cdpg_scat_fn  – scatter (gid_p1, val) pairs into a partitioned
 *                       buffer using per-worker per-partition cursors.
 *   3. cdpg_dedup_fn – per-partition open-addressing dedup; atomic
 *                       fetch-add into `odata[gid]`.
 * ════════════════════════════════════════════════════════════════════ */

#define CDPG_HASH(GID_P1, VAL) ({                                       \
    uint64_t _h_ = (uint64_t)(VAL) * 0x9E3779B97F4A7C15ULL;             \
    _h_ ^= (uint64_t)(GID_P1) * 0xBF58476D1CE4E5B9ULL;                  \
    _h_ ^= _h_ >> 33;                                                    \
    _h_ *= 0xC4CEB9FE1A85EC53ULL;                                        \
    _h_;                                                                 \
})

/* Partition hash: keyed on gid_p1 only.  This guarantees all rows for
 * a given gid land in the same partition, so the dedup pass can update
 * `odata[gid]` without atomics — each gid's distinct count is owned by
 * exactly one task.  Independent of CDPG_HASH (which keys on the
 * full (gid, val) pair so the per-partition open-addressing HT spreads
 * evenly across slots). */
#define CDPG_PART_HASH(GID_P1) ({                                       \
    uint64_t _h_ = (uint64_t)(GID_P1) * 0xBF58476D1CE4E5B9ULL;          \
    _h_ ^= _h_ >> 33;                                                    \
    _h_ *= 0xC4CEB9FE1A85EC53ULL;                                        \
    _h_;                                                                 \
})

typedef struct {
    /* Inputs (read-only) */
    int8_t          in_type;
    uint8_t         in_attrs;
    const void*     base;
    const int64_t*  row_gid;
    int64_t         n_rows;
    int64_t         n_groups;
    bool            has_nulls;
    const uint8_t*  null_bm;
    uint64_t        p_mask;          /* P - 1, P = number of partitions */
    /* Pass 1 outputs / pass 2 inputs.  Per-task counters: each worker
     * writes to its own slice of hist[task_id * P] / cursor[task_id * P]
     * so there's no atomic contention.  task_id is derived from `start`
     * via the dispatch grain (matches ray_pool_dispatch's tasking).
     *
     * Earlier comment claimed P=64 atomic-cursor contention was
     * negligible — it isn't.  Q11's parallel speedup was 1.2× (not 28×)
     * because the per-row atomic_fetch_add on cursor[h & p_mask] in
     * cdpg_scat_fn serialised through the partition cache lines. */
    int64_t         grain;           /* task grain (matches pool dispatch) */
    int64_t         n_tasks;         /* number of tasks in the dispatch */
    int64_t*        hist;            /* [n_tasks * P] — per-task counts */
    int64_t*        cursor;          /* [n_tasks * P] — per-task scat cursor */
    int64_t*        part_off;        /* P + 1, prefix offsets */
    /* Pass 2 outputs */
    int64_t*        gids_out;        /* total_pass entries */
    int64_t*        vals_out;
    /* Pass 3 outputs */
    int64_t*        odata;           /* n_groups, atomic per-group distinct count */
} cdpg_ctx_t;

/* Read column row r as int64.  Width-typed fast path; F64 bitcasts. */
static inline int64_t cdpg_read(const void* base, int64_t r,
                                int8_t in_type, uint8_t esz) {
    if (in_type == RAY_F64) {
        double fv = ((const double*)base)[r];
        if (fv != fv) fv = (double)NAN;
        else if (fv == 0.0) fv = 0.0;
        int64_t v;
        memcpy(&v, &fv, sizeof(int64_t));
        return v;
    }
    switch (esz) {
    case 1:  return (int64_t)((const uint8_t*)base)[r];
    case 2:  return (int64_t)((const int16_t*)base)[r];
    case 4:  return (int64_t)((const int32_t*)base)[r];
    default: return ((const int64_t*)base)[r];
    }
}

static void cdpg_hist_fn(void* ctx_, uint32_t worker_id,
                         int64_t start, int64_t end) {
    (void)worker_id;
    cdpg_ctx_t* x = (cdpg_ctx_t*)ctx_;
    uint8_t esz = ray_sym_elem_size(x->in_type, x->in_attrs);
    uint64_t p_mask = x->p_mask;
    /* Per-task private hist slot — task_id is derived from `start` so
     * scat_fn computes the SAME task_id and reads cursor[task_id*P+p]
     * we wrote here.  No atomics: each task owns its row. */
    int64_t task_id = start / x->grain;
    int64_t* my_hist = &x->hist[task_id * (p_mask + 1)];
    for (int64_t r = start; r < end; r++) {
        int64_t gid = x->row_gid[r];
        if (gid < 0 || gid >= x->n_groups) continue;
        if (x->has_nulls && x->null_bm &&
            ((x->null_bm[r/8] >> (r%8)) & 1)) continue;
        /* Partition by gid (not gid×val) so the dedup pass can write to
         * odata[gid] without atomics. */
        uint64_t h = CDPG_PART_HASH(gid + 1);
        my_hist[h & p_mask]++;
    }
    (void)esz;
}

static void cdpg_scat_fn(void* ctx_, uint32_t worker_id,
                         int64_t start, int64_t end) {
    (void)worker_id;
    cdpg_ctx_t* x = (cdpg_ctx_t*)ctx_;
    uint8_t esz = ray_sym_elem_size(x->in_type, x->in_attrs);
    uint64_t p_mask = x->p_mask;
    /* Each task uses its private cursor — pre-computed by the
     * orchestrator from per-task hist counts so writes are guaranteed
     * non-overlapping across tasks within the same partition. */
    int64_t task_id = start / x->grain;
    int64_t* my_cur = &x->cursor[task_id * (p_mask + 1)];
    for (int64_t r = start; r < end; r++) {
        int64_t gid = x->row_gid[r];
        if (gid < 0 || gid >= x->n_groups) continue;
        if (x->has_nulls && x->null_bm &&
            ((x->null_bm[r/8] >> (r%8)) & 1)) continue;
        int64_t val = cdpg_read(x->base, r, x->in_type, esz);
        int64_t gid_p1 = gid + 1;
        uint64_t h = CDPG_PART_HASH(gid_p1);
        int64_t pos = my_cur[h & p_mask]++;
        x->gids_out[pos] = gid_p1;
        x->vals_out[pos] = val;
    }
}

/* Per-partition dedup: open-addressing hash sized for the partition, then
 * atomic fetch-add into odata[gid] for each new distinct (gid, val). */
static void cdpg_dedup_fn(void* ctx_, uint32_t worker_id,
                          int64_t start, int64_t end) {
    (void)worker_id;
    cdpg_ctx_t* x = (cdpg_ctx_t*)ctx_;
    for (int64_t p = start; p < end; p++) {
        int64_t off = x->part_off[p];
        int64_t cnt = x->part_off[p + 1] - off;
        if (cnt == 0) continue;

        uint64_t cap = (uint64_t)cnt * 2;
        if (cap < 32) cap = 32;
        uint64_t c = 1;
        while (c && c < cap) c <<= 1;
        if (!c) continue;
        cap = c;
        uint64_t mask = cap - 1;

        ray_t* k_hdr = NULL;
        ray_t* v_hdr = NULL;
        int64_t* slot_gid = (int64_t*)scratch_calloc(&k_hdr,
                                                     (size_t)cap * sizeof(int64_t));
        int64_t* slot_val = (int64_t*)scratch_alloc(&v_hdr,
                                                    (size_t)cap * sizeof(int64_t));
        if (!slot_gid || !slot_val) {
            if (k_hdr) scratch_free(k_hdr);
            if (v_hdr) scratch_free(v_hdr);
            continue;
        }

        const int64_t* gids = x->gids_out + off;
        const int64_t* vals = x->vals_out + off;
        for (int64_t i = 0; i < cnt; i++) {
            int64_t gid_p1 = gids[i];
            int64_t val    = vals[i];
            uint64_t h = CDPG_HASH(gid_p1, val);
            uint64_t slot = h & mask;
            for (;;) {
                int64_t cur = slot_gid[slot];
                if (cur == 0) {
                    slot_gid[slot] = gid_p1;
                    slot_val[slot] = val;
                    /* Partition is keyed on gid (CDPG_PART_HASH), so
                     * each gid is owned by exactly one task — drop the
                     * atomic. */
                    x->odata[gid_p1 - 1]++;
                    break;
                }
                if (cur == gid_p1 && slot_val[slot] == val) break;
                slot = (slot + 1) & mask;
            }
        }
        scratch_free(k_hdr);
        scratch_free(v_hdr);
    }
}

/* Returns the populated `out` vector on success, or NULL to fall through
 * to the serial path on dispatch / allocation failure. */
static ray_t* count_distinct_per_group_parallel(
        ray_t* src, const int64_t* row_gid,
        int64_t n_rows, int64_t n_groups, ray_t* out)
{
    ray_pool_t* pool = ray_pool_get();
    if (!pool) return NULL;
    uint32_t nw = ray_pool_total_workers(pool);
    if (nw < 2) return NULL;

    /* Partition count: balance per-partition L2 fit vs. dispatch overhead.
     * 64 partitions on 28 workers gives 2.28 partitions per worker plus
     * room for skew; per-partition dedup data ~2 × (n_rows/64) × 16 B
     * which is well inside L2 even on 1 M-row inputs. */
    uint8_t p_bits = 6;
    uint64_t P = (uint64_t)1 << p_bits;
    uint64_t p_mask = P - 1;

    cdpg_ctx_t ctx = {
        .in_type = src->type,
        .in_attrs = src->attrs,
        .base = ray_data(src),
        .row_gid = row_gid,
        .n_rows = n_rows,
        .n_groups = n_groups,
        .has_nulls = (src->attrs & RAY_ATTR_HAS_NULLS) != 0,
        .null_bm = NULL,
        .p_mask = p_mask,
        .odata = (int64_t*)ray_data(out),
    };
    if (ctx.has_nulls)
        ctx.null_bm = ray_vec_nullmap_bytes(src, NULL, NULL);

    if (P > 256) return NULL;

    /* Match ray_pool_dispatch's task layout so task_id derived from
     * `start / grain` inside the worker fn matches the row range the
     * dispatch hands out.  Mirrors pool.c's TASK_GRAIN (8 morsels of
     * 1024 rows each) and MAX_RING_CAP (65536) clamping logic. */
    int64_t grain = (int64_t)RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS;
    int64_t n_tasks = (n_rows + grain - 1) / grain;
    if (n_tasks > 65536) {
        n_tasks = 65536;
        grain = (n_rows + n_tasks - 1) / n_tasks;
    }
    ctx.grain   = grain;
    ctx.n_tasks = n_tasks;

    /* Pass 1: per-task histograms — no atomics. */
    ray_t* hist_hdr = NULL;
    ctx.hist = (int64_t*)scratch_calloc(&hist_hdr,
                                        (size_t)n_tasks * (size_t)P * sizeof(int64_t));
    if (!ctx.hist) { return NULL; }
    ray_pool_dispatch(pool, cdpg_hist_fn, &ctx, n_rows);

    /* Compute global per-partition totals + prefix offsets, then per-task
     * scatter cursors.  Layout invariant: tasks within a partition write
     * to non-overlapping ranges, so scat_fn doesn't need atomics. */
    ray_t* off_hdr = NULL;
    ctx.part_off = (int64_t*)scratch_alloc(&off_hdr,
                                           (size_t)(P + 1) * sizeof(int64_t));
    ray_t* cur_hdr = NULL;
    ctx.cursor = (int64_t*)scratch_alloc(&cur_hdr,
                                         (size_t)n_tasks * (size_t)P * sizeof(int64_t));
    if (!ctx.part_off || !ctx.cursor) {
        if (off_hdr) scratch_free(off_hdr);
        if (cur_hdr) scratch_free(cur_hdr);
        scratch_free(hist_hdr);
        return NULL;
    }
    /* Two-step prefix: per-partition global offset, then per-(task,
     * partition) cursor by walking tasks in order. */
    int64_t total = 0;
    for (uint64_t p = 0; p < P; p++) {
        ctx.part_off[p] = total;
        int64_t cum = total;
        for (int64_t t = 0; t < n_tasks; t++) {
            int64_t cnt = ctx.hist[t * P + p];
            ctx.cursor[t * P + p] = cum;
            cum += cnt;
        }
        total = cum;
    }
    ctx.part_off[P] = total;

    /* Pass 2: scatter (gid+1, val) pairs into partitioned out_buf. */
    ray_t* gids_hdr = NULL;
    ray_t* vals_hdr = NULL;
    ctx.gids_out = (int64_t*)scratch_alloc(&gids_hdr,
                                           (size_t)total * sizeof(int64_t));
    ctx.vals_out = (int64_t*)scratch_alloc(&vals_hdr,
                                           (size_t)total * sizeof(int64_t));
    if (!ctx.gids_out || !ctx.vals_out) {
        if (gids_hdr) scratch_free(gids_hdr);
        if (vals_hdr) scratch_free(vals_hdr);
        scratch_free(cur_hdr); scratch_free(off_hdr); scratch_free(hist_hdr);
        return NULL;
    }
    if (total > 0)
        ray_pool_dispatch(pool, cdpg_scat_fn, &ctx, n_rows);

    /* Pass 3: per-partition dedup; partition is keyed on gid via
     * CDPG_PART_HASH so each gid is owned by exactly one task — odata
     * updates run without atomics. */
    if (total > 0)
        ray_pool_dispatch_n(pool, cdpg_dedup_fn, &ctx, (uint32_t)P);

    scratch_free(vals_hdr); scratch_free(gids_hdr);
    scratch_free(cur_hdr);  scratch_free(off_hdr);
    scratch_free(hist_hdr);
    return out;
}

/* Grouped count(distinct): single global hash keyed by (group_id, value).
 * One linear pass over all rows, O(n) total instead of O(per-group setup *
 * n_groups).  Returns an I64 vector of length n_groups with the per-group
 * distinct count.  Rows whose row_gid[r] < 0 are skipped.
 *
 * Supported value types: integers / SYM / TIMESTAMP / DATE / TIME / F64.
 * Caller is responsible for verifying the type up-front (it should match
 * exec_count_distinct's whitelist) and returning NULL on miss so the
 * legacy per-group fallback handles unsupported configs.
 *
 * Cap selection: 2 * n_rows rounded to power of 2.  Worst case all rows
 * are distinct pairs → load factor 0.5, no rehash needed.  Slot stores
 * gid+1 (so 0 means empty) and the int64-encoded value.  64-bit composite
 * hash mixes both halves so rare-gid collisions don't cluster. */
ray_t* ray_count_distinct_per_group(ray_t* src, const int64_t* row_gid,
                                    int64_t n_rows, int64_t n_groups) {
    if (!src || RAY_IS_ERR(src) || n_groups < 0) return ray_error("domain", NULL);
    int8_t in_type = src->type;
    switch (in_type) {
    case RAY_BOOL: case RAY_U8:
    case RAY_I16: case RAY_I32: case RAY_I64:
    case RAY_F64: case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
    case RAY_SYM:
        break;
    default:
        return NULL; /* unsupported — caller falls back. */
    }
    if (src->len < n_rows) return ray_error("domain", NULL);

    ray_t* out = ray_vec_new(RAY_I64, n_groups);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n_groups;
    int64_t* odata = (int64_t*)ray_data(out);
    memset(odata, 0, (size_t)n_groups * sizeof(int64_t));
    if (n_rows == 0 || n_groups == 0) return out;

    /* Parallel partitioned path for sizes where the serial global hash
     * blows L3.  Threshold tuned so the partition / scatter / dedup
     * dispatch overhead stays smaller than the cache-miss savings. */
    if (n_rows >= 200000) {
        ray_t* par = count_distinct_per_group_parallel(src, row_gid,
                                                        n_rows, n_groups, out);
        if (par) return par;
        /* par == NULL → no pool / OOM in scratch alloc → fall through to
         * serial path with the already-allocated `out` (still zeroed). */
    }

    /* Pick capacity ≥ 2 × n_rows rounded up to power of two.  This bounds
     * load factor at 0.5 even when every (gid, val) pair is distinct. */
    uint64_t cap = (uint64_t)n_rows * 2;
    if (cap < 32) cap = 32;
    uint64_t c = 1;
    while (c && c < cap) c <<= 1;
    if (!c) { ray_release(out); return ray_error("oom", NULL); }
    cap = c;
    uint64_t mask = cap - 1;

    /* Slot layout: parallel arrays of (gid_plus_one, value).  gid_plus_one
     * == 0 means slot is empty; storing gid+1 lets us skip a separate
     * `used` bitmap.  Both arrays are scratch_alloc so they go through
     * the slab/heap fast path. */
    ray_t* k_hdr = NULL;
    ray_t* v_hdr = NULL;
    int64_t* slot_gid = (int64_t*)scratch_calloc(&k_hdr,
                                                 (size_t)cap * sizeof(int64_t));
    int64_t* slot_val = (int64_t*)scratch_alloc(&v_hdr,
                                                (size_t)cap * sizeof(int64_t));
    if (!slot_gid || !slot_val) {
        if (k_hdr) scratch_free(k_hdr);
        if (v_hdr) scratch_free(v_hdr);
        ray_release(out);
        return ray_error("oom", NULL);
    }

    void* base = ray_data(src);
    bool has_nulls = (src->attrs & RAY_ATTR_HAS_NULLS) != 0;
    const uint8_t* null_bm = has_nulls ? ray_vec_nullmap_bytes(src, NULL, NULL)
                                        : NULL;

    /* Per-type read width — hoist the type dispatch out of the hot loop.
     * read_col_i64 was branching on `in_type` every iteration plus paying
     * an indirect call. */
    uint8_t esz = ray_sym_elem_size(in_type, src->attrs);

    /* Macro: insert (val) for current row, given that (gid, val) is the
     * candidate pair; expects local vars `slot`, `cur`, `gid_p1`. */
    #define CD_INSERT(VAL_EXPR) do {                                    \
        int64_t val = (VAL_EXPR);                                       \
        int64_t gid_p1 = gid + 1;                                       \
        uint64_t h = (uint64_t)val * 0x9E3779B97F4A7C15ULL;             \
        h ^= (uint64_t)gid_p1 * 0xBF58476D1CE4E5B9ULL;                  \
        h ^= h >> 33;                                                   \
        h *= 0xC4CEB9FE1A85EC53ULL;                                     \
        uint64_t slot = h & mask;                                       \
        for (;;) {                                                      \
            int64_t cur = slot_gid[slot];                               \
            if (cur == 0) {                                             \
                slot_gid[slot] = gid_p1;                                \
                slot_val[slot] = val;                                   \
                odata[gid]++;                                           \
                break;                                                  \
            }                                                           \
            if (cur == gid_p1 && slot_val[slot] == val) break;          \
            slot = (slot + 1) & mask;                                   \
        }                                                               \
    } while (0)

    /* Specialised per-type loops.  Each version reads the column with a
     * width-typed pointer dereference instead of dispatching through
     * read_col_i64 every row.  The has_nulls / no-nulls split keeps the
     * fast path branch-free for the common no-null SYM/I64 columns. */
    if (!has_nulls) {
        if (in_type == RAY_F64) {
            const double* d = (const double*)base;
            for (int64_t r = 0; r < n_rows; r++) {
                int64_t gid = row_gid[r];
                if (gid < 0 || gid >= n_groups) continue;
                double fv = d[r];
                if (fv != fv) fv = (double)NAN;
                else if (fv == 0.0) fv = 0.0;
                int64_t v;
                memcpy(&v, &fv, sizeof(int64_t));
                CD_INSERT(v);
            }
        } else if (esz == 8) {
            const int64_t* d = (const int64_t*)base;
            for (int64_t r = 0; r < n_rows; r++) {
                int64_t gid = row_gid[r];
                if (gid < 0 || gid >= n_groups) continue;
                CD_INSERT(d[r]);
            }
        } else if (esz == 4) {
            const int32_t* d = (const int32_t*)base;
            for (int64_t r = 0; r < n_rows; r++) {
                int64_t gid = row_gid[r];
                if (gid < 0 || gid >= n_groups) continue;
                CD_INSERT((int64_t)d[r]);
            }
        } else if (esz == 2) {
            const int16_t* d = (const int16_t*)base;
            for (int64_t r = 0; r < n_rows; r++) {
                int64_t gid = row_gid[r];
                if (gid < 0 || gid >= n_groups) continue;
                CD_INSERT((int64_t)d[r]);
            }
        } else { /* esz == 1 */
            const uint8_t* d = (const uint8_t*)base;
            for (int64_t r = 0; r < n_rows; r++) {
                int64_t gid = row_gid[r];
                if (gid < 0 || gid >= n_groups) continue;
                CD_INSERT((int64_t)d[r]);
            }
        }
    } else {
        /* Has-nulls fallback: keep the per-row null bitmap probe and
         * the generic read_col_i64 dispatch.  Adding eight specialised
         * has-nulls loops costs more code than the small gain on
         * already-rare null-bearing columns. */
        for (int64_t r = 0; r < n_rows; r++) {
            int64_t gid = row_gid[r];
            if (gid < 0 || gid >= n_groups) continue;
            if (null_bm && ((null_bm[r/8] >> (r%8)) & 1)) continue;
            /* Use a different name from the macro's inner `val` so
             * clang doesn't see an `int64_t val = (val);` self-init
             * after macro expansion. */
            int64_t row_val;
            if (in_type == RAY_F64) {
                double fv = ((double*)base)[r];
                if (fv != fv) fv = (double)NAN;
                else if (fv == 0.0) fv = 0.0;
                memcpy(&row_val, &fv, sizeof(int64_t));
            } else {
                row_val = read_col_i64(base, r, in_type, src->attrs);
            }
            CD_INSERT(row_val);
        }
    }

    #undef CD_INSERT

    scratch_free(k_hdr);
    scratch_free(v_hdr);
    return out;
}

/* ─── ray_median_per_group_buf ──────────────────────────────────────────
 *
 * Parallel exact-median per group using the bucket-scatter layout that
 * the upstream group-by phase has already produced (idx_buf is already
 * group-contiguous; offsets[g]..offsets[g]+grp_cnt[g] is group g's row-
 * index slice).  Each group becomes one task in ray_pool_dispatch_n:
 * the task allocates a stack-or-heap-backed double slice, reads
 * src[idx_buf[off+i]] into it, then runs ray_median_dbl_inplace.
 *
 * Why this layout — and why it matches DuckDB without paying their
 * realloc-per-group price:
 *   - DuckDB's holistic quantile aggregate accumulates a per-group
 *     vector<INPUT_TYPE> during the radix probe; each insert is a
 *     potential vector grow.  At finalize it nth_element's each group's
 *     vector in parallel.
 *   - rayforce's radix probe (see idxbuf_par_fn) already produced
 *     prefix-summed group-contiguous indices.  So we skip DuckDB's
 *     vector-grow phase entirely — we just dispatch n_groups tasks
 *     that each gather values + quickselect.
 *
 * Cache behaviour: the inner loop reads src[idx_buf[off+i]] for a
 * single group, then quickselects the resulting slice.  The slice is
 * sized at grp_cnt[g] (median group ~1k for q6) and stays L2-hot for
 * the partial-sort.  Inputs are random over src so reads are still
 * cache-missing on the source column, but those misses overlap with
 * parallel tasks on other cores — the 27-core dispatch hides them.
 *
 * Type support: F64 native; I64/I32/I16/U8 cast-to-double on read.
 * Null rows are skipped (pairwise complete, matching DuckDB).
 *
 * Returns: F64 vec of length n_groups, or NULL on unsupported type
 * (caller must fall back).  On error returns RAY_IS_ERR ptr.
 *
 * Threshold: serial fallback when n_groups < 8 OR total < 4096 — the
 * dispatch overhead for tiny inputs is not worth it. */

typedef struct {
    const void*    base;        /* ray_data(src) */
    int8_t         src_type;
    bool           has_nulls;
    const uint8_t* null_bm;
    const int64_t* idx_buf;
    const int64_t* offsets;
    const int64_t* grp_cnt;
    double*        scratch_pool; /* flat shared scratch, sized at sum(grp_cnt) */
    double*        out_data;     /* ray_data(out) */
    ray_t*         out;          /* for set_null */
} med_par_ctx_t;

static inline double med_read_as_f64(const void* base, int8_t t, int64_t row) {
    switch (t) {
        case RAY_F64: { double v; memcpy(&v, (const char*)base + (size_t)row * 8, 8); return v; }
        case RAY_I64: { int64_t v; memcpy(&v, (const char*)base + (size_t)row * 8, 8); return (double)v; }
        case RAY_I32: { int32_t v; memcpy(&v, (const char*)base + (size_t)row * 4, 4); return (double)v; }
        case RAY_I16: { int16_t v; memcpy(&v, (const char*)base + (size_t)row * 2, 2); return (double)v; }
        case RAY_U8:  return (double)((const uint8_t*)base)[row];
        default:      return 0.0;
    }
}

static void med_per_group_fn(void* ctx_v, uint32_t worker_id,
                             int64_t start, int64_t end) {
    (void)worker_id;
    med_par_ctx_t* c = (med_par_ctx_t*)ctx_v;
    for (int64_t g = start; g < end; g++) {
        int64_t cnt = c->grp_cnt[g];
        int64_t off = c->offsets[g];
        double* slice = c->scratch_pool + off;
        int64_t actual = 0;
        if (c->has_nulls && c->null_bm) {
            for (int64_t i = 0; i < cnt; i++) {
                int64_t row = c->idx_buf[off + i];
                if ((c->null_bm[row >> 3] >> (row & 7)) & 1) continue;
                slice[actual++] = med_read_as_f64(c->base, c->src_type, row);
            }
        } else {
            for (int64_t i = 0; i < cnt; i++) {
                int64_t row = c->idx_buf[off + i];
                slice[actual++] = med_read_as_f64(c->base, c->src_type, row);
            }
        }
        if (actual == 0) {
            c->out_data[g] = 0.0;
            ray_vec_set_null(c->out, g, true);
        } else {
            c->out_data[g] = ray_median_dbl_inplace(slice, actual);
        }
    }
}

ray_t* ray_median_per_group_buf(ray_t* src,
                                const int64_t* idx_buf,
                                const int64_t* offsets,
                                const int64_t* grp_cnt,
                                int64_t n_groups) {
    if (!src || RAY_IS_ERR(src) || n_groups < 0) return NULL;
    int8_t t = src->type;
    if (t != RAY_F64 && t != RAY_I64 && t != RAY_I32 &&
        t != RAY_I16 && t != RAY_U8) return NULL;

    int64_t total = 0;
    for (int64_t g = 0; g < n_groups; g++) total += grp_cnt[g];

    ray_t* out = ray_vec_new(RAY_F64, n_groups);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n_groups;

    ray_t* buf_hdr = NULL;
    double* scratch = NULL;
    if (total > 0) {
        scratch = (double*)scratch_alloc(&buf_hdr,
                                         (size_t)total * sizeof(double));
        if (!scratch) { ray_release(out); return ray_error("oom", NULL); }
    }

    med_par_ctx_t ctx = {
        .base = ray_data(src),
        .src_type = t,
        .has_nulls = (src->attrs & RAY_ATTR_HAS_NULLS) != 0,
        .null_bm = (src->attrs & RAY_ATTR_HAS_NULLS)
                   ? ray_vec_nullmap_bytes(src, NULL, NULL) : NULL,
        .idx_buf = idx_buf,
        .offsets = offsets,
        .grp_cnt = grp_cnt,
        .scratch_pool = scratch,
        .out_data = (double*)ray_data(out),
        .out = out,
    };

    ray_pool_t* pool = ray_pool_get();
    bool par = pool && n_groups >= 8 && total >= 4096;
    if (par) {
        /* dispatch_n's task ring is capped at MAX_RING_CAP (65536); when
         * n_groups exceeds that, fall back to elements-based dispatch
         * (auto-grows grain so every group is covered).  Under the cap,
         * one task per group gives the best parallelism for small K
         * per-group work like quickselect. */
        if (n_groups < (1 << 16))
            ray_pool_dispatch_n(pool, med_per_group_fn, &ctx, (uint32_t)n_groups);
        else
            ray_pool_dispatch(pool, med_per_group_fn, &ctx, n_groups);
    } else {
        med_per_group_fn(&ctx, 0, 0, n_groups);
    }

    if (buf_hdr) scratch_free(buf_hdr);
    return out;
}

/* ─── ray_topk_per_group_buf ──────────────────────────────────────────
 *
 * Parallel per-group bounded-heap top-K / bot-K.  Same idx_buf/offsets/
 * grp_cnt layout as the median kernel — produced by exec_group's
 * post-radix re-probe + histogram-scatter.  Each group becomes one
 * task; the task initialises a heap with the first kk = min(K, cnt)
 * source values, then scans the remaining cnt - kk values and replaces
 * the worst-of-kept whenever a better value arrives.  Final heap is
 * sorted in-place via heapsort_extract so the cell reads in the
 * conventional order (desc=1 → largest-first, desc=0 → smallest-first),
 * matching the standalone ray_top_fn / ray_bot_fn conventions.
 *
 * For K=2 (q8 canonical) the heap ops are nearly free — the dominant
 * cost is reading from the source column under random-index access.
 *
 * Output is a LIST of n_groups cells; cells are pre-allocated typed
 * vecs of the same element type as `src`, so workers can write into
 * cell data without locking.  Null rows are skipped (matches the
 * standalone topk_take_vec path which routes nulls-last for asc,
 * nulls-first for desc and gathers only the non-null prefix). */

typedef struct {
    const void*    base;
    int8_t         src_type;
    bool           has_nulls;
    const uint8_t* null_bm;
    int64_t        k;
    uint8_t        desc;
    const int64_t* idx_buf;
    const int64_t* offsets;
    const int64_t* grp_cnt;
    ray_t*         out_list;
} topk_par_ctx_t;

/* Read src element as f64 (for the F64 path).  Matches med_read_as_f64
 * but the topk kernel uses it only on the F64 type arm. */
static inline double topk_read_f64(const void* base, int64_t row) {
    double v; memcpy(&v, (const char*)base + (size_t)row * 8, 8); return v;
}

/* Read src element as int64 for integer source types. */
static inline int64_t topk_read_i64(const void* base, int8_t t, int64_t row) {
    switch (t) {
        case RAY_I64: case RAY_TIMESTAMP:
            { int64_t v; memcpy(&v, (const char*)base + (size_t)row * 8, 8); return v; }
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            { int32_t v; memcpy(&v, (const char*)base + (size_t)row * 4, 4); return (int64_t)v; }
        case RAY_I16:
            { int16_t v; memcpy(&v, (const char*)base + (size_t)row * 2, 2); return (int64_t)v; }
        case RAY_BOOL: case RAY_U8:
            return (int64_t)((const uint8_t*)base)[row];
        default: return 0;
    }
}

/* Write int64 value to dst at slot idx, narrowing to esz bytes. */
static inline void topk_write_i64(void* dst, int64_t idx, int64_t v, uint8_t esz) {
    switch (esz) {
        case 1: ((uint8_t*)dst)[idx]  = (uint8_t)v; break;
        case 2: ((int16_t*)dst)[idx]  = (int16_t)v; break;
        case 4: ((int32_t*)dst)[idx]  = (int32_t)v; break;
        default: ((int64_t*)dst)[idx] = v; break;
    }
}

/* sift_down on a double[] heap.  max=1 → max-heap (root is largest),
 * max=0 → min-heap (root is smallest).  Called only with i < n. */
static inline void topk_sift_down_dbl(double* h, int64_t n, int64_t i, int max_heap) {
    for (;;) {
        int64_t l = 2*i+1, r = 2*i+2, w = i;
        if (max_heap) {
            if (l < n && h[l] > h[w]) w = l;
            if (r < n && h[r] > h[w]) w = r;
        } else {
            if (l < n && h[l] < h[w]) w = l;
            if (r < n && h[r] < h[w]) w = r;
        }
        if (w == i) break;
        double t = h[i]; h[i] = h[w]; h[w] = t;
        i = w;
    }
}

static inline void topk_sift_down_i64(int64_t* h, int64_t n, int64_t i, int max_heap) {
    for (;;) {
        int64_t l = 2*i+1, r = 2*i+2, w = i;
        if (max_heap) {
            if (l < n && h[l] > h[w]) w = l;
            if (r < n && h[r] > h[w]) w = r;
        } else {
            if (l < n && h[l] < h[w]) w = l;
            if (r < n && h[r] < h[w]) w = r;
        }
        if (w == i) break;
        int64_t t = h[i]; h[i] = h[w]; h[w] = t;
        i = w;
    }
}

/* For top (desc=1), the kept-K live in a MIN-heap so the root is the
 * smallest of the kept (worst-of-best) — easy to evict when a larger
 * value arrives.  Final heapsort with a min-heap drains smallest-first,
 * so to emit largest-first we extract into the tail of the cell and
 * read forward.  Symmetric for bot.  This keeps the inner loop in the
 * cheap "compare against root, sift" shape. */
static void topk_per_group_fn(void* ctx_v, uint32_t worker_id,
                              int64_t start, int64_t end) {
    (void)worker_id;
    topk_par_ctx_t* c = (topk_par_ctx_t*)ctx_v;
    int8_t t = c->src_type;
    int64_t K = c->k;
    uint8_t desc = c->desc;
    for (int64_t gi = start; gi < end; gi++) {
        ray_t* cell = ray_list_get(c->out_list, gi);
        if (!cell) continue;
        int64_t cnt = c->grp_cnt[gi];
        int64_t off = c->offsets[gi];
        const int64_t* idxs = &c->idx_buf[off];

        /* Heap orientation: top (desc=1) keeps largest → min-heap
         * (root=smallest-of-kept) so a larger candidate evicts the root.
         * bot (desc=0) keeps smallest → max-heap symmetric.  max_heap
         * arg to sift_down follows that mapping (inverted from the
         * "what we want" direction). */
        int max_heap = desc ? 0 : 1;

        if (t == RAY_F64) {
            double* dst = (double*)ray_data(cell);
            int64_t kept = 0;
            int64_t init_end = 0;  /* idx into idxs[] right after init */
            for (int64_t i = 0; i < cnt && kept < K; i++) {
                int64_t row = idxs[i];
                init_end = i + 1;
                if (c->has_nulls && c->null_bm &&
                    ((c->null_bm[row >> 3] >> (row & 7)) & 1)) continue;
                dst[kept++] = topk_read_f64(c->base, row);
            }
            if (kept == K) {
                for (int64_t j = K/2 - 1; j >= 0; j--)
                    topk_sift_down_dbl(dst, K, j, max_heap);
                for (int64_t i = init_end; i < cnt; i++) {
                    int64_t row = idxs[i];
                    if (c->has_nulls && c->null_bm &&
                        ((c->null_bm[row >> 3] >> (row & 7)) & 1)) continue;
                    double v = topk_read_f64(c->base, row);
                    if (desc ? (v > dst[0]) : (v < dst[0])) {
                        dst[0] = v;
                        topk_sift_down_dbl(dst, K, 0, max_heap);
                    }
                }
            }
            /* Heapsort drains root-first.  Our heap orientation is
             * opposite to the desired output order (top → min-heap →
             * drains ascending, but we want descending), so the
             * standard heapsort + reverse sequence puts elements in
             * the correct order.  Equivalent shortcut: extract roots
             * into the tail.  We do that by sifting after swapping
             * heap[0] with heap[n-1] — that puts the root at the end
             * each iteration, which already gives the desired final
             * order. */
            int64_t n = kept;
            while (n > 1) {
                double tmp = dst[0]; dst[0] = dst[n-1]; dst[n-1] = tmp;
                n--;
                topk_sift_down_dbl(dst, n, 0, max_heap);
            }
            cell->len = kept;
        } else {
            /* Integer source: stage heap in stack buffer (K <= 1024 →
             * 8KB), then narrow back to cell esz on write. */
            void* dst = ray_data(cell);
            uint8_t esz = ray_sym_elem_size(t, cell->attrs);
            int64_t heap[1024];
            int64_t kept = 0;
            int64_t init_end = 0;
            for (int64_t i = 0; i < cnt && kept < K; i++) {
                int64_t row = idxs[i];
                init_end = i + 1;
                if (c->has_nulls && c->null_bm &&
                    ((c->null_bm[row >> 3] >> (row & 7)) & 1)) continue;
                heap[kept++] = topk_read_i64(c->base, t, row);
            }
            if (kept == K) {
                for (int64_t j = K/2 - 1; j >= 0; j--)
                    topk_sift_down_i64(heap, K, j, max_heap);
                for (int64_t i = init_end; i < cnt; i++) {
                    int64_t row = idxs[i];
                    if (c->has_nulls && c->null_bm &&
                        ((c->null_bm[row >> 3] >> (row & 7)) & 1)) continue;
                    int64_t v = topk_read_i64(c->base, t, row);
                    if (desc ? (v > heap[0]) : (v < heap[0])) {
                        heap[0] = v;
                        topk_sift_down_i64(heap, K, 0, max_heap);
                    }
                }
            }
            int64_t n = kept;
            while (n > 1) {
                int64_t tmp = heap[0]; heap[0] = heap[n-1]; heap[n-1] = tmp;
                n--;
                topk_sift_down_i64(heap, n, 0, max_heap);
            }
            for (int64_t i = 0; i < kept; i++)
                topk_write_i64(dst, i, heap[i], esz);
            cell->len = kept;
        }
    }
}

ray_t* ray_topk_per_group_buf(ray_t* src,
                              int64_t k,
                              uint8_t desc,
                              const int64_t* idx_buf,
                              const int64_t* offsets,
                              const int64_t* grp_cnt,
                              int64_t n_groups) {
    if (!src || RAY_IS_ERR(src) || n_groups < 0) return NULL;
    if (k < 1 || k > 1024) return NULL;
    int8_t t = src->type;
    if (t != RAY_F64 && t != RAY_I64 && t != RAY_I32 && t != RAY_I16 &&
        t != RAY_U8  && t != RAY_BOOL && t != RAY_DATE && t != RAY_TIME &&
        t != RAY_TIMESTAMP)
        return NULL;

    int64_t total = 0;
    for (int64_t g = 0; g < n_groups; g++) total += grp_cnt[g];

    ray_t* out = ray_list_new(n_groups);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);

    /* Pre-allocate per-group cells, sized at min(K, grp_cnt[gi]).
     * Cells are typed to match `src` so q8's F64 source gives F64
     * cells, and (top (as 'I32 v) 3) preserves I32 (matches the
     * standalone top_bot.rfl invariants). */
    for (int64_t gi = 0; gi < n_groups; gi++) {
        int64_t kk = grp_cnt[gi] < k ? grp_cnt[gi] : k;
        ray_t* cell = col_vec_new(src, kk);
        if (!cell || RAY_IS_ERR(cell)) {
            ray_release(out);
            return cell ? cell : ray_error("oom", NULL);
        }
        cell->len = 0;  /* worker fills in and sets cell->len = kept */
        ray_t* new_out = ray_list_append(out, cell);
        ray_release(cell);
        if (!new_out || RAY_IS_ERR(new_out)) {
            ray_release(out);
            return new_out ? new_out : ray_error("oom", NULL);
        }
        out = new_out;
    }

    topk_par_ctx_t ctx = {
        .base = ray_data(src),
        .src_type = t,
        .has_nulls = (src->attrs & RAY_ATTR_HAS_NULLS) != 0,
        .null_bm = (src->attrs & RAY_ATTR_HAS_NULLS)
                   ? ray_vec_nullmap_bytes(src, NULL, NULL) : NULL,
        .k = k,
        .desc = desc,
        .idx_buf = idx_buf,
        .offsets = offsets,
        .grp_cnt = grp_cnt,
        .out_list = out,
    };

    ray_pool_t* pool = ray_pool_get();
    bool par = pool && n_groups >= 8 && total >= 4096;
    if (par) {
        /* See ray_median_per_group_buf for the rationale on the
         * dispatch_n vs dispatch split. */
        if (n_groups < (1 << 16))
            ray_pool_dispatch_n(pool, topk_per_group_fn, &ctx, (uint32_t)n_groups);
        else
            ray_pool_dispatch(pool, topk_per_group_fn, &ctx, n_groups);
    } else {
        topk_per_group_fn(&ctx, 0, 0, n_groups);
    }

    return out;
}

static ray_t* reduction_i64_result(int64_t val, int8_t out_type) {
    switch (out_type) {
        case RAY_DATE:      return ray_date((int32_t)val);
        case RAY_TIME:      return ray_time(val);
        case RAY_TIMESTAMP: return ray_timestamp(val);
        case RAY_I32:       return ray_i32((int32_t)val);
        case RAY_I16:       return ray_i16((int16_t)val);
        case RAY_U8:        return ray_u8((uint8_t)val);
        default:            return ray_i64(val);
    }
}

static ray_t* reduction_extreme_result(ray_op_t* op, int8_t in_type, bool found,
                                       double fval, int64_t ival) {
    int8_t out_type = op->out_type ? op->out_type : in_type;
    if (!found) return ray_typed_null(-out_type);
    if (out_type == RAY_F64) return ray_f64(fval);
    return reduction_i64_result(ival, out_type);
}

ray_t* exec_reduction(ray_graph_t* g, ray_op_t* op, ray_t* input) {
    if (!input || RAY_IS_ERR(input)) return input;

    /* TABLE input: COUNT returns row count, others need a column */
    if (input->type == RAY_TABLE) {
        if (op->opcode == OP_COUNT)
            return ray_i64(ray_table_nrows(input));
        return ray_error("type", NULL);
    }

    /* Atom input: count returns 1 (or the string length for RAY_STR
     * since strings are length-prefixed atoms).  Other reductions on
     * a scalar are a type error — they need a column. */
    if (ray_is_atom(input)) {
        if (op->opcode == OP_COUNT) {
            if ((-input->type) == RAY_STR)
                return ray_i64((int64_t)ray_str_len(input));
            return ray_i64(1);
        }
        return ray_error("type", NULL);
    }

    int8_t in_type = input->type;
    int64_t len = input->len;

    /* Resolve null bitmap once before dispatching.  ray_vec_nullmap_bytes
     * handles slice / ext / inline / HAS_INDEX uniformly so this works on
     * vectors that carry an attached accelerator index. */
    bool has_nulls = (input->attrs & RAY_ATTR_HAS_NULLS) != 0;
    const uint8_t* null_bm = ray_vec_nullmap_bytes(input, NULL, NULL);

    /* Selection-aware reduction: when a lazy WHERE filter has installed
     * g->selection on the graph and the column we're reducing matches
     * the selection's source-row count, the reduction must walk only
     * the selected rows.  Without this, scalar aggs like
     *   (select {s: (sum v) from: T where: (>= v 500)})
     * silently sum the unfiltered column.  exec_group's by-keyed path
     * already pulls the selection through match_idx — this is the
     * non-grouped twin for OP_SUM/MIN/MAX/AVG/etc. dispatched from
     * exec.c via a vector input (OP_SELECT projection of a scalar agg).
     *
     * We borrow g->selection — the caller (the OP_SUM dispatcher in
     * exec.c, ultimately ray_execute) is responsible for releasing it.
     * Only the materialised index block is freed here. */
    ray_t* sel_idx_block = NULL;
    const int64_t* sel_idx = NULL;
    int64_t scan_n = len;
    if (g && g->selection) {
        ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
        if (sm->nrows == len) {
            sel_idx_block = ray_rowsel_to_indices(g->selection);
            if (!sel_idx_block) return ray_error("oom", NULL);
            sel_idx = (const int64_t*)ray_data(sel_idx_block);
            scan_n = sm->total_pass;
        }
    }

    /* O(1) short-circuit: first/last on numeric columns don't need a
     * full reduction pass.  Non-numeric types (STR, GUID) fall through
     * to the serial reduction path below. */
    if ((op->opcode == OP_FIRST || op->opcode == OP_LAST) &&
        (in_type == RAY_I64 || in_type == RAY_F64 || in_type == RAY_I32 ||
         in_type == RAY_I16 || in_type == RAY_BOOL || in_type == RAY_U8 ||
         in_type == RAY_TIMESTAMP || in_type == RAY_DATE || in_type == RAY_TIME ||
         in_type == RAY_SYM)) {
        int64_t row = -1;
        if (op->opcode == OP_FIRST) {
            for (int64_t i = 0; i < scan_n; i++) {
                int64_t r = sel_idx ? sel_idx[i] : i;
                if (!has_nulls || !((null_bm[r/8] >> (r%8)) & 1)) { row = r; break; }
            }
        } else {
            for (int64_t i = scan_n - 1; i >= 0; i--) {
                int64_t r = sel_idx ? sel_idx[i] : i;
                if (!has_nulls || !((null_bm[r/8] >> (r%8)) & 1)) { row = r; break; }
            }
        }
        if (sel_idx_block) ray_release(sel_idx_block);
        if (row < 0 || row >= len)
            return ray_typed_null(-in_type);
        void* base = ray_data(input);
        if (in_type == RAY_F64) return ray_f64(((const double*)base)[row]);
        return ray_i64(read_col_i64(base, row, in_type, input->attrs));
    }

    ray_pool_t* pool = ray_pool_get();
    if (pool && scan_n >= RAY_PARALLEL_THRESHOLD) {
        uint32_t nw = ray_pool_total_workers(pool);
        ray_t* accs_hdr;
        reduce_acc_t* accs = (reduce_acc_t*)scratch_calloc(&accs_hdr, nw * sizeof(reduce_acc_t));
        if (!accs) { if (sel_idx_block) ray_release(sel_idx_block); return ray_error("oom", NULL); }
        for (uint32_t i = 0; i < nw; i++) reduce_acc_init(&accs[i]);

        par_reduce_ctx_t ctx = { .input = input, .accs = accs,
                                 .has_nulls = has_nulls, .null_bm = null_bm,
                                 .idx = sel_idx };
        ray_pool_dispatch(pool, par_reduce_fn, &ctx, scan_n);

        /* Merge: worker 0 is the base, merge the rest in order */
        reduce_acc_t merged;
        reduce_acc_init(&merged);
        merged = accs[0];
        for (uint32_t i = 1; i < nw; i++) {
            if (!accs[i].has_first) continue;
            reduce_merge(&merged, &accs[i], in_type);
        }
        /* first = accs[first worker with data], last = accs[last worker with data] */
        for (uint32_t i = 0; i < nw; i++) {
            if (accs[i].has_first) {
                if (in_type == RAY_F64) merged.first_f = accs[i].first_f;
                else merged.first_i = accs[i].first_i;
                break;
            }
        }
        for (int32_t i = (int32_t)nw - 1; i >= 0; i--) {
            if (accs[i].has_first) {
                if (in_type == RAY_F64) merged.last_f = accs[i].last_f;
                else merged.last_i = accs[i].last_i;
                break;
            }
        }

        ray_t* result;
        switch (op->opcode) {
            case OP_SUM:   result = in_type == RAY_F64 ? ray_f64(merged.sum_f) : ray_i64(merged.sum_i); break;
            case OP_PROD:  result = in_type == RAY_F64 ? ray_f64(merged.prod_f) : ray_i64(merged.prod_i); break;
            case OP_MIN:   result = reduction_extreme_result(op, in_type, merged.cnt > 0, merged.min_f, merged.min_i); break;
            case OP_MAX:   result = reduction_extreme_result(op, in_type, merged.cnt > 0, merged.max_f, merged.max_i); break;
            /* COUNT returns total length including nulls — matches ray_count_fn's
             * "count all elements" semantics, not SQL's COUNT(col) non-null count. */
            case OP_COUNT: result = ray_i64(scan_n); break;
            case OP_AVG:   result = merged.cnt > 0 ? ray_f64(in_type == RAY_F64 ? merged.sum_f / merged.cnt : (double)merged.sum_i / merged.cnt) : ray_typed_null(-RAY_F64); break;
            case OP_FIRST: result = merged.has_first ? (in_type == RAY_F64 ? ray_f64(merged.first_f) : ray_i64(merged.first_i)) : ray_typed_null(-in_type); break;
            case OP_LAST:  result = merged.has_first ? (in_type == RAY_F64 ? ray_f64(merged.last_f) : ray_i64(merged.last_i)) : ray_typed_null(-in_type); break;
            case OP_VAR: case OP_VAR_POP:
            case OP_STDDEV: case OP_STDDEV_POP: {
                bool insufficient = (op->opcode == OP_VAR || op->opcode == OP_STDDEV) ? merged.cnt <= 1 : merged.cnt <= 0;
                if (insufficient) { result = ray_typed_null(-RAY_F64); break; }
                double mean, var_pop;
                if (in_type == RAY_F64) { mean = merged.sum_f / merged.cnt; var_pop = merged.sum_sq_f / merged.cnt - mean * mean; }
                else { mean = (double)merged.sum_i / merged.cnt; var_pop = (double)merged.sum_sq_i / merged.cnt - mean * mean; }
                if (var_pop < 0) var_pop = 0;
                double val;
                if (op->opcode == OP_VAR_POP) val = var_pop;
                else if (op->opcode == OP_VAR) val = var_pop * merged.cnt / (merged.cnt - 1);
                else if (op->opcode == OP_STDDEV_POP) val = sqrt(var_pop);
                else val = sqrt(var_pop * merged.cnt / (merged.cnt - 1));
                result = ray_f64(val);
                break;
            }
            default:       result = ray_error("nyi", NULL); break;
        }
        scratch_free(accs_hdr);
        if (sel_idx_block) ray_release(sel_idx_block);
        return result;
    }

    reduce_acc_t acc;
    reduce_acc_init(&acc);
    reduce_range(input, 0, scan_n, &acc, has_nulls, null_bm, sel_idx);
    if (sel_idx_block) ray_release(sel_idx_block);

    switch (op->opcode) {
        case OP_SUM:   return in_type == RAY_F64 ? ray_f64(acc.sum_f) : ray_i64(acc.sum_i);
        case OP_PROD:  return in_type == RAY_F64 ? ray_f64(acc.prod_f) : ray_i64(acc.prod_i);
        case OP_MIN:   return reduction_extreme_result(op, in_type, acc.cnt > 0, acc.min_f, acc.min_i);
        case OP_MAX:   return reduction_extreme_result(op, in_type, acc.cnt > 0, acc.max_f, acc.max_i);
        /* COUNT returns total length including nulls — matches ray_count_fn's
         * "count all elements" semantics, not SQL's COUNT(col) non-null count. */
        case OP_COUNT: return ray_i64(scan_n);
        case OP_AVG:   return acc.cnt > 0 ? ray_f64(in_type == RAY_F64 ? acc.sum_f / acc.cnt : (double)acc.sum_i / acc.cnt) : ray_typed_null(-RAY_F64);
        case OP_FIRST: return acc.has_first ? (in_type == RAY_F64 ? ray_f64(acc.first_f) : ray_i64(acc.first_i)) : ray_typed_null(-in_type);
        case OP_LAST:  return acc.has_first ? (in_type == RAY_F64 ? ray_f64(acc.last_f) : ray_i64(acc.last_i)) : ray_typed_null(-in_type);
        case OP_VAR: case OP_VAR_POP:
        case OP_STDDEV: case OP_STDDEV_POP: {
            bool insufficient = (op->opcode == OP_VAR || op->opcode == OP_STDDEV) ? acc.cnt <= 1 : acc.cnt <= 0;
            if (insufficient) return ray_typed_null(-RAY_F64);
            double mean, var_pop;
            if (in_type == RAY_F64) { mean = acc.sum_f / acc.cnt; var_pop = acc.sum_sq_f / acc.cnt - mean * mean; }
            else { mean = (double)acc.sum_i / acc.cnt; var_pop = (double)acc.sum_sq_i / acc.cnt - mean * mean; }
            if (var_pop < 0) var_pop = 0;
            double val;
            if (op->opcode == OP_VAR_POP) val = var_pop;
            else if (op->opcode == OP_VAR) val = var_pop * acc.cnt / (acc.cnt - 1);
            else if (op->opcode == OP_STDDEV_POP) val = sqrt(var_pop);
            else val = sqrt(var_pop * acc.cnt / (acc.cnt - 1));
            return ray_f64(val);
        }
        default:       return ray_error("nyi", NULL);
    }
}

/* ============================================================================
 * Group-by execution — with parallel local hash tables + merge
 * ============================================================================ */


/* Flags controlling which accumulator arrays are allocated */
/* GHT_NEED_* defined in exec_internal.h */

/* ── Row-layout HT ──────────────────────────────────────────────────────
 * Keys + accumulators stored inline in both radix entries and group rows.
 * After phase1 copies data from original columns, phase2 and phase3 never
 * touch column data again — all access is sequential/local.
 * ────────────────────────────────────────────────────────────────────── */

/* ght_layout_t defined in exec_internal.h */

ght_layout_t ght_compute_layout(uint8_t n_keys, uint8_t n_aggs,
                                        ray_t** agg_vecs, uint8_t need_flags,
                                        const uint16_t* agg_ops,
                                        const int8_t* key_types) {
    ght_layout_t ly;
    memset(&ly, 0, sizeof(ly));
    ly.n_keys = n_keys;
    ly.n_aggs = n_aggs;
    ly.need_flags = need_flags;

    /* Mark wide keys (those that don't fit in 8 bytes).  For each
     * wide key, the fat-entry and HT-row key slot stores a source
     * row index; probe/rehash/scatter resolve the actual bytes via
     * group_ht_t.key_data[k].  Currently only RAY_GUID is supported. */
    if (key_types) {
        for (uint8_t k = 0; k < n_keys && k < 8; k++) {
            if (key_types[k] == RAY_GUID) {
                ly.wide_key_mask |= (uint8_t)(1u << k);
                ly.wide_key_esz[k] = 16;
            }
        }
    }

    uint8_t nv = 0;
    for (uint8_t a = 0; a < n_aggs && a < 8; a++) {
        /* OP_MEDIAN / OP_TOP_N / OP_BOT_N reserve no row-layout slot —
         * the column is materialized in agg_vecs[a] but values are not
         * packed into entries or HT rows.  A post-radix pass over
         * row_gid+grp_cnt gathers per-group slices and runs quickselect
         * (median) or a bounded heap (top/bot); see
         * ray_median_per_group_buf / ray_topk_per_group_buf. */
        bool holistic = agg_ops && (agg_ops[a] == OP_MEDIAN ||
                                    agg_ops[a] == OP_TOP_N ||
                                    agg_ops[a] == OP_BOT_N);
        if (holistic) {
            ly.agg_is_holistic |= (uint8_t)(1u << a);
            ly.agg_val_slot[a] = -1;
        } else if (agg_vecs[a]) {
            ly.agg_val_slot[a] = (int8_t)nv;
            if (agg_vecs[a]->type == RAY_F64)
                ly.agg_is_f64 |= (1u << a);
            nv++;
            /* Binary aggregator (OP_PEARSON_CORR): the y-side input
             * occupies the very next slot so phase1 packs (x, y)
             * consecutively.  agg_is_binary bit drives that packing. */
            if (agg_ops && agg_ops[a] == OP_PEARSON_CORR) {
                ly.agg_is_binary |= (uint8_t)(1u << a);
                nv++;
            }
        } else {
            ly.agg_val_slot[a] = -1;
        }
        if (agg_ops) {
            if (agg_ops[a] == OP_FIRST) ly.agg_is_first |= (1u << a);
            if (agg_ops[a] == OP_LAST)  ly.agg_is_last  |= (1u << a);
            if (agg_ops[a] == OP_PROD)  ly.agg_is_prod  |= (1u << a);
        }
    }
    ly.n_agg_vals = nv;
    /* Key region = n_keys*8 + 8-byte null mask slot (stored after last key).
     * The null mask slot holds a bitmap of which keys were null in the source
     * row (bit k = key k is null). Folding this slot into hash/memcmp lets
     * null and 0 form distinct groups. */
    uint16_t key_region = (uint16_t)((uint16_t)n_keys * 8 + 8);
    /* Entry layout: hash | keys | null_mask | agg_vals | [entry_row?]
     * Tail entry_row slot is appended only when any agg is FIRST/LAST,
     * carrying the source-row index needed to merge correctly under
     * work-stealing dispatch (see radix_phase1_fn / accum_from_entry). */
    bool has_first_last = (ly.agg_is_first | ly.agg_is_last) != 0;
    uint16_t entry_tail = has_first_last ? (uint16_t)8 : (uint16_t)0;
    ly.entry_stride = (uint16_t)(8 + key_region + (uint16_t)nv * 8 + entry_tail);

    uint16_t off = (uint16_t)(8 + key_region);
    uint16_t block = (uint16_t)nv * 8;
    if (need_flags & GHT_NEED_SUM)   { ly.off_sum   = off; off += block; }
    if (need_flags & GHT_NEED_MIN)   { ly.off_min   = off; off += block; }
    if (need_flags & GHT_NEED_MAX)   { ly.off_max   = off; off += block; }
    if (need_flags & GHT_NEED_SUMSQ) { ly.off_sumsq = off; off += block; }
    /* Per-slot row-index bounds for FIRST/LAST.  Two int64 blocks of
     * n_agg_vals slots each, allocated only when needed. */
    if (has_first_last) {
        ly.off_first_row = off; off += block;
        ly.off_last_row  = off; off += block;
    }
    /* PEARSON y-side accumulators (Σy, Σy², Σxy).  Allocated when any
     * OP_PEARSON_CORR agg is present.  x-side reuses off_sum + off_sumsq
     * at the same slot index; the y value lives at slot+1 in agg_vals,
     * but its derived accumulators live in their own blocks below. */
    if (need_flags & GHT_NEED_PEARSON) {
        ly.off_sum_y   = off; off += block;
        ly.off_sumsq_y = off; off += block;
        ly.off_sumxy   = off; off += block;
    }
    ly.row_stride = off;
    return ly;
}

/* Packed HT slots: [salt:8 | gid:24] in 4 bytes.
 * Max groups per HT = 16M (24 bits) — ample for partitioned probes.
 * 4B slots halve cache footprint vs 8B, fitting HT in L2. */
#define HT_EMPTY    UINT32_MAX
#define HT_PACK(salt, gid)  (((uint32_t)(uint8_t)(salt) << 24) | ((gid) & 0xFFFFFF))
#define HT_GID(s)   ((s) & 0xFFFFFF)
#define HT_SALT_V(s) ((uint8_t)((s) >> 24))

/* group_ht_t defined in exec_internal.h */

static bool group_ht_init_sized(group_ht_t* ht, uint32_t cap,
                                 const ght_layout_t* ly, uint32_t init_grp_cap) {
    ht->ht_cap = cap;
    ht->oom = 0;
    ht->layout = *ly;
    /* key_data must be populated by the caller via group_ht_set_key_data
     * whenever wide_key_mask != 0. */
    memset(ht->key_data, 0, sizeof(ht->key_data));
    ht->slots = (uint32_t*)scratch_alloc(&ht->_h_slots, (size_t)cap * sizeof(uint32_t));
    if (!ht->slots) return false;
    memset(ht->slots, 0xFF, (size_t)cap * sizeof(uint32_t)); /* HT_EMPTY = all-1s */
    ht->grp_cap = init_grp_cap;
    ht->grp_count = 0;
    ht->rows = (char*)scratch_alloc(&ht->_h_rows,
        (size_t)init_grp_cap * ly->row_stride);
    if (!ht->rows) return false;
    return true;
}

bool group_ht_init(group_ht_t* ht, uint32_t cap, const ght_layout_t* ly) {
    return group_ht_init_sized(ht, cap, ly, 256);
}

/* Populate key_data[k] for wide-key resolution. Called by the HT path
 * right after group_ht_init / group_ht_init_sized when any key is wide. */
static inline void group_ht_set_key_data(group_ht_t* ht, void** kd) {
    uint8_t mask = ht->layout.wide_key_mask;
    if (!mask || !kd) return;
    for (uint8_t k = 0; k < ht->layout.n_keys && k < 8; k++) {
        if (mask & (1u << k)) ht->key_data[k] = kd[k];
    }
}

void group_ht_free(group_ht_t* ht) {
    scratch_free(ht->_h_slots);
    scratch_free(ht->_h_rows);
}

static bool group_ht_grow(group_ht_t* ht) {
    uint32_t old_cap = ht->grp_cap;
    uint32_t new_cap = old_cap * 2;
    uint16_t rs = ht->layout.row_stride;
    char* new_rows = (char*)scratch_realloc(
        &ht->_h_rows, (size_t)old_cap * rs, (size_t)new_cap * rs);
    if (!new_rows) return false;
    ht->rows = new_rows;
    ht->grp_cap = new_cap;
    return true;
}

/* Hash inline int64_t keys (for rehash — resolves wide keys via
 * the HT's key_data pointers). */
static inline uint64_t hash_keys_inline(const int64_t* keys, const int8_t* key_types,
                                         uint8_t n_keys, uint8_t wide_mask,
                                         const uint8_t* wide_esz, void* const* key_data) {
    uint64_t h = 0;
    for (uint8_t k = 0; k < n_keys; k++) {
        uint64_t kh;
        if (wide_mask & (1u << k)) {
            /* Wide key: keys[k] is the source row index. Hash the
             * actual bytes from key_data[k]. */
            int64_t row_idx = keys[k];
            uint8_t esz = wide_esz[k];
            const void* src = (const char*)key_data[k] + (size_t)row_idx * esz;
            kh = ray_hash_bytes(src, esz);
        } else if (key_types[k] == RAY_F64) {
            double dv;
            memcpy(&dv, &keys[k], 8);
            kh = ray_hash_f64(dv);
        } else {
            kh = ray_hash_i64(keys[k]);
        }
        h = (k == 0) ? kh : ray_hash_combine(h, kh);
    }
    /* Fold null mask (slot n_keys) into hash so null/0 form distinct groups */
    int64_t null_mask = keys[n_keys];
    if (null_mask)
        h = ray_hash_combine(h, ray_hash_i64(null_mask));
    return h;
}

static void group_ht_rehash(group_ht_t* ht, const int8_t* key_types) {
    uint32_t new_cap = ht->ht_cap * 2;
    ray_t* new_h = NULL;
    uint32_t* new_slots = (uint32_t*)scratch_alloc(&new_h, (size_t)new_cap * sizeof(uint32_t));
    if (!new_slots) return; /* OOM: keep old HT, it still works (just slower) */
    scratch_free(ht->_h_slots);
    ht->_h_slots = new_h;
    ht->slots = new_slots;
    memset(ht->slots, 0xFF, (size_t)new_cap * sizeof(uint32_t));
    ht->ht_cap = new_cap;
    uint32_t mask = new_cap - 1;
    uint16_t rs = ht->layout.row_stride;
    uint8_t nk = ht->layout.n_keys;
    uint8_t wide = ht->layout.wide_key_mask;
    for (uint32_t gi = 0; gi < ht->grp_count; gi++) {
        const int64_t* row_keys = (const int64_t*)(ht->rows + (size_t)gi * rs + 8);
        uint64_t h = hash_keys_inline(row_keys, key_types, nk, wide,
                                       ht->layout.wide_key_esz, ht->key_data);
        uint32_t slot = (uint32_t)(h & mask);
        while (ht->slots[slot] != HT_EMPTY)
            slot = (slot + 1) & mask;
        ht->slots[slot] = HT_PACK(HT_SALT(h), gi);
    }
}

/* Initialize accumulators for a new group from entry's inline agg values.
 * Each unified block has n_agg_vals slots of 8 bytes, typed by agg_is_f64. */
static inline void init_accum_from_entry(char* row, const char* entry,
                                          const ght_layout_t* ly) {
    uint16_t accum_start = (uint16_t)(8 + ((uint16_t)ly->n_keys + 1) * 8);
    if (ly->row_stride > accum_start)
        memset(row + accum_start, 0, ly->row_stride - accum_start);

    const char* agg_data = entry + 8 + ((size_t)ly->n_keys + 1) * 8;
    uint8_t na = ly->n_aggs;
    uint8_t nf = ly->need_flags;
    bool has_fl = (ly->agg_is_first | ly->agg_is_last) != 0;
    /* Entry tail slot carries the source-row index when has_fl. */
    int64_t entry_row = 0;
    if (has_fl)
        memcpy(&entry_row, entry + ly->entry_stride - 8, 8);

    uint8_t bin_mask = ly->agg_is_binary;
    for (uint8_t a = 0; a < na; a++) {
        int8_t s = ly->agg_val_slot[a];
        if (s < 0) continue;
        /* Copy raw 8 bytes from entry into each enabled accumulator block */
        if (nf & GHT_NEED_SUM) memcpy(row + ly->off_sum + s * 8, agg_data + s * 8, 8);
        if (nf & GHT_NEED_MIN) memcpy(row + ly->off_min + s * 8, agg_data + s * 8, 8);
        if (nf & GHT_NEED_MAX) memcpy(row + ly->off_max + s * 8, agg_data + s * 8, 8);
        if (nf & GHT_NEED_SUMSQ) {
            /* sumsq = v * v for the first entry */
            if (ly->agg_is_f64 & (1u << a)) {
                double v; memcpy(&v, agg_data + s * 8, 8);
                double sq = v * v;
                memcpy(row + ly->off_sumsq + s * 8, &sq, 8);
            } else {
                int64_t v; memcpy(&v, agg_data + s * 8, 8);
                double sq = (double)v * (double)v;
                memcpy(row + ly->off_sumsq + s * 8, &sq, 8);
            }
        }
        /* PEARSON y-side: seed Σy, Σy², Σxy from the (x, y) pair packed
         * at slots (s, s+1).  x-side Σx/Σx² are seeded by the SUM/SUMSQ
         * blocks above (OP_PEARSON_CORR sets both need-flags).  Reads
         * the typed bit-pattern packed by phase1 — F64 stays double,
         * i64 reinterprets and casts. */
        if ((nf & GHT_NEED_PEARSON) && (bin_mask & (1u << a))) {
            double x, y;
            if (ly->agg_is_f64 & (1u << a)) {
                memcpy(&x, agg_data +  s      * 8, 8);
                memcpy(&y, agg_data + (s + 1) * 8, 8);
            } else {
                int64_t xi, yi;
                memcpy(&xi, agg_data +  s      * 8, 8);
                memcpy(&yi, agg_data + (s + 1) * 8, 8);
                x = (double)xi; y = (double)yi;
            }
            memcpy(row + ly->off_sum_y   + s * 8, &y, 8);
            double yy = y * y;
            memcpy(row + ly->off_sumsq_y + s * 8, &yy, 8);
            double xy = x * y;
            memcpy(row + ly->off_sumxy   + s * 8, &xy, 8);
        }
        /* Seed per-slot row-index bounds with the row that opened this
         * group.  Only writes the populated slots; unpopulated slot
         * bytes stay zero from the memset above (harmless — those slots
         * never participate in accum_from_entry's compare/update). */
        if (has_fl) {
            memcpy(row + ly->off_first_row + s * 8, &entry_row, 8);
            memcpy(row + ly->off_last_row  + s * 8, &entry_row, 8);
        }
    }
}

/* Row-layout accessors: cast through void* for strict-aliasing safety.
 * All row offsets are 8-byte aligned by construction. */
/* ROW_RD/WR macros defined in exec_internal.h */

/* Accumulate into existing group from entry's inline agg values */
static inline void accum_from_entry(char* row, const char* entry,
                                     const ght_layout_t* ly) {
    const char* agg_data = entry + 8 + ((size_t)ly->n_keys + 1) * 8;
    uint8_t na = ly->n_aggs;
    uint8_t nf = ly->need_flags;
    /* Entry's source-row index — only present when any agg is FIRST/LAST.
     * Pool dispatch is work-stealing (atomic_fetch_add), so phase1 may
     * scatter entries into radix bufs out of source-row order; reading
     * the row index from the entry restores the absolute ordering that
     * "keep init / always overwrite" assumed. */
    bool has_fl = (ly->agg_is_first | ly->agg_is_last) != 0;
    int64_t entry_row = 0;
    if (has_fl)
        memcpy(&entry_row, entry + ly->entry_stride - 8, 8);

    for (uint8_t a = 0; a < na; a++) {
        int8_t s = ly->agg_val_slot[a];
        if (s < 0) continue;
        const char* val = agg_data + s * 8;

        uint8_t amask = (1u << a);
        bool take_first = false, take_last = false;
        if (has_fl && (ly->agg_is_first & amask)) {
            int64_t fr; memcpy(&fr, row + ly->off_first_row + s * 8, 8);
            take_first = (entry_row < fr);
        }
        if (has_fl && (ly->agg_is_last & amask)) {
            int64_t lr; memcpy(&lr, row + ly->off_last_row + s * 8, 8);
            take_last = (entry_row > lr);
        }
        if (ly->agg_is_f64 & amask) {
            double v;
            memcpy(&v, val, 8);
            if (nf & GHT_NEED_SUM) {
                if (ly->agg_is_first & amask) { if (take_first) memcpy(row + ly->off_sum + s * 8, val, 8); }
                else if (ly->agg_is_last & amask) { if (take_last) memcpy(row + ly->off_sum + s * 8, val, 8); }
                else if (ly->agg_is_prod & amask) { ROW_WR_F64(row, ly->off_sum, s) *= v; }
                else { ROW_WR_F64(row, ly->off_sum, s) += v; }
            }
            if (nf & GHT_NEED_MIN) { double* p = &ROW_WR_F64(row, ly->off_min, s); if (v < *p) *p = v; }
            if (nf & GHT_NEED_MAX) { double* p = &ROW_WR_F64(row, ly->off_max, s); if (v > *p) *p = v; }
            if (nf & GHT_NEED_SUMSQ) { ROW_WR_F64(row, ly->off_sumsq, s) += v * v; }
            /* PEARSON y-side: accumulate Σy, Σy², Σxy.  v above is x. */
            if ((nf & GHT_NEED_PEARSON) && (ly->agg_is_binary & amask)) {
                double y;
                memcpy(&y, agg_data + (s + 1) * 8, 8);
                ROW_WR_F64(row, ly->off_sum_y,   s) += y;
                ROW_WR_F64(row, ly->off_sumsq_y, s) += y * y;
                ROW_WR_F64(row, ly->off_sumxy,   s) += v * y;
            }
        } else {
            int64_t v;
            memcpy(&v, val, 8);
            if (nf & GHT_NEED_SUM) {
                if (ly->agg_is_first & amask) { if (take_first) memcpy(row + ly->off_sum + s * 8, val, 8); }
                else if (ly->agg_is_last & amask) { if (take_last) memcpy(row + ly->off_sum + s * 8, val, 8); }
                else if (ly->agg_is_prod & amask) { ROW_WR_I64(row, ly->off_sum, s) = (int64_t)((uint64_t)ROW_RD_I64(row, ly->off_sum, s) * (uint64_t)v); }
                else { ROW_WR_I64(row, ly->off_sum, s) += v; }
            }
            if (nf & GHT_NEED_MIN) { int64_t* p = &ROW_WR_I64(row, ly->off_min, s); if (v < *p) *p = v; }
            if (nf & GHT_NEED_MAX) { int64_t* p = &ROW_WR_I64(row, ly->off_max, s); if (v > *p) *p = v; }
            if (nf & GHT_NEED_SUMSQ) { ROW_WR_F64(row, ly->off_sumsq, s) += (double)v * (double)v; }
            /* PEARSON y-side (i64 input branch): y was packed via
             * read_col_i64 — reinterpret as int64 then cast to double. */
            if ((nf & GHT_NEED_PEARSON) && (ly->agg_is_binary & amask)) {
                int64_t yi; memcpy(&yi, agg_data + (s + 1) * 8, 8);
                double y  = (double)yi;
                double xd = (double)v;
                ROW_WR_F64(row, ly->off_sum_y,   s) += y;
                ROW_WR_F64(row, ly->off_sumsq_y, s) += y * y;
                ROW_WR_F64(row, ly->off_sumxy,   s) += xd * y;
            }
        }
        /* Commit row-index bounds after value writes so a later entry in
         * the same merge sees the updated bound. */
        if (take_first) memcpy(row + ly->off_first_row + s * 8, &entry_row, 8);
        if (take_last)  memcpy(row + ly->off_last_row  + s * 8, &entry_row, 8);
    }
}

/* Compare the n_keys key slots of two rows, handling wide keys via
 * key_data[] resolution.  Returns true if all keys are bytewise equal.
 * Hot path: when wide_mask == 0, reduces to a single memcmp over the
 * packed 8-byte-per-key region. */
static inline bool group_keys_equal(const int64_t* a_keys, const int64_t* b_keys,
                                      const ght_layout_t* ly, void* const* key_data) {
    uint8_t wide = ly->wide_key_mask;
    uint8_t nk = ly->n_keys;
    if (wide == 0) {
        /* memcmp covers nk values + trailing 8-byte null mask slot */
        return memcmp(a_keys, b_keys, (size_t)(nk + 1) * 8) == 0;
    }
    for (uint8_t k = 0; k < nk; k++) {
        if (wide & (1u << k)) {
            int64_t ra = a_keys[k];
            int64_t rb = b_keys[k];
            if (ra == rb) continue;  /* same source row - trivially equal */
            uint8_t esz = ly->wide_key_esz[k];
            const char* base = (const char*)key_data[k];
            if (memcmp(base + (size_t)ra * esz,
                       base + (size_t)rb * esz, esz) != 0) return false;
        } else {
            if (a_keys[k] != b_keys[k]) return false;
        }
    }
    /* Null mask slot must match too */
    if (a_keys[nk] != b_keys[nk]) return false;
    return true;
}

/* Probe + accumulate a single fat entry into the HT. Returns updated mask. */
static inline uint32_t group_probe_entry(group_ht_t* ht,
    const char* entry, const int8_t* key_types, uint32_t mask) {
    const ght_layout_t* ly = &ht->layout;
    uint64_t hash = *(const uint64_t*)entry;
    const char* ekeys = entry + 8;
    uint8_t salt = HT_SALT(hash);
    uint32_t slot = (uint32_t)(hash & mask);
    uint16_t key_bytes = (uint16_t)((ly->n_keys + 1) * 8);

    for (;;) {
        uint32_t sv = ht->slots[slot];
        if (sv == HT_EMPTY) {
            /* New group */
            if (ht->grp_count >= ht->grp_cap) {
                if (!group_ht_grow(ht)) { ht->oom = 1; return mask; }
            }
            uint32_t gid = ht->grp_count++;
            char* row = ht->rows + (size_t)gid * ly->row_stride;
            *(int64_t*)row = 1;   /* count = 1 */
            memcpy(row + 8, ekeys, key_bytes);
            init_accum_from_entry(row, entry, ly);
            ht->slots[slot] = HT_PACK(salt, gid);
            if (ht->grp_count * 2 > ht->ht_cap) {
                group_ht_rehash(ht, key_types);
                mask = ht->ht_cap - 1;
            }
            return mask;
        }
        if (HT_SALT_V(sv) == salt) {
            uint32_t gid = HT_GID(sv);
            char* row = ht->rows + (size_t)gid * ly->row_stride;
            if (group_keys_equal((const int64_t*)(row + 8),
                                  (const int64_t*)ekeys, ly, ht->key_data)) {
                (*(int64_t*)row)++;   /* count++ */
                accum_from_entry(row, entry, ly);
                return mask;
            }
        }
        slot = (slot + 1) & mask;
    }
}

static bool group_ht_insert_empty_group(group_ht_t* ht, const int64_t* keys,
                                        const int8_t* key_types) {
    const ght_layout_t* ly = &ht->layout;
    if (ht->grp_count >= ht->grp_cap) {
        if (!group_ht_grow(ht)) { ht->oom = 1; return false; }
    }
    uint64_t h = hash_keys_inline(keys, key_types, ly->n_keys,
                                  ly->wide_key_mask, ly->wide_key_esz,
                                  ht->key_data);
    uint32_t mask = ht->ht_cap - 1;
    uint32_t slot = (uint32_t)(h & mask);
    uint8_t salt = HT_SALT(h);
    while (ht->slots[slot] != HT_EMPTY)
        slot = (slot + 1) & mask;

    uint32_t gid = ht->grp_count++;
    char* row = ht->rows + (size_t)gid * ly->row_stride;
    memset(row, 0, ly->row_stride);
    memcpy(row + 8, keys, (size_t)(ly->n_keys + 1) * 8);
    ht->slots[slot] = HT_PACK(salt, gid);
    if (ht->grp_count * 2 > ht->ht_cap) {
        group_ht_rehash(ht, key_types);
        if (ht->oom) return false;
    }
    return true;
}

static inline void group_probe_existing_entry(group_ht_t* ht,
    const char* entry, const int8_t* key_types) {
    const ght_layout_t* ly = &ht->layout;
    uint64_t hash = *(const uint64_t*)entry;
    const char* ekeys = entry + 8;
    uint8_t salt = HT_SALT(hash);
    uint32_t mask = ht->ht_cap - 1;
    uint32_t slot = (uint32_t)(hash & mask);

    for (;;) {
        uint32_t sv = ht->slots[slot];
        if (sv == HT_EMPTY) return;
        if (HT_SALT_V(sv) == salt) {
            uint32_t gid = HT_GID(sv);
            char* row = ht->rows + (size_t)gid * ly->row_stride;
            if (group_keys_equal((const int64_t*)(row + 8),
                                  (const int64_t*)ekeys, ly, ht->key_data)) {
                (*(int64_t*)row)++;
                accum_from_entry(row, entry, ly);
                return;
            }
        }
        slot = (slot + 1) & mask;
    }
}

/* Process rows [start, end) from original columns into a local hash table.
 * Converts each row to a fat entry on the stack, then probes. */
#define GROUP_PREFETCH_BATCH 16

static inline int64_t group_strlen_at(const ray_t* col, int64_t row);

static inline bool group_rowsel_pass(ray_t* sel, int64_t row) {
    if (!sel) return true;
    ray_rowsel_t* m = ray_rowsel_meta(sel);
    if (row < 0 || row >= m->nrows) return false;
    uint32_t seg = (uint32_t)(row / RAY_MORSEL_ELEMS);
    uint8_t f = ray_rowsel_flags(sel)[seg];
    if (f == RAY_SEL_ALL) return true;
    if (f == RAY_SEL_NONE) return false;
    uint16_t local = (uint16_t)(row - (int64_t)seg * RAY_MORSEL_ELEMS);
    uint32_t lo = ray_rowsel_offsets(sel)[seg];
    uint32_t hi = ray_rowsel_offsets(sel)[seg + 1];
    const uint16_t* idx = ray_rowsel_idx(sel);
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        uint16_t v = idx[mid];
        if (v == local) return true;
        if (v < local) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

void group_rows_range(group_ht_t* ht, void** key_data, int8_t* key_types,
                              uint8_t* key_attrs, ray_t** key_vecs, ray_t** agg_vecs,
                              ray_t** agg_vecs2,
                              uint8_t* agg_strlen,
                              ray_t* rowsel,
                              int64_t start, int64_t end,
                              const int64_t* match_idx) {
    const ght_layout_t* ly = &ht->layout;
    uint8_t nk = ly->n_keys;
    uint8_t na = ly->n_aggs;
    uint8_t wide = ly->wide_key_mask;
    bool has_fl = (ly->agg_is_first | ly->agg_is_last) != 0;
    uint32_t mask = ht->ht_cap - 1;
    /* Stack buffer for one entry: hash + (nk+1) key slots + nv agg_vals
     * + optional 8-byte source-row tail (FIRST/LAST).
     * Max size: 8 + 9*8 + 8*8 + 8 = 152 bytes. */
    char ebuf[8 + 9 * 8 + 8 * 8 + 8];

    /* Check which key columns can produce nulls (parent vec's HAS_NULLS
     * attr for slices) — skips per-row null checks on the fast path. */
    uint8_t nullable_mask = 0;
    for (uint8_t k = 0; k < nk; k++) {
        if (!key_vecs || !key_vecs[k]) continue;
        ray_t* kv = key_vecs[k];
        ray_t* src = (kv->attrs & RAY_ATTR_SLICE) ? kv->slice_parent : kv;
        if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
            nullable_mask |= (uint8_t)(1u << k);
    }

    /* Wire the HT's key_data pointer table so probe/rehash can
     * resolve wide keys via the source columns. */
    if (wide) group_ht_set_key_data(ht, key_data);

    for (int64_t i = start; i < end; i++) {
        /* Cancellation checkpoint every 65536 rows — ~150 polls on a
         * 10M-row ingest, imperceptible in the inner loop and still
         * sub-100ms response time on Ctrl-C. */
        if (((i - start) & 65535) == 0 && ray_interrupted()) break;
        int64_t row = match_idx ? match_idx[i] : i;
        if (!match_idx && rowsel && !group_rowsel_pass(rowsel, row)) continue;
        uint64_t h = 0;
        int64_t* ek = (int64_t*)(ebuf + 8);
        int64_t null_mask = 0;
        for (uint8_t k = 0; k < nk; k++) {
            int8_t t = key_types[k];
            uint64_t kh;
            bool is_null = (nullable_mask & (1u << k))
                           && ray_vec_is_null(key_vecs[k], row);
            if (is_null) {
                null_mask |= (int64_t)(1u << k);
                ek[k] = 0;  /* canonical null value — real 0 differs via null_mask */
                kh = ray_hash_i64(0);
            } else if (wide & (1u << k)) {
                /* Wide key: store source row index, hash the actual bytes. */
                uint8_t esz = ly->wide_key_esz[k];
                const void* src = (const char*)key_data[k] + (size_t)row * esz;
                ek[k] = row;
                kh = ray_hash_bytes(src, esz);
            } else if (t == RAY_F64) {
                int64_t kv;
                memcpy(&kv, &((double*)key_data[k])[row], 8);
                ek[k] = kv;
                kh = ray_hash_f64(((double*)key_data[k])[row]);
            } else {
                int64_t kv = read_col_i64(key_data[k], row, t, key_attrs[k]);
                ek[k] = kv;
                kh = ray_hash_i64(kv);
            }
            h = (k == 0) ? kh : ray_hash_combine(h, kh);
        }
        ek[nk] = null_mask;
        if (null_mask) h = ray_hash_combine(h, ray_hash_i64(null_mask));
        *(uint64_t*)ebuf = h;

        int64_t* ev = (int64_t*)(ebuf + 8 + ((size_t)nk + 1) * 8);
        uint8_t vi = 0;
        uint8_t bin_mask = ly->agg_is_binary;
        uint8_t hol_mask = ly->agg_is_holistic;
        for (uint8_t a = 0; a < na; a++) {
            /* Holistic agg (OP_MEDIAN): no slot reserved — skip packing.
             * Source column read in the post-radix pass. */
            if (hol_mask & (1u << a)) continue;
            ray_t* ac = agg_vecs[a];
            if (!ac) continue;
            if (agg_strlen && agg_strlen[a])
                ev[vi] = group_strlen_at(ac, row);
            else if (ac->type == RAY_F64)
                memcpy(&ev[vi], &((double*)ray_data(ac))[row], 8);
            else
                ev[vi] = read_col_i64(ray_data(ac), row, ac->type, ac->attrs);
            vi++;
            /* Binary aggregator: pack y after x in the same entry. */
            if ((bin_mask & (1u << a)) && agg_vecs2 && agg_vecs2[a]) {
                ray_t* ay = agg_vecs2[a];
                if (ay->type == RAY_F64)
                    memcpy(&ev[vi], &((double*)ray_data(ay))[row], 8);
                else
                    ev[vi] = read_col_i64(ray_data(ay), row, ay->type, ay->attrs);
                vi++;
            }
        }
        /* Tail slot: source row index for FIRST/LAST tie-breaking.  Same
         * layout as the radix path's entries so accum_from_entry can read
         * it from the same offset. */
        if (has_fl)
            memcpy(ebuf + ly->entry_stride - 8, &row, 8);

        mask = group_probe_entry(ht, ebuf, key_types, mask);
    }
}

static void group_rows_range_existing(group_ht_t* ht, void** key_data,
                                      int8_t* key_types, uint8_t* key_attrs,
                                      ray_t** key_vecs, ray_t** agg_vecs,
                                      uint8_t* agg_strlen, ray_t* rowsel,
                                      int64_t start, int64_t end,
                                      const int64_t* match_idx) {
    const ght_layout_t* ly = &ht->layout;
    uint8_t nk = ly->n_keys;
    uint8_t na = ly->n_aggs;
    uint8_t wide = ly->wide_key_mask;
    bool has_fl = (ly->agg_is_first | ly->agg_is_last) != 0;
    char ebuf[8 + 9 * 8 + 8 * 8 + 8];

    uint8_t nullable_mask = 0;
    for (uint8_t k = 0; k < nk; k++) {
        if (!key_vecs || !key_vecs[k]) continue;
        ray_t* kv = key_vecs[k];
        ray_t* src = (kv->attrs & RAY_ATTR_SLICE) ? kv->slice_parent : kv;
        if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
            nullable_mask |= (uint8_t)(1u << k);
    }

    if (wide) group_ht_set_key_data(ht, key_data);

    for (int64_t i = start; i < end; i++) {
        if (((i - start) & 65535) == 0 && ray_interrupted()) break;
        int64_t row = match_idx ? match_idx[i] : i;
        if (!match_idx && rowsel && !group_rowsel_pass(rowsel, row)) continue;
        uint64_t h = 0;
        int64_t* ek = (int64_t*)(ebuf + 8);
        int64_t null_mask = 0;
        for (uint8_t k = 0; k < nk; k++) {
            int8_t t = key_types[k];
            uint64_t kh;
            bool is_null = (nullable_mask & (1u << k))
                           && ray_vec_is_null(key_vecs[k], row);
            if (is_null) {
                null_mask |= (int64_t)(1u << k);
                ek[k] = 0;
                kh = ray_hash_i64(0);
            } else if (wide & (1u << k)) {
                uint8_t esz = ly->wide_key_esz[k];
                const void* src = (const char*)key_data[k] + (size_t)row * esz;
                ek[k] = row;
                kh = ray_hash_bytes(src, esz);
            } else if (t == RAY_F64) {
                int64_t kv;
                memcpy(&kv, &((double*)key_data[k])[row], 8);
                ek[k] = kv;
                kh = ray_hash_f64(((double*)key_data[k])[row]);
            } else {
                int64_t kv = read_col_i64(key_data[k], row, t, key_attrs[k]);
                ek[k] = kv;
                kh = ray_hash_i64(kv);
            }
            h = (k == 0) ? kh : ray_hash_combine(h, kh);
        }
        ek[nk] = null_mask;
        if (null_mask) h = ray_hash_combine(h, ray_hash_i64(null_mask));
        *(uint64_t*)ebuf = h;

        int64_t* ev = (int64_t*)(ebuf + 8 + ((size_t)nk + 1) * 8);
        uint8_t vi = 0;
        for (uint8_t a = 0; a < na; a++) {
            ray_t* ac = agg_vecs[a];
            if (!ac) continue;
            if (agg_strlen && agg_strlen[a])
                ev[vi] = group_strlen_at(ac, row);
            else if (ac->type == RAY_F64)
                memcpy(&ev[vi], &((double*)ray_data(ac))[row], 8);
            else
                ev[vi] = read_col_i64(ray_data(ac), row, ac->type, ac->attrs);
            vi++;
        }
        if (has_fl)
            memcpy(ebuf + ly->entry_stride - 8, &row, 8);

        group_probe_existing_entry(ht, ebuf, key_types);
    }
}

/* ============================================================================
 * Radix-partitioned parallel group-by
 *
 * Phase 1 (parallel): Each worker reads keys+agg values from original columns,
 *         packs into fat entries (hash, keys, agg_vals), scatters into
 *         thread-local per-partition buffers.
 * Phase 2 (parallel): Each partition is aggregated independently using
 *         inline data — no original column access needed.
 * Phase 3: Build result columns from inline group rows.
 * ============================================================================ */

#define RADIX_BITS  8
#define RADIX_P     (1u << RADIX_BITS)   /* 256 partitions */
#define RADIX_MASK  (RADIX_P - 1)
#define RADIX_PART(h) (((uint32_t)((h) >> 16)) & RADIX_MASK)

/* Per-worker, per-partition buffer of fat entries */
typedef struct {
    char*    data;           /* flat buffer: data[i * entry_stride] */
    uint32_t count;
    uint32_t cap;
    bool     oom;            /* set on realloc failure */
    ray_t*    _hdr;
} radix_buf_t;

static inline void radix_buf_push(radix_buf_t* buf, uint16_t entry_stride,
                                   uint64_t hash, const int64_t* keys, uint8_t n_keys,
                                   int64_t null_mask,
                                   const int64_t* agg_vals, uint8_t n_agg_vals,
                                   bool has_first_last, int64_t row) {
    if (__builtin_expect(buf->count >= buf->cap, 0)) {
        uint32_t old_cap = buf->cap;
        uint32_t new_cap = old_cap * 2;
        char* new_data = (char*)scratch_realloc(
            &buf->_hdr, (size_t)old_cap * entry_stride,
            (size_t)new_cap * entry_stride);
        if (!new_data) { buf->oom = true; return; }
        buf->data = new_data;
        buf->cap = new_cap;
    }
    char* dst = buf->data + (size_t)buf->count * entry_stride;
    *(uint64_t*)dst = hash;
    memcpy(dst + 8, keys, (size_t)n_keys * 8);
    /* Null mask slot sits right after the keys */
    memcpy(dst + 8 + (size_t)n_keys * 8, &null_mask, 8);
    if (n_agg_vals)
        memcpy(dst + 8 + ((size_t)n_keys + 1) * 8, agg_vals, (size_t)n_agg_vals * 8);
    /* Tail slot: source row index for FIRST/LAST tie-breaking. */
    if (has_first_last)
        memcpy(dst + entry_stride - 8, &row, 8);
    buf->count++;
}

typedef struct {
    void**       key_data;
    int8_t*      key_types;
    uint8_t*     key_attrs;
    ray_t**      key_vecs;
    uint8_t      nullable_mask;   /* bit k = key k column may contain nulls */
    ray_t**       agg_vecs;
    /* Second input column per agg; NULL when no binary aggs in this
     * OP_GROUP.  Phase 1 reads agg_vecs2[a] alongside agg_vecs[a] and
     * packs (x, y) consecutively into the entry agg_vals area for any
     * agg whose layout bit agg_is_binary is set. */
    ray_t**       agg_vecs2;
    uint8_t*     agg_strlen;
    uint32_t     n_workers;
    radix_buf_t* bufs;        /* [n_workers * RADIX_P] */
    ght_layout_t layout;
    ray_t* rowsel;
    /* When non-NULL, workers iterate match_idx[start..end) and
     * read row=match_idx[i].  When NULL, row=i. */
    const int64_t* match_idx;
} radix_phase1_ctx_t;

static void radix_phase1_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    radix_phase1_ctx_t* c = (radix_phase1_ctx_t*)ctx;
    const ght_layout_t* ly = &c->layout;
    radix_buf_t* my_bufs = &c->bufs[(size_t)worker_id * RADIX_P];
    uint8_t nk = ly->n_keys;
    uint8_t na = ly->n_aggs;
    uint8_t nv = ly->n_agg_vals;
    uint8_t wide = ly->wide_key_mask;
    uint16_t estride = ly->entry_stride;
    bool has_fl = (ly->agg_is_first | ly->agg_is_last) != 0;
    const int64_t* match_idx = c->match_idx;

    int64_t keys[8];
    int64_t agg_vals[8];

    uint8_t nullable = c->nullable_mask;
    for (int64_t i = start; i < end; i++) {
        /* Cancellation checkpoint every 65536 rows — ~150 polls on a
         * 10M-row ingest, imperceptible in the inner loop and still
         * sub-100ms response time on Ctrl-C. */
        if (((i - start) & 65535) == 0 && ray_interrupted()) break;
        int64_t row = match_idx ? match_idx[i] : i;
        if (!match_idx && c->rowsel && !group_rowsel_pass(c->rowsel, row)) continue;
        uint64_t h = 0;
        int64_t null_mask = 0;
        for (uint8_t k = 0; k < nk; k++) {
            int8_t t = c->key_types[k];
            uint64_t kh;
            bool is_null = (nullable & (1u << k))
                           && ray_vec_is_null(c->key_vecs[k], row);
            if (is_null) {
                null_mask |= (int64_t)(1u << k);
                keys[k] = 0;
                kh = ray_hash_i64(0);
            } else if (wide & (1u << k)) {
                uint8_t esz = ly->wide_key_esz[k];
                const void* src = (const char*)c->key_data[k] + (size_t)row * esz;
                keys[k] = row;
                kh = ray_hash_bytes(src, esz);
            } else if (t == RAY_F64) {
                int64_t kv;
                memcpy(&kv, &((double*)c->key_data[k])[row], 8);
                keys[k] = kv;
                kh = ray_hash_f64(((double*)c->key_data[k])[row]);
            } else {
                int64_t kv = read_col_i64(c->key_data[k], row, t, c->key_attrs[k]);
                keys[k] = kv;
                kh = ray_hash_i64(kv);
            }
            h = (k == 0) ? kh : ray_hash_combine(h, kh);
        }
        if (null_mask) h = ray_hash_combine(h, ray_hash_i64(null_mask));

        uint8_t vi = 0;
        uint8_t bin_mask = ly->agg_is_binary;
        uint8_t hol_mask = ly->agg_is_holistic;
        for (uint8_t a = 0; a < na; a++) {
            /* Holistic agg (OP_MEDIAN): no slot reserved — skip
             * packing.  Source column is read in the post-radix pass. */
            if (hol_mask & (1u << a)) continue;
            ray_t* ac = c->agg_vecs[a];
            if (!ac) continue;
            if (c->agg_strlen && c->agg_strlen[a])
                agg_vals[vi] = group_strlen_at(ac, row);
            else if (ac->type == RAY_F64)
                memcpy(&agg_vals[vi], &((double*)ray_data(ac))[row], 8);
            else
                agg_vals[vi] = read_col_i64(ray_data(ac), row, ac->type, ac->attrs);
            vi++;
            /* Binary aggregator: read y-side value into the next slot.
             * Cast non-F64 inputs through read_col_i64 — pearson_corr's
             * finalize reads both slots as F64 doubles regardless of
             * input type (i64 will be reinterpreted; for now we only
             * support F64 inputs cleanly — i64 path is a perf followup). */
            if ((bin_mask & (1u << a)) && c->agg_vecs2 && c->agg_vecs2[a]) {
                ray_t* ay = c->agg_vecs2[a];
                if (ay->type == RAY_F64)
                    memcpy(&agg_vals[vi], &((double*)ray_data(ay))[row], 8);
                else
                    agg_vals[vi] = read_col_i64(ray_data(ay), row, ay->type, ay->attrs);
                vi++;
            }
        }

        uint32_t part = RADIX_PART(h);
        radix_buf_push(&my_bufs[part], estride, h, keys, nk, null_mask,
                       agg_vals, nv, has_fl, row);
    }
}

/* Process pre-partitioned fat entries into an HT with prefetch batching.
 * Two-phase prefetch: (1) prefetch HT slots, (2) prefetch group rows. */
static void group_rows_indirect(group_ht_t* ht, const int8_t* key_types,
                                 const char* entries, uint32_t n_entries,
                                 uint16_t entry_stride) {
    uint32_t mask = ht->ht_cap - 1;
    /* Stride-ahead prefetch: prefetch HT slot for entry i+D while processing i.
     * D=8 covers ~200ns L2/L3 latency at ~25ns per probe iteration. */
    enum { PF_DIST = 8 };
    /* Prime the prefetch pipeline */
    uint32_t pf_end = (n_entries < PF_DIST) ? n_entries : PF_DIST;
    for (uint32_t j = 0; j < pf_end; j++) {
        uint64_t h = *(const uint64_t*)(entries + (size_t)j * entry_stride);
        __builtin_prefetch(&ht->slots[(uint32_t)(h & mask)], 0, 1);
    }
    for (uint32_t i = 0; i < n_entries; i++) {
        /* Prefetch PF_DIST entries ahead */
        if (i + PF_DIST < n_entries) {
            uint64_t h = *(const uint64_t*)(entries + (size_t)(i + PF_DIST) * entry_stride);
            __builtin_prefetch(&ht->slots[(uint32_t)(h & mask)], 0, 1);
        }
        const char* e = entries + (size_t)i * entry_stride;
        mask = group_probe_entry(ht, e, key_types, mask);
    }
}

/* Phase 3: build result columns from inline group rows */
typedef struct {
    int8_t  out_type;
    bool    src_f64;
    uint16_t agg_op;
    bool    affine;
    double  bias_f64;
    int64_t bias_i64;
    void*   dst;
    ray_t*  vec;
} agg_out_t;

/* Aliases for shared parallel null helpers from internal.h */
#define grp_set_null       par_set_null
#define grp_prepare_nullmap par_prepare_nullmap
#define grp_finalize_nulls par_finalize_nulls

typedef struct {
    group_ht_t*   part_hts;
    uint32_t*     part_offsets;
    char**        key_dsts;
    int8_t*       key_types;
    uint8_t*      key_attrs;
    uint8_t*      key_esizes;
    ray_t**       key_cols;       /* [n_keys] output key vecs (for null bit writes) */
    uint8_t       n_keys;
    agg_out_t*    agg_outs;
    uint8_t       n_aggs;
    /* For wide-key columns (RAY_GUID), the stored key slot is a
     * source row index and we copy the actual bytes from the source
     * column here during the result scatter. */
    void**        key_src_data;   /* [n_keys]; NULL entry if not wide */
} radix_phase3_ctx_t;

static void radix_phase3_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    radix_phase3_ctx_t* c = (radix_phase3_ctx_t*)ctx;
    uint8_t nk = c->n_keys;
    uint8_t na = c->n_aggs;

    for (int64_t p = start; p < end; p++) {
        group_ht_t* ph = &c->part_hts[p];
        uint32_t gc = ph->grp_count;
        if (gc == 0) continue;
        uint32_t off = c->part_offsets[p];
        const ght_layout_t* ly = &ph->layout;
        uint16_t rs = ly->row_stride;

        /* Single pass over group rows: read each row once, scatter keys + aggs.
         * Reduces memory traffic from nk+na passes over group data to 1 pass. */
        for (uint32_t gi = 0; gi < gc; gi++) {
            const char* row = ph->rows + (size_t)gi * rs;
            const int64_t* rkeys = (const int64_t*)(const void*)(row + 8);
            int64_t cnt = *(const int64_t*)(const void*)row;
            int64_t null_mask = rkeys[nk];
            uint32_t di = off + gi;

            /* Scatter keys to result columns */
            for (uint8_t k = 0; k < nk; k++) {
                if (null_mask & (int64_t)(1u << k)) {
                    if (c->key_cols && c->key_cols[k])
                        grp_set_null(c->key_cols[k], di);
                    continue;
                }
                int64_t kv = rkeys[k];
                int8_t kt = c->key_types[k];
                char* dst = c->key_dsts[k];
                uint8_t esz = c->key_esizes[k];
                size_t doff = (size_t)di * esz;
                if (ly->wide_key_mask & (1u << k)) {
                    /* Wide key: kv is the source row index; copy the
                     * bytes from the source column into the output. */
                    const char* src = (const char*)c->key_src_data[k];
                    memcpy(dst + doff, src + (size_t)kv * esz, esz);
                } else if (kt == RAY_F64) {
                    memcpy(dst + doff, &kv, 8);
                } else {
                    write_col_i64(dst, di, kv, kt, c->key_attrs[k]);
                }
            }

            /* Scatter agg results to result columns */
            for (uint8_t a = 0; a < na; a++) {
                /* Holistic aggs (OP_MEDIAN) are filled by the
                 * post-radix pass — skip emitting from the row layout. */
                if (ly->agg_is_holistic & (1u << a)) continue;
                agg_out_t* ao = &c->agg_outs[a];
                if (!ao->dst) continue; /* allocation failed (OOM) */
                uint16_t op = ao->agg_op;
                bool sf = ao->src_f64;
                int8_t s = ly->agg_val_slot[a];
                if (ao->out_type == RAY_F64) {
                    double v;
                    switch (op) {
                        case OP_SUM:
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                            if (ao->affine) v += ao->bias_f64 * cnt;
                            break;
                        case OP_PROD:
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                            break;
                        case OP_AVG:
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s) / cnt
                                   : (double)ROW_RD_I64(row, ly->off_sum, s) / cnt;
                            if (ao->affine) v += ao->bias_f64;
                            break;
                        case OP_MIN:
                            v = sf ? ROW_RD_F64(row, ly->off_min, s)
                                   : (double)ROW_RD_I64(row, ly->off_min, s);
                            break;
                        case OP_MAX:
                            v = sf ? ROW_RD_F64(row, ly->off_max, s)
                                   : (double)ROW_RD_I64(row, ly->off_max, s);
                            break;
                        case OP_FIRST: case OP_LAST:
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                            break;
                        case OP_VAR: case OP_VAR_POP:
                        case OP_STDDEV: case OP_STDDEV_POP: {
                            bool insuf = (op == OP_VAR || op == OP_STDDEV) ? cnt <= 1 : cnt <= 0;
                            if (insuf) { v = 0.0; grp_set_null(ao->vec, di); break; }
                            double sum_val = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                                : (double)ROW_RD_I64(row, ly->off_sum, s);
                            double sq_val = ly->off_sumsq ? ROW_RD_F64(row, ly->off_sumsq, s) : 0.0;
                            double mean = sum_val / cnt;
                            double var_pop = sq_val / cnt - mean * mean;
                            if (var_pop < 0) var_pop = 0;
                            if (op == OP_VAR_POP) v = var_pop;
                            else if (op == OP_VAR) v = var_pop * cnt / (cnt - 1);
                            else if (op == OP_STDDEV_POP) v = sqrt(var_pop);
                            else v = sqrt(var_pop * cnt / (cnt - 1));
                            break;
                        }
                        case OP_PEARSON_CORR: {
                            /* Single-pass formula (same as ray_pearson_corr_fn):
                             *   r = (n·Σxy − Σx·Σy) /
                             *       sqrt((n·Σx² − Σx²)(n·Σy² − Σy²))
                             * Undefined for n<2 or constant side → emit
                             * NaN (canonicalize folds to null upstream). */
                            if (cnt < 2) { v = 0.0; grp_set_null(ao->vec, di); break; }
                            double sx  = sf ? ROW_RD_F64(row, ly->off_sum,    s)
                                            : (double)ROW_RD_I64(row, ly->off_sum, s);
                            double sxx = ly->off_sumsq ? ROW_RD_F64(row, ly->off_sumsq, s) : 0.0;
                            double sy  = ly->off_sum_y   ? ROW_RD_F64(row, ly->off_sum_y,   s) : 0.0;
                            double syy = ly->off_sumsq_y ? ROW_RD_F64(row, ly->off_sumsq_y, s) : 0.0;
                            double sxy = ly->off_sumxy   ? ROW_RD_F64(row, ly->off_sumxy,   s) : 0.0;
                            double dn  = (double)cnt;
                            double num = dn * sxy - sx * sy;
                            double dx  = dn * sxx - sx * sx;
                            double dy  = dn * syy - sy * sy;
                            if (dx <= 0.0 || dy <= 0.0) { v = NAN; break; }
                            v = num / sqrt(dx * dy);
                            break;
                        }
                        default: v = 0.0; break;
                    }
                    ((double*)(void*)ao->dst)[di] = v;
                } else {
                    int64_t v;
                    switch (op) {
                        case OP_SUM:
                            v = ROW_RD_I64(row, ly->off_sum, s);
                            if (ao->affine) v += ao->bias_i64 * cnt;
                            break;
                        case OP_PROD:  v = ROW_RD_I64(row, ly->off_sum, s); break;
                        case OP_COUNT: v = cnt; break;
                        case OP_MIN:   v = ROW_RD_I64(row, ly->off_min, s); break;
                        case OP_MAX:   v = ROW_RD_I64(row, ly->off_max, s); break;
                        case OP_FIRST: case OP_LAST: v = ROW_RD_I64(row, ly->off_sum, s); break;
                        default:       v = 0; break;
                    }
                    ((int64_t*)(void*)ao->dst)[di] = v;
                }
            }
        }
    }
}

/* Phase 2: aggregate each partition independently using inline data */
typedef struct {
    int8_t*      key_types;
    uint8_t      n_keys;
    uint32_t     n_workers;
    radix_buf_t* bufs;
    group_ht_t*  part_hts;
    ght_layout_t layout;
    /* Shared (read-only) source column bases for wide-key resolution.
     * Each partition HT stashes the ones matching wide_key_mask. */
    void**       key_data;
} radix_phase2_ctx_t;

static void radix_phase2_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    radix_phase2_ctx_t* c = (radix_phase2_ctx_t*)ctx;
    uint16_t estride = c->layout.entry_stride;

    for (int64_t p = start; p < end; p++) {
        uint32_t total = 0;
        for (uint32_t w = 0; w < c->n_workers; w++)
            total += c->bufs[(size_t)w * RADIX_P + p].count;
        if (total == 0) continue;

        uint32_t part_ht_cap = 256;
        {
            uint64_t target = (uint64_t)total * 2;
            if (target < 256) target = 256;
            while (part_ht_cap < target) part_ht_cap *= 2;
        }
        /* Pre-size group store to avoid grows. Use next_pow2(total) as upper
         * bound on groups. Over-allocation is bounded: worst case total >> groups,
         * but total * row_stride is already committed via HT capacity anyway. */
        uint32_t init_grp = 256;
        while (init_grp < total && init_grp < 65536) init_grp *= 2;
        if (!group_ht_init_sized(&c->part_hts[p], part_ht_cap, &c->layout, init_grp))
            continue;
        /* Wide keys need source-column resolution during probe/rehash. */
        if (c->layout.wide_key_mask && c->key_data)
            group_ht_set_key_data(&c->part_hts[p], c->key_data);

        for (uint32_t w = 0; w < c->n_workers; w++) {
            radix_buf_t* buf = &c->bufs[(size_t)w * RADIX_P + p];
            if (buf->count == 0) continue;
            group_rows_indirect(&c->part_hts[p], c->key_types,
                                buf->data, buf->count, estride);
        }
    }
}

/* ============================================================================
 * Parallel direct-array accumulation for low-cardinality single integer key
 * ============================================================================ */

/* Parallel min/max scan for direct-array key range detection */
typedef struct {
    const void* key_data;
    int8_t      key_type;
    uint8_t     key_attrs;
    int64_t*    per_worker_min;  /* [n_workers] */
    int64_t*    per_worker_max;  /* [n_workers] */
    uint32_t    n_workers;
    const int64_t* match_idx;    /* NULL = no selection */
    ray_t*      rowsel;
} minmax_ctx_t;

static void minmax_scan_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    minmax_ctx_t* c = (minmax_ctx_t*)ctx;
    uint32_t wid = worker_id % c->n_workers;
    const int64_t* match_idx = c->match_idx;
    int64_t kmin = INT64_MAX, kmax = INT64_MIN;
    int8_t t = c->key_type;

    #define MINMAX_SEG_LOOP(TYPE, CAST) \
        do { \
            const TYPE* kd = (const TYPE*)c->key_data; \
            for (int64_t i = start; i < end; i++) { \
                int64_t r = match_idx ? match_idx[i] : i; \
                if (!match_idx && c->rowsel && !group_rowsel_pass(c->rowsel, r)) continue; \
                int64_t v = (int64_t)CAST kd[r]; \
                if (v < kmin) kmin = v; \
                if (v > kmax) kmax = v; \
            } \
        } while (0)

    if (t == RAY_I64 || t == RAY_TIMESTAMP)
        MINMAX_SEG_LOOP(int64_t, );
    else if (RAY_IS_SYM(t)) {
        uint8_t w = c->key_attrs & RAY_SYM_W_MASK;
        if (w == RAY_SYM_W64) MINMAX_SEG_LOOP(int64_t, );
        else if (w == RAY_SYM_W32) MINMAX_SEG_LOOP(uint32_t, );
        else if (w == RAY_SYM_W16) MINMAX_SEG_LOOP(uint16_t, );
        else MINMAX_SEG_LOOP(uint8_t, );
    }
    else if (t == RAY_BOOL || t == RAY_U8)
        MINMAX_SEG_LOOP(uint8_t, );
    else if (t == RAY_I16)
        MINMAX_SEG_LOOP(int16_t, );
    else /* RAY_I32, RAY_DATE, RAY_TIME */
        MINMAX_SEG_LOOP(int32_t, );

    #undef MINMAX_SEG_LOOP

    /* Merge with existing per-worker values (a worker may process multiple morsels) */
    if (kmin < c->per_worker_min[wid]) c->per_worker_min[wid] = kmin;
    if (kmax > c->per_worker_max[wid]) c->per_worker_max[wid] = kmax;
}

typedef union { double f; int64_t i; } da_val_t;

typedef struct {
    da_val_t* sum;       /* SUM/AVG/FIRST/LAST [n_slots * n_aggs] */
    da_val_t* min_val;   /* MIN [n_slots * n_aggs] */
    da_val_t* max_val;   /* MAX [n_slots * n_aggs] */
    double*   sumsq_f64; /* sum-of-squares for STDDEV/VAR */
    int64_t*  count;     /* group counts [n_slots] */
    int64_t*  first_row; /* min row index seen per slot (FIRST) [n_slots] */
    int64_t*  last_row;  /* max row index seen per slot (LAST)  [n_slots] */
    /* Arena headers */
    ray_t* _h_sum;
    ray_t* _h_min;
    ray_t* _h_max;
    ray_t* _h_sumsq;
    ray_t* _h_count;
    ray_t* _h_first_row;
    ray_t* _h_last_row;
} da_accum_t;

static inline void da_accum_free(da_accum_t* a) {
    scratch_free(a->_h_sum);
    scratch_free(a->_h_min);
    scratch_free(a->_h_max);
    scratch_free(a->_h_sumsq);
    scratch_free(a->_h_count);
    scratch_free(a->_h_first_row);
    scratch_free(a->_h_last_row);
}

/* Unified agg result emitter — used by both DA and HT paths.
 * Arrays indexed by [gi * n_aggs + a], counts by [gi]. */
static void emit_agg_columns(ray_t** result, ray_graph_t* g, const ray_op_ext_t* ext,
                              ray_t* const* agg_vecs, uint32_t grp_count,
                              uint8_t n_aggs,
                              const double*  sum_f64,  const int64_t* sum_i64,
                              const double*  min_f64,  const double*  max_f64,
                              const int64_t* min_i64,  const int64_t* max_i64,
                              const int64_t* counts,
                              const agg_affine_t* affine,
                              const double*  sumsq_f64) {
    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t agg_op = ext->agg_ops[a];
        ray_t* agg_col = agg_vecs[a];
        bool is_f64 = agg_col && agg_col->type == RAY_F64;
        int8_t out_type;
        switch (agg_op) {
            case OP_AVG:
            case OP_STDDEV: case OP_STDDEV_POP:
            case OP_VAR: case OP_VAR_POP:
                out_type = RAY_F64; break;
            case OP_COUNT: out_type = RAY_I64; break;
            case OP_SUM: case OP_PROD:
                out_type = is_f64 ? RAY_F64 : RAY_I64; break;
            default:
                out_type = agg_col ? agg_col->type : RAY_I64; break;
        }
        ray_t* new_col = ray_vec_new(out_type, (int64_t)grp_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = (int64_t)grp_count;
        for (uint32_t gi = 0; gi < grp_count; gi++) {
            size_t idx = (size_t)gi * n_aggs + a;
            if (out_type == RAY_F64) {
                double v;
                switch (agg_op) {
                    case OP_SUM:
                        v = is_f64 ? sum_f64[idx] : (double)sum_i64[idx];
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_f64 * counts[gi];
                        break;
                    case OP_PROD:
                        v = is_f64 ? sum_f64[idx] : (double)sum_i64[idx];
                        break;
                    case OP_AVG:
                        v = is_f64 ? sum_f64[idx] / counts[gi] : (double)sum_i64[idx] / counts[gi];
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_f64;
                        break;
                    case OP_MIN: v = is_f64 ? min_f64[idx] : (double)min_i64[idx]; break;
                    case OP_MAX: v = is_f64 ? max_f64[idx] : (double)max_i64[idx]; break;
                    case OP_FIRST: case OP_LAST:
                        v = is_f64 ? sum_f64[idx] : (double)sum_i64[idx]; break;
                    case OP_VAR: case OP_VAR_POP:
                    case OP_STDDEV: case OP_STDDEV_POP: {
                        int64_t cnt = counts[gi];
                        bool insuf = (agg_op == OP_VAR || agg_op == OP_STDDEV) ? cnt <= 1 : cnt <= 0;
                        if (insuf) { v = 0.0; ray_vec_set_null(new_col, gi, true); break; }
                        double sum_val = is_f64 ? sum_f64[idx] : (double)sum_i64[idx];
                        double sq_val = sumsq_f64 ? sumsq_f64[idx] : 0.0;
                        double mean = sum_val / cnt;
                        double var_pop = sq_val / cnt - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        if (agg_op == OP_VAR_POP) v = var_pop;
                        else if (agg_op == OP_VAR) v = var_pop * cnt / (cnt - 1);
                        else if (agg_op == OP_STDDEV_POP) v = sqrt(var_pop);
                        else v = sqrt(var_pop * cnt / (cnt - 1));
                        break;
                    }
                    default:     v = 0.0; break;
                }
                ((double*)ray_data(new_col))[gi] = v;
            } else {
                int64_t v;
                switch (agg_op) {
                    case OP_SUM:
                        v = sum_i64[idx];
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_i64 * counts[gi];
                        break;
                    case OP_PROD:  v = sum_i64[idx]; break;
                    case OP_COUNT: v = counts[gi]; break;
                    case OP_MIN:   v = min_i64[idx]; break;
                    case OP_MAX:   v = max_i64[idx]; break;
                    case OP_FIRST: case OP_LAST: v = sum_i64[idx]; break;
                    default:       v = 0; break;
                }
                ((int64_t*)ray_data(new_col))[gi] = v;
            }
        }
        /* Generate unique column name: base_name + agg suffix (e.g. "v1_sum") */
        ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
        int64_t name_id;
        if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
            ray_t* name_atom = ray_sym_str(agg_ext->sym);
            const char* base = name_atom ? ray_str_ptr(name_atom) : NULL;
            size_t blen = base ? ray_str_len(name_atom) : 0;
            const char* sfx = "";
            size_t slen = 0;
            switch (agg_op) {
                case OP_SUM:   sfx = "_sum";   slen = 4; break;
                case OP_PROD:  sfx = "_prod";  slen = 5; break;
                case OP_COUNT: sfx = "_count"; slen = 6; break;
                case OP_AVG:   sfx = "_mean";  slen = 5; break;
                case OP_MIN:   sfx = "_min";   slen = 4; break;
                case OP_MAX:   sfx = "_max";   slen = 4; break;
                case OP_FIRST: sfx = "_first"; slen = 6; break;
                case OP_LAST:  sfx = "_last";  slen = 5; break;
                case OP_STDDEV:     sfx = "_stddev";     slen = 7; break;
                case OP_STDDEV_POP: sfx = "_stddev_pop"; slen = 11; break;
                case OP_VAR:        sfx = "_var";        slen = 4; break;
                case OP_VAR_POP:    sfx = "_var_pop";    slen = 8; break;
                case OP_MEDIAN:     sfx = "_median";     slen = 7; break;
                case OP_TOP_N:      sfx = "_top";        slen = 4; break;
                case OP_BOT_N:      sfx = "_bot";        slen = 4; break;
            }
            char buf[256];
            if (base && blen + slen < sizeof(buf)) {
                memcpy(buf, base, blen);
                memcpy(buf + blen, sfx, slen);
                name_id = ray_sym_intern(buf, blen + slen);
            } else {
                name_id = agg_ext->sym;
            }
        } else {
            /* Expression agg input — synthetic name like "_e0_sum" */
            char nbuf[32];
            int np = 0;
            nbuf[np++] = '_'; nbuf[np++] = 'e';
            /* Multi-digit agg index */
            { uint8_t v = a; char dig[3]; int nd = 0;
              do { dig[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
              while (nd--) nbuf[np++] = dig[nd]; }
            const char* nsfx = "";
            size_t nslen = 0;
            switch (agg_op) {
                case OP_SUM:   nsfx = "_sum";   nslen = 4; break;
                case OP_PROD:  nsfx = "_prod";  nslen = 5; break;
                case OP_COUNT: nsfx = "_count"; nslen = 6; break;
                case OP_AVG:   nsfx = "_mean";  nslen = 5; break;
                case OP_MIN:   nsfx = "_min";   nslen = 4; break;
                case OP_MAX:   nsfx = "_max";   nslen = 4; break;
                case OP_FIRST: nsfx = "_first"; nslen = 6; break;
                case OP_LAST:  nsfx = "_last";  nslen = 5; break;
                case OP_STDDEV:     nsfx = "_stddev";     nslen = 7; break;
                case OP_STDDEV_POP: nsfx = "_stddev_pop"; nslen = 11; break;
                case OP_VAR:        nsfx = "_var";        nslen = 4; break;
                case OP_VAR_POP:    nsfx = "_var_pop";    nslen = 8; break;
                case OP_MEDIAN:     nsfx = "_median";     nslen = 7; break;
                case OP_TOP_N:      nsfx = "_top";        nslen = 4; break;
                case OP_BOT_N:      nsfx = "_bot";        nslen = 4; break;
            }
            memcpy(nbuf + np, nsfx, nslen);
            name_id = ray_sym_intern(nbuf, (size_t)np + nslen);
        }
        *result = ray_table_add_col(*result, name_id, new_col);
        ray_release(new_col);
    }
}

/* Bitmask for which accumulator arrays are actually needed */
#define DA_NEED_SUM   0x01  /* da_val_t sum array */
#define DA_NEED_MIN   0x02  /* da_val_t min_val array */
#define DA_NEED_MAX   0x04  /* da_val_t max_val array */
#define DA_NEED_COUNT 0x08  /* count array */
#define DA_NEED_SUMSQ 0x10  /* sumsq_f64 array (for STDDEV/VAR) */

typedef struct {
    da_accum_t*    accums;
    uint32_t       n_accums;     /* number of accumulator sets (may < pool workers) */
    void**         key_ptrs;     /* key data pointers [n_keys] */
    int8_t*        key_types;    /* key type codes [n_keys] */
    uint8_t*       key_attrs;    /* key attrs for RAY_SYM width [n_keys] */
    uint8_t*       key_esz;      /* pre-computed per-key elem size [n_keys] */
    int64_t*       key_mins;     /* per-key minimum [n_keys] */
    int64_t*       key_strides;  /* per-key stride [n_keys] */
    uint8_t        n_keys;
    void**         agg_ptrs;
    int8_t*        agg_types;
    ray_t**        agg_cols;
    uint8_t*       agg_strlen;
    uint16_t*      agg_ops;      /* per-agg operation code */
    uint8_t        n_aggs;
    uint8_t        need_flags;   /* DA_NEED_* bitmask */
    uint32_t       agg_f64_mask; /* bitmask: bit a set if agg[a] is RAY_F64 */
    bool           all_sum;      /* true when all ops are SUM/AVG/COUNT (no MIN/MAX/FIRST/LAST) */
    uint32_t       n_slots;
    const int64_t* match_idx;    /* NULL = no selection */
    ray_t*         rowsel;
} da_ctx_t;

typedef struct {
    uint8_t*   used;
    int64_t*   keys;
    int64_t*   counts;
    da_val_t*  sums;
    uint32_t   cap;
    uint32_t   size;
    ray_t*     _h_used;
    ray_t*     _h_keys;
    ray_t*     _h_counts;
    ray_t*     _h_sums;
} sparse_i64_ht_t;

static inline uint64_t sparse_i64_mix(uint64_t x) {
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return x;
}

static inline uint32_t sparse_i64_pow2(uint32_t x) {
    if (x <= 1) return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

static void sparse_i64_free(sparse_i64_ht_t* ht) {
    if (!ht) return;
    scratch_free(ht->_h_used);
    scratch_free(ht->_h_keys);
    scratch_free(ht->_h_counts);
    scratch_free(ht->_h_sums);
    memset(ht, 0, sizeof(*ht));
}

static _Thread_local ray_group_emit_filter_t tl_group_emit_filter;

ray_group_emit_filter_t ray_group_emit_filter_get(void) {
    return tl_group_emit_filter;
}

void ray_group_emit_filter_set(ray_group_emit_filter_t filter) {
    tl_group_emit_filter = filter;
}

static int64_t da_count_emit_keep_min(const int64_t* counts, uint32_t n_slots,
                                      uint32_t group_count,
                                      ray_group_emit_filter_t filter)
{
    int64_t keep_min = filter.min_count_exclusive + 1;
    int64_t k_take = filter.top_count_take;
    if (k_take <= 0 || k_take >= (int64_t)group_count)
        return keep_min;

    ray_t* heap_hdr = NULL;
    int64_t* heap = (int64_t*)scratch_alloc(&heap_hdr,
                                            (size_t)k_take * sizeof(int64_t));
    if (!heap)
        return keep_min;

    int64_t heap_n = 0;
    for (uint32_t s = 0; s < n_slots; s++) {
        int64_t cnt = counts[s];
        if (cnt <= 0)
            continue;
        if (heap_n < k_take) {
            int64_t j = heap_n++;
            heap[j] = cnt;
            while (j > 0) {
                int64_t p = (j - 1) >> 1;
                if (heap[p] <= heap[j]) break;
                int64_t tmp = heap[p]; heap[p] = heap[j]; heap[j] = tmp;
                j = p;
            }
        } else if (cnt > heap[0]) {
            heap[0] = cnt;
            int64_t j = 0;
            for (;;) {
                int64_t l = j * 2 + 1, r = l + 1, m = j;
                if (l < heap_n && heap[l] < heap[m]) m = l;
                if (r < heap_n && heap[r] < heap[m]) m = r;
                if (m == j) break;
                int64_t tmp = heap[m]; heap[m] = heap[j]; heap[j] = tmp;
                j = m;
            }
        }
    }

    if (heap_n == k_take && heap[0] > keep_min)
        keep_min = heap[0];
    scratch_free(heap_hdr);
    return keep_min;
}

static int64_t da_count_emit_keep_min_u32(const uint32_t* counts,
                                          uint64_t n_slots,
                                          uint32_t group_count,
                                          ray_group_emit_filter_t filter)
{
    int64_t keep_min = filter.min_count_exclusive + 1;
    int64_t k_take = filter.top_count_take;
    if (k_take <= 0 || k_take >= (int64_t)group_count)
        return keep_min;

    ray_t* heap_hdr = NULL;
    int64_t* heap = (int64_t*)scratch_alloc(&heap_hdr,
                                            (size_t)k_take * sizeof(int64_t));
    if (!heap)
        return keep_min;

    int64_t heap_n = 0;
    for (uint64_t s = 0; s < n_slots; s++) {
        int64_t cnt = (int64_t)counts[s];
        if (cnt <= 0)
            continue;
        if (heap_n < k_take) {
            int64_t j = heap_n++;
            heap[j] = cnt;
            while (j > 0) {
                int64_t p = (j - 1) >> 1;
                if (heap[p] <= heap[j]) break;
                int64_t tmp = heap[p]; heap[p] = heap[j]; heap[j] = tmp;
                j = p;
            }
        } else if (cnt > heap[0]) {
            heap[0] = cnt;
            int64_t j = 0;
            for (;;) {
                int64_t l = j * 2 + 1, r = l + 1, m = j;
                if (l < heap_n && heap[l] < heap[m]) m = l;
                if (r < heap_n && heap[r] < heap[m]) m = r;
                if (m == j) break;
                int64_t tmp = heap[m]; heap[m] = heap[j]; heap[j] = tmp;
                j = m;
            }
        }
    }

    if (heap_n == k_take && heap[0] > keep_min)
        keep_min = heap[0];
    scratch_free(heap_hdr);
    return keep_min;
}

static bool sparse_i64_init(sparse_i64_ht_t* ht, uint32_t cap, uint8_t n_aggs,
                            bool need_sum) {
    memset(ht, 0, sizeof(*ht));
    if (cap < 1024) cap = 1024;
    cap = sparse_i64_pow2(cap);
    ht->used = (uint8_t*)scratch_calloc(&ht->_h_used, cap);
    ht->keys = (int64_t*)scratch_alloc(&ht->_h_keys, (size_t)cap * sizeof(int64_t));
    ht->counts = (int64_t*)scratch_calloc(&ht->_h_counts,
                                          (size_t)cap * sizeof(int64_t));
    if (need_sum) {
        ht->sums = (da_val_t*)scratch_calloc(&ht->_h_sums,
            (size_t)cap * n_aggs * sizeof(da_val_t));
    }
    if (!ht->used || !ht->keys || !ht->counts || (need_sum && !ht->sums)) {
        sparse_i64_free(ht);
        return false;
    }
    ht->cap = cap;
    return true;
}

static int32_t sparse_i64_find_slot(const sparse_i64_ht_t* ht, int64_t key) {
    uint32_t mask = ht->cap - 1;
    uint32_t pos = (uint32_t)sparse_i64_mix((uint64_t)key) & mask;
    while (ht->used[pos] && ht->keys[pos] != key)
        pos = (pos + 1) & mask;
    return (int32_t)pos;
}

static bool sparse_i64_rehash(sparse_i64_ht_t* ht, uint8_t n_aggs,
                              bool need_sum) {
    sparse_i64_ht_t old = *ht;
    sparse_i64_ht_t nw;
    if (!sparse_i64_init(&nw, old.cap * 2u, n_aggs, need_sum))
        return false;
    for (uint32_t i = 0; i < old.cap; i++) {
        if (!old.used[i]) continue;
        int32_t s = sparse_i64_find_slot(&nw, old.keys[i]);
        nw.used[s] = 1;
        nw.keys[s] = old.keys[i];
        nw.counts[s] = old.counts[i];
        if (need_sum)
            memcpy(&nw.sums[(size_t)s * n_aggs], &old.sums[(size_t)i * n_aggs],
                   (size_t)n_aggs * sizeof(da_val_t));
        nw.size++;
    }
    sparse_i64_free(&old);
    *ht = nw;
    return true;
}

static bool sparse_i64_touch(sparse_i64_ht_t* ht, int64_t key, uint8_t n_aggs,
                             bool need_sum, int32_t* out_slot) {
    if ((uint64_t)(ht->size + 1) * 10u >= (uint64_t)ht->cap * 7u) {
        if (!sparse_i64_rehash(ht, n_aggs, need_sum))
            return false;
    }
    int32_t s = sparse_i64_find_slot(ht, key);
    if (!ht->used[s]) {
        ht->used[s] = 1;
        ht->keys[s] = key;
        ht->counts[s] = 0;
        if (need_sum)
            memset(&ht->sums[(size_t)s * n_aggs], 0,
                   (size_t)n_aggs * sizeof(da_val_t));
        ht->size++;
    }
    *out_slot = s;
    return true;
}

/* Composite GID from multi-key.  Arithmetic overflow is prevented in practice
 * by the DA budget check (DA_PER_WORKER_MAX) which limits total_slots to 262K. */
static inline int32_t da_composite_gid(da_ctx_t* c, int64_t r) {
    int32_t gid = 0;
    for (uint8_t k = 0; k < c->n_keys; k++) {
        int64_t val = read_by_esz(c->key_ptrs[k], r, c->key_esz[k]);
        gid += (int32_t)((val - c->key_mins[k]) * c->key_strides[k]);
    }
    return gid;
}

/* Typed composite GID: eliminates per-element switch when all keys share width */
#define DEFINE_DA_COMPOSITE_GID_TYPED(SUFFIX, KTYPE) \
static inline int32_t da_composite_gid_##SUFFIX(da_ctx_t* c, int64_t r) { \
    int32_t gid = 0; \
    for (uint8_t k = 0; k < c->n_keys; k++) { \
        int64_t val = (int64_t)((const KTYPE*)c->key_ptrs[k])[r]; \
        gid += (int32_t)((val - c->key_mins[k]) * c->key_strides[k]); \
    } \
    return gid; \
}
DEFINE_DA_COMPOSITE_GID_TYPED(u8,  uint8_t)
DEFINE_DA_COMPOSITE_GID_TYPED(u16, uint16_t)
DEFINE_DA_COMPOSITE_GID_TYPED(u32, uint32_t)
DEFINE_DA_COMPOSITE_GID_TYPED(i64, int64_t)
#undef DEFINE_DA_COMPOSITE_GID_TYPED

static inline void da_read_val(const void* ptr, int8_t type, uint8_t attrs,
                               int64_t r, double* out_f64, int64_t* out_i64) {
    if (type == RAY_F64) {
        *out_f64 = ((const double*)ptr)[r];
        *out_i64 = (int64_t)*out_f64;
    } else {
        *out_i64 = read_col_i64(ptr, r, type, attrs);
        *out_f64 = (double)*out_i64;
    }
}

static inline int64_t group_strlen_at(const ray_t* col, int64_t row) {
    if (!col || ray_vec_is_null((ray_t*)col, row)) return 0;
    if (col->type == RAY_STR) {
        const ray_str_t* elems;
        const char* pool;
        (void)pool;
        str_resolve(col, &elems, &pool);
        return (int64_t)elems[row].len;
    }
    const char* sp;
    size_t sl;
    (void)sp;
    sym_elem(col, row, &sp, &sl);
    return (int64_t)sl;
}

static inline int64_t group_strlen_at_cached(const ray_t* col, int64_t row,
                                             ray_t** sym_strings,
                                             uint32_t sym_count) {
    if (!col || ray_vec_is_null((ray_t*)col, row)) return 0;
    if (col->type == RAY_STR) {
        const ray_str_t* elems;
        const char* pool;
        (void)pool;
        str_resolve(col, &elems, &pool);
        return (int64_t)elems[row].len;
    }
    if (col->type == RAY_SYM && sym_strings) {
        int64_t sym_id = ray_read_sym(ray_data((ray_t*)col), row,
                                      col->type, col->attrs);
        if (sym_id < 0 || (uint64_t)sym_id >= sym_count) return 0;
        ray_t* atom = sym_strings[sym_id];
        return atom ? (int64_t)ray_str_len(atom) : 0;
    }
    return group_strlen_at(col, row);
}

static bool try_strlen_sumavg_input(ray_graph_t* g, ray_t* tbl,
                                    ray_op_t* input_op, ray_t** out_vec) {
    if (!g || !tbl || !input_op || !out_vec) return false;
    if (input_op->opcode != OP_STRLEN || input_op->arity != 1 || !input_op->inputs[0])
        return false;
    ray_op_t* child = input_op->inputs[0];
    ray_op_ext_t* child_ext = find_ext(g, child->id);
    if (!child_ext || child_ext->base.opcode != OP_SCAN) return false;
    ray_t* col = ray_table_get_col(tbl, child_ext->sym);
    if (!col || (col->type != RAY_STR && col->type != RAY_SYM)) return false;
    *out_vec = col;
    return true;
}

/* Materialize a scalar (atom or len-1 vector) into a full-length vector so
 * group-aggregation loops can read row-wise without out-of-bounds access. */
static ray_t* materialize_broadcast_input(ray_t* src, int64_t nrows) {
    if (!src || RAY_IS_ERR(src) || nrows < 0) return NULL;

    int8_t out_type = ray_is_atom(src) ? (int8_t)-src->type : src->type;
    if (out_type <= 0 || out_type >= RAY_TYPE_COUNT) return NULL;

    ray_t* out = ray_vec_new(out_type, nrows);
    if (!out || RAY_IS_ERR(out)) return out;
    out->len = nrows;
    if (nrows == 0) return out;

    if (!ray_is_atom(src)) {
        uint8_t esz = col_esz(src);
        const char* s = (const char*)ray_data(src);
        char* d = (char*)ray_data(out);
        for (int64_t i = 0; i < nrows; i++)
            memcpy(d + (size_t)i * esz, s, esz);
        return out;
    }

    switch (src->type) {
        case -RAY_F64: {
            double v = src->f64;
            for (int64_t i = 0; i < nrows; i++) ((double*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_I64:
        case -RAY_SYM:
        case -RAY_TIMESTAMP: {
            int64_t v = src->i64;
            for (int64_t i = 0; i < nrows; i++) ((int64_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_DATE:
        case -RAY_TIME: {
            int32_t v = (int32_t)src->i64;
            for (int64_t i = 0; i < nrows; i++) ((int32_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_I32: {
            int32_t v = src->i32;
            for (int64_t i = 0; i < nrows; i++) ((int32_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_I16: {
            int16_t v = src->i16;
            for (int64_t i = 0; i < nrows; i++) ((int16_t*)ray_data(out))[i] = v;
            return out;
        }
        case -RAY_U8:
        case -RAY_BOOL: {
            uint8_t v = src->u8;
            for (int64_t i = 0; i < nrows; i++) ((uint8_t*)ray_data(out))[i] = v;
            return out;
        }
        default:
            ray_release(out);
            return NULL;
    }
}

/* ---- Scalar aggregate (n_keys==0): one flat scan, no GID, no hash ---- */
typedef struct {
    void**         agg_ptrs;
    int8_t*        agg_types;
    ray_t**        agg_cols;
    uint8_t*       agg_strlen;
    uint16_t*      agg_ops;
    agg_linear_t*  agg_linear;
    uint8_t        n_aggs;
    uint8_t        need_flags;
    const int64_t* match_idx;    /* NULL = no selection */
    ray_t*         rowsel;
    /* per-worker accumulators (1 slot each) */
    da_accum_t*    accums;
    uint32_t       n_accums;
} scalar_ctx_t;

static inline int64_t scalar_i64_at(const void* ptr, int8_t type, int64_t r) {
    return read_col_i64(ptr, r, type, 0);  /* attrs=0: agg columns are numeric, never SYM */
}

/* Tight SIMD-friendly loop for single SUM/AVG on i64 (no mask).
 * Note: int64 sum can overflow; caller responsibility to use appropriate types. */
static void scalar_sum_i64_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const int64_t* restrict data = (const int64_t*)c->agg_ptrs[0];
    int64_t sum = 0;
    for (int64_t r = start; r < end; r++)
        sum += data[r];
    acc->sum[0].i += sum;
    acc->count[0] += end - start;
}

/* Tight SIMD-friendly loop for single SUM/AVG on f64 (no mask) */
static void scalar_sum_f64_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const double* restrict data = (const double*)c->agg_ptrs[0];
    double sum = 0.0;
    for (int64_t r = start; r < end; r++)
        sum += data[r];
    acc->sum[0].f += sum;
    acc->count[0] += end - start;
}

/* Tight loop for single SUM/AVG on integer linear expression (no mask). */
static void scalar_sum_linear_i64_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const agg_linear_t* lin = &c->agg_linear[0];
    int64_t n = end - start;

    int64_t sum = lin->bias_i64 * n;
    for (uint8_t t = 0; t < lin->n_terms; t++) {
        int64_t coeff = lin->coeff_i64[t];
        if (coeff == 0) continue;
        const void* ptr = lin->term_ptrs[t];
        int8_t type = lin->term_types[t];
        int64_t term_sum = 0;
        for (int64_t r = start; r < end; r++)
            term_sum += scalar_i64_at(ptr, type, r);
        sum += coeff * term_sum;
    }

    acc->sum[0].i += sum;
    acc->count[0] += n;
}

/* Generic scalar accumulation: handles all ops, all types, mask */
/* Inner scalar accumulation for a single row */
static inline void scalar_accum_row(scalar_ctx_t* c, da_accum_t* acc, int64_t r) {
    uint8_t n_aggs = c->n_aggs;
    acc->count[0]++;
    for (uint8_t a = 0; a < n_aggs; a++) {
        double fv; int64_t iv;
        if (c->agg_linear && c->agg_linear[a].enabled) {
            const agg_linear_t* lin = &c->agg_linear[a];
            iv = lin->bias_i64;
            for (uint8_t t = 0; t < lin->n_terms; t++) {
                iv += lin->coeff_i64[t] *
                      scalar_i64_at(lin->term_ptrs[t], lin->term_types[t], r);
            }
            fv = (double)iv;
        } else {
            if (!c->agg_ptrs[a]) continue;
            if (c->agg_strlen && c->agg_strlen[a]) {
                iv = group_strlen_at(c->agg_cols[a], r);
                fv = (double)iv;
            } else {
                da_read_val(c->agg_ptrs[a], c->agg_types[a], 0, r, &fv, &iv);
            }
        }
        uint16_t op = c->agg_ops[a];
        bool is_f = (c->agg_types[a] == RAY_F64);
        if (op == OP_SUM || op == OP_AVG || op == OP_STDDEV || op == OP_STDDEV_POP || op == OP_VAR || op == OP_VAR_POP) {
            if (is_f) acc->sum[a].f += fv;
            else acc->sum[a].i += iv;
            if (acc->sumsq_f64) acc->sumsq_f64[a] += fv * fv;
        } else if (op == OP_PROD) {
            if (is_f) {
                if (acc->count[0] == 1) acc->sum[a].f = fv;
                else acc->sum[a].f *= fv;
            } else {
                if (acc->count[0] == 1) acc->sum[a].i = iv;
                else acc->sum[a].i = (int64_t)((uint64_t)acc->sum[a].i * (uint64_t)iv);
            }
        } else if (op == OP_FIRST) {
            if (acc->count[0] == 1) {
                if (is_f) acc->sum[a].f = fv; else acc->sum[a].i = iv;
            }
        } else if (op == OP_LAST) {
            if (is_f) acc->sum[a].f = fv; else acc->sum[a].i = iv;
        } else if (op == OP_MIN) {
            if (is_f) { if (fv < acc->min_val[a].f) acc->min_val[a].f = fv; }
            else      { if (iv < acc->min_val[a].i) acc->min_val[a].i = iv; }
        } else if (op == OP_MAX) {
            if (is_f) { if (fv > acc->max_val[a].f) acc->max_val[a].f = fv; }
            else      { if (iv > acc->max_val[a].i) acc->max_val[a].i = iv; }
        }
    }
}

static void scalar_accum_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    scalar_ctx_t* c = (scalar_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    const int64_t* match_idx = c->match_idx;

    for (int64_t i = start; i < end; i++) {
        int64_t r = match_idx ? match_idx[i] : i;
        if (!match_idx && c->rowsel && !group_rowsel_pass(c->rowsel, r)) continue;
        scalar_accum_row(c, acc, r);
    }
}

/* Inner DA accumulation for a single row — shared by single-key and multi-key paths.
 * Fast path for SUM/AVG-only queries: eliminates op-code dispatch and da_read_val
 * dual-write overhead.  The branch on c->all_sum is perfectly predicted (invariant
 * across all rows). */
static inline void da_accum_row(da_ctx_t* c, da_accum_t* acc, int32_t gid, int64_t r) {
    uint8_t n_aggs = c->n_aggs;
    acc->count[gid]++;
    size_t base = (size_t)gid * n_aggs;

    if (RAY_LIKELY(c->all_sum)) {
        /* SUM/AVG/COUNT fast path — no op-code dispatch, typed read only.
         * COUNT-only queries have acc->sum==NULL; count[gid]++ above suffices. */
        if (!acc->sum) return;
        uint32_t f64m = c->agg_f64_mask;
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (!c->agg_ptrs[a]) continue;
            size_t idx = base + a;
            if (c->agg_strlen && c->agg_strlen[a])
                acc->sum[idx].i += group_strlen_at(c->agg_cols[a], r);
            else if (f64m & (1u << a))
                acc->sum[idx].f += ((const double*)c->agg_ptrs[a])[r];
            else
                acc->sum[idx].i += read_col_i64(c->agg_ptrs[a], r,
                                                c->agg_types[a], 0);
        }
        return;
    }

    /* Track per-slot row-index bounds when FIRST/LAST is needed.  Pool
     * dispatch is work-stealing: tasks may be claimed by a single worker
     * out of index order, so rows do NOT arrive in monotonic order within
     * a worker.  Use explicit min/max comparison against r and update the
     * stored value only when the new row beats the current bound. */
    bool fl_take_first = (acc->first_row && r < acc->first_row[gid]);
    bool fl_take_last  = (acc->last_row  && r > acc->last_row[gid]);

    for (uint8_t a = 0; a < n_aggs; a++) {
        if (!c->agg_ptrs[a]) continue;
        size_t idx = base + a;
        double fv; int64_t iv;
        if (c->agg_strlen && c->agg_strlen[a]) {
            iv = group_strlen_at(c->agg_cols[a], r);
            fv = (double)iv;
        } else {
            da_read_val(c->agg_ptrs[a], c->agg_types[a], 0, r, &fv, &iv);
        }
        uint16_t op = c->agg_ops[a];
        if (op == OP_SUM || op == OP_AVG || op == OP_STDDEV || op == OP_STDDEV_POP || op == OP_VAR || op == OP_VAR_POP) {
            if (c->agg_types[a] == RAY_F64) acc->sum[idx].f += fv;
            else acc->sum[idx].i = (int64_t)((uint64_t)acc->sum[idx].i + (uint64_t)iv);
            if (acc->sumsq_f64) acc->sumsq_f64[idx] += fv * fv;
        } else if (op == OP_PROD) {
            if (c->agg_types[a] == RAY_F64) {
                if (acc->count[gid] == 1) acc->sum[idx].f = fv;
                else acc->sum[idx].f *= fv;
            } else {
                if (acc->count[gid] == 1) acc->sum[idx].i = iv;
                else acc->sum[idx].i = (int64_t)((uint64_t)acc->sum[idx].i * (uint64_t)iv);
            }
        } else if (op == OP_FIRST) {
            if (fl_take_first) {
                if (c->agg_types[a] == RAY_F64) acc->sum[idx].f = fv;
                else acc->sum[idx].i = iv;
            }
        } else if (op == OP_LAST) {
            if (fl_take_last) {
                if (c->agg_types[a] == RAY_F64) acc->sum[idx].f = fv;
                else acc->sum[idx].i = iv;
            }
        } else if (op == OP_MIN) {
            if (c->agg_types[a] == RAY_F64) {
                if (fv < acc->min_val[idx].f) acc->min_val[idx].f = fv;
            } else {
                if (iv < acc->min_val[idx].i) acc->min_val[idx].i = iv;
            }
        } else if (op == OP_MAX) {
            if (c->agg_types[a] == RAY_F64) {
                if (fv > acc->max_val[idx].f) acc->max_val[idx].f = fv;
            } else {
                if (iv > acc->max_val[idx].i) acc->max_val[idx].i = iv;
            }
        }
    }

    /* Commit row-index bounds after value writes. */
    if (fl_take_first) acc->first_row[gid] = r;
    if (fl_take_last)  acc->last_row[gid]  = r;
}

static void da_accum_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    da_ctx_t* c = (da_ctx_t*)ctx;
    da_accum_t* acc = &c->accums[worker_id];
    uint8_t n_aggs = c->n_aggs;
    uint8_t n_keys = c->n_keys;
    const int64_t* match_idx = c->match_idx;

    /* Fast path: single key — avoid composite GID loop overhead.
     * Templated by key element size: the entire loop is stamped out per width
     * so the compiler generates direct movzbl/movzwl/movl/movq — zero dispatch. */
    #define DA_PF_DIST 8
    #define DA_SINGLE_KEY_LOOP(KTYPE, KCAST) \
    do { \
        const KTYPE* kp = (const KTYPE*)c->key_ptrs[0]; \
        int64_t kmin = c->key_mins[0]; \
        bool da_pf = c->n_slots >= 4096; \
        for (int64_t i = start; i < end; i++) { \
            int64_t r = match_idx ? match_idx[i] : i; \
            if (!match_idx && c->rowsel && !group_rowsel_pass(c->rowsel, r)) continue; \
            if (da_pf && RAY_LIKELY(i + DA_PF_DIST < end)) { \
                int64_t pf_r = match_idx ? match_idx[i + DA_PF_DIST] : (i + DA_PF_DIST); \
                int64_t pfk = (int64_t)KCAST kp[pf_r]; \
                __builtin_prefetch(&acc->count[(int32_t)(pfk - kmin)], 1, 1); \
                if (acc->sum) __builtin_prefetch( \
                    &acc->sum[(size_t)(int32_t)(pfk - kmin) * n_aggs], 1, 1); \
            } \
            int64_t kv = (int64_t)KCAST kp[r]; \
            da_accum_row(c, acc, (int32_t)(kv - kmin), r); \
        } \
    } while (0)

    if (n_keys == 1) {
        switch (c->key_esz[0]) {
        case 1: DA_SINGLE_KEY_LOOP(uint8_t, ); break;
        case 2: DA_SINGLE_KEY_LOOP(uint16_t, ); break;
        case 4: DA_SINGLE_KEY_LOOP(uint32_t, (int64_t)); break;
        default: DA_SINGLE_KEY_LOOP(int64_t, ); break;
        }
        #undef DA_SINGLE_KEY_LOOP
        return;
    }

    /* Multi-key composite GID — typed inner loop eliminates read_by_esz switch.
     * When all keys share the same element size, use da_composite_gid_XX(). */
    #define DA_MULTI_KEY_LOOP(GID_FN) \
    do { \
        bool _da_pf = c->n_slots >= 4096; \
        for (int64_t i = start; i < end; i++) { \
            int64_t r = match_idx ? match_idx[i] : i; \
            if (!match_idx && c->rowsel && !group_rowsel_pass(c->rowsel, r)) continue; \
            if (_da_pf && RAY_LIKELY(i + DA_PF_DIST < end)) { \
                int64_t pf_r = match_idx ? match_idx[i + DA_PF_DIST] : (i + DA_PF_DIST); \
                int32_t pf_gid = GID_FN(pf_r); \
                __builtin_prefetch(&acc->count[pf_gid], 1, 1); \
                if (acc->sum) __builtin_prefetch(&acc->sum[(size_t)pf_gid * n_aggs], 1, 1); \
            } \
            da_accum_row(c, acc, GID_FN(r), r); \
        } \
    } while (0)

    /* Check if all keys share the same element size */
    bool uniform_esz = true;
    for (uint8_t k = 1; k < n_keys; k++)
        if (c->key_esz[k] != c->key_esz[0]) { uniform_esz = false; break; }

    if (uniform_esz) {
        switch (c->key_esz[0]) {
        case 1:
#define GID_FN(R) da_composite_gid_u8(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        case 2:
#define GID_FN(R) da_composite_gid_u16(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        case 4:
#define GID_FN(R) da_composite_gid_u32(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        default:
#define GID_FN(R) da_composite_gid_i64(c, (R))
            DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
            break;
        }
    } else {
#define GID_FN(R) da_composite_gid(c, (R))
        DA_MULTI_KEY_LOOP(GID_FN);
#undef GID_FN
    }
    #undef DA_MULTI_KEY_LOOP
    #undef DA_PF_DIST
}

/* Parallel DA merge: merge per-worker accumulators into accums[0] by
 * dispatching disjoint slot ranges across pool workers. */
typedef struct {
    da_accum_t* accums;
    uint32_t    n_src_workers; /* number of source workers to merge (1..n) */
    uint8_t     need_flags;
    uint8_t     n_aggs;
    const int8_t* agg_types;  /* per-agg value type (for typed merge) */
    const uint16_t* agg_ops;  /* per-agg opcode (for FIRST/LAST merge) */
} da_merge_ctx_t;

static void da_merge_fn(void* ctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    da_merge_ctx_t* c = (da_merge_ctx_t*)ctx;
    da_accum_t* merged = &c->accums[0];
    uint8_t n_aggs = c->n_aggs;
    const int8_t* agg_types = c->agg_types;
    for (uint32_t w = 1; w < c->n_src_workers; w++) {
        da_accum_t* wa = &c->accums[w];
        for (int64_t s = start; s < end; s++) {
            size_t base = (size_t)s * n_aggs;
            if (c->need_flags & DA_NEED_SUMSQ) {
                for (uint8_t a = 0; a < n_aggs; a++)
                    merged->sumsq_f64[base + a] += wa->sumsq_f64[base + a];
            }
            if (c->need_flags & DA_NEED_SUM) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    uint16_t aop = c->agg_ops ? c->agg_ops[a] : OP_SUM;
                    if (aop == OP_FIRST) {
                        /* Keep worker 0 value; take from w only if merged has no data */
                        if (merged->count[s] == 0 && wa->count[s] > 0)
                            merged->sum[idx] = wa->sum[idx];
                    } else if (aop == OP_LAST) {
                        /* Overwrite with last worker that has data */
                        if (wa->count[s] > 0)
                            merged->sum[idx] = wa->sum[idx];
                    } else if (aop == OP_PROD) {
                        if (wa->count[s] > 0) {
                            if (merged->count[s] == 0)
                                merged->sum[idx] = wa->sum[idx];
                            else if (agg_types[a] == RAY_F64)
                                merged->sum[idx].f *= wa->sum[idx].f;
                            else
                                merged->sum[idx].i = (int64_t)((uint64_t)merged->sum[idx].i * (uint64_t)wa->sum[idx].i);
                        }
                    } else if (agg_types[a] == RAY_F64)
                        merged->sum[idx].f += wa->sum[idx].f;
                    else
                        merged->sum[idx].i += wa->sum[idx].i;
                }
            }
            if (c->need_flags & DA_NEED_MIN) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    if (agg_types[a] == RAY_F64) {
                        if (wa->min_val[idx].f < merged->min_val[idx].f)
                            merged->min_val[idx].f = wa->min_val[idx].f;
                    } else {
                        if (wa->min_val[idx].i < merged->min_val[idx].i)
                            merged->min_val[idx].i = wa->min_val[idx].i;
                    }
                }
            }
            if (c->need_flags & DA_NEED_MAX) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    if (agg_types[a] == RAY_F64) {
                        if (wa->max_val[idx].f > merged->max_val[idx].f)
                            merged->max_val[idx].f = wa->max_val[idx].f;
                    } else {
                        if (wa->max_val[idx].i > merged->max_val[idx].i)
                            merged->max_val[idx].i = wa->max_val[idx].i;
                    }
                }
            }
            merged->count[s] += wa->count[s];
        }
    }
}

/* ============================================================================
 * Post-radix holistic-aggregate fill (OP_MEDIAN)
 *
 * After the radix pipeline produces stable per-partition group IDs in
 * part_hts[] + part_offsets[], we still need to materialize per-group
 * value slices to feed the holistic quickselect kernel.  This pass:
 *
 *   1. Re-probe each source row against part_hts[RADIX_PART(h)] to
 *      recover its global gid (parallel, lookup-only — no inserts).
 *      Writes row_gid[r] = part_offsets[p] + local_gid.
 *   2. Build idx_buf + offsets via the idxbuf hist/scat pattern over
 *      row_gid (parallel).
 *   3. For each OP_MEDIAN agg, call ray_median_per_group_buf and copy
 *      the F64 output into the pre-allocated agg_outs[a].vec.
 *
 * Cost: ~1 extra parallel hash+probe pass over nrows (~50 ms at 10 M
 * rows, 27 cores).  The eval-fallback this replaces was building a
 * LIST<LIST<key>> for the same data — ~5500 ms at the same scale.
 * ============================================================================ */

/* Lookup-only HT probe — finds the gid of the matching group without
 * modifying the HT.  Returns UINT32_MAX if the row's key combination
 * is absent (shouldn't happen post-phase-2 since every row was
 * inserted, but a defensive sentinel keeps callers robust under
 * partial-build OOM corner cases). */
static inline uint32_t group_ht_lookup_gid(const group_ht_t* ht,
                                            uint64_t hash,
                                            const int64_t* ekeys,
                                            const int8_t* key_types) {
    (void)key_types;
    const ght_layout_t* ly = &ht->layout;
    uint32_t mask = ht->ht_cap - 1;
    uint8_t salt = HT_SALT(hash);
    uint32_t slot = (uint32_t)(hash & mask);
    uint16_t rs = ly->row_stride;
    for (;;) {
        uint32_t sv = ht->slots[slot];
        if (sv == HT_EMPTY) return UINT32_MAX;
        if (HT_SALT_V(sv) == salt) {
            uint32_t gid = HT_GID(sv);
            const char* row = ht->rows + (size_t)gid * rs;
            if (group_keys_equal((const int64_t*)(const void*)(row + 8),
                                  ekeys, ly, ht->key_data))
                return gid;
        }
        slot = (slot + 1) & mask;
    }
}

typedef struct {
    void**        key_data;
    int8_t*       key_types;
    uint8_t*      key_attrs;
    ray_t**       key_vecs;
    uint8_t       n_keys;
    uint8_t       nullable_mask;
    uint8_t       wide_mask;
    const uint8_t* wide_esz;
    group_ht_t*   part_hts;
    const uint32_t* part_offsets;
    int64_t*      row_gid;          /* output [nrows] */
    const int64_t* match_idx;
} reprobe_ctx_t;

static void reprobe_rows_fn(void* vctx, uint32_t worker_id,
                            int64_t start, int64_t end) {
    (void)worker_id;
    reprobe_ctx_t* c = (reprobe_ctx_t*)vctx;
    uint8_t nk = c->n_keys;
    int64_t ek_buf[9];           /* nk + null_mask slot */
    int8_t* key_types = c->key_types;
    void** key_data = c->key_data;
    uint8_t* key_attrs = c->key_attrs;
    ray_t** key_vecs = c->key_vecs;
    uint8_t nullable = c->nullable_mask;
    uint8_t wide = c->wide_mask;
    const uint8_t* wide_esz = c->wide_esz;
    const int64_t* match_idx = c->match_idx;
    for (int64_t i = start; i < end; i++) {
        if (((i - start) & 65535) == 0 && ray_interrupted()) break;
        int64_t row = match_idx ? match_idx[i] : i;
        uint64_t h = 0;
        int64_t null_mask = 0;
        for (uint8_t k = 0; k < nk; k++) {
            int8_t t = key_types[k];
            uint64_t kh;
            bool is_null = (nullable & (1u << k))
                           && ray_vec_is_null(key_vecs[k], row);
            if (is_null) {
                null_mask |= (int64_t)(1u << k);
                ek_buf[k] = 0;
                kh = ray_hash_i64(0);
            } else if (wide & (1u << k)) {
                uint8_t esz = wide_esz[k];
                const void* src = (const char*)key_data[k] + (size_t)row * esz;
                ek_buf[k] = row;
                kh = ray_hash_bytes(src, esz);
            } else if (t == RAY_F64) {
                int64_t kv;
                memcpy(&kv, &((double*)key_data[k])[row], 8);
                ek_buf[k] = kv;
                kh = ray_hash_f64(((double*)key_data[k])[row]);
            } else {
                int64_t kv = read_col_i64(key_data[k], row, t, key_attrs[k]);
                ek_buf[k] = kv;
                kh = ray_hash_i64(kv);
            }
            h = (k == 0) ? kh : ray_hash_combine(h, kh);
        }
        ek_buf[nk] = null_mask;
        if (null_mask) h = ray_hash_combine(h, ray_hash_i64(null_mask));

        uint32_t part = RADIX_PART(h);
        uint32_t local = group_ht_lookup_gid(&c->part_hts[part], h,
                                              ek_buf, key_types);
        if (local == UINT32_MAX) {
            c->row_gid[row] = -1;
        } else {
            c->row_gid[row] = (int64_t)c->part_offsets[part] + (int64_t)local;
        }
    }
}

/* Histogram + scatter for idx_buf construction.  Identical pattern to
 * query.c's idxbuf_hist_fn / idxbuf_scat_fn — duplicated here to avoid
 * pulling a query.c-internal helper through internal.h.
 *
 * Dispatched via ray_pool_dispatch_n with n_tasks units.  Each unit owns
 * a contiguous row range [task_id*grain, min((task_id+1)*grain, nrows)).
 * grain is sized to give n_tasks ≈ total_workers — this caps the
 * hist/cur matrices at n_tasks * n_groups * 8 bytes (rather than
 * blowing up to ~1GB when n_groups is large and grain is the default
 * 8K morsel size).  The serial cumsum that walks hist by-gi becomes
 * cheap (n_groups * n_tasks ops, n_tasks small). */
typedef struct {
    const int64_t* row_gid;
    int64_t*       hist;          /* [n_tasks * n_groups] */
    int64_t*       cursor;        /* [n_tasks * n_groups] */
    int64_t*       idx_buf;
    int64_t        n_groups;
    int64_t        grain;
    int64_t        nrows;
} med_idx_ctx_t;

static void med_idx_hist_fn(void* vctx, uint32_t worker_id,
                            int64_t start, int64_t end) {
    (void)worker_id; (void)end;
    med_idx_ctx_t* c = (med_idx_ctx_t*)vctx;
    int64_t task_id = start;  /* dispatched via _n: start = task index */
    int64_t r_lo = task_id * c->grain;
    int64_t r_hi = r_lo + c->grain;
    if (r_hi > c->nrows) r_hi = c->nrows;
    int64_t* hist = c->hist + task_id * c->n_groups;
    const int64_t* row_gid = c->row_gid;
    for (int64_t r = r_lo; r < r_hi; r++) {
        int64_t gi = row_gid[r];
        if (gi >= 0) hist[gi]++;
    }
}

static void med_idx_scat_fn(void* vctx, uint32_t worker_id,
                            int64_t start, int64_t end) {
    (void)worker_id; (void)end;
    med_idx_ctx_t* c = (med_idx_ctx_t*)vctx;
    int64_t task_id = start;
    int64_t r_lo = task_id * c->grain;
    int64_t r_hi = r_lo + c->grain;
    if (r_hi > c->nrows) r_hi = c->nrows;
    int64_t* cur = c->cursor + task_id * c->n_groups;
    const int64_t* row_gid = c->row_gid;
    int64_t* idx_buf = c->idx_buf;
    for (int64_t r = r_lo; r < r_hi; r++) {
        int64_t gi = row_gid[r];
        if (gi >= 0) idx_buf[cur[gi]++] = r;
    }
}

/* ============================================================================
 * Partition-aware group-by: detect parted columns, concatenate segments into
 * a flat table, then run standard exec_group once.
 * ============================================================================ */
ray_t* exec_group(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                  int64_t group_limit); /* forward decl */

/* Forward declaration — defined below exec_group */
static ray_t* exec_group_per_partition(ray_t* parted_tbl, ray_op_ext_t* ext,
                                       int32_t n_parts, const int64_t* key_syms,
                                       const int64_t* agg_syms, int has_avg,
                                       int has_stddev, int64_t group_limit);

/* --------------------------------------------------------------------------
 * exec_group_parted — dispatch per-partition or concat-fallback
 * -------------------------------------------------------------------------- */
static ray_t* exec_group_parted(ray_graph_t* g, ray_op_t* op, ray_t* parted_tbl,
                               int64_t group_limit) {
    int64_t ncols = ray_table_ncols(parted_tbl);
    if (ncols <= 0) return ray_error("nyi", NULL);

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    uint8_t n_keys = ext->n_keys;
    uint8_t n_aggs = ext->n_aggs;

    /* Find partition count and total rows from first parted column */
    int32_t n_parts = 0;
    int64_t total_rows = 0;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(parted_tbl, c);
        if (col && RAY_IS_PARTED(col->type)) {
            n_parts = (int32_t)col->len;
            total_rows = ray_parted_nrows(col);
            break;
        }
    }
    if (n_parts <= 0 || total_rows <= 0) return ray_error("nyi", NULL);

    /* Check eligibility for per-partition exec + merge:
     * - All keys and agg inputs must be simple SCANs
     * - Supported agg ops: SUM, COUNT, MIN, MAX, AVG, FIRST, LAST,
     *   STDDEV, STDDEV_POP, VAR, VAR_POP */
    int can_partition = g->selection ? 0 : 1;
    int has_avg = 0;
    int has_stddev = 0;
    int64_t key_syms[8];
    for (uint8_t k = 0; k < n_keys && can_partition; k++) {
        ray_op_ext_t* ke = find_ext(g, ext->keys[k]->id);
        if (!ke || ke->base.opcode != OP_SCAN) { can_partition = 0; break; }
        key_syms[k] = ke->sym;
    }
    int64_t agg_syms[8];
    for (uint8_t a = 0; a < n_aggs && can_partition; a++) {
        uint16_t aop = ext->agg_ops[a];
        /* Holistic aggs (OP_MEDIAN / OP_TOP_N / OP_BOT_N) can't be
         * merged across partitions without re-scanning underlying
         * values — decline per-partition exec.  Falls through to the
         * concat path which sees the full vector. */
        if (aop == OP_MEDIAN || aop == OP_TOP_N || aop == OP_BOT_N) {
            can_partition = 0; break;
        }
        if (aop != OP_SUM && aop != OP_COUNT && aop != OP_MIN &&
            aop != OP_MAX && aop != OP_AVG && aop != OP_FIRST &&
            aop != OP_LAST && aop != OP_STDDEV && aop != OP_STDDEV_POP &&
            aop != OP_VAR && aop != OP_VAR_POP) { can_partition = 0; break; }
        if (aop == OP_AVG) has_avg = 1;
        if (aop == OP_STDDEV || aop == OP_STDDEV_POP ||
            aop == OP_VAR || aop == OP_VAR_POP) has_stddev = 1;
        ray_op_ext_t* ae = find_ext(g, ext->agg_ins[a]->id);
        if (!ae || ae->base.opcode != OP_SCAN) { can_partition = 0; break; }
        agg_syms[a] = ae->sym;
    }

    /* Cardinality gate: estimate groups from first partition.
     * Per-partition only wins when #groups << partition_size. */
    if (can_partition) {
        int64_t rows_per_part = total_rows / n_parts;
        int64_t est_groups = 1;
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_t* pcol = ray_table_get_col(parted_tbl, key_syms[k]);
            if (!pcol) { est_groups = rows_per_part; break; }
            /* MAPCOMMON key: constant per partition — excluded from
             * per-partition sub-GROUP-BY, contributes 0 to cardinality. */
            if (pcol->type == RAY_MAPCOMMON) { continue; }
            if (!RAY_IS_PARTED(pcol->type)) { est_groups = rows_per_part; break; }
            ray_t* seg0 = ((ray_t**)ray_data(pcol))[0];
            if (!seg0 || seg0->len <= 0) { est_groups = rows_per_part; break; }
            int8_t bt = RAY_PARTED_BASETYPE(pcol->type);
            int64_t card;
            if (RAY_IS_SYM(bt)) {
                uint32_t sym_n = ray_sym_count();
                if (sym_n == 0 || sym_n > 4194304) { est_groups = rows_per_part; break; }
                size_t bwords = ((size_t)sym_n + 63) / 64;
                ray_t* bits_hdr = NULL;
                uint64_t* bits = (uint64_t*)scratch_calloc(&bits_hdr, bwords * 8);
                if (!bits) { est_groups = rows_per_part; break; }
                for (int64_t r = 0; r < seg0->len; r++) {
                    uint32_t id = (uint32_t)ray_read_sym(ray_data(seg0), r, seg0->type, seg0->attrs);
                    bits[id / 64] |= 1ULL << (id % 64);
                }
                card = 0;
                for (size_t i = 0; i < bwords; i++)
                    card += __builtin_popcountll(bits[i]);
                scratch_free(bits_hdr);
            } else if (bt == RAY_I64) {
                const int64_t* v = (const int64_t*)ray_data(seg0);
                int64_t lo = v[0], hi = v[0];
                for (int64_t r = 1; r < seg0->len; r++) {
                    if (v[r] < lo) lo = v[r];
                    if (v[r] > hi) hi = v[r];
                }
                card = hi - lo + 1;
            } else if (bt == RAY_I32) {
                const int32_t* v = (const int32_t*)ray_data(seg0);
                int32_t lo = v[0], hi = v[0];
                for (int64_t r = 1; r < seg0->len; r++) {
                    if (v[r] < lo) lo = v[r];
                    if (v[r] > hi) hi = v[r];
                }
                card = (int64_t)(hi - lo + 1);
            } else {
                card = seg0->len;
            }
            est_groups *= card;
            if (est_groups > rows_per_part) { est_groups = rows_per_part; break; }
        }
        /* Block per-partition when cardinality is high AND the concat
         * fallback would fit in memory (< 4 GB estimated).  When concat is
         * too large, per-partition with batched merge is the only option. */
        int64_t concat_bytes = total_rows * 8LL * (int64_t)(n_keys + n_aggs);
        if (est_groups * 100 > rows_per_part &&
            concat_bytes < 4LL * 1024 * 1024 * 1024)
            can_partition = 0;
    }

    /* Try per-partition path (separate noinline function to avoid I-cache pressure) */
    if (can_partition) {
        ray_t* result = exec_group_per_partition(parted_tbl, ext, n_parts,
                                                 key_syms, agg_syms, has_avg,
                                                 has_stddev, group_limit);
        if (result) return result;
        /* NULL = per-partition failed, fall through to concat */
    }

    /* ---- Concat fallback ---- */
    /* ---- Concat-only-needed-columns fallback ----
     * Used when query has AVG or expression keys/aggs.
     * Only concatenates the columns actually referenced by the GROUP BY. */
    {
        /* Collect needed column sym IDs (keys + agg inputs) */
        int64_t needed[16];
        int n_needed = 0;
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_op_ext_t* ke = find_ext(g, ext->keys[k]->id);
            if (ke && ke->base.opcode == OP_SCAN) {
                int dup = 0;
                for (int i = 0; i < n_needed; i++)
                    if (needed[i] == ke->sym) { dup = 1; break; }
                if (!dup) needed[n_needed++] = ke->sym;
            }
        }
        for (uint8_t a = 0; a < n_aggs; a++) {
            ray_op_ext_t* ae = find_ext(g, ext->agg_ins[a]->id);
            if (ae && ae->base.opcode == OP_SCAN) {
                int dup = 0;
                for (int i = 0; i < n_needed; i++)
                    if (needed[i] == ae->sym) { dup = 1; break; }
                if (!dup) needed[n_needed++] = ae->sym;
            } else {
                /* Expression agg input — need all columns for evaluation.
                 * Fall back to copying everything. */
                n_needed = 0;
                break;
            }
        }

        /* Build flat table with only needed columns (or all if n_needed==0) */
        ray_t* flat_tbl = ray_table_new(n_needed > 0 ? (int64_t)n_needed : ncols);
        if (!flat_tbl || RAY_IS_ERR(flat_tbl)) return flat_tbl;

        int64_t cols_to_iter = n_needed > 0 ? (int64_t)n_needed : ncols;
        for (int64_t ci = 0; ci < cols_to_iter; ci++) {
            ray_t* col;
            int64_t name_id;
            if (n_needed > 0) {
                col = ray_table_get_col(parted_tbl, needed[ci]);
                name_id = needed[ci];
            } else {
                col = ray_table_get_col_idx(parted_tbl, ci);
                name_id = ray_table_col_name(parted_tbl, ci);
            }
            if (!col) continue;
            if (col->type == RAY_MAPCOMMON) {
                ray_t* mc_flat = materialize_mapcommon(col);
                if (mc_flat && !RAY_IS_ERR(mc_flat)) {
                    flat_tbl = ray_table_add_col(flat_tbl, name_id, mc_flat);
                    ray_release(mc_flat);
                }
                continue;
            }

            if (!RAY_IS_PARTED(col->type)) {
                ray_retain(col);
                flat_tbl = ray_table_add_col(flat_tbl, name_id, col);
                ray_release(col);
                continue;
            }

            int8_t base_type = (int8_t)RAY_PARTED_BASETYPE(col->type);
            ray_t** segs = (ray_t**)ray_data(col);
            ray_t* flat;

            if (base_type == RAY_STR) {
                flat = parted_flatten_str(segs, col->len, total_rows);
            } else {
                uint8_t base_attrs = (base_type == RAY_SYM)
                                   ? parted_first_attrs(segs, col->len) : 0;
                flat = typed_vec_new(base_type, base_attrs, total_rows);
                if (!flat || RAY_IS_ERR(flat)) {
                    ray_release(flat_tbl);
                    return ray_error("oom", NULL);
                }
                flat->len = total_rows;

                size_t elem_size = (size_t)ray_sym_elem_size(base_type, base_attrs);
                int64_t offset = 0;
                for (int32_t p = 0; p < n_parts; p++) {
                    ray_t* seg = segs[p];
                    if (!seg || seg->len <= 0) continue;
                    if (parted_seg_esz_ok(seg, base_type, (uint8_t)elem_size)) {
                        memcpy((char*)ray_data(flat) + (size_t)offset * elem_size,
                               ray_data(seg), (size_t)seg->len * elem_size);
                    } else {
                        memset((char*)ray_data(flat) + (size_t)offset * elem_size,
                               0, (size_t)seg->len * elem_size);
                    }
                    offset += seg->len;
                }
            }
            if (!flat || RAY_IS_ERR(flat)) {
                ray_release(flat_tbl);
                return ray_error("oom", NULL);
            }

            flat_tbl = ray_table_add_col(flat_tbl, name_id, flat);
            ray_release(flat);
        }

        ray_t* saved = g->table;
        g->table = flat_tbl;
        ray_t* result = exec_group(g, op, flat_tbl, 0);
        g->table = saved;
        ray_release(flat_tbl);
        return result;
    }
}

ray_t* exec_group(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                  int64_t group_limit) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    /* Selection-shape guard — runs BEFORE any fast path (parted
     * dispatch, factorized shortcut) so every exec_group code path
     * sees the same validated selection state.  A mismatch here
     * indicates a graph-construction bug: the caller installed a
     * selection that was built for a different table shape, and
     * silently ignoring it would return unfiltered results. */
    if (g->selection) {
        ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
        int64_t tbl_nrows = ray_table_nrows(tbl);
        if (sm->nrows != tbl_nrows)
            return ray_error("domain",
                "exec_group: selection nrows mismatch (sel=%lld tbl=%lld)",
                (long long)sm->nrows, (long long)tbl_nrows);
    }

    /* Parted dispatch: detect parted input columns */
    {
        int64_t nc = ray_table_ncols(tbl);
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(tbl, c);
            if (col && (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON)) {
                return exec_group_parted(g, op, tbl, group_limit);
            }
        }
    }

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    int64_t nrows = ray_table_nrows(tbl);
    uint8_t n_keys = ext->n_keys;
    uint8_t n_aggs = ext->n_aggs;

    /* Factorized shortcut: if input is a factorized expand result with
     * (_src, _count) columns, and GROUP BY _src with COUNT/SUM(_count),
     * return the pre-aggregated table directly without re-scanning.
     *
     * Interaction with g->selection: the factorized _count column
     * encodes weighted counts, so COUNT(*) must SUM _count to get
     * the true row count and SUM(_count) is the same thing.
     * Neither the shortcut (returns verbatim, no filter) nor the
     * main path (counts rows of the _src table, ignoring _count)
     * knows how to apply a row filter while preserving those
     * semantics.
     *
     * Other agg shapes — SUM/AVG/MIN/MAX of a non-_count column,
     * etc. — don't rely on the factorized weighting; the main
     * path handles them correctly with the selection installed.
     * So the rejection must mirror the shortcut's exact
     * compatibility check (all aggs are COUNT or SUM(_count)),
     * not just the presence of a _count column. */
    if (g->selection && n_keys == 1 && n_aggs > 0 && nrows > 0) {
        int64_t cnt_sym_probe = ray_sym_intern("_count", 6);
        ray_t*  cnt_col_probe = ray_table_get_col(tbl, cnt_sym_probe);
        ray_op_ext_t* key_ext_probe = find_ext(g, ext->keys[0]->id);
        int64_t src_sym_probe = ray_sym_intern("_src", 4);
        if (cnt_col_probe && cnt_col_probe->type == RAY_I64 &&
            key_ext_probe && key_ext_probe->base.opcode == OP_SCAN &&
            key_ext_probe->sym == src_sym_probe) {
            /* Reject on ANY agg whose semantics depend on the
             * factorized _count weighting: COUNT(*) counts
             * underlying source rows (not _src table rows) and
             * SUM(_count) is equivalent.  Even if only one agg in
             * a mixed query needs weighting, the main path can't
             * handle it correctly, so fail the whole query rather
             * than return a mix of right and wrong columns.
             *
             * Special case: an empty selection (total_pass == 0)
             * means every row was filtered out, so the result is
             * an empty group set regardless of which aggs are
             * involved.  The main path handles this correctly
             * even for count-weighted aggs because n_scan == 0
             * produces no group rows at all.  Let it fall
             * through. */
            ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
            if (sm->total_pass > 0) {
                bool needs_weighting = false;
                for (uint8_t a = 0; a < n_aggs; a++) {
                    uint16_t aop = ext->agg_ops[a];
                    ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
                    if (aop == OP_COUNT) { needs_weighting = true; break; }
                    if (aop == OP_SUM && agg_ext &&
                        agg_ext->base.opcode == OP_SCAN &&
                        agg_ext->sym == cnt_sym_probe) {
                        needs_weighting = true; break;
                    }
                }
                if (needs_weighting)
                    return ray_error("nyi",
                        "GROUP BY with selection on factorized expand result "
                        "(COUNT/SUM(_count) semantics)");
            }
        }
    }
    if (!g->selection && n_keys == 1 && n_aggs > 0 && nrows > 0) {
        int64_t cnt_sym = ray_sym_intern("_count", 6);
        ray_t* cnt_col = ray_table_get_col(tbl, cnt_sym);
        if (cnt_col && cnt_col->type == RAY_I64) {
            ray_op_ext_t* key_ext = find_ext(g, ext->keys[0]->id);
            int64_t src_sym = ray_sym_intern("_src", 4);
            if (key_ext && key_ext->base.opcode == OP_SCAN &&
                key_ext->sym == src_sym) {
                /* Verify all aggs are compatible with factorized data:
                 * COUNT(*) → use _count directly
                 * SUM(_count) → use _count directly */
                bool all_compat = true;
                for (uint8_t a = 0; a < n_aggs; a++) {
                    uint16_t aop = ext->agg_ops[a];
                    ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
                    if (aop == OP_COUNT) continue;
                    if (aop == OP_SUM && agg_ext &&
                        agg_ext->base.opcode == OP_SCAN &&
                        agg_ext->sym == cnt_sym) continue;
                    all_compat = false;
                    break;
                }
                if (all_compat) {
                    /* The factorized table already has one row per group.
                     * Build result with _src key + agg columns from _count. */
                    ray_t* src_col = ray_table_get_col(tbl, src_sym);
                    if (src_col) {
                        int64_t out_nkeys = 1;
                        int64_t out_ncols = out_nkeys + n_aggs;
                        ray_t* result = ray_table_new((int64_t)out_ncols);
                        if (!result || RAY_IS_ERR(result))
                            return ray_error("oom", NULL);
                        ray_retain(src_col);
                        ray_t* tmp_r = ray_table_add_col(result, src_sym, src_col);
                        ray_release(src_col);
                        if (!tmp_r || RAY_IS_ERR(tmp_r)) {
                            ray_release(result);
                            return ray_error("oom", NULL);
                        }
                        result = tmp_r;
                        for (uint8_t a = 0; a < n_aggs; a++) {
                            ray_retain(cnt_col);
                            int64_t agg_name = ray_sym_intern("_agg", 4);
                            if (n_aggs > 1) {
                                char buf[16];
                                int n = snprintf(buf, sizeof(buf), "_agg%d", a);
                                agg_name = ray_sym_intern(buf, (size_t)n);
                            }
                            tmp_r = ray_table_add_col(result, agg_name, cnt_col);
                            ray_release(cnt_col);
                            if (!tmp_r || RAY_IS_ERR(tmp_r)) {
                                ray_release(result);
                                return ray_error("oom", NULL);
                            }
                            result = tmp_r;
                        }
                        return result;
                    }
                }
            }
        }
    }

    if (n_keys > 8 || n_aggs > 8) return ray_error("nyi", NULL);

    /* Extract selection (rowsel) for pushdown.  Prefer streaming the
     * morsel-local rowsel directly; flattening to int64 indices is kept
     * only as a fallback for callers that still pass match_idx. */
    ray_t* match_idx_block = NULL;
    const int64_t* match_idx = NULL;
    ray_t* rowsel = NULL;
    int64_t n_scan = nrows;
    if (g->selection) {
        rowsel = g->selection;
    }

    /* Resolve key columns (VLA — n_keys ≤ 8; use ≥1 to avoid zero-size VLA UB) */
    uint8_t vla_keys = n_keys > 0 ? n_keys : 1;
    ray_t* key_vecs[vla_keys];
    memset(key_vecs, 0, vla_keys * sizeof(ray_t*));

    uint8_t key_owned[vla_keys]; /* 1 = we allocated via exec_node, must free */
    memset(key_owned, 0, vla_keys * sizeof(uint8_t));
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_op_t* key_op = ext->keys[k];
        ray_op_ext_t* key_ext = find_ext(g, key_op->id);
        if (key_ext && key_ext->base.opcode == OP_SCAN) {
            key_vecs[k] = ray_table_get_col(tbl, key_ext->sym);
        } else {
            /* Expression key (CASE WHEN etc) — evaluate against current tbl */
            ray_t* saved_table = g->table;
            g->table = tbl;
            ray_t* vec = exec_node(g, key_op);
            g->table = saved_table;
            if (vec && !RAY_IS_ERR(vec)) {
                key_vecs[k] = vec;
                key_owned[k] = 1;
            }
        }
        if (!key_vecs[k]) {
            for (uint8_t j = 0; j < k; j++)
                if (key_owned[j] && key_vecs[j]) ray_release(key_vecs[j]);
            return ray_error("domain", "by: column not found in table");
        }
    }

    /* Resolve agg input columns (VLA — n_aggs ≤ 8; use ≥1 to avoid zero-size VLA UB) */
    uint8_t vla_aggs = n_aggs > 0 ? n_aggs : 1;
    ray_t* agg_vecs[vla_aggs];
    /* Second input column per agg — non-NULL only for binary aggs
     * (OP_PEARSON_CORR).  Allocated independently of agg_vecs because
     * agg_owned2 may differ (each side can come from a different source
     * — OP_SCAN literal or expr_compile). */
    ray_t* agg_vecs2[vla_aggs];
    uint8_t agg_owned[vla_aggs]; /* 1 = we allocated via exec_node, must free */
    uint8_t agg_owned2[vla_aggs];
    uint8_t agg_strlen[vla_aggs];
    agg_affine_t agg_affine[vla_aggs];
    agg_linear_t agg_linear[vla_aggs];
    memset(agg_vecs, 0, vla_aggs * sizeof(ray_t*));
    memset(agg_vecs2, 0, vla_aggs * sizeof(ray_t*));
    memset(agg_owned, 0, vla_aggs * sizeof(uint8_t));
    memset(agg_owned2, 0, vla_aggs * sizeof(uint8_t));
    memset(agg_strlen, 0, vla_aggs * sizeof(uint8_t));
    memset(agg_affine, 0, vla_aggs * sizeof(agg_affine_t));
    memset(agg_linear, 0, vla_aggs * sizeof(agg_linear_t));

    for (uint8_t a = 0; a < n_aggs; a++) {
        ray_op_t* agg_input_op = ext->agg_ins[a];
        ray_op_ext_t* agg_ext = find_ext(g, agg_input_op->id);

        /* SUM/AVG(scan +/- const): aggregate base scan and apply bias at emit. */
        uint16_t agg_kind = ext->agg_ops[a];
        if ((agg_kind == OP_SUM || agg_kind == OP_AVG) &&
            try_affine_sumavg_input(g, tbl, agg_input_op, &agg_vecs[a], &agg_affine[a])) {
            continue;
        }

        if ((agg_kind == OP_SUM || agg_kind == OP_AVG) &&
            try_strlen_sumavg_input(g, tbl, agg_input_op, &agg_vecs[a])) {
            agg_strlen[a] = 1;
            continue;
        }

        /* SUM/AVG(integer-linear expr): scalar path can aggregate directly
         * without materializing the expression vector. */
        if (n_keys == 0 && nrows > 0 &&
            (agg_kind == OP_SUM || agg_kind == OP_AVG) &&
            try_linear_sumavg_input_i64(g, tbl, agg_input_op, &agg_linear[a])) {
            continue;
        }

        if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
            agg_vecs[a] = ray_table_get_col(tbl, agg_ext->sym);
        } else if (agg_ext && agg_ext->base.opcode == OP_CONST && agg_ext->literal) {
            agg_vecs[a] = agg_ext->literal;
        } else {
            /* Expression node (ADD/MUL etc) — try compiled expression first */
            ray_expr_t agg_expr;
            if (expr_compile(g, tbl, agg_input_op, &agg_expr)) {
                ray_t* vec = expr_eval_full(&agg_expr, nrows);
                if (vec && !RAY_IS_ERR(vec)) {
                    agg_vecs[a] = vec;
                    agg_owned[a] = 1;
                    goto resolve_ins2;
                }
            }
            /* Fallback: full recursive evaluation */
            ray_t* saved_table = g->table;
            g->table = tbl;
            ray_t* vec = exec_node(g, agg_input_op);
            g->table = saved_table;
            if (vec && !RAY_IS_ERR(vec)) {
                agg_vecs[a] = vec;
                agg_owned[a] = 1;
            }
        }
    resolve_ins2:;
        /* Binary aggregators (OP_PEARSON_CORR): mirror the resolution
         * above for the y-side input.  Same OP_SCAN / OP_CONST / expr
         * fallback ladder, separate ownership flag because each side
         * may have come from a different source. */
        if (ext->agg_ins2 && ext->agg_ins2[a]) {
            ray_op_t* agg_input_op2 = ext->agg_ins2[a];
            ray_op_ext_t* agg_ext2 = find_ext(g, agg_input_op2->id);
            if (agg_ext2 && agg_ext2->base.opcode == OP_SCAN) {
                agg_vecs2[a] = ray_table_get_col(tbl, agg_ext2->sym);
            } else if (agg_ext2 && agg_ext2->base.opcode == OP_CONST && agg_ext2->literal) {
                agg_vecs2[a] = agg_ext2->literal;
            } else {
                ray_expr_t agg_expr2;
                int compiled2 = 0;
                if (expr_compile(g, tbl, agg_input_op2, &agg_expr2)) {
                    ray_t* vec = expr_eval_full(&agg_expr2, nrows);
                    if (vec && !RAY_IS_ERR(vec)) {
                        agg_vecs2[a] = vec;
                        agg_owned2[a] = 1;
                        compiled2 = 1;
                    }
                }
                if (!compiled2) {
                    ray_t* saved_table = g->table;
                    g->table = tbl;
                    ray_t* vec = exec_node(g, agg_input_op2);
                    g->table = saved_table;
                    if (vec && !RAY_IS_ERR(vec)) {
                        agg_vecs2[a] = vec;
                        agg_owned2[a] = 1;
                    }
                }
            }
        }
    }

    /* Normalize scalar agg inputs to full-length vectors.
     * Constants and scalar sub-expressions (len=1) must be broadcast to nrows
     * before row-wise aggregation loops. */
    for (uint8_t a = 0; a < n_aggs; a++) {
        if (!agg_vecs[a] || RAY_IS_ERR(agg_vecs[a])) continue;
        if (ext->agg_ops[a] == OP_COUNT) continue; /* value is ignored for COUNT */
        if (agg_strlen[a]) continue;

        bool needs_broadcast = ray_is_atom(agg_vecs[a]) ||
                               (agg_vecs[a]->type > 0 && agg_vecs[a]->len == 1 && nrows > 1);
        if (!needs_broadcast) continue;

        ray_t* bcast = materialize_broadcast_input(agg_vecs[a], nrows);
        if (!bcast || RAY_IS_ERR(bcast)) {
            for (uint8_t i = 0; i < n_aggs; i++) {
                { if (agg_owned[i] && agg_vecs[i]) ray_release(agg_vecs[i]); if (agg_owned2[i] && agg_vecs2[i]) ray_release(agg_vecs2[i]); }
            }
            for (uint8_t k = 0; k < n_keys; k++) {
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            }
            return bcast && RAY_IS_ERR(bcast) ? bcast : ray_error("oom", NULL);
        }

        if (agg_owned[a]) ray_release(agg_vecs[a]);
        agg_vecs[a] = bcast;
        agg_owned[a] = 1;
    }

    /* Pre-compute key metadata (VLA — n_keys ≤ 8; vla_keys ≥ 1) */
    void* key_data[vla_keys];
    int8_t key_types[vla_keys];
    uint8_t key_attrs[vla_keys];
    for (uint8_t k = 0; k < n_keys; k++) {
        if (key_vecs[k]) {
            key_data[k]  = ray_data(key_vecs[k]);
            key_types[k] = key_vecs[k]->type;
            key_attrs[k] = key_vecs[k]->attrs;
        } else {
            key_data[k]  = NULL;
            key_types[k] = 0;
            key_attrs[k] = 0;
        }
    }
    ray_group_emit_filter_t emit_filter = ray_group_emit_filter_get();
    bool use_emit_filter = emit_filter.enabled &&
        emit_filter.agg_index < n_aggs &&
        ext->agg_ops[emit_filter.agg_index] == OP_COUNT;

    /* ---- Scalar aggregate fast path (n_keys == 0): flat vector scan ---- */
    if (n_keys == 0 && nrows > 0) {
        uint8_t need_flags = DA_NEED_COUNT;
        bool has_first_last = false;
        for (uint8_t a = 0; a < n_aggs; a++) {
            uint16_t aop = ext->agg_ops[a];
            if (aop == OP_SUM || aop == OP_PROD || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST)
                need_flags |= DA_NEED_SUM;
            else if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
                { need_flags |= DA_NEED_SUM; need_flags |= DA_NEED_SUMSQ; }
            else if (aop == OP_MIN) need_flags |= DA_NEED_MIN;
            else if (aop == OP_MAX) need_flags |= DA_NEED_MAX;
            if (aop == OP_FIRST || aop == OP_LAST) has_first_last = true;
        }

        void* agg_ptrs[vla_aggs];
        int8_t agg_types[vla_aggs];
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (agg_vecs[a]) {
                agg_ptrs[a]  = ray_data(agg_vecs[a]);
                agg_types[a] = agg_vecs[a]->type;
            } else {
                agg_ptrs[a]  = NULL;
                agg_types[a] = 0;
            }
        }

        ray_pool_t* sc_pool = ray_pool_get();
        /* Pool dispatch is work-stealing: chunks may be processed out of
         * row-index order across workers, so the "count[0]==1" sentinel
         * scalar_accum_row uses for FIRST (and the always-overwrite for
         * LAST) only yields the per-worker first/last, not the global
         * one.  The merge step then picks worker[0]'s FIRST regardless
         * of which range it actually covered.  Force serial execution
         * when FIRST/LAST is in play; the DA path (which does track
         * per-slot row bounds) is still preferred when we have keys. */
        uint32_t sc_n = (sc_pool && nrows >= RAY_PARALLEL_THRESHOLD && !has_first_last)
                        ? ray_pool_total_workers(sc_pool) : 1;

        ray_t* sc_hdr;
        da_accum_t* sc_acc = (da_accum_t*)scratch_calloc(&sc_hdr,
            sc_n * sizeof(da_accum_t));
        if (!sc_acc) goto da_path;

        /* Allocate 1-slot accumulators per worker (n_aggs entries) */
        bool alloc_ok = true;
        for (uint32_t w = 0; w < sc_n; w++) {
            if (need_flags & DA_NEED_SUM) {
                sc_acc[w].sum = (da_val_t*)scratch_calloc(&sc_acc[w]._h_sum,
                    n_aggs * sizeof(da_val_t));
                if (!sc_acc[w].sum) { alloc_ok = false; break; }
            }
            if (need_flags & DA_NEED_MIN) {
                sc_acc[w].min_val = (da_val_t*)scratch_alloc(&sc_acc[w]._h_min,
                    n_aggs * sizeof(da_val_t));
                if (!sc_acc[w].min_val) { alloc_ok = false; break; }
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) sc_acc[w].min_val[a].f = DBL_MAX;
                    else sc_acc[w].min_val[a].i = INT64_MAX;
                }
            }
            if (need_flags & DA_NEED_MAX) {
                sc_acc[w].max_val = (da_val_t*)scratch_alloc(&sc_acc[w]._h_max,
                    n_aggs * sizeof(da_val_t));
                if (!sc_acc[w].max_val) { alloc_ok = false; break; }
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) sc_acc[w].max_val[a].f = -DBL_MAX;
                    else sc_acc[w].max_val[a].i = INT64_MIN;
                }
            }
            if (need_flags & DA_NEED_SUMSQ) {
                sc_acc[w].sumsq_f64 = (double*)scratch_calloc(&sc_acc[w]._h_sumsq,
                    n_aggs * sizeof(double));
                if (!sc_acc[w].sumsq_f64) { alloc_ok = false; break; }
            }
            sc_acc[w].count = (int64_t*)scratch_calloc(&sc_acc[w]._h_count,
                1 * sizeof(int64_t));
            if (!sc_acc[w].count) { alloc_ok = false; break; }
        }
        if (!alloc_ok) {
            for (uint32_t w = 0; w < sc_n; w++) da_accum_free(&sc_acc[w]);
            scratch_free(sc_hdr);
            goto da_path;
        }

        scalar_ctx_t sc_ctx = {
            .agg_ptrs   = agg_ptrs,
            .agg_types  = agg_types,
            .agg_cols   = agg_vecs,
            .agg_strlen = agg_strlen,
            .agg_ops    = ext->agg_ops,
            .agg_linear = agg_linear,
            .n_aggs     = n_aggs,
            .need_flags = need_flags,
            .match_idx  = match_idx,
            .rowsel     = rowsel,
            .accums     = sc_acc,
            .n_accums   = sc_n,
        };

        /* Pick specialized tight loop when possible, else generic.
         * The specialized scalar_sum_*_fn variants don't honour
         * match_idx — they read data[r] directly — so they're only
         * safe when no selection is in flight. */
        typedef void (*scalar_fn_t)(void*, uint32_t, int64_t, int64_t);
        scalar_fn_t sc_fn = scalar_accum_fn;
        if (n_aggs == 1 && !match_idx && !rowsel && agg_ptrs[0] != NULL) {
            uint16_t op0 = ext->agg_ops[0];
            int8_t   t0  = agg_types[0];
            if ((op0 == OP_SUM || op0 == OP_AVG) &&
                (t0 == RAY_I64 || t0 == RAY_SYM || t0 == RAY_TIMESTAMP))
                sc_fn = scalar_sum_i64_fn;
            else if ((op0 == OP_SUM || op0 == OP_AVG) && t0 == RAY_F64)
                sc_fn = scalar_sum_f64_fn;
        } else if (n_aggs == 1 && !match_idx && !rowsel && agg_linear[0].enabled) {
            uint16_t op0 = ext->agg_ops[0];
            if (op0 == OP_SUM || op0 == OP_AVG)
                sc_fn = scalar_sum_linear_i64_fn;
        }

        if (sc_n > 1)
            ray_pool_dispatch(sc_pool, sc_fn, &sc_ctx, n_scan);
        else
            sc_fn(&sc_ctx, 0, 0, n_scan);

        /* Merge per-worker accumulators into sc_acc[0] */
        da_accum_t* m = &sc_acc[0];
        for (uint32_t w = 1; w < sc_n; w++) {
            da_accum_t* wa = &sc_acc[w];
            if (need_flags & DA_NEED_SUM) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    uint16_t merge_op = ext->agg_ops[a];
                    if (merge_op == OP_FIRST) {
                        if (m->count[0] == 0 && wa->count[0] > 0)
                            m->sum[a] = wa->sum[a];
                    } else if (merge_op == OP_LAST) {
                        if (wa->count[0] > 0)
                            m->sum[a] = wa->sum[a];
                    } else if (merge_op == OP_PROD) {
                        if (wa->count[0] > 0) {
                            if (m->count[0] == 0)
                                m->sum[a] = wa->sum[a];
                            else if (agg_types[a] == RAY_F64)
                                m->sum[a].f *= wa->sum[a].f;
                            else
                                m->sum[a].i = (int64_t)((uint64_t)m->sum[a].i * (uint64_t)wa->sum[a].i);
                        }
                    } else {
                        if (agg_types[a] == RAY_F64)
                            m->sum[a].f += wa->sum[a].f;
                        else
                            m->sum[a].i += wa->sum[a].i;
                    }
                }
            }
            if (need_flags & DA_NEED_SUMSQ) {
                for (uint8_t a = 0; a < n_aggs; a++)
                    m->sumsq_f64[a] += wa->sumsq_f64[a];
            }
            if (need_flags & DA_NEED_MIN) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) {
                        if (wa->min_val[a].f < m->min_val[a].f)
                            m->min_val[a].f = wa->min_val[a].f;
                    } else {
                        if (wa->min_val[a].i < m->min_val[a].i)
                            m->min_val[a].i = wa->min_val[a].i;
                    }
                }
            }
            if (need_flags & DA_NEED_MAX) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) {
                        if (wa->max_val[a].f > m->max_val[a].f)
                            m->max_val[a].f = wa->max_val[a].f;
                    } else {
                        if (wa->max_val[a].i > m->max_val[a].i)
                            m->max_val[a].i = wa->max_val[a].i;
                    }
                }
            }
            m->count[0] += wa->count[0];
        }
        for (uint32_t w = 1; w < sc_n; w++) da_accum_free(&sc_acc[w]);

        /* Emit 1-row result with no key columns */
        ray_t* result = ray_table_new(n_aggs);
        if (!result || RAY_IS_ERR(result)) {
            da_accum_free(&sc_acc[0]); scratch_free(sc_hdr);
            for (uint8_t a = 0; a < n_aggs; a++)
                { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
            for (uint8_t k = 0; k < n_keys; k++)
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            if (match_idx_block) ray_release(match_idx_block);
            return result ? result : ray_error("oom", NULL);
        }

        emit_agg_columns(&result, g, ext, agg_vecs, 1, n_aggs,
                         (double*)m->sum, (int64_t*)m->sum,
                         (double*)m->min_val, (double*)m->max_val,
                         (int64_t*)m->min_val, (int64_t*)m->max_val,
                         m->count, agg_affine, m->sumsq_f64);

        da_accum_free(&sc_acc[0]); scratch_free(sc_hdr);
        for (uint8_t a = 0; a < n_aggs; a++)
            { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
        for (uint8_t k = 0; k < n_keys; k++)
            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
        if (match_idx_block) ray_release(match_idx_block);
        return result;
    }

da_path:;
    /* ---- Direct-array fast path for low-cardinality integer keys ---- */
    /* Supports multi-key via composite index: product of ranges <= MAX */
    #define DA_MAX_COMPOSITE_SLOTS 262144  /* 256K slots max */
    #define DA_MEM_BUDGET      (256ULL << 20)  /* 256 MB total across all workers */
    #define DA_PER_WORKER_MAX  (6ULL << 20)    /* 6 MB per-worker max */
    {
        bool da_eligible = (nrows > 0 && n_keys > 0 && n_keys <= 8);
        /* Binary aggregators (OP_PEARSON_CORR) are not wired into the
         * dense-array accumulator's per-worker da_accum_t struct — force
         * the HT path which has the row-layout offsets allocated.
         * Holistic aggregators (OP_MEDIAN / OP_TOP_N / OP_BOT_N) have
         * no per-row accumulator at all — they need the post-radix
         * row_gid+grp_cnt pass which only the HT path provides. */
        for (uint8_t a = 0; a < n_aggs && da_eligible; a++) {
            if (ext->agg_ops[a] == OP_PEARSON_CORR) da_eligible = false;
            if (ext->agg_ops[a] == OP_MEDIAN)       da_eligible = false;
            if (ext->agg_ops[a] == OP_TOP_N)        da_eligible = false;
            if (ext->agg_ops[a] == OP_BOT_N)        da_eligible = false;
        }
        for (uint8_t k = 0; k < n_keys && da_eligible; k++) {
            if (!key_data[k]) { da_eligible = false; break; }
            int8_t t = key_types[k];
            if (t != RAY_I64 && t != RAY_SYM && t != RAY_I32
                && t != RAY_TIMESTAMP && t != RAY_DATE && t != RAY_TIME
                && t != RAY_BOOL && t != RAY_U8 && t != RAY_I16) {
                da_eligible = false;
            }
            /* DA path cannot represent nulls — fall back to HT path. */
            if (key_vecs[k]) {
                ray_t* src = (key_vecs[k]->attrs & RAY_ATTR_SLICE)
                             ? key_vecs[k]->slice_parent : key_vecs[k];
                if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
                    da_eligible = false;
            }
        }

        int64_t da_key_min[8], da_key_range[8], da_key_stride[8];
        uint64_t total_slots = 1;
        bool da_fits = false;


        if (da_eligible) {
            da_fits = true;
            ray_pool_t* mm_pool = ray_pool_get();
            uint32_t mm_n = (mm_pool && nrows >= RAY_PARALLEL_THRESHOLD)
                            ? ray_pool_total_workers(mm_pool) : 1;
            /* VLA bounded by worker count — max ~2KB per key even on 256-core systems. */
            int64_t mm_mins[mm_n], mm_maxs[mm_n];
            for (uint8_t k = 0; k < n_keys && da_fits; k++) {
                int64_t kmin, kmax;
                for (uint32_t w = 0; w < mm_n; w++) {
                    mm_mins[w] = INT64_MAX;
                    mm_maxs[w] = INT64_MIN;
                }
                minmax_ctx_t mm_ctx = {
                    .key_data       = key_data[k],
                    .key_type       = key_types[k],
                    .key_attrs      = key_attrs[k],
                    .per_worker_min = mm_mins,
                    .per_worker_max = mm_maxs,
                    .n_workers      = mm_n,
                    .match_idx      = match_idx,
                    .rowsel         = rowsel,
                };
                if (mm_n > 1) {
                    ray_pool_dispatch(mm_pool, minmax_scan_fn, &mm_ctx, n_scan);
                } else {
                    minmax_scan_fn(&mm_ctx, 0, 0, n_scan);
                }
                kmin = INT64_MAX; kmax = INT64_MIN;
                for (uint32_t w = 0; w < mm_n; w++) {
                    if (mm_mins[w] < kmin) kmin = mm_mins[w];
                    if (mm_maxs[w] > kmax) kmax = mm_maxs[w];
                }
                da_key_min[k]   = kmin;
                /* kmax - kmin may overflow i64 when keys span full range.
                 * Compute in uint64_t and reject if the span exceeds i64. */
                uint64_t span = (uint64_t)kmax - (uint64_t)kmin + 1;
                if (span > (uint64_t)INT64_MAX) { da_fits = false; break; }
                da_key_range[k] = (int64_t)span;
                if (da_key_range[k] <= 0) { da_fits = false; break; }
                total_slots *= (uint64_t)da_key_range[k];
                if (total_slots > DA_MAX_COMPOSITE_SLOTS) da_fits = false;
            }
        }

        if (da_fits) {
            /* Compute which accumulator arrays we actually need */
            uint8_t need_flags = DA_NEED_COUNT; /* always need count */
            for (uint8_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop == OP_SUM || aop == OP_PROD || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST) need_flags |= DA_NEED_SUM;
                else if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
                    { need_flags |= DA_NEED_SUM; need_flags |= DA_NEED_SUMSQ; }
                else if (aop == OP_MIN) need_flags |= DA_NEED_MIN;
                else if (aop == OP_MAX) need_flags |= DA_NEED_MAX;
            }

            /* Compute per-worker memory budget.  Actual allocation is 1 union
             * array per type, but MIN/MAX use conditional random writes that
             * perform worse than radix-partitioned HT at high group counts.
             * Weight MIN/MAX at 2x to keep those queries on the HT path. */
            uint32_t arrays_per_agg = 0;
            if (need_flags & DA_NEED_SUM) arrays_per_agg += 1;
            if (need_flags & DA_NEED_MIN) arrays_per_agg += 2; /* 2x: DA MIN slow at high cardinality */
            if (need_flags & DA_NEED_MAX) arrays_per_agg += 2; /* 2x: DA MAX slow at high cardinality */
            if (need_flags & DA_NEED_SUMSQ) arrays_per_agg += 1;
            uint64_t per_worker = total_slots * (arrays_per_agg * n_aggs + 1u) * 8u;
            if (per_worker > DA_PER_WORKER_MAX)
                da_fits = false;
        }

        if (da_fits) {
            /* Recompute need_flags (da_fits may have changed scope) */
            uint8_t need_flags = DA_NEED_COUNT;
            bool all_sum = true;
            bool da_has_first_last = false;
            for (uint8_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop == OP_SUM || aop == OP_PROD || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST) need_flags |= DA_NEED_SUM;
                else if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
                    { need_flags |= DA_NEED_SUM; need_flags |= DA_NEED_SUMSQ; }
                else if (aop == OP_MIN) need_flags |= DA_NEED_MIN;
                else if (aop == OP_MAX) need_flags |= DA_NEED_MAX;
                if (aop != OP_SUM && aop != OP_AVG && aop != OP_COUNT)
                    all_sum = false;
                if (aop == OP_FIRST || aop == OP_LAST) da_has_first_last = true;
            }

            /* Compute strides: stride[k] = product of ranges[k+1..n_keys-1]
             * Guard against overflow: if any product exceeds INT64_MAX,
             * fall through to HT path. */
            bool stride_overflow = false;
            for (uint8_t k = 0; k < n_keys; k++) {
                int64_t s = 1;
                for (uint8_t j = k + 1; j < n_keys; j++) {
                    if (da_key_range[j] != 0 && s > INT64_MAX / da_key_range[j]) {
                        stride_overflow = true; break;
                    }
                    s *= da_key_range[j];
                }
                if (stride_overflow) break;
                da_key_stride[k] = s;
            }
            if (stride_overflow) da_fits = false;

            uint32_t n_slots = (uint32_t)total_slots;
            size_t total = (size_t)n_slots * n_aggs;

            void* agg_ptrs[vla_aggs];
            int8_t agg_types[vla_aggs];
            uint32_t agg_f64_mask = 0;
            for (uint8_t a = 0; a < n_aggs; a++) {
                if (agg_vecs[a]) {
                    agg_ptrs[a]  = ray_data(agg_vecs[a]);
                    agg_types[a] = agg_vecs[a]->type;
                    if (agg_vecs[a]->type == RAY_F64)
                        agg_f64_mask |= (1u << a);
                } else {
                    agg_ptrs[a]  = NULL;
                    agg_types[a] = 0;
                }
            }

            ray_pool_t* da_pool = ray_pool_get();
            uint32_t da_n_workers = (da_pool && nrows >= RAY_PARALLEL_THRESHOLD)
                                    ? ray_pool_total_workers(da_pool) : 1;

            /* Check memory budget — need one accumulator set per worker.
             * Weight MIN/MAX at 2x in budget (same as eligibility check) to
             * keep MIN/MAX-heavy queries on the faster radix-HT path. */
            uint32_t arrays_per_agg = 0;
            if (need_flags & DA_NEED_SUM) arrays_per_agg += 1;
            if (need_flags & DA_NEED_MIN) arrays_per_agg += 2;
            if (need_flags & DA_NEED_MAX) arrays_per_agg += 2;
            if (need_flags & DA_NEED_SUMSQ) arrays_per_agg += 1;
            uint64_t per_worker_bytes = (uint64_t)n_slots * (arrays_per_agg * n_aggs + 1u) * 8u;
            if ((uint64_t)da_n_workers * per_worker_bytes > DA_MEM_BUDGET)
                da_n_workers = 1;

            ray_t* accums_hdr;
            da_accum_t* accums = (da_accum_t*)scratch_calloc(&accums_hdr,
                da_n_workers * sizeof(da_accum_t));
            if (!accums) goto ht_path;

            bool alloc_ok = true;
            for (uint32_t w = 0; w < da_n_workers; w++) {
                if (need_flags & DA_NEED_SUM) {
                    accums[w].sum = (da_val_t*)scratch_calloc(&accums[w]._h_sum,
                        total * sizeof(da_val_t));
                    if (!accums[w].sum) { alloc_ok = false; break; }
                }
                if (need_flags & DA_NEED_SUMSQ) {
                    accums[w].sumsq_f64 = (double*)scratch_calloc(&accums[w]._h_sumsq,
                        total * sizeof(double));
                    if (!accums[w].sumsq_f64) { alloc_ok = false; break; }
                }
                if (need_flags & DA_NEED_MIN) {
                    accums[w].min_val = (da_val_t*)scratch_alloc(&accums[w]._h_min,
                        total * sizeof(da_val_t));
                    if (!accums[w].min_val) { alloc_ok = false; break; }
                    for (size_t i = 0; i < total; i++) {
                        uint8_t a = (uint8_t)(i % n_aggs);
                        if (agg_types[a] == RAY_F64) accums[w].min_val[i].f = DBL_MAX;
                        else accums[w].min_val[i].i = INT64_MAX;
                    }
                }
                if (need_flags & DA_NEED_MAX) {
                    accums[w].max_val = (da_val_t*)scratch_alloc(&accums[w]._h_max,
                        total * sizeof(da_val_t));
                    if (!accums[w].max_val) { alloc_ok = false; break; }
                    for (size_t i = 0; i < total; i++) {
                        uint8_t a = (uint8_t)(i % n_aggs);
                        if (agg_types[a] == RAY_F64) accums[w].max_val[i].f = -DBL_MAX;
                        else accums[w].max_val[i].i = INT64_MIN;
                    }
                }
                accums[w].count = (int64_t*)scratch_calloc(&accums[w]._h_count,
                    n_slots * sizeof(int64_t));
                if (!accums[w].count) { alloc_ok = false; break; }
                if (da_has_first_last) {
                    accums[w].first_row = (int64_t*)scratch_alloc(
                        &accums[w]._h_first_row, n_slots * sizeof(int64_t));
                    if (!accums[w].first_row) { alloc_ok = false; break; }
                    for (uint32_t s = 0; s < n_slots; s++)
                        accums[w].first_row[s] = INT64_MAX;
                    accums[w].last_row = (int64_t*)scratch_alloc(
                        &accums[w]._h_last_row, n_slots * sizeof(int64_t));
                    if (!accums[w].last_row) { alloc_ok = false; break; }
                    for (uint32_t s = 0; s < n_slots; s++)
                        accums[w].last_row[s] = INT64_MIN;
                }
            }
            if (!alloc_ok) {
                for (uint32_t w = 0; w < da_n_workers; w++)
                    da_accum_free(&accums[w]);
                scratch_free(accums_hdr);
                goto ht_path;
            }


            /* Pre-compute per-key element sizes for fast DA reads */
            uint8_t da_key_esz[n_keys];
            for (uint8_t k = 0; k < n_keys; k++)
                da_key_esz[k] = ray_sym_elem_size(key_types[k], key_attrs[k]);

            da_ctx_t da_ctx = {
                .accums      = accums,
                .n_accums    = da_n_workers,
                .key_ptrs    = key_data,
                .key_types   = key_types,
                .key_attrs   = key_attrs,
                .key_esz     = da_key_esz,
                .key_mins    = da_key_min,
                .key_strides = da_key_stride,
                .n_keys      = n_keys,
                .agg_ptrs    = agg_ptrs,
                .agg_types   = agg_types,
                .agg_cols    = agg_vecs,
                .agg_strlen  = agg_strlen,
                .agg_ops     = ext->agg_ops,
                .n_aggs      = n_aggs,
                .need_flags  = need_flags,
                .agg_f64_mask = agg_f64_mask,
                .all_sum     = all_sum,
                .n_slots     = n_slots,
                .match_idx   = match_idx,
                .rowsel      = rowsel,
            };

            if (da_n_workers > 1)
                ray_pool_dispatch(da_pool, da_accum_fn, &da_ctx, n_scan);
            else
                da_accum_fn(&da_ctx, 0, 0, n_scan);

            /* Merge target is always accums[0] */
            da_accum_t* merged = &accums[0];

            /* Check if any agg is FIRST/LAST (needs ordered per-worker merge) */
            bool has_first_last = false;
            for (uint8_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop == OP_FIRST || aop == OP_LAST) { has_first_last = true; break; }
            }

            /* Merge per-worker accumulators into accums[0].
             * FIRST/LAST need row-index-aware merge: pool dispatch is
             * work-stealing, so worker_id ordering does not reflect global
             * row order.  Use per-slot first_row/last_row to pick the
             * worker whose entry has the smallest/largest row index. */
            if (has_first_last) {
                for (uint32_t w = 1; w < da_n_workers; w++) {
                    da_accum_t* wa = &accums[w];
                    if (need_flags & DA_NEED_SUMSQ) {
                        for (size_t i = 0; i < total; i++)
                            merged->sumsq_f64[i] += wa->sumsq_f64[i];
                    }
                    if (need_flags & DA_NEED_SUM) {
                        for (uint32_t s = 0; s < n_slots; s++) {
                            size_t base = (size_t)s * n_aggs;
                            bool take_first = wa->first_row && merged->first_row &&
                                wa->first_row[s] < merged->first_row[s];
                            bool take_last  = wa->last_row && merged->last_row &&
                                wa->last_row[s]  > merged->last_row[s];
                            for (uint8_t a = 0; a < n_aggs; a++) {
                                size_t idx = base + a;
                                uint16_t aop = ext->agg_ops[a];
                                if (aop == OP_SUM || aop == OP_AVG || aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP) {
                                    if (agg_types[a] == RAY_F64) merged->sum[idx].f += wa->sum[idx].f;
                                    else merged->sum[idx].i += wa->sum[idx].i;
                                } else if (aop == OP_PROD) {
                                    if (wa->count[s] > 0) {
                                        if (merged->count[s] == 0)
                                            merged->sum[idx] = wa->sum[idx];
                                        else if (agg_types[a] == RAY_F64)
                                            merged->sum[idx].f *= wa->sum[idx].f;
                                        else
                                            merged->sum[idx].i = (int64_t)((uint64_t)merged->sum[idx].i * (uint64_t)wa->sum[idx].i);
                                    }
                                } else if (aop == OP_FIRST) {
                                    if (take_first) merged->sum[idx] = wa->sum[idx];
                                } else if (aop == OP_LAST) {
                                    if (take_last)  merged->sum[idx] = wa->sum[idx];
                                }
                            }
                            if (take_first) merged->first_row[s] = wa->first_row[s];
                            if (take_last)  merged->last_row[s]  = wa->last_row[s];
                        }
                    }
                    if (need_flags & DA_NEED_MIN) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->min_val[i].f < merged->min_val[i].f)
                                    merged->min_val[i].f = wa->min_val[i].f;
                            } else {
                                if (wa->min_val[i].i < merged->min_val[i].i)
                                    merged->min_val[i].i = wa->min_val[i].i;
                            }
                        }
                    }
                    if (need_flags & DA_NEED_MAX) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->max_val[i].f > merged->max_val[i].f)
                                    merged->max_val[i].f = wa->max_val[i].f;
                            } else {
                                if (wa->max_val[i].i > merged->max_val[i].i)
                                    merged->max_val[i].i = wa->max_val[i].i;
                            }
                        }
                    }
                    for (uint32_t s = 0; s < n_slots; s++)
                        merged->count[s] += wa->count[s];
                }
            } else if (da_n_workers > 1 && n_slots >= 1024 && da_pool) {
                /* Parallel merge: dispatch over disjoint slot ranges */
                da_merge_ctx_t merge_ctx = {
                    .accums        = accums,
                    .n_src_workers = da_n_workers,
                    .need_flags    = need_flags,
                    .n_aggs        = n_aggs,
                    .agg_types     = agg_types,
                    .agg_ops       = ext->agg_ops,
                };
                ray_pool_dispatch(da_pool, da_merge_fn, &merge_ctx, (int64_t)n_slots);
            } else {
                /* Sequential merge for small slot counts */
                for (uint32_t w = 1; w < da_n_workers; w++) {
                    da_accum_t* wa = &accums[w];
                    if (need_flags & DA_NEED_SUMSQ) {
                        for (size_t i = 0; i < total; i++)
                            merged->sumsq_f64[i] += wa->sumsq_f64[i];
                    }
                    if (need_flags & DA_NEED_SUM) {
                        for (uint32_t s = 0; s < n_slots; s++) {
                            size_t base = (size_t)s * n_aggs;
                            for (uint8_t a = 0; a < n_aggs; a++) {
                                size_t idx = base + a;
                                uint16_t aop = ext->agg_ops[a];
                                if (aop == OP_FIRST) {
                                    if (merged->count[s] == 0 && wa->count[s] > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                } else if (aop == OP_LAST) {
                                    if (wa->count[s] > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                } else if (aop == OP_PROD) {
                                    if (wa->count[s] > 0) {
                                        if (merged->count[s] == 0)
                                            merged->sum[idx] = wa->sum[idx];
                                        else if (agg_types[a] == RAY_F64)
                                            merged->sum[idx].f *= wa->sum[idx].f;
                                        else
                                            merged->sum[idx].i = (int64_t)((uint64_t)merged->sum[idx].i * (uint64_t)wa->sum[idx].i);
                                    }
                                } else if (agg_types[a] == RAY_F64)
                                    merged->sum[idx].f += wa->sum[idx].f;
                                else
                                    merged->sum[idx].i += wa->sum[idx].i;
                            }
                        }
                    }
                    if (need_flags & DA_NEED_MIN) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->min_val[i].f < merged->min_val[i].f)
                                    merged->min_val[i].f = wa->min_val[i].f;
                            } else {
                                if (wa->min_val[i].i < merged->min_val[i].i)
                                    merged->min_val[i].i = wa->min_val[i].i;
                            }
                        }
                    }
                    if (need_flags & DA_NEED_MAX) {
                        for (size_t i = 0; i < total; i++) {
                            uint8_t a = (uint8_t)(i % n_aggs);
                            if (agg_types[a] == RAY_F64) {
                                if (wa->max_val[i].f > merged->max_val[i].f)
                                    merged->max_val[i].f = wa->max_val[i].f;
                            } else {
                                if (wa->max_val[i].i > merged->max_val[i].i)
                                    merged->max_val[i].i = wa->max_val[i].i;
                            }
                        }
                    }
                    for (uint32_t s = 0; s < n_slots; s++)
                        merged->count[s] += wa->count[s];
                }
            }



            for (uint32_t w = 1; w < da_n_workers; w++)
                da_accum_free(&accums[w]);

            da_val_t* da_sum      = merged->sum;      /* may be NULL if !DA_NEED_SUM */
            da_val_t* da_min_val  = merged->min_val;  /* may be NULL if !DA_NEED_MIN */
            da_val_t* da_max_val  = merged->max_val;  /* may be NULL if !DA_NEED_MAX */
            double*   da_sumsq   = merged->sumsq_f64; /* may be NULL if !DA_NEED_SUMSQ */
            int64_t*  da_count   = merged->count;

            uint32_t all_grp_count = 0;
            for (uint32_t s = 0; s < n_slots; s++)
                if (da_count[s] > 0) all_grp_count++;

            int64_t da_keep_min = use_emit_filter
                ? da_count_emit_keep_min(da_count, n_slots, all_grp_count, emit_filter)
                : 1;

            uint32_t grp_count = 0;
            for (uint32_t s = 0; s < n_slots; s++)
                if (da_count[s] >= da_keep_min) grp_count++;

            int64_t total_cols = n_keys + n_aggs;
            ray_t* result = ray_table_new(total_cols);
            if (!result || RAY_IS_ERR(result)) {
                da_accum_free(&accums[0]); scratch_free(accums_hdr);
                for (uint8_t a = 0; a < n_aggs; a++)
                    { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
                for (uint8_t k = 0; k < n_keys; k++)
                    if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                if (match_idx_block) ray_release(match_idx_block);
                return result ? result : ray_error("oom", NULL);
            }

            /* Key columns — decompose composite slot back to per-key values */
            for (uint8_t k = 0; k < n_keys; k++) {
                ray_t* src_col = key_vecs[k];
                if (!src_col) continue;
                ray_t* key_col = col_vec_new(src_col, (int64_t)grp_count);
                if (!key_col || RAY_IS_ERR(key_col)) continue;
                key_col->len = (int64_t)grp_count;
                uint32_t gi = 0;
                for (uint32_t s = 0; s < n_slots; s++) {
                    if (da_count[s] < da_keep_min) continue;
                    int64_t offset = ((int64_t)s / da_key_stride[k]) % da_key_range[k];
                    int64_t key_val = da_key_min[k] + offset;
                    write_col_i64(ray_data(key_col), gi, key_val, src_col->type, key_col->attrs);
                    gi++;
                }
                ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]->id);
                int64_t name_id = key_ext ? key_ext->sym : (int64_t)k;
                result = ray_table_add_col(result, name_id, key_col);
                ray_release(key_col);
            }

            /* Agg columns — compact sparse DA arrays into dense, then emit */
            size_t dense_total = (size_t)grp_count * n_aggs;
            ray_t *_h_dsum = NULL, *_h_dmin = NULL, *_h_dmax = NULL;
            ray_t *_h_dsq = NULL, *_h_dcnt = NULL;
            da_val_t* dense_sum     = da_sum     ? (da_val_t*)scratch_alloc(&_h_dsum, dense_total * sizeof(da_val_t)) : NULL;
            da_val_t* dense_min_val = da_min_val ? (da_val_t*)scratch_alloc(&_h_dmin, dense_total * sizeof(da_val_t)) : NULL;
            da_val_t* dense_max_val = da_max_val ? (da_val_t*)scratch_alloc(&_h_dmax, dense_total * sizeof(da_val_t)) : NULL;
            double*   dense_sumsq   = da_sumsq   ? (double*)scratch_alloc(&_h_dsq, dense_total * sizeof(double)) : NULL;
            int64_t*  dense_counts  = grp_count
                ? (int64_t*)scratch_alloc(&_h_dcnt, grp_count * sizeof(int64_t))
                : NULL;

            uint32_t gi = 0;
            for (uint32_t s = 0; s < n_slots; s++) {
                if (da_count[s] < da_keep_min) continue;
                dense_counts[gi] = da_count[s];
                for (uint8_t a = 0; a < n_aggs; a++) {
                    size_t si = (size_t)s * n_aggs + a;
                    size_t di = (size_t)gi * n_aggs + a;
                    if (dense_sum)     dense_sum[di]     = da_sum[si];
                    if (dense_min_val) dense_min_val[di] = da_min_val[si];
                    if (dense_max_val) dense_max_val[di] = da_max_val[si];
                    if (dense_sumsq)   dense_sumsq[di]   = da_sumsq[si];
                }
                gi++;
            }

            emit_agg_columns(&result, g, ext, agg_vecs, grp_count, n_aggs,
                             (double*)dense_sum, (int64_t*)dense_sum,
                             (double*)dense_min_val, (double*)dense_max_val,
                             (int64_t*)dense_min_val, (int64_t*)dense_max_val,
                             dense_counts, agg_affine, dense_sumsq);

            scratch_free(_h_dsum); scratch_free(_h_dmin);
            scratch_free(_h_dmax);
            scratch_free(_h_dsq); scratch_free(_h_dcnt);

            da_accum_free(&accums[0]); scratch_free(accums_hdr);
            for (uint8_t a = 0; a < n_aggs; a++)
                { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
            for (uint8_t k = 0; k < n_keys; k++)
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            if (match_idx_block) ray_release(match_idx_block);
            return result;
        }
    }

    {
        bool sp_eligible = (nrows > 0 && n_keys == 1 && key_data[0] != NULL);
        int8_t kt = sp_eligible ? key_types[0] : 0;
        if (sp_eligible && kt != RAY_I64 && kt != RAY_I32 && kt != RAY_I16 &&
            kt != RAY_U8 && kt != RAY_BOOL && kt != RAY_DATE &&
            kt != RAY_TIME && kt != RAY_TIMESTAMP && kt != RAY_SYM)
            sp_eligible = false;
        if (sp_eligible && key_vecs[0]) {
            ray_t* src = (key_vecs[0]->attrs & RAY_ATTR_SLICE)
                         ? key_vecs[0]->slice_parent : key_vecs[0];
            if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
                sp_eligible = false;
        }
        bool sp_need_sum = false;
        for (uint8_t a = 0; a < n_aggs && sp_eligible; a++) {
            uint16_t op = ext->agg_ops[a];
            if (op == OP_COUNT) continue;
            if (op != OP_SUM && op != OP_AVG)
                sp_eligible = false;
            else
                sp_need_sum = true;
        }

        if (sp_eligible) {
            void* agg_ptrs[vla_aggs];
            int8_t agg_types[vla_aggs];
            uint32_t agg_f64_mask = 0;
            for (uint8_t a = 0; a < n_aggs; a++) {
                if (agg_vecs[a]) {
                    agg_ptrs[a] = ray_data(agg_vecs[a]);
                    agg_types[a] = agg_vecs[a]->type;
                    if (agg_vecs[a]->type == RAY_F64)
                        agg_f64_mask |= (1u << a);
                } else {
                    agg_ptrs[a] = NULL;
                    agg_types[a] = 0;
                }
            }
            ray_t** strlen_sym_strings = NULL;
            uint32_t strlen_sym_count = 0;
            for (uint8_t a = 0; a < n_aggs; a++) {
                if (agg_strlen[a] && agg_vecs[a] &&
                    agg_vecs[a]->type == RAY_SYM) {
                    ray_sym_strings_borrow(&strlen_sym_strings,
                                           &strlen_sym_count);
                    break;
                }
            }

            uint8_t key_esz = ray_sym_elem_size(key_types[0], key_attrs[0]);

            if (use_emit_filter &&
                (emit_filter.min_count_exclusive > 0 ||
                 emit_filter.top_count_take > 0) &&
                n_scan <= UINT32_MAX) {
                uint64_t cap = 1u << 20;
                const uint64_t max_dense_cap = 1u << 24;
                bool count_only_first = (key_types[0] == RAY_SYM);
                ray_t *cnt_hdr = NULL, *range_sum_hdr = NULL;
                uint32_t* range_count = (uint32_t*)scratch_calloc(
                    &cnt_hdr, (size_t)cap * sizeof(uint32_t));
                da_val_t* range_sum = NULL;
                bool dyn_ok = range_count != NULL;
                if (dyn_ok && sp_need_sum && !count_only_first) {
                    range_sum = (da_val_t*)scratch_calloc(
                        &range_sum_hdr,
                        (size_t)cap * n_aggs * sizeof(da_val_t));
                    dyn_ok = range_sum != NULL;
                }

	                uint64_t max_seen = 0;
	                bool have_dyn_key = false;
#define DYN_DENSE_ACCUM_ROW(row_expr)                                            \
    do {                                                                         \
        int64_t dyn_row = (row_expr);                                            \
        int64_t key = read_by_esz(key_data[0], dyn_row, key_esz);                \
        if (key < 0 || (uint64_t)key >= max_dense_cap) {                         \
            dyn_ok = false;                                                      \
            goto dyn_dense_done;                                                 \
        }                                                                        \
        uint64_t off = (uint64_t)key;                                            \
        if (off >= cap) {                                                        \
            uint64_t old_cap = cap;                                              \
            while (off >= cap) cap <<= 1;                                        \
            uint32_t* new_count = (uint32_t*)scratch_realloc(                    \
                &cnt_hdr, (size_t)old_cap * sizeof(uint32_t),                    \
                (size_t)cap * sizeof(uint32_t));                                 \
            if (!new_count) {                                                    \
                dyn_ok = false;                                                  \
                goto dyn_dense_done;                                             \
            }                                                                    \
            range_count = new_count;                                             \
            memset(range_count + old_cap, 0,                                     \
                   (size_t)(cap - old_cap) * sizeof(uint32_t));                  \
            if (sp_need_sum && !count_only_first) {                              \
                da_val_t* new_sum = (da_val_t*)scratch_realloc(                  \
                    &range_sum_hdr,                                              \
                    (size_t)old_cap * n_aggs * sizeof(da_val_t),                 \
                    (size_t)cap * n_aggs * sizeof(da_val_t));                    \
                if (!new_sum) {                                                  \
                    dyn_ok = false;                                              \
                    goto dyn_dense_done;                                         \
                }                                                                \
                range_sum = new_sum;                                             \
                memset(range_sum + (size_t)old_cap * n_aggs, 0,                 \
                       (size_t)(cap - old_cap) * n_aggs * sizeof(da_val_t));     \
            }                                                                    \
        }                                                                        \
        have_dyn_key = true;                                                     \
        if (off > max_seen) max_seen = off;                                      \
        if (range_count[off] != UINT32_MAX) range_count[off]++;                  \
        if (range_sum) {                                                         \
            da_val_t* sums = &range_sum[(size_t)off * n_aggs];                   \
            for (uint8_t a = 0; a < n_aggs; a++) {                               \
                if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a]) continue;       \
                if (agg_strlen[a])                                               \
                    sums[a].i += group_strlen_at_cached(                         \
                        agg_vecs[a], dyn_row, strlen_sym_strings, strlen_sym_count); \
                else if (agg_f64_mask & (1u << a))                               \
                    sums[a].f += ((const double*)agg_ptrs[a])[dyn_row];          \
                else                                                             \
                    sums[a].i += read_col_i64(agg_ptrs[a], dyn_row, agg_types[a], 0); \
            }                                                                    \
        }                                                                        \
    } while (0)

	                if (dyn_ok && match_idx) {
	                    for (int64_t i = 0; i < n_scan; i++)
	                        DYN_DENSE_ACCUM_ROW(match_idx[i]);
	                } else if (dyn_ok && rowsel) {
	                    ray_rowsel_t* m = ray_rowsel_meta(rowsel);
	                    const uint8_t* flags = ray_rowsel_flags(rowsel);
	                    const uint32_t* offs = ray_rowsel_offsets(rowsel);
	                    const uint16_t* idx = ray_rowsel_idx(rowsel);
	                    uint32_t nseg = (uint32_t)((m->nrows + RAY_MORSEL_ELEMS - 1) /
	                                              RAY_MORSEL_ELEMS);
	                    for (uint32_t seg = 0; seg < nseg; seg++) {
	                        int64_t base = (int64_t)seg * RAY_MORSEL_ELEMS;
	                        if (flags[seg] == RAY_SEL_NONE) continue;
	                        if (flags[seg] == RAY_SEL_ALL) {
	                            int64_t end = base + RAY_MORSEL_ELEMS;
	                            if (end > m->nrows) end = m->nrows;
	                            for (int64_t r = base; r < end; r++)
	                                DYN_DENSE_ACCUM_ROW(r);
	                        } else {
	                            for (uint32_t p = offs[seg]; p < offs[seg + 1]; p++)
	                                DYN_DENSE_ACCUM_ROW(base + idx[p]);
	                        }
	                    }
	                } else if (dyn_ok) {
	                    for (int64_t r = 0; r < n_scan; r++)
	                        DYN_DENSE_ACCUM_ROW(r);
	                }
dyn_dense_done:
#undef DYN_DENSE_ACCUM_ROW

	                if (dyn_ok && have_dyn_key) {
                    uint32_t total_groups = 0;
                    for (uint64_t off = 0; off <= max_seen; off++)
                        if (range_count[off] > 0)
                            total_groups++;
                    int64_t keep_min = da_count_emit_keep_min_u32(
                        range_count, max_seen + 1, total_groups, emit_filter);
                    uint32_t grp_count = 0;
                    for (uint64_t off = 0; off <= max_seen; off++)
                        if ((int64_t)range_count[off] >= keep_min)
                            grp_count++;

                    ray_t* result = ray_table_new((int64_t)n_keys + n_aggs);
                    if (!result || RAY_IS_ERR(result)) {
                        scratch_free(range_sum_hdr); scratch_free(cnt_hdr);
                        for (uint8_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint8_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) ray_release(match_idx_block);
                        return result ? result : ray_error("oom", NULL);
                    }

                    ray_t* key_col = col_vec_new(key_vecs[0], (int64_t)grp_count);
                    if (!key_col || RAY_IS_ERR(key_col)) {
                        scratch_free(range_sum_hdr); scratch_free(cnt_hdr);
                        ray_release(result);
                        for (uint8_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint8_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) ray_release(match_idx_block);
                        return key_col ? key_col : ray_error("oom", NULL);
                    }
                    key_col->len = (int64_t)grp_count;

                    ray_t *_h_sum = NULL, *_h_cnt = NULL;
                    da_val_t* dense_sum = sp_need_sum
                        ? (da_val_t*)scratch_alloc(&_h_sum,
                            (size_t)grp_count * n_aggs * sizeof(da_val_t))
                        : NULL;
                    int64_t* dense_count = (int64_t*)scratch_alloc(
                        &_h_cnt, (size_t)grp_count * sizeof(int64_t));
                    if ((sp_need_sum && !dense_sum) || !dense_count) {
                        scratch_free(_h_sum); scratch_free(_h_cnt);
                        scratch_free(range_sum_hdr); scratch_free(cnt_hdr);
                        ray_release(key_col); ray_release(result);
                        for (uint8_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint8_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) ray_release(match_idx_block);
                        return ray_error("oom", NULL);
                    }
                    if (sp_need_sum && !range_sum)
                        memset(dense_sum, 0,
                               (size_t)grp_count * n_aggs * sizeof(da_val_t));

                    uint32_t gi = 0;
                    for (uint64_t off = 0; off <= max_seen; off++) {
                        uint32_t cnt = range_count[off];
                        if ((int64_t)cnt < keep_min) {
                            if (!range_sum) range_count[off] = 0;
                            continue;
                        }
                        write_col_i64(ray_data(key_col), gi, (int64_t)off,
                                      key_col->type, key_col->attrs);
                        dense_count[gi] = (int64_t)cnt;
                        if (range_sum) {
                            memcpy(&dense_sum[(size_t)gi * n_aggs],
                                   &range_sum[(size_t)off * n_aggs],
                                   (size_t)n_aggs * sizeof(da_val_t));
                        }
                        if (!range_sum) range_count[off] = gi + 1u;
                        gi++;
                    }

                    if (sp_need_sum && !range_sum) {
#define DYN_DENSE_SUM_ROW(row_expr)                                              \
    do {                                                                         \
        int64_t dyn_row = (row_expr);                                            \
        int64_t key = read_by_esz(key_data[0], dyn_row, key_esz);                \
        if (key < 0 || (uint64_t)key > max_seen) break;                          \
        uint32_t marker = range_count[(uint64_t)key];                            \
        if (!marker) break;                                                      \
        da_val_t* sums = &dense_sum[(size_t)(marker - 1u) * n_aggs];             \
        for (uint8_t a = 0; a < n_aggs; a++) {                                   \
            if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a]) continue;           \
            if (agg_strlen[a])                                                   \
                sums[a].i += group_strlen_at_cached(                             \
                    agg_vecs[a], dyn_row, strlen_sym_strings, strlen_sym_count); \
            else if (agg_f64_mask & (1u << a))                                   \
                sums[a].f += ((const double*)agg_ptrs[a])[dyn_row];              \
            else                                                                 \
                sums[a].i += read_col_i64(agg_ptrs[a], dyn_row, agg_types[a], 0);\
        }                                                                        \
    } while (0)
                        if (match_idx) {
                            for (int64_t i = 0; i < n_scan; i++)
                                DYN_DENSE_SUM_ROW(match_idx[i]);
                        } else if (rowsel) {
                            ray_rowsel_t* m = ray_rowsel_meta(rowsel);
                            const uint8_t* flags = ray_rowsel_flags(rowsel);
                            const uint32_t* offs = ray_rowsel_offsets(rowsel);
                            const uint16_t* idx = ray_rowsel_idx(rowsel);
                            uint32_t nseg = (uint32_t)((m->nrows + RAY_MORSEL_ELEMS - 1) /
                                                      RAY_MORSEL_ELEMS);
                            for (uint32_t seg = 0; seg < nseg; seg++) {
                                int64_t base = (int64_t)seg * RAY_MORSEL_ELEMS;
                                if (flags[seg] == RAY_SEL_NONE) continue;
                                if (flags[seg] == RAY_SEL_ALL) {
                                    int64_t end = base + RAY_MORSEL_ELEMS;
                                    if (end > m->nrows) end = m->nrows;
                                    for (int64_t r = base; r < end; r++)
                                        DYN_DENSE_SUM_ROW(r);
                                } else {
                                    for (uint32_t p = offs[seg]; p < offs[seg + 1]; p++)
                                        DYN_DENSE_SUM_ROW(base + idx[p]);
                                }
                            }
                        } else {
                            for (int64_t r = 0; r < n_scan; r++)
                                DYN_DENSE_SUM_ROW(r);
                        }
#undef DYN_DENSE_SUM_ROW
                    }

                    ray_op_ext_t* key_ext = find_ext(g, ext->keys[0]->id);
                    int64_t name_id = key_ext ? key_ext->sym : 0;
                    result = ray_table_add_col(result, name_id, key_col);
                    ray_release(key_col);
                    emit_agg_columns(&result, g, ext, agg_vecs, grp_count, n_aggs,
                                     (double*)dense_sum, (int64_t*)dense_sum,
                                     NULL, NULL, NULL, NULL,
                                     dense_count, agg_affine, NULL);

                    scratch_free(_h_sum); scratch_free(_h_cnt);
                    scratch_free(range_sum_hdr); scratch_free(cnt_hdr);
                    for (uint8_t a = 0; a < n_aggs; a++)
                        if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                    for (uint8_t k = 0; k < n_keys; k++)
                        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                    if (match_idx_block) ray_release(match_idx_block);
                    return result;
                }

                scratch_free(range_sum_hdr);
                scratch_free(cnt_hdr);
            }

            if (use_emit_filter &&
                (emit_filter.min_count_exclusive > 0 ||
                 emit_filter.top_count_take > 0) &&
                key_types[0] != RAY_SYM && n_scan <= UINT32_MAX) {
                bool have_key = false;
                int64_t min_key = 0, max_key = 0;
                for (int64_t i = 0; i < n_scan; i++) {
                    int64_t r = match_idx ? match_idx[i] : i;
                    if (!match_idx && rowsel && !group_rowsel_pass(rowsel, r))
                        continue;
                    int64_t key = read_by_esz(key_data[0], r, key_esz);
                    if (!have_key) {
                        min_key = max_key = key;
                        have_key = true;
                    } else {
                        if (key < min_key) min_key = key;
                        if (key > max_key) max_key = key;
                    }
                }

                uint64_t key_range = have_key
                    ? (uint64_t)((uint64_t)max_key - (uint64_t)min_key + 1u)
                    : 0u;
                if (have_key && key_range > 0 && key_range <= (1u << 26)) {
                    ray_t *cnt_hdr = NULL, *range_sum_hdr = NULL;
                    ray_t *_h_sum = NULL, *_h_cnt = NULL;
                    uint32_t* range_count = (uint32_t*)scratch_calloc(
                        &cnt_hdr, (size_t)key_range * sizeof(uint32_t));
                    if (!range_count)
                        goto ht_path;
                    da_val_t* range_sum = NULL;
                    if (sp_need_sum && key_range <= (1u << 24)) {
                        range_sum = (da_val_t*)scratch_calloc(
                            &range_sum_hdr,
                            (size_t)key_range * n_aggs * sizeof(da_val_t));
                        if (!range_sum) {
                            scratch_free(cnt_hdr);
                            goto ht_path;
                        }
                    }

                    for (int64_t i = 0; i < n_scan; i++) {
                        int64_t r = match_idx ? match_idx[i] : i;
                        if (!match_idx && rowsel && !group_rowsel_pass(rowsel, r))
                            continue;
                        int64_t key = read_by_esz(key_data[0], r, key_esz);
                        uint64_t off = (uint64_t)((uint64_t)key - (uint64_t)min_key);
                        if (range_count[off] != UINT32_MAX)
                            range_count[off]++;
                        if (range_sum) {
                            da_val_t* sums = &range_sum[(size_t)off * n_aggs];
                            for (uint8_t a = 0; a < n_aggs; a++) {
                                if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a])
                                    continue;
                                if (agg_strlen[a])
                                    sums[a].i += group_strlen_at_cached(
                                        agg_vecs[a], r, strlen_sym_strings,
                                        strlen_sym_count);
                                else if (agg_f64_mask & (1u << a))
                                    sums[a].f += ((const double*)agg_ptrs[a])[r];
                                else
                                    sums[a].i += read_col_i64(agg_ptrs[a], r,
                                                              agg_types[a], 0);
                            }
                        }
                    }

                    uint32_t total_groups = 0;
                    for (uint64_t off = 0; off < key_range; off++) {
                        if (range_count[off] > 0)
                            total_groups++;
                    }
                    int64_t keep_min = da_count_emit_keep_min_u32(
                        range_count, key_range, total_groups, emit_filter);
                    uint32_t grp_count = 0;
                    for (uint64_t off = 0; off < key_range; off++) {
                        if ((int64_t)range_count[off] >= keep_min)
                            grp_count++;
                    }

                    ray_t* result = ray_table_new((int64_t)n_keys + n_aggs);
                    if (!result || RAY_IS_ERR(result)) {
                        scratch_free(range_sum_hdr);
                        scratch_free(cnt_hdr);
                        for (uint8_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint8_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) ray_release(match_idx_block);
                        return result ? result : ray_error("oom", NULL);
                    }

                    ray_t* key_col = col_vec_new(key_vecs[0], (int64_t)grp_count);
                    if (!key_col || RAY_IS_ERR(key_col)) {
                        scratch_free(range_sum_hdr);
                        scratch_free(cnt_hdr);
                        ray_release(result);
                        for (uint8_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint8_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) ray_release(match_idx_block);
                        return key_col ? key_col : ray_error("oom", NULL);
                    }
                    key_col->len = (int64_t)grp_count;

                    da_val_t* dense_sum = sp_need_sum
                        ? (da_val_t*)scratch_calloc(&_h_sum,
                            (size_t)grp_count * n_aggs * sizeof(da_val_t))
                        : NULL;
                    int64_t* dense_count = (int64_t*)scratch_alloc(
                        &_h_cnt, (size_t)grp_count * sizeof(int64_t));
                    if ((sp_need_sum && !dense_sum) || !dense_count) {
                        scratch_free(_h_sum); scratch_free(_h_cnt);
                        scratch_free(range_sum_hdr);
                        scratch_free(cnt_hdr);
                        ray_release(key_col); ray_release(result);
                        for (uint8_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint8_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) ray_release(match_idx_block);
                        return ray_error("oom", NULL);
                    }

                    uint32_t gi = 0;
                    for (uint64_t off = 0; off < key_range; off++) {
                        uint32_t cnt = range_count[off];
                        if ((int64_t)cnt < keep_min) {
                            range_count[off] = 0;
                            continue;
                        }
                        int64_t key = (int64_t)((uint64_t)min_key + off);
                        write_col_i64(ray_data(key_col), gi, key,
                                      key_col->type, key_col->attrs);
                        dense_count[gi] = (int64_t)cnt;
                        if (range_sum) {
                            memcpy(&dense_sum[(size_t)gi * n_aggs],
                                   &range_sum[(size_t)off * n_aggs],
                                   (size_t)n_aggs * sizeof(da_val_t));
                        }
                        range_count[off] = gi + 1u;
                        gi++;
                    }

                    if (sp_need_sum && !range_sum) {
                        for (int64_t i = 0; i < n_scan; i++) {
                            int64_t r = match_idx ? match_idx[i] : i;
                            if (!match_idx && rowsel && !group_rowsel_pass(rowsel, r))
                                continue;
                            int64_t key = read_by_esz(key_data[0], r, key_esz);
                            uint64_t off = (uint64_t)((uint64_t)key - (uint64_t)min_key);
                            uint32_t marker = range_count[off];
                            if (!marker) continue;
                            da_val_t* sums = &dense_sum[(size_t)(marker - 1u) * n_aggs];
                            for (uint8_t a = 0; a < n_aggs; a++) {
                                if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a])
                                    continue;
                                if (agg_strlen[a])
                                    sums[a].i += group_strlen_at_cached(
                                        agg_vecs[a], r, strlen_sym_strings,
                                        strlen_sym_count);
                                else if (agg_f64_mask & (1u << a))
                                    sums[a].f += ((const double*)agg_ptrs[a])[r];
                                else
                                    sums[a].i += read_col_i64(agg_ptrs[a], r,
                                                              agg_types[a], 0);
                            }
                        }
                    }

                    scratch_free(range_sum_hdr);
                    scratch_free(cnt_hdr);
                    ray_op_ext_t* key_ext = find_ext(g, ext->keys[0]->id);
                    int64_t name_id = key_ext ? key_ext->sym : 0;
                    result = ray_table_add_col(result, name_id, key_col);
                    ray_release(key_col);

                    emit_agg_columns(&result, g, ext, agg_vecs, grp_count, n_aggs,
                                     (double*)dense_sum, (int64_t*)dense_sum,
                                     NULL, NULL, NULL, NULL,
                                     dense_count, agg_affine, NULL);

                    scratch_free(_h_sum);
                    scratch_free(_h_cnt);
                    for (uint8_t a = 0; a < n_aggs; a++)
                        if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                    for (uint8_t k = 0; k < n_keys; k++)
                        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                    if (match_idx_block) ray_release(match_idx_block);
                    return result;
                }
            }

            sparse_i64_ht_t sp_ht;
            memset(&sp_ht, 0, sizeof(sp_ht));
            bool sp_ok = true;

            if (use_emit_filter &&
                (emit_filter.min_count_exclusive > 0 ||
                 emit_filter.top_count_take > 0)) {
                uint64_t expected = (uint64_t)nrows / 64u;
                if (expected < 4096) expected = 4096;
                if (expected > (1u << 20)) expected = (1u << 20);
                if (!sparse_i64_init(&sp_ht, (uint32_t)expected, n_aggs, false))
                    goto ht_path;

                for (int64_t i = 0; i < n_scan; i++) {
                    int64_t r = match_idx ? match_idx[i] : i;
                    if (!match_idx && rowsel && !group_rowsel_pass(rowsel, r))
                        continue;
                    int64_t key = read_by_esz(key_data[0], r, key_esz);
                    int32_t slot;
                    if (!sparse_i64_touch(&sp_ht, key, n_aggs, false, &slot)) {
                        sp_ok = false;
                        break;
                    }
                    sp_ht.counts[slot]++;
                }
            } else {
                uint64_t expected = (uint64_t)nrows / 64u;
                if (expected < 4096) expected = 4096;
                if (expected > (1u << 20)) expected = (1u << 20);
                if (!sparse_i64_init(&sp_ht, (uint32_t)expected, n_aggs, sp_need_sum))
                    goto ht_path;

                for (int64_t i = 0; i < n_scan; i++) {
                    int64_t r = match_idx ? match_idx[i] : i;
                    if (!match_idx && rowsel && !group_rowsel_pass(rowsel, r))
                        continue;
                    int64_t key = read_by_esz(key_data[0], r, key_esz);
                    int32_t slot;
                    if (!sparse_i64_touch(&sp_ht, key, n_aggs, sp_need_sum, &slot)) {
                        sp_ok = false;
                        break;
                    }
                    sp_ht.counts[slot]++;
                    if (!sp_need_sum) continue;
                    da_val_t* sums = &sp_ht.sums[(size_t)slot * n_aggs];
                    for (uint8_t a = 0; a < n_aggs; a++) {
                        if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a])
                            continue;
                        if (agg_strlen[a])
                            sums[a].i += group_strlen_at_cached(
                                agg_vecs[a], r, strlen_sym_strings,
                                strlen_sym_count);
                        else if (agg_f64_mask & (1u << a))
                            sums[a].f += ((const double*)agg_ptrs[a])[r];
                        else
                            sums[a].i += read_col_i64(agg_ptrs[a], r, agg_types[a], 0);
                    }
                }
            }
            if (!sp_ok) {
                sparse_i64_free(&sp_ht);
                goto ht_path;
            }

            uint32_t total_groups = 0;
            for (uint32_t s = 0; s < sp_ht.cap; s++) {
                if (!sp_ht.used[s]) continue;
                total_groups++;
            }
            int64_t keep_min = use_emit_filter
                ? da_count_emit_keep_min(sp_ht.counts, sp_ht.cap,
                                         total_groups, emit_filter)
                : 1;
            uint32_t grp_count = 0;
            for (uint32_t s = 0; s < sp_ht.cap; s++) {
                if (!sp_ht.used[s]) continue;
                if (sp_ht.counts[s] < keep_min) continue;
                grp_count++;
            }
            ray_t* result = ray_table_new((int64_t)n_keys + n_aggs);
            if (!result || RAY_IS_ERR(result)) {
                sparse_i64_free(&sp_ht);
                for (uint8_t a = 0; a < n_aggs; a++)
                    if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                for (uint8_t k = 0; k < n_keys; k++)
                    if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                if (match_idx_block) ray_release(match_idx_block);
                return result ? result : ray_error("oom", NULL);
            }

            ray_t* key_col = col_vec_new(key_vecs[0], (int64_t)grp_count);
            if (!key_col || RAY_IS_ERR(key_col)) {
                sparse_i64_free(&sp_ht);
                ray_release(result);
                for (uint8_t a = 0; a < n_aggs; a++)
                    if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                for (uint8_t k = 0; k < n_keys; k++)
                    if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                if (match_idx_block) ray_release(match_idx_block);
                return key_col ? key_col : ray_error("oom", NULL);
            }
            key_col->len = (int64_t)grp_count;

            ray_t *_h_sum = NULL, *_h_cnt = NULL;
            da_val_t* dense_sum = sp_need_sum
                ? (da_val_t*)scratch_alloc(&_h_sum,
                    (size_t)grp_count * n_aggs * sizeof(da_val_t))
                : NULL;
            int64_t* dense_count = (int64_t*)scratch_alloc(&_h_cnt,
                (size_t)grp_count * sizeof(int64_t));
            if ((sp_need_sum && !dense_sum) || !dense_count) {
                scratch_free(_h_sum); scratch_free(_h_cnt);
                ray_release(key_col); ray_release(result);
                sparse_i64_free(&sp_ht);
                for (uint8_t a = 0; a < n_aggs; a++)
                    if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                for (uint8_t k = 0; k < n_keys; k++)
                    if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                if (match_idx_block) ray_release(match_idx_block);
                return ray_error("oom", NULL);
            }
            if (use_emit_filter && sp_need_sum)
                memset(dense_sum, 0, (size_t)grp_count * n_aggs * sizeof(da_val_t));

            sparse_i64_ht_t heavy_ht;
            memset(&heavy_ht, 0, sizeof(heavy_ht));
            if (use_emit_filter && grp_count > 0) {
                if (!sparse_i64_init(&heavy_ht, grp_count * 2u, n_aggs, false)) {
                    scratch_free(_h_sum); scratch_free(_h_cnt);
                    ray_release(key_col); ray_release(result);
                    sparse_i64_free(&sp_ht);
                    for (uint8_t a = 0; a < n_aggs; a++)
                        if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                    for (uint8_t k = 0; k < n_keys; k++)
                        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                    if (match_idx_block) ray_release(match_idx_block);
                    return ray_error("oom", NULL);
                }
            }

            uint32_t gi = 0;
            for (uint32_t s = 0; s < sp_ht.cap; s++) {
                if (!sp_ht.used[s]) continue;
                if (sp_ht.counts[s] < keep_min) continue;
                write_col_i64(ray_data(key_col), gi, sp_ht.keys[s],
                              key_col->type, key_col->attrs);
                dense_count[gi] = sp_ht.counts[s];
                if (use_emit_filter) {
                    int32_t hslot;
                    if (!sparse_i64_touch(&heavy_ht, sp_ht.keys[s], n_aggs, false, &hslot)) {
                        scratch_free(_h_sum); scratch_free(_h_cnt);
                        ray_release(key_col); ray_release(result);
                        sparse_i64_free(&heavy_ht);
                        sparse_i64_free(&sp_ht);
                        for (uint8_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint8_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) ray_release(match_idx_block);
                        return ray_error("oom", NULL);
                    }
                    heavy_ht.counts[hslot] = gi;
                } else if (sp_need_sum) {
                    memcpy(&dense_sum[(size_t)gi * n_aggs],
                           &sp_ht.sums[(size_t)s * n_aggs],
                           (size_t)n_aggs * sizeof(da_val_t));
                }
                gi++;
            }
            sparse_i64_free(&sp_ht);

            if (use_emit_filter && sp_need_sum) {
                for (int64_t i = 0; i < n_scan; i++) {
                    int64_t r = match_idx ? match_idx[i] : i;
                    if (!match_idx && rowsel && !group_rowsel_pass(rowsel, r))
                        continue;
                    int64_t key = read_by_esz(key_data[0], r, key_esz);
                    int32_t hslot = sparse_i64_find_slot(&heavy_ht, key);
                    if (!heavy_ht.used[hslot] || heavy_ht.keys[hslot] != key)
                        continue;
                    uint32_t out_gi = (uint32_t)heavy_ht.counts[hslot];
                    da_val_t* sums = &dense_sum[(size_t)out_gi * n_aggs];
                    for (uint8_t a = 0; a < n_aggs; a++) {
                        if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a])
                            continue;
                        if (agg_strlen[a])
                            sums[a].i += group_strlen_at_cached(
                                agg_vecs[a], r, strlen_sym_strings,
                                strlen_sym_count);
                        else if (agg_f64_mask & (1u << a))
                            sums[a].f += ((const double*)agg_ptrs[a])[r];
                        else
                            sums[a].i += read_col_i64(agg_ptrs[a], r, agg_types[a], 0);
                    }
                }
            }
            sparse_i64_free(&heavy_ht);
            ray_op_ext_t* key_ext = find_ext(g, ext->keys[0]->id);
            int64_t name_id = key_ext ? key_ext->sym : 0;
            result = ray_table_add_col(result, name_id, key_col);
            ray_release(key_col);

            emit_agg_columns(&result, g, ext, agg_vecs, grp_count, n_aggs,
                             (double*)dense_sum, (int64_t*)dense_sum,
                             NULL, NULL, NULL, NULL,
                             dense_count, agg_affine, NULL);

            scratch_free(_h_sum);
            scratch_free(_h_cnt);
            for (uint8_t a = 0; a < n_aggs; a++)
                if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
            for (uint8_t k = 0; k < n_keys; k++)
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            if (match_idx_block) ray_release(match_idx_block);
            return result;
        }
    }

ht_path:;
    /* Compute which accumulator arrays the HT needs based on agg ops.
     * COUNT only reads group row's count field — no accumulator needed. */
    uint8_t ght_need = 0;
    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop == OP_SUM || aop == OP_PROD || aop == OP_AVG || aop == OP_FIRST || aop == OP_LAST)
            ght_need |= GHT_NEED_SUM;
        if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
            { ght_need |= GHT_NEED_SUM; ght_need |= GHT_NEED_SUMSQ; }
        if (aop == OP_PEARSON_CORR)
            { ght_need |= GHT_NEED_SUM; ght_need |= GHT_NEED_SUMSQ;
              ght_need |= GHT_NEED_PEARSON; }
        if (aop == OP_MIN) ght_need |= GHT_NEED_MIN;
        if (aop == OP_MAX) ght_need |= GHT_NEED_MAX;
    }

    /* RAY_STR keys still need the eval-level path (variable-width
     * with a pool).  RAY_GUID uses the wide-key row-indirection
     * support in the layout; see ght_layout_t.wide_key_mask. */
    for (uint8_t k = 0; k < n_keys; k++) {
        if (key_types[k] == RAY_STR) {
            for (uint8_t kk = 0; kk < n_keys; kk++)
                if (key_owned[kk] && key_vecs[kk]) ray_release(key_vecs[kk]);
            for (uint8_t a = 0; a < n_aggs; a++)
                { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
            if (match_idx_block) ray_release(match_idx_block);
            return ray_error("nyi", NULL);
        }
    }

    /* Compute row-layout: keys + agg values inline */
    ght_layout_t ght_layout = ght_compute_layout(n_keys, n_aggs, agg_vecs, ght_need, ext->agg_ops, key_types);

    /* Right-sized hash table: start small, rehash on load > 0.5 */
    uint32_t ht_cap = 256;
    {
        uint64_t target = (uint64_t)nrows < 65536 ? (uint64_t)nrows : 65536;
        if (target < 256) target = 256;
        while (ht_cap < target) ht_cap *= 2;
    }

    /* Parallel path: radix-partitioned group-by */
    ray_pool_t* pool = ray_pool_get();
    uint32_t n_total = pool ? ray_pool_total_workers(pool) : 1;

    group_ht_t single_ht;
    group_ht_t top_ht;
    group_ht_t* final_ht = NULL;
    bool top_ht_ready = false;
    ray_t* result = NULL;

    ray_t* radix_bufs_hdr = NULL;
    radix_buf_t* radix_bufs = NULL;
    ray_t* part_hts_hdr = NULL;
    group_ht_t*  part_hts   = NULL;

    if (use_emit_filter && emit_filter.top_count_take > 0 &&
        n_keys > 1) {
        bool top_count_nonselective = false;
        if (n_keys >= 2 && n_keys <= 5) {
            bool supported = true;
            bool nullable = false;
            for (uint16_t k = 0; k < n_keys; k++) {
                if (key_types[k] == RAY_F64 || key_types[k] == RAY_GUID ||
                    key_types[k] == RAY_STR) {
                    supported = false;
                    break;
                }
                ray_t* src = key_vecs[k] && (key_vecs[k]->attrs & RAY_ATTR_SLICE)
                    ? key_vecs[k]->slice_parent : key_vecs[k];
                if (src && (src->attrs & RAY_ATTR_HAS_NULLS)) {
                    nullable = true;
                    break;
                }
            }
            if (!nullable && n_scan > 0 && n_scan <= INT32_MAX) {
                uint64_t want = ((uint64_t)n_scan * 4u) / 3u;
                uint32_t cap = 256;
                while ((uint64_t)cap < want && cap < (1u << 28)) cap <<= 1;
                if (supported && (uint64_t)cap >= want) {
                    ray_t *hk[5] = { NULL, NULL, NULL, NULL, NULL };
                    ray_t *hc = NULL;
                    int64_t* ck64[5] = { NULL, NULL, NULL, NULL, NULL };
                    int32_t* ck32[5] = { NULL, NULL, NULL, NULL, NULL };
                    uint8_t key_is_i32[5] = { 0, 0, 0, 0, 0 };
                    for (uint16_t k = 0; k < n_keys; k++) {
                        int8_t kt = key_types[k];
                        key_is_i32[k] = (kt == RAY_I32 || kt == RAY_DATE ||
                                          kt == RAY_TIME || kt == RAY_I16 ||
                                          kt == RAY_BOOL || kt == RAY_U8);
                        if (key_is_i32[k])
                            ck32[k] = (int32_t*)scratch_alloc(&hk[k], (size_t)cap * sizeof(int32_t));
                        else
                            ck64[k] = (int64_t*)scratch_alloc(&hk[k], (size_t)cap * sizeof(int64_t));
                    }
                    uint16_t* cc = (uint16_t*)scratch_calloc(&hc, (size_t)cap * sizeof(uint16_t));
                    bool keys_alloc_ok = true;
                    for (uint16_t k = 0; k < n_keys; k++) {
                        if (key_is_i32[k] ? (ck32[k] == NULL) : (ck64[k] == NULL)) {
                            keys_alloc_ok = false;
                            break;
                        }
                    }
                    if (keys_alloc_ok && cc) {
                        uint32_t mask = cap - 1;
                        bool counted_fast = false;
                        bool count_overflow = false;
#define TOP_COUNT2_FIXED_LOOP(T0, T1) do {                                      \
                            const T0* d0 = (const T0*)key_data[0];              \
                            const T1* d1 = (const T1*)key_data[1];              \
                            for (int64_t r = 0; r < n_scan; r++) {              \
                                int64_t v0 = (int64_t)d0[r];                    \
                                int64_t v1 = (int64_t)d1[r];                    \
                                uint64_t h = ray_hash_combine(ray_hash_i64(v0), \
                                                               ray_hash_i64(v1));\
                                uint32_t slot = (uint32_t)(h & mask);           \
                                while (cc[slot]) {                              \
                                    int64_t s0 = key_is_i32[0]                  \
                                        ? (int64_t)ck32[0][slot]                \
                                        : ck64[0][slot];                        \
                                    int64_t s1 = key_is_i32[1]                  \
                                        ? (int64_t)ck32[1][slot]                \
                                        : ck64[1][slot];                        \
                                    if (s0 == v0 && s1 == v1) break;            \
                                    slot = (slot + 1) & mask;                   \
                                }                                                \
                                if (!cc[slot]) {                                \
                                    if (key_is_i32[0]) ck32[0][slot] = (int32_t)v0; \
                                    else ck64[0][slot] = v0;                    \
                                    if (key_is_i32[1]) ck32[1][slot] = (int32_t)v1; \
                                    else ck64[1][slot] = v1;                    \
                                    cc[slot] = 1;                               \
                                } else if (cc[slot] != UINT16_MAX) {            \
                                    cc[slot]++;                                 \
                                } else {                                        \
                                    count_overflow = true;                      \
                                }                                                \
                            }                                                    \
                            counted_fast = true;                                \
                        } while (0)
                        if (!rowsel && !match_idx &&
                            n_keys == 2 && key_types[0] != RAY_SYM &&
                            key_types[1] != RAY_SYM) {
                            bool k0_64 = (key_types[0] == RAY_I64 ||
                                          key_types[0] == RAY_TIMESTAMP);
                            bool k1_64 = (key_types[1] == RAY_I64 ||
                                          key_types[1] == RAY_TIMESTAMP);
                            bool k0_32 = (key_types[0] == RAY_I32 ||
                                          key_types[0] == RAY_DATE ||
                                          key_types[0] == RAY_TIME);
                            bool k1_32 = (key_types[1] == RAY_I32 ||
                                          key_types[1] == RAY_DATE ||
                                          key_types[1] == RAY_TIME);
                            if (k0_64 && k1_64) TOP_COUNT2_FIXED_LOOP(int64_t, int64_t);
                            else if (k0_64 && k1_32) TOP_COUNT2_FIXED_LOOP(int64_t, int32_t);
                            else if (k0_32 && k1_64) TOP_COUNT2_FIXED_LOOP(int32_t, int64_t);
                            else if (k0_32 && k1_32) TOP_COUNT2_FIXED_LOOP(int32_t, int32_t);
                        }
#undef TOP_COUNT2_FIXED_LOOP
                        if (!counted_fast) {
                            for (int64_t i = 0; i < n_scan; i++) {
                                int64_t r = match_idx ? match_idx[i] : i;
                                if (!match_idx && rowsel && !group_rowsel_pass(rowsel, r))
                                    continue;
                                int64_t vals[5] = { 0, 0, 0, 0, 0 };
                                uint64_t h = 0;
                                for (uint16_t k = 0; k < n_keys; k++) {
                                    vals[k] = read_col_i64(key_data[k], r, key_types[k], key_attrs[k]);
                                    uint64_t kh = ray_hash_i64(vals[k]);
                                    h = (k == 0) ? kh : ray_hash_combine(h, kh);
                                }
                                uint32_t slot = (uint32_t)(h & mask);
                                while (cc[slot]) {
                                    bool same = true;
                                    for (uint16_t k = 0; k < n_keys; k++) {
                                        int64_t stored = key_is_i32[k]
                                            ? (int64_t)ck32[k][slot]
                                            : ck64[k][slot];
                                        if (stored != vals[k]) {
                                            same = false;
                                            break;
                                        }
                                    }
                                    if (same) break;
                                    slot = (slot + 1) & mask;
                                }
                                if (!cc[slot]) {
                                    for (uint16_t k = 0; k < n_keys; k++) {
                                        if (key_is_i32[k])
                                            ck32[k][slot] = (int32_t)vals[k];
                                        else
                                            ck64[k][slot] = vals[k];
                                    }
                                    cc[slot] = 1;
                                } else if (cc[slot] != UINT16_MAX) {
                                    cc[slot]++;
                                } else {
                                    count_overflow = true;
                                }
                            }
                        }

                        if (!count_overflow) {
                            int64_t k_take = emit_filter.top_count_take;
                            int64_t heap_n = 0;
                            int64_t heap[1024];
                            uint32_t heap_slots[1024];
                            if (k_take > (int64_t)(sizeof(heap) / sizeof(heap[0])))
                                k_take = (int64_t)(sizeof(heap) / sizeof(heap[0]));
                            uint32_t total_groups = 0;
                            for (uint32_t i = 0; i < cap; i++) {
                                if (!cc[i]) continue;
                                total_groups++;
                                int64_t cnt = cc[i];
                                if (heap_n < k_take) {
                                    int64_t j = heap_n++;
                                    heap[j] = cnt;
                                    heap_slots[j] = i;
                                    while (j > 0) {
                                        int64_t parent = (j - 1) >> 1;
                                        if (heap[parent] <= heap[j]) break;
                                        int64_t tmp = heap[parent]; heap[parent] = heap[j]; heap[j] = tmp;
                                        uint32_t stmp = heap_slots[parent]; heap_slots[parent] = heap_slots[j]; heap_slots[j] = stmp;
                                        j = parent;
                                    }
                                } else if (cnt > heap[0]) {
                                    heap[0] = cnt;
                                    heap_slots[0] = i;
                                    int64_t j = 0;
                                    for (;;) {
                                        int64_t l = j * 2 + 1, rr = l + 1, m = j;
                                        if (l < heap_n && heap[l] < heap[m]) m = l;
                                        if (rr < heap_n && heap[rr] < heap[m]) m = rr;
                                        if (m == j) break;
                                        int64_t tmp = heap[m]; heap[m] = heap[j]; heap[j] = tmp;
                                        uint32_t stmp = heap_slots[m]; heap_slots[m] = heap_slots[j]; heap_slots[j] = stmp;
                                        j = m;
                                    }
                                }
                            }
                            uint32_t heavy_count = (uint32_t)heap_n;

                            if (heavy_count > 0 && total_groups > 0) {
                                uint32_t hcap = 256;
                                while (hcap < heavy_count * 2u && hcap < (1u << 30)) hcap <<= 1;
                                memset(&top_ht, 0, sizeof(top_ht));
                                if (group_ht_init_sized(&top_ht, hcap, &ght_layout, heavy_count)) {
                                    top_ht_ready = true;
                                    group_ht_set_key_data(&top_ht, key_data);
                                    int64_t keys[6] = { 0, 0, 0, 0, 0, 0 };
	                                for (uint32_t hi = 0; hi < heavy_count && !top_ht.oom; hi++) {
	                                    uint32_t i = heap_slots[hi];
	                                    for (uint16_t k = 0; k < n_keys; k++)
	                                        keys[k] = key_is_i32[k]
	                                            ? (int64_t)ck32[k][i]
	                                            : ck64[k][i];
	                                    keys[n_keys] = 0;
                                        uint32_t gid = top_ht.grp_count;
	                                    if (!group_ht_insert_empty_group(&top_ht, keys, key_types))
	                                        break;
                                        if (gid < top_ht.grp_count) {
                                            char* row = top_ht.rows + (size_t)gid * ght_layout.row_stride;
                                            *(int64_t*)row = (int64_t)cc[i];
                                        }
                                    }
                                    if (!top_ht.oom) {
                                    bool count_only = true;
                                    for (uint8_t a = 0; a < n_aggs; a++) {
                                        if (ext->agg_ops[a] != OP_COUNT) {
                                            count_only = false;
                                            break;
                                        }
                                    }
                                    if (count_only) {
                                        for (uint16_t k = 0; k < n_keys; k++)
                                            scratch_free(hk[k]);
                                        scratch_free(hc);
                                        final_ht = &top_ht;
                                        goto build_from_final_ht;
                                    }
                                    bool direct_ok = (heavy_count <= 64);
                                    for (uint8_t a = 0; a < n_aggs && direct_ok; a++) {
                                        uint16_t aop = ext->agg_ops[a];
                                        if (aop == OP_COUNT) continue;
                                        if ((aop == OP_SUM || aop == OP_AVG) &&
                                            agg_vecs[a] && !agg_strlen[a] &&
                                            agg_vecs[a]->type != RAY_STR &&
                                            agg_vecs[a]->type != RAY_GUID)
                                            continue;
                                        direct_ok = false;
                                    }
                                    if (direct_ok) {
	                                        int64_t sel_keys[64][5];
	                                        for (uint32_t hi = 0; hi < heavy_count; hi++) {
	                                            uint32_t i = heap_slots[hi];
	                                            for (uint16_t k = 0; k < n_keys; k++)
	                                                sel_keys[hi][k] = key_is_i32[k]
	                                                    ? (int64_t)ck32[k][i]
	                                                    : ck64[k][i];
	                                        }
                                        bool unique_first_key = true;
                                        for (uint32_t i = 0; i < heavy_count && unique_first_key; i++) {
                                            for (uint32_t j = i + 1; j < heavy_count; j++) {
                                                if (sel_keys[i][0] == sel_keys[j][0]) {
                                                    unique_first_key = false;
                                                    break;
                                                }
                                            }
                                        }
                                        uint8_t lk_used[256];
                                        uint32_t lk_idx[256];
                                        int64_t lk_key[256];
                                        if (unique_first_key) {
                                            memset(lk_used, 0, sizeof(lk_used));
                                            for (uint32_t hi = 0; hi < heavy_count; hi++) {
                                                uint32_t pos = (uint32_t)ray_hash_i64(sel_keys[hi][0]) & 255u;
                                                while (lk_used[pos])
                                                    pos = (pos + 1u) & 255u;
                                                lk_used[pos] = 1;
                                                lk_key[pos] = sel_keys[hi][0];
                                                lk_idx[pos] = hi;
                                            }
                                        }
                                        for (uint16_t k = 0; k < n_keys; k++)
                                            scratch_free(hk[k]);
                                        scratch_free(hc);

                                        for (int64_t i = 0; i < n_scan; i++) {
                                            int64_t r = match_idx ? match_idx[i] : i;
                                            if (!match_idx && rowsel && !group_rowsel_pass(rowsel, r))
                                                continue;
                                            int64_t v0 = read_col_i64(key_data[0], r, key_types[0], key_attrs[0]);
                                            uint32_t hit = UINT32_MAX;
                                            if (unique_first_key) {
                                                uint32_t pos = (uint32_t)ray_hash_i64(v0) & 255u;
                                                while (lk_used[pos]) {
                                                    if (lk_key[pos] == v0) {
                                                        hit = lk_idx[pos];
                                                        break;
                                                    }
                                                    pos = (pos + 1u) & 255u;
                                                }
                                                if (hit == UINT32_MAX) continue;
                                                int64_t v1 = read_col_i64(key_data[1], r, key_types[1], key_attrs[1]);
                                                if (sel_keys[hit][1] != v1) continue;
                                                if (n_keys >= 3) {
                                                    int64_t v2 = read_col_i64(key_data[2], r, key_types[2], key_attrs[2]);
                                                    if (sel_keys[hit][2] != v2) continue;
                                                }
                                                if (n_keys >= 4) {
                                                    int64_t v3 = read_col_i64(key_data[3], r, key_types[3], key_attrs[3]);
                                                    if (sel_keys[hit][3] != v3) continue;
                                                }
                                                if (n_keys == 5) {
                                                    int64_t v4 = read_col_i64(key_data[4], r, key_types[4], key_attrs[4]);
                                                    if (sel_keys[hit][4] != v4) continue;
                                                }
                                            } else {
                                                int64_t v1 = read_col_i64(key_data[1], r, key_types[1], key_attrs[1]);
                                                int64_t v2 = 0;
                                                int64_t v3 = 0;
                                                int64_t v4 = 0;
                                                if (n_keys >= 3)
                                                    v2 = read_col_i64(key_data[2], r, key_types[2], key_attrs[2]);
                                                if (n_keys >= 4)
                                                    v3 = read_col_i64(key_data[3], r, key_types[3], key_attrs[3]);
                                                if (n_keys == 5)
                                                    v4 = read_col_i64(key_data[4], r, key_types[4], key_attrs[4]);
                                                for (uint32_t hi = 0; hi < heavy_count; hi++) {
                                                    if (sel_keys[hi][0] == v0 && sel_keys[hi][1] == v1 &&
                                                        (n_keys == 2 || sel_keys[hi][2] == v2) &&
                                                        (n_keys < 4 || sel_keys[hi][3] == v3) &&
                                                        (n_keys < 5 || sel_keys[hi][4] == v4)) {
                                                        hit = hi;
                                                        break;
                                                    }
                                                }
                                                if (hit == UINT32_MAX) continue;
                                            }
                                            char* row = top_ht.rows + (size_t)hit * ght_layout.row_stride;
                                            (*(int64_t*)row)++;
                                            for (uint8_t a = 0; a < n_aggs; a++) {
                                                uint16_t aop = ext->agg_ops[a];
                                                if (aop == OP_COUNT || !agg_vecs[a]) continue;
                                                int8_t s = ght_layout.agg_val_slot[a];
                                                if (s < 0) continue;
                                                if (agg_vecs[a]->type == RAY_F64) {
                                                    double* dst = &ROW_WR_F64(row, ght_layout.off_sum, s);
                                                    *dst += ((const double*)ray_data(agg_vecs[a]))[r];
                                                } else {
                                                    int64_t* dst = &ROW_WR_I64(row, ght_layout.off_sum, s);
                                                    *dst += read_col_i64(ray_data(agg_vecs[a]), r,
                                                                         agg_vecs[a]->type,
                                                                         agg_vecs[a]->attrs);
                                                }
                                            }
                                        }
                                        final_ht = &top_ht;
                                        goto build_from_final_ht;
                                    }
                                    for (uint16_t k = 0; k < n_keys; k++)
                                        scratch_free(hk[k]);
                                    scratch_free(hc);
                                    group_rows_range_existing(&top_ht, key_data, key_types,
                                        key_attrs, key_vecs, agg_vecs, agg_strlen, rowsel,
                                        0, n_scan, match_idx);
                                    if (ray_interrupted()) {
                                        result = ray_error("cancel", "interrupted");
                                        goto cleanup;
                                    }
                                    final_ht = &top_ht;
                                    goto build_from_final_ht;
                                    }
                                }
                            }
                        }
                    }
                    for (uint16_t k = 0; k < n_keys; k++)
                        scratch_free(hk[k]);
                    scratch_free(hc);
                    if (top_ht_ready) {
                        group_ht_free(&top_ht);
                        top_ht_ready = false;
                    }
                }
            }
        }

        if (top_count_nonselective)
            goto skip_top_count_filter;
        if (rowsel || match_idx)
            goto skip_top_count_filter;

        uint16_t cnt_op = OP_COUNT;
        ray_t* cnt_vecs[1] = { NULL };
        ght_layout_t cnt_layout =
            ght_compute_layout(n_keys, 1, cnt_vecs, 0, &cnt_op, key_types);
        pivot_ingest_t cnt_ingest;
        if (pivot_ingest_run(&cnt_ingest, &cnt_layout, key_data, key_types,
                             key_attrs, key_vecs, cnt_vecs, n_scan)) {
            if (ray_interrupted()) {
                pivot_ingest_free(&cnt_ingest);
                result = ray_error("cancel", "interrupted");
                goto cleanup;
            }

            int64_t k_take = emit_filter.top_count_take;
            int64_t heap_n = 0;
            int64_t heap[1024];
            if (k_take > (int64_t)(sizeof(heap) / sizeof(heap[0])))
                k_take = (int64_t)(sizeof(heap) / sizeof(heap[0]));
            int64_t total_count_groups = 0;
            for (uint32_t p = 0; p < cnt_ingest.n_parts; p++) {
                group_ht_t* ph = &cnt_ingest.part_hts[p];
                uint16_t rs = ph->layout.row_stride;
                total_count_groups += ph->grp_count;
                for (uint32_t gi = 0; gi < ph->grp_count; gi++) {
                    const char* row = ph->rows + (size_t)gi * rs;
                    int64_t cnt = *(const int64_t*)(const void*)row;
                    if (heap_n < k_take) {
                        int64_t j = heap_n++;
                        heap[j] = cnt;
                        while (j > 0) {
                            int64_t parent = (j - 1) >> 1;
                            if (heap[parent] <= heap[j]) break;
                            int64_t tmp = heap[parent]; heap[parent] = heap[j]; heap[j] = tmp;
                            j = parent;
                        }
                    } else if (cnt > heap[0]) {
                        heap[0] = cnt;
                        int64_t j = 0;
                        for (;;) {
                            int64_t l = j * 2 + 1, r = l + 1, m = j;
                            if (l < heap_n && heap[l] < heap[m]) m = l;
                            if (r < heap_n && heap[r] < heap[m]) m = r;
                            if (m == j) break;
                            int64_t tmp = heap[m]; heap[m] = heap[j]; heap[j] = tmp;
                            j = m;
                        }
                    }
                }
            }

            int64_t threshold = (heap_n == k_take) ? heap[0] : 1;
            uint32_t heavy_count = 0;
            for (uint32_t p = 0; p < cnt_ingest.n_parts; p++) {
                group_ht_t* ph = &cnt_ingest.part_hts[p];
                uint16_t rs = ph->layout.row_stride;
                for (uint32_t gi = 0; gi < ph->grp_count; gi++) {
                    const char* row = ph->rows + (size_t)gi * rs;
                    int64_t cnt = *(const int64_t*)(const void*)row;
                    if (cnt >= threshold) heavy_count++;
                }
            }

            if (threshold <= 1 ||
                (uint64_t)heavy_count * 4u >= (uint64_t)total_count_groups * 3u) {
                pivot_ingest_free(&cnt_ingest);
                goto skip_top_count_filter;
            }

            if (heavy_count > 0 && total_count_groups > 0) {
                uint32_t cap = 256;
                while (cap < heavy_count * 2u && cap < (1u << 30)) cap <<= 1;
                memset(&top_ht, 0, sizeof(top_ht));
                if (group_ht_init_sized(&top_ht, cap, &ght_layout, heavy_count)) {
                    top_ht_ready = true;
                    group_ht_set_key_data(&top_ht, key_data);
                    for (uint32_t p = 0; p < cnt_ingest.n_parts && !top_ht.oom; p++) {
                        group_ht_t* ph = &cnt_ingest.part_hts[p];
                        uint16_t rs = ph->layout.row_stride;
                        for (uint32_t gi = 0; gi < ph->grp_count; gi++) {
                            const char* row = ph->rows + (size_t)gi * rs;
                            int64_t cnt = *(const int64_t*)(const void*)row;
                            if (cnt < threshold) continue;
                            const int64_t* keys = (const int64_t*)(const void*)(row + 8);
                            if (!group_ht_insert_empty_group(&top_ht, keys, key_types))
                                break;
                        }
                    }
                    if (!top_ht.oom) {
                        pivot_ingest_free(&cnt_ingest);
                        group_rows_range_existing(&top_ht, key_data, key_types,
                            key_attrs, key_vecs, agg_vecs, agg_strlen, rowsel,
                            0, n_scan, match_idx);
                        if (ray_interrupted()) {
                            result = ray_error("cancel", "interrupted");
                            goto cleanup;
                        }
                        final_ht = &top_ht;
                        goto build_from_final_ht;
                    }
                }
            }
            pivot_ingest_free(&cnt_ingest);
            if (top_ht_ready) {
                group_ht_free(&top_ht);
                top_ht_ready = false;
            }
        }
    }

skip_top_count_filter:

    if (pool && nrows >= RAY_PARALLEL_THRESHOLD && n_total > 1 && !rowsel) {
        size_t n_bufs = (size_t)n_total * RADIX_P;
        radix_bufs = (radix_buf_t*)scratch_calloc(&radix_bufs_hdr,
            n_bufs * sizeof(radix_buf_t));
        if (!radix_bufs) goto sequential_fallback;

        /* Pre-size each buffer: 1.5x expected, capped so total ≤ 2 GB.
         * Buffers grow on demand via radix_buf_push doubling. */
        uint32_t buf_init = (uint32_t)((uint64_t)nrows / (RADIX_P * n_total));
        if (buf_init < 64) buf_init = 64;
        buf_init = buf_init + buf_init / 2;  /* 1.5x headroom */
        uint16_t estride = ght_layout.entry_stride;
        {
            /* Cap: total pre-alloc ≤ 2 GB */
            size_t total_pre = (size_t)n_bufs * buf_init * estride;
            if (total_pre > (size_t)2 << 30) {
                buf_init = (uint32_t)(((size_t)2 << 30) / ((size_t)n_bufs * estride));
                if (buf_init < 64) buf_init = 64;
            }
        }
        for (size_t i = 0; i < n_bufs; i++) {
            radix_bufs[i].data = (char*)scratch_alloc(
                &radix_bufs[i]._hdr, (size_t)buf_init * estride);
            radix_bufs[i].count = 0;
            radix_bufs[i].cap = buf_init;
        }

        /* Compute per-key nullability — lets phase1 skip null checks on
         * key columns with no nulls (the common case). */
        uint8_t p1_nullable = 0;
        for (uint8_t k = 0; k < n_keys; k++) {
            if (!key_vecs[k]) continue;
            ray_t* src = (key_vecs[k]->attrs & RAY_ATTR_SLICE)
                         ? key_vecs[k]->slice_parent : key_vecs[k];
            if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
                p1_nullable |= (uint8_t)(1u << k);
        }

        /* Phase 1: parallel hash + copy keys/agg values into fat entries */
        radix_phase1_ctx_t p1ctx = {
            .key_data      = key_data,
            .key_types     = key_types,
            .key_attrs     = key_attrs,
            .key_vecs      = key_vecs,
            .nullable_mask = p1_nullable,
            .agg_vecs      = agg_vecs,
            .agg_vecs2     = agg_vecs2,
            .agg_strlen    = agg_strlen,
            .n_workers     = n_total,
            .bufs          = radix_bufs,
            .layout        = ght_layout,
            .rowsel        = rowsel,
            .match_idx     = match_idx,
        };
        ray_pool_dispatch(pool, radix_phase1_fn, &p1ctx, n_scan);
        CHECK_CANCEL_GOTO(pool, cleanup);

        /* Check for OOM during phase 1 radix buffer growth */
        {
            bool phase1_oom = false;
            for (size_t i = 0; i < n_bufs; i++) {
                if (radix_bufs[i].oom) { phase1_oom = true; break; }
            }
            if (phase1_oom) {
                for (size_t i = 0; i < n_bufs; i++) scratch_free(radix_bufs[i]._hdr);
                scratch_free(radix_bufs_hdr);
                radix_bufs = NULL;
                goto sequential_fallback;
            }
        }

        /* Phase 2: parallel per-partition aggregation (no column access) */
        part_hts = (group_ht_t*)scratch_calloc(&part_hts_hdr,
            RADIX_P * sizeof(group_ht_t));
        if (!part_hts) {
            for (size_t i = 0; i < n_bufs; i++) scratch_free(radix_bufs[i]._hdr);
            scratch_free(radix_bufs_hdr);
            radix_bufs = NULL;
            goto sequential_fallback;
        }

        radix_phase2_ctx_t p2ctx = {
            .key_types   = key_types,
            .n_keys      = n_keys,
            .n_workers   = n_total,
            .bufs        = radix_bufs,
            .part_hts    = part_hts,
            .layout      = ght_layout,
            .key_data    = key_data,
        };
        ray_pool_dispatch_n(pool, radix_phase2_fn, &p2ctx, RADIX_P);
        CHECK_CANCEL_GOTO(pool, cleanup);

        if (radix_bufs) {
            size_t n_bufs_free = (size_t)n_total * RADIX_P;
            for (size_t i = 0; i < n_bufs_free; i++)
                scratch_free(radix_bufs[i]._hdr);
            scratch_free(radix_bufs_hdr);
            radix_bufs = NULL;
            radix_bufs_hdr = NULL;
            ray_heap_gc();
        }

        /* Prefix offsets */
        uint32_t part_offsets[RADIX_P + 1];
        part_offsets[0] = 0;
        for (uint32_t p = 0; p < RADIX_P; p++)
            part_offsets[p + 1] = part_offsets[p] + part_hts[p].grp_count;
        uint32_t total_grps = part_offsets[RADIX_P];

        /* Build result directly from partition HTs */
        int64_t total_cols = n_keys + n_aggs;
        result = ray_table_new(total_cols);
        if (!result || RAY_IS_ERR(result)) goto cleanup;

        /* Pre-allocate key columns */
        ray_t* key_cols[n_keys];
        char* key_dsts[n_keys];
        int8_t key_out_types[n_keys];
        uint8_t key_esizes[n_keys];
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_t* src_col = key_vecs[k];
            key_cols[k] = NULL;
            key_dsts[k] = NULL;
            key_out_types[k] = 0;
            key_esizes[k] = 0;
            if (!src_col) continue;
            uint8_t esz = ray_sym_elem_size(src_col->type, src_col->attrs);
            ray_t* new_col;
            if (src_col->type == RAY_SYM)
                new_col = ray_sym_vec_new(src_col->attrs & RAY_SYM_W_MASK, (int64_t)total_grps);
            else
                new_col = ray_vec_new(src_col->type, (int64_t)total_grps);
            if (!new_col || RAY_IS_ERR(new_col)) continue;
            new_col->len = (int64_t)total_grps;
            key_cols[k] = new_col;
            key_dsts[k] = (char*)ray_data(new_col);
            key_out_types[k] = src_col->type;
            key_esizes[k] = esz;
        }

        /* Pre-allocate agg result vectors */
        agg_out_t agg_outs[n_aggs];
        ray_t* agg_cols[n_aggs];
        for (uint8_t a = 0; a < n_aggs; a++) {
            uint16_t agg_op = ext->agg_ops[a];
            ray_t* agg_col = agg_vecs[a];
            bool is_f64 = agg_col && agg_col->type == RAY_F64;
            int8_t out_type;
            switch (agg_op) {
                case OP_AVG:
                case OP_STDDEV: case OP_STDDEV_POP:
                case OP_VAR: case OP_VAR_POP:
                case OP_PEARSON_CORR:
                case OP_MEDIAN:
                    out_type = RAY_F64; break;
                case OP_COUNT: out_type = RAY_I64; break;
                case OP_SUM: case OP_PROD:
                    out_type = is_f64 ? RAY_F64 : RAY_I64; break;
                default:
                    out_type = agg_col ? agg_col->type : RAY_I64; break;
            }
            ray_t* new_col = ray_vec_new(out_type, (int64_t)total_grps);
            if (!new_col || RAY_IS_ERR(new_col)) {
                agg_cols[a] = NULL;
                memset(&agg_outs[a], 0, sizeof(agg_outs[a]));
                continue;
            }
            new_col->len = (int64_t)total_grps;
            agg_cols[a] = new_col;
            agg_outs[a] = (agg_out_t){
                .out_type = out_type, .src_f64 = is_f64,
                .agg_op = agg_op,
                .affine = agg_affine[a].enabled,
                .bias_f64 = agg_affine[a].bias_f64,
                .bias_i64 = agg_affine[a].bias_i64,
                .dst = ray_data(new_col),
                .vec = new_col,
            };
        }

        /* Pre-allocate nullmaps for agg result vectors (parallel safety) */
        bool nullmap_prep_ok[n_aggs];
        for (uint8_t a = 0; a < n_aggs; a++)
            nullmap_prep_ok[a] = agg_cols[a] && (grp_prepare_nullmap(agg_outs[a].vec) == RAY_OK);

        /* Pre-prepare nullmaps on output key columns for parallel null writes */
        for (uint8_t k = 0; k < n_keys; k++)
            if (key_cols[k]) grp_prepare_nullmap(key_cols[k]);

        /* Phase 3: parallel key gather + agg result building from inline rows */
        {
            radix_phase3_ctx_t p3ctx = {
                .part_hts     = part_hts,
                .part_offsets = part_offsets,
                .key_dsts     = key_dsts,
                .key_types    = key_out_types,
                .key_attrs    = key_attrs,
                .key_esizes   = key_esizes,
                .key_cols     = key_cols,
                .n_keys       = n_keys,
                .agg_outs     = agg_outs,
                .n_aggs       = n_aggs,
                .key_src_data = key_data,
            };
            ray_pool_dispatch_n(pool, radix_phase3_fn, &p3ctx, RADIX_P);
        }

        /* Post-radix holistic fill: OP_MEDIAN slots need a per-group
         * value slice + quickselect that doesn't fit the row-layout HT.
         * Re-probe source rows to recover global gids, build a
         * group-contiguous idx_buf, then dispatch ray_median_per_group_buf
         * once per OP_MEDIAN agg.  See helpers above for the rationale. */
        if (ght_layout.agg_is_holistic) {
            int64_t n_groups = (int64_t)total_grps;

            /* row_gid[nrows] — global group id per source row, or -1 on
             * miss (defensive sentinel; phase-2 inserts every probed row). */
            ray_t* rg_hdr = NULL;
            int64_t* row_gid = (int64_t*)scratch_alloc(&rg_hdr,
                (size_t)nrows * sizeof(int64_t));
            if (!row_gid) { result = ray_error("oom", NULL); goto cleanup; }

            uint8_t reprobe_nullable = 0;
            for (uint8_t k = 0; k < n_keys; k++) {
                if (!key_vecs[k]) continue;
                ray_t* src = (key_vecs[k]->attrs & RAY_ATTR_SLICE)
                             ? key_vecs[k]->slice_parent : key_vecs[k];
                if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
                    reprobe_nullable |= (uint8_t)(1u << k);
            }
            reprobe_ctx_t rp = {
                .key_data = key_data,
                .key_types = key_types,
                .key_attrs = key_attrs,
                .key_vecs = key_vecs,
                .n_keys = n_keys,
                .nullable_mask = reprobe_nullable,
                .wide_mask = ght_layout.wide_key_mask,
                .wide_esz = ght_layout.wide_key_esz,
                .part_hts = part_hts,
                .part_offsets = part_offsets,
                .row_gid = row_gid,
                .match_idx = match_idx,
            };
            ray_pool_dispatch(pool, reprobe_rows_fn, &rp, n_scan);

            /* Build idx_buf + offsets + grp_cnt via histogram/scatter.
             *
             * n_tasks is capped to a small multiple of worker count: the
             * hist/cur matrices are sized [n_tasks * n_groups] and the
             * cumsum below walks every entry serially.  With the default
             * 8K-morsel grain, 10M rows × 100k groups would inflate hist
             * to ~1GB and the cumsum to ~120M cache-strided ops (≈1.4s).
             * Capping n_tasks ≈ worker count keeps memory in the L2/L3
             * regime and the cumsum in single-digit ms, while leaving
             * scatter parallelism saturated (each task is large enough). */
            int64_t n_workers = (int64_t)ray_pool_total_workers(pool);
            int64_t med_ntasks = n_workers > 1 ? n_workers : 1;
            /* Don't over-task tiny inputs — each task should see ≥ 8K
             * rows so the per-task fixed overhead is amortised. */
            int64_t min_grain = 8192;
            if (med_ntasks * min_grain > nrows)
                med_ntasks = (nrows + min_grain - 1) / min_grain;
            if (med_ntasks < 1) med_ntasks = 1;
            int64_t med_grain = (nrows + med_ntasks - 1) / med_ntasks;
            if (med_grain < 1) med_grain = 1;
            /* Recompute med_ntasks from grain so the last task covers the
             * tail without overflow (grain rounds up; final task may be
             * shorter). */
            med_ntasks = (nrows + med_grain - 1) / med_grain;
            ray_t* hist_hdr = NULL;
            ray_t* cur_hdr  = NULL;
            ray_t* cnt_hdr  = NULL;
            ray_t* off_hdr  = NULL;
            int64_t* hist = (int64_t*)scratch_calloc(&hist_hdr,
                (size_t)med_ntasks * (size_t)n_groups * sizeof(int64_t));
            int64_t* cur  = (int64_t*)scratch_alloc(&cur_hdr,
                (size_t)med_ntasks * (size_t)n_groups * sizeof(int64_t));
            int64_t* grp_cnt = (int64_t*)scratch_alloc(&cnt_hdr,
                (size_t)n_groups * sizeof(int64_t));
            int64_t* offsets = (int64_t*)scratch_alloc(&off_hdr,
                (size_t)n_groups * sizeof(int64_t));
            ray_t* idx_hdr = NULL;
            int64_t* idx_buf = NULL;
            if (hist && cur && grp_cnt && offsets) {
                med_idx_ctx_t mctx = {
                    .row_gid = row_gid,
                    .hist = hist,
                    .cursor = cur,
                    .idx_buf = NULL,
                    .n_groups = n_groups,
                    .grain = med_grain,
                    .nrows = nrows,
                };
                ray_pool_dispatch_n(pool, med_idx_hist_fn, &mctx,
                                    (uint32_t)med_ntasks);
                int64_t total = 0;
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    int64_t cum = total;
                    for (int64_t t = 0; t < med_ntasks; t++) {
                        int64_t cn = hist[t * n_groups + gi];
                        cur[t * n_groups + gi] = cum;
                        cum += cn;
                    }
                    grp_cnt[gi] = cum - total;
                    offsets[gi] = total;
                    total = cum;
                }
                idx_buf = (int64_t*)scratch_alloc(&idx_hdr,
                    (size_t)(total > 0 ? total : 1) * sizeof(int64_t));
                if (idx_buf) {
                    mctx.idx_buf = idx_buf;
                    ray_pool_dispatch_n(pool, med_idx_scat_fn, &mctx,
                                        (uint32_t)med_ntasks);
                }
            }

            if (idx_buf) {
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (!(ght_layout.agg_is_holistic & (1u << a))) continue;
                    if (!agg_vecs[a] || !agg_cols[a]) continue;
                    uint16_t aop = ext->agg_ops[a];
                    ray_t* hol_vec = NULL;
                    const char* err_tag = "median: type";
                    if (aop == OP_MEDIAN) {
                        hol_vec = ray_median_per_group_buf(
                            agg_vecs[a], idx_buf, offsets, grp_cnt, n_groups);
                    } else if (aop == OP_TOP_N || aop == OP_BOT_N) {
                        int64_t k_val = (ext->agg_k && ext->agg_k[a] > 0)
                                        ? ext->agg_k[a] : 1;
                        hol_vec = ray_topk_per_group_buf(
                            agg_vecs[a], k_val,
                            aop == OP_TOP_N ? 1 : 0,
                            idx_buf, offsets, grp_cnt, n_groups);
                        err_tag = "top/bot: type";
                    }
                    if (!hol_vec) {
                        if (hist_hdr) scratch_free(hist_hdr);
                        if (cur_hdr)  scratch_free(cur_hdr);
                        if (cnt_hdr)  scratch_free(cnt_hdr);
                        if (off_hdr)  scratch_free(off_hdr);
                        if (idx_hdr)  scratch_free(idx_hdr);
                        scratch_free(rg_hdr);
                        result = ray_error("nyi", err_tag);
                        goto cleanup;
                    }
                    if (RAY_IS_ERR(hol_vec)) {
                        if (hist_hdr) scratch_free(hist_hdr);
                        if (cur_hdr)  scratch_free(cur_hdr);
                        if (cnt_hdr)  scratch_free(cnt_hdr);
                        if (off_hdr)  scratch_free(off_hdr);
                        if (idx_hdr)  scratch_free(idx_hdr);
                        scratch_free(rg_hdr);
                        result = hol_vec;
                        goto cleanup;
                    }
                    /* Replace the stub agg_cols[a] vector with the
                     * filled holistic column.  Update agg_outs[a].vec
                     * to track the same pointer so the downstream
                     * finalize_nulls loop operates on live memory
                     * (the prior stub's ref hits zero on this
                     * release). */
                    ray_release(agg_cols[a]);
                    agg_cols[a] = hol_vec;
                    agg_outs[a].vec = hol_vec;
                }
            } else {
                if (hist_hdr) scratch_free(hist_hdr);
                if (cur_hdr)  scratch_free(cur_hdr);
                if (cnt_hdr)  scratch_free(cnt_hdr);
                if (off_hdr)  scratch_free(off_hdr);
                if (idx_hdr)  scratch_free(idx_hdr);
                scratch_free(rg_hdr);
                result = ray_error("oom", NULL);
                goto cleanup;
            }

            if (hist_hdr) scratch_free(hist_hdr);
            if (cur_hdr)  scratch_free(cur_hdr);
            if (cnt_hdr)  scratch_free(cnt_hdr);
            if (off_hdr)  scratch_free(off_hdr);
            if (idx_hdr)  scratch_free(idx_hdr);
            scratch_free(rg_hdr);
        }

        /* Fixup: if nullmap prep failed for any VAR/STDDEV agg, re-scan
         * hash tables sequentially to ensure all null bits were set */
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (nullmap_prep_ok[a] || !agg_cols[a]) continue;
            uint16_t op = agg_outs[a].agg_op;
            if (op != OP_VAR && op != OP_VAR_POP &&
                op != OP_STDDEV && op != OP_STDDEV_POP) continue;
            for (uint32_t p = 0; p < RADIX_P; p++) {
                group_ht_t* ph = &part_hts[p];
                uint32_t gc = ph->grp_count;
                uint32_t off = part_offsets[p];
                uint16_t rs = ph->layout.row_stride;
                for (uint32_t gi = 0; gi < gc; gi++) {
                    const char* row = ph->rows + (size_t)gi * rs;
                    int64_t cnt = *(const int64_t*)(const void*)row;
                    bool insuf = (op == OP_VAR || op == OP_STDDEV) ? cnt <= 1 : cnt <= 0;
                    if (insuf) ray_vec_set_null(agg_outs[a].vec, off + gi, true);
                }
            }
        }

        /* Finalize null flags after parallel execution.  Holistic slots
         * are filled by the post-radix pass into a fresh column; we
         * already updated agg_outs[a].vec to track it.  For RAY_LIST
         * cells (OP_TOP_N / OP_BOT_N) the per-cell nullmap is not
         * consulted downstream — finalize is a no-op-y read of attrs. */
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (!agg_cols[a]) continue;
            if (agg_outs[a].vec && agg_outs[a].vec->type == RAY_LIST) continue;
            grp_finalize_nulls(agg_outs[a].vec);
        }
        for (uint8_t k = 0; k < n_keys; k++) {
            if (!key_cols[k]) continue;
            grp_finalize_nulls(key_cols[k]);
        }

        /* Add key columns to result */
        for (uint8_t k = 0; k < n_keys; k++) {
            if (!key_cols[k]) continue;
            ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]->id);
            int64_t name_id = key_ext ? key_ext->sym : k;
            result = ray_table_add_col(result, name_id, key_cols[k]);
            ray_release(key_cols[k]);
        }

        /* Add agg columns to result */
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (!agg_cols[a]) continue;
            uint16_t agg_op = ext->agg_ops[a];
            ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
            int64_t name_id;
            if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
                ray_t* name_atom = ray_sym_str(agg_ext->sym);
                const char* base = name_atom ? ray_str_ptr(name_atom) : NULL;
                size_t blen = base ? ray_str_len(name_atom) : 0;
                const char* sfx = "";
                size_t slen = 0;
                switch (agg_op) {
                    case OP_SUM:   sfx = "_sum";   slen = 4; break;
                    case OP_COUNT: sfx = "_count"; slen = 6; break;
                    case OP_AVG:   sfx = "_mean";  slen = 5; break;
                    case OP_MIN:   sfx = "_min";   slen = 4; break;
                    case OP_MAX:   sfx = "_max";   slen = 4; break;
                    case OP_FIRST: sfx = "_first"; slen = 6; break;
                    case OP_LAST:  sfx = "_last";  slen = 5; break;
                    case OP_STDDEV:     sfx = "_stddev";     slen = 7; break;
                    case OP_STDDEV_POP: sfx = "_stddev_pop"; slen = 11; break;
                    case OP_VAR:        sfx = "_var";        slen = 4; break;
                    case OP_VAR_POP:    sfx = "_var_pop";    slen = 8; break;
                    case OP_MEDIAN:     sfx = "_median";     slen = 7; break;
                    case OP_TOP_N:      sfx = "_top";        slen = 4; break;
                    case OP_BOT_N:      sfx = "_bot";        slen = 4; break;
                }
                char buf[256];
                ray_t* name_dyn_hdr = NULL;
                char* nbp = buf;
                size_t nbc = sizeof(buf);
                if (base && blen + slen >= sizeof(buf)) {
                    nbp = (char*)scratch_alloc(&name_dyn_hdr, blen + slen + 1);
                    if (nbp) nbc = blen + slen + 1;
                    else { nbp = buf; nbc = sizeof(buf); }
                }
                if (base && blen + slen < nbc) {
                    memcpy(nbp, base, blen);
                    memcpy(nbp + blen, sfx, slen);
                    name_id = ray_sym_intern(nbp, blen + slen);
                } else {
                    name_id = agg_ext->sym;
                }
                scratch_free(name_dyn_hdr);
            } else {
                name_id = (int64_t)(n_keys + a);
            }
            result = ray_table_add_col(result, name_id, agg_cols[a]);
            ray_release(agg_cols[a]);
        }

        goto cleanup;
    }

sequential_fallback:;
    /* Sequential path using row-layout HT */
    if (!group_ht_init(&single_ht, ht_cap, &ght_layout)) {
        result = ray_error("oom", NULL);
        goto cleanup;
    }
    group_rows_range(&single_ht, key_data, key_types, key_attrs, key_vecs, agg_vecs,
                     agg_vecs2, agg_strlen, rowsel,
                     0, n_scan, match_idx);
    final_ht = &single_ht;
    if (ray_interrupted()) { result = ray_error("cancel", "interrupted"); goto cleanup; }
    if (single_ht.oom) { result = ray_error("oom", NULL); goto cleanup; }

    /* Build result from sequential HT (inline row layout) */
build_from_final_ht:
    {
    uint32_t grp_count = final_ht->grp_count;
    const ght_layout_t* ly = &final_ht->layout;
    int64_t total_cols = n_keys + n_aggs;
    result = ray_table_new(total_cols);
    if (!result || RAY_IS_ERR(result)) goto cleanup;

    /* Key columns: read from inline group rows, narrow to original type.
     * Wide keys store a source row index in the HT slot; resolve it
     * through the original key column (key_data[k]) and copy bytes. */
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* src_col = key_vecs[k];
        if (!src_col) continue;
        uint8_t esz = col_esz(src_col);
        int8_t kt = src_col->type;

        ray_t* new_col = col_vec_new(src_col, (int64_t)grp_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = (int64_t)grp_count;

        bool is_wide = (ly->wide_key_mask & (1u << k)) != 0;
        const char* src_base = is_wide ? (const char*)key_data[k] : NULL;

        for (uint32_t gi = 0; gi < grp_count; gi++) {
            const char* row = final_ht->rows + (size_t)gi * ly->row_stride;
            const int64_t* rkeys = (const int64_t*)(row + 8);
            int64_t kv = rkeys[k];
            int64_t null_mask = rkeys[n_keys];
            if (null_mask & (int64_t)(1u << k)) {
                ray_vec_set_null(new_col, (int64_t)gi, true);
                continue;
            }
            if (is_wide) {
                char* dst = (char*)ray_data(new_col) + (size_t)gi * esz;
                memcpy(dst, src_base + (size_t)kv * esz, esz);
            } else if (kt == RAY_F64) {
                char* dst = (char*)ray_data(new_col) + (size_t)gi * esz;
                memcpy(dst, &kv, 8);
            } else {
                write_col_i64(ray_data(new_col), gi, kv, kt, new_col->attrs);
            }
        }

        ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]->id);
        int64_t name_id = key_ext ? key_ext->sym : k;
        result = ray_table_add_col(result, name_id, new_col);
        ray_release(new_col);
    }

    /* If any holistic agg (OP_MEDIAN) is present, run a sequential
     * re-probe + median fill into a per-slot output vector array.
     * Built lazily on first need and reused across all median slots. */
    ray_t** med_out = NULL;
    ray_t* med_hdr = NULL;
    if (ly->agg_is_holistic) {
        med_out = (ray_t**)scratch_calloc(&med_hdr,
            (size_t)n_aggs * sizeof(ray_t*));
        if (med_out) {
            /* Build row_gid + grp_cnt + idx_buf sequentially.  The
             * seq path runs at small nrows so a single-thread pass is
             * fine; matches the radix path's logic but without
             * dispatch overhead. */
            ray_t* rg_hdr = NULL;
            int64_t* row_gid = (int64_t*)scratch_alloc(&rg_hdr,
                (size_t)nrows * sizeof(int64_t));
            ray_t* cnt_hdr_s = NULL;
            int64_t* grp_cnt_s = (int64_t*)scratch_calloc(&cnt_hdr_s,
                (size_t)grp_count * sizeof(int64_t));
            ray_t* off_hdr_s = NULL;
            int64_t* offsets_s = (int64_t*)scratch_alloc(&off_hdr_s,
                (size_t)grp_count * sizeof(int64_t));
            ray_t* pos_hdr_s = NULL;
            int64_t* pos_s = (int64_t*)scratch_alloc(&pos_hdr_s,
                (size_t)grp_count * sizeof(int64_t));
            if (row_gid && grp_cnt_s && offsets_s && pos_s) {
                uint8_t reprobe_nullable_s = 0;
                for (uint8_t k = 0; k < n_keys; k++) {
                    if (!key_vecs[k]) continue;
                    ray_t* src = (key_vecs[k]->attrs & RAY_ATTR_SLICE)
                                 ? key_vecs[k]->slice_parent : key_vecs[k];
                    if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
                        reprobe_nullable_s |= (uint8_t)(1u << k);
                }
                int64_t ek_buf[9];
                for (int64_t i = 0; i < n_scan; i++) {
                    int64_t row = match_idx ? match_idx[i] : i;
                    uint64_t h = 0;
                    int64_t null_mask = 0;
                    for (uint8_t k = 0; k < n_keys; k++) {
                        int8_t t = key_types[k];
                        uint64_t kh;
                        bool is_null = (reprobe_nullable_s & (1u << k))
                                       && ray_vec_is_null(key_vecs[k], row);
                        if (is_null) {
                            null_mask |= (int64_t)(1u << k);
                            ek_buf[k] = 0;
                            kh = ray_hash_i64(0);
                        } else if (ly->wide_key_mask & (1u << k)) {
                            uint8_t esz = ly->wide_key_esz[k];
                            const void* src = (const char*)key_data[k] + (size_t)row * esz;
                            ek_buf[k] = row;
                            kh = ray_hash_bytes(src, esz);
                        } else if (t == RAY_F64) {
                            int64_t kv;
                            memcpy(&kv, &((double*)key_data[k])[row], 8);
                            ek_buf[k] = kv;
                            kh = ray_hash_f64(((double*)key_data[k])[row]);
                        } else {
                            int64_t kv = read_col_i64(key_data[k], row, t, key_attrs[k]);
                            ek_buf[k] = kv;
                            kh = ray_hash_i64(kv);
                        }
                        h = (k == 0) ? kh : ray_hash_combine(h, kh);
                    }
                    ek_buf[n_keys] = null_mask;
                    if (null_mask) h = ray_hash_combine(h, ray_hash_i64(null_mask));
                    uint32_t gid = group_ht_lookup_gid(final_ht, h, ek_buf, key_types);
                    row_gid[row] = (gid == UINT32_MAX) ? -1 : (int64_t)gid;
                    if (gid != UINT32_MAX) grp_cnt_s[gid]++;
                }
                int64_t total_s = 0;
                for (uint32_t gi = 0; gi < grp_count; gi++) {
                    offsets_s[gi] = total_s;
                    pos_s[gi] = total_s;
                    total_s += grp_cnt_s[gi];
                }
                ray_t* ix_hdr_s = NULL;
                int64_t* idx_buf_s = (int64_t*)scratch_alloc(&ix_hdr_s,
                    (size_t)(total_s > 0 ? total_s : 1) * sizeof(int64_t));
                if (idx_buf_s) {
                    for (int64_t i = 0; i < n_scan; i++) {
                        int64_t row = match_idx ? match_idx[i] : i;
                        int64_t gi = row_gid[row];
                        if (gi >= 0) idx_buf_s[pos_s[gi]++] = row;
                    }
                    for (uint8_t a = 0; a < n_aggs; a++) {
                        if (!(ly->agg_is_holistic & (1u << a))) continue;
                        if (!agg_vecs[a]) continue;
                        uint16_t aop = ext->agg_ops[a];
                        ray_t* hol_vec = NULL;
                        if (aop == OP_MEDIAN) {
                            hol_vec = ray_median_per_group_buf(
                                agg_vecs[a], idx_buf_s, offsets_s, grp_cnt_s,
                                (int64_t)grp_count);
                        } else if (aop == OP_TOP_N || aop == OP_BOT_N) {
                            int64_t k_val = (ext->agg_k && ext->agg_k[a] > 0)
                                            ? ext->agg_k[a] : 1;
                            hol_vec = ray_topk_per_group_buf(
                                agg_vecs[a], k_val,
                                aop == OP_TOP_N ? 1 : 0,
                                idx_buf_s, offsets_s, grp_cnt_s,
                                (int64_t)grp_count);
                        }
                        med_out[a] = hol_vec;  /* NULL or RAY_IS_ERR handled below */
                    }
                    scratch_free(ix_hdr_s);
                }
            }
            scratch_free(rg_hdr);
            scratch_free(cnt_hdr_s);
            scratch_free(off_hdr_s);
            scratch_free(pos_hdr_s);
        }
    }

    /* Agg columns from inline accumulators */
    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t agg_op = ext->agg_ops[a];
        ray_t* agg_col = agg_vecs[a];
        bool is_f64 = agg_col && agg_col->type == RAY_F64;
        int8_t out_type;
        switch (agg_op) {
            case OP_AVG:
            case OP_STDDEV: case OP_STDDEV_POP:
            case OP_VAR: case OP_VAR_POP:
            case OP_PEARSON_CORR:
            case OP_MEDIAN:
                out_type = RAY_F64; break;
            case OP_COUNT: out_type = RAY_I64; break;
            case OP_SUM: case OP_PROD:
                out_type = is_f64 ? RAY_F64 : RAY_I64; break;
            default:
                out_type = agg_col ? agg_col->type : RAY_I64; break;
        }
        ray_t* new_col;
        bool is_holistic = (agg_op == OP_MEDIAN || agg_op == OP_TOP_N ||
                            agg_op == OP_BOT_N);
        if (is_holistic && med_out && med_out[a]
            && !RAY_IS_ERR(med_out[a])) {
            new_col = med_out[a];
            med_out[a] = NULL;  /* transferred ownership */
        } else if (is_holistic) {
            /* Unsupported source type or earlier failure — skip. */
            continue;
        } else {
            new_col = ray_vec_new(out_type, (int64_t)grp_count);
            if (!new_col || RAY_IS_ERR(new_col)) continue;
            new_col->len = (int64_t)grp_count;
        }

        int8_t s = ly->agg_val_slot[a]; /* unified accum slot */
        /* Holistic agg (OP_MEDIAN / OP_TOP_N / OP_BOT_N) is already
         * filled — skip row-layout reads.  Naming + add_col below
         * still applies. */
        if (is_holistic) goto med_attach;
        for (uint32_t gi = 0; gi < grp_count; gi++) {
            const char* row = final_ht->rows + (size_t)gi * ly->row_stride;
            int64_t cnt = *(const int64_t*)(const void*)row;
            if (out_type == RAY_F64) {
                double v;
                switch (agg_op) {
                    case OP_SUM:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_f64 * cnt;
                        break;
                    case OP_PROD:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                        break;
                    case OP_AVG:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s) / cnt
                                   : (double)ROW_RD_I64(row, ly->off_sum, s) / cnt;
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_f64;
                        break;
                    case OP_MIN:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_min, s)
                                   : (double)ROW_RD_I64(row, ly->off_min, s);
                        break;
                    case OP_MAX:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_max, s)
                                   : (double)ROW_RD_I64(row, ly->off_max, s);
                        break;
                    case OP_FIRST: case OP_LAST:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                        break;
                    case OP_VAR: case OP_VAR_POP:
                    case OP_STDDEV: case OP_STDDEV_POP: {
                        bool insuf = (agg_op == OP_VAR || agg_op == OP_STDDEV) ? cnt <= 1 : cnt <= 0;
                        if (insuf) { v = 0.0; ray_vec_set_null(new_col, gi, true); break; }
                        double sum_val = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                                : (double)ROW_RD_I64(row, ly->off_sum, s);
                        double sq_val = ly->off_sumsq ? ROW_RD_F64(row, ly->off_sumsq, s) : 0.0;
                        double mean = sum_val / cnt;
                        double var_pop = sq_val / cnt - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        if (agg_op == OP_VAR_POP) v = var_pop;
                        else if (agg_op == OP_VAR) v = var_pop * cnt / (cnt - 1);
                        else if (agg_op == OP_STDDEV_POP) v = sqrt(var_pop);
                        else v = sqrt(var_pop * cnt / (cnt - 1));
                        break;
                    }
                    case OP_PEARSON_CORR: {
                        if (cnt < 2) { v = 0.0; ray_vec_set_null(new_col, gi, true); break; }
                        double sx  = is_f64 ? ROW_RD_F64(row, ly->off_sum,    s)
                                            : (double)ROW_RD_I64(row, ly->off_sum, s);
                        double sxx = ly->off_sumsq ? ROW_RD_F64(row, ly->off_sumsq, s) : 0.0;
                        double sy  = ly->off_sum_y   ? ROW_RD_F64(row, ly->off_sum_y,   s) : 0.0;
                        double syy = ly->off_sumsq_y ? ROW_RD_F64(row, ly->off_sumsq_y, s) : 0.0;
                        double sxy = ly->off_sumxy   ? ROW_RD_F64(row, ly->off_sumxy,   s) : 0.0;
                        double dn  = (double)cnt;
                        double num = dn * sxy - sx * sy;
                        double dx  = dn * sxx - sx * sx;
                        double dy  = dn * syy - sy * sy;
                        if (dx <= 0.0 || dy <= 0.0) { v = NAN; break; }
                        v = num / sqrt(dx * dy);
                        break;
                    }
                    default: v = 0.0; break;
                }
                ((double*)ray_data(new_col))[gi] = v;
            } else {
                int64_t v;
                switch (agg_op) {
                    case OP_SUM:
                        v = ROW_RD_I64(row, ly->off_sum, s);
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_i64 * cnt;
                        break;
                    case OP_PROD:  v = ROW_RD_I64(row, ly->off_sum, s); break;
                    case OP_COUNT: v = cnt; break;
                    case OP_MIN:   v = ROW_RD_I64(row, ly->off_min, s); break;
                    case OP_MAX:   v = ROW_RD_I64(row, ly->off_max, s); break;
                    case OP_FIRST: case OP_LAST: v = ROW_RD_I64(row, ly->off_sum, s); break;
                    default:       v = 0; break;
                }
                ((int64_t*)ray_data(new_col))[gi] = v;
            }
        }

    med_attach:;
        /* Generate unique column name */
        ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]->id);
        int64_t name_id;
        if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
            ray_t* name_atom = ray_sym_str(agg_ext->sym);
            const char* base = name_atom ? ray_str_ptr(name_atom) : NULL;
            size_t blen = base ? ray_str_len(name_atom) : 0;
            const char* sfx = "";
            size_t slen = 0;
            switch (agg_op) {
                case OP_SUM:   sfx = "_sum";   slen = 4; break;
                case OP_PROD:  sfx = "_prod";  slen = 5; break;
                case OP_COUNT: sfx = "_count"; slen = 6; break;
                case OP_AVG:   sfx = "_mean";  slen = 5; break;
                case OP_MIN:   sfx = "_min";   slen = 4; break;
                case OP_MAX:   sfx = "_max";   slen = 4; break;
                case OP_FIRST: sfx = "_first"; slen = 6; break;
                case OP_LAST:  sfx = "_last";  slen = 5; break;
                case OP_STDDEV:     sfx = "_stddev";     slen = 7; break;
                case OP_STDDEV_POP: sfx = "_stddev_pop"; slen = 11; break;
                case OP_VAR:        sfx = "_var";        slen = 4; break;
                case OP_VAR_POP:    sfx = "_var_pop";    slen = 8; break;
                case OP_MEDIAN:     sfx = "_median";     slen = 7; break;
                case OP_TOP_N:      sfx = "_top";        slen = 4; break;
                case OP_BOT_N:      sfx = "_bot";        slen = 4; break;
            }
            char buf[256];
            if (base && blen + slen < sizeof(buf)) {
                memcpy(buf, base, blen);
                memcpy(buf + blen, sfx, slen);
                name_id = ray_sym_intern(buf, blen + slen);
            } else {
                name_id = agg_ext->sym;
            }
        } else {
            /* Expression agg input — synthetic name like "_e0_sum" */
            char nbuf[32];
            int np = 0;
            nbuf[np++] = '_'; nbuf[np++] = 'e';
            /* Multi-digit agg index */
            { uint8_t v = a; char dig[3]; int nd = 0;
              do { dig[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
              while (nd--) nbuf[np++] = dig[nd]; }
            const char* nsfx = "";
            size_t nslen = 0;
            switch (agg_op) {
                case OP_SUM:   nsfx = "_sum";   nslen = 4; break;
                case OP_PROD:  nsfx = "_prod";  nslen = 5; break;
                case OP_COUNT: nsfx = "_count"; nslen = 6; break;
                case OP_AVG:   nsfx = "_mean";  nslen = 5; break;
                case OP_MIN:   nsfx = "_min";   nslen = 4; break;
                case OP_MAX:   nsfx = "_max";   nslen = 4; break;
                case OP_FIRST: nsfx = "_first"; nslen = 6; break;
                case OP_LAST:  nsfx = "_last";  nslen = 5; break;
                case OP_STDDEV:     nsfx = "_stddev";     nslen = 7; break;
                case OP_STDDEV_POP: nsfx = "_stddev_pop"; nslen = 11; break;
                case OP_VAR:        nsfx = "_var";        nslen = 4; break;
                case OP_VAR_POP:    nsfx = "_var_pop";    nslen = 8; break;
                case OP_MEDIAN:     nsfx = "_median";     nslen = 7; break;
                case OP_TOP_N:      nsfx = "_top";        nslen = 4; break;
                case OP_BOT_N:      nsfx = "_bot";        nslen = 4; break;
            }
            memcpy(nbuf + np, nsfx, nslen);
            name_id = ray_sym_intern(nbuf, (size_t)np + nslen);
        }
        result = ray_table_add_col(result, name_id, new_col);
        ray_release(new_col);
    }
    if (med_out) {
        for (uint8_t a = 0; a < n_aggs; a++)
            if (med_out[a] && !RAY_IS_ERR(med_out[a])) ray_release(med_out[a]);
        scratch_free(med_hdr);
    }
    }

cleanup:
    if (final_ht == &single_ht) {
        group_ht_free(&single_ht);
    }
    if (top_ht_ready) {
        group_ht_free(&top_ht);
    }
    if (radix_bufs) {
        size_t n_bufs = (size_t)n_total * RADIX_P;
        for (size_t i = 0; i < n_bufs; i++) scratch_free(radix_bufs[i]._hdr);
        scratch_free(radix_bufs_hdr);
    }
    if (part_hts) {
        for (uint32_t p = 0; p < RADIX_P; p++) {
            if (part_hts[p].rows) group_ht_free(&part_hts[p]);
        }
        scratch_free(part_hts_hdr);
    }
    for (uint8_t a = 0; a < n_aggs; a++)
        { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
    for (uint8_t k = 0; k < n_keys; k++)
        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
    if (match_idx_block) ray_release(match_idx_block);

    ray_heap_gc();

    return result;
}

/* --------------------------------------------------------------------------
 * exec_group_per_partition — per-partition GROUP BY with merge
 *
 * Runs exec_group on each partition independently (zero-copy mmap segments),
 * then merges the small partial results via a second exec_group pass.
 *
 * Merge ops: SUM→SUM, COUNT→SUM, MIN→MIN, MAX→MAX, FIRST→FIRST, LAST→LAST.
 * AVG: decomposed into SUM+COUNT per partition, merged, then divided.
 * STDDEV/VAR: decomposed into SUM(x)+SUM(x²)+COUNT(x) per partition,
 *   merged with SUM, then final variance/stddev computed from merged totals.
 *
 * Returns NULL if any step fails (caller falls through to concat path).
 * -------------------------------------------------------------------------- */
static ray_t* __attribute__((noinline))
exec_group_per_partition(ray_t* parted_tbl, ray_op_ext_t* ext,
                         int32_t n_parts, const int64_t* key_syms,
                         const int64_t* agg_syms, int has_avg,
                         int has_stddev, int64_t group_limit) {

    uint8_t n_keys = ext->n_keys;
    uint8_t n_aggs = ext->n_aggs;

    /* Guard: fixed-size arrays below cap at 24 agg ops.
     * Each AVG adds 1 extra (COUNT), each STDDEV/VAR adds 2 (SUM_SQ + COUNT).
     * n_aggs + n_avg + 2*n_std must stay within 24. */
    if (n_aggs > 8 || n_keys > 8) return NULL;

    /* Identify MAPCOMMON vs PARTED keys.  MAPCOMMON keys are constant
     * within a partition, so they are excluded from per-partition GROUP BY
     * and reconstructed after concat. */
    uint8_t  n_mc_keys = 0;
    int64_t  mc_sym_ids[8];
    uint8_t  n_part_keys = 0;
    int64_t  pk_syms[8];       /* non-MAPCOMMON key sym IDs */

    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* pcol = ray_table_get_col(parted_tbl, key_syms[k]);
        if (pcol && pcol->type == RAY_MAPCOMMON) {
            mc_sym_ids[n_mc_keys++] = key_syms[k];
        } else {
            pk_syms[n_part_keys++] = key_syms[k];
        }
    }

    /* LIMIT pushdown: when all GROUP BY keys are MAPCOMMON (n_part_keys==0),
     * each partition produces exactly 1 group.  Limit the partition loop. */
    if (group_limit > 0 && n_part_keys == 0 && group_limit < n_parts)
        n_parts = (int32_t)group_limit;

    /* Decomposition: AVG(x) → SUM(x) + COUNT(x).
     * STDDEV/VAR(x) → SUM(x) + SUM(x²) + COUNT(x).
     * Build per-partition agg_ops with decomposed ops, then merge ops. */
    uint16_t part_ops[24];   /* per-partition agg ops */
    uint16_t merge_ops[24];  /* merge agg ops */
    uint8_t  avg_idx[8];     /* which original agg slots are AVG */
    uint8_t  std_idx[8];     /* which original agg slots are STDDEV/VAR */
    uint16_t std_orig_op[8]; /* original op for each std slot */
    uint8_t  n_avg = 0;
    uint8_t  n_std = 0;
    uint8_t  part_n_aggs = n_aggs;
    /* stddev_needs_sq[a]: index into part_ops for the SUM(x²) slot */
    uint8_t  std_sq_slot[8];
    uint8_t  std_cnt_slot[8];

    for (uint8_t a = 0; a < n_aggs; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop == OP_AVG) {
            part_ops[a] = OP_SUM;     /* partition: compute SUM */
            avg_idx[n_avg++] = a;
        } else if (aop == OP_STDDEV || aop == OP_STDDEV_POP ||
                   aop == OP_VAR || aop == OP_VAR_POP) {
            part_ops[a] = OP_SUM;     /* partition: compute SUM(x) */
            std_orig_op[n_std] = aop;
            std_idx[n_std++] = a;
        } else {
            part_ops[a] = aop;
        }
    }
    /* Guard: total decomposed slots must fit */
    if (n_aggs + n_avg + 2 * n_std > 24) return NULL;

    /* Append SUM(x²) for each STDDEV/VAR slot */
    for (uint8_t i = 0; i < n_std; i++) {
        std_sq_slot[i] = part_n_aggs;
        part_ops[part_n_aggs++] = OP_SUM;  /* SUM(x²) */
    }
    /* Append COUNT for each AVG column */
    for (uint8_t i = 0; i < n_avg; i++)
        part_ops[part_n_aggs++] = OP_COUNT;
    /* Append COUNT for each STDDEV/VAR column */
    for (uint8_t i = 0; i < n_std; i++) {
        std_cnt_slot[i] = part_n_aggs;
        part_ops[part_n_aggs++] = OP_COUNT;
    }

    /* Merge ops: SUM→SUM, COUNT→SUM, MIN→MIN, MAX→MAX,
     * FIRST→FIRST, LAST→LAST, all appended slots → SUM */
    for (uint8_t a = 0; a < part_n_aggs; a++) {
        merge_ops[a] = part_ops[a];
        if (merge_ops[a] == OP_COUNT) merge_ops[a] = OP_SUM;
    }

    /* Agg input syms for the decomposed ops.
     * AVG's COUNT uses same input column as the AVG itself.
     * STDDEV's SUM(x²) and COUNT use same input column as the STDDEV. */
    int64_t part_agg_syms[24];
    /* Flag: slot needs x*x graph node (for SUM(x²)) */
    int part_needs_sq[24];
    memset(part_needs_sq, 0, sizeof(part_needs_sq));

    for (uint8_t a = 0; a < n_aggs; a++)
        part_agg_syms[a] = agg_syms[a];
    /* SUM(x²) slots for STDDEV/VAR */
    for (uint8_t i = 0; i < n_std; i++) {
        part_agg_syms[std_sq_slot[i]] = agg_syms[std_idx[i]];
        part_needs_sq[std_sq_slot[i]] = 1;
    }
    /* COUNT slots for AVG */
    for (uint8_t i = 0; i < n_avg; i++)
        part_agg_syms[n_aggs + n_std + i] = agg_syms[avg_idx[i]];
    /* COUNT slots for STDDEV/VAR */
    for (uint8_t i = 0; i < n_std; i++)
        part_agg_syms[std_cnt_slot[i]] = agg_syms[std_idx[i]];

    /* ---- Batched incremental merge ----
     * Process partitions in batches of MERGE_BATCH.  After each batch:
     *   Phase 1: exec_group each partition in batch → batch_partials[]
     *   Phase 2: concat (running + batch_partials + MAPCOMMON) → merge_tbl
     *   Phase 3: merge GROUP BY → new running
     * Bounds peak memory to O(MERGE_BATCH × groups_per_partition). */
#define MERGE_BATCH 8

    /* Capture agg column name IDs from first partition result */
    int64_t agg_name_ids[24];
    int agg_names_captured = 0;

    ray_t* running = NULL;
    ray_t* merge_tbl = NULL;      /* last merge table (for column name fixup) */

    for (int32_t batch_start = 0; batch_start < n_parts;
         batch_start += MERGE_BATCH) {

        int32_t batch_end = batch_start + MERGE_BATCH;
        if (batch_end > n_parts) batch_end = n_parts;
        int32_t batch_n = batch_end - batch_start;

        /* Phase 1: exec_group each partition in this batch */
        ray_t* bp[MERGE_BATCH];
        memset(bp, 0, sizeof(bp));

        for (int32_t bi = 0; bi < batch_n; bi++) {
            int32_t p = batch_start + bi;

            /* Collect unique agg input sym IDs (avoid duplicate columns) */
            int64_t unique_agg[24];
            int n_unique_agg = 0;
            for (uint8_t a = 0; a < part_n_aggs; a++) {
                int dup = 0;
                for (int j = 0; j < n_unique_agg; j++)
                    if (unique_agg[j] == part_agg_syms[a]) { dup = 1; break; }
                if (!dup) {
                    for (uint8_t k = 0; k < n_keys; k++)
                        if (key_syms[k] == part_agg_syms[a]) { dup = 1; break; }
                    if (!dup) unique_agg[n_unique_agg++] = part_agg_syms[a];
                }
            }

            ray_t* sub = ray_table_new((int64_t)(n_part_keys + n_unique_agg));
            if (!sub || RAY_IS_ERR(sub)) goto batch_fail;

            for (uint8_t k = 0; k < n_part_keys; k++) {
                ray_t* pcol = ray_table_get_col(parted_tbl, pk_syms[k]);
                if (!pcol || !RAY_IS_PARTED(pcol->type)) {
                    ray_release(sub); goto batch_fail;
                }
                ray_t* seg = ((ray_t**)ray_data(pcol))[p];
                if (!seg) { ray_release(sub); goto batch_fail; }
                ray_retain(seg);
                sub = ray_table_add_col(sub, pk_syms[k], seg);
                ray_release(seg);
            }
            for (int j = 0; j < n_unique_agg; j++) {
                ray_t* pcol = ray_table_get_col(parted_tbl, unique_agg[j]);
                if (!pcol || !RAY_IS_PARTED(pcol->type)) {
                    ray_release(sub); goto batch_fail;
                }
                ray_t* seg = ((ray_t**)ray_data(pcol))[p];
                if (!seg) { ray_release(sub); goto batch_fail; }
                ray_retain(seg);
                sub = ray_table_add_col(sub, unique_agg[j], seg);
                ray_release(seg);
            }

            ray_graph_t* pg = ray_graph_new(sub);
            if (!pg) { ray_release(sub); goto batch_fail; }

            ray_op_t* pkeys[8];
            for (uint8_t k = 0; k < n_part_keys; k++) {
                ray_t* sym_atom = ray_sym_str(pk_syms[k]);
                pkeys[k] = ray_scan(pg, ray_str_ptr(sym_atom));
            }
            ray_op_t* pagg_ins[24];
            for (uint8_t a = 0; a < part_n_aggs; a++) {
                ray_t* sym_atom = ray_sym_str(part_agg_syms[a]);
                pagg_ins[a] = ray_scan(pg, ray_str_ptr(sym_atom));
            }
            for (uint8_t j = 0; j < n_std; j++) {
                uint8_t sq = std_sq_slot[j];
                ray_op_t* x = pagg_ins[sq];
                pagg_ins[sq] = ray_mul(pg, x, x);
            }

            ray_op_t* proot = ray_group(pg, pkeys, n_part_keys,
                                       part_ops, pagg_ins, part_n_aggs);
            proot = ray_optimize(pg, proot);
            bp[bi] = ray_execute(pg, proot);
            ray_graph_free(pg);
            ray_release(sub);

            if (!bp[bi] || RAY_IS_ERR(bp[bi])) goto batch_fail;

            /* Capture agg column name IDs once (all partials share names) */
            if (!agg_names_captured) {
                for (uint8_t a = 0; a < part_n_aggs; a++)
                    agg_name_ids[a] = ray_table_col_name(
                        bp[bi], (int64_t)n_part_keys + a);
                agg_names_captured = 1;
            }
        }

        /* Phase 2: concat (running + batch_partials + MAPCOMMON) */
        int64_t mrows = running ? ray_table_nrows(running) : 0;
        for (int32_t i = 0; i < batch_n; i++)
            mrows += ray_table_nrows(bp[i]);

        if (merge_tbl) { ray_release(merge_tbl); merge_tbl = NULL; }
        merge_tbl = ray_table_new((int64_t)(n_keys + part_n_aggs));
        if (!merge_tbl || RAY_IS_ERR(merge_tbl)) {
            merge_tbl = NULL; goto batch_fail;
        }

        /* Key columns */
        for (uint8_t k = 0; k < n_keys; k++) {
            int is_mc = 0;
            for (uint8_t m = 0; m < n_mc_keys; m++)
                if (mc_sym_ids[m] == key_syms[k]) { is_mc = 1; break; }

            /* Type reference for column allocation */
            ray_t* tref = NULL;
            if (running) {
                tref = ray_table_get_col(running, key_syms[k]);
            } else if (is_mc) {
                ray_t* mc_col = ray_table_get_col(parted_tbl, key_syms[k]);
                tref = ((ray_t**)ray_data(mc_col))[0];
            } else {
                tref = ray_table_get_col(bp[0], key_syms[k]);
            }
            if (!tref) goto batch_fail;

            size_t esz = (size_t)col_esz(tref);
            ray_t* flat = col_vec_new(tref, mrows);
            if (!flat || RAY_IS_ERR(flat)) goto batch_fail;
            flat->len = mrows;
            char* out = (char*)ray_data(flat);
            int64_t off = 0;

            /* Copy from running result */
            if (running) {
                ray_t* rc = ray_table_get_col(running, key_syms[k]);
                if (rc && rc->len > 0) {
                    memcpy(out, ray_data(rc), (size_t)rc->len * esz);
                    off = rc->len;
                }
            }

            /* Copy from batch partials */
            for (int32_t i = 0; i < batch_n; i++) {
                int64_t pnrows = ray_table_nrows(bp[i]);
                if (is_mc) {
                    /* MAPCOMMON: replicate this partition's key value */
                    int32_t p = batch_start + i;
                    ray_t* mc_col = ray_table_get_col(parted_tbl, key_syms[k]);
                    ray_t* mc_kv = ((ray_t**)ray_data(mc_col))[0];
                    const char* kdata = (const char*)ray_data(mc_kv);
                    for (int64_t r = 0; r < pnrows; r++)
                        memcpy(out + (size_t)(off + r) * esz,
                               kdata + (size_t)p * esz, esz);
                    off += pnrows;
                } else {
                    ray_t* pc = ray_table_get_col(bp[i], key_syms[k]);
                    if (pc && pc->len > 0) {
                        memcpy(out + (size_t)off * esz,
                               ray_data(pc), (size_t)pc->len * esz);
                        off += pc->len;
                    }
                }
            }

            merge_tbl = ray_table_add_col(merge_tbl, key_syms[k], flat);
            ray_release(flat);
        }

        /* Agg columns */
        for (uint8_t a = 0; a < part_n_aggs; a++) {
            ray_t* tref = running
                ? ray_table_get_col_idx(running, (int64_t)n_keys + a)
                : ray_table_get_col_idx(bp[0], (int64_t)n_part_keys + a);
            if (!tref) goto batch_fail;

            size_t esz = (size_t)col_esz(tref);
            ray_t* flat = col_vec_new(tref, mrows);
            if (!flat || RAY_IS_ERR(flat)) goto batch_fail;
            flat->len = mrows;
            char* out = (char*)ray_data(flat);
            int64_t off = 0;

            if (running) {
                ray_t* rc = ray_table_get_col_idx(running, (int64_t)n_keys + a);
                if (rc && rc->len > 0) {
                    memcpy(out, ray_data(rc), (size_t)rc->len * esz);
                    off = rc->len;
                }
            }

            for (int32_t i = 0; i < batch_n; i++) {
                ray_t* pc = ray_table_get_col_idx(bp[i],
                                                 (int64_t)n_part_keys + a);
                if (pc && pc->len > 0) {
                    memcpy(out + (size_t)off * esz,
                           ray_data(pc), (size_t)pc->len * esz);
                    off += pc->len;
                }
            }

            merge_tbl = ray_table_add_col(merge_tbl, agg_name_ids[a], flat);
            ray_release(flat);
        }

        /* Free batch partials */
        for (int32_t i = 0; i < batch_n; i++) {
            ray_release(bp[i]);
            bp[i] = NULL;
        }

        /* Phase 3: merge GROUP BY */
        ray_graph_t* mg = ray_graph_new(merge_tbl);
        if (!mg) goto batch_fail;

        ray_op_t* mkeys[8];
        for (uint8_t k = 0; k < n_keys; k++) {
            ray_t* sym_atom = ray_sym_str(key_syms[k]);
            mkeys[k] = ray_scan(mg, ray_str_ptr(sym_atom));
        }

        ray_op_t* magg_ins[24];
        for (uint8_t a = 0; a < part_n_aggs; a++) {
            ray_t* agg_name = ray_sym_str(agg_name_ids[a]);
            magg_ins[a] = ray_scan(mg, ray_str_ptr(agg_name));
        }

        ray_op_t* mroot = ray_group(mg, mkeys, n_keys,
                                   merge_ops, magg_ins, part_n_aggs);
        mroot = ray_optimize(mg, mroot);
        ray_t* new_running = ray_execute(mg, mroot);
        ray_graph_free(mg);

        if (running) ray_release(running);
        running = new_running;

        if (!running || RAY_IS_ERR(running)) {
            ray_release(merge_tbl);
            return NULL;
        }

        /* Rename running's agg columns back to the original partial names.
         * Without this, each merge adds an extra suffix (e.g. v1_sum → v1_sum_sum). */
        for (uint8_t a = 0; a < part_n_aggs; a++)
            ray_table_set_col_name(running, (int64_t)n_keys + a, agg_name_ids[a]);

        continue;

batch_fail:
        for (int32_t i = 0; i < batch_n; i++)
            if (bp[i]) ray_release(bp[i]);
        if (running) ray_release(running);
        if (merge_tbl) ray_release(merge_tbl);
        return NULL;
    }

    ray_t* result = running;

    if (!result || RAY_IS_ERR(result)) {
        if (merge_tbl) ray_release(merge_tbl);
        return NULL;
    }

    int64_t rncols = ray_table_ncols(result);

    /* AVG/STDDEV post-processing: build trimmed table (n_keys + n_aggs cols),
     * computing final AVG = SUM/COUNT and STDDEV/VAR from SUM, SUM_SQ, COUNT. */
    if (has_avg || has_stddev) {
        ray_t* trimmed = ray_table_new((int64_t)(n_keys + n_aggs));
        if (!trimmed || RAY_IS_ERR(trimmed)) {
            ray_release(result);
            if (merge_tbl) ray_release(merge_tbl);
            return NULL;
        }

        for (int64_t c = 0; c < (int64_t)(n_keys + n_aggs) && c < rncols; c++) {
            int64_t nm = ray_table_col_name(result, c);

            /* Check if this agg column is an AVG or STDDEV/VAR slot */
            int is_avg_slot = 0, is_std_slot = 0;
            uint8_t avg_i = 0, std_i = 0;
            if (c >= n_keys) {
                uint8_t a = (uint8_t)(c - n_keys);
                for (uint8_t j = 0; j < n_avg; j++) {
                    if (avg_idx[j] == a) { is_avg_slot = 1; avg_i = j; break; }
                }
                for (uint8_t j = 0; j < n_std; j++) {
                    if (std_idx[j] == a) { is_std_slot = 1; std_i = j; break; }
                }
            }

            if (is_avg_slot) {
                /* AVG = SUM(x) / COUNT(x) */
                int64_t sum_ci = c;
                /* AVG COUNT slots: after n_aggs + n_std SUM_SQ slots */
                int64_t cnt_ci = (int64_t)n_keys + n_aggs + n_std + avg_i;
                ray_t* sum_col = ray_table_get_col_idx(result, sum_ci);
                ray_t* cnt_col = (cnt_ci < rncols) ? ray_table_get_col_idx(result, cnt_ci) : NULL;
                if (!sum_col || !cnt_col) {
                    if (sum_col) {
                        ray_retain(sum_col);
                        trimmed = ray_table_add_col(trimmed, nm, sum_col);
                        ray_release(sum_col);
                    }
                    continue;
                }

                int64_t nrows = sum_col->len;
                ray_t* avg_col = ray_vec_new(RAY_F64, nrows);
                if (!avg_col || RAY_IS_ERR(avg_col)) {
                    ray_release(trimmed); ray_release(result);
                    if (merge_tbl) ray_release(merge_tbl);
                    return NULL;
                }
                avg_col->len = nrows;

                double* out = (double*)ray_data(avg_col);
                if (sum_col->type == RAY_F64) {
                    const double* sv = (const double*)ray_data(sum_col);
                    const int64_t* cv = (const int64_t*)ray_data(cnt_col);
                    for (int64_t r = 0; r < nrows; r++)
                        out[r] = cv[r] > 0 ? sv[r] / (double)cv[r] : 0.0;
                } else {
                    const int64_t* sv = (const int64_t*)ray_data(sum_col);
                    const int64_t* cv = (const int64_t*)ray_data(cnt_col);
                    for (int64_t r = 0; r < nrows; r++)
                        out[r] = cv[r] > 0 ? (double)sv[r] / (double)cv[r] : 0.0;
                }
                trimmed = ray_table_add_col(trimmed, nm, avg_col);
                ray_release(avg_col);
            } else if (is_std_slot) {
                /* STDDEV/VAR from merged SUM(x), SUM(x²), COUNT(x):
                 * var_pop = SUM_SQ/N - (SUM/N)²
                 * var_samp = var_pop * N/(N-1)
                 * stddev_pop = sqrt(var_pop), stddev_samp = sqrt(var_samp) */
                int64_t sum_ci = c;
                int64_t sq_ci  = (int64_t)n_keys + std_sq_slot[std_i];
                int64_t cnt_ci = (int64_t)n_keys + std_cnt_slot[std_i];
                ray_t* sum_col = ray_table_get_col_idx(result, sum_ci);
                ray_t* sq_col  = (sq_ci < rncols) ? ray_table_get_col_idx(result, sq_ci) : NULL;
                ray_t* cnt_col = (cnt_ci < rncols) ? ray_table_get_col_idx(result, cnt_ci) : NULL;
                if (!sum_col || !sq_col || !cnt_col) {
                    if (sum_col) {
                        ray_retain(sum_col);
                        trimmed = ray_table_add_col(trimmed, nm, sum_col);
                        ray_release(sum_col);
                    }
                    continue;
                }

                int64_t nrows = sum_col->len;
                ray_t* out_col = ray_vec_new(RAY_F64, nrows);
                if (!out_col || RAY_IS_ERR(out_col)) {
                    ray_release(trimmed); ray_release(result);
                    if (merge_tbl) ray_release(merge_tbl);
                    return NULL;
                }
                out_col->len = nrows;
                double* out = (double*)ray_data(out_col);

                uint16_t orig_op = std_orig_op[std_i];
                /* SUM(x) is always F64 after merge (SUM produces F64 for F64 input,
                 * I64 for integer input; SUM(x²) via ray_mul always produces F64). */
                const double* sq = (const double*)ray_data(sq_col);
                const int64_t* cv = (const int64_t*)ray_data(cnt_col);
                if (sum_col->type == RAY_F64) {
                    const double* sv = (const double*)ray_data(sum_col);
                    for (int64_t r = 0; r < nrows; r++) {
                        double n = (double)cv[r];
                        if (n <= 0) { out[r] = 0.0; ray_vec_set_null(out_col, r, true); continue; }
                        double mean = sv[r] / n;
                        double var_pop = sq[r] / n - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        bool insuf = (orig_op == OP_VAR || orig_op == OP_STDDEV) && n <= 1;
                        if (insuf) { out[r] = 0.0; ray_vec_set_null(out_col, r, true); continue; }
                        if (orig_op == OP_VAR_POP)         out[r] = var_pop;
                        else if (orig_op == OP_VAR)         out[r] = var_pop * n / (n - 1);
                        else if (orig_op == OP_STDDEV_POP)  out[r] = sqrt(var_pop);
                        else /* OP_STDDEV */                out[r] = sqrt(var_pop * n / (n - 1));
                    }
                } else {
                    const int64_t* sv = (const int64_t*)ray_data(sum_col);
                    for (int64_t r = 0; r < nrows; r++) {
                        double n = (double)cv[r];
                        if (n <= 0) { out[r] = 0.0; ray_vec_set_null(out_col, r, true); continue; }
                        double mean = (double)sv[r] / n;
                        double var_pop = sq[r] / n - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        bool insuf = (orig_op == OP_VAR || orig_op == OP_STDDEV) && n <= 1;
                        if (insuf) { out[r] = 0.0; ray_vec_set_null(out_col, r, true); continue; }
                        if (orig_op == OP_VAR_POP)         out[r] = var_pop;
                        else if (orig_op == OP_VAR)         out[r] = var_pop * n / (n - 1);
                        else if (orig_op == OP_STDDEV_POP)  out[r] = sqrt(var_pop);
                        else /* OP_STDDEV */                out[r] = sqrt(var_pop * n / (n - 1));
                    }
                }
                trimmed = ray_table_add_col(trimmed, nm, out_col);
                ray_release(out_col);
            } else {
                ray_t* col = ray_table_get_col_idx(result, c);
                if (col) {
                    ray_retain(col);
                    trimmed = ray_table_add_col(trimmed, nm, col);
                    ray_release(col);
                }
            }
        }
        ray_release(result);
        result = trimmed;
        rncols = ray_table_ncols(result);
    }

    /* Agg column names already fixed by ray_table_set_col_name inside batch loop.
     * Apply final name fixup for the user-facing n_aggs columns (trim decomposed extras). */
    for (uint8_t a = 0; a < n_aggs && (int64_t)(n_keys + a) < rncols; a++)
        ray_table_set_col_name(result, (int64_t)n_keys + a, agg_name_ids[a]);

    if (merge_tbl) ray_release(merge_tbl);
    return result;
}

/* ══════════════════════════════════════════════════════════════════════
 * pivot_ingest_run — shared parallel hash-aggregate for pivot
 *
 * Mirrors the phase1+phase2 radix pipeline exec_group uses, leaving
 * the result in per-partition HTs with prefix offsets so the caller
 * can iterate grouped rows without knowing about the radix internals.
 * Falls back to a single sequential HT for tiny inputs or when no
 * pool is available — the caller iterates n_parts ∈ {1, RADIX_P}.
 * ══════════════════════════════════════════════════════════════════════ */

static void pivot_ingest_sequential(pivot_ingest_t* out, const ght_layout_t* ly,
                                     void** key_data, int8_t* key_types,
                                     uint8_t* key_attrs, ray_t** key_vecs,
                                     ray_t** agg_vecs, int64_t n_scan,
                                     group_ht_t* scratch_ht) {
    (void)key_data;
    out->part_hts = scratch_ht;
    out->n_parts = 1;
    out->row_stride = ly->row_stride;
    group_rows_range(scratch_ht, key_data, key_types, key_attrs, key_vecs,
                     agg_vecs, NULL, NULL, NULL, 0, n_scan, NULL);
    out->total_grps = scratch_ht->grp_count;
    out->part_offsets[0] = 0;
    out->part_offsets[1] = scratch_ht->grp_count;
    out->part_hts = scratch_ht;
}

bool pivot_ingest_run(pivot_ingest_t* out,
                      const ght_layout_t* ly,
                      void** key_data, int8_t* key_types, uint8_t* key_attrs,
                      ray_t** key_vecs, ray_t** agg_vecs,
                      int64_t n_scan) {
    memset(out, 0, sizeof(*out));
    out->row_stride = ly->row_stride;

    /* Allocate a small offsets buffer up front (RADIX_P+1 is the max). */
    out->part_offsets = (uint32_t*)scratch_alloc(&out->_offsets_hdr,
        (size_t)(RADIX_P + 1) * sizeof(uint32_t));
    if (!out->part_offsets) return false;

    uint8_t n_keys = ly->n_keys;

    ray_pool_t* pool = ray_pool_get();
    uint32_t n_total = pool ? ray_pool_total_workers(pool) : 1;
    bool parallel_ok = (pool && n_scan >= RAY_PARALLEL_THRESHOLD && n_total > 1);

    if (!parallel_ok) {
        /* Sequential single-HT path — allocate the HT in its own scratch
         * block and wire part_hts/n_parts immediately so every failure
         * below funnels through pivot_ingest_free for cleanup. */
        group_ht_t* seq = (group_ht_t*)scratch_calloc(&out->_part_hts_hdr,
            sizeof(group_ht_t));
        if (!seq) return false;
        out->part_hts = seq;
        out->n_parts = 1;
        uint32_t seq_cap = 1024;
        uint64_t target = (uint64_t)n_scan * 2;
        while ((uint64_t)seq_cap < target && seq_cap < (1u << 24)) seq_cap <<= 1;
        if (!group_ht_init(seq, seq_cap, ly)) return false;
        pivot_ingest_sequential(out, ly, key_data, key_types, key_attrs,
                                key_vecs, agg_vecs, n_scan, seq);
        /* Surface grow-path OOM from group_probe_entry so callers don't
         * silently see a truncated result. */
        if (seq->oom) return false;
        return true;
    }

    /* ═════ Parallel radix path ═════ */
    size_t n_bufs = (size_t)n_total * RADIX_P;
    out->_n_bufs = n_bufs;
    radix_buf_t* radix_bufs = (radix_buf_t*)scratch_calloc(&out->_radix_bufs_hdr,
        n_bufs * sizeof(radix_buf_t));
    if (!radix_bufs) return false;
    out->_radix_bufs = radix_bufs;

    uint32_t buf_init = (uint32_t)((uint64_t)n_scan / (RADIX_P * n_total));
    if (buf_init < 64) buf_init = 64;
    buf_init = buf_init + buf_init / 2;
    uint16_t estride = ly->entry_stride;
    {
        size_t total_pre = (size_t)n_bufs * buf_init * estride;
        if (total_pre > (size_t)2 << 30) {
            buf_init = (uint32_t)(((size_t)2 << 30) / ((size_t)n_bufs * estride));
            if (buf_init < 64) buf_init = 64;
        }
    }
    for (size_t i = 0; i < n_bufs; i++) {
        radix_bufs[i].data = (char*)scratch_alloc(&radix_bufs[i]._hdr,
            (size_t)buf_init * estride);
        radix_bufs[i].count = 0;
        radix_bufs[i].cap = buf_init;
    }

    uint8_t p1_nullable = 0;
    for (uint8_t k = 0; k < n_keys; k++) {
        if (!key_vecs[k]) continue;
        ray_t* src = (key_vecs[k]->attrs & RAY_ATTR_SLICE)
                     ? key_vecs[k]->slice_parent : key_vecs[k];
        if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
            p1_nullable |= (uint8_t)(1u << k);
    }

    radix_phase1_ctx_t p1ctx = {
        .key_data      = key_data,
        .key_types     = key_types,
        .key_attrs     = key_attrs,
        .key_vecs      = key_vecs,
        .nullable_mask = p1_nullable,
        .agg_vecs      = agg_vecs,
        .agg_vecs2     = NULL,   /* this scratch path doesn't use binary aggs */
        .n_workers     = n_total,
        .bufs          = radix_bufs,
        .layout        = *ly,
        .match_idx     = NULL,
    };
    ray_pool_dispatch(pool, radix_phase1_fn, &p1ctx, n_scan);
    if (ray_interrupted()) return true; /* caller checks ray_interrupted() */
    /* Sync point — phase1 drained all rows, so rows_done == n_scan. */
    ray_progress_update(NULL, "hash-partition", (uint64_t)n_scan, (uint64_t)n_scan);

    for (size_t i = 0; i < n_bufs; i++)
        if (radix_bufs[i].oom) return false;

    group_ht_t* part_hts = (group_ht_t*)scratch_calloc(&out->_part_hts_hdr,
        RADIX_P * sizeof(group_ht_t));
    if (!part_hts) return false;

    radix_phase2_ctx_t p2ctx = {
        .key_types = key_types,
        .n_keys    = n_keys,
        .n_workers = n_total,
        .bufs      = radix_bufs,
        .part_hts  = part_hts,
        .layout    = *ly,
        .key_data  = key_data,
    };
    ray_pool_dispatch_n(pool, radix_phase2_fn, &p2ctx, RADIX_P);
    out->part_hts = part_hts;
    out->n_parts = RADIX_P;
    if (ray_interrupted()) return true;
    /* Sync point — partitions materialized; show RADIX_P/RADIX_P. */
    ray_progress_update(NULL, "per-partition aggregate", RADIX_P, RADIX_P);

    /* OOM detection for the parallel path. Two distinct failure modes
     * must be caught here so callers never see a silently-truncated
     * result:
     *   (a) phase2 init failed — radix_phase2_fn `continue`s when
     *       group_ht_init_sized returns false, leaving the partition
     *       HT with NULL rows despite a non-zero buffer count. Every
     *       entry routed into that partition would be dropped.
     *   (b) grow-path OOM — group_probe_entry sets part_hts[p].oom
     *       on scratch_realloc failure and returns without inserting
     *       the key, silently truncating later groups. */
    for (uint32_t p = 0; p < RADIX_P; p++) {
        if (part_hts[p].oom) return false;
        if (part_hts[p].rows) continue;
        uint32_t pcount = 0;
        for (uint32_t w = 0; w < n_total; w++)
            pcount += radix_bufs[(size_t)w * RADIX_P + p].count;
        if (pcount) return false;
    }

    out->part_offsets[0] = 0;
    for (uint32_t p = 0; p < RADIX_P; p++)
        out->part_offsets[p + 1] = out->part_offsets[p] + part_hts[p].grp_count;
    out->total_grps = out->part_offsets[RADIX_P];
    return true;
}

void pivot_ingest_free(pivot_ingest_t* out) {
    if (!out) return;
    if (out->part_hts) {
        for (uint32_t p = 0; p < out->n_parts; p++) {
            if (out->part_hts[p].rows || out->part_hts[p].slots)
                group_ht_free(&out->part_hts[p]);
        }
        scratch_free(out->_part_hts_hdr);
    }
    if (out->_radix_bufs) {
        radix_buf_t* bufs = (radix_buf_t*)out->_radix_bufs;
        for (size_t i = 0; i < out->_n_bufs; i++) scratch_free(bufs[i]._hdr);
        scratch_free(out->_radix_bufs_hdr);
    }
    scratch_free(out->_offsets_hdr);
    memset(out, 0, sizeof(*out));
}
