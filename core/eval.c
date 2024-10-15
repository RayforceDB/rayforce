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
#include "io.h"
#include "lambda.h"
#include "items.h"
#include "error.h"
#include "filter.h"
#include "group.h"
#include "string.h"
#include "aggr.h"

__thread interpreter_p __INTERPRETER = NULL;

nil_t error_add_loc(obj_p err, i64_t id, ctx_p ctx) {
    obj_p loc, nfo;
    span_t span;

    if (ctx->lambda == NULL_OBJ)
        return;

    nfo = AS_LAMBDA(ctx->lambda)->nfo;

    if (nfo == NULL_OBJ)
        return;

    span = nfo_get(nfo, id);
    loc = vn_list(4,
                  i64(span.id),                             // span
                  clone_obj(AS_LIST(nfo)[0]),               // filename
                  clone_obj(AS_LAMBDA(ctx->lambda)->name),  // function name
                  clone_obj(AS_LIST(nfo)[1])                // source
    );

    if (AS_ERROR(err)->locs == NULL_OBJ)
        AS_ERROR(err)->locs = vn_list(1, loc);
    else
        push_raw(&AS_ERROR(err)->locs, &loc);
}

interpreter_p interpreter_create(u64_t id) {
    interpreter_p interpreter;
    obj_p f;

    interpreter = (interpreter_p)heap_mmap(sizeof(struct interpreter_t));
    interpreter->id = id;
    interpreter->sp = 0;
    interpreter->stack = (obj_p *)heap_stack(sizeof(obj_p) * EVAL_STACK_SIZE);
    interpreter->cp = 0;
    interpreter->ctxstack = (ctx_p)heap_stack(sizeof(struct ctx_t) * EVAL_STACK_SIZE);
    interpreter->timeit.active = B8_FALSE;
    memset(interpreter->ctxstack, 0, sizeof(struct ctx_t) * EVAL_STACK_SIZE);

    __INTERPRETER = interpreter;

    // Directly allocate lambda to avoid using heap_alloc here
    f = (obj_p)heap_mmap(sizeof(struct obj_t) + sizeof(struct lambda_t));
    f->mmod = MMOD_INTERNAL;
    f->type = TYPE_LAMBDA;
    f->rc = 1;

    AS_LAMBDA(f)->name = NULL_OBJ;
    AS_LAMBDA(f)->nfo = NULL_OBJ;
    AS_LAMBDA(f)->args = NULL_OBJ;
    AS_LAMBDA(f)->body = NULL_OBJ;

    ctx_push(f);

    return interpreter;
}

nil_t interpreter_destroy(nil_t) {
    // cleanup stack (if any)
    DEBUG_ASSERT(__INTERPRETER->sp == 0, "stack is not empty");

    heap_unmap(__INTERPRETER->stack, EVAL_STACK_SIZE);
    heap_unmap(__INTERPRETER->ctxstack[0].lambda, sizeof(struct obj_t) + sizeof(struct lambda_t));
    heap_unmap(__INTERPRETER->ctxstack, sizeof(struct ctx_t) * EVAL_STACK_SIZE);
    heap_unmap(__INTERPRETER, sizeof(struct interpreter_t));
}

interpreter_p interpreter_current(nil_t) { return __INTERPRETER; }

obj_p call(obj_p obj, u64_t arity) {
    lambda_p lambda;
    ctx_p ctx;
    obj_p res;
    i64_t sp;

    sp = __INTERPRETER->sp - arity;

    // local env
    stack_push(NULL_OBJ);

    lambda = AS_LAMBDA(obj);

    // push context
    ctx = ctx_push(obj);
    ctx->sp = sp;

    switch (setjmp(ctx->jmp)) {
        // normal return
        case 0:
            res = eval(lambda->body);
            ctx_pop();

            // cleanup stack frame
            while (__INTERPRETER->sp > sp)
                drop_obj(stack_pop());

            return res;

        // return from function via 'return' call
        case 1:
            res = stack_pop();
            ctx_pop();

            // cleanup stack frame
            while (__INTERPRETER->sp > sp)
                drop_obj(stack_pop());

            return res;

        // error
        default:
            res = stack_pop();
            ctx_pop();

            // cleanup stack frame
            while (__INTERPRETER->sp > sp)
                drop_obj(stack_pop());

            return res;
    }
}

