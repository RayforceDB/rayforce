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

#include <stdio.h>
#include "cc.h"
#include "vm.h"
#include "alloc.h"
#include "format.h"
#include "util.h"
#include "vector.h"
#include "string.h"
#include "env.h"
#include "runtime.h"
#include "unary.h"
#include "binary.h"
#include "lambda.h"
#include "dict.h"
#include "ops.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#define stack_malloc(size) _alloca(size)
#else
#define stack_malloc(size) alloca(size)
#endif

typedef enum cc_result_t
{
    CC_OK,
    CC_NONE,
    CC_NULL,
    CC_ERROR,
} cc_result_t;

cc_result_t cc_compile_expr(bool_t has_consumer, cc_t *cc, rf_object_t *object);
rf_object_t cc_compile_lambda(bool_t top, str_t name, rf_object_t args, rf_object_t *body, u32_t id, i32_t len, debuginfo_t *debuginfo);

#define push_u8(c, x)                            \
    {                                            \
        vector_reserve((c), 1);                  \
        as_string(c)[(c)->adt->len++] = (u8_t)x; \
    }

#define push_opcode(c, k, v, x)                               \
    {                                                         \
        debuginfo_t *d = (c)->debuginfo;                      \
        debuginfo_t *p = &as_lambda(&(c)->lambda)->debuginfo; \
        span_t u = debuginfo_get(d, k);                       \
        debuginfo_insert(p, (u32_t)(v)->adt->len, u);         \
        push_u8(v, x);                                        \
    }

#define push_u64(c, x)                                                  \
    {                                                                   \
        str_t _p = align8(as_string(c) + (c)->adt->len);                \
        u64_t _o = _p - (as_string(c) + (c)->adt->len) + sizeof(u64_t); \
        vector_reserve((c), _o);                                        \
        _p = align8(as_string(c) + (c)->adt->len);                      \
        *(u64_t *)_p = (u64_t)x;                                        \
        (c)->adt->len += _o;                                            \
    }

#define push_const(c, k)                             \
    {                                                \
        lambda_t *_f = as_lambda(&(c)->lambda);      \
        push_u64(&_f->code, _f->constants.adt->len); \
        vector_push(&_f->constants, k);              \
    }

#define cerr(c, i, t, e)                                            \
    {                                                               \
        rf_object_free(&(c)->lambda);                               \
        (c)->lambda = error(t, e);                                  \
        (c)->lambda.adt->span = debuginfo_get((c)->debuginfo, (i)); \
        return CC_ERROR;                                            \
    }

#define ccerr(c, i, t, e)                                           \
    {                                                               \
        str_t m = e;                                                \
        rf_object_free(&(c)->lambda);                               \
        (c)->lambda = error(t, m);                                  \
        rf_free(m);                                                 \
        (c)->lambda.adt->span = debuginfo_get((c)->debuginfo, (i)); \
        return CC_ERROR;                                            \
    }

cc_result_t cc_compile_quote(bool_t has_consumer, cc_t *cc, rf_object_t *object)
{
    rf_object_t *car = &as_list(object)[0];
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;

    if (!has_consumer)
        return CC_NONE;

    push_opcode(cc, car->id, code, OP_PUSH);
    push_const(cc, rf_object_clone(&as_list(object)[1]));

    return as_list(object)[1].type;
}

cc_result_t cc_compile_time(cc_t *cc, rf_object_t *object, u32_t arity)
{
    cc_result_t res;
    rf_object_t *car = &as_list(object)[0];
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;

    if (arity != 1)
        cerr(cc, car->id, ERR_LENGTH, "'time' takes one argument");

    push_opcode(cc, car->id, code, OP_TIMER_SET);
    res = cc_compile_expr(false, cc, &as_list(object)[1]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car->id, code, OP_TIMER_GET);

    return CC_OK;
}

cc_result_t cc_compile_set(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    cc_result_t res;
    rf_object_t *car = &as_list(object)[0];
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;

    if (arity != 2)
        cerr(cc, car->id, ERR_LENGTH, "'set' takes two arguments");

    if (as_list(object)[1].type != -TYPE_SYMBOL)
        cerr(cc, car->id, ERR_TYPE, "'set' first argument must be a symbol");

    push_opcode(cc, car->id, code, OP_PUSH);
    push_const(cc, as_list(object)[1]);
    res = cc_compile_expr(true, cc, &as_list(object)[2]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car->id, code, OP_CALL2);
    push_u64(code, rf_set_variable);

    if (!has_consumer)
        push_opcode(cc, car->id, code, OP_POP);

    return CC_OK;
}

