/* Copyright (c) 2026 Anton Kundenko <singaraiona@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Conservative composition of literal select/from/select pairs (#752).
 * This is an admission proof, not a general expression simplifier. Only
 * total, row-local expressions may move across a filter. In particular,
 * (- x (avg x)), windows, vector literals, user calls and casts are barriers.
 */
#include "lang/internal.h"
#include "lang/env.h"
#include "ops/internal.h"
#include "table/dict.h"
#include "table/sym.h"
#include <string.h>

#define MERGE_COLS 128
#define MERGE_NODES 1024
#define MERGE_MIN_ROWS 8192
#define MERGE_PREFIX_ROWS 64

/* Runtime restarts invalidate intern IDs. Keep the cache thread-local and
 * refresh it once per symbol-table lifetime, not once per clause lookup. */
static _Thread_local uint64_t merge_epoch;
static _Thread_local int64_t merge_ids[9];

typedef struct {
    int64_t name;
    ray_t* expr;                    /* owned, expanded against the base */
    int type;
    unsigned uses;
    bool computed;
} merge_col_t;

typedef struct {
    ray_t* table;
    merge_col_t cols[MERGE_COLS];
    int n, nodes;
    bool projected;
} merge_scope_t;

static void merge_drop(ray_t* x) {
    if (x && RAY_IS_ERR(x)) ray_error_free(x);
    else if (x) ray_release(x);
}

static ray_t* merge_ref(ray_t* x) { ray_retain(x); return x; }

/* Fresh fixed-arity lists need no COW/growth operation. There is no partially
 * built list to lose if an append allocation fails. */
static ray_t* merge_call(ray_t* head, ray_t* a, ray_t* b) {
    ray_t* r = ray_list_new(b ? 3 : 2);
    if (!r || RAY_IS_ERR(r)) { merge_drop(r); return NULL; }
    ray_t** es = ray_data(r);
    es[0] = merge_ref(head); es[1] = merge_ref(a);
    if (b) es[2] = merge_ref(b);
    r->len = b ? 3 : 2;
    return r;
}

static int merge_clause(int64_t id) {
    for (int i = 1; i <= 7; i++) if (id == merge_ids[i]) return i;
    return 0;
}

static bool merge_dict_shape(ray_t* d) {
    if (!d || d->type != RAY_DICT) return false;
    ray_t* k = ray_dict_keys(d), *v = ray_dict_vals(d);
    if (!k || k->type != RAY_SYM || !v || v->type != RAY_LIST || k->len != v->len ||
        k->len > MERGE_COLS) return false;
    return true;
}

static bool merge_dict_ok(ray_t* d) {
    if (!merge_dict_shape(d)) return false;
    ray_t* k = ray_dict_keys(d);
    /* A duplicate clause has evaluation-order semantics; never normalize it. */
    for (int64_t i = 0; i < k->len; i++)
        for (int64_t j = 0; j < i; j++)
            if (sym_cell_runtime_id(k, i) == sym_cell_runtime_id(k, j)) return false;
    return true;
}

static ray_t* merge_get(ray_t* d, int clause) {
    ray_t* k = ray_dict_keys(d), **v = ray_data(ray_dict_vals(d));
    for (int64_t i = 0; i < k->len; i++)
        if (sym_cell_runtime_id(k, i) == merge_ids[clause]) return v[i];
    return NULL;
}

static ray_t* merge_put(ray_t* d, int64_t id, ray_t* value) {
    ray_t* k = ray_sym(id);
    if (!k || RAY_IS_ERR(k)) { merge_drop(k); merge_drop(d); return NULL; }
    ray_t* r = ray_dict_upsert(d, k, value);
    ray_release(k);
    if (r && RAY_IS_ERR(r)) { merge_drop(r); return NULL; }
    return r;
}

