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
#include "heap.h"
#include "ops.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "serde.h"
#include "runtime.h"
#include "compose.h"
#include "order.h"
#include "misc.h"
#include "error.h"
#include "aggr.h"
#include "index.h"
#include "string.h"
#include "pool.h"
#include "cmp.h"

obj_p ray_at(obj_p x, obj_p y) {
    u64_t i, j, yl, xl, n, size;
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

            if (!s || IS_ERROR(s) || s->type != TYPE_SYMBOL) {
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

            if (IS_ERROR(s))
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

            return load_obj(&buf, &xl);

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
                AS_LIST(res)[i] = load_obj(&buf, &size);
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
    u64_t i, j = 0, l;
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
    u64_t i, l, m, n, size;
    obj_p k, s, v, res;
    u8_t *buf;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I64, TYPE_B8):
            l = y->len;
            m = ABSI64(x->i64);
            res = B8(m);

            if (x->i64 >= 0) {
                for (i = 0; i < m; i++)
                    AS_B8(res)
                [i] = AS_B8(y)[i % l];
            } else {
                for (i = 0; i < m; i++)
                    AS_B8(res)
                [i] = AS_B8(y)[l - 1 - (i % l)];
            }

            return res;
        case MTYPE2(-TYPE_I64, TYPE_I64):
        case MTYPE2(-TYPE_I64, TYPE_SYMBOL):
        case MTYPE2(-TYPE_I64, TYPE_TIMESTAMP):
            l = y->len;
            m = ABSI64(x->i64);
            res = vector(y->type, m);

            if (x->i64 >= 0) {
                for (i = 0; i < m; i++)
                    AS_I64(res)
                [i] = AS_I64(y)[i % l];
            } else {
                for (i = 0; i < m; i++)
                    AS_I64(res)
                [i] = AS_I64(y)[l - 1 - (i % l)];
            }

            return res;

        case MTYPE2(-TYPE_I64, TYPE_F64):
            l = y->len;
            m = ABSI64(x->i64);
            res = F64(m);

            if (x->i64 >= 0) {
                for (i = 0; i < m; i++)
                    AS_F64(res)
                [i] = AS_F64(y)[i % l];
            } else {
                for (i = 0; i < m; i++)
                    AS_F64(res)
                [i] = AS_F64(y)[l - 1 - (i % l)];
            }

            return res;

        case MTYPE2(-TYPE_I64, -TYPE_I64):
        case MTYPE2(-TYPE_I64, -TYPE_SYMBOL):
        case MTYPE2(-TYPE_I64, -TYPE_TIMESTAMP):
            l = ABSI64(x->i64);
            res = I64(l);

            for (i = 0; i < l; i++)
                AS_I64(res)
            [i] = y->i64;

            return res;

        case MTYPE2(-TYPE_I64, -TYPE_F64):
            l = ABSI64(x->i64);
            res = F64(l);
            for (i = 0; i < l; i++)
                AS_F64(res)
            [i] = y->f64;

            return res;

        case MTYPE2(-TYPE_I64, -TYPE_GUID):
            l = ABSI64(x->i64);
            res = GUID(l);
            for (i = 0; i < l; i++)
                memcpy(AS_GUID(res)[i], AS_GUID(y)[0], sizeof(guid_t));

            return res;

        case MTYPE2(-TYPE_I64, TYPE_ENUM):
            k = ray_key(y);
            s = ray_get(k);
            drop_obj(k);

            v = ENUM_VAL(y);
            l = ABSI64(x->i64);
            m = v->len;

            if (s->type != TYPE_SYMBOL) {
                res = I64(l);

                if (x->i64 >= 0) {
                    for (i = 0; i < l; i++)
                        AS_I64(res)[i] = AS_I64(v)[i % m];
                } else {
                    for (i = 0; i < l; i++)
                        AS_I64(res)[i] = AS_I64(v)[m - 1 - (i % m)];
                }

                drop_obj(s);

                return res;
            }

            res = SYMBOL(l);

            if (x->i64 >= 0) {
                for (i = 0; i < l; i++) {
                    if (AS_I64(v)[i % m] >= (i64_t)s->len) {
                        drop_obj(s);
                        drop_obj(res);
                        THROW(ERR_INDEX, "take: enum can not be resolved: index out of range");
                    }

                    AS_SYMBOL(res)
                    [i] = AS_SYMBOL(s)[AS_I64(v)[i % m]];
                }
            } else {
                for (i = 0; i < l; i++) {
                    if (AS_I64(v)[m - 1 - (i % m)] >= (i64_t)s->len) {
                        drop_obj(s);
                        drop_obj(res);
                        THROW(ERR_INDEX, "take: enum can not be resolved: index out of range");
                    }

                    AS_SYMBOL(res)
                    [i] = AS_SYMBOL(s)[AS_I64(v)[m - 1 - (i % m)]];
                }
            }

            drop_obj(s);

            return res;

        case MTYPE2(-TYPE_I64, TYPE_MAPLIST):
            l = ABSI64(x->i64);
            res = vector(TYPE_LIST, l);

            size = l;

            k = MAPLIST_KEY(y);
            s = MAPLIST_VAL(y);

            m = k->len;
            n = s->len;

            if (x->i64 >= 0) {
                for (i = 0; i < l; i++) {
                    if (AS_I64(s)[i % n] >= (i64_t)m) {
                        buf = AS_U8(k) + AS_I64(s)[i % n];
                        v = load_obj(&buf, &size);

                        if (IS_ERROR(v)) {
                            res->len = i;
                            drop_obj(res);
                            return v;
                        }

                        AS_LIST(res)
                        [i] = v;
                    } else {
                        res->len = i;
                        drop_obj(res);
                        THROW(ERR_INDEX, "anymap value: index out of range: %d", AS_I64(s)[i % n]);
                    }
                }
            } else {
                for (i = 0; i < l; i++) {
                    if (AS_I64(s)[n - 1 - (i % n)] >= (i64_t)m) {
                        buf = AS_U8(k) + AS_I64(s)[n - 1 - (i % n)];
                        v = load_obj(&buf, &size);

                        if (IS_ERROR(v)) {
                            res->len = i;
                            drop_obj(res);
                            return v;
                        }

                        AS_LIST(res)
                        [i] = v;
                    } else {
                        res->len = i;
                        drop_obj(res);
                        THROW(ERR_INDEX, "anymap value: index out of range: %d", AS_I64(s)[n - 1 - (i % n)]);
                    }
                }
            }

            return res;

        case MTYPE2(-TYPE_I64, TYPE_C8):
            m = ABSI64(x->i64);
            n = y->len;
            res = C8(m);

            if (x->i64 >= 0) {
                for (i = 0; i < m; i++)
                    AS_C8(res)
                [i] = AS_C8(y)[i % n];
            } else {
                for (i = 0; i < m; i++)
                    AS_C8(res)
                [i] = AS_C8(y)[n - 1 - (i % n)];
            }

            return res;

        case MTYPE2(-TYPE_I64, TYPE_LIST):
            m = ABSI64(x->i64);
            n = y->len;
            res = vector(TYPE_LIST, m);

            if (x->i64 >= 0) {
                for (i = 0; i < m; i++)
                    AS_LIST(res)
                [i] = clone_obj(AS_LIST(y)[i % n]);
            } else {
                for (i = 0; i < m; i++)
                    AS_LIST(res)
                [i] = clone_obj(AS_LIST(y)[n - 1 - (i % n)]);
            }

            return res;

        case MTYPE2(-TYPE_I64, TYPE_GUID):
            m = ABSI64(x->i64);
            n = y->len;
            res = GUID(m);

            if (x->i64 >= 0) {
                for (i = 0; i < m; i++)
                    memcpy(AS_GUID(res)[i], AS_GUID(y)[i % n], sizeof(guid_t));
            } else {
                for (i = 0; i < m; i++)
                    memcpy(AS_GUID(res)[i], AS_GUID(y)[n - 1 - (i % n)], sizeof(guid_t));
            }

            return res;

        case MTYPE2(-TYPE_I64, TYPE_TABLE):
            l = AS_LIST(y)[1]->len;
            res = vector(TYPE_LIST, l);
            for (i = 0; i < l; i++) {
                v = ray_take(x, AS_LIST(AS_LIST(y)[1])[i]);

                if (IS_ERROR(v)) {
                    res->len = i;
                    drop_obj(v);
                    drop_obj(res);
                    return v;
                }

                AS_LIST(res)
                [i] = v;
            }

            return table(clone_obj(AS_LIST(y)[0]), res);

        default:
            THROW(ERR_TYPE, "take: unsupported types: '%s, %s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_in(obj_p x, obj_p y) {
    i64_t i, xl, yl, p, k;
    obj_p v, vec, set;
    i16_t min16;
    u64_t range;
    b8_t nl = 0;

    if ((IS_VECTOR(y) || y->type == TYPE_LIST) && y->len == 0) {
        if (IS_VECTOR(x) || x->type == TYPE_LIST) {
            vec = B8(x->len);
            memset(AS_B8(vec), 0, x->len);
            return vec;
        } else
            return b8(0);
    }

    if (!IS_VECTOR(x))
        return b8(find_obj_idx(y, x) != NULL_I64);

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_U8, TYPE_U8):
        case MTYPE2(TYPE_B8, TYPE_B8):
        case MTYPE2(TYPE_C8, TYPE_C8):
            range = 256;
            set = B8(range);
            memset(AS_U8(set), 0, range);
            for (i = 0; i < (i64_t)y->len; i++)
                AS_B8(set)[AS_U8(y)[i]] = 1;
            vec = B8(x->len);
            for (i = 0; i < (i64_t)x->len; i++)
                AS_B8(vec)[i] = AS_B8(set)[AS_U8(x)[i]];
            drop_obj(set);
            return vec;

        case MTYPE2(TYPE_I16, TYPE_I16):
        case MTYPE2(TYPE_I32, TYPE_I16):
        case MTYPE2(TYPE_I64, TYPE_I16):
            min16 = -32768;
            range = 65536;
            set = B8(range);
            memset(AS_B8(set), 0, range);
            for (i = 0; i < (i64_t)y->len; i++)
                AS_B8(set)[AS_I16(y)[i] - min16] = 1;
            vec = B8(x->len);
            switch (x->type) {
                case TYPE_I32:
                    for (i = 0; i < (i64_t)x->len; i++)
                        AS_B8(vec)
                    [i] = (AS_I32(x)[i] == NULL_I32 || (AS_I32(x)[i] > INT16_MIN && AS_I32(x)[i] <= INT16_MAX))
                              ? AS_B8(set)[i32_to_i16(AS_I32(x)[i]) - min16]
                              : 0;
                    break;
                case TYPE_I64:
                    for (i = 0; i < (i64_t)x->len; i++) {
                        p = AS_I64(x)[i];
                        AS_B8(vec)
                        [i] = ((p > INT16_MIN && p <= INT16_MAX) || p == NULL_I64)
                                  ? AS_B8(set)[i64_to_i16(AS_I64(x)[i]) - min16]
                                  : 0;
                    }
                    break;
                default:
                    for (i = 0; i < (i64_t)x->len; i++)
                        AS_B8(vec)[i] = AS_B8(set)[AS_I16(x)[i] - min16];
            }
            drop_obj(set);
            return vec;

        case MTYPE2(TYPE_I16, TYPE_I32):
        case MTYPE2(TYPE_I32, TYPE_I32):
        case MTYPE2(TYPE_I64, TYPE_I32):
        case MTYPE2(TYPE_DATE, TYPE_DATE):
        case MTYPE2(TYPE_TIME, TYPE_TIME):
            xl = x->len;
            yl = y->len;
            set = ht_oa_create(yl, -1);
            for (i = 0; i < yl; i++) {
                k = (i64_t)(AS_I32(y)[i]);
                p = ht_oa_tab_next(&set, k);
                if (AS_I64(AS_LIST(set)[0])[p] == NULL_I64)
                    AS_I64(AS_LIST(set)[0])[p] = k;
            }
            vec = B8(xl);
            switch (x->type) {
                case TYPE_I16:
                    for (i = 0; i < xl; i++) {
                        AS_B8(vec)[i] = (ht_oa_tab_get(set, (i64_t)(i16_to_i32(AS_I16(x)[i]))) != NULL_I64);
                    }
                    break;
                case TYPE_I64:
                    for (i = 0; i < xl; i++) {
                        p = AS_I64(x)[i];
                        AS_B8(vec)
                        [i] = ((p > INT32_MIN && p <= INT32_MAX) || p == NULL_I64)
                                  ? (ht_oa_tab_get(set, (i64_t)(i64_to_i32(p))) != NULL_I64)
                                  : 0;
                    }
                    break;
                default:
                    for (i = 0; i < xl; i++) {
                        AS_B8(vec)[i] = (ht_oa_tab_get(set, (i64_t)(AS_I32(x)[i])) != NULL_I64);
                    }
            }
            drop_obj(set);
            return vec;

        case MTYPE2(TYPE_I16, TYPE_I64):
        case MTYPE2(TYPE_I32, TYPE_I64):
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            xl = x->len;
            yl = y->len;
            set = ht_oa_create(yl, -1);
            for (i = 0; i < yl; i++) {
                if (AS_I64(y)[i] == NULL_I64)
                    nl = 1;
                else {
                    p = ht_oa_tab_next(&set, AS_I64(y)[i]);
                    if (AS_I64(AS_LIST(set)[0])[p] == NULL_I64)
                        AS_I64(AS_LIST(set)[0])[p] = AS_I64(y)[i];
                }
            }
            vec = B8(xl);
            switch (x->type) {
                case TYPE_I16:
                    for (i = 0; i < xl; i++) {
                        p = i16_to_i64(AS_I16(x)[i]);
                        AS_B8(vec)[i] = (p == NULL_I64) ? nl : (ht_oa_tab_get(set, p) != NULL_I64);
                    }
                    break;
                case TYPE_I32:
                    for (i = 0; i < xl; i++) {
                        p = i32_to_i64(AS_I32(x)[i]);
                        AS_B8(vec)[i] = (p == NULL_I64) ? nl : (ht_oa_tab_get(set, p) != NULL_I64);
                    }
                    break;
                default:
                    for (i = 0; i < xl; i++) {
                        p = AS_I64(x)[i];
                        AS_B8(vec)[i] = (p == NULL_I64) ? nl : (ht_oa_tab_get(set, p) != NULL_I64);
                    }
            }
            drop_obj(set);
            return vec;

        case MTYPE2(TYPE_U8, TYPE_LIST):
            xl = x->len;
            vec = B8(xl);
            for (i = 0; i < xl; i++)
                AS_B8(vec)[i] = ray_in(u8(AS_U8(x)[i]), y)->b8;
            return vec;
        case MTYPE2(TYPE_B8, TYPE_LIST):
            xl = x->len;
            vec = B8(xl);
            for (i = 0; i < xl; i++)
                AS_B8(vec)[i] = ray_in(b8(AS_B8(x)[i]), y)->b8;
            return vec;
        case MTYPE2(TYPE_C8, TYPE_LIST):
            xl = x->len;
            vec = B8(xl);
            for (i = 0; i < xl; i++)
                AS_B8(vec)[i] = ray_in(c8(AS_C8(x)[i]), y)->b8;
            return vec;
        case MTYPE2(TYPE_I16, TYPE_LIST):
            xl = x->len;
            vec = B8(xl);
            for (i = 0; i < xl; i++)
                AS_B8(vec)[i] = ray_in(i16(AS_I16(x)[i]), y)->b8;
            return vec;
        case MTYPE2(TYPE_I32, TYPE_LIST):
        case MTYPE2(TYPE_DATE, TYPE_LIST):
        case MTYPE2(TYPE_TIME, TYPE_LIST):
            xl = x->len;
            vec = B8(xl);
            for (i = 0; i < xl; i++)
                AS_B8(vec)[i] = ray_in(i32(AS_I32(x)[i]), y)->b8;
            return vec;
        case MTYPE2(TYPE_I64, TYPE_LIST):
        case MTYPE2(TYPE_SYMBOL, TYPE_LIST):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_LIST):
            xl = x->len;
            vec = B8(xl);
            for (i = 0; i < xl; i++)
                AS_B8(vec)[i] = ray_in(i64(AS_I64(x)[i]), y)->b8;
            return vec;
        case MTYPE2(TYPE_F64, TYPE_LIST):
            xl = x->len;
            vec = B8(xl);
            for (i = 0; i < xl; i++)
                AS_B8(vec)[i] = ray_in(f64(AS_F64(x)[i]), y)->b8;
            return vec;

        default:
            if (x->type == TYPE_LIST) {
                xl = x->len;
                vec = LIST(xl);

                for (i = 0; i < xl; i++) {
                    v = ray_in(AS_LIST(x)[i], y);
                    if (IS_ERROR(v)) {
                        vec->len = i;
                        drop_obj(vec);
                        return v;
                    }

                    AS_LIST(vec)[i] = v;
                }

                return vec;
            }

            if (y->type == TYPE_LIST) {
                yl = y->len;
                vec = B8(yl);

                for (i = 0; i < yl; i++) {
                    v = ray_eq(x, AS_LIST(y)[i]);
                    if (IS_ERROR(v)) {
                        drop_obj(vec);
                        return v;
                    }

                    // if (v->type != -TYPE_B8) {
                    //     drop_obj(v);
                    //     drop_obj(vec);
                    //     THROW(ERR_TYPE, "in: returned vector instead of atom");
                    // }

                    AS_B8(vec)[i] = v->b8;
                    drop_obj(v);
                }

                return vec;
            }

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
            if (IS_ERROR(mask))
                return mask;

            nmask = ray_not(mask);
            drop_obj(mask);

            if (IS_ERROR(nmask))
                return nmask;

            res = ray_filter(x, nmask);
            drop_obj(nmask);
            return res;
        case MTYPE2(TYPE_TABLE, -TYPE_SYMBOL):
            mask = ray_ne(AS_LIST(x)[0], y);
            if (IS_ERROR(mask))
                return mask;

            ids = ray_where(mask);
            drop_obj(mask);

            if (IS_ERROR(ids))
                return ids;

            k = ray_at(AS_LIST(x)[0], ids);
            v = ray_at(AS_LIST(x)[1], ids);
            drop_obj(ids);

            return table(k, v);
        case MTYPE2(TYPE_TABLE, TYPE_SYMBOL):
            mask = ray_in(AS_LIST(x)[0], y);
            if (IS_ERROR(mask))
                return mask;

            nmask = ray_not(mask);
            drop_obj(mask);

            if (IS_ERROR(nmask))
                return nmask;

            ids = ray_where(nmask);
            drop_obj(nmask);

            if (IS_ERROR(ids))
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

                        if (IS_ERROR(mask)) {
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
    u64_t l;

    switch (x->type) {
        case TYPE_MAPGROUP:
            return aggr_last(AS_LIST(x)[0], AS_LIST(x)[1]);
        default:
            l = ops_count(x);
            return at_idx(x, l ? l - 1 : l);
    }
}

obj_p ray_key(obj_p x) {
    u64_t l;
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
    u64_t size;
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
                    v = load_obj(&buf, &size);

                    if (IS_ERROR(v)) {
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
            l = AS_LIST(x)[1]->len;
            v = vector(TYPE_LIST, l);
            for (i = 0; i < l; i++) {
                res = ray_value(AS_LIST(AS_LIST(x)[1])[i]);
                if (IS_ERROR(res)) {
                    drop_obj(v);
                    return res;
                }

                AS_LIST(v)[i] = res;
            }
            return table(clone_obj(AS_LIST(x)[0]), v);

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
                    objptr[j] = load_obj(&buf, &size);
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
    u64_t i, l;
    obj_p res;

    switch (x->type) {
        case TYPE_B8:
            return ops_where(AS_B8(x), x->len);
        case TYPE_PARTEDB8:
            l = x->len;
            res = LIST(l);
            res->type = TYPE_PARTEDI64;

            for (i = 0; i < l; i++)
                AS_LIST(res)[i] = (AS_LIST(x)[i] == NULL_OBJ) ? NULL_OBJ : i64(1);

            return res;
        default:
            THROW(ERR_TYPE, "where: unsupported type: '%s", type_name(x->type));
    }
}