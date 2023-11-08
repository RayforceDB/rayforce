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

obj_t ops_distinct_raw(i64_t values[], i64_t indices[], u64_t len)
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

i32_t __obj_cmp(i64_t a, i64_t b, nil_t *seed)
{
    unused(seed);
    return objcmp((obj_t)a, (obj_t)b);
}

u64_t __obj_hash(i64_t a, nil_t *seed)
{
    unused(seed);
    return ops_hash_obj((obj_t)a);
}

obj_t ops_distinct_obj(obj_t values[], i64_t indices[], u64_t len)
{
    u64_t i, j;
    i64_t p, *out;
    obj_t vec, set;

    set = ht_tab(len, -1);

    for (i = 0; i < len; i++)
    {
        p = ht_tab_next_with(&set, (i64_t)values[i], &__obj_hash, &__obj_cmp, NULL);
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
            for (i = 0; i < len; i++)
            {
                out[i] = hashu64(*(u64_t *)&g64v[i], out[i]);
                out[i] = hashu64(*((u64_t *)&g64v[i] + 1), out[i]);
            }
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
    i64_t i, j, n, m, l, idx, min, max, range, *hk, *hv;
    obj_t keys, vals, ht, *vv;

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

    // use open addressing if range is small
    if (range <= len)
    {
        keys = vector_i64(range);
        vals = vector(TYPE_LIST, range);

        hk = as_i64(keys);
        vv = as_list(vals);

        for (i = 0; i < range; i++)
            hk[i] = 0;

        if (indices)
        {
            for (i = 0; i < len; i++)
                hk[values[indices[i]] - min]++;

            for (i = 0; i < range; i++)
            {
                vv[i] = hk[i] ? vector_i64(hk[i]) : null(0);
                hk[i] = 0;
            }

            // fill up positions
            for (i = 0; i < len; i++)
            {
                idx = values[indices[i]] - min;
                as_i64(vv[idx])[hk[idx]++] = indices[i];
            }
        }
        else
        {
            for (i = 0; i < len; i++)
                hk[values[i] - min]++;

            for (i = 0; i < range; i++)
            {
                vv[i] = hk[i] ? vector_i64(hk[i]) : null(0);
                hk[i] = 0;
            }

            // fill up positions
            for (i = 0; i < len; i++)
            {
                idx = values[i] - min;
                as_i64(vv[idx])[hk[idx]++] = i;
            }
        }

        // compact keys/vals
        for (i = 0, j = 0; i < range; i++)
        {
            if (hk[i])
            {
                hk[j] = i + min;
                vv[j++] = vv[i];
            }
        }

        resize(&keys, j);
        resize(&vals, j);

        return dict(keys, vals);
    }

    // use hash table if range is large
    ht = ht_tab(len, TYPE_I64);

    // count occurrences
    if (indices)
    {
        for (i = 0; i < len; i++)
        {
            n = values[indices[i]];
            idx = ht_tab_next(&ht, n);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
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
            n = values[i];
            idx = ht_tab_next(&ht, n);
            hk = as_i64(as_list(ht)[0]);
            hv = as_i64(as_list(ht)[1]);
            if (hk[idx] == NULL_I64)
            {
                hk[idx] = n;
                hv[idx] = 1;
            }
            else
                hv[idx]++;
        }
    }

    hk = as_i64(as_list(ht)[0]);
    hv = as_i64(as_list(ht)[1]);
    vv = as_list(as_list(ht)[1]);
    m = as_list(ht)[0]->len;

    // allocate positions
    for (i = 0; i < m; i++)
    {
        if (hk[i] != NULL_I64)
        {
            vv[i] = vector_i64(hv[i]);
            vv[i]->len = 0;
        }
    }

    // fill positions
    if (indices)
    {
        for (i = 0; i < len; i++)
        {
            n = values[indices[i]];
            idx = ht_tab_get(ht, n);
            l = vv[idx]->len++;
            as_i64(vv[idx])[l] = indices[i];
        }
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            n = values[i];
            idx = ht_tab_get(ht, n);
            l = vv[idx]->len++;
            as_i64(vv[idx])[l] = i;
        }
    }

    // pack offsets and fill keys
    for (i = 0, j = 0; i < m; i++)
    {
        if (hk[i] != NULL_I64)
        {
            hk[j] = hk[i];
            vv[j++] = vv[i];
        }
    }

    resize(&as_list(ht)[0], j);
    resize(&as_list(ht)[1], j);
    as_list(ht)[1]->type = TYPE_LIST;

    return ht;
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
