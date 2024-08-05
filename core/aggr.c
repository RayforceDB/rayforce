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

#define aggr_iter(index, len, offset, aggr)             \
    do                                                  \
    {                                                   \
        u64_t $i, $x, $y;                               \
        i64_t *group_ids, *source, *filter, shift;      \
        group_ids = as_i64(as_list(index)[1]);          \
                                                        \
        if (as_list(index)[3] != NULL_OBJ)              \
        {                                               \
            source = as_i64(as_list(index)[3]);         \
            shift = as_list(index)[2]->i64;             \
            if (as_list(index)[4] != NULL_OBJ)          \
            {                                           \
                filter = as_i64(as_list(index)[4]);     \
                for ($i = 0; $i < len; $i++)            \
                {                                       \
                    $x = filter[$i + offset];           \
                    $y = group_ids[source[$x] - shift]; \
                    aggr;                               \
                }                                       \
            }                                           \
            else                                        \
            {                                           \
                for ($i = 0; $i < len; $i++)            \
                {                                       \
                    $x = $i + offset;                   \
                    $y = group_ids[source[$x] - shift]; \
                    aggr;                               \
                }                                       \
            }                                           \
        }                                               \
        else                                            \
        {                                               \
            if (as_list(index)[4] != NULL_OBJ)          \
            {                                           \
                filter = as_i64(as_list(index)[4]);     \
                for ($i = 0; $i < len; $i++)            \
                {                                       \
                    $x = filter[$i + offset];           \
                    $y = group_ids[$i + offset];        \
                    aggr;                               \
                }                                       \
            }                                           \
            else                                        \
            {                                           \
                for ($i = 0; $i < len; $i++)            \
                {                                       \
                    $x = $i + offset;                   \
                    $y = group_ids[$x];                 \
                    aggr;                               \
                }                                       \
            }                                           \
        }                                               \
    } while (0)

obj_p aggr_map(raw_p aggr, obj_p val, obj_p index)
{
    pool_p pool = runtime_get()->pool;
    u64_t i, l, n, group_count, group_len, chunk;
    obj_p res;
    raw_p argv[6];

    group_count = index_group_count(index);
    group_len = index_group_len(index);
    n = pool_split_by(pool, group_len, group_count);

    if (n == 1)
    {
        res = vector(val->type, group_count);
        argv[0] = (raw_p)group_len;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        argv[4] = res;
        pool_call_task_fn(aggr, 5, argv);

        return vn_list(1, res);
    }

    pool_prepare(pool);

    l = group_len;
    chunk = l / n;

    for (i = 0; i < n - 1; i++)
        pool_add_task(pool, aggr, 5, chunk, i * chunk, val, index, vector(val->type, group_count));

    pool_add_task(pool, aggr, 5, l - i * chunk, i * chunk, val, index, vector(val->type, group_count));

    return pool_run(pool);
}

