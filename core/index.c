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

#include <limits.h>
#include "index.h"
#include "ops.h"
#include "hash.h"
#include "util.h"
#include "error.h"
#include "items.h"
#include "unary.h"
#include "string.h"

u64_t __hash_get(i64_t row, nil_t *seed)
{
    __find_ctx_t *ctx = (__find_ctx_t *)seed;
    return ctx->hashes[row];
}

i64_t __cmp_obj(i64_t row1, i64_t row2, nil_t *seed)
{
    __find_ctx_t *ctx = (__find_ctx_t *)seed;
    return cmp_obj(((obj_p *)ctx->lobj)[row1], ((obj_p *)ctx->robj)[row2]);
}

i64_t __hash_cmp_guid(i64_t row1, i64_t row2, nil_t *seed)
{
    __find_ctx_t *ctx = (__find_ctx_t *)seed;
    return memcmp((guid_t *)ctx->lobj + row1, (guid_t *)ctx->robj + row2, sizeof(guid_t));
}

u64_t __index_list_hash_get(i64_t row, nil_t *seed)
{
    __index_list_ctx_t *ctx = (__index_list_ctx_t *)seed;
    return ctx->hashes[row];
}

i64_t __index_list_cmp_row(i64_t row1, i64_t row2, nil_t *seed)
{
    u64_t i, l;
    __index_list_ctx_t *ctx = (__index_list_ctx_t *)seed;
    obj_p *lcols = as_list(ctx->lcols);
    obj_p *rcols = as_list(ctx->rcols);

    l = ctx->lcols->len;

    for (i = 0; i < l; i++)
        if (ops_eq_idx(lcols[i], row1, rcols[i], row2) == 0)
            return 1;

    return 0;
}

nil_t __index_list_precalc_hash(obj_p cols, u64_t *out, u64_t ncols, u64_t nrows, i64_t filter[], b8_t deref)
{
    u64_t i;

    for (i = 0; i < nrows; i++)
        out[i] = 0x9ddfea08eb382d69ull;

    for (i = 0; i < ncols; i++)
        index_hash_obj(as_list(cols)[i], out, filter, nrows, deref);
}

index_scope_t index_scope(i64_t values[], i64_t indices[], u64_t len)
{
    u64_t i;
    i64_t min, max;

    if (indices)
    {
        min = max = values[indices[0]];
        for (i = 0; i < len; i++)
        {
            min = values[indices[i]] < min ? values[indices[i]] : min;
            max = values[indices[i]] > max ? values[indices[i]] : max;
        }
    }
    else
    {
        min = max = values[0];
        for (i = 0; i < len; i++)
        {
            min = values[i] < min ? values[i] : min;
            max = values[i] > max ? values[i] : max;
        }
    }

    return (index_scope_t){min, max, max - min + 1};
}

obj_p index_distinct_i8(i8_t values[], u64_t len, b8_t term)
{
    u64_t i, j, range;
    i8_t min, *out;
    obj_p vec;

    min = -128;
    range = 256;

    vec = vector_u8(range);
    out = (i8_t *)as_u8(vec);
    memset(out, 0, range);

    for (i = 0; i < len; i++)
    {
        if (out[values[i] - min] == 0)
            out[values[i] - min] = 1;
    }

    // compact keys
    for (i = 0, j = 0; i < range; i++)
    {
        if (out[i])
            out[j++] = i + min;
    }

    if (term)
        out[j++] = 0;

    resize_obj(&vec, j);
    vec->attrs |= ATTR_DISTINCT;

    return vec;
}

