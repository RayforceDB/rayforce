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

__thread interpreter_t __INTERPRETER = NULL;

nil_t error_add_loc(obj_t err, i64_t id, ctx_t *ctx)
{
    obj_t loc, nfo;
    span_t span;

    nfo = as_lambda(ctx->lambda)->nfo;
    span = nfo_get(nfo, id);
    loc = vn_list(4,
                  i64(span.id),                        // span
                  clone(as_list(nfo)[0]),              // filename
                  clone(as_lambda(ctx->lambda)->name), // function name
                  clone(as_list(nfo)[1])               // source
    );

    if (as_error(err)->locs == NULL_OBJ)
        as_error(err)->locs = vn_list(1, loc);
    else
        push_raw(&as_error(err)->locs, &loc);
}

inline __attribute__((always_inline)) nil_t stack_push(obj_t val)
{
    __INTERPRETER->stack[__INTERPRETER->sp++] = val;
}

inline __attribute__((always_inline)) obj_t stack_pop()
{
    return __INTERPRETER->stack[--__INTERPRETER->sp];
}

inline __attribute__((always_inline)) obj_t *stack_peek(i64_t n)
{
    return &__INTERPRETER->stack[__INTERPRETER->sp - n - 1];
}

inline __attribute__((always_inline)) obj_t stack_at(i64_t n)
{
    return __INTERPRETER->stack[__INTERPRETER->sp - n - 1];
}

inline __attribute__((always_inline)) ctx_t *ctx_push(obj_t lambda)
{
    ctx_t *ctx = &__INTERPRETER->ctxstack[__INTERPRETER->cp++];
    ctx->lambda = lambda;
    return ctx;
}

inline __attribute__((always_inline)) ctx_t *ctx_pop()
{
    return &__INTERPRETER->ctxstack[--__INTERPRETER->cp];
}

inline __attribute__((always_inline)) ctx_t *ctx_get()
{
    return &__INTERPRETER->ctxstack[__INTERPRETER->cp - 1];
}

inline __attribute__((always_inline)) ctx_t *ctx_clone(obj_t info)
{
    ctx_t *ctx;
    i64_t sp;
    obj_t f;

    if (__INTERPRETER->cp == 1)
    {
        sp = __INTERPRETER->sp;
        stack_push(NULL_OBJ);
        f = lambda(vector_symbol(0), NULL_OBJ, info);
        as_lambda(f)->name = symbol("top-level");
    }
    else
    {
        ctx = ctx_get();
        sp = ctx->sp;
        f = ctx->lambda;
        drop(info);
    }

    ctx = ctx_push(f);
    ctx->sp = sp;

    return ctx;
}

inline __attribute__((always_inline)) obj_t unwrap(obj_t obj, i64_t id)
{
    if (is_error(obj))
        error_add_loc(obj, id, ctx_get());

    return obj;
}

inline __attribute__((always_inline)) bool_t stack_enough(u64_t n)
{
    return __INTERPRETER->sp + n < EVAL_STACK_SIZE;
}

interpreter_t interpreter_new()
{
    interpreter_t interpreter;
    obj_t f;

    interpreter = mmap_malloc(sizeof(struct interpreter_t));
    interpreter->sp = 0;
    interpreter->stack = (obj_t *)mmap_stack(sizeof(obj_t) * EVAL_STACK_SIZE);
    interpreter->cp = 0;
    interpreter->ctxstack = (ctx_t *)mmap_stack(sizeof(ctx_t) * EVAL_STACK_SIZE);
    memset(interpreter->ctxstack, 0, sizeof(ctx_t) * EVAL_STACK_SIZE);
    __INTERPRETER = interpreter;

    // push top-level context
    f = lambda(vector_symbol(0), NULL_OBJ, NULL_OBJ);
    as_lambda(f)->name = symbol("top-level");
    ctx_push(f);

    return interpreter;
}

