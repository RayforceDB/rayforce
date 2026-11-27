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
#include "lang/internal.h"  /* sym_domain_rep (sym-domain Phase 2) */
#include "ops/colgather.h"
#include <stdatomic.h>

/* ============================================================================
 * Filter execution — extracted from exec.c
 * ============================================================================ */

/* Filter a single vector by boolean predicate. */
static ray_t* exec_filter_vec(ray_t* input, ray_t* pred, int64_t pass_count) {
    /* RAY_LIST: cells are ray_t* — gather them with a retain.  The
     * byte-copy loop below would alias elements without a refcount (and
     * col_vec_new rejects RAY_LIST, type 0, outright). */
    if (input->type == RAY_LIST) {
        if (input->len != pred->len)
            return ray_error("length", "filter: predicate length must match input, got %lld and %lld",
                             (long long)input->len, (long long)pred->len);
        ray_t* result = ray_list_new(pass_count);
        if (!result || RAY_IS_ERR(result)) return result;
        ray_t** dst = (ray_t**)ray_data(result);
        ray_morsel_t mi, mf;
        ray_morsel_init(&mi, input);
        ray_morsel_init(&mf, pred);
        int64_t out_idx = 0;
        while (ray_morsel_next(&mi) && ray_morsel_next(&mf)) {
            uint8_t* bits = (uint8_t*)mf.morsel_ptr;
            ray_t** src = (ray_t**)mi.morsel_ptr;
            for (int64_t i = 0; i < mi.morsel_len; i++) {
                if (!bits[i]) continue;
                if (out_idx >= pass_count) break;
                ray_t* e = src[i];
                if (e) ray_retain(e);
                dst[out_idx++] = e;
            }
        }
        result->len = out_idx;
        return result;
    }

    uint8_t esz = col_esz(input);
    ray_t* result = col_vec_new(input, pass_count);
    if (!result || RAY_IS_ERR(result)) return result;
    result->len = pass_count;

    ray_morsel_t mi, mf;
    ray_morsel_init(&mi, input);
    ray_morsel_init(&mf, pred);
    int64_t out_idx = 0;

    if (input->len != pred->len) { int64_t il = input->len, pl = pred->len; ray_release(result); return ray_error("length", "filter: predicate length must match input, got %lld and %lld", (long long)il, (long long)pl); }

    while (ray_morsel_next(&mi) && ray_morsel_next(&mf)) {
        uint8_t* bits = (uint8_t*)mf.morsel_ptr;
        char* src = (char*)mi.morsel_ptr;
        char* dst = (char*)ray_data(result);
        for (int64_t i = 0; i < mi.morsel_len; i++) {
            if (bits[i]) {
                memcpy(dst + out_idx * esz, src + i * esz, esz);
                out_idx++;
            }
        }
    }

    col_propagate_str_pool(result, input);
    /* Output-vec rule (sym-domain Phase 2): the filtered SYM output
     * raw-copies cell ids from `input`, so it must resolve over the
     * same dictionary.  No-op while every domain is the runtime
     * singleton. */
    if (result->type == RAY_SYM)
        ray_sym_vec_adopt_domain(result, input);
    col_propagate_nulls_filter(result, input,
                               (const uint8_t*)ray_data(pred), input->len);
    return result;
}

/* Filter a parted column by boolean predicate (sequential).  `cnt`/`ncnt`
 * are the table's partition counts: a NULL segment (a partition without
 * the column) spans that many predicate rows, and those it passes come out
 * as the type's null. */
static ray_t* exec_filter_parted_vec(ray_t* parted_col, ray_t* pred,
                                     int64_t pass_count,
                                     const int64_t* cnt, int64_t ncnt) {
    int8_t base = (int8_t)RAY_PARTED_BASETYPE(parted_col->type);
    ray_t** segs = (ray_t**)ray_data(parted_col);
    int64_t n_segs = parted_col->len;
    uint8_t* pred_data = (uint8_t*)ray_data(pred);

    /* RAY_LIST: share (retain) elements — the byte-copy path below would
     * alias them without a refcount and double-free on release. */
    if (base == RAY_LIST)
        return parted_filter_list_c(segs, n_segs, pred_data, pass_count, cnt, ncnt);

    /* RAY_STR: deep-copy to handle multi-pool segments */
    if (base == RAY_STR) {
        ray_t* result = ray_vec_new(RAY_STR, pass_count);
        if (!result || RAY_IS_ERR(result)) return result;
        int64_t pred_off = 0;
        for (int64_t s = 0; s < n_segs; s++) {
            if (!segs[s]) {
                int64_t n = parted_seg_rows(segs, s, cnt, ncnt);
                for (int64_t i = 0; i < n; i++) {
                    if (!pred_data[pred_off + i]) continue;
                    result = ray_str_vec_append(result, "", 0);
                    if (RAY_IS_ERR(result)) return result;
                    ray_vec_set_null(result, result->len - 1, true);
                }
                pred_off += n;
                continue;
            }
            int64_t seg_len = segs[s]->len;
            const char* pool_base = segs[s]->str_pool
                                  ? (const char*)ray_data(segs[s]->str_pool) : NULL;
            for (int64_t i = 0; i < seg_len; i++) {
                if (pred_data[pred_off + i]) {
                    result = parted_str_append_elem(result, segs[s], i, pool_base);
                    if (RAY_IS_ERR(result)) return result;
                }
            }
            pred_off += seg_len;
        }
        return result;
    }

    uint8_t base_attrs = (base == RAY_SYM)
                       ? parted_sym_max_attrs(segs, n_segs) : 0;
    ray_t* result = typed_vec_new(base, base_attrs, pass_count);
    if (!result || RAY_IS_ERR(result)) return result;
    result->len = pass_count;

    int64_t out_idx = 0;
    int64_t pred_off = 0;

    for (int64_t s = 0; s < n_segs; s++) {
        if (!segs[s]) {
            int64_t n = parted_seg_rows(segs, s, cnt, ncnt);
            for (int64_t i = 0; i < n; i++) {
                if (!pred_data[pred_off + i]) continue;
                parted_fill_nulls(result, out_idx, 1);
                out_idx++;
            }
            pred_off += n;
            continue;
        }
        int64_t seg_len = segs[s]->len;
        char* dst = (char*)ray_data(result);
        bool seg_has_nulls = ray_vec_may_have_nulls(segs[s]);
        if (seg_has_nulls) {
            for (int64_t i = 0; i < seg_len; i++) {
                if (pred_data[pred_off + i]) {
                    parted_copy_cells(dst, base, base_attrs, out_idx,
                                      segs[s], i, 1);
                    if (ray_vec_is_null(segs[s], i))
                        ray_vec_set_null(result, out_idx, true);
                    out_idx++;
                }
            }
        } else {
            for (int64_t i = 0; i < seg_len; i++) {
                if (pred_data[pred_off + i]) {
                    parted_copy_cells(dst, base, base_attrs, out_idx,
                                      segs[s], i, 1);
                    out_idx++;
                }
            }
        }
        pred_off += seg_len;
    }
    /* SYM segments raw-copy into the output — adopt the partitions'
     * shared domain (all segments resolve over the root symfile). */
    if (result->type == RAY_SYM)
        ray_sym_vec_adopt_domain(result, sym_domain_rep(parted_col));
    return result;
}

/* Sequential table filter fallback (small tables or alloc failure). */
static ray_t* exec_filter_seq(ray_t* input, ray_t* pred, int64_t ncols,
                             int64_t pass_count) {
    int64_t ncnt = 0;
    const int64_t* cnt = parted_table_counts(input, &ncnt);
    ray_t* tbl = ray_table_new(ncols);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    /* Generation-2 slots gather under the predicate's selection, built
     * once on the first such slot (NULL when every row passes). */
    ray_t* sel = NULL;
    bool sel_built = false;
    for (int64_t c = 0; c < ncols; c++) {
        int64_t name_id = ray_table_col_name(input, c);
        ray_t* enc = ray_col_gather_slot(ray_table_get_col_enc_idx(input, c));
        if (enc) {
            if (!sel_built) {
                if (pred->len != ray_table_nrows(input)) {
                    int64_t il = ray_table_nrows(input), pl = pred->len;
                    ray_release(tbl);
                    return ray_error("length", "filter: predicate length must match input, got %lld and %lld",
                                     (long long)il, (long long)pl);
                }
                sel = ray_rowsel_from_pred(pred);
                sel_built = true;
                if (sel && RAY_IS_ERR(sel)) { ray_release(tbl); return sel; }
                if (!sel && pass_count != pred->len) { ray_release(tbl); return ray_error("oom", NULL); }
            }
            ray_t* gc = NULL;
            ray_t* ge = ray_col_gather_enc(enc, sel, false, NULL, 0, &gc);
            if (ge) { if (sel) ray_rowsel_release(sel); ray_release(tbl); return ge; }
            tbl = ray_table_add_col(tbl, name_id, gc);
            ray_release(gc);
            if (!tbl || RAY_IS_ERR(tbl)) { if (sel) ray_rowsel_release(sel); return tbl ? tbl : ray_error("oom", NULL); }
            continue;
        }
        ray_t* col = ray_table_col_idx(input, c);
        if (col && RAY_IS_ERR(col)) { if (sel) ray_rowsel_release(sel); ray_release(tbl); return col; }
        if (!col) continue;
        if (col->type == RAY_MAPCOMMON) {
            ray_t* mc_filt = materialize_mapcommon_filter(col, pred, pass_count);
            if (!mc_filt || RAY_IS_ERR(mc_filt)) { if (sel) ray_rowsel_release(sel); ray_release(tbl); return mc_filt; }
            tbl = ray_table_add_col(tbl, name_id, mc_filt);
            ray_release(mc_filt);
            continue;
        }
        ray_t* filtered;
        if (RAY_IS_PARTED(col->type))
            filtered = exec_filter_parted_vec(col, pred, pass_count, cnt, ncnt);
        else
            filtered = exec_filter_vec(col, pred, pass_count);
        if (!filtered || RAY_IS_ERR(filtered)) { if (sel) ray_rowsel_release(sel); ray_release(tbl); return filtered; }
        tbl = ray_table_add_col(tbl, name_id, filtered);
        ray_release(filtered);
    }
    if (sel) ray_rowsel_release(sel);
    return tbl;
}

/* ── Parallel bitmap → row-index list ─────────────────────────────────
 * Classic three-phase compaction: per-chunk popcount (parallel) → tiny
 * serial prefix over chunks → per-chunk index fill at disjoint offsets
 * (parallel).  Replaces the two sequential 0..nrows sweeps that made
 * every large WHERE-select serial-bound ahead of its parallel gather. */
