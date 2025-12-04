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

#include "items.h"
#include "util.h"
#include "ops.h"
#include "unary.h"
#include "serde.h"
#include "runtime.h"
#include "compose.h"
#include "order.h"
#include "error.h"
#include "aggr.h"
#include "index.h"
#include "string.h"
#include "pool.h"
#include "cmp.h"
#include "iter.h"

obj_p ray_at(obj_p x, obj_p y) {
    i64_t i, j, yl, xl, n, size;
    obj_p res, k, s, v, cols;
    u8_t *buf;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_B8, -TYPE_I64):
        case MTYPE2(TYPE_I32, -TYPE_I64):
        case MTYPE2(TYPE_DATE, -TYPE_I64):
        case MTYPE2(TYPE_TIME, -TYPE_I64):
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_F64, -TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_I64):
        case MTYPE2(TYPE_GUID, -TYPE_I64):
        case MTYPE2(TYPE_C8, -TYPE_I64):
        case MTYPE2(TYPE_LIST, -TYPE_I64):
            return at_idx(x, y->i64);
        case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
            return at_obj(x, y);
        case MTYPE2(TYPE_B8, TYPE_I64):
            yl = y->len;
            xl = x->len;
            res = B8(yl);
            for (i = 0; i < yl; i++) {
                if (AS_B8(y)[i] >= (i64_t)xl)
                    AS_B8(res)[i] = B8_FALSE;
                else
                    AS_B8(res)[i] = AS_B8(x)[(i32_t)AS_B8(y)[i]];
            }

            return res;
        case MTYPE2(TYPE_I32, TYPE_I64):
        case MTYPE2(TYPE_DATE, TYPE_I64):
        case MTYPE2(TYPE_TIME, TYPE_I64):
            yl = y->len;
            xl = x->len;
            res = vector(x->type, yl);
            for (i = 0; i < yl; i++) {
                if (AS_I64(y)[i] >= (i64_t)xl)
                    AS_I32(res)[i] = NULL_I32;
                else
                    AS_I32(res)[i] = AS_I32(x)[(i32_t)AS_I64(y)[i]];
            }

            return res;
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_I64):
            yl = y->len;
            xl = x->len;
            res = vector(x->type, yl);
            for (i = 0; i < yl; i++) {
                if (AS_I64(y)[i] >= (i64_t)xl)
                    AS_I64(res)[i] = NULL_I64;
                else
                    AS_I64(res)[i] = AS_I64(x)[AS_I64(y)[i]];
            }

            return res;
        case MTYPE2(TYPE_F64, TYPE_I64):
            yl = y->len;
            xl = x->len;
            res = F64(yl);
            for (i = 0; i < yl; i++) {
                if (AS_I64(y)[i] >= (i64_t)xl)
                    AS_F64(res)[i] = NULL_F64;
                else
                    AS_F64(res)[i] = AS_F64(x)[AS_I64(y)[i]];
            }

            return res;
        case MTYPE2(TYPE_GUID, TYPE_I64):
            yl = y->len;
            xl = x->len;
            res = GUID(yl);
            for (i = 0; i < yl; i++) {
                if (AS_I64(y)[i] >= (i64_t)xl)
                    memset(AS_GUID(res)[i], 0, sizeof(guid_t));
                else
                    memcpy(AS_GUID(res)[i], AS_GUID(x)[AS_I64(y)[i]], sizeof(guid_t));
            }

            return res;
        case MTYPE2(TYPE_C8, TYPE_I64):
            yl = y->len;
            xl = x->len;
            res = C8(yl);
            for (i = 0; i < yl; i++) {
                if (AS_I64(y)[i] >= (i64_t)xl)
                    AS_C8(res)
                [i] = ' ';
                else AS_C8(res)[i] = AS_C8(x)[(i32_t)AS_I64(y)[i]];
            }

            return res;
        case MTYPE2(TYPE_LIST, TYPE_I64):
            yl = y->len;
            xl = x->len;
            res = vector(TYPE_LIST, yl);
            for (i = 0; i < yl; i++) {
                if (AS_I64(y)[i] >= (i64_t)xl)
                    AS_LIST(res)
                [i] = NULL_OBJ;
                else AS_LIST(res)[i] = clone_obj(AS_LIST(x)[(i32_t)AS_I64(y)[i]]);
            }

            return res;
        case MTYPE2(TYPE_TABLE, TYPE_SYMBOL):
            xl = AS_LIST(x)[1]->len;
            yl = y->len;
            if (yl == 0)
                return NULL_OBJ;
            if (yl == 1) {
                for (j = 0; j < xl; j++) {
                    if (AS_SYMBOL(AS_LIST(x)[0])[j] == AS_SYMBOL(y)[0])
                        return clone_obj(AS_LIST(AS_LIST(x)[1])[j]);
                }

                THROW(ERR_INDEX, "at: column '%s' has not found in a table", str_from_symbol(AS_SYMBOL(y)[0]));
            }

            cols = vector(TYPE_LIST, yl);
            for (i = 0; i < yl; i++) {
                for (j = 0; j < xl; j++) {
                    if (AS_SYMBOL(AS_LIST(x)[0])[j] == AS_SYMBOL(y)[i]) {
                        AS_LIST(cols)
                        [i] = clone_obj(AS_LIST(AS_LIST(x)[1])[j]);
                        break;
                    }
                }

                if (j == xl) {
                    cols->len = i;
                    drop_obj(cols);
                    THROW(ERR_INDEX, "at: column '%s' has not found in a table", str_from_symbol(AS_SYMBOL(y)[i]));
                }
            }

            return cols;
        case MTYPE2(TYPE_ENUM, -TYPE_I64):
            k = ray_key(x);
            s = ray_get(k);
            drop_obj(k);

            v = ENUM_VAL(x);

            if (y->i64 >= (i64_t)v->len) {
                drop_obj(s);
                THROW(ERR_INDEX, "at: enum can not be resolved: index out of range");
            }

            if (!s || IS_ERR(s) || s->type != TYPE_SYMBOL) {
                drop_obj(s);
                return i64(AS_I64(v)[y->i64]);
            }

            if (AS_I64(v)[y->i64] >= (i64_t)s->len) {
                drop_obj(s);
                THROW(ERR_INDEX, "at: enum can not be resolved: index out of range");
            }

            res = at_idx(s, AS_I64(v)[y->i64]);

            drop_obj(s);

            return res;

        case MTYPE2(TYPE_ENUM, TYPE_I64):
            k = ray_key(x);
            v = ENUM_VAL(x);

            s = ray_get(k);
            drop_obj(k);

            if (IS_ERR(s))
                return s;

            xl = s->len;
            yl = y->len;
            n = v->len;

            if (!s || s->type != TYPE_SYMBOL) {
                res = I64(yl);

                for (i = 0; i < yl; i++) {
                    if (AS_I64(y)[i] >= (i64_t)n) {
                        drop_obj(s);
                        drop_obj(res);
                        THROW(ERR_INDEX, "at: enum can not be resolved: index out of range");
                    }

                    AS_I64(res)[i] = AS_I64(v)[AS_I64(y)[i]];
                }

                drop_obj(s);

                return res;
            }

            res = SYMBOL(yl);

            for (i = 0; i < yl; i++) {
                if (AS_I64(v)[i] >= (i64_t)xl) {
                    drop_obj(s);
                    drop_obj(res);
                    THROW(ERR_INDEX, "at: enum can not be resolved: index out of range");
                }

                AS_SYMBOL(res)[i] = AS_SYMBOL(s)[AS_I64(v)[AS_I64(y)[i]]];
            }

            drop_obj(s);

            return res;

        case MTYPE2(TYPE_MAPLIST, -TYPE_I64):
            k = MAPLIST_KEY(x);
            v = MAPLIST_VAL(x);

            xl = k->len;
            yl = v->len;

            if (y->i64 >= (i64_t)v->len)
                THROW(ERR_INDEX, "at: anymap can not be resolved: index out of range");

            buf = AS_U8(k) + AS_I64(v)[y->i64];

            return de_raw(buf, &xl);

        case MTYPE2(TYPE_MAPLIST, TYPE_I64):
            k = MAPLIST_KEY(x);
            v = MAPLIST_VAL(x);

            size = k->len;

            n = v->len;
            yl = y->len;

            res = vector(TYPE_LIST, yl);

            for (i = 0; i < yl; i++) {
                if (AS_I64(y)[i] >= (i64_t)n) {
                    res->len = i;
                    drop_obj(res);
                    THROW(ERR_INDEX, "at: anymap can not be resolved: index out of range");
                }

                buf = AS_U8(k) + AS_I64(v)[AS_I64(y)[i]];
                AS_LIST(res)[i] = de_raw(buf, &size);
            }

            return res;

        default:
            return at_obj(x, y);
    }
}