nil_t interpreter_free()
{
    // cleanup stack (if any)
    while (__INTERPRETER->sp)
        drop(__INTERPRETER->stack[--__INTERPRETER->sp]);

    mmap_free(__INTERPRETER->stack, EVAL_STACK_SIZE);

    // cleanup context stack
    drop(__INTERPRETER->ctxstack[0].lambda);

    mmap_free(__INTERPRETER->ctxstack, sizeof(ctx_t) * EVAL_STACK_SIZE);

    mmap_free(__INTERPRETER, sizeof(struct interpreter_t));
}

obj_t call(obj_t obj, u64_t arity)
{
    lambda_t *lambda;
    ctx_t *ctx;
    obj_t res;
    i64_t sp;

    sp = __INTERPRETER->sp - arity;

    // local env
    stack_push(NULL_OBJ);

    lambda = as_lambda(obj);

    // push context
    ctx = ctx_push(obj);
    ctx->sp = sp;

    switch (setjmp(ctx->jmp))
    {

    // normal return
    case 0:
        res = eval(lambda->body);
        ctx = ctx_pop();

        // cleanup stack frame
        while (__INTERPRETER->sp > sp)
            drop(stack_pop());

        return res;

    // return from function via 'return' call
    case 1:
        res = stack_pop();
        ctx = ctx_pop();

        // cleanup stack frame
        while (__INTERPRETER->sp > sp)
            drop(stack_pop());

        return res;

    // error
    default:
        res = stack_pop();
        ctx = ctx_pop();

        // cleanup stack frame
        while (__INTERPRETER->sp > sp)
            drop(stack_pop());

        return res;
    }
}

