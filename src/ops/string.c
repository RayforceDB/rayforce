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
#include "ops/hash.h"      /* ray_hash_bytes: the batch intern of substr results */
#include "lang/format.h"   /* ray_type_name (error context) */
#include "lang/env.h"
#include "lang/internal.h" /* ray_like_fn (list-of-strings delegate) */
#include "store/colrange.h"  /* a scanned column bound without decoding (#1645) */
#include "store/enccol.h"
#include "store/encfmt.h"

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
/* The dictionary sweep's rows: fewer name at most as many entries, which
 * cost no more read in row order, so the residency probe is not paid. */
#define LIKE_SWEEP_MIN_ROWS    65536

/* SYM-LIKE row worker: one pass over the rows.  lut[sid] is 0 while the
 * symbol is unresolved, 1 for "no match", 2 for "match".  A worker that
 * meets an unresolved sid runs the matcher and publishes the answer, so the
 * pattern runs once per distinct symbol the rows actually name — no seen
 * pass, no sweep of the dictionary (which is the whole runtime symbol table,
 * usually far larger than the column's vocabulary).  Two workers may resolve
 * the same sid at the same moment: they store the same value, the stores
 * are relaxed atomics, and the duplicate work is bounded by the worker
 * count.  Serially this is the two-load-one-store loop the direct builtin
 * always ran; the fused predicate (fused_pred.c) keeps its LUT the same way.
 * Width-specialised on the SYM dictionary width. */
typedef struct {
    const void* base;
    uint8_t*    dst;
    uint8_t*    lut;                        /* [dict_n], zeroed: 0 unresolved, 1 no, 2 yes */
    uint64_t    dict_n;
    int         sym_w;
    ray_t**     sym_strings;                /* runtime-domain snapshot, or NULL */
    /* sym_strings == NULL ⇒ FILE-domain column: resolve each sid through
     * `dom`; when pinned, `raw` reads the file prefix straight from the
     * mapping (no atom materialisation, no lock). */
    struct ray_sym_domain_s*   dom;
    ray_sym_domain_raw_t       raw;
    bool                       raw_ok;
    const ray_glob_compiled_t* pc;
    bool                       use_simple;
    const char*                pat_str;
    size_t                     pat_len;
    /* What an unknown symbol answers — an id past the dictionary, or one
     * with no string — is the pattern matched against "", as for an atom
     * whose string cannot be resolved. */
    uint8_t                    empty_match;
    /* Optional rowsel — when non-NULL, rows filtered out earlier are left
     * untouched (rowsel_refine reads pred[r] for surviving rows only). */
    const uint8_t*  sel_flg;
    const uint32_t* sel_offs;
    const uint16_t* sel_idx;
    uint32_t        sel_n_segs;
    /* Dictionary sweep, pass 1: the rows only mark the codes they name
     * (lut 0 -> 3) and resolve nothing; see like_sweep_fn. */
    bool            mark;
    /* Dictionary sweep, pass 2: walk the dictionary from its end. */
    bool            sweep_rev;
} like_rows_ctx_t;

static inline uint8_t like_rows_resolve(const like_rows_ctx_t* x, uint64_t sid) {
    uint8_t st = __atomic_load_n(&x->lut[sid], __ATOMIC_RELAXED);
    if (x->mark) {
        /* only the file's entries are swept; one appended past them stays
         * unresolved and is resolved in the row pass */
        if (!st && (int64_t)sid < x->raw.count)
            __atomic_store_n(&x->lut[sid], (uint8_t)3, __ATOMIC_RELAXED);
        return 0;
    }
    if (st) return (uint8_t)(st - 1);
    const char* sp = NULL;
    size_t sl = 0;
    if (x->raw_ok && (int64_t)sid < x->raw.count) {
        sp = ray_sym_domain_raw_str(&x->raw, (int64_t)sid, &sl);
    } else {
        ray_t* str = x->sym_strings ? x->sym_strings[sid]
                                    : ray_sym_domain_str(x->dom, (int64_t)sid);
        if (str) { sp = ray_str_ptr(str); sl = ray_str_len(str); }
    }
    uint8_t m = x->empty_match;
    if (sp)
        m = (x->use_simple ? ray_glob_match_compiled(x->pc, sp, sl)
                           : ray_glob_match(sp, sl, x->pat_str, x->pat_len)) ? 1 : 0;
    __atomic_store_n(&x->lut[sid], (uint8_t)(m + 1), __ATOMIC_RELAXED);
    return m;
}

#define LIKE_ROW(LOAD) do {                                            \
        uint64_t sid = (uint64_t)(LOAD);                                \
        dst[r] = (sid < dict_n) ? like_rows_resolve(x, sid) : x->empty_match; \
    } while (0)
#define LIKE_ROW_W(W) do {                                               \
        if ((W) == RAY_SYM_W8)       LIKE_ROW(((const uint8_t*)x->base)[r]);  \
        else if ((W) == RAY_SYM_W16) LIKE_ROW(((const uint16_t*)x->base)[r]); \
        else if ((W) == RAY_SYM_W32) LIKE_ROW(((const uint32_t*)x->base)[r]); \
        else                         LIKE_ROW(((const int64_t*)x->base)[r]);  \
    } while (0)

static void like_rows_fn(void* vctx, uint32_t worker_id,
                         int64_t start, int64_t end) {
    (void)worker_id;
    like_rows_ctx_t* x = (like_rows_ctx_t*)vctx;
    uint8_t* dst = x->dst;
    const uint64_t dict_n = x->dict_n;
    const int sym_w = x->sym_w;

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
                for (int64_t r = s_lo; r < s_hi; r++) LIKE_ROW_W(sym_w);
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
                LIKE_ROW_W(sym_w);
            }
        }
        return;
    }

    switch (sym_w) {
    case RAY_SYM_W8: {
        const uint8_t* d = (const uint8_t*)x->base;
        for (int64_t r = start; r < end; r++) LIKE_ROW(d[r]);
        break;
    }
    case RAY_SYM_W16: {
        const uint16_t* d = (const uint16_t*)x->base;
        for (int64_t r = start; r < end; r++) LIKE_ROW(d[r]);
        break;
    }
    case RAY_SYM_W32: {
        const uint32_t* d = (const uint32_t*)x->base;
        for (int64_t r = start; r < end; r++) LIKE_ROW(d[r]);
        break;
    }
    case RAY_SYM_W64:
    default: {
        const int64_t* d = (const int64_t*)x->base;
        for (int64_t r = start; r < end; r++) LIKE_ROW(d[r]);
        break;
    }
    }
}
#undef LIKE_ROW_W
#undef LIKE_ROW

/* Dictionary sweep, pass 2 (FILE domain, pinned raw view): resolves every
 * code pass 1 marked, in ascending code order.  The codes of a task are a
 * contiguous run of the symfile, so the kernel reads it ahead (MADV_WILLNEED
 * for the run) instead of faulting it in row order, and a page is read once
 * however many rows name its strings.  [start, end) is a code range. */
static void like_sweep_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    like_rows_ctx_t* x = (like_rows_ctx_t*)vctx;
    uint8_t* lut = x->lut;
    if (end > (int64_t)x->dict_n) end = (int64_t)x->dict_n;
    if (x->sweep_rev) {   /* the task ranges tile [0, dict_n); mirror them */
        int64_t lo = (int64_t)x->dict_n - end, hi = (int64_t)x->dict_n - start;
        start = lo; end = hi;
    }
    if (end > x->raw.count) end = x->raw.count;
    if (start >= end) return;
    /* Marked codes, estimated from every 16th: the run is asked for ahead
     * when they are dense enough that most of its pages are read anyway.
     * The last block has no next offset; its tail is left to read-ahead. */
    int64_t need = 0;
    for (int64_t i = start; i < end; i += 16) need += (lut[i] == 3);
    need *= 16;
    int64_t kl = start / RAY_SYM_INDEX_STRIDE;
    int64_t kh = (end - 1) / RAY_SYM_INDEX_STRIDE + 1;
    if (need * 32 >= end - start && x->raw.ib && kh * RAY_SYM_INDEX_STRIDE < x->raw.count)
        ray_vm_advise_willneed((void*)(x->raw.map + x->raw.ib[kl].off),
                               (size_t)(x->raw.ib[kh].off - x->raw.ib[kl].off));
    for (int64_t sid = start; sid < end; sid++) {
        if (lut[sid] != 3) continue;
        size_t sl = 0;
        const char* sp = ray_sym_domain_raw_str(&x->raw, sid, &sl);
        uint8_t m = x->empty_match;
        if (sp)
            m = (x->use_simple ? ray_glob_match_compiled(x->pc, sp, sl)
                               : ray_glob_match(sp, sl, x->pat_str, x->pat_len)) ? 1 : 0;
        lut[sid] = (uint8_t)(m + 1);
    }
}

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
        if (ray_pool_par_dispatch_ok(pool, seg_len, LIKE_PAR_MIN_ROWS_STR)) {
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
                                 uint8_t empty_match, int64_t total_len) {
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
    uint8_t* lut = dict_n > 0 ? (uint8_t*)scratch_calloc(&lut_hdr, (size_t)dict_n) : NULL;

    ray_pool_t* pool = ray_pool_get();
    if (lut) {
        /* One LUT for every segment: the partitions share the domain, so a
         * symbol resolved in one segment is known in the next. */
        ray_sym_domain_raw_t raw;
        bool raw_ok = dom ? ray_sym_domain_raw_pin(dom, &raw) : false;
        int64_t out_off = 0;
        for (int64_t s = 0; s < input->len; s++) {
            ray_t* seg = segs[s];
            if (!seg) continue;
            int64_t seg_len = seg->len;
            like_rows_ctx_t rctx = {
                .base = ray_data(seg), .dst = dst + out_off, .lut = lut,
                .dict_n = (uint64_t)dict_n,
                .sym_w = (int)(seg->attrs & RAY_SYM_W_MASK),
                .sym_strings = sym_strings, .dom = dom, .raw = raw, .raw_ok = raw_ok,
                .pc = pc, .use_simple = use_simple,
                .pat_str = pat_str, .pat_len = pat_len,
                .empty_match = empty_match,
            };
            if (ray_pool_par_dispatch_ok(pool, seg_len, LIKE_PAR_MIN_ROWS_SYM)) {
                ray_pool_dispatch(pool, like_rows_fn, &rctx, seg_len);
            } else {
                like_rows_fn(&rctx, 0, 0, seg_len);
            }
            out_off += seg_len;
        }
        if (raw_ok) ray_sym_domain_raw_unpin(dom);
        scratch_free(lut_hdr);
        return;
    }

    if (lut_hdr) scratch_free(lut_hdr);

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
            if (!str) { dst[out_off + i] = empty_match; continue; }
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

    ray_col_meta_t cm;
    if (!ray_table_col_meta(scan_tbl, ext->sym, &cm) || !RAY_IS_PARTED(cm.type))
        return exec_node(g, input_op);
    ray_t* col = ray_table_col(scan_tbl, ext->sym);
    if (col && RAY_IS_ERR(col)) return col;
    if (!col) return exec_node(g, input_op);
    {
        int8_t base = (int8_t)RAY_PARTED_BASETYPE(cm.type);
        if (base == RAY_STR || RAY_IS_SYM(base)) {
            ray_retain(col);
            return col;
        }
    }
    return exec_node(g, input_op);
}

/* A text cursor over a STR or SYM vector, flat or parted (a parted one's
 * missing segments hold no rows), or a STR/SYM atom repeated: row by row,
 * the text of each cell (a SYM cell through its vector's domain). */
typedef struct {
    ray_t*  atom;                /* a STR or SYM atom, the same text every row */
    ray_t*  held;                /* a SYM atom's text, owned */
    ray_t** segs;                /* the vectors: one, or a parted column's segments */
    int64_t n_segs, seg, row;
    const ray_str_t* elems;      /* the current STR segment */
    const char* pool;
} like_text_cur_t;

static bool like_text_kind(ray_t* v, bool atom_ok) {
    if (!v) return false;
    if (v->type == -RAY_STR || v->type == -RAY_SYM) return atom_ok;
    int8_t t = RAY_IS_PARTED(v->type) ? (int8_t)RAY_PARTED_BASETYPE(v->type) : v->type;
    return t == RAY_STR || RAY_IS_SYM(t);
}

