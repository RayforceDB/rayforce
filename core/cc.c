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
#include "heap.h"
#include "format.h"
#include "util.h"
#include "string.h"
#include "env.h"
#include "runtime.h"
#include "unary.h"
#include "vary.h"
#include "binary.h"
#include "lambda.h"
#include "ops.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#define stack_malloc(size) _alloca(size)
#else
#define stack_malloc(size) alloca(size)
#endif

#define push_u8(c, x)       \
    {                       \
        u8_t _x = x;        \
        push_raw((c), &_x); \
    }

#define push_const(c, k)                        \
    {                                           \
        obj_t _k = k;                           \
        lambda_t *_f = as_lambda((c)->lambda);  \
        push_u8(&_f->code, _f->constants->len); \
        push_raw(&_f->constants, &_k);          \
    }

#define push_opcode(c, k, v, x)                  \
    {                                            \
        nfo_t *d = (c)->nfo;                     \
        nfo_t *p = &as_lambda((c)->lambda)->nfo; \
        span_t u = nfo_get(d, (i64_t)k);         \
        nfo_insert(p, (u32_t)((*v))->len, u);    \
        push_u8(v, x);                           \
    }

#define push_un(c, x, n)                              \
    {                                                 \
        u64_t _l = (*c)->len;                         \
        str_t _p = align8(as_string(*c) + _l);        \
        n _o = _p - (as_string(*c) + _l) + sizeof(n); \
        resize((c), _l + _o);                         \
        _p = align8(as_string(*c) + _l);              \
        *(n *)_p = (n)x;                              \
    }

#define push_u32(c, x) push_un(c, x, u32_t)

#define push_u64(c, x) push_un(c, x, u64_t)

#define cerr(c, i, t, ...)                        \
    {                                             \
        str_t _m = str_fmt(0, __VA_ARGS__);       \
        nfo_t *_d = (c)->nfo;                     \
        span_t _u = nfo_get(_d, (i64_t)i);        \
        drop((c)->lambda);                        \
        (c)->lambda = error(t, _m);               \
        heap_free(_m);                            \
        *(span_t *)&as_list((c)->lambda)[2] = _u; \
        return CC_ERROR;                          \
    }

cc_result_t cc_compile_quote(bool_t has_consumer, cc_t *cc, obj_t obj)
{
    obj_t car = as_list(obj)[0];
    lambda_t *func = as_lambda(cc->lambda);
    obj_t *code = &func->code;

    if (!has_consumer)
        return CC_NULL;

    push_opcode(cc, car, code, OP_PUSH);
    push_const(cc, clone(as_list(obj)[1]));

    return CC_OK;
}

cc_result_t cc_compile_time(cc_t *cc, obj_t obj, u32_t arity)
{
    cc_result_t res;
    obj_t car = as_list(obj)[0];
    lambda_t *func = as_lambda(cc->lambda);
    obj_t *code = &func->code;

    if (arity != 1)
        cerr(cc, car, ERR_LENGTH, "'time' takes one argument");

    push_opcode(cc, car, code, OP_TIMER_SET);
    res = cc_compile_expr(false, cc, as_list(obj)[1]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car, code, OP_TIMER_GET);

    return CC_OK;
}

cc_result_t cc_compile_set(bool_t has_consumer, cc_t *cc, obj_t obj, u32_t arity)
{
    cc_result_t res;
    obj_t car = as_list(obj)[0];
    lambda_t *func = as_lambda(cc->lambda);
    obj_t *code = &func->code;

    if (arity != 2)
        cerr(cc, car, ERR_LENGTH, "'set' takes two arguments");

    push_opcode(cc, car, code, OP_PUSH);
    push_const(cc, clone(as_list(obj)[1]));
    res = cc_compile_expr(true, cc, as_list(obj)[2]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car, code, OP_CALL2);
    push_u8(code, 0);
    push_u64(code, rf_set);

    if (!has_consumer)
        push_opcode(cc, car, code, OP_POP);

    return CC_OK;
}

