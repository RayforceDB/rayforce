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

#include "env.h"
#include "monad.h"

#define REC(records, arity, name, ret, op, ...)                             \
    {                                                                       \
        env_record_t rec = {symbol(name).i64, ret, (i64_t)op, __VA_ARGS__}; \
        push(&as_list(records)[arity], env_record_t, rec);                  \
    }

// clang-format off
null_t init_instructions(rf_object_t *records)
{
    // Nilary
    REC(records, 0, "halt",   TYPE_LIST,    OP_HALT, {0                        });
    // Unary
    REC(records, 1, "type",  -TYPE_SYMBOL,  OP_TYPE, { TYPE_ANY                });
    REC(records, 1, "til" ,   TYPE_I64,     OP_TIL,  {-TYPE_I64                });
    // Binary
    REC(records, 2, "+",     -TYPE_I64,     OP_ADDI, {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "+",     -TYPE_F64,     OP_ADDF, {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "-",     -TYPE_I64,     OP_SUBI, {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "-",     -TYPE_F64,     OP_SUBF, {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "*",     -TYPE_I64,     OP_MULI, {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "*",     -TYPE_F64,     OP_MULF, {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "/",     -TYPE_F64,     OP_DIVI, {-TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "/",     -TYPE_F64,     OP_DIVF, {-TYPE_F64,   -TYPE_F64   });
    REC(records, 2, "sum",    TYPE_I64,     OP_SUMI, { TYPE_I64,   -TYPE_I64   });
    REC(records, 2, "like",  -TYPE_I64,     OP_LIKE, { TYPE_STRING, TYPE_STRING});
    // Ternary
    // Quaternary
}
// clang-format on

// clang-format off
null_t init_functions(rf_object_t *variables)
{
    // Nilary
    // Unary
    REC(variables, 1, "flip", TYPE_LIST, rf_flip,       { TYPE_ANY              });
    // Binary
    // Ternary
    // Quaternary
}
// clang-format on

env_t create_env()
{
    rf_object_t instructions = list(MAX_ARITY + 1);
    rf_object_t functions = list(MAX_ARITY + 1);
    rf_object_t variables = list(0);

    for (i32_t i = 0; i <= MAX_ARITY; i++)
        as_list(&instructions)[i] = vector(TYPE_STRING, sizeof(env_record_t), 0);

    for (i32_t i = 0; i <= MAX_ARITY; i++)
        as_list(&functions)[i] = vector(TYPE_STRING, sizeof(env_record_t), 0);

    init_instructions(&instructions);
    init_functions(&functions);

    env_t env = {
        .instructions = instructions,
        .functions = functions,
        .variables = variables,
    };

    return env;
}
