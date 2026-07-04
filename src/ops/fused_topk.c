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

/*
 * Fused filter + top-K.  One pass over the source rows: predicate eval
 * per morsel into stack bits[], then for each passing row a bounded-heap
 * insert that keeps only the K best by the sort key.  No intermediate
 * filtered-table materialization (the FILTER → SORT_TAKE pipeline writes
 * a 2.5M-row temp table; this skips it).
 *
 * Targets the ORDER BY + LIMIT cluster: previously the engine
 * materialised a full filtered table and sorted it before applying
 * TAKE; the heap-bounded merge here keeps only K rows in flight.
 *
 * Single sort key for now.  SYM keys go through the comparator path
 * (dict-id → string → memcmp).  Numeric keys do direct value compare.
 *
 * Public surface: ray_fused_topk_supported() (gate probe) and
 * ray_fused_topk_select() (executor).  The planner in query.c calls
 * these before building the DAG; on a shape miss the caller falls back
 * to the unfused FILTER + apply_sort_take path.
 */

#include "ops/fused_topk.h"
#include "ops/fused_pred.h"
#include "ops/fused_group.h"   /* ray_fused_group_supported */
#include "ops/internal.h"
#include "lang/internal.h"
#include "core/pool.h"

#include <string.h>

/* Use the same predicate-shape detector as fused_group.  Single comparison
 * or AND of comparisons against literals on flat int/temporal/SYM columns. */
int ray_fused_topk_supported(ray_t* where_expr, ray_t* tbl) {
    return ray_fused_group_supported(where_expr, tbl);
}

/* ───── Heap entry ─────────────────────────────────────────────────────
 * Two parallel arrays per worker: heap_idx[] (source row indices) and
 * heap_key[] (encoded uint64 sort key for numeric, dict-id for SYM).
 * For numeric the encoded key is order-preserving so a single uint64
 * compare gives the right order.  For SYM we fall back to comparator
 * (sym_str / memcmp).  Stored as max-heap for ASC top-K (the WORST is
 * at the root and gets evicted first), or min-heap for DESC.
 * ──────────────────────────────────────────────────────────────────── */

#define FPK_MAX_KEYS 16

typedef struct {
    int8_t      type;
    uint8_t     attrs;
    uint8_t     esz;
    uint8_t     desc;        /* 0 = asc, 1 = desc */
    /* When the column carries nulls, fpk_cmp consults them before
     * reading the raw payload and orders nulls LAST for ASC, FIRST for
     * DESC — matching sort.c's default null policy.  has_nulls is the
     * compile-time flag that gates the per-row probe. */
    uint8_t     has_nulls;
    /* SYM keys only: 1 when the column's domain is the runtime
     * singleton — cell ids then index the borrowed global snapshot
     * (ctx.sym_strings).  0 routes the compare through the column's
     * own domain (ray_sym_domain_str). */
    uint8_t     dom_runtime;
    int64_t     sym;
    const void* base;
    ray_t*      col;         /* for ray_vec_is_null when has_nulls */
} fpk_keyspec_t;

typedef struct {
    fp_pred_t      pred;
    fpk_keyspec_t  keys[FPK_MAX_KEYS];
    /* n_keys mirrors n_sort_keys (now uint32_t at the API boundary), but
     * its VALUE stays permanently <= FPK_MAX_KEYS==16 — a routing/arity
     * admission gate below (n_sort_keys > FPK_MAX_KEYS -> NULL), not a
     * cut-3 cap slated to lift.  Widened anyway so no narrower carrier
     * stores a value that arrived through the wide parameter. */
    uint32_t       n_keys;
    int64_t        k;
    /* Per-worker bounded heap of row indices.  Comparator is invoked
     * via fpk_cmp; multi-key comparisons short-circuit on the first
     * key that breaks the tie.  No payload — keys are re-read from the
     * source columns at compare time. */
    int64_t*       heap_idx;       /* [nw * k] */
    int32_t*       heap_n;         /* [nw] */
    ray_t*         tbl;
    /* Lock-free snapshot of the GLOBAL SYM dict — populated once per
     * query for any runtime-domain RAY_SYM sort key.  ray_sym_str()
     * takes a global lock per call, which on the SYM heap-compare hot
     * path turns 28-way parallel into 12× *slower* due to lock
     * contention.  Borrowing the strings array once lets workers index
     * into it without the lock.  Non-runtime-domain keys bypass the
     * snapshot and resolve per compare via ray_sym_domain_str (locked;
     * a per-domain snapshot is a flip-time optimization if needed). */
    ray_t**        sym_strings;
    uint32_t       sym_count;
    _Atomic(uint32_t) oom;
} fpk_par_ctx_t;