__attribute__((hot)) obj_t eval(obj_t obj)
{
    u64_t len, i;
    obj_t car, *val, *args, x, y, z, res;
    lambda_t *lambda;
    u8_t attrs = 0;

    switch (obj->type)
    {
    case TYPE_LIST:
        if (obj->len == 0)
            return NULL_OBJ;

        args = as_list(obj);
        car = args[0];
        len = obj->len - 1;
        args++;

    call:
        switch (car->type)
        {
        case TYPE_UNARY:
            if (len != 1)
                return unwrap(error_str(ERR_ARITY, "unary function must have 1 argument"), (i64_t)obj);
            if (car->attrs & FN_SPECIAL_FORM)
                res = ((unary_f)car->i64)(args[0]);
            else
            {
                x = eval(args[0]);
                if (is_error(x))
                    return x;

                if (!(car->attrs & FN_AGGR) && x->type == TYPE_GROUPMAP)
                {
                    y = group_collect(x);
                    drop(x);
                    x = y;
                }
                else if (x->type == TYPE_FILTERMAP)
                {
                    y = filter_collect(x);
                    drop(x);
                    x = y;
                }
                res = unary_call(car->attrs, (unary_f)car->i64, x);
                drop(x);
            }

            return unwrap(res, (i64_t)obj);

        case TYPE_BINARY:
            if (len != 2)
                return unwrap(error_str(ERR_ARITY, "binary function must have 2 arguments"), (i64_t)obj);
            if (car->attrs & FN_SPECIAL_FORM)
                res = ((binary_f)car->i64)(args[0], args[1]);
            else
            {
                x = eval(args[0]);
                if (is_error(x))
                    return x;

                if (!(car->attrs & FN_AGGR) && x->type == TYPE_GROUPMAP)
                {
                    y = group_collect(x);
                    drop(x);
                    x = y;
                }
                else if (x->type == TYPE_FILTERMAP)
                {
                    y = filter_collect(x);
                    drop(x);
                    x = y;
                }

                y = eval(args[1]);
                if (is_error(y))
                {
                    drop(x);
                    return y;
                }

                if (!(car->attrs & FN_AGGR) && y->type == TYPE_GROUPMAP)
                {
                    z = group_collect(y);
                    drop(y);
                    y = z;
                }
                else if (y->type == TYPE_FILTERMAP)
                {
                    z = filter_collect(y);
                    drop(y);
                    y = z;
                }

                res = binary_call(car->attrs, (binary_f)car->i64, x, y);
                drop(x);
                drop(y);
            }

            return unwrap(res, (i64_t)obj);

        case TYPE_VARY:
            if (car->attrs & FN_SPECIAL_FORM)
                res = ((vary_f)car->i64)(args, len);
            else
            {
                if (!stack_enough(len))
                    return unwrap(error_str(ERR_STACK_OVERFLOW, "stack overflow"), (i64_t)obj);

                for (i = 0; i < len; i++)
                {
                    x = eval(args[i]);
                    if (is_error(x))
                        return x;

                    if (!(car->attrs & FN_AGGR) && x->type == TYPE_GROUPMAP)
                    {
                        attrs = FN_GROUP_MAP;
                        y = group_collect(x);
                        drop(x);
                        x = y;
                    }
                    else if (x->type == TYPE_FILTERMAP)
                    {
                        y = filter_collect(x);
                        drop(x);
                        x = y;
                    }

                    stack_push(x);
                }

                res = vary_call(car->attrs | attrs, (vary_f)car->i64, stack_peek(len - 1), len);

                for (i = 0; i < len; i++)
                    drop(stack_pop());
            }

            return unwrap(res, (i64_t)obj);

        case TYPE_LAMBDA:
            lambda = as_lambda(car);
            if (len != lambda->args->len)
                return unwrap(error_str(ERR_ARITY, "wrong number of arguments"), (i64_t)obj);

            if (!stack_enough(len))
                return unwrap(error_str(ERR_STACK_OVERFLOW, "stack overflow"), (i64_t)obj);

            for (i = 0; i < len; i++)
            {
                x = eval(args[i]);
                if (is_error(x))
                    return x;

                if (x->type == TYPE_GROUPMAP)
                {
                    attrs = FN_GROUP_MAP;
                    y = group_collect(x);
                    drop(x);
                    x = y;
                }
                else if (x->type == TYPE_FILTERMAP)
                {
                    y = filter_collect(x);
                    drop(x);
                    x = y;
                }

                stack_push(x);
            }

            return unwrap(lambda_call(attrs, car, stack_peek(len - 1), len), (i64_t)obj);

        case -TYPE_SYMBOL:
            val = deref(car);
            if (val == NULL)
                return unwrap(error_str(ERR_EVAL, "undefined symbol"), (i64_t)obj);
            car = *val;
            goto call;

        default:
            return unwrap(error_str(ERR_EVAL, "not a callable"), (i64_t)args[-1]);
        }
    case -TYPE_SYMBOL:
        if (obj->attrs & ATTR_QUOTED)
            return symboli64(obj->i64);
        val = deref(obj);
        if (val == NULL)
            return unwrap(error_str(ERR_EVAL, "undefined symbol"), (i64_t)obj);
        return clone(*val);
    default:
        return clone(obj);
    }
}

obj_t *deref(obj_t sym)
{
    i64_t bp, *args;
    obj_t lambda, env;
    u64_t i, l, n;
    ctx_t *ctx;

    ctx = ctx_get();
    lambda = ctx->lambda;

    if (sym->i64 == KW_SELF)
        return &ctx->lambda;

    l = as_lambda(lambda)->args->len;
    bp = ctx->sp;

    // search locals
    env = __INTERPRETER->stack[bp + l];
    if (env != NULL_OBJ)
    {
        n = as_list(env)[0]->len;
        // search in a reverse order
        for (i = n; i > 0; i--)
        {
            if (as_symbol(as_list(env)[0])[i - 1] == sym->i64)
                return &as_list(as_list(env)[1])[i - 1];
        }
    }

    // search args
    args = as_symbol(as_lambda(lambda)->args);
    for (i = 0; i < l; i++)
    {
        if (args[i] == sym->i64)
            return &__INTERPRETER->stack[bp + i];
    }

    // search globals
    i = find_raw(as_list(runtime_get()->env.variables)[0], &sym->i64);
    if (i == as_list(runtime_get()->env.variables)[0]->len)
        return NULL;

    return &as_list(as_list(runtime_get()->env.variables)[1])[i];
}

