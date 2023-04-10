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
#include "unary.h"
#include "binary.h"
#include "nary.h"

#define REC_SIZE (MAX_ARITY + 2)

#define REC(records, arity, name, ret, op, ...)                        \
    {                                                                  \
        i8_t _args[4] = __VA_ARGS__;                                   \
        u32_t _pargs = 0;                                              \
        for (u32_t i = 0; i < MAX_ARITY; i++)                          \
            _pargs |= (u32_t)_args[i] << (MAX_ARITY - 1 - i) * 8;      \
        env_record_t rec = {symbol(name).i64, (i64_t)op, _pargs, ret}; \
        push(&as_list(records)[arity], env_record_t, rec);             \
    }

// clang-format off
null_t init_functions(rf_object_t *records)
{
    // Nilary
    REC(records, 0, "halt",   TYPE_LIST,    OP_HALT,   { 0                       });
    // Unary  
    REC(records, 1, "type",  -TYPE_SYMBOL,  OP_TYPE,   { TYPE_ANY                });
    REC(records, 1, "til" ,   TYPE_I64,     OP_TIL,    {-TYPE_I64                });
    REC(records, 1, "flip",   TYPE_LIST,    rf_flip,   { TYPE_ANY                });
    // Binary
    REC(records, 2, "+",     -TYPE_I64,     OP_ADDI,   {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "+",     -TYPE_F64,     OP_ADDF,   {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "-",     -TYPE_I64,     OP_SUBI,   {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "-",     -TYPE_F64,     OP_SUBF,   {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "*",     -TYPE_I64,     OP_MULI,   {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "*",     -TYPE_F64,     OP_MULF,   {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "/",     -TYPE_F64,     OP_DIVI,   {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "/",     -TYPE_F64,     OP_DIVF,   {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "sum",    TYPE_I64,     OP_SUMI,   { TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "like",  -TYPE_I64,     OP_LIKE,   { TYPE_STRING, TYPE_STRING});
    REC(records, 2, "dict",   TYPE_DICT,    rf_dict,   { TYPE_ANY,    TYPE_ANY   });
    // Ternary
    // Quaternary
    // Nary
    REC(records, 5, "list",   TYPE_LIST,    rf_list,   { 0                       });
}
// clang-format on

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

    return env;
}

null_t free_env(env_t *env)
{

    for (i32_t i = 0; i < REC_SIZE; i++)
        rf_object_free(&as_list(&env->functions)[i]);

    rf_object_free(&env->functions);
    rf_object_free(&env->variables);
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