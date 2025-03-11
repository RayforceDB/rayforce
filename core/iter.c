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
#include "util.h"
#include "lambda.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "eval.h"
#include "error.h"
#include "runtime.h"
#include "pool.h"

obj_p map_unary(obj_p f, obj_p x) {
    u64_t i, l, n;
    obj_p res, item, a, *v, parts;
    pool_p pool;
    unary_f fn;

    pool = pool_get();
    fn = (unary_f)f->i64;

    switch (x->type) {
        case TYPE_LIST:
            l = ops_count(x);

            if (l == 0)
                return NULL_OBJ;

            v = AS_LIST(x);
            n = pool_split_by(pool, l, 0);

            if (n > 1) {
                pool_prepare(pool);

                if (f->attrs & FN_ATOMIC) {
                    for (i = 0; i < l; i++)
                        pool_add_task(pool, (raw_p)map_unary, 3, f, v[i]);
                } else {
                    for (i = 0; i < l; i++)
                        pool_add_task(pool, (raw_p)f, 1, v[i]);
                }

                parts = pool_run(pool);

                return parts;
            }

            item = (f->attrs & FN_ATOMIC) ? map_unary(f, v[0]) : fn(v[0]);

            if (IS_ERROR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                item = (f->attrs & FN_ATOMIC) ? map_unary(f, v[i]) : fn(v[i]);

                if (IS_ERROR(item)) {
                    res->len = i;
                    drop_obj(res);
                    return item;
                }

                ins_obj(&res, i, item);
            }

            return res;

        case TYPE_MAPLIST:
            l = ops_count(x);
            if (l == 0)
                return NULL_OBJ;

            a = at_idx(x, 0);
            item = (f->attrs & FN_ATOMIC) ? map_unary(f, a) : fn(a);
            drop_obj(a);

            if (IS_ERROR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : vector(TYPE_LIST, l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                a = at_idx(x, i);
                item = (f->attrs & FN_ATOMIC) ? map_unary(f, a) : fn(a);
                drop_obj(a);

                if (IS_ERROR(item)) {
                    res->len = i;
                    drop_obj(res);
                    return item;
                }

                ins_obj(&res, i, item);
            }

            return res;

        default:
            fn = (unary_f)f->i64;
            return fn(x);
    }
}

obj_p map_binary_left(obj_p f, obj_p x, obj_p y) {
    u64_t i, l;
    binary_f fn;
    obj_p res, item, a;

    switch (x->type) {
        case TYPE_LIST:
            l = ops_count(x);
            a = AS_LIST(x)[0];
            item = map_binary_left(f, a, y);

            if (IS_ERROR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                a = AS_LIST(x)[i];
                item = map_binary_left(f, a, y);

                if (IS_ERROR(item)) {
                    res->len = i;
                    drop_obj(res);
                    return item;
                }

                ins_obj(&res, i, item);
            }

            return res;

        case TYPE_MAPLIST:
            l = ops_count(x);
            a = at_idx(x, 0);
            item = map_binary_left(f, a, y);
            drop_obj(a);

            if (item->type == TYPE_ERROR)
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                a = at_idx(x, i);
                item = map_binary_left(f, a, y);
                drop_obj(a);

                if (IS_ERROR(item)) {
                    res->len = i;
                    drop_obj(res);
                    return item;
                }

                ins_obj(&res, i, item);
            }

            return res;

        default:
            fn = (binary_f)f->i64;
            return fn(x, y);
    }
}

obj_p map_binary_right(obj_p f, obj_p x, obj_p y) {
    u64_t i, l;
    binary_f fn;
    obj_p res, item, b;

    switch (y->type) {
        case TYPE_LIST:
            l = ops_count(y);
            b = AS_LIST(y)[0];
            item = map_binary_right(f, x, b);

            if (IS_ERROR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                b = AS_LIST(y)[i];
                item = map_binary_right(f, x, b);

                if (IS_ERROR(item)) {
                    res->len = i;
                    drop_obj(res);
                    return item;
                }

                ins_obj(&res, i, item);
            }

            return res;

        case TYPE_MAPLIST:
            l = ops_count(y);
            b = at_idx(y, 0);
            item = map_binary_right(f, x, b);
            drop_obj(b);

            if (IS_ERROR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                b = at_idx(y, i);
                item = map_binary_right(f, x, b);
                drop_obj(b);

                if (item->type == TYPE_ERROR) {
                    res->len = i;
                    drop_obj(res);
                    return item;
                }

                ins_obj(&res, i, item);
            }

            return res;

        default:
            fn = (binary_f)f->i64;
            return fn(x, y);
    }
}

