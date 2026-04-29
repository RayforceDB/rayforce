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
 * Rerank executors: combine a filtered source table with a top-K
 * nearest-neighbour step (index-backed ANN or brute-force KNN) in
 * one DAG op.  Used by `select ... where <p> nearest (ann|knn ...) take k`.
 */

#include "ops/internal.h"
#include "ops/rowsel.h"
#include "mem/sys.h"
#include "store/hnsw.h"
#include <math.h>
#include <string.h>

/* ==========================================================================
 *  Helpers
 * ========================================================================== */

/* Element access into a numeric ray_t vector (F32 / F64 / I32 / I64) → double. */
static double rr_at_f64(ray_t* v, int64_t i) {
    void* d = ray_data(v);
    switch (v->type) {
        case RAY_F32: return (double)((float*)d)[i];
        case RAY_F64: return ((double*)d)[i];
        case RAY_I32: return (double)((int32_t*)d)[i];
        case RAY_I64: return (double)((int64_t*)d)[i];
        default:      return 0.0;
    }
}

static bool rr_is_numeric(ray_t* v) {
    if (!v || !ray_is_vec(v)) return false;
    return v->type == RAY_F32 || v->type == RAY_F64
        || v->type == RAY_I32 || v->type == RAY_I64;
}

/* Distance metrics — mirror row_score in src/ops/embedding.c. */
typedef enum { RR_COS_DIST, RR_IP_NEG, RR_L2_DIST } rr_metric_t;

static rr_metric_t rr_metric_from_hnsw(int32_t m) {
    switch ((ray_hnsw_metric_t)m) {
        case RAY_HNSW_L2: return RR_L2_DIST;
        case RAY_HNSW_IP: return RR_IP_NEG;
        case RAY_HNSW_COSINE:
        default:          return RR_COS_DIST;
    }
}

static double rr_row_dist(rr_metric_t m, ray_t* row,
                           const double* q, double q_norm, int32_t dim) {
    double acc = 0.0, r_norm_sq = 0.0;
    if (m == RR_L2_DIST) {
        for (int32_t j = 0; j < dim; j++) {
            double d = rr_at_f64(row, j) - q[j];
            acc += d * d;
        }
        return sqrt(acc);
    }
    for (int32_t j = 0; j < dim; j++) {
        double a = rr_at_f64(row, j);
        acc += a * q[j];
        if (m == RR_COS_DIST) r_norm_sq += a * a;
    }
    if (m == RR_IP_NEG) return -acc;
    double denom = q_norm * sqrt(r_norm_sq);
    double sim = (denom > 0.0) ? acc / denom : 0.0;
    return 1.0 - sim;
}

/* Build an empty-rows clone of the source schema plus a trailing _dist
 * column (F64, len=0).  Used for both the "source is empty" and "filter
 * rejected everything" cases so callers always get a stable 4-column
 * table shape. */
static ray_t* empty_result_with_dist(ray_t* src) {
    int64_t ncols = ray_table_ncols(src);
    ray_t* out = ray_table_new(ncols + 1);
    if (!out || RAY_IS_ERR(out)) return NULL;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* sc = ray_table_get_col_idx(src, c);
        if (!sc) continue;
        int8_t ct = RAY_IS_PARTED(sc->type)
                  ? (int8_t)RAY_PARTED_BASETYPE(sc->type) : sc->type;
        ray_t* nc = (ct == RAY_LIST) ? ray_list_new(0) : ray_vec_new(ct, 0);
        if (!nc || RAY_IS_ERR(nc)) { ray_release(out); return NULL; }
        nc->len = 0;
        out = ray_table_add_col(out, ray_table_col_name(src, c), nc);
        ray_release(nc);
        if (RAY_IS_ERR(out)) return NULL;
    }
    ray_t* dv = ray_vec_new(RAY_F64, 0);
    if (!dv || RAY_IS_ERR(dv)) { ray_release(out); return NULL; }
    dv->len = 0;
    out = ray_table_add_col(out, sym_intern_safe("_dist", 5), dv);
    ray_release(dv);
    return out;
}

/* Gather k rows from `tbl` at dense `rowids[]`, appending a `_dist` F64
 * column with the parallel distances.  Caller owns the returned table.
 * Returns NULL on OOM. */
