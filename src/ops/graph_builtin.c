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

/* ==========================================================================
 *  Rayfall builtins for the graph algorithm family (.graph.*).
 *
 *  Mirrors the HNSW pattern in embedding.c: the graph itself is an opaque
 *  ray_rel_t* wrapped inside a -RAY_I64 atom tagged with RAY_ATTR_GRAPH.
 *  Lifecycle:
 *    (.graph.build T 'src 'dst [ 'weight ])  → handle
 *    (.graph.info  h)                         → dict
 *    (.graph.free  h)                         → null  (idempotent)
 *
 *  Algorithm wrappers all share the same micro-skeleton:
 *    1. Validate the graph handle.
 *    2. Validate the remaining arguments.
 *    3. Build a one-node DAG, dispatch via ray_execute, return the table.
 *
 *  No libc malloc/free here — all scratch allocation goes through
 *  ray_alloc / ray_sys_alloc (CSR uses ray_sys_alloc internally).
 * ========================================================================== */

#include "ops/internal.h"
#include "lang/internal.h"

/* --------------------------------------------------------------------------
 * Handle wrap / unwrap
 * -------------------------------------------------------------------------- */

static ray_rel_t* graph_unwrap(ray_t* h) {
    if (!h) return NULL;
    if (h->type != -RAY_I64) return NULL;
    if (!(h->attrs & RAY_ATTR_GRAPH)) return NULL;
    return (ray_rel_t*)(uintptr_t)h->i64;
}

static ray_t* graph_wrap(ray_rel_t* rel) {
    ray_t* h = ray_alloc(0);
    if (!h || RAY_IS_ERR(h)) return h ? h : ray_error("oom", NULL);
    h->type   = -RAY_I64;
    h->attrs |= RAY_ATTR_GRAPH;
    h->i64    = (int64_t)(uintptr_t)rel;
    return h;
}

/* --------------------------------------------------------------------------
 * Argument helpers
 * -------------------------------------------------------------------------- */

static bool atom_is_int(ray_t* a) {
    return a && (a->type == -RAY_I64 || a->type == -RAY_I32 || a->type == -RAY_I16);
}

static int64_t atom_to_i64(ray_t* a) {
    if (!a) return 0;
    switch (a->type) {
        case -RAY_I64: return a->i64;
        case -RAY_I32: return (int64_t)a->i32;
        case -RAY_I16: return (int64_t)a->i16;
        default:       return 0;
    }
}

/* Resolve a sym-or-string argument to an interned sym ID.  Used by the
 * column-name slots of (.graph.build) — accept either 'name (already an
 * interned RAY_SYM atom) or "name" (turn into an intern). */
static int64_t arg_to_sym(ray_t* a) {
    if (!a) return -1;
    if (a->type == -RAY_SYM) return a->i64;
    if (a->type == -RAY_STR) {
        const char* p = ray_str_ptr(a);
        size_t len = ray_str_len(a);
        if (!p || len == 0) return -1;
        return ray_sym_intern(p, len);
    }
    return -1;
}

/* Coerce an integer column to RAY_I64 by allocating a fresh I64 vec.
 * For RAY_I64 input, just retain and return.  For RAY_I32/I16/U8/SYM,
 * widen.  Caller releases the result. */
static ray_t* widen_to_i64(ray_t* col) {
    if (!col || !ray_is_vec(col)) return NULL;
    if (col->type == RAY_I64) {
        ray_retain(col);
        return col;
    }
    int64_t n = col->len;
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (!out || RAY_IS_ERR(out)) return NULL;
    out->len = n;
    int64_t* dst = (int64_t*)ray_data(out);
    void* src = ray_data(col);
    switch (col->type) {
        case RAY_I32:
            for (int64_t i = 0; i < n; i++) dst[i] = (int64_t)((int32_t*)src)[i];
            break;
        case RAY_I16:
            for (int64_t i = 0; i < n; i++) dst[i] = (int64_t)((int16_t*)src)[i];
            break;
        case RAY_U8:
        case RAY_BOOL:
            for (int64_t i = 0; i < n; i++) dst[i] = (int64_t)((uint8_t*)src)[i];
            break;
        case RAY_SYM:
            for (int64_t i = 0; i < n; i++)
                dst[i] = read_col_i64(src, i, RAY_SYM, col->attrs);
            break;
        default:
            ray_release(out);
            return NULL;
    }
    return out;
}

