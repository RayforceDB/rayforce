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

obj_p aggr_sum(obj_p val, obj_p bins, obj_p filter)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *xo, *ids;
    f64_t *xf, *fo;
    obj_p res;

    n = as_list(bins)[0]->i64;
    l = as_list(bins)[1]->len;

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_TIMESTAMP:
    case TYPE_SYMBOL:
        xi = as_i64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector(val->type, n);
        xo = as_i64(res);
        for (i = 0; i < n; i++)
            xo[i] = 0;

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
                xo[xm[i]] = addi64(xo[xm[i]], xi[ids[i]]);
        }
        else
        {
            for (i = 0; i < l; i++)
                xo[xm[i]] = addi64(xo[xm[i]], xi[i]);
        }
        return res;
    case TYPE_F64:
        xf = as_f64(val);

        xm = as_i64(as_list(bins)[1]);
        res = vector_f64(n);
        fo = as_f64(res);

        for (i = 0; i < n; i++)
            fo[i] = 0;

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
                fo[xm[i]] = addf64(fo[xm[i]], xf[ids[i]]);
        }
        else
        {
            for (i = 0; i < l; i++)
                fo[xm[i]] = addf64(fo[xm[i]], xf[i]);
        }
        return res;
    default:
        return error(ERR_TYPE, "sum: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_first(obj_p val, obj_p bins, obj_p filter)
{
    u64_t i, l, n;
    u8_t *xb, *bo, NULL_GUID[16] = {0};
    i64_t *xi, *ei, *xm, *xo, *ids;
    f64_t *xf, *fo;
    obj_p *oi, *oo, k, v, res;
    guid_t *xg, *og;

    n = as_list(bins)[0]->i64;
    l = as_list(bins)[1]->len;

    switch (val->type)
    {
    case TYPE_U8:
    case TYPE_B8:
        xb = as_u8(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector(val->type, n);
        bo = as_u8(res);
        for (i = 0; i < n; i++)
            bo[i] = 0;

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (!bo[n])
                    bo[n] = xb[ids[i]];
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (!bo[n])
                    bo[n] = xb[i];
            }
        }

        return res;
    case TYPE_I64:
    case TYPE_TIMESTAMP:
    case TYPE_SYMBOL:
        xi = as_i64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector(val->type, n);
        xo = as_i64(res);
        for (i = 0; i < n; i++)
            xo[i] = NULL_I64;

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (xo[n] == NULL_I64)
                    xo[n] = xi[ids[i]];
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (xo[n] == NULL_I64)
                    xo[n] = xi[i];
            }
        }

        return res;
    case TYPE_F64:
        xf = as_f64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector_f64(n);
        fo = as_f64(res);
        for (i = 0; i < n; i++)
            fo[i] = NULL_F64;

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (ops_is_nan(fo[n]))
                    fo[n] = xf[ids[i]];
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (ops_is_nan(fo[n]))
                    fo[n] = xf[i];
            }
        }

        return res;
    case TYPE_LIST:
        oi = as_list(val);
        xm = as_i64(as_list(bins)[1]);
        res = list(n);
        xo = as_i64(res);
        for (i = 0; i < n; i++)
            xo[i] = NULL_I64;

        oo = as_list(res);

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (xo[n] == NULL_I64)
                    oo[n] = clone_obj(oi[ids[i]]);
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (xo[n] == NULL_I64)
                    oo[n] = clone_obj(oi[i]);
            }
        }

        return res;

    case TYPE_GUID:
        xg = as_guid(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector_guid(n);
        og = as_guid(res);
        for (i = 0; i < n; i++)
            memcpy(og + i, NULL_GUID, sizeof(guid_t));

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (0 == memcmp(og + n, NULL_GUID, sizeof(guid_t)))
                    og[n] = xg[ids[i]];
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (0 == memcmp(og + n, NULL_GUID, sizeof(guid_t)))
                    og[n] = xg[i];
            }
        }

        return res;

    case TYPE_ENUM:
        k = ray_key(val);
        if (is_error(k))
            return k;

        v = ray_get(k);
        drop_obj(k);

        if (is_error(v))
            return v;

        if (v->type != TYPE_SYMBOL)
            return error(ERR_TYPE, "enum: '%s' is not a 'Symbol'", type_name(v->type));

        xm = as_i64(as_list(bins)[1]);
        res = vector_symbol(n);
        xo = as_i64(res);
        for (i = 0; i < n; i++)
            xo[i] = NULL_I64;

        xi = as_i64(v);
        ei = as_i64(enum_val(val));

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (xo[n] == NULL_I64)
                    xo[n] = xi[ei[ids[i]]];
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (xo[n] == NULL_I64)
                    xo[n] = xi[ei[i]];
            }
        }

        drop_obj(v);

        return res;
    case TYPE_ANYMAP:
        v = ray_value(val);
        res = aggr_first(v, bins, filter);
        drop_obj(v);
        return res;
    default:
        return error(ERR_TYPE, "first: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_last(obj_p val, obj_p bins, obj_p filter)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *xo, *ids;
    obj_p res;

    n = as_list(bins)[0]->i64;
    l = as_list(bins)[1]->len;

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_TIMESTAMP:
    case TYPE_SYMBOL:
        xi = as_i64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector(val->type, n);
        xo = as_i64(res);
        for (i = 0; i < n; i++)
            xo[i] = NULL_I64;

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                xo[n] = xi[ids[i]];
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                xo[n] = xi[i];
            }
        }
        return res;

    default:
        return error(ERR_TYPE, "last: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_avg(obj_p val, obj_p bins, obj_p filter)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *ci, *ids;
    f64_t *xf, *fo;
    obj_p res;

    n = as_list(bins)[0]->i64;
    l = as_list(bins)[1]->len;

    group_fill_counts(bins);

    ci = as_i64(as_list(bins)[2]);

    switch (val->type)
    {
    case TYPE_I64:
        xi = as_i64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector_f64(n);
        fo = as_f64(res);
        memset(fo, 0, n * sizeof(i64_t));
        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
                fo[xm[i]] = addi64(fo[xm[i]], xi[ids[i]]);
        }
        else
        {
            for (i = 0; i < l; i++)
                fo[xm[i]] = addi64(fo[xm[i]], xi[i]);
        }

        // calc avgs
        for (i = 0; i < n; i++)
            fo[i] = divi64(fo[i], ci[i]);

        return res;
    case TYPE_F64:
        xf = as_f64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector_f64(n);
        fo = as_f64(res);
        memset(fo, 0, n * sizeof(i64_t));
        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
                fo[xm[i]] = addf64(fo[xm[i]], xf[ids[i]]);
        }
        else
        {
            for (i = 0; i < l; i++)
                fo[xm[i]] = addf64(fo[xm[i]], xf[i]);
        }

        // calc avgs
        for (i = 0; i < n; i++)
            fo[i] = fdivf64(fo[i], ci[i]);

        return res;
    default:
        return error(ERR_TYPE, "avg: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_max(obj_p val, obj_p bins, obj_p filter)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *xo, *ids;
    f64_t *xf, *fo;
    obj_p res;

    n = as_list(bins)[0]->i64;
    l = as_list(bins)[1]->len;

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_TIMESTAMP:
    case TYPE_SYMBOL:
        xi = as_i64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector(val->type, n);
        xo = as_i64(res);
        for (i = 0; i < n; i++)
            xo[i] = NULL_I64;

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                xo[n] = maxi64(xo[n], xi[ids[i]]);
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                xo[n] = maxi64(xo[n], xi[i]);
            }
        }
        return res;
    case TYPE_F64:
        xf = as_f64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector_f64(n);
        fo = as_f64(res);
        for (i = 0; i < n; i++)
            fo[i] = NULL_F64;

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                fo[n] = maxf64(fo[n], xf[ids[i]]);
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                fo[n] = maxf64(fo[n], xf[i]);
            }
        }
        return res;
    default:
        return error(ERR_TYPE, "max: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_min(obj_p val, obj_p bins, obj_p filter)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *xo, *ids;
    f64_t *xf, *fo;
    obj_p res;

    n = as_list(bins)[0]->i64;
    l = as_list(bins)[1]->len;

    switch (val->type)
    {
    case TYPE_I64:
    case TYPE_TIMESTAMP:
        xi = as_i64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector(val->type, n);
        xo = as_i64(res);
        for (i = 0; i < n; i++)
            xo[i] = NULL_I64;

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                if (xo[n] == NULL_I64 || xi[ids[i]] < xo[n])
                    xo[n] = xi[ids[i]];
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                xo[n] = mini64(xo[n], xi[i]);
            }
        }

        return res;
    case TYPE_F64:
        xf = as_f64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector_f64(n);
        fo = as_f64(res);
        for (i = 0; i < n; i++)
            fo[i] = NULL_F64;

        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                fo[n] = minf64(fo[n], xf[ids[i]]);
            }
        }
        else
        {
            for (i = 0; i < l; i++)
            {
                n = xm[i];
                fo[n] = minf64(fo[n], xf[i]);
            }
        }
        return res;
    default:
        return error(ERR_TYPE, "min: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_count(obj_p val, obj_p bins, obj_p filter)
{
    unused(val);
    unused(filter);
    group_fill_counts(bins);

    return clone_obj(as_list(bins)[2]);
}

obj_p aggr_med(obj_p val, obj_p bins, obj_p filter)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *ids;
    f64_t *xf, *fo;
    obj_p res;

    n = as_list(bins)[0]->i64;
    l = as_list(bins)[1]->len;

    switch (val->type)
    {
    case TYPE_I64:
        xi = as_i64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector_f64(n);
        fo = as_f64(res);
        memset(fo, 0, n * sizeof(i64_t));
        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
                fo[xm[i]] = addi64(fo[xm[i]], xi[ids[i]]);
        }
        else
        {
            for (i = 0; i < l; i++)
                fo[xm[i]] = addi64(fo[xm[i]], xi[i]);
        }

        // calc medians
        for (i = 0; i < n; i++)
            fo[i] = divi64(fo[i], 2);

        return res;
    case TYPE_F64:
        xf = as_f64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector_f64(n);
        fo = as_f64(res);
        memset(fo, 0, n * sizeof(i64_t));
        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
                fo[xm[i]] = addf64(fo[xm[i]], xf[ids[i]]);
        }
        else
        {
            for (i = 0; i < l; i++)
                fo[xm[i]] = addf64(fo[xm[i]], xf[i]);
        }

        // calc medians
        for (i = 0; i < n; i++)
            fo[i] = fdivf64(fo[i], 2);

        return res;
    default:
        return error(ERR_TYPE, "median: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_dev(obj_p val, obj_p bins, obj_p filter)
{
    u64_t i, l, n;
    i64_t *xi, *xm, *ids;
    f64_t *xf, *fo;
    obj_p res;

    n = as_list(bins)[0]->i64;
    l = as_list(bins)[1]->len;

    switch (val->type)
    {
    case TYPE_I64:
        xi = as_i64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector_f64(n);
        fo = as_f64(res);
        memset(fo, 0, n * sizeof(i64_t));
        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
                fo[xm[i]] = addi64(fo[xm[i]], xi[ids[i]]);
        }
        else
        {
            for (i = 0; i < l; i++)
                fo[xm[i]] = addi64(fo[xm[i]], xi[i]);
        }

        // calc stddev
        for (i = 0; i < n; i++)
            fo[i] = sqrt(fo[i]);

        return res;
    case TYPE_F64:
        xf = as_f64(val);
        xm = as_i64(as_list(bins)[1]);
        res = vector_f64(n);
        fo = as_f64(res);
        memset(fo, 0, n * sizeof(i64_t));
        if (filter != NULL_OBJ)
        {
            ids = as_i64(filter);
            for (i = 0; i < l; i++)
                fo[xm[i]] = addf64(fo[xm[i]], xf[ids[i]]);
        }
        else
        {
            for (i = 0; i < l; i++)
                fo[xm[i]] = addf64(fo[xm[i]], xf[i]);
        }

        // calc stddev
        for (i = 0; i < n; i++)
            fo[i] = sqrt(fo[i]);

        return res;
    default:
        return error(ERR_TYPE, "stddev: unsupported type: '%s'", type_name(val->type));
    }
}
