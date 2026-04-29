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
 * OP_IF: ternary select  result[i] = cond[i] ? then[i] : else[i]
 * ============================================================================ */

ray_t* exec_if(ray_graph_t* g, ray_op_t* op) {
    /* cond = inputs[0], then = inputs[1], else_id stored in ext->literal */
    ray_t* cond_v = exec_node(g, op->inputs[0]);
    ray_t* then_v = exec_node(g, op->inputs[1]);

    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t else_id = (uint32_t)(uintptr_t)ext->literal;
    ray_t* else_v = exec_node(g, &g->nodes[else_id]);

    if (!cond_v || RAY_IS_ERR(cond_v)) {
        if (then_v && !RAY_IS_ERR(then_v)) ray_release(then_v);
        if (else_v && !RAY_IS_ERR(else_v)) ray_release(else_v);
        return cond_v;
    }
    if (!then_v || RAY_IS_ERR(then_v)) {
        ray_release(cond_v);
        if (else_v && !RAY_IS_ERR(else_v)) ray_release(else_v);
        return then_v;
    }
    if (!else_v || RAY_IS_ERR(else_v)) {
        ray_release(cond_v); ray_release(then_v);
        return else_v;
    }

    int64_t len = cond_v->len;
    bool then_scalar = ray_is_atom(then_v) || (then_v->type > 0 && then_v->len == 1);
    bool else_scalar = ray_is_atom(else_v) || (else_v->type > 0 && else_v->len == 1);
    if (then_scalar && !else_scalar) len = else_v->len;
    if (!then_scalar) len = then_v->len;

    int8_t out_type = op->out_type;
    ray_t* result = ray_vec_new(out_type, len);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(cond_v); ray_release(then_v); ray_release(else_v);
        return result;
    }
    result->len = len;

    uint8_t* cond_p = (uint8_t*)ray_data(cond_v);

    if (out_type == RAY_F64) {
        double t_scalar = then_scalar ? (ray_is_atom(then_v) ? then_v->f64 : ((double*)ray_data(then_v))[0]) : 0;
        double e_scalar = else_scalar ? (ray_is_atom(else_v) ? else_v->f64 : ((double*)ray_data(else_v))[0]) : 0;
        double* t_arr = then_scalar ? NULL : (double*)ray_data(then_v);
        double* e_arr = else_scalar ? NULL : (double*)ray_data(else_v);
        double* dst = (double*)ray_data(result);
        for (int64_t i = 0; i < len; i++)
            dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar)
                               : (e_arr ? e_arr[i] : e_scalar);
    } else if (out_type == RAY_I64) {
        int64_t t_scalar = then_scalar ? (ray_is_atom(then_v) ? then_v->i64 : ((int64_t*)ray_data(then_v))[0]) : 0;
        int64_t e_scalar = else_scalar ? (ray_is_atom(else_v) ? else_v->i64 : ((int64_t*)ray_data(else_v))[0]) : 0;
        int64_t* t_arr = then_scalar ? NULL : (int64_t*)ray_data(then_v);
        int64_t* e_arr = else_scalar ? NULL : (int64_t*)ray_data(else_v);
        int64_t* dst = (int64_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++)
            dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar)
                               : (e_arr ? e_arr[i] : e_scalar);
    } else if (out_type == RAY_I32) {
        int32_t t_scalar = then_scalar ? (ray_is_atom(then_v) ? then_v->i32 : ((int32_t*)ray_data(then_v))[0]) : 0;
        int32_t e_scalar = else_scalar ? (ray_is_atom(else_v) ? else_v->i32 : ((int32_t*)ray_data(else_v))[0]) : 0;
        int32_t* t_arr = then_scalar ? NULL : (int32_t*)ray_data(then_v);
        int32_t* e_arr = else_scalar ? NULL : (int32_t*)ray_data(else_v);
        int32_t* dst = (int32_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++)
            dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar)
                               : (e_arr ? e_arr[i] : e_scalar);
    } else if (out_type == RAY_STR) {
        /* RAY_STR: resolve each side to string data and ray_str_vec_append.
         * Scalars may be -RAY_STR or RAY_SYM atoms. */
        result->len = 0; /* ray_str_vec_append manages len */
        for (int64_t i = 0; i < len; i++) {
            const char* sp;
            size_t sl;
            if (cond_p[i]) {
                if (then_scalar) {
                    if (then_v->type == -RAY_STR) {
                        sp = ray_str_ptr(then_v);
                        sl = ray_str_len(then_v);
                    } else if (then_v->type == RAY_STR) {
                        sp = ray_str_vec_get(then_v, 0, &sl);
                        if (!sp) { sp = ""; sl = 0; }
                    } else if (RAY_IS_SYM(then_v->type)) {
                        ray_t* s = ray_sym_str(then_v->i64);
                        sp = s ? ray_str_ptr(s) : "";
                        sl = s ? ray_str_len(s) : 0;
                    } else { sp = ""; sl = 0; }
                } else if (then_v->type == RAY_STR) {
                    sp = ray_str_vec_get(then_v, i, &sl);
                    if (!sp) { sp = ""; sl = 0; }
                } else {
                    /* RAY_SYM column */
                    int64_t sid = ray_read_sym(ray_data(then_v), i, then_v->type, then_v->attrs);
                    ray_t* sa = ray_sym_str(sid);
                    sp = sa ? ray_str_ptr(sa) : "";
                    sl = sa ? ray_str_len(sa) : 0;
                }
            } else {
                if (else_scalar) {
                    if (else_v->type == -RAY_STR) {
                        sp = ray_str_ptr(else_v);
                        sl = ray_str_len(else_v);
                    } else if (else_v->type == RAY_STR) {
                        sp = ray_str_vec_get(else_v, 0, &sl);
                        if (!sp) { sp = ""; sl = 0; }
                    } else if (RAY_IS_SYM(else_v->type)) {
                        ray_t* s = ray_sym_str(else_v->i64);
                        sp = s ? ray_str_ptr(s) : "";
                        sl = s ? ray_str_len(s) : 0;
                    } else { sp = ""; sl = 0; }
                } else if (else_v->type == RAY_STR) {
                    sp = ray_str_vec_get(else_v, i, &sl);
                    if (!sp) { sp = ""; sl = 0; }
                } else {
                    /* RAY_SYM column */
                    int64_t sid = ray_read_sym(ray_data(else_v), i, else_v->type, else_v->attrs);
                    ray_t* sa = ray_sym_str(sid);
                    sp = sa ? ray_str_ptr(sa) : "";
                    sl = sa ? ray_str_len(sa) : 0;
                }
            }
            result = ray_str_vec_append(result, sp, sl);
            if (RAY_IS_ERR(result)) break;
        }
    } else if (out_type == RAY_SYM) {
        /* SYM columns may have narrow widths (W8/W16/W32) — use ray_read_sym.
         * Scalars may be string atoms that need interning. Output is always W64. */
        int64_t t_scalar = 0, e_scalar = 0;
        if (then_scalar) {
            if (then_v->type == -RAY_STR) {
                t_scalar = ray_sym_intern(ray_str_ptr(then_v), ray_str_len(then_v));
            } else {
                t_scalar = then_v->i64;
            }
        }
        if (else_scalar) {
            if (else_v->type == -RAY_STR) {
                e_scalar = ray_sym_intern(ray_str_ptr(else_v), ray_str_len(else_v));
            } else {
                e_scalar = else_v->i64;
            }
        }
        int64_t* dst = (int64_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++) {
            int64_t tv = then_scalar ? t_scalar
                : ray_read_sym(ray_data(then_v), i, then_v->type, then_v->attrs);
            int64_t ev = else_scalar ? e_scalar
                : ray_read_sym(ray_data(else_v), i, else_v->type, else_v->attrs);
            dst[i] = cond_p[i] ? tv : ev;
        }
    } else if (out_type == RAY_BOOL || out_type == RAY_U8) {
        uint8_t t_scalar = then_scalar ? then_v->b8 : 0;
        uint8_t e_scalar = else_scalar ? else_v->b8 : 0;
        uint8_t* t_arr = then_scalar ? NULL : (uint8_t*)ray_data(then_v);
        uint8_t* e_arr = else_scalar ? NULL : (uint8_t*)ray_data(else_v);
        uint8_t* dst = (uint8_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++)
            dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar)
                               : (e_arr ? e_arr[i] : e_scalar);
    } else if (out_type == RAY_TIMESTAMP || out_type == RAY_TIME || out_type == RAY_DATE) {
        /* TIMESTAMP is 8B like I64; DATE and TIME are 4B like I32 */
        if (out_type == RAY_TIMESTAMP) {
            int64_t t_scalar2 = then_scalar ? then_v->i64 : 0;
            int64_t e_scalar2 = else_scalar ? else_v->i64 : 0;
            int64_t* t_arr = then_scalar ? NULL : (int64_t*)ray_data(then_v);
            int64_t* e_arr = else_scalar ? NULL : (int64_t*)ray_data(else_v);
            int64_t* dst = (int64_t*)ray_data(result);
            for (int64_t i = 0; i < len; i++)
                dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar2)
                                   : (e_arr ? e_arr[i] : e_scalar2);
        } else {
            int32_t t_scalar2 = then_scalar ? then_v->i32 : 0;
            int32_t e_scalar2 = else_scalar ? else_v->i32 : 0;
            int32_t* t_arr = then_scalar ? NULL : (int32_t*)ray_data(then_v);
            int32_t* e_arr = else_scalar ? NULL : (int32_t*)ray_data(else_v);
            int32_t* dst = (int32_t*)ray_data(result);
            for (int64_t i = 0; i < len; i++)
                dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar2)
                                   : (e_arr ? e_arr[i] : e_scalar2);
        }
    } else if (out_type == RAY_I16) {
        int16_t t_scalar = then_scalar ? (int16_t)then_v->i32 : 0;
        int16_t e_scalar = else_scalar ? (int16_t)else_v->i32 : 0;
        int16_t* t_arr = then_scalar ? NULL : (int16_t*)ray_data(then_v);
        int16_t* e_arr = else_scalar ? NULL : (int16_t*)ray_data(else_v);
        int16_t* dst = (int16_t*)ray_data(result);
        for (int64_t i = 0; i < len; i++)
            dst[i] = cond_p[i] ? (t_arr ? t_arr[i] : t_scalar)
                               : (e_arr ? e_arr[i] : e_scalar);
    }

    ray_release(cond_v); ray_release(then_v); ray_release(else_v);
    return result;
}