static ray_t* gather_rows_with_dist(ray_t* tbl,
                                     const int64_t* rowids, const double* dists,
                                     int64_t k) {
    int64_t ncols = ray_table_ncols(tbl);
    ray_t* result = ray_table_new(ncols + 1);
    if (!result || RAY_IS_ERR(result)) return NULL;

    for (int64_t c = 0; c < ncols; c++) {
        ray_t* src_col = ray_table_get_col_idx(tbl, c);
        if (!src_col) { ray_release(result); return NULL; }

        /* PARTED columns carry ray_t** segment pointers in their data
         * region, not raw element bytes — the byte-wise gather below
         * would read pointer values as column data.  Reject with a clear
         * error rather than produce garbage; PARTED support is future work. */
        if (RAY_IS_PARTED(src_col->type)) {
            ray_release(result);
            return ray_error("nyi",
                "nearest: PARTED columns not supported in result projection");
        }

        int8_t ct = src_col->type;

        /* Allocate the destination column with the right shape.  col_vec_new
         * handles SYM width preservation; LIST uses its own constructor. */
        ray_t* new_col = (ct == RAY_LIST) ? ray_list_new(k) : col_vec_new(src_col, k);
        if (!new_col || RAY_IS_ERR(new_col)) { ray_release(result); return NULL; }
        new_col->len = k;

        if (ct == RAY_LIST) {
            ray_t** d = (ray_t**)ray_data(new_col);
            ray_t** s = (ray_t**)ray_data(src_col);
            for (int64_t i = 0; i < k; i++) {
                d[i] = s[rowids[i]];
                if (d[i]) ray_retain(d[i]);
            }
        } else {
            /* All fixed-width types (including SYM at any width, RAY_STR's
             * 16-byte inline cells, DATE/TIME/TIMESTAMP, GUID) go through
             * byte-wise memcpy driven by the column's element size.
             * Mirrors sel_compact's gather convention. */
            uint8_t esz = col_esz(src_col);
            char* dst = (char*)ray_data(new_col);
            const char* src = (const char*)ray_data(src_col);
            for (int64_t i = 0; i < k; i++)
                memcpy(dst + i * esz, src + rowids[i] * esz, esz);

            /* RAY_STR: share the source pool (inline bytes reference
             * pooled long-string data). */
            if (ct == RAY_STR) col_propagate_str_pool(new_col, src_col);

            /* RAY_SYM: propagate the per-vector sym_dict so narrow-width
             * local indices resolve against the same dictionary.  For
             * sliced SYM columns the sym_dict lives on the slice_parent
             * (the slice's own union slot holds slice_parent/offset).
             * Guards against the inline-nullmap aliasing mirror sort.c:3307. */
            if (ct == RAY_SYM) {
                const ray_t* dict_owner = (src_col->attrs & RAY_ATTR_SLICE)
                                        ? src_col->slice_parent : src_col;
                if (dict_owner &&
                    (!(dict_owner->attrs & RAY_ATTR_HAS_NULLS) ||
                     (dict_owner->attrs & RAY_ATTR_NULLMAP_EXT)) &&
                    dict_owner->sym_dict) {
                    ray_retain(dict_owner->sym_dict);
                    new_col->sym_dict = dict_owner->sym_dict;
                }
            }

            /* Null bitmap: the shared col_propagate_nulls_gather only
             * inspects src's own attrs — for a sliced src it misses
             * HAS_NULLS on the parent.  Mirror sort.c:3315's slice-aware
             * check so sliced source columns don't lose their nulls. */
            bool src_has_nulls =
                (src_col->attrs & RAY_ATTR_HAS_NULLS) ||
                ((src_col->attrs & RAY_ATTR_SLICE) && src_col->slice_parent &&
                 (src_col->slice_parent->attrs & RAY_ATTR_HAS_NULLS));
            if (src_has_nulls) {
                for (int64_t r = 0; r < k; r++) {
                    if (ray_vec_is_null(src_col, rowids[r]))
                        ray_vec_set_null(new_col, r, true);
                }
            }
        }

        ray_t* prev = result;
        result = ray_table_add_col(result, ray_table_col_name(tbl, c), new_col);
        ray_release(new_col);
        if (!result || RAY_IS_ERR(result)) {
            /* ray_table_add_col's error paths don't release the input
             * table when they fail mid-way (cow may have returned the
             * same pointer).  Release our prior accumulator to avoid
             * leaking the partially-built table and its retained cols. */
            if (prev && !RAY_IS_ERR(prev) && prev != result) ray_release(prev);
            return NULL;
        }
    }

    /* Append _dist column */
    ray_t* dist_vec = ray_vec_new(RAY_F64, k);
    if (!dist_vec || RAY_IS_ERR(dist_vec)) { ray_release(result); return NULL; }
    dist_vec->len = k;
    double* dd = (double*)ray_data(dist_vec);
    for (int64_t i = 0; i < k; i++) dd[i] = dists[i];
    ray_t* prev = result;
    result = ray_table_add_col(result, sym_intern_safe("_dist", 5), dist_vec);
    ray_release(dist_vec);
    if (!result || RAY_IS_ERR(result)) {
        if (prev && !RAY_IS_ERR(prev) && prev != result) ray_release(prev);
        return NULL;
    }
    return result;
}

