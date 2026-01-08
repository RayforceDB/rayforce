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
#include "pool.h"
#include "def.h"

const i64_t MAX_RANGE = 1 << 20;

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

// Optimized row comparison - inlines common i64 case to avoid function call overhead
i64_t __index_list_cmp_row(i64_t row1, i64_t row2, raw_p seed) {
    i64_t i, l, r1, r2;
    __index_list_ctx_t *ctx = (__index_list_ctx_t *)seed;
    i64_t *filter = ctx->filter;
    obj_p *lcols = AS_LIST(ctx->lcols);
    obj_p *rcols = AS_LIST(ctx->rcols);
    obj_p lc, rc;

    l = ctx->lcols->len;
    r1 = filter ? filter[row1] : row1;
    r2 = filter ? filter[row2] : row2;

    // Fast path: check if all columns are i64-like types for direct comparison
    for (i = 0; i < l; i++) {
        lc = lcols[i];
        rc = rcols[i];
        
        // Inline comparison for common i64 types (avoid ops_eq_idx call overhead)
        switch (lc->type) {
            case TYPE_I64:
            case TYPE_SYMBOL:
            case TYPE_TIMESTAMP:
                if (AS_I64(lc)[r1] != AS_I64(rc)[r2])
                    return 1;
                break;
            case TYPE_I32:
            case TYPE_DATE:
            case TYPE_TIME:
                if (AS_I32(lc)[r1] != AS_I32(rc)[r2])
                    return 1;
                break;
            case TYPE_U8:
            case TYPE_B8:
            case TYPE_C8:
                if (AS_U8(lc)[r1] != AS_U8(rc)[r2])
                    return 1;
                break;
            default:
                // Fall back to generic comparison for other types
                if (ops_eq_idx(lc, r1, rc, r2) == 0)
                    return 1;
                break;
        }
    }

    return 0;
}

obj_p index_hash_obj_partial(obj_p obj, i64_t out[], i64_t filter[], i64_t len, i64_t offset, b8_t resolve) {
    u8_t *u8v;
    i32_t *i32v;
    guid_t *g64v;
    i64_t i, *u64v;
    obj_p k, v, *l64v;
    i64_t *ids;

    switch (obj->type) {
        case -TYPE_B8:
        case -TYPE_U8:
        case -TYPE_C8:
            out[offset] = hash_index_u64((i64_t)obj->u8, out[offset]);
            break;
        case -TYPE_I64:
        case -TYPE_SYMBOL:
        case -TYPE_TIMESTAMP:
            out[offset] = hash_index_u64((i64_t)obj->i64, out[offset]);
            break;
        case -TYPE_F64:
            out[offset] = hash_index_u64((i64_t)obj->f64, out[offset]);
            break;
        case -TYPE_GUID:
            out[offset] = hash_index_u64(*(i64_t *)AS_GUID(obj), *((i64_t *)AS_GUID(obj) + 1));
            break;
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            u8v = AS_U8(obj);
            if (filter)
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64((i64_t)u8v[filter[i]], out[i]);
            else
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64((i64_t)u8v[i], out[i]);
            break;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            i32v = (i32_t *)AS_I32(obj);
            if (filter) {
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64((i64_t)i32v[filter[i]], out[i]);
            } else {
                // Scalar loop - i32 to i64 conversion doesn't benefit much from SIMD
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64((i64_t)i32v[i], out[i]);
            }
            break;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            u64v = (i64_t *)AS_I64(obj);
            if (filter) {
                for (i = offset; i < len + offset; i++)
                    out[i] = hash_index_u64(u64v[filter[i]], out[i]);
            } else {
                // Use best available SIMD (AVX2/NEON) or scalar fallback
                hash_index_i64_batch(out + offset, u64v + offset, len);
            }
            break;
        case TYPE_F64:
            u64v = (i64_t *)AS_F64(obj);
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
                for (i = offset; i < len + offset; i++) {
                    out[i] = hash_index_u64(*(i64_t *)&g64v[filter[i]], out[i]);
                    out[i] = hash_index_u64(*((i64_t *)&g64v[filter[i]] + 1), out[i]);
                }
            else
                for (i = offset; i < len + offset; i++) {
                    out[i] = hash_index_u64(*(i64_t *)&g64v[i], out[i]);
                    out[i] = hash_index_u64(*((i64_t *)&g64v[i] + 1), out[i]);
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
                u64v = (i64_t *)AS_SYMBOL(v);
                ids = AS_I64(ENUM_VAL(obj));
                if (filter)
                    for (i = offset; i < len + offset; i++)
                        out[i] = hash_index_u64(u64v[ids[filter[i]]], out[i]);
                else
                    for (i = offset; i < len + offset; i++)
                        out[i] = hash_index_u64(u64v[ids[i]], out[i]);
                drop_obj(v);
            } else {
                u64v = (i64_t *)AS_I64(ENUM_VAL(obj));
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
            PANIC("type: %d", obj->type);
    }

    return NULL_OBJ;
}

nil_t __index_list_precalc_hash(obj_p cols, i64_t out[], i64_t ncols, i64_t nrows, i64_t filter[], b8_t resolve) {
    i64_t i, j, chunks, base_chunk;
    pool_p pool;
    obj_p v;

    pool = pool_get();
    chunks = pool_split_by(pool, nrows, 0);
    base_chunk = pool_chunk_aligned(nrows, chunks, sizeof(i64_t));

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
                pool_add_task(pool, (raw_p)index_hash_obj_partial, 6, AS_LIST(cols)[i], out, filter, base_chunk,
                              j * base_chunk, resolve);
            pool_add_task(pool, (raw_p)index_hash_obj_partial, 6, AS_LIST(cols)[i], out, filter,
                          nrows - (chunks - 1) * base_chunk, (chunks - 1) * base_chunk, resolve);
            v = pool_run(pool);
            drop_obj(v);
        }
    }
}

nil_t index_hash_obj(obj_p obj, i64_t out[], i64_t filter[], i64_t len, b8_t resolve) {
    index_hash_obj_partial(obj, out, filter, len, 0, resolve);
}