static int64_t like_text_rows(ray_t* v) {
    if (v->type < 0) return -1;
    return RAY_IS_PARTED(v->type) ? parted_row_count(v) : v->len;
}

static void like_text_open(like_text_cur_t* c, ray_t** one) {
    ray_t* v = *one;
    memset(c, 0, sizeof *c);
    if (v->type < 0) { c->atom = v; return; }
    if (RAY_IS_PARTED(v->type)) { c->segs = (ray_t**)ray_data(v); c->n_segs = v->len; }
    else { c->segs = one; c->n_segs = 1; }
}

static void like_text_close(like_text_cur_t* c) {
    if (c->held) ray_release(c->held);
    c->held = NULL;
}

/* The text of the next row; false past the last. */
static bool like_text_next(like_text_cur_t* c, const char** sp, size_t* sl) {
    if (c->atom) {
        if (c->atom->type == -RAY_SYM) {
            if (!c->held) c->held = ray_sym_str(c->atom->i64);   /* released by like_text_close */
            *sp = c->held ? ray_str_ptr(c->held) : ""; *sl = c->held ? ray_str_len(c->held) : 0;
        } else {
            *sp = ray_str_ptr(c->atom); *sl = ray_str_len(c->atom);
        }
        return true;
    }
    while (c->seg < c->n_segs && (!c->segs[c->seg] || c->row >= c->segs[c->seg]->len)) {
        c->seg++; c->row = 0; c->elems = NULL;
    }
    if (c->seg >= c->n_segs) return false;
    ray_t* seg = c->segs[c->seg];
    if (seg->type == RAY_STR) {
        if (!c->elems) str_resolve(seg, &c->elems, &c->pool);
        *sp = ray_str_t_ptr(&c->elems[c->row], c->pool);
        *sl = c->elems[c->row].len;
    } else {
        ray_t* t = ray_sym_vec_cell(seg, c->row);              /* borrowed */
        *sp = t ? ray_str_ptr(t) : ""; *sl = t ? ray_str_len(t) : 0;
    }
    c->row++;
    return true;
}

/* (like x p) with p a STR vector (a column): row i of x matched against
 * pattern i, x a STR or SYM vector of as many rows (flat or parted) or a
 * STR/SYM atom matched against every pattern (#1615).  `ci`: ilike.
 * Borrows both; returns an owned BOOL vector or an error. */
