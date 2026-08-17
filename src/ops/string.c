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
#include "table/domain.h"   /* sym-domain resolution (Phase 2) */
#include "ops/glob.h"
#include "ops/rowsel.h"
#include "core/pool.h"
#include "lang/format.h"   /* ray_type_name (error context) */

/* ============================================================================
 * OP_LIKE: glob pattern matching on STR / SYM columns.  See ops/glob.[ch].
 * Syntax: * (any), ? (one char), [abc] / [a-z] / [!abc] (character class).
 * ============================================================================ */

/* Parallelism crossover thresholds.  Below these row counts the
 * pool dispatch + per-task setup cost outweighs the parallel speedup.
 * Determined empirically against wide analytical workloads.  STR
 * scans set their threshold higher because the pattern is matched
 * per row (no dict-shared prefix); SYM is per-dict-entry so the work
 * scales with cardinality, not row count, and parallelises well at
 * lower row counts. */
#define LIKE_PAR_MIN_ROWS_STR  200000
#define LIKE_PAR_MIN_ROWS_SYM  100000

/* Pattern-resolve worker for the SYM-LIKE fast path.  Runs over a
 * range of sym_ids; for each marked-as-seen sid, runs the matcher and
 * writes the answer to lut[sid].  Pure read-only on the inputs after
 * the seen-mark phase, so workers are independent. */
