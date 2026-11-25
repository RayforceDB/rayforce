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
#include "store/embmodel.h"
#include "core/pool.h"
#include <stdatomic.h>

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
 * Caller frees with ray_free_raw. */
static float* list_flatten_floats(ray_t* list, int32_t dim, int64_t* out_n) {
    int64_t n = list->len;
    *out_n = n;
    if (n == 0) return NULL;
    float* buf = (float*)ray_calloc_raw((size_t)n * (size_t)dim * sizeof(float));
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
 * These are the values returned by .emb.cos-dist / .emb.inner-prod / .emb.l2-dist builtins. */
typedef enum { MET_COS_DIST, MET_INNER_PROD, MET_L2_DIST } metric_kind_t;

/* The distance kernels, one for each float width, in the width of the rows:
 * plain loops over two arrays, which is the shape that turns into SIMD.  Out
 * of line, so that the row loop around them is not what gets vectorized. */
#define DEF_SCORE(SUF, T)                                                     \
static __attribute__((noinline)) double score_##SUF(metric_kind_t k, const T* a, \
                                                    const T* q, double q_norm, int32_t dim) { \
    if (k == MET_L2_DIST) {                                                   \
        T acc = 0;                                                            \
        for (int32_t j = 0; j < dim; j++) { T d = a[j] - q[j]; acc += d * d; } \
        return sqrt((double)acc);                                             \
    }                                                                         \
    if (k == MET_INNER_PROD) {                                                \
        T acc = 0;                                                            \
        for (int32_t j = 0; j < dim; j++) acc += a[j] * q[j];                 \
        return (double)acc;                                                   \
    }                                                                         \
    T acc = 0, nn = 0;                                                        \
    for (int32_t j = 0; j < dim; j++) { acc += a[j] * q[j]; nn += a[j] * a[j]; } \
    double denom = q_norm * sqrt((double)nn);                                 \
    return 1.0 - (denom > 0.0 ? (double)acc / denom : 0.0);                   \
}
DEF_SCORE(f64, double)
DEF_SCORE(f32, float)

/* A query, laid out once for whatever width a row turns out to have. */
typedef struct {
    double* d;      /* dim doubles; owns the block the floats are in too */
    float*  f;      /* the same as floats */
    double  norm;
    int32_t dim;
} emb_query_t;

static bool emb_query_init(emb_query_t* q, ray_t* v, int32_t dim) {
    q->d = (double*)ray_calloc_raw((size_t)(dim > 0 ? dim : 1) * (sizeof(double) + sizeof(float)));
    if (!q->d) return false;
    q->f = (float*)(void*)(q->d + dim);
    q->dim = dim;
    double ns = 0.0;
    for (int32_t j = 0; j < dim; j++) {
        q->d[j] = rayvec_at_f64(v, j);
        q->f[j] = (float)q->d[j];
        ns += q->d[j] * q->d[j];
    }
    q->norm = sqrt(ns);
    return true;
}

static void emb_query_drop(emb_query_t* q) { if (q->d) ray_free_raw(q->d); q->d = NULL; }

/* The distance of one row from the query.  A float row is computed in its
 * own width; an integer one element by element, as doubles. */
static double row_score(metric_kind_t k, ray_t* row, const emb_query_t* q) {
    int32_t dim = q->dim;
    if (row->type == RAY_F32) return score_f32(k, (const float*)ray_data(row), q->f, q->norm, dim);
    if (row->type == RAY_F64) return score_f64(k, (const double*)ray_data(row), q->d, q->norm, dim);
    double acc = 0.0, r_norm_sq = 0.0;
    if (k == MET_L2_DIST) {
        for (int32_t j = 0; j < dim; j++) {
            double d = rayvec_at_f64(row, j) - q->d[j];
            acc += d * d;
        }
        return sqrt(acc);
    }
    for (int32_t j = 0; j < dim; j++) {
        double a = rayvec_at_f64(row, j);
        acc += a * q->d[j];
        if (k == MET_COS_DIST) r_norm_sq += a * a;
    }
    if (k == MET_INNER_PROD) return acc;
    double denom = q->norm * sqrt(r_norm_sq);
    return 1.0 - (denom > 0.0 ? acc / denom : 0.0);
}

/* The rows of an embedding column go to the pool in runs; below this many
 * numbers in all a dispatch costs more than it saves. */
#define EMB_PAR_CELLS  (1 << 16)
#define EMB_RUN_MIN    64

/* How the rows are shared out: `per` rows a task, `tasks` of them; one task
 * and NULL when the pool is not to be used. */
static ray_pool_t* emb_rows_plan(int64_t nrows, int32_t dim, int64_t* per, int64_t* tasks) {
    ray_pool_t* pool = ray_pool_get();
    *per = nrows > 0 ? nrows : 1; *tasks = 1;
    if (!ray_pool_par_dispatch_ok(pool, nrows * (int64_t)dim, EMB_PAR_CELLS)) return NULL;
    int64_t nw = (int64_t)ray_pool_total_workers(pool);
    int64_t p = (nrows + nw * 4 - 1) / (nw * 4);
    if (p < EMB_RUN_MIN) p = EMB_RUN_MIN;
    int64_t t = (nrows + p - 1) / p;
    if (t < 2) return NULL;
    *per = p; *tasks = t;
    return pool;
}

static bool emb_cancelled(ray_pool_t* pool) {
    return ray_interrupted() || (pool && atomic_load_explicit(&pool->cancelled, memory_order_relaxed));
}