obj_p index_scope_partial_i32(i64_t len, i32_t *values, i64_t *indices, i64_t offset, i64_t *pmin, i64_t *pmax) {
    i32_t min, max;
    i64_t i, l;

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

index_scope_t index_scope_i32(i32_t values[], i64_t indices[], i64_t len) {
    i64_t i, chunks, base_chunk;
    i64_t min, max;
    pool_p pool = pool_get();
    obj_p v;

    if (len == 0)
        return (index_scope_t){NULL_I64, NULL_I64, 0};

    chunks = pool_split_by(pool, len, 0);
    base_chunk = pool_chunk_aligned(len, chunks, sizeof(i32_t));

    if (chunks == 1)
        index_scope_partial_i32(len, values, indices, 0, &min, &max);
    else {
        i64_t mins[chunks];
        i64_t maxs[chunks];
        pool_prepare(pool);
        for (i = 0; i < chunks - 1; i++)
            pool_add_task(pool, (raw_p)index_scope_partial_i32, 6, base_chunk, values, indices, i * base_chunk,
                          &mins[i], &maxs[i]);
        pool_add_task(pool, (raw_p)index_scope_partial_i32, 6, len - (chunks - 1) * base_chunk, values, indices,
                      (chunks - 1) * base_chunk, &mins[chunks - 1], &maxs[chunks - 1]);
        v = pool_run(pool);
        drop_obj(v);
        min = max = mins[0];
        for (i = 0; i < chunks; i++) {
            min = mins[i] < min ? mins[i] : min;
            max = maxs[i] > max ? maxs[i] : max;
        }
    }
    timeit_tick("index scope");
    return (index_scope_t){min, max, (i64_t)(max - min + 1)};
}

obj_p index_scope_partial_i64(i64_t len, i64_t *values, i64_t *indices, i64_t offset, i64_t *pmin, i64_t *pmax) {
    i64_t min, max;
    i64_t i, l;

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

index_scope_t index_scope_i64(i64_t values[], i64_t indices[], i64_t len) {
    i64_t i, chunks, base_chunk;
    i64_t min, max;
    pool_p pool = pool_get();
    obj_p v;

    if (len == 0)
        return (index_scope_t){NULL_I64, NULL_I64, 0};

    chunks = pool_split_by(pool, len, 0);
    base_chunk = pool_chunk_aligned(len, chunks, sizeof(i64_t));

    if (chunks == 1)
        index_scope_partial_i64(len, values, indices, 0, &min, &max);
    else {
        i64_t mins[chunks];
        i64_t maxs[chunks];
        pool_prepare(pool);
        for (i = 0; i < chunks - 1; i++)
            pool_add_task(pool, (raw_p)index_scope_partial_i64, 6, base_chunk, values, indices, i * base_chunk,
                          &mins[i], &maxs[i]);
        pool_add_task(pool, (raw_p)index_scope_partial_i64, 6, len - (chunks - 1) * base_chunk, values, indices,
                      (chunks - 1) * base_chunk, &mins[chunks - 1], &maxs[chunks - 1]);
        v = pool_run(pool);
        drop_obj(v);
        min = max = mins[0];
        for (i = 0; i < chunks; i++) {
            min = mins[i] < min ? mins[i] : min;
            max = maxs[i] > max ? maxs[i] : max;
        }
    }
    timeit_tick("index scope");
    return (index_scope_t){min, max, (i64_t)(max - min + 1)};
}

obj_p index_distinct_i8(i8_t values[], i64_t len) {
    i64_t i, j, range;
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

    resize_obj(&vec, j);
    vec->attrs |= ATTR_DISTINCT;

    return vec;
}

obj_p index_distinct_i16(i16_t values[], i64_t len) {
    i64_t i, j, range;
    i16_t min, *out;
    obj_p vec;

    min = -32768;
    range = 65536;

    vec = I16(range);
    out = AS_I16(vec);
    memset(out, 0, sizeof(i16_t) * range);

    for (i = 0; i < len; i++)
        // TODO need bench // if (out[values[i] - min] == 0)
        out[values[i] - min] = 1;

    // compact keys
    for (i = 0, j = 0; i < range; i++) {
        if (out[i])
            out[j++] = i + min;
    }

    resize_obj(&vec, j);
    vec->attrs |= ATTR_DISTINCT;

    return vec;
}

obj_p index_distinct_i32(i32_t values[], i64_t len) {
    i64_t i, j, l;
    i32_t *out;
    i64_t p, *keys;
    obj_p vec, set;
    const index_scope_t scope = index_scope_i32(values, NULL, len);

    if (scope.range <= len || scope.range <= MAX_RANGE) {
        vec = I32(scope.range);
        out = AS_I32(vec);
        memset(out, 0, sizeof(i32_t) * scope.range);

        for (i = 0; i < len; i++)
            out[values[i] - scope.min] = 1;

        // compact values
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

    for (i = 0, j = 0; i < len; i++) {
        if (values[i] == NULL_I32)
            continue;
        p = ht_oa_tab_next(&set, (i64_t)values[i]);
        keys = AS_I64(AS_LIST(set)[0]);
        if (keys[p] == NULL_I64) {
            keys[p] = (i64_t)values[i];
            j++;
        }
    }

    vec = I32(j);
    out = AS_I32(vec);

    keys = AS_I64(AS_LIST(set)[0]);
    l = AS_LIST(set)[0]->len;

    for (i = 0, j = 0; i < l; i++) {
        if (keys[i] != NULL_I64)
            out[j++] = (i32_t)keys[i];
    }

    drop_obj(set);
    vec->attrs |= ATTR_DISTINCT;
    return vec;
}

obj_p index_distinct_i64(i64_t values[], i64_t len) {
    i64_t i, l, j = 0;
    i64_t p, *out, *keys;
    obj_p vec, set;
    const index_scope_t scope = index_scope_i64(values, NULL, len);

    // use open addressing if range is small
    if (scope.range <= len || scope.range <= MAX_RANGE) {
        vec = I64(scope.range);
        out = AS_I64(vec);
        memset(out, 0, sizeof(i64_t) * scope.range);

        for (i = 0; i < len; i++)
            // TODO need bench // if (out[values[i] - scope.min] == 0)
            out[values[i] - scope.min] = 1;

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
        if (values[i] == NULL_I64)
            continue;
        p = ht_oa_tab_next(&set, values[i]);
        out = AS_I64(AS_LIST(set)[0]);
        if (out[p] == NULL_I64) {
            out[p] = values[i];
            j++;
        }
    }

    vec = I64(j);
    out = AS_I64(vec);

    keys = AS_I64(AS_LIST(set)[0]);
    l = AS_LIST(set)[0]->len;

    for (i = 0, j = 0; i < l; i++) {
        if (keys[i] != NULL_I64)
            out[j++] = keys[i];
    }

    drop_obj(set);
    vec->attrs |= ATTR_DISTINCT;
    return vec;
}

obj_p index_distinct_guid(guid_t values[], i64_t len) {
    i64_t i, j;
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

obj_p index_distinct_obj(obj_p values[], i64_t len) {
    i64_t i, j;
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

obj_p index_in_i8_i8(i8_t x[], i64_t xl, i8_t y[], i64_t yl) {
    i64_t i, range;
    i8_t min, *s, *r;
    obj_p set, vec;

    min = -128;
    range = 256;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++)
        s[y[i] - min] = B8_TRUE;

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++)
        r[i] = s[x[i] - min];

    drop_obj(set);

    return vec;
}

obj_p index_in_i8_i16(i8_t x[], i64_t xl, i16_t y[], i64_t yl) {
    i64_t i, range;
    i16_t val;
    i8_t min, *s, *r;
    obj_p set, vec;

    min = -128;
    range = 256;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++) {
        val = y[i] - min;
        if (val >= 0 && val < (i16_t)range)
            s[val] = B8_TRUE;
    }

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++)
        r[i] = s[x[i] - min];

    drop_obj(set);

    return vec;
}

obj_p index_in_i8_i32(i8_t x[], i64_t xl, i32_t y[], i64_t yl) {
    i64_t i, range;
    i32_t val;
    i8_t min, *s, *r;
    obj_p set, vec;

    min = -128;
    range = 256;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++) {
        val = y[i] - min;
        if (val >= 0 && val < (i32_t)range)
            s[val] = B8_TRUE;
    }

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++)
        r[i] = s[x[i] - min];

    drop_obj(set);

    return vec;
}

obj_p index_in_i8_i64(i8_t x[], i64_t xl, i64_t y[], i64_t yl) {
    i64_t i, range;
    i64_t val;
    i8_t min, *s, *r;
    obj_p set, vec;

    min = -128;
    range = 256;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++) {
        val = y[i] - min;
        if (val >= 0 && val < (i64_t)range)
            s[val] = B8_TRUE;
    }

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++)
        r[i] = s[x[i] - min];

    drop_obj(set);

    return vec;
}

obj_p index_in_i16_i8(i16_t x[], i64_t xl, i8_t y[], i64_t yl) {
    i64_t i, range;
    i16_t val;
    i8_t min, *s, *r;
    obj_p set, vec;

    min = -128;
    range = 256;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++)
        s[y[i] - min] = B8_TRUE;

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++) {
        val = x[i] - min;
        if (val >= 0 && val < (i16_t)range)
            r[i] = s[val];
        else
            r[i] = B8_FALSE;
    }

    drop_obj(set);

    return vec;
}

obj_p index_in_i16_i16(i16_t x[], i64_t xl, i16_t y[], i64_t yl) {
    i64_t i, range;
    i8_t *s, *r;
    i16_t min;
    obj_p set, vec;

    min = -32768;
    range = 65536;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++)
        s[y[i] - min] = B8_TRUE;

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++)
        r[i] = s[x[i] - min];

    drop_obj(set);

    return vec;
}

obj_p index_in_i16_i32(i16_t x[], i64_t xl, i32_t y[], i64_t yl) {
    i64_t i, range;
    i32_t min, val;
    i8_t *s, *r;
    obj_p set, vec;

    min = -32768;
    range = 65536;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++) {
        if (y[i] == NULL_I32)
            s[0] = B8_TRUE;
        else {
            val = y[i] - min;
            if (val > 0 && val < (i32_t)range)
                s[val] = B8_TRUE;
        }
    }

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++)
        r[i] = s[x[i] - min];

    drop_obj(set);

    return vec;
}

obj_p index_in_i16_i64(i16_t x[], i64_t xl, i64_t y[], i64_t yl) {
    i64_t i, range;
    i64_t val;
    i16_t min;
    i8_t *s, *r;
    obj_p set, vec;

    min = -32768;
    range = 65536;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++) {
        if (y[i] == NULL_I64)
            s[0] = B8_TRUE;
        else {
            val = y[i] - min;
            if (val > 0 && val < (i64_t)range)
                s[val] = B8_TRUE;
        }
    }

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++)
        r[i] = s[x[i] - min];

    drop_obj(set);

    return vec;
}

obj_p index_in_i32_i8(i32_t x[], i64_t xl, i8_t y[], i64_t yl) {
    i64_t i, range;
    i32_t val;
    i8_t min, *s, *r;
    obj_p set, vec;

    min = -128;
    range = 256;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++)
        val = y[i] - min;

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++) {
        val = x[i] - min;
        if (val >= 0 && val < (i32_t)range)
            r[i] = s[val];
        else
            r[i] = B8_FALSE;
    }

    drop_obj(set);

    return vec;
}

obj_p index_in_i32_i16(i32_t x[], i64_t xl, i16_t y[], i64_t yl) {
    i64_t i, range;
    i32_t min, val;
    i8_t *s, *r;
    obj_p set, vec;

    min = -32768;
    range = 65536;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++)
        s[y[i] - min] = B8_TRUE;

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++) {
        if (x[i] == NULL_I32)
            r[i] = s[0];
        else {
            val = x[i] - min;
            if (val > 0 && val < (i32_t)range)
                r[i] = s[val];
            else
                r[i] = B8_FALSE;
        }
    }

    drop_obj(set);

    return vec;
}