typedef struct {
    ray_t**                    sym_strings;  /* runtime-domain snapshot, or NULL */
    /* sym_strings == NULL ⇒ FILE-domain column (sym-domain Phase 2):
     * resolve each sid through `dom` instead.  The LUT is sized by
     * that domain's count at setup (fused_group.c precedent). */
    struct ray_sym_domain_s*   dom;
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

static int64_t parted_row_count(ray_t* input) {
    ray_t** segs = (ray_t**)ray_data(input);
    int64_t total = 0;
    for (int64_t s = 0; s < input->len; s++)
        if (segs[s]) total += segs[s]->len;
    return total;
}

static void like_resolve_fn(void* ctx, uint32_t worker_id,
                            int64_t start, int64_t end) {
    (void)worker_id;
    like_resolve_ctx_t* x = (like_resolve_ctx_t*)ctx;
    for (int64_t sid = start; sid < end; sid++) {
        if (!x->seen[sid]) continue;
        ray_t* str = x->sym_strings ? x->sym_strings[sid]
                                    : ray_sym_domain_str(x->dom, sid);
        if (!str) { x->lut[sid] = 0; continue; }
        const char* sp = ray_str_ptr(str);
        size_t sl = ray_str_len(str);
        x->lut[sid] = (x->use_simple
                       ? ray_glob_match_compiled(x->pc, sp, sl)
                       : ray_glob_match(sp, sl, x->pat_str, x->pat_len))
                      ? 1 : 0;
    }
}

static void exec_like_parted_str(ray_t* input, uint8_t* dst,
                                 const ray_glob_compiled_t* pc,
                                 bool use_simple,
                                 const char* pat_str, size_t pat_len) {
    ray_t** segs = (ray_t**)ray_data(input);
    ray_pool_t* pool = ray_pool_get();
    int64_t out_off = 0;

    for (int64_t s = 0; s < input->len; s++) {
        ray_t* seg = segs[s];
        if (!seg) continue;
        int64_t seg_len = seg->len;
        const ray_str_t* elems;
        const char* pool_data;
        str_resolve(seg, &elems, &pool_data);

        str_like_par_ctx_t lctx = {
            .elems      = elems,
            .pool_data  = pool_data,
            .dst        = dst + out_off,
            .pc         = pc,
            .use_simple = use_simple,
            .pat_str    = pat_str,
            .pat_len    = pat_len,
        };
        if (pool && seg_len >= LIKE_PAR_MIN_ROWS_STR &&
            ray_pool_total_workers(pool) >= 2) {
            ray_pool_dispatch(pool, str_like_par_fn, &lctx, seg_len);
        } else {
            str_like_par_fn(&lctx, 0, 0, seg_len);
        }
        out_off += seg_len;
    }
}

static void exec_like_parted_sym(ray_t* input, uint8_t* dst,
                                 const ray_glob_compiled_t* pc,
                                 bool use_simple,
                                 const char* pat_str, size_t pat_len,
                                 int64_t total_len) {
    ray_t** segs = (ray_t**)ray_data(input);
    /* Cell ids are positions in the COLUMN's domain (sym-domain
     * Phase 2).  All partitions of a parted SYM column share ONE
     * domain by construction (the root symfile) — take it from the
     * first SYM segment.  Runtime domain: borrow the global string
     * snapshot (lock-free per sid).  FILE domain: size the LUT by the
     * domain's count and resolve via ray_sym_domain_str (fused_group
     * precedent).  Pre-flip this is always the runtime branch. */
    struct ray_sym_domain_s* dom = NULL;
    for (int64_t s = 0; s < input->len; s++)
        if (segs[s] && segs[s]->type == RAY_SYM) {
            dom = ray_sym_vec_domain(segs[s]);
            break;
        }
    ray_t** sym_strings = NULL;
    uint32_t dict_n = 0;
    if (!dom || dom == ray_sym_runtime_domain()) {
        dom = NULL;
        ray_sym_strings_borrow(&sym_strings, &dict_n);
    } else {
        int64_t dn = ray_sym_domain_count(dom);
        dict_n = (dn > 0 && dn <= (int64_t)UINT32_MAX) ? (uint32_t)dn : 0;
    }
    ray_t* lut_hdr = NULL;
    ray_t* seen_hdr = NULL;
    uint8_t* lut = NULL;
    uint8_t* seen = NULL;
    if (dict_n > 0) {
        lut  = (uint8_t*)scratch_alloc (&lut_hdr,  (size_t)dict_n);
        seen = (uint8_t*)scratch_calloc(&seen_hdr, (size_t)dict_n);
    }

    ray_pool_t* pool = ray_pool_get();
    if (lut && seen) {
        for (int64_t s = 0; s < input->len; s++) {
            ray_t* seg = segs[s];
            if (!seg) continue;
            int64_t seg_len = seg->len;
            like_seen_ctx_t sctx = {
                .base       = ray_data(seg),
                .seen       = seen,
                .dict_n     = (uint64_t)dict_n,
                .sym_w      = (int)(seg->attrs & RAY_SYM_W_MASK),
                .sel_flg    = NULL,
                .sel_offs   = NULL,
                .sel_idx    = NULL,
                .sel_n_segs = 0,
                .total_rows = seg_len,
            };
            if (pool && seg_len >= LIKE_PAR_MIN_ROWS_SYM &&
                ray_pool_total_workers(pool) >= 2) {
                ray_pool_dispatch(pool, like_seen_fn, &sctx, seg_len);
            } else {
                like_seen_fn(&sctx, 0, 0, seg_len);
            }
        }

        like_resolve_ctx_t rctx = {
            .sym_strings = sym_strings, .dom = dom, .seen = seen, .lut = lut,
            .pc = pc, .use_simple = use_simple,
            .pat_str = pat_str, .pat_len = pat_len,
        };
        if (pool && (int64_t)dict_n >= 16384) {
            ray_pool_dispatch(pool, like_resolve_fn, &rctx, (int64_t)dict_n);
        } else {
            like_resolve_fn(&rctx, 0, 0, (int64_t)dict_n);
        }

        int64_t out_off = 0;
        for (int64_t s = 0; s < input->len; s++) {
            ray_t* seg = segs[s];
            if (!seg) continue;
            int64_t seg_len = seg->len;
            like_proj_ctx_t pctx = {
                .base       = ray_data(seg),
                .dst        = dst + out_off,
                .lut        = lut,
                .dict_n     = (uint64_t)dict_n,
                .sym_w      = (int)(seg->attrs & RAY_SYM_W_MASK),
                .sel_flg    = NULL,
                .sel_offs   = NULL,
                .sel_idx    = NULL,
                .sel_n_segs = 0,
            };
            if (pool && seg_len >= LIKE_PAR_MIN_ROWS_SYM &&
                ray_pool_total_workers(pool) >= 2) {
                ray_pool_dispatch(pool, like_proj_fn, &pctx, seg_len);
            } else {
                like_proj_fn(&pctx, 0, 0, seg_len);
            }
            out_off += seg_len;
        }
        scratch_free(lut_hdr);
        scratch_free(seen_hdr);
        return;
    }

    if (lut_hdr) scratch_free(lut_hdr);
    if (seen_hdr) scratch_free(seen_hdr);

    int64_t out_off = 0;
    for (int64_t s = 0; s < input->len; s++) {
        ray_t* seg = segs[s];
        if (!seg) continue;
        const void* base = ray_data(seg);
        for (int64_t i = 0; i < seg->len; i++) {
            int64_t sym_id = ray_read_sym(base, i, seg->type, seg->attrs);
            ray_t* str = dom ? ray_sym_domain_str(dom, sym_id)
                       : (sym_strings && (uint64_t)sym_id < (uint64_t)dict_n)
                       ? sym_strings[sym_id] : NULL;
            if (!str) { dst[out_off + i] = 0; continue; }
            const char* sp = ray_str_ptr(str);
            size_t sl = ray_str_len(str);
            dst[out_off + i] = (use_simple
                                ? ray_glob_match_compiled(pc, sp, sl)
                                : ray_glob_match(sp, sl, pat_str, pat_len))
                               ? 1 : 0;
        }
        out_off += seg->len;
    }
    if (out_off < total_len)
        memset(dst + out_off, 0, (size_t)(total_len - out_off));
}

static ray_t* exec_like_input(ray_graph_t* g, ray_op_t* input_op) {
    if (!input_op || input_op->opcode != OP_SCAN)
        return exec_node(g, input_op);

    ray_op_ext_t* ext = find_ext(g, input_op->id);
    if (!ext) return exec_node(g, input_op);

    uint16_t stored_table_id = 0;
    memcpy(&stored_table_id, ext->base.pad, sizeof(uint16_t));
    ray_t* scan_tbl = NULL;
    if (stored_table_id > 0 && g->tables && (stored_table_id - 1) < g->n_tables)
        scan_tbl = g->tables[stored_table_id - 1];
    else
        scan_tbl = g->table;
    if (!scan_tbl) return exec_node(g, input_op);

    ray_t* col = ray_table_get_col(scan_tbl, ext->sym);
    if (!col) return exec_node(g, input_op);
    if (RAY_IS_PARTED(col->type)) {
        int8_t base = (int8_t)RAY_PARTED_BASETYPE(col->type);
        if (base == RAY_STR || RAY_IS_SYM(base)) {
            ray_retain(col);
            return col;
        }
    }
    return exec_node(g, input_op);
}

ray_t* exec_like(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_like_input(g, op_child(g, op, 0));
    ray_t* pat_v = exec_node(g, op_child(g, op, 1));
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

    int8_t in_type = input->type;
    bool in_parted = RAY_IS_PARTED(in_type);
    int8_t base_type = in_parted ? (int8_t)RAY_PARTED_BASETYPE(in_type) : in_type;
    int64_t len = in_parted ? parted_row_count(input) : input->len;
    ray_t* result = ray_vec_new(RAY_BOOL, len);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(input); ray_release(pat_v);
        return result;
    }
    result->len = len;
    uint8_t* dst = (uint8_t*)ray_data(result);

    if (in_parted && base_type == RAY_STR) {
        exec_like_parted_str(input, dst, &pc, use_simple, pat_str, pat_len);
    } else if (in_parted && RAY_IS_SYM(base_type)) {
        exec_like_parted_sym(input, dst, &pc, use_simple, pat_str, pat_len, len);
    } else if (in_type == RAY_STR) {
        /* Parallel substring/glob match over RAY_STR.  Wide text scans
         * over URL/title-like columns are memory-bandwidth bound; the
         * worker pool gives a 5-10× speedup
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
        if (str_pool && len >= LIKE_PAR_MIN_ROWS_STR && ray_pool_total_workers(str_pool) >= 2) {
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
        /* Cell ids are positions in the COLUMN's domain (sym-domain
         * Phase 2).  Runtime: borrow the global snapshot.  FILE: LUT
         * sized by the domain's count, resolved via ray_sym_domain_str
         * (fused_group precedent).  Pre-flip: always runtime. */
        struct ray_sym_domain_s* dom = ray_sym_vec_domain(input);
        ray_t** sym_strings = NULL;
        uint32_t dict_n = 0;
        if (dom == ray_sym_runtime_domain()) {
            dom = NULL;
            ray_sym_strings_borrow(&sym_strings, &dict_n);
        } else {
            int64_t dn = ray_sym_domain_count(dom);
            dict_n = (dn > 0 && dn <= (int64_t)UINT32_MAX) ? (uint32_t)dn : 0;
        }
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
            /* Pass 1: mark used sym_ids.  Parallelised because for
             * high-cardinality text columns the seen-
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
            if (pool && len >= LIKE_PAR_MIN_ROWS_SYM && ray_pool_total_workers(pool) >= 2) {
                ray_pool_dispatch(pool, like_seen_fn, &sctx, len);
            } else {
                like_seen_fn(&sctx, 0, 0, len);
            }

            /* Pass 2: parallel pattern resolve over the dict range. */
            like_resolve_ctx_t rctx = {
                .sym_strings = sym_strings, .dom = dom, .seen = seen, .lut = lut,
                .pc = &pc, .use_simple = use_simple,
                .pat_str = pat_str, .pat_len = pat_len,
            };
            if (pool && (int64_t)dict_n >= 16384) {
                ray_pool_dispatch(pool, like_resolve_fn, &rctx, (int64_t)dict_n);
            } else {
                like_resolve_fn(&rctx, 0, 0, (int64_t)dict_n);
            }

            /* Pass 3: row projection — gather lut[sid] into the per-row
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
            if (pool && len >= LIKE_PAR_MIN_ROWS_SYM && ray_pool_total_workers(pool) >= 2) {
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
                ray_t* s = dom ? ray_sym_domain_str(dom, sym_id)
                         : (sym_strings && (uint64_t)sym_id < (uint64_t)dict_n)
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
    ray_t* input = exec_node(g, op_child(g, op, 0));
    ray_t* pat_v = exec_node(g, op_child(g, op, 1));
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
        /* Dictionary-cached fast path — see exec_like.  Cell ids are
         * positions in the COLUMN's domain (sym-domain Phase 2); the
         * LUT is sized by that domain's count and resolved through it
         * (the runtime singleton delegates to ray_sym_count/_str —
         * exact no-op pre-flip). */
        const void* base = ray_data(input);
        struct ray_sym_domain_s* dom = ray_sym_vec_domain(input);
        int64_t dn = ray_sym_domain_count(dom);
        uint32_t dict_n = (dn > 0 && dn <= (int64_t)UINT32_MAX) ? (uint32_t)dn : 0;
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
                    ray_t* s = ray_sym_domain_str(dom, sid);
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
                ray_t* s = ray_sym_domain_str(dom, sym_id);
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
    ray_t* input = exec_node(g, op_child(g, op, 0));
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
    /* STR/SYM have no null — every row is a value. */
    for (int64_t i = 0; i < len; i++) {
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
    ray_t* input = exec_node(g, op_child(g, op, 0));
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
            if (ray_vec_is_null(input, i)) {
                dst[i] = NULL_I64;
                result->attrs |= RAY_ATTR_HAS_NULLS;
            } else {
                dst[i] = (int64_t)elems[i].len;
            }
        }
    } else {
        for (int64_t i = 0; i < len; i++) {
            if (ray_vec_is_null(input, i)) {
                dst[i] = NULL_I64;
                result->attrs |= RAY_ATTR_HAS_NULLS;
            } else {
                const char* sp; size_t sl;
                sym_elem(input, i, &sp, &sl);
                dst[i] = (int64_t)sl;
            }
        }
    }
    ray_release(input);
    return result;
}

