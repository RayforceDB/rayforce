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

obj_t ray_map(obj_t *x, u64_t n)
{
    i64_t i, j, l;
    obj_t f, v, *b, res;

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
        if (l == -1)
            throw(ERR_LENGTH, "'map': arguments have different lengths");

        // first item to get type of res
        for (j = 0; j < (i64_t)n; j++)
        {
            b = x + j;
            v = (is_vector(*b) || (*b)->type == TYPE_GROUPMAP) ? at_idx(*b, 0) : clone(*b);
            stack_push(v);
        }

        v = call(f, n);
        if (is_error(v))
            return v;

        res = v->type < 0 ? vector(v->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, v);

        for (i = 1; i < l; i++)
        {
            for (j = 0; j < (i64_t)n; j++)
            {
                b = x + j;
                v = (is_vector(*b) || (*b)->type == TYPE_GROUPMAP) ? at_idx(*b, i) : clone(*b);
                stack_push(v);
            }

            v = call(f, n);
            if (is_error(v))
            {
                res->len = i;
                drop(res);
                return v;
            }

            ins_obj(&res, i, v);
        }

        return res;
    default:
        throw(ERR_TYPE, "'map': unsupported function type: '%s", typename(f->type));
    }
}

obj_t ray_fold(obj_t *x, u64_t n)
{
    i64_t o, i, l;
    obj_t f, v, *b, x1, x2;

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
        if (l == -1)
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
            v = clone(x[0]);
        }
        else
            throw(ERR_LENGTH, "'fold': binary call with wrong arguments count");

        for (i = o; i < l; i++)
        {
            x1 = v;
            x2 = at_idx(*b, i);
            v = binary_call(FN_ATOMIC, (binary_f)f->i64, x1, x2);
            drop(x1);
            drop(x2);

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
        if (l == -1)
            throw(ERR_LENGTH, "'map': arguments have different lengths");
        return NULL;

        // vm = &runtime_get()->vm;

        // // interpret first arg as an initial value
        // if (n > 1)
        // {
        //     o = 1;
        //     v = clone(x[0]);
        // }
        // else
        // {
        //     o = 0;
        //     v = null(x[0]->type);
        // }

        // for (i = 0; i < l; i++)
        // {
        //     vm->stack[vm->sp++] = v;

        //     for (j = o; j < n; j++)
        //     {
        //         b = x + j;
        //         v = at_idx(*b, i);
        //         vm->stack[vm->sp++] = v;
        //     }

        //     ctx = vm_save_ctx(vm);
        //     v = vm_exec(vm, f);
        //     vm_restore_ctx(vm, ctx);

        //     // drop args
        //     for (j = 0; j < n; j++)
        //         drop(vm->stack[--vm->sp]);

        //     // check error
        //     if (is_error(v))
        //         return v;
        // }

        // return v;
    default:
        throw(ERR_TYPE, "'fold': unsupported function type: '%s", typename(f->type));
    }
}
