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

#include <stdio.h>
#include "pivot.h"
#include "rayforce.h"
#include "error.h"
#include "items.h"
#include "compose.h"
#include "join.h"
#include "aggr.h"
#include "query.h"
#include "symbols.h"
#include "ops.h"
#include "cmp.h"
#include "unary.h"
#include "math.h"
#include "misc.h"

typedef obj_p (*aggr_fn)(obj_p val, obj_p index);

static aggr_fn get_aggr_func(obj_p func) {
    if (func->type != TYPE_UNARY)
        return NULL;

    unary_f fn = (unary_f)func->i64;
    if (fn == ray_sum)   return aggr_sum;
    if (fn == ray_avg)   return aggr_avg;
    if (fn == ray_min)   return aggr_min;
    if (fn == ray_max)   return aggr_max;
    if (fn == ray_med)   return aggr_med;
    if (fn == ray_count) return aggr_count;
    if (fn == ray_first) return aggr_first;
    if (fn == ray_last)  return aggr_last;

    return NULL;
}

static i64_t pivot_val_to_symbol(obj_p pivot_val, i64_t fallback_idx) {
    if (pivot_val->type == -TYPE_SYMBOL)
        return pivot_val->i64;

    char buf[64];
    i64_t len;
    switch (pivot_val->type) {
        case -TYPE_I64:
            len = snprintf(buf, sizeof(buf), "%lld", (long long)pivot_val->i64);
            break;
        case -TYPE_F64:
            len = snprintf(buf, sizeof(buf), "%g", pivot_val->f64);
            break;
        default:
            len = snprintf(buf, sizeof(buf), "col%lld", (long long)fallback_idx);
            break;
    }
    return symbols_intern(buf, len);
}