ray_t* ray_like_rows(ray_t* x, ray_t* pat, bool ci) {
    const char* fn = ci ? "ilike" : "like";
    if (!like_text_kind(x, true))
        return ray_error("type", "%s: expected a string or symbol, or a vector of them, got %s",
                         fn, ray_type_name(x->type));
    int64_t n = like_text_rows(pat);
    int64_t xn = like_text_rows(x);
    if (xn >= 0 && xn != n)
        return ray_error("length", "%s: %lld values against %lld patterns", fn, (long long)xn, (long long)n);
    ray_t* out = ray_vec_new(RAY_BOOL, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;
    uint8_t* dst = (uint8_t*)ray_data(out);
    like_text_cur_t cx, cp;
    ray_t* xs[1] = { x };
    ray_t* ps[1] = { pat };
    like_text_open(&cx, xs);
    like_text_open(&cp, ps);
    for (int64_t i = 0; i < n; i++) {
        const char *s = "", *p = "";
        size_t sl = 0, pl = 0;
        if (!like_text_next(&cx, &s, &sl) || !like_text_next(&cp, &p, &pl)) {
            like_text_close(&cx);
            ray_release(out);
            return ray_error("length", "%s: the values and the patterns have different lengths", fn);
        }
        dst[i] = (ci ? ray_glob_match_ci(s, sl, p, pl) : ray_glob_match(s, sl, p, pl)) ? 1 : 0;
    }
    like_text_close(&cx);
    return out;
}

/* Is `pat` a pattern column: a STR vector, flat or parted? */
bool ray_like_pat_rows(ray_t* pat) {
    if (!pat || pat->type < 0) return false;
    int8_t t = RAY_IS_PARTED(pat->type) ? (int8_t)RAY_PARTED_BASETYPE(pat->type) : pat->type;
    return t == RAY_STR;
}

/* LIKE over a whole STR / SYM column (flat or parted): the parallel kernel
 * shared by the DAG executor (exec_like) and the direct builtin (ray_like_fn),
 * so `(like col pat)` and `where: (like col pat)` run the same code — one
 * pattern resolve per distinct symbol, the row passes spread over the worker
 * pool.  `selection` is the executor's rowsel, or NULL for every row.
 * `pat_v` is a string atom (both callers check it; a string column takes
 * ray_like_rows, #1615).
 * Borrows `input` and `pat_v`; returns an owned BOOL vector. */
ray_t* ray_like_vec(ray_t* input, ray_t* pat_v, ray_t* selection) {
    int8_t in_type = input->type;
    bool in_parted = RAY_IS_PARTED(in_type);
    int8_t base_type = in_parted ? (int8_t)RAY_PARTED_BASETYPE(in_type) : in_type;

    /* Get pattern string */
    const char* pat_str = ray_str_ptr(pat_v);
    size_t pat_len = ray_str_len(pat_v);

    /* Pre-compile pattern into the simple-shape form when possible — the
     * substring/prefix/suffix branches drive memmem/memcmp directly,
     * roughly an order of magnitude faster than the iterative matcher
     * for the very common `*literal*` shape. */
    ray_glob_compiled_t pc = ray_glob_compile(pat_str, pat_len);
    bool use_simple = pc.shape != RAY_GLOB_SHAPE_NONE;
    uint8_t empty_match = (use_simple ? ray_glob_match_compiled(&pc, "", 0)
                                      : ray_glob_match("", 0, pat_str, pat_len)) ? 1 : 0;

    int64_t len = in_parted ? parted_row_count(input) : input->len;
    ray_t* result = ray_vec_new(RAY_BOOL, len);
    if (!result || RAY_IS_ERR(result)) return result;   /* input/pat_v are borrowed */
    result->len = len;
    uint8_t* dst = (uint8_t*)ray_data(result);

    if (in_parted && base_type == RAY_STR) {
        exec_like_parted_str(input, dst, &pc, use_simple, pat_str, pat_len);
    } else if (in_parted && RAY_IS_SYM(base_type)) {
        exec_like_parted_sym(input, dst, &pc, use_simple, pat_str, pat_len, empty_match, len);
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
        if (ray_pool_par_dispatch_ok(str_pool, len, LIKE_PAR_MIN_ROWS_STR)) {
            ray_pool_dispatch(str_pool, str_like_par_fn, &lctx, len);
        } else {
            str_like_par_fn(&lctx, 0, 0, len);
        }
    } else if (RAY_IS_SYM(in_type)) {
        /* Dictionary-cached path: the pattern runs once per distinct symbol
         * the rows name, in the same parallel pass that writes the rows
         * (like_rows_fn).  Cell ids are positions in the COLUMN's domain.
         * Runtime domain: borrow the global string snapshot (lock-free per
         * sid).  FILE domain: LUT sized by the domain's count, strings from
         * the pinned mapping. */
        const void* base = ray_data(input);
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
        uint8_t* lut = dict_n > 0 ? (uint8_t*)scratch_calloc(&lut_hdr, (size_t)dict_n) : NULL;
        if (lut) {
            like_rows_ctx_t rctx = {
                .base = base, .dst = dst, .lut = lut, .dict_n = (uint64_t)dict_n,
                .sym_w = (int)(input->attrs & RAY_SYM_W_MASK),
                .sym_strings = sym_strings, .dom = dom,
                .pc = &pc, .use_simple = use_simple,
                .pat_str = pat_str, .pat_len = pat_len,
                .empty_match = empty_match,
            };
            if (selection) {
                ray_rowsel_t* sm = ray_rowsel_meta(selection);
                rctx.sel_flg    = ray_rowsel_flags(selection);
                rctx.sel_offs   = ray_rowsel_offsets(selection);
                rctx.sel_idx    = ray_rowsel_idx(selection);
                rctx.sel_n_segs = sm->n_segs;
            }
            int64_t work = len;
            if (selection) work = ray_rowsel_meta(selection)->total_pass;
            rctx.raw_ok = dom ? ray_sym_domain_raw_pin(dom, &rctx.raw) : false;
            ray_pool_t* pool = ray_pool_get();
            bool par = ray_pool_par_dispatch_ok(pool, len, LIKE_PAR_MIN_ROWS_SYM);
            /* A big FILE dictionary: mark the codes the rows name, resolve
             * them in dictionary order, then fill the rows from the table. */
            if (rctx.raw_ok && dict_n >= 65536 && work >= LIKE_SWEEP_MIN_ROWS &&
                ray_sym_domain_raw_cold(&rctx.raw)) {
                rctx.mark = true;
                if (par) ray_pool_dispatch(pool, like_rows_fn, &rctx, len);
                else like_rows_fn(&rctx, 0, 0, len);
                rctx.mark = false;
                /* Successive sweeps of one dictionary alternate direction: when
                 * the dictionary is larger than the page cache, the pages the
                 * last sweep read last are still cached when the next one
                 * starts there, instead of every pass evicting what the next
                 * needs first. */
                {
                    static _Atomic(uintptr_t) last_dom;
                    static _Atomic int last_rev;
                    uintptr_t d = (uintptr_t)dom;
                    rctx.sweep_rev = (atomic_exchange(&last_dom, d) == d) && !atomic_load(&last_rev);
                    atomic_store(&last_rev, rctx.sweep_rev ? 1 : 0);
                }
                if (par) ray_pool_dispatch_grain(pool, like_sweep_fn, &rctx, dict_n, 8 * RAY_MORSEL_ELEMS);
                else like_sweep_fn(&rctx, 0, 0, dict_n);
            }
            if (par) {
                ray_pool_dispatch(pool, like_rows_fn, &rctx, len);
            } else {
                like_rows_fn(&rctx, 0, 0, len);
            }
            if (rctx.raw_ok) ray_sym_domain_raw_unpin(dom);
            scratch_free(lut_hdr);
        } else {
            /* OOM building the LUT: fall back to per-row scan. */
            if (lut_hdr) scratch_free(lut_hdr);
            for (int64_t i = 0; i < len; i++) {
                int64_t sym_id = ray_read_sym(base, i, in_type, input->attrs);
                ray_t* s = dom ? ray_sym_domain_str(dom, sym_id)
                         : (sym_strings && (uint64_t)sym_id < (uint64_t)dict_n)
                           ? sym_strings[sym_id] : NULL;
                if (!s) { dst[i] = empty_match; continue; }
                const char* sp = ray_str_ptr(s);
                size_t sl = ray_str_len(s);
                dst[i] = (use_simple
                          ? ray_glob_match_compiled(&pc, sp, sl)
                          : ray_glob_match(sp, sl, pat_str, pat_len)) ? 1 : 0;
            }
        }
    }

    return result;
}

static ray_t* exec_like_with(ray_graph_t* g, ray_t* input, ray_t* pat_v);

/* ---- LIKE over a generation-2 STR column, a vector at a time (#1645) ----
 *
 * A scanned STR column whose segments (one, or a parted column's) are
 * generation-2 files not yet materialized is read where it is mapped
 * instead of decoded whole: a STR_DICT vector's codes are mapped through
 * the dictionary, each entry matched once per query (a byte per entry:
 * 0 unknown, else match + 1); any other vector is decoded alone into the
 * task's buffer and matched cell by cell.  Every encoded segment's vectors
 * are one dispatch, spread over the worker pool.  A plain (or already
 * materialized) segment takes the plain kernel.  The answer is the plain
 * kernel's: the same text, row for row.
 *
 * Which segments are read by vector: one whose vectors are all STR_DICT
 * (or CONST) always, as that is cheaper than any decode.  The others (FSST
 * and RAW vectors, which a repeated query would decode again) are
 * materialized as before when their whole decode fits the decode cache's
 * budget (ray_enccol_cache_trim keeps a quarter of the anonymous
 * watermark), so a repeat reads them cached; only when it does not fit are
 * they read by vector, and nothing is decoded whole. */
typedef struct {
    ray_enccol_t*           e;
    int64_t                 row0;       /* the segment's first row in the output */
    const ray_enc_sdict_t*  sd;         /* its STR dictionary, or NULL */
    _Atomic(uint8_t)*       lut;        /* sd->n entries, or NULL */
} like_enc_seg_t;

typedef struct {
    like_enc_seg_t*            segs;
    int64_t                    n_segs;
    const int64_t*             vbase;     /* n_segs + 1: each segment's first task (vector) */
    uint8_t*                   dst;
    const ray_glob_compiled_t* pc;
    bool                       use_simple;
    const char*                pat_str;
    size_t                     pat_len;
    ray_scan_fail_t            fail;
} like_enc_ctx_t;

static inline uint8_t like_enc_match(const like_enc_ctx_t* x, const char* sp, size_t sl) {
    return (x->use_simple ? ray_glob_match_compiled(x->pc, sp, sl)
                          : ray_glob_match(sp, sl, x->pat_str, x->pat_len)) ? 1 : 0;
}

static void like_enc_fn(void* vctx, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    like_enc_ctx_t* x = (like_enc_ctx_t*)vctx;
    ray_str_t cells[RAY_ENC_VEC];
    char* pool = NULL;
    uint64_t pool_cap = 0;
    int64_t s = 0;
    while (s + 1 < x->n_segs && x->vbase[s + 1] <= start) s++;
    for (int64_t g = start; g < end; g++) {
        if (ray_scan_fail_skip(&x->fail, g)) break;
        while (x->vbase[s + 1] <= g) s++;
        like_enc_seg_t* sg = &x->segs[s];
        int64_t v = g - x->vbase[s];
        int n = ray_enc_vec_len(sg->e->len, v);
        uint8_t* o = x->dst + sg->row0 + v * RAY_ENC_VEC;
        ray_t* err = NULL;
        if (sg->lut) {
            ray_enc_vview_t vv;
            int r = ray_enccol_vview(sg->e, v, &vv, &err);
            if (r < 0) { ray_scan_fail_note(&x->fail, g, err); break; }
            if (r == 1 && vv.enc == RAY_ENC_STR_DICT) {
                for (int i = 0; i < n; i++) {
                    uint32_t c = vv.codes[i];
                    uint8_t m = atomic_load_explicit(&sg->lut[c], memory_order_relaxed);
                    if (!m) {
                        const char* sp; uint32_t sl;
                        ray_enc_sdict_entry(sg->sd, c, &sp, &sl);
                        m = (uint8_t)(like_enc_match(x, sp, sl) + 1);
                        atomic_store_explicit(&sg->lut[c], m, memory_order_relaxed);
                    }
                    o[i] = (uint8_t)(m - 1);
                }
                continue;
            }
        }
        uint64_t pb = 0;
        err = ray_enccol_vec_pool_bytes(sg->e, v, &pb);
        if (!err && pb > pool_cap) {
            char* np = (char*)ray_alloc_raw((size_t)pb);
            if (!np) err = ray_error("oom", NULL);
            else { if (pool) ray_free_raw(pool); pool = np; pool_cap = pb; }
        }
        if (!err) err = ray_enccol_decode_vec(sg->e, v, cells, pool, 0);
        if (err) { ray_scan_fail_note(&x->fail, g, err); break; }
        for (int i = 0; i < n; i++)
            o[i] = like_enc_match(x, ray_str_t_ptr(&cells[i], pool), cells[i].len);
    }
    if (pool) ray_free_raw(pool);
}

/* The plain kernel over one plain STR vector, into dst. */
static void like_plain_str(ray_t* v, uint8_t* dst, const ray_glob_compiled_t* pc, bool use_simple,
                           const char* pat_str, size_t pat_len) {
    const ray_str_t* elems; const char* pool_data;
    str_resolve(v, &elems, &pool_data);
    str_like_par_ctx_t lctx = {
        .elems = elems, .pool_data = pool_data, .dst = dst, .pc = pc,
        .use_simple = use_simple, .pat_str = pat_str, .pat_len = pat_len,
    };
    ray_pool_t* pool = ray_pool_get();
    if (ray_pool_par_dispatch_ok(pool, v->len, LIKE_PAR_MIN_ROWS_STR))
        ray_pool_dispatch(pool, str_like_par_fn, &lctx, v->len);
    else
        str_like_par_fn(&lctx, 0, 0, v->len);
}

/* Every vector of the single-file column e is STR_DICT or CONST: reading
 * it by vector decodes no string. */
static bool like_enc_dict_only(const ray_enccol_t* e) {
    for (int64_t c = 0; c < e->n_chunks; c++) {
        int64_t rows = e->len - c * RAY_ENC_CHUNK;
        if (rows > RAY_ENC_CHUNK) rows = RAY_ENC_CHUNK;
        int nv = (int)((rows + RAY_ENC_VEC - 1) / RAY_ENC_VEC);
        const ray_enc_desc_t* d = ray_enccol_descs(e, c);
        for (int j = 0; j < nv; j++)
            if (d[j].encoding != RAY_ENC_STR_DICT && d[j].encoding != RAY_ENC_CONST) return false;
    }
    return true;
}

/* The bytes a whole decode of e holds: its cells and its string pool. */
static int64_t like_enc_decode_bytes(const ray_enccol_t* e) {
    return e->len * (int64_t)sizeof(ray_str_t) + (int64_t)e->hdr.str_pool_bytes;
}

/* Test hook: the segments like_encoded_str read by vector, since process
 * start. */
static _Atomic(uint64_t) g_like_enc_by_vector;
uint64_t ray_like_test_by_vector(void) { return atomic_load(&g_like_enc_by_vector); }

/* (like col pat) with col a scanned STR column holding a generation-2
 * segment not yet materialized and pat a STR atom: 1 with *out the BOOL
 * answer (or an owned error), 0 when the column is not such a column (the
 * caller reads it as before).  Decodes nothing whole. */
static int like_encoded_str(ray_t* tbl, int64_t sym, ray_t* pat_v, ray_t** out) {
    *out = NULL;
    ray_col_src_t src;
    memset(&src, 0, sizeof src);
    ray_t* err = ray_table_col_src(tbl, sym, &src);
    if (err) { ray_error_free(err); return 0; }
    int rc = 0;
    ray_t** segv = NULL; int64_t ns = 0; const int64_t* off = NULL;
    int64_t one_off[2];
    ray_t* one[1];
    like_enc_seg_t* es = NULL;
    int64_t* vbase = NULL;
    if (!src.obj || src.type != RAY_STR) goto done;
    if (src.parted) {
        if (!ray_col_src_parted_segs(&src, &ns, &segv, &off, &err)) {
            if (err) { *out = err; rc = 1; }
            goto done;
        }
    } else {
        one[0] = src.obj; segv = one; ns = 1;
        one_off[0] = 0; one_off[1] = src.len; off = one_off;
    }
    /* every segment a plain STR vector or a single-file generation-2 one of
     * its rows; at least one of those not materialized */
    int64_t n_enc = 0;
    for (int64_t i = 0; i < ns; i++) {
        int64_t sl = off[i + 1] - off[i];
        ray_t* sg = segv[i];
        if (!sg) { if (sl) goto done; continue; }
        if (sg->type == RAY_ENCCOL) {
            ray_enccol_t* e = ray_enccol_of(sg);
            if (!e || e->kind != RAY_ENCCOL_SINGLE || e->type != RAY_STR || e->len != sl) goto done;
            if (!ray_enccol_decoded_peek(sg)) n_enc++;
        } else if (sg->type != RAY_STR || sg->len != sl) {
            goto done;
        }
    }
    if (n_enc == 0) goto done;
    rc = 1;
    es = (like_enc_seg_t*)ray_calloc_raw((size_t)n_enc * sizeof *es);
    vbase = (int64_t*)ray_alloc_raw((size_t)(n_enc + 1) * sizeof *vbase);
    ray_t* res = ray_vec_new(RAY_BOOL, src.len);
    if (!es || !vbase || !res || RAY_IS_ERR(res)) {
        *out = (res && RAY_IS_ERR(res)) ? res : ray_error("oom", NULL);
        if (res && !RAY_IS_ERR(res)) ray_release(res);
        goto done;
    }
    res->len = src.len;
    uint8_t* dst = (uint8_t*)ray_data(res);
    const char* pat_str = ray_str_ptr(pat_v);
    size_t pat_len = ray_str_len(pat_v);
    ray_glob_compiled_t pc = ray_glob_compile(pat_str, pat_len);
    bool use_simple = pc.shape != RAY_GLOB_SHAPE_NONE;
    /* the segments a repeat would decode again: whole, as before, when
     * their decodes fit the cache's budget */
    int64_t need = 0;
    for (int64_t i = 0; i < ns; i++) {
        ray_t* sg = segv[i];
        if (!sg || off[i + 1] == off[i] || sg->type != RAY_ENCCOL || ray_enccol_decoded_peek(sg)) continue;
        if (!like_enc_dict_only(ray_enccol_of(sg))) need += like_enc_decode_bytes(ray_enccol_of(sg));
    }
    int64_t wm = ray_heap_anon_watermark();
    bool whole = need > 0 && (wm <= 0 || ray_enccol_cache_bytes() + need <= wm / 4);
    int64_t k = 0, nv = 0;
    for (int64_t i = 0; i < ns && !err; i++) {
        ray_t* sg = segv[i];
        if (!sg || off[i + 1] == off[i]) continue;
        ray_t* plain = sg->type == RAY_ENCCOL ? ray_enccol_decoded_peek(sg) : sg;
        if (!plain && whole && !like_enc_dict_only(ray_enccol_of(sg))) {
            plain = ray_enccol_materialize(sg);          /* borrowed, cached */
            if (plain && RAY_IS_ERR(plain)) { err = plain; break; }
        }
        if (plain) { like_plain_str(plain, dst + off[i], &pc, use_simple, pat_str, pat_len); continue; }
        atomic_fetch_add_explicit(&g_like_enc_by_vector, 1, memory_order_relaxed);
        like_enc_seg_t* t = &es[k];
        t->e = ray_enccol_of(sg);
        t->row0 = off[i];
        err = ray_enccol_strdict(t->e, &t->sd);
        if (!err && t->sd && t->sd->n > 0) {
            t->lut = (_Atomic(uint8_t)*)ray_calloc_raw((size_t)t->sd->n);
            if (!t->lut) err = ray_error("oom", NULL);
        }
        vbase[k] = nv;
        nv += ray_enc_nvecs(t->e->len);
        k++;
    }
    vbase[k] = nv;
    if (!err) {
        like_enc_ctx_t x = {
            .segs = es, .n_segs = k, .vbase = vbase, .dst = dst, .pc = &pc,
            .use_simple = use_simple, .pat_str = pat_str, .pat_len = pat_len,
        };
        ray_scan_fail_init(&x.fail);
        ray_pool_t* pool = ray_pool_get();
        if (nv >= 2 && nv <= (int64_t)UINT32_MAX && ray_pool_par_dispatch_ok(pool, src.len, LIKE_PAR_MIN_ROWS_STR))
            ray_pool_dispatch_n(pool, like_enc_fn, &x, (uint32_t)nv);
        else
            like_enc_fn(&x, 0, 0, nv);
        err = ray_scan_fail_finish(&x.fail, pool);
    }
    for (int64_t i = 0; i < k; i++) if (es[i].lut) ray_free_raw((void*)es[i].lut);
    if (err) { ray_release(res); *out = err; }
    else *out = res;
done:
    if (es) ray_free_raw(es);
    if (vbase) ray_free_raw(vbase);
    ray_col_src_release(&src);
    return rc;
}

/* The table and column a SCAN reads, without reading it: 1 when found. */
static int like_scan_col(ray_graph_t* g, ray_op_t* input_op, ray_t** tbl, int64_t* sym) {
    if (!input_op || input_op->opcode != OP_SCAN) return 0;
    ray_op_ext_t* ext = find_ext(g, input_op->id);
    if (!ext) return 0;
    uint16_t stored_table_id = 0;
    memcpy(&stored_table_id, ext->base.pad, sizeof(uint16_t));
    ray_t* t = NULL;
    if (stored_table_id > 0 && g->tables && (stored_table_id - 1) < g->n_tables)
        t = g->tables[stored_table_id - 1];
    else
        t = g->table;
    if (!t) return 0;
    *tbl = t; *sym = ext->sym;
    return 1;
}

ray_t* exec_like(ray_graph_t* g, ray_op_t* op) {
    {
        /* a generation-2 STR column is matched where it is mapped (#1645) */
        ray_t* st = NULL; int64_t ssym = 0;
        if (like_scan_col(g, op_child(g, op, 0), &st, &ssym)) {
            ray_col_meta_t cm;
            int8_t bt = 0;
            if (ray_table_col_meta(st, ssym, &cm))
                bt = RAY_IS_PARTED(cm.type) ? (int8_t)RAY_PARTED_BASETYPE(cm.type) : cm.type;
            if (bt == RAY_STR) {
                ray_t* pat = exec_node(g, op_child(g, op, 1));
                if (!pat || RAY_IS_ERR(pat)) return pat;
                if (pat->type == -RAY_STR) {
                    ray_t* r = NULL;
                    if (like_encoded_str(st, ssym, pat, &r)) { ray_release(pat); return r; }
                }
                ray_t* input = exec_like_input(g, op_child(g, op, 0));
                return exec_like_with(g, input, pat);
            }
        }
    }
    ray_t* input = exec_like_input(g, op_child(g, op, 0));
    ray_t* pat_v = exec_node(g, op_child(g, op, 1));
    return exec_like_with(g, input, pat_v);
}

/* exec_like over its evaluated operands; consumes both. */
static ray_t* exec_like_with(ray_graph_t* g, ray_t* input, ray_t* pat_v) {
    if (!input || RAY_IS_ERR(input)) { if (pat_v && !RAY_IS_ERR(pat_v)) ray_release(pat_v); return input; }
    if (!pat_v || RAY_IS_ERR(pat_v)) { ray_release(input); return pat_v; }
    /* A query compiles any expression into the pattern slot.  A string
     * column is matched row by row; a string atom takes the kernels below;
     * anything else (a symbol, a number, a SYM column) is the builtin's
     * type error.  Reading it as a string crashed or matched nothing
     * (#1615). */
    if (ray_like_pat_rows(pat_v)) {
        ray_t* r = ray_like_rows(input, pat_v, false);
        ray_release(input); ray_release(pat_v);
        return r;
    }
    if (pat_v->type != -RAY_STR) {
        int8_t pt = pat_v->type;
        ray_release(input); ray_release(pat_v);
        return ray_error("type", "like: pattern must be a STR atom or a STR vector, got %s", ray_type_name(pt));
    }

    int8_t in_type = input->type;
    bool in_parted = RAY_IS_PARTED(in_type);
    int8_t base_type = in_parted ? (int8_t)RAY_PARTED_BASETYPE(in_type) : in_type;

    /* Shapes this executor doesn't own — scalar subjects, list-of-atom
     * columns (the legacy STRL splayed-load shape), unsupported types —
     * delegate to the direct builtin BEFORE reading input->len or
     * allocating: an atom's SSO bytes alias ->len, so the result alloc
     * below would request a garbage capacity, and the old fallthrough
     * memset made unsupported predicates silently match nothing. */
    bool vec_shape = in_parted ? (base_type == RAY_STR || RAY_IS_SYM(base_type))
                               : (in_type == RAY_STR || RAY_IS_SYM(in_type));
    if (!vec_shape) {
        ray_t* r = ray_like_fn(input, pat_v);
        ray_release(input); ray_release(pat_v);
        return r;
    }

    ray_t* result = ray_like_vec(input, pat_v, g->selection);
    ray_release(input); ray_release(pat_v);
    return result;
}

/* Case-insensitive LIKE — same syntax as `like`, ASCII-fold both sides. */

/* ILIKE over the shapes exec_ilike's vector loops don't own — scalar
 * subjects, list-of-atom columns (the legacy STRL splayed-load shape) —
 * plus the type error for everything else.  ilike has no direct builtin
 * to delegate to, so this mirrors ray_like_fn's atom and list branches
 * with the case-folded matcher. */
static ray_t* ilike_non_vec(ray_t* input, const char* pat_str, size_t pat_len) {
    if (input->type == -RAY_STR || input->type == -RAY_SYM) {
        ray_t* sym_str = NULL;
        const char* s; size_t sl;
        if (input->type == -RAY_SYM) {
            sym_str = ray_sym_str(input->i64);
            s  = sym_str ? ray_str_ptr(sym_str) : "";
            sl = sym_str ? ray_str_len(sym_str) : 0;
        } else {
            s  = ray_str_ptr(input);
            sl = ray_str_len(input);
        }
        bool m = ray_glob_match_ci(s, sl, pat_str, pat_len);
        if (sym_str) ray_release(sym_str);
        return ray_bool(m);
    }
    if (input->type == RAY_LIST) {
        int64_t n = input->len;
        ray_t* result = ray_vec_new(RAY_BOOL, n);
        if (!result || RAY_IS_ERR(result)) return result;
        result->len = n;
        uint8_t* out = (uint8_t*)ray_data(result);
        ray_t** items = (ray_t**)ray_data(input);
        for (int64_t i = 0; i < n; i++) {
            ray_t* it = items[i];
            ray_t* sym_str = NULL;
            const char* s; size_t sl;
            if (it && it->type == -RAY_STR) {
                s  = ray_str_ptr(it);
                sl = ray_str_len(it);
            } else if (it && it->type == -RAY_SYM) {
                sym_str = ray_sym_str(it->i64);
                s  = sym_str ? ray_str_ptr(sym_str) : "";
                sl = sym_str ? ray_str_len(sym_str) : 0;
            } else {
                ray_release(result);
                return ray_error("type", "ilike: list items must be string or symbol atoms");
            }
            out[i] = ray_glob_match_ci(s, sl, pat_str, pat_len) ? 1 : 0;
            if (sym_str) ray_release(sym_str);
        }
        return result;
    }
    return ray_error("type", "ilike: expects string or symbol");
}

ray_t* exec_ilike(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op_child(g, op, 0));
    ray_t* pat_v = exec_node(g, op_child(g, op, 1));
    if (!input || RAY_IS_ERR(input)) { if (pat_v && !RAY_IS_ERR(pat_v)) ray_release(pat_v); return input; }
    if (!pat_v || RAY_IS_ERR(pat_v)) { ray_release(input); return pat_v; }
    /* As for like (#1615): a string column row by row, else a string atom. */
    if (ray_like_pat_rows(pat_v)) {
        ray_t* r = ray_like_rows(input, pat_v, true);
        ray_release(input); ray_release(pat_v);
        return r;
    }
    if (pat_v->type != -RAY_STR) {
        int8_t pt = pat_v->type;
        ray_release(input); ray_release(pat_v);
        return ray_error("type", "ilike: pattern must be a STR atom or a STR vector, got %s", ray_type_name(pt));
    }

    const char* pat_str = ray_str_ptr(pat_v);
    size_t pat_len = ray_str_len(pat_v);

    int8_t in_type = input->type;
    /* Dispatch before reading input->len or allocating — an atom's SSO
     * bytes alias ->len, and the old fallthrough memset made ilike over
     * an unsupported column type silently match nothing. */
    if (in_type != RAY_STR && !RAY_IS_SYM(in_type)) {
        ray_t* r = ilike_non_vec(input, pat_str, pat_len);
        ray_release(input); ray_release(pat_v);
        return r;
    }

    int64_t len = input->len;
    ray_t* result = ray_vec_new(RAY_BOOL, len);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(input); ray_release(pat_v);
        return result;
    }
    result->len = len;
    uint8_t* dst = (uint8_t*)ray_data(result);

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
            ray_sym_domain_raw_t raw;
            bool raw_ok = ray_sym_domain_raw_pin(dom, &raw);
            for (int64_t i = 0; i < len; i++) {
                int64_t sid = ray_read_sym(base, i, in_type, input->attrs);
                if ((uint64_t)sid >= (uint64_t)dict_n) { dst[i] = 0; continue; }
                if (!seen[sid]) {
                    const char* sp = NULL;
                    size_t sl = 0;
                    if (raw_ok && sid < raw.count) {
                        sp = ray_sym_domain_raw_str(&raw, sid, &sl);
                    } else {
                        ray_t* s = ray_sym_domain_str(dom, sid);
                        if (s) { sp = ray_str_ptr(s); sl = ray_str_len(s); }
                    }
                    lut[sid] = sp ? (ray_glob_match_ci(sp, sl, pat_str, pat_len) ? 1 : 0) : 0;
                    seen[sid] = 1;
                }
                dst[i] = lut[sid];
            }
            if (raw_ok) ray_sym_domain_raw_unpin(dom);
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
    }

    ray_release(input); ray_release(pat_v);
    return result;
}

