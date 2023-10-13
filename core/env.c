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
#include "math.h"
#include "rel.h"
#include "logic.h"
#include "items.h"
#include "compose.h"
#include "order.h"
#include "misc.h"
#include "io.h"

#define regf(r, n, t, f, o)                      \
    {                                            \
        i64_t _k = intern_keyword(n, strlen(n)); \
        push_raw(&as_list(r)[0], &_k);           \
        obj_t _o = atom(-t);                     \
        _o->attrs = f;                           \
        _o->i64 = (i64_t)o;                      \
        push_raw(&as_list(r)[1], &_o);           \
    };

#define regt(r, i, s)                  \
    {                                  \
        i64_t _i = i;                  \
        push_raw(&as_list(r)[0], &_i); \
        push_sym(&as_list(r)[1], s);   \
    };

obj_t ray_env()
{
    return clone(runtime_get()->env.variables);
}

obj_t ray_memstat()
{
    obj_t keys, vals;
    memstat_t stat = heap_memstat();

    keys = vector_symbol(3);
    ins_sym(&keys, 0, "total");
    ins_sym(&keys, 1, "used ");
    ins_sym(&keys, 2, "free ");

    vals = vector(TYPE_LIST, 3);
    as_list(vals)[0] = i64(stat.total);
    as_list(vals)[1] = i64(stat.used);
    as_list(vals)[2] = i64(stat.free);

    return dict(keys, vals);
}