/* Compare two source rows by the multi-key sort spec.  Returns
 * "a is worse than b" sense: positive means evict-a-first in the
 * max-heap of K-best entries.  Short-circuits on first non-equal key.
 *
 * Numeric/temporal narrow widths use sign-aware reads so a stored -1
 * compares as -1 and not 65535 — same fix as the fused-group agg
 * read.  Signed/unsigned matches the column class: BOOL/U8/SYM-id
 * are unsigned; I16/I32/I64 and the temporals (DATE/TIME/TIMESTAMP)
 * are signed.
 *
 * Final tie-break is by source row index ascending — produces a
 * deterministic, source-order-preserving result that matches a
 * stable sort of the surviving rows.  Without the tie-break, two
 * runs of the same query against the same data could return
 * different rows for ties because morsel scheduling between
 * workers is non-deterministic. */
static inline int fpk_cmp(const fpk_par_ctx_t* c, int64_t row_a, int64_t row_b) {
    if (RAY_UNLIKELY(row_a == row_b)) return 0;
    for (uint32_t k = 0; k < c->n_keys; k++) {
        const fpk_keyspec_t* ks = &c->keys[k];
        int cmp = 0;
        /* Null-aware leg.  Default policy matches sort.c: NULLS LAST
         * for ASC, NULLS FIRST for DESC.  In the max-heap-of-K-best
         * comparator, "worse" → evicted first, so for ASC a null is
         * worse than any non-null (so it goes last), and for DESC a
         * null is better than any non-null (so it goes first).  Both
         * legs short-circuit before the raw payload read. */
        if (ks->has_nulls) {
            bool a_null = ray_vec_is_null(ks->col, row_a);
            bool b_null = ray_vec_is_null(ks->col, row_b);
            if (a_null && b_null) continue;       /* tie on this key */
            if (a_null) return ks->desc ? -1 : 1; /* a is null */
            if (b_null) return ks->desc ?  1 : -1;
        }
        if (ks->type == RAY_SYM) {
            /* cell-data: ids resolve over the COLUMN's domain.  Runtime-
             * domain columns use the lock-free global snapshot; other
             * domains resolve through ray_sym_domain_str. */
            uint32_t ia = (uint32_t)read_by_esz(ks->base, row_a, ks->esz);
            uint32_t ib = (uint32_t)read_by_esz(ks->base, row_b, ks->esz);
            if (ia == ib) continue;
            ray_t* sa;
            ray_t* sb;
            if (ks->dom_runtime) {
                if (ia >= c->sym_count || ib >= c->sym_count) continue;
                sa = c->sym_strings[ia];
                sb = c->sym_strings[ib];
            } else {
                struct ray_sym_domain_s* dom = ray_sym_vec_domain(ks->col);
                sa = ray_sym_domain_str(dom, (int64_t)ia);
                sb = ray_sym_domain_str(dom, (int64_t)ib);
            }
            if (!sa || !sb) continue;
            cmp = ray_str_cmp(sa, sb);
        } else if (ks->type == RAY_STR) {
            /* Variable-length STR key: compare the actual strings (dict
             * codes, if any, are first-occurrence order — not sorted).
             * No null in the STR model; empty "" is a normal value. */
            size_t la = 0, lb = 0;
            const char* sa = ray_str_vec_get(ks->col, row_a, &la);
            const char* sb = ray_str_vec_get(ks->col, row_b, &lb);
            if (!sa) sa = "";
            if (!sb) sb = "";
            size_t ml = la < lb ? la : lb;
            cmp = ml ? memcmp(sa, sb, ml) : 0;
            if (cmp == 0) {
                if (la < lb) cmp = -1;
                else if (la > lb) cmp = 1;
            }
            if (cmp == 0) continue;
        } else if (ks->esz == 8) {
            int64_t va = ((const int64_t*)ks->base)[row_a];
            int64_t vb = ((const int64_t*)ks->base)[row_b];
            if (va < vb) cmp = -1; else if (va > vb) cmp = 1;
        } else if (ks->esz == 4) {
            int32_t va = ((const int32_t*)ks->base)[row_a];
            int32_t vb = ((const int32_t*)ks->base)[row_b];
            if (va < vb) cmp = -1; else if (va > vb) cmp = 1;
        } else if (ks->esz == 2) {
            int16_t va = ((const int16_t*)ks->base)[row_a];
            int16_t vb = ((const int16_t*)ks->base)[row_b];
            if (va < vb) cmp = -1; else if (va > vb) cmp = 1;
        } else { /* esz == 1 */
            uint8_t va = ((const uint8_t*)ks->base)[row_a];
            uint8_t vb = ((const uint8_t*)ks->base)[row_b];
            if (va < vb) cmp = -1; else if (va > vb) cmp = 1;
        }
        if (cmp != 0) return ks->desc ? -cmp : cmp;
    }
    /* All sort keys tie: break by source row index.  A stable sort
     * preserves source order on ties — the prefix of K rows that
     * survives is the K rows with the smallest original indices.
     * In the max-heap of K best entries, the root holds the worst
     * survivor (the one most likely to be evicted), so a row with
     * a higher source index ranks worse than a row with a lower
     * one.  Direction-independent: for both ASC and DESC top-K we
     * want stable source-order semantics on ties.
     *
     * Future: caller-specified NULLS FIRST / NULLS LAST.  The
     * has_nulls leg above implements the default policy (LAST for
     * ASC, FIRST for DESC).  An explicit NULLS FIRST/LAST clause
     * in the query DSL would need to thread through fpk_keyspec_t
     * as a third orientation flag and override the default leg —
     * the call site already has all the data needed.  Tracked
     * separately. */
    if (row_a > row_b) return  1;   /* a is worse — evict first */
    if (row_a < row_b) return -1;
    return 0;
}

