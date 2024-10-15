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

#include "logic.h"
#include "util.h"
#include "heap.h"
#include "ops.h"
#include "error.h"
#include "string.h"

obj_p ray_and(obj_p x, obj_p y) {
    i32_t i;
    i64_t l;
    obj_p res;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_B8, -TYPE_B8):
            return (b8(x->b8 && y->b8));

        case MTYPE2(TYPE_B8, TYPE_B8):
            l = x->len;
            res = B8(x->len);
            for (i = 0; i < l; i++)
                AS_B8(res)[i] = AS_B8(x)[i] & AS_B8(y)[i];

            return res;

        default:
            THROW(ERR_TYPE, "and: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_or(obj_p x, obj_p y) {
    i32_t i;
    i64_t l;
    obj_p res;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(-TYPE_B8, -TYPE_B8):
            return (b8(x->b8 || y->b8));

        case MTYPE2(TYPE_B8, TYPE_B8):
            l = x->len;
            res = B8(x->len);
            for (i = 0; i < l; i++)
                AS_B8(res)[i] = AS_B8(x)[i] | AS_B8(y)[i];

            return res;

        default:
            THROW(ERR_TYPE, "or: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}

obj_p ray_like(obj_p x, obj_p y) {
    i64_t i, l;
    obj_p res, e;

    switch (MTYPE2(x->type, y->type)) {
        case MTYPE2(TYPE_C8, TYPE_C8):
            return (b8(str_match(AS_C8(x), x->len, AS_C8(y), y->len)));
        case MTYPE2(TYPE_LIST, TYPE_C8):
            l = x->len;
            res = B8(l);
            for (i = 0; i < l; i++) {
                e = AS_LIST(x)[i];
                if (!e || e->type != TYPE_C8) {
                    res->len = i;
                    drop_obj(res);
                    THROW(ERR_TYPE, "like: unsupported types: '%s, %s", type_name(e->type), type_name(y->type));
                }

                AS_B8(res)[i] = str_match(AS_C8(e), e->len, AS_C8(y), y->len);
            }

            return res;

        case MTYPE2(TYPE_ANYMAP, TYPE_C8):
            l = x->len;
            res = B8(l);
            for (i = 0; i < l; i++) {
                e = at_idx(x, i);
                if (e == NULL_OBJ || e->type != TYPE_C8) {
                    res->len = i;
                    drop_obj(res);
                    drop_obj(e);
                    THROW(ERR_TYPE, "like: unsupported types: '%s, '%s", type_name(e->type), type_name(y->type));
                }

                AS_B8(res)[i] = str_match(AS_C8(e), e->len, AS_C8(y), y->len);
                drop_obj(e);
            }

            return res;

        default:
            THROW(ERR_TYPE, "like: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}