obj_p index_distinct_i64(i64_t values[], u64_t len)
{
    u64_t i, j;
    i64_t p, k, *out;
    obj_p vec, set;
    const index_scope_t scope = index_scope(values, NULL, len);

    // use open addressing if range is small
    if (scope.range <= len)
    {
        vec = vector_i64(scope.range);
        out = as_i64(vec);
        memset(out, 0, sizeof(i64_t) * scope.range);

        for (i = 0; i < len; i++)
            out[values[i] - scope.min]++;

        // compact keys
        for (i = 0, j = 0; i < scope.range; i++)
        {
            if (out[i])
                out[j++] = i + scope.min;
        }

        resize_obj(&vec, j);
        vec->attrs |= ATTR_DISTINCT;

        return vec;
    }

    // otherwise, use a hash table
    set = ht_tab(len, -1);

    for (i = 0; i < len; i++)
    {
        k = values[i] - scope.min;
        p = ht_tab_next(&set, k);
        out = as_i64(as_list(set)[0]);
        if (out[p] == NULL_I64)
            out[p] = k;
    }

    // compact keys
    out = as_i64(as_list(set)[0]);
    len = as_list(set)[0]->len;
    for (i = 0, j = 0; i < len; i++)
    {
        if (out[i] != NULL_I64)
            out[j++] = out[i] + scope.min;
    }

    resize_obj(&as_list(set)[0], j);
    vec = clone_obj(as_list(set)[0]);
    vec->attrs |= ATTR_DISTINCT;
    drop_obj(set);

    return vec;
}

obj_p index_distinct_guid(guid_t values[], u64_t len)
{
    u64_t i, j;
    i64_t p, *out;
    obj_p vec, set;
    guid_t *g;

    set = ht_tab(len, -1);

    for (i = 0, j = 0; i < len; i++)
    {
        p = ht_tab_next_with(&set, (i64_t)&values[i], &hash_guid, &hash_cmp_guid, NULL);
        out = as_i64(as_list(set)[0]);
        if (out[p] == NULL_I64)
        {
            out[p] = (i64_t)&values[i];
            j++;
        }
    }

    vec = vector_guid(j);
    g = as_guid(vec);

    out = as_i64(as_list(set)[0]);
    len = as_list(set)[0]->len;
    for (i = 0, j = 0; i < len; i++)
    {
        if (out[i] != NULL_I64)
            memcpy(&g[j++], (guid_t *)out[i], sizeof(guid_t));
    }

    vec->attrs |= ATTR_DISTINCT;
    drop_obj(set);

    return vec;
}

obj_p index_distinct_obj(obj_p values[], u64_t len)
{
    u64_t i, j;
    i64_t p, *out;
    obj_p vec, set;

    set = ht_tab(len, -1);

    for (i = 0; i < len; i++)
    {
        p = ht_tab_next_with(&set, (i64_t)values[i], &hash_obj, &hash_cmp_obj, NULL);
        out = as_i64(as_list(set)[0]);
        if (out[p] == NULL_I64)
            out[p] = (i64_t)clone_obj(values[i]);
    }

    // compact keys
    out = as_i64(as_list(set)[0]);
    len = as_list(set)[0]->len;
    for (i = 0, j = 0; i < len; i++)
    {
        if (out[i] != NULL_I64)
            out[j++] = out[i];
    }

    resize_obj(&as_list(set)[0], j);
    vec = clone_obj(as_list(set)[0]);
    vec->attrs |= ATTR_DISTINCT;
    vec->type = TYPE_LIST;
    drop_obj(set);

    return vec;
}

obj_p index_find_i8(i8_t x[], u64_t xl, i8_t y[], u64_t yl)
{
    u64_t i, range;
    i64_t min, n, *r, *f;
    obj_p vec;

    min = -128;
    range = 256;

    if (xl == 0)
        return vector_i64(0);

    vec = vector_i64(yl + range);
    r = as_i64(vec);
    f = r + yl;

    for (i = 0; i < range; i++)
        f[i] = NULL_I64;

    for (i = 0; i < xl; i++)
    {
        n = x[i] - min;
        f[n] = (f[n] == NULL_I64) ? (i64_t)i : NULL_I64;
    }

    for (i = 0; i < yl; i++)
    {
        n = y[i] - min;
        r[i] = f[n];
    }

    resize_obj(&vec, yl);

    return vec;
}

