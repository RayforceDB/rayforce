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
#include "index.h"
#include "symbols.h"
#include "ops.h"
#include "cmp.h"
#include "vary.h"
#include "unary.h"
#include "math.h"
#include "items.h"
#include "misc.h"

typedef obj_p (*aggr_fn)(obj_p val, obj_p index);

static b8_t is_function(obj_p obj) {
    return obj->type == TYPE_UNARY || obj->type == TYPE_BINARY || obj->type == TYPE_VARY || obj->type == TYPE_LAMBDA;
}

static aggr_fn get_optimized_aggr(obj_p func) {
    if (func->type != TYPE_UNARY)
        return NULL;

    unary_f fn = (unary_f)func->i64;
    if (fn == ray_sum)
        return aggr_sum;
    if (fn == ray_avg)
        return aggr_avg;
    if (fn == ray_min)
        return aggr_min;
    if (fn == ray_max)
        return aggr_max;
    if (fn == ray_med)
        return aggr_med;
    if (fn == ray_count)
        return aggr_count;
    if (fn == ray_first)
        return aggr_first;
    if (fn == ray_last)
        return aggr_last;

    return NULL;
}

static obj_p apply_aggr_func_slow(obj_p func, obj_p val, obj_p group_idx) {
    i64_t i, n;
    obj_p collected, res, v, args[2];

    collected = aggr_collect(val, group_idx);
    if (IS_ERR(collected))
        return collected;

    n = collected->len;
    res = LIST(n);

    for (i = 0; i < n; i++) {
        args[0] = func;
        args[1] = AS_LIST(collected)[i];
        v = ray_apply(args, 2);

        if (IS_ERR(v)) {
            res->len = i;
            drop_obj(res);
            drop_obj(collected);
            return v;
        }

        AS_LIST(res)[i] = v;
    }

    drop_obj(collected);

    obj_p unified = ray_unify(res);
    drop_obj(res);

    return unified;
}

