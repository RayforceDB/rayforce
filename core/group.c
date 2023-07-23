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

#include "group.h"
#include "vector.h"
#include "set.h"

#define MAX_LINEAR_VALUE 1024 * 1024 * 64
#define normalize(k) ((u64_t)(k - min))

bool_t cnt_update(i64_t key, i64_t val, null_t *seed, i64_t *tkey, i64_t *tval)
{
    UNUSED(key);
    UNUSED(*tkey);
    UNUSED(seed);
    UNUSED(val);

    *tval += 1;
    return true;
}

bool_t pos_update(i64_t key, i64_t val, null_t *seed, i64_t *tkey, i64_t *tval)
{
    UNUSED(key);
    UNUSED(*tkey);

    // contains count of elements (replace with vector)
    if ((*tval & (1ll << 62)) == 0)
    {
        rf_object_t *vv = (rf_object_t *)seed;
        rf_object_t v = I64(*tval);
        as_I64(&v)[0] = val;
        v.adt->len = 1;
        *vv = v;
        *tval = (i64_t)seed | 1ll << 62;

        return false;
    }

    // contains vector
    rf_object_t *vv = (rf_object_t *)(*tval & ~(1ll << 62));
    i64_t *v = as_I64(vv);
    v[vv->adt->len++] = val;
    return true;
}

rf_object_t rf_distinct_I64(rf_object_t *x)
{
    i64_t i, j = 0, p = 0, w = 0, xl = x->adt->len;
    i64_t n = 0, range, inrange = 0, min, max, *m, *iv1 = as_I64(x), *ov;
    rf_object_t mask, vec;
    set_t *set;

    if (xl == 0)
        return I64(0);

    if (x->adt->attrs.flags & VEC_ATTR_DISTINCT)
        return clone(x);

    max = min = iv1[0];

    for (i = 0; i < xl; i++)
    {
        if (iv1[i] < min)
            min = iv1[i];
        else if (iv1[i] > max)
            max = iv1[i];

        if (normalize(iv1[i]) < MAX_LINEAR_VALUE)
            inrange++;
    }

    vec = I64(xl);
    ov = as_I64(&vec);

    if (xl <= 10000)
        goto set;

    // all elements are in range
    if (inrange == xl)
    {
        range = max - min + 1;
        mask = I64(range / 64);
        m = as_I64(&mask);

        for (i = 0; i < range / 64; i++)
            m[i] = 0;

        for (i = 0; i < xl; i++)
        {
            n = normalize(iv1[i]);
            p = n / 64;
            w = 1ull << (n & 63);

            if (!(m[p] & w))
            {
                m[p] |= w;
                ov[j++] = iv1[i];
            }
        }

        drop(&mask);
        vector_shrink(&vec, j);

        vec.adt->attrs.flags |= VEC_ATTR_DISTINCT;

        return vec;
    }

    // two thirds of elements are in range
    if (inrange > xl / 3)
    {
        range = MAX_LINEAR_VALUE;
        mask = I64(range / 64);
        m = as_I64(&mask);

        for (i = 0; i < range / 64; i++)
            m[i] = 0;

        // create hash set those elements are not in range
        set = set_new(xl - inrange, &rfi_kmh_hash, &i64_cmp);

        for (i = 0; i < xl; i++)
        {
            n = normalize(iv1[i]);
            if (n < MAX_LINEAR_VALUE)
            {
                p = n / 64;
                w = 1ull << (n & 63);

                if (!(m[p] & w))
                {
                    m[p] |= w;
                    ov[j++] = iv1[i];
                }
            }
            else
            {
                if (set_insert(set, n))
                    ov[j++] = iv1[i];
            }
        }

        vec.adt->attrs.flags |= VEC_ATTR_DISTINCT;
        vector_shrink(&vec, j);
        drop(&mask);
        set_free(set);

        return vec;
    }

set:
    // most of elements are not in range
    set = set_new(xl, &rfi_kmh_hash, &i64_cmp);

    for (i = 0; i < xl; i++)
        if (set_insert(set, normalize(iv1[i])))
            ov[j++] = iv1[i];

    vec.adt->attrs.flags |= VEC_ATTR_DISTINCT;
    vector_shrink(&vec, j);
    set_free(set);

    return vec;
}

