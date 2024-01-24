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

#include "index.h"
#include "ops.h"
#include "hash.h"
#include "util.h"
#include "error.h"
#include "items.h"
#include "unary.h"

obj_t index_distinct_i8(i8_t values[], i64_t indices[], u64_t len)
{
    unused(values);
    unused(indices);
    unused(len);
    throw(ERR_NOT_IMPLEMENTED, "index_distinct_i8 not implemented");
}

obj_t index_distinct_i64(i64_t values[], i64_t indices[], u64_t len)
{
    u64_t i, j, range;
    i64_t p, min, max, k, *out;
    obj_t vec, set;

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
            max = (values[i] > max) ? values[i] : max;
            min = (values[i] < min) ? values[i] : min;
        }
    }

    range = max - min + 1;

    // use open addressing if range is small
    if (range <= len)
    {
        vec = vector_i64(range);
        out = as_i64(vec);
        memset(out, 0, sizeof(i64_t) * range);

        if (indices)
        {
            for (i = 0; i < len; i++)
                out[values[indices[i]] - min]++;
        }
        else
        {
            for (i = 0; i < len; i++)
                out[values[i] - min]++;
        }
        // compact keys
        for (i = 0, j = 0; i < range; i++)
        {
            if (out[i])
                out[j++] = i + min;
        }

        resize(&vec, j);
        vec->attrs |= ATTR_DISTINCT;

        return vec;
    }

    // otherwise, use a hash table
    set = ht_tab(len, -1);

    for (i = 0; i < len; i++)
    {
        k = values[i] - min;
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
            out[j++] = out[i] + min;
    }

    resize(&as_list(set)[0], j);
    vec = clone(as_list(set)[0]);
    vec->attrs |= ATTR_DISTINCT;
    drop(set);

    return vec;
}

obj_t index_distinct_guid(guid_t values[], i64_t indices[], u64_t len)
{
    unused(indices);
    u64_t i, j;
    i64_t p, *out;
    obj_t vec, set;
    guid_t *g;

    set = ht_tab(len, -1);

    for (i = 0, j = 0; i < len; i++)
    {
        p = ht_tab_next_with(&set, (i64_t)&values[i], &hash_guid, &cmp_guid, NULL);
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
    drop(set);

    return vec;
}

obj_t index_distinct_obj(obj_t values[], i64_t indices[], u64_t len)
{
    unused(indices);
    u64_t i, j;
    i64_t p, *out;
    obj_t vec, set;

    set = ht_tab(len, -1);

    for (i = 0; i < len; i++)
    {
        p = ht_tab_next_with(&set, (i64_t)values[i], &hash_obj, &cmp_obj, NULL);
        out = as_i64(as_list(set)[0]);
        if (out[p] == NULL_I64)
            out[p] = (i64_t)clone(values[i]);
    }

    // compact keys
    out = as_i64(as_list(set)[0]);
    len = as_list(set)[0]->len;
    for (i = 0, j = 0; i < len; i++)
    {
        if (out[i] != NULL_I64)
            out[j++] = out[i];
    }

    resize(&as_list(set)[0], j);
    vec = clone(as_list(set)[0]);
    vec->attrs |= ATTR_DISTINCT;
    vec->type = TYPE_LIST;
    drop(set);

    return vec;
}

i64_t index_range(i64_t *pmin, i64_t *pmax, i64_t values[], i64_t indices[], u64_t len)
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

    *pmin = min;
    *pmax = max;

    return max - min + 1;
}

obj_t index_group_i8(i8_t values[], i64_t indices[], u64_t len)
{
    u64_t i, j, n, range;
    i64_t min, *hk, *hv;
    obj_t keys, vals;

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

    drop(keys);

    return vn_list(2, i64(j), vals);
}

