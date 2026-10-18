/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "csr.h"
#include "store/col.h"
#include "mem/sys.h"
#include "store/fileio.h"   /* ray_mkdir */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include "core/pool.h"

/* Forward declaration */
static void csr_free(ray_csr_t* csr);

/* --------------------------------------------------------------------------
 * CSR construction helpers
 * -------------------------------------------------------------------------- */

/* One (target, row) cell of an adjacency list, for sorting a long list. */
typedef struct {
    int64_t target;
    int64_t row;
} adj_cell_t;

static int cmp_adj_cell(const void* a, const void* b) {
    const adj_cell_t* x = (const adj_cell_t*)a;
    const adj_cell_t* y = (const adj_cell_t*)b;
    if (x->target < y->target) return -1;
    if (x->target > y->target) return 1;
    if (x->row < y->row) return -1;
    if (x->row > y->row) return 1;
    return 0;
}

/* Sort every adjacency list by (target, row).
 *
 * The order is a total one, so a CSR built from the same edges is the same
 * bytes on every platform: parallel edges (equal key AND equal target) are
 * pinned by their original row, which rowmap (and any golden test or MST
 * that consumes it) relies on.  Lists are short in the common case, so an
 * insertion sort serves them; a hub's list past ADJ_INSERTION_MAX is packed
 * into (target, row) cells and qsorted to stay O(d log d). */
#define ADJ_INSERTION_MAX 32

static void csr_sort_adjacency_lists(ray_csr_t* csr) {
    int64_t* offsets = (int64_t*)ray_data(csr->offsets);
    int64_t* targets = (int64_t*)ray_data(csr->targets);
    int64_t* rowmap = csr->rowmap ? (int64_t*)ray_data(csr->rowmap) : NULL;

    adj_cell_t* cells = NULL;
    int64_t cells_cap = 0;

    for (int64_t node = 0; node < csr->n_nodes; node++) {
        int64_t start = offsets[node];
        int64_t end = offsets[node + 1];
        int64_t deg = end - start;
        if (deg <= 1) continue;

        if (deg > ADJ_INSERTION_MAX && rowmap) {
            if (deg > cells_cap) {
                ray_free_raw(cells);
                cells_cap = deg;
                cells = (adj_cell_t*)ray_alloc_raw((size_t)cells_cap * sizeof(adj_cell_t));
                if (!cells) { cells_cap = 0; goto insertion; }  /* OOM: slow path */
            }
            for (int64_t i = 0; i < deg; i++) {
                cells[i].target = targets[start + i];
                cells[i].row = rowmap[start + i];
            }
            qsort(cells, (size_t)deg, sizeof(adj_cell_t), cmp_adj_cell);
            for (int64_t i = 0; i < deg; i++) {
                targets[start + i] = cells[i].target;
                rowmap[start + i] = cells[i].row;
            }
            continue;
        }

insertion:
        for (int64_t i = start + 1; i < end; i++) {
            int64_t key = targets[i];
            int64_t row_key = rowmap ? rowmap[i] : 0;
            int64_t j = i - 1;
            while (j >= start && (targets[j] > key ||
                                  (targets[j] == key && rowmap && rowmap[j] > row_key))) {
                targets[j + 1] = targets[j];
                if (rowmap) rowmap[j + 1] = rowmap[j];
                j--;
            }
            targets[j + 1] = key;
            if (rowmap) rowmap[j + 1] = row_key;
        }
    }
    ray_free_raw(cells);
}

/* Build one CSR direction straight from the edge columns by counting sort:
 * degree count, prefix sum, scatter in row order, then sort each list.
 *
 * keys[i] is the list an edge belongs to and vals[i] its target; a NULL
 * keys means key = i (an FK column: every row of the source table is one
 * node with one outgoing edge).  An edge whose key falls outside
 * [0, n_nodes) is dropped; its val is stored as-is, like the row index.
 *
 * The lists come out sorted by (target, row), the one order a build from
 * these edges has ever had, so `sort_targets` only records that fact. */
