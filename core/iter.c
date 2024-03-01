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

obj_p ray_apply(obj_p *x, u64_t n)
{
    u64_t i;
    obj_p f;

    if (n < 2)
        return null(0);

    f = x[0];
    x++;
    n--;

    switch (f->type)
    {
    case TYPE_UNARY:
        if (n != 1)
            throw(ERR_LENGTH, "'apply': unary call with wrong arguments count");
        return unary_call(FN_ATOMIC, (unary_f)f->i64, x[0]);
    case TYPE_BINARY:
        if (n != 2)
            throw(ERR_LENGTH, "'apply': binary call with wrong arguments count");
        return binary_call(FN_ATOMIC, (binary_f)f->i64, x[0], x[1]);
    case TYPE_VARY:
        return vary_call(FN_ATOMIC, (vary_f)f->i64, x, n);
    case TYPE_LAMBDA:
        if (n != as_lambda(f)->args->len)
            throw(ERR_LENGTH, "'apply': lambda call with wrong arguments count");

        for (i = 0; i < n; i++)
            stack_push(clone_obj(x[i]));

        return call(f, n);
    default:
        throw(ERR_TYPE, "'map': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_map(obj_p *x, u64_t n)
{
    u64_t i, j, l;
    obj_p f, v, *b, res;

    if (n < 2)
        return list(0);

    f = x[0];
    x++;
    n--;

    switch (f->type)
    {
    case TYPE_UNARY:
        if (n != 1)
            throw(ERR_LENGTH, "'map': unary call with wrong arguments count");
        return unary_call(FN_ATOMIC, (unary_f)f->i64, x[0]);
    case TYPE_BINARY:
        if (n != 2)
            throw(ERR_LENGTH, "'map': binary call with wrong arguments count");
        return binary_call(FN_ATOMIC, (binary_f)f->i64, x[0], x[1]);
    case TYPE_VARY:
        return vary_call(FN_ATOMIC, (vary_f)f->i64, x, n);
    case TYPE_LAMBDA:
        if (n != as_lambda(f)->args->len)
            throw(ERR_LENGTH, "'map': lambda call with wrong arguments count");

        l = ops_rank(x, n);
        if (l == 0xfffffffffffffffful)
            throw(ERR_LENGTH, "'map': arguments have different lengths");

        // first item to get type of res
        for (j = 0; j < n; j++)
        {
            b = x + j;
            v = (is_vector(*b) || (*b)->type == TYPE_GROUPMAP) ? at_idx(*b, 0) : clone_obj(*b);
            stack_push(v);
        }

        v = call(f, n);
        if (is_error(v))
            return v;

        res = v->type < 0 ? vector(v->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, v);

        for (i = 1; i < l; i++)
        {
            for (j = 0; j < n; j++)
            {
                b = x + j;
                v = (is_vector(*b) || (*b)->type == TYPE_GROUPMAP) ? at_idx(*b, i) : clone_obj(*b);
                stack_push(v);
            }

            v = call(f, n);
            if (is_error(v))
            {
                res->len = i;
                drop_obj(res);
                return v;
            }

            ins_obj(&res, i, v);
        }

        return res;
    default:
        throw(ERR_TYPE, "'map': unsupported function type: '%s", type_name(f->type));
    }
}

obj_p ray_fold(obj_p *x, u64_t n)
{
    u64_t o, i, j, l;
    obj_p f, v, *b, x1, x2;

    if (n < 2)
        return vn_list(0);

    f = x[0];
    x++;
    n--;

    switch (f->type)
    {
    case TYPE_UNARY:
        if (n != 1)
            throw(ERR_LENGTH, "'fold': unary call with wrong arguments count");
        return unary_call(FN_ATOMIC, (unary_f)f->i64, x[0]);
    case TYPE_BINARY:
        l = ops_rank(x, n);
        if (l == 0xfffffffffffffffful)
            throw(ERR_LENGTH, "'map': arguments have different lengths");

        if (n == 1)
        {
            o = 1;
            b = x;
            v = at_idx(x[0], 0);
        }
        else if (n == 2)
        {
            o = 0;
            b = x + 1;
            v = clone_obj(x[0]);
        }
        else
            throw(ERR_LENGTH, "'fold': binary call with wrong arguments count");

        for (i = o; i < l; i++)
        {
            x1 = v;
            x2 = at_idx(*b, i);
            v = binary_call(FN_ATOMIC, (binary_f)f->i64, x1, x2);
            drop_obj(x1);
            drop_obj(x2);

            if (is_error(v))
                return v;
        }

        return v;
    case TYPE_VARY:
        return vary_call(FN_ATOMIC, (vary_f)f->i64, x, n);
    case TYPE_LAMBDA:
        if (n != as_lambda(f)->args->len)
            throw(ERR_LENGTH, "'fold': lambda call with wrong arguments count");

        l = ops_rank(x, n);
        if (l == 0xfffffffffffffffful)
            throw(ERR_LENGTH, "'fold': arguments have different lengths");

        // interpret first arg as an initial value
        if (n > 1)
        {
            o = 1;
            v = clone_obj(x[0]);
        }
        else
        {
            o = 0;
            v = null(x[0]->type);
        }

        for (i = 0; i < l; i++)
        {
            stack_push(v);

            for (j = o; j < n; j++)
            {
                b = x + j;
                v = at_idx(*b, i);
                stack_push(v);
            }

            v = call(f, n);

            if (is_error(v))
                return v;
        }

        return v;
    default:
        throw(ERR_TYPE, "'fold': unsupported function type: '%s", type_name(f->type));
    }
}
