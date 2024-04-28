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
#include "util.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "eval.h"
#include "items.h"
#include "compose.h"
#include "error.h"
#include "math.h"
#include "aggr.h"
#include "iter.h"
#include "index.h"
#include "group.h"
#include "filter.h"
#include "update.h"
#include "pool.h"
#include "runtime.h"

obj_p get_fields(obj_p obj)
{
    obj_p keywords, symbols;

    keywords = vn_symbol(4, "take", "by", "from", "where");
    symbols = ray_except(as_list(obj)[0], keywords);
    drop_obj(keywords);

    return symbols;
}

obj_p remap_filter(obj_p x, obj_p y)
{
    u64_t i, l;
    obj_p res;

    l = as_list(x)[1]->len;
    res = list(l);
    for (i = 0; i < l; i++)
        as_list(res)[i] = filter_map(clone_obj(as_list(as_list(x)[1])[i]), clone_obj(y));

    return table(clone_obj(as_list(x)[0]), res);
}

obj_p remap_group(obj_p *gvals, obj_p cols, obj_p tab, obj_p filter, obj_p gkeys, obj_p gcols)
{
    u64_t i, l;
    obj_p bins, v, lst, res;

    switch (gkeys->type)
    {
    case -TYPE_SYMBOL:
        bins = group_bins(cols, tab, filter);

        if (is_error(bins))
            return bins;

        res = group_map(tab, bins, filter);
        v = (gcols == NULL_OBJ) ? aggr_first(cols, bins, filter) : aggr_first(gcols, bins, filter);
        if (is_error(v))
        {
            drop_obj(res);
            return v;
        }

        *gvals = v;
        drop_obj(bins);

        return res;
    case TYPE_SYMBOL:
        bins = group_bins_list(cols, tab, filter);

        if (is_error(bins))
            return bins;

        res = group_map(tab, bins, filter);

        l = cols->len;
        lst = list(l);

        for (i = 0; i < l; i++)
        {
            v = aggr_first(as_list(cols)[i], bins, filter);

            if (is_error(v))
            {
                lst->len = i;
                drop_obj(res);
                drop_obj(bins);
                return v;
            }

            as_list(lst)[i] = v;
        }

        *gvals = lst;
        drop_obj(bins);

        return res;
    default:
        return error(ERR_TYPE, "grouping key mapping(s) must be a symbol(s)");
    }
}

obj_p get_gkeys(obj_p cols, obj_p obj)
{
    u64_t i, l;
    obj_p x;

    switch (obj->type)
    {
    case -TYPE_SYMBOL:
        l = cols->len;
        for (i = 0; i < l; i++)
            if (as_i64(cols)[i] == obj->i64)
                return symboli64(obj->i64);
        return NULL_OBJ;
    case TYPE_LIST:
        l = obj->len;
        for (i = 0; i < l; i++)
        {
            x = get_gkeys(cols, as_list(obj)[i]);
            if (x != NULL_OBJ)
                return x;
        }
        return NULL_OBJ;
    case TYPE_DICT:
        x = as_list(obj)[0];
        if (x->type != TYPE_SYMBOL)
            return error(ERR_TYPE, "grouping key(s) must be a symbol(s)");

        if (x->len == 1)
            return at_idx(as_list(obj)[0], 0);

        return clone_obj(as_list(obj)[0]);

    default:
        return NULL_OBJ;
    }
}

obj_p get_gvals(obj_p obj)
{
    u64_t i, l;
    obj_p vals, v, r, res;

    switch (obj->type)
    {
    case TYPE_DICT:
        vals = as_list(obj)[1];
        l = vals->len;

        if (l == 0)
            return NULL_OBJ;

        if (l == 1)
        {
            v = at_idx(vals, 0);
            res = eval(v);
            drop_obj(v);
            return res;
        }

        res = list(l);
        for (i = 0; i < l; i++)
        {
            v = at_idx(vals, i);
            r = eval(v);
            drop_obj(v);

            if (is_error(r))
            {
                res->len = i;
                drop_obj(res);
                return r;
            }

            as_list(res)[i] = r;
        }

        return res;
    default:
        return eval(obj);
    }
}

