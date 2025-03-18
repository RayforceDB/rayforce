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
#include "runtime.h"
#include "pool.h"
#include "term.h"

u64_t __hash_get(i64_t row, raw_p seed) {
    __index_find_ctx_t *ctx = (__index_find_ctx_t *)seed;
    return ctx->hashes[row];
}

i64_t __cmp_obj(i64_t row1, i64_t row2, raw_p seed) {
    __index_find_ctx_t *ctx = (__index_find_ctx_t *)seed;
    return cmp_obj(((obj_p *)ctx->lobj)[row1], ((obj_p *)ctx->robj)[row2]);
}

i64_t __hash_cmp_guid(i64_t row1, i64_t row2, raw_p seed) {
    __index_find_ctx_t *ctx = (__index_find_ctx_t *)seed;
    return memcmp((guid_t *)ctx->lobj + row1, (guid_t *)ctx->robj + row2, sizeof(guid_t));
}

u64_t __index_list_hash_get(i64_t row, raw_p seed) {
    __index_list_ctx_t *ctx = (__index_list_ctx_t *)seed;
    return ctx->hashes[row];
}

i64_t __index_list_cmp_row(i64_t row1, i64_t row2, raw_p seed) {
    u64_t i, l;
    __index_list_ctx_t *ctx = (__index_list_ctx_t *)seed;
    i64_t *filter = ctx->filter;
    obj_p *lcols = AS_LIST(ctx->lcols);
    obj_p *rcols = AS_LIST(ctx->rcols);

    l = ctx->lcols->len;

    if (filter) {
        for (i = 0; i < l; i++) {
            if (ops_eq_idx(lcols[i], filter[row1], rcols[i], filter[row2]) == 0)
                return 1;
        }
    } else {
        for (i = 0; i < l; i++)
            if (ops_eq_idx(lcols[i], row1, rcols[i], row2) == 0)
                return 1;
    }

    return 0;
}

obj_p index_hash_obj_partial(obj_p obj, u64_t out[], i64_t filter[], u64_t len, u64_t offset, b8_t resolve) {
    u8_t *u8v;
    guid_t *g64v;
    u64_t i, *u64v;
    obj_p k, v, *l64v;
    i64_t *ids;

    switch (obj->type) {
        case -TYPE_B8:
        case -TYPE_U8:
        case -TYPE_C8:
            out[offset] = hash_index_u64((u64_t)obj->u8, out[offset]);
            break;
        case -TYPE_I64:
        case -TYPE_SYMBOL:
        case -TYPE_TIMESTAMP:
            out[offset] = hash_index_u64((u64_t)obj->i64, out[offset]);
            break;
        case -TYPE_F64:
            out[offset] = hash_index_u64((u64_t)obj->f64, out[offset]);
            break;
        case -TYPE_GUID:
            out[offset] = hash_index_u64(*(u64_t *)AS_GUID(obj), *((u64_t *)AS_GUID(obj) + 1));
            break;
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            u8v = AS_U8(obj);
            if (filter)
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64((u64_t)u8v[filter[i]], out[i]);
            else
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64((u64_t)u8v[i], out[i]);
            break;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            u64v = (u64_t *)AS_I64(obj);
            if (filter)
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64(u64v[filter[i]], out[i]);
            else
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64(u64v[i], out[i]);
            break;
        case TYPE_F64:
            u64v = (u64_t *)AS_F64(obj);
            if (filter)
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64(u64v[filter[i]], out[i]);
            else
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64(u64v[i], out[i]);
            break;
        case TYPE_GUID:
            g64v = AS_GUID(obj);
            if (filter)
                for (i = 0; i < len; i++) {
                    out[i] = hash_index_u64(*(u64_t *)&g64v[filter[i]], out[i]);
                    out[i] = hash_index_u64(*((u64_t *)&g64v[filter[i]] + 1), out[i]);
                }
            else
                for (i = offset; i < len + offset; i++) {
                    out[i] = hash_index_u64(*(u64_t *)&g64v[i], out[i]);
                    out[i] = hash_index_u64(*((u64_t *)&g64v[i] + 1), out[i]);
                }
            break;
        case TYPE_LIST:
            l64v = AS_LIST(obj);
            if (filter)
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64(hash_index_obj(l64v[filter[i]]), out[i]);
            else
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64(hash_index_obj(l64v[i]), out[i]);
            break;
        case TYPE_ENUM:
            if (resolve) {
                k = ray_key(obj);
                v = ray_get(k);
                drop_obj(k);
                u64v = (u64_t *)AS_SYMBOL(v);
                ids = AS_I64(ENUM_VAL(obj));
                if (filter)
                    for (i = offset; i < len + offset; i++)
                        out[i] = hash_index_u64(u64v[ids[filter[i]]], out[i]);
                else
                    for (i = offset; i < len + offset; i++)
                        out[i] = hash_index_u64(u64v[ids[i]], out[i]);
                drop_obj(v);
            } else {
                u64v = (u64_t *)AS_I64(ENUM_VAL(obj));
                if (filter)
                    for (i = offset; i < len + offset; i++)
                        out[i] = hash_index_u64(u64v[filter[i]], out[i]);
                else
                    for (i = offset; i < len + offset; i++)
                        out[i] = hash_index_u64(u64v[i], out[i]);
            }
            break;
        case TYPE_MAPLIST:
            if (filter)
                for (i = offset; i < len + offset; i++) {
                    v = at_idx(obj, filter[i]);
                    out[i] = hash_index_u64(hash_index_obj(v), out[i]);
                    drop_obj(v);
                }
            else
                for (i = offset; i < len + offset; i++) {
                    v = at_idx(obj, i);
                    out[i] = hash_index_u64(hash_index_obj(v), out[i]);
                    drop_obj(v);
                }
            break;
        default:
            PANIC("hash list: unsupported type: %d", obj->type);
    }

    return NULL_OBJ;
}

