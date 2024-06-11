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
#include "update.h"
#include "join.h"
#include "query.h"
#include "cond.h"
#include "iter.h"
#include "dynlib.h"
#include "timer.h"
#include "string.h"

i64_t SYMBOL_FN;
i64_t SYMBOL_SELF;
i64_t SYMBOL_DO;
i64_t SYMBOL_SET;
i64_t SYMBOL_LET;

#define regf(r, n, t, f, o)                      \
    {                                            \
        i64_t _k = symbols_intern(n, strlen(n)); \
        push_raw(&as_list(r)[0], &_k);           \
        obj_p _o = atom(-t);                     \
        _o->attrs = f | ATTR_PROTECTED;          \
        _o->i64 = (i64_t)o;                      \
        push_raw(&as_list(r)[1], &_o);           \
    };

#define regt(r, i, s)                  \
    {                                  \
        i64_t _i = i;                  \
        push_raw(&as_list(r)[0], &_i); \
        push_sym(&as_list(r)[1], s);   \
    };

obj_p ray_env(obj_p *x, u64_t n)
{
    unused(x);
    unused(n);
    return clone_obj(runtime_get()->env.variables);
}

obj_p ray_memstat(obj_p *x, u64_t n)
{
    unused(x);
    unused(n);
    obj_p keys, vals;
    memstat_t stat = heap_memstat();
    symbols_p symbols = runtime_get()->symbols;

    keys = vector_symbol(4);
    ins_sym(&keys, 0, "msys");
    ins_sym(&keys, 1, "heap");
    ins_sym(&keys, 2, "free");
    ins_sym(&keys, 3, "syms");

    vals = list(4);
    // as_list(vals)[0] = i64(stat.system + symbols_memsize(symbols));
    as_list(vals)[1] = i64(stat.heap);
    as_list(vals)[2] = i64(stat.free);
    as_list(vals)[3] = i64(symbols_count(symbols));

    return dict(keys, vals);
}

