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

#include <math.h>
#include "aggr.h"
#include "math.h"
#include "ops.h"
#include "error.h"
#include "hash.h"
#include "util.h"
#include "items.h"
#include "unary.h"
#include "group.h"
#include "string.h"
#include "runtime.h"
#include "index.h"
#include "pool.h"

obj_p aggr_map(raw_p aggr, obj_p val, obj_p index)
{
    pool_p pool = runtime_get()->pool;
    u64_t i, l, n, group_count, chunk, buckets;
    obj_p res, parts;
    raw_p argv[6];

    buckets = (u64_t)as_list(index)[0]->i64;
    n = pool_executors_count(pool);
    group_count = index_group_count(index);

    if (n == 1 || group_count < 2)
    {
        res = vector(val->type, buckets);
        argv[0] = (raw_p)group_count;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        argv[4] = res;
        pool_call_task_fn(aggr, 5, argv);
        return vn_list(1, res);
    }

    pool_prepare(pool, n);

    l = val->len;
    chunk = l / n;

    for (i = 0; i < n - 1; i++)
        pool_add_task(pool, aggr, 5, chunk, i * chunk, val, index, vector(val->type, buckets));

    pool_add_task(pool, aggr, 5, l - i * chunk, i * chunk, val, index, vector(val->type, buckets));

    parts = pool_run(pool, n);

    return parts;
}

