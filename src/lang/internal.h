/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

/**   Shared helpers for eval.c split — included by arith.c, cmp.c, agg.c, etc.
 *   Small hot-path helpers are static inline; larger functions that remain in
 *   eval.c are declared extern.
 */

#ifndef RAY_LANG_INTERNAL_H
#define RAY_LANG_INTERNAL_H

#include "lang/eval.h"
#include "lang/format.h"
#include "core/types.h"
#include "mem/heap.h"
#include "table/sym.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════
 * Atom constructors
 * ══════════════════════════════════════════ */

static inline ray_t* make_i64(int64_t v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = -RAY_I64;
    obj->i64 = v;
    return obj;
}

static inline ray_t* make_f64(double v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = -RAY_F64;
    obj->f64 = v;
    return obj;
}

static inline ray_t* make_i16(int16_t v) {
    return ray_i16(v);
}

static inline ray_t* make_i32(int32_t v) {
    return ray_i32(v);
}

static inline ray_t* make_u8(uint8_t v) {
    return ray_u8(v);
}

static inline ray_t* make_bool(uint8_t v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = -RAY_BOOL;
    obj->b8 = v;
    return obj;
}

/* ══════════════════════════════════════════
 * Type checks and numeric extraction
 * ══════════════════════════════════════════ */

/* Helpers to extract numeric value as double */
static inline int is_numeric(ray_t* x) {
    return x->type == -RAY_I64 || x->type == -RAY_F64 ||
           x->type == -RAY_I16 || x->type == -RAY_I32 ||
           x->type == -RAY_U8  || x->type == -RAY_BOOL;
}

/* Check if an atom is a temporal type */
static inline int is_temporal(ray_t* x) {
    return x->type == -RAY_DATE || x->type == -RAY_TIME || x->type == -RAY_TIMESTAMP;
}

/* Convert temporal atom to nanoseconds for cross-temporal comparison.
 * DATE = days since epoch -> ns, TIME = ms since midnight -> ns, TIMESTAMP = ns */
static inline int64_t temporal_as_ns(ray_t* x) {
    if (x->type == -RAY_TIMESTAMP) return x->i64;
    if (x->type == -RAY_DATE)      return (int64_t)x->i32 * 86400000000000LL;
    if (x->type == -RAY_TIME)      return (int64_t)x->i32 * 1000000LL;
    return 0;
}

/* Extract integer value from any integer atom as int64_t */
static inline int64_t as_i64(ray_t* x) {
    if (x->type == -RAY_I64)  return x->i64;
    if (x->type == -RAY_I32)  return (int64_t)x->i32;
    if (x->type == -RAY_I16)  return (int64_t)x->i16;
    if (x->type == -RAY_U8)   return (int64_t)x->u8;
    return x->i64; /* fallback */
}

static inline double as_f64(ray_t* x) {
    if (x->type == -RAY_F64) return x->f64;
    if (x->type == -RAY_I64) return (double)x->i64;
    if (x->type == -RAY_I32) return (double)x->i32;
    if (x->type == -RAY_I16) return (double)x->i16;
    if (x->type == -RAY_U8)  return (double)x->u8;
    if (x->type == -RAY_STR && ray_str_len(x) == 1) return (double)(unsigned char)x->sdata[0];
    if (x->type == -RAY_BOOL) return (double)x->b8;
    if (x->type == -RAY_DATE || x->type == -RAY_TIME) return (double)x->i32;
    if (x->type == -RAY_TIMESTAMP) return (double)x->i64;
    return (double)x->i64;
}

static inline int is_float_op(ray_t* a, ray_t* b) {
    return a->type == -RAY_F64 || b->type == -RAY_F64;
}

/* ══════════════════════════════════════════
 * Null/type helpers
 * ══════════════════════════════════════════ */

/* RAY_ATOM_IS_NULL and ray_typed_null are in rayforce.h */