// clang-format off
nil_t init_functions(obj_t functions)
{
    // Unary
    regf(functions,  "get",       TYPE_UNARY,    FN_NONE,           ray_get);
    regf(functions,  "read",      TYPE_UNARY,    FN_NONE,           ray_read);
    regf(functions,  "type",      TYPE_UNARY,    FN_NONE,           ray_type);
    regf(functions,  "til",       TYPE_UNARY,    FN_NONE,           ray_til);
    regf(functions,  "reverse",   TYPE_UNARY,    FN_NONE,           ray_reverse);
    regf(functions,  "distinct",  TYPE_UNARY,    FN_NONE,           ray_distinct);
    regf(functions,  "group",     TYPE_UNARY,    FN_NONE,           ray_group);
    regf(functions,  "sum",       TYPE_UNARY,    FN_ATOMIC,         ray_sum);
    regf(functions,  "avg",       TYPE_UNARY,    FN_NONE,           ray_avg);
    regf(functions,  "min",       TYPE_UNARY,    FN_ATOMIC,         ray_min);
    regf(functions,  "max",       TYPE_UNARY,    FN_ATOMIC,         ray_max);
    regf(functions,  "first",     TYPE_UNARY,    FN_NONE,           ray_first);
    regf(functions,  "last",      TYPE_UNARY,    FN_NONE,           ray_last);
    regf(functions,  "count",     TYPE_UNARY,    FN_NONE,           ray_count);
    regf(functions,  "not",       TYPE_UNARY,    FN_ATOMIC,         ray_not);
    regf(functions,  "iasc",      TYPE_UNARY,    FN_ATOMIC,         ray_iasc);
    regf(functions,  "idesc",     TYPE_UNARY,    FN_ATOMIC,         ray_idesc);
    regf(functions,  "asc",       TYPE_UNARY,    FN_ATOMIC,         ray_asc);
    regf(functions,  "desc",      TYPE_UNARY,    FN_ATOMIC,         ray_desc);
    regf(functions,  "guid",      TYPE_UNARY,    FN_ATOMIC,         ray_guid);
    regf(functions,  "neg",       TYPE_UNARY,    FN_ATOMIC,         ray_neg);
    regf(functions,  "where",     TYPE_UNARY,    FN_ATOMIC,         ray_where);
    regf(functions,  "key",       TYPE_UNARY,    FN_NONE,           ray_key);
    regf(functions,  "value",     TYPE_UNARY,    FN_NONE,           ray_value);
    regf(functions,  "parse",     TYPE_UNARY,    FN_NONE,           ray_parse);
    regf(functions,  "ser",       TYPE_UNARY,    FN_NONE,           ser);
    regf(functions,  "de",        TYPE_UNARY,    FN_NONE,           de);
    regf(functions,  "hopen",     TYPE_UNARY,    FN_NONE,           ray_hopen);
    regf(functions,  "hclose",    TYPE_UNARY,    FN_NONE,           ray_hclose);
    
    // Binary           
    regf(functions,  "write",     TYPE_BINARY,   FN_NONE,           ray_write);
    regf(functions,  "at",        TYPE_BINARY,   FN_RIGHT_ATOMIC,   ray_at);
    regf(functions,  "==",        TYPE_BINARY,   FN_ATOMIC,         ray_eq);
    regf(functions,  "<",         TYPE_BINARY,   FN_ATOMIC,         ray_lt);
    regf(functions,  ">",         TYPE_BINARY,   FN_ATOMIC,         ray_gt);
    regf(functions,  "<=",        TYPE_BINARY,   FN_ATOMIC,         ray_le);
    regf(functions,  ">=",        TYPE_BINARY,   FN_ATOMIC,         ray_ge);
    regf(functions,  "!=",        TYPE_BINARY,   FN_ATOMIC,         ray_ne);
    regf(functions,  "and",       TYPE_BINARY,   FN_ATOMIC,         ray_and);
    regf(functions,  "or",        TYPE_BINARY,   FN_ATOMIC,         ray_or);
    regf(functions,  "+",         TYPE_BINARY,   FN_ATOMIC,         ray_add);
    regf(functions,  "-",         TYPE_BINARY,   FN_ATOMIC,         ray_sub);
    regf(functions,  "*",         TYPE_BINARY,   FN_ATOMIC,         ray_mul);
    regf(functions,  "%",         TYPE_BINARY,   FN_ATOMIC,         ray_mod);
    regf(functions,  "/",         TYPE_BINARY,   FN_ATOMIC,         ray_div);
    regf(functions,  "div",       TYPE_BINARY,   FN_ATOMIC,         ray_fdiv);
    regf(functions,  "like",      TYPE_BINARY,   FN_NONE,           ray_like);
    regf(functions,  "dict",      TYPE_BINARY,   FN_NONE,           ray_dict);
    regf(functions,  "table",     TYPE_BINARY,   FN_NONE,           ray_table);
    regf(functions,  "find",      TYPE_BINARY,   FN_ATOMIC,         ray_find);
    regf(functions,  "concat",    TYPE_BINARY,   FN_NONE,           ray_concat);
    regf(functions,  "filter",    TYPE_BINARY,   FN_ATOMIC,         ray_filter);
    regf(functions,  "take",      TYPE_BINARY,   FN_NONE,           ray_take);
    regf(functions,  "in",        TYPE_BINARY,   FN_ATOMIC,         ray_in);
    regf(functions,  "sect",      TYPE_BINARY,   FN_ATOMIC,         ray_sect);
    regf(functions,  "except",    TYPE_BINARY,   FN_ATOMIC,         ray_except);
    regf(functions,  "rand",      TYPE_BINARY,   FN_ATOMIC,         ray_rand);
    regf(functions,  "as",        TYPE_BINARY,   FN_NONE,           ray_cast);
    regf(functions,  "xasc",      TYPE_BINARY,   FN_NONE,           ray_xasc);
    regf(functions,  "xdesc",     TYPE_BINARY,   FN_NONE,           ray_xdesc);
    regf(functions,  "enum",      TYPE_BINARY,   FN_NONE,           ray_enum);
    
    // Vary       
    regf(functions,  "env",       TYPE_VARY,     FN_NONE,           ray_env);
    regf(functions,  "memstat",   TYPE_VARY,     FN_NONE,           ray_memstat);
    regf(functions,  "gc",        TYPE_VARY,     FN_NONE,           ray_gc);
    regf(functions,  "list",      TYPE_VARY,     FN_NONE,           ray_list);
    regf(functions,  "enlist",    TYPE_VARY,     FN_NONE,           ray_enlist);
    regf(functions,  "format",    TYPE_VARY,     FN_NONE,           ray_format);
    regf(functions,  "print",     TYPE_VARY,     FN_NONE,           ray_print);
    regf(functions,  "println",   TYPE_VARY,     FN_NONE,           ray_println);
    regf(functions,  "map",       TYPE_VARY,     FN_NONE,           ray_map_vary);
    regf(functions,  "args",      TYPE_VARY,     FN_NONE,           ray_args);
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
    regt(typenames,    TYPE_ENUM,       "Enum");
    regt(typenames,    TYPE_ANYMAP,     "Anymap");
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
    assert(intern_keyword("take",    4)  == KW_TAKE);
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

str_t env_get_internal_name(obj_t obj)
{
    obj_t functions = runtime_get()->env.functions;
    i64_t sym = 0;
    u64_t i, l;

    l = as_list(functions)[1]->len;
    for (i = 0; i < l; i++)
    {
        if (as_list(as_list(functions)[1])[i]->i64 == obj->i64)
        {
            sym = as_symbol(as_list(functions)[0])[i];
            break;
        }
    }

    if (sym)
        return symtostr(sym);

    return "unknown internal function";
}

obj_t env_get_internal_function(str_t name)
{
    i64_t i;

    i = find_sym(as_list(runtime_get()->env.functions)[0], name);

    if (i < (i64_t)as_list(runtime_get()->env.functions)[0]->len)
        return clone(as_list(as_list(runtime_get()->env.functions)[1])[i]);

    return null(0);
}