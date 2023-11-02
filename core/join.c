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

typedef struct lj_ctx_t
{
    obj_t cols;
    u64_t *hashes;
} lj_ctx_t;

obj_t select_column(obj_t left_col, obj_t right_col, i64_t ids[], u64_t len)
{
    u64_t i;
    obj_t v, res;
    i64_t idx;
    type_t type;

    // there is no such column in the right table
    if (is_null(right_col))
        return clone(left_col);

    type = is_null(left_col) ? right_col->type : left_col->type;

    if (right_col->type != type)
        emit(ERR_TYPE, "join_column: incompatible types");

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

inline __attribute__((always_inline)) bool_t __eq(obj_t a, i64_t ai, obj_t b, i64_t bi)
{
    switch (a->type)
    {
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        return as_i64(a)[ai] == as_i64(b)[bi];
    case TYPE_F64:
        return as_f64(a)[ai] == as_f64(b)[bi];
    case TYPE_CHAR:
        return as_string(a)[ai] == as_string(b)[bi];
    case TYPE_GUID:
        return memcmp(as_guid(a) + ai, as_guid(b) + bi, sizeof(guid_t)) == 0;
    case TYPE_LIST:
        return objcmp(as_list(a)[ai], as_list(b)[bi]) == 0;
    default:
        return false;
    }
}

inline __attribute__((always_inline)) u64_t hashi64(u64_t h, u64_t k)
{
    u64_t a, b;

    a = (h ^ k) * 0x9ddfea08eb382d69ull;
    a ^= (a >> 47);
    b = (roti64(k, 31) ^ a) * 0x9ddfea08eb382d69ull;
    b ^= (b >> 47);
    b *= 0x9ddfea08eb382d69ull;

    return b;
}

nil_t hash_column(obj_t obj, u64_t *out, u64_t len)
{
    u8_t *u8v;
    f64_t *f64v;
    guid_t *g64v;
    u64_t i, *u64v;

    switch (obj->type)
    {
    case TYPE_BOOL:
    case TYPE_BYTE:
    case TYPE_CHAR:
        u8v = as_u8(obj);
        for (i = 0; i < len; i++)
            out[i] = hashi64((u64_t)u8v[i], out[i]);
        break;
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        u64v = (u64_t *)as_i64(obj);
        for (i = 0; i < len; i++)
            out[i] = hashi64(u64v[i], out[i]);
        break;
    case TYPE_F64:
        f64v = as_f64(obj);
        for (i = 0; i < len; i++)
            out[i] = hashi64((u64_t)f64v[i], out[i]);
        break;
    case TYPE_GUID:
        g64v = as_guid(obj);
        for (i = 0; i < len; i++)
        {
            out[i] = hashi64(*(u64_t *)&g64v[i], out[i]);
            out[i] = hashi64(*((u64_t *)&g64v[i] + 1), out[i]);
        }
        break;
    case TYPE_LIST:
        for (i = 0; i < len; i++)
            out[i] = hashi64(ops_hash_obj(as_list(obj)[i]), out[i]);
        break;
    default:
        // TODO: error
        break;
    }

    return;
}

nil_t precalc_hash(obj_t cols, u64_t *out, u64_t ncols, u64_t nrows)
{
    u64_t i, j;

    // initialize hashes
    for (j = 0; j < nrows; j++)
        out[j] = 0xa5b6c7d8e9f01234ull;

    hash_column(as_list(cols)[0], out, nrows);

    for (i = 1; i < ncols; i++)
        hash_column(as_list(cols)[i], out, nrows);

    return;
}

u64_t hash_get(i64_t row, nil_t *seed)
{
    lj_ctx_t *ctx = (lj_ctx_t *)seed;
    return ctx->hashes[row];
}

i32_t cmp_row(i64_t row1, i64_t row2, nil_t *seed)
{
    u64_t i, l;
    lj_ctx_t *ctx = (lj_ctx_t *)seed;
    obj_t *cols = as_list(ctx->cols);

    l = ctx->cols->len;

    for (i = 0; i < l; i++)
        if (!__eq(cols[i], row1, cols[i], row2))
            return 1;

    return 0;
}

obj_t build_idx(obj_t lcols, obj_t rcols, u64_t len)
{
    u64_t i, ll, rl;
    obj_t ht, res;
    i64_t idx;
    lj_ctx_t ctx;

    if (len == 1)
        return ray_call_binary(0, ray_find, rcols, lcols);

    ll = as_list(lcols)[0]->len;
    rl = as_list(rcols)[0]->len;
    ht = ht_tab(maxi64(ll, rl) * 2, -1);
    res = vector_i64(maxi64(ll, rl));

    // Right hashes
    precalc_hash(rcols, (u64_t *)as_i64(res), len, rl);
    ctx = (lj_ctx_t){rcols, (u64_t *)as_i64(res)};
    for (i = 0; i < rl; i++)
    {
        idx = ht_tab_next_with(&ht, i, &hash_get, &cmp_row, &ctx);
        if (as_i64(as_list(ht)[0])[idx] == NULL_I64)
            as_i64(as_list(ht)[0])[idx] = i;
    }

    // Left hashes
    precalc_hash(lcols, (u64_t *)as_i64(res), len, ll);
    ctx = (lj_ctx_t){lcols, (u64_t *)as_i64(res)};
    for (i = 0; i < ll; i++)
    {
        idx = ht_tab_get_with(ht, i, &hash_get, &cmp_row, &ctx);
        as_i64(res)[i] = (idx == NULL_I64) ? NULL_I64 : as_i64(as_list(ht)[0])[idx];
    }

    drop(ht);

    return res;
}

obj_t ray_lj(obj_t *x, u64_t n)
{
    u64_t ll;
    i64_t i, j, l;
    obj_t k1, k2, c1, c2, un, col, cols, vals, idx, rescols, resvals;

    if (n != 3)
        emit(ERR_LENGTH, "lj");

    if (x[0]->type != TYPE_SYMBOL)
        emit(ERR_TYPE, "lj: first argument must be a symbol vector");

    if (x[1]->type != TYPE_TABLE)
        emit(ERR_TYPE, "lj: second argument must be a table");

    if (x[2]->type != TYPE_TABLE)
        emit(ERR_TYPE, "lj: third argument must be a table");

    if (ops_count(x[1]) == 0 || ops_count(x[2]) == 0)
        return clone(x[1]);

    ll = ops_count(x[1]);

    k1 = ray_at(x[1], x[0]);
    if (is_error(k1))
        return k1;

    k2 = ray_at(x[2], x[0]);
    if (is_error(k2))
    {
        drop(k1);
        return k2;
    }

    idx = build_idx(k1, k2, x[0]->len);
    drop(k2);

    if (is_error(idx))
    {
        drop(k1);
        return idx;
    }

    un = ray_union(as_list(x[1])[0], as_list(x[2])[0]);
    if (is_error(un))
    {
        drop(k1);
        return un;
    }

    // exclude columns that we are joining on
    cols = ray_except(un, x[0]);
    drop(un);

    if (is_error(cols))
    {
        dropn(2, k1, idx);
        return cols;
    }

    l = cols->len;

    if (l == 0)
    {
        dropn(3, k1, idx, cols);
        emit(ERR_LENGTH, "lj: no columns to join on");
    }

    // resulting columns
    vals = vector(TYPE_LIST, l);

    // process each column
    for (i = 0; i < l; i++)
    {
        c1 = null(0);
        c2 = null(0);

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
            dropn(4, k1, cols, idx, vals);
            return col;
        }

        as_list(vals)[i] = col;
    }

    drop(idx);
    rescols = ray_concat(x[0], cols);
    drop(cols);
    resvals = ray_concat(k1, vals);
    drop(vals);
    drop(k1);

    return table(rescols, resvals);
}