/* One distance for every row of a column. */
typedef struct {
    metric_kind_t kind;
    ray_t* list;
    const emb_query_t* q;
    double* out;
    int64_t n, per;
    _Atomic(int) bad;    /* a row that is not a numeric vector of the dimension */
} emb_dists_t;

static void emb_dists_task(void* vctx, uint32_t worker, int64_t start, int64_t end) {
    emb_dists_t* c = (emb_dists_t*)vctx;
    (void)worker; (void)end;
    int64_t lo = start * c->per, hi = lo + c->per < c->n ? lo + c->per : c->n;
    for (int64_t i = lo; i < hi; i++) {
        ray_t* row = ray_list_get(c->list, i);
        if (!rayvec_is_numeric(row) || row->len != c->q->dim) { atomic_store(&c->bad, 1); return; }
        /* Single-null float model: a metric over degenerate vectors can be
         * non-finite (NaN/Inf) → canonicalize to NULL_F64. */
        c->out[i] = ray_f64_fin(row_score(c->kind, row, c->q));
    }
}

static const char* metric_op_name(metric_kind_t kind) {
    switch (kind) {
        case MET_COS_DIST:   return ".emb.cos-dist";
        case MET_INNER_PROD: return ".emb.inner-prod";
        case MET_L2_DIST:    return ".emb.l2-dist";
        default:             return "vec-metric";
    }
}

/* Binary dispatcher for .emb.cos-dist / .emb.inner-prod / .emb.l2-dist. */
static ray_t* vec_binary_metric(metric_kind_t kind, ray_t* a, ray_t* b) {
    const char* op = metric_op_name(kind);
    /* a/b are NULL here, so no type names available. */
    if (!a || !b) return ray_error("type", "%s: missing operand", op);

    /* LIST × vec → F64 vector (one score per list entry).
     * vec × LIST → same (treat the LIST as the column). */
    ray_t* list = NULL;
    ray_t* query = NULL;
    if (a->type == RAY_LIST && rayvec_is_numeric(b))      { list = a; query = b; }
    else if (b->type == RAY_LIST && rayvec_is_numeric(a)) { list = b; query = a; }

    if (list) {
        /* The first row gives the dimension; every row is checked as it is computed. */
        int64_t n = list->len;
        ray_t* first = n > 0 ? ray_list_get(list, 0) : NULL;
        if (n > 0 && (!rayvec_is_numeric(first) || first->len <= 0 || first->len > INT32_MAX))
            return ray_error("type", "%s: list column must contain equal-length numeric vectors (f32, f64, i32 or i64)", op);
        int32_t dim = n > 0 ? (int32_t)first->len : 0;
        if (query->len != dim)
            return ray_error("length", "%s: query length must match column dim %lld, got %lld",
                             op, (long long)dim, (long long)query->len);

        emb_query_t q;
        if (!emb_query_init(&q, query, dim)) return ray_error("oom", NULL);
        ray_t* result = ray_vec_new(RAY_F64, n);
        if (!result || RAY_IS_ERR(result)) { emb_query_drop(&q); return ray_error("oom", NULL); }
        result->len = n;
        emb_dists_t c = { .kind = kind, .list = list, .q = &q, .out = (double*)ray_data(result), .n = n };
        int64_t tasks;
        ray_pool_t* pool = emb_rows_plan(n, dim, &c.per, &tasks);
        if (pool) ray_pool_dispatch_n(pool, emb_dists_task, &c, (uint32_t)tasks);
        else if (n > 0) emb_dists_task(&c, 0, 0, 1);
        emb_query_drop(&q);
        if (atomic_load(&c.bad)) {
            ray_release(result);
            return ray_error("type", "%s: list column must contain equal-length numeric vectors (f32, f64, i32 or i64)", op);
        }
        if (emb_cancelled(pool)) { ray_release(result); return ray_error("cancel", "interrupted"); }
        mark_f64_nonfinite_as_null(result, 0, n);
        return result;
    }

    /* vec × vec → scalar */
    if (!rayvec_is_numeric(a) || !rayvec_is_numeric(b))
        return ray_error("type", "%s: expects numeric vectors (f32, f64, i32 or i64), got %s and %s",
                         op, ray_type_name(a->type), ray_type_name(b->type));
    if (a->len != b->len || a->len <= 0)
        return ray_error("length", "%s: operands must be non-empty equal-length vectors, got %lld and %lld",
                         op, (long long)a->len, (long long)b->len);
    int32_t dim = (int32_t)a->len;

    emb_query_t q;
    if (!emb_query_init(&q, b, dim)) return ray_error("oom", NULL);
    double v = row_score(kind, a, &q);
    emb_query_drop(&q);
    return make_f64(v);
}

ray_t* ray_cos_dist_fn   (ray_t* a, ray_t* b) { return vec_binary_metric(MET_COS_DIST,   a, b); }
ray_t* ray_inner_prod_fn (ray_t* a, ray_t* b) { return vec_binary_metric(MET_INNER_PROD, a, b); }
ray_t* ray_l2_dist_fn    (ray_t* a, ray_t* b) { return vec_binary_metric(MET_L2_DIST,    a, b); }

/* (norm x): x is numeric vec → F64 scalar; x is LIST of numeric vecs → F64 vector. */
ray_t* ray_norm_fn(ray_t* x) {
    /* x is NULL here, so no type name available. */
    if (!x) return ray_error("type", "norm: missing operand");
    if (x->type == RAY_LIST) {
        int32_t dim;
        if (list_vec_validate(x, &dim) != 0)
            return ray_error("type", "norm: list must contain equal-length numeric vectors (f32, f64, i32 or i64)");
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
            out[i] = ray_f64_fin(sqrt(s));
        }
        mark_f64_nonfinite_as_null(result, 0, n);
        return result;
    }
    if (!rayvec_is_numeric(x))
        return ray_error("type", "norm: expects a numeric vector (f32, f64, i32 or i64) or list thereof, got %s",
                         ray_type_name(x->type));
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