obj_p aggr_sum_partial(u64_t len, u64_t offset, obj_p val, obj_p index, obj_p res)
{
    u64_t i, n;
    i64_t *xi, *xm, *xk, *xo, shift;
    f64_t *xf, *fo;

    n = index_group_count(index);

    switch (val->type)
    {
    case TYPE_I64:
        xi = as_i64(val) + offset;
        xo = as_i64(res);

        memset(xo, 0, n * sizeof(i64_t));

        for (i = 0; i < len; i++)
        {
            n = index_group_get_id(index, i + offset);
            xo[n] = addi64(xo[n], xi[i]);
        }

        return res;

    case TYPE_F64:
        xf = as_f64(val) + offset;
        xm = as_i64(as_list(index)[2]);
        xk = as_i64(as_list(index)[3]) + offset;
        fo = as_f64(res);

        memset(fo, 0, n * sizeof(f64_t));

        for (i = 0; i < len; i++)
        {
            n = xk[i] - shift;
            fo[xm[n]] = addf64(fo[xm[n]], xf[i]);
        }

        return res;
    default:
        return error(ERR_TYPE, "sum: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_sum(obj_p val, obj_p index)
{
    u64_t i, j, l, n;
    i64_t *xi, *xo;
    f64_t *fo, *fi;
    obj_p res, parts;

    switch (val->type)
    {
    case TYPE_I64:
        parts = aggr_map(aggr_sum_partial, val, index);
        n = as_list(index)[0]->i64;
        l = parts->len;
        res = clone_obj(as_list(parts)[0]);
        xo = as_i64(res);
        for (i = 1; i < l; i++)
        {
            xi = as_i64(as_list(parts)[i]);
            for (j = 0; j < n; j++)
                xo[j] = addi64(xo[j], xi[j]);
        }
        drop_obj(parts);
        return res;
    case TYPE_F64:
        parts = aggr_map(aggr_sum_partial, val, index);
        n = as_list(index)[0]->i64;
        l = parts->len;
        res = clone_obj(as_list(parts)[0]);
        fo = as_f64(res);
        for (i = 1; i < l; i++)
        {
            fi = as_f64(as_list(parts)[i]);
            for (j = 0; j < n; j++)
                fo[j] = addf64(fo[j], fi[j]);
        }
        drop_obj(parts);
        return res;
    default:
        return error(ERR_TYPE, "sum: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_first_partial(u64_t len, u64_t offset, obj_p val, obj_p index, obj_p res)
{
    u64_t i, n;
    i64_t *xi, *xo, *ids;
    f64_t *xf, *fo;

    n = index_group_count(index);

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
        xi = as_i64(val) + offset;
        xo = as_i64(res);

        for (i = 0; i < n; i++)
            xo[i] = NULL_I64;

        for (i = 0; i < len; i++)
        {
            n = index_group_get_id(index, i + offset);
            if (xo[n] == NULL_I64)
                xo[n] = xi[i];
        }

        return res;
    default:
        return error(ERR_TYPE, "first: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_first(obj_p val, obj_p index)
{
    u64_t i, j, l, n;
    i64_t *xi, *xo;
    obj_p res, parts;

    parts = aggr_map(aggr_first_partial, val, index);
    n = index_group_count(index);
    l = parts->len;

    res = clone_obj(as_list(parts)[0]);
    xo = as_i64(res);

    for (i = 1; i < l; i++)
    {
        xi = as_i64(as_list(parts)[i]);
        for (j = 0; j < n; j++)
            xo[j] = (xo[j] == NULL_I64) ? xi[j] : xo[j];
    }

    drop_obj(parts);

    return res;
}

obj_p aggr_last(obj_p val, obj_p index)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *xo, *ids;
    obj_p res;

    n = index_group_count(index);
    l = as_list(index)[1]->len;

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_TIMESTAMP:
    case TYPE_SYMBOL:
        // xi = as_i64(val);
        // xm = as_i64(as_list(index)[1]);
        // res = vector(val->type, n);
        // xo = as_i64(res);
        // for (i = 0; i < n; i++)
        //     xo[i] = NULL_I64;

        // if (filter != NULL_OBJ)
        // {
        //     ids = as_i64(filter);
        //     for (i = 0; i < l; i++)
        //     {
        //         n = xm[i];
        //         xo[n] = xi[ids[i]];
        //     }
        // }
        // else
        // {
        //     for (i = 0; i < l; i++)
        //     {
        //         n = xm[i];
        //         xo[n] = xi[i];
        //     }
        // }
        // return res;

    default:
        return error(ERR_TYPE, "last: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_max(obj_p val, obj_p index)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *xo, *ids;
    f64_t *xf, *fo;
    obj_p res;

    n = index_group_count(index);
    l = as_list(index)[1]->len;

    switch (val->type)
    {
    // case TYPE_I64:
    // case TYPE_TIMESTAMP:
    // case TYPE_SYMBOL:
    //     xi = as_i64(val);
    //     xm = as_i64(as_list(index)[1]);
    //     res = vector(val->type, n);
    //     xo = as_i64(res);
    //     for (i = 0; i < n; i++)
    //         xo[i] = NULL_I64;

    //     if (filter != NULL_OBJ)
    //     {
    //         ids = as_i64(filter);
    //         for (i = 0; i < l; i++)
    //         {
    //             n = xm[i];
    //             xo[n] = maxi64(xo[n], xi[ids[i]]);
    //         }
    //     }
    //     else
    //     {
    //         for (i = 0; i < l; i++)
    //         {
    //             n = xm[i];
    //             xo[n] = maxi64(xo[n], xi[i]);
    //         }
    //     }
    //     return res;
    // case TYPE_F64:
    //     xf = as_f64(val);
    //     xm = as_i64(as_list(bins)[1]);
    //     res = vector_f64(n);
    //     fo = as_f64(res);
    //     for (i = 0; i < n; i++)
    //         fo[i] = NULL_F64;

    //     if (filter != NULL_OBJ)
    //     {
    //         ids = as_i64(filter);
    //         for (i = 0; i < l; i++)
    //         {
    //             n = xm[i];
    //             fo[n] = maxf64(fo[n], xf[ids[i]]);
    //         }
    //     }
    //     else
    //     {
    //         for (i = 0; i < l; i++)
    //         {
    //             n = xm[i];
    //             fo[n] = maxf64(fo[n], xf[i]);
    //         }
    //     }
    //     return res;
    default:
        return error(ERR_TYPE, "max: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_min(obj_p val, obj_p index)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *xo, *ids;
    f64_t *xf, *fo;
    obj_p res;

    n = index_group_count(index);
    l = as_list(index)[1]->len;

    switch (val->type)
    {
    // case TYPE_I64:
    // case TYPE_TIMESTAMP:
    //     xi = as_i64(val);
    //     xm = as_i64(as_list(bins)[1]);
    //     res = vector(val->type, n);
    //     xo = as_i64(res);
    //     for (i = 0; i < n; i++)
    //         xo[i] = NULL_I64;

    //     if (filter != NULL_OBJ)
    //     {
    //         ids = as_i64(filter);
    //         for (i = 0; i < l; i++)
    //         {
    //             n = xm[i];
    //             if (xo[n] == NULL_I64 || xi[ids[i]] < xo[n])
    //                 xo[n] = xi[ids[i]];
    //         }
    //     }
    //     else
    //     {
    //         for (i = 0; i < l; i++)
    //         {
    //             n = xm[i];
    //             xo[n] = mini64(xo[n], xi[i]);
    //         }
    //     }

    //     return res;
    // case TYPE_F64:
    //     xf = as_f64(val);
    //     xm = as_i64(as_list(bins)[1]);
    //     res = vector_f64(n);
    //     fo = as_f64(res);
    //     for (i = 0; i < n; i++)
    //         fo[i] = NULL_F64;

    //     if (filter != NULL_OBJ)
    //     {
    //         ids = as_i64(filter);
    //         for (i = 0; i < l; i++)
    //         {
    //             n = xm[i];
    //             fo[n] = minf64(fo[n], xf[ids[i]]);
    //         }
    //     }
    //     else
    //     {
    //         for (i = 0; i < l; i++)
    //         {
    //             n = xm[i];
    //             fo[n] = minf64(fo[n], xf[i]);
    //         }
    //     }
    //     return res;
    default:
        return error(ERR_TYPE, "min: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_count_partial(u64_t len, u64_t offset, obj_p val, obj_p index, obj_p res)
{
    u64_t i, n;
    i64_t *xi, *xm, *xk, *xo, *ids, min;
    f64_t *xf, *fo;

    n = index_group_count(index);
    min = as_list(index)[1]->i64;

    switch (val->type)
    {
    // case TYPE_I64:
    //     xi = as_i64(val) + offset;
    //     xm = as_i64(as_list(bins)[2]);
    //     xk = as_i64(as_list(bins)[3]) + offset;
    //     xo = as_i64(res);

    //     memset(xo, 0, n * sizeof(i64_t));

    //     for (i = 0; i < len; i++)
    //     {
    //         n = xk[i] - min;
    //         xo[xm[n]]++;
    //     }

    //     return res;

    // case TYPE_F64:
    //     xf = as_f64(val) + offset;
    //     xm = as_i64(as_list(bins)[2]);
    //     xk = as_i64(as_list(bins)[3]) + offset;
    //     fo = as_f64(res);

    //     memset(fo, 0, n * sizeof(f64_t));

    //     for (i = 0; i < len; i++)
    //     {
    //         n = xk[i] - min;
    //         fo[xm[n]] = addf64(fo[xm[n]], xf[i]);
    //     }

    //     return res;
    default:
        return error(ERR_TYPE, "count: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_count(obj_p val, obj_p index)
{
    u64_t i, j, l, n;
    i64_t *xi, *xo;
    obj_p res, parts;

    parts = aggr_map(aggr_count_partial, val, index);
    n = as_list(index)[0]->i64;
    l = parts->len;

    res = clone_obj(as_list(parts)[0]);
    xo = as_i64(res);

    for (i = 1; i < l; i++)
    {
        xi = as_i64(as_list(parts)[i]);
        for (j = 0; j < n; j++)
            xo[j] += xi[j];
    }

    drop_obj(parts);

    return res;
}

obj_p aggr_avg(obj_p val, obj_p index)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *ci, *ids;
    f64_t *xf, *fo;
    obj_p res, sums, cnts;

    switch (val->type)
    {
    case TYPE_I64:
        sums = aggr_sum(val, index);
        cnts = aggr_count(val, index);

        xi = as_i64(sums);
        ci = as_i64(cnts);

        l = sums->len;
        res = vector_f64(l);
        fo = as_f64(res);

        for (i = 0; i < l; i++)
            fo[i] = fdivi64(xi[i], ci[i]);

        drop_obj(sums);
        drop_obj(cnts);

        return res;
    // case TYPE_F64:
    //     xf = as_f64(val);
    //     xm = as_i64(as_list(index)[1]);
    //     res = vector_f64(n);
    //     fo = as_f64(res);
    //     memset(fo, 0, n * sizeof(i64_t));
    //     if (filter != NULL_OBJ)
    //     {
    //         ids = as_i64(filter);
    //         for (i = 0; i < l; i++)
    //             fo[xm[i]] = addf64(fo[xm[i]], xf[ids[i]]);
    //     }
    //     else
    //     {
    //         for (i = 0; i < l; i++)
    //             fo[xm[i]] = addf64(fo[xm[i]], xf[i]);
    //     }

    //     // calc avgs
    //     for (i = 0; i < n; i++)
    //         fo[i] = fdivf64(fo[i], ci[i]);

    //     return res;
    default:
        return error(ERR_TYPE, "avg: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_med(obj_p val, obj_p index)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *ids;
    f64_t *xf, *fo;
    obj_p res;

    // n = as_list(bins)[0]->i64;
    // l = as_list(bins)[1]->len;

    // switch (val->type)
    // {
    // case TYPE_I64:
    //     xi = as_i64(val);
    //     xm = as_i64(as_list(bins)[1]);
    //     res = vector_f64(n);
    //     fo = as_f64(res);
    //     memset(fo, 0, n * sizeof(i64_t));
    //     if (filter != NULL_OBJ)
    //     {
    //         ids = as_i64(filter);
    //         for (i = 0; i < l; i++)
    //             fo[xm[i]] = addi64(fo[xm[i]], xi[ids[i]]);
    //     }
    //     else
    //     {
    //         for (i = 0; i < l; i++)
    //             fo[xm[i]] = addi64(fo[xm[i]], xi[i]);
    //     }

    //     // calc medians
    //     for (i = 0; i < n; i++)
    //         fo[i] = divi64(fo[i], 2);

    //     return res;
    // case TYPE_F64:
    //     xf = as_f64(val);
    //     xm = as_i64(as_list(bins)[1]);
    //     res = vector_f64(n);
    //     fo = as_f64(res);
    //     memset(fo, 0, n * sizeof(i64_t));
    //     if (filter != NULL_OBJ)
    //     {
    //         ids = as_i64(filter);
    //         for (i = 0; i < l; i++)
    //             fo[xm[i]] = addf64(fo[xm[i]], xf[ids[i]]);
    //     }
    //     else
    //     {
    //         for (i = 0; i < l; i++)
    //             fo[xm[i]] = addf64(fo[xm[i]], xf[i]);
    //     }

    //     // calc medians
    //     for (i = 0; i < n; i++)
    //         fo[i] = fdivf64(fo[i], 2);

    //     return res;
    // default:
    return error(ERR_TYPE, "median: unsupported type: '%s'", type_name(val->type));
    // }
}

obj_p aggr_dev(obj_p val, obj_p index)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *ids;
    f64_t *xf, *fo;
    obj_p res;

    // n = as_list(bins)[0]->i64;
    // l = as_list(bins)[1]->len;

    // switch (val->type)
    // {
    // case TYPE_I64:
    //     xi = as_i64(val);
    //     xm = as_i64(as_list(bins)[1]);
    //     res = vector_f64(n);
    //     fo = as_f64(res);
    //     memset(fo, 0, n * sizeof(i64_t));
    //     if (filter != NULL_OBJ)
    //     {
    //         ids = as_i64(filter);
    //         for (i = 0; i < l; i++)
    //             fo[xm[i]] = addi64(fo[xm[i]], xi[ids[i]]);
    //     }
    //     else
    //     {
    //         for (i = 0; i < l; i++)
    //             fo[xm[i]] = addi64(fo[xm[i]], xi[i]);
    //     }

    //     // calc stddev
    //     for (i = 0; i < n; i++)
    //         fo[i] = sqrt(fo[i]);

    //     return res;
    // case TYPE_F64:
    //     xf = as_f64(val);
    //     xm = as_i64(as_list(bins)[1]);
    //     res = vector_f64(n);
    //     fo = as_f64(res);
    //     memset(fo, 0, n * sizeof(i64_t));
    //     if (filter != NULL_OBJ)
    //     {
    //         ids = as_i64(filter);
    //         for (i = 0; i < l; i++)
    //             fo[xm[i]] = addf64(fo[xm[i]], xf[ids[i]]);
    //     }
    //     else
    //     {
    //         for (i = 0; i < l; i++)
    //             fo[xm[i]] = addf64(fo[xm[i]], xf[i]);
    //     }

    //     // calc stddev
    //     for (i = 0; i < n; i++)
    //         fo[i] = sqrt(fo[i]);

    //     return res;
    // default:
    return error(ERR_TYPE, "stddev: unsupported type: '%s'", type_name(val->type));
    // }
}