/* Reject shadowed builtins, including source columns used as call heads. */
static ray_t* merge_fn(merge_scope_t* s, ray_t* head) {
    if (!head || head->type != -RAY_SYM || (head->attrs & ATTR_QUOTED) ||
        ray_table_get_col(s->table, head->i64) || ray_env_has_lexical_local(head->i64)) return NULL;
    for (int i = 0; i < s->n; i++) if (s->cols[i].name == head->i64) return NULL;
    return ray_env_get(head->i64);
}

static bool merge_numeric(int t) { return t == RAY_I64 || t == RAY_F64; }

/* Charge the expanded tree, not just alias references. Repeated aliases can
 * otherwise turn a small input DAG into exponential work for the compiler. */
static bool merge_charge(merge_scope_t* s, ray_t* e, int depth) {
    if (depth > 16 || ++s->nodes > MERGE_NODES) return false;
    if (e->type != RAY_LIST) return true;
    ray_t** es = ray_data(e);
    for (int64_t i = 1; i < e->len; i++) if (!merge_charge(s, es[i], depth + 1)) return false;
    return true;
}

/* Return an owned expansion, or NULL to decline. `column` distinguishes
 * scalar expressions from expressions preserving the input row count. */
static ray_t* merge_expr(merge_scope_t* s, ray_t* e, int* type, bool* column, int depth) {
    *column = false;
    if (!e || depth > 16 || ++s->nodes > MERGE_NODES) return NULL;
    if (e->type == -RAY_SYM) {
        if (!(e->attrs & ATTR_QUOTED) && ray_env_has_lexical_local(e->i64)) return NULL;
        for (int i = s->n - 1; i >= 0; i--) if (s->cols[i].name == e->i64) {
            if (++s->cols[i].uses > 1 && s->cols[i].computed) return NULL;
            if (!merge_charge(s, s->cols[i].expr, depth)) return NULL;
            *type = s->cols[i].type; *column = true;
            return merge_ref(s->cols[i].expr);
        }
        ray_t* col = ray_table_get_col(s->table, e->i64);
        if (col && !s->projected && col->type > 0 && col->type <= RAY_STR) {
            *type = col->type; *column = true;
            return merge_ref(e);
        }
        /* A quoted literal must not become a column reference after merge. */
        if (!(e->attrs & ATTR_QUOTED) || col) return NULL;
        *type = RAY_SYM;
        return merge_ref(e);
    }
    if (e->type < 0 && (merge_numeric(-e->type) || e->type == -RAY_BOOL || e->type == -RAY_STR)) {
        *type = -e->type;
        return merge_ref(e);
    }
    if (e->type != RAY_LIST || e->len != 3 || (e->attrs & ATTR_QUOTED)) return NULL;
    ray_t** es = ray_data(e), *fn = merge_fn(s, es[0]);
    if (!fn) return NULL;
    int kind = 0;
    if (fn->type == RAY_BINARY) {
        ray_binary_fn f = (ray_binary_fn)(uintptr_t)fn->i64;
        if (f == ray_add_fn || f == ray_sub_fn || f == ray_mul_fn) kind = 1;
        if (f == ray_eq_fn || f == ray_neq_fn) kind = 2;
        if (f == ray_lt_fn || f == ray_lte_fn || f == ray_gt_fn || f == ray_gte_fn) kind = 3;
    }
    if (fn->type == RAY_VARY && ((ray_vary_fn)(uintptr_t)fn->i64 == ray_and_vary_fn ||
                               (ray_vary_fn)(uintptr_t)fn->i64 == ray_or_vary_fn)) kind = 4;
    if (!kind) return NULL;
    int lt = 0, rt = 0; bool lc = false, rc = false;
    ray_t* l = merge_expr(s, es[1], &lt, &lc, depth + 1);
    ray_t* r = l ? merge_expr(s, es[2], &rt, &rc, depth + 1) : NULL;
    bool numeric = merge_numeric(lt) && merge_numeric(rt);
    bool ok = kind == 1 ? numeric : kind == 4 ? lt == RAY_BOOL && rt == RAY_BOOL :
        numeric || (lt == rt && (lt == RAY_BOOL || (kind == 2 && (lt == RAY_SYM || lt == RAY_STR))));
    ray_t* out = NULL;
    if (l && r && ok) {
        *type = kind == 1 ? (lt == RAY_F64 || rt == RAY_F64 ? RAY_F64 : RAY_I64) : RAY_BOOL;
        *column = lc || rc;
        out = merge_call(es[0], l, r);
    }
    merge_drop(l); merge_drop(r);
    return out;
}

