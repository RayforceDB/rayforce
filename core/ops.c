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

#include <time.h>
#include "ops.h"
#include "string.h"
#include "util.h"
#include "hash.h"
#include "heap.h"
#include "serde.h"
#include "items.h"
#include "unary.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#else
#include <errno.h>
#endif

__thread u64_t __RND_SEED__ = 0;

/*
 * Treat obj as a bool
 */
bool_t ops_as_bool(obj_t x)
{
    switch (x->type)
    {
    case -TYPE_BOOL:
        return x->bool;
    case -TYPE_BYTE:
    case -TYPE_CHAR:
        return x->u8 != 0;
    case -TYPE_I64:
    case -TYPE_SYMBOL:
    case -TYPE_TIMESTAMP:
        return x->i64 != 0;
    case TYPE_BOOL:
    case TYPE_BYTE:
    case TYPE_CHAR:
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
    case TYPE_LIST:
        return x->len != 0;
    default:
        return true;
    }
}
/*
 * In case of using -Ofast compiler flag, we can not just use x != x due to
 * compiler optimizations. So we need to use memcpy to get the bits of the x
 * and then separate check mantissa and exponent.
 */
bool_t ops_is_nan(f64_t x)
{
    u64_t bits;
    memcpy(&bits, &x, sizeof(x));
    return (bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (bits & 0x000fffffffffffffull) != 0;
}

u64_t ops_rand_u64()
{
    __RND_SEED__ += time(0);
    __RND_SEED__ = (6364136223846793005ull * __RND_SEED__ + 1442695040888963407ull) % (1ull << 63);
    return __RND_SEED__;
}

bool_t cnt_update(i64_t key, i64_t val, nil_t *seed, i64_t *tkey, i64_t *tval)
{
    unused(key);
    unused(*tkey);
    unused(seed);
    unused(val);

    *tval += 1;
    return true;
}

bool_t pos_update(i64_t key, i64_t val, nil_t *seed, i64_t *tkey, i64_t *tval)
{
    unused(key);
    unused(*tkey);

    // contains count of elements (replace with vector)
    if ((*tval & (1ll << 62)) == 0)
    {
        obj_t *vv = (obj_t *)seed;
        obj_t v = vector_i64(*tval);
        as_i64(v)[0] = val;
        v->len = 1;
        *vv = v;
        *tval = (i64_t)seed | 1ll << 62;

        return false;
    }

    // contains vector
    obj_t *vv = (obj_t *)(*tval & ~(1ll << 62));
    i64_t *v = as_i64(*vv);
    v[(*vv)->len++] = val;
    return true;
}

obj_t ops_distinct(obj_t x)
{
    i64_t i, j, l, r, p, min, max, range, k, w, b;
    obj_t mask, vec, set;

    if (!x || x->len == 0)
        return vector_i64(0);

    if (x->attrs & ATTR_DISTINCT)
        return clone(x);

    min = max = as_i64(x)[0];

    l = x->len;

    for (i = 0; i < l; i++)
    {
        if (as_i64(x)[i] < min)
            min = as_i64(x)[i];
        else if (as_i64(x)[i] > max)
            max = as_i64(x)[i];
    }

    range = max - min + 1;

    if (range <= l)
    {
        r = alignup(range / 8, 8);
        if (!r)
            r = 1;

        mask = vector_bool(r);
        memset(as_bool(mask), 0, r);
        vec = vector_i64(range);

        for (i = 0, j = 0; i < l; i++)
        {
            k = as_i64(x)[i] - min;
            w = k >> 3; // k / 8
            b = k & 7;  // k % 8

            if (as_bool(mask)[w] & (1 << b))
                continue;

            as_bool(mask)[w] |= (1 << b);
            as_i64(vec)[j++] = as_i64(x)[i];
        }

        drop(mask);

        resize(&vec, j);

        return vec;
    }

    set = ht_tab(l, -1);
    vec = vector_i64(l);

    for (i = 0, j = 0; i < l; i++)
    {
        p = ht_tab_next(&set, as_i64(x)[i] - min);
        if (as_i64(as_list(set)[0])[p] == NULL_I64)
        {
            as_i64(as_list(set)[0])[p] = as_i64(x)[i] - min;
            as_i64(vec)[j++] = as_i64(x)[i];
        }
    }

    vec->attrs |= ATTR_DISTINCT;

    resize(&vec, j);
    drop(set);

    return vec;
}

bool_t ops_eq_idx(obj_t a, i64_t ai, obj_t b, i64_t bi)
{
    obj_t lv, rv;
    bool_t eq;

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
    case TYPE_ENUM:
        lv = at_idx(a, ai);
        rv = at_idx(b, bi);
        eq = lv->i64 == rv->i64;
        dropn(2, lv, rv);
        return eq;
    case TYPE_ANYMAP:
        lv = at_idx(a, ai);
        rv = at_idx(b, bi);
        eq = objcmp(lv, rv) == 0;
        dropn(2, lv, rv);
        return eq;
    default:
        panic("hash: unsupported type: %d", a->type);
    }
}

