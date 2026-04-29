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

/*  Table builtins — extracted from eval.c  */

#include "lang/internal.h"
#include "lang/env.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "ops/hash.h"
#include "ops/idxop.h"
#include "table/sym.h"
#include "mem/heap.h"
#include <stdio.h>
#include <inttypes.h>

/* ══════════════════════════════════════════
 * pivot_fn_to_agg_op
 * ══════════════════════════════════════════ */

/* Map a RAY_UNARY agg function pointer to a DAG opcode.
 * Returns 0 if the function is not a known aggregation builtin. */
uint16_t pivot_fn_to_agg_op(ray_t* fn) {
    if (fn->type != RAY_UNARY) return 0;
    ray_unary_fn f = (ray_unary_fn)(uintptr_t)fn->i64;
    if (f == ray_sum_fn)   return OP_SUM;
    if (f == ray_avg_fn)   return OP_AVG;
    if (f == ray_min_fn)      return OP_MIN;
    if (f == ray_max_fn)      return OP_MAX;
    if (f == ray_count_fn) return OP_COUNT;
    if (f == ray_first_fn) return OP_FIRST;
    if (f == ray_last_fn)  return OP_LAST;
    return 0;
}

/* ══════════════════════════════════════════
 * pivot
 * ══════════════════════════════════════════ */