obj_p index_find_i64(i64_t x[], u64_t xl, i64_t y[], u64_t yl)
{
    u64_t i;
    i64_t n, p, *r, *f;
    obj_p vec, ht;

    if (xl == 0)
        return vector_i64(0);

    const index_scope_t scope = index_scope(x, NULL, xl);

    if (scope.range <= yl)
    {
        vec = vector_i64(yl + scope.range);
        r = as_i64(vec);
        f = r + yl;

        for (i = 0; i < scope.range; i++)
            f[i] = NULL_I64;

        for (i = 0; i < xl; i++)
        {
            n = x[i] - scope.min;
            f[n] = (f[n] == NULL_I64) ? (i64_t)i : NULL_I64;
        }

        for (i = 0; i < yl; i++)
        {
            n = y[i] - scope.min;
            r[i] = (y[i] < scope.min || y[i] > scope.max) ? NULL_I64 : f[n];
        }

        resize_obj(&vec, yl);

        return vec;
    }

    vec = vector_i64(yl);
    r = as_i64(vec);

    // otherwise, use a hash table
    ht = ht_tab(xl, TYPE_I64);

    for (i = 0; i < xl; i++)
    {
        p = ht_tab_next(&ht, x[i] - scope.min);
        if (as_i64(as_list(ht)[0])[p] == NULL_I64)
        {
            as_i64(as_list(ht)[0])[p] = x[i] - scope.min;
            as_i64(as_list(ht)[1])[p] = i;
        }
    }

    for (i = 0; i < yl; i++)
    {
        p = ht_tab_get(ht, y[i] - scope.min);
        r[i] = p == NULL_I64 ? NULL_I64 : as_i64(as_list(ht)[1])[p];
    }

    drop_obj(ht);

    return vec;
}

obj_p index_find_guid(guid_t x[], u64_t xl, guid_t y[], u64_t yl)
{
    u64_t i, *hashes;
    obj_p ht, res;
    i64_t idx;
    __find_ctx_t ctx;

    // calc hashes
    res = vector_i64(maxi64(xl, yl));
    ht = ht_tab(maxi64(xl, yl) * 2, -1);

    hashes = (u64_t *)as_i64(res);

    for (i = 0; i < xl; i++)
        hashes[i] = hash_index_u64(*(u64_t *)(x + i), *((u64_t *)(x + i) + 1));

    ctx = (__find_ctx_t){.lobj = x, .robj = x, .hashes = hashes};
    for (i = 0; i < xl; i++)
    {
        idx = ht_tab_next_with(&ht, i, &__hash_get, &__hash_cmp_guid, &ctx);
        if (as_i64(as_list(ht)[0])[idx] == NULL_I64)
            as_i64(as_list(ht)[0])[idx] = i;
    }

    for (i = 0; i < yl; i++)
        hashes[i] = hash_index_u64(*(u64_t *)(y + i), *((u64_t *)(y + i) + 1));

    ctx = (__find_ctx_t){.lobj = x, .robj = y, .hashes = hashes};
    for (i = 0; i < yl; i++)
    {
        idx = ht_tab_get_with(ht, i, &__hash_get, &__hash_cmp_guid, &ctx);
        as_i64(res)[i] = (idx == NULL_I64) ? NULL_I64 : as_i64(as_list(ht)[0])[idx];
    }

    drop_obj(ht);
    resize_obj(&res, yl);

    return res;
}

obj_p index_find_obj(obj_p x[], u64_t xl, obj_p y[], u64_t yl)
{
    u64_t i, *hashes;
    obj_p ht, res;
    i64_t idx;
    __find_ctx_t ctx;

    // calc hashes
    res = vector_i64(maxi64(xl, yl));
    ht = ht_tab(maxi64(xl, yl) * 2, -1);

    hashes = (u64_t *)as_i64(res);

    for (i = 0; i < xl; i++)
        hashes[i] = hash_index_u64(hash_index_obj(x[i]), 0xa5b6c7d8e9f01234ull);

    ctx = (__find_ctx_t){.lobj = x, .robj = x, .hashes = hashes};
    for (i = 0; i < xl; i++)
    {
        idx = ht_tab_next_with(&ht, i, &__hash_get, &__cmp_obj, &ctx);
        if (as_i64(as_list(ht)[0])[idx] == NULL_I64)
            as_i64(as_list(ht)[0])[idx] = i;
    }

    for (i = 0; i < yl; i++)
        hashes[i] = hash_index_u64(hash_index_obj(y[i]), 0xa5b6c7d8e9f01234ull);

    ctx = (__find_ctx_t){.lobj = x, .robj = y, .hashes = hashes};
    for (i = 0; i < yl; i++)
    {
        idx = ht_tab_get_with(ht, i, &__hash_get, &__cmp_obj, &ctx);
        as_i64(res)[i] = (idx == NULL_I64) ? NULL_I64 : as_i64(as_list(ht)[0])[idx];
    }

    drop_obj(ht);
    resize_obj(&res, yl);

    return res;
}

