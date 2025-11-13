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

#include "iter.h"
#include "lambda.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "eval.h"
#include "error.h"
#include "pool.h"

obj_p map_unary_fn(unary_f fn, i64_t attrs, obj_p x) {
    i64_t i, l, n;
    obj_p res, item, a, *v, parts;
    pool_p pool;

    pool = pool_get();

    switch (x->type) {
        case TYPE_LIST:
            l = ops_count(x);

            if (l == 0)
                return NULL_OBJ;

            v = AS_LIST(x);
            n = pool_split_by(pool, l, 0);

            if (n > 1) {
                pool_prepare(pool);

                if (attrs & FN_ATOMIC) {
                    for (i = 0; i < l; i++)
                        pool_add_task(pool, (raw_p)map_unary_fn, 3, fn, attrs, v[i]);
                } else {
                    for (i = 0; i < l; i++)
                        pool_add_task(pool, (raw_p)fn, 1, v[i]);
                }

                parts = pool_run(pool);
                return unify_list(&parts);
            }

            item = (attrs & FN_ATOMIC) ? map_unary_fn(fn, attrs, v[0]) : fn(v[0]);

            if (IS_ERR(item))
                return item;

            res = (item->type < 0) ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                item = (attrs & FN_ATOMIC) ? map_unary_fn(fn, attrs, v[i]) : fn(v[i]);

                if (IS_ERR(item)) {
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
            item = (attrs & FN_ATOMIC) ? map_unary_fn(fn, attrs, a) : fn(a);
            drop_obj(a);

            if (IS_ERR(item))
                return item;

            res = (item->type < 0) ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                a = at_idx(x, i);
                item = (attrs & FN_ATOMIC) ? map_unary_fn(fn, attrs, a) : fn(a);
                drop_obj(a);

                if (IS_ERR(item)) {
                    res->len = i;
                    drop_obj(res);
                    return item;
                }

                ins_obj(&res, i, item);
            }

            return res;

        default:
            return fn(x);
    }
}

obj_p map_unary(obj_p f, obj_p x) { return map_unary_fn((unary_f)f->i64, f->attrs, x); }

obj_p map_binary_left_fn(binary_f fn, i64_t attrs, obj_p x, obj_p y) {
    i64_t i, l;
    obj_p res, item, a;

    switch (x->type) {
        case TYPE_C8:
        case TYPE_U8:
        case TYPE_B8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
        case TYPE_GUID:
        case TYPE_LIST:
        case TYPE_MAPLIST:
            l = ops_count(x);

            if (l == 0)
                return null(x->type);

            a = at_idx(x, 0);
            item = map_binary_left_fn(fn, attrs, a, y);
            drop_obj(a);

            if (IS_ERR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);
            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                a = at_idx(x, i);
                item = map_binary_left_fn(fn, attrs, a, y);
                drop_obj(a);

                if (IS_ERR(item)) {
                    res->len = i;
                    drop_obj(res);
                    return item;
                }

                ins_obj(&res, i, item);
            }
            return res;
        default:
            return fn(x, y);
    }
}

obj_p map_binary_left(obj_p f, obj_p x, obj_p y) { return map_binary_left_fn((binary_f)f->i64, f->attrs, x, y); }

obj_p map_binary_right_fn(binary_f fn, i64_t attrs, obj_p x, obj_p y) {
    i64_t i, l;
    obj_p res, item, a;

    switch (y->type) {
        case TYPE_C8:
        case TYPE_U8:
        case TYPE_B8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
        case TYPE_GUID:
        case TYPE_LIST:
        case TYPE_MAPLIST:
            l = ops_count(y);

            if (l == 0)
                return null(y->type);

            a = at_idx(y, 0);
            item = map_binary_right_fn(fn, attrs, x, a);
            drop_obj(a);

            if (IS_ERR(item))
                return item;

            res = item->type < 0 ? vector(item->type, l) : LIST(l);

            ins_obj(&res, 0, item);

            for (i = 1; i < l; i++) {
                a = at_idx(y, i);
                item = map_binary_right_fn(fn, attrs, x, a);
                drop_obj(a);

                if (IS_ERR(item)) {
                    res->len = i;
                    drop_obj(res);
                    return item;
                }

                ins_obj(&res, i, item);
            }

            return res;

        default:
            return fn(x, y);
    }
}