nil_t __index_list_precalc_hash(obj_p cols, u64_t out[], u64_t ncols, u64_t nrows, i64_t filter[], b8_t resolve) {
    u64_t i, j, chunks, chunk;
    pool_p pool;
    obj_p v;

    pool = pool_get();
    chunks = pool_split_by(pool, nrows, 0);
    chunk = nrows / chunks;

    // init hashes
    for (i = 0; i < nrows; i++)
        out[i] = U64_HASH_SEED;

    // calculate hashes
    if (chunks == 1) {
        for (i = 0; i < ncols; i++)
            index_hash_obj_partial(AS_LIST(cols)[i], out, filter, nrows, 0, resolve);
    } else {
        for (i = 0; i < ncols; i++) {
            pool_prepare(pool);

            for (j = 0; j < chunks - 1; j++)
                pool_add_task(pool, (raw_p)index_hash_obj_partial, 6, AS_LIST(cols)[i], out, filter, chunk, j * chunk,
                              resolve);

            pool_add_task(pool, (raw_p)index_hash_obj_partial, 6, AS_LIST(cols)[i], out, filter, nrows - j * chunk,
                          j * chunk, resolve);

            v = pool_run(pool);
            drop_obj(v);
        }
    }
}

nil_t index_hash_obj(obj_p obj, u64_t out[], i64_t filter[], u64_t len, b8_t resolve) {
    index_hash_obj_partial(obj, out, filter, len, 0, resolve);
}

obj_p index_scope_partial(u64_t len, i64_t *values, i64_t *indices, u64_t offset, i64_t *pmin, i64_t *pmax) {
    i64_t min, max;
    u64_t i, l;

    l = len + offset;

    if (indices) {
        min = max = values[indices[offset]];
        for (i = offset; i < l; i++) {
            min = values[indices[i]] < min ? values[indices[i]] : min;
            max = values[indices[i]] > max ? values[indices[i]] : max;
        }
    } else {
        min = max = values[offset];
        for (i = offset; i < l; i++) {
            min = values[i] < min ? values[i] : min;
            max = values[i] > max ? values[i] : max;
        }
    }

    *pmin = min;
    *pmax = max;

    return NULL_OBJ;
}

index_scope_t index_scope(i64_t values[], i64_t indices[], u64_t len) {
    u64_t i, chunks, chunk;
    i64_t min, max;
    pool_p pool = pool_get();
    obj_p v;

    if (len == 0)
        return (index_scope_t){NULL_I64, NULL_I64, 0};

    chunks = pool_split_by(pool, len, 0);

    if (chunks == 1)
        index_scope_partial(len, values, indices, 0, &min, &max);
    else {
        i64_t mins[chunks];
        i64_t maxs[chunks];
        pool_prepare(pool);
        chunk = len / chunks;
        for (i = 0; i < chunks - 1; i++)
            pool_add_task(pool, (raw_p)index_scope_partial, 6, chunk, values, indices, i * chunk, &mins[i], &maxs[i]);

        pool_add_task(pool, (raw_p)index_scope_partial, 6, len - i * chunk, values, indices, i * chunk, &mins[i],
                      &maxs[i]);

        v = pool_run(pool);
        drop_obj(v);

        min = max = mins[0];
        for (i = 0; i < chunks; i++) {
            min = mins[i] < min ? mins[i] : min;
            max = maxs[i] > max ? maxs[i] : max;
        }
    }

    timeit_tick("index scope");

    return (index_scope_t){min, max, (u64_t)(max - min + 1)};
}

obj_p index_distinct_i8(i8_t values[], u64_t len, b8_t term) {
    u64_t i, j, range;
    i8_t min, *out;
    obj_p vec;

    min = -128;
    range = 256;

    vec = U8(range);
    out = (i8_t *)AS_U8(vec);
    memset(out, 0, range);

    for (i = 0; i < len; i++) {
        if (out[values[i] - min] == 0)
            out[values[i] - min] = 1;
    }

    // compact keys
    for (i = 0, j = 0; i < range; i++) {
        if (out[i])
            out[j++] = i + min;
    }

    if (term)
        out[j++] = 0;

    resize_obj(&vec, j);
    vec->attrs |= ATTR_DISTINCT;

    return vec;
}