/* ============================================================================
 * String functions: UPPER, LOWER, TRIM, STRLEN, SUBSTR, REPLACE, CONCAT
 *
 * Over a SYM column, UPPER / LOWER / TRIM, SUBSTR at scalar bounds, REPLACE
 * and CONCAT with constants run once per distinct code (sym_map_by_code);
 * the STR paths and the per-row-argument shapes run their row loops.
 * ============================================================================ */

/* UPPER / LOWER / TRIM: exec_string_unary, after the SYM reader and the
 * per-code machinery it uses (sym_reader_t, sym_map_by_code, below). */

/* LENGTH — SYM → I64 */
typedef struct {
    ray_t*                   input;
    int64_t*                 dst;
    struct ray_sym_domain_s* dom;
    ray_sym_domain_raw_t     raw;
    bool                     raw_ok;
    _Atomic(int)             any_null;
} strlen_sym_ctx_t;
static void strlen_sym_task(void* vctx, uint32_t worker_id, int64_t lo, int64_t hi) {
    (void)worker_id;
    strlen_sym_ctx_t* c = (strlen_sym_ctx_t*)vctx;
    ray_t* input = c->input;
    const void* base = ray_data(input);
    bool any_null = false;
    for (int64_t i = lo; i < hi; i++) {
        if (ray_vec_is_null(input, i)) { c->dst[i] = NULL_I64; any_null = true; continue; }
        int64_t sid = ray_read_sym(base, i, input->type, input->attrs);
        if (c->raw_ok && sid >= 0 && sid < c->raw.count) {
            int64_t sl = ray_sym_domain_raw_len(&c->raw, sid);
            c->dst[i] = sl < 0 ? 0 : sl;
        } else {
            const char* sp; size_t sl;
            sym_elem(input, i, &sp, &sl);
            c->dst[i] = (int64_t)sl;
        }
    }
    if (any_null) atomic_store_explicit(&c->any_null, 1, memory_order_relaxed);
}
/* Lengths of a SYM vector into a pre-sized I64 vector.  A FILE domain's
 * entries carry their length as a u32 prefix in the mapping — read that by
 * position, no atom, no lock, in parallel; anything else (runtime domain,
 * appended positions) resolves the way it always did.  A null cell (the
 * empty symbol) is a null length, as the vector op returns. */
void ray_sym_strlen_into(ray_t* input, ray_t* result) {
    int64_t len = input->len;
    strlen_sym_ctx_t c = { .input = input, .dst = (int64_t*)ray_data(result), .raw_ok = false };
    c.dom = ray_sym_vec_domain(input);
    c.raw_ok = c.dom ? ray_sym_domain_raw_pin(c.dom, &c.raw) : false;
    atomic_store_explicit(&c.any_null, 0, memory_order_relaxed);
    ray_pool_t* pool = ray_pool_get();
    if (pool && len >= RAY_PARALLEL_THRESHOLD)
        ray_pool_dispatch(pool, strlen_sym_task, &c, len);
    else
        strlen_sym_task(&c, 0, 0, len);
    if (c.raw_ok) ray_sym_domain_raw_unpin(c.dom);
    if (atomic_load_explicit(&c.any_null, memory_order_relaxed))
        result->attrs |= RAY_ATTR_HAS_NULLS;
}
ray_t* exec_strlen(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op_child(g, op, 0));
    if (!input || RAY_IS_ERR(input)) return input;
    /* An atom's len aliases its payload; see exec_string_unary. */
    if (input->type < 0) {
        ray_t* r = ray_strlen_fn(input);
        ray_release(input);
        return r;
    }

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
        ray_sym_strlen_into(input, result);
    }
    ray_release(input);
    return result;
}

typedef struct {
    const ray_str_t* elems;
    const char* pool_data;
    const char* needle;
    size_t needle_len;
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
        bool found = ray_str_find_literal(sp, sl, c->needle, c->needle_len, &pos);
        c->dst[i] = found ? (int64_t)pos : NULL_I64;
        if (!found) any_null = true;
    }
    if (any_null)
        atomic_store_explicit(&c->any_null, 1, memory_order_relaxed);
}

/* ---- a SYM vector's strings, read by pool workers ----------------------
 * A FILE domain's entries come from the pinned mapping (a compressed one
 * through the calling thread's ring, good for the one read); any other
 * domain's through its atoms, under the table's lock. */