/* (pivot table index_col pivot_col value_col agg_fn) — pivot table */
ray_t* ray_pivot_fn(ray_t** args, int64_t n) {
    if (n != 5) return ray_error("arity", "pivot expects 5 arguments: table, index, pivot-col, value-col, agg-fn");
    ray_t* tbl            = args[0];
    ray_t* index_arg      = args[1];   /* sym atom or list of syms */
    ray_t* pivot_col_name = args[2];   /* sym atom */
    ray_t* value_col_name = args[3];   /* sym atom */
    ray_t* agg_fn         = args[4];   /* function */

    if (tbl->type != RAY_TABLE)
        return ray_error("type", "pivot: first argument must be a table");
    if (pivot_col_name->type != -RAY_SYM)
        return ray_error("type", "pivot: pivot-col must be a symbol");
    if (value_col_name->type != -RAY_SYM)
        return ray_error("type", "pivot: value-col must be a symbol");
    if (agg_fn->type != RAY_UNARY && agg_fn->type != RAY_LAMBDA &&
        agg_fn->type != RAY_VARY)
        return ray_error("type", "pivot: agg-fn must be a function");

    /* Determine index columns */
    int64_t idx_syms[16];
    int64_t n_idx = 0;
    if (index_arg->type == -RAY_SYM) {
        idx_syms[0] = index_arg->i64;
        n_idx = 1;
    } else if (index_arg->type == RAY_LIST || ray_is_vec(index_arg)) {
        int64_t len = ray_len(index_arg);
        if (len > 16) return ray_error("limit", "pivot: too many index columns");
        for (int64_t i = 0; i < len; i++) {
            int alloc = 0;
            ray_t* elem = collection_elem(index_arg, i, &alloc);
            if (RAY_IS_ERR(elem)) return elem;
            if (elem->type != -RAY_SYM) {
                if (alloc) ray_release(elem);
                return ray_error("type", "pivot: index columns must be symbols");
            }
            idx_syms[i] = elem->i64;
            if (alloc) ray_release(elem);
        }
        n_idx = len;
    } else {
        return ray_error("type", "pivot: index must be a symbol or list of symbols");
    }

    /* Get pivot column, value column */
    ray_t* pcol = ray_table_get_col(tbl, pivot_col_name->i64);
    if (!pcol) return ray_error("domain", "pivot: pivot column not found");
    ray_t* vcol = ray_table_get_col(tbl, value_col_name->i64);
    if (!vcol) return ray_error("domain", "pivot: value column not found");

    /* Get index columns */
    ray_t* icols[16];
    for (int64_t i = 0; i < n_idx; i++) {
        icols[i] = ray_table_get_col(tbl, idx_syms[i]);
        if (!icols[i]) return ray_error("domain", "pivot: index column not found");
    }

    int64_t nrows = ray_table_nrows(tbl);
    if (nrows == 0) return ray_table_new(0);

    /* DAG fast path: known agg builtins on hashable columns → OP_PIVOT */
    uint16_t agg_op = pivot_fn_to_agg_op(agg_fn);
    bool dag_ok = (agg_op != 0 && pcol->type != RAY_STR && vcol->type != RAY_STR);
    for (int64_t i = 0; i < n_idx && dag_ok; i++)
        if (icols[i]->type == RAY_STR) dag_ok = false;

    if (dag_ok) {
        ray_graph_t* g = ray_graph_new(tbl);
        if (!g) return ray_error("oom", NULL);
        ray_op_t* idx_ops[16];
        bool ok = true;
        for (int64_t i = 0; i < n_idx && ok; i++) {
            ray_t* s = ray_sym_str(idx_syms[i]);
            idx_ops[i] = s ? ray_scan(g, ray_str_ptr(s)) : NULL;
            if (!idx_ops[i]) ok = false;
        }
        ray_t* ps = ray_sym_str(pivot_col_name->i64);
        ray_t* vs = ray_sym_str(value_col_name->i64);
        ray_op_t* p_op = (ps && ok) ? ray_scan(g, ray_str_ptr(ps)) : NULL;
        ray_op_t* v_op = (vs && p_op) ? ray_scan(g, ray_str_ptr(vs)) : NULL;
        if (v_op) {
            ray_op_t* root = ray_pivot_op(g, idx_ops, (uint8_t)n_idx, p_op, v_op, agg_op);
            if (root) {
                ray_t* result = ray_execute(g, root);
                ray_graph_free(g);
                return result;
            }
        }
        ray_graph_free(g);
    }

    /* Generic fallback: use OP_GROUP DAG to group by (index_cols, pivot_col),
     * then apply agg_fn per group and unstack.  Single O(n) hash pass. */

    /* Build GROUP BY (idx0, ..., idxN-1, pivot_col) with COUNT agg via DAG */
    ray_graph_t* g = ray_graph_new(tbl);
    if (!g) return ray_error("oom", NULL);

    uint8_t n_keys = (uint8_t)(n_idx + 1);
    ray_op_t* key_ops[16];
    bool ok = true;
    for (int64_t i = 0; i < n_idx && ok; i++) {
        ray_t* s = ray_sym_str(idx_syms[i]);
        key_ops[i] = s ? ray_scan(g, ray_str_ptr(s)) : NULL;
        if (!key_ops[i]) ok = false;
    }
    {
        ray_t* ps = ray_sym_str(pivot_col_name->i64);
        key_ops[n_idx] = (ps && ok) ? ray_scan(g, ray_str_ptr(ps)) : NULL;
        if (!key_ops[n_idx]) ok = false;
    }
    /* Value column scan for COUNT (just need a column ref for group) */
    ray_t* vs = ray_sym_str(value_col_name->i64);
    ray_op_t* val_scan = (vs && ok) ? ray_scan(g, ray_str_ptr(vs)) : NULL;
    if (!val_scan) { ray_graph_free(g); return ray_error("domain", "pivot: failed to build DAG"); }

    uint16_t grp_agg_ops[1] = { OP_COUNT };
    ray_op_t* grp_agg_ins[1] = { val_scan };
    ray_op_t* grp_root = ray_group(g, key_ops, n_keys, grp_agg_ops, grp_agg_ins, 1);
    if (!grp_root) { ray_graph_free(g); return ray_error("oom", NULL); }

    ray_t* grouped = ray_execute(g, grp_root);
    ray_graph_free(g);
    if (!grouped || RAY_IS_ERR(grouped)) return grouped;

    /* `grouped` is a table: (idx0, ..., idxN-1, pivot_col, _count).
     * Each row is one (index, pivot) combination.
     * Now for each group, gather the value column subset and apply agg_fn. */
    int64_t n_grps = ray_table_nrows(grouped);

    /* Get grouped columns */
    ray_t* g_icols[16];
    for (int64_t i = 0; i < n_idx; i++)
        g_icols[i] = ray_table_get_col(grouped, idx_syms[i]);
    ray_t* g_pcol = ray_table_get_col(grouped, pivot_col_name->i64);

    /* Collect distinct pivot values and index keys from grouped table */
    ray_retain(g_pcol);
    ray_t* dvals = ray_distinct_fn(g_pcol);
    ray_release(g_pcol);
    if (RAY_IS_ERR(dvals)) { ray_release(grouped); return dvals; }
    int64_t n_pv = ray_len(dvals);

    /* Re-scan original table to assign a grouped-row index to each
     * input row.  Previously this was an O(nrows * n_grps) nested loop
     * that hung on any large pivot that took the generic fallback.
     * Replaced with an open-addressed hash table keyed by a cheap row
     * hash of (idx_cols..., pivot_col), giving O(nrows + n_grps) in the
     * common case.  Hash collisions re-verify via atom_eq so unhashable
     * cells (strings, guids) still match correctly.
     *
     * Hash helper: produces the same value when called on two rows with
     * equal cell values for numeric/sym/temporal columns; for strings
     * and guids we under-hash (returning a type-independent constant)
     * and rely entirely on atom_eq for equality. */
    #define FB_ROW_HASH(cols, ncols, pv, rid)                                \
        ({                                                                    \
            uint64_t _h = 0;                                                  \
            for (int64_t _k = 0; _k < (ncols); _k++) {                        \
                ray_t* _c = (cols)[_k];                                       \
                uint64_t _kh;                                                 \
                if (ray_vec_is_null(_c, (rid)))                               \
                    _kh = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(rid);            \
                else if (_c->type == RAY_F64)                                 \
                    _kh = ray_hash_f64(((double*)ray_data(_c))[(rid)]);       \
                else if (_c->type == RAY_STR || _c->type == RAY_GUID)         \
                    _kh = 0xDEADBEEFCAFEBABEULL;                              \
                else                                                           \
                    _kh = ray_hash_i64(read_col_i64(ray_data(_c), (rid),      \
                                                    _c->type, _c->attrs));    \
                _h = (_k == 0) ? _kh : ray_hash_combine(_h, _kh);             \
            }                                                                  \
            ray_t* _pc = (pv);                                                 \
            uint64_t _ph;                                                      \
            if (ray_vec_is_null(_pc, (rid)))                                  \
                _ph = 0x165667B19E3779F9ULL ^ (uint64_t)(rid);                \
            else if (_pc->type == RAY_F64)                                     \
                _ph = ray_hash_f64(((double*)ray_data(_pc))[(rid)]);          \
            else if (_pc->type == RAY_STR || _pc->type == RAY_GUID)            \
                _ph = 0xFEEDFACE12345678ULL;                                   \
            else                                                                \
                _ph = ray_hash_i64(read_col_i64(ray_data(_pc), (rid),         \
                                                 _pc->type, _pc->attrs));      \
            ray_hash_combine(_h, _ph);                                         \
        })

    uint32_t gid_cap = 256;
    while (gid_cap < (uint32_t)n_grps * 2 && gid_cap < (1u << 30)) gid_cap <<= 1;
    ray_t* gid_ht_hdr = ray_alloc((size_t)gid_cap * sizeof(uint32_t));
    if (!gid_ht_hdr) { ray_release(dvals); ray_release(grouped); return ray_error("oom", NULL); }
    uint32_t* gid_ht = (uint32_t*)ray_data(gid_ht_hdr);
    memset(gid_ht, 0xFF, gid_cap * sizeof(uint32_t));
    uint32_t gid_mask = gid_cap - 1;

    /* Insert each grouped row into the HT (grouped rows are already
     * distinct by construction — no equality check needed on insert). */
    for (int64_t gi = 0; gi < n_grps; gi++) {
        uint64_t h = FB_ROW_HASH(g_icols, n_idx, g_pcol, gi);
        uint32_t slot = (uint32_t)(h & gid_mask);
        while (gid_ht[slot] != UINT32_MAX) slot = (slot + 1) & gid_mask;
        gid_ht[slot] = (uint32_t)gi;
    }

    ray_t* gid_vec = ray_vec_new(RAY_I64, nrows);
    if (!gid_vec || RAY_IS_ERR(gid_vec)) {
        ray_free(gid_ht_hdr); ray_release(dvals); ray_release(grouped);
        return ray_error("oom", NULL);
    }
    gid_vec->len = nrows;
    int64_t* gids = (int64_t*)ray_data(gid_vec);

    /* Probe HT for each input row; on collision fall through to atom_eq. */
    for (int64_t r = 0; r < nrows; r++) {
        uint64_t h = FB_ROW_HASH(icols, n_idx, pcol, r);
        uint32_t slot = (uint32_t)(h & gid_mask);
        int64_t found = -1;
        while (gid_ht[slot] != UINT32_MAX) {
            int64_t gi = gid_ht[slot];
            bool match = true;
            for (int64_t ci = 0; ci < n_idx && match; ci++) {
                int a1 = 0, a2 = 0;
                ray_t* v1 = collection_elem(icols[ci], r, &a1);
                ray_t* v2 = collection_elem(g_icols[ci], gi, &a2);
                if (!atom_eq(v1, v2)) match = false;
                if (a1) ray_release(v1);
                if (a2) ray_release(v2);
            }
            if (match) {
                int a1 = 0, a2 = 0;
                ray_t* v1 = collection_elem(pcol, r, &a1);
                ray_t* v2 = collection_elem(g_pcol, gi, &a2);
                if (!atom_eq(v1, v2)) match = false;
                if (a1) ray_release(v1);
                if (a2) ray_release(v2);
            }
            if (match) { found = gi; break; }
            slot = (slot + 1) & gid_mask;
        }
        gids[r] = found;
    }
    ray_free(gid_ht_hdr);

    /* For each group, gather the value column subset and apply agg_fn */
    ray_t* agg_results = ray_alloc(n_grps * sizeof(ray_t*));
    if (!agg_results) { ray_release(gid_vec); ray_release(dvals); ray_release(grouped); return ray_error("oom", NULL); }
    agg_results->type = RAY_LIST;
    agg_results->len = n_grps;
    ray_t** ar = (ray_t**)ray_data(agg_results);

    /* Counting-sort rows by gid: O(nrows + n_grps) vs the previous
     * O(nrows * n_grps) double-scan per group. */
    ray_t* off_hdr = ray_alloc((size_t)(n_grps + 1) * sizeof(int64_t));
    if (!off_hdr) {
        ray_free(agg_results); ray_release(gid_vec); ray_release(dvals); ray_release(grouped);
        return ray_error("oom", NULL);
    }
    int64_t* offs = (int64_t*)ray_data(off_hdr);
    memset(offs, 0, (size_t)(n_grps + 1) * sizeof(int64_t));
    for (int64_t r = 0; r < nrows; r++) {
        int64_t g = gids[r];
        if (g >= 0) offs[g + 1]++;
    }
    for (int64_t gi = 0; gi < n_grps; gi++) offs[gi + 1] += offs[gi];

    ray_t* sorted_hdr = ray_alloc((size_t)nrows * sizeof(int64_t));
    if (!sorted_hdr) {
        ray_free(off_hdr);
        ray_free(agg_results); ray_release(gid_vec); ray_release(dvals); ray_release(grouped);
        return ray_error("oom", NULL);
    }
    int64_t* sorted = (int64_t*)ray_data(sorted_hdr);
    /* Write-cursor array derived from offs. */
    ray_t* wcur_hdr = ray_alloc((size_t)n_grps * sizeof(int64_t));
    if (!wcur_hdr) {
        ray_free(sorted_hdr); ray_free(off_hdr);
        ray_free(agg_results); ray_release(gid_vec); ray_release(dvals); ray_release(grouped);
        return ray_error("oom", NULL);
    }
    int64_t* wcur = (int64_t*)ray_data(wcur_hdr);
    memcpy(wcur, offs, (size_t)n_grps * sizeof(int64_t));
    for (int64_t r = 0; r < nrows; r++) {
        int64_t g = gids[r];
        if (g >= 0) sorted[wcur[g]++] = r;
    }
    ray_free(wcur_hdr);

    for (int64_t gi = 0; gi < n_grps; gi++) {
        int64_t cnt = offs[gi + 1] - offs[gi];
        ray_t* subset = gather_by_idx(vcol, sorted + offs[gi], cnt);
        if (RAY_IS_ERR(subset)) {
            for (int64_t j = 0; j < gi; j++) ray_release(ar[j]);
            ray_free(sorted_hdr); ray_free(off_hdr);
            ray_free(agg_results); ray_release(gid_vec); ray_release(dvals); ray_release(grouped);
            return subset;
        }
        ray_t* agg_val = call_fn1(agg_fn, subset);
        ray_release(subset);
        if (RAY_IS_ERR(agg_val)) {
            for (int64_t j = 0; j < gi; j++) ray_release(ar[j]);
            ray_free(sorted_hdr); ray_free(off_hdr);
            ray_free(agg_results); ray_release(gid_vec); ray_release(dvals); ray_release(grouped);
            return agg_val;
        }
        ar[gi] = agg_val;
    }
    ray_free(sorted_hdr);
    ray_free(off_hdr);
    ray_release(gid_vec);

    /* Unstack: collect distinct index keys, build wide result.
     * Map each group to (ix_idx, pv_idx). */
    ray_t* ix_list = ray_list_new(16);
    ray_t* gmap = ray_alloc(n_grps * 2 * sizeof(int64_t));
    int64_t* gm_ix = (int64_t*)ray_data(gmap);
    int64_t* gm_pv = gm_ix + n_grps;

    for (int64_t gi = 0; gi < n_grps; gi++) {
        /* Find pivot index */
        int a1 = 0;
        ray_t* pv = collection_elem(g_pcol, gi, &a1);
        gm_pv[gi] = -1;
        for (int64_t p = 0; p < n_pv; p++) {
            int a2 = 0;
            ray_t* dv = collection_elem(dvals, p, &a2);
            bool eq = atom_eq(pv, dv);
            if (a2) ray_release(dv);
            if (eq) { gm_pv[gi] = p; break; }
        }
        if (a1) ray_release(pv);

        /* Find or insert index key */
        gm_ix[gi] = -1;
        int64_t n_ix = ray_len(ix_list);
        ray_t** ix_items = (ray_t**)ray_data(ix_list);
        for (int64_t j = 0; j < n_ix; j++) {
            ray_t** ex = (ray_t**)ray_data(ix_items[j]);
            bool match = true;
            for (int64_t ci = 0; ci < n_idx && match; ci++) {
                int a2 = 0;
                ray_t* v = collection_elem(g_icols[ci], gi, &a2);
                if (!atom_eq(ex[ci], v)) match = false;
                if (a2) ray_release(v);
            }
            if (match) { gm_ix[gi] = j; break; }
        }
        if (gm_ix[gi] < 0) {
            gm_ix[gi] = ray_len(ix_list);
            ray_t* tup = ray_list_new((int32_t)n_idx);
            for (int64_t ci = 0; ci < n_idx; ci++) {
                int a2 = 0;
                ray_t* v = collection_elem(g_icols[ci], gi, &a2);
                if (!a2) ray_retain(v);
                tup = ray_list_append(tup, v);
                ray_release(v);
            }
            ix_list = ray_list_append(ix_list, tup);
            ray_release(tup);
        }
    }

    int64_t n_ix = ray_len(ix_list);

    /* Build result table */
    ray_t* result = ray_table_new(n_idx + n_pv);
    if (RAY_IS_ERR(result)) goto fb_cleanup;

    /* Index columns */
    { ray_t** ix_items = (ray_t**)ray_data(ix_list);
    for (int64_t ci = 0; ci < n_idx; ci++) {
        ray_t* col_vals = ray_list_new((int32_t)n_ix);
        for (int64_t r = 0; r < n_ix; r++) {
            ray_t* v = ((ray_t**)ray_data(ix_items[r]))[ci];
            ray_retain(v);
            col_vals = ray_list_append(col_vals, v);
            ray_release(v);
        }
        ray_t* col_vec = list_to_typed_vec(col_vals, icols[ci]->type);
        if (RAY_IS_ERR(col_vec)) { ray_release(result); result = col_vec; goto fb_cleanup; }
        result = ray_table_add_col(result, idx_syms[ci], col_vec);
        ray_release(col_vec);
        if (RAY_IS_ERR(result)) goto fb_cleanup;
    }
    }

    /* Value columns */
    for (int64_t p = 0; p < n_pv; p++) {
        ray_t* col_vals = ray_list_new((int32_t)n_ix);
        for (int64_t r = 0; r < n_ix; r++) {
            ray_t* zero = ray_i64(0);
            col_vals = ray_list_append(col_vals, zero);
            ray_release(zero);
        }

        for (int64_t gi = 0; gi < n_grps; gi++) {
            if (gm_pv[gi] != p) continue;
            ray_t** cv = (ray_t**)ray_data(col_vals);
            ray_release(cv[gm_ix[gi]]);
            ray_retain(ar[gi]);
            cv[gm_ix[gi]] = ar[gi];
        }

        int8_t agg_type = RAY_I64;
        { ray_t** cv = (ray_t**)ray_data(col_vals);
          for (int64_t r = 0; r < n_ix; r++)
            if (cv[r]->type == -RAY_F64) { agg_type = RAY_F64; break; }
        }
        ray_t* agg_vec = list_to_typed_vec(col_vals, agg_type);
        if (RAY_IS_ERR(agg_vec)) { ray_release(result); result = agg_vec; goto fb_cleanup; }

        /* Column name */
        int a1 = 0;
        ray_t* pval = collection_elem(dvals, p, &a1);
        int64_t col_sym;
        if (pval->type == -RAY_SYM) {
            col_sym = pval->i64;
        } else if (pval->type == -RAY_I64) {
            char buf[64]; int len = snprintf(buf, sizeof(buf), "%ld", (long)pval->i64);
            col_sym = ray_sym_intern(buf, (size_t)len);
        } else if (pval->type == -RAY_F64) {
            double fv = pval->f64; if (fv == 0.0 && signbit(fv)) fv = 0.0;
            char buf[64]; int len = snprintf(buf, sizeof(buf), "%g", fv);
            col_sym = ray_sym_intern(buf, (size_t)len);
        } else if (pval->type == -RAY_BOOL) {
            col_sym = ray_sym_intern(pval->b8 ? "true" : "false", pval->b8 ? 4 : 5);
        } else {
            char buf[64]; int len = snprintf(buf, sizeof(buf), "col%ld", (long)pval->i64);
            col_sym = ray_sym_intern(buf, (size_t)len);
        }
        if (a1) ray_release(pval);

        result = ray_table_add_col(result, col_sym, agg_vec);
        ray_release(agg_vec);
        if (RAY_IS_ERR(result)) goto fb_cleanup;
    }

fb_cleanup:
    ray_free(gmap);
    ray_release(ix_list);
    for (int64_t gi = 0; gi < n_grps; gi++) ray_release(ar[gi]);
    ray_free(agg_results);
    ray_release(dvals);
    ray_release(grouped);
    return result;
}

