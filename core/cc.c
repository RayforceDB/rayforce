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
#include "function.h"
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
rf_object_t cc_compile_function(bool_t top, str_t name, rf_object_t args, rf_object_t *body, u32_t id, i32_t len, debuginfo_t *debuginfo);

#define push_u8(c, x)                            \
    {                                            \
        vector_reserve((c), 1);                  \
        as_string(c)[(c)->adt->len++] = (u8_t)x; \
    }

#define push_opcode(c, k, v, x)                                   \
    {                                                             \
        debuginfo_t *d = (c)->debuginfo;                          \
        debuginfo_t *p = &as_function(&(c)->function)->debuginfo; \
        span_t u = debuginfo_get(d, k);                           \
        debuginfo_insert(p, (u32_t)(v)->adt->len, u);             \
        push_u8(v, x);                                            \
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

#define push_const(c, k)                              \
    {                                                 \
        function_t *_f = as_function(&(c)->function); \
        push_u64(&_f->code, _f->constants.adt->len);  \
        vector_push(&_f->constants, k);               \
    }

#define cerr(c, i, t, e)                                              \
    {                                                                 \
        rf_object_free(&(c)->function);                               \
        (c)->function = error(t, e);                                  \
        (c)->function.adt->span = debuginfo_get((c)->debuginfo, (i)); \
        return CC_ERROR;                                              \
    }

#define ccerr(c, i, t, e)                                             \
    {                                                                 \
        str_t m = e;                                                  \
        rf_object_free(&(c)->function);                               \
        (c)->function = error(t, m);                                  \
        rf_free(m);                                                   \
        (c)->function.adt->span = debuginfo_get((c)->debuginfo, (i)); \
        return CC_ERROR;                                              \
    }

env_record_t *find_record(rf_object_t *records, rf_object_t *car, u32_t *arity)
{
    u32_t i, records_len;
    env_record_t *rec;

    *arity = *arity > MAX_ARITY ? MAX_ARITY + 1 : *arity;
    records_len = get_records_len(records, *arity);

    // try to find matching function prototype
    for (i = 0; i < records_len; i++)
    {
        rec = get_record(records, *arity, i);
        if (car->i64 == rec->id)
            return rec;
    }

    // Try to find in nary functions
    *arity = MAX_ARITY + 1;
    records_len = get_records_len(records, *arity);
    for (i = 0; i < records_len; i++)
    {
        rec = get_record(records, *arity, i);

        if (car->i64 == rec->id)
            return rec;
    }

    return NULL;
}

cc_result_t cc_compile_quote(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    UNUSED(arity);

    rf_object_t *car = &as_list(object)[0];
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;

    if (!has_consumer)
        return CC_NONE;

    push_opcode(cc, car->id, code, OP_PUSH);
    push_const(cc, rf_object_clone(&as_list(object)[1]));

    return as_list(object)[1].type;
}

cc_result_t cc_compile_time(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    cc_result_t res;
    rf_object_t *car = &as_list(object)[0];
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;

    if (!has_consumer)
        return CC_NULL;

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
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;

    if (!has_consumer)
        return CC_NULL;

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

    return CC_OK;
}

cc_result_t cc_compile_fn(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    UNUSED(has_consumer);

    rf_object_t *car = &as_list(object)[0], *b, fun;
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;

    if (arity < 2)
        cerr(cc, car->id, ERR_LENGTH, "'fn' expects vector of symbols with function arguments and a list body");

    b = as_list(object) + 1;

    arity -= 1;
    fun = cc_compile_function(false, "anonymous", rf_object_clone(b), b + 1, car->id, arity, cc->debuginfo);

    if (fun.type == TYPE_ERROR)
    {
        rf_object_free(&cc->function);
        cc->function = fun;
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
    function_t *func = as_function(&cc->function);
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

type_t cc_compile_try(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    type_t type, type1;
    i64_t lbl1, lbl2;
    rf_object_t *car = &as_list(object)[0];
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;
    env_t *env = &runtime_get()->env;

    if (car->i64 == symbol("try").i64)
    {
        if (arity != 2)
            cerr(cc, car->id, ERR_LENGTH, "'try': expects 2 arguments");

        push_opcode(cc, car->id, code, OP_TRY);
        push_u64(code, 0);
        lbl1 = code->adt->len - sizeof(u64_t);

        // compile expression under trap
        type = cc_compile_expr(true, cc, &as_list(object)[1]);

        if (type == TYPE_ERROR)
            return type;

        push_opcode(cc, car->id, code, OP_JMP);
        push_u64(code, 0);
        lbl2 = code->adt->len - sizeof(u64_t);

        *(u64_t *)(as_string(code) + lbl1) = code->adt->len;

        // compile expression under catch
        type1 = cc_compile_expr(has_consumer, cc, &as_list(object)[2]);

        if (type1 == TYPE_ERROR)
            return type1;

        if (type != type1)
            ccerr(cc, object->id, ERR_TYPE,
                  str_fmt(0, "'try': different types of expressions: '%s', '%s'",
                          symbols_get(env_get_typename_by_type(env, type)),
                          symbols_get(env_get_typename_by_type(env, type1))));

        *(u64_t *)(as_string(code) + lbl2) = code->adt->len;

        return type;
    }

    return TYPE_NONE;
}

type_t cc_compile_throw(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    UNUSED(has_consumer);

    type_t type;
    rf_object_t *car = &as_list(object)[0];
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;
    env_t *env = &runtime_get()->env;

    if (car->i64 == symbol("throw").i64)
    {
        if (arity != 1)
            cerr(cc, car->id, ERR_LENGTH, "'throw': expects 1 argument");

        type = cc_compile_expr(true, cc, &as_list(object)[1]);

        if (type == TYPE_ERROR)
            return type;

        push_opcode(cc, car->id, code, OP_THROW);

        if (type != TYPE_CHAR)
            ccerr(cc, object->id, ERR_TYPE,
                  str_fmt(0, "'throw': argument must be a 'Char', not '%s'",
                          symbols_get(env_get_typename_by_type(env, type))));

        return TYPE_THROW;
    }

    return TYPE_NONE;
}

type_t cc_compile_catch(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    UNUSED(has_consumer);

    rf_object_t *car = &as_list(object)[0];
    function_t *func = as_function(&cc->function);
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

type_t cc_compile_map(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    UNUSED(has_consumer);
    UNUSED(cc);
    UNUSED(object);
    UNUSED(arity);

    //     type_t type, *args;
    //     rf_object_t *car, *addr, *arg_keys, *arg_vals;
    //     function_t *func = as_function(&cc->function);
    //     rf_object_t *code = &func->code;
    //     env_t *env = &runtime_get()->env;
    //     i64_t i, lbl0, lbl1, lbl2, sym;

    //     car = &as_list(object)[0];
    //     if (car->i64 == symbol("map").i64)
    //     {
    //         if (arity < 2)
    //             cerr(cc, car->id, ERR_LENGTH, "'map' takes at least two arguments");

    //         arity -= 1;
    //         args = (type_t *)stack_malloc(arity * sizeof(type_t));

    //         // reserve space for map result
    //         push_opcode(cc, car->id, code, OP_PUSH);
    //         push_const(cc, null());

    //         // compile args
    //         for (i = 0; i < arity; i++)
    //         {
    //             type = cc_compile_expr(true, cc, &as_list(object)[i + 2]);

    //             if (type == TYPE_ERROR)
    //                 return type;

    //             args[i] = -type;
    //         }

    //         push_opcode(cc, car->id, code, OP_ALLOC);
    //         lbl0 = code->adt->len;
    //         push_u8(code, 0);
    //         push_u8(code, arity);
    //         // additional check for zero length argument
    //         push_opcode(cc, car->id, code, OP_JNE);
    //         push_u64(code, 0);
    //         lbl1 = code->adt->len - sizeof(u64_t);

    //         // compile function
    //         type = cc_compile_expr(true, cc, &as_list(object)[1]);

    //         if (type != TYPE_FUNCTION)
    //             cerr(cc, car->id, ERR_LENGTH, "'map' takes function as first argument");

    //         addr = &as_list(&func->constants)[func->constants.adt->len - 1];
    //         func = as_function(addr);

    //         // specify type for alloc result as return type of the function
    //         *(type_t *)(as_string(code) + lbl0) = func->rettype < 0 ? -func->rettype : TYPE_LIST;

    //         arg_keys = &as_list(&func->args)[0];
    //         arg_vals = &as_list(&func->args)[1];

    //         // check function arity
    //         if (arg_keys->adt->len != arity)
    //             cerr(cc, car->id, ERR_LENGTH, "'map' function arity mismatch");

    //         // check function argument types
    //         for (i = 0; i < arity; i++)
    //         {
    //             sym = as_vector_i64(arg_vals)[i];
    //             type = env_get_type_by_typename(env, sym);

    //             if (args[i] != type)
    //                 ccerr(cc, as_list(object)[i + 2].id, ERR_TYPE,
    //                       str_fmt(0, "argument type mismatch: expected %s, got %s",
    //                               symbols_get(env_get_typename_by_type(env, type)),
    //                               symbols_get(env_get_typename_by_type(env, args[i]))));
    //         }

    //         lbl2 = code->adt->len;
    //         push_opcode(cc, car->id, code, OP_MAP);
    //         push_u8(code, arity);
    //         push_opcode(cc, car->id, code, OP_CALLF);
    //         push_opcode(cc, car->id, code, OP_COLLECT);
    //         push_u8(code, arity);
    //         push_opcode(cc, car->id, code, OP_JNE);
    //         push_u64(code, lbl2);
    //         // pop function
    //         push_opcode(cc, car->id, code, OP_POP);
    //         // pop arguments
    //         for (i = 0; i < arity; i++)
    //             push_opcode(cc, car->id, code, OP_POP);

    //         *(u64_t *)(as_string(code) + lbl1) = code->adt->len;

    //         // additional one for ctx
    //         func->stack_size += 2;

    //         if (!has_consumer)
    //             push_opcode(cc, car->id, code, OP_POP);

    //         return type;
    //     }

    return TYPE_NONE;
}

type_t cc_compile_select(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    UNUSED(has_consumer);
    UNUSED(cc);
    UNUSED(object);
    UNUSED(arity);

    // type_t type, i;
    // i64_t offset;
    // rf_object_t *car, *params, key, val, *keys, *vals, *tkeys, *tvals, mtkeys, mtvals;
    // function_t *func = as_function(&cc->function);
    // rf_object_t *code = &func->code;
    // env_t *env = &runtime_get()->env;

    // car = &as_list(object)[0];
    // if (car->i64 == symbol("select").i64)
    // {
    //     if (arity == 0)
    //         cerr(cc, car->id, ERR_LENGTH, "'select' takes at least two arguments");

    //     params = &as_list(object)[1];

    //     if (params->type != TYPE_DICT)
    //         cerr(cc, car->id, ERR_LENGTH, "'select' takes dict of params");

    //     keys = &as_list(params)[0];
    //     vals = &as_list(params)[1];

    //     key = symbol("from");
    //     val = dict_get(params, &key);

    //     offset = 0;

    //     type = cc_compile_expr(true, cc, &val);
    //     rf_object_free(&val);

    //     if (type(type) != TYPE_TABLE)
    //         cerr(cc, car->id, ERR_LENGTH, "'select': 'from <Table>' is required");

    //     cc->table = (cc_table_t){.type = type, .offset = offset};

    //     // compile filters
    //     key = symbol("where");
    //     val = dict_get(params, &key);

    //     if (val.i64 != NULL_I64)
    //     {
    //         type = cc_compile_expr(true, cc, &val);
    //         rf_object_free(&val);

    //         if (type == TYPE_ERROR)
    //             return type;

    //         if (type != TYPE_BOOL)
    //             cerr(cc, car->id, ERR_TYPE, "'select': condition must have a Bool result");
    //     }
    //     else
    //         rf_object_free(&val);

    //     cc->table = (cc_table_t){.type = NULL_I64};

    //     push_opcode(cc, car->id, code, OP_CALL2);
    //     push_u64(code, rf_filter_Table_Bool);

    //     return type;
    //     // --
    // }

    return TYPE_NONE;
}
/*
 * Special forms are those that are not in a table of functions because of their special nature.
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
        res = cc_compile_quote(has_consumer, cc, object, arity);
        break;
    case KW_TIME:
        res = cc_compile_time(has_consumer, cc, object, arity);
        break;
    case KW_SET:
        res = cc_compile_set(has_consumer, cc, object, arity);
        break;
    case KW_FN:
        res = cc_compile_fn(has_consumer, cc, object, arity);
        break;
    case KW_IF:
        res = cc_compile_cond(has_consumer, cc, object, arity);
        break;
    }

    return res;
}

cc_result_t cc_compile_call(cc_t *cc, rf_object_t *car, u32_t arity)
{
    u32_t found_arity = arity;
    rf_object_t *code = &as_function(&cc->function)->code, *records = &runtime_get()->env.functions;
    env_record_t *rec = find_record(records, car, &found_arity);

    if (!rec)
        ccerr(cc, car->id, ERR_LENGTH, str_fmt(0, "function name/arity mismatch"));

    // It is an instruction
    if (rec->op < OP_INVALID)
    {
        push_opcode(cc, car->id, code, rec->op);
        return CC_OK;
    }

    // It is a function call
    switch (found_arity)
    {
    case 0:
        push_opcode(cc, car->id, code, OP_CALL0);
        break;
    case 1:
        push_opcode(cc, car->id, code, OP_CALL1);
        break;
    case 2:
        push_opcode(cc, car->id, code, OP_CALL2);
        break;
    case 3:
        push_opcode(cc, car->id, code, OP_CALL3);
        break;
    case 4:
        push_opcode(cc, car->id, code, OP_CALL4);
        break;
    default:
        push_opcode(cc, car->id, code, OP_CALLN);
        push_opcode(cc, car->id, code, arity);
    }

    push_u64(code, rec->op);
    return CC_OK;
}

cc_result_t cc_compile_expr(bool_t has_consumer, cc_t *cc, rf_object_t *object)
{
    rf_object_t *car;
    u32_t i, arity;
    i64_t id;
    function_t *func = as_function(&cc->function);
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

        // then try to find in locals
        id = vector_find(&func->locals, object);

        if (id < func->locals.adt->len)
        {
            push_opcode(cc, object->id, code, OP_LOAD);
            push_u64(code, 1 + id);
            func->stack_size++;

            return CC_OK;
        }

        // then try to search in the function args
        id = vector_find(&func->args, object);

        if (id < func->args.adt->len)
        {
            push_opcode(cc, object->id, code, OP_LOAD);
            push_u64(code, -(func->args.adt->len - id + 1));
            func->stack_size++;

            return CC_OK;
        }

        // then in a global env
        push_opcode(cc, object->id, code, OP_PUSH);
        push_const(cc, *object);
        push_opcode(cc, object->id, code, OP_CALL1);
        push_u64(code, rf_get_variable);
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

        if (car->type == -TYPE_SYMBOL && car->i64 < 0 && car->i64 != KW_SELF)
        {
            res = cc_compile_call(cc, car, arity);

            if (res == CC_ERROR)
                return CC_ERROR;

            if (res != CC_NONE)
            {
                if (!has_consumer)
                    push_opcode(cc, car->id, code, OP_POP);

                return res;
            }
        }

        // otherwise it is a function call
        res = cc_compile_expr(true, cc, car);
        if (res == CC_ERROR)
            return CC_ERROR;

        push_opcode(cc, car->id, code, OP_CALLF);

        return CC_OK;

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
 * Compile function
 */
rf_object_t cc_compile_function(bool_t top, str_t name, rf_object_t args,
                                rf_object_t *body, u32_t id, i32_t len, debuginfo_t *debuginfo)
{
    cc_t cc = {
        .top_level = top,
        .debuginfo = debuginfo,
        .function = function(args, vector_symbol(0), string(0),
                             debuginfo_new(debuginfo->filename, name)),
    };

    cc_result_t res;
    i32_t i;
    function_t *func = as_function(&cc.function);
    rf_object_t *code = &func->code, *b = body;

    if (len == 0)
    {
        push_opcode(&cc, id, code, OP_PUSH);
        push_const(&cc, null());
        goto epilogue;
    }
    // Compile all arguments but the last one
    for (i = 0; i < len - 1; i++)
    {
        b += i;
        // skip const expressions
        if (b->type != TYPE_LIST)
            continue;

        res = cc_compile_expr(false, &cc, b);

        if (res == CC_ERROR)
            return cc.function;
    }

    // Compile last argument
    b += i;
    res = cc_compile_expr(true, &cc, b);

    if (res == CC_ERROR)
        return cc.function;
    // --

epilogue:
    push_opcode(&cc, id, code, top ? OP_HALT : OP_RET);

    return cc.function;
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

    return cc_compile_function(true, "top-level", vector_symbol(0), b, body->id, len, debuginfo);
}
