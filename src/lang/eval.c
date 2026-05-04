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

#include "lang/eval.h"
#include "lang/internal.h"
#include "app/repl.h"
#include "lang/env.h"
#include "lang/nfo.h"
#include "lang/parse.h"
#include "core/types.h"
#include "ops/ops.h"
#include "ops/temporal.h"
#include "ops/datalog.h"
#include "ops/idxop.h"
#include "ops/linkop.h"
#include "table/sym.h"
#include "core/profile.h"
#include "table/sym.h"
#include "mem/heap.h"
#include "mem/sys.h"
/* store/serde.h, store/splay.h, store/part.h moved to system.c */
/* ray_lang_print, ray_cast_fn, etc. moved to ops/builtins.c */
/* ray_error() is declared in <rayforce.h> (included via eval.h) */

#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <signal.h>
#include <time.h>

/* Maximum recursion depth for ray_eval() to prevent stack overflow */
#define RAY_EVAL_MAX_DEPTH 512
_Thread_local static int eval_depth = 0;

/* Thread-local nfo for eval context — tracks source locations during evaluation */
static _Thread_local ray_t* g_eval_nfo = NULL;

/* Thread-local error trace — list of [span_i64, filename, fn_name, source] frames */
static _Thread_local ray_t* g_error_trace = NULL;

/* Interrupt flag — set by REPL signal handler, checked by eval/VM loops */
static volatile sig_atomic_t g_eval_interrupted = 0;

void ray_request_interrupt(void)      { g_eval_interrupted = 1; }
void ray_clear_interrupt(void)        { g_eval_interrupted = 0; }
bool ray_interrupted(void)            { return g_eval_interrupted != 0; }

/* Legacy internal names — thin wrappers kept for existing callers. */
void ray_eval_request_interrupt(void) { ray_request_interrupt(); }
void ray_eval_clear_interrupt(void)   { ray_clear_interrupt(); }
int  ray_eval_is_interrupted(void)    { return ray_interrupted(); }

ray_t* ray_eval_get_nfo(void) { return g_eval_nfo; }
void   ray_eval_set_nfo(ray_t* nfo) { g_eval_nfo = nfo; }

ray_t* ray_get_error_trace(void) { return g_error_trace; }
void   ray_clear_error_trace(void) {
    if (g_error_trace) { ray_release(g_error_trace); g_error_trace = NULL; }
}

/* ══════════════════════════════════════════
 * Restricted-mode check
 * ══════════════════════════════════════════ */

static _Thread_local bool g_eval_restricted = false;

void ray_eval_set_restricted(bool on) { g_eval_restricted = on; }
bool ray_eval_get_restricted(void)    { return g_eval_restricted; }

static inline bool fn_is_restricted(ray_t* fn_obj) {
    return g_eval_restricted && (fn_obj->attrs & RAY_FN_RESTRICTED);
}

/* ══════════════════════════════════════════
 * Error handling: try / raise
 * ══════════════════════════════════════════ */

static _Thread_local ray_t *__raise_val = NULL;

/* (raise value) — raise an error with the given value */
ray_t* ray_raise_fn(ray_t* val) {
    if (__raise_val) ray_release(__raise_val);
    ray_retain(val);
    __raise_val = val;
    return ray_error("domain", NULL);
}

/* (try expr handler) — evaluate expr, if error call handler with error value.
 * Special form: receives unevaluated args. */
ray_t* ray_try_fn(ray_t* expr, ray_t* handler_expr) {
    ray_t* result = ray_eval(expr);
    if (!RAY_IS_ERR(result)) return result;

    /* Get error value (set by raise, or default for runtime errors) */
    ray_t* err_val = __raise_val;
    __raise_val = NULL;
    if (!err_val) err_val = make_i64(0);

    /* Evaluate handler expression */
    ray_t* handler = ray_eval(handler_expr);
    if (RAY_IS_ERR(handler)) {
        ray_release(err_val);
        return handler;
    }

    /* Call handler with error value */
    ray_t* handler_result;
    if (handler->type == RAY_LAMBDA) {
        ray_t* args[1] = { err_val };
        handler_result = call_lambda(handler, args, 1);
    } else if (handler->type == RAY_UNARY) {
        ray_unary_fn fn = (ray_unary_fn)(uintptr_t)handler->i64;
        handler_result = fn(err_val);
    } else {
        handler_result = ray_error("type", NULL);
    }

    ray_release(err_val);
    ray_release(handler);
    return handler_result;
}

/* ══════════════════════════════════════════
 * FN_ATOMIC auto-mapping helpers
 * ══════════════════════════════════════════ */

/* Convert a typed vector to a boxed list.  If already a list, retains
 * and returns it directly.  Caller owns the returned object. */
ray_t* to_boxed_list(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (x->type == RAY_LIST) { ray_retain(x); return x; }
    if (!ray_is_vec(x)) return ray_error("type", NULL);

    int64_t len = ray_len(x);
    ray_t* list = ray_alloc(len * sizeof(ray_t*));
    if (!list) return ray_error("oom", NULL);
    list->type = RAY_LIST;
    list->len = len;
    ray_t** dst = (ray_t**)ray_data(list);

    for (int64_t i = 0; i < len; i++) {
        int alloc = 0;
        dst[i] = collection_elem(x, i, &alloc);
        if (RAY_IS_ERR(dst[i])) {
            for (int64_t j = 0; j < i; j++) ray_release(dst[j]);
            ray_release(list);
            return dst[i];
        }
        /* collection_elem always allocates for typed vecs, so ownership transfers */
    }
    return list;
}

/* Unbox a typed vector argument to a boxed list for use in builtins.
 * Sets *_bx to the allocated boxed list (caller must release) or NULL.
 * Returns the (possibly converted) argument, or an error. */
ray_t* unbox_vec_arg(ray_t* x, ray_t** _bx) {
    *_bx = NULL;
    if (x && !RAY_IS_ERR(x) && ray_is_vec(x)) {
        *_bx = to_boxed_list(x);
        return *_bx;
    }
    return x;
}

/* Construct a zero-valued owned atom matching the element type of a
 * vector (typed or RAY_LIST).  Used only for empty-collection type
 * probing by the atomic-map helpers: it lets us invoke a binary or
 * unary `fn` with a representative scalar so the result's output
 * type is observable even when the input has no elements.
 *
 * Symbol / string / GUID columns must produce atoms of their own
 * element type — falling back to i64(0) for those would make, e.g.,
 * `(== empty_sym_col 'foo)` probe an integer comparison and return
 * I64 instead of the BOOL a non-empty input would yield.  Unknown
 * element types still fall back to ray_i64(0). */
static ray_t* zero_atom_for_elem_type(ray_t* coll) {
    if (!coll) return ray_i64(0);
    if (coll->type == RAY_LIST) return ray_i64(0);
    switch (coll->type) {
        case RAY_I64:       return ray_i64(0);
        case RAY_I32:       return ray_i32(0);
        case RAY_I16:       return ray_i16(0);
        case RAY_U8:        return ray_u8(0);
        case RAY_BOOL:      return make_bool(0);
        case RAY_F64:       return make_f64(0.0);
        case RAY_DATE:      return ray_date(0);
        case RAY_TIME:      return ray_time(0);
        case RAY_TIMESTAMP: return ray_timestamp(0);
        case RAY_SYM:       return ray_sym(0);
        case RAY_STR:       return ray_str("", 0);
        case RAY_GUID: {
            static const uint8_t zero_guid[16] = {0};
            return ray_guid(zero_guid);
        }
        default:            return ray_i64(0);
    }
}

/* Map a binary function element-wise over collections.
 * Both args can be collections (zip-map) or one scalar (broadcast).
 * Produces typed vectors when output is numeric/bool, boxed lists otherwise. */