/* ══════════════════════════════════════════
 * modify
 * ══════════════════════════════════════════ */

/* (modify tbl col_name fn) — apply fn to the named column, return new table */
ray_t* ray_modify_fn(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("arity", "modify expects 3 arguments: table, column, function");
    ray_t* tbl = args[0];
    ray_t* col_name = args[1];
    ray_t* fn = args[2];

    if (tbl->type != RAY_TABLE)
        return ray_error("type", "modify: first arg must be a table");
    if (col_name->type != -RAY_SYM)
        return ray_error("type", "modify: second arg must be a symbol");

    int64_t target_sym = col_name->i64;
    ray_t* col = ray_table_get_col(tbl, target_sym);
    if (!col) return ray_error("domain", "modify: column not found");

    /* Apply fn to the entire column vector (atomic fns will map element-wise) */
    ray_t* new_col = call_fn1(fn, col);
    if (RAY_IS_ERR(new_col)) return new_col;

    /* Build new table: copy all columns, replacing the target */
    int64_t ncols = ray_table_ncols(tbl);
    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) { ray_release(new_col); return result; }

    for (int64_t i = 0; i < ncols; i++) {
        int64_t cname = ray_table_col_name(tbl, i);
        ray_t* cvec = (cname == target_sym) ? new_col : ray_table_get_col_idx(tbl, i);
        result = ray_table_add_col(result, cname, cvec);
        if (RAY_IS_ERR(result)) { ray_release(new_col); return result; }
    }
    ray_release(new_col);
    return result;
}

