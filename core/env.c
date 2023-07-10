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
#include "guid.h"
#include "runtime.h"
#include "format.h"

#define regf(r, n, t, o)                                            \
    {                                                               \
        rf_object_t k = symboli64(intern_keyword(n, strlen(n)));    \
        dict_set(r, &k, (rf_object_t){.type = t, .i64 = (i64_t)o}); \
    };

#define regt(r, i, s)               \
    {                               \
        rf_object_t k = i64(i);     \
        dict_set(r, &k, symbol(s)); \
    };

rf_object_t rf_env()
{
    return rf_object_clone(&runtime_get()->env.variables);
}

rf_object_t rf_memstat()
{
    rf_object_t keys, vals;
    memstat_t stat = rf_alloc_memstat();

    keys = vector_symbol(3);
    as_vector_symbol(&keys)[0] = symbol("total").i64;
    as_vector_symbol(&keys)[1] = symbol("used ").i64;
    as_vector_symbol(&keys)[2] = symbol("free ").i64;

    vals = list(3);
    as_list(&vals)[0] = i64(stat.total);
    as_list(&vals)[1] = i64(stat.used);
    as_list(&vals)[2] = i64(stat.free);

    return dict(keys, vals);
}

// clang-format off
null_t init_functions(rf_object_t *functions)
{
    // Unary
    regf(functions, "type",        TYPE_UNARY,    rf_type);
    regf(functions, "til",         TYPE_UNARY,    rf_til);
    // REC(functions,  "trace",       OP_TRACE);
    regf(functions,  "distinct",   TYPE_UNARY,    rf_distinct);
    regf(functions,  "group",      TYPE_UNARY,    rf_group);
    regf(functions,  "sum",        TYPE_UNARY,    rf_sum);
    regf(functions,  "avg",        TYPE_UNARY,    rf_avg);
    regf(functions,  "min",        TYPE_UNARY,    rf_min);
    regf(functions,  "max",        TYPE_UNARY,    rf_max);
    regf(functions,  "count",      TYPE_UNARY,    rf_count);
    regf(functions,  "not",        TYPE_UNARY,    rf_not);
    regf(functions,  "iasc",       TYPE_UNARY,    rf_iasc);
    regf(functions,  "idesc",      TYPE_UNARY,    rf_idesc);
    regf(functions,  "asc",        TYPE_UNARY,    rf_asc);
    regf(functions,  "desc",       TYPE_UNARY,    rf_desc);
    regf(functions,  "flatten",    TYPE_UNARY,    vector_flatten);
    regf(functions,  "guid",       TYPE_UNARY,    rf_guid_generate);
    regf(functions,  "neg",        TYPE_UNARY,    rf_neg);
    regf(functions,  "where",      TYPE_UNARY,    rf_where);
    regf(functions,  "value",      TYPE_UNARY,    rf_value);
      
    // Binary         
    regf(functions, "==",          TYPE_BINARY,   rf_eq);
    regf(functions, "<",           TYPE_BINARY,   rf_lt);
    regf(functions, ">",           TYPE_BINARY,   rf_gt);
    regf(functions, "<=",          TYPE_BINARY,   rf_le);
    regf(functions, ">=",          TYPE_BINARY,   rf_ge);
    regf(functions, "!=",          TYPE_BINARY,   rf_ne);
    regf(functions, "and",         TYPE_BINARY,   rf_and);
    regf(functions, "or",          TYPE_BINARY,   rf_or);
    regf(functions,  "+",          TYPE_BINARY,   rf_add);
    regf(functions, "-",           TYPE_BINARY,   rf_sub);
    regf(functions, "*",           TYPE_BINARY,   rf_mul);
    regf(functions, "/",           TYPE_BINARY,   rf_div);
    regf(functions, "%",           TYPE_BINARY,   rf_mod);
    regf(functions, "div",         TYPE_BINARY,   rf_fdiv);
    regf(functions, "like",        TYPE_BINARY,   rf_like);
    regf(functions, "dict",        TYPE_BINARY,   rf_dict);
    regf(functions, "table",       TYPE_BINARY,   rf_table);
    regf(functions, "get",         TYPE_BINARY,   rf_get);
    regf(functions, "find",        TYPE_BINARY,   rf_find);
    regf(functions, "concat",      TYPE_BINARY,   rf_concat);
    regf(functions, "filter",      TYPE_BINARY,   rf_filter);
    regf(functions, "take",        TYPE_BINARY,   rf_take);
    regf(functions, "in",          TYPE_BINARY,   rf_in);
    regf(functions, "sect",        TYPE_BINARY,   rf_sect);
    regf(functions, "except",      TYPE_BINARY,   rf_except);
    regf(functions, "rand",        TYPE_BINARY,   rf_rand);
     
    // Lambdas       
    // regf(functions, "env",         rf_env);
    // regf(functions, "memstat",     rf_memstat);
    regf(functions,  "list",       TYPE_VARY,   rf_list);
    regf(functions, "format",      TYPE_VARY,   rf_format);
    regf(functions, "print",       TYPE_VARY,   rf_print);
    regf(functions, "println",     TYPE_VARY,   rf_println);
}    
    
