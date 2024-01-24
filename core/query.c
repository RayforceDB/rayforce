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

// obj_t field_map(obj_t car, obj_t *args, u64_t args_len, obj_t *group_counts)
// {
//     u64_t i;
//     type_t types[args_len];
//     obj_t res, x, y, v;
//     lambda_t *lambda;

//     switch (car->type)
//     {
//     case TYPE_UNARY:
//         x = eval(args[0]);

//         if (is_error(x))
//             return x;

//         if (x->type == TYPE_GROUPMAP)
//         {
//             if (!(car->attrs & FN_AGGR))
//             {
//                 v = collect_group(as_list(x)[0], as_list(x)[1], group_counts);
//                 drop(x);
//                 x = v;
//             }
//         }

//         res = ray_call_unary(car->attrs, (unary_f)car->i64, x);
//         drop(x);

//         return res;

//     case TYPE_BINARY:
//         x = eval(args[0]);

//         if (is_error(x))
//             return x;

//         y = eval(args[1]);

//         if (is_error(y))
//         {
//             drop(x);
//             return y;
//         }

//         if (x->type == TYPE_GROUPMAP)
//         {
//             if (!(car->attrs & FN_AGGR))
//             {
//                 v = collect_group(as_list(x)[0], as_list(x)[1], group_counts);
//                 drop(x);
//                 x = v;
//             }
//         }

//         if (y->type == TYPE_GROUPMAP)
//         {
//             if (!(car->attrs & FN_AGGR))
//             {
//                 v = collect_group(as_list(y)[0], as_list(y)[1], group_counts);
//                 drop(y);
//                 y = v;
//             }
//         }

//         res = ray_call_binary(car->attrs, (binary_f)car->i64, x, y);
//         drop(x);
//         drop(y);

//         return res;
//     case TYPE_VARY:
//         if (car->attrs & FN_SPECIAL_FORM)
//             res = ((vary_f)car->i64)(args, args_len);
//         else
//         {
//             if (!stack_enough(args_len))
//                 return unwrap(error_str(ERR_STACK_OVERFLOW, "stack overflow"), (i64_t)car);

//             for (i = 0; i < args_len; i++)
//             {
//                 x = eval(args[i]);
//                 if (is_error(x))
//                     return x;

//                 if (x->type == TYPE_GROUPMAP)
//                 {
//                     if (!(car->attrs & FN_AGGR))
//                     {
//                         v = collect_group(as_list(x)[0], as_list(x)[1], group_counts);
//                         drop(x);
//                         x = v;
//                     }
//                 }
//                 else if (x->type == TYPE_FILTERMAP)
//                 {
//                     v = at_obj(as_list(x)[0], as_list(x)[1]);
//                     drop(x);
//                     x = v;
//                 }
//                 stack_push(x);
//             }

//             res = ray_call_vary(car->attrs, (vary_f)car->i64, stack_peek(args_len - 1), args_len);

//             for (i = 0; i < args_len; i++)
//                 drop(stack_pop());
//         }

//         return unwrap(res, (i64_t)car);

//     case TYPE_LAMBDA:
//         lambda = as_lambda(car);
//         if (args_len != lambda->args->len)
//             return unwrap(error_str(ERR_ARITY, "wrong number of arguments"), (i64_t)car);

//         if (!stack_enough(args_len))
//             return unwrap(error_str(ERR_STACK_OVERFLOW, "stack overflow"), (i64_t)car);

//         res = list(args_len);

//         for (i = 0; i < args_len; i++)
//         {
//             x = eval(args[i]);
//             if (is_error(x))
//                 return x;

//             types[i] = x->type;
//             if (x->type == TYPE_GROUPMAP)
//             {
//                 v = collect_group(as_list(x)[0], as_list(x)[1], group_counts);
//                 drop(x);
//                 x = v;
//             }
//             else if (x->type == TYPE_FILTERMAP)
//             {
//                 v = at_obj(as_list(x)[0], as_list(x)[1]);
//                 drop(x);
//                 x = v;
//             }

//             if (is_error(x))
//             {
//                 res->len = i;
//                 drop(res);
//                 return x;
//             }

//             as_list(res)[i] = x;
//         }

//         if (*group_counts)
//             v = group_call(car, res, types, (*group_counts)->len);
//         else
//             v = filter_call(car, res);

//         drop(res);

//         return v;

//     default:
//         return error(ERR_TYPE, "eval_field: not callable: '%s'", typename(car->type));
//     }
// }

// obj_t field_eval(obj_t x, obj_t *group_counts)
// {
//     obj_t res, v;

//     switch (x->type)
//     {
//     case TYPE_LIST:
//         if (x->len == 0)
//             return NULL;

//         v = eval(as_list(x)[0]);
//         res = field_map(v, as_list(x) + 1, x->len - 1, group_counts);
//         drop(v);
//         return res;
//     default:
//         res = eval(x);
//         if (res->type == TYPE_GROUPMAP)
//         {
//             v = collect_group(as_list(res)[0], as_list(res)[1], group_counts);
//             drop(res);
//             res = v;
//         }
//         return res;
//     }
// }

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

        prm = group_map(&bycol, groupby, tab, filters);
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

        // Materialize groupmaps
        if (val->type == TYPE_GROUPMAP)
        {
            prm = group_collect(val);
            drop(val);
            val = prm;
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

    val = ray_table(keys, vals);
    drop(keys);
    drop(vals);

    return val;
}