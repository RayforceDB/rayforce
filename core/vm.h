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

#include "rayforce.h"
#include "mmap.h"

#define VM_STACK_SIZE PAGE_SIZE * 4

typedef enum vm_opcode_t
{
    OP_HALT = 0, // Halt the VM
    OP_PUSH,     // Push an object to the stack
    OP_POP,      // Pop an object from the stack
    OP_ADDI,     // Add two i64 from the stack
    OP_ADDF,     // Add two f64 from the stack
    // OP_SUB,      // Subtract two objects from the stack
    // OP_MUL,      // Multiply two objects from the stack
    OP_SUMI, // Sum i64 vector elements with scalar i64
} vm_opcode_t;

typedef struct vm_t
{
    i32_t ip;    // Instruction pointer
    i32_t sp;    // Stack pointer
    i8_t halted; // Halt flag
    rf_object_t r0, r1, r2, r3, r4,
        r5, r6, r7, r8; // Registers of objects
    rf_object_t *stack; // Stack of objects
} vm_t;

vm_t *vm_create();
rf_object_t vm_exec(vm_t *vm, str_t code) __attribute__((__noinline__));
null_t vm_free(vm_t *vm);

// void vm_print_stack(int *stack, int count);

#endif