cc_result_t cc_compile_let(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    cc_result_t res;
    rf_object_t *car = &as_list(object)[0];
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;

    if (arity != 2)
        cerr(cc, car->id, ERR_LENGTH, "'let' takes two arguments");

    if (as_list(object)[1].type != -TYPE_SYMBOL)
        cerr(cc, car->id, ERR_TYPE, "'let' first argument must be a symbol");

    push_opcode(cc, car->id, code, OP_PUSH);
    push_const(cc, as_list(object)[1]);

    res = cc_compile_expr(true, cc, &as_list(object)[2]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car->id, code, OP_LSET);

    if (!has_consumer)
        push_opcode(cc, car->id, code, OP_POP);

    return CC_OK;
}

cc_result_t cc_compile_fn(cc_t *cc, rf_object_t *object, u32_t arity)
{
    rf_object_t *car = &as_list(object)[0], *b, fun;
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;

    if (arity < 2)
        cerr(cc, car->id, ERR_LENGTH, "'fn' expects vector of symbols with lambda arguments and a list body");

    b = as_list(object) + 1;

    arity -= 1;
    fun = cc_compile_lambda(false, "anonymous", rf_object_clone(b), b + 1, car->id, arity, cc->debuginfo);

    if (fun.type == TYPE_ERROR)
    {
        rf_object_free(&cc->lambda);
        cc->lambda = fun;
        return CC_ERROR;
    }

    push_opcode(cc, object->id, code, OP_PUSH);
    push_const(cc, fun);
    func->stack_size++;

    return CC_OK;
}

cc_result_t cc_compile_cond(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    cc_result_t res;
    i64_t lbl1, lbl2;
    rf_object_t *car = &as_list(object)[0];
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;

    if (arity < 2 || arity > 3)
        cerr(cc, car->id, ERR_LENGTH, "'if' expects 2 .. 3 arguments");

    res = cc_compile_expr(true, cc, &as_list(object)[1]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car->id, code, OP_JNE);
    push_u64(code, 0);
    lbl1 = code->adt->len - sizeof(u64_t);

    // true branch
    res = cc_compile_expr(has_consumer, cc, &as_list(object)[2]);

    if (res == CC_ERROR)
        return CC_ERROR;

    // there is else branch
    if (arity == 3)
    {
        push_opcode(cc, car->id, code, OP_JMP);
        push_u64(code, 0);
        lbl2 = code->adt->len - sizeof(u64_t);
        *(u64_t *)(as_string(code) + lbl1) = code->adt->len;

        // false branch
        res = cc_compile_expr(has_consumer, cc, &as_list(object)[3]);

        if (res == CC_ERROR)
            return CC_ERROR;

        *(u64_t *)(as_string(code) + lbl2) = code->adt->len;
    }
    else
        *(u64_t *)(as_string(code) + lbl1) = code->adt->len;

    return CC_OK;
}

cc_result_t cc_compile_try(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    cc_result_t res;
    i64_t lbl1, lbl2;
    rf_object_t *car = &as_list(object)[0];
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;

    if (arity != 2)
        cerr(cc, car->id, ERR_LENGTH, "'try': expects 2 arguments");

    push_opcode(cc, car->id, code, OP_TRY);
    push_u64(code, 0);
    lbl1 = code->adt->len - sizeof(u64_t);

    // compile expression under trap
    res = cc_compile_expr(true, cc, &as_list(object)[1]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car->id, code, OP_JMP);
    push_u64(code, 0);
    lbl2 = code->adt->len - sizeof(u64_t);

    *(u64_t *)(as_string(code) + lbl1) = code->adt->len;

    // compile expression under catch
    res = cc_compile_expr(has_consumer, cc, &as_list(object)[2]);

    if (res == CC_ERROR)
        return CC_ERROR;

    *(u64_t *)(as_string(code) + lbl2) = code->adt->len;

    return CC_OK;
}

cc_result_t cc_compile_throw(cc_t *cc, rf_object_t *object, u32_t arity)
{
    cc_result_t res;
    rf_object_t *car = &as_list(object)[0];
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;

    if (arity != 1)
        cerr(cc, car->id, ERR_LENGTH, "'throw': expects 1 argument");

    res = cc_compile_expr(true, cc, &as_list(object)[1]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car->id, code, OP_THROW);

    return CC_OK;
}

type_t cc_compile_catch(cc_t *cc, rf_object_t *object, u32_t arity)
{
    rf_object_t *car = &as_list(object)[0];
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;

    if (car->i64 == symbol("catch").i64)
    {
        if (arity != 0)
            cerr(cc, car->id, ERR_LENGTH, "'catch': expects 0 arguments");

        push_opcode(cc, car->id, code, OP_CATCH);

        return TYPE_CHAR;
    }

    return TYPE_NONE;
}

