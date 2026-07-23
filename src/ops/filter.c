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

/* ============================================================================
 * Filter execution — extracted from exec.c
 * ============================================================================ */

/* Gather from a parted column using global row indices (sorted ascending).
 * Walks match_idx with an advancing segment cursor — O(count + n_segs). */
static void parted_gather_col(ray_t* parted_col, const int64_t* match_idx,
                               int64_t count, ray_t* dst_col) {
    int64_t n_segs = parted_col->len;
    if (n_segs == 0) return;  /* zero-length VLA is UB in C17 */
    ray_t** segs = (ray_t**)ray_data(parted_col);
    int8_t base = (int8_t)RAY_PARTED_BASETYPE(parted_col->type);
    /* Output width is fixed by dst_col (callers allocate it at the max
     * segment width); narrower segments are widened per cell. */
    uint8_t out_attrs = (base == RAY_SYM) ? dst_col->attrs : 0;
    uint8_t esz = ray_sym_elem_size(base, out_attrs);
    char* dst = (char*)ray_data(dst_col);
    memset(dst, 0, (size_t)count * esz);

    /* Build prefix-sum segment end table */
    int64_t seg_ends[n_segs];
    int64_t cumul = 0;
    for (int64_t i = 0; i < n_segs; i++) {
        cumul += segs[i] ? segs[i]->len : 0;
        seg_ends[i] = cumul;
    }

    /* Walk match_idx (sorted ascending) with advancing segment cursor */
    int64_t seg = 0;
    for (int64_t i = 0; i < count; i++) {
        int64_t row = match_idx[i];
        while (seg < n_segs - 1 && row >= seg_ends[seg]) seg++;
        if (!segs[seg])
            continue;  /* NULL segment — zero-fill from vec_new */
        int64_t seg_start = (seg > 0) ? seg_ends[seg - 1] : 0;
        int64_t local_row = row - seg_start;
        parted_copy_cells(dst, base, out_attrs, i, segs[seg], local_row, 1);
        if ((segs[seg]->attrs & RAY_ATTR_HAS_NULLS) &&
            ray_vec_is_null(segs[seg], local_row))
            ray_vec_set_null(dst_col, i, true);
    }
}

