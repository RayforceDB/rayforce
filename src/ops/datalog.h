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
/*
 * datalog.h — Datalog evaluation engine for Rayforce
 *
 * Compiles Datalog rules into ray_graph_t operation DAGs and evaluates
 * them to fixpoint using Rayforce's vectorized columnar execution engine.
 * Supports semi-naive evaluation, stratified negation, and multi-rule heads.
 */
#ifndef RAYFORCE_DATALOG_H
#define RAYFORCE_DATALOG_H

#include "rayforce.h"
#include "ops/ops.h"
#include <stdbool.h>
#include <stdint.h>

/* ===== Body literal types ===== */
#define DL_POS      0   /* positive atom: pred(X, Y, ...) */
#define DL_NEG      1   /* negated atom:  not pred(X, Y, ...) */
#define DL_CMP      2   /* comparison:    X < Y, X = c, etc. */
#define DL_ASSIGN   3   /* assignment:    X = expr */
#define DL_BUILTIN  4   /* builtin predicate */
#define DL_INTERVAL 5   /* interval bind: F @[S, E] */
#define DL_AGG      6   /* aggregate: (count ?N pred), (sum ?S ?expr pred), ... */

/* ===== Comparison operators (for DL_CMP) ===== */
#define DL_CMP_EQ   0
#define DL_CMP_NE   1
#define DL_CMP_LT   2
#define DL_CMP_LE   3
#define DL_CMP_GT   4
#define DL_CMP_GE   5

/* ===== Aggregate operators (for DL_AGG) ===== */
#define DL_AGG_COUNT 0
#define DL_AGG_SUM   1
#define DL_AGG_MIN   2
#define DL_AGG_MAX   3
#define DL_AGG_AVG   4

#define DL_AGG_MAX_KEYS 8

/* ===== Assignment operators (for DL_ASSIGN) ===== */
#define DL_OP_EQ    0   /* simple assignment: X = expr */

/* ===== Builtin predicate IDs (for DL_BUILTIN) ===== */
#define DL_BUILTIN_BEFORE          0  /* before(S, E, T): filter T < S */
#define DL_BUILTIN_DURATION_SINCE  1  /* duration_since(T1, T2, D): D = T2 - T1 */
#define DL_BUILTIN_ABS             2  /* abs(X, Y): Y = |X| */

/* ===== Expression AST for assignments ===== */
typedef enum {
    DL_EXPR_CONST,        /* integer constant (back-compat) */
    DL_EXPR_CONST_F64,    /* float constant */
    DL_EXPR_VAR,          /* bound variable reference */
    DL_EXPR_BINOP,        /* binary op: +, -, *, / */
} dl_expr_kind_t;

typedef struct dl_expr {
    dl_expr_kind_t  kind;
    int64_t         const_val;   /* for DL_EXPR_CONST */
    double          const_f64;   /* for DL_EXPR_CONST_F64 */
    int             var_idx;     /* for DL_EXPR_VAR */
    int             binop;       /* for DL_EXPR_BINOP: OP_ADD, OP_SUB, etc. */
    struct dl_expr *left;        /* for DL_EXPR_BINOP */
    struct dl_expr *right;       /* for DL_EXPR_BINOP */
} dl_expr_t;

/* Variable index sentinel: constant value, not a variable */
#define DL_CONST    (-1)

/* Maximum arity for any relation */
#define DL_MAX_ARITY 16

/* Maximum number of body literals per rule */
#define DL_MAX_BODY  16

/* Maximum number of rules in a program */
#define DL_MAX_RULES 128

/* Maximum number of relations */
#define DL_MAX_RELS  64

/* Maximum strata */
#define DL_MAX_STRATA 16

/* Program flags */
#define DL_FLAG_PROVENANCE  (1 << 0)  /* track which rule derived each tuple */