obj_p eval_field(raw_p x, u64_t n)
{
    unused(n);
    obj_p val, res;

    val = (obj_p)x;
    res = eval(val);

    // Materialize fields
    if (res->type == TYPE_GROUPMAP)
    {
        val = group_collect(res);
        drop_obj(res);
        res = val;
    }
    else if (res->type == TYPE_FILTERMAP)
    {
        val = filter_collect(res);
        drop_obj(res);
        res = val;
    }
    else if (res->type == TYPE_ENUM)
    {
        val = ray_value(res);
        drop_obj(res);
        res = val;
    }

    return res;
}

obj_p ray_select(obj_p obj)
{
    u64_t i, l, tablen;
    obj_p keys = NULL_OBJ, vals = NULL_OBJ, filters = NULL_OBJ, groupby = NULL_OBJ,
          gcol = NULL_OBJ, gkeys = NULL_OBJ, gvals = NULL_OBJ, tab, sym, prm, val;
    pool_p pool;

    if (obj->type != TYPE_DICT)
        throw(ERR_LENGTH, "'select' takes dict of params");

    if (as_list(obj)[0]->type != TYPE_SYMBOL)
        throw(ERR_LENGTH, "'select' takes dict with symbol keys");

    // Retrive a table
    prm = at_sym(obj, "from");

    if (is_null(prm))
        throw(ERR_LENGTH, "'select' expects 'from' param");

    tab = eval(prm);
    drop_obj(prm);

    if (is_error(tab))
        return tab;

    if (tab->type != TYPE_TABLE)
    {
        drop_obj(tab);
        throw(ERR_TYPE, "'select' from: expects table");
    }

    // Mount table columns to a local env
    tablen = as_list(tab)[0]->len;
    mount_env(tab);

    // Apply filters
    prm = at_sym(obj, "where");
    if (prm != NULL_OBJ)
    {
        val = eval(prm);
        drop_obj(prm);
        if (is_error(val))
        {
            drop_obj(tab);
            return val;
        }

        filters = ray_where(val);
        drop_obj(val);
        if (is_error(filters))
        {
            drop_obj(tab);
            return filters;
        }
    }

    // Apply groupping
    prm = at_sym(obj, "by");
    if (prm != NULL_OBJ)
    {
        gkeys = get_gkeys(as_list(tab)[0], prm);
        groupby = get_gvals(prm);

        if (gkeys == NULL_OBJ)
            gkeys = symbol("By");
        else if (prm->type != TYPE_DICT)
            gvals = eval(gkeys);

        drop_obj(prm);

        unmount_env(tablen);

        if (is_error(groupby))
        {
            drop_obj(gkeys);
            drop_obj(gvals);
            drop_obj(filters);
            drop_obj(tab);
            return groupby;
        }

        prm = remap_group(&gcol, groupby, tab, filters, gkeys, gvals);
        drop_obj(gvals);

        if (is_error(prm))
        {
            drop_obj(filters);
            drop_obj(groupby);
            drop_obj(gkeys);
            drop_obj(gcol);
            drop_obj(tab);
            return prm;
        }

        mount_env(prm);

        drop_obj(prm);
        drop_obj(filters);
        drop_obj(groupby);

        if (is_error(gcol))
        {
            drop_obj(tab);
            return gcol;
        }
    }
    else if (filters != NULL_OBJ)
    {
        // Unmount table columns from a local env
        unmount_env(tablen);
        // Create filtermaps over table
        val = remap_filter(tab, filters);
        drop_obj(filters);
        mount_env(val);
        drop_obj(val);
    }

    // Find all mappings (non-keyword fields)
    keys = get_fields(obj);
    l = keys->len;

    // Apply mappings
    if (l)
    {
        pool = runtime_get()->pool;

        // single-threaded
        if (!pool)
        {
            vals = list(l);

            for (i = 0; i < l; i++)
            {
                sym = at_idx(keys, i);
                prm = at_obj(obj, sym);
                drop_obj(sym);

                val = eval_field(prm, 1);
                drop_obj(prm);

                if (is_error(val))
                {
                    vals->len = i;
                    drop_obj(vals);
                    drop_obj(tab);
                    drop_obj(keys);
                    drop_obj(gkeys);
                    drop_obj(gcol);
                    return val;
                }

                if (is_error(val))
                {
                    vals->len = i;
                    drop_obj(vals);
                    drop_obj(tab);
                    drop_obj(keys);
                    drop_obj(gkeys);
                    drop_obj(gcol);
                    return val;
                }

                as_list(vals)[i] = val;
            }
        }
        else
        {
            pool_prepare(pool, l);

            for (i = 0; i < l; i++)
            {
                sym = at_idx(keys, i);
                prm = at_obj(obj, sym);
                drop_obj(sym);
                pool_add_task(pool, i, eval_field, prm, 1);
            }

            vals = pool_run(pool, l);
        }
    }
    else
    {
        drop_obj(keys);

        // Groupings
        if (gkeys != NULL_OBJ)
        {
            keys = ray_except(as_list(tab)[0], gkeys);
            l = keys->len;
            vals = list(l);

            for (i = 0; i < l; i++)
            {
                sym = at_idx(keys, i);
                prm = ray_get(sym);
                drop_obj(sym);

                if (is_error(prm))
                {
                    vals->len = i;
                    drop_obj(vals);
                    drop_obj(tab);
                    drop_obj(keys);
                    drop_obj(gkeys);
                    drop_obj(gcol);
                    return prm;
                }

                val = aggr_first(as_list(prm)[0], as_list(prm)[1], as_list(prm)[2]);
                drop_obj(prm);

                as_list(vals)[i] = val;
            }
        }
        // No groupings
        else
        {
            keys = clone_obj(as_list(tab)[0]);
            l = keys->len;
            vals = list(l);

            for (i = 0; i < l; i++)
            {
                sym = at_idx(keys, i);
                prm = ray_get(sym);
                drop_obj(sym);

                if (prm->type == TYPE_FILTERMAP)
                {
                    val = filter_collect(prm);
                    drop_obj(prm);
                }
                else if (prm->type == TYPE_ENUM)
                {
                    val = ray_value(prm);
                    drop_obj(prm);
                }
                else
                    val = prm;

                if (is_error(val))
                {
                    vals->len = i;
                    drop_obj(vals);
                    drop_obj(tab);
                    drop_obj(keys);
                    drop_obj(gkeys);
                    drop_obj(gcol);
                    return val;
                }

                as_list(vals)[i] = val;
            }
        }
    }

    // Prepare result table
    if (gkeys->type == -TYPE_SYMBOL) // Grouped by one column
    {
        val = ray_concat(gkeys, keys);
        drop_obj(keys);
        drop_obj(gkeys);
        keys = val;
        val = list(vals->len + 1);
        as_list(val)[0] = gcol;
        for (i = 0; i < vals->len; i++)
            as_list(val)[i + 1] = clone_obj(as_list(vals)[i]);
        drop_obj(vals);
        vals = val;
    }
    else if (gkeys->type == TYPE_SYMBOL) // Grouped by multiple columns
    {
        val = ray_concat(gkeys, keys);
        drop_obj(keys);
        drop_obj(gkeys);
        keys = val;

        val = list(vals->len + gcol->len);

        l = gcol->len;
        for (i = 0; i < l; i++)
            as_list(val)[i] = clone_obj(as_list(gcol)[i]);

        for (i = 0; i < vals->len; i++)
            as_list(val)[i + l] = clone_obj(as_list(vals)[i]);

        drop_obj(vals);
        drop_obj(gcol);

        vals = val;
    }

    unmount_env(tablen);
    drop_obj(tab);

    val = ray_table(keys, vals);
    drop_obj(keys);
    drop_obj(vals);

    return val;
}
