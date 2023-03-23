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

vm_t *vm_create()
{
    vm_t *vm;

    vm = (vm_t *)rayforce_malloc(sizeof(struct vm_t));
    memset(vm, 0, sizeof(struct vm_t));
    vm->stack = list(0);
    return vm;
}

rf_object_t vm_exec(vm_t *vm, str_t code)
{
    rf_object_t ob;
    vm->ip = vm->sp = 0;

    // The indices of labels in the dispatch_table are the relevant opcodes
    static null_t *dispatch_table[] = {
        &&vm_halt, &&vm_push, &&vm_pop};

#define dispatch() goto *dispatch_table[(i32_t)code[vm->ip]]

    dispatch();

vm_halt:
    vm->halted = 1;
    ob = vector_pop(&vm->stack);
    return ob;
vm_push:
    vm->ip++;
    ob = *(rf_object_t *)(code + vm->ip);
    vector_push(&vm->stack, ob);
    vm->ip += sizeof(rf_object_t);
    dispatch();
vm_pop:
    vm->ip++;
    ob = vector_pop(&vm->stack);
    dispatch();
}

null_t vm_free(vm_t *vm)
{
    rayforce_free(vm);
}

// str_t vm_code_fmt(i8_t *code)
// {
//     // TODO
// }