obj_t amend(obj_t sym, obj_t val)
{
    obj_t *env;
    obj_t lambda;
    i64_t bp;
    ctx_t *ctx;

    ctx = ctx_get();
    lambda = ctx->lambda;

    bp = ctx->sp;
    env = &__INTERPRETER->stack[bp + as_lambda(lambda)->args->len];

    if (*env != NULL_OBJ)
        set_obj(&as_list(*env)[0], sym, clone(val));
    else
    {
        *env = dict(vector(TYPE_SYMBOL, 1), vn_list(1, clone(val)));
        as_symbol(as_list(*env)[0])[0] = sym->i64;
    }

    return val;
}

obj_t mount_env(obj_t obj)
{
    u64_t i, l1, l2, l;
    obj_t *env;
    obj_t lambda, keys, vals;
    i64_t bp;
    ctx_t *ctx;

    ctx = ctx_get();
    lambda = ctx->lambda;

    bp = ctx->sp;
    env = &__INTERPRETER->stack[bp + as_lambda(lambda)->args->len];

    if (*env != NULL_OBJ)
    {
        l1 = as_list(*env)[0]->len;
        l2 = as_list(obj)[0]->len;
        l = l1 + l2;
        keys = vector_symbol(l);
        vals = list(l);

        for (i = 0; i < l1; i++)
        {
            as_symbol(keys)[i] = as_symbol(as_list(*env)[0])[i];
            as_list(vals)[i] = clone(as_list(as_list(*env)[1])[i]);
        }

        for (i = 0; i < l2; i++)
        {
            as_symbol(keys)[i + l1] = as_symbol(as_list(obj)[0])[i];
            as_list(vals)[i + l1] = clone(as_list(as_list(obj)[1])[i]);
        }

        drop(*env);
    }
    else
    {
        keys = clone(as_list(obj)[0]);
        vals = clone(as_list(obj)[1]);
    }

    *env = dict(keys, vals);

    return NULL_OBJ;
}

obj_t unmount_env(u64_t n)
{
    obj_t *env;
    obj_t lambda;
    i64_t bp;
    u64_t i, l;
    ctx_t *ctx;

    ctx = ctx_get();
    lambda = ctx->lambda;

    bp = ctx->sp;
    env = &__INTERPRETER->stack[bp + as_lambda(lambda)->args->len];

    if (ops_count(*env) == n)
    {
        drop(*env);
        *env = NULL_OBJ;
    }
    else
    {
        l = as_list(*env)[0]->len;
        resize(&as_list(*env)[0], l - n);

        // free values
        for (i = l; i > l - n; i--)
            drop(as_list(as_list(*env)[1])[i - 1]);

        resize(&as_list(*env)[1], l - n);
    }

    return NULL_OBJ;
}

obj_t ray_return(obj_t *x, u64_t n)
{
    if (__INTERPRETER->cp == 1)
        throw(ERR_NOT_SUPPORTED, "return outside of function");

    if (n == 0)
        stack_push(NULL_OBJ);
    else
        stack_push(clone(x[0]));

    longjmp(ctx_get()->jmp, 1);

    __builtin_unreachable();
}

obj_t ray_raise(obj_t obj)
{
    obj_t e;

    if (obj->type != TYPE_CHAR)
        throw(ERR_TYPE, "raise: expected 'string, got '%s", typename(obj->type));

    e = error_obj(ERR_RAISE, clone(obj));
    unwrap(e, (i64_t)obj);

    if (__INTERPRETER->cp == 1)
        return e;

    stack_push(e);

    longjmp(ctx_get()->jmp, 2);

    __builtin_unreachable();
}

obj_t parse_str(i64_t fd, obj_t str, obj_t file)
{
    unused(fd);
    obj_t info, res;

    if (str->type != TYPE_CHAR)
        throw(ERR_TYPE, "parse: expected string, got %s", typename(str->type));

    info = nfo(clone(file), clone(str));
    res = parse(as_string(str), info);
    drop(info);

    return res;
}