obj_p index_distinct_i64(i64_t values[], u64_t len) {
    u64_t i, j;
    i64_t p, k, *out;
    obj_p vec, set;
    const index_scope_t scope = index_scope(values, NULL, len);

    // use open addressing if range is small
    if (scope.range <= len) {
        vec = I64(scope.range);
        out = AS_I64(vec);
        memset(out, 0, sizeof(i64_t) * scope.range);

        for (i = 0; i < len; i++)
            out[values[i] - scope.min]++;

        // compact keys
        for (i = 0, j = 0; i < scope.range; i++) {
            if (out[i])
                out[j++] = i + scope.min;
        }

        resize_obj(&vec, j);
        vec->attrs |= ATTR_DISTINCT;

        return vec;
    }

    // otherwise, use a hash table
    set = ht_oa_create(len, -1);

    for (i = 0; i < len; i++) {
        k = values[i] - scope.min;
        p = ht_oa_tab_next(&set, k);
        out = AS_I64(AS_LIST(set)[0]);
        if (out[p] == NULL_I64)
            out[p] = k;
    }

    // compact keys
    out = AS_I64(AS_LIST(set)[0]);
    len = AS_LIST(set)[0]->len;
    for (i = 0, j = 0; i < len; i++) {
        if (out[i] != NULL_I64)
            out[j++] = out[i] + scope.min;
    }

    resize_obj(&AS_LIST(set)[0], j);
    vec = clone_obj(AS_LIST(set)[0]);
    vec->attrs |= ATTR_DISTINCT;
    drop_obj(set);

    return vec;
}

obj_p index_distinct_guid(guid_t values[], u64_t len) {
    u64_t i, j;
    i64_t p, *out;
    obj_p vec, set;
    guid_t *g;

    set = ht_oa_create(len, -1);

    for (i = 0, j = 0; i < len; i++) {
        p = ht_oa_tab_next_with(&set, (i64_t)&values[i], &hash_guid, &hash_cmp_guid, NULL);
        out = AS_I64(AS_LIST(set)[0]);
        if (out[p] == NULL_I64) {
            out[p] = (i64_t)&values[i];
            j++;
        }
    }

    vec = GUID(j);
    g = AS_GUID(vec);

    out = AS_I64(AS_LIST(set)[0]);
    len = AS_LIST(set)[0]->len;
    for (i = 0, j = 0; i < len; i++) {
        if (out[i] != NULL_I64)
            memcpy(&g[j++], (guid_t *)out[i], sizeof(guid_t));
    }

    vec->attrs |= ATTR_DISTINCT;
    drop_obj(set);

    return vec;
}

obj_p index_distinct_obj(obj_p values[], u64_t len) {
    u64_t i, j;
    i64_t p, *out;
    obj_p vec, set;

    set = ht_oa_create(len, -1);

    for (i = 0; i < len; i++) {
        p = ht_oa_tab_next_with(&set, (i64_t)values[i], &hash_obj, &hash_cmp_obj, NULL);
        out = AS_I64(AS_LIST(set)[0]);
        if (out[p] == NULL_I64)
            out[p] = (i64_t)clone_obj(values[i]);
    }

    // compact keys
    out = AS_I64(AS_LIST(set)[0]);
    len = AS_LIST(set)[0]->len;
    for (i = 0, j = 0; i < len; i++) {
        if (out[i] != NULL_I64)
            out[j++] = out[i];
    }

    resize_obj(&AS_LIST(set)[0], j);
    vec = clone_obj(AS_LIST(set)[0]);
    vec->attrs |= ATTR_DISTINCT;
    vec->type = TYPE_LIST;
    drop_obj(set);

    return vec;
}

obj_p index_find_i8(i8_t x[], u64_t xl, i8_t y[], u64_t yl) {
    u64_t i, range;
    i64_t min, n, *r, *f;
    obj_p vec;

    min = -128;
    range = 256;

    if (xl == 0)
        return I64(0);

    vec = I64(yl + range);
    r = AS_I64(vec);
    f = r + yl;

    for (i = 0; i < range; i++)
        f[i] = NULL_I64;

    for (i = 0; i < xl; i++) {
        n = x[i] - min;
        f[n] = (f[n] == NULL_I64) ? (i64_t)i : NULL_I64;
    }

    for (i = 0; i < yl; i++) {
        n = y[i] - min;
        r[i] = f[n];
    }

    resize_obj(&vec, yl);

    return vec;
}

