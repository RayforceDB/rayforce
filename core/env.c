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
#include "util.h"
#include "unary.h"
#include "binary.h"
#include "vary.h"
#include "guid.h"
#include "runtime.h"
#include "format.h"
#include "ops.h"
#include "serde.h"

#define regf(r, n, t, f, o)                      \
    {                                            \
        i64_t _k = intern_keyword(n, strlen(n)); \
        join_raw(&as_list(r)[0], &_k);           \
        obj_t _o = atom(-t);                     \
        _o->attrs = f;                           \
        _o->i64 = (i64_t)o;                      \
        join_raw(&as_list(r)[1], &_o);           \
    };

#define regt(r, i, s)                  \
    {                                  \
        i64_t _i = i;                  \
        join_raw(&as_list(r)[0], &_i); \
        join_sym(&as_list(r)[1], s);   \
    };

obj_t rf_env()
{
    return clone(runtime_get()->env.variables);
}

obj_t rf_memstat()
{
    // obj_t keys, vals;
    // memstat_t stat = heap_memstat();

    // keys = vector_symbol(3);
    // as_symbol(&keys)[0] = symbol("total").i64;
    // as_symbol(&keys)[1] = symbol("used ").i64;
    // as_symbol(&keys)[2] = symbol("free ").i64;

    // vals = list(3);
    // as_list(&vals)[0] = i64(stat.total);
    // as_list(&vals)[1] = i64(stat.used);
    // as_list(&vals)[2] = i64(stat.free);

    // return dict(keys, vals);

    return null(0);
}