obj_p ray_pivot(obj_p* x, i64_t n) {
    if (n != 5)
        return err_arity(5, n, 0);

    obj_p tab = x[0];
    obj_p index = x[1];
    obj_p columns = x[2];
    obj_p values = x[3];
    obj_p aggfunc = x[4];

    if (tab->type != TYPE_TABLE)
        return err_type(TYPE_TABLE, tab->type, 1, 0);

    if (index->type != -TYPE_SYMBOL && index->type != TYPE_SYMBOL)
        return err_type(TYPE_SYMBOL, index->type, 2, 0);

    if (columns->type != -TYPE_SYMBOL)
        return err_type(-TYPE_SYMBOL, columns->type, 3, 0);

    if (values->type != -TYPE_SYMBOL)
        return err_type(-TYPE_SYMBOL, values->type, 4, 0);

    aggr_fn agg_func = get_aggr_func(aggfunc);
    if (agg_func == NULL) {
        b8_t is_fn = (aggfunc->type == TYPE_UNARY || aggfunc->type == TYPE_BINARY ||
                      aggfunc->type == TYPE_VARY || aggfunc->type == TYPE_LAMBDA);
        return is_fn ? err_domain(5, 0) : err_type(TYPE_LAMBDA, aggfunc->type, 5, 0);
    }

    b8_t single_index = (index->type == -TYPE_SYMBOL);

    obj_p pivot_col = ray_at(tab, columns);
    if (IS_ERR(pivot_col))
        return pivot_col;

    obj_p unique_vals = ray_distinct(pivot_col);
    if (IS_ERR(unique_vals)) {
        drop_obj(pivot_col);
        return unique_vals;
    }

    i64_t num_pivots = unique_vals->len;
    if (num_pivots == 0) {
        drop_obj(unique_vals);
        drop_obj(pivot_col);
        return err_domain(3, 0);
    }

    obj_p index_syms;
    if (single_index) {
        index_syms = vector(TYPE_SYMBOL, 1);
        AS_SYMBOL(index_syms)[0] = index->i64;
    } else {
        index_syms = clone_obj(index);
    }

    obj_p val_col = ray_at(tab, values);
    if (IS_ERR(val_col)) {
        drop_obj(unique_vals);
        drop_obj(pivot_col);
        drop_obj(index_syms);
        return val_col;
    }

    // Fetch index columns: single returns vector, multi returns LIST
    obj_p index_raw = ray_at(tab, index_syms);
    if (IS_ERR(index_raw)) {
        drop_obj(unique_vals);
        drop_obj(pivot_col);
        drop_obj(index_syms);
        drop_obj(val_col);
        return index_raw;
    }

    // Normalize: always work with LIST of key column vectors
    obj_p key_cols;
    if (single_index) {
        key_cols = LIST(1);
        AS_LIST(key_cols)[0] = index_raw;
    } else {
        key_cols = index_raw;
    }
    i64_t nkeys = key_cols->len;

    // Build base result table with ALL unique index combinations
    obj_p result;
    if (single_index) {
        obj_p unique_idx = ray_distinct(AS_LIST(key_cols)[0]);
        if (IS_ERR(unique_idx)) {
            drop_obj(unique_vals);
            drop_obj(pivot_col);
            drop_obj(index_syms);
            drop_obj(val_col);
            drop_obj(key_cols);
            return unique_idx;
        }
        result = table(clone_obj(index_syms), vn_list(1, unique_idx));
    } else {
        // Use query_ctx + aggr_first to get unique composite keys
        struct query_ctx_t base_ctx;
        query_ctx_init(&base_ctx);
        base_ctx.groupby = clone_obj(key_cols);

        obj_p base_vals = LIST(nkeys);
        for (i64_t k = 0; k < nkeys; k++) {
            obj_p gk = aggr_first(AS_LIST(key_cols)[k], NULL_OBJ);
            if (IS_ERR(gk)) {
                base_vals->len = k;
                drop_obj(base_vals);
                query_ctx_destroy(&base_ctx);
                drop_obj(unique_vals);
                drop_obj(pivot_col);
                drop_obj(index_syms);
                drop_obj(val_col);
                drop_obj(key_cols);
                return gk;
            }
            AS_LIST(base_vals)[k] = gk;
        }
        query_ctx_destroy(&base_ctx);
        result = table(clone_obj(index_syms), base_vals);
    }

    if (IS_ERR(result)) {
        drop_obj(unique_vals);
        drop_obj(pivot_col);
        drop_obj(index_syms);
        drop_obj(val_col);
        drop_obj(key_cols);
        return result;
    }

    for (i64_t i = 0; i < num_pivots; i++) {
        obj_p pivot_val = at_idx(unique_vals, i);

        // Filter: pivot_col == pivot_val
        obj_p mask = ray_eq(pivot_col, pivot_val);
        if (IS_ERR(mask)) {
            drop_obj(pivot_val);
            drop_obj(result);
            result = mask;
            goto cleanup;
        }

        obj_p filter_idx = ray_where(mask);
        drop_obj(mask);

        if (IS_ERR(filter_idx)) {
            drop_obj(pivot_val);
            drop_obj(result);
            result = filter_idx;
            goto cleanup;
        }

        if (filter_idx->len == 0) {
            drop_obj(filter_idx);
            drop_obj(pivot_val);
            continue;
        }

        i64_t nfiltered = filter_idx->len;
        i64_t *fids = AS_I64(filter_idx);

        // Extract filtered value column
        obj_p fval = at_ids(val_col, fids, nfiltered);
        if (IS_ERR(fval)) {
            drop_obj(filter_idx);
            drop_obj(pivot_val);
            drop_obj(result);
            result = fval;
            goto cleanup;
        }

        // Extract filtered key columns
        obj_p fkeys = LIST(nkeys);
        for (i64_t k = 0; k < nkeys; k++) {
            obj_p fk = at_ids(AS_LIST(key_cols)[k], fids, nfiltered);
            if (IS_ERR(fk)) {
                fkeys->len = k;
                drop_obj(fkeys);
                drop_obj(fval);
                drop_obj(filter_idx);
                drop_obj(pivot_val);
                drop_obj(result);
                result = fk;
                goto cleanup;
            }
            AS_LIST(fkeys)[k] = fk;
        }
        drop_obj(filter_idx);

        // Set up query context for group-by aggregation
        struct query_ctx_t pctx;
        query_ctx_init(&pctx);
        pctx.groupby = clone_obj(fkeys);

        // Run aggregation — reads VM->query_ctx->groupby for grouping keys
        obj_p agg_result = agg_func(fval, NULL_OBJ);

        if (IS_ERR(agg_result)) {
            query_ctx_destroy(&pctx);
            drop_obj(fkeys);
            drop_obj(fval);
            drop_obj(pivot_val);
            drop_obj(result);
            result = agg_result;
            goto cleanup;
        }

        // Get unique key values per group (same ordering as agg_result)
        obj_p group_key_vals;
        if (single_index) {
            group_key_vals = aggr_first(AS_LIST(fkeys)[0], NULL_OBJ);
            if (IS_ERR(group_key_vals)) {
                drop_obj(agg_result);
                query_ctx_destroy(&pctx);
                drop_obj(fkeys);
                drop_obj(fval);
                drop_obj(pivot_val);
                drop_obj(result);
                result = group_key_vals;
                goto cleanup;
            }
        } else {
            group_key_vals = LIST(nkeys);
            for (i64_t k = 0; k < nkeys; k++) {
                obj_p gk = aggr_first(AS_LIST(fkeys)[k], NULL_OBJ);
                if (IS_ERR(gk)) {
                    group_key_vals->len = k;
                    drop_obj(group_key_vals);
                    drop_obj(agg_result);
                    query_ctx_destroy(&pctx);
                    drop_obj(fkeys);
                    drop_obj(fval);
                    drop_obj(pivot_val);
                    drop_obj(result);
                    result = gk;
                    goto cleanup;
                }
                AS_LIST(group_key_vals)[k] = gk;
            }
        }

        query_ctx_destroy(&pctx);
        drop_obj(fkeys);
        drop_obj(fval);

        i64_t pivot_name = pivot_val_to_symbol(pivot_val, i);
        drop_obj(pivot_val);

        // Build mini-table: [key_cols..., agg_result]
        obj_p tbl_keys, tbl_vals;
        if (single_index) {
            tbl_keys = SYMBOL(2);
            AS_SYMBOL(tbl_keys)[0] = index->i64;
            AS_SYMBOL(tbl_keys)[1] = pivot_name;
            tbl_vals = vn_list(2, group_key_vals, agg_result);
        } else {
            tbl_keys = SYMBOL(nkeys + 1);
            for (i64_t k = 0; k < nkeys; k++)
                AS_SYMBOL(tbl_keys)[k] = AS_SYMBOL(index)[k];
            AS_SYMBOL(tbl_keys)[nkeys] = pivot_name;

            tbl_vals = LIST(nkeys + 1);
            for (i64_t k = 0; k < nkeys; k++)
                AS_LIST(tbl_vals)[k] = clone_obj(AS_LIST(group_key_vals)[k]);
            AS_LIST(tbl_vals)[nkeys] = clone_obj(agg_result);
            drop_obj(group_key_vals);
            drop_obj(agg_result);
        }

        obj_p pivot_table = table(tbl_keys, tbl_vals);

        if (IS_ERR(pivot_table)) {
            drop_obj(result);
            result = pivot_table;
            goto cleanup;
        }

        obj_p join_syms = clone_obj(index_syms);
        obj_p join_args[3] = {join_syms, result, pivot_table};
        obj_p joined = ray_left_join(join_args, 3);
        drop_obj(join_syms);
        drop_obj(result);
        drop_obj(pivot_table);

        if (IS_ERR(joined)) {
            result = joined;
            goto cleanup;
        }

        result = joined;
    }

cleanup:
    drop_obj(unique_vals);
    drop_obj(pivot_col);
    drop_obj(val_col);
    drop_obj(key_cols);
    drop_obj(index_syms);

    return result;
}
