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
#include "binary.h"
#include "function.h"
#include "dict.h"

i8_t cc_compile_expr(bool_t has_consumer, cc_t *cc, rf_object_t *object);
rf_object_t cc_compile_function(bool_t top, str_t name, i8_t rettype, rf_object_t args, rf_object_t *body, u32_t id, i32_t len, debuginfo_t *debuginfo);

#define push_opcode(c, k, v, x)                                   \
    {                                                             \
        debuginfo_t *d = (c)->debuginfo;                          \
        debuginfo_t *p = &as_function(&(c)->function)->debuginfo; \
        span_t u = debuginfo_get(d, k);                           \
        debuginfo_insert(p, (u32_t)(v)->adt->len, u);             \
        vector_reserve(v, 1);                                     \
        as_string(v)[(v)->adt->len++] = (i8_t)x;                  \
    }

#define push_rf_object(c, x)                                \
    {                                                       \
        vector_reserve(c, sizeof(rf_object_t));             \
        *(rf_object_t *)(as_string(c) + (c)->adt->len) = x; \
        (c)->adt->len += sizeof(rf_object_t);               \
    }

#define peek_rf_object(c) (rf_object_t *)(as_string(c) + (c)->adt->len - sizeof(rf_object_t))

#define cerr(c, i, t, e)                                              \
    {                                                                 \
        rf_object_free(&(c)->function);                               \
        (c)->function = error(t, e);                                  \
        (c)->function.adt->span = debuginfo_get((c)->debuginfo, (i)); \
        return TYPE_ERROR;                                            \
    }

#define ccerr(c, i, t, e)                                             \
    {                                                                 \
        str_t m = e;                                                  \
        rf_object_free(&(c)->function);                               \
        (c)->function = error(t, m);                                  \
        rf_free(m);                                                   \
        (c)->function.adt->span = debuginfo_get((c)->debuginfo, (i)); \
        return TYPE_ERROR;                                            \
    }