/*
 * result is a list
 * [0] - groups number
 * [1] - bins
 * [2] - count of each group (will be filled later on demand)
 */

obj_p index_group_i8(i8_t values[], i64_t indices[], u64_t len)
{
    u64_t i, j, n, range;
    i64_t min, *hk, *hv;
    obj_p keys, vals;

    min = -128;
    range = 256;

    keys = vector_i64(range);
    hk = as_i64(keys);

    vals = vector_i64(len);
    hv = as_i64(vals);

    for (i = 0; i < range; i++)
        hk[i] = NULL_I64;

    // distribute bins
    if (indices)
    {
        for (i = 0, j = 0; i < len; i++)
        {
            n = values[indices[i]] - min;
            if (hk[n] == NULL_I64)
                hk[n] = j++;

            hv[i] = hk[n];
        }
    }
    else
    {
        for (i = 0, j = 0; i < len; i++)
        {
            n = values[i] - min;
            if (hk[n] == NULL_I64)
                hk[n] = j++;

            hv[i] = hk[n];
        }
    }

    drop_obj(keys);

    return vn_list(3, i64(j), vals, NULL_OBJ);
}

obj_p index_group_i64_scoped(i64_t values[], i64_t indices[], u64_t len, const index_scope_t scope)
{
    u64_t i, j, n;
    i64_t idx, *hk, *hv, *hp;
    obj_p keys, vals, ht;

    // use open addressing if range is compatible with the input length
    if (scope.range <= len)
    {
        keys = vector_i64(scope.range);
        hk = as_i64(keys);

        vals = vector_i64(len);
        hv = as_i64(vals);

        for (i = 0; i < scope.range; i++)
            hk[i] = NULL_I64;

        // distribute bins
        if (indices)
        {
            for (i = 0, j = 0; i < len; i++)
            {
                n = values[indices[i]] - scope.min;
                if (hk[n] == NULL_I64)
                    hk[n] = j++;

                hv[i] = hk[n];
            }
        }
        else
        {
            for (i = 0, j = 0; i < len; i++)
            {
                n = values[i] - scope.min;
                if (hk[n] == NULL_I64)
                    hk[n] = j++;

                hv[i] = hk[n];
            }
        }

        drop_obj(keys);

        return vn_list(3, i64(j), vals, NULL_OBJ);
    }

    // use hash table if range is large
    ht = ht_tab(len, TYPE_I64);
    vals = vector_i64(len);
    hp = as_i64(vals);

    // distribute bins
    if (indices)
    {
        for (i = 0, j = 0; i < len; i++)
        {
            n = values[indices[i]] - scope.min;
            idx = ht_tab_next_with(&ht, n, &hash_fnv1a, &hash_cmp_i64, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = n;
                hv[idx] = j++;
            }

            hp[i] = hv[idx];
        }
    }
    else
    {
        for (i = 0, j = 0; i < len; i++)
        {
            n = values[i] - scope.min;
            idx = ht_tab_next_with(&ht, n, &hash_fnv1a, &hash_cmp_i64, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = n;
                hv[idx] = j++;
            }

            hp[i] = hv[idx];
        }
    }

    drop_obj(ht);

    return vn_list(3, i64(j), vals, NULL_OBJ);
}

obj_p index_group_i64(i64_t values[], i64_t indices[], u64_t len)
{
    index_scope_t scope = index_scope(values, indices, len);
    return index_group_i64_scoped(values, indices, len, scope);
}

obj_p index_group_guid(guid_t values[], i64_t indices[], u64_t len)
{
    u64_t i, j;
    i64_t idx, *hk, *hv, *hp;
    obj_p vals, ht;

    ht = ht_tab(len, TYPE_I64);
    vals = vector_i64(len);
    hp = as_i64(vals);

    // distribute bins
    if (indices)
    {
        for (i = 0, j = 0; i < len; i++)
        {
            idx = ht_tab_next_with(&ht, (i64_t)&values[indices[i]], &hash_guid, &hash_cmp_guid, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = (i64_t)&values[indices[i]];
                hv[idx] = j++;
            }

            hp[i] = hv[idx];
        }
    }
    else
    {
        for (i = 0, j = 0; i < len; i++)
        {
            idx = ht_tab_next_with(&ht, (i64_t)&values[i], &hash_guid, &hash_cmp_guid, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = (i64_t)&values[i];
                hv[idx] = j++;
            }

            hp[i] = hv[idx];
        }
    }

    drop_obj(ht);

    return vn_list(3, i64(j), vals, NULL_OBJ);
}

