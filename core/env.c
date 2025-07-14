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
#include <stdarg.h>
#include "binary.h"
#include "cmp.h"
#include "compose.h"
#include "cond.h"
#include "dynlib.h"
#include "format.h"
#include "guid.h"
#include "io.h"
#include "items.h"
#include "iter.h"
#include "join.h"
#include "logic.h"
#include "math.h"
#include "misc.h"
#include "ops.h"
#include "order.h"
#include "query.h"
#include "runtime.h"
#include "serde.h"
#include "string.h"
#include "chrono.h"
#include "date.h"
#include "time.h"
#include "timestamp.h"
#include "unary.h"
#include "update.h"
#include "util.h"
#include "vary.h"
#include "os.h"
#include "proc.h"

i64_t SYMBOL_FN;
i64_t SYMBOL_SELF;
i64_t SYMBOL_DO;
i64_t SYMBOL_SET;
i64_t SYMBOL_LET;
i64_t SYMBOL_TAKE;
i64_t SYMBOL_BY;
i64_t SYMBOL_FROM;
i64_t SYMBOL_WHERE;
i64_t SYMBOL_SYM;

#define REGISTER_FN(r, n, t, f, o)               \
    {                                            \
        i64_t _k = symbols_intern(n, strlen(n)); \
        push_raw(&AS_LIST(r)[0], &_k);           \
        obj_p _o = atom(-t);                     \
        _o->attrs = f | ATTR_PROTECTED;          \
        _o->i64 = (i64_t)o;                      \
        push_raw(&AS_LIST(r)[1], &_o);           \
    };

#define REGISTER_TYPE(r, i, s)         \
    {                                  \
        i64_t _i = i;                  \
        push_raw(&AS_LIST(r)[0], &_i); \
        push_sym(&AS_LIST(r)[1], s);   \
    };

#define REGISTER_INTERNAL(r, n, o)               \
    {                                            \
        i64_t _k = symbols_intern(n, strlen(n)); \
        obj_p _v = (o);                          \
        push_raw(&AS_LIST(r)[0], &_k);           \
        push_raw(&AS_LIST(r)[1], &_v);           \
    };

obj_p ray_env(obj_p *x, i64_t n) {
    UNUSED(x);
    UNUSED(n);
    return clone_obj(runtime_get()->env.variables);
}

obj_p ray_memstat(obj_p *x, i64_t n) {
    UNUSED(x);
    UNUSED(n);
    obj_p keys, vals;
    memstat_t stat = heap_memstat();
    symbols_p symbols = runtime_get()->symbols;

    keys = SYMBOL(4);
    ins_sym(&keys, 0, "msys");
    ins_sym(&keys, 1, "heap");
    ins_sym(&keys, 2, "free");
    ins_sym(&keys, 3, "syms");

    vals = LIST(4);
    AS_LIST(vals)[0] = i64(stat.system);
    AS_LIST(vals)[1] = i64(stat.heap);
    AS_LIST(vals)[2] = i64(stat.free);
    AS_LIST(vals)[3] = i64(symbols_count(symbols));

    return dict(keys, vals);
}

