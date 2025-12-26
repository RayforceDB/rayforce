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
#include "ops.h"
#include "error.h"
#include "string.h"
#include "eval.h"
#include "runtime.h"

typedef obj_p (*logic_op_f)(raw_p, raw_p, raw_p, raw_p, raw_p);

// Operation functions
static obj_p and_op_partial(raw_p x, raw_p y, raw_p z, raw_p k, raw_p l) {
    b8_t m, *mask, *next_mask;
    i64_t i, n, c, offset;

    n = (i64_t)z;
    offset = (i64_t)k;
    c = (i64_t)l;

    mask = (b8_t *)x + offset;
    next_mask = (b8_t *)y + offset;

    // both vectors are the same length
    if (c == 1) {
        for (i = 0; i < n; i++)
            mask[i] = mask[i] && next_mask[i];

        return NULL_OBJ;
    }

    // else right is scalar
    m = next_mask[0];
    for (i = 0; i < n; i++)
        mask[i] = mask[i] && m;

    return NULL_OBJ;
}

static obj_p or_op_partial(raw_p x, raw_p y, raw_p z, raw_p k, raw_p l) {
    b8_t m, *mask, *next_mask;
    i64_t i, n, c, offset;

    n = (i64_t)z;
    offset = (i64_t)k;
    c = (i64_t)l;

    mask = (b8_t *)x + offset;
    next_mask = (b8_t *)y + offset;

    // both vectors are the same length
    if (c == 1) {
        for (i = 0; i < n; i++)
            mask[i] = mask[i] || next_mask[i];

        return NULL_OBJ;
    }

    // else right is scalar
    m = next_mask[0];
    for (i = 0; i < n; i++)
        mask[i] = mask[i] || m;

    return NULL_OBJ;
}

// Actual logic operation
static obj_p logic_map(obj_p *x, i64_t n, lit_p op_name, logic_op_f op_func) {
    i64_t i, j, c, m, l, chunk;
    obj_p next, res, v;
    pool_p pool = runtime_get()->pool;
    (void)op_name;

    if (n == 0)
        return B8(B8_FALSE);

    // Evaluate first expression and validate type
    res = eval(x[0]);
    if (IS_ERR(res) || n == 1)
        return res;

    // Process remaining expressions
    for (i = 1; i < n; i++) {
        next = eval(x[i]);

        if (IS_ERR(next)) {
            drop_obj(res);
            return next;
        }

        switch (MTYPE2(res->type, next->type)) {
            case MTYPE2(-TYPE_B8, -TYPE_B8):
                op_func(&res->b8, &next->b8, (raw_p)1, (raw_p)0, (raw_p)1);
                drop_obj(next);
                break;
            case MTYPE2(TYPE_B8, TYPE_B8):
                l = ops_count(res);
                c = ops_count(next);

                if (l != c) {
                    drop_obj(res);
                    drop_obj(next);
                    return err_type(0, 0, 0);
                }

                // Perform element-wise operation using the provided function
                m = pool_split_by(pool, l, 0);
                if (m == 1) {
                    op_func(AS_B8(res), AS_B8(next), (raw_p)l, (raw_p)0, (raw_p)1);
                } else {
                    pool_prepare(pool);
                    chunk = l / m;

                    for (j = 0; j < m - 1; ++j)
                        pool_add_task(pool, op_func, 5, AS_B8(res), AS_B8(next), chunk, j * chunk, (raw_p)1);

                    pool_add_task(pool, op_func, 5, AS_B8(res), AS_B8(next), l - j * chunk, j * chunk, (raw_p)1);

                    v = pool_run(pool);

                    if (IS_ERR(v)) {
                        drop_obj(res);
                        drop_obj(next);
                        return v;
                    }

                    drop_obj(v);
                }

                drop_obj(next);

                break;

            case MTYPE2(TYPE_B8, -TYPE_B8):
            va:
                l = ops_count(res);
                m = pool_split_by(pool, l, 0);

                if (m == 1) {
                    op_func(AS_B8(res), &next->b8, (raw_p)l, (raw_p)0, (raw_p)0);
                } else {
                    pool_prepare(pool);
                    chunk = l / m;

                    for (j = 0; j < m - 1; j++)
                        pool_add_task(pool, op_func, 5, AS_B8(res), &next->b8, chunk, j * chunk, (raw_p)0);

                    pool_add_task(pool, op_func, 5, AS_B8(res), &next->b8, l - j * chunk, j * chunk, (raw_p)0);

                    v = pool_run(pool);

                    if (IS_ERR(v)) {
                        drop_obj(res);
                        drop_obj(next);
                        return v;
                    }

                    drop_obj(v);
                }

                drop_obj(next);

                break;

            case MTYPE2(-TYPE_B8, TYPE_B8):
                v = res;
                res = next;
                next = v;
                goto va;

            case MTYPE2(TYPE_PARTEDB8, TYPE_PARTEDB8):
                // Combine two parted booleans - result is TYPE_PARTEDB8
                // Each element: NULL_OBJ (no match), b8(TRUE) (all match), or B8 vector
                l = res->len;
                if (l != (i64_t)next->len) {
                    drop_obj(res);
                    drop_obj(next);
                    return err_type(0, 0, 0);
                }

                for (j = 0; j < l; j++) {
                    obj_p a = AS_LIST(res)[j];
                    obj_p b = AS_LIST(next)[j];

                    // NULL_OBJ handling: and(NULL,x)=NULL, or(NULL,x)=x
                    if (a == NULL_OBJ) {
                        if (op_func == or_op_partial && b != NULL_OBJ) {
                            AS_LIST(res)[j] = b;
                            AS_LIST(next)[j] = NULL_OBJ;
                        }
                        continue;
                    }
                    if (b == NULL_OBJ) {
                        if (op_func == and_op_partial) {
                            drop_obj(a);
                            AS_LIST(res)[j] = NULL_OBJ;
                        }
                        continue;
                    }

                    // Both b8(TRUE): and=TRUE, or=TRUE
                    if (a->type == -TYPE_B8 && b->type == -TYPE_B8) {
                        op_func(&a->b8, &b->b8, (raw_p)1, (raw_p)0, (raw_p)1);
                        if (!a->b8) {
                            drop_obj(a);
                            AS_LIST(res)[j] = NULL_OBJ;
                        }
                        continue;
                    }

                    // b8(TRUE) with B8 vector: result is vector
                    if (a->type == -TYPE_B8) {
                        op_func(AS_B8(b), &a->b8, (raw_p)b->len, (raw_p)0, (raw_p)0);
                        drop_obj(a);
                        AS_LIST(res)[j] = b;
                        AS_LIST(next)[j] = NULL_OBJ;
                        continue;
                    }
                    if (b->type == -TYPE_B8) {
                        op_func(AS_B8(a), &b->b8, (raw_p)a->len, (raw_p)0, (raw_p)0);
                        continue;
                    }

                    // Both B8 vectors: element-wise
                    op_func(AS_B8(a), AS_B8(b), (raw_p)a->len, (raw_p)0, (raw_p)1);
                }

                drop_obj(next);
                break;

            default:
                drop_obj(res);
                drop_obj(next);
                return err_type(0, 0, 0);
        }
    }

    return res;
}

obj_p ray_and(obj_p *x, i64_t n) { return logic_map(x, n, "and", and_op_partial); }

obj_p ray_or(obj_p *x, i64_t n) { return logic_map(x, n, "or", or_op_partial); }

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
                    return err_type(0, 0, 0);
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
                    return err_type(0, 0, 0);
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
            return err_type(0, 0, 0);
    }
}
