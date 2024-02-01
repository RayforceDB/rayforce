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
#include <time.h>
#include "rayforce.h"
#include "lambda.h"
#include "mmap.h"
#include "nfo.h"

#define EVAL_STACK_SIZE 1024

typedef struct ctx_t
{
    i64_t sp;     // Stack pointer.
    obj_t lambda; // Lambda being evaluated.
    jmp_buf jmp;  // Jump buffer.
} ctx_t;

typedef struct interpreter_t
{
    i64_t sp;        // Stack pointer.
    obj_t *stack;    // Stack.
    i64_t cp;        // Context pointer.
    ctx_t *ctxstack; // Stack of contexts.
} *interpreter_t;

interpreter_t interpreter_new();
nil_t interpreter_free();
obj_t call(obj_t obj, u64_t arity);
obj_t *deref(obj_t sym);
obj_t amend(obj_t sym, obj_t val);
obj_t mount_env(obj_t obj);
obj_t unmount_env(u64_t n);
obj_t eval(obj_t obj);
obj_t ray_raise(obj_t obj);
obj_t try(obj_t obj, obj_t catch);
obj_t ray_return(obj_t *x, u64_t n);
nil_t stack_push(obj_t val);
obj_t stack_pop();
obj_t *stack_peek(i64_t n);
obj_t stack_at(i64_t n);
bool_t stack_enough(u64_t n);
obj_t unwrap(obj_t obj, i64_t id);
ctx_t *ctx_push(obj_t lambda);
ctx_t *ctx_pop();

#endif // EVAL_H