__attribute__((hot)) obj_p eval(obj_p obj) {
    u64_t len, i;
    obj_p car, *val, *args, x, y, z, res;
    lambda_p lambda;
    u8_t attrs = 0;

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
                        return unwrap(error_str(ERR_ARITY, "unary function must have 1 argument"), (i64_t)obj);
                    if (car->attrs & FN_SPECIAL_FORM)
                        res = ((unary_f)car->i64)(args[0]);
                    else {
                        x = eval(args[0]);

                        if (IS_ERROR(x))
                            return x;

                        if (!(car->attrs & FN_AGGR) && x->type == TYPE_GROUPMAP) {
                            y = aggr_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                            drop_obj(x);
                            x = y;
                        } else if (x->type == TYPE_FILTERMAP) {
                            y = filter_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                            drop_obj(x);
                            x = y;
                        }

                        res = unary_call(car->attrs, (unary_f)car->i64, x);
                        drop_obj(x);
                    }

                    return unwrap(res, (i64_t)obj);

                case TYPE_BINARY:
                    if (len != 2)
                        return unwrap(error_str(ERR_ARITY, "binary function must have 2 arguments"), (i64_t)obj);
                    if (car->attrs & FN_SPECIAL_FORM)
                        res = ((binary_f)car->i64)(args[0], args[1]);
                    else {
                        x = eval(args[0]);
                        if (IS_ERROR(x))
                            return x;

                        if (!(car->attrs & FN_AGGR) && x->type == TYPE_GROUPMAP) {
                            y = aggr_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                            drop_obj(x);
                            x = y;
                        } else if (x->type == TYPE_FILTERMAP) {
                            y = filter_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                            drop_obj(x);
                            x = y;
                        }

                        y = eval(args[1]);
                        if (IS_ERROR(y)) {
                            drop_obj(x);
                            return y;
                        }

                        if (!(car->attrs & FN_AGGR) && y->type == TYPE_GROUPMAP) {
                            z = aggr_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                            drop_obj(y);
                            y = z;
                        } else if (y->type == TYPE_FILTERMAP) {
                            z = filter_collect(AS_LIST(y)[0], AS_LIST(y)[1]);
                            drop_obj(y);
                            y = z;
                        }

                        res = binary_call(car->attrs, (binary_f)car->i64, x, y);
                        drop_obj(x);
                        drop_obj(y);
                    }

                    return unwrap(res, (i64_t)obj);

                case TYPE_VARY:
                    if (car->attrs & FN_SPECIAL_FORM)
                        res = ((vary_f)car->i64)(args, len);
                    else {
                        if (!stack_enough(len))
                            return unwrap(error_str(ERR_STACK_OVERFLOW, "stack overflow"), (i64_t)obj);

                        for (i = 0; i < len; i++) {
                            x = eval(args[i]);
                            if (IS_ERROR(x))
                                return x;

                            if (!(car->attrs & FN_AGGR) && x->type == TYPE_GROUPMAP) {
                                attrs = FN_GROUP_MAP;
                                y = aggr_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                                drop_obj(x);
                                x = y;
                            } else if (x->type == TYPE_FILTERMAP) {
                                y = filter_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                                drop_obj(x);
                                x = y;
                            }

                            stack_push(x);
                        }

                        res = vary_call(car->attrs | attrs, (vary_f)car->i64, stack_peek(len - 1), len);

                        for (i = 0; i < len; i++)
                            drop_obj(stack_pop());
                    }

                    // Special case for 'do' function (do not unwrap this one to prevent big stack trace for entire
                    // files)
                    return (car->i64 == (i64_t)ray_do) ? res : unwrap(res, (i64_t)obj);

                case TYPE_LAMBDA:
                    lambda = AS_LAMBDA(car);
                    if (len != lambda->args->len)
                        return unwrap(error_str(ERR_ARITY, "wrong number of arguments"), (i64_t)obj);

                    if (!stack_enough(len))
                        return unwrap(error_str(ERR_STACK_OVERFLOW, "stack overflow"), (i64_t)obj);

                    for (i = 0; i < len; i++) {
                        x = eval(args[i]);
                        if (IS_ERROR(x))
                            return x;

                        // if (x->type == TYPE_GROUPMAP)
                        // {
                        //     attrs = FN_GROUP_MAP;
                        //     y = aggr_collect(AS_LIST(x)[0], AS_LIST(x)[1]);
                        //     drop_obj(x);
                        //     x = y;
                        // }
                        // else if (x->type == TYPE_FILTERMAP)
                        // {
                        //     y = filter_collect(x);
                        //     drop_obj(x);
                        //     x = y;
                        // }

                        stack_push(x);
                    }

                    return unwrap(lambda_call(attrs, car, stack_peek(len - 1), len), (i64_t)obj);

                case -TYPE_SYMBOL:
                    val = resolve(car->i64);
                    if (val == NULL)
                        return unwrap(error(ERR_EVAL, "undefined symbol: '%s", str_from_symbol(car->i64)), (i64_t)obj);
                    car = *val;
                    goto call;

                default:
                    return unwrap(error(ERR_EVAL, "'%s is not a function", type_name(car->type)), (i64_t)args[-1]);
            }
        case -TYPE_SYMBOL:
            if (obj->attrs & ATTR_QUOTED)
                return symboli64(obj->i64);

            val = resolve(obj->i64);
            if (val == NULL)
                return unwrap(error(ERR_EVAL, "undefined symbol: '%s", str_from_symbol(obj->i64)), (i64_t)obj);
            return clone_obj(*val);
        default:
            return clone_obj(obj);
    }
}