/* Return a typed null for the promoted result type of two operands */
static inline ray_t* null_for_promoted(ray_t* a, ray_t* b) {
    if (a->type == -RAY_F64 || b->type == -RAY_F64)
        return ray_typed_null(-RAY_F64);
    if (a->type == -RAY_I64 || b->type == -RAY_I64)
        return ray_typed_null(-RAY_I64);
    if (a->type == -RAY_I32 || b->type == -RAY_I32)
        return ray_typed_null(-RAY_I32);
    if (a->type == -RAY_I16 || b->type == -RAY_I16)
        return ray_typed_null(-RAY_I16);
    return ray_typed_null(-RAY_I64);
}

/* ══════════════════════════════════════════
 * Type promotion
 * ══════════════════════════════════════════ */

/* Determine the promoted integer result type for two numeric operands.
 * Returns atom type code (negative). */
static inline int8_t promote_int_type(ray_t* a, ray_t* b) {
    if (a->type == -RAY_I64 || b->type == -RAY_I64) return -RAY_I64;
    if (a->type == -RAY_I32 || b->type == -RAY_I32) return -RAY_I32;
    if (a->type == -RAY_U8 || b->type == -RAY_U8) {
        /* u8 op u8 -> u8, but u8 op i16 -> i16 etc */
        if (a->type == -RAY_U8 && b->type == -RAY_U8) return -RAY_U8;
        return (a->type == -RAY_I16 || b->type == -RAY_I16) ? -RAY_I16 : -RAY_I64;
    }
    if (a->type == -RAY_I16 || b->type == -RAY_I16) return -RAY_I16;
    return -RAY_I64;
}

/* Promote integer type following right-operand's type (K/q semantics for sub) */
static inline int8_t promote_int_type_right(ray_t* a, ray_t* b) {
    (void)a;
    int8_t bt = b->type;
    if (bt == -RAY_I32 || bt == -RAY_I16 || bt == -RAY_U8 || bt == -RAY_I64)
        return bt;
    int8_t at = a->type;
    if (at == -RAY_I32 || at == -RAY_I16 || at == -RAY_U8 || at == -RAY_I64)
        return at;
    return -RAY_I64;
}

/* Create a result atom of the given type from an int64_t value */
static inline ray_t* make_typed_int(int8_t atom_type, int64_t val) {
    switch (atom_type) {
    case -RAY_I16: return make_i16((int16_t)val);
    case -RAY_I32: return make_i32((int32_t)val);
    case -RAY_U8:  return make_u8((uint8_t)val);
    default:       return make_i64(val);
    }
}

/* ══════════════════════════════════════════
 * Type name helper
 * ══════════════════════════════════════════ */

/* Removed: type_sym_name() — use ray_type_name() directly.
 * Lowercase for atoms (negative type), uppercase for vectors (positive). */

/* ══════════════════════════════════════════
 * Truthiness
 * ══════════════════════════════════════════ */

/* Logical -- coerce to truthiness (0/nil/false = falsy, else truthy).
 * Null forms (RAY_NULL singleton and typed null atoms) are falsy. */
static inline int is_truthy(ray_t* x) {
    if (RAY_IS_NULL(x) || RAY_ATOM_IS_NULL(x)) return 0;
    if (x->type == -RAY_BOOL) return x->b8;
    if (x->type == -RAY_I64)  return x->i64 != 0;
    if (x->type == -RAY_F64)  return x->f64 != 0.0;
    return 1; /* non-null objects are truthy */
}

/* ══════════════════════════════════════════
 * Collection helpers
 * ══════════════════════════════════════════ */

static inline int is_list(ray_t* x) {
    return x && !RAY_IS_ERR(x) && x->type == RAY_LIST;
}

/* Check if x is a collection: boxed list OR typed vector */
static inline int is_collection(ray_t* x) {
    return x && !RAY_IS_ERR(x) && (x->type == RAY_LIST || ray_is_vec(x));
}

/* Extract the i-th element of a collection as a ray_t* atom.
 * For boxed lists, returns the stored pointer (no alloc).
 * For typed vectors, allocates a new atom.  Caller must release
 * atoms obtained from typed vectors (allocated == 1). */
