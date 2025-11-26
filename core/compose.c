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

#include "compose.h"
#include "rayforce.h"
#include "util.h"
#include "heap.h"
#include "ops.h"
#include "unary.h"
#include "runtime.h"
#include "items.h"
#include "guid.h"
#include "index.h"
#include "error.h"
#include "string.h"
#include "guid.h"
#include "aggr.h"

obj_p ray_cast_obj(obj_p x, obj_p y) {
    i8_t type;
    obj_p err;
    obj_p fmt, msg;

    if (x->type != -TYPE_SYMBOL)
        THROW(ERR_TYPE, "as: first argument must be a symbol");

    type = env_get_type_by_type_name(&runtime_get()->env, x->i64);

    if (type == TYPE_ERR) {
        fmt = obj_fmt(x, B8_TRUE);
        msg = str_fmt(-1, "as: not a type: '%s", fmt);
        err = error_obj(ERR_TYPE, msg);
        heap_free(fmt);
        return err;
    }

    return cast_obj(type, y);
}

obj_p ray_til_partial(i64_t len, i64_t offset, i64_t filter[], i64_t out[]) {
    i64_t i;

    if (filter) {
        for (i = 0; i < len; i++)
            out[i] = filter[i + offset];
    } else {
        for (i = 0; i < len; i++)
            out[i] = i + offset;
    }

    return NULL_OBJ;
}

obj_p __til(obj_p x, obj_p filter) {
    i64_t i, l, n, chunk;
    i64_t *ids = NULL;
    obj_p v, vec;
    pool_p pool;

    l = (i64_t)x->i64;

    if (filter != NULL_OBJ) {
        ids = AS_I64(filter);
        l = filter->len;
    }

    vec = I64(l);

    if (IS_ERR(vec))
        return vec;

    vec->attrs = ATTR_ASC | ATTR_DISTINCT;

    pool = pool_get();
    n = pool_split_by(pool, l, 0);

    if (n == 1) {
        ray_til_partial(l, 0, ids, AS_I64(vec));
        return vec;
    }

    chunk = l / n;
    pool_prepare(pool);

    for (i = 0; i < n - 1; i++)
        pool_add_task(pool, (raw_p)ray_til_partial, 4, chunk, i * chunk, ids, AS_I64(vec) + i * chunk);

    pool_add_task(pool, (raw_p)ray_til_partial, 4, l - i * chunk, i * chunk, ids, AS_I64(vec) + i * chunk);

    v = pool_run(pool);
    drop_obj(v);

    return vec;
}

obj_p ray_til(obj_p x) {
    if (x->type != -TYPE_I64)
        return error_str(ERR_TYPE, "til: expected i64");

    return __til(x, NULL_OBJ);
}

obj_p ray_reverse(obj_p x) {
    i64_t i, l;
    obj_p res;

    switch (x->type) {
        case TYPE_C8:
        case TYPE_U8:
        case TYPE_B8:
            l = x->len;
            res = C8(l);

            for (i = l - 1; i >= 0; i--)
                AS_C8(res)[l - i] = AS_C8(x)[i];
            break;

        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            l = x->len;
            res = vector(x->type, l);

            for (i = 0; i < l; i++)
                AS_I32(res)[i] = AS_I32(x)[l - i - 1];
            break;

        case TYPE_I64:
        case TYPE_TIMESTAMP:
        case TYPE_SYMBOL:
            l = x->len;
            res = vector(x->type, l);

            for (i = 0; i < l; i++)
                AS_I64(res)[i] = AS_I64(x)[l - i - 1];
            break;

        case TYPE_F64:
            l = x->len;
            res = F64(l);

            for (i = 0; i < l; i++)
                AS_F64(res)[i] = AS_F64(x)[l - i - 1];
            break;

        case TYPE_LIST:
            l = x->len;
            res = vector(TYPE_LIST, l);

            for (i = 0; i < l; i++)
                AS_LIST(res)[i] = clone_obj(AS_LIST(x)[l - i - 1]);
            break;

        default:
            THROW(ERR_TYPE, "reverse: unsupported type: '%s", type_name(x->type));
    }

    res->attrs = (x->attrs & ~(ATTR_ASC | ATTR_DESC)) |
                 ((x->attrs & ATTR_ASC) ? ATTR_DESC : 0) |
                 ((x->attrs & ATTR_DESC) ? ATTR_ASC : 0);

    return res;
}