/* Parallel two-level build.  Keys are first partitioned into B contiguous
 * node-range buckets (B ~ CSR_PAR_BUCKETS, each a power-of-two range of
 * nodes), then every bucket is counting-sorted on its own.  Each task has
 * a private histogram row, so no pass touches a shared counter; the only
 * cross-task step is the serial prefix scan over T*B entries.
 *
 * Scratch is (key, val) as int32 when n_nodes fits (the usual case; the
 * scatter is memory-bound and this cuts its traffic by a third) plus the
 * int64 row, so 16 bytes per edge; 24 with int64 keys. */
int64_t ray_csr_par_min_edges = 1 << 20;   /* below this the serial build wins; tests lower it */
#define CSR_PAR_MAX_TASKS  8           /* memory-bound: 4-8 tasks is the knee on a 32-thread box */
#define CSR_PAR_BUCKETS    256

typedef struct {
    const int64_t* keys;      /* NULL: key = row index */
    const int64_t* vals;
    int64_t   n_edges, n_nodes;
    int       T, B, shift;
    bool      narrow;         /* int32 scratch */
    int64_t*  hist;           /* [T * B]: counts, then cursors */
    int64_t*  bstart;         /* [B + 1]: bucket edge ranges in scratch */
    void*     skeys; void* svals;   /* int32 or int64, n_edges each */
    int64_t*  srow;           /* n_edges */
    int64_t*  off;  int64_t* tgt;  int64_t* rmap;   /* final CSR arrays */
    int64_t*  pos;            /* [n_nodes] cursors (per-bucket private) */
} csr_par_t;

static inline int64_t csr_par_key(const csr_par_t* c, int64_t i) {
    return c->keys ? c->keys[i] : i;
}

static void csr_par_task_range(const csr_par_t* c, uint32_t t,
                               int64_t* lo, int64_t* hi) {
    *lo = c->n_edges * (int64_t)t / c->T;
    *hi = c->n_edges * (int64_t)(t + 1) / c->T;
}

/* Phase 1: per-task bucket histogram. */
static void csr_par_hist_fn(void* ctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    csr_par_t* c = (csr_par_t*)ctx;
    uint32_t t = (uint32_t)start;
    int64_t lo, hi; csr_par_task_range(c, t, &lo, &hi);
    int64_t* h = c->hist + (size_t)t * c->B;
    for (int64_t i = lo; i < hi; i++) {
        int64_t k = csr_par_key(c, i);
        if (k >= 0 && k < c->n_nodes) h[k >> c->shift]++;
    }
}

/* Phase 2: per-task scatter into bucket-ordered scratch. */
static void csr_par_scatter_fn(void* ctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    csr_par_t* c = (csr_par_t*)ctx;
    uint32_t t = (uint32_t)start;
    int64_t lo, hi; csr_par_task_range(c, t, &lo, &hi);
    int64_t* cur = c->hist + (size_t)t * c->B;
    if (c->narrow) {
        int32_t* sk = (int32_t*)c->skeys; int32_t* sv = (int32_t*)c->svals;
        for (int64_t i = lo; i < hi; i++) {
            int64_t k = csr_par_key(c, i);
            if (k < 0 || k >= c->n_nodes) continue;
            int64_t p = cur[k >> c->shift]++;
            sk[p] = (int32_t)k; sv[p] = (int32_t)c->vals[i]; c->srow[p] = i;
        }
    } else {
        int64_t* sk = (int64_t*)c->skeys; int64_t* sv = (int64_t*)c->svals;
        for (int64_t i = lo; i < hi; i++) {
            int64_t k = csr_par_key(c, i);
            if (k < 0 || k >= c->n_nodes) continue;
            int64_t p = cur[k >> c->shift]++;
            sk[p] = k; sv[p] = c->vals[i]; c->srow[p] = i;
        }
    }
}

