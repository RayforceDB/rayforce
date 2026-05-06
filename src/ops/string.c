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
#include "ops/glob.h"
#include "ops/rowsel.h"
#include "core/pool.h"

/* ============================================================================
 * OP_LIKE: glob pattern matching on STR / SYM columns.  See ops/glob.[ch].
 * Syntax: * (any), ? (one char), [abc] / [a-z] / [!abc] (character class).
 * ============================================================================ */

/* Pattern-resolve worker for the SYM-LIKE fast path.  Runs over a
 * range of sym_ids; for each marked-as-seen sid, runs the matcher and
 * writes the answer to lut[sid].  Pure read-only on the inputs after
 * the seen-mark phase, so workers are independent. */
typedef struct {
    ray_t**                    sym_strings;
    uint8_t*                   seen;
    uint8_t*                   lut;
    const ray_glob_compiled_t* pc;
    bool                       use_simple;
    const char*                pat_str;
    size_t                     pat_len;
} like_resolve_ctx_t;

/* Worker for the SYM-LIKE seen-mark phase.  Marks `seen[sid] = 1` for
 * every row's sym_id.  Multiple workers can target the same byte, so
 * the store is via __atomic_store_n with relaxed ordering — same
 * machine code as a plain byte store on x86, but standard-defined
 * (plain non-atomic concurrent writes are UB even when the value is
 * idempotent).  Width-specialised on the SYM dictionary width. */
typedef struct {
    const void* base;
    uint8_t*    seen;
    uint64_t    dict_n;
    int         sym_w;
    /* Optional rowsel — when non-NULL, skip rows already filtered out
     * by an earlier WHERE conjunct.  Reduces seen[] population to just
     * the surviving rows' sym_ids and short-circuits phase 2 work
     * (resolve runs over the smaller seen set). */
    const uint8_t*  sel_flg;
    const uint32_t* sel_offs;
    const uint16_t* sel_idx;
    uint32_t        sel_n_segs;
    int64_t         total_rows;
} like_seen_ctx_t;

/* Macro to mark seen[sid] for a single row at index `r`.  The store
 * is __atomic_store_n with relaxed ordering — workers can race on the
 * same byte (different rows can resolve to the same dict ID), and the
 * relaxed atomic gives UB-free semantics with the same x86 codegen as
 * a plain byte store. */
#define LIKE_SEEN_MARK(SID) \
    __atomic_store_n(&seen[(SID)], (uint8_t)1, __ATOMIC_RELAXED)
#define LIKE_SEEN_MARK_ROW(W) do {                                  \
    if ((W) == RAY_SYM_W8) {                                         \
        uint64_t sid = ((const uint8_t*)x->base)[r];                 \
        if (sid < dict_n) LIKE_SEEN_MARK(sid);                       \
    } else if ((W) == RAY_SYM_W16) {                                 \
        uint64_t sid = ((const uint16_t*)x->base)[r];                \
        if (sid < dict_n) LIKE_SEEN_MARK(sid);                       \
    } else if ((W) == RAY_SYM_W32) {                                 \
        uint64_t sid = ((const uint32_t*)x->base)[r];                \
        if (sid < dict_n) LIKE_SEEN_MARK(sid);                       \
    } else {                                                         \
        int64_t sid = ((const int64_t*)x->base)[r];                  \
        if ((uint64_t)sid < dict_n) LIKE_SEEN_MARK(sid);             \
    }                                                                \
} while (0)

