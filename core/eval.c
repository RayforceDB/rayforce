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

#include "eval.h"
#include "runtime.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "lambda.h"
#include "error.h"
#include "filter.h"
#include "string.h"
#include "aggr.h"
#include "cc.h"
#include "env.h"

// Thread-local VM pointer
__thread vm_p __VM = NULL;

vm_p vm_create(i32_t id, struct pool_t *pool) {
    vm_p vm;

    // Use raw mmap for VM allocation (can't use heap before VM exists)
    vm = (vm_p)mmap_alloc(sizeof(struct vm_t));
    // Hot fields first
    vm->sp = 0;
    vm->fp = 0;
    vm->rp = 0;
    vm->fn = NULL_OBJ;
    vm->id = id;
    vm->pool = pool;
    // Cold fields
    vm->nfo = NULL_OBJ;
    vm->trace = NULL_OBJ;
    vm->timeit = NULL;     // Lazy allocated when timing enabled
    vm->query_ctx = NULL;  // No active query context
    vm->rc_sync = 0;       // Single-threaded by default

    // Set VM for this thread so heap_create can use it
    __VM = vm;

    // Create heap for this VM
    vm->heap = heap_create(id);

    return vm;
}

nil_t vm_destroy(vm_p vm) {
    // Clear any remaining items on the stack
    while (vm->sp > 0)
        drop_obj(vm->ps[--vm->sp]);

    // Free timeit if allocated
    if (vm->timeit) {
        heap_free(vm->timeit);
        vm->timeit = NULL;
    }

    // Destroy heap
    heap_destroy(vm->heap);
    vm->heap = NULL;

    // Clear thread-local
    __VM = NULL;

    // Then destroy VM
    mmap_free(vm, sizeof(struct vm_t));
}

// ============================================================================
// Symbol Resolution
// ============================================================================

// Helper: resolve symbol in a lambda's env (unified args + locals)
// Returns pointer to value slot, or NULL if not found
// - offset < nargs: return stack pointer at fp_offset + offset
// - offset >= nargs: return env->values pointer
static obj_p *resolve_in_env(obj_p fn, i64_t sym, i64_t fp_offset) {
    obj_p env;
    i64_t i, n;
    vm_p vm = VM;

    if (fn == NULL_OBJ)
        return NULL;

    // First check args (always on stack at fp + offset)
    if (AS_LAMBDA(fn)->args != NULL_OBJ) {
        n = AS_LAMBDA(fn)->args->len;
        i64_t *args = AS_SYMBOL(AS_LAMBDA(fn)->args);
        for (i = 0; i < n; i++) {
            if (args[i] == sym)
                return &vm->ps[fp_offset + i];
        }
    }

    // Then check let-bound locals in env
    env = AS_LAMBDA(fn)->env;
    if (env != NULL_OBJ && env->type == TYPE_DICT) {
        n = AS_LIST(env)[0]->len;
        for (i = 0; i < n; i++) {
            if (AS_SYMBOL(AS_LIST(env)[0])[i] == sym) {
                obj_p values = AS_LIST(env)[1];
                if (i < values->len) {
                    return &AS_LIST(values)[i];
                }
                return NULL;  // Found in env but value not available
            }
        }
    }

    return NULL;
}

obj_p *resolve(i64_t sym) {
    i64_t j, frame;
    obj_p fn, *result;
    i64_t i, n;
    vm_p vm = VM;

    fn = vm->fn;

    if (sym == SYMBOL_SELF)
        return &vm->fn;

    // Search in current lambda's env (args + let-bound locals)
    result = resolve_in_env(fn, sym, vm->fp);
    if (result != NULL)
        return result;

    // Walk up return stack - check each parent lambda's env
    for (frame = vm->rp; frame > 0; frame--) {
        fn = vm->rs[frame - 1].fn;
        result = resolve_in_env(fn, sym, vm->rs[frame - 1].fp);
        if (result != NULL)
            return result;
    }

    // Search in query context stack (for table column access)
    for (query_ctx_p ctx = vm->query_ctx; ctx != NULL; ctx = ctx->parent) {
        if (ctx->table != NULL_OBJ) {
            obj_p cols = AS_LIST(ctx->table)[0];  // column names (symbol vector)
            n = cols->len;
            for (i = 0; i < n; i++) {
                if (AS_SYMBOL(cols)[i] == sym)
                    return &AS_LIST(AS_LIST(ctx->table)[1])[i];
            }
        }
    }

    // Search in globals
    j = find_raw(AS_LIST(runtime_get()->env.variables)[0], &sym);
    if (j == NULL_I64)
        return NULL;

    return &AS_LIST(AS_LIST(runtime_get()->env.variables)[1])[j];
}