cc_result_t cc_compile_let(bool_t has_consumer, cc_t *cc, obj_t obj, u32_t arity)
{
    cc_result_t res;
    obj_t car = as_list(obj)[0];
    lambda_t *func = as_lambda(cc->lambda);
    obj_t *code = &func->code;

    if (arity != 2)
        cerr(cc, car, ERR_LENGTH, "'let' takes two arguments");

    if (as_list(obj)[1]->type != -TYPE_SYMBOL)
        cerr(cc, car, ERR_TYPE, "'let' first argument must be a symbol");

    push_opcode(cc, car, code, OP_PUSH);
    push_const(cc, clone(as_list(obj)[1]));

    res = cc_compile_expr(true, cc, as_list(obj)[2]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car, code, OP_LSET);

    if (!has_consumer)
        push_opcode(cc, car, code, OP_POP);

    return CC_OK;
}

cc_result_t cc_compile_fn(cc_t *cc, obj_t obj, u32_t arity)
{
    obj_t fun;
    lambda_t *func = as_lambda(cc->lambda);
    obj_t *code = &func->code;

    if (arity < 2)
        cerr(cc, obj, ERR_LENGTH, "'fn' expects vector of symbols with lambda arguments and a list body");

    arity -= 1;
    fun = cc_compile_lambda("anonymous", clone(*(as_list(obj) + 1)), rf_list(as_list(obj) + 2, arity), cc->nfo);
    if (fun->type == TYPE_ERROR)
    {
        drop(cc->lambda);
        cc->lambda = fun;
        return CC_ERROR;
    }

    push_opcode(cc, obj, code, OP_PUSH);
    push_const(cc, fun);
    func->stack_size++;

    return CC_OK;
}

cc_result_t cc_compile_cond(bool_t has_consumer, cc_t *cc, obj_t obj, u32_t arity)
{
    cc_result_t res;
    i64_t lbl1, lbl2;
    obj_t car = as_list(obj)[0];
    lambda_t *func = as_lambda(cc->lambda);
    obj_t *code = &func->code;

    if (arity < 2 || arity > 3)
        cerr(cc, car, ERR_LENGTH, "'if' expects 2 .. 3 arguments");

    res = cc_compile_expr(true, cc, as_list(obj)[1]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car, code, OP_JNE);
    push_u64(code, 0);
    lbl1 = (*code)->len - sizeof(u64_t);

    // true branch
    res = cc_compile_expr(has_consumer, cc, as_list(obj)[2]);

    if (res == CC_ERROR)
        return CC_ERROR;

    // there is else branch
    if (arity == 3)
    {
        push_opcode(cc, car, code, OP_JMP);
        push_u64(code, 0);
        lbl2 = (*code)->len - sizeof(u64_t);
        *(u64_t *)(as_string(*code) + lbl1) = (*code)->len;

        // false branch
        res = cc_compile_expr(has_consumer, cc, as_list(obj)[3]);

        if (res == CC_ERROR)
            return CC_ERROR;

        *(u64_t *)(as_string(*code) + lbl2) = (*code)->len;
    }
    else
        *(u64_t *)(as_string(*code) + lbl1) = (*code)->len;

    return CC_OK;
}

cc_result_t cc_compile_try(bool_t has_consumer, cc_t *cc, obj_t obj, u32_t arity)
{
    cc_result_t res;
    i64_t lbl1, lbl2;
    obj_t car = as_list(obj)[0];
    lambda_t *func = as_lambda(&cc->lambda);
    obj_t *code = &func->code;

    if (arity != 2)
        cerr(cc, car, ERR_LENGTH, "'try': expects 2 arguments");

    push_opcode(cc, car, code, OP_TRY);
    push_u64(code, 0);
    lbl1 = (*code)->len - sizeof(u64_t);

    // compile expression under trap
    res = cc_compile_expr(true, cc, as_list(obj)[1]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car, code, OP_JMP);
    push_u64(code, 0);
    lbl2 = (*code)->len - sizeof(u64_t);

    *(u64_t *)(as_string(*code) + lbl1) = (*code)->len;

    // compile expression under catch
    res = cc_compile_expr(has_consumer, cc, as_list(obj)[2]);

    if (res == CC_ERROR)
        return CC_ERROR;

    *(u64_t *)(as_string(*code) + lbl2) = (*code)->len;

    return CC_OK;
}

cc_result_t cc_compile_throw(cc_t *cc, obj_t obj, u32_t arity)
{
    cc_result_t res;
    obj_t car = as_list(obj)[0];
    lambda_t *func = as_lambda(&cc->lambda);
    obj_t *code = &func->code;

    if (arity != 1)
        cerr(cc, car, ERR_LENGTH, "'throw': expects 1 argument");

    res = cc_compile_expr(true, cc, as_list(obj)[1]);

    if (res == CC_ERROR)
        return CC_ERROR;

    push_opcode(cc, car, code, OP_THROW);

    return CC_OK;
}

