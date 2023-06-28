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
#include "function.h"
#include "mmap.h"
#include "debuginfo.h"

#define VM_STACK_SIZE PAGE_SIZE * 4

typedef enum vm_opcode_t
{
    OP_HALT = 0,  // Halt the VM
    OP_RET,       // Return from function
    OP_PUSH,      // Push an rf_object to the stack
    OP_POP,       // Pop an rf_object from the stack
    OP_JNE,       // Jump if not equal
    OP_JMP,       // Jump
    OP_TIMER_SET, // Start timer
    OP_TIMER_GET, // Get timer value
    OP_CALL0,     // Call function with no arguments
    OP_CALL1,     // Call function with one argument
    OP_CALL2,     // Call function with two arguments
    OP_CALL3,     // Call function with three arguments
    OP_CALL4,     // Call function with four arguments
    OP_CALLN,     // Call function with n arguments
    OP_CALLF,     // Call user function
    OP_STORE,     // Store value somewhere in a stack pointed by argument
    OP_LOAD,      // Load value from somewhere in a stack pointed by argument
    OP_LSET,      // Set local variable
    OP_LGET,      // Get local variable
    OP_CAST,      // Cast rf_object to another type
    OP_TRY,       // Trap an expression to return here on error
    OP_CATCH,     // Catch an error from vm register and push it onto the stack
    OP_THROW,     // Throw an error
    OP_TRACE,     // Print stack trace (limit)
    OP_ALLOC,     // Allocate rf_object
    OP_MAP,       // Map function over array
    OP_COLLECT,   // Collect array of results

    OP_INVALID, // Invalid opcode
} vm_opcode_t;

CASSERT(OP_INVALID < 127, vm_h)

typedef struct vm_t
{
    i8_t halted;        // Halt flag
    u8_t trace;         // Trace flag (print stack trace on error limited to n frames)
    i32_t ip;           // Instruction pointer
    i32_t sp;           // Stack pointer
    i32_t bp;           // Base pointer (beginning on stack frame)
    i64_t timer;        // Timer for execution time
    i64_t counter;      // Iteration counter
    rf_object_t acc;    // Accumulator
    rf_object_t *stack; // Stack of arguments
} vm_t;

vm_t *vm_new();
rf_object_t vm_exec(vm_t *vm, rf_object_t *fun) __attribute__((__noinline__));
null_t vm_free(vm_t *vm);
str_t vm_code_fmt(rf_object_t *fun);

#endif