obj_p amend(obj_p sym, obj_p val) {
    obj_p *env_slot;
    vm_p vm = VM;

    // Add to current lambda's env, or globals if at top level
    if (vm->fn != NULL_OBJ) {
        env_slot = &AS_LAMBDA(vm->fn)->env;
        if (*env_slot != NULL_OBJ)
            set_obj(env_slot, sym, clone_obj(val));
        else {
            *env_slot = dict(vector(TYPE_SYMBOL, 1), vn_list(1, clone_obj(val)));
            AS_SYMBOL(AS_LIST(*env_slot)[0])[0] = sym->i64;
        }
    } else {
        // Top level: add to globals
        set_obj(&runtime_get()->env.variables, sym, clone_obj(val));
    }

    return val;
}

// ============================================================================
// Error Handling
// ============================================================================

nil_t error_add_loc(obj_p err, i64_t id, ctx_t *ctx) {
    obj_p loc, nfo;
    span_t span;
    (void)err;

    if (ctx->fn == NULL_OBJ)
        return;

    nfo = AS_LAMBDA(ctx->fn)->nfo;

    if (nfo == NULL_OBJ)
        return;

    vm_p vm = VM;
    span = nfo_get(nfo, id);
    loc = vn_list(4, i64(span.id), clone_obj(AS_LIST(nfo)[0]), clone_obj(AS_LAMBDA(ctx->fn)->name),
                  clone_obj(AS_LIST(nfo)[1]));

    if (vm->trace == NULL_OBJ)
        vm->trace = vn_list(1, loc);
    else
        push_raw(&vm->trace, &loc);
}

// Add location info from bytecode debug info
nil_t bc_error_add_loc(obj_p err, obj_p fn, i64_t ip) {
    obj_p loc, nfo, filename, source;
    span_t span = {0};
    lambda_p lambda;
    (void)err;

    if (fn == NULL_OBJ || fn->type != TYPE_LAMBDA)
        return;

    lambda = AS_LAMBDA(fn);
    nfo = lambda->nfo;

    vm_p vm = VM;
    
    // Try to get span from bytecode debug info
    if (lambda->dbg != NULL_OBJ)
        span = bc_dbg_get(lambda->dbg, ip);

    // Get filename and source from nfo if available
    if (nfo != NULL_OBJ && nfo->type == TYPE_LIST && nfo->len >= 2) {
        filename = clone_obj(AS_LIST(nfo)[0]);
        source = clone_obj(AS_LIST(nfo)[1]);
    } else {
        // No nfo - create minimal info with just lambda name
        filename = string_from_str("<unknown>", 9);
        source = string_from_str("", 0);
    }
    
    // Create location frame - even without precise span, show the call chain
    loc = vn_list(4, i64(span.id), filename, clone_obj(lambda->name), source);

    if (vm->trace == NULL_OBJ)
        vm->trace = vn_list(1, loc);
    else
        push_raw(&vm->trace, &loc);
}

