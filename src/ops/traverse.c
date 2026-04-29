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

/* ============================================================================
 * Graph execution functions
 * ============================================================================ */

/* exec_expand_factorized: emit factorized output for expand+group fusion.
 * Returns a table with _src (unique sources) and _count (degree per source).
 * This avoids materializing the full (src, dst) cross-product. */
static ray_t* exec_expand_factorized(ray_rel_t* rel, uint8_t direction, ray_t* src_vec) {
    int64_t n_src = src_vec->len;
    int64_t* src_data = (int64_t*)ray_data(src_vec);

    /* Compute degrees for each source node */
    ray_t* out_src = ray_vec_new(RAY_I64, n_src > 0 ? n_src : 1);
    ray_t* out_cnt = ray_vec_new(RAY_I64, n_src > 0 ? n_src : 1);
    if (!out_src || RAY_IS_ERR(out_src) || !out_cnt || RAY_IS_ERR(out_cnt)) {
        if (out_src && !RAY_IS_ERR(out_src)) ray_release(out_src);
        if (out_cnt && !RAY_IS_ERR(out_cnt)) ray_release(out_cnt);
        return ray_error("oom", NULL);
    }

    int64_t* sd = (int64_t*)ray_data(out_src);
    int64_t* cd = (int64_t*)ray_data(out_cnt);
    int64_t out_len = 0;

    for (int64_t i = 0; i < n_src; i++) {
        int64_t node = src_data[i];
        int64_t deg = 0;
        if (direction == 0 || direction == 2) {
            if (node >= 0 && node < rel->fwd.n_nodes)
                deg += ray_csr_degree(&rel->fwd, node);
        }
        if (direction == 1 || direction == 2) {
            if (node >= 0 && node < rel->rev.n_nodes)
                deg += ray_csr_degree(&rel->rev, node);
        }
        if (deg > 0) {
            sd[out_len] = node;
            cd[out_len] = deg;
            out_len++;
        }
    }
    out_src->len = out_len;
    out_cnt->len = out_len;

    int64_t src_sym = ray_sym_intern("_src", 4);
    int64_t cnt_sym = ray_sym_intern("_count", 6);
    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(out_src); ray_release(out_cnt);
        return ray_error("oom", NULL);
    }
    ray_t* tmp = ray_table_add_col(result, src_sym, out_src);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(out_src); ray_release(out_cnt); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, cnt_sym, out_cnt);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(out_src); ray_release(out_cnt); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    ray_release(out_src); ray_release(out_cnt);
    return result;
}

/* exec_expand: 1-hop CSR neighbor expansion.
 * Count-then-fill pattern (same as exec_join). */
ray_t* exec_expand(ray_graph_t* g, ray_op_t* op, ray_t* src_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    /* Factorized mode: emit pre-aggregated degree counts */
    if (ext->graph.factorized)
        return exec_expand_factorized(rel, ext->graph.direction, src_vec);

    uint8_t direction = ext->graph.direction;
    int64_t n_src = src_vec->len;
    int64_t* src_data = (int64_t*)ray_data(src_vec);

    /* SIP runtime: check for source-side selection bitmap stored on the
     * expand ext node (set by optimizer sip_pass or manually for testing).
     *
     * If sip_sel is not pre-built but the optimizer left a filter hint in
     * pad[2..3], build a source-side bitmap by marking all source nodes
     * that have degree > 0 in the active CSR direction. */
    uint64_t* src_sel_bits = NULL;
    int64_t src_sel_len = 0;
    ray_t* sip_sel = (ray_t*)ext->graph.sip_sel;
    if (!sip_sel) {
        uint8_t filter_hint = ext->base.pad[2];
        if (filter_hint > 0 && n_src > 64) {
            /* Build SIP bitmap: mark source nodes with degree > 0.
             * For direction==2 (both), check both fwd and rev CSRs. */
            int64_t nn = rel->fwd.n_nodes;
            if (rel->rev.n_nodes > nn) nn = rel->rev.n_nodes;
            ray_t* built_sel = ray_sel_new(nn);
            if (built_sel && !RAY_IS_ERR(built_sel)) {
                uint64_t* bits = ray_sel_bits(built_sel);
                if (direction == 0 || direction == 2) {
                    for (int64_t nd = 0; nd < rel->fwd.n_nodes; nd++)
                        if (ray_csr_degree(&rel->fwd, nd) > 0)
                            RAY_SEL_BIT_SET(bits, nd);
                }
                if (direction == 1 || direction == 2) {
                    for (int64_t nd = 0; nd < rel->rev.n_nodes; nd++)
                        if (ray_csr_degree(&rel->rev, nd) > 0)
                            RAY_SEL_BIT_SET(bits, nd);
                }
                ext->graph.sip_sel = built_sel;
                sip_sel = built_sel;
            }
        }
    }
    if (sip_sel && !RAY_IS_ERR(sip_sel) && sip_sel->type == RAY_SEL) {
        src_sel_bits = ray_sel_bits(sip_sel);
        src_sel_len = sip_sel->len;
    }

    /* Helper to expand one CSR direction */
    #define EXPAND_DIR(csr_ptr) do { \
        ray_csr_t* csr = (csr_ptr); \
        /* Phase 1: count total output pairs */ \
        int64_t total = 0; \
        for (int64_t i = 0; i < n_src; i++) { \
            int64_t node = src_data[i]; \
            /* SIP skip: if source node not in selection, skip */ \
            if (src_sel_bits && node >= 0 && node < src_sel_len \
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue; \
            if (node >= 0 && node < csr->n_nodes) \
                total += ray_csr_degree(csr, node); \
        } \
        /* Phase 2: fill */ \
        ray_t* d_src = ray_vec_new(RAY_I64, total > 0 ? total : 1); \
        ray_t* d_dst = ray_vec_new(RAY_I64, total > 0 ? total : 1); \
        if (!d_src || RAY_IS_ERR(d_src) || !d_dst || RAY_IS_ERR(d_dst)) { \
            if (d_src && !RAY_IS_ERR(d_src)) ray_release(d_src); \
            if (d_dst && !RAY_IS_ERR(d_dst)) ray_release(d_dst); \
            return ray_error("oom", NULL); \
        } \
        d_src->len = total; d_dst->len = total; \
        int64_t* sd = (int64_t*)ray_data(d_src); \
        int64_t* dd = (int64_t*)ray_data(d_dst); \
        int64_t pos = 0; \
        for (int64_t i = 0; i < n_src; i++) { \
            int64_t node = src_data[i]; \
            if (node < 0 || node >= csr->n_nodes) continue; \
            /* SIP skip: must match count phase */ \
            if (src_sel_bits && node < src_sel_len \
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue; \
            int64_t cnt; \
            int64_t* nbrs = ray_csr_neighbors(csr, node, &cnt); \
            for (int64_t j = 0; j < cnt; j++) { \
                sd[pos] = node; \
                dd[pos] = nbrs[j]; \
                pos++; \
            } \
        } \
        /* Build result table */ \
        int64_t src_sym = ray_sym_intern("_src", 4); \
        int64_t dst_sym = ray_sym_intern("_dst", 4); \
        ray_t* result = ray_table_new(2); \
        if (!result || RAY_IS_ERR(result)) { \
            ray_release(d_src); ray_release(d_dst); \
            return ray_error("oom", NULL); \
        } \
        ray_t* _tmp = ray_table_add_col(result, src_sym, d_src); \
        if (!_tmp || RAY_IS_ERR(_tmp)) { ray_release(d_src); ray_release(d_dst); ray_release(result); return ray_error("oom", NULL); } \
        result = _tmp; \
        _tmp = ray_table_add_col(result, dst_sym, d_dst); \
        if (!_tmp || RAY_IS_ERR(_tmp)) { ray_release(d_src); ray_release(d_dst); ray_release(result); return ray_error("oom", NULL); } \
        result = _tmp; \
        ray_release(d_src); ray_release(d_dst); \
        return result; \
    } while (0)

    if (direction == 0) {
        EXPAND_DIR(&rel->fwd);
    } else if (direction == 1) {
        EXPAND_DIR(&rel->rev);
    } else {
        /* direction == 2: both — expand fwd, then rev, concat */
        ray_csr_t* fwd = &rel->fwd;
        ray_csr_t* rev = &rel->rev;

        /* Count forward */
        int64_t fwd_total = 0;
        for (int64_t i = 0; i < n_src; i++) {
            int64_t node = src_data[i];
            if (src_sel_bits && node >= 0 && node < src_sel_len
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue;
            if (node >= 0 && node < fwd->n_nodes)
                fwd_total += ray_csr_degree(fwd, node);
        }
        /* Count reverse */
        int64_t rev_total = 0;
        for (int64_t i = 0; i < n_src; i++) {
            int64_t node = src_data[i];
            if (src_sel_bits && node >= 0 && node < src_sel_len
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue;
            if (node >= 0 && node < rev->n_nodes)
                rev_total += ray_csr_degree(rev, node);
        }

        int64_t total = fwd_total + rev_total;
        ray_t* d_src = ray_vec_new(RAY_I64, total > 0 ? total : 1);
        ray_t* d_dst = ray_vec_new(RAY_I64, total > 0 ? total : 1);
        if (!d_src || RAY_IS_ERR(d_src) || !d_dst || RAY_IS_ERR(d_dst)) {
            if (d_src && !RAY_IS_ERR(d_src)) ray_release(d_src);
            if (d_dst && !RAY_IS_ERR(d_dst)) ray_release(d_dst);
            return ray_error("oom", NULL);
        }
        d_src->len = total; d_dst->len = total;
        int64_t* sd = (int64_t*)ray_data(d_src);
        int64_t* dd = (int64_t*)ray_data(d_dst);
        int64_t pos = 0;

        /* Fill forward */
        for (int64_t i = 0; i < n_src; i++) {
            int64_t node = src_data[i];
            if (node < 0 || node >= fwd->n_nodes) continue;
            if (src_sel_bits && node < src_sel_len
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue;
            int64_t cnt;
            int64_t* nbrs = ray_csr_neighbors(fwd, node, &cnt);
            for (int64_t j = 0; j < cnt; j++) {
                sd[pos] = node; dd[pos] = nbrs[j]; pos++;
            }
        }
        /* Fill reverse */
        for (int64_t i = 0; i < n_src; i++) {
            int64_t node = src_data[i];
            if (node < 0 || node >= rev->n_nodes) continue;
            if (src_sel_bits && node < src_sel_len
                && !RAY_SEL_BIT_TEST(src_sel_bits, node)) continue;
            int64_t cnt;
            int64_t* nbrs = ray_csr_neighbors(rev, node, &cnt);
            for (int64_t j = 0; j < cnt; j++) {
                sd[pos] = node; dd[pos] = nbrs[j]; pos++;
            }
        }

        int64_t src_sym = ray_sym_intern("_src", 4);
        int64_t dst_sym = ray_sym_intern("_dst", 4);
        ray_t* result = ray_table_new(2);
        if (!result || RAY_IS_ERR(result)) {
            ray_release(d_src); ray_release(d_dst);
            return ray_error("oom", NULL);
        }
        ray_t* tmp = ray_table_add_col(result, src_sym, d_src);
        if (!tmp || RAY_IS_ERR(tmp)) { ray_release(d_src); ray_release(d_dst); ray_release(result); return ray_error("oom", NULL); }
        result = tmp;
        tmp = ray_table_add_col(result, dst_sym, d_dst);
        if (!tmp || RAY_IS_ERR(tmp)) { ray_release(d_src); ray_release(d_dst); ray_release(result); return ray_error("oom", NULL); }
        result = tmp;
        ray_release(d_src); ray_release(d_dst);
        return result;
    }
    #undef EXPAND_DIR
}

/* exec_var_expand: iterative BFS with depth limit and cycle detection */
ray_t* exec_var_expand(ray_graph_t* g, ray_op_t* op, ray_t* start_vec) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    uint8_t direction = ext->graph.direction;
    uint8_t min_depth = ext->graph.min_depth;
    uint8_t max_depth = ext->graph.max_depth;
    ray_csr_t* csr_fwd = &rel->fwd;
    ray_csr_t* csr_rev = &rel->rev;
    /* For direction==2 (both), use fwd for n_nodes bound but expand both */
    ray_csr_t* csr = (direction == 1) ? csr_rev : csr_fwd;

    int64_t n_start = start_vec->len;
    int64_t* start_data = (int64_t*)ray_data(start_vec);

    /* Pre-allocate output buffers (grow as needed) */
    int64_t out_cap = 1024;
    ray_t *start_hdr, *end_hdr, *depth_hdr;
    int64_t* out_start = (int64_t*)scratch_alloc(&start_hdr, (size_t)out_cap * sizeof(int64_t));
    int64_t* out_end   = (int64_t*)scratch_alloc(&end_hdr,   (size_t)out_cap * sizeof(int64_t));
    int64_t* out_depth = (int64_t*)scratch_alloc(&depth_hdr, (size_t)out_cap * sizeof(int64_t));
    if (!out_start || !out_end || !out_depth) {
        scratch_free(start_hdr); scratch_free(end_hdr); scratch_free(depth_hdr);
        return ray_error("oom", NULL);
    }
    int64_t out_count = 0;

    /* For direction==2, use the larger n_nodes bound */
    int64_t bfs_n_nodes = csr->n_nodes;
    if (direction == 2 && csr_rev->n_nodes > bfs_n_nodes)
        bfs_n_nodes = csr_rev->n_nodes;

    /* BFS per start node */
    for (int64_t s = 0; s < n_start; s++) {
        int64_t start_node = start_data[s];
        if (start_node < 0 || start_node >= bfs_n_nodes) continue;

        /* Visited bitmap via RAY_SEL */
        ray_t* visited_sel = ray_sel_new(bfs_n_nodes);
        if (!visited_sel || RAY_IS_ERR(visited_sel)) continue;
        uint64_t* visited = ray_sel_bits(visited_sel);
        RAY_SEL_BIT_SET(visited, start_node);

        /* Frontier */
        ray_t* front_hdr;
        int64_t front_cap = 256;
        int64_t* frontier = (int64_t*)scratch_alloc(&front_hdr, (size_t)front_cap * sizeof(int64_t));
        if (!frontier) { ray_release(visited_sel); continue; }
        frontier[0] = start_node;
        int64_t front_len = 1;

        for (uint8_t depth = 1; depth <= max_depth && front_len > 0; depth++) {
            ray_t* next_hdr;
            int64_t next_cap = (front_len > INT64_MAX / 4) ? INT64_MAX : front_len * 4;
            if (next_cap < 64) next_cap = 64;
            int64_t* next_front = (int64_t*)scratch_alloc(&next_hdr, (size_t)next_cap * sizeof(int64_t));
            if (!next_front) { scratch_free(front_hdr); ray_release(visited_sel); goto cleanup; }
            int64_t next_len = 0;

            for (int64_t f = 0; f < front_len; f++) {
                int64_t node = frontier[f];
                /* Expand neighbors from active CSR(s).
                 * For direction==2 (both), expand fwd then rev. */
                int n_csrs = (direction == 2) ? 2 : 1;
                ray_csr_t* csrs[2] = { csr, csr_rev };
                for (int ci = 0; ci < n_csrs; ci++) {
                    ray_csr_t* cur_csr = csrs[ci];
                    if (node < 0 || node >= cur_csr->n_nodes) continue;
                int64_t cnt;
                int64_t* nbrs = ray_csr_neighbors(cur_csr, node, &cnt);
                for (int64_t j = 0; j < cnt; j++) {
                    int64_t nbr = nbrs[j];
                    if (nbr < 0 || nbr >= bfs_n_nodes) continue;
                    if (RAY_SEL_BIT_TEST(visited, nbr)) continue;
                    RAY_SEL_BIT_SET(visited, nbr);

                    /* Grow next_front if needed */
                    if (next_len >= next_cap) {
                        if (next_cap > INT64_MAX / 2) break;
                        int64_t new_cap = next_cap * 2;
                        int64_t* new_nf = (int64_t*)scratch_realloc(&next_hdr,
                            (size_t)next_cap * sizeof(int64_t),
                            (size_t)new_cap * sizeof(int64_t));
                        if (!new_nf) break;
                        next_front = new_nf;
                        next_cap = new_cap;
                    }
                    next_front[next_len++] = nbr;

                    /* Emit if within depth range */
                    if (depth >= min_depth) {
                        if (out_count >= out_cap) {
                            if (out_cap > INT64_MAX / 2) break;
                            int64_t new_oc = out_cap * 2;
                            /* Grow all three buffers atomically — alloc new
                             * copies first, commit only if all succeed. */
                            ray_t *ns_h = NULL, *ne_h = NULL, *nd_h = NULL;
                            size_t old_sz = (size_t)out_cap * sizeof(int64_t);
                            size_t new_sz = (size_t)new_oc * sizeof(int64_t);
                            int64_t* ns = (int64_t*)scratch_alloc(&ns_h, new_sz);
                            int64_t* ne = (int64_t*)scratch_alloc(&ne_h, new_sz);
                            int64_t* nd_buf = (int64_t*)scratch_alloc(&nd_h, new_sz);
                            if (!ns || !ne || !nd_buf) {
                                scratch_free(ns_h); scratch_free(ne_h); scratch_free(nd_h);
                                break;
                            }
                            memcpy(ns, out_start, old_sz);
                            memcpy(ne, out_end, old_sz);
                            memcpy(nd_buf, out_depth, old_sz);
                            scratch_free(start_hdr); scratch_free(end_hdr); scratch_free(depth_hdr);
                            start_hdr = ns_h; end_hdr = ne_h; depth_hdr = nd_h;
                            out_start = ns; out_end = ne; out_depth = nd_buf;
                            out_cap = new_oc;
                        }
                        out_start[out_count] = start_node;
                        out_end[out_count] = nbr;
                        out_depth[out_count] = depth;
                        out_count++;
                    }
                }
                } /* end for ci (CSR directions) */
            }

            scratch_free(front_hdr);
            front_hdr = next_hdr;
            frontier = next_front;
            front_len = next_len;
        }

        scratch_free(front_hdr);
        ray_release(visited_sel);
    }

cleanup:;
    /* Build output table */
    ray_t* v_start = ray_vec_from_raw(RAY_I64, out_start, out_count);
    ray_t* v_end   = ray_vec_from_raw(RAY_I64, out_end,   out_count);
    ray_t* v_depth = ray_vec_from_raw(RAY_I64, out_depth, out_count);
    scratch_free(start_hdr); scratch_free(end_hdr); scratch_free(depth_hdr);

    if (!v_start || RAY_IS_ERR(v_start) || !v_end || RAY_IS_ERR(v_end) ||
        !v_depth || RAY_IS_ERR(v_depth)) {
        if (v_start && !RAY_IS_ERR(v_start)) ray_release(v_start);
        if (v_end && !RAY_IS_ERR(v_end)) ray_release(v_end);
        if (v_depth && !RAY_IS_ERR(v_depth)) ray_release(v_depth);
        return ray_error("oom", NULL);
    }

    int64_t start_sym = ray_sym_intern("_start", 6);
    int64_t end_sym   = ray_sym_intern("_end", 4);
    int64_t depth_sym = ray_sym_intern("_depth", 6);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(v_start); ray_release(v_end); ray_release(v_depth);
        return ray_error("oom", NULL);
    }
    ray_t* tmp = ray_table_add_col(result, start_sym, v_start);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_start); ray_release(v_end); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, end_sym, v_end);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_start); ray_release(v_end); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, depth_sym, v_depth);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_start); ray_release(v_end); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    ray_release(v_start); ray_release(v_end); ray_release(v_depth);
    return result;
}

