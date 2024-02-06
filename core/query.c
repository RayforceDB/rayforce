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

obj_t get_param(obj_t obj, str_t name)
{
    obj_t key, val;

    key = symbol(name);
    val = at_obj(obj, key);
    drop(key);

    return val;
}

obj_t get_symbols(obj_t obj)
{
    obj_t keywords, symbols;

    keywords = vn_symbol(4, "take", "by", "from", "where");
    symbols = ray_except(as_list(obj)[0], keywords);
    drop(keywords);

    return symbols;
}

obj_t remap_filter(obj_t x, obj_t y)
{
    u64_t i, l;
    obj_t res;

    l = as_list(x)[1]->len;
    res = list(l);
    for (i = 0; i < l; i++)
        as_list(res)[i] = filter_map(clone(as_list(as_list(x)[1])[i]), clone(y));

    return table(clone(as_list(x)[0]), res);
}

obj_t ray_select(obj_t obj)
{
    u64_t i, l, tablen;
    obj_t keys = NULL_OBJ, vals = NULL_OBJ, filters = NULL_OBJ, groupby = NULL_OBJ,
          bycol = NULL_OBJ, bysym = NULL_OBJ, tab, sym, prm, val;

    if (obj->type != TYPE_DICT)
        throw(ERR_LENGTH, "'select' takes dict of params");

    if (as_list(obj)[0]->type != TYPE_SYMBOL)
        throw(ERR_LENGTH, "'select' takes dict with symbol keys");

    // Retrive a table
    prm = get_param(obj, "from");

    if (is_null(prm))
        throw(ERR_LENGTH, "'select' expects 'from' param");

    tab = eval(prm);
    drop(prm);

    if (is_error(tab))
        return tab;

    if (tab->type != TYPE_TABLE)
    {
        drop(tab);
        throw(ERR_TYPE, "'select' from: expects table");
    }

    // Mount table columns to a local env
    tablen = as_list(tab)[0]->len;
    mount_env(tab);

    // Apply filters
    prm = get_param(obj, "where");
    if (prm != NULL_OBJ)
    {
        val = eval(prm);
        drop(prm);
        if (is_error(val))
        {
            drop(tab);
            return val;
        }

        filters = ray_where(val);
        drop(val);
        if (is_error(filters))
        {
            drop(tab);
            return filters;
        }
    }

    // Apply groupping
    prm = get_param(obj, "by");
    if (prm != NULL_OBJ)
    {
        groupby = eval(prm);
        if (prm->type == -TYPE_SYMBOL)
            bysym = prm;
        else
        {
            bysym = symbol("x");
            drop(prm);
        }

        unmount_env(tablen);

        if (is_error(groupby))
        {
            drop(tab);
            return groupby;
        }

        prm = group_map(&bycol, groupby, tab, filters);

        if (is_error(prm))
        {
            drop(tab);
            drop(filters);
            drop(groupby);
            drop(bysym);
            return prm;
        }

        mount_env(prm);

        drop(prm);
        drop(filters);
        drop(groupby);

        if (is_error(bycol))
        {
            drop(tab);
            return bycol;
        }
    }
    else if (filters != NULL_OBJ)
    {
        // Unmount table columns from a local env
        unmount_env(tablen);
        // Create filtermaps over table
        val = remap_filter(tab, filters);
        drop(filters);
        mount_env(val);
        drop(val);
    }

    // Find all mappings (non-keyword fields)
    keys = get_symbols(obj);
    l = keys->len;

    // Apply mappings
    if (l)
    {
        vals = list(l);
        for (i = 0; i < l; i++)
        {
            sym = at_idx(keys, i);
            prm = at_obj(obj, sym);
            drop(sym);
            val = eval(prm);
            drop(prm);

            if (is_error(val))
            {
                vals->len = i;
                drop(vals);
                drop(tab);
                drop(keys);
                drop(bysym);
                drop(bycol);
                return val;
            }

            // Materialize fields
            if (val->type == TYPE_GROUPMAP)
            {
                prm = group_collect(val);
                drop(val);
                val = prm;
            }
            else if (val->type == TYPE_FILTERMAP)
            {
                prm = filter_collect(val);
                drop(val);
                val = prm;
            }
            else if (val->type == TYPE_ENUM)
            {
                prm = ray_value(val);
                drop(val);
                val = prm;
            }

            if (is_error(val))
            {
                vals->len = i;
                drop(vals);
                drop(tab);
                drop(keys);
                drop(bysym);
                drop(bycol);
                return val;
            }

            as_list(vals)[i] = val;
        }
    }
    else
    {
        drop(keys);

        // Groupings
        if (bysym != NULL_OBJ)
        {
            keys = ray_except(as_list(tab)[0], bysym);
            l = keys->len;
            vals = list(l);

            for (i = 0; i < l; i++)
            {
                sym = at_idx(keys, i);
                prm = ray_get(sym);
                drop(sym);

                if (is_error(prm))
                {
                    vals->len = i;
                    drop(vals);
                    drop(tab);
                    drop(bysym);
                    drop(bycol);
                    return prm;
                }

                val = aggr_first(as_list(prm)[0], as_list(prm)[1], as_list(prm)[2]);
                drop(prm);

                as_list(vals)[i] = val;
            }
        }
        // No groupings
        else
        {
            keys = clone(as_list(tab)[0]);
            l = keys->len;
            vals = list(l);

            for (i = 0; i < l; i++)
            {
                sym = at_idx(keys, i);
                prm = ray_get(sym);
                drop(sym);

                if (prm->type == TYPE_FILTERMAP)
                {
                    val = filter_collect(prm);
                    drop(prm);
                }
                else if (prm->type == TYPE_ENUM)
                {
                    val = ray_value(prm);
                    drop(prm);
                }
                else
                    val = prm;

                if (is_error(val))
                {
                    vals->len = i;
                    drop(vals);
                    drop(tab);
                    return val;
                }

                as_list(vals)[i] = val;
            }
        }
    }

    // Prepare result table
    if (bysym != NULL_OBJ)
    {
        val = ray_concat(bysym, keys);
        drop(keys);
        drop(bysym);
        keys = val;
        val = list(vals->len + 1);
        as_list(val)[0] = bycol;
        for (i = 0; i < vals->len; i++)
            as_list(val)[i + 1] = clone(as_list(vals)[i]);
        drop(vals);
        vals = val;
    }

    unmount_env(tablen);
    drop(tab);

    val = ray_table(keys, vals);
    drop(keys);
    drop(vals);

    return val;
}
