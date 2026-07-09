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
#include "ops/rowsel.h"
#include "ops/hll.h"        /* approximate count-distinct via HyperLogLog */
#include "lang/internal.h"  /* for ray_median_dbl_inplace */
#include "lang/format.h"    /* ray_type_name */
#include "table/domain.h"   /* sym-domain resolution (ray_sym_domain_count) */
#include "ops/agg_engine.h" /* v2 agg engine routing gate (ray_agg_engine_v2) */
#include "vec/str.h"        /* ray_str_t SSO hash/eq for wide STR group keys */
#include "ops/idxop.h"      /* RAY_IDX_DICT: group on persisted string codes */
#include "core/runtime.h"   /* __VM — per-thread group-key cardinality hint */

#include <stdlib.h>

/* Monotonic count of exec_group_per_partition entries (the streaming parted
 * GROUP kernel).  Bumped ONCE at the kernel entry — O(1) per query, never
 * per-row, so it does not violate the "instrumentation never costs O(data)"
 * rule.  Counts every entry regardless of success/cancel/NULL-return.  Atomic
 * because group exec may run on worker threads.  Surfaced via
 * ray_group_perpart_runs() and (.sys.mem)'s "group-perpart-runs" entry. */
static _Atomic(int64_t) ray_group_perpart_runs_ctr = 0;

int64_t ray_group_perpart_runs(void) {
    return atomic_load_explicit(&ray_group_perpart_runs_ctr,
                                memory_order_relaxed);
}

/* ============================================================================
 * Reduction execution
 * ============================================================================ */

typedef struct {
    double sum_f, min_f, max_f, prod_f, first_f, last_f, sum_sq_f;
    int64_t sum_i, min_i, max_i, prod_i, first_i, last_i, sum_sq_i;
    /* Parallel f64 sum of the integer stream — used by AVG so the
     * mean of an i64 column whose sum exceeds 2^63 stays accurate
     * instead of being whatever (uint64) wrap left in sum_i. */
    double sum_d;
    int64_t cnt;
    int64_t null_count;
    int64_t zero_count;
    bool has_first;
} reduce_acc_t;

static void reduce_acc_init(reduce_acc_t* acc) {
    acc->sum_f = 0; acc->min_f = DBL_MAX; acc->max_f = -DBL_MAX;
    acc->prod_f = 1.0; acc->first_f = 0; acc->last_f = 0; acc->sum_sq_f = 0;
    acc->sum_i = 0; acc->min_i = INT64_MAX; acc->max_i = INT64_MIN;
    acc->prod_i = 1; acc->first_i = 0; acc->last_i = 0; acc->sum_sq_i = 0;
    acc->sum_d = 0;
    acc->cnt = 0; acc->null_count = 0; acc->zero_count = 0; acc->has_first = false;
}

/* Lexicographic SYM compare — resolves both sym_ids to strings and
 * memcmps.  Used by SYM MIN/MAX so the result is consistent with
 * asc/desc (sort.c uses build_enum_rank for the same lex semantic).
 * Sym-id comparison would expose intern-order which is a global session
 * state — not a stable, user-visible ordering.
 *
 * Both ids are CELL-DATA of one SYM column, so they resolve through
 * that COLUMN's domain (sym-domain Phase 2) — pass
 * ray_sym_vec_domain(col).  Exact no-op while the domain is the runtime
 * singleton (ray_sym_domain_str delegates to ray_sym_str). */
static inline bool sym_lex_lt(struct ray_sym_domain_s* dom,
                              int64_t a, int64_t b) {
    if (a == b) return false;
    ray_t* sa = ray_sym_domain_str(dom, a);
    ray_t* sb = ray_sym_domain_str(dom, b);
    if (!sa || !sb) return a < b;
    const char* pa = ray_str_ptr(sa);
    const char* pb = ray_str_ptr(sb);
    size_t la = ray_str_len(sa);
    size_t lb = ray_str_len(sb);
    size_t m = la < lb ? la : lb;
    int c = memcmp(pa, pb, m);
    if (c != 0) return c < 0;
    return la < lb;
}
static inline bool sym_lex_gt(struct ray_sym_domain_s* dom,
                              int64_t a, int64_t b) {
    return sym_lex_lt(dom, b, a);
}

/* ── Wide-element (STR/GUID) min/max/first/last ──────────────────────────
 * STR (a 16-byte ray_str_t: pool pointer + length) and GUID (16 raw bytes)
 * do not fit the 8-byte integer reduce accumulators, so the int64 fast paths
 * silently truncate them to a single byte.  These helpers instead track the
 * WINNING ROW INDEX — by content comparison (lexicographic for STR, byte
 * order for GUID) for min/max, or by position for first/last — and the caller
 * materialises that element with collection_elem.  This matches the scalar
 * first/last builtins and gives min/max real lexical results, unifying the
 * scalar and DAG aggregation paths for wide element types. */
static inline bool agg_is_wide_type(int8_t t) {
    return t == RAY_STR || t == RAY_GUID;
}

/* Group key/agg output columns are filled by copying ray_str_t descriptors
 * (by source row index).  Share the source column's string pool so pooled
 * (>12 B) descriptors resolve against it (inline ≤12 B are self-contained). */
static inline void out_col_adopt_str_pool(ray_t* dst, const ray_t* src) {
    if (!dst || RAY_IS_ERR(dst) || dst->type != RAY_STR || !src) return;
    const ray_t* owner = (src->attrs & RAY_ATTR_SLICE) ? src->slice_parent : src;
    if (owner && owner->str_pool && !RAY_IS_ERR(owner->str_pool)) {
        ray_retain(owner->str_pool);
        dst->str_pool = owner->str_pool;
    }
}
/* Scan rows [optionally via sel] and return the winning row index for
 * op (OP_MIN/OP_MAX/OP_FIRST/OP_LAST), or -1 if every scanned row is null.
 *
 * The element type (STR vs GUID) and the operator are resolved ONCE here,
 * before any loop — the inner loops carry no type/op switch.  first/last are
 * pure positional scans (no value comparison); min/max run a single
 * type-specialised compare loop whose direction (want_min) is hoisted out.
 * (sel/has_nulls remain a predictable per-row branch, exactly as the integer
 * reduce loops below do; the goal is no *type/op dispatch* inside the loop.) */
static int64_t wide_winner_row(ray_t* input, uint16_t op,
                               const int64_t* sel, int64_t scan_n,
                               bool has_nulls) {
    /* first/last: positional — return the first/last non-null row, no compare. */
    if (op == OP_FIRST) {
        for (int64_t i = 0; i < scan_n; i++) {
            int64_t row = sel ? sel[i] : i;
            if (!has_nulls || !ray_vec_is_null(input, row)) return row;
        }
        return -1;
    }
    if (op == OP_LAST) {
        for (int64_t i = scan_n - 1; i >= 0; i--) {
            int64_t row = sel ? sel[i] : i;
            if (!has_nulls || !ray_vec_is_null(input, row)) return row;
        }
        return -1;
    }
    /* min/max: one type-specialised compare loop, direction hoisted out. */
    const bool want_min = (op == OP_MIN);
    int64_t best = -1;
    if (input->type == RAY_GUID) {
        const uint8_t* d = (const uint8_t*)ray_data(input);
        for (int64_t i = 0; i < scan_n; i++) {
            int64_t row = sel ? sel[i] : i;
            if (has_nulls && ray_vec_is_null(input, row)) continue;
            if (best < 0) { best = row; continue; }
            int c = memcmp(d + (size_t)row * 16, d + (size_t)best * 16, 16);
            if (want_min ? (c < 0) : (c > 0)) best = row;
        }
    } else {  /* RAY_STR — lexicographic over the pooled bytes */
        for (int64_t i = 0; i < scan_n; i++) {
            int64_t row = sel ? sel[i] : i;
            if (has_nulls && ray_vec_is_null(input, row)) continue;
            if (best < 0) { best = row; continue; }
            size_t la = 0, lb = 0;
            const char* sa = ray_str_vec_get(input, row,  &la);
            const char* sb = ray_str_vec_get(input, best, &lb);
            size_t m = la < lb ? la : lb;
            int c = (m && sa && sb) ? memcmp(sa, sb, m) : 0;
            if (!c) c = (la > lb) - (la < lb);
            if (want_min ? (c < 0) : (c > 0)) best = row;
        }
    }
    return best;
}
/* Whole-table (optionally selected) min/max/first/last over a wide column. */
static ray_t* agg_wide_reduce(ray_t* input, uint16_t op,
                              const int64_t* sel, int64_t scan_n,
                              bool has_nulls) {
    int64_t best = wide_winner_row(input, op, sel, scan_n, has_nulls);
    if (best < 0) return ray_typed_null(-input->type);
    int alloc;
    return collection_elem(input, best, &alloc);
}

/* Integer reduction loop — reads native type T, accumulates as i64.
 * HAS_NULLS and HAS_IDX must be integer literal constants (0 or 1) so the
 * compiler dead-code-eliminates the corresponding branches in every
 * specialisation.  reduce_range dispatches to the right combination
 * before calling this macro so the hot path (no nulls, no idx) contains
 * zero per-element runtime branches.
 *
 * NULL_SENT is the type-correct NULL_* sentinel value for T (NULL_I16,
 * NULL_I32, NULL_I64).  For BOOL/U8 the sentinel slot is unused
 * (those types are non-nullable; dispatcher pins HAS_NULLS=0) so any
 * value works; we pass 0 for compileability. */
#define REDUCE_LOOP_I(T, NULL_SENT, base, start, end, acc, HAS_NULLS, HAS_IDX, idx) \
    do { \
        const T* d = (const T*)(base); \
        for (int64_t i = start; i < end; i++) { \
            int64_t row = (HAS_IDX) ? (idx)[i] : i; \
            T raw = d[row]; \
            if ((HAS_NULLS) && raw == (T)(NULL_SENT)) { (acc)->null_count++; continue; } \
            int64_t v = (int64_t)raw; \
            /* sum/sum_sq may overflow on signed arithmetic — use defined \
             * unsigned wrap (same semantic, no UBSan whine). */ \
            (acc)->sum_i    = (int64_t)((uint64_t)(acc)->sum_i    + (uint64_t)v); \
            (acc)->sum_sq_i = (int64_t)((uint64_t)(acc)->sum_sq_i + (uint64_t)v * (uint64_t)v); \
            (acc)->prod_i   = (int64_t)((uint64_t)(acc)->prod_i   * (uint64_t)v); \
            (acc)->sum_d   += (double)v; \
            if (v == 0) (acc)->zero_count++; \
            if (v < (acc)->min_i) (acc)->min_i = v; \
            if (v > (acc)->max_i) (acc)->max_i = v; \
            if (!(acc)->has_first) { (acc)->first_i = v; (acc)->has_first = true; } \
            (acc)->last_i = v; (acc)->cnt++; \
        } \
    } while (0)

/* Float reduction loop — see REDUCE_LOOP_I for HAS_NULLS/HAS_IDX semantics.
 * F64 null = NaN (NULL_F64); detect via v != v (only NaN fails self-equality). */
#define REDUCE_LOOP_F(base, start, end, acc, HAS_NULLS, HAS_IDX, idx) \
    do { \
        const double* d = (const double*)(base); \
        for (int64_t i = start; i < end; i++) { \
            int64_t row = (HAS_IDX) ? (idx)[i] : i; \
            double v = d[row]; \
            if ((HAS_NULLS) && v != v) { (acc)->null_count++; continue; } \
            (acc)->sum_f += v; (acc)->sum_sq_f += v * v; (acc)->prod_f *= v; \
            if (v == 0.0) (acc)->zero_count++; \
            if (v < (acc)->min_f) (acc)->min_f = v; \
            if (v > (acc)->max_f) (acc)->max_f = v; \
            if (!(acc)->has_first) { (acc)->first_f = v; (acc)->has_first = true; } \
            (acc)->last_f = v; (acc)->cnt++; \
        } \
    } while (0)

/* Dispatch helper: expand REDUCE_LOOP_I/F with compile-time 0/1 constants for
 * HAS_NULLS and HAS_IDX based on the runtime pointers so the compiler can
 * dead-code-eliminate the branches inside each specialisation. */
#define DISPATCH_I(T, NULL_SENT, base, start, end, acc, has_nulls, idx) \
    do { \
        if (!(has_nulls) && !(idx)) \
            REDUCE_LOOP_I(T, NULL_SENT, base, start, end, acc, 0, 0, idx); \
        else if (!(has_nulls)) \
            REDUCE_LOOP_I(T, NULL_SENT, base, start, end, acc, 0, 1, idx); \
        else if (!(idx)) \
            REDUCE_LOOP_I(T, NULL_SENT, base, start, end, acc, 1, 0, idx); \
        else \
            REDUCE_LOOP_I(T, NULL_SENT, base, start, end, acc, 1, 1, idx); \
    } while (0)

#define DISPATCH_F(base, start, end, acc, has_nulls, idx) \
    do { \
        if (!(has_nulls) && !(idx)) \
            REDUCE_LOOP_F(base, start, end, acc, 0, 0, idx); \
        else if (!(has_nulls)) \
            REDUCE_LOOP_F(base, start, end, acc, 0, 1, idx); \
        else if (!(idx)) \
            REDUCE_LOOP_F(base, start, end, acc, 1, 0, idx); \
        else \
            REDUCE_LOOP_F(base, start, end, acc, 1, 1, idx); \
    } while (0)

/* Pin the keyless reduction kernel's hot-loop/jump alignment.  This kernel's
 * tight inner reduction loops are alignment-fragile on 32-byte-fetch cores (DSB
 * / JCC-erratum class): as the branch as a whole grows, .o files linked
 * before group.o shift reduce_range's ABSOLUTE address (it is early in the TU,
 * so the shift is cumulative binary-layout, not a group.c edit), which moved
 * its hot I64 loop off its 32-byte boundary and dropped IPC 1.56→1.01 on q03/
 * q02 (+48% cycles, byte-identical instructions) — RCA task-9.  Entry alignment
 * is NOT the lever (the loop's offset from entry is fixed, so its absolute
 * alignment tracks the entry's low bits).  `align-loops=32` re-pins every loop
 * head AND `align-jumps=32` re-pins the branch targets inside the unrolled
 * reduction (the min/max-update return path) — both are needed: loops alone
 * only recovered ~⅔ of the gap (IPC 1.37); adding jump alignment restores full
 * parity.  Now the hot loop's internal alignment is invariant to reduce_range's
 * absolute address, so future binary-layout churn can no longer reshuffle it
 * onto a bad boundary.  aligned(64) keeps the entry cacheline-stable too.
 * Per-function attributes only — no global codegen flag, no -m target hack.
 * GCC-only: clang has no `optimize` function attribute and -Werror promotes
 * the unknown-attribute warning to an error; the pin is performance-only,
 * so clang builds simply go without it (the pre-pin status quo). */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((aligned(64), optimize("align-loops=32","align-jumps=32")))
#endif
static void reduce_range(ray_t* input, int64_t start, int64_t end,
                         reduce_acc_t* acc, bool has_nulls,
                         const int64_t* idx) {
    void* base = ray_data(input);
    switch (input->type) {
    case RAY_BOOL: case RAY_U8: {
        /* BOOL/U8 are non-nullable; has_nulls is always false here,
         * so the per-element null check is dead code in practice. */
        const uint8_t* d = (const uint8_t*)base;
        for (int64_t i = start; i < end; i++) {
            int64_t row = idx ? idx[i] : i;
            if (has_nulls && ray_vec_is_null(input, row)) { acc->null_count++; continue; }
            int64_t v = (int64_t)d[row];
            acc->sum_i    = (int64_t)((uint64_t)acc->sum_i    + (uint64_t)v);
            acc->sum_sq_i = (int64_t)((uint64_t)acc->sum_sq_i + (uint64_t)v * (uint64_t)v);
            acc->prod_i   = (int64_t)((uint64_t)acc->prod_i   * (uint64_t)v);
            acc->sum_d   += (double)v;
            if (v == 0) acc->zero_count++;
            if (v < acc->min_i) acc->min_i = v;
            if (v > acc->max_i) acc->max_i = v;
            if (!acc->has_first) { acc->first_i = v; acc->has_first = true; }
            acc->last_i = v; acc->cnt++;
        }
        break;
    }
    case RAY_I16:
        DISPATCH_I(int16_t, NULL_I16, base, start, end, acc, has_nulls, idx); break;
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        DISPATCH_I(int32_t, NULL_I32, base, start, end, acc, has_nulls, idx); break;
    case RAY_I64: case RAY_TIMESTAMP:
        DISPATCH_I(int64_t, NULL_I64, base, start, end, acc, has_nulls, idx); break;
    case RAY_F64:
        DISPATCH_F(base, start, end, acc, has_nulls, idx); break;
    case RAY_SYM: {
        /* Adaptive-width SYM columns — read_col_i64 produces the i64
         * sym id; id 0 is the canonical null sym (interned empty string
         * reserved at ray_sym_init).  MIN/MAX use sym_lex_lt/gt so the
         * order is by string content (matches asc/desc), not by intern
         * id.  Same 4-way dispatch to eliminate the per-element
         * null/idx branches. */
        struct ray_sym_domain_s* dom = ray_sym_vec_domain(input);
        if (!has_nulls && !idx) {
            for (int64_t i = start; i < end; i++) {
                int64_t v = read_col_i64(base, i, input->type, input->attrs);
                acc->sum_i += v; acc->sum_sq_i += v * v;
                acc->prod_i = (int64_t)((uint64_t)acc->prod_i * (uint64_t)v);
                if (acc->cnt == 0) { acc->min_i = v; acc->max_i = v; }
                else { if (sym_lex_lt(dom, v, acc->min_i)) acc->min_i = v;
                       if (sym_lex_gt(dom, v, acc->max_i)) acc->max_i = v; }
                if (!acc->has_first) { acc->first_i = v; acc->has_first = true; }
                acc->last_i = v; acc->cnt++;
            }
        } else if (!has_nulls) {
            for (int64_t i = start; i < end; i++) {
                int64_t row = idx[i];
                int64_t v = read_col_i64(base, row, input->type, input->attrs);
                acc->sum_i += v; acc->sum_sq_i += v * v;
                acc->prod_i = (int64_t)((uint64_t)acc->prod_i * (uint64_t)v);
                if (acc->cnt == 0) { acc->min_i = v; acc->max_i = v; }
                else { if (sym_lex_lt(dom, v, acc->min_i)) acc->min_i = v;
                       if (sym_lex_gt(dom, v, acc->max_i)) acc->max_i = v; }
                if (!acc->has_first) { acc->first_i = v; acc->has_first = true; }
                acc->last_i = v; acc->cnt++;
            }
        } else if (!idx) {
            for (int64_t i = start; i < end; i++) {
                int64_t v = read_col_i64(base, i, input->type, input->attrs);
                if (v == 0) { acc->null_count++; continue; }
                acc->sum_i += v; acc->sum_sq_i += v * v;
                acc->prod_i = (int64_t)((uint64_t)acc->prod_i * (uint64_t)v);
                if (acc->cnt == 0) { acc->min_i = v; acc->max_i = v; }
                else { if (sym_lex_lt(dom, v, acc->min_i)) acc->min_i = v;
                       if (sym_lex_gt(dom, v, acc->max_i)) acc->max_i = v; }
                if (!acc->has_first) { acc->first_i = v; acc->has_first = true; }
                acc->last_i = v; acc->cnt++;
            }
        } else {
            for (int64_t i = start; i < end; i++) {
                int64_t row = idx[i];
                int64_t v = read_col_i64(base, row, input->type, input->attrs);
                if (v == 0) { acc->null_count++; continue; }
                acc->sum_i += v; acc->sum_sq_i += v * v;
                acc->prod_i = (int64_t)((uint64_t)acc->prod_i * (uint64_t)v);
                if (acc->cnt == 0) { acc->min_i = v; acc->max_i = v; }
                else { if (sym_lex_lt(dom, v, acc->min_i)) acc->min_i = v;
                       if (sym_lex_gt(dom, v, acc->max_i)) acc->max_i = v; }
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
    const int64_t* idx;    /* NULL = no selection; else int64[total_pass] */
} par_reduce_ctx_t;

static void par_reduce_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    par_reduce_ctx_t* c = (par_reduce_ctx_t*)ctx;
    reduce_range(c->input, start, end, &c->accs[worker_id],
                 c->has_nulls, c->idx);
}

static void reduce_merge(reduce_acc_t* dst, const reduce_acc_t* src, int8_t in_type,
                         struct ray_sym_domain_s* sym_dom) {
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
        dst->sum_d   += src->sum_d;
        if (in_type == RAY_SYM) {
            /* Lex compare for SYM min/max (see sym_lex_lt). */
            if (src->cnt > 0) {
                if (dst->cnt == 0) { dst->min_i = src->min_i; dst->max_i = src->max_i; }
                else { if (sym_lex_lt(sym_dom, src->min_i, dst->min_i)) dst->min_i = src->min_i;
                       if (sym_lex_gt(sym_dom, src->max_i, dst->max_i)) dst->max_i = src->max_i; }
            }
        } else {
            if (src->min_i < dst->min_i) dst->min_i = src->min_i;
            if (src->max_i > dst->max_i) dst->max_i = src->max_i;
        }
    }
    dst->cnt += src->cnt;
    dst->null_count += src->null_count;
    dst->zero_count += src->zero_count;
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
            else fv = clear_neg_zero(fv);
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
            else fv = clear_neg_zero(fv);
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
    /* CD_HASH_K1 is the golden-ratio multiplier: it is a Fibonacci hash,
     * whose well-mixed bits live in the HIGH end of the product, so the
     * initial slot must come from the top bits (h >> shift), NOT h & mask.
     * Masking the low bits collapses to a single bucket for keys whose low
     * bits are all zero — e.g. integer-valued doubles, whose IEEE-754 bit
     * patterns are large power-of-two multiples — turning the dedup into an
     * O(n^2) probe storm (a query-reachable DoS on count(distinct f64col)). */
    int cd_shift = 64 - __builtin_ctzll(cap);

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
            else fv = clear_neg_zero(fv);
            memcpy(&val, &fv, sizeof(int64_t));
        } else {
            val = read_col_i64(base, i, in_type, in_attrs);
        }
        uint64_t h = (uint64_t)val * CD_HASH_K1;
        uint64_t slot = h >> cd_shift;   /* Fibonacci hash: use the high bits */
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

static int64_t cd_sym_dense_count(ray_t* input) {
    /* Cell ids are positions in the COLUMN's domain — size seen[] by that
     * domain's count, not the global intern count (a FILE-domain position
     * can exceed the global count → spurious -2 bail; and the global count
     * can dwarf the column's domain → oversized calloc).  Identical value
     * while the domain is the runtime singleton. */
    int64_t dom_n = ray_sym_domain_count(ray_sym_vec_domain(input));
    uint32_t nsyms = (dom_n > 0 && dom_n <= UINT32_MAX) ? (uint32_t)dom_n : 0;
    if (nsyms == 0) return dom_n == 0 ? 0 : -2;

    ray_t* seen_hdr = NULL;
    uint8_t* seen = (uint8_t*)scratch_calloc(&seen_hdr, (size_t)nsyms);
    if (!seen) return -1;

    const void* base = ray_data(input);
    int64_t distinct = 0;
    int64_t len = input->len;
    uint8_t esz = ray_sym_elem_size(input->type, input->attrs);

#define CD_SYM_DENSE_LOOP(T) do {                                      \
        const T* ids = (const T*)base;                                  \
        for (int64_t i = 0; i < len; i++) {                             \
            uint64_t id = (uint64_t)ids[i];                             \
            if (RAY_UNLIKELY(id >= nsyms)) {                            \
                scratch_free(seen_hdr);                                 \
                return -2;                                              \
            }                                                           \
            if (!seen[id]) { seen[id] = 1; distinct++; }                \
        }                                                               \
    } while (0)

    switch (esz) {
    case 1:  CD_SYM_DENSE_LOOP(uint8_t);  break;
    case 2:  CD_SYM_DENSE_LOOP(uint16_t); break;
    case 4:  CD_SYM_DENSE_LOOP(uint32_t); break;
    default: CD_SYM_DENSE_LOOP(uint64_t); break;
    }

#undef CD_SYM_DENSE_LOOP

    scratch_free(seen_hdr);
    return distinct;
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

    /* For inputs above this row count, switch to the HyperLogLog
     * cardinality sketch (~0.8% std error at P=14, 16 KB per shard).
     * Exact dedup-via-hashset is O(unique·log) and becomes memory-
     * bandwidth-bound past ~1 M rows; HLL is single-pass, mergeable,
     * and constant-memory per worker.  Below the threshold the exact
     * path is fast enough and avoids approximation entirely — so small
     * tests still match `len-after-distinct` byte-for-byte. */

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
        return ray_error("type", "count distinct: unsupported column type, got %s", ray_type_name(in_type));
    }

    void* base = ray_data(input);
    ray_pool_t* pool = ray_pool_get();

    if (in_type == RAY_SYM) {
        int64_t cnt = cd_sym_dense_count(input);
        if (cnt >= 0) return ray_i64(cnt);
        if (cnt == -1) return ray_error("oom", NULL);
    }

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

/* Type-correct null check for the column row r.  Mirrors sentinel_is_null
 * but specialised for cdpg's pre-resolved (base, in_type, esz) ctx so the
 * hot loop avoids the ray_t pointer indirection. */
static inline bool cdpg_is_null(const void* base, int64_t r,
                                int8_t in_type, uint8_t esz) {
    (void)esz;  /* width only mattered for the (now removed) SYM arm */
    switch (in_type) {
        case RAY_F64: { double f = ((const double*)base)[r]; return f != f; }
        case RAY_F32: { float  f = ((const float*) base)[r]; return f != f; }
        case RAY_I64: case RAY_TIMESTAMP:
            return ((const int64_t*)base)[r] == NULL_I64;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            return ((const int32_t*)base)[r] == NULL_I32;
        case RAY_I16:
            return ((const int16_t*)base)[r] == NULL_I16;
        /* SYM has no null — the empty sym (id 0) is a value, not null.  This
         * helper is only reached behind a has_nulls guard (= src HAS_NULLS,
         * never set on SYM), so an in_type==RAY_SYM call cannot occur. */
        default:  /* BOOL / U8 — non-nullable */
            return false;
    }
}

/* Read column row r as int64.  Width-typed fast path; F64 bitcasts. */
static inline int64_t cdpg_read(const void* base, int64_t r,
                                int8_t in_type, uint8_t esz) {
    if (in_type == RAY_F64) {
        double fv = ((const double*)base)[r];
        if (fv != fv) fv = (double)NAN;
        else fv = clear_neg_zero(fv);
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
        if (x->has_nulls && cdpg_is_null(x->base, r, x->in_type, esz)) continue;
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
        if (x->has_nulls && cdpg_is_null(x->base, r, x->in_type, esz)) continue;
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
        .p_mask = p_mask,
        .odata = (int64_t*)ray_data(out),
    };

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

/* Approximate per-group count(distinct) via HyperLogLog with sparse
 * representation.  Builds (idx_buf, offsets, counts) from row_gid on the
 * fly and delegates to ray_count_distinct_approx_pg_buf.
 *
 * Memory: each task sketch starts sparse (1 KB) and converts to dense
 * (16 KB) only for groups that exceed RAY_HLL_SPARSE_CAP unique values.
 * Total concurrent memory is bounded by n_workers × 17 KB regardless of
 * n_groups — that's the property that lets us run HLL at n_groups > 50K
 * where the dense-only sketch would have needed multi-GB.
 *
 * Returns the populated `out` vector on success, NULL on type miss /
 * dispatch failure.  Caller (ray_count_distinct_per_group) falls back
 * to the exact partitioned dedup. */
__attribute__((unused)) static ray_t* count_distinct_per_group_hll(ray_t* src, const int64_t* row_gid,
                                           int64_t n_rows, int64_t n_groups,
                                           ray_t* out) {
    if (!src || n_rows <= 0 || n_groups <= 0) return NULL;
    /* Build group-major idx_buf: for each group g, idx_buf[offsets[g] ..
     * offsets[g] + counts[g]) lists the source row indices in that group.
     * Serial two-pass; for n_rows = 10 M this is ~80 MB of int64 reads
     * twice ≈ 25 ms on the bench box.  The HLL pass itself dominates. */
    ray_t* cnt_hdr = NULL;
    ray_t* off_hdr = NULL;
    int64_t* counts  = (int64_t*)scratch_calloc(&cnt_hdr,
                                                 (size_t)n_groups * sizeof(int64_t));
    int64_t* offsets = (int64_t*)scratch_alloc(&off_hdr,
                                                 (size_t)n_groups * sizeof(int64_t));
    if (!counts || !offsets) {
        if (cnt_hdr) scratch_free(cnt_hdr);
        if (off_hdr) scratch_free(off_hdr);
        return NULL;
    }
    /* Pass 1: histogram. */
    int64_t total = 0;
    for (int64_t r = 0; r < n_rows; r++) {
        int64_t g = row_gid[r];
        if (g >= 0 && g < n_groups) counts[g]++;
    }
    /* Prefix sums → offsets. */
    for (int64_t g = 0; g < n_groups; g++) {
        offsets[g] = total;
        total += counts[g];
    }
    if (total == 0) {
        scratch_free(cnt_hdr); scratch_free(off_hdr);
        return out;
    }
    ray_t* idx_hdr = NULL;
    int64_t* idx_buf = (int64_t*)scratch_alloc(&idx_hdr,
                                                 (size_t)total * sizeof(int64_t));
    if (!idx_buf) {
        scratch_free(cnt_hdr); scratch_free(off_hdr);
        return NULL;
    }
    /* Pass 2: scatter into group-major buf using a cursor copy of offsets. */
    ray_t* pos_hdr = NULL;
    int64_t* pos = (int64_t*)scratch_alloc(&pos_hdr,
                                            (size_t)n_groups * sizeof(int64_t));
    if (!pos) {
        scratch_free(idx_hdr); scratch_free(cnt_hdr); scratch_free(off_hdr);
        return NULL;
    }
    memcpy(pos, offsets, (size_t)n_groups * sizeof(int64_t));
    for (int64_t r = 0; r < n_rows; r++) {
        int64_t g = row_gid[r];
        if (g >= 0 && g < n_groups) idx_buf[pos[g]++] = r;
    }
    scratch_free(pos_hdr);

    int64_t* odata = (int64_t*)ray_data(out);
    int rc = ray_count_distinct_approx_pg_buf(src, idx_buf, offsets, counts,
                                              n_groups, RAY_HLL_DEFAULT_P, odata);
    scratch_free(idx_hdr);
    scratch_free(cnt_hdr);
    scratch_free(off_hdr);
    if (rc != 0) return NULL;
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
    if (!src || RAY_IS_ERR(src)) return ray_error("domain", "count distinct per group: invalid source column");
    if (n_groups < 0) return ray_error("domain", "count distinct per group: group count must be non-negative, got %lld", (long long)n_groups);
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
    if (src->len < n_rows) return ray_error("domain", "count distinct per group: source has fewer rows than required, got %lld need %lld", (long long)src->len, (long long)n_rows);

    ray_t* out = ray_vec_new(RAY_I64, n_groups);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n_groups;
    int64_t* odata = (int64_t*)ray_data(out);
    memset(odata, 0, (size_t)n_groups * sizeof(int64_t));
    if (n_rows == 0 || n_groups == 0) return out;

    /* Approximate path: when n_rows clears the HLL threshold (same as
     * the buf-form caller — 1 M rows), build a group-major idx layout
     * and run the sparse-HLL per-group kernel.  Sparse-representation
     * HLL makes this memory-bounded regardless of n_groups: each task
     * holds one sketch that's ≤ 17 KB total (1 KB sparse + 16 KB
     * dense, allocated together on the stack), so concurrent footprint
     * is n_workers × 17 KB instead of n_groups × 16 KB.  Returns a
     * ~0.8 % std-error estimate; callers that need exact counts at
     * this scale must not hit this gate. */
    /* COUNT(DISTINCT col) per group is EXACT.  Earlier streaming HLL
     * and CSR HLL fast paths returned approximation; removed.  The
     * partitioned exact path below handles all sizes. */

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
                else fv = clear_neg_zero(fv);
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
            if (cdpg_is_null(base, r, in_type, esz)) continue;
            /* Use a different name from the macro's inner `val` so
             * clang doesn't see an `int64_t val = (val);` self-init
             * after macro expansion. */
            int64_t row_val;
            if (in_type == RAY_F64) {
                double fv = ((double*)base)[r];
                if (fv != fv) fv = (double)NAN;
                else fv = clear_neg_zero(fv);
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
 * Why this layout avoids the realloc-per-group price:
 *   - A conventional holistic quantile aggregate accumulates a per-group
 *     value vector during the radix probe; each insert is a potential
 *     vector grow.  Finalization then nth_element's each group vector
 *     in parallel.
 *   - rayforce's radix probe (see idxbuf_par_fn) already produced
 *     prefix-summed group-contiguous indices.  So we skip the vector-grow
 *     phase entirely; each dispatched group task gathers values and
 *     quickselects.
 *
 * Cache behaviour: the inner loop reads src[idx_buf[off+i]] for a
 * single group, then quickselects the resulting slice.  The slice is
 * sized at grp_cnt[g] (median group ~1k for q6) and stays L2-hot for
 * the partial-sort.  Inputs are random over src so reads are still
 * cache-missing on the source column, but those misses overlap with
 * parallel tasks on other cores — the 27-core dispatch hides them.
 *
 * Type support: F64 native; I64/I32/I16/U8 cast-to-double on read.
 * Null rows are skipped pairwise.
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
    bool           use_quantile;
    double         q;
    const int64_t* idx_buf;
    const int64_t* offsets;
    const int64_t* grp_cnt;
    double*        scratch_pool; /* flat shared scratch, sized at sum(grp_cnt) */
    double*        out_data;     /* ray_data(out) */
    ray_t*         out;          /* for set_null */
} med_par_ctx_t;

typedef struct {
    uint64_t key;
    int64_t  count;
    int64_t  first_row;
    int64_t  first_pos;
    uint8_t  used;
} mode_scalar_entry_t;

typedef struct {
    uint64_t hash;
    int64_t  count;
    int64_t  first_row;
    int64_t  first_pos;
    uint8_t  used;
} mode_wide_entry_t;

typedef struct {
    ray_t*         src;
    ray_t*         out;
    const int64_t* idx_buf;
    const int64_t* offsets;
    const int64_t* grp_cnt;
    _Atomic(int)   oom;
    _Atomic(int)   cancel;
} mode_par_ctx_t;

static inline double med_read_as_f64(const void* base, int8_t t, int64_t row) {
    switch (t) {
        case RAY_F64: { double v; memcpy(&v, (const char*)base + (size_t)row * 8, 8); return v; }
        case RAY_I64: { int64_t v; memcpy(&v, (const char*)base + (size_t)row * 8, 8); return (double)v; }
        case RAY_I32: { int32_t v; memcpy(&v, (const char*)base + (size_t)row * 4, 4); return (double)v; }
        case RAY_DATE:
        case RAY_TIME: { int32_t v; memcpy(&v, (const char*)base + (size_t)row * 4, 4); return (double)v; }
        case RAY_TIMESTAMP: { int64_t v; memcpy(&v, (const char*)base + (size_t)row * 8, 8); return (double)v; }
        case RAY_I16: { int16_t v; memcpy(&v, (const char*)base + (size_t)row * 2, 2); return (double)v; }
        case RAY_U8:  return (double)((const uint8_t*)base)[row];
        default:      return 0.0;
    }
}

/* Type-correct sentinel null check for the med_par paths.  U8 is
 * non-nullable; med only accepts the listed types so SYM/STR/GUID/F32
 * never reach here. */
static inline bool med_is_null(const void* base, int8_t t, int64_t row) {
    switch (t) {
        case RAY_F64: { double v; memcpy(&v, (const char*)base + (size_t)row * 8, 8); return v != v; }
        case RAY_I64: return ((const int64_t*)base)[row] == NULL_I64;
        case RAY_I32: return ((const int32_t*)base)[row] == NULL_I32;
        case RAY_DATE:
        case RAY_TIME: return ((const int32_t*)base)[row] == NULL_I32;
        case RAY_TIMESTAMP: return ((const int64_t*)base)[row] == NULL_I64;
        case RAY_I16: return ((const int16_t*)base)[row] == NULL_I16;
        case RAY_U8:  return false;  /* non-nullable */
        default:      return false;
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
        if (c->has_nulls) {
            for (int64_t i = 0; i < cnt; i++) {
                int64_t row = c->idx_buf[off + i];
                if (med_is_null(c->base, c->src_type, row)) continue;
                slice[actual++] = med_read_as_f64(c->base, c->src_type, row);
            }
        } else {
            for (int64_t i = 0; i < cnt; i++) {
                int64_t row = c->idx_buf[off + i];
                slice[actual++] = med_read_as_f64(c->base, c->src_type, row);
            }
        }
        if (actual == 0) {
            c->out_data[g] = NULL_F64;
            ray_vec_set_null(c->out, g, true);
        } else {
            c->out_data[g] = c->use_quantile
                ? ray_quantile_dbl_inplace(slice, actual, c->q)
                : ray_median_dbl_inplace(slice, actual);
        }
    }
}

static double decode_agg_f64_param(int64_t bits) {
    double v = 0.0;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

static ray_t* rank_per_group_buf(ray_t* src,
                                 const int64_t* idx_buf,
                                 const int64_t* offsets,
                                 const int64_t* grp_cnt,
                                 int64_t n_groups,
                                 double q,
                                 bool use_quantile) {
    if (!src || RAY_IS_ERR(src) || n_groups < 0) return NULL;
    int8_t t = src->type;
    if (t != RAY_F64 && t != RAY_I64 && t != RAY_I32 &&
        t != RAY_I16 && t != RAY_U8 && t != RAY_DATE &&
        t != RAY_TIME && t != RAY_TIMESTAMP) return NULL;
    if (use_quantile && (!__builtin_isfinite(q) || q < 0.0 || q > 1.0))
        return ray_error("domain", "quantile: probability out of range");

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
        .use_quantile = use_quantile,
        .q = q,
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

ray_t* ray_median_per_group_buf(ray_t* src,
                                const int64_t* idx_buf,
                                const int64_t* offsets,
                                const int64_t* grp_cnt,
                                int64_t n_groups) {
    return rank_per_group_buf(src, idx_buf, offsets, grp_cnt, n_groups, 0.0, false);
}

ray_t* ray_quantile_per_group_buf(ray_t* src,
                                  const int64_t* idx_buf,
                                  const int64_t* offsets,
                                  const int64_t* grp_cnt,
                                  int64_t n_groups,
                                  double q) {
    return rank_per_group_buf(src, idx_buf, offsets, grp_cnt, n_groups, q, true);
}

static uint64_t mode_hash_cap(int64_t n) {
    uint64_t cap = 16;
    uint64_t need = n > 0 ? (uint64_t)n * 2u : 16u;
    if (need < 16) need = 16;
    while (cap < need && cap <= (UINT64_MAX >> 1)) cap <<= 1;
    return cap >= need ? cap : 0;
}

static inline uint64_t mode_scalar_key(ray_t* src, int64_t row) {
    const void* base = ray_data(src);
    switch (src->type) {
        case RAY_F64: {
            double v;
            memcpy(&v, (const char*)base + (size_t)row * 8, 8);
            if (v == 0.0) v = 0.0;
            uint64_t k;
            memcpy(&k, &v, 8);
            return k;
        }
        case RAY_F32: {
            float v;
            memcpy(&v, (const char*)base + (size_t)row * 4, 4);
            if (v == 0.0f) v = 0.0f;
            uint32_t k32;
            memcpy(&k32, &v, 4);
            return (uint64_t)k32;
        }
        case RAY_SYM:
            return (uint64_t)ray_read_sym(base, row, src->type, src->attrs);
        case RAY_I64:
        case RAY_TIMESTAMP: {
            int64_t v;
            memcpy(&v, (const char*)base + (size_t)row * 8, 8);
            return (uint64_t)v;
        }
        case RAY_I32:
        case RAY_DATE:
        case RAY_TIME: {
            int32_t v;
            memcpy(&v, (const char*)base + (size_t)row * 4, 4);
            return (uint64_t)(int64_t)v;
        }
        case RAY_I16: {
            int16_t v;
            memcpy(&v, (const char*)base + (size_t)row * 2, 2);
            return (uint64_t)(int64_t)v;
        }
        case RAY_BOOL:
        case RAY_U8:
            return (uint64_t)((const uint8_t*)base)[row];
        default:
            return 0;
    }
}

static inline void mode_copy_fixed_cell(ray_t* out, int64_t dst,
                                        ray_t* src, int64_t row) {
    uint8_t esz = col_esz(src);
    memcpy((char*)ray_data(out) + (size_t)dst * esz,
           (const char*)ray_data(src) + (size_t)row * esz,
           esz);
}

static inline void mode_set_null_cell(ray_t* out, int64_t dst) {
    switch (out->type) {
        case RAY_F64:
        case RAY_F32:
        case RAY_I64:
        case RAY_TIMESTAMP:
        case RAY_I32:
        case RAY_DATE:
        case RAY_TIME:
        case RAY_I16:
            par_set_null(out, dst);
            return;
        case RAY_GUID:
            memset((uint8_t*)ray_data(out) + (size_t)dst * 16, 0, 16);
            __atomic_fetch_or(&out->attrs, (uint8_t)RAY_ATTR_HAS_NULLS,
                              __ATOMIC_RELAXED);
            return;
        case RAY_STR:
            memset((ray_str_t*)ray_data(out) + dst, 0, sizeof(ray_str_t));
            return;
        case RAY_SYM:
            ray_write_sym(ray_data(out), dst, 0, out->type, out->attrs);
            return;
        case RAY_BOOL:
        case RAY_U8:
            ((uint8_t*)ray_data(out))[dst] = 0;
            return;
        default:
            return;
    }
}

static int64_t mode_scalar_group(ray_t* src, const int64_t* rows,
                                 int64_t cnt, int* cancelled) {
    uint64_t cap = mode_hash_cap(cnt);
    if (!cap || cap > SIZE_MAX / sizeof(mode_scalar_entry_t)) return -2;
    ray_t* hdr = NULL;
    mode_scalar_entry_t* ht = (mode_scalar_entry_t*)scratch_calloc(
        &hdr, (size_t)cap * sizeof(mode_scalar_entry_t));
    if (!ht) return -2;
    uint64_t mask = cap - 1;
    bool has_nulls = (src->attrs & RAY_ATTR_HAS_NULLS) != 0;
    int64_t best_count = 0, best_row = -1, best_pos = INT64_MAX;
    for (int64_t i = 0; i < cnt; i++) {
        if ((i & 65535) == 0 && ray_interrupted()) {
            *cancelled = 1;
            scratch_free(hdr);
            return -3;
        }
        int64_t row = rows[i];
        if (has_nulls && ray_vec_is_null(src, row)) continue;
        uint64_t key = mode_scalar_key(src, row);
        uint64_t slot = ray_hash_i64((int64_t)key) & mask;
        for (;;) {
            mode_scalar_entry_t* e = &ht[slot];
            if (!e->used) {
                e->used = 1;
                e->key = key;
                e->count = 1;
                e->first_row = row;
                e->first_pos = i;
                if (best_count == 0 || e->first_pos < best_pos) {
                    best_count = 1;
                    best_row = row;
                    best_pos = i;
                }
                break;
            }
            if (e->key == key) {
                e->count++;
                if (e->count > best_count ||
                    (e->count == best_count && e->first_pos < best_pos)) {
                    best_count = e->count;
                    best_row = e->first_row;
                    best_pos = e->first_pos;
                }
                break;
            }
            slot = (slot + 1) & mask;
        }
    }
    scratch_free(hdr);
    return best_row;
}

static int64_t mode_guid_group(ray_t* src, const int64_t* rows,
                               int64_t cnt, int* cancelled) {
    uint64_t cap = mode_hash_cap(cnt);
    if (!cap || cap > SIZE_MAX / sizeof(mode_wide_entry_t)) return -2;
    ray_t* hdr = NULL;
    mode_wide_entry_t* ht = (mode_wide_entry_t*)scratch_calloc(
        &hdr, (size_t)cap * sizeof(mode_wide_entry_t));
    if (!ht) return -2;
    uint64_t mask = cap - 1;
    const uint8_t* base = (const uint8_t*)ray_data(src);
    bool has_nulls = (src->attrs & RAY_ATTR_HAS_NULLS) != 0;
    int64_t best_count = 0, best_row = -1, best_pos = INT64_MAX;
    for (int64_t i = 0; i < cnt; i++) {
        if ((i & 65535) == 0 && ray_interrupted()) {
            *cancelled = 1;
            scratch_free(hdr);
            return -3;
        }
        int64_t row = rows[i];
        if (has_nulls && ray_vec_is_null(src, row)) continue;
        const uint8_t* key = base + (size_t)row * 16;
        uint64_t h = ray_hash_bytes(key, 16);
        uint64_t slot = h & mask;
        for (;;) {
            mode_wide_entry_t* e = &ht[slot];
            if (!e->used) {
                e->used = 1;
                e->hash = h;
                e->count = 1;
                e->first_row = row;
                e->first_pos = i;
                if (best_count == 0 || e->first_pos < best_pos) {
                    best_count = 1;
                    best_row = row;
                    best_pos = i;
                }
                break;
            }
            if (e->hash == h &&
                memcmp(key, base + (size_t)e->first_row * 16, 16) == 0) {
                e->count++;
                if (e->count > best_count ||
                    (e->count == best_count && e->first_pos < best_pos)) {
                    best_count = e->count;
                    best_row = e->first_row;
                    best_pos = e->first_pos;
                }
                break;
            }
            slot = (slot + 1) & mask;
        }
    }
    scratch_free(hdr);
    return best_row;
}

static void mode_fixed_per_group_fn(void* ctx_v, uint32_t worker_id,
                                    int64_t start, int64_t end) {
    (void)worker_id;
    mode_par_ctx_t* c = (mode_par_ctx_t*)ctx_v;
    for (int64_t g = start; g < end; g++) {
        if (atomic_load_explicit(&c->oom, memory_order_relaxed) ||
            atomic_load_explicit(&c->cancel, memory_order_relaxed))
            return;
        int64_t off = c->offsets[g];
        int64_t cnt = c->grp_cnt[g];
        int cancelled = 0;
        int64_t best = (c->src->type == RAY_GUID)
            ? mode_guid_group(c->src, &c->idx_buf[off], cnt, &cancelled)
            : mode_scalar_group(c->src, &c->idx_buf[off], cnt, &cancelled);
        if (cancelled || best == -3) {
            atomic_store_explicit(&c->cancel, 1, memory_order_relaxed);
            return;
        }
        if (best == -2) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        if (best < 0) mode_set_null_cell(c->out, g);
        else mode_copy_fixed_cell(c->out, g, c->src, best);
    }
}

static ray_t* mode_str_per_group_buf(ray_t* src,
                                     const int64_t* idx_buf,
                                     const int64_t* offsets,
                                     const int64_t* grp_cnt,
                                     int64_t n_groups) {
    ray_t* out = col_vec_new(src, n_groups);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n_groups;

    for (int64_t g = 0; g < n_groups; g++) {
        int64_t cnt = grp_cnt[g];
        int64_t off = offsets[g];
        uint64_t cap = mode_hash_cap(cnt);
        if (!cap || cap > SIZE_MAX / sizeof(mode_wide_entry_t)) {
            ray_release(out);
            return ray_error("oom", NULL);
        }
        ray_t* hdr = NULL;
        mode_wide_entry_t* ht = (mode_wide_entry_t*)scratch_calloc(
            &hdr, (size_t)cap * sizeof(mode_wide_entry_t));
        if (!ht) {
            ray_release(out);
            return ray_error("oom", NULL);
        }
        uint64_t mask = cap - 1;
        int64_t best_count = 0, best_row = -1, best_pos = INT64_MAX;
        for (int64_t i = 0; i < cnt; i++) {
            if ((i & 65535) == 0 && ray_interrupted()) {
                scratch_free(hdr);
                ray_release(out);
                return ray_error("cancel", "interrupted");
            }
            int64_t row = idx_buf[off + i];
            size_t len = 0;
            const char* key = ray_str_vec_get(src, row, &len);
            if (!key) { key = ""; len = 0; }
            uint64_t h = ray_hash_bytes(key, len);
            uint64_t slot = h & mask;
            for (;;) {
                mode_wide_entry_t* e = &ht[slot];
                if (!e->used) {
                    e->used = 1;
                    e->hash = h;
                    e->count = 1;
                    e->first_row = row;
                    e->first_pos = i;
                    if (best_count == 0 || e->first_pos < best_pos) {
                        best_count = 1;
                        best_row = row;
                        best_pos = i;
                    }
                    break;
                }
                size_t olen = 0;
                const char* old = ray_str_vec_get(src, e->first_row, &olen);
                if (!old) { old = ""; olen = 0; }
                if (e->hash == h && olen == len &&
                    (len == 0 || memcmp(old, key, len) == 0)) {
                    e->count++;
                    if (e->count > best_count ||
                        (e->count == best_count && e->first_pos < best_pos)) {
                        best_count = e->count;
                        best_row = e->first_row;
                        best_pos = e->first_pos;
                    }
                    break;
                }
                slot = (slot + 1) & mask;
            }
        }
        scratch_free(hdr);
        if (best_row < 0) {
            mode_set_null_cell(out, g);
        } else {
            size_t len = 0;
            const char* s = ray_str_vec_get(src, best_row, &len);
            ray_t* nv = ray_str_vec_set(out, g, s ? s : "", s ? len : 0);
            if (!nv || RAY_IS_ERR(nv)) {
                ray_release(out);
                return nv ? nv : ray_error("oom", NULL);
            }
            out = nv;
        }
    }
    return out;
}

ray_t* ray_mode_per_group_buf(ray_t* src,
                              const int64_t* idx_buf,
                              const int64_t* offsets,
                              const int64_t* grp_cnt,
                              int64_t n_groups) {
    if (!src || RAY_IS_ERR(src) || n_groups < 0) return NULL;
    switch (src->type) {
        case RAY_BOOL:
        case RAY_U8:
        case RAY_I16:
        case RAY_I32:
        case RAY_I64:
        case RAY_F32:
        case RAY_F64:
        case RAY_DATE:
        case RAY_TIME:
        case RAY_TIMESTAMP:
        case RAY_GUID:
        case RAY_SYM:
        case RAY_STR:
            break;
        default:
            return NULL;
    }
    if (src->type == RAY_STR)
        return mode_str_per_group_buf(src, idx_buf, offsets, grp_cnt, n_groups);

    int64_t total = 0;
    for (int64_t g = 0; g < n_groups; g++) total += grp_cnt[g];

    ray_t* out = col_vec_new(src, n_groups);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    if (out->type == RAY_SYM)
        ray_sym_vec_adopt_domain(out, sym_domain_rep(src));
    out->len = n_groups;

    mode_par_ctx_t ctx = {
        .src = src,
        .out = out,
        .idx_buf = idx_buf,
        .offsets = offsets,
        .grp_cnt = grp_cnt,
        .oom = 0,
        .cancel = 0,
    };

    ray_pool_t* pool = ray_pool_get();
    bool par = pool && n_groups >= 8 && total >= 4096;
    if (par) {
        if (n_groups < (1 << 16))
            ray_pool_dispatch_n(pool, mode_fixed_per_group_fn, &ctx,
                                (uint32_t)n_groups);
        else
            ray_pool_dispatch(pool, mode_fixed_per_group_fn, &ctx, n_groups);
    } else {
        mode_fixed_per_group_fn(&ctx, 0, 0, n_groups);
    }

    if (atomic_load_explicit(&ctx.cancel, memory_order_relaxed) ||
        ray_interrupted()) {
        ray_release(out);
        return ray_error("cancel", "interrupted");
    }
    if (atomic_load_explicit(&ctx.oom, memory_order_relaxed)) {
        ray_release(out);
        return ray_error("oom", NULL);
    }
    if (out->type != RAY_STR && out->type != RAY_BOOL &&
        out->type != RAY_U8 && out->type != RAY_SYM)
        par_finalize_nulls(out);
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
                if (c->has_nulls && med_is_null(c->base, c->src_type, row)) continue;
                dst[kept++] = topk_read_f64(c->base, row);
            }
            if (kept == K) {
                for (int64_t j = K/2 - 1; j >= 0; j--)
                    topk_sift_down_dbl(dst, K, j, max_heap);
                for (int64_t i = init_end; i < cnt; i++) {
                    int64_t row = idxs[i];
                    if (c->has_nulls && med_is_null(c->base, c->src_type, row)) continue;
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
                if (c->has_nulls && med_is_null(c->base, c->src_type, row)) continue;
                heap[kept++] = topk_read_i64(c->base, t, row);
            }
            if (kept == K) {
                for (int64_t j = K/2 - 1; j >= 0; j--)
                    topk_sift_down_i64(heap, K, j, max_heap);
                for (int64_t i = init_end; i < cnt; i++) {
                    int64_t row = idxs[i];
                    if (c->has_nulls && med_is_null(c->base, c->src_type, row)) continue;
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

/* ─── ray_wide_minmax_per_group_buf ───────────────────────────────────────
 *
 * Per-group min/max/first/last for wide element types (STR/GUID) that don't
 * fit the 8-byte integer accumulators.  Same idx_buf/offsets/grp_cnt layout
 * as the median/topk kernels — produced by exec_group's group-contiguous row
 * gather — but instead of a numeric quickselect it finds the winning row per
 * group (lexicographic for STR, byte order for GUID; positional for
 * first/last) and materialises that element into a typed result column.
 *
 * Runs SERIAL: ray_str_vec_set COW-mutates the result vector and its shared
 * string pool, so concurrent group writers would corrupt the pool.  Wide
 * min/max is a cold path, not a vectorised bench kernel, so this is fine. */
ray_t* ray_wide_minmax_per_group_buf(ray_t* src, uint16_t op,
                                     const int64_t* idx_buf,
                                     const int64_t* offsets,
                                     const int64_t* grp_cnt,
                                     int64_t n_groups) {
    if (!src || RAY_IS_ERR(src) || n_groups < 0) return NULL;
    if (!agg_is_wide_type(src->type)) return NULL;  /* caller falls back */
    bool has_nulls = (src->attrs & RAY_ATTR_HAS_NULLS) != 0;

    ray_t* out = col_vec_new(src, n_groups);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n_groups;

    for (int64_t g = 0; g < n_groups; g++) {
        int64_t cnt = grp_cnt[g];
        int64_t off = offsets[g];
        int64_t best = wide_winner_row(src, op, &idx_buf[off], cnt, has_nulls);
        if (best < 0) { ray_vec_set_null(out, g, true); continue; }
        int alloc;
        ray_t* e = collection_elem(src, best, &alloc);
        if (src->type == RAY_STR) {
            ray_t* nv = ray_str_vec_set(out, g, ray_str_ptr(e), ray_str_len(e));
            if (alloc) ray_release(e);
            if (!nv || RAY_IS_ERR(nv)) { if (nv != out) ray_release(out); return nv ? nv : ray_error("oom", NULL); }
            out = nv;
        } else {  /* RAY_GUID — fixed 16-byte in-place store */
            store_typed_elem(out, g, e);
            if (alloc) ray_release(e);
        }
    }
    return out;
}

/* `src` is the reduced SYM column when out_type == RAY_SYM (NULL
 * otherwise): the accumulated value is a CELL id in src's domain, and
 * the result is a SYM ATOM — runtime-domain by the atom rule — so it
 * must be re-expressed (raw copy while src is runtime-domain). */
static ray_t* reduction_i64_result(int64_t val, int8_t out_type, ray_t* src) {
    switch (out_type) {
        case RAY_BOOL:      return ray_bool((bool)val);
        case RAY_DATE:      return ray_date((int32_t)val);
        case RAY_TIME:      return ray_time(val);
        case RAY_TIMESTAMP: return ray_timestamp(val);
        case RAY_I32:       return ray_i32((int32_t)val);
        case RAY_I16:       return ray_i16((int16_t)val);
        case RAY_U8:        return ray_u8((uint8_t)val);
        case RAY_SYM:       return ray_sym(src ? sym_id_runtime(src, val) : val);
        default:            return ray_i64(val);
    }
}

static ray_t* reduction_extreme_result(ray_op_t* op, int8_t in_type, bool found,
                                       double fval, int64_t ival, ray_t* src) {
    int8_t out_type = op->out_type ? op->out_type : in_type;
    if (!found) return ray_typed_null(-out_type);
    /* Single-null float model: min/max of finite inputs is finite, but guard
     * against an ±Inf init sentinel surfacing as a value. */
    if (out_type == RAY_F64) return ray_f64(ray_f64_fin(fval));
    return reduction_i64_result(ival, out_type, out_type == RAY_SYM ? src : NULL);
}

ray_t* exec_reduction(ray_graph_t* g, ray_op_t* op, ray_t* input) {
    if (!input || RAY_IS_ERR(input)) return input;

    /* TABLE input: COUNT returns row count, others need a column */
    if (input->type == RAY_TABLE) {
        if (op->opcode == OP_COUNT)
            return ray_i64(ray_table_nrows(input));
        return ray_error("type", "reduction: %s requires a column, got a table", ray_opcode_name(op->opcode));
    }

    /* Atom input: a reduction of a scalar sub-expression (e.g.
     * (sum (max v)), (sum (count (distinct v)))) reaches here when the
     * reduction is compiled as a DAG node whose input materialises to an
     * atom.  Delegate to the scalar builtin so the DAG agrees with the
     * direct scalar form — (sum 6) and (sum (max v)) must behave the same.
     * The builtins handle the atom in place (no recursion back here) and
     * return an owned value without consuming `input` (the caller releases
     * it).  COUNT keeps its column-cardinality semantics (1 / strlen). */
    if (ray_is_atom(input)) {
        switch (op->opcode) {
            case OP_COUNT:
                if ((-input->type) == RAY_STR)
                    return ray_i64((int64_t)ray_str_len(input));
                return ray_i64(1);
            case OP_SUM:        return ray_sum_fn(input);
            case OP_ALL:        return ray_all_fn(input);
            case OP_ANY:        return ray_any_fn(input);
            case OP_AVG:        return ray_avg_fn(input);
            case OP_MIN:        return ray_min_fn(input);
            case OP_MAX:        return ray_max_fn(input);
            case OP_FIRST:      return ray_first_fn(input);
            case OP_LAST:       return ray_last_fn(input);
            case OP_MEDIAN:     return ray_med_fn(input);
            case OP_VAR:        return ray_var_fn(input);
            case OP_VAR_POP:    return ray_var_pop_fn(input);
            case OP_STDDEV:     return ray_stddev_fn(input);
            case OP_STDDEV_POP: return ray_stddev_pop_fn(input);
            /* OP_PROD has no scalar builtin; prod of a single element is
             * that element. */
            case OP_PROD:       ray_retain(input); return input;
            default:            return ray_error("type", "reduction: unsupported op %s on atom of type %s", ray_opcode_name(op->opcode), ray_type_name(input->type));
        }
    }

    int8_t in_type = input->type;
    int64_t len = input->len;

    /* Sentinel-based per-element null detection happens inside
     * REDUCE_LOOP_I/F via the type-correct NULL_* constant; the
     * has_nulls attribute below is the vec-level fast-path gate. */
    bool has_nulls = (input->attrs & RAY_ATTR_HAS_NULLS) != 0;

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

    /* Wide element types (STR/GUID) overflow the 8-byte reduce
     * accumulators; resolve min/max/first/last by materialising the
     * winning row instead.  COUNT keeps the generic length-based path. */
    if (agg_is_wide_type(in_type) &&
        (op->opcode == OP_MIN || op->opcode == OP_MAX ||
         op->opcode == OP_FIRST || op->opcode == OP_LAST)) {
        ray_t* r = agg_wide_reduce(input, op->opcode, sel_idx, scan_n, has_nulls);
        if (sel_idx_block) ray_release(sel_idx_block);
        return r;
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
                if (!has_nulls || !ray_vec_is_null(input, r)) { row = r; break; }
            }
        } else {
            for (int64_t i = scan_n - 1; i >= 0; i--) {
                int64_t r = sel_idx ? sel_idx[i] : i;
                if (!has_nulls || !ray_vec_is_null(input, r)) { row = r; break; }
            }
        }
        if (sel_idx_block) ray_release(sel_idx_block);
        if (row < 0 || row >= len)
            return ray_typed_null(-in_type);
        void* base = ray_data(input);
        if (in_type == RAY_F64) return ray_f64(((const double*)base)[row]);
        return reduction_i64_result(read_col_i64(base, row, in_type, input->attrs), in_type,
                                    in_type == RAY_SYM ? input : NULL);
    }

    ray_pool_t* pool = ray_pool_get();
    if (pool && scan_n >= RAY_PARALLEL_THRESHOLD) {
        uint32_t nw = ray_pool_total_workers(pool);
        ray_t* accs_hdr;
        reduce_acc_t* accs = (reduce_acc_t*)scratch_calloc(&accs_hdr, nw * sizeof(reduce_acc_t));
        if (!accs) { if (sel_idx_block) ray_release(sel_idx_block); return ray_error("oom", NULL); }
        for (uint32_t i = 0; i < nw; i++) reduce_acc_init(&accs[i]);

        par_reduce_ctx_t ctx = { .input = input, .accs = accs,
                                 .has_nulls = has_nulls, .idx = sel_idx };
        ray_pool_dispatch(pool, par_reduce_fn, &ctx, scan_n);

        /* Merge: worker 0 is the base, merge the rest in order */
        reduce_acc_t merged;
        reduce_acc_init(&merged);
        merged = accs[0];
        for (uint32_t i = 1; i < nw; i++) {
            if (!accs[i].has_first) continue;
            reduce_merge(&merged, &accs[i], in_type, ray_sym_vec_domain(input));
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
            case OP_SUM:   result = in_type == RAY_F64 ? ray_f64(ray_f64_fin(merged.sum_f)) : (in_type == RAY_TIME ? ray_time(merged.sum_i) : ray_i64(merged.sum_i)); break;
            case OP_PROD:  result = in_type == RAY_F64 ? ray_f64(ray_f64_fin(merged.prod_f)) : ray_i64(merged.prod_i); break;
            case OP_ALL:   result = ray_bool(merged.zero_count == 0); break;
            case OP_ANY:   result = ray_bool(merged.cnt > merged.zero_count); break;
            case OP_MIN:   result = reduction_extreme_result(op, in_type, merged.cnt > 0, merged.min_f, merged.min_i, input); break;
            case OP_MAX:   result = reduction_extreme_result(op, in_type, merged.cnt > 0, merged.max_f, merged.max_i, input); break;
            /* COUNT returns total length including nulls — matches ray_count_fn's
             * "count all elements" semantics, not SQL's COUNT(col) non-null count. */
            case OP_COUNT: result = ray_i64(scan_n); break;
            case OP_AVG:   result = merged.cnt > 0 ? ray_f64(ray_f64_fin(in_type == RAY_F64 ? merged.sum_f / merged.cnt : merged.sum_d / merged.cnt)) : ray_typed_null(-RAY_F64); break;
            case OP_FIRST: result = merged.has_first ? (in_type == RAY_F64 ? ray_f64(merged.first_f) : reduction_i64_result(merged.first_i, in_type, in_type == RAY_SYM ? input : NULL)) : ray_typed_null(-in_type); break;
            case OP_LAST:  result = merged.has_first ? (in_type == RAY_F64 ? ray_f64(merged.last_f) : reduction_i64_result(merged.last_i, in_type, in_type == RAY_SYM ? input : NULL)) : ray_typed_null(-in_type); break;
            case OP_VAR: case OP_VAR_POP:
            case OP_STDDEV: case OP_STDDEV_POP: {
                bool insufficient = (op->opcode == OP_VAR || op->opcode == OP_STDDEV) ? merged.cnt <= 1 : merged.cnt <= 0;
                if (insufficient) { result = ray_typed_null(-RAY_F64); break; }
                double mean, var_pop;
                if (in_type == RAY_F64) { mean = merged.sum_f / merged.cnt; var_pop = merged.sum_sq_f / merged.cnt - mean * mean; }
                else { mean = merged.sum_d / merged.cnt; var_pop = (double)merged.sum_sq_i / merged.cnt - mean * mean; }
                if (var_pop < 0) var_pop = 0;
                double val;
                if (op->opcode == OP_VAR_POP) val = var_pop;
                else if (op->opcode == OP_VAR) val = var_pop * merged.cnt / (merged.cnt - 1);
                else if (op->opcode == OP_STDDEV_POP) val = sqrt(var_pop);
                else val = sqrt(var_pop * merged.cnt / (merged.cnt - 1));
                result = ray_f64(ray_f64_fin(val));
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
    reduce_range(input, 0, scan_n, &acc, has_nulls, sel_idx);
    if (sel_idx_block) ray_release(sel_idx_block);

    switch (op->opcode) {
        case OP_SUM:   return in_type == RAY_F64 ? ray_f64(ray_f64_fin(acc.sum_f)) : (in_type == RAY_TIME ? ray_time(acc.sum_i) : ray_i64(acc.sum_i));
        case OP_PROD:  return in_type == RAY_F64 ? ray_f64(ray_f64_fin(acc.prod_f)) : ray_i64(acc.prod_i);
        case OP_ALL:   return ray_bool(acc.zero_count == 0);
        case OP_ANY:   return ray_bool(acc.cnt > acc.zero_count);
        case OP_MIN:   return reduction_extreme_result(op, in_type, acc.cnt > 0, acc.min_f, acc.min_i, input);
        case OP_MAX:   return reduction_extreme_result(op, in_type, acc.cnt > 0, acc.max_f, acc.max_i, input);
        /* COUNT returns total length including nulls — matches ray_count_fn's
         * "count all elements" semantics, not SQL's COUNT(col) non-null count. */
        case OP_COUNT: return ray_i64(scan_n);
        case OP_AVG:   return acc.cnt > 0 ? ray_f64(ray_f64_fin(in_type == RAY_F64 ? acc.sum_f / acc.cnt : acc.sum_d / acc.cnt)) : ray_typed_null(-RAY_F64);
        case OP_FIRST: return acc.has_first ? (in_type == RAY_F64 ? ray_f64(acc.first_f) : reduction_i64_result(acc.first_i, in_type, in_type == RAY_SYM ? input : NULL)) : ray_typed_null(-in_type);
        case OP_LAST:  return acc.has_first ? (in_type == RAY_F64 ? ray_f64(acc.last_f) : reduction_i64_result(acc.last_i, in_type, in_type == RAY_SYM ? input : NULL)) : ray_typed_null(-in_type);
        case OP_VAR: case OP_VAR_POP:
        case OP_STDDEV: case OP_STDDEV_POP: {
            bool insufficient = (op->opcode == OP_VAR || op->opcode == OP_STDDEV) ? acc.cnt <= 1 : acc.cnt <= 0;
            if (insufficient) return ray_typed_null(-RAY_F64);
            double mean, var_pop;
            if (in_type == RAY_F64) { mean = acc.sum_f / acc.cnt; var_pop = acc.sum_sq_f / acc.cnt - mean * mean; }
            else { mean = acc.sum_d / acc.cnt; var_pop = (double)acc.sum_sq_i / acc.cnt - mean * mean; }
            if (var_pop < 0) var_pop = 0;
            double val;
            if (op->opcode == OP_VAR_POP) val = var_pop;
            else if (op->opcode == OP_VAR) val = var_pop * acc.cnt / (acc.cnt - 1);
            else if (op->opcode == OP_STDDEV_POP) val = sqrt(var_pop);
            else val = sqrt(var_pop * acc.cnt / (acc.cnt - 1));
            return ray_f64(ray_f64_fin(val));
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

/* Aim the base pointers at the in-struct inline arrays (≤ GHT_INLINE case). */
static inline void ght_layout_point_inline(ght_layout_t* ly) {
    ly->agg_val_slot  = ly->agg_val_slot_in;
    ly->agg_flags     = ly->agg_flags_in;
    ly->agg_flags2    = ly->agg_flags2_in;
    ly->agg_null_sentinel = ly->agg_null_sentinel_in;
    ly->agg_dom       = ly->agg_dom_in;
    ly->key_off       = ly->key_off_in;
    ly->key_flags     = ly->key_flags_in;
    ly->wide_key_esz  = ly->wide_key_esz_in;
    ly->wide_key_type = ly->wide_key_type_in;
}

/* Carve one owned heap block for a wide (> GHT_INLINE) layout and aim the
 * base pointers into it.  8-byte-first (agg_dom is a pointer array), then the
 * uint16 key_off vector, then the byte arrays.  Zero-initialised (scratch_calloc)
 * to match the memset-cleared inline path.  Unreachable while the width gates
 * hold; kept correct for later cuts.  Returns false on OOM. */
static bool ght_layout_alloc_spill(ght_layout_t* ly, uint32_t n_keys, uint32_t n_aggs) {
    size_t off = 0;
    /* 8-byte-first: pointer array (agg_dom) then the int64 sentinel array,
     * then the uint16 key_off vector, then the byte arrays. */
    size_t dom_off = off;                        off += (size_t)n_aggs * sizeof(void*);
    size_t sent_off = off;                        off += (size_t)n_aggs * sizeof(int64_t);
    size_t koff_off = off;                       off += (size_t)(n_keys + 1) * sizeof(uint16_t);
    off = (off + 1u) & ~(size_t)1u;              /* re-align not needed after uint16, but keep bytes packed */
    size_t vslot_off = off;                      off += (size_t)n_aggs;
    size_t aflags_off = off;                     off += (size_t)n_aggs;
    size_t aflags2_off = off;                     off += (size_t)n_aggs;
    size_t kflags_off = off;                     off += (size_t)n_keys;
    size_t wesz_off = off;                       off += (size_t)n_keys;
    size_t wtype_off = off;                      off += (size_t)n_keys;
    char* base = (char*)scratch_calloc(&ly->spill_hdr, off ? off : 1);
    if (!base) { ly->spill_hdr = NULL; return false; }
    ly->agg_dom       = (struct ray_sym_domain_s**)(void*)(base + dom_off);
    ly->agg_null_sentinel = (int64_t*)(void*)(base + sent_off);
    ly->key_off       = (uint16_t*)(void*)(base + koff_off);
    ly->agg_val_slot  = (int8_t*)(base + vslot_off);
    ly->agg_flags     = (uint8_t*)(base + aflags_off);
    ly->agg_flags2    = (uint8_t*)(base + aflags2_off);
    ly->key_flags     = (uint8_t*)(base + kflags_off);
    ly->wide_key_esz  = (uint8_t*)(base + wesz_off);
    ly->wide_key_type = (int8_t*)(base + wtype_off);
    return true;
}

void ght_layout_free(ght_layout_t* ly) {
    if (ly && ly->spill_hdr) { scratch_free(ly->spill_hdr); ly->spill_hdr = NULL; }
}

void ght_layout_copy(ght_layout_t* dst, const ght_layout_t* src) {
    *dst = *src;   /* scalars + inline-array CONTENTS; the pointers are wrong */
    /* Decide by STORAGE, not ownership: src->spill_hdr == NULL is true both
     * for a genuine inline layout AND for a BORROWER (a prior copy of a
     * spilled master — its bases point into the master's spill block but it
     * does not own that block, so its own spill_hdr is NULL).  Testing
     * spill_hdr here would send a borrower-of-a-borrower down the inline
     * branch, re-pointing it at its own zeroed size-8 *_in arrays — silently
     * truncating any layout with > GHT_INLINE keys/aggs two copies deep
     * (e.g. master ctx -> per-partition ctx -> per-partition group_ht_t).
     *
     * The storage-identity test below is depth-invariant: src->agg_val_slot
     * == src->agg_val_slot_in is true iff src's bases are self-referential,
     * i.e. src itself is inline storage (whether or not src owns it) — a
     * spilled master, a borrower, and a borrower-of-a-borrower all read
     * false here alike, so master -> b1 -> b2 -> b3 ... all correctly keep
     * borrowing the one shared spill block.  This needs no new field: the
     * inline arrays already carry a unique identity (dst's own struct
     * address), so pointer identity against them IS the storage test. */
    if (src->agg_val_slot == src->agg_val_slot_in) {
        /* Inline storage: re-aim the bases at dst's own inline arrays
         * (self-contained — no dependency on src's lifetime). */
        ght_layout_point_inline(dst);
        dst->spill_hdr = NULL;
    } else {
        /* Spill storage (src is either the owning master or a prior
         * borrower): BORROW the shared read-only spill block.  The base
         * pointers copied above already aim into it — that's true whether
         * src owns the block or is itself borrowing it, since a borrower's
         * bases are copied verbatim from what it borrowed.  Drop ownership
         * so ght_layout_free(dst) is a no-op.  Lifetime rule: the OWNING
         * MASTER must outlive every borrower transitively copied from it —
         * all borrowers (at any depth) must be done with / freed before the
         * master frees the spill. */
        dst->spill_hdr = NULL;
    }
}

bool ght_compute_layout(ght_layout_t* out, uint32_t n_keys, uint32_t n_aggs,
                        ray_t** agg_vecs, uint8_t need_flags,
                        const uint16_t* agg_ops,
                        const int8_t* key_types) {
    memset(out, 0, sizeof(*out));
    out->n_keys = (uint16_t)n_keys;
    out->n_aggs = (uint16_t)n_aggs;
    out->need_flags = need_flags;
    /* Inline (≤ GHT_INLINE keys AND aggs) reuses the same cache lines as the
     * old fixed [8] fields; wider layouts carve one owned spill block. */
    if (n_keys > GHT_INLINE || n_aggs > GHT_INLINE) {
        if (!ght_layout_alloc_spill(out, n_keys, n_aggs)) return false;
    } else {
        ght_layout_point_inline(out);
    }

    /* Mark wide keys (those that don't fit in 8 bytes).  For each wide key
     * the fat-entry / HT-row key slot stores a source row index; probe/rehash/
     * scatter resolve the actual bytes via group_ht_t.key_data[k].  RAY_STR
     * keys additionally store the 16-byte ray_str_t descriptor INLINE in the
     * key region (GHT_KEYF_INLINE_STR) so hash/eq are cache-local (key_pool[k]
     * still used for >12 B full eq). */
    if (key_types) {
        for (uint32_t k = 0; k < n_keys; k++) {
            if (key_types[k] == RAY_GUID) {
                out->key_flags[k] = GHT_KEYF_WIDE;
                out->wide_key_esz[k] = 16;
                out->wide_key_type[k] = RAY_GUID;
                out->any_wide_key = 1;
            } else if (key_types[k] == RAY_STR) {
                out->key_flags[k] = (uint8_t)(GHT_KEYF_WIDE | GHT_KEYF_INLINE_STR);
                out->wide_key_esz[k] = (uint8_t)sizeof(ray_str_t);
                out->wide_key_type[k] = RAY_STR;
                out->any_wide_key = 1;
                out->any_inline_str = 1;
            }
        }
    }

    uint16_t nv = 0;
    uint8_t agg_any = 0;
    for (uint32_t a = 0; a < n_aggs; a++) {
        /* OP_MEDIAN / OP_TOP_N / OP_BOT_N / OP_MODE reserve no row-layout slot —
         * the column is materialized in agg_vecs[a] but values are not
         * packed into entries or HT rows.  A post-radix pass over
         * row_gid+grp_cnt gathers per-group slices and runs the matching
         * per-group kernel. */
        /* Wide-element (STR/GUID) min/max/first/last also reserve no
         * row-layout slot: the 8-byte accumulators can't hold a 16-byte
         * GUID or a pooled string, so they are resolved by the same
         * post-radix per-group pass via ray_wide_minmax_per_group_buf. */
        bool wide_mm = agg_ops && agg_vecs[a] &&
                       agg_is_wide_type(agg_vecs[a]->type) &&
                       (agg_ops[a] == OP_MIN  || agg_ops[a] == OP_MAX ||
                        agg_ops[a] == OP_FIRST || agg_ops[a] == OP_LAST);
        bool holistic = agg_ops && (agg_ops[a] == OP_MEDIAN ||
                                    agg_ops[a] == OP_QUANTILE ||
                                    agg_ops[a] == OP_MODE ||
                                    agg_ops[a] == OP_TOP_N ||
                                    agg_ops[a] == OP_BOT_N || wide_mm);
        uint8_t af = 0;
        if (holistic) {
            af |= GHT_AF_HOLISTIC;
            if (wide_mm) af |= GHT_AF_WIDE;
            out->agg_val_slot[a] = -1;
        } else if (agg_vecs[a]) {
            out->agg_val_slot[a] = (int8_t)nv;
            if (agg_vecs[a]->type == RAY_F64)
                af |= GHT_AF_F64;
            if (agg_vecs[a]->type == RAY_SYM) {
                af |= GHT_AF_SYM;
                /* lex MIN/MAX resolves cell ids through the COLUMN's
                 * domain (borrowed; the agg vec outlives the layout). */
                out->agg_dom[a] = ray_sym_vec_domain(agg_vecs[a]);
            }
            nv++;
            /* Binary aggregator (OP_PEARSON_CORR): the y-side input
             * occupies the very next slot so phase1 packs (x, y)
             * consecutively.  GHT_AF_BINARY drives that packing. */
            if (agg_ops && agg_is_binary_agg(agg_ops[a])) {
                af |= GHT_AF_BINARY;
                nv++;
            }
        } else {
            out->agg_val_slot[a] = -1;
        }
        if (agg_ops && !wide_mm) {
            /* wide first/last are resolved holistically and need no
             * entry-tail row slot, so they are excluded here. */
            if (agg_ops[a] == OP_FIRST) af |= GHT_AF_FIRST;
            if (agg_ops[a] == OP_LAST)  af |= GHT_AF_LAST;
            if (agg_ops[a] == OP_PROD)  af |= GHT_AF_PROD;
        }
        out->agg_flags[a] = af;
        agg_any |= af;
        /* Null metadata for value-slot aggs whose input column advertises
         * HAS_NULLS.  Holistic/wide aggs reserve no accum slot (their per-group
         * pass skips nulls itself), so they carry no nullable flag here.
         * COUNT reserves an nv slot for generic bookkeeping (phase1 still
         * stages its raw value) but its emit reads the group's row count
         * (cnt) directly, never off_nn.  Gate on the agg actually owning a
         * value slot (vslot >= 0).  A nullable value-slot agg makes the
         * row-layout accumulators skip nulls (F64 NaN or the type's NULL_I*
         * sentinel) and count non-nulls in the off_nn block. */
        uint8_t af2 = 0;
        int64_t sent = 0;
        int8_t vslot = out->agg_val_slot[a];
        if (agg_ops && (agg_ops[a] == OP_ALL || agg_ops[a] == OP_ANY))
            af2 |= GHT_AF2_TRUTHY;
        if (vslot >= 0 && agg_vecs[a]) {
            ray_t* src = (agg_vecs[a]->attrs & RAY_ATTR_SLICE)
                         ? agg_vecs[a]->slice_parent : agg_vecs[a];
            if (src && (src->attrs & RAY_ATTR_HAS_NULLS)) {
                af2 |= GHT_AF2_NULLABLE;
                sent = agg_int_null_sentinel_for(agg_vecs[a]->type);
                out->any_agg_null = 1;
            }
        }
        out->agg_flags2[a] = af2;
        out->agg_null_sentinel[a] = sent;
    }
    out->n_agg_vals = nv;
    out->agg_flags_any = agg_any;
    /* Null tracking: ceil(n_keys/64) int64 words, floored at 1 so the rare
     * n_keys==0 HT fallback keeps a (trivially-zero) null slot — byte-identical
     * to the legacy single-int64 layout for every ≤64-key shape. */
    uint32_t null_words = n_keys ? ((n_keys + 63u) >> 6) : 1u;
    out->null_words = (uint16_t)null_words;
    /* Key region = keys + null_words*8 null-mask words (stored after last key).
     * The null-mask words hold a bitmap of which keys were null in the source
     * row (word k>>6 bit k&63 = key k is null).  Folding them into hash/memcmp
     * lets null and 0 form distinct groups.  Inline-STR keys are 16 B, all
     * others 8 B; when no inline-STR key is present every offset is k*8 and the
     * single null word sits at n_keys*8 (byte-identical to the legacy fixed-8
     * layout, so non-STR ≤64-key group-bys are unchanged). */
    {
        uint32_t koff = 0;
        for (uint32_t k = 0; k < n_keys; k++) {
            /* stride budget, not a slot count: key_off / key_region are uint16,
             * so the whole key region must fit in 64 KiB. */
            if (koff > UINT16_MAX) { ght_layout_free(out); return false; }
            out->key_off[k] = (uint16_t)koff;
            koff += (out->key_flags[k] & GHT_KEYF_INLINE_STR) ? 16 : 8;
        }
        if (koff > UINT16_MAX) { ght_layout_free(out); return false; }
        out->key_off[n_keys] = (uint16_t)koff;   /* null-mask word 0 */
        koff += null_words * 8;
        if (koff > UINT16_MAX) { ght_layout_free(out); return false; }  /* stride budget, not a slot count */
        out->key_region = (uint16_t)koff;
    }
    uint16_t key_region = out->key_region;
    /* Entry layout: hash | keys | null_mask | agg_vals | [entry_row?]
     * Tail entry_row slot is appended only when any agg is FIRST/LAST,
     * carrying the source-row index needed to merge correctly under
     * work-stealing dispatch (see radix_phase1_fn / accum_from_entry).
     *
     * The agg region is a representational budget, exactly like the key
     * region above: agg_val_slot is int8 (so ≤ INT8_MAX value slots), and
     * entry_stride / row_stride / every off_* are uint16 (so the whole
     * entry and HT row must fit in 64 KiB).  Accumulate the strides in
     * uint32 and REFUSE a layout that would overflow either budget — a
     * silent wrap here scatters aggs to wrong offsets (the key region was
     * already budget-guarded; the agg region was not). */
    bool has_first_last = (agg_any & (GHT_AF_FIRST | GHT_AF_LAST)) != 0;
    uint32_t entry_tail = has_first_last ? 8u : 0u;
    uint32_t entry_stride = 8u + key_region + (uint32_t)nv * 8 + entry_tail;

    uint32_t off = 8u + key_region;
    uint32_t block = (uint32_t)nv * 8;
    if (need_flags & GHT_NEED_SUM)   { out->off_sum   = (uint16_t)off; off += block; }
    if (need_flags & GHT_NEED_MIN)   { out->off_min   = (uint16_t)off; off += block; }
    if (need_flags & GHT_NEED_MAX)   { out->off_max   = (uint16_t)off; off += block; }
    if (need_flags & GHT_NEED_SUMSQ) { out->off_sumsq = (uint16_t)off; off += block; }
    /* Per-slot row-index bounds for FIRST/LAST.  Two int64 blocks of
     * n_agg_vals slots each, allocated only when needed. */
    if (has_first_last) {
        out->off_first_row = (uint16_t)off; off += block;
        out->off_last_row  = (uint16_t)off; off += block;
    }
    /* PEARSON y-side accumulators (Σy, Σy², Σxy).  Allocated when any
     * OP_PEARSON_CORR agg is present.  x-side reuses off_sum + off_sumsq
     * at the same slot index; the y value lives at slot+1 in agg_vals,
     * but its derived accumulators live in their own blocks below. */
    if (need_flags & GHT_NEED_PEARSON) {
        out->off_sum_y   = (uint16_t)off; off += block;
        out->off_sumsq_y = (uint16_t)off; off += block;
        out->off_sumxy   = (uint16_t)off; off += block;
    }
    /* Per-slot non-null count block — only when a nullable agg is present.
     * Null-free shapes leave off_nn == 0 and finalize on the group row count,
     * so their row_stride and layout are byte-identical to before. */
    if (out->any_agg_null) { out->off_nn = (uint16_t)off; off += block; }
    /* Refuse (not wrap) any layout past the int8 value-slot count or the
     * uint16 entry/row-stride budget.  On overflow the off_* casts above
     * stored wrapped junk, but nothing reads it — the layout is freed and
     * the caller sees a clean failure (mirrors the key-region guards). */
    if (nv > INT8_MAX || entry_stride > UINT16_MAX || off > UINT16_MAX) {
        ght_layout_free(out);
        return false;
    }
    out->entry_stride = (uint16_t)entry_stride;
    out->row_stride = (uint16_t)off;
    return true;
}

/* Packed HT slots: [salt:8 | gid:24] in 4 bytes.
 * Max groups per HT = 16M (24 bits) — ample for partitioned probes.
 * 4B slots halve cache footprint vs 8B, fitting HT in L2. */
#define HT_EMPTY    UINT32_MAX
#define HT_PACK(salt, gid)  (((uint32_t)(uint8_t)(salt) << 24) | ((gid) & 0xFFFFFF))
#define HT_GID(s)   ((s) & 0xFFFFFF)
#define HT_SALT_V(s) ((uint8_t)((s) >> 24))

/* group_ht_t defined in exec_internal.h */

/* Aim the HT's wide-key resolution tables (key_data / key_pool) at the inline
 * [8] arrays (n_keys ≤ 8 — byte-identical to the legacy fixed layout) or at one
 * owned heap block carved for wider layouts (unbounded-slots cut 4).  Zeroes the
 * live slots so unset wide keys resolve to NULL.  Returns false on spill OOM. */
static bool group_ht_wire_wide(group_ht_t* ht, uint32_t nk) {
    uint32_t n = nk > 8 ? nk : 8;
    if (nk <= 8) {
        ht->key_wide_hdr = NULL;
        ht->key_data = ht->key_data_in;
        ht->key_pool = ht->key_pool_in;
    } else {
        ht->key_wide_hdr = NULL;
        /* One block: [key_data: nk ptrs][key_pool: nk ptrs].  Per-HT, carved
         * once at init (never per row); freed by group_ht_free. */
        void* blk = scratch_alloc(&ht->key_wide_hdr, (size_t)nk * 2 * sizeof(void*));
        if (!blk) return false;
        ht->key_data = (void**)blk;
        ht->key_pool = (const void**)((void**)blk + nk);
    }
    memset(ht->key_data, 0, (size_t)n * sizeof(void*));
    memset((void*)ht->key_pool, 0, (size_t)n * sizeof(void*));
    return true;
}

static bool group_ht_init_sized(group_ht_t* ht, uint32_t cap,
                                 const ght_layout_t* ly, uint32_t init_grp_cap) {
    ht->ht_cap = cap;
    ht->oom = 0;
    /* By-value embed: re-point the layout's bases at this HT's own inline
     * arrays (inline src) or borrow the shared spill (wide src).  The source
     * layout (owned by the caller's exec_group/pivot master) outlives this HT. */
    ght_layout_copy(&ht->layout, ly);
    /* Wire the wide-key resolution tables (key_data/key_pool base pointers) —
     * inline [8] for ≤8 keys, one owned heap block for wider.  key_data[k]
     * must still be populated by the caller via group_ht_set_key_data whenever
     * any_wide_key != 0. */
    if (!group_ht_wire_wide(ht, ly->n_keys)) return false;
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
    if (!ht->layout.any_wide_key || !kd) return;
    const uint8_t* const kflags = ht->layout.key_flags;
    /* key_data is a base pointer sized to n_keys (inline ≤8, spilled wider). */
    for (uint16_t k = 0; k < ht->layout.n_keys; k++) {
        if (kflags[k] & GHT_KEYF_WIDE) ht->key_data[k] = kd[k];
    }
}

/* Fill out[k] with the string-pool base for each wide STR key (NULL else),
 * derived from the key column's str_pool — paired with key_data[k] for
 * SSO-aware resolution. */
static inline void derive_key_pool(const ght_layout_t* ly, ray_t* const* key_vecs,
                                   const void** out) {
    /* `out` holds n_keys pool slots (group_ht_t base pointer, or a caller
     * buffer sized to n_keys — every derive_key_pool call site provides at
     * least n_keys slots). */
    for (uint16_t k = 0; k < ly->n_keys; k++) {
        out[k] = NULL;
        if ((ly->key_flags[k] & GHT_KEYF_WIDE) && ly->wide_key_type[k] == RAY_STR
            && key_vecs && key_vecs[k]) {
            ray_t* kv = key_vecs[k];
            ray_t* owner = (kv->attrs & RAY_ATTR_SLICE) ? kv->slice_parent : kv;
            ray_t* pool = owner ? owner->str_pool : NULL;
            out[k] = (pool && !RAY_IS_ERR(pool)) ? ray_data(pool) : NULL;
        }
    }
}

/* From key columns (derives the pool). */
static inline void group_ht_set_key_pool(group_ht_t* ht, ray_t* const* key_vecs) {
    if (ht->layout.any_wide_key && key_vecs)
        derive_key_pool(&ht->layout, key_vecs, ht->key_pool);
}
/* From a precomputed pool array (ctxs that carry it but not the key columns). */
static inline void group_ht_copy_key_pool(group_ht_t* ht, const void* const* kp) {
    if (!ht->layout.any_wide_key || !kp) return;
    const uint8_t* const kflags = ht->layout.key_flags;
    /* key_pool is a base pointer sized to n_keys (inline ≤8, spilled wider). */
    for (uint16_t k = 0; k < ht->layout.n_keys; k++)
        if (kflags[k] & GHT_KEYF_WIDE) ht->key_pool[k] = kp[k];
}

void group_ht_free(group_ht_t* ht) {
    scratch_free(ht->_h_slots);
    scratch_free(ht->_h_rows);
    scratch_free(ht->key_wide_hdr);   /* NULL (inline) → no-op */
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

/* ── Wide-key resolution (GUID fixed bytes / STR SSO via pool) ──
 * A wide key's 8-byte slot stores a source row index; these resolve the
 * actual bytes from key_data[k] (+ key_pool[k] for STR) and hash/compare. */
static inline uint64_t wide_key_hash_at(const ght_layout_t* ly, uint8_t k,
                                         void* const* key_data,
                                         const void* const* key_pool, int64_t row) {
    if (ly->wide_key_type[k] == RAY_STR) {
        const ray_str_t* d = &((const ray_str_t*)key_data[k])[row];
        return ray_str_t_hash(d, key_pool ? (const char*)key_pool[k] : NULL);
    }
    uint8_t esz = ly->wide_key_esz[k];
    return ray_hash_bytes((const char*)key_data[k] + (size_t)row * esz, esz);
}

static inline bool wide_key_eq_at(const ght_layout_t* ly, uint8_t k,
                                  void* const* key_data, const void* const* key_pool,
                                  int64_t ra, int64_t rb) {
    if (ra == rb) return true;
    if (ly->wide_key_type[k] == RAY_STR) {
        const ray_str_t* a = &((const ray_str_t*)key_data[k])[ra];
        const ray_str_t* b = &((const ray_str_t*)key_data[k])[rb];
        const char* pool = key_pool ? (const char*)key_pool[k] : NULL;
        return ray_str_t_eq(a, pool, b, pool);
    }
    uint8_t esz = ly->wide_key_esz[k];
    const char* base = (const char*)key_data[k];
    return memcmp(base + (size_t)ra * esz, base + (size_t)rb * esz, esz) == 0;
}

/* Fold the null-mask words [nullw, nullw+null_words) into the running key
 * hash.  Mirrors the historical `if (null_mask) combine` exactly, word-wise:
 * an all-zero word contributes nothing, so a single-word (≤64-key) layout
 * produces the byte-identical hash of the legacy single-int64 null slot. */
static inline uint64_t ght_hash_null_words(uint64_t h, const int64_t* nullw,
                                           uint32_t null_words) {
    for (uint32_t w = 0; w < null_words; w++)
        if (nullw[w]) h = ray_hash_combine(h, ray_hash_i64(nullw[w]));
    return h;
}

/* True when key column kv (or its slice parent) carries the HAS_NULLS attr and
 * so may yield a null at some row.  Replaces the old per-key `1u << k` nullable
 * bitmask (which silently dropped keys past index 7 / was UB past 31): callers
 * hoist a single any_nullable summary out of the row loop, then re-test this
 * per key ONLY on the rare null-bearing path — correct at any key count
 * (unbounded-slots cut 4). */
static inline bool ray_key_may_be_null(const ray_t* kv) {
    if (!kv) return false;
    const ray_t* src = (kv->attrs & RAY_ATTR_SLICE) ? kv->slice_parent : kv;
    return src && (src->attrs & RAY_ATTR_HAS_NULLS);
}

/* ── Inline-STR key resolution (descriptor stored in the entry/row) ──
 * key_inline_str keys hold their 16-byte ray_str_t descriptor at
 * keybase+key_off[k], so hash/compare are cache-local; key_pool[k] (the source
 * pool) is touched only for the full eq/hash of pooled (>12 B) strings. */
static inline uint64_t inline_str_hash(const ght_layout_t* ly, uint8_t k,
                                       const void* keybase,
                                       const void* const* key_pool) {
    const ray_str_t* d = (const ray_str_t*)((const char*)keybase + ly->key_off[k]);
    return ray_str_t_hash(d, key_pool ? (const char*)key_pool[k] : NULL);
}
static inline bool inline_str_eq(const ght_layout_t* ly, uint8_t k,
                                 const void* a_base, const void* b_base,
                                 const void* const* key_pool) {
    const ray_str_t* a = (const ray_str_t*)((const char*)a_base + ly->key_off[k]);
    const ray_str_t* b = (const ray_str_t*)((const char*)b_base + ly->key_off[k]);
    const char* pool = key_pool ? (const char*)key_pool[k] : NULL;
    return ray_str_t_eq(a, pool, b, pool);
}
/* Per-key hash within an inline-STR layout (offsets shifted by 16-byte keys). */
static inline uint64_t inline_layout_key_hash(const ght_layout_t* ly, uint8_t k,
                                              const int8_t* key_types,
                                              const void* keybase, void* const* key_data,
                                              const void* const* key_pool) {
    if (ly->key_flags[k] & GHT_KEYF_INLINE_STR)
        return inline_str_hash(ly, k, keybase, key_pool);
    int64_t v = *(const int64_t*)((const char*)keybase + ly->key_off[k]);
    if (ly->key_flags[k] & GHT_KEYF_WIDE)         /* GUID: v is a source row idx */
        return wide_key_hash_at(ly, k, key_data, key_pool, v);
    if (key_types[k] == RAY_F64) {
        double dv; memcpy(&dv, &v, 8); return ray_hash_f64(dv);
    }
    return ray_hash_i64(v);
}

/* Build the key region of a fat entry for an inline-STR layout (offsets
 * shifted by 16-byte descriptors).  Writes keys + null-mask words at keybase
 * (entry+8 / row+8) and returns the combined hash.  Mirrors the legacy
 * fixed-8 build but addresses every slot via key_off[k]. */
static inline uint64_t inline_build_keys(const ght_layout_t* ly, const int8_t* key_types,
        void* const* key_data, const uint8_t* key_attrs, const void* const* key_pool,
        ray_t* const* key_vecs, uint8_t any_nullable, int64_t row,
        char* keybase) {
    uint64_t h = 0;
    uint16_t nk = ly->n_keys;
    const uint8_t* const kflags = ly->key_flags;
    const uint16_t* const koff = ly->key_off;
    /* Null-mask words live in the key region at koff[nk]; ceil(nk/64) words,
     * so bit k is word (k>>6) bit (k&63).  Written in-place — no scalar
     * accumulator caps the key count at 64. */
    int64_t* nullw = (int64_t*)(keybase + koff[nk]);
    uint32_t null_words = ly->null_words;
    for (uint32_t w = 0; w < null_words; w++) nullw[w] = 0;
    for (uint32_t k = 0; k < nk; k++) {
        char* slot = keybase + koff[k];
        uint64_t kh;
        bool is_null = any_nullable && key_vecs && ray_key_may_be_null(key_vecs[k])
                       && ray_vec_is_null(key_vecs[k], row);
        if (is_null) {
            nullw[k >> 6] |= (int64_t)((uint64_t)1 << (k & 63));
            if (kflags[k] & GHT_KEYF_INLINE_STR) memset(slot, 0, sizeof(ray_str_t));
            else *(int64_t*)slot = 0;
            kh = ray_hash_i64(0);
        } else if (kflags[k] & GHT_KEYF_INLINE_STR) {
            *(ray_str_t*)slot = ((const ray_str_t*)key_data[k])[row];
            kh = ray_str_t_hash((const ray_str_t*)slot,
                                key_pool ? (const char*)key_pool[k] : NULL);
        } else if (kflags[k] & GHT_KEYF_WIDE) {            /* GUID: row index */
            *(int64_t*)slot = row;
            kh = wide_key_hash_at(ly, k, key_data, key_pool, row);
        } else if (key_types[k] == RAY_F64) {
            *(int64_t*)slot = ((const int64_t*)key_data[k])[row];
            kh = ray_hash_f64(((const double*)key_data[k])[row]);
        } else {
            int64_t kv = read_col_i64(key_data[k], row, key_types[k], key_attrs[k]);
            *(int64_t*)slot = kv;
            kh = ray_hash_i64(kv);
        }
        h = (k == 0) ? kh : ray_hash_combine(h, kh);
    }
    return ght_hash_null_words(h, nullw, null_words);
}

/* Hash inline int64_t keys (for rehash — resolves wide keys via
 * the HT's key_data pointers). */
static inline uint64_t hash_keys_inline(const int64_t* keys, const int8_t* key_types,
                                         uint16_t n_keys, void* const* key_data,
                                         const ght_layout_t* ly,
                                         const void* const* key_pool) {
    if (ly->any_inline_str) {
        /* Inline-STR layout: key offsets are shifted by 16-byte descriptors,
         * so address every key via key_off[k] and hash the inline descriptor. */
        uint64_t h = 0;
        for (uint32_t k = 0; k < n_keys; k++) {
            uint64_t kh = inline_layout_key_hash(ly, k, key_types, keys, key_data, key_pool);
            h = (k == 0) ? kh : ray_hash_combine(h, kh);
        }
        const int64_t* nullw = (const int64_t*)((const char*)keys + ly->key_off[n_keys]);
        return ght_hash_null_words(h, nullw, ly->null_words);
    }
    const uint8_t* const kflags = ly->key_flags;
    uint64_t h = 0;
    for (uint32_t k = 0; k < n_keys; k++) {
        uint64_t kh;
        if (kflags[k] & GHT_KEYF_WIDE) {
            /* Wide key: keys[k] is the source row index.  Resolve + hash the
             * actual bytes (GUID fixed / STR SSO via pool). */
            kh = wide_key_hash_at(ly, k, key_data, key_pool, keys[k]);
        } else if (key_types[k] == RAY_F64) {
            double dv;
            memcpy(&dv, &keys[k], 8);
            kh = ray_hash_f64(dv);
        } else {
            kh = ray_hash_i64(keys[k]);
        }
        h = (k == 0) ? kh : ray_hash_combine(h, kh);
    }
    /* Fold null-mask words (at slot n_keys) into hash so null/0 form distinct
     * groups.  Non-inline keys are 8 B each, so word w is keys[n_keys+w]. */
    return ght_hash_null_words(h, keys + n_keys, ly->null_words);
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
    uint16_t nk = ht->layout.n_keys;
    for (uint32_t gi = 0; gi < ht->grp_count; gi++) {
        const int64_t* row_keys = (const int64_t*)(ht->rows + (size_t)gi * rs + 8);
        uint64_t h = hash_keys_inline(row_keys, key_types, nk, ht->key_data,
                                       &ht->layout, ht->key_pool);
        uint32_t slot = (uint32_t)(h & mask);
        while (ht->slots[slot] != HT_EMPTY)
            slot = (slot + 1) & mask;
        ht->slots[slot] = HT_PACK(HT_SALT(h), gi);
    }
}

static void group_ht_rebuild_slots(group_ht_t* ht, const int8_t* key_types) {
    if (!ht->slots || ht->ht_cap == 0) return;
    memset(ht->slots, 0xFF, (size_t)ht->ht_cap * sizeof(uint32_t));
    uint32_t mask = ht->ht_cap - 1;
    uint16_t rs = ht->layout.row_stride;
    uint16_t nk = ht->layout.n_keys;
    for (uint32_t gi = 0; gi < ht->grp_count; gi++) {
        const int64_t* row_keys = (const int64_t*)(ht->rows + (size_t)gi * rs + 8);
        uint64_t h = hash_keys_inline(row_keys, key_types, nk, ht->key_data,
                                      &ht->layout, ht->key_pool);
        uint32_t slot = (uint32_t)(h & mask);
        while (ht->slots[slot] != HT_EMPTY)
            slot = (slot + 1) & mask;
        ht->slots[slot] = HT_PACK(HT_SALT(h), gi);
    }
}

/* Null-aware accumulator variants (defined below).  Reached only when the
 * layout carries a nullable agg (ly->any_agg_null) — a single hoisted,
 * perfectly-predicted branch, so the null-free hot path is byte-for-byte
 * unchanged. */
static void init_accum_from_entry_nullable(char* row, const char* entry,
                                            const ght_layout_t* ly);
static void accum_from_entry_nullable(char* row, const char* entry,
                                      const ght_layout_t* ly);

/* Initialize accumulators for a new group from entry's inline agg values.
 * Each unified block has n_agg_vals slots of 8 bytes, typed by agg_is_f64. */
static inline void init_accum_from_entry(char* row, const char* entry,
                                          const ght_layout_t* ly) {
    if (ly->any_agg_null) { init_accum_from_entry_nullable(row, entry, ly); return; }
    /* key_region-based (mirrors init_accum_from_entry_nullable): the old
     * `8 + (n_keys+1)*8` assumed exactly one trailing null-mask word.  For
     * null_words > 1 (n_keys > 64) that understates the accumulator start
     * by (null_words-1)*8 bytes, so this memset's zero-fill reached
     * backward into the last null-mask word — clobbering it to 0
     * immediately after group_probe_entry's memcpy stored it correctly.
     * Not structurally invisible to plain GROUP BY — the radix phase-3
     * scatter DOES re-read these same null words straight out of the HT row
     * (radix_phase3_fn) — but empirically it surfaced first through PIVOT:
     * no pre-existing GROUP-BY test exercised a >64-key null in the trailing
     * null-mask word, whereas PIVOT's own ix-dedupe/emit code (pivot.c) reads
     * the null words directly back out of these rows and its 65-index
     * conflation-detector cell (tblop_branch_cov.rfl) caught the clobber.
     * The 65-key null GROUP cell (width_matrix.rfl) now covers the GROUP
     * side of the same fix. */
    uint16_t accum_start = (uint16_t)(8 + ly->key_region);
    if (ly->row_stride > accum_start)
        memset(row + accum_start, 0, ly->row_stride - accum_start);

    const char* agg_data = entry + 8 + (size_t)ly->key_region;
    uint16_t na = ly->n_aggs;
    uint8_t nf = ly->need_flags;
    bool has_fl = (ly->agg_flags_any & (GHT_AF_FIRST | GHT_AF_LAST)) != 0;
    /* Entry tail slot carries the source-row index when has_fl. */
    int64_t entry_row = 0;
    if (has_fl)
        memcpy(&entry_row, entry + ly->entry_stride - 8, 8);

    /* Hoist the per-agg base pointers to const locals before the row loop
     * (cut-3 register-promotion doctrine): each agg reads one agg_flags[a]
     * byte (register bit-tests) instead of several distinct in-struct mask
     * loads. */
    const int8_t*  const vslot  = ly->agg_val_slot;
    const uint8_t* const aflags = ly->agg_flags;
    const uint8_t* const aflags2 = ly->agg_flags2;
    for (uint32_t a = 0; a < na; a++) {
        int8_t s = vslot[a];
        if (s < 0) continue;
        uint8_t af = aflags[a];
        /* Copy raw 8 bytes from entry into each enabled accumulator block */
        if (nf & GHT_NEED_SUM) {
            if (aflags2[a] & GHT_AF2_TRUTHY) {
                int64_t tv;
                if (af & GHT_AF_F64) { double v; memcpy(&v, agg_data + s * 8, 8); tv = (v != 0.0); }
                else { int64_t v; memcpy(&v, agg_data + s * 8, 8); tv = (v != 0); }
                memcpy(row + ly->off_sum + s * 8, &tv, 8);
            } else {
                memcpy(row + ly->off_sum + s * 8, agg_data + s * 8, 8);
            }
        }
        if (nf & GHT_NEED_MIN) memcpy(row + ly->off_min + s * 8, agg_data + s * 8, 8);
        if (nf & GHT_NEED_MAX) memcpy(row + ly->off_max + s * 8, agg_data + s * 8, 8);
        if (nf & GHT_NEED_SUMSQ) {
            /* sumsq = v * v for the first entry */
            if (af & GHT_AF_F64) {
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
        if ((nf & GHT_NEED_PEARSON) && (af & GHT_AF_BINARY)) {
            double x, y;
            if (af & GHT_AF_F64) {
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
    if (ly->any_agg_null) { accum_from_entry_nullable(row, entry, ly); return; }
    const char* agg_data = entry + 8 + (size_t)ly->key_region;
    uint16_t na = ly->n_aggs;
    uint8_t nf = ly->need_flags;
    /* Entry's source-row index — only present when any agg is FIRST/LAST.
     * Pool dispatch is work-stealing (atomic_fetch_add), so phase1 may
     * scatter entries into radix bufs out of source-row order; reading
     * the row index from the entry restores the absolute ordering that
     * "keep init / always overwrite" assumed. */
    bool has_fl = (ly->agg_flags_any & (GHT_AF_FIRST | GHT_AF_LAST)) != 0;
    int64_t entry_row = 0;
    if (has_fl)
        memcpy(&entry_row, entry + ly->entry_stride - 8, 8);

    /* Hoist the per-agg base pointers to const locals BEFORE the row loop —
     * the single most-exposed layout read in the engine (once per agg per
     * aggregating row).  Each agg loads one agg_flags[a] byte and bit-tests
     * it in registers, replacing the old six distinct in-struct mask loads
     * (agg_is_f64/first/last/prod/sym/binary). */
    const int8_t*  const vslot  = ly->agg_val_slot;
    const uint8_t* const aflags = ly->agg_flags;
    const uint8_t* const aflags2 = ly->agg_flags2;
    struct ray_sym_domain_s* const* const adom = ly->agg_dom;
    for (uint32_t a = 0; a < na; a++) {
        int8_t s = vslot[a];
        if (s < 0) continue;
        const char* val = agg_data + s * 8;

        uint8_t af = aflags[a];
        bool take_first = false, take_last = false;
        if (has_fl && (af & GHT_AF_FIRST)) {
            int64_t fr; memcpy(&fr, row + ly->off_first_row + s * 8, 8);
            take_first = (entry_row < fr);
        }
        if (has_fl && (af & GHT_AF_LAST)) {
            int64_t lr; memcpy(&lr, row + ly->off_last_row + s * 8, 8);
            take_last = (entry_row > lr);
        }
        if (af & GHT_AF_F64) {
            double v;
            memcpy(&v, val, 8);
            if (nf & GHT_NEED_SUM) {
                if (af & GHT_AF_FIRST) { if (take_first) memcpy(row + ly->off_sum + s * 8, val, 8); }
                else if (af & GHT_AF_LAST) { if (take_last) memcpy(row + ly->off_sum + s * 8, val, 8); }
                else if (aflags2[a] & GHT_AF2_TRUTHY) { ROW_WR_I64(row, ly->off_sum, s) += (v != 0.0); }
                else if (af & GHT_AF_PROD) { ROW_WR_F64(row, ly->off_sum, s) *= v; }
                else { ROW_WR_F64(row, ly->off_sum, s) += v; }
            }
            if (nf & GHT_NEED_MIN) { double* p = &ROW_WR_F64(row, ly->off_min, s); if (v < *p) *p = v; }
            if (nf & GHT_NEED_MAX) { double* p = &ROW_WR_F64(row, ly->off_max, s); if (v > *p) *p = v; }
            if (nf & GHT_NEED_SUMSQ) { ROW_WR_F64(row, ly->off_sumsq, s) += v * v; }
            /* PEARSON y-side: accumulate Σy, Σy², Σxy.  v above is x. */
            if ((nf & GHT_NEED_PEARSON) && (af & GHT_AF_BINARY)) {
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
                if (af & GHT_AF_FIRST) { if (take_first) memcpy(row + ly->off_sum + s * 8, val, 8); }
                else if (af & GHT_AF_LAST) { if (take_last) memcpy(row + ly->off_sum + s * 8, val, 8); }
                else if (aflags2[a] & GHT_AF2_TRUTHY) { ROW_WR_I64(row, ly->off_sum, s) += (v != 0); }
                else if (af & GHT_AF_PROD) { ROW_WR_I64(row, ly->off_sum, s) = (int64_t)((uint64_t)ROW_RD_I64(row, ly->off_sum, s) * (uint64_t)v); }
                else { ROW_WR_I64(row, ly->off_sum, s) += v; }
            }
            if (nf & GHT_NEED_MIN) {
                int64_t* p = &ROW_WR_I64(row, ly->off_min, s);
                if (af & GHT_AF_SYM) {
                    if (*p == INT64_MAX || sym_lex_lt(adom[a], v, *p)) *p = v;
                } else if (v < *p) *p = v;
            }
            if (nf & GHT_NEED_MAX) {
                int64_t* p = &ROW_WR_I64(row, ly->off_max, s);
                if (af & GHT_AF_SYM) {
                    if (*p == INT64_MIN || sym_lex_gt(adom[a], v, *p)) *p = v;
                } else if (v > *p) *p = v;
            }
            if (nf & GHT_NEED_SUMSQ) { ROW_WR_F64(row, ly->off_sumsq, s) += (double)v * (double)v; }
            /* PEARSON y-side (i64 input branch): y was packed via
             * read_col_i64 — reinterpret as int64 then cast to double. */
            if ((nf & GHT_NEED_PEARSON) && (af & GHT_AF_BINARY)) {
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

/* ── Null-aware row-layout accumulators ──────────────────────────────────
 * Reached only when the layout carries a nullable agg input.  They skip
 * null values (F64 NaN, or the per-agg NULL_I* sentinel for integer/temporal
 * columns) and maintain a per-slot non-null count in the off_nn block, so
 * finalize divides AVG/VAR/STDDEV by the non-null count and emits a typed
 * null for an all-null group — identical semantics to the DA and scalar
 * paths (agg_int_null_mask + nn_count).  A new group seeds accumulator
 * IDENTITIES (SUM 0 / PROD 1 / MIN +max / MAX −max) rather than the opening
 * value, so a null opening row neither poisons MIN/MAX/FIRST/LAST nor is
 * summed. */
static inline bool agg_entry_is_null(uint8_t af, uint8_t af2, int64_t sentinel,
                                     const char* val) {
    if (!(af2 & GHT_AF2_NULLABLE)) return false;
    if (af & GHT_AF_F64) { double v; memcpy(&v, val, 8); return v != v; }
    int64_t v; memcpy(&v, val, 8); return v == sentinel;
}

static void accum_from_entry_nullable(char* row, const char* entry,
                                      const ght_layout_t* ly) {
    const char* agg_data = entry + 8 + (size_t)ly->key_region;
    uint16_t na = ly->n_aggs;
    uint8_t nf = ly->need_flags;
    bool has_fl = (ly->agg_flags_any & (GHT_AF_FIRST | GHT_AF_LAST)) != 0;
    int64_t entry_row = 0;
    if (has_fl)
        memcpy(&entry_row, entry + ly->entry_stride - 8, 8);
    const int8_t*  const vslot  = ly->agg_val_slot;
    const uint8_t* const aflags = ly->agg_flags;
    const uint8_t* const aflags2 = ly->agg_flags2;
    const int64_t* const asent  = ly->agg_null_sentinel;
    struct ray_sym_domain_s* const* const adom = ly->agg_dom;
    int64_t* const nn = (int64_t*)(void*)(row + ly->off_nn);
    for (uint32_t a = 0; a < na; a++) {
        int8_t s = vslot[a];
        if (s < 0) continue;
        const char* val = agg_data + s * 8;
        uint8_t af = aflags[a];
        if (agg_entry_is_null(af, aflags2[a], asent[a], val))
            continue;                     /* null: no accumulate, no nn++ */
        nn[s]++;
        if (af & GHT_AF_F64) {
            double v; memcpy(&v, val, 8);
            if (nf & GHT_NEED_SUM) {
                if (af & GHT_AF_FIRST) {
                    if (entry_row < ROW_RD_I64(row, ly->off_first_row, s)) {
                        memcpy(row + ly->off_sum + s * 8, val, 8);
                        ROW_WR_I64(row, ly->off_first_row, s) = entry_row;
                    }
                } else if (af & GHT_AF_LAST) {
                    if (entry_row > ROW_RD_I64(row, ly->off_last_row, s)) {
                        memcpy(row + ly->off_sum + s * 8, val, 8);
                        ROW_WR_I64(row, ly->off_last_row, s) = entry_row;
                    }
                } else if (aflags2[a] & GHT_AF2_TRUTHY) { ROW_WR_I64(row, ly->off_sum, s) += (v != 0.0); }
                else if (af & GHT_AF_PROD) { ROW_WR_F64(row, ly->off_sum, s) *= v; }
                else { ROW_WR_F64(row, ly->off_sum, s) += v; }
            }
            if (nf & GHT_NEED_MIN) { double* p = &ROW_WR_F64(row, ly->off_min, s); if (v < *p) *p = v; }
            if (nf & GHT_NEED_MAX) { double* p = &ROW_WR_F64(row, ly->off_max, s); if (v > *p) *p = v; }
            if (nf & GHT_NEED_SUMSQ) { ROW_WR_F64(row, ly->off_sumsq, s) += v * v; }
            if ((nf & GHT_NEED_PEARSON) && (af & GHT_AF_BINARY)) {
                double y; memcpy(&y, agg_data + (s + 1) * 8, 8);
                ROW_WR_F64(row, ly->off_sum_y,   s) += y;
                ROW_WR_F64(row, ly->off_sumsq_y, s) += y * y;
                ROW_WR_F64(row, ly->off_sumxy,   s) += v * y;
            }
        } else {
            int64_t v; memcpy(&v, val, 8);
            if (nf & GHT_NEED_SUM) {
                if (af & GHT_AF_FIRST) {
                    if (entry_row < ROW_RD_I64(row, ly->off_first_row, s)) {
                        memcpy(row + ly->off_sum + s * 8, val, 8);
                        ROW_WR_I64(row, ly->off_first_row, s) = entry_row;
                    }
                } else if (af & GHT_AF_LAST) {
                    if (entry_row > ROW_RD_I64(row, ly->off_last_row, s)) {
                        memcpy(row + ly->off_sum + s * 8, val, 8);
                        ROW_WR_I64(row, ly->off_last_row, s) = entry_row;
                    }
                } else if (aflags2[a] & GHT_AF2_TRUTHY) { ROW_WR_I64(row, ly->off_sum, s) += (v != 0); }
                else if (af & GHT_AF_PROD) { ROW_WR_I64(row, ly->off_sum, s) = (int64_t)((uint64_t)ROW_RD_I64(row, ly->off_sum, s) * (uint64_t)v); }
                else { ROW_WR_I64(row, ly->off_sum, s) += v; }
            }
            if (nf & GHT_NEED_MIN) {
                int64_t* p = &ROW_WR_I64(row, ly->off_min, s);
                if (af & GHT_AF_SYM) { if (*p == INT64_MAX || sym_lex_lt(adom[a], v, *p)) *p = v; }
                else if (v < *p) *p = v;
            }
            if (nf & GHT_NEED_MAX) {
                int64_t* p = &ROW_WR_I64(row, ly->off_max, s);
                if (af & GHT_AF_SYM) { if (*p == INT64_MIN || sym_lex_gt(adom[a], v, *p)) *p = v; }
                else if (v > *p) *p = v;
            }
            if (nf & GHT_NEED_SUMSQ) { ROW_WR_F64(row, ly->off_sumsq, s) += (double)v * (double)v; }
            if ((nf & GHT_NEED_PEARSON) && (af & GHT_AF_BINARY)) {
                int64_t yi; memcpy(&yi, agg_data + (s + 1) * 8, 8);
                double y = (double)yi; double xd = (double)v;
                ROW_WR_F64(row, ly->off_sum_y,   s) += y;
                ROW_WR_F64(row, ly->off_sumsq_y, s) += y * y;
                ROW_WR_F64(row, ly->off_sumxy,   s) += xd * y;
            }
        }
    }
}

static void init_accum_from_entry_nullable(char* row, const char* entry,
                                            const ght_layout_t* ly) {
    /* Zero the entire accumulator region (incl. off_nn); key_region-based
     * start is correct for every key shape (inline STR, >1 null word). */
    uint16_t accum_start = (uint16_t)(8 + ly->key_region);
    if (ly->row_stride > accum_start)
        memset(row + accum_start, 0, ly->row_stride - accum_start);

    uint16_t na = ly->n_aggs;
    uint8_t nf = ly->need_flags;
    bool has_fl = (ly->agg_flags_any & (GHT_AF_FIRST | GHT_AF_LAST)) != 0;
    const int8_t*  const vslot  = ly->agg_val_slot;
    const uint8_t* const aflags = ly->agg_flags;
    for (uint32_t a = 0; a < na; a++) {
        int8_t s = vslot[a];
        if (s < 0) continue;
        uint8_t af = aflags[a];
        /* Identities: MIN +max, MAX −max, PROD 1.  SUM / SUMSQ / nn / y-side
         * stay 0 from the memset above.  Row-index bounds seed so any
         * non-null row beats them. */
        if (nf & GHT_NEED_MIN) {
            if (af & GHT_AF_F64) ROW_WR_F64(row, ly->off_min, s) = DBL_MAX;
            else                 ROW_WR_I64(row, ly->off_min, s) = INT64_MAX;
        }
        if (nf & GHT_NEED_MAX) {
            if (af & GHT_AF_F64) ROW_WR_F64(row, ly->off_max, s) = -DBL_MAX;
            else                 ROW_WR_I64(row, ly->off_max, s) = INT64_MIN;
        }
        if ((nf & GHT_NEED_SUM) && (af & GHT_AF_PROD)) {
            if (af & GHT_AF_F64) ROW_WR_F64(row, ly->off_sum, s) = 1.0;
            else                 ROW_WR_I64(row, ly->off_sum, s) = 1;
        }
        if (has_fl) {
            ROW_WR_I64(row, ly->off_first_row, s) = INT64_MAX;
            ROW_WR_I64(row, ly->off_last_row,  s) = INT64_MIN;
        }
    }
    /* Fold the opening row in through the same null-aware update. */
    accum_from_entry_nullable(row, entry, ly);
}

/* Compare the n_keys key slots of two rows, handling wide keys via
 * key_data[] resolution.  Returns true if all keys are bytewise equal.
 * Hot path: when wide_mask == 0, reduces to a single memcmp over the
 * packed 8-byte-per-key region. */
static inline bool group_keys_equal(const int64_t* a_keys, const int64_t* b_keys,
                                      const ght_layout_t* ly, void* const* key_data,
                                      const void* const* key_pool) {
    uint16_t nk = ly->n_keys;
    if (!ly->any_wide_key) {
        /* memcmp covers nk 8-byte values + the null_words trailing words:
         * key_region == (nk + null_words)*8 with no wide/inline-STR keys, so
         * the compare length widens with the null region — no shape change. */
        return memcmp(a_keys, b_keys, (size_t)ly->key_region) == 0;
    }
    const uint8_t* const kflags = ly->key_flags;
    const uint16_t* const koff = ly->key_off;
    if (ly->any_inline_str) {
        /* Inline-STR layout: address every key via key_off[k]; STR keys compare
         * their inline descriptors cache-locally (pool only for >12 B). */
        const char* a = (const char*)a_keys;
        const char* b = (const char*)b_keys;
        for (uint32_t k = 0; k < nk; k++) {
            if (kflags[k] & GHT_KEYF_INLINE_STR) {
                if (!inline_str_eq(ly, k, a_keys, b_keys, key_pool)) return false;
            } else if (kflags[k] & GHT_KEYF_WIDE) {  /* GUID: row indices */
                int64_t ra = *(const int64_t*)(a + koff[k]);
                int64_t rb = *(const int64_t*)(b + koff[k]);
                if (!wide_key_eq_at(ly, k, key_data, key_pool, ra, rb)) return false;
            } else {
                if (*(const int64_t*)(a + koff[k]) !=
                    *(const int64_t*)(b + koff[k])) return false;
            }
        }
        return memcmp(a + koff[nk], b + koff[nk],
                      (size_t)ly->null_words * 8) == 0;
    }
    for (uint32_t k = 0; k < nk; k++) {
        if (kflags[k] & GHT_KEYF_WIDE) {
            if (!wide_key_eq_at(ly, k, key_data, key_pool, a_keys[k], b_keys[k]))
                return false;
        } else {
            if (a_keys[k] != b_keys[k]) return false;
        }
    }
    /* Null-mask words must match too (non-inline keys are 8 B, so the words
     * start at slot nk). */
    if (memcmp(a_keys + nk, b_keys + nk, (size_t)ly->null_words * 8) != 0)
        return false;
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
    uint16_t key_bytes = ly->key_region;  /* keys + null mask (incl. inline STR descriptors) */

    /* For count-only queries (no SUM/MIN/MAX/SUMSQ/PEARSON aggregator
     * state, no FIRST/LAST row tracking, no binary aggregator y-side)
     * init_accum_from_entry and accum_from_entry are no-ops on every
     * non-count slot — the per-row call still iterates n_aggs slots,
     * reads agg_val_slot[a], memcpy's the entry's agg value into a
     * local, then drops it.  That's ~6 ns / row × n_keys=1 millions of
     * rows, ~7 ms wall on q15.  Skip the call when none of the flags
     * that drive its writes are set. */
    uint8_t accum_skip = (ly->need_flags == 0
        && (ly->agg_flags_any & (GHT_AF_FIRST | GHT_AF_LAST | GHT_AF_BINARY)) == 0);
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
            if (!accum_skip)
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
                                  (const int64_t*)ekeys, ly, ht->key_data, ht->key_pool)) {
                (*(int64_t*)row)++;   /* count++ */
                if (!accum_skip)
                    accum_from_entry(row, entry, ly);
                return mask;
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
    uint16_t nk = ly->n_keys;
    uint16_t na = ly->n_aggs;
    /* Hoisted per-key / per-agg flag bases + scalar shape guards (cut-3
     * register-promotion doctrine): the common int/sym-key numeric path has
     * both guards 0, so the wide/inline/holistic per-slot branches short-
     * circuit on a register test without touching the flag arrays. */
    const uint8_t* const kflags = ly->key_flags;
    const uint8_t* const aflags = ly->agg_flags;
    uint8_t wide_any = ly->any_wide_key;
    bool has_fl = (ly->agg_flags_any & (GHT_AF_FIRST | GHT_AF_LAST)) != 0;
    uint32_t mask = ht->ht_cap - 1;
    /* Stack buffer for one entry: hash + (nk+1) key slots + nv agg_vals
     * + optional 8-byte source-row tail (FIRST/LAST).  The ≤8-key/≤8-agg
     * layout fits 8 + 9*8 + 8*8 + 8 = 152 bytes (byte-identical to the legacy
     * fixed buffer); a wider layout spills to one per-call heap block sized
     * to entry_stride (carved once here, never per row — unbounded-slots
     * cut 4). */
    char ebuf_stk[8 + 9 * 8 + 8 * 8 + 8];
    char* ebuf = ebuf_stk;
    ray_t* ebuf_hdr = NULL;
    if (ly->entry_stride > sizeof(ebuf_stk)) {
        ebuf = (char*)scratch_alloc(&ebuf_hdr, ly->entry_stride);
        if (!ebuf) { ht->oom = 1; return; }
    }

    /* Any key column that can produce nulls (parent vec's HAS_NULLS attr for
     * slices)?  When none, every per-row null check short-circuits on this
     * register test (common fast path).  A precomputed per-key bitmask is
     * avoided so key indices past 63 track correctly (unbounded-slots cut 4);
     * with any_nullable set, the per-key nullability is re-derived cheaply
     * from the key vec's attr (only on the rare null-bearing path). */
    uint8_t any_nullable = 0;
    for (uint32_t k = 0; k < nk; k++)
        if (key_vecs && ray_key_may_be_null(key_vecs[k])) { any_nullable = 1; break; }

    /* Wire the HT's key_data + key_pool tables so probe/rehash can
     * resolve wide keys (GUID bytes / STR descriptors) via the source columns. */
    if (wide_any) { group_ht_set_key_data(ht, key_data); group_ht_set_key_pool(ht, key_vecs); }

    for (int64_t i = start; i < end; i++) {
        /* Cancellation checkpoint every 65536 rows — ~150 polls on a
         * 10M-row ingest, imperceptible in the inner loop and still
         * sub-100ms response time on Ctrl-C. */
        if (((i - start) & 65535) == 0 && ray_interrupted()) break;
        int64_t row = match_idx ? match_idx[i] : i;
        if (!match_idx && rowsel && !group_rowsel_pass(rowsel, row)) continue;
        uint64_t h = 0;
        int64_t* ek = (int64_t*)(ebuf + 8);
        if (ly->any_inline_str) {
            h = inline_build_keys(ly, key_types, key_data, key_attrs, ht->key_pool,
                                  key_vecs, any_nullable, row, ebuf + 8);
        } else {
        /* Non-inline keys are 8 B each, so the null-mask words start at ek[nk]
         * (== ebuf+8+key_off[nk]); write bit k into word k>>6 in place. */
        int64_t* nullw = ek + nk;
        uint32_t null_words = ly->null_words;
        for (uint32_t w = 0; w < null_words; w++) nullw[w] = 0;
        for (uint32_t k = 0; k < nk; k++) {
            int8_t t = key_types[k];
            uint64_t kh;
            bool is_null = any_nullable && ray_key_may_be_null(key_vecs[k])
                           && ray_vec_is_null(key_vecs[k], row);
            if (is_null) {
                nullw[k >> 6] |= (int64_t)((uint64_t)1 << (k & 63));
                ek[k] = 0;  /* canonical null value — real 0 differs via null mask */
                kh = ray_hash_i64(0);
            } else if (wide_any && (kflags[k] & GHT_KEYF_WIDE)) {
                /* Wide key: store source row index, hash the actual bytes
                 * (GUID fixed / STR SSO via pool). */
                ek[k] = row;
                kh = wide_key_hash_at(ly, k, key_data, ht->key_pool, row);
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
        h = ght_hash_null_words(h, nullw, null_words);
        }
        *(uint64_t*)ebuf = h;

        int64_t* ev = (int64_t*)(ebuf + 8 + (size_t)ly->key_region);
        uint8_t vi = 0;
        for (uint32_t a = 0; a < na; a++) {
            uint8_t af = aflags[a];
            /* Holistic agg (OP_MEDIAN): no slot reserved — skip packing.
             * Source column read in the post-radix pass. */
            if (af & GHT_AF_HOLISTIC) continue;
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
            if ((af & GHT_AF_BINARY) && agg_vecs2 && agg_vecs2[a]) {
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
    scratch_free(ebuf_hdr);   /* NULL (inline ebuf) → no-op */
}

/* ============================================================================
 * Radix-partitioned parallel group-by
 *
 * Pass 1 (parallel): Each worker reads keys+agg values from original columns,
 *         packs into fat entries (hash, keys, agg_vals), scatters into
 *         thread-local per-partition buffers.
 * Pass 2 (parallel): Each partition is aggregated independently using
 *         inline data — no original column access needed.
 * Pass 3: Build result columns from inline group rows.
 * ============================================================================ */

#define RADIX_BITS  8
#define RADIX_P     (1u << RADIX_BITS)   /* 256 partitions */
#define RADIX_MASK  (RADIX_P - 1)
#define RADIX_PART(h) (((uint32_t)((h) >> 16)) & RADIX_MASK)

/* Selection-aware group iteration gate.  When a WHERE leaves fewer than
 * nrows >> SEL_MATCH_GATE_SHIFT survivors, the high-card group build iterates
 * the survivor row list (match_idx) instead of scanning all nrows with a
 * per-row rowsel check.  Dense selections keep the sequential scan (a large
 * survivor array + scattered row access would not pay off).  Shift of 1 =
 * "fewer than half the rows survive"; tuned in Task 2. */
#define SEL_MATCH_GATE_SHIFT 1

/* Per-worker, per-partition buffer of fat entries */
typedef struct {
    char*    data;           /* flat buffer: data[i * entry_stride] */
    uint32_t count;
    uint32_t cap;
    bool     oom;            /* set on realloc failure */
    ray_t*    _hdr;
} radix_buf_t;

/* key_region_buf holds the full prebuilt key region (keys + null-mask words),
 * contiguous, key_region bytes — inline-STR and plain layouts both stage it
 * this way, so one memcpy serves both. */
static inline void radix_buf_push(radix_buf_t* buf, uint16_t entry_stride,
                                   uint64_t hash, const int64_t* key_region_buf,
                                   const int64_t* agg_vals, uint16_t n_agg_vals,
                                   bool has_first_last, int64_t row,
                                   uint16_t key_region) {
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
    memcpy(dst + 8, key_region_buf, key_region);
    if (n_agg_vals)
        memcpy(dst + 8 + key_region, agg_vals, (size_t)n_agg_vals * 8);
    /* Tail slot: source row index for FIRST/LAST tie-breaking. */
    if (has_first_last)
        memcpy(dst + entry_stride - 8, &row, 8);
    buf->count++;
}

typedef struct {
    void**       key_data;
    const void** key_pool;        /* [n_keys] str-pool base per wide STR key (NULL else) */
    int8_t*      key_types;
    uint8_t*     key_attrs;
    ray_t**      key_vecs;
    uint8_t      nullable_mask;   /* 0/1: any key may be null (see build) */
    ray_t**       agg_vecs;
    /* Second input column per agg; NULL when no binary aggs in this
     * OP_GROUP.  Pass 1 reads agg_vecs2[a] alongside agg_vecs[a] and
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
    uint16_t nk = ly->n_keys;
    uint16_t na = ly->n_aggs;
    uint16_t nv = ly->n_agg_vals;
    const uint8_t* const kflags = ly->key_flags;
    const uint8_t* const aflags = ly->agg_flags;
    uint8_t wide_any = ly->any_wide_key;
    uint8_t inline_str = ly->any_inline_str;
    uint16_t estride = ly->entry_stride;
    bool has_fl = (ly->agg_flags_any & (GHT_AF_FIRST | GHT_AF_LAST)) != 0;
    const int64_t* match_idx = c->match_idx;

    /* Per-worker key/agg staging.  The ≤8-key/≤8-agg layout stages through
     * stack arrays (keys: 8 keys + 1 null word; agg_vals: 8 slots; keybuf:
     * 8*16 + 8 inline-STR region) — byte-identical to the legacy fixed
     * buffers.  A wider layout carves one per-worker heap block ONCE here
     * (never per row — unbounded-slots cut 4): keybuf | keys | agg_vals. */
    int64_t keys_stk[9];
    int64_t agg_vals_stk[8];
    char    keybuf_stk[136];
    int64_t* keys = keys_stk;
    int64_t* agg_vals = agg_vals_stk;
    char*    keybuf = keybuf_stk;
    ray_t*   stage_hdr = NULL;
    if (ly->key_region > sizeof(keys_stk) || nv > 8) {
        size_t kb = ly->key_region;
        char* blk = (char*)scratch_alloc(&stage_hdr, kb + kb + (size_t)nv * 8);
        if (!blk) return;   /* OOM on a pathologically wide layout: drop worker */
        keybuf   = blk;
        keys     = (int64_t*)(blk + kb);
        agg_vals = (int64_t*)(blk + kb + kb);
    }

    uint8_t nullable = c->nullable_mask;   /* 0/1: any key may be null (see build) */
    for (int64_t i = start; i < end; i++) {
        /* Cancellation checkpoint every 65536 rows — ~150 polls on a
         * 10M-row ingest, imperceptible in the inner loop and still
         * sub-100ms response time on Ctrl-C. */
        if (((i - start) & 65535) == 0 && ray_interrupted()) break;
        int64_t row = match_idx ? match_idx[i] : i;
        if (!match_idx && c->rowsel && !group_rowsel_pass(c->rowsel, row)) continue;
        uint64_t h = 0;
        if (inline_str) {
            h = inline_build_keys(ly, c->key_types, c->key_data, c->key_attrs,
                                  c->key_pool, c->key_vecs, nullable, row,
                                  keybuf);
        } else {
        /* Stage keys + null-mask words contiguously (key-region shaped) so the
         * push is one memcpy; null words start at keys[nk]. */
        int64_t* nullw = keys + nk;
        uint32_t null_words = ly->null_words;
        for (uint32_t w = 0; w < null_words; w++) nullw[w] = 0;
        for (uint32_t k = 0; k < nk; k++) {
            int8_t t = c->key_types[k];
            uint64_t kh;
            bool is_null = nullable && ray_key_may_be_null(c->key_vecs[k])
                           && ray_vec_is_null(c->key_vecs[k], row);
            if (is_null) {
                nullw[k >> 6] |= (int64_t)((uint64_t)1 << (k & 63));
                keys[k] = 0;
                kh = ray_hash_i64(0);
            } else if (wide_any && (kflags[k] & GHT_KEYF_WIDE)) {
                keys[k] = row;
                kh = wide_key_hash_at(ly, k, c->key_data, c->key_pool, row);
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
        h = ght_hash_null_words(h, nullw, null_words);
        }

        uint8_t vi = 0;
        for (uint32_t a = 0; a < na; a++) {
            uint8_t af = aflags[a];
            /* Holistic agg (OP_MEDIAN): no slot reserved — skip
             * packing.  Source column is read in the post-radix pass. */
            if (af & GHT_AF_HOLISTIC) continue;
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
            if ((af & GHT_AF_BINARY) && c->agg_vecs2 && c->agg_vecs2[a]) {
                ray_t* ay = c->agg_vecs2[a];
                if (ay->type == RAY_F64)
                    memcpy(&agg_vals[vi], &((double*)ray_data(ay))[row], 8);
                else
                    agg_vals[vi] = read_col_i64(ray_data(ay), row, ay->type, ay->attrs);
                vi++;
            }
        }

        uint32_t part = RADIX_PART(h);
        radix_buf_push(&my_bufs[part], estride, h,
                       inline_str ? (const int64_t*)keybuf : keys,
                       agg_vals, nv, has_fl, row, ly->key_region);
    }
    scratch_free(stage_hdr);   /* NULL (inline staging) → no-op */
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

/* Pass 3: build result columns from inline group rows */
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
#define grp_finalize_nulls par_finalize_nulls

typedef struct {
    group_ht_t*   part_hts;
    uint32_t*     part_offsets;
    char**        key_dsts;
    int8_t*       key_types;
    uint8_t*      key_attrs;
    uint8_t*      key_esizes;
    ray_t**       key_cols;       /* [n_keys] output key vecs (for null bit writes) */
    uint32_t      n_keys;         /* full width — a uint8 here wrapped 256 keys to 0 */
    agg_out_t*    agg_outs;
    uint32_t      n_aggs;
    /* For wide-key columns (RAY_GUID), the stored key slot is a
     * source row index and we copy the actual bytes from the source
     * column here during the result scatter. */
    void**        key_src_data;   /* [n_keys]; NULL entry if not wide */
} radix_phase3_ctx_t;

static void radix_phase3_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    radix_phase3_ctx_t* c = (radix_phase3_ctx_t*)ctx;
    uint32_t nk = c->n_keys;
    uint32_t na = c->n_aggs;

    for (int64_t p = start; p < end; p++) {
        group_ht_t* ph = &c->part_hts[p];
        uint32_t gc = ph->grp_count;
        if (gc == 0) continue;
        uint32_t off = c->part_offsets[p];
        const ght_layout_t* ly = &ph->layout;
        uint16_t rs = ly->row_stride;
        const uint16_t* const koff = ly->key_off;
        const uint8_t*  const kflags = ly->key_flags;
        const uint8_t*  const aflags = ly->agg_flags;
        const int8_t*   const vslot = ly->agg_val_slot;

        /* Single pass over group rows: read each row once, scatter keys + aggs.
         * Reduces memory traffic from nk+na passes over group data to 1 pass. */
        for (uint32_t gi = 0; gi < gc; gi++) {
            const char* row = ph->rows + (size_t)gi * rs;
            const char* rk = row + 8;     /* key region (key_off-addressed) */
            int64_t cnt = *(const int64_t*)(const void*)row;
            const int64_t* nullw = (const int64_t*)(const void*)(rk + koff[nk]);
            /* Per-slot non-null count when nullable aggs are present; NULL
             * (→ use cnt) for null-free layouts (byte-identical to before). */
            const int64_t* nnbase = ly->off_nn
                ? (const int64_t*)(const void*)(row + ly->off_nn) : NULL;
            uint32_t di = off + gi;

            /* Scatter keys to result columns */
            for (uint32_t k = 0; k < nk; k++) {
                char* dst = c->key_dsts[k];
                uint8_t esz = c->key_esizes[k];
                int8_t kt = c->key_types[k];
                size_t doff = (size_t)di * esz;
                if (nullw[k >> 6] & ((int64_t)((uint64_t)1 << (k & 63)))) {
                    if (c->key_cols && c->key_cols[k])
                        grp_set_null(c->key_cols[k], di);
                    /* Fill the correct-width sentinel. */
                    switch (kt) {
                        case RAY_F64: {
                            double v = NULL_F64; memcpy(dst + doff, &v, 8); break;
                        }
                        case RAY_I64: case RAY_TIMESTAMP: {
                            int64_t v = NULL_I64; memcpy(dst + doff, &v, 8); break;
                        }
                        case RAY_I32: case RAY_DATE: case RAY_TIME: {
                            int32_t v = NULL_I32; memcpy(dst + doff, &v, 4); break;
                        }
                        case RAY_I16: {
                            int16_t v = NULL_I16; memcpy(dst + doff, &v, 2); break;
                        }
                        case RAY_STR: { memset(dst + doff, 0, sizeof(ray_str_t)); break; }
                        default: break;
                    }
                    continue;
                }
                if (kflags[k] & GHT_KEYF_INLINE_STR) {
                    /* Inline STR key: the 16-byte descriptor lives in the row;
                     * copy it to the output (which shares the source pool). */
                    memcpy(dst + doff, rk + koff[k], sizeof(ray_str_t));
                    continue;
                }
                int64_t kv = *(const int64_t*)(const void*)(rk + koff[k]);
                if (kflags[k] & GHT_KEYF_WIDE) {
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
            for (uint32_t a = 0; a < na; a++) {
                /* Holistic aggs (OP_MEDIAN) are filled by the
                 * post-radix pass — skip emitting from the row layout. */
                if (aflags[a] & GHT_AF_HOLISTIC) continue;
                agg_out_t* ao = &c->agg_outs[a];
                if (!ao->dst) continue; /* allocation failed (OOM) */
                uint16_t op = ao->agg_op;
                bool sf = ao->src_f64;
                int8_t s = vslot[a];
                /* nn = per-slot non-null count (nullable layout) or the group
                 * row count (null-free).  Drives the AVG/VAR/STDDEV divisor
                 * and the all-null → typed-null decision, matching the DA path. */
                int64_t nn = nnbase ? nnbase[s] : cnt;
                if (ao->out_type == RAY_F64) {
                    double v;
                    switch (op) {
                        case OP_SUM:
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                            if (ao->affine) v += ao->bias_f64 * cnt;
                            break;
                        case OP_PROD:
                            if (nn == 0) { v = NULL_F64; grp_set_null(ao->vec, di); break; }
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                            break;
                        case OP_AVG:
                            if (nn == 0) { v = NULL_F64; grp_set_null(ao->vec, di); break; }
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s) / nn
                                   : (double)ROW_RD_I64(row, ly->off_sum, s) / nn;
                            if (ao->affine) v += ao->bias_f64;
                            break;
                        case OP_MIN:
                            if (nn == 0) { v = NULL_F64; grp_set_null(ao->vec, di); break; }
                            v = sf ? ROW_RD_F64(row, ly->off_min, s)
                                   : (double)ROW_RD_I64(row, ly->off_min, s);
                            break;
                        case OP_MAX:
                            if (nn == 0) { v = NULL_F64; grp_set_null(ao->vec, di); break; }
                            v = sf ? ROW_RD_F64(row, ly->off_max, s)
                                   : (double)ROW_RD_I64(row, ly->off_max, s);
                            break;
                        case OP_FIRST: case OP_LAST:
                            if (nn == 0) { v = NULL_F64; grp_set_null(ao->vec, di); break; }
                            v = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                            break;
                        case OP_VAR: case OP_VAR_POP:
                        case OP_STDDEV: case OP_STDDEV_POP: {
                            bool insuf = (op == OP_VAR || op == OP_STDDEV) ? nn <= 1 : nn <= 0;
                            if (insuf) { v = NULL_F64; grp_set_null(ao->vec, di); break; }
                            double sum_val = sf ? ROW_RD_F64(row, ly->off_sum, s)
                                                : (double)ROW_RD_I64(row, ly->off_sum, s);
                            double sq_val = ly->off_sumsq ? ROW_RD_F64(row, ly->off_sumsq, s) : 0.0;
                            double mean = sum_val / nn;
                            double var_pop = sq_val / nn - mean * mean;
                            if (var_pop < 0) var_pop = 0;
                            if (op == OP_VAR_POP) v = var_pop;
                            else if (op == OP_VAR) v = var_pop * nn / (nn - 1);
                            else if (op == OP_STDDEV_POP) v = sqrt(var_pop);
                            else v = sqrt(var_pop * nn / (nn - 1));
                            break;
                        }
                        case OP_PEARSON_CORR:
                        case OP_COV:
                        case OP_SCOV:
                        case OP_WSUM:
                        case OP_WAVG: {
                            /* Single-pass formula (same as ray_pearson_corr_fn):
                             *   r = (n·Σxy − Σx·Σy) /
                             *       sqrt((n·Σx² − Σx²)(n·Σy² − Σy²))
                             * Undefined for n<2 or constant side → emit
                             * NaN (canonicalize folds to null upstream). */
                            if ((op == OP_PEARSON_CORR || op == OP_SCOV) && cnt < 2) { v = 0.0; grp_set_null(ao->vec, di); break; }
                            if ((op == OP_COV || op == OP_WAVG) && cnt < 1) { v = 0.0; grp_set_null(ao->vec, di); break; }
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
                            if (op == OP_WSUM) { v = sxy; break; }
                            if (op == OP_WAVG) { if (sx == 0.0) { v = NULL_F64; break; } v = sxy / sx; break; }
                            if (op == OP_COV) { v = (sxy - sx * sy / dn) / dn; break; }
                            if (op == OP_SCOV) { v = (sxy - sx * sy / dn) / (dn - 1.0); break; }
                            if (dx <= 0.0 || dy <= 0.0) { v = NULL_F64; break; }
                            v = num / sqrt(dx * dy);
                            break;
                        }
                        default: v = 0.0; break;
                    }
                    /* Single-null float model: canonicalize non-finite (overflow,
                     * pearson sqrt(≤0)/0-0, etc.) to NULL_F64.  HAS_NULLS is set
                     * by grp_finalize_nulls(agg_outs[a].vec) after dispatch. */
                    ((double*)(void*)ao->dst)[di] = ray_f64_fin(v);
                } else {
                    int64_t v;
                    /* All-null group emits the width-correct sentinel (matches
                     * the DA int emit); SUM/COUNT are never nulled. */
                    int64_t int_null = agg_int_null_sentinel_for(ao->out_type);
                    switch (op) {
                        case OP_SUM:
                            v = ROW_RD_I64(row, ly->off_sum, s);
                            if (ao->affine) v += ao->bias_i64 * cnt;
                            break;
                        case OP_PROD:
                            if (nn == 0) { v = int_null; grp_set_null(ao->vec, di); break; }
                            v = ROW_RD_I64(row, ly->off_sum, s); break;
                        case OP_COUNT: v = cnt; break;
                        case OP_ALL:   v = (ROW_RD_I64(row, ly->off_sum, s) == nn) ? 1 : 0; break;
                        case OP_ANY:   v = (ROW_RD_I64(row, ly->off_sum, s) > 0) ? 1 : 0; break;
                        case OP_MIN:
                            if (nn == 0) { v = int_null; grp_set_null(ao->vec, di); break; }
                            v = ROW_RD_I64(row, ly->off_min, s); break;
                        case OP_MAX:
                            if (nn == 0) { v = int_null; grp_set_null(ao->vec, di); break; }
                            v = ROW_RD_I64(row, ly->off_max, s); break;
                        case OP_FIRST: case OP_LAST:
                            if (nn == 0) { v = int_null; grp_set_null(ao->vec, di); break; }
                            v = ROW_RD_I64(row, ly->off_sum, s); break;
                        default:       v = 0; break;
                    }
                    /* MIN/MAX/PROD/FIRST/LAST keep out_type = agg_col->type
                     * (~9955 region), so ao->vec/ao->dst may be a narrow-int
                     * (I32/I16/U8) column — mirror the serial emit (~4586):
                     * a fixed 8-byte store overshoots a narrower slot,
                     * corrupting/segfaulting past index 0. */
                    topk_write_i64(ao->dst, di, v,
                                   ray_sym_elem_size(ao->out_type, ao->vec->attrs));
                }
            }
        }
    }
}

/* Pass 2: aggregate each partition independently using inline data */
typedef struct {
    int8_t*      key_types;
    uint32_t     n_keys;       /* widened from uint8_t (unbounded-ready) */
    uint32_t     n_workers;
    radix_buf_t* bufs;
    group_ht_t*  part_hts;
    ght_layout_t layout;
    /* Shared (read-only) source column bases for wide-key resolution.
     * Each partition HT stashes the ones matching wide_key_mask. */
    void**       key_data;
    const void** key_pool;     /* [n_keys] str-pool base per wide STR key */
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
        if (c->layout.any_wide_key && c->key_data) {
            group_ht_set_key_data(&c->part_hts[p], c->key_data);
            group_ht_copy_key_pool(&c->part_hts[p], c->key_pool);
        }

        for (uint32_t w = 0; w < c->n_workers; w++) {
            radix_buf_t* buf = &c->bufs[(size_t)w * RADIX_P + p];
            if (buf->count == 0) continue;
            group_rows_indirect(&c->part_hts[p], c->key_types,
                                buf->data, buf->count, estride);
        }
    }
}

/* ============================================================================
 * Fused radix: per-(worker, partition) HT direct-insert + per-partition merge
 *
 *   Replaces the materialise-fat-entries-then-build-HTs round trip with a
 *   single-pass aggregation per (worker, partition) HT, followed by an
 *   in-cache merge per partition.  Currently restricted to count-only
 *   queries (every agg is OP_COUNT) — the merge primitive here only
 *   knows how to combine counts; SUM/AVG/MIN/MAX would need their own
 *   state-merge logic (next increment).
 *
 *   Per-(worker, partition) HT for a 10M-row count-by-UserID: ~3M distinct
 *   keys ÷ 256 parts ÷ 8 workers ≈ 1.5K groups → cap ~4K slots → ~64 KB
 *   row store, L1/L2-resident.  Worker w processes its row range; per row
 *   it hashes keys, computes partition = RADIX_PART(h), probes its local
 *   HT_p.  Phase2 dispatches partitions across workers; each merges the n
 *   worker HTs for one partition into a final partition HT in part_hts[p].
 *   Phase3 (radix_phase3_fn) emits from part_hts[] exactly as before.
 * ============================================================================ */

/* Merge one source group row into the target HT.  Hash is recomputed from
 * the row's key region via hash_keys_inline — identical to what
 * group_probe_entry did when the row was first inserted, so the partition
 * assignment is consistent.  Supports need_flags ∈ {0, GHT_NEED_SUM}:
 * count-only and count+SUM/AVG.  On miss, the entire source row is copied
 * verbatim (memcpy of row_stride); on hit, count += src.count and, when
 * need_sum, each enabled sum slot accumulates the source's sum (f64 or
 * i64 per agg_is_f64).  Caller's v2 gate filters out PROD/FIRST/LAST/
 * MIN/MAX/SUMSQ/PEARSON/MEDIAN — those need richer state merges. */
static inline uint32_t group_merge_row(group_ht_t* ht,
    const char* src_row, const int8_t* key_types, uint32_t mask)
{
    const ght_layout_t* ly = &ht->layout;
    int64_t src_count = *(const int64_t*)src_row;
    const int64_t* skeys = (const int64_t*)(src_row + 8);
    uint64_t h = hash_keys_inline(skeys, key_types, ly->n_keys,
                                  ht->key_data, ly, ht->key_pool);
    uint8_t salt = HT_SALT(h);
    uint32_t slot = (uint32_t)(h & mask);
    uint16_t na = ly->n_aggs;
    const uint8_t* const aflags = ly->agg_flags;
    const int8_t*  const vslot = ly->agg_val_slot;
    uint16_t off_sum = ly->off_sum;
    bool need_sum = (ly->need_flags & GHT_NEED_SUM) != 0;
    for (;;) {
        uint32_t sv = ht->slots[slot];
        if (sv == HT_EMPTY) {
            if (ht->grp_count >= ht->grp_cap) {
                if (!group_ht_grow(ht)) { ht->oom = 1; return mask; }
            }
            uint32_t gid = ht->grp_count++;
            char* row = ht->rows + (size_t)gid * ly->row_stride;
            /* Whole-row copy: count + keys/null_mask + aggregator state. */
            memcpy(row, src_row, ly->row_stride);
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
                                  skeys, ly, ht->key_data, ht->key_pool)) {
                *(int64_t*)row += src_count;
                if (need_sum) {
                    for (uint32_t a = 0; a < na; a++) {
                        int8_t s = vslot[a];
                        if (s < 0) continue;
                        size_t off = (size_t)off_sum + (size_t)s * 8;
                        if (aflags[a] & GHT_AF_F64) {
                            double sv_f;
                            memcpy(&sv_f, src_row + off, 8);
                            // cppcheck-suppress invalidPointerCast // GHT rows are 8-byte aligned and off_sum/s*8 are multiples of 8
                            *(double*)(row + off) += sv_f;
                        } else {
                            int64_t sv_i;
                            memcpy(&sv_i, src_row + off, 8);
                            *(int64_t*)(row + off) += sv_i;
                        }
                    }
                }
                return mask;
            }
        }
        slot = (slot + 1) & mask;
    }
}

typedef struct {
    void**         key_data;
    int8_t*        key_types;
    uint8_t*       key_attrs;
    ray_t**        key_vecs;
    ray_t**        agg_vecs;        /* may be NULL for pure COUNT (n_agg_vals==0) */
    ray_t**        agg_vecs2;
    uint8_t*       agg_strlen;
    uint8_t        nullable_mask;
    uint32_t       n_workers;
    group_ht_t*    wpart_hts;        /* [n_workers * RADIX_P] */
    ght_layout_t   layout;
    ray_t*         rowsel;
    const int64_t* match_idx;
    _Atomic(int)   oom;
} radix_v2_phase1_ctx_t;

static void radix_v2_phase1_fn(void* ctx, uint32_t worker_id,
                               int64_t start, int64_t end) {
    radix_v2_phase1_ctx_t* c = (radix_v2_phase1_ctx_t*)ctx;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    const ght_layout_t* ly = &c->layout;
    uint16_t nk = ly->n_keys;
    const uint8_t* const kflags = ly->key_flags;
    const uint8_t* const aflags = ly->agg_flags;
    uint8_t wide_any = ly->any_wide_key;
    uint8_t inline_str = ly->any_inline_str;
    uint8_t nullable = c->nullable_mask;
    const int64_t* match_idx = c->match_idx;

    group_ht_t* my_hts = &c->wpart_hts[(size_t)worker_id * RADIX_P];
    /* Lazily init this worker's 256 partition HTs. */
    for (uint32_t p = 0; p < RADIX_P; p++) {
        if (!my_hts[p].slots) {
            if (!group_ht_init_sized(&my_hts[p], 256, ly, 128)) {
                atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
                return;
            }
            if (wide_any && c->key_data) {
                group_ht_set_key_data(&my_hts[p], c->key_data);
                group_ht_set_key_pool(&my_hts[p], c->key_vecs);
            }
        }
    }
    uint32_t masks[RADIX_P];
    for (uint32_t p = 0; p < RADIX_P; p++) masks[p] = my_hts[p].ht_cap - 1;

    /* Stack-resident transient entry + per-key str-pool table, same layout as
     * group_rows_range.  ≤8 keys/aggs stay on the stack (byte-identical);
     * wider layouts carve one per-worker heap block ONCE (never per row —
     * unbounded-slots cut 4): ebuf(entry_stride) | kpool(n_keys ptrs). */
    char ebuf_stk[8 + 9 * 8 + 8 * 8 + 8];
    const void* kpool_stk[8];
    char* ebuf = ebuf_stk;
    const void** kpool = kpool_stk;
    ray_t* v2_stage_hdr = NULL;
    if (ly->entry_stride > sizeof(ebuf_stk) || nk > 8) {
        char* blk = (char*)scratch_alloc(&v2_stage_hdr,
            (size_t)ly->entry_stride + (size_t)nk * sizeof(void*));
        if (!blk) { atomic_store_explicit(&c->oom, 1, memory_order_relaxed); return; }
        ebuf  = blk;
        kpool = (const void**)(blk + ly->entry_stride);
    }
    derive_key_pool(ly, c->key_vecs, kpool);
    for (int64_t i = start; i < end; i++) {
        if (((i - start) & 65535) == 0 && ray_interrupted()) break;
        int64_t row = match_idx ? match_idx[i] : i;
        if (!match_idx && c->rowsel && !group_rowsel_pass(c->rowsel, row))
            continue;
        uint64_t h = 0;
        int64_t* ek = (int64_t*)(ebuf + 8);
        if (inline_str) {
            h = inline_build_keys(ly, c->key_types, c->key_data, c->key_attrs,
                                  kpool, c->key_vecs, nullable, row, ebuf + 8);
        } else {
        int64_t* nullw = ek + nk;   /* null-mask words at key_off[nk]==nk*8 */
        uint32_t null_words = ly->null_words;
        for (uint32_t w = 0; w < null_words; w++) nullw[w] = 0;
        for (uint32_t k = 0; k < nk; k++) {
            int8_t t = c->key_types[k];
            uint64_t kh;
            bool is_null = nullable && ray_key_may_be_null(c->key_vecs[k])
                           && ray_vec_is_null(c->key_vecs[k], row);
            if (is_null) {
                nullw[k >> 6] |= (int64_t)((uint64_t)1 << (k & 63));
                ek[k] = 0;
                kh = ray_hash_i64(0);
            } else if (wide_any && (kflags[k] & GHT_KEYF_WIDE)) {
                ek[k] = row;
                kh = wide_key_hash_at(ly, k, c->key_data, kpool, row);
            } else if (t == RAY_F64) {
                int64_t kv;
                memcpy(&kv, &((double*)c->key_data[k])[row], 8);
                ek[k] = kv;
                kh = ray_hash_f64(((double*)c->key_data[k])[row]);
            } else {
                int64_t kv = read_col_i64(c->key_data[k], row, t, c->key_attrs[k]);
                ek[k] = kv;
                kh = ray_hash_i64(kv);
            }
            h = (k == 0) ? kh : ray_hash_combine(h, kh);
        }
        h = ght_hash_null_words(h, nullw, null_words);
        }
        *(uint64_t*)ebuf = h;
        /* Pack agg values into entry — only when the HT layout actually
         * reads them.  For count-only need_flags == 0 and accum_from_entry
         * skips every agg slot; packing here would be a wasted column
         * read per row (a measurable regression on q15-class queries). */
        if (ly->need_flags) {
            int64_t* ev = (int64_t*)(ebuf + 8 + (size_t)ly->key_region);
            uint8_t vi = 0;
            uint16_t na = ly->n_aggs;
            for (uint32_t a = 0; a < na; a++) {
                uint8_t af = aflags[a];
                if (af & GHT_AF_HOLISTIC) continue;
                ray_t* ac = c->agg_vecs ? c->agg_vecs[a] : NULL;
                if (!ac) continue;
                if (c->agg_strlen && c->agg_strlen[a])
                    ev[vi] = group_strlen_at(ac, row);
                else if (ac->type == RAY_F64)
                    memcpy(&ev[vi], &((double*)ray_data(ac))[row], 8);
                else
                    ev[vi] = read_col_i64(ray_data(ac), row, ac->type, ac->attrs);
                vi++;
                if ((af & GHT_AF_BINARY) && c->agg_vecs2 && c->agg_vecs2[a]) {
                    ray_t* ay = c->agg_vecs2[a];
                    if (ay->type == RAY_F64)
                        memcpy(&ev[vi], &((double*)ray_data(ay))[row], 8);
                    else
                        ev[vi] = read_col_i64(ray_data(ay), row, ay->type, ay->attrs);
                    vi++;
                }
            }
        }
        uint32_t p = RADIX_PART(h);
        uint32_t new_mask = group_probe_entry(&my_hts[p], ebuf,
                                              c->key_types, masks[p]);
        if (my_hts[p].oom) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            break;
        }
        masks[p] = new_mask;
    }
    scratch_free(v2_stage_hdr);   /* NULL (inline staging) → no-op */
}

typedef struct {
    group_ht_t*   wpart_hts;     /* [n_workers * RADIX_P] — input */
    group_ht_t*   part_hts;      /* [RADIX_P] — output */
    int8_t*       key_types;
    uint32_t      n_workers;
    ght_layout_t  layout;
    void**        key_data;
    const void**  key_pool;    /* [n_keys] str-pool base per wide STR key */
    _Atomic(int)  oom;
} radix_v2_phase2_ctx_t;

static void radix_v2_phase2_fn(void* ctx, uint32_t worker_id,
                               int64_t start, int64_t end) {
    (void)worker_id;
    radix_v2_phase2_ctx_t* c = (radix_v2_phase2_ctx_t*)ctx;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    uint16_t row_stride = c->layout.row_stride;
    for (int64_t p = start; p < end; p++) {
        /* Upper bound on the merged partition: sum of worker grp_counts
         * (some keys may be present in multiple workers — the merge will
         * fold those, so the final grp_count is ≤ this sum). */
        uint32_t total_grps = 0;
        for (uint32_t w = 0; w < c->n_workers; w++)
            total_grps += c->wpart_hts[(size_t)w * RADIX_P + p].grp_count;
        if (total_grps == 0) continue;
        uint32_t ht_cap = 256;
        {
            uint64_t target = (uint64_t)total_grps * 2;
            if (target < 256) target = 256;
            while (ht_cap < target) ht_cap *= 2;
        }
        uint32_t init_grp = 256;
        while (init_grp < total_grps && init_grp < 65536) init_grp *= 2;
        if (!group_ht_init_sized(&c->part_hts[p], ht_cap, &c->layout, init_grp)) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
        if (c->layout.any_wide_key && c->key_data) {
            group_ht_set_key_data(&c->part_hts[p], c->key_data);
            group_ht_copy_key_pool(&c->part_hts[p], c->key_pool);
        }
        uint32_t mask = c->part_hts[p].ht_cap - 1;
        for (uint32_t w = 0; w < c->n_workers; w++) {
            group_ht_t* src = &c->wpart_hts[(size_t)w * RADIX_P + p];
            if (src->grp_count == 0) continue;
            const char* rows = src->rows;
            for (uint32_t gi = 0; gi < src->grp_count; gi++) {
                mask = group_merge_row(&c->part_hts[p],
                                       rows + (size_t)gi * row_stride,
                                       c->key_types, mask);
                if (c->part_hts[p].oom) {
                    atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
                    return;
                }
            }
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
    /* DA-path early-out: once any worker observes a key span wider than
     * span_budget the direct-array path is provably infeasible (its slot
     * count would exceed DA_MAX_COMPOSITE_SLOTS), so the whole scan can
     * stop instead of reading the rest of a 10M-row column for nothing. */
    int64_t          span_budget;
    _Atomic(int)*    abort_flag;
} minmax_ctx_t;

static void minmax_scan_fn(void* ctx, uint32_t worker_id, int64_t start, int64_t end) {
    minmax_ctx_t* c = (minmax_ctx_t*)ctx;
    uint32_t wid = worker_id % c->n_workers;
    const int64_t* match_idx = c->match_idx;
    int64_t kmin = INT64_MAX, kmax = INT64_MIN;
    int8_t t = c->key_type;
    const int64_t span_budget = c->span_budget;

    /* Span check and abort poll are batched (every 1024 rows) so the
     * hot per-row loop body stays a branchless min/max with no atomics.
     * 8192 was too sparse — the dispatcher hands out 8K-row morsels, so
     * `(i-start) & 8191 == 0` only ever fired at the morsel boundary
     * (where kmin=INT64_MAX/kmax=INT64_MIN make the span check vacuous),
     * leaving every full 8K morsel to run end-to-end on doomed columns. */
    #define MINMAX_SEG_LOOP(TYPE, CAST) \
        do { \
            const TYPE* kd = (const TYPE*)c->key_data; \
            for (int64_t i = start; i < end; i++) { \
                if (((i - start) & 1023) == 0) { \
                    if (atomic_load_explicit(c->abort_flag, \
                                             memory_order_relaxed)) \
                        goto minmax_done; \
                    if (kmax >= kmin && \
                        (uint64_t)(kmax - kmin) > (uint64_t)span_budget) { \
                        atomic_store_explicit(c->abort_flag, 1, \
                                              memory_order_relaxed); \
                        goto minmax_done; \
                    } \
                } \
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

minmax_done:
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
    int64_t*  nn_count;  /* per-(group, agg) non-null counts [n_slots * n_aggs];
                          * incremented inside the F64 NaN-skip / integer
                          * sentinel-skip guards.  Drives null-aware divisors
                          * (AVG/VAR/STDDEV) and all-null finalization
                          * (MIN/MAX/PROD/FIRST/LAST).  NULL when none of the
                          * aggs needs null tracking (no HAS_NULLS columns). */
    int64_t*  first_row; /* min row index seen per slot (FIRST) [n_slots] */
    int64_t*  last_row;  /* max row index seen per slot (LAST)  [n_slots] */
    /* Arena headers */
    ray_t* _h_sum;
    ray_t* _h_min;
    ray_t* _h_max;
    ray_t* _h_sumsq;
    ray_t* _h_count;
    ray_t* _h_nn_count;
    ray_t* _h_first_row;
    ray_t* _h_last_row;
} da_accum_t;

static inline void da_accum_free(da_accum_t* a) {
    scratch_free(a->_h_sum);
    scratch_free(a->_h_min);
    scratch_free(a->_h_max);
    scratch_free(a->_h_sumsq);
    scratch_free(a->_h_count);
    scratch_free(a->_h_nn_count);
    scratch_free(a->_h_first_row);
    scratch_free(a->_h_last_row);
}

/* Unified agg result emitter — used by both DA and HT paths.
 * Arrays indexed by [gi * n_aggs + a], counts by [gi].  nn_counts (if
 * non-NULL) carries the per-(group, agg) non-null row count: AVG/VAR/
 * STDDEV use it as the divisor and MIN/MAX/PROD/FIRST/LAST emit a typed
 * null when it is zero.  Pass NULL to keep the legacy count[gid]-divisor
 * behaviour (callers without HAS_NULLS aggs need not allocate it). */
static void emit_agg_columns(ray_t** result, ray_graph_t* g, const ray_op_ext_t* ext,
                              ray_t* const* agg_vecs, uint32_t grp_count,
                              uint32_t n_aggs,
                              const double*  sum_f64,  const int64_t* sum_i64,
                              const double*  min_f64,  const double*  max_f64,
                              const int64_t* min_i64,  const int64_t* max_i64,
                              const int64_t* counts,
                              const agg_affine_t* affine,
                              const agg_prod_t* prod,
                              const double*  sumsq_f64,
                              const int64_t* nn_counts,
                              const double*  sum_y_f64,
                              const double*  sumsq_y_f64,
                              const double*  sumxy_f64) {
    for (uint32_t a = 0; a < n_aggs; a++) {
        uint16_t agg_op = ext->agg_ops[a];
        ray_t* agg_col = agg_vecs[a];
        /* agg_col == NULL means a fused input.  The accumulator family
         * follows the FUSION, not the expression's out_type: a fused
         * product accumulates F64; the linear i64 plan accumulates i64
         * even when its compiled expression promotes to F64. */
        bool is_f64 = agg_col
            ? (agg_col->type == RAY_F64)
            : (prod && prod[a].enabled);
        int8_t out_type;
        switch (agg_op) {
            case OP_AVG:
            case OP_STDDEV: case OP_STDDEV_POP:
            case OP_VAR: case OP_VAR_POP:
            case OP_PEARSON_CORR:
            case OP_COV: case OP_SCOV:
            case OP_WSUM: case OP_WAVG:
                out_type = RAY_F64; break;
            case OP_ALL:
            case OP_ANY:
                out_type = RAY_BOOL; break;
            case OP_COUNT: out_type = RAY_I64; break;
            case OP_SUM: {
                /* sum preserves TIME (a duration-like temporal): time+time is
                 * a time, matching the scalar ray_sum_fn.  Other integer
                 * families widen to I64; DATE/TIMESTAMP are rejected at
                 * type-admission so never reach here.  The affine/linear SUM
                 * fast paths leave agg_col NULL (they aggregate without
                 * materializing the input vector), so recover the source type
                 * from the aggregation input op when the vector is absent. */
                int8_t src_t = agg_col ? agg_col->type
                             : (op_node(g, ext->agg_ins[a]) ? op_node(g, ext->agg_ins[a])->out_type : 0);
                out_type = is_f64 ? RAY_F64 : (src_t == RAY_TIME ? RAY_TIME : RAY_I64);
                break;
            }
            case OP_PROD:
                out_type = is_f64 ? RAY_F64 : RAY_I64; break;
            default:
                out_type = agg_col ? agg_col->type : RAY_I64; break;
        }
        ray_t* new_col = ray_vec_new(out_type, (int64_t)grp_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        /* SYM MIN/MAX/FIRST/LAST: the emitted values are RAW cell ids
         * accumulated from ONE source column — the output resolves over
         * that column's dictionary (no-op while runtime-domain). */
        if (out_type == RAY_SYM)
            ray_sym_vec_adopt_domain(new_col, sym_domain_rep(agg_col));
        new_col->len = (int64_t)grp_count;
        for (uint32_t gi = 0; gi < grp_count; gi++) {
            size_t idx = (size_t)gi * n_aggs + a;
            /* nn_counts[idx] == 0 means the group is all-null for this
             * agg column — null-aware operators (MIN/MAX/PROD/FIRST/LAST/
             * AVG/VAR/STDDEV) must surface a typed null instead of leaking
             * the accumulator seed (DBL_MAX / -DBL_MAX / 0). */
            int64_t nn = nn_counts ? nn_counts[idx] : counts[gi];
            if (out_type == RAY_F64) {
                double v;
                switch (agg_op) {
                    case OP_SUM:
                        v = is_f64 ? sum_f64[idx] : (double)sum_i64[idx];
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_f64 * counts[gi];
                        break;
                    case OP_PROD:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                        v = is_f64 ? sum_f64[idx] : (double)sum_i64[idx];
                        break;
                    case OP_AVG:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                        v = is_f64 ? sum_f64[idx] / nn : (double)sum_i64[idx] / nn;
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_f64;
                        break;
                    case OP_MIN:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                        v = is_f64 ? min_f64[idx] : (double)min_i64[idx]; break;
                    case OP_MAX:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                        v = is_f64 ? max_f64[idx] : (double)max_i64[idx]; break;
                    case OP_FIRST: case OP_LAST:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                        v = is_f64 ? sum_f64[idx] : (double)sum_i64[idx]; break;
                    case OP_VAR: case OP_VAR_POP:
                    case OP_STDDEV: case OP_STDDEV_POP: {
                        int64_t cnt = nn;
                        bool insuf = (agg_op == OP_VAR || agg_op == OP_STDDEV) ? cnt <= 1 : cnt <= 0;
                        if (insuf) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
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
                    case OP_PEARSON_CORR:
                    case OP_COV:
                    case OP_SCOV:
                    case OP_WSUM:
                    case OP_WAVG: {
                        int64_t cnt = nn;
                        if ((agg_op == OP_PEARSON_CORR || agg_op == OP_SCOV) && cnt < 2) {
                            v = NULL_F64; ray_vec_set_null(new_col, gi, true); break;
                        }
                        if ((agg_op == OP_COV || agg_op == OP_WAVG) && cnt < 1) {
                            v = NULL_F64; ray_vec_set_null(new_col, gi, true); break;
                        }
                        double sx = sum_f64 ? sum_f64[idx] : 0.0;
                        double sy = sum_y_f64 ? sum_y_f64[idx] : 0.0;
                        double sxx = sumsq_f64 ? sumsq_f64[idx] : 0.0;
                        double syy = sumsq_y_f64 ? sumsq_y_f64[idx] : 0.0;
                        double sxy = sumxy_f64 ? sumxy_f64[idx] : 0.0;
                        if (agg_op == OP_WSUM) { v = sxy; break; }
                        if (agg_op == OP_WAVG) {
                            if (sx == 0.0) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                            v = sxy / sx; break;
                        }
                        double dn = (double)cnt;
                        if (agg_op == OP_COV) { v = (sxy - sx * sy / dn) / dn; break; }
                        if (agg_op == OP_SCOV) { v = (sxy - sx * sy / dn) / (dn - 1.0); break; }
                        double num = dn * sxy - sx * sy;
                        double dx = dn * sxx - sx * sx;
                        double dy = dn * syy - sy * sy;
                        if (dx <= 0.0 || dy <= 0.0) {
                            v = NULL_F64; ray_vec_set_null(new_col, gi, true); break;
                        }
                        v = num / sqrt(dx * dy);
                        break;
                    }
                    default:     v = 0.0; break;
                }
                /* Single-null float model: canonicalize a non-finite computed
                 * aggregate (sum overflow → ±Inf, var/stddev overflow, etc.) to
                 * NULL_F64.  Explicit empty-group cases above already set the
                 * null bit; a grp_finalize_nulls(new_col) scan below flips
                 * HAS_NULLS for any sentinel produced here. */
                ((double*)ray_data(new_col))[gi] = ray_f64_fin(v);
            } else {
                int64_t v;
                /* Null sentinel must match out_type's width: NULL_I64 truncated
                 * into a narrow (I32/I16) slot by topk_write_i64 collapses to 0
                 * and shadows the null. */
                int64_t int_null = agg_int_null_sentinel_for(out_type);
                switch (agg_op) {
                    case OP_SUM:
                        v = sum_i64[idx];
                        if (affine && affine[a].enabled)
                            v += affine[a].bias_i64 * counts[gi];
                        break;
                    case OP_PROD:
                        if (nn == 0) { v = int_null; ray_vec_set_null(new_col, gi, true); break; }
                        v = sum_i64[idx]; break;
                    case OP_COUNT: v = counts[gi]; break;
                    case OP_ALL:   v = (sum_i64[idx] == nn) ? 1 : 0; break;
                    case OP_ANY:   v = (sum_i64[idx] > 0) ? 1 : 0; break;
                    case OP_MIN:
                        if (nn == 0) { v = int_null; ray_vec_set_null(new_col, gi, true); break; }
                        v = min_i64[idx]; break;
                    case OP_MAX:
                        if (nn == 0) { v = int_null; ray_vec_set_null(new_col, gi, true); break; }
                        v = max_i64[idx]; break;
                    case OP_FIRST: case OP_LAST:
                        if (nn == 0) { v = int_null; ray_vec_set_null(new_col, gi, true); break; }
                        v = sum_i64[idx]; break;
                    default:       v = 0; break;
                }
                /* MIN/MAX/FIRST/LAST/PROD keep out_type = agg_col->type, so
                 * new_col may be a narrow-int (I32/I16/U8) or adaptive-width
                 * SYM vector.  Store at new_col's true element width — a fixed
                 * 8-byte int64 store overshoots narrow slots, leaving every
                 * group past index 0 unwritten (correct only at gi==0). */
                topk_write_i64(ray_data(new_col), gi, v,
                               ray_sym_elem_size(out_type, new_col->attrs));
            }
        }
        /* Generate unique column name: base_name + agg suffix (e.g. "v1_sum") */
        ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]);
        int64_t name_id;
        if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
            /* Shared with exec_group_v2 (agg_engine.c) — parity by
             * construction.  Same input-name + per-op-suffix logic, same
             * 256-byte buffer overflow fallback to the input sym. */
            name_id = agg_result_col_name(agg_ext->sym, agg_op);
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
                case OP_ALL:   nsfx = "_all";   nslen = 4; break;
                case OP_ANY:   nsfx = "_any";   nslen = 4; break;
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
                case OP_QUANTILE:   nsfx = "_quantile";   nslen = 9; break;
                case OP_MODE:       nsfx = "_mode";       nslen = 5; break;
                case OP_COV:        nsfx = "_cov";        nslen = 4; break;
                case OP_SCOV:       nsfx = "_scov";       nslen = 5; break;
                case OP_WSUM:       nsfx = "_wsum";       nslen = 5; break;
                case OP_WAVG:       nsfx = "_wavg";       nslen = 5; break;
                case OP_TOP_N:      nsfx = "_top";        nslen = 4; break;
                case OP_BOT_N:      nsfx = "_bot";        nslen = 4; break;
            }
            memcpy(nbuf + np, nsfx, nslen);
            name_id = ray_sym_intern(nbuf, (size_t)np + nslen);
        }
        /* Single-null float model: flip HAS_NULLS if the F64 store loop above
         * canonicalized any non-finite aggregate to NULL_F64 (e.g. overflow)
         * without an explicit ray_vec_set_null. */
        if (new_col->type == RAY_F64) grp_finalize_nulls(new_col);
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
    agg_prod_t*    agg_prod;     /* fused SUM/AVG(a*b) slots (may be NULL) */
    uint16_t*      agg_ops;      /* per-agg operation code */
    uint8_t        n_aggs;
    uint8_t        need_flags;   /* DA_NEED_* bitmask */
    uint64_t       agg_f64_mask; /* bitmask: bit a set if agg[a] is RAY_F64 */
    uint64_t       agg_int_null_mask; /* bitmask: bit a set if agg[a] is an integer col with HAS_NULLS */
    int64_t*       agg_int_null_sentinel; /* per-agg int sentinel (NULL_I64 etc) when bit set in mask */
    bool           all_sum;      /* true when all ops are SUM/AVG/COUNT (no MIN/MAX/FIRST/LAST) */
    uint32_t       n_slots;
    const int64_t* match_idx;    /* NULL = no selection */
    ray_t*         rowsel;
    ray_t**        sym_strings;  /* borrowed sym snapshot for strlen-on-SYM aggs */
    uint32_t       sym_count;
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
    for (uint32_t k = 0; k < c->n_keys; k++) {
        int64_t val = read_by_esz(c->key_ptrs[k], r, c->key_esz[k]);
        gid += (int32_t)((val - c->key_mins[k]) * c->key_strides[k]);
    }
    return gid;
}

/* Typed composite GID: eliminates per-element switch when all keys share width */
#define DEFINE_DA_COMPOSITE_GID_TYPED(SUFFIX, KTYPE) \
static inline int32_t da_composite_gid_##SUFFIX(da_ctx_t* c, int64_t r) { \
    int32_t gid = 0; \
    for (uint32_t k = 0; k < c->n_keys; k++) { \
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
    /* SYM cell: resolve through the COLUMN's domain (sym-domain Phase 2)
     * — sym_elem resolves via the global table, wrong for FILE-domain
     * columns. */
    ray_t* s = ray_sym_vec_cell((ray_t*)col, row);
    return s ? (int64_t)ray_str_len(s) : 0;
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
    /* The lock-free cache is a borrow of the GLOBAL sym table — only
     * valid when this column's cell ids ARE global ids (runtime domain).
     * FILE-domain columns fall through to the domain-aware resolver. */
    if (col->type == RAY_SYM && sym_strings &&
        ray_sym_vec_domain((ray_t*)col) == ray_sym_runtime_domain()) {
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
    if (input_op->opcode != OP_STRLEN || input_op->arity != 1 || !op_child(g, input_op, 0))
        return false;
    ray_op_t* child = op_child(g, input_op, 0);
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

static bool sum_atom_value(ray_t* x, bool* is_f64, double* out_f64, int64_t* out_i64) {
    if (!x || RAY_IS_ERR(x) || !ray_is_atom(x) || RAY_ATOM_IS_NULL(x))
        return false;
    switch (x->type) {
        case -RAY_F64:
            *is_f64 = true;
            *out_f64 = x->f64;
            return true;
        case -RAY_I64:
        case -RAY_DATE:
        case -RAY_TIME:
        case -RAY_TIMESTAMP:
        case -RAY_SYM:
            *is_f64 = false;
            *out_i64 = x->i64;
            return true;
        case -RAY_I32:
            *is_f64 = false;
            *out_i64 = x->i32;
            return true;
        case -RAY_I16:
            *is_f64 = false;
            *out_i64 = x->i16;
            return true;
        case -RAY_U8:
        case -RAY_BOOL:
            *is_f64 = false;
            *out_i64 = x->u8;
            return true;
        default:
            return false;
    }
}

/* agg_int_null_sentinel_for moved to internal.h (shared with pivot.c). */

/* Fused SUM/AVG(a*b) per-row product — both sides promoted to double,
 * matching the expr path exactly (see try_prod_sumavg_input_f64). */
static inline double prod_val_f64(const agg_prod_t* p, int64_t r) {
    double x = (p->ta == RAY_F64) ? ((const double*)p->pa)[r]
             : (double)read_col_i64(p->pa, r, p->ta, p->aa);
    double y = (p->tb == RAY_F64) ? ((const double*)p->pb)[r]
             : (double)read_col_i64(p->pb, r, p->tb, p->ab);
    return x * y;
}

/* Pool worker + fallback materializer for fused-product slots.  Group
 * paths WITHOUT the fused read (radix sp / HT row-layout) call this at
 * entry: each enabled slot becomes an ordinary owned F64 agg vector and
 * the slot is disabled — semantics identical to the never-fused flow.
 * Returns false on OOM (nothing partially converted is leaked: converted
 * slots are ordinary owned agg_vecs freed by the caller's error path). */
typedef struct {
    const agg_prod_t* prod;
    double*           dst;
} prod_fill_ctx_t;

static void prod_fill_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    prod_fill_ctx_t* c = (prod_fill_ctx_t*)raw;
    for (int64_t i = start; i < end; i++)
        c->dst[i] = prod_val_f64(c->prod, i);
}

static bool group_materialize_prod_slots(agg_prod_t* prod, ray_t** agg_vecs,
                                         uint8_t* agg_owned, uint8_t n_aggs,
                                         int64_t nrows) {
    for (uint32_t a = 0; a < n_aggs; a++) {
        if (!prod[a].enabled) continue;
        ray_t* v = ray_vec_new(RAY_F64, nrows > 0 ? nrows : 1);
        if (!v || RAY_IS_ERR(v)) return false;
        v->len = nrows;
        prod_fill_ctx_t fc = { .prod = &prod[a], .dst = (double*)ray_data(v) };
        ray_pool_t* pool = ray_pool_get();
        if (pool && nrows >= RAY_PARALLEL_THRESHOLD)
            ray_pool_dispatch(pool, prod_fill_fn, &fc, nrows);
        else
            prod_fill_fn(&fc, 0, 0, nrows);
        agg_vecs[a] = v;
        agg_owned[a] = 1;
        prod[a].enabled = false;
    }
    return true;
}

/* ---- Scalar aggregate (n_keys==0): one flat scan, no GID, no hash ---- */
typedef struct {
    void**         agg_ptrs;
    int8_t*        agg_types;
    ray_t**        agg_cols;
    uint8_t*       agg_strlen;
    uint16_t*      agg_ops;
    agg_linear_t*  agg_linear;
    agg_prod_t*    agg_prod;     /* fused SUM/AVG(a*b) slots (may be NULL) */
    uint32_t       n_aggs;
    uint8_t        need_flags;
    const int64_t* match_idx;    /* NULL = no selection */
    ray_t*         rowsel;
    /* per-worker accumulators (1 slot each) */
    da_accum_t*    accums;
    uint32_t       n_accums;
    /* Per-agg integer-null sentinel + has-nulls flag.  A per-element array
     * (not a bitmask, unlike da_ctx_t's agg_int_null_mask) because this
     * path's n_aggs is VLA-sized with no fixed cap — a 32/64-bit mask would
     * silently alias past that many aggregates. */
    const bool*    agg_int_null_has;
    int64_t*       agg_int_null_sentinel;
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
    /* n_terms is bounded by AGG_LINEAR_MAX_TERMS (8, internal.h) — an
     * unrelated fixed cap on linear-expression arity, not a GROUP n_keys/
     * n_aggs count, so it stays uint8_t. */
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
    uint32_t n_aggs = c->n_aggs;
    acc->count[0]++;
    /* Per-(group, agg) non-null counters drive AVG/VAR/STDDEV divisors
     * and all-null finalization for MIN/MAX/PROD/FIRST/LAST.  Only
     * allocated when at least one agg can produce a null
     * (acc->nn_count != NULL). */
    int64_t* nn = acc->nn_count;
    for (uint32_t a = 0; a < n_aggs; a++) {
        double fv; int64_t iv;
        if (c->agg_prod && c->agg_prod[a].enabled) {
            fv = prod_val_f64(&c->agg_prod[a], r);
            iv = (int64_t)fv;
        } else if (c->agg_linear && c->agg_linear[a].enabled) {
            const agg_linear_t* lin = &c->agg_linear[a];
            iv = lin->bias_i64;
            /* n_terms bounded by AGG_LINEAR_MAX_TERMS (8) — see the note in
             * scalar_sum_linear_i64_fn above; unrelated to n_keys/n_aggs. */
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
                uint8_t attrs = c->agg_cols[a] ? c->agg_cols[a]->attrs : 0;
                da_read_val(c->agg_ptrs[a], c->agg_types[a], attrs, r, &fv, &iv);
            }
        }
        uint16_t op = c->agg_ops[a];
        bool is_f = (c->agg_types[a] == RAY_F64);
        /* NULL_I* sentinel = null. */
        bool int_null = !is_f && c->agg_int_null_has[a] &&
                        iv == c->agg_int_null_sentinel[a];
        bool is_null = is_f ? !(fv == fv) : int_null;
        if (op == OP_ALL || op == OP_ANY) {
            if (!is_null) {
                acc->sum[a].i += (is_f ? (fv != 0.0) : (iv != 0)) ? 1 : 0;
                if (nn) nn[a]++;
            }
        } else if (op == OP_SUM || op == OP_AVG || op == OP_STDDEV || op == OP_STDDEV_POP || op == OP_VAR || op == OP_VAR_POP) {
            if (is_f) {
                /* NaN payload = null, skip from sum/sumsq. */
                if (RAY_LIKELY(fv == fv)) {
                    acc->sum[a].f += fv;
                    if (acc->sumsq_f64) acc->sumsq_f64[a] += fv * fv;
                    if (nn) nn[a]++;
                }
            } else if (RAY_LIKELY(!int_null)) {
                acc->sum[a].i += iv;
                if (acc->sumsq_f64) acc->sumsq_f64[a] += fv * fv;
                if (nn) nn[a]++;
            }
        } else if (op == OP_PROD) {
            /* "First non-null" marker: nn[a]==0 when nn is tracked,
             * otherwise count[0]==1 (always non-null without nn). */
            bool first_seen = nn ? (nn[a] == 0) : (acc->count[0] == 1);
            if (is_f) {
                if (fv == fv) {
                    if (first_seen) acc->sum[a].f = fv;
                    else acc->sum[a].f *= fv;
                    if (nn) nn[a]++;
                }
            } else if (RAY_LIKELY(!int_null)) {
                if (first_seen) acc->sum[a].i = iv;
                else acc->sum[a].i = (int64_t)((uint64_t)acc->sum[a].i * (uint64_t)iv);
                if (nn) nn[a]++;
            }
        } else if (op == OP_FIRST) {
            /* Only commit the value AND advance the "first non-null seen"
             * marker when the row is non-null — otherwise a null at row 0
             * would block every later non-null row. */
            if (!is_null) {
                bool first_seen = nn ? (nn[a] == 0) : (acc->count[0] == 1);
                if (first_seen) {
                    if (is_f) acc->sum[a].f = fv;
                    else acc->sum[a].i = iv;
                }
                if (nn) nn[a]++;
            }
        } else if (op == OP_LAST) {
            if (!is_null) {
                if (is_f) acc->sum[a].f = fv;
                else acc->sum[a].i = iv;
                if (nn) nn[a]++;
            }
        } else if (op == OP_MIN) {
            if (is_f) { if (fv == fv && fv < acc->min_val[a].f) acc->min_val[a].f = fv; }
            else if (c->agg_types[a] == RAY_SYM) {
                /* Lex compare for SYM; INT64_MAX = "not seen yet". */
                if (acc->min_val[a].i == INT64_MAX ||
                    sym_lex_lt(ray_sym_vec_domain(c->agg_cols[a]), iv, acc->min_val[a].i))
                    acc->min_val[a].i = iv;
            }
            else if (!int_null) { if (iv < acc->min_val[a].i) acc->min_val[a].i = iv; }
            if (!is_null && nn) nn[a]++;
        } else if (op == OP_MAX) {
            if (is_f) { if (fv == fv && fv > acc->max_val[a].f) acc->max_val[a].f = fv; }
            else if (c->agg_types[a] == RAY_SYM) {
                if (acc->max_val[a].i == INT64_MIN ||
                    sym_lex_gt(ray_sym_vec_domain(c->agg_cols[a]), iv, acc->max_val[a].i))
                    acc->max_val[a].i = iv;
            }
            else if (!int_null) { if (iv > acc->max_val[a].i) acc->max_val[a].i = iv; }
            if (!is_null && nn) nn[a]++;
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
        uint64_t f64m = c->agg_f64_mask;
        uint64_t inm = c->agg_int_null_mask;
        int64_t* nn = acc->nn_count;
        for (uint32_t a = 0; a < n_aggs; a++) {
            size_t idx = base + a;
            if (c->agg_prod && c->agg_prod[a].enabled) {
                /* Fused product: gated null-free, so every row counts. */
                acc->sum[idx].f += prod_val_f64(&c->agg_prod[a], r);
                if (nn) nn[idx]++;
                continue;
            }
            if (!c->agg_ptrs[a]) continue;
            if (c->agg_strlen && c->agg_strlen[a]) {
                acc->sum[idx].i += group_strlen_at_cached(
                    c->agg_cols[a], r, c->sym_strings, c->sym_count);
                if (nn) nn[idx]++;
            } else if (f64m & ((uint64_t)1 << a)) {
                /* NaN payload = null, skip from sum. */
                double v = ((const double*)c->agg_ptrs[a])[r];
                if (RAY_LIKELY(v == v)) { acc->sum[idx].f += v; if (nn) nn[idx]++; }
            } else {
                /* NULL_I* sentinel = null, skip from sum.  Only paid when
                 * the source column actually advertises nulls.  A user-stored
                 * INT_MIN value in a HAS_NULLS column is indistinguishable
                 * from a null and is dropped — this is the standard cost of
                 * sentinel-based null encoding for integers. */
                uint8_t v_attrs = c->agg_cols[a] ? c->agg_cols[a]->attrs : 0;
                int64_t v = read_col_i64(c->agg_ptrs[a], r, c->agg_types[a], v_attrs);
                if (RAY_LIKELY(!((inm >> a) & 1) || v != c->agg_int_null_sentinel[a])) {
                    acc->sum[idx].i += v;
                    if (nn) nn[idx]++;
                }
            }
        }
        return;
    }

    /* Track per-slot row-index bounds when FIRST/LAST is needed.  Pool
     * dispatch is work-stealing: tasks may be claimed by a single worker
     * out of index order, so rows do NOT arrive in monotonic order within
     * a worker.  Use explicit min/max comparison against r and update the
     * stored value only when the new row beats the current bound.
     *
     * Multi-FIRST limitation: first_row[gid] is shared across all FIRST
     * aggs in this group, so two FIRST aggs A and B on different columns
     * with disjoint null patterns can race — whichever non-null lands
     * first stakes first_row and the other agg never gets a chance.
     * The result for the "loser" agg is a typed null (nn[idx] stays 0),
     * which is strictly safer than leaking the 0 calloc seed but still
     * not the true first-non-null value.  Fix would require per-(group,
     * agg) first_row arrays — documented for future work. */
    bool fl_take_first = (acc->first_row && r < acc->first_row[gid]);
    bool fl_take_last  = (acc->last_row  && r > acc->last_row[gid]);
    bool first_advanced = false, last_advanced = false;

    int64_t* nn = acc->nn_count;
    for (uint32_t a = 0; a < n_aggs; a++) {
        if (!c->agg_ptrs[a] && !(c->agg_prod && c->agg_prod[a].enabled))
            continue;
        size_t idx = base + a;
        double fv; int64_t iv;
        if (c->agg_prod && c->agg_prod[a].enabled) {
            fv = prod_val_f64(&c->agg_prod[a], r);
            iv = (int64_t)fv;
        } else if (c->agg_strlen && c->agg_strlen[a]) {
            iv = group_strlen_at_cached(c->agg_cols[a], r,
                                        c->sym_strings, c->sym_count);
            fv = (double)iv;
        } else {
            uint8_t attrs = c->agg_cols[a] ? c->agg_cols[a]->attrs : 0;
            da_read_val(c->agg_ptrs[a], c->agg_types[a], attrs, r, &fv, &iv);
        }
        uint16_t op = c->agg_ops[a];
        bool is_f = (c->agg_types[a] == RAY_F64);
        /* NULL_I* sentinel = null.  Bit set in agg_int_null_mask AND
         * value equal to per-agg sentinel means this row is null for
         * an integer aggregation column. */
        bool int_null = (c->agg_int_null_mask & ((uint64_t)1 << a)) &&
                        iv == c->agg_int_null_sentinel[a];
        bool is_null = is_f ? !(fv == fv) : int_null;
        if (op == OP_ALL || op == OP_ANY) {
            if (!is_null) {
                acc->sum[idx].i += (is_f ? (fv != 0.0) : (iv != 0)) ? 1 : 0;
                if (nn) nn[idx]++;
            }
        } else if (op == OP_SUM || op == OP_AVG || op == OP_STDDEV || op == OP_STDDEV_POP || op == OP_VAR || op == OP_VAR_POP) {
            if (is_f) {
                /* NaN payload = null, skip from sum/sumsq. */
                if (RAY_LIKELY(fv == fv)) {
                    acc->sum[idx].f += fv;
                    if (acc->sumsq_f64) acc->sumsq_f64[idx] += fv * fv;
                    if (nn) nn[idx]++;
                }
            } else if (RAY_LIKELY(!int_null)) {
                acc->sum[idx].i = (int64_t)((uint64_t)acc->sum[idx].i + (uint64_t)iv);
                if (acc->sumsq_f64) acc->sumsq_f64[idx] += fv * fv;
                if (nn) nn[idx]++;
            }
        } else if (op == OP_PROD) {
            /* "First non-null" marker: nn[idx]==0 when nn is tracked,
             * otherwise count[gid]==1 (always non-null without nn). */
            bool first_seen = nn ? (nn[idx] == 0) : (acc->count[gid] == 1);
            if (is_f) {
                if (fv == fv) {
                    if (first_seen) acc->sum[idx].f = fv;
                    else acc->sum[idx].f *= fv;
                    if (nn) nn[idx]++;
                }
            } else if (RAY_LIKELY(!int_null)) {
                if (first_seen) acc->sum[idx].i = iv;
                else acc->sum[idx].i = (int64_t)((uint64_t)acc->sum[idx].i * (uint64_t)iv);
                if (nn) nn[idx]++;
            }
        } else if (op == OP_FIRST) {
            /* Only stake the first-row claim when this row's value for the
             * agg column is actually non-null — a null prefix would block
             * later non-null rows otherwise. */
            if (fl_take_first && !is_null) {
                if (is_f) acc->sum[idx].f = fv;
                else acc->sum[idx].i = iv;
                first_advanced = true;
                if (nn) nn[idx]++;
            }
        } else if (op == OP_LAST) {
            if (fl_take_last && !is_null) {
                if (is_f) acc->sum[idx].f = fv;
                else acc->sum[idx].i = iv;
                last_advanced = true;
                if (nn) nn[idx]++;
            }
        } else if (op == OP_MIN) {
            if (is_f) {
                /* NaN comparisons are always false, but make the skip
                 * explicit. */
                if (fv == fv && fv < acc->min_val[idx].f) acc->min_val[idx].f = fv;
            } else if (c->agg_types[a] == RAY_SYM) {
                /* Lex compare for SYM; INT64_MAX = "not seen yet". */
                if (acc->min_val[idx].i == INT64_MAX ||
                    sym_lex_lt(ray_sym_vec_domain(c->agg_cols[a]), iv, acc->min_val[idx].i))
                    acc->min_val[idx].i = iv;
            } else if (!int_null) {
                if (iv < acc->min_val[idx].i) acc->min_val[idx].i = iv;
            }
            if (!is_null && nn) nn[idx]++;
        } else if (op == OP_MAX) {
            if (is_f) {
                if (fv == fv && fv > acc->max_val[idx].f) acc->max_val[idx].f = fv;
            } else if (c->agg_types[a] == RAY_SYM) {
                if (acc->max_val[idx].i == INT64_MIN ||
                    sym_lex_gt(ray_sym_vec_domain(c->agg_cols[a]), iv, acc->max_val[idx].i))
                    acc->max_val[idx].i = iv;
            } else if (!int_null) {
                if (iv > acc->max_val[idx].i) acc->max_val[idx].i = iv;
            }
            if (!is_null && nn) nn[idx]++;
        }
    }

    /* Commit row-index bounds only when an OP_FIRST/OP_LAST actually
     * accepted this row's value.  An all-null row at the smallest index
     * must NOT advance first_row[gid] — otherwise the next non-null row
     * loses the FIRST race. */
    if (first_advanced) acc->first_row[gid] = r;
    if (last_advanced)  acc->last_row[gid]  = r;
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
    for (uint32_t k = 1; k < n_keys; k++)
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
    ray_t* const* agg_cols;   /* per-agg input vec (SYM lex via its domain) */
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
                for (uint32_t a = 0; a < n_aggs; a++)
                    merged->sumsq_f64[base + a] += wa->sumsq_f64[base + a];
            }
            if (c->need_flags & DA_NEED_SUM) {
                for (uint32_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    uint16_t aop = c->agg_ops ? c->agg_ops[a] : OP_SUM;
                    /* nn_count is per-(group, agg); count is per group.
                     * Fall back to count when nn_count is absent. */
                    int64_t mnn = merged->nn_count ? merged->nn_count[idx] : merged->count[s];
                    int64_t wnn = wa->nn_count ? wa->nn_count[idx] : wa->count[s];
                    if (aop == OP_FIRST) {
                        /* Keep worker 0 value; take from w only if merged has no non-null value */
                        if (mnn == 0 && wnn > 0)
                            merged->sum[idx] = wa->sum[idx];
                    } else if (aop == OP_LAST) {
                        /* Overwrite with last worker that has a non-null value */
                        if (wnn > 0)
                            merged->sum[idx] = wa->sum[idx];
                    } else if (aop == OP_PROD) {
                        if (wnn > 0) {
                            if (mnn == 0)
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
                for (uint32_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    if (agg_types[a] == RAY_F64) {
                        if (wa->min_val[idx].f < merged->min_val[idx].f)
                            merged->min_val[idx].f = wa->min_val[idx].f;
                    } else if (agg_types[a] == RAY_SYM) {
                        if (wa->min_val[idx].i != INT64_MAX &&
                            (merged->min_val[idx].i == INT64_MAX ||
                             sym_lex_lt(ray_sym_vec_domain(c->agg_cols[a]),
                                        wa->min_val[idx].i, merged->min_val[idx].i)))
                            merged->min_val[idx].i = wa->min_val[idx].i;
                    } else {
                        if (wa->min_val[idx].i < merged->min_val[idx].i)
                            merged->min_val[idx].i = wa->min_val[idx].i;
                    }
                }
            }
            if (c->need_flags & DA_NEED_MAX) {
                for (uint32_t a = 0; a < n_aggs; a++) {
                    size_t idx = base + a;
                    if (agg_types[a] == RAY_F64) {
                        if (wa->max_val[idx].f > merged->max_val[idx].f)
                            merged->max_val[idx].f = wa->max_val[idx].f;
                    } else if (agg_types[a] == RAY_SYM) {
                        if (wa->max_val[idx].i != INT64_MIN &&
                            (merged->max_val[idx].i == INT64_MIN ||
                             sym_lex_gt(ray_sym_vec_domain(c->agg_cols[a]),
                                        wa->max_val[idx].i, merged->max_val[idx].i)))
                            merged->max_val[idx].i = wa->max_val[idx].i;
                    } else {
                        if (wa->max_val[idx].i > merged->max_val[idx].i)
                            merged->max_val[idx].i = wa->max_val[idx].i;
                    }
                }
            }
            if (merged->nn_count && wa->nn_count) {
                for (uint32_t a = 0; a < n_aggs; a++)
                    merged->nn_count[base + a] += wa->nn_count[base + a];
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
                                  ekeys, ly, ht->key_data, ht->key_pool))
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
    uint32_t      n_keys;           /* widened from uint8_t (unbounded-ready) */
    uint8_t       nullable_mask;    /* 0/1: any key may be null (see build) */
    const ght_layout_t* layout;     /* borrowed; carries key_flags/wide_key_type */
    const void**  key_pool;         /* [n_keys] str-pool base per wide STR key */
    group_ht_t*   part_hts;
    const uint32_t* part_offsets;
    int64_t*      row_gid;          /* output [nrows] */
    const int64_t* match_idx;
    ray_t*        rowsel;           /* non-NULL when selection carried as rowsel */
} reprobe_ctx_t;

static void reprobe_rows_fn(void* vctx, uint32_t worker_id,
                            int64_t start, int64_t end) {
    (void)worker_id;
    reprobe_ctx_t* c = (reprobe_ctx_t*)vctx;
    uint32_t nk = c->n_keys;
    /* Key lookup staging: ≤8 keys stay on the stack (ek_buf: 8 keys + 1 null
     * word; keybuf: inline-STR region), wider layouts carve one per-worker
     * heap block ONCE (never per row — unbounded-slots cut 4). */
    int64_t  ek_buf_stk[9];
    char     keybuf_stk[136];
    int64_t* ek_buf = ek_buf_stk;
    char*    keybuf = keybuf_stk;
    ray_t*   rp_stage_hdr = NULL;
    if ((size_t)c->layout->key_region > sizeof(ek_buf_stk)) {
        size_t kb = c->layout->key_region;
        char* blk = (char*)scratch_alloc(&rp_stage_hdr, kb + kb);
        if (!blk) return;   /* OOM on a pathologically wide layout: drop worker */
        keybuf = blk;
        ek_buf = (int64_t*)(blk + kb);
    }
    int8_t* key_types = c->key_types;
    void** key_data = c->key_data;
    uint8_t* key_attrs = c->key_attrs;
    ray_t** key_vecs = c->key_vecs;
    uint8_t nullable = c->nullable_mask;
    const uint8_t* const kflags = c->layout->key_flags;
    uint8_t wide_any = c->layout->any_wide_key;
    const int64_t* match_idx = c->match_idx;
    ray_t* rowsel = c->rowsel;
    for (int64_t i = start; i < end; i++) {
        if (((i - start) & 65535) == 0 && ray_interrupted()) break;
        int64_t row = match_idx ? match_idx[i] : i;
        /* Honor a rowsel-carried selection: filtered-out rows must not map to
         * any group, else the holistic (median/top) idx_buf would include
         * them.  -1 is the same "no group" sentinel the HT-miss case uses. */
        if (!match_idx && rowsel && !group_rowsel_pass(rowsel, row)) {
            c->row_gid[row] = -1;
            continue;
        }
        uint64_t h = 0;
        const int64_t* lookup_keys;
        if (c->layout->any_inline_str) {
            h = inline_build_keys(c->layout, key_types, key_data, key_attrs,
                                  c->key_pool, key_vecs, nullable, row, keybuf);
            lookup_keys = (const int64_t*)keybuf;
        } else {
        int64_t* nullw = ek_buf + nk;   /* null-mask words at key_off[nk]==nk*8 */
        uint32_t null_words = c->layout->null_words;
        for (uint32_t w = 0; w < null_words; w++) nullw[w] = 0;
        for (uint32_t k = 0; k < nk; k++) {
            int8_t t = key_types[k];
            uint64_t kh;
            bool is_null = nullable && ray_key_may_be_null(key_vecs[k])
                           && ray_vec_is_null(key_vecs[k], row);
            if (is_null) {
                nullw[k >> 6] |= (int64_t)((uint64_t)1 << (k & 63));
                ek_buf[k] = 0;
                kh = ray_hash_i64(0);
            } else if (wide_any && (kflags[k] & GHT_KEYF_WIDE)) {
                ek_buf[k] = row;
                kh = wide_key_hash_at(c->layout, k, key_data, c->key_pool, row);
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
        h = ght_hash_null_words(h, nullw, null_words);
        lookup_keys = ek_buf;
        }

        uint32_t part = RADIX_PART(h);
        uint32_t local = group_ht_lookup_gid(&c->part_hts[part], h,
                                              lookup_keys, key_types);
        if (local == UINT32_MAX || local >= c->part_hts[part].grp_count) {
            c->row_gid[row] = -1;
        } else {
            c->row_gid[row] = (int64_t)c->part_offsets[part] + (int64_t)local;
        }
    }
    scratch_free(rp_stage_hdr);   /* NULL (inline staging) → no-op */
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
        if (gi >= 0 && gi < c->n_groups) hist[gi]++;
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
        if (gi >= 0 && gi < c->n_groups) idx_buf[cur[gi]++] = r;
    }
}

/* ============================================================================
 * Partition-aware group-by: detect parted columns, concatenate segments into
 * a flat table, then run standard exec_group once.
 * ============================================================================ */
ray_t* exec_group(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                  int64_t group_limit); /* forward decl */

/* Forward declaration — defined below exec_group */
static ray_t* exec_group_per_partition(ray_graph_t* g, ray_t* parted_tbl,
                                       ray_op_ext_t* ext, int32_t n_parts,
                                       const int64_t* key_syms,
                                       const int64_t* agg_syms,
                                       const uint8_t* key_is_expr,
                                       const uint8_t* agg_is_expr,
                                       const int64_t* borrow_syms, int n_borrow,
                                       int has_avg, int has_stddev,
                                       int64_t group_limit);

/* ── Per-partition streaming: expression key / agg-input support ──────────
 *
 * A parted GROUP BY streams per-partition only when every key and agg input
 * is either a bare OP_SCAN or a PURE ROW-LOCAL scalar expression.  The pass-3
 * merge re-GROUPs on the materialized key VALUE, so equal logical keys across
 * partitions must produce BIT-IDENTICAL materialized values — this holds for
 * deterministic element-wise ops (the scalar element-wise core plus direct
 * unary math) and ONLY those.  No cross-row operator (rank /
 * window / aggregate) or table-shape-dependent op lives in that opcode range,
 * so admitting exactly that range is both sufficient and safe.  None of those
 * ops produce SYM output (casts target numeric types; concat/upper/lower/trim
 * produce STR, self-contained with no symfile domain), so the cross-partition
 * SYM-domain identity concern that applies to plain SYM key columns does not
 * arise for admitted expression keys.
 *
 * group_expr_src_syms walks the subtree rooted at `root_id`, collecting the
 * distinct source-column syms it scans into out_syms[] (up to max).  Returns
 * the count, or -1 if the subtree contains any non row-local op, references a
 * non-default table (stored_table_id != 0 — a joined table that would NOT
 * resolve against the per-partition sub-table), overflows, or an oversized
 * graph.  A -1 result means "decline per-partition streaming for this key". */
static inline bool group_expr_rowlocal_opcode(uint16_t opcode) {
    return (opcode >= OP_ROUND && opcode <= OP_IDIV) ||
           (opcode >= OP_SIN && opcode <= OP_SIGNUM);
}

static int group_expr_src_syms(ray_graph_t* g, uint32_t root_id,
                               int64_t* out_syms, int max) {
    uint32_t nc = g->node_count;
    if (nc > 4096 || root_id >= nc) return -1;
    uint32_t stack[64];
    int sp = 0, n = 0;
    bool visited[4096];
    memset(visited, 0, nc * sizeof(bool));
    stack[sp++] = root_id;
    while (sp > 0) {
        uint32_t nid = stack[--sp];
        if (nid >= nc || visited[nid]) continue;
        visited[nid] = true;
        ray_op_t* node = &g->nodes[nid];
        if (node->flags & OP_FLAG_DEAD) continue;
        if (node->opcode == OP_SCAN) {
            ray_op_ext_t* sx = find_ext(g, nid);
            if (!sx) return -1;
            /* Only the default (from-) table resolves against the
             * per-partition sub-table; a scan bound to another table id
             * would read the full joined table, not this partition. */
            uint16_t tbl_id = 0;
            memcpy(&tbl_id, sx->base.pad, sizeof(uint16_t));
            if (tbl_id != 0) return -1;
            bool dup = false;
            for (int j = 0; j < n; j++)
                if (out_syms[j] == sx->sym) { dup = true; break; }
            if (!dup) {
                if (n >= max) return -1;
                out_syms[n++] = sx->sym;
            }
            continue;
        }
        if (node->opcode == OP_CONST) continue;
        /* Admit ONLY pure row-local element-wise ops (see header). */
        if (!group_expr_rowlocal_opcode(node->opcode)) return -1;
        for (int i = 0; i < node->arity && i < 2; i++) {
            if (node->in_id[i] == RAY_OP_NONE) continue;
            if (sp >= 64) return -1;
            stack[sp++] = node->in_id[i];
        }
        /* 3-ary ops (OP_IF else / OP_SUBSTR len / OP_REPLACE repl) keep the
         * third operand in ext->third_in. */
        ray_op_ext_t* nx = find_ext(g, nid);
        if (nx && nx->third_in != RAY_OP_NONE && nx->third_in != 0) {
            if (sp >= 64) return -1;
            stack[sp++] = nx->third_in;
        }
    }
    return n;
}

/* Materialize a pure row-local expr subtree over partition `p` of parted_tbl.
 * Borrows segment[p] of each source column named in src_syms[] (all must be
 * plain PARTED columns) into a throwaway sub-table, evaluates `root` via
 * exec_node with g->table swapped to that sub-table (the same mechanism the
 * flat key-expression path uses), and returns a fresh vec of the partition's
 * row count — caller releases — or NULL on any failure.  Only pure row-local
 * element-wise subtrees are admitted here, and those do not consult
 * g->selection, so this materialization is unfiltered regardless of whether a
 * pushed-down WHERE is active (the per-partition sub-group applies the
 * filter, not the key materialization). */
static ray_t* group_eval_part_expr(ray_graph_t* g, ray_t* parted_tbl,
                                   const int64_t* src_syms, int n_src,
                                   uint32_t root, int32_t p) {
    ray_t* sub = ray_table_new(n_src > 0 ? (int64_t)n_src : 1);
    if (!sub || RAY_IS_ERR(sub)) return NULL;
    for (int j = 0; j < n_src; j++) {
        ray_t* pcol = ray_table_get_col(parted_tbl, src_syms[j]);
        if (!pcol || !RAY_IS_PARTED(pcol->type)) { ray_release(sub); return NULL; }
        ray_t* seg = ((ray_t**)ray_data(pcol))[p];
        if (!seg) { ray_release(sub); return NULL; }
        ray_retain(seg);
        sub = ray_table_add_col(sub, src_syms[j], seg);
        ray_release(seg);
        if (!sub || RAY_IS_ERR(sub)) return NULL;
    }
    ray_t* saved = g->table;
    g->table = sub;
    ray_t* vec = exec_node(g, op_node(g, root));
    g->table = saved;
    ray_release(sub);
    if (!vec || RAY_IS_ERR(vec) || !ray_is_vec(vec)) {
        if (vec && !RAY_IS_ERR(vec)) ray_release(vec);
        return NULL;
    }
    return vec;
}

/* --------------------------------------------------------------------------
 * exec_group_parted — dispatch per-partition or concat-fallback
 * -------------------------------------------------------------------------- */
static ray_t* exec_group_parted(ray_graph_t* g, ray_op_t* op, ray_t* parted_tbl,
                               int64_t group_limit) {
    int64_t ncols = ray_table_ncols(parted_tbl);
    if (ncols <= 0) return ray_error("nyi", NULL);

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    uint32_t n_keys = ext->n_keys;
    uint32_t n_aggs = ext->n_aggs;

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
     *   STDDEV, STDDEV_POP, VAR, VAR_POP
     *
     * A pushed-down WHERE (g->selection set) no longer forces the concat
     * fallback: g->selection is a rowsel over the parted table's GLOBAL
     * concatenated row space (segment order, nrows == ray_parted_nrows —
     * the same order OP_SCAN flattens partitions in), so it can be sliced
     * per partition and applied as each sub-table's own local selection.
     * exec_group_per_partition performs that translation. */
    int can_partition = 1;
    int has_avg = 0;
    int has_stddev = 0;
    /* Exact-size scratch carves — n_keys/n_aggs are uint8_t (up to 255 on the
     * unbounded-slots branch) and this dispatch runs BEFORE the n_keys/n_aggs
     * > 8 nyi guard, so fixed key_syms[8]/agg_syms[8] stack arrays overflowed
     * for a parted GROUP BY with 9+ keys or agg inputs.  Sized min 1 to avoid
     * a zero-byte allocation; both stay live through the
     * exec_group_per_partition call below (they are passed to it), then freed
     * before the concat fallback (which re-derives its columns via find_ext,
     * not these arrays). */
    int64_t key_max = n_keys > 0 ? (int64_t)n_keys : 1;
    ray_t* key_syms_hdr = NULL;
    int64_t* key_syms = (int64_t*)scratch_alloc(&key_syms_hdr,
            (size_t)key_max * sizeof(int64_t));
    if (!key_syms) return ray_error("oom", NULL);
    int64_t agg_max = n_aggs > 0 ? (int64_t)n_aggs : 1;
    ray_t* agg_syms_hdr = NULL;
    int64_t* agg_syms = (int64_t*)scratch_alloc(&agg_syms_hdr,
            (size_t)agg_max * sizeof(int64_t));
    if (!agg_syms) { scratch_free(key_syms_hdr); return ray_error("oom", NULL); }

    /* Per-slot expression flags + the borrow set of real source columns that
     * every eligible expression / scan key + agg input needs materialized in
     * its per-partition sub-table.  key_syms[k]/agg_syms[a] hold the real
     * source-column sym for a bare OP_SCAN, or a SYNTHETIC sym ("_gpk<k>" /
     * "_gpa<a>") that names the per-partition-materialized value for an
     * expression.  borrow_syms is the deduped union of the plain (non-
     * MAPCOMMON) PARTED columns those keys/aggs scan; distinct source cols can
     * never exceed the table's column count.  All carves are freed both after
     * the exec_group_per_partition call and before the concat fallback. */
    ray_t *key_is_expr_hdr = NULL, *agg_is_expr_hdr = NULL,
          *borrow_hdr = NULL, *esrc_hdr = NULL;
    uint8_t* key_is_expr = (uint8_t*)scratch_calloc(&key_is_expr_hdr, (size_t)key_max);
    uint8_t* agg_is_expr = (uint8_t*)scratch_calloc(&agg_is_expr_hdr, (size_t)agg_max);
    int64_t bsz = ncols > 0 ? ncols : 1;
    int64_t* borrow_syms = (int64_t*)scratch_alloc(&borrow_hdr, (size_t)bsz * sizeof(int64_t));
    int64_t* esrc = (int64_t*)scratch_alloc(&esrc_hdr, (size_t)bsz * sizeof(int64_t));
    if (!key_is_expr || !agg_is_expr || !borrow_syms || !esrc) {
        scratch_free(key_syms_hdr); scratch_free(agg_syms_hdr);
        scratch_free(key_is_expr_hdr); scratch_free(agg_is_expr_hdr);
        scratch_free(borrow_hdr); scratch_free(esrc_hdr);
        return ray_error("oom", NULL);
    }
    int n_borrow = 0;
    /* Add a real, plain PARTED source column to the borrow set (deduped).
     * Returns 0 if the column is missing or MAPCOMMON (expression sources
     * must be plain PARTED so segment[p] can be borrowed directly). */
    #define GP_BORROW_ADD(sym) do {                                          \
        ray_t* _pc = ray_table_get_col(parted_tbl, (sym));                   \
        if (!_pc || !RAY_IS_PARTED(_pc->type)) { can_partition = 0; break; } \
        int _dup = 0;                                                        \
        for (int _i = 0; _i < n_borrow; _i++)                                \
            if (borrow_syms[_i] == (sym)) { _dup = 1; break; }               \
        if (!_dup && n_borrow < bsz) borrow_syms[n_borrow++] = (sym);        \
    } while (0)

    for (uint32_t k = 0; k < n_keys && can_partition; k++) {
        ray_op_ext_t* ke = find_ext(g, ext->keys[k]);
        if (ke && ke->base.opcode == OP_SCAN) {
            key_syms[k] = ke->sym;
            key_is_expr[k] = 0;
            /* MAPCOMMON scan keys are reconstructed post-merge, not borrowed. */
            ray_t* pc = ray_table_get_col(parted_tbl, ke->sym);
            if (pc && pc->type == RAY_MAPCOMMON) continue;
            GP_BORROW_ADD(ke->sym);
        } else {
            /* Expression key: admit only pure row-local scalar exprs and
             * record its source columns for per-partition materialization. */
            int ns = group_expr_src_syms(g, ext->keys[k], esrc, (int)bsz);
            if (ns <= 0) { can_partition = 0; break; }
            char nbuf[24];
            int nl = snprintf(nbuf, sizeof(nbuf), "_gpk%u", (unsigned)k);
            int64_t synth = ray_sym_intern(nbuf, (size_t)nl);
            /* A real column shadowing the synthetic name would be borrowed
             * over the materialized value — decline. */
            if (ray_table_get_col(parted_tbl, synth)) { can_partition = 0; break; }
            for (int s = 0; s < ns && can_partition; s++) GP_BORROW_ADD(esrc[s]);
            key_syms[k] = synth;
            key_is_expr[k] = 1;
        }
    }
    for (uint32_t a = 0; a < n_aggs && can_partition; a++) {
        uint16_t aop = ext->agg_ops[a];
        /* Holistic aggs can't be
         * merged across partitions without re-scanning underlying
         * values — decline per-partition exec.  Falls through to the
         * concat path which sees the full vector. */
        if (aop == OP_MEDIAN || aop == OP_QUANTILE || aop == OP_MODE ||
            aop == OP_TOP_N || aop == OP_BOT_N) {
            can_partition = 0; break;
        }
        if (aop != OP_SUM && aop != OP_COUNT && aop != OP_MIN &&
            aop != OP_MAX && aop != OP_AVG && aop != OP_FIRST &&
            aop != OP_LAST && aop != OP_STDDEV && aop != OP_STDDEV_POP &&
            aop != OP_VAR && aop != OP_VAR_POP) { can_partition = 0; break; }
        if (aop == OP_AVG) has_avg = 1;
        if (aop == OP_STDDEV || aop == OP_STDDEV_POP ||
            aop == OP_VAR || aop == OP_VAR_POP) has_stddev = 1;
        ray_op_ext_t* ae = find_ext(g, ext->agg_ins[a]);
        if (ae && ae->base.opcode == OP_SCAN) {
            agg_syms[a] = ae->sym;
            agg_is_expr[a] = 0;
            GP_BORROW_ADD(ae->sym);
        } else {
            /* Expression agg input (e.g. (sum (* a b))): admit only pure
             * row-local scalar exprs and record its source columns. */
            int ns = group_expr_src_syms(g, ext->agg_ins[a], esrc, (int)bsz);
            if (ns <= 0) { can_partition = 0; break; }
            char nbuf[24];
            int nl = snprintf(nbuf, sizeof(nbuf), "_gpa%u", (unsigned)a);
            int64_t synth = ray_sym_intern(nbuf, (size_t)nl);
            if (ray_table_get_col(parted_tbl, synth)) { can_partition = 0; break; }
            for (int s = 0; s < ns && can_partition; s++) GP_BORROW_ADD(esrc[s]);
            agg_syms[a] = synth;
            agg_is_expr[a] = 1;
        }
    }
    #undef GP_BORROW_ADD

    /* Per-partition WHERE needs at least one real borrowed source column to
     * carry the selection's surviving rows into each sub-table.  With none
     * (e.g. a COUNT grouped solely by MAPCOMMON keys) the sub-table would be
     * empty and the local selection could not be applied — fall back to the
     * concat path, which applies g->selection over the flattened table. */
    if (g->selection && n_borrow == 0) can_partition = 0;

    /* Cardinality gate: estimate groups from first partition.
     * Per-partition only wins when #groups << partition_size. */
    if (can_partition) {
        int64_t rows_per_part = total_rows / n_parts;
        int64_t est_groups = 1;
        for (uint32_t k = 0; k < n_keys; k++) {
            if (key_is_expr[k]) {
                /* Expression key: materialize partition 0 and estimate its
                 * distinct group count via HyperLogLog.  A range-based proxy
                 * (as used for integer scan keys below) would badly over-count
                 * bucketed keys — xbar values are spaced N apart, so hi-lo
                 * spans the whole domain range, not the bucket count. */
                ray_t* v0 = group_eval_part_expr(g, parted_tbl, borrow_syms,
                                                 n_borrow, ext->keys[k], 0);
                if (!v0) { est_groups = rows_per_part; break; }
                ray_t* cd = ray_count_distinct_approx(v0);
                ray_release(v0);
                if (!cd || RAY_IS_ERR(cd) || !ray_is_atom(cd)) {
                    if (cd && !RAY_IS_ERR(cd)) ray_release(cd);
                    est_groups = rows_per_part; break;
                }
                int64_t card = cd->i64;
                ray_release(cd);
                if (card < 1) card = 1;
                est_groups *= card;
                if (est_groups > rows_per_part) { est_groups = rows_per_part; break; }
                continue;
            }
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
                /* seg0's cell ids are positions in ITS domain — size the
                 * presence bitmap by that domain's count, not the global
                 * intern count (a FILE-domain position can exceed the
                 * global count → OOB).  Identical value pre-flip. */
                int64_t dom_n = ray_sym_domain_count(ray_sym_vec_domain(seg0));
                uint32_t sym_n = (dom_n > 0 && dom_n <= UINT32_MAX) ? (uint32_t)dom_n : 0;
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
        /* Post-filter cap: est_groups is estimated from partition 0's
         * UNFILTERED rows, but a pushed-down WHERE lets a partition yield at
         * most one group per SURVIVING row.  Without this cap a selective
         * filter over a genuinely low-cardinality group-by would be judged
         * high-cardinality against the (still-unfiltered) rows_per_part and
         * wrongly forced onto the concat path — which materialises ALL rows,
         * exactly the work per-partition streaming avoids.  rows_per_part
         * itself stays on total_rows: it models the concat fallback's cost
         * (that path flattens every row, not just survivors), so the density
         * test correctly keeps low-cardinality group-bys streaming even when
         * the filter is highly selective (few survivors). */
        if (g->selection) {
            ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
            int64_t pass_per_part = sm->total_pass / (n_parts > 0 ? n_parts : 1);
            if (est_groups > pass_per_part) est_groups = pass_per_part;
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
        ray_t* result = exec_group_per_partition(g, parted_tbl, ext, n_parts,
                                                 key_syms, agg_syms,
                                                 key_is_expr, agg_is_expr,
                                                 borrow_syms, n_borrow,
                                                 has_avg, has_stddev, group_limit);
        if (result) {
            scratch_free(key_syms_hdr);
            scratch_free(agg_syms_hdr);
            scratch_free(key_is_expr_hdr);
            scratch_free(agg_is_expr_hdr);
            scratch_free(borrow_hdr);
            scratch_free(esrc_hdr);
            return result;
        }
        /* NULL = per-partition failed, fall through to concat */
    }
    /* key_syms/agg_syms are unused past this point (the concat fallback
     * re-derives its columns via find_ext) — free before falling through. */
    scratch_free(key_syms_hdr);
    scratch_free(agg_syms_hdr);
    scratch_free(key_is_expr_hdr);
    scratch_free(agg_is_expr_hdr);
    scratch_free(borrow_hdr);
    scratch_free(esrc_hdr);

    /* ---- Concat fallback ---- */
    /* ---- Concat-only-needed-columns fallback ----
     * Used when query has AVG or expression keys/aggs.
     * Only concatenates the columns actually referenced by the GROUP BY. */
    {
        /* Collect needed column sym IDs (keys + agg inputs).  A bare OP_SCAN
         * key/agg contributes its own source column; an EXPRESSION key (e.g.
         * `(xbar time N)`) contributes the distinct source columns its subtree
         * scans — those MUST be carried into flat_tbl or exec_group's re-eval
         * of the key over flat_tbl fails "by: column not found" (the former
         * collector silently dropped expression-key sources).  A distinct
         * source column can never exceed the table's column count, and each
         * slot may reference several, so bound the union at n_keys+n_aggs+ncols
         * (min 1); the dedup keeps the live count ≤ ncols. */
        int64_t needed_max = (int64_t)n_keys + (int64_t)n_aggs + ncols;
        if (needed_max < 1) needed_max = 1;
        ray_t* needed_hdr = NULL;
        int64_t* needed = (int64_t*)scratch_alloc(&needed_hdr,
                (size_t)needed_max * sizeof(int64_t));
        int64_t esrc_max = ncols > 0 ? ncols : 1;
        ray_t* c_esrc_hdr = NULL;
        int64_t* c_esrc = (int64_t*)scratch_alloc(&c_esrc_hdr,
                (size_t)esrc_max * sizeof(int64_t));
        if (!needed || !c_esrc) {
            scratch_free(needed_hdr); scratch_free(c_esrc_hdr);
            return ray_error("oom", NULL);
        }
        int n_needed = 0;
        int copy_all = 0;   /* 1 = an unbounded/unknown ref forces "copy all" */
        for (uint32_t k = 0; k < n_keys && !copy_all; k++) {
            ray_op_ext_t* ke = find_ext(g, ext->keys[k]);
            if (ke && ke->base.opcode == OP_SCAN) {
                int dup = 0;
                for (int i = 0; i < n_needed; i++)
                    if (needed[i] == ke->sym) { dup = 1; break; }
                if (!dup) needed[n_needed++] = ke->sym;
            } else {
                /* Expression key — carry its source columns.  If the walk
                 * can't characterize the subtree, copy everything (safe). */
                int ns = group_expr_src_syms(g, ext->keys[k], c_esrc, (int)esrc_max);
                if (ns < 0) { copy_all = 1; break; }
                for (int s = 0; s < ns; s++) {
                    int dup = 0;
                    for (int i = 0; i < n_needed; i++)
                        if (needed[i] == c_esrc[s]) { dup = 1; break; }
                    if (!dup) needed[n_needed++] = c_esrc[s];
                }
            }
        }
        for (uint32_t a = 0; !copy_all && a < n_aggs; a++) {
            ray_op_ext_t* ae = find_ext(g, ext->agg_ins[a]);
            if (ae && ae->base.opcode == OP_SCAN) {
                int dup = 0;
                for (int i = 0; i < n_needed; i++)
                    if (needed[i] == ae->sym) { dup = 1; break; }
                if (!dup) needed[n_needed++] = ae->sym;
            } else {
                /* Expression agg input — carry its source columns; unknown
                 * subtrees copy everything (safe). */
                int ns = group_expr_src_syms(g, ext->agg_ins[a], c_esrc, (int)esrc_max);
                if (ns < 0) { copy_all = 1; break; }
                for (int s = 0; s < ns; s++) {
                    int dup = 0;
                    for (int i = 0; i < n_needed; i++)
                        if (needed[i] == c_esrc[s]) { dup = 1; break; }
                    if (!dup) needed[n_needed++] = c_esrc[s];
                }
            }
        }
        if (copy_all) n_needed = 0;   /* 0 ⇒ downstream copies all ncols */
        scratch_free(c_esrc_hdr);

        /* Build flat table with only needed columns (or all if n_needed==0) */
        ray_t* flat_tbl = ray_table_new(n_needed > 0 ? (int64_t)n_needed : ncols);
        if (!flat_tbl || RAY_IS_ERR(flat_tbl)) { scratch_free(needed_hdr); return flat_tbl; }

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
                                   ? parted_sym_max_attrs(segs, col->len) : 0;
                flat = typed_vec_new(base_type, base_attrs, total_rows);
                if (!flat || RAY_IS_ERR(flat)) {
                    ray_release(flat_tbl);
                    scratch_free(needed_hdr);
                    return ray_error("oom", NULL);
                }
                /* segment cells are copied id-preserving — all partitions
                 * resolve over the root symfile's domain (PARTED
                 * contract); adopt it from the first SYM segment. */
                if (base_type == RAY_SYM)
                    ray_sym_vec_adopt_domain(flat, sym_domain_rep(col));
                flat->len = total_rows;

                int64_t offset = 0;
                uint8_t seg_nulls = 0;
                for (int32_t p = 0; p < n_parts; p++) {
                    ray_t* seg = segs[p];
                    if (!seg || seg->len <= 0) continue;
                    parted_copy_cells(ray_data(flat), base_type, base_attrs,
                                      offset, seg, 0, seg->len);
                    seg_nulls |= seg->attrs & RAY_ATTR_HAS_NULLS;
                    offset += seg->len;
                }
                /* Null propagation: propagate HAS_NULLS across segments. */
                flat->attrs |= seg_nulls;
            }
            if (!flat || RAY_IS_ERR(flat)) {
                ray_release(flat_tbl);
                scratch_free(needed_hdr);
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
        scratch_free(needed_hdr);
        return result;
    }
}

static ray_t* exec_group_run(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                             int64_t group_limit);

/* Map an I32 dictionary-code result column back to strings via the source
 * column: code -> first_occ[code] -> the string at that row. */
static ray_t* dict_codes_to_str(const ray_t* codes_col, ray_t* src_col,
                                const ray_index_t* dix) {
    int64_t n = codes_col->len, ndist = dix->u.dict.n_distinct;
    const int32_t* cd   = (const int32_t*)ray_data((ray_t*)codes_col);
    const int32_t* focc = (const int32_t*)ray_data(dix->u.dict.first_occ);
    if (n <= 0) return ray_vec_new(RAY_STR, 1);
    /* Bulk-build in one pass: gather each group's source-string (ptr,len) — the
     * pointers are stable into src_col's descriptors/pool — then ray_str_vec_
     * from_parts sizes the pool once, avoiding the per-append realloc/COW (the
     * memmove that dominated dict_codes_to_str). */
    const char** ptrs = (const char**)ray_alloc_raw((size_t)n * sizeof(const char*));
    uint32_t*    lens = (uint32_t*)ray_alloc_raw((size_t)n * sizeof(uint32_t));
    if (ptrs && lens) {
        for (int64_t i = 0; i < n; i++) {
            int32_t code = cd[i];
            if (code < 0 || code >= ndist) { ptrs[i] = ""; lens[i] = 0; }
            else {
                size_t vl; const char* vp = ray_str_vec_get(src_col, focc[code], &vl);
                ptrs[i] = vp ? vp : "";
                lens[i] = vp ? (uint32_t)vl : 0;
            }
        }
        ray_t* out = ray_str_vec_from_parts(ptrs, lens, NULL, n);
        ray_free_raw(ptrs);
        ray_free_raw(lens);
        return out;
    }
    if (ptrs) ray_free_raw(ptrs);
    if (lens) ray_free_raw(lens);
    /* Fallback: per-append build (rare OOM of the scratch arrays). */
    ray_t* out = ray_vec_new(RAY_STR, n);
    if (!out || RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        int32_t code = cd[i];
        if (code < 0 || code >= ndist) { out = ray_str_vec_append(out, "", 0); }
        else {
            size_t vl; const char* vp = ray_str_vec_get(src_col, focc[code], &vl);
            out = ray_str_vec_append(out, vp ? vp : "", vp ? vl : 0);
        }
        if (!out || RAY_IS_ERR(out)) return out;
    }
    return out;
}

/* Thread-local stash of the result group dict codes so the
 * count-distinct path can build row_gid by remapping dict codes (cheap int)
 * instead of re-hashing strings. */
static _Thread_local ray_dict_cd_t tl_dict_cd;
static void ray_dict_cd_stash(ray_t* codes_col, int64_t n_groups, int64_t key_sym,
                              const ray_t* tbl, int64_t n_distinct) {
    if (tl_dict_cd.result_codes) { ray_free_raw(tl_dict_cd.result_codes); tl_dict_cd.result_codes = NULL; }
    tl_dict_cd.valid = 0;
    if (!codes_col || n_groups <= 0) return;
    int32_t* rc = ray_alloc_raw((size_t)n_groups * sizeof(int32_t));
    if (!rc) return;
    memcpy(rc, ray_data(codes_col), (size_t)n_groups * sizeof(int32_t));
    tl_dict_cd.result_codes = rc;
    tl_dict_cd.n_groups = n_groups;
    tl_dict_cd.key_sym = key_sym;
    tl_dict_cd.tbl = tbl;
    tl_dict_cd.n_distinct = n_distinct;
    tl_dict_cd.valid = 1;
}
ray_dict_cd_t ray_dict_cd_get(void) { return tl_dict_cd; }
void ray_dict_cd_clear(void) {
    if (tl_dict_cd.result_codes) ray_free_raw(tl_dict_cd.result_codes);
    memset(&tl_dict_cd, 0, sizeof(tl_dict_cd));
}

/* Known cardinality of group-key columns, keyed by column sym; lives in the
 * VM ctx (__VM->grp_card_*), mirroring the proj_keep query-scoped hint with a
 * save/publish/restore around each exec_group so a NESTED group-by (an outer
 * group whose survivors come from an inner group) cannot clobber the outer's
 * hint.  Populated by exec_group() when it swaps a dict-encoded STR key for
 * its int32 code vector (n_distinct); consulted by exec_group_v2's DA-
 * eligibility check to reject an infeasible composite up front (skipping the
 * min/max prescan) without re-deriving the code range by scanning survivors.
 * Dict codes are dense 0..n_distinct-1 over the FULL column, so over the
 * FILTERED survivors n_distinct is an upper bound on the filtered slot span
 * (a filter can only shrink the present code set).  Using it to reject is
 * conservative in the CORRECTNESS direction: it can only SKIP the DA path,
 * never wrongly ENABLE it, so it never changes results — though a selective
 * filter that compresses the surviving code range could make it over-reject
 * (a perf-only effect).  Generalizes to any key type with a cheaply-known
 * tight slot-span bound; dict-only for now. */
static void ray_grp_card_add(int64_t sym, int64_t n_distinct) {
    if (__VM && __VM->grp_card_n < 8) {
        __VM->grp_card_sym[__VM->grp_card_n] = sym;
        __VM->grp_card_val[__VM->grp_card_n] = n_distinct;
        __VM->grp_card_n++;
    }
}
static int64_t ray_grp_card_lookup(int64_t sym) {
    if (!__VM) return 0;
    /* grp_card_n is the VM's known-cardinality hint cache (capped at 8 by
     * ray_grp_card_add above) — unrelated to this GROUP op's n_keys/n_aggs. */
    for (uint8_t i = 0; i < __VM->grp_card_n; i++)
        if (__VM->grp_card_sym[i] == sym) return __VM->grp_card_val[i];
    return 0;
}

/* exec_group wrapper: when a plain STR scan key carries a RAY_IDX_DICT, group on
 * its int32 codes (cheap integer path) instead of the 16-byte descriptors, then
 * map codes -> strings on the small result.  Falls through otherwise. */
ray_t* exec_group(ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t group_limit) {
    ray_dict_cd_clear();
    /* Save the outer group-key cardinality hint and publish a fresh empty one
     * for this group (nest-safe, mirrors proj_keep in query.c); restore it on
     * every exit at the `grp_done:` label.  All consumption happens inside the
     * exec_group_run() calls below, before restore. */
    ray_vm_t* gvm = __VM;
    int64_t sv_grp_sym[8], sv_grp_val[8]; uint8_t sv_grp_n = 0;
    if (gvm) {
        sv_grp_n = gvm->grp_card_n;
        memcpy(sv_grp_sym, gvm->grp_card_sym, sizeof sv_grp_sym);
        memcpy(sv_grp_val, gvm->grp_card_val, sizeof sv_grp_val);
        gvm->grp_card_n = 0;
    }
    ray_t* result;
    /* Dict-STR key substitution admits any key count (unbounded-slots cut 4):
     * substituting each dict'd STR key with its int32 code vector rewrites the
     * group to integer keys, which exec_group_run's v2 engine serves at any
     * width.  The former n_keys > 8 bail is retired; key_col/dict_sym are one
     * scratch block sized to n_keys (a VLA can't cross the `goto grp_done`
     * bail-outs below).  wrap_hdr stays NULL on the early bails. */
    ray_t*  wrap_hdr = NULL;
    ray_t** key_col = NULL;     /* source STR column per dict'd key (for output) */
    int64_t* dict_sym = NULL;
    if (!tbl || RAY_IS_ERR(tbl) || tbl->type != RAY_TABLE) {
        result = exec_group_run(g, op, tbl, group_limit); goto grp_done;
    }
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext || ext->n_keys == 0) {
        result = exec_group_run(g, op, tbl, group_limit); goto grp_done;
    }

    uint32_t nk = ext->n_keys;
    key_col = (ray_t**)scratch_alloc(&wrap_hdr,
        (size_t)nk * (sizeof(ray_t*) + sizeof(int64_t)));
    if (!key_col) { result = exec_group_run(g, op, tbl, group_limit); goto grp_done; }
    dict_sym = (int64_t*)(key_col + nk);
    for (uint32_t k = 0; k < nk; k++) key_col[k] = NULL;
    bool any = false;
    for (uint32_t k = 0; k < nk; k++) {
        ray_op_t* key_op = op_node(g, ext->keys[k]);
        ray_op_ext_t* ke = key_op ? find_ext(g, key_op->id) : NULL;
        if (!ke || ke->base.opcode != OP_SCAN) continue;
        int64_t sym = ke->sym;
        ray_t* col = ray_table_get_col(tbl, sym);
        if (!col || col->type != RAY_STR) continue;
        if (ray_index_kind(col) != RAY_IDX_DICT) continue;
        /* Skip if any non-COUNT agg reads this key column as strings. */
        bool unsafe = false;
        for (uint32_t a = 0; a < ext->n_aggs; a++) {
            if (ext->agg_ops[a] == OP_COUNT) continue;
            ray_op_t* ain = op_node(g, ext->agg_ins[a]);
            ray_op_ext_t* ae = ain ? find_ext(g, ain->id) : NULL;
            if (ae && ae->base.opcode == OP_SCAN && ae->sym == sym) { unsafe = true; break; }
        }
        if (unsafe) continue;
        key_col[k] = col;
        dict_sym[k] = sym;
        any = true;
    }
    if (!any) { result = exec_group_run(g, op, tbl, group_limit); goto grp_done; }

    /* Substitute each dict'd STR key column with its int32 code vector. */
    int64_t ncols = ray_table_ncols(tbl);
    ray_t* sub = ray_table_new(ncols);
    if (!sub || RAY_IS_ERR(sub)) { result = exec_group_run(g, op, tbl, group_limit); goto grp_done; }
    for (int64_t c = 0; c < ncols; c++) {
        int64_t name = ray_table_col_name(tbl, c);
        ray_t* use = ray_table_get_col_idx(tbl, c);
        for (uint32_t k = 0; k < nk; k++)
            if (key_col[k] && dict_sym[k] == name) {
                const ray_index_t* dix = ray_index_payload(key_col[k]->index);
                use = dix->u.dict.codes;
                /* Record the dict's distinct-count so exec_group_v2's DA
                 * check can reject an infeasible composite without a scan. */
                ray_grp_card_add(name, dix->u.dict.n_distinct);
                break;
            }
        sub = ray_table_add_col(sub, name, use);
        if (!sub || RAY_IS_ERR(sub)) { result = exec_group_run(g, op, tbl, group_limit); goto grp_done; }
    }

    result = exec_group_run(g, op, sub, group_limit);
    ray_release(sub);

    if (result && !RAY_IS_ERR(result) && result->type == RAY_TABLE) {
        for (uint32_t k = 0; k < nk; k++) {
            if (!key_col[k]) continue;
            ray_t* codes_col = ray_table_get_col_idx(result, k);
            if (codes_col && codes_col->type == RAY_I32) {
                /* Stash the result group codes (single-key only) so
                 * the count-distinct can build row_gid from dict codes instead
                 * of re-hashing strings. */
                if (nk == 1) {
                    ray_dict_cd_stash(codes_col, ray_table_nrows(result),
                                      dict_sym[k], tbl,
                                      ray_index_payload(key_col[k]->index)->u.dict.n_distinct);
                }
                ray_t* str_col = dict_codes_to_str(
                    codes_col, key_col[k], ray_index_payload(key_col[k]->index));
                if (str_col && !RAY_IS_ERR(str_col)) {
                    ray_table_set_col_idx(result, k, str_col);
                    ray_release(str_col);
                } else if (str_col) ray_release(str_col);
            }
        }
    }
grp_done:
    scratch_free(wrap_hdr);   /* NULL on early bails → no-op */
    if (gvm) {
        gvm->grp_card_n = sv_grp_n;
        memcpy(gvm->grp_card_sym, sv_grp_sym, sizeof sv_grp_sym);
        memcpy(gvm->grp_card_val, sv_grp_val, sizeof sv_grp_val);
    }
    return result;
}

/* ============================================================================
 * Slice-group path — FILTER(in/eq on key col) + GROUP(by key col) fusion
 * ============================================================================
 *
 * The probe (exec.c ray_slice_group_probe) resolved the WHERE key set
 * against the group-key column's CSR hash index: g->sg_slices_hdr holds
 * one (domain-id, rows, n) slice per surviving key, ascending by domain
 * id — the DA path's emit order.  Aggregate each slice directly: the
 * filter scan, the survivor gather and group discovery all vanish; per
 * group the accumulation applies the same all_sum recurrence da_accum_row
 * applies row-by-row (same read helpers, same in-order accumulate), so
 * results match the generic path.  Ineligible shapes — unsupported aggs,
 * non-scan non-prod-fusable inputs, HAS_NULLS or F32/exotic agg columns,
 * limits, emit filters — return NULL and the caller folds the slices into
 * the equivalent selection. */

/* Row-chunk task: slice gi, rows [lo, hi) within that slice.  Slices are
 * chunked so one dominant key (a most-frequent sym can carry ~95% of the
 * surviving rows) still spreads across the pool; each task accumulates
 * into its own partial slot and the caller folds partials in task order,
 * so the per-group accumulation order is chunk-sequential — independent
 * of worker count. */
#define SG_CHUNK_ROWS 32768
typedef struct { int64_t gi, lo, hi; } sg_task_t;

typedef struct {
    const ray_idx_slice_t* slices;
    const sg_task_t*  tasks;
    ray_t* const*     agg_vecs;
    ray_t* const*     agg_vecs2;
    const agg_prod_t* prod;
    const uint16_t*   agg_ops;
    uint8_t           n_aggs;
    da_val_t*         partials; /* [n_tasks * n_aggs], zeroed */
    double*           partial_sumsq;
    double*           partial_sum_y;
    double*           partial_sumsq_y;
    double*           partial_sumxy;
    /* Shared-stream fusion: pair_sum[a] = sibling SUM agg slot whose bare
     * scan is prod[a]'s int side (-1 = none); fused_by[b] = the prod slot
     * that computes SUM b (-1 = b runs its own loop). */
    int8_t            pair_sum[16];
    int8_t            fused_by[16];
} sg_ctx_t;

/* Fused-product partial over a CONTIGUOUS row range — type-specialized
 * so the inner loop is a plain FMA-able stream (the per-row type switch
 * inside prod_val_f64 defeats vectorization).  Falls back to the
 * per-row reader for uncommon type pairs. */
static inline double sg_prod_range(const agg_prod_t* p, int64_t r0, int64_t n,
                                   int64_t* int_side_sum) {
    const void* pa = p->pa; const void* pb = p->pb;
    int8_t ta = p->ta, tb = p->tb;
    /* Canonicalize: put the F64 side (guaranteed by the fusion gate to
     * exist) in `fa`. */
    if (tb == RAY_F64 && ta != RAY_F64) {
        const void* tmp = pa; pa = pb; pb = tmp;
        int8_t tt = ta; ta = tb; tb = tt;
    }
    /* Four independent accumulator chains: the i64→f64 convert has no
     * packed AVX2 form, so these loops stay scalar — a single
     * accumulator then serializes at FMA latency (~4-5 cyc/row).  Four
     * chains keep the FMA and convert ports saturated instead. */
    double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    int64_t j = 0;
    if (ta == RAY_F64 && tb == RAY_F64) {
        const double* restrict x = (const double*)pa + r0;
        const double* restrict y = (const double*)pb + r0;
        for (; j + 4 <= n; j += 4) {
            a0 += x[j] * y[j];         a1 += x[j + 1] * y[j + 1];
            a2 += x[j + 2] * y[j + 2]; a3 += x[j + 3] * y[j + 3];
        }
        for (; j < n; j++) a0 += x[j] * y[j];
    } else if (ta == RAY_F64 && (tb == RAY_I64 || tb == RAY_TIME)) {
        const double*  restrict x = (const double*)pa + r0;
        const int64_t* restrict y = (const int64_t*)pb + r0;
        uint64_t s0 = 0;
        if (int_side_sum) {
            /* Shared-stream fusion: a sibling SUM over the product's int
             * side rides the same loads (row order preserved — the int
             * sum is order-exact either way). */
            for (; j + 4 <= n; j += 4) {
                a0 += x[j] * (double)y[j];         a1 += x[j + 1] * (double)y[j + 1];
                a2 += x[j + 2] * (double)y[j + 2]; a3 += x[j + 3] * (double)y[j + 3];
                s0 += (uint64_t)y[j] + (uint64_t)y[j + 1] +
                      (uint64_t)y[j + 2] + (uint64_t)y[j + 3];
            }
            for (; j < n; j++) { a0 += x[j] * (double)y[j]; s0 += (uint64_t)y[j]; }
            *int_side_sum = (int64_t)s0;
        } else {
            for (; j + 4 <= n; j += 4) {
                a0 += x[j] * (double)y[j];         a1 += x[j + 1] * (double)y[j + 1];
                a2 += x[j + 2] * (double)y[j + 2]; a3 += x[j + 3] * (double)y[j + 3];
            }
            for (; j < n; j++) a0 += x[j] * (double)y[j];
        }
    } else if (ta == RAY_F64 && tb == RAY_I32) {
        const double*  restrict x = (const double*)pa + r0;
        const int32_t* restrict y = (const int32_t*)pb + r0;
        uint64_t s0 = 0;
        if (int_side_sum) {
            for (; j + 4 <= n; j += 4) {
                a0 += x[j] * (double)y[j];         a1 += x[j + 1] * (double)y[j + 1];
                a2 += x[j + 2] * (double)y[j + 2]; a3 += x[j + 3] * (double)y[j + 3];
                s0 += (uint64_t)(int64_t)y[j] + (uint64_t)(int64_t)y[j + 1] +
                      (uint64_t)(int64_t)y[j + 2] + (uint64_t)(int64_t)y[j + 3];
            }
            for (; j < n; j++) { a0 += x[j] * (double)y[j]; s0 += (uint64_t)(int64_t)y[j]; }
            *int_side_sum = (int64_t)s0;
        } else {
            for (; j + 4 <= n; j += 4) {
                a0 += x[j] * (double)y[j];         a1 += x[j + 1] * (double)y[j + 1];
                a2 += x[j + 2] * (double)y[j + 2]; a3 += x[j + 3] * (double)y[j + 3];
            }
            for (; j < n; j++) a0 += x[j] * (double)y[j];
        }
    } else {
        if (int_side_sum) *int_side_sum = 0;   /* unreachable by pairing gate */
        for (; j < n; j++) a0 += prod_val_f64(p, r0 + j);
    }
    return (a0 + a1) + (a2 + a3);
}

static inline double sg_num_at(ray_t* v, int64_t row) {
    if (v->type == RAY_F64)
        return ((const double*)ray_data(v))[row];
    return (double)read_col_i64(ray_data(v), row, v->type, v->attrs);
}

static inline void sg_pair_accum(ray_t* x, ray_t* y, const int64_t* rows,
                                 int64_t n, bool contig, int64_t r0,
                                 double* sx, double* sy, double* sxx,
                                 double* syy, double* sxy) {
    double ax = 0.0, ay = 0.0, axx = 0.0, ayy = 0.0, axy = 0.0;
    if (contig) {
        for (int64_t j = 0; j < n; j++) {
            int64_t r = r0 + j;
            double xv = sg_num_at(x, r);
            double yv = sg_num_at(y, r);
            ax += xv; ay += yv;
            axx += xv * xv; ayy += yv * yv; axy += xv * yv;
        }
    } else {
        for (int64_t j = 0; j < n; j++) {
            int64_t r = rows[j];
            double xv = sg_num_at(x, r);
            double yv = sg_num_at(y, r);
            ax += xv; ay += yv;
            axx += xv * xv; ayy += yv * yv; axy += xv * yv;
        }
    }
    *sx = ax; *sy = ay; *sxx = axx; *syy = ayy; *sxy = axy;
}

static void sg_accum_fn(void* raw, uint32_t wid, int64_t tstart, int64_t tend) {
    (void)wid;
    sg_ctx_t* c = (sg_ctx_t*)raw;
    for (int64_t ti = tstart; ti < tend; ti++) {
        const sg_task_t* tk = &c->tasks[ti];
        const ray_idx_slice_t* sl = &c->slices[tk->gi];
        const int64_t* restrict rows = sl->rows + tk->lo;
        int64_t n = tk->hi - tk->lo;
        /* Parted layout: each key's rows form one contiguous run — the
         * accumulate then streams raw column pointers and vectorizes. */
        bool contig = (n > 0 && rows[n - 1] - rows[0] + 1 == n);
        int64_t r0 = (n > 0) ? rows[0] : 0;
        for (uint32_t a = 0; a < c->n_aggs; a++) {
            size_t idx = (size_t)ti * c->n_aggs + a;
            uint16_t op = c->agg_ops[a];
            if (agg_is_binary_agg(op)) {
                double sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
                sg_pair_accum(c->agg_vecs[a], c->agg_vecs2[a], rows, n,
                              contig, r0, &sx, &sy, &sxx, &syy, &sxy);
                c->partials[idx].f = sx;
                if (c->partial_sumsq)   c->partial_sumsq[idx] = sxx;
                if (c->partial_sum_y)   c->partial_sum_y[idx] = sy;
                if (c->partial_sumsq_y) c->partial_sumsq_y[idx] = syy;
                if (c->partial_sumxy)   c->partial_sumxy[idx] = sxy;
            } else if (c->prod[a].enabled) {
                /* Fused product: gated null-free, every row counts. */
                double acc = 0.0;
                if (contig) {
                    int8_t pb_slot = c->pair_sum[a];
                    if (pb_slot >= 0) {
                        int64_t iss = 0;
                        acc = sg_prod_range(&c->prod[a], r0, n, &iss);
                        c->partials[(size_t)ti * c->n_aggs + pb_slot].i = iss;
                    } else {
                        acc = sg_prod_range(&c->prod[a], r0, n, NULL);
                    }
                } else {
                    for (int64_t j = 0; j < n; j++)
                        acc += prod_val_f64(&c->prod[a], rows[j]);
                }
                c->partials[idx].f = acc;
            } else if (contig && c->fused_by[a] >= 0) {
                /* SUM computed by its paired product loop above (aggs
                 * iterate ascending and pairing enforces prod < sum? no —
                 * pairing is order-free: the prod branch writes this slot
                 * directly whichever order they appear in). */
            } else if (op == OP_COUNT) {
                /* counts[gi] above suffices. */
            } else if (c->agg_vecs[a]->type == RAY_F64) {
                /* NaN payload = null, skip from sum (mirror all_sum). */
                const double* restrict d =
                    (const double*)ray_data(c->agg_vecs[a]);
                double acc = 0.0;
                double ssq = 0.0;
                bool need_sq = c->partial_sumsq &&
                    (op == OP_STDDEV || op == OP_STDDEV_POP ||
                     op == OP_VAR || op == OP_VAR_POP);
                if (contig) {
                    const double* restrict dr = d + r0;
                    for (int64_t j = 0; j < n; j++) {
                        double v = dr[j];
                        if (RAY_LIKELY(v == v)) {
                            acc += v;
                            if (need_sq) ssq += v * v;
                        }
                    }
                } else {
                    for (int64_t j = 0; j < n; j++) {
                        double v = d[rows[j]];
                        if (RAY_LIKELY(v == v)) {
                            acc += v;
                            if (need_sq) ssq += v * v;
                        }
                    }
                }
                c->partials[idx].f = acc;
                if (need_sq) c->partial_sumsq[idx] = ssq;
            } else {
                /* Integer family, null-free by admission (unsigned wrap
                 * add mirrors da_accum_row). */
                ray_t* av = c->agg_vecs[a];
                const void* p = ray_data(av);
                int8_t t = av->type;
                uint8_t at = av->attrs;
                uint64_t acc = 0;
                double ssq = 0.0;
                bool need_sq = c->partial_sumsq &&
                    (op == OP_STDDEV || op == OP_STDDEV_POP ||
                     op == OP_VAR || op == OP_VAR_POP);
                if (contig && (t == RAY_I64 || t == RAY_TIME)) {
                    const int64_t* restrict x = (const int64_t*)p + r0;
                    for (int64_t j = 0; j < n; j++) {
                        int64_t v = x[j];
                        acc += (uint64_t)v;
                        if (need_sq) { double d = (double)v; ssq += d * d; }
                    }
                } else if (contig && t == RAY_I32) {
                    const int32_t* restrict x = (const int32_t*)p + r0;
                    for (int64_t j = 0; j < n; j++) {
                        int64_t v = (int64_t)x[j];
                        acc += (uint64_t)v;
                        if (need_sq) { double d = (double)v; ssq += d * d; }
                    }
                } else {
                    for (int64_t j = 0; j < n; j++) {
                        int64_t v = read_col_i64(p, rows[j], t, at);
                        acc += (uint64_t)v;
                        if (need_sq) { double d = (double)v; ssq += d * d; }
                    }
                }
                c->partials[idx].i = (int64_t)acc;
                if (need_sq) c->partial_sumsq[idx] = ssq;
            }
        }
    }
}

static int sg_cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

static inline void sg_hint_release(ray_graph_t* g) {
    if (g->sg_col)        { ray_release(g->sg_col); g->sg_col = NULL; }
    if (g->sg_slices_hdr) { ray_free(g->sg_slices_hdr); g->sg_slices_hdr = NULL; }
    g->sg_nslices = 0;
}

/* Ineligible-shape fallback: fold the resolved slices into exactly the
 * rowsel the skipped FILTER would have produced, so the generic paths
 * below run unchanged.  Returns an error (hint released) or NULL on
 * success with g->selection installed. */
static ray_t* sg_hint_to_selection(ray_graph_t* g, ray_t* tbl) {
    int64_t nrows = ray_table_nrows(tbl);
    const ray_idx_slice_t* sl = g->sg_slices_hdr
        ? (const ray_idx_slice_t*)ray_data(g->sg_slices_hdr) : NULL;
    int64_t K = g->sg_nslices;
    int64_t total = 0;
    for (int64_t i = 0; i < K; i++) total += sl[i].n;
    ray_t* sel = NULL;
    if (total == 0) {
        sel = ray_index_rowsel_from_ids(nrows, NULL, 0);
    } else {
        ray_t* hdr = ray_alloc((size_t)total * (int64_t)sizeof(int64_t));
        if (!hdr) { sg_hint_release(g); return ray_error("oom", NULL); }
        int64_t* ids = (int64_t*)ray_data(hdr);
        int64_t w = 0;
        for (int64_t i = 0; i < K; i++) {
            memcpy(ids + w, sl[i].rows, (size_t)sl[i].n * sizeof(int64_t));
            w += sl[i].n;
        }
        /* Slices are disjoint and each ascending; a plain sort restores
         * the global ascending order the rowsel builder requires. */
        qsort(ids, (size_t)total, sizeof(int64_t), sg_cmp_i64);
        sel = ray_index_rowsel_from_ids(nrows, ids, total);
        ray_free(hdr);
    }
    sg_hint_release(g);
    if (!sel) return ray_error("oom", NULL);
    g->selection = sel;
    return NULL;
}

/* Shape/agg admission shared by the kernel and by the early settle hook
 * (exec.c OP_GROUP runs it BEFORE its sparse pre-compaction so an
 * ineligible hint folds into a selection in time to benefit from
 * compaction).  On success fills agg_vecs (bare scan cols or NULL) and
 * prod (fused product plans). */
static bool sg_shape_eligible(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                              int64_t group_limit,
                              ray_t** agg_vecs, ray_t** agg_vecs2,
                              agg_prod_t* prod) {
    if (group_limit != 0) return false;
    if (ray_group_emit_filter_get().enabled) return false;
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext || ext->n_keys != 1 || ext->n_aggs < 1 || ext->n_aggs > 16)
        return false;
    if (ext->agg_k) return false;

    /* The single group key must be the bare scan of the probed column. */
    ray_op_ext_t* ke = find_ext(g, ext->keys[0]);
    if (!ke || ke->base.opcode != OP_SCAN) return false;
    ray_t* key_col = ray_table_get_col(tbl, ke->sym);
    if (!key_col || key_col != g->sg_col) return false;

    for (uint32_t a = 0; a < ext->n_aggs; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop == OP_COUNT) continue;    /* group size; input unused */
        bool pair = agg_is_binary_agg(aop);
        if (!pair &&
            aop != OP_SUM && aop != OP_AVG &&
            aop != OP_STDDEV && aop != OP_STDDEV_POP &&
            aop != OP_VAR && aop != OP_VAR_POP)
            return false;
        ray_op_t* in = op_node(g, ext->agg_ins[a]);
        if (!in) return false;
        if (!pair && (aop == OP_SUM || aop == OP_AVG) &&
            try_prod_sumavg_input_f64(g, tbl, in, &prod[a]))
            continue;
        ray_op_ext_t* ae = find_ext(g, in->id);
        if (!ae || ae->base.opcode != OP_SCAN) return false;
        ray_t* col = ray_table_get_col(tbl, ae->sym);
        if (!col || ray_is_atom(col)) return false;
        if (col->attrs & RAY_ATTR_HAS_NULLS) return false;
        if (!agg_type_admitted(aop, col->type)) return false;
        switch (col->type) {
            case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
            case RAY_TIME: case RAY_F64: break;
            default: return false;  /* F32 / exotic → generic path */
        }
        agg_vecs[a] = col;
        if (pair) {
            if (!ext->agg_ins2 || ext->agg_ins2[a] == RAY_OP_NONE) return false;
            ray_op_t* in2 = op_node(g, ext->agg_ins2[a]);
            if (!in2) return false;
            ray_op_ext_t* ae2 = find_ext(g, in2->id);
            if (!ae2 || ae2->base.opcode != OP_SCAN) return false;
            ray_t* col2 = ray_table_get_col(tbl, ae2->sym);
            if (!col2 || ray_is_atom(col2)) return false;
            if (col2->attrs & RAY_ATTR_HAS_NULLS) return false;
            if (!agg_type_admitted(aop, col2->type)) return false;
            switch (col2->type) {
                case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
                case RAY_F64: break;
                default: return false;
            }
            agg_vecs2[a] = col2;
        } else if (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE) {
            return false;
        }
    }
    return true;
}

/* Early settle hook — called from exec.c OP_GROUP before its sparse
 * pre-compaction: an armed hint whose group shape can't take the slice
 * path folds into the equivalent selection NOW, so downstream sparse
 * optimizations (pre-compaction, selection-aware iteration) see it.
 * Returns an error to propagate, or NULL (hint either left armed for
 * the kernel or folded into g->selection). */
ray_t* ray_group_slice_hint_settle(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                   int64_t group_limit) {
    if (!g->sg_col || !tbl || tbl->type != RAY_TABLE) return NULL;
    ray_t* agg_vecs[16] = {0};
    ray_t* agg_vecs2[16] = {0};
    agg_prod_t prod[16];
    memset(prod, 0, sizeof(prod));
    if (sg_shape_eligible(g, op, tbl, group_limit, agg_vecs, agg_vecs2, prod))
        return NULL;                      /* kernel will consume it */
    return sg_hint_to_selection(g, tbl);  /* fold; NULL on success */
}

static ray_t* exec_group_slices(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                int64_t group_limit) {
    ray_t* agg_vecs[16] = {0};
    ray_t* agg_vecs2[16] = {0};
    agg_prod_t prod[16];
    memset(prod, 0, sizeof(prod));
    if (!sg_shape_eligible(g, op, tbl, group_limit, agg_vecs, agg_vecs2, prod))
        return NULL;
    ray_op_ext_t* ext = find_ext(g, op->id);
    ray_op_ext_t* ke = find_ext(g, ext->keys[0]);
    ray_t* key_col = ray_table_get_col(tbl, ke->sym);
    uint32_t n_aggs = ext->n_aggs;

    int64_t K = g->sg_nslices;
    const ray_idx_slice_t* slices = (K > 0)
        ? (const ray_idx_slice_t*)ray_data(g->sg_slices_hdr) : NULL;

    bool need_sumsq = false, need_pair = false;
    for (uint32_t a = 0; a < n_aggs; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop == OP_STDDEV || aop == OP_STDDEV_POP ||
            aop == OP_VAR || aop == OP_VAR_POP ||
            agg_is_binary_agg(aop))
            need_sumsq = true;
        if (agg_is_binary_agg(aop))
            need_pair = true;
    }

    ray_t *sum_hdr = NULL, *cnt_hdr = NULL, *task_hdr = NULL, *part_hdr = NULL;
    ray_t *sumsq_hdr = NULL, *sum_y_hdr = NULL, *sumsq_y_hdr = NULL, *sumxy_hdr = NULL;
    ray_t *part_sumsq_hdr = NULL, *part_sum_y_hdr = NULL, *part_sumsq_y_hdr = NULL, *part_sumxy_hdr = NULL;
    da_val_t* sums = NULL;
    double *sumsq = NULL, *sum_y = NULL, *sumsq_y = NULL, *sumxy = NULL;
    int64_t* counts = NULL;
    if (K > 0) {
        sums = (da_val_t*)scratch_calloc(&sum_hdr,
                   (size_t)K * n_aggs * sizeof(da_val_t));
        counts = (int64_t*)scratch_calloc(&cnt_hdr,
                   (size_t)K * sizeof(int64_t));
        if (need_sumsq)
            sumsq = (double*)scratch_calloc(&sumsq_hdr,
                       (size_t)K * n_aggs * sizeof(double));
        if (need_pair) {
            sum_y = (double*)scratch_calloc(&sum_y_hdr,
                       (size_t)K * n_aggs * sizeof(double));
            sumsq_y = (double*)scratch_calloc(&sumsq_y_hdr,
                       (size_t)K * n_aggs * sizeof(double));
            sumxy = (double*)scratch_calloc(&sumxy_hdr,
                       (size_t)K * n_aggs * sizeof(double));
        }
        /* Chunk slices into row tasks so one dominant key still spreads
         * across the pool (see sg_task_t). */
        int64_t n_tasks = 0;
        for (int64_t i = 0; i < K; i++)
            n_tasks += (slices[i].n + SG_CHUNK_ROWS - 1) / SG_CHUNK_ROWS;
        sg_task_t* tasks = NULL;
        da_val_t* partials = NULL;
        double *part_sumsq = NULL, *part_sum_y = NULL, *part_sumsq_y = NULL, *part_sumxy = NULL;
        if (sums && counts) {
            task_hdr = ray_alloc((size_t)n_tasks * (int64_t)sizeof(sg_task_t));
            tasks = task_hdr ? (sg_task_t*)ray_data(task_hdr) : NULL;
            partials = (da_val_t*)scratch_calloc(&part_hdr,
                           (size_t)n_tasks * n_aggs * sizeof(da_val_t));
            if (need_sumsq)
                part_sumsq = (double*)scratch_calloc(&part_sumsq_hdr,
                               (size_t)n_tasks * n_aggs * sizeof(double));
            if (need_pair) {
                part_sum_y = (double*)scratch_calloc(&part_sum_y_hdr,
                               (size_t)n_tasks * n_aggs * sizeof(double));
                part_sumsq_y = (double*)scratch_calloc(&part_sumsq_y_hdr,
                               (size_t)n_tasks * n_aggs * sizeof(double));
                part_sumxy = (double*)scratch_calloc(&part_sumxy_hdr,
                               (size_t)n_tasks * n_aggs * sizeof(double));
            }
        }
        if (!sums || !counts || !tasks || !partials ||
            (need_sumsq && (!sumsq || !part_sumsq)) ||
            (need_pair && (!sum_y || !sumsq_y || !sumxy ||
                           !part_sum_y || !part_sumsq_y || !part_sumxy))) {
            scratch_free(sum_hdr); scratch_free(cnt_hdr);
            scratch_free(sumsq_hdr); scratch_free(sum_y_hdr);
            scratch_free(sumsq_y_hdr); scratch_free(sumxy_hdr);
            if (task_hdr) ray_free(task_hdr);
            scratch_free(part_hdr);
            scratch_free(part_sumsq_hdr); scratch_free(part_sum_y_hdr);
            scratch_free(part_sumsq_y_hdr); scratch_free(part_sumxy_hdr);
            return NULL;    /* OOM → generic path via fallback */
        }
        int64_t total_rows = 0, tw = 0;
        for (int64_t i = 0; i < K; i++) {
            total_rows += slices[i].n;
            for (int64_t lo = 0; lo < slices[i].n; lo += SG_CHUNK_ROWS) {
                int64_t hi = lo + SG_CHUNK_ROWS;
                if (hi > slices[i].n) hi = slices[i].n;
                tasks[tw].gi = i; tasks[tw].lo = lo; tasks[tw].hi = hi; tw++;
            }
        }
        sg_ctx_t ctx = { slices, tasks, agg_vecs, agg_vecs2, prod, ext->agg_ops,
                         n_aggs, partials, part_sumsq, part_sum_y,
                         part_sumsq_y, part_sumxy, {0}, {0} };
        /* Shared-stream pairing: a bare-scan SUM/AVG over the same column
         * a product's int side already streams rides the product loop —
         * one pass over the column instead of two. */
        /* pair_sum/fused_by are fixed sg_ctx_t scratch, [16] admitted via
         * sg_shape_eligible's ≤16 aggs gate, which runs before this call
         * site — not the exec_group_run width guard that runs after. */
        for (uint8_t a = 0; a < 16; a++) { ctx.pair_sum[a] = -1; ctx.fused_by[a] = -1; }
        for (uint32_t a = 0; a < n_aggs; a++) {
            if (!prod[a].enabled) continue;
            const void* ip; int8_t it;
            if (prod[a].ta != RAY_F64)      { ip = prod[a].pa; it = prod[a].ta; }
            else if (prod[a].tb != RAY_F64) { ip = prod[a].pb; it = prod[a].tb; }
            else continue;                   /* F64×F64 — no int side */
            if (it != RAY_I64 && it != RAY_TIME && it != RAY_I32) continue;
            for (uint32_t b = 0; b < n_aggs; b++) {
                if (b == a || !agg_vecs[b] || ctx.fused_by[b] >= 0) continue;
                if (ext->agg_ops[b] != OP_SUM && ext->agg_ops[b] != OP_AVG) continue;
                if (ray_data(agg_vecs[b]) != ip || agg_vecs[b]->type != it) continue;
                ctx.pair_sum[a] = (int8_t)b;
                ctx.fused_by[b] = (int8_t)a;
                break;
            }
        }
        ray_pool_t* pool = ray_pool_get();
        /* dispatch_n: n_tasks is a task count (tens), far below the
         * element-grain of ray_pool_dispatch, which would lump them
         * into ONE serial task. */
        if (pool && n_tasks > 1 && total_rows >= RAY_PARALLEL_THRESHOLD)
            ray_pool_dispatch_n(pool, sg_accum_fn, &ctx, (uint32_t)n_tasks);
        else
            sg_accum_fn(&ctx, 0, 0, n_tasks);
        /* Fold task partials in task order — chunk-sequential per group,
         * independent of worker count. */
        for (int64_t ti = 0; ti < n_tasks; ti++) {
            int64_t gi = tasks[ti].gi;
            counts[gi] += tasks[ti].hi - tasks[ti].lo;
            for (uint32_t a = 0; a < n_aggs; a++) {
                size_t di = (size_t)gi * n_aggs + a;
                size_t si = (size_t)ti * n_aggs + a;
                bool pair = agg_is_binary_agg(ext->agg_ops[a]);
                if (pair || prod[a].enabled || ext->agg_ops[a] == OP_COUNT ||
                    (agg_vecs[a] && agg_vecs[a]->type == RAY_F64))
                    sums[di].f += partials[si].f;
                else
                    sums[di].i = (int64_t)((uint64_t)sums[di].i +
                                           (uint64_t)partials[si].i);
                if (sumsq) sumsq[di] += part_sumsq[si];
                if (pair) {
                    sum_y[di] += part_sum_y[si];
                    sumsq_y[di] += part_sumsq_y[si];
                    sumxy[di] += part_sumxy[si];
                }
            }
        }
        ray_free(task_hdr);
        scratch_free(part_hdr);
        scratch_free(part_sumsq_hdr); scratch_free(part_sum_y_hdr);
        scratch_free(part_sumsq_y_hdr); scratch_free(part_sumxy_hdr);
    }

    /* Emit: key column (domain ids in slice order), then the shared agg
     * emitter — same types, same divisors, same fusion families as the
     * DA path. */
    ray_t* result = ray_table_new(1 + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        scratch_free(sum_hdr); scratch_free(cnt_hdr);
        scratch_free(sumsq_hdr); scratch_free(sum_y_hdr);
        scratch_free(sumsq_y_hdr); scratch_free(sumxy_hdr);
        return NULL;
    }
    ray_t* kc = col_vec_new(key_col, K > 0 ? K : 1);
    if (!kc || RAY_IS_ERR(kc)) {
        if (kc) ray_release(kc);
        ray_release(result);
        scratch_free(sum_hdr); scratch_free(cnt_hdr);
        scratch_free(sumsq_hdr); scratch_free(sum_y_hdr);
        scratch_free(sumsq_y_hdr); scratch_free(sumxy_hdr);
        return NULL;
    }
    if (kc->type == RAY_SYM)
        ray_sym_vec_adopt_domain(kc, sym_domain_rep(key_col));
    kc->len = K;
    for (int64_t gi = 0; gi < K; gi++)
        write_col_i64(ray_data(kc), gi, slices[gi].dom,
                      key_col->type, kc->attrs);
    result = ray_table_add_col(result, ke->sym, kc);
    ray_release(kc);
    if (!result || RAY_IS_ERR(result)) {
        scratch_free(sum_hdr); scratch_free(cnt_hdr);
        scratch_free(sumsq_hdr); scratch_free(sum_y_hdr);
        scratch_free(sumsq_y_hdr); scratch_free(sumxy_hdr);
        return result;
    }

    emit_agg_columns(&result, g, ext, agg_vecs, (uint32_t)K, n_aggs,
                     (double*)sums, (int64_t*)sums,
                     NULL, NULL, NULL, NULL,
                     counts, NULL, prod, sumsq, NULL, sum_y, sumsq_y, sumxy);

    scratch_free(sum_hdr); scratch_free(cnt_hdr);
    scratch_free(sumsq_hdr); scratch_free(sum_y_hdr);
    scratch_free(sumsq_y_hdr); scratch_free(sumxy_hdr);
    return result;
}

/* v2 bridge for expression agg inputs — see the call site in
 * exec_group_run.  Returns the v2 result (or an error), or NULL when the
 * shape is ineligible and the caller should continue on the legacy path. */
static ray_t* exec_group_v2_exprs(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    if (!tbl || tbl->type != RAY_TABLE) return NULL;
    ray_op_ext_t* ext = find_ext(g, op->id);
    /* Own gate: this v2-expression shadow path serves any key/agg count
     * (unbounded-slots cut 4 — the former 16 caps are retired).  Its scratch
     * (synth/names/in_ids/keys/ins/ins2) is now VLA-sized to n_aggs/n_keys and
     * every index loop below is uint32_t. */
    if (!ext || ext->n_keys < 1 || ext->n_aggs < 1) return NULL;

    /* At least one agg input must be a compiled-expression node; scans,
     * COUNT and binary/holistic second inputs pass through untouched. */
    int64_t nrows = ray_table_nrows(tbl);
    if (nrows <= 0) return NULL;
    uint32_t na = ext->n_aggs, nkk = ext->n_keys;
    bool any_expr = false;
    for (uint32_t a = 0; a < na; a++) {
        if (ext->agg_ops[a] == OP_COUNT) continue;
        ray_op_t* in = op_node(g, ext->agg_ins[a]);
        if (!in) return NULL;
        if (in->opcode == OP_SCAN || in->opcode == OP_CONST) continue;
        any_expr = true;
    }
    if (!any_expr) return NULL;

    /* Materialize expression inputs (full length — the selection, if any,
     * is applied by exec_group_v2 itself).  VLA scratch, n_aggs/n_keys-sized;
     * names[a] holds "_e" + up to 10 uint32 digits + NUL ≤ 16. */
    ray_t* synth[na];
    char   names[na][16];
    for (uint32_t a = 0; a < na; a++) synth[a] = NULL;
    ray_t* sub = NULL;
    bool ok = true;
    for (uint32_t a = 0; a < na && ok; a++) {
        if (ext->agg_ops[a] == OP_COUNT) continue;
        ray_op_t* in = op_node(g, ext->agg_ins[a]);
        if (in->opcode == OP_SCAN || in->opcode == OP_CONST) continue;
        snprintf(names[a], sizeof(names[a]), "_e%u", (unsigned)a);
        if (ray_table_get_col(tbl, ray_sym_intern(names[a], strlen(names[a])))) {
            ok = false; break;   /* user column shadows the synthetic name */
        }
        ray_expr_t ex;
        if (!expr_compile(g, tbl, in, &ex)) { ok = false; break; }
        ray_t* v = expr_eval_full(&ex, nrows);
        if (!v || RAY_IS_ERR(v) || !ray_is_vec(v) || v->len != nrows) {
            if (v && !RAY_IS_ERR(v)) ray_release(v);
            else if (v) ray_release(v);
            ok = false; break;
        }
        synth[a] = v;
    }
    if (ok) {
        int64_t ncols = ray_table_ncols(tbl);
        sub = ray_table_new(ncols + na);
        if (!sub || RAY_IS_ERR(sub)) { ok = false; }
        for (int64_t c = 0; c < ncols && ok; c++) {
            sub = ray_table_add_col(sub, ray_table_col_name(tbl, c),
                                    ray_table_get_col_idx(tbl, c));
            if (!sub || RAY_IS_ERR(sub)) ok = false;
        }
        for (uint32_t a = 0; a < na && ok; a++) {
            if (!synth[a]) continue;
            sub = ray_table_add_col(sub,
                    ray_sym_intern(names[a], strlen(names[a])), synth[a]);
            if (!sub || RAY_IS_ERR(sub)) ok = false;
        }
    }
    ray_op_t* op2 = NULL;
    if (ok) {
        /* Shadow GROUP node: original key/scan/second inputs by node,
         * expression inputs replaced by scans of the synthetic columns.
         * Build all scans BEFORE the group node — node creation may
         * realloc g->nodes and dangle earlier ray_op_t pointers. */
        uint32_t in_ids[na];
        bool has2 = false, hask = false;
        for (uint32_t a = 0; a < na && ok; a++) {
            if (synth[a]) {
                ray_op_t* sc = ray_scan(g, names[a]);
                if (!sc) { ok = false; break; }
                in_ids[a] = sc->id;
            } else {
                in_ids[a] = ext->agg_ins[a];
            }
            if (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE) has2 = true;
            if (ext->agg_k && ext->agg_k[a]) hask = true;
        }
        if (ok) {
            ray_op_t* keys[nkk];
            ray_op_t* ins[na];
            ray_op_t* ins2[na];
            for (uint32_t k = 0; k < nkk; k++)
                keys[k] = op_node(g, ext->keys[k]);
            for (uint32_t a = 0; a < na; a++) {
                ins[a] = op_node(g, in_ids[a]);
                ins2[a] = (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE)
                    ? op_node(g, ext->agg_ins2[a]) : NULL;
            }
            if (hask || has2)
                op2 = ray_group_build(g, keys, ext->n_keys, ext->agg_ops, ins,
                                      has2 ? ins2 : NULL,
                                      hask ? ext->agg_k : NULL,
                                      ext->n_aggs);
            else
                op2 = ray_group(g, keys, ext->n_keys, ext->agg_ops, ins,
                                ext->n_aggs);
            if (!op2) ok = false;
        }
    }
    ray_t* result = NULL;
    if (ok && agg_v2_can_handle(g, op2, sub))
        result = exec_group_v2(g, op2, sub);
    for (uint32_t a = 0; a < na; a++)
        if (synth[a]) ray_release(synth[a]);
    if (sub) ray_release(sub);
    return result;   /* NULL → legacy path */
}

/* ---- q34/count hot path extracted from exec_group_run ------------------
 * Single-key (n_keys==1) sparse-dense emit-filter path: the dict-SYM
 * count-only scatter (`range_count[off]++`) plus its keep-min compaction
 * and result emit.  Pulled out of exec_group_run — a 3700-line monolith
 * whose ~1600-line cut-4 growth degraded this loop's register allocation
 * (stack spills at ~1MB frame offsets, +5.5% executed instructions on q34)
 * and unseated the alignment the checkpoint pin had tuned.  Isolating it in
 * its own noinline function gives it an independent register allocation and
 * code layout (mirrors exec_group_per_partition's noinline isolation).
 * Called once per dispatch — no per-row overhead at the boundary.
 * Returns the group result (or an OOM/error object) when it handled the
 * query; returns NULL when the dynamic-dense probe bailed (unbounded key or
 * no surviving row), leaving all shared resources untouched so the caller
 * continues on the next sp path. */
typedef struct {
    ray_graph_t*   g;
    ray_op_ext_t*  ext;
    void**         key_data;
    int8_t*        key_types;
    ray_t**        key_vecs;
    uint8_t*       key_owned;
    void**         agg_ptrs;
    int8_t*        agg_types;
    uint8_t*       agg_strlen;
    ray_t**        agg_vecs;
    uint8_t*       agg_owned;
    agg_affine_t*  agg_affine;
    agg_prod_t*    agg_prod;
    ray_t**        strlen_sym_strings;
    uint32_t       strlen_sym_count;
    uint64_t       agg_f64_mask;
    uint32_t       n_aggs;
    uint32_t       n_keys;
    int64_t        n_scan;
    uint8_t        key_esz;
    bool           sp_need_sum;
    const int64_t* match_idx;
    ray_t*         rowsel;
    ray_t*         match_idx_block;
    ray_t*         vla_hdr;
    ray_group_emit_filter_t emit_filter;
} sp_dyn_ctx_t;

static ray_t* __attribute__((noinline))
exec_group_sp_dyn_emit(const sp_dyn_ctx_t* c) {
    ray_graph_t*   g          = c->g;
    ray_op_ext_t*  ext        = c->ext;
    void**         key_data   = c->key_data;
    int8_t*        key_types  = c->key_types;
    ray_t**        key_vecs   = c->key_vecs;
    uint8_t*       key_owned  = c->key_owned;
    void**         agg_ptrs   = c->agg_ptrs;
    int8_t*        agg_types  = c->agg_types;
    uint8_t*       agg_strlen = c->agg_strlen;
    ray_t**        agg_vecs   = c->agg_vecs;
    uint8_t*       agg_owned  = c->agg_owned;
    agg_affine_t*  agg_affine = c->agg_affine;
    agg_prod_t*    agg_prod   = c->agg_prod;
    ray_t**        strlen_sym_strings = c->strlen_sym_strings;
    uint32_t       strlen_sym_count   = c->strlen_sym_count;
    uint64_t       agg_f64_mask = c->agg_f64_mask;
    uint32_t       n_aggs     = c->n_aggs;
    uint32_t       n_keys     = c->n_keys;
    int64_t        n_scan     = c->n_scan;
    uint8_t        key_esz    = c->key_esz;
    bool           sp_need_sum = c->sp_need_sum;
    const int64_t* match_idx  = c->match_idx;
    ray_t*         rowsel     = c->rowsel;
    ray_t*         match_idx_block = c->match_idx_block;
    ray_t*         vla_hdr    = c->vla_hdr;
    ray_group_emit_filter_t emit_filter = c->emit_filter;

                uint64_t cap = key_esz == 1 ? 256u
                             : key_esz == 2 ? (1u << 16)
                             : (1u << 20);
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
            for (uint32_t a = 0; a < n_aggs; a++) {                               \
                if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a]) continue;       \
                if (agg_strlen[a])                                               \
                    sums[a].i += group_strlen_at_cached(                         \
                        agg_vecs[a], dyn_row, strlen_sym_strings, strlen_sym_count); \
                else if (agg_f64_mask & ((uint64_t)1 << a))                      \
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
                        for (uint32_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint32_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
                        return result ? result : ray_error("oom", NULL);
                    }

                    ray_t* key_col = col_vec_new(key_vecs[0], (int64_t)grp_count);
                    out_col_adopt_str_pool(key_col, key_vecs[0]);
                    if (key_col && !RAY_IS_ERR(key_col) && key_col->type == RAY_SYM)
                        /* raw cell ids from key_vecs[0] — adopt its domain */
                        ray_sym_vec_adopt_domain(key_col, sym_domain_rep(key_vecs[0]));
                    if (!key_col || RAY_IS_ERR(key_col)) {
                        scratch_free(range_sum_hdr); scratch_free(cnt_hdr);
                        ray_release(result);
                        for (uint32_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint32_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
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
                        for (uint32_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint32_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
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
        for (uint32_t a = 0; a < n_aggs; a++) {                                   \
            if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a]) continue;           \
            if (agg_strlen[a])                                                   \
                sums[a].i += group_strlen_at_cached(                             \
                    agg_vecs[a], dyn_row, strlen_sym_strings, strlen_sym_count); \
            else if (agg_f64_mask & ((uint64_t)1 << a))                          \
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

                    ray_op_ext_t* key_ext = find_ext(g, ext->keys[0]);
                    int64_t name_id = key_ext ? key_ext->sym : 0;
                    result = ray_table_add_col(result, name_id, key_col);
                    ray_release(key_col);
                    /* nn_counts == NULL: this fast path rejected HAS_NULLS
                     * inputs at the sp_eligible gate (~line 5737), so every
                     * row is non-null and the legacy count-based divisor is
                     * correct. */
                    emit_agg_columns(&result, g, ext, agg_vecs, grp_count, n_aggs,
                                     (double*)dense_sum, (int64_t*)dense_sum,
                                     NULL, NULL, NULL, NULL,
                                     dense_count, agg_affine, agg_prod, NULL, NULL,
                                     NULL, NULL, NULL);

                    scratch_free(_h_sum); scratch_free(_h_cnt);
                    scratch_free(range_sum_hdr); scratch_free(cnt_hdr);
                    for (uint32_t a = 0; a < n_aggs; a++)
                        if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                    for (uint32_t k = 0; k < n_keys; k++)
                        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                    if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
                    return result;
                }

                scratch_free(range_sum_hdr);
                scratch_free(cnt_hdr);

    /* Dynamic-dense probe bailed (unbounded key or no surviving row): shared
     * resources are untouched — caller continues on the next sp path. */
    return NULL;
}

/* Pin the DA-prescan dense-group finalize alignment.  This function owns the
 * dict/dense-array (DA) group path whose finalize loop (the total_groups /
 * keep_min / range_count compaction region below, ~line 8395-8475, family
 * da_count_emit_keep_min_u32) is the dominant symbol for high-cardinality
 * group-by + count + desc-sort + take shapes.  The unbounded-slots work
 * added lines elsewhere in group.c
 * (the legacy ght_layout_t hash path — unrelated, byte-identical to baseline
 * at the hot lines), shifting this loop's absolute address/alignment and
 * dropping IPC 2.07→1.96 (+7.2% cycles, +1.6% instructions — a placement
 * artifact, not new work; RCA in .superpowers/sdd/task-2-bench-checkpoint.md).
 * Same remedy as reduce_range's cut-3 pin: aligned(64) stabilizes the entry
 * cacheline; align-loops=32 re-pins every loop head and align-jumps=32 the
 * unrolled branch targets, making the finalize loop's internal alignment
 * invariant to future whole-TU layout churn.  Per-function attributes only —
 * no global codegen flag.  GCC-only: clang lacks the `optimize` attribute and
 * -Werror would promote the unknown-attribute warning; the pin is
 * performance-only, so clang builds go without it. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((aligned(64), optimize("align-loops=32","align-jumps=32")))
#endif
static ray_t* exec_group_run(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
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

    /* Slice-group hint (armed instead of the WHERE filter — see
     * ray_slice_group_probe): aggregate the key slices directly, or
     * fold them into the selection the skipped filter would have
     * produced and continue on the generic paths. */
    if (g->sg_col) {
        ray_t* sgr = exec_group_slices(g, op, tbl, group_limit);
        if (sgr) { sg_hint_release(g); return sgr; }
        ray_t* err = sg_hint_to_selection(g, tbl);
        if (err) return err;
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

    /* v2 doesn't implement the top-count emit filter (old-engine feature);
     * when one is active, stay on the legacy path that honors it. */
    if (ray_agg_engine_v2 && group_limit == 0
        && !ray_group_emit_filter_get().enabled
        && agg_v2_can_handle(g, op, tbl))
        return exec_group_v2(g, op, tbl);

    /* v2 with EXPRESSION agg inputs: v2 admission requires plain-column
     * scans, so a group like {sum(a*b), stddev(c), cor(x,y)} — where ONE
     * input is a MUL — used to drop the whole 6-agg pass onto the legacy
     * row-layout path (per-row accum_from_entry + full-width product
     * materialization: q42's 9.25M-row group ran 540ms where its aggs
     * individually cost ~7-10ms each under v2).  Materialize each
     * expression input once (expr_eval_full — pool-parallel), extend the
     * table with synthetic `_e{a}` columns, point a shadow GROUP node's
     * inputs at scans of them, and dispatch v2.  `_e{a}` is chosen so
     * v2's scan-input naming (agg_result_col_name) emits the SAME
     * `_e{a}_{op}` output names the legacy expression emit produced.
     * Any ineligibility falls through to the legacy path unchanged. */
    if (ray_agg_engine_v2 && group_limit == 0
        && !ray_group_emit_filter_get().enabled) {
        ray_t* r = exec_group_v2_exprs(g, op, tbl);
        if (r) return r;
    }

    int64_t nrows = ray_table_nrows(tbl);
    uint32_t n_keys = ext->n_keys;
    uint32_t n_aggs = ext->n_aggs;

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
        ray_op_ext_t* key_ext_probe = find_ext(g, ext->keys[0]);
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
                for (uint32_t a = 0; a < n_aggs; a++) {
                    uint16_t aop = ext->agg_ops[a];
                    ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]);
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
            ray_op_ext_t* key_ext = find_ext(g, ext->keys[0]);
            int64_t src_sym = ray_sym_intern("_src", 4);
            if (key_ext && key_ext->base.opcode == OP_SCAN &&
                key_ext->sym == src_sym) {
                /* Verify all aggs are compatible with factorized data:
                 * COUNT(*) → use _count directly
                 * SUM(_count) → use _count directly */
                bool all_compat = true;
                for (uint32_t a = 0; a < n_aggs; a++) {
                    uint16_t aop = ext->agg_ops[a];
                    ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]);
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
                        for (uint32_t a = 0; a < n_aggs; a++) {
                            ray_retain(cnt_col);
                            int64_t agg_name = ray_sym_intern("_agg", 4);
                            if (n_aggs > 1) {
                                char buf[16];
                                int n = snprintf(buf, sizeof(buf), "_agg%u", a);
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

    /* Width guard RETIRED (unbounded-slots cut 4): the legacy ght/scalar
     * machinery now serves any key/agg count.  ght_layout_t is unbounded
     * (inline-or-spill); group_ht_t's key_data/key_pool wide-key tables are
     * base pointers (inline ≤8, heap-spilled wider); every fixed entry-staging
     * buffer on this path (ebuf/keys/agg_vals/keybuf/ek_buf/kpool + the driver
     * key_data/key_types/key_attrs) is layout-sized; and per-key null tracking
     * uses ceil(n_keys/64) mask words (no <64 cap).  Shapes that reach here are
     * exactly the v2/v2-exprs declines (F64/STR/GUID/expression keys, or
     * v2-ineligible aggs like FIRST/LAST/PROD/narrow-int-SUM), at any width. */

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

    /* Resolve key columns (VLA scales with n_keys — the ≤8-key guard is
     * retired; use ≥1 to avoid zero-size VLA UB). */
    uint32_t vla_keys = n_keys > 0 ? n_keys : 1;
    ray_t* key_vecs[vla_keys];
    memset(key_vecs, 0, vla_keys * sizeof(ray_t*));

    uint8_t key_owned[vla_keys]; /* 1 = we allocated via exec_node, must free */
    memset(key_owned, 0, vla_keys * sizeof(uint8_t));
    /* Source-column sym per key (OP_SCAN keys only; -1 for expression keys) —
     * used to consult the dict known-cardinality map for the DA-reject. */
    int64_t key_scan_sym[vla_keys];
    for (uint32_t k = 0; k < vla_keys; k++) key_scan_sym[k] = -1;
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_op_t* key_op = op_node(g, ext->keys[k]);
        ray_op_ext_t* key_ext = find_ext(g, key_op->id);
        if (key_ext && key_ext->base.opcode == OP_SCAN) {
            key_vecs[k] = ray_table_get_col(tbl, key_ext->sym);
            key_scan_sym[k] = key_ext->sym;
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
            for (uint32_t j = 0; j < k; j++)
                if (key_owned[j] && key_vecs[j]) ray_release(key_vecs[j]);
            return ray_error("domain", "by: column not found in table");
        }
    }

    /* Resolve agg input columns.  n_aggs is unbounded regardless of n_keys
     * (the old ≤8-agg keyed guard is retired — 2-key 9-agg FIRST/LAST shapes
     * flow here — and the keyless path never capped it; the cut-2 flip also
     * lifted query.c's 255 compile cap), so these per-agg arrays scale with
     * the query and must not sit on the stack.
     * One consolidated scratch carve: the two ray_t* arrays first, then the
     * affine/linear/prod structs (all int64/pointer-backed, so 8-aligned and
     * multiple-of-8 sized), then the three byte-flag arrays last.  Second
     * input column (agg_vecs2) is non-NULL only for binary aggs
     * (OP_PEARSON_CORR); it carries its own agg_owned2 because each side can
     * come from a different source (OP_SCAN literal or expr_compile).
     * calloc-zeroed: the cleanup loop reads agg_owned/agg_vecs slots the
     * resolve loop may leave unwritten on an early error, and the
     * *_affine/_linear/_prod `.enabled` fields must default false.
     * vla_aggs ≥ 1 avoids a zero-size carve.  vla_hdr is freed at every
     * function exit (each early return's manual-cleanup block and the shared
     * `cleanup:` label). */
    uint32_t vla_aggs = n_aggs > 0 ? n_aggs : 1;
    ray_t* vla_hdr = NULL;
    size_t vla_bytes = (size_t)vla_aggs * (2 * sizeof(ray_t*)
                        + sizeof(agg_affine_t) + sizeof(agg_linear_t)
                        + sizeof(agg_prod_t) + 3 * sizeof(uint8_t));
    uint8_t* vla_blk = (uint8_t*)scratch_calloc(&vla_hdr, vla_bytes);
    if (!vla_blk) {
        for (uint32_t k = 0; k < n_keys; k++)
            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
        return ray_error("oom", NULL);
    }
    ray_t**       agg_vecs   = (ray_t**)vla_blk;
    ray_t**       agg_vecs2  = agg_vecs + vla_aggs;
    agg_affine_t* agg_affine = (agg_affine_t*)(agg_vecs2 + vla_aggs);
    agg_linear_t* agg_linear = (agg_linear_t*)(agg_affine + vla_aggs);
    agg_prod_t*   agg_prod   = (agg_prod_t*)(agg_linear + vla_aggs);
    uint8_t*      agg_owned  = (uint8_t*)(agg_prod + vla_aggs);
    uint8_t*      agg_owned2 = agg_owned + vla_aggs;
    uint8_t*      agg_strlen = agg_owned2 + vla_aggs;

    for (uint32_t a = 0; a < n_aggs; a++) {
        ray_op_t* agg_input_op = op_node(g, ext->agg_ins[a]);
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

        /* SUM/AVG(a * b) float product: the scalar and DA accumulators
         * multiply per row; paths without the fused read (radix sp / HT)
         * materialize on entry via group_materialize_prod_slots. */
        if ((agg_kind == OP_SUM || agg_kind == OP_AVG) && nrows > 0 &&
            try_prod_sumavg_input_f64(g, tbl, agg_input_op, &agg_prod[a])) {
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
                if (vec && RAY_IS_ERR(vec)) ray_release(vec);
            }
            /* Fallback: full recursive evaluation */
            ray_t* saved_table = g->table;
            g->table = tbl;
            ray_t* vec = exec_node(g, agg_input_op);
            g->table = saved_table;
            if (vec && RAY_IS_ERR(vec)) {
                for (uint32_t i = 0; i < a; i++)
                    { if (agg_owned[i] && agg_vecs[i]) ray_release(agg_vecs[i]); if (agg_owned2[i] && agg_vecs2[i]) ray_release(agg_vecs2[i]); }
                for (uint32_t k = 0; k < n_keys; k++)
                    if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                scratch_free(vla_hdr);
                return vec;
            }
            if (vec) {
                agg_vecs[a] = vec;
                agg_owned[a] = 1;
            }
        }
    resolve_ins2:;
        /* Binary aggregators (OP_PEARSON_CORR): mirror the resolution
         * above for the y-side input.  Same OP_SCAN / OP_CONST / expr
         * fallback ladder, separate ownership flag because each side
         * may have come from a different source. */
        if (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE) {
            ray_op_t* agg_input_op2 = op_node(g, ext->agg_ins2[a]);
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
                    } else if (vec) {
                        ray_release(vec);
                    }
                }
                if (!compiled2) {
                    ray_t* saved_table = g->table;
                    g->table = tbl;
                    ray_t* vec = exec_node(g, agg_input_op2);
                    g->table = saved_table;
                    if (vec && RAY_IS_ERR(vec)) {
                        if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint32_t i = 0; i < a; i++)
                            { if (agg_owned[i] && agg_vecs[i]) ray_release(agg_vecs[i]); if (agg_owned2[i] && agg_vecs2[i]) ray_release(agg_vecs2[i]); }
                        for (uint32_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        scratch_free(vla_hdr);
                        return vec;
                    }
                    if (vec) {
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
    for (uint32_t a = 0; a < n_aggs; a++) {
        if (!agg_vecs[a] || RAY_IS_ERR(agg_vecs[a])) continue;
        if (ext->agg_ops[a] == OP_COUNT) continue; /* value is ignored for COUNT */
        if (agg_strlen[a]) continue;

        bool needs_broadcast = ray_is_atom(agg_vecs[a]) ||
                               (agg_vecs[a]->type > 0 && agg_vecs[a]->len == 1 && nrows > 1);
        if (!needs_broadcast) continue;

        ray_t* bcast = materialize_broadcast_input(agg_vecs[a], nrows);
        if (!bcast || RAY_IS_ERR(bcast)) {
            for (uint32_t i = 0; i < n_aggs; i++) {
                { if (agg_owned[i] && agg_vecs[i]) ray_release(agg_vecs[i]); if (agg_owned2[i] && agg_vecs2[i]) ray_release(agg_vecs2[i]); }
            }
            for (uint32_t k = 0; k < n_keys; k++) {
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            }
            scratch_free(vla_hdr);
            return bcast && RAY_IS_ERR(bcast) ? bcast : ray_error("oom", NULL);
        }

        if (agg_owned[a]) ray_release(agg_vecs[a]);
        agg_vecs[a] = bcast;
        agg_owned[a] = 1;
    }

    /* Pre-compute key metadata.  VLA sized to vla_keys (== max(n_keys,1)),
     * matching key_vecs above: the width guard is retired (unbounded-slots
     * cut 4), so n_keys is no longer capped at 8 here — every k < n_keys write
     * is provably within the vla_keys-sized array. */
    void* key_data[vla_keys];
    int8_t key_types[vla_keys];
    uint8_t key_attrs[vla_keys];
    for (uint32_t k = 0; k < n_keys; k++) {
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
    /* Historical: enabled only for OP_COUNT (the min_count_exclusive
     * heavy-hitter filter and the top_count_take heap).  The
     * top_count_take heap path now also accepts SUM/MIN/MAX — those
     * fire through the v2_emit per-partition compact below, which
     * reads the agg's int64 row slot directly.  The non-COUNT paths
     * (sparse_i64 range-counting, the n_keys>1 macro fast path) still
     * gate on COUNT because they DON'T have the agg value available
     * outside the row slot. */
    bool use_emit_filter = emit_filter.enabled &&
        emit_filter.agg_index < n_aggs &&
        ext->agg_ops[emit_filter.agg_index] == OP_COUNT;
    bool use_topn_filter = emit_filter.enabled &&
        emit_filter.top_count_take > 0 &&
        emit_filter.agg_index < n_aggs &&
        (ext->agg_ops[emit_filter.agg_index] == OP_COUNT ||
         ext->agg_ops[emit_filter.agg_index] == OP_SUM   ||
         ext->agg_ops[emit_filter.agg_index] == OP_MIN   ||
         ext->agg_ops[emit_filter.agg_index] == OP_MAX);

    /* ---- Scalar aggregate fast path (n_keys == 0): flat vector scan ---- */
    /* Binary aggregators (OP_PEARSON_CORR) carry cross-term row slots
     * (off_sum_y / off_sumsq_y / off_sumxy) and a y-side input that the
     * scalar da_accum_t path neither allocates nor reads — mirror the
     * DA-path guard below and fall through to the HT path which computes
     * them. Without this the Pearson slot accumulates nothing and emit
     * falls to its integer default, returning 0. */
    bool sc_has_binary = false;
    for (uint32_t a = 0; a < n_aggs; a++)
        if (agg_is_binary_agg(ext->agg_ops[a])) { sc_has_binary = true; break; }
    if (n_keys == 0 && nrows > 0 && !sc_has_binary) {
        uint8_t need_flags = DA_NEED_COUNT;
        bool has_first_last = false;
        for (uint32_t a = 0; a < n_aggs; a++) {
            uint16_t aop = ext->agg_ops[a];
            if (aop == OP_SUM || aop == OP_PROD || aop == OP_AVG || aop == OP_ALL || aop == OP_ANY || aop == OP_FIRST || aop == OP_LAST)
                need_flags |= DA_NEED_SUM;
            else if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
                { need_flags |= DA_NEED_SUM; need_flags |= DA_NEED_SUMSQ; }
            else if (aop == OP_MIN) need_flags |= DA_NEED_MIN;
            else if (aop == OP_MAX) need_flags |= DA_NEED_MAX;
            if (aop == OP_FIRST || aop == OP_LAST) has_first_last = true;
        }

        /* Keyless path serves unbounded n_aggs — VLAs here scale with the query,
         * so carve exactly (8-byte arrays first). */
        ray_t* sc_vla_hdr = NULL;
        size_t sc_vla_bytes = (size_t)vla_aggs * (sizeof(void*) + sizeof(int64_t)
                                                  + sizeof(int8_t) + sizeof(bool));
        uint8_t* sc_blk = (uint8_t*)scratch_alloc(&sc_vla_hdr, sc_vla_bytes);
        if (!sc_blk) {
            for (uint32_t a = 0; a < n_aggs; a++)
                { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
            for (uint32_t k = 0; k < n_keys; k++)
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
            return ray_error("oom", NULL);
        }
        void*    *agg_ptrs             = (void**)sc_blk;
        int64_t  *sc_int_null_sentinel = (int64_t*)(agg_ptrs + vla_aggs);
        int8_t   *agg_types            = (int8_t*)(sc_int_null_sentinel + vla_aggs);
        /* Per-element flag, not a bitmask: this path's n_aggs is VLA-sized
         * with no fixed cap, so a fixed-width bitmask would silently alias
         * once n_aggs exceeded its bit width. */
        bool     *sc_int_null_has      = (bool*)(agg_types + vla_aggs);
        bool sc_any_nullable = false;
        for (uint32_t a = 0; a < n_aggs; a++) {
            if (agg_prod[a].enabled) {
                /* Fused product: F64 accumulate, no source vec. */
                agg_ptrs[a]  = NULL;
                agg_types[a] = RAY_F64;
                sc_int_null_sentinel[a] = 0;
                sc_int_null_has[a] = false;
                continue;
            }
            if (agg_vecs[a]) {
                agg_ptrs[a]  = ray_data(agg_vecs[a]);
                agg_types[a] = agg_vecs[a]->type;
                sc_int_null_sentinel[a] = agg_int_null_sentinel_for(agg_vecs[a]->type);
                /* Only flag int-null for storage types whose sentinel is
                 * meaningful.  BOOL/U8/SYM use 0 as their default
                 * "sentinel" which collides with legitimate values
                 * (FALSE / zero byte / SYM id 0); gating those would silently
                 * drop real rows from SUM/MIN/MAX.  F64 has its own NaN path. */
                int8_t t = agg_vecs[a]->type;
                bool is_sentinel_typed = (t == RAY_I16 || t == RAY_I32 || t == RAY_I64 ||
                                          t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP);
                sc_int_null_has[a] = is_sentinel_typed && (agg_vecs[a]->attrs & RAY_ATTR_HAS_NULLS);
                if ((agg_vecs[a]->attrs & RAY_ATTR_HAS_NULLS) &&
                    (agg_vecs[a]->type == RAY_F64 || is_sentinel_typed))
                    sc_any_nullable = true;
            } else {
                agg_ptrs[a]  = NULL;
                agg_types[a] = 0;
                sc_int_null_sentinel[a] = 0;
                sc_int_null_has[a] = false;
            }
        }

        if (!match_idx && !rowsel && !sc_any_nullable && n_aggs > 1) {
            ray_t* base_col = NULL;
            const void* base_ptr = NULL;
            int8_t base_type = 0;
            int64_t base_len = 0;
            bool one_base_input = true;
            for (uint32_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop != OP_SUM && aop != OP_AVG) { one_base_input = false; break; }
                if (agg_prod[a].enabled || agg_strlen[a]) {
                    one_base_input = false;
                    break;
                }

                const void* slot_ptr = NULL;
                int8_t slot_type = 0;
                int64_t slot_len = nrows;
                if (agg_vecs[a] && agg_ptrs[a]) {
                    if (!agg_type_admitted(OP_SUM, agg_vecs[a]->type)) {
                        one_base_input = false;
                        break;
                    }
                    slot_ptr = agg_ptrs[a];
                    slot_type = agg_vecs[a]->type;
                    slot_len = agg_vecs[a]->len;
                    if (!base_col) base_col = agg_vecs[a];
                } else if (agg_linear[a].enabled &&
                           agg_linear[a].n_terms == 1 &&
                           agg_linear[a].coeff_i64[0] == 1 &&
                           agg_linear[a].bias_i64 == 0 &&
                           agg_linear[a].term_ptrs[0]) {
                    if (!agg_type_admitted(OP_SUM, agg_linear[a].term_types[0])) {
                        one_base_input = false;
                        break;
                    }
                    slot_ptr = agg_linear[a].term_ptrs[0];
                    slot_type = agg_linear[a].term_types[0];
                } else {
                    one_base_input = false;
                    break;
                }

                if (!base_ptr) {
                    base_ptr = slot_ptr;
                    base_type = slot_type;
                    base_len = slot_len;
                } else if (slot_ptr != base_ptr || slot_type != base_type ||
                           slot_len != base_len) {
                    one_base_input = false;
                    break;
                }
            }

            if (one_base_input && base_col) {
                ray_t* base_sum_obj = ray_sum_fn(base_col);
                if (!base_sum_obj || RAY_IS_ERR(base_sum_obj)) {
                    scratch_free(sc_vla_hdr);
                    for (uint32_t a = 0; a < n_aggs; a++)
                        { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
                    for (uint32_t k = 0; k < n_keys; k++)
                        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                    if (match_idx_block) ray_release(match_idx_block);
                    scratch_free(vla_hdr);
                    return base_sum_obj ? base_sum_obj : ray_error("oom", NULL);
                }
                if (ray_is_lazy(base_sum_obj))
                    base_sum_obj = ray_lazy_materialize(base_sum_obj);
                if (!base_sum_obj || RAY_IS_ERR(base_sum_obj)) {
                    scratch_free(sc_vla_hdr);
                    for (uint32_t a = 0; a < n_aggs; a++)
                        { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
                    for (uint32_t k = 0; k < n_keys; k++)
                        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                    if (match_idx_block) ray_release(match_idx_block);
                    scratch_free(vla_hdr);
                    return base_sum_obj ? base_sum_obj : ray_error("oom", NULL);
                }

                bool base_is_f64 = false;
                double base_sum_f64 = 0.0;
                int64_t base_sum_i64 = 0;
                if (sum_atom_value(base_sum_obj, &base_is_f64,
                                   &base_sum_f64, &base_sum_i64)) {
                    ray_t *sum_hdr = NULL, *cnt_hdr = NULL;
                    double* sums_f64 = NULL;
                    int64_t* sums_i64 = NULL;
                    if (base_is_f64)
                        sums_f64 = (double*)scratch_alloc(&sum_hdr,
                            (size_t)n_aggs * sizeof(double));
                    else
                        sums_i64 = (int64_t*)scratch_alloc(&sum_hdr,
                            (size_t)n_aggs * sizeof(int64_t));
                    int64_t* counts = (int64_t*)scratch_alloc(&cnt_hdr,
                        sizeof(int64_t));
                    if ((base_is_f64 ? (sums_f64 != NULL) : (sums_i64 != NULL)) &&
                        counts) {
                        if (base_is_f64) {
                            for (uint32_t a = 0; a < n_aggs; a++)
                                sums_f64[a] = base_sum_f64;
                        } else {
                            for (uint32_t a = 0; a < n_aggs; a++)
                                sums_i64[a] = base_sum_i64;
                        }
                        counts[0] = nrows;

                        ray_t* result = ray_table_new(n_aggs);
                        if (!result || RAY_IS_ERR(result)) {
                            scratch_free(sum_hdr);
                            scratch_free(cnt_hdr);
                            ray_release(base_sum_obj);
                            scratch_free(sc_vla_hdr);
                            for (uint32_t a = 0; a < n_aggs; a++)
                                { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
                            for (uint32_t k = 0; k < n_keys; k++)
                                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                            if (match_idx_block) ray_release(match_idx_block);
                            scratch_free(vla_hdr);
                            return result ? result : ray_error("oom", NULL);
                        }

                        emit_agg_columns(&result, g, ext, agg_vecs, 1, n_aggs,
                                         sums_f64, sums_i64,
                                         NULL, NULL, NULL, NULL,
                                         counts, agg_affine, agg_prod,
                                         NULL, NULL, NULL, NULL, NULL);
                        scratch_free(sum_hdr);
                        scratch_free(cnt_hdr);
                        ray_release(base_sum_obj);
                        scratch_free(sc_vla_hdr);
                        for (uint32_t a = 0; a < n_aggs; a++)
                            { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
                        for (uint32_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) ray_release(match_idx_block);
                        scratch_free(vla_hdr);
                        return result;
                    }
                    scratch_free(sum_hdr);
                    scratch_free(cnt_hdr);
                }
                ray_release(base_sum_obj);
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
        if (!sc_acc) { scratch_free(sc_vla_hdr); goto da_path; }

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
                for (uint32_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) sc_acc[w].min_val[a].f = DBL_MAX;
                    else sc_acc[w].min_val[a].i = INT64_MAX;
                }
            }
            if (need_flags & DA_NEED_MAX) {
                sc_acc[w].max_val = (da_val_t*)scratch_alloc(&sc_acc[w]._h_max,
                    n_aggs * sizeof(da_val_t));
                if (!sc_acc[w].max_val) { alloc_ok = false; break; }
                for (uint32_t a = 0; a < n_aggs; a++) {
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
            if (sc_any_nullable) {
                sc_acc[w].nn_count = (int64_t*)scratch_calloc(
                    &sc_acc[w]._h_nn_count, n_aggs * sizeof(int64_t));
                if (!sc_acc[w].nn_count) { alloc_ok = false; break; }
            }
        }
        if (!alloc_ok) {
            for (uint32_t w = 0; w < sc_n; w++) da_accum_free(&sc_acc[w]);
            scratch_free(sc_hdr);
            scratch_free(sc_vla_hdr);
            goto da_path;
        }

        scalar_ctx_t sc_ctx = {
            .agg_ptrs   = agg_ptrs,
            .agg_types  = agg_types,
            .agg_cols   = agg_vecs,
            .agg_strlen = agg_strlen,
            .agg_ops    = ext->agg_ops,
            .agg_linear = agg_linear,
            .agg_prod   = agg_prod,
            .n_aggs     = n_aggs,
            .need_flags = need_flags,
            .match_idx  = match_idx,
            .rowsel     = rowsel,
            .accums     = sc_acc,
            .n_accums   = sc_n,
            .agg_int_null_has = sc_int_null_has,
            .agg_int_null_sentinel = sc_int_null_sentinel,
        };

        /* Pick specialized tight loop when possible, else generic.
         * The specialized scalar_sum_*_fn variants don't honour
         * match_idx — they read data[r] directly — so they're only
         * safe when no selection is in flight.  They also read the
         * slot raw, so they require null-free input: NULL_I{16,32,64}
         * sentinels in null slots would poison the sum.  Fall back to
         * the generic masked path when the source vector advertises
         * nulls.  (try_linear_sumavg_input_i64 already refuses to build
         * a linear plan when any term column has nulls, so
         * agg_linear[0].enabled implies null-free.) */
        typedef void (*scalar_fn_t)(void*, uint32_t, int64_t, int64_t);
        scalar_fn_t sc_fn = scalar_accum_fn;
        bool agg0_has_nulls = n_aggs > 0 &&
            (sc_int_null_has[0] ||
             (agg_vecs[0] && agg_vecs[0]->type == RAY_F64 &&
              (agg_vecs[0]->attrs & RAY_ATTR_HAS_NULLS)));
        if (n_aggs == 1 && !match_idx && !rowsel && agg_ptrs[0] != NULL && !agg0_has_nulls) {
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
                for (uint32_t a = 0; a < n_aggs; a++) {
                    uint16_t merge_op = ext->agg_ops[a];
                    /* nn_count is per-agg; count is per worker.  Fall back
                     * to count when nn_count is absent (no nullable aggs). */
                    int64_t mnn = m->nn_count ? m->nn_count[a] : m->count[0];
                    int64_t wnn = wa->nn_count ? wa->nn_count[a] : wa->count[0];
                    if (merge_op == OP_FIRST) {
                        if (mnn == 0 && wnn > 0)
                            m->sum[a] = wa->sum[a];
                    } else if (merge_op == OP_LAST) {
                        if (wnn > 0)
                            m->sum[a] = wa->sum[a];
                    } else if (merge_op == OP_PROD) {
                        if (wnn > 0) {
                            if (mnn == 0)
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
                for (uint32_t a = 0; a < n_aggs; a++)
                    m->sumsq_f64[a] += wa->sumsq_f64[a];
            }
            if (need_flags & DA_NEED_MIN) {
                for (uint32_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) {
                        if (wa->min_val[a].f < m->min_val[a].f)
                            m->min_val[a].f = wa->min_val[a].f;
                    } else if (agg_types[a] == RAY_SYM) {
                        if (wa->min_val[a].i != INT64_MAX &&
                            (m->min_val[a].i == INT64_MAX ||
                             sym_lex_lt(ray_sym_vec_domain(agg_vecs[a]),
                                        wa->min_val[a].i, m->min_val[a].i)))
                            m->min_val[a].i = wa->min_val[a].i;
                    } else {
                        if (wa->min_val[a].i < m->min_val[a].i)
                            m->min_val[a].i = wa->min_val[a].i;
                    }
                }
            }
            if (need_flags & DA_NEED_MAX) {
                for (uint32_t a = 0; a < n_aggs; a++) {
                    if (agg_types[a] == RAY_F64) {
                        if (wa->max_val[a].f > m->max_val[a].f)
                            m->max_val[a].f = wa->max_val[a].f;
                    } else if (agg_types[a] == RAY_SYM) {
                        if (wa->max_val[a].i != INT64_MIN &&
                            (m->max_val[a].i == INT64_MIN ||
                             sym_lex_gt(ray_sym_vec_domain(agg_vecs[a]),
                                        wa->max_val[a].i, m->max_val[a].i)))
                            m->max_val[a].i = wa->max_val[a].i;
                    } else {
                        if (wa->max_val[a].i > m->max_val[a].i)
                            m->max_val[a].i = wa->max_val[a].i;
                    }
                }
            }
            if (m->nn_count && wa->nn_count) {
                for (uint32_t a = 0; a < n_aggs; a++)
                    m->nn_count[a] += wa->nn_count[a];
            }
            m->count[0] += wa->count[0];
        }
        for (uint32_t w = 1; w < sc_n; w++) da_accum_free(&sc_acc[w]);

        /* Emit 1-row result with no key columns */
        ray_t* result = ray_table_new(n_aggs);
        if (!result || RAY_IS_ERR(result)) {
            da_accum_free(&sc_acc[0]); scratch_free(sc_hdr);
            scratch_free(sc_vla_hdr);
            for (uint32_t a = 0; a < n_aggs; a++)
                { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
            for (uint32_t k = 0; k < n_keys; k++)
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
            return result ? result : ray_error("oom", NULL);
        }

        emit_agg_columns(&result, g, ext, agg_vecs, 1, n_aggs,
                         (double*)m->sum, (int64_t*)m->sum,
                         (double*)m->min_val, (double*)m->max_val,
                         (int64_t*)m->min_val, (int64_t*)m->max_val,
                         m->count, agg_affine, agg_prod, m->sumsq_f64, m->nn_count,
                         NULL, NULL, NULL);

        /* Wide-element (STR/GUID) min/max/first/last overflow emit_agg_columns'
         * fixed-width slots (it truncated them to 1 byte above).  Recompute
         * those 1-row columns by materialising the winning row — mirroring
         * exec_reduction — and override them in the result table. */
        for (uint32_t a = 0; a < n_aggs; a++) {
            uint16_t aop = ext->agg_ops[a];
            if (!(agg_vecs[a] && agg_is_wide_type(agg_vecs[a]->type) &&
                  (aop == OP_MIN || aop == OP_MAX ||
                   aop == OP_FIRST || aop == OP_LAST)))
                continue;
            ray_t* wsel_blk = NULL; const int64_t* wsel = NULL; int64_t wscan = nrows;
            if (g->selection) {
                ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
                if (sm && sm->nrows == nrows) {
                    wsel_blk = ray_rowsel_to_indices(g->selection);
                    wsel = wsel_blk ? (const int64_t*)ray_data(wsel_blk) : NULL;
                    wscan = sm->total_pass;
                }
            }
            bool hn = (agg_vecs[a]->attrs & RAY_ATTR_HAS_NULLS) != 0;
            ray_t* atom = agg_wide_reduce(agg_vecs[a], aop, wsel, wscan, hn);
            if (wsel_blk) ray_release(wsel_blk);
            ray_t* col = col_vec_new(agg_vecs[a], 1);
            if (col && !RAY_IS_ERR(col)) {
                col->len = 1;
                if (RAY_ATOM_IS_NULL(atom)) {
                    ray_vec_set_null(col, 0, true);
                } else if (agg_vecs[a]->type == RAY_STR) {
                    ray_t* nv = ray_str_vec_set(col, 0, ray_str_ptr(atom), ray_str_len(atom));
                    if (nv && !RAY_IS_ERR(nv)) col = nv;
                } else {
                    store_typed_elem(col, 0, atom);
                }
                ray_table_set_col_idx(result, a, col);
                ray_release(col);
            }
            ray_release(atom);
        }

        /* Whole-table holistic aggregates have no n_keys==0 accumulator, so emit_agg_columns
         * left its column at the integer default (0).  Recompute it over the
         * whole (optionally selected) column as a single group via the same
         * per-group kernel the by-group path uses.  (top/bot are not handled
         * here: the no-by planner does not carry the K argument, so they keep
         * their scalar-builtin form.) */
        {
            bool any_rank = false;
            for (uint32_t a = 0; a < n_aggs; a++)
                if (ext->agg_ops[a] == OP_MEDIAN ||
                    ext->agg_ops[a] == OP_QUANTILE ||
                    ext->agg_ops[a] == OP_MODE) { any_rank = true; break; }
            if (any_rank) {
                ray_t* hsel_blk = NULL; const int64_t* hsel = NULL; int64_t hscan = nrows;
                if (g->selection) {
                    ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
                    if (sm && sm->nrows == nrows) {
                        hsel_blk = ray_rowsel_to_indices(g->selection);
                        hsel = hsel_blk ? (const int64_t*)ray_data(hsel_blk) : NULL;
                        hscan = sm->total_pass;
                    }
                }
                ray_t* ix_hdr = NULL;
                int64_t* idxb = (int64_t*)scratch_alloc(&ix_hdr,
                    (size_t)(hscan > 0 ? hscan : 1) * sizeof(int64_t));
                if (idxb) {
                    /* selection branch hoisted out of the fill loop */
                    if (hsel) { for (int64_t i = 0; i < hscan; i++) idxb[i] = hsel[i]; }
                    else      { for (int64_t i = 0; i < hscan; i++) idxb[i] = i; }
                    int64_t offs[1] = {0};
                    int64_t cnts[1] = {hscan};
                    for (uint32_t a = 0; a < n_aggs; a++) {
                        if (!agg_vecs[a]) continue;
                        uint16_t aop = ext->agg_ops[a];
                        ray_t* hv = NULL;
                        if (aop == OP_MEDIAN) {
                            hv = ray_median_per_group_buf(agg_vecs[a], idxb, offs, cnts, 1);
                        } else if (aop == OP_QUANTILE) {
                            double q = ext->agg_k
                                ? decode_agg_f64_param(ext->agg_k[a]) : 0.5;
                            hv = ray_quantile_per_group_buf(agg_vecs[a], idxb, offs, cnts, 1, q);
                        } else if (aop == OP_MODE) {
                            hv = ray_mode_per_group_buf(agg_vecs[a], idxb, offs, cnts, 1);
                        } else {
                            continue;
                        }
                        if (hv && !RAY_IS_ERR(hv)) ray_table_set_col_idx(result, a, hv);
                        if (hv) ray_release(hv);
                    }
                    scratch_free(ix_hdr);
                }
                if (hsel_blk) ray_release(hsel_blk);
            }
        }

        da_accum_free(&sc_acc[0]); scratch_free(sc_hdr);
        scratch_free(sc_vla_hdr);
        for (uint32_t a = 0; a < n_aggs; a++)
            { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
        for (uint32_t k = 0; k < n_keys; k++)
            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
        if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
        return result;
    }

da_path:;
    /* ---- Direct-array fast path for low-cardinality integer keys ---- */
    /* Supports multi-key via composite index: product of ranges <= MAX */
    #define DA_MAX_COMPOSITE_SLOTS 262144  /* 256K slots max */
    #define DA_MEM_BUDGET      (256ULL << 20)  /* 256 MB total across all workers */
    #define DA_PER_WORKER_MAX  (6ULL << 20)    /* 6 MB per-worker max */
    {
        /* n_aggs <= 64 is not a slot cap — it is the width of the da_ctx_t
         * agg_f64_mask/agg_int_null_mask bitmasks (uint64_t, one bit per
         * agg).  Wider agg lists route to the hash (HT) path instead, which
         * carries per-agg arrays rather than a fixed-width bitmask. */
        bool da_eligible = (nrows > 0 && n_keys > 0 && n_keys <= 8 &&
                             n_aggs <= 64);
        if (da_eligible && rowsel && n_keys == 1) {
            ray_rowsel_t* sm = ray_rowsel_meta(rowsel);
            if (sm && sm->total_pass * 4 < nrows)
                da_eligible = false;
        }
        /* Binary aggregators (OP_PEARSON_CORR) are not wired into the
         * dense-array accumulator's per-worker da_accum_t struct — force
         * the HT path which has the row-layout offsets allocated.
         * Holistic aggregators have
         * no per-row accumulator at all — they need the post-radix
         * row_gid+grp_cnt pass which only the HT path provides. */
        for (uint32_t a = 0; a < n_aggs && da_eligible; a++) {
            uint16_t aop = ext->agg_ops[a];
            if (agg_is_binary_agg(aop)) da_eligible = false;
            if (aop == OP_MEDIAN)       da_eligible = false;
            if (aop == OP_QUANTILE)     da_eligible = false;
            if (aop == OP_MODE)         da_eligible = false;
            if (aop == OP_TOP_N)        da_eligible = false;
            if (aop == OP_BOT_N)        da_eligible = false;
            /* Wide-element (STR/GUID) min/max/first/last need the holistic
             * post-fill; the DA emit (emit_agg_columns) would truncate them. */
            if (agg_vecs[a] && agg_is_wide_type(agg_vecs[a]->type) &&
                (aop == OP_MIN || aop == OP_MAX ||
                 aop == OP_FIRST || aop == OP_LAST))
                da_eligible = false;
        }
        for (uint32_t k = 0; k < n_keys && da_eligible; k++) {
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

        /* Upfront known-cardinality reject: a dict-substituted STR key carries
         * a known distinct-count (dense codes 0..n_distinct-1 over the full
         * column), so over the filtered survivors n_distinct is an upper bound
         * on its slot span.  If the product of such KNOWN bounds already
         * exceeds the DA budget, the composite is infeasible — reject now and
         * skip the min/max prescan (a full gather-scan of the survivors)
         * entirely.  Conservative in the CORRECTNESS direction: keys without a
         * known count contribute factor 1, and rejecting only ever SKIPS the
         * DA path (never wrongly ENABLES it), so results never change; a
         * selective filter that compresses the surviving code range could make
         * this over-reject, but that is a perf-only effect. */
        if (da_eligible && __VM && __VM->grp_card_n > 0) {
            uint64_t known_ub = 1;
            for (uint32_t k = 0; k < n_keys; k++) {
                if (key_scan_sym[k] < 0) continue;
                int64_t card = ray_grp_card_lookup(key_scan_sym[k]);
                if (card <= 1) continue;
                if ((uint64_t)card > (uint64_t)DA_MAX_COMPOSITE_SLOTS / known_ub) {
                    da_eligible = false;
                    break;
                }
                known_ub *= (uint64_t)card;
                if (known_ub > DA_MAX_COMPOSITE_SLOTS) { da_eligible = false; break; }
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
            /* Shared across keys: once any key proves the DA slot count
             * infeasible the scan aborts instead of reading the rest. */
            _Atomic(int) mm_abort = 0;
            for (uint32_t k = 0; k < n_keys && da_fits; k++) {
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
                    .span_budget    = DA_MAX_COMPOSITE_SLOTS,
                    .abort_flag     = &mm_abort,
                };
                if (mm_n > 1) {
                    ray_pool_dispatch(mm_pool, minmax_scan_fn, &mm_ctx, n_scan);
                } else {
                    minmax_scan_fn(&mm_ctx, 0, 0, n_scan);
                }
                if (atomic_load_explicit(&mm_abort, memory_order_relaxed)) {
                    da_fits = false;
                    break;
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
            for (uint32_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop == OP_SUM || aop == OP_PROD || aop == OP_AVG || aop == OP_ALL || aop == OP_ANY || aop == OP_FIRST || aop == OP_LAST) need_flags |= DA_NEED_SUM;
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
            for (uint32_t a = 0; a < n_aggs; a++) {
                uint16_t aop = ext->agg_ops[a];
                if (aop == OP_SUM || aop == OP_PROD || aop == OP_AVG || aop == OP_ALL || aop == OP_ANY || aop == OP_FIRST || aop == OP_LAST) need_flags |= DA_NEED_SUM;
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
            for (uint32_t k = 0; k < n_keys; k++) {
                int64_t s = 1;
                for (uint32_t j = k + 1; j < n_keys; j++) {
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

            /* VLA sized to vla_aggs (== n_aggs): agg_ptrs/agg_types/
             * da_int_null_sentinel are per-element arrays, not bitmasks, so
             * they scale with n_aggs directly.  This whole block is under
             * da_fits, which requires da_eligible, which caps n_aggs <= 64 —
             * the width of the agg_f64_mask/da_int_null_mask bitmasks built
             * just below, not a VLA bound. */
            void* agg_ptrs[vla_aggs];
            int8_t agg_types[vla_aggs];
            int64_t da_int_null_sentinel[vla_aggs];
            uint64_t agg_f64_mask = 0;
            uint64_t da_int_null_mask = 0;
            /* Track whether any agg column can produce a null so we can
             * allocate per-(group, agg) non-null counts only when required.
             * F64 with HAS_NULLS uses NaN-skip; sentinel-typed integers
             * with HAS_NULLS use sentinel-skip. */
            bool da_any_nullable = false;
            for (uint32_t a = 0; a < n_aggs; a++) {
                if (agg_prod[a].enabled) {
                    /* Fused product: F64 accumulate, no source vec. */
                    agg_ptrs[a]  = NULL;
                    agg_types[a] = RAY_F64;
                    agg_f64_mask |= ((uint64_t)1 << a);
                    da_int_null_sentinel[a] = 0;
                    continue;
                }
                if (agg_vecs[a]) {
                    agg_ptrs[a]  = ray_data(agg_vecs[a]);
                    agg_types[a] = agg_vecs[a]->type;
                    if (agg_vecs[a]->type == RAY_F64)
                        agg_f64_mask |= ((uint64_t)1 << a);
                    da_int_null_sentinel[a] = agg_int_null_sentinel_for(agg_vecs[a]->type);
                    /* Only set the int-null mask bit for storage types whose
                     * sentinel is meaningful.  BOOL/U8/SYM use 0 as their default
                     * "sentinel" which collides with legitimate values
                     * (FALSE / zero byte / SYM id 0); gating those would silently
                     * drop real rows from SUM/MIN/MAX.  F64 has its own NaN path. */
                    int8_t t = agg_vecs[a]->type;
                    bool is_sentinel_typed = (t == RAY_I16 || t == RAY_I32 || t == RAY_I64 ||
                                              t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP);
                    if (is_sentinel_typed && (agg_vecs[a]->attrs & RAY_ATTR_HAS_NULLS))
                        da_int_null_mask |= ((uint64_t)1 << a);
                    if ((agg_vecs[a]->attrs & RAY_ATTR_HAS_NULLS) &&
                        (agg_vecs[a]->type == RAY_F64 || is_sentinel_typed))
                        da_any_nullable = true;
                } else {
                    agg_ptrs[a]  = NULL;
                    agg_types[a] = 0;
                    da_int_null_sentinel[a] = 0;
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
            /* Nullable aggs add a per-(group, agg) non-null count array.
             * ~8 bytes per (group, agg). */
            if (da_any_nullable) arrays_per_agg += 1;
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
                if (da_any_nullable) {
                    accums[w].nn_count = (int64_t*)scratch_calloc(
                        &accums[w]._h_nn_count, total * sizeof(int64_t));
                    if (!accums[w].nn_count) { alloc_ok = false; break; }
                }
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
            for (uint32_t k = 0; k < n_keys; k++)
                da_key_esz[k] = ray_sym_elem_size(key_types[k], key_attrs[k]);

            /* strlen-on-SYM aggs (e.g. avg(strlen URL)) read the sym
             * string per row.  ray_sym_str takes a lock per call — 10M
             * rows = 10M locked dict lookups.  Borrow the sym snapshot
             * once and let da_accum_row index it lock-free. */
            ray_t** da_sym_strings = NULL;
            uint32_t da_sym_count = 0;
            for (uint32_t a = 0; a < n_aggs; a++) {
                if (agg_strlen[a] && agg_vecs[a] &&
                    agg_vecs[a]->type == RAY_SYM) {
                    ray_sym_strings_borrow(&da_sym_strings, &da_sym_count);
                    break;
                }
            }
            da_ctx_t da_ctx = {
                .accums      = accums,
                .sym_strings = da_sym_strings,
                .sym_count   = da_sym_count,
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
                .agg_prod    = agg_prod,
                .agg_ops     = ext->agg_ops,
                .n_aggs      = n_aggs,
                .need_flags  = need_flags,
                .agg_f64_mask = agg_f64_mask,
                .agg_int_null_mask = da_int_null_mask,
                .agg_int_null_sentinel = da_int_null_sentinel,
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
            for (uint32_t a = 0; a < n_aggs; a++) {
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
                            for (uint32_t a = 0; a < n_aggs; a++) {
                                size_t idx = base + a;
                                uint16_t aop = ext->agg_ops[a];
                                if (aop == OP_SUM || aop == OP_AVG || aop == OP_ALL || aop == OP_ANY || aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP) {
                                    if (agg_types[a] == RAY_F64) merged->sum[idx].f += wa->sum[idx].f;
                                    else merged->sum[idx].i += wa->sum[idx].i;
                                } else if (aop == OP_PROD) {
                                    /* Use per-(group, agg) non-null counts when
                                     * available so an all-null worker doesn't
                                     * fold a stale seed into the merged product. */
                                    int64_t mnn = merged->nn_count ? merged->nn_count[idx] : merged->count[s];
                                    int64_t wnn = wa->nn_count ? wa->nn_count[idx] : wa->count[s];
                                    if (wnn > 0) {
                                        if (mnn == 0)
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
                            } else if (agg_types[a] == RAY_SYM) {
                                /* Lexicographic merge — mirrors da_merge_fn.
                                 * INT64_MAX is the "no value seen" seed. */
                                if (wa->min_val[i].i != INT64_MAX &&
                                    (merged->min_val[i].i == INT64_MAX ||
                                     sym_lex_lt(ray_sym_vec_domain(agg_vecs[a]),
                                                wa->min_val[i].i, merged->min_val[i].i)))
                                    merged->min_val[i].i = wa->min_val[i].i;
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
                            } else if (agg_types[a] == RAY_SYM) {
                                /* Lexicographic merge — mirrors da_merge_fn.
                                 * INT64_MIN is the "no value seen" seed. */
                                if (wa->max_val[i].i != INT64_MIN &&
                                    (merged->max_val[i].i == INT64_MIN ||
                                     sym_lex_gt(ray_sym_vec_domain(agg_vecs[a]),
                                                wa->max_val[i].i, merged->max_val[i].i)))
                                    merged->max_val[i].i = wa->max_val[i].i;
                            } else {
                                if (wa->max_val[i].i > merged->max_val[i].i)
                                    merged->max_val[i].i = wa->max_val[i].i;
                            }
                        }
                    }
                    if (merged->nn_count && wa->nn_count) {
                        for (size_t i = 0; i < total; i++)
                            merged->nn_count[i] += wa->nn_count[i];
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
                    .agg_cols      = agg_vecs,
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
                            for (uint32_t a = 0; a < n_aggs; a++) {
                                size_t idx = base + a;
                                uint16_t aop = ext->agg_ops[a];
                                int64_t mnn = merged->nn_count ? merged->nn_count[idx] : merged->count[s];
                                int64_t wnn = wa->nn_count ? wa->nn_count[idx] : wa->count[s];
                                if (aop == OP_FIRST) {
                                    if (mnn == 0 && wnn > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                } else if (aop == OP_LAST) {
                                    if (wnn > 0)
                                        merged->sum[idx] = wa->sum[idx];
                                } else if (aop == OP_PROD) {
                                    if (wnn > 0) {
                                        if (mnn == 0)
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
                            } else if (agg_types[a] == RAY_SYM) {
                                /* Lexicographic merge — mirrors da_merge_fn.
                                 * INT64_MAX is the "no value seen" seed. */
                                if (wa->min_val[i].i != INT64_MAX &&
                                    (merged->min_val[i].i == INT64_MAX ||
                                     sym_lex_lt(ray_sym_vec_domain(agg_vecs[a]),
                                                wa->min_val[i].i, merged->min_val[i].i)))
                                    merged->min_val[i].i = wa->min_val[i].i;
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
                            } else if (agg_types[a] == RAY_SYM) {
                                /* Lexicographic merge — mirrors da_merge_fn.
                                 * INT64_MIN is the "no value seen" seed. */
                                if (wa->max_val[i].i != INT64_MIN &&
                                    (merged->max_val[i].i == INT64_MIN ||
                                     sym_lex_gt(ray_sym_vec_domain(agg_vecs[a]),
                                                wa->max_val[i].i, merged->max_val[i].i)))
                                    merged->max_val[i].i = wa->max_val[i].i;
                            } else {
                                if (wa->max_val[i].i > merged->max_val[i].i)
                                    merged->max_val[i].i = wa->max_val[i].i;
                            }
                        }
                    }
                    if (merged->nn_count && wa->nn_count) {
                        for (size_t i = 0; i < total; i++)
                            merged->nn_count[i] += wa->nn_count[i];
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
            int64_t*  da_nn_count = merged->nn_count; /* may be NULL when no agg can be null */

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
                for (uint32_t a = 0; a < n_aggs; a++)
                    { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
                for (uint32_t k = 0; k < n_keys; k++)
                    if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
                return result ? result : ray_error("oom", NULL);
            }

            /* Key columns — decompose composite slot back to per-key values */
            for (uint32_t k = 0; k < n_keys; k++) {
                ray_t* src_col = key_vecs[k];
                if (!src_col) continue;
                ray_t* key_col = col_vec_new(src_col, (int64_t)grp_count);
                out_col_adopt_str_pool(key_col, src_col);
                if (!key_col || RAY_IS_ERR(key_col)) continue;
                /* group keys are RAW cell ids reconstructed from the DA
                 * slot — the output resolves over the source column's
                 * dictionary (no-op while runtime-domain). */
                if (key_col->type == RAY_SYM)
                    ray_sym_vec_adopt_domain(key_col, sym_domain_rep(src_col));
                key_col->len = (int64_t)grp_count;
                uint32_t gi = 0;
                for (uint32_t s = 0; s < n_slots; s++) {
                    if (da_count[s] < da_keep_min) continue;
                    int64_t offset = ((int64_t)s / da_key_stride[k]) % da_key_range[k];
                    int64_t key_val = da_key_min[k] + offset;
                    write_col_i64(ray_data(key_col), gi, key_val, src_col->type, key_col->attrs);
                    gi++;
                }
                ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]);
                int64_t name_id = key_ext ? key_ext->sym : (int64_t)k;
                result = ray_table_add_col(result, name_id, key_col);
                ray_release(key_col);
            }

            /* Agg columns — compact sparse DA arrays into dense, then emit */
            size_t dense_total = (size_t)grp_count * n_aggs;
            ray_t *_h_dsum = NULL, *_h_dmin = NULL, *_h_dmax = NULL;
            ray_t *_h_dsq = NULL, *_h_dcnt = NULL, *_h_dnn = NULL;
            da_val_t* dense_sum     = da_sum     ? (da_val_t*)scratch_alloc(&_h_dsum, dense_total * sizeof(da_val_t)) : NULL;
            da_val_t* dense_min_val = da_min_val ? (da_val_t*)scratch_alloc(&_h_dmin, dense_total * sizeof(da_val_t)) : NULL;
            da_val_t* dense_max_val = da_max_val ? (da_val_t*)scratch_alloc(&_h_dmax, dense_total * sizeof(da_val_t)) : NULL;
            double*   dense_sumsq   = da_sumsq   ? (double*)scratch_alloc(&_h_dsq, dense_total * sizeof(double)) : NULL;
            int64_t*  dense_counts  = grp_count
                ? (int64_t*)scratch_alloc(&_h_dcnt, grp_count * sizeof(int64_t))
                : NULL;
            int64_t*  dense_nn_counts = (da_nn_count && grp_count)
                ? (int64_t*)scratch_alloc(&_h_dnn, dense_total * sizeof(int64_t))
                : NULL;

            uint32_t gi = 0;
            for (uint32_t s = 0; s < n_slots; s++) {
                if (da_count[s] < da_keep_min) continue;
                dense_counts[gi] = da_count[s];
                for (uint32_t a = 0; a < n_aggs; a++) {
                    size_t si = (size_t)s * n_aggs + a;
                    size_t di = (size_t)gi * n_aggs + a;
                    if (dense_sum)     dense_sum[di]     = da_sum[si];
                    if (dense_min_val) dense_min_val[di] = da_min_val[si];
                    if (dense_max_val) dense_max_val[di] = da_max_val[si];
                    if (dense_sumsq)   dense_sumsq[di]   = da_sumsq[si];
                    if (dense_nn_counts) dense_nn_counts[di] = da_nn_count[si];
                }
                gi++;
            }

            emit_agg_columns(&result, g, ext, agg_vecs, grp_count, n_aggs,
                             (double*)dense_sum, (int64_t*)dense_sum,
                             (double*)dense_min_val, (double*)dense_max_val,
                             (int64_t*)dense_min_val, (int64_t*)dense_max_val,
                             dense_counts, agg_affine, agg_prod, dense_sumsq,
                             dense_nn_counts, NULL, NULL, NULL);

            scratch_free(_h_dsum); scratch_free(_h_dmin);
            scratch_free(_h_dmax);
            scratch_free(_h_dsq); scratch_free(_h_dcnt);
            scratch_free(_h_dnn);

            da_accum_free(&accums[0]); scratch_free(accums_hdr);
            for (uint32_t a = 0; a < n_aggs; a++)
                { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
            for (uint32_t k = 0; k < n_keys; k++)
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
            return result;
        }
    }

    {
        /* n_aggs <= 64: same mask-width bound as da_eligible above — this
         * path's independent agg_f64_mask is also a uint64_t.  Beyond 64
         * aggs, fall through to the hash path. */
        bool sp_eligible = (nrows > 0 && n_keys == 1 && key_data[0] != NULL &&
                             n_aggs <= 64);
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
        for (uint32_t a = 0; a < n_aggs && sp_eligible; a++) {
            uint16_t op = ext->agg_ops[a];
            if (op == OP_COUNT) continue;
            if (op != OP_SUM && op != OP_AVG)
                sp_eligible = false;
            else {
                /* The single-key sparse aggregation path reads agg slots
                 * raw via read_col_i64 / direct double load; nullable
                 * input columns would poison the sum with NULL_I* or
                 * NULL_F64 sentinels.  Fall back to slower paths that
                 * mask nulls properly.  (The multi-key radix HT at
                 * accum_from_entry inherits the same nullable-agg gap.) */
                if (agg_vecs[a] && (agg_vecs[a]->attrs & RAY_ATTR_HAS_NULLS))
                    sp_eligible = false;
                else
                    sp_need_sum = true;
            }
        }

        if (sp_eligible) {
            /* The radix entry layout reads agg_vecs directly — convert any
             * fused-product slots into ordinary materialized inputs.  OOM
             * here simply skips the sp path; ht_path materializes again. */
            if (!group_materialize_prod_slots(agg_prod, agg_vecs, agg_owned,
                                              n_aggs, nrows))
                goto ht_path;
            /* VLA sized to vla_aggs (== n_aggs): agg_ptrs/agg_types are
             * per-element arrays, not bitmasks.  sp_eligible caps n_aggs <=
             * 64 — the width of the agg_f64_mask bitmask built just below,
             * not a VLA bound. */
            void* agg_ptrs[vla_aggs];
            int8_t agg_types[vla_aggs];
            uint64_t agg_f64_mask = 0;
            for (uint32_t a = 0; a < n_aggs; a++) {
                if (agg_vecs[a]) {
                    agg_ptrs[a] = ray_data(agg_vecs[a]);
                    agg_types[a] = agg_vecs[a]->type;
                    if (agg_vecs[a]->type == RAY_F64)
                        agg_f64_mask |= ((uint64_t)1 << a);
                } else {
                    agg_ptrs[a] = NULL;
                    agg_types[a] = 0;
                }
            }
            ray_t** strlen_sym_strings = NULL;
            uint32_t strlen_sym_count = 0;
            for (uint32_t a = 0; a < n_aggs; a++) {
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
                sp_dyn_ctx_t sc = {
                    .g = g, .ext = ext, .key_data = key_data,
                    .key_types = key_types, .key_vecs = key_vecs,
                    .key_owned = key_owned, .agg_ptrs = agg_ptrs,
                    .agg_types = agg_types, .agg_strlen = agg_strlen,
                    .agg_vecs = agg_vecs, .agg_owned = agg_owned,
                    .agg_affine = agg_affine, .agg_prod = agg_prod,
                    .strlen_sym_strings = strlen_sym_strings,
                    .strlen_sym_count = strlen_sym_count,
                    .agg_f64_mask = agg_f64_mask, .n_aggs = n_aggs,
                    .n_keys = n_keys, .n_scan = n_scan, .key_esz = key_esz,
                    .sp_need_sum = sp_need_sum, .match_idx = match_idx,
                    .rowsel = rowsel, .match_idx_block = match_idx_block,
                    .vla_hdr = vla_hdr, .emit_filter = emit_filter,
                };
                ray_t* sp_r = exec_group_sp_dyn_emit(&sc);
                if (sp_r) return sp_r;
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
                            for (uint32_t a = 0; a < n_aggs; a++) {
                                if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a])
                                    continue;
                                if (agg_strlen[a])
                                    sums[a].i += group_strlen_at_cached(
                                        agg_vecs[a], r, strlen_sym_strings,
                                        strlen_sym_count);
                                else if (agg_f64_mask & ((uint64_t)1 << a))
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
                        for (uint32_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint32_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
                        return result ? result : ray_error("oom", NULL);
                    }

                    ray_t* key_col = col_vec_new(key_vecs[0], (int64_t)grp_count);
                    out_col_adopt_str_pool(key_col, key_vecs[0]);
                    if (key_col && !RAY_IS_ERR(key_col) && key_col->type == RAY_SYM)
                        /* raw cell ids from key_vecs[0] — adopt its domain */
                        ray_sym_vec_adopt_domain(key_col, sym_domain_rep(key_vecs[0]));
                    if (!key_col || RAY_IS_ERR(key_col)) {
                        scratch_free(range_sum_hdr);
                        scratch_free(cnt_hdr);
                        ray_release(result);
                        for (uint32_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint32_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
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
                        for (uint32_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint32_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
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
                            for (uint32_t a = 0; a < n_aggs; a++) {
                                if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a])
                                    continue;
                                if (agg_strlen[a])
                                    sums[a].i += group_strlen_at_cached(
                                        agg_vecs[a], r, strlen_sym_strings,
                                        strlen_sym_count);
                                else if (agg_f64_mask & ((uint64_t)1 << a))
                                    sums[a].f += ((const double*)agg_ptrs[a])[r];
                                else
                                    sums[a].i += read_col_i64(agg_ptrs[a], r,
                                                              agg_types[a], 0);
                            }
                        }
                    }

                    scratch_free(range_sum_hdr);
                    scratch_free(cnt_hdr);
                    ray_op_ext_t* key_ext = find_ext(g, ext->keys[0]);
                    int64_t name_id = key_ext ? key_ext->sym : 0;
                    result = ray_table_add_col(result, name_id, key_col);
                    ray_release(key_col);

                    /* nn_counts == NULL: same null-free guard as above; the
                     * emit-filter range path only runs when sp_eligible was
                     * true. */
                    emit_agg_columns(&result, g, ext, agg_vecs, grp_count, n_aggs,
                                     (double*)dense_sum, (int64_t*)dense_sum,
                                     NULL, NULL, NULL, NULL,
                                     dense_count, agg_affine, agg_prod, NULL, NULL,
                                     NULL, NULL, NULL);

                    scratch_free(_h_sum);
                    scratch_free(_h_cnt);
                    for (uint32_t a = 0; a < n_aggs; a++)
                        if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                    for (uint32_t k = 0; k < n_keys; k++)
                        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                    if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
                    return result;
                }
            }

            sparse_i64_ht_t sp_ht;
            memset(&sp_ht, 0, sizeof(sp_ht));
            bool sp_ok = true;

            if (use_emit_filter &&
                (emit_filter.min_count_exclusive > 0 ||
                 emit_filter.top_count_take > 0)) {
                if (n_scan > (1 << 21)) goto ht_path;
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
                    for (uint32_t a = 0; a < n_aggs; a++) {
                        if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a])
                            continue;
                        if (agg_strlen[a])
                            sums[a].i += group_strlen_at_cached(
                                agg_vecs[a], r, strlen_sym_strings,
                                strlen_sym_count);
                        else if (agg_f64_mask & ((uint64_t)1 << a))
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
                for (uint32_t a = 0; a < n_aggs; a++)
                    if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                for (uint32_t k = 0; k < n_keys; k++)
                    if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
                return result ? result : ray_error("oom", NULL);
            }

            ray_t* key_col = col_vec_new(key_vecs[0], (int64_t)grp_count);
            out_col_adopt_str_pool(key_col, key_vecs[0]);
            if (key_col && !RAY_IS_ERR(key_col) && key_col->type == RAY_SYM)
                /* raw cell ids from key_vecs[0] — adopt its domain */
                ray_sym_vec_adopt_domain(key_col, sym_domain_rep(key_vecs[0]));
            if (!key_col || RAY_IS_ERR(key_col)) {
                sparse_i64_free(&sp_ht);
                ray_release(result);
                for (uint32_t a = 0; a < n_aggs; a++)
                    if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                for (uint32_t k = 0; k < n_keys; k++)
                    if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
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
                for (uint32_t a = 0; a < n_aggs; a++)
                    if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                for (uint32_t k = 0; k < n_keys; k++)
                    if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
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
                    for (uint32_t a = 0; a < n_aggs; a++)
                        if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                    for (uint32_t k = 0; k < n_keys; k++)
                        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                    if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
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
                        for (uint32_t a = 0; a < n_aggs; a++)
                            if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
                        for (uint32_t k = 0; k < n_keys; k++)
                            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
                        if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
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
                    for (uint32_t a = 0; a < n_aggs; a++) {
                        if (ext->agg_ops[a] == OP_COUNT || !agg_ptrs[a])
                            continue;
                        if (agg_strlen[a])
                            sums[a].i += group_strlen_at_cached(
                                agg_vecs[a], r, strlen_sym_strings,
                                strlen_sym_count);
                        else if (agg_f64_mask & ((uint64_t)1 << a))
                            sums[a].f += ((const double*)agg_ptrs[a])[r];
                        else
                            sums[a].i += read_col_i64(agg_ptrs[a], r, agg_types[a], 0);
                    }
                }
            }
            sparse_i64_free(&heavy_ht);
            ray_op_ext_t* key_ext = find_ext(g, ext->keys[0]);
            int64_t name_id = key_ext ? key_ext->sym : 0;
            result = ray_table_add_col(result, name_id, key_col);
            ray_release(key_col);

            /* nn_counts == NULL: sparse HT path only handles SUM/AVG/COUNT
             * and is gated to null-free agg columns (sp_eligible guard at
             * ~line 5737), so counts[gi] is the correct divisor. */
            emit_agg_columns(&result, g, ext, agg_vecs, grp_count, n_aggs,
                             (double*)dense_sum, (int64_t*)dense_sum,
                             NULL, NULL, NULL, NULL,
                             dense_count, agg_affine, agg_prod, NULL, NULL,
                             NULL, NULL, NULL);

            scratch_free(_h_sum);
            scratch_free(_h_cnt);
            for (uint32_t a = 0; a < n_aggs; a++)
                if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
            for (uint32_t k = 0; k < n_keys; k++)
                if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
            if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
            return result;
        }
    }

ht_path:;
    /* n_aggs > 8 guard RETIRED (unbounded-slots cut 4): the HT row-layout is
     * now layout-sized end to end (ght_layout_t agg metadata is unbounded, the
     * entry-staging buffers spill past 8), so this fallback — reached by the
     * keyless shapes that skip the scalar fast path (a PEARSON_CORR binary agg,
     * or an empty table) — serves any agg count.  n_keys > 0 shapes flow the
     * same machinery. */
    /* The HT row-layout reads agg_vecs directly — convert any fused-product
     * slots into ordinary materialized inputs first. */
    if (!group_materialize_prod_slots(agg_prod, agg_vecs, agg_owned,
                                      n_aggs, nrows)) {
        for (uint32_t a = 0; a < n_aggs; a++)
            { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
              if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
        for (uint32_t k = 0; k < n_keys; k++)
            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
        if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
        return ray_error("oom", NULL);
    }

    /* Compute which accumulator arrays the HT needs based on agg ops.
     * COUNT only reads group row's count field — no accumulator needed. */
    uint8_t ght_need = 0;
    for (uint32_t a = 0; a < n_aggs; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop == OP_SUM || aop == OP_PROD || aop == OP_AVG || aop == OP_ALL || aop == OP_ANY || aop == OP_FIRST || aop == OP_LAST)
            ght_need |= GHT_NEED_SUM;
        if (aop == OP_STDDEV || aop == OP_STDDEV_POP || aop == OP_VAR || aop == OP_VAR_POP)
            { ght_need |= GHT_NEED_SUM; ght_need |= GHT_NEED_SUMSQ; }
        if (agg_is_binary_agg(aop))
            { ght_need |= GHT_NEED_SUM; ght_need |= GHT_NEED_SUMSQ;
              ght_need |= GHT_NEED_PEARSON; }
        if (aop == OP_MIN) ght_need |= GHT_NEED_MIN;
        if (aop == OP_MAX) ght_need |= GHT_NEED_MAX;
    }

    /* RAY_STR keys are now handled inline by the wide-key mechanism (row
     * indirection + SSO-aware hash/eq via key_pool); see ght_layout_t. */

    /* Compute row-layout: keys + agg values inline.  The ≤8-key/≤8-agg width
     * guards are retired: any key/agg count is served, with ght_compute_layout
     * carving an owned spill block past GHT_INLINE.  It fails here only on
     * spill OOM or a representational budget overflow — the uint16 key/agg
     * stride budgets or the int8 value-slot count — none of which a ≤8 shape
     * can hit. */
    ght_layout_t ght_layout;
    if (!ght_compute_layout(&ght_layout, n_keys, n_aggs, agg_vecs, ght_need,
                            ext->agg_ops, key_types)) {
        for (uint32_t a = 0; a < n_aggs; a++)
            { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]);
              if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
        for (uint32_t k = 0; k < n_keys; k++)
            if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
        if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);
        return ray_error("limit", "group: layout stride/slot budget exceeded (or OOM)");
    }

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

    /* Top-N-by-count (`select … by … desc:c take:N`) is served by the
     * parallel radix_v2 path below: phase1/phase2 build per-partition HTs
     * and the v2_emit bounded-heap compaction (use_topn_filter) keeps only
     * the globally top-N groups before phase3 emit.  That path is
     * competitive-or-faster than a single-threaded SoA candidate finder
     * across all input sizes (measured), so there is no separate
     * size-gated shortcut here. */

    if (pool && nrows >= RAY_PARALLEL_THRESHOLD && n_total > 1) {
        /* Filtered (rowsel/match_idx) group-bys are allowed on the parallel
         * paths: both radix_v2_phase1_fn and radix_phase1_fn honour c->rowsel
         * (skip non-passing rows during scatter), so partitioned data holds
         * only passing rows.  Previously gated `!rowsel`, which dropped every
         * WHERE'd group-by onto the single-threaded group_rows_range.
         * Per-(worker, partition) direct-insert path: aggregates into
         * thread-local partition HTs during phase1, then merges per
         * partition.  Bypasses the phase1 fat-entry materialisation +
         * phase2 re-read DRAM round trip.  On success it populates
         * part_hts[] in the format the existing phase3 emit consumes.
         *
         * Gate: every agg is COUNT/SUM/AVG (the merge primitive knows
         * how to add counts and sum slots; PROD/MIN/MAX/FIRST/LAST/
         * SUMSQ/PEARSON/MEDIAN need richer state-merge logic).  Agg
         * input columns must be non-nullable for now — sentinel-skip
         * inside accum_from_entry is correct, but the merge step needs
         * an nn_count and that isn't tracked yet. */
        bool v2_ok = (n_keys >= 1 && n_aggs > 0);
        /* Exclude SYM keys from the per-worker direct-insert path: it pays
         * here because SYM keys make the per-row composite-key probe
         * expensive.  Direct-insert hashes and probes the full composite
         * key into a per-(worker, partition) hash table on every input row;
         * with a SYM key that means resolving the SYM dictionary id and
         * mixing it into the composite hash per probe.  The fat-entry
         * fallback instead lets the radix machinery assign a dense group id
         * once (a single keyed pass) and then accumulates by that compact
         * id, so it avoids re-probing the wide composite key per row.  At
         * mid/high SYM cardinality the per-probe cost dominates and
         * direct-insert is ~3x slower; only at very low cardinality (where
         * the working set is tiny and probes hit cache) do the two converge.
         * So route any SYM-keyed group-by through the fat-entry pipeline. */
        for (uint32_t k = 0; k < n_keys && v2_ok; k++)
            if (key_types[k] == RAY_SYM) v2_ok = false;
        for (uint32_t a = 0; a < n_aggs && v2_ok; a++) {
            uint16_t op = ext->agg_ops[a];
            if (op != OP_COUNT && op != OP_SUM && op != OP_AVG) {
                v2_ok = false;
                break;
            }
            if (agg_vecs[a]) {
                ray_t* src = (agg_vecs[a]->attrs & RAY_ATTR_SLICE)
                             ? agg_vecs[a]->slice_parent : agg_vecs[a];
                if (src && (src->attrs & RAY_ATTR_HAS_NULLS))
                    v2_ok = false;
            }
        }
        if (v2_ok && !(ght_layout.agg_flags_any & (GHT_AF_FIRST | GHT_AF_LAST
                        | GHT_AF_HOLISTIC | GHT_AF_BINARY))) {
            ray_t* wpart_hdr = NULL;
            size_t v2_n_w = (size_t)n_total * RADIX_P;
            group_ht_t* wpart_hts = (group_ht_t*)scratch_calloc(
                &wpart_hdr, v2_n_w * sizeof(group_ht_t));
            ray_t* v2_part_hdr = NULL;
            group_ht_t* v2_part_hts = wpart_hts
                ? (group_ht_t*)scratch_calloc(&v2_part_hdr,
                                              RADIX_P * sizeof(group_ht_t))
                : NULL;
            if (!wpart_hts || !v2_part_hts) {
                if (wpart_hts) scratch_free(wpart_hdr);
                if (v2_part_hts) scratch_free(v2_part_hdr);
                goto v2_done;
            }
            uint8_t v2_nullable = 0;   /* 0/1: any key may be null (per-key re-checked in phase1) */
            for (uint32_t k = 0; k < n_keys; k++)
                if (ray_key_may_be_null(key_vecs[k])) { v2_nullable = 1; break; }
            /* Selection-aware iteration: for a sparse WHERE, iterate the
             * survivor row list instead of scanning all nrows with a per-row
             * rowsel check.  Scoped to this v2 build; freed right after the
             * (blocking) phase-1 dispatch — phase-2 merges HTs and never reads
             * match_idx.  Left NULL (scan path) for dense selections.
             * Byte-identity of results depends on ray_rowsel_to_indices
             * returning survivors in ASCENDING row order (rowsel.h): that
             * matches the full-scan skip order, so HT insertion order — and
             * thus the emitted group order — is unchanged. */
            ray_t*         sel_match_block = NULL;
            const int64_t* sel_match       = NULL;
            int64_t        sel_n           = 0;
            if (g->selection) {
                ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
                if (sm && sm->nrows == nrows
                    && sm->total_pass < (nrows >> SEL_MATCH_GATE_SHIFT)) {
                    sel_match_block = ray_rowsel_to_indices(g->selection);
                    if (sel_match_block) {
                        sel_match = (const int64_t*)ray_data(sel_match_block);
                        sel_n     = sm->total_pass;
                    }
                }
            }
            radix_v2_phase1_ctx_t v2p1 = {
                .key_data      = key_data,
                .key_types     = key_types,
                .key_attrs     = key_attrs,
                .key_vecs      = key_vecs,
                .agg_vecs      = agg_vecs,
                .agg_vecs2     = agg_vecs2,
                .agg_strlen    = agg_strlen,
                .nullable_mask = v2_nullable,
                .n_workers     = n_total,
                .wpart_hts     = wpart_hts,
                .rowsel        = rowsel,
                .match_idx     = sel_match,
                .oom           = 0,
            };
            /* By-value embed the layout, fixing its base pointers to v2p1's
             * own inline arrays (inline) / shared spill (wide); the master
             * ght_layout outlives this dispatch. */
            ght_layout_copy(&v2p1.layout, &ght_layout);
            ray_pool_dispatch(pool, radix_v2_phase1_fn, &v2p1,
                              sel_match ? sel_n : n_scan);
            if (sel_match_block) ray_release(sel_match_block);
            CHECK_CANCEL_GOTO(pool, cleanup);
            if (atomic_load_explicit(&v2p1.oom, memory_order_relaxed)) {
                for (size_t i = 0; i < v2_n_w; i++)
                    group_ht_free(&wpart_hts[i]);
                scratch_free(wpart_hdr);
                scratch_free(v2_part_hdr);
                goto v2_done;
            }
            radix_v2_phase2_ctx_t v2p2 = {
                .wpart_hts = wpart_hts,
                .part_hts  = v2_part_hts,
                .key_types = key_types,
                .n_workers = n_total,
                .key_data  = key_data,
                .oom       = 0,
            };
            ght_layout_copy(&v2p2.layout, &ght_layout);
            /* Wide-key str-pool table: phase2 copies each into its part_hts
             * (whose bases point into the source columns), so this temp is
             * freed right after the dispatch — mirrors p2ctx below. */
            ray_t* v2p2_kp_hdr = NULL;
            if (ght_layout.any_wide_key) {
                v2p2.key_pool = (const void**)scratch_alloc(&v2p2_kp_hdr,
                    (size_t)(n_keys ? n_keys : 1) * sizeof(void*));
                if (!v2p2.key_pool) {
                    /* cleanup frees only part_hts (wired at v2_emit, below); the
                     * worker HTs + both partition-table blocks are still local
                     * here — free them like the phase1-OOM bail above. */
                    for (size_t i = 0; i < v2_n_w; i++)
                        group_ht_free(&wpart_hts[i]);
                    scratch_free(wpart_hdr);
                    scratch_free(v2_part_hdr);
                    result = ray_error("oom", NULL); goto cleanup;
                }
                derive_key_pool(&ght_layout, key_vecs, v2p2.key_pool);
            }
            ray_pool_dispatch_n(pool, radix_v2_phase2_fn, &v2p2, RADIX_P);
            scratch_free(v2p2_kp_hdr);
            CHECK_CANCEL_GOTO(pool, cleanup);
            /* Worker HTs are no longer needed once the merge is done. */
            for (size_t i = 0; i < v2_n_w; i++)
                group_ht_free(&wpart_hts[i]);
            scratch_free(wpart_hdr);
            if (atomic_load_explicit(&v2p2.oom, memory_order_relaxed)) {
                for (uint32_t p = 0; p < RADIX_P; p++)
                    group_ht_free(&v2_part_hts[p]);
                scratch_free(v2_part_hdr);
                goto v2_done;
            }
            /* Hand off to the existing phase3 emit. */
            part_hts = v2_part_hts;
            part_hts_hdr = v2_part_hdr;
            goto v2_emit;
        }
v2_done:;
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

        /* Any key column that may hold nulls — lets phase1 skip the per-key
         * null check on the common (no-nulls) fast path.  0/1 summary so key
         * indices past 63 track correctly; phase1 re-derives per-key. */
        uint8_t p1_nullable = 0;
        for (uint32_t k = 0; k < n_keys; k++)
            if (ray_key_may_be_null(key_vecs[k])) { p1_nullable = 1; break; }

        /* Pass 1: parallel hash + copy keys/agg values into fat entries */
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
            .rowsel        = rowsel,
            .match_idx     = match_idx,
        };
        ght_layout_copy(&p1ctx.layout, &ght_layout);
        /* Wide-key str-pool table (n_keys slots): carve once (never per row);
         * consumed within the phase1 dispatch, freed right after — cut 4. */
        ray_t* p1_kp_hdr = NULL;
        if (ght_layout.any_wide_key) {
            p1ctx.key_pool = (const void**)scratch_alloc(&p1_kp_hdr,
                (size_t)n_keys * sizeof(void*));
            if (!p1ctx.key_pool) { result = ray_error("oom", NULL); goto cleanup; }
            derive_key_pool(&ght_layout, key_vecs, p1ctx.key_pool);
        }
        ray_pool_dispatch(pool, radix_phase1_fn, &p1ctx, n_scan);
        scratch_free(p1_kp_hdr);
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

        /* Pass 2: parallel per-partition aggregation (no column access) */
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
            .key_data    = key_data,
        };
        ght_layout_copy(&p2ctx.layout, &ght_layout);
        /* Wide-key str-pool table: phase2 copies each into its part_hts (whose
         * bases point into the source columns), so this temp is freed right
         * after the dispatch — cut 4. */
        ray_t* p2_kp_hdr = NULL;
        if (ght_layout.any_wide_key) {
            p2ctx.key_pool = (const void**)scratch_alloc(&p2_kp_hdr,
                (size_t)n_keys * sizeof(void*));
            if (!p2ctx.key_pool) { result = ray_error("oom", NULL); goto cleanup; }
            derive_key_pool(&ght_layout, key_vecs, p2ctx.key_pool);
        }
        ray_pool_dispatch_n(pool, radix_phase2_fn, &p2ctx, RADIX_P);
        scratch_free(p2_kp_hdr);
        CHECK_CANCEL_GOTO(pool, cleanup);

        if (radix_bufs) {
            size_t n_bufs_free = (size_t)n_total * RADIX_P;
            for (size_t i = 0; i < n_bufs_free; i++)
                scratch_free(radix_bufs[i]._hdr);
            scratch_free(radix_bufs_hdr);
            radix_bufs = NULL;
            radix_bufs_hdr = NULL;
            /* No explicit GC — top-level statement GC catches it. */
        }

v2_emit:;
        /* Top-N aware compaction: when the (select … by … desc: c take: N)
         * shape is in flight (use_emit_filter + top_count_take, COUNT agg),
         * the global answer is the N rows with the largest count across
         * all partitions.  Run a global bounded-heap (size N) over the
         * union of per-partition rows here, then in-place compact each
         * partition's row array to contain only globally-surviving rows.
         * Phase3 below then emits N rows total instead of total_grps —
         * the major win for high-cardinality keys like UserID/URL where
         * total_grps is in the millions but N is ≤ 1024.
         *
         * Implementation notes:
         *  - The bounded heap orders by count (the agg at COUNT slot, the
         *    first int64 in each row).  Equal counts are stable: the
         *    first row seen wins.  Final per-partition row order is
         *    preserved so apply_sort_take below can do the final
         *    arrange-by-agg deterministically.
         *  - We also handle the "fewer total rows than N" case — compact
         *    becomes a no-op.
         *  - Only fires when emit_filter.top_count_take > 0; existing
         *    min_count_exclusive-only filters fall through unchanged. */
        if (use_topn_filter) {
            int64_t k_take = emit_filter.top_count_take;
            uint32_t total_pre = 0;
            for (uint32_t p = 0; p < RADIX_P; p++)
                total_pre += part_hts[p].grp_count;
            /* Resolve the in-row offset of the order-by agg's value.  For
             * COUNT it's the leading int64 at offset 0; for SUM/MIN/MAX
             * it's the per-slot int64 in off_sum/off_min/off_max.  F64
             * agg outputs (sum over an F64 column) compare by bitcast —
             * for IEEE 754 the bit pattern preserves ordering for finite
             * positive values; mixed-sign and NaN cases drop the heap
             * back to a wider comparator.  To stay correct we exclude
             * F64-output aggs from this fast path (the COUNT count is
             * always I64, and SUM/MIN/MAX over an integer column keep
             * an I64 slot — agg_is_f64 marks the SUM-over-F64 case). */
            uint16_t order_op = emit_filter.agg_op
                ? emit_filter.agg_op
                : (uint16_t)OP_COUNT;
            uint8_t  agg_index_local = emit_filter.agg_index;
            uint16_t order_off = 0;  /* default: COUNT at row+0 */
            bool order_is_f64 = false;
            if (agg_index_local < n_aggs &&
                (ght_layout.agg_flags[agg_index_local] & GHT_AF_F64))
                order_is_f64 = true;
            int8_t agg_slot = ght_layout.agg_val_slot[agg_index_local];
            if (order_op == OP_SUM) {
                if (agg_slot < 0 || order_is_f64) goto topn_compact_skip;
                order_off = (uint16_t)(ght_layout.off_sum
                                       + (uint16_t)agg_slot * 8u);
            } else if (order_op == OP_MIN) {
                if (agg_slot < 0 || order_is_f64) goto topn_compact_skip;
                if (ght_layout.agg_flags[agg_index_local] & GHT_AF_SYM)
                    goto topn_compact_skip;
                order_off = (uint16_t)(ght_layout.off_min
                                       + (uint16_t)agg_slot * 8u);
            } else if (order_op == OP_MAX) {
                if (agg_slot < 0 || order_is_f64) goto topn_compact_skip;
                if (ght_layout.agg_flags[agg_index_local] & GHT_AF_SYM)
                    goto topn_compact_skip;
                order_off = (uint16_t)(ght_layout.off_max
                                       + (uint16_t)agg_slot * 8u);
            }
            uint8_t desc_dir = emit_filter.desc ? 1 : 0;
            /* COUNT defaults to desc when the filter struct's desc bit
             * isn't set (old single-bit filter shape).  Producer code in
             * query.c sets it explicitly. */
            if (order_op == OP_COUNT && !emit_filter.desc) desc_dir = 1;
            if ((int64_t)total_pre > k_take && k_take > 0 && k_take <= 1024) {
                /* Stack heap: (val, part, gid) triples.  k_take ≤ 1024
                 * caps the footprint at 1024 * 16 B = 16 KiB.  The heap
                 * invariant flips by direction: min-heap for desc (we
                 * evict the smallest to keep the largest N), max-heap
                 * for asc (evict the largest to keep the smallest N). */
                int64_t hval[1024];
                uint32_t hpart[1024];
                uint32_t hgid[1024];
                int64_t hn = 0;
                /* For top-N largest (desc=1): min-heap.  Root is smallest;
                 * incoming v replaces root iff v > root.  Heap invariant:
                 * parent ≤ child (so swap when parent > child).
                 *
                 * For top-N smallest (desc=0): max-heap.  Root is largest;
                 * incoming v replaces root iff v < root.  Heap invariant:
                 * parent ≥ child (so swap when parent < child).
                 *
                 * TOPN_NEEDS_SWAP(parent, child) := does the parent
                 * violate the invariant relative to child? */
                #define TOPN_NEEDS_SWAP(parent, child) \
                    (desc_dir ? ((parent) > (child)) : ((parent) < (child)))
                #define TOPN_SHOULD_REPLACE(new_v, root_v) \
                    (desc_dir ? ((new_v) > (root_v)) : ((new_v) < (root_v)))
                for (uint32_t p = 0; p < RADIX_P; p++) {
                    group_ht_t* ph = &part_hts[p];
                    uint16_t rs = ph->layout.row_stride;
                    uint32_t gc = ph->grp_count;
                    for (uint32_t gi = 0; gi < gc; gi++) {
                        const char* row = ph->rows + (size_t)gi * rs;
                        int64_t v = *(const int64_t*)(const void*)
                                    (row + order_off);
                        if (hn < k_take) {
                            int64_t j = hn++;
                            hval[j] = v; hpart[j] = p; hgid[j] = gi;
                            /* Sift up: bubble new entry toward root while
                             * parent violates invariant. */
                            while (j > 0) {
                                int64_t pr = (j - 1) >> 1;
                                if (!TOPN_NEEDS_SWAP(hval[pr], hval[j])) break;
                                int64_t tc = hval[pr]; hval[pr] = hval[j]; hval[j] = tc;
                                uint32_t tp = hpart[pr]; hpart[pr] = hpart[j]; hpart[j] = tp;
                                uint32_t tg = hgid[pr]; hgid[pr] = hgid[j]; hgid[j] = tg;
                                j = pr;
                            }
                        } else if (TOPN_SHOULD_REPLACE(v, hval[0])) {
                            hval[0] = v; hpart[0] = p; hgid[0] = gi;
                            int64_t j = 0;
                            /* Sift down: find the child that should be
                             * promoted (the one most violating the
                             * invariant) and swap. */
                            for (;;) {
                                int64_t l = j * 2 + 1, r = l + 1, m = j;
                                if (l < hn && TOPN_NEEDS_SWAP(hval[m], hval[l])) m = l;
                                if (r < hn && TOPN_NEEDS_SWAP(hval[m], hval[r])) m = r;
                                if (m == j) break;
                                int64_t tc = hval[m]; hval[m] = hval[j]; hval[j] = tc;
                                uint32_t tp = hpart[m]; hpart[m] = hpart[j]; hpart[j] = tp;
                                uint32_t tg = hgid[m]; hgid[m] = hgid[j]; hgid[j] = tg;
                                j = m;
                            }
                        }
                    }
                }
                #undef TOPN_NEEDS_SWAP
                #undef TOPN_SHOULD_REPLACE
                if (hn > 0) {
                    /* Build per-partition keep lists (sorted asc by gid so
                     * the in-place compact below is a single forward sweep). */
                    uint16_t keep_n[RADIX_P];
                    for (uint32_t p = 0; p < RADIX_P; p++) keep_n[p] = 0;
                    /* Cap per-partition kept count at hn (≤ k_take ≤ 1024). */
                    uint32_t kgid[RADIX_P][1024];
                    for (int64_t i = 0; i < hn; i++) {
                        uint32_t p = hpart[i];
                        uint16_t kn = keep_n[p];
                        /* Insertion-sort into kgid[p][] keeping asc order. */
                        uint16_t j = kn;
                        while (j > 0 && kgid[p][j - 1] > hgid[i]) {
                            kgid[p][j] = kgid[p][j - 1];
                            j--;
                        }
                        kgid[p][j] = hgid[i];
                        keep_n[p] = (uint16_t)(kn + 1);
                    }
                    /* In-place compact each partition. */
                    bool rebuilt_slots = false;
                    for (uint32_t p = 0; p < RADIX_P; p++) {
                        group_ht_t* ph = &part_hts[p];
                        uint16_t rs = ph->layout.row_stride;
                        uint16_t kn = keep_n[p];
                        if (kn == ph->grp_count) continue;  /* all kept */
                        rebuilt_slots = true;
                        if (kn == 0) { ph->grp_count = 0; continue; }
                        for (uint16_t i = 0; i < kn; i++) {
                            uint32_t src = kgid[p][i];
                            if (src == (uint32_t)i) continue;
                            memmove(ph->rows + (size_t)i * rs,
                                    ph->rows + (size_t)src * rs, rs);
                        }
                        ph->grp_count = kn;
                    }
                    if (rebuilt_slots) {
                        for (uint32_t p = 0; p < RADIX_P; p++)
                            group_ht_rebuild_slots(&part_hts[p], key_types);
                    }
                }
            }
            topn_compact_skip:;
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
        for (uint32_t k = 0; k < n_keys; k++) {
            ray_t* src_col = key_vecs[k];
            key_cols[k] = NULL;
            key_dsts[k] = NULL;
            key_out_types[k] = 0;
            key_esizes[k] = 0;
            if (!src_col) continue;
            uint8_t esz = ray_sym_elem_size(src_col->type, src_col->attrs);
            ray_t* new_col;
            if (src_col->type == RAY_SYM) {
                new_col = ray_sym_vec_new(src_col->attrs & RAY_SYM_W_MASK, (int64_t)total_grps);
                /* raw key cell ids copied from src_col — adopt its domain */
                if (new_col && !RAY_IS_ERR(new_col))
                    ray_sym_vec_adopt_domain(new_col, sym_domain_rep(src_col));
            } else if (src_col->type == RAY_STR) {
                /* Wide STR key: the emit copies the 16-byte ray_str_t
                 * descriptor by source row index; SHARE the source column's
                 * string pool so pooled descriptors (>12 B) resolve against it
                 * (inline ≤12 B descriptors are self-contained). */
                new_col = ray_vec_new(RAY_STR, (int64_t)total_grps);
                if (new_col && !RAY_IS_ERR(new_col)) {
                    ray_t* owner = (src_col->attrs & RAY_ATTR_SLICE)
                                   ? src_col->slice_parent : src_col;
                    if (owner && owner->str_pool && !RAY_IS_ERR(owner->str_pool)) {
                        ray_retain(owner->str_pool);
                        new_col->str_pool = owner->str_pool;
                    }
                }
            } else
                new_col = ray_vec_new(src_col->type, (int64_t)total_grps);
            if (!new_col || RAY_IS_ERR(new_col)) continue;
            new_col->len = (int64_t)total_grps;
            key_cols[k] = new_col;
            key_dsts[k] = (char*)ray_data(new_col);
            key_out_types[k] = src_col->type;
            key_esizes[k] = esz;
        }

        /* Pre-allocate agg result vectors.  These VLAs scale with n_aggs
         * (the ≤8-agg ht_path guard is retired — 9/17/65-agg legacy shapes
         * reach here); ght_compute_layout has already accepted this n_aggs,
         * so it is within the layout stride/slot budget. */
        agg_out_t agg_outs[n_aggs];
        ray_t* agg_cols[n_aggs];
        for (uint32_t a = 0; a < n_aggs; a++) {
            uint16_t agg_op = ext->agg_ops[a];
            ray_t* agg_col = agg_vecs[a];
            bool is_f64 = agg_col && agg_col->type == RAY_F64;
            int8_t out_type;
            switch (agg_op) {
                case OP_AVG:
                case OP_STDDEV: case OP_STDDEV_POP:
                case OP_VAR: case OP_VAR_POP:
                case OP_PEARSON_CORR:
                case OP_COV: case OP_SCOV:
                case OP_WSUM: case OP_WAVG:
                case OP_MEDIAN:
                case OP_QUANTILE:
                    out_type = RAY_F64; break;
                case OP_ALL:
                case OP_ANY:
                    out_type = RAY_BOOL; break;
                case OP_COUNT: out_type = RAY_I64; break;
                case OP_SUM:
                    /* sum preserves TIME (duration-like); other integer
                     * families widen to I64.  DATE/TIMESTAMP rejected at
                     * type-admission. */
                    out_type = is_f64 ? RAY_F64
                             : (agg_col && agg_col->type == RAY_TIME ? RAY_TIME : RAY_I64);
                    break;
                case OP_PROD:
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
            /* SYM MIN/MAX/FIRST/LAST emit RAW cell ids accumulated from
             * agg_col — the output resolves over its dictionary. */
            if (out_type == RAY_SYM)
                ray_sym_vec_adopt_domain(new_col, sym_domain_rep(agg_col));
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

        /* Pass 3: parallel key gather + agg result building from inline rows */
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
        if (ght_layout.agg_flags_any & GHT_AF_HOLISTIC) {
            int64_t n_groups = (int64_t)total_grps;

            /* row_gid[nrows] — global group id per source row, or -1 on
             * miss (defensive sentinel; phase-2 inserts every probed row). */
            ray_t* rg_hdr = NULL;
            int64_t* row_gid = (int64_t*)scratch_alloc(&rg_hdr,
                (size_t)nrows * sizeof(int64_t));
            if (!row_gid) { result = ray_error("oom", NULL); goto cleanup; }
            memset(row_gid, 0xff, (size_t)nrows * sizeof(int64_t));

            uint8_t reprobe_nullable = 0;   /* 0/1: any key may be null */
            for (uint32_t k = 0; k < n_keys; k++)
                if (ray_key_may_be_null(key_vecs[k])) { reprobe_nullable = 1; break; }
            reprobe_ctx_t rp = {
                .key_data = key_data,
                .key_types = key_types,
                .key_attrs = key_attrs,
                .key_vecs = key_vecs,
                .n_keys = n_keys,
                .nullable_mask = reprobe_nullable,
                .layout = &ght_layout,
                .part_hts = part_hts,
                .part_offsets = part_offsets,
                .row_gid = row_gid,
                .match_idx = match_idx,
                .rowsel = rowsel,
            };
            /* Wide-key str-pool table (n_keys slots): carve once here (never
             * per row); NULL when no wide keys — cut 4. */
            ray_t* rp_kp_hdr = NULL;
            if (ght_layout.any_wide_key) {
                rp.key_pool = (const void**)scratch_alloc(&rp_kp_hdr,
                    (size_t)n_keys * sizeof(void*));
                if (!rp.key_pool) { scratch_free(rg_hdr); result = ray_error("oom", NULL); goto cleanup; }
                derive_key_pool(&ght_layout, key_vecs, rp.key_pool);
            }
            ray_pool_dispatch(pool, reprobe_rows_fn, &rp, n_scan);
            scratch_free(rp_kp_hdr);

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
                for (uint32_t a = 0; a < n_aggs; a++) {
                    if (!(ght_layout.agg_flags[a] & GHT_AF_HOLISTIC)) continue;
                    if (!agg_vecs[a] || !agg_cols[a]) continue;
                    uint16_t aop = ext->agg_ops[a];
                    ray_t* hol_vec = NULL;
                    const char* err_tag = "median: type";
                    if (aop == OP_MEDIAN) {
                        hol_vec = ray_median_per_group_buf(
                            agg_vecs[a], idx_buf, offsets, grp_cnt, n_groups);
                    } else if (aop == OP_QUANTILE) {
                        double q = ext->agg_k
                            ? decode_agg_f64_param(ext->agg_k[a]) : 0.5;
                        hol_vec = ray_quantile_per_group_buf(
                            agg_vecs[a], idx_buf, offsets, grp_cnt, n_groups, q);
                        err_tag = "quantile: type";
                    } else if (aop == OP_MODE) {
                        hol_vec = ray_mode_per_group_buf(
                            agg_vecs[a], idx_buf, offsets, grp_cnt, n_groups);
                        err_tag = "mode: type";
                    } else if (aop == OP_TOP_N || aop == OP_BOT_N) {
                        int64_t k_val = (ext->agg_k && ext->agg_k[a] > 0)
                                        ? ext->agg_k[a] : 1;
                        hol_vec = ray_topk_per_group_buf(
                            agg_vecs[a], k_val,
                            aop == OP_TOP_N ? 1 : 0,
                            idx_buf, offsets, grp_cnt, n_groups);
                        err_tag = "top/bot: type";
                    } else if (aop == OP_MIN || aop == OP_MAX ||
                               aop == OP_FIRST || aop == OP_LAST) {
                        hol_vec = ray_wide_minmax_per_group_buf(
                            agg_vecs[a], aop, idx_buf, offsets, grp_cnt, n_groups);
                        err_tag = "minmax: type";
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

        /* Finalize null flags after parallel execution.  Holistic slots
         * are filled by the post-radix pass into a fresh column; we
         * already updated agg_outs[a].vec to track it.  For RAY_LIST
         * cells (OP_TOP_N / OP_BOT_N) the per-cell null state is not
         * consulted downstream — finalize is a no-op-y read of attrs. */
        for (uint32_t a = 0; a < n_aggs; a++) {
            if (!agg_cols[a]) continue;
            if (agg_outs[a].vec && agg_outs[a].vec->type == RAY_LIST) continue;
            grp_finalize_nulls(agg_outs[a].vec);
        }
        for (uint32_t k = 0; k < n_keys; k++) {
            if (!key_cols[k]) continue;
            grp_finalize_nulls(key_cols[k]);
        }

        /* Add key columns to result */
        for (uint32_t k = 0; k < n_keys; k++) {
            if (!key_cols[k]) continue;
            ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]);
            int64_t name_id = key_ext ? key_ext->sym : k;
            result = ray_table_add_col(result, name_id, key_cols[k]);
            ray_release(key_cols[k]);
        }

        /* Add agg columns to result */
        for (uint32_t a = 0; a < n_aggs; a++) {
            if (!agg_cols[a]) continue;
            uint16_t agg_op = ext->agg_ops[a];
            ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]);
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
                    case OP_ALL:   sfx = "_all";   slen = 4; break;
                    case OP_ANY:   sfx = "_any";   slen = 4; break;
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
                    case OP_QUANTILE:   sfx = "_quantile";   slen = 9; break;
                    case OP_MODE:       sfx = "_mode";       slen = 5; break;
                    case OP_COV:        sfx = "_cov";        slen = 4; break;
                    case OP_SCOV:       sfx = "_scov";       slen = 5; break;
                    case OP_WSUM:       sfx = "_wsum";       slen = 5; break;
                    case OP_WAVG:       sfx = "_wavg";       slen = 5; break;
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
    {
    uint32_t grp_count = final_ht->grp_count;
    const ght_layout_t* ly = &final_ht->layout;
    int64_t total_cols = n_keys + n_aggs;
    result = ray_table_new(total_cols);
    if (!result || RAY_IS_ERR(result)) goto cleanup;

    /* Key columns: read from inline group rows, narrow to original type.
     * Wide keys store a source row index in the HT slot; resolve it
     * through the original key column (key_data[k]) and copy bytes. */
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* src_col = key_vecs[k];
        if (!src_col) continue;
        uint8_t esz = col_esz(src_col);
        int8_t kt = src_col->type;

        ray_t* new_col = col_vec_new(src_col, (int64_t)grp_count);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        out_col_adopt_str_pool(new_col, src_col);
        /* HT rows hold RAW key cell ids copied from src_col — the
         * output resolves over its dictionary (no-op while runtime). */
        if (new_col->type == RAY_SYM)
            ray_sym_vec_adopt_domain(new_col, sym_domain_rep(src_col));
        new_col->len = (int64_t)grp_count;

        bool is_wide = (ly->key_flags[k] & GHT_KEYF_WIDE) != 0;
        const char* src_base = is_wide ? (const char*)key_data[k] : NULL;

        bool inline_str_k = (ly->key_flags[k] & GHT_KEYF_INLINE_STR) != 0;
        /* Key k's null bit lives in null-mask word (k>>6), bit (k&63). */
        size_t null_woff = (size_t)ly->key_off[n_keys] + (size_t)(k >> 6) * 8;
        int64_t null_kbit = (int64_t)((uint64_t)1 << (k & 63));
        for (uint32_t gi = 0; gi < grp_count; gi++) {
            const char* row = final_ht->rows + (size_t)gi * ly->row_stride;
            const char* rk = row + 8;
            int64_t null_word = *(const int64_t*)(rk + null_woff);
            if (null_word & null_kbit) {
                ray_vec_set_null(new_col, (int64_t)gi, true);
                /* Fill the correct-width sentinel. */
                switch (kt) {
                    case RAY_F64:
                        ((double*)ray_data(new_col))[gi] = NULL_F64; break;
                    case RAY_I64: case RAY_TIMESTAMP:
                        ((int64_t*)ray_data(new_col))[gi] = NULL_I64; break;
                    case RAY_I32: case RAY_DATE: case RAY_TIME:
                        ((int32_t*)ray_data(new_col))[gi] = NULL_I32; break;
                    case RAY_I16:
                        ((int16_t*)ray_data(new_col))[gi] = NULL_I16; break;
                    case RAY_STR:
                        memset((char*)ray_data(new_col) + (size_t)gi * esz, 0,
                               sizeof(ray_str_t)); break;
                    default: break;
                }
                continue;
            }
            if (inline_str_k) {
                /* Inline STR descriptor lives in the row; output shares the pool. */
                memcpy((char*)ray_data(new_col) + (size_t)gi * esz,
                       rk + ly->key_off[k], sizeof(ray_str_t));
                continue;
            }
            int64_t kv = *(const int64_t*)(rk + ly->key_off[k]);
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

        ray_op_ext_t* key_ext = find_ext(g, ext->keys[k]);
        int64_t name_id = key_ext ? key_ext->sym : k;
        result = ray_table_add_col(result, name_id, new_col);
        ray_release(new_col);
    }

    /* If any holistic agg (OP_MEDIAN) is present, run a sequential
     * re-probe + median fill into a per-slot output vector array.
     * Built lazily on first need and reused across all median slots. */
    ray_t** med_out = NULL;
    ray_t* med_hdr = NULL;
    if (ly->agg_flags_any & GHT_AF_HOLISTIC) {
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
                uint8_t reprobe_nullable_s = 0;   /* 0/1: any key may be null */
                for (uint32_t k = 0; k < n_keys; k++)
                    if (ray_key_may_be_null(key_vecs[k])) { reprobe_nullable_s = 1; break; }
                /* Key lookup staging + per-key str-pool table: stack for ≤8
                 * keys, one heap block for wider (carved once — cut 4). */
                const void* reprobe_pool_stk[8];
                int64_t ek_buf_stk[9];
                char    keybuf_stk[136];
                const void** reprobe_pool = reprobe_pool_stk;
                int64_t* ek_buf = ek_buf_stk;
                char*    keybuf = keybuf_stk;
                ray_t*   hol_stage_hdr = NULL;
                if ((size_t)ly->key_region > sizeof(ek_buf_stk) || n_keys > 8) {
                    size_t kb = ly->key_region;
                    char* blk = (char*)scratch_alloc(&hol_stage_hdr,
                        kb + kb + (size_t)n_keys * sizeof(void*));
                    if (!blk) {
                        scratch_free(rg_hdr); scratch_free(cnt_hdr_s);
                        scratch_free(off_hdr_s); scratch_free(pos_hdr_s);
                        result = ray_error("oom", NULL); goto cleanup;
                    }
                    keybuf       = blk;
                    ek_buf       = (int64_t*)(blk + kb);
                    reprobe_pool = (const void**)(blk + kb + kb);
                }
                derive_key_pool(ly, key_vecs, reprobe_pool);
                for (int64_t i = 0; i < n_scan; i++) {
                    int64_t row = match_idx ? match_idx[i] : i;
                    /* Holistic fill re-scans rows independently of the streaming
                     * grouping accumulators, so it must honor the pushed WHERE
                     * filter itself.  When the selection is carried as a rowsel
                     * (match_idx == NULL, the default), skip rows that don't
                     * pass — otherwise median/top would aggregate filtered-out
                     * rows (the unfiltered group), diverging from every other
                     * agg which already respects the selection. */
                    if (!match_idx && !group_rowsel_pass(rowsel, row)) continue;
                    uint64_t h = 0;
                    const int64_t* lookup_keys;
                    if (ly->any_inline_str) {
                        h = inline_build_keys(ly, key_types, key_data, key_attrs,
                                              reprobe_pool, key_vecs, reprobe_nullable_s,
                                              row, keybuf);
                        lookup_keys = (const int64_t*)keybuf;
                    } else {
                    int64_t* nullw = ek_buf + n_keys;   /* null-mask words at key_off[nk] */
                    uint32_t null_words = ly->null_words;
                    for (uint32_t w = 0; w < null_words; w++) nullw[w] = 0;
                    for (uint32_t k = 0; k < n_keys; k++) {
                        int8_t t = key_types[k];
                        uint64_t kh;
                        bool is_null = reprobe_nullable_s && ray_key_may_be_null(key_vecs[k])
                                       && ray_vec_is_null(key_vecs[k], row);
                        if (is_null) {
                            nullw[k >> 6] |= (int64_t)((uint64_t)1 << (k & 63));
                            ek_buf[k] = 0;
                            kh = ray_hash_i64(0);
                        } else if (ly->key_flags[k] & GHT_KEYF_WIDE) {
                            ek_buf[k] = row;
                            kh = wide_key_hash_at(ly, k, key_data, reprobe_pool, row);
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
                    h = ght_hash_null_words(h, nullw, null_words);
                    lookup_keys = ek_buf;
                    }
                    uint32_t gid = group_ht_lookup_gid(final_ht, h, lookup_keys, key_types);
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
                        if (!match_idx && !group_rowsel_pass(rowsel, row)) continue;
                        int64_t gi = row_gid[row];
                        if (gi >= 0) idx_buf_s[pos_s[gi]++] = row;
                    }
                    for (uint32_t a = 0; a < n_aggs; a++) {
                        if (!(ly->agg_flags[a] & GHT_AF_HOLISTIC)) continue;
                        if (!agg_vecs[a]) continue;
                        uint16_t aop = ext->agg_ops[a];
                        ray_t* hol_vec = NULL;
                        if (aop == OP_MEDIAN) {
                            hol_vec = ray_median_per_group_buf(
                                agg_vecs[a], idx_buf_s, offsets_s, grp_cnt_s,
                                (int64_t)grp_count);
                        } else if (aop == OP_QUANTILE) {
                            double q = ext->agg_k
                                ? decode_agg_f64_param(ext->agg_k[a]) : 0.5;
                            hol_vec = ray_quantile_per_group_buf(
                                agg_vecs[a], idx_buf_s, offsets_s, grp_cnt_s,
                                (int64_t)grp_count, q);
                        } else if (aop == OP_MODE) {
                            hol_vec = ray_mode_per_group_buf(
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
                        } else if (aop == OP_MIN || aop == OP_MAX ||
                                   aop == OP_FIRST || aop == OP_LAST) {
                            hol_vec = ray_wide_minmax_per_group_buf(
                                agg_vecs[a], aop, idx_buf_s, offsets_s,
                                grp_cnt_s, (int64_t)grp_count);
                        }
                        med_out[a] = hol_vec;  /* NULL or RAY_IS_ERR handled below */
                    }
                    scratch_free(ix_hdr_s);
                }
                scratch_free(hol_stage_hdr);   /* NULL (inline staging) → no-op */
            }
            scratch_free(rg_hdr);
            scratch_free(cnt_hdr_s);
            scratch_free(off_hdr_s);
            scratch_free(pos_hdr_s);
        }
    }

    /* Agg columns from inline accumulators */
    for (uint32_t a = 0; a < n_aggs; a++) {
        uint16_t agg_op = ext->agg_ops[a];
        ray_t* agg_col = agg_vecs[a];
        bool is_f64 = agg_col && agg_col->type == RAY_F64;
        int8_t out_type;
        switch (agg_op) {
            case OP_AVG:
            case OP_STDDEV: case OP_STDDEV_POP:
            case OP_VAR: case OP_VAR_POP:
            case OP_PEARSON_CORR:
            case OP_COV: case OP_SCOV:
            case OP_WSUM: case OP_WAVG:
            case OP_MEDIAN:
            case OP_QUANTILE:
                out_type = RAY_F64; break;
            case OP_ALL:
            case OP_ANY:
                out_type = RAY_BOOL; break;
            case OP_COUNT: out_type = RAY_I64; break;
            case OP_SUM: {
                /* sum preserves TIME (a duration-like temporal): time+time is
                 * a time, matching the scalar ray_sum_fn.  Other integer
                 * families widen to I64; DATE/TIMESTAMP are rejected at
                 * type-admission so never reach here.  The affine/linear SUM
                 * fast paths leave agg_col NULL (they aggregate without
                 * materializing the input vector), so recover the source type
                 * from the aggregation input op when the vector is absent. */
                int8_t src_t = agg_col ? agg_col->type
                             : (op_node(g, ext->agg_ins[a]) ? op_node(g, ext->agg_ins[a])->out_type : 0);
                out_type = is_f64 ? RAY_F64 : (src_t == RAY_TIME ? RAY_TIME : RAY_I64);
                break;
            }
            case OP_PROD:
                out_type = is_f64 ? RAY_F64 : RAY_I64; break;
            default:
                out_type = agg_col ? agg_col->type : RAY_I64; break;
        }
        ray_t* new_col;
        /* Drive off the layout bitmask, not the op literal: wide-element
         * (STR/GUID) min/max/first/last are holistic too and their column
         * lives in med_out[a], not the truncating row-layout read below. */
        bool is_holistic = (ly->agg_flags[a] & GHT_AF_HOLISTIC) != 0;
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
            /* SYM MIN/MAX/FIRST/LAST emit RAW cell ids accumulated from
             * agg_col — the output resolves over its dictionary. */
            if (out_type == RAY_SYM)
                ray_sym_vec_adopt_domain(new_col, sym_domain_rep(agg_col));
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
            /* nn = per-slot non-null count (nullable layout) or the group row
             * count (null-free — byte-identical to before). */
            int64_t nn = ly->off_nn
                ? ((const int64_t*)(const void*)(row + ly->off_nn))[s] : cnt;
            if (out_type == RAY_F64) {
                double v;
                switch (agg_op) {
                    case OP_SUM:
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_f64 * cnt;
                        break;
                    case OP_PROD:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                        break;
                    case OP_AVG:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s) / nn
                                   : (double)ROW_RD_I64(row, ly->off_sum, s) / nn;
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_f64;
                        break;
                    case OP_MIN:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                        v = is_f64 ? ROW_RD_F64(row, ly->off_min, s)
                                   : (double)ROW_RD_I64(row, ly->off_min, s);
                        break;
                    case OP_MAX:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                        v = is_f64 ? ROW_RD_F64(row, ly->off_max, s)
                                   : (double)ROW_RD_I64(row, ly->off_max, s);
                        break;
                    case OP_FIRST: case OP_LAST:
                        if (nn == 0) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                        v = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                   : (double)ROW_RD_I64(row, ly->off_sum, s);
                        break;
                    case OP_VAR: case OP_VAR_POP:
                    case OP_STDDEV: case OP_STDDEV_POP: {
                        bool insuf = (agg_op == OP_VAR || agg_op == OP_STDDEV) ? nn <= 1 : nn <= 0;
                        if (insuf) { v = NULL_F64; ray_vec_set_null(new_col, gi, true); break; }
                        double sum_val = is_f64 ? ROW_RD_F64(row, ly->off_sum, s)
                                                : (double)ROW_RD_I64(row, ly->off_sum, s);
                        double sq_val = ly->off_sumsq ? ROW_RD_F64(row, ly->off_sumsq, s) : 0.0;
                        double mean = sum_val / nn;
                        double var_pop = sq_val / nn - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        if (agg_op == OP_VAR_POP) v = var_pop;
                        else if (agg_op == OP_VAR) v = var_pop * nn / (nn - 1);
                        else if (agg_op == OP_STDDEV_POP) v = sqrt(var_pop);
                        else v = sqrt(var_pop * nn / (nn - 1));
                        break;
                    }
                    case OP_PEARSON_CORR:
                    case OP_COV:
                    case OP_SCOV:
                    case OP_WSUM:
                    case OP_WAVG: {
                        if ((agg_op == OP_PEARSON_CORR || agg_op == OP_SCOV) && cnt < 2) { v = 0.0; ray_vec_set_null(new_col, gi, true); break; }
                        if ((agg_op == OP_COV || agg_op == OP_WAVG) && cnt < 1) { v = 0.0; ray_vec_set_null(new_col, gi, true); break; }
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
                        if (agg_op == OP_WSUM) { v = sxy; break; }
                        if (agg_op == OP_WAVG) { if (sx == 0.0) { v = NULL_F64; break; } v = sxy / sx; break; }
                        if (agg_op == OP_COV) { v = (sxy - sx * sy / dn) / dn; break; }
                        if (agg_op == OP_SCOV) { v = (sxy - sx * sy / dn) / (dn - 1.0); break; }
                        if (dx <= 0.0 || dy <= 0.0) { v = NULL_F64; break; }
                        v = num / sqrt(dx * dy);
                        break;
                    }
                    default: v = 0.0; break;
                }
                /* Single-null float model: canonicalize non-finite computed
                 * aggregate to NULL_F64 (HAS_NULLS via the scan added below). */
                ((double*)ray_data(new_col))[gi] = ray_f64_fin(v);
            } else {
                int64_t v;
                /* All-null group emits the width-correct sentinel; SUM/COUNT
                 * are never nulled. */
                int64_t int_null = agg_int_null_sentinel_for(out_type);
                switch (agg_op) {
                    case OP_SUM:
                        v = ROW_RD_I64(row, ly->off_sum, s);
                        if (agg_affine[a].enabled) v += agg_affine[a].bias_i64 * cnt;
                        break;
                    case OP_PROD:
                        if (nn == 0) { v = int_null; ray_vec_set_null(new_col, gi, true); break; }
                        v = ROW_RD_I64(row, ly->off_sum, s); break;
                    case OP_COUNT: v = cnt; break;
                    case OP_ALL:   v = (ROW_RD_I64(row, ly->off_sum, s) == nn) ? 1 : 0; break;
                    case OP_ANY:   v = (ROW_RD_I64(row, ly->off_sum, s) > 0) ? 1 : 0; break;
                    case OP_MIN:
                        if (nn == 0) { v = int_null; ray_vec_set_null(new_col, gi, true); break; }
                        v = ROW_RD_I64(row, ly->off_min, s); break;
                    case OP_MAX:
                        if (nn == 0) { v = int_null; ray_vec_set_null(new_col, gi, true); break; }
                        v = ROW_RD_I64(row, ly->off_max, s); break;
                    case OP_FIRST: case OP_LAST:
                        if (nn == 0) { v = int_null; ray_vec_set_null(new_col, gi, true); break; }
                        v = ROW_RD_I64(row, ly->off_sum, s); break;
                    default:       v = 0; break;
                }
                /* Narrow-int / adaptive-width SYM store: see the matching
                 * comment in the DA emit path above. A fixed int64 store
                 * corrupts groups past index 0 for I32/I16/U8 out_types. */
                topk_write_i64(ray_data(new_col), gi, v,
                               ray_sym_elem_size(out_type, new_col->attrs));
            }
        }

    med_attach:;
        /* Generate unique column name */
        ray_op_ext_t* agg_ext = find_ext(g, ext->agg_ins[a]);
        int64_t name_id;
        if (agg_ext && agg_ext->base.opcode == OP_SCAN) {
            /* Shared with exec_group_v2 (agg_engine.c) — parity by
             * construction.  Same input-name + per-op-suffix logic, same
             * 256-byte buffer overflow fallback to the input sym. */
            name_id = agg_result_col_name(agg_ext->sym, agg_op);
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
                case OP_ALL:   nsfx = "_all";   nslen = 4; break;
                case OP_ANY:   nsfx = "_any";   nslen = 4; break;
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
                case OP_QUANTILE:   nsfx = "_quantile";   nslen = 9; break;
                case OP_MODE:       nsfx = "_mode";       nslen = 5; break;
                case OP_COV:        nsfx = "_cov";        nslen = 4; break;
                case OP_SCOV:       nsfx = "_scov";       nslen = 5; break;
                case OP_WSUM:       nsfx = "_wsum";       nslen = 5; break;
                case OP_WAVG:       nsfx = "_wavg";       nslen = 5; break;
                case OP_TOP_N:      nsfx = "_top";        nslen = 4; break;
                case OP_BOT_N:      nsfx = "_bot";        nslen = 4; break;
            }
            memcpy(nbuf + np, nsfx, nslen);
            name_id = ray_sym_intern(nbuf, (size_t)np + nslen);
        }
        /* Single-null float model: flip HAS_NULLS if a non-finite F64 aggregate
         * was canonicalized to NULL_F64 above without an explicit set_null. */
        if (new_col->type == RAY_F64) grp_finalize_nulls(new_col);
        result = ray_table_add_col(result, name_id, new_col);
        ray_release(new_col);
    }
    if (med_out) {
        for (uint32_t a = 0; a < n_aggs; a++)
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
    /* Master layout owns any spill block; every by-value copy borrowed it.
     * cleanup: is reached only after ght_layout is initialised (all gotos to
     * it are below the ght_compute_layout call).  NULL-safe / no-op inline. */
    ght_layout_free(&ght_layout);
    for (uint32_t a = 0; a < n_aggs; a++)
        { if (agg_owned[a] && agg_vecs[a]) ray_release(agg_vecs[a]); if (agg_owned2[a] && agg_vecs2[a]) ray_release(agg_vecs2[a]); }
    for (uint32_t k = 0; k < n_keys; k++)
        if (key_owned[k] && key_vecs[k]) ray_release(key_vecs[k]);
    if (match_idx_block) { ray_release(match_idx_block); } scratch_free(vla_hdr);

    /* No explicit GC — top-level statement runner (run_piped / repl)
     * calls ray_heap_gc() once per statement, catching every
     * intermediate freed above.  The duplicate inner call doubled the
     * per-query GC cost on bench loops. */

    return result;
}

/* Smallest index i in a[0..n) with a[i] >= key (lower_bound over a SORTED
 * ascending int64 array).  Used to slice the flattened global selection ids
 * into per-partition ranges. */
static inline int64_t grp_sorted_lower_bound(const int64_t* a, int64_t n,
                                             int64_t key) {
    int64_t lo = 0, hi = n;
    while (lo < hi) {
        int64_t mid = lo + ((hi - lo) >> 1);
        if (a[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
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
 * Pushed-down WHERE (g->selection set): the global selection covers the parted
 * table's concatenated row space in segment order.  It is flattened once to a
 * sorted global-id array, g->selection is cleared for the duration (so the
 * per-partition key/agg materialization runs unfiltered), and each partition's
 * range [off_p, off_p+len_p) is binary-searched out, rebased to partition-local
 * ids, and installed as that sub-group's own local selection.  The original
 * g->selection is restored before returning so the concat fallback still sees
 * it when this path declines.
 *
 * Returns NULL if any step fails (caller falls through to concat path).
 * -------------------------------------------------------------------------- */
static ray_t* __attribute__((noinline))
exec_group_per_partition(ray_graph_t* g, ray_t* parted_tbl, ray_op_ext_t* ext,
                         int32_t n_parts, const int64_t* key_syms,
                         const int64_t* agg_syms, const uint8_t* key_is_expr,
                         const uint8_t* agg_is_expr, const int64_t* borrow_syms,
                         int n_borrow, int has_avg, int has_stddev,
                         int64_t group_limit) {

    /* Count every entry into the streaming per-partition kernel (O(1), see the
     * ray_group_perpart_runs_ctr definition near the top of this file). */
    atomic_fetch_add_explicit(&ray_group_perpart_runs_ctr, 1,
                              memory_order_relaxed);

    uint32_t n_keys = ext->n_keys;
    uint32_t n_aggs = ext->n_aggs;

    /* Unbounded-slots: this per-partition optimizer sizes its key/agg scratch
     * from the query's true n_keys/n_aggs (was capped at [8]/[24]).  A pre-pass
     * counts the AVG/STDDEV/VAR decompositions so the appended-slot arrays are
     * sized exactly: each AVG adds 1 COUNT slot, each STDDEV/VAR adds 2 slots
     * (SUM(x²) + COUNT).  All carves are freed via the pp_cleanup epilogue on
     * every return path. */
    uint32_t n_avg = 0, n_std = 0;
    for (uint32_t a = 0; a < n_aggs; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop == OP_AVG) n_avg++;
        else if (aop == OP_STDDEV || aop == OP_STDDEV_POP ||
                 aop == OP_VAR || aop == OP_VAR_POP) n_std++;
    }
    uint32_t part_n_aggs_max = n_aggs + n_avg + 2 * n_std;

    size_t key_n = n_keys ? n_keys : 1;             /* min-1 guarded */
    size_t agg_n = n_aggs ? n_aggs : 1;
    size_t pna_n = part_n_aggs_max ? part_n_aggs_max : 1;

    ray_t* ret = NULL;   /* pp_cleanup return value (NULL on any early bail) */

    /* Pushed-down WHERE state (Part A).  saved_sel is captured now and
     * g->selection cleared later (in the setup block below) so the setup's
     * failures before that point leave g->selection untouched; pp_cleanup
     * always restores it.  All carves/blocks freed in pp_cleanup. */
    ray_t*   saved_sel     = g->selection;
    ray_t*   sel_ids_block = NULL;
    const int64_t* sel_ids = NULL;
    int64_t  sel_total     = 0;
    ray_t*   part_off_hdr  = NULL;
    int64_t* part_off      = NULL;   /* [n_parts+1] global prefix-sum offsets */
    ray_t*   local_ids_hdr = NULL;
    int64_t* local_ids     = NULL;   /* [max_part_len] rebased-id scratch     */

    /* key-scoped carves (sized n_keys) */
    ray_t *mc_sym_ids_hdr = NULL, *pk_syms_hdr = NULL,
          *pkeys_hdr = NULL, *mkeys_hdr = NULL;
    int64_t*   mc_sym_ids = scratch_alloc(&mc_sym_ids_hdr, key_n * sizeof(int64_t));
    int64_t*   pk_syms    = scratch_alloc(&pk_syms_hdr,    key_n * sizeof(int64_t));
    ray_op_t** pkeys      = scratch_alloc(&pkeys_hdr,      key_n * sizeof(ray_op_t*));
    ray_op_t** mkeys      = scratch_alloc(&mkeys_hdr,      key_n * sizeof(ray_op_t*));

    /* per-agg decomposition carves (sized n_aggs) */
    ray_t *avg_idx_hdr = NULL, *std_idx_hdr = NULL, *std_orig_op_hdr = NULL,
          *std_sq_slot_hdr = NULL, *std_cnt_slot_hdr = NULL;
    uint8_t*  avg_idx      = scratch_alloc(&avg_idx_hdr,      agg_n * sizeof(uint8_t));
    uint8_t*  std_idx      = scratch_alloc(&std_idx_hdr,      agg_n * sizeof(uint8_t));
    uint16_t* std_orig_op  = scratch_alloc(&std_orig_op_hdr,  agg_n * sizeof(uint16_t));
    uint8_t*  std_sq_slot  = scratch_alloc(&std_sq_slot_hdr,  agg_n * sizeof(uint8_t));
    uint8_t*  std_cnt_slot = scratch_alloc(&std_cnt_slot_hdr, agg_n * sizeof(uint8_t));

    /* decomposed per-partition/merge slot carves (sized part_n_aggs_max) */
    ray_t *part_ops_hdr = NULL, *merge_ops_hdr = NULL, *part_agg_syms_hdr = NULL,
          *part_needs_sq_hdr = NULL, *agg_name_ids_hdr = NULL, *pagg_ins_hdr = NULL,
          *magg_ins_hdr = NULL, *unique_agg_hdr = NULL;
    uint16_t* part_ops      = scratch_alloc(&part_ops_hdr,      pna_n * sizeof(uint16_t));
    uint16_t* merge_ops     = scratch_alloc(&merge_ops_hdr,     pna_n * sizeof(uint16_t));
    int64_t*  part_agg_syms = scratch_alloc(&part_agg_syms_hdr, pna_n * sizeof(int64_t));
    int*      part_needs_sq = scratch_alloc(&part_needs_sq_hdr, pna_n * sizeof(int));
    int64_t*  agg_name_ids  = scratch_alloc(&agg_name_ids_hdr,  pna_n * sizeof(int64_t));
    ray_op_t** pagg_ins     = scratch_alloc(&pagg_ins_hdr,      pna_n * sizeof(ray_op_t*));
    ray_op_t** magg_ins     = scratch_alloc(&magg_ins_hdr,      pna_n * sizeof(ray_op_t*));
    int64_t*  unique_agg    = scratch_alloc(&unique_agg_hdr,    pna_n * sizeof(int64_t));

    if (!mc_sym_ids || !pk_syms || !pkeys || !mkeys || !avg_idx || !std_idx ||
        !std_orig_op || !std_sq_slot || !std_cnt_slot || !part_ops ||
        !merge_ops || !part_agg_syms || !part_needs_sq || !agg_name_ids ||
        !pagg_ins || !magg_ins || !unique_agg)
        goto pp_cleanup;

    /* Identify MAPCOMMON vs PARTED keys.  MAPCOMMON keys are constant
     * within a partition, so they are excluded from per-partition GROUP BY
     * and reconstructed after concat. */
    uint32_t n_mc_keys = 0;
    uint32_t n_part_keys = 0;

    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* pcol = ray_table_get_col(parted_tbl, key_syms[k]);
        if (pcol && pcol->type == RAY_MAPCOMMON) {
            mc_sym_ids[n_mc_keys++] = key_syms[k];
        } else {
            pk_syms[n_part_keys++] = key_syms[k];
        }
    }

    /* LIMIT pushdown: when all GROUP BY keys are MAPCOMMON (n_part_keys==0),
     * each partition produces exactly 1 group.  Limit the partition loop.
     * Disabled under a pushed-down WHERE: a filtered partition may yield 0
     * groups (all rows filtered out), breaking the "1 group per partition"
     * assumption the limit relies on — the outer take/limit truncates the
     * merged result instead. */
    if (group_limit > 0 && n_part_keys == 0 && group_limit < n_parts && !saved_sel)
        n_parts = (int32_t)group_limit;

    /* Pushed-down WHERE setup (Part A): flatten the global selection once,
     * clear g->selection for the materialization phase, and precompute the
     * per-partition global row offsets (prefix sum of segment lengths, which
     * every column shares).  Any failure here routes through pp_cleanup →
     * NULL → the caller's concat fallback (which still has g->selection). */
    if (saved_sel) {
        ray_rowsel_t* sm = ray_rowsel_meta(saved_sel);
        sel_total     = sm->total_pass;
        sel_ids_block = ray_rowsel_to_indices(saved_sel);
        if (!sel_ids_block) goto pp_cleanup;   /* OOM → concat fallback */
        sel_ids = (const int64_t*)ray_data(sel_ids_block);
        g->selection = NULL;                   /* restored in pp_cleanup */

        part_off = scratch_alloc(&part_off_hdr,
                                 (size_t)(n_parts + 1) * sizeof(int64_t));
        if (!part_off) goto pp_cleanup;

        /* Reference column for per-partition lengths: any PARTED column (seg
         * len) or MAPCOMMON (counts[p]).  All columns share partition
         * boundaries, so any one is authoritative. */
        int64_t pncols = ray_table_ncols(parted_tbl);
        ray_t*  ref    = NULL;
        for (int64_t c = 0; c < pncols; c++) {
            ray_t* col = ray_table_get_col_idx(parted_tbl, c);
            if (col && (RAY_IS_PARTED(col->type) ||
                        col->type == RAY_MAPCOMMON)) { ref = col; break; }
        }
        if (!ref) goto pp_cleanup;
        const int64_t* mc_counts = (ref->type == RAY_MAPCOMMON)
            ? (const int64_t*)ray_data(((ray_t**)ray_data(ref))[1]) : NULL;
        ray_t** ref_segs = (ref->type == RAY_MAPCOMMON)
            ? NULL : (ray_t**)ray_data(ref);
        int64_t cum = 0, max_len = 0;
        for (int32_t p = 0; p < n_parts; p++) {
            part_off[p] = cum;
            int64_t len_p = mc_counts ? mc_counts[p]
                          : (ref_segs[p] ? ref_segs[p]->len : 0);
            cum += len_p;
            if (len_p > max_len) max_len = len_p;
        }
        part_off[n_parts] = cum;

        local_ids = scratch_alloc(&local_ids_hdr,
                                  (size_t)(max_len > 0 ? max_len : 1) * sizeof(int64_t));
        if (!local_ids) goto pp_cleanup;
        if (ray_interrupted()) { ret = ray_error("cancel", NULL); goto pp_cleanup; }
    }

    /* Decomposition: AVG(x) → SUM(x) + COUNT(x).
     * STDDEV/VAR(x) → SUM(x) + SUM(x²) + COUNT(x).
     * Build per-partition agg_ops with decomposed ops, then merge ops.
     * part_ops/merge_ops/avg_idx/std_idx/std_orig_op/std_sq_slot/std_cnt_slot
     * are carved above; n_avg/n_std are recounted here to fill them. */
    n_avg = 0;
    n_std = 0;
    uint32_t part_n_aggs = n_aggs;

    for (uint32_t a = 0; a < n_aggs; a++) {
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
    /* Append SUM(x²) for each STDDEV/VAR slot. */
    for (uint32_t i = 0; i < n_std; i++) {
        std_sq_slot[i] = part_n_aggs;
        part_ops[part_n_aggs++] = OP_SUM;  /* SUM(x²) */
    }
    /* Append COUNT for each AVG column */
    for (uint32_t i = 0; i < n_avg; i++)
        part_ops[part_n_aggs++] = OP_COUNT;
    /* Append COUNT for each STDDEV/VAR column */
    for (uint32_t i = 0; i < n_std; i++) {
        std_cnt_slot[i] = part_n_aggs;
        part_ops[part_n_aggs++] = OP_COUNT;
    }

    /* Merge ops: SUM→SUM, COUNT→SUM, MIN→MIN, MAX→MAX,
     * FIRST→FIRST, LAST→LAST, all appended slots → SUM */
    for (uint32_t a = 0; a < part_n_aggs; a++) {
        merge_ops[a] = part_ops[a];
        if (merge_ops[a] == OP_COUNT) merge_ops[a] = OP_SUM;
    }

    /* Agg input syms for the decomposed ops.
     * AVG's COUNT uses same input column as the AVG itself.
     * STDDEV's SUM(x²) and COUNT use same input column as the STDDEV. */
    /* part_agg_syms/part_needs_sq carved above (sized part_n_aggs_max).
     * part_needs_sq flags slots that need an x*x graph node (for SUM(x²)). */
    memset(part_needs_sq, 0, pna_n * sizeof(int));

    for (uint32_t a = 0; a < n_aggs; a++)
        part_agg_syms[a] = agg_syms[a];
    /* SUM(x²) slots for STDDEV/VAR */
    for (uint32_t i = 0; i < n_std; i++) {
        part_agg_syms[std_sq_slot[i]] = agg_syms[std_idx[i]];
        part_needs_sq[std_sq_slot[i]] = 1;
    }
    /* COUNT slots for AVG */
    for (uint32_t i = 0; i < n_avg; i++)
        part_agg_syms[n_aggs + n_std + i] = agg_syms[avg_idx[i]];
    /* COUNT slots for STDDEV/VAR */
    for (uint32_t i = 0; i < n_std; i++)
        part_agg_syms[std_cnt_slot[i]] = agg_syms[std_idx[i]];

    /* ---- Batched incremental merge ----
     * Process partitions in batches of MERGE_BATCH.  After each batch:
     *   Pass 1: exec_group each partition in batch → batch_partials[]
     *   Pass 2: concat (running + batch_partials + MAPCOMMON) → merge_tbl
     *   Pass 3: merge GROUP BY → new running
     * Bounds peak memory to O(MERGE_BATCH × groups_per_partition). */
#define MERGE_BATCH 8

    /* Capture agg column name IDs from first partition result
     * (agg_name_ids carved above, sized part_n_aggs_max). */
    int agg_names_captured = 0;

    ray_t* running = NULL;
    ray_t* merge_tbl = NULL;      /* last merge table (for column name fixup) */

    for (int32_t batch_start = 0; batch_start < n_parts;
         batch_start += MERGE_BATCH) {

        int32_t batch_end = batch_start + MERGE_BATCH;
        if (batch_end > n_parts) batch_end = n_parts;
        int32_t batch_n = batch_end - batch_start;

        /* Pass 1: exec_group each partition in this batch */
        ray_t* bp[MERGE_BATCH];
        memset(bp, 0, sizeof(bp));
        /* First BUILT partial in this batch — the schema/type reference used
         * below when there is no `running` yet (first batch).  Zero-survivor
         * partitions may be skipped (bp[bi] left NULL), but the reference
         * partition is always built, so ref_bi stays valid. */
        int32_t ref_bi = -1;

        for (int32_t bi = 0; bi < batch_n; bi++) {
            int32_t p = batch_start + bi;

            /* Poll cancellation once per partition (in-memory parity: abort
             * within one partition, not one whole sweep) and tick the sweep
             * meter so a single progress bar advances 0→n_parts across all
             * partitions.  batch_fail frees every live intermediate
             * (bp[] partials, running, merge_tbl) — mirror the error path. */
            if (ray_interrupted()) {
                ret = ray_error("cancel", NULL);
                goto batch_fail;
            }
            ray_progress_update("group", "per-partition aggregate",
                                (uint64_t)p, (uint64_t)n_parts);

            /* Pushed-down WHERE: slice this partition's surviving global row
             * ids out of the sorted flattened selection.  A partition with
             * zero survivors contributes no groups — skip its sub-table build
             * and group execution entirely (bp[bi] stays NULL; the merge
             * concat loop is NULL-safe), provided a schema reference already
             * exists (a prior `running` result or an earlier built partial in
             * this batch).  The reference partition is never skipped, so the
             * bp[ref_bi] type lookups below always resolve. */
            int64_t sel_off = 0, sel_len = 0, sel_lo = 0, sel_cnt = -1;
            if (saved_sel) {
                sel_off = part_off[p];
                sel_len = part_off[p + 1] - sel_off;
                sel_lo  = grp_sorted_lower_bound(sel_ids, sel_total, sel_off);
                int64_t sel_hi = grp_sorted_lower_bound(sel_ids, sel_total,
                                                        sel_off + sel_len);
                sel_cnt = sel_hi - sel_lo;
                if (sel_cnt == 0 && (running || ref_bi >= 0)) {
                    bp[bi] = NULL;
                    continue;
                }
            }

            /* Build the per-partition sub-table.  It carries (a) the borrow
             * set — the deduped plain PARTED source columns every scan key,
             * scan agg input, AND expression source references (computed once
             * by exec_group_parted); plus (b) one materialized column per
             * expression key / agg input, added under its synthetic sym.  The
             * pkeys/pagg_ins scans below then always see plain columns — a
             * scan slot by its real sym, an expression slot by its synthetic
             * sym — so ALL downstream per-partition/merge machinery is
             * unchanged.  (unique_agg's key/agg dedup is subsumed by
             * borrow_syms, which is already deduplicated.) */
            (void)unique_agg;
            ray_t* sub = ray_table_new((int64_t)(n_borrow + n_keys + n_aggs));
            if (!sub || RAY_IS_ERR(sub)) goto batch_fail;

            int64_t part_nrows = -1;
            for (int j = 0; j < n_borrow; j++) {
                ray_t* pcol = ray_table_get_col(parted_tbl, borrow_syms[j]);
                if (!pcol || !RAY_IS_PARTED(pcol->type)) {
                    ray_release(sub); goto batch_fail;
                }
                ray_t* seg = ((ray_t**)ray_data(pcol))[p];
                if (!seg) { ray_release(sub); goto batch_fail; }
                if (part_nrows < 0) part_nrows = seg->len;
                ray_retain(seg);
                sub = ray_table_add_col(sub, borrow_syms[j], seg);
                ray_release(seg);
            }

            /* Materialize expression keys / agg inputs over this partition.
             * exec_node resolves the subtree's scans against `sub` (g->table
             * swap); g->selection is NULL on this path.  Each result must be a
             * full-partition-length vector — a length mismatch (e.g. a scan
             * bound to a different table) fails the whole per-partition attempt
             * cleanly, falling back to concat. */
            for (uint32_t k = 0; k < n_keys; k++) {
                if (!key_is_expr[k]) continue;
                ray_t* saved = g->table;
                g->table = sub;
                ray_t* v = exec_node(g, op_node(g, ext->keys[k]));
                g->table = saved;
                if (!v || RAY_IS_ERR(v) || !ray_is_vec(v) ||
                    (part_nrows >= 0 && v->len != part_nrows)) {
                    if (v && !RAY_IS_ERR(v)) ray_release(v);
                    ray_release(sub); goto batch_fail;
                }
                sub = ray_table_add_col(sub, key_syms[k], v);
                ray_release(v);
            }
            for (uint32_t a = 0; a < n_aggs; a++) {
                if (!agg_is_expr[a]) continue;
                ray_t* saved = g->table;
                g->table = sub;
                ray_t* v = exec_node(g, op_node(g, ext->agg_ins[a]));
                g->table = saved;
                if (!v || RAY_IS_ERR(v) || !ray_is_vec(v) ||
                    (part_nrows >= 0 && v->len != part_nrows)) {
                    if (v && !RAY_IS_ERR(v)) ray_release(v);
                    ray_release(sub); goto batch_fail;
                }
                sub = ray_table_add_col(sub, agg_syms[a], v);
                ray_release(v);
            }

            ray_graph_t* pg = ray_graph_new(sub);
            if (!pg) { ray_release(sub); goto batch_fail; }

            for (uint32_t k = 0; k < n_part_keys; k++) {
                ray_t* sym_atom = ray_sym_str(pk_syms[k]);
                pkeys[k] = ray_scan(pg, ray_str_ptr(sym_atom));
            }
            for (uint32_t a = 0; a < part_n_aggs; a++) {
                ray_t* sym_atom = ray_sym_str(part_agg_syms[a]);
                pagg_ins[a] = ray_scan(pg, ray_str_ptr(sym_atom));
            }
            for (uint32_t j = 0; j < n_std; j++) {
                uint8_t sq = std_sq_slot[j];
                ray_op_t* x = pagg_ins[sq];
                /* STDDEV/VAR is inherently F64 (mean, sqrt).  Cast input to
                 * F64 before squaring so SUM(x²) is F64 across partitions —
                 * readout below assumes F64 sumsq.  Also avoids I64 overflow
                 * for large x (matters near INT_MAX). */
                ray_op_t* xf = (x->out_type == RAY_F64) ? x : ray_cast(pg, x, RAY_F64);
                pagg_ins[sq] = ray_mul(pg, xf, xf);
            }

            ray_op_t* proot = ray_group(pg, pkeys, n_part_keys,
                                       part_ops, pagg_ins, part_n_aggs);
            proot = ray_optimize(pg, proot);

            /* Pushed-down WHERE: translate this partition's global survivor
             * ids to partition-LOCAL ids (rebased by sel_off) and install them
             * as pg's own selection.  Its nrows MUST equal the sub-table row
             * count (exec_group_run's selection-shape guard rejects a mismatch
             * — fail-loud); part_nrows (the borrow segment length) equals
             * sel_len (the reference column's segment length) by the parted
             * partition invariant, so build the local rowsel over sel_len.
             * ray_graph_free(pg) releases pg->selection. */
            if (saved_sel) {
                if (part_nrows >= 0 && part_nrows != sel_len) {
                    ray_graph_free(pg); ray_release(sub); goto batch_fail;
                }
                for (int64_t j = 0; j < sel_cnt; j++)
                    local_ids[j] = sel_ids[sel_lo + j] - sel_off;
                ray_t* lsel = ray_index_rowsel_from_ids(sel_len, local_ids,
                                                        sel_cnt);
                if (!lsel) { ray_graph_free(pg); ray_release(sub); goto batch_fail; }
                pg->selection = lsel;
            }

            bp[bi] = ray_execute(pg, proot);
            ray_graph_free(pg);
            ray_release(sub);

            if (!bp[bi] || RAY_IS_ERR(bp[bi])) goto batch_fail;
            if (ref_bi < 0) ref_bi = bi;   /* first built partial in this batch */

            /* Capture agg column name IDs once (all partials share names) */
            if (!agg_names_captured) {
                for (uint32_t a = 0; a < part_n_aggs; a++)
                    agg_name_ids[a] = ray_table_col_name(
                        bp[bi], (int64_t)n_part_keys + a);
                agg_names_captured = 1;
            }
        }

        /* Pass 2: concat (running + batch_partials + MAPCOMMON) */
        int64_t mrows = running ? ray_table_nrows(running) : 0;
        for (int32_t i = 0; i < batch_n; i++)
            mrows += ray_table_nrows(bp[i]);

        if (merge_tbl) { ray_release(merge_tbl); merge_tbl = NULL; }
        merge_tbl = ray_table_new((int64_t)(n_keys + part_n_aggs));
        if (!merge_tbl || RAY_IS_ERR(merge_tbl)) {
            merge_tbl = NULL; goto batch_fail;
        }

        /* Key columns */
        for (uint32_t k = 0; k < n_keys; k++) {
            /* Poll cancellation before allocating this column's flat vec
             * (each concat copy is O(total merged groups)).  No live `flat`
             * yet, so batch_fail's bp[]/running/merge_tbl release is complete. */
            if (ray_interrupted()) {
                ret = ray_error("cancel", NULL);
                goto batch_fail;
            }
            int is_mc = 0;
            for (uint32_t m = 0; m < n_mc_keys; m++)
                if (mc_sym_ids[m] == key_syms[k]) { is_mc = 1; break; }

            /* Type reference for column allocation */
            ray_t* tref = NULL;
            if (running) {
                tref = ray_table_get_col(running, key_syms[k]);
            } else if (is_mc) {
                ray_t* mc_col = ray_table_get_col(parted_tbl, key_syms[k]);
                tref = ((ray_t**)ray_data(mc_col))[0];
            } else {
                /* ref_bi = first built partial (skipped zero-survivor
                 * partitions leave bp[bi] NULL); always >= 0 here. */
                tref = ray_table_get_col(bp[ref_bi >= 0 ? ref_bi : 0], key_syms[k]);
            }
            if (!tref) goto batch_fail;

            /* Per-partition group outputs keep their partition's SYM index
             * width, so partials in one batch can legitimately differ —
             * size the merged flat at the max width and convert per
             * source (mixed-width parted tables). */
            uint8_t out_attrs = tref->attrs;
            if (tref->type == RAY_SYM) {
                uint8_t w = tref->attrs & RAY_SYM_W_MASK;
                if (running) {
                    ray_t* rc = ray_table_get_col(running, key_syms[k]);
                    if (rc && rc->type == RAY_SYM &&
                        (rc->attrs & RAY_SYM_W_MASK) > w)
                        w = rc->attrs & RAY_SYM_W_MASK;
                }
                for (int32_t i = 0; i < batch_n; i++) {
                    ray_t* pc = is_mc ? NULL
                              : ray_table_get_col(bp[i], key_syms[k]);
                    if (pc && pc->type == RAY_SYM &&
                        (pc->attrs & RAY_SYM_W_MASK) > w)
                        w = pc->attrs & RAY_SYM_W_MASK;
                }
                out_attrs = w;
            }
            ray_t* flat = typed_vec_new(tref->type, out_attrs, mrows);
            if (!flat || RAY_IS_ERR(flat)) goto batch_fail;
            /* cell ids copied id-preserving from running/partial/MAPCOMMON
             * key vecs — all resolve over the same dictionary (PARTED
             * contract: one root symfile domain), represented by tref. */
            if (flat->type == RAY_SYM)
                ray_sym_vec_adopt_domain(flat, sym_domain_rep(tref));
            flat->len = mrows;
            char* out = (char*)ray_data(flat);
            int64_t off = 0;

            uint8_t src_nulls = 0;

            /* Copy from running result */
            if (running) {
                ray_t* rc = ray_table_get_col(running, key_syms[k]);
                if (rc && rc->len > 0) {
                    parted_copy_cells(out, flat->type, out_attrs, 0,
                                      rc, 0, rc->len);
                    src_nulls |= rc->attrs & RAY_ATTR_HAS_NULLS;
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
                    for (int64_t r = 0; r < pnrows; r++) {
                        /* O(groups) replicate: stride-poll cancellation.
                         * `flat` is live and not yet added to merge_tbl —
                         * release it before routing through batch_fail. */
                        if ((r & 0xFFFF) == 0 && ray_interrupted()) {
                            ray_release(flat);
                            ret = ray_error("cancel", NULL);
                            goto batch_fail;
                        }
                        parted_copy_cells(out, flat->type, out_attrs,
                                          off + r, mc_kv, p, 1);
                    }
                    src_nulls |= mc_kv->attrs & RAY_ATTR_HAS_NULLS;
                    off += pnrows;
                } else {
                    ray_t* pc = ray_table_get_col(bp[i], key_syms[k]);
                    if (pc && pc->len > 0) {
                        parted_copy_cells(out, flat->type, out_attrs, off,
                                          pc, 0, pc->len);
                        src_nulls |= pc->attrs & RAY_ATTR_HAS_NULLS;
                        off += pc->len;
                    }
                }
            }

            /* Null propagation: propagate HAS_NULLS from copied sources. */
            flat->attrs |= src_nulls;
            merge_tbl = ray_table_add_col(merge_tbl, key_syms[k], flat);
            ray_release(flat);
        }

        /* Agg columns */
        for (uint32_t a = 0; a < part_n_aggs; a++) {
            /* Poll cancellation before allocating this column's flat vec
             * (concat copy is O(total merged groups)); no live `flat` yet. */
            if (ray_interrupted()) {
                ret = ray_error("cancel", NULL);
                goto batch_fail;
            }
            ray_t* tref = running
                ? ray_table_get_col_idx(running, (int64_t)n_keys + a)
                : ray_table_get_col_idx(bp[ref_bi >= 0 ? ref_bi : 0],
                                        (int64_t)n_part_keys + a);
            if (!tref) goto batch_fail;

            /* SYM partials (FIRST/LAST/MIN/MAX) keep their partition's
             * index width — size at max width, convert per source. */
            uint8_t out_attrs = tref->attrs;
            if (tref->type == RAY_SYM) {
                uint8_t w = tref->attrs & RAY_SYM_W_MASK;
                for (int32_t i = 0; i < batch_n; i++) {
                    ray_t* pc = ray_table_get_col_idx(bp[i],
                                                     (int64_t)n_part_keys + a);
                    if (pc && pc->type == RAY_SYM &&
                        (pc->attrs & RAY_SYM_W_MASK) > w)
                        w = pc->attrs & RAY_SYM_W_MASK;
                }
                out_attrs = w;
            }
            ray_t* flat = typed_vec_new(tref->type, out_attrs, mrows);
            if (!flat || RAY_IS_ERR(flat)) goto batch_fail;
            /* SYM FIRST/LAST/MIN/MAX partials carry raw cell ids — same
             * single-dictionary contract as the key flats above. */
            if (flat->type == RAY_SYM)
                ray_sym_vec_adopt_domain(flat, sym_domain_rep(tref));
            flat->len = mrows;
            char* out = (char*)ray_data(flat);
            int64_t off = 0;
            uint8_t src_nulls = 0;

            if (running) {
                ray_t* rc = ray_table_get_col_idx(running, (int64_t)n_keys + a);
                if (rc && rc->len > 0) {
                    parted_copy_cells(out, flat->type, out_attrs, 0,
                                      rc, 0, rc->len);
                    src_nulls |= rc->attrs & RAY_ATTR_HAS_NULLS;
                    off = rc->len;
                }
            }

            for (int32_t i = 0; i < batch_n; i++) {
                ray_t* pc = ray_table_get_col_idx(bp[i],
                                                 (int64_t)n_part_keys + a);
                if (pc && pc->len > 0) {
                    parted_copy_cells(out, flat->type, out_attrs, off,
                                      pc, 0, pc->len);
                    src_nulls |= pc->attrs & RAY_ATTR_HAS_NULLS;
                    off += pc->len;
                }
            }

            /* Null propagation: propagate HAS_NULLS from copied sources. */
            flat->attrs |= src_nulls;
            merge_tbl = ray_table_add_col(merge_tbl, agg_name_ids[a], flat);
            ray_release(flat);
        }

        /* Free batch partials */
        for (int32_t i = 0; i < batch_n; i++) {
            ray_release(bp[i]);
            bp[i] = NULL;
        }

        /* Pass 3: merge GROUP BY */
        ray_graph_t* mg = ray_graph_new(merge_tbl);
        if (!mg) goto batch_fail;

        for (uint32_t k = 0; k < n_keys; k++) {
            ray_t* sym_atom = ray_sym_str(key_syms[k]);
            mkeys[k] = ray_scan(mg, ray_str_ptr(sym_atom));
        }

        for (uint32_t a = 0; a < part_n_aggs; a++) {
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
            goto pp_cleanup;
        }

        /* Rename running's agg columns back to the original partial names.
         * Without this, each merge adds an extra suffix (e.g. v1_sum → v1_sum_sum). */
        for (uint32_t a = 0; a < part_n_aggs; a++)
            ray_table_set_col_name(running, (int64_t)n_keys + a, agg_name_ids[a]);

        continue;

batch_fail:
        for (int32_t i = 0; i < batch_n; i++)
            if (bp[i]) ray_release(bp[i]);
        if (running) ray_release(running);
        if (merge_tbl) ray_release(merge_tbl);
        goto pp_cleanup;
    }

    /* Sweep complete: settle the meter at 100%. */
    ray_progress_update("group", "per-partition aggregate",
                        (uint64_t)n_parts, (uint64_t)n_parts);

    ray_t* result = running;

    if (!result || RAY_IS_ERR(result)) {
        if (merge_tbl) ray_release(merge_tbl);
        goto pp_cleanup;
    }

    int64_t rncols = ray_table_ncols(result);

    /* AVG/STDDEV post-processing: build trimmed table (n_keys + n_aggs cols),
     * computing final AVG = SUM/COUNT and STDDEV/VAR from SUM, SUM_SQ, COUNT. */
    if (has_avg || has_stddev) {
        ray_t* trimmed = ray_table_new((int64_t)(n_keys + n_aggs));
        if (!trimmed || RAY_IS_ERR(trimmed)) {
            ray_release(result);
            if (merge_tbl) ray_release(merge_tbl);
            goto pp_cleanup;
        }

        for (int64_t c = 0; c < (int64_t)(n_keys + n_aggs) && c < rncols; c++) {
            int64_t nm = ray_table_col_name(result, c);

            /* Check if this agg column is an AVG or STDDEV/VAR slot */
            int is_avg_slot = 0, is_std_slot = 0;
            uint8_t avg_i = 0, std_i = 0;
            if (c >= n_keys) {
                uint8_t a = (uint8_t)(c - n_keys);
                for (uint32_t j = 0; j < n_avg; j++) {
                    if (avg_idx[j] == a) { is_avg_slot = 1; avg_i = j; break; }
                }
                for (uint32_t j = 0; j < n_std; j++) {
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
                    goto pp_cleanup;
                }
                avg_col->len = nrows;

                double* out = (double*)ray_data(avg_col);
                if (sum_col->type == RAY_F64) {
                    const double* sv = (const double*)ray_data(sum_col);
                    const int64_t* cv = (const int64_t*)ray_data(cnt_col);
                    for (int64_t r = 0; r < nrows; r++)
                        out[r] = cv[r] > 0 ? ray_f64_fin(sv[r] / (double)cv[r]) : 0.0;
                } else {
                    const int64_t* sv = (const int64_t*)ray_data(sum_col);
                    const int64_t* cv = (const int64_t*)ray_data(cnt_col);
                    for (int64_t r = 0; r < nrows; r++)
                        out[r] = cv[r] > 0 ? ray_f64_fin((double)sv[r] / (double)cv[r]) : 0.0;
                }
                /* Single-null float model: flip HAS_NULLS for any avg
                 * canonicalized to NULL_F64 (overflow). */
                grp_finalize_nulls(avg_col);
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
                    goto pp_cleanup;
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
                        if (n <= 0) { out[r] = NULL_F64; ray_vec_set_null(out_col, r, true); continue; }
                        double mean = sv[r] / n;
                        double var_pop = sq[r] / n - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        bool insuf = (orig_op == OP_VAR || orig_op == OP_STDDEV) && n <= 1;
                        if (insuf) { out[r] = NULL_F64; ray_vec_set_null(out_col, r, true); continue; }
                        if (orig_op == OP_VAR_POP)         out[r] = ray_f64_fin(var_pop);
                        else if (orig_op == OP_VAR)         out[r] = ray_f64_fin(var_pop * n / (n - 1));
                        else if (orig_op == OP_STDDEV_POP)  out[r] = ray_f64_fin(sqrt(var_pop));
                        else /* OP_STDDEV */                out[r] = ray_f64_fin(sqrt(var_pop * n / (n - 1)));
                    }
                } else {
                    const int64_t* sv = (const int64_t*)ray_data(sum_col);
                    for (int64_t r = 0; r < nrows; r++) {
                        double n = (double)cv[r];
                        if (n <= 0) { out[r] = NULL_F64; ray_vec_set_null(out_col, r, true); continue; }
                        double mean = (double)sv[r] / n;
                        double var_pop = sq[r] / n - mean * mean;
                        if (var_pop < 0) var_pop = 0;
                        bool insuf = (orig_op == OP_VAR || orig_op == OP_STDDEV) && n <= 1;
                        if (insuf) { out[r] = NULL_F64; ray_vec_set_null(out_col, r, true); continue; }
                        if (orig_op == OP_VAR_POP)         out[r] = ray_f64_fin(var_pop);
                        else if (orig_op == OP_VAR)         out[r] = ray_f64_fin(var_pop * n / (n - 1));
                        else if (orig_op == OP_STDDEV_POP)  out[r] = ray_f64_fin(sqrt(var_pop));
                        else /* OP_STDDEV */                out[r] = ray_f64_fin(sqrt(var_pop * n / (n - 1)));
                    }
                }
                /* Single-null float model: flip HAS_NULLS for any var/stddev
                 * canonicalized to NULL_F64 (overflow). */
                grp_finalize_nulls(out_col);
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

    /* Key column names: the intermediate tables carry each key under its
     * runtime name (a real source sym for a scan key, or a synthetic "_gpk<k>"
     * for an expression key).  Rename the user-facing key columns to EXACTLY
     * what the flat exec_group emit produces — `find_ext(ext->keys[k])->sym`
     * (the source sym for scans; a non-scan node's ext sym, or the key index k
     * when it has no ext) — so the streaming result is name-for-name identical
     * to the concat fallback and the in-memory path (a select on a bare
     * computed by-key inherits this name directly). */
    for (uint32_t k = 0; k < n_keys && (int64_t)k < rncols; k++) {
        ray_op_ext_t* ke = find_ext(g, ext->keys[k]);
        ray_table_set_col_name(result, (int64_t)k, ke ? ke->sym : (int64_t)k);
    }

    /* Agg column names already fixed by ray_table_set_col_name inside batch loop.
     * Apply final name fixup for the user-facing n_aggs columns (trim decomposed extras). */
    for (uint32_t a = 0; a < n_aggs && (int64_t)(n_keys + a) < rncols; a++)
        ray_table_set_col_name(result, (int64_t)n_keys + a, agg_name_ids[a]);

    if (merge_tbl) ray_release(merge_tbl);
    ret = result;

pp_cleanup:
    scratch_free(mc_sym_ids_hdr);
    scratch_free(pk_syms_hdr);
    scratch_free(pkeys_hdr);
    scratch_free(mkeys_hdr);
    scratch_free(avg_idx_hdr);
    scratch_free(std_idx_hdr);
    scratch_free(std_orig_op_hdr);
    scratch_free(std_sq_slot_hdr);
    scratch_free(std_cnt_slot_hdr);
    scratch_free(part_ops_hdr);
    scratch_free(merge_ops_hdr);
    scratch_free(part_agg_syms_hdr);
    scratch_free(part_needs_sq_hdr);
    scratch_free(agg_name_ids_hdr);
    scratch_free(pagg_ins_hdr);
    scratch_free(magg_ins_hdr);
    scratch_free(unique_agg_hdr);
    /* Pushed-down WHERE cleanup: free the flattened-ids block and offset/
     * local-id scratch, and restore the caller's global selection (needed by
     * the concat fallback when this path returns NULL). */
    if (sel_ids_block) ray_release(sel_ids_block);
    scratch_free(part_off_hdr);
    scratch_free(local_ids_hdr);
    g->selection = saved_sel;
    return ret;
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

    /* uint32_t, not uint8_t: ly->n_keys is uint16_t (unbounded-ready, Task
     * 1).  This local used to truncate mod 256 — dormant while every
     * caller's n_keys stayed <=8, but pivot.c (the only caller of this
     * function) now admits any n_idx, so n_keys > 255 is reachable.  A
     * truncated value here undersizes the key_pool carve below while
     * derive_key_pool still writes ly->n_keys (untruncated) entries into
     * it — a heap buffer overflow, not just a wrong answer, once any
     * index key is wide (RAY_GUID/RAY_STR) and n_keys > 255. */
    uint32_t n_keys = ly->n_keys;

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

    uint8_t p1_nullable = 0;   /* 0/1: any key may be null (per-key re-checked in phase1) */
    for (uint32_t k = 0; k < n_keys; k++)
        if (ray_key_may_be_null(key_vecs[k])) { p1_nullable = 1; break; }

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
        .match_idx     = NULL,
    };
    ght_layout_copy(&p1ctx.layout, ly);
    /* Wide-key str-pool table (n_keys slots): carved once, freed post-dispatch. */
    ray_t* p1_kp_hdr = NULL;
    if (ly->any_wide_key) {
        p1ctx.key_pool = (const void**)scratch_alloc(&p1_kp_hdr,
            (size_t)n_keys * sizeof(void*));
        if (!p1ctx.key_pool) return false;
        derive_key_pool(ly, key_vecs, p1ctx.key_pool);
    }
    ray_pool_dispatch(pool, radix_phase1_fn, &p1ctx, n_scan);
    scratch_free(p1_kp_hdr);
    if (ray_interrupted()) return true; /* caller checks ray_interrupted() */
    /* Sync point — phase1 drained all rows, so rows_done == n_scan. */
    ray_progress_update(NULL, "hash-partition", (uint64_t)n_scan, (uint64_t)n_scan);

    for (size_t i = 0; i < n_bufs; i++)
        if (radix_bufs[i].oom) return false;

    group_ht_t* part_hts = (group_ht_t*)scratch_calloc(&out->_part_hts_hdr,
        RADIX_P * sizeof(group_ht_t));
    if (!part_hts) return false;
    /* Wire out->part_hts/n_parts at alloc time (like the sequential path) so
     * every failure below funnels through pivot_ingest_free — the calloc-zeroed
     * HTs are skipped by its rows||slots guard until phase2 fills them. */
    out->part_hts = part_hts;
    out->n_parts = RADIX_P;

    radix_phase2_ctx_t p2ctx = {
        .key_types = key_types,
        .n_keys    = n_keys,
        .n_workers = n_total,
        .bufs      = radix_bufs,
        .part_hts  = part_hts,
        .key_data  = key_data,
    };
    ght_layout_copy(&p2ctx.layout, ly);
    /* Wide-key str-pool table: copied into part_hts by phase2, freed after. */
    ray_t* p2_kp_hdr = NULL;
    if (ly->any_wide_key) {
        p2ctx.key_pool = (const void**)scratch_alloc(&p2_kp_hdr,
            (size_t)n_keys * sizeof(void*));
        if (!p2ctx.key_pool) return false;
        derive_key_pool(ly, key_vecs, p2ctx.key_pool);
    }
    ray_pool_dispatch_n(pool, radix_phase2_fn, &p2ctx, RADIX_P);
    scratch_free(p2_kp_hdr);
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