static inline ray_t* collection_elem(ray_t* coll, int64_t i, int *allocated) {
    if (coll->type == RAY_LIST) {
        *allocated = 0;
        return ((ray_t**)ray_data(coll))[i];
    }
    *allocated = 1;
    if (ray_vec_is_null(coll, i))
        return ray_typed_null(-coll->type);
    void* d = ray_data(coll);
    switch (coll->type) {
        case RAY_I64:       return ray_i64(((int64_t*)d)[i]);
        case RAY_F64:       return ray_f64(((double*)d)[i]);
        case RAY_I32:       return ray_i32(((int32_t*)d)[i]);
        case RAY_I16:       return ray_i16(((int16_t*)d)[i]);
        case RAY_BOOL:      return ray_bool(((bool*)d)[i]);
        case RAY_SYM:       return ray_sym(ray_read_sym(d, i, coll->type, coll->attrs));
        case RAY_U8:        return ray_u8(((uint8_t*)d)[i]);
        case RAY_DATE:      return ray_date((int64_t)((int32_t*)d)[i]);
        case RAY_TIME:      return ray_time((int64_t)((int32_t*)d)[i]);
        case RAY_TIMESTAMP: return ray_timestamp(((int64_t*)d)[i]);
        case RAY_GUID: {
            const uint8_t* gd = ((uint8_t*)d) + i * 16;
            return ray_guid(gd);
        }
        /* RAY_CHAR removed -- char vectors no longer exist */
        case RAY_STR: {
            size_t slen = 0;
            const char* sp = ray_str_vec_get(coll, i, &slen);
            return ray_str(sp ? sp : "", sp ? slen : 0);
        }
        default:            *allocated = 0; return ray_error("type", NULL);
    }
}

/* Extract a value from an atom for storage, handling cross-type casting.
 * Returns the value as int64_t (for integer/temporal types). */
static inline int64_t elem_as_i64(ray_t* elem) {
    if (elem->type == -RAY_I64 || elem->type == -RAY_TIMESTAMP ||
        elem->type == -RAY_DATE || elem->type == -RAY_TIME ||
        elem->type == -RAY_SYM) return elem->i64;
    if (elem->type == -RAY_I32)  return (int64_t)elem->i32;
    if (elem->type == -RAY_I16)  return (int64_t)elem->i16;
    if (elem->type == -RAY_U8)   return (int64_t)elem->u8;
    if (elem->type == -RAY_F64)  return (int64_t)elem->f64;
    return elem->i64;
}

/* Store a scalar result into a typed vector at position i.
 * Returns 0 on success, -1 if the element type doesn't match. */
static inline int store_typed_elem(ray_t* vec, int64_t i, ray_t* elem) {
    if (RAY_ATOM_IS_NULL(elem)) {
        int esz = ray_elem_size(vec->type);
        memset((char*)ray_data(vec) + i * esz, 0, esz);
        ray_vec_set_null(vec, i, true);
        return 0;
    }
    switch (vec->type) {
        case RAY_I64:       ((int64_t*)ray_data(vec))[i]  = elem_as_i64(elem); return 0;
        case RAY_F64:       ((double*)ray_data(vec))[i]    = (elem->type == -RAY_F64) ? elem->f64 : (double)elem_as_i64(elem); return 0;
        case RAY_I32:       ((int32_t*)ray_data(vec))[i]   = (int32_t)elem_as_i64(elem); return 0;
        case RAY_I16:       ((int16_t*)ray_data(vec))[i]   = (int16_t)elem_as_i64(elem); return 0;
        case RAY_BOOL:      ((bool*)ray_data(vec))[i]      = elem->b8;  return 0;
        case RAY_U8:        ((uint8_t*)ray_data(vec))[i]   = (uint8_t)elem_as_i64(elem); return 0;
        case RAY_DATE:      ((int32_t*)ray_data(vec))[i]   = (int32_t)elem_as_i64(elem); return 0;
        case RAY_TIME:      ((int32_t*)ray_data(vec))[i]   = (int32_t)elem_as_i64(elem); return 0;
        case RAY_TIMESTAMP: ((int64_t*)ray_data(vec))[i]   = elem_as_i64(elem); return 0;
        case RAY_SYM:       ray_write_sym(ray_data(vec), i, (uint64_t)elem->i64, vec->type, vec->attrs); return 0;
        case RAY_GUID:      if (elem->obj) memcpy(((uint8_t*)ray_data(vec)) + i * 16, ray_data(elem->obj), 16); return 0;
        default: return -1;
    }
}

