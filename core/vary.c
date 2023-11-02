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

#include <stdio.h>
#include "vm.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "heap.h"
#include "string.h"
#include "format.h"
#include "util.h"
#include "runtime.h"

#define __NULL_ARG 0xffffffffffffffff
#define __args_height(l, x, n)                                  \
    {                                                           \
        l = args_height(x, n);                                  \
        if (l == __NULL_ARG)                                    \
            emit(ERR_LENGTH, "inconsistent arguments lengths"); \
                                                                \
        if (l == 0)                                             \
            return null(0);                                     \
    }

obj_t ray_call_vary_atomic(vary_f f, obj_t *x, u64_t n)
{
    u64_t i, lists = 0;

    for (i = 0; i < n; i++)
        if (is_vector(x[i]))
            lists++;

    if (lists == n)
        return f(x, n);
    else
        return f(x, n);
}

obj_t ray_call_vary(u8_t attrs, vary_f f, obj_t *x, u64_t n)
{
    switch (attrs)
    {
    case FN_ATOMIC:
        return ray_call_vary_atomic(f, x, n);
    default:
        return f(x, n);
    }
}

u64_t args_height(obj_t *x, u64_t n)
{
    u64_t i, l;
    obj_t *b;

    l = __NULL_ARG;
    for (i = 0; i < n; i++)
    {
        b = x + i;
        if ((is_vector(*b) || (*b)->type == TYPE_LISTMAP) && l == __NULL_ARG)
            l = ops_count(*b);
        else if ((is_vector(*b) || (*b)->type == TYPE_LISTMAP) && ops_count(*b) != l)
            return __NULL_ARG;
    }

    // all are atoms
    if (l == __NULL_ARG)
        l = 1;

    return l;
}

obj_t ray_map_vary_f(obj_t f, obj_t *x, u64_t n)
{
    u64_t i, j, l;
    vm_t *vm;
    vm_ctx_t ctx;
    obj_t v, *b, res;

    switch (f->type)
    {
    case TYPE_UNARY:
        if (n != 1)
            emit(ERR_TYPE, "'map': unary call with wrong arguments count");
        return ray_call_unary(FN_ATOMIC, (unary_f)f->i64, x[0]);
    case TYPE_BINARY:
        if (n != 2)
            emit(ERR_TYPE, "'map': binary call with wrong arguments count");
        return ray_call_binary(FN_ATOMIC, (binary_f)f->i64, x[0], x[1]);
    case TYPE_VARY:
        return ray_call_vary(FN_ATOMIC, (vary_f)f->i64, x, n);
    case TYPE_LAMBDA:
        if (n != as_lambda(f)->args->len)
            emit(ERR_TYPE, "'map': lambda call with wrong arguments count");

        __args_height(l, x, n);

        vm = &runtime_get()->vm;

        // first item to get type of res
        for (j = 0; j < n; j++)
        {
            b = x + j;
            v = (is_vector(*b) || (*b)->type == TYPE_LISTMAP) ? at_idx(*b, 0) : clone(*b);
            vm->stack[vm->sp++] = v;
        }

        ctx = vm_save_ctx(vm);
        v = vm_exec(vm, f);
        vm_restore_ctx(vm, ctx);

        if (is_error(v))
            return v;

        res = v->type < 0 ? vector(v->type, l) : vector(TYPE_LIST, l);

        ins_obj(&res, 0, v);

        // drop args
        for (j = 0; j < n; j++)
            drop(vm->stack[--vm->sp]);

        for (i = 1; i < l; i++)
        {
            for (j = 0; j < n; j++)
            {
                b = x + j;
                v = (is_vector(*b) || (*b)->type == TYPE_LISTMAP) ? at_idx(*b, i) : clone(*b);
                vm->stack[vm->sp++] = v;
            }

            ctx = vm_save_ctx(vm);
            v = vm_exec(vm, f);
            vm_restore_ctx(vm, ctx);

            // drop args
            for (j = 0; j < n; j++)
                drop(vm->stack[--vm->sp]);

            // check error
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
        emit(ERR_TYPE, "'map': unsupported function type: %d", f->type);
    }
}

obj_t ray_fold_vary_f(obj_t f, obj_t *x, u64_t n)
{
    u64_t i, j, o, l;
    vm_t *vm;
    vm_ctx_t ctx;
    obj_t v, x1, x2, *b;

    switch (f->type)
    {
    case TYPE_UNARY:
        if (n != 1)
            emit(ERR_TYPE, "'fold': unary call with wrong arguments count");
        return ray_call_unary(FN_ATOMIC, (unary_f)f->i64, x[0]);
    case TYPE_BINARY:
        __args_height(l, x, n);
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
            emit(ERR_TYPE, "'fold': binary call with wrong arguments count");

        for (i = o; i < l; i++)
        {
            x1 = v;
            x2 = at_idx(*b, i);
            v = ray_call_binary(FN_ATOMIC, (binary_f)f->i64, x1, x2);
            drop(x1);
            drop(x2);

            if (is_error(v))
                return v;
        }

        return v;
    case TYPE_VARY:
        return ray_call_vary(FN_ATOMIC, (vary_f)f->i64, x, n);
    case TYPE_LAMBDA:
        if (n != as_lambda(f)->args->len)
            emit(ERR_TYPE, "'fold': lambda call with wrong arguments count");

        __args_height(l, x, n);

        vm = &runtime_get()->vm;

        // interpret first arg as an initial value
        if (n > 1)
        {
            o = 1;
            v = clone(x[0]);
        }
        else
        {
            o = 0;
            v = null(x[0]->type);
        }

        for (i = 0; i < l; i++)
        {
            vm->stack[vm->sp++] = v;

            for (j = o; j < n; j++)
            {
                b = x + j;
                v = at_idx(*b, i);
                vm->stack[vm->sp++] = v;
            }

            ctx = vm_save_ctx(vm);
            v = vm_exec(vm, f);
            vm_restore_ctx(vm, ctx);

            // drop args
            for (j = 0; j < n; j++)
                drop(vm->stack[--vm->sp]);

            // check error
            if (is_error(v))
                return v;
        }

        return v;
    default:
        emit(ERR_TYPE, "'fold': unsupported function type: %d", f->type);
    }
}

obj_t ray_map_vary(obj_t *x, u64_t n)
{
    if (n == 0)
        return list(0);

    return ray_map_vary_f(x[0], x + 1, n - 1);
}

obj_t ray_fold_vary(obj_t *x, u64_t n)
{
    if (n == 0)
        return null(0);

    return ray_fold_vary_f(x[0], x + 1, n - 1);
}

obj_t ray_gc(obj_t *x, u64_t n)
{
    unused(x);
    unused(n);
    return i64(heap_gc());
}

obj_t ray_format(obj_t *x, u64_t n)
{
    str_t s = obj_fmt_n(x, n);
    obj_t ret;

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    ret = string_from_str(s, strlen(s));
    heap_free(s);

    return ret;
}

obj_t ray_print(obj_t *x, u64_t n)
{
    str_t s = obj_fmt_n(x, n);

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    printf("%s", s);
    heap_free(s);

    return null(0);
}

obj_t ray_println(obj_t *x, u64_t n)
{
    str_t s = obj_fmt_n(x, n);

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    printf("%s\n", s);
    heap_free(s);

    return null(0);
}

obj_t ray_args(obj_t *x, u64_t n)
{
    unused(x);
    unused(n);
    return clone(runtime_get()->args);
}