obj_p amend(obj_p sym, obj_p val) {
    obj_p *env;
    obj_p lambda;
    i64_t bp;
    ctx_p ctx;

    ctx = ctx_get();
    lambda = ctx->lambda;

    bp = ctx->sp;
    env = &__INTERPRETER->stack[bp + AS_LAMBDA(lambda)->args->len];

    if (*env != NULL_OBJ)
        set_obj(env, sym, clone_obj(val));
    else {
        *env = dict(vector(TYPE_SYMBOL, 1), vn_list(1, clone_obj(val)));
        AS_SYMBOL(AS_LIST(*env)[0])
        [0] = sym->i64;
    }

    return val;
}

obj_p mount_env(obj_p obj) {
    u64_t i, l1, l2, l;
    obj_p *env;
    obj_p lambda, keys, vals;
    i64_t bp;
    ctx_p ctx;

    ctx = ctx_get();
    lambda = ctx->lambda;

    bp = ctx->sp;
    env = &__INTERPRETER->stack[bp + AS_LAMBDA(lambda)->args->len];

    if (*env != NULL_OBJ) {
        l1 = AS_LIST(*env)[0]->len;
        l2 = AS_LIST(obj)[0]->len;
        l = l1 + l2;
        keys = SYMBOL(l);
        vals = LIST(l);

        for (i = 0; i < l1; i++) {
            AS_SYMBOL(keys)
            [i] = AS_SYMBOL(AS_LIST(*env)[0])[i];
            AS_LIST(vals)
            [i] = clone_obj(AS_LIST(AS_LIST(*env)[1])[i]);
        }

        for (i = 0; i < l2; i++) {
            AS_SYMBOL(keys)
            [i + l1] = AS_SYMBOL(AS_LIST(obj)[0])[i];
            AS_LIST(vals)
            [i + l1] = clone_obj(AS_LIST(AS_LIST(obj)[1])[i]);
        }

        drop_obj(*env);
    } else {
        keys = clone_obj(AS_LIST(obj)[0]);
        vals = clone_obj(AS_LIST(obj)[1]);
    }

    *env = dict(keys, vals);

    return NULL_OBJ;
}