static bool merge_has_offset(ray_t* e) {
    if (e->type != RAY_LIST) return false;
    ray_t** es = ray_data(e);
    ray_t* fn = ray_env_get(es[0]->i64);
    if (fn && fn->type == RAY_BINARY &&
        ((ray_binary_fn)(uintptr_t)fn->i64 == ray_add_fn ||
         (ray_binary_fn)(uintptr_t)fn->i64 == ray_sub_fn)) return true;
    for (int64_t i = 1; i < e->len; i++) if (merge_has_offset(es[i])) return true;
    return false;
}

static ray_t* merge_output(merge_scope_t* s, ray_t* e, int* type) {
    bool col;
    if (e && e->type == RAY_LIST && e->len == 2) {
        ray_t** es = ray_data(e), *fn = merge_fn(s, es[0]);
        if (!fn || fn->type != RAY_UNARY) return NULL;
        ray_unary_fn f = (ray_unary_fn)(uintptr_t)fn->i64;
        if (f != ray_sum_fn && f != ray_avg_fn && f != ray_min_fn && f != ray_max_fn && f != ray_count_fn)
            return NULL;
        ray_t* arg = merge_expr(s, es[1], type, &col, 0);
        if (!arg || !col || (!merge_numeric(*type) && f != ray_count_fn)) { merge_drop(arg); return NULL; }
        /* Offset lowering cannot account for nulls CREATED by F64 overflow,
         * even when the input has no HAS_NULLS flag. Integer intermediates
         * can likewise overflow into their null sentinel. */
        if (arg->type == RAY_LIST && (*type != RAY_F64 || merge_has_offset(arg))) {
            merge_drop(arg); return NULL;
        }
        ray_t* r = merge_call(es[0], arg, NULL);
        merge_drop(arg);
        if (r && RAY_IS_ERR(r)) { merge_drop(r); return NULL; }
        return r;
    }
    ray_t* r = merge_expr(s, e, type, &col, 0);
    if (!col) { merge_drop(r); return NULL; }
    return r;
}

static bool merge_identity(ray_t* e, int64_t name) {
    return e && e->type == -RAY_SYM && e->i64 == name;
}

static bool merge_mentions(ray_t* e, int64_t name) {
    if (e->type == -RAY_SYM) return e->i64 == name;
    if (e->type != RAY_LIST) return false;
    ray_t** es = ray_data(e);
    for (int64_t i = 1; i < e->len; i++) if (merge_mentions(es[i], name)) return true;
    return false;
}

/* A nonempty PREFIX proves the source is nonempty without scanning for a rare
 * or absent match. This check visits at most 64 rows, independent of table size.
 * If it cannot prove nonemptiness, leave the original query materialized. The
 * existing aggregate paths distinguish an empty source from an empty filter.
 */