// Add location info for tree-walking eval using vm->nfo
nil_t eval_error_add_loc(obj_p expr) {
    obj_p loc, nfo;
    span_t span;
    vm_p vm = VM;

    if (vm == NULL || vm->nfo == NULL_OBJ || expr == NULL_OBJ)
        return;

    nfo = vm->nfo;

    // Check nfo has proper structure (list with at least 2 elements)
    if (nfo->type != TYPE_LIST || nfo->len < 2)
        return;

    span = nfo_get(nfo, (i64_t)expr);
    if (span.id == 0)
        return;

    loc = vn_list(4, i64(span.id), clone_obj(AS_LIST(nfo)[0]), NULL_OBJ, clone_obj(AS_LIST(nfo)[1]));

    if (vm->trace == NULL_OBJ)
        vm->trace = vn_list(1, loc);
    else
        push_raw(&vm->trace, &loc);
}

static inline obj_p unwrap(obj_p obj, i64_t id) {
    if (IS_ERR(obj))
        eval_error_add_loc((obj_p)id);
    return obj;
}

// Unwind return stack on error, adding trace frames for each caller
static inline obj_p vm_error_unwind(vm_p vm, obj_p err) {
    ctx_t *rs;
    while (vm->rp > 0) {
        rs = &vm->rs[vm->rp - 1];
        if (rs->ip == -1) {
            // External call marker - stop unwinding
            vm->rp--;
            vm->fn = rs->fn;
            vm->fp = rs->fp;
            break;
        }
        // Add caller's location to trace
        bc_error_add_loc(err, rs->fn, rs->ip - 1);
        vm->rp--;
        vm->fn = rs->fn;
        vm->fp = rs->fp;
    }
    return err;
}

// ============================================================================
// Bytecode Interpreter (Computed Goto)
// ============================================================================