/* ============================================================================
 * exec_pivot — single-pass hash-aggregated pivot table
 *
 * Groups by (index_cols, pivot_col), aggregates value_col, then unstacks
 * pivot values into separate output columns.
 * ============================================================================ */

ray_t* exec_pivot(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    uint8_t n_idx   = ext->pivot.n_index;
    uint16_t agg_op = ext->pivot.agg_op;
    int64_t nrows   = ray_table_nrows(tbl);

    /* Resolve input columns */
    ray_t* idx_vecs[16];
    for (uint8_t i = 0; i < n_idx; i++) {
        ray_op_ext_t* ie = find_ext(g, ext->pivot.index_cols[i]->id);
        idx_vecs[i] = (ie && ie->base.opcode == OP_SCAN)
                     ? ray_table_get_col(tbl, ie->sym) : NULL;
        if (!idx_vecs[i]) return ray_error("domain", "pivot: index column not found");
    }

    ray_op_ext_t* pe = find_ext(g, ext->pivot.pivot_col->id);
    ray_t* pcol = (pe && pe->base.opcode == OP_SCAN)
                ? ray_table_get_col(tbl, pe->sym) : NULL;
    if (!pcol) return ray_error("domain", "pivot: pivot column not found");

    ray_op_ext_t* ve = find_ext(g, ext->pivot.value_col->id);
    ray_t* vcol = (ve && ve->base.opcode == OP_SCAN)
                ? ray_table_get_col(tbl, ve->sym) : NULL;
    if (!vcol) return ray_error("domain", "pivot: value column not found");

    if (nrows == 0) return ray_table_new(0);

    /* Combined keys: index_cols + pivot_col */
    uint8_t n_keys = n_idx + 1;
    if (n_keys > 8) return ray_error("limit", "pivot: too many index columns");

    /* Wide-key resolution: for RAY_GUID the HT slot holds a source row
     * index rather than the 16 raw bytes, so phase2 dedupe and emit
     * route wide keys through the source column (key_data[k]). */
    bool idx_wide[8] = {0};
    for (uint8_t k = 0; k < n_idx; k++)
        idx_wide[k] = (idx_vecs[k]->type == RAY_GUID);
    bool pvt_wide = (pcol->type == RAY_GUID);

    void*   key_data[8];
    int8_t  key_types[8];
    uint8_t key_attrs[8];
    ray_t*  key_vecs[8];
    for (uint8_t k = 0; k < n_idx; k++) {
        key_data[k]  = ray_data(idx_vecs[k]);
        key_types[k] = idx_vecs[k]->type;
        key_attrs[k] = idx_vecs[k]->attrs;
        key_vecs[k]  = idx_vecs[k];
    }
    key_data[n_idx]  = ray_data(pcol);
    key_types[n_idx] = pcol->type;
    key_attrs[n_idx] = pcol->attrs;
    key_vecs[n_idx]  = pcol;

    /* Single agg input: value column */
    ray_t* agg_vecs[1] = { vcol };
    uint16_t agg_ops[1] = { agg_op };

    /* Compute need_flags for the agg op */
    uint8_t need_flags = GHT_NEED_SUM; /* always need sum (used for FIRST/LAST too) */
    if (agg_op == OP_MIN) need_flags |= GHT_NEED_MIN;
    if (agg_op == OP_MAX) need_flags |= GHT_NEED_MAX;

    ght_layout_t ly = ght_compute_layout(n_keys, 1, agg_vecs, need_flags, agg_ops, key_types);

    /* Hash-aggregate all rows via the shared radix pipeline — parallel
     * across thread-pool workers for n_scan ≥ RAY_PARALLEL_THRESHOLD,
     * sequential single-HT for smaller inputs. */
    ray_progress_update("pivot", "hash-aggregate", 0, (uint64_t)nrows);
    pivot_ingest_t pg;
    if (!pivot_ingest_run(&pg, &ly, key_data, key_types, key_attrs,
                          key_vecs, agg_vecs, nrows)) {
        pivot_ingest_free(&pg);
        return ray_error("oom", NULL);
    }
    ray_progress_update("pivot", "dedupe", 0, (uint64_t)pg.total_grps);
    if (ray_interrupted()) { pivot_ingest_free(&pg); return ray_error("cancel", "interrupted"); }
    uint32_t grp_count = pg.total_grps;
    if (grp_count == 0) { pivot_ingest_free(&pg); return ray_table_new(0); }

    /* Phase 2: Collect distinct pivot values and distinct index keys.
     * Each group row layout: [hash:8][key0:8]...[keyN-1:8][null_mask:8][accum...]
     * where the keys region holds n_idx index keys + 1 pivot key,
     * followed by the key-null bitmap written by group_rows_range. */

    /* SQL PIVOT treats a null pivot key as "no column" — drop those groups. */
    const uint8_t pvt_null_bit = (uint8_t)(1u << n_idx);

    /* Collect distinct pivot values */
    uint32_t pv_cap = 64, pv_count = 0;
    ray_t* pv_hdr = NULL;
    int64_t* pv_vals = (int64_t*)scratch_alloc(&pv_hdr, pv_cap * sizeof(int64_t));
    if (!pv_vals) { pivot_ingest_free(&pg); return ray_error("oom", NULL); }

    const char* pvt_base = pvt_wide ? (const char*)key_data[n_idx] : NULL;
    for (uint32_t _p = 0; _p < pg.n_parts; _p++) {
        group_ht_t* ph = &pg.part_hts[_p];
        uint32_t pcount = ph->grp_count;
        for (uint32_t gi_local = 0; gi_local < pcount; gi_local++) {
            const char* row = ph->rows + (size_t)gi_local * pg.row_stride;
            const int64_t* rkeys = (const int64_t*)(row + 8);
            int64_t nmask = rkeys[n_keys];
            if (nmask & pvt_null_bit) continue;
            int64_t pval = rkeys[n_idx];
            bool found = false;
            for (uint32_t p = 0; p < pv_count; p++) {
                if (pvt_wide) {
                    if (memcmp(pvt_base + (size_t)pv_vals[p] * 16,
                               pvt_base + (size_t)pval * 16, 16) == 0) { found = true; break; }
                } else {
                    if (pv_vals[p] == pval) { found = true; break; }
                }
            }
            if (!found) {
                if (pv_count >= pv_cap) {
                    uint32_t new_cap = pv_cap * 2;
                    int64_t* new_pv = (int64_t*)scratch_realloc(&pv_hdr,
                        pv_cap * sizeof(int64_t), new_cap * sizeof(int64_t));
                    if (!new_pv) { pivot_ingest_free(&pg); return ray_error("oom", NULL); }
                    pv_vals = new_pv;
                    pv_cap = new_cap;
                }
                pv_vals[pv_count++] = pval;
            }
        }
    }

    /* Collect distinct index keys.
     * Flat append-only entry array + secondary open-addressed HT keyed by
     * the hash of (idx_keys + idx_null_mask). The HT makes phase2 dedupe
     * O(grp_count) instead of the previous O(grp_count * ix_count)
     * linear scan which hung on large pivots.
     * Entry layout: [hash:8 | idx_keys:8*n_idx | idx_null_mask:8]. */
    uint32_t ix_cap = 256, ix_count = 0;
    ray_t* ix_hdr = NULL;
    size_t ix_entry = 8 + (size_t)n_idx * 8 + 8;
    const uint8_t idx_null_bits = (uint8_t)((1u << n_idx) - 1u);
    char* ix_rows = (char*)scratch_alloc(&ix_hdr, ix_cap * ix_entry);
    if (!ix_rows) { scratch_free(pv_hdr); pivot_ingest_free(&pg); return ray_error("oom", NULL); }

    /* Secondary HT: hash slot -> ix_row index; empty = UINT32_MAX. */
    uint32_t ix_ht_cap = 256;
    while (ix_ht_cap < (uint32_t)grp_count * 2 && ix_ht_cap < (1u << 30)) ix_ht_cap <<= 1;
    ray_t* ix_ht_hdr = NULL;
    uint32_t* ix_ht = (uint32_t*)scratch_alloc(&ix_ht_hdr, ix_ht_cap * sizeof(uint32_t));
    if (!ix_ht) {
        scratch_free(ix_hdr); scratch_free(pv_hdr); pivot_ingest_free(&pg);
        return ray_error("oom", NULL);
    }
    memset(ix_ht, 0xFF, ix_ht_cap * sizeof(uint32_t));
    uint32_t ix_ht_mask = ix_ht_cap - 1;

    /* Map: group_id -> (ix_row, pv_idx) for result cell placement */
    ray_t* map_hdr = NULL;
    uint32_t* grp_ix  = (uint32_t*)scratch_alloc(&map_hdr, grp_count * 2 * sizeof(uint32_t));
    if (!grp_ix) { scratch_free(ix_ht_hdr); scratch_free(ix_hdr); scratch_free(pv_hdr); pivot_ingest_free(&pg); return ray_error("oom", NULL); }
    uint32_t* grp_pv = grp_ix + grp_count;

    for (uint32_t _p = 0; _p < pg.n_parts; _p++) {
        group_ht_t* ph = &pg.part_hts[_p];
        uint32_t pcount = ph->grp_count;
        uint32_t gi_base = pg.part_offsets[_p];
        /* Progress tick at each partition boundary — time-gated so
         * 256 small partitions do not spam the callback. */
        ray_progress_update(NULL, NULL, gi_base, (uint64_t)grp_count);
        for (uint32_t gi_local = 0; gi_local < pcount; gi_local++) {
            uint32_t gi = gi_base + gi_local;
            const char* row = ph->rows + (size_t)gi_local * pg.row_stride;
            const int64_t* keys = (const int64_t*)(row + 8);
            int64_t nmask = keys[n_keys];
            if (nmask & pvt_null_bit) {
                grp_ix[gi] = UINT32_MAX;
                grp_pv[gi] = UINT32_MAX;
                continue;
            }
        int64_t idx_nmask = nmask & idx_null_bits;

        /* Hash index keys only (exclude pivot key) + null mask.
         * Wide keys (GUID) resolve actual bytes via key_data[k]. */
        uint64_t ih = 0;
        for (uint8_t k = 0; k < n_idx; k++) {
            uint64_t kh;
            if (idx_wide[k]) {
                const char* base = (const char*)key_data[k];
                kh = ray_hash_bytes(base + (size_t)keys[k] * 16, 16);
            } else if (key_types[k] == RAY_F64) {
                kh = ray_hash_f64(*(const double*)&keys[k]);
            } else {
                kh = ray_hash_i64(keys[k]);
            }
            ih = (k == 0) ? kh : ray_hash_combine(ih, kh);
        }
        if (idx_nmask) ih = ray_hash_combine(ih, ray_hash_i64(idx_nmask));

        /* Open-addressed HT probe. On match, reuse; else insert. */
        uint32_t ix_row = UINT32_MAX;
        uint32_t slot = (uint32_t)(ih & ix_ht_mask);
        for (;;) {
            uint32_t ent = ix_ht[slot];
            if (ent == UINT32_MAX) break; /* empty → insert below */
            const char* ix_entry_p = ix_rows + (size_t)ent * ix_entry;
            if (*(const uint64_t*)ix_entry_p == ih) {
                const int64_t* ekeys = (const int64_t*)(ix_entry_p + 8);
                bool eq = true;
                for (uint8_t k = 0; k < n_idx && eq; k++) {
                    if (idx_wide[k]) {
                        const char* base = (const char*)key_data[k];
                        eq = (memcmp(base + (size_t)ekeys[k] * 16,
                                      base + (size_t)keys[k] * 16, 16) == 0);
                    } else {
                        eq = (ekeys[k] == keys[k]);
                    }
                }
                int64_t ent_nmask;
                memcpy(&ent_nmask, ix_entry_p + 8 + (size_t)n_idx * 8, 8);
                if (eq && ent_nmask == idx_nmask) { ix_row = ent; break; }
            }
            slot = (slot + 1) & ix_ht_mask;
        }
        if (ix_row == UINT32_MAX) {
            if (ix_count >= ix_cap) {
                uint32_t new_cap = ix_cap * 2;
                char* new_rows = (char*)scratch_realloc(&ix_hdr,
                    ix_cap * ix_entry, new_cap * ix_entry);
                if (!new_rows) {
                    scratch_free(map_hdr); scratch_free(ix_ht_hdr);
                    scratch_free(pv_hdr); pivot_ingest_free(&pg);
                    return ray_error("oom", NULL);
                }
                ix_rows = new_rows;
                ix_cap = new_cap;
            }
            ix_row = ix_count++;
            char* dst = ix_rows + (size_t)ix_row * ix_entry;
            *(uint64_t*)dst = ih;
            memcpy(dst + 8, keys, (size_t)n_idx * 8);
            memcpy(dst + 8 + (size_t)n_idx * 8, &idx_nmask, 8);
            ix_ht[slot] = ix_row;
        }

        /* Find pivot column index. For wide pivot keys both slot values
         * are source row indices — resolve to actual bytes for compare,
         * otherwise duplicate GUID pivot values map to the wrong column. */
        int64_t pval = keys[n_idx];
        uint32_t pv_idx = UINT32_MAX;
        for (uint32_t p = 0; p < pv_count; p++) {
            if (pvt_wide) {
                if (memcmp(pvt_base + (size_t)pv_vals[p] * 16,
                           pvt_base + (size_t)pval * 16, 16) == 0) { pv_idx = p; break; }
            } else {
                if (pv_vals[p] == pval) { pv_idx = p; break; }
            }
        }

            grp_ix[gi] = ix_row;
            grp_pv[gi] = pv_idx;
        }
    }

    /* Phase 3: Build output table */
    ray_progress_update("pivot", "scatter", 0, (uint64_t)pv_count);
    bool val_is_f64 = vcol->type == RAY_F64;
    int8_t out_agg_type;
    switch (agg_op) {
        case OP_AVG:   out_agg_type = RAY_F64; break;
        case OP_COUNT: out_agg_type = RAY_I64; break;
        case OP_SUM:   out_agg_type = val_is_f64 ? RAY_F64 : RAY_I64; break;
        default:       out_agg_type = vcol->type; break;
    }

    int64_t out_ncols = (int64_t)n_idx + (int64_t)pv_count;
    ray_t* result = ray_table_new(out_ncols);
    if (!result || RAY_IS_ERR(result)) goto pivot_cleanup;

    /* Index columns */
    for (uint8_t k = 0; k < n_idx; k++) {
        ray_t* new_col = col_vec_new(idx_vecs[k], (int64_t)ix_count);
        if (!new_col || RAY_IS_ERR(new_col)) { ray_release(result); result = ray_error("oom", NULL); goto pivot_cleanup; }
        new_col->len = (int64_t)ix_count;
        uint8_t esz = col_esz(idx_vecs[k]);
        int8_t kt = idx_vecs[k]->type;
        const char* src_base = idx_wide[k] ? (const char*)key_data[k] : NULL;
        for (uint32_t r = 0; r < ix_count; r++) {
            const char* ix_entry_p = ix_rows + r * ix_entry;
            int64_t kv = ((const int64_t*)(ix_entry_p + 8))[k];
            int64_t ent_nmask;
            memcpy(&ent_nmask, ix_entry_p + 8 + (size_t)n_idx * 8, 8);
            if (ent_nmask & (int64_t)(1u << k)) {
                ray_vec_set_null(new_col, (int64_t)r, true);
                continue;
            }
            if (idx_wide[k]) {
                /* kv is a source row index; copy the 16 raw bytes. */
                memcpy((char*)ray_data(new_col) + (size_t)r * esz,
                       src_base + (size_t)kv * 16, 16);
            } else if (kt == RAY_F64) {
                memcpy((char*)ray_data(new_col) + (size_t)r * esz, &kv, 8);
            } else {
                write_col_i64(ray_data(new_col), (int64_t)r, kv, kt, new_col->attrs);
            }
        }
        if (idx_vecs[k]->type == RAY_STR)
            col_propagate_str_pool(new_col, idx_vecs[k]);

        ray_op_ext_t* ie = find_ext(g, ext->pivot.index_cols[k]->id);
        result = ray_table_add_col(result, ie->sym, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) goto pivot_cleanup;
    }

    /* Value columns — one per distinct pivot value */
    {
    int8_t s = ly.agg_val_slot[0]; /* single agg input -> slot 0 */
    for (uint32_t p = 0; p < pv_count; p++) {
        ray_t* new_col = (out_agg_type == vcol->type)
                        ? col_vec_new(vcol, (int64_t)ix_count)
                        : ray_vec_new(out_agg_type, (int64_t)ix_count);
        if (!new_col || RAY_IS_ERR(new_col)) { ray_release(result); result = ray_error("oom", NULL); goto pivot_cleanup; }
        new_col->len = (int64_t)ix_count;

        /* Initialize with zero (missing cells get 0) */
        memset(ray_data(new_col), 0, (size_t)ix_count * (out_agg_type == RAY_F64 ? 8 : (size_t)col_esz(new_col)));

        for (uint32_t _pp = 0; _pp < pg.n_parts; _pp++) {
            group_ht_t* ph = &pg.part_hts[_pp];
            uint32_t pcount = ph->grp_count;
            uint32_t gi_base = pg.part_offsets[_pp];
            for (uint32_t gi_local = 0; gi_local < pcount; gi_local++) {
                uint32_t gi = gi_base + gi_local;
                if (grp_pv[gi] != p) continue;
                uint32_t r = grp_ix[gi];
                const char* row = ph->rows + (size_t)gi_local * pg.row_stride;
                int64_t cnt = *(const int64_t*)(const void*)row;

            if (out_agg_type == RAY_F64) {
                double v;
                switch (agg_op) {
                    case OP_SUM:
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_sum, s)
                                       : (double)ROW_RD_I64(row, ly.off_sum, s);
                        break;
                    case OP_AVG:
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_sum, s) / cnt
                                       : (double)ROW_RD_I64(row, ly.off_sum, s) / cnt;
                        break;
                    case OP_MIN:
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_min, s)
                                       : (double)ROW_RD_I64(row, ly.off_min, s);
                        break;
                    case OP_MAX:
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_max, s)
                                       : (double)ROW_RD_I64(row, ly.off_max, s);
                        break;
                    case OP_FIRST: case OP_LAST:
                        v = val_is_f64 ? ROW_RD_F64(row, ly.off_sum, s)
                                       : (double)ROW_RD_I64(row, ly.off_sum, s);
                        break;
                    default: v = 0.0; break;
                }
                ((double*)ray_data(new_col))[r] = v;
            } else {
                int64_t v;
                switch (agg_op) {
                    case OP_SUM:   v = ROW_RD_I64(row, ly.off_sum, s); break;
                    case OP_COUNT: v = cnt; break;
                    case OP_MIN:   v = ROW_RD_I64(row, ly.off_min, s); break;
                    case OP_MAX:   v = ROW_RD_I64(row, ly.off_max, s); break;
                    case OP_FIRST: case OP_LAST: v = ROW_RD_I64(row, ly.off_sum, s); break;
                    default:       v = 0; break;
                }
                    write_col_i64(ray_data(new_col), (int64_t)r, v, out_agg_type, new_col->attrs);
                }
            }
        }

        /* Column name from pivot value — match pivot_val_to_sym semantics */
        int64_t pval = pv_vals[p];
        int64_t col_sym;
        if (pcol->type == RAY_SYM) {
            col_sym = pval;
        } else if (pvt_wide) {
            /* GUID: format 16 bytes as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx.
             * pval is a source row index into pvt_base. */
            static const char hex[] = "0123456789abcdef";
            static const int groups[] = {4, 2, 2, 2, 6};
            char buf[37];
            const uint8_t* bytes = (const uint8_t*)pvt_base + (size_t)pval * 16;
            int pos = 0, bpos = 0;
            for (int g = 0; g < 5; g++) {
                if (g > 0) buf[bpos++] = '-';
                for (int j = 0; j < groups[g]; j++) {
                    buf[bpos++] = hex[bytes[pos] >> 4];
                    buf[bpos++] = hex[bytes[pos] & 0x0F];
                    pos++;
                }
            }
            col_sym = ray_sym_intern(buf, (size_t)bpos);
        } else {
            char buf[128];
            int len = 0;
            int8_t pt = key_types[n_idx];
            if (pt == RAY_F64) {
                double fv;
                memcpy(&fv, &pval, 8);
                if (fv == 0.0 && signbit(fv)) fv = 0.0;
                len = snprintf(buf, sizeof(buf), "%g", fv);
            } else if (pt == RAY_BOOL) {
                len = snprintf(buf, sizeof(buf), "%s", pval ? "true" : "false");
            } else if (pt == RAY_I64 || pt == RAY_I32 || pt == RAY_I16 ||
                       pt == RAY_DATE || pt == RAY_TIME || pt == RAY_TIMESTAMP) {
                len = snprintf(buf, sizeof(buf), "%ld", (long)pval);
            } else {
                len = snprintf(buf, sizeof(buf), "col%ld", (long)pval);
            }
            col_sym = ray_sym_intern(buf, (size_t)len);
        }

        result = ray_table_add_col(result, col_sym, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) goto pivot_cleanup;
    }
    }

pivot_cleanup:
    scratch_free(map_hdr);
    scratch_free(ix_ht_hdr);
    scratch_free(ix_hdr);
    scratch_free(pv_hdr);
    pivot_ingest_free(&pg);
    return result;
}