static bool merge_prefix_nonempty(ray_t* table, ray_t* where) {
    int64_t nc = ray_table_ncols(table);
    ray_t* prefix = ray_table_new(nc);
    if (!prefix || RAY_IS_ERR(prefix)) { merge_drop(prefix); return false; }
    for (int64_t i = 0; i < nc; i++) {
        ray_t* c = ray_vec_slice(ray_table_get_col_idx(table, i), 0, MERGE_PREFIX_ROWS);
        if (!c || RAY_IS_ERR(c)) { merge_drop(c); merge_drop(prefix); return false; }
        prefix = ray_table_add_col(prefix, ray_table_col_name(table, i), c);
        merge_drop(c);
        if (!prefix || RAY_IS_ERR(prefix)) { merge_drop(prefix); return false; }
    }
    ray_t* d = ray_dict_new(ray_vec_new(RAY_SYM, 0), ray_list_new(0));
    if (!d || RAY_IS_ERR(d)) { merge_drop(d); merge_drop(prefix); return false; }
    d = merge_put(d, merge_ids[1], prefix);
    merge_drop(prefix);
    if (d) d = merge_put(d, merge_ids[2], where);
    if (!d) return false;
    ray_t* r = ray_select(&d, 1);
    bool nonempty = r && !RAY_IS_ERR(r) && r->type == RAY_TABLE && ray_table_nrows(r) > 0;
    merge_drop(r); merge_drop(d);
    return nonempty;
}