type_t cc_compile_catch(cc_t *cc, obj_t obj, u32_t arity)
{
    obj_t car = as_list(obj)[0];
    lambda_t *func = as_lambda(&cc->lambda);
    obj_t *code = &func->code;

    if (arity != 0)
        cerr(cc, car, ERR_LENGTH, "'catch': expects 0 arguments");

    push_opcode(cc, car, code, OP_CATCH);

    return CC_OK;
}

cc_result_t cc_compile_call(cc_t *cc, obj_t car, u8_t arity)
{
    cc_result_t res;
    lambda_t *func = as_lambda(cc->lambda);
    obj_t *code = &func->code, prim;
    i64_t i;

    // self is a special case
    if (car->type == -TYPE_SYMBOL && car->i64 == KW_SELF)
    {
        push_opcode(cc, car, code, OP_PUSH);
        push_const(cc, cc->lambda);

        push_opcode(cc, car, code, OP_CALLD);
        push_u8(code, arity);

        func->stack_size++;

        return CC_OK;
    }

    i = find_obj(as_list(runtime_get()->env.functions)[0], car);

    if (i == (i64_t)as_list(runtime_get()->env.functions)[0]->len)
    {
        res = cc_compile_expr(true, cc, car);

        if (res == CC_ERROR)
            return CC_ERROR;

        push_opcode(cc, car, code, OP_CALLD);
        push_u8(code, arity);

        func->stack_size++;

        return CC_OK;
    }

    prim = as_list(as_list(runtime_get()->env.functions)[1])[i];

    switch (prim->type)
    {
    case TYPE_UNARY:
        if (arity != 1)
            cerr(cc, car, ERR_LENGTH, "unary function expects 1 argument");

        push_opcode(cc, car, code, OP_CALL1);
        push_u8(code, prim->attrs);
        push_u64(code, prim->i64);

        return CC_OK;
    case TYPE_BINARY:
        if (arity != 2)
            cerr(cc, car, ERR_LENGTH, "binary function expects 2 arguments");

        push_opcode(cc, car, code, OP_CALL2);
        push_u8(code, prim->attrs);
        push_u64(code, prim->i64);

        return CC_OK;
    case TYPE_VARY:
        push_opcode(cc, car, code, OP_CALLN);
        push_u8(code, arity);
        push_u8(code, prim->attrs);
        push_u64(code, prim->i64);

        return CC_OK;
    default:
        cerr(cc, car, ERR_NOT_FOUND, "function not found");
    }
}

nil_t find_used_symbols(obj_t lst, obj_t *syms)
{
    i64_t i, l;

    switch (lst->type)
    {
    case -TYPE_SYMBOL:
        if (lst->i64 > 0)
            push_raw(syms, &lst->i64);
        return;
    case TYPE_LIST:
        l = lst->len;
        if (l == 0)
            return;
        for (i = 0; i < l; i++)
            find_used_symbols(as_list(lst)[i], syms);
        return;
    default:
        return;
    }
}