obj_p ray_dict(obj_p x, obj_p y) {
    if (!IS_VECTOR(x) || !IS_VECTOR(y))
        return error_str(ERR_TYPE, "Keys and Values must be lists");

    if (ops_count(x) != ops_count(y))
        return error_str(ERR_LENGTH, "Keys and Values must have the same length");

    return dict(clone_obj(x), clone_obj(y));
}

obj_p ray_table(obj_p x, obj_p y) {
    b8_t synergy = B8_TRUE;
    i64_t i, j, len, cl = 0;
    obj_p lst, c, l = NULL_OBJ;

    if (x->type != TYPE_SYMBOL)
        return error_str(ERR_TYPE, "table: keys must be a symbol vector");

    if (y->type != TYPE_LIST) {
        if (x->len != 1)
            return error_str(ERR_LENGTH, "table: keys and values must have the same length");

        l = LIST(1);
        AS_LIST(l)[0] = clone_obj(y);
        y = l;
    }

    if (x->len != y->len && y->len > 0) {
        drop_obj(l);
        return error_str(ERR_LENGTH, "table: keys and values must have the same length");
    }

    len = y->len;

    for (i = 0; i < len; i++) {
        switch (AS_LIST(y)[i]->type) {
            case -TYPE_B8:
            case -TYPE_U8:
            case -TYPE_C8:
            case -TYPE_I16:
            case -TYPE_I32:
            case -TYPE_TIME:
            case -TYPE_DATE:
            case -TYPE_I64:
            case -TYPE_SYMBOL:
            case -TYPE_TIMESTAMP:
            case -TYPE_F64:
            case -TYPE_GUID:
            case TYPE_LAMBDA:
            case TYPE_DICT:
            case TYPE_TABLE:
                synergy = B8_FALSE;
                break;
            case TYPE_B8:
            case TYPE_U8:
            case TYPE_C8:
            case TYPE_I16:
            case TYPE_I32:
            case TYPE_DATE:
            case TYPE_TIME:
            case TYPE_I64:
            case TYPE_F64:
            case TYPE_TIMESTAMP:
            case TYPE_SYMBOL:
            case TYPE_LIST:
            case TYPE_GUID:
                j = AS_LIST(y)[i]->len;
                if (cl != 0 && j != cl)
                    return error(ERR_LENGTH, "table: values must be of the same length");

                cl = j;
                break;
            case TYPE_ENUM:
                synergy = B8_FALSE;
                j = AS_LIST(AS_LIST(y)[i])[1]->len;
                if (cl != 0 && j != cl)
                    return error(ERR_LENGTH, "table: values must be of the same length");

                cl = j;
                break;
            case TYPE_MAPCOMMON:
                j = AS_LIST(AS_LIST(y)[i])[0]->len;
                if (cl != 0 && j != cl)
                    return error(ERR_LENGTH, "table: values must be of the same length");
                break;
            default:
                return error(ERR_TYPE, "table: unsupported type: '%s' in a values list",
                             type_name(AS_LIST(y)[i]->type));
        }
    }

    // there are no atoms, no lazytypes and all columns are of the same length
    if (synergy) {
        drop_obj(l);
        return table(clone_obj(x), clone_obj(y));
    }

    // otherwise we need to expand atoms to vectors
    lst = vector(TYPE_LIST, len);

    if (cl == 0)
        cl = 1;

    for (i = 0; i < len; i++) {
        switch (AS_LIST(y)[i]->type) {
            case -TYPE_B8:
            case -TYPE_U8:
            case -TYPE_C8:
            case -TYPE_I16:
            case -TYPE_I32:
            case -TYPE_DATE:
            case -TYPE_TIME:
            case -TYPE_I64:
            case -TYPE_SYMBOL:
            case -TYPE_TIMESTAMP:
            case -TYPE_F64:
            case -TYPE_GUID:
                c = i64(cl);
                AS_LIST(lst)[i] = ray_take(c, AS_LIST(y)[i]);
                drop_obj(c);
                break;
            case TYPE_ENUM:
                AS_LIST(lst)[i] = ray_value(AS_LIST(y)[i]);
                break;
            case TYPE_MAPCOMMON:
                AS_LIST(lst)[i] = ray_value(AS_LIST(y)[i]);
                break;
            default:
                AS_LIST(lst)[i] = clone_obj(AS_LIST(y)[i]);
        }
    }

    drop_obj(l);

    return table(clone_obj(x), lst);
}

