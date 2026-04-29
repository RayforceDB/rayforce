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
#include "lang/internal.h"
#include "mem/sys.h"

/* --------------------------------------------------------------------------
 * exec_cosine_sim: cosine similarity between embedding column and query vector.
 * dot(a,b) / (||a|| * ||b||) per row.
 * Input: RAY_F32 embedding column (flat N*D floats)
 * Output: RAY_F64 vector of similarities (one per row)
 * -------------------------------------------------------------------------- */
ray_t* exec_cosine_sim(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    const float* query = ext->vector.query_vec;
    int32_t dim = ext->vector.dim;

    if (!query || dim <= 0) return ray_error("schema", NULL);
    if (emb_vec->type != RAY_F32) return ray_error("type", NULL);

    int64_t total = emb_vec->len;
    int64_t nrows = total / dim;
    if (nrows * dim != total) return ray_error("length", NULL);

    const float* data = (const float*)ray_data(emb_vec);

    /* Precompute query norm */
    double q_norm_sq = 0.0;
    for (int32_t j = 0; j < dim; j++) {
        q_norm_sq += (double)query[j] * (double)query[j];
    }
    double q_norm = sqrt(q_norm_sq);

    /* Compute per-row similarity */
    ray_t* result = ray_vec_new(RAY_F64, nrows);
    if (!result || RAY_IS_ERR(result)) return ray_error("oom", NULL);
    result->len = nrows;
    double* out = (double*)ray_data(result);

    for (int64_t i = 0; i < nrows; i++) {
        const float* row = data + i * dim;
        double dot = 0.0;
        double r_norm_sq = 0.0;
        for (int32_t j = 0; j < dim; j++) {
            dot += (double)row[j] * (double)query[j];
            r_norm_sq += (double)row[j] * (double)row[j];
        }
        double r_norm = sqrt(r_norm_sq);
        double denom = q_norm * r_norm;
        out[i] = (denom > 0.0) ? dot / denom : 0.0;
    }

    return result;
}

/* --------------------------------------------------------------------------
 * exec_euclidean_dist: euclidean distance between embedding column and query.
 * sqrt(sum((a_i - b_i)^2)) per row.
 * -------------------------------------------------------------------------- */
ray_t* exec_euclidean_dist(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    const float* query = ext->vector.query_vec;
    int32_t dim = ext->vector.dim;

    if (!query || dim <= 0) return ray_error("schema", NULL);
    if (emb_vec->type != RAY_F32) return ray_error("type", NULL);

    int64_t total = emb_vec->len;
    int64_t nrows = total / dim;
    if (nrows * dim != total) return ray_error("length", NULL);

    const float* data = (const float*)ray_data(emb_vec);

    ray_t* result = ray_vec_new(RAY_F64, nrows);
    if (!result || RAY_IS_ERR(result)) return ray_error("oom", NULL);
    result->len = nrows;
    double* out = (double*)ray_data(result);

    for (int64_t i = 0; i < nrows; i++) {
        const float* row = data + i * dim;
        double sum_sq = 0.0;
        for (int32_t j = 0; j < dim; j++) {
            double d = (double)row[j] - (double)query[j];
            sum_sq += d * d;
        }
        out[i] = sqrt(sum_sq);
    }

    return result;
}

/* --------------------------------------------------------------------------
 * exec_knn: brute-force top-K nearest neighbors over a flat RAY_F32 column.
 *
 * Dispatches on ext->vector.metric (default COSINE — 0-initialized struct).
 * Returns RAY_TABLE with _rowid (I64) and _dist (F64), sorted ascending so
 * lower = closer across all metrics.
 *
 * Distance encoding:
 *   COSINE → 1 - cosine_similarity
 *   L2     → sqrt(Σ (a - b)^2)
 *   IP     → -dot(a, b)
 * -------------------------------------------------------------------------- */

/* Max-heap entry keyed on distance (root = farthest of top-K kept). */
typedef struct {
    double  dist;
    int64_t rowid;
} knn_entry_t;

