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

#define VM_STACK_SIZE 1024

// Bytecode opcodes
typedef enum op_type_t {
    OP_RET = 0,  // Return from function
    OP_PUSHC,    // Push constant
    OP_DUP,      // Duplicate stack value at offset
    OP_POP,      // Pop and discard
    OP_JMPZ,     // Jump if zero/false
    OP_JMP,      // Unconditional jump
    OP_DEREF,    // Dereference symbol
    OP_CALF1,    // Call unary function
    OP_CALF2,    // Call binary function
    OP_CALF0,    // Call variadic function
    OP_CALFN,    // Call lambda function
    OP_CALFS,    // Call self (recursive)
    OP_CALFD,    // Call dynamic (resolved at runtime)
} op_type_t;

// Forward declarations
struct pool_t;
struct heap_t;

// Return stack frame
typedef struct ctx_t {
    obj_p fn;   // function pointer
    obj_p env;  // local environment (for closures, future use)
    i64_t fp;   // frame pointer
    i64_t ip;   // instruction pointer
} ctx_t;

// Virtual machine state
typedef struct vm_t {
    obj_p fn;                                              // current function pointer
    obj_p env;                                             // current environment frame
    obj_p nfo;                                             // current source nfo for error reporting
    obj_p last_err;                                        // last error (glibc-like errno)
    obj_p last_locs;                                       // last error locations
    i64_t id;                                              // VM id
    i64_t fp;                                              // frame pointer
    i64_t sp;                                              // program stack pointer
    i64_t rp;                                              // return stack pointer
    heap_p heap;                                           // heap pointer
    struct pool_t *pool;                                   // pool pointer
    timeit_t timeit;                                       // Timeit spans
    obj_p ps[VM_STACK_SIZE] __attribute__((aligned(32)));  // program stack
    ctx_t rs[VM_STACK_SIZE] __attribute__((aligned(32)));  // return stack
} __attribute__((aligned(32))) * vm_p;

// Thread-local VM pointer
extern __thread vm_p __VM;

// VM lifecycle
vm_p vm_create(i64_t id, struct pool_t *pool);
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
obj_p mount_env(obj_p obj);
obj_p unmount_env(i64_t n);

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

// Macros for stack operations (for compatibility with existing code)
#define stack_push(val) vm_stack_push(val)
#define stack_pop() vm_stack_pop()
#define stack_peek(n) vm_stack_peek(n)
#define stack_enough(n) vm_stack_enough(n)

#endif  // EVAL_H