env_record_t *find_record(rf_object_t *records, rf_object_t *car, i8_t *args, u32_t *arity)
{
    bool_t match_rec = false;
    u8_t match_args = 0;
    u32_t i, j, records_len;
    env_record_t *rec;

    *arity = *arity > MAX_ARITY ? MAX_ARITY + 1 : *arity;
    records_len = get_records_len(records, *arity);

    // try to find matching function prototype
    for (i = 0; i < records_len; i++)
    {
        rec = get_record(records, *arity, i);

        if (car->i64 == rec->id)
        {
            match_rec = true;
            for (j = 0; j < *arity; j++)
            {
                if (rec->args[j] == TYPE_ANY)
                    match_args++;
                else if (rec->args[j] == args[j])
                    match_args++;
                else
                    break;
            }
        }

        if (match_rec && match_args == *arity)
            return rec;

        match_rec = false;
        match_args = 0;
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

i8_t cc_compile_time(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    i8_t type;
    rf_object_t *car = &as_list(object)[0];
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;

    // compile special forms
    if (car->i64 == symbol("time").i64)
    {
        if (!has_consumer)
            return TYPE_NULL;

        if (arity != 1)
            cerr(cc, car->id, ERR_LENGTH, "'time' takes one argument");

        push_opcode(cc, car->id, code, OP_TIMER_SET);
        type = cc_compile_expr(false, cc, &as_list(object)[1]);

        if (type == TYPE_ERROR)
            return TYPE_ERROR;

        push_opcode(cc, car->id, code, OP_TIMER_GET);

        return -TYPE_F64;
    }

    return TYPE_NONE;
}

i8_t cc_compile_set(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    i8_t type;
    i64_t id;
    rf_object_t *car = &as_list(object)[0], *addr, name;
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;

    if (car->i64 == symbol("set").i64)
    {
        if (arity != 2)
            cerr(cc, car->id, ERR_LENGTH, "'set' takes two arguments");

        if (as_list(object)[1].type != -TYPE_SYMBOL)
            cerr(cc, car->id, ERR_TYPE, "'set' takes symbol as first argument");

        type = cc_compile_expr(true, cc, &as_list(object)[2]);

        if (type == TYPE_ERROR)
            return TYPE_ERROR;

        // check if variable is not set or has the same type
        addr = env_get_variable(&runtime_get()->env, &as_list(object)[1]);

        if (addr != NULL && type != addr->type)
            cerr(cc, car->id, ERR_TYPE, "'set': variable type mismatch");

        push_opcode(cc, car->id, code, OP_GSET);
        push_rf_object(code, as_list(object)[1]);

        if (!has_consumer)
            push_opcode(cc, car->id, code, OP_POP);

        return type;
    }

    if (car->i64 == symbol("let").i64)
    {
        if (arity != 2)
            cerr(cc, car->id, ERR_LENGTH, "'let' takes two arguments");

        if (as_list(object)[1].type != -TYPE_SYMBOL)
            cerr(cc, car->id, ERR_LENGTH, "'let' takes symbol as first argument");

        type = cc_compile_expr(true, cc, &as_list(object)[2]);

        if (type == TYPE_ERROR)
            return type;

        func->locals = dict(vector_symbol(0), vector_symbol(0));
        name = i64(env_get_typename_by_type(&runtime_get()->env, type));
        name.type = -TYPE_SYMBOL;

        dict_set(&func->locals, &as_list(object)[1], name);

        push_opcode(cc, car->id, code, OP_LSET);

        id = vector_find(&as_list(&func->locals)[0], &as_list(object)[1]);
        push_rf_object(code, i64(1 + id));

        if (!has_consumer)
            push_opcode(cc, car->id, code, OP_POP);

        return type;
    }

    return TYPE_NONE;
}

i8_t cc_compile_cast(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    i8_t type;
    rf_object_t *car = &as_list(object)[0];
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;
    env_t *env = &runtime_get()->env;

    if (car->i64 == symbol("as").i64)
    {
        if (arity != 2)
            cerr(cc, car->id, ERR_LENGTH, "'as' takes two arguments");

        if (as_list(object)[1].type != -TYPE_SYMBOL)
            cerr(cc, car->id, ERR_LENGTH, "'as' takes symbol as first argument");

        type = env_get_type_by_typename(env, as_list(object)[1].i64);

        if (type == TYPE_NONE)
            ccerr(cc, as_list(object)[1].id, ERR_TYPE,
                  str_fmt(0, "'as': unknown type '%s", symbols_get(as_list(object)[1].i64)));

        if (cc_compile_expr(true, cc, &as_list(object)[2]) == TYPE_ERROR)
            return TYPE_ERROR;

        push_opcode(cc, car->id, code, OP_CAST);
        push_opcode(cc, car->id, code, type);

        if (!has_consumer)
            push_opcode(cc, car->id, code, OP_POP);

        return type;
    }

    return TYPE_NONE;
}

i8_t cc_compile_fn(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    UNUSED(has_consumer);

    i8_t type = TYPE_NULL;
    rf_object_t *car = &as_list(object)[0], *b, fun;
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;
    env_t *env = &runtime_get()->env;

    if (car->i64 == symbol("fn").i64)
    {
        if (arity == 0)
            cerr(cc, car->id, ERR_LENGTH, "'fn' expects dict with function arguments");

        b = as_list(object) + 1;

        // first argument is return type
        if (b->type == -TYPE_SYMBOL)
        {
            type = env_get_type_by_typename(env, b->i64);

            if (type == TYPE_NONE)
                ccerr(cc, as_list(object)[1].id, ERR_TYPE,
                      str_fmt(0, "'fn': unknown type '%s", symbols_get(as_list(object)[1].i64)));

            arity -= 1;
            b += 1;
        }

        if (b->type != TYPE_DICT)
            cerr(cc, b->id, ERR_LENGTH, "'fn' expects dict with function arguments");

        arity -= 1;
        fun = cc_compile_function(false, "anonymous", type, rf_object_clone(b), b + 1, car->id, arity, cc->debuginfo);

        if (fun.type == TYPE_ERROR)
        {
            rf_object_free(&cc->function);
            cc->function = fun;
            return TYPE_ERROR;
        }

        push_opcode(cc, object->id, code, OP_PUSH);
        vector_i64_push(&func->const_addrs, code->adt->len);
        push_rf_object(code, fun);
        func->stack_size++;

        return TYPE_FUNCTION;
    }

    return TYPE_NONE;
}

i8_t cc_compile_cond(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    i8_t type, type1;
    i64_t lbl1, lbl2;
    rf_object_t *car = &as_list(object)[0];
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;
    env_t *env = &runtime_get()->env;

    if (car->i64 == symbol("if").i64)
    {
        if (arity < 2 || arity > 3)
            cerr(cc, car->id, ERR_LENGTH, "'if' expects 2 .. 3 arguments");

        type = cc_compile_expr(true, cc, &as_list(object)[1]);

        if (type == TYPE_ERROR)
            return type;

        if (type != -TYPE_BOOL)
            cerr(cc, car->id, ERR_TYPE, "'if': condition must have a bool result");

        push_opcode(cc, car->id, code, OP_JNE);
        lbl1 = code->adt->len;
        push_rf_object(code, i64(0));

        // true branch
        type = cc_compile_expr(has_consumer, cc, &as_list(object)[2]);

        if (type == TYPE_ERROR)
            return type;

        // there is else branch
        if (arity == 3)
        {
            push_opcode(cc, car->id, code, OP_JMP);
            lbl2 = code->adt->len;
            push_rf_object(code, i64(0));
            ((rf_object_t *)(as_string(code) + lbl1))->i64 = code->adt->len;

            // false branch
            type1 = cc_compile_expr(has_consumer, cc, &as_list(object)[3]);

            if (type1 == TYPE_ERROR)
                return type1;

            if (type != type1)
                ccerr(cc, object->id, ERR_TYPE,
                      str_fmt(0, "'if': different types of branches: '%s', '%s'",
                              symbols_get(env_get_typename_by_type(env, type)),
                              symbols_get(env_get_typename_by_type(env, type1))));

            ((rf_object_t *)(as_string(code) + lbl2))->i64 = code->adt->len;
        }
        else
            ((rf_object_t *)(as_string(code) + lbl1))->i64 = code->adt->len;

        return type;
    }

    return TYPE_NONE;
}

i8_t cc_compile_try(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    i8_t type, type1;
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
        lbl1 = code->adt->len;
        push_rf_object(code, i64(0));

        // compile expression under trap
        type = cc_compile_expr(true, cc, &as_list(object)[1]);

        if (type == TYPE_ERROR)
            return type;

        push_opcode(cc, car->id, code, OP_JMP);
        lbl2 = code->adt->len;
        push_rf_object(code, i64(0));

        ((rf_object_t *)(as_string(code) + lbl1))->i64 = code->adt->len;

        // compile expression under catch
        type1 = cc_compile_expr(has_consumer, cc, &as_list(object)[2]);

        if (type1 == TYPE_ERROR)
            return type1;

        if (type != type1)
            ccerr(cc, object->id, ERR_TYPE,
                  str_fmt(0, "'try': different types of expressions: '%s', '%s'",
                          symbols_get(env_get_typename_by_type(env, type)),
                          symbols_get(env_get_typename_by_type(env, type1))));

        ((rf_object_t *)(as_string(code) + lbl2))->i64 = code->adt->len;

        return type;
    }

    return TYPE_NONE;
}

i8_t cc_compile_throw(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    UNUSED(has_consumer);

    i8_t type;
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

i8_t cc_compile_catch(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
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

i8_t cc_compile_map(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    UNUSED(has_consumer);

    i8_t type, *args;
    rf_object_t *car, *addr, *arg_keys, *arg_vals;
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;
    env_t *env = &runtime_get()->env;
    i64_t i, lbl0, lbl1, lbl2, sym;

    car = &as_list(object)[0];
    if (car->i64 == symbol("map").i64)
    {
        if (arity < 2)
            cerr(cc, car->id, ERR_LENGTH, "'map' takes at least two arguments");

        arity -= 1;
        args = (i8_t *)alloca(arity);

        // reserve space for map result
        push_opcode(cc, car->id, code, OP_PUSH);
        push_rf_object(code, null());

        // compile args
        for (i = 0; i < arity; i++)
        {
            type = cc_compile_expr(true, cc, &as_list(object)[i + 2]);

            if (type == TYPE_ERROR)
                return type;

            args[i] = -type;
        }

        push_opcode(cc, car->id, code, OP_ALLOC);
        lbl0 = code->adt->len;
        push_opcode(cc, car->id, code, 0);
        // additional check for zero length argument
        push_opcode(cc, car->id, code, OP_JNE);
        lbl1 = code->adt->len;
        push_rf_object(code, i64(0));

        // compile function
        type = cc_compile_expr(true, cc, &as_list(object)[1]);

        if (type != TYPE_FUNCTION)
            cerr(cc, car->id, ERR_LENGTH, "'map' takes function as first argument");

        addr = peek_rf_object(code);
        func = as_function(addr);

        // specify type for alloc result as return type of the function
        *(i8_t *)(as_string(code) + lbl0) = -func->rettype;

        arg_keys = &as_list(&func->args)[0];
        arg_vals = &as_list(&func->args)[1];

        // check function arity
        if (arg_keys->adt->len != arity)
            cerr(cc, car->id, ERR_LENGTH, "'map' function arity mismatch");

        // check function argument types
        for (i = 0; i < arity; i++)
        {
            sym = as_vector_i64(arg_vals)[i];
            type = env_get_type_by_typename(env, sym);

            if (args[i] != type)
                ccerr(cc, as_list(object)[i + 2].id, ERR_TYPE,
                      str_fmt(0, "argument type mismatch: expected %s, got %s",
                              symbols_get(env_get_typename_by_type(env, type)),
                              symbols_get(env_get_typename_by_type(env, args[i]))));
        }

        lbl2 = code->adt->len;
        push_opcode(cc, car->id, code, OP_MAP);
        push_opcode(cc, car->id, code, (i8_t)arity);
        push_opcode(cc, car->id, code, OP_CALLF);
        push_opcode(cc, car->id, code, OP_COLLECT);
        push_opcode(cc, car->id, code, OP_JNE);
        push_rf_object(code, i64(lbl2));
        // pop function
        push_opcode(cc, car->id, code, OP_POP);
        ((rf_object_t *)(as_string(code) + lbl1))->i64 = code->adt->len;

        // additional one for ctx
        func->stack_size += 2;

        return type;
    }

    return TYPE_NONE;
}

/*
 * Special forms are those that are not in a table of functions because of their special nature.
 * return TYPE_ERROR if there is an error
 * return TYPE_NONE if it is not a special form
 * return type of the special form if it is a special form
 */
i8_t cc_compile_special_forms(bool_t has_consumer, cc_t *cc, rf_object_t *object, u32_t arity)
{
    i8_t type;

    type = cc_compile_time(has_consumer, cc, object, arity);

    if (type != TYPE_NONE)
        return type;

    type = cc_compile_set(has_consumer, cc, object, arity);

    if (type != TYPE_NONE)
        return type;

    type = cc_compile_cast(has_consumer, cc, object, arity);

    if (type != TYPE_NONE)
        return type;

    type = cc_compile_fn(has_consumer, cc, object, arity);

    if (type != TYPE_NONE)
        return type;

    type = cc_compile_cond(has_consumer, cc, object, arity);

    if (type != TYPE_NONE)
        return type;

    type = cc_compile_try(has_consumer, cc, object, arity);

    if (type != TYPE_NONE)
        return type;

    type = cc_compile_catch(has_consumer, cc, object, arity);

    if (type != TYPE_NONE)
        return type;

    type = cc_compile_throw(has_consumer, cc, object, arity);

    if (type != TYPE_NONE)
        return type;

    type = cc_compile_map(has_consumer, cc, object, arity);

    if (type != TYPE_NONE)
        return type;

    return type;
}

i8_t cc_compile_call(cc_t *cc, rf_object_t *car, i8_t *args, u32_t arity)
{
    i32_t i, l, o = 0, n = 0;
    rf_object_t fn;
    u32_t found_arity = arity, j;
    rf_object_t *code = &as_function(&cc->function)->code, *records = &runtime_get()->env.functions;
    env_record_t *rec = find_record(records, car, args, &found_arity);
    str_t err;

    if (!rec)
    {
        l = get_records_len(records, arity);
        err = str_fmt(0, "function name/arity mismatch");
        n = o = strlen(err);
        str_fmt_into(&err, &n, &o, 0, ", here are the possible variants:\n");

        for (i = 0; i < l; i++)
        {
            rec = get_record(records, arity, i);

            if (rec->id == car->i64)
            {
                str_fmt_into(&err, &n, &o, 0, "{'%s'", symbols_get(rec->id));
                for (j = 0; j < arity; j++)
                    str_fmt_into(&err, &n, &o, 0, ", '%s'", symbols_get(env_get_typename_by_type(&runtime_get()->env, rec->args[j])));

                str_fmt_into(&err, &n, &o, 0, " -> %'s'}\n", symbols_get(env_get_typename_by_type(&runtime_get()->env, rec->ret)));
            }
        }

        ccerr(cc, car->id, ERR_LENGTH, err);
    }

    // It is an instruction
    if (rec->op < OP_INVALID)
    {
        push_opcode(cc, car->id, code, rec->op);
        return rec->ret;
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

    fn = i64(rec->op);
    fn.id = car->id;
    push_rf_object(code, fn);
    return rec->ret;
}

i8_t cc_compile_expr(bool_t has_consumer, cc_t *cc, rf_object_t *object)
{
    rf_object_t *car, *addr, *arg_keys, *arg_vals, lst;
    i8_t *args, type = TYPE_NULL;
    u32_t l, i, arity;
    i64_t id, sym;
    env_t *env = &runtime_get()->env;
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;

    switch (object->type)
    {
    case -TYPE_I64:
        push_opcode(cc, object->id, code, OP_PUSH);
        push_rf_object(code, *object);
        func->stack_size++;
        return -TYPE_I64;

    case -TYPE_F64:
        push_opcode(cc, object->id, code, OP_PUSH);
        push_rf_object(code, *object);
        func->stack_size++;
        return -TYPE_F64;

    case -TYPE_SYMBOL:
        if (!has_consumer)
            return TYPE_NONE;

        // symbol is quoted
        if (object->flags == 1)
        {
            object->flags = 0;
            push_opcode(cc, object->id, code, OP_PUSH);
            push_rf_object(code, *object);
            func->stack_size++;
            return -TYPE_SYMBOL;
        }

        // first find in locals
        arg_keys = &as_list(&func->locals)[0];
        arg_vals = &as_list(&func->locals)[1];
        id = vector_find(arg_keys, object);

        if (id < arg_vals->adt->len)
        {
            sym = as_vector_i64(arg_vals)[id];
            type = env_get_type_by_typename(env, sym);
            push_opcode(cc, object->id, code, OP_LLOAD);
            push_rf_object(code, i64(1 + id));
            func->stack_size++;

            return type;
        }

        // then try to search in the function args
        arg_keys = &as_list(&func->args)[0];
        arg_vals = &as_list(&func->args)[1];
        id = vector_find(arg_keys, object);

        if (id < arg_vals->adt->len)
        {
            sym = as_vector_i64(arg_vals)[id];
            type = env_get_type_by_typename(env, sym);
            push_opcode(cc, object->id, code, OP_LLOAD);
            push_rf_object(code, i64(-(arg_keys->adt->len - id)));
            func->stack_size++;

            return type;
        }

        // then in a global env
        addr = env_get_variable(&runtime_get()->env, object);

        if (addr == NULL)
            cerr(cc, object->id, ERR_TYPE, "unknown symbol");

        push_opcode(cc, object->id, code, OP_GLOAD);
        push_rf_object(code, i64((i64_t)addr));
        func->stack_size++;

        return addr->type;

    case TYPE_LIST:
        if (object->adt->len == 0 || object->flags == 1)
        {
            lst = rf_object_clone(object);
            lst.flags = 0;
            push_opcode(cc, object->id, code, OP_PUSH);
            vector_i64_push(&func->const_addrs, code->adt->len);
            push_rf_object(code, lst);
            func->stack_size++;

            return TYPE_LIST;
        }

        car = &as_list(object)[0];
        arity = object->adt->len - 1;
        args = (i8_t *)alloca(arity);

        // special forms compilation need to be done before arguments compilation
        type = cc_compile_special_forms(has_consumer, cc, object, arity);

        if (type == TYPE_ERROR)
            return type;

        if (type != TYPE_NONE)
            return type;
        // --

        // compile arguments
        for (i = 0; i < arity; i++)
        {
            type = cc_compile_expr(true, cc, &as_list(object)[i + 1]);

            if (type == TYPE_ERROR)
                return TYPE_ERROR;

            args[i] = type;
        }

        // no need for compilation of car, just try to dereference it
        if (car->type == -TYPE_SYMBOL)
        {
            if (car->i64 == symbol("self").i64)
            {
                if (cc->top_level)
                    cerr(cc, car->id, ERR_TYPE, "'self' has no meaning at top level");

                addr = &cc->function;
                func = as_function(addr);

                // check args
                arg_vals = &as_list(&func->args)[1];
                l = arg_vals->adt->len;

                if (l != arity)
                    if (l != arity)
                        ccerr(cc, car->id, ERR_LENGTH,
                              str_fmt(0, "arguments length mismatch: expected %d, got %d", l, arity));

                for (i = 0; i < arity; i++)
                {
                    sym = as_vector_i64(arg_vals)[i];
                    type = env_get_type_by_typename(env, sym);

                    if (args[i] != type)
                        ccerr(cc, as_list(object)[i + 1].id, ERR_TYPE,
                              str_fmt(0, "argument type mismatch: expected %s, got %s",
                                      symbols_get(env_get_typename_by_type(env, type)), symbols_get(env_get_typename_by_type(env, args[i]))));
                }

                push_opcode(cc, car->id, code, OP_PUSH);
                push_rf_object(code, rf_object_clone(addr));
                push_opcode(cc, car->id, code, OP_CALLF);

                // additional one for ctx
                func->stack_size += 2;

                return type;
            }
            else
            {
                // try to find function in a global env
                addr = env_get_variable(env, car);

                // try to find function in a functions table
                if (addr == NULL)
                {
                    type = cc_compile_call(cc, car, args, arity);

                    if (type == TYPE_ERROR)
                        return type;

                    if (!has_consumer)
                        push_opcode(cc, car->id, code, OP_POP);

                    return type;
                }

                if (addr->type != TYPE_FUNCTION)
                    cerr(cc, car->id, ERR_TYPE, "expected function/symbol as first argument");

                func = as_function(addr);
                arg_vals = &as_list(&func->args)[1];
                l = arg_vals->adt->len;

                if (l != arity)
                    if (l != arity)
                        ccerr(cc, car->id, ERR_LENGTH,
                              str_fmt(0, "arguments length mismatch: expected %d, got %d", l, arity));

                for (i = 0; i < arity; i++)
                {
                    sym = as_vector_i64(arg_vals)[i];
                    type = env_get_type_by_typename(env, sym);

                    if (args[i] != type)
                        ccerr(cc, as_list(object)[i + 1].id, ERR_TYPE,
                              str_fmt(0, "argument type mismatch: expected %s, got %s",
                                      symbols_get(env_get_typename_by_type(env, type)), symbols_get(env_get_typename_by_type(env, args[i]))));
                }

                push_opcode(cc, car->id, code, OP_GLOAD);
                push_rf_object(code, i64((i64_t)addr));
                push_opcode(cc, car->id, code, OP_CALLF);

                // additional one for ctx
                func->stack_size += 2;

                return func->rettype;
            }
        }

        // compile car
        type = cc_compile_expr(true, cc, car);

        if (type == TYPE_ERROR)
            return TYPE_ERROR;

        if (type != TYPE_FUNCTION)
            cerr(cc, car->id, ERR_TYPE, "expected function/symbol as first argument");

        addr = peek_rf_object(code);
        func = as_function(addr);

        arg_keys = &as_list(&func->args)[0];
        arg_vals = &as_list(&func->args)[1];
        l = arg_vals->adt->len;

        if (l != arity)
            ccerr(cc, car->id, ERR_LENGTH,
                  str_fmt(0, "arguments length mismatch: expected %d, got %d", l, arity));

        for (i = 0; i < arity; i++)
        {
            sym = as_vector_i64(arg_vals)[i];
            type = env_get_type_by_typename(env, sym);

            if (args[i] != type)
                ccerr(cc, as_list(object)[i + 1].id, ERR_TYPE,
                      str_fmt(0, "argument type mismatch: expected %s, got %s",
                              symbols_get(env_get_typename_by_type(env, type)), symbols_get(env_get_typename_by_type(env, args[i]))));
        }

        push_opcode(cc, car->id, code, OP_CALLF);

        // additional one for ctx
        func->stack_size += 2;

        return func->rettype;

    default:
        push_opcode(cc, object->id, code, OP_PUSH);
        vector_i64_push(&func->const_addrs, code->adt->len);
        push_rf_object(code, rf_object_clone(object));
        func->stack_size++;

        return object->type;
    }
}

/*
 * Compile function
 */
rf_object_t cc_compile_function(bool_t top, str_t name, i8_t rettype, rf_object_t args,
                                rf_object_t *body, u32_t id, i32_t len, debuginfo_t *debuginfo)
{
    cc_t cc = {
        .top_level = top,
        .debuginfo = debuginfo,
        .function = function(rettype, args, dict(vector_symbol(0), vector_i64(0)), string(0),
                             debuginfo_new(debuginfo->filename, name)),
    };

    i8_t type;
    i32_t i;
    function_t *func = as_function(&cc.function);
    rf_object_t *code = &func->code, *b = body, err;
    env_t *env = &runtime_get()->env;
    str_t msg;
    if (len == 0)
    {
        push_opcode(&cc, id, code, OP_PUSH);
        push_rf_object(code, null());
        type = TYPE_LIST;
        goto epilogue;
    }
    // Compile all arguments but the last one
    for (i = 0; i < len - 1; i++)
    {
        b += i;
        // skip const expressions
        if (b->type != TYPE_LIST)
            continue;

        type = cc_compile_expr(false, &cc, b);

        if (type == TYPE_ERROR)
            return cc.function;
    }

    // Compile last argument
    b += i;
    type = cc_compile_expr(true, &cc, b);

    if (type == TYPE_ERROR)
        return cc.function;
    // --

epilogue:
    if (func->rettype != TYPE_NULL && func->rettype != type)
    {
        rf_object_free(&cc.function);
        msg = str_fmt(0, "function returns type '%s', but declared '%s'",
                      symbols_get(env_get_typename_by_type(env, type)),
                      symbols_get(env_get_typename_by_type(env, func->rettype)));
        err = error(ERR_TYPE, msg);
        rf_free(msg);
        cc.function = err;
        cc.function.adt->span = debuginfo_get(cc.debuginfo, b->id);
        return err;
    }

    func->rettype = type;

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

    return cc_compile_function(true, "top-level", TYPE_NULL,
                               dict(vector_symbol(0), vector_i64(0)), b, body->id, len, debuginfo);
}
