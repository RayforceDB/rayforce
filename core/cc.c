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

i8_t cc_compile_expr(cc_t *cc, rf_object_t *object);
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

env_record_t *find_record(rf_object_t *records, rf_object_t *car, i32_t args, u32_t *arity)
{
    u32_t i = 0, records_len;
    env_record_t *rec;

    *arity = *arity > MAX_ARITY ? MAX_ARITY + 1 : *arity;
    records_len = get_records_len(records, *arity);

    // try to find matching function prototype
    for (i = 0; i < records_len; i++)
    {
        rec = get_record(records, *arity, i);

        if ((car->i64 == rec->id) && (rec->args == (args & rec->args)))
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

/*
 * Special forms are those that are not in a table of functions because of their special nature.
 * return TYPE_ERROR if there is an error
 * return TYPE_ANY if it is not a special form
 * return type of the special form if it is a special form
 */
i8_t cc_compile_special_forms(cc_t *cc, rf_object_t *object, u32_t arity)
{
    i8_t type, type1, rettype = TYPE_ANY;
    i64_t id, lbl1, lbl2;
    rf_object_t *car = &as_list(object)[0], *addr, *b, fun, name, err;
    function_t *func = as_function(&cc->function);
    rf_object_t *code = &func->code;
    env_t *env = &runtime_get()->env;

    // compile special forms
    if (car->i64 == symbol("time").i64)
    {
        if (arity != 1)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'time' takes one argument");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }

        push_opcode(cc, car->id, code, OP_TIMER_SET);
        type = cc_compile_expr(cc, &as_list(object)[1]);

        if (type == TYPE_ERROR)
            return TYPE_ERROR;

        push_opcode(cc, car->id, code, OP_TIMER_GET);

        return -TYPE_F64;
    }

    if (car->i64 == symbol("set").i64)
    {
        if (arity != 2)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'set' takes two arguments");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }
        if (as_list(object)[1].type != -TYPE_SYMBOL)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'set' takes symbol as first argument");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }

        type = cc_compile_expr(cc, &as_list(object)[2]);

        if (type == TYPE_ERROR)
            return TYPE_ERROR;

        // check if variable is not set or has the same type
        addr = env_get_variable(&runtime_get()->env, as_list(object)[1]);

        if (addr != NULL && type != addr->type)
        {
            rf_object_free(code);
            err = error(ERR_TYPE, "'set': variable type mismatch");
            err.id = as_list(object)[1].id;
            *code = err;
            return TYPE_ERROR;
        }

        push_opcode(cc, car->id, code, OP_GSET);
        push_rf_object(code, as_list(object)[1]);

        return type;
    }

    if (car->i64 == symbol("let").i64)
    {
        if (arity != 2)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'let' takes two arguments");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }
        if (as_list(object)[1].type != -TYPE_SYMBOL)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'let' takes symbol as first argument");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }

        type = cc_compile_expr(cc, &as_list(object)[2]);

        if (type == TYPE_ERROR)
            return type;

        if (func->locals.type != TYPE_DICT)
            func->locals = dict(vector_symbol(0), vector_symbol(0));

        name = i64(env_get_typename_by_type(&runtime_get()->env, type));
        name.type = -TYPE_SYMBOL;

        dict_set(&func->locals, as_list(object)[1], name);

        push_opcode(cc, car->id, code, OP_LSET);

        id = vector_i64_find(&as_list(&func->locals)[0], as_list(object)[1].i64);
        push_rf_object(code, i64(-1 - id));

        return type;
    }

    if (car->i64 == symbol("cast").i64)
    {
        if (arity != 2)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'cast' takes two arguments");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }

        if (as_list(object)[1].type != -TYPE_SYMBOL)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'cast' takes symbol as first argument");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }

        type = env_get_type_by_typename(env, as_list(object)[1].i64);

        if (type == TYPE_ANY)
        {
            rf_object_free(code);
            err = error(ERR_TYPE, str_fmt(0, "'cast': unknown type '%s", symbols_get(as_list(object)[1].i64)));
            err.id = as_list(object)[1].id;
            *code = err;
            return TYPE_ERROR;
        }

        if (cc_compile_expr(cc, &as_list(object)[2]) == TYPE_ERROR)
            return TYPE_ERROR;

        push_opcode(cc, car->id, code, OP_CAST);
        push_opcode(cc, car->id, code, type);

        return type;
    }

    if (car->i64 == symbol("fn").i64)
    {
        if (arity == 0)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'fn' expects dict with function arguments");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }

        b = as_list(object) + 1;

        // first argument is return type
        if (b->type == -TYPE_SYMBOL)
        {
            rettype = env_get_type_by_typename(env, b->i64);

            if (rettype == TYPE_ANY)
            {
                rf_object_free(code);
                err = error(ERR_TYPE, str_fmt(0, "'fn': unknown type '%s", symbols_get(as_list(object)[1].i64)));
                err.id = as_list(object)[1].id;
                *code = err;
                return TYPE_ERROR;
            }

            arity -= 1;
            b += 1;
        }

        if (b->type != TYPE_DICT)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'fn' expects dict with function arguments");
            err.id = b->id;
            *code = err;
            return TYPE_ERROR;
        }

        arity -= 1;
        fun = cc_compile_function(false, "anonymous", rettype, rf_object_clone(b), b + 1, car->id, arity, cc->debuginfo);
        // printf("%s\n", vm_code_fmt(&fun));
        push_opcode(cc, object->id, code, OP_PUSH);
        push_rf_object(code, fun);
        return TYPE_FUNCTION;
    }

    if (car->i64 == symbol("if").i64)
    {
        if (arity < 2)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'if' takes at least two arguments");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }

        type = cc_compile_expr(cc, &as_list(object)[1]);

        if (type == TYPE_ERROR)
            return type;

        if (type != -TYPE_BOOL)
        {
            rf_object_free(code);
            err = error(ERR_TYPE, "'if': condition must have a bool result");
            err.id = as_list(object)[1].id;
            *code = err;
            return TYPE_ERROR;
        }

        push_opcode(cc, car->id, code, OP_JNE);
        lbl1 = code->adt->len;
        push_rf_object(code, i64(0));

        type = cc_compile_expr(cc, &as_list(object)[2]);

        if (type == TYPE_ERROR)
            return type;

        // if (arity == 2)
        // {
        //     *lbl = code->adt->len;
        //     return type;
        // }

        push_opcode(cc, car->id, code, OP_JMP);
        lbl2 = code->adt->len;
        push_rf_object(code, i64(0));
        ((rf_object_t *)(as_string(code) + lbl1))->i64 = code->adt->len;
        type1 = cc_compile_expr(cc, &as_list(object)[3]);

        if (type1 == TYPE_ERROR)
            return type1;

        if (type != type1)
        {
            rf_object_free(code);
            err = error(ERR_TYPE, str_fmt(0, "'if': different types of branches: '%s', '%s'",
                                          symbols_get(env_get_typename_by_type(env, type)),
                                          symbols_get(env_get_typename_by_type(env, type1))));
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }

        ((rf_object_t *)(as_string(code) + lbl2))->i64 = code->adt->len;

        return type;
    }

    return TYPE_ANY;
}

i8_t cc_compile_call(cc_t *cc, rf_object_t *car, i32_t args, u32_t arity)
{
    rf_object_t fn;
    u32_t found_arity = arity;
    rf_object_t *code = &as_function(&cc->function)->code;
    env_record_t *rec = find_record(&runtime_get()->env.functions, car, args, &found_arity);

    if (!rec)
        return TYPE_ERROR;

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

i8_t cc_compile_expr(cc_t *cc, rf_object_t *object)
{
    rf_object_t *car, err, *addr, *arg_keys, *arg_vals;
    i8_t type = TYPE_ANY, len = 0;
    u32_t i, arity;
    i32_t args = 0;
    i64_t id, sym;
    env_t *env = &runtime_get()->env;
    rf_object_t *code = &as_function(&cc->function)->code;
    function_t *func = as_function(&cc->function);

    switch (object->type)
    {
    case -TYPE_I64:
        push_opcode(cc, object->id, code, OP_PUSH);
        push_rf_object(code, *object);
        return -TYPE_I64;

    case -TYPE_F64:
        push_opcode(cc, object->id, code, OP_PUSH);
        push_rf_object(code, *object);
        return -TYPE_F64;

    case -TYPE_SYMBOL:
        // symbol is quoted
        if (object->flags == 1)
        {
            object->flags = 0;
            push_opcode(cc, object->id, code, OP_PUSH);
            push_rf_object(code, *object);
            return -TYPE_SYMBOL;
        }

        // first find in locals
        if (func->locals.type == TYPE_DICT)
        {
            arg_keys = &as_list(&func->locals)[0];
            arg_vals = &as_list(&func->locals)[1];

            id = vector_i64_find(arg_keys, object->i64);

            if (id < arg_vals->adt->len)
            {
                sym = as_vector_i64(arg_vals)[id];
                type = env_get_type_by_typename(&runtime_get()->env, sym);
                push_opcode(cc, object->id, code, OP_LLOAD);
                push_rf_object(code, i64(-1 - id));

                return type;
            }
        }

        // then try to search in the function args
        if (func->args.type == TYPE_DICT)
        {
            arg_keys = &as_list(&func->args)[0];
            arg_vals = &as_list(&func->args)[1];

            id = vector_i64_find(arg_keys, object->i64);

            if (func->locals.type == TYPE_DICT)
                len = (i8_t)as_list(&func->locals)[0].adt->len;

            if (id < arg_vals->adt->len)
            {
                sym = as_vector_i64(arg_vals)[id];
                type = env_get_type_by_typename(&runtime_get()->env, sym);
                push_opcode(cc, object->id, code, OP_LLOAD);
                push_rf_object(code, i64(-1 - (id + len)));

                return type;
            }
        }

        // then in a global env
        addr = env_get_variable(&runtime_get()->env, *object);

        if (addr == NULL)
        {
            rf_object_free(code);
            err = error(ERR_TYPE, "unknown symbol");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }

        push_opcode(cc, object->id, code, OP_GLOAD);
        push_rf_object(code, i64((i64_t)addr));

        return addr->type;

    case TYPE_LIST:
        if (object->adt->len == 0 || object->flags == 1)
        {
            object->flags = 0;
            push_opcode(cc, object->id, code, OP_PUSH);
            push_rf_object(code, rf_object_clone(object));
            return TYPE_LIST;
        }

        car = &as_list(object)[0];
        if (car->type != -TYPE_SYMBOL)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "expected symbol as first argument");
            err.id = car->id;
            *code = err;
            return TYPE_ERROR;
        }

        arity = object->adt->len - 1;

        type = cc_compile_special_forms(cc, object, arity);

        if (type == TYPE_ERROR)
            return type;

        if (type != TYPE_ANY)
            return type;

        // Compile user function call
        if (car->i64 == symbol("self").i64)
        {
            if (cc->top_level)
            {
                rf_object_free(code);
                err = error(ERR_TYPE, "'self' has no meaning at top level");
                err.id = car->id;
                *code = err;
                return TYPE_ERROR;
            }

            addr = &cc->function;
        }
        else
        {
            addr = env_get_variable(&runtime_get()->env, *car);
        }

        if (addr && addr->type == TYPE_FUNCTION)
        {
            func = as_function(addr);
            if (func->args.type != TYPE_DICT)
            {
                rf_object_free(code);
                err = error(ERR_TYPE, "expected dict as function arguments");
                err.id = car->id;
                *code = err;
                return TYPE_ERROR;
            }

            arg_keys = &as_list(&func->args)[0];
            arg_vals = &as_list(&func->args)[1];

            if (arg_keys->adt->len != arity)
            {
                rf_object_free(code);
                err = error(ERR_LENGTH, str_fmt(0, "arguments length mismatch: expected %d, got %d",
                                                arg_keys->adt->len, arity));
                err.id = car->id;
                *code = err;
                return TYPE_ERROR;
            }

            // compile arguments
            for (i = 1; i <= arity; i++)
            {
                type = cc_compile_expr(cc, &as_list(object)[i]);

                if (type == TYPE_ERROR)
                    return TYPE_ERROR;

                if (type != env_get_type_by_typename(env, as_vector_symbol(arg_vals)[i - 1]))
                {
                    rf_object_free(code);
                    err = error(ERR_TYPE, str_fmt(0, "argument type mismatch: expected %s, got %s",
                                                  symbols_get(as_vector_symbol(arg_vals)[i - 1]),
                                                  symbols_get(env_get_typename_by_type(env, type))));
                    err.id = as_list(object)[i].id;
                    *code = err;
                    return TYPE_ERROR;
                }

                // pack arguments only if function is not nary
                if (arity <= MAX_ARITY)
                    args |= (u8_t)type << (MAX_ARITY - i) * 8;
            }

            // reserve stack space for locals
            if (func->locals.type == TYPE_DICT)
            {
                len = (i8_t)as_list(&func->locals)[0].adt->len;
                push_opcode(cc, car->id, code, OP_RESERVE);
                push_opcode(cc, car->id, code, len);
            }
            push_opcode(cc, car->id, code, OP_CALLF);
            push_rf_object(code, rf_object_clone(addr));
            // Cleanup arguments from the stack after call
            push_opcode(cc, car->id, code, OP_SWAPN);
            push_opcode(cc, car->id, code, (i8_t)arity + len);

            return func->rettype;
        }

        // compile arguments
        for (i = 1; i <= arity; i++)
        {
            type = cc_compile_expr(cc, &as_list(object)[i]);

            if (type == TYPE_ERROR)
                return TYPE_ERROR;

            // pack arguments only if function is not nary
            if (arity <= MAX_ARITY)
                args |= (u8_t)type << (MAX_ARITY - i) * 8;
        }

        type = cc_compile_call(cc, car, args, arity);

        if (type != TYPE_ERROR)
            return type;

        rf_object_free(code);
        err = error(ERR_LENGTH, "function has not found");
        err.id = car->id;
        *code = err;
        return type;

    default:
        push_opcode(cc, object->id, code, OP_PUSH);
        push_rf_object(code, rf_object_clone(object));
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
        .function = function(rettype, args, null(), string(0), debuginfo_new(debuginfo->filename, name)),
    };

    i8_t type, op;
    i32_t i;
    function_t *func = as_function(&cc.function);
    rf_object_t *code = &func->code, *b, err;
    env_t *env = &runtime_get()->env;

    if (len == 0)
    {
        push_opcode(&cc, id, code, OP_PUSH);
        push_rf_object(code, null());
        return cc.function;
    }

    // Compile all arguments but last one
    for (i = 0; i < len - 1; i++)
    {
        b = body + i;
        // skip const expressions
        if (b->type != TYPE_LIST)
            continue;

        type = cc_compile_expr(&cc, b);

        if (type == TYPE_ERROR)
        {
            code->adt->span = debuginfo_get(cc.debuginfo, code->id);
            err = rf_object_clone(code);
            rf_object_free(&cc.function);
            return err;
        }

        push_opcode(&cc, id, code, OP_POP);
    }

    // Compile last argument
    b = body + i;
    type = cc_compile_expr(&cc, b);

    if (type == TYPE_ERROR)
    {
        code->adt->span = debuginfo_get(cc.debuginfo, code->id);
        err = rf_object_clone(code);
        rf_object_free(&cc.function);
        return err;
    }
    // --

    if (func->rettype != TYPE_ANY && func->rettype != type)
    {
        rf_object_free(&cc.function);
        rf_object_free(code);
        err = error(ERR_TYPE, str_fmt(0, "function return type mismatch: specified %s, inferred %s",
                                      symbols_get(env_get_typename_by_type(env, func->rettype)),
                                      symbols_get(env_get_typename_by_type(env, type))));
        err.id = id;
        *code = err;
        return err;
    }

    func->rettype = type;

    if (top)
        op = OP_HALT;
    else
        op = OP_RET;

    push_opcode(&cc, id, code, op);

    return cc.function;
}

/*
 * Compile top level expression
 */
rf_object_t cc_compile(rf_object_t *body, debuginfo_t *debuginfo)
{
    if (body->type != TYPE_LIST)
        return error(ERR_TYPE, str_fmt(0, "compile '%s': expected list", "top-level"));

    rf_object_t *b = as_list(body);
    i32_t len = body->adt->len;

    return cc_compile_function(true, "top-level", TYPE_ANY, null(), b, body->id, len, debuginfo);
}
