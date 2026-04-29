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
 * datalog.c — Datalog evaluation engine for Rayforce
 *
 * Compiles Datalog rules into ray_graph_t operation DAGs and evaluates
 * them to fixpoint using semi-naive evaluation with stratified negation.
 */
#include "ops/datalog.h"
#include "lang/internal.h"
#include "lang/env.h"
#include "table/sym.h"
#include "ops/ops.h"
#include "ops/hash.h"          /* ray_hash_i64, ray_hash_combine */
#include "ops/internal.h"      /* col_propagate_str_pool */
#include "mem/sys.h"           /* ray_sys_alloc / ray_sys_free */
#include <string.h>
#include <stdio.h>

/* From core/runtime.h — avoiding the include because it pulls in a ray_vm_t
 * typedef that conflicts with the one in lang/eval.h. */
extern const char* ray_error_msg(void);

/* ========================================================================
 * Program lifecycle
 * ======================================================================== */

dl_program_t* dl_program_new(void) {
    /* Allocate via ray_alloc and use the data region for the program struct.
     * This avoids alignment issues since ray_alloc returns a ray_t* header. */
    ray_t* block = ray_alloc(sizeof(dl_program_t));
    if (!block) return NULL;
    dl_program_t* prog = (dl_program_t*)ray_data(block);
    memset(prog, 0, sizeof(dl_program_t));
    return prog;
}

/* Recover the ray_t header from a dl_program_t pointer for ray_free. */
static inline ray_t* dl_prog_block(dl_program_t* prog) {
    return (ray_t*)((char*)prog - 32);  /* ray_data is at offset 32 */
}

void dl_program_free(dl_program_t* prog) {
    if (!prog) return;
    for (int i = 0; i < prog->n_rels; i++) {
        if (prog->rels[i].table && !RAY_IS_ERR(prog->rels[i].table))
            ray_release(prog->rels[i].table);
        if (prog->rels[i].prov_col && !RAY_IS_ERR(prog->rels[i].prov_col))
            ray_release(prog->rels[i].prov_col);
        if (prog->rels[i].prov_src_offsets && !RAY_IS_ERR(prog->rels[i].prov_src_offsets))
            ray_release(prog->rels[i].prov_src_offsets);
        if (prog->rels[i].prov_src_data && !RAY_IS_ERR(prog->rels[i].prov_src_data))
            ray_release(prog->rels[i].prov_src_data);
    }
    ray_free(dl_prog_block(prog));
}

/* ========================================================================
 * Relation management
 * ======================================================================== */

int dl_find_rel(dl_program_t* prog, const char* name) {
    for (int i = 0; i < prog->n_rels; i++) {
        if (strcmp(prog->rels[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* Generate a unique column name for a relation: "{relname}__c{idx}" */
static int64_t dl_col_sym(const char* rel_name, int col_idx) {
    char buf[80];
    snprintf(buf, sizeof(buf), "%s__c%d", rel_name, col_idx);
    return ray_sym_intern(buf, strlen(buf));
}

int dl_add_edb(dl_program_t* prog, const char* name, ray_t* table, int arity) {
    if (!prog || !name || !table || prog->n_rels >= DL_MAX_RELS)
        return -1;

    int idx = prog->n_rels++;
    dl_rel_t* rel = &prog->rels[idx];
    memset(rel, 0, sizeof(dl_rel_t));

    size_t name_len = strlen(name);
    if (name_len >= sizeof(rel->name)) name_len = sizeof(rel->name) - 1;
    memcpy(rel->name, name, name_len);
    rel->name[name_len] = '\0';

    rel->arity = arity;
    rel->is_idb = false;

    /* Build a new table with relation-prefixed column names to avoid
     * collisions when multiple tables participate in a join. */
    for (int c = 0; c < arity && c < DL_MAX_ARITY; c++)
        rel->col_names[c] = dl_col_sym(name, c);

    ray_t* new_tbl = ray_table_new(arity);
    for (int c = 0; c < arity; c++) {
        ray_t* col = ray_table_get_col_idx(table, c);
        if (!col) { ray_release(new_tbl); return -1; }
        new_tbl = ray_table_add_col(new_tbl, rel->col_names[c], col);
        if (RAY_IS_ERR(new_tbl)) return -1;
    }
    rel->table = new_tbl;

    return idx;
}

int dl_ensure_idb(dl_program_t* prog, const char* name, int arity) {
    int idx = dl_find_rel(prog, name);
    if (idx >= 0) return idx;

    if (prog->n_rels >= DL_MAX_RELS) return -1;
    idx = prog->n_rels++;
    dl_rel_t* rel = &prog->rels[idx];
    memset(rel, 0, sizeof(dl_rel_t));

    size_t name_len = strlen(name);
    if (name_len >= sizeof(rel->name)) name_len = sizeof(rel->name) - 1;
    memcpy(rel->name, name, name_len);
    rel->name[name_len] = '\0';

    /* Create empty table with arity columns */
    rel->table = ray_table_new(arity);
    if (!rel->table || RAY_IS_ERR(rel->table)) return -1;

    rel->arity = arity;
    rel->is_idb = true;

    for (int c = 0; c < arity && c < DL_MAX_ARITY; c++) {
        rel->col_names[c] = dl_col_sym(name, c);
        ray_t* empty_col = ray_vec_new(RAY_I64, 0);
        if (empty_col && !RAY_IS_ERR(empty_col)) {
            rel->table = ray_table_add_col(rel->table, rel->col_names[c], empty_col);
            ray_release(empty_col);
        }
    }

    return idx;
}

/* ========================================================================
 * Rule management
 * ======================================================================== */

/* When a rule has a typed head constant at slot c, the IDB relation's
 * column c must be of that type so ray_vec_concat (used by table_union)
 * doesn't reject the merge.  Rebuilds matching columns on an *empty* IDB
 * table in-place.  Safe because schema is established before evaluation. */
static void dl_idb_align_head_const_types(dl_program_t* prog, const dl_rule_t* rule) {
    int rel_idx = dl_find_rel(prog, rule->head_pred);
    if (rel_idx < 0) return;
    dl_rel_t* rel = &prog->rels[rel_idx];
    if (!rel->is_idb) return;
    if (!rel->table || RAY_IS_ERR(rel->table)) return;
    if (ray_table_nrows(rel->table) != 0) return;  /* types already committed */

    int ncols = (int)ray_table_ncols(rel->table);
    if (ncols != rel->arity) return;

    bool any_change = false;
    int8_t desired[DL_MAX_ARITY];
    for (int c = 0; c < rel->arity; c++) {
        ray_t* col = ray_table_get_col_idx(rel->table, c);
        int8_t cur = col ? col->type : RAY_I64;
        int8_t want = rule->head_const_types[c];
        if (want == 0) {
            desired[c] = cur;
        } else if (cur != RAY_I64 && cur != want) {
            /* First-non-zero-wins policy: once a slot is committed to a
             * non-default type by a prior rule, any later rule that
             * disagrees is a program-level conflict.  Mark the program
             * so dl_eval (which reads eval_err after evaluation) reports
             * failure — no stderr write from a non-debug code path. */
            prog->eval_err = true;
            return;
        } else {
            desired[c] = want;
            if (want != cur) any_change = true;
        }
    }
    if (!any_change) return;

    /* Rebuild the table with typed empty columns.  Alignment is required
     * for later evaluation to produce type-matching table_union inputs,
     * so any failure here must also set prog->eval_err = true — silently
     * returning would leave the IDB schema unaligned and dl_eval would
     * later hit a ray_vec_concat type mismatch without any error signal. */
    ray_t* fresh = ray_table_new(rel->arity);
    if (!fresh) { prog->eval_err = true; return; }
    if (RAY_IS_ERR(fresh)) { prog->eval_err = true; ray_error_free(fresh); return; }
    for (int c = 0; c < rel->arity; c++) {
        ray_t* empty_col = ray_vec_new(desired[c], 0);
        if (!empty_col) { prog->eval_err = true; ray_release(fresh); return; }
        if (RAY_IS_ERR(empty_col)) {
            prog->eval_err = true;
            ray_error_free(empty_col);
            ray_release(fresh);
            return;
        }
        ray_t* prev = fresh;
        fresh = ray_table_add_col(fresh, rel->col_names[c], empty_col);
        ray_release(empty_col);
        if (!fresh) { prog->eval_err = true; ray_release(prev); return; }
        if (RAY_IS_ERR(fresh)) {
            prog->eval_err = true;
            ray_release(prev);
            ray_error_free(fresh);
            return;
        }
    }
    ray_release(rel->table);
    rel->table = fresh;
}

int dl_add_rule(dl_program_t* prog, const dl_rule_t* rule) {
    if (!prog || !rule || prog->n_rules >= DL_MAX_RULES)
        return -1;
    int idx = prog->n_rules++;
    memcpy(&prog->rules[idx], rule, sizeof(dl_rule_t));
    prog->rules[idx].stratum = -1;

    /* Ensure IDB relation exists for the head predicate */
    dl_ensure_idb(prog, rule->head_pred, rule->head_arity);

    /* Align IDB column types to any typed head constants in this rule.
     * Must run before evaluation so table_union/concat see matching types. */
    dl_idb_align_head_const_types(prog, rule);

    return idx;
}

/* ========================================================================
 * Rule builder helpers
 * ======================================================================== */

void dl_rule_init(dl_rule_t* rule, const char* head_pred, int head_arity) {
    memset(rule, 0, sizeof(dl_rule_t));
    size_t len = strlen(head_pred);
    if (len >= sizeof(rule->head_pred)) len = sizeof(rule->head_pred) - 1;
    memcpy(rule->head_pred, head_pred, len);
    rule->head_pred[len] = '\0';
    rule->head_arity = head_arity;
    rule->n_body = 0;
    rule->n_vars = 0;
    rule->stratum = -1;
    for (int i = 0; i < DL_MAX_ARITY; i++)
        rule->head_vars[i] = DL_CONST;
}

void dl_rule_head_var(dl_rule_t* rule, int pos, int var_idx) {
    if (pos < 0 || pos >= rule->head_arity) return;
    rule->head_vars[pos] = var_idx;
    rule->head_const_types[pos] = 0;
    if (var_idx + 1 > rule->n_vars) rule->n_vars = var_idx + 1;
}

void dl_rule_head_const_typed(dl_rule_t* rule, int pos, int64_t val, int8_t type) {
    if (pos < 0 || pos >= rule->head_arity) return;
    /* Default to RAY_I64 if an unrecognized type sneaks through; keeps
     * old-callers-with-no-type compat when writing to the slot. */
    if (type != RAY_I64 && type != RAY_SYM && type != RAY_F64)
        type = RAY_I64;
    rule->head_vars[pos] = DL_CONST;
    rule->head_consts[pos] = val;
    rule->head_const_types[pos] = type;
}

/* Backward-compatible I64 wrapper.  Pre-aggregates-PR external callers
 * used this 3-arg form; it now forwards to the typed variant with
 * RAY_I64. */
void dl_rule_head_const(dl_rule_t* rule, int pos, int64_t val) {
    dl_rule_head_const_typed(rule, pos, val, RAY_I64);
}

void dl_rule_head_const_f64(dl_rule_t* rule, int pos, double val) {
    int64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    dl_rule_head_const_typed(rule, pos, bits, RAY_F64);
}

int dl_rule_add_atom(dl_rule_t* rule, const char* pred, int arity) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_POS;
    size_t len = strlen(pred);
    if (len >= sizeof(b->pred)) len = sizeof(b->pred) - 1;
    memcpy(b->pred, pred, len);
    b->pred[len] = '\0';
    b->arity = arity;
    for (int i = 0; i < DL_MAX_ARITY; i++)
        b->vars[i] = DL_CONST;
    return idx;
}

void dl_body_set_var(dl_rule_t* rule, int body_idx, int pos, int var_idx) {
    if (body_idx < 0 || body_idx >= rule->n_body) return;
    if (pos < 0 || pos >= rule->body[body_idx].arity) return;
    rule->body[body_idx].vars[pos] = var_idx;
    if (var_idx + 1 > rule->n_vars) rule->n_vars = var_idx + 1;
}

void dl_body_set_const_typed(dl_rule_t* rule, int body_idx, int pos,
                             int64_t val, int8_t ray_type) {
    if (body_idx < 0 || body_idx >= rule->n_body) return;
    if (pos < 0 || pos >= rule->body[body_idx].arity) return;
    rule->body[body_idx].vars[pos] = DL_CONST;
    rule->body[body_idx].const_vals[pos] = val;
    rule->body[body_idx].const_types[pos] = ray_type;
}

void dl_body_set_const(dl_rule_t* rule, int body_idx, int pos, int64_t val) {
    /* Default the type tag to RAY_I64 — callers that care about
     * RAY_SYM / RAY_STR literals (and thus DATOM-tag-aware compares
     * against I64 columns) should call dl_body_set_const_typed. */
    dl_body_set_const_typed(rule, body_idx, pos, val, RAY_I64);
}

int dl_rule_add_neg(dl_rule_t* rule, const char* pred, int arity) {
    int idx = dl_rule_add_atom(rule, pred, arity);
    if (idx >= 0) rule->body[idx].type = DL_NEG;
    return idx;
}

int dl_rule_add_cmp(dl_rule_t* rule, int cmp_op, int lhs_var, int rhs_var) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_CMP;
    b->cmp_op = cmp_op;
    b->cmp_lhs = lhs_var;
    b->cmp_rhs = rhs_var;
    if (lhs_var + 1 > rule->n_vars) rule->n_vars = lhs_var + 1;
    if (rhs_var + 1 > rule->n_vars) rule->n_vars = rhs_var + 1;
    return idx;
}

int dl_rule_add_cmp_const(dl_rule_t* rule, int cmp_op, int lhs_var, int64_t rhs_val) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_CMP;
    b->cmp_op = cmp_op;
    b->cmp_lhs = lhs_var;
    b->cmp_rhs = DL_CONST;
    b->cmp_const = rhs_val;
    if (lhs_var + 1 > rule->n_vars) rule->n_vars = lhs_var + 1;
    return idx;
}

/* ========================================================================
 * Expression tree builders
 * ======================================================================== */

static dl_expr_t* dl_expr_alloc(void) {
    ray_t* block = ray_alloc(sizeof(dl_expr_t));
    if (!block) return NULL;
    dl_expr_t* e = (dl_expr_t*)ray_data(block);
    memset(e, 0, sizeof(dl_expr_t));
    return e;
}

dl_expr_t* dl_expr_const(int64_t val) {
    dl_expr_t* e = dl_expr_alloc();
    if (!e) return NULL;
    e->kind = DL_EXPR_CONST;
    e->const_val = val;
    return e;
}

dl_expr_t* dl_expr_const_f64(double val) {
    dl_expr_t* e = dl_expr_alloc();
    if (!e) return NULL;
    e->kind = DL_EXPR_CONST_F64;
    e->const_f64 = val;
    return e;
}

dl_expr_t* dl_expr_var(int var_idx) {
    dl_expr_t* e = dl_expr_alloc();
    if (!e) return NULL;
    e->kind = DL_EXPR_VAR;
    e->var_idx = var_idx;
    return e;
}

dl_expr_t* dl_expr_binop(int op, dl_expr_t* left, dl_expr_t* right) {
    dl_expr_t* e = dl_expr_alloc();
    if (!e) return NULL;
    e->kind = DL_EXPR_BINOP;
    e->binop = op;
    e->left = left;
    e->right = right;
    return e;
}

/* ========================================================================
 * Assignment and builtin rule builders
 * ======================================================================== */

int dl_rule_add_assign(dl_rule_t* rule, int target_var, int op, dl_expr_t* expr) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_ASSIGN;
    b->assign_var = target_var;
    b->assign_expr = expr;
    if (target_var + 1 > rule->n_vars) rule->n_vars = target_var + 1;
    (void)op;  /* reserved for future assignment operators */
    return idx;
}

int dl_rule_add_builtin(dl_rule_t* rule, int builtin_id, int arity) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_BUILTIN;
    b->builtin_id = builtin_id;
    b->arity = arity;
    for (int i = 0; i < DL_MAX_ARITY; i++)
        b->vars[i] = DL_CONST;
    return idx;
}

static int dl_expr_max_var(const dl_expr_t* e) {
    if (!e) return -1;
    if (e->kind == DL_EXPR_VAR) return e->var_idx;
    if (e->kind == DL_EXPR_BINOP) {
        int l = dl_expr_max_var(e->left);
        int r = dl_expr_max_var(e->right);
        return l > r ? l : r;
    }
    return -1;
}

int dl_rule_add_cmp_expr(dl_rule_t* rule, int cmp_op, dl_expr_t* lhs, dl_expr_t* rhs) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_CMP;
    b->cmp_op = cmp_op;
    b->cmp_lhs_expr = lhs;
    b->cmp_rhs_expr = rhs;
    /* Update n_vars from the expression trees */
    int mv = dl_expr_max_var(lhs);
    int rv = dl_expr_max_var(rhs);
    if (rv > mv) mv = rv;
    if (mv + 1 > rule->n_vars) rule->n_vars = mv + 1;
    return idx;
}

int dl_rule_add_interval(dl_rule_t* rule, int fact_var, int start_var, int end_var) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_INTERVAL;
    b->interval_fact_var = fact_var;
    b->interval_start_var = start_var;
    b->interval_end_var = end_var;
    if (fact_var + 1 > rule->n_vars) rule->n_vars = fact_var + 1;
    if (start_var + 1 > rule->n_vars) rule->n_vars = start_var + 1;
    if (end_var + 1 > rule->n_vars) rule->n_vars = end_var + 1;
    return idx;
}

int dl_rule_add_agg(dl_rule_t* rule, int op, int target_var,
                    const char* pred, int pred_arity, int value_col) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(*b));
    b->type           = DL_AGG;
    b->agg_op         = op;
    b->agg_target_var = target_var;
    snprintf(b->agg_pred, sizeof(b->agg_pred), "%s", pred);
    b->agg_arity      = pred_arity;
    b->agg_value_col  = value_col;
    b->agg_n_group_keys = 0;
    if (target_var + 1 > rule->n_vars) rule->n_vars = target_var + 1;
    return idx;
}

int dl_rule_agg_set_group(dl_rule_t* rule, int body_idx,
                          const int* key_vars, const int* key_cols, int n_keys) {
    if (!rule || body_idx < 0 || body_idx >= rule->n_body) return -1;
    if (n_keys < 0 || n_keys > DL_AGG_MAX_KEYS) return -1;
    dl_body_t* b = &rule->body[body_idx];
    if (b->type != DL_AGG) return -1;
    b->agg_n_group_keys = n_keys;
    for (int i = 0; i < n_keys; i++) {
        b->agg_group_key_vars[i] = key_vars[i];
        b->agg_group_key_cols[i] = key_cols[i];
        if (key_vars[i] + 1 > rule->n_vars)
            rule->n_vars = key_vars[i] + 1;
    }
    return 0;
}

/* ========================================================================
 * Stratification — topological sort on negation dependency graph
 * ======================================================================== */

int dl_stratify(dl_program_t* prog) {
    if (!prog) return -1;

    /* Build dependency graph: for each IDB predicate, which other IDB
     * predicates does it depend on positively or negatively? */
    int n = prog->n_rels;
    /* dep[i][j]: 0 = no dep, 1 = positive dep, 2 = negative dep */
    int dep[DL_MAX_RELS][DL_MAX_RELS];
    memset(dep, 0, sizeof(dep));

    for (int r = 0; r < prog->n_rules; r++) {
        dl_rule_t* rule = &prog->rules[r];
        int head_idx = dl_find_rel(prog, rule->head_pred);
        if (head_idx < 0) continue;

        for (int b = 0; b < rule->n_body; b++) {
            dl_body_t* body = &rule->body[b];
            if (body->type == DL_AGG) {
                /* Aggregates are non-monotonic: head must live in a higher
                 * stratum than the predicate being aggregated. */
                int body_idx = dl_find_rel(prog, body->agg_pred);
                if (body_idx < 0) continue;
                dep[head_idx][body_idx] = 2;  /* negative (non-monotonic) dep */
                continue;
            }
            if (body->type != DL_POS && body->type != DL_NEG) continue;
            int body_idx = dl_find_rel(prog, body->pred);
            if (body_idx < 0) continue;
            if (body->type == DL_NEG)
                dep[head_idx][body_idx] = 2;  /* negative dep */
            else if (dep[head_idx][body_idx] == 0)
                dep[head_idx][body_idx] = 1;  /* positive dep (don't override neg) */
        }
    }

    /* Assign strata: predicates with no negative dependencies go to stratum 0.
     * A predicate with a negative dep on stratum S goes to stratum S+1.
     * Repeat until stable. If unstable after n iterations, there's a cycle. */
    int stratum[DL_MAX_RELS];
    memset(stratum, 0, sizeof(stratum));

    for (int iter = 0; iter < n + 1; iter++) {
        bool changed = false;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                if (dep[i][j] == 2) {
                    /* Negative dependency: head must be in higher stratum */
                    if (stratum[i] <= stratum[j]) {
                        stratum[i] = stratum[j] + 1;
                        changed = true;
                    }
                } else if (dep[i][j] == 1) {
                    /* Positive dependency: head must be >= stratum */
                    if (stratum[i] < stratum[j]) {
                        stratum[i] = stratum[j];
                        changed = true;
                    }
                }
            }
        }
        if (!changed) break;
        if (iter == n) return -1;  /* unstratifiable negation cycle */
    }

    /* Build strata arrays */
    int max_stratum = 0;
    for (int i = 0; i < n; i++) {
        if (stratum[i] > max_stratum) max_stratum = stratum[i];
    }
    prog->n_strata = max_stratum + 1;
    memset(prog->strata_sizes, 0, sizeof(prog->strata_sizes));

    for (int i = 0; i < n; i++) {
        int s = stratum[i];
        if (s < DL_MAX_STRATA && prog->strata_sizes[s] < DL_MAX_RELS) {
            prog->strata[s][prog->strata_sizes[s]++] = i;
        }
    }

    /* Assign stratum to each rule */
    for (int r = 0; r < prog->n_rules; r++) {
        int head_idx = dl_find_rel(prog, prog->rules[r].head_pred);
        if (head_idx >= 0)
            prog->rules[r].stratum = stratum[head_idx];
    }

    return 0;
}

