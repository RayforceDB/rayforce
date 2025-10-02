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

#include "join.h"
#include "rayforce.h"
#include "items.h"
#include "ops.h"
#include "compose.h"
#include "error.h"
#include "index.h"
#include "group.h"
#include "eval.h"
#include "filter.h"
#include "aggr.h"

obj_p select_column(obj_p left_col, obj_p right_col, i64_t ids[], i64_t len) {
    i64_t i;
    obj_p v, res;
    i64_t idx;
    i8_t type;

    // there is no such column in the right table
    if (is_null(right_col))
        return clone_obj(left_col);

    type = is_null(left_col) ? right_col->type : left_col->type;

    if (right_col->type != type)
        THROW(ERR_TYPE, "select_column: incompatible types");

    res = vector(type, len);

    for (i = 0; i < len; i++) {
        idx = ids[i];
        if (idx != NULL_I64)
            v = at_idx(right_col, idx);
        else
            v = at_idx(left_col, i);

        ins_obj(&res, i, v);
    }

    return res;
}

obj_p get_column(obj_p left_col, obj_p right_col, obj_p lids, obj_p rids) {
    i8_t type;

    // there is no such column in the right table
    if (is_null(right_col))
        return at_ids(left_col, AS_I64(lids), lids->len);

    type = is_null(left_col) ? right_col->type : left_col->type;

    if (right_col->type != type)
        THROW(ERR_TYPE, "get_column: incompatible types");

    return at_ids(right_col, AS_I64(rids), rids->len);
}

static obj_p __left_join_inner(obj_p ltab, obj_p rtab, obj_p ksyms, obj_p kcols, obj_p idx) {
    obj_p un, cols, vals, rescols, resvals;
    i64_t i, j, l, len;
    obj_p c1, c2, col;

    un = ray_union(AS_LIST(ltab)[0], AS_LIST(rtab)[0]);
    if (IS_ERR(un))
        return un;

    // exclude columns that we are joining on
    cols = ray_except(un, ksyms);
    drop_obj(un);

    if (IS_ERR(cols))
        return cols;

    l = cols->len;

    if (l == 0) {
        drop_obj(cols);
        THROW(ERR_LENGTH, "left-join: no columns to join on");
    }

    // resulting columns
    vals = LIST(l);
    len = ops_count(ltab);

    // process each column
    for (i = 0; i < l; i++) {
        c1 = NULL_OBJ;
        c2 = NULL_OBJ;

        for (j = 0; j < (i64_t)AS_LIST(ltab)[0]->len; j++) {
            if (AS_SYMBOL(AS_LIST(ltab)[0])[j] == AS_SYMBOL(cols)[i]) {
                c1 = AS_LIST(AS_LIST(ltab)[1])[j];
                break;
            }
        }

        for (j = 0; j < (i64_t)AS_LIST(rtab)[0]->len; j++) {
            if (AS_SYMBOL(AS_LIST(rtab)[0])[j] == AS_SYMBOL(cols)[i]) {
                c2 = AS_LIST(AS_LIST(rtab)[1])[j];
                break;
            }
        }

        col = select_column(c1, c2, AS_I64(idx), len);
        if (IS_ERR(col)) {
            drop_obj(cols);
            drop_obj(vals);
            return col;
        }
        AS_LIST(vals)[i] = col;
    }

    // cleanup and assemble result table
    rescols = ray_concat(ksyms, cols);
    drop_obj(cols);

    // handle case when columns list is just one-element list
    if (ksyms->len == 1) {
        l = rescols->len;
        resvals = vector(TYPE_LIST, l);
        AS_LIST(resvals)[0] = kcols;
        for (i = 1; i < l; i++)
            AS_LIST(resvals)[i] = clone_obj(AS_LIST(vals)[i - 1]);
        drop_obj(vals);
    } else {
        resvals = ray_concat(kcols, vals);
        drop_obj(vals);
    }

    return table(rescols, resvals);
}

obj_p ray_left_join(obj_p *x, i64_t n) {
    obj_p k1, k2, idx, res;

    if (n != 3)
        THROW(ERR_LENGTH, "left-join");

    if (x[0]->type != TYPE_SYMBOL)
        THROW(ERR_TYPE, "left-join: first argument must be a symbol vector");

    if (x[1]->type != TYPE_TABLE)
        THROW(ERR_TYPE, "left-join: second argument must be a table");

    if (x[2]->type != TYPE_TABLE)
        THROW(ERR_TYPE, "left-join: third argument must be a table");

    if (ops_count(x[1]) == 0 || ops_count(x[2]) == 0)
        return clone_obj(x[1]);

    k1 = ray_at(x[1], x[0]);
    if (IS_ERR(k1))
        return k1;

    k2 = ray_at(x[2], x[0]);
    if (IS_ERR(k2)) {
        drop_obj(k1);
        return k2;
    }

    idx = index_left_join_obj(k1, k2, x[0]->len);
    drop_obj(k2);

    if (IS_ERR(idx)) {
        drop_obj(k1);
        return idx;
    }

    res = __left_join_inner(x[1], x[2], x[0], k1, idx);
    drop_obj(idx);
    drop_obj(k1);
    return res;
}