/* exec_shortest_path: BFS from src to dst with parent tracking */
ray_t* exec_shortest_path(ray_graph_t* g, ray_op_t* op,
                                 ray_t* src_val, ray_t* dst_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);
    uint8_t direction = ext->graph.direction;
    ray_csr_t* csr = (direction == 1) ? &rel->rev : &rel->fwd;
    ray_csr_t* csr_rev = &rel->rev;
    int n_csrs = (direction == 2) ? 2 : 1;
    ray_csr_t* csrs[2] = { csr, csr_rev };
    int64_t bfs_n_nodes = csr->n_nodes;
    if (direction == 2 && csr_rev->n_nodes > bfs_n_nodes)
        bfs_n_nodes = csr_rev->n_nodes;
    uint8_t max_depth = ext->graph.max_depth;

    /* Extract single I64 values */
    int64_t src_node, dst_node;
    if (ray_is_atom(src_val)) {
        src_node = src_val->i64;
    } else {
        if (src_val->len == 0) return ray_error("range", NULL);
        src_node = ((int64_t*)ray_data(src_val))[0];
    }
    if (ray_is_atom(dst_val)) {
        dst_node = dst_val->i64;
    } else {
        if (dst_val->len == 0) return ray_error("range", NULL);
        dst_node = ((int64_t*)ray_data(dst_val))[0];
    }

    if (src_node < 0 || src_node >= bfs_n_nodes ||
        dst_node < 0 || dst_node >= bfs_n_nodes)
        return ray_error("range", NULL);

    /* Special case: src == dst */
    if (src_node == dst_node) {
        ray_t* v_node = ray_vec_from_raw(RAY_I64, &src_node, 1);
        int64_t zero = 0;
        ray_t* v_depth = ray_vec_from_raw(RAY_I64, &zero, 1);
        if (!v_node || RAY_IS_ERR(v_node) || !v_depth || RAY_IS_ERR(v_depth)) {
            if (v_node && !RAY_IS_ERR(v_node)) ray_release(v_node);
            if (v_depth && !RAY_IS_ERR(v_depth)) ray_release(v_depth);
            return ray_error("oom", NULL);
        }
        ray_t* result = ray_table_new(2);
        if (!result || RAY_IS_ERR(result)) { ray_release(v_node); ray_release(v_depth); return ray_error("oom", NULL); }
        ray_t* tmp = ray_table_add_col(result, sym_intern_safe("_node", 5), v_node);
        if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_node); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
        result = tmp;
        tmp = ray_table_add_col(result, sym_intern_safe("_depth", 6), v_depth);
        if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_node); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
        result = tmp;
        ray_release(v_node); ray_release(v_depth);
        return result;
    }

    /* Allocate parent array (-1 = unvisited) */
    ray_t* parent_hdr;
    int64_t* parent = (int64_t*)scratch_alloc(&parent_hdr,
                                               (size_t)bfs_n_nodes * sizeof(int64_t));
    if (!parent) return ray_error("oom", NULL);
    memset(parent, 0xFF, (size_t)bfs_n_nodes * sizeof(int64_t)); /* -1 */
    parent[src_node] = src_node;

    /* BFS queue */
    ray_t* queue_hdr;
    int64_t q_cap = 1024;
    int64_t* queue = (int64_t*)scratch_alloc(&queue_hdr, (size_t)q_cap * sizeof(int64_t));
    if (!queue) { scratch_free(parent_hdr); return ray_error("oom", NULL); }
    queue[0] = src_node;
    int64_t q_start = 0, q_end = 1;
    bool found = false;

    for (uint8_t depth = 1; depth <= max_depth && !found; depth++) {
        int64_t level_end = q_end;
        for (int64_t qi = q_start; qi < level_end && !found; qi++) {
            int64_t node = queue[qi];
            for (int ci = 0; ci < n_csrs && !found; ci++) {
                ray_csr_t* cur_csr = csrs[ci];
                if (node < 0 || node >= cur_csr->n_nodes) continue;
                int64_t cnt;
                int64_t* nbrs = ray_csr_neighbors(cur_csr, node, &cnt);
                for (int64_t j = 0; j < cnt; j++) {
                    int64_t nbr = nbrs[j];
                    if (nbr < 0 || nbr >= bfs_n_nodes) continue;
                    if (parent[nbr] != -1) continue;
                    parent[nbr] = node;

                    if (nbr == dst_node) { found = true; break; }

                    /* Grow queue if needed */
                    if (q_end >= q_cap) {
                        if (q_cap > INT64_MAX / 2) { found = false; goto bfs_done; }
                        int64_t new_cap = q_cap * 2;
                        int64_t* new_q = (int64_t*)scratch_realloc(&queue_hdr,
                            (size_t)q_cap * sizeof(int64_t),
                            (size_t)new_cap * sizeof(int64_t));
                        if (!new_q) { found = false; goto bfs_done; }
                        queue = new_q;
                        q_cap = new_cap;
                    }
                    queue[q_end++] = nbr;
                }
            } /* end for ci (CSR directions) */
        }
        q_start = level_end;
    }