/* ========================================================================
 * Rule compiler — materializing approach
 *
 * Instead of building a single graph with joins, we execute each body
 * atom separately, producing intermediate tables, and join them C-level.
 * This avoids column-name-collision issues in the graph-level join.
 * ======================================================================== */

/* ========================================================================
 * Expression evaluation — compute column from expression tree
 * ======================================================================== */

/* Helper: materialize a column of the given type/size as a copy or promotion
 * of src. If target==RAY_F64 and src is RAY_I64, promote. Returns new owned column. */
static ray_t* dl_col_as_f64(ray_t* src, int64_t nrows) {
    ray_t* out = ray_vec_new(RAY_F64, nrows);
    if (!out) return NULL;
    if (RAY_IS_ERR(out)) { ray_error_free(out); return NULL; }
    out->len = nrows;
    double* od = (double*)ray_data(out);
    if (src->type == RAY_F64) {
        memcpy(od, ray_data(src), (size_t)nrows * sizeof(double));
    } else { /* RAY_I64 */
        int64_t* sd = (int64_t*)ray_data(src);
        for (int64_t r = 0; r < nrows; r++) od[r] = (double)sd[r];
    }
    return out;
}

/* Evaluate an expression tree against the accumulator table.
 * Returns a new owned vector of length nrows. The element type is RAY_F64
 * if the expression involves any float constant or any RAY_F64 source column,
 * otherwise RAY_I64. */
static ray_t* dl_eval_expr(dl_expr_t* expr, ray_t* accum,
                             int* var_col, int64_t nrows) {
    if (!expr) return NULL;

    switch (expr->kind) {
    case DL_EXPR_CONST: {
        ray_t* col = ray_vec_new(RAY_I64, nrows);
        if (!col) return NULL;
        if (RAY_IS_ERR(col)) { ray_error_free(col); return NULL; }
        col->len = nrows;
        int64_t* d = (int64_t*)ray_data(col);
        for (int64_t r = 0; r < nrows; r++)
            d[r] = expr->const_val;
        return col;
    }
    case DL_EXPR_CONST_F64: {
        ray_t* col = ray_vec_new(RAY_F64, nrows);
        if (!col) return NULL;
        if (RAY_IS_ERR(col)) { ray_error_free(col); return NULL; }
        col->len = nrows;
        double* d = (double*)ray_data(col);
        for (int64_t r = 0; r < nrows; r++)
            d[r] = expr->const_f64;
        return col;
    }
    case DL_EXPR_VAR: {
        int ci = var_col[expr->var_idx];
        ray_t* src = ray_table_get_col_idx(accum, ci);
        if (!src) return NULL;
        if (src->type != RAY_I64 && src->type != RAY_F64) return NULL;
        size_t elem = (src->type == RAY_F64) ? sizeof(double) : sizeof(int64_t);
        ray_t* dst = ray_vec_new(src->type, nrows);
        if (!dst) return NULL;
        if (RAY_IS_ERR(dst)) { ray_error_free(dst); return NULL; }
        dst->len = nrows;
        memcpy(ray_data(dst), ray_data(src), (size_t)nrows * elem);
        return dst;
    }
    case DL_EXPR_BINOP: {
        ray_t* lv = dl_eval_expr(expr->left, accum, var_col, nrows);
        ray_t* rv = dl_eval_expr(expr->right, accum, var_col, nrows);
        if (!lv || !rv) {
            if (lv) ray_release(lv);
            if (rv) ray_release(rv);
            return NULL;
        }
        bool is_f64 = (lv->type == RAY_F64) || (rv->type == RAY_F64);
        if (is_f64) {
            ray_t* lf = dl_col_as_f64(lv, nrows);
            ray_t* rf = dl_col_as_f64(rv, nrows);
            ray_release(lv); ray_release(rv);
            if (!lf || !rf) {
                if (lf) ray_release(lf);
                if (rf) ray_release(rf);
                return NULL;
            }
            ray_t* out = ray_vec_new(RAY_F64, nrows);
            if (!out) { ray_release(lf); ray_release(rf); return NULL; }
            if (RAY_IS_ERR(out)) {
                ray_error_free(out);
                ray_release(lf); ray_release(rf); return NULL;
            }
            out->len = nrows;
            double* ld = (double*)ray_data(lf);
            double* rd = (double*)ray_data(rf);
            double* od = (double*)ray_data(out);
            for (int64_t r = 0; r < nrows; r++) {
                switch (expr->binop) {
                case OP_ADD: od[r] = ld[r] + rd[r]; break;
                case OP_SUB: od[r] = ld[r] - rd[r]; break;
                case OP_MUL: od[r] = ld[r] * rd[r]; break;
                case OP_DIV: od[r] = rd[r] != 0.0 ? ld[r] / rd[r] : 0.0; break;
                default:     od[r] = 0.0; break;
                }
            }
            ray_release(lf); ray_release(rf);
            return out;
        }
        ray_t* out = ray_vec_new(RAY_I64, nrows);
        if (!out) { ray_release(lv); ray_release(rv); return NULL; }
        if (RAY_IS_ERR(out)) {
            ray_error_free(out);
            ray_release(lv); ray_release(rv); return NULL;
        }
        out->len = nrows;
        int64_t* ld = (int64_t*)ray_data(lv);
        int64_t* rd = (int64_t*)ray_data(rv);
        int64_t* od = (int64_t*)ray_data(out);
        for (int64_t r = 0; r < nrows; r++) {
            switch (expr->binop) {
            case OP_ADD: od[r] = ld[r] + rd[r]; break;
            case OP_SUB: od[r] = ld[r] - rd[r]; break;
            case OP_MUL: od[r] = ld[r] * rd[r]; break;
            case OP_DIV: od[r] = rd[r] != 0 ? ld[r] / rd[r] : 0; break;
            default:     od[r] = 0; break;
            }
        }
        ray_release(lv);
        ray_release(rv);
        return out;
    }
    }
    return NULL;
}

/* Helper: append a new column to a table. Returns new owned table. */
static ray_t* dl_table_add_computed_col(ray_t* tbl, ray_t* new_col, const char* name) {
    int64_t ncols = ray_table_ncols(tbl);
    ray_t* out = ray_table_new((int)(ncols + 1));
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col)
            out = ray_table_add_col(out, ray_table_col_name(tbl, c), col);
    }
    int64_t sym = ray_sym_intern(name, strlen(name));
    out = ray_table_add_col(out, sym, new_col);
    return out;
}

/* ========================================================================
 * Builtin predicate evaluation helpers
 * ======================================================================== */

/* before(S, E, T): keep rows where T < S */
static ray_t* dl_builtin_before(ray_t* tbl, int s_col, int t_col) {
    if (!tbl || RAY_IS_ERR(tbl) || ray_table_nrows(tbl) == 0) return tbl;

    int64_t nrows = ray_table_nrows(tbl);
    int64_t ncols = ray_table_ncols(tbl);
    int64_t* sd = (int64_t*)ray_data(ray_table_get_col_idx(tbl, s_col));
    int64_t* t_data = (int64_t*)ray_data(ray_table_get_col_idx(tbl, t_col));

    int64_t count = 0;
    for (int64_t r = 0; r < nrows; r++)
        if (t_data[r] < sd[r]) count++;

    if (count == nrows) { ray_retain(tbl); return tbl; }

    ray_t* out = ray_table_new((int)ncols);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* src = ray_table_get_col_idx(tbl, c);
        if (!src) continue;
        ray_t* dst = ray_vec_new(src->type, count);
        if (!dst || RAY_IS_ERR(dst)) continue;
        dst->len = count;
        int64_t* src_d = (int64_t*)ray_data(src);
        int64_t* dst_d = (int64_t*)ray_data(dst);
        int64_t j = 0;
        for (int64_t r = 0; r < nrows; r++)
            if (t_data[r] < sd[r])
                dst_d[j++] = src_d[r];
        out = ray_table_add_col(out, ray_table_col_name(tbl, c), dst);
        ray_release(dst);
    }
    return out;
}

/* duration_since(T1, T2, D): compute D = T2 - T1, append as new column */
static ray_t* dl_builtin_duration_since(ray_t* tbl, int t1_col, int t2_col,
                                          const char* out_name) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nrows = ray_table_nrows(tbl);
    int64_t* t1 = (int64_t*)ray_data(ray_table_get_col_idx(tbl, t1_col));
    int64_t* t2 = (int64_t*)ray_data(ray_table_get_col_idx(tbl, t2_col));

    ray_t* col = ray_vec_new(RAY_I64, nrows);
    if (!col || RAY_IS_ERR(col)) { ray_retain(tbl); return tbl; }
    col->len = nrows;
    int64_t* d = (int64_t*)ray_data(col);
    for (int64_t r = 0; r < nrows; r++)
        d[r] = t2[r] - t1[r];

    ray_t* out = dl_table_add_computed_col(tbl, col, out_name);
    ray_release(col);
    return out;
}

/* abs(X, Y): compute Y = |X|, append as new column */
static ray_t* dl_builtin_abs(ray_t* tbl, int x_col, const char* out_name) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nrows = ray_table_nrows(tbl);
    int64_t* xd = (int64_t*)ray_data(ray_table_get_col_idx(tbl, x_col));

    ray_t* col = ray_vec_new(RAY_I64, nrows);
    if (!col || RAY_IS_ERR(col)) { ray_retain(tbl); return tbl; }
    col->len = nrows;
    int64_t* d = (int64_t*)ray_data(col);
    for (int64_t r = 0; r < nrows; r++)
        d[r] = xd[r] < 0 ? -xd[r] : xd[r];

    ray_t* out = dl_table_add_computed_col(tbl, col, out_name);
    ray_release(col);
    return out;
}

/* Helper: join two tables on specified column pairs. Returns new owned table.
 * left_cols[k] and right_cols[k] are column indices in left/right tables. */
static ray_t* dl_join_tables(ray_t* left, ray_t* right,
                              const int* left_cols, const int* right_cols, int n_keys) {
    if (!left || RAY_IS_ERR(left) || !right || RAY_IS_ERR(right)) return NULL;
    if (ray_table_nrows(left) == 0 || ray_table_nrows(right) == 0) {
        /* Return empty table with left+right non-key columns */
        int64_t lnc = ray_table_ncols(left);
        int64_t rnc = ray_table_ncols(right);
        ray_t* empty = ray_table_new((int)(lnc + rnc));
        for (int64_t c = 0; c < lnc; c++) {
            ray_t* col = ray_table_get_col_idx(left, c);
            if (!col) continue;
            ray_t* ec = ray_vec_new(col->type, 0);
            if (ec && !RAY_IS_ERR(ec)) {
                empty = ray_table_add_col(empty, ray_table_col_name(left, c), ec);
                ray_release(ec);
            }
        }
        return empty;
    }

    /* Build unique column names for the join using a single graph */
    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return NULL;

    /* Create copies with unique names */
    int64_t lnc = ray_table_ncols(left);
    int64_t rnc = ray_table_ncols(right);
    ray_t* ltbl = ray_table_new((int)lnc);
    for (int64_t c = 0; c < lnc; c++) {
        ray_t* col = ray_table_get_col_idx(left, c);
        if (!col) continue;
        char name[32]; snprintf(name, sizeof(name), "L%d", (int)c);
        int64_t sym = ray_sym_intern(name, strlen(name));
        ltbl = ray_table_add_col(ltbl, sym, col);
    }
    ray_t* rtbl = ray_table_new((int)rnc);
    for (int64_t c = 0; c < rnc; c++) {
        ray_t* col = ray_table_get_col_idx(right, c);
        if (!col) continue;
        char name[32]; snprintf(name, sizeof(name), "R%d", (int)c);
        int64_t sym = ray_sym_intern(name, strlen(name));
        rtbl = ray_table_add_col(rtbl, sym, col);
    }

    uint16_t l_tid = ray_graph_add_table(g, ltbl);
    uint16_t r_tid = ray_graph_add_table(g, rtbl);
    ray_op_t* l_op = ray_const_table(g, ltbl);
    ray_op_t* r_op = ray_const_table(g, rtbl);

    ray_op_t* lkeys[DL_MAX_ARITY];
    ray_op_t* rkeys[DL_MAX_ARITY];
    for (int k = 0; k < n_keys; k++) {
        char lname[32]; snprintf(lname, sizeof(lname), "L%d", left_cols[k]);
        char rname[32]; snprintf(rname, sizeof(rname), "R%d", right_cols[k]);
        lkeys[k] = ray_scan_table(g, l_tid, lname);
        rkeys[k] = ray_scan_table(g, r_tid, rname);
    }

    ray_op_t* join = ray_join(g, l_op, lkeys, r_op, rkeys, (uint8_t)n_keys, 0);
    ray_t* result = ray_execute(g, join);
    ray_graph_free(g);
    ray_release(ltbl);
    ray_release(rtbl);
    return result;
}

/* Helper: antijoin two tables on specified column pairs. Returns new owned table. */
static ray_t* dl_antijoin_tables(ray_t* left, ray_t* right,
                                  const int* left_cols, const int* right_cols, int n_keys) {
    if (!left || RAY_IS_ERR(left)) return left;
    if (!right || RAY_IS_ERR(right) || ray_table_nrows(right) == 0) {
        ray_retain(left); return left;
    }
    if (ray_table_nrows(left) == 0) { ray_retain(left); return left; }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) { ray_retain(left); return left; }

    int64_t lnc = ray_table_ncols(left);
    int64_t rnc = ray_table_ncols(right);
    ray_t* ltbl = ray_table_new((int)lnc);
    for (int64_t c = 0; c < lnc; c++) {
        ray_t* col = ray_table_get_col_idx(left, c);
        if (!col) continue;
        char name[32]; snprintf(name, sizeof(name), "L%d", (int)c);
        ltbl = ray_table_add_col(ltbl, ray_sym_intern(name, strlen(name)), col);
    }
    ray_t* rtbl = ray_table_new((int)rnc);
    for (int64_t c = 0; c < rnc; c++) {
        ray_t* col = ray_table_get_col_idx(right, c);
        if (!col) continue;
        char name[32]; snprintf(name, sizeof(name), "R%d", (int)c);
        rtbl = ray_table_add_col(rtbl, ray_sym_intern(name, strlen(name)), col);
    }

    uint16_t l_tid = ray_graph_add_table(g, ltbl);
    uint16_t r_tid = ray_graph_add_table(g, rtbl);
    ray_op_t* l_op = ray_const_table(g, ltbl);
    ray_op_t* r_op = ray_const_table(g, rtbl);

    ray_op_t* lkeys[DL_MAX_ARITY];
    ray_op_t* rkeys[DL_MAX_ARITY];
    for (int k = 0; k < n_keys; k++) {
        char lname[32]; snprintf(lname, sizeof(lname), "L%d", left_cols[k]);
        char rname[32]; snprintf(rname, sizeof(rname), "R%d", right_cols[k]);
        lkeys[k] = ray_scan_table(g, l_tid, lname);
        rkeys[k] = ray_scan_table(g, r_tid, rname);
    }

    ray_op_t* aj = ray_antijoin(g, l_op, lkeys, r_op, rkeys, (uint8_t)n_keys);
    ray_t* result = ray_execute(g, aj);
    ray_graph_free(g);
    ray_release(ltbl);
    ray_release(rtbl);
    return result;
}

/* Helper: filter a table to rows where column col_idx == value.
 * `const_type` is the source ray type of the body literal (RAY_STR /
 * RAY_SYM / RAY_I64 / RAY_F64) so the row-equality helper can dispatch
 * a tag-aware compare for DATOM-encoded I64 columns — see
 * dl_col_eq_row below. */
/* Row-at-index read helper: read an I64 from either a RAY_I64 column
 * or from a RAY_SYM column (of any adaptive width) as a sym ID.  Other
 * types aren't supported by the constant-filter path and cause the
 * caller to pass through the input table unchanged.
 *
 * RAY_I64 columns may hold either plain integers or DATOM-tagged sym
 * IDs (`(0x4000... | sym_id)` for STR, `(0x2000... | sym_id)` for SYM)
 * deposited by an EAV frontend. The body literal's source ray type
 * (`const_type`) is the only signal we have for which encoding the
 * caller intended: a `"foo"` literal interns as a plain sym ID but
 * targets a STR-tagged cell; a `'foo` literal interns plain and
 * targets a SYM-tagged cell. We always try the direct compare first
 * (plain-int columns and same-tag columns hit this path) and fall
 * back to a payload compare when the cell carries the matching tag —
 * so a body literal can pin both an untagged RAY_SYM column built
 * from a rule head and a DATOM-tagged RAY_I64 column built from EAV
 * storage without the frontend having to know which is which. */
static bool dl_col_eq_row(ray_t* col, int64_t row, int64_t value,
                          int8_t const_type) {
    if (col->type == RAY_I64) {
        int64_t cell = ((int64_t*)ray_data(col))[row];
        if (cell == value) return true;
        int64_t cell_tag = cell & (int64_t)0x6000000000000000;
        if (cell_tag == 0) return false;  /* plain int column */
        int64_t cell_payload = cell & (int64_t)0x1FFFFFFFFFFFFFFF;
        if (const_type == RAY_STR &&
            cell_tag == (int64_t)0x4000000000000000)
            return cell_payload == value;
        if (const_type == RAY_SYM &&
            cell_tag == (int64_t)0x2000000000000000)
            return cell_payload == value;
        return false;
    }
    if (col->type == RAY_SYM)
        return ray_read_sym(ray_data(col), row, col->type, col->attrs) == value;
    return false;
}

static ray_t* dl_filter_eq(ray_t* tbl, int col_idx, int64_t value,
                           int8_t const_type) {
    /* Contract: always return an owned reference (rc bumped) so the
     * caller can release uniformly.  Every pass-through must therefore
     * retain — else the caller's `ray_release(body_tbl); body_tbl =
     * filtered;` pattern would leave body_tbl under-referenced and a
     * later release could land on freed memory. */
    if (!tbl || RAY_IS_ERR(tbl)) { if (tbl) ray_retain(tbl); return tbl; }
    if (ray_table_nrows(tbl) == 0) { ray_retain(tbl); return tbl; }

    ray_t* col = ray_table_get_col_idx(tbl, col_idx);
    if (!col) { ray_retain(tbl); return tbl; }
    /* Non-numeric, non-sym keys: not supported by this filter — pass
     * through (retained) rather than miscompare via raw memcpy. */
    if (col->type != RAY_I64 && col->type != RAY_SYM) {
        ray_retain(tbl);
        return tbl;
    }

    int64_t nrows = ray_table_nrows(tbl);
    int64_t ncols = ray_table_ncols(tbl);

    /* Count matching rows — type-aware read for RAY_SYM adaptive width. */
    int64_t count = 0;
    for (int64_t r = 0; r < nrows; r++)
        if (dl_col_eq_row(col, r, value, const_type)) count++;

    if (count == nrows) { ray_retain(tbl); return tbl; }

    /* Build filtered table.  Each surviving column is allocated with
     * its source's element-size (via ray_sym_elem_size) so narrow-SYM
     * stays narrow rather than being silently widened to W64. */
    ray_t* out = ray_table_new((int)ncols);
    if (!out) return ray_error("memory", "dl_filter_eq: table_new");
    if (RAY_IS_ERR(out)) return out;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* src = ray_table_get_col_idx(tbl, c);
        if (!src) {
            ray_release(out);
            return ray_error("domain", "dl_filter_eq: missing source column");
        }
        ray_t* dst = (src->type == RAY_SYM)
            ? ray_sym_vec_new(src->attrs & RAY_SYM_W_MASK, count)
            : ray_vec_new(src->type, count);
        if (!dst) { ray_release(out); return ray_error("memory", "dl_filter_eq: vec_new"); }
        if (RAY_IS_ERR(dst)) { ray_error_free(dst); ray_release(out); return ray_error("memory", "dl_filter_eq: vec_new"); }
        dst->len = count;
        uint8_t esz = ray_sym_elem_size(src->type, src->attrs);
        const uint8_t* src_b = (const uint8_t*)ray_data(src);
        uint8_t* dst_b = (uint8_t*)ray_data(dst);
        int64_t j = 0;
        for (int64_t r = 0; r < nrows; r++) {
            if (dl_col_eq_row(col, r, value, const_type)) {
                memcpy(dst_b + (size_t)j * esz,
                       src_b + (size_t)r * esz,
                       (size_t)esz);
                j++;
            }
        }
        if (src->type == RAY_STR) col_propagate_str_pool(dst, src);
        ray_t* next = ray_table_add_col(out, ray_table_col_name(tbl, c), dst);
        ray_release(dst);
        /* ray_table_add_col does not release `out` on failure, so we
         * must release the partially-built table before bailing out. */
        if (!next) {
            ray_release(out);
            return ray_error("memory", "dl_filter_eq: add_col");
        }
        if (RAY_IS_ERR(next)) {
            ray_release(out);
            return next;
        }
        out = next;
    }
    return out;
}

/* Helper: build a fully-owned broadcast column for a constant head slot.
 *
 * Returns a fresh ray_t* vec with refcount 1, caller-owned.  The caller is
 * expected to hand the ref to a table via ray_table_add_col (which retains)
 * and then ray_release our owning ref, leaving the table as sole owner.
 *
 * Correctness note: this must be a real, heap-allocated vec — not a view
 * onto rule-local scratch — so that the IDB relation table can outlive the
 * per-iteration scratch that built it.  Cross-IDB reads at subsequent
 * strata borrow from this column via ray_table_get_col_idx. */
