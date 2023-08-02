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
#include "lambda.h"
#include "mmap.h"
#include "nfo.h"

#define VM_STACK_SIZE PAGE_SIZE * 4
#define VM_STACK_STUB 0xFACE1E5500000000
#define VM_SPILL_REGS 6

typedef enum vm_opcode_t
{
    OP_HALT = 0,   // Halt the VM
    OP_PUSH_CONST, // Push an obj_t to the stack
    OP_PUSH_ACC,   // Push an accumulator to the stack
    OP_POP,        // Pop an obj_t from the stack
    OP_SWAP,       // Swap two obj_ts on the stack
    OP_DUP,        // Duplicate an obj_t on the stack
    OP_CMP,        // Get top of stack, interpret it as bool and push result into cmp register
    OP_JNE,        // Jump if not equal
    OP_JMP,        // Jump
    OP_CALL1,      // Call unary
    OP_CALL2,      // Call binary
    OP_CALLN,      // Call vary
    OP_CALLD,      // Dynamic call (call function from stack with n arguments)
    OP_RET,        // Return from lambda
    OP_TIMER_SET,  // Start timer
    OP_TIMER_GET,  // Get timer value
    OP_STORE,      // Store value somewhere in a stack pointed by argument
    OP_LOAD,       // Load value from somewhere in a stack pointed by argument
    OP_LSET,       // Set local variable
    OP_LGET,       // Get local variable
    OP_LPUSH,      // Attach dict frame to local variables
    OP_LPOP,       // Detach dict frame from local variables
    OP_TRY,        // Trap an expression to return here on error
    OP_CATCH,      // Catch an error from vm register and push it onto the stack
    OP_THROW,      // Throw an error
    OP_TRACE,      // Print stack trace (limit)
    OP_ALLOC,      // Allocate obj_t
    OP_MAP,        // Map lambda over array
    OP_COLLECT,    // Collect array of results
    OP_EVAL,       // Compile/evaluate list of expressions as a lambda
    OP_FLOAD,      // Load source file
    OP_DEBUG,      // Print msg for debugging

    OP_INVALID, // Invalid opcode
} vm_opcode_t;

typedef struct vm_t
{
    i8_t halted;  // Halt flag
    bool_t cmp;   // Compare result flag
    u8_t trace;   // Trace flag (print stack trace on error limited to n frames)
    i64_t ip;     // Instruction pointer
    i64_t sp;     // Stack pointer
    i64_t bp;     // Base pointer (beginning on stack frame)
    i64_t cnt;    // Counter
    obj_t acc;    // Accumulator
    i64_t timer;  // Timer for execution time
    obj_t *stack; // Stack of arguments
} vm_t;

vm_t vm_new();
obj_t vm_exec(vm_t *vm, obj_t fun) __attribute__((__noinline__));
nil_t vm_free(vm_t *vm);
str_t vm_code_fmt(obj_t fun);

#endif