obj_p aggr_sum_partial(u64_t len, u64_t offset, obj_p val, obj_p index, obj_p res)
{
    u64_t i, n;
    i64_t *xi, *xm, *xk, *xo;
    f64_t *xf, *fo;

    n = index_group_count(index);

    switch (val->type)
    {
    case TYPE_I64:
        xi = as_i64(val);
        xo = as_i64(res);
        memset(xo, 0, n * sizeof(i64_t));
        aggr_iter(index, len, offset, xo[$y] = addi64(xo[$y], xi[$x]));
        return res;
    case TYPE_F64:
        xf = as_f64(val);
        fo = as_f64(res);
        memset(fo, 0, n * sizeof(i64_t));
        aggr_iter(index, len, offset, fo[$y] = addf64(fo[$y], xf[$x]));
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

    // parts = aggr_map(aggr_sum_partial, val, index);
    // unwrap_list(parts);
    n = index_group_count(index);
    l = index_group_len(index);
    // l = parts->len;
    // res = clone_obj(as_list(parts)[0]);

    switch (val->type)
    {
    case TYPE_I64:
        res = vector(val->type, n);
        xi = as_i64(val);
        xo = as_i64(res);
        memset(xo, 0, n * sizeof(i64_t));

        aggr_iter(index, l, 0, xo[$y] = addi64(xo[$y], xi[$x]));

        return res;

        // xo = as_i64(res);
        // for (i = 1; i < l; i++)
        // {
        //     xi = as_i64(as_list(parts)[i]);
        //     for (j = 0; j < n; j++)
        //         xo[j] = addi64(xo[j], xi[j]);
        // }
        // drop_obj(parts);
        return res;
    case TYPE_F64:
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
        drop_obj(res);
        drop_obj(parts);
        return error(ERR_TYPE, "sum: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_first(obj_p val, obj_p index)
{
    u64_t i, j, xl, l, n;
    i64_t k, *xi, *xo, *xe, *group_ids, shift;
    f64_t *fo, *fi;
    guid_t *gi, *go;
    obj_p res, *oi, *oo, ek, sym;

    n = index_group_count(index);
    l = index_group_len(index);

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_ENUM:
        res = vector(val->type, n);

        xi = as_i64(val);
        xo = as_i64(res);

        for (i = 0; i < n; i++)
            xo[i] = NULL_I64;

        aggr_iter(index, l, 0, if (xo[$y] == NULL_I64) xo[$y] = xi[$x]);

        if (val->type == TYPE_ENUM)
        {
            ek = ray_key(val);
            sym = ray_get(ek);
            drop_obj(ek);

            if (is_error(sym))
            {
                drop_obj(res);
                return sym;
            }

            if (is_null(sym) || sym->type != TYPE_SYMBOL)
            {
                drop_obj(sym);
                drop_obj(res);
                return error(ERR_TYPE, "first: can not resolve an enum");
            }

            xe = as_symbol(sym);

            for (i = 0; i < n; i++)
                xo[i] = xe[xo[i]];

            drop_obj(sym);
        }

        return res;

    case TYPE_F64:
        res = vector_f64(n);

        fi = as_f64(val);
        fo = as_f64(res);

        for (i = 0; i < n; i++)
            fo[i] = NULL_F64;

        aggr_iter(index, l, 0, if (ops_is_nan(fo[$y])) fo[$y] = fi[$x]);

        return res;

    case TYPE_GUID:
        res = vector_guid(n);

        gi = as_guid(val);
        go = as_guid(res);

        for (i = 0; i < n; i++)
            memcpy(go[i], NULL_GUID, sizeof(guid_t));

        aggr_iter(index, l, 0, if (memcmp(go[$y], NULL_GUID, sizeof(guid_t)) == 0) memcpy(go[$y], gi[$x], sizeof(guid_t)));

        return res;

        // case TYPE_LIST:
        //     xo = as_list(val);
        //     yo = as_list(res);

        //     for (i = 0; i < n; i++)
        //         yo[i] = NULL_OBJ;

        //     aggr_iter(index, len, offset, if (yo[$y] == NULL_OBJ) yo[$y] = clone_obj(xo[$x]));

        //     return res;

    default:
        return error(ERR_TYPE, "first: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_last(obj_p val, obj_p index)
{
    u64_t i, j, l, n;
    i64_t *xi, *yi;
    f64_t *xf, *yf;
    guid_t *xg, *yg;
    obj_p res, *xo, *yo;

    n = index_group_count(index);

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        xi = as_i64(val);
        res = vector(val->type, n);
        yi = as_i64(res);

        for (i = 0; i < n; i++)
            yi[i] = xi[i];

        return res;
    case TYPE_F64:
        xf = as_f64(val);
        res = vector(val->type, n);
        yf = as_f64(res);

        for (i = 0; i < n; i++)
            yf[i] = xf[i];

        return res;
    case TYPE_GUID:
        xg = as_guid(val);
        res = vector(val->type, n);
        yg = as_guid(res);

        for (i = 0; i < n; i++)
            memcpy(yg[i], xg[i], sizeof(guid_t));

        return res;
    case TYPE_LIST:
        xo = as_list(val);
        res = vector(val->type, n);
        yo = as_list(res);

        for (i = 0; i < n; i++)
            yo[i] = NULL_OBJ;

        for (i = 0; i < n; i++)
        {
            drop_obj(yo[i]);
            yo[i] = clone_obj(xo[i]);
        }

        return res;
    default:
        return error(ERR_TYPE, "last: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_max_partial(u64_t len, u64_t offset, obj_p val, obj_p index, obj_p res)
{
    u64_t i, n;
    i64_t *xi, *yi;
    f64_t *xf, *yf;
    obj_p *xo, *yo;
    guid_t *xg, *yg;

    n = index_group_count(index);

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_ENUM:
    case TYPE_TIMESTAMP:
        xi = as_i64(val);
        yi = as_i64(res);

        for (i = 0; i < n; i++)
            yi[i] = NULL_I64;

        aggr_iter(index, len, offset, yi[$y] = maxi64(yi[$y], xi[$x]));

        return res;
    case TYPE_F64:
        xf = as_f64(val);
        yf = as_f64(res);

        for (i = 0; i < n; i++)
            yf[i] = NULL_F64;

        aggr_iter(index, len, offset, yf[$y] = maxf64(yf[$y], xf[$x]));

        return res;
    case TYPE_GUID:
        xg = as_guid(val);
        yg = as_guid(res);

        memset(yg, 0, n * sizeof(guid_t));

        aggr_iter(index, len, offset,
                  if (memcmp(xg[$y], yg[$x], sizeof(guid_t)) > 0)
                      memcpy(yg[$y], xg[$x], sizeof(guid_t)));

        return res;
    case TYPE_LIST:
        xo = as_list(val);
        yo = as_list(res);

        for (i = 0; i < n; i++)
            yo[i] = NULL_OBJ;

        aggr_iter(index, len, offset, if (ray_gt(xo[$x], yo[$y])) {
            drop_obj(yo[$y]);
            yo[$y] = clone_obj(xo[$x]); });

        return res;
    default:
        drop_obj(res);
        return error(ERR_TYPE, "max: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_max(obj_p val, obj_p index)
{
    u64_t i, j, l, n;
    i64_t *xi, *yi;
    f64_t *xf, *yf;
    guid_t *xg, *yg;
    obj_p res, parts, *xo, *yo;

    parts = aggr_map(aggr_max_partial, val, index);
    unwrap_list(parts);
    n = index_group_count(index);
    l = parts->len;
    res = clone_obj(as_list(parts)[0]);

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        yi = as_i64(res);
        for (i = 1; i < l; i++)
        {
            xi = as_i64(as_list(parts)[i]);
            for (j = 0; j < n; j++)
                yi[j] = maxi64(yi[j], xi[j]);
        }

        drop_obj(parts);
        return res;
    case TYPE_F64:
        yf = as_f64(res);
        for (i = 1; i < l; i++)
        {
            xf = as_f64(as_list(parts)[i]);
            for (j = 0; j < n; j++)
                yf[j] = maxf64(yf[j], xf[j]);
        }
        drop_obj(parts);
        return res;
    case TYPE_GUID:
        yg = as_guid(res);
        for (i = 1; i < l; i++)
        {
            xg = as_guid(as_list(parts)[i]);
            for (j = 0; j < n; j++)
                if (memcmp(xg[j], yg[j], sizeof(guid_t)) > 0)
                    memcpy(yg[j], xg[j], sizeof(guid_t));
        }
        drop_obj(parts);
        return res;
    case TYPE_LIST:
        yo = as_list(res);
        for (i = 1; i < l; i++)
        {
            xo = as_list(as_list(parts)[i]);
            for (j = 0; j < n; j++)
                if (ray_gt(xo[j], yo[j]))
                {
                    drop_obj(yo[j]);
                    yo[j] = clone_obj(xo[j]);
                }
        }
        drop_obj(parts);
        return res;
    default:
        drop_obj(res);
        drop_obj(parts);
        return error(ERR_TYPE, "max: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_min_partial(u64_t len, u64_t offset, obj_p val, obj_p index, obj_p res)
{
    u64_t i, n;
    i64_t *xi, *yi;
    f64_t *xf, *yf;
    obj_p *xo, *yo;
    guid_t *xg, *yg;

    n = index_group_count(index);

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_ENUM:
    case TYPE_TIMESTAMP:
        xi = as_i64(val);
        yi = as_i64(res);

        for (i = 0; i < n; i++)
            yi[i] = NULL_I64;

        aggr_iter(index, len, offset, yi[$y] = mini64(yi[$y], xi[$x]));

        return res;
    case TYPE_F64:
        xf = as_f64(val);
        yf = as_f64(res);

        for (i = 0; i < n; i++)
            yf[i] = NULL_F64;

        aggr_iter(index, len, offset, yf[$y] = minf64(yf[$y], xf[$x]));

        return res;
    case TYPE_GUID:
        xg = as_guid(val);
        yg = as_guid(res);

        memset(yg, 0, n * sizeof(guid_t));

        aggr_iter(index, len, offset,
                  if (memcmp(xg[$y], yg[$y], sizeof(guid_t)) < 0)
                      memcpy(yg[$y], xg[$x], sizeof(guid_t)));

        return res;
    case TYPE_LIST:
        xo = as_list(val);
        yo = as_list(res);

        for (i = 0; i < n; i++)
            yo[i] = NULL_OBJ;

        aggr_iter(index, len, offset, if (ray_lt(xo[$x], yo[$y])) {
            drop_obj(yo[$y]);
            yo[$y] = clone_obj(xo[$x]); });

        return res;
    default:
        drop_obj(res);
        return error(ERR_TYPE, "min: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_min(obj_p val, obj_p index)
{
    u64_t i, j, l, n;
    i64_t *xi, *yi;
    f64_t *xf, *yf;
    guid_t *xg, *yg;
    obj_p res, parts, *xo, *yo;

    parts = aggr_map(aggr_min_partial, val, index);
    unwrap_list(parts);
    n = index_group_count(index);
    l = parts->len;
    res = clone_obj(as_list(parts)[0]);

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        yi = as_i64(res);
        for (i = 1; i < l; i++)
        {
            xi = as_i64(as_list(parts)[i]);
            for (j = 0; j < n; j++)
                yi[j] = mini64(yi[j], xi[j]);
        }

        drop_obj(parts);
        return res;
    case TYPE_F64:
        yf = as_f64(res);
        for (i = 1; i < l; i++)
        {
            xf = as_f64(as_list(parts)[i]);
            for (j = 0; j < n; j++)
                yf[j] = minf64(yf[j], xf[j]);
        }
        drop_obj(parts);
        return res;
    case TYPE_GUID:
        yg = as_guid(res);
        for (i = 1; i < l; i++)
        {
            xg = as_guid(as_list(parts)[i]);
            for (j = 0; j < n; j++)
                if (memcmp(xg[j], yg[j], sizeof(guid_t)) < 0)
                    memcpy(yg[j], xg[j], sizeof(guid_t));
        }
        drop_obj(parts);
        return res;
    case TYPE_LIST:
        yo = as_list(res);
        for (i = 1; i < l; i++)
        {
            xo = as_list(as_list(parts)[i]);
            for (j = 0; j < n; j++)
                if (ray_lt(xo[j], yo[j]))
                {
                    drop_obj(yo[j]);
                    yo[j] = clone_obj(xo[j]);
                }
        }
        drop_obj(parts);
        return res;
    default:
        drop_obj(res);
        drop_obj(parts);
        return error(ERR_TYPE, "min: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_count_partial(u64_t len, u64_t offset, obj_p val, obj_p index, obj_p res)
{
    unused(val);
    u64_t i, n;
    i64_t *yi;

    n = index_group_count(index);
    yi = as_i64(res);

    for (i = 0; i < n; i++)
        yi[i] = 0;

    aggr_iter(index, len, offset, yi[$y]++);

    return res;
}

obj_p aggr_count(obj_p val, obj_p index)
{
    u64_t i, j, l, n;
    i64_t *xi, *xo;
    obj_p res, parts;

    parts = aggr_map(aggr_count_partial, val, index);
    unwrap_list(parts);
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
    case TYPE_F64:
        sums = aggr_sum(val, index);
        cnts = aggr_count(val, index);

        xf = as_f64(sums);
        ci = as_i64(cnts);

        l = sums->len;
        res = vector_f64(l);
        fo = as_f64(res);

        for (i = 0; i < l; i++)
            fo[i] = fdivf64(xf[i], ci[i]);

        drop_obj(sums);
        drop_obj(cnts);

        return res;

    default:
        return error(ERR_TYPE, "avg: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_med_partial(u64_t len, u64_t offset, obj_p val, obj_p index, obj_p res)
{
    unused(val);
    u64_t i, n;
    i64_t *xi, *xm, *ids;
    f64_t *xf, *fo;

    n = index_group_count(index);

    switch (val->type)
    {
    case TYPE_I64:
        xi = as_i64(val);
        xm = as_i64(as_list(index)[1]);
        fo = as_f64(res);
        memset(fo, 0, n * sizeof(i64_t));
        aggr_iter(index, len, offset, fo[$y] = addi64(fo[$y], xi[$x]));
        for (i = 0; i < n; i++)
            fo[i] = divi64(fo[i], 2);
        return res;
    case TYPE_F64:
        xf = as_f64(val);
        xm = as_i64(as_list(index)[1]);
        fo = as_f64(res);
        memset(fo, 0, n * sizeof(i64_t));
        aggr_iter(index, len, offset, fo[$y] = addf64(fo[$y], xf[$x]));
        for (i = 0; i < n; i++)
            fo[i] = fdivf64(fo[i], 2);
        return res;
    default:
        drop_obj(res);
        return error(ERR_TYPE, "median: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_med(obj_p val, obj_p index)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *ids;
    f64_t *xf, *fo;
    obj_p res;

    switch (val->type)
    {
    // case TYPE_I64:
    //     res = vector_f64(n);
    //     fo = as_f64(res);
    //     memset(fo, 0, n * sizeof(i64_t));
    //     aggr_iter(index, len, offset, fo[$y] = addi64(fo[$y], xi[$x]));
    //     for (i = 0; i < n; i++)
    //         fo[i] = divi64(fo[i], 2);
    //     return res;
    // case TYPE_F64:
    //     res = vector_f64(n);
    //     fo = as_f64(res);
    //     memset(fo, 0, n * sizeof(i64_t));
    //     aggr_iter(index, len, offset, fo[$y] = addf64(fo[$y], xf[$x]));
    //     for (i = 0; i < n; i++)
    //         fo[i] = fdivf64(fo[i], 2);
    //     return res;
    default:
        return error(ERR_TYPE, "median: unsupported type: '%s'", type_name(val->type));
    }
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

obj_p aggr_collect(obj_p val, obj_p index)
{
    u64_t i, l, m, n;
    i64_t *cnts;
    obj_p cnt, res;

    cnt = aggr_count(val, index);
    if (is_error(cnt))
        return cnt;

    cnts = as_i64(cnt);
    n = cnt->len;
    l = index_group_len(index);

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        res = list(n);

        // alloc vectors for each group
        for (i = 0; i < n; i++)
        {
            m = cnts[i];
            as_list(res)[i] = vector(val->type, m);
            as_list(res)[i]->len = 0;
        }

        // fill vectors with values
        aggr_iter(index, l, 0, as_i64(as_list(res)[$y])[as_list(res)[$y]->len++] = as_i64(val)[$x]);

        drop_obj(cnt);

        return res;
        // case TYPE_F64:
        //     res = list(n);
        //     for (i = 0; i < n; i++)
        //     {
        //         m = cnts[i];
        //         as_list(res)[i] = vector_f64(m);
        //         as_list(res)[i]->len = 0;
        //     }

        //     if (filters)
        //     {
        //         for (i = 0; i < l; i++)
        //         {
        //             n = as_i64(bins)[i];
        //             as_f64(as_list(res)[n])[as_list(res)[n]->len++] = as_f64(obj)[filters[i]];
        //         }

        //         return res;
        //     }

        //     for (i = 0; i < l; i++)
        //     {
        //         n = as_i64(bins)[i];
        //         as_f64(as_list(res)[n])[as_list(res)[n]->len++] = as_f64(obj)[i];
        //     }

        //     return res;
        // case TYPE_ENUM:
        //     k = ray_key(obj);
        //     if (is_error(k))
        //         return k;

        //     v = ray_get(k);
        //     drop_obj(k);

        //     if (is_error(v))
        //         return v;

        //     if (v->type != TYPE_SYMBOL)
        //         return error(ERR_TYPE, "enum: '%s' is not a 'Symbol'", type_name(v->type));

        //     res = list(n);
        //     for (i = 0; i < n; i++)
        //     {
        //         m = cnts[i];
        //         as_list(res)[i] = vector_symbol(m);
        //         as_list(res)[i]->len = 0;
        //     }

        //     if (filters)
        //     {
        //         for (i = 0; i < l; i++)
        //         {
        //             n = as_i64(bins)[i];
        //             as_i64(as_list(res)[n])[as_list(res)[n]->len++] = as_i64(v)[as_i64(enum_val(obj))[filters[i]]];
        //         }

        //         drop_obj(v);

        //         return res;
        //     }

        //     for (i = 0; i < l; i++)
        //     {
        //         n = as_i64(bins)[i];
        //         as_i64(as_list(res)[n])[as_list(res)[n]->len++] = as_i64(v)[as_i64(enum_val(obj))[i]];
        //     }

        //     drop_obj(v);

        //     return res;
        // case TYPE_LIST:
        //     res = list(n);
        //     for (i = 0; i < n; i++)
        //     {
        //         m = cnts[i];
        //         as_list(res)[i] = list(m);
        //         as_list(res)[i]->len = 0;
        //     }

        //     if (filters)
        //     {
        //         for (i = 0; i < l; i++)
        //         {
        //             n = as_i64(bins)[i];
        //             as_list(as_list(res)[n])[as_list(res)[n]->len++] = clone_obj(as_list(obj)[filters[i]]);
        //         }

        //         return res;
        //     }

        //     for (i = 0; i < l; i++)
        //     {
        //         n = as_i64(bins)[i];
        //         as_list(as_list(res)[n])[as_list(res)[n]->len++] = clone_obj(as_list(obj)[i]);
        //     }

        //     return res;
        // case TYPE_ANYMAP:
        //     res = list(n);
        //     for (i = 0; i < n; i++)
        //     {
        //         m = cnts[i];
        //         as_list(res)[i] = list(m);
        //         as_list(res)[i]->len = 0;
        //     }

        //     if (filters)
        //     {
        //         for (i = 0; i < l; i++)
        //         {
        //             n = as_i64(bins)[i];
        //             as_list(as_list(res)[n])[as_list(res)[n]->len++] = at_idx(obj, filters[i]);
        //         }

        //         return res;
        //     }

        //     for (i = 0; i < l; i++)
        //     {
        //         n = as_i64(bins)[i];
        //         as_list(as_list(res)[n])[as_list(res)[n]->len++] = at_idx(obj, i);
        //     }

        //     return res;

    default:
        drop_obj(cnt);
        throw(ERR_TYPE, "aggr collect: unsupported type: '%s", type_name(val->type));
    }
}

obj_p aggr_indices(obj_p val, obj_p index)
{
    u64_t i, l, m, n;
    i64_t *cnts;
    obj_p cnt, res;

    cnt = aggr_count(val, index);
    if (is_error(cnt))
        return cnt;

    cnts = as_i64(cnt);
    n = cnt->len;
    l = ops_count(val);

    res = list(n);

    // alloc vectors for each group
    for (i = 0; i < n; i++)
    {
        m = cnts[i];
        as_list(res)[i] = vector_i64(m);
        as_list(res)[i]->len = 0;
    }

    // fill vectors with indices
    aggr_iter(index, l, 0, as_i64(as_list(res)[$y])[as_list(res)[$y]->len++] = $x);

    drop_obj(cnt);

    return res;
}