/* Compute n_nodes for a relationship by scanning a widened I64 column.
 * Returns max(col[i]) + 1, or 0 if the column is empty. */
static int64_t max_plus_one(ray_t* col_i64) {
    int64_t n = col_i64->len;
    if (n <= 0) return 0;
    int64_t* d = (int64_t*)ray_data(col_i64);
    int64_t mx = d[0];
    for (int64_t i = 1; i < n; i++) if (d[i] > mx) mx = d[i];
    return mx >= 0 ? mx + 1 : 0;
}

/* --------------------------------------------------------------------------
 * (.graph.build T 'src 'dst [ 'weight ]) → graph handle
 *
 * Builds a CSR over the rows of T.  src/dst columns must be integer- or
 * sym-typed; if either is not RAY_I64 it is widened into a temporary I64
 * vector for ray_rel_from_edges.  When a weight column is supplied, it
 * must be RAY_F64 and is exposed via ray_rel_set_props for algorithms
 * that consume edge weights (dijkstra, mst, k-shortest, astar).
 * -------------------------------------------------------------------------- */
ray_t* ray_graph_build_fn(ray_t** args, int64_t n) {
    if (n < 3 || n > 4) return ray_error("rank", NULL);
    ray_t* tbl = args[0];
    if (!tbl || tbl->type != RAY_TABLE) return ray_error("type", NULL);

    int64_t src_sym = arg_to_sym(args[1]);
    int64_t dst_sym = arg_to_sym(args[2]);
    if (src_sym < 0 || dst_sym < 0) return ray_error("type", NULL);

    int64_t weight_sym = -1;
    if (n == 4) {
        weight_sym = arg_to_sym(args[3]);
        if (weight_sym < 0) return ray_error("type", NULL);
    }

    ray_t* src_col = ray_table_get_col(tbl, src_sym);
    ray_t* dst_col = ray_table_get_col(tbl, dst_sym);
    if (!src_col || !dst_col) return ray_error("name", NULL);
    if (!ray_is_vec(src_col) || !ray_is_vec(dst_col)) return ray_error("type", NULL);
    if (src_col->len != dst_col->len) return ray_error("length", NULL);

    /* Widen src/dst into RAY_I64 if needed.  ray_rel_from_edges insists
     * on I64 columns directly. */
    ray_t* src_i64 = widen_to_i64(src_col);
    if (!src_i64) return ray_error("type", NULL);
    ray_t* dst_i64 = widen_to_i64(dst_col);
    if (!dst_i64) { ray_release(src_i64); return ray_error("type", NULL); }

    /* Determine n_nodes from the larger of (max(src), max(dst)) + 1.  This
     * lets the user omit a node-count argument; bigger graphs that need a
     * specific node universe can post-process by reusing fk-build. */
    int64_t n_src = max_plus_one(src_i64);
    int64_t n_dst = max_plus_one(dst_i64);
    int64_t n_nodes = n_src > n_dst ? n_src : n_dst;

    /* Build a temporary edge table that holds the widened columns under the
     * sym IDs that ray_rel_from_edges expects ("src", "dst" — names are
     * looked up by these strings).  The original table's column names are
     * arbitrary; we always re-name internally. */
    int64_t s_sym = sym_intern_safe("src", 3);
    int64_t d_sym = sym_intern_safe("dst", 3);
    ray_t* edges = ray_table_new(2);
    if (!edges || RAY_IS_ERR(edges)) {
        ray_release(src_i64); ray_release(dst_i64);
        return ray_error("oom", NULL);
    }
    edges = ray_table_add_col(edges, s_sym, src_i64);
    ray_release(src_i64);
    edges = ray_table_add_col(edges, d_sym, dst_i64);
    ray_release(dst_i64);
    if (!edges || RAY_IS_ERR(edges)) return ray_error("oom", NULL);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst",
                                         n_nodes, n_nodes, true);
    if (!rel) {
        ray_release(edges);
        return ray_error("oom", NULL);
    }

    /* Optional: attach a weight column.  Algorithms that need edge weights
     * (dijkstra, mst, …) look the column up by sym ID via rel->fwd.props.
     * The column is referenced by its original name in the user's table. */
    if (weight_sym >= 0) {
        ray_t* w_col = ray_table_get_col(tbl, weight_sym);
        if (!w_col || !ray_is_vec(w_col)) {
            ray_rel_free(rel); ray_release(edges);
            return ray_error("name", NULL);
        }
        if (w_col->len != src_col->len) {
            ray_rel_free(rel); ray_release(edges);
            return ray_error("length", NULL);
        }
        if (w_col->type != RAY_F64 && w_col->type != RAY_I64 &&
            w_col->type != RAY_I32 && w_col->type != RAY_F32) {
            ray_rel_free(rel); ray_release(edges);
            return ray_error("type", NULL);
        }

        /* Coerce weights to RAY_F64 (the algorithms expect F64). */
        ray_t* w_f64 = NULL;
        if (w_col->type == RAY_F64) {
            ray_retain(w_col);
            w_f64 = w_col;
        } else {
            int64_t nrow = w_col->len;
            w_f64 = ray_vec_new(RAY_F64, nrow > 0 ? nrow : 1);
            if (!w_f64 || RAY_IS_ERR(w_f64)) {
                ray_rel_free(rel); ray_release(edges);
                return ray_error("oom", NULL);
            }
            w_f64->len = nrow;
            double* dst = (double*)ray_data(w_f64);
            void*   src = ray_data(w_col);
            switch (w_col->type) {
                case RAY_I64:
                    for (int64_t i = 0; i < nrow; i++) dst[i] = (double)((int64_t*)src)[i];
                    break;
                case RAY_I32:
                    for (int64_t i = 0; i < nrow; i++) dst[i] = (double)((int32_t*)src)[i];
                    break;
                case RAY_F32:
                    for (int64_t i = 0; i < nrow; i++) dst[i] = (double)((float*)src)[i];
                    break;
                default: break;
            }
        }

        /* Wrap as a one-column "weight" props table — the algorithms read
         * the column by the sym name baked into op->graph.weight_col_sym. */
        int64_t w_sym_local = weight_sym;
        ray_t* props = ray_table_new(1);
        if (!props || RAY_IS_ERR(props)) {
            ray_release(w_f64);
            ray_rel_free(rel); ray_release(edges);
            return ray_error("oom", NULL);
        }
        props = ray_table_add_col(props, w_sym_local, w_f64);
        ray_release(w_f64);
        if (!props || RAY_IS_ERR(props)) {
            ray_rel_free(rel); ray_release(edges);
            return ray_error("oom", NULL);
        }
        ray_rel_set_props(rel, props);
        ray_release(props);
    }

    ray_release(edges);

    ray_t* h = graph_wrap(rel);
    if (!h || RAY_IS_ERR(h)) { ray_rel_free(rel); return h; }
    return h;
}