/* width_template: when type == RAY_SYM, this column is consulted for its
 * SYM attrs/width so the broadcast matches the IDB relation's existing
 * adaptive width (otherwise ray_vec_new would default to W64 and a
 * later table_union would hit a ray_vec_concat width mismatch).  Pass
 * NULL (no existing column) to get the W64 default.  Using a pointer
 * here rather than a uint8_t hint avoids the W8=0 sentinel ambiguity
 * of an "a zero hint means default" convention. */
static ray_t* dl_broadcast_const_col(int64_t nrows, int8_t type, int64_t val,
                                      const ray_t* width_template) {
    if (type != RAY_I64 && type != RAY_SYM && type != RAY_F64) {
        return ray_error("type", NULL);
    }
    uint8_t sym_w = RAY_SYM_W64;
    if (type == RAY_SYM && width_template && width_template->type == RAY_SYM)
        sym_w = width_template->attrs & RAY_SYM_W_MASK;
    ray_t* v = (type == RAY_SYM)
        ? ray_sym_vec_new(sym_w, nrows)
        : ray_vec_new(type, nrows);
    if (!v || RAY_IS_ERR(v)) return v;
    v->len = nrows;

    if (type == RAY_SYM) {
        /* Use the generic writer so it handles any adaptive width. */
        void* data = ray_data(v);
        for (int64_t i = 0; i < nrows; i++) {
            ray_write_sym(data, i, (uint64_t)val, v->type, v->attrs);
        }
    } else if (type == RAY_F64) {
        double d;
        memcpy(&d, &val, sizeof(d));
        double* data = (double*)ray_data(v);
        for (int64_t i = 0; i < nrows; i++) data[i] = d;
    } else {  /* RAY_I64 */
        int64_t* data = (int64_t*)ray_data(v);
        for (int64_t i = 0; i < nrows; i++) data[i] = val;
    }
    return v;
}

/* Helper: project table to selected columns, producing output with head relation naming.
 *
 * For each output slot c:
 *   - if col_indices[c] >= 0, copy that column from `tbl`
 *   - else (constant slot), synthesize a broadcast column from head_consts[c]
 *     with type head_const_types[c]. */
static ray_t* dl_project(ray_t* tbl, const int* col_indices, int n_out,
                          dl_rel_t* head_rel, const int64_t* head_consts,
                          const int8_t* head_const_types) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nrows = ray_table_nrows(tbl);
    ray_t* out = ray_table_new(n_out);
    if (!out || RAY_IS_ERR(out))
        return out ? out : ray_error("memory", "dl_project: table_new");
    /* If accum collapsed to zero rows (e.g. antijoin removed everything),
     * its schema may have been dropped too.  Fall back to the IDB's existing
     * column types so downstream table_union sees a matching schema. */
    bool empty_accum = (nrows == 0);
    for (int c = 0; c < n_out; c++) {
        int src_idx = col_indices[c];
        if (src_idx >= 0) {
            ray_t* src = ray_table_get_col_idx(tbl, src_idx);
            if (!src) {
                if (empty_accum && head_rel && head_rel->table) {
                    ray_t* hcol = ray_table_get_col_idx(head_rel->table, c);
                    int8_t htype = hcol ? hcol->type : RAY_I64;
                    /* For SYM columns, preserve the head-relation's
                     * adaptive-width attrs — ray_vec_new(RAY_SYM, …) would
                     * force W64 and a later table_union onto a narrower
                     * head-rel column would hit the column-count check,
                     * or worse, produce a width-mismatched merge. */
                    ray_t* ecol = (htype == RAY_SYM && hcol)
                        ? ray_sym_vec_new(hcol->attrs & RAY_SYM_W_MASK, 0)
                        : ray_vec_new(htype, 0);
                    if (!ecol) {
                        ray_release(out);
                        return ray_error("memory", "dl_project: empty col");
                    }
                    if (RAY_IS_ERR(ecol)) {
                        ray_error_free(ecol);
                        ray_release(out);
                        return ray_error("memory", "dl_project: empty col");
                    }
                    ray_t* next = ray_table_add_col(out, head_rel->col_names[c], ecol);
                    ray_release(ecol);
                    if (!next) {
                        ray_release(out);
                        return ray_error("memory", "dl_project: add_col");
                    }
                    if (RAY_IS_ERR(next)) {
                        ray_release(out);
                        return next;
                    }
                    out = next;
                    continue;
                }
                ray_release(out);
                return ray_error("domain", "dl_project: source column missing");
            }
            /* Preserve SYM index width: ray_vec_new(RAY_SYM, …) would always
             * produce a W64 vec, so memcpy'ing with the source's narrower
             * element size would leave the upper bytes of each W64 slot
             * uninitialized.  ray_sym_vec_new mirrors src's attrs width. */
            ray_t* dst = (src->type == RAY_SYM)
                ? ray_sym_vec_new(src->attrs & RAY_SYM_W_MASK, nrows)
                : ray_vec_new(src->type, nrows);
            if (!dst) {
                ray_release(out);
                return ray_error("memory", "dl_project: vec_new");
            }
            if (RAY_IS_ERR(dst)) {
                ray_error_free(dst);
                ray_release(out);
                return ray_error("memory", "dl_project: vec_new");
            }
            dst->len = nrows;
            uint8_t esz = ray_sym_elem_size(src->type, src->attrs);
            if (esz == 0) {
                ray_release(dst);
                ray_release(out);
                return ray_error("type", "dl_project: unsupported column type");
            }
            memcpy(ray_data(dst), ray_data(src), (size_t)nrows * (size_t)esz);
            /* RAY_STR stores 16-byte ray_str_t handles inline; strings >12
             * bytes keep their bytes in a per-vector pool referenced via
             * pool_off.  The memcpy above copies the handles but not the
             * pool, so propagate the source's pool onto dst or later
             * reads through pool_off would land in a NULL pool. */
            if (src->type == RAY_STR) col_propagate_str_pool(dst, src);
            ray_t* next = ray_table_add_col(out, head_rel->col_names[c], dst);
            ray_release(dst);
            /* Release the partial `out` on failure — ray_table_add_col
             * does not free its input on error. */
            if (!next) {
                ray_release(out);
                return ray_error("memory", "dl_project: add_col");
            }
            if (RAY_IS_ERR(next)) {
                ray_release(out);
                return next;
            }
            out = next;
        } else {
            /* Constant head slot: materialize an owned broadcast column. */
            int8_t ctype = head_const_types ? head_const_types[c] : 0;
            if (ctype == 0) {
                ray_release(out);
                return ray_error("domain", "dl_project: unset head-const type");
            }
            /* When the head relation's slot is an existing SYM column
             * (from a prior aligned rule), match its width so
             * table_union's ray_vec_concat doesn't reject a W64 vs
             * narrow mismatch. */
            const ray_t* width_tpl = NULL;
            if (ctype == RAY_SYM && head_rel && head_rel->table)
                width_tpl = ray_table_get_col_idx(head_rel->table, c);
            ray_t* bcast = dl_broadcast_const_col(nrows, ctype, head_consts[c], width_tpl);
            if (!bcast || RAY_IS_ERR(bcast)) {
                ray_release(out);
                return bcast ? bcast : ray_error("memory", "dl_project: broadcast");
            }
            ray_t* next = ray_table_add_col(out, head_rel->col_names[c], bcast);
            ray_release(bcast);
            if (!next) {
                ray_release(out);
                return ray_error("memory", "dl_project: add_col");
            }
            if (RAY_IS_ERR(next)) {
                ray_release(out);
                return next;
            }
            out = next;
        }
    }
    return out;
}

