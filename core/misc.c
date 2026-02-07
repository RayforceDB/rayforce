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
#include "items.h"
#include "filter.h"
#include "lambda.h"

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
            obj_p collected = filter_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
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
            obj_p flat = ray_value(x);
            i64_t count = flat->len;
            drop_obj(flat);
            return i64(count);
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

// Helper to create common metadata dict with type, mmod, attrs
static obj_p meta_common(obj_p x) {
    obj_p keys, vals;

    keys = SYMBOL(3);
    ins_sym(&keys, 0, "type");
    ins_sym(&keys, 1, "mmod");
    ins_sym(&keys, 2, "attrs");

    vals = LIST(3);
    AS_LIST(vals)[0] = symboli64(env_get_typename_by_type(&runtime_get()->env, x->type));
    AS_LIST(vals)[1] = i64(x->mmod);
    AS_LIST(vals)[2] = i64(x->attrs);

    return dict(keys, vals);
}

// Helper to create metadata dict for vectors with type, len, mmod, attrs
static obj_p meta_vector(obj_p x) {
    obj_p keys, vals;

    keys = SYMBOL(4);
    ins_sym(&keys, 0, "type");
    ins_sym(&keys, 1, "len");
    ins_sym(&keys, 2, "mmod");
    ins_sym(&keys, 3, "attrs");

    vals = LIST(4);
    AS_LIST(vals)[0] = symboli64(env_get_typename_by_type(&runtime_get()->env, x->type));
    AS_LIST(vals)[1] = i64(x->len);
    AS_LIST(vals)[2] = i64(x->mmod);
    AS_LIST(vals)[3] = i64(x->attrs);

    return dict(keys, vals);
}

// Metadata for table
static obj_p meta_table(obj_p x) {
    i64_t i, l;
    obj_p keys, vals;

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
        AS_SYMBOL(AS_LIST(vals)[1])[i] = env_get_typename_by_type(&runtime_get()->env, AS_LIST(AS_LIST(x)[1])[i]->type);
        AS_I64(AS_LIST(vals)[2])[i] = AS_LIST(x)[0]->mmod;
        AS_I64(AS_LIST(vals)[3])[i] = AS_LIST(x)[0]->attrs;
    }

    return table(keys, vals);
}

// Metadata for dict
static obj_p meta_dict(obj_p x) {
    obj_p keys, vals;
    obj_p dict_keys = AS_LIST(x)[0];
    obj_p dict_vals = AS_LIST(x)[1];

    keys = SYMBOL(5);
    ins_sym(&keys, 0, "type");
    ins_sym(&keys, 1, "len");
    ins_sym(&keys, 2, "key_type");
    ins_sym(&keys, 3, "val_type");
    ins_sym(&keys, 4, "keys");

    vals = LIST(5);
    AS_LIST(vals)[0] = symboli64(env_get_typename_by_type(&runtime_get()->env, x->type));
    AS_LIST(vals)[1] = i64(dict_keys->len);
    AS_LIST(vals)[2] = symboli64(env_get_typename_by_type(&runtime_get()->env, dict_keys->type));
    AS_LIST(vals)[3] = symboli64(env_get_typename_by_type(&runtime_get()->env, dict_vals->type));
    AS_LIST(vals)[4] = clone_obj(dict_keys);

    return dict(keys, vals);
}

// Metadata for lambda
static obj_p meta_lambda(obj_p x) {
    obj_p keys, vals;
    lambda_p lam = AS_LAMBDA(x);

    keys = SYMBOL(5);
    ins_sym(&keys, 0, "type");
    ins_sym(&keys, 1, "name");
    ins_sym(&keys, 2, "arity");
    ins_sym(&keys, 3, "args");
    ins_sym(&keys, 4, "body");

    vals = LIST(5);
    AS_LIST(vals)[0] = symboli64(env_get_typename_by_type(&runtime_get()->env, x->type));
    AS_LIST(vals)[1] = clone_obj(lam->name);
    AS_LIST(vals)[2] = i64(lam->args->len);
    AS_LIST(vals)[3] = clone_obj(lam->args);
    AS_LIST(vals)[4] = clone_obj(lam->body);

    return dict(keys, vals);
}

// Metadata for list
static obj_p meta_list(obj_p x) {
    i64_t i, l;
    obj_p keys, vals, elem_types;

    l = x->len;
    keys = SYMBOL(4);
    ins_sym(&keys, 0, "type");
    ins_sym(&keys, 1, "len");
    ins_sym(&keys, 2, "mmod");
    ins_sym(&keys, 3, "elem_types");

    elem_types = SYMBOL(l);
    for (i = 0; i < l; i++) {
        AS_SYMBOL(elem_types)[i] = env_get_typename_by_type(&runtime_get()->env, AS_LIST(x)[i]->type);
    }

    vals = LIST(4);
    AS_LIST(vals)[0] = symboli64(env_get_typename_by_type(&runtime_get()->env, x->type));
    AS_LIST(vals)[1] = i64(l);
    AS_LIST(vals)[2] = i64(x->mmod);
    AS_LIST(vals)[3] = elem_types;

    return dict(keys, vals);
}

// Metadata for enum
static obj_p meta_enum(obj_p x) {
    obj_p keys, vals;

    keys = SYMBOL(4);
    ins_sym(&keys, 0, "type");
    ins_sym(&keys, 1, "len");
    ins_sym(&keys, 2, "mmod");
    ins_sym(&keys, 3, "domain");

    vals = LIST(4);
    AS_LIST(vals)[0] = symboli64(env_get_typename_by_type(&runtime_get()->env, x->type));
    AS_LIST(vals)[1] = i64(x->len);
    AS_LIST(vals)[2] = i64(x->mmod);
    AS_LIST(vals)[3] = clone_obj(AS_LIST(x)[0]);  // domain is stored as first element

    return dict(keys, vals);
}

obj_p ray_meta(obj_p x) {
    // Handle atoms (negative types)
    if (x->type < 0) {
        return meta_common(x);
    }

    switch (x->type) {
        case TYPE_TABLE:
            return meta_table(x);

        case TYPE_DICT:
            return meta_dict(x);

        case TYPE_LAMBDA:
            return meta_lambda(x);

        case TYPE_LIST:
            return meta_list(x);

        case TYPE_ENUM:
            return meta_enum(x);

        // Vectors
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_C8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_I64:
        case TYPE_F64:
        case TYPE_SYMBOL:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_TIMESTAMP:
        case TYPE_GUID:
            return meta_vector(x);

        // Parted types
        case TYPE_PARTEDLIST:
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
            return meta_vector(x);

        // Map types
        case TYPE_MAPFILTER:
        case TYPE_MAPGROUP:
        case TYPE_MAPFD:
        case TYPE_MAPCOMMON:
        case TYPE_MAPLIST:
            return meta_common(x);

        // Other special types
        case TYPE_UNARY:
        case TYPE_BINARY:
        case TYPE_VARY:
        case TYPE_TOKEN:
            return meta_common(x);

        case TYPE_NULL:
        case TYPE_ERR:
            return meta_common(x);

        default:
            return meta_common(x);
    }
}