/* One of the nearest so far.  Nearer is better, and of two as near, the
 * earlier row: so the answer does not depend on how the rows were shared out. */
typedef struct { double d; int64_t id; } knn_ent_t;

static inline bool knn_worse(knn_ent_t a, knn_ent_t b) {
    return a.d > b.d || (a.d == b.d && a.id > b.id);
}

static inline void knn_sift_down(knn_ent_t* heap, int64_t hsz, int64_t j) {
    for (;;) {
        int64_t l = 2 * j + 1, r = l + 1, top = j;
        if (l < hsz && knn_worse(heap[l], heap[top])) top = l;
        if (r < hsz && knn_worse(heap[r], heap[top])) top = r;
        if (top == j) break;
        knn_ent_t t = heap[j]; heap[j] = heap[top]; heap[top] = t;
        j = top;
    }
}

/* Keeps the k best: a heap whose root is the worst of those kept. */
static inline void knn_push(knn_ent_t* heap, int64_t* hsz, int64_t k, knn_ent_t e) {
    if (*hsz < k) {
        int64_t j = (*hsz)++;
        heap[j] = e;
        while (j > 0) {
            int64_t p = (j - 1) / 2;
            if (!knn_worse(heap[j], heap[p])) break;
            knn_ent_t t = heap[p]; heap[p] = heap[j]; heap[j] = t;
            j = p;
        }
    } else if (knn_worse(heap[0], e)) {
        heap[0] = e;
        knn_sift_down(heap, *hsz, 0);
    }
}

/* The exact search: each task keeps the best of its run of rows. */
typedef struct {
    ray_t* const* segs;      /* the column: one list, or a parted column's segments in order */
    const int64_t* off;      /* off[s]: the first row of segment s; off[ns]: the rows in all */
    int64_t ns;
    const int64_t* only;     /* the rows to look at, or NULL for all of them */
    const emb_query_t* q;
    ray_hnsw_metric_t metric;
    int64_t n, per, kk;      /* n: rows to look at; kk: what one task keeps, the lesser of k and a run */
    _Atomic(int64_t) bad_row;
    knn_ent_t* heaps;        /* tasks x kk */
    int64_t* sizes;          /* how many each task kept */
    _Atomic(int) bad;
} knn_ctx_t;

static void knn_task(void* vctx, uint32_t worker, int64_t start, int64_t end) {
    knn_ctx_t* c = (knn_ctx_t*)vctx;
    (void)worker; (void)end;
    int64_t lo = start * c->per, hi = lo + c->per < c->n ? lo + c->per : c->n;
    knn_ent_t* heap = c->heaps + start * c->kk;
    int64_t hsz = 0;
    /* The inner product is negated so that lower is closer, as with the others. */
    metric_kind_t kind = c->metric == RAY_HNSW_L2 ? MET_L2_DIST : c->metric == RAY_HNSW_IP ? MET_INNER_PROD : MET_COS_DIST;
    double sign = c->metric == RAY_HNSW_IP ? -1.0 : 1.0;
    int64_t nrows = c->off[c->ns];
    int64_t sg = 0;          /* the segment the last row was in: rows mostly go on from there */
    for (int64_t at = lo; at < hi; at++) {
        int64_t i = c->only ? c->only[at] : at;
        if (i < 0 || i >= nrows) continue;
        while (i >= c->off[sg + 1]) sg++;
        while (i < c->off[sg]) sg--;
        ray_t* seg = c->segs[sg];
        /* a partition without this column, or holding something else than vectors, has no row to give */
        ray_t* row = seg && seg->type == RAY_LIST ? ray_list_get(seg, i - c->off[sg]) : NULL;
        if (!rayvec_is_numeric(row) || row->len != c->q->dim) { atomic_store(&c->bad_row, i); atomic_store(&c->bad, 1); return; }
        knn_push(heap, &hsz, c->kk, (knn_ent_t){ sign * row_score(kind, row, c->q), i });
    }
    c->sizes[start] = hsz;
}

/* The exact search, for the builtin and for the query's operator alike: the
 * k nearest of the rows of `col` (all, or the n listed in `only`) to the
 * query, into ids and dists, nearest first.  Returns how many, or one of the
 * codes below; *bad_row is then the row that is no vector of the dimension. */