cc_result_t cc_compile_call(cc_t *cc, rf_object_t *car, u8_t arity)
{
    cc_result_t res;
    rf_object_t *code = &as_lambda(&cc->lambda)->code, rec;

    rec = dict_get(&runtime_get()->env.functions, car);

    switch (rec.type)
    {
    case TYPE_UNARY:
        if (arity != 1)
            cerr(cc, car->id, ERR_LENGTH, str_fmt(0, "unary function expects 1 argument"));
        push_opcode(cc, car->id, code, OP_CALL1);
        push_u64(code, rec.i64);
        return CC_OK;
    case TYPE_BINARY:
        if (arity != 2)
            cerr(cc, car->id, ERR_LENGTH, str_fmt(0, "binary function expects 2 arguments"));
        push_opcode(cc, car->id, code, OP_CALL2);
        push_u64(code, rec.i64);
        return CC_OK;
    case TYPE_VARY:
        push_opcode(cc, car->id, code, OP_CALLN);
        push_opcode(cc, car->id, code, arity);
        push_u64(code, rec.i64);
        return CC_OK;
    default:
        cc_compile_expr(true, cc, car);
        push_opcode(cc, car->id, code, OP_CALLD);
        push_opcode(cc, car->id, code, arity);
        return CC_OK;
    }
}

cc_result_t cc_compile_map(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    cc_result_t res = CC_NONE;
    rf_object_t *car;
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;
    i64_t i, lbl0, lbl1;

    if (arity < 2)
        cerr(cc, object->id, ERR_LENGTH, "'map' takes at least two arguments");

    object = as_list(object) + 1;
    car = object;

    arity -= 1;

    // reserve space for map result (accumulator)
    push_opcode(cc, car->id, code, OP_PUSH);
    push_const(cc, null());

    // compile args
    for (i = 0; i < arity; i++)
    {
        res = cc_compile_expr(true, cc, object + 1 + i);

        if (res == CC_ERROR)
            return CC_ERROR;
    }

    push_opcode(cc, car->id, code, OP_ALLOC);
    push_u8(code, arity);

    // check if iteration is done
    lbl0 = code->adt->len;
    push_opcode(cc, car->id, code, OP_JNE);
    push_u64(code, 0);
    lbl1 = code->adt->len - sizeof(u64_t);

    push_opcode(cc, car->id, code, OP_MAP);
    push_u8(code, arity);

    // compile lambda
    res = cc_compile_call(cc, object, arity);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car->id, code, OP_COLLECT);
    push_u8(code, arity);
    push_opcode(cc, car->id, code, OP_JMP);
    push_u64(code, lbl0);

    *(u64_t *)(as_string(code) + lbl1) = code->adt->len;

    // pop arguments
    for (i = 0; i < arity; i++)
        push_opcode(cc, car->id, code, OP_POP);

    // additional one for ctx
    func->stack_size += 2;

    push_opcode(cc, car->id, code, OP_CALL1);
    push_u64(code, vector_flatten);

    if (!has_consumer)
        push_opcode(cc, car->id, code, OP_POP);

    return CC_OK;
}

null_t find_used_symbols(rf_object_t *lst, rf_object_t *syms)
{
    i64_t i, l;

    switch (lst->type)
    {
    case -TYPE_SYMBOL:
        if (lst->i64 > 0)
            vector_push(syms, *lst);
        return;
    case TYPE_LIST:
        l = lst->adt->len;
        if (l == 0)
            return;
        for (i = 0; i < l; i++)
            find_used_symbols(&as_list(lst)[i], syms);
        return;
    default:
        return;
    }
}

