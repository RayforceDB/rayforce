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

/*
 * result is a list:
 *   [0] - value
 *   [1] - bins
 *   [2] - filters
 */
obj_t compose_groupmap(obj_t x, obj_t y, obj_t z)
{
    u64_t i, l;
    obj_t v, res;

    switch (x->type)
    {
    case TYPE_TABLE:
        l = as_list(x)[1]->len;
        res = vector(TYPE_LIST, l);
        for (i = 0; i < l; i++)
        {
            v = as_list(as_list(x)[1])[i];
            as_list(res)[i] = compose_groupmap(v, y, z);
        }

        return table(clone(as_list(x)[0]), res);

    default:
        res = vn_list(3, clone(x), clone(y), clone(z));
        res->type = TYPE_GROUPMAP;
        return res;
    }
}

obj_t group_by(obj_t x, obj_t y, obj_t z)
{
    u64_t l;
    i64_t *ids;
    obj_t bins, res;

    if (z)
    {
        l = z->len;
        ids = as_i64(z);
    }
    else
    {
        l = x->len;
        ids = NULL;
    }

    switch (x->type)
    {
    case TYPE_BOOL:
    case TYPE_BYTE:
    case TYPE_CHAR:
        bins = index_group_i8((i8_t *)as_u8(x), ids, l);
        break;
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        bins = index_group_i64(as_i64(x), ids, l);
        break;
    case TYPE_F64:
        bins = index_group_i64((i64_t *)as_f64(x), ids, l);
        break;
    case TYPE_GUID:
        bins = index_group_guid(as_guid(x), ids, l);
        break;
    case TYPE_LIST:
        bins = index_group_obj(as_list(x), ids, l);
        break;
    default:
        throw(ERR_TYPE, "'by' unable to group by: %s", typename(x->type));
    }

    res = compose_groupmap(y, bins, z);
    mount_env(res);
    drop(res);

    res = aggr_first(x, bins, z);
    drop(bins);

    return res;
}

obj_t collect_group(obj_t obj, obj_t grp, obj_t *group_counts)
{
    u64_t i, l, m, n;
    obj_t bins, res;
    i64_t *cnts, *grps, *ids;
    // Count groups
    if (*group_counts == NULL)
    {
        n = as_list(grp)[0]->i64;
        l = as_list(grp)[1]->len;
        *group_counts = vector_i64(n);
        grps = as_i64(*group_counts);
        memset(grps, 0, n * sizeof(i64_t));
        ids = as_i64(as_list(grp)[1]);
        for (i = 0; i < l; i++)
            grps[ids[i]]++;
    }

    cnts = as_i64(*group_counts);
    bins = as_list(grp)[1];
    n = (*group_counts)->len;
    l = bins->len;

    switch (obj->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        res = list(n);
        for (i = 0; i < n; i++)
        {
            m = cnts[i];
            as_list(res)[i] = vector(obj->type, m);
            as_list(res)[i]->len = 0;
        }

        for (i = 0; i < l; i++)
        {
            n = as_i64(bins)[i];
            as_i64(as_list(res)[n])[as_list(res)[n]->len++] = as_i64(obj)[i];
        }

        return res;
    case TYPE_F64:
        res = list(n);
        for (i = 0; i < n; i++)
        {
            m = cnts[i];
            as_list(res)[i] = vector(obj->type, m);
            as_list(res)[i]->len = 0;
        }

        for (i = 0; i < l; i++)
        {
            n = as_i64(bins)[i];
            as_f64(as_list(res)[n])[as_list(res)[n]->len++] = as_f64(obj)[i];
        }

        return res;

    case TYPE_LIST:
        res = list(n);
        for (i = 0; i < n; i++)
        {
            m = cnts[i];
            as_list(res)[i] = vector(obj->type, m);
            as_list(res)[i]->len = 0;
        }

        for (i = 0; i < l; i++)
        {
            n = as_i64(bins)[i];
            as_list(as_list(res)[n])[as_list(res)[n]->len++] = clone(as_list(obj)[i]);
        }

        return res;
    default:
        throw(ERR_TYPE, "collect_group: unsupported type: '%s", typename(obj->type));
    }
}

obj_t group_call(obj_t car, obj_t args, type_t types[], u64_t cnt)
{
    u64_t i, j, l;
    obj_t v, res;

    l = args->len;

    res = list(cnt);

    for (i = 0; i < cnt; i++)
    {
        for (j = 0; j < l; j++)
        {
            if (types[j] == TYPE_GROUPMAP)
                v = at_idx(as_list(args)[j], i);
            else
                v = clone(as_list(args)[j]);

            stack_push(v);
        }

        v = call(car, l);

        if (is_error(v))
        {
            res->len = i;
            drop(res);
            return v;
        }

        ins_obj(&res, i, v);
    }

    return res;
}

obj_t filter_call(obj_t car, obj_t args)
{
    u64_t i, l;
    obj_t v;

    l = args->len;

    for (i = 0; i < l; i++)
    {
        v = clone(as_list(args)[i]);
        stack_push(v);
    }

    return call(car, l);
}