static int64_t knn_scan(ray_t* col, const int64_t* part_rows, int64_t n_parts, const int64_t* only, int64_t n,
                        const emb_query_t* q, ray_hnsw_metric_t metric, int64_t k, int64_t* ids, double* dists,
                        int64_t* bad_row) {
    if (k > n) k = n;
    if (k <= 0) return 0;
    /* The column as segments with the row each begins at: a list is one. */
    ray_t* one[1] = { col };
    int64_t one_off[2] = { 0, col->len };
    ray_t* const* segs = one;
    const int64_t* off = one_off;
    int64_t ns = 1;
    int64_t* made_off = NULL;
    if (RAY_IS_PARTED(col->type)) {
        ns = col->len;
        segs = (ray_t* const*)ray_data(col);
        made_off = (int64_t*)ray_alloc_raw((size_t)(ns + 1) * sizeof(int64_t));
        if (!made_off) return RAY_EMB_KNN_OOM;
        made_off[0] = 0;
        /* A partition that lacks the column still has its rows, and they are
         * numbered: counted as every reader of a parted column counts them
         * (parted_seg_rows), so that a row number means here what it means to
         * the filter before the search and to the gather after it. */
        for (int64_t s = 0; s < ns; s++)
            made_off[s + 1] = made_off[s] + parted_seg_rows((ray_t**)ray_data(col), s, part_rows, n_parts);
        off = made_off;
        if (ns == 0 || off[ns] == 0) { ray_free_raw(made_off); return 0; }
    }
    knn_ctx_t c = { .segs = segs, .off = off, .ns = ns, .only = only, .q = q, .metric = metric, .n = n };
    int64_t tasks;
    ray_pool_t* pool = emb_rows_plan(n, q->dim, &c.per, &tasks);
    c.kk = k < c.per ? k : c.per;
    c.heaps = (knn_ent_t*)ray_calloc_raw((size_t)tasks * (size_t)c.kk * sizeof(knn_ent_t));
    c.sizes = (int64_t*)ray_calloc_raw((size_t)tasks * sizeof(int64_t));
    knn_ent_t* heap = (knn_ent_t*)ray_calloc_raw((size_t)k * sizeof(knn_ent_t));
    int64_t out = RAY_EMB_KNN_OOM;
    if (c.heaps && c.sizes && heap) {
        if (pool) ray_pool_dispatch_n(pool, knn_task, &c, (uint32_t)tasks);
        else knn_task(&c, 0, 0, 1);
        if (atomic_load(&c.bad)) { out = RAY_EMB_KNN_BAD_ROW; if (bad_row) *bad_row = atomic_load(&c.bad_row); }
        else if (emb_cancelled(pool)) out = RAY_EMB_KNN_CANCELLED;
        else {
            /* the best of all the runs, then nearest first */
            int64_t hsz = 0;
            for (int64_t t = 0; t < tasks; t++)
                for (int64_t i = 0; i < c.sizes[t]; i++) knn_push(heap, &hsz, k, c.heaps[t * c.kk + i]);
            for (int64_t i = hsz - 1; i > 0; i--) {
                knn_ent_t t = heap[0]; heap[0] = heap[i]; heap[i] = t;
                knn_sift_down(heap, i, 0);
            }
            for (int64_t i = 0; i < hsz; i++) { ids[i] = heap[i].id; dists[i] = heap[i].d; }
            out = hsz;
        }
    }
    if (c.heaps) ray_free_raw(c.heaps);
    if (c.sizes) ray_free_raw(c.sizes);
    if (heap) ray_free_raw(heap);
    if (made_off) ray_free_raw(made_off);
    return out;
}

int64_t ray_emb_knn_scan(ray_t* col, const int64_t* part_rows, int64_t n_parts, const int64_t* only, int64_t n,
                         const float* query, int32_t dim, int metric, int64_t k, int64_t* ids, double* dists, int64_t* bad_row) {
    emb_query_t q;
    q.d = (double*)ray_calloc_raw((size_t)(dim > 0 ? dim : 1) * (sizeof(double) + sizeof(float)));
    if (!q.d) return RAY_EMB_KNN_OOM;
    q.f = (float*)(void*)(q.d + dim);
    q.dim = dim;
    double ns = 0.0;
    for (int32_t j = 0; j < dim; j++) { q.f[j] = query[j]; q.d[j] = (double)query[j]; ns += q.d[j] * q.d[j]; }
    q.norm = sqrt(ns);
    int64_t out = knn_scan(col, part_rows, n_parts, only, n, &q, (ray_hnsw_metric_t)metric, k, ids, dists, bad_row);
    emb_query_drop(&q);
    return out;
}