obj_p map_binary(obj_p f, obj_p x, obj_p y) {
    u64_t i, l;
    binary_f fn;
    obj_p res, item, a, b;
    i8_t xt, yt;

    if (!x || !y)
        THROW(ERR_TYPE, "binary: null argument");

    xt = x->type;
    yt = y->type;
    if (((xt == TYPE_LIST || xt == TYPE_MAPLIST) && IS_VECTOR(y)) ||
        ((yt == TYPE_LIST || yt == TYPE_MAPLIST) && IS_VECTOR(x))) {
        l = ops_count(x);

        if (l != ops_count(y))
            return error_str(ERR_LENGTH, "binary: vectors must be of the same length");

        if (l == 0) {
            fn = (binary_f)f->i64;
            return fn(x, y);
        }

        a = xt == TYPE_LIST ? AS_LIST(x)[0] : at_idx(x, 0);
        b = yt == TYPE_LIST ? AS_LIST(y)[0] : at_idx(y, 0);
        item = map_binary(f, a, b);

        if (xt != TYPE_LIST)
            drop_obj(a);
        if (yt != TYPE_LIST)
            drop_obj(b);

        if (IS_ERROR(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : LIST(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++) {
            a = xt == TYPE_LIST ? AS_LIST(x)[i] : at_idx(x, i);
            b = yt == TYPE_LIST ? AS_LIST(y)[i] : at_idx(y, i);
            item = map_binary(f, a, b);

            if (xt != TYPE_LIST)
                drop_obj(a);
            if (yt != TYPE_LIST)
                drop_obj(b);

            if (IS_ERROR(item)) {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    } else if (xt == TYPE_LIST || xt == TYPE_MAPLIST) {
        l = ops_count(x);
        if (l == 0) {
            fn = (binary_f)f->i64;
            return fn(x, y);
        }

        a = xt == TYPE_LIST ? AS_LIST(x)[0] : at_idx(x, 0);
        item = map_binary(f, a, y);
        if (xt != TYPE_LIST)
            drop_obj(a);

        if (IS_ERROR(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : LIST(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++) {
            a = xt == TYPE_LIST ? AS_LIST(x)[i] : at_idx(x, i);
            item = map_binary(f, a, y);
            if (xt != TYPE_LIST)
                drop_obj(a);

            if (IS_ERROR(item)) {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    } else if (yt == TYPE_LIST || yt == TYPE_MAPLIST) {
        l = ops_count(y);
        if (l == 0) {
            fn = (binary_f)f->i64;
            return fn(x, y);
        }

        b = yt == TYPE_LIST ? AS_LIST(y)[0] : at_idx(y, 0);
        item = map_binary(f, x, b);
        if (yt != TYPE_LIST)
            drop_obj(b);

        if (IS_ERROR(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : LIST(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++) {
            b = yt == TYPE_LIST ? AS_LIST(y)[i] : at_idx(y, i);
            item = map_binary(f, x, b);
            if (yt != TYPE_LIST)
                drop_obj(b);

            if (IS_ERROR(item)) {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    }

    fn = (binary_f)f->i64;
    return fn(x, y);
}

obj_p map_vary(obj_p f, obj_p *x, u64_t n) {
    u64_t i, j, l;
    vary_f fn;
    obj_p v, res;

    if (n == 0)
        return NULL_OBJ;

    l = ops_rank(x, n);
    if (l == 0xfffffffffffffffful)
        THROW(ERR_LENGTH, "vary: arguments have different lengths");

    for (j = 0; j < n; j++)
        stack_push(at_idx(x[j], 0));

    fn = (vary_f)f->i64;
    v = fn(x + n, n);

    if (IS_ERROR(v))
        res = v;

    res = v->type < 0 ? vector(v->type, l) : LIST(l);

    ins_obj(&res, 0, v);

    for (i = 1; i < l; i++) {
        for (j = 0; j < n; j++)
            stack_push(at_idx(x[j], i));

        v = (f->attrs & FN_ATOMIC) ? map_vary(f, x + n, n) : fn(x + n, n);

        // cleanup stack
        for (j = 0; j < n; j++)
            drop_obj(stack_pop());

        if (IS_ERROR(v)) {
            res->len = i;
            drop_obj(res);
            return v;
        }

        ins_obj(&res, i, v);
    }

    return res;
}

obj_p map_lambda_partial(obj_p f, obj_p *lst, u64_t n, u64_t arg) {
    u64_t i;
    obj_p res;

    for (i = 0; i < n; i++)
        stack_push(at_idx(lst[i], arg));

    res = call(f, n);

    for (i = 0; i < n; i++)
        drop_obj(stack_pop());

    return res;
}

obj_p map_lambda(obj_p f, obj_p *x, u64_t n) {
    u64_t i, j, l, executors;
    obj_p v, res;
    pool_p pool;

    l = ops_rank(x, n);

    if (n == 0 || l == 0 || l == 0xfffffffffffffffful)
        return NULL_OBJ;

    pool = pool_get();
    executors = pool_split_by(pool, l, 0);

    if (executors > 1) {
        pool_prepare(pool);

        for (j = 0; j < l; j++)
            pool_add_task(pool, (raw_p)map_lambda_partial, 4, f, x, n, j);

        return pool_run(pool);
    }

    for (j = 0; j < n; j++)
        stack_push(at_idx(x[j], 0));

    v = (f->attrs & FN_ATOMIC) ? map_lambda(f, x, n) : call(f, n);

    for (j = 0; j < n; j++)
        drop_obj(stack_pop());

    if (IS_ERROR(v))
        return v;

    res = v->type < 0 ? vector(v->type, l) : LIST(l);

    ins_obj(&res, 0, v);

    for (i = 1; i < l; i++) {
        for (j = 0; j < n; j++)
            stack_push(at_idx(x[j], i));

        v = (f->attrs & FN_ATOMIC) ? map_lambda(f, x, n) : call(f, n);

        for (j = 0; j < n; j++)
            drop_obj(stack_pop());

        if (IS_ERROR(v)) {
            res->len = i;
            drop_obj(res);
            return v;
        }

        ins_obj(&res, i, v);
    }

    return res;
}

obj_p ray_map(obj_p *x, u64_t n) {
    u64_t l;
    obj_p f;

    if (n < 2)
        return LIST(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'map': unary call with wrong arguments count");
            return map_unary(f, x[0]);
        case TYPE_BINARY:
            if (n != 2)
                THROW(ERR_LENGTH, "'map': binary call with wrong arguments count");
            return map_binary(f, x[0], x[1]);
        case TYPE_VARY:
            return map_vary(f, x, n);
        case TYPE_LAMBDA:
            if (n != AS_LAMBDA(f)->args->len)
                THROW(ERR_LENGTH, "'map': lambda call with wrong arguments count");

            l = ops_rank(x, n);
            if (l == 0xfffffffffffffffful)
                THROW(ERR_LENGTH, "'map': arguments have different lengths");

            if (l < 1)
                return vector(x[0]->type, 0);

            return map_lambda(f, x, n);

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
            return map_unary(f, x[0]);
        case TYPE_BINARY:
            if (n != 2)
                THROW(ERR_LENGTH, "'map-left': binary call with wrong arguments count");
            return map_binary_left(f, x[0], x[1]);
        case TYPE_VARY:
            return map_vary(f, x, n);
        case TYPE_LAMBDA:
            if (n != AS_LAMBDA(f)->args->len)
                THROW(ERR_LENGTH, "'map-left': lambda call with wrong arguments count");

            if (!IS_VECTOR(x[0])) {
                for (i = 0; i < n; i++)
                    stack_push(clone_obj(x[i]));

                return call(f, n);
            }

            l = ops_count(x[0]);
            if (l < 1)
                return vector(x[0]->type, 0);

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
            return map_unary(f, x[0]);
        case TYPE_BINARY:
            if (n != 2)
                THROW(ERR_LENGTH, "'map-right': binary call with wrong arguments count");
            return map_binary_right(f, x[0], x[1]);
        case TYPE_VARY:
            return map_vary(f, x, n);
        case TYPE_LAMBDA:
            if (n != AS_LAMBDA(f)->args->len)
                THROW(ERR_LENGTH, "'map-right': lambda call with wrong arguments count");

            if (!IS_VECTOR(x[n - 1])) {
                for (i = 0; i < n; i++)
                    stack_push(clone_obj(x[i]));

                return call(f, n);
            }

            l = ops_count(x[n - 1]);
            if (l < 1)
                return vector(x[n - 1]->type, 0);

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
            return unary_call(f, x[0]);
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
                v = binary_call(f, x1, x2);
                drop_obj(x1);
                drop_obj(x2);

                if (IS_ERROR(v))
                    return v;
            }

            return v;
        case TYPE_VARY:
            return vary_call(f, x, n);
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

                // cleanup stack
                drop_obj(stack_pop());
                drop_obj(stack_pop());

                if (IS_ERROR(v))
                    return v;
            }

            return v;
        default:
            THROW(ERR_TYPE, "'fold': unsupported function type: '%s", type_name(f->type));
    }
}