ray_op_t* dl_compile_rule(dl_program_t* prog, dl_rule_t* rule,
                          int delta_pos, int rule_idx, ray_graph_t* g) {
    /* Materializing approach: execute body atoms one at a time.
     *
     * For each positive body atom, we get the relation table and apply
     * constant filters. Then join with the accumulated result.
     * Variable bindings track which column in the accumulated table
     * holds each variable's value.
     *
     * var_col[v] = column index in `accum` table for variable v.
     */
    int var_col[DL_MAX_ARITY * DL_MAX_BODY];  /* column index in accum per variable */
    bool var_bound[DL_MAX_ARITY * DL_MAX_BODY];
    memset(var_bound, 0, sizeof(var_bound));
    memset(var_col, -1, sizeof(var_col));

    ray_t* accum = NULL;  /* accumulated result table */

    for (int b = 0; b < rule->n_body; b++) {
        dl_body_t* body = &rule->body[b];
        if (body->type != DL_POS) continue;

        int rel_idx = dl_find_rel(prog, body->pred);
        if (rel_idx < 0) { if (accum) ray_release(accum); return NULL; }
        dl_rel_t* rel = &prog->rels[rel_idx];
        ray_t* body_tbl = rel->table;
        ray_retain(body_tbl);

        /* Apply constant filters */
        for (int c = 0; c < body->arity; c++) {
            if (body->vars[c] == DL_CONST) {
                ray_t* filtered = dl_filter_eq(body_tbl, c, body->const_vals[c],
                                                body->const_types[c]);
                ray_release(body_tbl);
                if (!filtered) {
                    /* Treat as genuine failure — dl_filter_eq returns an
                     * owned reference on every non-NULL path, so NULL
                     * means something went wrong inside the helper. */
                    if (accum) ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }
                if (RAY_IS_ERR(filtered)) {
                    ray_error_free(filtered);
                    if (accum) ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }
                body_tbl = filtered;
            }
        }

        if (accum == NULL) {
            /* First body atom: accum = body_tbl */
            accum = body_tbl;
            /* Bind variables to column indices */
            for (int c = 0; c < body->arity; c++) {
                int v = body->vars[c];
                if (v == DL_CONST) continue;
                if (!var_bound[v]) {
                    var_bound[v] = true;
                    var_col[v] = c;
                }
            }
        } else {
            /* Join accum with body_tbl on shared variables */
            int lkeys[DL_MAX_ARITY], rkeys[DL_MAX_ARITY];
            int n_jk = 0;
            for (int c = 0; c < body->arity; c++) {
                int v = body->vars[c];
                if (v == DL_CONST) continue;
                if (var_bound[v]) {
                    lkeys[n_jk] = var_col[v];
                    rkeys[n_jk] = c;
                    n_jk++;
                }
            }

            ray_t* joined;
            if (n_jk > 0) {
                joined = dl_join_tables(accum, body_tbl, lkeys, rkeys, n_jk);
            } else {
                /* Cross product: use dummy key */
                int lk0 = 0, rk0 = 0;
                joined = dl_join_tables(accum, body_tbl, &lk0, &rk0, 0);
            }

            int64_t accum_ncols = ray_table_ncols(accum);
            ray_release(accum);
            ray_release(body_tbl);
            accum = joined;

            /* Bind new variables: their columns come after left columns in join output.
             * Join output = [all left cols] + [non-key right cols].
             * We need to track which right columns appear in output. */
            int right_col_map[DL_MAX_ARITY]; /* right col c -> output col idx */
            int out_idx = (int)accum_ncols;
            for (int c = 0; c < body->arity; c++) {
                bool is_key = false;
                for (int k = 0; k < n_jk; k++) {
                    if (rkeys[k] == c) { is_key = true; break; }
                }
                if (is_key) {
                    right_col_map[c] = -1;  /* key col not in output */
                } else {
                    right_col_map[c] = out_idx++;
                }
            }

            for (int c = 0; c < body->arity; c++) {
                int v = body->vars[c];
                if (v == DL_CONST) continue;
                if (!var_bound[v]) {
                    var_bound[v] = true;
                    var_col[v] = right_col_map[c];
                }
            }
        }
    }

    /* Rules with only aggregates (no positive body atoms) still need a
     * one-row binding environment so aggregate results can be projected. */
    if (!accum) {
        bool has_agg = false;
        for (int bi = 0; bi < rule->n_body; bi++) {
            if (rule->body[bi].type == DL_AGG) {
                has_agg = true;
                break;
            }
        }
        if (!has_agg)
            return NULL;
        ray_t* one_val = ray_vec_new(RAY_I64, 1);
        if (!one_val) { prog->eval_err = true; return NULL; }
        if (RAY_IS_ERR(one_val)) {
            ray_error_free(one_val);
            prog->eval_err = true;
            return NULL;
        }
        one_val->len = 1;
        ((int64_t*)ray_data(one_val))[0] = 0;
        accum = ray_table_new(1);
        if (!accum) {
            ray_release(one_val);
            prog->eval_err = true;
            return NULL;
        }
        if (RAY_IS_ERR(accum)) {
            ray_error_free(accum);
            ray_release(one_val);
            prog->eval_err = true;
            return NULL;
        }
        int64_t unit_sym = ray_sym_intern("_unit", 5);
        ray_t* accum_unit = ray_table_add_col(accum, unit_sym, one_val);
        ray_release(one_val);
        /* ray_table_add_col doesn't free `accum` on error — release it
         * ourselves so the partially-built table isn't leaked. */
        if (!accum_unit) {
            ray_release(accum);
            prog->eval_err = true;
            return NULL;
        }
        if (RAY_IS_ERR(accum_unit)) {
            ray_error_free(accum_unit);
            ray_release(accum);
            prog->eval_err = true;
            return NULL;
        }
        accum = accum_unit;
    }

    if (!accum) return NULL;

    /* Process non-join body literals in declared order.
     * This ensures dependencies between literals (e.g., interval bind before
     * assignment, assignment before comparison) are respected. */
    for (int b = 0; b < rule->n_body; b++) {
        dl_body_t* body = &rule->body[b];
        if (body->type == DL_POS) continue;  /* already processed above */
        if (!accum || RAY_IS_ERR(accum)) break;

        switch (body->type) {
        case DL_NEG: {
            int rel_idx = dl_find_rel(prog, body->pred);
            if (rel_idx < 0) { ray_release(accum); return NULL; }
            dl_rel_t* rel = &prog->rels[rel_idx];

            /* Apply constant filters to the negated relation first */
            ray_t* neg_tbl = rel->table;
            ray_retain(neg_tbl);
            for (int c = 0; c < body->arity; c++) {
                if (body->vars[c] == DL_CONST) {
                    ray_t* filtered = dl_filter_eq(neg_tbl, c, body->const_vals[c],
                                                    body->const_types[c]);
                    ray_release(neg_tbl);
                    if (!filtered) {
                        ray_release(accum);
                        prog->eval_err = true;
                        return NULL;
                    }
                    if (RAY_IS_ERR(filtered)) {
                        ray_error_free(filtered);
                        ray_release(accum);
                        prog->eval_err = true;
                        return NULL;
                    }
                    neg_tbl = filtered;
                }
            }

            int lkeys[DL_MAX_ARITY], rkeys[DL_MAX_ARITY];
            int n_keys = 0;
            for (int c = 0; c < body->arity; c++) {
                int v = body->vars[c];
                if (v == DL_CONST) continue;
                if (var_bound[v]) {
                    lkeys[n_keys] = var_col[v];
                    rkeys[n_keys] = c;
                    n_keys++;
                }
            }

            if (n_keys > 0) {
                ray_t* result = dl_antijoin_tables(accum, neg_tbl, lkeys, rkeys, n_keys);
                ray_release(accum);
                accum = result;
            }
            ray_release(neg_tbl);
            break;
        }

        case DL_ASSIGN: {
            int64_t nrows = ray_table_nrows(accum);
            ray_t* new_col = dl_eval_expr(body->assign_expr, accum, var_col, nrows);
            /* Silently breaking would leave assign_var unbound and let
             * the rest of the rule keep compiling with stale bindings,
             * producing a dl_eval == 0 return alongside wrong rows. */
            if (!new_col) {
                ray_release(accum);
                prog->eval_err = true;
                return NULL;
            }
            if (RAY_IS_ERR(new_col)) {
                ray_error_free(new_col);
                ray_release(accum);
                prog->eval_err = true;
                return NULL;
            }

            int new_col_idx = (int)ray_table_ncols(accum);
            char colname[32];
            snprintf(colname, sizeof(colname), "_a%d", body->assign_var);
            ray_t* new_accum = dl_table_add_computed_col(accum, new_col, colname);
            ray_release(new_col);
            ray_release(accum);
            if (!new_accum) { prog->eval_err = true; return NULL; }
            if (RAY_IS_ERR(new_accum)) {
                ray_error_free(new_accum);
                prog->eval_err = true;
                return NULL;
            }
            accum = new_accum;

            var_bound[body->assign_var] = true;
            var_col[body->assign_var] = new_col_idx;
            break;
        }

        case DL_AGG: {
            if (body->agg_n_group_keys > 0) {
                /* Grouped aggregation: use rayforce's ray_group on src_table.
                 *
                 * Mixed-rule guard: this path assumes accum is the singleton
                 * _unit placeholder created for aggregate-only rules. If the
                 * rule has real positive body atoms, accum carries bound
                 * variables from a prior join that we would need to intersect
                 * against the group result — not yet supported. Bail early. */
                bool has_pos = false;
                for (int bi = 0; bi < rule->n_body; bi++) {
                    if (rule->body[bi].type == DL_POS) { has_pos = true; break; }
                }
                if (has_pos) {
                    /* nyi: grouped aggregate + positive body atoms.
                     * Surface via eval_err so dl_eval reports failure
                     * instead of writing a warning to stderr in a
                     * non-debug build. */
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }

                int src_idx = dl_find_rel(prog, body->agg_pred);
                if (src_idx < 0) { ray_release(accum); return NULL; }
                ray_t* src_table = prog->rels[src_idx].table;
                int64_t src_nrows = (src_table && !RAY_IS_ERR(src_table))
                    ? ray_table_nrows(src_table) : 0;
                if (src_nrows == 0) {
                    /* No source rows -> no groups -> rule produces no head tuples. */
                    ray_release(accum);
                    return NULL;
                }

                dl_rel_t* src_rel = &prog->rels[src_idx];
                int nk = body->agg_n_group_keys;

                /* Build a sub-graph that SCANs src_table's columns by symbol name.
                 * ray_graph_new retains src_table internally; no extra retain needed. */
                ray_graph_t* gg = ray_graph_new(src_table);
                if (!gg) {
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }

                ray_op_t* keys_ops[DL_AGG_MAX_KEYS];
                for (int i = 0; i < nk; i++) {
                    int kc = body->agg_group_key_cols[i];
                    if (kc < 0 || kc >= src_rel->arity) {
                        ray_graph_free(gg);
                        ray_release(accum);
                        prog->eval_err = true;
                        return NULL;
                    }
                    int64_t sym = src_rel->col_names[kc];
                    ray_t* s = ray_sym_str(sym);
                    if (!s) {
                        ray_graph_free(gg);
                        ray_release(accum);
                        prog->eval_err = true;
                        return NULL;
                    }
                    keys_ops[i] = ray_scan(gg, ray_str_ptr(s));
                    if (!keys_ops[i]) {
                        ray_graph_free(gg);
                        ray_release(accum);
                        prog->eval_err = true;
                        return NULL;
                    }
                }

                /* Agg input: value column (for COUNT we still pass a column; any
                 * column works since COUNT only counts rows).  Must be bounds-
                 * checked — silently clamping to 0 would compute a valid-looking
                 * but wrong result over an unrelated column. */
                int value_col = body->agg_value_col;
                if (body->agg_op != DL_AGG_COUNT &&
                    (value_col < 0 || value_col >= src_rel->arity)) {
                    ray_graph_free(gg);
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }
                if (value_col < 0 || value_col >= src_rel->arity) value_col = 0;
                ray_t* vs = ray_sym_str(src_rel->col_names[value_col]);
                if (!vs) {
                    ray_graph_free(gg);
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }
                ray_op_t* agg_in = ray_scan(gg, ray_str_ptr(vs));
                if (!agg_in) {
                    ray_graph_free(gg);
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }

                uint16_t op_code;
                switch (body->agg_op) {
                    case DL_AGG_COUNT: op_code = OP_COUNT; break;
                    case DL_AGG_SUM:   op_code = OP_SUM;   break;
                    case DL_AGG_MIN:   op_code = OP_MIN;   break;
                    case DL_AGG_MAX:   op_code = OP_MAX;   break;
                    case DL_AGG_AVG:   op_code = OP_AVG;   break;
                    default:
                        ray_graph_free(gg);
                        ray_release(accum); return NULL;
                }

                ray_op_t* ag_ins[1] = { agg_in };
                ray_op_t* root = ray_group(gg, keys_ops, (uint8_t)nk, &op_code, ag_ins, 1);
                if (!root) {
                    ray_graph_free(gg);
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }
                ray_t* group_tbl = ray_execute(gg, root);
                ray_graph_free(gg);

                if (!group_tbl) {
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }
                if (RAY_IS_ERR(group_tbl)) {
                    ray_error_free(group_tbl);
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }

                /* Replace accum with group_tbl (schema: key0..key{nk-1}, agg).
                 * This is valid because the DL_AGG case for aggregate-only rules
                 * created a singleton _unit accum that we can discard. Mixed
                 * rules (body atoms + grouped agg) are not supported here; they
                 * would require a join on shared vars and fall under A5/later. */
                ray_release(accum);
                accum = group_tbl;

                /* Bind key variables to the key columns in the group output */
                for (int i = 0; i < nk; i++) {
                    int kv = body->agg_group_key_vars[i];
                    var_bound[kv] = true;
                    var_col[kv] = i;
                }
                /* Bind target variable to the aggregate column (last column) */
                var_bound[body->agg_target_var] = true;
                var_col[body->agg_target_var] = nk;  /* agg column immediately follows keys */
                break;
            }
            /* -------- existing scalar path below unchanged -------- */
            int src_idx = dl_find_rel(prog, body->agg_pred);
            if (src_idx < 0) {
                ray_release(accum);
                return NULL;
            }
            dl_rel_t* src_rel_s = &prog->rels[src_idx];
            ray_t* src_table = src_rel_s->table;
            int64_t src_nrows = (src_table && !RAY_IS_ERR(src_table))
                ? ray_table_nrows(src_table)
                : 0;

            /* Bounds-check value column up front for every value-taking op
             * (SUM/MIN/MAX/AVG).  Must happen before the empty-source early
             * returns below, otherwise an out-of-range index on an empty
             * source would silently emit the SUM identity 0 / 0.0. */
            bool need_value_col = (body->agg_op == DL_AGG_SUM
                                   || body->agg_op == DL_AGG_MIN
                                   || body->agg_op == DL_AGG_MAX
                                   || body->agg_op == DL_AGG_AVG);
            if (need_value_col &&
                (body->agg_value_col < 0 ||
                 body->agg_value_col >= src_rel_s->arity)) {
                ray_release(accum);
                prog->eval_err = true;
                return NULL;
            }

            if (src_nrows == 0 && (body->agg_op == DL_AGG_MIN
                     || body->agg_op == DL_AGG_MAX
                     || body->agg_op == DL_AGG_AVG)) {
                /* Empty-source: MIN/MAX/AVG emit no row (matches rayforce core's domain
                 * error / typed-null semantics). COUNT and SUM keep their identities (0). */
                ray_release(accum);
                return NULL;
            }

            int64_t result_i = 0;
            double  result_f = 0.0;
            bool    is_avg   = (body->agg_op == DL_AGG_AVG);
            /* Float promotion: AVG always emits f64; SUM/MIN/MAX track their
             * source column type (i64 in -> i64 out; f64 in -> f64 out).
             * COUNT is always i64.  For empty SUM, we still need to inspect
             * the column type so the identity (0 / 0.0) is emitted in the
             * correct result type. */
            bool    is_float = is_avg;
            if (need_value_col) {
                ray_t* vc0 = ray_table_get_col_idx(src_table, body->agg_value_col);
                if (vc0) {
                    if (vc0->type == RAY_F64) {
                        is_float = true;
                    } else if (vc0->type != RAY_I64) {
                        /* Non-numeric source: reject regardless of row count. */
                        ray_release(accum);
                        prog->eval_err = true;
                        return NULL;
                    }
                }
            }
            switch (body->agg_op) {
            case DL_AGG_COUNT:
                result_i = src_nrows;
                break;
            case DL_AGG_SUM:
            case DL_AGG_MIN:
            case DL_AGG_MAX:
            case DL_AGG_AVG:
                if (src_nrows > 0) {
                    ray_t* val_col =
                        ray_table_get_col_idx(src_table, body->agg_value_col);
                    if (!val_col) {
                        ray_release(accum);
                        return NULL;
                    }
                    if (val_col->type == RAY_I64) {
                        int64_t* vd = (int64_t*)ray_data(val_col);
                        if (body->agg_op == DL_AGG_SUM) {
                            result_i = 0;
                            for (int64_t i = 0; i < src_nrows; i++)
                                result_i += vd[i];
                        } else if (body->agg_op == DL_AGG_MIN) {
                            result_i = vd[0];
                            for (int64_t i = 1; i < src_nrows; i++) {
                                if (vd[i] < result_i)
                                    result_i = vd[i];
                            }
                        } else if (body->agg_op == DL_AGG_MAX) {
                            result_i = vd[0];
                            for (int64_t i = 1; i < src_nrows; i++) {
                                if (vd[i] > result_i)
                                    result_i = vd[i];
                            }
                        } else { /* DL_AGG_AVG */
                            int64_t acc = 0;
                            for (int64_t i = 0; i < src_nrows; i++)
                                acc += vd[i];
                            result_f = (double)acc / (double)src_nrows;
                        }
                    } else if (val_col->type == RAY_F64) {
                        is_float = true;  /* SUM/MIN/MAX promote to f64 */
                        double* vd = (double*)ray_data(val_col);
                        if (body->agg_op == DL_AGG_SUM) {
                            result_f = 0.0;
                            for (int64_t i = 0; i < src_nrows; i++)
                                result_f += vd[i];
                        } else if (body->agg_op == DL_AGG_MIN) {
                            result_f = vd[0];
                            for (int64_t i = 1; i < src_nrows; i++) {
                                if (vd[i] < result_f)
                                    result_f = vd[i];
                            }
                        } else if (body->agg_op == DL_AGG_MAX) {
                            result_f = vd[0];
                            for (int64_t i = 1; i < src_nrows; i++) {
                                if (vd[i] > result_f)
                                    result_f = vd[i];
                            }
                        } else { /* DL_AGG_AVG */
                            double acc = 0.0;
                            for (int64_t i = 0; i < src_nrows; i++)
                                acc += vd[i];
                            result_f = acc / (double)src_nrows;
                        }
                    } else {
                        /* Non-numeric source column — reject loudly rather than
                         * silently returning zero. */
                        ray_release(accum);
                        prog->eval_err = true;
                        return NULL;
                    }
                }
                break;
            default:
                break;
            }

            int64_t nrows = ray_table_nrows(accum);
            if (nrows == 0)
                break;
            ray_t* new_col = ray_vec_new(is_float ? RAY_F64 : RAY_I64, nrows);
            /* Silent break would leave agg_target_var unbound and eval
             * would keep running with a partially-constructed rule —
             * surface the allocation failure so dl_eval returns -1. */
            if (!new_col) {
                ray_release(accum);
                prog->eval_err = true;
                return NULL;
            }
            if (RAY_IS_ERR(new_col)) {
                ray_error_free(new_col);
                ray_release(accum);
                prog->eval_err = true;
                return NULL;
            }
            new_col->len = nrows;
            if (is_float) {
                double* nd = (double*)ray_data(new_col);
                for (int64_t r = 0; r < nrows; r++) nd[r] = result_f;
            } else {
                int64_t* nd = (int64_t*)ray_data(new_col);
                for (int64_t r = 0; r < nrows; r++) nd[r] = result_i;
            }

            int new_col_idx = (int)ray_table_ncols(accum);
            char colname[32];
            snprintf(colname, sizeof(colname), "_g%d", body->agg_target_var);
            ray_t* new_accum = dl_table_add_computed_col(accum, new_col, colname);
            ray_release(new_col);
            ray_release(accum);
            accum = new_accum;

            var_bound[body->agg_target_var] = true;
            var_col[body->agg_target_var] = new_col_idx;
            break;
        }

        case DL_BUILTIN: {
            switch (body->builtin_id) {
            case DL_BUILTIN_BEFORE: {
                int s_col = var_col[body->vars[0]];
                int t_col = var_col[body->vars[2]];
                ray_t* filtered = dl_builtin_before(accum, s_col, t_col);
                ray_release(accum);
                accum = filtered;
                break;
            }
            case DL_BUILTIN_DURATION_SINCE: {
                int t1_col = var_col[body->vars[0]];
                int t2_col = var_col[body->vars[1]];
                int d_var = body->vars[2];
                int new_idx = (int)ray_table_ncols(accum);
                char colname[32];
                snprintf(colname, sizeof(colname), "_d%d", d_var);
                ray_t* result = dl_builtin_duration_since(accum, t1_col, t2_col, colname);
                ray_release(accum);
                accum = result;
                var_bound[d_var] = true;
                var_col[d_var] = new_idx;
                break;
            }
            case DL_BUILTIN_ABS: {
                int x_col = var_col[body->vars[0]];
                int y_var = body->vars[1];
                int new_idx = (int)ray_table_ncols(accum);
                char colname[32];
                snprintf(colname, sizeof(colname), "_abs%d", y_var);
                ray_t* result = dl_builtin_abs(accum, x_col, colname);
                ray_release(accum);
                accum = result;
                var_bound[y_var] = true;
                var_col[y_var] = new_idx;
                break;
            }
            }
            break;
        }

        case DL_CMP: {
            int64_t nrows = ray_table_nrows(accum);
            if (nrows == 0) break;

            ray_t* lhs_evaled = NULL;
            ray_t* rhs_evaled = NULL;
            ray_t* lhs_src = NULL;  /* borrowed reference for type inspection */
            ray_t* rhs_src = NULL;

            if (body->cmp_lhs_expr) {
                lhs_evaled = dl_eval_expr(body->cmp_lhs_expr, accum, var_col, nrows);
                /* LHS evaluation failure can't be silently skipped — a
                 * missing filter changes the query's answer. */
                if (!lhs_evaled) {
                    prog->eval_err = true;
                    ray_release(accum);
                    return NULL;
                }
                if (RAY_IS_ERR(lhs_evaled)) {
                    ray_error_free(lhs_evaled);
                    prog->eval_err = true;
                    ray_release(accum);
                    return NULL;
                }
                lhs_src = lhs_evaled;
            } else {
                int lhs_col = var_col[body->cmp_lhs];
                lhs_src = ray_table_get_col_idx(accum, lhs_col);
                if (!lhs_src) {
                    prog->eval_err = true;
                    ray_release(accum);
                    return NULL;
                }
            }

            if (body->cmp_rhs_expr) {
                rhs_evaled = dl_eval_expr(body->cmp_rhs_expr, accum, var_col, nrows);
                if (!rhs_evaled) {
                    if (lhs_evaled) ray_release(lhs_evaled);
                    prog->eval_err = true;
                    ray_release(accum);
                    return NULL;
                }
                if (RAY_IS_ERR(rhs_evaled)) {
                    ray_error_free(rhs_evaled);
                    if (lhs_evaled) ray_release(lhs_evaled);
                    prog->eval_err = true;
                    ray_release(accum);
                    return NULL;
                }
                rhs_src = rhs_evaled;
            } else if (body->cmp_rhs != DL_CONST) {
                int rhs_col = var_col[body->cmp_rhs];
                rhs_src = ray_table_get_col_idx(accum, rhs_col);
                if (!rhs_src) {
                    if (lhs_evaled) ray_release(lhs_evaled);
                    prog->eval_err = true;
                    ray_release(accum);
                    return NULL;
                }
            }
            /* else rhs is a constant i64 body->cmp_const */

            /* Reject non-numeric sources — DL_CMP has no meaningful
             * comparison for SYM/STR columns without an ordering hook. */
            bool lhs_is_f64 = lhs_src && lhs_src->type == RAY_F64;
            bool rhs_is_f64 = rhs_src && rhs_src->type == RAY_F64;
            if (lhs_src && lhs_src->type != RAY_I64 && lhs_src->type != RAY_F64) {
                if (lhs_evaled) ray_release(lhs_evaled);
                if (rhs_evaled) ray_release(rhs_evaled);
                prog->eval_err = true;
                ray_release(accum);
                return NULL;
            }
            if (rhs_src && rhs_src->type != RAY_I64 && rhs_src->type != RAY_F64) {
                if (lhs_evaled) ray_release(lhs_evaled);
                if (rhs_evaled) ray_release(rhs_evaled);
                prog->eval_err = true;
                ray_release(accum);
                return NULL;
            }

            /* Promote to f64 iff either side is f64.  Otherwise stay in
             * i64 arithmetic for speed and exact integer semantics. */
            bool use_f64 = lhs_is_f64 || rhs_is_f64;
            const int64_t* lhs_i = !use_f64 ? (const int64_t*)ray_data(lhs_src) : NULL;
            const int64_t* rhs_i = !use_f64 && rhs_src ? (const int64_t*)ray_data(rhs_src) : NULL;
            const double*  lhs_f = use_f64 && !lhs_is_f64 ? NULL
                                 : (use_f64 ? (const double*)ray_data(lhs_src) : NULL);
            const double*  rhs_f = use_f64 && rhs_src && rhs_is_f64
                                 ? (const double*)ray_data(rhs_src) : NULL;

            ray_t* mask_block = ray_alloc((size_t)nrows * sizeof(bool));
            if (!mask_block) {
                if (lhs_evaled) ray_release(lhs_evaled);
                if (rhs_evaled) ray_release(rhs_evaled);
                break;
            }
            bool* mask = (bool*)ray_data(mask_block);
            int64_t count = 0;
            for (int64_t r = 0; r < nrows; r++) {
                bool pass = false;
                if (use_f64) {
                    /* Widen the non-f64 side — mixed arithmetic is already
                     * supported by dl_eval_expr, and DL_CMP_const is i64. */
                    double lv = lhs_is_f64 ? lhs_f[r] : (double)((const int64_t*)ray_data(lhs_src))[r];
                    double rv;
                    if (rhs_src)
                        rv = rhs_is_f64 ? rhs_f[r] : (double)((const int64_t*)ray_data(rhs_src))[r];
                    else
                        rv = (double)body->cmp_const;
                    switch (body->cmp_op) {
                    case DL_CMP_EQ: pass = (lv == rv); break;
                    case DL_CMP_NE: pass = (lv != rv); break;
                    case DL_CMP_LT: pass = (lv <  rv); break;
                    case DL_CMP_LE: pass = (lv <= rv); break;
                    case DL_CMP_GT: pass = (lv >  rv); break;
                    case DL_CMP_GE: pass = (lv >= rv); break;
                    }
                } else {
                    int64_t lv = lhs_i[r];
                    int64_t rv = rhs_i ? rhs_i[r] : body->cmp_const;
                    switch (body->cmp_op) {
                    case DL_CMP_EQ: pass = (lv == rv); break;
                    case DL_CMP_NE: pass = (lv != rv); break;
                    case DL_CMP_LT: pass = (lv <  rv); break;
                    case DL_CMP_LE: pass = (lv <= rv); break;
                    case DL_CMP_GT: pass = (lv >  rv); break;
                    case DL_CMP_GE: pass = (lv >= rv); break;
                    }
                }
                (void)lhs_f;  /* silence unused warnings in non-f64 paths */
                mask[r] = pass;
                if (pass) count++;
            }

            if (lhs_evaled) ray_release(lhs_evaled);
            if (rhs_evaled) ray_release(rhs_evaled);

            if (count == nrows) {
                ray_free(mask_block);
                break;  /* all rows pass */
            }

            /* Build filtered table — element-size-aware memcpy so f64
             * columns and narrow-SYM columns survive the mask unchanged.
             * Silently `continue`-ing past missing columns would yield
             * a table with fewer columns than accum, breaking schema
             * invariants in downstream table_union.  Treat every such
             * failure as unrecoverable. */
            int64_t ncols = ray_table_ncols(accum);
            ray_t* out = ray_table_new((int)ncols);
            if (!out || RAY_IS_ERR(out)) {
                if (out && RAY_IS_ERR(out)) ray_error_free(out);
                ray_free(mask_block);
                ray_release(accum);
                prog->eval_err = true;
                return NULL;
            }
            for (int64_t c = 0; c < ncols; c++) {
                ray_t* src = ray_table_get_col_idx(accum, c);
                if (!src) {
                    ray_release(out); ray_free(mask_block);
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }
                ray_t* dst = (src->type == RAY_SYM)
                    ? ray_sym_vec_new(src->attrs & RAY_SYM_W_MASK, count)
                    : ray_vec_new(src->type, count);
                if (!dst) {
                    ray_release(out); ray_free(mask_block);
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }
                if (RAY_IS_ERR(dst)) {
                    ray_error_free(dst);
                    ray_release(out); ray_free(mask_block);
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }
                dst->len = count;
                uint8_t esz = ray_sym_elem_size(src->type, src->attrs);
                const uint8_t* sb = (const uint8_t*)ray_data(src);
                uint8_t* db = (uint8_t*)ray_data(dst);
                int64_t j = 0;
                for (int64_t r = 0; r < nrows; r++)
                    if (mask[r]) {
                        memcpy(db + (size_t)j * esz, sb + (size_t)r * esz, esz);
                        j++;
                    }
                if (src->type == RAY_STR) col_propagate_str_pool(dst, src);
                ray_t* next = ray_table_add_col(out, ray_table_col_name(accum, c), dst);
                ray_release(dst);
                if (!next) {
                    ray_release(out); ray_free(mask_block);
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }
                if (RAY_IS_ERR(next)) {
                    ray_error_free(next);
                    ray_release(out); ray_free(mask_block);
                    ray_release(accum);
                    prog->eval_err = true;
                    return NULL;
                }
                out = next;
            }
            ray_free(mask_block);
            ray_release(accum);
            accum = out;
            break;
        }

        case DL_INTERVAL: {
            int fact_col = var_col[body->interval_fact_var];
            int start_col = fact_col;
            int end_col = fact_col + 1;

            var_bound[body->interval_start_var] = true;
            var_col[body->interval_start_var] = start_col;

            var_bound[body->interval_end_var] = true;
            var_col[body->interval_end_var] = end_col;
            break;
        }
        } /* switch */
    }

    /* Project to head variables */
    int head_idx = dl_find_rel(prog, rule->head_pred);
    if (head_idx < 0) { ray_release(accum); return NULL; }
    dl_rel_t* head_rel = &prog->rels[head_idx];

    int proj_cols[DL_MAX_ARITY];
    for (int c = 0; c < rule->head_arity; c++) {
        int v = rule->head_vars[c];
        if (v == DL_CONST) {
            proj_cols[c] = -1;
        } else {
            proj_cols[c] = var_col[v];
        }
    }

    ray_t* projected = dl_project(accum, proj_cols, rule->head_arity, head_rel,
                                   rule->head_consts, rule->head_const_types);
    ray_release(accum);

    /* dl_project now surfaces hard failures (alloc OOM, type errors, add-col
     * errors) as RAY_ERROR objects.  Catch those here and flag the program
     * so dl_eval can return -1 instead of silently dropping the rule's
     * output via the const_table/execute chain. */
    if (!projected) return NULL;
    if (RAY_IS_ERR(projected)) {
        ray_error_free(projected);
        prog->eval_err = true;
        return NULL;
    }

    /* Store result in the graph as a const_table so the caller can execute */
    ray_op_t* result_node = ray_const_table(g, projected);
    ray_release(projected);
    return result_node;
}

/* ========================================================================
 * Table utilities for fixpoint evaluation
 * ======================================================================== */

/* Rename table columns to match the head relation's expected names.
 * This is needed because ray_select output column names come from the scan
 * nodes (e.g., "edge__c0"), but we need them to match the head relation
 * (e.g., "path__c0"). Returns a new owned table. */
static ray_t* table_rename_cols(ray_t* tbl, dl_rel_t* target_rel) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t ncols = ray_table_ncols(tbl);
    if (ncols <= 0) { ray_retain(tbl); return tbl; }

    int arity = target_rel->arity;
    if (ncols != arity) { ray_retain(tbl); return tbl; }

    /* Check if renaming is needed */
    bool needs_rename = false;
    for (int c = 0; c < arity; c++) {
        if (ray_table_col_name(tbl, c) != target_rel->col_names[c]) {
            needs_rename = true;
            break;
        }
    }
    if (!needs_rename) { ray_retain(tbl); return tbl; }

    /* Build new table with correct column names sharing the same column data */
    ray_t* out = ray_table_new(arity);
    for (int c = 0; c < arity; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col)
            out = ray_table_add_col(out, target_rel->col_names[c], col);
    }
    return out;
}

/* Canonicalize column names to "c0","c1",... Returns new owned table. */
static ray_t* canonicalize(ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nc = ray_table_ncols(tbl);
    ray_t* out = ray_table_new(nc);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (!col) continue;
        char buf[16];
        snprintf(buf, sizeof(buf), "c%d", (int)c);
        int64_t sym = ray_sym_intern(buf, strlen(buf));
        out = ray_table_add_col(out, sym, col);
    }
    return out;
}

/* Restore original column names from `src` onto `tbl`. */
static ray_t* restore_names(ray_t* tbl, ray_t* src) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nc = ray_table_ncols(tbl);
    ray_t* out = ray_table_new(nc);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col)
            out = ray_table_add_col(out, ray_table_col_name(src, c), col);
    }
    ray_release(tbl);
    return out;
}

/* Create a table by concatenating all rows from tables a and b (same schema).
 * Uses column-wise ray_vec_concat. Returns new owned table with a's names. */
static ray_t* table_union(ray_t* a, ray_t* b) {
    /* Pass-through paths always return a retained non-NULL result so
     * callers can release uniformly.  A NULL operand falls back to the
     * other side; a RAY_ERROR operand is *propagated* (retained) rather
     * than masked by the non-error side — otherwise a real failure on
     * `b` would silently surface as `a` and the caller would never see
     * the error.  ray_retain is a no-op on errors so the retain call is
     * safe and keeps the contract "release is always valid". */
    if (!a) {
        if (b) ray_retain(b);
        return b;
    }
    if (RAY_IS_ERR(a)) {
        ray_retain(a);  /* no-op for errors; documents "owned return" */
        return a;
    }
    if (!b) { ray_retain(a); return a; }
    if (RAY_IS_ERR(b)) {
        ray_retain(b);
        return b;
    }

    /* Column-count check must run before the empty-rows short-circuit.
     * Otherwise one side having 0 rows but a stripped schema (e.g. an
     * antijoin result that collapsed to (0 rows, 0 cols)) would silently
     * return the other side's schema and the caller would store a table
     * whose arity differs from what it expected. */
    int64_t ncols_a = ray_table_ncols(a);
    int64_t ncols_b = ray_table_ncols(b);
    if (ncols_a != ncols_b)
        return ray_error("schema", "table_union: column count mismatch");
    int64_t ncols = ncols_a;

    if (ray_table_nrows(a) == 0) { ray_retain(b); return b; }
    if (ray_table_nrows(b) == 0) { ray_retain(a); return a; }

    ray_t* out = ray_table_new((int)ncols);
    if (!out || RAY_IS_ERR(out))
        return out ? out : ray_error("memory", "table_union: table_new");
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col_a = ray_table_get_col_idx(a, c);
        ray_t* col_b = ray_table_get_col_idx(b, c);
        if (!col_a || !col_b) {
            /* Silently dropping a column would produce a schema-incomplete
             * result that the caller mistakes for a successful union. */
            ray_release(out);
            return ray_error("domain", "table_union: missing column");
        }
        ray_t* merged = ray_vec_concat(col_a, col_b);
        if (!merged) {
            ray_release(out);
            return ray_error("memory", "table_union: concat");
        }
        if (RAY_IS_ERR(merged)) {
            /* Propagate the original error (e.g. "type" for schema
             * mismatch) so callers see the real diagnostic instead of
             * a generic "memory". */
            ray_release(out);
            return merged;
        }
        ray_t* next = ray_table_add_col(out, ray_table_col_name(a, c), merged);
        ray_release(merged);
        if (!next) {
            ray_release(out);
            return ray_error("memory", "table_union: add_col");
        }
        if (RAY_IS_ERR(next)) {
            ray_release(out);
            return next;
        }
        out = next;
    }
    return out;
}