obj_t field_map(obj_t car, obj_t *args, u64_t args_len, obj_t *group_counts)
{
    u64_t i;
    type_t types[args_len];
    obj_t res, x, y, v;
    lambda_t *lambda;

    switch (car->type)
    {
    case TYPE_UNARY:
        x = eval(args[0]);

        if (is_error(x))
            return x;

        if (x->type == TYPE_GROUPMAP)
        {
            if (!(car->attrs & FN_AGGR))
            {
                v = collect_group(as_list(x)[0], as_list(x)[1], group_counts);
                drop(x);
                x = v;
            }
        }

        res = ray_call_unary(car->attrs, (unary_f)car->i64, x);
        drop(x);

        return res;

    case TYPE_BINARY:
        x = eval(args[0]);

        if (is_error(x))
            return x;

        y = eval(args[1]);

        if (is_error(y))
        {
            drop(x);
            return y;
        }

        if (x->type == TYPE_GROUPMAP)
        {
            if (!(car->attrs & FN_AGGR))
            {
                v = collect_group(as_list(x)[0], as_list(x)[1], group_counts);
                drop(x);
                x = v;
            }
        }

        if (y->type == TYPE_GROUPMAP)
        {
            if (!(car->attrs & FN_AGGR))
            {
                v = collect_group(as_list(y)[0], as_list(y)[1], group_counts);
                drop(y);
                y = v;
            }
        }

        res = ray_call_binary(car->attrs, (binary_f)car->i64, x, y);
        drop(x);
        drop(y);

        return res;
    case TYPE_VARY:
        if (car->attrs & FN_SPECIAL_FORM)
            res = ((vary_f)car->i64)(args, args_len);
        else
        {
            if (!stack_enough(args_len))
                return unwrap(error_str(ERR_STACK_OVERFLOW, "stack overflow"), (i64_t)car);

            for (i = 0; i < args_len; i++)
            {
                x = eval(args[i]);
                if (is_error(x))
                    return x;

                if (x->type == TYPE_GROUPMAP)
                {
                    if (!(car->attrs & FN_AGGR))
                    {
                        v = collect_group(as_list(x)[0], as_list(x)[1], group_counts);
                        drop(x);
                        x = v;
                    }
                }
                else if (x->type == TYPE_FILTERMAP)
                {
                    v = at_obj(as_list(x)[0], as_list(x)[1]);
                    drop(x);
                    x = v;
                }
                stack_push(x);
            }

            res = ray_call_vary(car->attrs, (vary_f)car->i64, stack_peek(args_len - 1), args_len);

            for (i = 0; i < args_len; i++)
                drop(stack_pop());
        }

        return unwrap(res, (i64_t)car);

    case TYPE_LAMBDA:
        lambda = as_lambda(car);
        if (args_len != lambda->args->len)
            return unwrap(error_str(ERR_ARITY, "wrong number of arguments"), (i64_t)car);

        if (!stack_enough(args_len))
            return unwrap(error_str(ERR_STACK_OVERFLOW, "stack overflow"), (i64_t)car);

        res = list(args_len);

        for (i = 0; i < args_len; i++)
        {
            x = eval(args[i]);
            if (is_error(x))
                return x;

            types[i] = x->type;
            if (x->type == TYPE_GROUPMAP)
            {
                v = collect_group(as_list(x)[0], as_list(x)[1], group_counts);
                drop(x);
                x = v;
            }
            else if (x->type == TYPE_FILTERMAP)
            {
                v = at_obj(as_list(x)[0], as_list(x)[1]);
                drop(x);
                x = v;
            }

            if (is_error(x))
            {
                res->len = i;
                drop(res);
                return x;
            }

            as_list(res)[i] = x;
        }

        if (*group_counts)
            v = group_call(car, res, types, (*group_counts)->len);
        else
            v = filter_call(car, res);

        drop(res);

        return v;

    default:
        return error(ERR_TYPE, "eval_field: not callable: '%s'", typename(car->type));
    }
}

obj_t field_eval(obj_t x, obj_t *group_counts)
{
    obj_t res, v;

    switch (x->type)
    {
    case TYPE_LIST:
        if (x->len == 0)
            return NULL;

        v = eval(as_list(x)[0]);
        res = field_map(v, as_list(x) + 1, x->len - 1, group_counts);
        drop(v);
        return res;
    default:
        res = eval(x);
        if (res->type == TYPE_GROUPMAP)
        {
            v = collect_group(as_list(res)[0], as_list(res)[1], group_counts);
            drop(res);
            res = v;
        }
        return res;
    }
}

obj_t ray_select(obj_t obj)
{
    u64_t i, l, tablen;
    obj_t keys = NULL, vals = NULL, filters = NULL, groupby = NULL,
          bycol = NULL, bysym = NULL, tab, sym, prm, val;

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
    if (prm)
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

    // if (filters->len == 0)
    // {
    //     drop(filters);
    //     filters = NULL;
    // }

    // Apply groupping
    prm = get_param(obj, "by");
    if (prm)
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

        bycol = group_by(groupby, tab, filters);
        drop(filters);
        drop(groupby);

        if (is_error(bycol))
        {
            drop(tab);
            return bycol;
        }
    }
    else if (filters)
    {
        // Unmount table columns from a local env
        unmount_env(tablen);
        // Create filtermaps over table
        val = ray_filtermap(tab, filters);
        drop(filters);
        mount_env(val);
        drop(val);
    }

    // Find all mappings (non-keyword fields)
    keys = get_symbols(obj);
    l = keys->len;
    vals = list(l);

    // Apply mappings
    for (i = 0; i < l; i++)
    {
        sym = at_idx(keys, i);
        prm = at_obj(obj, sym);
        drop(sym);
        val = eval(prm);
        drop(prm);

        if (!val || is_error(val))
        {
            vals->len = i;
            drop(vals);
            drop(tab);
            drop(keys);
            drop(bysym);
            drop(bycol);
            return val;
        }

        ins_obj(&vals, i, val);
    }

    // Prepare result table
    if (bysym)
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

    return table(keys, vals);
}