/* (.graph.free h) → null.  Idempotent: clears the ATTR bit so a second
 * call returns "type" instead of double-freeing. */
ray_t* ray_graph_free_fn(ray_t* h) {
    ray_rel_t* rel = graph_unwrap(h);
    if (!rel) return ray_error("type", NULL);
    ray_rel_free(rel);
    h->i64 = 0;
    h->attrs &= ~RAY_ATTR_GRAPH;
    return RAY_NULL_OBJ;
}

/* (.graph.info h) → dict { n_nodes, n_edges, sorted, has_weights }.
 * Mirrors hnsw-info — keys are interned syms with no hyphens so the
 * 'foo lookup syntax works. */
ray_t* ray_graph_info_fn(ray_t* h) {
    ray_rel_t* rel = graph_unwrap(h);
    if (!rel) return ray_error("type", NULL);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 4);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(4);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    int64_t k_nodes  = sym_intern_safe("n_nodes",  7);
    int64_t k_edges  = sym_intern_safe("n_edges",  7);
    int64_t k_sorted = sym_intern_safe("sorted",   6);
    int64_t k_wts    = sym_intern_safe("has_weights", 11);

    ray_t* v_nodes  = make_i64(rel->fwd.n_nodes);
    ray_t* v_edges  = make_i64(rel->fwd.n_edges);
    ray_t* v_sorted = make_bool(rel->fwd.sorted ? 1 : 0);
    ray_t* v_wts    = make_bool(rel->fwd.props != NULL);

    keys = ray_vec_append(keys, &k_nodes);
    vals = ray_list_append(vals, v_nodes); ray_release(v_nodes);
    keys = ray_vec_append(keys, &k_edges);
    vals = ray_list_append(vals, v_edges); ray_release(v_edges);
    keys = ray_vec_append(keys, &k_sorted);
    vals = ray_list_append(vals, v_sorted); ray_release(v_sorted);
    keys = ray_vec_append(keys, &k_wts);
    vals = ray_list_append(vals, v_wts); ray_release(v_wts);

    return ray_dict_new(keys, vals);
}

