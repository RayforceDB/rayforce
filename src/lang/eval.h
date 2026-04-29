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

#ifndef RAY_EVAL_H
#define RAY_EVAL_H

#include <rayforce.h>
#include <stdio.h>
#include "lang/nfo.h"

/* ===== Function Attribute Flags (stored in attrs byte) ===== */

#define RAY_FN_NONE          0x00
#define RAY_FN_LEFT_ATOMIC   0x01  /* auto-map left arg over vectors */
#define RAY_FN_RIGHT_ATOMIC  0x02  /* auto-map right arg over vectors */
#define RAY_FN_ATOMIC        0x04  /* auto-map all args over vectors */
#define RAY_FN_AGGR          0x08  /* aggregation function */
#define RAY_FN_SPECIAL_FORM  0x10  /* receives unevaluated args */
#define RAY_FN_RESTRICTED    0x20  /* forbidden during -U restricted IPC evals */

/* AST name flag (distinguishes symbol literal from variable reference) */
#define RAY_ATTR_NAME        0x20  /* ray_t SYM atom with this flag = name reference */

/* Function type signatures */
typedef ray_t* (*ray_unary_fn)(ray_t*);
typedef ray_t* (*ray_binary_fn)(ray_t*, ray_t*);
typedef ray_t* (*ray_vary_fn)(ray_t**, int64_t);

/* DAG opcode stored in nullmap[0..1] for binary builtins with DAG-exec paths.
 * Eliminates dispatch table lookups in atomic_map_binary — just read the opcode. */
#define RAY_FN_OPCODE(fn)        (*(uint16_t*)(fn)->nullmap)
#define RAY_FN_SET_OPCODE(fn,op) (*(uint16_t*)(fn)->nullmap = (uint16_t)(op))

/* ===== VM Bytecode Opcodes ===== */

enum {
    OP_RET = 0,       /* return top of stack */
    OP_JMP,           /* unconditional jump (2-byte signed offset) */
    OP_JMPF,          /* jump if false (2-byte signed offset) */
    OP_LOADCONST,     /* push constant pool[operand] (1-byte index) */
    OP_LOADENV,       /* push local variable (1-byte slot index) */
    OP_STOREENV,      /* pop and store into local (1-byte slot index) */
    OP_POP,           /* discard top of stack */
    OP_RESOLVE,       /* resolve global name: constant pool[operand] is sym_id */
    OP_CALL1,         /* call unary: pop fn + 1 arg, push result */
    OP_CALL2,         /* call binary: pop fn + 2 args, push result */
    OP_CALLN,         /* call variadic: operand = argc, pop fn + N args */
    OP_CALLF,         /* call compiled lambda: push frame, jump to callee */
    OP_CALLS,         /* tail call: reuse frame */
    OP_CALLD,         /* dynamic dispatch: fallback to ray_eval() */
    OP_DUP,           /* duplicate top of stack */
    OP_LOADCONST_W,   /* push constant pool[operand] (2-byte index) */
    OP_RESOLVE_W,     /* resolve global name: 2-byte constant pool index */
    OP_TRAP,          /* push trap frame, 2-byte handler offset */
    OP_TRAP_END,      /* pop trap frame (success path) */
    OP__COUNT
};

/* ===== Compiled Lambda Layout =====
 *
 * A RAY_LAMBDA object with attrs & RAY_FN_COMPILED stores compiled
 * bytecode in its data area:
 *
 *   data[0] = ray_t* params_list   (same as interpreted)
 *   data[1] = ray_t* body          (parsed body, same as interpreted)
 *   data[2] = ray_t* bytecode      (RAY_U8 vector of opcodes)
 *   data[3] = ray_t* constants     (RAY_LIST of constant pool entries)
 *   data[4] = int32_t n_locals    (number of local slots needed)
 *   data[5] = ray_t* nfo          (source location info, NULL if absent)
 *   data[6] = ray_t* dbg          (debug metadata, NULL if absent)
 */

#define RAY_FN_COMPILED  0x40   /* lambda has been compiled to bytecode */

#define LAMBDA_PARAMS(lam)    (((ray_t**)ray_data(lam))[0])
#define LAMBDA_BODY(lam)      (((ray_t**)ray_data(lam))[1])
#define LAMBDA_BC(lam)        (((ray_t**)ray_data(lam))[2])
#define LAMBDA_CONSTS(lam)    (((ray_t**)ray_data(lam))[3])
#define LAMBDA_NLOCALS(lam)   (*((int32_t*)&((ray_t**)ray_data(lam))[4]))
#define LAMBDA_NFO(lam)       (((ray_t**)ray_data(lam))[5])
#define LAMBDA_DBG(lam)       (((ray_t**)ray_data(lam))[6])

