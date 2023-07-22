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
#include <string.h>
#include "lambda.h"
#include "vm.h"
#include "rayforce.h"
#include "alloc.h"
#include "format.h"
#include "util.h"
#include "string.h"
#include "ops.h"
#include "env.h"
#include "runtime.h"
#include "dict.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "cc.h"

CASSERT(OP_INVALID < 127, vm_h)

#define stack_push(v, x) (v->stack[v->sp++] = x)
#define stack_pop(v) (v->stack[--v->sp])
#define stack_peek(v) (&v->stack[v->sp - 1])
#define stack_peek_n(v, n) (&v->stack[v->sp - 1 - (n)])
#define stack_pop_free(v)             \
    {                                 \
        rf_object_t o = stack_pop(v); \
        rf_object_free(&o);           \
    }
#define stack_debug(v)                                                     \
    {                                                                      \
        i32_t _i = v->sp;                                                  \
        while (_i > 0)                                                     \
        {                                                                  \
            debug("%d: %s", v->sp - _i, rf_object_fmt(&v->stack[_i - 1])); \
            _i--;                                                          \
        }                                                                  \
    }

typedef struct ctx_t
{
    lambda_t *addr;
    i32_t ip;
    i32_t bp;
} ctx_t;

CASSERT(sizeof(struct ctx_t) == sizeof(struct rf_object_t), vm_c)

vm_t vm_new()
{
    rf_object_t *stack = (rf_object_t *)mmap_stack(VM_STACK_SIZE);

    vm_t vm = {
        .trace = 0,
        .acc = null(),
        .stack = stack,
    };

    return vm;
}

/*
 * Execute the lambda
 */
