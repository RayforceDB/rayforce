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
#include "storm.h"
#include "alloc.h"

vm_t vm_create()
{
    vm_t vm;

    vm = (vm_t)storm_malloc(sizeof(struct vm_t));
    memset(vm, 0, sizeof(struct vm_t));
    return vm;
}

null_t vm_exec(vm_t vm, u8_t *code)
{
    // i8_t *stack = vm->stack;
    // i64_t *regs = vm->regs;
    i32_t *ip = &vm->ip;
    // i32_t *sp = &vm->sp;

    while (!vm->halted)
    {
        switch (code[*ip++])
        {
        case VM_HALT:
            printf("HALT");
            vm->halted = 1;
            break;
        // case VM_PUSH:
        //     stack[*sp++] = regs[code[*ip++]];
        //     break;
        // case VM_POP:
        //     regs[code[*ip++]] = stack[--*sp];
        //     break;
        // case VM_ADD:
        //     regs[code[*ip++]] = stack[--(*sp)] + stack[--*sp];
        //     break;
        // case VM_SUB:
        //     regs[code[*ip++]] = stack[--*sp] - stack[--*sp];
        //     break;
        // case VM_MUL:
        //     regs[code[*ip++]] = stack[--*sp] * stack[--*sp];
        //     break;
        default:
            return;
        }
    }
}

null_t vm_free(vm_t vm)
{
    storm_free(vm);
}

// str_t vm_code_fmt(u8_t *code)
// {
//     // TODO
// }
