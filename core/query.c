/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
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

#include "query.h"
#include "env.h"
#include "unary.h"
#include "eval.h"
#include "items.h"
#include "compose.h"
#include "error.h"
#include "iter.h"
#include "aggr.h"
#include "heap.h"
#include "math.h"
#include "misc.h"
#include "index.h"
#include "group.h"
#include "filter.h"
#include "chrono.h"
#include "runtime.h"
#include "symbols.h"


// ============================================================================
// Fused aggregation: pre-scan mappings and compute all aggregates in one pass
// ============================================================================

// Find column index by symbol id in table. Returns -1 if not found.
static i64_t find_col_idx(obj_p table, i64_t sym) {
    obj_p cols = AS_LIST(table)[0];
    i64_t n = cols->len;
    for (i64_t i = 0; i < n; i++)
        if (AS_I64(cols)[i] == sym)
            return i;
    return -1;
}

// Try to pre-scan all mapping expressions and run fused aggregation.
// Returns B8_TRUE on success (all mappings handled), B8_FALSE to fall back.
// Deferred binary operation over two aggregate results
typedef struct {
    i64_t out_sym;     // Output symbol ID
    i64_t plan_a;      // First operand plan index
    i64_t plan_b;      // Second operand plan index
    obj_p op_fn;       // Binary operation function object
} deferred_op_t;

// Check if an expression is (aggr_fn col_sym) or (aggr_fn expr) and add it to the plan.
// Returns the plan index on success, -1 on failure.
static i64_t try_add_aggr_to_plan(obj_p expr, obj_p table, fused_plan_t *plan, i64_t *nplan) {
    if (expr->type == TYPE_LIST && expr->len == 2) {
        obj_p fn_obj = AS_LIST(expr)[0];
        obj_p arg = AS_LIST(expr)[1];
        if ((fn_obj->type == TYPE_UNARY || fn_obj->type == TYPE_BINARY || fn_obj->type == TYPE_VARY)
            && (fn_obj->attrs & FN_AGGR)) {
            i8_t func_id = aggr_identify_func(fn_obj);
            if (func_id >= 0 && func_id != AGGR_ID_MED) {
                // Pattern A: (aggr col_sym) — direct column reference
                if (arg->type == -TYPE_SYMBOL) {
                    i64_t col_idx = find_col_idx(table, arg->i64);
                    if (col_idx >= 0) {
                        i64_t idx = *nplan;
                        plan[idx].func_id = func_id;
                        plan[idx].col_idx = col_idx;
                        plan[idx].col_ptr = NULL;
                        (*nplan)++;
                        return idx;
                    }
                }
                // Pattern B: (aggr expr) — pre-evaluate expression as virtual column
                else {
                    obj_p vcol = eval(arg);
                    if (!IS_ERR(vcol) && vcol->type >= TYPE_B8 && vcol->type <= TYPE_F64) {
                        i64_t idx = *nplan;
                        plan[idx].func_id = func_id;
                        plan[idx].col_idx = -1;
                        plan[idx].col_ptr = vcol;  // Ownership transferred to plan
                        (*nplan)++;
                        return idx;
                    }
                    if (!IS_ERR(vcol)) drop_obj(vcol);
                }
            }
        }
    }
    return -1;
}