/* ══════════════════════════════════════════
 * alter
 * ══════════════════════════════════════════ */

/* Cleanup helper for alter set's ray_cow failure paths.  Releases the
 * caller's retain on the original vec plus the eval'd args, then returns
 * an owning RAY_ERROR — substituting "oom" when ray_cow itself returned
 * NULL (RAY_IS_ERR(NULL) is false, but the caller still owes us a
 * structured error to propagate).
 *
 * Exposed (non-static) so test code can pin the contract directly:
 * NULL cow_result must produce an "oom" error, RAY_ERROR cow_result
 * passes through, and `original_var` is released exactly once. */
ray_t* ray_alter_set_cow_fail(ray_t* original_var, ray_t* cow_result,
                              ray_t* idx, ray_t* val, ray_t* name_sym) {
    ray_release(original_var);
    if (idx)      ray_release(idx);
    if (val)      ray_release(val);
    if (name_sym) ray_release(name_sym);
    return cow_result ? cow_result : ray_error("oom", NULL);
}

ray_t* ray_alter_fn(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("domain", NULL);
    /* First arg: evaluate to get the symbol */
    ray_t* name_sym = ray_eval(args[0]);
    if (!name_sym || RAY_IS_ERR(name_sym)) return name_sym ? name_sym : ray_error("type", NULL);
    if (name_sym->type != -RAY_SYM) { ray_release(name_sym); return ray_error("type", NULL); }

    /* Resolve the variable */
    ray_t* var = ray_env_get(name_sym->i64);
    if (!var) { ray_release(name_sym); return ray_error("name", NULL); }

    /* Second arg: operation name (unevaluated, must be a name) */
    ray_t* op = args[1];
    if (!op || op->type != -RAY_SYM) { ray_release(name_sym); return ray_error("type", NULL); }
    ray_t* op_name = ray_sym_str(op->i64);
    if (!op_name) { ray_release(name_sym); return ray_error("domain", NULL); }
    const char* oname = ray_str_ptr(op_name);
    size_t olen = ray_str_len(op_name);

    if (olen == 3 && memcmp(oname, "set", 3) == 0) {
        /* (alter 'v set idx val) — idx can be scalar or vector of indices */
        ray_release(op_name);
        if (n < 4) { ray_release(name_sym); return ray_error("domain", NULL); }
        ray_t* idx = ray_eval(args[2]);
        if (!idx || RAY_IS_ERR(idx)) { ray_release(name_sym); return idx ? idx : ray_error("type", NULL); }
        ray_t* val = ray_eval(args[3]);
        if (!val || RAY_IS_ERR(val)) { ray_release(idx); ray_release(name_sym); return val ? val : ray_error("type", NULL); }
        if (!ray_is_vec(var) && var->type != RAY_LIST) { ray_release(idx); ray_release(val); ray_release(name_sym); return ray_error("type", NULL); }

        /* For LIST types, build a new list with replaced elements */
        if (var->type == RAY_LIST) {
            int64_t vlen = ray_len(var);
            ray_t** elems = (ray_t**)ray_data(var);
            ray_t* new_list = ray_alloc(vlen * sizeof(ray_t*));
            if (!new_list) { ray_release(idx); ray_release(val); ray_release(name_sym); return ray_error("oom", NULL); }
            new_list->type = RAY_LIST;
            new_list->len = vlen;
            ray_t** out = (ray_t**)ray_data(new_list);
            for (int64_t i = 0; i < vlen; i++) { ray_retain(elems[i]); out[i] = elems[i]; }

            if (ray_is_atom(idx) && is_numeric(idx)) {
                int64_t i = as_i64(idx);
                if (i >= 0 && i < vlen) { ray_release(out[i]); ray_retain(val); out[i] = val; }
            } else if (ray_is_vec(idx)) {
                int64_t nidx = idx->len;
                for (int64_t k = 0; k < nidx; k++) {
                    int alloc = 0;
                    ray_t* ie = collection_elem(idx, k, &alloc);
                    int64_t i = as_i64(ie);
                    if (alloc) ray_release(ie);
                    if (i >= 0 && i < vlen) { ray_release(out[i]); ray_retain(val); out[i] = val; }
                }
            }
            ray_release(idx); ray_release(val);
            ray_env_set(name_sym->i64, new_list);
            ray_release(name_sym);
            ray_retain(new_list);
            return new_list;
        }

        /* `var` came from ray_env_get as a BORROWED ref.  ray_cow's
         * contract is "I take your owning ref; I give you back a ref"
         * — so calling it on a borrow over-decrements the env's
         * binding when the rc>1 copy path fires (releasing v drops
         * env's count from N to N-1; if some other env binding also
         * pointed at v, that binding now sees an extra under-retain
         * and risks UAF when later replaced).
         *
         * Retain up-front so the ref we hand to ray_cow is genuinely
         * ours.  Track the original pointer so the cow-OOM path
         * (alloc_copy fails before ray_cow's release would have run)
         * can still release the retain — without that, OOM leaks the
         * extra ref. */
        ray_t* original_var = var;
        ray_retain(var);
        ray_t* cow_result = ray_cow(var);
        /* ray_cow returns NULL when ray_alloc_copy returned NULL (heap
         * exhaustion past RAY_HEAP_MAX_ORDER) and a RAY_ERROR pointer
         * when alloc_copy hit its own len-overflow guard.  Both leave
         * the input ref untouched, so the cleanup helper releases
         * `original_var` and either propagates the error pointer or
         * synthesizes an "oom" RAY_ERROR for the NULL case (test code
         * pins both branches). */
        if (!cow_result || RAY_IS_ERR(cow_result)) {
            return ray_alter_set_cow_fail(original_var, cow_result, idx, val, name_sym);
        }
        var = cow_result;

        /* Validate idx shape + (for the atom case) bounds BEFORE we
         * touch any state.  The accelerator-index drop below would
         * otherwise outlive a failed write. */
        bool idx_is_atom_num = ray_is_atom(idx) && is_numeric(idx);
        bool idx_is_vec      = ray_is_vec(idx);
        if (!idx_is_atom_num && !idx_is_vec) {
            ray_release(var);
            ray_release(idx); ray_release(val); ray_release(name_sym);
            return ray_error("type", NULL);
        }
        if (idx_is_atom_num) {
            int64_t i_check = as_i64(idx);
            if (i_check < 0 || i_check >= var->len) {
                ray_release(var);
                ray_release(idx); ray_release(val); ray_release(name_sym);
                return ray_error("index", NULL);
            }
        }

        /* alter's set path writes via store_typed_elem, which bypasses
         * ray_vec_set's mutation guard.  Now that we know the write
         * will reach the data array, drop any attached accelerator
         * index so it can't outlive the mutation. */
        if (var->attrs & RAY_ATTR_HAS_INDEX) {
            ray_t* drop_r = ray_index_drop(&var);
            if (RAY_IS_ERR(drop_r)) {
                ray_release(var);
                ray_release(idx); ray_release(val); ray_release(name_sym);
                return drop_r;
            }
        }

        if (idx_is_atom_num) {
            /* Single index — bounds already validated above. */
            int64_t i = as_i64(idx);
            ray_release(idx);
            store_typed_elem(var, i, val);
        } else {
            /* Vector of indices — set each to val.
             * If val is a vector of same length, set pairwise.
             * If val is scalar or shorter, broadcast. */
            int64_t nidx = idx->len;
            int val_is_vec = ray_is_vec(val) && val->len == nidx;
            for (int64_t k = 0; k < nidx; k++) {
                int alloc = 0;
                ray_t* ie = collection_elem(idx, k, &alloc);
                int64_t i = as_i64(ie);
                if (alloc) ray_release(ie);
                if (i < 0 || i >= var->len) continue;
                if (val_is_vec) {
                    int va = 0;
                    ray_t* ve = collection_elem(val, k, &va);
                    store_typed_elem(var, i, ve);
                    if (va) ray_release(ve);
                } else {
                    store_typed_elem(var, i, val);
                }
            }
            ray_release(idx);
        }
        ray_release(val);
        ray_env_set(name_sym->i64, var);
        ray_release(name_sym);
        /* The retain-first at the top of the set path gave us an owning
         * ref to var.  ray_env_set already retained for the env binding;
         * transferring our existing ref to the caller via return is
         * correct.  No additional ray_retain here. */
        return var;
    }
    if (olen == 6 && memcmp(oname, "concat", 6) == 0) {
        /* (alter 'v concat val) */
        ray_release(op_name);
        if (n < 3) { ray_release(name_sym); return ray_error("domain", NULL); }
        ray_t* val = ray_eval(args[2]);
        if (!val || RAY_IS_ERR(val)) { ray_release(name_sym); return val ? val : ray_error("type", NULL); }
        ray_t* new_vec = ray_concat_fn(var, val);
        ray_release(val);
        if (RAY_IS_ERR(new_vec)) { ray_release(name_sym); return new_vec; }
        ray_env_set(name_sym->i64, new_vec);
        ray_release(name_sym);
        ray_retain(new_vec);
        return new_vec;
    }
    if (olen == 6 && memcmp(oname, "remove", 6) == 0) {
        /* (alter 'v remove idx) — remove element(s) at index/indices */
        ray_release(op_name);
        if (n < 3) { ray_release(name_sym); return ray_error("domain", NULL); }
        ray_t* idx = ray_eval(args[2]);
        if (!idx || RAY_IS_ERR(idx)) { ray_release(name_sym); return idx ? idx : ray_error("type", NULL); }

        if (!var || var->type != RAY_LIST) {
            ray_release(idx); ray_release(name_sym);
            return ray_error("type", NULL);
        }

        int64_t vlen = ray_len(var);
        ray_t** elems = (ray_t**)ray_data(var);

        /* Build a set of indices to remove */
        int64_t remove_idx[256];
        int64_t nremove = 0;
        if (ray_is_atom(idx) && is_numeric(idx)) {
            remove_idx[0] = as_i64(idx);
            nremove = 1;
        } else if (ray_is_vec(idx)) {
            nremove = idx->len;
            if (nremove > 256) { ray_release(idx); ray_release(name_sym); return ray_error("limit", NULL); }
            for (int64_t i = 0; i < nremove; i++) {
                int alloc = 0;
                ray_t* e = collection_elem(idx, i, &alloc);
                remove_idx[i] = as_i64(e);
                if (alloc) ray_release(e);
            }
        } else {
            ray_release(idx); ray_release(name_sym);
            return ray_error("type", NULL);
        }
        ray_release(idx);

        /* Build new list without the removed indices */
        int64_t new_len = vlen;
        for (int64_t i = 0; i < nremove; i++)
            if (remove_idx[i] >= 0 && remove_idx[i] < vlen) new_len--;

        ray_t* new_list = ray_alloc(new_len * sizeof(ray_t*));
        if (!new_list) { ray_release(name_sym); return ray_error("oom", NULL); }
        new_list->type = RAY_LIST;
        new_list->len = new_len;
        ray_t** out = (ray_t**)ray_data(new_list);
        int64_t j = 0;
        for (int64_t i = 0; i < vlen; i++) {
            int skip = 0;
            for (int64_t k = 0; k < nremove; k++)
                if (remove_idx[k] == i) { skip = 1; break; }
            if (!skip) {
                ray_retain(elems[i]);
                out[j++] = elems[i];
            }
        }
        new_list->len = j;
        ray_env_set(name_sym->i64, new_list);
        ray_release(name_sym);
        ray_retain(new_list);
        return new_list;
    }
    ray_release(op_name);
    ray_release(name_sym);
    return ray_error("domain", NULL);
}

