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

#include <setjmp.h>
#include "time.h"
#include "rayforce.h"
#include "lambda.h"
#include "mmap.h"
#include "nfo.h"
#include "util.h"
#include "heap.h"

#define EVAL_STACK_SIZE 1024

typedef struct ctx_t {
    i64_t sp;      // Stack pointer.
    obj_p lambda;  // Lambda being evaluated.
    jmp_buf jmp;   // Jump buffer.
} *ctx_p;

typedef struct interpreter_t {
    u64_t id;         // Interpreter id.
    i64_t sp;         // Stack pointer.
    obj_p *stack;     // Stack.
    i64_t cp;         // Context pointer.
    ctx_p ctxstack;   // Stack of contexts.
    timeit_t timeit;  // Timeit spans.
} *interpreter_p;

extern __thread interpreter_p __INTERPRETER;

interpreter_p interpreter_create(u64_t id);
nil_t interpreter_destroy(nil_t);
interpreter_p interpreter_current(nil_t);
obj_p call(obj_p obj, u64_t arity);
obj_p *resolve(i64_t sym);
obj_p amend(obj_p sym, obj_p val);
obj_p mount_env(obj_p obj);
obj_p unmount_env(u64_t n);
obj_p eval(obj_p obj);
obj_p ray_parse_str(i64_t fd, obj_p str, obj_p file);
obj_p ray_eval_str(obj_p str, obj_p file);
obj_p ray_raise(obj_p obj);
obj_p try_eval(obj_p obj, obj_p ctch);
obj_p ray_return(obj_p *x, u64_t n);
obj_p interpreter_env_get(nil_t);
nil_t error_add_loc(obj_p err, i64_t id, ctx_p ctx);
// TODO: replace with correct functions
nil_t interpreter_env_set(interpreter_p interpreter, obj_p env);
nil_t interpreter_env_unset(interpreter_p interpreter);

inline __attribute__((always_inline)) nil_t stack_push(obj_p val) { __INTERPRETER->stack[__INTERPRETER->sp++] = val; }

inline __attribute__((always_inline)) obj_p stack_pop(nil_t) { return __INTERPRETER->stack[--__INTERPRETER->sp]; }

inline __attribute__((always_inline)) obj_p *stack_peek(i64_t n) {
    return &__INTERPRETER->stack[__INTERPRETER->sp - n - 1];
}

inline __attribute__((always_inline)) obj_p stack_at(i64_t n) {
    return __INTERPRETER->stack[__INTERPRETER->sp - n - 1];
}

inline __attribute__((always_inline)) ctx_p ctx_push(obj_p lambda) {
    ctx_p ctx = &__INTERPRETER->ctxstack[__INTERPRETER->cp++];
    ctx->lambda = lambda;
    return ctx;
}

inline __attribute__((always_inline)) ctx_p ctx_pop(nil_t) { return &__INTERPRETER->ctxstack[--__INTERPRETER->cp]; }

inline __attribute__((always_inline)) ctx_p ctx_get(nil_t) { return &__INTERPRETER->ctxstack[__INTERPRETER->cp - 1]; }

inline __attribute__((always_inline)) ctx_p ctx_top(obj_p info) {
    ctx_p ctx;
    i64_t sp;

    sp = __INTERPRETER->sp;
    stack_push(NULL_OBJ);
    ctx = ctx_get();
    AS_LAMBDA(ctx->lambda)->nfo = info;
    ctx->sp = sp;

    return ctx;
}

inline __attribute__((always_inline)) obj_p unwrap(obj_p obj, i64_t id) {
    if (IS_ERROR(obj))
        error_add_loc(obj, id, ctx_get());

    return obj;
}

inline __attribute__((always_inline)) b8_t stack_enough(u64_t n) { return __INTERPRETER->sp + n < EVAL_STACK_SIZE; }

#endif  // EVAL_H