// clang-format off
nil_t init_functions(obj_p functions)
{
    // Unary
    regf(functions,  "get",               TYPE_UNARY,    FN_NONE,                   ray_get);
    regf(functions,  "raise",             TYPE_UNARY,    FN_NONE,                   ray_raise);
    regf(functions,  "read",              TYPE_UNARY,    FN_NONE,                   ray_read);
    regf(functions,  "parse",             TYPE_UNARY,    FN_NONE,                   ray_parse);
    regf(functions,  "eval",              TYPE_UNARY,    FN_NONE,                   ray_eval);
    regf(functions,  "load",              TYPE_UNARY,    FN_NONE,                   ray_load);
    regf(functions,  "type",              TYPE_UNARY,    FN_NONE,                   ray_type);
    regf(functions,  "til",               TYPE_UNARY,    FN_NONE,                   ray_til);
    regf(functions,  "reverse",           TYPE_UNARY,    FN_NONE,                   ray_reverse);
    regf(functions,  "distinct",          TYPE_UNARY,    FN_NONE,                   ray_distinct);
    regf(functions,  "group",             TYPE_UNARY,    FN_NONE,                   ray_group);
    regf(functions,  "sum",               TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_sum);
    regf(functions,  "avg",               TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_avg);
    regf(functions,  "med",               TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_med);
    regf(functions,  "dev",               TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_dev);
    regf(functions,  "min",               TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_min);
    regf(functions,  "max",               TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_max);
    regf(functions,  "round",             TYPE_UNARY,    FN_ATOMIC,                 ray_round);
    regf(functions,  "floor",             TYPE_UNARY,    FN_ATOMIC,                 ray_floor);
    regf(functions,  "ceil",              TYPE_UNARY,    FN_ATOMIC,                 ray_ceil);
    regf(functions,  "first",             TYPE_UNARY,    FN_NONE | FN_AGGR,         ray_first);
    regf(functions,  "last",              TYPE_UNARY,    FN_NONE | FN_AGGR,         ray_last);
    regf(functions,  "count",             TYPE_UNARY,    FN_NONE | FN_AGGR,         ray_count);
    regf(functions,  "not",               TYPE_UNARY,    FN_ATOMIC,                 ray_not);
    regf(functions,  "iasc",              TYPE_UNARY,    FN_ATOMIC,                 ray_iasc);
    regf(functions,  "idesc",             TYPE_UNARY,    FN_ATOMIC,                 ray_idesc);
    regf(functions,  "asc",               TYPE_UNARY,    FN_ATOMIC,                 ray_asc);
    regf(functions,  "desc",              TYPE_UNARY,    FN_ATOMIC,                 ray_desc);
    regf(functions,  "guid",              TYPE_UNARY,    FN_ATOMIC,                 ray_guid);
    regf(functions,  "neg",               TYPE_UNARY,    FN_ATOMIC,                 ray_neg);
    regf(functions,  "where",             TYPE_UNARY,    FN_ATOMIC,                 ray_where);
    regf(functions,  "key",               TYPE_UNARY,    FN_NONE,                   ray_key);
    regf(functions,  "value",             TYPE_UNARY,    FN_NONE,                   ray_value);
    regf(functions,  "parse",             TYPE_UNARY,    FN_NONE,                   ray_parse);
    regf(functions,  "ser",               TYPE_UNARY,    FN_NONE,                   ser_obj);
    regf(functions,  "de",                TYPE_UNARY,    FN_NONE,                   de_obj);
    regf(functions,  "hopen",             TYPE_UNARY,    FN_NONE,                   ray_hopen);
    regf(functions,  "hclose",            TYPE_UNARY,    FN_NONE,                   ray_hclose);
    regf(functions,  "rc",                TYPE_UNARY,    FN_NONE,                   ray_rc);
    regf(functions,  "select",            TYPE_UNARY,    FN_NONE,                   ray_select);
    regf(functions,  "timeit",            TYPE_UNARY,    FN_NONE | FN_SPECIAL_FORM, ray_timeit);
    regf(functions,  "bins",              TYPE_UNARY,    FN_NONE,                   ray_bins);
    regf(functions,  "update",            TYPE_UNARY,    FN_NONE,                   ray_update);
    regf(functions,  "graphic",           TYPE_UNARY,    FN_NONE,                   ray_graphic_format);
    regf(functions,  "set-fpr",           TYPE_UNARY,    FN_NONE,                   ray_set_fpr);
    regf(functions,  "set-display-width", TYPE_UNARY,    FN_NONE,                   ray_set_display_width);
    
    // Binary           
    regf(functions,  "try",               TYPE_BINARY,   FN_NONE | FN_SPECIAL_FORM, try_obj);
    regf(functions,  "set",               TYPE_BINARY,   FN_NONE | FN_SPECIAL_FORM, ray_set);
    regf(functions,  "let",               TYPE_BINARY,   FN_NONE | FN_SPECIAL_FORM, ray_let);
    regf(functions,  "write",             TYPE_BINARY,   FN_NONE,                   ray_write);
    regf(functions,  "at",                TYPE_BINARY,   FN_RIGHT_ATOMIC,           ray_at);
    regf(functions,  "==",                TYPE_BINARY,   FN_ATOMIC,                 ray_eq);
    regf(functions,  "<",                 TYPE_BINARY,   FN_ATOMIC,                 ray_lt);
    regf(functions,  ">",                 TYPE_BINARY,   FN_ATOMIC,                 ray_gt);
    regf(functions,  "<=",                TYPE_BINARY,   FN_ATOMIC,                 ray_le);
    regf(functions,  ">=",                TYPE_BINARY,   FN_ATOMIC,                 ray_ge);
    regf(functions,  "!=",                TYPE_BINARY,   FN_ATOMIC,                 ray_ne);
    regf(functions,  "and",               TYPE_BINARY,   FN_ATOMIC,                 ray_and);
    regf(functions,  "or",                TYPE_BINARY,   FN_ATOMIC,                 ray_or);
    regf(functions,  "+",                 TYPE_BINARY,   FN_ATOMIC,                 ray_add);
    regf(functions,  "-",                 TYPE_BINARY,   FN_ATOMIC,                 ray_sub);
    regf(functions,  "*",                 TYPE_BINARY,   FN_ATOMIC,                 ray_mul);
    regf(functions,  "%",                 TYPE_BINARY,   FN_ATOMIC,                 ray_mod);
    regf(functions,  "/",                 TYPE_BINARY,   FN_ATOMIC,                 ray_div);
    regf(functions,  "div",               TYPE_BINARY,   FN_ATOMIC,                 ray_fdiv);
    regf(functions,  "like",              TYPE_BINARY,   FN_NONE,                   ray_like);
    regf(functions,  "dict",              TYPE_BINARY,   FN_NONE,                   ray_dict);
    regf(functions,  "table",             TYPE_BINARY,   FN_NONE,                   ray_table);
    regf(functions,  "find",              TYPE_BINARY,   FN_NONE,                   ray_find);
    regf(functions,  "concat",            TYPE_BINARY,   FN_NONE,                   ray_concat);
    regf(functions,  "filter",            TYPE_BINARY,   FN_ATOMIC,                 ray_filter);
    regf(functions,  "take",              TYPE_BINARY,   FN_NONE,                   ray_take);
    regf(functions,  "in",                TYPE_BINARY,   FN_ATOMIC,                 ray_in);
    regf(functions,  "sect",              TYPE_BINARY,   FN_ATOMIC,                 ray_sect);
    regf(functions,  "except",            TYPE_BINARY,   FN_ATOMIC,                 ray_except);
    regf(functions,  "rand",              TYPE_BINARY,   FN_ATOMIC,                 ray_rand);
    regf(functions,  "as",                TYPE_BINARY,   FN_NONE,                   ray_cast_obj);
    regf(functions,  "xasc",              TYPE_BINARY,   FN_NONE,                   ray_xasc);
    regf(functions,  "xdesc",             TYPE_BINARY,   FN_NONE,                   ray_xdesc);
    regf(functions,  "enum",              TYPE_BINARY,   FN_NONE,                   ray_enum);
    regf(functions,  "xbar",              TYPE_BINARY,   FN_ATOMIC,                 ray_xbar);
        
    // Vary               
    regf(functions,  "do",                TYPE_VARY,     FN_NONE | FN_SPECIAL_FORM, ray_do);
    regf(functions,  "env",               TYPE_VARY,     FN_NONE,                   ray_env);
    regf(functions,  "memstat",           TYPE_VARY,     FN_NONE,                   ray_memstat);
    regf(functions,  "gc",                TYPE_VARY,     FN_NONE,                   ray_gc);
    regf(functions,  "list",              TYPE_VARY,     FN_NONE,                   ray_list);
    regf(functions,  "enlist",            TYPE_VARY,     FN_NONE,                   ray_enlist);
    regf(functions,  "format",            TYPE_VARY,     FN_NONE,                   ray_format);
    regf(functions,  "print",             TYPE_VARY,     FN_NONE,                   ray_print);
    regf(functions,  "println",           TYPE_VARY,     FN_NONE,                   ray_println);
    regf(functions,  "apply",             TYPE_VARY,     FN_NONE,                   ray_apply);
    regf(functions,  "map",               TYPE_VARY,     FN_NONE,                   ray_map);
    regf(functions,  "fold",              TYPE_VARY,     FN_NONE,                   ray_fold);
    regf(functions,  "args",              TYPE_VARY,     FN_NONE,                   ray_args);
    regf(functions,  "alter",             TYPE_VARY,     FN_NONE,                   ray_alter);
    regf(functions,  "modify",            TYPE_VARY,     FN_NONE,                   ray_modify);
    regf(functions,  "insert",            TYPE_VARY,     FN_NONE,                   ray_insert);
    regf(functions,  "upsert",            TYPE_VARY,     FN_NONE,                   ray_upsert);
    regf(functions,  "read-csv",          TYPE_VARY,     FN_NONE,                   ray_read_csv);
    regf(functions,  "lj",                TYPE_VARY,     FN_NONE,                   ray_lj);
    regf(functions,  "ij",                TYPE_VARY,     FN_NONE,                   ray_ij);
    regf(functions,  "if",                TYPE_VARY,     FN_NONE | FN_SPECIAL_FORM, ray_cond);
    regf(functions,  "return",            TYPE_VARY,     FN_NONE,                   ray_return);
    regf(functions,  "exit",              TYPE_VARY,     FN_NONE,                   ray_exit);
    regf(functions,  "loadfn",            TYPE_VARY,     FN_NONE,                   ray_loadfn);
    regf(functions,  "timer",             TYPE_VARY,     FN_NONE,                   ray_timer);
}    
    