/* --------------------------------------------------------------------------
 * Algorithm wrappers — every one of them follows the same template:
 *   1. Unwrap the graph handle (and validate).
 *   2. Pull optional/positional algorithm parameters off args[].
 *   3. ray_graph_new(NULL) → builder → ray_execute → ray_graph_free.
 *   4. Return the resulting table (or error verbatim).
 * -------------------------------------------------------------------------- */

/* Helper: retrieve weight-column sym ID from rel->fwd.props.  Algorithms
 * like dijkstra/mst need a const char* but op-builders take it through
 * ray_sym_intern; we just look up the first column name. */
static const char* rel_weight_col_name(ray_rel_t* rel) {
    if (!rel || !rel->fwd.props) return NULL;
    int64_t name_id = ray_table_col_name(rel->fwd.props, 0);
    if (name_id < 0) return NULL;
    ray_t* s = ray_sym_str(name_id);
    if (!s) return NULL;
    /* ray_sym_str returns an arena-owned string view — its data outlives
     * the call (the symbol table is process-lived).  Safe to alias. */
    const char* p = ray_str_ptr(s);
    ray_release(s);
    return p;
}

/* (.graph.pagerank h [iter] [damping]) → table {_node, _rank} */
ray_t* ray_graph_pagerank_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 3) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);

    uint16_t iters = 30;
    double damping = 0.85;
    if (n >= 2) {
        if (!atom_is_int(args[1])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[1]);
        if (v <= 0 || v > 65535) return ray_error("domain", NULL);
        iters = (uint16_t)v;
    }
    if (n >= 3) {
        if (args[2]->type != -RAY_F64 && !atom_is_int(args[2]))
            return ray_error("type", NULL);
        damping = (args[2]->type == -RAY_F64) ? args[2]->f64
                                              : (double)atom_to_i64(args[2]);
        if (damping <= 0.0 || damping >= 1.0) return ray_error("domain", NULL);
    }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* op = ray_pagerank(g, rel, iters, damping);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.connected h) → table {_node, _component} */
ray_t* ray_graph_connected_fn(ray_t** args, int64_t n) {
    if (n != 1) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* op = ray_connected_comp(g, rel);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.dijkstra h src [dst] [max-depth]) → table {_node, _dist, _depth} */
ray_t* ray_graph_dijkstra_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 4) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);
    if (!atom_is_int(args[1])) return ray_error("type", NULL);
    int64_t src_id = atom_to_i64(args[1]);

    int64_t dst_id = -1;
    if (n >= 3 && args[2]) {
        /* nil-or-int permitted for dst; nil => single-source mode. */
        if (RAY_IS_NULL(args[2])) {
            dst_id = -1;
        } else if (atom_is_int(args[2])) {
            dst_id = atom_to_i64(args[2]);
        } else {
            return ray_error("type", NULL);
        }
    }
    uint8_t max_depth = 255;
    if (n >= 4) {
        if (!atom_is_int(args[3])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[3]);
        if (v <= 0 || v > 255) return ray_error("domain", NULL);
        max_depth = (uint8_t)v;
    }

    const char* w_col = rel_weight_col_name(rel);
    if (!w_col) return ray_error("schema", "graph has no weight column");

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* src_op = ray_const_i64(g, src_id);
    ray_op_t* dst_op = (dst_id >= 0) ? ray_const_i64(g, dst_id) : NULL;
    if (!src_op || (dst_id >= 0 && !dst_op)) {
        ray_graph_free(g); return ray_error("oom", NULL);
    }
    ray_op_t* op = ray_dijkstra(g, src_op, dst_op, rel, w_col, max_depth);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.louvain h [max-iter]) → table {_node, _community} */