/* Deduplicate table rows on all columns. Returns new owned table. */
static ray_t* table_distinct(ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t nrows = ray_table_nrows(tbl);
    if (nrows <= 1) { ray_retain(tbl); return tbl; }

    int64_t ncols = ray_table_ncols(tbl);
    if (ncols <= 0) { ray_retain(tbl); return tbl; }

    ray_t* canonical = canonicalize(tbl);
    if (!canonical || RAY_IS_ERR(canonical))
        return canonical ? canonical : ray_error("memory", "table_distinct: canonicalize");

    ray_graph_t* g = ray_graph_new(canonical);
    if (!g) {
        ray_release(canonical);
        return ray_error("memory", "table_distinct: graph_new");
    }

    ray_op_t* keys[DL_MAX_ARITY];
    for (int64_t c = 0; c < ncols && c < DL_MAX_ARITY; c++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "c%d", (int)c);
        keys[c] = ray_scan(g, buf);
    }

    ray_op_t* dist = ray_distinct(g, keys, (uint8_t)ncols);
    dist = ray_optimize(g, dist);
    ray_t* deduped = ray_execute(g, dist);
    ray_graph_free(g);
    ray_release(canonical);

    return restore_names(deduped, tbl);
}

/* Anti-join: rows in `left` that don't appear in `right` (same schema).
 * Returns new owned table with left's original column names. */
static ray_t* table_antijoin(ray_t* left, ray_t* right) {
    if (!left || RAY_IS_ERR(left)) return left;
    if (!right || RAY_IS_ERR(right) || ray_table_nrows(right) == 0) {
        ray_retain(left);
        return left;
    }
    if (ray_table_nrows(left) == 0) {
        ray_retain(left);
        return left;
    }

    int64_t ncols = ray_table_ncols(left);
    if (ncols <= 0) { ray_retain(left); return left; }

    ray_t* cl = canonicalize(left);
    if (!cl || RAY_IS_ERR(cl))
        return cl ? cl : ray_error("memory", "table_antijoin: canonicalize left");
    ray_t* cr = canonicalize(right);
    if (!cr || RAY_IS_ERR(cr)) {
        ray_release(cl);
        return cr ? cr : ray_error("memory", "table_antijoin: canonicalize right");
    }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) {
        ray_release(cl);
        ray_release(cr);
        return ray_error("memory", "table_antijoin: graph_new");
    }

    ray_op_t* l = ray_const_table(g, cl);
    ray_op_t* r = ray_const_table(g, cr);

    uint16_t l_tid = ray_graph_add_table(g, cl);
    uint16_t r_tid = ray_graph_add_table(g, cr);

    ray_op_t* lkeys[DL_MAX_ARITY];
    ray_op_t* rkeys[DL_MAX_ARITY];
    for (int64_t c = 0; c < ncols && c < DL_MAX_ARITY; c++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "c%d", (int)c);
        lkeys[c] = ray_scan_table(g, l_tid, buf);
        rkeys[c] = ray_scan_table(g, r_tid, buf);
    }

    ray_op_t* aj = ray_antijoin(g, l, lkeys, r, rkeys, (uint8_t)ncols);
    ray_t* raw = ray_execute(g, aj);
    ray_graph_free(g);
    ray_release(cl);
    ray_release(cr);

    return restore_names(raw, left);
}

/* Normalize column names of a table to match the target relation's naming scheme.
 * Returns a new owned table with correct names (shares column data).
 * Currently unused but retained for future use by external callers. */
static ray_t* normalize_columns(ray_t* tbl, dl_rel_t* rel)
    __attribute__((unused));
static ray_t* normalize_columns(ray_t* tbl, dl_rel_t* rel) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    int64_t ncols = ray_table_ncols(tbl);
    if (ncols != rel->arity) {
        /* Arity mismatch — can't normalize */
        ray_retain(tbl);
        return tbl;
    }
    /* Check if already correct */
    bool ok = true;
    for (int c = 0; c < rel->arity; c++) {
        if (ray_table_col_name(tbl, c) != rel->col_names[c]) { ok = false; break; }
    }
    if (ok) { ray_retain(tbl); return tbl; }

    /* Rebuild with correct names, sharing column data */
    ray_t* out = ray_table_new(rel->arity);
    for (int c = 0; c < rel->arity; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col)
            out = ray_table_add_col(out, rel->col_names[c], col);
    }
    return out;
}

/* ========================================================================
 * Provenance helpers
 * ======================================================================== */

/* Hash all columns of ref at row r into a single key. */
static uint64_t dl_row_hash(int64_t** col_data, int64_t ncols, int64_t r) {
    uint64_t h = ray_hash_i64(col_data[0][r]);
    for (int64_t c = 1; c < ncols; c++)
        h = ray_hash_combine(h, ray_hash_i64(col_data[c][r]));
    return h;
}

/* Check if rows match across `ncols` columns. */
static bool dl_row_eq(int64_t** a_cols, int64_t ar,
                     int64_t** b_cols, int64_t br, int64_t ncols) {
    for (int64_t c = 0; c < ncols; c++)
        if (a_cols[c][ar] != b_cols[c][br]) return false;
    return true;
}

/* Open-addressing hash set keyed by ref-row tuple.  Slot stores ref row
 * index; lookup hashes the probe-row tuple and walks the probe chain.
 * Replaces the per-call O(ref_rows) linear scan in dl_row_in_table —
 * the previous shape was O(tbl_rows × ref_rows × ncols) which is
 * quadratic for typical datalog provenance workloads. */
typedef struct {
    int64_t* slots;        /* row index, -1 = empty */
    ray_t*   block;
    int64_t  cap;
    int64_t  mask;
    int64_t** ref_cols;    /* cached column data ptrs for ref */
    int64_t  ncols;
} dl_rowset_t;

static bool dl_rowset_init(dl_rowset_t* rs, ray_t* ref) {
    int64_t ncols = ray_table_ncols(ref);
    int64_t nrows = ray_table_nrows(ref);
    rs->ncols = ncols;
    rs->ref_cols = (int64_t**)ray_sys_alloc(sizeof(int64_t*) * (size_t)ncols);
    if (!rs->ref_cols) return false;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(ref, c);
        rs->ref_cols[c] = col ? (int64_t*)ray_data(col) : NULL;
    }
    int64_t cap = 16;
    while (cap < (nrows > 0 ? nrows * 2 : 16)) cap *= 2;
    rs->block = ray_alloc((size_t)cap * sizeof(int64_t));
    if (!rs->block || RAY_IS_ERR(rs->block)) {
        ray_sys_free(rs->ref_cols);
        rs->ref_cols = NULL;
        return false;
    }
    rs->slots = (int64_t*)ray_data(rs->block);
    rs->cap = cap;
    rs->mask = cap - 1;
    for (int64_t i = 0; i < cap; i++) rs->slots[i] = -1;

    for (int64_t r = 0; r < nrows; r++) {
        uint64_t h = dl_row_hash(rs->ref_cols, ncols, r);
        int64_t s = (int64_t)(h & (uint64_t)rs->mask);
        while (rs->slots[s] != -1) s = (s + 1) & rs->mask;
        rs->slots[s] = r;
    }
    return true;
}

static void dl_rowset_destroy(dl_rowset_t* rs) {
    if (rs->block) { ray_release(rs->block); rs->block = NULL; }
    if (rs->ref_cols) { ray_sys_free(rs->ref_cols); rs->ref_cols = NULL; }
}

/* True if the row at `tbl_cols[..][row]` is present in the set. */
static bool dl_rowset_contains(dl_rowset_t* rs, int64_t** tbl_cols, int64_t row) {
    uint64_t h = dl_row_hash(tbl_cols, rs->ncols, row);
    int64_t s = (int64_t)(h & (uint64_t)rs->mask);
    while (rs->slots[s] != -1) {
        int64_t r = rs->slots[s];
        if (dl_row_eq(tbl_cols, row, rs->ref_cols, r, rs->ncols))
            return true;
        s = (s + 1) & rs->mask;
    }
    return false;
}

/* Build source provenance for one IDB relation in CSR format.
 *
 * For each derived row, extracts head variable bindings from the firing rule
 * and scans each positive body atom's relation for rows consistent with those
 * bindings.  Results are stored as two parallel vectors on the relation:
 *
 *   prov_src_offsets — I64[nrows+1]: offsets[i] = start index in prov_src_data
 *                      for derived row i.  offsets[nrows] = total entry count.
 *   prov_src_data    — I64[total]: each entry = (rel_idx << 32) | row_idx,
 *                      packed reference to the contributing source row.
 *                      Row indices are truncated to 32 bits (max ~4 billion rows
 *                      per relation).
 *
 * Body-only variables (not appearing in the head) are unconstrained during
 * source lookup, so the entry set may be a superset of the true proof. */
static void dl_build_source_prov(dl_program_t* prog, dl_rel_t* rel,
                                  int64_t nrows, int64_t* pd) {
    ray_t* off_vec = ray_vec_new(RAY_I64, nrows + 1);
    if (!off_vec || RAY_IS_ERR(off_vec)) return;
    off_vec->len = nrows + 1;
    int64_t* off = (int64_t*)ray_data(off_vec);

    int64_t buf_cap = (nrows < 16) ? 64 : nrows * 4;
    ray_t* buf_block = ray_alloc((size_t)buf_cap * sizeof(int64_t));
    if (!buf_block) { ray_release(off_vec); return; }
    int64_t* buf = (int64_t*)ray_data(buf_block);
    int64_t buf_len = 0;

    for (int64_t row = 0; row < nrows; row++) {
        off[row] = buf_len;
        if (pd[row] < 0) continue;

        dl_rule_t* rule = &prog->rules[pd[row]];

        int64_t var_vals[DL_MAX_ARITY * DL_MAX_BODY];
        bool    var_set [DL_MAX_ARITY * DL_MAX_BODY];
        memset(var_set, 0, sizeof(var_set));

        /* Extract head variable bindings from this derived row */
        for (int h = 0; h < rule->head_arity; h++) {
            int v = rule->head_vars[h];
            if (v == DL_CONST) continue;
            ray_t* col = ray_table_get_col_idx(rel->table, h);
            if (!col) continue;
            var_vals[v] = ((int64_t*)ray_data(col))[row];
            var_set[v]  = true;
        }

        /* For each positive body atom, find matching source rows */
        for (int b = 0; b < rule->n_body; b++) {
            dl_body_t* body = &rule->body[b];
            if (body->type != DL_POS) continue;

            int bri = dl_find_rel(prog, body->pred);
            if (bri < 0) continue;
            dl_rel_t* brel   = &prog->rels[bri];
            int64_t   bnrows = ray_table_nrows(brel->table);

            for (int64_t br = 0; br < bnrows; br++) {
                bool match = true;
                for (int c = 0; c < body->arity; c++) {
                    ray_t* bcol = ray_table_get_col_idx(brel->table, c);
                    if (!bcol) { match = false; break; }
                    int64_t cell = ((int64_t*)ray_data(bcol))[br];
                    int     v    = body->vars[c];
                    if (v == DL_CONST) {
                        if (cell != body->const_vals[c]) { match = false; break; }
                    } else if (var_set[v]) {
                        if (cell != var_vals[v])         { match = false; break; }
                    }
                    /* body-only variable: unconstrained, always matches */
                }
                if (!match) continue;

                if (buf_len >= buf_cap) {
                    int64_t   new_cap   = buf_cap * 2;
                    ray_t*    new_block = ray_alloc((size_t)new_cap * sizeof(int64_t));
                    if (!new_block) goto oom;
                    memcpy(ray_data(new_block), buf, (size_t)buf_len * sizeof(int64_t));
                    ray_free(buf_block);
                    buf_block = new_block;
                    buf       = (int64_t*)ray_data(new_block);
                    buf_cap   = new_cap;
                }
                buf[buf_len++] = ((int64_t)bri << 32) | (int64_t)(uint32_t)br;
            }
        }
    }

    /* Success path: finalize CSR */
    off[nrows] = buf_len;
    {
        ray_t* data_vec = ray_vec_new(RAY_I64, buf_len > 0 ? buf_len : 1);
        if (!data_vec || RAY_IS_ERR(data_vec)) goto oom;
        data_vec->len = buf_len;
        if (buf_len > 0)
            memcpy(ray_data(data_vec), buf, (size_t)buf_len * sizeof(int64_t));
        ray_free(buf_block);

        if (rel->prov_src_offsets) ray_release(rel->prov_src_offsets);
        if (rel->prov_src_data)    ray_release(rel->prov_src_data);
        rel->prov_src_offsets = off_vec;
        rel->prov_src_data    = data_vec;
        return;
    }

oom:
    /* Allocation failed — discard partial results, leave both fields NULL */
    ray_free(buf_block);
    ray_release(off_vec);
    if (rel->prov_src_offsets) { ray_release(rel->prov_src_offsets); rel->prov_src_offsets = NULL; }
    if (rel->prov_src_data)    { ray_release(rel->prov_src_data);    rel->prov_src_data    = NULL; }
}

/* Build provenance for all IDB relations.
 * For each rule, compile with final tables and mark matching tuples.
 * Then build deep source provenance (CSR offsets + packed source refs). */
static void dl_build_provenance(dl_program_t* prog) {
    for (int ri = 0; ri < prog->n_rels; ri++) {
        dl_rel_t* rel = &prog->rels[ri];
        if (!rel->is_idb) continue;

        int64_t nrows = ray_table_nrows(rel->table);
        if (nrows == 0) continue;

        /* Allocate provenance column initialized to -1 (unknown) */
        ray_t* prov = ray_vec_new(RAY_I64, nrows);
        if (!prov || RAY_IS_ERR(prov)) continue;
        prov->len = nrows;
        int64_t* pd = (int64_t*)ray_data(prov);
        for (int64_t r = 0; r < nrows; r++)
            pd[r] = -1;

        /* For each rule with this head predicate */
        for (int r = 0; r < prog->n_rules; r++) {
            dl_rule_t* rule = &prog->rules[r];
            if (strcmp(rule->head_pred, rel->name) != 0) continue;

            /* Compile and execute the rule to get its derivable tuples */
            ray_graph_t* g = ray_graph_new(NULL);
            if (!g) continue;

            ray_op_t* output = dl_compile_rule(prog, rule, -1, r, g);
            if (!output) { ray_graph_free(g); continue; }

            ray_t* raw = ray_execute(g, output);
            ray_graph_free(g);
            if (!raw || RAY_IS_ERR(raw)) continue;

            ray_t* derived = table_rename_cols(raw, rel);
            ray_release(raw);
            if (!derived || RAY_IS_ERR(derived)) continue;

            /* Mark rows in rel->table that appear in derived.  Build a
             * hashset over `derived` once and probe per row of rel —
             * was O(nrows × derived_rows × ncols), now O(nrows + derived_rows). */
            dl_rowset_t rs;
            if (dl_rowset_init(&rs, derived)) {
                int64_t ncols_t = ray_table_ncols(rel->table);
                int64_t** tbl_cols = (int64_t**)ray_sys_alloc(sizeof(int64_t*) * (size_t)ncols_t);
                if (tbl_cols) {
                    for (int64_t c = 0; c < ncols_t; c++) {
                        ray_t* col = ray_table_get_col_idx(rel->table, c);
                        tbl_cols[c] = col ? (int64_t*)ray_data(col) : NULL;
                    }
                    for (int64_t row = 0; row < nrows; row++) {
                        if (pd[row] >= 0) continue;
                        if (dl_rowset_contains(&rs, tbl_cols, row))
                            pd[row] = r;
                    }
                    ray_sys_free(tbl_cols);
                }
                dl_rowset_destroy(&rs);
            }
            ray_release(derived);
        }

        if (rel->prov_col) ray_release(rel->prov_col);
        rel->prov_col = prov;

        dl_build_source_prov(prog, rel, nrows, pd);
    }
}

/* ========================================================================
 * Semi-naive fixpoint evaluation
 * ======================================================================== */