/* Phase 3: one task per bucket — counting sort of that bucket's node
 * range.  off[v] for v in the range and the bucket's edge span of tgt/rmap
 * are written by this task only. */
static void csr_par_bucket_fn(void* ctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; (void)end;
    csr_par_t* c = (csr_par_t*)ctx;
    int b = (int)start;
    int64_t nlo = (int64_t)b << c->shift, nhi = (int64_t)(b + 1) << c->shift;
    if (nhi > c->n_nodes) nhi = c->n_nodes;
    if (nlo >= nhi) return;
    int64_t elo = c->bstart[b], ehi = c->bstart[b + 1];
    for (int64_t v = nlo; v < nhi; v++) c->pos[v] = 0;
    if (c->narrow) {
        const int32_t* sk = (const int32_t*)c->skeys;
        for (int64_t i = elo; i < ehi; i++) c->pos[sk[i]]++;
    } else {
        const int64_t* sk = (const int64_t*)c->skeys;
        for (int64_t i = elo; i < ehi; i++) c->pos[sk[i]]++;
    }
    int64_t run = elo;
    for (int64_t v = nlo; v < nhi; v++) {
        int64_t d = c->pos[v]; c->off[v] = run; c->pos[v] = run; run += d;
    }
    if (c->narrow) {
        const int32_t* sk = (const int32_t*)c->skeys;
        const int32_t* sv = (const int32_t*)c->svals;
        for (int64_t i = elo; i < ehi; i++) {
            int64_t p = c->pos[sk[i]]++;
            c->tgt[p] = sv[i]; c->rmap[p] = c->srow[i];
        }
    } else {
        const int64_t* sk = (const int64_t*)c->skeys;
        const int64_t* sv = (const int64_t*)c->svals;
        for (int64_t i = elo; i < ehi; i++) {
            int64_t p = c->pos[sk[i]]++;
            c->tgt[p] = sv[i]; c->rmap[p] = c->srow[i];
        }
    }
}

/* Count valid edges and fill out->offsets/targets/rowmap in parallel.
 * Returns false (nothing allocated into out) when the pool should not be
 * used or scratch cannot be had; the caller then builds serially. */