static void like_seen_fn(void* vctx, uint32_t worker_id,
                         int64_t start, int64_t end) {
    (void)worker_id;
    like_seen_ctx_t* x = (like_seen_ctx_t*)vctx;
    uint8_t* seen = x->seen;
    uint64_t dict_n = x->dict_n;
    int sym_w = x->sym_w;

    /* Selection-aware path: walk per morsel segment and only mark
     * surviving rows.  When the segment is fully out, skip its row
     * range entirely; when fully in, run the dense width-typed loop;
     * MIX builds the per-segment in-bitmap and probes per row. */
    if (x->sel_flg) {
        const uint8_t*  flg  = x->sel_flg;
        const uint32_t* offs = x->sel_offs;
        const uint16_t* lidx = x->sel_idx;
        uint32_t seg_lo = (uint32_t)(start / RAY_MORSEL_ELEMS);
        uint32_t seg_hi = (uint32_t)((end + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);
        if (seg_hi > x->sel_n_segs) seg_hi = x->sel_n_segs;
        for (uint32_t seg = seg_lo; seg < seg_hi; seg++) {
            int64_t s_lo = (int64_t)seg * RAY_MORSEL_ELEMS;
            int64_t s_hi = s_lo + RAY_MORSEL_ELEMS;
            if (s_lo < start) s_lo = start;
            if (s_hi > end)   s_hi = end;
            uint8_t f = flg[seg];
            if (f == RAY_SEL_NONE) continue;
            if (f == RAY_SEL_ALL) {
                for (int64_t r = s_lo; r < s_hi; r++) LIKE_SEEN_MARK_ROW(sym_w);
                continue;
            }
            uint8_t in_seg[RAY_MORSEL_ELEMS / 8] = {0};
            uint32_t off = offs[seg];
            uint32_t cnt = offs[seg + 1] - off;
            for (uint32_t i = 0; i < cnt; i++) {
                uint16_t loc = lidx[off + i];
                in_seg[loc >> 3] |= (uint8_t)(1u << (loc & 7));
            }
            int64_t base = (int64_t)seg * RAY_MORSEL_ELEMS;
            for (int64_t r = s_lo; r < s_hi; r++) {
                uint16_t loc = (uint16_t)(r - base);
                if (!(in_seg[loc >> 3] & (1u << (loc & 7)))) continue;
                LIKE_SEEN_MARK_ROW(sym_w);
            }
        }
        return;
    }

    /* No selection: dense width-typed loop. */
    switch (sym_w) {
    case RAY_SYM_W8: {
        const uint8_t* d = (const uint8_t*)x->base;
        for (int64_t i = start; i < end; i++) {
            uint64_t sid = d[i];
            if (sid < dict_n) LIKE_SEEN_MARK(sid);
        }
        break;
    }
    case RAY_SYM_W16: {
        const uint16_t* d = (const uint16_t*)x->base;
        for (int64_t i = start; i < end; i++) {
            uint64_t sid = d[i];
            if (sid < dict_n) LIKE_SEEN_MARK(sid);
        }
        break;
    }
    case RAY_SYM_W32: {
        const uint32_t* d = (const uint32_t*)x->base;
        for (int64_t i = start; i < end; i++) {
            uint64_t sid = d[i];
            if (sid < dict_n) LIKE_SEEN_MARK(sid);
        }
        break;
    }
    case RAY_SYM_W64:
    default: {
        const int64_t* d = (const int64_t*)x->base;
        for (int64_t i = start; i < end; i++) {
            int64_t sid = d[i];
            if ((uint64_t)sid < dict_n) LIKE_SEEN_MARK(sid);
        }
        break;
    }
    }
}
#undef LIKE_SEEN_MARK_ROW

/* Worker for the SYM-LIKE row-projection phase.  Reads the per-sid
 * answer from `lut[]` and writes into the per-row bool destination.
 * Workers write to disjoint slices of `dst`, so no synchronisation is
 * needed.  Width-specialised on the SYM dictionary width. */
typedef struct {
    const void* base;
    uint8_t*    dst;
    const uint8_t* lut;
    uint64_t    dict_n;
    int         sym_w;
    /* Optional rowsel — when non-NULL, leave dst[r] untouched for rows
     * already filtered out (those positions don't matter to the caller
     * since rowsel_refine only reads pred[r] for surviving rows). */
    const uint8_t*  sel_flg;
    const uint32_t* sel_offs;
    const uint16_t* sel_idx;
    uint32_t        sel_n_segs;
} like_proj_ctx_t;

#define LIKE_PROJ_SET_ROW(W) do {                                   \
    if ((W) == RAY_SYM_W8) {                                         \
        uint64_t sid = ((const uint8_t*)x->base)[r];                 \
        dst[r] = (sid < dict_n) ? lut[sid] : 0;                      \
    } else if ((W) == RAY_SYM_W16) {                                 \
        uint64_t sid = ((const uint16_t*)x->base)[r];                \
        dst[r] = (sid < dict_n) ? lut[sid] : 0;                      \
    } else if ((W) == RAY_SYM_W32) {                                 \
        uint64_t sid = ((const uint32_t*)x->base)[r];                \
        dst[r] = (sid < dict_n) ? lut[sid] : 0;                      \
    } else {                                                         \
        int64_t sid = ((const int64_t*)x->base)[r];                  \
        dst[r] = ((uint64_t)sid < dict_n) ? lut[sid] : 0;            \
    }                                                                \
} while (0)

