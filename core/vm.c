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
#include "string.h"
#include "lambda.h"
#include "vm.h"
#include "rayforce.h"
#include "heap.h"
#include "format.h"
#include "util.h"
#include "string.h"
#include "ops.h"
#include "env.h"
#include "runtime.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "cc.h"

// CASSERT(sizeof(struct vm_t) == 48, vm_h)
CASSERT(OP_INVALID < 127, vm_h)

#define stack_push(x) (vm->stack[vm->sp++] = x)
#define stack_pop() (vm->stack[--vm->sp])
#define stack_peek() (&vm->stack[vm->sp - 1])
#define stack_peek_n(n) (&vm->stack[vm->sp - (n)])
#define stack_debug()                                        \
    {                                                        \
        for (i64_t _i = vm->sp; _i > 0;)                     \
            debug("%lld: %p", vm->sp - _i, vm->stack[--_i]); \
    }

vm_t vm_new(obj_t *stack)
{
    if (stack == NULL)
        stack = (obj_t *)mmap_stack(VM_STACK_SIZE);

    vm_t vm = {
        .stack = stack,
        .sp = 0,
    };

    return vm;
}

/*
 * Execute the lambda
 */
obj_t __attribute__((hot)) vm_exec(vm_t *vm, obj_t fun)
{
    type_t t;
    lambda_t *f = as_lambda(fun);
    str_t code = as_string(f->code);
    obj_t arg[4], e, r, *addr;
    u8_t n, attrs;
    u64_t i, j, l;
    i32_t b, sp, bp, ip;
    volatile u64_t a;

    // init registers
    vm->ip = 0;
    vm->bp = vm->sp;

    b = 0;
    // offset into an args array
    a = 0;

    // we are getting here from call longjmp(vm->jmp, ..);
    sp = setjmp(vm->jmp);
    if (sp > 0)
    {
        sp ^= vm->sp;
        vm->sp ^= sp;
        sp ^= vm->sp;

        e = stack_pop(); // pop en error
        span_t span = nfo_get(&f->nfo, b);
        *(span_t *)&as_list(e)[2] = span;

        // clear stack (if any)
        while (vm->sp > sp)
            drop(stack_pop());

        // clear args (if any)
        for (; a > 0; a--)
            drop(arg[a - 1]);

        return e;
    }

    // The indices of labels in the dispatch_table are the relevant opcodes
    static nil_t *dispatch_table[] = {
        &&op_ret, &&op_push, &&op_pop, &&op_swap, &&op_dup, &&op_jne, &&op_jmp, &&op_call1,
        &&op_call2, &&op_calln, &&op_calld, &&op_timer_set, &&op_timer_get, &&op_store, &&op_load,
        &&op_lset, &&op_lget, &&op_lpush, &&op_lpop, &&op_try, &&op_catch, &&op_throw, &&op_trace};

#define dispatch() goto *dispatch_table[(i32_t)code[vm->ip]]

#define unwrap(x, y)                                      \
    {                                                     \
        obj_t _o = x, _v;                                 \
        if (_o && _o->type == TYPE_ERROR)                 \
        {                                                 \
            if (!as_list(_o)[2])                          \
            {                                             \
                span_t span = nfo_get(&f->nfo, (i64_t)y); \
                *(span_t *)&as_list(_o)[2] = span;        \
            }                                             \
            while (vm->sp != vm->bp)                      \
            {                                             \
                _v = stack_pop();                         \
                drop(_v);                                 \
            }                                             \
            return _o;                                    \
        }                                                 \
    }

#define load_u64(x, v)                                    \
    {                                                     \
        str_t _p = align8(code + (v)->ip);                \
        u64_t _o = _p - (code + (v)->ip) + sizeof(u64_t); \
        (v)->ip += _o;                                    \
        x = *(u64_t *)_p;                                 \
    }

    dispatch();

op_ret:
    if (vm->sp > 0)
        return stack_pop();
    else
        return null(0);
op_push:
    vm->ip++;
    n = code[vm->ip++];
    arg[0] = clone(as_list(f->constants)[n]);
    stack_push(arg[0]);
    dispatch();
op_pop:
    vm->ip++;
    drop(stack_pop());
    dispatch();
op_swap:
    vm->ip++;
    arg[1] = stack_pop();
    arg[0] = stack_pop();
    stack_push(arg[1]);
    stack_push(arg[0]);
    dispatch();
op_dup:
    vm->ip++;
    addr = stack_peek();
    stack_push(clone(*addr));
    dispatch();
op_jne:
    vm->ip++;
    arg[0] = stack_pop();
    load_u64(l, vm);
    if (!rfi_as_bool(arg[0]))
        vm->ip = l;
    drop(arg[0]);
    dispatch();
op_jmp:
    vm->ip++;
    load_u64(l, vm);
    vm->ip = l;
    dispatch();
op_call1:
    b = vm->ip++;
    attrs = code[vm->ip++];
    load_u64(l, vm);
made_call1:
    a = 1;
    arg[0] = stack_pop();
    r = rf_call_unary(attrs, (unary_f)l, arg[0]);
    drop(arg[0]);
    unwrap(r, b);
    stack_push(r);
    dispatch();
op_call2:
    b = vm->ip++;
    attrs = code[vm->ip++];
    load_u64(l, vm);
made_call2:
    a = 2;
    arg[1] = stack_pop();
    arg[0] = stack_pop();
    r = rf_call_binary(attrs, (binary_f)l, arg[0], arg[1]);
    drop(arg[0]);
    drop(arg[1]);
    unwrap(r, b);
    stack_push(r);
    dispatch();
op_calln:
    b = vm->ip++;
    n = code[vm->ip++];
    attrs = code[vm->ip++];
    load_u64(l, vm);
made_calln:
    a = 0;
    addr = (obj_t *)(&vm->stack[vm->sp - n]);
    r = rf_call_vary(attrs, (vary_f)l, addr, n);
    for (i = 0; i < n; i++)
        drop(stack_pop()); // pop args
    unwrap(r, b);
    stack_push(r);
    dispatch();
op_calld:
    b = vm->ip++;
    n = code[vm->ip++];
    a = 0;
    arg[0] = stack_pop();
    switch (arg[0]->type)
    {
    case TYPE_UNARY:
        if (n != 1)
            unwrap(error(ERR_LENGTH, "wrong number of arguments"), b);
        l = arg[0]->i64;
        attrs = arg[0]->attrs;
        drop(arg[0]);
        goto made_call1;
    case TYPE_BINARY:
        if (n != 2)
            unwrap(error(ERR_LENGTH, "wrong number of arguments"), b);
        l = arg[0]->i64;
        attrs = arg[0]->attrs;
        drop(arg[0]);
        goto made_call2;
    case TYPE_VARY:
        l = arg[0]->i64;
        attrs = arg[0]->attrs;
        drop(arg[0]);
        goto made_calln;
    case TYPE_LAMBDA:
        l = as_lambda(arg[0])->args->len;
        if (n != l)
            unwrap(error(ERR_LENGTH, "wrong number of arguments"), b);
        if ((vm->sp + as_lambda(arg[0])->stack_size) * sizeof(obj_t) > VM_STACK_SIZE)
            unwrap(error(ERR_STACK_OVERFLOW, "stack overflow"), b);
        bp = vm->bp;
        ip = vm->ip;
        r = vm_exec(vm, arg[0]);
        vm->bp = bp;
        vm->ip = ip;
        drop(arg[0]); // drop lambda
        // drop args
        for (i = 0; i < l; i++)
            drop(stack_pop());
        unwrap(r, b);
        stack_push(r);
        dispatch();
    default:
        unwrap(error(ERR_TYPE, "call"), b);
    }
op_timer_set:
    vm->ip++;
    vm->timer = clock();
    dispatch();
op_timer_get:
    vm->ip++;
    stack_push(f64((((f64_t)(clock() - vm->timer)) / CLOCKS_PER_SEC) * 1000));
    dispatch();
op_store:
    b = vm->ip++;
    load_u64(t, vm);
    r = stack_pop();
    vm->stack[vm->bp - t] = r;
    dispatch();
op_load:
    b = vm->ip++;
    load_u64(t, vm);
    r = vm->stack[vm->bp - t];
    stack_push(clone(r));
    dispatch();
op_lset:
    b = vm->ip++;
    a = 2;
    arg[1] = stack_pop();
    arg[0] = stack_pop();
    if (f->locals->len == 0)
        push_obj(&f->locals, dict(vector_symbol(0), list(0)));
    set_obj(&as_list(f->locals)[f->locals->len - 1], arg[0], clone(arg[1]));
    drop(arg[0]);
    stack_push(arg[1]);
    dispatch();
op_lget:
    b = vm->ip++;
    a = 2;
    j = f->locals->len;
    arg[0] = stack_pop();
    arg[1] = null(0);
    for (i = 0; i < j; i++)
    {
        arg[1] = at_obj(as_list(f->locals)[j - i - 1], arg[0]);
        if (!is_null(arg[1]))
            break;
    }
    if (is_null(arg[1]))
        arg[1] = rf_get(arg[0]);
    drop(arg[0]);
    unwrap(arg[1], b);
    stack_push(arg[1]);
    dispatch();
op_lpush:
    b = vm->ip++;
    arg[0] = stack_pop(); // table or dict
    if (!arg[0] || (arg[0]->type != TYPE_TABLE && arg[0]->type != TYPE_DICT))
    {
        drop(arg[0]);
        unwrap(error(ERR_TYPE, "expected dict or table"), b);
    }
    push_obj(&f->locals, arg[0]);
    dispatch();
op_lpop:
    b = vm->ip++;
    arg[0] = pop_obj(&f->locals);
    stack_push(arg[0]);
    dispatch();
op_try:
    b = vm->ip++;
    // load_u64(t, vm);
    // // save ctx
    // ctx = (ctx_t){.addr = NULL, .ip = (i32_t)t, .bp = vm->bp};
    // vm->bp = vm->sp;
    // ((ctx_t *)vm->stack)[vm->sp++] = ctx;
    //--
    dispatch();
op_catch:
    b = vm->ip++;
    // x1 = vm->acc;
    // x1.type = TYPE_CHAR;
    // vm->acc = null(0);
    // stack_push( x1);
    dispatch();
op_throw:
    b = vm->ip++;
    // x1 = stack_pop();
    // x1.type = TYPE_ERROR;
    // x1.adt->code = ERR_THROW;
    // unwrap(x1, b);
    // must not ever rich here, just to satisfy compiler
    dispatch();
op_trace:
    b = vm->ip++;
    // x1 = stack_pop();
    // vm->trace = (u8_t)x1.i64;
    dispatch();
}