ray_t* ray_graph_louvain_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 2) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);
    uint16_t iters = 100;
    if (n == 2) {
        if (!atom_is_int(args[1])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[1]);
        if (v <= 0 || v > 65535) return ray_error("domain", NULL);
        iters = (uint16_t)v;
    }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* op = ray_louvain(g, rel, iters);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.degree h) → table {_node, _in_degree, _out_degree, _degree} */
ray_t* ray_graph_degree_fn(ray_t** args, int64_t n) {
    if (n != 1) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* op = ray_degree_cent(g, rel);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.topsort h) → table {_node, _order} */
ray_t* ray_graph_topsort_fn(ray_t** args, int64_t n) {
    if (n != 1) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* op = ray_topsort(g, rel);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.dfs h src [max-depth]) → table {_node, _depth, _parent} */
ray_t* ray_graph_dfs_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 3) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);
    if (!atom_is_int(args[1])) return ray_error("type", NULL);
    int64_t src_id = atom_to_i64(args[1]);
    uint8_t max_depth = 255;
    if (n == 3) {
        if (!atom_is_int(args[2])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[2]);
        if (v < 0 || v > 255) return ray_error("domain", NULL);
        max_depth = (uint8_t)v;
    }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* src_op = ray_const_i64(g, src_id);
    if (!src_op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_op_t* op = ray_dfs(g, src_op, rel, max_depth);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.cluster h) → table {_node, _coefficient} */
ray_t* ray_graph_cluster_fn(ray_t** args, int64_t n) {
    if (n != 1) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* op = ray_cluster_coeff(g, rel);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.betweenness h [sample]) → table {_node, _centrality} */
ray_t* ray_graph_betweenness_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 2) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);
    uint16_t sample = 0;  /* 0 = exact */
    if (n == 2) {
        if (!atom_is_int(args[1])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[1]);
        if (v < 0 || v > 65535) return ray_error("domain", NULL);
        sample = (uint16_t)v;
    }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* op = ray_betweenness(g, rel, sample);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.closeness h [sample]) → table {_node, _centrality} */
ray_t* ray_graph_closeness_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 2) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);
    uint16_t sample = 0;
    if (n == 2) {
        if (!atom_is_int(args[1])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[1]);
        if (v < 0 || v > 65535) return ray_error("domain", NULL);
        sample = (uint16_t)v;
    }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* op = ray_closeness(g, rel, sample);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.mst h) → table {_src, _dst, _weight} */
ray_t* ray_graph_mst_fn(ray_t** args, int64_t n) {
    if (n != 1) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);
    const char* w_col = rel_weight_col_name(rel);
    if (!w_col) return ray_error("schema", "graph has no weight column");

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* op = ray_mst(g, rel, w_col);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.random-walk h src [walk-len]) → table {_step, _node} */
ray_t* ray_graph_random_walk_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 3) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);
    if (!atom_is_int(args[1])) return ray_error("type", NULL);
    int64_t src_id = atom_to_i64(args[1]);
    uint16_t walk_len = 10;
    if (n == 3) {
        if (!atom_is_int(args[2])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[2]);
        if (v <= 0 || v > 65535) return ray_error("domain", NULL);
        walk_len = (uint16_t)v;
    }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* src_op = ray_const_i64(g, src_id);
    if (!src_op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_op_t* op = ray_random_walk(g, src_op, rel, walk_len);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.k-shortest h src dst k) → table {_path_id, _node, _dist} */
ray_t* ray_graph_k_shortest_fn(ray_t** args, int64_t n) {
    if (n != 4) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);
    if (!atom_is_int(args[1]) || !atom_is_int(args[2]) || !atom_is_int(args[3]))
        return ray_error("type", NULL);
    int64_t src_id = atom_to_i64(args[1]);
    int64_t dst_id = atom_to_i64(args[2]);
    int64_t k_v    = atom_to_i64(args[3]);
    if (k_v <= 0 || k_v > 65535) return ray_error("domain", NULL);
    const char* w_col = rel_weight_col_name(rel);
    if (!w_col) return ray_error("schema", "graph has no weight column");

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* src_op = ray_const_i64(g, src_id);
    ray_op_t* dst_op = ray_const_i64(g, dst_id);
    if (!src_op || !dst_op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_op_t* op = ray_k_shortest(g, src_op, dst_op, rel, w_col, (uint16_t)k_v);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.shortest-path h src dst [max-depth]) → BFS-shortest path table.
 *
 * The underlying ray_shortest_path consumes its src/dst operands as op
 * inputs; we materialise them via ray_const_i64 the same way the other
 * src/dst wrappers do. */