// clang-format off
nil_t init_functions(obj_p functions)
{
    // Unary
    REGISTER_FN(functions,  "get",                 TYPE_UNARY,    FN_NONE,                   ray_get);
    REGISTER_FN(functions,  "quote",               TYPE_UNARY,    FN_NONE | FN_SPECIAL_FORM, ray_quote);
    REGISTER_FN(functions,  "raise",               TYPE_UNARY,    FN_NONE,                   ray_raise);
    REGISTER_FN(functions,  "read",                TYPE_UNARY,    FN_NONE,                   ray_read);
    REGISTER_FN(functions,  "parse",               TYPE_UNARY,    FN_NONE,                   ray_parse);
    REGISTER_FN(functions,  "eval",                TYPE_UNARY,    FN_NONE,                   ray_eval);
    REGISTER_FN(functions,  "load",                TYPE_UNARY,    FN_NONE,                   ray_load);
    REGISTER_FN(functions,  "type",                TYPE_UNARY,    FN_NONE,                   ray_type);
    REGISTER_FN(functions,  "til",                 TYPE_UNARY,    FN_NONE,                   ray_til);
    REGISTER_FN(functions,  "reverse",             TYPE_UNARY,    FN_NONE,                   ray_reverse);
    REGISTER_FN(functions,  "distinct",            TYPE_UNARY,    FN_NONE,                   ray_distinct);
    REGISTER_FN(functions,  "group",               TYPE_UNARY,    FN_NONE,                   ray_group);
    REGISTER_FN(functions,  "sum",                 TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_sum);
    REGISTER_FN(functions,  "avg",                 TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_avg);
    REGISTER_FN(functions,  "med",                 TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_med);
    REGISTER_FN(functions,  "dev",                 TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_dev);
    REGISTER_FN(functions,  "min",                 TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_min);
    REGISTER_FN(functions,  "max",                 TYPE_UNARY,    FN_ATOMIC | FN_AGGR,       ray_max);
    REGISTER_FN(functions,  "round",               TYPE_UNARY,    FN_ATOMIC,                 ray_round);
    REGISTER_FN(functions,  "floor",               TYPE_UNARY,    FN_ATOMIC,                 ray_floor);
    REGISTER_FN(functions,  "ceil",                TYPE_UNARY,    FN_ATOMIC,                 ray_ceil);
    REGISTER_FN(functions,  "first",               TYPE_UNARY,    FN_NONE | FN_AGGR,         ray_first);
    REGISTER_FN(functions,  "last",                TYPE_UNARY,    FN_NONE | FN_AGGR,         ray_last);
    REGISTER_FN(functions,  "count",               TYPE_UNARY,    FN_NONE | FN_AGGR,         ray_count);
    REGISTER_FN(functions,  "not",                 TYPE_UNARY,    FN_ATOMIC,                 ray_not);
    REGISTER_FN(functions,  "iasc",                TYPE_UNARY,    FN_NONE,                   ray_iasc);
    REGISTER_FN(functions,  "idesc",               TYPE_UNARY,    FN_NONE,                   ray_idesc);
    REGISTER_FN(functions,  "asc",                 TYPE_UNARY,    FN_NONE,                   ray_asc);
    REGISTER_FN(functions,  "desc",                TYPE_UNARY,    FN_NONE,                   ray_desc);
    REGISTER_FN(functions,  "guid",                TYPE_UNARY,    FN_ATOMIC,                 ray_guid);
    REGISTER_FN(functions,  "neg",                 TYPE_UNARY,    FN_ATOMIC,                 ray_neg);
    REGISTER_FN(functions,  "where",               TYPE_UNARY,    FN_ATOMIC,                 ray_where);
    REGISTER_FN(functions,  "key",                 TYPE_UNARY,    FN_NONE,                   ray_key);
    REGISTER_FN(functions,  "value",               TYPE_UNARY,    FN_NONE,                   ray_value);
    REGISTER_FN(functions,  "parse",               TYPE_UNARY,    FN_NONE,                   ray_parse);
    REGISTER_FN(functions,  "ser",                 TYPE_UNARY,    FN_NONE,                   ser_obj);
    REGISTER_FN(functions,  "de",                  TYPE_UNARY,    FN_NONE,                   de_obj);
    REGISTER_FN(functions,  "hclose",              TYPE_UNARY,    FN_NONE,                   ray_hclose);
    REGISTER_FN(functions,  "rc",                  TYPE_UNARY,    FN_NONE,                   ray_rc);
    REGISTER_FN(functions,  "select",              TYPE_UNARY,    FN_NONE,                   ray_select);
    REGISTER_FN(functions,  "update",              TYPE_UNARY,    FN_NONE,                   ray_update);
    REGISTER_FN(functions,  "date",                TYPE_UNARY,    FN_NONE,                   ray_date);
    REGISTER_FN(functions,  "time",                TYPE_UNARY,    FN_NONE,                   ray_time);
    REGISTER_FN(functions,  "timestamp",           TYPE_UNARY,    FN_NONE,                   ray_timestamp);
    REGISTER_FN(functions,  "nil?",                TYPE_UNARY,    FN_NONE,                   ray_is_null);
    REGISTER_FN(functions,  "resolve",             TYPE_UNARY,    FN_NONE,                   ray_resolve);
    REGISTER_FN(functions,  "show",                TYPE_UNARY,    FN_NONE,                   ray_show);
    REGISTER_FN(functions,  "meta",                TYPE_UNARY,    FN_NONE,                   ray_meta);
    REGISTER_FN(functions,  "os-get-var",          TYPE_UNARY,    FN_NONE,                   ray_os_get_var);
    REGISTER_FN(functions,  "system",              TYPE_UNARY,    FN_NONE,                   ray_system);
    REGISTER_FN(functions,  "unify",               TYPE_UNARY,    FN_NONE,                   ray_unify);
    REGISTER_FN(functions,  "diverse",             TYPE_UNARY,    FN_NONE,                   ray_diverse);
    
    // Binary           
    REGISTER_FN(functions,  "try",                 TYPE_BINARY,   FN_NONE | FN_SPECIAL_FORM, try_obj);
    REGISTER_FN(functions,  "set",                 TYPE_BINARY,   FN_NONE | FN_SPECIAL_FORM, ray_set);
    REGISTER_FN(functions,  "let",                 TYPE_BINARY,   FN_NONE | FN_SPECIAL_FORM, ray_let);
    REGISTER_FN(functions,  "write",               TYPE_BINARY,   FN_NONE,                   ray_write);
    REGISTER_FN(functions,  "at",                  TYPE_BINARY,   FN_RIGHT_ATOMIC,           ray_at);
    REGISTER_FN(functions,  "==",                  TYPE_BINARY,   FN_ATOMIC,                 ray_eq);
    REGISTER_FN(functions,  "<",                   TYPE_BINARY,   FN_ATOMIC,                 ray_lt);
    REGISTER_FN(functions,  ">",                   TYPE_BINARY,   FN_ATOMIC,                 ray_gt);
    REGISTER_FN(functions,  "<=",                  TYPE_BINARY,   FN_ATOMIC,                 ray_le);
    REGISTER_FN(functions,  ">=",                  TYPE_BINARY,   FN_ATOMIC,                 ray_ge);
    REGISTER_FN(functions,  "!=",                  TYPE_BINARY,   FN_ATOMIC,                 ray_ne);
    REGISTER_FN(functions,  "+",                   TYPE_BINARY,   FN_ATOMIC,                 ray_add);
    REGISTER_FN(functions,  "-",                   TYPE_BINARY,   FN_ATOMIC,                 ray_sub);
    REGISTER_FN(functions,  "*",                   TYPE_BINARY,   FN_ATOMIC,                 ray_mul);
    REGISTER_FN(functions,  "%",                   TYPE_BINARY,   FN_ATOMIC,                 ray_mod);
    REGISTER_FN(functions,  "/",                   TYPE_BINARY,   FN_ATOMIC,                 ray_div);
    REGISTER_FN(functions,  "div",                 TYPE_BINARY,   FN_ATOMIC,                 ray_fdiv);
    REGISTER_FN(functions,  "like",                TYPE_BINARY,   FN_NONE,                   ray_like);
    REGISTER_FN(functions,  "dict",                TYPE_BINARY,   FN_NONE,                   ray_dict);
    REGISTER_FN(functions,  "table",               TYPE_BINARY,   FN_NONE,                   ray_table);
    REGISTER_FN(functions,  "find",                TYPE_BINARY,   FN_NONE,                   ray_find);
    REGISTER_FN(functions,  "concat",              TYPE_BINARY,   FN_NONE,                   ray_concat);
    REGISTER_FN(functions,  "remove",              TYPE_BINARY,   FN_NONE,                   ray_remove);
    REGISTER_FN(functions,  "filter",              TYPE_BINARY,   FN_NONE,                   ray_filter);
    REGISTER_FN(functions,  "take",                TYPE_BINARY,   FN_NONE,                   ray_take);
    REGISTER_FN(functions,  "in",                  TYPE_BINARY,   FN_NONE,                   ray_in);
    REGISTER_FN(functions,  "within",              TYPE_BINARY,   FN_NONE,                   ray_within);
    REGISTER_FN(functions,  "sect",                TYPE_BINARY,   FN_ATOMIC,                 ray_sect);
    REGISTER_FN(functions,  "except",              TYPE_BINARY,   FN_NONE,                   ray_except);
    REGISTER_FN(functions,  "union",               TYPE_BINARY,   FN_NONE,                   ray_union);
    REGISTER_FN(functions,  "rand",                TYPE_BINARY,   FN_ATOMIC,                 ray_rand);
    REGISTER_FN(functions,  "as",                  TYPE_BINARY,   FN_NONE,                   ray_cast_obj);
    REGISTER_FN(functions,  "xasc",                TYPE_BINARY,   FN_NONE,                   ray_xasc);
    REGISTER_FN(functions,  "xdesc",               TYPE_BINARY,   FN_NONE,                   ray_xdesc);
    REGISTER_FN(functions,  "enum",                TYPE_BINARY,   FN_NONE,                   ray_enum);
    REGISTER_FN(functions,  "xbar",                TYPE_BINARY,   FN_ATOMIC,                 ray_xbar);
    REGISTER_FN(functions,  "os-set-var",          TYPE_BINARY,   FN_ATOMIC,                 ray_os_set_var);
    REGISTER_FN(functions,  "split",               TYPE_BINARY,   FN_NONE,                   ray_split);

    // Vary               
    REGISTER_FN(functions,  "do",                  TYPE_VARY,     FN_NONE | FN_SPECIAL_FORM, ray_do);
    REGISTER_FN(functions,  "and",                 TYPE_VARY,     FN_NONE | FN_SPECIAL_FORM, ray_and);
    REGISTER_FN(functions,  "or",                  TYPE_VARY,     FN_NONE | FN_SPECIAL_FORM, ray_or);
    REGISTER_FN(functions,  "env",                 TYPE_VARY,     FN_NONE,                   ray_env);
    REGISTER_FN(functions,  "timeit",              TYPE_VARY,     FN_NONE | FN_SPECIAL_FORM, ray_timeit);
    REGISTER_FN(functions,  "memstat",             TYPE_VARY,     FN_NONE,                   ray_memstat);
    REGISTER_FN(functions,  "gc",                  TYPE_VARY,     FN_NONE,                   ray_gc);
    REGISTER_FN(functions,  "list",                TYPE_VARY,     FN_NONE,                   ray_list);
    REGISTER_FN(functions,  "enlist",              TYPE_VARY,     FN_NONE,                   ray_enlist);
    REGISTER_FN(functions,  "format",              TYPE_VARY,     FN_NONE,                   ray_format);
    REGISTER_FN(functions,  "print",               TYPE_VARY,     FN_NONE,                   ray_print);
    REGISTER_FN(functions,  "println",             TYPE_VARY,     FN_NONE,                   ray_println);
    REGISTER_FN(functions,  "apply",               TYPE_VARY,     FN_NONE,                   ray_apply);
    REGISTER_FN(functions,  "map",                 TYPE_VARY,     FN_NONE,                   ray_map);
    REGISTER_FN(functions,  "map-left",            TYPE_VARY,     FN_NONE,                   ray_map_left);
    REGISTER_FN(functions,  "map-right",           TYPE_VARY,     FN_NONE,                   ray_map_right);
    REGISTER_FN(functions,  "fold",                TYPE_VARY,     FN_NONE,                   ray_fold);
    REGISTER_FN(functions,  "fold-left",           TYPE_VARY,     FN_NONE,                   ray_fold_left);
    REGISTER_FN(functions,  "fold-right",          TYPE_VARY,     FN_NONE,                   ray_fold_right);
    REGISTER_FN(functions,  "scan",                TYPE_VARY,     FN_NONE,                   ray_scan);
    REGISTER_FN(functions,  "scan-left",           TYPE_VARY,     FN_NONE,                   ray_scan_left);
    REGISTER_FN(functions,  "scan-right",          TYPE_VARY,     FN_NONE,                   ray_scan_right);
    REGISTER_FN(functions,  "args",                TYPE_VARY,     FN_NONE,                   ray_args);
    REGISTER_FN(functions,  "alter",               TYPE_VARY,     FN_NONE,                   ray_alter);
    REGISTER_FN(functions,  "modify",              TYPE_VARY,     FN_NONE,                   ray_modify);
    REGISTER_FN(functions,  "insert",              TYPE_VARY,     FN_NONE,                   ray_insert);
    REGISTER_FN(functions,  "upsert",              TYPE_VARY,     FN_NONE,                   ray_upsert);
    REGISTER_FN(functions,  "read-csv",            TYPE_VARY,     FN_NONE,                   ray_read_csv);
    REGISTER_FN(functions,  "left-join",           TYPE_VARY,     FN_NONE,                   ray_left_join);
    REGISTER_FN(functions,  "inner-join",          TYPE_VARY,     FN_NONE,                   ray_inner_join);
    REGISTER_FN(functions,  "if",                  TYPE_VARY,     FN_NONE | FN_SPECIAL_FORM, ray_cond);
    REGISTER_FN(functions,  "return",              TYPE_VARY,     FN_NONE,                   ray_return);
    REGISTER_FN(functions,  "hopen",               TYPE_VARY,     FN_NONE,                   ray_hopen);
    REGISTER_FN(functions,  "exit",                TYPE_VARY,     FN_NONE,                   ray_exit);
    REGISTER_FN(functions,  "loadfn",              TYPE_VARY,     FN_NONE,                   ray_loadfn);
    REGISTER_FN(functions,  "timer",               TYPE_VARY,     FN_NONE,                   ray_timer);
    REGISTER_FN(functions,  "set-splayed",         TYPE_VARY,     FN_NONE,                   ray_set_splayed);
    REGISTER_FN(functions,  "get-splayed",         TYPE_VARY,     FN_NONE,                   ray_get_splayed);
    REGISTER_FN(functions,  "set-parted",          TYPE_VARY,     FN_NONE,                   ray_set_parted);
    REGISTER_FN(functions,  "get-parted",          TYPE_VARY,     FN_NONE,                   ray_get_parted);
    REGISTER_FN(functions,  "internals",           TYPE_VARY,     FN_NONE,                   ray_internals);
    REGISTER_FN(functions,  "row-index",           TYPE_VARY,     FN_NONE,                   ray_row_index);
}    
    