/* (.emb.knn col query k [metric]) → table {_rowid, _dist} */
ray_t* ray_knn_fn(ray_t** args, int64_t n) {
    if (n < 3 || n > 4) return ray_error("rank", ".emb.knn: expects 3 or 4 args (col query k [metric]), got %lld", (long long)n);
    ray_t* col   = args[0];
    ray_t* query = args[1];
    ray_t* katom = args[2];
    if (!col || col->type != RAY_LIST)
        return ray_error("type", ".emb.knn: col must be a list of numeric vectors, got %s", ray_type_name(col ? col->type : 0));
    if (!rayvec_is_numeric(query))
        return ray_error("type", ".emb.knn: query must be a numeric vector (f32, f64, i32 or i64), got %s", ray_type_name(query ? query->type : 0));
    if (!atom_is_int(katom))
        return ray_error("type", ".emb.knn: k must be an integer atom, got %s", ray_type_name(katom ? katom->type : 0));

    ray_hnsw_metric_t metric = RAY_HNSW_COSINE;
    if (n == 4 && !parse_metric_sym(args[3], &metric))
        return ray_error("domain", ".emb.knn: metric must be 'cosine, 'l2 or 'ip");

    /* The first row gives the dimension; every row is checked as it is computed. */
    int64_t nrows = col->len;
    ray_t* first = nrows > 0 ? ray_list_get(col, 0) : NULL;
    if (nrows > 0 && (!rayvec_is_numeric(first) || first->len <= 0 || first->len > INT32_MAX))
        return ray_error("type", ".emb.knn: col must contain equal-length numeric vectors (f32, f64, i32 or i64)");
    int32_t dim = nrows > 0 ? (int32_t)first->len : 0;
    if (query->len != dim)
        return ray_error("length", ".emb.knn: query length must match column dim %lld, got %lld",
                         (long long)dim, (long long)query->len);

    int64_t k = atom_to_i64(katom);
    if (k <= 0) return ray_error("domain", ".emb.knn: k must be positive, got %lld", (long long)k);
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

    ray_t* rv = ray_vec_new(RAY_I64, k);
    ray_t* dv = ray_vec_new(RAY_F64, k);
    emb_query_t q = {0};
    if (!rv || RAY_IS_ERR(rv) || !dv || RAY_IS_ERR(dv) || !emb_query_init(&q, query, dim)) {
        if (rv && !RAY_IS_ERR(rv)) ray_release(rv);
        if (dv && !RAY_IS_ERR(dv)) ray_release(dv);
        return ray_error("oom", NULL);
    }
    int64_t got = knn_scan(col, NULL, 0, NULL, nrows, &q, metric, k, (int64_t*)ray_data(rv), (double*)ray_data(dv), NULL);
    emb_query_drop(&q);
    if (got < 0) {
        ray_release(rv); ray_release(dv);
        if (got == RAY_EMB_KNN_BAD_ROW)
            return ray_error("type", ".emb.knn: col must contain equal-length numeric vectors (f32, f64, i32 or i64)");
        return got == RAY_EMB_KNN_CANCELLED ? ray_error("cancel", "interrupted") : ray_error("oom", NULL);
    }
    rv->len = got;
    dv->len = got;

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

/* (.emb.hnsw.build col [metric] [M] [ef_c]) → I64 handle (RAY_ATTR_HNSW) */
ray_t* ray_hnsw_build_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 4) return ray_error("rank", ".emb.hnsw.build: expects 1 to 4 args (col [metric] [M] [ef_c]), got %lld", (long long)n);
    ray_t* col = args[0];
    if (!col || col->type != RAY_LIST)
        return ray_error("type", ".emb.hnsw.build: col must be a list of numeric vectors, got %s", ray_type_name(col ? col->type : 0));

    ray_hnsw_metric_t metric = RAY_HNSW_COSINE;
    if (n >= 2 && !parse_metric_sym(args[1], &metric))
        return ray_error("domain", ".emb.hnsw.build: metric must be 'cosine, 'l2 or 'ip");

    int32_t M = HNSW_DEFAULT_M;
    if (n >= 3) {
        if (!atom_is_int(args[2]))
            return ray_error("type", ".emb.hnsw.build: M must be an integer atom, got %s", ray_type_name(args[2] ? args[2]->type : 0));
        int64_t v = atom_to_i64(args[2]);
        if (v > 0 && v <= 512) M = (int32_t)v;
    }
    int32_t ef_c = HNSW_DEFAULT_EF_C;
    if (n >= 4) {
        if (!atom_is_int(args[3]))
            return ray_error("type", ".emb.hnsw.build: ef_c must be an integer atom, got %s", ray_type_name(args[3] ? args[3]->type : 0));
        int64_t v = atom_to_i64(args[3]);
        if (v > 0 && v <= 4096) ef_c = (int32_t)v;
    }

    int32_t dim;
    if (list_vec_validate(col, &dim) != 0)
        return ray_error("type", ".emb.hnsw.build: col must contain equal-length numeric vectors (f32, f64, i32 or i64)");
    if (dim <= 0) return ray_error("length", ".emb.hnsw.build: vector dim must be positive, got %lld", (long long)dim);

    int64_t n_rows;
    float* flat = list_flatten_floats(col, dim, &n_rows);
    if (!flat && n_rows > 0) return ray_error("oom", NULL);

    ray_hnsw_t* idx = ray_hnsw_build(flat, n_rows, dim, metric, M, ef_c);
    /* ray_hnsw_build COPIES the vectors (idx->owns_data == true), so free our scratch. */
    if (flat) ray_free_raw(flat);
    if (!idx) return ray_interrupted() ? ray_error("cancel", "interrupted")
                                       : ray_error("oom", NULL);

    ray_t* h = hnsw_wrap(idx);
    if (!h || RAY_IS_ERR(h)) { ray_hnsw_free(idx); return h; }
    return h;
}