static void like_proj_fn(void* vctx, uint32_t worker_id,
                         int64_t start, int64_t end) {
    (void)worker_id;
    like_proj_ctx_t* x = (like_proj_ctx_t*)vctx;
    uint8_t* dst = x->dst;
    const uint8_t* lut = x->lut;
    uint64_t dict_n = x->dict_n;
    int sym_w = x->sym_w;

    /* Selection-aware path: only project rows still in the rowsel.
     * Rows filtered out by an earlier WHERE conjunct have dst[r]
     * undefined — that's fine because ray_rowsel_refine ignores them
     * when chaining the next selection. */
    if (x->sel_flg) {
        const uint8_t*  flg  = x->sel_flg;
        const uint32_t* offs = x->sel_offs;
        const uint16_t* lidx = x->sel_idx;
        uint32_t seg_lo = (uint32_t)(start / RAY_MORSEL_ELEMS);
        uint32_t seg_hi = (uint32_t)((end + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);
        if (seg_hi > x->sel_n_segs) seg_hi = x->sel_n_segs;
        for (uint32_t seg = seg_lo; seg < seg_hi; seg++) {
            int64_t s_lo = (int64_t)seg * RAY_MORSEL_ELEMS;
            int64_t s_hi = s_lo + RAY_MORSEL_ELEMS;
            if (s_lo < start) s_lo = start;
            if (s_hi > end)   s_hi = end;
            uint8_t f = flg[seg];
            if (f == RAY_SEL_NONE) continue;
            if (f == RAY_SEL_ALL) {
                for (int64_t r = s_lo; r < s_hi; r++) LIKE_PROJ_SET_ROW(sym_w);
                continue;
            }
            uint8_t in_seg[RAY_MORSEL_ELEMS / 8] = {0};
            uint32_t off = offs[seg];
            uint32_t cnt = offs[seg + 1] - off;
            for (uint32_t i = 0; i < cnt; i++) {
                uint16_t loc = lidx[off + i];
                in_seg[loc >> 3] |= (uint8_t)(1u << (loc & 7));
            }
            int64_t base = (int64_t)seg * RAY_MORSEL_ELEMS;
            for (int64_t r = s_lo; r < s_hi; r++) {
                uint16_t loc = (uint16_t)(r - base);
                if (!(in_seg[loc >> 3] & (1u << (loc & 7)))) continue;
                LIKE_PROJ_SET_ROW(sym_w);
            }
        }
        return;
    }

    switch (sym_w) {
    case RAY_SYM_W8: {
        const uint8_t* d = (const uint8_t*)x->base;
        for (int64_t i = start; i < end; i++) {
            uint64_t sid = d[i];
            dst[i] = (sid < dict_n) ? lut[sid] : 0;
        }
        break;
    }
    case RAY_SYM_W16: {
        const uint16_t* d = (const uint16_t*)x->base;
        for (int64_t i = start; i < end; i++) {
            uint64_t sid = d[i];
            dst[i] = (sid < dict_n) ? lut[sid] : 0;
        }
        break;
    }
    case RAY_SYM_W32: {
        const uint32_t* d = (const uint32_t*)x->base;
        for (int64_t i = start; i < end; i++) {
            uint64_t sid = d[i];
            dst[i] = (sid < dict_n) ? lut[sid] : 0;
        }
        break;
    }
    case RAY_SYM_W64:
    default: {
        const int64_t* d = (const int64_t*)x->base;
        for (int64_t i = start; i < end; i++) {
            int64_t sid = d[i];
            dst[i] = ((uint64_t)sid < dict_n) ? lut[sid] : 0;
        }
        break;
    }
    }
}
#undef LIKE_PROJ_SET_ROW

/* Worker for the RAY_STR-LIKE parallel path.  Each task scans its
 * row range against the (pre-compiled) glob pattern; rows are
 * independent so no synchronisation needed. */
typedef struct {
    const ray_str_t*           elems;
    const char*                pool_data;
    uint8_t*                   dst;
    const ray_glob_compiled_t* pc;
    bool                       use_simple;
    const char*                pat_str;
    size_t                     pat_len;
} str_like_par_ctx_t;

static void str_like_par_fn(void* vctx, uint32_t worker_id,
                            int64_t start, int64_t end) {
    (void)worker_id;
    str_like_par_ctx_t* x = (str_like_par_ctx_t*)vctx;
    for (int64_t i = start; i < end; i++) {
        const char* sp = ray_str_t_ptr(&x->elems[i], x->pool_data);
        size_t sl = x->elems[i].len;
        x->dst[i] = (x->use_simple
                     ? ray_glob_match_compiled(x->pc, sp, sl)
                     : ray_glob_match(sp, sl, x->pat_str, x->pat_len)) ? 1 : 0;
    }
}

static void like_resolve_fn(void* ctx, uint32_t worker_id,
                            int64_t start, int64_t end) {
    (void)worker_id;
    like_resolve_ctx_t* x = (like_resolve_ctx_t*)ctx;
    for (int64_t sid = start; sid < end; sid++) {
        if (!x->seen[sid]) continue;
        ray_t* str = x->sym_strings[sid];
        if (!str) { x->lut[sid] = 0; continue; }
        const char* sp = ray_str_ptr(str);
        size_t sl = ray_str_len(str);
        x->lut[sid] = (x->use_simple
                       ? ray_glob_match_compiled(x->pc, sp, sl)
                       : ray_glob_match(sp, sl, x->pat_str, x->pat_len))
                      ? 1 : 0;
    }
}

ray_t* exec_like(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* pat_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (pat_v && !RAY_IS_ERR(pat_v)) ray_release(pat_v); return input; }
    if (!pat_v || RAY_IS_ERR(pat_v)) { ray_release(input); return pat_v; }

    /* Get pattern string */
    const char* pat_str = ray_str_ptr(pat_v);
    size_t pat_len = ray_str_len(pat_v);

    /* Pre-compile pattern into the simple-shape form when possible — the
     * substring/prefix/suffix branches drive memmem/memcmp directly,
     * roughly an order of magnitude faster than the iterative matcher
     * for the very common `*literal*` shape. */
    ray_glob_compiled_t pc = ray_glob_compile(pat_str, pat_len);
    bool use_simple = pc.shape != RAY_GLOB_SHAPE_NONE;

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_BOOL, len);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(input); ray_release(pat_v);
        return result;
    }
    result->len = len;
    uint8_t* dst = (uint8_t*)ray_data(result);

    int8_t in_type = input->type;
    if (in_type == RAY_STR) {
        /* Parallel substring/glob match over RAY_STR.  Q22/Q23 ClickBench
         * are 5 M URL/Title columns × ~80 chars/row = ~400 MB scan,
         * memory-bandwidth bound; the worker pool gives a 5-10× speedup
         * since glob_match is independent per row. */
        const ray_str_t* elems; const char* pool_data;
        str_resolve(input, &elems, &pool_data);

        str_like_par_ctx_t lctx = {
            .elems      = elems,
            .pool_data  = pool_data,
            .dst        = dst,
            .pc         = &pc,
            .use_simple = use_simple,
            .pat_str    = pat_str,
            .pat_len    = pat_len,
        };
        ray_pool_t* str_pool = ray_pool_get();
        if (str_pool && len >= 100000 && ray_pool_total_workers(str_pool) >= 2) {
            ray_pool_dispatch(str_pool, str_like_par_fn, &lctx, len);
        } else {
            str_like_par_fn(&lctx, 0, 0, len);
        }
    } else if (RAY_IS_SYM(in_type)) {
        /* Dictionary-cached fast path.
         *
         * Three-phase pipeline:
         *   (1) seen-mark — single sequential row scan that flips a
         *       byte in `seen[]` for every referenced sym_id.  Cheap;
         *       just sets a byte per row.
         *   (2) parallel pattern resolve — partition the dict_n range
         *       across pool workers; for each sid where seen[sid]==1,
         *       run the matcher and store the answer in lut[sid].
         *   (3) parallel row projection — every row reads lut[sid_i].
         *
         * Splitting the resolve from the row scan lets phase (2) drive
         * the pattern matcher (memmem on long URL strings) across the
         * worker pool.  ray_sym_count is the GLOBAL dictionary so for
         * a low-card column like BrowserCountry phase (1) keeps the
         * resolve work bounded to that column's actual sym_ids. */
        const void* base = ray_data(input);
        ray_t** sym_strings = NULL;
        uint32_t dict_n = 0;
        ray_sym_strings_borrow(&sym_strings, &dict_n);
        ray_t* lut_hdr = NULL;
        ray_t* seen_hdr = NULL;
        uint8_t* lut = NULL;
        uint8_t* seen = NULL;
        if (dict_n > 0) {
            lut  = (uint8_t*)scratch_alloc (&lut_hdr,  (size_t)dict_n);
            seen = (uint8_t*)scratch_calloc(&seen_hdr, (size_t)dict_n);
        }
        if (lut && seen) {
            int sym_w = (int)(input->attrs & RAY_SYM_W_MASK);

            ray_pool_t* pool = ray_pool_get();
            /* Phase 1: mark used sym_ids.  Parallelised because for
             * high-cardinality columns (URL on ClickBench) the seen-
             * mark scan was a 5 ms-class serial pass.  Multiple workers
             * may write 1 to the same byte concurrently — the value is
             * idempotent so the race is benign. */
            like_seen_ctx_t sctx = {
                .base       = base,
                .seen       = seen,
                .dict_n     = (uint64_t)dict_n,
                .sym_w      = sym_w,
                .sel_flg    = NULL,
                .sel_offs   = NULL,
                .sel_idx    = NULL,
                .sel_n_segs = 0,
                .total_rows = len,
            };
            if (g->selection) {
                ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
                sctx.sel_flg    = ray_rowsel_flags(g->selection);
                sctx.sel_offs   = ray_rowsel_offsets(g->selection);
                sctx.sel_idx    = ray_rowsel_idx(g->selection);
                sctx.sel_n_segs = sm->n_segs;
            }
            if (pool && len >= 200000 && ray_pool_total_workers(pool) >= 2) {
                ray_pool_dispatch(pool, like_seen_fn, &sctx, len);
            } else {
                like_seen_fn(&sctx, 0, 0, len);
            }

            /* Phase 2: parallel pattern resolve over the dict range. */
            like_resolve_ctx_t rctx = {
                .sym_strings = sym_strings, .seen = seen, .lut = lut,
                .pc = &pc, .use_simple = use_simple,
                .pat_str = pat_str, .pat_len = pat_len,
            };
            if (pool && (int64_t)dict_n >= 16384) {
                ray_pool_dispatch(pool, like_resolve_fn, &rctx, (int64_t)dict_n);
            } else {
                like_resolve_fn(&rctx, 0, 0, (int64_t)dict_n);
            }

            /* Phase 3: row projection — gather lut[sid] into the per-row
             * bool dst.  Parallelised because it's a 5 M-row pass (~5 ms
             * serial on a W64 SYM column).  Width-specialised in the
             * worker fn so the inner load is a typed pointer dereference. */
            like_proj_ctx_t pctx = {
                .base       = base,
                .dst        = dst,
                .lut        = lut,
                .dict_n     = (uint64_t)dict_n,
                .sym_w      = sym_w,
                .sel_flg    = sctx.sel_flg,
                .sel_offs   = sctx.sel_offs,
                .sel_idx    = sctx.sel_idx,
                .sel_n_segs = sctx.sel_n_segs,
            };
            if (pool && len >= 200000 && ray_pool_total_workers(pool) >= 2) {
                ray_pool_dispatch(pool, like_proj_fn, &pctx, len);
            } else {
                like_proj_fn(&pctx, 0, 0, len);
            }

            scratch_free(lut_hdr);
            scratch_free(seen_hdr);
        } else {
            /* OOM building the LUT: fall back to per-row scan. */
            if (lut_hdr) scratch_free(lut_hdr);
            if (seen_hdr) scratch_free(seen_hdr);
            for (int64_t i = 0; i < len; i++) {
                int64_t sym_id = ray_read_sym(base, i, in_type, input->attrs);
                ray_t* s = (sym_strings && (uint64_t)sym_id < (uint64_t)dict_n)
                           ? sym_strings[sym_id] : NULL;
                if (!s) { dst[i] = 0; continue; }
                const char* sp = ray_str_ptr(s);
                size_t sl = ray_str_len(s);
                dst[i] = (use_simple
                          ? ray_glob_match_compiled(&pc, sp, sl)
                          : ray_glob_match(sp, sl, pat_str, pat_len)) ? 1 : 0;
            }
        }
    } else {
        memset(dst, 0, (size_t)len);
    }

    ray_release(input); ray_release(pat_v);
    return result;
}