ray_t* ray_graph_shortest_path_fn(ray_t** args, int64_t n) {
    if (n < 3 || n > 4) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);
    if (!atom_is_int(args[1]) || !atom_is_int(args[2])) return ray_error("type", NULL);
    int64_t src_id = atom_to_i64(args[1]);
    int64_t dst_id = atom_to_i64(args[2]);
    uint8_t max_depth = 255;
    if (n == 4) {
        if (!atom_is_int(args[3])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[3]);
        if (v <= 0 || v > 255) return ray_error("domain", NULL);
        max_depth = (uint8_t)v;
    }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* src_op = ray_const_i64(g, src_id);
    ray_op_t* dst_op = ray_const_i64(g, dst_id);
    if (!src_op || !dst_op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_op_t* op = ray_shortest_path(g, src_op, dst_op, rel, max_depth);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.expand h src [direction]) → table {_src, _dst}.
 * direction: 0=forward (default), 1=reverse, 2=both. */
ray_t* ray_graph_expand_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 3) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);
    if (!atom_is_int(args[1])) return ray_error("type", NULL);
    int64_t src_id = atom_to_i64(args[1]);
    uint8_t direction = 0;
    if (n == 3) {
        if (!atom_is_int(args[2])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[2]);
        if (v < 0 || v > 2) return ray_error("domain", NULL);
        direction = (uint8_t)v;
    }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    /* OP_EXPAND consumes a src-nodes vector input.  Pack the single seed
     * into a 1-element vec so the execution path matches multi-seed use. */
    ray_t* seed_vec = ray_vec_from_raw(RAY_I64, &src_id, 1);
    if (!seed_vec || RAY_IS_ERR(seed_vec)) {
        ray_graph_free(g); return ray_error("oom", NULL);
    }
    ray_op_t* src_op = ray_const_vec(g, seed_vec);
    ray_release(seed_vec);
    if (!src_op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_op_t* op = ray_expand(g, src_op, rel, direction);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}

/* (.graph.var-expand h src min-depth max-depth [direction] [track-path]) */
ray_t* ray_graph_var_expand_fn(ray_t** args, int64_t n) {
    if (n < 4 || n > 6) return ray_error("rank", NULL);
    ray_rel_t* rel = graph_unwrap(args[0]);
    if (!rel) return ray_error("type", NULL);
    if (!atom_is_int(args[1]) || !atom_is_int(args[2]) || !atom_is_int(args[3]))
        return ray_error("type", NULL);
    int64_t src_id    = atom_to_i64(args[1]);
    int64_t min_d_v   = atom_to_i64(args[2]);
    int64_t max_d_v   = atom_to_i64(args[3]);
    if (min_d_v < 0 || min_d_v > 255 || max_d_v < min_d_v || max_d_v > 255)
        return ray_error("domain", NULL);
    uint8_t direction = 0;
    bool    track     = false;
    if (n >= 5) {
        if (!atom_is_int(args[4])) return ray_error("type", NULL);
        int64_t v = atom_to_i64(args[4]);
        if (v < 0 || v > 2) return ray_error("domain", NULL);
        direction = (uint8_t)v;
    }
    if (n >= 6) {
        if (args[5]->type != -RAY_BOOL && !atom_is_int(args[5]))
            return ray_error("type", NULL);
        track = (args[5]->type == -RAY_BOOL) ? (args[5]->b8 != 0)
                                              : (atom_to_i64(args[5]) != 0);
    }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_t* seed_vec = ray_vec_from_raw(RAY_I64, &src_id, 1);
    if (!seed_vec || RAY_IS_ERR(seed_vec)) {
        ray_graph_free(g); return ray_error("oom", NULL);
    }
    ray_op_t* src_op = ray_const_vec(g, seed_vec);
    ray_release(seed_vec);
    if (!src_op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_op_t* op = ray_var_expand(g, src_op, rel, direction,
                                   (uint8_t)min_d_v, (uint8_t)max_d_v, track);
    if (!op) { ray_graph_free(g); return ray_error("oom", NULL); }
    ray_t* result = ray_execute(g, op);
    ray_graph_free(g);
    return result;
}
