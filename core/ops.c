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

static u64_t __RND_SEED__ = 0;

#define MAX_LINEAR_VALUE 1024 * 1024 * 64
#define normalize(k) ((u64_t)(k - min))

/*
 * Incase of using -Ofast compiler flag, we can not just use x != x due to
 * compiler optimizations. So we need to use memcpy to get the bits of the x
 * and then separate check mantissa and exponent.
 */
bool_t rfi_is_nan(f64_t x)
{
    u64_t bits;
    memcpy(&bits, &x, sizeof(x));
    return (bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (bits & 0x000FFFFFFFFFFFFFULL) != 0;
}

bool_t rfi_eq(obj_t x, obj_t y)
{
    if (x->type != y->type)
        return false;

    switch (x->type)
    {
    case -TYPE_BOOL:
        return x->bool == y->bool;
    case -TYPE_I64:
        return x->i64 == y->i64;
    case -TYPE_F64:
        return x->f64 == y->f64;
    case TYPE_CHAR:
        return strcmp(as_string(x), as_string(y)) == 0;
    default:
        return false;
    }
}

bool_t rfi_lt(obj_t x, obj_t y)
{
    if (x->type != y->type)
        return false;

    switch (x->type)
    {
    case -TYPE_BOOL:
        return x->bool - y->bool;
    case -TYPE_I64:
        return x->i64 < y->i64;
    case -TYPE_F64:
        return x->f64 < y->f64;
    case TYPE_CHAR:
        return strcmp(as_string(x), as_string(y)) < 0;
    default:
        return 0;
    }
}

i32_t i64_cmp(i64_t a, i64_t b)
{
    return a != b;
}

i64_t rfi_round_f64(f64_t x)
{
    return x >= 0.0 ? (i64_t)(x + 0.5) : (i64_t)(x - 0.5);
}

i64_t rfi_floor_f64(f64_t x)
{
    return x >= 0.0 ? (i64_t)x : (i64_t)(x - 1.0);
}

i64_t rfi_ceil_f64(f64_t x)
{
    return x >= 0.0 ? (i64_t)(x + 1.0) : (i64_t)x;
}

u64_t rfi_rand_u64()
{
#define A 6364136223846793005ULL
#define C 1442695040888963407ULL
#define M (1ULL << 63)
    __RND_SEED__ += time(0);
    __RND_SEED__ = (A * __RND_SEED__ + C) % M;
    return __RND_SEED__;
}

u64_t rfi_kmh_hash(i64_t key)
{
#define LARGE_PRIME 6364136223846793005ULL
    return (key * LARGE_PRIME) >> 32;
}

u64_t rfi_fnv1a_hash_64(i64_t key)
{
#define FNV_OFFSET_64 14695981039346656037ULL
#define FNV_PRIME_64 1099511628211ULL
    u64_t hash = FNV_OFFSET_64;
    i32_t i;
    for (i = 0; i < 8; i++)
    {
        u8_t byte = (key >> (i * 8)) & 0xff;
        hash ^= byte;
        hash *= FNV_PRIME_64;
    }

    return hash;
}

u64_t rfi_i64_hash(i64_t key)
{
    return (u64_t)key;
}

bool_t rfi_as_bool(obj_t x)
{
    switch (x->type)
    {
    case -TYPE_BOOL:
        return x->bool;
    case -TYPE_I64:
        return x->i64 != 0;
    case -TYPE_F64:
        return x->f64 != 0.0;
    case -TYPE_CHAR:
        return x->schar != 0;
    case TYPE_BOOL:
    case TYPE_I64:
    case TYPE_F64:
    case TYPE_CHAR:
    case TYPE_LIST:
        return x->len != 0;
    default:
        return false;
    }
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

obj_t distinct(obj_t x)
{
    i64_t i, j = 0, l, *p, min, max, range;
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
        mask = vector_bool(range);
        memset(as_bool(mask), 0, range);

        for (i = 0; i < l; i++)
        {
            if (!as_bool(mask)[as_i64(x)[i] - min])
            {
                as_bool(mask)[as_i64(x)[i] - min] = true;
                j++;
            }
        }

        vec = vector_i64(j);
        l = mask->len;
        j = 0;
        for (i = 0; i < l; i++)
            if (as_bool(mask)[i])
                as_i64(vec)[j++] = i + min;

        drop(mask);

        return vec;
    }

    set = ht_set(l);
    vec = vector_i64(l);

    for (i = 0; i < l; i++)
    {
        p = ht_set_get(&set, as_i64(x)[i]);
        if (p[0] == NULL_I64)
        {
            p[0] = as_i64(x)[i];
            as_i64(vec)[j++] = as_i64(x)[i];
        }
    }

    vec->attrs |= ATTR_DISTINCT;

    drop(set);
    resize(&vec, j);

    return vec;
}

obj_t group(obj_t x)
{
    i64_t i, j = 0, xl = x->len, range, inrange = 0, min, max, *m, n;
    obj_t keys, vals, mask, v, ht;

    if (xl == 0)
        return dict(vector_i64(0), list(0));

    ht = ht_tab(xl);

    // calculate counts for each key
    for (i = 0; i < xl; i++)
    {
        m = ht_tab_get(&ht, as_i64(x)[i]);
        if (m[0] == NULL_I64)
        {
            m[0] = as_i64(x)[i];
            m[1] = 1;
            j++;
        }
        else
            m[1] += 1;
    }

    keys = vector_i64(j);
    vals = vector(TYPE_LIST, j);

    // finally, fill vectors with positions
    j = 0;
    for (i = 0; i < xl; i++)
    {
        m = ht_tab_get(&ht, as_i64(x)[i]);
        if (m[1] & (1ll << 62))
        {
            v = (obj_t)(m[1] & ~(1ll << 62));
            as_i64(v)[v->len++] = i;
        }
        else
        {
            as_i64(keys)[j] = as_i64(x)[i];
            v = vector_i64(m[1]);
            v->len = 1;
            as_i64(v)[0] = i;
            as_list(vals)[j++] = v;
            m[1] = (i64_t)v | 1ll << 62;
        }
    }

    drop(ht);

    return dict(keys, vals);
}