obj_p index_find_i64(i64_t x[], u64_t xl, i64_t y[], u64_t yl) {
    u64_t i;
    i64_t n, p, *r, *f;
    obj_p vec, ht;

    if (xl == 0)
        return I64(0);

    const index_scope_t scope = index_scope(x, NULL, xl);

    if (scope.range <= yl) {
        vec = I64(yl + scope.range);
        r = AS_I64(vec);
        f = r + yl;

        for (i = 0; i < scope.range; i++)
            f[i] = NULL_I64;

        for (i = 0; i < xl; i++) {
            n = x[i] - scope.min;
            f[n] = (f[n] == NULL_I64) ? (i64_t)i : NULL_I64;
        }

        for (i = 0; i < yl; i++) {
            n = y[i] - scope.min;
            r[i] = (y[i] < scope.min || y[i] > scope.max) ? NULL_I64 : f[n];
        }

        resize_obj(&vec, yl);

        return vec;
    }

    vec = I64(yl);
    r = AS_I64(vec);

    // otherwise, use a hash table
    ht = ht_oa_create(xl, TYPE_I64);

    for (i = 0; i < xl; i++) {
        p = ht_oa_tab_next(&ht, x[i] - scope.min);
        if (AS_I64(AS_LIST(ht)[0])[p] == NULL_I64) {
            AS_I64(AS_LIST(ht)[0])[p] = x[i] - scope.min;
            AS_I64(AS_LIST(ht)[1])[p] = i;
        }
    }

    for (i = 0; i < yl; i++) {
        p = ht_oa_tab_get(ht, y[i] - scope.min);
        r[i] = p == NULL_I64 ? NULL_I64 : AS_I64(AS_LIST(ht)[1])[p];
    }

    drop_obj(ht);

    return vec;
}

obj_p index_find_guid(guid_t x[], u64_t xl, guid_t y[], u64_t yl) {
    u64_t i, *hashes;
    obj_p ht, res;
    i64_t idx;
    __index_find_ctx_t ctx;

    // calc hashes
    res = I64(MAXU64(xl, yl));
    ht = ht_oa_create(MAXU64(xl, yl) * 2, -1);

    hashes = (u64_t *)AS_I64(res);

    for (i = 0; i < xl; i++)
        hashes[i] = hash_index_u64(*(u64_t *)(x + i), *((u64_t *)(x + i) + 1));

    ctx = (__index_find_ctx_t){.lobj = x, .robj = x, .hashes = hashes};
    for (i = 0; i < xl; i++) {
        idx = ht_oa_tab_next_with(&ht, i, &__hash_get, &__hash_cmp_guid, &ctx);
        if (AS_I64(AS_LIST(ht)[0])[idx] == NULL_I64)
            AS_I64(AS_LIST(ht)[0])[idx] = i;
    }

    for (i = 0; i < yl; i++)
        hashes[i] = hash_index_u64(*(u64_t *)(y + i), *((u64_t *)(y + i) + 1));

    ctx = (__index_find_ctx_t){.lobj = x, .robj = y, .hashes = hashes};
    for (i = 0; i < yl; i++) {
        idx = ht_oa_tab_get_with(ht, i, &__hash_get, &__hash_cmp_guid, &ctx);
        AS_I64(res)[i] = (idx == NULL_I64) ? NULL_I64 : AS_I64(AS_LIST(ht)[0])[idx];
    }

    drop_obj(ht);
    resize_obj(&res, yl);

    return res;
}

obj_p index_find_obj(obj_p x[], u64_t xl, obj_p y[], u64_t yl) {
    u64_t i, *hashes;
    obj_p ht, res;
    i64_t idx;
    __index_find_ctx_t ctx;

    // calc hashes
    res = I64(MAXU64(xl, yl));
    ht = ht_oa_create(MAXU64(xl, yl) * 2, -1);

    hashes = (u64_t *)AS_I64(res);

    for (i = 0; i < xl; i++)
        hashes[i] = hash_index_u64(hash_index_obj(x[i]), 0xa5b6c7d8e9f01234ull);

    ctx = (__index_find_ctx_t){.lobj = x, .robj = x, .hashes = hashes};
    for (i = 0; i < xl; i++) {
        idx = ht_oa_tab_next_with(&ht, i, &__hash_get, &__cmp_obj, &ctx);
        if (AS_I64(AS_LIST(ht)[0])[idx] == NULL_I64)
            AS_I64(AS_LIST(ht)[0])[idx] = i;
    }

    for (i = 0; i < yl; i++)
        hashes[i] = hash_index_u64(hash_index_obj(y[i]), 0xa5b6c7d8e9f01234ull);

    ctx = (__index_find_ctx_t){.lobj = x, .robj = y, .hashes = hashes};
    for (i = 0; i < yl; i++) {
        idx = ht_oa_tab_get_with(ht, i, &__hash_get, &__cmp_obj, &ctx);
        AS_I64(res)[i] = (idx == NULL_I64) ? NULL_I64 : AS_I64(AS_LIST(ht)[0])[idx];
    }

    drop_obj(ht);
    resize_obj(&res, yl);

    return res;
}

u64_t index_group_count(obj_p index) { return (u64_t)AS_LIST(index)[1]->i64; }