static void knn_heap_insert(knn_entry_t* heap, int64_t k, int64_t* size,
                             double dist, int64_t rowid) {
    if (*size < k) {
        int64_t i = (*size)++;
        heap[i].dist = dist;
        heap[i].rowid = rowid;
        /* Sift up (max-heap: root = largest distance = worst kept) */
        while (i > 0) {
            int64_t parent = (i - 1) / 2;
            if (heap[parent].dist >= heap[i].dist) break;
            knn_entry_t tmp = heap[parent]; heap[parent] = heap[i]; heap[i] = tmp;
            i = parent;
        }
    } else if (dist < heap[0].dist) {
        heap[0].dist = dist;
        heap[0].rowid = rowid;
        int64_t i = 0;
        while (1) {
            int64_t left = 2*i+1, right = 2*i+2, best = i;
            if (left  < *size && heap[left].dist  > heap[best].dist) best = left;
            if (right < *size && heap[right].dist > heap[best].dist) best = right;
            if (best == i) break;
            knn_entry_t tmp = heap[i]; heap[i] = heap[best]; heap[best] = tmp;
            i = best;
        }
    }
}

static double knn_row_dist(int32_t metric,
                             const float* row, const float* query,
                             double q_norm, int32_t dim) {
    if (metric == RAY_HNSW_L2) {
        double s = 0.0;
        for (int32_t j = 0; j < dim; j++) {
            double d = (double)row[j] - (double)query[j];
            s += d * d;
        }
        return sqrt(s);
    }
    double dot = 0.0, r_norm_sq = 0.0;
    for (int32_t j = 0; j < dim; j++) {
        dot += (double)row[j] * (double)query[j];
        if (metric == RAY_HNSW_COSINE) r_norm_sq += (double)row[j] * (double)row[j];
    }
    if (metric == RAY_HNSW_IP) return -dot;
    /* COSINE */
    double denom = q_norm * sqrt(r_norm_sq);
    return (denom > 0.0) ? 1.0 - (dot / denom) : 1.0;
}

