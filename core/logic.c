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
#include "eval.h"

typedef nil_t (*logic_op_f)(b8_t *, b8_t *, u64_t);

// Actual logic operation
static obj_p ray_logic_op(obj_p *x, u64_t n, lit_p op_name, logic_op_f op_func) {
    b8_t *mask, *next_mask;
    u64_t i, l;
    obj_p next, res;

    if (n == 0)
        return B8(B8_FALSE);

    // Evaluate first expression and validate type
    res = eval(x[0]);
    if (IS_ERR(res) || n == 1)
        return res;

    if (res->type != TYPE_B8) {
        drop_obj(res);
        THROW(ERR_TYPE, "%s: unsupported types: '%s, '%s", op_name, type_name(res->type), type_name(TYPE_B8));
    }

    l = res->len;
    mask = AS_B8(res);

    // Process remaining expressions
    for (i = 1; i < n; i++) {
        next = eval(x[i]);
        if (IS_ERR(next)) {
            drop_obj(res);
            return next;
        }

        if (next->type != TYPE_B8) {
            drop_obj(res);
            drop_obj(next);
            THROW(ERR_TYPE, "%s: unsupported types: '%s, '%s", op_name, type_name(next->type), type_name(TYPE_B8));
        }

        if (next->len != l) {
            drop_obj(res);
            drop_obj(next);
            THROW(ERR_TYPE, "%s: different lengths: '%ld, '%ld", op_name, l, next->len);
        }

        // Perform element-wise operation using the provided function
        next_mask = AS_B8(next);
        op_func(mask, next_mask, l);

        drop_obj(next);
    }

    return res;
}

// Operation functions
static nil_t and_op(b8_t *mask, b8_t *next_mask, u64_t n) {
    u64_t i;

    for (i = 0; i < n; i++)
        mask[i] &= next_mask[i];
}

static nil_t or_op(b8_t *mask, b8_t *next_mask, u64_t n) {
    u64_t i;

    for (i = 0; i < n; i++)
        mask[i] |= next_mask[i];
}

obj_p ray_and(obj_p *x, u64_t n) { return ray_logic_op(x, n, "and", and_op); }

obj_p ray_or(obj_p *x, u64_t n) { return ray_logic_op(x, n, "or", or_op); }

// obj_p ray_or(obj_p x, obj_p y) {
//     u64_t i, l;
//     obj_p v, res;

//     switch (MTYPE2(x->type, y->type)) {
//         case MTYPE2(-TYPE_B8, -TYPE_B8):
//             return (b8(x->b8 || y->b8));

//         case MTYPE2(TYPE_B8, TYPE_B8):
//             l = x->len;
//             res = B8(x->len);
//             for (i = 0; i < l; i++)
//                 AS_B8(res)[i] = AS_B8(x)[i] | AS_B8(y)[i];

//             return res;

//         case MTYPE2(TYPE_PARTEDB8, TYPE_PARTEDB8):
//             l = x->len;
//             if (l != y->len)
//                 THROW(ERR_TYPE, "or: different lengths: '%ld, '%ld", x->len, y->len);

//             res = LIST(l);
//             res->type = TYPE_PARTEDB8;
//             for (i = 0; i < l; i++) {
//                 if (AS_LIST(x)[i] == NULL_OBJ)
//                     AS_LIST(res)[i] = clone_obj(AS_LIST(y)[i]);
//                 else if (AS_LIST(y)[i] == NULL_OBJ)
//                     AS_LIST(res)[i] = clone_obj(AS_LIST(x)[i]);
//                 else if (AS_LIST(x)[i]->type == -TYPE_B8 || AS_LIST(y)[i]->type == -TYPE_B8)
//                     AS_LIST(res)[i] = b8(B8_TRUE);
//                 else {
//                     v = ray_or(AS_LIST(x)[i], AS_LIST(y)[i]);
//                     if (IS_ERR(v)) {
//                         res->len = i;
//                         drop_obj(res);
//                         return v;
//                     }

//                     AS_LIST(res)[i] = v;
//                 }
//             }

//             return res;

//         default:
//             THROW(ERR_TYPE, "or: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
//     }
// }

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

        case MTYPE2(TYPE_MAPLIST, TYPE_C8):
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

        case MTYPE2(TYPE_PARTEDLIST, TYPE_C8):
            l = x->len;
            res = LIST(l);
            for (i = 0; i < l; i++) {
                e = AS_LIST(x)[i];
                AS_LIST(res)[i] = ray_like(e, y);
            }

            return res;

        default:
            THROW(ERR_TYPE, "like: unsupported types: '%s, '%s", type_name(x->type), type_name(y->type));
    }
}
