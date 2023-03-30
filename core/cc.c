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

#define push_opcode(c, x)                       \
    {                                           \
        vector_reserve(c, 1);                   \
        as_string(c)[(c)->adt.len++] = (i8_t)x; \
    }

#define push_object(c, x)                                  \
    {                                                      \
        vector_reserve(c, sizeof(rf_object_t));            \
        *(rf_object_t *)(as_string(c) + (c)->adt.len) = x; \
        (c)->adt.len += sizeof(rf_object_t);               \
    }

typedef struct dispatch_record_t
{
    str_t name;
    i8_t args[8];
    i8_t ret;
    vm_opcode_t opcode;
} dispatch_record_t;

#define DISPATCH_TABLE_SIZE 5
#define DISPATCH_RECORD_SIZE 16

// clang-format off
static dispatch_record_t _DISPATCH_TABLE[DISPATCH_TABLE_SIZE][DISPATCH_RECORD_SIZE] = {
    // Nilary
    {
        {"halt", {0},  TYPE_LIST, OP_HALT}
    },
    // Unary
    {
        {"-",    {-TYPE_I64}, -TYPE_I64,  OP_HALT},            
    },
    // Binary
    {
        {"+",    {-TYPE_I64,     -TYPE_I64}, -TYPE_I64,    OP_ADDI}, 
        {"+",    {-TYPE_F64,     -TYPE_F64}, -TYPE_F64,    OP_ADDF},
        {"sum",  {TYPE_I64,      -TYPE_I64},  TYPE_I64,    OP_SUMI},
        {"like", {TYPE_STRING, TYPE_STRING},  TYPE_I64,    OP_LIKE}
    },
    // Ternary
    {{0}},
    // Quaternary
    {{0}},
};
// clang-format on

i8_t cc_compile_code(rf_object_t *object, rf_object_t *code)
{
    u32_t arity, i = 0, j = 0, match = 0;
    rf_object_t *car, err;
    dispatch_record_t *rec;
    i8_t ret = TYPE_ERROR, arg_types[8], type;

    switch (object->type)
    {
    case -TYPE_I64:
        push_opcode(code, OP_PUSH);
        push_object(code, *object);
        return -TYPE_I64;

    case -TYPE_F64:
        push_opcode(code, OP_PUSH);
        push_object(code, *object);
        return -TYPE_F64;

    case -TYPE_SYMBOL:
        push_opcode(code, OP_PUSH);
        push_object(code, *object);
        return -TYPE_SYMBOL;

    case TYPE_LIST:
        if (object->adt.len == 0)
        {
            push_opcode(code, OP_PUSH);
            push_object(code, object_clone(object));
            return TYPE_LIST;
        }

        car = &as_list(object)[0];
        if (car->type != -TYPE_SYMBOL)
        {
            object_free(code);
            err = error(ERR_LENGTH, "compile list: expected symbol in a head");
            err.id = car->id;
            *code = err;
            return TYPE_ERROR;
        }

        arity = object->adt.len - 1;
        if (arity > 4)
        {
            object_free(code);
            err = error(ERR_LENGTH, "compile list: too many arguments");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }

        // compile arguments from right to left
        for (j = arity; j > 0; j--)
        {
            type = cc_compile_code(&as_list(object)[j], code);

            if (type == TYPE_ERROR)
                return TYPE_ERROR;

            arg_types[j - 1] = type;
        }

        // try to find matching function prototype
        while ((rec = &_DISPATCH_TABLE[arity][i++]))
        {
            if (i > DISPATCH_RECORD_SIZE)
                break;

            if (rec->name != 0 && strcmp(symbols_get(car->i64), rec->name) == 0)
            {
                for (j = arity; j > 0; j--)
                {
                    if (arg_types[j - 1] != rec->args[j - 1])
                        break;

                    match++;
                }

                if (match < arity)
                {
                    match = 0;
                    continue;
                }

                ret = rec->ret;
                push_opcode(code, rec->opcode);

                break;
            }
        }

        if (!match && arity)
        {
            object_free(code);
            err = error(ERR_LENGTH, "compile list: function proto or arity mismatch");
            err.id = object->id;
            *code = err;
            return TYPE_ERROR;
        }

        return ret;

    default:
        push_opcode(code, OP_PUSH);
        push_object(code, object_clone(object));
        return object->type;
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

    for (u32_t i = 0; i < list->adt.len; i++)
        cc_compile_code(&as_list(list)[i], &code);

    if (list->adt.len == 0)
    {
        push_opcode(&code, OP_PUSH);
        push_object(&code, null());
    }

    if (code.type != TYPE_ERROR)
        push_opcode(&code, OP_HALT);

    // printf("CODE: %s\n", cc_code_fmt(&code));

    return code;
}

str_t cc_code_fmt(rf_object_t *code)
{
    i32_t p = 0, c = 0, len = code->adt.len;
    str_t ip = code->adt.ptr;
    str_t s = str_fmt(0, "-- code:\n");

    p = strlen(s);

    while ((ip - ((str_t)code->adt.ptr)) < len)
    {
        switch (*ip)
        {
        case OP_HALT:
            p += str_fmt_into(0, p, &s, "%.4d: halt\n", c++);
            break;
        case OP_PUSH:
            p += str_fmt_into(0, p, &s, "%.4d: push %s\n", c++, object_fmt(((rf_object_t *)(ip + 1))));
            ip += sizeof(rf_object_t);
            break;
        case OP_POP:
            p += str_fmt_into(0, p, &s, "%.4d: pop\n", c++);
            break;
        case OP_ADDI:
            p += str_fmt_into(0, p, &s, "%.4d: addi\n", c++);
            break;
        case OP_ADDF:
            p += str_fmt_into(0, p, &s, "%.4d: addf\n", c++);
            break;
        case OP_SUMI:
            p += str_fmt_into(0, p, &s, "%.4d: sumi\n", c++);
            break;
        default:
            p += str_fmt_into(0, p, &s, "%.4d: unknown %d\n", c++, *ip);
            break;
        }

        ip++;
    }

    return s;
}