ray_t* exec_knn(ray_graph_t* g, ray_op_t* op, ray_t* emb_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    const float* query = ext->vector.query_vec;
    int32_t dim = ext->vector.dim;
    int64_t k = ext->vector.k;
    int32_t metric = ext->vector.metric;
    if (metric < RAY_HNSW_COSINE || metric > RAY_HNSW_IP) metric = RAY_HNSW_COSINE;

    if (!query || dim <= 0 || k <= 0) return ray_error("schema", NULL);
    if (emb_vec->type != RAY_F32) return ray_error("type", NULL);

    int64_t total = emb_vec->len;
    int64_t nrows = total / dim;
    if (nrows * dim != total) return ray_error("length", NULL);
    if (k > nrows) k = nrows;

    const float* data = (const float*)ray_data(emb_vec);

    /* Precompute query norm once (only used by cosine). */
    double q_norm = 0.0;
    if (metric == RAY_HNSW_COSINE) {
        double q_norm_sq = 0.0;
        for (int32_t j = 0; j < dim; j++)
            q_norm_sq += (double)query[j] * (double)query[j];
        q_norm = sqrt(q_norm_sq);
    }

    ray_t* heap_hdr = NULL;
    knn_entry_t* heap = (knn_entry_t*)scratch_alloc(&heap_hdr, (size_t)k * sizeof(knn_entry_t));
    if (!heap) return ray_error("oom", NULL);
    int64_t heap_size = 0;

    for (int64_t i = 0; i < nrows; i++) {
        double d = knn_row_dist(metric, data + i * dim, query, q_norm, dim);
        knn_heap_insert(heap, k, &heap_size, d, i);
    }

    /* Insertion sort ascending by distance (k is small). */
    for (int64_t i = 1; i < heap_size; i++) {
        knn_entry_t key = heap[i];
        int64_t j = i - 1;
        while (j >= 0 && heap[j].dist > key.dist) {
            heap[j + 1] = heap[j];
            j--;
        }
        heap[j + 1] = key;
    }

    ray_t* rowid_vec = ray_vec_new(RAY_I64, heap_size);
    ray_t* dist_vec  = ray_vec_new(RAY_F64, heap_size);
    if (!rowid_vec || RAY_IS_ERR(rowid_vec) || !dist_vec || RAY_IS_ERR(dist_vec)) {
        scratch_free(heap_hdr);
        if (rowid_vec && !RAY_IS_ERR(rowid_vec)) ray_release(rowid_vec);
        if (dist_vec && !RAY_IS_ERR(dist_vec))   ray_release(dist_vec);
        return ray_error("oom", NULL);
    }

    int64_t* rdata = (int64_t*)ray_data(rowid_vec);
    double*  ddata = (double*)ray_data(dist_vec);
    for (int64_t i = 0; i < heap_size; i++) {
        rdata[i] = heap[i].rowid;
        ddata[i] = heap[i].dist;
    }
    rowid_vec->len = heap_size;
    dist_vec->len  = heap_size;
    scratch_free(heap_hdr);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(rowid_vec);
        ray_release(dist_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_rowid", 6), rowid_vec);
    ray_release(rowid_vec);
    result = ray_table_add_col(result, sym_intern_safe("_dist", 5), dist_vec);
    ray_release(dist_vec);
    return result;
}

ray_t* exec_hnsw_knn(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_hnsw_t* idx = (ray_hnsw_t*)ext->hnsw.hnsw_idx;
    const float* query = ext->hnsw.query_vec;
    int32_t dim = ext->hnsw.dim;
    int64_t k = ext->hnsw.k;
    int32_t ef = ext->hnsw.ef_search;

    if (!idx || !query || dim <= 0 || k <= 0) return ray_error("schema", NULL);

    /* Pre-allocate output arrays */
    ray_t* ids_hdr = NULL;
    int64_t* out_ids = (int64_t*)scratch_alloc(&ids_hdr, (size_t)k * sizeof(int64_t));
    if (!out_ids) return ray_error("oom", NULL);

    ray_t* dists_hdr = NULL;
    double* out_dists = (double*)scratch_alloc(&dists_hdr, (size_t)k * sizeof(double));
    if (!out_dists) { scratch_free(ids_hdr); return ray_error("oom", NULL); }

    int64_t n_found = ray_hnsw_search(idx, query, dim, k, ef, out_ids, out_dists);
    if (n_found < 0) {
        scratch_free(ids_hdr);
        scratch_free(dists_hdr);
        return ray_error("oom", NULL);
    }

    /* Build output table: _rowid (I64), _dist (F64).  ray_hnsw_search writes
     * metric-native distances (lower = closer across COSINE / L2 / IP), so we
     * pass them through unchanged. */
    ray_t* rowid_vec = ray_vec_new(RAY_I64, n_found);
    ray_t* dist_vec  = ray_vec_new(RAY_F64, n_found);
    if (!rowid_vec || RAY_IS_ERR(rowid_vec) || !dist_vec || RAY_IS_ERR(dist_vec)) {
        scratch_free(ids_hdr);
        scratch_free(dists_hdr);
        if (rowid_vec && !RAY_IS_ERR(rowid_vec)) ray_release(rowid_vec);
        if (dist_vec && !RAY_IS_ERR(dist_vec))   ray_release(dist_vec);
        return ray_error("oom", NULL);
    }

    int64_t* rdata = (int64_t*)ray_data(rowid_vec);
    double*  ddata = (double*)ray_data(dist_vec);
    for (int64_t i = 0; i < n_found; i++) {
        rdata[i] = out_ids[i];
        ddata[i] = out_dists[i];
    }
    rowid_vec->len = n_found;
    dist_vec->len  = n_found;

    scratch_free(ids_hdr);
    scratch_free(dists_hdr);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(rowid_vec);
        ray_release(dist_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_rowid", 6), rowid_vec);
    ray_release(rowid_vec);
    result = ray_table_add_col(result, sym_intern_safe("_dist", 5), dist_vec);
    ray_release(dist_vec);

    return result;
}

/* ==========================================================================
 *  Rayfall builtins — direct metrics, exact KNN, HNSW lifecycle/query
 *
 *  Column shape for all builtins accepting a "column" argument:
 *    RAY_LIST whose entries are numeric vectors (RAY_F32 preferred,
 *    RAY_F64/RAY_I32/RAY_I64 coerced to double).  All entries must have
 *    the same length == D.
 *
 *  Output of knn / ann:  table {_rowid: I64, _dist: F64} sorted ascending.
 * ========================================================================== */

static bool rayvec_is_numeric(ray_t* v) {
    if (!v || !ray_is_vec(v)) return false;
    return v->type == RAY_F32 || v->type == RAY_F64
        || v->type == RAY_I32 || v->type == RAY_I64;
}

static double rayvec_at_f64(ray_t* v, int64_t i) {
    void* d = ray_data(v);
    switch (v->type) {
        case RAY_F32: return (double)((float*)d)[i];
        case RAY_F64: return ((double*)d)[i];
        case RAY_I32: return (double)((int32_t*)d)[i];
        case RAY_I64: return (double)((int64_t*)d)[i];
        default:      return 0.0;
    }
}

/* Copy a numeric vector into a float buffer.  Assumes v->len == dim. */
static void rayvec_to_floats(ray_t* v, float* dst, int32_t dim) {
    if (v->type == RAY_F32) {
        memcpy(dst, ray_data(v), (size_t)dim * sizeof(float));
        return;
    }
    for (int32_t i = 0; i < dim; i++) dst[i] = (float)rayvec_at_f64(v, i);
}

/* Validate list of numeric vectors, set *out_dim to the common length.
 * Returns 0 on success, non-zero on error. */
static int list_vec_validate(ray_t* list, int32_t* out_dim) {
    if (!list || list->type != RAY_LIST) return 1;
    if (list->len <= 0) { *out_dim = 0; return 0; }
    ray_t* first = ray_list_get(list, 0);
    if (!rayvec_is_numeric(first) || first->len <= 0) return 2;
    int32_t dim = (int32_t)first->len;
    for (int64_t i = 1; i < list->len; i++) {
        ray_t* e = ray_list_get(list, i);
        if (!rayvec_is_numeric(e) || e->len != dim) return 3;
    }
    *out_dim = dim;
    return 0;
}

/* Flatten LIST of numeric vectors into a new float[] buffer.
 * Caller frees with ray_sys_free. */
static float* list_flatten_floats(ray_t* list, int32_t dim, int64_t* out_n) {
    int64_t n = list->len;
    *out_n = n;
    if (n == 0) return NULL;
    float* buf = (float*)ray_sys_alloc((size_t)n * (size_t)dim * sizeof(float));
    if (!buf) return NULL;
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = ray_list_get(list, i);
        rayvec_to_floats(e, buf + i * dim, dim);
    }
    return buf;
}

/* Metric kinds:
 *   COS_DIST    → 1 - cos(a, b)        (lower = closer, range [0, 2])
 *   INNER_PROD  → raw dot(a, b)         (sign varies — not a distance)
 *   L2_DIST     → sqrt(Σ (a - b)^2)     (lower = closer)
 * These are the values returned by cos-dist / inner-prod / l2-dist builtins. */
typedef enum { MET_COS_DIST, MET_INNER_PROD, MET_L2_DIST } metric_kind_t;

static double row_score(metric_kind_t k, ray_t* row,
                         const double* q, double q_norm, int32_t dim) {
    double acc = 0.0, r_norm_sq = 0.0;
    if (k == MET_L2_DIST) {
        for (int32_t j = 0; j < dim; j++) {
            double d = rayvec_at_f64(row, j) - q[j];
            acc += d * d;
        }
        return sqrt(acc);
    }
    for (int32_t j = 0; j < dim; j++) {
        double a = rayvec_at_f64(row, j);
        acc += a * q[j];
        if (k == MET_COS_DIST) r_norm_sq += a * a;
    }
    if (k == MET_INNER_PROD) return acc;
    /* COS_DIST = 1 - cos_sim */
    double denom = q_norm * sqrt(r_norm_sq);
    double sim = (denom > 0.0) ? acc / denom : 0.0;
    return 1.0 - sim;
}

/* Extract query vector to a double[] scratch buffer. */
static double* query_to_doubles(ray_t* q, int32_t dim, double* q_norm_out) {
    double* buf = (double*)ray_sys_alloc((size_t)dim * sizeof(double));
    if (!buf) return NULL;
    double ns = 0.0;
    for (int32_t j = 0; j < dim; j++) {
        buf[j] = rayvec_at_f64(q, j);
        ns += buf[j] * buf[j];
    }
    *q_norm_out = sqrt(ns);
    return buf;
}

/* Binary dispatcher for cos-dist / inner-prod / l2-dist. */
static ray_t* vec_binary_metric(metric_kind_t kind, ray_t* a, ray_t* b) {
    if (!a || !b) return ray_error("type", NULL);

    /* LIST × vec → F64 vector (one score per list entry).
     * vec × LIST → same (treat the LIST as the column). */
    ray_t* list = NULL;
    ray_t* query = NULL;
    if (a->type == RAY_LIST && rayvec_is_numeric(b))      { list = a; query = b; }
    else if (b->type == RAY_LIST && rayvec_is_numeric(a)) { list = b; query = a; }

    if (list) {
        int32_t dim;
        if (list_vec_validate(list, &dim) != 0) return ray_error("type", NULL);
        if (query->len != dim) return ray_error("length", NULL);

        double q_norm;
        double* q = query_to_doubles(query, dim, &q_norm);
        if (!q) return ray_error("oom", NULL);

        int64_t n = list->len;
        ray_t* result = ray_vec_new(RAY_F64, n);
        if (!result || RAY_IS_ERR(result)) { ray_sys_free(q); return ray_error("oom", NULL); }
        result->len = n;
        double* out = (double*)ray_data(result);
        for (int64_t i = 0; i < n; i++) {
            ray_t* row = ray_list_get(list, i);
            out[i] = row_score(kind, row, q, q_norm, dim);
        }
        ray_sys_free(q);
        return result;
    }

    /* vec × vec → scalar */
    if (!rayvec_is_numeric(a) || !rayvec_is_numeric(b)) return ray_error("type", NULL);
    if (a->len != b->len || a->len <= 0) return ray_error("length", NULL);
    int32_t dim = (int32_t)a->len;

    double q_norm;
    double* q = query_to_doubles(b, dim, &q_norm);
    if (!q) return ray_error("oom", NULL);
    double v = row_score(kind, a, q, q_norm, dim);
    ray_sys_free(q);
    return make_f64(v);
}

ray_t* ray_cos_dist_fn   (ray_t* a, ray_t* b) { return vec_binary_metric(MET_COS_DIST,   a, b); }
ray_t* ray_inner_prod_fn (ray_t* a, ray_t* b) { return vec_binary_metric(MET_INNER_PROD, a, b); }
ray_t* ray_l2_dist_fn    (ray_t* a, ray_t* b) { return vec_binary_metric(MET_L2_DIST,    a, b); }

/* (norm x): x is numeric vec → F64 scalar; x is LIST of numeric vecs → F64 vector. */
ray_t* ray_norm_fn(ray_t* x) {
    if (!x) return ray_error("type", NULL);
    if (x->type == RAY_LIST) {
        int32_t dim;
        if (list_vec_validate(x, &dim) != 0) return ray_error("type", NULL);
        int64_t n = x->len;
        ray_t* result = ray_vec_new(RAY_F64, n);
        if (!result || RAY_IS_ERR(result)) return ray_error("oom", NULL);
        result->len = n;
        double* out = (double*)ray_data(result);
        for (int64_t i = 0; i < n; i++) {
            ray_t* v = ray_list_get(x, i);
            double s = 0.0;
            for (int32_t j = 0; j < dim; j++) {
                double e = rayvec_at_f64(v, j);
                s += e * e;
            }
            out[i] = sqrt(s);
        }
        return result;
    }
    if (!rayvec_is_numeric(x)) return ray_error("type", NULL);
    double s = 0.0;
    for (int64_t i = 0; i < x->len; i++) {
        double e = rayvec_at_f64(x, i);
        s += e * e;
    }
    return make_f64(sqrt(s));
}

/* Parse a metric symbol.  Accepted: 'cosine, 'l2, 'ip.  Matches the three
 * distance flavors. */
static int parse_metric_sym(ray_t* s, ray_hnsw_metric_t* out) {
    if (!s || s->type != -RAY_SYM) return 0;
    int64_t id = s->i64;
    if (id == ray_sym_find("cosine", 6)) { *out = RAY_HNSW_COSINE; return 1; }
    if (id == ray_sym_find("l2",     2)) { *out = RAY_HNSW_L2;     return 1; }
    if (id == ray_sym_find("ip",     2)) { *out = RAY_HNSW_IP;     return 1; }
    return 0;
}

static int64_t atom_to_i64(ray_t* a) {
    if (!a) return 0;
    switch (a->type) {
        case -RAY_I64: return a->i64;
        case -RAY_I32: return (int64_t)a->i32;
        case -RAY_I16: return (int64_t)a->i16;
        default: return 0;
    }
}

static bool atom_is_int(ray_t* a) {
    return a && (a->type == -RAY_I64 || a->type == -RAY_I32 || a->type == -RAY_I16);
}

/* (knn col query k [metric]) → table {_rowid, _dist} */
ray_t* ray_knn_fn(ray_t** args, int64_t n) {
    if (n < 3 || n > 4) return ray_error("rank", NULL);
    ray_t* col   = args[0];
    ray_t* query = args[1];
    ray_t* katom = args[2];
    if (!col || col->type != RAY_LIST) return ray_error("type", NULL);
    if (!rayvec_is_numeric(query))     return ray_error("type", NULL);
    if (!atom_is_int(katom))           return ray_error("type", NULL);

    ray_hnsw_metric_t metric = RAY_HNSW_COSINE;
    if (n == 4 && !parse_metric_sym(args[3], &metric)) return ray_error("domain", NULL);

    int32_t dim;
    if (list_vec_validate(col, &dim) != 0) return ray_error("type", NULL);
    if (query->len != dim) return ray_error("length", NULL);

    int64_t k = atom_to_i64(katom);
    if (k <= 0) return ray_error("domain", NULL);
    int64_t nrows = col->len;
    if (k > nrows) k = nrows;
    if (nrows == 0) {
        /* Empty result table. */
        ray_t* rv = ray_vec_new(RAY_I64, 0);
        ray_t* dv = ray_vec_new(RAY_F64, 0);
        ray_t* tbl = ray_table_new(2);
        tbl = ray_table_add_col(tbl, sym_intern_safe("_rowid", 6), rv);
        tbl = ray_table_add_col(tbl, sym_intern_safe("_dist",  5), dv);
        ray_release(rv); ray_release(dv);
        return tbl;
    }

    /* Prepare query as doubles (cached across all rows). */
    double q_norm;
    double* q = query_to_doubles(query, dim, &q_norm);
    if (!q) return ray_error("oom", NULL);

    /* Max-heap on distance (root = farthest of top-K kept). */
    typedef struct { double d; int64_t id; } ent_t;
    ent_t* heap = (ent_t*)ray_sys_alloc((size_t)k * sizeof(ent_t));
    if (!heap) { ray_sys_free(q); return ray_error("oom", NULL); }
    int64_t hsz = 0;

    for (int64_t i = 0; i < nrows; i++) {
        ray_t* row = ray_list_get(col, i);
        double d;
        switch (metric) {
            case RAY_HNSW_L2:
                d = row_score(MET_L2_DIST, row, q, q_norm, dim);
                break;
            case RAY_HNSW_IP:
                /* Negate inner product so lower = closer. */
                d = -row_score(MET_INNER_PROD, row, q, q_norm, dim);
                break;
            case RAY_HNSW_COSINE:
            default:
                d = row_score(MET_COS_DIST, row, q, q_norm, dim);
                break;
        }

        if (hsz < k) {
            int64_t j = hsz++;
            heap[j] = (ent_t){ d, i };
            while (j > 0) {
                int64_t p = (j - 1) / 2;
                if (heap[p].d >= heap[j].d) break;
                ent_t t = heap[p]; heap[p] = heap[j]; heap[j] = t;
                j = p;
            }
        } else if (d < heap[0].d) {
            heap[0] = (ent_t){ d, i };
            int64_t j = 0;
            for (;;) {
                int64_t l = 2*j+1, r = 2*j+2, best = j;
                if (l < hsz && heap[l].d > heap[best].d) best = l;
                if (r < hsz && heap[r].d > heap[best].d) best = r;
                if (best == j) break;
                ent_t t = heap[j]; heap[j] = heap[best]; heap[best] = t;
                j = best;
            }
        }
    }

    ray_sys_free(q);

    /* Sort ascending by distance. */
    for (int64_t i = 1; i < hsz; i++) {
        ent_t key = heap[i];
        int64_t j = i - 1;
        while (j >= 0 && heap[j].d > key.d) {
            heap[j + 1] = heap[j];
            j--;
        }
        heap[j + 1] = key;
    }

    ray_t* rv = ray_vec_new(RAY_I64, hsz);
    ray_t* dv = ray_vec_new(RAY_F64, hsz);
    if (!rv || RAY_IS_ERR(rv) || !dv || RAY_IS_ERR(dv)) {
        ray_sys_free(heap);
        if (rv && !RAY_IS_ERR(rv)) ray_release(rv);
        if (dv && !RAY_IS_ERR(dv)) ray_release(dv);
        return ray_error("oom", NULL);
    }
    int64_t* rd = (int64_t*)ray_data(rv);
    double*  dd = (double*)ray_data(dv);
    for (int64_t i = 0; i < hsz; i++) { rd[i] = heap[i].id; dd[i] = heap[i].d; }
    rv->len = hsz;
    dv->len = hsz;
    ray_sys_free(heap);

    ray_t* tbl = ray_table_new(2);
    if (!tbl || RAY_IS_ERR(tbl)) { ray_release(rv); ray_release(dv); return ray_error("oom", NULL); }
    tbl = ray_table_add_col(tbl, sym_intern_safe("_rowid", 6), rv);
    ray_release(rv);
    tbl = ray_table_add_col(tbl, sym_intern_safe("_dist",  5), dv);
    ray_release(dv);
    return tbl;
}

/* ---------- HNSW handle plumbing ---------- */

static ray_hnsw_t* hnsw_unwrap(ray_t* h) {
    if (!h) return NULL;
    if (h->type != -RAY_I64) return NULL;
    if (!(h->attrs & RAY_ATTR_HNSW)) return NULL;
    return (ray_hnsw_t*)(uintptr_t)h->i64;
}

static ray_t* hnsw_wrap(ray_hnsw_t* idx) {
    ray_t* h = ray_alloc(0);
    if (!h || RAY_IS_ERR(h)) return h ? h : ray_error("oom", NULL);
    h->type  = -RAY_I64;
    h->attrs |= RAY_ATTR_HNSW;
    h->i64   = (int64_t)(uintptr_t)idx;
    return h;
}

/* (hnsw-build col [metric] [M] [ef_c]) → I64 handle (RAY_ATTR_HNSW) */
ray_t* ray_hnsw_build_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 4) return ray_error("rank", NULL);
    ray_t* col = args[0];
    if (!col || col->type != RAY_LIST) return ray_error("type", NULL);

    ray_hnsw_metric_t metric = RAY_HNSW_COSINE;
    if (n >= 2 && !parse_metric_sym(args[1], &metric)) return ray_error("domain", NULL);

    int32_t M = HNSW_DEFAULT_M;
    if (n >= 3) {
        if (!atom_is_int(args[2])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[2]);
        if (v > 0 && v <= 512) M = (int32_t)v;
    }
    int32_t ef_c = HNSW_DEFAULT_EF_C;
    if (n >= 4) {
        if (!atom_is_int(args[3])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[3]);
        if (v > 0 && v <= 4096) ef_c = (int32_t)v;
    }

    int32_t dim;
    if (list_vec_validate(col, &dim) != 0) return ray_error("type", NULL);
    if (dim <= 0) return ray_error("length", NULL);

    int64_t n_rows;
    float* flat = list_flatten_floats(col, dim, &n_rows);
    if (!flat && n_rows > 0) return ray_error("oom", NULL);

    ray_hnsw_t* idx = ray_hnsw_build(flat, n_rows, dim, metric, M, ef_c);
    /* ray_hnsw_build COPIES the vectors (idx->owns_data == true), so free our scratch. */
    if (flat) ray_sys_free(flat);
    if (!idx) return ray_error("oom", NULL);

    ray_t* h = hnsw_wrap(idx);
    if (!h || RAY_IS_ERR(h)) { ray_hnsw_free(idx); return h; }
    return h;
}