nil_t init_typenames(obj_p typenames)    
{
    regt(typenames,   -TYPE_ERROR,     "Null");
    regt(typenames,   -TYPE_B8,        "b8");
    regt(typenames,   -TYPE_U8,        "byte");
    regt(typenames,   -TYPE_I64,       "i64");
    regt(typenames,   -TYPE_F64,       "f64");
    regt(typenames,   -TYPE_C8,        "char");
    regt(typenames,   -TYPE_SYMBOL,    "symbol");
    regt(typenames,   -TYPE_TIMESTAMP, "timestamp");
    regt(typenames,   -TYPE_GUID,      "guid");
    regt(typenames,    TYPE_B8,        "B8");
    regt(typenames,    TYPE_U8,        "Byte");
    regt(typenames,    TYPE_I64,       "I64");
    regt(typenames,    TYPE_F64,       "F64");
    regt(typenames,    TYPE_C8,        "String");
    regt(typenames,    TYPE_ENUM,      "Enum");
    regt(typenames,    TYPE_ANYMAP,    "Anymap");
    regt(typenames,    TYPE_SYMBOL,    "Symbol");
    regt(typenames,    TYPE_TIMESTAMP, "Timestamp");
    regt(typenames,    TYPE_GUID,      "Guid");
    regt(typenames,    TYPE_LIST,      "List");
    regt(typenames,    TYPE_TABLE,     "Table");
    regt(typenames,    TYPE_DICT,      "Dict");
    regt(typenames,    TYPE_UNARY,     "Unary");
    regt(typenames,    TYPE_BINARY,    "Binary");
    regt(typenames,    TYPE_VARY,      "Vary");
    regt(typenames,    TYPE_LAMBDA,    "Lambda");
    regt(typenames,    TYPE_FILTERMAP, "Filtermap");
    regt(typenames,    TYPE_GROUPMAP,  "Groupmap");
    regt(typenames,    TYPE_NULL,      "Null");
    regt(typenames,    TYPE_ERROR,     "Error");
}
// clang-format on