/* (.emb.ann handle query k [ef_s]) → table {_rowid, _dist} */
ray_t* ray_ann_fn(ray_t** args, int64_t n) {
    if (n < 3 || n > 4) return ray_error("rank", ".emb.ann: expects 3 or 4 args (handle query k [ef_s]), got %lld", (long long)n);
    ray_hnsw_t* idx = hnsw_unwrap(args[0]);
    if (!idx) return ray_error("type", ".emb.ann: handle must be an hnsw index handle, got %s", ray_type_name(args[0] ? args[0]->type : 0));
    if (!rayvec_is_numeric(args[1]))
        return ray_error("type", ".emb.ann: query must be a numeric vector (f32, f64, i32 or i64), got %s", ray_type_name(args[1] ? args[1]->type : 0));
    if (!atom_is_int(args[2]))
        return ray_error("type", ".emb.ann: k must be an integer atom, got %s", ray_type_name(args[2] ? args[2]->type : 0));

    int32_t dim = idx->dim;
    if (args[1]->len != dim)
        return ray_error("length", ".emb.ann: query length must match index dim %lld, got %lld",
                         (long long)dim, (long long)args[1]->len);
    int64_t k = atom_to_i64(args[2]);
    if (k <= 0) return ray_error("domain", ".emb.ann: k must be positive, got %lld", (long long)k);

    int32_t ef = (int32_t)k;
    if (ef < HNSW_DEFAULT_EF_S) ef = HNSW_DEFAULT_EF_S;
    if (n == 4) {
        if (!atom_is_int(args[3]))
            return ray_error("type", ".emb.ann: ef_s must be an integer atom, got %s", ray_type_name(args[3] ? args[3]->type : 0));
        int64_t v = atom_to_i64(args[3]);
        if (v > 0 && v <= 4096) ef = (int32_t)v;
    }

    /* Copy query into float[] scratch. */
    float* qbuf = (float*)ray_calloc_raw((size_t)dim * sizeof(float));
    if (!qbuf) return ray_error("oom", NULL);
    rayvec_to_floats(args[1], qbuf, dim);

    int64_t* out_ids = (int64_t*)ray_calloc_raw((size_t)k * sizeof(int64_t));
    double*  out_ds  = (double*)ray_calloc_raw((size_t)k * sizeof(double));
    if (!out_ids || !out_ds) {
        ray_free_raw(qbuf);
        if (out_ids) ray_free_raw(out_ids);
        if (out_ds)  ray_free_raw(out_ds);
        return ray_error("oom", NULL);
    }

    int64_t found = ray_hnsw_search(idx, qbuf, dim, k, ef, out_ids, out_ds);
    if (found < 0) {
        ray_free_raw(qbuf); ray_free_raw(out_ids); ray_free_raw(out_ds);
        return ray_error("oom", NULL);
    }

    ray_t* rv = ray_vec_new(RAY_I64, found);
    ray_t* dv = ray_vec_new(RAY_F64, found);
    if (!rv || RAY_IS_ERR(rv) || !dv || RAY_IS_ERR(dv)) {
        ray_free_raw(qbuf); ray_free_raw(out_ids); ray_free_raw(out_ds);
        if (rv && !RAY_IS_ERR(rv)) ray_release(rv);
        if (dv && !RAY_IS_ERR(dv)) ray_release(dv);
        return ray_error("oom", NULL);
    }
    int64_t* rd = (int64_t*)ray_data(rv);
    double*  dd = (double*)ray_data(dv);
    for (int64_t i = 0; i < found; i++) { rd[i] = out_ids[i]; dd[i] = out_ds[i]; }
    rv->len = found;
    dv->len = found;
    ray_free_raw(qbuf); ray_free_raw(out_ids); ray_free_raw(out_ds);

    ray_t* tbl = ray_table_new(2);
    if (!tbl || RAY_IS_ERR(tbl)) { ray_release(rv); ray_release(dv); return ray_error("oom", NULL); }
    tbl = ray_table_add_col(tbl, sym_intern_safe("_rowid", 6), rv);
    ray_release(rv);
    tbl = ray_table_add_col(tbl, sym_intern_safe("_dist",  5), dv);
    ray_release(dv);
    return tbl;
}

/* (.emb.hnsw.free handle) → null.  Idempotent: clearing the ATTR on success
 * means a second call returns a type error rather than double-freeing. */
ray_t* ray_hnsw_free_fn(ray_t* h) {
    ray_hnsw_t* idx = hnsw_unwrap(h);
    if (!idx) return ray_error("type", ".emb.hnsw.free: expects an hnsw index handle, got %s", ray_type_name(h ? h->type : 0));
    ray_hnsw_free(idx);
    h->i64 = 0;
    h->attrs &= ~RAY_ATTR_HNSW;
    return RAY_NULL_OBJ;
}

/* (.emb.hnsw.set path handle) → null.  The path comes first, as in
 * (.db.splayed.set dir table). */
ray_t* ray_hnsw_save_fn(ray_t* path, ray_t* h) {
    ray_hnsw_t* idx = hnsw_unwrap(h);
    if (!idx) return ray_error("type", ".emb.hnsw.set: handle must be an hnsw index handle, got %s", ray_type_name(h ? h->type : 0));
    if (!path || path->type != -RAY_STR)
        return ray_error("type", ".emb.hnsw.set: path must be a string, got %s", ray_type_name(path ? path->type : 0));
    const char* p = ray_str_ptr(path);
    size_t len = ray_str_len(path);
    if (!p || len == 0 || len >= 1023)
        return ray_error("domain", ".emb.hnsw.set: path length must be 1 to 1022 bytes, got %lld", (long long)len);
    char buf[1024];
    memcpy(buf, p, len);
    buf[len] = '\0';
    ray_err_t err = ray_hnsw_save(idx, buf);
    if (err != RAY_OK) return ray_error("io", NULL);
    return RAY_NULL_OBJ;
}

/* (.emb.hnsw.get path) → I64 handle */
ray_t* ray_hnsw_load_fn(ray_t* path) {
    if (!path || path->type != -RAY_STR)
        return ray_error("type", ".emb.hnsw.get: path must be a string, got %s", ray_type_name(path ? path->type : 0));
    const char* p = ray_str_ptr(path);
    size_t len = ray_str_len(path);
    if (!p || len == 0 || len >= 1023)
        return ray_error("domain", ".emb.hnsw.get: path length must be 1 to 1022 bytes, got %lld", (long long)len);
    char buf[1024];
    memcpy(buf, p, len);
    buf[len] = '\0';
    ray_hnsw_t* idx = ray_hnsw_load(buf);
    if (!idx)
        return ray_error("io", ".emb.hnsw.get: index '%s' is invalid or incomplete; "
                               "restore a compatible copy or rebuild it",
                         buf);
    ray_t* h = hnsw_wrap(idx);
    if (!h || RAY_IS_ERR(h)) { ray_hnsw_free(idx); return h; }
    return h;
}

/* (.emb.hnsw.info handle) → dict { nrows, dim, metric, nlayers, M, efc }.
 * Keys avoid hyphens so the 'quote-tick' syntax works: 'nrows, 'dim, etc. */
