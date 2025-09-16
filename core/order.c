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
#include "util.h"
#include "ops.h"
#include "items.h"
#include "heap.h"
#include "sort.h"
#include "error.h"
#include "compose.h"
#include <string.h>

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

    if (x->attrs & ATTR_ASC)
        return clone_obj(x);

    if (x->attrs & ATTR_DESC) {
        res = ray_reverse(x);
        res->attrs &= ~ATTR_DESC;
        res->attrs |= ATTR_ASC;
        return res;
    }

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
            res->attrs |= ATTR_ASC;
            drop_obj(idx);
            return res;

        case TYPE_I16:
            idx = ray_sort_asc(x);
            l = x->len;
            res = I16(l);
            for (i = 0; i < l; i++)
                AS_I16(res)[i] = AS_I16(x)[AS_I64(idx)[i]];
            res->attrs |= ATTR_ASC;
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
            res->attrs |= ATTR_ASC;
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
            idx->attrs |= ATTR_ASC;
            idx->type = x->type;
            return idx;

        case TYPE_LIST:
            idx = ray_sort_asc(x);
            l = x->len;
            res = LIST(l);
            for (i = 0; i < l; i++)
                AS_LIST(res)[i] = clone_obj(AS_LIST(x)[AS_I64(idx)[i]]);
            res->attrs |= ATTR_ASC;
            drop_obj(idx);
            return res;

        case TYPE_DICT: {
            idx = ray_sort_asc(AS_LIST(x)[1]);
            obj_p sorted_keys = at_obj(AS_LIST(x)[0], idx);
            obj_p sorted_vals = at_obj(AS_LIST(x)[1], idx);

            res = dict(sorted_keys, sorted_vals);
            res->attrs |= ATTR_ASC;
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

    if (x->attrs & ATTR_DESC)
        return clone_obj(x);

    if (x->attrs & ATTR_ASC) {
        res = ray_reverse(x);
        res->attrs &= ~ATTR_ASC;
        res->attrs |= ATTR_DESC;
        return res;
    }

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
            res->attrs |= ATTR_DESC;
            drop_obj(idx);
            return res;

        case TYPE_I16:
            idx = ray_sort_desc(x);
            l = x->len;
            res = I16(l);
            for (i = 0; i < l; i++)
                AS_I16(res)[i] = AS_I16(x)[AS_I64(idx)[i]];
            res->attrs |= ATTR_DESC;
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
            res->attrs |= ATTR_DESC;
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
            idx->attrs |= ATTR_DESC;
            idx->type = x->type;
            return idx;

        case TYPE_LIST:
            idx = ray_sort_desc(x);
            l = x->len;
            res = LIST(l);
            for (i = 0; i < l; i++)
                AS_LIST(res)[i] = clone_obj(AS_LIST(x)[AS_I64(idx)[i]]);
            res->attrs |= ATTR_DESC;
            drop_obj(idx);
            return res;

        case TYPE_DICT: {
            idx = ray_sort_desc(AS_LIST(x)[1]);
            obj_p sorted_keys = at_obj(AS_LIST(x)[0], idx);
            obj_p sorted_vals = at_obj(AS_LIST(x)[1], idx);

            res = dict(sorted_keys, sorted_vals);
            res->attrs |= ATTR_DESC;
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
            i64_t *indices = AS_I64(idx);
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
                i64_t *local = AS_I64(local_idx);
                i64_t *tmp = malloc(sizeof(i64_t) * nrow);
                for (i64_t i = 0; i < nrow; i++)
                    tmp[i] = indices[local[i]];

                memcpy(indices, tmp, sizeof(i64_t) * nrow);
                free(tmp);

                drop_obj(col_reordered);
                drop_obj(local_idx);
            }

            obj_p res = at_obj(x, idx);
            drop_obj(idx);
            return res;
        }

        case MTYPE2(TYPE_TABLE, TYPE_I64):
            // Handle empty vector [] (which has type I64 with length 0)
            if (y->len == 0) {
                return clone_obj(x);
            }
            // Fall through to default error case

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
            i64_t *indices = AS_I64(idx);
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
                i64_t *local = AS_I64(local_idx);
                i64_t *tmp = malloc(sizeof(i64_t) * nrow);
                for (i64_t i = 0; i < nrow; i++)
                    tmp[i] = indices[local[i]];
                for (i64_t i = 0; i < nrow; i++)
                    indices[i] = tmp[i];
                free(tmp);

                drop_obj(col_reordered);
                drop_obj(local_idx);
            }

            obj_p res = at_obj(x, idx);
            drop_obj(idx);
            return res;
        }

        case MTYPE2(TYPE_TABLE, TYPE_I64):
            // Handle empty vector [] (which has type I64 with length 0)
            if (y->len == 0) {
                return clone_obj(x);
            }
            // Fall through to default error case

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
