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
#include "util.h"
#include "heap.h"
#include "ops.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "runtime.h"
#include "items.h"
#include "guid.h"
#include "index.h"
#include "error.h"
#include "string.h"
#include "group.h"
#include "aggr.h"

obj_p ray_cast_obj(obj_p x, obj_p y) {
    i8_t type;
    obj_p err;
    obj_p fmt, msg;

    if (x->type != -TYPE_SYMBOL)
        THROW(ERR_TYPE, "as: first argument must be a symbol");

    type = env_get_type_by_type_name(&runtime_get()->env, x->i64);

    if (type == TYPE_ERROR) {
        fmt = obj_fmt(x, B8_TRUE);
        msg = str_fmt(-1, "as: not a type: '%s", fmt);
        err = error_obj(ERR_TYPE, msg);
        heap_free(fmt);
        return err;
    }

    return cast_obj(type, y);
}

obj_p ray_til(obj_p x) {
    if (x->type != -TYPE_I64)
        return error_str(ERR_TYPE, "til: expected i64");

    i32_t i, l = (i32_t)x->i64;
    obj_p vec = NULL;

    vec = I64(l);

    if (IS_ERROR(vec))
        return vec;

    for (i = 0; i < l; i++)
        AS_I64(vec)[i] = i;

    vec->attrs = ATTR_ASC | ATTR_DISTINCT;

    return vec;
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

            return res;

        case TYPE_I64:
        case TYPE_TIMESTAMP:
        case TYPE_SYMBOL:
            l = x->len;
            res = vector(x->type, l);

            for (i = 0; i < l; i++)
                AS_I64(res)[i] = AS_I64(x)[l - i - 1];

            return res;

        case TYPE_F64:
            l = x->len;
            res = F64(l);

            for (i = 0; i < l; i++)
                AS_F64(res)[i] = AS_F64(x)[l - i - 1];

            return res;

        case TYPE_LIST:
            l = x->len;
            res = vector(TYPE_LIST, l);

            for (i = 0; i < l; i++)
                AS_LIST(res)[i] = clone_obj(AS_LIST(x)[l - i - 1]);

            return res;

        default:
            THROW(ERR_TYPE, "reverse: unsupported type: '%s", type_name(x->type));
    }
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
    u64_t i, j, len, cl = 0;
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
            case -TYPE_I64:
            case -TYPE_F64:
            case -TYPE_C8:
            case -TYPE_SYMBOL:
            case -TYPE_TIMESTAMP:
            case -TYPE_GUID:
            case TYPE_LAMBDA:
            case TYPE_DICT:
            case TYPE_TABLE:
                synergy = B8_FALSE;
                break;
            case TYPE_B8:
            case TYPE_U8:
            case TYPE_I64:
            case TYPE_F64:
            case TYPE_TIMESTAMP:
            case TYPE_C8:
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
            case -TYPE_I64:
            case -TYPE_F64:
            case -TYPE_C8:
            case -TYPE_SYMBOL:
            case -TYPE_TIMESTAMP:
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

obj_p ray_list(obj_p *x, u64_t n) {
    u64_t i;
    obj_p lst = LIST(n);

    for (i = 0; i < n; i++)
        AS_LIST(lst)[i] = clone_obj(x[i]);

    return lst;
}