obj_t eval_obj(i64_t fd, obj_t obj)
{
    // eval onto self host
    if (fd == 0)
        return eval(obj);

    throw(ERR_NOT_IMPLEMENTED, "eval: not implemented");

    // sync request
    if (fd > 0)
    {
        // v = ser(y);
        // r = sock_send(x->i64, as_u8(v), v->len);
        // drop(v);

        // if (r == -1)
        //     throw(ERR_IO, "write: failed to write to socket: %s", get_os_error());

        // if (sock_recv(x->i64, (u8_t *)&header, sizeof(header_t)) == -1)
        //     throw(ERR_IO, "write: failed to read from socket: %s", get_os_error());

        // buf = heap_alloc(header.size + sizeof(header_t));
        // memcpy(buf, &header, sizeof(header_t));

        // if (sock_recv(x->i64, buf + sizeof(header_t), header.size) == -1)
        // {
        //     heap_free(buf);
        //     throw(ERR_IO, "write: failed to read from socket: %s", get_os_error());
        // }

        // v = de_raw(buf, header.size + sizeof(header_t));
        // heap_free(buf);

        // return v;
    }
}

obj_t eval_str(i64_t fd, obj_t str, obj_t file)
{
    unused(fd);
    obj_t parsed, res, info;
    ctx_t *ctx;
    i64_t sp;

    if (str->type != TYPE_CHAR)
        throw(ERR_TYPE, "eval: expected string, got %s", typename(str->type));

    info = nfo(clone(file), clone(str));
    parsed = parse(as_string(str), info);

    if (is_error(parsed))
    {
        drop(info);
        return parsed;
    }

    ctx = ctx_clone(info);

    if (setjmp(ctx->jmp) == 0)
    {
        res = eval(parsed);
        ctx_pop();
        if (__INTERPRETER->cp == 1)
            drop(ctx->lambda);
        drop(parsed);

        // cleanup stack frame
        sp = ctx->sp;
        while (__INTERPRETER->sp > sp)
            drop(stack_pop());

        return res;
    }

    ctx_pop();
    drop(parsed);
    if (__INTERPRETER->cp == 1)
        drop(ctx->lambda);
    res = stack_pop();

    // cleanup stack frame
    sp = ctx->sp;
    while (__INTERPRETER->sp > sp)
        drop(stack_pop());

    return res;
}

obj_t try_obj(obj_t obj, obj_t catch)
{
    ctx_t *ctx;
    i64_t sp;
    obj_t res;
    bool_t sig;

    ctx = ctx_clone(NULL_OBJ);

    switch (setjmp(ctx->jmp))
    {
    case 0:
        res = eval(obj);
        sig = false;
        break;
    case 1:
        res = stack_pop();
        sig = false;
        break;
    case 2:
    case 3:
        res = stack_pop();
        sig = true;
        break;
    default:
        __builtin_unreachable();
    }

    ctx = ctx_pop();
    sp = ctx->sp;

    if (__INTERPRETER->cp == 1)
        drop(ctx->lambda);

    // cleanup stack frame
    while (__INTERPRETER->sp > sp)
        drop(stack_pop());

    if (is_error(res) || sig)
    {
        if (catch)
        {
            if (catch->type == TYPE_LAMBDA)
            {
                if (as_lambda(catch)->args->len != 1)
                {
                    drop(res);
                    throw(ERR_LENGTH, "catch: expected 1 argument, got %llu", as_lambda(catch)->args->len);
                }
                if (is_error(res))
                {
                    stack_push(clone(as_error(res)->msg));
                    drop(res);
                }
                else
                    stack_push(res);

                return call(catch, 1);
            }

            drop(res);
            return clone(catch);
        }
        else
        {
            drop(res);
            return NULL_OBJ;
        }
    }

    return res;
}
