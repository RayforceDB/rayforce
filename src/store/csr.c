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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

/* Forward declaration */
static void csr_free(ray_csr_t* csr);

/* --------------------------------------------------------------------------
 * CSR construction helpers
 * -------------------------------------------------------------------------- */

/* Pair for sorting edges */
typedef struct {
    int64_t src;
    int64_t dst;
    int64_t row;  /* original row index for rowmap */
} edge_pair_t;

/* Comparison by src then dst */
static int cmp_edge_by_src(const void* a, const void* b) {
    const edge_pair_t* ea = (const edge_pair_t*)a;
    const edge_pair_t* eb = (const edge_pair_t*)b;
    if (ea->src < eb->src) return -1;
    if (ea->src > eb->src) return 1;
    if (ea->dst < eb->dst) return -1;
    if (ea->dst > eb->dst) return 1;
    return 0;
}

/* Comparison by dst then src (for reverse CSR) */
static int cmp_edge_by_dst(const void* a, const void* b) {
    const edge_pair_t* ea = (const edge_pair_t*)a;
    const edge_pair_t* eb = (const edge_pair_t*)b;
    if (ea->dst < eb->dst) return -1;
    if (ea->dst > eb->dst) return 1;
    if (ea->src < eb->src) return -1;
    if (ea->src > eb->src) return 1;
    return 0;
}

/* Sort targets within each adjacency list (for LFTJ) */
static void csr_sort_adjacency_lists(ray_csr_t* csr) {
    int64_t* offsets = (int64_t*)ray_data(csr->offsets);
    int64_t* targets = (int64_t*)ray_data(csr->targets);
    int64_t* rowmap = csr->rowmap ? (int64_t*)ray_data(csr->rowmap) : NULL;

    for (int64_t node = 0; node < csr->n_nodes; node++) {
        int64_t start = offsets[node];
        int64_t end = offsets[node + 1];
        int64_t deg = end - start;
        if (deg <= 1) continue;

        /* Simple insertion sort — adjacency lists are typically small */
        for (int64_t i = start + 1; i < end; i++) {
            int64_t key = targets[i];
            int64_t row_key = rowmap ? rowmap[i] : 0;
            int64_t j = i - 1;
            while (j >= start && targets[j] > key) {
                targets[j + 1] = targets[j];
                if (rowmap) rowmap[j + 1] = rowmap[j];
                j--;
            }
            targets[j + 1] = key;
            if (rowmap) rowmap[j + 1] = row_key;
        }
    }
}

/* Build CSR from sorted edge pairs.
 * pairs must be sorted by the 'key' field (src for fwd, dst for rev). */
