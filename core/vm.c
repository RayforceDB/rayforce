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
#include "cast.h"
#include "function.h"

#define stack_push(v, x) (v->stack[v->sp++] = x)
#define stack_pop(v) (v->stack[--v->sp])
#define stack_peek(v) (&v->stack[v->sp - 1])
#define stack_pop_free(v)             \
    {                                 \
        rf_object_t o = stack_pop(v); \
        rf_object_free(&o);           \
    }

typedef struct ctx_t
{
    function_t *addr;
    i32_t ip;
    i32_t bp;
} ctx_t;

CASSERT(sizeof(ctx_t) == sizeof(rf_object_t), vm_c)

vm_t *vm_new()
{
    vm_t *vm = (vm_t *)mmap(NULL, sizeof(struct vm_t),
                            PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_PRIVATE,
                            -1, 0);

    memset(vm, 0, sizeof(struct vm_t));

    vm->trace = 0;
    vm->acc = null();
    vm->stack = (rf_object_t *)mmap(NULL, VM_STACK_SIZE,
                                    PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_STACK | MAP_GROWSDOWN,
                                    -1, 0);

    return vm;
}

/*
 * Execute the function
 */
rf_object_t vm_exec(vm_t *vm, rf_object_t *fun)
{
    function_t *f = as_function(fun);
    str_t code = as_string(&f->code);
    rf_object_t x1, x2, x3, x4, x5, x6, *addr;
    i64_t t;
    i32_t i, l, b;
    nilary_t f0;
    unary_t f1;
    binary_t f2;
    ternary_t f3;
    quaternary_t f4;
    nary_t fn;
    ctx_t ctx;

    vm->ip = 0;
    vm->sp = 0;
    vm->bp = -1;

    // The indices of labels in the dispatch_table are the relevant opcodes
    static null_t *dispatch_table[] = {
        &&op_halt, &&op_ret, &&op_push, &&op_pop, &&op_jne, &&op_jmp, &&op_type, &&op_timer_set, &&op_timer_get,
        &&op_call0, &&op_call1, &&op_call2, &&op_call3, &&op_call4, &&op_calln, &&op_callf, &&op_lset, &&op_gset,
        &&op_lload, &&op_gload, &&op_cast, &&op_try, &&op_catch, &&op_throw, &&op_trace};

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

op_dispatch:
    dispatch();
op_halt:
    vm->halted = 1;
    if (vm->sp > 0)
        return stack_pop(vm);
    else
        return null();
op_ret:
    vm->ip++;
    x3 = stack_pop(vm); // return value
    l = (i32_t)as_list(&f->locals)[0].adt->len;
    for (i = 0; i < l; i++)
        stack_pop_free(vm); // pop locals
    x1 = stack_pop(vm);     // ctx
    l = (i32_t)as_list(&f->args)[0].adt->len;
    for (i = 0; i < l; i++)
        stack_pop_free(vm); // pop args
    ctx = *(ctx_t *)&x1;
    vm->ip = ctx.ip;
    vm->bp = ctx.bp;
    f = ctx.addr;
    code = as_string(&f->code);
    stack_push(vm, x3); // push back return value
    dispatch();
op_push:
    vm->ip++;
    x1 = rf_object_clone((rf_object_t *)(code + vm->ip));
    stack_push(vm, x1);
    vm->ip += sizeof(rf_object_t);
    dispatch();
op_pop:
    vm->ip++;
    stack_pop_free(vm);
    dispatch();
op_jne:
    vm->ip++;
    x2 = stack_pop(vm);
    x1 = *(rf_object_t *)(code + vm->ip);
    if (!x2.bool)
        vm->ip = (i32_t)x1.i64;
    else
        vm->ip += sizeof(rf_object_t);
    dispatch();
op_jmp:
    vm->ip++;
    x1 = *(rf_object_t *)(code + vm->ip);
    vm->ip = (i32_t)x1.i64;
    dispatch();
op_type:
    vm->ip++;
    x2 = stack_pop(vm);
    t = env_get_typename_by_type(&runtime_get()->env, x2.type);
    x1 = i64(t);
    x1.type = -TYPE_SYMBOL;
    stack_push(vm, x1);
    rf_object_free(&x2);
    dispatch();
op_timer_set:
    vm->ip++;
    vm->timer = clock();
    dispatch();
op_timer_get:
    vm->ip++;
    stack_push(vm, f64((((f64_t)(clock() - vm->timer)) / CLOCKS_PER_SEC) * 1000));
    dispatch();
op_call0:
    b = vm->ip++;
    x2 = *(rf_object_t *)(code + vm->ip);
    vm->ip += sizeof(rf_object_t);
    f0 = (nilary_t)x2.i64;
    x1 = f0();
    unwrap(x1, b);
    stack_push(vm, x1);
    dispatch();
op_call1:
    b = vm->ip++;
    x3 = *(rf_object_t *)(code + vm->ip);
    vm->ip += sizeof(rf_object_t);
    x2 = stack_pop(vm);
    f1 = (unary_t)x3.i64;
    x1 = f1(&x2);
    unwrap(x1, b);
    rf_object_free(&x2);
    stack_push(vm, x1);
    dispatch();
op_call2:
    b = vm->ip++;
    x4 = *(rf_object_t *)(code + vm->ip);
    vm->ip += sizeof(rf_object_t);
    x3 = stack_pop(vm);
    x2 = stack_pop(vm);
    f2 = (binary_t)x4.i64;
    x1 = f2(&x2, &x3);
    unwrap(x1, b);
    stack_push(vm, x1);
    rf_object_free(&x2);
    rf_object_free(&x3);
    dispatch();
op_call3:
    b = vm->ip++;
    x5 = *(rf_object_t *)(code + vm->ip);
    vm->ip += sizeof(rf_object_t);
    x4 = stack_pop(vm);
    x3 = stack_pop(vm);
    x2 = stack_pop(vm);
    f3 = (ternary_t)x5.i64;
    x1 = f3(&x2, &x3, &x4);
    rf_object_free(&x2);
    rf_object_free(&x3);
    rf_object_free(&x4);
    unwrap(x1, b);
    stack_push(vm, x1);
    dispatch();
op_call4:
    b = vm->ip++;
    x6 = *(rf_object_t *)(code + vm->ip);
    vm->ip += sizeof(rf_object_t);
    x5 = stack_pop(vm);
    x4 = stack_pop(vm);
    x3 = stack_pop(vm);
    x2 = stack_pop(vm);
    f4 = (quaternary_t)x6.i64;
    x1 = f4(&x2, &x3, &x4, &x5);
    rf_object_free(&x2);
    rf_object_free(&x3);
    rf_object_free(&x4);
    rf_object_free(&x5);
    unwrap(x1, b);
    stack_push(vm, x1);
    dispatch();
op_calln:
    b = vm->ip++;
    l = code[vm->ip++];
    x2 = *(rf_object_t *)(code + vm->ip);
    vm->ip += sizeof(rf_object_t);
    fn = (nary_t)x2.i64;
    addr = (rf_object_t *)(&vm->stack[vm->sp - l]);
    x1 = fn(addr, l);
    for (i = 0; i < l; i++)
        stack_pop_free(vm); // pop args
    unwrap(x1, b);
    stack_push(vm, x1);
    dispatch();
op_callf:
    /* Call stack of user function call looks as follows:
     * +-------------------+
     * |      localn       |
     * +-------------------+
     * |       ...         |
     * +-------------------+
     * |      local2       |
     * +-------------------+
     * |      local1       |
     * +-------------------+
     * | ctx {ret, ip, sp} | <- bp
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
    b = vm->ip++;
    x2 = *(rf_object_t *)(code + vm->ip);
    vm->ip += sizeof(rf_object_t);
    if ((vm->sp + f->stack_size) * sizeof(rf_object_t) > VM_STACK_SIZE)
        unwrap(error(ERR_STACK_OVERFLOW, "stack overflow"), b);
    // save ctx
    ctx = (ctx_t){.addr = f, .ip = vm->ip, .bp = vm->bp};
    memcpy(&x1, &ctx, sizeof(ctx_t));
    vm->ip = 0;
    vm->bp = vm->sp;
    stack_push(vm, x1);
    // --
    f = as_function(&x2);
    code = as_string(&f->code);
    vm->sp += (i32_t)as_list(&f->locals)[0].adt->len;
    dispatch();
op_lset:
    b = vm->ip++;
    t = ((rf_object_t *)(code + vm->ip))->i64;
    vm->ip += sizeof(rf_object_t);
    vm->stack[vm->bp + t] = rf_object_clone(stack_peek(vm));
    dispatch();
op_gset:
    b = vm->ip++;
    x2 = *(rf_object_t *)(code + vm->ip);
    vm->ip += sizeof(rf_object_t);
    env_set_variable(&runtime_get()->env, x2, rf_object_clone(stack_peek(vm)));
    dispatch();
op_lload:
    b = vm->ip++;
    t = ((rf_object_t *)(code + vm->ip))->i64;
    vm->ip += sizeof(rf_object_t);
    x1 = vm->stack[vm->bp + t];
    stack_push(vm, rf_object_clone(&x1));
    dispatch();
op_gload:
    b = vm->ip++;
    addr = (rf_object_t *)((rf_object_t *)(code + vm->ip))->i64;
    vm->ip += sizeof(rf_object_t);
    stack_push(vm, rf_object_clone(addr));
    dispatch();
op_cast:
    b = vm->ip++;
    i = code[vm->ip++];
    x2 = stack_pop(vm);
    x1 = rf_cast(i, &x2);
    unwrap(x1, b);
    stack_push(vm, x1);
    rf_object_free(&x2);
    dispatch();
op_try:
    b = vm->ip++;
    t = ((rf_object_t *)(code + vm->ip))->i64;
    vm->ip += sizeof(rf_object_t);
    // save ctx
    ctx = (ctx_t){.addr = NULL, .ip = (i32_t)t, .bp = vm->bp};
    memcpy(&x1, &ctx, sizeof(ctx_t));
    vm->bp = vm->sp;
    stack_push(vm, x1);
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
}

null_t vm_free(vm_t *vm)
{
    munmap(vm->stack, VM_STACK_SIZE);
    munmap(vm, sizeof(struct vm_t));
}

/*
 * Format code object in a user readable form for debugging
 */
str_t vm_code_fmt(rf_object_t *fun)
{
    function_t *f = as_function(fun);
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
        case OP_TYPE:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] type\n", c++, ip++);
            break;
        case OP_TIMER_SET:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] timer_set\n", c++, ip++);
            break;
        case OP_TIMER_GET:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] timer_get\n", c++, ip++);
            break;
        case OP_CALL0:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] call0 ", c++, ip++);
            rf_object_fmt_into(&s, &l, &o, 0, 0, (rf_object_t *)(code + ip));
            str_fmt_into(&s, &l, &o, 0, "\n");
            ip += sizeof(rf_object_t);
            break;
        case OP_CALL1:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] call1 ", c++, ip++);
            rf_object_fmt_into(&s, &l, &o, 0, 0, (rf_object_t *)(code + ip));
            str_fmt_into(&s, &l, &o, 0, "\n");
            ip += sizeof(rf_object_t);
            break;
        case OP_CALL2:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] call2 ", c++, ip++);
            rf_object_fmt_into(&s, &l, &o, 0, 0, (rf_object_t *)(code + ip));
            str_fmt_into(&s, &l, &o, 0, "\n");
            ip += sizeof(rf_object_t);
            break;
        case OP_CALL3:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] call3 ", c++, ip++);
            rf_object_fmt_into(&s, &l, &o, 0, 0, (rf_object_t *)(code + ip));
            str_fmt_into(&s, &l, &o, 0, "\n");
            ip += sizeof(rf_object_t);
            break;
        case OP_CALL4:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] call4 ", c++, ip++);
            rf_object_fmt_into(&s, &l, &o, 0, 0, (rf_object_t *)(code + ip));
            str_fmt_into(&s, &l, &o, 0, "\n");
            ip += sizeof(rf_object_t);
            break;
        case OP_CALLN:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] calln %d %p\n", c++, ip, code[ip + 1], ((rf_object_t *)(code + ip + 2))->i64);
            ip += 2 + sizeof(rf_object_t);
            break;
        case OP_CALLF:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] callf %d %p\n", c++, ip, code[ip + 1], ((rf_object_t *)(code + ip + 2))->i64);
            ip += 2 + sizeof(rf_object_t);
            break;
        case OP_LSET:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] lset [%d]\n", c++, ip, (i32_t)((rf_object_t *)(code + ip + 1))->i64);
            ip += 1 + sizeof(rf_object_t);
            break;
        case OP_GSET:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] gset %s\n", c++, ip, symbols_get(((rf_object_t *)(code + ip + 1))->i64));
            ip += 1 + sizeof(rf_object_t);
            break;
        case OP_LLOAD:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] lload [%d]\n", c++, ip, (i32_t)((rf_object_t *)(code + ip + 1))->i64);
            ip += 1 + sizeof(rf_object_t);
            break;
        case OP_GLOAD:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] gload %p\n", c++, ip, ((rf_object_t *)(code + ip + 1))->i64);
            ip += 1 + sizeof(rf_object_t);
            break;
        case OP_CAST:
            str_fmt_into(&s, &l, &o, 0, "%.4d: [%.4d] cast %d\n", c++, ip, code[ip + 1]);
            ip += 2;
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
        default:
            str_fmt_into(&s, &l, &o, 0, "%.4d: unknown %d\n", c++, ip++);
            break;
        }
    }

    return s;
}