/* ══════════════════════════════════════════
 * Extern forward declarations — larger functions that stay in eval.c
 * ══════════════════════════════════════════ */

ray_t* atomic_map_binary_op(ray_binary_fn fn, uint16_t dag_opcode, ray_t* left, ray_t* right);
ray_t* atomic_map_unary(ray_unary_fn fn, ray_t* arg);
ray_t* to_boxed_list(ray_t* x);
ray_t* unbox_vec_arg(ray_t* x, ray_t** _bx);
ray_t* call_lambda(ray_t* lambda, ray_t** call_args, int64_t argc);
ray_t* call_fn1(ray_t* fn, ray_t* arg);
ray_t* call_fn2(ray_t* fn, ray_t* a, ray_t* b);
ray_t* gather_by_idx(ray_t* vec, int64_t* idx, int64_t n);
ray_t* ray_sort(ray_t** cols, uint8_t* descs, uint8_t* nulls_first,
                uint8_t n_cols, int64_t nrows);
int    char_str_cmp(ray_t* a, ray_t* b, int *out);
int    is_comparable(ray_t* x);

/* Arithmetic builtins (formerly static in eval.c, now in arith.c) */
ray_t* ray_round_fn(ray_t* x);
ray_t* ray_floor_fn(ray_t* x);
ray_t* ray_ceil_fn(ray_t* x);
ray_t* ray_abs_fn(ray_t* x);
ray_t* ray_sqrt_fn(ray_t* x);
ray_t* ray_log_fn(ray_t* x);
ray_t* ray_exp_fn(ray_t* x);

/* Collection helpers (formerly static in eval.c, now in collection.c) */
int    atom_eq(ray_t* a, ray_t* b);
ray_t* list_to_typed_vec(ray_t* list, int8_t orig_vec_type);

/* Collection builtins (formerly static in eval.c, now in collection.c) */
ray_t* ray_map_fn(ray_t** args, int64_t n);
ray_t* ray_pmap_fn(ray_t** args, int64_t n);
ray_t* ray_fold_fn(ray_t** args, int64_t n);
ray_t* ray_scan_fn(ray_t** args, int64_t n);
ray_t* ray_filter_fn(ray_t* vec, ray_t* mask);
ray_t* ray_apply_fn(ray_t** args, int64_t n);
ray_t* ray_distinct_fn(ray_t* x);
ray_t* ray_in_fn(ray_t* val, ray_t* vec);
ray_t* ray_except_fn(ray_t* vec1, ray_t* vec2);
ray_t* ray_union_fn(ray_t* vec1, ray_t* vec2);
ray_t* ray_sect_fn(ray_t* vec1, ray_t* vec2);
ray_t* ray_take_fn(ray_t* vec, ray_t* n_obj);
ray_t* ray_at_fn(ray_t* vec, ray_t* idx);
ray_t* ray_find_fn(ray_t* vec, ray_t* val);
ray_t* ray_til_fn(ray_t* x);
ray_t* ray_reverse_fn(ray_t* x);
ray_t* ray_rand_fn(ray_t* a, ray_t* b);
ray_t* ray_bin_fn(ray_t* sorted, ray_t* val);
ray_t* ray_binr_fn(ray_t* sorted, ray_t* val);
ray_t* ray_map_left_fn(ray_t** args, int64_t n);
ray_t* ray_map_right_fn(ray_t** args, int64_t n);
ray_t* ray_fold_left_fn(ray_t** args, int64_t n);
ray_t* ray_fold_right_fn(ray_t** args, int64_t n);
ray_t* ray_scan_left_fn(ray_t** args, int64_t n);
ray_t* ray_scan_right_fn(ray_t** args, int64_t n);
ray_t* ray_enlist_fn(ray_t** args, int64_t n);