obj_p ray_find(obj_p x, obj_p y) {
    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_B8, -TYPE_B8):
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
        case MTYPE2(TYPE_F64, -TYPE_F64):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        case MTYPE2(TYPE_GUID, -TYPE_GUID):
        case MTYPE2(TYPE_C8, -TYPE_C8):
            return i64(find_obj_idx(x, y));
        case MTYPE2(TYPE_B8, TYPE_B8):
        case MTYPE2(TYPE_U8, TYPE_U8):
        case MTYPE2(TYPE_C8, TYPE_C8):
            return index_find_i8((i8_t *)AS_U8(x), x->len, (i8_t *)AS_U8(y), y->len);
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            return index_find_i64(AS_I64(x), x->len, AS_I64(y), y->len);
        case MTYPE2(TYPE_F64, TYPE_F64):
            return index_find_i64((i64_t *)AS_F64(x), x->len, (i64_t *)AS_F64(y), y->len);
        case MTYPE2(TYPE_GUID, TYPE_GUID):
            return index_find_guid(AS_GUID(x), x->len, AS_GUID(y), y->len);
        case MTYPE2(TYPE_LIST, TYPE_LIST):
            return index_find_obj(AS_LIST(x), x->len, AS_LIST(y), y->len);
        default:
            THROW(ERR_TYPE, "find: unsupported types: '%s '%s", type_name(x->type), type_name(y->type));
    }
}