int dl_eval(dl_program_t* prog) {
    if (!prog) return -1;

    /* eval_err is sticky: it may have been raised at rule-add time (e.g.
     * by a head-const type conflict in dl_idb_align_head_const_types) —
     * resetting here would silently discard that signal.  Additional
     * failures during stratify/compile/exec below keep setting the flag,
     * and the final return honors it either way. */
    if (prog->eval_err) {
        /* Short-circuit: compile-time errors already stand; don't run
         * a potentially broken fixpoint. */
        return -1;
    }

    /* Stratify if not already done */
    if (prog->n_strata == 0) {
        if (dl_stratify(prog) != 0) return -1;
    }

    /* Process each stratum */
    for (int s = 0; s < prog->n_strata; s++) {
        /* Collect rules in this stratum */
        dl_rule_t* stratum_rules[DL_MAX_RULES];
        int stratum_rule_idx[DL_MAX_RULES];  /* original index in prog->rules */
        int n_stratum_rules = 0;

        for (int r = 0; r < prog->n_rules; r++) {
            if (prog->rules[r].stratum == s) {
                stratum_rule_idx[n_stratum_rules] = r;
                stratum_rules[n_stratum_rules++] = &prog->rules[r];
            }
        }
        if (n_stratum_rules == 0) continue;

        /* Phase A: Initial evaluation — evaluate each rule with full relations */
        /* Group rules by head predicate */
        for (int ri = 0; ri < n_stratum_rules; ri++) {
            dl_rule_t* rule = stratum_rules[ri];
            int head_idx = dl_find_rel(prog, rule->head_pred);
            if (head_idx < 0) continue;
            dl_rel_t* head_rel = &prog->rels[head_idx];

            ray_graph_t* g = ray_graph_new(NULL);
            if (!g) { prog->eval_err = true; continue; }

            ray_op_t* output = dl_compile_rule(prog, rule, -1, stratum_rule_idx[ri], g);
            if (!output) {
                /* dl_compile_rule marks eval_err on genuine failures; a bare
                 * NULL means "rule has no rows this pass" — not a fault. */
                ray_graph_free(g);
                continue;
            }

            ray_t* raw_tuples = ray_execute(g, output);
            ray_graph_free(g);

            if (!raw_tuples) continue;
            if (RAY_IS_ERR(raw_tuples)) { prog->eval_err = true; ray_error_free(raw_tuples); continue; }

            /* Rename columns to match head relation's expected names */
            ray_t* new_tuples = table_rename_cols(raw_tuples, head_rel);
            ray_release(raw_tuples);
            if (!new_tuples) continue;
            if (RAY_IS_ERR(new_tuples)) { prog->eval_err = true; ray_error_free(new_tuples); continue; }

            /* Merge into the head relation's table */
            ray_t* merged = table_union(head_rel->table, new_tuples);
            ray_release(new_tuples);
            if (!merged) { prog->eval_err = true; continue; }
            if (RAY_IS_ERR(merged)) { prog->eval_err = true; ray_error_free(merged); continue; }
            ray_t* deduped = table_distinct(merged);
            ray_release(merged);
            if (!deduped) { prog->eval_err = true; continue; }
            if (RAY_IS_ERR(deduped)) { prog->eval_err = true; ray_error_free(deduped); continue; }
            ray_release(head_rel->table);
            head_rel->table = deduped;
        }

        /* Phase B: Semi-naive loop — iterate with delta relations */
        /* For each IDB predicate in this stratum, compute delta as the
         * difference between current and previous table states. */
        ray_t* prev_tables[DL_MAX_RELS];
        ray_t* delta_tables[DL_MAX_RELS];
        memset(prev_tables, 0, sizeof(prev_tables));
        memset(delta_tables, 0, sizeof(delta_tables));

        /* Initially, delta = full table (all tuples are new) */
        for (int p = 0; p < prog->strata_sizes[s]; p++) {
            int rel_idx = prog->strata[s][p];
            dl_rel_t* rel = &prog->rels[rel_idx];
            if (rel->is_idb) {
                ray_retain(rel->table);
                delta_tables[rel_idx] = rel->table;
                /* prev = empty table with same schema as the relation.
                 * Column types must match rel->table so later ray_vec_concat
                 * calls don't reject the merge when the relation has
                 * non-i64 columns (e.g. RAY_SYM from head-constant slots). */
                prev_tables[rel_idx] = ray_table_new(rel->arity);
                for (int c = 0; c < rel->arity && c < DL_MAX_ARITY; c++) {
                    ray_t* src = ray_table_get_col_idx(rel->table, c);
                    int8_t ctype = src ? src->type : RAY_I64;
                    ray_t* empty_col = ray_vec_new(ctype, 0);
                    if (empty_col && !RAY_IS_ERR(empty_col)) {
                        prev_tables[rel_idx] = ray_table_add_col(
                            prev_tables[rel_idx], rel->col_names[c], empty_col);
                        ray_release(empty_col);
                    }
                }
            }
        }

        /* Semi-naive iteration */
        int max_iter = 1000;
        for (int iter = 0; iter < max_iter; iter++) {
            /* Check convergence: all deltas empty */
            bool any_new = false;
            for (int p = 0; p < prog->strata_sizes[s]; p++) {
                int rel_idx = prog->strata[s][p];
                if (delta_tables[rel_idx] &&
                    !RAY_IS_ERR(delta_tables[rel_idx]) &&
                    ray_table_nrows(delta_tables[rel_idx]) > 0) {
                    any_new = true;
                    break;
                }
            }
            if (!any_new) break;

            /* For each rule, for each positive body position that uses a
             * delta relation, compile and execute */
            ray_t* new_tuples_per_rel[DL_MAX_RELS];
            memset(new_tuples_per_rel, 0, sizeof(new_tuples_per_rel));

            for (int ri = 0; ri < n_stratum_rules; ri++) {
                dl_rule_t* rule = stratum_rules[ri];
                int head_idx = dl_find_rel(prog, rule->head_pred);
                if (head_idx < 0) continue;

                for (int b = 0; b < rule->n_body; b++) {
                    dl_body_t* body = &rule->body[b];
                    if (body->type != DL_POS) continue;

                    int body_rel = dl_find_rel(prog, body->pred);
                    if (body_rel < 0) continue;
                    if (!prog->rels[body_rel].is_idb) continue;
                    if (!delta_tables[body_rel] ||
                        ray_table_nrows(delta_tables[body_rel]) == 0) continue;

                    /* Swap in delta relation for this body position */
                    ray_t* saved = prog->rels[body_rel].table;
                    prog->rels[body_rel].table = delta_tables[body_rel];

                    ray_graph_t* g = ray_graph_new(NULL);
                    if (!g) {
                        prog->rels[body_rel].table = saved;
                        prog->eval_err = true;
                        continue;
                    }

                    ray_op_t* output = dl_compile_rule(prog, rule, b, stratum_rule_idx[ri], g);
                    if (!output) {
                        ray_graph_free(g);
                        prog->rels[body_rel].table = saved;
                        /* dl_compile_rule sets eval_err itself on genuine
                         * failures; NULL without the flag means "rule yields
                         * no rows this iteration" and should not fault. */
                        continue;
                    }

                    ray_t* raw_result = ray_execute(g, output);
                    ray_graph_free(g);
                    prog->rels[body_rel].table = saved;

                    if (!raw_result) continue;
                    if (RAY_IS_ERR(raw_result)) { prog->eval_err = true; ray_error_free(raw_result); continue; }

                    /* Rename columns to match head relation */
                    dl_rel_t* head_rel2 = &prog->rels[head_idx];
                    ray_t* result = table_rename_cols(raw_result, head_rel2);
                    ray_release(raw_result);
                    if (!result) continue;
                    if (RAY_IS_ERR(result)) { prog->eval_err = true; ray_error_free(result); continue; }

                    /* Accumulate new tuples for this head */
                    if (new_tuples_per_rel[head_idx]) {
                        ray_t* u = table_union(new_tuples_per_rel[head_idx], result);
                        ray_release(new_tuples_per_rel[head_idx]);
                        ray_release(result);
                        if (!u) {
                            prog->eval_err = true;
                            new_tuples_per_rel[head_idx] = NULL;
                            continue;
                        }
                        if (RAY_IS_ERR(u)) {
                            prog->eval_err = true;
                            ray_error_free(u);
                            new_tuples_per_rel[head_idx] = NULL;
                            continue;
                        }
                        new_tuples_per_rel[head_idx] = u;
                    } else {
                        new_tuples_per_rel[head_idx] = result;
                    }
                }
            }

            /* For each IDB: dedup new tuples, subtract existing, merge */
            for (int p = 0; p < prog->strata_sizes[s]; p++) {
                int rel_idx = prog->strata[s][p];
                dl_rel_t* rel = &prog->rels[rel_idx];
                if (!rel->is_idb) continue;

                /* Free old delta */
                if (delta_tables[rel_idx] && !RAY_IS_ERR(delta_tables[rel_idx]))
                    ray_release(delta_tables[rel_idx]);
                delta_tables[rel_idx] = NULL;

                ray_t* new_tuples = new_tuples_per_rel[rel_idx];
                if (!new_tuples) { delta_tables[rel_idx] = NULL; continue; }
                if (RAY_IS_ERR(new_tuples)) {
                    prog->eval_err = true;
                    ray_error_free(new_tuples);
                    delta_tables[rel_idx] = NULL;
                    continue;
                }

                /* Deduplicate */
                ray_t* deduped = table_distinct(new_tuples);
                ray_release(new_tuples);
                if (!deduped) { prog->eval_err = true; continue; }
                if (RAY_IS_ERR(deduped)) { prog->eval_err = true; ray_error_free(deduped); continue; }

                /* Subtract existing relation to get true delta */
                ray_t* delta = table_antijoin(deduped, rel->table);
                ray_release(deduped);
                if (!delta) { prog->eval_err = true; continue; }
                if (RAY_IS_ERR(delta)) { prog->eval_err = true; ray_error_free(delta); continue; }

                delta_tables[rel_idx] = delta;

                /* Merge delta into full relation.  A merge failure here
                 * leaves delta_tables set but rel->table stale — that would
                 * desync the fixpoint, so treat it as a hard failure. */
                if (ray_table_nrows(delta) > 0) {
                    ray_t* merged = table_union(rel->table, delta);
                    if (!merged) { prog->eval_err = true; continue; }
                    if (RAY_IS_ERR(merged)) {
                        prog->eval_err = true;
                        ray_error_free(merged);
                        continue;
                    }
                    ray_release(rel->table);
                    rel->table = merged;
                }
            }

            /* Update prev tables */
            for (int p = 0; p < prog->strata_sizes[s]; p++) {
                int rel_idx = prog->strata[s][p];
                if (prev_tables[rel_idx] && !RAY_IS_ERR(prev_tables[rel_idx]))
                    ray_release(prev_tables[rel_idx]);
                ray_retain(prog->rels[rel_idx].table);
                prev_tables[rel_idx] = prog->rels[rel_idx].table;
            }
        }

        /* Cleanup stratum temporaries */
        for (int p = 0; p < prog->strata_sizes[s]; p++) {
            int rel_idx = prog->strata[s][p];
            if (prev_tables[rel_idx] && !RAY_IS_ERR(prev_tables[rel_idx]))
                ray_release(prev_tables[rel_idx]);
            if (delta_tables[rel_idx] && !RAY_IS_ERR(delta_tables[rel_idx]))
                ray_release(delta_tables[rel_idx]);
        }
    }

    /* Build provenance if requested */
    if (prog->flags & DL_FLAG_PROVENANCE)
        dl_build_provenance(prog);

    /* Any compile-time or runtime error surfaced by a rule causes dl_eval
     * to report failure, so callers (notably ray_query_fn) can turn this
     * into a user-visible "evaluation failed" error instead of shipping a
     * silently-incomplete result. */
    return prog->eval_err ? -1 : 0;
}

/* ========================================================================
 * Query — retrieve result after evaluation
 * ======================================================================== */

ray_t* dl_query(dl_program_t* prog, const char* pred_name) {
    if (!prog || !pred_name) return NULL;
    int idx = dl_find_rel(prog, pred_name);
    if (idx < 0) return NULL;
    return prog->rels[idx].table;
}

ray_t* dl_get_provenance(dl_program_t* prog, const char* pred_name) {
    if (!prog || !pred_name) return NULL;
    if (!(prog->flags & DL_FLAG_PROVENANCE)) return NULL;
    int idx = dl_find_rel(prog, pred_name);
    if (idx < 0) return NULL;
    return prog->rels[idx].prov_col;
}

ray_t* dl_get_provenance_src_offsets(dl_program_t* prog, const char* pred_name) {
    if (!prog || !pred_name) return NULL;
    if (!(prog->flags & DL_FLAG_PROVENANCE)) return NULL;
    int idx = dl_find_rel(prog, pred_name);
    if (idx < 0) return NULL;
    return prog->rels[idx].prov_src_offsets;
}

ray_t* dl_get_provenance_src_data(dl_program_t* prog, const char* pred_name) {
    if (!prog || !pred_name) return NULL;
    if (!(prog->flags & DL_FLAG_PROVENANCE)) return NULL;
    int idx = dl_find_rel(prog, pred_name);
    if (idx < 0) return NULL;
    return prog->rels[idx].prov_src_data;
}

/* ── Builtins ── */

/* ══════════════════════════════════════════
 * EAV triple storage — datoms, assert-fact, scan-eav
 * ══════════════════════════════════════════ */

/* (datoms) — create empty EAV table with schema [e a v] */
ray_t* ray_datoms_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("arity", "datoms takes no arguments");

    int64_t e_id = ray_sym_intern("e", 1);
    int64_t a_id = ray_sym_intern("a", 1);
    int64_t v_id = ray_sym_intern("v", 1);

    ray_t* tbl = ray_table_new(3);
    if (RAY_IS_ERR(tbl)) return tbl;

    /* e column: RAY_I64 */
    ray_t* e_col = ray_vec_new(RAY_I64, 0);
    if (RAY_IS_ERR(e_col)) { ray_release(tbl); return e_col; }
    tbl = ray_table_add_col(tbl, e_id, e_col);
    ray_release(e_col);
    if (RAY_IS_ERR(tbl)) return tbl;

    /* a column: RAY_SYM */
    ray_t* a_col = ray_vec_new(RAY_SYM, 0);
    if (RAY_IS_ERR(a_col)) { ray_release(tbl); return a_col; }
    tbl = ray_table_add_col(tbl, a_id, a_col);
    ray_release(a_col);
    if (RAY_IS_ERR(tbl)) return tbl;

    /* v column: RAY_I64 (symbols stored as intern ID, integers as-is) */
    ray_t* v_col = ray_vec_new(RAY_I64, 0);
    if (RAY_IS_ERR(v_col)) { ray_release(tbl); return v_col; }
    tbl = ray_table_add_col(tbl, v_id, v_col);
    ray_release(v_col);

    return tbl;
}

/* (assert-fact db entity attr value) — append a triple to the datoms table */
ray_t* ray_assert_fact_fn(ray_t** args, int64_t n) {
    if (n != 4) return ray_error("arity", "assert-fact expects 4 arguments: db entity attr value");

    ray_t* db     = args[0];
    ray_t* entity = args[1];
    ray_t* attr   = args[2];
    ray_t* value  = args[3];

    /* Validate db is a table with 3 columns */
    if (db->type != RAY_TABLE || ray_table_ncols(db) != 3)
        return ray_error("type", "assert-fact: first arg must be a datoms table");

    /* Validate entity is i64 */
    if (entity->type != -RAY_I64)
        return ray_error("type", "assert-fact: entity must be an integer");

    /* Validate attr is a symbol */
    if (attr->type != -RAY_SYM)
        return ray_error("type", "assert-fact: attr must be a symbol");

    /* Value: accept i64 or sym. Store as i64 (sym -> intern ID). */
    int64_t v_val;
    if (value->type == -RAY_I64) {
        v_val = value->i64;
    } else if (value->type == -RAY_SYM) {
        v_val = value->i64;  /* sym intern ID is already i64 */
    } else {
        return ray_error("type", "assert-fact: value must be an integer or symbol");
    }

    /* Build new table with appended row */
    int64_t ncols = 3;
    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) return result;

    for (int64_t c = 0; c < ncols; c++) {
        ray_t* old_col = ray_table_get_col_idx(db, c);
        int64_t col_name = ray_table_col_name(db, c);

        /* Clone the column via retain + COW on append */
        ray_retain(old_col);
        ray_t* new_col = old_col;

        if (c == 0) {
            /* e column: append entity i64 */
            int64_t e_val = entity->i64;
            new_col = ray_vec_append(new_col, &e_val);
        } else if (c == 1) {
            /* a column: append attr sym ID */
            int64_t a_val = attr->i64;
            new_col = ray_vec_append(new_col, &a_val);
        } else {
            /* v column: append value as i64 */
            new_col = ray_vec_append(new_col, &v_val);
        }

        if (RAY_IS_ERR(new_col)) {
            /* ray_cow inside ray_vec_append already released old_col ref on error/copy */
            ray_release(result);
            return new_col;
        }
        /* ray_cow consumed our retain when it copied; don't double-release old_col */

        result = ray_table_add_col(result, col_name, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) return result;
    }

    return result;
}

/* (retract-fact db entity attr value) — remove a triple from the datoms table */
ray_t* ray_retract_fact_fn(ray_t** args, int64_t n) {
    if (n != 4) return ray_error("arity", "retract-fact expects 4 arguments: db entity attr value");

    ray_t* db     = args[0];
    ray_t* entity = args[1];
    ray_t* attr   = args[2];
    ray_t* value  = args[3];

    if (db->type != RAY_TABLE || ray_table_ncols(db) != 3)
        return ray_error("type", "retract-fact: first arg must be a datoms table");
    if (entity->type != -RAY_I64)
        return ray_error("type", "retract-fact: entity must be an integer");
    if (attr->type != -RAY_SYM)
        return ray_error("type", "retract-fact: attr must be a symbol");

    int64_t match_e = entity->i64;
    int64_t match_a = attr->i64;
    int64_t match_v;
    if (value->type == -RAY_I64)
        match_v = value->i64;
    else if (value->type == -RAY_SYM)
        match_v = value->i64;
    else
        return ray_error("type", "retract-fact: value must be an integer or symbol");

    /* Get existing columns */
    ray_t* e_col = ray_table_get_col_idx(db, 0);
    ray_t* a_col = ray_table_get_col_idx(db, 1);
    ray_t* v_col = ray_table_get_col_idx(db, 2);
    int64_t nrows = ray_len(e_col);

    int64_t* e_data = (int64_t*)ray_data(e_col);
    int64_t* a_data = (int64_t*)ray_data(a_col);
    int64_t* v_data = (int64_t*)ray_data(v_col);

    /* Build new columns, skipping matching rows */
    ray_t* new_e = ray_vec_new(RAY_I64, nrows);
    if (RAY_IS_ERR(new_e)) return new_e;
    ray_t* new_a = ray_vec_new(RAY_SYM, nrows);
    if (RAY_IS_ERR(new_a)) { ray_release(new_e); return new_a; }
    ray_t* new_v = ray_vec_new(RAY_I64, nrows);
    if (RAY_IS_ERR(new_v)) { ray_release(new_e); ray_release(new_a); return new_v; }

    for (int64_t r = 0; r < nrows; r++) {
        if (e_data[r] == match_e && a_data[r] == match_a && v_data[r] == match_v)
            continue; /* skip this row */
        new_e = ray_vec_append(new_e, &e_data[r]);
        if (RAY_IS_ERR(new_e)) { ray_release(new_a); ray_release(new_v); return new_e; }
        new_a = ray_vec_append(new_a, &a_data[r]);
        if (RAY_IS_ERR(new_a)) { ray_release(new_e); ray_release(new_v); return new_a; }
        new_v = ray_vec_append(new_v, &v_data[r]);
        if (RAY_IS_ERR(new_v)) { ray_release(new_e); ray_release(new_a); return new_v; }
    }

    /* Build result table */
    ray_t* result = ray_table_new(3);
    if (RAY_IS_ERR(result)) { ray_release(new_e); ray_release(new_a); ray_release(new_v); return result; }
    result = ray_table_add_col(result, ray_table_col_name(db, 0), new_e);
    ray_release(new_e);
    if (RAY_IS_ERR(result)) { ray_release(new_a); ray_release(new_v); return result; }
    result = ray_table_add_col(result, ray_table_col_name(db, 1), new_a);
    ray_release(new_a);
    if (RAY_IS_ERR(result)) { ray_release(new_v); return result; }
    result = ray_table_add_col(result, ray_table_col_name(db, 2), new_v);
    ray_release(new_v);
    return result;
}

/* (scan-eav db attr) — filter by attribute, return [e v] table
   (scan-eav db entity attr) — filter by entity+attr, return single value */
ray_t* ray_scan_eav_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 3)
        return ray_error("arity", "scan-eav expects 2 or 3 arguments");

    ray_t* db = args[0];
    if (db->type != RAY_TABLE || ray_table_ncols(db) != 3)
        return ray_error("type", "scan-eav: first arg must be a datoms table");

    ray_t* e_col = ray_table_get_col_idx(db, 0);
    ray_t* a_col = ray_table_get_col_idx(db, 1);
    ray_t* v_col = ray_table_get_col_idx(db, 2);
    int64_t nrows = ray_table_nrows(db);

    if (n == 2) {
        /* (scan-eav db attr) — filter by attribute, return [e v] table */
        ray_t* attr_arg = args[1];
        if (attr_arg->type != -RAY_SYM)
            return ray_error("type", "scan-eav: attr must be a symbol");
        int64_t attr_id = attr_arg->i64;

        int64_t e_name = ray_sym_intern("e", 1);
        int64_t v_name = ray_sym_intern("v", 1);

        ray_t* re = ray_vec_new(RAY_I64, nrows);
        if (RAY_IS_ERR(re)) return re;
        ray_t* rv = ray_vec_new(RAY_I64, nrows);
        if (RAY_IS_ERR(rv)) { ray_release(re); return rv; }

        const int64_t* e_data = (const int64_t*)ray_data(e_col);
        const int64_t* v_data = (const int64_t*)ray_data(v_col);

        for (int64_t r = 0; r < nrows; r++) {
            int64_t a_val = ray_read_sym(ray_data(a_col), r, a_col->type, a_col->attrs);
            if (a_val == attr_id) {
                re = ray_vec_append(re, &e_data[r]);
                if (RAY_IS_ERR(re)) { ray_release(rv); return re; }
                rv = ray_vec_append(rv, &v_data[r]);
                if (RAY_IS_ERR(rv)) { ray_release(re); return rv; }
            }
        }

        ray_t* result = ray_table_new(2);
        if (RAY_IS_ERR(result)) { ray_release(re); ray_release(rv); return result; }
        result = ray_table_add_col(result, e_name, re);
        ray_release(re);
        if (RAY_IS_ERR(result)) { ray_release(rv); return result; }
        result = ray_table_add_col(result, v_name, rv);
        ray_release(rv);
        return result;

    } else {
        /* (scan-eav db entity attr) — filter by entity+attr, return single value */
        ray_t* entity_arg = args[1];
        ray_t* attr_arg   = args[2];

        if (entity_arg->type != -RAY_I64)
            return ray_error("type", "scan-eav: entity must be an integer");
        if (attr_arg->type != -RAY_SYM)
            return ray_error("type", "scan-eav: attr must be a symbol");

        int64_t entity_id = entity_arg->i64;
        int64_t attr_id   = attr_arg->i64;

        const int64_t* e_data = (const int64_t*)ray_data(e_col);

        const int64_t* v_data = (const int64_t*)ray_data(v_col);

        for (int64_t r = 0; r < nrows; r++) {
            if (e_data[r] != entity_id) continue;
            int64_t a_val = ray_read_sym(ray_data(a_col), r, a_col->type, a_col->attrs);
            if (a_val == attr_id) {
                return ray_i64(v_data[r]);
            }
        }

        return ray_error("value", "scan-eav: no matching triple found");
    }
}

/* (pull db entity) — all attributes of entity as dict
   (pull db entity [attrs]) — only specified attributes as dict */
ray_t* ray_pull_fn(ray_t** args, int64_t n) {
    if (n < 2 || n > 3)
        return ray_error("arity", "pull expects 2 or 3 arguments: db entity [attrs]");

    ray_t* db     = args[0];
    ray_t* entity = args[1];

    if (db->type != RAY_TABLE || ray_table_ncols(db) != 3)
        return ray_error("type", "pull: first arg must be a datoms table");
    if (entity->type != -RAY_I64)
        return ray_error("type", "pull: entity must be an integer");

    /* Optional attribute filter */
    ray_t* attr_filter = NULL;
    int64_t n_filter = 0;
    const int64_t* filter_ids = NULL;
    if (n == 3) {
        attr_filter = args[2];
        if (!ray_is_vec(attr_filter) || attr_filter->type != RAY_SYM)
            return ray_error("type", "pull: third arg must be a symbol vector [attr ...]");
        n_filter = attr_filter->len;
        filter_ids = (const int64_t*)ray_data(attr_filter);
    }

    int64_t entity_id = entity->i64;
    ray_t* e_col = ray_table_get_col_idx(db, 0);
    ray_t* a_col = ray_table_get_col_idx(db, 1);
    ray_t* v_col = ray_table_get_col_idx(db, 2);
    int64_t nrows = ray_table_nrows(db);

    const int64_t* e_data = (const int64_t*)ray_data(e_col);
    const int64_t* v_data = (const int64_t*)ray_data(v_col);

    /* Build dict: keys SYM vec of attribute IDs, vals LIST of i64 atoms. */
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 8);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(8);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    for (int64_t r = 0; r < nrows; r++) {
        if (e_data[r] != entity_id) continue;
        int64_t a_val = ray_read_sym(ray_data(a_col), r, a_col->type, a_col->attrs);

        /* Check filter if present */
        if (attr_filter) {
            int found = 0;
            for (int64_t f = 0; f < n_filter; f++) {
                if (filter_ids[f] == a_val) { found = 1; break; }
            }
            if (!found) continue;
        }

        keys = ray_vec_append(keys, &a_val);
        if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }

        ray_t* val = ray_i64(v_data[r]);
        if (RAY_IS_ERR(val)) { ray_release(keys); ray_release(vals); return val; }
        vals = ray_list_append(vals, val);
        ray_release(val);
        if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    }

    return ray_dict_new(keys, vals);
}

/* ══════════════════════════════════════════
 * Datalog — rule definitions and query compilation
 * ══════════════════════════════════════════ */

/* Check if a symbol name starts with '?' (Datalog variable) */
static int is_dl_var(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return 0;
    ray_t* s = ray_sym_str(x->i64);
    if (!s) return 0;
    const char* p = ray_str_ptr(s);
    return p && p[0] == '?';
}