static ray_t* merge_pair(ray_t* outer, ray_t* inner) {
    ray_t* base = merge_get(inner, 1);
    ray_t* tbl = base;
    /* ray_env_get searches caller scopes before globals (env_lookup_flat),
     * so a shadowing lambda source supplies the rows and types proved here. */
    if (base && base->type == -RAY_SYM && !(base->attrs & ATTR_QUOTED) && !ray_sym_is_dotted(base->i64))
        tbl = ray_env_get(base->i64);
    if (!tbl || tbl->type != RAY_TABLE || ray_table_nrows(tbl) < MERGE_MIN_ROWS) return NULL;
    /* Partitioned/lazy and list columns need their own materialization proof,
     * including errors from columns an outer projection would discard. */
    for (int64_t i = 0; i < ray_table_ncols(tbl); i++) {
        ray_t* c = ray_table_get_col_idx(tbl, i);
        if (!c || c->type <= 0 || c->type > RAY_STR) return NULL;
    }
    ray_t* ik = ray_dict_keys(inner), **iv = ray_data(ray_dict_vals(inner));
    ray_t* ok = ray_dict_keys(outer), **ov = ray_data(ray_dict_vals(outer));
    for (int64_t i = 0; i < ik->len; i++) if (merge_clause(sym_cell_runtime_id(ik, i)) > 2) return NULL;
    if (merge_get(outer, 7)) return NULL;
    /* The single-predicate scalar reduction path is slower than compacting
     * first. Keep it materialized; the two-predicate and grouped wins remain. */
    if (merge_get(inner, 2) && !merge_get(outer, 2) && !merge_get(outer, 3)) {
        for (int64_t i = 0; i < ok->len; i++)
            if (!merge_clause(sym_cell_runtime_id(ok, i)) &&
                ov[i]->type == RAY_LIST && ov[i]->len == 2) return NULL;
    }
    /* Avoid introducing the AND + take early-stop regression reported in #752. */
    if (merge_get(outer, 4) && merge_get(inner, 2) && merge_get(outer, 2)) return NULL;
    merge_scope_t s = {.table = tbl};
    ray_t* where = NULL, *out = NULL, *v = NULL;
    int type; bool col;
    if (merge_get(inner, 2)) {
        where = merge_expr(&s, merge_get(inner, 2), &type, &col, 0);
        if (!where || type != RAY_BOOL || !col) goto done;
    }
    for (int64_t i = 0; i < ik->len; i++) {
        int64_t name = sym_cell_runtime_id(ik, i);
        if (merge_clause(name)) continue;
        v = merge_expr(&s, iv[i], &type, &col, 0);
        if (!v || !col || (ray_table_get_col(tbl, name) && !merge_identity(v, name))) goto done;
        s.cols[s.n++] = (merge_col_t){.name = name, .expr = v, .type = type,
                                     .computed = v->type == RAY_LIST};
        v = NULL;
    }
    s.projected = s.n > 0;
    out = ray_dict_new(ray_vec_new(RAY_SYM, 0), ray_list_new(0));
    if (!out || RAY_IS_ERR(out)) goto done;
    out = merge_put(out, merge_ids[1], base);
    if (!out) goto done;
    if (merge_get(outer, 2)) {
        v = merge_expr(&s, merge_get(outer, 2), &type, &col, 0);
        if (!v || type != RAY_BOOL || !col) goto decline;
        if (where) {
            ray_t* head = ray_sym(merge_ids[8]);
            ray_t* fn = head && !RAY_IS_ERR(head) ? merge_fn(&s, head) : NULL;
            if (!fn || fn->type != RAY_VARY || (ray_vary_fn)(uintptr_t)fn->i64 != ray_and_vary_fn) {
                merge_drop(head); goto decline;
            }
            ray_t* both = merge_call(head, where, v);
            merge_drop(head); merge_drop(where); merge_drop(v); v = NULL; where = both;
            if (!where || RAY_IS_ERR(where)) goto decline;
        } else { where = v; v = NULL; }
    }
    if (where) out = merge_put(out, merge_ids[2], where);
    if (!out) goto done;
    int64_t outputs[MERGE_COLS]; int nout = 0;
    bool aggregate = false;
    for (int64_t i = 0; i < ok->len; i++) {
        int64_t name = sym_cell_runtime_id(ok, i);
        int clause = merge_clause(name);
        if (clause == 1 || clause == 2) continue;
        if (clause == 4) {
            if (ov[i]->type != -RAY_I64 || RAY_ATOM_IS_NULL(ov[i]) || ov[i]->i64 < 0) goto decline;
            v = merge_ref(ov[i]);
        } else if (clause == 3) {
            /* Keep group-key labels: only identity keys in this first pass. */
            v = merge_expr(&s, ov[i], &type, &col, 0);
            if (!v || !col || ov[i]->type != -RAY_SYM || !merge_identity(v, ov[i]->i64)) goto decline;
        } else if (clause == 5 || clause == 6) {
            /* Sort names refer to the result, not the source. No substitution. */
            if (ov[i]->type != -RAY_SYM) goto decline;
            v = merge_ref(ov[i]);
        } else {
            v = merge_output(&s, ov[i], &type);
            if (!v || (ray_table_get_col(tbl, name) && !merge_identity(v, name))) goto decline;
            aggregate |= v->type == RAY_LIST && v->len == 2;
            if (merge_get(outer, 4) && !(v->type == RAY_LIST && v->len == 2) &&
                (type == RAY_SYM || type == RAY_STR)) goto decline;
            /* Expansion must not capture an earlier output alias. */
            for (int j = 0; j < nout; j++)
                if (merge_mentions(v, outputs[j]) || merge_mentions(ov[i], outputs[j])) goto decline;
            outputs[nout++] = name;
        }
        out = merge_put(out, name, v); merge_drop(v); v = NULL;
        if (!out) goto done;
    }
    if (!nout && s.projected) {
        if (merge_get(outer, 3)) goto decline;
        for (int i = 0; i < s.n; i++) {
            if (++s.cols[i].uses > 1 && s.cols[i].computed) goto decline;
            /* The take/filter gather and projection paths currently retain
             * different text HAS_NULLS metadata. Keep their wire result stable. */
            if (merge_get(outer, 4) && (s.cols[i].type == RAY_SYM || s.cols[i].type == RAY_STR)) goto decline;
            out = merge_put(out, s.cols[i].name, s.cols[i].expr);
            if (!out) goto done;
        }
    }
    if (!nout && !s.projected && merge_get(outer, 4)) {
        for (int64_t i = 0; i < ray_table_ncols(tbl); i++) {
            ray_t* c = ray_table_get_col_idx(tbl, i);
            if (!c || c->type == RAY_SYM || c->type == RAY_STR) goto decline;
        }
    }
    for (int clause = 5; clause <= 6; clause++) {
        ray_t* sort = merge_get(outer, clause);
        if (!sort) continue;
        bool visible = !nout && !s.projected && ray_table_get_col(tbl, sort->i64);
        for (int i = 0; i < nout; i++) visible |= outputs[i] == sort->i64;
        if (!nout) for (int i = 0; i < s.n; i++) visible |= s.cols[i].name == sort->i64;
        /* A group key is also an output even without an explicit projection. */
        ray_t* by = merge_get(outer, 3);
        visible |= by && merge_identity(by, sort->i64);
        if (!visible) goto decline;
    }
    for (int i = 0; i < s.n; i++) if (s.cols[i].computed && !s.cols[i].uses) goto decline;
    if (aggregate && merge_get(inner, 2) && !merge_prefix_nonempty(tbl, merge_get(inner, 2))) goto decline;
    goto done;
decline:
    merge_drop(out); out = NULL;
done:
    if (out && RAY_IS_ERR(out)) { merge_drop(out); out = NULL; }
    merge_drop(where); merge_drop(v);
    for (int i = 0; i < s.n; i++) merge_drop(s.cols[i].expr);
    return out;
}