bfs_done:
    scratch_free(queue_hdr);

    if (!found) {
        scratch_free(parent_hdr);
        return ray_error("range", NULL);
    }

    /* Reconstruct path */
    int64_t path_buf[256];
    int64_t path_len = 0;
    int64_t cur = dst_node;
    while (cur != src_node && path_len < 255) {
        path_buf[path_len++] = cur;
        cur = parent[cur];
    }
    if (cur != src_node) {
        scratch_free(parent_hdr);
        return ray_error("range", "path exceeds 254 hops");
    }
    path_buf[path_len++] = src_node;
    scratch_free(parent_hdr);

    /* Reverse path */
    for (int64_t i = 0; i < path_len / 2; i++) {
        int64_t tmp = path_buf[i];
        path_buf[i] = path_buf[path_len - 1 - i];
        path_buf[path_len - 1 - i] = tmp;
    }

    /* Build output table */
    ray_t* v_node = ray_vec_from_raw(RAY_I64, path_buf, path_len);
    ray_t* v_depth = ray_vec_new(RAY_I64, path_len);
    if (!v_node || RAY_IS_ERR(v_node) || !v_depth || RAY_IS_ERR(v_depth)) {
        if (v_node && !RAY_IS_ERR(v_node)) ray_release(v_node);
        if (v_depth && !RAY_IS_ERR(v_depth)) ray_release(v_depth);
        return ray_error("oom", NULL);
    }
    v_depth->len = path_len;
    int64_t* dep_data = (int64_t*)ray_data(v_depth);
    for (int64_t i = 0; i < path_len; i++) dep_data[i] = i;

    int64_t node_sym  = ray_sym_intern("_node", 5);
    int64_t depth_sym = ray_sym_intern("_depth", 6);
    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) { ray_release(v_node); ray_release(v_depth); return ray_error("oom", NULL); }
    ray_t* tmp = ray_table_add_col(result, node_sym, v_node);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_node); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, depth_sym, v_depth);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(v_node); ray_release(v_depth); ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    ray_release(v_node); ray_release(v_depth);
    return result;
}

/* --------------------------------------------------------------------------
 * exec_pagerank: iterative PageRank over CSR adjacency.
 *
 * rank[v] = (1 - d)/N + d * SUM(rank[u] / out_degree[u]) for u in in_neighbors(v)
 *
 * Uses reverse CSR for in-neighbors, forward CSR for out-degree.
 * -------------------------------------------------------------------------- */