obj_p ray_inner_join(obj_p *x, i64_t n) {
    i64_t i, j, l;
    obj_p k1, k2, c1, c2, un, col, cols, vals, idx;

    if (n != 3)
        THROW(ERR_LENGTH, "inner-join");

    if (x[0]->type != TYPE_SYMBOL)
        THROW(ERR_TYPE, "inner-join: first argument must be a symbol vector");

    if (x[1]->type != TYPE_TABLE)
        THROW(ERR_TYPE, "inner-join: second argument must be a table");

    if (x[2]->type != TYPE_TABLE)
        THROW(ERR_TYPE, "inner-join: third argument must be a table");

    if (ops_count(x[1]) == 0 || ops_count(x[2]) == 0)
        return clone_obj(x[1]);

    k1 = ray_at(x[1], x[0]);
    if (IS_ERR(k1))
        return k1;

    k2 = ray_at(x[2], x[0]);
    if (IS_ERR(k2)) {
        drop_obj(k1);
        return k2;
    }

    idx = index_inner_join_obj(k1, k2, x[0]->len);
    drop_obj(k1);
    drop_obj(k2);

    if (IS_ERR(idx))
        return idx;

    un = ray_union(AS_LIST(x[1])[0], AS_LIST(x[2])[0]);
    if (IS_ERR(un))
        return un;

    // exclude columns that we are joining on
    cols = ray_except(un, x[0]);
    drop_obj(un);

    if (IS_ERR(cols)) {
        drop_obj(idx);
        return cols;
    }

    if (cols->len == 0) {
        drop_obj(idx);
        drop_obj(cols);
        THROW(ERR_LENGTH, "inner-join: no columns to join on");
    }

    col = ray_concat(x[0], cols);
    drop_obj(cols);
    cols = col;

    l = cols->len;

    // resulting columns
    vals = LIST(l);

    // process each column
    for (i = 0; i < l; i++) {
        c1 = NULL_OBJ;
        c2 = NULL_OBJ;

        for (j = 0; j < (i64_t)AS_LIST(x[1])[0]->len; j++) {
            if (AS_SYMBOL(AS_LIST(x[1])[0])[j] == AS_SYMBOL(cols)[i]) {
                c1 = AS_LIST(AS_LIST(x[1])[1])[j];
                break;
            }
        }

        for (j = 0; j < (i64_t)AS_LIST(x[2])[0]->len; j++) {
            if (AS_SYMBOL(AS_LIST(x[2])[0])[j] == AS_SYMBOL(cols)[i]) {
                c2 = AS_LIST(AS_LIST(x[2])[1])[j];
                break;
            }
        }

        col = get_column(c1, c2, AS_LIST(idx)[0], AS_LIST(idx)[1]);
        if (IS_ERR(col)) {
            drop_obj(cols);
            drop_obj(idx);
            drop_obj(vals);
            return col;
        }

        AS_LIST(vals)[i] = col;
    }

    // cleanup and assemble result table
    drop_obj(idx);

    return table(cols, vals);
}

obj_p ray_asof_join(obj_p *x, i64_t n) {
    obj_p idx, v, ajkl, ajkr, keys, lvals, rvals, res;

    if (n != 3)
        THROW(ERR_ARITY, "asof-join");

    if (x[0]->type != TYPE_SYMBOL)
        THROW(ERR_TYPE, "asof-join: first argument must be a symbol vector");

    if (x[1]->type != TYPE_TABLE)
        THROW(ERR_TYPE, "asof-join: second argument must be a table");

    if (x[2]->type != TYPE_TABLE)
        THROW(ERR_TYPE, "asof-join: third argument must be a table");

    v = ray_last(x[0]);
    ajkl = ray_at(x[1], v);
    ajkr = ray_at(x[2], v);
    drop_obj(v);

    if (is_null(ajkl) || is_null(ajkr)) {
        drop_obj(ajkl);
        drop_obj(ajkr);
        THROW(ERR_INDEX, "asof-join: key not found");
    }

    if (ajkl->type != ajkr->type) {
        drop_obj(ajkl);
        drop_obj(ajkr);
        THROW(ERR_TYPE, "asof-join: incompatible types");
    }

    keys = cow_obj(x[0]);
    keys = remove_idx(&keys, keys->len - 1);
    lvals = at_obj(x[1], keys);
    rvals = at_obj(x[2], keys);

    idx = index_asof_join_obj(lvals, ajkl, rvals, ajkr);

    drop_obj(keys);
    drop_obj(lvals);
    drop_obj(rvals);
    drop_obj(ajkl);
    drop_obj(ajkr);

    keys = at_obj(x[1], x[0]);

    res = __left_join_inner(x[1], x[2], x[0], keys, idx);
    drop_obj(idx);
    drop_obj(keys);
    return res;
}