obj_p index_in_i32_i32(i32_t x[], i64_t xl, i32_t y[], i64_t yl) {
    i64_t i, range;
    i64_t val, min, max;
    obj_p vec, set;
    i8_t *s, *r;

    if (xl == 0)
        return B8(0);

    const index_scope_t scope_x = index_scope_i32(x, NULL, xl);
    const index_scope_t scope_y = index_scope_i32(y, NULL, yl);

    min = (scope_x.min > scope_y.min) ? scope_x.min : scope_y.min;
    max = (scope_x.max < scope_y.max) ? scope_x.max : scope_y.max;

    vec = B8(xl);
    r = AS_B8(vec);

    if (min > max) {
        memset(r, 0, xl);
        return vec;
    }

    range = max - min + 1;

    if (range <= MAX_RANGE) {
        set = B8(range);
        s = AS_B8(set);
        memset(s, 0, range);

        for (i = 0; i < yl; i++) {
            val = y[i] - min;
            if (val >= 0 && val < (i64_t)range)
                s[val] = B8_TRUE;
        }

        for (i = 0; i < xl; i++) {
            val = x[i] - min;
            if (val >= 0 && val < (i64_t)range)
                r[i] = s[val];
            else
                r[i] = B8_FALSE;
        }

        drop_obj(set);

        return vec;
    }

    // otherwise, use a hash table
    set = ht_oa_create(xl, -1);

    for (i = 0; i < yl; i++) {
        val = ht_oa_tab_next(&set, (i64_t)y[i]);
        AS_I64(AS_LIST(set)[0])[val] = (i64_t)y[i];
    }

    for (i = 0; i < xl; i++) {
        val = ht_oa_tab_get(set, (i64_t)x[i]);
        r[i] = val != NULL_I64;
    }

    drop_obj(set);

    return vec;
}

obj_p index_in_i32_i64(i32_t x[], i64_t xl, i64_t y[], i64_t yl) {
    i64_t i, range;
    i64_t val, min, max;
    obj_p vec, set;
    i8_t *s, *r;

    if (xl == 0)
        return B8(0);

    const index_scope_t scope_x = index_scope_i32(x, NULL, xl);
    const index_scope_t scope_y = index_scope_i64(y, NULL, yl);

    min = (scope_x.min > scope_y.min) ? scope_x.min : scope_y.min;
    max = (scope_x.max < scope_y.max) ? scope_x.max : scope_y.max;

    vec = B8(xl);
    r = AS_B8(vec);

    if (min > max) {
        memset(r, 0, xl);
        return vec;
    }

    range = max - min + 1;

    if (range <= MAX_RANGE) {
        set = B8(range);
        s = AS_B8(set);
        memset(s, 0, range);

        for (i = 0; i < yl; i++) {
            val = y[i] - min;
            if (val >= 0 && val < (i64_t)range)
                s[val] = B8_TRUE;
        }

        for (i = 0; i < xl; i++) {
            val = x[i] - min;
            if (val >= 0 && val < (i64_t)range)
                r[i] = s[val];
            else
                r[i] = B8_FALSE;
        }

        drop_obj(set);

        return vec;
    }

    // otherwise, use a hash table
    set = ht_oa_create(xl, -1);

    for (i = 0; i < yl; i++)
        if (y[i] == NULL_I64)
            AS_I64(AS_LIST(set)[0])[ht_oa_tab_next(&set, (i64_t)NULL_I32)] = (i64_t)NULL_I32;
        else if (y[i] > (i64_t)NULL_I32 && y[i] <= (i64_t)INF_I32)
            AS_I64(AS_LIST(set)[0])[ht_oa_tab_next(&set, y[i])] = y[i];

    for (i = 0; i < xl; i++)
        r[i] = ht_oa_tab_get(set, (i64_t)x[i]) != NULL_I64;

    drop_obj(set);

    return vec;
}

obj_p index_in_i64_i8(i64_t x[], i64_t xl, i8_t y[], i64_t yl) {
    i64_t i, range;
    i64_t val;
    i8_t min;
    b8_t *s, *r;
    obj_p set, vec;

    min = -128;
    range = 256;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++)
        s[y[i] - min] = B8_TRUE;

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++) {
        val = x[i] - min;
        if (val >= 0 && val < (i64_t)range)
            r[i] = s[val];
        else
            r[i] = B8_FALSE;
    }

    drop_obj(set);

    return vec;
}

obj_p index_in_i64_i16(i64_t x[], i64_t xl, i16_t y[], i64_t yl) {
    i64_t i, range;
    i64_t min, val;
    b8_t *s, *r;
    obj_p set, vec;

    min = -32768;
    range = 65536;

    if (xl == 0)
        return B8(0);

    set = B8(range);
    s = AS_B8(set);
    memset(s, 0, range);

    for (i = 0; i < yl; i++)
        s[y[i] - min] = B8_TRUE;

    vec = B8(xl);
    r = AS_B8(vec);

    for (i = 0; i < xl; i++) {
        if (x[i] == NULL_I64)
            r[i] = s[0];
        else {
            val = x[i] - min;
            if (val > 0 && val < (i64_t)range)
                r[i] = s[val];
            else
                r[i] = B8_FALSE;
        }
    }

    drop_obj(set);

    return vec;
}

obj_p index_in_i64_i32(i64_t x[], i64_t xl, i32_t y[], i64_t yl) {
    i64_t i, range;
    i64_t val, min, max;
    obj_p vec, set;
    i8_t *s, *r;

    if (xl == 0)
        return B8(0);

    const index_scope_t scope_x = index_scope_i64(x, NULL, xl);
    const index_scope_t scope_y = index_scope_i32(y, NULL, yl);

    min = (scope_x.min > scope_y.min) ? scope_x.min : scope_y.min;
    max = (scope_x.max < scope_y.max) ? scope_x.max : scope_y.max;

    vec = B8(xl);
    r = AS_B8(vec);

    if (min > max) {
        memset(r, 0, xl);
        return vec;
    }

    range = max - min + 1;

    if (range <= MAX_RANGE) {
        set = B8(range);
        s = AS_B8(set);
        memset(s, 0, range);

        for (i = 0; i < yl; i++) {
            val = y[i] - min;
            if (val >= 0 && val < (i64_t)range)
                s[val] = B8_TRUE;
        }

        for (i = 0; i < xl; i++) {
            val = x[i] - min;
            if (val >= 0 && val < (i64_t)range)
                r[i] = s[val];
            else
                r[i] = B8_FALSE;
        }

        drop_obj(set);

        return vec;
    }

    // otherwise, use a hash table
    set = ht_oa_create(xl, -1);

    for (i = 0; i < yl; i++)
        AS_I64(AS_LIST(set)[0])[ht_oa_tab_next(&set, (i64_t)y[i])] = (i64_t)y[i];

    for (i = 0; i < xl; i++)
        if (x[i] == NULL_I64)
            r[i] = ht_oa_tab_get(set, (i64_t)NULL_I32) != NULL_I64;
        else if (x[i] > (i64_t)NULL_I32 && x[i] <= (i64_t)INF_I32)
            r[i] = ht_oa_tab_get(set, x[i]) != NULL_I64;
        else
            r[i] = B8_FALSE;

    drop_obj(set);

    return vec;
}

obj_p index_in_i64_i64(i64_t x[], i64_t xl, i64_t y[], i64_t yl) {
    i64_t i, range;
    i64_t val, min, max;
    obj_p vec, set;
    i8_t *s, *r;
    b8_t nl = B8_FALSE;

    if (xl == 0)
        return B8(0);

    const index_scope_t scope_x = index_scope_i64(x, NULL, xl);
    const index_scope_t scope_y = index_scope_i64(y, NULL, yl);

    min = (scope_x.min > scope_y.min) ? scope_x.min : scope_y.min;
    max = (scope_x.max < scope_y.max) ? scope_x.max : scope_y.max;

    vec = B8(xl);
    r = AS_B8(vec);

    if (min > max) {
        memset(r, 0, xl);
        return vec;
    }

    // TODO: fix this
    __int128 rng = (__int128)max - (__int128)min;
    range = rng > (__int128)MAX_RANGE ? INF_I64 : (i64_t)rng + 1;

    if (range <= MAX_RANGE) {
        set = B8(range);
        s = AS_B8(set);
        memset(s, 0, range);

        for (i = 0; i < yl; i++) {
            val = y[i] - min;
            if (val >= 0 && val < (i64_t)range)
                s[val] = B8_TRUE;
        }

        for (i = 0; i < xl; i++) {
            val = x[i] - min;
            if (val >= 0 && val < (i64_t)range)
                r[i] = s[val];
            else
                r[i] = B8_FALSE;
        }

        drop_obj(set);

        return vec;
    }

    // otherwise, use a hash table
    set = ht_oa_create(xl, -1);

    for (i = 0; i < yl; i++)
        if (y[i] == NULL_I64)
            nl = B8_TRUE;
        else
            AS_I64(AS_LIST(set)[0])[ht_oa_tab_next(&set, y[i])] = y[i];

    for (i = 0; i < xl; i++)
        if (x[i] == NULL_I64)
            r[i] = nl;
        else
            r[i] = ht_oa_tab_get(set, x[i]) != NULL_I64;

    drop_obj(set);

    return vec;
}

obj_p index_in_guid_guid(guid_t x[], i64_t xl, guid_t y[], i64_t yl) {
    i64_t i, *hashes;
    obj_p ht, hs, res;
    i64_t idx;
    __index_find_ctx_t ctx;

    // calc hashes
    hs = I64(xl);
    ht = ht_oa_create(xl, -1);

    hashes = (i64_t *)AS_I64(hs);

    for (i = 0; i < xl; i++)
        hashes[i] = hash_index_u64(*(i64_t *)(x + i), *((i64_t *)(x + i) + 1));

    ctx = (__index_find_ctx_t){.lobj = x, .robj = x, .hashes = hashes};
    for (i = 0; i < xl; i++) {
        idx = ht_oa_tab_next_with(&ht, i, &__hash_get, &__hash_cmp_guid, &ctx);
        if (AS_I64(AS_LIST(ht)[0])[idx] == NULL_I64)
            AS_I64(AS_LIST(ht)[0])[idx] = i;
    }

    for (i = 0; i < yl; i++)
        hashes[i] = hash_index_u64(*(i64_t *)(y + i), *((i64_t *)(y + i) + 1));

    res = B8(yl);

    ctx = (__index_find_ctx_t){.lobj = x, .robj = y, .hashes = hashes};
    for (i = 0; i < yl; i++) {
        idx = ht_oa_tab_get_with(ht, i, &__hash_get, &__hash_cmp_guid, &ctx);
        AS_B8(res)[i] = idx != NULL_I64;
    }

    drop_obj(ht);
    drop_obj(hs);

    return res;
}

