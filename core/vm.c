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

#define stack_push(v, x) (v->stack[v->sp++] = x)
#define stack_pop(v) (v->stack[--v->sp])
#define stack_peek(v) (&v->stack[v->sp - 1])

vm_t *
vm_new()
{
    vm_t *vm;

    vm = (vm_t *)rf_malloc(sizeof(struct vm_t));
    memset(vm, 0, sizeof(struct vm_t));

    vm->stack = (rf_object_t *)mmap(NULL, VM_STACK_SIZE,
                                    PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_STACK | MAP_GROWSDOWN,
                                    -1, 0);

    return vm;
}

/*
 * Dispatch using computed goto technique
 */
rf_object_t vm_exec(vm_t *vm, str_t code, debuginfo_t *debuginfo)
{
    rf_object_t x1, x2, x3, x4, x5, x6, *addr;
    i64_t *v, t;
    i32_t i, l, b;
    nilary_t f0;
    unary_t f1;
    binary_t f2;
    ternary_t f3;
    quaternary_t f4;
    nary_t fn;

    vm->ip = 0;
    vm->sp = 0;

    // The indices of labels in the dispatch_table are the relevant opcodes
    static null_t *dispatch_table[] = {
        &&op_halt, &&op_push, &&op_pop, &&op_addi, &&op_addf, &&op_subi, &&op_subf,
        &&op_muli, &&op_mulf, &&op_divi, &&op_divf, &&op_sumi, &&op_like, &&op_type,
        &&op_timer_set, &&op_timer_get, &&op_til, &&op_call0, &&op_call1, &&op_call2,
        &&op_call3, &&op_call4, &&op_calln, &&op_set, &&op_get, &&op_cast};

#define dispatch() goto *dispatch_table[(i32_t)code[vm->ip]]

#define unwrap(x, y)                               \
    if (x.type == TYPE_ERROR)                      \
    {                                              \
        x.adt->span = debuginfo_get(debuginfo, y); \
        return x;                                  \
    }

    dispatch();

op_halt:
    vm->halted = 1;
    if (vm->sp > 0)
        return stack_pop(vm);
    else
        return null();
op_push:
    vm->ip++;
    x1 = *(rf_object_t *)(code + vm->ip);
    stack_push(vm, x1);
    vm->ip += sizeof(rf_object_t);
    dispatch();
op_pop:
    vm->ip++;
    x1 = stack_pop(vm);
    dispatch();
op_addi:
    vm->ip++;
    x3 = stack_pop(vm);
    x2 = stack_pop(vm);
    x1 = i64(ADDI64(x2.i64, x3.i64));
    stack_push(vm, x1);
    dispatch();
op_addf:
    vm->ip++;
    x1 = stack_pop(vm);
    stack_peek(vm)->f64 += x1.f64;
    dispatch();
op_subi:
    vm->ip++;
    x3 = stack_pop(vm);
    x2 = stack_pop(vm);
    x1 = i64(SUBI64(x2.i64, x3.i64));
    stack_push(vm, x1);
    dispatch();
op_subf:
    vm->ip++;
    x1 = stack_pop(vm);
    stack_peek(vm)->f64 -= x1.f64;
    dispatch();
op_muli:
    vm->ip++;
    x3 = stack_pop(vm);
    x2 = stack_pop(vm);
    x1 = i64(MULI64(x2.i64, x3.i64));
    stack_push(vm, x1);
    dispatch();
op_mulf:
    vm->ip++;
    x1 = stack_pop(vm);
    stack_peek(vm)->f64 *= x1.f64;
    dispatch();
op_divi:
    vm->ip++;
    x3 = stack_pop(vm);
    x2 = stack_pop(vm);
    x1 = f64(DIVI64(x2.i64, x3.i64));
    stack_push(vm, x1);
    dispatch();
op_divf:
    vm->ip++;
    x3 = stack_pop(vm);
    x2 = stack_pop(vm);
    x1 = f64(DIVF64(x2.f64, x3.f64));
    stack_push(vm, x1);
    dispatch();
op_sumi:
    vm->ip++;
    x3 = stack_pop(vm);
    x2 = stack_pop(vm);
    l = x2.adt->len;
    v = as_vector_i64(&x2);
    for (i = 0; i < l; i++)
        v[i] = ADDI64(v[i], x3.i64);
    stack_push(vm, x2);
    dispatch();
op_like:
    vm->ip++;
    x2 = stack_pop(vm);
    x1 = stack_pop(vm);
    stack_push(vm, i64(string_match(as_string(&x1), as_string(&x2))));
    dispatch();
op_type:
    vm->ip++;
    x2 = stack_pop(vm);
    t = env_get_typename_by_type(&runtime_get()->env, x2.type);
    x1 = i64(t);
    x1.type = -TYPE_SYMBOL;
    stack_push(vm, x1);
    dispatch();
op_timer_set:
    vm->ip++;
    vm->timer = clock();
    dispatch();
op_timer_get:
    vm->ip++;
    stack_push(vm, f64((((f64_t)(clock() - vm->timer)) / CLOCKS_PER_SEC) * 1000));
    dispatch();
op_til:
    vm->ip++;
    x2 = stack_pop(vm);
    x1 = vector_i64(x2.i64);
    v = as_vector_i64(&x1);
    for (i = 0; i < x2.i64; i++)
        v[i] = i;
    x1.id = x2.id;
    stack_push(vm, x1);
    dispatch();
op_call0:
    b = vm->ip++;
    x2 = *(rf_object_t *)(code + vm->ip);
    vm->ip += sizeof(rf_object_t);
    f0 = (nilary_t)x2.i64;
    x1 = f0();
    // TODO: unwind
    if (x1.type == TYPE_ERROR)
        return x1;
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
    unwrap(x1, b);
    vm->sp -= l;
    stack_push(vm, x1);
    dispatch();
op_set:
    b = vm->ip++;
    x2 = *(rf_object_t *)(code + vm->ip);
    vm->ip += sizeof(rf_object_t);
    x1 = stack_pop(vm);
    env_set_variable(&runtime_get()->env, x2, rf_object_clone(&x1));
    stack_push(vm, x1);
    dispatch();
op_get:
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
    dispatch();
}

null_t vm_free(vm_t *vm)
{
    rf_free(vm);
}