ray_t* exec_pagerank(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n       = rel->fwd.n_nodes;
    uint16_t iters  = ext->graph.max_iter;
    double damping  = ext->graph.damping;

    if (n <= 0) return ray_error("length", NULL);

    /* Arena for all scratch memory — freed in one shot */
    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    double* rank     = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    double* rank_new = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    if (!rank || !rank_new) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    double init = 1.0 / (double)n;
    for (int64_t i = 0; i < n; i++) rank[i] = init;

    /* Get raw CSR arrays for direct access */
    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    double base = (1.0 - damping) / (double)n;

    for (uint16_t iter = 0; iter < iters; iter++) {
        /* Dangling node correction: redistribute rank of zero-out-degree nodes */
        double dangling_sum = 0.0;
        for (int64_t u = 0; u < n; u++) {
            if (fwd_off[u + 1] == fwd_off[u]) dangling_sum += rank[u];
        }
        double adjusted_base = base + damping * dangling_sum / (double)n;

        for (int64_t v = 0; v < n; v++) {
            double sum = 0.0;
            /* Iterate over in-neighbors of v using reverse CSR */
            int64_t rev_start = rev_off[v];
            int64_t rev_end   = rev_off[v + 1];
            for (int64_t j = rev_start; j < rev_end; j++) {
                int64_t u = rev_tgt[j];
                /* out_degree of u from forward CSR */
                int64_t out_deg = fwd_off[u + 1] - fwd_off[u];
                if (out_deg > 0) {
                    sum += rank[u] / (double)out_deg;
                }
            }
            rank_new[v] = adjusted_base + damping * sum;
        }
        /* Swap */
        double* tmp = rank;
        rank = rank_new;
        rank_new = tmp;
    }

    /* Build output table: _node (I64), _rank (F64) */
    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* rank_vec = ray_vec_new(RAY_F64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !rank_vec || RAY_IS_ERR(rank_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (rank_vec && !RAY_IS_ERR(rank_vec)) ray_release(rank_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    double*  rdata = (double*)ray_data(rank_vec);
    for (int64_t i = 0; i < n; i++) {
        ndata[i] = i;
        rdata[i] = rank[i];
    }
    node_vec->len = n;
    rank_vec->len = n;

    ray_scratch_arena_reset(&arena);

    /* Package as table with named columns */
    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(rank_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_rank", 5), rank_vec);
    ray_release(rank_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_connected_comp: connected components via label propagation.
 * Treats graph as undirected (uses both forward and reverse CSR).
 * O(diameter * |E|) time.
 * -------------------------------------------------------------------------- */
ray_t* exec_connected_comp(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);

    /* Arena for all scratch memory — freed in one shot */
    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    int64_t* label = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!label) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    /* Initialize: each node is its own component */
    for (int64_t i = 0; i < n; i++) label[i] = i;

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    /* Iterate until convergence */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int64_t v = 0; v < n; v++) {
            int64_t min_label = label[v];
            /* Forward neighbors */
            for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
                int64_t u = fwd_tgt[j];
                if (label[u] < min_label) min_label = label[u];
            }
            /* Reverse neighbors */
            for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
                int64_t u = rev_tgt[j];
                if (label[u] < min_label) min_label = label[u];
            }
            if (min_label < label[v]) {
                label[v] = min_label;
                changed = true;
            }
        }
    }

    /* Build output table */
    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* comp_vec = ray_vec_new(RAY_I64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !comp_vec || RAY_IS_ERR(comp_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (comp_vec && !RAY_IS_ERR(comp_vec)) ray_release(comp_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    int64_t* cdata = (int64_t*)ray_data(comp_vec);
    for (int64_t i = 0; i < n; i++) {
        ndata[i] = i;
        cdata[i] = label[i];
    }
    node_vec->len = n;
    comp_vec->len = n;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(comp_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_component", 10), comp_vec);
    ray_release(comp_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_dijkstra: weighted shortest path via Dijkstra's algorithm.
 * Uses a binary min-heap. Reads edge weights from CSR property table.
 * Returns table with _node (I64), _dist (F64), _depth (I64).
 * -------------------------------------------------------------------------- */

/* Min-heap entry for Dijkstra */
typedef struct {
    double   dist;
    int64_t  node;
} dijk_entry_t;

static void dijk_heap_push(dijk_entry_t* heap, int64_t* size,
                            double dist, int64_t node) {
    int64_t i = (*size)++;
    heap[i].dist = dist;
    heap[i].node = node;
    /* Sift up */
    while (i > 0) {
        int64_t parent = (i - 1) / 2;
        if (heap[parent].dist <= heap[i].dist) break;
        dijk_entry_t tmp = heap[parent];
        heap[parent] = heap[i];
        heap[i] = tmp;
        i = parent;
    }
}

static dijk_entry_t dijk_heap_pop(dijk_entry_t* heap, int64_t* size) {
    dijk_entry_t top = heap[0];
    (*size)--;
    if (*size > 0) {
        heap[0] = heap[*size];
        /* Sift down */
        int64_t i = 0;
        while (1) {
            int64_t left  = 2 * i + 1;
            int64_t right = 2 * i + 2;
            int64_t smallest = i;
            if (left  < *size && heap[left].dist  < heap[smallest].dist) smallest = left;
            if (right < *size && heap[right].dist < heap[smallest].dist) smallest = right;
            if (smallest == i) break;
            dijk_entry_t tmp = heap[i];
            heap[i] = heap[smallest];
            heap[smallest] = tmp;
            i = smallest;
        }
    }
    return top;
}

/* Reusable Dijkstra with optional node/edge masks (for Yen's k-shortest) */
static double dijkstra_masked(
    int64_t* fwd_off, int64_t* fwd_tgt, int64_t* fwd_row,
    double* weights, int64_t n,
    int64_t src_id, int64_t dst_id,
    bool* node_mask,    /* NULL or bool[n]: true = blocked */
    bool* edge_mask,    /* NULL or bool[m]: true = blocked */
    double* dist,       /* pre-allocated double[n] */
    int64_t* parent,    /* pre-allocated int64_t[n] */
    dijk_entry_t* heap, /* pre-allocated */
    bool* visited)      /* pre-allocated bool[n] */
{
    for (int64_t i = 0; i < n; i++) {
        dist[i] = 1e308;
        parent[i] = -1;
        visited[i] = false;
    }

    dist[src_id] = 0.0;
    int64_t heap_size = 0;
    dijk_heap_push(heap, &heap_size, 0.0, src_id);

    while (heap_size > 0) {
        dijk_entry_t top = dijk_heap_pop(heap, &heap_size);
        int64_t u = top.node;
        if (visited[u]) continue;
        visited[u] = true;

        if (u == dst_id) break;

        for (int64_t j = fwd_off[u]; j < fwd_off[u + 1]; j++) {
            if (edge_mask && edge_mask[j]) continue;
            int64_t v = fwd_tgt[j];
            if (node_mask && node_mask[v]) continue;
            int64_t edge_row = fwd_row[j];
            double w = weights[edge_row];
            double new_dist = dist[u] + w;
            if (new_dist < dist[v]) {
                dist[v] = new_dist;
                parent[v] = u;
                dijk_heap_push(heap, &heap_size, new_dist, v);
            }
        }
    }

    return dist[dst_id];
}

ray_t* exec_dijkstra(ray_graph_t* g, ray_op_t* op,
                             ray_t* src_val, ray_t* dst_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);
    if (!rel->fwd.props) return ray_error("schema", NULL); /* need edge properties */

    int64_t n = rel->fwd.n_nodes;
    int64_t m = rel->fwd.n_edges;
    int64_t src_id = ray_is_atom(src_val) ? src_val->i64 : ((int64_t*)ray_data(src_val))[0];
    int64_t dst_id = !dst_val ? -1 : ray_is_atom(dst_val) ? dst_val->i64 : ((int64_t*)ray_data(dst_val))[0];

    if (src_id < 0 || src_id >= n) return ray_error("range", NULL);
    if (dst_id != -1 && (dst_id < 0 || dst_id >= n)) return ray_error("range", NULL);

    /* Find weight column in edge properties */
    int64_t weight_sym = ext->graph.weight_col_sym;
    ray_t* props = rel->fwd.props;
    ray_t* weight_vec = ray_table_get_col(props, weight_sym);
    if (!weight_vec || RAY_IS_ERR(weight_vec)) return ray_error("schema", NULL);
    if (weight_vec->type != RAY_F64) return ray_error("schema", NULL);
    double* weights = (double*)ray_data(weight_vec);

    /* Dijkstra requires non-negative edge weights */
    for (int64_t i = 0; i < m; i++) {
        if (weights[i] < 0.0)
            return ray_error("domain", "Dijkstra requires non-negative edge weights");
    }

    /* Allocate working arrays.
     * Heap capacity = max(n, m) + 1: each edge relaxation can push one entry,
     * and with lazy deletion (visited check on pop) the heap can grow up to m. */
    int64_t heap_cap = (m > n ? m : n) + 1;

    /* Arena for all scratch memory — freed in one shot */
    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    double*  dist    = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    bool*    visited = (bool*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(bool));
    int64_t* depth   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    dijk_entry_t* heap = (dijk_entry_t*)ray_scratch_arena_push(&arena,
                              (size_t)heap_cap * sizeof(dijk_entry_t));
    if (!dist || !visited || !depth || !heap) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }
    memset(visited, 0, (size_t)n * sizeof(bool));
    memset(depth, 0, (size_t)n * sizeof(int64_t));

    for (int64_t i = 0; i < n; i++) {
        dist[i] = 1e308;  /* infinity */
    }
    dist[src_id] = 0.0;

    int64_t heap_size = 0;
    dijk_heap_push(heap, &heap_size, 0.0, src_id);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* fwd_row = (int64_t*)ray_data(rel->fwd.rowmap);

    while (heap_size > 0) {
        dijk_entry_t top = dijk_heap_pop(heap, &heap_size);
        int64_t u = top.node;
        if (visited[u]) continue;
        visited[u] = true;

        if (u == dst_id) break;  /* early exit if destination reached */

        for (int64_t j = fwd_off[u]; j < fwd_off[u + 1]; j++) {
            int64_t v = fwd_tgt[j];
            int64_t edge_row = fwd_row[j];
            double w = weights[edge_row];
            double new_dist = dist[u] + w;
            if (new_dist < dist[v]) {
                dist[v] = new_dist;
                depth[v] = depth[u] + 1;
                dijk_heap_push(heap, &heap_size, new_dist, v);
            }
        }
    }

    /* Collect reachable nodes */
    int64_t count = 0;
    for (int64_t i = 0; i < n; i++) {
        if (dist[i] < 1e308) count++;
    }

    ray_t* node_vec  = ray_vec_new(RAY_I64, count);
    ray_t* dist_vec  = ray_vec_new(RAY_F64, count);
    ray_t* depth_vec = ray_vec_new(RAY_I64, count);
    if (!node_vec || RAY_IS_ERR(node_vec) ||
        !dist_vec || RAY_IS_ERR(dist_vec) ||
        !depth_vec || RAY_IS_ERR(depth_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (dist_vec && !RAY_IS_ERR(dist_vec)) ray_release(dist_vec);
        if (depth_vec && !RAY_IS_ERR(depth_vec)) ray_release(depth_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    double*  ddata = (double*)ray_data(dist_vec);
    int64_t* hdata = (int64_t*)ray_data(depth_vec);
    int64_t idx = 0;
    for (int64_t i = 0; i < n; i++) {
        if (dist[i] < 1e308) {
            ndata[idx] = i;
            ddata[idx] = dist[i];
            hdata[idx] = depth[i];
            idx++;
        }
    }
    node_vec->len = count;
    dist_vec->len = count;
    depth_vec->len = count;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(dist_vec);
        ray_release(depth_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_dist", 5), dist_vec);
    ray_release(dist_vec);
    result = ray_table_add_col(result, sym_intern_safe("_depth", 6), depth_vec);
    ray_release(depth_vec);

    return result;
}

/* exec_wco_join: Worst-Case Optimal Join via general Leapfrog Triejoin */
ray_t* exec_wco_join(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t** rels = (ray_rel_t**)ext->wco.rels;
    uint8_t n_rels = ext->wco.n_rels;
    uint8_t n_vars = ext->wco.n_vars;

    if (!rels || n_rels == 0) return ray_error("schema", NULL);
    if (n_vars > LFTJ_MAX_VARS) return ray_error("nyi", NULL);

    /* Validate sorted CSR (both fwd and rev, since LFTJ may use either) */
    for (uint8_t r = 0; r < n_rels; r++) {
        if (!rels[r] || !rels[r]->fwd.sorted || !rels[r]->rev.sorted)
            return ray_error("domain", NULL);
    }

    /* Build binding plan */
    lftj_enum_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!lftj_build_default_plan(&ctx, rels, n_rels, n_vars))
        return ray_error("nyi", NULL);

    /* Allocate output buffers */
    int64_t out_cap = 4096;
    ray_t* col_data_block;
    int64_t** col_data = (int64_t**)scratch_alloc(&col_data_block,
                              (size_t)n_vars * sizeof(int64_t*));
    if (!col_data) {
        scratch_free(col_data_block);
        return ray_error("oom", NULL);
    }

    for (uint8_t v = 0; v < n_vars; v++) {
        ray_t* h = ray_alloc((size_t)out_cap * sizeof(int64_t));
        if (!h) {
            for (uint8_t j = 0; j < v; j++) ray_free(ctx.buf_hdrs[j]);
            scratch_free(col_data_block);
            return ray_error("oom", NULL);
        }
        ctx.buf_hdrs[v] = h;
        col_data[v] = (int64_t*)ray_data(h);
    }

    ctx.col_data = col_data;
    ctx.out_count = 0;
    ctx.out_cap = out_cap;
    ctx.oom = false;

    /* Run general LFTJ enumeration */
    lftj_enumerate(&ctx, 0);

    if (ctx.oom) {
        for (uint8_t v = 0; v < n_vars; v++) ray_free(ctx.buf_hdrs[v]);
        scratch_free(col_data_block);
        return ray_error("oom", NULL);
    }

    /* Build output table */
    ray_t* result = ray_table_new(n_vars);
    if (!result || RAY_IS_ERR(result)) {
        for (uint8_t v = 0; v < n_vars; v++) ray_free(ctx.buf_hdrs[v]);
        scratch_free(col_data_block);
        return ray_error("oom", NULL);
    }

    for (uint8_t v = 0; v < n_vars; v++) {
        ray_t* vec = ray_vec_from_raw(RAY_I64, ctx.col_data[v], ctx.out_count);
        ray_free(ctx.buf_hdrs[v]);
        if (!vec || RAY_IS_ERR(vec)) {
            for (uint8_t j = v + 1; j < n_vars; j++) ray_free(ctx.buf_hdrs[j]);
            scratch_free(col_data_block);
            ray_release(result);
            return ray_error("oom", NULL);
        }
        char name_buf[12];
        int n = snprintf(name_buf, sizeof(name_buf), "_v%d", v);
        int64_t name_id = ray_sym_intern(name_buf, (size_t)n);
        ray_t* new_result = ray_table_add_col(result, name_id, vec);
        ray_release(vec);
        if (!new_result || RAY_IS_ERR(new_result)) {
            for (uint8_t j = v + 1; j < n_vars; j++) ray_free(ctx.buf_hdrs[j]);
            scratch_free(col_data_block);
            ray_release(result);
            return ray_error("oom", NULL);
        }
        result = new_result;
    }

    scratch_free(col_data_block);
    return result;
}

/* --------------------------------------------------------------------------
 * exec_louvain: community detection via Louvain modularity optimization.
 * Phase 1 only (no graph contraction).
 * Maximizes modularity Q = (1/2m) * SUM[(A_ij - k_i*k_j/2m) * delta(c_i, c_j)]
 * Treats graph as undirected. Uses forward+reverse CSR.
 * -------------------------------------------------------------------------- */
ray_t* exec_louvain(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    int64_t m = rel->fwd.n_edges;
    uint16_t max_iter = ext->graph.max_iter;

    if (n <= 0) return ray_error("length", NULL);

    /* Arena for all scratch memory — freed in one shot */
    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    int64_t* community = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* degree    = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* comm_tot  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!community || !degree || !comm_tot) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    /* Initialize: each node in its own community */
    for (int64_t i = 0; i < n; i++) {
        community[i] = i;
        degree[i] = (fwd_off[i+1] - fwd_off[i]) + (rev_off[i+1] - rev_off[i]);
        comm_tot[i] = degree[i];
    }

    double two_m = (double)(2 * m);
    if (two_m == 0) two_m = 1;

    /* Scratch space for per-community edge counts (reused across iterations).
     * k_i_in[c] = number of edges from node v to community c. */
    int64_t* k_i_in = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    /* Track which communities were touched so we can reset k_i_in efficiently */
    int64_t* touched = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!k_i_in || !touched) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }
    memset(k_i_in, 0, (size_t)n * sizeof(int64_t));

    for (uint16_t iter = 0; iter < max_iter; iter++) {
        bool moved = false;
        for (int64_t v = 0; v < n; v++) {
            int64_t old_comm = community[v];
            int64_t n_touched = 0;

            /* Aggregate edges per neighbor community (forward + reverse) */
            for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
                int64_t c = community[fwd_tgt[j]];
                if (c == old_comm) continue;
                if (k_i_in[c] == 0) touched[n_touched++] = c;
                k_i_in[c]++;
            }
            for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
                int64_t c = community[rev_tgt[j]];
                if (c == old_comm) continue;
                if (k_i_in[c] == 0) touched[n_touched++] = c;
                k_i_in[c]++;
            }

            /* Evaluate modularity gain for each candidate community.
             * delta_Q = k_i_in[c] / two_m - (sigma_tot[c] * k_v) / (two_m * two_m) */
            int64_t best_comm = old_comm;
            double best_gain = 0.0;
            double k_v = (double)degree[v];

            for (int64_t t = 0; t < n_touched; t++) {
                int64_t c = touched[t];
                double sigma_tot = (double)comm_tot[c];
                double gain = (double)k_i_in[c] / two_m
                            - (sigma_tot * k_v) / (two_m * two_m);
                if (gain > best_gain) {
                    best_gain = gain;
                    best_comm = c;
                }
            }

            /* Reset k_i_in for touched communities */
            for (int64_t t = 0; t < n_touched; t++) {
                k_i_in[touched[t]] = 0;
            }

            if (best_comm != old_comm) {
                comm_tot[old_comm] -= degree[v];
                comm_tot[best_comm] += degree[v];
                community[v] = best_comm;
                moved = true;
            }
        }
        if (!moved) break;
    }

    /* Normalize community IDs to 0..k-1 */
    int64_t* remap = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!remap) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }
    for (int64_t i = 0; i < n; i++) remap[i] = -1;
    int64_t next_id = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t c = community[i];
        if (remap[c] < 0) remap[c] = next_id++;
        community[i] = remap[c];
    }

    /* Build output table */
    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* comm_vec = ray_vec_new(RAY_I64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !comm_vec || RAY_IS_ERR(comm_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (comm_vec && !RAY_IS_ERR(comm_vec)) ray_release(comm_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    int64_t* cdata = (int64_t*)ray_data(comm_vec);
    for (int64_t i = 0; i < n; i++) {
        ndata[i] = i;
        cdata[i] = community[i];
    }
    node_vec->len = n;
    comm_vec->len = n;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(comm_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_community", 10), comm_vec);
    ray_release(comm_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_degree_cent: in/out/total degree from CSR offsets. O(n).
 * -------------------------------------------------------------------------- */
ray_t* exec_degree_cent(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);

    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* in_vec   = ray_vec_new(RAY_I64, n);
    ray_t* out_vec  = ray_vec_new(RAY_I64, n);
    ray_t* deg_vec  = ray_vec_new(RAY_I64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) ||
        !in_vec   || RAY_IS_ERR(in_vec)   ||
        !out_vec  || RAY_IS_ERR(out_vec)  ||
        !deg_vec  || RAY_IS_ERR(deg_vec)) {
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (in_vec   && !RAY_IS_ERR(in_vec))   ray_release(in_vec);
        if (out_vec  && !RAY_IS_ERR(out_vec))  ray_release(out_vec);
        if (deg_vec  && !RAY_IS_ERR(deg_vec))  ray_release(deg_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata   = (int64_t*)ray_data(node_vec);
    int64_t* in_data = (int64_t*)ray_data(in_vec);
    int64_t* out_data= (int64_t*)ray_data(out_vec);
    int64_t* deg_data= (int64_t*)ray_data(deg_vec);

    for (int64_t i = 0; i < n; i++) {
        ndata[i]    = i;
        out_data[i] = fwd_off[i + 1] - fwd_off[i];
        in_data[i]  = rev_off[i + 1] - rev_off[i];
        deg_data[i] = out_data[i] + in_data[i];
    }
    node_vec->len = n;
    in_vec->len   = n;
    out_vec->len  = n;
    deg_vec->len  = n;

    ray_t* result = ray_table_new(4);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec); ray_release(in_vec);
        ray_release(out_vec);  ray_release(deg_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_in_degree", 10), in_vec);
    ray_release(in_vec);
    result = ray_table_add_col(result, sym_intern_safe("_out_degree", 11), out_vec);
    ray_release(out_vec);
    result = ray_table_add_col(result, sym_intern_safe("_degree", 7), deg_vec);
    ray_release(deg_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_topsort: topological sort via Kahn's algorithm. O(n+m).
 * Returns error if graph contains a cycle.
 * -------------------------------------------------------------------------- */
ray_t* exec_topsort(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    int64_t* in_deg = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* queue  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* order  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!in_deg || !queue || !order) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    /* Compute in-degrees from reverse CSR */
    for (int64_t i = 0; i < n; i++)
        in_deg[i] = rev_off[i + 1] - rev_off[i];

    /* Enqueue zero-degree nodes */
    int64_t head = 0, tail = 0;
    for (int64_t i = 0; i < n; i++) {
        if (in_deg[i] == 0) queue[tail++] = i;
    }

    /* BFS — decrement in-degrees, enqueue new zeros */
    int64_t count = 0;
    while (head < tail) {
        int64_t v = queue[head++];
        order[v] = count++;

        int64_t start = fwd_off[v];
        int64_t end   = fwd_off[v + 1];
        for (int64_t j = start; j < end; j++) {
            int64_t u = fwd_tgt[j];
            if (--in_deg[u] == 0) queue[tail++] = u;
        }
    }

    /* Cycle detection: not all nodes processed */
    if (count < n) {
        ray_scratch_arena_reset(&arena);
        return ray_error("domain", NULL);  /* cycle detected */
    }

    /* Build result */
    ray_t* node_vec  = ray_vec_new(RAY_I64, n);
    ray_t* order_vec = ray_vec_new(RAY_I64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !order_vec || RAY_IS_ERR(order_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (order_vec && !RAY_IS_ERR(order_vec)) ray_release(order_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    int64_t* odata = (int64_t*)ray_data(order_vec);
    for (int64_t i = 0; i < n; i++) {
        ndata[i] = i;
        odata[i] = order[i];
    }
    node_vec->len  = n;
    order_vec->len = n;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec); ray_release(order_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_order", 6), order_vec);
    ray_release(order_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_cluster_coeff: clustering coefficient via triangle counting. O(n*d^2).
 * For each node v, count triangles among undirected neighbors using bitset.
 * -------------------------------------------------------------------------- */
ray_t* exec_cluster_coeff(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    /* Scratch: merged neighbor list per node (max possible size = n) */
    int64_t* nbrs = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    /* Scratch: quick-lookup set for neighbor checking */
    uint8_t* in_nbr = (uint8_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(uint8_t));
    if (!nbrs || !in_nbr) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }
    memset(in_nbr, 0, (size_t)n * sizeof(uint8_t));

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    /* Allocate result vectors */
    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* lcc_vec  = ray_vec_new(RAY_F64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !lcc_vec || RAY_IS_ERR(lcc_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (lcc_vec  && !RAY_IS_ERR(lcc_vec))  ray_release(lcc_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata = (int64_t*)ray_data(node_vec);
    double*  ldata = (double*)ray_data(lcc_vec);

    for (int64_t v = 0; v < n; v++) {
        ndata[v] = v;

        /* Merge forward and reverse neighbors into deduplicated list */
        int64_t deg = 0;
        for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
            int64_t u = fwd_tgt[j];
            if (u >= 0 && u < n && !in_nbr[u]) {
                in_nbr[u] = 1;
                nbrs[deg++] = u;
            }
        }
        for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
            int64_t u = rev_tgt[j];
            if (u >= 0 && u < n && !in_nbr[u]) {
                in_nbr[u] = 1;
                nbrs[deg++] = u;
            }
        }

        if (deg < 2) {
            ldata[v] = 0.0;
        } else {
            /* Count directed fwd edges between neighbors of v */
            int64_t triangles = 0;
            for (int64_t i = 0; i < deg; i++) {
                int64_t u = nbrs[i];
                /* Check fwd edges of u against neighbor set */
                for (int64_t j = fwd_off[u]; j < fwd_off[u + 1]; j++) {
                    if (in_nbr[fwd_tgt[j]]) triangles++;
                }
            }
            ldata[v] = (double)triangles / ((double)deg * (double)(deg - 1));
        }

        /* Reset in_nbr for next node */
        for (int64_t i = 0; i < deg; i++) in_nbr[nbrs[i]] = 0;
    }

    node_vec->len = n;
    lcc_vec->len  = n;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(lcc_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_coefficient", 12), lcc_vec);
    ray_release(lcc_vec);

    return result;
}

/* --------------------------------------------------------------------------
 * exec_betweenness: Brandes betweenness centrality. O(n*m) exact,
 * O(sample*m) approximate when sample_size > 0.
 * -------------------------------------------------------------------------- */
ray_t* exec_betweenness(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);
    uint16_t sample = ext->graph.max_iter;
    int64_t n_sources = (sample > 0 && (int64_t)sample < n) ? (int64_t)sample : n;

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    double*  cb      = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    double*  sigma   = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    double*  delta   = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    int64_t* dist    = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* queue   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* stack   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));

    /* Predecessor storage: flat CSR-style array with per-node offsets.
     * Two-pass approach: BFS counts predecessors per node, prefix-sum builds
     * offsets, then a second pass over the stack fills pred_data in grouped order. */
    int64_t m_total = rel->fwd.n_edges + rel->rev.n_edges;
    if (m_total == 0) m_total = 1;
    int64_t* pred_data   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)m_total * sizeof(int64_t));
    int64_t* pred_off    = (int64_t*)ray_scratch_arena_push(&arena, (size_t)(n + 1) * sizeof(int64_t));
    int64_t* pred_cursor = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    /* Per-v dedup marker: tracks which neighbors were already counted via fwd edges
     * to avoid double-counting sigma/predecessors for bidirectional edges. */
    int64_t* seen_epoch  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));

    if (!cb || !sigma || !delta || !dist || !queue || !stack ||
        !pred_data || !pred_off || !pred_cursor || !seen_epoch) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    memset(cb, 0, (size_t)n * sizeof(double));

    int64_t stride = (sample > 0 && (int64_t)sample < n) ? (n / n_sources) : 1;

    for (int64_t si = 0; si < n_sources; si++) {
        int64_t s = (si * stride) % n;

        /* Initialize */
        for (int64_t i = 0; i < n; i++) {
            sigma[i] = 0.0;
            delta[i] = 0.0;
            dist[i]  = -1;
        }
        sigma[s] = 1.0;
        dist[s]  = 0;
        memset(pred_off, 0, (size_t)(n + 1) * sizeof(int64_t));
        memset(seen_epoch, 0, (size_t)n * sizeof(int64_t));

        /* BFS pass 1: discover nodes, compute sigma, count predecessors */
        int64_t q_head = 0, q_tail = 0;
        int64_t stack_top = 0;
        queue[q_tail++] = s;

        /* Use epoch counter to deduplicate: for each v popped from queue,
         * mark forward neighbors with epoch, then skip reverse neighbors
         * already marked (bidirectional edges). Epoch increments per v. */
        int64_t epoch = 0;
        while (q_head < q_tail) {
            int64_t v = queue[q_head++];
            stack[stack_top++] = v;
            epoch++;

            /* Forward neighbors */
            for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
                int64_t w = fwd_tgt[j];
                if (dist[w] < 0) {
                    dist[w] = dist[v] + 1;
                    queue[q_tail++] = w;
                }
                if (dist[w] == dist[v] + 1) {
                    sigma[w] += sigma[v];
                    pred_off[w + 1]++;
                    seen_epoch[w] = epoch;  /* mark w as counted for this v */
                }
            }
            /* Reverse neighbors (undirected), skip if already counted via fwd */
            for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
                int64_t w = rev_tgt[j];
                if (dist[w] < 0) {
                    dist[w] = dist[v] + 1;
                    queue[q_tail++] = w;
                }
                if (dist[w] == dist[v] + 1 && seen_epoch[w] != epoch) {
                    sigma[w] += sigma[v];
                    pred_off[w + 1]++;
                }
            }
        }

        /* Convert pred_off counts to cumulative offsets */
        for (int64_t i = 1; i <= n; i++)
            pred_off[i] += pred_off[i - 1];

        /* BFS pass 2: fill pred_data grouped by target node using write cursors.
         * Same dedup logic as pass 1 to avoid duplicate predecessor entries. */
        for (int64_t i = 0; i < n; i++) pred_cursor[i] = pred_off[i];
        epoch = 0;
        for (int64_t si2 = 0; si2 < stack_top; si2++) {
            int64_t v = stack[si2];
            epoch++;
            for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
                int64_t w = fwd_tgt[j];
                if (dist[w] == dist[v] + 1) {
                    pred_data[pred_cursor[w]++] = v;
                    seen_epoch[w] = epoch;
                }
            }
            for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
                int64_t w = rev_tgt[j];
                if (dist[w] == dist[v] + 1 && seen_epoch[w] != epoch)
                    pred_data[pred_cursor[w]++] = v;
            }
        }

        /* Back-propagation of dependencies */
        while (stack_top > 0) {
            int64_t w = stack[--stack_top];
            for (int64_t pi = pred_off[w]; pi < pred_off[w + 1]; pi++) {
                int64_t v = pred_data[pi];
                delta[v] += (sigma[v] / sigma[w]) * (1.0 + delta[w]);
            }
            if (w != s) cb[w] += delta[w];
        }
    }

    /* Undirected normalization: BFS from each source counts every unordered
     * pair {s,t} twice (once as source=s, once as source=t), so halve. */
    for (int64_t i = 0; i < n; i++) cb[i] /= 2.0;

    /* Normalize if sampled */
    if (sample > 0 && (int64_t)sample < n) {
        double scale = (double)n / (double)sample;
        for (int64_t i = 0; i < n; i++) cb[i] *= scale;
    }

    /* Build result table */
    ray_t* node_vec = ray_vec_new(RAY_I64, n);
    ray_t* cent_vec = ray_vec_new(RAY_F64, n);
    if (!node_vec || RAY_IS_ERR(node_vec) || !cent_vec || RAY_IS_ERR(cent_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (cent_vec && !RAY_IS_ERR(cent_vec)) ray_release(cent_vec);
        return ray_error("oom", NULL);
    }
    int64_t* ndata = (int64_t*)ray_data(node_vec);
    double*  cdata = (double*)ray_data(cent_vec);
    for (int64_t i = 0; i < n; i++) { ndata[i] = i; cdata[i] = cb[i]; }
    node_vec->len = n;
    cent_vec->len = n;
    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec); ray_release(cent_vec);
        return ray_error("oom", NULL);
    }
    ray_t* tmp = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(result); ray_release(cent_vec); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, sym_intern_safe("_centrality", 11), cent_vec);
    ray_release(cent_vec);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    return result;
}