__attribute__((hot)) obj_p vm_eval(obj_p fn) {
    vm_p vm = VM;
    u8_t *bc;
    obj_p *consts;
    ctx_t *rs;
    i64_t ip = 0;
    i64_t n;
    obj_p x, y, r, *l, res, *val;

    // Setup VM for this function
    vm->fn = fn;
    vm->fp = vm->sp - AS_LAMBDA(fn)->args->len;
    bc = AS_U8(AS_LAMBDA(fn)->bc);
    consts = AS_LIST(AS_LAMBDA(fn)->consts);

    // Computed goto dispatch table - must match op_type_t order
    static const void *OPS[] = {
        &&OP_RET,        // 0: Control flow
        &&OP_JMP,        // 1
        &&OP_JMPF,       // 2
        &&OP_LOADCONST,  // 3: Data operations
        &&OP_LOADENV,    // 4
        &&OP_STOREENV,   // 5
        &&OP_POP,        // 6
        &&OP_RESOLVE,    // 7: Symbol resolution
        &&OP_CALL1,      // 8: Function calls
        &&OP_CALL2,      // 9
        &&OP_CALLN,      // 10
        &&OP_CALLF,      // 11
        &&OP_CALLS,      // 12
        &&OP_CALLD,      // 13
    };

#define next() goto *OPS[bc[ip++]]
#define push(v) (vm->ps[vm->sp++] = (v))
#define pop() (vm->ps[--vm->sp])
#define top(n) (&vm->ps[vm->sp - (n)])

    next();

OP_RET:
    if (vm->rp != 0) {
        rs = &vm->rs[vm->rp - 1];
        // Check for external call marker (ip == -1)
        if (rs->ip == -1) {
            // Return to external caller (call() function)
            // Don't clean up args - caller (lambda_call) handles that
            // Just return the result; leave args on stack for caller to pop
            return pop();
        }
        // Return from nested bytecode call
        r = pop();
        while (vm->sp > vm->fp)
            drop_obj(pop());
        vm->rp--;
        ip = rs->ip;
        vm->fp = rs->fp;
        vm->fn = rs->fn;
        bc = AS_U8(AS_LAMBDA(vm->fn)->bc);
        consts = AS_LIST(AS_LAMBDA(vm->fn)->consts);
        // Add caller's location to trace if error is propagating
        if (UNLIKELY(IS_ERR(r)))
            bc_error_add_loc(r, vm->fn, ip - 1);
        push(r);
        next();
    }
    // Top-level return
    return pop();

OP_JMP:
    n = bc[ip++];
    ip += n;
    next();

OP_JMPF:
    n = bc[ip++];
    x = pop();
    if (!ops_as_b8(x))
        ip += n;
    drop_obj(x);
    next();

OP_LOADCONST:
    push(clone_obj(consts[bc[ip++]]));
    next();

OP_POP:
    drop_obj(pop());
    next();

OP_RESOLVE:
    x = pop();
    val = resolve(x->i64);
    if (UNLIKELY(val == NULL)) {
        r = err_value(x->i64);  // symbol not found
        drop_obj(x);
        bc_error_add_loc(r, vm->fn, ip - 1);
        return vm_error_unwind(vm, r);
    }
    y = clone_obj(*val);
    drop_obj(x);
    push(y);
    next();

OP_CALL1:
    // Function is on stack (pushed last by LOADCONST)
    y = pop();  // function (pushed last)
    x = pop();  // argument
    r = unary_call(y, x);
    drop_obj(x);
    drop_obj(y);
    if (UNLIKELY(IS_ERR(r))) {
        bc_error_add_loc(r, vm->fn, ip - 1);
        return vm_error_unwind(vm, r);
    }
    push(r);
    next();

OP_CALL2:
    r = pop();  // function (pushed last)
    y = pop();  // second argument
    x = pop();  // first argument
    res = binary_call(r, x, y);
    drop_obj(x);
    drop_obj(y);
    drop_obj(r);
    if (UNLIKELY(IS_ERR(res))) {
        bc_error_add_loc(res, vm->fn, ip - 1);
        return vm_error_unwind(vm, res);
    }
    push(res);
    next();

OP_CALLN:
    n = bc[ip++];  // arity
    r = pop();     // function
    l = top(n);
    res = vary_call(r, l, n);
    for (i64_t i = 0; i < n; ++i)
        drop_obj(l[i]);
    vm->sp -= n;
    drop_obj(r);
    if (UNLIKELY(IS_ERR(res))) {
        bc_error_add_loc(res, vm->fn, ip - 1);
        return vm_error_unwind(vm, res);
    }
    push(res);
    next();

OP_CALLF:
    x = pop();  // lambda function
    if (UNLIKELY(x->type != TYPE_LAMBDA)) {
        i8_t got = x->type;
        drop_obj(x);
        r = err_type(TYPE_LAMBDA, got, 0, 0);
        bc_error_add_loc(r, vm->fn, ip - 1);
        return vm_error_unwind(vm, r);
    }
callf:
    // Compile if not already compiled
    if (UNLIKELY(AS_LAMBDA(x)->bc == NULL_OBJ)) {
        r = cc_compile(x);
        if (UNLIKELY(IS_ERR(r))) {
            drop_obj(x);
            bc_error_add_loc(r, vm->fn, ip - 1);
            return vm_error_unwind(vm, r);
        }
    }
    // Check for stack overflow
    if (UNLIKELY(vm->rp >= VM_STACK_SIZE)) {
        drop_obj(x);
        r = err_limit(VM_STACK_SIZE);
        bc_error_add_loc(r, vm->fn, ip - 1);
        return vm_error_unwind(vm, r);
    }
    // Save current context
    rs = &vm->rs[vm->rp++];
    rs->ip = ip;
    rs->fp = vm->fp;
    rs->fn = vm->fn;
    // Switch to new function
    vm->fp = vm->sp - AS_LAMBDA(x)->args->len;
    vm->fn = x;
    bc = AS_U8(AS_LAMBDA(x)->bc);
    consts = AS_LIST(AS_LAMBDA(x)->consts);
    ip = 0;
    drop_obj(x);  // Safe because it's on the constants pool
    next();

OP_CALLS:
    // Self-recursive call - check for stack overflow
    if (UNLIKELY(vm->rp >= VM_STACK_SIZE)) {
        r = err_limit(VM_STACK_SIZE);
        bc_error_add_loc(r, vm->fn, ip - 1);
        return vm_error_unwind(vm, r);
    }
    rs = &vm->rs[vm->rp++];
    rs->ip = ip;
    rs->fp = vm->fp;
    rs->fn = vm->fn;
    vm->fp = vm->sp - AS_LAMBDA(vm->fn)->args->len;
    ip = 0;
    next();

OP_CALLD:
    n = bc[ip++];  // arity
    x = pop();     // function (dynamically resolved)
    switch (x->type) {
        case TYPE_UNARY:
            if (UNLIKELY(n != 1)) {
                drop_obj(x);
                r = err_arity(1, n, 0);
                bc_error_add_loc(r, vm->fn, ip - 1);
                return vm_error_unwind(vm, r);
            }
            y = pop();
            r = unary_call(x, y);
            drop_obj(y);
            drop_obj(x);
            if (UNLIKELY(IS_ERR(r))) {
                bc_error_add_loc(r, vm->fn, ip - 1);
                return vm_error_unwind(vm, r);
            }
            push(r);
            break;
        case TYPE_BINARY:
            if (UNLIKELY(n != 2)) {
                drop_obj(x);
                r = err_arity(2, n, 0);
                bc_error_add_loc(r, vm->fn, ip - 1);
                return vm_error_unwind(vm, r);
            }
            y = pop();
            r = pop();
            res = binary_call(x, r, y);
            drop_obj(r);
            drop_obj(y);
            drop_obj(x);
            if (UNLIKELY(IS_ERR(res))) {
                bc_error_add_loc(res, vm->fn, ip - 1);
                return vm_error_unwind(vm, res);
            }
            push(res);
            break;
        case TYPE_VARY:
            l = top(n);
            r = vary_call(x, l, n);
            for (i64_t i = 0; i < n; ++i)
                drop_obj(l[i]);
            vm->sp -= n;
            drop_obj(x);
            if (UNLIKELY(IS_ERR(r))) {
                bc_error_add_loc(r, vm->fn, ip - 1);
                return vm_error_unwind(vm, r);
            }
            push(r);
            break;
        case TYPE_LAMBDA:
            goto callf;
        default: {
            i8_t got = x->type;
            drop_obj(x);
            r = err_type(TYPE_LAMBDA, got, 0, 0);  // not callable
            bc_error_add_loc(r, vm->fn, ip - 1);
            return vm_error_unwind(vm, r);
        }
    }
    next();

OP_LOADENV:
    // Load from env by offset
    // - offset < nargs: load from stack (args passed on stack)
    // - offset >= nargs: load from lambda->env values (let-bound locals)
    n = bc[ip++];
    {
        i64_t nargs = AS_LAMBDA(vm->fn)->args->len;
        if (n < nargs) {
            // Args are on stack at fp+offset
            x = vm->ps[vm->fp + n];
            push(clone_obj(x));
        } else {
            // Let-bound locals in env values
            obj_p env = AS_LAMBDA(vm->fn)->env;
            if (env != NULL_OBJ && env->type == TYPE_DICT) {
                i64_t local_idx = n - nargs;
                obj_p values = AS_LIST(env)[1];
                if (local_idx < values->len) {
                    push(clone_obj(AS_LIST(values)[local_idx]));
                } else {
                    // Local not yet defined - push null
                    push(clone_obj(NULL_OBJ));
                }
            } else {
                push(clone_obj(NULL_OBJ));
            }
        }
    }
    next();

OP_STOREENV:
    // Store to env by offset (for let bindings)
    // Value is on stack top, store to env->values[offset - nargs]
    n = bc[ip++];
    {
        i64_t nargs = AS_LAMBDA(vm->fn)->args->len;
        obj_p env = AS_LAMBDA(vm->fn)->env;
        x = pop();  // value to store

        if (n < nargs) {
            // Storing to arg slot - update stack
            drop_obj(vm->ps[vm->fp + n]);
            vm->ps[vm->fp + n] = x;
        } else if (env != NULL_OBJ && env->type == TYPE_DICT) {
            // Store to let-bound local
            i64_t local_idx = n - nargs;
            obj_p values = AS_LIST(env)[1];

            // Extend values list if needed
            while (values->len <= local_idx) {
                obj_p null_val = NULL_OBJ;
                push_obj(&AS_LIST(env)[1], clone_obj(null_val));
                values = AS_LIST(env)[1];
            }

            // Store value
            drop_obj(AS_LIST(values)[local_idx]);
            AS_LIST(values)[local_idx] = x;
        } else {
            // No env, just drop the value
            drop_obj(x);
        }
        // Push the value back (let returns the value)
        push(clone_obj(n < nargs ? vm->ps[vm->fp + n] : AS_LIST(AS_LIST(env)[1])[n - nargs]));
    }
    next();

#undef next
#undef push
#undef pop
#undef top
}

