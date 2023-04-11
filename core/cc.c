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

i8_t cc_compile_fn(rf_object_t *rf_object, rf_object_t *code);

#define push_opcode(c, x)                        \
    {                                            \
        vector_reserve(c, 1);                    \
        as_string(c)[(c)->adt->len++] = (i8_t)x; \
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

i8_t cc_compile_op(rf_object_t *car, i32_t args, u32_t arity, rf_object_t *code)
{
    env_record_t *rec;
    rf_object_t fn;
    u32_t found_arity = arity;

    rec = find_record(&runtime_get()->env.functions, car, args, &found_arity);

    if (!rec)
        return TYPE_ERROR;

    // It is an instruction
    if (rec->op < OP_INVALID)
    {
        push_opcode(code, rec->op);
        return rec->ret;
    }

    // It is a function call
    switch (found_arity)
    {
    case 0:
        push_opcode(code, OP_CALL0);
        break;
    case 1:
        push_opcode(code, OP_CALL1);
        break;
    case 2:
        push_opcode(code, OP_CALL2);
        break;
    case 3:
        push_opcode(code, OP_CALL3);
        break;
    case 4:
        push_opcode(code, OP_CALL4);
        break;
    default:
        push_opcode(code, OP_CALLN);
        push_opcode(code, arity);
    }

    fn = i64(rec->op);
    fn.id = car->id;
    push_rf_object(code, fn);
    return rec->ret;
}

/*
 * Special forms are those that are not in a table of functions because of their special nature.
 * return TYPE_ERROR if there is an error
 * return TYPE_ANY if it is not a special form
 * return type of the special form if it is a special form
 */
i8_t cc_compile_special_forms(rf_object_t *rf_object, u32_t arity, rf_object_t *code)
{
    i8_t type;
    rf_object_t *car = &as_list(rf_object)[0], *addr, err;

    // compile special forms
    if (car->i64 == symbol("time").i64)
    {
        if (arity != 1)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'time' takes one argument");
            err.id = rf_object->id;
            *code = err;
            return TYPE_ERROR;
        }

        push_opcode(code, OP_TIMER_SET);
        type = cc_compile_fn(&as_list(rf_object)[1], code);

        if (type == TYPE_ERROR)
            return TYPE_ERROR;

        push_opcode(code, OP_TIMER_GET);

        return -TYPE_F64;
    }

    if (car->i64 == symbol("set").i64)
    {
        if (arity != 2)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'set' takes two arguments");
            err.id = rf_object->id;
            *code = err;
            return TYPE_ERROR;
        }
        if (as_list(rf_object)[1].type != -TYPE_SYMBOL)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'set' takes symbol as first argument");
            err.id = rf_object->id;
            *code = err;
            return TYPE_ERROR;
        }

        type = cc_compile_fn(&as_list(rf_object)[2], code);

        if (type == TYPE_ERROR)
            return TYPE_ERROR;

        // check if variable is not set or has the same type
        addr = env_get_variable(&runtime_get()->env, as_list(rf_object)[1]);

        if (addr != NULL && type != addr->type)
        {
            rf_object_free(code);
            err = error(ERR_TYPE, "variable type mismatch");
            err.id = rf_object->id;
            *code = err;
            return TYPE_ERROR;
        }

        push_opcode(code, OP_SET);
        push_rf_object(code, as_list(rf_object)[1]);

        return type;
    }

    if (car->i64 == symbol("cast").i64)
    {
        if (arity != 2)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'cast' takes two arguments");
            err.id = rf_object->id;
            *code = err;
            return TYPE_ERROR;
        }

        if (as_list(rf_object)[1].type != -TYPE_SYMBOL)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "'cast' takes symbol as first argument");
            err.id = rf_object->id;
            *code = err;
            return TYPE_ERROR;
        }

        type = env_get_type_by_typename(&runtime_get()->env, as_list(rf_object)[1].i64);

        cc_compile_fn(&as_list(rf_object)[1], code);
        cc_compile_fn(&as_list(rf_object)[2], code);

        push_opcode(code, OP_CAST);
        push_opcode(code, type);

        return type;
    }

    return TYPE_ANY;
}