ray_t* atomic_map_binary_op(ray_binary_fn fn, uint16_t dag_opcode, ray_t* left, ray_t* right) {
    int left_coll = is_collection(left);
    int right_coll = is_collection(right);

    if (!left_coll && !right_coll) return fn(left, right);

    int64_t len;
    if (left_coll && right_coll) {
        len = ray_len(left) < ray_len(right) ? ray_len(left) : ray_len(right);
    } else {
        len = left_coll ? ray_len(left) : ray_len(right);
    }

    if (len == 0) {
        /* Empty collection — no first element to probe, so fabricate a
         * zero-valued atom of each operand's element type and run `fn`
         * on it to learn the output type.  Without this the result was
         * hardcoded to I64 and lost the semantics of type-preserving
         * ops (e.g. `(xbar empty_TIME_col 10000)` returned an I64 empty
         * vector instead of a TIME one). */
        ray_t* la = left_coll  ? zero_atom_for_elem_type(left)  : left;
        ray_t* ra = right_coll ? zero_atom_for_elem_type(right) : right;
        ray_t* probe = (la && ra && !RAY_IS_ERR(la) && !RAY_IS_ERR(ra))
                       ? fn(la, ra) : NULL;
        if (left_coll  && la) ray_release(la);
        if (right_coll && ra) ray_release(ra);
        if (probe && !RAY_IS_ERR(probe) && probe->type < 0) {
            int8_t t = (int8_t)(-probe->type);
            ray_release(probe);
            return ray_vec_new(t, 0);
        }
        if (probe && !RAY_IS_ERR(probe)) ray_release(probe);
        return ray_vec_new(RAY_I64, 0);
    }

    /* Probe first element to determine output type */
    int la0 = 0, ra0 = 0;
    ray_t* a0 = left_coll  ? collection_elem(left, 0, &la0)  : left;
    ray_t* b0 = right_coll ? collection_elem(right, 0, &ra0) : right;
    ray_t* e0;
    if (RAY_IS_ERR(a0) || RAY_IS_ERR(b0)) {
        e0 = ray_error("type", NULL);
    } else if (is_collection(a0) || is_collection(b0)) {
        e0 = atomic_map_binary(fn, a0, b0);
    } else {
        e0 = fn(a0, b0);
    }
    if (la0) ray_release(a0);
    if (ra0) ray_release(b0);
    if (RAY_IS_ERR(e0)) return e0;

    int8_t out_type = -(e0->type);  /* atom type (-RAY_I64) → vector type (RAY_I64) */

    /* If either input is a boxed list (mixed types), always use boxed list output
     * to preserve type heterogeneity */
    int force_boxed = (left_coll && left->type == RAY_LIST) ||
                      (right_coll && right->type == RAY_LIST);

    /* When the probed result is a null atom, the fn already chose the correct
     * result type (e.g., division returns left-operand-typed null).  Skip the
     * wider-wins promotion so the typed null lands in the right vector type. */
    int e0_null = RAY_ATOM_IS_NULL(e0);

    /* When the probed result is a boolean (from comparison ops like ==, <, etc.),
     * preserve the bool output type — do not promote to wider integer type. */
    int e0_bool = (e0->type == -RAY_BOOL);

    /* When LEFT is scalar broadcast to RIGHT vector, the output type follows
     * the RIGHT vector's element type for integer types,
     * unless float or temporal promotion is involved. */
    if (!e0_null && !e0_bool && !left_coll && right_coll && ray_is_vec(right) && out_type != RAY_F64) {
        int8_t vec_type = right->type;
        /* Only override for integer family: if probed type is wider int, downcast */
        int out_is_int = (out_type == RAY_I64 || out_type == RAY_I32 || out_type == RAY_I16 || out_type == RAY_U8);
        int vec_is_int = (vec_type == RAY_I64 || vec_type == RAY_I32 || vec_type == RAY_I16 || vec_type == RAY_U8);
        if (out_is_int && vec_is_int)
            out_type = vec_type;
        /* For temporal: only override if both are same temporal family */
        if ((vec_type == RAY_DATE || vec_type == RAY_TIME || vec_type == RAY_TIMESTAMP) &&
            out_type == vec_type)
            out_type = vec_type; /* no-op, just keep it */
    }
    /* When LEFT is vector and RIGHT is scalar, output follows WIDER integer
     * type between left vector and right scalar */
    if (!e0_null && !e0_bool && left_coll && !right_coll && ray_is_vec(left) && out_type != RAY_F64 &&
        ray_is_atom(right)) {
        int8_t vt = left->type, st = -(right->type);
        int vt_int = (vt == RAY_I64 || vt == RAY_I32 || vt == RAY_I16 || vt == RAY_U8);
        int st_int = (st == RAY_I64 || st == RAY_I32 || st == RAY_I16 || st == RAY_U8);
        int out_is_int = (out_type == RAY_I64 || out_type == RAY_I32 || out_type == RAY_I16 || out_type == RAY_U8);
        if (out_is_int && vt_int && st_int)
            out_type = (vt >= st) ? vt : st; /* wider wins */
    }
    /* When both are vectors, output type follows wider integer type */
    if (!e0_null && !e0_bool && left_coll && right_coll && ray_is_vec(left) && ray_is_vec(right) && out_type != RAY_F64) {
        int8_t lt = left->type, rt = right->type;
        int lt_int = (lt == RAY_I64 || lt == RAY_I32 || lt == RAY_I16 || lt == RAY_U8);
        int rt_int = (rt == RAY_I64 || rt == RAY_I32 || rt == RAY_I16 || rt == RAY_U8);
        if (lt_int && rt_int) {
            /* Pick wider: I64 > I32 > I16 > U8 (using type tag ordering) */
            out_type = (lt >= rt) ? lt : rt;
        }
    }

    /* When LEFT is a vector collection, override i32 output to match the
     * left vector type or i64 (e.g., [DATE]-DATE → i64, [i64]-i32 → i64).
     * Keeps i32 only when left vector is actually i32. */
    if (!e0_null && !e0_bool && out_type == RAY_I32 && left_coll && ray_is_vec(left) && left->type != RAY_I32) {
        out_type = RAY_I64;
    }

    /* ══════════════════════════════════════════════════════════════
     * FAST PATH: opcode-driven vectorized execution.
     * I64 ops use direct array loops (lowest overhead).
     * F64/comparison ops route through DAG executor.
     * ══════════════════════════════════════════════════════════════ */

    /* Direct array loops — only for cross-temporal and mixed-width cases
     * that the DAG can't handle. All same-type ops go through DAG. */
    if (0 && !force_boxed && (dag_opcode == OP_DIV || dag_opcode == OP_MOD)) {
        int8_t ltype = left_coll ? left->type : -(left->type);
        int8_t rtype = right_coll ? right->type : -(right->type);
        int esz_l = (ltype == RAY_I64 || ltype == RAY_TIMESTAMP) ? 8 :
                    (ltype == RAY_I32 || ltype == RAY_DATE || ltype == RAY_TIME) ? 4 :
                    (ltype == RAY_I16) ? 2 : (ltype == RAY_U8) ? 1 : 0;
        int esz_r = (rtype == RAY_I64 || rtype == RAY_TIMESTAMP) ? 8 :
                    (rtype == RAY_I32 || rtype == RAY_DATE || rtype == RAY_TIME) ? 4 :
                    (rtype == RAY_I16) ? 2 : (rtype == RAY_U8) ? 1 : 0;
        int lv = left_coll && ray_is_vec(left) && esz_l > 0;
        int rv = right_coll && ray_is_vec(right) && esz_r > 0;
        int ls = !left_coll && esz_l > 0;
        int rs = !right_coll && esz_r > 0;

        /* Cross-type temporal arithmetic (DATE+TIME→TIMESTAMP) needs eval-level
         * conversion — only use fast path when types are compatible for raw arithmetic */
        int8_t ltype2 = lv ? left->type : -(left->type);
        int8_t rtype2 = rv ? right->type : -(right->type);
        int same_class = (esz_l == esz_r) || /* same storage width */
                         (ltype2 == RAY_I64 && rtype2 == RAY_I64) || /* both i64 */
                         (ltype2 == RAY_TIMESTAMP && rtype2 == RAY_TIMESTAMP) ||
                         /* scalar int + any integer vec is fine (just adds raw values) */
                         (ls && (rtype2 == ltype2 || ltype2 == RAY_I64)) ||
                         (rs && (ltype2 == rtype2 || rtype2 == RAY_I64));
        /* Reject cross-temporal: DATE+TIME, TIMESTAMP+DATE, etc. */
        int l_temporal = (ltype2==RAY_DATE||ltype2==RAY_TIME||ltype2==RAY_TIMESTAMP);
        int r_temporal = (rtype2==RAY_DATE||rtype2==RAY_TIME||rtype2==RAY_TIMESTAMP);
        if (l_temporal && r_temporal && ltype2 != rtype2) same_class = 0;

        if (same_class && ((ls && rv) || (lv && rs) || (lv && rv))) {
            /* Read elements as i64 regardless of storage width */
            #define READ_INT(ptr, esz, i) \
                ((esz)==8 ? ((int64_t*)(ptr))[(i)] : \
                 (esz)==4 ? (int64_t)((int32_t*)(ptr))[(i)] : \
                 (esz)==2 ? (int64_t)((int16_t*)(ptr))[(i)] : \
                            (int64_t)((uint8_t*)(ptr))[(i)])
            #define SCALAR_INT(obj) \
                (((obj)->type==-RAY_I64||(obj)->type==-RAY_TIMESTAMP) ? (obj)->i64 : \
                 ((obj)->type==-RAY_I32||(obj)->type==-RAY_DATE||(obj)->type==-RAY_TIME) ? (int64_t)(obj)->i32 : \
                 ((obj)->type==-RAY_I16) ? (int64_t)(obj)->i16 : (int64_t)(obj)->u8)

            /* Reuse input buffer when rc==1 and type matches (avoids allocation).
             * Retain so the caller's ray_release(left/right) doesn't free our output. */
            ray_t* vec;
            if (lv && left->rc == 1 && left->type == out_type) {
                vec = left;
                ray_retain(vec);  /* caller will release left; we keep ownership */
            } else if (rv && right->rc == 1 && right->type == out_type) {
                vec = right;
                ray_retain(vec);
            } else {
                vec = ray_vec_new(out_type, len);
            }
            if (!vec || RAY_IS_ERR(vec)) { ray_release(e0); return vec; }
            vec->len = len;

            void* ldata = lv ? ray_data(left) : NULL;
            void* rdata = rv ? ray_data(right) : NULL;
            int64_t lsv = ls ? SCALAR_INT(left) : 0;
            int64_t rsv = rs ? SCALAR_INT(right) : 0;
            int out_esz = ray_elem_size(out_type);
            int l_atom_null = ls && RAY_ATOM_IS_NULL(left);
            int r_atom_null = rs && RAY_ATOM_IS_NULL(right);

            #define LA(i) (ldata ? READ_INT(ldata, esz_l, i) : lsv)
            #define RA(i) (rdata ? READ_INT(rdata, esz_r, i) : rsv)

            /* Hoist null check: skip per-element null testing when no nulls */
            bool l_has_nulls = l_atom_null || (lv && (left->attrs & RAY_ATTR_HAS_NULLS));
            bool r_has_nulls = r_atom_null || (rv && (right->attrs & RAY_ATTR_HAS_NULLS));
            bool any_nulls = l_has_nulls || r_has_nulls;
            void* out_data = ray_data(vec);  /* hoist out of loop */

            if (!any_nulls) {
                /* Fast path: no nulls — tight loop, no per-element checks */
                for (int64_t i = 0; i < len; i++) {
                    int64_t a = LA(i), b = RA(i);
                    int64_t r;
                    switch (dag_opcode) {
                    case OP_ADD: r = (int64_t)((uint64_t)a + (uint64_t)b); break;
                    case OP_SUB: r = (int64_t)((uint64_t)a - (uint64_t)b); break;
                    case OP_MUL: r = (int64_t)((uint64_t)a * (uint64_t)b); break;
                    case OP_DIV: if (b==0) { if (out_esz==8) ((int64_t*)out_data)[i]=0; else if (out_esz==4) ((int32_t*)out_data)[i]=0; else if (out_esz==2) ((int16_t*)out_data)[i]=0; else ((uint8_t*)out_data)[i]=0; ray_vec_set_null(vec,i,true); continue; }
                                r=a/b; if ((a^b)<0 && r*b!=a) r--; break;
                    case OP_MOD: if (b==0) { if (out_esz==8) ((int64_t*)out_data)[i]=0; else if (out_esz==4) ((int32_t*)out_data)[i]=0; else if (out_esz==2) ((int16_t*)out_data)[i]=0; else ((uint8_t*)out_data)[i]=0; ray_vec_set_null(vec,i,true); continue; }
                                r=a%b; if (r && (r^b)<0) r+=b; break;
                    default: r = 0; break;
                    }
                    if (out_esz == 8)      ((int64_t*)out_data)[i] = r;
                    else if (out_esz == 4)  ((int32_t*)out_data)[i] = (int32_t)r;
                    else if (out_esz == 2)  ((int16_t*)out_data)[i] = (int16_t)r;
                    else                    ((uint8_t*)out_data)[i] = (uint8_t)r;
                }
            } else {
                /* Slow path: check nulls per element */
                #define ISNULL_L(i) (l_atom_null || (lv && ray_vec_is_null(left, i)))
                #define ISNULL_R(i) (r_atom_null || (rv && ray_vec_is_null(right, i)))
                for (int64_t i = 0; i < len; i++) {
                    int64_t a = LA(i), b = RA(i);
                    int64_t r;
                    if (ISNULL_L(i) || ISNULL_R(i)) {
                        if (out_esz == 8)      ((int64_t*)out_data)[i] = 0;
                        else if (out_esz == 4)  ((int32_t*)out_data)[i] = 0;
                        else if (out_esz == 2)  ((int16_t*)out_data)[i] = 0;
                        else                    ((uint8_t*)out_data)[i] = 0;
                        ray_vec_set_null(vec, i, true);
                        continue;
                    }
                    switch (dag_opcode) {
                    case OP_ADD: r = (int64_t)((uint64_t)a + (uint64_t)b); break;
                    case OP_SUB: r = (int64_t)((uint64_t)a - (uint64_t)b); break;
                    case OP_MUL: r = (int64_t)((uint64_t)a * (uint64_t)b); break;
                    case OP_DIV: if (b==0) { ((int64_t*)out_data)[i]=0; ray_vec_set_null(vec,i,true); continue; }
                                r=a/b; if ((a^b)<0 && r*b!=a) r--; break;
                    case OP_MOD: if (b==0) { ((int64_t*)out_data)[i]=0; ray_vec_set_null(vec,i,true); continue; }
                                r=a%b; if (r && (r^b)<0) r+=b; break;
                    default: r = 0; break;
                    }
                    if (out_esz == 8)      ((int64_t*)out_data)[i] = r;
                    else if (out_esz == 4)  ((int32_t*)out_data)[i] = (int32_t)r;
                    else if (out_esz == 2)  ((int16_t*)out_data)[i] = (int16_t)r;
                    else                    ((uint8_t*)out_data)[i] = (uint8_t)r;
                }
                #undef ISNULL_L
                #undef ISNULL_R
            }
            #undef LA
            #undef RA
            #undef READ_INT
            #undef SCALAR_INT
            ray_release(e0);
            return vec;
        }
    }

    /* DAG executor — for F64 and comparisons */
    if (!force_boxed && dag_opcode > 0) {
        int is_idiv = (dag_opcode == OP_DIV || dag_opcode == OP_MOD);
        int is_cmp  = (dag_opcode >= OP_EQ && dag_opcode <= OP_GE);

        /* Classify operands: numeric/temporal vectors or scalars */
        int8_t lt = left_coll ? left->type : -(left->type);
        int8_t rt = right_coll ? right->type : -(right->type);
        #define IS_NUM_TYPE(t) ((t)==RAY_I64||(t)==RAY_F64||(t)==RAY_I32||(t)==RAY_I16|| \
                                (t)==RAY_U8||(t)==RAY_DATE||(t)==RAY_TIME||(t)==RAY_TIMESTAMP)
        int l_num_vec = left_coll && ray_is_vec(left) && IS_NUM_TYPE(lt);
        int r_num_vec = right_coll && ray_is_vec(right) && IS_NUM_TYPE(rt);
        int l_num_scalar = !left_coll && IS_NUM_TYPE(lt);
        int r_num_scalar = !right_coll && IS_NUM_TYPE(rt);
        #undef IS_NUM_TYPE

        int can_dag = (l_num_vec || r_num_vec) &&
                      (l_num_vec || l_num_scalar) && (r_num_vec || r_num_scalar);
        /* Null scalar atoms lose their null bit in DAG constants — use slow path */
        if (l_num_scalar && RAY_ATOM_IS_NULL(left)) can_dag = 0;
        if (r_num_scalar && RAY_ATOM_IS_NULL(right)) can_dag = 0;
        /* TODO: migrate expr.c to bitmap nulls and remove this bail-out.
         * DAG executor (expr.c) still uses sentinel-based null checks. */
        if (l_num_vec && (left->attrs & RAY_ATTR_HAS_NULLS)) can_dag = 0;
        if (r_num_vec && (right->attrs & RAY_ATTR_HAS_NULLS)) can_dag = 0;

        /* Div/mod: only I64×I64 (executor has floor-div semantics for I64) */
        if (is_idiv && !(lt == RAY_I64 && rt == RAY_I64)) can_dag = 0;
        /* Comparisons: same-type only (cross-type promotion loses type info) */
        if (is_cmp && lt != rt) can_dag = 0;
        /* Cross-type temporal: DAG promote() loses type tag (int+TIMESTAMP→I64 not TIMESTAMP) */
        {   int lt_temp = (lt==RAY_DATE||lt==RAY_TIME||lt==RAY_TIMESTAMP);
            int rt_temp = (rt==RAY_DATE||rt==RAY_TIME||rt==RAY_TIMESTAMP);
            if ((lt_temp || rt_temp) && lt != rt) can_dag = 0;
        }

        if (can_dag) {
                ray_graph_t* g = ray_graph_new(NULL);
                if (g) {
                    /* Build left operand node */
                    ray_op_t* lop = NULL;
                    if (l_num_scalar) {
                        if (left->type == -RAY_F64)
                            lop = ray_const_f64(g, left->f64);
                        else {
                            int64_t sv = as_i64(left);
                            lop = ray_const_i64(g, sv);
                            if (lop) lop->out_type = -(left->type);
                        }
                    } else {
                        lop = ray_const_vec(g, left);
                    }
                    ray_op_t* rop = NULL;
                    if (r_num_scalar) {
                        if (right->type == -RAY_F64)
                            rop = ray_const_f64(g, right->f64);
                        else {
                            int64_t sv = as_i64(right);
                            rop = ray_const_i64(g, sv);
                            if (rop) rop->out_type = -(right->type);
                        }
                    } else {
                        rop = ray_const_vec(g, right);
                    }
                    if (lop && rop) {
                        ray_op_t* root = ray_binop(g, dag_opcode, lop, rop);
                        if (root) {
                            /* For integer floor-division: ray_binop sets F64 output
                             * for OP_DIV; override to I64 for floor-div with null prop */
                            if (is_idiv) root->out_type = RAY_I64;
                            ray_t* result = ray_execute(g, root);
                            ray_graph_free(g);
                            if (result && !RAY_IS_ERR(result)) {
                                /* Restore temporal type tag if promote() collapsed it */
                                if (ray_is_vec(result) && result->type != out_type &&
                                    ray_elem_size(result->type) == ray_elem_size(out_type))
                                    result->type = out_type;
                                /* Floor-div post-pass (OP_DIV only) */
                                if (dag_opcode == OP_DIV && ray_is_vec(result) &&
                                    result->type == RAY_F64) {
                                    double* d = (double*)ray_data(result);
                                    for (int64_t fi = 0; fi < result->len; fi++)
                                        d[fi] = floor(d[fi]);
                                }
                                ray_release(e0);
                                return result;
                            }
                        } else { ray_graph_free(g); }
                    } else { ray_graph_free(g); }
                }
            }
        }
    /* SLOW PATH: per-element scalar loop (fallback for mixed types, temporal, etc.) */
    if (!force_boxed &&
        (out_type == RAY_I64 || out_type == RAY_F64 || out_type == RAY_I32 ||
         out_type == RAY_I16 || out_type == RAY_BOOL || out_type == RAY_U8 ||
         out_type == RAY_DATE || out_type == RAY_TIME || out_type == RAY_TIMESTAMP)) {
        ray_t* vec = ray_vec_new(out_type, len);
        if (RAY_IS_ERR(vec)) { ray_release(e0); return vec; }
        vec->len = len;
        store_typed_elem(vec, 0, e0);
        ray_release(e0);

        for (int64_t i = 1; i < len; i++) {
            int la = 0, ra = 0;
            ray_t* a = left_coll  ? collection_elem(left, i, &la)  : left;
            ray_t* b = right_coll ? collection_elem(right, i, &ra) : right;
            ray_t* elem = (RAY_IS_ERR(a) || RAY_IS_ERR(b))
                         ? ray_error("type", NULL) : fn(a, b);
            if (la) ray_release(a);
            if (ra) ray_release(b);
            if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
            store_typed_elem(vec, i, elem);
            ray_release(elem);
        }
        return vec;
    }

    /* Determine scalar int type for list+scalar coercion.
     * When a boxed list is combined with a scalar, integer results
     * are coerced to the scalar's integer type (K/q semantics). */
    int8_t scalar_int_type = 0;
    if (force_boxed) {
        ray_t* scalar = (!left_coll) ? left : (!right_coll ? right : NULL);
        if (scalar && ray_is_atom(scalar)) {
            int8_t st = scalar->type;
            if (st == -RAY_I16 || st == -RAY_I32 || st == -RAY_I64 || st == -RAY_U8)
                scalar_int_type = st;
        }
    }

    /* Coerce an integer atom to the scalar's integer type */
    #define COERCE_TO_SCALAR(elem) do { \
        if (scalar_int_type && ray_is_atom(elem) && elem->type != scalar_int_type && \
            elem->type != -RAY_F64 && is_numeric(elem)) { \
            int64_t _v = as_i64(elem); \
            ray_t* _coerced = make_typed_int(scalar_int_type, _v); \
            ray_release(elem); \
            elem = _coerced; \
        } \
    } while(0)

    /* Fallback: boxed list for non-numeric output or mixed-type input */
    COERCE_TO_SCALAR(e0);
    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { ray_release(e0); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    out[0] = e0;  /* first element already computed */

    for (int64_t i = 1; i < len; i++) {
        int la = 0, ra = 0;
        ray_t* a = left_coll  ? collection_elem(left, i, &la)  : left;
        ray_t* b = right_coll ? collection_elem(right, i, &ra) : right;
        ray_t* elem;
        if (RAY_IS_ERR(a) || RAY_IS_ERR(b)) {
            elem = ray_error("type", NULL);
        } else if (is_collection(a) || is_collection(b)) {
            /* Recursive auto-map when list element is itself a collection */
            elem = atomic_map_binary(fn, a, b);
        } else {
            elem = fn(a, b);
        }
        if (la) ray_release(a);
        if (ra) ray_release(b);
        if (RAY_IS_ERR(elem)) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            ray_release(result);
            return elem;
        }
        COERCE_TO_SCALAR(elem);
        out[i] = elem;
    }
    #undef COERCE_TO_SCALAR
    return result;
}

/* Map a unary function element-wise over a collection.
 * Produces typed vectors when output is numeric/bool, boxed lists otherwise. */