/* (ann handle query k [ef_s]) → table {_rowid, _dist} */
ray_t* ray_ann_fn(ray_t** args, int64_t n) {
    if (n < 3 || n > 4) return ray_error("rank", NULL);
    ray_hnsw_t* idx = hnsw_unwrap(args[0]);
    if (!idx) return ray_error("type", NULL);
    if (!rayvec_is_numeric(args[1])) return ray_error("type", NULL);
    if (!atom_is_int(args[2]))       return ray_error("type", NULL);

    int32_t dim = idx->dim;
    if (args[1]->len != dim) return ray_error("length", NULL);
    int64_t k = atom_to_i64(args[2]);
    if (k <= 0) return ray_error("domain", NULL);

    int32_t ef = (int32_t)k;
    if (ef < HNSW_DEFAULT_EF_S) ef = HNSW_DEFAULT_EF_S;
    if (n == 4) {
        if (!atom_is_int(args[3])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[3]);
        if (v > 0 && v <= 4096) ef = (int32_t)v;
    }

    /* Copy query into float[] scratch. */
    float* qbuf = (float*)ray_sys_alloc((size_t)dim * sizeof(float));
    if (!qbuf) return ray_error("oom", NULL);
    rayvec_to_floats(args[1], qbuf, dim);

    int64_t* out_ids = (int64_t*)ray_sys_alloc((size_t)k * sizeof(int64_t));
    double*  out_ds  = (double*)ray_sys_alloc((size_t)k * sizeof(double));
    if (!out_ids || !out_ds) {
        ray_sys_free(qbuf);
        if (out_ids) ray_sys_free(out_ids);
        if (out_ds)  ray_sys_free(out_ds);
        return ray_error("oom", NULL);
    }

    int64_t found = ray_hnsw_search(idx, qbuf, dim, k, ef, out_ids, out_ds);
    if (found < 0) {
        ray_sys_free(qbuf); ray_sys_free(out_ids); ray_sys_free(out_ds);
        return ray_error("oom", NULL);
    }

    ray_t* rv = ray_vec_new(RAY_I64, found);
    ray_t* dv = ray_vec_new(RAY_F64, found);
    if (!rv || RAY_IS_ERR(rv) || !dv || RAY_IS_ERR(dv)) {
        ray_sys_free(qbuf); ray_sys_free(out_ids); ray_sys_free(out_ds);
        if (rv && !RAY_IS_ERR(rv)) ray_release(rv);
        if (dv && !RAY_IS_ERR(dv)) ray_release(dv);
        return ray_error("oom", NULL);
    }
    int64_t* rd = (int64_t*)ray_data(rv);
    double*  dd = (double*)ray_data(dv);
    for (int64_t i = 0; i < found; i++) { rd[i] = out_ids[i]; dd[i] = out_ds[i]; }
    rv->len = found;
    dv->len = found;
    ray_sys_free(qbuf); ray_sys_free(out_ids); ray_sys_free(out_ds);

    ray_t* tbl = ray_table_new(2);
    if (!tbl || RAY_IS_ERR(tbl)) { ray_release(rv); ray_release(dv); return ray_error("oom", NULL); }
    tbl = ray_table_add_col(tbl, sym_intern_safe("_rowid", 6), rv);
    ray_release(rv);
    tbl = ray_table_add_col(tbl, sym_intern_safe("_dist",  5), dv);
    ray_release(dv);
    return tbl;
}