obj_p index_group_obj(obj_p values[], i64_t indices[], u64_t len)
{
    u64_t i, j, n;
    i64_t idx, *hk, *hv, *hp;
    obj_p vals, ht;

    ht = ht_tab(len, TYPE_I64);
    vals = vector_i64(len);
    hp = as_i64(vals);

    // distribute bins
    if (indices)
    {
        for (i = 0, j = 0; i < len; i++)
        {
            n = (i64_t)values[indices[i]];
            idx = ht_tab_next_with(&ht, n, &hash_obj, &hash_cmp_obj, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = n;
                hv[idx] = j++;
            }

            hp[i] = hv[idx];
        }
    }
    else
    {
        for (i = 0, j = 0; i < len; i++)
        {
            n = (i64_t)values[i];
            idx = ht_tab_next_with(&ht, n, &hash_obj, &hash_cmp_obj, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = n;
                hv[idx] = j++;
            }

            hp[i] = hv[idx];
        }
    }

    drop_obj(ht);

    return vn_list(3, i64(j), vals, NULL_OBJ);
}

obj_p index_group_list_direct(obj_p lst, i64_t filter[], u64_t len)
{
    u8_t *xb;
    u64_t i, j, product, l = lst->len, multipliers[l];
    i64_t *xi, *xo;
    obj_p ht, col, res;
    index_scope_t *scopes, scope;

    if (l == 0)
        return NULL_OBJ;

    // First, check if columns types are suitable for direct hashing
    for (i = 0; i < l; i++)
    {
        switch (as_list(lst)[i]->type)
        {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_ENUM:
            break;
        default:
            return NULL_OBJ;
        }
    }

    scopes = (index_scope_t *)heap_alloc(l * sizeof(index_scope_t));

    // calculate scopes of each column to check if we can use direct hashing
    for (i = 0; i < l; i++)
    {
        switch (as_list(lst)[i]->type)
        {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            scopes[i].max = 255;
            scopes[i].min = 0;
            scopes[i].range = 256;
            break;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_ENUM:
            scopes[i] = index_scope(as_i64(as_list(lst)[i]), filter, len);
            break;
        default:
            // because we already checked the types, this should never happen
            __builtin_unreachable();
        }
    }

    // Precompute multipliers for each column
    for (i = 0, product = 1, scope = (index_scope_t){0, 0, 0}; i < l; i++)
    {
        if (ULLONG_MAX / product < scopes[i].range)
        {
            heap_free(scopes);
            return NULL_OBJ; // Overflow would occur
        }

        scope.max += (scopes[i].max - scopes[i].min) * product;

        multipliers[i] = product;
        product *= scopes[i].range;
    }

    scope.range = scope.max + 1;

    ht = vector_i64(len);
    xo = as_i64(ht);
    memset(xo, 0, len * sizeof(i64_t));

    for (i = 0; i < l; i++)
    {
        col = as_list(lst)[i];
        switch (col->type)
        {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            xb = as_u8(col);
            if (filter)
            {
                for (j = 0; j < len; j++)
                    xo[j] += (xb[filter[j]] - scopes[i].min) * multipliers[i];
            }
            else
            {
                for (j = 0; j < len; j++)
                    xo[j] += (xb[j] - scopes[i].min) * multipliers[i];
            }
            break;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            xi = as_i64(col);
            if (filter)
            {
                for (j = 0; j < len; j++)
                    xo[j] += (xi[filter[j]] - scopes[i].min) * multipliers[i];
            }
            else
            {
                for (j = 0; j < len; j++)
                    xo[j] += (xi[j] - scopes[i].min) * multipliers[i];
            }
            break;
        case TYPE_ENUM:
            xi = as_i64(enum_val(col));
            if (filter)
            {
                for (j = 0; j < len; j++)
                    xo[j] += (xi[filter[j]] - scopes[i].min) * multipliers[i];
            }
            else
            {
                for (j = 0; j < len; j++)
                    xo[j] += (xi[j] - scopes[i].min) * multipliers[i];
            }
            break;
        default:
            // because we already checked the types, this should never happen
            __builtin_unreachable();
        }
    }

    heap_free(scopes);
    res = index_group_i64_scoped(xo, NULL, len, scope);
    drop_obj(ht);

    return res;
}