/* String builtins (formerly static in eval.c, now in str_builtin.c) */
ray_t* ray_split_fn(ray_t* str, ray_t* delim);
ray_t* ray_like_fn(ray_t* x, ray_t* pattern);
ray_t* ray_sym_name_fn(ray_t* x);

/* Table builtins (formerly static in eval.c, now in table_builtin.c) */
uint16_t pivot_fn_to_agg_op(ray_t* fn);
ray_t* ray_pivot_fn(ray_t** args, int64_t n);
ray_t* ray_modify_fn(ray_t** args, int64_t n);
ray_t* ray_alter_fn(ray_t** args, int64_t n);
ray_t* ray_del_fn(ray_t** args, int64_t n);
ray_t* ray_row_fn(ray_t* tbl, ray_t* idx);
ray_t* ray_union_all_fn(ray_t* t1, ray_t* t2);
ray_t* ray_table_distinct_fn(ray_t* tbl);
ray_t* ray_unify_fn(ray_t* a, ray_t* b);

/* Concat (formerly static in eval.c, now extern for table_builtin.c) */
ray_t* ray_concat_fn(ray_t* a, ray_t* b);

/* Temporal builtins (formerly static in eval.c, now in temporal.c) */
ray_t* ray_date_clock_fn(ray_t* arg);
ray_t* ray_time_clock_fn(ray_t* arg);
ray_t* ray_timestamp_clock_fn(ray_t* arg);

/* Sort builtins (formerly static in eval.c, now in sort.c) */
ray_t* ray_asc_fn(ray_t* x);
ray_t* ray_desc_fn(ray_t* x);
ray_t* ray_iasc_fn(ray_t* x);
ray_t* ray_idesc_fn(ray_t* x);
ray_t* ray_rank_fn(ray_t* x);
ray_t* sort_table_by_keys(ray_t* tbl, ray_t* keys, uint8_t descending);
ray_t* ray_xasc_fn(ray_t* tbl, ray_t* keys);
ray_t* ray_xdesc_fn(ray_t* tbl, ray_t* keys);
ray_t* ray_xrank_fn(ray_t* n_obj, ray_t* vec);

/* Datalog builtins (formerly static in eval.c, now in datalog_builtin.c) */
ray_t* ray_datoms_fn(ray_t** args, int64_t n);
ray_t* ray_assert_fact_fn(ray_t** args, int64_t n);
ray_t* ray_retract_fact_fn(ray_t** args, int64_t n);
ray_t* ray_scan_eav_fn(ray_t** args, int64_t n);
ray_t* ray_pull_fn(ray_t** args, int64_t n);
ray_t* ray_rule_fn(ray_t** args, int64_t n);
ray_t* ray_query_fn(ray_t** args, int64_t n);
ray_t* ray_dl_program_fn(ray_t** args, int64_t n);
ray_t* ray_dl_add_edb_fn(ray_t** args, int64_t n);
ray_t* ray_dl_stratify_fn(ray_t* x);
ray_t* ray_dl_eval_fn(ray_t* x);
ray_t* ray_dl_query_fn(ray_t* prog_obj, ray_t* pred_obj);
ray_t* ray_dl_provenance_fn(ray_t* prog_obj, ray_t* pred_obj);
void   ray_dl_reset_rules(void);