// ============================================================================
// Lambda Call (Main entry point for calling lambdas)
// ============================================================================

obj_p call(obj_p fn, i64_t arity) {
    lambda_p lam = AS_LAMBDA(fn);
    obj_p res;
    vm_p vm = VM;
    obj_p saved_fn = vm->fn;
    i64_t saved_fp = vm->fp;
    UNUSED(arity);

    // Compile on first call - bytecode is cached in the lambda for reuse
    if (lam->bc == NULL_OBJ) {
        res = cc_compile(fn);
        if (IS_ERR(res))
            return res;
    }

    // Push to return stack - this lets resolve() find parent scope
    // ip = -1 marks this as an external call (OP_RET will return to vm_eval top level)
    vm->rs[vm->rp].fn = saved_fn;
    vm->rs[vm->rp].fp = saved_fp;
    vm->rs[vm->rp].ip = -1;  // External call marker
    vm->rp++;

    // Execute bytecode
    res = vm_eval(fn);

    // Pop frame and restore context
    vm->rp--;
    vm->fn = saved_fn;
    vm->fp = saved_fp;

    return res;
}

// ============================================================================
// Recursive Tree-Walking Evaluator
// ============================================================================

// Collect lazy evaluation results (MAPGROUP/MAPFILTER)
static inline obj_p collect_lazy(obj_p x) {
    obj_p y;
    if (x->type == TYPE_MAPGROUP) {
        y = aggr_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
        drop_obj(x);
        return y;
    }
    if (x->type == TYPE_MAPFILTER) {
        y = filter_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
        drop_obj(x);
        return y;
    }
    return x;
}