#define COMMA ,
#define __FILTER(x, y, tx, s1, s2, s3)                                                 \
    ({                                                                                 \
        if (x->len != y->len)                                                          \
            return error_str(ERR_LENGTH, "filter: arguments must be the same length"); \
        l = x->len;                                                                    \
        res = tx(l);                                                                   \
        for (i = 0; i < l; i++)                                                        \
            if (AS_B8(y)[i])                                                           \
                s1(AS_##tx(res)[j++] s2(AS_##tx(x)[i]) s3);                            \
        resize_obj(&res, j);                                                           \
        return res;                                                                    \
    })

obj_p ray_filter(obj_p x, obj_p y) {
    i64_t i, j = 0, l;
    obj_p res, vals;
    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_B8, TYPE_B8):
            __FILTER(x, y, B8, , =, );
        case MTYPE2(TYPE_U8, TYPE_B8):
            __FILTER(x, y, U8, , =, );
        case MTYPE2(TYPE_C8, TYPE_B8):
            __FILTER(x, y, C8, , =, );
        case MTYPE2(TYPE_I16, TYPE_B8):
            __FILTER(x, y, I16, , =, );
        case MTYPE2(TYPE_I32, TYPE_B8):
            __FILTER(x, y, I32, , =, );
        case MTYPE2(TYPE_DATE, TYPE_B8):
            __FILTER(x, y, DATE, , =, );
        case MTYPE2(TYPE_TIME, TYPE_B8):
            __FILTER(x, y, TIME, , =, );
        case MTYPE2(TYPE_I64, TYPE_B8):
            __FILTER(x, y, I64, , =, );
        case MTYPE2(TYPE_SYMBOL, TYPE_B8):
            __FILTER(x, y, SYMBOL, , =, );
        case MTYPE2(TYPE_TIMESTAMP, TYPE_B8):
            __FILTER(x, y, TIMESTAMP, , =, );
        case MTYPE2(TYPE_F64, TYPE_B8):
            __FILTER(x, y, F64, , =, );

        case MTYPE2(TYPE_GUID, TYPE_B8):
            __FILTER(x, y, GUID, memcpy, COMMA, COMMA sizeof(guid_t));

        case MTYPE2(TYPE_LIST, TYPE_B8):
            __FILTER(x, y, LIST, , = clone_obj, );

        case MTYPE2(TYPE_TABLE, TYPE_B8):
            vals = AS_LIST(x)[1];
            l = vals->len;
            res = LIST(l);
            for (i = 0; i < l; i++) {
                AS_LIST(res)[i] = ray_filter(AS_LIST(vals)[i], y);
            }
            return table(clone_obj(AS_LIST(x)[0]), res);

        default:
            THROW(ERR_TYPE, "filter: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_take(obj_p x, obj_p y) {
    i64_t i, j, f, l, m, n, size;
    obj_p k, s, v, res;
    u8_t *buf;

    switch (x->type) {
        case -TYPE_I64:
            f = 0 > x->i64;
            m = ABSI64(x->i64);
            break;
        case -TYPE_I16:
            f = 0 > x->i16;
            m = ABSI64((i64_t)x->i16);
            break;
        case -TYPE_I32:
            f = 0 > x->i32;
            m = ABSI64((i64_t)x->i32);
            break;
        default:
            THROW(ERR_TYPE, "take: unsupported types: '%s, %s", type_name(x->type), type_name(y->type));
    }

    switch (y->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            l = y->len;
            res = vector(y->type, m);
            for (i = 0, j = (l - m % l) * f; i < m; i++, j++)
                AS_U8(res)[i] = AS_U8(y)[j % l];
            return res;

        case -TYPE_B8:
        case -TYPE_U8:
        case -TYPE_C8:
            res = vector(-y->type, m);
            for (i = 0; i < m; i++)
                AS_U8(res)[i] = y->u8;
            return res;

        case TYPE_I16:
            l = y->len;
            res = I16(m);
            for (i = 0, j = (l - m % l) * f; i < m; i++, j++)
                AS_I16(res)[i] = AS_I16(y)[j % l];
            return res;

        case -TYPE_I16:
            res = vector(-y->type, m);
            for (i = 0; i < m; i++)
                AS_I16(res)[i] = y->i16;
            return res;

        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            l = y->len;
            res = vector(y->type, m);
            for (i = 0, j = (l - m % l) * f; i < m; i++, j++)
                AS_I32(res)[i] = AS_I32(y)[j % l];
            return res;

        case -TYPE_I32:
        case -TYPE_DATE:
        case -TYPE_TIME:
            res = vector(-y->type, m);
            for (i = 0; i < m; i++)
                AS_I32(res)[i] = y->i32;
            return res;

        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
            l = y->len;
            res = vector(y->type, m);
            for (i = 0, j = (l - m % l) * f; i < m; i++, j++)
                AS_I64(res)[i] = AS_I64(y)[j % l];
            return res;

        case -TYPE_I64:
        case -TYPE_SYMBOL:
        case -TYPE_TIMESTAMP:
        case -TYPE_F64:
            res = vector(-y->type, m);
            for (i = 0; i < m; i++)
                AS_I64(res)[i] = y->i64;
            return res;

        case TYPE_GUID:
            l = y->len;
            res = GUID(m);
            for (i = 0, j = (l - m % l) * f; i < m; i++, j++)
                memcpy(AS_GUID(res)[i], AS_GUID(y)[j % l], sizeof(guid_t));
            return res;

        case -TYPE_GUID:
            res = GUID(m);
            for (i = 0; i < m; i++)
                memcpy(AS_GUID(res)[i], AS_GUID(y)[0], sizeof(guid_t));
            return res;

        case TYPE_DICT:
            l = AS_LIST(y)[0]->len;
            obj_p keys_res = vector(AS_LIST(y)[0]->type, 0);
            obj_p vals_res = vector(AS_LIST(y)[1]->type, 0);
            for (i = 0, j = (l - m % l) * f; i < m; i++, j++) {
                push_obj(&keys_res, clone_obj(at_idx(AS_LIST(y)[0], j % l)));
                push_obj(&vals_res, clone_obj(at_idx(AS_LIST(y)[1], j % l)));
            }
            return dict(keys_res, vals_res);

        case TYPE_ENUM:
            k = ray_key(y);
            s = ray_get(k);
            v = ENUM_VAL(y);
            l = v->len;
            res = I64(m);
            for (i = 0, j = (l - m % l) * f; i < m; i++, j++) {
                AS_I64(res)[i] = AS_I64(v)[j % l];
            }
            drop_obj(s);
            if (s->type != TYPE_SYMBOL) {
                drop_obj(k);
                return res;
            } else
                return enumerate(k, res);

        case TYPE_MAPLIST:
            k = MAPLIST_KEY(y);
            s = MAPLIST_VAL(y);
            n = k->len;
            l = s->len;
            res = vector(TYPE_LIST, m);
            size = m;

            for (i = 0, j = (l - m % l) * f; i < m; i++, j++) {
                if (AS_I64(s)[j % l] >= (i64_t)n) {
                    buf = AS_U8(k) + AS_I64(s)[j % l];
                    v = de_raw(buf, &size);
                    if (IS_ERR(v)) {
                        res->len = i;
                        drop_obj(res);
                        return v;
                    }
                    AS_LIST(res)[i] = v;
                } else {
                    res->len = i;
                    drop_obj(res);
                    THROW(ERR_INDEX, "anymap value: index out of range: %d", AS_I64(s)[j % l]);
                }
            }
            return res;

        case TYPE_LIST:
            l = y->len;
            res = vector(TYPE_LIST, m);
            for (i = 0, j = (l - m % l) * f; i < m; i++, j++)
                AS_LIST(res)[i] = clone_obj(AS_LIST(y)[j % l]);
            return res;

        case TYPE_TABLE:
            n = AS_LIST(y)[1]->len;
            res = vector(TYPE_LIST, n);
            for (i = 0; i < n; i++) {
                v = ray_take(x, AS_LIST(AS_LIST(y)[1])[i]);

                if (IS_ERR(v)) {
                    res->len = i;
                    drop_obj(res);
                    return v;
                }

                AS_LIST(res)[i] = v;
            }
            return table(clone_obj(AS_LIST(y)[0]), res);

        default:
            THROW(ERR_TYPE, "take: unsupported types: '%s, %s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_in(obj_p x, obj_p y) {
    i64_t i;
    obj_p vec;

    if (IS_ATOM(x) && IS_ATOM(y))
        return b8(cmp_obj(x, y) == 0);

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_U8, TYPE_U8):
        case MTYPE2(TYPE_B8, TYPE_B8):
        case MTYPE2(TYPE_C8, TYPE_C8):
            return index_in_i8_i8(AS_I8(x), x->len, AS_I8(y), y->len);
        case MTYPE2(TYPE_U8, TYPE_I16):
        case MTYPE2(TYPE_B8, TYPE_I16):
            return index_in_i8_i16(AS_I8(x), x->len, AS_I16(y), y->len);
        case MTYPE2(TYPE_U8, TYPE_I32):
        case MTYPE2(TYPE_B8, TYPE_I32):
            return index_in_i8_i32(AS_I8(x), x->len, AS_I32(y), y->len);
        case MTYPE2(TYPE_U8, TYPE_I64):
        case MTYPE2(TYPE_B8, TYPE_I64):
            return index_in_i8_i64(AS_I8(x), x->len, AS_I64(y), y->len);
        case MTYPE2(TYPE_I16, TYPE_U8):
            return index_in_i16_i8(AS_I16(x), x->len, AS_I8(y), y->len);
        case MTYPE2(TYPE_I16, TYPE_I16):
            return index_in_i16_i16(AS_I16(x), x->len, AS_I16(y), y->len);
        case MTYPE2(TYPE_I16, TYPE_I32):
            return index_in_i16_i32(AS_I16(x), x->len, AS_I32(y), y->len);
        case MTYPE2(TYPE_I16, TYPE_I64):
            return index_in_i16_i64(AS_I16(x), x->len, AS_I64(y), y->len);
        case MTYPE2(TYPE_I32, TYPE_U8):
            return index_in_i32_i8(AS_I32(x), x->len, AS_I8(y), y->len);
        case MTYPE2(TYPE_I32, TYPE_I16):
            return index_in_i32_i16(AS_I32(x), x->len, AS_I16(y), y->len);
        case MTYPE2(TYPE_I32, TYPE_I32):
        case MTYPE2(TYPE_DATE, TYPE_DATE):
        case MTYPE2(TYPE_TIME, TYPE_TIME):
            return index_in_i32_i32(AS_I32(x), x->len, AS_I32(y), y->len);
        case MTYPE2(TYPE_I32, TYPE_I64):
            return index_in_i32_i64(AS_I32(x), x->len, AS_I64(y), y->len);
        case MTYPE2(TYPE_I64, TYPE_U8):
            return index_in_i64_i8(AS_I64(x), x->len, AS_I8(y), y->len);
        case MTYPE2(TYPE_I64, TYPE_I16):
            return index_in_i64_i16(AS_I64(x), x->len, AS_I16(y), y->len);
        case MTYPE2(TYPE_I64, TYPE_I32):
            return index_in_i64_i32(AS_I64(x), x->len, AS_I32(y), y->len);
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            return index_in_i64_i64(AS_I64(x), x->len, AS_I64(y), y->len);
        case MTYPE2(TYPE_GUID, TYPE_GUID):
            return index_in_guid_guid(AS_GUID(x), x->len, AS_GUID(y), y->len);
        default:
            if ((IS_VECTOR(y) || y->type == TYPE_LIST) && y->len == 0) {
                if (IS_VECTOR(x) || x->type == TYPE_LIST) {
                    vec = B8(x->len);
                    memset(AS_B8(vec), 0, x->len);
                    return vec;
                } else
                    return b8(0);
            }

            // Handle TYPE_MAPCOMMON (parted column like Date)
            // Structure: AS_LIST(x)[0] = unique values, AS_LIST(x)[1] = counts per partition
            if (x->type == TYPE_MAPCOMMON) {
                obj_p vals = AS_LIST(x)[0];
                i64_t l = vals->len;
                obj_p res = LIST(l);
                res->type = TYPE_PARTEDB8;

                // For each unique value, check if it's in y
                for (i = 0; i < l; i++) {
                    obj_p v = at_idx(vals, i);
                    obj_p match = ray_in(v, y);
                    drop_obj(v);

                    if (IS_ERR(match)) {
                        res->len = i;
                        drop_obj(res);
                        return match;
                    }

                    // Convert result to b8(B8_TRUE) or NULL_OBJ for parted boolean
                    if (match->type == -TYPE_B8 && match->b8) {
                        AS_LIST(res)[i] = b8(B8_TRUE);
                    } else {
                        AS_LIST(res)[i] = NULL_OBJ;
                    }
                    drop_obj(match);
                }

                return res;
            }

            if (x->type == TYPE_LIST) {
                vec = LIST(x->len);
                for (i = 0; i < (i64_t)x->len; i++)
                    AS_LIST(vec)[i] = ray_in(AS_LIST(x)[i], y);
                return vec;
            }

            if (IS_VECTOR(x) || !IS_VECTOR(y))
                return map_binary_left_fn(ray_in, 0, x, y);

            if (!IS_VECTOR(x))
                return b8(find_obj_idx(y, x) != NULL_I64);

            THROW(ERR_TYPE, "in: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }

    return NULL_OBJ;
}

obj_p ray_within(obj_p x, obj_p y) {
    i64_t i, l, min, max;
    obj_p res;

    if (!IS_VECTOR(y) || y->len != 2)
        return error_str(ERR_TYPE, "within: second argument must be a 2-element vector");

    switch
        MTYPE2(x->type, y->type) {
            case MTYPE2(-TYPE_I64, TYPE_I64):
                min = AS_I64(y)[0];
                max = AS_I64(y)[1];

                return B8(x->i64 >= min && x->i64 <= max);

            case MTYPE2(TYPE_I64, TYPE_I64):
                l = x->len;
                min = AS_I64(y)[0];
                max = AS_I64(y)[1];
                res = B8(l);

                for (i = 0; i < l; i++)
                    AS_B8(res)[i] = AS_I64(x)[i] >= min && AS_I64(x)[i] <= max;

                return res;

            case MTYPE2(TYPE_MAPCOMMON, TYPE_TIMESTAMP):
                l = AS_LIST(x)[0]->len;
                min = AS_I64(y)[0];
                max = AS_I64(y)[1];

                res = LIST(l);
                res->type = TYPE_PARTEDB8;

                for (i = 0; i < l; i++) {
                    if (AS_TIMESTAMP(AS_LIST(x)[0])[i] < min || AS_TIMESTAMP(AS_LIST(x)[0])[i] > max)
                        AS_LIST(res)[i] = NULL_OBJ;
                    else
                        AS_LIST(res)[i] = b8(B8_TRUE);
                }

                return res;

            default:
                THROW(ERR_TYPE, "within: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
        }

    return NULL_OBJ;
}

obj_p ray_sect(obj_p x, obj_p y) {
    obj_p mask, res;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
            mask = ray_in(x, y);
            res = ray_filter(x, mask);
            drop_obj(mask);
            return res;

        default:
            THROW(ERR_TYPE, "sect: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }

    return NULL_OBJ;
}

obj_p ray_except(obj_p x, obj_p y) {
    i64_t i, j, l;
    obj_p mask, nmask, ids, k, v, res;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
            l = x->len;
            res = vector(x->type, l);

            for (i = 0, j = 0; i < l; i++) {
                if (AS_I64(x)[i] != y->i64)
                    AS_I64(res)[j++] = AS_I64(x)[i];
            }

            resize_obj(&res, j);

            return res;
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
            mask = ray_in(x, y);
            if (IS_ERR(mask))
                return mask;

            nmask = ray_not(mask);
            drop_obj(mask);

            if (IS_ERR(nmask))
                return nmask;

            res = ray_filter(x, nmask);
            drop_obj(nmask);
            return res;
        case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
            mask = ray_ne(AS_LIST(x)[0], y);
            if (IS_ERR(mask))
                return mask;

            ids = ray_where(mask);
            drop_obj(mask);

            if (IS_ERR(ids))
                return ids;

            k = ray_at(AS_LIST(x)[0], ids);
            v = ray_at(AS_LIST(x)[1], ids);
            drop_obj(ids);

            return table(k, v);
        case MTYPE2(TYPE_TABLE, TYPE_SYMBOL):
            mask = ray_in(AS_LIST(x)[0], y);
            if (IS_ERR(mask))
                return mask;

            nmask = ray_not(mask);
            drop_obj(mask);

            if (IS_ERR(nmask))
                return nmask;

            ids = ray_where(nmask);
            drop_obj(nmask);

            if (IS_ERR(ids))
                return ids;

            k = ray_at(AS_LIST(x)[0], ids);
            v = ray_at(AS_LIST(x)[1], ids);

            drop_obj(ids);

            return table(k, v);
        default:
            if (x->type == TYPE_LIST) {
                if (y->type == TYPE_LIST) {
                    // TODO!!!!
                } else {
                    l = x->len;
                    res = LIST(l);

                    for (i = 0, j = 0; i < l; i++) {
                        mask = ray_eq(AS_LIST(x)[i], y);

                        if (IS_ERR(mask)) {
                            res->len = i;
                            drop_obj(res);
                            return mask;
                        }

                        if (!mask->b8)
                            AS_LIST(res)[j++] = clone_obj(AS_LIST(x)[i]);

                        drop_obj(mask);
                    }

                    resize_obj(&res, j);

                    return res;
                }
            }

            THROW(ERR_TYPE, "except: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

// TODO: implement
obj_p ray_union(obj_p x, obj_p y) {
    obj_p c, res;

    c = ray_concat(x, y);
    res = ray_distinct(c);
    drop_obj(c);
    return res;
}

obj_p ray_first(obj_p x) {
    switch (x->type) {
        case TYPE_MAPGROUP:
            return aggr_first(AS_LIST(x)[0], AS_LIST(x)[1]);
        default:
            return at_idx(x, 0);
    }
}

obj_p ray_last(obj_p x) {
    i64_t l;

    switch (x->type) {
        case TYPE_MAPGROUP:
            return aggr_last(AS_LIST(x)[0], AS_LIST(x)[1]);
        default:
            l = ops_count(x);
            return at_idx(x, l ? l - 1 : l);
    }
}

obj_p ray_key(obj_p x) {
    i64_t l;
    lit_p k;

    switch (x->type) {
        case TYPE_TABLE:
        case TYPE_DICT:
            return clone_obj(AS_LIST(x)[0]);
        case TYPE_ENUM:
            k = ENUM_KEY(x);
            l = strlen(k);
            return symbol(k, l);
        case TYPE_MAPLIST:
            return clone_obj(MAPLIST_KEY(x));
        default:
            return clone_obj(x);
    }
}

obj_p ray_value(obj_p x) {
    obj_p sym, k, v, res, e, *objptr;
    u8_t *u8ptr, *buf;
    i64_t size;
    i64_t i, j, l, n, sl, xl, *i64ptr;
    f64_t *f64ptr;
    guid_t *guidptr;

    switch (x->type) {
        case TYPE_ENUM:
            k = ray_key(x);
            sym = at_obj(runtime_get()->env.variables, k);
            drop_obj(k);

            e = ENUM_VAL(x);
            xl = e->len;

            if (is_null(sym) || sym->type != TYPE_SYMBOL) {
                res = I64(xl);

                for (i = 0; i < xl; i++)
                    AS_I64(res)
                [i] = AS_I64(e)[i];

                drop_obj(sym);

                return res;
            }

            sl = sym->len;

            res = SYMBOL(xl);

            for (i = 0; i < xl; i++) {
                if (AS_I64(e)[i] < sl)
                    AS_SYMBOL(res)[i] = AS_SYMBOL(sym)[AS_I64(e)[i]];
                else
                    AS_SYMBOL(res)[i] = NULL_I64;
            }

            drop_obj(sym);

            return res;

        case TYPE_MAPLIST:
            k = MAPLIST_KEY(x);
            e = MAPLIST_VAL(x);

            xl = e->len;
            sl = k->len;

            size = sl;

            res = vector(TYPE_LIST, xl);

            for (i = 0; i < xl; i++) {
                if (AS_I64(e)[i] < sl) {
                    buf = AS_U8(k) + AS_I64(e)[i];
                    v = de_raw(buf, &size);

                    if (IS_ERR(v)) {
                        res->len = i;
                        drop_obj(res);
                        return v;
                    }

                    AS_LIST(res)[i] = v;
                } else {
                    res->len = i;
                    drop_obj(res);
                    THROW(ERR_INDEX, "anymap value: index out of range: %d", AS_I64(e)[i]);
                }
            }

            return res;

        case TYPE_TABLE:
        case TYPE_DICT:
            return clone_obj(AS_LIST(x)[1]);

        case TYPE_PARTEDLIST:
            l = x->len;
            n = ops_count(x);
            res = LIST(n);
            objptr = AS_LIST(res);

            // WARNING: here is assumed that inside parted list there are only map lists
            for (i = 0; i < l; i++) {
                n = ops_count(AS_LIST(x)[i]);
                k = MAPLIST_KEY(AS_LIST(x)[i]);
                v = MAPLIST_VAL(AS_LIST(x)[i]);
                size = k->len;

                for (j = 0; j < n; j++) {
                    buf = AS_U8(k) + AS_I64(v)[j];
                    objptr[j] = de_raw(buf, &size);
                }

                objptr += n;
            }

            return res;

        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
            l = x->len;
            n = ops_count(x);
            res = vector(AS_LIST(x)[0]->type, n);
            u8ptr = AS_U8(res);

            for (i = 0; i < l; i++) {
                n = AS_LIST(x)[i]->len;
                memcpy(u8ptr, AS_U8(AS_LIST(x)[i]), n * sizeof(i8_t));
                u8ptr += n;
            }

            return res;
        case TYPE_PARTEDI64:
        case TYPE_PARTEDTIMESTAMP:
            l = x->len;
            n = ops_count(x);
            res = vector(AS_LIST(x)[0]->type, n);
            i64ptr = AS_I64(res);

            for (i = 0; i < l; i++) {
                n = AS_LIST(x)[i]->len;
                memcpy(i64ptr, AS_I64(AS_LIST(x)[i]), n * sizeof(i64_t));
                i64ptr += n;
            }

            return res;
        case TYPE_PARTEDF64:
            l = x->len;
            n = ops_count(x);
            res = vector(AS_LIST(x)[0]->type, n);
            f64ptr = AS_F64(res);

            for (i = 0; i < l; i++) {
                n = AS_LIST(x)[i]->len;
                memcpy(f64ptr, AS_F64(AS_LIST(x)[i]), n * sizeof(f64_t));
                f64ptr += n;
            }

            return res;
        case TYPE_PARTEDGUID:
            l = x->len;
            n = ops_count(x);
            res = vector(AS_LIST(x)[0]->type, n);
            guidptr = AS_GUID(res);

            for (i = 0; i < l; i++) {
                n = AS_LIST(x)[i]->len;
                memcpy(guidptr, AS_GUID(AS_LIST(x)[i]), n * sizeof(guid_t));
                guidptr += n;
            }

            return res;
        case TYPE_PARTEDENUM:
            l = x->len;
            n = ops_count(x);
            res = SYMBOL(n);
            i64ptr = AS_I64(res);

            // TODO: optimise enums
            for (i = 0; i < l; i++) {
                v = ray_value(AS_LIST(x)[i]);
                n = v->len;
                memcpy(i64ptr, AS_I64(v), n * sizeof(i64_t));
                drop_obj(v);
                i64ptr += n;
            }

            return res;

            // case TYPE_MAPCOMMON:
            //     l = AS_LIST(x)[0]->len;
            //     if (l == 0)
            //         return NULL_OBJ;

            //     n = ops_count(x);
            //     res = vector(AS_LIST(x)[0]->type, n);
            //     i64ptr = AS_I64(res);

            //     for (i = 0; i < l; i++) {
            //         n = AS_I64(AS_LIST(x)[1])[i];

            //         for (j = 0; j < n; j++)
            //             i64ptr[j] = AS_I64(AS_LIST(x)[0])[i];

            //         i64ptr += n;
            //     }

            //     return res;

        default:
            return clone_obj(x);
    }
}

obj_p ray_where(obj_p x) {
    i64_t i, l;
    obj_p res;

    switch (x->type) {
        case TYPE_B8:
            return ops_where(AS_B8(x), x->len);
        case TYPE_PARTEDB8:
            l = x->len;
            res = LIST(l);
            res->type = TYPE_PARTEDI64;

            for (i = 0; i < l; i++) {
                obj_p elem = AS_LIST(x)[i];
                if (elem == NULL_OBJ) {
                    // Partition doesn't match
                    AS_LIST(res)[i] = NULL_OBJ;
                } else if (elem->type < 0) {
                    // Atom (e.g., b8(B8_TRUE)) - entire partition matches
                    // Use i64(-1) as a marker meaning "take all rows from this partition"
                    AS_LIST(res)[i] = i64(-1);
                } else {
                    // Vector - compute actual indices where value is true
                    AS_LIST(res)[i] = ops_where(AS_B8(elem), elem->len);
                }
            }

            return res;
        default:
            THROW(ERR_TYPE, "where: unsupported type: '%s", type_name(x->type));
    }
}

static obj_p ray_bin_partial(raw_p a, raw_p b, raw_p c, raw_p d, raw_p e) {
    i32_t *i32x, *i32y;
    i64_t *i64x, *i64y, *out;
    i64_t i, left, right, mid, idx;
    obj_p x = (obj_p)a;
    obj_p y = (obj_p)b;
    i64_t len = (i64_t)c;
    i64_t offset = (i64_t)d;
    out = AS_I64((obj_p)e) + offset;

    switch (x->type) {
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            i32x = AS_I32(x);
            i32y = AS_I32(y) + offset;
            for (i = 0; i < len; i++) {
                left = 0, right = x->len - 1, idx = -1;
                while (left <= right) {
                    mid = left + (right - left) / 2;
                    if (i32x[mid] <= i32y[i]) {
                        idx = mid;
                        left = mid + 1;
                    } else {
                        right = mid - 1;
                    }
                }
                out[i] = idx;
            }
            return NULL_OBJ;
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            i64x = AS_I64(x);
            i64y = AS_I64(y) + offset;
            for (i = 0; i < len; i++) {
                left = 0, right = x->len - 1, idx = -1;
                while (left <= right) {
                    mid = left + (right - left) / 2;
                    if (i64x[mid] <= i64y[i]) {
                        idx = mid;
                        left = mid + 1;
                    } else {
                        right = mid - 1;
                    }
                }
                out[i] = idx;
            }
            return NULL_OBJ;
        default:
            THROW(ERR_TYPE, "ray_bin_partial: unsupported type: '%s", type_name(x->type));
    }
}

static obj_p ray_binr_partial(raw_p a, raw_p b, raw_p c, raw_p d, raw_p e) {
    i32_t *i32x, *i32y;
    i64_t *i64x, *i64y, *out;
    i64_t i, left, right, mid, idx;
    obj_p x = (obj_p)a;
    obj_p y = (obj_p)b;
    i64_t len = (i64_t)c;
    i64_t offset = (i64_t)d;
    out = AS_I64((obj_p)e) + offset;

    switch (x->type) {
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            i32x = AS_I32(x);
            i32y = AS_I32(y) + offset;
            for (i = 0; i < len; i++) {
                left = 0, right = x->len - 1, idx = x->len;
                while (left <= right) {
                    mid = left + (right - left) / 2;
                    if (i32x[mid] >= i32y[i]) {
                        idx = mid;
                        right = mid - 1;
                    } else {
                        left = mid + 1;
                    }
                }
                out[i] = idx;
            }
            return NULL_OBJ;
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            i64x = AS_I64(x);
            i64y = AS_I64(y) + offset;
            for (i = 0; i < len; i++) {
                left = 0, right = x->len - 1, idx = x->len;
                while (left <= right) {
                    mid = left + (right - left) / 2;
                    if (i64x[mid] >= i64y[i]) {
                        idx = mid;
                        right = mid - 1;
                    } else {
                        left = mid + 1;
                    }
                }
                out[i] = idx;
            }
            return NULL_OBJ;
        default:
            THROW(ERR_TYPE, "ray_binr_partial: unsupported type: '%s", type_name(x->type));
    }
}

static obj_p bin_map(raw_p op, obj_p x, obj_p y) {
    i64_t i, l, n, chunk;
    obj_p v, out;
    pool_p pool;
    raw_p argv[5];

    l = y->len;
    pool = runtime_get()->pool;
    n = pool_split_by(pool, l, 0);
    out = I64(l);

    if (n == 1) {
        argv[0] = (raw_p)x;
        argv[1] = (raw_p)y;
        argv[2] = (raw_p)l;
        argv[3] = (raw_p)0;
        argv[4] = (raw_p)out;
        v = pool_call_task_fn(op, 5, argv);
        if (IS_ERR(v)) {
            out->len = 0;
            drop_obj(out);
            return v;
        }
        return out;
    }

    pool_prepare(pool);
    chunk = l / n;

    for (i = 0; i < n - 1; i++)
        pool_add_task(pool, op, 5, x, y, chunk, i * chunk, out);

    pool_add_task(pool, op, 5, x, y, l - i * chunk, i * chunk, out);

    v = pool_run(pool);
    if (IS_ERR(v))
        return v;

    drop_obj(v);

    return out;
}

obj_p ray_bin(obj_p x, obj_p y) {
    i64_t left, right, mid, idx;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_I32, -TYPE_I32):
        case MTYPE2(TYPE_DATE, -TYPE_DATE):
        case MTYPE2(TYPE_TIME, -TYPE_TIME):
            left = 0;
            right = x->len - 1;
            idx = -1;
            while (left <= right) {
                mid = left + (right - left) / 2;
                if (AS_I32(x)[mid] <= y->i32) {
                    idx = mid;
                    left = mid + 1;
                } else {
                    right = mid - 1;
                }
            }
            return i32(idx);
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
            left = 0;
            right = x->len - 1;
            idx = -1;
            while (left <= right) {
                mid = left + (right - left) / 2;
                if (AS_I64(x)[mid] <= y->i64) {
                    idx = mid;
                    left = mid + 1;
                } else {
                    right = mid - 1;
                }
            }
            return i64(idx);
        case MTYPE2(TYPE_I32, TYPE_I32):
        case MTYPE2(TYPE_DATE, TYPE_DATE):
        case MTYPE2(TYPE_TIME, TYPE_TIME):
            return bin_map(ray_bin_partial, x, y);
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            return bin_map(ray_bin_partial, x, y);
        default:
            THROW(ERR_TYPE, "bin: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_binr(obj_p x, obj_p y) {
    i64_t left, right, mid, idx;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_I32, -TYPE_I32):
        case MTYPE2(TYPE_DATE, -TYPE_DATE):
        case MTYPE2(TYPE_TIME, -TYPE_TIME):
            left = 0;
            right = x->len - 1;
            idx = x->len;
            while (left <= right) {
                mid = left + (right - left) / 2;
                if (AS_I32(x)[mid] >= y->i32) {
                    idx = mid;
                    right = mid - 1;
                } else {
                    left = mid + 1;
                }
            }
            return i64(idx);
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
            left = 0;
            right = x->len - 1;
            idx = x->len;
            while (left <= right) {
                mid = left + (right - left) / 2;
                if (AS_I64(x)[mid] >= y->i64) {
                    idx = mid;
                    right = mid - 1;
                } else {
                    left = mid + 1;
                }
            }
            return i64(idx);
        case MTYPE2(TYPE_I32, TYPE_I32):
        case MTYPE2(TYPE_DATE, TYPE_DATE):
        case MTYPE2(TYPE_TIME, TYPE_TIME):
            return bin_map(ray_binr_partial, x, y);
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            return bin_map(ray_binr_partial, x, y);
        default:
            THROW(ERR_TYPE, "binr: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}