inline __attribute__((always_inline)) u64_t hashu64(u64_t h, u64_t k)
{
    u64_t a, b;

    a = (h ^ k) * 0x9ddfea08eb382d69ull;
    a ^= (a >> 47);
    b = (roti64(k, 31) ^ a) * 0x9ddfea08eb382d69ull;
    b ^= (b >> 47);
    b *= 0x9ddfea08eb382d69ull;

    return b;
}

u64_t ops_hash_obj(obj_t obj)
{
    u64_t hash, len, i;
    str_t str;

    switch (obj->type)
    {
    case -TYPE_I64:
    case -TYPE_SYMBOL:
    case -TYPE_TIMESTAMP:
        return (u64_t)obj->i64;
    case -TYPE_F64:
        return (u64_t)obj->f64;
    case TYPE_CHAR:
        len = obj->len;
        hash = 0xcbf29ce484222325ull;
        str = as_string(obj);
        for (i = 0; i < len; i++)
        {
            hash ^= (u64_t)str[i];
            hash *= 0x100000001b3ull;
        }
        return hash;

    default:
        panic("hash: unsupported type: %d", obj->type);
    }
}

nil_t ops_hash_list(obj_t obj, u64_t out[], u64_t len, u64_t seed)
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
                out[i] = hashu64((u64_t)u8v[i], seed);
        else
            for (i = 0; i < len; i++)
                out[i] = hashu64((u64_t)u8v[i], out[i]);
        break;
    case TYPE_I64:
    case TYPE_SYMBOL:
    case TYPE_TIMESTAMP:
        u64v = (u64_t *)as_i64(obj);
        if (seed != 0)
            for (i = 0; i < len; i++)
                out[i] = hashu64(u64v[i], seed);
        else
            for (i = 0; i < len; i++)
                out[i] = hashu64(u64v[i], out[i]);
        break;
    case TYPE_F64:
        f64v = as_f64(obj);
        if (seed != 0)
            for (i = 0; i < len; i++)
                out[i] = hashu64((u64_t)f64v[i], seed);
        else
            for (i = 0; i < len; i++)
                out[i] = hashu64((u64_t)f64v[i], out[i]);
        break;
    case TYPE_GUID:
        g64v = as_guid(obj);
        if (seed != 0)
        {
            for (i = 0; i < len; i++)
            {
                out[i] = hashu64(*(u64_t *)&g64v[i], seed);
                out[i] = hashu64(*((u64_t *)&g64v[i] + 1), out[i]);
            }
        }
        else
        {
            out[i] = hashu64(*(u64_t *)&g64v[i], out[i]);
            out[i] = hashu64(*((u64_t *)&g64v[i] + 1), out[i]);
        }
        break;
    case TYPE_LIST:
        if (seed != 0)
            for (i = 0; i < len; i++)
                out[i] = hashu64(ops_hash_obj(as_list(obj)[i]), seed);
        else
            for (i = 0; i < len; i++)
                out[i] = hashu64(ops_hash_obj(as_list(obj)[i]), out[i]);
        break;
    case TYPE_ENUM:
        k = ray_key(obj);
        v = ray_get(k);
        drop(k);
        u64v = (u64_t *)as_symbol(v);
        ids = as_i64(enum_val(obj));
        if (seed != 0)
            for (i = 0; i < len; i++)
                out[i] = hashu64(u64v[ids[i]], seed);
        else
            for (i = 0; i < len; i++)
                out[i] = hashu64(u64v[ids[i]], out[i]);
        drop(v);
        break;
    case TYPE_ANYMAP:
        if (seed != 0)
        {
            for (i = 0; i < len; i++)
            {
                v = at_idx(obj, i);
                out[i] = hashu64(ops_hash_obj(v), seed);
                drop(v);
            }
        }
        else
        {
            for (i = 0; i < len; i++)
            {
                v = at_idx(obj, i);
                out[i] = hashu64(ops_hash_obj(v), out[i]);
                drop(v);
            }
        }
        break;
    default:
        panic("hash list: unsupported type: %d", obj->type);
    }
}