nil_t init_kw(nil_t)
{
    SYMBOL_FN = symbols_intern("fn", 2);
    SYMBOL_SELF = symbols_intern("self", 4);
    SYMBOL_DO = symbols_intern("do", 2);
    SYMBOL_SET = symbols_intern("set", 3);
    SYMBOL_LET = symbols_intern("let", 3);
}

env_t create_env(nil_t)
{
    obj_p functions = dict(vector_symbol(0), list(0));
    obj_p variables = dict(vector_symbol(0), list(0));
    obj_p typenames = dict(vector_i64(0), vector_symbol(0));

    init_kw();
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
    drop_obj(env->functions);
    drop_obj(env->variables);
    drop_obj(env->typenames);
}

i64_t env_get_typename_by_type(env_t *env, i8_t type)
{
    i64_t t, i;

    t = type;
    i = find_raw(as_list(env->typenames)[0], &t);

    if (i == (i64_t)as_list(env->typenames)[0]->len)
        return as_symbol(as_list(env->typenames)[1])[0];

    return as_symbol(as_list(env->typenames)[1])[i];
}

i8_t env_get_type_by_type_name(env_t *env, i64_t name)
{
    i64_t n, i;

    n = name;
    i = find_raw(as_list(env->typenames)[1], &n);

    if (i == (i64_t)as_list(env->typenames)[1]->len)
        return TYPE_ERROR;

    return (i8_t)as_i64(as_list(env->typenames)[0])[i];
}

str_p env_get_type_name(i8_t type)
{
    env_t *env = &runtime_get()->env;
    i64_t name = env_get_typename_by_type(env, type);

    return str_from_symbol(name);
}

str_p env_get_internal_name(obj_p obj)
{
    obj_p functions = runtime_get()->env.functions;
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
        return str_from_symbol(sym);

    return (str_p) "@fn";
}