rf_object_t rf_group_I64(rf_object_t *x)
{
    i64_t i, j = 0, xl = x->adt->len, *iv1 = as_I64(x), *kv, range, inrange = 0, min, max, *m, n;
    rf_object_t keys, vals, mask, *vv;
    ht_t *ht;

    if (xl == 0)
        return dict(I64(0), list(0));

    max = min = iv1[0];

    for (i = 0; i < xl; i++)
    {
        if (iv1[i] < min)
            min = iv1[i];
        else if (iv1[i] > max)
            max = iv1[i];

        if (normalize(iv1[i]) < MAX_LINEAR_VALUE)
            inrange++;
    }

    if (xl <= 10000)
        goto hash;

    // all elements are in range
    if (inrange == xl)
    {
        range = max - min + 1;
        mask = I64(range);
        m = as_I64(&mask);

        keys = I64(xl);
        vals = list(xl);

        kv = as_I64(&keys);
        vv = as_list(&vals);

        for (i = 0; i < range; i++)
            m[i] = 0;

        for (i = 0; i < xl; i++)
        {
            n = normalize(iv1[i]);
            m[n]++;

            if (m[n] == 1)
                kv[j++] = iv1[i];
        }

        j = 0;
        for (i = 0; i < xl; i++)
        {
            n = normalize(iv1[i]);
            if (m[n] & (1ll << 62))
            {
                rf_object_t *v = (rf_object_t *)(m[n] & ~(1ll << 62));
                as_I64(v)[v->adt->len++] = i;
            }
            else
            {
                rf_object_t v = I64(m[n]);
                as_I64(&v)[0] = i;
                v.adt->len = 1;
                vv[j] = v;
                m[n] = (i64_t)(vv + j) | 1ll << 62;
                j++;
            }
        }

        drop(&mask);

        vector_shrink(&keys, j);
        vector_shrink(&vals, j);

        return dict(keys, vals);
    }

    // two thirds of elements are in range
    if (inrange > xl / 3)
    {
        range = MAX_LINEAR_VALUE;
        mask = I64(range);
        m = as_I64(&mask);

        keys = I64(xl);
        vals = list(xl);

        kv = as_I64(&keys);
        vv = as_list(&vals);

        for (i = 0; i < range; i++)
            m[i] = 0;

        ht = ht_new(xl - inrange, &rfi_i64_hash, &i64_cmp);

        for (i = 0; i < xl; i++)
        {
            n = normalize(iv1[i]);
            if (n < MAX_LINEAR_VALUE)
            {
                m[n]++;

                if (m[n] == 1)
                    kv[j++] = iv1[i];
            }
            else
            {
                if (!ht_upsert_with(ht, n, i, vv + j, &cnt_update))
                    kv[j++] = iv1[i];
            }
        }

        j = 0;
        for (i = 0; i < xl; i++)
        {
            n = normalize(iv1[i]);
            if (n < MAX_LINEAR_VALUE)
            {
                if (m[n] & (1ll << 62))
                {
                    rf_object_t *v = (rf_object_t *)(m[n] & ~(1ll << 62));
                    as_I64(v)[v->adt->len++] = i;
                }
                else
                {
                    rf_object_t v = I64(m[n]);
                    as_I64(&v)[0] = i;
                    v.adt->len = 1;
                    vv[j] = v;
                    m[n] = (i64_t)(vv + j) | 1ll << 62;
                    j++;
                }
            }
            else
            {
                if (!ht_upsert_with(ht, n, i, vv + j, &pos_update))
                    j++;
            }
        }

        drop(&mask);
        ht_free(ht);

        vector_shrink(&keys, j);
        vector_shrink(&vals, j);

        return dict(keys, vals);
    }

hash:
    ht = ht_new(xl, &rfi_i64_hash, &i64_cmp);

    // calculate counts for each key
    for (i = 0; i < xl; i++)
        ht_upsert_with(ht, normalize(iv1[i]), 1, NULL, &cnt_update);

    keys = I64(ht->count);
    vals = list(ht->count);

    kv = as_I64(&keys);
    vv = as_list(&vals);

    // finally, fill vectors with positions
    for (i = 0; i < xl; i++)
    {
        if (!ht_upsert_with(ht, normalize(iv1[i]), i, vv + j, &pos_update))
            kv[j++] = iv1[i];
    }

    ht_free(ht);

    return dict(keys, vals);
}