static b8_t try_fused_aggregate(obj_p obj, query_ctx_p ctx) {
    obj_p map_keys = ray_except(AS_LIST(obj)[0], runtime_get()->env.keywords);
    i64_t nmaps = map_keys->len;

    // No explicit mappings is OK — key assembly handles everything
    if (nmaps == 0) {
        drop_obj(map_keys);

        // Still need to build groups for key assembly
        fused_plan_t dummy;
        aggr_fused_compute((struct query_ctx_t *)ctx, &dummy, 0, NULL);
        return B8_TRUE;
    }

    fused_plan_t plan[MAX_FUSED];
    i64_t syms[MAX_FUSED];
    i64_t nplan = 0;

    // Deferred binary ops: (op (aggr col1) (aggr col2))
    deferred_op_t deferred[MAX_FUSED];
    i64_t ndeferred = 0;

    if (nmaps > MAX_FUSED / 2) {
        drop_obj(map_keys);
        return B8_FALSE;
    }

    for (i64_t i = 0; i < nmaps; i++) {
        obj_p sym = at_idx(map_keys, i);
        obj_p expr = at_obj(obj, sym);
        i64_t sym_id = sym->i64;
        drop_obj(sym);

        b8_t matched = B8_FALSE;

        // Pattern 1: (aggr_fn col_sym) — e.g., (sum Price)
        if (expr->type == TYPE_LIST && expr->len == 2) {
            i64_t idx = try_add_aggr_to_plan(expr, ctx->table, plan, &nplan);
            if (idx >= 0) {
                syms[idx] = sym_id;
                matched = B8_TRUE;
            }
        }
        // Pattern 2: (binary_op (aggr col1) (aggr col2)) — e.g., (- (max v1) (min v2))
        else if (expr->type == TYPE_LIST && expr->len == 3) {
            obj_p fn_obj = AS_LIST(expr)[0];
            obj_p arg_a = AS_LIST(expr)[1];
            obj_p arg_b = AS_LIST(expr)[2];

            if (fn_obj->type == TYPE_BINARY && !(fn_obj->attrs & FN_AGGR)
                && nplan + 2 <= MAX_FUSED && ndeferred < MAX_FUSED) {
                i64_t idx_a = try_add_aggr_to_plan(arg_a, ctx->table, plan, &nplan);
                if (idx_a >= 0) {
                    syms[idx_a] = -1;  // Internal — not directly exposed
                    i64_t idx_b = try_add_aggr_to_plan(arg_b, ctx->table, plan, &nplan);
                    if (idx_b >= 0) {
                        syms[idx_b] = -1;  // Internal
                        deferred[ndeferred].out_sym = sym_id;
                        deferred[ndeferred].plan_a = idx_a;
                        deferred[ndeferred].plan_b = idx_b;
                        deferred[ndeferred].op_fn = fn_obj;
                        ndeferred++;
                        matched = B8_TRUE;
                    }
                }
            }
        }
        // Pattern 3: bare column symbol — implicit first
        else if (expr->type == -TYPE_SYMBOL) {
            i64_t col_idx = find_col_idx(ctx->table, expr->i64);
            if (col_idx >= 0) {
                plan[nplan].func_id = AGGR_ID_FIRST;
                plan[nplan].col_idx = col_idx;
                plan[nplan].col_ptr = NULL;
                syms[nplan] = sym_id;
                nplan++;
                matched = B8_TRUE;
            }
        }

        drop_obj(expr);

        if (!matched) {
            // Clean up any virtual columns already allocated
            for (i64_t j = 0; j < nplan; j++)
                if (plan[j].col_ptr != NULL) drop_obj(plan[j].col_ptr);
            drop_obj(map_keys);
            return B8_FALSE;  // Can't fuse this mapping — fall back
        }
    }

    drop_obj(map_keys);

    // Run fused aggregation
    obj_p results[MAX_FUSED];
    aggr_fused_compute((struct query_ctx_t *)ctx, plan, nplan, results);

    // Free virtual columns (pre-evaluated expressions)
    for (i64_t i = 0; i < nplan; i++)
        if (plan[i].col_ptr != NULL) { drop_obj(plan[i].col_ptr); plan[i].col_ptr = NULL; }

    // Apply deferred binary operations element-wise
    obj_p deferred_results[MAX_FUSED];
    b8_t deferred_err = B8_FALSE;
    for (i64_t d = 0; d < ndeferred; d++) {
        obj_p ra = results[deferred[d].plan_a];
        obj_p rb = results[deferred[d].plan_b];
        deferred_results[d] = map_binary(deferred[d].op_fn, ra, rb);
        if (IS_ERR(deferred_results[d])) deferred_err = B8_TRUE;
    }

    if (deferred_err) {
        // Clean up all results and deferred results, return B8_FALSE to fall back
        for (i64_t i = 0; i < nplan; i++) drop_obj(results[i]);
        for (i64_t d = 0; d < ndeferred; d++)
            if (!IS_ERR(deferred_results[d])) drop_obj(deferred_results[d]);
        return B8_FALSE;
    }

    // Store results in ctx: direct aggregates first, then deferred ops
    i64_t nfused = 0;
    for (i64_t i = 0; i < nplan; i++) {
        if (syms[i] != -1) {
            ctx->fused[nfused].sym = syms[i];
            ctx->fused[nfused].result = results[i];
            nfused++;
        } else {
            drop_obj(results[i]);
        }
    }
    for (i64_t d = 0; d < ndeferred; d++) {
        ctx->fused[nfused].sym = deferred[d].out_sym;
        ctx->fused[nfused].result = deferred_results[d];
        nfused++;
    }
    ctx->nfused = nfused;

    return B8_TRUE;
}