typedef struct {
    const ray_str_t* elems;
    const char* pool_data;
    const ray_strpat_t* pattern;
    int64_t* dst;
    ray_pool_t* pool;
    _Atomic(int) any_null;
} str_find_parts_ctx_t;

static void str_find_parts_task(void* vctx, uint32_t worker_id,
                                int64_t lo, int64_t hi) {
    (void)worker_id;
    str_find_parts_ctx_t* c = (str_find_parts_ctx_t*)vctx;
    bool any_null = false;
    for (int64_t i = lo; i < hi; i++) {
        if (((i - lo) & (RAY_MORSEL_ELEMS - 1)) == 0 &&
            pool_cancelled(c->pool))
            return;
        const char* sp = ray_str_t_ptr(&c->elems[i], c->pool_data);
        size_t sl = c->elems[i].len;
        size_t pos = 0;
        bool found = ray_strpat_find(c->pattern, sp, sl, &pos);
        c->dst[i] = found ? (int64_t)pos : NULL_I64;
        if (!found) any_null = true;
    }
    if (any_null)
        atomic_store_explicit(&c->any_null, 1, memory_order_relaxed);
}

static bool exec_string_atom_arg(ray_t* x, const char** out, size_t* out_len) {
    if (x->type == -RAY_STR) {
        *out = ray_str_ptr(x);
        *out_len = ray_str_len(x);
        return true;
    }
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        *out = s ? ray_str_ptr(s) : "";
        *out_len = s ? ray_str_len(s) : 0;
        return true;
    }
    return false;
}