static bool csr_build_parallel(const int64_t* keys, const int64_t* vals,
                               int64_t n_edges, int64_t n_nodes,
                               ray_csr_t* out) {
    ray_pool_t* pool = ray_pool_get();
    if (!ray_pool_par_dispatch_ok(pool, n_edges, ray_csr_par_min_edges) ||
        n_nodes < 2)
        return false;
    int T = (int)ray_pool_total_workers(pool);
    if (T > CSR_PAR_MAX_TASKS) T = CSR_PAR_MAX_TASKS;
    int shift = 0;
    while (((n_nodes - 1) >> shift) >= CSR_PAR_BUCKETS) shift++;
    int B = (int)(((n_nodes - 1) >> shift) + 1);
    bool narrow = n_nodes <= INT32_MAX;
    /* Values are stored unchecked (an out-of-range dst stays in fwd); the
     * narrow scratch needs every value to fit as well. */
    if (narrow) {
        for (int64_t i = 0; i < n_edges; i++)
            if (vals[i] < INT32_MIN || vals[i] > INT32_MAX) { narrow = false; break; }
    }
    size_t cell = narrow ? sizeof(int32_t) : sizeof(int64_t);

    csr_par_t c = { .keys = keys, .vals = vals, .n_edges = n_edges,
                    .n_nodes = n_nodes, .T = T, .B = B, .shift = shift,
                    .narrow = narrow };
    c.hist   = ray_calloc_raw((size_t)T * B * sizeof(int64_t));
    c.bstart = ray_alloc_raw((size_t)(B + 1) * sizeof(int64_t));
    c.skeys  = ray_alloc_raw((size_t)n_edges * cell);
    c.svals  = ray_alloc_raw((size_t)n_edges * cell);
    c.srow   = ray_alloc_raw((size_t)n_edges * sizeof(int64_t));
    c.pos    = ray_alloc_raw((size_t)n_nodes * sizeof(int64_t));
    bool ok = c.hist && c.bstart && c.skeys && c.svals && c.srow && c.pos;

    if (ok) {
        ray_pool_dispatch_n(pool, csr_par_hist_fn, &c, (uint32_t)T);
        int64_t run = 0;
        for (int b = 0; b < B; b++) {
            c.bstart[b] = run;
            for (int t = 0; t < T; t++) {
                int64_t* h = c.hist + (size_t)t * B + b;
                int64_t d = *h; *h = run; run += d;
            }
        }
        c.bstart[B] = run;
        int64_t valid_edges = run;
        out->n_edges = valid_edges;

        out->offsets = ray_vec_new(RAY_I64, n_nodes + 1);
        out->targets = ray_vec_new(RAY_I64, valid_edges > 0 ? valid_edges : 1);
        out->rowmap  = ray_vec_new(RAY_I64, valid_edges > 0 ? valid_edges : 1);
        ok = out->offsets && !RAY_IS_ERR(out->offsets) &&
             out->targets && !RAY_IS_ERR(out->targets) &&
             out->rowmap  && !RAY_IS_ERR(out->rowmap);
        if (ok) {
            out->offsets->len = n_nodes + 1;
            out->targets->len = valid_edges;
            out->rowmap->len  = valid_edges;
            c.off  = (int64_t*)ray_data(out->offsets);
            c.tgt  = (int64_t*)ray_data(out->targets);
            c.rmap = (int64_t*)ray_data(out->rowmap);
            c.off[n_nodes] = valid_edges;
            ray_pool_dispatch_n(pool, csr_par_scatter_fn, &c, (uint32_t)T);
            ray_pool_dispatch_n(pool, csr_par_bucket_fn,  &c, (uint32_t)B);
        } else {
            if (out->offsets && !RAY_IS_ERR(out->offsets)) ray_release(out->offsets);
            if (out->targets && !RAY_IS_ERR(out->targets)) ray_release(out->targets);
            if (out->rowmap  && !RAY_IS_ERR(out->rowmap))  ray_release(out->rowmap);
            out->offsets = out->targets = out->rowmap = NULL;
        }
    }
    ray_free_raw(c.hist);  ray_free_raw(c.bstart);
    ray_free_raw(c.skeys); ray_free_raw(c.svals);
    ray_free_raw(c.srow);  ray_free_raw(c.pos);
    return ok;
}