/* Case-insensitive LIKE — same syntax as `like`, ASCII-fold both sides. */

ray_t* exec_ilike(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* pat_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (pat_v && !RAY_IS_ERR(pat_v)) ray_release(pat_v); return input; }
    if (!pat_v || RAY_IS_ERR(pat_v)) { ray_release(input); return pat_v; }

    const char* pat_str = ray_str_ptr(pat_v);
    size_t pat_len = ray_str_len(pat_v);

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_BOOL, len);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(input); ray_release(pat_v);
        return result;
    }
    result->len = len;
    uint8_t* dst = (uint8_t*)ray_data(result);

    int8_t in_type = input->type;
    if (in_type == RAY_STR) {
        const ray_str_t* elems; const char* pool;
        str_resolve(input, &elems, &pool);
        for (int64_t i = 0; i < len; i++) {
            const char* sp = ray_str_t_ptr(&elems[i], pool);
            size_t sl = elems[i].len;
            dst[i] = ray_glob_match_ci(sp, sl, pat_str, pat_len) ? 1 : 0;
        }
    } else if (RAY_IS_SYM(in_type)) {
        /* Dictionary-cached fast path — see exec_like. */
        const void* base = ray_data(input);
        uint32_t dict_n = ray_sym_count();
        ray_t* lut_hdr = NULL;
        ray_t* seen_hdr = NULL;
        uint8_t* lut = NULL;
        uint8_t* seen = NULL;
        if (dict_n > 0) {
            lut  = (uint8_t*)scratch_alloc (&lut_hdr,  (size_t)dict_n);
            seen = (uint8_t*)scratch_calloc(&seen_hdr, (size_t)dict_n);
        }
        if (lut && seen) {
            for (int64_t i = 0; i < len; i++) {
                int64_t sid = ray_read_sym(base, i, in_type, input->attrs);
                if ((uint64_t)sid >= (uint64_t)dict_n) { dst[i] = 0; continue; }
                if (!seen[sid]) {
                    ray_t* s = ray_sym_str(sid);
                    if (!s) { lut[sid] = 0; }
                    else {
                        lut[sid] = ray_glob_match_ci(ray_str_ptr(s), ray_str_len(s),
                                                     pat_str, pat_len) ? 1 : 0;
                    }
                    seen[sid] = 1;
                }
                dst[i] = lut[sid];
            }
            scratch_free(lut_hdr);
            scratch_free(seen_hdr);
        } else {
            if (lut_hdr) scratch_free(lut_hdr);
            if (seen_hdr) scratch_free(seen_hdr);
            for (int64_t i = 0; i < len; i++) {
                int64_t sym_id = ray_read_sym(base, i, in_type, input->attrs);
                ray_t* s = ray_sym_str(sym_id);
                if (!s) { dst[i] = 0; continue; }
                dst[i] = ray_glob_match_ci(ray_str_ptr(s), ray_str_len(s), pat_str, pat_len) ? 1 : 0;
            }
        }
    } else {
        memset(dst, 0, (size_t)len);
    }

    ray_release(input); ray_release(pat_v);
    return result;
}

