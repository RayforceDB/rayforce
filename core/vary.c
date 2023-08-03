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

obj_t rf_call_vary_atomic(vary_f f, obj_t *x, u64_t n)
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

obj_t rf_call_vary(u8_t attrs, vary_f f, obj_t *x, u64_t n)
{
    switch (attrs)
    {
    case FLAG_ATOMIC:
        return rf_call_vary_atomic(f, x, n);
    default:
        return f(x, n);
    }
}

obj_t rf_map_vary(obj_t *x, u64_t n)
{
    u64_t i, j, l;
    vm_t vm;
    obj_t v, *b, res;

    if (n < 2)
        return null(0);

    switch ((*x)->type)
    {
    case TYPE_UNARY:
        if (n != 2)
            raise(ERR_TYPE, "'map': unary call with wrong arguments count");
        return rf_call_unary(FLAG_ATOMIC, (unary_f)(*x)->i64, x[1]);
    case TYPE_BINARY:
        if (n != 3)
            raise(ERR_TYPE, "'map': binary call with wrong arguments count");
        return rf_call_binary(FLAG_ATOMIC, (binary_f)(*x)->i64, x[1], x[2]);
    case TYPE_VARY:
        return rf_call_vary(FLAG_ATOMIC, (vary_f)(*x)->i64, x + 1, n - 1);
    case TYPE_LAMBDA:
        if ((n - 1) != as_lambda(*x)->args->len)
            raise(ERR_TYPE, "'map': lambda call with wrong arguments count");
        l = 0xffffffffffffffff;
        for (i = 1; i < n; i++)
        {
            b = x + i;
            if (is_vector(*b) && l == 0xffffffffffffffff)
                l = (*b)->len;
            else if (is_vector(*b) && (*b)->len != l)
                raise(ERR_LENGTH, "'map': inconsistent arguments lengths")
        }

        if (l == 0)
            return null(0);

        // all are atoms
        if (l == 0xffffffffffffffff)
            l = 1;

        vm = vm_new(runtime_get()->vm.stack);
        vm.sp = runtime_get()->vm.sp;

        // first item to get type of res
        for (j = 1; j < n; j++)
        {
            b = x + j;
            v = is_vector(*b) ? at_idx(*b, 0) : clone(*b);
            vm.stack[vm.sp++] = v;
        }

        v = vm_exec(&vm, *x);

        if (is_error(v))
            return v;

        res = vector(v->type, l);

        write_obj(&res, 0, v);

        // drop args
        for (j = 1; j < n; j++)
            drop(vm.stack[--vm.sp]);

        for (i = 1; i < l; i++)
        {
            for (j = 1; j < n; j++)
            {
                b = x + j;
                v = is_vector(*b) ? at_idx(*b, i) : clone(*b);
                vm.stack[vm.sp++] = v;
            }

            v = vm_exec(&vm, *x);

            // drop args
            for (j = 1; j < n; j++)
                drop(vm.stack[--vm.sp]);

            // check error
            if (is_error(v))
            {
                res->len = i;
                drop(res);
                return v;
            }

            write_obj(&res, i, v);
        }

        return res;
    default:
        raise(ERR_TYPE, "'map': unsupported function type: %d", (*x)->type);
    }
}

obj_t rf_list(obj_t *x, u64_t n)
{
    u64_t i;
    obj_t lst = list(n);

    for (i = 0; i < n; i++)
        as_list(lst)[i] = clone(x[i]);

    return lst;
}

obj_t rf_enlist(obj_t *x, u64_t n)
{
    if (n == 0)
        return list(0);

    u64_t i;
    obj_t lst = list(n);

    for (i = 0; i < n; i++)
        as_list(lst)[i] = clone(x[i]);

    return lst;
}

obj_t rf_gc(obj_t *x, u64_t n)
{
    unused(x);
    unused(n);
    return i64(heap_gc());
}

obj_t rf_format(obj_t *x, u64_t n)
{
    str_t s = obj_fmt_n(x, n);
    obj_t ret;

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    ret = string_from_str(s, strlen(s));
    heap_free(s);

    return ret;
}

obj_t rf_print(obj_t *x, u64_t n)
{
    str_t s = obj_fmt_n(x, n);

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    printf("%s", s);
    heap_free(s);

    return null(0);
}

obj_t rf_println(obj_t *x, u64_t n)
{
    str_t s = obj_fmt_n(x, n);

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    printf("%s\n", s);
    heap_free(s);

    return null(0);
}