ray_t* atomic_map_unary(ray_unary_fn fn, ray_t* arg) {
    if (!is_collection(arg)) return fn(arg);

    int64_t len = ray_len(arg);

    if (len == 0) {
        /* Empty — fabricate a zero atom of the element type and run
         * `fn` to learn the output type; fall back to I64 if the
         * probe can't resolve a typed atom. */
        ray_t* z = zero_atom_for_elem_type(arg);
        ray_t* probe = (z && !RAY_IS_ERR(z)) ? fn(z) : NULL;
        if (z) ray_release(z);
        if (probe && !RAY_IS_ERR(probe) && probe->type < 0) {
            int8_t t = (int8_t)(-probe->type);
            ray_release(probe);
            return ray_vec_new(t, 0);
        }
        if (probe && !RAY_IS_ERR(probe)) ray_release(probe);
        return ray_vec_new(RAY_I64, 0);
    }

    /* Probe first element to determine output type */
    int alloc0 = 0;
    ray_t* e0_in = collection_elem(arg, 0, &alloc0);
    ray_t* e0 = RAY_IS_ERR(e0_in) ? e0_in : fn(e0_in);
    if (alloc0) ray_release(e0_in);
    if (RAY_IS_ERR(e0)) return e0;

    int8_t out_type = -(e0->type);

    /* Try typed vector path for numeric/bool/temporal output */
    if (out_type == RAY_I64 || out_type == RAY_F64 || out_type == RAY_I32 ||
        out_type == RAY_I16 || out_type == RAY_BOOL || out_type == RAY_U8 ||
        out_type == RAY_DATE || out_type == RAY_TIME || out_type == RAY_TIMESTAMP) {
        ray_t* vec = ray_vec_new(out_type, len);
        if (RAY_IS_ERR(vec)) { ray_release(e0); return vec; }
        vec->len = len;
        store_typed_elem(vec, 0, e0);
        ray_release(e0);

        for (int64_t i = 1; i < len; i++) {
            int alloc = 0;
            ray_t* e = collection_elem(arg, i, &alloc);
            ray_t* elem = RAY_IS_ERR(e) ? e : fn(e);
            if (alloc) ray_release(e);
            if (RAY_IS_ERR(elem)) { ray_release(vec); return elem; }
            store_typed_elem(vec, i, elem);
            ray_release(elem);
        }
        return vec;
    }

    /* Fallback: boxed list for non-numeric output */
    ray_t* result = ray_alloc(len * sizeof(ray_t*));
    if (!result) { ray_release(e0); return ray_error("oom", NULL); }
    result->type = RAY_LIST;
    result->len = len;
    ray_t** out = (ray_t**)ray_data(result);
    out[0] = e0;

    for (int64_t i = 1; i < len; i++) {
        int alloc = 0;
        ray_t* e = collection_elem(arg, i, &alloc);
        ray_t* elem = RAY_IS_ERR(e) ? e : fn(e);
        if (alloc) ray_release(e);
        if (RAY_IS_ERR(elem)) {
            for (int64_t j = 0; j < i; j++) ray_release(out[j]);
            ray_release(result);
            return elem;
        }
        out[i] = elem;
    }
    return result;
}

/* ══════════════════════════════════════════
 * Higher-order functions: map, pmap, fold, scan, filter, apply
 * ══════════════════════════════════════════ */

/* Helper: call a function object with 1 arg, returning result.
 * Handles UNARY, BINARY, LAMBDA types. Does not release fn or arg. */
ray_t* call_fn1(ray_t* fn, ray_t* arg) {
    if (fn_is_restricted(fn)) return ray_error("access", "restricted");
    if (fn->type == RAY_UNARY) {
        ray_unary_fn f = (ray_unary_fn)(uintptr_t)fn->i64;
        ray_t* a = arg;
        bool owned = false;
        if (!(fn->attrs & RAY_FN_LAZY_AWARE) && ray_is_lazy(a)) {
            ray_retain(a);               /* give materialise its own ref; arg is borrowed */
            a = ray_lazy_materialize(a); /* consumes the retain we just added */
            if (!a || RAY_IS_ERR(a)) return a ? a : ray_error("type", NULL);
            owned = true;
        }
        ray_t* r;
        if ((fn->attrs & RAY_FN_ATOMIC) && is_collection(a))
            r = atomic_map_unary(f, a);
        else
            r = f(a);
        if (owned) ray_release(a);
        return r;
    }
    if (fn->type == RAY_LAMBDA) {
        ray_t* args[1] = { arg };
        return call_lambda(fn, args, 1);
    }
    return ray_error("type", NULL);
}

/* Helper: call a function object with 2 args. Does not release fn or args. */
ray_t* call_fn2(ray_t* fn, ray_t* a, ray_t* b) {
    if (fn_is_restricted(fn)) return ray_error("access", "restricted");
    if (fn->type == RAY_BINARY) {
        ray_binary_fn f = (ray_binary_fn)(uintptr_t)fn->i64;
        ray_t* la = a;
        ray_t* lb = b;
        bool owned_a = false, owned_b = false;
        if (!(fn->attrs & RAY_FN_LAZY_AWARE)) {
            if (ray_is_lazy(la)) {
                ray_retain(la);               /* give materialise its own ref; a is borrowed */
                la = ray_lazy_materialize(la); /* consumes the retain we just added */
                if (!la || RAY_IS_ERR(la)) return la ? la : ray_error("type", NULL);
                owned_a = true;
            }
            if (ray_is_lazy(lb)) {
                ray_retain(lb);               /* give materialise its own ref; b is borrowed */
                lb = ray_lazy_materialize(lb); /* consumes the retain we just added */
                if (!lb || RAY_IS_ERR(lb)) {
                    if (owned_a) ray_release(la);
                    return lb ? lb : ray_error("type", NULL);
                }
                owned_b = true;
            }
        }
        ray_t* r;
        if ((fn->attrs & RAY_FN_ATOMIC) && (is_collection(la) || is_collection(lb)))
            r = atomic_map_binary(f, la, lb);
        else
            r = f(la, lb);
        if (owned_a) ray_release(la);
        if (owned_b) ray_release(lb);
        return r;
    }
    if (fn->type == RAY_LAMBDA) {
        ray_t* args[2] = { a, b };
        return call_lambda(fn, args, 2);
    }
    if (fn->type == RAY_UNARY) {
        /* Partial application not supported, just call with first arg */
        ray_unary_fn f = (ray_unary_fn)(uintptr_t)fn->i64;
        return f(a);
    }
    return ray_error("type", NULL);
}


/* ══════════════════════════════════════════
 * Sorting builtins
 * ══════════════════════════════════════════ */

/* Reorder vector elements by an index array */
ray_t* gather_by_idx(ray_t* vec, int64_t* idx, int64_t n) {
    int8_t type = vec->type;

    /* Check nulls once — resolve through slices */
    bool has_nulls = (vec->attrs & RAY_ATTR_HAS_NULLS) ||
                     ((vec->attrs & RAY_ATTR_SLICE) && vec->slice_parent &&
                      (vec->slice_parent->attrs & RAY_ATTR_HAS_NULLS));

    if (type == RAY_STR) {
        ray_t* result = ray_vec_new(type, n);
        if (RAY_IS_ERR(result)) return result;
        result->len = n;
        for (int64_t i = 0; i < n; i++) {
            if (has_nulls && ray_vec_is_null(vec, idx[i])) {
                result = ray_str_vec_set(result, i, "", 0);
                ray_vec_set_null(result, i, true);
            } else {
                size_t slen;
                const char* s = ray_str_vec_get(vec, idx[i], &slen);
                result = ray_str_vec_set(result, i, s ? s : "", s ? slen : 0);
            }
        }
        return result;
    }

    /* RAY_SYM: use adaptive width, create with matching width */
    if (type == RAY_SYM) {
        uint8_t w = vec->attrs & RAY_SYM_W_MASK;
        ray_t* result = ray_sym_vec_new(w, n);
        if (RAY_IS_ERR(result)) return result;
        result->len = n;
        uint8_t esz = (uint8_t)RAY_SYM_ELEM(w);
        char* src = (char*)ray_data(vec);
        char* dst = (char*)ray_data(result);
        switch (esz) {
        case 8: for (int64_t i = 0; i < n; i++) memcpy(dst + i*8, src + idx[i]*8, 8); break;
        case 4: for (int64_t i = 0; i < n; i++) memcpy(dst + i*4, src + idx[i]*4, 4); break;
        case 2: for (int64_t i = 0; i < n; i++) memcpy(dst + i*2, src + idx[i]*2, 2); break;
        case 1: for (int64_t i = 0; i < n; i++) dst[i] = src[idx[i]]; break;
        default: for (int64_t i = 0; i < n; i++) memcpy(dst + i*esz, src + idx[i]*esz, esz); break;
        }
        if (vec->sym_dict) {
            ray_retain(vec->sym_dict);
            result->sym_dict = vec->sym_dict;
        }
        if (has_nulls) {
            for (int64_t i = 0; i < n; i++)
                if (ray_vec_is_null(vec, idx[i]))
                    ray_vec_set_null(result, i, true);
        }
        return result;
    }

    /* LIST: pointer gather with retain */
    if (type == RAY_LIST) {
        ray_t* result = ray_alloc(n * sizeof(ray_t*));
        if (!result || RAY_IS_ERR(result)) return result ? result : ray_error("oom", NULL);
        result->type = type;
        result->len = n;
        ray_t** src_ptrs = (ray_t**)ray_data(vec);
        ray_t** dst_ptrs = (ray_t**)ray_data(result);
        for (int64_t i = 0; i < n; i++) {
            dst_ptrs[i] = src_ptrs[idx[i]];
            if (dst_ptrs[i]) ray_retain(dst_ptrs[i]);
        }
        return result;
    }

    ray_t* result = ray_vec_new(type, n);
    if (RAY_IS_ERR(result)) return result;
    result->len = n;
    uint8_t esz = ray_type_sizes[type];
    char* src = (char*)ray_data(vec);
    char* dst = (char*)ray_data(result);
    /* Typed gather — compiler constant esz enables vectorization, alias-safe */
    switch (esz) {
    case 8: for (int64_t i = 0; i < n; i++) memcpy(dst + i*8, src + idx[i]*8, 8); break;
    case 4: for (int64_t i = 0; i < n; i++) memcpy(dst + i*4, src + idx[i]*4, 4); break;
    case 2: for (int64_t i = 0; i < n; i++) memcpy(dst + i*2, src + idx[i]*2, 2); break;
    case 1: for (int64_t i = 0; i < n; i++) dst[i] = src[idx[i]]; break;
    default: for (int64_t i = 0; i < n; i++) memcpy(dst + i*esz, src + idx[i]*esz, esz); break;
    case 16: for (int64_t i = 0; i < n; i++) memcpy(dst + i*16, src + idx[i]*16, 16); break;
    }

    /* Propagate null bitmap */
    if (has_nulls) {
        for (int64_t i = 0; i < n; i++)
            if (ray_vec_is_null(vec, idx[i]))
                ray_vec_set_null(result, i, true);
    }

    return result;
}

/* ══════════════════════════════════════════
 * Table construction and access
 * ══════════════════════════════════════════ */

/* (list v1 v2 ...) — package args into a list */
ray_t* ray_list_fn(ray_t** args, int64_t n) {
    ray_t* result = ray_alloc(n * sizeof(ray_t*));
    if (!result) return ray_error("oom", NULL);
    result->type = RAY_LIST;
    result->len = n;
    ray_t** out = (ray_t**)ray_data(result);
    for (int64_t i = 0; i < n; i++) {
        ray_retain(args[i]);
        out[i] = args[i];
    }
    return result;
}

/* (table [col_names] (list col1 col2 ...)) — build a RAY_TABLE */
ray_t* ray_table_fn(ray_t* names, ray_t* cols) {
    ray_t *_bxn = NULL, *_bxc = NULL;
    names = unbox_vec_arg(names, &_bxn);
    if (RAY_IS_ERR(names)) return names;
    cols = unbox_vec_arg(cols, &_bxc);
    if (RAY_IS_ERR(cols)) { if (_bxn) ray_release(_bxn); return cols; }
    if (!is_list(names) || !is_list(cols)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("type", NULL); }
    int64_t ncols = ray_len(names);
    if (ray_len(cols) != ncols) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("domain", NULL); }

    ray_t** name_elems = (ray_t**)ray_data(names);
    ray_t** col_elems = (ray_t**)ray_data(cols);
    int64_t expected_rows = -1;

    ray_t* tbl = ray_table_new(ncols);
    if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }

    for (int64_t i = 0; i < ncols; i++) {
        if (name_elems[i]->type != -RAY_SYM)
            { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("type", NULL); }
        int64_t name_id = name_elems[i]->i64;

        /* Convert Rayfall list (or typed vec) to typed column vector */
        ray_t* col_src = col_elems[i];

        /* Single atom → wrap in a 1-element vector */
        ray_t* atom_wrap = NULL;
        if (ray_is_atom(col_src) && col_src->type != -RAY_SYM) {
            int8_t atype = -col_src->type;
            if (atype == RAY_GUID) {
                atom_wrap = ray_vec_new(RAY_GUID, 1);
                if (!RAY_IS_ERR(atom_wrap) && col_src->obj)
                    memcpy(ray_data(atom_wrap), ray_data(col_src->obj), 16);
                if (!RAY_IS_ERR(atom_wrap)) atom_wrap->len = 1;
            } else if (atype == RAY_TIMESTAMP || atype == RAY_I64 || atype == RAY_SYM) {
                atom_wrap = ray_vec_new(atype, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((int64_t*)ray_data(atom_wrap))[0] = col_src->i64; atom_wrap->len = 1; }
            } else if (atype == RAY_F64) {
                atom_wrap = ray_vec_new(RAY_F64, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((double*)ray_data(atom_wrap))[0] = col_src->f64; atom_wrap->len = 1; }
            } else if (atype == RAY_DATE || atype == RAY_TIME || atype == RAY_I32) {
                atom_wrap = ray_vec_new(atype, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((int32_t*)ray_data(atom_wrap))[0] = col_src->i32; atom_wrap->len = 1; }
            } else if (atype == RAY_BOOL) {
                atom_wrap = ray_vec_new(RAY_BOOL, 1);
                if (!RAY_IS_ERR(atom_wrap)) { ((uint8_t*)ray_data(atom_wrap))[0] = col_src->b8; atom_wrap->len = 1; }
            }
            if (atom_wrap && !RAY_IS_ERR(atom_wrap)) col_src = atom_wrap;
        }

        /* If the column is already a typed vector, use it directly */
        if (ray_is_vec(col_src)) {
            int64_t nrows = ray_len(col_src);
            if (expected_rows < 0) expected_rows = nrows;
            else if (nrows != expected_rows)
                { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("domain", NULL); }
            ray_retain(col_src);
            tbl = ray_table_add_col(tbl, name_id, col_src);
            ray_release(col_src);
            if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }
            continue;
        }

        if (!is_list(col_src))
            { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("type", NULL); }
        int64_t nrows = ray_len(col_src);

        /* Validate all columns have consistent row count */
        if (expected_rows < 0) expected_rows = nrows;
        else if (nrows != expected_rows)
            { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return ray_error("domain", NULL); }

        ray_t** row_elems = (ray_t**)ray_data(col_src);

        /* If the LIST contains non-atom values (e.g. nested vectors for an
         * embedding column), store the LIST as the column directly rather
         * than trying to build a typed vector from non-atomic elements. */
        if (nrows > 0 && row_elems[0] && !ray_is_atom(row_elems[0])) {
            ray_retain(col_src);
            tbl = ray_table_add_col(tbl, name_id, col_src);
            ray_release(col_src);
            if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }
            continue;
        }

        /* Determine column type from elements (scan for mixed I64/F64 → F64) */
        int8_t col_type = RAY_I64;
        if (nrows > 0) {
            if (row_elems[0]->type == -RAY_F64) col_type = RAY_F64;
            else if (row_elems[0]->type == -RAY_BOOL) col_type = RAY_BOOL;
            else if (row_elems[0]->type == -RAY_SYM) col_type = RAY_SYM;
            else if (row_elems[0]->type == -RAY_STR) col_type = RAY_STR;
            else if (row_elems[0]->type == -RAY_GUID) col_type = RAY_GUID;
            else if (row_elems[0]->type == -RAY_TIMESTAMP) col_type = RAY_TIMESTAMP;
            else if (row_elems[0]->type == -RAY_DATE) col_type = RAY_DATE;
            else if (row_elems[0]->type == -RAY_TIME) col_type = RAY_TIME;
            /* RAY_CHAR removed — char atoms are now -RAY_STR */
        }
        /* Promote I64 → F64 if any element is F64 */
        if (col_type == RAY_I64) {
            for (int64_t j = 0; j < nrows; j++) {
                if (row_elems[j]->type == -RAY_F64) { col_type = RAY_F64; break; }
            }
        }

        ray_t* col_vec = ray_vec_new(col_type, nrows);
        if (RAY_IS_ERR(col_vec))
            { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return col_vec; }

        for (int64_t j = 0; j < nrows; j++) {
            if (col_type == RAY_STR) {
                if (row_elems[j]->type != -RAY_STR) {
                    ray_release(col_vec); ray_release(tbl);
                    if (_bxn) ray_release(_bxn);
                    if (_bxc) ray_release(_bxc);
                    return ray_error("type", NULL);
                }
                const char *sptr = ray_str_ptr(row_elems[j]);
                size_t slen = ray_str_len(row_elems[j]);
                col_vec = ray_str_vec_append(col_vec, sptr, slen);
            } else if (col_type == RAY_GUID) {
                if (row_elems[j]->type != -RAY_GUID || !row_elems[j]->obj) {
                    ray_release(col_vec); ray_release(tbl);
                    if (_bxn) ray_release(_bxn);
                    if (_bxc) ray_release(_bxc);
                    return ray_error("type", NULL);
                }
                col_vec = ray_vec_append(col_vec, ray_data(row_elems[j]->obj));
            } else {
                /* Validate each element matches the column type (allow I64→F64 promotion) */
                int type_ok = (row_elems[j]->type == -col_type);
                if (!type_ok && col_type == RAY_F64 && row_elems[j]->type == -RAY_I64) type_ok = 1;
                if (!type_ok) {
                    ray_release(col_vec); ray_release(tbl);
                    if (_bxn) ray_release(_bxn);
                    if (_bxc) ray_release(_bxc);
                    return ray_error("type", NULL);
                }
                void* val_ptr;
                double promoted;
                if (col_type == RAY_F64 && row_elems[j]->type == -RAY_I64) {
                    promoted = (double)row_elems[j]->i64;
                    val_ptr = &promoted;
                } else if (col_type == RAY_I64) val_ptr = &row_elems[j]->i64;
                else if (col_type == RAY_F64) val_ptr = &row_elems[j]->f64;
                else if (col_type == RAY_BOOL) val_ptr = &row_elems[j]->b8;
                else val_ptr = &row_elems[j]->i64; /* SYM/TIMESTAMP/DATE/TIME stored as i64 */
                col_vec = ray_vec_append(col_vec, val_ptr);
            }
            if (RAY_IS_ERR(col_vec))
                { ray_release(tbl); if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return col_vec; }
        }

        tbl = ray_table_add_col(tbl, name_id, col_vec);
        ray_release(col_vec);
        if (RAY_IS_ERR(tbl)) { if (_bxn) ray_release(_bxn); if (_bxc) ray_release(_bxc); return tbl; }
    }

    if (_bxn) ray_release(_bxn);
    if (_bxc) ray_release(_bxc);
    return tbl;
}