rf_object_t __attribute__((hot)) vm_exec(vm_t *vm, rf_object_t *fun)
{
    type_t c;
    lambda_t *f = as_lambda(fun);
    str_t code = as_string(&f->code);
    rf_object_t x0, x1, x2, x3, *addr;
    i64_t t;
    u8_t n, flags;
    u64_t l, p;
    i32_t i, j, b;
    ctx_t ctx;

    vm->ip = 0;
    vm->sp = 0;
    vm->bp = -1;

    // The indices of labels in the dispatch_table are the relevant opcodes
    static null_t *dispatch_table[] = {
        &&op_halt, &&op_push, &&op_pop, &&op_swap, &&op_dup, &&op_jne, &&op_jmp, &&op_call1, &&op_call2, &&op_calln,
        &&op_calld, &&op_ret, &&op_timer_set, &&op_timer_get, &&op_store, &&op_load, &&op_lset,
        &&op_lget, &&op_lpush, &&op_lpop, &&op_try, &&op_catch, &&op_throw,
        &&op_trace, &&op_alloc, &&op_map, &&op_collect, &&op_eval, &&op_fload};

#define dispatch() goto *dispatch_table[(i32_t)code[vm->ip]]

#define unwrap(x, y)                                                                                               \
    {                                                                                                              \
        rf_object_t o = x;                                                                                         \
        if (o.type == TYPE_ERROR)                                                                                  \
        {                                                                                                          \
            o.adt->span = debuginfo_get(&f->debuginfo, y);                                                         \
            u8_t n = 0;                                                                                            \
            while (vm->sp)                                                                                         \
            {                                                                                                      \
                x1 = stack_pop(vm);                                                                                \
                if (vm->sp == vm->bp)                                                                              \
                {                                                                                                  \
                    memcpy(&ctx, &x1, sizeof(ctx_t));                                                              \
                    if (!ctx.addr)                                                                                 \
                    {                                                                                              \
                        vm->bp = ctx.bp;                                                                           \
                        vm->ip = ctx.ip;                                                                           \
                        vm->acc = o;                                                                               \
                        goto op_dispatch;                                                                          \
                    }                                                                                              \
                    if (n < vm->trace)                                                                             \
                    {                                                                                              \
                        printf("-> %s:[%d:%d:%d:%d]\n", f->debuginfo.filename,                                     \
                               o.adt->span.start_line + 1, o.adt->span.end_line + 1, o.adt->span.start_column + 1, \
                               o.adt->span.end_column + 1);                                                        \
                        n++;                                                                                       \
                    }                                                                                              \
                    vm->ip = ctx.ip;                                                                               \
                    vm->bp = ctx.bp;                                                                               \
                    f = ctx.addr;                                                                                  \
                    code = as_string(&f->code);                                                                    \
                    continue;                                                                                      \
                }                                                                                                  \
                                                                                                                   \
                if (vm->sp == 0)                                                                                   \
                {                                                                                                  \
                    printf("-> %s:[%d:%d:%d:%d]\n", f->debuginfo.filename,                                         \
                           o.adt->span.start_line + 1, o.adt->span.end_line + 1, o.adt->span.start_column + 1,     \
                           o.adt->span.end_column + 1);                                                            \
                }                                                                                                  \
                                                                                                                   \
                rf_object_free(&x1);                                                                               \
            }                                                                                                      \
            return o;                                                                                              \
        }                                                                                                          \
    }

#define load_u64(x, v)                                    \
    {                                                     \
        str_t _p = align8(code + (v)->ip);                \
        u64_t _o = _p - (code + (v)->ip) + sizeof(u64_t); \
        (v)->ip += _o;                                    \
        x = *(u64_t *)_p;                                 \
    }

op_dispatch:
    dispatch();
op_halt:
    vm->halted = 1;
    if (vm->sp > 0)
        return stack_pop(vm);
    else
        return null();
op_push:
    vm->ip++;
    load_u64(l, vm);
    x1 = vector_get(&f->constants, l);
    stack_push(vm, x1);
    dispatch();
op_pop:
    vm->ip++;
    stack_pop_free(vm);
    dispatch();
op_swap:
    vm->ip++;
    x1 = stack_pop(vm);
    x2 = stack_pop(vm);
    stack_push(vm, x1);
    stack_push(vm, x2);
    dispatch();
op_dup:
    vm->ip++;
    addr = stack_peek(vm);
    stack_push(vm, rf_object_clone(addr));
    dispatch();
op_jne:
    vm->ip++;
    x2 = stack_pop(vm);
    load_u64(l, vm);
    if (!rfi_as_bool(&x2))
        vm->ip = l;
    dispatch();
op_jmp:
    vm->ip++;
    load_u64(l, vm);
    vm->ip = l;
    dispatch();
op_call1:
    b = vm->ip++;
    flags = code[vm->ip++];
    load_u64(l, vm);
made_call1:
    x2 = stack_pop(vm);
    x1 = rf_call_unary(flags, (unary_t)l, &x2);
    rf_object_free(&x2);
    unwrap(x1, b);
    stack_push(vm, x1);
    dispatch();
op_call2:
    b = vm->ip++;
    flags = code[vm->ip++];
    load_u64(l, vm);
made_call2:
    x3 = stack_pop(vm);
    x2 = stack_pop(vm);
    x1 = rf_call_binary(flags, (binary_t)l, &x2, &x3);
    rf_object_free(&x2);
    rf_object_free(&x3);
    unwrap(x1, b);
    stack_push(vm, x1);
    dispatch();
op_calln:
    b = vm->ip++;
    n = code[vm->ip++];
    flags = code[vm->ip++];
    load_u64(l, vm);
made_calln:
    addr = (rf_object_t *)(&vm->stack[vm->sp - n]);
    x1 = rf_call_vary(flags, (vary_t)l, addr, n);
    for (i = 0; i < n; i++)
        stack_pop_free(vm); // pop args
    unwrap(x1, b);
    stack_push(vm, x1);
    dispatch();
op_calld:
    b = vm->ip++;
    n = code[vm->ip++];
made_calld:
    addr = stack_peek(vm);
    switch (addr->type)
    {
    case TYPE_UNARY:
        if (n != 1)
            unwrap(error(ERR_LENGTH, "wrong number of arguments"), b);
        x0 = stack_pop(vm);
        l = x0.i64;
        flags = x0.flags;
        goto made_call1;
    case TYPE_BINARY:
        if (n != 2)
            unwrap(error(ERR_LENGTH, "wrong number of arguments"), b);
        x0 = stack_pop(vm);
        l = x0.i64;
        flags = x0.flags;
        goto made_call2;
    case TYPE_VARY:
        x0 = stack_pop(vm);
        l = x0.i64;
        flags = x0.flags;
        goto made_calln;
    case TYPE_LAMBDA:
        /* Call stack of user lambda call looks as follows:
         * +-------------------+
         * |       ...         |
         * +-------------------+
         * | ctx {ret, ip, sp} | <- bp
         * +-------------------+
         * |     <lambda>      |
         * +-------------------+
         * |       argn        |
         * +-------------------+
         * |       ...         |
         * +-------------------+
         * |       arg2        |
         * +-------------------+
         * |       arg1        |
         * +-------------------+
         */
        if (n != as_lambda(addr)->args.adt->len)
            unwrap(error(ERR_LENGTH, "wrong number of arguments"), b);
        if ((vm->sp + as_lambda(addr)->stack_size) * sizeof(rf_object_t) > VM_STACK_SIZE)
            unwrap(error(ERR_STACK_OVERFLOW, "stack overflow"), b);
        // save ctx
        ctx = (ctx_t){.addr = f, .ip = vm->ip, .bp = vm->bp};
        vm->ip = 0;
        vm->bp = vm->sp;
        ((ctx_t *)vm->stack)[vm->sp++] = ctx;
        // --
        f = as_lambda(addr);
        code = as_string(&f->code);
        break;
    default:
        unwrap(error(ERR_TYPE, "call"), b);
    }
    dispatch();
op_ret:
    vm->ip++;
    x3 = stack_pop(vm); // return value
    x2 = stack_pop(vm); // ctx
    j = (i32_t)f->args.adt->len;
    stack_pop_free(vm); // free lambda
    for (i = 0; i < j; i++)
        stack_pop_free(vm); // pop args
    ctx = *(ctx_t *)&x2;
    vm->ip = ctx.ip;
    vm->bp = ctx.bp;
    f = ctx.addr;
    code = as_string(&f->code);
    stack_push(vm, x3); // push back return value
    dispatch();
op_timer_set:
    vm->ip++;
    vm->timer = clock();
    dispatch();
op_timer_get:
    vm->ip++;
    stack_push(vm, f64((((f64_t)(clock() - vm->timer)) / CLOCKS_PER_SEC) * 1000));
    dispatch();
op_store:
    b = vm->ip++;
    load_u64(t, vm);
    x1 = stack_pop(vm);
    vm->stack[vm->bp + t] = x1;
    dispatch();
op_load:
    b = vm->ip++;
    load_u64(t, vm);
    x1 = vm->stack[vm->bp + t];
    stack_push(vm, rf_object_clone(&x1));
    dispatch();
op_lset:
    b = vm->ip++;
    x2 = stack_pop(vm);
    x1 = stack_pop(vm);
    if (f->locals.adt->len == 0)
        vector_push(&f->locals, dict(vector_symbol(0), list(0)));
    dict_set(&as_list(&f->locals)[f->locals.adt->len - 1], &x1, x2);
    dispatch();
op_lget:
    b = vm->ip++;
    x1 = stack_pop(vm);
    j = f->locals.adt->len;
    x2 = null();
    for (i = 0; i < j; i++)
    {
        x2 = dict_get(&as_list(&f->locals)[j - i - 1], &x1);
        if (!is_null(&x2))
            break;
    }
    if (is_null(&x2))
        x2 = rf_get_variable(&x1);
    unwrap(x2, b);
    stack_push(vm, x2);
    dispatch();
op_lpush:
    b = vm->ip++;
    x1 = stack_pop(vm); // table or dict
    if (x1.type != TYPE_TABLE && x1.type != TYPE_DICT)
        unwrap(error(ERR_TYPE, "expected dict or table"), b);
    vector_push(&f->locals, x1);
    dispatch();
op_lpop:
    b = vm->ip++;
    x1 = vector_pop(&f->locals);
    stack_push(vm, x1);
    dispatch();
op_try:
    b = vm->ip++;
    load_u64(t, vm);
    // save ctx
    ctx = (ctx_t){.addr = NULL, .ip = (i32_t)t, .bp = vm->bp};
    vm->bp = vm->sp;
    ((ctx_t *)vm->stack)[vm->sp++] = ctx;
    //--
    dispatch();
op_catch:
    b = vm->ip++;
    x1 = vm->acc;
    x1.type = TYPE_CHAR;
    vm->acc = null();
    stack_push(vm, x1);
    dispatch();
op_throw:
    b = vm->ip++;
    x1 = stack_pop(vm);
    x1.type = TYPE_ERROR;
    x1.adt->code = ERR_THROW;
    unwrap(x1, b);
    // must not ever rich here, just to satisfy compiler
    dispatch();
op_trace:
    b = vm->ip++;
    x1 = stack_pop(vm);
    vm->trace = (u8_t)x1.i64;
    dispatch();
op_alloc:
    b = vm->ip++;
    c = code[vm->ip++];
    addr = stack_peek_n(vm, c - 1);
    l = addr->adt->len;
    // allocate result and write to a preserved space on the stack
    x1 = vector(addr->type, l);
    x1.adt->len = 0;
    *stack_peek_n(vm, c) = x1;
    // push counter
    stack_push(vm, i64(l));
    dispatch();
op_map:
    b = vm->ip++;
    // arguments count
    c = code[vm->ip++];
    l = stack_peek_n(vm, c)->adt->len;
    j = 0;
    // push arguments
    for (i = c - 1; i >= 0; i--)
    {
        addr = stack_peek_n(vm, i + j++);
        if (addr->type > 0)
            stack_push(vm, vector_get(addr, l));
        else
            stack_push(vm, *addr);
    }
    dispatch();
op_collect:
    b = vm->ip++;
    // arguments count
    c = code[vm->ip++];
    // get the result from previous iteration
    x1 = stack_pop(vm);
    // load result
    addr = stack_peek_n(vm, c);
    l = addr->adt->len++;
    // store result
    vector_write(addr, l, x1);
    // load first argument len
    p = stack_peek_n(vm, c - 1)->adt->len;
    // push counter
    stack_push(vm, i64(p - l - 1));
    dispatch();
op_eval:
    b = vm->ip++;
    x1 = stack_pop(vm);
    if (x1.type != TYPE_LIST)
    {
        rf_object_free(&x1);
        unwrap(error(ERR_TYPE, "eval: expects list"), b);
    }
    x2 = cc_compile_lambda(false, "anonymous", vector_symbol(0), as_list(&x1), x1.id, x1.adt->len, NULL);
    rf_object_free(&x1);
    unwrap(x2, b);
    stack_push(vm, x2);
    n = 0;
    goto made_calld;
op_fload:
    b = vm->ip++;
    x1 = stack_pop(vm);
    x2 = rf_read_parse_compile(&x1);
    rf_object_free(&x1);
    unwrap(x2, b);
    stack_push(vm, x2);
    n = 0;
    goto made_calld;
}