cc_result_t cc_compile_select(bool_t has_consumer, cc_t *cc, obj_t obj, u32_t arity)
{
    cc_result_t res;
    i64_t i, l;
    obj_t car, params, key, val, cols, syms, k, v;
    lambda_t *func = as_lambda(cc->lambda);
    obj_t *code = &func->code, take = null(0);
    bool_t groupby = false, map = false;

    car = as_list(obj)[0];

    if (arity == 0)
        cerr(cc, car, ERR_LENGTH, "'select' takes at least two arguments");

    params = as_list(obj)[1];

    if (params->type != TYPE_DICT)
        cerr(cc, car, ERR_LENGTH, "'select' takes dict of params");

    l = as_list(params)[0]->len;

    // compile table
    key = symboli64(KW_FROM);
    val = at_obj(params, key);

    if (is_null(val))
        cerr(cc, car, ERR_LENGTH, "'select' expects 'from' param");

    res = cc_compile_expr(true, cc, val);
    dropn(2, key, val);

    if (res == CC_ERROR)
        return CC_ERROR;

    // determine which of columns are used in select and which names will be used for result columns
    cols = vector_symbol(0);
    syms = vector_symbol(0);

    // first check by because it is special case in mapping
    key = symboli64(KW_BY);
    val = at_obj(params, key);

    if (!is_null(val))
    {
        groupby = true;
        if (val->type == -TYPE_SYMBOL)
            push_obj(&cols, clone(val));
        else
            push_sym(&cols, "x");
    }

    dropn(2, key, val);

    for (i = 0; i < l; i++)
    {
        k = at_idx(as_list(params)[0], i);

        if (k->i64 != KW_FROM && k->i64 != KW_WHERE)
        {
            v = at_obj(params, k);

            if (k->i64 == KW_TAKE)
            {
                drop(k);
                take = v;
                continue;
            }

            find_used_symbols(v, &syms);

            if (k->i64 == KW_BY)
            {
                dropn(2, k, v);
                continue;
            }

            push_obj(&cols, k);
            drop(v);
            map = true;
        }
        else
            drop(k);
    }

    k = rf_distinct(syms);

    if (is_error(k))
    {
        dropn(2, cols, syms);
        cerr(cc, car, ERR_TYPE, "invalid column name");
    }

    drop(syms);

    // compile filters
    key = symboli64(KW_WHERE);
    val = at_obj(params, key);
    drop(key);

    if (!is_null(val))
    {
        push_opcode(cc, car, code, OP_LPUSH);

        res = cc_compile_expr(true, cc, val);

        if (res == CC_ERROR)
        {
            dropn(2, cols, val);
            return CC_ERROR;
        }

        push_opcode(cc, obj, code, OP_CALL1);
        push_u8(code, 0);
        push_u64(code, rf_where);
        push_opcode(cc, val, code, OP_LPOP);

        // reduce by used columns (if any)
        if (map)
        {
            push_opcode(cc, val, code, OP_DUP);
            push_opcode(cc, val, code, OP_CALL1);
            push_u8(code, 0);
            push_u64(code, rf_key);
            push_opcode(cc, val, code, OP_PUSH);
            push_const(cc, k);
            push_opcode(cc, val, code, OP_CALL2);
            push_u8(code, 0);
            push_u64(code, rf_sect);
            push_opcode(cc, val, code, OP_CALL2);
            push_u8(code, 0);
            push_u64(code, rf_at);
        }
        else
            drop(k);

        // create vecmaps over the table
        push_opcode(cc, val, code, OP_SWAP);
        push_opcode(cc, val, code, OP_CALL2);
        push_u8(code, 0);
        push_u64(code, rf_vecmap);
        drop(val);
    }
    else
    {
        // reduce by used columns (if any)
        if (map)
        {
            push_opcode(cc, params, code, OP_DUP);
            push_opcode(cc, params, code, OP_CALL1);
            push_u8(code, 0);
            push_u64(code, rf_key);
            push_opcode(cc, params, code, OP_PUSH);
            push_const(cc, k);
            push_opcode(cc, params, code, OP_CALL2);
            push_u8(code, 0);
            push_u64(code, rf_sect);
            push_opcode(cc, params, code, OP_CALL2);
            push_u8(code, 0);
            push_u64(code, rf_at);
        }
        else
            drop(k);
    }

    // compile take (if any)
    if (!is_null(take))
    {
        push_opcode(cc, car, code, OP_PUSH);
        push_const(cc, take);
        push_opcode(cc, car, code, OP_SWAP);
        push_opcode(cc, car, code, OP_CALL2);
        push_u8(code, 0);
        push_u64(code, rf_take);
    }

    if (map || groupby)
        push_opcode(cc, car, code, OP_LPUSH);

    // Group?
    key = symboli64(KW_BY);
    val = at_obj(params, key);
    drop(key);

    if (!is_null(val))
    {
        res = cc_compile_expr(true, cc, val);
        drop(val);

        if (res == CC_ERROR)
        {
            drop(cols);
            return CC_ERROR;
        }

        push_opcode(cc, obj, code, OP_CALL1);
        push_opcode(cc, obj, code, 0);
        push_u64(code, rf_group);

        push_opcode(cc, obj, code, OP_DUP);
        push_opcode(cc, obj, code, OP_CALL1);
        push_opcode(cc, obj, code, 0);
        push_u64(code, rf_value);
        push_opcode(cc, car, code, OP_LPOP);

        // remove column used for grouping from result
        push_opcode(cc, obj, code, OP_DUP);
        push_opcode(cc, car, code, OP_CALL1);
        push_opcode(cc, car, code, 0);
        push_u64(code, rf_key);
        push_opcode(cc, car, code, OP_PUSH);
        push_const(cc, at_idx(cols, 0));
        push_opcode(cc, car, code, OP_CALL2);
        push_u8(code, 0);
        push_u64(code, rf_except);
        push_opcode(cc, car, code, OP_CALL2);
        push_opcode(cc, car, code, 0);
        push_u64(code, rf_at);

        push_opcode(cc, car, code, OP_SWAP);

        // apply grouping
        push_opcode(cc, car, code, OP_CALL2);
        push_u8(code, 0);
        push_u64(code, rf_group_Table);

        push_opcode(cc, car, code, OP_LPUSH);

        push_opcode(cc, obj, code, OP_CALL1);
        push_u8(code, 0);
        push_u64(code, rf_key);

        if (!map)
            push_opcode(cc, car, code, OP_LPOP);
    }

    // compile mappings (if specified)
    if (map)
    {
        push_opcode(cc, car, code, OP_PUSH);
        push_const(cc, cols);

        if (groupby)
            push_opcode(cc, car, code, OP_SWAP);

        for (i = 0; i < l; i++)
        {
            k = at_idx(as_list(params)[0], i);
            if (k->i64 != KW_FROM && k->i64 != KW_WHERE && k->i64 != KW_BY && k->i64 != KW_TAKE)
            {
                v = at_idx(as_list(params)[1], i);
                res = cc_compile_expr(true, cc, v);
                dropn(2, k, v);

                if (res == CC_ERROR)
                    return CC_ERROR;
            }
            else
                drop(k);
        }

        push_opcode(cc, car, code, OP_CALLN);
        push_u8(code, cols->len);
        push_u8(code, 0);
        push_u64(code, rf_list);

        push_opcode(cc, car, code, OP_CALL2);
        push_u8(code, 0);
        push_u64(code, rf_table);
    }
    else
        drop(cols);

    if (!has_consumer)
        push_opcode(cc, car, code, OP_POP);

    return CC_OK;
}