ray_t* exec_str_find(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op_child(g, op, 0));
    ray_t* pat_v = exec_node(g, op_child(g, op, 1));
    if (!input || RAY_IS_ERR(input)) { if (pat_v && !RAY_IS_ERR(pat_v)) ray_release(pat_v); return input; }
    if (!pat_v || RAY_IS_ERR(pat_v)) { ray_release(input); return pat_v; }

    const char* pattern = NULL;
    size_t pattern_len = 0;
    if (!exec_string_atom_arg(pat_v, &pattern, &pattern_len)) {
        ray_release(input);
        ray_release(pat_v);
        return ray_error("type", "str-find: pattern must be string or symbol atom");
    }
    ray_strpat_t search;
    if (!ray_strpat_compile(pattern, pattern_len, &search)) {
        ray_release(input);
        ray_release(pat_v);
        return ray_error("domain", "str-find: '*' is not supported in search patterns");
    }

    if (input->type != RAY_STR && input->type != RAY_SYM) {
        ray_release(input);
        ray_release(pat_v);
        return ray_error("type", "str-find: expected string or symbol vector");
    }

    int64_t nrows = input->len;
    ray_t* result = ray_vec_new(RAY_I64, nrows);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(input);
        ray_release(pat_v);
        return result ? result : ray_error("oom", NULL);
    }
    result->len = nrows;
    int64_t* dst = (int64_t*)ray_data(result);

    if (input->type == RAY_STR) {
        const ray_str_t* elems = NULL;
        const char* pool_data = NULL;
        str_resolve(input, &elems, &pool_data);
        ray_pool_t* pool = nrows >= RAY_PARALLEL_THRESHOLD ? ray_pool_get() : NULL;
        str_find_parts_ctx_t ctx = {
            .elems = elems,
            .pool_data = pool_data,
            .pattern = &search,
            .dst = dst,
            .pool = pool
        };
        atomic_init(&ctx.any_null, 0);

        if (pool && ray_pool_total_workers(pool) >= 2)
            ray_pool_dispatch(pool, str_find_parts_task, &ctx, nrows);
        else
            str_find_parts_task(&ctx, 0, 0, nrows);

        if (pool_cancelled(pool)) {
            ray_release(result);
            ray_release(input);
            ray_release(pat_v);
            return ray_error("cancel", NULL);
        }
        if (atomic_load_explicit(&ctx.any_null, memory_order_relaxed))
            result->attrs |= RAY_ATTR_HAS_NULLS;
        ray_release(input);
        ray_release(pat_v);
        return result;
    }

    bool any_null = false;
    for (int64_t i = 0; i < nrows; i++) {
        if ((i & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(NULL)) {
            ray_release(result);
            ray_release(input);
            ray_release(pat_v);
            return ray_error("cancel", NULL);
        }
        const char* sp = NULL;
        size_t sl = 0;
        sym_elem(input, i, &sp, &sl);
        size_t pos = 0;
        bool found = ray_strpat_find(&search, sp, sl, &pos);
        dst[i] = found ? (int64_t)pos : NULL_I64;
        if (!found) any_null = true;
    }
    if (any_null) result->attrs |= RAY_ATTR_HAS_NULLS;
    ray_release(input);
    ray_release(pat_v);
    return result;
}