obj_p index_find_i8(i8_t x[], i64_t xl, i8_t y[], i64_t yl) {
    i64_t i, range;
    i64_t min, val, *d, *r;
    obj_p vec, dict;

    min = -128;
    range = 256;

    if (xl == 0)
        return I64(0);

    dict = I64(range);
    d = AS_I64(dict);

    for (i = 0; i < range; i++)
        d[i] = NULL_I64;

    for (i = 0; i < xl; i++) {
        val = x[i] - min;
        if (d[val] == NULL_I64)
            d[val] = (i64_t)i;
    }

    vec = I64(yl);
    r = AS_I64(vec);

    for (i = 0; i < yl; i++) {
        val = y[i] - min;
        r[i] = d[val];
    }

    drop_obj(dict);

    return vec;
}

obj_p index_find_i32(i32_t x[], i64_t xl, i32_t y[], i64_t yl) {
    i64_t i, range;
    i64_t min, max, val, *d, *r;
    obj_p vec, dict;

    if (xl == 0)
        return I64(0);

    const index_scope_t scope_x = index_scope_i32(x, NULL, xl);
    const index_scope_t scope_y = index_scope_i32(y, NULL, yl);

    min = (scope_x.min > scope_y.min) ? scope_x.min : scope_y.min;
    max = (scope_x.max < scope_y.max) ? scope_x.max : scope_y.max;

    vec = I64(yl);
    r = AS_I64(vec);

    if (min > max) {
        for (i = 0; i < yl; i++)
            r[i] = NULL_I64;
        return vec;
    }

    range = max - min + 1;

    if (range <= MAX_RANGE) {
        dict = I64(range);
        d = AS_I64(dict);

        for (i = 0; i < range; i++)
            d[i] = NULL_I64;

        for (i = 0; i < xl; i++) {
            val = x[i] - min;
            if (val >= 0 && val < (i64_t)range && d[val] == NULL_I64)
                d[val] = (i64_t)i;
        }

        for (i = 0; i < yl; i++) {
            val = y[i] - min;
            r[i] = (val < 0 || val >= (i64_t)range) ? NULL_I64 : d[val];
        }

        drop_obj(dict);

        return vec;
    }

    // otherwise, use a hash table
    dict = ht_oa_create(xl, TYPE_I32);

    for (i = 0; i < xl; i++) {
        val = ht_oa_tab_next(&dict, (i64_t)x[i]);
        if (AS_I64(AS_LIST(dict)[0])[val] == NULL_I64) {
            AS_I64(AS_LIST(dict)[0])[val] = (i64_t)x[i];
            AS_I64(AS_LIST(dict)[1])[val] = i;
        }
    }

    for (i = 0; i < yl; i++) {
        val = ht_oa_tab_get(dict, (i64_t)y[i]);
        r[i] = val == NULL_I64 ? NULL_I64 : AS_I64(AS_LIST(dict)[1])[val];
    }

    drop_obj(dict);

    return vec;
}

obj_p index_find_i64(i64_t x[], i64_t xl, i64_t y[], i64_t yl) {
    i64_t i, range;
    i64_t min, max, val, *d, *r;
    obj_p vec, dict;

    if (xl == 0)
        return I64(0);

    const index_scope_t scope_x = index_scope_i64(x, NULL, xl);
    const index_scope_t scope_y = index_scope_i64(y, NULL, yl);

    min = (scope_x.min > scope_y.min) ? scope_x.min : scope_y.min;
    max = (scope_x.max < scope_y.max) ? scope_x.max : scope_y.max;

    vec = I64(yl);
    r = AS_I64(vec);

    if (min > max) {
        for (i = 0; i < yl; i++)
            r[i] = NULL_I64;
        return vec;
    }

    range = max - min + 1;

    if (range <= MAX_RANGE) {
        dict = I64(range);
        d = AS_I64(dict);

        for (i = 0; i < range; i++)
            d[i] = NULL_I64;

        for (i = 0; i < xl; i++) {
            val = x[i] - min;
            if (val >= 0 && val < (i64_t)range && d[val] == NULL_I64)
                d[val] = (i64_t)i;
        }

        for (i = 0; i < yl; i++) {
            val = y[i] - min;
            r[i] = (val < 0 || val >= (i64_t)range) ? NULL_I64 : d[val];
        }

        drop_obj(dict);

        return vec;
    }

    // otherwise, use a hash table
    dict = ht_oa_create(xl, TYPE_I64);

    for (i = 0; i < xl; i++) {
        val = ht_oa_tab_next(&dict, x[i]);
        if (AS_I64(AS_LIST(dict)[0])[val] == NULL_I64) {
            AS_I64(AS_LIST(dict)[0])[val] = x[i];
            AS_I64(AS_LIST(dict)[1])[val] = i;
        }
    }

    for (i = 0; i < yl; i++) {
        val = ht_oa_tab_get(dict, y[i]);
        r[i] = val == NULL_I64 ? NULL_I64 : AS_I64(AS_LIST(dict)[1])[val];
    }

    drop_obj(dict);

    return vec;
}

obj_p index_find_guid(guid_t x[], i64_t xl, guid_t y[], i64_t yl) {
    i64_t i, *hashes;
    obj_p ht, res;
    i64_t idx;
    __index_find_ctx_t ctx;

    // calc hashes
    res = I64(MAXI64(xl, yl));
    ht = ht_oa_create(MAXI64(xl, yl) * 2, -1);

    hashes = (i64_t *)AS_I64(res);

    for (i = 0; i < xl; i++)
        hashes[i] = hash_index_u64(*(i64_t *)(x + i), *((i64_t *)(x + i) + 1));

    ctx = (__index_find_ctx_t){.lobj = x, .robj = x, .hashes = hashes};
    for (i = 0; i < xl; i++) {
        idx = ht_oa_tab_next_with(&ht, i, &__hash_get, &__hash_cmp_guid, &ctx);
        if (AS_I64(AS_LIST(ht)[0])[idx] == NULL_I64)
            AS_I64(AS_LIST(ht)[0])[idx] = i;
    }

    for (i = 0; i < yl; i++)
        hashes[i] = hash_index_u64(*(i64_t *)(y + i), *((i64_t *)(y + i) + 1));

    ctx = (__index_find_ctx_t){.lobj = x, .robj = y, .hashes = hashes};
    for (i = 0; i < yl; i++) {
        idx = ht_oa_tab_get_with(ht, i, &__hash_get, &__hash_cmp_guid, &ctx);
        AS_I64(res)[i] = (idx == NULL_I64) ? NULL_I64 : AS_I64(AS_LIST(ht)[0])[idx];
    }

    drop_obj(ht);
    resize_obj(&res, yl);

    return res;
}