obj_p ray_enlist(obj_p *x, u64_t n) {
    u64_t i;
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

            if (IS_ERROR(s))
                return s;

            if (!s || s->type != TYPE_SYMBOL) {
                drop_obj(s);
                THROW(ERR_TYPE, "enum: expected vector symbol");
            }

            v = index_find_i64(AS_I64(s), s->len, AS_I64(y), y->len);
            drop_obj(s);

            if (IS_ERROR(v)) {
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
    obj_p vec;

    if (!x || !y)
        return vn_list(2, clone_obj(x), clone_obj(y));

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_B8, -TYPE_B8):
            vec = B8(2);
            AS_B8(vec)[0] = x->b8;
            AS_B8(vec)[1] = y->b8;
            return vec;

        case MTYPE2(-TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
        case MTYPE2(-TYPE_I64, -TYPE_I64):
        case MTYPE2(-TYPE_SYMBOL, -TYPE_SYMBOL):
            vec = vector(-x->type, 2);
            AS_I64(vec)[0] = x->i64;
            AS_I64(vec)[1] = y->i64;
            return vec;

        case MTYPE2(-TYPE_F64, -TYPE_F64):
            vec = F64(2);
            AS_F64(vec)[0] = x->f64;
            AS_F64(vec)[1] = y->f64;
            return vec;

        case MTYPE2(-TYPE_GUID, -TYPE_GUID):
            vec = GUID(2);
            memcpy(&AS_GUID(vec)[0], AS_GUID(x), sizeof(guid_t));
            memcpy(&AS_GUID(vec)[1], AS_GUID(y), sizeof(guid_t));
            return vec;

        case MTYPE2(-TYPE_C8, -TYPE_C8):
            vec = C8(2);
            AS_C8(vec)[0] = x->c8;
            AS_C8(vec)[1] = y->c8;
            return vec;

        case MTYPE2(TYPE_B8, -TYPE_B8):
            xl = x->len;
            vec = B8(xl + 1);
            for (i = 0; i < xl; i++)
                AS_B8(vec)[i] = AS_B8(x)[i];

            AS_B8(vec)[xl] = y->b8;
            return vec;

        case MTYPE2(TYPE_I64, -TYPE_I64):
        case MTYPE2(TYPE_SYMBOL, -TYPE_SYMBOL):
        case MTYPE2(TYPE_TIMESTAMP, -TYPE_TIMESTAMP):
            xl = x->len;
            vec = I64(xl + 1);
            for (i = 0; i < xl; i++)
                AS_I64(vec)[i] = AS_I64(x)[i];

            AS_I64(vec)[xl] = y->i64;
            return vec;

        case MTYPE2(-TYPE_I64, TYPE_I64):
        case MTYPE2(-TYPE_SYMBOL, TYPE_SYMBOL):
        case MTYPE2(-TYPE_TIMESTAMP, TYPE_TIMESTAMP):
            yl = y->len;
            vec = vector(x->type, yl + 1);
            AS_I64(vec)[0] = x->i64;
            for (i = 1; i <= yl; i++)
                AS_I64(vec)[i] = AS_I64(y)[i - 1];

            return vec;

        case MTYPE2(TYPE_F64, -TYPE_F64):
            xl = x->len;
            vec = F64(xl + 1);
            for (i = 0; i < xl; i++)
                AS_F64(vec)[i] = AS_F64(x)[i];

            AS_F64(vec)[xl] = y->f64;
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
                memcpy(AS_GUID(vec)[i], AS_GUID(y)[i], sizeof(guid_t));

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

        case MTYPE2(TYPE_F64, TYPE_F64):
            xl = x->len;
            yl = y->len;
            vec = F64(xl + yl);
            for (i = 0; i < xl; i++)
                AS_F64(vec)[i] = AS_F64(x)[i];
            for (i = 0; i < yl; i++)
                AS_F64(vec)[i + xl] = AS_F64(y)[i];
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

        case MTYPE2(TYPE_C8, TYPE_C8):
            xl = ops_count(x);
            yl = ops_count(y);

            if (xl > 0 && AS_C8(x)[xl - 1] == '\0')
                xl--;

            vec = C8(xl + yl);

            for (i = 0; i < xl; i++)
                AS_C8(vec)[i] = AS_C8(x)[i];
            for (i = 0; i < yl; i++)
                AS_C8(vec)[i + xl] = AS_C8(y)[i];

            return vec;

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

            THROW(ERR_TYPE, "concat: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_distinct(obj_p x) {
    obj_p res = NULL;
    u64_t l;

    switch (x->type) {
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
            l = ops_count(x);
            res = index_distinct_i8((i8_t *)AS_U8(x), l, x->type == TYPE_C8);
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
    v = aggr_ids(x, index);
    k = aggr_first(x, index);
    drop_obj(index);

    return dict(k, v);
}