/* ============================================================================
 * String functions: UPPER, LOWER, TRIM, STRLEN, SUBSTR, REPLACE, CONCAT
 *
 * These functions call ray_sym_intern() per output row, which is
 * O(n * sym_table_lookup) per string op.  Acceptable for current workloads;
 * could be optimized with batch interning if profiling shows a bottleneck.
 * ============================================================================ */

/* UPPER / LOWER / TRIM — unary SYM/STR → SYM/STR */
ray_t* exec_string_unary(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;

    int64_t len = input->len;
    bool is_str = (input->type == RAY_STR);

    ray_t* result;
    if (is_str) {
        result = ray_vec_new(RAY_STR, len);
    } else {
        result = ray_vec_new(RAY_SYM, len);
    }
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    if (!is_str) result->len = len;
    int64_t* sym_dst = is_str ? NULL : (int64_t*)ray_data(result);

    const ray_str_t* str_elems = NULL;
    const char* str_pool = NULL;
    if (is_str) str_resolve(input, &str_elems, &str_pool);

    uint16_t opc = op->opcode;
    for (int64_t i = 0; i < len; i++) {
        /* Propagate null */
        if (ray_vec_is_null((ray_t*)input, i)) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                sym_dst[i] = 0;
                ray_vec_set_null(result, i, true);
            }
            continue;
        }
        const char* sp; size_t sl;
        if (is_str) {
            sp = ray_str_t_ptr(&str_elems[i], str_pool);
            sl = str_elems[i].len;
        } else {
            sym_elem(input, i, &sp, &sl);
        }

        char sbuf[8192];
        char* buf = sbuf;
        ray_t* dyn_hdr = NULL;
        if (sl >= sizeof(sbuf)) {
            buf = (char*)scratch_alloc(&dyn_hdr, sl + 1);
            if (!buf) {
                ray_release(result);
                ray_release(input);
                return ray_error("oom", NULL);
            }
        }
        size_t out_len = sl;
        if (opc == OP_UPPER) {
            for (size_t j = 0; j < out_len; j++) buf[j] = (char)toupper((unsigned char)sp[j]);
        } else if (opc == OP_LOWER) {
            for (size_t j = 0; j < out_len; j++) buf[j] = (char)tolower((unsigned char)sp[j]);
        } else { /* OP_TRIM */
            size_t start = 0, end = sl;
            while (start < sl && isspace((unsigned char)sp[start])) start++;
            while (end > start && isspace((unsigned char)sp[end - 1])) end--;
            out_len = end - start;
            memcpy(buf, sp + start, out_len);
        }

        if (is_str) {
            ray_t* prev = result;
            result = ray_str_vec_append(result, buf, out_len);
            if (RAY_IS_ERR(result)) { ray_release(prev); scratch_free(dyn_hdr); break; }
        } else {
            buf[out_len] = '\0';
            sym_dst[i] = ray_sym_intern(buf, out_len);
        }
        scratch_free(dyn_hdr);
    }
    ray_release(input);
    return result;
}

