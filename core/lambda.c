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

#include "lambda.h"
#include "string.h"
#include "heap.h"
#include "eval.h"
#include "util.h"
#include "group.h"
#include "pool.h"

obj_p lambda(obj_p args, obj_p body, obj_p nfo) {
    obj_p obj;
    lambda_p f;

    obj = (obj_p)heap_alloc(sizeof(struct obj_t) + sizeof(struct lambda_t));
    obj->mmod = MMOD_INTERNAL;
    obj->type = TYPE_LAMBDA;
    obj->rc = 1;

    f = (lambda_p)obj->arr;
    f->name = NULL_OBJ;
    f->args = args;
    f->body = body;
    f->nfo = nfo;

    return obj;
}

obj_p lambda_map_partial(obj_p f, obj_p *lst, u64_t n, u64_t arg) {
    u64_t i;

    for (i = 0; i < n; i++)
        stack_push(at_idx(lst[i], arg));

    return call(f, n);
}

obj_p lambda_map(obj_p f, obj_p *x, u64_t n) {
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
            pool_add_task(pool, (raw_p)lambda_map_partial, 4, f, x, n, j);

        res = pool_run(pool);
        UNWRAP_LIST(res);

        goto cleanup;
    }

    for (j = 0; j < n; j++)
        stack_push(at_idx(x[j], 0));

    v = call(f, n);

    if (IS_ERROR(v)) {
        res = v;
        goto cleanup;
    }

    res = v->type < 0 ? vector(v->type, l) : LIST(l);

    ins_obj(&res, 0, v);

    for (i = 1; i < l; i++) {
        for (j = 0; j < n; j++)
            stack_push(at_idx(x[j], i));

        v = call(f, n);

        if (IS_ERROR(v)) {
            res->len = i;
            drop_obj(res);
            res = v;
            goto cleanup;
        }

        ins_obj(&res, i, v);
    }

// cleanup stack
cleanup:
    for (j = 0; j < n; j++)
        drop_obj(stack_pop());

    return res;
}

obj_p lambda_call(u8_t attrs, obj_p f, obj_p *x, u64_t n) {
    UNUSED(attrs);
    UNUSED(x);
    return call(f, n);
}
