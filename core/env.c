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
#include "ternary.h"
#include "nary.h"
#include "guid.h"
#include "runtime.h"

#define REC_SIZE (MAX_ARITY + 2)

#define REC(r, a, n, o)                          \
    {                                            \
        i64_t id = intern_keyword(n, strlen(n)); \
        env_record_t rec = {                     \
            .id = id,                            \
            .op = (i64_t)o,                      \
            .arity = a,                          \
        };                                       \
        push(&as_list(r)[a], env_record_t, rec); \
    }

// clang-format off
null_t init_functions(rf_object_t *records)
{
    // Nilary
    REC(records, 0, "halt",        OP_HALT);
    REC(records, 0, "env",         rf_env);
    REC(records, 0, "memstat",     rf_memstat);
            
    // Unary
    REC(records, 1, "type",        rf_type);
    REC(records, 1, "til" ,        rf_til);
    REC(records, 1, "trace",       OP_TRACE);
    REC(records, 1, "distinct",    rf_distinct);
    REC(records, 1, "group",       rf_group);
    REC(records, 1, "sum",         rf_sum);
    REC(records, 1, "avg",         rf_avg);
    REC(records, 1, "min",         rf_min);
    REC(records, 1, "max",         rf_max);
    REC(records, 1, "count",       rf_count);
    REC(records, 1, "not",         rf_not);
    REC(records, 1, "iasc",        rf_iasc);
    REC(records, 1, "idesc",       rf_idesc);
    REC(records, 1, "asc",         rf_asc);
    REC(records, 1, "desc",        rf_desc);
    REC(records, 1, "flatten",     vector_flatten);
    REC(records, 1, "guid",        rf_guid_generate);
    REC(records, 1, "neg",         rf_neg);
    REC(records, 1, "where",       rf_where);
      
    // Binary         
    REC(records, 2, "==",          rf_eq);
    REC(records, 2, "<",           rf_lt);
    REC(records, 2, ">",           rf_gt);
    REC(records, 2, "<=",          rf_le);
    REC(records, 2, ">=",          rf_ge);
    REC(records, 2, "!=",          rf_ne);
    REC(records, 2, "and",         rf_and);
    REC(records, 2, "or",          rf_or);
    REC(records, 2, "+",           rf_add);
    REC(records, 2, "-",           rf_sub);
    REC(records, 2, "*",           rf_mul);
    REC(records, 2, "/",           rf_div);
    REC(records, 2, "%",           rf_mod);
    REC(records, 2, "div",         rf_fdiv);
    REC(records, 2, "like",        rf_like);
    REC(records, 2, "dict",        rf_dict);
    REC(records, 2, "table",       rf_table);
    REC(records, 2, "get",         rf_get);
    REC(records, 2, "find",        rf_find);
    REC(records, 2, "concat",      rf_concat);
    REC(records, 2, "filter",      rf_filter);
    REC(records, 2, "take",        rf_take);
    REC(records, 2, "in",          rf_in);
    REC(records, 2, "except",      rf_except);
     
    // Ternary       
    REC(records, 3, "rand",        rf_rand);
         
    // Quaternary

    // Nary       
    REC(records, 5, "list",        rf_list);
    REC(records, 5, "format",      rf_format);
    REC(records, 5, "print",       rf_print);
    REC(records, 5, "println",     rf_println);
}    
    
null_t init_typenames(i64_t *typenames)    
{
    typenames[-TYPE_BOOL      + TYPE_OFFSET] = symbol("bool").i64;
    typenames[-TYPE_I64       + TYPE_OFFSET] = symbol("i64").i64;
    typenames[-TYPE_F64       + TYPE_OFFSET] = symbol("f64").i64;
    typenames[-TYPE_SYMBOL    + TYPE_OFFSET] = symbol("symbol").i64;
    typenames[-TYPE_TIMESTAMP + TYPE_OFFSET] = symbol("timestamp").i64;
    typenames[-TYPE_GUID      + TYPE_OFFSET] = symbol("guid").i64;
    typenames[-TYPE_CHAR      + TYPE_OFFSET] = symbol("char").i64;
    typenames[ TYPE_NULL      + TYPE_OFFSET] = symbol("Null").i64;
    typenames[ TYPE_BOOL      + TYPE_OFFSET] = symbol("Bool").i64;
    typenames[ TYPE_I64       + TYPE_OFFSET] = symbol("I64").i64;
    typenames[ TYPE_F64       + TYPE_OFFSET] = symbol("F64").i64;
    typenames[ TYPE_SYMBOL    + TYPE_OFFSET] = symbol("Symbol").i64;
    typenames[ TYPE_TIMESTAMP + TYPE_OFFSET] = symbol("Timestamp").i64;
    typenames[ TYPE_GUID      + TYPE_OFFSET] = symbol("Guid").i64;
    typenames[ TYPE_CHAR      + TYPE_OFFSET] = symbol("Char").i64;
    typenames[ TYPE_LIST      + TYPE_OFFSET] = symbol("List").i64;
    typenames[ TYPE_DICT      + TYPE_OFFSET] = symbol("Dict").i64;
    typenames[ TYPE_TABLE     + TYPE_OFFSET] = symbol("Table").i64;
    typenames[ TYPE_FUNCTION  + TYPE_OFFSET] = symbol("Function").i64;
    typenames[ TYPE_ERROR     + TYPE_OFFSET] = symbol("Error").i64;
}


null_t init_kw_symbols()
{
    assert(intern_symbol("",       0)  == NULL_SYM);
    assert(intern_keyword("time",  4)  == KW_TIME);
    assert(intern_keyword("`",     1)  == KW_QUOTE);
    assert(intern_keyword("set",   3)  == KW_SET);
    assert(intern_keyword("let",   3)  == KW_LET);
    assert(intern_keyword("fn",    2)  == KW_FN);
    assert(intern_keyword("self",  4)  == KW_SELF);
    assert(intern_keyword("if",    2)  == KW_IF);
    assert(intern_keyword("try",   3)  == KW_TRY);
    assert(intern_keyword("catch", 5)  == KW_CATCH);
    assert(intern_keyword("throw", 5)  == KW_THROW);
    assert(intern_keyword("map",   3)  == KW_MAP);
    assert(intern_keyword("select",6)  == KW_SELECT);
    assert(intern_keyword("from",  4)  == KW_FROM);
    assert(intern_keyword("where", 5)  == KW_WHERE);
    assert(intern_keyword("by",    2)  == KW_BY);
    assert(intern_keyword("order", 5)  == KW_ORDER);
}
// clang-format on

env_t create_env()
{
    rf_object_t functions = list(REC_SIZE);
    rf_object_t variables = dict(vector_symbol(0), list(0));

    for (i32_t i = 0; i < REC_SIZE; i++)
        as_list(&functions)[i] = vector(TYPE_CHAR, 0);

    init_kw_symbols();

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
    rf_object_free(&env->variables);
    rf_object_free(&env->functions);
}

i64_t env_get_typename_by_type(env_t *env, type_t type)
{
    return env->typenames[type + TYPE_OFFSET];
}

type_t env_get_type_by_typename(env_t *env, i64_t name)
{
    for (i32_t i = 0; i < MAX_TYPE; i++)
        if (env->typenames[i] == name)
            return i - TYPE_OFFSET;

    return TYPE_NONE;
}

str_t env_get_typename(type_t type)
{
    env_t *env = &runtime_get()->env;
    i64_t name = env_get_typename_by_type(env, type);

    return symbols_get(name);
}