#define FIDX_CHUNK_ROWS 65536

typedef struct {
    const uint8_t* bits;
    int64_t  nrows;
    int64_t  chunk_rows;
    int64_t* chunk_counts;   /* [n_chunks]: popcount, then prefix offset */
    int64_t* match_idx;      /* fill-phase output (NULL during count) */
} fidx_ctx_t;

/* Dispatched via ray_pool_dispatch_n — each task is ONE chunk ([i,i+1)). */
static void fidx_count_fn(void* ctxp, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    fidx_ctx_t* c = (fidx_ctx_t*)ctxp;
    for (int64_t ch = start; ch < end; ch++) {
        int64_t lo = ch * c->chunk_rows;
        int64_t hi = lo + c->chunk_rows;
        if (hi > c->nrows) hi = c->nrows;
        int64_t n = 0;
        for (int64_t i = lo; i < hi; i++) n += c->bits[i] ? 1 : 0;
        c->chunk_counts[ch] = n;
    }
}

static void fidx_fill_fn(void* ctxp, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    fidx_ctx_t* c = (fidx_ctx_t*)ctxp;
    for (int64_t ch = start; ch < end; ch++) {
        int64_t lo = ch * c->chunk_rows;
        int64_t hi = lo + c->chunk_rows;
        if (hi > c->nrows) hi = c->nrows;
        int64_t j = c->chunk_counts[ch];   /* prefix offset after phase 2 */
        for (int64_t i = lo; i < hi; i++)
            if (c->bits[i]) c->match_idx[j++] = i;
    }
}

static ray_t* where_mask_fill(int64_t tn, uint8_t on) {
    ray_t* v = ray_vec_new(RAY_BOOL, tn);
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom", NULL);
    v->len = tn;
    if (tn > 0) memset(ray_data(v), on, (size_t)tn);
    return v;
}

/* The one rule for a `where:` value, applied wherever a predicate meets a
 * table of `tn` rows (OP_FILTER, the take: head filter, the eval fallback,
 * the count-select and the update/delete mask paths):
 *
 *   - a BOOL vector of tn rows is the mask;
 *   - a scalar with a truth value (bool, numeric, temporal — is_truthy, so
 *     zero and null are false) holds for every row or none; a one-element
 *     BOOL over tn != 1 rows likewise;
 *   - any other vector — a numeric, temporal, symbol or string column — is
 *     a type error, and a BOOL vector of another length a length error.
 *     Before this a bare I64 column reached the filter kernels, which read
 *     a mask as one byte per row (#678).
 *
 * Consumes `pred`; returns an owned BOOL vector of tn rows or an error. */
ray_t* ray_where_mask_coerce(ray_t* pred, int64_t tn, const char* label) {
    if (!pred || RAY_IS_ERR(pred)) return pred;
    if (ray_is_atom(pred)) {
        if (!ray_pred_atom_type_ok(pred->type)) {
            int8_t pt = pred->type;
            ray_release(pred);
            return ray_error("type", "%s: a scalar predicate must be boolean or numeric, got %s",
                             label, ray_type_name(pt));
        }
        uint8_t on = is_truthy(pred) ? 1 : 0;
        ray_release(pred);
        return where_mask_fill(tn, on);
    }
    if (ray_is_lazy(pred)) {
        pred = ray_lazy_materialize(pred);
        if (!pred || RAY_IS_ERR(pred)) return pred ? pred : ray_error("oom", NULL);
    }
    if (pred->type == RAY_BOOL) {
        if (pred->len == tn) return pred;
        if (pred->len == 1 && tn != 1) {
            uint8_t on = ((const uint8_t*)ray_data(pred))[0];
            ray_release(pred);
            return where_mask_fill(tn, on);
        }
        int64_t got = pred->len;
        ray_release(pred);
        return ray_error("length", "%s: WHERE mask length %lld does not match row count %lld",
                         label, (long long)got, (long long)tn);
    }
    /* Any other vector — a numeric, temporal, symbol or string column — is
     * no mask (query_coverage.rfl, where_eval_fallback.rfl pin this); the
     * filter kernels read a mask as one byte per row, so an I64 column let
     * through here kept the rows whose low byte of every eighth value was
     * set (#678). */
    int8_t pt = pred->type;
    ray_release(pred);
    return ray_error("type", "%s: WHERE must produce a bool mask or truthy scalar, got %s",
                     label, ray_type_name(pt));
}