obj_t ops_find(i64_t x[], u64_t xl, i64_t y[], u64_t yl)
{
    u64_t i, range;
    i64_t max = 0, min = 0, n, p, *r, *f;
    obj_t vec, ht;

    if (xl == 0)
        return vector_i64(0);

    max = min = x[0];

    for (i = 0; i < xl; i++)
    {
        max = (x[i] > max) ? x[i] : max;
        min = (x[i] < min) ? x[i] : min;
    }

    range = max - min + 1;

    if (range <= yl)
    {
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
            r[i] = (y[i] < min || y[i] > max) ? NULL_I64 : f[n];
        }

        resize(&vec, yl);

        return vec;
    }

    vec = vector_i64(yl);
    r = as_i64(vec);

    // otherwise, use a hash table
    ht = ht_tab(xl, TYPE_I64);

    for (i = 0; i < xl; i++)
    {
        p = ht_tab_next(&ht, x[i] - min);
        if (as_i64(as_list(ht)[0])[p] == NULL_I64)
        {
            as_i64(as_list(ht)[0])[p] = x[i] - min;
            as_i64(as_list(ht)[1])[p] = i;
        }
    }

    for (i = 0; i < yl; i++)
    {
        p = ht_tab_get(ht, y[i] - min);
        r[i] = p == NULL_I64 ? NULL_I64 : as_i64(as_list(ht)[1])[p];
    }

    drop(ht);

    return vec;
}

