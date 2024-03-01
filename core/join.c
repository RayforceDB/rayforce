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
#include "heap.h"
#include "util.h"
#include "items.h"
#include "rel.h"
#include "ops.h"
#include "binary.h"
#include "compose.h"
#include "error.h"
#include "index.h"

obj_p select_column(obj_p left_col, obj_p right_col, i64_t ids[], u64_t len)
{
    u64_t i;
    obj_p v, res;
    i64_t idx;
    i8_t type;

    // there is no such column in the right table
    if (is_null(right_col))
        return clone_obj(left_col);

    type = is_null(left_col) ? right_col->type : left_col->type;

    if (right_col->type != type)
        throw(ERR_TYPE, "join_column: incompatible types");

    res = vector(type, len);

    for (i = 0; i < len; i++)
    {
        idx = ids[i];
        if (idx != NULL_I64)
            v = at_idx(right_col, idx);
        else
            v = at_idx(left_col, i);

        ins_obj(&res, i, v);
    }

    return res;
}

obj_p ray_lj(obj_p *x, u64_t n)
{
    u64_t ll;
    i64_t i, j, l;
    obj_p k1, k2, c1, c2, un, col, cols, vals, idx, rescols, resvals;

    if (n != 3)
        throw(ERR_LENGTH, "lj");

    if (x[0]->type != TYPE_SYMBOL)
        throw(ERR_TYPE, "lj: first argument must be a symbol vector");

    if (x[1]->type != TYPE_TABLE)
        throw(ERR_TYPE, "lj: second argument must be a table");

    if (x[2]->type != TYPE_TABLE)
        throw(ERR_TYPE, "lj: third argument must be a table");

    if (ops_count(x[1]) == 0 || ops_count(x[2]) == 0)
        return clone_obj(x[1]);

    ll = ops_count(x[1]);

    k1 = ray_at(x[1], x[0]);
    if (is_error(k1))
        return k1;

    k2 = ray_at(x[2], x[0]);
    if (is_error(k2))
    {
        drop_obj(k1);
        return k2;
    }

    idx = index_join_obj(k1, k2, x[0]->len);
    drop_obj(k2);

    if (is_error(idx))
    {
        drop_obj(k1);
        return idx;
    }

    un = ray_union(as_list(x[1])[0], as_list(x[2])[0]);
    if (is_error(un))
    {
        drop_obj(k1);
        return un;
    }

    // exclude columns that we are joining on
    cols = ray_except(un, x[0]);
    drop_obj(un);

    if (is_error(cols))
    {
        drop_obj(k1);
        drop_obj(idx);
        return cols;
    }

    l = cols->len;

    if (l == 0)
    {
        drop_obj(k1);
        drop_obj(idx);
        drop_obj(cols);
        throw(ERR_LENGTH, "lj: no columns to join on");
    }

    // resulting columns
    vals = list(l);

    // process each column
    for (i = 0; i < l; i++)
    {
        c1 = NULL_OBJ;
        c2 = NULL_OBJ;

        for (j = 0; j < (i64_t)as_list(x[1])[0]->len; j++)
        {
            if (as_symbol(as_list(x[1])[0])[j] == as_symbol(cols)[i])
            {
                c1 = as_list(as_list(x[1])[1])[j];
                break;
            }
        }

        for (j = 0; j < (i64_t)as_list(x[2])[0]->len; j++)
        {
            if (as_symbol(as_list(x[2])[0])[j] == as_symbol(cols)[i])
            {
                c2 = as_list(as_list(x[2])[1])[j];
                break;
            }
        }

        col = select_column(c1, c2, as_i64(idx), ll);
        if (is_error(col))
        {
            drop_obj(k1);
            drop_obj(cols);
            drop_obj(idx);
            drop_obj(vals);
            return col;
        }

        as_list(vals)[i] = col;
    }

    // cleanup and assemble result table
    drop_obj(idx);
    rescols = ray_concat(x[0], cols);
    drop_obj(cols);

    // handle case when columns list is just one-element list
    if (x[0]->len == 1)
    {
        l = rescols->len;
        resvals = vector(TYPE_LIST, l);
        as_list(resvals)[0] = k1;
        for (i = 1; i < l; i++)
            as_list(resvals)[i] = clone_obj(as_list(vals)[i - 1]);
        drop_obj(vals);
    }
    else
    {
        resvals = ray_concat(k1, vals);
        drop_obj(k1);
        drop_obj(vals);
    }

    return table(rescols, resvals);
}