ray_t* exec_filter(ray_graph_t* g, ray_op_t* op, ray_t* input, ray_t* pred) {
    int64_t ncnt = 0;
    const int64_t* cnt = parted_table_counts(input, &ncnt);   /* ragged partitions */
    (void)g;
    (void)op;
    if (!input || RAY_IS_ERR(input)) return input;
    if (!pred || RAY_IS_ERR(pred)) return pred;

    /* An atom predicate (scalar subject in a where-clause) has no per-row
     * bits.  Reject it before any morsel walk: ray_morsel_init derives the
     * element size from the type, and a negative (atom) type indexes
     * ray_type_sizes out of bounds. */
    if (pred->type < 0)
        return ray_error("length", "filter: predicate must be a BOOL vector matching the input length, got an atom");
    /* The morsel sweeps below read the mask as one byte per row; any other
     * vector type must be coerced by ray_where_mask_coerce before it gets
     * here (an I64 column read this way kept the rows whose low byte of
     * every eighth value was set, #678). */
    if (pred->type != RAY_BOOL)
        return ray_error("type", "filter: predicate must be a BOOL vector, got %s",
                         ray_type_name(pred->type));

    /* A plain contiguous BOOL predicate over a large input takes the
     * parallel chunked count/fill; anything else (lazy / morsel-backed
     * pred, small input) keeps the sequential morsel sweep. */
    ray_pool_t* fidx_pool = ray_pool_get();
    const int64_t fidx_rows = pred->len;   /* == table nrows for table input */
    bool fidx_par = ray_pool_par_dispatch_ok(fidx_pool, fidx_rows, RAY_PARALLEL_THRESHOLD) &&
                    pred->type == RAY_BOOL && !ray_is_lazy(pred);
    ray_t* fidx_hdr = NULL;
    fidx_ctx_t fidx = { .bits = NULL, .nrows = 0, .chunk_rows = FIDX_CHUNK_ROWS,
                        .chunk_counts = NULL, .match_idx = NULL };
    int64_t fidx_n_chunks = 0;

    /* Count passing elements */
    int64_t pass_count = 0;
    if (fidx_par) {
        fidx.bits  = (const uint8_t*)ray_data(pred);
        fidx.nrows = pred->len;
        /* One task per chunk (dispatch_n).  Chunk count is capped at 1024 —
         * the pool's INITIAL ring capacity — so dispatch_n never needs to
         * grow the ring (its clamp-on-grow-failure silently DROPS tasks,
         * which here would leave uninitialized prefix entries and cause
         * out-of-bounds fill offsets).  1024 tasks is ample granularity. */
        while ((fidx.nrows + fidx.chunk_rows - 1) / fidx.chunk_rows > RAY_POOL_INIT_TASKS)
            fidx.chunk_rows *= 2;
        fidx_n_chunks = (fidx.nrows + fidx.chunk_rows - 1) / fidx.chunk_rows;
        fidx.chunk_counts = (int64_t*)scratch_alloc(&fidx_hdr,
                                (size_t)fidx_n_chunks * sizeof(int64_t));
        if (fidx.chunk_counts)
            ray_pool_dispatch_n(fidx_pool, fidx_count_fn, &fidx,
                                (uint32_t)fidx_n_chunks);
        else
            fidx_par = false;
    }
    if (fidx_par) {
        /* Exclusive prefix: chunk_counts becomes per-chunk write offsets. */
        int64_t cum = 0;
        for (int64_t ch = 0; ch < fidx_n_chunks; ch++) {
            int64_t n = fidx.chunk_counts[ch];
            fidx.chunk_counts[ch] = cum;
            cum += n;
        }
        pass_count = cum;
    } else {
        ray_morsel_t mp;
        ray_morsel_init(&mp, pred);
        while (ray_morsel_next(&mp)) {
            uint8_t* bits = (uint8_t*)mp.morsel_ptr;
            for (int64_t i = 0; i < mp.morsel_len; i++)
                if (bits[i]) pass_count++;
        }
    }

    /* Vector filter — single column, sequential fill.  The parallel count
     * above is NOT wasted here: pass_count sizes the output vector, saving
     * exec_filter_vec its own counting sweep.  Only the chunk-offset prefix
     * (≤1024 adds) goes unused. */
    if (input->type != RAY_TABLE) {
        scratch_free(fidx_hdr);
        return exec_filter_vec(input, pred, pass_count);
    }

    /* table filter: parallel gather using compact match index */
    int64_t ncols = ray_table_ncols(input);

    /* Fall back to sequential for tiny inputs or degenerate tables
     * (fidx_rows == pred->len == table nrows — the same row count the
     * fidx_par gate keyed off, so gate and fallback can never disagree). */
    if (fidx_rows <= RAY_PARALLEL_THRESHOLD || ncols <= 0) {
        scratch_free(fidx_hdr);
        return exec_filter_seq(input, pred, ncols, pass_count);
    }

    /* VLA guard: cap at 256 columns for stack safety (256*16 = 4KB).
     * Wider tables fall back to sequential filter. */
    if (ncols > 256) {
        scratch_free(fidx_hdr);
        return exec_filter_seq(input, pred, ncols, pass_count);
    }

    /* Build match_idx: match_idx[j] = row of j-th matching element */
    ray_t* idx_hdr = NULL;
    int64_t* match_idx = (int64_t*)scratch_alloc(&idx_hdr,
                                   (size_t)pass_count * sizeof(int64_t));
    if (!match_idx) {
        scratch_free(fidx_hdr);
        return exec_filter_seq(input, pred, ncols, pass_count);
    }

    if (fidx_par) {
        fidx.match_idx = match_idx;
        ray_pool_dispatch_n(fidx_pool, fidx_fill_fn, &fidx,
                            (uint32_t)fidx_n_chunks);
    } else {
        int64_t j = 0;
        ray_morsel_t mp;
        ray_morsel_init(&mp, pred);
        int64_t row_base = 0;
        while (ray_morsel_next(&mp)) {
            uint8_t* bits = (uint8_t*)mp.morsel_ptr;
            for (int64_t i = 0; i < mp.morsel_len; i++)
                if (bits[i]) match_idx[j++] = row_base + i;
            row_base += mp.morsel_len;
        }
    }
    scratch_free(fidx_hdr);

    /* Parallel gather — same pattern as sort gather */
    ray_pool_t* pool = ray_pool_get();
    ray_t* tbl = ray_table_new(ncols);
    if (!tbl || RAY_IS_ERR(tbl)) { scratch_free(idx_hdr); return tbl; }

    /* Pre-allocate output columns */
    ray_t* new_cols[ncols];
    int64_t col_names[ncols];

    ray_t* src_cols[ncols];   /* borrowed: the plain columns read below */

    bool has_parted_cols = false;
    for (int64_t c = 0; c < ncols; c++) {
        col_names[c] = ray_table_col_name(input, c);
        /* A generation-2 slot gathers at match_idx (ascending, so its
         * bucketing is one pass) into its finished column; src_cols[c]
         * stays NULL, so every pass below skips it. */
        ray_t* enc = ray_col_gather_slot(ray_table_get_col_enc_idx(input, c));
        if (enc) {
            src_cols[c] = NULL;
            ray_t* ge = ray_col_gather_enc(enc, NULL, true, match_idx, pass_count, &new_cols[c]);
            if (ge) {
                for (int64_t k = 0; k < c; k++)
                    if (new_cols[k]) ray_release(new_cols[k]);
                scratch_free(idx_hdr);
                ray_release(tbl);
                return ge;
            }
            continue;
        }
        ray_t* col = ray_table_col_idx(input, c);
        src_cols[c] = col;
        if (col && RAY_IS_ERR(col)) {
            for (int64_t k = 0; k < c; k++)
                if (new_cols[k]) ray_release(new_cols[k]);
            scratch_free(idx_hdr);
            ray_release(tbl);
            return col;
        }
        if (!col) { new_cols[c] = NULL; continue; }
        if (col->type == RAY_MAPCOMMON) {
            /* Materialize MAPCOMMON through filter predicate */
            new_cols[c] = materialize_mapcommon_filter(col, pred, pass_count);
            if (!new_cols[c] || RAY_IS_ERR(new_cols[c])) new_cols[c] = NULL;
            continue;
        }
        int8_t out_type = RAY_IS_PARTED(col->type)
                        ? (int8_t)RAY_PARTED_BASETYPE(col->type)
                        : col->type;
        uint8_t out_attrs = 0;
        if (out_type == RAY_SYM) {
            if (RAY_IS_PARTED(col->type)) {
                ray_t** sp = (ray_t**)ray_data(col);
                out_attrs = parted_sym_max_attrs(sp, col->len);
            } else {
                out_attrs = col->attrs;
            }
        }
        if (RAY_IS_PARTED(col->type)) has_parted_cols = true;
        /* RAY_LIST == 0; typed_vec_new rejects type <= 0, which would leave
         * new_cols[c] NULL and silently DROP the LIST column.  Allocate it
         * as a proper list; elements are gathered (with a retain) in the
         * dedicated LIST pass below. */
        ray_t* nc = (out_type == RAY_LIST)
                  ? ray_list_new(pass_count)
                  : typed_vec_new(out_type, out_attrs, pass_count);
        if (!nc || RAY_IS_ERR(nc)) { new_cols[c] = NULL; continue; }
        nc->len = pass_count;
        new_cols[c] = nc;
    }

    /* LIST columns (flat or parted-LIST): gather element pointers with a
     * retain.  The byte-copy gather branches below copy raw 8-byte ray_t*
     * cells without retaining — that would alias source elements and
     * double-free on release (issue #355) — so LIST columns are filled
     * here and skipped there. */
    for (int64_t c = 0; c < ncols; c++) {
        if (!new_cols[c]) continue;
        ray_t* col = src_cols[c];
        if (!col) continue;
        if (col->type == RAY_LIST) {
            ray_t** d = (ray_t**)ray_data(new_cols[c]);
            ray_t** s = (ray_t**)ray_data(col);
            for (int64_t i = 0; i < pass_count; i++) {
                ray_t* e = s[match_idx[i]];
                if (e) ray_retain(e);
                d[i] = e;
            }
        } else if (parted_base_is_list(col)) {
            ray_release(new_cols[c]);
            new_cols[c] = parted_gather_list_rows_c((ray_t**)ray_data(col),
                                                    col->len, match_idx,
                                                    pass_count, cnt, ncnt);
            if (new_cols[c] && RAY_IS_ERR(new_cols[c])) {
                ray_error_free(new_cols[c]);
                new_cols[c] = NULL;
            }
        }
    }

    if (has_parted_cols) {
        /* Parted-aware gather: use parted_gather_col for parted columns,
         * sequential flat gather for non-parted columns */
        for (int64_t c = 0; c < ncols; c++) {
            ray_t* col = src_cols[c];
            if (!col || !new_cols[c]) continue;
            if (col->type == RAY_MAPCOMMON) continue; /* already materialized */
            if (col->type == RAY_LIST || parted_base_is_list(col))
                continue;  /* gathered above with retain */
            if (RAY_IS_PARTED(col->type)) {
                int8_t pbase = (int8_t)RAY_PARTED_BASETYPE(col->type);
                if (pbase == RAY_STR) {
                    ray_t** psegs = (ray_t**)ray_data(col);
                    ray_release(new_cols[c]);
                    new_cols[c] = parted_gather_str_rows_c(psegs, col->len,
                                                           match_idx, pass_count,
                                                           cnt, ncnt);
                } else {
                    parted_gather_col(col, match_idx, pass_count, new_cols[c],
                                      cnt, ncnt);
                }
            } else {
                uint8_t esz = col_esz(col);
                char* src = (char*)ray_data(col);
                char* dst = (char*)ray_data(new_cols[c]);
                for (int64_t i = 0; i < pass_count; i++)
                    memcpy(dst + i * esz, src + match_idx[i] * esz, esz);
            }
        }
    } else if (ray_pool_par_dispatch_ok(pool, pass_count, RAY_PARALLEL_THRESHOLD)) {
        /* Batched fused multi-column gather (≤MGATHER_MAX_COLS per dispatch) */
        multi_gather_ctx_t mgctx = { .idx = match_idx, .ncols = 0 };
        for (int64_t c = 0; c < ncols; c++) {
            if (!new_cols[c]) continue;
            ray_t* col = src_cols[c];
            if (!col) continue;                              /* generation 2: gathered above */
            if (col && col->type == RAY_MAPCOMMON) continue; /* already materialized */
            if (col && col->type == RAY_LIST) continue; /* gathered above with retain */
            int64_t ci = mgctx.ncols;
            mgctx.srcs[ci] = (char*)ray_data(col);
            mgctx.dsts[ci] = (char*)ray_data(new_cols[c]);
            mgctx.esz[ci]  = col_esz(col);
            mgctx.ncols++;
            if (mgctx.ncols == MGATHER_MAX_COLS) {
                ray_pool_dispatch(pool, multi_gather_fn, &mgctx, pass_count);
                mgctx.ncols = 0;
            }
        }
        if (mgctx.ncols > 0)
            ray_pool_dispatch(pool, multi_gather_fn, &mgctx, pass_count);
    } else {
        /* Sequential gather with index */
        for (int64_t c = 0; c < ncols; c++) {
            ray_t* col = src_cols[c];
            if (!col || !new_cols[c]) continue;
            if (col->type == RAY_LIST) continue; /* gathered above with retain */
            uint8_t esz = col_esz(col);
            char* src = (char*)ray_data(col);
            char* dst = (char*)ray_data(new_cols[c]);
            for (int64_t i = 0; i < pass_count; i++)
                memcpy(dst + i * esz, src + match_idx[i] * esz, esz);
        }
    }

    /* Propagate str_pool for any RAY_STR columns gathered by index */
    /* Propagate str_pool for non-STR parted and flat columns.
     * STR parted columns were already deep-copied with their own pool.
     * SYM outputs raw-copy cell ids — adopt the source's domain
     * (sym-domain Phase 2; no-op while all domains are the runtime
     * singleton). */
    for (int64_t c = 0; c < ncols; c++) {
        if (!new_cols[c]) continue;
        ray_t* col = src_cols[c];
        if (!col) continue;
        if (col->type == RAY_LIST || parted_base_is_list(col))
            continue;  /* elements gathered+retained; no str-pool to touch */
        if (RAY_IS_PARTED(col->type)) {
            int8_t pb = (int8_t)RAY_PARTED_BASETYPE(col->type);
            if (pb != RAY_STR) {
                ray_t** sp = (ray_t**)ray_data(col);
                col_propagate_str_pool_parted(new_cols[c], sp, col->len);
            }
        } else {
            col_propagate_str_pool(new_cols[c], col);
            /* the flat gathers above copy values only (parted columns carry
             * their null bits in parted_gather_col / _str_rows); SYM and STR
             * nulls travel in the copied cell, so only a HAS_NULLS column of
             * another type takes the pass (#784) */
            if (col->type != RAY_SYM && col->type != RAY_STR && ray_vec_may_have_nulls(col))
                col_propagate_nulls_gather(new_cols[c], col, match_idx, pass_count);
        }
        if (new_cols[c]->type == RAY_SYM)
            ray_sym_vec_adopt_domain(new_cols[c], sym_domain_rep(col));
    }

    for (int64_t c = 0; c < ncols; c++) {
        if (!new_cols[c]) continue;
        tbl = ray_table_add_col(tbl, col_names[c], new_cols[c]);
        ray_release(new_cols[c]);
    }

    scratch_free(idx_hdr);
    return tbl;
}

/* ============================================================================
 * exec_filter_head — filter table, keeping only the first `limit` matches
 *
 * Scans the predicate sequentially, collecting matching row indices and
 * stopping as soon as `limit` matches are found.  Only those rows are
 * gathered into the result table, avoiding full-table gather when the
 * number of matches far exceeds the limit.
 * ============================================================================ */
/* The rows `match_idx[0, found)` of `input`, ascending, as a flat table:
 * every kind of column a table can hold, plain, parted, encoded, or a
 * partition key.  The head of a filter and the rows a nearest-neighbour
 * search found are both gathered here. */