// Evaluate and collect (unless aggregation function)
static inline obj_p eval_arg(obj_p arg, b8_t is_aggr) {
    obj_p x = eval(arg);
    if (IS_ERR(x))
        return x;
    return is_aggr ? x : collect_lazy(x);
}

// Evaluate unary function call
static obj_p eval_unary(obj_p fn, obj_p *args, i64_t id) {
    obj_p x, res;

    if (fn->attrs & FN_SPECIAL_FORM)
        return ((unary_f)fn->i64)(args[0]);

    x = eval_arg(args[0], fn->attrs & FN_AGGR);
    if (IS_ERR(x))
        return x;

    res = unary_call(fn, x);
    drop_obj(x);
    return unwrap(res, id);
}

// Evaluate binary function call
static obj_p eval_binary(obj_p fn, obj_p *args, i64_t id) {
    obj_p x, y, res;
    b8_t is_aggr = fn->attrs & FN_AGGR;

    if (fn->attrs & FN_SPECIAL_FORM)
        return ((binary_f)fn->i64)(args[0], args[1]);

    x = eval_arg(args[0], is_aggr);
    if (IS_ERR(x))
        return x;

    y = eval_arg(args[1], is_aggr);
    if (IS_ERR(y)) {
        drop_obj(x);
        return y;
    }

    res = binary_call(fn, x, y);
    drop_obj(x);
    drop_obj(y);
    return unwrap(res, id);
}