/* LENGTH — SYM → I64 */
ray_t* exec_strlen(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_I64, len);
    if (!result || RAY_IS_ERR(result)) { ray_release(input); return result; }
    result->len = len;
    int64_t* dst = (int64_t*)ray_data(result);

    if (input->type == RAY_STR) {
        const ray_str_t* elems; const char* pool;
        str_resolve(input, &elems, &pool);
        for (int64_t i = 0; i < len; i++) {
            if (ray_vec_is_null((ray_t*)input, i)) {
                dst[i] = 0;
                ray_vec_set_null(result, i, true);
                continue;
            }
            dst[i] = (int64_t)elems[i].len;
        }
    } else {
        for (int64_t i = 0; i < len; i++) {
            if (ray_vec_is_null((ray_t*)input, i)) {
                dst[i] = 0;
                ray_vec_set_null(result, i, true);
                continue;
            }
            const char* sp; size_t sl;
            sym_elem(input, i, &sp, &sl);
            dst[i] = (int64_t)sl;
        }
    }
    ray_release(input);
    return result;
}

/* SUBSTR(str, start, len) — 1-based start */
ray_t* exec_substr(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* start_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (start_v && !RAY_IS_ERR(start_v)) ray_release(start_v); return input; }
    if (!start_v || RAY_IS_ERR(start_v)) { ray_release(input); return start_v; }

    /* Get len arg from ext node's literal field */
    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t len_id = (uint32_t)(uintptr_t)ext->literal;
    ray_t* len_v = exec_node(g, &g->nodes[len_id]);
    if (!len_v || RAY_IS_ERR(len_v)) { ray_release(input); ray_release(start_v); return len_v; }

    int64_t nrows = input->len;
    bool is_str = (input->type == RAY_STR);

    ray_t* result;
    if (is_str) {
        result = ray_vec_new(RAY_STR, nrows);
    } else {
        result = ray_vec_new(RAY_SYM, nrows);
    }
    if (!result || RAY_IS_ERR(result)) { ray_release(input); ray_release(start_v); ray_release(len_v); return result; }
    if (!is_str) result->len = nrows;
    int64_t* sym_dst = is_str ? NULL : (int64_t*)ray_data(result);

    const ray_str_t* str_elems = NULL;
    const char* str_pool = NULL;
    if (is_str) str_resolve(input, &str_elems, &str_pool);

    /* start_v and len_v may be atom scalars or vectors.
     * Handle RAY_I32 vectors correctly (read as int32_t, not int64_t). */
    int64_t s_scalar = 0, l_scalar = 0;
    const int64_t* s_data = NULL;
    const int64_t* l_data = NULL;
    const int32_t* s_data_i32 = NULL;
    const int32_t* l_data_i32 = NULL;
    if (start_v->type == -RAY_I64) s_scalar = start_v->i64;
    else if (start_v->type == -RAY_F64) s_scalar = (int64_t)start_v->f64;
    else if (start_v->len == 1) {
        if (start_v->type == RAY_F64)
            s_scalar = (int64_t)((double*)ray_data(start_v))[0];
        else if (start_v->type == RAY_I32)
            s_scalar = (int64_t)((int32_t*)ray_data(start_v))[0];
        else
            s_scalar = ((int64_t*)ray_data(start_v))[0];
    }
    else if (start_v->type == RAY_I32) s_data_i32 = (const int32_t*)ray_data(start_v);
    else s_data = (const int64_t*)ray_data(start_v);
    if (len_v->type == -RAY_I64) l_scalar = len_v->i64;
    else if (len_v->type == -RAY_F64) l_scalar = (int64_t)len_v->f64;
    else if (len_v->len == 1) {
        if (len_v->type == RAY_F64)
            l_scalar = (int64_t)((double*)ray_data(len_v))[0];
        else if (len_v->type == RAY_I32)
            l_scalar = (int64_t)((int32_t*)ray_data(len_v))[0];
        else
            l_scalar = ((int64_t*)ray_data(len_v))[0];
    }
    else if (len_v->type == RAY_I32) l_data_i32 = (const int32_t*)ray_data(len_v);
    else l_data = (const int64_t*)ray_data(len_v);

    for (int64_t i = 0; i < nrows; i++) {
        /* Propagate null — from input, start, or length */
        if (ray_vec_is_null((ray_t*)input, i) ||
            ((s_data || s_data_i32) && ray_vec_is_null((ray_t*)start_v, i)) ||
            ((l_data || l_data_i32) && ray_vec_is_null((ray_t*)len_v, i))) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                sym_dst[i] = 0;
                ray_vec_set_null(result, i, true);
            }
            continue;
        }
        const char* sp; size_t sl;
        if (is_str) {
            sp = ray_str_t_ptr(&str_elems[i], str_pool);
            sl = str_elems[i].len;
        } else {
            sym_elem(input, i, &sp, &sl);
        }
        int64_t st = (s_data ? s_data[i] : s_data_i32 ? (int64_t)s_data_i32[i] : s_scalar) - 1; /* 1-based → 0-based */
        int64_t ln = l_data ? l_data[i] : l_data_i32 ? (int64_t)l_data_i32[i] : l_scalar;
        if (st < 0) st = 0;
        if ((size_t)st >= sl) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
            }
            else { sym_dst[i] = ray_sym_intern("", 0); }
            continue;
        }
        if (ln < 0 || ln > (int64_t)(sl - (size_t)st)) ln = (int64_t)sl - st;
        if (is_str) {
            result = ray_str_vec_append(result, sp + st, (size_t)ln);
            if (RAY_IS_ERR(result)) break;
        } else {
            sym_dst[i] = ray_sym_intern(sp + st, (size_t)ln);
        }
    }
    ray_release(input); ray_release(start_v); ray_release(len_v);
    return result;
}