ray_t* ray_table_gather_rows(ray_t* input, const int64_t* match_idx, int64_t found) {
    int64_t ncnt = 0;
    const int64_t* cnt = parted_table_counts(input, &ncnt);   /* ragged partitions */
    int64_t ncols = ray_table_ncols(input);
    ray_t* tbl = ray_table_new(ncols);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    for (int64_t c = 0; c < ncols; c++) {
        int64_t name_id = ray_table_col_name(input, c);
        /* a generation-2 slot: the head's rows, gathered by range */
        ray_t* enc = ray_col_gather_slot(ray_table_get_col_enc_idx(input, c));
        if (enc) {
            ray_t* gc = NULL;
            ray_t* ge = ray_col_gather_enc(enc, NULL, true, match_idx, found, &gc);
            if (ge) { ray_release(tbl); return ge; }
            tbl = ray_table_add_col(tbl, name_id, gc);
            ray_release(gc);
            if (!tbl || RAY_IS_ERR(tbl)) { return tbl ? tbl : ray_error("oom", NULL); }
            continue;
        }
        ray_t* col = ray_table_col_idx(input, c);
        if (col && RAY_IS_ERR(col)) { ray_release(tbl); return col; }
        if (!col) continue;
        if (col->type == RAY_MAPCOMMON) {
            ray_t* mc = materialize_mapcommon_gather(col, match_idx, found);
            if (!mc || RAY_IS_ERR(mc)) {
                ray_release(tbl);
                return mc;
            }
            tbl = ray_table_add_col(tbl, name_id, mc);
            ray_release(mc);
            continue;
        }
        int8_t out_type = RAY_IS_PARTED(col->type)
                        ? (int8_t)RAY_PARTED_BASETYPE(col->type) : col->type;
        uint8_t out_attrs = 0;
        if (out_type == RAY_SYM) {
            if (RAY_IS_PARTED(col->type)) {
                ray_t** sp = (ray_t**)ray_data(col);
                out_attrs = parted_sym_max_attrs(sp, col->len);
            } else out_attrs = col->attrs;
        }
        /* LIST (flat or parted-LIST): gather element pointers with a
         * retain — the byte-copy gather below would alias them without a
         * refcount and double-free on release (issue #355).  typed_vec_new
         * also rejects RAY_LIST (type 0), which would silently drop the
         * column. */
        if (out_type == RAY_LIST) {
            ray_t* lcol;
            if (RAY_IS_PARTED(col->type)) {
                lcol = parted_gather_list_rows_c((ray_t**)ray_data(col),
                                               col->len, match_idx, found, cnt, ncnt);
            } else {
                lcol = ray_list_new(found);
                if (lcol && !RAY_IS_ERR(lcol)) {
                    lcol->len = found;
                    ray_t** d = (ray_t**)ray_data(lcol);
                    ray_t** s = (ray_t**)ray_data(col);
                    for (int64_t j = 0; j < found; j++) {
                        ray_t* e = s[match_idx[j]];
                        if (e) ray_retain(e);
                        d[j] = e;
                    }
                }
            }
            if (!lcol) continue;
            if (RAY_IS_ERR(lcol)) { ray_error_free(lcol); continue; }
            tbl = ray_table_add_col(tbl, name_id, lcol);
            ray_release(lcol);
            continue;
        }
        uint8_t esz = ray_sym_elem_size(out_type, out_attrs);
        ray_t* new_col = typed_vec_new(out_type, out_attrs, found);
        if (!new_col || RAY_IS_ERR(new_col)) continue;
        new_col->len = found;
        char* dst = (char*)ray_data(new_col);
        memset(dst, 0, (size_t)found * esz);

        if (RAY_IS_PARTED(col->type)) {
            ray_t** segs = (ray_t**)ray_data(col);
            int64_t n_segs = col->len;
            if (out_type == RAY_STR) {
                /* Deep-copy STR to handle multi-pool segments */
                ray_release(new_col);
                new_col = parted_gather_str_rows_c(segs, n_segs, match_idx, found,
                                                   cnt, ncnt);
            } else {
                /* Non-STR parted gather: the shared helper spans a NULL
                 * segment over its partition's rows and carries each
                 * present segment's null cells over. */
                parted_gather_col(col, match_idx, found, new_col, cnt, ncnt);
            }
        } else {
            char* src = (char*)ray_data(col);
            for (int64_t j = 0; j < found; j++)
                memcpy(dst + j * esz, src + match_idx[j] * esz, esz);
            col_propagate_str_pool(new_col, col);
            /* a null bitmap only where HAS_NULLS says so; SYM and STR
             * nulls travel in the copied cell (#784) */
            if (col->type != RAY_SYM && col->type != RAY_STR && ray_vec_may_have_nulls(col))
                col_propagate_nulls_gather(new_col, col, match_idx, found);
        }
        /* SYM gather copied cell ids verbatim — resolve over the source
         * column's domain (flat → col itself; parted → first SYM seg). */
        if (new_col && new_col->type == RAY_SYM)
            ray_sym_vec_adopt_domain(new_col, sym_domain_rep(col));
        tbl = ray_table_add_col(tbl, name_id, new_col);
        ray_release(new_col);
    }

    return tbl;
}

static _Atomic(uint64_t) g_cols_gathered;   /* test hook: columns a filter gathered */
uint64_t ray_filter_test_cols_gathered(void) { return atomic_load_explicit(&g_cols_gathered, memory_order_relaxed); }

ray_t* exec_filter_head(ray_t* input, ray_t* pred, int64_t limit) {
    if (!input || RAY_IS_ERR(input)) return input;
    if (!pred || RAY_IS_ERR(pred)) return pred;
    /* Over a table the predicate goes through the one where: rule
     * (ray_where_mask_coerce): a scalar or one-element BOOL is spread over
     * the rows, any other vector (a numeric or symbol column) is a type
     * error.  The checks below used to pass an atom and
     * any non-BOOL vector through as "keep the whole table" — so
     * `where: x take: 2` over an I64 column returned every row. */
    if (input->type == RAY_TABLE &&
        !(pred->type == RAY_BOOL && pred->len == ray_table_nrows(input))) {
        ray_retain(pred);
        ray_t* m = ray_where_mask_coerce(pred, ray_table_nrows(input), "where");
        if (!m || RAY_IS_ERR(m)) return m ? m : ray_error("oom", NULL);
        ray_t* r = exec_filter_head(input, m, limit);
        ray_release(m);
        return r;
    }
    /* Pass-through must hand back an OWNED ref: the exec.c caller
     * releases its own input ref and returns this value as the node
     * result, so a bare `input` would leave the result dangling once
     * that release dropped the last ref.  (NULL/error above stay bare —
     * retain/release are no-ops for both.) */
    if (input->type != RAY_TABLE || pred->type != RAY_BOOL) {
        ray_retain(input);
        return input;
    }

    int64_t ncols = ray_table_ncols(input);
    int64_t nrows = ray_table_nrows(input);
    if (ncols <= 0) return ray_table_new(0);
    if (limit <= 0) {
        /* No rows, but the table's columns: an empty table without them
         * made `(at r 'x)` fail on `take: 0`. */
        ray_t* zero = make_i64(0);
        ray_t* r = zero ? ray_take_fn(input, zero) : NULL;
        if (zero) ray_release(zero);
        if (r && !RAY_IS_ERR(r)) return r;
        if (r) ray_error_free(r);
        limit = 0;   /* a parted table takes no `take`: gather no rows */
    }
    if (limit > nrows) limit = nrows;

    /* VLA guard */
    if (ncols > 256) return ray_error("limit", "table exceeds 256 columns");

    /* Collect up to `limit` matching row indices, stopping early */
    ray_t* idx_hdr = NULL;
    int64_t* match_idx = (int64_t*)scratch_alloc(&idx_hdr,
                                    (size_t)(limit > 0 ? limit : 1) * sizeof(int64_t));
    if (!match_idx) return ray_error("oom", NULL);

    int64_t found = 0;
    {
        ray_morsel_t mp;
        ray_morsel_init(&mp, pred);
        int64_t row_base = 0;
        while (ray_morsel_next(&mp) && found < limit) {
            uint8_t* bits = (uint8_t*)mp.morsel_ptr;
            for (int64_t i = 0; i < mp.morsel_len && found < limit; i++)
                if (bits[i]) match_idx[found++] = row_base + i;
            row_base += mp.morsel_len;
        }
    }

#ifdef DEBUG
    atomic_fetch_add_explicit(&g_cols_gathered, (uint64_t)ncols, memory_order_relaxed);
#endif
    ray_t* tbl = ray_table_gather_rows(input, match_idx, found);
    scratch_free(idx_hdr);
    return tbl;
}

/* ============================================================================
 * sel_compact — materialize a table by applying a RAY_SEL bitmap
 *
 * Used at boundary ops (sort/join/window) that need dense contiguous data.
 * Reuses the same parallel multi-column gather as exec_filter.
 * ============================================================================ */

/* Parallel per-seg match_idx fill for sel_compact: each worker writes
 * its segments' passing rows at the precomputed disjoint offsets.
 * Dispatched via ray_pool_dispatch_n — one task per SEG-CHUNK ([i,i+1)),
 * segs_per_chunk segments each. */
typedef struct {
    const uint8_t*  flags;
    const uint32_t* offsets;
    const uint16_t* idx;
    const int64_t*  seg_out;   /* exclusive prefix of per-seg pass counts */
    int64_t*        match_idx;
    int64_t         nrows;
    uint32_t        n_segs;
    uint32_t        segs_per_chunk;
} scidx_ctx_t;

static void scidx_fill_fn(void* ctxp, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    scidx_ctx_t* c = (scidx_ctx_t*)ctxp;
    for (int64_t ch = start; ch < end; ch++) {
        uint32_t seg0 = (uint32_t)ch * c->segs_per_chunk;
        uint32_t seg1 = seg0 + c->segs_per_chunk;
        if (seg1 > c->n_segs) seg1 = c->n_segs;
        for (uint32_t seg = seg0; seg < seg1; seg++) {
            uint8_t f = c->flags[seg];
            if (f == RAY_SEL_NONE) continue;
            int64_t seg_start = (int64_t)seg * RAY_MORSEL_ELEMS;
            int64_t seg_end = seg_start + RAY_MORSEL_ELEMS;
            if (seg_end > c->nrows) seg_end = c->nrows;
            int64_t j = c->seg_out[seg];
            if (f == RAY_SEL_ALL) {
                for (int64_t r = seg_start; r < seg_end; r++)
                    c->match_idx[j++] = r;
            } else {
                const uint16_t* slice = c->idx + c->offsets[seg];
                uint32_t n = c->offsets[seg + 1] - c->offsets[seg];
                for (uint32_t i = 0; i < n; i++)
                    c->match_idx[j++] = seg_start + slice[i];
            }
        }
    }
}

/* A gathered SYM column's null flag.  A SYM null is id 0 and the gather
 * copied the ids verbatim, so the column holds a null exactly where the
 * output holds a 0: one pass over the output on the workers sets the
 * HAS_NULLS bit that a per-row test of the source set one row at a time. */
