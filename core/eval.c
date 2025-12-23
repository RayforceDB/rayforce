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

// Thread-local VM pointer
__thread vm_p __VM = NULL;

vm_p vm_create(i64_t id, struct pool_t *pool) {
    vm_p vm;

    // Use raw mmap for VM allocation (can't use heap before VM exists)
    vm = (vm_p)mmap_alloc(sizeof(struct vm_t));
    vm->id = id;
    vm->sp = 0;
    vm->fp = 0;
    vm->rp = 0;
    vm->fn = NULL_OBJ;
    vm->env = NULL_OBJ;
    vm->pool = pool;
    vm->timeit.active = B8_FALSE;
    vm->last_err = NULL_OBJ;
    vm->last_locs = NULL_OBJ;
    memset(vm->ps, 0, sizeof(obj_p) * VM_STACK_SIZE);
    memset(vm->rs, 0, sizeof(ctx_t) * VM_STACK_SIZE);

    // Set VM for this thread so heap_create can use it
    __VM = vm;

    // Create heap for this VM
    vm->heap = heap_create(id);

    return vm;
}

nil_t vm_destroy(vm_p vm) {
    // Clear any remaining items on the stack
    while (vm->sp > 0) {
        drop_obj(vm->ps[--vm->sp]);
    }

    // Destroy heap first
    heap_destroy(vm->heap);
    vm->heap = NULL;

    // Clear thread-local
    __VM = NULL;

    // Then destroy VM
    mmap_free(vm, sizeof(struct vm_t));
}

obj_p vm_env_get(nil_t) { return __VM->env; }

nil_t vm_env_set(vm_p vm, obj_p env) { vm->env = env; }

nil_t vm_env_unset(vm_p vm) {
    if (vm->env != NULL_OBJ) {
        drop_obj(vm->env);
        vm->env = NULL_OBJ;
    }
}

// ============================================================================
// Symbol Resolution
// ============================================================================