obj_p ray_guid(obj_p x) {
    i64_t i, count;
    obj_p vec;
    guid_t *g;

    switch (x->type) {
        case -TYPE_I64:
            count = x->i64;
            vec = GUID(count);
            g = AS_GUID(vec);

            for (i = 0; i < count; i++)
                guid_generate(g + i);

            return vec;

        default:
            THROW(ERR_TYPE, "guid: unsupported type: '%s", type_name(x->type));
    }
}

obj_p ray_list(obj_p *x, i64_t n) {
    i64_t i;
    obj_p lst = LIST(n);

    for (i = 0; i < n; i++)
        AS_LIST(lst)[i] = clone_obj(x[i]);

    return lst;
}

obj_p ray_enlist(obj_p *x, i64_t n) {
    i64_t i;
    obj_p lst;

    if (n == 0)
        return LIST(0);

    lst = vector(x[0]->type, n);

    for (i = 0; i < n; i++)
        ins_obj(&lst, i, clone_obj(x[i]));

    return lst;
}

obj_p ray_enum(obj_p x, obj_p y) {
    obj_p s, v;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_SYMBOL, TYPE_SYMBOL):
            s = ray_get(x);

            if (IS_ERR(s))
                return s;

            if (!s || s->type != TYPE_SYMBOL) {
                drop_obj(s);
                THROW(ERR_TYPE, "enum: expected vector symbol");
            }

            v = index_find_i64(AS_I64(s), s->len, AS_I64(y), y->len);
            drop_obj(s);

            if (IS_ERR(v)) {
                drop_obj(v);
                THROW(ERR_TYPE, "enum: can not be fully indexed");
            }

            return enumerate(clone_obj(x), v);
        default:
            THROW(ERR_TYPE, "enum: unsupported types: '%s '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_rand(obj_p x, obj_p y) {
    i64_t i, count;
    obj_p vec;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_I64, -TYPE_I64):
            count = x->i64;
            vec = I64(count);

            for (i = 0; i < count; i++)
                AS_I64(vec)[i] = ops_rand_u64() % y->i64;

            return vec;

        default:
            THROW(ERR_TYPE, "rand: unsupported types: '%s '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_concat(obj_p x, obj_p y) {
    i64_t i, xl, yl;
    obj_p vec, kx, kxy, ix, iy, dx, dy;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_B8, -TYPE_B8):
            vec = B8(2);
            AS_B8(vec)[0] = x->b8;
            AS_B8(vec)[1] = y->b8;
            return vec;
        case MTYPE2(TYPE_B8, -TYPE_B8):
            xl = x->len;
            vec = B8(xl + 1);
            for (i = 0; i < xl; i++)
                AS_B8(vec)[i] = AS_B8(x)[i];
            AS_B8(vec)[xl] = y->b8;
            return vec;
        case MTYPE2(-TYPE_B8, TYPE_B8):
            yl = y->len;
            vec = B8(yl + 1);
            AS_B8(vec)[0] = x->b8;
            for (i = 1; i <= yl; i++)
                AS_B8(vec)[i] = AS_B8(y)[i - 1];
            return vec;
        case MTYPE2(TYPE_B8, TYPE_B8):
            xl = x->len;
            yl = y->len;
            vec = B8(xl + yl);
            for (i = 0; i < xl; i++)
                AS_B8(vec)[i] = AS_B8(x)[i];
            for (i = 0; i < yl; i++)
                AS_B8(vec)[i + xl] = AS_B8(y)[i];
            return vec;

        case MTYPE2(-TYPE_U8, -TYPE_U8):
            vec = U8(2);
            AS_U8(vec)[0] = x->u8;
            AS_U8(vec)[1] = y->u8;
            return vec;
        case MTYPE2(TYPE_U8, -TYPE_U8):
            xl = x->len;
            vec = U8(xl + 1);
            for (i = 0; i < xl; i++)
                AS_U8(vec)[i] = AS_U8(x)[i];
            AS_U8(vec)[xl] = y->u8;
            return vec;
        case MTYPE2(-TYPE_U8, TYPE_U8):
            yl = y->len;
            vec = U8(yl + 1);
            AS_U8(vec)[0] = x->u8;
            for (i = 1; i <= yl; i++)
                AS_U8(vec)[i] = AS_U8(y)[i - 1];
            return vec;
        case MTYPE2(TYPE_U8, TYPE_U8):
            xl = x->len;
            yl = y->len;
            vec = U8(xl + yl);
            for (i = 0; i < xl; i++)
                AS_U8(vec)[i] = AS_U8(x)[i];
            for (i = 0; i < yl; i++)
                AS_U8(vec)[i + xl] = AS_U8(y)[i];
            return vec;

        case MTYPE2(-TYPE_C8, -TYPE_C8):
            vec = C8(2);
            AS_C8(vec)[0] = x->c8;
            AS_C8(vec)[1] = y->c8;
            return vec;
        case MTYPE2(TYPE_C8, -TYPE_C8):
            xl = ops_count(x);
            if (xl > 0 && AS_C8(x)[xl - 1] == '\0')
                xl--;
            vec = C8(xl + 1);
            for (i = 0; i < xl; i++)
                AS_C8(vec)[i] = AS_C8(x)[i];
            AS_C8(vec)[xl] = y->c8;
            return vec;
        case MTYPE2(-TYPE_C8, TYPE_C8):
            yl = ops_count(y);
            if (yl > 0 && AS_C8(y)[yl - 1] == '\0')
                yl--;
            vec = C8(yl + 1);
            AS_C8(vec)[0] = x->c8;
            for (i = 0; i < yl; i++)
                AS_C8(vec)[i + 1] = AS_C8(y)[i];
            return vec;
        case MTYPE2(TYPE_C8, TYPE_C8):
            xl = ops_count(x);
            yl = ops_count(y);
            if (xl > 0 && AS_C8(x)[xl - 1] == '\0')
                xl--;
            if (yl > 0 && AS_C8(y)[yl - 1] == '\0')
                yl--;
            vec = C8(xl + yl);
            for (i = 0; i < xl; i++)
                AS_C8(vec)[i] = AS_C8(x)[i];
            for (i = 0; i < yl; i++)
                AS_C8(vec)[i + xl] = AS_C8(y)[i];
            return vec;
        case MTYPE2(-TYPE_I16, -TYPE_I16):
            vec = I16(2);
            AS_I16(vec)[0] = x->i16;
            AS_I16(vec)[1] = y->i16;
            return vec;
        case MTYPE2(TYPE_I16, -TYPE_I16):
            xl = x->len;
            vec = I16(xl + 1);
            for (i = 0; i < xl; i++)
                AS_I16(vec)[i] = AS_I16(x)[i];
            AS_I16(vec)[xl] = y->i16;
            return vec;
        case MTYPE2(-TYPE_I16, TYPE_I16):
            yl = y->len;
            vec = I16(yl + 1);
            AS_I16(vec)[0] = x->i16;
            for (i = 1; i <= yl; i++)
                AS_I16(vec)[i] = AS_I16(y)[i - 1];
            return vec;
        case MTYPE2(TYPE_I16, TYPE_I16):
            xl = x->len;
            yl = y->len;
            vec = I16(xl + yl);
            for (i = 0; i < xl; i++)
                AS_I16(vec)[i] = AS_I16(x)[i];
            for (i = 0; i < yl; i++)
                AS_I16(vec)[i + xl] = AS_I16(y)[i];
            return vec;

        case MTYPE2(-TYPE_I32, -TYPE_I32):
        case MTYPE2(-TYPE_DATE, -TYPE_DATE):
        case MTYPE2(-TYPE_TIME, -TYPE_TIME):
            vec = vector(-x->type, 2);
            AS_I32(vec)[0] = x->i32;
            AS_I32(vec)[1] = y->i32;
            return vec;
        case MTYPE2(TYPE_I32, -TYPE_I32):
        case MTYPE2(TYPE_DATE, -TYPE_DATE):
        case MTYPE2(TYPE_TIME, -TYPE_TIME):
            xl = x->len;
            vec = vector(x->type, xl + 1);
            for (i = 0; i < xl; i++)
                AS_I32(vec)[i] = AS_I32(x)[i];
            AS_I32(vec)[xl] = y->i32;
            return vec;
        case MTYPE2(-TYPE_I32, TYPE_I32):
        case MTYPE2(-TYPE_DATE, TYPE_DATE):
        case MTYPE2(-TYPE_TIME, TYPE_TIME):
            yl = y->len;
            vec = vector(y->type, yl + 1);
            AS_I32(vec)[0] = x->i32;
            for (i = 1; i <= yl; i++)
                AS_I32(vec)[i] = AS_I32(y)[i - 1];
            return vec;
        case MTYPE2(TYPE_I32, TYPE_I32):
        case MTYPE2(TYPE_DATE, TYPE_DATE):
        case MTYPE2(TYPE_TIME, TYPE_TIME):
            xl = x->len;
            yl = y->len;
            vec = vector(x->type, xl + yl);
            for (i = 0; i < xl; i++)
                AS_I32(vec)[i] = AS_I32(x)[i];
            for (i = 0; i < yl; i++)
                AS_I32(vec)[i + xl] = AS_I32(y)[i];
            return vec;

        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        case MTYPE2(-TYPE_I64, -TYPE_I64):
        case MTYPE2(-TYPE_SYMBOL, -TYPE_SYMBOL):
            vec = vector(-x->type, 2);
            AS_I64(vec)[0] = x->i64;
            AS_I64(vec)[1] = y->i64;
            return vec;
        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
            xl = x->len;
            vec = vector(x->type, xl + 1);
            for (i = 0; i < xl; i++)
                AS_I64(vec)[i] = AS_I64(x)[i];
            AS_I64(vec)[xl] = y->i64;
            return vec;
        case MTYPE2(-TYPE_I64, TYPE_I64):
        case MTYPE2(-TYPE_SYMBOL, TYPE_SYMBOL):
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            yl = y->len;
            vec = vector(y->type, yl + 1);
            AS_I64(vec)[0] = x->i64;
            for (i = 1; i <= yl; i++)
                AS_I64(vec)[i] = AS_I64(y)[i - 1];
            return vec;
        case MTYPE2(TYPE_I64, TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            xl = x->len;
            yl = y->len;
            vec = vector(x->type, xl + yl);
            for (i = 0; i < xl; i++)
                AS_I64(vec)[i] = AS_I64(x)[i];
            for (i = 0; i < yl; i++)
                AS_I64(vec)[i + xl] = AS_I64(y)[i];
            return vec;

        case MTYPE2(-TYPE_F64, -TYPE_F64):
            vec = F64(2);
            AS_F64(vec)[0] = x->f64;
            AS_F64(vec)[1] = y->f64;
            return vec;
        case MTYPE2(TYPE_F64, -TYPE_F64):
            xl = x->len;
            vec = F64(xl + 1);
            for (i = 0; i < xl; i++)
                AS_F64(vec)[i] = AS_F64(x)[i];
            AS_F64(vec)[xl] = y->f64;
            return vec;
        case MTYPE2(-TYPE_F64, TYPE_F64):
            yl = y->len;
            vec = F64(yl + 1);
            AS_F64(vec)[0] = x->f64;
            for (i = 1; i <= yl; i++)
                AS_F64(vec)[i] = AS_F64(y)[i - 1];
            return vec;
        case MTYPE2(TYPE_F64, TYPE_F64):
            xl = x->len;
            yl = y->len;
            vec = F64(xl + yl);
            for (i = 0; i < xl; i++)
                AS_F64(vec)[i] = AS_F64(x)[i];
            for (i = 0; i < yl; i++)
                AS_F64(vec)[i + xl] = AS_F64(y)[i];
            return vec;

        case MTYPE2(-TYPE_GUID, -TYPE_GUID):
            vec = GUID(2);
            memcpy(&AS_GUID(vec)[0], AS_GUID(x), sizeof(guid_t));
            memcpy(&AS_GUID(vec)[1], AS_GUID(y), sizeof(guid_t));
            return vec;
        case MTYPE2(TYPE_GUID, -TYPE_GUID):
            xl = x->len;
            vec = GUID(xl + 1);
            for (i = 0; i < xl; i++)
                memcpy(AS_GUID(vec)[i], AS_GUID(x)[i], sizeof(guid_t));
            memcpy(&AS_GUID(vec)[xl], AS_GUID(y), sizeof(guid_t));
            return vec;
        case MTYPE2(-TYPE_GUID, TYPE_GUID):
            yl = y->len + 1;
            vec = GUID(yl);
            memcpy(&AS_GUID(vec)[0], AS_GUID(x), sizeof(guid_t));
            for (i = 1; i < yl; i++)
                memcpy(AS_GUID(vec)[i], AS_GUID(y)[i - 1], sizeof(guid_t));
            return vec;
        case MTYPE2(TYPE_GUID, TYPE_GUID):
            xl = x->len;
            yl = y->len;
            vec = GUID(xl + yl);
            for (i = 0; i < xl; i++)
                memcpy(AS_GUID(vec)[i], AS_GUID(x)[i], sizeof(guid_t));
            for (i = 0; i < yl; i++)
                memcpy(AS_GUID(vec)[i + xl], AS_GUID(y)[i], sizeof(guid_t));
            return vec;

        case MTYPE2(TYPE_DICT, TYPE_DICT):
            kxy = ray_sect(AS_LIST(x)[0], AS_LIST(y)[0]);
            dx = ray_except(AS_LIST(x)[0], kxy);
            dy = ray_except(AS_LIST(y)[0], kxy);

            vec = vector(TYPE_LIST, dx->len + kxy->len + dy->len);

            // add items in x that are not in y
            ix = ray_find(AS_LIST(x)[0], dx);
            for (i = 0; i < (i64_t)dx->len; i++) {
                AS_LIST(vec)[AS_I64(ix)[i]] = clone_obj(AS_LIST(AS_LIST(x)[1])[AS_I64(ix)[i]]);
            }
            drop_obj(ix);

            // add items in x and y with the same key
            ix = ray_find(AS_LIST(x)[0], kxy);
            iy = ray_find(AS_LIST(y)[0], kxy);
            for (i = 0; i < (i64_t)kxy->len; i++) {
                AS_LIST(vec)[AS_I64(ix)[i]] = clone_obj(AS_LIST(AS_LIST(y)[1])[AS_I64(iy)[i]]);
            }
            drop_obj(ix);
            drop_obj(iy);

            // add items in y that are not in x
            iy = ray_find(AS_LIST(y)[0], dy);
            for (i = 0; i < (i64_t)dy->len; i++) {
                AS_LIST(vec)[i + dx->len + kxy->len] = clone_obj(AS_LIST(AS_LIST(y)[1])[AS_I64(iy)[i]]);
            }
            drop_obj(iy);

            vec = dict(ray_concat(AS_LIST(x)[0], dy), vec);
            drop_obj(kxy);
            drop_obj(dx);
            drop_obj(dy);
            return vec;

        case MTYPE2(TYPE_TABLE, TYPE_TABLE):
            kx = ray_key(x);
            kxy = ray_sect(AS_LIST(x)[0], AS_LIST(y)[0]);
            if (kx->len != kxy->len || cmp_obj(kx, kxy) != 0) {
                drop_obj(kx);
                drop_obj(kxy);
                THROW(ERR_TYPE, "concat: keys a not compatible");
            }
            iy = ray_find(AS_LIST(y)[0], AS_LIST(x)[0]);

            for (i = 0; i < (i64_t)kx->len; i++) {
                if (AS_LIST(AS_LIST(x)[1])[i]->type != AS_LIST(AS_LIST(y)[1])[AS_I64(iy)[i]]->type) {
                    drop_obj(kx);
                    drop_obj(kxy);
                    drop_obj(iy);
                    THROW(ERR_TYPE, "concat: values a not compatible");
                }
            }
            vec = vector(TYPE_LIST, kx->len);
            for (i = 0; i < (i64_t)kx->len; i++) {
                AS_LIST(vec)[i] = ray_concat(AS_LIST(AS_LIST(x)[1])[i], AS_LIST(AS_LIST(y)[1])[AS_I64(iy)[i]]);
            }
            drop_obj(kx);
            drop_obj(kxy);
            drop_obj(iy);
            return table(clone_obj(AS_LIST(x)[0]), vec);

        case MTYPE2(TYPE_LIST, TYPE_LIST):
            xl = x->len;
            yl = y->len;
            vec = LIST(xl + yl);
            for (i = 0; i < xl; i++)
                AS_LIST(vec)[i] = clone_obj(AS_LIST(x)[i]);
            for (i = 0; i < yl; i++)
                AS_LIST(vec)[i + xl] = clone_obj(AS_LIST(y)[i]);
            return vec;

        default:
            if (x->type == TYPE_LIST) {
                xl = x->len;
                vec = LIST(xl + 1);
                for (i = 0; i < xl; i++)
                    AS_LIST(vec)[i] = clone_obj(AS_LIST(x)[i]);

                AS_LIST(vec)[xl] = clone_obj(y);
                return vec;
            }
            if (y->type == TYPE_LIST) {
                yl = y->len;
                vec = LIST(yl + 1);
                AS_LIST(vec)[0] = clone_obj(x);
                for (i = 0; i < yl; i++)
                    AS_LIST(vec)[i + 1] = clone_obj(AS_LIST(y)[i]);
                return vec;
            }
            vec = LIST(2);
            AS_LIST(vec)[0] = clone_obj(x);
            AS_LIST(vec)[1] = clone_obj(y);
            return vec;
    }
}