/* Sift root down (root = current "worst" entry that gets evicted next). */
static inline void fpk_sift_down(const fpk_par_ctx_t* c, int64_t* heap,
                                 int32_t n, int32_t i) {
    for (;;) {
        int32_t worst = i;
        int32_t l = 2 * i + 1, r = 2 * i + 2;
        if (l < n && fpk_cmp(c, heap[l], heap[worst]) > 0) worst = l;
        if (r < n && fpk_cmp(c, heap[r], heap[worst]) > 0) worst = r;
        if (worst == i) return;
        int64_t ti = heap[i]; heap[i] = heap[worst]; heap[worst] = ti;
        i = worst;
    }
}

static inline void fpk_heapify(const fpk_par_ctx_t* c, int64_t* heap, int32_t n) {
    for (int32_t i = n / 2 - 1; i >= 0; i--)
        fpk_sift_down(c, heap, n, i);
}

/* Worker fn: scan rows [start, end), eval predicate per morsel, do
 * heap inserts for passing rows. */
static void fpk_par_fn(void* raw, uint32_t worker_id, int64_t start, int64_t end) {
    fpk_par_ctx_t* c = (fpk_par_ctx_t*)raw;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    int32_t  k    = (int32_t)c->k;
    int64_t* hidx = &c->heap_idx[(size_t)worker_id * (size_t)k];
    int32_t  hn   = c->heap_n[worker_id];

    int64_t row = start;
    while (row < end) {
        int64_t mend = row + RAY_MORSEL_ELEMS;
        if (mend > end) mend = end;
        int64_t mlen = mend - row;
        uint8_t bits[RAY_MORSEL_ELEMS];
        fp_eval_pred(&c->pred, row, mend, bits);

        for (int64_t r = 0; r < mlen; r++) {
            if (!bits[r]) continue;
            int64_t src_row = row + r;
            if (hn < k) {
                hidx[hn++] = src_row;
                if (hn == k) fpk_heapify(c, hidx, k);
            } else {
                /* Reject fast: skip if new ≥ current worst (heap root). */
                if (fpk_cmp(c, src_row, hidx[0]) >= 0) continue;
                hidx[0] = src_row;
                fpk_sift_down(c, hidx, k, 0);
            }
        }
        row = mend;
    }
    c->heap_n[worker_id] = hn;
}