/* --------------------------------------------------------------------------
 * exec_closeness: closeness centrality via BFS distance sums.
 * closeness[v] = reachable / sum_dist[v]. O(n*m) exact,
 * O(sample*m) approximate when sample_size > 0.
 * -------------------------------------------------------------------------- */
ray_t* exec_closeness(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    if (n <= 0) return ray_error("length", NULL);
    uint16_t sample = ext->graph.max_iter;
    int64_t n_sources = (sample > 0 && (int64_t)sample < n) ? (int64_t)sample : n;

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* rev_off = (int64_t*)ray_data(rel->rev.offsets);
    int64_t* rev_tgt = (int64_t*)ray_data(rel->rev.targets);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    double*  closeness = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    int64_t* dist      = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* queue     = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));

    if (!closeness || !dist || !queue) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    memset(closeness, 0, (size_t)n * sizeof(double));

    int64_t stride = (sample > 0 && (int64_t)sample < n) ? (n / n_sources) : 1;

    for (int64_t si = 0; si < n_sources; si++) {
        int64_t s = (si * stride) % n;

        /* Initialize distances */
        for (int64_t i = 0; i < n; i++) dist[i] = -1;
        dist[s] = 0;

        /* BFS from s */
        int64_t q_head = 0, q_tail = 0;
        queue[q_tail++] = s;

        while (q_head < q_tail) {
            int64_t v = queue[q_head++];

            /* Forward neighbors */
            for (int64_t j = fwd_off[v]; j < fwd_off[v + 1]; j++) {
                int64_t w = fwd_tgt[j];
                if (dist[w] < 0) {
                    dist[w] = dist[v] + 1;
                    queue[q_tail++] = w;
                }
            }
            /* Reverse neighbors (undirected) */
            for (int64_t j = rev_off[v]; j < rev_off[v + 1]; j++) {
                int64_t w = rev_tgt[j];
                if (dist[w] < 0) {
                    dist[w] = dist[v] + 1;
                    queue[q_tail++] = w;
                }
            }
        }

        /* Sum distances and count reachable nodes */
        int64_t sum_dist = 0;
        int64_t reachable = 0;
        for (int64_t i = 0; i < n; i++) {
            if (dist[i] > 0) {
                sum_dist += dist[i];
                reachable++;
            }
        }

        if (reachable > 0 && sum_dist > 0) {
            closeness[s] = (double)reachable / (double)sum_dist;
        }
    }

    /* Build result table: when sampling, only emit computed nodes */
    int64_t n_out = n_sources;
    ray_t* node_vec = ray_vec_new(RAY_I64, n_out);
    ray_t* cent_vec = ray_vec_new(RAY_F64, n_out);
    if (!node_vec || RAY_IS_ERR(node_vec) || !cent_vec || RAY_IS_ERR(cent_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (cent_vec && !RAY_IS_ERR(cent_vec)) ray_release(cent_vec);
        return ray_error("oom", NULL);
    }
    int64_t* ndata = (int64_t*)ray_data(node_vec);
    double*  cdata = (double*)ray_data(cent_vec);
    if (n_sources == n) {
        for (int64_t i = 0; i < n; i++) { ndata[i] = i; cdata[i] = closeness[i]; }
    } else {
        for (int64_t si = 0; si < n_sources; si++) {
            int64_t s = (si * stride) % n;
            ndata[si] = s;
            cdata[si] = closeness[s];
        }
    }
    node_vec->len = n_out;
    cent_vec->len = n_out;
    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec); ray_release(cent_vec);
        return ray_error("oom", NULL);
    }
    ray_t* tmp = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(result); ray_release(cent_vec); return ray_error("oom", NULL); }
    result = tmp;
    tmp = ray_table_add_col(result, sym_intern_safe("_centrality", 11), cent_vec);
    ray_release(cent_vec);
    if (!tmp || RAY_IS_ERR(tmp)) { ray_release(result); return ray_error("oom", NULL); }
    result = tmp;
    return result;
}