obj_p get_gkeys(obj_p cols, obj_p obj) {
    i64_t i, l;
    obj_p x;

    switch (obj->type) {
        case -TYPE_SYMBOL:
            l = cols->len;
            for (i = 0; i < l; i++)
                if (AS_I64(cols)[i] == obj->i64)
                    return symboli64(obj->i64);
            return NULL_OBJ;
        case TYPE_LIST:
            l = obj->len;
            for (i = 0; i < l; i++) {
                x = get_gkeys(cols, AS_LIST(obj)[i]);
                if (x != NULL_OBJ)
                    return x;
            }
            return NULL_OBJ;
        case TYPE_DICT:
            x = AS_LIST(obj)[0];
            if (x->type != TYPE_SYMBOL)
                return err_type(0, 0, 0, 0);

            if (x->len == 1)
                return at_idx(AS_LIST(obj)[0], 0);

            return clone_obj(AS_LIST(obj)[0]);

        default:
            return NULL_OBJ;
    }
}

obj_p get_gvals(obj_p obj) {
    i64_t i, l;
    obj_p vals, v, r, res;

    switch (obj->type) {
        case TYPE_DICT:
            vals = AS_LIST(obj)[1];
            l = vals->len;

            if (l == 0)
                return NULL_OBJ;

            if (l == 1) {
                v = at_idx(vals, 0);
                res = eval(v);
                drop_obj(v);
                return res;
            }

            res = LIST(l);
            for (i = 0; i < l; i++) {
                v = at_idx(vals, i);
                r = eval(v);
                drop_obj(v);

                if (IS_ERR(r)) {
                    res->len = i;
                    drop_obj(res);
                    return r;
                }

                AS_LIST(res)
                [i] = r;
            }

            return res;
        default:
            return eval(obj);
    }
}

nil_t query_ctx_init(query_ctx_p ctx) {
    vm_p vm = VM;
    ctx->table = NULL_OBJ;
    ctx->take = NULL_OBJ;
    ctx->filter = NULL_OBJ;
    ctx->groupby = NULL_OBJ;
    ctx->group_keys = NULL_OBJ;
    ctx->orig_table = NULL_OBJ;
    ctx->filter_bool = NULL_OBJ;
    ctx->ngroups = 0;
    ctx->first_rows = NULL;
    ctx->last_rows = NULL;
    ctx->prebuilt_keys = NULL_OBJ;
    ctx->nfused = 0;
    ctx->parent = vm->query_ctx;
    vm->query_ctx = ctx;
}

nil_t query_ctx_destroy(query_ctx_p ctx) {
    VM->query_ctx = ctx->parent;

    drop_obj(ctx->table);
    drop_obj(ctx->take);
    drop_obj(ctx->filter);
    drop_obj(ctx->groupby);
    drop_obj(ctx->group_keys);
    drop_obj(ctx->orig_table);
    drop_obj(ctx->filter_bool);
    drop_obj(ctx->prebuilt_keys);

    if (ctx->first_rows) heap_free(ctx->first_rows);
    if (ctx->last_rows) heap_free(ctx->last_rows);
    for (i64_t i = 0; i < ctx->nfused; i++)
        drop_obj(ctx->fused[i].result);
}

obj_p select_fetch_table(obj_p obj, query_ctx_p ctx) {
    obj_p prm, val;

    prm = at_sym(obj, "from", 4);

    if (is_null(prm))
        return err_value(symbols_intern("from", 4));

    val = eval(prm);
    drop_obj(prm);

    if (IS_ERR(val))
        return val;

    if (val->type != TYPE_TABLE) {
        i8_t actual_type = val->type;
        drop_obj(val);
        return err_type(TYPE_TABLE, actual_type, 0, symbols_intern("from", 4));
    }

    ctx->table = val;

    prm = at_sym(obj, "take", 4);

    if (!is_null(prm)) {
        val = eval(prm);
        drop_obj(prm);

        if (IS_ERR(val))
            return val;

        ctx->take = val;
    }

    timeit_tick("fetch table");

    return NULL_OBJ;
}

obj_p select_apply_filters(obj_p obj, query_ctx_p ctx) {
    obj_p prm, val, fil;

    timeit_span_start("filters");

    prm = at_sym(obj, "where", 5);
    if (prm != NULL_OBJ) {
        val = eval(prm);
        timeit_tick("eval filters");
        drop_obj(prm);

        if (IS_ERR(val))
            return val;

        if (val->type == TYPE_B8) {
            // Store boolean vector for fused-filter aggregation path.
            // Defer ray_where — only build indices lazily if fallback needs them.
            ctx->filter_bool = clone_obj(val);
            drop_obj(val);
        } else {
            // Non-boolean filter (parted, etc.) — must build indices now
            fil = ray_where(val);
            timeit_tick("find indices");
            drop_obj(val);

            if (IS_ERR(fil))
                return fil;

            ctx->filter = fil;
        }
    }

    timeit_span_end("filters");

    return NULL_OBJ;
}