/* Extract the accepted rowid set from a possibly-lazy source table.
 *
 * Returns:
 *   - NULL pointer AND *count = nrows     — no filter: identity scan, all rows accepted.
 *   - NULL pointer AND *count = 0         — filter rejected every row.
 *   - NULL pointer AND *count = -1        — ALLOCATION FAILURE: caller must propagate OOM.
 *   - non-NULL pointer AND *count > 0     — explicit rowid list to walk.
 *
 * `g->selection` is always cleared before returning when this helper has
 * observed it — success or failure — so downstream ops don't double-filter. */
static int64_t* accepted_rowids(ray_graph_t* g, int64_t nrows, int64_t* count) {
    if (!g->selection) { *count = nrows; return NULL; }

    int64_t n_accepted = ray_rowsel_meta(g->selection)->total_pass;

    /* Consume the selection up front so all exit paths leave g->selection
     * clean regardless of downstream allocation outcomes. */
    ray_t* sel = g->selection;
    g->selection = NULL;

    if (n_accepted == 0) {
        ray_release(sel);
        *count = 0;
        return NULL;
    }

    ray_t* idx_blk = ray_rowsel_to_indices(sel);
    if (!idx_blk) {
        ray_release(sel);
        *count = -1;  /* OOM */
        return NULL;
    }

    int64_t* dense = (int64_t*)ray_sys_alloc((size_t)n_accepted * sizeof(int64_t));
    if (!dense) {
        ray_release(idx_blk);
        ray_release(sel);
        *count = -1;  /* OOM */
        return NULL;
    }
    memcpy(dense, ray_data(idx_blk), (size_t)n_accepted * sizeof(int64_t));
    ray_release(idx_blk);
    ray_release(sel);
    *count = n_accepted;
    return dense;
}

/* Max-heap top-K by distance (lower=closer).  Mirrors the heap in
 * src/ops/embedding.c:ray_knn_fn. */
typedef struct { double d; int64_t id; } rr_ent_t;

static void rr_heap_insert(rr_ent_t* heap, int64_t k, int64_t* size,
                            double d, int64_t id) {
    if (*size < k) {
        int64_t j = (*size)++;
        heap[j] = (rr_ent_t){ d, id };
        while (j > 0) {
            int64_t p = (j - 1) / 2;
            if (heap[p].d >= heap[j].d) break;
            rr_ent_t t = heap[p]; heap[p] = heap[j]; heap[j] = t;
            j = p;
        }
    } else if (d < heap[0].d) {
        heap[0] = (rr_ent_t){ d, id };
        int64_t j = 0;
        for (;;) {
            int64_t l = 2*j+1, r = 2*j+2, best = j;
            if (l < *size && heap[l].d > heap[best].d) best = l;
            if (r < *size && heap[r].d > heap[best].d) best = r;
            if (best == j) break;
            rr_ent_t t = heap[j]; heap[j] = heap[best]; heap[best] = t;
            j = best;
        }
    }
}

static void rr_heap_sort(rr_ent_t* heap, int64_t size) {
    /* Insertion sort ascending by distance — size is small. */
    for (int64_t i = 1; i < size; i++) {
        rr_ent_t key = heap[i];
        int64_t j = i - 1;
        while (j >= 0 && heap[j].d > key.d) {
            heap[j + 1] = heap[j];
            j--;
        }
        heap[j + 1] = key;
    }
}

/* ==========================================================================
 *  exec_ann_rerank — index-backed, filter-aware iterative scan.
 *
 *  Pushes the filter's accepted-rowid bitmap into HNSW's beam search as
 *  a predicate callback (`ray_hnsw_search_filter`).  Rejected nodes are
 *  still traversed for graph connectivity; only accepted nodes enter the
 *  result heap.  This replaces the prior oversample+refilter loop which
 *  degraded to near-full-scan for highly selective filters with no
 *  recall guarantee.
 * ========================================================================== */

/* Predicate context — membership bitmap over the index's row space. */
typedef struct {
    const uint8_t* member;
    int64_t        n_nodes;
} rr_member_ctx_t;

