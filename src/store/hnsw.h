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

#ifndef RAY_HNSW_H
#define RAY_HNSW_H

#include <rayforce.h>

/* ---------- HNSW Index ----------
 *
 * Multi-layer proximity graph for approximate nearest neighbor search.
 *
 * Memory layout per node:
 *   - Layer 0: up to M_max0 neighbors (default 2*M)
 *   - Layers 1+: up to M neighbors each
 *
 * Neighbor lists stored as flat arrays:
 *   neighbors[node * M_max + i] = neighbor_id  (or -1 if unused)
 *
 * Each layer stores its own neighbor array for all nodes at that layer.
 */

#define HNSW_MAX_LAYERS    16
#define HNSW_DEFAULT_M     16
#define HNSW_DEFAULT_EF_C  200
#define HNSW_DEFAULT_EF_S  50

/* Distance metric driving beam search.  HNSW requires lower-is-closer;
 * we choose the encoding so each metric sorts ascending:
 *   COSINE → 1 - cos(a, b)     range [0, 2]
 *   L2     → sqrt(sum(sq diff)) range [0, ∞)
 *   IP     → -dot(a, b)         range (-∞, ∞)   (negated so lower=closer) */
typedef enum {
    RAY_HNSW_COSINE = 0,
    RAY_HNSW_L2     = 1,
    RAY_HNSW_IP     = 2
} ray_hnsw_metric_t;

typedef struct ray_hnsw_layer {
    int64_t*  neighbors;     /* flat array: n_nodes_in_layer * M_max entries */
    int64_t   n_nodes;       /* number of nodes in this layer */
    int64_t   M_max;         /* max neighbors per node in this layer */
    int64_t*  node_ids;      /* mapping: layer_idx -> global node id */
} ray_hnsw_layer_t;

typedef struct ray_hnsw {
    int64_t          n_nodes;         /* total number of vectors */
    int32_t          dim;             /* embedding dimension */
    int32_t          n_layers;        /* number of layers (including layer 0) */
    int32_t          M;               /* max neighbors per node (layers 1+) */
    int32_t          M_max0;          /* max neighbors per node (layer 0) */
    int32_t          ef_construction;  /* beam width during construction */
    int32_t          metric;          /* ray_hnsw_metric_t */
    int64_t          entry_point;     /* entry point node (highest layer) */
    int8_t*          node_level;      /* max layer for each node (n_nodes entries) */
    ray_hnsw_layer_t  layers[HNSW_MAX_LAYERS];
    const float*     vectors;         /* pointer to embedding data (not owned) */
    bool             owns_data;       /* true if loaded from disk (owns neighbor arrays etc.) */
} ray_hnsw_t;

/* --- Build / Free / Clone --- */
ray_hnsw_t* ray_hnsw_build(const float* vectors, int64_t n_nodes, int32_t dim,
                           ray_hnsw_metric_t metric,
                           int32_t M, int32_t ef_construction);
void ray_hnsw_free(ray_hnsw_t* idx);
/* Deep-copy an index: duplicates vectors, node levels, and every layer's
 * neighbor + node_id arrays.  Returns a new fully-owned index with the
 * same semantics as the source.  Returns NULL on OOM. */
ray_hnsw_t* ray_hnsw_clone(const ray_hnsw_t* src);

/* --- Search --- */
/* Returns top-K nearest neighbors as (node_id, distance) pairs.
 * out_ids and out_dists must be pre-allocated with k entries.
 *
 * Return value:
 *   >= 0 : number of results written (may be < k).
 *   -1   : allocation failure (OOM) — callers must surface a distinct
 *          error rather than treat the 0-return as "no matches".
 */
int64_t ray_hnsw_search(const ray_hnsw_t* idx,
                         const float* query, int32_t dim,
                         int64_t k, int32_t ef_search,
                         int64_t* out_ids, double* out_dists);

/* Predicate callback used by the filtered iterative-scan variant below.
 * Return true to accept `node_id` into the result set, false to reject.
 * Rejected nodes still participate in candidate-graph exploration so
 * connectivity through them is preserved — this is the standard
 * "iterative scan" shape. */
typedef bool (*ray_hnsw_accept_fn)(int64_t node_id, void* ctx);

/* Like ray_hnsw_search, but only nodes passing `accept(node_id, ctx)`
 * enter the top-K result set.  Candidate-queue expansion still traverses
 * rejected nodes so their accepted descendants remain reachable.
 * Falls back to exhaustive graph exploration for pathologically selective
 * filters (bounded by idx->n_nodes).
 *
 * Return value matches ray_hnsw_search: >= 0 = result count, -1 = OOM. */
int64_t ray_hnsw_search_filter(const ray_hnsw_t* idx,
                               const float* query, int32_t dim,
                               int64_t k, int32_t ef_search,
                               ray_hnsw_accept_fn accept, void* ctx,
                               int64_t* out_ids, double* out_dists);

/* --- Accessors --- */
int32_t ray_hnsw_dim(const ray_hnsw_t* idx);

/* --- Persistence --- */
ray_err_t ray_hnsw_save(const ray_hnsw_t* idx, const char* dir);
ray_hnsw_t* ray_hnsw_load(const char* dir);
ray_hnsw_t* ray_hnsw_mmap(const char* dir);

#endif /* RAY_HNSW_H */