/* SUBSTR(str, start, len) — 1-based start */
static bool substr_scalar_arg(ray_t* v, int64_t* out) {
    if (!v || RAY_IS_ERR(v)) return false;
    if (ray_is_atom(v)) {
        if (RAY_ATOM_IS_NULL(v)) return false;
        switch (v->type) {
        case -RAY_I64:  *out = v->i64; return true;
        case -RAY_I32:  *out = (int64_t)v->i32; return true;
        case -RAY_I16:  *out = (int64_t)v->i16; return true;
        case -RAY_U8:
        case -RAY_BOOL: *out = (int64_t)v->u8; return true;
        case -RAY_F64:  *out = (int64_t)v->f64; return true;
        default: return false;
        }
    }
    if (!ray_is_vec(v) || v->len != 1 || (v->attrs & RAY_ATTR_HAS_NULLS))
        return false;
    switch (v->type) {
    case RAY_I64:  *out = ((int64_t*)ray_data(v))[0]; return true;
    case RAY_I32:  *out = (int64_t)((int32_t*)ray_data(v))[0]; return true;
    case RAY_I16:  *out = (int64_t)((int16_t*)ray_data(v))[0]; return true;
    case RAY_U8:
    case RAY_BOOL: *out = (int64_t)((uint8_t*)ray_data(v))[0]; return true;
    case RAY_F64:  *out = (int64_t)((double*)ray_data(v))[0]; return true;
    default: return false;
    }
}