static bool rr_member_accept(int64_t node_id, void* ctx) {
    const rr_member_ctx_t* c = (const rr_member_ctx_t*)ctx;
    if (node_id < 0 || node_id >= c->n_nodes) return false;
    return (c->member[node_id / 8] >> (node_id % 8)) & 1;
}

ray_t* exec_ann_rerank(ray_graph_t* g, ray_op_t* op, ray_t* src) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    if (!src || src->type != RAY_TABLE) return ray_error("type", NULL);

    ray_hnsw_t* idx    = (ray_hnsw_t*)ext->rerank.hnsw_idx;
    const float* query = ext->rerank.query_vec;
    int32_t      dim   = ext->rerank.dim;
    int64_t      k     = ext->rerank.k;
    int32_t      ef    = ext->rerank.ef_search;
    if (!idx || !query || dim <= 0 || k <= 0) return ray_error("schema", NULL);
    if (dim != idx->dim) return ray_error("length", NULL);

    int64_t src_rows = ray_table_nrows(src);

    /* Special-case empty source: return a well-shaped empty result. */
    if (src_rows == 0) {
        if (g->selection) { ray_release(g->selection); g->selection = NULL; }
        ray_t* r = empty_result_with_dist(src);
        return r ? r : ray_error("oom", NULL);
    }

    int64_t accepted_count = 0;
    int64_t* accepted = accepted_rowids(g, src_rows, &accepted_count);
    if (accepted_count < 0) return ray_error("oom", NULL);
    if (accepted_count == 0) {
        ray_t* r = empty_result_with_dist(src);
        return r ? r : ray_error("oom", NULL);
    }

    int64_t n_nodes = idx->n_nodes;
    int32_t ef_search = ef;
    if ((int64_t)ef_search < k) ef_search = (int32_t)k;

    int64_t* out_ids = (int64_t*)ray_sys_alloc((size_t)k * sizeof(int64_t));
    double*  out_ds  = (double*)ray_sys_alloc((size_t)k * sizeof(double));
    if (!out_ids || !out_ds) {
        if (out_ids) ray_sys_free(out_ids);
        if (out_ds)  ray_sys_free(out_ds);
        if (accepted) ray_sys_free(accepted);
        return ray_error("oom", NULL);
    }

    int64_t n_found;
    if (!accepted) {
        /* No filter — plain search with no per-candidate callback. */
        n_found = ray_hnsw_search(idx, query, dim, k, ef_search, out_ids, out_ds);
    } else {
        /* Build membership bitmap over the index's row space and hand it
         * to the filtered iterative scan as a predicate callback. */
        size_t bm_size = ((size_t)n_nodes + 7) / 8;
        uint8_t* member = (uint8_t*)ray_sys_alloc(bm_size);
        if (!member) {
            ray_sys_free(out_ids); ray_sys_free(out_ds); ray_sys_free(accepted);
            return ray_error("oom", NULL);
        }
        memset(member, 0, bm_size);
        for (int64_t i = 0; i < accepted_count; i++) {
            int64_t rid = accepted[i];
            if (rid >= 0 && rid < n_nodes) member[rid / 8] |= (uint8_t)(1u << (rid % 8));
        }
        ray_sys_free(accepted);
        accepted = NULL;

        rr_member_ctx_t cb_ctx = { .member = member, .n_nodes = n_nodes };
        n_found = ray_hnsw_search_filter(idx, query, dim, k, ef_search,
                                          rr_member_accept, &cb_ctx,
                                          out_ids, out_ds);
        ray_sys_free(member);
    }
    if (accepted) ray_sys_free(accepted);

    /* ray_hnsw_search / _filter return -1 on internal OOM — surface it as
     * an error rather than silently returning a zero-row table. */
    if (n_found < 0) {
        ray_sys_free(out_ids);
        ray_sys_free(out_ds);
        return ray_error("oom", NULL);
    }

    ray_t* result = gather_rows_with_dist(src, out_ids, out_ds, n_found);
    ray_sys_free(out_ids);
    ray_sys_free(out_ds);
    if (!result) return ray_error("oom", NULL);
    return result;
}

/* ==========================================================================
 *  exec_knn_rerank — brute force over a filtered column
 * ========================================================================== */