ray_t* ray_hnsw_info_fn(ray_t* h) {
    ray_hnsw_t* idx = hnsw_unwrap(h);
    if (!idx) return ray_error("type", ".emb.hnsw.info: expects an hnsw index handle, got %s", ray_type_name(h ? h->type : 0));

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

/* ---------- embedding models ---------- */

static ray_embmodel_t* embmodel_unwrap(ray_t* h) {
    if (!h || h->type != -RAY_I64 || !(h->attrs & RAY_ATTR_EMBMODEL)) return NULL;
    return (ray_embmodel_t*)(uintptr_t)h->i64;
}

/* (.emb.model.get dir) → I64 handle (RAY_ATTR_EMBMODEL).  The directory is a
 * model as its publisher ships it: config.json, tokenizer.json and
 * model.safetensors. */
ray_t* ray_emb_model_get_fn(ray_t* path) {
    if (!path || path->type != -RAY_STR)
        return ray_error("type", ".emb.model.get: path must be a string, got %s", ray_type_name(path ? path->type : 0));
    const char* p = ray_str_ptr(path);
    size_t len = ray_str_len(path);
    if (!p || len == 0 || len >= 1023)
        return ray_error("domain", ".emb.model.get: path length must be 1 to 1022 bytes, got %lld", (long long)len);
    char buf[1024];
    memcpy(buf, p, len);
    buf[len] = '\0';
    ray_t* err = NULL;
    ray_embmodel_t* m = ray_embmodel_open(buf, &err);
    if (!m) return err ? err : ray_error("io", ".emb.model.get: the model could not be opened");
    ray_t* h = ray_alloc(0);
    if (!h || RAY_IS_ERR(h)) { ray_embmodel_release(m); return h ? h : ray_error("oom", NULL); }
    h->type  = -RAY_I64;
    h->attrs |= RAY_ATTR_EMBMODEL;
    h->i64   = (int64_t)(uintptr_t)m;
    return h;
}

/* (.emb.model.info handle) → dict { kind, tokenizer, vocab, dim, layers, heads, inner, tokens } */
ray_t* ray_emb_model_info_fn(ray_t* h) {
    ray_embmodel_t* m = embmodel_unwrap(h);
    if (!m) return ray_error("type", ".emb.model.info: expects a model handle, got %s", ray_type_name(h ? h->type : 0));
    ray_embmodel_info_t in;
    ray_embmodel_info(m, &in);
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 8);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(8);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    struct { const char* name; ray_t* val; } rows[] = {
        { "kind",      ray_sym(sym_intern_safe(in.kind, strlen(in.kind))) },
        { "tokenizer", ray_sym(sym_intern_safe(in.tokenizer, strlen(in.tokenizer))) },
        { "vocab",     make_i64(in.vocab) },
        { "dim",       make_i64(in.dim) },
        { "layers",    make_i64(in.layers) },
        { "heads",     make_i64(in.heads) },
        { "inner",     make_i64(in.inner) },
        { "tokens",    make_i64(in.max_tokens) },
    };
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        int64_t s = sym_intern_safe(rows[i].name, strlen(rows[i].name));
        keys = ray_vec_append(keys, &s);
        vals = ray_list_append(vals, rows[i].val);
        ray_release(rows[i].val);
    }
    return ray_dict_new(keys, vals);
}

static ray_t* emb_ids_vec(const int32_t* ids, int64_t n) {
    ray_t* v = ray_vec_new(RAY_I32, n);
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom", NULL);
    v->len = n;
    if (n > 0) memcpy(ray_data(v), ids, (size_t)n * sizeof(int32_t));
    return v;
}

/* (.emb.tokens handle text) → the ids the model's tokenizer cuts the text
 * into, an I32 vector; a vector of strings gives a list of them. */
ray_t* ray_emb_tokens_fn(ray_t* h, ray_t* x) {
    ray_embmodel_t* m = embmodel_unwrap(h);
    if (!m) return ray_error("type", ".emb.tokens: expects a model handle, got %s", ray_type_name(h ? h->type : 0));
    if (!x || (x->type != -RAY_STR && x->type != RAY_STR))
        return ray_error("type", ".emb.tokens: expects a string or a vector of strings, got %s", ray_type_name(x ? x->type : 0));
    ray_embwork_t* w = ray_embwork_new(m);
    if (!w) return ray_error("oom", NULL);
    ray_t* out;
    const int32_t* ids;
    if (x->type == -RAY_STR) {
        int64_t n = ray_embmodel_tokens(m, w, ray_str_ptr(x), ray_str_len(x), &ids);
        out = n < 0 ? ray_error("oom", NULL) : emb_ids_vec(ids, n);
    } else {
        out = ray_list_new(x->len);
        for (int64_t i = 0; i < x->len && out && !RAY_IS_ERR(out); i++) {
            size_t len = 0;
            const char* s = ray_str_vec_get(x, i, &len);
            int64_t n = ray_embmodel_tokens(m, w, s ? s : "", s ? len : 0, &ids);
            ray_t* row = n < 0 ? ray_error("oom", NULL) : emb_ids_vec(ids, n);
            if (RAY_IS_ERR(row)) { ray_release(out); out = row; break; }
            out = ray_list_append(out, row);
            ray_release(row);
        }
        if (!out) out = ray_error("oom", NULL);
    }
    ray_embwork_free(w);
    return out;
}

/* One task of .emb.encode: a run of texts, each into the vector made for it. */
typedef struct {
    const ray_embmodel_t* m;
    ray_t* texts;            /* RAY_STR vector */
    ray_t** rows;            /* the F32 vectors, made before the dispatch */
    ray_embwork_t** work;    /* one per worker, made by the worker that first needs it */
    ray_pool_t* inner;       /* set when the texts go one after another, each over all the workers */
    int64_t n, per, max_tokens;
    _Atomic(int) failed;     /* 1 out of memory */
} emb_encode_ctx_t;