static ray_t* substr_str_scalar_view(ray_t* input, int64_t start, int64_t length) {
    if (!input || input->type != RAY_STR || (input->attrs & RAY_ATTR_HAS_NULLS))
        return NULL;

    int64_t nrows = input->len;
    ray_t* result = ray_vec_new(RAY_STR, nrows);
    if (!result || RAY_IS_ERR(result)) return result ? result : ray_error("oom", NULL);
    result->len = nrows;

    const ray_str_t* src = NULL;
    const char* pool = NULL;
    str_resolve(input, &src, &pool);
    ray_t* owner = (input->attrs & RAY_ATTR_SLICE) ? input->slice_parent : input;
    ray_t* pool_obj = owner ? owner->str_pool : NULL;
    if (pool_obj && !RAY_IS_ERR(pool_obj)) {
        ray_retain(pool_obj);
        result->str_pool = pool_obj;
    }

    ray_str_t* dst = (ray_str_t*)ray_data(result);
    for (int64_t i = 0; i < nrows; i++) {
        const ray_str_t* s = &src[i];
        ray_str_t* d = &dst[i];
        memset(d, 0, sizeof(*d));

        int64_t st = start - 1;
        int64_t sl = (int64_t)s->len;
        if (st < 0) st = 0;
        if (st >= sl) continue;

        int64_t ln = length;
        if (ln < 0 || ln > sl - st) ln = sl - st;
        if (ln <= 0) continue;

        d->len = (uint32_t)ln;
        if (!ray_str_is_inline(s) && !pool) {
            ray_release(result);
            return NULL;
        }
        const char* sp = ray_str_t_ptr(s, pool) + st;
        if (ln <= RAY_STR_INLINE_MAX) {
            memcpy(d->data, sp, (size_t)ln);
        } else if (!ray_str_is_inline(s) && pool_obj) {
            if ((uint64_t)s->pool_off + (uint64_t)st > UINT32_MAX) {
                ray_release(result);
                return ray_error("range", "substr: pool offset exceeds %lld bytes", (long long)UINT32_MAX);
            }
            memcpy(d->prefix, sp, 4);
            d->pool_off = s->pool_off + (uint32_t)st;
            ray_str_t_cache_hash(d, pool);
        } else {
            ray_release(result);
            return NULL;
        }
    }

    return result;
}