/* REPLACE(str, from, to) */
ray_t* exec_replace(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op->inputs[0]);
    ray_t* from_v = exec_node(g, op->inputs[1]);
    if (!input || RAY_IS_ERR(input)) { if (from_v && !RAY_IS_ERR(from_v)) ray_release(from_v); return input; }
    if (!from_v || RAY_IS_ERR(from_v)) { ray_release(input); return from_v; }

    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t to_id = (uint32_t)(uintptr_t)ext->literal;
    ray_t* to_v = exec_node(g, &g->nodes[to_id]);
    if (!to_v || RAY_IS_ERR(to_v)) { ray_release(input); ray_release(from_v); return to_v; }

    /* from_v and to_v should be string constants (SYM atoms) */
    const char* from_str = ray_str_ptr(from_v);
    size_t from_len = ray_str_len(from_v);
    const char* to_str = ray_str_ptr(to_v);
    size_t to_len = ray_str_len(to_v);

    int64_t nrows = input->len;
    bool is_str = (input->type == RAY_STR);

    ray_t* result;
    if (is_str) {
        result = ray_vec_new(RAY_STR, nrows);
    } else {
        result = ray_vec_new(RAY_SYM, nrows);
    }
    if (!result || RAY_IS_ERR(result)) { ray_release(input); ray_release(from_v); ray_release(to_v); return result; }
    if (!is_str) result->len = nrows;
    int64_t* sym_dst = is_str ? NULL : (int64_t*)ray_data(result);

    const ray_str_t* str_elems = NULL;
    const char* str_pool = NULL;
    if (is_str) str_resolve(input, &str_elems, &str_pool);

    for (int64_t i = 0; i < nrows; i++) {
        /* Propagate null */
        if (ray_vec_is_null((ray_t*)input, i)) {
            if (is_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                sym_dst[i] = 0;
                ray_vec_set_null(result, i, true);
            }
            continue;
        }
        const char* sp; size_t sl;
        if (is_str) {
            sp = ray_str_t_ptr(&str_elems[i], str_pool);
            sl = str_elems[i].len;
        } else {
            sym_elem(input, i, &sp, &sl);
        }
        /* Simple find-and-replace-all */
        /* Worst case: every char is a match, each replaced by to_len bytes.
         * Guard against size_t overflow when to_len >> from_len. */
        size_t n_matches = (from_len > 0) ? sl / from_len : 0;
        size_t worst;
        if (from_len > 0 && to_len > from_len && n_matches > SIZE_MAX / to_len) {
            worst = SIZE_MAX; /* overflow → cap at max; scratch_alloc will OOM */
        } else if (from_len > 0 && to_len >= from_len) {
            /* Expanding or same-size: max output when every chunk matches */
            worst = n_matches * to_len + (sl % from_len) + 1;
        } else {
            /* Shrinking or from_len==0: max output when nothing matches → sl */
            worst = sl + 1;
        }
        char sbuf[8192];
        char* buf = sbuf;
        ray_t* dyn_hdr = NULL;
        if (worst > sizeof(sbuf)) {
            buf = (char*)scratch_alloc(&dyn_hdr, worst);
            if (!buf) {
                ray_release(result);
                ray_release(input); ray_release(from_v); ray_release(to_v);
                return ray_error("oom", NULL);
            }
        }
        size_t buf_cap = dyn_hdr ? worst : sizeof(sbuf);
        size_t bi = 0;
        for (size_t j = 0; j < sl; ) {
            if (from_len > 0 && j + from_len <= sl && memcmp(sp + j, from_str, from_len) == 0) {
                if (bi + to_len < buf_cap) { memcpy(buf + bi, to_str, to_len); bi += to_len; }
                j += from_len;
            } else {
                if (bi < buf_cap - 1) buf[bi++] = sp[j];
                j++;
            }
        }
        if (is_str) {
            ray_t* prev = result;
            result = ray_str_vec_append(result, buf, bi);
            if (RAY_IS_ERR(result)) { ray_release(prev); scratch_free(dyn_hdr); break; }
        } else {
            buf[bi] = '\0';
            sym_dst[i] = ray_sym_intern(buf, bi);
        }
        scratch_free(dyn_hdr);
    }
    ray_release(input); ray_release(from_v); ray_release(to_v);
    return result;
}

