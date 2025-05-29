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
#include "iter.h"

obj_p lambda(obj_p args, obj_p body, obj_p nfo) {
    obj_p obj;
    lambda_p f;

    obj = (obj_p)heap_alloc(sizeof(struct obj_t) + sizeof(struct lambda_f));
    obj->mmod = MMOD_INTERNAL;
    obj->type = TYPE_LAMBDA;
    obj->rc = 1;

    f = (lambda_p)obj->raw;
    f->name = NULL_OBJ;
    f->args = args;
    f->body = body;
    f->nfo = nfo;

    return obj;
}

obj_p lambda_call(obj_p f, obj_p *x, i64_t n) {
    i64_t i;
    obj_p res;

    if (f->attrs & FN_ATOMIC) {
        return map_lambda(f, x, n);
    } else {
        res = call(f, n);
        for (i = 0; i < n; i++)
            drop_obj(stack_pop());

        return res;
    }
}