obj_p *resolve(i64_t sym) {
    i64_t j, *args;
    obj_p fn, env;
    i64_t i, l, n;
    vm_p vm = VM;

    fn = vm->fn;

    // Check for self reference
    extern i64_t symbols_intern(lit_p, i64_t);
    static i64_t SYMBOL_SELF = 0;
    if (SYMBOL_SELF == 0)
        SYMBOL_SELF = symbols_intern("self", 4);

    if (sym == SYMBOL_SELF)
        return &vm->fn;

    // Search in function arguments (on stack)
    if (fn != NULL_OBJ && AS_LAMBDA(fn)->args != NULL_OBJ) {
        l = AS_LAMBDA(fn)->args->len;
        args = AS_SYMBOL(AS_LAMBDA(fn)->args);
        for (i = 0; i < l; i++) {
            if (args[i] == sym)
                return &vm->ps[vm->fp + i];
        }
    }

    // Search in local environment
    env = vm->env;
    if (env != NULL_OBJ && env->type == TYPE_DICT) {
        n = AS_LIST(env)[0]->len;
        for (i = n; i > 0; i--) {
            if (AS_SYMBOL(AS_LIST(env)[0])[i - 1] == sym)
                return &AS_LIST(AS_LIST(env)[1])[i - 1];
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

    env_slot = &vm->env;

    if (*env_slot != NULL_OBJ)
        set_obj(env_slot, sym, clone_obj(val));
    else {
        *env_slot = dict(vector(TYPE_SYMBOL, 1), vn_list(1, clone_obj(val)));
        AS_SYMBOL(AS_LIST(*env_slot)[0])[0] = sym->i64;
    }

    return val;
}

obj_p mount_env(obj_p obj) {
    i64_t i, l1, l2, l;
    obj_p *env_slot;
    obj_p keys, vals;
    vm_p vm = VM;

    env_slot = &vm->env;

    if (*env_slot != NULL_OBJ) {
        l1 = AS_LIST(*env_slot)[0]->len;
        l2 = AS_LIST(obj)[0]->len;
        l = l1 + l2;
        keys = SYMBOL(l);
        vals = LIST(l);

        for (i = 0; i < l1; i++) {
            AS_SYMBOL(keys)[i] = AS_SYMBOL(AS_LIST(*env_slot)[0])[i];
            AS_LIST(vals)[i] = clone_obj(AS_LIST(AS_LIST(*env_slot)[1])[i]);
        }

        for (i = 0; i < l2; i++) {
            AS_SYMBOL(keys)[i + l1] = AS_SYMBOL(AS_LIST(obj)[0])[i];
            AS_LIST(vals)[i + l1] = clone_obj(AS_LIST(AS_LIST(obj)[1])[i]);
        }

        drop_obj(*env_slot);
    } else {
        keys = clone_obj(AS_LIST(obj)[0]);
        vals = clone_obj(AS_LIST(obj)[1]);
    }

    *env_slot = dict(keys, vals);

    return NULL_OBJ;
}

obj_p unmount_env(i64_t n) {
    obj_p *env_slot;
    i64_t i, l;
    vm_p vm = VM;

    env_slot = &vm->env;

    if (*env_slot == NULL_OBJ)
        return NULL_OBJ;

    if (ops_count(*env_slot) == n) {
        drop_obj(*env_slot);
        *env_slot = NULL_OBJ;
    } else {
        l = AS_LIST(*env_slot)[0]->len;
        resize_obj(&AS_LIST(*env_slot)[0], l - n);

        for (i = l; i > l - n; i--)
            drop_obj(AS_LIST(AS_LIST(*env_slot)[1])[i - 1]);

        resize_obj(&AS_LIST(*env_slot)[1], l - n);
    }

    return NULL_OBJ;
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

    if (vm->last_locs == NULL_OBJ)
        vm->last_locs = vn_list(1, loc);
    else
        push_raw(&vm->last_locs, &loc);
}

// Add location info from bytecode debug info
nil_t bc_error_add_loc(obj_p err, obj_p fn, i64_t ip) {
    obj_p loc, nfo;
    span_t span;
    lambda_p lambda;
    (void)err;

    if (fn == NULL_OBJ || fn->type != TYPE_LAMBDA)
        return;

    lambda = AS_LAMBDA(fn);
    nfo = lambda->nfo;

    if (nfo == NULL_OBJ)
        return;

    vm_p vm = VM;
    // Look up span from bytecode debug info
    span = bc_dbg_get(lambda->dbg, ip);
    if (span.id == 0)
        return;

    loc = vn_list(4, i64(span.id), clone_obj(AS_LIST(nfo)[0]), clone_obj(lambda->name), clone_obj(AS_LIST(nfo)[1]));

    if (vm->last_locs == NULL_OBJ)
        vm->last_locs = vn_list(1, loc);
    else
        push_raw(&vm->last_locs, &loc);
}

static inline obj_p unwrap(obj_p obj, i64_t id) {
    UNUSED(id);
    return obj;
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
    obj_p x, y, r, *l;

    // Setup VM for this function
    vm->fn = fn;
    vm->fp = vm->sp - AS_LAMBDA(fn)->args->len;
    bc = AS_U8(AS_LAMBDA(fn)->bc);
    consts = AS_LIST(AS_LAMBDA(fn)->consts);

    // Computed goto dispatch table
    static const void *OPS[] = {&&OP_RET,   &&OP_PUSHC, &&OP_DUP,   &&OP_POP,   &&OP_JMPZ,  &&OP_JMP,  &&OP_DEREF,
                                &&OP_CALF1, &&OP_CALF2, &&OP_CALF0, &&OP_CALFN, &&OP_CALFS, &&OP_CALFD};

#define next() goto *OPS[bc[ip++]]
#define push(v) (vm->ps[vm->sp++] = (v))
#define pop() (vm->ps[--vm->sp])
#define top(n) (&vm->ps[vm->sp - (n)])

    next();

OP_RET:
    if (vm->rp != 0) {
        // Return from nested call
        r = pop();
        while (vm->sp > vm->fp)
            drop_obj(pop());
        rs = &vm->rs[--vm->rp];
        ip = rs->ip;
        vm->fp = rs->fp;
        vm->fn = rs->fn;
        bc = AS_U8(AS_LAMBDA(vm->fn)->bc);
        consts = AS_LIST(AS_LAMBDA(vm->fn)->consts);
        push(r);
        next();
    }
    // Top-level return
    return pop();

OP_PUSHC:
    push(clone_obj(consts[bc[ip++]]));
    next();

OP_DUP:
    n = bc[ip++];
    x = vm->ps[vm->fp + n];
    push(clone_obj(x));
    next();

OP_POP:
    drop_obj(pop());
    next();

OP_JMPZ:
    n = bc[ip++];
    x = pop();
    if (!ops_as_b8(x))
        ip += n;
    drop_obj(x);
    next();

OP_JMP:
    n = bc[ip++];
    ip += n;
    next();

OP_DEREF:
    x = pop();
    {
        obj_p *val = resolve(x->i64);
        if (val == NULL) {
            r = ray_err(ERR_EVAL);
            drop_obj(x);
            bc_error_add_loc(r, vm->fn, ip);
            return r;
        }
        y = clone_obj(*val);
    }
    drop_obj(x);
    push(y);
    next();

OP_CALF1:
    // Function is on stack (pushed last by PUSHC)
    y = pop();  // function (pushed last)
    x = pop();  // argument
    r = unary_call(y, x);
    drop_obj(x);
    drop_obj(y);
    if (IS_ERR(r)) {
        bc_error_add_loc(r, vm->fn, ip);
        return r;
    }
    push(r);
    next();

OP_CALF2:
    r = pop();  // function (pushed last)
    y = pop();  // second argument
    x = pop();  // first argument
    {
        obj_p res = binary_call(r, x, y);
        drop_obj(x);
        drop_obj(y);
        drop_obj(r);
        if (IS_ERR(res)) {
            bc_error_add_loc(res, vm->fn, ip);
            return res;
        }
        push(res);
    }
    next();

OP_CALF0:
    n = bc[ip++];  // arity
    r = pop();     // function
    l = top(n);
    {
        obj_p res = vary_call(r, l, n);
        for (i64_t i = 0; i < n; ++i)
            drop_obj(l[i]);
        vm->sp -= n;
        drop_obj(r);
        if (IS_ERR(res)) {
            bc_error_add_loc(res, vm->fn, ip);
            return res;
        }
        push(res);
    }
    next();

OP_CALFN:
    x = pop();  // lambda function
    if (x->type != TYPE_LAMBDA) {
        drop_obj(x);
        r = ray_err(ERR_TYPE);
        bc_error_add_loc(r, vm->fn, ip);
        return r;
    }
callfn:
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

OP_CALFS:
    // Self-recursive call
    rs = &vm->rs[vm->rp++];
    rs->ip = ip;
    rs->fp = vm->fp;
    rs->fn = vm->fn;
    vm->fp = vm->sp - AS_LAMBDA(vm->fn)->args->len;
    ip = 0;
    next();

OP_CALFD:
    n = bc[ip++];  // arity
    x = pop();     // function (dynamically resolved)
    switch (x->type) {
        case TYPE_UNARY:
            if (n != 1) {
                drop_obj(x);
                r = ray_err(ERR_ARITY);
                bc_error_add_loc(r, vm->fn, ip);
                return r;
            }
            y = pop();
            r = unary_call(x, y);
            drop_obj(y);
            drop_obj(x);
            if (IS_ERR(r)) {
                bc_error_add_loc(r, vm->fn, ip);
                return r;
            }
            push(r);
            break;
        case TYPE_BINARY:
            if (n != 2) {
                drop_obj(x);
                r = ray_err(ERR_ARITY);
                bc_error_add_loc(r, vm->fn, ip);
                return r;
            }
            y = pop();
            r = pop();
            {
                obj_p res = binary_call(x, r, y);
                drop_obj(r);
                drop_obj(y);
                drop_obj(x);
                if (IS_ERR(res)) {
                    bc_error_add_loc(res, vm->fn, ip);
                    return res;
                }
                push(res);
            }
            break;
        case TYPE_VARY:
            l = top(n);
            r = vary_call(x, l, n);
            for (i64_t i = 0; i < n; ++i)
                drop_obj(l[i]);
            vm->sp -= n;
            drop_obj(x);
            if (IS_ERR(r)) {
                bc_error_add_loc(r, vm->fn, ip);
                return r;
            }
            push(r);
            break;
        case TYPE_LAMBDA:
            goto callfn;
        default:
            drop_obj(x);
            r = ray_err(ERR_TYPE);
            bc_error_add_loc(r, vm->fn, ip);
            return r;
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
    obj_p saved_env = vm->env;
    obj_p compiled_fn = fn;
    b8_t need_drop = B8_FALSE;
    UNUSED(arity);

    // Compile if not already compiled
    if (lam->bc == NULL_OBJ) {
        // Clone the lambda before compiling to avoid modifying the AST
        // This is necessary because the lambda might be part of the parsed AST
        // which will be dropped after evaluation
        compiled_fn = lambda(clone_obj(lam->args), clone_obj(lam->body), clone_obj(lam->nfo));
        res = cc_compile(compiled_fn);
        if (IS_ERR(res)) {
            drop_obj(compiled_fn);
            return res;
        }
        need_drop = B8_TRUE;
    }

    // Execute bytecode
    // Note: arguments are already on the stack, pushed by caller
    // Caller is responsible for cleanup after we return
    res = vm_eval(compiled_fn);

    // Restore previous context for tree-walking eval
    vm->fn = saved_fn;
    vm->env = saved_env;

    // Drop the compiled copy if we created one
    if (need_drop)
        drop_obj(compiled_fn);

    return res;
}

// ============================================================================
// Recursive Tree-Walking Evaluator
// ============================================================================

__attribute__((hot)) obj_p eval(obj_p obj) {
    i64_t len, i;
    obj_p car, *val, *args, x, y, z, res;
    lambda_p lambda;
    switch (obj->type) {
        case TYPE_LIST:
            if (obj->len == 0)
                return NULL_OBJ;

            args = AS_LIST(obj);
            car = args[0];
            len = obj->len - 1;
            args++;

        call:
            switch (car->type) {
                case TYPE_UNARY:
                    if (len != 1)
                        return unwrap(ray_err(ERR_ARITY), (i64_t)obj);
                    if (car->attrs & FN_SPECIAL_FORM)
                        res = ((unary_f)car->i64)(args[0]);
                    else {
                        x = eval(args[0]);
                        if (IS_ERR(x))
                            return x;

                        if (!(car->attrs & FN_AGGR) && x->type == TYPE_MAPGROUP) {
                            y = aggr_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                            drop_obj(x);
                            x = y;
                        } else if (!(car->attrs & FN_AGGR) && x->type == TYPE_MAPFILTER) {
                            y = filter_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                            drop_obj(x);
                            x = y;
                        }

                        res = unary_call(car, x);
                        drop_obj(x);
                    }
                    return unwrap(res, (i64_t)obj);

                case TYPE_BINARY:
                    if (len != 2)
                        return unwrap(ray_err(ERR_ARITY), (i64_t)obj);
                    if (car->attrs & FN_SPECIAL_FORM)
                        res = ((binary_f)car->i64)(args[0], args[1]);
                    else {
                        x = eval(args[0]);
                        if (IS_ERR(x))
                            return x;

                        if (!(car->attrs & FN_AGGR) && x->type == TYPE_MAPGROUP) {
                            y = aggr_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                            drop_obj(x);
                            x = y;
                        } else if (!(car->attrs & FN_AGGR) && x->type == TYPE_MAPFILTER) {
                            y = filter_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                            drop_obj(x);
                            x = y;
                        }

                        y = eval(args[1]);
                        if (IS_ERR(y)) {
                            drop_obj(x);
                            return y;
                        }

                        if (!(car->attrs & FN_AGGR) && y->type == TYPE_MAPGROUP) {
                            z = aggr_collect(AS_LIST(y)[0], AS_LIST(y)[1]);
                            drop_obj(y);
                            y = z;
                        } else if (!(car->attrs & FN_AGGR) && y->type == TYPE_MAPFILTER) {
                            z = filter_collect(AS_LIST(y)[0], AS_LIST(y)[1]);
                            drop_obj(y);
                            y = z;
                        }

                        res = binary_call(car, x, y);
                        drop_obj(x);
                        drop_obj(y);
                    }
                    return unwrap(res, (i64_t)obj);

                case TYPE_VARY:
                    if (car->attrs & FN_SPECIAL_FORM)
                        res = ((vary_f)car->i64)(args, len);
                    else {
                        if (!stack_enough(len))
                            return unwrap(ray_err(ERR_STACK), (i64_t)obj);

                        for (i = 0; i < len; i++) {
                            x = eval(args[i]);
                            if (IS_ERR(x))
                                return x;

                            if (!(car->attrs & FN_AGGR) && x->type == TYPE_MAPGROUP) {
                                y = aggr_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                                drop_obj(x);
                                x = y;
                            } else if (!(car->attrs & FN_AGGR) && x->type == TYPE_MAPFILTER) {
                                y = filter_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                                drop_obj(x);
                                x = y;
                            }

                            stack_push(x);
                        }

                        res = vary_call(car, stack_peek(len - 1), len);

                        for (i = 0; i < len; i++)
                            drop_obj(stack_pop());
                    }
                    return (car->i64 == (i64_t)ray_do) ? res : unwrap(res, (i64_t)obj);

                case TYPE_LAMBDA:
                    lambda = AS_LAMBDA(car);
                    if (len != lambda->args->len)
                        return unwrap(ray_err(ERR_ARITY), (i64_t)obj);

                    if (!stack_enough(len))
                        return unwrap(ray_err(ERR_STACK), (i64_t)obj);

                    for (i = 0; i < len; i++) {
                        x = eval(args[i]);
                        if (IS_ERR(x))
                            return x;
                        stack_push(x);
                    }

                    return unwrap(lambda_call(car, stack_peek(len - 1), len), (i64_t)obj);

                case -TYPE_SYMBOL:
                    val = resolve(car->i64);
                    if (val == NULL)
                        return unwrap(ray_err(ERR_EVAL), (i64_t)obj);
                    car = *val;
                    goto call;

                default:
                    return unwrap(ray_err(ERR_EVAL), (i64_t)args[-1]);
            }

        case -TYPE_SYMBOL:
            if (obj->attrs & ATTR_QUOTED)
                return symboli64(obj->i64);

            val = resolve(obj->i64);
            if (val == NULL)
                return unwrap(ray_err(ERR_EVAL), (i64_t)obj);
            return clone_obj(*val);

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
        return ray_err(ERR_TYPE);

    // Clone the message since error_obj takes ownership but caller drops the argument
    e = ray_err(ERR_RAISE);
    return e;
}

obj_p ray_parse_str(i64_t fd, obj_p str, obj_p file) {
    UNUSED(fd);
    obj_p info, res;

    if (str->type != TYPE_C8)
        return ray_err(ERR_TYPE);

    info = nfo(clone_obj(file), clone_obj(str));
    res = parse(AS_C8(str), str->len, info);
    drop_obj(info);

    return res;
}

obj_p eval_obj(obj_p obj) { return eval(obj); }

obj_p eval_str_w_attr(lit_p str, i64_t len, obj_p nfo) {
    obj_p parsed, res;

    timeit_reset();
    timeit_span_start("top-level");

    parsed = parse(str, len, nfo);
    timeit_tick("parse");

    if (IS_ERR(parsed)) {
        drop_obj(nfo);
        return parsed;
    }

    res = eval(parsed);
    drop_obj(parsed);

    timeit_span_end("top-level");

    return res;
}

obj_p eval_str(lit_p str) { return eval_str_w_attr(str, strlen(str), NULL_OBJ); }

obj_p ray_eval_str(obj_p str, obj_p file) {
    obj_p info;

    if (str->type != TYPE_C8)
        return ray_err(ERR_TYPE);

    info = (file != NULL && file != NULL_OBJ) ? nfo(clone_obj(file), clone_obj(str)) : NULL_OBJ;

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
                    return ray_err(ERR_ARITY);
                }
                // Push error message as string for catch handler
                stack_push(str_fmt(-1, "%s", ray_err_msg(res)));
                drop_obj(res);
                res = call(fn, 1);
                drop_obj(stack_pop());
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

b8_t ray_is_main_thread(nil_t) { return heap_get()->id == 0; }