/* (hnsw-free handle) → null.  Idempotent: clearing the ATTR on success
 * means a second call returns a type error rather than double-freeing. */
ray_t* ray_hnsw_free_fn(ray_t* h) {
    ray_hnsw_t* idx = hnsw_unwrap(h);
    if (!idx) return ray_error("type", NULL);
    ray_hnsw_free(idx);
    h->i64 = 0;
    h->attrs &= ~RAY_ATTR_HNSW;
    return RAY_NULL_OBJ;
}

/* (hnsw-save handle path) → null */
ray_t* ray_hnsw_save_fn(ray_t* h, ray_t* path) {
    ray_hnsw_t* idx = hnsw_unwrap(h);
    if (!idx) return ray_error("type", NULL);
    if (!path || path->type != -RAY_STR) return ray_error("type", NULL);
    const char* p = ray_str_ptr(path);
    size_t len = ray_str_len(path);
    if (!p || len == 0 || len >= 1023) return ray_error("domain", NULL);
    char buf[1024];
    memcpy(buf, p, len);
    buf[len] = '\0';
    ray_err_t err = ray_hnsw_save(idx, buf);
    if (err != RAY_OK) return ray_error("io", NULL);
    return RAY_NULL_OBJ;
}

/* (hnsw-load path) → I64 handle */
ray_t* ray_hnsw_load_fn(ray_t* path) {
    if (!path || path->type != -RAY_STR) return ray_error("type", NULL);
    const char* p = ray_str_ptr(path);
    size_t len = ray_str_len(path);
    if (!p || len == 0 || len >= 1023) return ray_error("domain", NULL);
    char buf[1024];
    memcpy(buf, p, len);
    buf[len] = '\0';
    ray_hnsw_t* idx = ray_hnsw_load(buf);
    if (!idx) return ray_error("io", NULL);
    ray_t* h = hnsw_wrap(idx);
    if (!h || RAY_IS_ERR(h)) { ray_hnsw_free(idx); return h; }
    return h;
}

