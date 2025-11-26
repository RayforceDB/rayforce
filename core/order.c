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

#include "order.h"
#include "ops.h"
#include "sort.h"
#include "error.h"
#include "compose.h"
#include "string.h"
#include "pool.h"

obj_p ray_iasc(obj_p x) {
    switch (x->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_I64:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
        case TYPE_LIST:
        case TYPE_SYMBOL:
        case TYPE_DICT:
            return ray_sort_asc(x);
        default:
            THROW(ERR_TYPE, "iasc: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_idesc(obj_p x) {
    switch (x->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_I64:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
        case TYPE_LIST:
        case TYPE_SYMBOL:
        case TYPE_DICT:
            return ray_sort_desc(x);
        default:
            THROW(ERR_TYPE, "idesc: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_asc(obj_p x) {
    obj_p idx, res;
    i64_t l, i;
    u8_t distinct = x->attrs & ATTR_DISTINCT;

    if (x->attrs & ATTR_ASC)
        return clone_obj(x);

    if (x->attrs & ATTR_DESC)
        return ray_reverse(x);

    switch (x->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            idx = ray_sort_asc(x);
            l = x->len;
            res = C8(l);
            res->type = x->type;
            for (i = 0; i < l; i++)
                AS_C8(res)[i] = AS_C8(x)[AS_I64(idx)[i]];
            res->attrs |= ATTR_ASC | distinct;
            drop_obj(idx);
            return res;

        case TYPE_I16:
            idx = ray_sort_asc(x);
            l = x->len;
            res = I16(l);
            for (i = 0; i < l; i++)
                AS_I16(res)[i] = AS_I16(x)[AS_I64(idx)[i]];
            res->attrs |= ATTR_ASC | distinct;
            drop_obj(idx);
            return res;

        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            idx = ray_sort_asc(x);
            l = x->len;
            res = I32(l);
            res->type = x->type;
            for (i = 0; i < l; i++)
                AS_I32(res)[i] = AS_I32(x)[AS_I64(idx)[i]];
            res->attrs |= ATTR_ASC | distinct;
            drop_obj(idx);
            return res;

        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
            idx = ray_sort_asc(x);
            l = x->len;
            for (i = 0; i < l; i++)
                AS_I64(idx)[i] = AS_I64(x)[AS_I64(idx)[i]];
            idx->attrs |= ATTR_ASC | distinct;
            idx->type = x->type;
            return idx;

        case TYPE_LIST:
            idx = ray_sort_asc(x);
            l = x->len;
            res = LIST(l);
            for (i = 0; i < l; i++)
                AS_LIST(res)[i] = clone_obj(AS_LIST(x)[AS_I64(idx)[i]]);
            res->attrs |= ATTR_ASC | distinct;
            drop_obj(idx);
            return res;

        case TYPE_DICT: {
            idx = ray_sort_asc(AS_LIST(x)[1]);
            obj_p sorted_keys = at_obj(AS_LIST(x)[0], idx);
            obj_p sorted_vals = at_obj(AS_LIST(x)[1], idx);

            res = dict(sorted_keys, sorted_vals);
            res->attrs |= ATTR_ASC | distinct;
            drop_obj(idx);
            return res;
        }

        default:
            THROW(ERR_TYPE, "asc: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_desc(obj_p x) {
    obj_p idx, res;
    i64_t l, i;
    u8_t distinct = x->attrs & ATTR_DISTINCT;

    if (x->attrs & ATTR_DESC)
        return clone_obj(x);

    if (x->attrs & ATTR_ASC)
        return ray_reverse(x);

    switch (x->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            idx = ray_sort_desc(x);
            l = x->len;
            res = C8(l);
            res->type = x->type;
            for (i = 0; i < l; i++)
                AS_C8(res)[i] = AS_C8(x)[AS_I64(idx)[i]];
            res->attrs |= ATTR_DESC | distinct;
            drop_obj(idx);
            return res;

        case TYPE_I16:
            idx = ray_sort_desc(x);
            l = x->len;
            res = I16(l);
            for (i = 0; i < l; i++)
                AS_I16(res)[i] = AS_I16(x)[AS_I64(idx)[i]];
            res->attrs |= ATTR_DESC | distinct;
            drop_obj(idx);
            return res;

        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            idx = ray_sort_desc(x);
            l = x->len;
            res = I32(l);
            res->type = x->type;
            for (i = 0; i < l; i++)
                AS_I32(res)[i] = AS_I32(x)[AS_I64(idx)[i]];
            res->attrs |= ATTR_DESC | distinct;
            drop_obj(idx);
            return res;

        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
            idx = ray_sort_desc(x);
            l = x->len;
            for (i = 0; i < l; i++)
                AS_I64(idx)[i] = AS_I64(x)[AS_I64(idx)[i]];
            idx->attrs |= ATTR_DESC | distinct;
            idx->type = x->type;
            return idx;

        case TYPE_LIST:
            idx = ray_sort_desc(x);
            l = x->len;
            res = LIST(l);
            for (i = 0; i < l; i++)
                AS_LIST(res)[i] = clone_obj(AS_LIST(x)[AS_I64(idx)[i]]);
            res->attrs |= ATTR_DESC | distinct;
            drop_obj(idx);
            return res;

        case TYPE_DICT: {
            idx = ray_sort_desc(AS_LIST(x)[1]);
            obj_p sorted_keys = at_obj(AS_LIST(x)[0], idx);
            obj_p sorted_vals = at_obj(AS_LIST(x)[1], idx);

            res = dict(sorted_keys, sorted_vals);
            res->attrs |= ATTR_DESC | distinct;
            drop_obj(idx);
            return res;
        }

        default:
            THROW(ERR_TYPE, "desc: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_xasc(obj_p x, obj_p y) {
    obj_p idx, col, res;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
            col = at_obj(x, y);
            if (IS_ERR(col))
                return col;

            idx = ray_iasc(col);
            drop_obj(col);

            if (IS_ERR(idx))
                return idx;

            res = at_obj(x, idx);
            drop_obj(idx);

            return res;

        case MTYPE2(TYPE_TABLE, TYPE_SYMBOL): {
            i64_t n = y->len;

            // Handle empty symbol vector - return original table
            if (n == 0) {
                return clone_obj(x);
            }

            i64_t nrow = AS_LIST(AS_LIST(x)[1])[0]->len;
            obj_p idx = I64(nrow);
            i64_t* indices = AS_I64(idx);
            for (i64_t i = 0; i < nrow; i++)
                indices[i] = i;

            // Stable sort by each column from last to first, extracting only one column at a time
            for (i64_t c = n - 1; c >= 0; c--) {
                obj_p col_name = at_idx(y, c);
                obj_p col = at_obj(x, col_name);
                drop_obj(col_name);
                if (IS_ERR(col)) {
                    drop_obj(idx);
                    return col;
                }

                obj_p col_reordered = at_obj(col, idx);
                if (IS_ERR(col_reordered)) {
                    drop_obj(col);
                    drop_obj(idx);
                    return col_reordered;
                }
                obj_p local_idx = ray_iasc(col_reordered);
                drop_obj(col);
                if (IS_ERR(local_idx)) {
                    drop_obj(col_reordered);
                    drop_obj(idx);
                    return local_idx;
                }

                // Reorder indices according to local_idx
                i64_t* local = AS_I64(local_idx);
                obj_p obj_tmp = I64(nrow);
                i64_t* tmp = AS_I64(obj_tmp);
                for (i64_t i = 0; i < nrow; i++)
                    tmp[i] = indices[local[i]];

                memcpy(indices, tmp, sizeof(i64_t) * nrow);

                drop_obj(obj_tmp);
                drop_obj(col_reordered);
                drop_obj(local_idx);
            }

            obj_p res = at_obj(x, idx);
            drop_obj(idx);
            return res;
        }

        case MTYPE2(TYPE_TABLE, TYPE_I64):
            // Handle empty vector [] (which has type I64 with length 0)
            if (y->len == 0)
                return clone_obj(x);

            THROW(ERR_TYPE, "xasc: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
        default:
            THROW(ERR_TYPE, "xasc: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_xdesc(obj_p x, obj_p y) {
    obj_p idx, col, res;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
            col = at_obj(x, y);
            if (IS_ERR(col))
                return col;

            idx = ray_idesc(col);
            drop_obj(col);

            if (IS_ERR(idx))
                return idx;

            res = at_obj(x, idx);
            drop_obj(idx);

            return res;

        case MTYPE2(TYPE_TABLE, TYPE_SYMBOL): {
            i64_t n = y->len;

            // Handle empty symbol vector - return original table
            if (n == 0) {
                return clone_obj(x);
            }

            i64_t nrow = AS_LIST(AS_LIST(x)[1])[0]->len;
            obj_p idx = I64(nrow);
            i64_t* indices = AS_I64(idx);
            for (i64_t i = 0; i < nrow; i++)
                indices[i] = i;

            // Stable sort by each column from last to first, extracting only one column at a time
            for (i64_t c = n - 1; c >= 0; c--) {
                obj_p col_name = at_idx(y, c);
                obj_p col = at_obj(x, col_name);
                drop_obj(col_name);
                if (IS_ERR(col)) {
                    drop_obj(idx);
                    return col;
                }

                obj_p col_reordered = at_obj(col, idx);
                if (IS_ERR(col_reordered)) {
                    drop_obj(col);
                    drop_obj(idx);
                    return col_reordered;
                }
                obj_p local_idx = ray_idesc(col_reordered);
                drop_obj(col);
                if (IS_ERR(local_idx)) {
                    drop_obj(col_reordered);
                    drop_obj(idx);
                    return local_idx;
                }

                // Reorder indices according to local_idx
                i64_t* local = AS_I64(local_idx);
                obj_p obj_tmp = I64(nrow);
                i64_t* tmp = AS_I64(obj_tmp);
                for (i64_t i = 0; i < nrow; i++)
                    tmp[i] = indices[local[i]];
                for (i64_t i = 0; i < nrow; i++)
                    indices[i] = tmp[i];

                drop_obj(obj_tmp);
                drop_obj(col_reordered);
                drop_obj(local_idx);
            }

            obj_p res = at_obj(x, idx);
            drop_obj(idx);
            return res;
        }

        case MTYPE2(TYPE_TABLE, TYPE_I64):
            // Handle empty vector [] (which has type I64 with length 0)
            if (y->len == 0)
                return clone_obj(x);

            THROW(ERR_TYPE, "xdesc: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
        default:
            THROW(ERR_TYPE, "xdesc: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_not(obj_p x) {
    i32_t i;
    i64_t l;
    obj_p res;

    switch (x->type) {
        case -TYPE_B8:
            return b8(!x->b8);

        case TYPE_B8:
            l = x->len;
            res = B8(l);
            for (i = 0; i < l; i++)
                AS_B8(res)
            [i] = !AS_B8(x)[i];

            return res;

        default:
            THROW(ERR_TYPE, "not: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_neg(obj_p x) {
    obj_p res;
    i64_t i, l;

    switch (x->type) {
        case -TYPE_B8:
        case -TYPE_U8:
            return i64(-x->b8);
        case -TYPE_I16:
            return i16(-x->i16);
        case -TYPE_I32:
            return i32(-x->i32);
        case -TYPE_I64:
            return i64(-x->i64);
        case -TYPE_F64:
            return f64(-x->f64);
        case TYPE_B8:
        case TYPE_U8:
            l = x->len;
            res = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(res)[i] = -(i64_t)AS_U8(x)[i];
            return res;
        case TYPE_I16:
            l = x->len;
            res = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(res)[i] = -(i64_t)AS_I16(x)[i];
            return res;
        case TYPE_I32:
            l = x->len;
            res = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(res)[i] = -(i64_t)AS_I32(x)[i];
            return res;
        case TYPE_I64:
            l = x->len;
            res = I64(l);
            for (i = 0; i < l; i++)
                AS_I64(res)[i] = -AS_I64(x)[i];
            return res;
        case TYPE_F64:
            l = x->len;
            res = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(res)
            [i] = -AS_F64(x)[i];
            return res;

        default:
            THROW(ERR_TYPE, "neg: unsupported type: '%s", type_name(x->type));
    }
}

typedef struct {
    i64_t* perm;
    i64_t* out;
    i64_t total_len;
} rank_ctx_t;

obj_p rank_desc_worker(i64_t len, i64_t offset, void* ctx) {
    rank_ctx_t* c = ctx;
    for (i64_t i = 0; i < len; i++)
        c->out[offset + i] = c->total_len - 1 - (offset + i);
    return NULL_OBJ;
}

obj_p rank_worker(i64_t len, i64_t offset, void* ctx) {
    rank_ctx_t* c = ctx;
    for (i64_t i = 0; i < len; i++)
        c->out[c->perm[i + offset]] = i + offset;
    return NULL_OBJ;
}

obj_p ray_rank(obj_p x) {
    i64_t l;
    obj_p perm, res;

    // Fast path for sorted vectors
    if (x->attrs & ATTR_ASC) {
        obj_p n = i64(x->len);
        res = ray_til(n);
        drop_obj(n);
        return res;
    }
    if (x->attrs & ATTR_DESC) {
        l = x->len;
        res = I64(l);
        if (IS_ERR(res))
            return res;
        rank_ctx_t ctx = { NULL, AS_I64(res), l };
        pool_map(l, rank_desc_worker, &ctx);
        return res;
    }

    perm = ray_iasc(x);
    if (IS_ERR(perm))
        return perm;

    l = x->len;
    res = I64(l);
    if (IS_ERR(res)) {
        drop_obj(perm);
        return res;
    }

    rank_ctx_t ctx = { AS_I64(perm), AS_I64(res) };
    pool_map(l, rank_worker, &ctx);

    drop_obj(perm);
    return res;
}

typedef struct {
    i64_t* out;
    i64_t n_buckets;
    i64_t total_len;
} xrank_asc_ctx_t;

obj_p xrank_asc_worker(i64_t len, i64_t offset, void* ctx) {
    xrank_asc_ctx_t* c = ctx;
    for (i64_t i = 0; i < len; i++) {
        i64_t idx = i + offset;
        c->out[idx] = (idx * c->n_buckets) / c->total_len;
    }
    return NULL_OBJ;
}

obj_p xrank_desc_worker(i64_t len, i64_t offset, void* ctx) {
    xrank_asc_ctx_t* c = ctx;
    for (i64_t i = 0; i < len; i++) {
        i64_t idx = i + offset;
        c->out[idx] = ((c->total_len - 1 - idx) * c->n_buckets) / c->total_len;
    }
    return NULL_OBJ;
}

typedef struct {
    i64_t* perm;
    i64_t* out;
    i64_t n_buckets;
    i64_t total_len;
} xrank_ctx_t;

obj_p xrank_worker(i64_t len, i64_t offset, void* ctx) {
    xrank_ctx_t* c = ctx;
    for (i64_t i = 0; i < len; i++) {
        i64_t rank = i + offset;
        c->out[c->perm[rank]] = (rank * c->n_buckets) / c->total_len;
    }
    return NULL_OBJ;
}

obj_p ray_xrank(obj_p x, obj_p y) {
    i64_t l, n_buckets;
    obj_p perm, res;

    switch (x->type) {
        case -TYPE_I64: n_buckets = x->i64; break;
        case -TYPE_I32: n_buckets = x->i32; break;
        case -TYPE_I16: n_buckets = x->i16; break;
        case -TYPE_U8:  n_buckets = x->u8;  break;
        default:
            THROW(ERR_TYPE, "xrank: first arg must be integer");
    }
    if (n_buckets <= 0)
        THROW(ERR_TYPE, "xrank: number of buckets must be positive");

    l = y->len;
    res = I64(l);
    if (IS_ERR(res))
        return res;

    // Fast path for sorted vectors
    if (y->attrs & ATTR_ASC) {
        xrank_asc_ctx_t ctx = { AS_I64(res), n_buckets, l };
        pool_map(l, xrank_asc_worker, &ctx);
        return res;
    }
    if (y->attrs & ATTR_DESC) {
        xrank_asc_ctx_t ctx = { AS_I64(res), n_buckets, l };
        pool_map(l, xrank_desc_worker, &ctx);
        return res;
    }

    perm = ray_iasc(y);
    if (IS_ERR(perm)) {
        drop_obj(res);
        return perm;
    }

    xrank_ctx_t ctx = { AS_I64(perm), AS_I64(res), n_buckets, l };
    pool_map(l, xrank_worker, &ctx);

    drop_obj(perm);
    return res;
}