null_t init_typenames(rf_object_t *typenames)    
{
    regt(typenames, -TYPE_BOOL,       "bool");
    regt(typenames, -TYPE_I64,        "i64");
    regt(typenames, -TYPE_F64,        "f64");
    regt(typenames, -TYPE_CHAR,       "char");
    regt(typenames, -TYPE_SYMBOL,     "symbol");
    regt(typenames, -TYPE_TIMESTAMP,  "timestamp");
    regt(typenames, -TYPE_GUID,       "guid");
    regt(typenames, TYPE_BOOL,        "Bool");
    regt(typenames, TYPE_I64,         "I64");
    regt(typenames, TYPE_F64,         "F64");
    regt(typenames, TYPE_CHAR,        "Char");
    regt(typenames, TYPE_SYMBOL,      "Symbol");
    regt(typenames, TYPE_TIMESTAMP,   "Timestamp");
    regt(typenames, TYPE_GUID,        "Guid");
    regt(typenames, TYPE_LIST,        "List");
    regt(typenames, TYPE_TABLE,       "Table");
    regt(typenames, TYPE_DICT,        "Dict");
    regt(typenames, TYPE_UNARY,       "Unary");
    regt(typenames, TYPE_BINARY,      "Binary");
    regt(typenames, TYPE_VARY,        "Vary");
    regt(typenames, TYPE_LAMBDA,      "Lambda");
    regt(typenames, TYPE_ERROR,       "Error");
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
    rf_object_t functions = dict(vector_symbol(0), list(0));
    rf_object_t variables = dict(vector_symbol(0), list(0));
    rf_object_t typenames = dict(vector_i64(0), vector_symbol(0));

    init_kw_symbols();

    init_functions(&functions);

    init_typenames(&typenames);

    env_t env = {
        .functions = functions,
        .variables = variables,
        .typenames = typenames,
    };

    return env;
}

null_t free_env(env_t *env)
{
    rf_object_free(&env->variables);
    rf_object_free(&env->functions);
    rf_object_free(&env->typenames);
}

i64_t env_get_typename_by_type(env_t *env, type_t type)
{
    rf_object_t k, n;
    k = i64(type);
    n = dict_get(&env->typenames, &k);

    return n.i64;
}

type_t env_get_type_by_typename(env_t *env, i64_t name)
{
    i64_t i, l;
    l = as_list(&env->typenames)[1].adt->len;

    for (i = 0; i < l; i++)
        if (as_list(&as_list(&env->typenames)[1])[i].i64 == name)
            return (type_t)as_list(&as_list(&env->typenames)[0])[i].i64;

    return TYPE_NONE;
}

str_t env_get_typename(type_t type)
{
    env_t *env = &runtime_get()->env;
    i64_t name = env_get_typename_by_type(env, type);

    return symbols_get(name);
}