obj_p unmount_env(u64_t n) {
    obj_p *env;
    obj_p lambda;
    i64_t bp;
    u64_t i, l;
    ctx_p ctx;

    ctx = ctx_get();
    lambda = ctx->lambda;

    bp = ctx->sp;
    env = &__INTERPRETER->stack[bp + AS_LAMBDA(lambda)->args->len];

    if (ops_count(*env) == n) {
        drop_obj(*env);
        *env = NULL_OBJ;
    } else {
        l = AS_LIST(*env)[0]->len;
        resize_obj(&AS_LIST(*env)[0], l - n);

        // free values
        for (i = l; i > l - n; i--)
            drop_obj(AS_LIST(AS_LIST(*env)[1])[i - 1]);

        resize_obj(&AS_LIST(*env)[1], l - n);
    }

    return NULL_OBJ;
}

obj_p ray_return(obj_p *x, u64_t n) {
    if (__INTERPRETER->cp == 1)
        THROW(ERR_NOT_SUPPORTED, "return outside of function");

    if (n == 0)
        stack_push(NULL_OBJ);
    else
        stack_push(clone_obj(x[0]));

    longjmp(ctx_get()->jmp, 1);

    __builtin_unreachable();
}

obj_p ray_raise(obj_p obj) {
    obj_p e;

    if (obj->type != TYPE_C8)
        THROW(ERR_TYPE, "raise: expected 'string, got '%s", type_name(obj->type));

    e = error_obj(ERR_RAISE, clone_obj(obj));
    unwrap(e, (i64_t)obj);

    if (__INTERPRETER->cp == 1)
        return e;

    stack_push(e);

    longjmp(ctx_get()->jmp, 2);

    __builtin_unreachable();
}

obj_p ray_parse_str(i64_t fd, obj_p str, obj_p file) {
    UNUSED(fd);
    obj_p info, res;

    if (str->type != TYPE_C8)
        THROW(ERR_TYPE, "parse: expected string, got %s", type_name(str->type));

    info = nfo(clone_obj(file), clone_obj(str));
    res = parse(AS_C8(str), info);
    drop_obj(info);

    return res;
}

obj_p eval_obj(obj_p obj) {
    obj_p res, fn;
    ctx_p ctx;
    i64_t sp;

    fn = lambda(NULL_OBJ, NULL_OBJ, NULL_OBJ);

    sp = __INTERPRETER->sp;

    stack_push(NULL_OBJ);  // null env

    ctx = ctx_push(fn);
    ctx->sp = sp;
    res = (setjmp(ctx->jmp) == 0) ? eval(obj) : stack_pop();

    // cleanup stack frame
    while (__INTERPRETER->sp > sp)
        drop_obj(stack_pop());

    drop_obj(fn);

    ctx_pop();

    return res;
}

obj_p ray_eval_str(obj_p str, obj_p file) {
    obj_p parsed, res, info, fn;
    ctx_p ctx;
    i64_t sp;

    if (str->type != TYPE_C8)
        THROW(ERR_TYPE, "eval: expected string, got %s", type_name(str->type));

    info = nfo(clone_obj(file), clone_obj(str));

    timeit_reset();
    timeit_span_start("top-level");
    parsed = parse(AS_C8(str), info);
    timeit_tick("parse");

    if (IS_ERROR(parsed)) {
        drop_obj(info);
        return parsed;
    }

    sp = __INTERPRETER->sp;
    stack_push(NULL_OBJ);  // null env
    fn = lambda(NULL_OBJ, NULL_OBJ, info);

    ctx = ctx_push(fn);
    ctx->sp = sp;

    res = (setjmp(ctx->jmp) == 0) ? eval(parsed) : stack_pop();

    drop_obj(parsed);
    drop_obj(fn);

    // cleanup stack frame
    while (__INTERPRETER->sp > sp)
        drop_obj(stack_pop());

    ctx_pop();

    timeit_span_end("top-level");

    return res;
}