i8_t cc_compile_fn(rf_object_t *rf_object, rf_object_t *code)
{
    rf_object_t *car, err, *addr;
    i8_t type;
    u32_t i, arity;
    i32_t args = 0;

    switch (rf_object->type)
    {
    case -TYPE_I64:
        push_opcode(code, OP_PUSH);
        push_rf_object(code, *rf_object);
        return -TYPE_I64;

    case -TYPE_F64:
        push_opcode(code, OP_PUSH);
        push_rf_object(code, *rf_object);
        return -TYPE_F64;

    case -TYPE_SYMBOL:
        // symbol is quoted
        if (rf_object->flags == 1)
        {
            rf_object->flags = 0;
            push_opcode(code, OP_PUSH);
            push_rf_object(code, *rf_object);
            return -TYPE_SYMBOL;
        }

        addr = env_get_variable(&runtime_get()->env, *rf_object);

        if (addr == NULL)
        {
            rf_object_free(code);
            err = error(ERR_TYPE, "unknown symbol");
            err.id = rf_object->id;
            *code = err;
            return TYPE_ERROR;
        }

        push_opcode(code, OP_GET);
        push_rf_object(code, i64((i64_t)addr));

        return addr->type;

    case TYPE_LIST:
        if (rf_object->adt->len == 0 || rf_object->flags == 1)
        {
            rf_object->flags = 0;
            push_opcode(code, OP_PUSH);
            push_rf_object(code, rf_object_clone(rf_object));
            return TYPE_LIST;
        }

        car = &as_list(rf_object)[0];
        if (car->type != -TYPE_SYMBOL)
        {
            rf_object_free(code);
            err = error(ERR_LENGTH, "expected symbol in a head");
            err.id = car->id;
            *code = err;
            return TYPE_ERROR;
        }

        arity = rf_object->adt->len - 1;

        type = cc_compile_special_forms(rf_object, arity, code);

        if (type == TYPE_ERROR)
            return type;

        if (type != TYPE_ANY)
            return type;

        // compile arguments
        for (i = 1; i <= arity; i++)
        {
            type = cc_compile_fn(&as_list(rf_object)[i], code);

            if (type == TYPE_ERROR)
                return TYPE_ERROR;

            // pack arguments only if function is not nary
            if (arity <= MAX_ARITY)
                args |= (u8_t)type << (MAX_ARITY - i) * 8;
        }

        type = cc_compile_op(car, args, arity, code);

        if (type != TYPE_ERROR)
            return type;

        rf_object_free(code);
        err = error(ERR_LENGTH, "function proto or arity mismatch");
        err.id = rf_object->id;
        *code = err;
        return type;

    default:
        push_opcode(code, OP_PUSH);
        push_rf_object(code, rf_object_clone(rf_object));
        return rf_object->type;
    }
}

/*
 * Compile entire program as a list
 */
rf_object_t cc_compile(rf_object_t *list)
{
    if (list->type != TYPE_LIST)
        return error(ERR_TYPE, "compile: expected list");

    rf_object_t code = string(0);

    for (u32_t i = 0; i < list->adt->len; i++)
        cc_compile_fn(&as_list(list)[i], &code);

    if (list->adt->len == 0)
    {
        push_opcode(&code, OP_PUSH);
        push_rf_object(&code, null());
    }

    if (code.type != TYPE_ERROR)
        push_opcode(&code, OP_HALT);

    // printf("CODE: %s\n", cc_code_fmt(&code));

    return code;
}

str_t cc_code_fmt(rf_object_t *code)
{
    i32_t p = 0, c = 0, len = code->adt->len;
    str_t ip = as_string(code);
    str_t s = str_fmt(0, "-- code:\n");

    p = strlen(s);

    // while ((ip - as_string(code)) < len)
    // {
    //     switch (*ip)
    //     {
    //     case OP_HALT:
    //         p += str_fmt_into(&s, 0, p, "%.4d: halt\n", c++);
    //         break;
    //     case OP_PUSH:
    //         p += str_fmt_into(&s, 0, p, "%.4d: push %s\n", c++, rf_object_fmt(((rf_object_t *)(ip + 1))));
    //         ip += sizeof(rf_object_t);
    //         break;
    //     case OP_POP:
    //         p += str_fmt_into(&s, 0, p, "%.4d: pop\n", c++);
    //         break;
    //     case OP_ADDI:
    //         p += str_fmt_into(&s, 0, p, "%.4d: addi\n", c++);
    //         break;
    //     case OP_ADDF:
    //         p += str_fmt_into(&s, 0, p, "%.4d: addf\n", c++);
    //         break;
    //     case OP_SUMI:
    //         p += str_fmt_into(&s, 0, p, "%.4d: sumi\n", c++);
    //         break;
    //     case OP_CALL1:
    //         p += str_fmt_into(&s, 0, p, "%.4d: call1 %p\n", c++, ((rf_object_t *)(ip + 1))->i64);
    //         ip += sizeof(rf_object_t);
    //         break;
    //     case OP_CALL2:
    //         p += str_fmt_into(&s, 0, p, "%.4d: call2 %p\n", c++, ((rf_object_t *)(ip + 1))->i64);
    //         ip += sizeof(rf_object_t);
    //         break;
    //     case OP_CALL3:
    //         p += str_fmt_into(&s, 0, p, "%.4d: call3 %p\n", c++, ((rf_object_t *)(ip + 1))->i64);
    //         ip += sizeof(rf_object_t);
    //         break;
    //     case OP_CALL4:
    //         p += str_fmt_into(&s, 0, p, "%.4d: call4 %p\n", c++, ((rf_object_t *)(ip + 1))->i64);
    //         ip += sizeof(rf_object_t);
    //         break;
    //     case OP_CALLN:
    //         ip++;
    //         p += str_fmt_into(&s, 0, p, "%.4d: calln %p\n", c++, ((rf_object_t *)(ip + 1))->i64);
    //         ip += sizeof(rf_object_t);
    //         break;
    //     default:
    //         p += str_fmt_into(&s, 0, p, "%.4d: unknown %d\n", c++, *ip);
    //         break;
    //     }

    //     ip++;
    // }

    return s;
}