nil_t init_typenames(obj_p typenames)    
{
    REGISTER_TYPE(typenames,   -TYPE_ERR,           "Null");
    REGISTER_TYPE(typenames,   -TYPE_B8,              "b8");
    REGISTER_TYPE(typenames,   -TYPE_U8,              "u8");
    REGISTER_TYPE(typenames,   -TYPE_I16,             "i16");
    REGISTER_TYPE(typenames,   -TYPE_I32,             "i32");
    REGISTER_TYPE(typenames,   -TYPE_I64,             "i64");
    REGISTER_TYPE(typenames,   -TYPE_F64,             "f64");
    REGISTER_TYPE(typenames,   -TYPE_C8,              "char");
    REGISTER_TYPE(typenames,   -TYPE_SYMBOL,          "symbol");
    REGISTER_TYPE(typenames,   -TYPE_DATE,            "date");
    REGISTER_TYPE(typenames,   -TYPE_TIME,            "time");
    REGISTER_TYPE(typenames,   -TYPE_TIMESTAMP,       "timestamp");
    REGISTER_TYPE(typenames,   -TYPE_GUID,            "guid");
    REGISTER_TYPE(typenames,    TYPE_B8,              "B8");
    REGISTER_TYPE(typenames,    TYPE_U8,              "U8");
    REGISTER_TYPE(typenames,    TYPE_I16,             "I16");
    REGISTER_TYPE(typenames,    TYPE_I32,             "I32");
    REGISTER_TYPE(typenames,    TYPE_I64,             "I64");
    REGISTER_TYPE(typenames,    TYPE_F64,             "F64");
    REGISTER_TYPE(typenames,    TYPE_C8,              "String");
    REGISTER_TYPE(typenames,    TYPE_ENUM,            "Enum");
    REGISTER_TYPE(typenames,    TYPE_PARTEDLIST,      "Partedlist");
    REGISTER_TYPE(typenames,    TYPE_PARTEDB8,        "Partedb8");
    REGISTER_TYPE(typenames,    TYPE_PARTEDU8,        "Partedu8");
    REGISTER_TYPE(typenames,    TYPE_PARTEDI64,       "Partedi64");
    REGISTER_TYPE(typenames,    TYPE_PARTEDF64,       "Partedf64");
    REGISTER_TYPE(typenames,    TYPE_PARTEDTIMESTAMP, "Partedtimestamp");
    REGISTER_TYPE(typenames,    TYPE_PARTEDGUID,      "Partedguid");
    REGISTER_TYPE(typenames,    TYPE_PARTEDENUM,      "Partedenum");
    REGISTER_TYPE(typenames,    TYPE_MAPLIST,         "Maplist");
    REGISTER_TYPE(typenames,    TYPE_MAPFILTER,       "Mapfilter");
    REGISTER_TYPE(typenames,    TYPE_MAPGROUP,        "Mapgroup");
    REGISTER_TYPE(typenames,    TYPE_MAPFD,           "Mapfd");
    REGISTER_TYPE(typenames,    TYPE_MAPCOMMON,       "Mapcommon");
    REGISTER_TYPE(typenames,    TYPE_SYMBOL,          "Symbol");
    REGISTER_TYPE(typenames,    TYPE_DATE,            "Date");
    REGISTER_TYPE(typenames,    TYPE_TIME,            "Time");
    REGISTER_TYPE(typenames,    TYPE_TIMESTAMP,       "Timestamp");
    REGISTER_TYPE(typenames,    TYPE_GUID,            "Guid");
    REGISTER_TYPE(typenames,    TYPE_LIST,            "List");
    REGISTER_TYPE(typenames,    TYPE_TABLE,           "Table");
    REGISTER_TYPE(typenames,    TYPE_DICT,            "Dict");
    REGISTER_TYPE(typenames,    TYPE_UNARY,           "Unary");
    REGISTER_TYPE(typenames,    TYPE_BINARY,          "Binary");
    REGISTER_TYPE(typenames,    TYPE_VARY,            "Vary");
    REGISTER_TYPE(typenames,    TYPE_LAMBDA,          "Lambda");
    REGISTER_TYPE(typenames,    TYPE_NULL,            "Null");
    REGISTER_TYPE(typenames,    TYPE_ERR,           "Error");
}
// clang-format on