obj_t index_group_i64(i64_t values[], i64_t indices[], u64_t len)
{
    u64_t i, j, n, range;
    i64_t idx, min, max, *hk, *hv, *hp;
    obj_t keys, vals, ht;

    range = index_range(&min, &max, values, indices, len);

    // use open addressing if range is compatible with the input length
    if (range <= len)
    {
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

        drop(keys);

        return vn_list(2, i64(j), vals);
    }

    // use hash table if range is large
    ht = ht_tab(len, TYPE_I64);

    // distribute bins
    if (indices)
    {
        for (i = 0, j = 0; i < len; i++)
        {
            n = values[indices[i]] - min;
            idx = ht_tab_next_with(&ht, n, &hash_fnv1a, &cmp_i64, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = n;
                hv[idx] = j++;
            }
        }
    }
    else
    {
        for (i = 0, j = 0; i < len; i++)
        {
            n = values[i] - min;
            idx = ht_tab_next_with(&ht, n, &hash_fnv1a, &cmp_i64, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = n;
                hv[idx] = j++;
            }
        }
    }

    vals = vector_i64(len);
    hp = as_i64(vals);

    if (indices)
    {
        for (i = 0; i < len; i++)
        {
            n = values[indices[i]] - min;
            idx = ht_tab_get_with(ht, n, &hash_fnv1a, &cmp_i64, NULL);
            hp[i] = hv[idx];
        }
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            n = values[i] - min;
            idx = ht_tab_get_with(ht, n, &hash_fnv1a, &cmp_i64, NULL);
            hp[i] = hv[idx];
        }
    }

    drop(ht);

    return vn_list(2, i64(j), vals);
}

obj_t index_group_guid(guid_t values[], i64_t indices[], u64_t len)
{
    u64_t i, j, n;
    i64_t idx, *hk, *hv, *hp;
    obj_t vals, ht;

    ht = ht_tab(len, TYPE_I64);

    // distribute bins
    if (indices)
    {
        for (i = 0, j = 0; i < len; i++)
        {
            idx = ht_tab_next_with(&ht, (i64_t)&values[indices[i]], &hash_guid, &cmp_guid, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = (i64_t)&values[indices[i]];
                hv[idx] = j++;
            }
        }
    }
    else
    {
        for (i = 0, j = 0; i < len; i++)
        {
            idx = ht_tab_next_with(&ht, (i64_t)&values[i], &hash_guid, &cmp_guid, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = (i64_t)&values[i];
                hv[idx] = j++;
            }
        }
    }

    vals = vector_i64(len);
    hp = as_i64(vals);

    if (indices)
    {
        for (i = 0; i < len; i++)
        {
            n = (i64_t)&values[indices[i]];
            idx = ht_tab_get_with(ht, n, &hash_guid, &cmp_guid, NULL);
            hp[i] = hv[idx];
        }
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            n = (i64_t)&values[i];
            idx = ht_tab_get_with(ht, n, &hash_guid, &cmp_guid, NULL);
            hp[i] = hv[idx];
        }
    }

    drop(ht);

    return vn_list(2, i64(j), vals);
}

obj_t index_group_obj(obj_t values[], i64_t indices[], u64_t len)
{
    u64_t i, j, n;
    i64_t idx, *hk, *hv, *hp;
    obj_t vals, ht;

    ht = ht_tab(len, TYPE_I64);

    // distribute bins
    if (indices)
    {
        for (i = 0, j = 0; i < len; i++)
        {
            n = (i64_t)values[indices[i]];
            idx = ht_tab_next_with(&ht, n, &hash_obj, &cmp_obj, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = n;
                hv[idx] = j++;
            }
        }
    }
    else
    {
        for (i = 0, j = 0; i < len; i++)
        {
            n = (i64_t)values[i];
            idx = ht_tab_next_with(&ht, n, &hash_obj, &cmp_obj, NULL);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = n;
                hv[idx] = j++;
            }
        }
    }

    vals = vector_i64(len);
    hp = as_i64(vals);

    if (indices)
    {
        for (i = 0; i < len; i++)
        {
            n = (i64_t)values[indices[i]];
            idx = ht_tab_get_with(ht, n, &hash_obj, &cmp_obj, NULL);
            hp[i] = hv[idx];
        }
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            n = (i64_t)values[i];
            idx = ht_tab_get_with(ht, n, &hash_obj, &cmp_obj, NULL);
            hp[i] = hv[idx];
        }
    }

    drop(ht);

    return vn_list(2, i64(j), vals);
}