obj_p index_group_list(obj_p lst, i64_t filter[], u64_t len)
{
    u64_t i;
    i64_t g, idx, *hk, *hv, *xo;
    obj_p ht, res;
    __index_list_ctx_t ctx;

    // If the list values are small, use direct hashing
    res = index_group_list_direct(lst, filter, len);
    if (!is_null(res))
        return res;

    // Otherwise use a hash table
    ht = ht_tab(len, TYPE_I64);
    res = vector_i64(len);
    xo = as_i64(res);
    ctx = (__index_list_ctx_t){lst, lst, (u64_t *)as_i64(res)};

    // distribute bins
    __index_list_precalc_hash(lst, (u64_t *)as_i64(res), lst->len, len, filter, B8_FALSE);

    if (filter)
    {
        for (i = 0, g = 0; i < len; i++)
        {
            idx = ht_tab_next_with(&ht, filter[i], &__index_list_hash_get, &__index_list_cmp_row, &ctx);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);

            if (hk[idx] == NULL_I64)
            {
                hk[idx] = filter[i];
                hv[idx] = g++;
            }

            xo[i] = hv[idx];
        }
    }
    else
    {
        for (i = 0, g = 0; i < len; i++)
        {
            idx = ht_tab_next_with(&ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);

            if (hk[idx] == NULL_I64)
            {
                hk[idx] = i;
                hv[idx] = g++;
            }

            xo[i] = hv[idx];
        }
    }

    drop_obj(ht);

    return vn_list(3, i64(g), res, NULL_OBJ);
}

nil_t index_hash_obj(obj_p obj, u64_t out[], i64_t filter[], u64_t len, b8_t deref)
{
    u8_t *u8v;
    f64_t *f64v;
    guid_t *g64v;
    u64_t i, *u64v;
    obj_p k, v;
    i64_t *ids;

    switch (obj->type)
    {
    case -TYPE_B8:
    case -TYPE_U8:
    case -TYPE_C8:
        out[0] = hash_index_u64((u64_t)obj->u8, out[0]);
        break;
    case -TYPE_I64:
    case -TYPE_SYMBOL:
    case -TYPE_TIMESTAMP:
        out[0] = hash_index_u64((u64_t)obj->i64, out[0]);
        break;
    case -TYPE_F64:
        out[0] = hash_index_u64((u64_t)obj->f64, out[0]);
        break;
    case -TYPE_GUID:
        out[0] = hash_index_u64(*(u64_t *)as_guid(obj), *((u64_t *)as_guid(obj) + 1));
        break;
    case TYPE_B8:
    case TYPE_U8:
    case TYPE_C8:
        u8v = as_u8(obj);
        if (filter)
            for (i = 0; i < len; i++)
                out[i] = hash_index_u64((u64_t)u8v[filter[i]], out[i]);
        else
            for (i = 0; i < len; i++)
                out[i] = hash_index_u64((u64_t)u8v[i], out[i]);
        break;
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        u64v = (u64_t *)as_i64(obj);
        if (filter)
            for (i = 0; i < len; i++)
                out[i] = hash_index_u64(u64v[filter[i]], out[i]);
        else
            for (i = 0; i < len; i++)
                out[i] = hash_index_u64(u64v[i], out[i]);
        break;
    case TYPE_F64:
        f64v = as_f64(obj);
        if (filter)
            for (i = 0; i < len; i++)
                out[i] = hash_index_u64((u64_t)f64v[filter[i]], out[i]);
        else
            for (i = 0; i < len; i++)
                out[i] = hash_index_u64((u64_t)f64v[i], out[i]);
        break;
    case TYPE_GUID:
        g64v = as_guid(obj);
        if (filter)
            for (i = 0; i < len; i++)
            {
                out[i] = hash_index_u64(*(u64_t *)&g64v[filter[i]], out[i]);
                out[i] = hash_index_u64(*((u64_t *)&g64v[filter[i]] + 1), out[i]);
            }
        else
            for (i = 0; i < len; i++)
            {
                out[i] = hash_index_u64(*(u64_t *)&g64v[i], out[i]);
                out[i] = hash_index_u64(*((u64_t *)&g64v[i] + 1), out[i]);
            }
        break;
    case TYPE_LIST:
        if (filter)
            for (i = 0; i < len; i++)
                out[i] = hash_index_u64(hash_index_obj(as_list(obj)[filter[i]]), out[i]);
        else
            for (i = 0; i < len; i++)
                out[i] = hash_index_u64(hash_index_obj(as_list(obj)[i]), out[i]);
        break;
    case TYPE_ENUM:
        if (deref)
        {
            k = ray_key(obj);
            v = ray_get(k);
            drop_obj(k);
            u64v = (u64_t *)as_symbol(v);
            ids = as_i64(enum_val(obj));
            if (filter)
                for (i = 0; i < len; i++)
                    out[i] = hash_index_u64(u64v[ids[filter[i]]], out[i]);
            else
                for (i = 0; i < len; i++)
                    out[i] = hash_index_u64(u64v[ids[i]], out[i]);
            drop_obj(v);
        }
        else
        {
            u64v = (u64_t *)as_i64(enum_val(obj));
            if (filter)
                for (i = 0; i < len; i++)
                    out[i] = hash_index_u64(u64v[filter[i]], out[i]);
            else
                for (i = 0; i < len; i++)
                    out[i] = hash_index_u64(u64v[i], out[i]);
        }
        break;
    case TYPE_ANYMAP:
        if (filter)
            for (i = 0; i < len; i++)
            {
                v = at_idx(obj, filter[i]);
                out[i] = hash_index_u64(hash_index_obj(v), out[i]);
                drop_obj(v);
            }
        else
            for (i = 0; i < len; i++)
            {
                v = at_idx(obj, i);
                out[i] = hash_index_u64(hash_index_obj(v), out[i]);
                drop_obj(v);
            }
        break;
    default:
        panic("hash list: unsupported type: %d", obj->type);
    }
}