nil_t init_internals(obj_p internals) {
    REGISTER_INTERNAL(internals, "pid", i64(proc_get_pid()));
    // REGISTER_INTERNAL(internals, "started", timestamp(timestamp_into_i64(timestamp_current("local"))));
}

nil_t init_keywords(obj_p *keywords) {
    SYMBOL_FN = symbols_intern("fn", 2);
    push_raw(keywords, &SYMBOL_FN);
    SYMBOL_DO = symbols_intern("do", 2);
    push_raw(keywords, &SYMBOL_DO);
    SYMBOL_SET = symbols_intern("set", 3);
    push_raw(keywords, &SYMBOL_SET);
    SYMBOL_SELF = symbols_intern("self", 4);
    push_raw(keywords, &SYMBOL_SELF);
    SYMBOL_LET = symbols_intern("let", 3);
    push_raw(keywords, &SYMBOL_LET);
    SYMBOL_TAKE = symbols_intern("take", 4);
    push_raw(keywords, &SYMBOL_TAKE);
    SYMBOL_BY = symbols_intern("by", 2);
    push_raw(keywords, &SYMBOL_BY);
    SYMBOL_FROM = symbols_intern("from", 4);
    push_raw(keywords, &SYMBOL_FROM);
    SYMBOL_WHERE = symbols_intern("where", 5);
    push_raw(keywords, &SYMBOL_WHERE);
    SYMBOL_SYM = symbols_intern("sym", 3);
    push_raw(keywords, &SYMBOL_SYM);
}