cc_result_t cc_compile_return(cc_t *cc, obj_t obj, u32_t arity)
{
    cc_result_t res;
    obj_t car = as_list(obj)[0];
    lambda_t *func = as_lambda(cc->lambda);
    obj_t *code = &func->code;

    switch (arity)
    {
    case 0:
        push_opcode(cc, car, code, OP_PUSH);
        push_const(cc, null(0));
        break;
    case 1:
        res = cc_compile_expr(true, cc, as_list(obj)[1]);
        if (res == CC_ERROR)
            return res;
        break;
    default:
        cerr(cc, obj, ERR_LENGTH, "'return' takes at most one argument");
    }

    push_opcode(cc, car, code, OP_RET);

    return CC_OK;
}

cc_result_t cc_compile_special_forms(bool_t has_consumer, cc_t *cc, obj_t obj, u32_t arity)
{
    switch (as_list(obj)[0]->i64)
    {
    case KW_QUOTE:
        return cc_compile_quote(has_consumer, cc, obj);
    case KW_TIME:
        return cc_compile_time(cc, obj, arity);
    case KW_SET:
        return cc_compile_set(has_consumer, cc, obj, arity);
    case KW_LET:
        return cc_compile_let(has_consumer, cc, obj, arity);
    case KW_FN:
        return cc_compile_fn(cc, obj, arity);
    case KW_IF:
        return cc_compile_cond(has_consumer, cc, obj, arity);
    case KW_TRY:
        return cc_compile_try(has_consumer, cc, obj, arity);
    case KW_THROW:
        return cc_compile_throw(cc, obj, arity);
    case KW_SELECT:
        return cc_compile_select(has_consumer, cc, obj, arity);
    // case KW_EVAL:
    //     return cc_compile_eval(has_consumer, cc, obj, arity);
    // case KW_LOAD:
    //     return cc_compile_load(has_consumer, cc, obj, arity);
    case KW_RETURN:
        return cc_compile_return(cc, obj, arity);
    default:
        return CC_NULL;
    }
}