ray_t* exec_knn_rerank(ray_graph_t* g, ray_op_t* op, ray_t* src) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    if (!src || src->type != RAY_TABLE) return ray_error("type", NULL);

    int64_t      col_sym = ext->rerank.col_sym;
    const float* query   = ext->rerank.query_vec;
    int32_t      dim     = ext->rerank.dim;
    int64_t      k       = ext->rerank.k;
    rr_metric_t  metric  = rr_metric_from_hnsw(ext->rerank.metric);
    if (col_sym <= 0 || !query || dim <= 0 || k <= 0) return ray_error("schema", NULL);

    /* Special-case empty source: return a well-shaped empty result rather
     * than falling into the top-K code with k_eff=0. */
    int64_t src_rows = ray_table_nrows(src);
    if (src_rows == 0) {
        /* Consume any dangling selection to keep downstream ops clean. */
        if (g->selection) { ray_release(g->selection); g->selection = NULL; }
        ray_t* r = empty_result_with_dist(src);
        return r ? r : ray_error("oom", NULL);
    }

    /* We walk the ORIGINAL source table and skip non-accepted rows via
     * an accepted-rowid list.  Avoids sel_compact, which currently
     * doesn't correctly materialise RAY_LIST columns. */
    ray_t* col = ray_table_get_col(src, col_sym);
    if (!col) return ray_error("name", NULL);
    if (col->type != RAY_LIST) return ray_error("type", NULL);

    int64_t nrows = col->len;

    int64_t  accepted_count = 0;
    int64_t* accepted = accepted_rowids(g, nrows, &accepted_count);
    if (accepted_count < 0) return ray_error("oom", NULL);
    if (accepted_count == 0) {
        ray_t* r = empty_result_with_dist(src);
        return r ? r : ray_error("oom", NULL);
    }

    /* Convert query float* → double[] + norm. */
    double* q_buf = (double*)ray_sys_alloc((size_t)dim * sizeof(double));
    if (!q_buf) { if (accepted) ray_sys_free(accepted); return ray_error("oom", NULL); }
    double q_norm_sq = 0.0;
    for (int32_t j = 0; j < dim; j++) {
        q_buf[j] = (double)query[j];
        q_norm_sq += q_buf[j] * q_buf[j];
    }
    double q_norm = sqrt(q_norm_sq);

    int64_t k_eff = k;
    if (k_eff > accepted_count) k_eff = accepted_count;

    rr_ent_t* heap = (rr_ent_t*)ray_sys_alloc((size_t)k_eff * sizeof(rr_ent_t));
    if (!heap) {
        ray_sys_free(q_buf); if (accepted) ray_sys_free(accepted);
        return ray_error("oom", NULL);
    }
    int64_t heap_size = 0;

    /* Walk accepted rows — identity scan if no filter, dense rowid list otherwise. */
    if (accepted) {
        for (int64_t ai = 0; ai < accepted_count; ai++) {
            int64_t i = accepted[ai];
            if (i < 0 || i >= nrows) continue;
            ray_t* row = ray_list_get(col, i);
            if (!rr_is_numeric(row) || row->len != dim) {
                ray_sys_free(heap); ray_sys_free(q_buf); ray_sys_free(accepted);
                return ray_error("type", NULL);
            }
            double d = rr_row_dist(metric, row, q_buf, q_norm, dim);
            rr_heap_insert(heap, k_eff, &heap_size, d, i);
        }
    } else {
        for (int64_t i = 0; i < nrows; i++) {
            ray_t* row = ray_list_get(col, i);
            if (!rr_is_numeric(row) || row->len != dim) {
                ray_sys_free(heap); ray_sys_free(q_buf);
                return ray_error("type", NULL);
            }
            double d = rr_row_dist(metric, row, q_buf, q_norm, dim);
            rr_heap_insert(heap, k_eff, &heap_size, d, i);
        }
    }
    ray_sys_free(q_buf);
    if (accepted) ray_sys_free(accepted);

    rr_heap_sort(heap, heap_size);

    int64_t* out_ids = (int64_t*)ray_sys_alloc((size_t)heap_size * sizeof(int64_t));
    double*  out_ds  = (double*)ray_sys_alloc((size_t)heap_size * sizeof(double));
    if ((!out_ids || !out_ds) && heap_size > 0) {
        if (out_ids) ray_sys_free(out_ids);
        if (out_ds)  ray_sys_free(out_ds);
        ray_sys_free(heap);
        return ray_error("oom", NULL);
    }
    for (int64_t i = 0; i < heap_size; i++) {
        out_ids[i] = heap[i].id;
        out_ds[i]  = heap[i].d;
    }
    ray_sys_free(heap);

    ray_t* result = gather_rows_with_dist(src, out_ids, out_ds, heap_size);
    if (out_ids) ray_sys_free(out_ids);
    if (out_ds)  ray_sys_free(out_ds);
    if (!result) return ray_error("oom", NULL);
    return result;
}