typedef struct {
    const void*   data;
    uint8_t       w;
    _Atomic(int)  zero;
} sym_zero_scan_t;

#define SYM_MIN_SCAN(T, MAXV) do {                                  \
        const T* p = c->data;                                       \
        T m = MAXV;                                                 \
        for (int64_t i = start; i < end; i++) m = p[i] < m ? p[i] : m; \
        mn = m;                                                     \
    } while (0)

static void sym_zero_scan_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    sym_zero_scan_t* c = raw;
    if (atomic_load_explicit(&c->zero, memory_order_relaxed)) return;
    uint64_t mn;
    switch (c->w) {
    case RAY_SYM_W8:  SYM_MIN_SCAN(uint8_t, UINT8_MAX);   break;
    case RAY_SYM_W16: SYM_MIN_SCAN(uint16_t, UINT16_MAX); break;
    case RAY_SYM_W32: SYM_MIN_SCAN(uint32_t, UINT32_MAX); break;
    default:          SYM_MIN_SCAN(uint64_t, UINT64_MAX); break;
    }
    if (mn == 0) atomic_store_explicit(&c->zero, 1, memory_order_relaxed);
}
#undef SYM_MIN_SCAN

static void sym_gather_set_nulls(ray_t* dst, int64_t n, ray_pool_t* pool) {
    if (n <= 0) return;
    sym_zero_scan_t c = { .data = ray_data(dst), .w = (uint8_t)(dst->attrs & RAY_SYM_W_MASK) };
    atomic_init(&c.zero, 0);
    if (ray_pool_par_dispatch_ok(pool, n, RAY_PARALLEL_THRESHOLD))
        ray_pool_dispatch(pool, sym_zero_scan_fn, &c, n);
    else
        sym_zero_scan_fn(&c, 0, 0, n);
    if (atomic_load_explicit(&c.zero, memory_order_relaxed)) dst->attrs |= RAY_ATTR_HAS_NULLS;
}

/* A fixed-width column under the selection, read from the selection's
 * morsels (#1122, #1757): a parted column's segments, or a flat column as
 * one segment.  A morsel whose rows all pass goes as one copy a segment it
 * spans.  A mixed morsel's positions rise strictly, so a block of them
 * whose last is its first plus the block's length less one holds
 * consecutive rows: it goes as one copy, any other block cell by cell
 * (sel_run_cells_*).  One test a block, not one a cell, and no index of
 * the kept rows: a dense selection costs about what a whole copy of the
 * rows does, a scattered one moves only its cells.  Only when every
 * segment is present and none may
 * hold nulls (a flat column's null bits follow in sel_compact): the tasks
 * then write their own output cells and nothing else.  A SYM segment saved
 * at another width than the output's converts its cells
 * (parted_copy_cells). */
typedef struct {
    ray_t* const*   segs;
    const int64_t*  seg_ends;     /* [n_segs] cumulative rows */
    int64_t         n_segs;
    const uint8_t*  flags;        /* the selection's morsels */
    const uint32_t* offsets;
    const uint16_t* idx;
    const int64_t*  mdst;         /* [n_morsels] each morsel's first output cell */
    int64_t         nrows;
    int64_t         n_morsels;
    int64_t         per_task;     /* morsels a pool task takes */
    char*           dst;
    uint8_t         esz;
    int8_t          base;
    uint8_t         out_attrs;
    bool            plain;        /* every segment's cells at the output's width */
} sel_runs_ctx_t;

/* Rows [row, row + n) to output cells [d, d + n), across the segments
 * they span; *seg is the task's segment cursor (rows ascending). */
static inline void sel_runs_span(const sel_runs_ctx_t* c, int64_t* seg, int64_t row, int64_t n, int64_t d) {
    while (n > 0) {
        while (*seg < c->n_segs - 1 && row >= c->seg_ends[*seg]) (*seg)++;
        int64_t s0 = *seg > 0 ? c->seg_ends[*seg - 1] : 0;
        int64_t k = c->seg_ends[*seg] - row;
        if (k > n) k = n;
        ray_t* sv = c->segs[*seg];
        if (!c->plain) parted_copy_cells(c->dst, c->base, c->out_attrs, d, sv, row - s0, k);
        else memcpy(c->dst + (size_t)d * c->esz, (const char*)ray_data(sv) + (size_t)(row - s0) * c->esz,
                    (size_t)k * c->esz);
        row += k; d += k; n -= k;
    }
}

/* out[q] = in[off + sl[q]] for q in [0, n), positions ascending, by blocks
 * of B positions, 64 bytes of cells: a block of consecutive rows is one
 * copy of a fixed size, a run of SEL_RUN_LONG positions one library copy,
 * any other block cell by cell. */
#define SEL_RUN_LONG 256u
#define SEL_RUN_CELLS_FN(T, NAME)                                                        \
    static void NAME(T* restrict out, const T* restrict in, int64_t off,                 \
                     const uint16_t* restrict sl, uint32_t n) {                          \
        enum { B = 64 / sizeof(T) };                                                     \
        uint32_t q = 0;                                                                  \
        while (q + B <= n) {                                                             \
            if ((uint32_t)sl[q + B - 1] - sl[q] == B - 1) {                              \
                if (q + SEL_RUN_LONG <= n && (uint32_t)sl[q + SEL_RUN_LONG - 1] - sl[q] == SEL_RUN_LONG - 1) { \
                    memcpy(out + q, in + (off + sl[q]), SEL_RUN_LONG * sizeof(T));       \
                    q += SEL_RUN_LONG;                                                   \
                } else {                                                                 \
                    memcpy(out + q, in + (off + sl[q]), B * sizeof(T));                  \
                    q += B;                                                              \
                }                                                                        \
            } else {                                                                     \
                for (uint32_t t = 0; t < B; t++) out[q + t] = in[off + sl[q + t]];       \
                q += B;                                                                  \
            }                                                                            \
        }                                                                                \
        for (; q < n; q++) out[q] = in[off + sl[q]];                                     \
    }
SEL_RUN_CELLS_FN(uint8_t,  sel_run_cells_1)
SEL_RUN_CELLS_FN(uint16_t, sel_run_cells_2)
SEL_RUN_CELLS_FN(uint32_t, sel_run_cells_4)
SEL_RUN_CELLS_FN(uint64_t, sel_run_cells_8)
#undef SEL_RUN_CELLS_FN
#undef SEL_RUN_LONG

/* A mixed morsel's n positions sl, rows base + sl[q], to output cells
 * from d: the positions in one segment at a time. */
static void sel_runs_mixed(const sel_runs_ctx_t* c, int64_t* seg, int64_t base,
                           const uint16_t* sl, uint32_t n, int64_t d) {
    uint32_t q = 0;
    while (q < n) {
        while (*seg < c->n_segs - 1 && base + sl[q] >= c->seg_ends[*seg]) (*seg)++;
        int64_t s0 = *seg > 0 ? c->seg_ends[*seg - 1] : 0, lim = c->seg_ends[*seg] - base;
        uint32_t j = n;
        if (lim < RAY_MORSEL_ELEMS && *seg < c->n_segs - 1) {   /* the segment ends inside the morsel */
            j = q + 1;
            while (j < n && sl[j] < lim) j++;
        }
        if (c->plain) {
            const void* in = ray_data(c->segs[*seg]);
            char* out = c->dst + (size_t)(d + q) * c->esz;
            int64_t off = base - s0;
            switch (c->esz) {
                case 1: sel_run_cells_1((uint8_t*)out, in, off, sl + q, j - q); break;
                case 2: sel_run_cells_2((uint16_t*)(void*)out, in, off, sl + q, j - q); break;
                case 4: sel_run_cells_4((uint32_t*)(void*)out, in, off, sl + q, j - q); break;
                case 8: sel_run_cells_8((uint64_t*)(void*)out, in, off, sl + q, j - q); break;
                default:
                    for (uint32_t i = q; i < j; ) {
                        uint32_t k = i + 1;
                        while (k < j && sl[k] == sl[i] + (k - i)) k++;
                        sel_runs_span(c, seg, base + sl[i], k - i, d + i);
                        i = k;
                    }
                    break;
            }
        } else {
            for (uint32_t i = q; i < j; ) {
                uint32_t k = i + 1;
                while (k < j && sl[k] == sl[i] + (k - i)) k++;
                sel_runs_span(c, seg, base + sl[i], k - i, d + i);
                i = k;
            }
        }
        q = j;
    }
}

_Static_assert(RAY_SEL_NONE == 0, "sel_runs_fn skips empty morsels as zero bytes");
static void sel_runs_fn(void* raw, uint32_t wid, int64_t t0, int64_t t1) {
    (void)wid;
    const sel_runs_ctx_t* c = raw;
    int64_t m0 = t0 * c->per_task, m1 = t1 * c->per_task;
    if (m1 > c->n_morsels) m1 = c->n_morsels;
    if (m0 >= m1) return;
    int64_t first = m0 * RAY_MORSEL_ELEMS;
    int64_t lo = 0, hi = c->n_segs - 1;                  /* the first morsel's segment */
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (first < c->seg_ends[mid]) hi = mid; else lo = mid + 1;
    }
    int64_t seg = lo;
    for (int64_t m = m0; m < m1; m++) {
        uint8_t f = c->flags[m];
        if (f == RAY_SEL_NONE) {
            /* the empty morsels that follow, eight flags a load */
            for (uint64_t w; m + 9 <= m1; m += 8) {
                memcpy(&w, c->flags + m + 1, 8);
                if (w) break;
            }
            continue;
        }
        int64_t base = m * RAY_MORSEL_ELEMS;
        if (f == RAY_SEL_ALL) {
            int64_t end = base + RAY_MORSEL_ELEMS < c->nrows ? base + RAY_MORSEL_ELEMS : c->nrows;
            sel_runs_span(c, &seg, base, end - base, c->mdst[m]);
        } else {
            sel_runs_mixed(c, &seg, base, c->idx + c->offsets[m],
                           c->offsets[m + 1] - c->offsets[m], c->mdst[m]);
        }
    }
}

#ifdef DEBUG
static _Atomic(uint64_t) g_parted_runs;   /* test hook: parted columns gathered on the pool by runs */
uint64_t ray_filter_test_parted_runs(void) { return atomic_load_explicit(&g_parted_runs, memory_order_relaxed); }
#endif

#ifndef SEL_RUNS_MIN
#define SEL_RUNS_MIN 32   /* kept rows, and kept rows a kept morsel, for a gather by runs */
#endif