/* Insertion sort over K entries so output rows match full-sort + take order. */
static void fpk_sort_final(const fpk_par_ctx_t* c, int64_t* idx, int32_t n) {
    for (int32_t i = 1; i < n; i++) {
        int64_t v = idx[i];
        int32_t j = i - 1;
        while (j >= 0 && fpk_cmp(c, idx[j], v) > 0) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = v;
    }
}

ray_t* ray_fused_topk_select(ray_t* tbl,
                             ray_t* where_expr,
                             const int64_t* sort_key_syms,
                             const uint8_t* sort_descs,
                             uint32_t n_sort_keys,
                             int64_t k,
                             const int64_t* out_col_syms,
                             const int64_t* out_alias_syms,
                             uint32_t n_out)
{
    if (!tbl || tbl->type != RAY_TABLE || k <= 0 || n_out == 0) return NULL;
    if (k > FPK_MAX_K) return NULL;
    if (n_sort_keys == 0 || n_sort_keys > FPK_MAX_KEYS) return NULL;
    int64_t nrows = ray_table_nrows(tbl);
    if (nrows <= 0 || k >= nrows) return NULL;

    for (uint32_t c = 0; c < n_out; c++) {
        ray_t* col = ray_table_get_col(tbl, out_col_syms[c]);
        if (!col) return NULL;
        int8_t ot = col->type;
        if (RAY_IS_PARTED(ot) || ot == RAY_MAPCOMMON) return NULL;
        if (!ray_is_vec(col))
            return NULL;
    }

    /* Resolve sort-key columns + decide if any need the SYM dict snapshot. */
    fpk_par_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int sym_needed = 0;
    for (uint32_t i = 0; i < n_sort_keys; i++) {
        ray_t* col = ray_table_get_col(tbl, sort_key_syms[i]);
        if (!col) return NULL;
        int8_t kt = col->type;
        if (RAY_IS_PARTED(kt) || kt == RAY_MAPCOMMON) return NULL;
        if (kt != RAY_SYM && kt != RAY_STR && kt != RAY_BOOL && kt != RAY_U8
            && kt != RAY_I16 && kt != RAY_I32 && kt != RAY_I64
            && kt != RAY_DATE && kt != RAY_TIME && kt != RAY_TIMESTAMP)
            return NULL;
        ctx.keys[i].type      = kt;
        ctx.keys[i].attrs     = col->attrs;
        ctx.keys[i].esz       = ray_sym_elem_size(kt, col->attrs);
        ctx.keys[i].desc      = sort_descs[i];
        ctx.keys[i].has_nulls = (col->attrs & RAY_ATTR_HAS_NULLS) ? 1 : 0;
        ctx.keys[i].dom_runtime =
            (kt == RAY_SYM &&
             ray_sym_vec_domain(col) == ray_sym_runtime_domain()) ? 1 : 0;
        ctx.keys[i].sym       = sort_key_syms[i];
        ctx.keys[i].base      = ray_data(col);
        ctx.keys[i].col       = col;
        if (ctx.keys[i].dom_runtime) sym_needed = 1;
    }
    ctx.n_keys = n_sort_keys;
    ctx.k      = k;
    ctx.tbl    = tbl;

    /* Compile the predicate via a temp graph just for the WHERE clause. */
    ray_graph_t* g = ray_graph_new(tbl);
    if (!g) return NULL;
    ray_op_t* pred_dag = compile_expr_dag(g, where_expr);
    if (!pred_dag) { ray_graph_free(g); return NULL; }
    if (fp_compile_pred(g, pred_dag, tbl, &ctx.pred) != 0) {
        fp_pred_cleanup(&ctx.pred);
        ray_graph_free(g);
        return NULL;
    }

    if (sym_needed) {
        ray_sym_strings_borrow(&ctx.sym_strings, &ctx.sym_count);
        if (!ctx.sym_strings) { ray_graph_free(g); return NULL; }
    }
    atomic_store_explicit(&ctx.oom, 0, memory_order_relaxed);

    /* Allocate per-worker heaps. */
    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = pool ? ray_pool_total_workers(pool) : 1;
    ray_t* idx_hdr = NULL;
    ray_t* hn_hdr  = NULL;
    ctx.heap_idx = (int64_t*)scratch_alloc(&idx_hdr,
                                           (size_t)nw * (size_t)k * sizeof(int64_t));
    ctx.heap_n   = (int32_t*)scratch_calloc(&hn_hdr,
                                            (size_t)nw * sizeof(int32_t));
    if (!ctx.heap_idx || !ctx.heap_n) {
        if (idx_hdr) scratch_free(idx_hdr);
        if (hn_hdr)  scratch_free(hn_hdr);
        fp_pred_cleanup(&ctx.pred);
        ray_graph_free(g);
        return NULL;
    }

    if (pool) ray_pool_dispatch(pool, fpk_par_fn, &ctx, nrows);
    else      fpk_par_fn(&ctx, 0, 0, nrows);

    if (atomic_load_explicit(&ctx.oom, memory_order_relaxed)) {
        scratch_free(idx_hdr); scratch_free(hn_hdr);
        fp_pred_cleanup(&ctx.pred);
        ray_graph_free(g);
        return NULL;
    }

    /* Combine per-worker heaps into one global K-heap.  Stack-resident
     * to avoid an alloc on the hot combine path.  k <= FPK_MAX_K is
     * enforced at function entry. */
    int64_t global_idx[FPK_MAX_K];
    int32_t global_n = 0;
    for (uint32_t w = 0; w < nw; w++) {
        int32_t  hn   = ctx.heap_n[w];
        int64_t* hidx = &ctx.heap_idx[(size_t)w * (size_t)k];
        for (int32_t i = 0; i < hn; i++) {
            if (global_n < (int32_t)k) {
                global_idx[global_n++] = hidx[i];
                if (global_n == (int32_t)k)
                    fpk_heapify(&ctx, global_idx, global_n);
            } else {
                if (fpk_cmp(&ctx, hidx[i], global_idx[0]) >= 0) continue;
                global_idx[0] = hidx[i];
                fpk_sift_down(&ctx, global_idx, global_n, 0);
            }
        }
    }

    scratch_free(idx_hdr);
    scratch_free(hn_hdr);

    fpk_sort_final(&ctx, global_idx, global_n);

    /* Materialize n_out output columns by gathering rows[global_idx]. */
    ray_t* result = ray_table_new(n_out);
    if (!result || RAY_IS_ERR(result)) {
        fp_pred_cleanup(&ctx.pred);
        ray_graph_free(g);
        return result ? result : ray_error("oom", NULL);
    }
    int build_ok = 1;
    for (uint32_t c = 0; c < n_out; c++) {
        int64_t cs    = out_col_syms[c];
        int64_t alias = out_alias_syms ? out_alias_syms[c] : cs;
        ray_t* src = ray_table_get_col(tbl, cs);
        if (!src) { build_ok = 0; break; }
        ray_t* col = gather_by_idx(src, global_idx, global_n);
        if (!col || RAY_IS_ERR(col)) { build_ok = 0; break; }
        result = ray_table_add_col(result, alias, col);
        ray_release(col);
    }
    ray_graph_free(g);
    fp_pred_cleanup(&ctx.pred);
    if (!build_ok) {
        ray_release(result);
        return ray_error("schema", NULL);
    }
    return result;
}