/* (key dict/table) — return keys vector */
ray_t* ray_key_fn(ray_t* x) {
    if (x->type == RAY_DICT) {
        ray_t* keys = ray_dict_keys(x);
        if (!keys) return ray_error("type", NULL);
        ray_retain(keys);
        return keys;
    }
    if (x->type != RAY_TABLE) return ray_error("type", NULL);
    int64_t ncols = ray_table_ncols(x);
    ray_t* vec = ray_vec_new(RAY_SYM, ncols);
    if (RAY_IS_ERR(vec)) return vec;
    vec->len = ncols;
    int64_t* out = (int64_t*)ray_data(vec);
    for (int64_t i = 0; i < ncols; i++)
        out[i] = ray_table_col_name(x, i);
    return vec;
}

/* (value dict/table) — extract values */
ray_t* ray_value_fn(ray_t* x) {
    /* Table: return list of column vectors */
    if (x->type == RAY_TABLE) {
        /* Table cols slot is a RAY_LIST already — return a fresh copy. */
        int64_t ncols = ray_table_ncols(x);
        ray_t* result = ray_list_new(ncols);
        if (!result || RAY_IS_ERR(result)) return result ? result : ray_error("oom", NULL);
        for (int64_t i = 0; i < ncols; i++) {
            ray_t* c = ray_table_get_col_idx(x, i);
            result = ray_list_append(result, c);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    if (x->type != RAY_DICT) return ray_error("type", NULL);
    ray_t* vals = ray_dict_vals(x);
    if (!vals) return ray_error("type", NULL);
    ray_retain(vals);
    return vals;
}



/* ray_lang_print, fmt_interpolate, ray_println_fn, ray_show_fn, ray_format_fn,
 * ray_resolve_fn, ray_timeit_fn, ray_exit_fn, resolve_type_name,
 * ray_read_csv_fn, ray_write_csv_fn, cast_match, ray_cast_fn, ray_type_fn,
 * ray_read_file_fn, ray_load_file_fn, ray_write_file_fn
 * moved to ops/builtins.c */

/* ══════════════════════════════════════════
 * Special forms: set, let, if, do
 * ══════════════════════════════════════════ */

/* (set name value) — bind in global env. Receives unevaluated args. */
ray_t* ray_set_fn(ray_t* name_obj, ray_t* val_expr) {
    if (name_obj->type != -RAY_SYM)
        return ray_error("type", NULL);
    ray_t* val = ray_eval(val_expr);
    if (RAY_IS_ERR(val)) return val;
    /* Materialize lazy handles before binding */
    if (ray_is_lazy(val))
        val = ray_lazy_materialize(val);
    if (RAY_IS_ERR(val)) return val;
    ray_err_t err = ray_env_set(name_obj->i64, val);
    if (err != RAY_OK) {
        ray_release(val);
        return ray_error(ray_err_code_str(err), NULL);
    }
    return val;  /* set returns the value */
}

/* (let name value) — bind in local scope. Receives unevaluated args. */
ray_t* ray_let_fn(ray_t* name_obj, ray_t* val_expr) {
    if (name_obj->type != -RAY_SYM)
        return ray_error("type", NULL);
    ray_t* val = ray_eval(val_expr);
    if (RAY_IS_ERR(val)) return val;
    /* Materialize lazy handles before binding */
    if (ray_is_lazy(val))
        val = ray_lazy_materialize(val);
    if (RAY_IS_ERR(val)) return val;
    ray_err_t err = ray_env_set_local(name_obj->i64, val);
    if (err != RAY_OK) { ray_release(val); return ray_error(ray_err_code_str(err), NULL); }
    return val;
}

/* (if cond then else?) — conditional. Receives unevaluated args. */
ray_t* ray_cond_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    ray_t* cond = ray_eval(args[0]);
    if (RAY_IS_ERR(cond)) return cond;
    /* Materialize lazy handles before testing truthiness */
    if (ray_is_lazy(cond))
        cond = ray_lazy_materialize(cond);
    if (RAY_IS_ERR(cond)) return cond;
    int truthy = is_truthy(cond);
    ray_release(cond);
    if (truthy) return ray_eval(args[1]);
    if (n >= 3) return ray_eval(args[2]);
    /* No else branch: return 0 */
    return make_i64(0);
}

/* (do expr1 expr2 ...) — evaluate in sequence, return last. Pushes local scope. */
ray_t* ray_do_fn(ray_t** args, int64_t n) {
    if (n == 0) return make_i64(0);
    if (ray_env_push_scope() != RAY_OK) return ray_error("oom", NULL);
    ray_t* result = NULL;
    for (int64_t i = 0; i < n; i++) {
        if (result) ray_release(result);
        result = ray_eval(args[i]);
        if (RAY_IS_ERR(result)) {
            ray_env_pop_scope();
            return result;
        }
    }
    ray_env_pop_scope();
    return result;
}

/* ══════════════════════════════════════════
 * Lambda functions
 * ══════════════════════════════════════════ */

/* (fn [params...] body...) — create a lambda object.
 * Stores params list and body expressions in data area. */
ray_t* ray_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);
    /* args[0] = param vector (list of name symbols), args[1..n-1] = body exprs */
    ray_t* params_list = args[0];

    /* Reject lambda parameters named under the reserved `.` namespace.
     * Even though the bytecode VM resolves them to slot indices rather
     * than env entries, a user-defined fn with `.sys.gc` as a parameter
     * would silently override the builtin inside the body via the
     * compile-time name→slot map — that counts as shadowing and is
     * disallowed for the same reason `(let .sys.gc ...)` is.  Lambda
     * param lists are SYM *vectors* (not RAY_LISTs): `[a b c]` of all
     * syms is stored as a flat i64 sym-id array. */
    if (params_list) {
        int64_t nparams = ray_len(params_list);
        if (params_list->type == RAY_SYM) {
            int64_t* ids = (int64_t*)ray_data(params_list);
            for (int64_t i = 0; i < nparams; i++)
                if (ray_sym_is_reserved(ids[i]))
                    return ray_error("reserve",
                        "lambda parameter '%s' is in the reserved namespace",
                        ray_str_ptr(ray_sym_str(ids[i])));
        } else if (params_list->type == RAY_LIST) {
            ray_t** pelems = (ray_t**)ray_data(params_list);
            for (int64_t i = 0; i < nparams; i++) {
                ray_t* p = pelems[i];
                if (p && p->type == -RAY_SYM && ray_sym_is_reserved(p->i64))
                    return ray_error("reserve",
                        "lambda parameter '%s' is in the reserved namespace",
                        ray_str_ptr(ray_sym_str(p->i64)));
            }
        }
    }

    /* Create lambda object with space for 7 slots:
     * [0] params, [1] body, [2] bytecode, [3] constants, [4] n_locals,
     * [5] nfo (source location), [6] dbg (debug metadata) */
    ray_t* lambda = ray_alloc(7 * sizeof(ray_t*));
    if (!lambda) return ray_error("oom", NULL);
    lambda->type = RAY_LAMBDA;
    lambda->attrs = 0;
    lambda->len = 0;

    /* Store params list */
    ray_retain(params_list);
    LAMBDA_PARAMS(lambda) = params_list;

    /* Build body list: wrap body expressions in a RAY_LIST */
    int64_t body_count = n - 1;
    ray_t* body = ray_alloc(body_count * sizeof(ray_t*));
    if (!body) {
        ray_release(params_list);
        ray_release(lambda);
        return ray_error("oom", NULL);
    }
    body->type = RAY_LIST;
    body->len = body_count;
    ray_t** body_elems = (ray_t**)ray_data(body);
    for (int64_t i = 0; i < body_count; i++) {
        ray_retain(args[i + 1]);
        body_elems[i] = args[i + 1];
    }
    LAMBDA_BODY(lambda) = body;

    /* Clear compiled slots */
    LAMBDA_BC(lambda) = NULL;
    LAMBDA_CONSTS(lambda) = NULL;
    LAMBDA_NLOCALS(lambda) = 0;

    /* Attach source location info from current eval context */
    if (g_eval_nfo) {
        LAMBDA_NFO(lambda) = g_eval_nfo;
        ray_retain(g_eval_nfo);
    } else {
        LAMBDA_NFO(lambda) = NULL;
    }
    LAMBDA_DBG(lambda) = NULL;

    return lambda;
}

/* Build a [span_i64, filename, fn_name, source] frame from a resolved span
 * and append it to g_error_trace.  Shared by the bytecode and eval paths. */
static void append_error_frame(ray_t* nfo, ray_span_t span) {
    if (span.id == 0) return;

    ray_t* frame = ray_alloc(4 * sizeof(ray_t*));
    if (!frame || RAY_IS_ERR(frame)) return;
    frame->type = RAY_LIST;
    frame->len = 4;
    ray_t** fe = (ray_t**)ray_data(frame);

    fe[0] = ray_i64(span.id);
    if (nfo && NFO_FILENAME(nfo)) {
        fe[1] = NFO_FILENAME(nfo);
        ray_retain(fe[1]);
    } else {
        fe[1] = ray_str("<unknown>", 9);
    }
    fe[2] = NULL;
    if (nfo && NFO_SOURCE(nfo)) {
        fe[3] = NFO_SOURCE(nfo);
        ray_retain(fe[3]);
    } else {
        fe[3] = ray_str("", 0);
    }

    if (!g_error_trace) {
        g_error_trace = ray_alloc(sizeof(ray_t*));
        if (!g_error_trace) { ray_release(frame); return; }
        g_error_trace->type = RAY_LIST;
        g_error_trace->len = 1;
        ((ray_t**)ray_data(g_error_trace))[0] = frame;
    } else {
        g_error_trace = ray_list_append(g_error_trace, frame);
        ray_release(frame);
    }
}

/* Build a single error trace frame from a lambda's debug/nfo info at the given
 * bytecode IP. */
static void add_error_frame(ray_t* fn, int32_t ip) {
    if (!fn || fn->type != RAY_LAMBDA) return;
    ray_t* dbg = LAMBDA_DBG(fn);
    ray_t* nfo = LAMBDA_NFO(fn);
    if (!dbg && !nfo) return;

    ray_span_t span = {0};
    if (dbg) span = ray_bc_dbg_get(dbg, ip);
    append_error_frame(nfo, span);
}

/* Add error frame from eval context (nfo + AST node) for call-site errors. */
static void add_eval_error_frame(ray_t* nfo, ray_t* node) {
    if (!nfo || !node) return;
    append_error_frame(nfo, ray_nfo_get(nfo, node));
}

/* Execute compiled bytecode for a lambda. */
static ray_t* vm_exec(ray_t* lambda, ray_t** call_args, int64_t argc);

/* Call a lambda: compile on first call, then execute bytecode. */
ray_t* call_lambda(ray_t* lambda, ray_t** call_args, int64_t argc) {
    /* Lazy compilation on first call */
    if (!LAMBDA_IS_COMPILED(lambda)) {
        ray_compile(lambda);
    }

    /* If compilation succeeded, run bytecode; otherwise fall back to tree-walk */
    if (LAMBDA_IS_COMPILED(lambda)) {
        return vm_exec(lambda, call_args, argc);
    }

    /* Fallback: tree-walking interpreter */
    ray_t* params_list = LAMBDA_PARAMS(lambda);
    ray_t* body = LAMBDA_BODY(lambda);

    int64_t param_count = ray_len(params_list);

    if (argc != param_count)
        return ray_error("arity", "expected %" PRId64 " args, got %" PRId64, param_count, argc);

    if (ray_env_push_scope() != RAY_OK) return ray_error("oom", NULL);

    /* Bind 'self' to the current lambda for recursion */
    {
        static int64_t self_sym_id = -1;
        if (self_sym_id < 0) self_sym_id = ray_sym_intern("self", 4);
        ray_env_set_local(self_sym_id, lambda);
    }

    int64_t* param_ids = (int64_t*)ray_data(params_list);
    for (int64_t i = 0; i < param_count && i < argc; i++) {
        (void)ray_env_set_local(param_ids[i], call_args[i]);
    }

    int64_t body_count = ray_len(body);
    ray_t** body_exprs = (ray_t**)ray_data(body);
    ray_t* result = NULL;
    for (int64_t i = 0; i < body_count; i++) {
        if (result) ray_release(result);
        result = ray_eval(body_exprs[i]);
        if (RAY_IS_ERR(result)) {
            ray_env_pop_scope();
            return result;
        }
    }

    ray_env_pop_scope();
    return result;
}

/* ══════════════════════════════════════════
 * Stack-based VM executor (computed goto, frame-based)
 * ══════════════════════════════════════════ */

static _Thread_local ray_vm_t *__VM = NULL;