static bool substr_len_at(ray_t* len_v, int64_t row, int64_t* out) {
    if (!len_v || !out) return false;
    if (len_v->attrs & RAY_ATTR_HAS_NULLS) {
        if (ray_vec_is_null(len_v, row)) return false;
    }
    switch (len_v->type) {
    case RAY_I64: {
        int64_t v = ((const int64_t*)ray_data(len_v))[row];
        if (v == NULL_I64) return false;
        *out = v;
        return true;
    }
    case RAY_I32: {
        int32_t v = ((const int32_t*)ray_data(len_v))[row];
        if (v == NULL_I32) return false;
        *out = (int64_t)v;
        return true;
    }
    default:
        return false;
    }
}

static ray_t* substr_str_scalar_start_len_view(ray_t* input,
                                               int64_t start,
                                               ray_t* len_v) {
    if (!input || input->type != RAY_STR || !len_v ||
        (input->attrs & RAY_ATTR_HAS_NULLS))
        return NULL;
    if (len_v->type != RAY_I64 && len_v->type != RAY_I32)
        return NULL;
    int64_t nrows = input->len;
    if (len_v->len != nrows)
        return NULL;

    ray_t* result = ray_vec_new(RAY_STR, nrows);
    if (!result || RAY_IS_ERR(result)) return result ? result : ray_error("oom", NULL);
    result->len = nrows;

    const ray_str_t* src = NULL;
    const char* pool = NULL;
    str_resolve(input, &src, &pool);
    ray_t* owner = (input->attrs & RAY_ATTR_SLICE) ? input->slice_parent : input;
    ray_t* pool_obj = owner ? owner->str_pool : NULL;
    if (pool_obj && !RAY_IS_ERR(pool_obj)) {
        ray_retain(pool_obj);
        result->str_pool = pool_obj;
    }

    ray_str_t* dst = (ray_str_t*)ray_data(result);
    int64_t st0 = start - 1;
    if (st0 < 0) st0 = 0;
    for (int64_t i = 0; i < nrows; i++) {
        ray_str_t* d = &dst[i];
        memset(d, 0, sizeof(*d));

        int64_t ln = 0;
        if (!substr_len_at(len_v, i, &ln)) {
            ray_vec_set_null(result, i, true);
            continue;
        }

        const ray_str_t* s = &src[i];
        int64_t sl = (int64_t)s->len;
        if (st0 >= sl) continue;

        if (ln < 0 || ln > sl - st0) ln = sl - st0;
        if (ln <= 0) continue;

        d->len = (uint32_t)ln;
        if (!ray_str_is_inline(s) && !pool) {
            ray_release(result);
            return NULL;
        }
        const char* sp = ray_str_t_ptr(s, pool) + st0;
        if (ln <= RAY_STR_INLINE_MAX) {
            memcpy(d->data, sp, (size_t)ln);
        } else if (!ray_str_is_inline(s) && pool_obj) {
            if ((uint64_t)s->pool_off + (uint64_t)st0 > UINT32_MAX) {
                ray_release(result);
                return ray_error("range", "substr: pool offset exceeds %lld bytes", (long long)UINT32_MAX);
            }
            memcpy(d->prefix, sp, 4);
            d->pool_off = s->pool_off + (uint32_t)st0;
            ray_str_t_cache_hash(d, pool);
        } else {
            ray_release(result);
            return NULL;
        }
    }

    return result;
}

/* True when a whole-column (scalar) start/length argument is null.  Such an
 * argument applies to every row, so the entire result is null — and a null
 * integer scalar is INT64_MIN, which must never reach the `scalar - 1`
 * subtraction in the row loop (signed-overflow UB).  Only scalars are tested
 * here; the per-row vector case is handled inline via ray_vec_is_null. */