#define LAMBDA_IS_COMPILED(lam) ((lam)->attrs & RAY_FN_COMPILED)

/* ===== VM Types ===== */

#define VM_STACK_SIZE 1024

typedef struct {
    ray_t   *fn;     /* lambda being executed */
    int32_t fp;     /* frame pointer */
    int32_t ip;     /* instruction pointer */
} vm_ctx_t;

typedef struct {
    int32_t  rp;        /* return stack depth at trap point */
    int32_t  sp;        /* stack depth at trap point */
    int32_t  handler_ip;/* IP of handler code */
    ray_t    *fn;        /* function containing handler code */
    int32_t  fp;        /* frame pointer at trap point */
    int32_t  n_locals;  /* n_locals at trap point */
} vm_trap_t;

#define VM_TRAP_SIZE 16

typedef struct {
    int32_t  sp;                    /* stack pointer */
    int32_t  fp;                    /* frame pointer */
    int32_t  rp;                    /* return stack pointer */
    int32_t  id;                    /* VM identifier */
    ray_t    *fn;                    /* current lambda */
    void    *heap;                  /* heap pointer (future use) */
    int32_t  tp;                    /* trap stack pointer */
    ray_t    *ps[VM_STACK_SIZE];     /* program stack */
    vm_ctx_t rs[VM_STACK_SIZE];     /* return stack */
    vm_trap_t ts[VM_TRAP_SIZE];     /* trap frames */
} ray_vm_t;

/* ===== Public API ===== */

/* Initialize the Rayfall runtime: symbols, environment, builtins. */
ray_err_t ray_lang_init(void);
void     ray_lang_destroy(void);

/* Evaluate a parsed ray_t object tree. */
ray_t* ray_eval(ray_t* obj);

/* Parse + eval convenience. */
ray_t* ray_eval_str(const char* source);

/* Compile a lambda's body to bytecode. Called lazily on first invocation. */
void ray_compile(ray_t* lambda);

/* Reset compiler cached state (call from ray_lang_destroy). */
void ray_compile_reset(void);

/* Look up the source span for a bytecode IP from a lambda's debug vector.
 * Returns a span with id==0 if not found. */
ray_span_t ray_bc_dbg_get(ray_t* dbg, int32_t ip);

/* Print a ray_t value to a FILE stream. */
void ray_lang_print(FILE* fp, ray_t* val);

/* Interrupt support: allow external code (REPL signal handler) to request
 * that the evaluator abort early.  ray_eval() and the bytecode VM check
 * this flag at function-call and loop boundaries. */
void ray_eval_request_interrupt(void);
void ray_eval_clear_interrupt(void);
int  ray_eval_is_interrupted(void);

/* Return the current eval context's nfo (source location) object, or NULL. */
ray_t* ray_eval_get_nfo(void);
void   ray_eval_set_nfo(ray_t* nfo);

/* Error trace: list of [span_i64, filename, fn_name, source] frames built when
 * a VM error propagates without a trap.  Cleared at the start of ray_eval_str. */
ray_t* ray_get_error_trace(void);
void   ray_clear_error_trace(void);

/* Restricted mode: when true, builtins with RAY_FN_RESTRICTED are blocked. */
void ray_eval_set_restricted(bool on);
bool ray_eval_get_restricted(void);

/* ===== Rayfall Builtin Functions ===== */

/* Arithmetic */
ray_t* ray_add_fn(ray_t* a, ray_t* b);
ray_t* ray_sub_fn(ray_t* a, ray_t* b);
ray_t* ray_mul_fn(ray_t* a, ray_t* b);
ray_t* ray_div_fn(ray_t* a, ray_t* b);
ray_t* ray_mod_fn(ray_t* a, ray_t* b);

/* Comparison */
ray_t* ray_gt_fn(ray_t* a, ray_t* b);
ray_t* ray_lt_fn(ray_t* a, ray_t* b);
ray_t* ray_gte_fn(ray_t* a, ray_t* b);
ray_t* ray_lte_fn(ray_t* a, ray_t* b);
ray_t* ray_eq_fn(ray_t* a, ray_t* b);
ray_t* ray_neq_fn(ray_t* a, ray_t* b);