/* ══════════════════════════════════════════
 * Datalog wrappers — thin layer over src/datalog/datalog.h
 *
 * Global rule storage lives in g_dl_rules[] / g_dl_n_rules.
 * ray_rule_fn parses Rayfall (rule ...) syntax and stores rules.
 * ray_query_fn builds a temporary dl_program_t, copies global rules,
 * registers the EAV table, evaluates to fixpoint, and returns results.
 * ══════════════════════════════════════════ */

/* Global rule storage: rules defined via (rule ...) persist across queries */
static dl_rule_t  g_dl_rules[DL_MAX_RULES];
static int        g_dl_n_rules = 0;

void dl_append_global_rules(dl_program_t* prog) {
    if (!prog) return;
    for (int i = 0; i < g_dl_n_rules; i++)
        dl_add_rule(prog, &g_dl_rules[i]);
}

/* Variable name -> index map for parsing a single rule or query body */
typedef struct {
    int64_t syms[DL_MAX_ARITY * DL_MAX_BODY];
    int     n;
} dl_var_map_t;

static int dl_var_get_or_create(dl_var_map_t* map, int64_t sym_id) {
    for (int i = 0; i < map->n; i++)
        if (map->syms[i] == sym_id) return i;
    if (map->n >= DL_MAX_ARITY * DL_MAX_BODY) return -1;
    map->syms[map->n] = sym_id;
    return map->n++;
}

/* Map Rayfall comparison operator name to DL_CMP_* constant.
 * Returns -1 if not a recognized comparison. */
static int dl_cmp_op_from_name(const char* name) {
    if (strcmp(name, ">")  == 0) return DL_CMP_GT;
    if (strcmp(name, ">=") == 0) return DL_CMP_GE;
    if (strcmp(name, "<")  == 0) return DL_CMP_LT;
    if (strcmp(name, "<=") == 0) return DL_CMP_LE;
    if (strcmp(name, "==") == 0) return DL_CMP_EQ;
    if (strcmp(name, "!=") == 0) return DL_CMP_NE;
    return -1;
}

/* Map Rayfall arithmetic operator name to OP_* constant for dl_expr_t.
 * Returns -1 if not recognized. */
static int dl_arith_op_from_name(const char* name) {
    if (strcmp(name, "+") == 0) return OP_ADD;
    if (strcmp(name, "-") == 0) return OP_SUB;
    if (strcmp(name, "*") == 0) return OP_MUL;
    if (strcmp(name, "/") == 0) return OP_DIV;
    return -1;
}

/* Build a dl_expr_t from a Rayfall AST node.
 * Handles: integer constants, ?variables, (op expr expr). */
static dl_expr_t* dl_build_expr(ray_t* node, dl_var_map_t* vars) {
    if (!node) return NULL;
    if (node->type == -RAY_I64)
        return dl_expr_const(node->i64);
    if (node->type == -RAY_F64)
        return dl_expr_const_f64(node->f64);
    if (node->type == -RAY_SYM && is_dl_var(node)) {
        int vi = dl_var_get_or_create(vars, node->i64);
        return (vi >= 0) ? dl_expr_var(vi) : NULL;
    }
    if (is_list(node) && ray_len(node) == 3) {
        ray_t** elems = (ray_t**)ray_data(node);
        if (elems[0]->type == -RAY_SYM) {
            ray_t* op_str = ray_sym_str(elems[0]->i64);
            if (op_str) {
                int op = dl_arith_op_from_name(ray_str_ptr(op_str));
                if (op >= 0) {
                    dl_expr_t* l = dl_build_expr(elems[1], vars);
                    dl_expr_t* r = dl_build_expr(elems[2], vars);
                    if (l && r) return dl_expr_binop(op, l, r);
                }
            }
        }
    }
    /* Fallback: treat symbols (non-variable) as constants (sym ID) */
    if (node->type == -RAY_SYM)
        return dl_expr_const(node->i64);
    return NULL;
}

/* Check if a Rayfall list clause is a triple pattern: (?e :attr ?v)
 * A triple pattern has exactly 3 elements and the first element is a
 * ?variable (distinguishing it from rule invocations where the first
 * element is a predicate name symbol). */
static bool dl_is_wildcard(ray_t* node) {
    if (node->type != -RAY_SYM) return false;
    ray_t* s = ray_sym_str(node->i64);
    return s && ray_str_len(s) == 1 && ray_str_ptr(s)[0] == '_';
}



static bool dl_is_triple_pattern(ray_t* clause) {
    if (!is_list(clause) || ray_len(clause) != 3) return false;
    ray_t** ce = (ray_t**)ray_data(clause);
    /* Position 0 must be a ?variable, wildcard _, integer constant,
     * or quoted symbol (not a bare name that could be a rule predicate).
     * Triple patterns: (?e :attr ?v), (_ :attr ?v), (1 :attr ?v) */
    if (is_dl_var(ce[0])) return true;
    if (ce[0]->type == -RAY_I64) return true;
    if (dl_is_wildcard(ce[0]) && ce[1]->type == -RAY_SYM && !is_dl_var(ce[1]))
        return true;  /* _ is always wildcard -- reserved, never a predicate */
    /* Quoted symbol (no RAY_ATTR_NAME) in position 0 + non-var symbol in position 1 */
    if (ce[0]->type == -RAY_SYM && !(ce[0]->attrs & RAY_ATTR_NAME)) {
        if (ce[1]->type == -RAY_SYM && !is_dl_var(ce[1]))
            return true;
    }
    return false;
}

/* Check if a clause is a negation: (not (...)) */
static bool dl_is_negation(ray_t* clause) {
    if (!is_list(clause) || ray_len(clause) != 2) return false;
    ray_t** ce = (ray_t**)ray_data(clause);
    if (ce[0]->type != -RAY_SYM) return false;
    ray_t* name = ray_sym_str(ce[0]->i64);
    return name && strcmp(ray_str_ptr(name), "not") == 0;
}

/* Check if a clause is a comparison: (> ?x ?y) or (> ?x 100) */
static bool dl_is_comparison(ray_t* clause) {
    if (!is_list(clause) || ray_len(clause) < 3) return false;
    ray_t** ce = (ray_t**)ray_data(clause);
    if (ce[0]->type != -RAY_SYM) return false;
    ray_t* name = ray_sym_str(ce[0]->i64);
    if (!name) return false;
    return dl_cmp_op_from_name(ray_str_ptr(name)) >= 0;
}

/* Check if a clause is an assignment: (= ?var expr) */
static bool dl_is_assignment(ray_t* clause) {
    if (!is_list(clause) || ray_len(clause) != 3) return false;
    ray_t** ce = (ray_t**)ray_data(clause);
    if (ce[0]->type != -RAY_SYM) return false;
    ray_t* name = ray_sym_str(ce[0]->i64);
    if (!name || strcmp(ray_str_ptr(name), "=") != 0) return false;
    /* LHS must be a variable */
    return is_dl_var(ce[1]);
}

static bool dl_is_aggregate(ray_t* clause) {
    if (!is_list(clause) || ray_len(clause) < 3) return false;
    ray_t** ce = (ray_t**)ray_data(clause);
    if (ce[0]->type != -RAY_SYM) return false;
    ray_t* name = ray_sym_str(ce[0]->i64);
    if (!name) return false;
    const char* n = ray_str_ptr(name);
    return strcmp(n, "count") == 0 || strcmp(n, "sum") == 0
        || strcmp(n, "min")   == 0 || strcmp(n, "max") == 0
        || strcmp(n, "avg")   == 0;
}

static int dl_agg_op_from_name(const char* n) {
    if (strcmp(n, "count") == 0) return DL_AGG_COUNT;
    if (strcmp(n, "sum")   == 0) return DL_AGG_SUM;
    if (strcmp(n, "min")   == 0) return DL_AGG_MIN;
    if (strcmp(n, "max")   == 0) return DL_AGG_MAX;
    if (strcmp(n, "avg")   == 0) return DL_AGG_AVG;
    return -1;
}

static bool dl_sym_is_name(ray_t* sym, const char* lit) {
    if (!sym || sym->type != -RAY_SYM) return false;
    ray_t* s = ray_sym_str(sym->i64);
    return s && strcmp(ray_str_ptr(s), lit) == 0;
}

/* Resolve an AST node to a variable or constant in a body atom.
 * Sets the body position to either a variable or constant.
 * For expressions like (quote x), evaluates them first. */
static ray_t* dl_set_body_pos(dl_rule_t* rule, int bidx, int pos,
                                ray_t* node, dl_var_map_t* vars) {
    if (is_dl_var(node)) {
        int vi = dl_var_get_or_create(vars, node->i64);
        dl_body_set_var(rule, bidx, pos, vi);
        return NULL;
    }
    if (node->type == -RAY_I64) {
        dl_body_set_const_typed(rule, bidx, pos, node->i64, RAY_I64);
        return NULL;
    }
    if (node->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(node->i64);
        if (s && strcmp(ray_str_ptr(s), "_") == 0) {
            /* Wildcard: create a fresh variable */
            int vi = vars->n++;
            vars->syms[vi] = -1 - vi;
            dl_body_set_var(rule, bidx, pos, vi);
        } else {
            dl_body_set_const_typed(rule, bidx, pos, node->i64, RAY_SYM);
        }
        return NULL;
    }
    if (node->type == -RAY_STR) {
        /* Quoted string literal in body: intern as sym so it compares
         * equal to other sym-interned constants. Record the source type
         * as RAY_STR so the row-equality helper can also try a tagged-
         * payload compare against DATOM-encoded I64 columns. */
        int64_t sym = ray_sym_intern(ray_str_ptr(node), ray_str_len(node));
        dl_body_set_const_typed(rule, bidx, pos, sym, RAY_STR);
        return NULL;
    }
    /* For other forms (e.g., (quote x)), evaluate to get constant */
    ray_t* val = ray_eval(node);
    if (!val || RAY_IS_ERR(val))
        return val ? val : ray_error("type", "rule: cannot evaluate constant in body");
    if (val->type == -RAY_I64) {
        dl_body_set_const_typed(rule, bidx, pos, val->i64, RAY_I64);
    } else if (val->type == -RAY_SYM) {
        dl_body_set_const_typed(rule, bidx, pos, val->i64, RAY_SYM);
    } else {
        ray_release(val);
        return ray_error("type", "rule: unsupported constant type in body");
    }
    ray_release(val);
    return NULL;
}

/* Parse a single body clause and add it to the dl_rule_t.
 * Handles triple patterns, negations, comparisons, assignments,
 * and rule invocations (positive atoms). */
static ray_t* dl_parse_body_clause(dl_rule_t* rule, ray_t* clause,
                                     dl_var_map_t* vars, dl_program_t* prog) {
    if (!is_list(clause) || ray_len(clause) < 1)
        return ray_error("type", "rule/query: body clause must be a list");

    ray_t** ce = (ray_t**)ray_data(clause);
    int64_t clen = ray_len(clause);

    /* -- Triple pattern: (?e :attr ?v) -- */
    if (dl_is_triple_pattern(clause)) {
        /* Register as 3-arity atom on "eav" relation:
         * position 0 = entity, 1 = attr (constant), 2 = value */
        int bidx = dl_rule_add_atom(rule, "eav", 3);
        if (bidx < 0) return ray_error("domain", "rule: too many body literals");

        ray_t* err;
        err = dl_set_body_pos(rule, bidx, 0, ce[0], vars);
        if (err) return err;
        err = dl_set_body_pos(rule, bidx, 1, ce[1], vars);
        if (err) return err;
        err = dl_set_body_pos(rule, bidx, 2, ce[2], vars);
        if (err) return err;
        return NULL; /* success */
    }

    /* -- Negation: (not (?e :attr ?v))  or  (not (rule-name ?args...)) -- */
    if (dl_is_negation(clause)) {
        ray_t* inner = ce[1];
        if (!is_list(inner) || ray_len(inner) < 1)
            return ray_error("type", "not: inner clause must be a list");
        ray_t** ie = (ray_t**)ray_data(inner);
        int64_t ilen = ray_len(inner);

        if (dl_is_triple_pattern(inner)) {
            /* Negated triple: (not (?e :attr ?v)) */
            int bidx = dl_rule_add_neg(rule, "eav", 3);
            if (bidx < 0) return ray_error("domain", "rule: too many body literals");

            ray_t* err;
            err = dl_set_body_pos(rule, bidx, 0, ie[0], vars);
            if (err) return err;
            err = dl_set_body_pos(rule, bidx, 1, ie[1], vars);
            if (err) return err;
            err = dl_set_body_pos(rule, bidx, 2, ie[2], vars);
            if (err) return err;
        } else {
            /* Negated rule invocation: (not (rule-name ?a ?b)) */
            if (ie[0]->type != -RAY_SYM)
                return ray_error("type", "not: inner clause head must be a symbol");
            ray_t* pred_name = ray_sym_str(ie[0]->i64);
            if (!pred_name)
                return ray_error("type", "not: cannot resolve predicate name");

            int bidx = dl_rule_add_neg(rule, ray_str_ptr(pred_name), (int)(ilen - 1));
            if (bidx < 0) return ray_error("domain", "rule: too many body literals");

            for (int64_t j = 1; j < ilen; j++) {
                ray_t* err = dl_set_body_pos(rule, bidx, (int)(j - 1), ie[j], vars);
                if (err) return err;
            }
        }
        return NULL;
    }

    /* -- Aggregate: (count ?N pred) | (sum ?S pred col) | ... [by ?k col ...] -- */
    if (dl_is_aggregate(clause)) {
        ray_t* op_str = ray_sym_str(ce[0]->i64);
        if (!op_str) return ray_error("type", "aggregate: bad operator");
        int op = dl_agg_op_from_name(ray_str_ptr(op_str));
        if (op < 0) return ray_error("type", "aggregate: unknown operator");

        if (!is_dl_var(ce[1]))
            return ray_error("type", "aggregate: first argument must be ?variable");
        int target_vi = dl_var_get_or_create(vars, ce[1]->i64);
        if (target_vi < 0)
            return ray_error("domain", "aggregate: too many variables");

        if (ce[2]->type != -RAY_SYM)
            return ray_error("type", "aggregate: predicate must be a symbol");
        ray_t* pred_sym = ray_sym_str(ce[2]->i64);
        if (!pred_sym)
            return ray_error("type", "aggregate: cannot resolve predicate name");
        const char* pred_name = ray_str_ptr(pred_sym);

        /* Record arity=0 as "unknown" when we can't resolve it against the
         * program (prog=NULL or predicate not yet registered).  The compiler
         * and env auto-register treat 0 as a wildcard and resolve against the
         * source relation at evaluation time.  A hardcoded 1 would spuriously
         * reject any env-bound table whose arity isn't 1. */
        int pred_arity = 0;
        if (prog) {
            int ri = dl_find_rel(prog, pred_name);
            if (ri >= 0) pred_arity = prog->rels[ri].arity;
        }

        int i = 3;
        bool has_value_col = false;
        int value_col = 0;
        int key_vars[DL_AGG_MAX_KEYS];
        int key_cols[DL_AGG_MAX_KEYS];
        int n_keys = 0;

        while (i < clen) {
            if (dl_sym_is_name(ce[i], "by")) {
                i++;
                while (i < clen) {
                    if (!is_dl_var(ce[i]))
                        return ray_error("type", "aggregate: group key must be ?variable");
                    if (n_keys >= DL_AGG_MAX_KEYS)
                        return ray_error("domain", "aggregate: too many group keys");
                    key_vars[n_keys] = dl_var_get_or_create(vars, ce[i]->i64);
                    i++;
                    if (i >= clen || ce[i]->type != -RAY_I64)
                        return ray_error("type", "aggregate: group key column must be integer");
                    key_cols[n_keys] = (int)ce[i]->i64;
                    i++;
                    n_keys++;
                }
                break;
            }
            if (ce[i]->type == -RAY_I64) {
                if (has_value_col)
                    return ray_error("type", "aggregate: at most one value column index");
                has_value_col = true;
                value_col = (int)ce[i]->i64;
                i++;
                continue;
            }
            return ray_error("type", "aggregate: unexpected token in aggregate clause");
        }

        if (op == DL_AGG_COUNT) {
            if (has_value_col)
                return ray_error("type", "aggregate: count does not take a value column");
        } else {
            if (!has_value_col)
                return ray_error("type", "aggregate: sum/min/max/avg require a value column index");
        }

        int bidx = dl_rule_add_agg(rule, op, target_vi, pred_name, pred_arity, has_value_col ? value_col : 0);
        if (bidx < 0) return ray_error("domain", "rule: too many body literals");
        if (n_keys > 0) {
            if (dl_rule_agg_set_group(rule, bidx, key_vars, key_cols, n_keys) != 0)
                return ray_error("domain", "aggregate: cannot attach group keys");
        }
        return NULL;
    }

    /* -- Between sugar: (between ?x lo hi) -> (>= ?x lo) and (<= ?x hi) -- */
    if (clen == 4 && ce[0]->type == -RAY_SYM) {
        ray_t* nm = ray_sym_str(ce[0]->i64);
        if (nm && strcmp(ray_str_ptr(nm), "between") == 0) {
            if (!is_dl_var(ce[1]))
                return ray_error("type", "between target must be a ?variable");
            int vi = dl_var_get_or_create(vars, ce[1]->i64);
            if (vi < 0)
                return ray_error("domain", "between: too many variables");
            if (ce[2]->type != -RAY_I64 || ce[3]->type != -RAY_I64)
                return ray_error("type", "between bounds must be integer constants");
            if (dl_rule_add_cmp_const(rule, DL_CMP_GE, vi, ce[2]->i64) < 0)
                return ray_error("domain", "rule: too many body literals");
            if (dl_rule_add_cmp_const(rule, DL_CMP_LE, vi, ce[3]->i64) < 0)
                return ray_error("domain", "rule: too many body literals");
            return NULL;
        }
    }

    /* -- Assignment: (= ?var expr) -- */
    if (dl_is_assignment(clause)) {
        int target_vi = dl_var_get_or_create(vars, ce[1]->i64);
        dl_expr_t* expr = dl_build_expr(ce[2], vars);
        if (!expr)
            return ray_error("type", "rule: cannot parse assignment expression");
        dl_rule_add_assign(rule, target_vi, DL_OP_EQ, expr);
        return NULL;
    }

    /* -- Comparison: (> ?x ?y) or (> ?x 100) -- */
    if (dl_is_comparison(clause)) {
        ray_t* op_str = ray_sym_str(ce[0]->i64);
        int cmp_op = dl_cmp_op_from_name(ray_str_ptr(op_str));

        /* LHS */
        bool lhs_is_var = is_dl_var(ce[1]);
        int lhs_vi = lhs_is_var ? dl_var_get_or_create(vars, ce[1]->i64) : -1;
        bool lhs_is_const = (!lhs_is_var && (ce[1]->type == -RAY_I64 || ce[1]->type == -RAY_SYM));
        int64_t lhs_const = lhs_is_const ? ce[1]->i64 : 0;

        /* RHS */
        bool rhs_is_var = (clen > 2) && is_dl_var(ce[2]);
        int rhs_vi = rhs_is_var ? dl_var_get_or_create(vars, ce[2]->i64) : -1;
        bool rhs_is_const = (clen > 2) && !rhs_is_var &&
                            (ce[2]->type == -RAY_I64 || ce[2]->type == -RAY_SYM);
        int64_t rhs_const = rhs_is_const ? ce[2]->i64 : 0;

        if (lhs_is_var && rhs_is_var) {
            dl_rule_add_cmp(rule, cmp_op, lhs_vi, rhs_vi);
        } else if (lhs_is_var && rhs_is_const) {
            dl_rule_add_cmp_const(rule, cmp_op, lhs_vi, rhs_const);
        } else if (lhs_is_const && rhs_is_var) {
            /* Flip: const op var -> var flipped_op const */
            int flipped = cmp_op;
            switch (cmp_op) {
            case DL_CMP_GT: flipped = DL_CMP_LT; break;
            case DL_CMP_GE: flipped = DL_CMP_LE; break;
            case DL_CMP_LT: flipped = DL_CMP_GT; break;
            case DL_CMP_LE: flipped = DL_CMP_GE; break;
            default: break;
            }
            dl_rule_add_cmp_const(rule, flipped, rhs_vi, lhs_const);
        } else {
            /* Expression-based comparison */
            dl_expr_t* le = dl_build_expr(ce[1], vars);
            dl_expr_t* re = (clen > 2) ? dl_build_expr(ce[2], vars) : NULL;
            if (le && re)
                dl_rule_add_cmp_expr(rule, cmp_op, le, re);
            else
                return ray_error("type", "rule: cannot parse comparison operands");
        }
        return NULL;
    }

    /* -- Rule invocation / positive atom: (pred-name ?a ?b ...) -- */
    if (ce[0]->type == -RAY_SYM) {
        ray_t* pred_name = ray_sym_str(ce[0]->i64);
        if (!pred_name)
            return ray_error("type", "rule: cannot resolve predicate name");

        int bidx = dl_rule_add_atom(rule, ray_str_ptr(pred_name), (int)(clen - 1));
        if (bidx < 0) return ray_error("domain", "rule: too many body literals");

        for (int64_t j = 1; j < clen; j++) {
            ray_t* err = dl_set_body_pos(rule, bidx, (int)(j - 1), ce[j], vars);
            if (err) return err;
        }
        return NULL;
    }

    return ray_error("type", "rule/query: unrecognized body clause form");
}