/* --------------------------------------------------------------------------
 * exec_mst: Minimum Spanning Tree / Forest via Kruskal's algorithm.
 * Collects weighted edges from forward CSR, sorts by weight, builds MST
 * using union-find with path compression and union by rank.
 * -------------------------------------------------------------------------- */
typedef struct { double w; int64_t src; int64_t dst; } mst_edge_t;

static int mst_edge_cmp(const void* a, const void* b) {
    double da = ((const mst_edge_t*)a)->w;
    double db = ((const mst_edge_t*)b)->w;
    return (da > db) - (da < db);
}

static int64_t uf_find(int64_t* parent, int64_t x) {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
}

static bool uf_union(int64_t* parent, int64_t* rank_arr, int64_t a, int64_t b) {
    a = uf_find(parent, a); b = uf_find(parent, b);
    if (a == b) return false;
    if (rank_arr[a] < rank_arr[b]) { int64_t tmp = a; a = b; b = tmp; }
    parent[b] = a;
    if (rank_arr[a] == rank_arr[b]) rank_arr[a]++;
    return true;
}

ray_t* exec_mst(ray_graph_t* g, ray_op_t* op) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel || !rel->fwd.props) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    int64_t m = rel->fwd.n_edges;
    if (n <= 0) return ray_error("length", NULL);

    int64_t weight_sym = ext->graph.weight_col_sym;
    ray_t* weight_vec = ray_table_get_col(rel->fwd.props, weight_sym);
    if (!weight_vec || weight_vec->type != RAY_F64) return ray_error("schema", NULL);
    double* weights = (double*)ray_data(weight_vec);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* fwd_row = (int64_t*)ray_data(rel->fwd.rowmap);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    mst_edge_t* edges_arr = (mst_edge_t*)ray_scratch_arena_push(&arena,
                                (size_t)m * sizeof(mst_edge_t));
    int64_t* uf_parent = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* uf_rank   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!edges_arr || !uf_parent || !uf_rank) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    /* Fill edge array from forward CSR */
    int64_t ei = 0;
    for (int64_t u = 0; u < n; u++) {
        for (int64_t j = fwd_off[u]; j < fwd_off[u + 1]; j++) {
            edges_arr[ei].src = u;
            edges_arr[ei].dst = fwd_tgt[j];
            edges_arr[ei].w   = weights[fwd_row[j]];
            ei++;
        }
    }

    /* Sort edges by weight */
    qsort(edges_arr, (size_t)ei, sizeof(mst_edge_t), mst_edge_cmp);

    /* Initialize union-find */
    for (int64_t i = 0; i < n; i++) { uf_parent[i] = i; uf_rank[i] = 0; }

    /* Build MST */
    int64_t max_mst = n - 1;
    int64_t mst_count = 0;
    ray_t* src_vec = ray_vec_new(RAY_I64, max_mst);
    ray_t* dst_vec = ray_vec_new(RAY_I64, max_mst);
    ray_t* wt_vec  = ray_vec_new(RAY_F64, max_mst);
    if (!src_vec || RAY_IS_ERR(src_vec) ||
        !dst_vec || RAY_IS_ERR(dst_vec) ||
        !wt_vec  || RAY_IS_ERR(wt_vec)) {
        ray_scratch_arena_reset(&arena);
        if (src_vec && !RAY_IS_ERR(src_vec)) ray_release(src_vec);
        if (dst_vec && !RAY_IS_ERR(dst_vec)) ray_release(dst_vec);
        if (wt_vec  && !RAY_IS_ERR(wt_vec))  ray_release(wt_vec);
        return ray_error("oom", NULL);
    }

    int64_t* sdata = (int64_t*)ray_data(src_vec);
    int64_t* ddata = (int64_t*)ray_data(dst_vec);
    double*  wdata = (double*)ray_data(wt_vec);

    for (int64_t i = 0; i < ei && mst_count < max_mst; i++) {
        if (uf_union(uf_parent, uf_rank, edges_arr[i].src, edges_arr[i].dst)) {
            sdata[mst_count] = edges_arr[i].src;
            ddata[mst_count] = edges_arr[i].dst;
            wdata[mst_count] = edges_arr[i].w;
            mst_count++;
        }
    }

    src_vec->len = mst_count;
    dst_vec->len = mst_count;
    wt_vec->len  = mst_count;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(src_vec); ray_release(dst_vec); ray_release(wt_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_src", 4), src_vec);
    ray_release(src_vec);
    result = ray_table_add_col(result, sym_intern_safe("_dst", 4), dst_vec);
    ray_release(dst_vec);
    result = ray_table_add_col(result, sym_intern_safe("_weight", 7), wt_vec);
    ray_release(wt_vec);
    return result;
}

/* --------------------------------------------------------------------------
 * exec_random_walk: random walk from source node using xorshift64 PRNG.
 * -------------------------------------------------------------------------- */