static ray_t* vm_exec(ray_t* lambda, ray_t** call_args, int64_t argc) {
    /* Computed goto dispatch table */
    static void *dispatch[OP__COUNT] = {
        [OP_RET]        = &&op_ret,
        [OP_JMP]        = &&op_jmp,
        [OP_JMPF]       = &&op_jmpf,
        [OP_LOADCONST]  = &&op_loadconst,
        [OP_LOADENV]    = &&op_loadenv,
        [OP_STOREENV]   = &&op_storeenv,
        [OP_POP]        = &&op_pop,
        [OP_RESOLVE]    = &&op_resolve,
        [OP_CALL1]      = &&op_call1,
        [OP_CALL2]      = &&op_call2,
        [OP_CALLN]      = &&op_calln,
        [OP_CALLF]      = &&op_callf,
        [OP_CALLS]      = &&op_calls,
        [OP_CALLD]      = &&op_calld,
        [OP_DUP]        = &&op_dup,
        [OP_LOADCONST_W] = &&op_loadconst_w,
        [OP_RESOLVE_W]  = &&op_resolve_w,
        [OP_TRAP]       = &&op_trap,
        [OP_TRAP_END]   = &&op_trap_end,
    };

    /* Arity check before allocating VM state */
    {
        int64_t param_count = ray_len(LAMBDA_PARAMS(lambda));
        if (argc != param_count)
            return ray_error("arity", "expected %" PRId64 " args, got %" PRId64, param_count, argc);
    }

    ray_t *vm_block = ray_alloc(sizeof(ray_vm_t));
    if (!vm_block || RAY_IS_ERR(vm_block)) return ray_error("oom", NULL);
    ray_vm_t *vmp = (ray_vm_t *)ray_data(vm_block);
    memset(vmp, 0, sizeof(ray_vm_t));
    __VM = vmp;

#define vm (*vmp)

    /* Set up initial frame */
    vm.fn = lambda;
    ray_retain(lambda);
    int32_t n_locals = LAMBDA_NLOCALS(lambda);
    vm.fp = 0;
    vm.sp = n_locals;

    /* Bind parameters into local slots */
    int64_t param_count = ray_len(LAMBDA_PARAMS(lambda));
    for (int64_t i = 0; i < param_count && i < argc; i++) {
        ray_retain(call_args[i]);
        vm.ps[i] = call_args[i];
    }

    uint8_t *code = (uint8_t *)ray_data(LAMBDA_BC(lambda));
    ray_t **cpool = (ray_t **)ray_data(LAMBDA_CONSTS(lambda));
    int32_t ip = 0;
    ray_t *vm_err_obj = NULL;

#define DISPATCH() goto *dispatch[code[ip++]]
#define PUSH(v)    do { if (vm.sp >= VM_STACK_SIZE) goto vm_error_limit; vm.ps[vm.sp++] = (v); } while(0)
#define POP()      (vm.ps[--vm.sp])
#define PEEK()     (vm.ps[vm.sp - 1])
#define LOCAL(s)   (vm.ps[vm.fp + (s)])

    DISPATCH();

op_loadconst: {
    uint8_t idx = code[ip++];
    ray_t *val = cpool[idx];
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_loadconst_w: {
    uint16_t idx = (uint16_t)(code[ip] << 8) | code[ip + 1];
    ip += 2;
    ray_t *val = cpool[idx];
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_loadenv: {
    uint8_t slot = code[ip++];
    ray_t *val = LOCAL(slot);
    if (val) ray_retain(val);
    else val = make_i64(0);
    PUSH(val);
    DISPATCH();
}

op_storeenv: {
    uint8_t slot = code[ip++];
    ray_t *val = POP();
    if (LOCAL(slot)) ray_release(LOCAL(slot));
    LOCAL(slot) = val;
    DISPATCH();
}

op_pop: {
    if (vm.sp > vm.fp + n_locals) {
        ray_t *val = POP();
        if (val) ray_release(val);
    }
    DISPATCH();
}

op_dup: {
    ray_t *val = PEEK();
    ray_retain(val);
    PUSH(val);
    DISPATCH();
}

op_resolve: {
    uint8_t idx = code[ip++];
    ray_t *name_obj = cpool[idx];
    ray_t *val = ray_env_resolve(name_obj->i64);
    if (!val) goto vm_error_name;
    /* env_resolve returns an owned ref (rc >= 1); no extra retain needed.
     * It can also return a real error (e.g. nyi from a parted-target link
     * deref inside the dotted walker) — surface that as a VM error rather
     * than pushing it onto the stack as if it were a normal value. */
    if (RAY_IS_ERR(val)) { vm_err_obj = val; goto vm_error; }
    PUSH(val);
    DISPATCH();
}

op_resolve_w: {
    uint16_t idx = (uint16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ray_t *name_obj = cpool[idx];
    ray_t *val = ray_env_resolve(name_obj->i64);
    if (!val) goto vm_error_name;
    if (RAY_IS_ERR(val)) { vm_err_obj = val; goto vm_error; }
    PUSH(val);
    DISPATCH();
}

op_jmp: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ip += offset;
    if (offset < 0 && g_eval_interrupted) goto vm_error_limit;
    DISPATCH();
}

op_jmpf: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    ray_t *cond = POP();
    int truthy = is_truthy(cond);
    ray_release(cond);
    if (!truthy) ip += offset;
    DISPATCH();
}

op_call1: {
    ray_t *arg = POP();
    ray_t *fn_obj = POP();
    ray_unary_fn fn = (ray_unary_fn)(uintptr_t)fn_obj->i64;
    ray_t *result;
    /* Materialise lazy args for non-lazy-aware fns.
     * arg is an owned ref (POPped from VM stack); ray_lazy_materialize consumes
     * it internally — no extra ray_release needed here. */
    if (!(fn_obj->attrs & RAY_FN_LAZY_AWARE) && arg && ray_is_lazy(arg)) {
        arg = ray_lazy_materialize(arg); /* consumes owned ref; result is new owned ref */
        if (!arg || RAY_IS_ERR(arg)) {
            ray_release(fn_obj);
            vm_err_obj = arg ? arg : ray_error("type", NULL);
            goto vm_error;
        }
    }
    if (RAY_UNLIKELY(!arg || RAY_IS_NULL(arg))) {
        result = (fn == (ray_unary_fn)ray_nil_fn || fn == (ray_unary_fn)ray_type_fn)
                 ? fn(arg) : ray_error("type", NULL);
    } else if ((fn_obj->attrs & RAY_FN_ATOMIC) && arg->type >= 0)
        result = atomic_map_unary(fn, arg);
    else
        result = fn(arg);
    ray_release(arg);
    ray_release(fn_obj);
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
    PUSH(result);
    DISPATCH();
}

op_call2: {
    ray_t *right = POP();
    ray_t *left = POP();
    ray_t *fn_obj = POP();
    ray_binary_fn fn = (ray_binary_fn)(uintptr_t)fn_obj->i64;
    ray_t *result;
    /* Materialise lazy args for non-lazy-aware fns.
     * left/right are owned refs (POPped from VM stack); ray_lazy_materialize
     * consumes them internally — no extra ray_release needed after the call. */
    if (!(fn_obj->attrs & RAY_FN_LAZY_AWARE)) {
        if (left && ray_is_lazy(left)) {
            left = ray_lazy_materialize(left); /* consumes owned ref */
            if (!left || RAY_IS_ERR(left)) {
                ray_release(right); ray_release(fn_obj);
                vm_err_obj = left ? left : ray_error("type", NULL);
                goto vm_error;
            }
        }
        if (right && ray_is_lazy(right)) {
            right = ray_lazy_materialize(right); /* consumes owned ref */
            if (!right || RAY_IS_ERR(right)) {
                ray_release(left); ray_release(fn_obj);
                vm_err_obj = right ? right : ray_error("type", NULL);
                goto vm_error;
            }
        }
    }
    if (RAY_UNLIKELY(!left || !right || RAY_IS_NULL(left) || RAY_IS_NULL(right))) {
        result = (fn == (ray_binary_fn)ray_eq_fn || fn == (ray_binary_fn)ray_neq_fn)
                 ? fn(left, right) : ray_error("type", NULL);
    /* Fast path: atoms have negative type — skip collection check entirely.
     * Only call is_collection when at least one arg has type >= 0 (vector/list). */
    } else if ((fn_obj->attrs & RAY_FN_ATOMIC) && (left->type >= 0 || right->type >= 0))
        result = atomic_map_binary_op(fn, RAY_FN_OPCODE(fn_obj), left, right);
    else
        result = fn(left, right);
    ray_release(left);
    ray_release(right);
    ray_release(fn_obj);
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
    PUSH(result);
    DISPATCH();
}

op_calln: {
    uint8_t n = code[ip++];
    if (n > 64) goto vm_error;
    ray_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    ray_t *fn_obj = POP();
    ray_vary_fn fn = (ray_vary_fn)(uintptr_t)fn_obj->i64;
    ray_t *result = fn(fn_args, n);
    for (int32_t i = 0; i < n; i++)
        ray_release(fn_args[i]);
    ray_release(fn_obj);
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
    PUSH(result);
    DISPATCH();
}

op_callf: {
    uint8_t n = code[ip++];
    if (n > 64) goto vm_error;
    ray_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    ray_t *fn_obj = POP();

    /* Compiled lambda: push frame, switch to callee bytecode */
    if (fn_obj->type == RAY_LAMBDA) {
        if (!LAMBDA_IS_COMPILED(fn_obj))
            ray_compile(fn_obj);

        if (LAMBDA_IS_COMPILED(fn_obj)) {
            /* All checks before any VM state mutation.
             * Stack limits take priority over arity (safety first). */
            int64_t pcnt = ray_len(LAMBDA_PARAMS(fn_obj));
            int32_t callee_locals = LAMBDA_NLOCALS(fn_obj);
            if (vm.rp >= VM_STACK_SIZE ||
                vm.sp + callee_locals >= VM_STACK_SIZE) {
                for (int32_t i = 0; i < n; i++)
                    if (fn_args[i]) ray_release(fn_args[i]);
                ray_release(fn_obj);
                goto vm_error_limit;
            }
            if (n != pcnt) {
                for (int32_t i = 0; i < n; i++)
                    if (fn_args[i]) ray_release(fn_args[i]);
                ray_release(fn_obj);
                vm_err_obj = ray_error("arity", "expected %" PRId64 " args, got %d", pcnt, n);
                goto vm_error;
            }

            /* Push return frame */
            vm.rs[vm.rp++] = (vm_ctx_t){ .fn = vm.fn, .fp = vm.fp, .ip = ip };

            /* Set up new frame */
            vm.fn = fn_obj;  /* takes ownership of stack ref */
            vm.fp = vm.sp;
            vm.sp += callee_locals;
            n_locals = callee_locals;

            /* Bind parameters */
            int64_t bind = pcnt < n ? pcnt : n;
            for (int64_t i = 0; i < bind; i++)
                LOCAL(i) = fn_args[i];  /* transfer ownership from args */
            for (int32_t i = (int32_t)bind; i < callee_locals; i++)
                LOCAL(i) = NULL;
            for (int64_t i = bind; i < n; i++)
                ray_release(fn_args[i]);  /* excess args */

            /* Check for Ctrl-C interrupt on each compiled call */
            if (g_eval_interrupted) goto vm_error_limit;

            /* Switch to callee bytecode */
            code = (uint8_t *)ray_data(LAMBDA_BC(fn_obj));
            cpool = (ray_t **)ray_data(LAMBDA_CONSTS(fn_obj));
            ip = 0;
            DISPATCH();
        }
    }

    /* Non-lambda or uncompiled: dispatch by type */
    {
        ray_t *result;
        switch (fn_obj->type) {
        case RAY_UNARY:
            if (fn_is_restricted(fn_obj)) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_error("access", "restricted"); break; }
            if (n != 1) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_error("arity", "expected 1 arg, got %d", n); break; }
            /* fn_args[0] is an owned ref (POPped from VM stack); materialise
             * consumes it — no extra ray_release after the call. */
            if (!(fn_obj->attrs & RAY_FN_LAZY_AWARE) && fn_args[0] && ray_is_lazy(fn_args[0])) {
                fn_args[0] = ray_lazy_materialize(fn_args[0]); /* consumes owned ref */
                if (!fn_args[0] || RAY_IS_ERR(fn_args[0])) { result = fn_args[0] ? fn_args[0] : ray_error("type", NULL); fn_args[0] = NULL; break; }
            }
            result = ((ray_unary_fn)(uintptr_t)fn_obj->i64)(fn_args[0]);
            ray_release(fn_args[0]);
            break;
        case RAY_BINARY:
            if (fn_is_restricted(fn_obj)) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_error("access", "restricted"); break; }
            if (n != 2) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_error("arity", "expected 2 args, got %d", n); break; }
            /* fn_args[0/1] are owned refs (POPped from VM stack); materialise
             * consumes them — no extra ray_release after each call. */
            if (!(fn_obj->attrs & RAY_FN_LAZY_AWARE)) {
                if (fn_args[0] && ray_is_lazy(fn_args[0])) {
                    fn_args[0] = ray_lazy_materialize(fn_args[0]); /* consumes owned ref */
                    if (!fn_args[0] || RAY_IS_ERR(fn_args[0])) { result = fn_args[0] ? fn_args[0] : ray_error("type", NULL); fn_args[0] = NULL; ray_release(fn_args[1]); fn_args[1] = NULL; break; }
                }
                if (fn_args[1] && ray_is_lazy(fn_args[1])) {
                    fn_args[1] = ray_lazy_materialize(fn_args[1]); /* consumes owned ref */
                    if (!fn_args[1] || RAY_IS_ERR(fn_args[1])) { result = fn_args[1] ? fn_args[1] : ray_error("type", NULL); ray_release(fn_args[0]); fn_args[0] = NULL; fn_args[1] = NULL; break; }
                }
            }
            result = ((ray_binary_fn)(uintptr_t)fn_obj->i64)(fn_args[0], fn_args[1]);
            ray_release(fn_args[0]);
            ray_release(fn_args[1]);
            break;
        case RAY_VARY:
            if (fn_is_restricted(fn_obj)) { for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]); result = ray_error("access", "restricted"); break; }
            result = ((ray_vary_fn)(uintptr_t)fn_obj->i64)(fn_args, n);
            for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
            break;
        case RAY_LAMBDA:
            result = call_lambda(fn_obj, fn_args, n);
            for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
            break;
        default:
            for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
            result = ray_error("type", NULL);
            break;
        }
        ray_release(fn_obj);
        if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
        PUSH(result);
        DISPATCH();
    }
}

op_calls: {
    /* Self-recursive call — lean path matching rayforce 1.
     * No fn object on stack. Args are already at sp-argc..sp.
     * Push return frame, set fp so args become locals, extend for extra locals. */
    uint8_t argc = code[ip++];

    /* Stack overflow guard */
    if (RAY_UNLIKELY(vm.rp >= VM_STACK_SIZE)) goto vm_error_limit;
    if (RAY_UNLIKELY(vm.sp + n_locals >= VM_STACK_SIZE)) goto vm_error_limit;

    /* Push return frame (fn=NULL signals self-call to OP_RET) */
    vm.rs[vm.rp++] = (vm_ctx_t){ .fn = NULL, .fp = vm.fp, .ip = ip };

    /* Args on stack become the new frame's first locals.
     * Compiler guarantees argc == param count, so argc <= n_locals. */
    vm.fp = vm.sp - argc;

    /* Extend stack for extra locals beyond params (let bindings etc.) */
    for (int32_t i = argc; i < n_locals; i++)
        vm.ps[vm.sp++] = NULL;

    ip = 0;
    DISPATCH();
}

op_calld: {
    /* Dynamic dispatch: evaluate AST directly via ray_eval */
    uint8_t n = code[ip++];
    if (n == 0) {
        /* n=0: the AST itself is on the stack, eval it directly */
        ray_t *ast = POP();
        ray_t *result = ray_eval(ast);
        ray_release(ast);
        if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
        PUSH(result);
        DISPATCH();
    }
    /* n>0: build call list and eval */
    ray_t *fn_args[64];
    for (int32_t i = n - 1; i >= 0; i--)
        fn_args[i] = POP();
    ray_t *fn_obj = POP();

    ray_t *call_list = ray_alloc((n + 1) * sizeof(ray_t *));
    if (!call_list || RAY_IS_ERR(call_list)) {
        for (int32_t i = 0; i < n; i++) ray_release(fn_args[i]);
        ray_release(fn_obj);
        goto vm_error;
    }
    call_list->type = RAY_LIST;
    call_list->len = n + 1;
    ray_t **elems = (ray_t **)ray_data(call_list);
    elems[0] = fn_obj;
    for (int32_t i = 0; i < n; i++)
        elems[i + 1] = fn_args[i];

    ray_t *result = ray_eval(call_list);
    ray_release(call_list);
    if (RAY_IS_ERR(result)) { vm_err_obj = result; goto vm_error; }
    PUSH(result);
    DISPATCH();
}