obj_p ray_remove(obj_p x, obj_p y) {
    obj_p r;
    switch (y->type) {
        case -TYPE_I32:
            r = cow_obj(x);
            return remove_idx(&r, y->i32);
        case -TYPE_I64:
            r = cow_obj(x);
            return remove_idx(&r, y->i64);
        default:
            THROW(ERR_TYPE, "remove: unsupported type: '%s", type_name(y->type));
    }
}

obj_p ray_distinct(obj_p x) {
    obj_p res = NULL;
    i64_t l;

    switch (x->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            l = ops_count(x);
            res = index_distinct_i8((i8_t *)AS_U8(x), l);
            res->type = x->type;
            return res;
        case TYPE_I16:
            l = x->len;
            res = index_distinct_i16(AS_I16(x), l);
            res->type = x->type;
            return res;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            l = x->len;
            res = index_distinct_i32(AS_I32(x), l);
            res->type = x->type;
            return res;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            l = x->len;
            res = index_distinct_i64(AS_I64(x), l);
            res->type = x->type;
            return res;
        case TYPE_ENUM:
            l = ops_count(x);
            res = index_distinct_i64(AS_I64(ENUM_VAL(x)), l);
            res = enumerate(ray_key(x), res);
            return res;
        case TYPE_LIST:
            l = ops_count(x);
            res = index_distinct_obj(AS_LIST(x), l);
            return res;
        case TYPE_GUID:
            l = x->len;
            res = index_distinct_guid(AS_GUID(x), l);
            return res;
        default:
            THROW(ERR_TYPE, "distinct: invalid type: '%s", type_name(x->type));
    }
}