obj_p try_obj(obj_p obj, obj_p ctch) {
    ctx_p ctx;
    i64_t sp;
    obj_p fn, res;
    b8_t sig;

    fn = lambda(NULL_OBJ, NULL_OBJ, NULL_OBJ);
    ctx = ctx_push(fn);

    switch (setjmp(ctx->jmp)) {
        case 0:
            res = eval(obj);
            sig = B8_FALSE;
            break;
        case 1:
            res = stack_pop();
            sig = B8_FALSE;
            break;
        case 2:
        case 3:
            res = stack_pop();
            sig = B8_TRUE;
            break;
        default:
            __builtin_unreachable();
    }

    sp = ctx->sp;

    if (__INTERPRETER->cp == 1)
        drop_obj(AS_LAMBDA(ctx->lambda)->nfo);

    // cleanup stack frame
    while (__INTERPRETER->sp > sp)
        drop_obj(stack_pop());

    drop_obj(fn);
    ctx_pop();

    if (IS_ERROR(res) || sig) {
        if (ctch) {
            if (ctch->type == TYPE_LAMBDA) {
                if (AS_LAMBDA(ctch)->args->len != 1) {
                    drop_obj(res);
                    THROW(ERR_LENGTH, "catch: expected 1 argument, got %llu", AS_LAMBDA(ctch)->args->len);
                }
                if (IS_ERROR(res)) {
                    stack_push(clone_obj(AS_ERROR(res)->msg));
                    drop_obj(res);
                } else
                    stack_push(res);

                return call(ctch, 1);
            }

            drop_obj(res);
            return clone_obj(ctch);
        } else {
            drop_obj(res);
            return NULL_OBJ;
        }
    }

    return res;
}

obj_p interpreter_env_get(nil_t) {
    obj_p lambda, env;
    u64_t l;
    ctx_p ctx;

    ctx = ctx_get();
    lambda = ctx->lambda;

    l = AS_LAMBDA(lambda)->args->len;
    env = __INTERPRETER->stack[ctx->sp + l];

    return env;
}

nil_t interpreter_env_set(interpreter_p interpreter, obj_p env) { interpreter->stack[interpreter->sp++] = env; }

nil_t interpreter_env_unset(interpreter_p interpreter) { drop_obj(interpreter->stack[--interpreter->sp]); }

obj_p *resolve(i64_t sym) {
    i64_t bp, *args;
    obj_p lambda, env;
    u64_t i, l, n;
    ctx_p ctx;

    ctx = ctx_get();
    lambda = ctx->lambda;

    if (sym == SYMBOL_SELF)
        return &ctx->lambda;

    l = AS_LAMBDA(lambda)->args->len;
    bp = ctx->sp;

    // search locals
    env = __INTERPRETER->stack[bp + l];

    if (env != NULL_OBJ) {
        n = AS_LIST(env)[0]->len;

        // search in a reverse order
        for (i = n; i > 0; i--) {
            if (AS_SYMBOL(AS_LIST(env)[0])[i - 1] == sym)
                return &AS_LIST(AS_LIST(env)[1])[i - 1];
        }
    }

    // search args
    args = AS_SYMBOL(AS_LAMBDA(lambda)->args);
    for (i = 0; i < l; i++) {
        if (args[i] == sym)
            return &__INTERPRETER->stack[bp + i];
    }

    // search globals
    i = find_raw(AS_LIST(runtime_get()->env.variables)[0], &sym);
    if (i == AS_LIST(runtime_get()->env.variables)[0]->len)
        return NULL;

    return &AS_LIST(AS_LIST(runtime_get()->env.variables)[1])[i];
}