/* ══════════════════════════════════════════
 * del
 * ══════════════════════════════════════════ */

/* (del name) — delete variable from environment (special form, unevaluated arg) */
ray_t* ray_del_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("arity", "del expects 1 argument");
    ray_t* name = args[0];
    if (name->type != -RAY_SYM)
        return ray_error("type", "del expects a symbol");
    /* Propagate ray_env_set's failure: silently ignoring the return
     * value would let `(del .sys.gc)` appear to succeed while leaving
     * the builtin intact — a confusing lie.  Emit a precise message
     * per error code rather than blaming every failure on the
     * reserved-namespace guard (OOM on dotted-path upsert, for
     * example, is not a reserve error). */
    ray_err_t err = ray_env_set(name->i64, NULL);
    if (err == RAY_OK) return ray_i64(0);
    const char* nm = ray_str_ptr(ray_sym_str(name->i64));
    if (err == RAY_ERR_RESERVED)
        return ray_error("reserve",
                         "cannot delete reserved binding '%s'", nm);
    return ray_error(ray_err_code_str(err),
                     "del '%s' failed", nm);
}

/* ══════════════════════════════════════════
 * row
 * ══════════════════════════════════════════ */

/* (row table idx) — extract a single row from a table as a dict */
ray_t* ray_row_fn(ray_t* tbl, ray_t* idx) {
    if (tbl->type != RAY_TABLE) return ray_error("type", "row expects a table");
    if (!is_numeric(idx)) return ray_error("type", "row index must be integer");
    /* Delegate to at — it already handles table integer indexing */
    return ray_at_fn(tbl, idx);
}