// Evaluate variadic function call
static obj_p eval_vary(obj_p fn, obj_p *args, i64_t len, i64_t id) {
    obj_p x, res;
    i64_t i;
    b8_t is_aggr = fn->attrs & FN_AGGR;

    if (fn->attrs & FN_SPECIAL_FORM) {
        res = ((vary_f)fn->i64)(args, len);
        return (fn->i64 == (i64_t)ray_do) ? res : unwrap(res, id);
    }

    if (!vm_stack_enough(len))
        return unwrap(err_limit(VM_STACK_SIZE), id);

    for (i = 0; i < len; i++) {
        x = eval_arg(args[i], is_aggr);
        if (IS_ERR(x))
            return x;
        vm_stack_push(x);
    }

    res = vary_call(fn, vm_stack_peek(len - 1), len);

    for (i = 0; i < len; i++)
        drop_obj(vm_stack_pop());

    return (fn->i64 == (i64_t)ray_do) ? res : unwrap(res, id);
}

// Evaluate lambda function call
static obj_p eval_lambda(obj_p fn, obj_p *args, i64_t len, i64_t id) {
    obj_p x;
    i64_t i;
    lambda_p lam = AS_LAMBDA(fn);

    if (len != lam->args->len)
        return unwrap(err_length(lam->args->len, len, 0, 0, 0, 0), id);

    if (!vm_stack_enough(len))
        return unwrap(err_limit(VM_STACK_SIZE), id);

    for (i = 0; i < len; i++) {
        x = eval(args[i]);
        if (IS_ERR(x))
            return x;
        // Materialize lazy values (MAPGROUP/MAPFILTER) for lambda arguments
        x = collect_lazy(x);
        if (IS_ERR(x))
            return x;
        vm_stack_push(x);
    }

    return unwrap(lambda_call(fn, vm_stack_peek(len - 1), len), id);
}

// Evaluate symbol lookup
static inline obj_p eval_sym(obj_p sym) {
    obj_p *val;

    if (sym->attrs & ATTR_QUOTED)
        return symboli64(sym->i64);

    val = resolve(sym->i64);
    if (val == NULL)
        return unwrap(err_value(sym->i64), (i64_t)sym);

    return clone_obj(*val);
}

// Evaluate list (function call)
static obj_p eval_list(obj_p obj) {
    obj_p car, *val, *args;
    i64_t len, id;

    if (obj->len == 0)
        return NULL_OBJ;

    args = AS_LIST(obj);
    car = args[0];
    len = obj->len - 1;
    args++;
    id = (i64_t)obj;

dispatch:
    switch (car->type) {
        case TYPE_UNARY:
            if (len != 1)
                return unwrap(err_arity(1, len, 0), id);
            return eval_unary(car, args, id);

        case TYPE_BINARY:
            if (len != 2)
                return unwrap(err_arity(2, len, 0), id);
            return eval_binary(car, args, id);

        case TYPE_VARY:
            return eval_vary(car, args, len, id);

        case TYPE_LAMBDA:
            return eval_lambda(car, args, len, id);

        case -TYPE_SYMBOL:
            val = resolve(car->i64);
            if (val == NULL)
                return unwrap(err_value(car->i64), id);
            car = *val;
            goto dispatch;

        default:
            return unwrap(err_type(TYPE_LAMBDA, car->type, 0, 0), id);  // not callable
    }
}

// Main eval dispatcher
__attribute__((hot)) obj_p eval(obj_p obj) {
    switch (obj->type) {
        case TYPE_LIST:
            return eval_list(obj);
        case -TYPE_SYMBOL:
            return eval_sym(obj);
        default:
            return clone_obj(obj);
    }
}

// ============================================================================
// Special Forms
// ============================================================================

obj_p ray_return(obj_p *x, i64_t n) {
    UNUSED(x);
    UNUSED(n);
    // Return is now handled via normal bytecode execution
    // For recursive eval, we just return the value
    if (n == 0)
        return NULL_OBJ;
    return clone_obj(x[0]);
}

obj_p ray_raise(obj_p obj) {
    obj_p e;

    if (obj->type != TYPE_C8)
        return err_type(TYPE_C8, obj->type, 0, 0);

    // Clone the message since error_obj takes ownership but caller drops the argument
    e = err_user(NULL);
    return e;
}