// clang-format off
nil_t init_functions(obj_t functions)
{
    // Unary
    regf(functions,  "get",       TYPE_UNARY,    FLAG_NONE,         rf_get);
    regf(functions,  "type",      TYPE_UNARY,    FLAG_NONE,         rf_type);
    regf(functions,  "til",       TYPE_UNARY,    FLAG_NONE,         rf_til);
    regf(functions,  "distinct",  TYPE_UNARY,    FLAG_ATOMIC,       rf_distinct);
    regf(functions,  "group",     TYPE_UNARY,    FLAG_ATOMIC,       rf_group);
    regf(functions,  "sum",       TYPE_UNARY,    FLAG_ATOMIC,       rf_sum);
    regf(functions,  "avg",       TYPE_UNARY,    FLAG_ATOMIC,       rf_avg);
    regf(functions,  "min",       TYPE_UNARY,    FLAG_ATOMIC,       rf_min);
    regf(functions,  "max",       TYPE_UNARY,    FLAG_ATOMIC,       rf_max);
    regf(functions,  "count",     TYPE_UNARY,    FLAG_NONE,         rf_count);
    regf(functions,  "not",       TYPE_UNARY,    FLAG_ATOMIC,       rf_not);
    regf(functions,  "iasc",      TYPE_UNARY,    FLAG_ATOMIC,       rf_iasc);
    regf(functions,  "idesc",     TYPE_UNARY,    FLAG_ATOMIC,       rf_idesc);
    regf(functions,  "asc",       TYPE_UNARY,    FLAG_ATOMIC,       rf_asc);
    regf(functions,  "desc",      TYPE_UNARY,    FLAG_ATOMIC,       rf_desc);
    regf(functions,  "guid",      TYPE_UNARY,    FLAG_ATOMIC,       rf_guid_generate);
    regf(functions,  "neg",       TYPE_UNARY,    FLAG_ATOMIC,       rf_neg);
    regf(functions,  "where",     TYPE_UNARY,    FLAG_ATOMIC,       rf_where);
    regf(functions,  "key",       TYPE_UNARY,    FLAG_NONE,         rf_key);
    regf(functions,  "value",     TYPE_UNARY,    FLAG_NONE,         rf_value);
    regf(functions,  "read",      TYPE_UNARY,    FLAG_NONE,         rf_read);
    regf(functions,  "parse",     TYPE_UNARY,    FLAG_NONE,         rf_parse);
    regf(functions,  "ser",       TYPE_UNARY,    FLAG_NONE,         ser);
    regf(functions,  "de",        TYPE_UNARY,    FLAG_NONE,         de);
    
    // Binary           
    regf(functions,  "set",       TYPE_BINARY,   FLAG_NONE,         rf_set);
    regf(functions,  "==",        TYPE_BINARY,   FLAG_ATOMIC,       rf_eq);
    regf(functions,  "<",         TYPE_BINARY,   FLAG_ATOMIC,       rf_lt);
    regf(functions,  ">",         TYPE_BINARY,   FLAG_ATOMIC,       rf_gt);
    regf(functions,  "<=",        TYPE_BINARY,   FLAG_ATOMIC,       rf_le);
    regf(functions,  ">=",        TYPE_BINARY,   FLAG_ATOMIC,       rf_ge);
    regf(functions,  "!=",        TYPE_BINARY,   FLAG_ATOMIC,       rf_ne);
    regf(functions,  "and",       TYPE_BINARY,   FLAG_ATOMIC,       rf_and);
    regf(functions,  "or",        TYPE_BINARY,   FLAG_ATOMIC,       rf_or);
    regf(functions,   "+",        TYPE_BINARY,   FLAG_ATOMIC,       rf_add);
    regf(functions,  "-",         TYPE_BINARY,   FLAG_ATOMIC,       rf_sub);
    regf(functions,  "*",         TYPE_BINARY,   FLAG_ATOMIC,       rf_mul);
    regf(functions,  "/",         TYPE_BINARY,   FLAG_ATOMIC,       rf_div);
    regf(functions,  "%",         TYPE_BINARY,   FLAG_ATOMIC,       rf_mod);
    regf(functions,  "div",       TYPE_BINARY,   FLAG_ATOMIC,       rf_fdiv);
    regf(functions,  "like",      TYPE_BINARY,   FLAG_ATOMIC,       rf_like);
    regf(functions,  "dict",      TYPE_BINARY,   FLAG_NONE,         rf_dict);
    regf(functions,  "table",     TYPE_BINARY,   FLAG_NONE,         rf_table);
    regf(functions,  "find",      TYPE_BINARY,   FLAG_ATOMIC,       rf_find);
    regf(functions,  "concat",    TYPE_BINARY,   FLAG_NONE,         rf_concat);
    regf(functions,  "filter",    TYPE_BINARY,   FLAG_ATOMIC,       rf_filter);
    regf(functions,  "take",      TYPE_BINARY,   FLAG_RIGHT_ATOMIC, rf_take);
    regf(functions,  "in",        TYPE_BINARY,   FLAG_ATOMIC,       rf_in);
    regf(functions,  "sect",      TYPE_BINARY,   FLAG_ATOMIC,       rf_sect);
    regf(functions,  "except",    TYPE_BINARY,   FLAG_ATOMIC,       rf_except);
    regf(functions,  "rand",      TYPE_BINARY,   FLAG_ATOMIC,       rf_rand);
    regf(functions,  "as",        TYPE_BINARY,   FLAG_NONE,         rf_cast);
    regf(functions,  "xasc",      TYPE_BINARY,   FLAG_NONE,         rf_xasc);
    regf(functions,  "xdesc",     TYPE_BINARY,   FLAG_NONE,         rf_xdesc);
    regf(functions,  "write",     TYPE_BINARY,   FLAG_NONE,         rf_write);
    
    // Lambdas       
    // regf(function s, "env",        rf_env);
    // regf(function s, "memstat",    rf_memstat);
    regf(functions,  "gc",        TYPE_VARY,     FLAG_NONE,         rf_gc);
    regf(functions,  "list",      TYPE_VARY,     FLAG_NONE,         rf_list);
    regf(functions,  "enlist",    TYPE_VARY,     FLAG_NONE,         rf_enlist);
    regf(functions,  "format",    TYPE_VARY,     FLAG_NONE,         rf_format);
    regf(functions,  "print",     TYPE_VARY,     FLAG_NONE,         rf_print);
    regf(functions,  "println",   TYPE_VARY,     FLAG_NONE,         rf_println);
    regf(functions,  "map",       TYPE_VARY,     FLAG_NONE,         rf_map_vary);
}    
    