obj_p index_find_obj(obj_p x[], i64_t xl, obj_p y[], i64_t yl) {
    i64_t i, *hashes;
    obj_p ht, res;
    i64_t idx;
    __index_find_ctx_t ctx;

    // calc hashes
    res = I64(MAXI64(xl, yl));
    ht = ht_oa_create(MAXI64(xl, yl) * 2, -1);

    hashes = (i64_t *)AS_I64(res);

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

i64_t index_group_count(obj_p index) { return (i64_t)AS_LIST(index)[1]->i64; }

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

i64_t index_group_len(obj_p index) {
    if (AS_LIST(index)[0]->i64 == INDEX_TYPE_WINDOW)
        return AS_LIST(index)[1]->i64;

    if (AS_LIST(index)[5] != NULL_OBJ)  // filter
        return AS_LIST(index)[5]->len;

    if (AS_LIST(index)[4] != NULL_OBJ)  // source
        return AS_LIST(index)[4]->len;

    return ops_count(AS_LIST(index)[2]);
}

index_type_t index_group_type(obj_p index) { return (index_type_t)AS_LIST(index)[0]->i64; }

i64_t *index_group_source(obj_p index) { return AS_I64(AS_LIST(index)[4]); }

i64_t index_group_shift(obj_p index) { return AS_LIST(index)[3]->i64; }

obj_p index_group_meta(obj_p index) { return AS_LIST(index)[6]; }

i64_t *index_group_first_ids(obj_p index) {
    obj_p meta = AS_LIST(index)[6];
    if (is_null(meta) || meta->type != TYPE_I64)
        return NULL;
    return AS_I64(meta);
}

static obj_p index_group_build(index_type_t tp, i64_t groups_count, obj_p group_ids, obj_p index_min, obj_p source,
                               obj_p filter, obj_p meta) {
    return vn_list(7, i64(tp), i64(groups_count), group_ids, index_min, source, filter, meta);
}

// Context for chunk-based parallel grouping
typedef struct __group_chunk_ctx_t {
    i64_t *keys;
    i64_t *filter;
    i64_t *out;
    i64_t *local_groups;  // per-chunk group count
    hash_f hash;
    cmp_f cmp;
    obj_p *local_hts;  // per-chunk hash tables
} __group_chunk_ctx_t;

// Phase 1: Each chunk builds its own local hash table and assigns local group IDs
obj_p index_group_chunk_local(i64_t len, i64_t offset, __group_chunk_ctx_t *ctx) {
    i64_t i, idx, n, groups;
    i64_t *k, *v, *keys, *filter, *out;
    obj_p ht;
    hash_f hash;
    cmp_f cmp;

    keys = ctx->keys;
    filter = ctx->filter;
    out = ctx->out;
    hash = ctx->hash;
    cmp = ctx->cmp;

    ht = ht_oa_create(len, TYPE_I64);
    groups = 0;

    if (filter) {
        for (i = offset; i < offset + len; i++) {
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
        for (i = offset; i < offset + len; i++) {
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

    ctx->local_groups[offset > 0 ? 1 : 0] = groups;  // Store in appropriate slot based on chunk
    ctx->local_hts[offset > 0 ? 1 : 0] = ht;

    return NULL_OBJ;
}

// Phase 2: Remap local group IDs to global IDs using the merged hash table
obj_p index_group_chunk_remap(i64_t len, i64_t offset, i64_t *out, i64_t *remap) {
    i64_t i;

    for (i = offset; i < offset + len; i++)
        out[i] = remap[out[i]];

    return NULL_OBJ;
}

i64_t index_group_distribute(i64_t keys[], i64_t filter[], i64_t out[], i64_t len, hash_f hash, cmp_f cmp) {
    i64_t i, j, parts, groups, chunk, last_chunk;
    i64_t idx, n, *k, *v, *remap;
    i64_t offsets[32], local_groups[32];  // Support up to 32 chunks
    obj_p local_hts[32];
    pool_p pool;
    obj_p ht, merged_ht, res;
    __group_chunk_ctx_t ctx;

    pool = pool_get();
    parts = pool_split_by(pool, len, 0);
    groups = 0;

    // Single-threaded path
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

    // Limit parts to avoid stack overflow
    if (parts > 32)
        parts = 32;

    // Setup context
    ctx.keys = keys;
    ctx.filter = filter;
    ctx.out = out;
    ctx.local_groups = local_groups;
    ctx.hash = hash;
    ctx.cmp = cmp;
    ctx.local_hts = local_hts;

    // Initialize
    for (i = 0; i < parts; i++) {
        local_groups[i] = 0;
        local_hts[i] = NULL_OBJ;
    }

    // Calculate chunk sizes
    chunk = len / parts;
    last_chunk = len - chunk * (parts - 1);

    // Phase 1: Build local hash tables in parallel (each chunk gets its own)
    pool_prepare(pool);
    for (i = 0; i < parts; i++) {
        offsets[i] = i * chunk;
        ctx.local_groups = local_groups + i;
        ctx.local_hts = local_hts + i;

        if (i < parts - 1)
            pool_add_task(pool, (raw_p)index_group_chunk_local, 3, chunk, i * chunk, &ctx);
        else
            pool_add_task(pool, (raw_p)index_group_chunk_local, 3, last_chunk, i * chunk, &ctx);
    }
    res = pool_run(pool);
    drop_obj(res);

    // Phase 2: Merge local hash tables sequentially to assign global group IDs
    // This is O(total_unique_keys) which is typically much smaller than O(n)
    merged_ht = ht_oa_create(len, TYPE_I64);
    groups = 0;

    // For each chunk, create a remap array and merge its unique keys
    for (i = 0; i < parts; i++) {
        if (local_hts[i] == NULL_OBJ)
            continue;

        ht = local_hts[i];
        i64_t ht_len = AS_LIST(ht)[0]->len;
        i64_t *ht_keys = AS_I64(AS_LIST(ht)[0]);
        i64_t *ht_vals = AS_I64(AS_LIST(ht)[1]);

        // Create remap array for this chunk's local IDs -> global IDs
        remap = (i64_t *)heap_alloc(local_groups[i] * sizeof(i64_t));

        for (j = 0; j < ht_len; j++) {
            if (ht_keys[j] != NULL_I64) {
                i64_t local_id = ht_vals[j];
                n = ht_keys[j];

                // Check if this key exists in merged table
                idx = ht_oa_tab_next_with(&merged_ht, n, hash, cmp, NULL);
                k = AS_I64(AS_LIST(merged_ht)[0]);
                v = AS_I64(AS_LIST(merged_ht)[1]);

                if (k[idx] == NULL_I64) {
                    k[idx] = n;
                    v[idx] = groups++;
                }

                remap[local_id] = v[idx];
            }
        }

        // Remap this chunk's output values
        i64_t chunk_start = offsets[i];
        i64_t chunk_len = (i < parts - 1) ? chunk : last_chunk;
        for (j = chunk_start; j < chunk_start + chunk_len; j++)
            out[j] = remap[out[j]];

        heap_free(remap);
        drop_obj(ht);
    }

    drop_obj(merged_ht);

    return groups;
}

obj_p index_group_i8(obj_p obj, obj_p filter) {
    i64_t i, j, n, len, range;
    i64_t min, *hk, *hv, *indices;
    i8_t *values;
    obj_p keys, vals;

    values = AS_I8(obj);
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

    return index_group_build(INDEX_TYPE_IDS, j, vals, i64(NULL_I64), NULL_OBJ, clone_obj(filter), NULL_OBJ);
}

obj_p index_group_i64_unscoped(obj_p obj, obj_p filter) {
    i64_t len;
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

    return index_group_build(INDEX_TYPE_IDS, g, vals, i64(NULL_I64), NULL_OBJ, clone_obj(filter), NULL_OBJ);
}

obj_p fill_null_i64(i64_t *arr, i64_t len, i64_t offset) {
    for (i64_t i = offset; i < offset + len; i++)
        arr[i] = NULL_I64;
    return NULL_OBJ;
}

obj_p index_group_i64_scoped_partial(i64_t input[], i64_t filter[], i64_t group_ids[], i64_t len, i64_t offset,
                                     i64_t min, i64_t out[]) {
    i64_t i, l;

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
    i64_t i, n, len, groups, chunks, base_chunk;
    i64_t *hk, *hv, *hf, *values, *indices;
    obj_p keys, vals, firsts, v;
    pool_p pool;

    values = AS_I64(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : obj->len;

    // perfect hash
    if (scope.range <= len) {
        keys = I64(scope.range);
        hk = AS_I64(keys);
        // Initialize keys array in parallel
        pool = pool_get();
        chunks = pool_split_by(pool, scope.range, 0);
        if (chunks == 1) {
            for (i = 0; i < scope.range; i++)
                hk[i] = NULL_I64;
        } else {
            base_chunk = pool_chunk_aligned(scope.range, chunks, sizeof(i64_t));
            pool_prepare(pool);
            for (i = 0; i < chunks - 1; i++)
                pool_add_task(pool, (raw_p)fill_null_i64, 3, hk, base_chunk, i * base_chunk);
            pool_add_task(pool, (raw_p)fill_null_i64, 3, hk, scope.range - (chunks - 1) * base_chunk,
                          (chunks - 1) * base_chunk);
            v = pool_run(pool);
            drop_obj(v);
        }

        // Pre-allocate firsts array (we'll resize if needed, but usually groups < scope.range)
        firsts = I64(len);  // Worst case: every row is a new group
        hf = AS_I64(firsts);

        if (indices) {
            for (i = 0, groups = 0; i < len; i++) {
                n = values[indices[i]] - scope.min;
                if (hk[n] == NULL_I64) {
                    hk[n] = groups;
                    hf[groups] = i;  // Track first row index directly by group ID
                    groups++;
                }
            }
        } else {
            for (i = 0, groups = 0; i < len; i++) {
                n = values[i] - scope.min;
                if (hk[n] == NULL_I64) {
                    hk[n] = groups;
                    hf[groups] = i;  // Track first row index directly by group ID
                    groups++;
                }
            }
        }

        // Resize firsts to actual number of groups
        resize_obj(&firsts, groups);
        hf = AS_I64(firsts);

        // For small scope.range, use SHIFT (no group_ids array needed)
        // For large scope.range, use IDS (better cache locality in aggregation)
        if (scope.range <= INDEX_SCOPE_LIMIT) {
            timeit_tick("index group scoped perfect simple");
            return index_group_build(INDEX_TYPE_SHIFT, groups, keys, i64(scope.min), clone_obj(obj), clone_obj(filter),
                                     firsts);
        }

        // Build group_ids array for better aggregation performance
        vals = I64(len);
        hv = AS_I64(vals);
        pool = pool_get();
        chunks = pool_split_by(pool, len, 0);
        base_chunk = pool_chunk_aligned(len, chunks, sizeof(i64_t));
        if (chunks == 1)
            index_group_i64_scoped_partial(values, indices, hk, len, 0, scope.min, hv);
        else {
            pool_prepare(pool);
            for (i = 0; i < chunks - 1; i++)
                pool_add_task(pool, (raw_p)index_group_i64_scoped_partial, 7, values, indices, hk, base_chunk,
                              i * base_chunk, scope.min, hv);
            pool_add_task(pool, (raw_p)index_group_i64_scoped_partial, 7, values, indices, hk,
                          len - (chunks - 1) * base_chunk, (chunks - 1) * base_chunk, scope.min, hv);
            v = pool_run(pool);
            drop_obj(v);
        }
        drop_obj(keys);
        timeit_tick("index group scoped perfect");
        return index_group_build(INDEX_TYPE_IDS, groups, vals, i64(NULL_I64), NULL_OBJ, clone_obj(filter), firsts);
    }
    return index_group_i64_unscoped(obj, filter);
}

obj_p index_group_i64(obj_p obj, obj_p filter) {
    index_scope_t scope;
    i64_t *values, *indices;
    i64_t len;

    values = AS_I64(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : obj->len;

    scope = index_scope_i64(values, indices, len);

    return index_group_i64_scoped(obj, filter, scope);
}

obj_p index_group_f64(obj_p obj, obj_p filter) { return index_group_i64_unscoped(obj, filter); }

obj_p index_group_guid(obj_p obj, obj_p filter) {
    i64_t i, j, len;
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

    return index_group_build(INDEX_TYPE_IDS, j, vals, i64(NULL_I64), NULL_OBJ, clone_obj(filter), NULL_OBJ);
}

obj_p index_group_obj(obj_p obj, obj_p filter) {
    i64_t g, len;
    i64_t *out, *indices, *values;
    obj_p vals;

    values = (i64_t *)AS_LIST(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : obj->len;

    vals = I64(len);
    out = AS_I64(vals);

    g = index_group_distribute(values, indices, out, len, &hash_obj, &hash_cmp_obj);

    return index_group_build(INDEX_TYPE_IDS, g, vals, i64(NULL_I64), NULL_OBJ, clone_obj(filter), NULL_OBJ);
}

obj_p index_group(obj_p val, obj_p filter) {
    i64_t i, l, g;
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
                    obj_p fentry = AS_LIST(filter)[i];
                    // Count non-null AND non-empty filter entries
                    // i64(-1) marker means "take all rows" - count it
                    // Empty I64 vector means no rows match - don't count it
                    if (fentry != NULL_OBJ) {
                        if (fentry->type == -TYPE_I64 && fentry->i64 == -1) {
                            g++;  // Marker for "take all rows"
                        } else if (fentry->len > 0) {
                            g++;  // Has actual indices
                        }
                        // Empty vector (len == 0) - don't count
                    }
                }
            } else {
                g = AS_LIST(val)[0]->len;
            }

            return index_group_build(INDEX_TYPE_PARTEDCOMMON, g, clone_obj(val), i64(NULL_I64), NULL_OBJ,
                                     clone_obj(filter), NULL_OBJ);
        default:
            return err_type(0, 0, 0, 0);
    }
}

// Context for parallel combined key computation
typedef struct __perfect_hash_ctx_t {
    obj_p *values;
    i64_t *indices;
    i64_t *out;
    index_scope_t *scopes;
    i64_t *multipliers;
    i64_t ncols;
} __perfect_hash_ctx_t;

obj_p index_group_list_perfect_partial(i64_t len, i64_t offset, __perfect_hash_ctx_t *ctx) {
    i64_t j, c, ncols;
    i64_t *out, *indices, *multipliers;
    obj_p *values, col;
    index_scope_t *scopes;

    out = ctx->out;
    indices = ctx->indices;
    values = ctx->values;
    scopes = ctx->scopes;
    multipliers = ctx->multipliers;
    ncols = ctx->ncols;

    // Process all columns in a single pass for better cache efficiency
    if (indices) {
        for (j = offset; j < offset + len; j++) {
            i64_t key = 0;
            i64_t row = indices[j];
            for (c = 0; c < ncols; c++) {
                col = values[c];
                switch (col->type) {
                    case TYPE_B8:
                    case TYPE_U8:
                    case TYPE_C8:
                        key += (AS_U8(col)[row] - scopes[c].min) * multipliers[c];
                        break;
                    case TYPE_I64:
                    case TYPE_SYMBOL:
                    case TYPE_TIMESTAMP:
                        key += (AS_I64(col)[row] - scopes[c].min) * multipliers[c];
                        break;
                    case TYPE_ENUM:
                        key += (AS_I64(ENUM_VAL(col))[row] - scopes[c].min) * multipliers[c];
                        break;
                    default:
                        break;
                }
            }
            out[j] = key;
        }
    } else {
        for (j = offset; j < offset + len; j++) {
            i64_t key = 0;
            for (c = 0; c < ncols; c++) {
                col = values[c];
                switch (col->type) {
                    case TYPE_B8:
                    case TYPE_U8:
                    case TYPE_C8:
                        key += (AS_U8(col)[j] - scopes[c].min) * multipliers[c];
                        break;
                    case TYPE_I64:
                    case TYPE_SYMBOL:
                    case TYPE_TIMESTAMP:
                        key += (AS_I64(col)[j] - scopes[c].min) * multipliers[c];
                        break;
                    case TYPE_ENUM:
                        key += (AS_I64(ENUM_VAL(col))[j] - scopes[c].min) * multipliers[c];
                        break;
                    default:
                        break;
                }
            }
            out[j] = key;
        }
    }

    return NULL_OBJ;
}

obj_p index_group_list_perfect(obj_p obj, obj_p filter) {
    u64_t i, l, len, product;
    i64_t *xo, *indices;
    obj_p ht, res, *values, v;
    index_scope_t *scopes, scope;
    pool_p pool;
    i64_t chunks, base_chunk;
    __perfect_hash_ctx_t ctx;

    l = obj->len;
    i64_t multipliers[l];
    UNUSED(v);  // May be unused in single-threaded path

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
                scopes[i] = index_scope_i64(AS_I64(values[i]), indices, len);
                break;
            default:
                __builtin_unreachable();
        }
    }

    // Precompute multipliers for each column
    for (i = 0, product = 1, scope = (index_scope_t){0, 0, 0}; i < l; i++) {
        if (ULLONG_MAX / product < (u64_t)scopes[i].range) {
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

    // Parallel combined key computation - single pass over all columns
    ctx = (__perfect_hash_ctx_t){
        .values = values,
        .indices = indices,
        .out = xo,
        .scopes = scopes,
        .multipliers = multipliers,
        .ncols = l
    };

    pool = pool_get();
    chunks = pool_split_by(pool, len, 0);
    base_chunk = pool_chunk_aligned(len, chunks, sizeof(i64_t));

    if (chunks == 1) {
        index_group_list_perfect_partial(len, 0, &ctx);
    } else {
        pool_prepare(pool);
        for (i = 0; i < (u64_t)(chunks - 1); i++)
            pool_add_task(pool, (raw_p)index_group_list_perfect_partial, 3, base_chunk, i * base_chunk, &ctx);
        pool_add_task(pool, (raw_p)index_group_list_perfect_partial, 3, len - (chunks - 1) * base_chunk,
                      (chunks - 1) * base_chunk, &ctx);
        v = pool_run(pool);
        drop_obj(v);
    }

    heap_free(scopes);
    res = index_group_i64_scoped(ht, NULL_OBJ, scope);
    drop_obj(ht);

    return res;
}

// Context for parallel list grouping
typedef struct __group_list_chunk_ctx_t {
    __index_list_ctx_t *list_ctx;
    i64_t *out;
    i64_t *hashes;
    i64_t local_groups;
    obj_p local_ht;
} __group_list_chunk_ctx_t;

// Phase 1: Each chunk builds its own local hash table
obj_p index_group_list_chunk_local(i64_t len, i64_t offset, __group_list_chunk_ctx_t *ctx) {
    i64_t i, v, groups;
    i64_t *out;
    obj_p ht;
    __index_list_ctx_t *list_ctx;

    out = ctx->out;
    list_ctx = ctx->list_ctx;

    ht = ht_oa_create(len, TYPE_I64);
    groups = 0;

    for (i = offset; i < offset + len; i++) {
        v = ht_oa_tab_insert_with(&ht, i, groups, &__index_list_hash_get, &__index_list_cmp_row, list_ctx);
        if (v == groups)
            groups++;

        out[i] = v;
    }

    ctx->local_groups = groups;
    ctx->local_ht = ht;

    return NULL_OBJ;
}

obj_p index_group_list(obj_p obj, obj_p filter) {
    i64_t i, j, len, parts, chunk, last_chunk, groups;
    i64_t g, v, *xo, *indices, *remap;
    obj_p res, *values, ht, poolres;
    __index_list_ctx_t ctx;
    __group_list_chunk_ctx_t chunk_ctxs[32];
    i64_t offsets[32];
    pool_p pool;

    if (ops_count(obj) == 0)
        return err_type(0, 0, 0, 0);

    if (ops_count(obj) == 1)
        return index_group(AS_LIST(obj)[0], filter);

    // If the list values range is small, use perfect hashing
    res = index_group_list_perfect(obj, filter);
    if (!is_null(res)) {
        timeit_tick("group index list perfect");
        return res;
    }

    values = AS_LIST(obj);
    indices = is_null(filter) ? NULL : AS_I64(filter);
    len = indices ? filter->len : values[0]->len;

    res = I64(len);
    xo = AS_I64(res);

    __index_list_precalc_hash(obj, (i64_t *)xo, obj->len, len, indices, B8_FALSE);
    timeit_tick("group index precalc hash");

    ctx = (__index_list_ctx_t){.lcols = obj, .rcols = obj, .hashes = (i64_t *)xo, .filter = indices};

    pool = pool_get();
    parts = pool_split_by(pool, len, 0);

    // Single-threaded path
    if (parts == 1) {
        ht = ht_oa_create(len, TYPE_I64);

        // distribute bins
        for (i = 0, g = 0; i < len; i++) {
            v = ht_oa_tab_insert_with(&ht, i, g, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
            if (v == g)
                g++;

            xo[i] = v;
        }

        drop_obj(ht);
        timeit_tick("group index list");

        return index_group_build(INDEX_TYPE_IDS, g, res, i64(NULL_I64), NULL_OBJ, clone_obj(filter), NULL_OBJ);
    }

    // Limit parts
    if (parts > 32)
        parts = 32;

    // Calculate chunk sizes
    chunk = len / parts;
    last_chunk = len - chunk * (parts - 1);

    // Initialize chunk contexts
    for (i = 0; i < parts; i++) {
        offsets[i] = i * chunk;
        chunk_ctxs[i].list_ctx = &ctx;
        chunk_ctxs[i].out = xo;
        chunk_ctxs[i].hashes = xo;
        chunk_ctxs[i].local_groups = 0;
        chunk_ctxs[i].local_ht = NULL_OBJ;
    }

    // Phase 1: Build local hash tables in parallel
    pool_prepare(pool);
    for (i = 0; i < parts; i++) {
        if (i < parts - 1)
            pool_add_task(pool, (raw_p)index_group_list_chunk_local, 3, chunk, i * chunk, &chunk_ctxs[i]);
        else
            pool_add_task(pool, (raw_p)index_group_list_chunk_local, 3, last_chunk, i * chunk, &chunk_ctxs[i]);
    }
    poolres = pool_run(pool);
    drop_obj(poolres);

    // Phase 2: Merge local hash tables sequentially
    // This is O(total_unique_keys) which is typically much smaller than O(n)
    ht = ht_oa_create(len, TYPE_I64);
    groups = 0;

    for (i = 0; i < parts; i++) {
        if (chunk_ctxs[i].local_ht == NULL_OBJ)
            continue;

        obj_p local_ht = chunk_ctxs[i].local_ht;
        i64_t ht_len = AS_LIST(local_ht)[0]->len;
        i64_t *ht_keys = AS_I64(AS_LIST(local_ht)[0]);
        i64_t *ht_vals = AS_I64(AS_LIST(local_ht)[1]);

        // Create remap array for this chunk's local IDs -> global IDs
        remap = (i64_t *)heap_alloc(chunk_ctxs[i].local_groups * sizeof(i64_t));

        // For each unique key in this chunk's hash table
        for (j = 0; j < ht_len; j++) {
            if (ht_keys[j] != NULL_I64) {
                i64_t local_id = ht_vals[j];
                i64_t row_idx = ht_keys[j];  // This stores the row index, not the key itself

                // Try to insert into merged table
                v = ht_oa_tab_insert_with(&ht, row_idx, groups, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
                if (v == groups)
                    groups++;

                remap[local_id] = v;
            }
        }

        // Remap this chunk's output values
        i64_t chunk_start = offsets[i];
        i64_t chunk_len = (i < parts - 1) ? chunk : last_chunk;
        for (j = chunk_start; j < chunk_start + chunk_len; j++)
            xo[j] = remap[xo[j]];

        heap_free(remap);
        drop_obj(local_ht);
    }

    drop_obj(ht);

    timeit_tick("group index list");

    return index_group_build(INDEX_TYPE_IDS, groups, res, i64(NULL_I64), NULL_OBJ, clone_obj(filter), NULL_OBJ);
}

obj_p index_left_join_obj(obj_p lcols, obj_p rcols, i64_t len) {
    i64_t i, ll, rl;
    obj_p ht, ids, hashes;
    i64_t idx;
    __index_list_ctx_t ctx;

    // one column join
    if (len == 1)
        return ray_find(rcols, lcols);

    // multiple columns join
    ll = ops_count(AS_LIST(lcols)[0]);
    rl = ops_count(AS_LIST(rcols)[0]);
    ht = ht_oa_create(rl, -1);
    hashes = I64(MAXI64(ll, rl));

    // Right hashes
    __index_list_precalc_hash(rcols, (i64_t *)AS_I64(hashes), len, rl, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, rcols, (i64_t *)AS_I64(hashes), NULL};
    for (i = 0; i < rl; i++) {
        idx = ht_oa_tab_next_with(&ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        if (AS_I64(AS_LIST(ht)[0])[idx] == NULL_I64)
            AS_I64(AS_LIST(ht)[0])[idx] = i;
    }

    ids = I64(ll);

    // Left hashes
    __index_list_precalc_hash(lcols, (i64_t *)AS_I64(hashes), len, ll, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, lcols, (i64_t *)AS_I64(hashes), NULL};
    for (i = 0; i < ll; ++i) {
        idx = ht_oa_tab_get_with(ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        if (idx != NULL_I64)
            AS_I64(ids)[i] = AS_I64(AS_LIST(ht)[0])[idx];
        else
            AS_I64(ids)[i] = NULL_I64;
    }

    drop_obj(hashes);
    drop_obj(ht);

    return ids;
}

obj_p index_inner_join_obj(obj_p lcols, obj_p rcols, i64_t len) {
    i64_t i, j, ll, rl;
    obj_p ht, lids, rids, find_res;
    i64_t idx;
    __index_list_ctx_t ctx;

    if (len == 1) {
        // For single key: find matching indices from left in right
        find_res = ray_find(rcols, lcols);
        if (IS_ERR(find_res))
            return find_res;

        // Count matches (non-NULL results)
        ll = find_res->len;
        j = 0;
        for (i = 0; i < ll; i++) {
            if (AS_I64(find_res)[i] != NULL_I64)
                j++;
        }

        // Build result arrays with only matching rows
        lids = I64(j);
        rids = I64(j);
        j = 0;
        for (i = 0; i < ll; i++) {
            if (AS_I64(find_res)[i] != NULL_I64) {
                AS_I64(lids)[j] = i;                    // index in left table
                AS_I64(rids)[j] = AS_I64(find_res)[i];  // index in right table
                j++;
            }
        }

        drop_obj(find_res);
        return vn_list(2, lids, rids);
    }

    ll = ops_count(AS_LIST(lcols)[0]);
    rl = ops_count(AS_LIST(rcols)[0]);
    ht = ht_oa_create(rl, -1);
    rids = I64(MAXI64(ll, rl));

    // Right hashes
    __index_list_precalc_hash(rcols, (i64_t *)AS_I64(rids), len, rl, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, rcols, (i64_t *)AS_I64(rids), NULL};
    for (i = 0; i < rl; i++) {
        idx = ht_oa_tab_next_with(&ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        if (AS_I64(AS_LIST(ht)[0])[idx] == NULL_I64)
            AS_I64(AS_LIST(ht)[0])[idx] = i;
    }

    lids = I64(ll);

    // Left hashes
    __index_list_precalc_hash(lcols, (i64_t *)AS_I64(rids), len, ll, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, lcols, (i64_t *)AS_I64(rids), NULL};
    for (i = 0, j = 0; i < ll; i++) {
        idx = ht_oa_tab_get_with(ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        if (idx != NULL_I64) {
            AS_I64(rids)[j] = AS_I64(AS_LIST(ht)[0])[idx];
            AS_I64(lids)[j++] = i;
        }
    }

    drop_obj(ht);

    resize_obj(&lids, j);
    resize_obj(&rids, j);

    return vn_list(2, lids, rids);
}

obj_p index_upsert_obj(obj_p lcols, obj_p rcols, i64_t len) {
    u64_t i, ll, rl;
    obj_p ht, res;
    i64_t idx;
    __index_list_ctx_t ctx;

    if (len == 1) {
        if (rcols->len == 0) {
            if (IS_VECTOR(lcols)) {
                ll = lcols->len;
                res = I64(ll);
                for (i = 0; i < ll; i++)
                    AS_I64(res)[i] = NULL_I64;
            } else {
                res = I64(1);
                AS_I64(res)[0] = NULL_I64;
            }
            return res;
        }

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

    if (rl == 0) {
        res = I64(ll);
        for (i = 0; i < (u64_t)ll; i++)
            AS_I64(res)[i] = NULL_I64;
        return res;
    }

    ht = ht_oa_create(MAXU64(ll, rl), -1);
    res = I64(MAXU64(ll, rl));

    // Right hashes
    __index_list_precalc_hash(rcols, AS_I64(res), len, rl, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, rcols, AS_I64(res), NULL};
    for (i = 0; i < rl; i++) {
        idx = ht_oa_tab_next_with(&ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        if (AS_I64(AS_LIST(ht)[0])[idx] == NULL_I64)
            AS_I64(AS_LIST(ht)[0])[idx] = i;
    }

    // Left hashes
    __index_list_precalc_hash(lcols, AS_I64(res), len, ll, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, lcols, AS_I64(res), NULL};
    for (i = 0; i < ll; i++) {
        idx = ht_oa_tab_get_with(ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        AS_I64(res)[i] = (idx == NULL_I64) ? NULL_I64 : AS_I64(AS_LIST(ht)[0])[idx];
    }

    drop_obj(ht);

    return res;
}

i64_t index_bin_u8(u8_t val, u8_t vals[], i64_t ids[], i64_t len) {
    i64_t left, right, mid, idx;
    if (len == 0)
        return NULL_I64;
    left = 0, right = len - 1, idx = NULL_I64;
    while (left <= right) {
        mid = left + (right - left) / 2;
        if (vals[ids[mid]] <= val) {
            idx = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    return (idx == NULL_I64) ? NULL_I64 : ids[idx];
}

i64_t index_bin_i16(i16_t val, i16_t vals[], i64_t ids[], i64_t len) {
    i64_t left, right, mid, idx;
    if (len == 0)
        return NULL_I64;
    left = 0, right = len - 1, idx = NULL_I64;
    while (left <= right) {
        mid = left + (right - left) / 2;
        if (vals[ids[mid]] <= val) {
            idx = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    return (idx == NULL_I64) ? NULL_I64 : ids[idx];
}

i64_t index_bin_i32(i32_t val, i32_t vals[], i64_t ids[], i64_t len) {
    i64_t left, right, mid, idx;
    if (len == 0)
        return NULL_I64;
    left = 0, right = len - 1, idx = NULL_I64;
    while (left <= right) {
        mid = left + (right - left) / 2;
        if (vals[ids[mid]] <= val) {
            idx = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    return (idx == NULL_I64) ? NULL_I64 : ids[idx];
}

i64_t index_bin_i64(i64_t val, i64_t vals[], i64_t ids[], i64_t len) {
    i64_t left, right, mid, idx;
    if (len == 0)
        return NULL_I64;
    left = 0, right = len - 1, idx = NULL_I64;
    while (left <= right) {
        mid = left + (right - left) / 2;
        if (vals[ids[mid]] <= val) {
            idx = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    return (idx == NULL_I64) ? NULL_I64 : ids[idx];
}

i64_t index_bin_f64(f64_t val, f64_t vals[], i64_t ids[], i64_t len) {
    i64_t left, right, mid, idx;
    if (len == 0)
        return NULL_I64;
    left = 0, right = len - 1, idx = NULL_I64;
    while (left <= right) {
        mid = left + (right - left) / 2;
        if (vals[ids[mid]] <= val) {
            idx = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    return (idx == NULL_I64) ? NULL_I64 : ids[idx];
}

static obj_p __asof_ids_partial(__index_list_ctx_t *ctx, obj_p lxcol, obj_p rxcol, obj_p ht, i64_t len, i64_t offset,
                                obj_p ids) {
    i64_t i, idx, p;

    switch (lxcol->type) {
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            for (i = offset; i < len + offset; i++) {
                idx = ht_oa_tab_get_with(ht, i, &__index_list_hash_get, &__index_list_cmp_row, ctx);
                if (idx != NULL_I64) {
                    p = index_bin_i32(AS_I32(lxcol)[i], AS_I32(rxcol), AS_I64(AS_LIST(AS_LIST(ht)[1])[idx]),
                                      AS_LIST(AS_LIST(ht)[1])[idx]->len);
                    AS_I64(ids)[i] = p;
                } else
                    AS_I64(ids)[i] = NULL_I64;
            }
            break;
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            for (i = offset; i < len + offset; i++) {
                idx = ht_oa_tab_get_with(ht, i, &__index_list_hash_get, &__index_list_cmp_row, ctx);
                if (idx != NULL_I64) {
                    p = index_bin_i64(AS_I64(lxcol)[i], AS_I64(rxcol), AS_I64(AS_LIST(AS_LIST(ht)[1])[idx]),
                                      AS_LIST(AS_LIST(ht)[1])[idx]->len);
                    AS_I64(ids)[i] = p;
                } else
                    AS_I64(ids)[i] = NULL_I64;
            }
            break;
        default:
            return err_type(0, 0, 0, 0);
    }

    return NULL_OBJ;
}

obj_p index_asof_join_obj(obj_p lcols, obj_p lxcol, obj_p rcols, obj_p rxcol) {
    i64_t i, ll, rl, n, chunk;
    obj_p v, ht, ids, hashes;
    i64_t idx;
    __index_list_ctx_t ctx;
    pool_p pool;

    // asof-join requires at least 2 keys: grouping key(s) + temporal asof key
    // Single-key asof-join is not supported (needs time component)
    if (lcols->len == 0)
        return err_length(2, 1, 1, 0, 0, 0);  // arg 1 = keys

    ll = ops_count(AS_LIST(lcols)[0]);
    rl = ops_count(AS_LIST(rcols)[0]);
    ht = ht_oa_create(rl, TYPE_I64);
    hashes = I64(MAXI64(ll, rl));

    // Right hashes
    __index_list_precalc_hash(rcols, (i64_t *)AS_I64(hashes), rcols->len, rl, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, rcols, (i64_t *)AS_I64(hashes), NULL};
    for (i = 0; i < rl; i++) {
        idx = ht_oa_tab_next_with(&ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        if (AS_I64(AS_LIST(ht)[0])[idx] == NULL_I64) {
            AS_I64(AS_LIST(ht)[0])[idx] = i;
            v = I64(1);
            AS_I64(v)[0] = i;
            AS_LIST(AS_LIST(ht)[1])[idx] = v;
        } else {
            push_raw(AS_LIST(AS_LIST(ht)[1]) + idx, (raw_p)&i);
        }
    }

    ids = I64(ll);

    // Left hashes
    __index_list_precalc_hash(lcols, (i64_t *)AS_I64(hashes), lcols->len, ll, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, lcols, (i64_t *)AS_I64(hashes), NULL};

    pool = pool_get();
    n = pool_split_by(pool, ll, 0);

    if (n == 1) {
        __asof_ids_partial(&ctx, lxcol, rxcol, ht, ll, 0, ids);
        goto clean;
    } else {
        pool_prepare(pool);
        chunk = ll / n;

        for (i = 0; i < n - 1; i++)
            pool_add_task(pool, (raw_p)__asof_ids_partial, 7, &ctx, lxcol, rxcol, ht, chunk, i * chunk, ids);
        pool_add_task(pool, (raw_p)__asof_ids_partial, 7, &ctx, lxcol, rxcol, ht, ll - i * chunk, i * chunk, ids);
        v = pool_run(pool);

        if (IS_ERR(v)) {
            drop_obj(ids);
            drop_obj(hashes);
            drop_obj(ht);
            return v;
        }

        drop_obj(v);
    }

clean:
    drop_obj(hashes);
    rl = AS_LIST(ht)[0]->len;
    for (i = 0; i < rl; i++)
        if (AS_I64(AS_LIST(ht)[0])[i] != NULL_I64)
            drop_obj(AS_LIST(AS_LIST(ht)[1])[i]);

    drop_obj(ht);

    return ids;
}

static obj_p __window_join_fill(__index_list_ctx_t *ctx, obj_p ht, i64_t len, i64_t offset, obj_p out) {
    i64_t i, idx;
    obj_p *ids;

    ids = AS_LIST(out);
    len += offset;

    for (i = offset; i < len; i++) {
        idx = ht_oa_tab_get_with(ht, i, &__index_list_hash_get, &__index_list_cmp_row, ctx);
        if (idx != NULL_I64)
            ids[i] = clone_obj(AS_LIST(AS_LIST(ht)[1])[idx]);
        else
            ids[i] = NULL_OBJ;
    }

    return NULL_OBJ;
}

obj_p index_window_join_obj(obj_p lcols, obj_p lxcol, obj_p rcols, obj_p rxcol, obj_p windows, obj_p ltab, obj_p rtab,
                            i64_t jtype) {
    i64_t i, ll, rl, n, chunk;
    obj_p v, ht, hashes, index;
    i64_t idx;
    __index_list_ctx_t ctx;
    pool_p pool;

    ll = ops_count(ltab);
    rl = ops_count(rtab);
    ht = ht_oa_create(rl, TYPE_I64);
    hashes = I64(MAXI64(ll, rl));

    // Right hashes
    __index_list_precalc_hash(rcols, (i64_t *)AS_I64(hashes), rcols->len, rl, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, rcols, (i64_t *)AS_I64(hashes), NULL};
    for (i = 0; i < rl; i++) {
        idx = ht_oa_tab_next_with(&ht, i, &__index_list_hash_get, &__index_list_cmp_row, &ctx);
        if (AS_I64(AS_LIST(ht)[0])[idx] == NULL_I64) {
            AS_I64(AS_LIST(ht)[0])[idx] = i;
            v = I64(2);
            AS_I64(v)[0] = i;
            AS_I64(v)[1] = i;
            AS_LIST(AS_LIST(ht)[1])[idx] = v;
        } else {
            AS_I64(AS_LIST(AS_LIST(ht)[1])[idx])[1] = i;
        }
    }

    index = LIST(ll);

    // Left hashes
    __index_list_precalc_hash(lcols, (i64_t *)AS_I64(hashes), lcols->len, ll, NULL, B8_TRUE);
    ctx = (__index_list_ctx_t){rcols, lcols, (i64_t *)AS_I64(hashes), NULL};

    pool = pool_get();
    n = pool_split_by(pool, ll, 0);

    if (n == 1) {
        __window_join_fill(&ctx, ht, ll, 0, index);
    } else {
        pool_prepare(pool);
        chunk = ll / n;
        for (i = 0; i < n - 1; i++)
            pool_add_task(pool, (raw_p)__window_join_fill, 5, &ctx, ht, chunk, i * chunk, index);
        pool_add_task(pool, (raw_p)__window_join_fill, 5, &ctx, ht, ll - i * chunk, i * chunk, index);
        v = pool_run(pool);
        drop_obj(v);
    }

    drop_obj(hashes);
    rl = AS_LIST(ht)[0]->len;
    for (i = 0; i < rl; i++)
        if (AS_I64(AS_LIST(ht)[0])[i] != NULL_I64)
            drop_obj(AS_LIST(AS_LIST(ht)[1])[i]);

    drop_obj(ht);

    return index_group_build(INDEX_TYPE_WINDOW, ll, clone_obj(lxcol), clone_obj(rxcol), clone_obj(windows), index,
                             i64(jtype));
}