/* (hnsw-info handle) → dict { nrows, dim, metric, nlayers, M, efc }.
 * Keys avoid hyphens so the 'quote-tick' syntax works: 'nrows, 'dim, etc. */
ray_t* ray_hnsw_info_fn(ray_t* h) {
    ray_hnsw_t* idx = hnsw_unwrap(h);
    if (!idx) return ray_error("type", NULL);

    const char* mname = "cosine";
    switch ((ray_hnsw_metric_t)idx->metric) {
        case RAY_HNSW_L2: mname = "l2"; break;
        case RAY_HNSW_IP: mname = "ip"; break;
        default: break;
    }

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 6);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(6);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    struct { const char* name; size_t nlen; ray_t* val; } rows[] = {
        { "nrows",   5, make_i64(idx->n_nodes)               },
        { "dim",     3, make_i64((int64_t)idx->dim)          },
        { "metric",  6, ray_sym(sym_intern_safe(mname, strlen(mname))) },
        { "nlayers", 7, make_i64((int64_t)idx->n_layers)     },
        { "M",       1, make_i64((int64_t)idx->M)            },
        { "efc",     3, make_i64((int64_t)idx->ef_construction) },
    };
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        int64_t s = sym_intern_safe(rows[i].name, rows[i].nlen);
        keys = ray_vec_append(keys, &s);
        vals = ray_list_append(vals, rows[i].val);
        ray_release(rows[i].val);
    }
    return ray_dict_new(keys, vals);
}