/* Each of the selection's n morsels' first output cell: the kept rows
 * before it (RAY_SEL_ALL its rows, RAY_SEL_MIX its positions). */
static void sel_morsel_starts(const uint8_t* flags, const uint32_t* offsets, int64_t nrows,
                              uint32_t n, int64_t* out) {
    int64_t cum = 0;
    for (uint32_t m = 0; m < n; m++) {
        out[m] = cum;
        if (flags[m] == RAY_SEL_ALL) {
            int64_t lo = (int64_t)m * RAY_MORSEL_ELEMS;
            cum += (lo + RAY_MORSEL_ELEMS < nrows ? lo + RAY_MORSEL_ELEMS : nrows) - lo;
        } else if (flags[m] == RAY_SEL_MIX) {
            cum += offsets[m + 1] - offsets[m];
        }
    }
}

/* The selection's morsels, once for every column sel_compact gathers by
 * runs: each morsel's first output cell. */
typedef struct {
    const uint8_t*  flags;
    const uint32_t* offsets;
    const uint16_t* idx;
    const int64_t*  mdst;
    int64_t         nrows;
    int64_t         n_morsels;
    int64_t*        ends;         /* a parted column's cumulative rows, its segments at most */
    int64_t         count;        /* the kept rows */
} sel_runs_plan_t;

/* Can a fixed-width column's segments go by runs (sel_runs_gather)?  Every
 * segment present, and a parted one's free of nulls: a flat column's null
 * bits follow in sel_compact, a parted one's would not. */
static bool sel_runs_ok(ray_t* const* segs, int64_t n_segs, bool parted) {
    if (n_segs <= 0) return false;
    for (int64_t s = 0; s < n_segs; s++)
        if (!segs[s] || RAY_IS_ERR(segs[s]) || (parted && ray_vec_may_have_nulls(segs[s]))) return false;
    return true;
}

/* A flat column sel_compact gathers by runs: fixed width of a cell of 1, 2,
 * 4 or 8 bytes, its cells copied as they are. */
static bool sel_runs_flat_ok(const ray_t* col) {
    if (col->type == RAY_LIST || col->type == RAY_MAPCOMMON || col->type == RAY_STR ||
        RAY_IS_PARTED(col->type) || col->type <= 0) return false;
    uint8_t e = col_esz(col);
    return e == 1 || e == 2 || e == 4 || e == 8;
}

/* Gather a column's kept rows into dst by runs (sel_runs_fn): segs[0,
 * n_segs) with sel_runs_ok, ends their cumulative rows.  False when the
 * pool's dispatch was cancelled: dst's cells are then not all written. */
static bool sel_runs_gather(ray_pool_t* pool, const sel_runs_plan_t* p, ray_t* const* segs,
                            const int64_t* ends, int64_t n_segs, int8_t base, bool parted, ray_t* dst) {
    uint8_t out_attrs = base == RAY_SYM ? dst->attrs : 0;
    bool plain = true;
    for (int64_t s = 0; s < n_segs && plain; s++)
        plain = base != RAY_SYM || ((segs[s]->attrs ^ out_attrs) & RAY_SYM_W_MASK) == 0;
    sel_runs_ctx_t c = {
        .segs = segs, .seg_ends = ends, .n_segs = n_segs,
        .flags = p->flags, .offsets = p->offsets, .idx = p->idx, .mdst = p->mdst,
        .nrows = p->nrows, .n_morsels = p->n_morsels, .per_task = p->n_morsels,
        .dst = (char*)ray_data(dst), .esz = (uint8_t)ray_sym_elem_size(base, out_attrs),
        .base = base, .out_attrs = out_attrs, .plain = plain,
    };
    if (p->n_morsels > 1 && ray_pool_par_dispatch_ok(pool, p->count, RAY_PARALLEL_THRESHOLD)) {
        /* morsels a task, so the tasks fit the pool's ring (see sel_compact's index fill) */
        int64_t per = 1;
        while ((p->n_morsels + per - 1) / per > RAY_POOL_INIT_TASKS) per *= 2;
        c.per_task = per;
        ray_pool_dispatch_n(pool, sel_runs_fn, &c, (uint32_t)((p->n_morsels + per - 1) / per));
#ifdef DEBUG
        if (parted) atomic_fetch_add_explicit(&g_parted_runs, 1, memory_order_relaxed);
#endif
        if (pool_cancelled(pool)) return false;          /* skipped tasks */
    } else {
        sel_runs_fn(&c, 0, 0, 1);
    }
    (void)parted;
    return true;
}

/* A parted fixed-width column (not STR or LIST) by runs when by_runs
 * (sel_runs_ok, and the plan's buffers allocated); parted_gather_col from
 * the rows idx otherwise.  False on a cancelled dispatch. */
static bool sel_parted_gather(ray_pool_t* pool, const sel_runs_plan_t* p, ray_t* col, bool by_runs,
                              const int64_t* idx, ray_t* dst, const int64_t* cnt, int64_t ncnt) {
    if (!by_runs) { parted_gather_col(col, idx, p->count, dst, cnt, ncnt); return true; }
    int64_t n_segs = col->len;
    ray_t** segs = (ray_t**)ray_data(col);
    int64_t cum = 0;
    for (int64_t s = 0; s < n_segs; s++) p->ends[s] = cum += segs[s]->len;
    return sel_runs_gather(pool, p, segs, p->ends, n_segs, (int8_t)RAY_PARTED_BASETYPE(col->type), true, dst);
}

/* The kept rows as an int64 index (*out, its block *hdr), for the columns
 * that read it.  Per-seg pass counts are derivable from the rowsel itself
 * (ALL→seg_len, MIX→offsets delta, NONE→0), so this parallelizes as: tiny
 * serial prefix over segs (mdst when the caller has it) → parallel per-seg
 * fill at disjoint offsets.  *out stays NULL when the index cannot be
 * allocated; a cancelled fill answers the cancel, the index freed. */
static inline ray_t* sel_compact_idx(ray_t* sel, int64_t nrows, int64_t pass_count, const int64_t* mdst,
                              int64_t** out, ray_t** hdr) {
    const uint8_t*  flags   = ray_rowsel_flags(sel);
    const uint32_t* offsets = ray_rowsel_offsets(sel);
    const uint16_t* sidx    = ray_rowsel_idx(sel);
    uint32_t n_segs = ray_rowsel_meta(sel)->n_segs;
    int64_t* match_idx = (int64_t*)scratch_alloc(hdr, (size_t)pass_count * sizeof(int64_t));
    *out = NULL;
    if (!match_idx) return NULL;
    ray_pool_t* sc_pool = ray_pool_get();
    if (ray_pool_par_dispatch_ok(sc_pool, nrows, RAY_PARALLEL_THRESHOLD)) {
        ray_t* soff_hdr = NULL;
        int64_t* own = mdst ? NULL : (int64_t*)scratch_alloc(&soff_hdr, (size_t)n_segs * sizeof(int64_t));
        if (own) sel_morsel_starts(flags, offsets, nrows, n_segs, own);
        const int64_t* seg_out = mdst ? mdst : own;
        if (seg_out) {
            /* One task per seg-chunk (dispatch_n; ray_pool_dispatch would
             * morselize the seg RANGE by 8192 segs → 1 task under 8.4M
             * rows).  Chunk count capped at 1024 = the pool's initial ring
             * capacity, so the ring never needs to grow (see exec_filter). */
            uint32_t spc = 64;
            while ((n_segs + spc - 1) / spc > RAY_POOL_INIT_TASKS) spc *= 2;
            uint32_t sc_chunks = (n_segs + spc - 1) / spc;
            scidx_ctx_t sc = {
                .flags = flags, .offsets = offsets, .idx = sidx,
                .seg_out = seg_out, .match_idx = match_idx, .nrows = nrows,
                .n_segs = n_segs, .segs_per_chunk = spc,
            };
            ray_pool_dispatch_n(sc_pool, scidx_fill_fn, &sc, sc_chunks);
            scratch_free(soff_hdr);
            /* a cancelled dispatch skips its tasks: match_idx is unfilled
             * and every gather would read its garbage as rows */
            if (pool_cancelled(sc_pool)) {
                scratch_free(*hdr);
                *hdr = NULL;
                return ray_error("cancel", NULL);
            }
            *out = match_idx;
            return NULL;
        }
    }
    int64_t j = 0;
    for (uint32_t seg = 0; seg < n_segs; seg++) {
        uint8_t f = flags[seg];
        if (f == RAY_SEL_NONE) continue;
        int64_t seg_start = (int64_t)seg * RAY_MORSEL_ELEMS;
        int64_t seg_end = seg_start + RAY_MORSEL_ELEMS;
        if (seg_end > nrows) seg_end = nrows;
        if (f == RAY_SEL_ALL) {
            for (int64_t r = seg_start; r < seg_end; r++)
                match_idx[j++] = r;
        } else {
            const uint16_t* slice = sidx + offsets[seg];
            uint32_t n = offsets[seg + 1] - offsets[seg];
            for (uint32_t i = 0; i < n; i++)
                match_idx[j++] = seg_start + slice[i];
        }
    }
    *out = match_idx;
    return NULL;
}

/* keep-set membership: is column name-sym `nm` present in keep_syms? */
static inline bool sel_compact_keep(int64_t nm, const int64_t* keep_syms, int keep_n) {
    for (int j = 0; j < keep_n; j++)
        if (keep_syms[j] == nm) return true;
    return false;
}