obj_p select_apply_groupings(obj_p obj, query_ctx_p ctx) {
    obj_p prm, val, gkeys = NULL_OBJ, gvals = NULL_OBJ, groupby = NULL_OBJ, gcol = NULL_OBJ;
    b8_t by_is_dict;

    // Materialize parted table columns into a NEW table (avoid mutating the global table).
    // Handles both unfiltered and filtered-parted paths uniformly.
    {
        obj_p tab_vals = AS_LIST(ctx->table)[1];
        i64_t nc = tab_vals->len;
        b8_t has_parted = B8_FALSE;

        for (i64_t mc = 0; mc < nc; mc++) {
            obj_p col = AS_LIST(tab_vals)[mc];
            if (col->type == TYPE_MAPCOMMON || col->type >= TYPE_PARTEDLIST) {
                has_parted = B8_TRUE;
                break;
            }
        }

        if (has_parted) {
            obj_p tab_keys = AS_LIST(ctx->table)[0];

            // Extract partition sizes from MAPCOMMON before materializing
            i64_t nparts = 0;
            i64_t *psizes = NULL;
            for (i64_t mc = 0; mc < nc; mc++) {
                obj_p col = AS_LIST(tab_vals)[mc];
                if (col->type == TYPE_MAPCOMMON) {
                    obj_p ucnts = AS_LIST(col)[1];
                    nparts = ucnts->len;
                    psizes = AS_I64(ucnts);
                    break;
                }
            }

            // Build new table with materialized flat columns
            obj_p new_vals = LIST(nc);
            for (i64_t mc = 0; mc < nc; mc++) {
                obj_p col = AS_LIST(tab_vals)[mc];
                if (col->type == TYPE_MAPCOMMON) {
                    obj_p uvals = AS_LIST(col)[0];
                    obj_p ucnts = AS_LIST(col)[1];
                    i64_t np = uvals->len;
                    i64_t total = ops_count(col);
                    obj_p flat = vector(uvals->type, total);
                    i64_t off = 0;
                    for (i64_t pi = 0; pi < np; pi++) {
                        i64_t cnt = AS_I64(ucnts)[pi];
                        for (i64_t ri = 0; ri < cnt; ri++) {
                            switch (uvals->type) {
                                case TYPE_DATE: case TYPE_TIME: case TYPE_I32:
                                    AS_I32(flat)[off + ri] = AS_I32(uvals)[pi]; break;
                                case TYPE_I64: case TYPE_TIMESTAMP:
                                    AS_I64(flat)[off + ri] = AS_I64(uvals)[pi]; break;
                                default:
                                    AS_I64(flat)[off + ri] = AS_I64(uvals)[pi]; break;
                            }
                        }
                        off += cnt;
                    }
                    AS_LIST(new_vals)[mc] = flat;
                } else if (col->type >= TYPE_PARTEDLIST) {
                    obj_p flat = ray_value(col);
                    // Strip temporal types to base storage types
                    if (flat->type == TYPE_TIME || flat->type == TYPE_DATE) flat->type = TYPE_I32;
                    else if (flat->type == TYPE_TIMESTAMP) flat->type = TYPE_I64;
                    AS_LIST(new_vals)[mc] = flat;
                } else {
                    AS_LIST(new_vals)[mc] = clone_obj(col);
                }
            }

            // Flatten parted filter (TYPE_PARTEDI64) to regular I64 indices
            if (ctx->filter != NULL_OBJ && ctx->filter->type == TYPE_PARTEDI64 && psizes) {
                i64_t total = 0;
                for (i64_t pi = 0; pi < nparts; pi++) {
                    obj_p idx = AS_LIST(ctx->filter)[pi];
                    if (idx == NULL_OBJ) continue;
                    if (idx->type == -TYPE_I64 && idx->i64 == -1)
                        total += psizes[pi];
                    else if (idx->len > 0)
                        total += idx->len;
                }

                obj_p flat_filter = vector(TYPE_I64, total);
                i64_t off = 0, fpos = 0;
                for (i64_t pi = 0; pi < nparts; pi++) {
                    obj_p idx = AS_LIST(ctx->filter)[pi];
                    if (idx == NULL_OBJ) {
                        off += psizes[pi];
                        continue;
                    }
                    if (idx->type == -TYPE_I64 && idx->i64 == -1) {
                        for (i64_t ri = 0; ri < psizes[pi]; ri++)
                            AS_I64(flat_filter)[fpos++] = off + ri;
                    } else if (idx->len > 0) {
                        for (i64_t ri = 0; ri < idx->len; ri++)
                            AS_I64(flat_filter)[fpos++] = off + AS_I64(idx)[ri];
                    }
                    off += psizes[pi];
                }

                drop_obj(ctx->filter);
                ctx->filter = flat_filter;
            }

            // Replace table with materialized version
            obj_p new_table = table(clone_obj(tab_keys), new_vals);
            drop_obj(ctx->table);
            ctx->table = new_table;
        }
    }

    prm = at_sym(obj, "by", 2);
    if (prm != NULL_OBJ) {
        timeit_span_start("group");

        by_is_dict = (prm->type == TYPE_DICT);
        gkeys = get_gkeys(AS_LIST(ctx->table)[0], prm);
        groupby = get_gvals(prm);

        if (gkeys == NULL_OBJ)
            gkeys = symbol("By", 2);
        else if (!by_is_dict)
            gvals = eval(gkeys);

        drop_obj(prm);

        if (IS_ERR(groupby)) {
            drop_obj(gkeys);
            drop_obj(gvals);
            timeit_span_end("group");
            return groupby;
        }

        timeit_tick("get keys");

        // Store key columns for fused hash-aggregate
        // Only treat as multi-column if the "by:" parameter was a dict
        // A TYPE_LIST result from a simple "by: Name" is a single string column
        if (by_is_dict && groupby->type == TYPE_LIST) {
            ctx->groupby = clone_obj(groupby);
        } else {
            ctx->groupby = LIST(1);
            AS_LIST(ctx->groupby)[0] = clone_obj(groupby);
        }

        // Materialize parted key columns for fused hash path
        {
            i64_t nk = ctx->groupby->len;
            for (i64_t mk = 0; mk < nk; mk++) {
                obj_p kcol = AS_LIST(ctx->groupby)[mk];
                if (kcol->type == TYPE_MAPCOMMON) {
                    obj_p uvals = AS_LIST(kcol)[0];
                    obj_p ucnts = AS_LIST(kcol)[1];
                    i64_t nparts = uvals->len;
                    i64_t total = ops_count(kcol);
                    obj_p flat = vector(uvals->type, total);
                    i64_t off = 0;
                    for (i64_t pi = 0; pi < nparts; pi++) {
                        i64_t cnt = AS_I64(ucnts)[pi];
                        for (i64_t ri = 0; ri < cnt; ri++) {
                            switch (uvals->type) {
                                case TYPE_DATE: case TYPE_TIME: case TYPE_I32:
                                    AS_I32(flat)[off + ri] = AS_I32(uvals)[pi]; break;
                                case TYPE_I64: case TYPE_TIMESTAMP:
                                    AS_I64(flat)[off + ri] = AS_I64(uvals)[pi]; break;
                                default:
                                    AS_I64(flat)[off + ri] = AS_I64(uvals)[pi]; break;
                            }
                        }
                        off += cnt;
                    }
                    drop_obj(kcol);
                    AS_LIST(ctx->groupby)[mk] = flat;
                } else if (kcol->type >= TYPE_PARTEDLIST) {
                    obj_p flat = ray_value(kcol);
                    drop_obj(kcol);
                    AS_LIST(ctx->groupby)[mk] = flat;
                }
            }
        }

        // If filter is set, apply it before grouping
        if (ctx->filter_bool != NULL_OBJ && ctx->filter_bool->type == TYPE_B8) {
            // Boolean-fused path: keys stay unfiltered.
            // aggr_fused_compute will use filter_bool to skip non-matching rows.
            // No key filtering, no orig_table, no index needed.
        } else if (ctx->filter != NULL_OBJ) {
            // Index-based path: filter key columns through fids
            i64_t nf = ctx->filter->len;
            i64_t* fids = AS_I64(ctx->filter);
            i64_t nk = ctx->groupby->len;
            for (i64_t fk = 0; fk < nk; fk++) {
                obj_p filtered = at_ids(AS_LIST(ctx->groupby)[fk], fids, nf);
                drop_obj(AS_LIST(ctx->groupby)[fk]);
                AS_LIST(ctx->groupby)[fk] = filtered;
            }
            ctx->orig_table = clone_obj(ctx->table);
        }

        drop_obj(gvals);
        drop_obj(groupby);

        // Store key column names for result table assembly
        ctx->group_keys = gkeys;

        // Try fused aggregation (single-pass)
        if (!try_fused_aggregate(obj, ctx)) {
            // Fused failed — materialize filtered table for fallback path
            if (ctx->filter_bool != NULL_OBJ && ctx->filter_bool->type == TYPE_B8
                && ctx->orig_table == NULL_OBJ) {
                // Boolean path failed — lazily build filter indices now
                obj_p fil = ray_where(ctx->filter_bool);
                if (IS_ERR(fil)) { timeit_span_end("group"); return fil; }
                ctx->filter = fil;
                i64_t nf = fil->len;
                i64_t *fids = AS_I64(fil);
                i64_t nk = ctx->groupby->len;
                for (i64_t fk = 0; fk < nk; fk++) {
                    obj_p filtered = at_ids(AS_LIST(ctx->groupby)[fk], fids, nf);
                    drop_obj(AS_LIST(ctx->groupby)[fk]);
                    AS_LIST(ctx->groupby)[fk] = filtered;
                }
                // Also materialize table values for fallback
                obj_p tab_keys_f = AS_LIST(ctx->table)[0];
                obj_p tab_vals_f = AS_LIST(ctx->table)[1];
                i64_t nc = tab_keys_f->len;
                obj_p fvals = LIST(nc);
                for (i64_t fi = 0; fi < nc; fi++)
                    AS_LIST(fvals)[fi] = at_ids(AS_LIST(tab_vals_f)[fi], fids, nf);
                obj_p new_keys = clone_obj(tab_keys_f);
                drop_obj(ctx->table);
                ctx->table = table(new_keys, fvals);
            } else if (ctx->orig_table != NULL_OBJ && ctx->filter != NULL_OBJ) {
                i64_t nf = ctx->filter->len;
                i64_t *fids = AS_I64(ctx->filter);
                obj_p tab_keys_f = AS_LIST(ctx->table)[0];
                obj_p tab_vals_f = AS_LIST(ctx->table)[1];
                i64_t nc = tab_keys_f->len;
                obj_p fvals = LIST(nc);
                for (i64_t fi = 0; fi < nc; fi++)
                    AS_LIST(fvals)[fi] = at_ids(AS_LIST(tab_vals_f)[fi], fids, nf);
                drop_obj(ctx->table);
                ctx->table = table(clone_obj(tab_keys_f), fvals);
            }
            // Filter fully applied — clear stale state so MAT_COL uses
            // group_map's MAPGROUP columns instead of empty-vector shortcut
            if (ctx->filter_bool != NULL_OBJ) {
                drop_obj(ctx->filter_bool);
                ctx->filter_bool = NULL_OBJ;
            }
            if (ctx->orig_table != NULL_OBJ) {
                drop_obj(ctx->orig_table);
                ctx->orig_table = NULL_OBJ;
            }
            // Fallback: old path with group_map
            prm = group_map(ctx->table, NULL_OBJ);

            if (IS_ERR(prm)) {
                timeit_span_end("group");
                return prm;
            }

            // Replace table with remapped table for column resolution
            drop_obj(ctx->table);
            ctx->table = prm;
        }

        if (IS_ERR(gcol)) {
            timeit_span_end("group");
            return gcol;
        }

        timeit_span_end("group");
    } else if (ctx->filter != NULL_OBJ || ctx->filter_bool != NULL_OBJ) {
        // Remap filtered table for column resolution
        if (ctx->filter == NULL_OBJ && ctx->filter_bool != NULL_OBJ) {
            // Lazily build filter indices from boolean vector
            obj_p fil = ray_where(ctx->filter_bool);
            if (IS_ERR(fil)) return fil;
            ctx->filter = fil;
        }
        val = filter_map(ctx->table, ctx->filter);

        if (IS_ERR(val))
            return val;

        // Replace table with filtered table
        drop_obj(ctx->table);
        ctx->table = val;
    }

    return NULL_OBJ;
}