env_t env_create(nil_t) {
    obj_p keywords = SYMBOL(0);
    obj_p functions = dict(SYMBOL(0), LIST(0));
    obj_p variables = dict(SYMBOL(0), LIST(0));
    obj_p typenames = dict(I64(0), SYMBOL(0));
    obj_p internals = dict(SYMBOL(0), LIST(0));

    init_keywords(&keywords);
    init_functions(functions);
    init_typenames(typenames);
    init_internals(internals);

    env_t env = {
        .keywords = keywords,
        .functions = functions,
        .variables = variables,
        .typenames = typenames,
        .internals = internals,
    };

    return env;
}

nil_t env_destroy(env_t *env) {
    drop_obj(env->keywords);
    drop_obj(env->functions);
    drop_obj(env->variables);
    drop_obj(env->typenames);
    drop_obj(env->internals);
}

i64_t env_get_typename_by_type(env_t *env, i8_t type) {
    i64_t t, i;

    t = type;
    i = find_raw(AS_LIST(env->typenames)[0], &t);

    if (i == NULL_I64)
        return AS_SYMBOL(AS_LIST(env->typenames)[1])[0];

    return AS_SYMBOL(AS_LIST(env->typenames)[1])[i];
}

i8_t env_get_type_by_type_name(env_t *env, i64_t name) {
    i64_t n, i;

    n = name;
    i = find_raw(AS_LIST(env->typenames)[1], &n);

    if (i == NULL_I64)
        return TYPE_ERR;

    return (i8_t)AS_I64(AS_LIST(env->typenames)[0])[i];
}