/* System builtins (formerly static in eval.c, now in system.c) */
ray_t* ray_eval_builtin_fn(ray_t* x);
ray_t* ray_parse_builtin_fn(ray_t* x);
ray_t* ray_print_fn(ray_t** args, int64_t n);
ray_t* ray_meta_fn(ray_t* x);
ray_t* ray_gc_fn(ray_t** args, int64_t n);
ray_t* ray_system_fn(ray_t* x);
ray_t* ray_getenv_fn(ray_t* x);
ray_t* ray_setenv_fn(ray_t* name, ray_t* val);
ray_t* ray_quote_fn(ray_t** args, int64_t n);
ray_t* ray_return_fn(ray_t* x);
ray_t* ray_args_fn(ray_t* x);
ray_t* ray_rc_fn(ray_t* x);
ray_t* ray_diverse_fn(ray_t* x);
ray_t* ray_get_fn(ray_t* dict, ray_t* key);
ray_t* ray_remove_fn(ray_t* dict, ray_t* key);
ray_t* ray_timer_fn(ray_t* x);
ray_t* ray_env_fn(ray_t* x);
ray_t* ray_internals_fn(ray_t** args, int64_t n);
ray_t* ray_memstat_fn(ray_t** args, int64_t n);
ray_t* ray_sysinfo_fn(ray_t** args, int64_t n);
ray_t* ray_ser_fn(ray_t* val);
ray_t* ray_de_fn(ray_t* val);
ray_t* ray_hopen_fn(ray_t* x);
ray_t* ray_hclose_fn(ray_t* x);
ray_t* ray_hsend_fn(ray_t* handle, ray_t* msg);
ray_t* ray_set_splayed_fn(ray_t** args, int64_t n);
ray_t* ray_get_splayed_fn(ray_t** args, int64_t n);
ray_t* ray_get_parted_fn(ray_t** args, int64_t n);
ray_t* ray_guid_fn(ray_t* n_arg);

/* Group (formerly static in eval.c, now extern for query.c) */
ray_t* ray_group_fn(ray_t* x);

/* I/O and formatting builtins (formerly in eval.c, now in ops/builtins.c) */
ray_t* ray_println_fn(ray_t** args, int64_t n);
ray_t* ray_show_fn(ray_t** args, int64_t n);
ray_t* ray_format_fn(ray_t** args, int64_t n);
ray_t* ray_resolve_fn(ray_t** args, int64_t n);
ray_t* ray_timeit_fn(ray_t** args, int64_t n);
ray_t* ray_exit_fn(ray_t* arg);
ray_t* ray_read_csv_fn(ray_t** args, int64_t n);
ray_t* ray_write_csv_fn(ray_t** args, int64_t n);
ray_t* ray_cast_fn(ray_t* type_sym, ray_t* val);
ray_t* ray_type_fn(ray_t* val);
ray_t* ray_read_file_fn(ray_t* path_obj);
ray_t* ray_load_file_fn(ray_t* path_obj);
ray_t* ray_write_file_fn(ray_t* path_obj, ray_t* content);

/* Misc builtins (formerly in eval.c, now in ops/builtins.c) */
ray_t* ray_dict_fn(ray_t* keys, ray_t* vals);
ray_t* ray_nil_fn(ray_t* x);
ray_t* ray_where_fn(ray_t* x);
ray_t* ray_raze_fn(ray_t* x);
ray_t* ray_within_fn(ray_t* vals, ray_t* range);
ray_t* ray_fdiv_fn(ray_t* a, ray_t* b);

/* Query bridge builtins (formerly in eval.c, now in ops/query.c) */
ray_t* ray_select_fn(ray_t** args, int64_t n);
ray_t* ray_update_fn(ray_t** args, int64_t n);
ray_t* ray_insert_fn(ray_t** args, int64_t n);
ray_t* ray_upsert_fn(ray_t** args, int64_t n);
ray_t* ray_xbar_fn(ray_t* col, ray_t* bucket);
ray_t* ray_left_join_fn(ray_t** args, int64_t n);
ray_t* ray_inner_join_fn(ray_t** args, int64_t n);
ray_t* ray_anti_join_fn(ray_t** args, int64_t n);
ray_t* ray_window_join_fn(ray_t** args, int64_t n);
ray_t* ray_asof_join_fn(ray_t** args, int64_t n);

/* Convenience wrapper: atomic_map_binary with no DAG opcode */
static inline ray_t* atomic_map_binary(ray_binary_fn fn, ray_t* left, ray_t* right) {
    return atomic_map_binary_op(fn, 0, left, right);
}

#endif /* RAY_LANG_INTERNAL_H */