obj_p index_group_cnts(obj_p grp)
{
    u64_t i, l, n;
    i64_t *grps, *ids;
    obj_p res;

    // Count groups
    n = as_list(grp)[0]->i64;
    l = as_list(grp)[1]->len;
    res = vector_i64(n);
    grps = as_i64(res);
    memset(grps, 0, n * sizeof(i64_t));
    ids = as_i64(as_list(grp)[1]);
    for (i = 0; i < l; i++)
        grps[ids[i]]++;

    return res;
}

obj_p index_join_obj(obj_p lcols, obj_p rcols, u64_t len)
{
    u64_t i, ll, rl;
    obj_p ht, res;
    i64_t idx;
    __index_list_ctx_t ctx;

    if (len == 1)
    {
        res = ray_find(rcols, lcols);
        if (res->type == -TYPE_I64)
        {
            idx = res->i64;
            drop_obj(res);
            res = vector_i64(1);
            as_i64(res)[0] = idx;
        }

        return res;
    }

    ll = ops_count(as_list(lcols)[0]);
    rl = ops_count(as_list(rcols)[0]);
    ht = ht_tab(maxi64(ll, rl), -1);
    res = vector_i64(maxi64(ll, rl));

    // Right hashes
    __index_list_precalc_hash(rcols, (u64_t *)as_i64(res), len, rl, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, rcols, (u64_t *)as_i64(res)};
    for (i = 0; i < rl; i++)
    {
        idx = ht_tab_next_with(&ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        if (as_i64(as_list(ht)[0])[idx] == NULL_I64)
            as_i64(as_list(ht)[0])[idx] = i;
    }

    // Left hashes
    __index_list_precalc_hash(lcols, (u64_t *)as_i64(res), len, ll, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, lcols, (u64_t *)as_i64(res)};
    for (i = 0; i < ll; i++)
    {
        idx = ht_tab_get_with(ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        as_i64(res)[i] = (idx == NULL_I64) ? NULL_I64 : as_i64(as_list(ht)[0])[idx];
    }

    drop_obj(ht);

    return res;
}