i64_t *index_group_ids(obj_p index) {
    if (AS_LIST(index)[2] != NULL_OBJ)
        return AS_I64(AS_LIST(index)[2]);

    return NULL;
}

i64_t *index_group_filter_ids(obj_p index) {
    if (AS_LIST(index)[5]->type == TYPE_I64)
        return AS_I64(AS_LIST(index)[5]);

    return NULL;
}

obj_p index_group_filter(obj_p index) { return AS_LIST(index)[5]; }

u64_t index_group_len(obj_p index) {
    if (AS_LIST(index)[5] != NULL_OBJ)  // filter
        return AS_LIST(index)[5]->len;

    if (AS_LIST(index)[4] != NULL_OBJ)  // source
        return AS_LIST(index)[4]->len;

    return ops_count(AS_LIST(index)[2]);
}

index_type_t index_group_type(obj_p index) { return (index_type_t)AS_LIST(index)[0]->i64; }

i64_t *index_group_source(obj_p index) { return AS_I64(AS_LIST(index)[4]); }

i64_t index_group_shift(obj_p index) { return AS_LIST(index)[3]->i64; }

obj_p index_group_build(index_type_t tp, u64_t groups_count, obj_p group_ids, i64_t index_min, obj_p source,
                        obj_p filter) {
    return vn_list(6, i64(tp), i64(groups_count), group_ids, i64(index_min), source, filter);
}

typedef struct __group_radix_part_ctx_t {
    u64_t partitions;
    u64_t partition;
} *group_radix_part_ctx_p;

obj_p index_group_distribute_partial(group_radix_part_ctx_p ctx, u64_t *groups, i64_t keys[], i64_t filter[],
                                     i64_t out[], u64_t len, hash_f hash, cmp_f cmp) {
    u64_t i, partition_id, partitions, size, partition;
    i64_t *k, *v, n, idx;
    obj_p ht;

    partitions = ctx->partitions;
    partition = ctx->partition;

    size = (partition + 1 < partitions) ? (len / partitions) : (len - (len / partitions) * partition);
    ht = ht_oa_create(size, TYPE_I64);

    if (filter) {
        for (i = 0; i < len; i++) {
            n = keys[filter[i]];

            // determine if the key is ours due to radix partitioning
            partition_id = n % partitions;
            if (partition_id != partition)
                continue;

            idx = ht_oa_tab_next_with(&ht, n, hash, cmp, NULL);
            k = AS_I64(AS_LIST(ht)[0]);
            v = AS_I64(AS_LIST(ht)[1]);

            if (k[idx] == NULL_I64) {
                k[idx] = n;
                v[idx] = __atomic_fetch_add(groups, 1, __ATOMIC_RELAXED);
            }

            out[i] = v[idx];
        }
    } else {
        for (i = 0; i < len; i++) {
            n = keys[i];

            // determine if the key is ours due to radix partitioning
            partition_id = n % partitions;
            if (partition_id != partition)
                continue;

            idx = ht_oa_tab_next_with(&ht, n, hash, cmp, NULL);
            k = AS_I64(AS_LIST(ht)[0]);
            v = AS_I64(AS_LIST(ht)[1]);

            if (k[idx] == NULL_I64) {
                k[idx] = n;
                v[idx] = __atomic_fetch_add(groups, 1, __ATOMIC_RELAXED);
            }

            out[i] = v[idx];
        }
    }

    drop_obj(ht);

    return NULL_OBJ;
}

u64_t index_group_distribute(i64_t keys[], i64_t filter[], i64_t out[], u64_t len, hash_f hash, cmp_f cmp) {
    u64_t i, parts, groups;
    i64_t idx, n, *k, *v;
    pool_p pool;
    obj_p ht, res;

    pool = pool_get();
    parts = pool_split_by(pool, len, 0);
    groups = 0;

    if (parts == 1) {
        ht = ht_oa_create(len, TYPE_I64);

        if (filter) {
            for (i = 0; i < len; i++) {
                n = keys[filter[i]];
                idx = ht_oa_tab_next_with(&ht, n, hash, cmp, NULL);
                k = AS_I64(AS_LIST(ht)[0]);
                v = AS_I64(AS_LIST(ht)[1]);

                if (k[idx] == NULL_I64) {
                    k[idx] = n;
                    v[idx] = groups++;
                }

                out[i] = v[idx];
            }
        } else {
            for (i = 0; i < len; i++) {
                n = keys[i];
                idx = ht_oa_tab_next_with(&ht, n, hash, cmp, NULL);
                k = AS_I64(AS_LIST(ht)[0]);
                v = AS_I64(AS_LIST(ht)[1]);

                if (k[idx] == NULL_I64) {
                    k[idx] = n;
                    v[idx] = groups++;
                }

                out[i] = v[idx];
            }
        }

        drop_obj(ht);

        return groups;
    }

    struct __group_radix_part_ctx_t ctx[parts];

    pool_prepare(pool);
    for (i = 0; i < parts; i++) {
        ctx[i].partitions = parts;
        ctx[i].partition = i;
        pool_add_task(pool, (raw_p)index_group_distribute_partial, 8, &ctx[i], &groups, keys, filter, out, len, hash,
                      cmp);
    }

    res = pool_run(pool);
    drop_obj(res);

    return groups;
}