/* ===== Body literal ===== */
typedef struct {
    int     type;                   /* DL_POS, DL_NEG, DL_CMP, DL_ASSIGN */
    char    pred[64];               /* predicate name (for DL_POS/DL_NEG) */
    int     arity;                  /* number of argument positions */
    int     vars[DL_MAX_ARITY];    /* variable indices (DL_CONST for constants) */
    int64_t const_vals[DL_MAX_ARITY]; /* constant values (I64/SYM) */
    int     cmp_op;                /* comparison operator (for DL_CMP) */
    int     cmp_lhs;               /* left variable index (for DL_CMP) */
    int     cmp_rhs;               /* right variable index or DL_CONST */
    int64_t cmp_const;             /* constant value if cmp_rhs == DL_CONST */
    int     assign_var;            /* target variable index (for DL_ASSIGN) */
    dl_expr_t *assign_expr;        /* expression tree (for DL_ASSIGN) */
    int     builtin_id;            /* builtin ID (for DL_BUILTIN) */
    dl_expr_t *cmp_lhs_expr;      /* expression tree for LHS (for DL_CMP with expressions) */
    dl_expr_t *cmp_rhs_expr;      /* expression tree for RHS (for DL_CMP with expressions) */
    int     interval_fact_var;     /* fact variable index (for DL_INTERVAL) */
    int     interval_start_var;    /* start variable index (for DL_INTERVAL) */
    int     interval_end_var;      /* end variable index (for DL_INTERVAL) */
    int     agg_op;                /* aggregate operator (for DL_AGG) */
    int     agg_target_var;        /* variable that receives the aggregate result */
    char    agg_pred[64];          /* predicate name being aggregated over */
    int     agg_arity;             /* arity of agg_pred */
    int     agg_value_col;         /* column index inside agg_pred to aggregate (sum/min/max/avg) */
    int     agg_n_group_keys;      /* 0 = scalar; >0 = grouped */
    int     agg_group_key_vars[DL_AGG_MAX_KEYS];
    int     agg_group_key_cols[DL_AGG_MAX_KEYS];
} dl_body_t;

/* ===== Datalog rule: head :- body ===== */
typedef struct {
    char    head_pred[64];          /* head predicate name */
    int     head_arity;
    int     head_vars[DL_MAX_ARITY]; /* variable indices in head */
    int64_t head_consts[DL_MAX_ARITY]; /* constants (when head_vars[i] == DL_CONST) */
    int8_t  head_const_types[DL_MAX_ARITY]; /* ray type tag per head slot:
                                             *   RAY_I64 / RAY_SYM / RAY_F64 when head_vars[i] == DL_CONST,
                                             *   0 when head_vars[i] is a variable. */
    int     n_body;                 /* number of body literals */
    dl_body_t body[DL_MAX_BODY];
    int     n_vars;                 /* total distinct variable count in rule */
    int     stratum;                /* assigned stratum (-1 if not yet stratified) */
} dl_rule_t;

/* ===== Datalog relation ===== */
typedef struct {
    char    name[64];               /* relation name */
    ray_t*  table;                  /* backing columnar table */
    int     arity;                  /* number of columns */
    bool    is_idb;                 /* true = derived (intensional) */
    int64_t col_names[DL_MAX_ARITY]; /* interned column name symbols */
    ray_t*  prov_col;               /* provenance column (when DL_FLAG_PROVENANCE) */
    ray_t*  prov_src_offsets;       /* CSR offsets into prov_src_data, length nrows+1 */
    ray_t*  prov_src_data;          /* packed source refs: (rel_idx << 32) | row_idx */
} dl_rel_t;

/* ===== Datalog program ===== */
typedef struct {
    dl_rel_t    rels[DL_MAX_RELS];
    int         n_rels;
    dl_rule_t   rules[DL_MAX_RULES];
    int         n_rules;
    int         strata[DL_MAX_STRATA][DL_MAX_RELS]; /* predicate indices per stratum */
    int         strata_sizes[DL_MAX_STRATA];         /* number of predicates per stratum */
    int         n_strata;
    uint32_t    flags;                                /* DL_FLAG_* bitmask */
    bool        eval_err;                             /* set by compile/eval on
                                                         unrecoverable failure
                                                         (distinct from "rule
                                                         produced no rows"); read
                                                         by dl_eval to return -1 */
} dl_program_t;