/* Parse head + body clauses into out (shared by rule and query inline rules). */
static ray_t* dl_parse_rule_from_head_and_body(dl_rule_t* out, ray_t* head,
                                                ray_t** body_args, int64_t n_body,
                                                dl_var_map_t* vars, dl_program_t* prog) {
    if (!is_list(head) || ray_len(head) < 1)
        return ray_error("type", "rule: head must be (name ?var ...)");

    ray_t** hd = (ray_t**)ray_data(head);
    int64_t hlen = ray_len(head);

    if (hd[0]->type != -RAY_SYM)
        return ray_error("type", "rule: head name must be a symbol");

    ray_t* head_name_str = ray_sym_str(hd[0]->i64);
    if (!head_name_str)
        return ray_error("type", "rule: cannot resolve head name");

    if (ray_str_len(head_name_str) == 1 && ray_str_ptr(head_name_str)[0] == '_')
        return ray_error("domain", "rule: _ is reserved as wildcard");

    int head_arity = (int)(hlen - 1);
    dl_rule_init(out, ray_str_ptr(head_name_str), head_arity);

    for (int i = 0; i < head_arity; i++) {
        ray_t* harg = hd[i + 1];
        if (is_dl_var(harg)) {
            int vi = dl_var_get_or_create(vars, harg->i64);
            dl_rule_head_var(out, i, vi);
        } else if (harg->type == -RAY_I64) {
            dl_rule_head_const_typed(out, i, harg->i64, RAY_I64);
        } else if (harg->type == -RAY_SYM) {
            dl_rule_head_const_typed(out, i, harg->i64, RAY_SYM);
        } else if (harg->type == -RAY_F64) {
            int64_t bits;
            memcpy(&bits, &harg->f64, sizeof(bits));
            dl_rule_head_const_typed(out, i, bits, RAY_F64);
        } else if (harg->type == -RAY_STR) {
            /* Intern the string as a sym so it can be stored in a RAY_SYM
             * column.  Matches the body-literal parser convention. */
            int64_t sym = ray_sym_intern(ray_str_ptr(harg), ray_str_len(harg));
            dl_rule_head_const_typed(out, i, sym, RAY_SYM);
        } else {
            return ray_error("type", "rule: head arguments must be ?variables or constants");
        }
    }

    for (int64_t i = 0; i < n_body; i++) {
        ray_t* err = dl_parse_body_clause(out, body_args[i], vars, prog);
        if (err) return err;
    }

    out->n_vars = vars->n;
    return NULL;
}

/* One inline rule: ((head-name ?a ...) body1 body2 ...) */
static ray_t* dl_parse_inline_rule(dl_rule_t* out, ray_t* rule_list, dl_program_t* prog) {
    if (!is_list(rule_list) || ray_len(rule_list) < 1)
        return ray_error("type", "query: each (rules ...) entry must be a non-empty list");

    ray_t** re = (ray_t**)ray_data(rule_list);
    int64_t rlen = ray_len(rule_list);
    dl_var_map_t vars;
    memset(&vars, 0, sizeof(vars));
    return dl_parse_rule_from_head_and_body(out, re[0], &re[1], rlen - 1, &vars, prog);
}

/* (rule (head-name ?v1 ?v2 ...) clause1 clause2 ...)
 * Special form: args are NOT evaluated.
 * Parses the head and body into a dl_rule_t and stores it globally. */
ray_t* ray_rule_fn(ray_t** args, int64_t n) {
    if (n < 2)
        return ray_error("arity", "rule expects at least a head and one body clause");

    if (g_dl_n_rules >= DL_MAX_RULES)
        return ray_error("domain", "rule: too many rules (max 128)");

    dl_var_map_t vars;
    memset(&vars, 0, sizeof(vars));
    dl_rule_t rule;
    ray_t* perr = dl_parse_rule_from_head_and_body(&rule, args[0], &args[1], n - 1, &vars, NULL);
    if (perr) return perr;

    memcpy(&g_dl_rules[g_dl_n_rules++], &rule, sizeof(dl_rule_t));
    return ray_bool(true);
}

/* (query db (find ?a ?b ...) (where clause1 clause2 ...) [(rules ...)])
 * Optional fourth arg (rules ...) supplies inline rules only (globals ignored).
 * Special form: db is evaluated, find/where are NOT evaluated.
 * Creates a temporary dl_program_t, registers the EAV table,
 * copies global rules (unless inline rules), builds a synthetic query rule, and evaluates. */
ray_t* ray_query_fn(ray_t** args, int64_t n) {
    if (n < 3 || n > 4)
        return ray_error("arity", "query expects: db (find ...) (where ...) [(rules ...)]");

    /* Evaluate db (first arg) */
    ray_t* db = ray_eval(args[0]);
    if (!db || RAY_IS_ERR(db)) return db ? db : ray_error("type", "query: db is null");
    if (db->type != RAY_TABLE) { ray_release(db); return ray_error("type", "query: first arg must be a datoms table"); }

    /* Parse find clause */
    ray_t* find_clause = args[1];
    if (!is_list(find_clause) || ray_len(find_clause) < 2) {
        ray_release(db);
        return ray_error("type", "query: second arg must be (find ?var ...)");
    }
    ray_t** find_elems = (ray_t**)ray_data(find_clause);
    int64_t find_len = ray_len(find_clause);

    /* Verify it starts with 'find' */
    if (find_elems[0]->type != -RAY_SYM) {
        ray_release(db);
        return ray_error("type", "query: expected (find ...)");
    }
    ray_t* find_name = ray_sym_str(find_elems[0]->i64);
    if (!find_name || strcmp(ray_str_ptr(find_name), "find") != 0) {
        ray_release(db);
        return ray_error("type", "query: expected (find ...) as second argument");
    }

    /* Collect find variable sym IDs */
    int64_t find_var_syms[DL_MAX_ARITY];
    int n_find_vars = 0;
    for (int64_t i = 1; i < find_len && n_find_vars < DL_MAX_ARITY; i++) {
        if (!is_dl_var(find_elems[i])) {
            ray_release(db);
            return ray_error("type", "query: find arguments must be ?variables");
        }
        find_var_syms[n_find_vars++] = find_elems[i]->i64;
    }

    /* Parse where clause */
    ray_t* where_clause = args[2];
    if (!is_list(where_clause) || ray_len(where_clause) < 2) {
        ray_release(db);
        return ray_error("type", "query: third arg must be (where clause ...)");
    }
    ray_t** where_elems = (ray_t**)ray_data(where_clause);
    int64_t where_len = ray_len(where_clause);

    /* Verify it starts with 'where' */
    if (where_elems[0]->type != -RAY_SYM) {
        ray_release(db);
        return ray_error("type", "query: expected (where ...)");
    }
    ray_t* where_name = ray_sym_str(where_elems[0]->i64);
    if (!where_name || strcmp(ray_str_ptr(where_name), "where") != 0) {
        ray_release(db);
        return ray_error("type", "query: expected (where ...) as third argument");
    }

    /* Optional 4th arg must be (rules ...) — inline rules override globals */
    ray_t* rules_clause = NULL;
    if (n == 4) {
        ray_t* fourth = args[3];
        if (!is_list(fourth) || ray_len(fourth) < 1) {
            ray_release(db);
            return ray_error("type", "query: fourth argument must be (rules ...)");
        }
        ray_t** re4 = (ray_t**)ray_data(fourth);
        if (re4[0]->type != -RAY_SYM) {
            ray_release(db);
            return ray_error("type", "query: fourth argument must be (rules ...)");
        }
        ray_t* rname = ray_sym_str(re4[0]->i64);
        if (!rname || strcmp(ray_str_ptr(rname), "rules") != 0) {
            ray_release(db);
            return ray_error("type", "query: fourth argument must be (rules ...)");
        }
        rules_clause = fourth;
    }

    /* Build variable map for the query */
    dl_var_map_t vars;
    memset(&vars, 0, sizeof(vars));

    /* Pre-populate the variable map with find variables so they get
     * the lowest indices (0, 1, 2, ...) -- makes projection trivial */
    for (int i = 0; i < n_find_vars; i++)
        dl_var_get_or_create(&vars, find_var_syms[i]);

    /* Build synthetic query rule: __query(?find_vars...) :- body_clauses... */
    dl_rule_t qrule;
    dl_rule_init(&qrule, "__query", n_find_vars);
    for (int i = 0; i < n_find_vars; i++)
        dl_rule_head_var(&qrule, i, i);

    /* Parse body clauses into the query rule */
    for (int64_t i = 1; i < where_len; i++) {
        ray_t* err = dl_parse_body_clause(&qrule, where_elems[i], &vars, NULL);
        if (err) { ray_release(db); return err; }
    }
    qrule.n_vars = vars.n;

    /* Create temporary program */
    dl_program_t* prog = dl_program_new();
    if (!prog) { ray_release(db); return ray_error("oom", "query: cannot create program"); }

    /* Register the EAV table as a 3-arity "eav" relation.
     * The 'a' column is RAY_SYM with adaptive width -- the Datalog engine
     * operates on I64 data only, so convert SYM columns to I64 first. */
    {
        int64_t nrows_db = ray_table_nrows(db);
        ray_t* eav_tbl = ray_table_new(3);
        for (int c = 0; c < 3; c++) {
            ray_t* col = ray_table_get_col_idx(db, c);
            if (!col) continue;
            if (col->type == RAY_SYM) {
                /* Convert SYM -> I64: read sym IDs via ray_read_sym */
                ray_t* i64col = ray_vec_new(RAY_I64, nrows_db);
                if (i64col && !RAY_IS_ERR(i64col)) {
                    i64col->len = nrows_db;
                    int64_t* d = (int64_t*)ray_data(i64col);
                    for (int64_t r = 0; r < nrows_db; r++)
                        d[r] = ray_read_sym(ray_data(col), r, col->type, col->attrs);
                    eav_tbl = ray_table_add_col(eav_tbl, ray_table_col_name(db, c), i64col);
                    ray_release(i64col);
                }
            } else {
                eav_tbl = ray_table_add_col(eav_tbl, ray_table_col_name(db, c), col);
            }
        }
        dl_add_edb(prog, "eav", eav_tbl, 3);
        ray_release(eav_tbl);
    }

    if (rules_clause) {
        ray_t** re = (ray_t**)ray_data(rules_clause);
        int64_t rlen = ray_len(rules_clause);
        for (int64_t i = 1; i < rlen; i++) {
            dl_rule_t irule;
            ray_t* rerr = dl_parse_inline_rule(&irule, re[i], prog);
            if (rerr) {
                dl_program_free(prog);
                ray_release(db);
                return rerr;
            }
            if (dl_add_rule(prog, &irule) < 0) {
                dl_program_free(prog);
                ray_release(db);
                return ray_error("domain", "query: too many rules");
            }
        }
    } else {
        for (int i = 0; i < g_dl_n_rules; i++)
            dl_add_rule(prog, &g_dl_rules[i]);
    }

    /* Add the synthetic query rule */
    dl_add_rule(prog, &qrule);

    /* Auto-register env-bound EDB tables referenced from rule bodies.
     *
     * Rationale: the primary `db` argument becomes the `eav` EDB (above).
     * User rules can also reference additional relations by name
     * (e.g. `(facts_i64 ?e ?a ?v)`). Rather than force callers to pre-declare
     * every EDB, scan the program's rule bodies for positive / negative atom
     * predicates that are not yet known as a relation, look them up in the
     * global ray env, and register them when they resolve to a RAY_TABLE of
     * matching arity. SYM columns are converted to I64 (same treatment as
     * the primary `eav` table).
     *
     * Aggregate sources are handled too (`DL_AGG` uses `agg_pred`).
     * The built-in synthetic "__query" / "eav" names are skipped. */
    for (int ri = 0; ri < prog->n_rules; ri++) {
        dl_rule_t* rr = &prog->rules[ri];
        for (int bi = 0; bi < rr->n_body; bi++) {
            dl_body_t* bd = &rr->body[bi];
            const char* pred_name = NULL;
            int pred_arity = 0;

            if (bd->type == DL_POS || bd->type == DL_NEG) {
                pred_name = bd->pred;
                pred_arity = bd->arity;
            } else if (bd->type == DL_AGG) {
                pred_name = bd->agg_pred;
                pred_arity = bd->agg_arity;
            } else {
                continue;
            }

            if (!pred_name || pred_name[0] == '\0') continue;
            if (strcmp(pred_name, "eav") == 0) continue;
            if (dl_find_rel(prog, pred_name) >= 0) continue;

            int64_t env_sym = ray_sym_intern(pred_name, strlen(pred_name));
            ray_t* env_val = ray_env_get(env_sym);
            if (!env_val || env_val->type != RAY_TABLE) continue;
            int64_t ncols = ray_table_ncols(env_val);
            /* pred_arity == 0 is a "not yet known" sentinel used when the
             * aggregate parser couldn't resolve the source predicate's arity
             * at parse time (prog=NULL, surface syntax).  Resolve it from the
             * env-bound table's column count now. */
            if (pred_arity == 0) pred_arity = (int)ncols;
            if (ncols != pred_arity) continue;

            int64_t nrows_env = ray_table_nrows(env_val);
            ray_t* clean = ray_table_new(pred_arity);
            if (!clean || RAY_IS_ERR(clean)) {
                if (clean) ray_release(clean);
                dl_program_free(prog);
                ray_release(db);
                return ray_error("memory", "query: failed to create env-backed EDB table");
            }
            for (int c = 0; c < pred_arity; c++) {
                ray_t* col = ray_table_get_col_idx(env_val, c);
                ray_t* next_clean;
                if (!col) {
                    /* Silently skipping would build `clean` with fewer than
                     * pred_arity columns yet still register it via dl_add_edb
                     * — the program would see a schema-inconsistent EDB. */
                    ray_release(clean);
                    dl_program_free(prog);
                    ray_release(db);
                    return ray_error("schema", "query: env-backed EDB table missing expected column");
                }
                if (col->type == RAY_SYM) {
                    ray_t* i64col = ray_vec_new(RAY_I64, nrows_env);
                    if (!i64col) {
                        ray_release(clean);
                        dl_program_free(prog);
                        ray_release(db);
                        return ray_error("memory", "query: failed to convert env-backed SYM column");
                    }
                    if (RAY_IS_ERR(i64col)) {
                        ray_error_free(i64col);
                        ray_release(clean);
                        dl_program_free(prog);
                        ray_release(db);
                        return ray_error("memory", "query: failed to convert env-backed SYM column");
                    }
                    i64col->len = nrows_env;
                    int64_t* d = (int64_t*)ray_data(i64col);
                    for (int64_t r = 0; r < nrows_env; r++)
                        d[r] = ray_read_sym(ray_data(col), r, col->type, col->attrs);
                    next_clean = ray_table_add_col(clean, ray_table_col_name(env_val, c), i64col);
                    ray_release(i64col);
                } else {
                    next_clean = ray_table_add_col(clean, ray_table_col_name(env_val, c), col);
                }
                if (!next_clean) {
                    ray_release(clean);
                    dl_program_free(prog);
                    ray_release(db);
                    return ray_error("memory", "query: failed to build env-backed EDB table");
                }
                if (RAY_IS_ERR(next_clean)) {
                    ray_error_free(next_clean);
                    ray_release(clean);
                    dl_program_free(prog);
                    ray_release(db);
                    return ray_error("memory", "query: failed to build env-backed EDB table");
                }
                clean = next_clean;
            }
            if (dl_add_edb(prog, pred_name, clean, pred_arity) < 0) {
                ray_release(clean);
                dl_program_free(prog);
                ray_release(db);
                return ray_error("domain", "query: failed to register env-backed EDB table");
            }
            ray_release(clean);
        }
    }

    /* Stratify and evaluate */
    if (dl_stratify(prog) != 0) {
        dl_program_free(prog);
        ray_release(db);
        return ray_error("domain", "query: unstratifiable negation cycle");
    }

    if (dl_eval(prog) != 0) {
        /* Preserve any inner-error detail that ray_error() stashed in the
         * thread-local error buffer before dl_eval freed the offending
         * object. Without this, callers see a generic "evaluation failed". */
        char prev[256];
        const char* p = ray_error_msg();
        prev[0] = '\0';
        if (p && *p) {
            strncpy(prev, p, sizeof(prev) - 1);
            prev[sizeof(prev) - 1] = '\0';
        }
        dl_program_free(prog);
        ray_release(db);
        if (prev[0]) {
            return ray_error("domain", "query: evaluation failed: %s", prev);
        }
        return ray_error("domain", "query: evaluation failed");
    }

    /* Get the result */
    ray_t* raw = dl_query(prog, "__query");
    if (!raw || RAY_IS_ERR(raw)) {
        dl_program_free(prog);
        ray_release(db);
        return raw ? raw : ray_error("domain", "query: no result");
    }

    /* Build result table with user-friendly column names (the ?variable names) */
    int64_t nrows = ray_table_nrows(raw);
    int64_t ncols = ray_table_ncols(raw);
    ray_t* result = ray_table_new(n_find_vars);
    for (int i = 0; i < n_find_vars && i < (int)ncols; i++) {
        ray_t* col = ray_table_get_col_idx(raw, i);
        if (col)
            result = ray_table_add_col(result, find_var_syms[i], col);
    }

    /* Handle empty result: ensure schema is correct */
    if (nrows == 0 && n_find_vars > 0 && ray_table_ncols(result) == 0) {
        ray_release(result);
        result = ray_table_new(n_find_vars);
        for (int i = 0; i < n_find_vars; i++) {
            ray_t* ev = ray_vec_new(RAY_I64, 0);
            if (!RAY_IS_ERR(ev)) {
                result = ray_table_add_col(result, find_var_syms[i], ev);
                ray_release(ev);
            }
        }
    }

    dl_program_free(prog);
    ray_release(db);
    return result;
}

/* ══════════════════════════════════════════
 * Programmatic Datalog API builtins
 * ══════════════════════════════════════════ */

/* Opaque handle for dl_program_t stored in a ray_t atom.
 * We store the pointer in the i64 field. */
static ray_t* dl_wrap_program(dl_program_t* prog) {
    ray_t* obj = ray_alloc(0);
    if (!obj || RAY_IS_ERR(obj)) return ray_error("oom", NULL);
    obj->type = -RAY_I64;
    obj->i64 = (int64_t)(uintptr_t)prog;
    return obj;
}

static dl_program_t* dl_unwrap_program(ray_t* obj) {
    if (!obj || obj->type != -RAY_I64) return NULL;
    return (dl_program_t*)(uintptr_t)obj->i64;
}

/* (dl-program) — create a new empty dl_program_t */
ray_t* ray_dl_program_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 0) return ray_error("arity", "dl-program takes no arguments");
    dl_program_t* prog = dl_program_new();
    if (!prog) return ray_error("oom", "dl-program: cannot allocate");
    return dl_wrap_program(prog);
}

/* (dl-add-edb prog "name" table arity) — register EDB */
ray_t* ray_dl_add_edb_fn(ray_t** args, int64_t n) {
    if (n != 4) return ray_error("arity", "dl-add-edb expects: prog name table arity");
    dl_program_t* prog = dl_unwrap_program(args[0]);
    if (!prog) return ray_error("type", "dl-add-edb: first arg must be a dl-program");

    /* Name can be a symbol or string */
    const char* name = NULL;
    ray_t* name_str = NULL;
    if (args[1]->type == -RAY_SYM) {
        name_str = ray_sym_str(args[1]->i64);
        name = name_str ? ray_str_ptr(name_str) : NULL;
    }
    if (!name) return ray_error("type", "dl-add-edb: name must be a symbol");

    if (args[2]->type != RAY_TABLE)
        return ray_error("type", "dl-add-edb: third arg must be a table");
    if (args[3]->type != -RAY_I64)
        return ray_error("type", "dl-add-edb: arity must be an integer");

    int rc = dl_add_edb(prog, name, args[2], (int)args[3]->i64);
    return (rc >= 0) ? ray_bool(true) : ray_error("domain", "dl-add-edb: failed");
}

/* (dl-stratify prog) — compute strata */
ray_t* ray_dl_stratify_fn(ray_t* x) {
    dl_program_t* prog = dl_unwrap_program(x);
    if (!prog) return ray_error("type", "dl-stratify: arg must be a dl-program");
    int rc = dl_stratify(prog);
    return (rc == 0) ? ray_bool(true) : ray_error("domain", "dl-stratify: unstratifiable");
}

/* (dl-eval prog) — evaluate to fixpoint */
ray_t* ray_dl_eval_fn(ray_t* x) {
    dl_program_t* prog = dl_unwrap_program(x);
    if (!prog) return ray_error("type", "dl-eval: arg must be a dl-program");
    int rc = dl_eval(prog);
    return (rc == 0) ? ray_bool(true) : ray_error("domain", "dl-eval: evaluation failed");
}

/* (dl-query prog "pred") — get result table */
ray_t* ray_dl_query_fn(ray_t* prog_obj, ray_t* pred_obj) {
    dl_program_t* prog = dl_unwrap_program(prog_obj);
    if (!prog) return ray_error("type", "dl-query: first arg must be a dl-program");

    const char* pred = NULL;
    if (pred_obj->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(pred_obj->i64);
        pred = s ? ray_str_ptr(s) : NULL;
    }
    if (!pred) return ray_error("type", "dl-query: pred must be a symbol");

    ray_t* result = dl_query(prog, pred);
    if (!result) return ray_error("domain", "dl-query: predicate not found");
    ray_retain(result);
    return result;
}

/* (dl-provenance prog "pred") — get provenance column */
ray_t* ray_dl_provenance_fn(ray_t* prog_obj, ray_t* pred_obj) {
    dl_program_t* prog = dl_unwrap_program(prog_obj);
    if (!prog) return ray_error("type", "dl-provenance: first arg must be a dl-program");

    const char* pred = NULL;
    if (pred_obj->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(pred_obj->i64);
        pred = s ? ray_str_ptr(s) : NULL;
    }
    if (!pred) return ray_error("type", "dl-provenance: pred must be a symbol");

    ray_t* prov = dl_get_provenance(prog, pred);
    if (!prov) return ray_error("domain", "dl-provenance: not available");
    ray_retain(prov);
    return prov;
}

/* Reset global Datalog rule storage (called from ray_lang_destroy) */
void ray_dl_reset_rules(void) {
    g_dl_n_rules = 0;
}
