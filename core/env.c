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

#include <stdarg.h>
#include "env.h"
#include "dict.h"
#include "nilary.h"
#include "unary.h"
#include "binary.h"
#include "nary.h"

#define REC_SIZE (MAX_ARITY + 2)

#define REC(records, arity, name, ret, op, ...)                        \
    {                                                                  \
        i8_t _args[4] = __VA_ARGS__;                                   \
        i32_t _pargs = 0;                                              \
        for (i32_t i = 0; i < MAX_ARITY; i++)                          \
            _pargs |= (u8_t)_args[i] << (MAX_ARITY - 1 - i) * 8;       \
        env_record_t rec = {symbol(name).i64, (i64_t)op, _pargs, ret}; \
        push(&as_list(records)[arity], env_record_t, rec);             \
    }

// clang-format off
null_t init_functions(rf_object_t *records)
{
    // Nilary
    REC(records, 0, "halt",   TYPE_LIST,    OP_HALT,   { 0                       });
    REC(records, 0, "env",    TYPE_DICT,    rf_env,    { 0                       });
    // Unary  
    REC(records, 1, "type",  -TYPE_SYMBOL,  OP_TYPE,   { TYPE_NULL                });
    REC(records, 1, "til" ,   TYPE_I64,     OP_TIL,    {-TYPE_I64                 });
    REC(records, 1, "trace" , TYPE_I64,     OP_TRACE,  {TYPE_NULL                 });

    // Binary
    REC(records, 2, "==",    -TYPE_BOOL,    OP_EQ,     { TYPE_NULL,    TYPE_NULL   });
    // REC(records, 2, "!=",    -TYPE_BOOL,    OP_NEQ,    { TYPE_NULL,    TYPE_NULL   });
    REC(records, 2, "<",     -TYPE_BOOL,    OP_LT,     { TYPE_NULL,    TYPE_NULL   });
    // REC(records, 2, ">",     -TYPE_BOOL,    OP_GT,     { TYPE_NULL,    TYPE_NULL   });
    // REC(records, 2, "<=",    -TYPE_BOOL,    OP_LE,     { TYPE_NULL,    TYPE_NULL   });
    // REC(records, 2, ">=",    -TYPE_BOOL,    OP_GE,     { TYPE_NULL,    TYPE_NULL   });
    REC(records, 2, "+",     -TYPE_I64,     OP_ADDI,   {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "+",     -TYPE_F64,     OP_ADDF,   {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "-",     -TYPE_I64,     OP_SUBI,   {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "-",     -TYPE_F64,     OP_SUBF,   {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "*",     -TYPE_I64,     OP_MULI,   {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "*",     -TYPE_F64,     OP_MULF,   {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "/",     -TYPE_F64,     OP_DIVI,   {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "/",     -TYPE_F64,     OP_DIVF,   {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "sum",    TYPE_I64,     OP_SUMI,   { TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "like",  -TYPE_BOOL,    OP_LIKE,   { TYPE_STRING, TYPE_STRING});
    REC(records, 2, "dict",   TYPE_DICT,    rf_dict,   { TYPE_NULL,    TYPE_NULL });
    // Ternary
    // Quaternary
    // Nary
    REC(records, 5, "list",    TYPE_LIST,    rf_list,    { 0                       });
    REC(records, 5, "format",  TYPE_STRING,  rf_format,  { 0                       });
    REC(records, 5, "print",   TYPE_NULL,    rf_print,   { 0                       });
    REC(records, 5, "println", TYPE_NULL,    rf_println, { 0                       });
}

null_t init_typenames(i64_t *typenames)
{
    typenames[-TYPE_BOOL    + TYPE_OFFSET] = symbol("bool").i64;
    typenames[-TYPE_I64     + TYPE_OFFSET] = symbol("i64").i64;
    typenames[-TYPE_F64     + TYPE_OFFSET] = symbol("f64").i64;
    typenames[-TYPE_SYMBOL  + TYPE_OFFSET] = symbol("symbol").i64;
    typenames[TYPE_NULL     + TYPE_OFFSET] = symbol("Null").i64;
    typenames[TYPE_BOOL     + TYPE_OFFSET] = symbol("Bool").i64;
    typenames[TYPE_I64      + TYPE_OFFSET] = symbol("I64").i64;
    typenames[TYPE_F64      + TYPE_OFFSET] = symbol("F64").i64;
    typenames[TYPE_SYMBOL   + TYPE_OFFSET] = symbol("Symbol").i64;
    typenames[TYPE_STRING   + TYPE_OFFSET] = symbol("String").i64;
    typenames[TYPE_LIST     + TYPE_OFFSET] = symbol("List").i64;
    typenames[TYPE_DICT     + TYPE_OFFSET] = symbol("Dict").i64;
    typenames[TYPE_TABLE    + TYPE_OFFSET] = symbol("Table").i64;
    typenames[TYPE_FUNCTION + TYPE_OFFSET] = symbol("Function").i64;
}
// clang-format on

// TODO: figure out if we need to allocate env related objects using mmap
env_t create_env()
{
    rf_object_t functions = list(REC_SIZE);
    rf_object_t variables = dict(vector_symbol(0), vector_i64(0));

    for (i32_t i = 0; i < REC_SIZE; i++)
        as_list(&functions)[i] = vector(TYPE_STRING, sizeof(env_record_t), 0);

    init_functions(&functions);

    env_t env = {
        .functions = functions,
        .variables = variables,
    };

    init_typenames(env.typenames);

    return env;
}

null_t free_env(env_t *env)
{
    rf_object_t *variables = &as_list(&env->variables)[1], *var;
    i64_t len = variables->adt->len;
    i64_t *vals = as_vector_i64(variables);
    i32_t i;

    for (i = 0; i < len; i++)
    {
        var = (rf_object_t *)vals[i];
        rf_object_free(var);
        rf_free(var);
    }

    rf_object_free(&env->variables);
    rf_object_free(&env->functions);
}

rf_object_t *env_get_variable(env_t *env, rf_object_t name)
{
    rf_object_t addr = dict_get(&env->variables, name);
    if (is_null(&addr))
        return NULL;

    return (rf_object_t *)addr.i64;
}

null_t env_set_variable(env_t *env, rf_object_t name, rf_object_t value)
{
    rf_object_t *new_addr = (rf_object_t *)rf_malloc(sizeof(rf_object_t));
    *new_addr = value;

    rf_object_t *addr = env_get_variable(env, name);
    if (addr)
        rf_object_free(addr);

    dict_set(&env->variables, name, i64((i64_t)new_addr));
}

extern i64_t env_get_typename_by_type(env_t *env, i8_t type)
{
    return env->typenames[type + TYPE_OFFSET];
}

extern i8_t env_get_type_by_typename(env_t *env, i64_t name)
{
    for (i32_t i = 0; i < MAX_TYPE; i++)
        if (env->typenames[i] == name)
            return i - TYPE_OFFSET;

    return TYPE_NONE;
}