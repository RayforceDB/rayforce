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

#ifndef VM_H
#define VM_H

#include "storm.h"

#define VM_STACK_SIZE 4096

typedef enum vm_opcode_t
{
    VM_HALT = 0, // Halt the VM
    VM_PUSH,     // Push a value to the stack
    VM_POP,      // Pop a value from the stack
    VM_ADD,      // Add two values from the stack
    VM_SUB,      // Subtract two values from the stack
    VM_MUL,      // Multiply two values from the stack
} vm_opcode_t;

typedef struct vm_t
{
    i32_t ip;    // Instruction pointer
    i32_t sp;    // Stack pointer
    i8_t halted; // Halt flag
    i64_t regs[16];
    struct value_t stack[VM_STACK_SIZE];
} *vm_t;

vm_t vm_create();
null_t vm_exec(vm_t vm, u8_t *code);
null_t vm_free(vm_t vm);

// void vm_init(VM *vm, int *code, int code_size, int nglobals);
// void vm_print_instr(i16_t *code, int ip);
// void vm_print_stack(int *stack, int count);
// void vm_print_data(int *globals, int count);

#endif
