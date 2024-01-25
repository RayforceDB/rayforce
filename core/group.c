/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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

#include "group.h"
#include "error.h"
#include "ops.h"
#include "util.h"
#include "index.h"
#include "aggr.h"

/*
 * group index is a list:
 * [0] - groups number
 * [1] - bins
 * [2] - count of each group
 * --
 * result is a list
 * [0] - indexed object
 * [1] - group index (see above)
 * [2] - filters
 */
obj_t __group(obj_t x, obj_t y, obj_t z)
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
            as_list(res)[i] = __group(v, y, z);
        }

        return table(clone(as_list(x)[0]), res);

    default:
        res = vn_list(3, clone(x), clone(y), clone(z));
        res->type = TYPE_GROUPMAP;
        return res;
    }
}

obj_t group_map(obj_t *aggr, obj_t x, obj_t y, obj_t z)
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

    if (l > ops_count(y))
        throw(ERR_LENGTH, "'by': groups count: %lld can't be greater than source length: %lld", l, ops_count(y));

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

    res = __group(y, bins, z);

    *aggr = aggr_first(x, bins, z);
    drop(bins);

    return res;
}

obj_t group_collect(obj_t x)
{
    u64_t i, l, m, n;
    obj_t obj, grp, bins, res, group_counts;
    i64_t *cnts, *grps, *ids, *filters;

    obj = as_list(x)[0];
    grp = as_list(x)[1];

    filters = (as_list(x)[2] != NULL) ? as_i64(as_list(x)[2]) : NULL;

    // Count groups
    // TODO: this point must be synchronized in case of parallel execution
    group_counts = as_list(grp)[2];
    if (group_counts == NULL)
    {
        n = as_list(grp)[0]->i64;
        l = as_list(grp)[1]->len;
        group_counts = vector_i64(n);
        grps = as_i64(group_counts);
        memset(grps, 0, n * sizeof(i64_t));
        ids = as_i64(as_list(grp)[1]);
        for (i = 0; i < l; i++)
            grps[ids[i]]++;

        as_list(grp)[2] = group_counts;
    }
    // --

    cnts = as_i64(group_counts);
    bins = as_list(grp)[1];
    n = group_counts->len;
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

        if (filters)
        {
            for (i = 0; i < l; i++)
            {
                n = as_i64(bins)[i];
                as_i64(as_list(res)[n])[as_list(res)[n]->len++] = as_i64(obj)[filters[i]];
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = as_i64(bins)[i];
                as_i64(as_list(res)[n])[as_list(res)[n]->len++] = as_i64(obj)[i];
            }
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

// obj_t group_call(obj_t car, obj_t args, type_t types[], u64_t cnt)
// {
//     u64_t i, j, l;
//     obj_t v, res;

//     l = args->len;

//     res = list(cnt);

//     for (i = 0; i < cnt; i++)
//     {
//         for (j = 0; j < l; j++)
//         {
//             if (types[j] == TYPE_GROUPMAP)
//                 v = at_idx(as_list(args)[j], i);
//             else
//                 v = clone(as_list(args)[j]);

//             stack_push(v);
//         }

//         v = call(car, l);

//         if (is_error(v))
//         {
//             res->len = i;
//             drop(res);
//             return v;
//         }

//         ins_obj(&res, i, v);
//     }

//     return res;
// }

// obj_t filter_call(obj_t car, obj_t args)
// {
//     u64_t i, l;
//     obj_t v;

//     l = args->len;

//     for (i = 0; i < l; i++)
//     {
//         v = clone(as_list(args)[i]);
//         stack_push(v);
//     }

//     return call(car, l);
// }