obj_p select_apply_mappings(obj_p obj, query_ctx_p ctx) {
    i64_t i, l;
    obj_p prm, sym, val, keys, res;

    // Fused path: return pre-computed results directly
    if (ctx->nfused > 0) {
        keys = ray_except(AS_LIST(obj)[0], runtime_get()->env.keywords);
        l = keys->len;
        res = LIST(l);
        for (i = 0; i < l; i++) {
            sym = at_idx(keys, i);
            i64_t sid = sym->i64;
            drop_obj(sym);
            // Find matching fused result
            for (i64_t j = 0; j < ctx->nfused; j++) {
                if (ctx->fused[j].sym == sid) {
                    AS_LIST(res)[i] = clone_obj(ctx->fused[j].result);
                    break;
                }
            }
        }
        return table(keys, res);
    }

    // Find all mappings (non-keyword fields)
    keys = ray_except(AS_LIST(obj)[0], runtime_get()->env.keywords);
    l = keys->len;

    // Mapppings specified
    if (l) {
        res = LIST(l);

        for (i = 0; i < l; i++) {
            sym = at_idx(keys, i);
            prm = at_obj(obj, sym);
            drop_obj(sym);
            val = eval(prm);
            drop_obj(prm);

            if (IS_ERR(val)) {
                res->len = i;
                drop_obj(res);
                drop_obj(keys);
                return val;
            }

            // Materialize fields
            switch (val->type) {
                case TYPE_MAPFILTER:
                    prm = filter_collect(AS_LIST(val)[0], AS_LIST(val)[1]);
                    drop_obj(val);
                    val = prm;
                    break;
                case TYPE_MAPGROUP:
                    prm = aggr_collect(AS_LIST(val)[0], AS_LIST(val)[1]);
                    drop_obj(val);
                    val = prm;
                    break;
                default:
                    prm = ray_value(val);
                    drop_obj(val);
                    val = prm;
                    break;
            }

            if (IS_ERR(val)) {
                res->len = i;
                drop_obj(res);
                drop_obj(keys);
                return val;
            }

            // Wrap atoms into 1-element vectors for proper table column format
            if (IS_ATOM(val)) {
                i8_t atype = -(val->type);
                prm = vector(atype, 1);
                switch (atype) {
                    case TYPE_F64:
                        AS_F64(prm)[0] = val->f64; break;
                    case TYPE_I64: case TYPE_TIMESTAMP:
                        AS_I64(prm)[0] = val->i64; break;
                    case TYPE_I32: case TYPE_DATE: case TYPE_TIME:
                        AS_I32(prm)[0] = (i32_t)val->i64; break;
                    case TYPE_I16:
                        AS_I16(prm)[0] = (i16_t)val->i64; break;
                    case TYPE_B8: case TYPE_U8:
                        AS_U8(prm)[0] = (u8_t)val->i64; break;
                    default:
                        AS_I64(prm)[0] = val->i64; break;
                }
                drop_obj(val);
                val = prm;
            }

            AS_LIST(res)[i] = val;
        }

        timeit_tick("apply mappings");

        return table(keys, res);
    }

    drop_obj(keys);

    return NULL_OBJ;
}