/* ===== Public API ===== */

/* Create a new empty Datalog program */
dl_program_t* dl_program_new(void);

/* Free a Datalog program and release all owned tables */
void dl_program_free(dl_program_t* prog);

/** Append rules registered via the Rayfall (rule ...) special form into a program. */
void dl_append_global_rules(dl_program_t* prog);

/* Register an EDB (extensional) relation backed by an existing table.
 * Column names are auto-generated as "c0", "c1", ... unless the table
 * already has named columns. */
int dl_add_edb(dl_program_t* prog, const char* name, ray_t* table, int arity);

/* Add a rule to the program. The rule struct is copied. */
int dl_add_rule(dl_program_t* prog, const dl_rule_t* rule);

/* Compute stratification (topological sort of negation dependency graph).
 * Returns 0 on success, -1 if program has unstratifiable negation cycle. */
int dl_stratify(dl_program_t* prog);

/* Evaluate the program to fixpoint using semi-naive evaluation.
 * Returns 0 on success, -1 on error. */
int dl_eval(dl_program_t* prog);

/* Query the result of a derived relation after evaluation.
 * Returns the backing table (caller does NOT own it). */
ray_t* dl_query(dl_program_t* prog, const char* pred_name);

/* Retrieve the provenance column from a derived relation.
 * Only valid when DL_FLAG_PROVENANCE is set. Returns the I64 column
 * of rule indices, or NULL if provenance not enabled/available. */
ray_t* dl_get_provenance(dl_program_t* prog, const char* pred_name);

/* Retrieve deep provenance source offsets for a derived relation.
 * Returns an I64 vector of length nrows+1 in CSR format: offsets[i] is the
 * start index in the source-data vector for derived row i.
 * Only valid when DL_FLAG_PROVENANCE is set. Returns NULL if unavailable. */
ray_t* dl_get_provenance_src_offsets(dl_program_t* prog, const char* pred_name);

/* Retrieve deep provenance source data for a derived relation.
 * Returns a flat I64 vector of packed source references. Each entry encodes
 * (relation_index << 32) | row_index, identifying which EDB or IDB relation
 * and row contributed to deriving a given output tuple. Row indices are
 * truncated to 32 bits (max ~4 billion rows per relation).
 *
 * For rules with body-only variables (variables appearing in body atoms but
 * not in the head), source entries include all body rows consistent with
 * head-visible bindings. Cross-body join constraints are not re-enforced
 * during source lookup, so entries may be a superset of the true derivation.
 *
 * Only valid when DL_FLAG_PROVENANCE is set. Returns NULL if unavailable. */
ray_t* dl_get_provenance_src_data(dl_program_t* prog, const char* pred_name);

/* ===== Rule builder helpers ===== */

/* Initialize a rule with the given head predicate and arity */
void dl_rule_init(dl_rule_t* rule, const char* head_pred, int head_arity);

/* Set a head argument to a variable */
void dl_rule_head_var(dl_rule_t* rule, int pos, int var_idx);

/* Set a head argument to an I64 constant — backward-compatible
 * signature. Equivalent to dl_rule_head_const_typed(rule, pos, val,
 * RAY_I64).  Prefer the typed variant for new code. */
void dl_rule_head_const(dl_rule_t* rule, int pos, int64_t val);

/* Set a head argument to a typed constant.
 *   type must be RAY_I64, RAY_SYM, or RAY_F64.
 *   For RAY_F64 callers should pass a double reinterpreted via memcpy/union
 *   into val's int64 slot; dl_rule_head_const_f64 is the safe wrapper. */
void dl_rule_head_const_typed(dl_rule_t* rule, int pos, int64_t val, int8_t type);

/* Convenience wrapper: set a head argument to a RAY_F64 constant. */
void dl_rule_head_const_f64(dl_rule_t* rule, int pos, double val);

/* Add a positive body atom. Returns body literal index. */
int dl_rule_add_atom(dl_rule_t* rule, const char* pred, int arity);

/* Set a body atom argument to a variable */
void dl_body_set_var(dl_rule_t* rule, int body_idx, int pos, int var_idx);