cc_result_t cc_compile_select(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    cc_result_t res;
    i64_t i, l, k;
    rf_object_t *car, *params, key, val, cols, syms, g;
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;

    car = &as_list(object)[0];

    if (arity == 0)
        cerr(cc, car->id, ERR_LENGTH, "'select' takes at least two arguments");

    params = &as_list(object)[1];

    if (params->type != TYPE_DICT)
        cerr(cc, car->id, ERR_LENGTH, "'select' takes dict of params");

    l = as_list(params)[0].adt->len;

    // compile from
    key = symboli64(KW_FROM);
    val = dict_get(params, &key);
    res = cc_compile_expr(true, cc, &val);
    rf_object_free(&val);

    if (res == CC_ERROR)
        return CC_ERROR;

    // determine which of columns are used in select and which names will be used for result columns
    cols = vector_symbol(0);
    syms = vector_symbol(0);
    for (i = 0; i < l; i++)
    {
        k = as_vector_symbol(&as_list(params)[0])[i];
        if (k != KW_FROM && k != KW_WHERE && k != KW_BY)
        {
            find_used_symbols(&as_list(&as_list(params)[1])[i], &syms);
            vector_push(&cols, symboli64(k));
        }
    }
    // debug_object(&syms);

    // compile filters
    key = symboli64(KW_WHERE);
    val = dict_get(params, &key);
    if (val.type != TYPE_NULL)
    {
        // remap table of columns (if specified)
        if (cols.adt->len > 0)
        {
            push_opcode(cc, car->id, code, OP_PUSH);
            push_const(cc, syms);
            push_opcode(cc, car->id, code, OP_CALL2);
            push_u64(code, rf_take);
        }
        else
            rf_object_free(&syms);

        push_opcode(cc, car->id, code, OP_LATTACH);

        res = cc_compile_expr(true, cc, &val);
        rf_object_free(&val);

        if (res == CC_ERROR)
        {
            rf_object_free(&cols);
            return CC_ERROR;
        }

        push_opcode(cc, car->id, code, OP_CALL1);
        push_u64(code, rf_where);

        // remap table of columns (by applying filters)
        push_opcode(cc, car->id, code, OP_LDETACH);
        push_opcode(cc, car->id, code, OP_CALL2);
        push_u64(code, rf_take);
    }

    // group by
    key = symboli64(KW_BY);
    val = dict_get(params, &key);
    if (val.type != TYPE_NULL)
    {

        push_opcode(cc, car->id, code, OP_LATTACH);

        res = cc_compile_expr(true, cc, &val);
        rf_object_free(&val);

        if (res == CC_ERROR)
        {
            rf_object_free(&cols);
            return CC_ERROR;
        }

        push_opcode(cc, car->id, code, OP_CALL1);
        push_u64(code, rf_group);
        push_opcode(cc, car->id, code, OP_POP);

        // detach and drop table from env
        push_opcode(cc, car->id, code, OP_LDETACH);
        push_opcode(cc, car->id, code, OP_PUSH);
        push_const(cc, syms);
        push_opcode(cc, car->id, code, OP_CALL2);
        push_u64(code, rf_take);

        // push_opcode(cc, car->id, code, OP_CALL2);
        // push_u64(code, rf_take);
    }

    // compile mappings (if specified)
    if (cols.adt->len > 0)
    {
        push_opcode(cc, car->id, code, OP_LATTACH);
        push_opcode(cc, car->id, code, OP_PUSH);
        push_const(cc, cols);

        for (i = 0; i < l; i++)
        {
            k = as_vector_symbol(&as_list(params)[0])[i];
            if (k != KW_FROM && k != KW_WHERE && k != KW_BY)
            {
                val = as_list(&as_list(params)[1])[i];
                res = cc_compile_expr(true, cc, &val);

                if (res == CC_ERROR)
                    return CC_ERROR;
            }
        }

        push_opcode(cc, car->id, code, OP_CALLN);
        push_opcode(cc, car->id, code, (u8_t)cols.adt->len);
        push_u64(code, rf_list);

        push_opcode(cc, car->id, code, OP_CALL2);
        push_u64(code, rf_table);

        // detach and drop table from env
        push_opcode(cc, car->id, code, OP_LDETACH);
        push_opcode(cc, car->id, code, OP_POP);
    }
    else
        rf_object_free(&cols);

    if (!has_consumer)
        push_opcode(cc, car->id, code, OP_POP);

    return CC_OK;
}
/*
 * Special forms are those that are not in a table of lambdas because of their special nature.
 * return TYPE_ERROR if there is an error
 * return TYPE_NONE if it is not a special form
 * return type of the special form if it is a special form
 */
cc_result_t cc_compile_special_forms(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    cc_result_t res = CC_NONE;
    rf_object_t *car = &as_list(object)[0];

    switch (car->i64)
    {
    case KW_QUOTE:
        res = cc_compile_quote(has_consumer, cc, object);
        break;
    case KW_TIME:
        res = cc_compile_time(cc, object, arity);
        break;
    case KW_SET:
        res = cc_compile_set(has_consumer, cc, object, arity);
        break;
    case KW_LET:
        res = cc_compile_let(has_consumer, cc, object, arity);
        break;
    case KW_FN:
        res = cc_compile_fn(cc, object, arity);
        break;
    case KW_IF:
        res = cc_compile_cond(has_consumer, cc, object, arity);
        break;
    case KW_MAP:
        res = cc_compile_map(has_consumer, cc, object, arity);
        break;
    case KW_TRY:
        res = cc_compile_try(has_consumer, cc, object, arity);
        break;
    case KW_THROW:
        res = cc_compile_throw(cc, object, arity);
        break;
    case KW_SELECT:
        res = cc_compile_select(has_consumer, cc, object, arity);
        break;
    }

    return res;
}