obj_p ray_parse_str(i64_t fd, obj_p str, obj_p file) {
    UNUSED(fd);
    obj_p info, res;

    if (str->type != TYPE_C8)
        return err_type(TYPE_C8, str->type, 0, 0);

    info = nfo(clone_obj(file), clone_obj(str));
    res = parse(AS_C8(str), str->len, info);
    drop_obj(info);

    return res;
}

obj_p eval_obj(obj_p obj) { return eval(obj); }

obj_p eval_str_w_attr(lit_p str, i64_t len, obj_p nfo_arg) {
    obj_p parsed, res;
    vm_p vm = VM;
    obj_p saved_nfo = vm ? vm->nfo : NULL_OBJ;

    timeit_reset();
    timeit_span_start("top-level");

    parsed = parse(str, len, nfo_arg);
    timeit_tick("parse");

    if (IS_ERR(parsed)) {
        drop_obj(nfo_arg);
        return parsed;
    }

    // Set nfo for error reporting during tree-walking eval
    if (vm)
        vm->nfo = nfo_arg;

    res = eval(parsed);
    drop_obj(parsed);

    // Restore previous nfo
    if (vm)
        vm->nfo = saved_nfo;

    // Drop nfo_arg - we own it and are done with it
    drop_obj(nfo_arg);

    timeit_span_end("top-level");

    return res;
}

obj_p eval_str(lit_p str) { return eval_str_w_attr(str, strlen(str), NULL_OBJ); }

obj_p ray_eval_str(obj_p str, obj_p file) {
    return ray_eval_str_line(str, file, 0);
}

obj_p ray_eval_str_line(obj_p str, obj_p file, i64_t line) {
    obj_p info, filename;
    c8_t buf[64];
    i64_t len;

    if (str->type != TYPE_C8)
        return err_type(TYPE_C8, str->type, 0, 0);

    // Create filename with line number if provided
    if (file != NULL && file != NULL_OBJ) {
        if (line > 0) {
            // Append line number to filename: "repl:42"
            len = snprintf(buf, 64, "%.*s:%lld", (int)file->len, AS_C8(file), (long long)line);
            filename = string_from_str(buf, len);
        } else {
            filename = clone_obj(file);
        }
    } else {
        if (line > 0) {
            len = snprintf(buf, 64, "repl:%lld", (long long)line);
            filename = string_from_str(buf, len);
        } else {
            filename = string_from_str("repl", 4);
        }
    }

    info = nfo(filename, clone_obj(str));
    return eval_str_w_attr(AS_C8(str), str->len, info);
}

obj_p try_obj(obj_p obj, obj_p ctch) {
    obj_p res;

    // Simple try - just evaluate and catch errors
    res = eval(obj);

    if (IS_ERR(res)) {
        obj_p *pfn;
        obj_p fn = ctch;

        switch (fn->type) {
            case TYPE_LAMBDA:
            call_catch:
                if (AS_LAMBDA(fn)->args->len != 1) {
                    drop_obj(res);
                    return err_arity(1, AS_LAMBDA(fn)->args->len, 0);
                }
                // Push error message as string for catch handler
                vm_stack_push(str_fmt(-1, "%s", err_msg(res)));
                drop_obj(res);
                res = call(fn, 1);
                drop_obj(vm_stack_pop());
                return res;

            case -TYPE_SYMBOL:
                pfn = resolve(fn->i64);
                if (pfn == NULL) {
                    drop_obj(res);
                    return clone_obj(fn);
                }
                fn = *pfn;
                if (fn->type == TYPE_LAMBDA)
                    goto call_catch;
                drop_obj(res);
                return eval(*pfn);

            default:
                drop_obj(res);
                return eval(fn);
        }
    }

    return res;
}

obj_p ray_exit(obj_p *x, i64_t n) {
    i64_t code;

    if (n == 0)
        code = 0;
    else
        code = (x[0]->type == -TYPE_I64) ? x[0]->i64 : (i64_t)n;

    poll_exit(runtime_get()->poll, code);

    return NULL_OBJ;
}

b8_t ray_is_main_thread(nil_t) { return VM->id == 0; }