op_ret: {
    ray_t *result;
    bool from_stack = (vm.sp > vm.fp + n_locals);
    if (from_stack) {
        result = POP();
        ray_retain(result);  /* prevent free during cleanup if aliased in locals */
    } else {
        result = RAY_NULL_OBJ;
    }

    /* Clean up current frame — release all locals and leftover stack slots */
    while (vm.sp > vm.fp) {
        ray_t *v = vm.ps[--vm.sp];
        if (v) ray_release(v);
    }

    /* Undo protective retain — POP's reference is the caller's ownership */
    if (from_stack) ray_release(result);

    if (vm.rp == 0) {
        /* Top-level return */
        ray_release(vm.fn);
        __VM = NULL;
#undef vm
        ray_free(vm_block);
        return result;  /* caller owns the POP'd reference */
#define vm (*vmp)
    }

    /* Pop return frame */
    vm.rp--;
    vm.fp = vm.rs[vm.rp].fp;
    ip = vm.rs[vm.rp].ip;
    if (vm.rs[vm.rp].fn) {
        /* Normal call: restore caller's function */
        ray_release(vm.fn);
        vm.fn = vm.rs[vm.rp].fn;
        code = (uint8_t *)ray_data(LAMBDA_BC(vm.fn));
        cpool = (ray_t **)ray_data(LAMBDA_CONSTS(vm.fn));
        n_locals = LAMBDA_NLOCALS(vm.fn);
    }
    /* Self-call (fn==NULL): vm.fn/code/cpool/n_locals are already correct */
    PUSH(result);
    DISPATCH();
}

op_trap: {
    int16_t offset = (int16_t)((code[ip] << 8) | code[ip + 1]);
    ip += 2;
    if (vm.tp >= VM_TRAP_SIZE) goto vm_error_limit;
    vm.ts[vm.tp++] = (vm_trap_t){
        .rp = vm.rp, .sp = vm.sp, .handler_ip = ip + offset,
        .fn = vm.fn, .fp = vm.fp, .n_locals = n_locals
    };
    ray_retain(vm.fn);
    DISPATCH();
}

op_trap_end: {
    if (vm.tp > 0) {
        vm.tp--;
        ray_release(vm.ts[vm.tp].fn);
    }
    DISPATCH();
}

    const char *vm_err_str = "domain";
    const char *vm_err_detail = NULL;
    goto vm_error_cleanup;

vm_error_limit:
    vm_err_str = "limit";
    vm_err_detail = "stack overflow";
    goto vm_error_cleanup;

vm_error_name:
    vm_err_str = "name";
    vm_err_detail = NULL;
    goto vm_error_cleanup;

vm_error:
    vm_err_str = "domain";
    vm_err_detail = NULL;

vm_error_cleanup: {
    /* Check for trap frame */
    if (vm.tp > 0) {
        vm.tp--;
        vm_trap_t trap = vm.ts[vm.tp];

        /* Clean up return frames above trap point */
        while (vm.rp > trap.rp) {
            vm.rp--;
            if (vm.rs[vm.rp].fn) ray_release(vm.rs[vm.rp].fn);
        }

        /* Clean up stack above trap point */
        while (vm.sp > trap.sp) {
            ray_t *v = vm.ps[--vm.sp];
            if (v) ray_release(v);
        }

        /* Get error value — prefer vm_err_obj (VM-detected errors like
         * arity mismatch) over __raise_val (user raise expressions) */
        ray_t *err_val = vm_err_obj ? vm_err_obj : __raise_val;
        vm_err_obj = NULL;
        __raise_val = NULL;
        if (!err_val) err_val = make_i64(0);

        /* Restore context and push error value */
        ray_release(vm.fn);
        vm.fn = trap.fn;  /* takes ownership from trap frame */
        vm.fp = trap.fp;
        n_locals = trap.n_locals;
        code = (uint8_t *)ray_data(LAMBDA_BC(vm.fn));
        cpool = (ray_t **)ray_data(LAMBDA_CONSTS(vm.fn));
        ip = trap.handler_ip;
        PUSH(err_val);
        DISPATCH();
    }

    /* No trap frame — regular error cleanup */

    /* Build error trace: current frame + callers from return stack */
    add_error_frame(vm.fn, ip > 0 ? ip - 1 : 0);
    for (int32_t i = vm.rp - 1; i >= 0; i--) {
        if (vm.rs[i].fn)
            add_error_frame(vm.rs[i].fn, vm.rs[i].ip > 0 ? vm.rs[i].ip - 1 : 0);
    }

    for (int32_t i = 0; i < vm.sp; i++)
        if (vm.ps[i]) ray_release(vm.ps[i]);
    ray_release(vm.fn);
    for (int32_t i = 0; i < vm.rp; i++)
        if (vm.rs[i].fn) ray_release(vm.rs[i].fn);
    for (int32_t i = 0; i < vm.tp; i++)
        ray_release(vm.ts[i].fn);
    __VM = NULL;
#undef vm
    ray_free(vm_block);
    if (vm_err_obj)
        return vm_err_obj;
    if (vm_err_detail)
        return ray_error(vm_err_str, "%s", vm_err_detail);
    return ray_error(vm_err_str, NULL);
}

#undef DISPATCH
#undef PUSH
#undef POP
#undef PEEK
#undef LOCAL
#undef vm
}


/* ray_enlist_fn, ray_dict_fn, ray_nil_fn, ray_where_fn, ray_group_fn,
 * ray_concat_fn, ray_raze_fn, ray_within_fn, ray_fdiv_fn
 * moved to ops/builtins.c */

/* ══════════════════════════════════════════
 * Builtin registration
 * ══════════════════════════════════════════ */

/* Bind `obj` under `name` in the global env.  For reserved-namespace
 * names like `.sys.gc`:
 *
 *   - `.sys` itself is a RAY_DICT in the env (keys SYM vec + vals
 *     LIST).  Typing `.sys` at the REPL returns the whole dict for
 *     introspection.
 *   - `.sys.gc` is ALSO bound flat in the env, pointing at the same
 *     function object.  This keeps direct lookup O(1), surfaces the
 *     full name to `ray_env_lookup_prefix` (so tab completion and
 *     REPL highlighting continue to see every reserved builtin),
 *     and lets error messages cite the fully-qualified name.
 *
 * The two bindings are created at startup and kept in sync — writes
 * to any `.`-prefixed name are refused by ray_env_set, so user code
 * can't drift them apart.  Only 2-level namespaces are in use; the
 * assert below guards against silent breakage if that changes. */
/* Get-or-create a child dict by key on `parent`.  ray_dict_get
 * returns an owned ref (or NULL if missing), so we either reuse it
 * (after type-checking) or build a fresh subdict and upsert it.
 * Returns the (possibly-COWd) parent; the child is handed back via
 * `*out_child`, owned by the caller. */
static ray_t* dict_get_or_create_subdict(ray_t* parent, ray_t* key,
                                         ray_t** out_child) {
    ray_t* existing = ray_dict_get(parent, key);
    if (existing && !RAY_IS_ERR(existing) && existing->type == RAY_DICT) {
        *out_child = existing;
        return parent;
    }
    if (existing) ray_release(existing);
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 4);
    ray_t* vals = ray_list_new(4);
    assert(keys && !RAY_IS_ERR(keys) && vals && !RAY_IS_ERR(vals));
    ray_t* child = ray_dict_new(keys, vals);
    assert(child && !RAY_IS_ERR(child));
    ray_retain(child);  /* caller retains; dict_upsert below also retains */
    parent = ray_dict_upsert(parent, key, child);
    *out_child = child;
    return parent;
}

static void reg_bind(const char* name, ray_t* obj) {
    int64_t sym = ray_sym_intern(name, strlen(name));
    if (name[0] == '.' && ray_sym_is_dotted(sym)) {
        const int64_t* segs;
        int nsegs = ray_sym_segs(sym, &segs);
        assert(nsegs >= 2 && "reg_bind: dotted reserved name must have ≥ 2 segments");

        int64_t root_sym = segs[0];      /* e.g. `.sys` or `.db` */
        int64_t leaf_sym = segs[nsegs-1];/* leaf action sym */

        /* 1. Get-or-create the root dict bound at `.<ns>`. */
        ray_t* root = ray_env_get(root_sym);
        if (root) {
            ray_retain(root);
        } else {
            ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 4);
            ray_t* vals = ray_list_new(4);
            assert(keys && !RAY_IS_ERR(keys) && vals && !RAY_IS_ERR(vals));
            root = ray_dict_new(keys, vals);
            assert(root && !RAY_IS_ERR(root));
        }

        /* 2. For each intermediate segment, descend into (or create)
         *    a sub-dict.  Two-level names skip this loop entirely
         *    and fall through to the leaf upsert below.  After the
         *    walk, `cur` points at the dict that should hold the
         *    leaf; `chain[]` records the parents we still need to
         *    write back so a COW upsert at the deepest level
         *    propagates upward through every parent. */
        enum { MAX_DEPTH = 4 };
        ray_t* chain[MAX_DEPTH] = { root };
        int64_t chain_keys[MAX_DEPTH] = { 0 };
        int chain_len = 1;
        ray_t* cur = root;
        for (int i = 1; i < nsegs - 1; i++) {
            assert(chain_len < MAX_DEPTH);
            ray_t* mid_key = ray_sym(segs[i]);
            ray_t* child = NULL;
            cur = dict_get_or_create_subdict(cur, mid_key, &child);
            ray_release(mid_key);
            chain[chain_len - 1] = cur;
            chain_keys[chain_len - 1] = segs[i];
            chain[chain_len++] = child;
            cur = child;
        }

        /* 3. Upsert the leaf into the deepest dict, then walk back up
         *    re-upserting any COWd parents into their parents. */
        ray_t* leaf_key = ray_sym(leaf_sym);
        ray_t* deepest = ray_dict_upsert(cur, leaf_key, obj);
        ray_release(leaf_key);
        chain[chain_len - 1] = deepest;
        for (int i = chain_len - 1; i > 0; i--) {
            ray_t* parent_key = ray_sym(chain_keys[i - 1]);
            chain[i - 1] = ray_dict_upsert(chain[i - 1], parent_key, chain[i]);
            ray_release(parent_key);
            ray_release(chain[i]);  /* dict_upsert retained */
        }

        /* 4. Bind the (possibly-COWd) root and the flat fully-qualified
         *    name so ray_env_lookup_prefix (REPL completion / syntax
         *    highlighting) enumerates every reserved builtin by name.
         *    ray_env_bind_flat skips the dotted-walk so this doesn't
         *    re-upsert into the same dict we just built. */
        assert(ray_env_bind(root_sym, chain[0]) == RAY_OK);
        ray_release(chain[0]);
        assert(ray_env_bind_flat(sym, obj) == RAY_OK);
        return;
    }
    assert(ray_env_bind(sym, obj) == RAY_OK);
}

static void register_binary(const char* name, uint8_t attrs, ray_binary_fn fn) {
    ray_t* obj = ray_fn_binary(name, attrs, fn);
    reg_bind(name, obj);
    ray_release(obj);
}

/* Register binary with a DAG opcode for vectorized execution */
static void register_binary_op(const char* name, uint8_t attrs, ray_binary_fn fn, uint16_t opcode) {
    ray_t* obj = ray_fn_binary(name, attrs, fn);
    RAY_FN_SET_OPCODE(obj, opcode);
    reg_bind(name, obj);
    ray_release(obj);
}

static void register_unary(const char* name, uint8_t attrs, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(name, attrs, fn);
    reg_bind(name, obj);
    ray_release(obj);
}

static void register_unary_op(const char* name, uint8_t attrs, ray_unary_fn fn, uint16_t opcode) {
    ray_t* obj = ray_fn_unary(name, attrs, fn);
    RAY_FN_SET_OPCODE(obj, opcode);
    reg_bind(name, obj);
    ray_release(obj);
}

static void register_vary(const char* name, uint8_t attrs, ray_vary_fn fn) {
    ray_t* obj = ray_fn_vary(name, attrs, fn);
    reg_bind(name, obj);
    ray_release(obj);
}

