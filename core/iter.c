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

#include "heap.h"
#include "iter.h"
#include "ops.h"
#include "util.h"
#include "lambda.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "eval.h"
#include "error.h"
#include "runtime.h"
#include "pool.h"

obj_p ray_apply(obj_p *x, u64_t n) {
    u64_t i;
    obj_p f;

    if (n < 2)
        return null(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'apply': unary call with wrong arguments count");
            return unary_call(FN_ATOMIC, (unary_f)f->i64, x[0]);
        case TYPE_BINARY:
            if (n != 2)
                THROW(ERR_LENGTH, "'apply': binary call with wrong arguments count");
            return binary_call(FN_ATOMIC, (binary_f)f->i64, x[0], x[1]);
        case TYPE_VARY:
            return vary_call(FN_ATOMIC, (vary_f)f->i64, x, n);
        case TYPE_LAMBDA:
            if (n != AS_LAMBDA(f)->args->len)
                THROW(ERR_LENGTH, "'apply': lambda call with wrong arguments count");

            for (i = 0; i < n; i++)
                stack_push(clone_obj(x[i]));

            return call(f, n);
        default:
            THROW(ERR_TYPE, "'map': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_map(obj_p *x, u64_t n) {
    u64_t i, j, l;
    obj_p f, v, *b, res;

    if (n < 2)
        return LIST(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'map': unary call with wrong arguments count");
            return unary_call(FN_ATOMIC, (unary_f)f->i64, x[0]);
        case TYPE_BINARY:
            if (n != 2)
                THROW(ERR_LENGTH, "'map': binary call with wrong arguments count");
            return binary_call(FN_ATOMIC, (binary_f)f->i64, x[0], x[1]);
        case TYPE_VARY:
            return vary_call(FN_ATOMIC, (vary_f)f->i64, x, n);
        case TYPE_LAMBDA:
            if (n != AS_LAMBDA(f)->args->len)
                THROW(ERR_LENGTH, "'map': lambda call with wrong arguments count");

            l = ops_rank(x, n);
            if (l == 0xfffffffffffffffful)
                THROW(ERR_LENGTH, "'map': arguments have different lengths");

            // first item to get type of res
            for (j = 0; j < n; j++) {
                b = x + j;
                v = (IS_VECTOR(*b) || (*b)->type == TYPE_MAPGROUP) ? at_idx(*b, 0) : clone_obj(*b);
                stack_push(v);
            }

            v = call(f, n);
            if (IS_ERROR(v))
                return v;

            res = v->type < 0 ? vector(v->type, l) : vector(TYPE_LIST, l);

            ins_obj(&res, 0, v);

            for (i = 1; i < l; i++) {
                for (j = 0; j < n; j++) {
                    b = x + j;
                    v = (IS_VECTOR(*b) || (*b)->type == TYPE_MAPGROUP) ? at_idx(*b, i) : clone_obj(*b);
                    stack_push(v);
                }

                v = call(f, n);
                if (IS_ERROR(v)) {
                    res->len = i;
                    drop_obj(res);
                    return v;
                }

                ins_obj(&res, i, v);
            }

            return res;
        default:
            THROW(ERR_TYPE, "'map': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_map_left(obj_p *x, u64_t n) {
    u64_t i, j, l;
    obj_p f, v, *b, res;

    if (n < 2)
        return LIST(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'map-left': unary call with wrong arguments count");
            return unary_call(FN_ATOMIC, (unary_f)f->i64, x[0]);
        case TYPE_BINARY:
            if (n != 2)
                THROW(ERR_LENGTH, "'map-left': binary call with wrong arguments count");
            return binary_call(FN_ATOMIC, (binary_f)f->i64, x[0], x[1]);
        case TYPE_VARY:
            return vary_call(FN_ATOMIC, (vary_f)f->i64, x, n);
        case TYPE_LAMBDA:
            if (n != AS_LAMBDA(f)->args->len)
                THROW(ERR_LENGTH, "'map-left': lambda call with wrong arguments count");

            if (!IS_VECTOR(x[0])) {
                for (i = 0; i < n; i++)
                    stack_push(clone_obj(x[i]));

                return call(f, n);
            }

            l = ops_count(x[0]);

            // first item to get type of res
            stack_push(at_idx(x[0], 0));

            for (j = 1; j < n; j++) {
                b = x + j;
                v = clone_obj(*b);
                stack_push(v);
            }

            v = call(f, n);
            if (IS_ERROR(v))
                return v;

            res = v->type < 0 ? vector(v->type, l) : vector(TYPE_LIST, l);

            ins_obj(&res, 0, v);

            for (i = 1; i < l; i++) {
                stack_push(at_idx(x[0], i));
                for (j = 1; j < n; j++) {
                    b = x + j;
                    v = clone_obj(*b);
                    stack_push(v);
                }

                v = call(f, n);
                if (IS_ERROR(v)) {
                    res->len = i;
                    drop_obj(res);
                    return v;
                }

                ins_obj(&res, i, v);
            }

            return res;
        default:
            THROW(ERR_TYPE, "'map-left': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_map_right(obj_p *x, u64_t n) {
    u64_t i, j, l;
    obj_p f, v, *b, res;

    if (n < 2)
        return LIST(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'map-right': unary call with wrong arguments count");
            return unary_call(FN_ATOMIC, (unary_f)f->i64, x[0]);
        case TYPE_BINARY:
            if (n != 2)
                THROW(ERR_LENGTH, "'map-right': binary call with wrong arguments count");
            return binary_call(FN_ATOMIC, (binary_f)f->i64, x[0], x[1]);
        case TYPE_VARY:
            return vary_call(FN_ATOMIC, (vary_f)f->i64, x, n);
        case TYPE_LAMBDA:
            if (n != AS_LAMBDA(f)->args->len)
                THROW(ERR_LENGTH, "'map-right': lambda call with wrong arguments count");

            if (!IS_VECTOR(x[n - 1])) {
                for (i = 0; i < n; i++)
                    stack_push(clone_obj(x[i]));

                return call(f, n);
            }

            l = ops_count(x[n - 1]);

            // first item to get type of res
            for (j = 0; j < n - 1; j++) {
                b = x + j;
                v = clone_obj(*b);
                stack_push(v);
            }
            stack_push(at_idx(x[n - 1], 0));

            v = call(f, n);
            if (IS_ERROR(v))
                return v;

            res = v->type < 0 ? vector(v->type, l) : vector(TYPE_LIST, l);

            ins_obj(&res, 0, v);

            for (i = 1; i < l; i++) {
                for (j = 0; j < n - 1; j++) {
                    b = x + j;
                    v = clone_obj(*b);
                    stack_push(v);
                }
                stack_push(at_idx(x[n - 1], i));

                v = call(f, n);
                if (IS_ERROR(v)) {
                    res->len = i;
                    drop_obj(res);
                    return v;
                }

                ins_obj(&res, i, v);
            }

            return res;
        default:
            THROW(ERR_TYPE, "'map-right': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_fold(obj_p *x, u64_t n) {
    u64_t a, o, i, l;
    obj_p f, v, *b, x1, x2;

    if (n < 2)
        return LIST(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'fold': unary call with wrong arguments count");
            return unary_call(FN_ATOMIC, (unary_f)f->i64, x[0]);
        case TYPE_BINARY:
            l = ops_rank(x, n);
            if (l == 0xfffffffffffffffful)
                THROW(ERR_LENGTH, "'fold': arguments have different lengths");

            if (n == 1) {
                o = 1;
                b = x;
                v = at_idx(x[0], 0);
            } else if (n == 2) {
                o = 0;
                b = x + 1;
                v = clone_obj(x[0]);
            } else
                THROW(ERR_LENGTH, "'fold': binary call with wrong arguments count");

            for (i = o; i < l; i++) {
                x1 = v;
                x2 = at_idx(*b, i);
                v = binary_call(FN_ATOMIC, (binary_f)f->i64, x1, x2);
                drop_obj(x1);
                drop_obj(x2);

                if (IS_ERROR(v))
                    return v;
            }

            return v;
        case TYPE_VARY:
            return vary_call(FN_ATOMIC, (vary_f)f->i64, x, n);
        case TYPE_LAMBDA:
            l = ops_rank(x, n);
            if (l == 0xfffffffffffffffful)
                THROW(ERR_LENGTH, "'fold': arguments have different lengths");

            if (n != 1 && n != AS_LAMBDA(f)->args->len)
                THROW(ERR_LENGTH, "'fold': lambda call with wrong arguments count");

            // interpret first arg as an initial value
            if (n == 1) {
                a = 2;
                o = 1;
                b = x;
                v = at_idx(x[0], 0);
            } else if (n == 2) {
                a = 2;
                o = 0;
                b = x + 1;
                v = clone_obj(x[0]);
            } else
                THROW(ERR_LENGTH, "'fold': binary call with wrong arguments count");

            for (i = o; i < l; i++) {
                stack_push(v);
                x2 = at_idx(*b, i);
                stack_push(x2);

                // for (j = o; j < n; j++) {
                //     b = x + j;
                //     v = at_idx(*b, i);
                //     stack_push(v);
                // }

                v = call(f, a);

                if (IS_ERROR(v))
                    return v;
            }

            return v;
        default:
            THROW(ERR_TYPE, "'fold': unsupported function type: '%s", type_name(f->type));
    }
}