nil_t init_typenames(obj_t typenames)    
{
    regt(typenames,   -TYPE_ERROR,      "Null");
    regt(typenames,   -TYPE_BOOL,       "bool");
    regt(typenames,   -TYPE_BYTE,       "byte");
    regt(typenames,   -TYPE_I64,        "i64");
    regt(typenames,   -TYPE_F64,        "f64");
    regt(typenames,   -TYPE_CHAR,       "char");
    regt(typenames,   -TYPE_SYMBOL,     "symbol");
    regt(typenames,   -TYPE_TIMESTAMP,  "timestamp");
    regt(typenames,   -TYPE_GUID,       "guid");
    regt(typenames,    TYPE_BOOL,       "Bool");
    regt(typenames,    TYPE_BYTE,       "Byte");
    regt(typenames,    TYPE_I64,        "I64");
    regt(typenames,    TYPE_F64,        "F64");
    regt(typenames,    TYPE_CHAR,       "string");
    regt(typenames,    TYPE_SYMBOL,     "Symbol");
    regt(typenames,    TYPE_TIMESTAMP,  "Timestamp");
    regt(typenames,    TYPE_GUID,       "Guid");
    regt(typenames,    TYPE_LIST,       "List");
    regt(typenames,    TYPE_TABLE,      "Table");
    regt(typenames,    TYPE_DICT,       "Dict");
    regt(typenames,    TYPE_UNARY,      "Unary");
    regt(typenames,    TYPE_BINARY,     "Binary");
    regt(typenames,    TYPE_VARY,       "Vary");
    regt(typenames,    TYPE_LAMBDA,     "Lambda");
    regt(typenames,    TYPE_ERROR,      "Error");
}


nil_t init_kw_symbols()
{
    assert(intern_symbol("",         0)  == NULL_SYM);
    assert(intern_keyword("time",    4)  == KW_TIME);
    assert(intern_keyword("`",       1)  == KW_QUOTE);
    assert(intern_keyword("set",     3)  == KW_SET);
    assert(intern_keyword("let",     3)  == KW_LET);
    assert(intern_keyword("fn",      2)  == KW_FN);
    assert(intern_keyword("self",    4)  == KW_SELF);
    assert(intern_keyword("if",      2)  == KW_IF);
    assert(intern_keyword("try",     3)  == KW_TRY);
    assert(intern_keyword("catch",   5)  == KW_CATCH);
    assert(intern_keyword("throw",   5)  == KW_THROW);
    assert(intern_keyword("map",     3)  == KW_MAP);
    assert(intern_keyword("select",  6)  == KW_SELECT);
    assert(intern_keyword("from",    4)  == KW_FROM);
    assert(intern_keyword("where",   5)  == KW_WHERE);
    assert(intern_keyword("by",      2)  == KW_BY);
    assert(intern_keyword("order",   5)  == KW_ORDER);
    assert(intern_keyword("eval",    4)  == KW_EVAL);
    assert(intern_keyword("load",    4)  == KW_LOAD);
    assert(intern_keyword("return",  6)  == KW_RETURN);

}
// clang-format on

env_t create_env()
{
    obj_t functions = dict(vector_symbol(0), list(0));
    obj_t variables = dict(vector_symbol(0), list(0));
    obj_t typenames = dict(vector_i64(0), vector_symbol(0));

    init_kw_symbols();

    init_functions(functions);

    init_typenames(typenames);

    env_t env = {
        .functions = functions,
        .variables = variables,
        .typenames = typenames,
    };

    return env;
}

nil_t free_env(env_t *env)
{
    drop(env->functions);
    drop(env->variables);
    drop(env->typenames);
}

i64_t env_get_typename_by_type(env_t *env, type_t type)
{
    i64_t t, i;

    t = type;
    i = find_raw(as_list(env->typenames)[0], &t);

    if (i == (i64_t)as_list(env->typenames)[0]->len)
        return as_symbol(as_list(env->typenames)[1])[0];

    return as_symbol(as_list(env->typenames)[1])[i];
}

type_t env_get_type_by_typename(env_t *env, i64_t name)
{
    i64_t n, i;

    n = name;
    i = find_raw(as_list(env->typenames)[1], &n);

    if (i == (i64_t)as_list(env->typenames)[1]->len)
        return TYPE_ERROR;

    return (type_t)as_i64(as_list(env->typenames)[0])[i];
}

str_t env_get_typename(type_t type)
{
    env_t *env = &runtime_get()->env;
    i64_t name = env_get_typename_by_type(env, type);

    return symtostr(name);
}