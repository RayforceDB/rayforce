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

#ifndef EVAL_H
#define EVAL_H

#include "chrono.h"
#include "rayforce.h"
#include "lambda.h"
#include "mmap.h"
#include "nfo.h"
#include "util.h"
#include "heap.h"

// Forward declaration for query context (defined in query.h)
struct query_ctx_t;

#define VM_STACK_SIZE 1024

// Bytecode opcodes - grouped by category
typedef enum op_type_t {
    // === Control Flow ===
    OP_RET = 0,  // Return from function
    OP_JMP,      // Unconditional jump (operand: offset)
    OP_JMPF,     // Jump if false/zero (operand: offset)

    // === Data Operations ===
    OP_LOADCONST,  // Load constant by offset from consts pool (no name)
    OP_LOADENV,    // Load from env by offset (args + locals, resolvable by name)
    OP_STOREENV,   // Store to env by offset (for let bindings)
    OP_POP,        // Pop and discard top of stack

    // === Symbol Resolution ===
    OP_RESOLVE,  // Resolve symbol dynamically (globals, parent scopes)

    // === Function Calls ===
    OP_CALL1,  // Call unary function (1 arg on stack, then fn)
    OP_CALL2,  // Call binary function (2 args on stack, then fn)
    OP_CALLN,  // Call variadic function (operand: arity)
    OP_CALLF,  // Call lambda function
    OP_CALLS,  // Call self (recursive tail call)
    OP_CALLD,  // Call dynamic - runtime dispatch (operand: arity)
} op_type_t;

// Forward declarations
struct pool_t;
struct heap_t;

// Return stack frame - compact for cache efficiency (16 bytes)
typedef struct ctx_t {
    obj_p fn;  // function pointer (lambda->env has the environment)
    i32_t fp;  // frame pointer (max VM_STACK_SIZE = 1024)
    i32_t ip;  // instruction pointer (max bytecode ~64KB)
} ctx_t;

// Virtual machine state - layout optimized for cache efficiency
typedef struct vm_t {
    // === First cache line (64 bytes) - VERY HOT ===
    // Registers (16 bytes) - compact i32 for cache efficiency
    i32_t sp;  // stack pointer (max VM_STACK_SIZE = 1024)
    i32_t fp;  // frame pointer
    i32_t rp;  // return stack pointer
    i32_t id;  // VM id (thread index)
    // Pointers (32 bytes)
    obj_p fn;             // current function (fn->env has local variables)
    obj_p env;            // query/dynamic env (table column mounting, NOT for lambda locals)
    heap_p heap;          // heap pointer
    struct pool_t *pool;  // pool pointer
    // === 48 bytes total - fits in first cache line with room to spare ===
    // === Stacks - HOT ===
    obj_p ps[VM_STACK_SIZE] __attribute__((aligned(64)));  // program stack
    ctx_t rs[VM_STACK_SIZE] __attribute__((aligned(64)));  // return stack
    // === COLD section ===
    obj_p nfo;                      // error source info
    obj_p trace;                    // error stack trace
    timeit_t *timeit;               // timeit (lazy allocated)
    struct query_ctx_t *query_ctx;  // query context stack (for table column resolution)
    b8_t rc_sync;                   // use atomic RC (multi-threaded mode)
} __attribute__((aligned(64))) * vm_p;

// Thread-local VM pointer
extern __thread vm_p __VM;

// VM lifecycle
vm_p vm_create(i32_t id, struct pool_t *pool);
nil_t vm_destroy(vm_p vm);

// Fast access to current VM - macro for zero overhead
#define VM (__VM)

// Stack operations (inlined for performance) - use __VM directly
inline __attribute__((always_inline)) nil_t vm_stack_push(obj_p val) { __VM->ps[__VM->sp++] = val; }
inline __attribute__((always_inline)) obj_p vm_stack_pop(nil_t) { return __VM->ps[--__VM->sp]; }
inline __attribute__((always_inline)) obj_p vm_stack_at(i64_t n) { return __VM->ps[__VM->sp - n - 1]; }
inline __attribute__((always_inline)) obj_p *vm_stack_peek(i64_t n) { return &__VM->ps[__VM->sp - n - 1]; }
inline __attribute__((always_inline)) b8_t vm_stack_enough(i64_t n) { return __VM->sp + n < VM_STACK_SIZE; }

// Evaluation functions
obj_p eval(obj_p obj);               // Recursive tree-walking eval
obj_p vm_eval(obj_p lambda);         // Bytecode interpreter
obj_p call(obj_p obj, i64_t arity);  // Call a compiled lambda

// Symbol resolution
obj_p *resolve(i64_t sym);
obj_p amend(obj_p sym, obj_p val);

// Environment management
obj_p vm_env_get(nil_t);
nil_t vm_env_set(vm_p vm, obj_p env);
nil_t vm_env_unset(vm_p vm);

// Special forms
obj_p ray_parse_str(i64_t fd, obj_p str, obj_p file);
obj_p ray_eval_str(obj_p str, obj_p file);
obj_p ray_raise(obj_p obj);
obj_p ray_return(obj_p *x, i64_t n);
obj_p ray_exit(obj_p *x, i64_t n);
obj_p ray_cond(obj_p *x, i64_t n);

// Error handling
nil_t error_add_loc(obj_p err, i64_t id, ctx_t *ctx);

// Thread utilities
b8_t ray_is_main_thread(nil_t);

#endif  // EVAL_H