nil_t index_hash_list(obj_t obj, u64_t out[], u64_t len, u64_t seed)
{
    u8_t *u8v;
    f64_t *f64v;
    guid_t *g64v;
    u64_t i, *u64v;
    obj_t k, v;
    i64_t *ids;

    switch (obj->type)
    {
    case TYPE_BOOL:
    case TYPE_BYTE:
    case TYPE_CHAR:
        u8v = as_u8(obj);
        if (seed != 0)
            for (i = 0; i < len; i++)
                out[i] = index_hash_u64((u64_t)u8v[i], seed);
        else
            for (i = 0; i < len; i++)
                out[i] = index_hash_u64((u64_t)u8v[i], out[i]);
        break;
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        u64v = (u64_t *)as_i64(obj);
        if (seed != 0)
            for (i = 0; i < len; i++)
                out[i] = index_hash_u64(u64v[i], seed);
        else
            for (i = 0; i < len; i++)
                out[i] = index_hash_u64(u64v[i], out[i]);
        break;
    case TYPE_F64:
        f64v = as_f64(obj);
        if (seed != 0)
            for (i = 0; i < len; i++)
                out[i] = index_hash_u64((u64_t)f64v[i], seed);
        else
            for (i = 0; i < len; i++)
                out[i] = index_hash_u64((u64_t)f64v[i], out[i]);
        break;
    case TYPE_GUID:
        g64v = as_guid(obj);
        if (seed != 0)
        {
            for (i = 0; i < len; i++)
            {
                out[i] = index_hash_u64(*(u64_t *)&g64v[i], seed);
                out[i] = index_hash_u64(*((u64_t *)&g64v[i] + 1), out[i]);
            }
        }
        else
        {
            for (i = 0; i < len; i++)
            {
                out[i] = index_hash_u64(*(u64_t *)&g64v[i], out[i]);
                out[i] = index_hash_u64(*((u64_t *)&g64v[i] + 1), out[i]);
            }
        }
        break;
    case TYPE_LIST:
        if (seed != 0)
            for (i = 0; i < len; i++)
                out[i] = index_hash_u64(index_hash_obj(as_list(obj)[i]), seed);
        else
            for (i = 0; i < len; i++)
                out[i] = index_hash_u64(index_hash_obj(as_list(obj)[i]), out[i]);
        break;
    case TYPE_ENUM:
        k = ray_key(obj);
        v = ray_get(k);
        drop(k);
        u64v = (u64_t *)as_symbol(v);
        ids = as_i64(enum_val(obj));
        if (seed != 0)
            for (i = 0; i < len; i++)
                out[i] = index_hash_u64(u64v[ids[i]], seed);
        else
            for (i = 0; i < len; i++)
                out[i] = index_hash_u64(u64v[ids[i]], out[i]);
        drop(v);
        break;
    case TYPE_ANYMAP:
        if (seed != 0)
        {
            for (i = 0; i < len; i++)
            {
                v = at_idx(obj, i);
                out[i] = index_hash_u64(index_hash_obj(v), seed);
                drop(v);
            }
        }
        else
        {
            for (i = 0; i < len; i++)
            {
                v = at_idx(obj, i);
                out[i] = index_hash_u64(index_hash_obj(v), out[i]);
                drop(v);
            }
        }
        break;
    default:
        panic("hash list: unsupported type: %d", obj->type);
    }
}

obj_t index_group_cnts(obj_t grp)
{
    u64_t i, l, n;
    i64_t *grps, *ids;
    obj_t res;

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