/* CONCAT(a, b, ...) */
ray_t* exec_concat(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    int64_t raw_nargs = ext->sym;
    if (raw_nargs < 2 || raw_nargs > 255) return ray_error("domain", NULL);
    int n_args = (int)raw_nargs;

    /* Evaluate all inputs */
    ray_t* args_stack[16];
    ray_t** args = args_stack;
    ray_t* args_hdr = NULL;
    if (n_args > 16) {
        args = (ray_t**)scratch_calloc(&args_hdr, (size_t)n_args * sizeof(ray_t*));
        if (!args) return ray_error("oom", NULL);
    }

    args[0] = exec_node(g, op->inputs[0]);
    args[1] = exec_node(g, op->inputs[1]);
    uint32_t* trail = (uint32_t*)((char*)(ext + 1));
    for (int i = 2; i < n_args; i++) {
        args[i] = exec_node(g, &g->nodes[trail[i - 2]]);
    }
    /* Error check */
    for (int i = 0; i < n_args; i++) {
        if (!args[i] || RAY_IS_ERR(args[i])) {
            ray_t* err = args[i];
            for (int j = 0; j < n_args; j++) {
                if (j != i && args[j] && !RAY_IS_ERR(args[j])) ray_release(args[j]);
            }
            scratch_free(args_hdr);
            return err;
        }
    }

    /* Derive nrows from first vector arg (scalar args have byte-length in len) */
    int64_t nrows = 1;
    bool out_str = false;
    for (int a = 0; a < n_args; a++) {
        int8_t at = args[a]->type;
        if (at == RAY_STR) { out_str = true; if (nrows == 1) nrows = args[a]->len; }
        if (RAY_IS_SYM(at)) { if (nrows == 1) nrows = args[a]->len; }
        if (!ray_is_atom(args[a]) && nrows == 1) { nrows = args[a]->len; }
    }
    ray_t* result = ray_vec_new(out_str ? RAY_STR : RAY_SYM, nrows);
    if (!result || RAY_IS_ERR(result)) {
        for (int i = 0; i < n_args; i++) ray_release(args[i]);
        scratch_free(args_hdr);
        return result;
    }
    if (!out_str) result->len = nrows;
    int64_t* dst = out_str ? NULL : (int64_t*)ray_data(result);

    for (int64_t r = 0; r < nrows; r++) {
        /* Check if any arg is null at this row */
        bool any_null = false;
        for (int a = 0; a < n_args; a++) {
            if (ray_is_atom(args[a])) {
                if (RAY_ATOM_IS_NULL(args[a])) { any_null = true; break; }
            } else if (ray_vec_is_null((ray_t*)args[a], r < args[a]->len ? r : 0)) {
                any_null = true;
                break;
            }
        }
        if (any_null) {
            if (out_str) {
                result = ray_str_vec_append(result, "", 0);
                if (RAY_IS_ERR(result)) break;
                ray_vec_set_null(result, result->len - 1, true);
            } else {
                dst[r] = 0;
                ray_vec_set_null(result, r, true);
            }
            continue;
        }
        /* Pre-scan to compute total concat length for this row */
        size_t total = 0;
        for (int a = 0; a < n_args; a++) {
            int8_t t = args[a]->type;
            if (t == RAY_STR) {
                const ray_str_t* elems; const char* p;
                str_resolve(args[a], &elems, &p);
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                total += elems[ar].len;
            } else if (RAY_IS_SYM(t)) {
                const char* sp; size_t sl;
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                sym_elem(args[a], ar, &sp, &sl);
                total += sl;
            } else if (t == -RAY_STR) {
                total += ray_str_len(args[a]);
            }
        }
        char sbuf[8192];
        char* buf = sbuf;
        ray_t* dyn_hdr = NULL;
        size_t buf_cap = sizeof(sbuf);
        if (total >= sizeof(sbuf)) {
            buf = (char*)scratch_alloc(&dyn_hdr, total + 1);
            if (!buf) {
                ray_release(result);
                for (int i = 0; i < n_args; i++) ray_release(args[i]);
                scratch_free(args_hdr);
                return ray_error("oom", NULL);
            }
            buf_cap = total + 1;
        }
        size_t bi = 0;
        for (int a = 0; a < n_args; a++) {
            int8_t t = args[a]->type;
            if (t == RAY_STR) {
                const ray_str_t* elems; const char* pool;
                str_resolve(args[a], &elems, &pool);
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                const char* sp = ray_str_t_ptr(&elems[ar], pool);
                size_t sl = elems[ar].len;
                if (bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            } else if (RAY_IS_SYM(t)) {
                const char* sp; size_t sl;
                int64_t ar = ray_is_atom(args[a]) ? 0 : (r < args[a]->len ? r : 0);
                sym_elem(args[a], ar, &sp, &sl);
                if (bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            } else if (t == -RAY_STR) {
                const char* sp = ray_str_ptr(args[a]);
                size_t sl = ray_str_len(args[a]);
                if (sp && bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            }
        }
        if (out_str) {
            ray_t* prev = result;
            result = ray_str_vec_append(result, buf, bi);
            if (RAY_IS_ERR(result)) { ray_release(prev); scratch_free(dyn_hdr); break; }
        } else {
            buf[bi] = '\0';
            dst[r] = ray_sym_intern(buf, bi);
        }
        scratch_free(dyn_hdr);
    }
    for (int i = 0; i < n_args; i++) ray_release(args[i]);
    scratch_free(args_hdr);
    return result;
}
