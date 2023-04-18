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

#include <time.h>
#include "rayforce.h"
#include "mmap.h"
#include "debuginfo.h"

#define VM_STACK_SIZE PAGE_SIZE * 4

typedef enum vm_opcode_t
{
    OP_HALT = 0,  // Halt the VM
    OP_PUSH,      // Push an rf_object to the stack
    OP_POP,       // Pop an rf_object from the stack
    OP_ADDI,      // Add two i64 from the stack
    OP_ADDF,      // Add two f64 from the stack
    OP_SUBI,      // Subtract two i64 from the stack
    OP_SUBF,      // Subtract two f64 from the stack
    OP_MULI,      // Multiply two i64 from the stack
    OP_MULF,      // Multiply two f64 from the stack
    OP_DIVI,      // Divide two i64 from the stack
    OP_DIVF,      // Divide two f64 from the stack
    OP_SUMI,      // Sum i64 vector elements with scalar i64
    OP_LIKE,      // Compare string with regex
    OP_TYPE,      // Get type of rf_object
    OP_TIMER_SET, // Start timer
    OP_TIMER_GET, // Get timer value
    OP_TIL,       // Create i64 vector of length n
    OP_CALL0,     // Call function with no arguments
    OP_CALL1,     // Call function with one argument
    OP_CALL2,     // Call function with two arguments
    OP_CALL3,     // Call function with three arguments
    OP_CALL4,     // Call function with four arguments
    OP_CALLN,     // Call function with n arguments
    OP_SET,       // Set global variable
    OP_GET,       // Get global variable
    OP_CAST,      // Cast rf_object to another type

    OP_INVALID, // Invalid opcode
} vm_opcode_t;

CASSERT(OP_INVALID < 127, vm_h)

typedef struct vm_t
{
    i32_t ip;           // Instruction pointer
    i32_t sp;           // Stack pointer
    i8_t halted;        // Halt flag
    rf_object_t r[8];   // Registers of rf_objects
    clock_t timer;      // Timer for execution time
    rf_object_t *stack; // Stack of rf_objects
} vm_t;

vm_t *vm_new();
rf_object_t vm_exec(vm_t *vm, str_t code, debuginfo_t *debuginfo) __attribute__((__noinline__));
null_t vm_free(vm_t *vm);

// void vm_print_stack(int *stack, int count);

#endif