static ray_err_t csr_build_direct(const int64_t* keys, const int64_t* vals,
                                  int64_t n_edges, int64_t n_nodes,
                                  bool sort_targets, ray_csr_t* out) {
    out->n_nodes = n_nodes;
    out->props = NULL;

    if (csr_build_parallel(keys, vals, n_edges, n_nodes, out)) {
        csr_sort_adjacency_lists(out);
        out->sorted = sort_targets;
        return RAY_OK;
    }

    /* Offsets (n_nodes + 1), zeroed: off[k + 1] accumulates the degree of k. */
    out->offsets = ray_vec_new(RAY_I64, n_nodes + 1);
    if (!out->offsets || RAY_IS_ERR(out->offsets)) return RAY_ERR_OOM;
    out->offsets->len = n_nodes + 1;
    int64_t* off = (int64_t*)ray_data(out->offsets);
    memset(off, 0, (size_t)(n_nodes + 1) * sizeof(int64_t));
    int64_t valid_edges = 0;
    for (int64_t i = 0; i < n_edges; i++) {
        int64_t key = keys ? keys[i] : i;
        if (key >= 0 && key < n_nodes) { off[key + 1]++; valid_edges++; }
    }
    out->n_edges = valid_edges;

    for (int64_t i = 1; i <= n_nodes; i++)
        off[i] += off[i - 1];

    out->targets = ray_vec_new(RAY_I64, valid_edges > 0 ? valid_edges : 1);
    if (!out->targets || RAY_IS_ERR(out->targets)) {
        ray_release(out->offsets); out->offsets = NULL;
        return RAY_ERR_OOM;
    }
    out->targets->len = valid_edges;
    int64_t* tgt = (int64_t*)ray_data(out->targets);

    out->rowmap = ray_vec_new(RAY_I64, valid_edges > 0 ? valid_edges : 1);
    if (!out->rowmap || RAY_IS_ERR(out->rowmap)) {
        ray_release(out->offsets); out->offsets = NULL;
        ray_release(out->targets); out->targets = NULL;
        return RAY_ERR_OOM;
    }
    out->rowmap->len = valid_edges;
    int64_t* rmap = (int64_t*)ray_data(out->rowmap);

    /* Scatter: pos[k] is the next free slot of list k. */
    ray_t* pos_hdr = ray_alloc((size_t)(n_nodes > 0 ? n_nodes : 1) * sizeof(int64_t));
    if (!pos_hdr) {
        ray_release(out->offsets); out->offsets = NULL;
        ray_release(out->targets); out->targets = NULL;
        ray_release(out->rowmap); out->rowmap = NULL;
        return RAY_ERR_OOM;
    }
    int64_t* pos = (int64_t*)ray_data(pos_hdr);
    if (n_nodes > 0)
        memcpy(pos, off, (size_t)n_nodes * sizeof(int64_t));

    for (int64_t i = 0; i < n_edges; i++) {
        int64_t key = keys ? keys[i] : i;
        if (key >= 0 && key < n_nodes) {
            int64_t p = pos[key]++;
            tgt[p] = vals[i];
            rmap[p] = i;
        }
    }
    ray_free(pos_hdr);

    csr_sort_adjacency_lists(out);
    out->sorted = sort_targets;
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * ray_rel_from_edges — build from explicit edge table
 * -------------------------------------------------------------------------- */

ray_rel_t* ray_rel_from_edges(ray_t* edge_table,
                             const char* src_col, const char* dst_col,
                             int64_t n_src_nodes, int64_t n_dst_nodes,
                             bool sort_targets) {
    if (!edge_table || RAY_IS_ERR(edge_table) || edge_table->type != RAY_TABLE)
        return NULL;

    int64_t src_sym = ray_sym_intern(src_col, strlen(src_col));
    int64_t dst_sym = ray_sym_intern(dst_col, strlen(dst_col));
    if (src_sym < 0 || dst_sym < 0) return NULL;  /* sym intern OOM */

    ray_t* src_vec = ray_table_get_col(edge_table, src_sym);
    ray_t* dst_vec = ray_table_get_col(edge_table, dst_sym);
    if (!src_vec || !dst_vec) return NULL;
    if (src_vec->type != RAY_I64 || dst_vec->type != RAY_I64) return NULL;

    int64_t n_edges = src_vec->len;
    if (n_edges != dst_vec->len) return NULL;
    if (n_src_nodes < 0 || n_dst_nodes < 0) return NULL;

    const int64_t* src_data = (const int64_t*)ray_data(src_vec);
    const int64_t* dst_data = (const int64_t*)ray_data(dst_vec);

    ray_rel_t* rel = (ray_rel_t*)ray_sys_alloc(sizeof(ray_rel_t));
    if (!rel) return NULL;
    memset(rel, 0, sizeof(ray_rel_t));
    rel->name_sym = -1;

    /* Forward CSR: lists keyed by src, targets are dst. */
    ray_err_t err = csr_build_direct(src_data, dst_data, n_edges, n_src_nodes,
                                     sort_targets, &rel->fwd);
    if (err != RAY_OK) {
        ray_sys_free(rel);
        return NULL;
    }

    /* Reverse CSR: lists keyed by dst, targets are src. */
    err = csr_build_direct(dst_data, src_data, n_edges, n_dst_nodes,
                           sort_targets, &rel->rev);
    if (err != RAY_OK) {
        csr_free(&rel->fwd);
        ray_sys_free(rel);
        return NULL;
    }

    return rel;
}

/* --------------------------------------------------------------------------
 * ray_rel_build — build from FK column in source table
 * -------------------------------------------------------------------------- */

ray_rel_t* ray_rel_build(ray_t* from_table, const char* fk_col,
                         int64_t n_target_nodes, bool sort_targets) {
    if (!from_table || RAY_IS_ERR(from_table) || from_table->type != RAY_TABLE)
        return NULL;

    int64_t fk_sym = ray_sym_intern(fk_col, strlen(fk_col));
    ray_t* fk_vec = ray_table_get_col(from_table, fk_sym);
    if (!fk_vec || fk_vec->type != RAY_I64) return NULL;
    if (n_target_nodes < 0) return NULL;

    int64_t n_edges = fk_vec->len;
    int64_t n_src_nodes = ray_table_nrows(from_table);

    const int64_t* fk_data = (const int64_t*)ray_data(fk_vec);

    ray_rel_t* rel = (ray_rel_t*)ray_sys_alloc(sizeof(ray_rel_t));
    if (!rel) return NULL;
    memset(rel, 0, sizeof(ray_rel_t));
    rel->name_sym = -1;

    /* Forward CSR: row i is node i with the single edge i -> fk[i]. */
    ray_err_t err = csr_build_direct(NULL, fk_data, n_edges, n_src_nodes,
                                     sort_targets, &rel->fwd);
    if (err != RAY_OK) {
        ray_sys_free(rel);
        return NULL;
    }

    /* Reverse CSR: lists keyed by fk value, targets are the row indices. */
    ray_t* rows_hdr = ray_alloc((size_t)(n_edges > 0 ? n_edges : 1) * sizeof(int64_t));
    if (!rows_hdr) {
        csr_free(&rel->fwd);
        ray_sys_free(rel);
        return NULL;
    }
    int64_t* rows = (int64_t*)ray_data(rows_hdr);
    for (int64_t i = 0; i < n_edges; i++) rows[i] = i;
    err = csr_build_direct(fk_data, rows, n_edges, n_target_nodes,
                           sort_targets, &rel->rev);
    ray_free(rows_hdr);
    if (err != RAY_OK) {
        csr_free(&rel->fwd);
        ray_sys_free(rel);
        return NULL;
    }

    return rel;
}

/* --------------------------------------------------------------------------
 * CSR free
 * -------------------------------------------------------------------------- */

static void csr_free(ray_csr_t* csr) {
    if (csr->offsets) ray_release(csr->offsets);
    if (csr->targets) ray_release(csr->targets);
    if (csr->rowmap) ray_release(csr->rowmap);
    if (csr->props) ray_release(csr->props);
    csr->offsets = NULL;
    csr->targets = NULL;
    csr->rowmap = NULL;
    csr->props = NULL;
}

void ray_rel_set_props(ray_rel_t* rel, ray_t* props) {
    if (!rel || !props) return;
    /* Retain twice: fwd.props and rev.props both alias the same pointer,
     * and csr_free() releases each independently. */
    ray_retain(props);
    ray_retain(props);
    if (rel->fwd.props) ray_release(rel->fwd.props);
    if (rel->rev.props) ray_release(rel->rev.props);
    rel->fwd.props = props;
    rel->rev.props = props;
}

void ray_rel_free(ray_rel_t* rel) {
    if (!rel) return;
    csr_free(&rel->fwd);
    csr_free(&rel->rev);
    ray_sys_free(rel);
}

/* --- Public CSR neighbor access ------------------------------------------- */

const int64_t* ray_rel_neighbors(ray_rel_t* rel, int64_t node,
                                 uint8_t direction, int64_t* out_count) {
    if (!rel) { if (out_count) *out_count = 0; return NULL; }
    ray_csr_t* csr = (direction == 1) ? &rel->rev : &rel->fwd;
    return ray_csr_neighbors(csr, node, out_count);
}

int64_t ray_rel_n_nodes(ray_rel_t* rel, uint8_t direction) {
    if (!rel) return 0;
    ray_csr_t* csr = (direction == 1) ? &rel->rev : &rel->fwd;
    return csr->n_nodes;
}

/* --------------------------------------------------------------------------
 * CSR persistence — save/load/mmap using existing column file format
 * -------------------------------------------------------------------------- */


static ray_err_t csr_save(ray_csr_t* csr, const char* dir, const char* prefix) {
    char path[1024];
    int len;

    len = snprintf(path, sizeof(path), "%s/%s_offsets", dir, prefix);
    if (len < 0 || (size_t)len >= sizeof(path)) return RAY_ERR_IO;
    ray_err_t err = ray_col_save(csr->offsets, path);
    if (err != RAY_OK) return err;

    len = snprintf(path, sizeof(path), "%s/%s_targets", dir, prefix);
    if (len < 0 || (size_t)len >= sizeof(path)) return RAY_ERR_IO;
    err = ray_col_save(csr->targets, path);
    if (err != RAY_OK) return err;

    if (csr->rowmap) {
        len = snprintf(path, sizeof(path), "%s/%s_rowmap", dir, prefix);
        if (len < 0 || (size_t)len >= sizeof(path)) return RAY_ERR_IO;
        err = ray_col_save(csr->rowmap, path);
        if (err != RAY_OK) return err;
    }

    return RAY_OK;
}

static ray_err_t csr_load_impl(ray_csr_t* csr, const char* dir, const char* prefix,
                                bool use_mmap) {
    char path[1024];
    int len;

    len = snprintf(path, sizeof(path), "%s/%s_offsets", dir, prefix);
    if (len < 0 || (size_t)len >= sizeof(path)) return RAY_ERR_IO;
    csr->offsets = use_mmap ? ray_col_mmap(path) : ray_col_load(path);
    if (!csr->offsets || RAY_IS_ERR(csr->offsets)) {
        csr->offsets = NULL;
        return RAY_ERR_IO;
    }

    len = snprintf(path, sizeof(path), "%s/%s_targets", dir, prefix);
    if (len < 0 || (size_t)len >= sizeof(path)) return RAY_ERR_IO;
    csr->targets = use_mmap ? ray_col_mmap(path) : ray_col_load(path);
    if (!csr->targets || RAY_IS_ERR(csr->targets)) {
        ray_release(csr->offsets); csr->offsets = NULL;
        csr->targets = NULL;
        return RAY_ERR_IO;
    }

    len = snprintf(path, sizeof(path), "%s/%s_rowmap", dir, prefix);
    if (len < 0 || (size_t)len >= sizeof(path)) return RAY_ERR_IO;
    csr->rowmap = use_mmap ? ray_col_mmap(path) : ray_col_load(path);
    if (!csr->rowmap || RAY_IS_ERR(csr->rowmap)) {
        /* rowmap is optional — ignore error */
        csr->rowmap = NULL;
    }

    if (csr->offsets->len < 1) {
        ray_release(csr->offsets); csr->offsets = NULL;
        ray_release(csr->targets); csr->targets = NULL;
        if (csr->rowmap) { ray_release(csr->rowmap); csr->rowmap = NULL; }
        return RAY_ERR_IO;
    }
    csr->n_nodes = csr->offsets->len - 1;
    csr->n_edges = csr->targets->len;

    /* Consistency: offsets[n_nodes] must equal targets->len */
    int64_t* off_data = (int64_t*)ray_data(csr->offsets);
    if (off_data[csr->n_nodes] != csr->n_edges) {
        ray_release(csr->offsets); csr->offsets = NULL;
        ray_release(csr->targets); csr->targets = NULL;
        if (csr->rowmap) { ray_release(csr->rowmap); csr->rowmap = NULL; }
        return RAY_ERR_IO;
    }

    /* Validate offset monotonicity: offsets[i] <= offsets[i+1] */
    for (int64_t i = 0; i < csr->n_nodes; i++) {
        if (off_data[i] < 0 || off_data[i] > off_data[i + 1]) {
            ray_release(csr->offsets); csr->offsets = NULL;
            ray_release(csr->targets); csr->targets = NULL;
            if (csr->rowmap) { ray_release(csr->rowmap); csr->rowmap = NULL; }
            return RAY_ERR_IO;  /* corrupt: non-monotonic offsets */
        }
    }

    csr->sorted = false;  /* caller sets if known */
    csr->props = NULL;

    return RAY_OK;
}

ray_err_t ray_rel_save(ray_rel_t* rel, const char* dir) {
    if (!rel || !dir) return RAY_ERR_IO;

    /* Create directory */
    if (ray_mkdir(dir) != RAY_OK) return RAY_ERR_IO;

    ray_err_t err = csr_save(&rel->fwd, dir, "fwd");
    if (err != RAY_OK) return err;

    err = csr_save(&rel->rev, dir, "rev");
    if (err != RAY_OK) return err;

    /* Save metadata (from_table, to_table, name_sym, sorted flags) */
    char path[1024];
    int len = snprintf(path, sizeof(path), "%s/meta", dir);
    if (len < 0 || (size_t)len >= sizeof(path)) return RAY_ERR_IO;

    /* Pack metadata into an I64 vector: [from_table, to_table, name_sym, fwd_sorted, rev_sorted] */
    int64_t meta_data[5];
    meta_data[0] = (int64_t)rel->from_table;
    meta_data[1] = (int64_t)rel->to_table;
    meta_data[2] = rel->name_sym;
    meta_data[3] = rel->fwd.sorted ? 1 : 0;
    meta_data[4] = rel->rev.sorted ? 1 : 0;
    ray_t* meta_vec = ray_vec_from_raw(RAY_I64, meta_data, 5);
    if (!meta_vec || RAY_IS_ERR(meta_vec)) return RAY_ERR_OOM;
    err = ray_col_save(meta_vec, path);
    ray_release(meta_vec);

    return err;
}

static ray_rel_t* rel_load_impl(const char* dir, bool use_mmap) {
    if (!dir) return NULL;

    ray_rel_t* rel = (ray_rel_t*)ray_sys_alloc(sizeof(ray_rel_t));
    if (!rel) return NULL;
    memset(rel, 0, sizeof(ray_rel_t));

    ray_err_t err = csr_load_impl(&rel->fwd, dir, "fwd", use_mmap);
    if (err != RAY_OK) { ray_sys_free(rel); return NULL; }

    err = csr_load_impl(&rel->rev, dir, "rev", use_mmap);
    if (err != RAY_OK) {
        csr_free(&rel->fwd);
        ray_sys_free(rel);
        return NULL;
    }

    /* Load metadata */
    char path[1024];
    int len = snprintf(path, sizeof(path), "%s/meta", dir);
    if (len >= 0 && (size_t)len < sizeof(path)) {
        ray_t* meta = use_mmap ? ray_col_mmap(path) : ray_col_load(path);
        if (meta && !RAY_IS_ERR(meta) && meta->len >= 5) {
            int64_t* md = (int64_t*)ray_data(meta);
            rel->from_table = (uint16_t)md[0];
            rel->to_table = (uint16_t)md[1];
            rel->name_sym = md[2];
            rel->fwd.sorted = md[3] != 0;
            rel->rev.sorted = md[4] != 0;
            ray_release(meta);
        } else if (meta && !RAY_IS_ERR(meta)) {
            ray_release(meta);
        }
    }

    return rel;
}

ray_rel_t* ray_rel_load(const char* dir) {
    return rel_load_impl(dir, false);
}

ray_rel_t* ray_rel_mmap(const char* dir) {
    return rel_load_impl(dir, true);
}