/* Filter a single vector by boolean predicate. */
static ray_t* exec_filter_vec(ray_t* input, ray_t* pred, int64_t pass_count) {
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

/* Filter a parted column by boolean predicate (sequential). */
static ray_t* exec_filter_parted_vec(ray_t* parted_col, ray_t* pred,
                                     int64_t pass_count) {
    int8_t base = (int8_t)RAY_PARTED_BASETYPE(parted_col->type);
    ray_t** segs = (ray_t**)ray_data(parted_col);
    int64_t n_segs = parted_col->len;
    uint8_t* pred_data = (uint8_t*)ray_data(pred);

    /* RAY_STR: deep-copy to handle multi-pool segments */
    if (base == RAY_STR) {
        ray_t* result = ray_vec_new(RAY_STR, pass_count);
        if (!result || RAY_IS_ERR(result)) return result;
        int64_t pred_off = 0;
        for (int64_t s = 0; s < n_segs; s++) {
            if (!segs[s]) continue;
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
        if (!segs[s]) continue;
        int64_t seg_len = segs[s]->len;
        char* dst = (char*)ray_data(result);
        bool seg_has_nulls = (segs[s]->attrs & RAY_ATTR_HAS_NULLS) != 0;
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
    ray_t* tbl = ray_table_new(ncols);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(input, c);
        if (!col || RAY_IS_ERR(col)) continue;
        int64_t name_id = ray_table_col_name(input, c);
        if (col->type == RAY_MAPCOMMON) {
            ray_t* mc_filt = materialize_mapcommon_filter(col, pred, pass_count);
            if (!mc_filt || RAY_IS_ERR(mc_filt)) { ray_release(tbl); return mc_filt; }
            tbl = ray_table_add_col(tbl, name_id, mc_filt);
            ray_release(mc_filt);
            continue;
        }
        ray_t* filtered;
        if (RAY_IS_PARTED(col->type))
            filtered = exec_filter_parted_vec(col, pred, pass_count);
        else
            filtered = exec_filter_vec(col, pred, pass_count);
        if (!filtered || RAY_IS_ERR(filtered)) { ray_release(tbl); return filtered; }
        tbl = ray_table_add_col(tbl, name_id, filtered);
        ray_release(filtered);
    }
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

ray_t* exec_filter(ray_graph_t* g, ray_op_t* op, ray_t* input, ray_t* pred) {
    (void)g;
    (void)op;
    if (!input || RAY_IS_ERR(input)) return input;
    if (!pred || RAY_IS_ERR(pred)) return pred;

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

    bool has_parted_cols = false;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(input, c);
        col_names[c] = ray_table_col_name(input, c);
        if (!col || RAY_IS_ERR(col)) { new_cols[c] = NULL; continue; }
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
        ray_t* nc = typed_vec_new(out_type, out_attrs, pass_count);
        if (!nc || RAY_IS_ERR(nc)) { new_cols[c] = NULL; continue; }
        nc->len = pass_count;
        new_cols[c] = nc;
    }

    if (has_parted_cols) {
        /* Parted-aware gather: use parted_gather_col for parted columns,
         * sequential flat gather for non-parted columns */
        for (int64_t c = 0; c < ncols; c++) {
            ray_t* col = ray_table_get_col_idx(input, c);
            if (!col || !new_cols[c]) continue;
            if (col->type == RAY_MAPCOMMON) continue; /* already materialized */
            if (RAY_IS_PARTED(col->type)) {
                int8_t pbase = (int8_t)RAY_PARTED_BASETYPE(col->type);
                if (pbase == RAY_STR) {
                    ray_t** psegs = (ray_t**)ray_data(col);
                    ray_release(new_cols[c]);
                    new_cols[c] = parted_gather_str_rows(psegs, col->len,
                                                         match_idx, pass_count);
                } else {
                    parted_gather_col(col, match_idx, pass_count, new_cols[c]);
                }
            } else {
                uint8_t esz = col_esz(col);
                char* src = (char*)ray_data(col);
                char* dst = (char*)ray_data(new_cols[c]);
                for (int64_t i = 0; i < pass_count; i++)
                    memcpy(dst + i * esz, src + match_idx[i] * esz, esz);
            }
        }
    } else if (pool) {
        /* Batched fused multi-column gather (≤MGATHER_MAX_COLS per dispatch) */
        multi_gather_ctx_t mgctx = { .idx = match_idx, .ncols = 0 };
        for (int64_t c = 0; c < ncols; c++) {
            if (!new_cols[c]) continue;
            ray_t* col = ray_table_get_col_idx(input, c);
            if (col && col->type == RAY_MAPCOMMON) continue; /* already materialized */
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
            ray_t* col = ray_table_get_col_idx(input, c);
            if (!col || !new_cols[c]) continue;
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
        ray_t* col = ray_table_get_col_idx(input, c);
        if (!col) continue;
        if (RAY_IS_PARTED(col->type)) {
            int8_t pb = (int8_t)RAY_PARTED_BASETYPE(col->type);
            if (pb != RAY_STR) {
                ray_t** sp = (ray_t**)ray_data(col);
                col_propagate_str_pool_parted(new_cols[c], sp, col->len);
            }
        } else {
            col_propagate_str_pool(new_cols[c], col);
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
ray_t* exec_filter_head(ray_t* input, ray_t* pred, int64_t limit) {
    if (!input || RAY_IS_ERR(input)) return input;
    if (!pred || RAY_IS_ERR(pred)) return pred;
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
    if (limit <= 0 || ncols <= 0) return ray_table_new(0);
    if (limit > nrows) limit = nrows;

    /* VLA guard */
    if (ncols > 256) return ray_error("limit", "table exceeds 256 columns");

    /* Collect up to `limit` matching row indices, stopping early */
    ray_t* idx_hdr = NULL;
    int64_t* match_idx = (int64_t*)scratch_alloc(&idx_hdr,
                                    (size_t)limit * sizeof(int64_t));
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

    /* Build result table with gathered rows */
    ray_t* tbl = ray_table_new(ncols);
    if (!tbl || RAY_IS_ERR(tbl)) { scratch_free(idx_hdr); return tbl; }

    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(input, c);
        int64_t name_id = ray_table_col_name(input, c);
        if (!col) continue;
        int8_t out_type = RAY_IS_PARTED(col->type)
                        ? (int8_t)RAY_PARTED_BASETYPE(col->type) : col->type;
        if (out_type == RAY_MAPCOMMON) continue;
        uint8_t out_attrs = 0;
        if (out_type == RAY_SYM) {
            if (RAY_IS_PARTED(col->type)) {
                ray_t** sp = (ray_t**)ray_data(col);
                out_attrs = parted_sym_max_attrs(sp, col->len);
            } else out_attrs = col->attrs;
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
                new_col = parted_gather_str_rows(segs, n_segs, match_idx, found);
            } else {
                /* Non-STR parted gather */
                int64_t seg_start = 0;
                int64_t cur_seg = 0;
                int64_t cur_seg_end = (n_segs > 0 && segs[0]) ? segs[0]->len : 0;
                for (int64_t j = 0; j < found; j++) {
                    int64_t r = match_idx[j];
                    while (cur_seg < n_segs - 1 && r >= cur_seg_end) {
                        seg_start = cur_seg_end;
                        cur_seg++;
                        cur_seg_end += segs[cur_seg] ? segs[cur_seg]->len : 0;
                    }
                    if (!segs[cur_seg])
                        continue;  /* NULL segment — zero-fill from memset */
                    parted_copy_cells(dst, out_type, out_attrs, j,
                                      segs[cur_seg], r - seg_start, 1);
                }
            }
        } else {
            char* src = (char*)ray_data(col);
            for (int64_t j = 0; j < found; j++)
                memcpy(dst + j * esz, src + match_idx[j] * esz, esz);
            col_propagate_str_pool(new_col, col);
        }
        /* SYM gather copied cell ids verbatim — resolve over the source
         * column's domain (flat → col itself; parted → first SYM seg). */
        if (new_col && new_col->type == RAY_SYM)
            ray_sym_vec_adopt_domain(new_col, sym_domain_rep(col));
        tbl = ray_table_add_col(tbl, name_id, new_col);
        ray_release(new_col);
    }

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

/* keep-set membership: is column name-sym `nm` present in keep_syms? */
static inline bool sel_compact_keep(int64_t nm, const int64_t* keep_syms, int keep_n) {
    for (int j = 0; j < keep_n; j++)
        if (keep_syms[j] == nm) return true;
    return false;
}

ray_t* sel_compact(ray_graph_t* g, ray_t* tbl, ray_t* sel,
                   const int64_t* keep_syms, int keep_n) {
    (void)g;
    if (!tbl || RAY_IS_ERR(tbl) || !sel) return tbl;

    /* keep-set: when provided, materialize only columns whose name-sym is in
     * keep_syms (order preserved); NULL/0 => materialize all columns. */
    bool use_keep = (keep_syms != NULL && keep_n > 0);

    int64_t nrows = ray_table_nrows(tbl);
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
        if (!empty || RAY_IS_ERR(empty)) return empty;
        for (int64_t c = 0; c < ncols; c++) {
            if (use_keep &&
                !sel_compact_keep(ray_table_col_name(tbl, c), keep_syms, keep_n))
                continue;
            ray_t* col = ray_table_get_col_idx(tbl, c);
            if (!col) continue;
            int8_t ct = RAY_IS_PARTED(col->type)
                      ? (int8_t)RAY_PARTED_BASETYPE(col->type) : col->type;
            /* RAY_LIST == 0; ray_vec_new rejects type <= 0, so a LIST
             * column needs ray_list_new — otherwise the empty result
             * silently drops it and the 0-row schema loses a column. */
            ray_t* nc = (ct == RAY_LIST) ? ray_list_new(0) : ray_vec_new(ct, 0);
            if (nc && !RAY_IS_ERR(nc)) {
                nc->len = 0;
                empty = ray_table_add_col(empty, ray_table_col_name(tbl, c), nc);
                ray_release(nc);
            }
        }
        return empty;
    }

    int64_t ncols = ray_table_ncols(tbl);
    if (ncols <= 0) { ray_retain(tbl); return tbl; }

    /* Build match_idx from bitmap.  Per-seg pass counts are derivable
     * from the rowsel itself (ALL→seg_len, MIX→offsets delta, NONE→0),
     * so this parallelizes as: tiny serial prefix over segs → parallel
     * per-seg fill at disjoint offsets. */
    ray_t* idx_hdr = NULL;
    int64_t* match_idx = (int64_t*)scratch_alloc(&idx_hdr,
                                       (size_t)pass_count * sizeof(int64_t));
    if (!match_idx) { ray_retain(tbl); return tbl; }

    {
        const uint8_t*  flags   = ray_rowsel_flags(sel);
        const uint32_t* offsets = ray_rowsel_offsets(sel);
        const uint16_t* idx     = ray_rowsel_idx(sel);
        uint32_t n_segs = meta->n_segs;
        ray_pool_t* sc_pool = ray_pool_get();
        ray_t* soff_hdr = NULL;
        int64_t* seg_out = ray_pool_par_dispatch_ok(sc_pool, nrows, RAY_PARALLEL_THRESHOLD)
            ? (int64_t*)scratch_alloc(&soff_hdr, (size_t)n_segs * sizeof(int64_t))
            : NULL;
        if (seg_out) {
            int64_t cum = 0;
            for (uint32_t seg = 0; seg < n_segs; seg++) {
                seg_out[seg] = cum;
                uint8_t f = flags[seg];
                if (f == RAY_SEL_NONE) continue;
                int64_t seg_start = (int64_t)seg * RAY_MORSEL_ELEMS;
                int64_t seg_end = seg_start + RAY_MORSEL_ELEMS;
                if (seg_end > nrows) seg_end = nrows;
                cum += (f == RAY_SEL_ALL) ? (seg_end - seg_start)
                                          : (int64_t)(offsets[seg + 1] - offsets[seg]);
            }
            /* One task per seg-chunk (dispatch_n; ray_pool_dispatch would
             * morselize the seg RANGE by 8192 segs → 1 task under 8.4M
             * rows).  Chunk count capped at 1024 = the pool's initial ring
             * capacity, so the ring never needs to grow (see exec_filter). */
            uint32_t spc = 64;
            while ((n_segs + spc - 1) / spc > RAY_POOL_INIT_TASKS) spc *= 2;
            uint32_t sc_chunks = (n_segs + spc - 1) / spc;
            scidx_ctx_t sc = {
                .flags = flags, .offsets = offsets, .idx = idx,
                .seg_out = seg_out, .match_idx = match_idx, .nrows = nrows,
                .n_segs = n_segs, .segs_per_chunk = spc,
            };
            ray_pool_dispatch_n(sc_pool, scidx_fill_fn, &sc, sc_chunks);
            scratch_free(soff_hdr);
        } else {
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
                    const uint16_t* slice = idx + offsets[seg];
                    uint32_t n = offsets[seg + 1] - offsets[seg];
                    for (uint32_t i = 0; i < n; i++)
                        match_idx[j++] = seg_start + slice[i];
                }
            }
        }
    }

    /* Parallel multi-column gather (same pattern as exec_filter) */
    ray_pool_t* pool = ray_pool_get();
    ray_t* out = ray_table_new(ncols);
    if (!out || RAY_IS_ERR(out)) { scratch_free(idx_hdr); return out; }

    /* VLA guard: 256 cols max for stack arrays */
    if (ncols > 256) { scratch_free(idx_hdr); return ray_error("limit", "table exceeds 256 columns"); }

    ray_t* new_cols[ncols];
    int64_t col_names[ncols];
    bool has_parted = false;

    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        col_names[c] = ray_table_col_name(tbl, c);
        if (use_keep && !sel_compact_keep(col_names[c], keep_syms, keep_n)) {
            new_cols[c] = NULL; continue;
        }
        if (!col || RAY_IS_ERR(col)) { new_cols[c] = NULL; continue; }
        if (col->type == RAY_MAPCOMMON) {
            ray_t** mc_ptrs = (ray_t**)ray_data(col);
            ray_t* kv = mc_ptrs[0];
            ray_t* rc = mc_ptrs[1];
            if (!kv || !rc || col->len < 2) { new_cols[c] = NULL; continue; }
            int64_t n_parts = kv->len;
            int8_t kv_type = kv->type;
            size_t esz = (size_t)ray_sym_elem_size(kv_type, kv->attrs);
            const char* kdata = (const char*)ray_data(kv);
            const int64_t* counts = (const int64_t*)ray_data(rc);
            ray_t* flat = ray_vec_new(kv_type, pass_count);
            if (!flat || RAY_IS_ERR(flat)) { new_cols[c] = NULL; continue; }
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
        if (!nc || RAY_IS_ERR(nc)) { new_cols[c] = NULL; continue; }
        nc->len = pass_count;
        new_cols[c] = nc;
    }

    /* LIST columns: gather element pointers with a retain.  The byte-copy
     * gather paths below copy raw esz bytes (8-byte ray_t* pointers) without
     * retaining — that would alias source elements and double-free on
     * release — so LIST columns are filled here and skipped there. */
    for (int64_t c = 0; c < ncols; c++) {
        if (!new_cols[c]) continue;
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (!col || col->type != RAY_LIST) continue;
        ray_t** d = (ray_t**)ray_data(new_cols[c]);
        ray_t** s = (ray_t**)ray_data(col);
        for (int64_t i = 0; i < pass_count; i++) {
            ray_t* e = s[match_idx[i]];
            d[i] = e;
            if (e) ray_retain(e);
        }
    }

    if (has_parted) {
        for (int64_t c = 0; c < ncols; c++) {
            ray_t* col = ray_table_get_col_idx(tbl, c);
            if (!col || !new_cols[c]) continue;
            if (col->type == RAY_LIST) continue;  /* gathered above with retain */
            if (RAY_IS_PARTED(col->type)) {
                int8_t pbase = (int8_t)RAY_PARTED_BASETYPE(col->type);
                if (pbase == RAY_STR) {
                    ray_t** psegs = (ray_t**)ray_data(col);
                    ray_release(new_cols[c]);
                    new_cols[c] = parted_gather_str_rows(psegs, col->len,
                                                         match_idx, pass_count);
                } else {
                    parted_gather_col(col, match_idx, pass_count, new_cols[c]);
                }
            } else {
                uint8_t esz = col_esz(col);
                char* src = (char*)ray_data(col);
                char* dst = (char*)ray_data(new_cols[c]);
                for (int64_t i = 0; i < pass_count; i++)
                    memcpy(dst + i * esz, src + match_idx[i] * esz, esz);
            }
        }
    } else if (pool) {
        /* Batched multi-column gather: fill mgctx with up to MGATHER_MAX_COLS
         * kept columns, dispatch, then continue with the next batch.  This is
         * column-count-agnostic — no per-column fallback. */
        multi_gather_ctx_t mgctx = { .idx = match_idx, .ncols = 0 };
        for (int64_t c = 0; c < ncols; c++) {
            if (!new_cols[c]) continue;
            ray_t* col = ray_table_get_col_idx(tbl, c);
            if (col->type == RAY_LIST) continue;  /* gathered above with retain */
            /* RAY_MAPCOMMON is not skipped here: the pre-alloc loop above already
             * materialized it to a flat new_cols[c], so col_esz returns 0 and the
             * gather is a no-op.  exec_filter adds an explicit continue for clarity;
             * sel_compact relies on the 0-byte no-op instead. */
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
        for (int64_t c = 0; c < ncols; c++) {
            ray_t* col = ray_table_get_col_idx(tbl, c);
            if (!col || !new_cols[c]) continue;
            if (col->type == RAY_LIST) continue;  /* gathered above with retain */
            uint8_t esz = col_esz(col);
            char* src = (char*)ray_data(col);
            char* dst = (char*)ray_data(new_cols[c]);
            for (int64_t i = 0; i < pass_count; i++)
                memcpy(dst + i * esz, src + match_idx[i] * esz, esz);
        }
    }

    for (int64_t c = 0; c < ncols; c++) {
        if (!new_cols[c]) continue;
        ray_t* scol = ray_table_get_col_idx(tbl, c);
        if (scol && scol->type == RAY_LIST) {
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
            col_propagate_nulls_gather(new_cols[c], scol, match_idx, pass_count);
        }
        /* SYM gathers above copied cell ids verbatim — resolve over the
         * source column's domain (flat → scol; parted → first SYM seg;
         * MAPCOMMON → keys vec, all via sym_domain_rep). */
        if (new_cols[c]->type == RAY_SYM)
            ray_sym_vec_adopt_domain(new_cols[c], sym_domain_rep(scol));
        out = ray_table_add_col(out, col_names[c], new_cols[c]);
        ray_release(new_cols[c]);
    }

    scratch_free(idx_hdr);
    return out;
}