obj_p env_get_internal_function(lit_p name)
{
    i64_t i;

    i = find_sym(as_list(runtime_get()->env.functions)[0], name);

    if (i < (i64_t)as_list(runtime_get()->env.functions)[0]->len)
        return clone_obj(as_list(as_list(runtime_get()->env.functions)[1])[i]);

    return NULL_OBJ;
}

obj_p env_get_internal_function_by_id(i64_t id)
{
    i64_t i;

    i = find_raw(as_list(runtime_get()->env.functions)[0], &id);

    if (i < (i64_t)as_list(runtime_get()->env.functions)[0]->len)
        return clone_obj(as_list(as_list(runtime_get()->env.functions)[1])[i]);

    return NULL_OBJ;
}

str_p env_get_internal_function_lit(lit_p name, u64_t len, u64_t *start, b8_t exact)
{
    i64_t i, l, *names;
    u64_t n;
    str_p nm;

    l = as_list(runtime_get()->env.functions)[0]->len;
    names = as_i64(as_list(runtime_get()->env.functions)[0]);

    if (exact)
    {
        for (i = 0; i < l; i++)
        {
            nm = str_from_symbol(names[i]);
            n = strlen(nm);
            if (n == len && strncmp(name, nm, len) == 0)
                return nm;
        }
    }
    else
    {
        for (i = *start; i < l; i++)
        {
            nm = str_from_symbol(names[i]);
            n = mini64((i64_t)len, (i64_t)strlen(nm));
            if (strncmp(name, nm, n) == 0)
            {
                *start = i + 1;
                return nm;
            }
        }
    }

    return NULL;
}

str_p env_get_internal_kw_lit(lit_p name, u64_t len, b8_t exact)
{
    if (exact)
    {
        if (len == 2 && strncmp(name, "fn", 2) == 0)
            return (str_p) "fn";
        if (len == 4 && strncmp(name, "self", 4) == 0)
            return (str_p) "self";
        if (len == 2 && strncmp(name, "do", 2) == 0)
            return (str_p) "do";
        if (len == 3 && strncmp(name, "set", 3) == 0)
            return (str_p) "set";
        if (len == 3 && strncmp(name, "let", 3) == 0)
            return (str_p) "let";
    }
    else
    {
        if (strncmp(name, "fn", mini64((i64_t)len, 2)) == 0)
            return (str_p) "fn";
        if (strncmp(name, "self", mini64((i64_t)len, 4)) == 0)
            return (str_p) "self";
        if (strncmp(name, "do", mini64((i64_t)len, 2)) == 0)
            return (str_p) "do";
        if (strncmp(name, "set", mini64((i64_t)len, 3)) == 0)
            return (str_p) "set";
        if (strncmp(name, "let", mini64((i64_t)len, 3)) == 0)
            return (str_p) "let";
    }

    return NULL;
}

str_p env_get_internal_lit_lit(lit_p name, u64_t len, b8_t exact)
{
    if (exact)
    {
        if (len == 2 && strncmp(name, "0i", 2) == 0)
            return (str_p) "0i";
        if (len == 2 && strncmp(name, "0f", 2) == 0)
            return (str_p) "0f";
        if (len == 2 && strncmp(name, "by", 2) == 0)
            return (str_p) "by";
        if (len == 4 && strncmp(name, "from", 4) == 0)
            return (str_p) "from";
        if (len == 4 && strncmp(name, "true", 4) == 0)
            return (str_p) "true";
        if (len == 5 && strncmp(name, "false", 5) == 0)
            return (str_p) "false";
    }
    else
    {
        if (strncmp(name, "0i", mini64((i64_t)len, 2)) == 0)
            return (str_p) "0i";
        if (strncmp(name, "0f", mini64((i64_t)len, 2)) == 0)
            return (str_p) "0f";
        if (strncmp(name, "by", mini64((i64_t)len, 2)) == 0)
            return (str_p) "by";
        if (strncmp(name, "from", mini64((i64_t)len, 4)) == 0)
            return (str_p) "from";
        if (strncmp(name, "true", mini64((i64_t)len, 4)) == 0)
            return (str_p) "true";
        if (strncmp(name, "false", mini64((i64_t)len, 5)) == 0)
            return (str_p) "false";
    }

    return NULL;
}