cc_result_t cc_compile_expr(bool_t has_consumer, cc_t *cc, rf_object_t *object)
{
    rf_object_t *car;
    u32_t i, arity;
    i64_t id;
    lambda_t *func = as_lambda(&cc->lambda);
    rf_object_t *code = &func->code;
    cc_result_t res = CC_NONE;

    switch (object->type)
    {

    case -TYPE_SYMBOL:
        if (!has_consumer)
            return CC_NONE;

        // self is a special case
        if (object->i64 == KW_SELF)
        {
            push_opcode(cc, object->id, code, OP_LOAD);
            push_u64(code, -1);
            func->stack_size++;

            return CC_OK;
        }

        // internal function
        if (object->i64 < 0)
        {
            push_opcode(cc, object->id, code, OP_PUSH);
            push_const(cc, dict_get(&runtime_get()->env.functions, object));
            func->stack_size++;

            return CC_OK;
        }

        // try to search in the lambda args
        id = vector_find(&func->args, object);

        if (id < (i64_t)func->args.adt->len)
        {
            push_opcode(cc, object->id, code, OP_LOAD);
            push_u64(code, -(func->args.adt->len - id + 1));
            func->stack_size++;

            return CC_OK;
        }

        // then in a local or global env
        push_opcode(cc, object->id, code, OP_PUSH);
        push_const(cc, *object);
        push_opcode(cc, object->id, code, OP_LGET);
        func->stack_size++;

        return CC_OK;

    case TYPE_LIST:
        car = &as_list(object)[0];
        arity = object->adt->len - 1;

        // special forms compilation need to be done before arguments compilation
        res = cc_compile_special_forms(has_consumer, cc, object, arity);

        if (res == CC_ERROR || res != CC_NONE)
            return res;

        // compile arguments
        for (i = 0; i < arity; i++)
        {
            res = cc_compile_expr(true, cc, &as_list(object)[i + 1]);

            if (res == CC_ERROR)
                return CC_ERROR;
        }

        res = cc_compile_call(cc, car, arity);

        if (res == CC_ERROR)
            return CC_ERROR;

        if (!has_consumer)
            push_opcode(cc, object->id, code, OP_POP);

        return res;

    default:
        if (!has_consumer)
            return CC_NONE;

        push_opcode(cc, object->id, code, OP_PUSH);
        push_const(cc, rf_object_clone(object));
        func->stack_size++;

        return CC_OK;
    }
}

/*
 * Compile lambda
 */
rf_object_t cc_compile_lambda(bool_t top, str_t name, rf_object_t args,
                              rf_object_t *body, u32_t id, i32_t len, debuginfo_t *debuginfo)
{
    cc_t cc = {
        .top_level = top,
        .debuginfo = debuginfo,
        .lambda = lambda(args, string(0), debuginfo_new(debuginfo->filename, name)),
    };

    cc_result_t res;
    i32_t i;
    lambda_t *func = as_lambda(&cc.lambda);
    rf_object_t *code = &func->code, *b;

    if (len == 0)
    {
        push_opcode(&cc, id, code, OP_PUSH);
        push_const(&cc, null());
        goto epilogue;
    }
    // Compile all arguments but the last one
    for (i = 0; i < len - 1; i++)
    {
        b = body + i;
        // skip const expressions
        if (b->type != TYPE_LIST)
            continue;

        res = cc_compile_expr(false, &cc, b);

        if (res == CC_ERROR)
            return cc.lambda;
    }

    // Compile last argument
    b = body + i;
    res = cc_compile_expr(true, &cc, b);

    if (res == CC_ERROR)
        return cc.lambda;
    // --

epilogue:
    push_opcode(&cc, id, code, top ? OP_HALT : OP_RET);

    return cc.lambda;
}

/*
 * Compile top level expression
 */
rf_object_t cc_compile(rf_object_t *body, debuginfo_t *debuginfo)
{
    str_t msg;
    rf_object_t err;

    if (body->type != TYPE_LIST)
    {
        msg = str_fmt(0, "compile '%s': expected list", "top-level");
        err = error(ERR_TYPE, msg);
        rf_free(msg);
        return err;
    }

    rf_object_t *b = as_list(body);
    i32_t len = body->adt->len;

    return cc_compile_lambda(true, "top-level", vector_symbol(0), b, body->id, len, debuginfo);
}