obj_t ops_group(i64_t values[], i64_t indices[], i64_t len)
{
    i64_t i, j, n, m, idx, min, max, range, *hk, *hv, *kv, *vv;
    obj_t keys, vals, k, v, ht;

    if (len == 0)
        return dict(vector_i64(0), list(0));

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

    range = max - min + 1;

    // use flat vector instead of hash table
    if (range <= len)
    {
        ht = vector_i64(range);
        hk = as_i64(ht);
        memset(hk, 0, range * sizeof(i64_t));

        // First pass - Count occurrences
        if (indices)
        {
            for (i = 0, j = 0; i < len; i++)
            {
                n = values[indices[i]] - min;
                if (hk[n] == 0)
                {
                    hk[n] = 1;
                    j++;
                }
                else
                    hk[n]++;
            }
        }
        else
        {
            for (i = 0, j = 0; i < len; i++)
            {
                n = values[i] - min;
                if (hk[n] == 0)
                {
                    hk[n] = 1;
                    j++;
                }
                else
                    hk[n]++;
            }
        }

        // allocate space for distinct keys
        keys = vector_i64(j);
        kv = as_i64(keys);

        // Pre-compute offsets into all_indices array
        for (i = 0, n = 0; i < range; i++)
        {
            if (hk[i] == 0)
                continue;

            j = hk[i];
            hk[i] = n;
            n += j;
        }

        // Allocate space for all indices
        vals = vector_i64(n);
        vv = as_i64(vals);

        // Single pass through the input, populating all_indices
        if (indices)
        {
            for (i = 0; i < len; i++)
            {
                n = values[indices[i]] - min;
                vv[hk[n]++] = indices[i];
            }
        }
        else
        {
            for (i = 0; i < len; i++)
            {
                n = values[i] - min;
                vv[hk[n]++] = i;
            }
        }

        // pack offsets and fill keys
        n = ht->len;
        for (i = 0, j = 0; i < n; i++)
        {
            if (hk[i] != 0)
            {
                kv[j] = i + min;
                hk[j++] = hk[i];
            }
        }

        resize(&ht, j);

        return list(3, keys, ht, vals);
    }

    // Use hash table
    ht = ht_tab(len, TYPE_I64);
    hk = as_i64(as_list(ht)[0]);
    hv = as_i64(as_list(ht)[1]);

    // First pass - Count occurrences
    if (indices)
    {
        for (i = 0; i < len; i++)
        {
            n = values[indices[i]] - min;
            idx = ht_tab_next(&ht, n);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = n;
                hv[idx] = 1;
            }
            else
                hv[idx]++;
        }
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            n = values[i] - min;
            idx = ht_tab_next(&ht, n);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = n;
                hv[idx] = 1;
            }
            else
                hv[idx]++;
        }
    }

    // Pre-compute offsets into all_indices array
    m = as_list(ht)[0]->len;
    hk = as_i64(as_list(ht)[0]);
    hv = as_i64(as_list(ht)[1]);
    for (i = 0, n = 0; i < m; i++)
    {
        if (hk[i] != NULL_I64)
        {
            j = hv[i];
            hv[i] = n;
            n += j;
        }
    }

    // Allocate space for all indices
    vals = vector_i64(n);
    vv = as_i64(vals);

    // Single pass through the input, populating all_indices
    if (indices)
    {
        for (i = 0; i < len; i++)
        {
            n = values[indices[i]] - min;
            idx = ht_tab_next(&ht, n);
            vv[as_i64(as_list(ht)[1])[idx]++] = indices[i];
        }
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            n = values[i] - min;
            idx = ht_tab_next(&ht, n);
            vv[as_i64(as_list(ht)[1])[idx]++] = i;
        }
    }

    // pack offsets and fill keys
    m = as_list(ht)[0]->len;
    hk = as_i64(as_list(ht)[0]);
    hv = as_i64(as_list(ht)[1]);
    for (i = 0, j = 0; i < m; i++)
    {
        if (hk[i] != NULL_I64)
        {
            hk[j] = hk[i] + min;
            hv[j++] = hv[i];
        }
    }

    k = clone(as_list(ht)[0]);
    v = clone(as_list(ht)[1]);

    drop(ht);

    resize(&k, j);
    resize(&v, j);

    return list(3, k, v, vals);
}

u64_t ops_count(obj_t x)
{
    if (!x)
        return 0;

    switch (x->type)
    {
    case TYPE_TABLE:
        return as_list(as_list(x)[1])[0]->len;
    case TYPE_DICT:
        return as_list(x)[0]->len;
    case TYPE_ENUM:
        return enum_val(x)->len;
    case TYPE_ANYMAP:
        return anymap_val(x)->len;
    case TYPE_VECMAP:
    case TYPE_LISTMAP:
        return as_list(x)[1]->len;
    default:
        return x->len;
    }
}

#if defined(_WIN32) || defined(__CYGWIN__)

obj_t sys_error(os_error_type_t type, str_t msg)
{
    str_t emsg;
    obj_t err;
    DWORD dw;
    LPVOID lpMsgBuf;

    switch (type)
    {
    case ERROR_TYPE_OS:
        emsg = str_fmt(0, "%s: %s", msg, strerror(errno));
        err = error(ERR_IO, emsg);
        heap_free(emsg);
        return err;

    case ERROR_TYPE_SOCK:
        dw = WSAGetLastError();
        break;
    default:
        dw = GetLastError();
    }

    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        dw,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&lpMsgBuf,
        0, NULL);

    emsg = str_fmt(0, "%s: %s", msg, lpMsgBuf);
    err = error(ERR_IO, emsg);
    heap_free(emsg);

    LocalFree(lpMsgBuf);

    return err;
}

#else

obj_t sys_error(os_error_type_t type, str_t msg)
{
    unused(type);
    str_t emsg;
    obj_t err;

    emsg = str_fmt(0, "%s: %s", msg, strerror(errno));
    err = error(ERR_IO, emsg);
    heap_free(emsg);
    return err;
}

#endif