static ray_err_t csr_build_from_pairs(edge_pair_t* pairs, int64_t n_edges,
                                      int64_t n_nodes, bool is_reverse,
                                      bool sort_targets, ray_csr_t* out) {
    out->n_nodes = n_nodes;
    out->props = NULL;

    /* Count valid edges (those within [0, n_nodes) range) */
    int64_t valid_edges = 0;
    for (int64_t i = 0; i < n_edges; i++) {
        int64_t key = is_reverse ? pairs[i].dst : pairs[i].src;
        if (key >= 0 && key < n_nodes) valid_edges++;
    }
    out->n_edges = valid_edges;

    /* Allocate offsets (n_nodes + 1) */
    out->offsets = ray_vec_new(RAY_I64, n_nodes + 1);
    if (!out->offsets || RAY_IS_ERR(out->offsets)) return RAY_ERR_OOM;
    out->offsets->len = n_nodes + 1;
    int64_t* off = (int64_t*)ray_data(out->offsets);
    memset(off, 0, (size_t)(n_nodes + 1) * sizeof(int64_t));

    /* Allocate targets */
    out->targets = ray_vec_new(RAY_I64, valid_edges > 0 ? valid_edges : 1);
    if (!out->targets || RAY_IS_ERR(out->targets)) {
        ray_release(out->offsets); out->offsets = NULL;
        return RAY_ERR_OOM;
    }
    out->targets->len = valid_edges;
    int64_t* tgt = (int64_t*)ray_data(out->targets);

    /* Allocate rowmap */
    out->rowmap = ray_vec_new(RAY_I64, valid_edges > 0 ? valid_edges : 1);
    if (!out->rowmap || RAY_IS_ERR(out->rowmap)) {
        ray_release(out->offsets); out->offsets = NULL;
        ray_release(out->targets); out->targets = NULL;
        return RAY_ERR_OOM;
    }
    out->rowmap->len = valid_edges;
    int64_t* rmap = (int64_t*)ray_data(out->rowmap);

    /* Count degrees */
    for (int64_t i = 0; i < n_edges; i++) {
        int64_t key = is_reverse ? pairs[i].dst : pairs[i].src;
        if (key >= 0 && key < n_nodes) off[key + 1]++;
    }

    /* Prefix sum */
    for (int64_t i = 1; i <= n_nodes; i++)
        off[i] += off[i - 1];

    /* Fill targets + rowmap using a position array */
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
        int64_t key = is_reverse ? pairs[i].dst : pairs[i].src;
        int64_t val = is_reverse ? pairs[i].src : pairs[i].dst;
        if (key >= 0 && key < n_nodes) {
            int64_t p = pos[key]++;
            tgt[p] = val;
            rmap[p] = pairs[i].row;
        }
    }
    ray_free(pos_hdr);

    /* Sort within adjacency lists if requested */
    if (sort_targets) {
        csr_sort_adjacency_lists(out);
        out->sorted = true;
    } else {
        out->sorted = false;
    }

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

    /* Build edge pairs */
    ray_t* pairs_hdr = ray_alloc((size_t)n_edges * sizeof(edge_pair_t));
    if (!pairs_hdr) return NULL;
    edge_pair_t* pairs = (edge_pair_t*)ray_data(pairs_hdr);

    int64_t* src_data = (int64_t*)ray_data(src_vec);
    int64_t* dst_data = (int64_t*)ray_data(dst_vec);
    for (int64_t i = 0; i < n_edges; i++) {
        pairs[i].src = src_data[i];
        pairs[i].dst = dst_data[i];
        pairs[i].row = i;
    }

    /* Allocate rel */
    ray_rel_t* rel = (ray_rel_t*)ray_sys_alloc(sizeof(ray_rel_t));
    if (!rel) { ray_free(pairs_hdr); return NULL; }
    memset(rel, 0, sizeof(ray_rel_t));
    rel->name_sym = -1;

    /* Build forward CSR (sorted by src) */
    /* qsort is from libc, not an external dep */
    qsort(pairs, (size_t)n_edges, sizeof(edge_pair_t), cmp_edge_by_src);
    ray_err_t err = csr_build_from_pairs(pairs, n_edges, n_src_nodes, false,
                                         sort_targets, &rel->fwd);
    if (err != RAY_OK) {
        ray_free(pairs_hdr);
        ray_sys_free(rel);
        return NULL;
    }

    /* Build reverse CSR (sorted by dst) */
    qsort(pairs, (size_t)n_edges, sizeof(edge_pair_t), cmp_edge_by_dst);
    err = csr_build_from_pairs(pairs, n_edges, n_dst_nodes, true,
                                sort_targets, &rel->rev);
    if (err != RAY_OK) {
        ray_free(pairs_hdr);
        csr_free(&rel->fwd);
        ray_sys_free(rel);
        return NULL;
    }

    ray_free(pairs_hdr);
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

    /* Build edge pairs: src = row index, dst = fk value */
    ray_t* pairs_hdr = ray_alloc((size_t)n_edges * sizeof(edge_pair_t));
    if (!pairs_hdr) return NULL;
    edge_pair_t* pairs = (edge_pair_t*)ray_data(pairs_hdr);

    int64_t* fk_data = (int64_t*)ray_data(fk_vec);
    for (int64_t i = 0; i < n_edges; i++) {
        pairs[i].src = i;
        pairs[i].dst = fk_data[i];
        pairs[i].row = i;
    }

    ray_rel_t* rel = (ray_rel_t*)ray_sys_alloc(sizeof(ray_rel_t));
    if (!rel) { ray_free(pairs_hdr); return NULL; }
    memset(rel, 0, sizeof(ray_rel_t));
    rel->name_sym = -1;

    /* Build forward CSR */
    qsort(pairs, (size_t)n_edges, sizeof(edge_pair_t), cmp_edge_by_src);
    ray_err_t err = csr_build_from_pairs(pairs, n_edges, n_src_nodes, false,
                                         sort_targets, &rel->fwd);
    if (err != RAY_OK) {
        ray_free(pairs_hdr);
        ray_sys_free(rel);
        return NULL;
    }

    /* Build reverse CSR */
    qsort(pairs, (size_t)n_edges, sizeof(edge_pair_t), cmp_edge_by_dst);
    err = csr_build_from_pairs(pairs, n_edges, n_target_nodes, true,
                                sort_targets, &rel->rev);
    if (err != RAY_OK) {
        ray_free(pairs_hdr);
        csr_free(&rel->fwd);
        ray_sys_free(rel);
        return NULL;
    }

    ray_free(pairs_hdr);
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
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return RAY_ERR_IO;

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