/* Set a body atom argument to a constant */
void dl_body_set_const(dl_rule_t* rule, int body_idx, int pos, int64_t val);

/* Add a negated body atom. Returns body literal index. */
int dl_rule_add_neg(dl_rule_t* rule, const char* pred, int arity);

/* Add a comparison. Returns body literal index. */
int dl_rule_add_cmp(dl_rule_t* rule, int cmp_op, int lhs_var, int rhs_var);

/* Add a comparison with a constant RHS. Returns body literal index. */
int dl_rule_add_cmp_const(dl_rule_t* rule, int cmp_op, int lhs_var, int64_t rhs_val);

/* Add an assignment: target_var = expr. Returns body literal index. */
int dl_rule_add_assign(dl_rule_t* rule, int target_var, int op, dl_expr_t* expr);

/* Add a builtin predicate. Returns body literal index.
 * Arguments are set via dl_body_set_var (same as atoms). */
int dl_rule_add_builtin(dl_rule_t* rule, int builtin_id, int arity);

/* Add a comparison with expression trees on both sides.
 * E.g., "X + Y < Z * 2" -> cmp_op=DL_CMP_LT, lhs=binop(+,X,Y), rhs=binop(*,Z,2).
 * Returns body literal index. */
int dl_rule_add_cmp_expr(dl_rule_t* rule, int cmp_op, dl_expr_t* lhs, dl_expr_t* rhs);

/* Add an interval bind: decompose two consecutive columns at the fact variable's
 * position into start_var and end_var. Returns body literal index. */
int dl_rule_add_interval(dl_rule_t* rule, int fact_var, int start_var, int end_var);

/* pred_arity is advisory; evaluator re-resolves against program EDB/IDB at compile time. */
/* Add an aggregate body literal: (op ?target pred col)
 *  - op: DL_AGG_COUNT (col is ignored), DL_AGG_SUM/MIN/MAX/AVG
 *  - target_var: variable that receives the aggregate result
 *  - pred: predicate to aggregate over
 *  - pred_arity: arity of that predicate
 *  - value_col: which column to aggregate (ignored for COUNT)
 * Returns body literal index. */
int dl_rule_add_agg(dl_rule_t* rule, int op, int target_var,
                    const char* pred, int pred_arity, int value_col);

/* Attach group-by keys to an aggregate body literal previously added via
 * dl_rule_add_agg. body_idx is that builder's return value.
 * key_vars and key_cols have n_keys entries (<= DL_AGG_MAX_KEYS).
 * Returns 0 on success, -1 if n_keys is out of range. */
int dl_rule_agg_set_group(dl_rule_t* rule, int body_idx,
                          const int* key_vars, const int* key_cols, int n_keys);

/* ===== Expression tree builders ===== */

/* Create a constant expression */
dl_expr_t* dl_expr_const(int64_t val);

/* Create a float constant expression */
dl_expr_t* dl_expr_const_f64(double val);

/* Create a variable reference expression */
dl_expr_t* dl_expr_var(int var_idx);

/* Create a binary operation expression (OP_ADD, OP_SUB, OP_MUL, OP_DIV) */
dl_expr_t* dl_expr_binop(int op, dl_expr_t* left, dl_expr_t* right);

/* ===== Internal (used by compiler) ===== */

/* Find relation by name. Returns index or -1. */
int dl_find_rel(dl_program_t* prog, const char* name);

/* Ensure an IDB relation exists for the given head predicate.
 * Creates it with the correct arity if it doesn't exist yet. */
int dl_ensure_idb(dl_program_t* prog, const char* name, int arity);

/* Compile one rule into a ray_graph_t for one fixpoint iteration.
 * delta_pos: which body atom uses the delta relation (-1 for initial pass).
 * rule_idx: index of this rule in prog->rules (used for provenance).
 * Returns the output node in g that produces new head tuples. */
ray_op_t* dl_compile_rule(dl_program_t* prog, dl_rule_t* rule,
                          int delta_pos, int rule_idx, ray_graph_t* g);

#endif /* RAYFORCE_DATALOG_H */