static void ray_register_builtins(void) {
    register_binary_op("+",   RAY_FN_ATOMIC, ray_add_fn, OP_ADD);
    register_binary_op("-",   RAY_FN_ATOMIC, ray_sub_fn, OP_SUB);
    register_binary_op("*",   RAY_FN_ATOMIC, ray_mul_fn, OP_MUL);
    register_binary_op("/",   RAY_FN_ATOMIC, ray_div_fn, OP_DIV);
    register_binary_op("%",   RAY_FN_ATOMIC, ray_mod_fn, OP_MOD);
    register_binary_op(">",   RAY_FN_ATOMIC, ray_gt_fn,  OP_GT);
    register_binary_op("<",   RAY_FN_ATOMIC, ray_lt_fn,  OP_LT);
    register_binary_op(">=",  RAY_FN_ATOMIC, ray_gte_fn,    OP_GE);
    register_binary_op("<=",  RAY_FN_ATOMIC, ray_lte_fn,    OP_LE);
    register_binary_op("==",  RAY_FN_ATOMIC, ray_eq_fn,  OP_EQ);
    register_binary_op("!=",  RAY_FN_ATOMIC, ray_neq_fn,    OP_NE);
    /* Special-form so args are passed unevaluated and the kernel can
     * short-circuit on the first determining scalar (matches v1 and the
     * Lisp/Clojure convention). */
    register_vary("and", RAY_FN_SPECIAL_FORM, ray_and_vary_fn);
    register_vary("or",  RAY_FN_SPECIAL_FORM, ray_or_vary_fn);
    register_unary_op("not",  RAY_FN_NONE,   ray_not_fn, OP_NOT);
    register_unary_op("neg",  RAY_FN_ATOMIC, ray_neg_fn, OP_NEG);
    register_unary("round",   RAY_FN_ATOMIC, ray_round_fn);
    register_unary_op("floor", RAY_FN_ATOMIC, ray_floor_fn, OP_FLOOR);
    register_unary_op("ceil",  RAY_FN_ATOMIC, ray_ceil_fn,  OP_CEIL);
    register_unary_op("abs",   RAY_FN_ATOMIC, ray_abs_fn,  OP_ABS);
    register_unary_op("sqrt",  RAY_FN_ATOMIC, ray_sqrt_fn, OP_SQRT);
    register_unary_op("log",   RAY_FN_ATOMIC, ray_log_fn,  OP_LOG);
    register_unary_op("exp",   RAY_FN_ATOMIC, ray_exp_fn,  OP_EXP);

    /* Special forms */
    register_binary("set", RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_set_fn);
    register_binary("let", RAY_FN_SPECIAL_FORM, ray_let_fn);
    register_vary("if",    RAY_FN_SPECIAL_FORM, ray_cond_fn);
    register_vary("do",    RAY_FN_SPECIAL_FORM, ray_do_fn);
    register_vary("fn",    RAY_FN_SPECIAL_FORM, ray_fn);

    /* Aggregation builtins */
    /* RAY_FN_LAZY_AWARE: these fns have 'if (ray_is_lazy(x)) return ray_lazy_append(...)' shells
     * and must NOT have their args materialised by the dispatcher. */
    register_unary("sum",   RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_sum_fn);
    register_unary("count", RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_count_fn);
    register_unary("avg",   RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_avg_fn);
    register_unary("min",   RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_min_fn);
    register_unary("max",   RAY_FN_AGGR | RAY_FN_LAZY_AWARE, ray_max_fn);
    register_unary("first", RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_first_fn);
    register_unary("last",  RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_last_fn);
    /* med/dev/stddev/var materialise inline (no ray_lazy_append shell) — not LAZY_AWARE */
    register_unary("med",   RAY_FN_AGGR, ray_med_fn);
    register_unary("dev",        RAY_FN_AGGR, ray_dev_fn);
    register_unary("stddev",     RAY_FN_AGGR, ray_stddev_fn);
    register_unary("stddev_pop", RAY_FN_AGGR, ray_stddev_pop_fn);
    register_unary("dev_pop",    RAY_FN_AGGR, ray_stddev_pop_fn);
    register_unary("var",        RAY_FN_AGGR, ray_var_fn);
    register_unary("var_pop",    RAY_FN_AGGR, ray_var_pop_fn);

    /* Error handling */
    register_unary("raise", RAY_FN_NONE, ray_raise_fn);
    register_binary("try",  RAY_FN_SPECIAL_FORM, ray_try_fn);

    /* Higher-order functions */
    register_vary("map",    RAY_FN_NONE, ray_map_fn);
    register_vary("pmap",   RAY_FN_NONE, ray_pmap_fn);
    register_vary("fold",   RAY_FN_NONE, ray_fold_fn);
    register_vary("scan",   RAY_FN_NONE, ray_scan_fn);
    register_binary("filter", RAY_FN_NONE, ray_filter_fn);
    register_vary("apply",  RAY_FN_NONE, ray_apply_fn);

    /* Collection operations */
    register_unary("distinct", RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_distinct_fn);
    register_binary("in",      RAY_FN_NONE, ray_in_fn);
    register_binary("except",  RAY_FN_NONE, ray_except_fn);
    register_binary("union",   RAY_FN_NONE, ray_union_fn);
    register_binary("sect",    RAY_FN_NONE, ray_sect_fn);
    register_binary("take",    RAY_FN_NONE, ray_take_fn);
    register_binary("at",      RAY_FN_NONE, ray_at_fn);
    register_binary("find",    RAY_FN_NONE, ray_find_fn);
    register_unary("reverse",  RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_reverse_fn);
    register_unary("til",      RAY_FN_NONE, ray_til_fn);

    /* Sorting operations */
    register_unary("asc",      RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_asc_fn);
    register_unary("desc",     RAY_FN_NONE | RAY_FN_LAZY_AWARE, ray_desc_fn);
    register_unary("iasc",     RAY_FN_NONE, ray_iasc_fn);
    register_unary("idesc",    RAY_FN_NONE, ray_idesc_fn);
    register_unary("rank",     RAY_FN_NONE, ray_rank_fn);
    register_binary("xasc",    RAY_FN_NONE, ray_xasc_fn);
    register_binary("xdesc",   RAY_FN_NONE, ray_xdesc_fn);

    /* Table operations */
    register_vary("list",      RAY_FN_NONE, ray_list_fn);
    register_binary("table",   RAY_FN_NONE, ray_table_fn);
    register_unary("key",      RAY_FN_NONE, ray_key_fn);
    register_unary("value",    RAY_FN_NONE, ray_value_fn);
    register_binary("union-all",      RAY_FN_NONE, ray_union_all_fn);
    /* table-distinct removed — distinct dispatches on type */

    /* Query operations */
    register_vary("select",    RAY_FN_SPECIAL_FORM, ray_select_fn);
    register_vary("update",    RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_update_fn);
    register_vary("insert",    RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_insert_fn);
    register_vary("upsert",    RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_upsert_fn);
    register_binary("xbar",    RAY_FN_ATOMIC, ray_xbar_fn);

    /* Join operations */
    register_vary("left-join",   RAY_FN_NONE, ray_left_join_fn);
    register_vary("inner-join",  RAY_FN_NONE, ray_inner_join_fn);
    register_vary("anti-join",   RAY_FN_NONE, ray_anti_join_fn);
    register_vary("window-join", RAY_FN_SPECIAL_FORM, ray_window_join_fn);
    register_vary("window-join1", RAY_FN_SPECIAL_FORM, ray_window_join_fn);
    register_vary("asof-join",   RAY_FN_NONE, ray_asof_join_fn);

    /* I/O builtins */
    register_vary("println",    RAY_FN_NONE, ray_println_fn);
    register_vary("show",       RAY_FN_NONE, ray_show_fn);
    register_vary("format",     RAY_FN_NONE, ray_format_fn);
    register_vary(".csv.read",  RAY_FN_RESTRICTED, ray_read_csv_fn);
    register_vary(".csv.write", RAY_FN_RESTRICTED, ray_write_csv_fn);
    register_binary("as",       RAY_FN_NONE, ray_cast_fn);
    register_unary("type",      RAY_FN_NONE, ray_type_fn);
    register_unary("read",      RAY_FN_RESTRICTED, ray_read_file_fn);
    register_binary("write",    RAY_FN_RESTRICTED, ray_write_file_fn);
    register_unary("load",      RAY_FN_RESTRICTED, ray_load_file_fn);
    register_unary("exit",      RAY_FN_RESTRICTED, ray_exit_fn);
    register_vary("resolve",    RAY_FN_SPECIAL_FORM, ray_resolve_fn);
    register_vary("timeit",     RAY_FN_SPECIAL_FORM, ray_timeit_fn);

    /* Additional builtins (ported from rayforce) */
    register_vary("enlist",     RAY_FN_NONE, ray_enlist_fn);
    register_binary("dict",     RAY_FN_NONE, ray_dict_fn);
    register_unary("nil?",      RAY_FN_NONE, ray_nil_fn);
    register_unary("where",     RAY_FN_NONE, ray_where_fn);
    register_unary("group",     RAY_FN_NONE, ray_group_fn);
    register_binary("concat",   RAY_FN_NONE, ray_concat_fn);
    register_unary("raze",      RAY_FN_NONE, ray_raze_fn);
    register_binary("within",   RAY_FN_NONE, ray_within_fn);
    register_binary("div",      RAY_FN_ATOMIC, ray_fdiv_fn);
    register_binary("rand",     RAY_FN_NONE, ray_rand_fn);
    register_binary("bin",      RAY_FN_NONE, ray_bin_fn);
    register_binary("binr",     RAY_FN_NONE, ray_binr_fn);
    register_vary("map-left",   RAY_FN_NONE, ray_map_left_fn);
    register_vary("map-right",  RAY_FN_NONE, ray_map_right_fn);

    /* String operations */
    register_binary("split",     RAY_FN_NONE, ray_split_fn);

    /* Serialization */
    register_unary("ser",        RAY_FN_NONE, ray_ser_fn);
    register_unary("de",         RAY_FN_NONE, ray_de_fn);

    /* Splayed / partitioned table I/O */
    /* Database storage — splayed and parted table I/O.  Kept under a
     * dedicated `.db.*` namespace so format-specific siblings stay
     * grouped (set/get/mount per format) and there's room to grow
     * without polluting the top-level builtin namespace. */
    register_vary(".db.splayed.set",   RAY_FN_RESTRICTED, ray_set_splayed_fn);
    register_vary(".db.splayed.get",   RAY_FN_NONE,       ray_get_splayed_fn);
    register_vary(".db.splayed.mount", RAY_FN_NONE,       ray_db_splayed_mount_fn);
    register_vary(".db.parted.get",    RAY_FN_NONE,       ray_get_parted_fn);
    register_vary(".db.parted.mount",  RAY_FN_NONE,       ray_db_parted_mount_fn);

    /* GUID generation */
    register_unary("guid",       RAY_FN_NONE, ray_guid_fn);

    /* In-place mutation */
    register_vary("alter",       RAY_FN_SPECIAL_FORM, ray_alter_fn);

    /* Pattern matching */
    register_binary("like",      RAY_FN_NONE, ray_like_fn);

    /* Temporal clocks */
    register_unary("date",       RAY_FN_NONE, ray_date_clock_fn);
    register_unary("time",       RAY_FN_NONE, ray_time_clock_fn);
    register_unary("timestamp",  RAY_FN_NONE, ray_timestamp_clock_fn);

    /* Temporal field accessors: unary builtins that map 1:1 onto
     * ray_temporal_extract.  Registered here so `(ss ts)` / `(dd d)`
     * participate in the normal call machinery and `ts.ss` / `d.dd`
     * resolve through env_resolve's "is segment a callable" lookup
     * instead of a bespoke sym→field table. */
    register_unary("ss",         RAY_FN_NONE, ray_extract_ss_fn);
    register_unary("hh",         RAY_FN_NONE, ray_extract_hh_fn);
    register_unary("minute",     RAY_FN_NONE, ray_extract_minute_fn);
    register_unary("yyyy",       RAY_FN_NONE, ray_extract_yyyy_fn);
    register_unary("mm",         RAY_FN_NONE, ray_extract_mm_fn);
    register_unary("dd",         RAY_FN_NONE, ray_extract_dd_fn);
    register_unary("dow",        RAY_FN_NONE, ray_extract_dow_fn);
    register_unary("doy",        RAY_FN_NONE, ray_extract_doy_fn);

    /* Eval, parse, print, meta */
    register_unary("eval",       RAY_FN_NONE, ray_eval_builtin_fn);
    register_unary("parse",      RAY_FN_NONE, ray_parse_builtin_fn);
    register_vary("print",       RAY_FN_NONE, ray_print_fn);
    register_unary("meta",       RAY_FN_NONE, ray_meta_fn);

    /* System builtins — bound under the reserved `.sys.*` namespace so
     * user code can't shadow them and a glance at the name identifies
     * the category. */
    register_vary (".sys.gc",   RAY_FN_NONE,        ray_gc_fn);
    register_unary(".sys.exec", RAY_FN_RESTRICTED,  ray_system_fn);
    /* Registry-dispatched system commands.  `.sys.cmd "name args"` is
     * the kdb-style entry point; the per-command direct builtins below
     * skip the string parse for callers that already have a typed arg
     * in hand.  All share the table in lang/syscmd.c. */
    register_unary(".sys.cmd",    RAY_FN_RESTRICTED, ray_syscmd_string_dispatch_fn);
    register_vary (".sys.timeit", RAY_FN_NONE,       ray_sys_timeit_fn);
    register_unary(".sys.listen", RAY_FN_RESTRICTED, ray_sys_listen_fn);
    register_vary (".sys.env",    RAY_FN_NONE,       ray_sys_env_fn);

    /* OS env / process interaction under `.os.*` */
    register_unary( ".os.getenv", RAY_FN_RESTRICTED,  ray_getenv_fn);
    register_binary(".os.setenv", RAY_FN_RESTRICTED,  ray_setenv_fn);
    /* Filesystem metadata (issue #36): size + listing.  Predicates
     * (exists / is-file / is-dir) are reachable via `try` on these
     * or via shell fallback through `.sys.cmd`. */
    register_unary( ".os.size",   RAY_FN_NONE,        ray_os_size_fn);
    register_unary( ".os.list",   RAY_FN_NONE,        ray_os_list_fn);

    /* IPC client primitives under `.ipc.*` */
    register_unary( ".ipc.open",  RAY_FN_RESTRICTED,  ray_hopen_fn);
    register_unary( ".ipc.close", RAY_FN_RESTRICTED,  ray_hclose_fn);
    register_binary(".ipc.send",  RAY_FN_RESTRICTED,  ray_hsend_fn);

    /* Remote-REPL session control under `.repl.*`.  Once .repl.connect
     * succeeds, the local REPL line-loop reroutes each subsequent input
     * through ray_ipc_send_verbose() until .repl.disconnect.  See
     * src/app/repl_remote.c for the state model. */
    register_unary(".repl.connect",    RAY_FN_RESTRICTED, ray_repl_connect_fn);
    register_vary( ".repl.disconnect", RAY_FN_RESTRICTED, ray_repl_disconnect_fn);

    /* Transaction-log journaling under `.log.*` — q/kdb's -l/-L feature.
     * The CLI flags -l <base> / -L <base> call ray_journal_open() at
     * startup; these builtins expose the same machinery to Rayfall code
     * for manual control (open from a script, snapshot on demand, etc). */
    register_vary(".log.open",     RAY_FN_RESTRICTED, ray_log_open_fn);
    register_unary(".log.write",   RAY_FN_NONE,       ray_log_write_fn);
    register_unary(".log.replay",  RAY_FN_RESTRICTED, ray_log_replay_fn);
    register_unary(".log.validate",RAY_FN_NONE,       ray_log_validate_fn);
    register_vary(".log.roll",     RAY_FN_RESTRICTED, ray_log_roll_fn);
    register_vary(".log.snapshot", RAY_FN_RESTRICTED, ray_log_snapshot_fn);
    register_vary(".log.sync",     RAY_FN_NONE,       ray_log_sync_fn);
    register_vary(".log.close",    RAY_FN_RESTRICTED, ray_log_close_fn);

    /* quote — special form (unevaluated argument) */
    register_vary("quote",       RAY_FN_SPECIAL_FORM, ray_quote_fn);

    /* return — early return (identity) */
    register_unary("return",     RAY_FN_NONE, ray_return_fn);

    /* args — command line arguments */
    register_unary("args",       RAY_FN_NONE, ray_args_fn);

    /* rc — reference count */
    register_unary("rc",         RAY_FN_NONE, ray_rc_fn);

    /* diverse — check if all elements unique */
    register_unary("diverse",    RAY_FN_NONE, ray_diverse_fn);

    /* get — dictionary/table lookup (alias for at) */
    register_binary("get",       RAY_FN_NONE, ray_get_fn);

    /* remove — remove key from dict */
    register_binary("remove",    RAY_FN_NONE, ray_remove_fn);

    /* row — single row from table */
    register_binary("row",       RAY_FN_NONE, ray_row_fn);

    /* timer — high-res monotonic nanosecond timestamp */
    register_unary("timer",      RAY_FN_NONE, ray_timer_fn);

    /* env — list all global environment bindings */
    register_unary("env",        RAY_FN_NONE, ray_env_fn);

    /* Directional fold/scan variants */
    register_vary("fold-left",   RAY_FN_NONE, ray_fold_left_fn);
    register_vary("fold-right",  RAY_FN_NONE, ray_fold_right_fn);
    register_vary("scan-left",   RAY_FN_NONE, ray_scan_left_fn);
    register_vary("scan-right",  RAY_FN_NONE, ray_scan_right_fn);

    /* del, modify, pivot remain top-level language primitives.
     * Runtime/heap introspection moves under `.sys.*`. */
    register_vary("del",          RAY_FN_SPECIAL_FORM | RAY_FN_RESTRICTED, ray_del_fn);
    register_vary(".sys.build", RAY_FN_NONE, ray_internals_fn);
    register_vary(".sys.mem",   RAY_FN_NONE, ray_memstat_fn);
    register_vary("modify",     RAY_FN_RESTRICTED, ray_modify_fn);
    register_vary("pivot",      RAY_FN_NONE, ray_pivot_fn);
    register_vary(".sys.info",  RAY_FN_NONE, ray_sysinfo_fn);
    register_unary("sym-name",   RAY_FN_NONE, ray_sym_name_fn);
    register_binary("unify",     RAY_FN_NONE, ray_unify_fn);
    register_binary("xrank",     RAY_FN_NONE, ray_xrank_fn);

    /* EAV triple storage */
    register_vary("datoms",        RAY_FN_NONE, ray_datoms_fn);
    register_vary("assert-fact",   RAY_FN_NONE, ray_assert_fact_fn);
    register_vary("retract-fact",  RAY_FN_NONE, ray_retract_fact_fn);
    register_vary("scan-eav",      RAY_FN_NONE, ray_scan_eav_fn);
    register_vary("pull",          RAY_FN_NONE, ray_pull_fn);

    /* Datalog */
    register_vary("rule",         RAY_FN_SPECIAL_FORM, ray_rule_fn);
    register_vary("query",        RAY_FN_SPECIAL_FORM, ray_query_fn);

    /* Programmatic Datalog API */
    register_vary("dl-program",    RAY_FN_NONE, ray_dl_program_fn);
    register_vary("dl-add-edb",    RAY_FN_NONE, ray_dl_add_edb_fn);
    register_unary("dl-stratify",  RAY_FN_NONE, ray_dl_stratify_fn);
    register_unary("dl-eval",      RAY_FN_NONE, ray_dl_eval_fn);
    register_binary("dl-query",    RAY_FN_NONE, ray_dl_query_fn);
    register_binary("dl-provenance", RAY_FN_NONE, ray_dl_provenance_fn);

    /* Vector similarity / embeddings / HNSW */
    register_binary("cos-dist",    RAY_FN_NONE, ray_cos_dist_fn);
    register_binary("inner-prod",  RAY_FN_NONE, ray_inner_prod_fn);
    register_binary("l2-dist",     RAY_FN_NONE, ray_l2_dist_fn);
    register_unary ("norm",        RAY_FN_NONE, ray_norm_fn);
    register_vary  ("knn",         RAY_FN_NONE, ray_knn_fn);
    register_vary  ("hnsw-build",  RAY_FN_NONE, ray_hnsw_build_fn);
    register_vary  ("ann",         RAY_FN_NONE, ray_ann_fn);
    register_unary ("hnsw-free",   RAY_FN_NONE, ray_hnsw_free_fn);
    register_binary("hnsw-save",   RAY_FN_RESTRICTED, ray_hnsw_save_fn);
    register_unary ("hnsw-load",   RAY_FN_RESTRICTED, ray_hnsw_load_fn);
    register_unary ("hnsw-info",   RAY_FN_NONE, ray_hnsw_info_fn);

    /* Per-vector accelerator indices (see src/ops/idxop.h) */
    register_unary (".idx.zone",   RAY_FN_NONE, ray_idx_zone_fn);
    register_unary (".idx.hash",   RAY_FN_NONE, ray_idx_hash_fn);
    register_unary (".idx.sort",   RAY_FN_NONE, ray_idx_sort_fn);
    register_unary (".idx.bloom",  RAY_FN_NONE, ray_idx_bloom_fn);
    register_unary (".idx.drop",   RAY_FN_NONE, ray_idx_drop_fn);
    register_unary (".idx.has?",   RAY_FN_NONE, ray_idx_has_fn);
    register_unary (".idx.info",   RAY_FN_NONE, ray_idx_info_fn);

    /* Linked columns (see src/ops/linkop.h) */
    register_binary(".col.link",   RAY_FN_NONE, ray_col_link_fn);
    register_unary (".col.unlink", RAY_FN_NONE, ray_col_unlink_fn);
    register_unary (".col.link?",  RAY_FN_NONE, ray_col_link_p_fn);
    register_unary (".col.target", RAY_FN_NONE, ray_col_target_fn);

    /* Graph algorithms (see src/ops/graph_builtin.c, src/ops/traverse.c).
     * Build a CSR handle from an edge table, then dispatch any of the 18
     * algorithms in src/ops/traverse.c.  All wrappers are vary-arity so
     * the underlying algorithm's optional parameters surface uniformly.
     * Lifecycle handles auto-free on rc→0 (see RAY_ATTR_GRAPH branch in
     * src/mem/heap.c). */
    register_vary (".graph.build",         RAY_FN_NONE, ray_graph_build_fn);
    register_unary(".graph.free",          RAY_FN_NONE, ray_graph_free_fn);
    register_unary(".graph.info",          RAY_FN_NONE, ray_graph_info_fn);
    register_vary (".graph.pagerank",      RAY_FN_NONE, ray_graph_pagerank_fn);
    register_vary (".graph.connected",     RAY_FN_NONE, ray_graph_connected_fn);
    register_vary (".graph.dijkstra",      RAY_FN_NONE, ray_graph_dijkstra_fn);
    register_vary (".graph.louvain",       RAY_FN_NONE, ray_graph_louvain_fn);
    register_vary (".graph.degree",        RAY_FN_NONE, ray_graph_degree_fn);
    register_vary (".graph.topsort",       RAY_FN_NONE, ray_graph_topsort_fn);
    register_vary (".graph.dfs",           RAY_FN_NONE, ray_graph_dfs_fn);
    register_vary (".graph.cluster",       RAY_FN_NONE, ray_graph_cluster_fn);
    register_vary (".graph.betweenness",   RAY_FN_NONE, ray_graph_betweenness_fn);
    register_vary (".graph.closeness",     RAY_FN_NONE, ray_graph_closeness_fn);
    register_vary (".graph.mst",           RAY_FN_NONE, ray_graph_mst_fn);
    register_vary (".graph.random-walk",   RAY_FN_NONE, ray_graph_random_walk_fn);
    register_vary (".graph.k-shortest",    RAY_FN_NONE, ray_graph_k_shortest_fn);
    register_vary (".graph.shortest-path", RAY_FN_NONE, ray_graph_shortest_path_fn);
    register_vary (".graph.expand",        RAY_FN_NONE, ray_graph_expand_fn);
    register_vary (".graph.var-expand",    RAY_FN_NONE, ray_graph_var_expand_fn);
}