static void emb_encode_task(void* vctx, uint32_t worker, int64_t start, int64_t end) {
    emb_encode_ctx_t* c = (emb_encode_ctx_t*)vctx;
    (void)end;
    if (atomic_load_explicit(&c->failed, memory_order_relaxed)) return;
    ray_embwork_t* w = c->work[worker];
    if (!w) { w = c->work[worker] = ray_embwork_new(c->m); if (!w) { atomic_store(&c->failed, 1); return; } }
    int64_t lo = start * c->per, hi = lo + c->per < c->n ? lo + c->per : c->n;
    for (int64_t i = lo; i < hi; i++) {
        if (ray_interrupted()) return;   /* the caller sees it after the dispatch */
        size_t len = 0;
        const char* s = ray_str_vec_get(c->texts, i, &len);
        if (ray_embmodel_encode(c->m, w, c->inner, s ? s : "", s ? len : 0, c->max_tokens, (float*)ray_data(c->rows[i])) < 0) {
            atomic_store(&c->failed, 1);
            return;
        }
    }
}

/* (.emb.encode handle text [max-tokens]) → the text's vector, F32, of length
 * one; a vector of strings gives a list of them, computed on the pool. */
ray_t* ray_emb_encode_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 3) return ray_error("rank", ".emb.encode: expects 2 or 3 args (model text [max-tokens]), got %lld", (long long)n);
    ray_embmodel_t* m = embmodel_unwrap(args[0]);
    if (!m) return ray_error("type", ".emb.encode: expects a model handle, got %s", ray_type_name(args[0] ? args[0]->type : 0));
    ray_t* x = args[1];
    if (!x || (x->type != -RAY_STR && x->type != RAY_STR))
        return ray_error("type", ".emb.encode: expects a string or a vector of strings, got %s", ray_type_name(x ? x->type : 0));
    int64_t max_tokens = 0;
    if (n == 3) {
        if (!args[2] || args[2]->type != -RAY_I64 || args[2]->attrs != 0 || args[2]->i64 < 2)
            return ray_error("domain", ".emb.encode: max-tokens must be an integer of 2 or more");
        max_tokens = args[2]->i64;
    }
    ray_embmodel_info_t in;
    ray_embmodel_info(m, &in);
    if (x->type == -RAY_STR) {
        ray_embwork_t* w = ray_embwork_new(m);
        if (!w) return ray_error("oom", NULL);
        ray_t* v = ray_vec_new(RAY_F32, in.dim);
        if (v && !RAY_IS_ERR(v)) {
            v->len = in.dim;
            /* One text has the whole pool: the rows of its products and its heads go to the workers. */
            ray_pool_t* pool = ray_pool_get();
            if (!ray_pool_par_dispatch_ok(pool, 1, 1)) pool = NULL;
            if (ray_embmodel_encode(m, w, pool, ray_str_ptr(x), ray_str_len(x), max_tokens, (float*)ray_data(v)) < 0) {
                ray_release(v);
                v = ray_error("oom", NULL);
            } else if (ray_interrupted() || (pool && atomic_load_explicit(&pool->cancelled, memory_order_relaxed))) {
                ray_release(v);
                v = ray_error("cancel", "interrupted");
            }
        }
        ray_embwork_free(w);
        return v ? v : ray_error("oom", NULL);
    }
    int64_t cnt = x->len;
    ray_t* out = ray_list_new(cnt);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int64_t i = 0; i < cnt; i++) {
        ray_t* v = ray_vec_new(RAY_F32, in.dim);
        if (!v || RAY_IS_ERR(v)) { ray_release(out); return v ? v : ray_error("oom", NULL); }
        v->len = in.dim;
        out = ray_list_append(out, v);
        ray_release(v);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    }
    if (cnt == 0) return out;
    ray_pool_t* pool = ray_pool_get();
    if (!ray_pool_par_dispatch_ok(pool, 1, 1)) pool = NULL;
    uint32_t workers = pool ? ray_pool_total_workers(pool) : 1;
    emb_encode_ctx_t c = { .m = m, .texts = x, .rows = (ray_t**)ray_data(out), .n = cnt, .max_tokens = max_tokens };
    c.work = (ray_embwork_t**)ray_sys_alloc((size_t)workers * sizeof(ray_embwork_t*));
    if (!c.work) { ray_release(out); return ray_error("oom", NULL); }
    /* Texts differ in length, so the runs are short: a worker that drew long ones does not hold the rest up. */
    c.per = cnt / ((int64_t)workers * 16) + 1;
    int64_t tasks = (cnt + c.per - 1) / c.per;
    /* Texts enough to keep every worker busy go to the workers whole.  Fewer
     * go one after another, each over all the workers, so that a handful of
     * long texts does not leave most of the pool idle. */
    if (pool && cnt >= (int64_t)workers) ray_pool_dispatch_n(pool, emb_encode_task, &c, (uint32_t)tasks);
    else {
        c.inner = pool;
        for (int64_t t = 0; t < tasks; t++) emb_encode_task(&c, 0, t, t + 1);
    }
    for (uint32_t i = 0; i < workers; i++) ray_embwork_free(c.work[i]);
    ray_sys_free(c.work);
    if (atomic_load(&c.failed)) { ray_release(out); return ray_error("oom", NULL); }
    /* A cancelled dispatch skips the tasks it had not started: the vectors
     * of those texts were never computed. */
    if (ray_interrupted() || (pool && atomic_load_explicit(&pool->cancelled, memory_order_relaxed))) {
        ray_release(out);
        return ray_error("cancel", "interrupted");
    }
    return out;
}