str_p env_get_type_name(i8_t type) {
    env_t *env = &runtime_get()->env;
    i64_t name = env_get_typename_by_type(env, type);

    return str_from_symbol(name);
}

i64_t env_get_internal_id(obj_p obj) {
    obj_p functions = runtime_get()->env.functions;
    i64_t i, l, s = NULL_I64;

    l = AS_LIST(functions)[1]->len;
    for (i = 0; i < l; i++) {
        if (AS_LIST(AS_LIST(functions)[1])[i]->i64 == obj->i64) {
            s = AS_SYMBOL(AS_LIST(functions)[0])[i];
            break;
        }
    }

    return s;
}

str_p env_get_internal_name(obj_p obj) {
    obj_p functions = runtime_get()->env.functions;
    i64_t sym = 0;
    i64_t i, l;

    l = AS_LIST(functions)[1]->len;
    for (i = 0; i < l; i++) {
        if (AS_LIST(AS_LIST(functions)[1])[i]->i64 == obj->i64) {
            sym = AS_SYMBOL(AS_LIST(functions)[0])[i];
            break;
        }
    }

    if (sym)
        return str_from_symbol(sym);

    return (str_p) "@fn";
}

obj_p env_get_internal_function(lit_p name) {
    i64_t i;

    i = find_sym(AS_LIST(runtime_get()->env.functions)[0], name);

    if (i != NULL_I64)
        return clone_obj(AS_LIST(AS_LIST(runtime_get()->env.functions)[1])[i]);

    return NULL_OBJ;
}