// fast path for built-ins, slow path for custom functions
static obj_p apply_aggr_func(obj_p func, obj_p val, obj_p group_idx) {
    aggr_fn optimized = get_optimized_aggr(func);
    if (optimized != NULL)
        return optimized(val, group_idx);

    return apply_aggr_func_slow(func, val, group_idx);
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

    if (!is_function(aggfunc))
        return err_type(TYPE_LAMBDA, aggfunc->type, 5, 0);

    obj_p pivot_col = ray_at(tab, columns);
    if (IS_ERR(pivot_col))
        return pivot_col;

    obj_p unique_vals = ray_distinct(pivot_col);
    drop_obj(pivot_col);

    if (IS_ERR(unique_vals))
        return unique_vals;

    i64_t num_pivots = unique_vals->len;
    if (num_pivots == 0) {
        drop_obj(unique_vals);
        return err_domain(3, 0);
    }

    obj_p index_syms;
    b8_t single_index = (index->type == -TYPE_SYMBOL);
    if (single_index) {
        index_syms = vector(TYPE_SYMBOL, 1);
        AS_SYMBOL(index_syms)[0] = index->i64;
    } else {
        index_syms = clone_obj(index);
    }

    // Get value column and index data
    obj_p val_col = ray_at(tab, values);
    if (IS_ERR(val_col)) {
        drop_obj(unique_vals);
        drop_obj(index_syms);
        return val_col;
    }

    obj_p index_data = ray_at(tab, index_syms);
    if (IS_ERR(index_data)) {
        drop_obj(unique_vals);
        drop_obj(index_syms);
        drop_obj(val_col);
        return index_data;
    }

    pivot_col = ray_at(tab, columns);
    if (IS_ERR(pivot_col)) {
        drop_obj(unique_vals);
        drop_obj(index_syms);
        drop_obj(val_col);
        drop_obj(index_data);
        return pivot_col;
    }

    obj_p unique_index;
    if (single_index) {
        unique_index = ray_distinct(index_data);
    } else {
        obj_p group_idx = index_group_list(index_data, NULL_OBJ);
        if (IS_ERR(group_idx)) {
            drop_obj(unique_vals);
            drop_obj(index_syms);
            drop_obj(val_col);
            drop_obj(index_data);
            drop_obj(pivot_col);
            return group_idx;
        }
        i64_t ncols = index_data->len;
        unique_index = LIST(ncols);
        for (i64_t j = 0; j < ncols; j++) {
            obj_p col = aggr_first(AS_LIST(index_data)[j], group_idx);
            if (IS_ERR(col)) {
                unique_index->len = j;
                drop_obj(unique_index);
                drop_obj(group_idx);
                drop_obj(unique_vals);
                drop_obj(index_syms);
                drop_obj(val_col);
                drop_obj(index_data);
                drop_obj(pivot_col);
                return col;
            }
            AS_LIST(unique_index)[j] = col;
        }
        drop_obj(group_idx);
    }

    if (IS_ERR(unique_index)) {
        drop_obj(unique_vals);
        drop_obj(index_syms);
        drop_obj(val_col);
        drop_obj(index_data);
        drop_obj(pivot_col);
        return unique_index;
    }

    obj_p base_keys = clone_obj(index_syms);
    obj_p base_vals;
    if (single_index) {
        base_vals = vn_list(1, clone_obj(unique_index));
    } else {
        i64_t ncols = unique_index->len;
        base_vals = LIST(ncols);
        for (i64_t j = 0; j < ncols; j++) {
            AS_LIST(base_vals)[j] = clone_obj(AS_LIST(unique_index)[j]);
        }
    }
    obj_p result = table(base_keys, base_vals);
    drop_obj(unique_index);

    if (IS_ERR(result)) {
        drop_obj(unique_vals);
        drop_obj(index_syms);
        drop_obj(val_col);
        drop_obj(index_data);
        drop_obj(pivot_col);
        return result;
    }

    for (i64_t i = 0; i < num_pivots; i++) {
        obj_p pivot_val = at_idx(unique_vals, i);

        // filter mask: pivot_col == pivot_val
        obj_p mask = ray_eq(pivot_col, pivot_val);
        if (IS_ERR(mask)) {
            drop_obj(pivot_val);
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

        obj_p filtered_index;
        if (single_index) {
            filtered_index = at_ids(index_data, AS_I64(filter_idx), filter_idx->len);
        } else {
            i64_t ncols = index_data->len;
            filtered_index = LIST(ncols);
            for (i64_t j = 0; j < ncols; j++) {
                obj_p col = at_ids(AS_LIST(index_data)[j], AS_I64(filter_idx), filter_idx->len);
                if (IS_ERR(col)) {
                    filtered_index->len = j;
                    drop_obj(filtered_index);
                    drop_obj(filter_idx);
                    drop_obj(pivot_val);
                    drop_obj(result);
                    result = col;
                    goto cleanup;
                }
                AS_LIST(filtered_index)[j] = col;
            }
        }

        if (IS_ERR(filtered_index)) {
            drop_obj(filter_idx);
            drop_obj(pivot_val);
            drop_obj(result);
            result = filtered_index;
            goto cleanup;
        }

        obj_p filtered_val = at_ids(val_col, AS_I64(filter_idx), filter_idx->len);
        drop_obj(filter_idx);

        if (IS_ERR(filtered_val)) {
            drop_obj(filtered_index);
            drop_obj(pivot_val);
            drop_obj(result);
            result = filtered_val;
            goto cleanup;
        }

        obj_p group_idx =
            single_index ? index_group(filtered_index, NULL_OBJ) : index_group_list(filtered_index, NULL_OBJ);

        if (IS_ERR(group_idx)) {
            drop_obj(filtered_index);
            drop_obj(filtered_val);
            drop_obj(pivot_val);
            drop_obj(result);
            result = group_idx;
            goto cleanup;
        }

        obj_p agg_result = apply_aggr_func(aggfunc, filtered_val, group_idx);
        drop_obj(filtered_val);

        if (IS_ERR(agg_result)) {
            drop_obj(group_idx);
            drop_obj(filtered_index);
            drop_obj(pivot_val);
            drop_obj(result);
            result = agg_result;
            goto cleanup;
        }

        obj_p group_keys;
        if (single_index) {
            group_keys = aggr_first(filtered_index, group_idx);
        } else {
            i64_t ncols = filtered_index->len;
            group_keys = LIST(ncols);
            for (i64_t j = 0; j < ncols; j++) {
                obj_p col = aggr_first(AS_LIST(filtered_index)[j], group_idx);
                if (IS_ERR(col)) {
                    group_keys->len = j;
                    drop_obj(group_keys);
                    drop_obj(group_idx);
                    drop_obj(agg_result);
                    drop_obj(filtered_index);
                    drop_obj(pivot_val);
                    drop_obj(result);
                    result = col;
                    goto cleanup;
                }
                AS_LIST(group_keys)[j] = col;
            }
        }

        drop_obj(group_idx);
        drop_obj(filtered_index);

        if (IS_ERR(group_keys)) {
            drop_obj(agg_result);
            drop_obj(pivot_val);
            drop_obj(result);
            result = group_keys;
            goto cleanup;
        }

        i64_t pivot_name = pivot_val_to_symbol(pivot_val, i);
        drop_obj(pivot_val);

        obj_p tbl_keys, tbl_vals;
        if (single_index) {
            tbl_keys = SYMBOL(2);
            AS_SYMBOL(tbl_keys)[0] = index->i64;
            AS_SYMBOL(tbl_keys)[1] = pivot_name;
            tbl_vals = vn_list(2, group_keys, agg_result);
        } else {
            i64_t ncols = index_syms->len;
            tbl_keys = SYMBOL(ncols + 1);
            for (i64_t j = 0; j < ncols; j++)
                AS_SYMBOL(tbl_keys)[j] = AS_SYMBOL(index_syms)[j];
            AS_SYMBOL(tbl_keys)[ncols] = pivot_name;

            tbl_vals = LIST(ncols + 1);
            for (i64_t j = 0; j < ncols; j++)
                AS_LIST(tbl_vals)[j] = clone_obj(AS_LIST(group_keys)[j]);
            AS_LIST(tbl_vals)[ncols] = clone_obj(agg_result);
            drop_obj(group_keys);
            drop_obj(agg_result);
        }

        obj_p pivot_table = table(tbl_keys, tbl_vals);

        if (IS_ERR(pivot_table)) {
            drop_obj(tbl_keys);
            drop_obj(tbl_vals);
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
    drop_obj(index_syms);
    drop_obj(val_col);
    drop_obj(index_data);
    drop_obj(pivot_col);

    return result;
}