static bool substr_scalar_is_null(ray_t* v) {
    if (!v) return false;
    if (ray_is_atom(v)) return RAY_ATOM_IS_NULL(v);
    if (ray_is_vec(v) && v->len == 1 && (v->attrs & RAY_ATTR_HAS_NULLS))
        return ray_vec_is_null(v, 0);
    return false;
}

ray_t* exec_substr(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op_child(g, op, 0));
    ray_t* start_v = exec_node(g, op_child(g, op, 1));
    if (!input || RAY_IS_ERR(input)) { if (start_v && !RAY_IS_ERR(start_v)) ray_release(start_v); return input; }
    if (!start_v || RAY_IS_ERR(start_v)) { ray_release(input); return start_v; }

    /* Get len arg from ext node's third input id */
    ray_op_ext_t* ext = find_ext(g, op->id);
    ray_t* len_v = exec_node(g, op_node(g, ext->third_in));
    if (!len_v || RAY_IS_ERR(len_v)) { ray_release(input); ray_release(start_v); return len_v; }

    int64_t nrows = input->len;
    bool is_str = (input->type == RAY_STR);

    if (is_str) {
        int64_t s_const = 0, l_const = 0;
        if (substr_scalar_arg(start_v, &s_const) &&
            substr_scalar_arg(len_v, &l_const)) {
            ray_t* view = substr_str_scalar_view(input, s_const, l_const);
            if (view) {
                ray_release(input);
                ray_release(start_v);
                ray_release(len_v);
                return view;
            }
        }
        if (substr_scalar_arg(start_v, &s_const) &&
            !ray_is_atom(len_v) && len_v->len == nrows) {
            ray_t* view = substr_str_scalar_start_len_view(input, s_const, len_v);
            if (view) {
                ray_release(input);
                ray_release(start_v);
                ray_release(len_v);
                return view;
            }
        }
    }

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

    /* A scalar (whole-column) null start or length makes every result row
     * null.  The per-row check below only inspects the vector (s_data /
     * l_data) case, so a scalar NULL_I64 start would otherwise reach the
     * `s_scalar - 1` subtraction and overflow (signed-overflow UB). */
    bool s_all_null = !s_data && !s_data_i32 && substr_scalar_is_null(start_v);
    bool l_all_null = !l_data && !l_data_i32 && substr_scalar_is_null(len_v);

    for (int64_t i = 0; i < nrows; i++) {
        /* Propagate null — from input, start, or length */
        /* STR input has no null; start/len are I64 and can be null. */
        if (s_all_null || l_all_null ||
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
    ray_t* input = exec_node(g, op_child(g, op, 0));
    ray_t* from_v = exec_node(g, op_child(g, op, 1));
    if (!input || RAY_IS_ERR(input)) { if (from_v && !RAY_IS_ERR(from_v)) ray_release(from_v); return input; }
    if (!from_v || RAY_IS_ERR(from_v)) { ray_release(input); return from_v; }

    ray_op_ext_t* ext = find_ext(g, op->id);
    ray_t* to_v = exec_node(g, op_node(g, ext->third_in));
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

    /* STR/SYM have no null — every row is a value. */
    for (int64_t i = 0; i < nrows; i++) {
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
    if (raw_nargs < 2 || raw_nargs > 255) return ray_error("arity", "concat: expected 2 to 255 arguments, got %lld", (long long)raw_nargs);
    int n_args = (int)raw_nargs;

    /* Evaluate all inputs */
    ray_t* args_stack[16];
    ray_t** args = args_stack;
    ray_t* args_hdr = NULL;
    if (n_args > 16) {
        args = (ray_t**)scratch_calloc(&args_hdr, (size_t)n_args * sizeof(ray_t*));
        if (!args) return ray_error("oom", NULL);
    }

    args[0] = exec_node(g, op_child(g, op, 0));
    args[1] = exec_node(g, op_child(g, op, 1));
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
            /* SYM atoms can be null (sym 0); STR/SYM vecs cannot. */
            if (ray_is_atom(args[a]) && RAY_ATOM_IS_NULL(args[a])) { any_null = true; break; }
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