ray_t* exec_random_walk(ray_graph_t* g, ray_op_t* op, ray_t* src_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);
    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    uint16_t walk_len = ext->graph.max_iter;
    if (n <= 0) return ray_error("length", NULL);

    int64_t start_node;
    if (ray_is_atom(src_val)) {
        start_node = src_val->i64;
    } else {
        start_node = ((int64_t*)ray_data(src_val))[0];
    }
    if (start_node < 0 || start_node >= n) return ray_error("range", NULL);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);

    int64_t total = (int64_t)walk_len + 1;
    ray_t* step_vec = ray_vec_new(RAY_I64, total);
    ray_t* node_vec = ray_vec_new(RAY_I64, total);
    if (!step_vec || RAY_IS_ERR(step_vec) || !node_vec || RAY_IS_ERR(node_vec)) {
        if (step_vec && !RAY_IS_ERR(step_vec)) ray_release(step_vec);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        return ray_error("oom", NULL);
    }

    int64_t* sdata = (int64_t*)ray_data(step_vec);
    int64_t* ndata = (int64_t*)ray_data(node_vec);

    /* xorshift64 PRNG seeded from source node */
    uint64_t rng = (uint64_t)start_node * 6364136223846793005ULL + 1442695040888963407ULL;
    if (rng == 0) rng = 1;

    int64_t current = start_node;
    int64_t count = 0;
    for (int64_t i = 0; i < total; i++) {
        sdata[i] = i;
        ndata[i] = current;
        count++;
        if (i < walk_len) {
            int64_t deg = fwd_off[current + 1] - fwd_off[current];
            if (deg == 0) break;  /* dead end */
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            int64_t pick = (int64_t)(rng % (uint64_t)deg);
            current = fwd_tgt[fwd_off[current] + pick];
        }
    }

    step_vec->len = count;
    node_vec->len = count;

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(step_vec); ray_release(node_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_step", 5), step_vec);
    ray_release(step_vec);
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    return result;
}

/* --------------------------------------------------------------------------
 * exec_dfs: depth-first search from source node. O(n+m).
 * -------------------------------------------------------------------------- */
ray_t* exec_dfs(ray_graph_t* g, ray_op_t* op, ray_t* src_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    uint8_t max_depth = ext->graph.max_depth;
    if (n <= 0) return ray_error("length", NULL);

    /* Get source node ID */
    int64_t start_node;
    if (ray_is_atom(src_val)) {
        start_node = src_val->i64;
    } else {
        start_node = ((int64_t*)ray_data(src_val))[0];
    }
    if (start_node < 0 || start_node >= n) return ray_error("range", NULL);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    /* Stack can hold up to m entries (one per edge traversal) */
    int64_t m = rel->fwd.n_edges;
    int64_t stack_cap = m > n ? m + 1 : n + 1;

    int64_t* stack_node   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)stack_cap * sizeof(int64_t));
    int64_t* stack_depth  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)stack_cap * sizeof(int64_t));
    int64_t* stack_parent = (int64_t*)ray_scratch_arena_push(&arena, (size_t)stack_cap * sizeof(int64_t));
    uint8_t* visited      = (uint8_t*)ray_scratch_arena_push(&arena, (size_t)n);
    int64_t* res_node     = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* res_depth    = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    int64_t* res_parent   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    if (!stack_node || !stack_depth || !stack_parent || !visited ||
        !res_node || !res_depth || !res_parent) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    memset(visited, 0, (size_t)n);

    /* Push source */
    int64_t sp = 0;
    stack_node[sp]   = start_node;
    stack_depth[sp]  = 0;
    stack_parent[sp] = -1;
    sp++;

    int64_t count = 0;

    while (sp > 0) {
        sp--;
        int64_t v = stack_node[sp];
        int64_t d = stack_depth[sp];
        int64_t p = stack_parent[sp];

        if (visited[v]) continue;
        visited[v] = 1;

        res_node[count]   = v;
        res_depth[count]  = d;
        res_parent[count] = p;
        count++;

        if (d < max_depth) {
            /* Push neighbors in reverse order so first neighbor is visited first */
            int64_t start = fwd_off[v];
            int64_t end   = fwd_off[v + 1];
            for (int64_t j = end - 1; j >= start; j--) {
                int64_t u = fwd_tgt[j];
                if (!visited[u]) {
                    stack_node[sp]   = u;
                    stack_depth[sp]  = d + 1;
                    stack_parent[sp] = v;
                    sp++;
                }
            }
        }
    }

    /* Build result vectors */
    ray_t* node_vec   = ray_vec_new(RAY_I64, count);
    ray_t* depth_vec  = ray_vec_new(RAY_I64, count);
    ray_t* parent_vec = ray_vec_new(RAY_I64, count);
    if (!node_vec || RAY_IS_ERR(node_vec) ||
        !depth_vec || RAY_IS_ERR(depth_vec) ||
        !parent_vec || RAY_IS_ERR(parent_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (depth_vec && !RAY_IS_ERR(depth_vec)) ray_release(depth_vec);
        if (parent_vec && !RAY_IS_ERR(parent_vec)) ray_release(parent_vec);
        return ray_error("oom", NULL);
    }

    memcpy(ray_data(node_vec),   res_node,   (size_t)count * sizeof(int64_t));
    memcpy(ray_data(depth_vec),  res_depth,  (size_t)count * sizeof(int64_t));
    memcpy(ray_data(parent_vec), res_parent, (size_t)count * sizeof(int64_t));
    node_vec->len   = count;
    depth_vec->len  = count;
    parent_vec->len = count;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec); ray_release(depth_vec); ray_release(parent_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_depth", 6), depth_vec);
    ray_release(depth_vec);
    result = ray_table_add_col(result, sym_intern_safe("_parent", 7), parent_vec);
    ray_release(parent_vec);

    return result;
}

/* exec_astar: A* shortest path with Euclidean coordinate heuristic */
ray_t* exec_astar(ray_graph_t* g, ray_op_t* op,
                         ray_t* src_val, ray_t* dst_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel) return ray_error("schema", NULL);
    if (!rel->fwd.props) return ray_error("schema", NULL);

    ray_t* np = (ray_t*)ext->graph.node_props;
    if (!np) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    int64_t m = rel->fwd.n_edges;
    int64_t src_id = src_val->i64;
    int64_t dst_id = dst_val->i64;

    if (src_id < 0 || src_id >= n) return ray_error("range", NULL);
    if (dst_id < 0 || dst_id >= n) return ray_error("range", NULL);

    /* Resolve weight column from edge properties */
    int64_t weight_sym = ext->graph.weight_col_sym;
    ray_t* weight_vec = ray_table_get_col(rel->fwd.props, weight_sym);
    if (!weight_vec || RAY_IS_ERR(weight_vec)) return ray_error("schema", NULL);
    double* weights_arr = (double*)ray_data(weight_vec);

    /* Resolve coordinate columns from node properties */
    ray_t* lat_vec = ray_table_get_col(np, ext->graph.coord_col_syms[0]);
    ray_t* lon_vec = ray_table_get_col(np, ext->graph.coord_col_syms[1]);
    if (!lat_vec || !lon_vec) return ray_error("schema", NULL);
    double* lat = (double*)ray_data(lat_vec);
    double* lon = (double*)ray_data(lon_vec);

    int64_t heap_cap = (m > n ? m : n) + 1;

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    double*  dist_a    = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    bool*    visited = (bool*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(bool));
    int64_t* depth_a   = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    dijk_entry_t* heap = (dijk_entry_t*)ray_scratch_arena_push(&arena,
                              (size_t)heap_cap * sizeof(dijk_entry_t));
    if (!dist_a || !visited || !depth_a || !heap) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }
    memset(visited, 0, (size_t)n * sizeof(bool));
    memset(depth_a, 0, (size_t)n * sizeof(int64_t));

    for (int64_t i = 0; i < n; i++) dist_a[i] = 1e308;
    dist_a[src_id] = 0.0;

    /* A* uses f = g + h; heap stores f-cost for priority ordering */
    double dx = lat[src_id] - lat[dst_id];
    double dy = lon[src_id] - lon[dst_id];
    double h0 = sqrt(dx * dx + dy * dy);
    int64_t heap_size = 0;
    dijk_heap_push(heap, &heap_size, h0, src_id);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* fwd_row = (int64_t*)ray_data(rel->fwd.rowmap);

    while (heap_size > 0) {
        dijk_entry_t top = dijk_heap_pop(heap, &heap_size);
        int64_t u = top.node;
        if (visited[u]) continue;
        visited[u] = true;

        if (u == dst_id) break;

        for (int64_t j = fwd_off[u]; j < fwd_off[u + 1]; j++) {
            int64_t v = fwd_tgt[j];
            int64_t edge_row = fwd_row[j];
            double w = weights_arr[edge_row];
            double new_dist = dist_a[u] + w;
            if (new_dist < dist_a[v]) {
                dist_a[v] = new_dist;
                depth_a[v] = depth_a[u] + 1;
                /* f = g + h (Euclidean heuristic) */
                double hdx = lat[v] - lat[dst_id];
                double hdy = lon[v] - lon[dst_id];
                double hv = sqrt(hdx * hdx + hdy * hdy);
                dijk_heap_push(heap, &heap_size, new_dist + hv, v);
            }
        }
    }

    /* Collect reachable nodes */
    int64_t acount = 0;
    for (int64_t i = 0; i < n; i++) {
        if (dist_a[i] < 1e308) acount++;
    }

    ray_t* node_vec  = ray_vec_new(RAY_I64, acount);
    ray_t* dist_vec  = ray_vec_new(RAY_F64, acount);
    ray_t* depth_vec = ray_vec_new(RAY_I64, acount);
    if (!node_vec || RAY_IS_ERR(node_vec) ||
        !dist_vec || RAY_IS_ERR(dist_vec) ||
        !depth_vec || RAY_IS_ERR(depth_vec)) {
        ray_scratch_arena_reset(&arena);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (dist_vec && !RAY_IS_ERR(dist_vec)) ray_release(dist_vec);
        if (depth_vec && !RAY_IS_ERR(depth_vec)) ray_release(depth_vec);
        return ray_error("oom", NULL);
    }

    int64_t* ndata_a = (int64_t*)ray_data(node_vec);
    double*  ddata_a = (double*)ray_data(dist_vec);
    int64_t* hdata_a = (int64_t*)ray_data(depth_vec);
    int64_t idx = 0;
    for (int64_t i = 0; i < n; i++) {
        if (dist_a[i] < 1e308) {
            ndata_a[idx] = i;
            ddata_a[idx] = dist_a[i];
            hdata_a[idx] = depth_a[i];
            idx++;
        }
    }
    node_vec->len = acount;
    dist_vec->len = acount;
    depth_vec->len = acount;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(node_vec);
        ray_release(dist_vec);
        ray_release(depth_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_dist", 5), dist_vec);
    ray_release(dist_vec);
    result = ray_table_add_col(result, sym_intern_safe("_depth", 6), depth_vec);
    ray_release(depth_vec);

    return result;
}