typedef struct {
    ray_t*                   v;
    struct ray_sym_domain_s* dom;
    ray_sym_domain_raw_t     raw;
    bool                     raw_ok;
    bool                     runtime;  /* the global table: its atoms are read in blocks, under one lock */
} sym_reader_t;

static void sym_reader_init(sym_reader_t* r, ray_t* v) {
    r->v = v;
    r->dom = ray_sym_vec_domain(v);
    r->raw_ok = r->dom ? ray_sym_domain_raw_pin(r->dom, &r->raw) : false;
    r->runtime = !r->dom || r->dom == ray_sym_runtime_domain();
}

/* The strings at positions pos[0..n) (n <= SYM_READ_BLOCK). */
#define SYM_READ_BLOCK 256
static void sym_reader_at(const sym_reader_t* r, int64_t pos, const char** sp, size_t* sl);
static void sym_reader_block(const sym_reader_t* r, const int64_t* pos, int n,
                             const char** sp, size_t* sl) {
    if (r->runtime) {
        ray_t* atoms[SYM_READ_BLOCK];
        ray_sym_str_batch(pos, n, atoms);
        for (int i = 0; i < n; i++) {
            if (!atoms[i]) { sp[i] = ""; sl[i] = 0; continue; }
            sp[i] = ray_str_ptr(atoms[i]);
            sl[i] = ray_str_len(atoms[i]);
        }
        return;
    }
    /* a file's entries: plain ones only (a compressed entry's bytes live in
     * the thread's ring until its next few reads — callers with such a
     * domain read one at a time) */
    for (int i = 0; i < n; i++) sym_reader_at(r, pos[i], &sp[i], &sl[i]);
}

static void sym_reader_done(sym_reader_t* r) {
    if (r->raw_ok) ray_sym_domain_raw_unpin(r->dom);
}

/* The string at position `pos` of the vector's domain ("" when absent). */
static void sym_reader_at(const sym_reader_t* r, int64_t pos,
                          const char** sp, size_t* sl) {
    if (r->raw_ok && pos >= 0 && pos < r->raw.count) {
        const char* p = ray_sym_domain_raw_str(&r->raw, pos, sl);
        if (p) { *sp = p; return; }
    }
    ray_t* atom = ray_sym_domain_str(r->dom, pos);
    if (!atom) { *sp = ""; *sl = 0; return; }
    *sp = ray_str_ptr(atom);
    *sl = ray_str_len(atom);
}

/* ---- a string function of a SYM column, once per distinct code ----------
 * UPPER / LOWER / TRIM, SUBSTR at scalar bounds, REPLACE (constant pattern
 * and replacement) and CONCAT of one SYM column with constants are
 * functions of the cell's string alone, so the rows only say WHICH entries
 * of the input's domain take part: a parallel pass marks the codes the rows
 * name (a bit per domain position), the marked entries are transformed in
 * rank order (parallel, in chunks of bounded bytes) and interned as a batch
 * into a domain of the result's own (ray_sym_domain_new; the batch dedupes,
 * so entries that map to one string — "ABC" and "abc" under lower — share
 * one position), and a last parallel pass maps each row through rank →
 * position.  The global table never grows by a derived value (the
 * derived-key path of group-by, query.c, keeps its keys the same way), and
 * the function runs K times for K distinct codes, not once per row.
 * Width: the narrowest that holds the result's dictionary.
 * A few distinct results (SYM_MAP_GLOBAL_MAX: a country or language
 * column, a substring that few share) are then interned into the global
 * table, as the row loop did, and the result is in the runtime domain: the
 * growth is bounded by the constant, and what reads the result next — a
 * compare with another such column, a group — takes its same-domain path.
 * 1 when served, 0 when not (a code outside the domain, no memory: the
 * caller runs its row loop, as before), -1 cancelled. */
#define SYM_MAP_MIN_ROWS   4096                    /* below: the row loop (bounded interns, no bitmap of the domain) */
#define SYM_MAP_GLOBAL_MAX 256                     /* distinct results interned globally, not into a domain of their own */
#define SYM_MAP_CHUNK      ((int64_t)1 << 20)      /* ranks per intern batch */
#define SYM_MAP_CHUNK_BYTES ((int64_t)64 << 20)    /* output bytes per intern batch */

/* The function: its opcode and constant arguments. */
typedef struct {
    uint16_t    opc;          /* OP_UPPER, OP_LOWER, OP_TRIM, OP_SUBSTR, OP_REPLACE, OP_CONCAT */
    int64_t     st, ln;       /* SUBSTR: 0-based start; ln < 0: to the end */
    const char* from; size_t from_len;   /* REPLACE */
    const char* to;   size_t to_len;
    const char* pre;  size_t pre_len;    /* CONCAT: the constants before and after the column */
    const char* post; size_t post_len;
} sym_map_op_t;

/* A bound on the output's bytes from the input's length: the length pass
 * sizes the chunk buffer by it without reading the strings. */
static inline uint64_t sym_map_bound(const sym_map_op_t* m, uint64_t sl) {
    switch (m->opc) {
        case OP_CONCAT:  return sl + m->pre_len + m->post_len;
        case OP_REPLACE:
            /* every from_len bytes a match replaced by to_len (the row loop's worst) */
            if (m->from_len > 0 && m->to_len > m->from_len)
                return (sl / m->from_len) * m->to_len + sl % m->from_len;
            return sl;
        default:         return sl;
    }
}

/* The function of one string, written into `out` (at least
 * sym_map_bound(sl) bytes); its length. */
static inline size_t sym_map_apply(const sym_map_op_t* m, const char* sp, size_t sl, char* out) {
    switch (m->opc) {
        case OP_UPPER:
            for (size_t j = 0; j < sl; j++) out[j] = (char)toupper((unsigned char)sp[j]);
            return sl;
        case OP_LOWER:
            for (size_t j = 0; j < sl; j++) out[j] = (char)tolower((unsigned char)sp[j]);
            return sl;
        case OP_TRIM: {
            size_t start = 0, end = sl;
            while (start < sl && isspace((unsigned char)sp[start])) start++;
            while (end > start && isspace((unsigned char)sp[end - 1])) end--;
            memcpy(out, sp + start, end - start);
            return end - start;
        }
        case OP_SUBSTR: {
            if ((uint64_t)m->st >= sl) return 0;
            int64_t ln = m->ln;
            if (ln < 0 || ln > (int64_t)(sl - (size_t)m->st)) ln = (int64_t)sl - m->st;
            memcpy(out, sp + m->st, (size_t)ln);
            return (size_t)ln;
        }
        case OP_REPLACE: {
            size_t bi = 0;
            for (size_t j = 0; j < sl; ) {
                if (m->from_len > 0 && j + m->from_len <= sl && memcmp(sp + j, m->from, m->from_len) == 0) {
                    memcpy(out + bi, m->to, m->to_len); bi += m->to_len;
                    j += m->from_len;
                } else {
                    out[bi++] = sp[j++];
                }
            }
            return bi;
        }
        default: {   /* OP_CONCAT */
            memcpy(out, m->pre, m->pre_len);
            memcpy(out + m->pre_len, sp, sl);
            memcpy(out + m->pre_len + sl, m->post, m->post_len);
            return m->pre_len + sl + m->post_len;
        }
    }
}

typedef struct {
    sym_reader_t   rd;
    const sym_map_op_t* m;
    uint64_t*      bm;        /* codes present, a bit per domain position */
    int64_t        words;
    int64_t        dn;
    ray_pool_t*    pool;
    _Atomic(int)   bad;       /* a code outside the domain */
    const int32_t* base;      /* rank of each word's first set bit */
    uint32_t*      olen;      /* [K] the bound on the entry's output, by rank */
    /* the chunk of ranks [r0, r0 + n) over words [w0, w1) */
    int64_t        w0, r0;
    const uint64_t* off;      /* [n + 1] byte offset of each rank's output in `bytes` */
    char*          bytes;
    const char**   strs;
    size_t*        lens;
    uint32_t*      hashes;
    const int32_t* ids;       /* result position by rank */
    void*          dst;
    uint8_t        dst_attrs;
} sym_map_ctx_t;