obj_p env_get_internal_function_by_id(i64_t id) {
    i64_t i;

    i = find_raw(AS_LIST(runtime_get()->env.functions)[0], &id);

    if (i != NULL_I64)
        return clone_obj(AS_LIST(AS_LIST(runtime_get()->env.functions)[1])[i]);

    return NULL_OBJ;
}

str_p env_get_internal_entry_name(lit_p name, i64_t len, obj_p entries, i64_t *index, b8_t exact) {
    i64_t i, l, *names;
    i64_t n;
    str_p nm;

    l = entries->len;
    names = AS_SYMBOL(entries);

    if (exact) {
        for (i = 0; i < l; i++) {
            nm = str_from_symbol(names[i]);
            n = SYMBOL_STRLEN(names[i]);
            if (n == len && strncmp(name, nm, len) == 0)
                return nm;
        }
    } else {
        for (i = *index; i < l; i++) {
            nm = str_from_symbol(names[i]);
            n = SYMBOL_STRLEN(names[i]);
            if (len < n && strncmp(name, nm, len) == 0) {
                *index = i + 1;
                return nm;
            }
        }
    }

    return NULL;
}

str_p env_get_internal_keyword_name(lit_p name, i64_t len, i64_t *index, b8_t exact) {
    return env_get_internal_entry_name(name, len, runtime_get()->env.keywords, index, exact);
}

str_p env_get_internal_function_name(lit_p name, i64_t len, i64_t *index, b8_t exact) {
    return env_get_internal_entry_name(name, len, AS_LIST(runtime_get()->env.functions)[0], index, exact);
}

str_p env_get_global_name(lit_p name, i64_t len, i64_t *index, i64_t *sbidx) {
    i64_t *names, *cols;
    i64_t i, j, n, l, m;
    str_p nm;
    obj_p *vals;

    l = AS_LIST(runtime_get()->env.variables)[0]->len;
    names = AS_I64(AS_LIST(runtime_get()->env.variables)[0]);
    vals = AS_LIST(AS_LIST(runtime_get()->env.variables)[1]);

    for (i = *index; i < l; i++) {
        nm = str_from_symbol(names[i]);
        n = SYMBOL_STRLEN(names[i]);
        if (len < n && strncmp(name, nm, len) == 0) {
            *index = i + 1;
            return nm;
        }

        if (vals[i]->type == TYPE_TABLE) {
            cols = AS_I64(AS_LIST(vals[i])[0]);
            m = AS_LIST(vals[i])[0]->len;
            for (j = *sbidx; j < m; j++) {
                nm = str_from_symbol(cols[j]);
                n = SYMBOL_STRLEN(cols[j]);
                if (len < n && strncmp(name, nm, len) == 0) {
                    *sbidx = j + 1;
                    return nm;
                }
            }

            *sbidx = 0;
        }
    }

    return NULL;
}

obj_p ray_internals(obj_p *x, i64_t n) {
    UNUSED(x);
    UNUSED(n);
    return clone_obj(runtime_get()->env.internals);
}