/* ══════════════════════════════════════════
 * union-all
 * ══════════════════════════════════════════ */

/* (union-all t1 t2) — concatenate two tables row-wise (same schema) */
ray_t* ray_union_all_fn(ray_t* t1, ray_t* t2) {
    if (t1->type != RAY_TABLE)
        return ray_error("type", "union-all: first arg must be a table");
    if (t2->type != RAY_TABLE)
        return ray_error("type", "union-all: second arg must be a table");

    int64_t ncols = ray_table_ncols(t1);
    if (ncols != ray_table_ncols(t2))
        return ray_error("type", "union-all: tables must have same number of columns");

    /* Validate matching column names */
    for (int64_t c = 0; c < ncols; c++) {
        if (ray_table_col_name(t1, c) != ray_table_col_name(t2, c))
            return ray_error("type", "union-all: column names must match");
    }

    ray_t* result = ray_table_new(ncols);
    if (!result || RAY_IS_ERR(result)) return result;

    for (int64_t c = 0; c < ncols; c++) {
        int64_t name_id = ray_table_col_name(t1, c);
        ray_t* col1 = ray_table_get_col_idx(t1, c);
        ray_t* col2 = ray_table_get_col_idx(t2, c);

        if (!col1 || !col2) {
            ray_release(result);
            return ray_error("type", "union-all: missing column");
        }

        ray_t* combined = ray_vec_concat(col1, col2);
        if (!combined || RAY_IS_ERR(combined)) {
            ray_release(result);
            return combined ? combined : ray_error("oom", NULL);
        }

        result = ray_table_add_col(result, name_id, combined);
        ray_release(combined);
        if (!result || RAY_IS_ERR(result)) return result;
    }

    return result;
}