obj_p ray_select(obj_p obj) {
    obj_p res;
    struct query_ctx_t ctx;

    query_ctx_init(&ctx);

    if (obj->type != TYPE_DICT)
        return err_type(0, 0, 0, 0);

    if (AS_LIST(obj)[0]->type != TYPE_SYMBOL)
        return err_type(0, 0, 0, 0);

    timeit_span_start("select");

    // Fetch table - ctx.table is set, resolve() will find columns via query_ctx
    res = select_fetch_table(obj, &ctx);
    if (IS_ERR(res))
        goto cleanup;

    // Apply filters
    res = select_apply_filters(obj, &ctx);
    if (IS_ERR(res))
        goto cleanup;

    // Apply groupping
    res = select_apply_groupings(obj, &ctx);
    if (IS_ERR(res))
        goto cleanup;

    // Apply mappings
    res = select_apply_mappings(obj, &ctx);
    if (IS_ERR(res))
        goto cleanup;

    // Helper: get ki-th key symbol ID
    #define GKEY_SYM(gk, ki) (IS_ATOM(gk) ? (gk)->i64 : AS_I64(gk)[ki])

    // Helper: materialize a column in grouped context
    #define MAT_COL(col) \
        (ctx.ngroups > 0 ? at_ids((col), ctx.first_rows, ctx.ngroups) \
         : (ctx.orig_table != NULL_OBJ || ctx.filter_bool != NULL_OBJ) ? vector((col)->type, 0) \
         : (col)->type == TYPE_MAPGROUP ? aggr_collect(AS_LIST(col)[0], AS_LIST(col)[1]) \
         : ray_value(col))

    // If groupby is active, handle key column assembly
    if (ctx.groupby != NULL_OBJ && ctx.group_keys != NULL_OBJ) {
        obj_p tab_keys = AS_LIST(ctx.table)[0];
        obj_p tab_vals = AS_LIST(ctx.table)[1];
        i64_t ncols = tab_keys->len;
        i64_t nkeys = IS_ATOM(ctx.group_keys) ? 1 : ctx.group_keys->len;

        if (res == NULL_OBJ) {
            // No explicit mappings: build default result table
            // Key columns first, then remaining columns in original order
            obj_p rkeys = vector(TYPE_SYMBOL, ncols);
            obj_p rvals = LIST(ncols);
            i64_t ci, ki, ri = 0;

            for (ki = 0; ki < nkeys; ki++) {
                i64_t key_sym = GKEY_SYM(ctx.group_keys, ki);
                for (ci = 0; ci < ncols; ci++) {
                    if (AS_I64(tab_keys)[ci] == key_sym) {
                        obj_p mat;
                        if (ctx.prebuilt_keys != NULL_OBJ && ki < ctx.prebuilt_keys->len)
                            mat = clone_obj(AS_LIST(ctx.prebuilt_keys)[ki]);
                        else {
                            obj_p col = AS_LIST(tab_vals)[ci];
                            mat = MAT_COL(col);
                        }
                        if (IS_ERR(mat)) {
                            rvals->len = ri;
                            drop_obj(rvals);
                            drop_obj(rkeys);
                            res = mat;
                            goto cleanup;
                        }
                        AS_I64(rkeys)[ri] = key_sym;
                        AS_LIST(rvals)[ri] = mat;
                        ri++;
                        break;
                    }
                }
            }

            for (ci = 0; ci < ncols; ci++) {
                b8_t is_key = B8_FALSE;
                for (ki = 0; ki < nkeys; ki++) {
                    if (AS_I64(tab_keys)[ci] == GKEY_SYM(ctx.group_keys, ki)) {
                        is_key = B8_TRUE;
                        break;
                    }
                }
                if (is_key) continue;

                obj_p col = AS_LIST(tab_vals)[ci];
                obj_p mat = MAT_COL(col);
                if (IS_ERR(mat)) {
                    rvals->len = ri;
                    drop_obj(rvals);
                    drop_obj(rkeys);
                    res = mat;
                    goto cleanup;
                }
                AS_I64(rkeys)[ri] = AS_I64(tab_keys)[ci];
                AS_LIST(rvals)[ri] = mat;
                ri++;
            }

            res = table(rkeys, rvals);
        } else {
            // Explicit mappings: prepend key columns to the mapped result
            obj_p map_keys = AS_LIST(res)[0];
            obj_p map_vals = AS_LIST(res)[1];
            i64_t nmap = map_keys->len;
            i64_t total = nkeys + nmap;

            obj_p rkeys = vector(TYPE_SYMBOL, total);
            obj_p rvals = LIST(total);
            i64_t ki, ri = 0;

            // First: materialize key columns from the grouped table
            for (ki = 0; ki < nkeys; ki++) {
                i64_t key_sym = GKEY_SYM(ctx.group_keys, ki);
                i64_t ci;
                for (ci = 0; ci < ncols; ci++) {
                    if (AS_I64(tab_keys)[ci] == key_sym) {
                        obj_p mat;
                        if (ctx.prebuilt_keys != NULL_OBJ && ki < ctx.prebuilt_keys->len)
                            mat = clone_obj(AS_LIST(ctx.prebuilt_keys)[ki]);
                        else {
                            obj_p col = AS_LIST(tab_vals)[ci];
                            mat = MAT_COL(col);
                        }
                        if (IS_ERR(mat)) {
                            rvals->len = ri;
                            drop_obj(rvals);
                            drop_obj(rkeys);
                            drop_obj(res);
                            res = mat;
                            goto cleanup;
                        }
                        AS_I64(rkeys)[ri] = key_sym;
                        AS_LIST(rvals)[ri] = mat;
                        ri++;
                        break;
                    }
                }
            }

            // Then: append mapped columns
            for (i64_t mi = 0; mi < nmap; mi++) {
                AS_I64(rkeys)[ri] = AS_I64(map_keys)[mi];
                AS_LIST(rvals)[ri] = clone_obj(AS_LIST(map_vals)[mi]);
                ri++;
            }

            drop_obj(res);
            res = table(rkeys, rvals);
        }
    }

    #undef MAT_COL
    #undef GKEY_SYM

    // No explicit mappings and no groupby: materialize the (possibly filtered) table
    if (res == NULL_OBJ && ctx.table != NULL_OBJ) {
        obj_p tab_keys = AS_LIST(ctx.table)[0];
        obj_p tab_vals = AS_LIST(ctx.table)[1];
        i64_t ncols = tab_keys->len;
        obj_p rkeys = clone_obj(tab_keys);
        obj_p rvals = LIST(ncols);

        for (i64_t ci = 0; ci < ncols; ci++) {
            obj_p col = AS_LIST(tab_vals)[ci];
            obj_p mat;
            if (col->type == TYPE_MAPFILTER)
                mat = filter_collect(AS_LIST(col)[0], AS_LIST(col)[1]);
            else
                mat = ray_value(col);

            if (IS_ERR(mat)) {
                rvals->len = ci;
                drop_obj(rvals);
                drop_obj(rkeys);
                res = mat;
                goto cleanup;
            }

            AS_LIST(rvals)[ci] = mat;
        }

        res = table(rkeys, rvals);

        if (ctx.take != NULL_OBJ) {
            obj_p taken = ray_take(res, ctx.take);
            drop_obj(res);
            res = taken;
        }
    }

cleanup:
    query_ctx_destroy(&ctx);
    timeit_span_end("select");

    return res;
}