cc_result_t cc_compile_expr(bool_t has_consumer, cc_t *cc, obj_t obj)
{
    u32_t i, arity;
    i64_t id;
    lambda_t *func = as_lambda(cc->lambda);
    obj_t *code = &func->code;
    cc_result_t res = CC_NULL;

    if (!obj)
    {
        if (!has_consumer)
            return CC_NULL;

        // then in a local or global env
        push_opcode(cc, obj, code, OP_PUSH);
        push_const(cc, clone(obj));
        func->stack_size++;

        return CC_OK;
    }

    switch (obj->type)
    {
    case -TYPE_SYMBOL:
        if (!has_consumer)
            return CC_NULL;

        // Symbol is quoted
        if (obj->attrs & ATTR_QUOTED)
        {
            push_opcode(cc, obj, code, OP_PUSH);
            push_const(cc, symboli64(obj->i64));
            func->stack_size++;

            return CC_OK;
        }

        // internal function
        if (obj->i64 < 0)
        {
            push_opcode(cc, obj, code, OP_PUSH);

            id = find_obj(as_list(runtime_get()->env.functions)[0], obj);

            if (id == (i64_t)as_list(runtime_get()->env.functions)[0]->len)
                cerr(cc, obj, ERR_LENGTH, "function not found");

            push_const(cc, clone(as_list(as_list(runtime_get()->env.functions)[1])[id]));
            func->stack_size++;

            return CC_OK;
        }

        // try to search in the lambda args
        id = find_obj(func->args, obj);

        if (id < (i64_t)func->args->len)
        {
            push_opcode(cc, obj, code, OP_LOAD);
            push_u64(code, func->args->len - id);
            func->stack_size++;

            return CC_OK;
        }

        // then in a local or global env
        push_opcode(cc, obj, code, OP_PUSH);
        push_const(cc, clone(obj));
        push_opcode(cc, obj, code, OP_LGET);
        func->stack_size++;

        return CC_OK;

    case TYPE_LIST:
        if (obj->len == 0)
            goto other;

        arity = obj->len - 1;

        // special forms compilation need to be done before arguments compilation
        res = cc_compile_special_forms(has_consumer, cc, obj, arity);

        if (res == CC_ERROR || res != CC_NULL)
            return res;

        // compile arguments
        for (i = 0; i < arity; i++)
        {
            res = cc_compile_expr(true, cc, as_list(obj)[i + 1]);
            if (res == CC_ERROR)
                return CC_ERROR;
        }

        res = cc_compile_call(cc, as_list(obj)[0], arity);

        if (res == CC_ERROR)
            return CC_ERROR;

        if (!has_consumer)
            push_opcode(cc, obj, code, OP_POP);

        return res;

    default:
    other:
        if (!has_consumer)
            return CC_NULL;

        push_opcode(cc, obj, code, OP_PUSH);
        push_const(cc, clone(obj));
        func->stack_size++;

        return CC_OK;
    }

    return CC_OK;
}

obj_t cc_compile_lambda(str_t name, obj_t args, obj_t body, nfo_t *nfo)
{
    span_t span;
    str_t msg;
    obj_t err, *code;
    nfo_t *pi, di;
    cc_result_t res;
    u64_t i = 0, len;
    lambda_t *func;

    if (body->type != TYPE_LIST)
    {
        span = nfo_get(nfo, (i64_t)body);
        msg = str_fmt(0, "compile '%s': expected list", "top-level");
        err = error(ERR_TYPE, msg);
        heap_free(msg);
        *(span_t *)as_list(err)[2] = span;

        return err;
    }

    len = body->len;

    if (nfo == NULL)
    {
        di = nfo_new("top-level", name);
        pi = &di;
    }
    else
    {
        di = nfo_new(nfo->filename, name);
        pi = nfo;
    }

    cc_t cc = {
        .nfo = pi,
        .lambda = lambda(args, body, string(0), di),
    };

    func = as_lambda(cc.lambda);
    code = &func->code;

    if (len == 0)
    {
        push_opcode(&cc, body, code, OP_PUSH);
        push_const(&cc, null(0));
        goto epilogue;
    }

    // Compile all arguments but the last one
    for (i = 0; i < len - 1; i++)
    {
        // skip const expressions
        if (as_list(body)[i]->type != TYPE_LIST)
            continue;

        res = cc_compile_expr(false, &cc, as_list(body)[i]);

        if (res == CC_ERROR)
            return cc.lambda;
    }

    // Compile last argument
    res = cc_compile_expr(true, &cc, as_list(body)[i]);

    if (res == CC_ERROR)
        return cc.lambda;
    // --

epilogue:
    push_opcode(&cc, body, code, OP_RET);

    return cc.lambda;
}

/*
 * Compile top level expression as an anonymous lambda
 */
obj_t cc_compile(obj_t body, nfo_t *nfo)
{
    return cc_compile_lambda("top-level", vector_symbol(0), clone(body), nfo);
}