/* ══════════════════════════════════════════
 * Runtime lifecycle
 * ══════════════════════════════════════════ */

ray_err_t ray_lang_init(void) {
    ray_err_t err = ray_env_init();
    if (err != RAY_OK) return err;
    ray_register_builtins();
    return RAY_OK;
}

void ray_lang_destroy(void) {
    if (__raise_val) { ray_release(__raise_val); __raise_val = NULL; }
    /* Reset global Datalog rule storage */
    ray_dl_reset_rules();
    ray_env_destroy();
    ray_compile_reset();
}

/* ══════════════════════════════════════════
 * Tree-walking evaluator
 * ══════════════════════════════════════════ */

ray_t* ray_eval(ray_t* obj) {
    if (!obj || RAY_IS_ERR(obj)) return obj;

    /* Check for external interrupt (e.g. Ctrl-C from REPL) */
    if (g_eval_interrupted) return ray_error("limit", "interrupted");

    if (++eval_depth > RAY_EVAL_MAX_DEPTH) {
        eval_depth--;
        return ray_error("limit", "eval depth exceeded");
    }

    ray_t* ret;

    /* Atoms: return themselves (retain) */
    if (ray_is_atom(obj)) {
        /* Name reference: resolve from env */
        if (obj->type == -RAY_SYM && (obj->attrs & RAY_ATTR_NAME)) {
            /* Check for null keyword — compare by string, not cached sym_id,
             * because sym table may be reinitialized between test runs */
            {
                ray_t* name_str = ray_sym_str(obj->i64);
                if (name_str && ray_str_len(name_str) == 4 &&
                    memcmp(ray_str_ptr(name_str), "null", 4) == 0) {
                    ray_release(name_str);
                    ret = NULL; goto out;
                }
                if (name_str) ray_release(name_str);
            }

            ray_t* val = ray_env_resolve(obj->i64);
            if (!val) {
                ray_t* ns = ray_sym_str(obj->i64);
                if (ns) {
                    ret = ray_error("name", "'%.*s' undefined",
                                    (int)ray_str_len(ns), ray_str_ptr(ns));
                    ray_release(ns);
                } else {
                    ret = ray_error("name", NULL);
                }
                goto out;
            }
            /* env_resolve may also return a real error (e.g. nyi from a
             * parted-target link deref inside the dotted walker) — surface
             * it directly rather than treating it as a found value. */
            if (RAY_IS_ERR(val)) { ret = val; goto out; }
            /* env_resolve hands back an owned ref; no extra retain. */
            ret = val; goto out;
        }
        ray_retain(obj);
        ret = obj; goto out;
    }

    /* Non-list vectors (incl. RAY_DICT/RAY_TABLE): return themselves —
     * dict literals are self-evaluating; values stay unevaluated.  Use
     * the (dict ...) builtin for evaluated construction. */
    if (obj->type != RAY_LIST) { ray_retain(obj); ret = obj; goto out; }

    /* Empty list */
    if (ray_len(obj) == 0) { ray_retain(obj); ret = obj; goto out; }

    /* List: evaluate first element, dispatch by type */
    ray_t** elems = (ray_t**)ray_data(obj);
    ray_t* head = ray_eval(elems[0]);
    if (RAY_IS_ERR(head)) { ret = head; goto out; }

    int64_t n = ray_len(obj);

    switch (head->type) {
        case RAY_UNARY: {
            if (n != 2) { ray_release(head); ret = ray_error("arity", "expected 1 arg, got %d", (int)(n-1)); goto out; }
            if (fn_is_restricted(head)) { ray_release(head); ret = ray_error("access", "restricted"); goto out; }
            ray_unary_fn fn = (ray_unary_fn)(uintptr_t)head->i64;
            uint8_t fn_attrs = head->attrs;
            ray_t* arg = ray_eval(elems[1]);
            ray_release(head);
            if (arg && RAY_IS_ERR(arg)) { ret = arg; goto out; }
            /* Materialise lazy arg for non-lazy-aware fns.
             * arg is an owned ref (returned from ray_eval); materialise
             * consumes it — no extra ray_release needed after the call. */
            if (!(fn_attrs & RAY_FN_LAZY_AWARE) && arg && ray_is_lazy(arg)) {
                arg = ray_lazy_materialize(arg); /* consumes owned ref */
                if (!arg || RAY_IS_ERR(arg)) { ret = arg ? arg : ray_error("type", NULL); goto out; }
            }
            ray_t* result;
            if (!arg || RAY_IS_NULL(arg)) {
                /* Only nil?/type/ser safely handle null */
                result = (fn == (ray_unary_fn)ray_nil_fn || fn == (ray_unary_fn)ray_type_fn ||
                          fn == (ray_unary_fn)ray_ser_fn) ? fn(arg) : ray_error("type", NULL);
            } else if ((fn_attrs & RAY_FN_ATOMIC) && is_collection(arg))
                result = atomic_map_unary(fn, arg);
            else
                result = fn(arg);
            if (arg) ray_release(arg);
            ret = result; goto out;
        }
        case RAY_BINARY: {
            if (n != 3) { ray_release(head); ret = ray_error("arity", "expected 2 args, got %d", (int)(n-1)); goto out; }
            if (fn_is_restricted(head)) { ray_release(head); ret = ray_error("access", "restricted"); goto out; }
            ray_binary_fn fn = (ray_binary_fn)(uintptr_t)head->i64;
            uint8_t fn_attrs = head->attrs;
            if (fn_attrs & RAY_FN_SPECIAL_FORM) {
                ray_release(head);
                ret = fn(elems[1], elems[2]); goto out;
            }
            ray_t* left = ray_eval(elems[1]);
            if (left && RAY_IS_ERR(left)) {
                ray_release(head);
                ret = left; goto out;
            }
            ray_t* right = ray_eval(elems[2]);
            if (right && RAY_IS_ERR(right)) {
                ray_release(head); if (left) ray_release(left);
                ret = right; goto out;
            }
            /* Materialise lazy args for non-lazy-aware fns.
             * left/right are owned refs (returned from ray_eval); materialise
             * consumes them — no extra ray_release needed after each call. */
            if (!(fn_attrs & RAY_FN_LAZY_AWARE)) {
                if (left && ray_is_lazy(left)) {
                    left = ray_lazy_materialize(left); /* consumes owned ref */
                    if (!left || RAY_IS_ERR(left)) {
                        ray_release(head); if (right) ray_release(right);
                        ret = left ? left : ray_error("type", NULL); goto out;
                    }
                }
                if (right && ray_is_lazy(right)) {
                    right = ray_lazy_materialize(right); /* consumes owned ref */
                    if (!right || RAY_IS_ERR(right)) {
                        ray_release(head); if (left) ray_release(left);
                        ret = right ? right : ray_error("type", NULL); goto out;
                    }
                }
            }
            /* If either arg is NULL/void, only == and != can handle it */
            if (!left || !right || RAY_IS_NULL(left) || RAY_IS_NULL(right)) {
                if (fn == (ray_binary_fn)ray_eq_fn || fn == (ray_binary_fn)ray_neq_fn) {
                    ray_release(head);
                    ray_t* result = fn(left, right);
                    ray_release(left);
                    ray_release(right);
                    ret = result; goto out;
                }
                ray_release(head);
                ray_release(left);
                ray_release(right);
                ret = ray_error("type", NULL); goto out;
            }
            uint16_t fn_opcode = RAY_FN_OPCODE(head);
            ray_release(head);
            ray_t* result;
            if ((fn_attrs & RAY_FN_ATOMIC) && (is_collection(left) || is_collection(right)))
                result = atomic_map_binary_op(fn, fn_opcode, left, right);
            else
                result = fn(left, right);
            ray_release(left);
            ray_release(right);
            ret = result; goto out;
        }
        case RAY_VARY: {
            if (fn_is_restricted(head)) { ray_release(head); ret = ray_error("access", "restricted"); goto out; }
            ray_vary_fn fn = (ray_vary_fn)(uintptr_t)head->i64;
            if (head->attrs & RAY_FN_SPECIAL_FORM) {
                ray_release(head);
                ret = fn(elems + 1, n - 1); goto out;
            }
            int64_t argc = n - 1;
            if (argc > 64) { ray_release(head); ret = ray_error("domain", NULL); goto out; }
            ray_t* args[64];
            for (int64_t i = 0; i < argc; i++) {
                args[i] = ray_eval(elems[i + 1]);
                if (!args[i] || RAY_IS_ERR(args[i])) {
                    ray_t* err = (!args[i]) ? ray_error("type", NULL) : args[i];
                    for (int64_t j = 0; j < i; j++) ray_release(args[j]);
                    ray_release(head);
                    ret = err; goto out;
                }
            }
            ray_release(head);
            ray_t* result = fn(args, argc);
            for (int64_t i = 0; i < argc; i++) ray_release(args[i]);
            ret = result; goto out;
        }
        case RAY_LAMBDA: {
            int64_t argc = n - 1;
            if (argc > 64) { ray_release(head); ret = ray_error("domain", NULL); goto out; }
            ray_t* args[64];
            for (int64_t i = 0; i < argc; i++) {
                args[i] = ray_eval(elems[i + 1]);
                if (!args[i] || RAY_IS_ERR(args[i])) {
                    ray_t* err = (!args[i]) ? ray_error("type", NULL) : args[i];
                    for (int64_t j = 0; j < i; j++) ray_release(args[j]);
                    ray_release(head);
                    ret = err; goto out;
                }
            }
            ray_t* result = call_lambda(head, args, argc);
            for (int64_t i = 0; i < argc; i++) ray_release(args[i]);
            ray_release(head);
            if (RAY_IS_ERR(result))
                add_eval_error_frame(g_eval_nfo, obj);
            ret = result; goto out;
        }
        default:
            ray_release(head);
            ret = ray_error("type", NULL); goto out;
    }

out:
    eval_depth--;
    /* End-of-top-level-expression cleanup hook. Every path that
     * entered ray_eval — REPL, IPC, ray_eval_str, file mode — exits
     * through here; firing ray_progress_end exactly when the depth
     * returns to 0 guarantees the progress bar is cleared no matter
     * which builtin drove the update (including ray_group_fn etc.
     * that bypass ray_execute). */
    if (eval_depth == 0) ray_progress_end();
    return ret;
}

ray_t* ray_eval_str(const char* source) {
    ray_clear_error_trace();
    ray_t* nfo = ray_nfo_create("repl", 4, source, strlen(source));
    ray_t* parsed = ray_parse_with_nfo(source, nfo);
    if (RAY_IS_ERR(parsed)) { ray_release(nfo); return parsed; }

    ray_t* prev_nfo = g_eval_nfo;
    g_eval_nfo = nfo;
    ray_t* result = ray_eval(parsed);
    g_eval_nfo = prev_nfo;

    ray_release(parsed);
    ray_release(nfo);
    /* ray_eval_str is the public top-level boundary — materialise any lazy
     * chain before returning to callers who expect a concrete value. */
    if (ray_is_lazy(result))
        result = ray_lazy_materialize(result);
    return result;
}