obj_p map_binary_right(obj_p f, obj_p x, obj_p y) { return map_binary_right_fn((binary_f)f->i64, f->attrs, x, y); }

obj_p map_binary_fn(binary_f fn, i64_t attrs, obj_p x, obj_p y) {
    i64_t i, l;
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

        if (l == 0)
            return fn(x, y);

        a = xt == TYPE_LIST ? AS_LIST(x)[0] : at_idx(x, 0);
        b = yt == TYPE_LIST ? AS_LIST(y)[0] : at_idx(y, 0);
        item = map_binary_fn(fn, attrs, a, b);

        if (xt != TYPE_LIST)
            drop_obj(a);
        if (yt != TYPE_LIST)
            drop_obj(b);

        if (IS_ERR(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : LIST(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++) {
            a = xt == TYPE_LIST ? AS_LIST(x)[i] : at_idx(x, i);
            b = yt == TYPE_LIST ? AS_LIST(y)[i] : at_idx(y, i);
            item = map_binary_fn(fn, attrs, a, b);

            if (xt != TYPE_LIST)
                drop_obj(a);
            if (yt != TYPE_LIST)
                drop_obj(b);

            if (IS_ERR(item)) {
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
            return fn(x, y);
        }

        a = xt == TYPE_LIST ? AS_LIST(x)[0] : at_idx(x, 0);
        item = map_binary_fn(fn, attrs, a, y);
        if (xt != TYPE_LIST)
            drop_obj(a);

        if (IS_ERR(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : LIST(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++) {
            a = xt == TYPE_LIST ? AS_LIST(x)[i] : at_idx(x, i);
            item = map_binary_fn(fn, attrs, a, y);
            if (xt != TYPE_LIST)
                drop_obj(a);

            if (IS_ERR(item)) {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    } else if (yt == TYPE_LIST || yt == TYPE_MAPLIST) {
        l = ops_count(y);
        if (l == 0)
            return fn(x, y);

        b = yt == TYPE_LIST ? AS_LIST(y)[0] : at_idx(y, 0);
        item = map_binary_fn(fn, attrs, x, b);
        if (yt != TYPE_LIST)
            drop_obj(b);

        if (IS_ERR(item))
            return item;

        res = item->type < 0 ? vector(item->type, l) : LIST(l);

        ins_obj(&res, 0, item);

        for (i = 1; i < l; i++) {
            b = yt == TYPE_LIST ? AS_LIST(y)[i] : at_idx(y, i);
            item = map_binary_fn(fn, attrs, x, b);
            if (yt != TYPE_LIST)
                drop_obj(b);

            if (IS_ERR(item)) {
                res->len = i;
                drop_obj(res);
                return item;
            }

            ins_obj(&res, i, item);
        }

        return res;
    }

    return fn(x, y);
}

obj_p map_binary(obj_p f, obj_p x, obj_p y) { return map_binary_fn((binary_f)f->i64, f->attrs, x, y); }

obj_p map_vary_fn(vary_f fn, i64_t attrs, obj_p *x, i64_t n) {
    i64_t i, j, l;
    obj_p v, res;

    if (n == 0)
        return NULL_OBJ;

    l = ops_rank(x, n);
    if (l == NULL_I64)
        THROW(ERR_LENGTH, "vary: arguments have different lengths");

    for (j = 0; j < n; j++)
        stack_push(at_idx(x[j], 0));

    v = fn(x + n, n);

    if (IS_ERR(v))
        res = v;

    res = v->type < 0 ? vector(v->type, l) : LIST(l);

    ins_obj(&res, 0, v);

    for (i = 1; i < l; i++) {
        for (j = 0; j < n; j++)
            stack_push(at_idx(x[j], i));

        v = (attrs & FN_ATOMIC) ? map_vary_fn(fn, attrs, x + n, n) : fn(x + n, n);

        // cleanup stack
        for (j = 0; j < n; j++)
            drop_obj(stack_pop());

        if (IS_ERR(v)) {
            res->len = i;
            drop_obj(res);
            return v;
        }

        ins_obj(&res, i, v);
    }

    return res;
}

obj_p map_vary(obj_p f, obj_p *x, i64_t n) { return map_vary_fn((vary_f)f->i64, f->attrs, x, n); }

obj_p map_lambda_partial(obj_p f, obj_p *lst, i64_t n, i64_t arg) {
    i64_t i;
    obj_p res;

    for (i = 0; i < n; i++)
        stack_push(at_idx(lst[i], arg));

    res = call(f, n);

    for (i = 0; i < n; i++)
        drop_obj(stack_pop());

    return res;
}

obj_p map_lambda(obj_p f, obj_p *x, i64_t n) {
    i64_t i, j, l, executors;
    obj_p v, res;
    pool_p pool;

    l = ops_rank(x, n);

    if (n == 0 || l == 0 || l == NULL_I64)
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

    if (IS_ERR(v))
        return v;

    res = v->type < 0 ? vector(v->type, l) : LIST(l);

    ins_obj(&res, 0, v);

    for (i = 1; i < l; i++) {
        for (j = 0; j < n; j++)
            stack_push(at_idx(x[j], i));

        v = (f->attrs & FN_ATOMIC) ? map_lambda(f, x, n) : call(f, n);

        for (j = 0; j < n; j++)
            drop_obj(stack_pop());

        if (IS_ERR(v)) {
            res->len = i;
            drop_obj(res);
            return v;
        }

        ins_obj(&res, i, v);
    }

    return res;
}

obj_p ray_map(obj_p *x, i64_t n) {
    i64_t l;
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
            if (l == NULL_I64)
                THROW(ERR_LENGTH, "'map': arguments have different lengths");

            if (l < 1)
                return vector(x[0]->type, 0);

            return map_lambda(f, x, n);

        default:
            THROW(ERR_TYPE, "'map': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_map_left(obj_p *x, i64_t n) {
    i64_t i, j, l;
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

                res = call(f, n);
                for (i = 0; i < n; i++)
                    drop_obj(stack_pop());
                return res;
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
            for (i = 0; i < n; i++)
                drop_obj(stack_pop());

            if (IS_ERR(v))
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
                for (j = 0; j < n; j++)
                    drop_obj(stack_pop());

                if (IS_ERR(v)) {
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

obj_p ray_map_right(obj_p *x, i64_t n) {
    i64_t i, j, l;
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

                res = call(f, n);
                for (i = 0; i < n; i++)
                    drop_obj(stack_pop());
                return res;
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
            for (i = 0; i < n; i++)
                drop_obj(stack_pop());

            if (IS_ERR(v))
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
                for (j = 0; j < n; j++)
                    drop_obj(stack_pop());

                if (IS_ERR(v)) {
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

obj_p ray_fold(obj_p *x, i64_t n) {
    i64_t i, j, l;
    obj_p f, v, x1, x2;
    i8_t xt, yt;

    if (n < 2)
        return LIST(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'fold': unary call with wrong arguments count");
            return map_unary(f, x[0]);
        case TYPE_BINARY:
            if (n < 2)
                THROW(ERR_LENGTH, "'fold': binary call with wrong arguments count");

            xt = x[0]->type;
            yt = x[1]->type;
            if (((xt == TYPE_LIST || xt == TYPE_MAPLIST) && IS_VECTOR(x[1])) ||
                ((yt == TYPE_LIST || yt == TYPE_MAPLIST) && IS_VECTOR(x[0]))) {
                l = ops_count(x[0]);

                if (l != ops_count(x[1]))
                    return error_str(ERR_LENGTH, "'fold': vectors must be of the same length");

                if (l == 0)
                    return LIST(0);

                x1 = xt == TYPE_LIST ? AS_LIST(x[0])[0] : at_idx(x[0], 0);
                x2 = yt == TYPE_LIST ? AS_LIST(x[1])[0] : at_idx(x[1], 0);
                v = binary_call(f, x1, x2);

                if (xt != TYPE_LIST)
                    drop_obj(x1);
                if (yt != TYPE_LIST)
                    drop_obj(x2);

                if (IS_ERR(v))
                    return v;

                for (i = 1; i < l; i++) {
                    x1 = xt == TYPE_LIST ? AS_LIST(x[0])[i] : at_idx(x[0], i);
                    x2 = yt == TYPE_LIST ? AS_LIST(x[1])[i] : at_idx(x[1], i);
                    v = binary_call(f, v, x2);  // Use accumulated value as first argument

                    if (xt != TYPE_LIST)
                        drop_obj(x1);
                    if (yt != TYPE_LIST)
                        drop_obj(x2);

                    if (IS_ERR(v))
                        return v;
                }

                return v;
            } else if (xt == TYPE_LIST || xt == TYPE_MAPLIST) {
                l = ops_count(x[0]);
                if (l == 0)
                    return clone_obj(x[1]);

                x1 = xt == TYPE_LIST ? AS_LIST(x[0])[0] : at_idx(x[0], 0);
                v = binary_call(f, x1, x[1]);
                if (xt != TYPE_LIST)
                    drop_obj(x1);

                if (IS_ERR(v))
                    return v;

                for (i = 1; i < l; i++) {
                    x1 = xt == TYPE_LIST ? AS_LIST(x[0])[i] : at_idx(x[0], i);
                    v = binary_call(f, v, x1);  // Use accumulated value as first argument
                    if (xt != TYPE_LIST)
                        drop_obj(x1);

                    if (IS_ERR(v))
                        return v;
                }

                return v;
            } else if (yt == TYPE_LIST || yt == TYPE_MAPLIST) {
                l = ops_count(x[1]);
                if (l == 0)
                    return clone_obj(x[0]);

                x2 = yt == TYPE_LIST ? AS_LIST(x[1])[0] : at_idx(x[1], 0);
                v = binary_call(f, x[0], x2);
                if (yt != TYPE_LIST)
                    drop_obj(x2);

                if (IS_ERR(v))
                    return v;

                for (i = 1; i < l; i++) {
                    x2 = yt == TYPE_LIST ? AS_LIST(x[1])[i] : at_idx(x[1], i);
                    v = binary_call(f, v, x2);  // Use accumulated value as first argument
                    if (yt != TYPE_LIST)
                        drop_obj(x2);

                    if (IS_ERR(v))
                        return v;
                }

                return v;
            }

            return binary_call(f, x[0], x[1]);

        case TYPE_VARY:
            if (n == 0)
                return NULL_OBJ;

            l = ops_rank(x, n);
            if (l == NULL_I64)
                THROW(ERR_LENGTH, "'fold': arguments have different lengths");

            for (i = 0; i < n; i++)
                stack_push(at_idx(x[i], 0));

            v = vary_call(f, x, n);

            // cleanup stack
            for (i = 0; i < n; i++)
                drop_obj(stack_pop());

            if (IS_ERR(v))
                return v;

            for (i = 1; i < l; i++) {
                for (j = 0; j < n; j++)
                    stack_push(at_idx(x[j], i));

                v = vary_call(f, x, n);

                // cleanup stack
                for (j = 0; j < n; j++)
                    drop_obj(stack_pop());

                if (IS_ERR(v))
                    return v;
            }

            return v;
        case TYPE_LAMBDA:
            l = ops_rank(x, n);
            if (l == NULL_I64)
                THROW(ERR_LENGTH, "'fold': arguments have different lengths");

            if (n != 1 && n != AS_LAMBDA(f)->args->len)
                THROW(ERR_LENGTH, "'fold': lambda call with wrong arguments count");

            if (n == 1) {
                l = ops_count(x[0]);
                if (l == 0)
                    return LIST(0);

                v = at_idx(x[0], 0);

                for (i = 1; i < l; i++) {
                    stack_push(clone_obj(v));
                    x2 = at_idx(x[0], i);
                    stack_push(x2);

                    v = call(f, 2);

                    drop_obj(stack_pop());
                    drop_obj(stack_pop());

                    if (IS_ERR(v))
                        return v;
                }

                return v;
            } else if (n == 2) {
                l = ops_count(x[0]);
                if (l == 0)
                    return LIST(0);

                v = at_idx(x[0], 0);

                for (i = 1; i < l; i++) {
                    stack_push(clone_obj(v));
                    x2 = at_idx(x[1], i);
                    stack_push(x2);

                    v = call(f, 2);

                    drop_obj(stack_pop());
                    drop_obj(stack_pop());

                    if (IS_ERR(v))
                        return v;
                }

                return v;
            } else
                THROW(ERR_LENGTH, "'fold': binary call with wrong arguments count");
        default:
            THROW(ERR_TYPE, "'fold': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_fold_left(obj_p *x, i64_t n) {
    i64_t i, j, l;
    obj_p f, v, x1, x2;

    if (n < 2)
        return LIST(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'fold-left': unary call with wrong arguments count");
            return unary_call(f, x[0]);
        case TYPE_BINARY:
            if (n < 2)
                THROW(ERR_LENGTH, "'fold-left': binary call with wrong arguments count");

            l = ops_count(x[0]);
            if (l == 0)
                return clone_obj(x[1]);

            v = clone_obj(x[1]);  // Use rightmost argument as initial value
            for (i = 0; i < l; i++) {
                // Push current element from the leftmost argument first
                x1 = at_idx(x[0], i);
                stack_push(x1);
                // Push result of previous iteration as second argument
                x2 = clone_obj(v);
                stack_push(x2);
                // Push all other arguments in between
                for (j = 1; j < n - 1; j++) {
                    stack_push(clone_obj(x[j]));
                }

                v = call(f, n);

                // Cleanup stack
                for (j = 0; j < n; j++) {
                    drop_obj(stack_pop());
                }

                if (IS_ERR(v))
                    return v;
            }

            return v;
        case TYPE_VARY:
            return vary_call(f, x, n);
        case TYPE_LAMBDA:
            if (n < 2 || AS_LAMBDA(f)->args->len != n)
                THROW(ERR_LENGTH, "'fold-left': lambda call with wrong arguments count");

            l = ops_count(x[0]);
            if (l == 0)
                return clone_obj(x[1]);

            v = clone_obj(x[1]);  // Use rightmost argument as initial value
            for (i = 0; i < l; i++) {
                // Push current element from the leftmost argument first
                x1 = at_idx(x[0], i);
                stack_push(x1);
                // Push result of previous iteration as second argument
                x2 = clone_obj(v);
                stack_push(x2);
                // Push all other arguments in between
                for (j = 1; j < n - 1; j++) {
                    stack_push(clone_obj(x[j]));
                }

                v = call(f, n);

                // Cleanup stack
                for (j = 0; j < n; j++) {
                    drop_obj(stack_pop());
                }

                if (IS_ERR(v))
                    return v;
            }

            return v;
        default:
            THROW(ERR_TYPE, "'fold-left': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_fold_right(obj_p *x, i64_t n) {
    i64_t i, j, l;
    obj_p f, v, x1, x2;

    if (n < 2)
        return LIST(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'fold-right': unary call with wrong arguments count");
            return unary_call(f, x[0]);
        case TYPE_BINARY:
            if (n < 2)
                THROW(ERR_LENGTH, "'fold-right': binary call with wrong arguments count");

            l = ops_count(x[n - 1]);  // Last argument is the one we iterate over
            if (l == 0)
                return clone_obj(x[0]);

            v = clone_obj(x[0]);
            for (i = 0; i < l; i++) {
                x1 = at_idx(x[n - 1], i);
                x2 = v;
                v = binary_call(f, x1, x2);
                drop_obj(x1);
                drop_obj(x2);

                if (IS_ERR(v))
                    return v;
            }

            return v;
        case TYPE_VARY:
            return vary_call(f, x, n);
        case TYPE_LAMBDA:
            if (n < 2 || AS_LAMBDA(f)->args->len != n)
                THROW(ERR_LENGTH, "'fold-right': lambda call with wrong arguments count");

            l = ops_count(x[n - 1]);  // Last argument is the one we iterate over
            if (l == 0)
                return clone_obj(x[0]);

            v = clone_obj(x[0]);
            for (i = 0; i < l; i++) {
                // Push all arguments except the last one (which we iterate over)
                for (j = 0; j < n - 1; j++) {
                    if (j == 0) {
                        stack_push(v);  // Push accumulator
                    } else {
                        x1 = at_idx(x[j], i);
                        stack_push(x1);
                    }
                }
                // Push current element from the last argument
                x1 = at_idx(x[n - 1], i);
                stack_push(x1);

                v = call(f, n);

                // Cleanup stack
                for (j = 0; j < n; j++) {
                    drop_obj(stack_pop());
                }

                if (IS_ERR(v))
                    return v;
            }

            return v;
        default:
            THROW(ERR_TYPE, "'fold-right': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_scan(obj_p *x, i64_t n) {
    i64_t i, j, l;
    obj_p f, v, x1, x2, res;
    i8_t xt, yt;

    if (n < 2)
        return LIST(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'scan': unary call with wrong arguments count");
            return map_unary(f, x[0]);
        case TYPE_BINARY:
            if (n < 2)
                THROW(ERR_LENGTH, "'scan': binary call with wrong arguments count");

            xt = x[0]->type;
            yt = x[1]->type;
            if (((xt == TYPE_LIST || xt == TYPE_MAPLIST) && IS_VECTOR(x[1])) ||
                ((yt == TYPE_LIST || yt == TYPE_MAPLIST) && IS_VECTOR(x[0]))) {
                l = ops_count(x[0]);

                if (l != ops_count(x[1]))
                    return error_str(ERR_LENGTH, "'scan': vectors must be of the same length");

                if (l == 0)
                    return LIST(0);

                x1 = xt == TYPE_LIST ? AS_LIST(x[0])[0] : at_idx(x[0], 0);
                x2 = yt == TYPE_LIST ? AS_LIST(x[1])[0] : at_idx(x[1], 0);
                v = binary_call(f, x1, x2);

                if (xt != TYPE_LIST)
                    drop_obj(x1);
                if (yt != TYPE_LIST)
                    drop_obj(x2);

                if (IS_ERR(v))
                    return v;

                res = LIST(l);
                ins_obj(&res, 0, v);

                for (i = 1; i < l; i++) {
                    x1 = xt == TYPE_LIST ? AS_LIST(x[0])[i] : at_idx(x[0], i);
                    x2 = yt == TYPE_LIST ? AS_LIST(x[1])[i] : at_idx(x[1], i);
                    v = binary_call(f, x1, x2);

                    if (xt != TYPE_LIST)
                        drop_obj(x1);
                    if (yt != TYPE_LIST)
                        drop_obj(x2);

                    if (IS_ERR(v)) {
                        res->len = i;
                        drop_obj(res);
                        return v;
                    }

                    v = clone_obj(v);  // Clone result before storing
                    ins_obj(&res, i, v);
                    drop_obj(v);  // Drop the original result
                }

                return res;
            } else if (xt == TYPE_LIST || xt == TYPE_MAPLIST) {
                l = ops_count(x[0]);
                if (l == 0)
                    return LIST(0);

                x1 = xt == TYPE_LIST ? AS_LIST(x[0])[0] : at_idx(x[0], 0);
                v = binary_call(f, x1, x[1]);
                if (xt != TYPE_LIST)
                    drop_obj(x1);

                if (IS_ERR(v))
                    return v;

                res = LIST(l);
                ins_obj(&res, 0, v);

                for (i = 1; i < l; i++) {
                    x1 = xt == TYPE_LIST ? AS_LIST(x[0])[i] : at_idx(x[0], i);
                    v = binary_call(f, x1, v);
                    if (xt != TYPE_LIST)
                        drop_obj(x1);

                    if (IS_ERR(v)) {
                        res->len = i;
                        drop_obj(res);
                        return v;
                    }

                    v = clone_obj(v);  // Clone result before storing
                    ins_obj(&res, i, v);
                    drop_obj(v);  // Drop the original result
                }

                return res;
            } else if (yt == TYPE_LIST || yt == TYPE_MAPLIST) {
                l = ops_count(x[1]);
                if (l == 0)
                    return LIST(0);

                x2 = yt == TYPE_LIST ? AS_LIST(x[1])[0] : at_idx(x[1], 0);
                v = binary_call(f, x[0], x2);
                if (yt != TYPE_LIST)
                    drop_obj(x2);

                if (IS_ERR(v))
                    return v;

                res = LIST(l);
                ins_obj(&res, 0, v);

                for (i = 1; i < l; i++) {
                    x2 = yt == TYPE_LIST ? AS_LIST(x[1])[i] : at_idx(x[1], i);
                    v = binary_call(f, v, x2);
                    if (yt != TYPE_LIST)
                        drop_obj(x2);

                    if (IS_ERR(v)) {
                        res->len = i;
                        drop_obj(res);
                        return v;
                    }

                    v = clone_obj(v);  // Clone result before storing
                    ins_obj(&res, i, v);
                    drop_obj(v);  // Drop the original result
                }

                return res;
            }

            v = binary_call(f, x[0], x[1]);
            if (IS_ERR(v))
                return v;

            res = LIST(1);
            ins_obj(&res, 0, v);
            return res;

        case TYPE_VARY:
            if (n == 0)
                return NULL_OBJ;

            l = ops_rank(x, n);
            if (l == NULL_I64)
                THROW(ERR_LENGTH, "'scan': arguments have different lengths");

            for (i = 0; i < n; i++)
                stack_push(at_idx(x[i], 0));

            v = vary_call(f, x, n);

            // cleanup stack
            for (i = 0; i < n; i++)
                drop_obj(stack_pop());

            if (IS_ERR(v))
                return v;

            res = v->type < 0 ? vector(v->type, l) : LIST(l);
            ins_obj(&res, 0, v);

            for (i = 1; i < l; i++) {
                for (j = 0; j < n; j++)
                    stack_push(at_idx(x[j], i));

                v = vary_call(f, x, n);

                // cleanup stack
                for (j = 0; j < n; j++)
                    drop_obj(stack_pop());

                if (IS_ERR(v)) {
                    res->len = i;
                    drop_obj(res);
                    return v;
                }

                v = clone_obj(v);  // Clone result before storing
                ins_obj(&res, i, v);
                drop_obj(v);  // Drop the original result
            }

            return res;
        case TYPE_LAMBDA:
            l = ops_rank(x, n);
            if (l == NULL_I64)
                THROW(ERR_LENGTH, "'scan': arguments have different lengths");

            if (n != 1 && n != AS_LAMBDA(f)->args->len)
                THROW(ERR_LENGTH, "'scan': lambda call with wrong arguments count");

            if (n == 1) {
                l = ops_count(x[0]);
                if (l == 0)
                    return LIST(0);

                v = at_idx(x[0], 0);
                res = LIST(l);
                ins_obj(&res, 0, v);

                for (i = 1; i < l; i++) {
                    stack_push(clone_obj(v));
                    x2 = at_idx(x[0], i);
                    stack_push(x2);

                    v = call(f, 2);

                    drop_obj(stack_pop());
                    drop_obj(stack_pop());

                    if (IS_ERR(v)) {
                        res->len = i;
                        drop_obj(res);
                        return v;
                    }

                    v = clone_obj(v);  // Clone result before storing
                    ins_obj(&res, i, v);
                    drop_obj(v);  // Drop the original result
                }

                return res;
            } else if (n == 2) {
                l = ops_count(x[0]);
                if (l == 0)
                    return LIST(0);

                v = at_idx(x[0], 0);
                res = LIST(l);
                ins_obj(&res, 0, v);

                for (i = 1; i < l; i++) {
                    stack_push(clone_obj(v));
                    x2 = at_idx(x[1], i);
                    stack_push(x2);

                    v = call(f, 2);

                    drop_obj(stack_pop());
                    drop_obj(stack_pop());

                    if (IS_ERR(v)) {
                        res->len = i;
                        drop_obj(res);
                        return v;
                    }

                    v = clone_obj(v);  // Clone result before storing
                    ins_obj(&res, i, v);
                    drop_obj(v);  // Drop the original result
                }

                return res;
            } else
                THROW(ERR_LENGTH, "'scan': binary call with wrong arguments count");
        default:
            THROW(ERR_TYPE, "'scan': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_scan_left(obj_p *x, i64_t n) {
    i64_t i, j, l;
    obj_p f, v, x1, x2, res;

    if (n < 2)
        return LIST(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'scan-left': unary call with wrong arguments count");
            return unary_call(f, x[0]);
        case TYPE_BINARY:
            if (n < 2)
                THROW(ERR_LENGTH, "'scan-left': binary call with wrong arguments count");

            l = ops_count(x[0]);
            if (l == 0)
                return LIST(0);

            v = clone_obj(x[1]);  // Use rightmost argument as initial value
            res = LIST(l + 1);
            ins_obj(&res, 0, v);

            for (i = 0; i < l; i++) {
                x1 = at_idx(x[0], i);        // Current element from leftmost argument
                x2 = clone_obj(v);           // Clone previous result
                v = binary_call(f, x1, x2);  // Pass current element as first argument
                drop_obj(x1);
                drop_obj(x2);

                if (IS_ERR(v)) {
                    res->len = i + 1;
                    drop_obj(res);
                    return v;
                }

                v = clone_obj(v);  // Clone result before storing
                ins_obj(&res, i + 1, v);
                drop_obj(v);  // Drop the original result
            }

            return res;
        case TYPE_VARY:
            return vary_call(f, x, n);
        case TYPE_LAMBDA:
            if (n < 2 || AS_LAMBDA(f)->args->len != n)
                THROW(ERR_LENGTH, "'scan-left': lambda call with wrong arguments count");

            l = ops_count(x[0]);
            if (l == 0)
                return LIST(0);

            v = clone_obj(x[1]);  // Use rightmost argument as initial value
            res = LIST(l + 1);
            ins_obj(&res, 0, v);

            for (i = 0; i < l; i++) {
                // Push current element from the leftmost argument first
                x1 = at_idx(x[0], i);
                stack_push(x1);
                // Push result of previous iteration as second argument
                x2 = clone_obj(v);
                stack_push(x2);
                // Push all other arguments in between
                for (j = 1; j < n - 1; j++) {
                    stack_push(clone_obj(x[j]));
                }

                v = call(f, n);

                // Cleanup stack
                for (j = 0; j < n; j++) {
                    drop_obj(stack_pop());
                }

                if (IS_ERR(v)) {
                    res->len = i + 1;
                    drop_obj(res);
                    return v;
                }

                v = clone_obj(v);  // Clone result before storing
                ins_obj(&res, i + 1, v);
                drop_obj(v);  // Drop the original result
            }

            return res;
        default:
            THROW(ERR_TYPE, "'scan-left': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_scan_right(obj_p *x, i64_t n) {
    i64_t i, j, l;
    obj_p f, v, x1, x2, res;

    if (n < 2)
        return LIST(0);

    f = x[0];
    x++;
    n--;

    switch (f->type) {
        case TYPE_UNARY:
            if (n != 1)
                THROW(ERR_LENGTH, "'scan-right': unary call with wrong arguments count");
            return unary_call(f, x[0]);
        case TYPE_BINARY:
            if (n < 2)
                THROW(ERR_LENGTH, "'scan-right': binary call with wrong arguments count");

            l = ops_count(x[n - 1]);  // Last argument is the one we iterate over
            if (l == 0)
                return LIST(0);

            v = clone_obj(x[0]);  // Use leftmost argument as initial value
            res = LIST(l + 1);
            ins_obj(&res, 0, v);

            for (i = 0; i < l; i++) {
                x1 = at_idx(x[n - 1], i);  // Current element from rightmost argument
                x2 = clone_obj(v);         // Clone previous result
                v = binary_call(f, x1, x2);
                drop_obj(x1);
                drop_obj(x2);

                if (IS_ERR(v)) {
                    res->len = i + 1;
                    drop_obj(res);
                    return v;
                }

                v = clone_obj(v);  // Clone result before storing
                ins_obj(&res, i + 1, v);
                drop_obj(v);  // Drop the original result
            }

            return res;
        case TYPE_VARY:
            return vary_call(f, x, n);
        case TYPE_LAMBDA:
            if (n < 2 || AS_LAMBDA(f)->args->len != n)
                THROW(ERR_LENGTH, "'scan-right': lambda call with wrong arguments count");

            l = ops_count(x[n - 1]);  // Last argument is the one we iterate over
            if (l == 0)
                return LIST(0);

            v = clone_obj(x[0]);  // Use leftmost argument as initial value
            res = LIST(l + 1);
            ins_obj(&res, 0, v);

            for (i = 0; i < l; i++) {
                // Push current element from the rightmost argument
                x1 = at_idx(x[n - 1], i);
                stack_push(x1);
                // Push result of previous iteration
                x2 = clone_obj(v);
                stack_push(x2);
                // Push all other arguments in between
                for (j = 0; j < n - 1; j++) {
                    stack_push(clone_obj(x[j]));
                }

                v = call(f, n);

                // Cleanup stack
                for (j = 0; j < n; j++) {
                    drop_obj(stack_pop());
                }

                if (IS_ERR(v)) {
                    res->len = i + 1;
                    drop_obj(res);
                    return v;
                }

                v = clone_obj(v);  // Clone result before storing
                ins_obj(&res, i + 1, v);
                drop_obj(v);  // Drop the original result
            }

            return res;
        default:
            THROW(ERR_TYPE, "'scan-right': unsupported function type: '%s", type_name(f->type));
    }
}
