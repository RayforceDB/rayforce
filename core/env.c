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

#define REC(records, arity, name, ret, op, ...)                             \
    {                                                                       \
        env_record_t rec = {symbol(name).i64, (i64_t)op, __VA_ARGS__, ret}; \
        push(&as_list(records)[arity], env_record_t, rec);                  \
    }

// clang-format off
null_t init_functions(rf_object_t *records)
{
    // Nilary
    REC(records, 0, "halt",      TYPE_LIST,       OP_HALT,                 { 0                         });
    REC(records, 0, "env",       TYPE_DICT,       rf_env,                  { 0                         });
    REC(records, 0, "memstat",   TYPE_DICT,       rf_memstat,              { 0                         });
  
    // Unary   
    REC(records, 1, "type",     -TYPE_SYMBOL,     OP_TYPE,                 { TYPE_ANY                  });
    REC(records, 1, "til" ,      TYPE_I64,        rf_til_i64,              {-TYPE_I64                  });
    REC(records, 1, "trace" ,    TYPE_I64,        OP_TRACE,                { TYPE_ANY                  });
    REC(records, 1, "distinct",  TYPE_I64,        rf_distinct_i64,         { TYPE_I64                  });
    REC(records, 1, "sum",      -TYPE_I64,        rf_sum_I64,              { TYPE_I64                  });
    REC(records, 1, "sum",      -TYPE_F64,        rf_sum_F64,              { TYPE_F64                  });
    REC(records, 1, "avg",      -TYPE_F64,        rf_avg_I64,              { TYPE_I64                  });
    REC(records, 1, "avg",      -TYPE_F64,        rf_avg_F64,              { TYPE_F64                  });
    REC(records, 1, "min",      -TYPE_I64,        rf_min_I64,              { TYPE_I64                  });
    REC(records, 1, "min",      -TYPE_F64,        rf_min_F64,              { TYPE_F64                  });
    REC(records, 1, "max",      -TYPE_I64,        rf_max_I64,              { TYPE_I64                  });
    REC(records, 1, "max",      -TYPE_F64,        rf_max_F64,              { TYPE_F64                  });
    REC(records, 1, "count",    -TYPE_I64,        rf_count,                { TYPE_ANY                  });
    REC(records, 1, "not",      -TYPE_BOOL,       rf_not_bool,             {-TYPE_BOOL                 });
    REC(records, 1, "not",       TYPE_BOOL,       rf_not_Bool,             { TYPE_BOOL                 });
 
    // Binary 
    REC(records, 2, "==",       -TYPE_BOOL,       rf_eq_i64_i64,           {-TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "==",       -TYPE_BOOL,       rf_eq_f64_f64,           {-TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "==",        TYPE_BOOL,       rf_eq_I64_i64,           { TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "==",        TYPE_BOOL,       rf_eq_I64_I64,           { TYPE_I64,     TYPE_I64    });
    REC(records, 2, "<",        -TYPE_BOOL,       rf_lt_i64_i64,           {-TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "<",        -TYPE_BOOL,       rf_lt_f64_f64,           {-TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "<",         TYPE_BOOL,       rf_lt_I64_i64,           { TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "<",         TYPE_BOOL,       rf_lt_I64_I64,           { TYPE_I64,     TYPE_I64    });
    REC(records, 2, ">",        -TYPE_BOOL,       rf_gt_i64_i64,           {-TYPE_I64,    -TYPE_I64    });
    REC(records, 2, ">",        -TYPE_BOOL,       rf_gt_f64_f64,           {-TYPE_F64,    -TYPE_F64    });
    REC(records, 2, ">",         TYPE_BOOL,       rf_gt_I64_i64,           { TYPE_I64,    -TYPE_I64    });
    REC(records, 2, ">",         TYPE_BOOL,       rf_gt_I64_I64,           { TYPE_I64,     TYPE_I64    });
    REC(records, 2, "<=",       -TYPE_BOOL,       rf_le_i64_i64,           {-TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "<=",       -TYPE_BOOL,       rf_le_f64_f64,           {-TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "<=",        TYPE_BOOL,       rf_le_I64_i64,           { TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "<=",        TYPE_BOOL,       rf_le_I64_I64,           { TYPE_I64,     TYPE_I64    });
    REC(records, 2, ">=",       -TYPE_BOOL,       rf_ge_i64_i64,           {-TYPE_I64,    -TYPE_I64    });
    REC(records, 2, ">=",       -TYPE_BOOL,       rf_ge_f64_f64,           {-TYPE_F64,    -TYPE_F64    });
    REC(records, 2, ">=",        TYPE_BOOL,       rf_ge_I64_i64,           { TYPE_I64,    -TYPE_I64    });
    REC(records, 2, ">=",        TYPE_BOOL,       rf_ge_I64_I64,           { TYPE_I64,     TYPE_I64    });
    REC(records, 2, "!=",        TYPE_BOOL,       rf_ne_i64_i64,           {-TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "!=",        TYPE_BOOL,       rf_ne_f64_f64,           {-TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "!=",       -TYPE_BOOL,       rf_ne_I64_i64,           { TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "!=",       -TYPE_BOOL,       rf_ne_I64_I64,           { TYPE_I64,     TYPE_I64    });
    REC(records, 2, "and",       TYPE_BOOL,       rf_and_Bool_Bool,        { TYPE_BOOL,    TYPE_BOOL   });
    REC(records, 2, "or",        TYPE_BOOL,       rf_or_Bool_Bool,         { TYPE_BOOL,    TYPE_BOOL   });
    REC(records, 2, "+",        -TYPE_I64,        rf_add_i64_i64,          {-TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "+",        -TYPE_F64,        rf_add_f64_f64,          {-TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "+",         TYPE_I64,        rf_add_I64_i64,          { TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "+",         TYPE_I64,        rf_add_I64_I64,          { TYPE_I64,     TYPE_I64    });
    REC(records, 2, "+",         TYPE_F64,        rf_add_F64_f64,          { TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "+",         TYPE_F64,        rf_add_F64_F64,          { TYPE_F64,     TYPE_F64    });
    REC(records, 2, "-",        -TYPE_I64,        rf_sub_i64_i64,          {-TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "-",        -TYPE_F64,        rf_sub_f64_f64,          {-TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "-",         TYPE_I64,        rf_sub_I64_i64,          { TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "-",         TYPE_I64,        rf_sub_I64_I64,          { TYPE_I64,     TYPE_I64    });
    REC(records, 2, "-",         TYPE_F64,        rf_sub_F64_f64,          { TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "-",         TYPE_F64,        rf_sub_F64_F64,          { TYPE_F64,     TYPE_F64    });
    REC(records, 2, "*",        -TYPE_I64,        rf_mul_i64_i64,          {-TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "*",        -TYPE_F64,        rf_mul_f64_f64,          {-TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "*",         TYPE_I64,        rf_mul_I64_i64,          { TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "*",         TYPE_I64,        rf_mul_I64_I64,          { TYPE_I64,     TYPE_I64    });
    REC(records, 2, "*",         TYPE_F64,        rf_mul_F64_f64,          { TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "*",         TYPE_F64,        rf_mul_F64_F64,          { TYPE_F64,     TYPE_F64    });
    REC(records, 2, "/",        -TYPE_I64,        rf_div_i64_i64,          {-TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "/",        -TYPE_F64,        rf_div_f64_f64,          {-TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "/",         TYPE_I64,        rf_div_I64_i64,          { TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "/",         TYPE_I64,        rf_div_I64_I64,          { TYPE_I64,     TYPE_I64    });
    REC(records, 2, "/",         TYPE_F64,        rf_div_F64_f64,          { TYPE_F64,    -TYPE_F64    });
    REC(records, 2, "/",         TYPE_F64,        rf_div_F64_F64,          { TYPE_F64,     TYPE_F64    });
    REC(records, 2, "like",     -TYPE_BOOL,       rf_like_Char_Char,       { TYPE_CHAR,    TYPE_CHAR   });
    REC(records, 2, "dict",      TYPE_DICT,       rf_dict,                 { TYPE_ANY,     TYPE_ANY    });
    REC(records, 2, "nth",      -TYPE_I64,        rf_nth_I64_i64,          { TYPE_I64,    -TYPE_I64    });
    REC(records, 2, "nth",      -TYPE_F64,        rf_nth_F64_i64,          { TYPE_F64,    -TYPE_I64    });
    REC(records, 2, "nth",      -TYPE_I64,        rf_nth_I64_I64,          { TYPE_I64,     TYPE_I64    });
    REC(records, 2, "nth",      -TYPE_F64,        rf_nth_F64_I64,          { TYPE_F64,     TYPE_I64    });
    REC(records, 2, "nth",       TYPE_CHAR,       rf_nth_Char_i64,         { TYPE_CHAR,   -TYPE_I64    });
    REC(records, 2, "nth",       TYPE_CHAR,       rf_nth_Char_I64,         { TYPE_CHAR,    TYPE_I64    });
    REC(records, 2, "nth",       TYPE_ANY,        rf_nth_List_i64,         { TYPE_LIST,   -TYPE_I64    });
    REC(records, 2, "nth",       TYPE_LIST,       rf_nth_List_I64,         { TYPE_LIST,    TYPE_I64    });

    // Ternary  
    // Quaternary  
    // Nary  
    REC(records, 5, "list",      TYPE_LIST,       rf_list,                 { 0                         });
    REC(records, 5, "format",    TYPE_CHAR,       rf_format,               { 0                         });
    REC(records, 5, "print",     TYPE_NULL,       rf_print,                { 0                         });
    REC(records, 5, "println",   TYPE_NULL,       rf_println,              { 0                         });
}

null_t init_typenames(i64_t *typenames)
{
    typenames[-TYPE_BOOL    + TYPE_OFFSET] = symbol("bool").i64;
    typenames[-TYPE_I64     + TYPE_OFFSET] = symbol("i64").i64;
    typenames[-TYPE_F64     + TYPE_OFFSET] = symbol("f64").i64;
    typenames[-TYPE_SYMBOL  + TYPE_OFFSET] = symbol("symbol").i64;
    typenames[-TYPE_CHAR    + TYPE_OFFSET] = symbol("char").i64;
    typenames[TYPE_NULL     + TYPE_OFFSET] = symbol("Null").i64;
    typenames[TYPE_BOOL     + TYPE_OFFSET] = symbol("Bool").i64;
    typenames[TYPE_I64      + TYPE_OFFSET] = symbol("I64").i64;
    typenames[TYPE_F64      + TYPE_OFFSET] = symbol("F64").i64;
    typenames[TYPE_SYMBOL   + TYPE_OFFSET] = symbol("Symbol").i64;
    typenames[TYPE_CHAR     + TYPE_OFFSET] = symbol("Char").i64;
    typenames[TYPE_LIST     + TYPE_OFFSET] = symbol("List").i64;
    typenames[TYPE_DICT     + TYPE_OFFSET] = symbol("Dict").i64;
    typenames[TYPE_TABLE    + TYPE_OFFSET] = symbol("Table").i64;
    typenames[TYPE_FUNCTION + TYPE_OFFSET] = symbol("Function").i64;
}
// clang-format on

env_t create_env()
{
    rf_object_t functions = list(REC_SIZE);
    rf_object_t variables = dict(vector_symbol(0), vector_i64(0));

    for (i32_t i = 0; i < REC_SIZE; i++)
        as_list(&functions)[i] = vector(TYPE_CHAR, sizeof(env_record_t), 0);

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