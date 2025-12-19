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

#include "misc.h"
#include "ops.h"
#include "runtime.h"
#include "aggr.h"
#include "index.h"
#include "filter.h"

obj_p ray_type(obj_p x) {
    i8_t type;
    if (!x)
        type = -TYPE_ERR;
    else
        type = x->type;

    i64_t t = env_get_typename_by_type(&runtime_get()->env, type);
    return symboli64(t);
}

obj_p ray_count(obj_p x) {
    switch (x->type) {
        case TYPE_MAPGROUP:
            return aggr_count(AS_LIST(x)[0], AS_LIST(x)[1]);
        case TYPE_MAPFILTER: {
            obj_p val = AS_LIST(x)[0];
            obj_p filter = AS_LIST(x)[1];
            if (val->type >= TYPE_PARTEDLIST && val->type <= TYPE_PARTEDGUID && filter->type == TYPE_PARTEDI64) {
                obj_p index = vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ,
                                      clone_obj(filter), NULL_OBJ);
                obj_p res = aggr_count(val, index);
                drop_obj(index);
                return res;
            }
            obj_p collected = filter_collect(val, filter);
            obj_p res = ray_count(collected);
            drop_obj(collected);
            return res;
        }
        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDTIMESTAMP:
        case TYPE_PARTEDGUID:
        case TYPE_PARTEDENUM:
        case TYPE_PARTEDLIST: {
            obj_p index =
                vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ, NULL_OBJ, NULL_OBJ);
            obj_p res = aggr_count(x, index);
            drop_obj(index);
            return res;
        }
        default:
            return i64(ops_count(x));
    }
}

obj_p ray_rc(obj_p x) {
    // substract 1 to skip the our reference
    return i64(rc_obj(x) - 1);
}

obj_p ray_quote(obj_p x) { return clone_obj(x); }

obj_p ray_meta(obj_p x) {
    i64_t i, l;
    obj_p keys, vals;

    switch (x->type) {
        case TYPE_TABLE:
            keys = SYMBOL(4);
            ins_sym(&keys, 0, "name");
            ins_sym(&keys, 1, "type");
            ins_sym(&keys, 2, "mmod");
            ins_sym(&keys, 3, "attrs");

            l = AS_LIST(x)[0]->len;
            vals = LIST(4);
            AS_LIST(vals)[0] = clone_obj(AS_LIST(x)[0]);
            AS_LIST(vals)[1] = SYMBOL(l);
            AS_LIST(vals)[2] = I64(l);
            AS_LIST(vals)[3] = I64(l);

            for (i = 0; i < l; i++) {
                AS_SYMBOL(AS_LIST(vals)[1])
                [i] = env_get_typename_by_type(&runtime_get()->env, AS_LIST(AS_LIST(x)[1])[i]->type);
                AS_I64(AS_LIST(vals)[2])[i] = AS_LIST(x)[0]->mmod;
                AS_I64(AS_LIST(vals)[3])[i] = AS_LIST(x)[0]->attrs;
            }

            return table(keys, vals);
        default:
            return NULL_OBJ;
    }
}