null_t vm_free(vm_t *vm)
{
    // clear stack (if any)
    while (vm->sp)
        stack_pop_free(vm);
    mmap_free(vm->stack, VM_STACK_SIZE);
}

/*
 * Format code object in a user readable form for debugging
 */
str_t vm_code_fmt(rf_object_t *fun)
{
    lambda_t *f = as_lambda(fun);
    str_t code = as_string(&f->code);
    i32_t c = 0, l = 0, o = 0;
    u32_t ip = 0, len = (u32_t)f->code.adt->len;
    str_t s = NULL;

    while (ip < len)
    {
        switch (code[ip])
        {
        case OP_HALT:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] halt\n", c++, ip++);
            break;
        case OP_RET:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] ret [%d:%d]\n", c++, ip, code[ip + 1], code[ip + 2]);
            ip += 3;
            break;
        case OP_PUSH:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] push ", c++, ip++);
            rf_object_fmt_into(&s, &l, &o, 0, 0, (rf_object_t *)(code + ip));
            str_fmt_into(&s, &l, &o, 0, "\n");
            ip += sizeof(rf_object_t);
            break;
        case OP_POP:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] pop\n", c++, ip++);
            break;
        case OP_JNE:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] jne ", c++, ip++);
            rf_object_fmt_into(&s, &l, &o, 0, 0, (rf_object_t *)(code + ip));
            str_fmt_into(&s, &l, &o, 0, "\n");
            ip += sizeof(rf_object_t);
            break;
        case OP_JMP:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] jmp ", c++, ip++);
            rf_object_fmt_into(&s, &l, &o, 0, 0, (rf_object_t *)(code + ip));
            str_fmt_into(&s, &l, &o, 0, "\n");
            ip += sizeof(rf_object_t);
            break;
        case OP_CALL1:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] call1 ", c++, ip++);
            break;
        case OP_TIMER_SET:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] timer_set\n", c++, ip++);
            break;
        case OP_TIMER_GET:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] timer_get\n", c++, ip++);
            break;
        case OP_STORE:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] store [%d]\n", c++, ip, (i32_t)((rf_object_t *)(code + ip + 1))->i64);
            ip += 1 + sizeof(rf_object_t);
            break;
        case OP_LOAD:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] load [%d]\n", c++, ip, (i32_t)((rf_object_t *)(code + ip + 1))->i64);
            ip += 1 + sizeof(rf_object_t);
            break;
        case OP_TRY:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] try ", c++, ip++);
            rf_object_fmt_into(&s, &l, &o, 0, 0, (rf_object_t *)(code + ip));
            str_fmt_into(&s, &l, &o, 0, "\n");
            ip += sizeof(rf_object_t);
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
        case OP_ALLOC:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] alloc\n", c++, ip++);
            break;
        case OP_MAP:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] map\n", c++, ip++);
            break;
        case OP_COLLECT:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] collect\n", c++, ip++);
            break;
        default:
            str_fmt_into(&s, &l, &o, 0, "%.4d: unknown %d\n", c++, ip++);
            break;
        }
    }

    return s;
}