obj_p ray_group(obj_p x) {
    obj_p k, v, index;

    index = index_group(x, NULL_OBJ);
    v = aggr_row(x, index);
    k = aggr_first(x, index);
    drop_obj(index);

    return dict(k, v);
}

obj_p ray_diverse(obj_p x) {
    obj_p res;

    res = cow_obj(x);
    return diverse_obj(&res);
}

obj_p ray_unify(obj_p x) {
    obj_p res;

    res = cow_obj(x);
    return unify_list(&res);
}

obj_p ray_row(obj_p x) {
    switch (x->type) {
        case TYPE_MAPGROUP:
            return aggr_row(AS_LIST(x)[0], AS_LIST(x)[1]);
        default:
            return i64(ops_count(x));
    }
}

#define CUT_VECTOR(x, xt, y, yt)                                                                       \
    ({                                                                                                 \
        i64_t $i, $j, $xl, $yl;                                                                        \
        yt##_t $id, $last_id;                                                                          \
        obj_p $v, $res;                                                                                \
        $xl = x->len;                                                                                  \
        $yl = y->len;                                                                                  \
        if ($yl > $xl)                                                                                 \
            THROW(ERR_LENGTH, "cut: vector length mismatch: %d, %d", $xl, $yl);                        \
                                                                                                       \
        $res = LIST($yl);                                                                              \
        $last_id = __AS_##yt(y)[0];                                                                    \
        if ($last_id < 0 || $last_id >= $xl) {                                                         \
            drop_obj($res);                                                                            \
            THROW(ERR_INDEX, "cut: invalid index or index vector is not sorted: %lld", $last_id);      \
        }                                                                                              \
                                                                                                       \
        for ($i = 0; $i < $yl; $i++) {                                                                 \
            $id = ($i == $yl - 1) ? $xl : __AS_##yt(y)[$i + 1];                                        \
            if ($id < $last_id || $id > $xl) {                                                         \
                $res->len = $i;                                                                        \
                drop_obj($res);                                                                        \
                THROW(ERR_INDEX, "cut: invalid index or index vector is not sorted: %lld", $id);       \
            }                                                                                          \
                                                                                                       \
            if ($id == $last_id)                                                                       \
                $v = vector(x->type, 0);                                                               \
            else {                                                                                     \
                $v = vector(x->type, $id - $last_id);                                                  \
                if (x->type == TYPE_LIST) {                                                            \
                    for ($j = 0; $j < $id - $last_id; $j++)                                            \
                        AS_LIST($v)[$j] = clone_obj(AS_LIST(x)[$last_id + $j]);                        \
                    unify_list(&$v);                                                                   \
                } else {                                                                               \
                    memcpy(__AS_##xt($v), &__AS_##xt(x)[$last_id], ($id - $last_id) * __SIZE_OF_##xt); \
                }                                                                                      \
            }                                                                                          \
                                                                                                       \
            AS_LIST($res)[$i] = $v;                                                                    \
            $last_id = $id;                                                                            \
        }                                                                                              \
        $res;                                                                                          \
    })

obj_p cut_vector(obj_p x, obj_p y) {
    if (y->len == 0)
        return NULL_OBJ;

    switch (x->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            switch (y->type) {
                case TYPE_I16:
                    return CUT_VECTOR(x, u8, y, i16);
                case TYPE_I32:
                    return CUT_VECTOR(x, u8, y, i32);
                case TYPE_I64:
                    return CUT_VECTOR(x, u8, y, i64);
                default:
                    THROW(ERR_TYPE, "cut: unsupported index type: '%s", type_name(y->type));
            }
        case TYPE_I16:
            switch (y->type) {
                case TYPE_I16:
                    return CUT_VECTOR(x, i16, y, i16);
                case TYPE_I32:
                    return CUT_VECTOR(x, i16, y, i32);
                case TYPE_I64:
                    return CUT_VECTOR(x, i16, y, i64);
                default:
                    THROW(ERR_TYPE, "cut: unsupported index type: '%s", type_name(y->type));
            }
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            switch (y->type) {
                case TYPE_I16:
                    return CUT_VECTOR(x, i32, y, i16);
                case TYPE_I32:
                    return CUT_VECTOR(x, i32, y, i32);
                case TYPE_I64:
                    return CUT_VECTOR(x, i32, y, i64);
                default:
                    THROW(ERR_TYPE, "cut: unsupported index type: '%s", type_name(y->type));
            }
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            switch (y->type) {
                case TYPE_I16:
                    return CUT_VECTOR(x, i64, y, i16);
                case TYPE_I32:
                    return CUT_VECTOR(x, i64, y, i32);
                case TYPE_I64:
                    return CUT_VECTOR(x, i64, y, i64);
                default:
                    THROW(ERR_TYPE, "cut: unsupported index type: '%s", type_name(y->type));
            }
        case TYPE_F64:
            switch (y->type) {
                case TYPE_I16:
                    return CUT_VECTOR(x, f64, y, i16);
                case TYPE_I32:
                    return CUT_VECTOR(x, f64, y, i32);
                case TYPE_I64:
                    return CUT_VECTOR(x, f64, y, i64);
                default:
                    THROW(ERR_TYPE, "cut: unsupported index type: '%s", type_name(y->type));
            }
        case TYPE_GUID:
            switch (y->type) {
                case TYPE_I16:
                    return CUT_VECTOR(x, guid, y, i16);
                case TYPE_I32:
                    return CUT_VECTOR(x, guid, y, i32);
                case TYPE_I64:
                    return CUT_VECTOR(x, guid, y, i64);
                default:
                    THROW(ERR_TYPE, "cut: unsupported index type: '%s", type_name(y->type));
            }
        case TYPE_LIST:
            switch (y->type) {
                case TYPE_I16:
                    return CUT_VECTOR(x, list, y, i16);
                case TYPE_I32:
                    return CUT_VECTOR(x, list, y, i32);
                case TYPE_I64:
                    return CUT_VECTOR(x, list, y, i64);
                default:
                    THROW(ERR_TYPE, "cut: unsupported index type: '%s", type_name(y->type));
            }
        default:
            THROW(ERR_TYPE, "cut: unsupported vector type: '%s", type_name(x->type));
    }
}

obj_p ray_split(obj_p x, obj_p y) {
    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_SYMBOL, -TYPE_C8):
            return str_split(str_from_symbol(x->i64), SYMBOL_STRLEN(x->i64), &y->c8, 1);
        case MTYPE2(-TYPE_SYMBOL, TYPE_C8):
            return str_split(str_from_symbol(x->i64), SYMBOL_STRLEN(x->i64), AS_C8(y), y->len);
        case MTYPE2(TYPE_C8, -TYPE_C8):
            return str_split(AS_C8(x), x->len, &y->c8, 1);
        case MTYPE2(TYPE_C8, TYPE_C8):
            return str_split(AS_C8(x), x->len, AS_C8(y), y->len);
        default:
            if (IS_VECTOR(x))
                return cut_vector(x, y);

            THROW(ERR_TYPE, "split: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}