/* Logic */
ray_t* ray_and_fn(ray_t* a, ray_t* b);
ray_t* ray_or_fn(ray_t* a, ray_t* b);
ray_t* ray_and_vary_fn(ray_t** args, int64_t n);
ray_t* ray_or_vary_fn(ray_t** args, int64_t n);
ray_t* ray_not_fn(ray_t* x);
ray_t* ray_neg_fn(ray_t* x);

/* Aggregation */
ray_t* ray_sum_fn(ray_t* x);
ray_t* ray_count_fn(ray_t* x);
ray_t* ray_avg_fn(ray_t* x);
ray_t* ray_min_fn(ray_t* x);
ray_t* ray_max_fn(ray_t* x);
ray_t* ray_first_fn(ray_t* x);
ray_t* ray_last_fn(ray_t* x);
ray_t* ray_med_fn(ray_t* x);
ray_t* ray_dev_fn(ray_t* x);
ray_t* ray_stddev_fn(ray_t* x);
ray_t* ray_stddev_pop_fn(ray_t* x);
ray_t* ray_var_fn(ray_t* x);
ray_t* ray_var_pop_fn(ray_t* x);

/* Higher-order */
ray_t* ray_map_fn(ray_t** args, int64_t n);
ray_t* ray_pmap_fn(ray_t** args, int64_t n);
ray_t* ray_fold_fn(ray_t** args, int64_t n);
ray_t* ray_scan_fn(ray_t** args, int64_t n);
ray_t* ray_filter_fn(ray_t* vec, ray_t* mask);
ray_t* ray_apply_fn(ray_t** args, int64_t n);

/* Collection */
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

/* Table construction */
ray_t* ray_list_fn(ray_t** args, int64_t n);
ray_t* ray_table_fn(ray_t* names, ray_t* cols);
ray_t* ray_key_fn(ray_t* x);
ray_t* ray_value_fn(ray_t* x);

/* Query */
ray_t* ray_select_fn(ray_t** args, int64_t n);
ray_t* ray_update_fn(ray_t** args, int64_t n);
ray_t* ray_insert_fn(ray_t** args, int64_t n);
ray_t* ray_upsert_fn(ray_t** args, int64_t n);
ray_t* ray_xbar_fn(ray_t* col, ray_t* bucket);

/* Joins */
ray_t* ray_left_join_fn(ray_t** args, int64_t n);
ray_t* ray_inner_join_fn(ray_t** args, int64_t n);
ray_t* ray_window_join_fn(ray_t** args, int64_t n);

/* I/O */
ray_t* ray_println_fn(ray_t** args, int64_t n);
ray_t* ray_read_csv_fn(ray_t** args, int64_t n);
ray_t* ray_write_csv_fn(ray_t** args, int64_t n);
ray_t* ray_read_file_fn(ray_t* path_obj);
ray_t* ray_write_file_fn(ray_t* path_obj, ray_t* content);

/* Vector similarity / embeddings / HNSW.
 * cos-dist and l2-dist return distance (lower = closer); inner-prod is
 * the raw mathematical dot product. */
ray_t* ray_cos_dist_fn(ray_t* a, ray_t* b);
ray_t* ray_inner_prod_fn(ray_t* a, ray_t* b);
ray_t* ray_l2_dist_fn(ray_t* a, ray_t* b);
ray_t* ray_norm_fn(ray_t* x);
ray_t* ray_knn_fn(ray_t** args, int64_t n);
ray_t* ray_hnsw_build_fn(ray_t** args, int64_t n);
ray_t* ray_ann_fn(ray_t** args, int64_t n);
ray_t* ray_hnsw_free_fn(ray_t* h);
ray_t* ray_hnsw_save_fn(ray_t* h, ray_t* path);
ray_t* ray_hnsw_load_fn(ray_t* path);
ray_t* ray_hnsw_info_fn(ray_t* h);

/* Cast and type */
ray_t* ray_cast_fn(ray_t* type_sym, ray_t* val);
ray_t* ray_type_fn(ray_t* val);

/* Special forms */
ray_t* ray_set_fn(ray_t* name_obj, ray_t* val_expr);
ray_t* ray_let_fn(ray_t* name_obj, ray_t* val_expr);
ray_t* ray_cond_fn(ray_t** args, int64_t n);
ray_t* ray_do_fn(ray_t** args, int64_t n);
ray_t* ray_fn(ray_t** args, int64_t n);
ray_t* ray_raise_fn(ray_t* val);
ray_t* ray_try_fn(ray_t* expr, ray_t* handler_expr);


#endif /* RAY_EVAL_H */