static obj_p __window_join(obj_p *x, i64_t n, i64_t tp) {
    i64_t i, l;
    obj_p k, v, wjkl, wjkr, keys, lvals, rvals, idx;
    obj_p agrvals, resyms, recols, rtab;

    if (n != 5)
        THROW(ERR_ARITY, "window-join");

    if (x[0]->type != TYPE_SYMBOL)
        THROW(ERR_TYPE, "window-join: first argument must be a symbol vector");

    if (x[1]->type != TYPE_LIST)
        THROW(ERR_TYPE, "window-join: second argument must be a windows list");

    if (x[2]->type != TYPE_TABLE)
        THROW(ERR_TYPE, "window-join: third argument must be a table");

    if (x[3]->type != TYPE_TABLE)
        THROW(ERR_TYPE, "window-join: fourth argument must be a table");

    if (x[4]->type != TYPE_DICT)
        THROW(ERR_TYPE, "window-join: fifth argument must be a dict");

    v = ray_last(x[0]);
    wjkl = ray_at(x[2], v);
    wjkr = ray_at(x[3], v);
    drop_obj(v);

    if (is_null(wjkl) || is_null(wjkr)) {
        drop_obj(wjkl);
        drop_obj(wjkr);
        THROW(ERR_INDEX, "window-join: key not found");
    }

    if (wjkl->type != wjkr->type) {
        drop_obj(wjkl);
        drop_obj(wjkr);
        THROW(ERR_TYPE, "window-join: incompatible types");
    }

    keys = cow_obj(x[0]);
    keys = remove_idx(&keys, keys->len - 1);
    lvals = at_obj(x[2], keys);
    rvals = at_obj(x[3], keys);

    idx = index_window_join_obj(lvals, wjkl, rvals, wjkr, x[1], x[2], x[3], tp);

    drop_obj(keys);
    drop_obj(lvals);
    drop_obj(rvals);
    drop_obj(wjkl);
    drop_obj(wjkr);

    rtab = group_map(x[3], idx);
    mount_env(rtab);

    l = ops_count(x[4]);
    agrvals = LIST(l);

    for (i = 0; i < l; i++) {
        v = eval(AS_LIST(AS_LIST(x[4])[1])[i]);
        if (IS_ERR(v)) {
            unmount_env(AS_LIST(x[3])[0]->len);
            agrvals->len = i;
            drop_obj(agrvals);
            drop_obj(rtab);
            return v;
        }

        // Materialize fields
        switch (v->type) {
            case TYPE_MAPFILTER:
                k = filter_collect(AS_LIST(v)[0], AS_LIST(v)[1]);
                drop_obj(v);
                v = k;
                break;
            case TYPE_MAPGROUP:
                k = aggr_collect(AS_LIST(v)[0], AS_LIST(v)[1]);
                drop_obj(v);
                v = k;
                break;
            default:
                k = ray_value(v);
                drop_obj(v);
                v = k;
                break;
        }

        if (IS_ERR(v)) {
            agrvals->len = i;
            drop_obj(agrvals);
            drop_obj(rtab);
            return v;
        }

        AS_LIST(agrvals)[i] = v;
    }

    unmount_env(AS_LIST(x[3])[0]->len);
    drop_obj(rtab);

    resyms = ray_concat(AS_LIST(x[2])[0], AS_LIST(x[4])[0]);
    recols = ray_concat(AS_LIST(x[2])[1], agrvals);

    drop_obj(agrvals);
    drop_obj(idx);

    return table(resyms, recols);
}

obj_p ray_window_join(obj_p *x, i64_t n) { return __window_join(x, n, 0); }

obj_p ray_window_join1(obj_p *x, i64_t n) { return __window_join(x, n, 1); }