/* exec_k_shortest: Yen's k-shortest paths via iterative masked Dijkstra */
ray_t* exec_k_shortest(ray_graph_t* g, ray_op_t* op,
                               ray_t* src_val, ray_t* dst_val) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    ray_rel_t* rel = (ray_rel_t*)ext->graph.rel;
    if (!rel || !rel->fwd.props) return ray_error("schema", NULL);

    int64_t n = rel->fwd.n_nodes;
    int64_t m = rel->fwd.n_edges;
    int64_t src_id = src_val->i64;
    int64_t dst_id = dst_val->i64;
    uint16_t K = ext->graph.max_iter;

    if (src_id < 0 || src_id >= n || dst_id < 0 || dst_id >= n)
        return ray_error("range", NULL);

    int64_t weight_sym = ext->graph.weight_col_sym;
    ray_t* weight_vec = ray_table_get_col(rel->fwd.props, weight_sym);
    if (!weight_vec || RAY_IS_ERR(weight_vec)) return ray_error("schema", NULL);
    double* weights_k = (double*)ray_data(weight_vec);

    int64_t* fwd_off = (int64_t*)ray_data(rel->fwd.offsets);
    int64_t* fwd_tgt = (int64_t*)ray_data(rel->fwd.targets);
    int64_t* fwd_row = (int64_t*)ray_data(rel->fwd.rowmap);

    int64_t heap_cap = (m > n ? m : n) + 1;

    ray_scratch_arena_t arena;
    ray_scratch_arena_init(&arena);

    /* Dijkstra working arrays */
    double*       dist_arr  = (double*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(double));
    int64_t*      parent    = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));
    bool*         vis       = (bool*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(bool));
    dijk_entry_t* heap      = (dijk_entry_t*)ray_scratch_arena_push(&arena,
                                    (size_t)heap_cap * sizeof(dijk_entry_t));
    bool*         node_mask = (bool*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(bool));
    bool*         edge_mask = (bool*)ray_scratch_arena_push(&arena, (size_t)m * sizeof(bool));

    /* Path storage: K paths, each up to n nodes */
    int64_t* paths_data = (int64_t*)ray_scratch_arena_push(&arena, (size_t)K * (size_t)n * sizeof(int64_t));
    int64_t* path_lens  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)K * sizeof(int64_t));
    double*  path_costs = (double*)ray_scratch_arena_push(&arena, (size_t)K * sizeof(double));

    /* Candidate storage */
    int64_t max_cand = (int64_t)K * n;
    if (max_cand > 4096) max_cand = 4096;
    int64_t* cand_data  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)max_cand * (size_t)n * sizeof(int64_t));
    int64_t* cand_lens  = (int64_t*)ray_scratch_arena_push(&arena, (size_t)max_cand * sizeof(int64_t));
    double*  cand_costs = (double*)ray_scratch_arena_push(&arena, (size_t)max_cand * sizeof(double));

    /* Temp buffer for path reconstruction */
    int64_t* tmp_path = (int64_t*)ray_scratch_arena_push(&arena, (size_t)n * sizeof(int64_t));

    if (!dist_arr || !parent || !vis || !heap || !node_mask || !edge_mask ||
        !paths_data || !path_lens || !path_costs ||
        !cand_data || !cand_lens || !cand_costs || !tmp_path) {
        ray_scratch_arena_reset(&arena);
        return ray_error("oom", NULL);
    }

    int64_t num_found = 0;
    int64_t num_cand  = 0;

    /* Step 1: Find shortest path P[0] */
    double d = dijkstra_masked(fwd_off, fwd_tgt, fwd_row, weights_k, n,
                                src_id, dst_id, NULL, NULL,
                                dist_arr, parent, heap, vis);

    if (d >= 1e308) {
        ray_scratch_arena_reset(&arena);
        ray_t* nv = ray_vec_new(RAY_I64, 0); nv->len = 0;
        ray_t* dv = ray_vec_new(RAY_F64, 0); dv->len = 0;
        ray_t* pv = ray_vec_new(RAY_I64, 0); pv->len = 0;
        ray_t* result = ray_table_new(3);
        result = ray_table_add_col(result, sym_intern_safe("_path_id", 8), pv); ray_release(pv);
        result = ray_table_add_col(result, sym_intern_safe("_node", 5), nv); ray_release(nv);
        result = ray_table_add_col(result, sym_intern_safe("_dist", 5), dv); ray_release(dv);
        return result;
    }

    /* Reconstruct P[0] from parent array (reverse then flip) */
    int64_t plen = 0;
    for (int64_t v = dst_id; v != -1; v = parent[v]) {
        tmp_path[plen++] = v;
        if (plen > n) break;  /* safety: avoid infinite loop on corrupt parent */
    }
    for (int64_t i = 0; i < plen / 2; i++) {
        int64_t tmp = tmp_path[i];
        tmp_path[i] = tmp_path[plen - 1 - i];
        tmp_path[plen - 1 - i] = tmp;
    }

    memcpy(&paths_data[0], tmp_path, (size_t)plen * sizeof(int64_t));
    path_lens[0] = plen;
    path_costs[0] = d;
    num_found = 1;

    /* Step 2: Iteratively find paths P[1]..P[K-1] */
    for (uint16_t k = 1; k < K; k++) {
        int64_t* prev_path = &paths_data[(int64_t)(k - 1) * n];
        int64_t prev_len = path_lens[k - 1];

        for (int64_t i = 0; i < prev_len - 1; i++) {
            int64_t spur_node = prev_path[i];

            /* Compute root path cost */
            double root_cost = 0.0;
            for (int64_t r = 0; r < i; r++) {
                int64_t from = prev_path[r];
                int64_t to   = prev_path[r + 1];
                for (int64_t e = fwd_off[from]; e < fwd_off[from + 1]; e++) {
                    if (fwd_tgt[e] == to) {
                        root_cost += weights_k[fwd_row[e]];
                        break;
                    }
                }
            }

            /* Mask edges used by found paths sharing the root prefix */
            memset(edge_mask, 0, (size_t)m * sizeof(bool));
            memset(node_mask, 0, (size_t)n * sizeof(bool));

            for (int64_t j = 0; j < num_found; j++) {
                int64_t* pj = &paths_data[j * n];
                int64_t pj_len = path_lens[j];
                if (pj_len <= i) continue;

                bool same_prefix = true;
                for (int64_t r = 0; r <= i; r++) {
                    if (pj[r] != prev_path[r]) { same_prefix = false; break; }
                }
                if (!same_prefix) continue;

                int64_t from = pj[i];
                int64_t to   = pj[i + 1];
                for (int64_t e = fwd_off[from]; e < fwd_off[from + 1]; e++) {
                    if (fwd_tgt[e] == to) { edge_mask[e] = true; break; }
                }
            }

            /* Mask root path nodes except spur node */
            for (int64_t r = 0; r < i; r++) {
                node_mask[prev_path[r]] = true;
            }

            /* Dijkstra from spur to dst with masks */
            double spur_dist = dijkstra_masked(fwd_off, fwd_tgt, fwd_row, weights_k, n,
                                                spur_node, dst_id, node_mask, edge_mask,
                                                dist_arr, parent, heap, vis);
            if (spur_dist >= 1e308) continue;

            /* Reconstruct spur path */
            int64_t spur_len = 0;
            for (int64_t v = dst_id; v != -1; v = parent[v]) {
                tmp_path[spur_len++] = v;
                if (spur_len > n) break;
            }
            for (int64_t a = 0; a < spur_len / 2; a++) {
                int64_t tmp = tmp_path[a];
                tmp_path[a] = tmp_path[spur_len - 1 - a];
                tmp_path[spur_len - 1 - a] = tmp;
            }

            double total_cost = root_cost + spur_dist;
            int64_t total_len = i + spur_len;
            if (total_len > n || num_cand >= max_cand) continue;

            /* Check for duplicate candidates */
            bool dup = false;
            for (int64_t c = 0; c < num_cand && !dup; c++) {
                if (cand_lens[c] != total_len) continue;
                bool same = true;
                int64_t* cp = &cand_data[c * n];
                for (int64_t r = 0; r < i && same; r++) {
                    if (cp[r] != prev_path[r]) same = false;
                }
                for (int64_t r = 0; r < spur_len && same; r++) {
                    if (cp[i + r] != tmp_path[r]) same = false;
                }
                if (same) dup = true;
            }
            /* Check against already-found paths */
            for (int64_t f = 0; f < num_found && !dup; f++) {
                if (path_lens[f] != total_len) continue;
                bool same = true;
                int64_t* fp = &paths_data[f * n];
                for (int64_t r = 0; r < i && same; r++) {
                    if (fp[r] != prev_path[r]) same = false;
                }
                for (int64_t r = 0; r < spur_len && same; r++) {
                    if (fp[i + r] != tmp_path[r]) same = false;
                }
                if (same) dup = true;
            }
            if (dup) continue;

            /* Store candidate: root_path[0..i-1] + spur_path */
            int64_t* cp = &cand_data[num_cand * n];
            memcpy(cp, prev_path, (size_t)i * sizeof(int64_t));
            memcpy(cp + i, tmp_path, (size_t)spur_len * sizeof(int64_t));
            cand_lens[num_cand] = total_len;
            cand_costs[num_cand] = total_cost;
            num_cand++;
        }

        if (num_cand == 0) break;

        /* Pick cheapest candidate */
        int64_t best = 0;
        for (int64_t c = 1; c < num_cand; c++) {
            if (cand_costs[c] < cand_costs[best]) best = c;
        }

        memcpy(&paths_data[(int64_t)k * n], &cand_data[best * n],
               (size_t)cand_lens[best] * sizeof(int64_t));
        path_lens[k] = cand_lens[best];
        path_costs[k] = cand_costs[best];
        num_found++;

        /* Remove used candidate (swap with last) */
        if (best < num_cand - 1) {
            memcpy(&cand_data[best * n], &cand_data[(num_cand - 1) * n],
                   (size_t)cand_lens[num_cand - 1] * sizeof(int64_t));
            cand_lens[best] = cand_lens[num_cand - 1];
            cand_costs[best] = cand_costs[num_cand - 1];
        }
        num_cand--;
    }

    /* Build output: _path_id, _node, _dist (running dist along each path) */
    int64_t total_rows = 0;
    for (int64_t k = 0; k < num_found; k++) total_rows += path_lens[k];

    ray_t* pid_vec  = ray_vec_new(RAY_I64, total_rows);
    ray_t* node_vec = ray_vec_new(RAY_I64, total_rows);
    ray_t* dist_vec = ray_vec_new(RAY_F64, total_rows);
    if (!pid_vec  || RAY_IS_ERR(pid_vec) ||
        !node_vec || RAY_IS_ERR(node_vec) ||
        !dist_vec || RAY_IS_ERR(dist_vec)) {
        ray_scratch_arena_reset(&arena);
        if (pid_vec  && !RAY_IS_ERR(pid_vec))  ray_release(pid_vec);
        if (node_vec && !RAY_IS_ERR(node_vec)) ray_release(node_vec);
        if (dist_vec && !RAY_IS_ERR(dist_vec)) ray_release(dist_vec);
        return ray_error("oom", NULL);
    }

    int64_t* pids  = (int64_t*)ray_data(pid_vec);
    int64_t* nodes_k = (int64_t*)ray_data(node_vec);
    double*  dists = (double*)ray_data(dist_vec);

    int64_t row = 0;
    for (int64_t k = 0; k < num_found; k++) {
        int64_t* path = &paths_data[k * n];
        int64_t pk_len = path_lens[k];
        double running = 0.0;
        for (int64_t j = 0; j < pk_len; j++) {
            pids[row]  = k;
            nodes_k[row] = path[j];
            if (j > 0) {
                int64_t from = path[j - 1];
                int64_t to   = path[j];
                for (int64_t e = fwd_off[from]; e < fwd_off[from + 1]; e++) {
                    if (fwd_tgt[e] == to) {
                        running += weights_k[fwd_row[e]];
                        break;
                    }
                }
            }
            dists[row] = running;
            row++;
        }
    }

    pid_vec->len  = total_rows;
    node_vec->len = total_rows;
    dist_vec->len = total_rows;

    ray_scratch_arena_reset(&arena);

    ray_t* result = ray_table_new(3);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(pid_vec); ray_release(node_vec); ray_release(dist_vec);
        return ray_error("oom", NULL);
    }
    result = ray_table_add_col(result, sym_intern_safe("_path_id", 8), pid_vec);
    ray_release(pid_vec);
    result = ray_table_add_col(result, sym_intern_safe("_node", 5), node_vec);
    ray_release(node_vec);
    result = ray_table_add_col(result, sym_intern_safe("_dist", 5), dist_vec);
    ray_release(dist_vec);
    return result;
}