static bool merge_row_shape(ray_t* e, int depth) {
    if (!e || depth > 16) return false;
    if (e->type < 0) return true;
    if (e->type != RAY_LIST || e->len != 3) return false;
    ray_t** es = ray_data(e);
    return merge_row_shape(es[1], depth + 1) && merge_row_shape(es[2], depth + 1);
}

static ray_t* merge_plan(ray_t* dict, int depth) {
    if (depth > 8 || !merge_dict_shape(dict)) return NULL;
    ray_t* from = merge_get(dict, 1);
    if (!from || from->type != RAY_LIST || from->len != 2 || (from->attrs & ATTR_QUOTED)) return NULL;
    ray_t** es = ray_data(from);
    if (!es[0] || es[0]->type != -RAY_SYM || (es[0]->attrs & ATTR_QUOTED) ||
        es[0]->i64 != merge_ids[0]) return NULL;
    if (!merge_dict_shape(es[1])) return NULL;
    /* Tiny wide projections must also skip quadratic duplicate-key checks. */
    ray_t* base = merge_get(es[1], 1), *table = base;
    if (base && base->type == -RAY_SYM && !(base->attrs & ATTR_QUOTED) && !ray_sym_is_dotted(base->i64))
        table = ray_env_get(base->i64);
    if (table && table->type == RAY_TABLE && ray_table_nrows(table) < MERGE_MIN_ROWS) return NULL;
    ray_t* fn = ray_env_get(es[0]->i64);
    if (!fn || fn->type != RAY_VARY || (ray_vary_fn)(uintptr_t)fn->i64 != ray_select ||
        !merge_dict_ok(dict) || !merge_dict_ok(es[1])) return NULL;
    /* Do not speculatively plan (and probe) an aggregate child only to throw
     * the plan away because the parent cannot cross that aggregate. */
    ray_t* keys = ray_dict_keys(es[1]), **vals = ray_data(ray_dict_vals(es[1]));
    for (int64_t i = 0; i < keys->len; i++) {
        int clause = merge_clause(sym_cell_runtime_id(keys, i));
        if (clause > 2 || (!clause && !merge_row_shape(vals[i], 0))) return NULL;
    }
    ray_t* inner = merge_plan(es[1], depth + 1);
    ray_t* out = merge_pair(dict, inner ? inner : es[1]);
    merge_drop(inner);
    return out;
}

ray_t* ray_select_merge_plan(ray_t* dict) {
    uint64_t epoch = ray_sym_epoch();
    if (merge_epoch != epoch) {
        static const char* const names[] = {"select", "from", "where", "by", "take", "asc", "desc", "nearest", "and"};
        for (int i = 0; i < 9; i++) {
            merge_ids[i] = ray_sym_intern(names[i], strlen(names[i]));
            if (merge_ids[i] < 0) return NULL;
        }
        merge_epoch = epoch;
    }
    return merge_plan(dict, 0);
}