/* ══════════════════════════════════════════
 * table-distinct
 * ══════════════════════════════════════════ */

/* (table-distinct t) — remove duplicate rows via DAG group-by */
ray_t* ray_table_distinct_fn(ray_t* tbl) {
    if (tbl->type != RAY_TABLE)
        return ray_error("type", "table-distinct expects a table");

    int64_t ncols = ray_table_ncols(tbl);
    if (ncols == 0) { ray_retain(tbl); return tbl; }

    ray_graph_t* g = ray_graph_new(tbl);
    if (!g) return ray_error("oom", NULL);

    ray_op_t* keys[256];
    if (ncols > 256) { ray_graph_free(g); return ray_error("range", "too many columns"); }

    for (int64_t c = 0; c < ncols; c++) {
        int64_t name_id = ray_table_col_name(tbl, c);
        ray_t* name_str = ray_sym_str(name_id);
        if (!name_str) { ray_graph_free(g); return ray_error("type", "bad column name"); }
        keys[c] = ray_scan(g, ray_str_ptr(name_str));
        if (!keys[c]) { ray_graph_free(g); return ray_error("oom", NULL); }
    }

    ray_op_t* root = ray_distinct(g, keys, (uint8_t)ncols);
    if (!root) { ray_graph_free(g); return ray_error("oom", NULL); }

    ray_t* result = ray_execute(g, root);
    ray_graph_free(g);
    return result;
}

/* ══════════════════════════════════════════
 * unify
 * ══════════════════════════════════════════ */

/* (unify a b) — return list of two vectors promoted to a common type */
ray_t* ray_unify_fn(ray_t* a, ray_t* b) {
    /* Build a 2-element list containing both values */
    ray_t* result = ray_list_new(2);
    if (RAY_IS_ERR(result)) return result;

    if (a->type == b->type || ray_is_atom(a) || ray_is_atom(b)) {
        /* Same type or atoms: return as-is */
        ray_retain(a); ray_retain(b);
        result = ray_list_append(result, a); ray_release(a);
        result = ray_list_append(result, b); ray_release(b);
        return result;
    }

    /* Different vector types: attempt numeric promotion */
    /* For now: wrap both without conversion */
    ray_retain(a); ray_retain(b);
    result = ray_list_append(result, a); ray_release(a);
    result = ray_list_append(result, b); ray_release(b);
    return result;
}