obj_p index_group_i8(obj_p obj, obj_p filter) {
    u64_t i, j, n, len, range;
    i64_t min, *hk, *hv, *values, *indices;
    obj_p keys, vals;

    values = AS_I64(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : obj->len;

    min = -128;
    range = 256;

    keys = I64(range);
    hk = AS_I64(keys);

    vals = I64(len);
    hv = AS_I64(vals);

    for (i = 0; i < range; i++)
        hk[i] = NULL_I64;

    // distribute bins
    if (indices) {
        for (i = 0, j = 0; i < len; i++) {
            n = values[indices[i]] - min;
            if (hk[n] == NULL_I64)
                hk[n] = j++;

            hv[i] = hk[n];
        }
    } else {
        for (i = 0, j = 0; i < len; i++) {
            n = values[i] - min;
            if (hk[n] == NULL_I64)
                hk[n] = j++;

            hv[i] = hk[n];
        }
    }

    drop_obj(keys);

    return index_group_build(INDEX_TYPE_IDS, j, vals, NULL_I64, NULL_OBJ, clone_obj(filter));
}

obj_p index_group_i64_unscoped(obj_p obj, obj_p filter) {
    u64_t len;
    i64_t *out, *values, *indices, g;
    obj_p vals;

    values = AS_I64(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : obj->len;

    // use hash table if range is large
    vals = I64(len);
    out = AS_I64(vals);

    g = index_group_distribute(values, indices, out, len, &hash_fnv1a, &hash_cmp_i64);

    timeit_tick("index group unscoped");

    return index_group_build(INDEX_TYPE_IDS, g, vals, NULL_I64, NULL_OBJ, clone_obj(filter));
}

obj_p index_group_i64_scoped_partial(i64_t input[], i64_t filter[], i64_t group_ids[], u64_t len, u64_t offset,
                                     i64_t min, i64_t out[]) {
    u64_t i, l;

    l = len + offset;

    if (filter != NULL) {
        for (i = offset; i < l; i++)
            out[i] = group_ids[input[filter[i]] - min];
    } else {
        for (i = offset; i < l; i++)
            out[i] = group_ids[input[i] - min];
    }

    return NULL_OBJ;
}

obj_p index_group_i64_scoped(obj_p obj, obj_p filter, const index_scope_t scope) {
    u64_t i, n, len, groups, chunks, chunk;
    i64_t *hk, *hv, *values, *indices;
    obj_p keys, vals, v;
    pool_p pool;

    values = AS_I64(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : obj->len;

    // perfect hash
    if (scope.range <= len) {
        keys = I64(scope.range);
        hk = AS_I64(keys);

        for (i = 0; i < scope.range; i++)
            hk[i] = NULL_I64;

        if (indices) {
            for (i = 0, groups = 0; i < len; i++) {
                n = values[indices[i]] - scope.min;
                if (hk[n] == NULL_I64)
                    hk[n] = groups++;
            }
        } else {
            for (i = 0, groups = 0; i < len; i++) {
                n = values[i] - scope.min;
                if (hk[n] == NULL_I64)
                    hk[n] = groups++;
            }
        }

        //  do not compute group indices as they can be obtained from the keys
        if (scope.range <= INDEX_SCOPE_LIMIT) {
            timeit_tick("index group scoped perfect simple");
            return index_group_build(INDEX_TYPE_SHIFT, groups, keys, scope.min, clone_obj(obj), clone_obj(filter));
        }

        vals = I64(len);
        hv = AS_I64(vals);

        pool = pool_get();
        chunks = pool_split_by(pool, len, 0);

        if (chunks == 1)
            index_group_i64_scoped_partial(values, indices, hk, len, 0, scope.min, hv);
        else {
            pool_prepare(pool);
            chunk = len / chunks;
            for (i = 0; i < chunks - 1; i++)
                pool_add_task(pool, (raw_p)index_group_i64_scoped_partial, 7, values, indices, hk, chunk, i * chunk,
                              scope.min, hv);

            pool_add_task(pool, (raw_p)index_group_i64_scoped_partial, 7, values, indices, hk, len - i * chunk,
                          i * chunk, scope.min, hv);

            v = pool_run(pool);
            drop_obj(v);
        }

        drop_obj(keys);

        timeit_tick("index group scoped perfect");

        return index_group_build(INDEX_TYPE_IDS, groups, vals, NULL_I64, NULL_OBJ, clone_obj(filter));
    }

    return index_group_i64_unscoped(obj, filter);
}

obj_p index_group_i64(obj_p obj, obj_p filter) {
    index_scope_t scope;
    i64_t *values, *indices;
    u64_t len;

    values = AS_I64(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : obj->len;

    scope = index_scope(values, indices, len);

    return index_group_i64_scoped(obj, filter, scope);
}

obj_p index_group_f64(obj_p obj, obj_p filter) { return index_group_i64_unscoped(obj, filter); }

obj_p index_group_guid(obj_p obj, obj_p filter) {
    u64_t i, j, len;
    i64_t idx, *hk, *hv, *hp, *indices;
    guid_t *values;
    obj_p vals, ht;

    values = AS_GUID(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : obj->len;

    ht = ht_oa_create(len, TYPE_I64);
    vals = I64(len);
    hp = AS_I64(vals);

    // distribute bins
    if (indices) {
        for (i = 0, j = 0; i < len; i++) {
            idx = ht_oa_tab_next_with(&ht, (i64_t)&values[indices[i]], &hash_guid, &hash_cmp_guid, NULL);
            hk = AS_I64(AS_LIST(ht)[0]);
            hv = AS_I64(AS_LIST(ht)[1]);
            if (hk[idx] == NULL_I64) {
                hk[idx] = (i64_t)&values[indices[i]];
                hv[idx] = j++;
            }

            hp[i] = hv[idx];
        }
    } else {
        for (i = 0, j = 0; i < len; i++) {
            idx = ht_oa_tab_next_with(&ht, (i64_t)&values[i], &hash_guid, &hash_cmp_guid, NULL);
            hk = AS_I64(AS_LIST(ht)[0]);
            hv = AS_I64(AS_LIST(ht)[1]);
            if (hk[idx] == NULL_I64) {
                hk[idx] = (i64_t)&values[i];
                hv[idx] = j++;
            }

            hp[i] = hv[idx];
        }
    }

    drop_obj(ht);

    return index_group_build(INDEX_TYPE_IDS, j, vals, NULL_I64, NULL_OBJ, clone_obj(filter));
}

obj_p index_group_obj(obj_p obj, obj_p filter) {
    u64_t g, len;
    i64_t *out, *indices, *values;
    obj_p vals;

    values = (i64_t *)AS_LIST(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : obj->len;

    vals = I64(len);
    out = AS_I64(vals);

    g = index_group_distribute(values, indices, out, len, &hash_obj, &hash_cmp_obj);

    return index_group_build(INDEX_TYPE_IDS, g, vals, NULL_I64, NULL_OBJ, clone_obj(filter));
}

obj_p index_group(obj_p val, obj_p filter) {
    u64_t i, l, g;
    obj_p bins, v;

    switch (val->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            return index_group_i8(val, filter);
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            return index_group_i64(val, filter);
        case TYPE_F64:
            return index_group_f64(val, filter);
        case TYPE_GUID:
            return index_group_guid(val, filter);
        case TYPE_ENUM:
            return index_group_i64(ENUM_VAL(val), filter);
        case TYPE_LIST:
            return index_group_obj(val, filter);
        case TYPE_MAPLIST:
            v = ray_value(val);
            bins = index_group_obj(v, filter);
            drop_obj(v);
            return bins;
        case TYPE_MAPCOMMON:
            g = 0;
            if (filter->type == TYPE_PARTEDI64) {
                l = filter->len;
                for (i = 0; i < l; i++) {
                    if (AS_LIST(filter)[i] != NULL_OBJ)
                        g++;
                }
            } else {
                g = AS_LIST(val)[0]->len;
            }

            return index_group_build(INDEX_TYPE_PARTEDCOMMON, g, clone_obj(val), NULL_I64, NULL_OBJ, clone_obj(filter));
        default:
            THROW(ERR_TYPE, "'index group' unable to group by: %s", type_name(val->type));
    }
}

obj_p index_group_list_perfect(obj_p obj, obj_p filter) {
    u8_t *xb;
    u64_t i, j, l, len, product;
    i64_t *xi, *xo, *indices;
    obj_p ht, col, res, *values;
    index_scope_t *scopes, scope;

    l = obj->len;
    u64_t multipliers[l];

    if (l == 0)
        return NULL_OBJ;

    values = AS_LIST(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : values[0]->len;

    // First, check if columns types are suitable for perfect hashing
    for (i = 0; i < l; i++) {
        switch (values[i]->type) {
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
    for (i = 0; i < l; i++) {
        switch (values[i]->type) {
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
                scopes[i] = index_scope(AS_I64(values[i]), indices, len);
                break;
            default:
                // because we already checked the types, this should never happen
                __builtin_unreachable();
        }
    }

    // Precompute multipliers for each column
    for (i = 0, product = 1, scope = (index_scope_t){0, 0, 0}; i < l; i++) {
        if (ULLONG_MAX / product < scopes[i].range) {
            heap_free(scopes);
            return NULL_OBJ;  // Overflow would occur
        }

        scope.max += (scopes[i].max - scopes[i].min) * product;

        multipliers[i] = product;
        product *= scopes[i].range;
    }

    scope.range = scope.max + 1;

    ht = I64(len);
    xo = AS_I64(ht);
    memset(xo, 0, len * sizeof(i64_t));

    for (i = 0; i < l; i++) {
        col = values[i];
        switch (col->type) {
            case TYPE_B8:
            case TYPE_U8:
            case TYPE_C8:
                xb = AS_U8(col);
                if (indices) {
                    for (j = 0; j < len; j++)
                        xo[j] += (xb[indices[j]] - scopes[i].min) * multipliers[i];
                } else {
                    for (j = 0; j < len; j++)
                        xo[j] += (xb[j] - scopes[i].min) * multipliers[i];
                }
                break;
            case TYPE_I64:
            case TYPE_SYMBOL:
            case TYPE_TIMESTAMP:
                xi = AS_I64(col);
                if (indices) {
                    for (j = 0; j < len; j++)
                        xo[j] += (xi[indices[j]] - scopes[i].min) * multipliers[i];
                } else {
                    for (j = 0; j < len; j++)
                        xo[j] += (xi[j] - scopes[i].min) * multipliers[i];
                }
                break;
            case TYPE_ENUM:
                xi = AS_I64(ENUM_VAL(col));
                if (indices) {
                    for (j = 0; j < len; j++)
                        xo[j] += (xi[indices[j]] - scopes[i].min) * multipliers[i];
                } else {
                    for (j = 0; j < len; j++)
                        xo[j] += (xi[j] - scopes[i].min) * multipliers[i];
                }
                break;
            case TYPE_F64:
                xi = (i64_t *)AS_F64(col);
                if (indices) {
                    for (j = 0; j < len; j++)
                        xo[j] += (xi[indices[j]] - scopes[i].min) * multipliers[i];
                } else {
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
    res = index_group_i64_scoped(ht, NULL_OBJ, scope);
    drop_obj(ht);

    return res;
}

obj_p index_group_list(obj_p obj, obj_p filter) {
    u64_t i, len;
    i64_t g, v, *xo, *indices;
    obj_p res, *values, ht;
    __index_list_ctx_t ctx;

    if (ops_count(obj) == 0)
        return error(ERR_LENGTH, "group index list: empty source");

    // If the list values range is small, use perfect hashing
    res = index_group_list_perfect(obj, filter);
    if (!is_null(res)) {
        timeit_tick("group index list perfect");
        return res;
    }

    values = AS_LIST(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : values[0]->len;

    ht = ht_oa_create(len, TYPE_I64);

    res = I64(len);
    xo = AS_I64(res);

    __index_list_precalc_hash(obj, (u64_t *)xo, obj->len, len, indices, B8_FALSE);
    timeit_tick("group index precalc hash");

    ctx = (__index_list_ctx_t){.lcols = obj, .rcols = obj, .hashes = (u64_t *)xo, .filter = indices};

    // NOTE: We can reuse the same vector for output indices, that is used for hashes, because
    // it's guaranteed do not rehash the table caus ewe reserved enough space for it

    // distribute bins
    for (i = 0, g = 0; i < len; i++) {
        v = ht_oa_tab_insert_with(&ht, i, g, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        if (v == g)
            g++;

        xo[i] = v;
    }

    drop_obj(ht);

    timeit_tick("group index list");

    return index_group_build(INDEX_TYPE_IDS, g, res, NULL_I64, NULL_OBJ, clone_obj(filter));
}

obj_p index_join_obj(obj_p lcols, obj_p rcols, u64_t len) {
    u64_t i, ll, rl;
    obj_p ht, res;
    i64_t idx;
    __index_list_ctx_t ctx;

    if (len == 1) {
        res = ray_find(rcols, lcols);
        if (res->type == -TYPE_I64) {
            idx = res->i64;
            drop_obj(res);
            res = I64(1);
            AS_I64(res)[0] = idx;
        }

        return res;
    }

    ll = ops_count(AS_LIST(lcols)[0]);
    rl = ops_count(AS_LIST(rcols)[0]);
    ht = ht_oa_create(MAXU64(ll, rl), -1);
    res = I64(MAXU64(ll, rl));

    // Right hashes
    __index_list_precalc_hash(rcols, (u64_t *)AS_I64(res), len, rl, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, rcols, (u64_t *)AS_I64(res), NULL};
    for (i = 0; i < rl; i++) {
        idx = ht_oa_tab_next_with(&ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        if (AS_I64(AS_LIST(ht)[0])[idx] == NULL_I64)
            AS_I64(AS_LIST(ht)[0])[idx] = i;
    }

    // Left hashes
    __index_list_precalc_hash(lcols, (u64_t *)AS_I64(res), len, ll, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, lcols, (u64_t *)AS_I64(res), NULL};
    for (i = 0; i < ll; i++) {
        idx = ht_oa_tab_get_with(ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        AS_I64(res)[i] = (idx == NULL_I64) ? NULL_I64 : AS_I64(AS_LIST(ht)[0])[idx];
    }

    drop_obj(ht);

    return res;
}