nil_t vm_free(vm_t *vm)
{
    // clear stack (if any)
    while (vm->sp > 0)
        drop(stack_pop());

    mmap_free(vm->stack, VM_STACK_SIZE);
}

/*
 * Format code obj in a user readable form for debugging
 */
str_t vm_code_fmt(obj_t fun)
{
#define get_u64(x, c, v)                         \
    {                                            \
        str_t _p = align8(c + v);                \
        u64_t _o = _p - (c + v) + sizeof(u64_t); \
        v += _o;                                 \
        x = *(u64_t *)_p;                        \
    }

    lambda_t *f = as_lambda(fun);
    str_t code = as_string(f->code);
    i32_t c = 0, l = 0, o = 0;
    u32_t b, ip = 0, len = (u32_t)f->code->len;
    u64_t p;
    str_t s = NULL;
    u8_t n, m;

    while (ip < len)
    {
        switch (code[ip])
        {
        case OP_RET:
            b = ip++;
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] ret\n", c++, b);
            break;
        case OP_PUSH:
            b = ip++;
            n = code[ip++];
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] push <const id: %d>\n", c++, b, n);
            break;
        case OP_POP:
            b = ip++;
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] pop\n", c++, b);
            break;
        case OP_JNE:
            b = ip++;
            get_u64(p, code, ip);
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] jne <to: %lld>\n", c++, b, p);
            break;
        case OP_JMP:
            b = ip++;
            get_u64(p, code, ip);
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] jmp <to: %lld>\n", c++, b, p);
            break;
        case OP_CALL1:
            b = ip++;
            n = code[ip++];
            get_u64(p, code, ip);
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] call1 <attrs: %d fn: %p>\n", c++, b, n, p);
            break;
        case OP_CALL2:
            b = ip++;
            n = code[ip++];
            get_u64(p, code, ip);
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] call2 <attrs: %d fn: %p>\n", c++, b, n, p);
            break;
        case OP_CALLN:
            b = ip++;
            m = code[ip++];
            n = code[ip++];
            get_u64(p, code, ip);
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] calln <argn: %d attrs: %d fn: %p>\n", c++, b, m, n, p);
            break;
        case OP_CALLD:
            b = ip++;
            m = code[ip++];
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] calld <argn: %d>\n", c++, b, m);
            break;
        case OP_TIMER_SET:
            b = ip++;
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] timer_set\n", c++, b);
            break;
        case OP_TIMER_GET:
            b = ip++;
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] timer_get\n", c++, b);
            break;
        case OP_STORE:
            b = ip++;
            get_u64(p, code, ip);
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] store <at: %d>\n", c++, b, (i32_t)p);
            break;
        case OP_LOAD:
            b = ip++;
            get_u64(p, code, ip);
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] load <at: %d>\n", c++, b, (i32_t)p);
            break;
        case OP_LSET:
            b = ip++;
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] lset\n", c++, b);
            break;
        case OP_LGET:
            b = ip++;
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] lget\n", c++, b);
            break;
        case OP_TRY:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] try ", c++, ip++);
            obj_fmt_into(&s, &l, &o, 0, 0, false, (obj_t)(code + ip));
            str_fmt_into(&s, &l, &o, 0, "\n");
            ip += sizeof(obj_t);
            break;
        case OP_CATCH:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] catch ", c++, ip++);
            break;
        case OP_THROW:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] throw\n", c++, ip++);
            break;
        case OP_TRACE:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] trace\n", c++, ip++);
            break;
        default:
            str_fmt_into(&s, &l, &o, 0, "%.4d: unknown %d\n", c++, ip++);
            break;
        }
    }

    return s;
}