ray_t* sel_compact(ray_graph_t* g, ray_t* tbl, ray_t* sel,
                   const int64_t* keep_syms, int keep_n) {
    int64_t ncnt = 0;
    const int64_t* cnt = parted_table_counts(tbl, &ncnt);   /* ragged partitions */
    /* A projection of parted columns carries no partition-key column: the
     * counts come from the graph's source table, whose partitions it shares. */
    if (!cnt && g && g->table) cnt = parted_table_counts(g->table, &ncnt);
    (void)g;
    if (!tbl || RAY_IS_ERR(tbl) || !sel) return tbl;

    /* keep-set: when provided, materialize only columns whose name-sym is in
     * keep_syms (order preserved); NULL/0 => materialize all columns. */
    bool use_keep = (keep_syms != NULL && keep_n > 0);

    /* The table's rows: through the partition counts when its first column
     * is a parted column with missing segments (ragged parted table). */
    int64_t nrows = ray_table_nrows(tbl);
    {
        ray_col_meta_t fm;   /* metadata first: an encoded column stays encoded */
        bool parted = ray_table_ncols(tbl) > 0 && ray_table_col_meta_idx(tbl, 0, &fm) &&
                      RAY_IS_PARTED(fm.type);
        /* an encoded parted column has every segment (lazy all-null ones
         * for a ragged partition): its rows are its length */
        if (parted && fm.enc) { parted = false; nrows = fm.len; }
        ray_t* first = parted ? ray_table_col_idx(tbl, 0) : NULL;
        if (first && RAY_IS_ERR(first)) return first;
        if (first && RAY_IS_PARTED(first->type))
            nrows = parted_rows_counted(first, cnt, ncnt);
    }
    ray_rowsel_t* meta = ray_rowsel_meta(sel);

    /* Defensive: the selection must have been built for a table
     * with this exact row count.  Mismatch means the caller passed
     * a stale selection — aborting here is strictly safer than
     * silently gathering via out-of-range indices. */
    if (meta->nrows != nrows)
        return ray_error("domain",
            "sel_compact: selection nrows mismatch (sel=%lld tbl=%lld)",
            (long long)meta->nrows, (long long)nrows);

    int64_t pass_count = meta->total_pass;

    /* All-pass: nothing to compact.  (In practice this path is
     * unreachable because ray_rowsel_from_pred returns NULL for
     * all-pass; the caller skips sel_compact in that case.
     * Handled here for safety.) */
    if (pass_count == nrows) { ray_retain(tbl); return tbl; }

    /* None-pass: return empty table with same schema */
    if (pass_count == 0) {
        int64_t ncols = ray_table_ncols(tbl);
        ray_t* empty = ray_table_new(ncols);
        if (!empty || RAY_IS_ERR(empty)) return empty ? empty : ray_error("oom", NULL);
        for (int64_t c = 0; c < ncols; c++) {
            if (use_keep &&
                !sel_compact_keep(ray_table_col_name(tbl, c), keep_syms, keep_n))
                continue;
            ray_col_meta_t cm;
            if (!ray_table_col_meta_idx(tbl, c, &cm)) continue;
            if (cm.type == RAY_MAPCOMMON) {
                ray_t* col = ray_table_col_idx(tbl, c);
                if (!col || RAY_IS_ERR(col)) { ray_release(empty); return col; }
                ray_t* mc = materialize_mapcommon_head(col, 0);
                if (!mc || RAY_IS_ERR(mc)) { ray_release(empty); return mc ? mc : ray_error("oom", NULL); }
                empty = ray_table_add_col(empty, ray_table_col_name(tbl, c), mc);
                ray_release(mc);
                if (!empty || RAY_IS_ERR(empty)) return empty ? empty : ray_error("oom", NULL);
                continue;
            }
            int8_t ct = RAY_IS_PARTED(cm.type)
                      ? (int8_t)RAY_PARTED_BASETYPE(cm.type) : cm.type;
            /* RAY_LIST == 0; ray_vec_new rejects type <= 0, so a LIST
             * column needs ray_list_new — otherwise the empty result
             * silently drops it and the 0-row schema loses a column. */
            /* Even an empty partial can supply the symbol dictionary for a merge. */
            ray_t* nc = ct == RAY_SYM ? ray_tbl_col_empty(tbl, ray_table_col_name(tbl, c))
                      : ct == RAY_LIST ? ray_list_new(0) : ray_vec_new(ct, 0);
            if (!nc || RAY_IS_ERR(nc)) { ray_release(empty); return nc ? nc : ray_error("oom", NULL); }
            nc->len = 0;
            empty = ray_table_add_col(empty, ray_table_col_name(tbl, c), nc);
            ray_release(nc);
            if (!empty || RAY_IS_ERR(empty)) return empty ? empty : ray_error("oom", NULL);
        }
        return empty;
    }

    int64_t ncols = ray_table_ncols(tbl);
    if (ncols <= 0) { ray_retain(tbl); return tbl; }

    /* Gather by runs only when it pays for its setup a column and its walk
     * of the morsels: SEL_RUNS_MIN kept rows at least, and as many on
     * average in each morsel that keeps any.  Fewer, and the index of the
     * kept rows, built once for every column, costs less. */
    const uint8_t*  flags   = ray_rowsel_flags(sel);
    const uint32_t* offsets = ray_rowsel_offsets(sel);
    const uint16_t* sidx    = ray_rowsel_idx(sel);
    uint32_t n_segs = meta->n_segs;
    bool try_runs = pass_count >= SEL_RUNS_MIN;
    if (try_runs) {
        int64_t kept_morsels = 0;
        for (uint32_t m = 0; m < n_segs; m++) kept_morsels += flags[m] != RAY_SEL_NONE;
        try_runs = pass_count >= kept_morsels * SEL_RUNS_MIN;
    }

    /* Not by runs: match_idx first, as every column reads it */
    ray_t* idx_hdr = NULL;
    int64_t* match_idx = NULL;
    if (!try_runs) {
        ray_t* fail = sel_compact_idx(sel, nrows, pass_count, NULL, &match_idx, &idx_hdr);
        if (fail) return fail;
        /* never the table as it is: that would be every row (#1818) */
        if (!match_idx) return ray_error("oom", NULL);
    }

    /* Parallel multi-column gather (same pattern as exec_filter) */
    ray_pool_t* pool = ray_pool_get();
    ray_t* out = ray_table_new(ncols);
    if (!out || RAY_IS_ERR(out)) { scratch_free(idx_hdr); return out ? out : ray_error("oom", NULL); }

    /* VLA guard: 256 cols max for stack arrays */
    if (ncols > 256) { scratch_free(idx_hdr); ray_release(out); return ray_error("limit", "table exceeds 256 columns"); }

    ray_t* new_cols[ncols];
    ray_t* src_cols[ncols];   /* borrowed: the plain kept columns */
    int64_t col_names[ncols];
    bool by_runs[ncols];      /* gathered from the selection's morsels (sel_runs_gather): read only when any_runs */
    bool has_parted = false;
    bool need_idx = false;    /* a column reads the kept rows as match_idx */
    bool any_runs = false;    /* a column gathers by runs */
    bool has_mapc = false;    /* a MAPCOMMON column */
    int64_t max_segs = 0;
    /* A failure from here on is the answer (sc_fail), never a table short
     * of a column or the rows unselected (#1818). */
    memset(new_cols, 0, sizeof new_cols);
    ray_t* plan_hdr = NULL;
    ray_t* sc_err = NULL;


    for (int64_t c = 0; c < ncols; c++) {
        col_names[c] = ray_table_col_name(tbl, c);
        src_cols[c] = NULL;
        if (use_keep && !sel_compact_keep(col_names[c], keep_syms, keep_n)) {
            new_cols[c] = NULL; continue;
        }
#ifdef DEBUG
        atomic_fetch_add_explicit(&g_cols_gathered, 1, memory_order_relaxed);
#endif
        /* A generation-2 slot (single file or parted) gathers under the
         * selection into its finished column; src_cols[c] stays NULL, so
         * the batch gather, the LIST and parted passes and the fix-ups
         * below skip it. */
        ray_t* enc = ray_col_gather_slot(ray_table_get_col_enc_idx(tbl, c));
        if (enc) {
            ray_t* ge = ray_col_gather_enc(enc, sel, false, NULL, 0, &new_cols[c]);
            if (ge) {
                for (int64_t k = 0; k < c; k++)
                    if (new_cols[k]) ray_release(new_cols[k]);
                scratch_free(idx_hdr);
                ray_release(out);
                return ge;
            }
            continue;
        }
        ray_t* col = ray_table_col_idx(tbl, c);
        if (col && RAY_IS_ERR(col)) {
            for (int64_t k = 0; k < c; k++)
                if (new_cols[k]) ray_release(new_cols[k]);
            scratch_free(idx_hdr);
            ray_release(out);
            return col;
        }
        if (!col) { new_cols[c] = NULL; continue; }
        src_cols[c] = col;
        if (col->type == RAY_MAPCOMMON) {
            /* materialized from match_idx once it is built, below */
            new_cols[c] = NULL;
            need_idx = has_mapc = true;
            continue;
        }
        int8_t ct = RAY_IS_PARTED(col->type)
                  ? (int8_t)RAY_PARTED_BASETYPE(col->type) : col->type;
        uint8_t ca = 0;
        if (ct == RAY_SYM) {
            if (RAY_IS_PARTED(col->type)) {
                ray_t** sp = (ray_t**)ray_data(col);
                ca = parted_sym_max_attrs(sp, col->len);
            } else ca = col->attrs;
        }
        if (RAY_IS_PARTED(col->type)) has_parted = true;
        /* RAY_LIST == 0; typed_vec_new/ray_vec_new reject type <= 0, which
         * would leave new_cols[c] NULL and silently DROP the LIST column.
         * Allocate it as a proper list; its elements are gathered (with a
         * retain) in the dedicated LIST pass below. */
        ray_t* nc = (ct == RAY_LIST)
                  ? ray_list_new(pass_count)
                  : typed_vec_new(ct, ca, pass_count);
        if (!nc || RAY_IS_ERR(nc)) { sc_err = nc ? nc : ray_error("oom", NULL); goto sc_fail; }
        nc->len = pass_count;
        new_cols[c] = nc;
    }

    /* Which columns gather by runs, and which read match_idx */
    for (int64_t c = 0; try_runs && c < ncols; c++) {
        ray_t* col = src_cols[c];
        by_runs[c] = false;
        if (!col || !new_cols[c]) continue;
        if (RAY_IS_PARTED(col->type)) {
            if (RAY_PARTED_BASETYPE(col->type) != RAY_STR && !parted_base_is_list(col) &&
                sel_runs_ok((ray_t* const*)ray_data(col), col->len, true)) {
                by_runs[c] = true;
                if (col->len > max_segs) max_segs = col->len;
            }
        } else if (sel_runs_flat_ok(col)) {
            by_runs[c] = true;
            /* its null bits from match_idx, but for a SYM's (a zero cell,
             * sym_gather_set_nulls) */
            if (ray_vec_may_have_nulls(col) &&
                !(col->type == RAY_SYM && !(col->attrs & RAY_ATTR_SLICE)))
                need_idx = true;
        }
        if (by_runs[c]) any_runs = true; else need_idx = true;
    }

    /* The selection's morsels: each one's first output cell, for the
     * columns gathered by runs.  Without the buffers they read match_idx. */
    sel_runs_plan_t plan = {
        .flags = flags, .offsets = offsets, .idx = sidx, .nrows = nrows,
        .n_morsels = n_segs, .count = pass_count,
    };
    int64_t* plan_buf = any_runs
        ? (int64_t*)scratch_alloc(&plan_hdr, ((size_t)n_segs + (size_t)max_segs) * sizeof(int64_t))
        : NULL;
    if (plan_buf) {
        sel_morsel_starts(flags, offsets, nrows, n_segs, plan_buf);
        plan.mdst = plan_buf;
        plan.ends = plan_buf + n_segs;
    } else if (any_runs) {
        for (int64_t c = 0; c < ncols; c++) by_runs[c] = false;
        any_runs = false;
        need_idx = true;
    }

    /* match_idx for the columns that read it, when not built up front */
    if (need_idx && !match_idx) {
        ray_t* fail = sel_compact_idx(sel, nrows, pass_count, plan.mdst, &match_idx, &idx_hdr);
        if (fail || !match_idx) {   /* a cancel, or no index: oom */
            sc_err = fail ? fail : ray_error("oom", NULL);
            goto sc_fail;
        }
    }

    /* MAPCOMMON: each kept row's partition value */
    for (int64_t c = 0; has_mapc && c < ncols; c++) {
        ray_t* col = src_cols[c];
        if (!col || col->type != RAY_MAPCOMMON) continue;
        ray_t** mc_ptrs = (ray_t**)ray_data(col);
        ray_t* kv = mc_ptrs[0];
        ray_t* rc = mc_ptrs[1];
        if (!kv || !rc || col->len < 2) continue;
        int64_t n_parts = kv->len;
        int8_t kv_type = kv->type;
        size_t esz = (size_t)ray_sym_elem_size(kv_type, kv->attrs);
        const char* kdata = (const char*)ray_data(kv);
        const int64_t* counts = (const int64_t*)ray_data(rc);
        ray_t* flat = ray_vec_new(kv_type, pass_count);
        if (!flat || RAY_IS_ERR(flat)) { sc_err = flat ? flat : ray_error("oom", NULL); goto sc_fail; }
        flat->len = pass_count;
        char* out_mc = (char*)ray_data(flat);
        for (int64_t i = 0; i < pass_count; i++) {
            int64_t row_i = match_idx[i];
            int64_t cum = 0;
            int64_t pi = 0;
            for (; pi < n_parts - 1; pi++) {
                cum += counts[pi];
                if (row_i < cum) break;
            }
            memcpy(out_mc + (size_t)i * esz, kdata + (size_t)pi * esz, esz);
        }
        new_cols[c] = flat;
    }

    /* LIST columns (flat or parted-LIST): gather element pointers with a
     * retain.  The byte-copy gather paths below copy raw esz bytes (8-byte
     * ray_t* pointers) without retaining — that would alias source elements
     * and double-free on release — so LIST columns are filled here and
     * skipped there.  A parted LIST column's type is RAY_PARTED_BASE +
     * RAY_LIST, not RAY_LIST, so it must be matched explicitly (issue
     * #355). */
    for (int64_t c = 0; c < ncols; c++) {
        if (!new_cols[c]) continue;
        ray_t* col = src_cols[c];
        if (!col) continue;
        if (parted_base_is_list(col)) {
            ray_release(new_cols[c]);
            new_cols[c] = parted_gather_list_rows_c((ray_t**)ray_data(col),
                                                  col->len, match_idx,
                                                  pass_count, cnt, ncnt);
            if (!new_cols[c] || RAY_IS_ERR(new_cols[c])) {
                sc_err = new_cols[c] ? new_cols[c] : ray_error("oom", NULL);
                new_cols[c] = NULL;
                goto sc_fail;
            }
            continue;
        }
        if (col->type != RAY_LIST) continue;
        ray_t** d = (ray_t**)ray_data(new_cols[c]);
        ray_t** s = (ray_t**)ray_data(col);
        for (int64_t i = 0; i < pass_count; i++) {
            ray_t* e = s[match_idx[i]];
            d[i] = e;
            if (e) ray_retain(e);
        }
    }

    /* Fixed-width columns by runs from the selection's morsels; a flat
     * column is one segment.  A cancelled dispatch leaves cells unwritten:
     * the compaction answers the cancel. */
    bool cancelled = false;
    for (int64_t c = 0; any_runs && c < ncols && !cancelled; c++) {
        ray_t* col = src_cols[c];
        if (!col || !new_cols[c] || !by_runs[c] || RAY_IS_PARTED(col->type)) continue;
        ray_t* one[1] = { col };
        int64_t end[1] = { nrows };
        cancelled = !sel_runs_gather(pool, &plan, one, end, 1, col->type, false, new_cols[c]);
    }

    if (cancelled) {
        /* no other gather: the cancel below */
    } else if (has_parted) {
        for (int64_t c = 0; c < ncols && !cancelled; c++) {
            ray_t* col = src_cols[c];
            if (!col || !new_cols[c]) continue;
            if (col->type == RAY_LIST || parted_base_is_list(col) || col->type == RAY_MAPCOMMON)
                continue;  /* gathered above */
            if (RAY_IS_PARTED(col->type)) {
                int8_t pbase = (int8_t)RAY_PARTED_BASETYPE(col->type);
                if (pbase == RAY_STR) {
                    ray_t** psegs = (ray_t**)ray_data(col);
                    ray_release(new_cols[c]);
                    new_cols[c] = parted_gather_str_rows_c(psegs, col->len,
                                                           match_idx, pass_count,
                                                           cnt, ncnt);
                    if (!new_cols[c] || RAY_IS_ERR(new_cols[c])) {
                        sc_err = new_cols[c] ? new_cols[c] : ray_error("oom", NULL);
                        new_cols[c] = NULL;
                        goto sc_fail;
                    }
                } else {
                    cancelled = !sel_parted_gather(pool, &plan, col, any_runs && by_runs[c], match_idx,
                                                   new_cols[c], cnt, ncnt);
                }
            } else if (!(any_runs && by_runs[c])) {
                uint8_t esz = col_esz(col);
                char* src = (char*)ray_data(col);
                char* dst = (char*)ray_data(new_cols[c]);
                for (int64_t i = 0; i < pass_count; i++)
                    memcpy(dst + i * esz, src + match_idx[i] * esz, esz);
            }
        }
    } else if (match_idx && ray_pool_par_dispatch_ok(pool, pass_count, RAY_PARALLEL_THRESHOLD)) {
        /* Batched multi-column gather: fill mgctx with up to MGATHER_MAX_COLS
         * kept columns, dispatch, then continue with the next batch.  This is
         * column-count-agnostic — no per-column fallback. */
        multi_gather_ctx_t mgctx = { .idx = match_idx, .ncols = 0 };
        for (int64_t c = 0; c < ncols; c++) {
            if (!new_cols[c] || (any_runs && by_runs[c])) continue;
            ray_t* col = src_cols[c];
            if (!col) continue;                   /* generation 2: gathered above */
            if (col->type == RAY_LIST || col->type == RAY_MAPCOMMON) continue;  /* gathered above */
            int64_t ci = mgctx.ncols;
            mgctx.srcs[ci] = (char*)ray_data(col);
            mgctx.dsts[ci] = (char*)ray_data(new_cols[c]);
            mgctx.esz[ci]  = col_esz(col);
            mgctx.ncols++;
            if (mgctx.ncols == MGATHER_MAX_COLS) {
                ray_pool_dispatch(pool, multi_gather_fn, &mgctx, pass_count);
                mgctx.ncols = 0;
            }
        }
        if (mgctx.ncols > 0)
            ray_pool_dispatch(pool, multi_gather_fn, &mgctx, pass_count);
    } else if (match_idx) {
        for (int64_t c = 0; c < ncols; c++) {
            ray_t* col = src_cols[c];
            if (!col || !new_cols[c] || (any_runs && by_runs[c])) continue;
            if (col->type == RAY_LIST || col->type == RAY_MAPCOMMON) continue;  /* gathered above */
            uint8_t esz = col_esz(col);
            char* src = (char*)ray_data(col);
            char* dst = (char*)ray_data(new_cols[c]);
            for (int64_t i = 0; i < pass_count; i++)
                memcpy(dst + i * esz, src + match_idx[i] * esz, esz);
        }
    }

    if (cancelled) {
        for (int64_t c = 0; c < ncols; c++)
            if (new_cols[c]) ray_release(new_cols[c]);
        ray_release(out);
        scratch_free(idx_hdr);
        scratch_free(plan_hdr);
        return ray_error("cancel", NULL);
    }

    for (int64_t c = 0; c < ncols; c++) {
        if (!new_cols[c]) continue;
        ray_t* scol = src_cols[c];
        if (scol && (scol->type == RAY_LIST || parted_base_is_list(scol))) {
            /* LIST elements already gathered+retained; no str-pool / null
             * bitmap to propagate. */
        } else if (scol && RAY_IS_PARTED(scol->type)) {
            int8_t pb = (int8_t)RAY_PARTED_BASETYPE(scol->type);
            if (pb != RAY_STR) {
                ray_t** sp = (ray_t**)ray_data(scol);
                col_propagate_str_pool_parted(new_cols[c], sp, scol->len);
            }
            /* Parted null propagation handled in parted_gather_col / parted_gather_str_rows */
        } else if (scol) {
            col_propagate_str_pool(new_cols[c], scol);
            if (scol->type == RAY_SYM && !(scol->attrs & RAY_ATTR_SLICE) && new_cols[c]->type == RAY_SYM &&
                (new_cols[c]->attrs & RAY_SYM_W_MASK) == (scol->attrs & RAY_SYM_W_MASK))
                sym_gather_set_nulls(new_cols[c], pass_count, pool);
            else if (match_idx)   /* built whenever such a column may hold nulls */
                col_propagate_nulls_gather(new_cols[c], scol, match_idx, pass_count);
        }
        /* SYM gathers above copied cell ids verbatim — resolve over the
         * source column's domain (flat → scol; parted → first SYM seg;
         * MAPCOMMON → keys vec, all via sym_domain_rep). */
        if (scol && new_cols[c]->type == RAY_SYM)   /* a gathered slot (scol NULL) holds its domain */
            ray_sym_vec_adopt_domain(new_cols[c], sym_domain_rep(scol));
        out = ray_table_add_col(out, col_names[c], new_cols[c]);
        ray_release(new_cols[c]);
        new_cols[c] = NULL;
    }

    scratch_free(idx_hdr);
    scratch_free(plan_hdr);
    return out;

sc_fail:
    for (int64_t c = 0; c < ncols; c++)
        if (new_cols[c]) ray_release(new_cols[c]);
    if (out) ray_release(out);
    scratch_free(idx_hdr);
    scratch_free(plan_hdr);
    return sc_err;
}