static void sym_map_mark(void* vctx, uint32_t wid, int64_t lo, int64_t hi) {
    (void)wid;
    sym_map_ctx_t* c = (sym_map_ctx_t*)vctx;
    const void* base = ray_data(c->rd.v);
    uint8_t attrs = c->rd.v->attrs;
    for (int64_t i = lo; i < hi; i++) {
        if (((i - lo) & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(c->pool)) return;
        int64_t pos = ray_read_sym(base, i, RAY_SYM, attrs);
        if (pos < 0 || pos >= c->dn) { atomic_store_explicit(&c->bad, 1, memory_order_relaxed); return; }
        uint64_t bit = (uint64_t)1 << (pos & 63);
        if (!(__atomic_load_n(&c->bm[pos >> 6], __ATOMIC_RELAXED) & bit))
            __atomic_fetch_or(&c->bm[pos >> 6], bit, __ATOMIC_RELAXED);
    }
}

static inline uint32_t sym_map_olen(const sym_map_op_t* m, uint64_t sl) {
    uint64_t b = sym_map_bound(m, sl);
    return b > UINT32_MAX ? UINT32_MAX : (uint32_t)b;
}

/* The passes over the marked entries are dispatched in UNITS of eight
 * positions (a byte of the bitmap), so a vocabulary of a million codes
 * splits into tasks of RAY_MORSEL_ELEMS units (8192 entries), not two
 * tasks of half the words each.  A task's range is a whole number of
 * grains, each a multiple of eight units (ray_pool_dispatch_grain's
 * contract), and the unit count a multiple of eight: the words a range
 * covers are whole, [lo / 8, hi / 8). */
#define SYM_MAP_UNIT_LOG 3

static void sym_map_run_units(ray_pool_t* pool, void (*fn)(void*, uint32_t, int64_t, int64_t),
                              sym_map_ctx_t* c, int64_t n_units) {
    if (ray_pool_par_dispatch_ok(pool, n_units << SYM_MAP_UNIT_LOG, RAY_PARALLEL_THRESHOLD))
        ray_pool_dispatch_grain(pool, fn, c, n_units, RAY_MORSEL_ELEMS);
    else
        fn(c, 0, 0, n_units);
}

/* The bound on each marked entry's output, by rank: a file's lengths from
 * its index (no string read), any other's from its atom.  Over units. */
static void sym_map_len(void* vctx, uint32_t wid, int64_t lo, int64_t hi) {
    (void)wid;
    sym_map_ctx_t* c = (sym_map_ctx_t*)vctx;
    int64_t w0 = lo >> SYM_MAP_UNIT_LOG, w1 = (hi + 7) >> SYM_MAP_UNIT_LOG;
    for (int64_t w = w0; w < w1; w++) {
        if (((w - w0) & 63) == 0 && pool_cancelled(c->pool)) return;
        uint64_t bits = c->bm[w];
        if (!bits) continue;
        int64_t k = c->base[w];
        int64_t ids[64]; const char* sp[64]; size_t sl[64];
        int n = 0;
        while (bits) { ids[n++] = w * 64 + __builtin_ctzll(bits); bits &= bits - 1; }
        if (c->rd.raw_ok && ids[n - 1] < c->rd.raw.count) {
            for (int j = 0; j < n; j++, k++) {
                int64_t l = ray_sym_domain_raw_len(&c->rd.raw, ids[j]);
                c->olen[k] = sym_map_olen(c->m, l < 0 ? 0 : (uint64_t)l);
            }
            continue;
        }
        if (c->rd.runtime) {
            // cppcheck-suppress uninitvar // bits is nonzero here, so the loop above fills ids[0, n)
            sym_reader_block(&c->rd, ids, n, sp, sl);
        } else {
            for (int j = 0; j < n; j++) sym_reader_at(&c->rd, ids[j], &sp[j], &sl[j]);
        }
        for (int j = 0; j < n; j++, k++) c->olen[k] = sym_map_olen(c->m, sl[j]);
    }
}

/* The chunk's entries transformed into `bytes` at their offsets, with the
 * batch's pointers, lengths and hashes.  A compressed file's entries are
 * decoded one at a time (the thread's ring holds few), any other's read in
 * blocks.  An entry whose output would not fit its bound (the bound is
 * exact or larger; only a 4 GB string is clipped) is cut at the bound.
 * Over the chunk's units, from the chunk's first word. */
static void sym_map_cut(void* vctx, uint32_t wid, int64_t lo, int64_t hi) {
    (void)wid;
    sym_map_ctx_t* c = (sym_map_ctx_t*)vctx;
    bool one_at_a_time = c->rd.raw_ok && c->rd.raw.z;
    int64_t w0 = c->w0 + (lo >> SYM_MAP_UNIT_LOG), w1 = c->w0 + ((hi + 7) >> SYM_MAP_UNIT_LOG);
    for (int64_t w = w0; w < w1; w++) {
        if (((w - w0) & 63) == 0 && pool_cancelled(c->pool)) return;
        uint64_t bits = c->bm[w];
        if (!bits) continue;
        int64_t k = (int64_t)c->base[w] - c->r0;
        int64_t ids[64]; const char* sp[64]; size_t sl[64];
        int n = 0;
        while (bits) { ids[n++] = w * 64 + __builtin_ctzll(bits); bits &= bits - 1; }
        if (!one_at_a_time) {
            // cppcheck-suppress uninitvar // bits is nonzero here, so the loop above fills ids[0, n)
            sym_reader_block(&c->rd, ids, n, sp, sl);
        }
        for (int j = 0; j < n; j++, k++) {
            if (one_at_a_time) sym_reader_at(&c->rd, ids[j], &sp[j], &sl[j]);
            char* out = c->bytes + c->off[k];
            size_t room = (size_t)(c->off[k + 1] - c->off[k]);
            size_t l = sl[j];
            while (l > 0 && sym_map_bound(c->m, l) > room) l--;
            size_t ol = sym_map_apply(c->m, sp[j], l, out);
            c->strs[k] = out;
            c->lens[k] = ol;
            c->hashes[k] = (uint32_t)ray_hash_bytes(out, ol);
        }
    }
}

static void sym_map_fill(void* vctx, uint32_t wid, int64_t lo, int64_t hi) {
    (void)wid;
    sym_map_ctx_t* c = (sym_map_ctx_t*)vctx;
    const void* base = ray_data(c->rd.v);
    uint8_t attrs = c->rd.v->attrs;
    const uint64_t* bm = c->bm;
    const int32_t* rank0 = c->base;
    const int32_t* ids = c->ids;
    uint8_t w = (uint8_t)(c->dst_attrs & RAY_SYM_W_MASK);
#define SYM_MAP_FILL_LOOP(T)                                                          \
    do {                                                                              \
        T* d = (T*)c->dst;                                                            \
        for (int64_t i = lo; i < hi; i++) {                                           \
            if (((i - lo) & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(c->pool)) return; \
            int64_t pos = ray_read_sym(base, i, RAY_SYM, attrs);                      \
            int64_t wd = pos >> 6;                                                    \
            uint64_t below = bm[wd] & (((uint64_t)1 << (pos & 63)) - 1);              \
            d[i] = (T)ids[rank0[wd] + __builtin_popcountll(below)];                   \
        }                                                                             \
    } while (0)
    if (w == RAY_SYM_W8)       SYM_MAP_FILL_LOOP(uint8_t);
    else if (w == RAY_SYM_W16) SYM_MAP_FILL_LOOP(uint16_t);
    else if (w == RAY_SYM_W32) SYM_MAP_FILL_LOOP(uint32_t);
    else                       SYM_MAP_FILL_LOOP(int64_t);
#undef SYM_MAP_FILL_LOOP
}

static void sym_map_run(ray_pool_t* pool, void (*fn)(void*, uint32_t, int64_t, int64_t),
                        sym_map_ctx_t* c, int64_t n) {
    if (ray_pool_par_dispatch_ok(pool, n, RAY_PARALLEL_THRESHOLD)) ray_pool_dispatch(pool, fn, c, n);
    else                                                           fn(c, 0, 0, n);
}

/* Runs of the per-code path that served (O(1)); (.sys.mem)'s sym-map-runs,
 * so a test can assert how many times a query transformed a column. */
static _Atomic(int64_t) ray_string_sym_map_runs_ctr = 0;
int64_t ray_string_sym_map_runs(void) {
    return atomic_load_explicit(&ray_string_sym_map_runs_ctr, memory_order_relaxed);
}

static int sym_map_by_code(ray_t* input, const sym_map_op_t* m, ray_t** out) {
    int64_t nrows = input->len;
    ray_pool_t* pool = ray_pool_get();
    sym_map_ctx_t c;
    memset(&c, 0, sizeof c);
    sym_reader_init(&c.rd, input);
    c.m = m;
    int rc = 0;
    ray_t *bm_hdr = NULL, *base_hdr = NULL, *olen_hdr = NULL, *ids_hdr = NULL,
          *chunk_hdr = NULL, *off_hdr = NULL, *bytes_hdr = NULL;
    struct ray_sym_domain_s* kdom = NULL;
    c.dn = ray_sym_domain_count(c.rd.dom);
    if (c.dn <= 0 || c.dn > ((int64_t)1 << 31)) goto out;
    c.words = (c.dn + 63) >> 6;
    c.pool  = pool;
    c.bm = (uint64_t*)scratch_calloc(&bm_hdr, (size_t)c.words * sizeof(uint64_t));
    if (!c.bm) goto out;
    atomic_init(&c.bad, 0);
    sym_map_run(pool, sym_map_mark, &c, nrows);
    ray_profile_tick("sym map: codes marked");
    if (pool_cancelled(pool)) { rc = -1; goto out; }
    if (atomic_load_explicit(&c.bad, memory_order_relaxed)) goto out;
    int32_t* base = (int32_t*)scratch_alloc(&base_hdr, (size_t)c.words * sizeof(int32_t));
    if (!base) goto out;
    int64_t K = 0;
    for (int64_t w = 0; w < c.words; w++) { base[w] = (int32_t)K; K += __builtin_popcountll(c.bm[w]); }
    if (K <= 0 || K > INT32_MAX) goto out;
    c.base = base;
    c.olen = (uint32_t*)scratch_alloc(&olen_hdr, (size_t)K * sizeof(uint32_t));
    if (!c.olen) goto out;
    sym_map_run_units(pool, sym_map_len, &c, c.words << SYM_MAP_UNIT_LOG);
    ray_profile_tick("sym map: lengths read");
    if (pool_cancelled(pool)) { rc = -1; goto out; }
    int32_t* ids = (int32_t*)scratch_alloc(&ids_hdr, (size_t)K * sizeof(int32_t));
    if (!ids) goto out;
    int64_t cap = K < SYM_MAP_CHUNK ? K : SYM_MAP_CHUNK;    /* ranks a chunk holds at most */
    /* the 8-byte arrays first, so each stays aligned whatever `cap` is */
    size_t per = sizeof(const char*) + sizeof(int64_t) + sizeof(size_t) + sizeof(uint32_t);
    char* chunk = (char*)scratch_alloc(&chunk_hdr, (size_t)cap * per);
    if (!chunk) goto out;
    c.strs   = (const char**)chunk;
    int64_t* pos = (int64_t*)(c.strs + cap);
    c.lens   = (size_t*)(pos + cap);
    c.hashes = (uint32_t*)(c.lens + cap);
    uint64_t* off = (uint64_t*)scratch_alloc(&off_hdr, (size_t)(cap + 1) * sizeof(uint64_t));
    if (!off) goto out;
    c.off = off;
    kdom = ray_sym_domain_new();
    if (!kdom) goto out;
    /* chunks of whole words: at most SYM_MAP_CHUNK ranks and about
     * SYM_MAP_CHUNK_BYTES of output each (one word may exceed either) */
    int64_t w0 = 0;
    size_t bytes_cap = 0;
    while (w0 < c.words) {
        int64_t r0 = base[w0], w1 = w0;
        uint64_t nb = 0;
        int64_t n = 0;
        for (;;) {
            int64_t r1 = w1 + 1 < c.words ? (int64_t)base[w1 + 1] : K;
            uint64_t wb = 0;
            for (int64_t r = (int64_t)base[w1]; r < r1; r++) wb += c.olen[r];
            if (w1 > w0 && (r1 - r0 > SYM_MAP_CHUNK || nb + wb > (uint64_t)SYM_MAP_CHUNK_BYTES)) break;
            nb += wb; n = r1 - r0; w1++;
            if (w1 >= c.words || n >= SYM_MAP_CHUNK || nb >= (uint64_t)SYM_MAP_CHUNK_BYTES) break;
        }
        if (n > 0) {
            if (nb + 1 > bytes_cap) {
                if (bytes_hdr) scratch_free(bytes_hdr);
                bytes_hdr = NULL;
                bytes_cap = (size_t)nb + 1;
                c.bytes = (char*)scratch_alloc(&bytes_hdr, bytes_cap);
                if (!c.bytes) goto out;
            }
            off[0] = 0;
            for (int64_t r = 0; r < n; r++) off[r + 1] = off[r] + c.olen[r0 + r];
            c.w0 = w0; c.r0 = r0;
            sym_map_run_units(pool, sym_map_cut, &c, (w1 - w0) << SYM_MAP_UNIT_LOG);
            ray_profile_tick("sym map: chunk transformed");
            if (pool_cancelled(pool)) { rc = -1; goto out; }
            if (!ray_sym_domain_intern_batch(kdom, n, c.strs, c.lens, c.hashes, pos)) {
                rc = pool_cancelled(pool) ? -1 : 0;
                goto out;
            }
            ray_profile_tick("sym map: chunk interned");
            for (int64_t j = 0; j < n; j++) {
                if (pos[j] < 0 || pos[j] > INT32_MAX) goto out;
                ids[r0 + j] = (int32_t)pos[j];
            }
        }
        w0 = w1;
    }
    c.ids = ids;
    /* few distinct results: runtime ids as the row loop wrote them (position
     * 0 is the seeded "", used or not) */
    int64_t kc = ray_sym_domain_count(kdom);
    bool global = kc <= SYM_MAP_GLOBAL_MAX + 1;
    if (global) {
        int32_t gmap[SYM_MAP_GLOBAL_MAX + 1];
        for (int64_t q = 0; q < kc; q++) {
            ray_t* str = ray_sym_domain_str(kdom, q);
            int64_t id = str ? ray_sym_intern(ray_str_ptr(str), ray_str_len(str)) : -1;
            if (id < 0 || id > INT32_MAX) goto out;
            gmap[q] = (int32_t)id;
        }
        for (int64_t r = 0; r < K; r++) ids[r] = gmap[ids[r]];
    }
    {
        /* W64 for runtime ids, as the row loop wrote them; a domain of the
         * result's own at the narrowest width that holds it */
        uint8_t w = global ? RAY_SYM_W64 : ray_sym_dict_width(kc);
        ray_t* res = ray_sym_vec_new(w, nrows);
        if (!res || RAY_IS_ERR(res)) { if (res) ray_error_free(res); goto out; }
        res->len = nrows;
        if (!global) {
            /* the result owns the dictionary (the ref ray_sym_domain_new gave) */
            ray_sym_domain_release(res->sym_domain);
            res->sym_domain = kdom;
            kdom = NULL;
        }
        c.dst = ray_data(res);
        c.dst_attrs = res->attrs;
        sym_map_run(pool, sym_map_fill, &c, nrows);
        ray_profile_tick("sym map: rows filled");
        if (pool_cancelled(pool)) { ray_release(res); rc = -1; goto out; }
        atomic_fetch_add_explicit(&ray_string_sym_map_runs_ctr, 1, memory_order_relaxed);
        *out = res;
        rc = 1;
    }
out:
    if (kdom) ray_sym_domain_release(kdom);
    sym_reader_done(&c.rd);
    if (bytes_hdr) scratch_free(bytes_hdr);
    if (off_hdr) scratch_free(off_hdr);
    if (chunk_hdr) scratch_free(chunk_hdr);
    if (ids_hdr) scratch_free(ids_hdr);
    if (olen_hdr) scratch_free(olen_hdr);
    if (base_hdr) scratch_free(base_hdr);
    if (bm_hdr) scratch_free(bm_hdr);
    return rc;
}

/* The by-code path for a SYM column of enough rows: *res set and 1 when it
 * served, -1 cancelled, 0 when the caller runs its row loop. */
static int sym_map_column(ray_t* input, const sym_map_op_t* m, ray_t** res) {
    if (input->type != RAY_SYM || input->len < SYM_MAP_MIN_ROWS) return 0;
    return sym_map_by_code(input, m, res);
}

/* UPPER / LOWER / TRIM — unary SYM/STR → SYM/STR */
ray_t* exec_string_unary(ray_graph_t* g, ray_op_t* op) {
    ray_t* input = exec_node(g, op_child(g, op, 0));
    if (!input || RAY_IS_ERR(input)) return input;
    /* A literal (`(trim " ab")`) arrives as an atom, whose len field
     * aliases its payload: the loop below read that many rows past it.
     * The builtin answers an atom as one value. */
    if (input->type < 0) {
        ray_t* r = op->opcode == OP_UPPER ? ray_upper_fn(input)
                 : op->opcode == OP_LOWER ? ray_lower_fn(input)
                 :                          ray_trim_fn(input);
        ray_release(input);
        return r;
    }

    int64_t len = input->len;
    bool is_str = (input->type == RAY_STR);

    /* A SYM column: once per distinct code, into the result's own domain
     * (above).  A small one keeps the row loop: its interns are few, and
     * the bitmap of the domain is not paid. */
    {
        sym_map_op_t m = { .opc = op->opcode };
        ray_t* res = NULL;
        int rc = sym_map_column(input, &m, &res);
        if (rc != 0) {
            ray_release(input);
            return rc < 0 ? ray_error("cancel", NULL) : res;
        }
    }

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

typedef struct {
    sym_reader_t         rd;
    const char*          needle;
    size_t               needle_len;
    int64_t*             dst;
    ray_pool_t*          pool;
    _Atomic(int)         any_null;
} str_find_sym_ctx_t;

static void str_find_sym_task(void* vctx, uint32_t worker_id, int64_t lo, int64_t hi) {
    (void)worker_id;
    str_find_sym_ctx_t* c = (str_find_sym_ctx_t*)vctx;
    const void* base = ray_data(c->rd.v);
    uint8_t attrs = c->rd.v->attrs;
    bool any_null = false;
    if (!c->rd.runtime) {
        /* A file's strings one at a time: a compressed entry is decoded
         * into the thread's ring, good until the next few reads. */
        for (int64_t i = lo; i < hi; i++) {
            if (((i - lo) & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(c->pool)) return;
            const char* sp; size_t sl;
            sym_reader_at(&c->rd, ray_read_sym(base, i, RAY_SYM, attrs), &sp, &sl);
            size_t pos = 0;
            bool found = ray_str_find_literal(sp, sl, c->needle, c->needle_len, &pos);
            c->dst[i] = found ? (int64_t)pos : NULL_I64;
            if (!found) any_null = true;
        }
        if (any_null) atomic_store_explicit(&c->any_null, 1, memory_order_relaxed);
        return;
    }
    int64_t ids[SYM_READ_BLOCK];
    const char* sp[SYM_READ_BLOCK];
    size_t sl[SYM_READ_BLOCK];
    for (int64_t b = lo; b < hi; b += SYM_READ_BLOCK) {
        if (pool_cancelled(c->pool)) return;
        int n = (int)(hi - b < SYM_READ_BLOCK ? hi - b : SYM_READ_BLOCK);
        for (int j = 0; j < n; j++) ids[j] = ray_read_sym(base, b + j, RAY_SYM, attrs);
        sym_reader_block(&c->rd, ids, n, sp, sl);
        for (int j = 0; j < n; j++) {
            size_t pos = 0;
            bool found = ray_str_find_literal(sp[j], sl[j], c->needle, c->needle_len, &pos);
            c->dst[b + j] = found ? (int64_t)pos : NULL_I64;
            if (!found) any_null = true;
        }
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
            .needle = pattern,
            .needle_len = pattern_len,
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

    /* SYM: the rows in parallel, each string read from its domain. */
    {
        ray_pool_t* pool = nrows >= RAY_PARALLEL_THRESHOLD ? ray_pool_get() : NULL;
        str_find_sym_ctx_t ctx = { .needle = pattern, .needle_len = pattern_len,
                                   .dst = dst, .pool = pool };
        sym_reader_init(&ctx.rd, input);
        atomic_init(&ctx.any_null, 0);
        if (pool && ray_pool_total_workers(pool) >= 2)
            ray_pool_dispatch(pool, str_find_sym_task, &ctx, nrows);
        else
            str_find_sym_task(&ctx, 0, 0, nrows);
        sym_reader_done(&ctx.rd);
        if (pool_cancelled(pool)) {
            ray_release(result);
            ray_release(input);
            ray_release(pat_v);
            return ray_error("cancel", NULL);
        }
        if (atomic_load_explicit(&ctx.any_null, memory_order_relaxed))
            result->attrs |= RAY_ATTR_HAS_NULLS;
    }
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
    if (!ray_is_vec(v) || v->len != 1 || ray_vec_may_have_nulls(v))
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

/* True when a whole-column (scalar) start/length argument is null.  Such an
 * argument applies to every row, so the entire result is null — and a null
 * integer scalar is INT64_MIN, which must never reach the `scalar - 1`
 * subtraction in the row loop (signed-overflow UB).  Only scalars are tested
 * here; the per-row vector case is handled inline via ray_vec_is_null. */
static bool substr_scalar_is_null(ray_t* v) {
    if (!v) return false;
    if (ray_is_atom(v)) return RAY_ATOM_IS_NULL(v);
    if (ray_is_vec(v) && v->len == 1 && ray_vec_may_have_nulls(v))
        return ray_vec_is_null(v, 0);
    return false;
}

/* Per-row start / length argument of substr: an atom, a 1-element vector
 * (scalar), or a vector with one value per row (I64 / I32 / F64).  `ok` is
 * false for a null. */
typedef struct {
    int64_t        scalar;
    const int64_t* i64;
    const int32_t* i32;
    const double*  f64;
    ray_t*         v;          /* the vector, for null checks; NULL for scalars */
    bool           all_null;
} substr_arg_t;

static bool substr_arg_init(ray_t* v, int64_t nrows, substr_arg_t* a) {
    memset(a, 0, sizeof(*a));
    int64_t sc;
    if (substr_scalar_arg(v, &sc)) { a->scalar = sc; return true; }
    if (substr_scalar_is_null(v)) { a->all_null = true; return true; }
    if (!ray_is_vec(v) || v->len != nrows) return false;
    a->v = v;
    switch (v->type) {
    case RAY_I64: a->i64 = (const int64_t*)ray_data(v); return true;
    case RAY_I32: a->i32 = (const int32_t*)ray_data(v); return true;
    case RAY_F64: a->f64 = (const double*)ray_data(v);  return true;
    default: return false;
    }
}

static inline bool substr_arg_at(const substr_arg_t* a, int64_t r, int64_t* out) {
    if (a->all_null) return false;
    if (!a->v) { *out = a->scalar; return true; }
    if (a->i64) { int64_t x = a->i64[r]; if (x == NULL_I64) return false; *out = x; return true; }
    if (a->i32) { int32_t x = a->i32[r]; if (x == NULL_I32) return false; *out = (int64_t)x; return true; }
    double d = a->f64[r];
    if (d != d) return false;
    *out = (int64_t)d;
    return true;
}

/* Substring of a STR column as descriptors over the column's own pool:
 * an inline result copies its bytes, a longer one points into the parent
 * pool at the shifted offset.  No bytes are copied and no pool is built,
 * so the pass is descriptor-bound and runs on the worker pool.  A null
 * start or length gives a null (empty) row; the empty result of a start
 * past the end is an empty row.  Returns NULL for shapes it does not take
 * (the caller keeps the general loop). */
typedef struct {
    const ray_str_t* src;
    const char*      pool;
    ray_str_t*       dst;
    substr_arg_t     start;
    substr_arg_t     len;
    _Atomic(uint32_t) any_null;
    _Atomic(uint32_t) range_err;
    _Atomic(uint64_t) pooled_bytes;   /* bytes the pooled results point at */
} substr_view_ctx_t;

static void substr_view_fn(void* vctx, uint32_t worker_id, int64_t lo, int64_t hi) {
    (void)worker_id;
    substr_view_ctx_t* c = (substr_view_ctx_t*)vctx;
    bool null_seen = false, range_seen = false;
    uint64_t pooled = 0;
    for (int64_t i = lo; i < hi; i++) {
        ray_str_t* d = &c->dst[i];
        memset(d, 0, sizeof(*d));
        int64_t st, ln;
        if (!substr_arg_at(&c->start, i, &st) || !substr_arg_at(&c->len, i, &ln)) {
            null_seen = true;
            continue;
        }
        const ray_str_t* s = &c->src[i];
        int64_t sl = (int64_t)s->len;
        st -= 1;                       /* 1-based → 0-based */
        if (st < 0) st = 0;
        if (st >= sl) continue;
        if (ln < 0 || ln > sl - st) ln = sl - st;
        if (ln <= 0) continue;
        const char* sp = ray_str_t_ptr(s, c->pool) + st;
        d->len = (uint32_t)ln;
        if (ln <= RAY_STR_INLINE_MAX) {
            memcpy(d->data, sp, (size_t)ln);
        } else {
            /* a pooled result needs a pooled source (an inline source is at
             * most 12 bytes, so a longer result never comes from one) */
            if ((uint64_t)s->pool_off + (uint64_t)st > UINT32_MAX) { range_seen = true; d->len = 0; continue; }
            memcpy(d->prefix, sp, 4);
            d->pool_off = s->pool_off + (uint32_t)st;
            pooled += (uint64_t)ln;
            /* hash32 stays 0: a consumer that needs it computes it once
             * (ray_str_t_hash32); hashing every substring here paid a pass
             * over the bytes that most consumers never used. */
        }
    }
    if (null_seen)  atomic_store_explicit(&c->any_null, 1, memory_order_relaxed);
    if (range_seen) atomic_store_explicit(&c->range_err, 1, memory_order_relaxed);
    if (pooled) atomic_fetch_add_explicit(&c->pooled_bytes, pooled, memory_order_relaxed);
}

/* A view whose bytes are a small share (under an eighth) of the pool it
 * points into is worth rebuilding over its own bytes: the copy costs the
 * few bytes it keeps, the pool it would otherwise pin costs the rest.  A
 * larger share stays a view — the parent pool is usually alive anyway (a
 * column, a sibling intermediate), and copying most of it would only add
 * a second copy for the view's lifetime. */
bool ray_str_view_should_compact(uint64_t pooled_bytes, int64_t pool_len) {
    return pool_len > 0 && pooled_bytes * 8 < (uint64_t)pool_len;
}

static ray_t* substr_str_view(ray_t* input, ray_t* start_v, ray_t* len_v) {
    if (!input || input->type != RAY_STR) return NULL;
    int64_t nrows = input->len;
    substr_view_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!substr_arg_init(start_v, nrows, &ctx.start)) return NULL;
    if (!substr_arg_init(len_v, nrows, &ctx.len)) return NULL;

    ray_t* result = ray_vec_new(RAY_STR, nrows);
    if (!result || RAY_IS_ERR(result)) return result ? result : ray_error("oom", NULL);
    result->len = nrows;
    str_resolve(input, &ctx.src, &ctx.pool);
    ray_t* owner = (input->attrs & RAY_ATTR_SLICE) ? input->slice_parent : input;
    ray_t* pool_obj = owner ? owner->str_pool : NULL;
    if (pool_obj && !RAY_IS_ERR(pool_obj)) {
        ray_retain(pool_obj);
        result->str_pool = pool_obj;
    }
    ctx.dst = (ray_str_t*)ray_data(result);
    atomic_store_explicit(&ctx.any_null, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx.range_err, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx.pooled_bytes, 0, memory_order_relaxed);

    ray_pool_t* pool = ray_pool_get();
    if (ray_pool_par_dispatch_ok(pool, nrows, RAY_PARALLEL_THRESHOLD))
        ray_pool_dispatch(pool, substr_view_fn, &ctx, nrows);
    else
        substr_view_fn(&ctx, 0, 0, nrows);
    if (atomic_load_explicit(&ctx.range_err, memory_order_relaxed)) {
        ray_release(result);
        return ray_error("range", "substr: pool offset exceeds %lld bytes", (long long)UINT32_MAX);
    }
    if (atomic_load_explicit(&ctx.any_null, memory_order_relaxed))
        result->attrs |= RAY_ATTR_HAS_NULLS;
    /* A result with no pooled descriptor (every substring fits inline)
     * has nothing in the parent pool to keep alive.  A view that does
     * point into it stays a view: the column is alive anyway, and a
     * sibling `if` over two such views can pick either side without
     * copying (a compacted view would give it two different pools). */
    if (result->str_pool &&
        atomic_load_explicit(&ctx.pooled_bytes, memory_order_relaxed) == 0) {
        ray_release(result->str_pool);
        result->str_pool = NULL;
    }
    return result;
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

    /* An atom's len aliases its payload; see exec_string_unary. */
    if (input->type < 0) {
        ray_t* args[3] = { input, start_v, len_v };
        ray_t* r = ray_substr_fn(args, 3);
        ray_release(input); ray_release(start_v); ray_release(len_v);
        return r;
    }

    int64_t nrows = input->len;
    bool is_str = (input->type == RAY_STR);

    if (is_str) {
        /* Descriptors over the column's own pool, for every scalar / per-row
         * combination of start and length that reads as an integer. */
        ray_t* view = substr_str_view(input, start_v, len_v);
        if (view) {
            ray_release(input);
            ray_release(start_v);
            ray_release(len_v);
            return view;
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

    /* A SYM column cut at scalar bounds: once per code (sym_map_by_code). */
    if (!is_str && !s_all_null && !l_all_null && !s_data && !s_data_i32 && !l_data && !l_data_i32) {
        sym_map_op_t m = { .opc = OP_SUBSTR, .st = s_scalar - 1 < 0 ? 0 : s_scalar - 1, .ln = l_scalar };
        ray_t* res = NULL;
        int rc = sym_map_column(input, &m, &res);
        if (rc != 0) {
            ray_release(result);
            ray_release(input); ray_release(start_v); ray_release(len_v);
            return rc < 0 ? ray_error("cancel", NULL) : res;
        }
    }

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

    /* An atom's len aliases its payload; see exec_string_unary. */
    if (input->type < 0) {
        ray_t* args[3] = { input, from_v, to_v };
        ray_t* r = ray_replace_fn(args, 3);
        ray_release(input); ray_release(from_v); ray_release(to_v);
        return r;
    }

    /* from and to are string or symbol atoms, as for an atom input
     * (replace_atom): a symbol is its text, never a string header.  A query
     * compiles any expression into these slots, so a column, a vector or
     * another type is refused here, as is an input that is not text. */
    const char* from_str = NULL;
    const char* to_str = NULL;
    size_t from_len = 0, to_len = 0;
    bool from_ok = exec_string_atom_arg(from_v, &from_str, &from_len);
    if (!from_ok || !exec_string_atom_arg(to_v, &to_str, &to_len)) {
        ray_release(input); ray_release(from_v); ray_release(to_v);
        return ray_error("type", "replace: %s must be string or symbol atom",
                         from_ok ? "to" : "from");
    }
    if (input->type != RAY_STR && input->type != RAY_SYM) {
        int8_t t = input->type;
        ray_release(input); ray_release(from_v); ray_release(to_v);
        return ray_error("type", "replace: expected string or symbol, got %s", ray_type_name(t));
    }

    int64_t nrows = input->len;
    bool is_str = (input->type == RAY_STR);

    /* A SYM column: once per distinct code (sym_map_by_code). */
    {
        sym_map_op_t m = { .opc = OP_REPLACE, .from = from_str, .from_len = from_len, .to = to_str, .to_len = to_len };
        ray_t* res = NULL;
        int rc = sym_map_column(input, &m, &res);
        if (rc != 0) {
            ray_release(input); ray_release(from_v); ray_release(to_v);
            return rc < 0 ? ray_error("cancel", NULL) : res;
        }
    }

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

/* Shared text kernel. Consumes one reference to each argument.  A STR
 * vector makes the result STR; `str_out` says a STR atom does too (a cell
 * of a STR column read row by row).  The STR null "" and the SYM null are
 * empty text wherever they come from (a column's cell, a reduced cell or a
 * literal), as the eager (concat "" "a") is "a" (#1584): no operand nulls
 * the row. */
static ray_t* concat_text_values(ray_t** args, int n_args, bool str_out) {
    /* Only atoms broadcast. A one-row vector is still a vector, and an
     * empty vector must never be indexed as if it contained row zero. */
    int64_t nrows = 1;
    bool out_str = str_out, has_vector = false;
    for (int a = 0; a < n_args; a++) {
        if (args[a]->type == RAY_STR) out_str = true;
        if (ray_is_atom(args[a])) continue;
        if (has_vector && args[a]->len != nrows) {
            for (int i = 0; i < n_args; i++) ray_release(args[i]);
            return ray_error("length", "concat: text columns must have equal lengths");
        }
        nrows = args[a]->len;
        has_vector = true;
    }
    /* One SYM column among constants: once per distinct code
     * (sym_map_by_code), the constants' bytes gathered before and after
     * it.  A null constant is empty text, as the row loop has it. */
    if (!out_str && has_vector) {
        int col = -1;
        bool fits = true;
        size_t pre_len = 0, post_len = 0;
        for (int a = 0; a < n_args && fits; a++) {
            if (args[a]->type == RAY_SYM) { fits = col < 0; col = a; continue; }
            if (args[a]->type != -RAY_STR && args[a]->type != -RAY_SYM) { fits = false; break; }
            size_t l = 0;
            if (args[a]->type == -RAY_STR) l = ray_str_len(args[a]);
            else { ray_t* st = ray_sym_str(args[a]->i64); if (st) l = ray_str_len(st); }
            if (col < 0) pre_len += l; else post_len += l;
        }
        if (fits && col >= 0 && args[col]->len >= SYM_MAP_MIN_ROWS) {
            ray_t* khdr = NULL;
            char* kb = (char*)scratch_alloc(&khdr, pre_len + post_len + 1);
            if (kb) {
                size_t bi = 0;
                for (int a = 0; a < n_args; a++) {
                    if (a == col) continue;
                    const char* sp = NULL; size_t l = 0;
                    if (args[a]->type == -RAY_STR) { sp = ray_str_ptr(args[a]); l = ray_str_len(args[a]); }
                    else { ray_t* st = ray_sym_str(args[a]->i64); if (st) { sp = ray_str_ptr(st); l = ray_str_len(st); } }
                    if (sp && l) { memcpy(kb + bi, sp, l); bi += l; }
                }
                sym_map_op_t m = { .opc = OP_CONCAT, .pre = kb, .pre_len = pre_len,
                                   .post = kb + pre_len, .post_len = post_len };
                ray_t* res = NULL;
                int rc = sym_map_column(args[col], &m, &res);
                scratch_free(khdr);
                if (rc != 0) {
                    for (int i = 0; i < n_args; i++) ray_release(args[i]);
                    return rc < 0 ? ray_error("cancel", NULL) : res;
                }
            }
        }
    }

    ray_t* result = ray_vec_new(out_str ? RAY_STR : RAY_SYM, nrows);
    if (!result || RAY_IS_ERR(result)) {
        for (int i = 0; i < n_args; i++) ray_release(args[i]);
        return result;
    }
    if (!out_str) result->len = nrows;
    int64_t* dst = out_str ? NULL : (int64_t*)ray_data(result);

    for (int64_t r = 0; r < nrows; r++) {
        /* Pre-scan to compute total concat length for this row */
        size_t total = 0;
        for (int a = 0; a < n_args; a++) {
            int8_t t = args[a]->type;
            if (t == RAY_STR) {
                const ray_str_t* elems; const char* p;
                str_resolve(args[a], &elems, &p);
                int64_t ar = r;
                total += elems[ar].len;
            } else if (RAY_IS_SYM(t)) {
                const char* sp; size_t sl;
                int64_t ar = r;
                sym_elem(args[a], ar, &sp, &sl);
                total += sl;
            } else if (t == -RAY_STR) {
                total += ray_str_len(args[a]);
            } else if (t == -RAY_SYM) {
                /* A symbol literal or a scalar such as (first s): RAY_IS_SYM
                 * matches SYM vectors only, so its text was left out and
                 * (concat 'ab 'cd) came back null. */
                ray_t* st = ray_sym_str(args[a]->i64);
                if (st) total += ray_str_len(st);
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
                int64_t ar = r;
                const char* sp = ray_str_t_ptr(&elems[ar], pool);
                size_t sl = elems[ar].len;
                if (bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            } else if (RAY_IS_SYM(t)) {
                const char* sp; size_t sl;
                int64_t ar = r;
                sym_elem(args[a], ar, &sp, &sl);
                if (bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            } else if (t == -RAY_STR) {
                const char* sp = ray_str_ptr(args[a]);
                size_t sl = ray_str_len(args[a]);
                if (sp && bi + sl < buf_cap) { memcpy(buf + bi, sp, sl); bi += sl; }
            } else if (t == -RAY_SYM) {
                ray_t* st = ray_sym_str(args[a]->i64);
                const char* sp = st ? ray_str_ptr(st) : NULL;
                size_t sl = st ? ray_str_len(st) : 0;
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

    /* An operand that reduces a text column to one value (first, last, min,
     * max) is a cell of the rows: a cell of a STR column makes the result
     * STR, as a STR column itself does, at every operand position. */
    bool str_out = false;
    for (int i = 0; i < n_args; i++) {
        ray_op_t* child = i < 2 ? op_child(g, op, i) : &g->nodes[trail[i - 2]];
        if (child && (child->opcode == OP_FIRST || child->opcode == OP_LAST ||
                      child->opcode == OP_MIN || child->opcode == OP_MAX) &&
            child->out_type == RAY_STR)
            str_out = true;
    }
    ray_t* result = concat_text_values(args, n_args, str_out);
    scratch_free(args_hdr);
    return result;
}

/* Query fallback uses the same text kernel after checking argument syntax.
 * Ordinary builtin calls retain collection semantics. */
ray_t* ray_concat_text_fn(ray_t* a, ray_t* b, bool str_out) {
    bool atoms = ray_is_atom(a) && ray_is_atom(b);
    /* A null operand, cell or literal, is empty text (#1584). */
    bool a_null = RAY_ATOM_IS_NULL(a), b_null = RAY_ATOM_IS_NULL(b);
    /* Two STR atoms join as strings, as the planner folds them. */
    if (a->type == -RAY_STR && b->type == -RAY_STR)
        return ray_concat_fn(a, b);
    if (atoms && (a->type == -RAY_SYM || a->type == -RAY_STR) &&
        (b->type == -RAY_SYM || b->type == -RAY_STR)) {
        /* Two cells, the row-by-row case: build the atom directly rather
         * than a one-row vector that is unpacked again. */
        ray_t* sa = a->type == -RAY_SYM && !a_null ? ray_sym_str(a->i64) : NULL;
        ray_t* sb = b->type == -RAY_SYM && !b_null ? ray_sym_str(b->i64) : NULL;
        if ((a->type == -RAY_SYM && !a_null && !sa) || (b->type == -RAY_SYM && !b_null && !sb)) {
            if (sa) ray_release(sa);
            if (sb) ray_release(sb);
            return ray_error("oom", NULL);
        }
        const char* pa = a_null ? "" : sa ? ray_str_ptr(sa) : ray_str_ptr(a);
        const char* pb = b_null ? "" : sb ? ray_str_ptr(sb) : ray_str_ptr(b);
        size_t la = a_null ? 0 : sa ? ray_str_len(sa) : ray_str_len(a);
        size_t lb = b_null ? 0 : sb ? ray_str_len(sb) : ray_str_len(b);
        char small[256];
        ray_t* hdr = NULL;
        char* buf = la + lb <= sizeof small ? small : (char*)scratch_calloc(&hdr, la + lb + 1);
        ray_t* out;
        if (!buf) out = ray_error("oom", NULL);
        else {
            memcpy(buf, pa, la);
            memcpy(buf + la, pb, lb);
            out = str_out ? ray_str(buf, la + lb) : ray_sym(ray_sym_intern(buf, la + lb));
        }
        if (hdr) scratch_free(hdr);
        if (sa) ray_release(sa);
        if (sb) ray_release(sb);
        return out;
    }
    ray_t* args[2] = { a, b };
    ray_retain(a);
    ray_retain(b);
    ray_t* result = concat_text_values(args, 2, str_out);
    if (!atoms || !result || RAY_IS_ERR(result)) return result;
    int allocated = 0;
    ray_t* atom = collection_elem(result, 0, &allocated);
    if (atom && !allocated) ray_retain(atom);
    ray_release(result);
    return atom;
}
