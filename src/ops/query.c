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

/**   Query bridge: select, update, insert, upsert, join operations.
 *   Extracted from eval.c.
 */

#include "lang/internal.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "ops/hash.h"
#include "ops/rowsel.h"
#include "ops/fused_group.h"
#include "ops/fused_topk.h"
#include "ops/temporal.h"
#include "table/sym.h"
#include "table/dict.h"
#include "mem/sys.h"

#include <string.h>
#include <math.h>
#include <inttypes.h>

/* ══════════════════════════════════════════
 * Select query — DAG bridge
 * ══════════════════════════════════════════ */

/* Helper: look up a key in a select-clause dict (RAY_DICT).
 * Returns the value expression (unevaluated), or NULL if not found. */
static ray_t* dict_get(ray_t* dict, const char* key) {
    if (!dict || dict->type != RAY_DICT) return NULL;
    int64_t key_id = ray_sym_intern(key, strlen(key));
    return ray_dict_probe_sym_borrowed(dict, key_id);
}

/* Flatten a RAY_DICT (keys SYM vec + vals LIST) into a transient
 * [k0,v0,k1,v1,...] array view so the existing dict-walking loops in
 * ray_select_fn et al. can iterate without rewriting every site.
 *
 * Caller passes stack-local buffers sized at DICT_VIEW_MAX.  If the dict
 * has more pairs than fits, sets `*out_n = -1` to flag overflow — every
 * call site checks this and returns a "domain" error rather than letting
 * the writes spill past the buffers.  The previous version of this helper
 * had no such guard and silently corrupted the stack on user-controlled
 * dicts with > 64 pairs.
 *
 * `key_atoms` must hold at least DICT_VIEW_MAX entries; `out_elems` at
 * least 2 * DICT_VIEW_MAX.  Keys are synthesized as -RAY_SYM atoms in
 * `key_atoms`; values are borrowed from the dict's vals list. */
#define DICT_VIEW_MAX 256
static void dict_pair_view(ray_t* d, ray_t* key_atoms, ray_t** out_elems, int64_t* out_n) {
    *out_n = 0;
    if (!d || d->type != RAY_DICT) return;
    ray_t* keys = ray_dict_keys(d);
    ray_t* vals = ray_dict_vals(d);
    if (!keys || keys->type != RAY_SYM || !vals || vals->type != RAY_LIST) return;
    int64_t n = keys->len;
    if (n > DICT_VIEW_MAX) { *out_n = -1; return; }
    void* kbase = ray_data(keys);
    ray_t** vptrs = (ray_t**)ray_data(vals);
    for (int64_t i = 0; i < n; i++) {
        memset(&key_atoms[i], 0, sizeof(ray_t));
        key_atoms[i].type = -RAY_SYM;
        key_atoms[i].i64  = ray_read_sym(kbase, i, RAY_SYM, keys->attrs);
        out_elems[i*2]   = &key_atoms[i];
        out_elems[i*2+1] = vptrs[i];
    }
    *out_n = 2 * n;
}

#define DICT_VIEW_DECL(name)                            \
    ray_t   name##_keybuf[DICT_VIEW_MAX];               \
    ray_t*  name[DICT_VIEW_MAX * 2];                    \
    int64_t name##_n
#define DICT_VIEW_OPEN(d, name)                          \
    dict_pair_view((d), name##_keybuf, name, &name##_n)
/* Returns true if the open exceeded DICT_VIEW_MAX — caller should
 * `ray_release(tbl); return ray_error("domain", "clause too big");`. */
#define DICT_VIEW_OVERFLOW(name) ((name##_n) < 0)

/* Convert a RAY_DICT (keys, vals) into a transient interleaved
 * [k0_atom, v0, k1_atom, v1, …] RAY_LIST.  Used by select's group-by
 * aggregation paths which were written for the old in-place pair-array
 * representation of grouping output.  Returns an owned RAY_LIST (rc=1).
 * Atom keys are freshly boxed for typed-vector key columns (sym, i64,
 * etc.); for RAY_LIST keys they are retained borrows. */
static ray_t* groups_to_pair_list(ray_t* d) {
    if (!d || d->type != RAY_DICT) return ray_error("type", NULL);
    ray_t* keys = ray_dict_keys(d);
    ray_t* vals = ray_dict_vals(d);
    int64_t n = keys ? keys->len : 0;
    ray_t* out = ray_list_new(n * 2);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    ray_t** vptrs = (vals && vals->type == RAY_LIST) ? (ray_t**)ray_data(vals) : NULL;
    for (int64_t i = 0; i < n; i++) {
        ray_t* k = NULL;
        if (!keys) {
            k = NULL;
        } else if (keys->type == RAY_LIST) {
            k = ((ray_t**)ray_data(keys))[i];
            if (k) ray_retain(k);
        } else {
            void* base = ray_data(keys);
            switch (keys->type) {
                case RAY_SYM: k = ray_sym(ray_read_sym(base, i, RAY_SYM, keys->attrs)); break;
                case RAY_I64:
                case RAY_TIMESTAMP: k = ray_i64(((int64_t*)base)[i]); break;
                case RAY_I32:
                case RAY_DATE:
                case RAY_TIME: k = ray_i32(((int32_t*)base)[i]); break;
                case RAY_I16: k = ray_i16(((int16_t*)base)[i]); break;
                case RAY_BOOL:
                case RAY_U8:  k = ray_u8(((uint8_t*)base)[i]); break;
                case RAY_F64: k = ray_f64(((double*)base)[i]); break;
                case RAY_STR: { size_t sl = 0; const char* sp = ray_str_vec_get(keys, i, &sl);
                                 k = ray_str(sp ? sp : "", sp ? sl : 0); break; }
                case RAY_GUID: k = ray_guid(((uint8_t*)base) + i * 16); break;
                default: k = NULL; break;
            }
        }
        out = ray_list_append(out, k);
        if (k) ray_release(k);
        ray_t* v = vptrs ? vptrs[i] : NULL;
        out = ray_list_append(out, v);
    }
    return out;
}

/* Map a Rayfall builtin name to a DAG binary op constructor */
typedef ray_op_t* (*dag_binary_ctor)(ray_graph_t*, ray_op_t*, ray_op_t*);
typedef ray_op_t* (*dag_unary_ctor)(ray_graph_t*, ray_op_t*);

static dag_binary_ctor resolve_binary_dag(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return NULL;
    const char* name = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    if (len == 1) {
        switch (name[0]) {
            case '+': return ray_add;
            case '-': return ray_sub;
            case '*': return ray_mul;
            case '/': return ray_div;
            case '%': return ray_mod;
            case '>': return ray_gt;
            case '<': return ray_lt;
        }
    } else if (len == 2) {
        if (name[0] == '>' && name[1] == '=') return ray_ge;
        if (name[0] == '<' && name[1] == '=') return ray_le;
        if (name[0] == '=' && name[1] == '=') return ray_eq;
        if (name[0] == '!' && name[1] == '=') return ray_ne;
        if (name[0] == 'o' && name[1] == 'r') return ray_or;
        if (name[0] == 'i' && name[1] == 'n') return ray_in;
    } else if (len == 3) {
        if (memcmp(name, "and",  3) == 0) return ray_and;
        if (memcmp(name, "div",  3) == 0) return ray_idiv;
    } else if (len == 4) {
        if (memcmp(name, "like", 4) == 0) return ray_like;
    } else if (len == 5) {
        if (memcmp(name, "ilike", 5) == 0) return ray_ilike;
    } else if (len == 6) {
        if (memcmp(name, "not-in", 6) == 0) return ray_not_in;
    }
    return NULL;
}

static dag_unary_ctor resolve_unary_dag(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return NULL;
    const char* name = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    if (len == 3) {
        if (memcmp(name, "neg", 3) == 0) return ray_neg;
        if (memcmp(name, "not", 3) == 0) return ray_not;
        if (memcmp(name, "abs", 3) == 0) return ray_abs;
        if (memcmp(name, "exp", 3) == 0) return ray_exp_op;
        if (memcmp(name, "log", 3) == 0) return ray_log_op;
    } else if (len == 4) {
        if (memcmp(name, "ceil",  4) == 0) return ray_ceil_op;
        if (memcmp(name, "sqrt",  4) == 0) return ray_sqrt_op;
        if (memcmp(name, "trim",  4) == 0) return ray_trim_op;
    } else if (len == 5) {
        if (memcmp(name, "floor", 5) == 0) return ray_floor_op;
        if (memcmp(name, "round", 5) == 0) return ray_round_op;
        if (memcmp(name, "upper", 5) == 0) return ray_upper;
        if (memcmp(name, "lower", 5) == 0) return ray_lower;
    } else if (len == 6) {
        if (memcmp(name, "strlen", 6) == 0) return ray_strlen;
    }
    /* NOTE: no DAG wiring for nil?/isnull yet.  The eval-level
     * builtin `nil?` (src/lang/eval.c:2029) is atom-only — it
     * returns false when applied to a column vec.  OP_ISNULL in
     * the DAG is per-element.  Wiring `nil?` here would diverge
     * from the eval fallback.  A proper pass should first add an
     * element-wise null-check builtin at eval level, then map it
     * here. */
    return NULL;
}

/* Map Rayfall aggregation name to DAG opcode */
static uint16_t resolve_agg_opcode(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return 0;
    const char* name = ray_str_ptr(s);
    size_t len = ray_str_len(s);
    if (len == 3 && memcmp(name, "sum",   3) == 0) return OP_SUM;
    if (len == 3 && memcmp(name, "avg",   3) == 0) return OP_AVG;
    if (len == 3 && memcmp(name, "min",   3) == 0) return OP_MIN;
    if (len == 3 && memcmp(name, "max",   3) == 0) return OP_MAX;
    if (len == 3 && memcmp(name, "dev",   3) == 0) return OP_STDDEV;
    if (len == 3 && memcmp(name, "var",   3) == 0) return OP_VAR;
    if (len == 4 && memcmp(name, "prod",  4) == 0) return OP_PROD;
    if (len == 4 && memcmp(name, "last",  4) == 0) return OP_LAST;
    if (len == 5 && memcmp(name, "count", 5) == 0) return OP_COUNT;
    if (len == 5 && memcmp(name, "first", 5) == 0) return OP_FIRST;
    if (len == 6 && memcmp(name, "stddev",6) == 0) return OP_STDDEV;
    if (len == 7 && memcmp(name, "dev_pop",      7) == 0) return OP_STDDEV_POP;
    if (len == 7 && memcmp(name, "var_pop",      7) == 0) return OP_VAR_POP;
    if (len == 10 && memcmp(name, "stddev_pop", 10) == 0) return OP_STDDEV_POP;
    return 0;
}

/* Apply sort (asc/desc) and take clauses to a materialized result table.
 * Used by eval-level paths that bypass the DAG (e.g., LIST/STR group keys).
 * Builds a temporary DAG for sorting (supports per-column direction flags)
 * and applies take via ray_head/ray_tail or ray_take_fn.
 *
 * Top-K fast path: when there is exactly one sort key (a single column
 * name), an atom take with K << nrows, and the result is a flat table
 * with no LIST columns, dispatch to ray_topk_table — bounded-heap
 * selection in O(n log K) instead of full sort + gather. */
static ray_t* apply_sort_take(ray_t* result, ray_t** dict_elems, int64_t dict_n,
                              int64_t asc_id, int64_t desc_id, int64_t take_id) {
    if (!result || RAY_IS_ERR(result)) return result;

    /* Check for sort/take clauses */
    bool has_sort = false;
    ray_t* take_val_expr = NULL;
    for (int64_t i = 0; i + 1 < dict_n; i += 2) {
        int64_t kid = dict_elems[i]->i64;
        if (kid == asc_id || kid == desc_id) has_sort = true;
        if (kid == take_id) take_val_expr = dict_elems[i + 1];
    }
    if (!has_sort && !take_val_expr) return result;

    /* ---- Top-K fast path detection ----
     * Conditions:
     *  - Exactly ONE asc:/desc: clause naming a SINGLE scalar column.
     *  - take is an atom in [1, K_MAX], where K_MAX is well under nrows.
     *  - result has no LIST columns (the topk gather handles LIST too,
     *    but skip to keep the surface area small until we have LIST
     *    test fixtures).  Most benchmark workloads are LIST-free.
     *
     * Anything else falls through to the full-sort DAG path below. */
    if (has_sort && take_val_expr && result->type == RAY_TABLE) {
        /* Collect ALL sort keys (across asc:/desc: clauses) into a flat
         * (sym, dir) list.  Single-key takes the radix-encoded fast
         * path; multi-key takes the comparator-based bounded heap. */
        enum { TOPK_MAX_KEYS = 16 };
        int64_t key_syms[TOPK_MAX_KEYS];
        uint8_t key_descs[TOPK_MAX_KEYS];
        uint8_t n_keys = 0;
        int     bad_clause = 0;
        for (int64_t i = 0; i + 1 < dict_n; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            uint8_t is_desc = 0;
            if (kid == asc_id) is_desc = 0;
            else if (kid == desc_id) is_desc = 1;
            else continue;
            ray_t* val = dict_elems[i + 1];
            if (!val) { bad_clause = 1; break; }
            if (val->type == -RAY_SYM) {
                if (n_keys >= TOPK_MAX_KEYS) { bad_clause = 1; break; }
                key_syms[n_keys] = val->i64;
                key_descs[n_keys] = is_desc;
                n_keys++;
            } else if (ray_is_vec(val) && val->type == RAY_SYM) {
                for (int64_t c = 0; c < val->len; c++) {
                    if (n_keys >= TOPK_MAX_KEYS) { bad_clause = 1; break; }
                    key_syms[n_keys] = ray_read_sym(ray_data(val), c,
                                                    val->type, val->attrs);
                    key_descs[n_keys] = is_desc;
                    n_keys++;
                }
                if (bad_clause) break;
            } else {
                /* Computed sort key (expression) — full DAG path handles it. */
                bad_clause = 1;
                break;
            }
        }
        if (!bad_clause && n_keys > 0) {
            /* Probe the take expression — only atom-K with K > 0 qualifies. */
            ray_t* tv = ray_eval(take_val_expr);
            if (tv && !RAY_IS_ERR(tv) && ray_is_atom(tv) &&
                (tv->type == -RAY_I64 || tv->type == -RAY_I32)) {
                int64_t k = (tv->type == -RAY_I64) ? tv->i64 : tv->i32;
                ray_release(tv);
                int64_t nrows = ray_table_nrows(result);
                /* Bound K and the over-cardinality ratio: only useful
                 * when K is well under nrows.  Leave the take=full /
                 * negative-take cases to the existing path. */
                if (k > 0 && k < nrows && k <= FPK_MAX_K) {
                    /* Reject LIST columns — full path handles those. */
                    int has_list = 0;
                    int64_t ncols = ray_table_ncols(result);
                    for (int64_t c = 0; c < ncols; c++) {
                        ray_t* col = ray_table_get_col_idx(result, c);
                        if (col && col->type == RAY_LIST) { has_list = 1; break; }
                    }
                    if (!has_list) {
                        ray_t* topk = NULL;
                        if (n_keys == 1) {
                            ray_t* sort_col = ray_table_get_col(result, key_syms[0]);
                            if (sort_col) {
                                topk = ray_topk_table(result, sort_col,
                                    key_descs[0], key_descs[0]
                                    /*nf=desc by default*/, k);
                            }
                        } else {
                            ray_t* key_cols[TOPK_MAX_KEYS];
                            uint8_t nfs[TOPK_MAX_KEYS];
                            int ok = 1;
                            for (uint8_t i = 0; i < n_keys; i++) {
                                key_cols[i] = ray_table_get_col(result, key_syms[i]);
                                nfs[i] = key_descs[i];
                                if (!key_cols[i]) { ok = 0; break; }
                            }
                            if (ok) {
                                topk = ray_topk_table_multi(result, key_cols,
                                    key_descs, nfs, n_keys, k);
                            }
                        }
                        if (topk && !RAY_IS_ERR(topk)) {
                            ray_release(result);
                            return topk;
                        }
                        if (topk && RAY_IS_ERR(topk)) ray_release(topk);
                        /* topk == NULL: unsupported config, fall through. */
                    }
                }
            } else if (tv) {
                ray_release(tv);
            }
        }
    }

    /* Build temporary DAG on the materialized result */
    ray_graph_t* g = ray_graph_new(result);
    if (!g) return result;
    ray_op_t* root = ray_const_table(g, result);

    /* Sort */
    if (has_sort) {
        ray_op_t* sort_keys[16];
        uint8_t   sort_descs[16];
        uint8_t   n_sort = 0;
        for (int64_t i = 0; i + 1 < dict_n && n_sort < 16; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            uint8_t is_desc = 0;
            if (kid == asc_id) is_desc = 0;
            else if (kid == desc_id) is_desc = 1;
            else continue;
            ray_t* val = dict_elems[i + 1];
            if (val->type == -RAY_SYM) {
                ray_t* s = ray_sym_str(val->i64);
                sort_keys[n_sort] = ray_scan(g, ray_str_ptr(s));
                sort_descs[n_sort] = is_desc;
                n_sort++;
            } else if (ray_is_vec(val) && val->type == RAY_SYM) {
                for (int64_t c = 0; c < val->len && n_sort < 16; c++) {
                    int64_t sid = ray_read_sym(ray_data(val), c, val->type, val->attrs);
                    ray_t* s = ray_sym_str(sid);
                    sort_keys[n_sort] = ray_scan(g, ray_str_ptr(s));
                    sort_descs[n_sort] = is_desc;
                    n_sort++;
                }
            }
        }
        if (n_sort > 0)
            root = ray_sort_op(g, root, sort_keys, sort_descs, NULL, n_sort);
    }

    /* Take: avoid the DAG ray_head/ray_tail op — it can't handle
     * tables with LIST columns (from non-agg scatter).  Use
     * ray_take_fn, but convert the atom form into a `[start amount]`
     * range so we get CLAMP semantics (group-by take),
     * not the wrap/pad behavior of atom-n take on a short table. */
    ray_t* take_range   = NULL;    /* [start amount] literal form */
    int    take_is_atom = 0;
    int64_t atom_n      = 0;
    if (take_val_expr) {
        ray_t* tv = ray_eval(take_val_expr);
        if (!tv || RAY_IS_ERR(tv)) {
            ray_graph_free(g); ray_release(result);
            return tv ? tv : ray_error("domain", NULL);
        }
        if (ray_is_atom(tv) && (tv->type == -RAY_I64 || tv->type == -RAY_I32)) {
            atom_n = (tv->type == -RAY_I64) ? tv->i64 : tv->i32;
            take_is_atom = 1;
            ray_release(tv);
        } else if (ray_is_vec(tv) && (tv->type == RAY_I64 || tv->type == RAY_I32) && tv->len == 2) {
            take_range = tv;
        } else {
            ray_release(tv); ray_graph_free(g); ray_release(result);
            return ray_error("domain", NULL);
        }
    }

    root = ray_optimize(g, root);
    ray_t* sorted = ray_execute(g, root);
    ray_graph_free(g);
    ray_release(result);

    if (take_is_atom && sorted && !RAY_IS_ERR(sorted)) {
        /* Build [start, amount] so ray_take_fn uses its range
         * branch, which clamps to the available length. */
        int64_t nrows = (sorted->type == RAY_TABLE)
                      ? ray_table_nrows(sorted)
                      : (ray_is_vec(sorted) ? sorted->len : 0);
        int64_t start, amount;
        if (atom_n >= 0) {
            start  = 0;
            amount = atom_n < nrows ? atom_n : nrows;
        } else {
            int64_t want = -atom_n;
            amount = want < nrows ? want : nrows;
            start  = nrows - amount;
        }
        ray_t* rng = ray_vec_new(RAY_I64, 2);
        if (!rng || RAY_IS_ERR(rng)) {
            ray_release(sorted);
            return rng ? rng : ray_error("oom", NULL);
        }
        ((int64_t*)ray_data(rng))[0] = start;
        ((int64_t*)ray_data(rng))[1] = amount;
        rng->len = 2;
        ray_t* sliced = ray_take_fn(sorted, rng);
        ray_release(sorted);
        ray_release(rng);
        return sliced;
    }
    if (take_range && sorted && !RAY_IS_ERR(sorted)) {
        ray_t* sliced = ray_take_fn(sorted, take_range);
        ray_release(sorted);
        ray_release(take_range);
        return sliced;
    }
    if (take_range) ray_release(take_range);
    return sorted;
}

/* --------------------------------------------------------------------------
 * Compile-time local env helpers for lambda / let inlining.
 *
 * compile_expr_dag hangs a small stack of {formal_sym_id → node_id}
 * bindings on the graph.  When the recursive walker encounters a
 * name reference, it checks the env first; if the name is bound,
 * return &g->nodes[node_id] — otherwise fall through to ray_scan.
 *
 * Store IDs, not pointers: g->nodes is a dynamically-resized array,
 * and any realloc between push and lookup would dangle stored
 * pointers.  IDs are stable across reallocs; we re-resolve
 * &g->nodes[id] on every lookup.
 *
 * Shadowing is automatic: nested lambda / let pushes appear later in
 * the stack, and cexpr_env_lookup walks top-down so the innermost
 * binding wins.  Pops are counted — never partial rewinds.
 *
 * No retain/release: op nodes live in g->nodes and are freed
 * uniformly by ray_graph_free.
 * -------------------------------------------------------------------------- */
static ray_op_t* cexpr_env_lookup(ray_graph_t* g, int64_t sym) {
    for (int i = g->cexpr_env_top - 1; i >= 0; i--)
        if (g->cexpr_env[i].sym == sym)
            return &g->nodes[g->cexpr_env[i].node_id];
    return NULL;
}

static bool cexpr_env_push(ray_graph_t* g, int64_t sym, ray_op_t* node) {
    if (g->cexpr_env_top >= 32) return false;
    g->cexpr_env[g->cexpr_env_top].sym     = sym;
    g->cexpr_env[g->cexpr_env_top].node_id = node->id;
    g->cexpr_env_top++;
    return true;
}

static void cexpr_env_pop(ray_graph_t* g, int n) {
    g->cexpr_env_top -= n;
    if (g->cexpr_env_top < 0) g->cexpr_env_top = 0;  /* defensive */
}

/* Re-resolve a ray_op_t* by its stable node ID.  Use this whenever
 * a pointer to an op node has been held across another DAG-building
 * call (which may grow g->nodes via graph_alloc_node and invalidate
 * all previously-returned pointers).  The ID is stable; only the
 * backing address may change. */

/* Compile a Rayfall AST expression into a DAG node */
ray_op_t* compile_expr_dag(ray_graph_t* g, ray_t* expr) {
    if (!expr) return NULL;

    /* Atom literal → const node.  Handle non-null scalar literals
     * via the dedicated ctors that carry just the raw value; typed
     * null atoms (e.g. `0Nl`, `0Nf`) must go through ray_const_atom
     * so the null flag in atom->nullmap rides along — otherwise
     * downstream comparisons lose the null-ness and fall back to
     * sentinel-value equality. */
    if (expr->type == -RAY_I64 && !RAY_ATOM_IS_NULL(expr))
        return ray_const_i64(g, expr->i64);
    if (expr->type == -RAY_F64 && !RAY_ATOM_IS_NULL(expr))
        return ray_const_f64(g, expr->f64);
    if (expr->type == -RAY_BOOL && !RAY_ATOM_IS_NULL(expr))
        return ray_const_bool(g, expr->b8);
    if (expr->type == -RAY_STR && !RAY_ATOM_IS_NULL(expr)) {
        const char *ptr = ray_str_ptr(expr);
        size_t len = ray_str_len(expr);
        return ray_const_str(g, ptr, len);
    }

    /* Name reference → local env first, then column scan, then
     * global env (for set-bound constants).  Local env holds lambda
     * / let bindings and takes precedence so formals shadow columns
     * naturally.  Global env is a last resort — it catches cases
     * like `(set threshold 50)` used inside a lambda body. */
    if (expr->type == -RAY_SYM && (expr->attrs & RAY_ATTR_NAME)) {
        ray_op_t* bound = cexpr_env_lookup(g, expr->i64);
        if (bound) return bound;
        ray_t* s = ray_sym_str(expr->i64);
        if (!s) return NULL;

        /* Dotted name — desugar at compile time by walking the
         * segments: emit a scan for the head column, then for each
         * subsequent segment look up the name's registered DAG-level
         * emitter and chain it.  `col.ss` → scan(col) → extract(SS),
         * `col.date` → scan(col) → date_trunc(DAY), etc.  Segment
         * resolution uses the same name table as the runtime
         * `(ss col)` form, so adding a new accessor means registering
         * one unary builtin (temporal or otherwise) — no bespoke sym
         * → field map here.  Anything we can't lower returns NULL
         * (compile error), avoiding the old crash path where unknown
         * dotted names became scans of non-existent columns. */
        if (ray_sym_is_dotted(expr->i64)) {
            const int64_t* segs;
            int nsegs = ray_sym_segs(expr->i64, &segs);
            if (nsegs < 2) return NULL;
            if (!g->table || g->table->type != RAY_TABLE) return NULL;
            if (!ray_table_get_col(g->table, segs[0])) return NULL;
            ray_t* head_name = ray_sym_str(segs[0]);
            if (!head_name) return NULL;
            ray_op_t* op = ray_scan(g, ray_str_ptr(head_name));
            if (!op) return NULL;
            for (int i = 1; i < nsegs; i++) {
                int field = ray_temporal_field_from_sym(segs[i]);
                if (field >= 0) {
                    op = ray_extract(g, op, field);
                    if (!op) return NULL;
                    continue;
                }
                int trunc_kind = ray_temporal_trunc_from_sym(segs[i]);
                if (trunc_kind >= 0) {
                    op = ray_date_trunc(g, op, trunc_kind);
                    if (!op) return NULL;
                    continue;
                }
                return NULL;
            }
            return op;
        }

        /* Column names on the bound table shadow global env —
         * matches eval-level name-resolution order. */
        if (g->table && g->table->type == RAY_TABLE &&
            ray_table_get_col(g->table, expr->i64))
            return ray_scan(g, ray_str_ptr(s));
        /* Global env: atom literals / typed vectors compile as
         * const nodes.  Lambdas only make sense as call heads
         * and are handled in the list branch below. */
        ray_t* gv = ray_env_get(expr->i64);
        if (gv) {
            if (ray_is_atom(gv)) return ray_const_atom(g, gv);
            if (ray_is_vec(gv))  return ray_const_vec(g, gv);
        }
        /* Unknown name — let ray_scan produce a column-not-found
         * error at exec time, matching prior behavior. */
        return ray_scan(g, ray_str_ptr(s));
    }

    /* Symbol literal (no RAY_ATTR_NAME) → const atom node. */
    if (expr->type == -RAY_SYM)
        return ray_const_atom(g, expr);

    /* Other atom literal types → const atom node.  Also falls
     * through to here for typed null I64/F64/BOOL/STR atoms
     * (which the fast-path branches above rejected via
     * RAY_ATOM_IS_NULL). */
    if (ray_is_atom(expr) && !(expr->attrs & RAY_ATTR_NAME))
        return ray_const_atom(g, expr);

    /* Typed-vector literal (e.g. [1 2 3], [AAPL MSFT], ["a" "b"]) →
     * const vector node.  ray_const_vec already stores any ray_t*
     * vec in ext->literal, and the OP_CONST executor returns it
     * directly — so this unlocks every typed literal vector as a
     * DAG operand (crucial for OP_IN set operands). */
    if (ray_is_vec(expr) && !(expr->attrs & RAY_ATTR_NAME))
        return ray_const_vec(g, expr);

    /* List → function call: (fn arg1 arg2 ...) */
    if (expr->type == RAY_LIST) {
        int64_t n = ray_len(expr);
        if (n == 0) return NULL;
        ray_t** elems = (ray_t**)ray_data(expr);
        ray_t* head = elems[0];

        /* Lambda invocation: `((fn [formals] body) a1 a2 …)`.
         * β-reduce at the DAG-node level — compile each actual
         * arg into its own op (in the current env), push the
         * {formal_i → actual_op_i} frame, recurse into the body
         * (which reads the env via cexpr_env_lookup when it hits
         * a name reference), then pop.  Sub-expression sharing is
         * automatic: multiple uses of a formal all resolve to the
         * single compiled actual op. */
        if (head->type == RAY_LIST) {
            int64_t hn = ray_len(head);
            if (hn != 3) return NULL;
            ray_t** hel = (ray_t**)ray_data(head);
            if (hel[0]->type != -RAY_SYM) return NULL;
            ray_t* hname_str = ray_sym_str(hel[0]->i64);
            if (!hname_str || ray_str_len(hname_str) != 2 ||
                memcmp(ray_str_ptr(hname_str), "fn", 2) != 0) return NULL;

            ray_t* formals = hel[1];
            ray_t* body    = hel[2];
            if (!ray_is_vec(formals) || formals->type != RAY_SYM) return NULL;
            int64_t nf = formals->len;
            if (n - 1 != nf) return NULL;              /* arity mismatch */
            if (nf > 16) return NULL;                  /* too many formals */
            if (g->cexpr_env_top + (int)nf > 32) return NULL; /* env overflow */

            /* Compile actuals in the CURRENT env, before pushing.
             * Snapshot IDs, not pointers — g->nodes can realloc
             * between successive compile_expr_dag calls so any
             * raw ray_op_t* saved from an earlier iteration may
             * dangle by the time we push it. */
            uint32_t actual_ids[16];
            for (int64_t i = 0; i < nf; i++) {
                ray_op_t* a = compile_expr_dag(g, elems[i + 1]);
                if (!a) return NULL;
                actual_ids[i] = a->id;
            }
            int64_t* fids = (int64_t*)ray_data(formals);
            int pushed = 0;
            for (int64_t i = 0; i < nf; i++) {
                if (g->cexpr_env_top >= 32) {
                    cexpr_env_pop(g, pushed);
                    return NULL;
                }
                g->cexpr_env[g->cexpr_env_top].sym     = fids[i];
                g->cexpr_env[g->cexpr_env_top].node_id = actual_ids[i];
                g->cexpr_env_top++;
                pushed++;
            }
            ray_op_t* result = compile_expr_dag(g, body);
            cexpr_env_pop(g, pushed);
            return result;
        }

        /* Named-lambda call: `(f a1 a2 …)` where `f` is globally
         * bound to a RAY_LAMBDA with a single-expression body.
         * Inline exactly like the literal `((fn …) …)` case.
         * Shadowing order matches the value-position name-ref
         * branch: local cexpr_env > table columns > globals.  A
         * column named `f` isn't callable, but we still must honor
         * shadowing so the exec-time error is consistent. */
        if (head->type == -RAY_SYM && (head->attrs & RAY_ATTR_NAME) &&
            cexpr_env_lookup(g, head->i64) == NULL &&
            !(g->table && g->table->type == RAY_TABLE &&
              ray_table_get_col(g->table, head->i64))) {
            ray_t* gv = ray_env_get(head->i64);
            if (gv && gv->type == RAY_LAMBDA) {
                ray_t* formals  = LAMBDA_PARAMS(gv);
                ray_t* body_lst = LAMBDA_BODY(gv);
                if (formals && body_lst && body_lst->type == RAY_LIST &&
                    ray_len(body_lst) == 1 &&
                    ray_is_vec(formals) && formals->type == RAY_SYM) {
                    int64_t nf = formals->len;
                    if (n - 1 == nf && nf <= 16 &&
                        g->cexpr_env_top + (int)nf <= 32) {
                        ray_t* body = ((ray_t**)ray_data(body_lst))[0];
                        uint32_t actual_ids[16];
                        for (int64_t i = 0; i < nf; i++) {
                            ray_op_t* a = compile_expr_dag(g, elems[i + 1]);
                            if (!a) return NULL;
                            actual_ids[i] = a->id;
                        }
                        int64_t* fids = (int64_t*)ray_data(formals);
                        int pushed = 0;
                        for (int64_t i = 0; i < nf; i++) {
                            g->cexpr_env[g->cexpr_env_top].sym     = fids[i];
                            g->cexpr_env[g->cexpr_env_top].node_id = actual_ids[i];
                            g->cexpr_env_top++;
                            pushed++;
                        }
                        ray_op_t* result = compile_expr_dag(g, body);
                        cexpr_env_pop(g, pushed);
                        return result;
                    }
                }
            }
        }

        /* Head must be a name referencing a builtin */
        if (head->type != -RAY_SYM) return NULL;
        int64_t fn_sym = head->i64;

        /* Check for xbar */
        ray_t* fn_name_str = ray_sym_str(fn_sym);
        const char* fname = fn_name_str ? ray_str_ptr(fn_name_str) : NULL;
        size_t fname_len = fn_name_str ? ray_str_len(fn_name_str) : 0;

        if (fname_len == 4 && memcmp(fname, "xbar", 4) == 0) {
            if (n != 3) return NULL;
            ray_op_t* col = compile_expr_dag(g, elems[1]);
            if (!col) return NULL;
            uint32_t col_id = col->id;
            ray_op_t* bucket = compile_expr_dag(g, elems[2]);
            if (!bucket) return NULL;
            col = &g->nodes[col_id];
            /* xbar(x, b) = x - (x % b)  (stays in integer domain) */
            ray_op_t* m = ray_mod(g, col, bucket);
            if (!m) return NULL;
            col = &g->nodes[col_id];
            return ray_sub(g, col, m);
        }

        /* (if cond then else) — 4 elements (fn + 3 args).  Compiles
         * to OP_IF which is supported by the element-wise fusion
         * pipeline. */
        if (fname_len == 2 && memcmp(fname, "if", 2) == 0) {
            if (n != 4) return NULL;
            ray_op_t* c = compile_expr_dag(g, elems[1]);
            if (!c) return NULL;
            uint32_t c_id = c->id;
            ray_op_t* t = compile_expr_dag(g, elems[2]);
            if (!t) return NULL;
            uint32_t t_id = t->id;
            ray_op_t* e = compile_expr_dag(g, elems[3]);
            if (!e) return NULL;
            c = &g->nodes[c_id];
            t = &g->nodes[t_id];
            return ray_if(g, c, t, e);
        }

        /* (substr str start len) — 4 elements. */
        if (fname_len == 6 && memcmp(fname, "substr", 6) == 0) {
            if (n != 4) return NULL;
            ray_op_t* str = compile_expr_dag(g, elems[1]);
            if (!str) return NULL;
            uint32_t str_id = str->id;
            ray_op_t* start = compile_expr_dag(g, elems[2]);
            if (!start) return NULL;
            uint32_t start_id = start->id;
            ray_op_t* ln = compile_expr_dag(g, elems[3]);
            if (!ln) return NULL;
            str = &g->nodes[str_id];
            start = &g->nodes[start_id];
            return ray_substr(g, str, start, ln);
        }

        /* (replace str from to) — 4 elements. */
        if (fname_len == 7 && memcmp(fname, "replace", 7) == 0) {
            if (n != 4) return NULL;
            ray_op_t* str = compile_expr_dag(g, elems[1]);
            if (!str) return NULL;
            uint32_t str_id = str->id;
            ray_op_t* from = compile_expr_dag(g, elems[2]);
            if (!from) return NULL;
            uint32_t from_id = from->id;
            ray_op_t* to = compile_expr_dag(g, elems[3]);
            if (!to) return NULL;
            str = &g->nodes[str_id];
            from = &g->nodes[from_id];
            return ray_replace(g, str, from, to);
        }

        /* (concat a b ...) — variadic string concat. */
        if (fname_len == 6 && memcmp(fname, "concat", 6) == 0) {
            if (n < 2 || n - 1 > 16) return NULL;
            uint32_t arg_ids[16];
            for (int64_t i = 1; i < n; i++) {
                ray_op_t* a = compile_expr_dag(g, elems[i]);
                if (!a) return NULL;
                arg_ids[i - 1] = a->id;
            }
            ray_op_t* args[16];
            for (int64_t i = 0; i < n - 1; i++)
                args[i] = &g->nodes[arg_ids[i]];
            return ray_concat(g, args, (int)(n - 1));
        }

        /* (as 'TYPE col) — cast.  The type is a sym literal like 'I64 / 'F64. */
        if (fname_len == 2 && memcmp(fname, "as", 2) == 0) {
            if (n != 3) return NULL;
            ray_t* type_expr = elems[1];
            if (type_expr->type != -RAY_SYM) return NULL;
            int8_t tgt = -1;
            ray_t* ts = ray_sym_str(type_expr->i64);
            if (ts) {
                const char* tn = ray_str_ptr(ts);
                size_t tl = ray_str_len(ts);
                if (tl == 3 && memcmp(tn, "I64", 3) == 0)       tgt = RAY_I64;
                else if (tl == 3 && memcmp(tn, "F64", 3) == 0)  tgt = RAY_F64;
                else if (tl == 3 && memcmp(tn, "I32", 3) == 0)  tgt = RAY_I32;
                else if (tl == 3 && memcmp(tn, "I16", 3) == 0)  tgt = RAY_I16;
                else if (tl == 3 && memcmp(tn, "F32", 3) == 0)  tgt = RAY_F32;
                else if (tl == 2 && memcmp(tn, "U8", 2) == 0)   tgt = RAY_U8;
                else if (tl == 4 && memcmp(tn, "BOOL", 4) == 0) tgt = RAY_BOOL;
            }
            if (tgt < 0) return NULL;
            ray_op_t* col = compile_expr_dag(g, elems[2]);
            if (!col) return NULL;
            return ray_cast(g, col, tgt);
        }

        /* Temporal extract: (year col), (month col), (day col), ... */
        if (n == 2) {
            int64_t field = -1;
            if (fname_len == 4 && memcmp(fname, "year",  4) == 0) field = RAY_EXTRACT_YEAR;
            else if (fname_len == 5 && memcmp(fname, "month", 5) == 0) field = RAY_EXTRACT_MONTH;
            else if (fname_len == 3 && memcmp(fname, "day",   3) == 0) field = RAY_EXTRACT_DAY;
            else if (fname_len == 4 && memcmp(fname, "hour",  4) == 0) field = RAY_EXTRACT_HOUR;
            else if (fname_len == 6 && memcmp(fname, "minute",6) == 0) field = RAY_EXTRACT_MINUTE;
            else if (fname_len == 6 && memcmp(fname, "second",6) == 0) field = RAY_EXTRACT_SECOND;
            else if (fname_len == 9 && memcmp(fname, "dayofweek",9) == 0) field = RAY_EXTRACT_DOW;
            else if (fname_len == 9 && memcmp(fname, "dayofyear",9) == 0) field = RAY_EXTRACT_DOY;
            if (field >= 0) {
                ray_op_t* col = compile_expr_dag(g, elems[1]);
                if (!col) return NULL;
                return ray_extract(g, col, field);
            }
        }

        /* (do e1 e2 ... en) → compile only the last expression.
         * Earlier expressions can't have side-effects in DAG context;
         * if they do, they'll be silently dropped.  Use eval-level
         * for side-effectful scripts. */
        if (fname_len == 2 && memcmp(fname, "do", 2) == 0) {
            if (n < 2) return NULL;
            return compile_expr_dag(g, elems[n - 1]);
        }

        /* (let var val body) — compile `val` in the current env,
         * bind var → val_op by ID (pointer-safe across reallocs),
         * compile `body`, pop.  Same β-reduction mechanism as
         * lambda inlining, just with a single binding. */
        if (fname_len == 3 && memcmp(fname, "let", 3) == 0) {
            if (n != 4) return NULL;
            ray_t* var_expr = elems[1];
            if (var_expr->type != -RAY_SYM) return NULL;
            int64_t var_sym = var_expr->i64;
            ray_op_t* val_op = compile_expr_dag(g, elems[2]);
            if (!val_op) return NULL;
            /* cexpr_env_push already snapshots node->id, which is
             * stable across subsequent graph reallocations. */
            if (!cexpr_env_push(g, var_sym, val_op)) return NULL;
            ray_op_t* body_op = compile_expr_dag(g, elems[3]);
            cexpr_env_pop(g, 1);
            return body_op;
        }

        /* (cond (p1 e1) (p2 e2) ... (else en)) → nested OP_IF. */
        if (fname_len == 4 && memcmp(fname, "cond", 4) == 0) {
            if (n < 2) return NULL;
            /* Walk right-to-left, building an OP_IF chain.  The last
             * clause must be an `else` form. */
            uint32_t chain_id = UINT32_MAX;
            for (int64_t i = n - 1; i >= 1; i--) {
                ray_t* clause = elems[i];
                if (clause->type != RAY_LIST || ray_len(clause) != 2) return NULL;
                ray_t** cpair = (ray_t**)ray_data(clause);
                int is_else = 0;
                if (cpair[0]->type == -RAY_SYM) {
                    ray_t* ns = ray_sym_str(cpair[0]->i64);
                    if (ns && ray_str_len(ns) == 4 &&
                        memcmp(ray_str_ptr(ns), "else", 4) == 0)
                        is_else = 1;
                }
                if (is_else) {
                    if (i != n - 1) return NULL;
                    ray_op_t* c = compile_expr_dag(g, cpair[1]);
                    if (!c) return NULL;
                    chain_id = c->id;
                } else {
                    if (chain_id == UINT32_MAX) return NULL;
                    ray_op_t* pred = compile_expr_dag(g, cpair[0]);
                    if (!pred) return NULL;
                    uint32_t pred_id = pred->id;
                    ray_op_t* body = compile_expr_dag(g, cpair[1]);
                    if (!body) return NULL;
                    pred = &g->nodes[pred_id];
                    ray_op_t* chain = &g->nodes[chain_id];
                    ray_op_t* r = ray_if(g, pred, body, chain);
                    if (!r) return NULL;
                    chain_id = r->id;
                }
            }
            if (chain_id == UINT32_MAX) return NULL;
            return &g->nodes[chain_id];
        }

        /* Variadic `and`/`or`: fold into a balanced binary tree.
         * `(and a b c d)` → `(and (and a b) (and c d))` — depth log2(N).
         * Without this, n>=4 falls through `compile_expr_dag` and the
         * caller (e.g. select WHERE) reports "WHERE predicate not
         * supported by DAG compiler".  The fused-expr executor evaluates
         * the resulting tree as a sequence of binary AND/OR instructions
         * sharing scratch registers — no extra column allocations vs
         * what hand-nested binary forms already do.
         *
         * Balanced tree (rather than left-fold) keeps the canonical
         * shape symmetric and minimises dependency-chain depth, which
         * future OoO / parallel-instruction executors can exploit. */
        if (n >= 4) {
            bool is_and = (fname_len == 3 && memcmp(fname, "and", 3) == 0);
            bool is_or  = (fname_len == 2 && memcmp(fname, "or",  2) == 0);
            if (is_and || is_or) {
                int64_t k = n - 1;
                if (k > 64) return NULL;          /* depth/space guard */
                uint32_t arg_ids[64];
                for (int64_t i = 0; i < k; i++) {
                    ray_op_t* a = compile_expr_dag(g, elems[i + 1]);
                    if (!a) return NULL;
                    arg_ids[i] = a->id;
                }
                dag_binary_ctor ctor = is_and ? ray_and : ray_or;
                /* Iterative pairwise reduction: at each round, fold
                 * adjacent pairs into a single node, halving the count.
                 * Equivalent to recursive bisect but avoids a helper. */
                int64_t cnt = k;
                while (cnt > 1) {
                    int64_t out = 0;
                    for (int64_t i = 0; i + 1 < cnt; i += 2) {
                        /* make_binary re-resolves both inputs via stored
                         * IDs after its own potential realloc, so the
                         * pointers we pass here are safe to use. */
                        ray_op_t* l = &g->nodes[arg_ids[i]];
                        ray_op_t* r = &g->nodes[arg_ids[i + 1]];
                        ray_op_t* combined = ctor(g, l, r);
                        if (!combined) return NULL;
                        arg_ids[out++] = combined->id;
                    }
                    if (cnt & 1)                    /* carry odd tail */
                        arg_ids[out++] = arg_ids[cnt - 1];
                    cnt = out;
                }
                return &g->nodes[arg_ids[0]];
            }
        }

        /* Binary op? */
        if (n == 3) {
            dag_binary_ctor ctor = resolve_binary_dag(fn_sym);
            if (ctor) {
                ray_op_t* left = compile_expr_dag(g, elems[1]);
                if (!left) return NULL;
                uint32_t left_id = left->id;
                ray_op_t* right = compile_expr_dag(g, elems[2]);
                if (!right) return NULL;
                left = &g->nodes[left_id];
                return ctor(g, left, right);
            }
        }

        /* Unary op or aggregation? */
        if (n == 2) {
            /* Check for unary DAG ops */
            dag_unary_ctor uctor = resolve_unary_dag(fn_sym);
            if (uctor) {
                ray_op_t* arg = compile_expr_dag(g, elems[1]);
                return arg ? uctor(g, arg) : NULL;
            }
            /* Aggregation functions return DAG agg nodes */
            uint16_t agg_op = resolve_agg_opcode(fn_sym);
            if (agg_op) {
                ray_op_t* arg = compile_expr_dag(g, elems[1]);
                if (!arg) return NULL;
                switch (agg_op) {
                    case OP_SUM:         return ray_sum(g, arg);
                    case OP_AVG:         return ray_avg(g, arg);
                    case OP_MIN:         return ray_min_op(g, arg);
                    case OP_MAX:         return ray_max_op(g, arg);
                    case OP_COUNT:       return ray_count(g, arg);
                    case OP_FIRST:       return ray_first(g, arg);
                    case OP_LAST:        return ray_last(g, arg);
                    case OP_PROD:        return ray_prod(g, arg);
                    case OP_STDDEV:      return ray_stddev(g, arg);
                    case OP_STDDEV_POP:  return ray_stddev_pop(g, arg);
                    case OP_VAR:         return ray_var(g, arg);
                    case OP_VAR_POP:     return ray_var_pop(g, arg);
                    default: return NULL;
                }
            }
        }
    }

    return NULL;
}

/* Walk an expression tree and bind any name-symbols that match table columns
 * into the current local scope. Recurses into list sub-expressions. */
static void expr_bind_table_names(ray_t* expr, ray_t* tbl) {
    if (!expr) return;
    if (expr->type == -RAY_SYM && (expr->attrs & RAY_ATTR_NAME)) {
        /* Plain column reference — bind the column into local scope. */
        ray_t* col = ray_table_get_col(tbl, expr->i64);
        if (col) { ray_env_set_local(expr->i64, col); return; }
        /* Dotted reference (e.g. `Timestamp.ss`) — the whole dotted
         * sym isn't a column name, but its HEAD segment might be.
         * Bind the head so ray_env_resolve's dotted walk can reach
         * it when ray_eval fires on this expression.  Non-column
         * heads (globals, locals) fall through to env_resolve's
         * normal scope-chain lookup. */
        if (ray_sym_is_dotted(expr->i64)) {
            const int64_t* segs;
            int nsegs = ray_sym_segs(expr->i64, &segs);
            if (nsegs >= 1) {
                ray_t* head_col = ray_table_get_col(tbl, segs[0]);
                if (head_col) ray_env_set_local(segs[0], head_col);
            }
        }
        return;
    }
    if (expr->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(expr);
        int64_t n = ray_len(expr);
        for (int64_t i = 0; i < n; i++)
            expr_bind_table_names(elems[i], tbl);
    }
}

static int is_agg_expr(ray_t* expr);  /* defined below */

/* Return 1 if expr references a table column in a position where the
 * column is expected to flow through row-by-row (not reduced by an
 * enclosing aggregation).  Used to decide whether a non-agg expression
 * is expected to produce a row-aligned result — pure constants and
 * aggregation-reduced expressions (e.g. `(+ 1 (sum p))`) legitimately
 * produce scalars/short-length results that must be broadcast.
 *
 * The walker stops recursing when it hits an aggregation call: any
 * column refs inside get reduced to a scalar, so they don't drive the
 * row-alignment expectation.
 *
 * Lambda call forms `((fn ...) actuals)` are also treated as
 * "unknown shape" — even if the actuals reference columns, the
 * body may reduce them via an enclosed aggregation.  Returning 0
 * here means the scatter will rely purely on the runtime shape
 * check (row-aligned → gather, else broadcast) instead of
 * erroring.  This loses a bug-catching net for lambda calls whose
 * body IS row-preserving but returns a mismatched-length result,
 * but that's a niche case compared to the common "lambda wrapping
 * an agg" pattern users actually write. */
static int expr_refs_row_column(ray_t* expr, ray_t* tbl) {
    if (!expr) return 0;
    if (expr->type == -RAY_SYM && (expr->attrs & RAY_ATTR_NAME)) {
        if (ray_table_get_col(tbl, expr->i64)) return 1;
        /* Dotted name whose head is a column is a row-aligned ref —
         * `Timestamp.ss` flows through row-by-row the same as plain
         * `Timestamp` would, so the scatter must treat it as one. */
        if (ray_sym_is_dotted(expr->i64)) {
            const int64_t* segs;
            int nsegs = ray_sym_segs(expr->i64, &segs);
            if (nsegs >= 1 && ray_table_get_col(tbl, segs[0])) return 1;
        }
        return 0;
    }
    if (expr->type == RAY_LIST) {
        /* If this call is itself an aggregation, its column refs
         * collapse to a scalar — don't recurse.  The whole subtree
         * is treated as a constant from the row-alignment POV. */
        if (is_agg_expr(expr)) return 0;
        ray_t** elems = (ray_t**)ray_data(expr);
        int64_t n = ray_len(expr);
        if (n == 0) return 0;
        /* Lambda call form: head is itself a LIST.  We can't tell
         * from the outside whether the body is row-preserving or
         * aggregating, so surrender row-alignment enforcement. */
        if (elems[0]->type == RAY_LIST) return 0;
        /* Skip elems[0] — it's the function name, not a column. */
        for (int64_t i = 1; i < n; i++)
            if (expr_refs_row_column(elems[i], tbl)) return 1;
    }
    return 0;
}

/* Check if an expression is an aggregation call (head is an agg function) */
static int is_agg_expr(ray_t* expr) {
    if (!expr || expr->type != RAY_LIST) return 0;
    if (expr->type == RAY_DICT) return 0;
    int64_t n = ray_len(expr);
    if (n < 2) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (elems[0]->type != -RAY_SYM) return 0;
    return resolve_agg_opcode(elems[0]->i64) != 0;
}

static int expr_contains_call_named(ray_t* expr, const char* name, size_t name_len) {
    if (!expr) return 0;
    if (expr->type != RAY_LIST) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    int64_t n = ray_len(expr);
    if (n <= 0) return 0;
    ray_t* head = elems[0];
    if (head && head->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(head->i64);
        if (s && ray_str_len(s) == name_len &&
            memcmp(ray_str_ptr(s), name, name_len) == 0)
            return 1;
    }
    for (int64_t i = 0; i < n; i++)
        if (expr_contains_call_named(elems[i], name, name_len))
            return 1;
    return 0;
}

/* True when a grouped aggregate expression can be lowered to OP_GROUP.
 * `(count (distinct col))` is semantically an aggregate, but `distinct`
 * is not a row-aligned DAG input inside GROUP.  Route it through the
 * per-group eval fallback so `distinct` sees each group's slice. */
static int is_group_dag_agg_expr(ray_t* expr) {
    if (!is_agg_expr(expr)) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    return !expr_contains_call_named(elems[1], "distinct", 8);
}

/* True for `(fn arg ...)` where fn resolves to a RAY_UNARY marked
 * RAY_FN_AGGR — i.e. a builtin aggregator (sum/avg/min/max/count and
 * the non-whitelisted med/dev/var/stddev/etc).  Used to route these
 * through the streaming-style per-group AGG branch rather than the
 * full ray_eval per-group fallback.  This is a SUPERSET of is_agg_expr:
 * it includes everything resolve_agg_opcode names plus the AGGR
 * builtins that lack a streaming-engine opcode. */
static int is_aggr_unary_call(ray_t* expr) {
    if (!expr || expr->type != RAY_LIST) return 0;
    int64_t n = ray_len(expr);
    if (n < 2) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (elems[0]->type != -RAY_SYM) return 0;
    ray_t* fn_obj = ray_env_get(elems[0]->i64);
    if (!fn_obj || fn_obj->type != RAY_UNARY) return 0;
    return (fn_obj->attrs & RAY_FN_AGGR) != 0;
}

static int is_streaming_aggr_unary_call(ray_t* expr) {
    if (!is_aggr_unary_call(expr)) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    return !expr_contains_call_named(elems[1], "distinct", 8);
}

/* Detect `(count (distinct <inner>))` exactly — the only shape that
 * routes through the OP_COUNT_DISTINCT fast path per group.  Returns
 * the inner expression on success, NULL otherwise.  More complex
 * forms like `(count (distinct (+ col 1)))` are accepted; the inner
 * expr is full-table-evaluable.  Anything where the outer call is
 * not a plain `(count …)` or the inner is not a plain `(distinct …)`
 * is rejected so the eval fallback handles it. */
static ray_t* match_count_distinct(ray_t* expr) {
    if (!expr || expr->type != RAY_LIST) return NULL;
    int64_t n = ray_len(expr);
    if (n != 2) return NULL;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (!elems[0] || elems[0]->type != -RAY_SYM) return NULL;
    ray_t* nm = ray_sym_str(elems[0]->i64);
    if (!nm || ray_str_len(nm) != 5 ||
        memcmp(ray_str_ptr(nm), "count", 5) != 0) return NULL;

    ray_t* inner = elems[1];
    if (!inner || inner->type != RAY_LIST) return NULL;
    int64_t in_n = ray_len(inner);
    if (in_n != 2) return NULL;
    ray_t** in_elems = (ray_t**)ray_data(inner);
    if (!in_elems[0] || in_elems[0]->type != -RAY_SYM) return NULL;
    ray_t* dnm = ray_sym_str(in_elems[0]->i64);
    if (!dnm || ray_str_len(dnm) != 8 ||
        memcmp(ray_str_ptr(dnm), "distinct", 8) != 0) return NULL;
    return in_elems[1];
}

/* Walk expr once, gather unique column-ref symbol ids that resolve to
 * columns of `tbl`.  Dotted refs (`Timestamp.ss`) record the head
 * segment.  Caps at `max_out` entries (16 is plenty for s: clauses);
 * returns the count gathered.  Used by the per-group fallback to slice
 * each ref exactly once per group instead of re-walking the AST. */
static int collect_col_refs(ray_t* expr, ray_t* tbl,
                            int64_t* out_syms, int max_out, int n) {
    if (!expr || n >= max_out) return n;
    if (expr->type == -RAY_SYM && (expr->attrs & RAY_ATTR_NAME)) {
        int64_t want = -1;
        if (ray_table_get_col(tbl, expr->i64)) {
            want = expr->i64;
        } else if (ray_sym_is_dotted(expr->i64)) {
            const int64_t* segs;
            int nsegs = ray_sym_segs(expr->i64, &segs);
            if (nsegs >= 1 && ray_table_get_col(tbl, segs[0])) want = segs[0];
        }
        if (want >= 0) {
            for (int i = 0; i < n; i++) if (out_syms[i] == want) return n;
            if (n < max_out) out_syms[n++] = want;
        }
        return n;
    }
    if (expr->type == RAY_LIST) {
        ray_t** elems = (ray_t**)ray_data(expr);
        int64_t cnt = ray_len(expr);
        for (int64_t i = 0; i < cnt && n < max_out; i++)
            n = collect_col_refs(elems[i], tbl, out_syms, max_out, n);
    }
    return n;
}

/* Bind a single column-id to a slice of its column under `idx_list`.
 * Helper used inside the per-group hot loop (slices the table's column
 * via ray_at_fn, hands the slice to env_bind_local which retains, then
 * drops our ref).  Returns 0 on success, error ray_t* on failure. */
static ray_t* bind_col_slice(int64_t sym, ray_t* col, ray_t* idx_list) {
    ray_t* slice = ray_at_fn(col, idx_list);
    if (!slice || RAY_IS_ERR(slice)) {
        return slice ? slice : ray_error("oom", NULL);
    }
    ray_env_set_local(sym, slice);
    ray_release(slice);
    return NULL;
}

/* Convert a partly-filled typed vec (indices 0..fill-1 valid) back into
 * a LIST of n_groups owned atom refs (only first `fill` initialized).
 * Used by the per-group eval fallback when the probe-typed-direct path
 * detects a mid-loop type mismatch and has to demote to a list. */
static ray_t* typed_vec_to_list(ray_t* tv, int64_t fill, int64_t n_groups) {
    ray_t* list_col = ray_alloc(n_groups * sizeof(ray_t*));
    if (!list_col) return ray_error("oom", NULL);
    list_col->type = RAY_LIST;
    list_col->len = 0;
    ray_t** out = (ray_t**)ray_data(list_col);
    for (int64_t k = 0; k < fill; k++) {
        int allocated = 0;
        ray_t* atom = collection_elem(tv, k, &allocated);
        if (!allocated && atom) ray_retain(atom);
        out[k] = atom;
        list_col->len = k + 1;
    }
    return list_col;
}

/* Inner per-group eval body shared by the LIST-`groups` and `idx_buf`
 * variants.  Pre-collects unique column refs, pushes ONE local scope
 * around the whole loop, and probes the first cell:
 *   - scalar atom of a typed-vec primitive → write directly into a
 *     pre-allocated typed vec (no list intermediate, no post-collapse);
 *   - otherwise → collect into a LIST column.
 * If the typed-direct path hits a mid-loop type mismatch, it demotes
 * to a LIST cleanly (one-time cost).  `feeder` produces the per-group
 * idx_list ray_t* (caller controls its lifetime / reuse); the closure
 * over `feeder_state` lets the buf variant reuse a single I64 wrapper.
 *
 * Returns either a typed vec (homogeneous scalars) or a LIST col. */
typedef ray_t* (*idx_feeder_fn)(int64_t gi, void* state);

static ray_t* nonagg_eval_per_group_core(ray_t* expr, ray_t* tbl,
                                         idx_feeder_fn feeder, void* fstate,
                                         int64_t n_groups) {
    int64_t col_syms[16];
    int n_cols = collect_col_refs(expr, tbl, col_syms, 16, 0);
    ray_t* cols[16];
    for (int i = 0; i < n_cols; i++)
        cols[i] = ray_table_get_col(tbl, col_syms[i]);

    if (ray_env_push_scope() != RAY_OK) return ray_error("oom", NULL);

    ray_t* result = NULL;       /* typed vec OR list col */
    int direct_typed = 0;       /* non-zero → result is a typed vec */
    int8_t typed_t = 0;         /* atom type sentinel for the typed path */

    for (int64_t gi = 0; gi < n_groups; gi++) {
        ray_t* idx_list = feeder(gi, fstate);
        if (!idx_list) {
            ray_env_pop_scope();
            if (result) ray_release(result);
            return ray_error("oom", NULL);
        }
        for (int i = 0; i < n_cols; i++) {
            ray_t* err = bind_col_slice(col_syms[i], cols[i], idx_list);
            if (err) {
                ray_env_pop_scope();
                if (result) ray_release(result);
                return err;
            }
        }
        ray_t* cell = ray_eval(expr);
        if (!cell || RAY_IS_ERR(cell)) {
            ray_env_pop_scope();
            if (result) ray_release(result);
            return cell ? cell : ray_error("domain", NULL);
        }

        if (gi == 0) {
            int8_t t = cell->type;
            int collapsable = (t < 0 && t != -RAY_SYM && t != -RAY_STR && t != -RAY_GUID);
            if (collapsable) {
                int8_t vt = (int8_t)(-t);
                result = ray_vec_new(vt, n_groups);
                if (!result || RAY_IS_ERR(result)) {
                    ray_env_pop_scope(); ray_release(cell);
                    return result ? result : ray_error("oom", NULL);
                }
                result->len = n_groups;
                if (store_typed_elem(result, 0, cell) == 0) {
                    direct_typed = 1; typed_t = t;
                    ray_release(cell);
                } else {
                    /* type unsupported by store_typed_elem → fall to list */
                    ray_release(result); result = NULL;
                    collapsable = 0;
                }
            }
            if (!collapsable) {
                result = ray_alloc(n_groups * sizeof(ray_t*));
                if (!result) {
                    ray_env_pop_scope(); ray_release(cell);
                    return ray_error("oom", NULL);
                }
                result->type = RAY_LIST;
                result->len = 0;
                ((ray_t**)ray_data(result))[0] = cell;
                result->len = 1;
            }
            continue;
        }

        if (direct_typed) {
            if (cell->type == typed_t && store_typed_elem(result, gi, cell) == 0) {
                ray_release(cell);
            } else {
                /* Demote: convert typed vec [0..gi-1] to list, append cell, continue as list. */
                ray_t* list_col = typed_vec_to_list(result, gi, n_groups);
                ray_release(result);
                if (RAY_IS_ERR(list_col)) {
                    ray_env_pop_scope(); ray_release(cell);
                    return list_col;
                }
                result = list_col;
                ((ray_t**)ray_data(result))[gi] = cell;  /* takes ownership */
                result->len = gi + 1;
                direct_typed = 0;
            }
        } else {
            ((ray_t**)ray_data(result))[gi] = cell;  /* takes ownership */
            result->len = gi + 1;
        }
    }

    ray_env_pop_scope();
    return result;
}

/* idx_feeder for the eval-fallback's LIST `groups` layout. */
typedef struct { ray_t** items; } groups_state_t;
static ray_t* groups_idx_feed(int64_t gi, void* st) {
    groups_state_t* s = (groups_state_t*)st;
    return s->items[gi * 2 + 1];
}

static ray_t* nonagg_eval_per_group(ray_t* expr, ray_t* tbl,
                                    ray_t* groups, int64_t n_groups) {
    groups_state_t st = { .items = (ray_t**)ray_data(groups) };
    return nonagg_eval_per_group_core(expr, tbl, groups_idx_feed, &st, n_groups);
}

/* idx_feeder for the DAG fast-path's idx_buf+offsets+grp_cnt layout.
 * Reuses a single RAY_I64 wrapper across all groups: just retargets the
 * data pointer-equivalent by memcpy'ing into its data area and adjusting
 * `len`.  Saves n_groups vec allocs/frees. */
typedef struct {
    const int64_t* idx_buf;
    const int64_t* offsets;
    const int64_t* grp_cnt;
    ray_t*         scratch;     /* RAY_I64 vec, sized to max grp_cnt */
} buf_state_t;

static ray_t* buf_idx_feed(int64_t gi, void* st) {
    buf_state_t* s = (buf_state_t*)st;
    int64_t cnt = s->grp_cnt[gi];
    s->scratch->len = cnt;
    if (cnt > 0) {
        memcpy(ray_data(s->scratch), &s->idx_buf[s->offsets[gi]],
               (size_t)cnt * sizeof(int64_t));
    }
    return s->scratch;
}

static ray_t* nonagg_eval_per_group_buf(ray_t* expr, ray_t* tbl,
                                        const int64_t* idx_buf,
                                        const int64_t* offsets,
                                        const int64_t* grp_cnt,
                                        int64_t n_groups) {
    int64_t max_cnt = 0;
    for (int64_t gi = 0; gi < n_groups; gi++)
        if (grp_cnt[gi] > max_cnt) max_cnt = grp_cnt[gi];
    ray_t* scratch = ray_vec_new(RAY_I64, max_cnt > 0 ? max_cnt : 1);
    if (!scratch || RAY_IS_ERR(scratch))
        return scratch ? scratch : ray_error("oom", NULL);
    buf_state_t st = { idx_buf, offsets, grp_cnt, scratch };
    ray_t* res = nonagg_eval_per_group_core(expr, tbl, buf_idx_feed, &st, n_groups);
    ray_release(scratch);
    return res;
}

/* Streaming-style per-group AGG body, DAG flavor.  For an expression
 * like `(med v)` (head is RAY_FN_AGGR + RAY_UNARY, second elem is a
 * column ref or full-table-eval-able sub-expression), slice src per
 * group via ray_at_fn, call the unary fn directly, store the scalar
 * result into a pre-sized typed vec.  Mirrors the eval-fallback's AGG
 * branch (`query.c:~1955`) but with the idx_buf+offsets+grp_cnt
 * layout the DAG path produces. */
static ray_t* aggr_unary_per_group_buf(ray_t* expr, ray_t* tbl,
                                       const int64_t* idx_buf,
                                       const int64_t* offsets,
                                       const int64_t* grp_cnt,
                                       int64_t n_groups) {
    ray_t** elems = (ray_t**)ray_data(expr);
    ray_t* fn_name = elems[0];
    ray_t* col_expr = elems[1];

    ray_t* fn_obj = ray_env_get(fn_name->i64);
    if (!fn_obj || fn_obj->type != RAY_UNARY)
        return ray_error("type", NULL);
    ray_unary_fn uf = (ray_unary_fn)(uintptr_t)fn_obj->i64;

    /* Resolve the source column: either a direct column ref (no copy)
     * or a full-table eval of the sub-expression. */
    ray_t* src = NULL;
    if (col_expr->type == -RAY_SYM && (col_expr->attrs & RAY_ATTR_NAME)) {
        src = ray_table_get_col(tbl, col_expr->i64);
        if (src) ray_retain(src);
    }
    if (!src) {
        /* Bind table cols and eval — same pattern as the existing path. */
        if (ray_env_push_scope() != RAY_OK) return ray_error("oom", NULL);
        expr_bind_table_names(col_expr, tbl);
        src = ray_eval(col_expr);
        ray_env_pop_scope();
        if (!src || RAY_IS_ERR(src)) return src ? src : ray_error("domain", NULL);
    }

    /* Reusable I64 idx wrapper. */
    int64_t max_cnt = 0;
    for (int64_t gi = 0; gi < n_groups; gi++)
        if (grp_cnt[gi] > max_cnt) max_cnt = grp_cnt[gi];
    ray_t* idx_vec = ray_vec_new(RAY_I64, max_cnt > 0 ? max_cnt : 1);
    if (!idx_vec || RAY_IS_ERR(idx_vec)) {
        ray_release(src);
        return idx_vec ? idx_vec : ray_error("oom", NULL);
    }

    ray_t* agg_vec = NULL;
    int8_t agg_atom_t = 0;

    for (int64_t gi = 0; gi < n_groups; gi++) {
        idx_vec->len = grp_cnt[gi];
        if (grp_cnt[gi] > 0) {
            memcpy(ray_data(idx_vec), &idx_buf[offsets[gi]],
                   (size_t)grp_cnt[gi] * sizeof(int64_t));
        }
        ray_t* subset = ray_at_fn(src, idx_vec);
        if (!subset || RAY_IS_ERR(subset)) continue;
        ray_t* agg_val = uf(subset);
        ray_release(subset);
        if (!agg_val || RAY_IS_ERR(agg_val)) continue;

        if (!agg_vec) {
            agg_atom_t = agg_val->type;
            int8_t vt = (int8_t)(-agg_atom_t);
            agg_vec = ray_vec_new(vt, n_groups);
            if (!agg_vec || RAY_IS_ERR(agg_vec)) {
                ray_release(agg_val); ray_release(idx_vec); ray_release(src);
                return agg_vec ? agg_vec : ray_error("oom", NULL);
            }
            agg_vec->len = n_groups;
        }
        if (agg_val->type != agg_atom_t || store_typed_elem(agg_vec, gi, agg_val) != 0) {
            /* Fallback: shouldn't happen for well-behaved aggregators; if it
             * does, demote to a list so we don't return a partly-typed vec.
             * Convert what we have so far to a list, then reattempt as a
             * generic non-streaming eval. */
            ray_release(agg_val);
            ray_release(idx_vec); ray_release(src);
            ray_release(agg_vec);
            return nonagg_eval_per_group_buf(expr, tbl, idx_buf, offsets, grp_cnt, n_groups);
        }
        ray_release(agg_val);
    }

    ray_release(idx_vec); ray_release(src);
    if (!agg_vec) {
        /* No groups produced a value (all empty?) — return an empty typed
         * vec sized n_groups; default to I64 for lack of a better guess. */
        agg_vec = ray_vec_new(RAY_I64, n_groups);
        if (agg_vec && !RAY_IS_ERR(agg_vec)) agg_vec->len = n_groups;
    }
    return agg_vec;
}

/* Per-group count(distinct) parallel kernel — one task per group, each
 * task does its own dedup with a scratch hash table.  Skips the
 * gather_by_idx + exec_count_distinct allocation that the serial path
 * pays per group.
 *
 * Caller resolves `src` (typed vector with per-row layout matching
 * idx_buf rows).  Worker tasks read via idx_buf[offsets[gi]+i] directly,
 * no intermediate compaction.
 *
 * Specialised on element width (1/2/4/8 bytes + F64) so the inner read
 * folds to a typed pointer dereference.  Has-nulls falls through to
 * ray_count_distinct_per_group_buf serial path (acceptable: null-bearing
 * columns are rare in ClickBench-style aggregates). */
typedef struct {
    int8_t          in_type;
    uint8_t         in_attrs;
    const void*     base;
    bool            has_nulls;
    const uint8_t*  null_bm;
    uint8_t         esz;        /* 1/2/4/8 */
    bool            is_f64;
    const int64_t*  idx_buf;
    const int64_t*  offsets;
    const int64_t*  grp_cnt;
    int64_t*        odata;
    _Atomic(int32_t) oom;
} cdpg_buf_par_ctx_t;

#define CDPG_BUF_HASH_K1 0x9E3779B97F4A7C15ULL

/* DuckDB-style single-array HT: empty slot is encoded as 0; the
 * (extremely rare) v == 0 input goes into a separate saw_zero flag.
 * Compared to the previous (set + used) two-array layout this halves
 * the memory footprint per slot (8 B vs 9 B) and — more importantly —
 * collapses two cache-line accesses (used byte + set int64) into a
 * single int64 read on the hot path.  Hits Q9-class queries where the
 * per-group HT churn was thrashing L2. */
#define CDPG_BUF_INSERT(VAL_EXPR) do {                              \
    int64_t v = (int64_t)(VAL_EXPR);                                \
    if (RAY_UNLIKELY(v == 0)) {                                     \
        if (!saw_zero) { saw_zero = 1; distinct++; }                \
        break;                                                      \
    }                                                               \
    uint64_t h = (uint64_t)v * CDPG_BUF_HASH_K1;                    \
    h ^= h >> 33;                                                   \
    uint64_t slot = h & mask;                                       \
    for (;;) {                                                      \
        int64_t cur = set[slot];                                    \
        if (cur == 0) {                                             \
            set[slot] = v;                                          \
            distinct++;                                             \
            break;                                                  \
        }                                                           \
        if (cur == v) break;                                        \
        slot = (slot + 1) & mask;                                   \
    }                                                               \
} while (0)

static void cdpg_buf_par_fn(void* vctx, uint32_t worker_id,
                            int64_t start, int64_t end) {
    (void)worker_id;
    cdpg_buf_par_ctx_t* ctx = (cdpg_buf_par_ctx_t*)vctx;
    if (atomic_load_explicit(&ctx->oom, memory_order_relaxed)) return;

    for (int64_t gi = start; gi < end; gi++) {
        int64_t cnt = ctx->grp_cnt[gi];
        if (cnt == 0) { ctx->odata[gi] = 0; continue; }
        const int64_t* idxs = &ctx->idx_buf[ctx->offsets[gi]];

        uint64_t cap = (uint64_t)cnt * 2;
        if (cap < 32) cap = 32;
        uint64_t c = 1;
        while (c && c < cap) c <<= 1;
        if (!c) {
            atomic_store_explicit(&ctx->oom, 1, memory_order_relaxed);
            return;
        }
        cap = c;
        uint64_t mask = cap - 1;

        ray_t* set_hdr  = NULL;
        int64_t* set    = (int64_t*)scratch_calloc(&set_hdr,
                                                   (size_t)cap * sizeof(int64_t));
        if (!set) {
            atomic_store_explicit(&ctx->oom, 1, memory_order_relaxed);
            return;
        }

        int64_t distinct = 0;
        int saw_zero = 0;
        const uint8_t* null_bm = ctx->null_bm;
        bool has_nulls = ctx->has_nulls;

        if (ctx->is_f64) {
            const double* d = (const double*)ctx->base;
            for (int64_t i = 0; i < cnt; i++) {
                int64_t r = idxs[i];
                if (has_nulls && null_bm && ((null_bm[r/8] >> (r%8)) & 1)) continue;
                double fv = d[r];
                if (fv != fv) fv = (double)NAN;
                else if (fv == 0.0) fv = 0.0;
                int64_t vbits = 0;
                memcpy(&vbits, &fv, sizeof(int64_t));
                CDPG_BUF_INSERT(vbits);
            }
        } else if (ctx->esz == 8) {
            const int64_t* d = (const int64_t*)ctx->base;
            if (has_nulls && null_bm) {
                for (int64_t i = 0; i < cnt; i++) {
                    int64_t r = idxs[i];
                    if ((null_bm[r/8] >> (r%8)) & 1) continue;
                    CDPG_BUF_INSERT(d[r]);
                }
            } else {
                for (int64_t i = 0; i < cnt; i++) {
                    CDPG_BUF_INSERT(d[idxs[i]]);
                }
            }
        } else if (ctx->esz == 4) {
            const int32_t* d = (const int32_t*)ctx->base;
            if (has_nulls && null_bm) {
                for (int64_t i = 0; i < cnt; i++) {
                    int64_t r = idxs[i];
                    if ((null_bm[r/8] >> (r%8)) & 1) continue;
                    CDPG_BUF_INSERT((int64_t)d[r]);
                }
            } else {
                for (int64_t i = 0; i < cnt; i++) {
                    CDPG_BUF_INSERT((int64_t)d[idxs[i]]);
                }
            }
        } else if (ctx->esz == 2) {
            const int16_t* d = (const int16_t*)ctx->base;
            for (int64_t i = 0; i < cnt; i++) {
                int64_t r = idxs[i];
                if (has_nulls && null_bm && ((null_bm[r/8] >> (r%8)) & 1)) continue;
                CDPG_BUF_INSERT((int64_t)d[r]);
            }
        } else { /* esz == 1 */
            const uint8_t* d = (const uint8_t*)ctx->base;
            for (int64_t i = 0; i < cnt; i++) {
                int64_t r = idxs[i];
                if (has_nulls && null_bm && ((null_bm[r/8] >> (r%8)) & 1)) continue;
                CDPG_BUF_INSERT((int64_t)d[r]);
            }
        }

        scratch_free(set_hdr);
        ctx->odata[gi] = distinct;
    }
}
#undef CDPG_BUF_INSERT

/* Parallel idx_buf construction from row_gid.  Builds the
 * groupwise inverted index used by per-group-slice consumers
 * (count_distinct_per_group_buf, nonagg_eval_per_group_buf, etc.)
 *
 * Two passes:
 *   Pass 1 (cnt_fn): per-task histograms of row_gid → grp_cnt buckets
 *                    (kept in task-local rows so no atomics needed).
 *   Pass 2 (scat_fn): per-task scatter into idx_buf using the cumulative
 *                     per-(task,group) cursor pre-computed by the caller.
 *
 * On 5 M-row Q11 with 84 groups the serial loop was 8-10 ms; this
 * parallel form takes ~0.5 ms at 28 workers and drops Q11/Q14-class
 * queries by an order of magnitude when the per-group dedup is also
 * parallelised. */
typedef struct {
    const int64_t*  row_gid;
    int64_t*        hist;        /* [n_tasks * n_groups] */
    int64_t*        cursor;      /* [n_tasks * n_groups] */
    int64_t*        idx_buf;
    int64_t         n_groups;
    int64_t         grain;       /* rows per task (for task_id derivation) */
} idxbuf_par_ctx_t;

static void idxbuf_hist_fn(void* vctx, uint32_t worker_id,
                           int64_t start, int64_t end) {
    (void)worker_id;
    idxbuf_par_ctx_t* ctx = (idxbuf_par_ctx_t*)vctx;
    int64_t task_id = start / ctx->grain;
    int64_t* hist = ctx->hist + task_id * ctx->n_groups;
    const int64_t* row_gid = ctx->row_gid;
    for (int64_t r = start; r < end; r++) {
        int64_t gi = row_gid[r];
        if (gi >= 0) hist[gi]++;
    }
}

static void idxbuf_scat_fn(void* vctx, uint32_t worker_id,
                           int64_t start, int64_t end) {
    (void)worker_id;
    idxbuf_par_ctx_t* ctx = (idxbuf_par_ctx_t*)vctx;
    int64_t task_id = start / ctx->grain;
    int64_t* cur = ctx->cursor + task_id * ctx->n_groups;
    const int64_t* row_gid = ctx->row_gid;
    int64_t* idx_buf = ctx->idx_buf;
    for (int64_t r = start; r < end; r++) {
        int64_t gi = row_gid[r];
        if (gi >= 0) idx_buf[cur[gi]++] = r;
    }
}

/* Per-group count(distinct) using the existing OP_COUNT_DISTINCT kernel.
 * Mirrors aggr_unary_per_group_buf but slices the source column once per
 * group and calls exec_count_distinct directly — bypasses the full
 * ray_eval per-group path that re-walks the (count (distinct …)) AST
 * for each slice.
 *
 * `inner_expr` is the operand to `distinct` extracted via
 * match_count_distinct (typically a column ref, possibly a dotted-name
 * or computed sub-expression).  Returns an I64 vector of length
 * n_groups with the per-group distinct count. */
static ray_t* count_distinct_per_group_buf(ray_t* inner_expr, ray_t* tbl,
                                           const int64_t* idx_buf,
                                           const int64_t* offsets,
                                           const int64_t* grp_cnt,
                                           int64_t n_groups) {
    /* Resolve the source vector — either a direct column ref (zero copy)
     * or a full-table eval of the inner sub-expression. */
    ray_t* src = NULL;
    if (inner_expr && inner_expr->type == -RAY_SYM &&
        (inner_expr->attrs & RAY_ATTR_NAME)) {
        src = ray_table_get_col(tbl, inner_expr->i64);
        if (src) ray_retain(src);
    }
    if (!src) {
        if (ray_env_push_scope() != RAY_OK) return ray_error("oom", NULL);
        expr_bind_table_names(inner_expr, tbl);
        src = ray_eval(inner_expr);
        ray_env_pop_scope();
        if (!src || RAY_IS_ERR(src)) return src ? src : ray_error("domain", NULL);
    }

    ray_t* out = ray_vec_new(RAY_I64, n_groups);
    if (!out || RAY_IS_ERR(out)) {
        ray_release(src);
        return out ? out : ray_error("oom", NULL);
    }
    out->len = n_groups;
    int64_t* odata = (int64_t*)ray_data(out);

    /* Parallel path: dispatch one task per group when src has a flat
     * numeric / SYM layout we can read with a typed pointer.  Each task
     * does its own dedup with a scratch hash table — no gather_by_idx
     * allocation, no exec_count_distinct overhead per group.
     *
     * Q11/Q14/Q15-class workloads see ≥ 10× speedup at 28 workers
     * because the serial loop was leaving the cores idle.  Fall through
     * to the existing serial path on type mismatch, error, or OOM. */
    {
        int8_t st = src->type;
        bool flat_ok = (st == RAY_BOOL || st == RAY_U8 ||
                        st == RAY_I16  || st == RAY_I32 || st == RAY_I64 ||
                        st == RAY_F64  || st == RAY_DATE || st == RAY_TIME ||
                        st == RAY_TIMESTAMP || RAY_IS_SYM(st));
        ray_pool_t* pool = ray_pool_get();
        if (flat_ok && pool && ray_pool_total_workers(pool) >= 2 && n_groups >= 4) {
            cdpg_buf_par_ctx_t pctx = {
                .in_type   = st,
                .in_attrs  = src->attrs,
                .base      = ray_data(src),
                .has_nulls = (src->attrs & RAY_ATTR_HAS_NULLS) != 0,
                .null_bm   = NULL,
                .esz       = ray_sym_elem_size(st, src->attrs),
                .is_f64    = (st == RAY_F64),
                .idx_buf   = idx_buf,
                .offsets   = offsets,
                .grp_cnt   = grp_cnt,
                .odata     = odata,
                .oom       = 0,
            };
            if (pctx.has_nulls)
                pctx.null_bm = ray_vec_nullmap_bytes(src, NULL, NULL);
            ray_pool_dispatch_n(pool, cdpg_buf_par_fn, &pctx, (uint32_t)n_groups);
            if (!atomic_load_explicit(&pctx.oom, memory_order_relaxed)) {
                ray_release(src);
                return out;
            }
            /* OOM in scratch — fall through to serial path with the
             * already-allocated `out` (will be overwritten). */
        }
    }

    for (int64_t gi = 0; gi < n_groups; gi++) {
        int64_t cnt = grp_cnt[gi];
        if (cnt == 0) { odata[gi] = 0; continue; }
        /* gather_by_idx preserves the source's typed layout (I64 stays
         * I64, SYM stays SYM with adaptive width, etc.) — exactly what
         * exec_count_distinct expects.  ray_at_fn would coerce numeric
         * vec + numeric idx vec into a RAY_LIST of atoms, breaking the
         * type-dispatch in exec_count_distinct. */
        ray_t* subset = gather_by_idx(src,
            (int64_t*)&idx_buf[offsets[gi]], cnt);
        if (!subset || RAY_IS_ERR(subset)) {
            ray_t* err = subset ? subset : ray_error("oom", NULL);
            ray_release(src); ray_release(out);
            return err;
        }
        ray_t* cv = exec_count_distinct(NULL, NULL, subset);
        ray_release(subset);
        if (!cv || RAY_IS_ERR(cv)) {
            ray_t* err = cv ? cv : ray_error("oom", NULL);
            ray_release(src); ray_release(out);
            return err;
        }
        /* exec_count_distinct returns an i64 atom. */
        odata[gi] = (cv->type == -RAY_I64) ? cv->i64
                  : (cv->type == -RAY_I32) ? (int64_t)cv->i32 : 0;
        ray_release(cv);
    }

    ray_release(src);
    return out;
}

/* Variant for the LIST-`groups` layout used by the eval-fallback
 * (ray_group_fn output is a 2-list of {key, idx_list} pairs).  Slices
 * via ray_at_fn the same way and dispatches to exec_count_distinct. */
static ray_t* count_distinct_per_group_groups(ray_t* inner_expr, ray_t* tbl,
                                              ray_t* groups, int64_t n_groups) {
    ray_t* src = NULL;
    if (inner_expr && inner_expr->type == -RAY_SYM &&
        (inner_expr->attrs & RAY_ATTR_NAME)) {
        src = ray_table_get_col(tbl, inner_expr->i64);
        if (src) ray_retain(src);
    }
    if (!src) {
        if (ray_env_push_scope() != RAY_OK) return ray_error("oom", NULL);
        expr_bind_table_names(inner_expr, tbl);
        src = ray_eval(inner_expr);
        ray_env_pop_scope();
        if (!src || RAY_IS_ERR(src)) return src ? src : ray_error("domain", NULL);
    }

    ray_t* out = ray_vec_new(RAY_I64, n_groups);
    if (!out || RAY_IS_ERR(out)) { ray_release(src); return out ? out : ray_error("oom", NULL); }
    out->len = n_groups;
    int64_t* odata = (int64_t*)ray_data(out);

    ray_t** items = (ray_t**)ray_data(groups);
    for (int64_t gi = 0; gi < n_groups; gi++) {
        ray_t* idx_list = items[gi * 2 + 1];
        if (!idx_list) { odata[gi] = 0; continue; }
        int64_t cnt = ray_len(idx_list);
        if (cnt == 0) { odata[gi] = 0; continue; }

        /* idx_list from ray_group_fn is an I64 vector — gather_by_idx
         * needs a raw int64_t* + count, so resolve the pointer either
         * directly (typed I64 vec) or by walking the LIST cells. */
        ray_t* subset = NULL;
        ray_t* tmp_hdr = NULL;
        if (idx_list->type == RAY_I64) {
            subset = gather_by_idx(src, (int64_t*)ray_data(idx_list), cnt);
        } else {
            /* Fallback: copy indices into a scratch buffer.  Rare path —
             * shouldn't trigger for well-formed ray_group_fn output. */
            int64_t* tmp = (int64_t*)scratch_alloc(&tmp_hdr,
                (size_t)cnt * sizeof(int64_t));
            if (!tmp) {
                ray_release(src); ray_release(out);
                return ray_error("oom", NULL);
            }
            for (int64_t k = 0; k < cnt; k++) {
                int alloc = 0;
                ray_t* e = collection_elem(idx_list, k, &alloc);
                tmp[k] = e ? as_i64(e) : 0;
                if (alloc && e) ray_release(e);
            }
            subset = gather_by_idx(src, tmp, cnt);
            scratch_free(tmp_hdr);
        }
        if (!subset || RAY_IS_ERR(subset)) {
            ray_t* err = subset ? subset : ray_error("oom", NULL);
            ray_release(src); ray_release(out);
            return err;
        }
        ray_t* cv = exec_count_distinct(NULL, NULL, subset);
        ray_release(subset);
        if (!cv || RAY_IS_ERR(cv)) {
            ray_t* err = cv ? cv : ray_error("oom", NULL);
            ray_release(src); ray_release(out);
            return err;
        }
        odata[gi] = (cv->type == -RAY_I64) ? cv->i64
                  : (cv->type == -RAY_I32) ? (int64_t)cv->i32 : 0;
        ray_release(cv);
    }

    ray_release(src);
    return out;
}

/* Width-agnostic key reader: read row `idx` of a group-key column as
 * int64_t.  Same coverage as the KEY_READ macro inside ray_select_fn,
 * lifted to file scope so the parallel row→gid probe worker can use it. */
static inline int64_t key_read_i64(const void* d, int64_t idx,
                                   int8_t bt, uint8_t attrs) {
    switch (bt) {
    case RAY_BOOL:
    case RAY_U8:        return ((const uint8_t*)d)[idx];
    case RAY_I16:       return ((const int16_t*)d)[idx];
    case RAY_I32:
    case RAY_DATE:
    case RAY_TIME:      return ((const int32_t*)d)[idx];
    case RAY_I64:
    case RAY_TIMESTAMP: return ((const int64_t*)d)[idx];
    case RAY_F32: { uint32_t u;
        memcpy(&u, &((const float*)d)[idx], 4);
        return (int64_t)u; }
    case RAY_F64: { int64_t u;
        memcpy(&u, &((const double*)d)[idx], 8);
        return u; }
    case RAY_SYM:       return ray_read_sym(d, idx, bt, attrs);
    default:            return 0; /* caller validates type */
    }
}

/* Parallel row→gid probe.  Hash table is read-only by the time the probe
 * runs (the insert phase that built it is single-threaded), so each
 * worker can process its row range independently with no synchronisation.
 *
 * The probe's per-row work is one cache-cold load + a short linear-probe
 * walk in a hash sized to 2 × n_groups.  At Q14 scale (611 K groups,
 * ~18 MB hash) the serial loop spends most of its time waiting on cache
 * misses; spreading the rows across 28 cores gives near-linear speedup
 * because each core has its own cache hierarchy. */
typedef struct {
    /* Hash table contents (read-only). */
    const int64_t* hk_keys;
    const int32_t* hk_gid_p1;     /* one of these is non-NULL */
    const int64_t* hk_gid64;
    uint64_t       mask;
    /* Group-key column being probed. */
    const void* orig_key_data;
    int8_t      okt;
    uint8_t     okt_attrs;
    /* Per-row output. */
    int64_t* row_gid;
    /* Optional selection — when non-NULL, skip rows that don't pass.
     * Lets row_gid build and selection mask happen in one parallel pass
     * (saves the serial sel_mask loop downstream).  Both can be NULL
     * (full-table probe). */
    ray_t*          selection;
    const uint8_t*  sel_flg;
    const uint32_t* sel_offs;
    const uint16_t* sel_idx;
    uint32_t        sel_n_segs;
} rgid_probe_ctx_t;

static void rgid_probe_fn(void* ctx_, uint32_t worker_id,
                          int64_t start, int64_t end) {
    (void)worker_id;
    rgid_probe_ctx_t* x = (rgid_probe_ctx_t*)ctx_;
    int use_i64 = (x->hk_gid64 != NULL);
    uint64_t mask = x->mask;

    /* Fused selection-aware probe: when a rowsel is attached, walk per
     * morsel segment and only do the HT probe for rows that actually
     * passed the WHERE filter.  Filtered-out rows write -1 directly,
     * skipping the (potentially expensive) hash + linear-probe lookup.
     *
     * This eliminates the separate serial sel_mask pass downstream that
     * was costing ~3 ms on Q11 (5 M rows / 4880 segments scalar walk). */
    if (x->selection) {
        const uint8_t*  flg  = x->sel_flg;
        const uint32_t* offs = x->sel_offs;
        const uint16_t* lidx = x->sel_idx;
        uint32_t seg_lo = (uint32_t)(start / RAY_MORSEL_ELEMS);
        uint32_t seg_hi = (uint32_t)((end + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);
        if (seg_hi > x->sel_n_segs) seg_hi = x->sel_n_segs;
        for (uint32_t seg = seg_lo; seg < seg_hi; seg++) {
            int64_t s_lo = (int64_t)seg * RAY_MORSEL_ELEMS;
            int64_t s_hi = s_lo + RAY_MORSEL_ELEMS;
            if (s_lo < start) s_lo = start;
            if (s_hi > end)   s_hi = end;
            uint8_t f = flg[seg];
            if (f == RAY_SEL_NONE) {
                for (int64_t r = s_lo; r < s_hi; r++) x->row_gid[r] = -1;
                continue;
            }
            if (f == RAY_SEL_ALL) {
                for (int64_t r = s_lo; r < s_hi; r++) {
                    int64_t rv = key_read_i64(x->orig_key_data, r, x->okt, x->okt_attrs);
                    uint64_t h = (uint64_t)rv * 0x9E3779B97F4A7C15ULL;
                    h ^= h >> 33;
                    uint64_t sl = h & mask;
                    int64_t found = -1;
                    for (;;) {
                        int64_t cur_p1 = use_i64 ? x->hk_gid64[sl]
                                                 : (int64_t)x->hk_gid_p1[sl];
                        if (cur_p1 == 0) break;
                        if (x->hk_keys[sl] == rv) { found = cur_p1 - 1; break; }
                        sl = (sl + 1) & mask;
                    }
                    x->row_gid[r] = found;
                }
                continue;
            }
            /* RAY_SEL_MIX: build in-segment bitmap, then probe + mask. */
            uint8_t in_seg[RAY_MORSEL_ELEMS / 8] = {0};
            uint32_t off = offs[seg];
            uint32_t cnt = offs[seg + 1] - off;
            for (uint32_t i = 0; i < cnt; i++) {
                uint16_t loc = lidx[off + i];
                in_seg[loc >> 3] |= (uint8_t)(1u << (loc & 7));
            }
            int64_t base = (int64_t)seg * RAY_MORSEL_ELEMS;
            for (int64_t r = s_lo; r < s_hi; r++) {
                uint16_t loc = (uint16_t)(r - base);
                if (!(in_seg[loc >> 3] & (1u << (loc & 7)))) {
                    x->row_gid[r] = -1;
                    continue;
                }
                int64_t rv = key_read_i64(x->orig_key_data, r, x->okt, x->okt_attrs);
                uint64_t h = (uint64_t)rv * 0x9E3779B97F4A7C15ULL;
                h ^= h >> 33;
                uint64_t sl = h & mask;
                int64_t found = -1;
                for (;;) {
                    int64_t cur_p1 = use_i64 ? x->hk_gid64[sl]
                                             : (int64_t)x->hk_gid_p1[sl];
                    if (cur_p1 == 0) break;
                    if (x->hk_keys[sl] == rv) { found = cur_p1 - 1; break; }
                    sl = (sl + 1) & mask;
                }
                x->row_gid[r] = found;
            }
        }
        return;
    }

    for (int64_t r = start; r < end; r++) {
        int64_t rv = key_read_i64(x->orig_key_data, r, x->okt, x->okt_attrs);
        uint64_t h = (uint64_t)rv * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 33;
        uint64_t s = h & mask;
        int64_t found = -1;
        for (;;) {
            int64_t cur_p1 = use_i64 ? x->hk_gid64[s] : (int64_t)x->hk_gid_p1[s];
            if (cur_p1 == 0) break;
            if (x->hk_keys[s] == rv) { found = cur_p1 - 1; break; }
            s = (s + 1) & mask;
        }
        x->row_gid[r] = found;
    }
}

/* Forward declarations for eval-level groupby fallback */

/* R8: cheap predicate for whether atom_broadcast_vec can handle this
 * atom AND the atom is a self-evaluating literal (not a name binding
 * that needs ray_eval to resolve to a column or computed value).  Used
 * by the all-literal pre-check so we don't half-apply a partial set of
 * broadcasts and then have to roll back.
 *
 * `RAY_ATTR_NAME` distinguishes `m2: m` (the SYM `m` references a
 * column) from `one: 1` (the I64 literal 1).  Without that filter we'd
 * eagerly broadcast the column reference and skip the per-group gather
 * the chained passthrough relies on. */
static int can_atom_broadcast(ray_t* a) {
    if (!a || !ray_is_atom(a)) return 0;
    if (a->attrs & RAY_ATTR_NAME) return 0;
    int8_t vt = (int8_t)(-a->type);
    switch (vt) {
    case RAY_BOOL: case RAY_U8:
    case RAY_I16:  case RAY_I32:
    case RAY_I64:  case RAY_F64:
    case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
    case RAY_SYM:
        return 1;
    default:
        return 0;
    }
}

/* R8: build a typed N-cell vector all containing the value of atom `a`.
 *
 * The non-agg scatter path used to bind a `{lit: 1, c: count(...) by: K}`
 * style query into a per-group RAY_LIST of N retained atoms, which
 * ballooned Q35 from ~21 ms to ~140 ms (one ray_retain + list slot
 * per group, scaling with output cardinality, not row count).  Allocate
 * once and fill — Q35 falls back into parity with Q34.
 *
 * Returns NULL for atom types not yet handled (RAY_STR, RAY_GUID, F32);
 * caller falls back to the per-cell LIST path. */
static ray_t* atom_broadcast_vec(ray_t* a, int64_t n) {
    if (!a || !ray_is_atom(a) || n <= 0) return NULL;
    int8_t vec_type = (int8_t)(-a->type);
    if (vec_type <= 0) return NULL;

    ray_t* v;
    if (vec_type == RAY_SYM) {
        uint8_t w = (uint8_t)(a->attrs & RAY_SYM_W_MASK);
        v = ray_sym_vec_new(w, n);
    } else {
        v = ray_vec_new(vec_type, n);
    }
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = n;

    void* dst = ray_data(v);
    switch (vec_type) {
    case RAY_BOOL:
    case RAY_U8: {
        memset(dst, a->b8, (size_t)n);
        break;
    }
    case RAY_I16: {
        int16_t val = a->i16;
        int16_t* d = (int16_t*)dst;
        for (int64_t i = 0; i < n; i++) d[i] = val;
        break;
    }
    case RAY_I32:
    case RAY_DATE:
    case RAY_TIME: {
        int32_t val = a->i32;
        int32_t* d = (int32_t*)dst;
        for (int64_t i = 0; i < n; i++) d[i] = val;
        break;
    }
    case RAY_I64:
    case RAY_TIMESTAMP: {
        int64_t val = a->i64;
        int64_t* d = (int64_t*)dst;
        for (int64_t i = 0; i < n; i++) d[i] = val;
        break;
    }
    case RAY_F64: {
        double val = a->f64;
        double* d = (double*)dst;
        for (int64_t i = 0; i < n; i++) d[i] = val;
        break;
    }
    case RAY_SYM: {
        /* SYM stores the ID in `i64` regardless of width; truncate per
         * the vector's width attribute.  Width came from the atom and
         * was carried by ray_sym_vec_new above. */
        uint8_t w = (uint8_t)(a->attrs & RAY_SYM_W_MASK);
        if (w == RAY_SYM_W8) {
            memset(dst, (uint8_t)a->i64, (size_t)n);
        } else if (w == RAY_SYM_W16) {
            uint16_t val = (uint16_t)a->i64;
            uint16_t* d = (uint16_t*)dst;
            for (int64_t i = 0; i < n; i++) d[i] = val;
        } else { /* W32 — default */
            uint32_t val = (uint32_t)a->i64;
            uint32_t* d = (uint32_t*)dst;
            for (int64_t i = 0; i < n; i++) d[i] = val;
        }
        break;
    }
    default:
        ray_release(v);
        return NULL;
    }

    /* Propagate atom-null: an entirely-null broadcast keeps the null bit
     * of every cell so `is_null` and aggregations behave the same as
     * the LIST path would have. */
    if (RAY_ATOM_IS_NULL(a)) {
        v->attrs |= RAY_ATTR_HAS_NULLS;
        memset(v->nullmap, 0xFF, 16);
    }
    return v;
}

/* (select {from: t [where: pred] [by: key] [col: expr ...]})
 * Special form — receives unevaluated dict arg. */
ray_t* ray_select_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);
    ray_t* dict = args[0];
    if (!dict || dict->type != RAY_DICT)
        return ray_error("type", NULL);

    /* Evaluate 'from:' to get the source table */
    ray_t* from_expr = dict_get(dict, "from");
    if (!from_expr) return ray_error("domain", NULL);
    ray_t* tbl = ray_eval(from_expr);
    if (RAY_IS_ERR(tbl)) return tbl;
    if (tbl->type != RAY_TABLE) { ray_release(tbl); return ray_error("type", NULL); }

    ray_t* where_expr = dict_get(dict, "where");
    ray_t* by_expr = dict_get(dict, "by");
    ray_t* take_expr = dict_get(dict, "take");
    ray_t* nearest_expr = dict_get(dict, "nearest");

    /* Collect output columns (keys that are not reserved).  The dict's
     * physical layout is [keys, vals] but the iteration loops below were
     * written for the old interleaved [k0,v0,...] form — open a transient
     * pair view so the existing code keeps working. */
    DICT_VIEW_DECL(dv);
    DICT_VIEW_OPEN(dict, dv);
    if (DICT_VIEW_OVERFLOW(dv)) {
        ray_release(tbl);
        return ray_error("domain", "select clause has too many keys");
    }
    int64_t dict_n = dv_n;
    ray_t** dict_elems = dv;
    int64_t from_id    = ray_sym_intern("from",    4);
    int64_t where_id   = ray_sym_intern("where",   5);
    int64_t by_id      = ray_sym_intern("by",      2);
    int64_t take_id    = ray_sym_intern("take",    4);
    int64_t asc_id     = ray_sym_intern("asc",     3);
    int64_t desc_id    = ray_sym_intern("desc",    4);
    int64_t nearest_id = ray_sym_intern("nearest", 7);

    /* Check for asc/desc presence */
    bool has_sort = false;
    for (int64_t i = 0; i + 1 < dict_n; i += 2) {
        int64_t kid = dict_elems[i]->i64;
        if (kid == asc_id || kid == desc_id) { has_sort = true; break; }
    }

    /* `nearest` is mutually exclusive with `asc`/`desc`/`by` — ANN
     * ordering is an index scan, not a column sort, and cannot be
     * composed with group-by in this phase. */
    if (nearest_expr) {
        if (has_sort) {
            ray_release(tbl);
            return ray_error("domain",
                "select: `nearest` cannot be combined with asc/desc");
        }
        if (by_expr) {
            ray_release(tbl);
            return ray_error("domain",
                "select: `nearest` cannot be combined with `by`");
        }
    }

    /* Count output columns */
    int n_out = 0;
    for (int64_t i = 0; i + 1 < dict_n; i += 2) {
        int64_t kid = dict_elems[i]->i64;
        if (kid != from_id && kid != where_id && kid != by_id &&
            kid != take_id && kid != asc_id && kid != desc_id &&
            kid != nearest_id)
            n_out++;
    }

    /* Simple case: no clauses at all → return table as-is */
    if (n_out == 0 && !where_expr && !by_expr && !take_expr && !has_sort && !nearest_expr)
        return tbl;

    /* Fused filter + top-K: shape `(select {col1: c1 col2: c2 …
     * from: T where: <pred> asc/desc: <key> take: <K>})` with no by/
     * nearest clause and all output columns being plain SYM names of
     * source columns.  Bypasses the FILTER → SORT_TAKE pipeline: streams
     * predicate eval and bounded-heap insert in one pass, no
     * intermediate filtered table materialised.  The biggest open gap
     * vs DuckDB on ClickBench (Q25/Q26/Q27 are 2.5–4.5× slower without
     * this fast path). */
    if (where_expr && take_expr && has_sort && !by_expr && !nearest_expr) {
        const char* off_env = getenv("RAYFORCE_DISABLE_FUSED_TOPK");
        int env_disabled = (off_env && off_env[0] == '1');
        if (!env_disabled && ray_fused_topk_supported(where_expr, tbl)) {
            /* Walk the dict and check: exactly one asc/desc clause naming
             * a single scalar column, take is an atom K, and every
             * output column is a -RAY_SYM source-column reference (no
             * agg, no expression). */
            int64_t sort_key_syms[16];
            uint8_t sort_descs[16];
            uint8_t n_sort_keys = 0;
            int     bad_clause = 0;
            int64_t out_syms[64];
            uint8_t n_out_syms = 0;
            for (int64_t i = 0; i + 1 < dict_n; i += 2) {
                int64_t kid = dict_elems[i]->i64;
                ray_t*  v   = dict_elems[i + 1];
                if (kid == from_id || kid == where_id || kid == take_id ||
                    kid == nearest_id) continue;
                if (kid == asc_id || kid == desc_id) {
                    uint8_t is_desc = (kid == desc_id) ? 1 : 0;
                    if (v && v->type == -RAY_SYM && (v->attrs & RAY_ATTR_NAME)) {
                        if (n_sort_keys >= 16) { bad_clause = 1; break; }
                        sort_key_syms[n_sort_keys] = v->i64;
                        sort_descs[n_sort_keys] = is_desc;
                        n_sort_keys++;
                    } else if (v && v->type == RAY_SYM) {
                        int64_t nv = ray_len(v);
                        if (n_sort_keys + nv > 16) { bad_clause = 1; break; }
                        int64_t* sv = (int64_t*)ray_data(v);
                        for (int64_t kk = 0; kk < nv; kk++) {
                            sort_key_syms[n_sort_keys] = sv[kk];
                            sort_descs[n_sort_keys] = is_desc;
                            n_sort_keys++;
                        }
                    } else {
                        bad_clause = 1; break;
                    }
                    continue;
                }
                if (kid == by_id) { bad_clause = 1; break; }
                /* Output column must be a *trivial* projection of a source
                 * column — and the dict key (the output alias) must equal
                 * the source column sym, since the fused materialiser names
                 * output columns after the source.  Renamings like
                 * `{alias: source_col}` would silently emit a column named
                 * `source_col` instead of `alias`, which is a schema bug.
                 * Until the fused exec accepts a separate aliases array,
                 * gate the fused path off for any rename. */
                if (n_out_syms >= 64) { bad_clause = 1; break; }
                if (v && v->type == -RAY_SYM && (v->attrs & RAY_ATTR_NAME)
                    && v->i64 == kid
                    && ray_table_get_col(tbl, v->i64) != NULL) {
                    out_syms[n_out_syms++] = v->i64;
                } else {
                    bad_clause = 1;
                    break;
                }
            }
            /* Nullable columns are not supported by the fused materialise:
             * the fused path reads raw payload values and writes them back
             * via write_col_i64 without propagating the source column's
             * nullmap.  Sort key compares also bypass null ordering.  Gate
             * fused off if any sort key or output column carries nulls so
             * the unfused FILTER + SORT + TAKE path handles them.  Same
             * rationale is applied to sort/output cols (not just sort keys)
             * because materialised null sentinels would surface even if
             * the column isn't on the sort path. */
            if (!bad_clause) {
                for (uint8_t i = 0; i < n_sort_keys && !bad_clause; i++) {
                    ray_t* kc = ray_table_get_col(tbl, sort_key_syms[i]);
                    if (!kc || (kc->attrs & RAY_ATTR_HAS_NULLS)) bad_clause = 1;
                }
                for (uint8_t i = 0; i < n_out_syms && !bad_clause; i++) {
                    ray_t* oc = ray_table_get_col(tbl, out_syms[i]);
                    if (!oc || (oc->attrs & RAY_ATTR_HAS_NULLS)) bad_clause = 1;
                }
            }
            if (!bad_clause && n_sort_keys > 0 && n_out_syms > 0) {
                ray_t* tv = ray_eval(take_expr);
                if (tv && !RAY_IS_ERR(tv) && ray_is_atom(tv) &&
                    (tv->type == -RAY_I64 || tv->type == -RAY_I32)) {
                    int64_t k = (tv->type == -RAY_I64) ? tv->i64 : tv->i32;
                    ray_release(tv);
                    if (k > 0 && k <= FPK_MAX_K && k < ray_table_nrows(tbl)) {
                        ray_t* res = ray_fused_topk_select(tbl, where_expr,
                                                           sort_key_syms,
                                                           sort_descs,
                                                           n_sort_keys, k,
                                                           out_syms, n_out_syms);
                        if (res && !RAY_IS_ERR(res)) {
                            ray_release(tbl);
                            return res;
                        }
                        if (res && RAY_IS_ERR(res)) ray_release(res);
                    }
                } else if (tv) {
                    ray_release(tv);
                }
            }
        }
    }

    /* Dict-form by-clause pre-evaluation: MUST happen before we
     * build the DAG, so the graph sees the augmented table with
     * the materialised dict-val columns already present.
     * (select {... by: {o: OrderId b: (xbar Ts 1000)} ...})
     * Dict values can be any expression; we eval each against tbl
     * with its columns bound as locals, add the result as a new
     * column named after the dict key, then rewrite by_expr as a
     * plain RAY_SYM vector of the dict keys so the rest of
     * ray_select_fn sees a standard multi-key group-by. */
    ray_t* by_sym_vec_owned = NULL;

    /* Selection saved across the path-A graph free for count(distinct
     * col_ref) non-aggs.  Path B leaves this NULL because the
     * materialised filtered_tbl already encodes the selection in row
     * positions.  Declared here at function scope so the cleanup at
     * the bottom of ray_select_fn can release it. */
    ray_t* saved_selection = NULL;
    DICT_VIEW_DECL(byv);
    if (by_expr && by_expr->type == RAY_DICT) {
        DICT_VIEW_OPEN(by_expr, byv);
        if (DICT_VIEW_OVERFLOW(byv)) {
            ray_release(tbl);
            return ray_error("domain", "by-dict has too many keys");
        }
        int64_t dlen = byv_n;
        int64_t nk = dlen / 2;
        if (nk == 0 || nk > 16) {
            ray_release(tbl);
            return ray_error("domain", "by-dict must have 1..16 keys");
        }
        ray_t** d_elems = byv;

        ray_env_push_scope();
        int64_t in_ncols = ray_table_ncols(tbl);
        for (int64_t c = 0; c < in_ncols; c++) {
            int64_t cn = ray_table_col_name(tbl, c);
            ray_t* cv = ray_table_get_col_idx(tbl, c);
            if (cv) ray_env_set_local(cn, cv);
        }

        by_sym_vec_owned = ray_vec_new(RAY_SYM, nk);
        if (!by_sym_vec_owned || RAY_IS_ERR(by_sym_vec_owned)) {
            ray_env_pop_scope();
            ray_release(tbl);
            return ray_error("oom", NULL);
        }
        int64_t* sv_data = (int64_t*)ray_data(by_sym_vec_owned);
        by_sym_vec_owned->len = nk;

        bool failed = false;
        ray_t* fail_err = NULL;
        int64_t expected_len = ray_table_nrows(tbl);
        for (int64_t i = 0; i < nk; i++) {
            ray_t* k = d_elems[i * 2];
            ray_t* v = d_elems[i * 2 + 1];
            if (!k || k->type != -RAY_SYM) {
                fail_err = ray_error("domain", "by-dict key must be a symbol name");
                failed = true; break;
            }
            /* Duplicate key guard: {g: A g: B} would otherwise append
             * two cols both named g, then group on the first g twice
             * (silently dropping B).  Reject explicitly. */
            bool duplicate_key = false;
            for (int64_t j = 0; j < i && !duplicate_key; j++)
                if (d_elems[j * 2]->i64 == k->i64) duplicate_key = true;
            if (duplicate_key) {
                fail_err = ray_error("domain", "by-dict has duplicate key");
                failed = true; break;
            }
            /* Collision check: if the dict key already exists in the
             * input table, ray_table_add_col would append a second
             * column with the same name and ray_table_get_col finds
             * the ORIGINAL, so the group-by would silently scan the
             * user's existing column instead of our materialised
             * one.  The one allowed exception is {x: x}, a trivial
             * self-alias: the input column is already exactly what
             * we want to group on. */
            bool already_in_tbl = (ray_table_get_col(tbl, k->i64) != NULL);
            bool trivial_self = (v->type == -RAY_SYM && v->i64 == k->i64);
            if (already_in_tbl && !trivial_self) {
                fail_err = ray_error("domain",
                    "by-dict alias shadows an existing input column");
                failed = true; break;
            }
            if (trivial_self) {
                /* No eval / no add: just group on the existing col. */
                sv_data[i] = k->i64;
                continue;
            }
            ray_t* col_vec = ray_eval(v);
            if (!col_vec || RAY_IS_ERR(col_vec)) {
                fail_err = col_vec ? col_vec : ray_error("domain", "by-dict val eval");
                failed = true; break;
            }
            if (!ray_is_vec(col_vec) || ray_len(col_vec) != expected_len) {
                ray_release(col_vec);
                fail_err = ray_error("length", "by-dict val must be a column vector");
                failed = true; break;
            }
            ray_t* new_tbl = ray_table_add_col(tbl, k->i64, col_vec);
            ray_release(col_vec);
            if (!new_tbl || RAY_IS_ERR(new_tbl)) {
                fail_err = new_tbl ? new_tbl : ray_error("oom", NULL);
                failed = true; break;
            }
            tbl = new_tbl;
            /* Re-bind the newly added column under its dict key so
             * later dict vals can reference earlier keys. */
            ray_env_set_local(k->i64, col_vec);
            sv_data[i] = k->i64;
        }
        ray_env_pop_scope();
        if (failed) {
            ray_release(by_sym_vec_owned);
            ray_release(tbl);
            return fail_err;
        }
        by_expr = by_sym_vec_owned;
    }

    /* Build DAG */
    ray_graph_t* g = ray_graph_new(tbl);
    if (!g) {
        if (by_sym_vec_owned) ray_release(by_sym_vec_owned);
        ray_release(tbl); return ray_error("oom", NULL);
    }

    ray_op_t* root = ray_const_table(g, tbl);

    /* Non-agg expression tracking for post-DAG scatter (used in GROUP BY) */
    int64_t nonagg_names[16];
    ray_t*  nonagg_exprs[16];
    uint8_t n_nonaggs = 0;
    int synth_count_col = 0;  /* 1 if we synthesized OP_COUNT for group boundaries */

    /* Phase-1 OP_FILTERED_GROUP gate.  When the (select … where … by …)
     * shape matches the supported vocabulary, route through the fused
     * operator instead of FILTER + GROUP.  We pre-scan the dict here so
     * the WHERE block and the eager-filter step downstream can be
     * short-circuited; the fused node consumes the predicate directly.
     *
     * Guard with RAYFORCE_DISABLE_FUSED_GROUP=1 for A/B comparisons. */
    int can_fuse_phase1 = 0;
    ray_op_t* fused_pred_op = NULL;  /* compiled below in the WHERE block */
    {
        const char* off_env = getenv("RAYFORCE_DISABLE_FUSED_GROUP");
        int env_disabled = (off_env && off_env[0] == '1');
        /* by_expr forms accepted:
         *   - scalar  -RAY_SYM with NAME    (single key, e.g. Q8/Q37)
         *   - vector  RAY_SYM with len 1..16 (dict-form pre-evaluated)
         * Multi-key cases (len > 1) only fire if the multi path can pack
         * keys into ≤ 8 bytes total (composite int64 slot). */
        int single_key_scalar = by_expr && by_expr->type == -RAY_SYM
                              && (by_expr->attrs & RAY_ATTR_NAME);
        int multi_key_vec     = by_expr && by_expr->type == RAY_SYM
                              && ray_len(by_expr) >= 1
                              && ray_len(by_expr) <= 16;
        if (!env_disabled
            && where_expr && by_expr && !nearest_expr
            && (single_key_scalar || multi_key_vec)
            && ray_fused_group_supported(where_expr, tbl))
        {
            /* Walk the dict aggs.  Accept any combination of count/sum/
             * min/max/avg with non-COUNT requiring an integer/temporal
             * input column. */
            int n_aggs_ok  = 0;
            int n_other    = 0;
            int has_only_count = 1;
            int64_t count_sym = ray_sym_intern("count", 5);
            int64_t sum_sym   = ray_sym_intern("sum",   3);
            int64_t min_sym   = ray_sym_intern("min",   3);
            int64_t max_sym   = ray_sym_intern("max",   3);
            int64_t avg_sym   = ray_sym_intern("avg",   3);
            for (int64_t i = 0; i + 1 < dict_n; i += 2) {
                int64_t kid = dict_elems[i]->i64;
                if (kid == from_id || kid == where_id || kid == by_id ||
                    kid == take_id || kid == asc_id || kid == desc_id ||
                    kid == nearest_id) continue;
                ray_t* val_expr = dict_elems[i + 1];
                if (!is_group_dag_agg_expr(val_expr)) { n_other++; break; }
                ray_t** ae = (ray_t**)ray_data(val_expr);
                int64_t aid = ae[0]->i64;
                int op_ok = (aid == count_sym || aid == sum_sym ||
                             aid == min_sym   || aid == max_sym ||
                             aid == avg_sym);
                if (!op_ok || ray_len(val_expr) < 2) { n_other++; break; }
                if (aid != count_sym) has_only_count = 0;
                ray_t* ae1 = ae[1];
                if (!ae1 || !((ae1->type == -RAY_SYM
                               && (ae1->attrs & RAY_ATTR_NAME)))) {
                    n_other++; break;
                }
                if (aid != count_sym) {
                    ray_t* in_col = ray_table_get_col(tbl, ae1->i64);
                    if (!in_col) { n_other++; break; }
                    int8_t ict = in_col->type;
                    if (RAY_IS_PARTED(ict) || ict == RAY_MAPCOMMON)
                        { n_other++; break; }
                    if (ict != RAY_BOOL && ict != RAY_U8 && ict != RAY_I16
                        && ict != RAY_I32 && ict != RAY_I64
                        && ict != RAY_DATE && ict != RAY_TIME
                        && ict != RAY_TIMESTAMP)
                        { n_other++; break; }
                    /* Mirror mk_compile's null-rejection: the fused agg
                     * kernels read raw payload, so a stored null sentinel
                     * would corrupt SUM/MIN/MAX/AVG.  Keep these on the
                     * unfused null-aware OP_GROUP path. */
                    if (in_col->attrs & RAY_ATTR_HAS_NULLS)
                        { n_other++; break; }
                }
                n_aggs_ok++;
            }
            if (n_aggs_ok >= 1 && n_aggs_ok <= 8 && n_other == 0) {
                /* Validate keys: total packed bytes ≤ 8 for multi-key. */
                int64_t key_syms_buf[16];
                uint8_t n_keys_local = 0;
                if (single_key_scalar) {
                    key_syms_buf[0] = by_expr->i64;
                    n_keys_local = 1;
                } else {
                    int64_t* sv = (int64_t*)ray_data(by_expr);
                    n_keys_local = (uint8_t)ray_len(by_expr);
                    for (uint8_t i = 0; i < n_keys_local; i++)
                        key_syms_buf[i] = sv[i];
                }
                int keys_ok = 1;
                int total_bytes = 0;
                for (uint8_t i = 0; i < n_keys_local && keys_ok; i++) {
                    ray_t* kc = ray_table_get_col(tbl, key_syms_buf[i]);
                    if (!kc) { keys_ok = 0; break; }
                    int8_t kct = kc->type;
                    if (RAY_IS_PARTED(kct) || kct == RAY_MAPCOMMON)
                        { keys_ok = 0; break; }
                    if (kct != RAY_SYM && kct != RAY_BOOL && kct != RAY_U8
                        && kct != RAY_I16 && kct != RAY_I32 && kct != RAY_I64
                        && kct != RAY_DATE && kct != RAY_TIME
                        && kct != RAY_TIMESTAMP)
                        { keys_ok = 0; break; }
                    /* Mirror mk_compile's null-rejection: the composite
                     * key compose treats every byte as part of the key,
                     * so a null-sentinel collides with a row legitimately
                     * holding the same bit pattern.  Fall back to
                     * OP_GROUP, which buckets null keys separately. */
                    if (kc->attrs & RAY_ATTR_HAS_NULLS)
                        { keys_ok = 0; break; }
                    total_bytes += ray_sym_elem_size(kct, kc->attrs);
                }
                /* Single-key case fits unconditionally (one key column, one
                 * slot).  Multi-key narrow path (≤ 8 bytes packed) uses a
                 * single int64 slot; the wide path (9..16 bytes) adds a
                 * side kv_hi side array.  The wide path's extra hi compare
                 * + extra memory traffic only pays back for single-COUNT
                 * shapes (Q36, Q41); multi-agg high-card workloads (Q31,
                 * Q32) regress against the regular FILTER+GROUP path, so
                 * keep them on it. */
                int wide_fits  = (total_bytes >  8 && total_bytes <= 16
                                  && n_aggs_ok == 1 && has_only_count);
                int narrow_fits = (total_bytes <= 8);
                int fits = (n_keys_local == 1) || narrow_fits || wide_fits;
                if (keys_ok && fits) {
                    /* Don't fire the multi path when n_keys == 1 AND not
                     * count-only: the multi path's per-row update has higher
                     * overhead than count1.  Specifically, count1 owns the
                     * common-case wins. */
                    if (n_keys_local == 1 && n_aggs_ok == 1 && has_only_count) {
                        can_fuse_phase1 = 1;  /* will use count1 exec */
                    } else if (narrow_fits || wide_fits) {
                        can_fuse_phase1 = 1;  /* will use multi exec */
                    }
                }
            }
        }
    }

    /* Apply WHERE filter (unless folded into OP_FILTERED_GROUP). */
    if (where_expr) {
        /* When the WHERE is `(and a b c …)` and we're not folding into
         * OP_FILTERED_GROUP, compile each conjunct as its own OP_FILTER.
         * exec_filter on a TABLE input refines g->selection in place via
         * ray_rowsel_refine, so subsequent conjuncts only need to make
         * their predicate's output bool VALID for surviving rows — the
         * refine pass only reads pred[r] when r is still in the rowsel.
         *
         * This unlocks short-circuit-style behaviour for predicate
         * evaluators that opt into selection-aware execution
         * (exec_like).  Cheap conjuncts (binary_range comparisons) still
         * evaluate over the full table, but the AND-merge is replaced
         * by progressively-refined rowsel chaining — saving the OP_AND
         * binary AND-of-bool-vecs work. */
        int and_chained = 0;
        if (!can_fuse_phase1 && where_expr->type == RAY_LIST
            && ray_len(where_expr) >= 3)
        {
            ray_t** elems = (ray_t**)ray_data(where_expr);
            ray_t* head = elems[0];
            if (head && head->type == -RAY_SYM) {
                ray_t* hs = ray_sym_str(head->i64);
                if (hs && ray_str_len(hs) == 3
                    && memcmp(ray_str_ptr(hs), "and", 3) == 0)
                {
                    int64_t k = ray_len(where_expr) - 1;
                    /* Each conjunct must be a vec-producing predicate —
                     * compile to a non-OP_CONST node whose output type
                     * resolves to a vector.  Pure-literal conjuncts
                     * (e.g. `(== 1 0)`) compile to OP_CONST atoms that
                     * the OP_FILTER lazy path doesn't handle; fall back
                     * to the existing OP_AND tree, which ray_optimize
                     * already constant-folds. */
                    int all_ok = 1;
                    /* `reorder_safe` flips to 0 when any conjunct contains
                     * an op that can fault, error or have observable
                     * side effects — division, modulo, call into a user
                     * function, etc.  In that case we still chain the
                     * conjuncts as separate filters (the selection-aware
                     * win), but evaluate them in user-given order so an
                     * earlier guard like `(!= y 0)` keeps protecting a
                     * later `(< (/ x y) 10)`. */
                    int reorder_safe = 1;
                    ray_op_t* compiled[64];
                    int       cost[64];
                    if (k > 64) all_ok = 0;
                    /* Each conjunct must compile, must not be a pure
                     * constant, and must transitively reference at
                     * least one OP_SCAN (column read).  Pure-literal
                     * conjuncts compile to atom/scalar outputs that the
                     * lazy OP_FILTER path can't refine into a rowsel —
                     * fall back to the OP_AND tree (ray_optimize already
                     * constant-folds those).  Compute a coarse cost
                     * estimate per conjunct so we can sort cheap-first
                     * (DuckDB ExpressionHeuristics-style); cheap
                     * predicates make subsequent expensive ones evaluate
                     * over a far smaller rowsel. */
                    for (int64_t i = 0; i < k && all_ok; i++) {
                        ray_op_t* p = compile_expr_dag(g, elems[i + 1]);
                        if (!p || p->opcode == OP_CONST) { all_ok = 0; break; }
                        /* Walk the sub-DAG via stack DFS, looking for
                         * any OP_SCAN; tally cost contributions along
                         * the way.  Bound the depth to keep this check
                         * cheap on degenerate trees. */
                        int has_scan = 0;
                        int c = 0;
                        ray_op_t* stk[64];
                        int sp = 0;
                        stk[sp++] = p;
                        while (sp > 0) {
                            ray_op_t* cur = stk[--sp];
                            if (cur->opcode == OP_SCAN) has_scan = 1;
                            /* Coarse per-node cost: comparison ops are
                             * cheap, OP_LIKE / OP_IN / function calls
                             * are expensive.  Matches DuckDB's
                             * ExpressionHeuristics ordering. */
                            switch (cur->opcode) {
                            case OP_LIKE:
                                c += 50;  /* substring-search is ~10× a
                                             plain compare */
                                break;
                            case OP_IN: case OP_NOT_IN:
                                c += 20;
                                break;
                            case OP_NOT:
                                c += 1;
                                break;
                            case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
                            case OP_GT: case OP_GE: case OP_AND: case OP_OR:
                                c += 5;
                                break;
                            case OP_SCAN: case OP_CONST:
                                break;  /* leaves — no compute cost */
                            default:
                                c += 5;
                                /* Unknown op — could be arithmetic that
                                 * faults (OP_DIV / OP_MOD divide-by-zero),
                                 * a user-defined call with side effects,
                                 * or any op whose error behavior depends
                                 * on what the prior conjuncts already
                                 * filtered out.  Keep this conjunct in
                                 * its user-given position so a guard like
                                 * `(!= y 0)` continues to short-circuit
                                 * a later `(< (/ x y) 10)`. */
                                reorder_safe = 0;
                                break;
                            }
                            for (uint8_t a = 0; a < cur->arity && sp < 64; a++)
                                if (cur->inputs[a]) stk[sp++] = cur->inputs[a];
                        }
                        if (!has_scan) { all_ok = 0; break; }
                        compiled[i] = p;
                        cost[i] = c;
                    }
                    if (all_ok) {
                        /* Selection-sort by cost ascending — small k so
                         * O(k²) is fine and avoids dragging a bigger
                         * sort into the query path.  Skipped when the
                         * conjunct set contains a fault-able / side-
                         * effecting op (see `reorder_safe` below) so
                         * user-given short-circuit order is preserved. */
                        int order[64];
                        for (int64_t i = 0; i < k; i++) order[i] = (int)i;
                        if (reorder_safe) {
                            for (int64_t i = 0; i < k - 1; i++) {
                                int min_i = (int)i;
                                for (int64_t j = i + 1; j < k; j++)
                                    if (cost[order[j]] < cost[order[min_i]])
                                        min_i = (int)j;
                                if (min_i != (int)i) {
                                    int tmp = order[i];
                                    order[i] = order[min_i];
                                    order[min_i] = tmp;
                                }
                            }
                        }
                        for (int64_t i = 0; i < k; i++)
                            root = ray_filter(g, root, compiled[order[i]]);
                        and_chained = 1;
                    }
                }
            }
        }
        if (!and_chained) {
            ray_op_t* pred = compile_expr_dag(g, where_expr);
            if (!pred) {
                ray_graph_free(g); ray_release(tbl);
                return ray_error("domain",
                    "WHERE predicate not supported by DAG compiler — "
                    "most common causes: arity mismatch "
                    "(e.g. `(in v)` instead of `(in col v)`), "
                    "unknown function name, unsupported special form, "
                    "or a sub-expression the compiler can't lower");
            }
            if (can_fuse_phase1) {
                fused_pred_op = pred;  /* consumed by ray_filtered_group below */
            } else {
                root = ray_filter(g, root, pred);
            }
        }
    }

    /* Apply NEAREST (ANN/KNN) re-ranking.  Mutually exclusive with
     * asc/desc/by (already rejected above).  Runs after WHERE so the
     * filter feeds the rerank executor directly.  `take k` becomes the
     * target result count; the rerank executor handles the take internally
     * so the bottom-of-function take block is skipped when nearest is set. */
    float* nearest_query_owned = NULL;   /* freed after ray_execute below */
    ray_t* nearest_handle_owned = NULL;  /* HNSW handle kept alive for the
                                          * DAG's lifetime; released after
                                          * ray_execute.  Without this, an
                                          * inline `(ann (hnsw-build ...) ...)`
                                          * drops the handle's rc to 0 before
                                          * exec runs — the rc→0 hook frees
                                          * the index and the ext's stored
                                          * pointer dangles. */
    if (nearest_expr) {
        if (nearest_expr->type != RAY_LIST || ray_len(nearest_expr) < 3) {
            ray_graph_free(g); ray_release(tbl);
            return ray_error("domain",
                "nearest: expected (ann <handle> <query> [ef]) or (knn <col> <query> [metric])");
        }
        int64_t nlen = ray_len(nearest_expr);
        ray_t** nlist = (ray_t**)ray_data(nearest_expr);
        ray_t* head = nlist[0];
        if (head->type != -RAY_SYM) {
            ray_graph_free(g); ray_release(tbl);
            return ray_error("domain",
                "nearest: first element must be the symbol `ann` or `knn`");
        }
        int64_t ann_sym_id = ray_sym_intern("ann", 3);
        int64_t knn_sym_id = ray_sym_intern("knn", 3);

        /* Resolve k from take (default 10). */
        int64_t k_req = 10;
        if (take_expr) {
            ray_t* tv = ray_eval(take_expr);
            if (!tv || RAY_IS_ERR(tv)) {
                ray_graph_free(g); ray_release(tbl);
                return tv ? tv : ray_error("domain", NULL);
            }
            if (tv->type == -RAY_I64)      k_req = tv->i64;
            else if (tv->type == -RAY_I32) k_req = tv->i32;
            else {
                ray_release(tv);
                ray_graph_free(g); ray_release(tbl);
                return ray_error("type", "nearest: take must be an integer atom");
            }
            ray_release(tv);
            if (k_req <= 0) {
                ray_graph_free(g); ray_release(tbl);
                return ray_error("domain", "nearest: take must be positive");
            }
        }

        /* Evaluate the query vector (arg index 2). */
        ray_t* qvec = ray_eval(nlist[2]);
        if (!qvec || RAY_IS_ERR(qvec)) {
            ray_graph_free(g); ray_release(tbl);
            return qvec ? qvec : ray_error("domain", NULL);
        }
        if (!ray_is_vec(qvec) ||
            (qvec->type != RAY_F32 && qvec->type != RAY_F64 &&
             qvec->type != RAY_I32 && qvec->type != RAY_I64)) {
            ray_release(qvec);
            ray_graph_free(g); ray_release(tbl);
            return ray_error("type", "nearest: query must be a numeric vector");
        }
        int32_t dim = (int32_t)qvec->len;
        if (dim <= 0) {
            ray_release(qvec);
            ray_graph_free(g); ray_release(tbl);
            return ray_error("length", "nearest: query vector is empty");
        }

        /* Copy query into a fresh float[] that the DAG op borrows; freed
         * after ray_execute completes. */
        nearest_query_owned = (float*)ray_sys_alloc((size_t)dim * sizeof(float));
        if (!nearest_query_owned) {
            ray_release(qvec);
            ray_graph_free(g); ray_release(tbl);
            return ray_error("oom", NULL);
        }
        switch (qvec->type) {
            case RAY_F32:
                memcpy(nearest_query_owned, ray_data(qvec), (size_t)dim * sizeof(float));
                break;
            case RAY_F64: {
                double* s = (double*)ray_data(qvec);
                for (int32_t j = 0; j < dim; j++) nearest_query_owned[j] = (float)s[j];
                break;
            }
            case RAY_I32: {
                int32_t* s = (int32_t*)ray_data(qvec);
                for (int32_t j = 0; j < dim; j++) nearest_query_owned[j] = (float)s[j];
                break;
            }
            case RAY_I64: {
                int64_t* s = (int64_t*)ray_data(qvec);
                for (int32_t j = 0; j < dim; j++) nearest_query_owned[j] = (float)s[j];
                break;
            }
        }
        ray_release(qvec);

        if (head->i64 == ann_sym_id) {
            ray_t* hobj = ray_eval(nlist[1]);
            if (!hobj || RAY_IS_ERR(hobj)) {
                ray_sys_free(nearest_query_owned);
                ray_graph_free(g); ray_release(tbl);
                return hobj ? hobj : ray_error("domain", NULL);
            }
            if (hobj->type != -RAY_I64 || !(hobj->attrs & RAY_ATTR_HNSW)) {
                ray_release(hobj); ray_sys_free(nearest_query_owned);
                ray_graph_free(g); ray_release(tbl);
                return ray_error("type",
                    "nearest (ann): first arg must be an HNSW handle (from hnsw-build)");
            }
            ray_hnsw_t* idx = (ray_hnsw_t*)(uintptr_t)hobj->i64;
            if (!idx) {
                /* Defensive: attr set but pointer cleared — treat as invalid. */
                ray_release(hobj); ray_sys_free(nearest_query_owned);
                ray_graph_free(g); ray_release(tbl);
                return ray_error("type",
                    "nearest (ann): HNSW handle has been freed");
            }
            if (idx->dim != dim) {
                ray_release(hobj); ray_sys_free(nearest_query_owned);
                ray_graph_free(g); ray_release(tbl);
                return ray_error("length",
                    "nearest (ann): query dim does not match index dim");
            }
            int32_t ef = HNSW_DEFAULT_EF_S;
            if (nlen >= 4) {
                ray_t* ev = ray_eval(nlist[3]);
                if (!ev || RAY_IS_ERR(ev)) {
                    ray_release(hobj); ray_sys_free(nearest_query_owned);
                    ray_graph_free(g); ray_release(tbl);
                    return ev ? ev : ray_error("domain",
                        "nearest (ann): ef expression failed to evaluate");
                }
                if (ev->type == -RAY_I64)      ef = (int32_t)ev->i64;
                else if (ev->type == -RAY_I32) ef = ev->i32;
                else {
                    ray_release(ev); ray_release(hobj);
                    ray_sys_free(nearest_query_owned);
                    ray_graph_free(g); ray_release(tbl);
                    return ray_error("type",
                        "nearest (ann): ef must be an integer atom");
                }
                ray_release(ev);
            }
            if ((int64_t)ef < k_req) ef = (int32_t)k_req;
            root = ray_ann_rerank(g, root, idx, nearest_query_owned, dim, k_req, ef);
            /* Steal the retain from ray_eval — the ext now borrows `idx`
             * through hobj.  Released in the common exit path after
             * ray_execute has completed. */
            nearest_handle_owned = hobj;
        } else if (head->i64 == knn_sym_id) {
            ray_t* col_expr = nlist[1];
            if (col_expr->type != -RAY_SYM) {
                ray_sys_free(nearest_query_owned);
                ray_graph_free(g); ray_release(tbl);
                return ray_error("type",
                    "nearest (knn): first arg must be an unquoted column name");
            }
            int64_t col_sym = col_expr->i64;
            ray_hnsw_metric_t metric = RAY_HNSW_COSINE;
            if (nlen >= 4) {
                ray_t* mv = nlist[3];
                if (mv && mv->type == -RAY_SYM) {
                    int64_t mid = mv->i64;
                    if (mid == ray_sym_find("l2", 2))          metric = RAY_HNSW_L2;
                    else if (mid == ray_sym_find("ip", 2))     metric = RAY_HNSW_IP;
                    else if (mid == ray_sym_find("cosine", 6)) metric = RAY_HNSW_COSINE;
                    else {
                        ray_sys_free(nearest_query_owned);
                        ray_graph_free(g); ray_release(tbl);
                        return ray_error("domain",
                            "nearest (knn): metric must be 'cosine, 'l2, or 'ip");
                    }
                }
            }
            root = ray_knn_rerank(g, root, col_sym, nearest_query_owned, dim, k_req, metric);
        } else {
            ray_sys_free(nearest_query_owned);
            ray_graph_free(g); ray_release(tbl);
            return ray_error("domain",
                "nearest: expected `ann` or `knn` as the first element");
        }
        if (!root) {
            if (nearest_handle_owned) ray_release(nearest_handle_owned);
            ray_sys_free(nearest_query_owned);
            ray_graph_free(g); ray_release(tbl);
            return ray_error("oom", NULL);
        }

        /* When the user didn't specify output columns, project only the
         * source schema — NOT the rerank's synthetic `_dist`.  This keeps
         * `(select {from: t nearest: ...})` shape-compatible with
         * `(select {from: t})`; users who want `_dist` must name it
         * explicitly (e.g. `{from: t d: _dist ...}`).
         *
         * Must handle arbitrarily wide tables (up to ray_select's uint8
         * limit of 255 cols) — a silent 16-col cap would let `_dist`
         * leak through for real-world tables. */
        if (n_out == 0) {
            int64_t src_ncols = ray_table_ncols(tbl);
            if (src_ncols > 255) {
                if (nearest_handle_owned) ray_release(nearest_handle_owned);
                ray_sys_free(nearest_query_owned);
                ray_graph_free(g); ray_release(tbl);
                return ray_error("limit",
                    "nearest: implicit projection exceeds 255 source columns — "
                    "specify output columns explicitly");
            }
            if (src_ncols > 0) {
                ray_op_t** col_ops = (ray_op_t**)ray_sys_alloc(
                    (size_t)src_ncols * sizeof(ray_op_t*));
                if (!col_ops) {
                    if (nearest_handle_owned) ray_release(nearest_handle_owned);
                    ray_sys_free(nearest_query_owned);
                    ray_graph_free(g); ray_release(tbl);
                    return ray_error("oom", NULL);
                }
                int nc = 0;
                bool scan_err = false;
                for (int64_t c = 0; c < src_ncols; c++) {
                    int64_t name_id = ray_table_col_name(tbl, c);
                    ray_t* s = ray_sym_str(name_id);
                    if (!s) continue;
                    ray_op_t* scan_op = ray_scan(g, ray_str_ptr(s));
                    if (!scan_op) { scan_err = true; break; }
                    col_ops[nc++] = scan_op;
                }
                if (scan_err) {
                    ray_sys_free(col_ops);
                    if (nearest_handle_owned) ray_release(nearest_handle_owned);
                    ray_sys_free(nearest_query_owned);
                    ray_graph_free(g); ray_release(tbl);
                    return ray_error("oom", NULL);
                }
                root = ray_select(g, root, col_ops, (uint8_t)nc);
                ray_sys_free(col_ops);
                if (!root) {
                    if (nearest_handle_owned) ray_release(nearest_handle_owned);
                    ray_sys_free(nearest_query_owned);
                    ray_graph_free(g); ray_release(tbl);
                    return ray_error("oom", NULL);
                }
            }
        }
    }

    /* GROUP BY */
    if (by_expr) {
        /* Resolve a "single key" sym id when by_expr is either a
         * scalar -RAY_SYM name or a single-element RAY_SYM vector.
         * The eval_group branch and several downstream sites used to
         * read `by_expr->i64` directly, which is garbage when by_expr
         * is a vector — use by_key_sym instead. */
        int64_t by_key_sym = -1;
        if (by_expr->type == -RAY_SYM && (by_expr->attrs & RAY_ATTR_NAME))
            by_key_sym = by_expr->i64;
        else if (by_expr->type == RAY_SYM && ray_len(by_expr) == 1)
            by_key_sym = ((int64_t*)ray_data(by_expr))[0];

        /* Detect non-aggregate expressions before routing so we can
         * decide whether GUID keys go to the DAG HT path or fall back
         * to eval-level. */
        int any_nonagg = 0;
        if (n_out > 0) {
            for (int64_t i = 0; i + 1 < dict_n; i += 2) {
                int64_t kid = dict_elems[i]->i64;
                if (kid == from_id || kid == where_id || kid == by_id ||
                    kid == take_id || kid == asc_id || kid == desc_id) continue;
                if (!is_group_dag_agg_expr(dict_elems[i + 1])) { any_nonagg = 1; break; }
            }
        }

        /* Decide routing.  LIST/STR always fall to the eval-level
         * grouping because the DAG HT path can't pack them into
         * 8-byte key slots.  GUID is packed via row-indirection in
         * the HT layout (wide_key_mask), so it uses the parallel DAG
         * path *except* for queries with non-aggregate expressions
         * (the non-agg scatter still requires 8-byte-packable key
         * reads through its KEY_READ macro). */
        int use_eval_group = 0;
        if (by_key_sym >= 0) {
            ray_t* key_col = ray_table_get_col(tbl, by_key_sym);
            if (key_col) {
                int8_t kct = key_col->type;
                if (RAY_IS_PARTED(kct)) kct = (int8_t)RAY_PARTED_BASETYPE(kct);
                if (kct == RAY_LIST || kct == RAY_STR)
                    use_eval_group = 1;
                else if (kct == RAY_GUID && (any_nonagg || n_out == 0))
                    /* RAY_GUID routes to eval-level ray_group_fn only
                     * for (a) non-agg expression queries (existing
                     * behavior) and (b) the "no output columns" form
                     * `(select {from: t by: guid})` which otherwise
                     * lands in the DAG no-agg-no-nonagg branch whose
                     * first-occurrence scanner is O(N × n_groups) and
                     * truncates wide keys to 8 bytes via ray_read_sym.
                     * Pure-agg group-bys with GUID keys still take the
                     * DAG path (exec_group handles wide keys correctly
                     * and stays parallel / segment-streamed on parted
                     * tables). */
	                    use_eval_group = 1;
	            }
	        }
        if (!use_eval_group && by_expr->type == RAY_SYM && ray_len(by_expr) > 1) {
            int64_t nk = ray_len(by_expr);
            int64_t* sym_ids = (int64_t*)ray_data(by_expr);
            for (int64_t k = 0; k < nk; k++) {
                ray_t* key_col = ray_table_get_col(tbl, sym_ids[k]);
                if (!key_col) continue;
                int8_t kct = key_col->type;
                if (RAY_IS_PARTED(kct)) kct = (int8_t)RAY_PARTED_BASETYPE(kct);
                if (kct == RAY_LIST || kct == RAY_STR) {
                    use_eval_group = 1;
                    break;
                }
            }
        }
        /* Non-aggregation expressions (arithmetic, lambda, etc.) are
         * handled post-DAG: aggs go through the parallel GROUP pipeline,
         * then non-agg results are evaluated on the full table and
         * scattered per-group into LIST columns.  The scatter block
         * only handles single scalar-key by-clauses — for multi-key
         * or computed-key groupings, fall back to eval-level so the
         * non-agg scatter has a well-defined row→group mapping. */
        if (!use_eval_group && any_nonagg) {
            /* Fast path requires a single scalar-named key column.
             * Multi-key and computed-key by-clauses with non-agg
             * expressions are not yet supported. */
            int single_scalar_key = 0;
            if (by_expr->type == -RAY_SYM && (by_expr->attrs & RAY_ATTR_NAME)) {
                single_scalar_key = 1;
            } else if (by_expr->type == RAY_SYM && ray_len(by_expr) == 1) {
                single_scalar_key = 1;
            }
            if (!single_scalar_key) {
                ray_graph_free(g); ray_release(tbl);
                return ray_error("nyi", "non-agg expression with multi-key or computed group key");
            }
        }
        if (use_eval_group) {
            /* Apply WHERE filter first (if any), then eval-level groupby */
            ray_t* eval_tbl = tbl;
            if (where_expr) {
                root = ray_optimize(g, root);
                ray_t* fres = ray_execute(g, root);
                ray_graph_free(g); g = NULL;
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                if (ray_is_lazy(fres)) fres = ray_lazy_materialize(fres);
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                eval_tbl = fres;
            } else {
                ray_graph_free(g); g = NULL;
            }
            if (by_key_sym < 0 && by_expr->type == RAY_SYM && ray_len(by_expr) > 1) {
                int64_t nk = ray_len(by_expr);
                int64_t* key_syms = (int64_t*)ray_data(by_expr);
                int64_t nrows = ray_table_nrows(eval_tbl);
                ray_t* key_cols[16];
                if (nk <= 0 || nk > 16) {
                    if (eval_tbl != tbl) ray_release(eval_tbl);
                    ray_release(tbl);
                    return ray_error("domain", "eval-level multi-key groupby requires 1..16 keys");
                }
                for (int64_t k = 0; k < nk; k++) {
                    key_cols[k] = ray_table_get_col(eval_tbl, key_syms[k]);
                    if (!key_cols[k]) {
                        if (eval_tbl != tbl) ray_release(eval_tbl);
                        ray_release(tbl);
                        return ray_error("domain", "group key column not found");
                    }
                }

                ray_t* composite_keys = ray_list_new(nrows);
                if (!composite_keys || RAY_IS_ERR(composite_keys)) {
                    if (eval_tbl != tbl) ray_release(eval_tbl);
                    ray_release(tbl);
                    return composite_keys ? composite_keys : ray_error("oom", NULL);
                }
                for (int64_t r = 0; r < nrows; r++) {
                    ray_t* row_key = ray_list_new(nk);
                    if (!row_key || RAY_IS_ERR(row_key)) {
                        ray_release(composite_keys);
                        if (eval_tbl != tbl) ray_release(eval_tbl);
                        ray_release(tbl);
                        return row_key ? row_key : ray_error("oom", NULL);
                    }
                    for (int64_t k = 0; k < nk; k++) {
                        int alloc = 0;
                        ray_t* cell = collection_elem(key_cols[k], r, &alloc);
                        if (!cell || RAY_IS_ERR(cell)) {
                            ray_release(row_key);
                            ray_release(composite_keys);
                            if (eval_tbl != tbl) ray_release(eval_tbl);
                            ray_release(tbl);
                            return cell ? cell : ray_error("domain", NULL);
                        }
                        row_key = ray_list_append(row_key, cell);
                        if (alloc) ray_release(cell);
                        if (!row_key || RAY_IS_ERR(row_key)) {
                            ray_release(composite_keys);
                            if (eval_tbl != tbl) ray_release(eval_tbl);
                            ray_release(tbl);
                            return row_key ? row_key : ray_error("oom", NULL);
                        }
                    }
                    composite_keys = ray_list_append(composite_keys, row_key);
                    ray_release(row_key);
                    if (!composite_keys || RAY_IS_ERR(composite_keys)) {
                        if (eval_tbl != tbl) ray_release(eval_tbl);
                        ray_release(tbl);
                        return composite_keys ? composite_keys : ray_error("oom", NULL);
                    }
                }

                ray_t* groups_dict = ray_group_fn(composite_keys);
                ray_release(composite_keys);
                if (!groups_dict || RAY_IS_ERR(groups_dict)) {
                    if (eval_tbl != tbl) ray_release(eval_tbl);
                    ray_release(tbl);
                    return groups_dict ? groups_dict : ray_error("domain", NULL);
                }
                ray_t* groups = groups_to_pair_list(groups_dict);
                ray_release(groups_dict);
                if (!groups || RAY_IS_ERR(groups)) {
                    if (eval_tbl != tbl) ray_release(eval_tbl);
                    ray_release(tbl);
                    return groups ? groups : ray_error("domain", NULL);
                }
                int64_t n_groups = ray_len(groups) / 2;

                int n_agg_out = 0;
                int64_t agg_names[16];
                ray_t* agg_results[16] = {0};
                for (int64_t i = 0; i + 1 < dict_n && n_agg_out < 16; i += 2) {
                    int64_t kid = dict_elems[i]->i64;
                    if (kid == from_id || kid == where_id || kid == by_id ||
                        kid == take_id || kid == asc_id || kid == desc_id) continue;
                    ray_t* val_expr_item = dict_elems[i + 1];

                    /* Per-group count(distinct) — bypass full ray_eval per
                     * group and dispatch directly to exec_count_distinct on
                     * each group's slice.  Same kernel the standalone
                     * `(count (distinct col))` fast path uses. */
                    ray_t* cd_inner = match_count_distinct(val_expr_item);
                    if (cd_inner) {
                        ray_t* per_group = count_distinct_per_group_groups(
                            cd_inner, eval_tbl, groups, n_groups);
                        if (!per_group || RAY_IS_ERR(per_group)) {
                            for (int ai = 0; ai < n_agg_out; ai++) if (agg_results[ai]) ray_release(agg_results[ai]);
                            ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                            return per_group ? per_group : ray_error("domain", NULL);
                        }
                        agg_names[n_agg_out] = kid;
                        agg_results[n_agg_out] = per_group;
                        n_agg_out++;
                        continue;
                    }

                    if (is_streaming_aggr_unary_call(val_expr_item)) {
                        ray_t** agg_elems = (ray_t**)ray_data(val_expr_item);
                        ray_t* agg_fn_name = agg_elems[0];
                        ray_t* agg_col_expr = agg_elems[1];
                        ray_t* src_col_val = NULL;
                        if (agg_col_expr->type == -RAY_SYM && (agg_col_expr->attrs & RAY_ATTR_NAME)) {
                            src_col_val = ray_table_get_col(eval_tbl, agg_col_expr->i64);
                            if (src_col_val) ray_retain(src_col_val);
                        }
                        if (!src_col_val) {
                            src_col_val = ray_eval(agg_col_expr);
                            if (!src_col_val || RAY_IS_ERR(src_col_val)) {
                                for (int ai = 0; ai < n_agg_out; ai++) if (agg_results[ai]) ray_release(agg_results[ai]);
                                ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                                return src_col_val ? src_col_val : ray_error("domain", NULL);
                            }
                        }

                        ray_t* agg_vec = NULL;
                        ray_t** grp_items = (ray_t**)ray_data(groups);
                        for (int64_t gi = 0; gi < n_groups; gi++) {
                            ray_t* idx_list = grp_items[gi * 2 + 1];
                            ray_t* subset = ray_at_fn(src_col_val, idx_list);
                            if (!subset || RAY_IS_ERR(subset)) continue;
                            ray_t* agg_val = NULL;
                            ray_t* fn_obj = ray_env_get(agg_fn_name->i64);
                            if (fn_obj && fn_obj->type == RAY_UNARY) {
                                ray_unary_fn uf = (ray_unary_fn)(uintptr_t)fn_obj->i64;
                                agg_val = uf(subset);
                            }
                            ray_release(subset);
                            if (!agg_val || RAY_IS_ERR(agg_val)) continue;
                            if (!agg_vec) {
                                int8_t vt = -(agg_val->type);
                                agg_vec = ray_vec_new(vt, n_groups);
                                if (!agg_vec || RAY_IS_ERR(agg_vec)) { ray_release(agg_val); break; }
                                agg_vec->len = n_groups;
                            }
                            store_typed_elem(agg_vec, gi, agg_val);
                            ray_release(agg_val);
                        }
                        ray_release(src_col_val);
                        agg_names[n_agg_out] = kid;
                        agg_results[n_agg_out] = agg_vec;
                        n_agg_out++;
                    } else {
                        ray_t* per_group = nonagg_eval_per_group(val_expr_item, eval_tbl, groups, n_groups);
                        if (!per_group || RAY_IS_ERR(per_group)) {
                            for (int ai = 0; ai < n_agg_out; ai++) if (agg_results[ai]) ray_release(agg_results[ai]);
                            ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                            return per_group ? per_group : ray_error("domain", NULL);
                        }
                        agg_names[n_agg_out] = kid;
                        agg_results[n_agg_out] = per_group;
                        n_agg_out++;
                    }
                }

                ray_t* result = ray_table_new(nk + n_agg_out);
                if (!result || RAY_IS_ERR(result)) {
                    for (int ai = 0; ai < n_agg_out; ai++) if (agg_results[ai]) ray_release(agg_results[ai]);
                    ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                    return result ? result : ray_error("oom", NULL);
                }
                ray_t** grp_items = (ray_t**)ray_data(groups);
                for (int64_t k = 0; k < nk; k++) {
                    ray_t* src = key_cols[k];
                    int8_t kt = src->type;
                    if (RAY_IS_PARTED(kt)) kt = (int8_t)RAY_PARTED_BASETYPE(kt);
                    ray_t* key_vec = NULL;
                    if (kt == RAY_STR) {
                        key_vec = ray_vec_new(RAY_STR, n_groups);
                        for (int64_t gi = 0; gi < n_groups && key_vec && !RAY_IS_ERR(key_vec); gi++) {
                            ray_t* row_key = grp_items[gi * 2];
                            ray_t* cell = (row_key && row_key->type == RAY_LIST && k < row_key->len)
                                        ? ((ray_t**)ray_data(row_key))[k] : NULL;
                            const char* sp = cell ? ray_str_ptr(cell) : "";
                            size_t slen = cell ? ray_str_len(cell) : 0;
                            key_vec = ray_str_vec_append(key_vec, sp ? sp : "", sp ? slen : 0);
                        }
                    } else {
                        key_vec = (kt == RAY_SYM)
                                ? ray_sym_vec_new(src->attrs & RAY_SYM_W_MASK, n_groups)
                                : ray_vec_new(kt, n_groups);
                        if (key_vec && !RAY_IS_ERR(key_vec)) {
                            key_vec->len = n_groups;
                            memset(ray_data(key_vec), 0, (size_t)n_groups * ray_sym_elem_size(kt, key_vec->attrs));
                            for (int64_t gi = 0; gi < n_groups; gi++) {
                                ray_t* row_key = grp_items[gi * 2];
                                ray_t* cell = (row_key && row_key->type == RAY_LIST && k < row_key->len)
                                            ? ((ray_t**)ray_data(row_key))[k] : NULL;
                                if (cell) store_typed_elem(key_vec, gi, cell);
                            }
                        }
                    }
                    if (!key_vec || RAY_IS_ERR(key_vec)) {
                        for (int ai = 0; ai < n_agg_out; ai++) if (agg_results[ai]) ray_release(agg_results[ai]);
                        ray_release(result); ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                        return key_vec ? key_vec : ray_error("oom", NULL);
                    }
                    result = ray_table_add_col(result, key_syms[k], key_vec);
                    ray_release(key_vec);
                    if (RAY_IS_ERR(result)) {
                        for (int ai = 0; ai < n_agg_out; ai++) if (agg_results[ai]) ray_release(agg_results[ai]);
                        ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                        return result;
                    }
                }
                for (int ai = 0; ai < n_agg_out; ai++) {
                    if (agg_results[ai]) {
                        result = ray_table_add_col(result, agg_names[ai], agg_results[ai]);
                        ray_release(agg_results[ai]);
                        if (RAY_IS_ERR(result)) { ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return result; }
                    }
                }

                ray_release(groups);
                if (eval_tbl != tbl) ray_release(eval_tbl);
                ray_release(tbl);
                return apply_sort_take(result, dict_elems, dict_n, asc_id, desc_id, take_id);
            }

	            /* eval_group path supports only simple scalar / [col] by-forms;
	             * computed keys shouldn't land here. */
	            if (by_key_sym < 0) {
	                if (eval_tbl != tbl) ray_release(eval_tbl);
	                ray_release(tbl);
	                return ray_error("nyi", "eval-level groupby requires scalar key");
	            }
            ray_t* key_col = ray_table_get_col(eval_tbl, by_key_sym);

            /* Fast path: (select {from: t by: k}) with no aggs and
             * no non-agg expressions — we only need first-of-group
             * for each non-key column, not full per-group index
             * lists. Scan the key column once, record the first
             * row index of each distinct key in a hash table, then
             * gather that index list from every other column. This
             * avoids ray_group_fn's per-group ray_vec_append churn
             * which dominated the cost on 10M-row / 1M-group
             * workloads. */
            if (n_out == 0 && key_col && key_col->type == RAY_GUID) {
                int64_t n = key_col->len;
                const uint8_t* kb = (const uint8_t*)ray_data(key_col);
                uint32_t cap = 64;
                while ((uint64_t)cap < (uint64_t)n * 2 && cap < (1u << 28)) cap <<= 1;
                uint32_t mask = cap - 1;
                ray_t* ht_hdr = ray_alloc((size_t)cap * sizeof(uint32_t));
                if (!ht_hdr) { if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                uint32_t* ht = (uint32_t*)ray_data(ht_hdr);
                memset(ht, 0xFF, (size_t)cap * sizeof(uint32_t));

                int64_t fi_cap = n < 1024 ? 1024 : (n < (1 << 20) ? n : (1 << 20));
                if (fi_cap < 256) fi_cap = 256;
                ray_t* fi_hdr = ray_alloc((size_t)fi_cap * sizeof(int64_t));
                if (!fi_hdr) { ray_free(ht_hdr); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                int64_t* fi = (int64_t*)ray_data(fi_hdr);
                int64_t ngroups = 0;

                for (int64_t i = 0; i < n; i++) {
                    if ((i & 65535) == 0) {
                        if (ray_interrupted()) {
                            ray_free(fi_hdr);
                            ray_free(ht_hdr);
                            if (eval_tbl != tbl) ray_release(eval_tbl);
                            ray_release(tbl);
                            return ray_error("cancel", "interrupted");
                        }
                        ray_progress_update("select", "by: first-of-group",
                                            (uint64_t)i, (uint64_t)n);
                    }
                    const uint8_t* cur = kb + (size_t)i * 16;
                    uint64_t h; memcpy(&h, cur, 8); h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
                    uint32_t slot = (uint32_t)(h & mask);
                    uint32_t gi = UINT32_MAX;
                    while (ht[slot] != UINT32_MAX) {
                        uint32_t cand = ht[slot];
                        if (memcmp(kb + (size_t)fi[cand] * 16, cur, 16) == 0) { gi = cand; break; }
                        slot = (slot + 1) & mask;
                    }
                    if (gi == UINT32_MAX) {
                        if (ngroups >= fi_cap) {
                            int64_t new_cap = fi_cap * 2;
                            ray_t* new_hdr = ray_alloc((size_t)new_cap * sizeof(int64_t));
                            if (!new_hdr) { ray_free(fi_hdr); ray_free(ht_hdr); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                            memcpy(ray_data(new_hdr), fi, (size_t)ngroups * sizeof(int64_t));
                            ray_free(fi_hdr);
                            fi_hdr = new_hdr;
                            fi = (int64_t*)ray_data(fi_hdr);
                            fi_cap = new_cap;
                        }
                        fi[ngroups] = i;
                        ht[slot] = (uint32_t)ngroups;
                        ngroups++;
                    }
                }
                ray_free(ht_hdr);

                /* Build result table: key column first (gathered from
                 * the original at fi[]), then every other column the
                 * same way. Allocation failures and width mismatches
                 * must propagate — partial results silently dropping
                 * columns would be a correctness bug. */
                int64_t nc_src = ray_table_ncols(eval_tbl);
                ray_t* res = ray_table_new(nc_src);
                ray_t* first_err = NULL;
                if (!res || RAY_IS_ERR(res)) {
                    first_err = res && RAY_IS_ERR(res) ? res : ray_error("oom", NULL);
                    res = NULL;
                    goto fog_cleanup;
                }

                for (int64_t pass = 0; pass < nc_src + 1 && !first_err; pass++) {
                    int64_t cn;
                    if (pass == 0) cn = by_key_sym;
                    else {
                        cn = ray_table_col_name(eval_tbl, pass - 1);
                        if (cn == by_key_sym) continue;
                    }
                    ray_t* sc = ray_table_get_col(eval_tbl, cn);
                    if (!sc) continue;
                    ray_t* dst = NULL;
                    int8_t sct = sc->type;
                    if (RAY_IS_PARTED(sct)) sct = (int8_t)RAY_PARTED_BASETYPE(sct);

                    if (sct == RAY_STR) {
                        dst = ray_vec_new(RAY_STR, ngroups);
                        for (int64_t gi = 0; gi < ngroups && dst && !RAY_IS_ERR(dst); gi++) {
                            size_t slen = 0;
                            const char* sp = ray_str_vec_get(sc, fi[gi], &slen);
                            dst = ray_str_vec_append(dst, sp ? sp : "", sp ? slen : 0);
                        }
                    } else if (sct == RAY_LIST) {
                        dst = ray_list_new((int32_t)ngroups);
                        if (dst && !RAY_IS_ERR(dst)) {
                            ray_t** sitems = (ray_t**)ray_data(sc);
                            ray_t** dout = (ray_t**)ray_data(dst);
                            for (int64_t gi = 0; gi < ngroups; gi++) {
                                dout[gi] = sitems[fi[gi]];
                                ray_retain(dout[gi]);
                            }
                            dst->len = ngroups;
                        }
                    } else if (sct == RAY_SYM) {
                        /* Preserve the source sym-width from attrs so
                         * narrow sym columns (1/2/4-byte indices)
                         * memcpy the same esz on both sides. */
                        dst = ray_sym_vec_new(sc->attrs & RAY_SYM_W_MASK, ngroups);
                        if (dst && !RAY_IS_ERR(dst)) {
                            dst->len = ngroups;
                            uint8_t esz = ray_sym_elem_size(sct, dst->attrs);
                            const char* sb = (const char*)ray_data(sc);
                            char* db = (char*)ray_data(dst);
                            bool src_has_nulls = (sc->attrs & RAY_ATTR_HAS_NULLS) != 0;
                            if (src_has_nulls) {
                                for (int64_t gi = 0; gi < ngroups; gi++) {
                                    memcpy(db + (size_t)gi * esz,
                                           sb + (size_t)fi[gi] * esz, esz);
                                    if (ray_vec_is_null(sc, fi[gi]))
                                        ray_vec_set_null(dst, gi, true);
                                }
                            } else {
                                for (int64_t gi = 0; gi < ngroups; gi++)
                                    memcpy(db + (size_t)gi * esz,
                                           sb + (size_t)fi[gi] * esz, esz);
                            }
                        }
                    } else {
                        dst = ray_vec_new(sct, ngroups);
                        if (dst && !RAY_IS_ERR(dst)) {
                            dst->len = ngroups;
                            uint8_t esz = ray_sym_elem_size(sct, sc->attrs);
                            const char* sb = (const char*)ray_data(sc);
                            char* db = (char*)ray_data(dst);
                            bool src_has_nulls = (sc->attrs & RAY_ATTR_HAS_NULLS) != 0;
                            if (src_has_nulls) {
                                for (int64_t gi = 0; gi < ngroups; gi++) {
                                    memcpy(db + (size_t)gi * esz,
                                           sb + (size_t)fi[gi] * esz, esz);
                                    if (ray_vec_is_null(sc, fi[gi]))
                                        ray_vec_set_null(dst, gi, true);
                                }
                            } else {
                                for (int64_t gi = 0; gi < ngroups; gi++)
                                    memcpy(db + (size_t)gi * esz,
                                           sb + (size_t)fi[gi] * esz, esz);
                            }
                        }
                    }

                    if (!dst || RAY_IS_ERR(dst)) {
                        first_err = (dst && RAY_IS_ERR(dst)) ? dst : ray_error("oom", NULL);
                        if (dst && !RAY_IS_ERR(dst)) ray_release(dst);
                        break;
                    }
                    res = ray_table_add_col(res, cn, dst);
                    ray_release(dst);
                    if (RAY_IS_ERR(res)) { first_err = res; res = NULL; break; }
                }

            fog_cleanup:
                ray_free(fi_hdr);
                if (eval_tbl != tbl) ray_release(eval_tbl);
                ray_release(tbl);
                if (first_err) {
                    if (res) ray_release(res);
                    return first_err;
                }
                return apply_sort_take(res, dict_elems, dict_n, asc_id, desc_id, take_id);
            }

            ray_t* groups_dict = ray_group_fn(key_col);
            if (RAY_IS_ERR(groups_dict)) { if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return groups_dict; }
            /* Flatten the dict into the legacy [k0,v0,…] interleaved LIST
             * representation that the rest of this branch was written for. */
            ray_t* groups = groups_to_pair_list(groups_dict);
            ray_release(groups_dict);
            if (RAY_IS_ERR(groups)) { if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return groups; }

            int64_t gn = ray_len(groups);
            int64_t n_groups = gn / 2;

            /* Empty groups with no explicit aggs: return empty table with full schema */
            if (n_groups == 0 && n_out == 0) {
                ray_release(groups);
                int64_t nc0 = ray_table_ncols(eval_tbl);
                ray_t* empty = ray_table_new(nc0);
                if (!RAY_IS_ERR(empty)) {
                    /* Key column first */
                    { ray_t* sc = ray_table_get_col(eval_tbl, by_key_sym);
                      if (sc) {
                        ray_t* ev = ray_vec_new(sc->type, 0);
                        if (ev && !RAY_IS_ERR(ev)) { empty = ray_table_add_col(empty, by_key_sym, ev); ray_release(ev); }
                      }
                    }
                    for (int64_t c = 0; c < nc0; c++) {
                        int64_t cn = ray_table_col_name(eval_tbl, c);
                        if (cn == by_key_sym) continue;
                        ray_t* sc = ray_table_get_col_idx(eval_tbl, c);
                        ray_t* ev = (sc->type == RAY_STR) ? ray_vec_new(RAY_STR, 0) :
                                    (sc->type == RAY_LIST) ? ray_list_new(0) :
                                    ray_vec_new(sc->type, 0);
                        if (ev && !RAY_IS_ERR(ev)) { empty = ray_table_add_col(empty, cn, ev); ray_release(ev); }
                    }
                }
                if (eval_tbl != tbl) ray_release(eval_tbl);
                ray_release(tbl);
                return empty;
            }

            /* Collect aggregation results */
            int n_agg_out = 0;
            int64_t agg_names[16];
            ray_t* agg_results[16];
            for (int64_t i = 0; i + 1 < dict_n && n_agg_out < 16; i += 2) {
                int64_t kid = dict_elems[i]->i64;
                if (kid == from_id || kid == where_id || kid == by_id || kid == take_id || kid == asc_id || kid == desc_id) continue;
                ray_t* val_expr_item = dict_elems[i + 1];

                /* Per-group count(distinct) — bypass full ray_eval per
                 * group and dispatch directly to exec_count_distinct. */
                {
                    ray_t* cd_inner = match_count_distinct(val_expr_item);
                    if (cd_inner) {
                        ray_t* per_group = count_distinct_per_group_groups(
                            cd_inner, eval_tbl, groups, n_groups);
                        if (!per_group || RAY_IS_ERR(per_group)) {
                            for (int ai = 0; ai < n_agg_out; ai++) { if (agg_results[ai]) ray_release(agg_results[ai]); }
                            ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                            return per_group ? per_group : ray_error("domain", NULL);
                        }
                        agg_names[n_agg_out] = kid;
                        agg_results[n_agg_out] = per_group;
                        n_agg_out++;
                        continue;
                    }
                }

                if (is_streaming_aggr_unary_call(val_expr_item)) {
                    /* Streaming-style per-group AGG branch.  Accepts both
                     * the resolve_agg_opcode whitelist (sum/avg/min/max/...)
                     * and the broader RAY_FN_AGGR + RAY_UNARY set
                     * (med/dev/var/stddev/...) — for the eval-fallback path
                     * the only thing the body needs is a unary fn pointer
                     * to call directly with the per-group slice. */
                    ray_t** agg_elems = (ray_t**)ray_data(val_expr_item);
                    ray_t* agg_fn_name = agg_elems[0];
                    ray_t* agg_col_expr = agg_elems[1];

                    /* Resolve source column from filtered table */
                    ray_t* src_col_val = NULL;
                    if (agg_col_expr->type == -RAY_SYM && (agg_col_expr->attrs & RAY_ATTR_NAME)) {
                        src_col_val = ray_table_get_col(eval_tbl, agg_col_expr->i64);
                        if (src_col_val) ray_retain(src_col_val);
                    }
                    if (!src_col_val) {
                        src_col_val = ray_eval(agg_col_expr);
                        if (RAY_IS_ERR(src_col_val)) {
                            for (int ai = 0; ai < n_agg_out; ai++) { if (agg_results[ai]) ray_release(agg_results[ai]); }
                            ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return src_col_val;
                        }
                    }

                    /* For each group, compute aggregation */
                    ray_t* agg_vec = NULL;
                    ray_t** grp_items = (ray_t**)ray_data(groups);
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        ray_t* idx_list = grp_items[gi * 2 + 1];
                        ray_t* subset = ray_at_fn(src_col_val, idx_list);
                        if (RAY_IS_ERR(subset)) continue;
                        ray_t* agg_val = NULL;
                        ray_t* fn_obj = ray_env_get(agg_fn_name->i64);
                        if (fn_obj && fn_obj->type == RAY_UNARY) {
                            ray_unary_fn uf = (ray_unary_fn)(uintptr_t)fn_obj->i64;
                            agg_val = uf(subset);
                        }
                        ray_release(subset);
                        if (!agg_val || RAY_IS_ERR(agg_val)) continue;

                        if (!agg_vec) {
                            int8_t vt = -(agg_val->type);
                            agg_vec = ray_vec_new(vt, n_groups);
                            if (RAY_IS_ERR(agg_vec)) { ray_release(agg_val); break; }
                            agg_vec->len = n_groups;
                        }
                        store_typed_elem(agg_vec, gi, agg_val);
                        ray_release(agg_val);
                    }
                    ray_release(src_col_val);
                    agg_names[n_agg_out] = kid;
                    agg_results[n_agg_out] = agg_vec;
                    n_agg_out++;
                } else {
                    if (is_agg_expr(val_expr_item)) {
                        ray_t* per_group = nonagg_eval_per_group(
                            val_expr_item, eval_tbl, groups, n_groups);
                        if (RAY_IS_ERR(per_group)) {
                            for (int ai = 0; ai < n_agg_out; ai++) { if (agg_results[ai]) ray_release(agg_results[ai]); }
                            ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                            return per_group;
                        }
                        agg_names[n_agg_out] = kid;
                        agg_results[n_agg_out] = per_group;
                        n_agg_out++;
                        continue;
                    }

                    /* Non-aggregation expression: evaluate on full table,
                     * then gather per-group subsets into a LIST column
                     * (non-agg produces list-of-vectors). */
                    if (ray_env_push_scope() != RAY_OK) {
                        for (int ai = 0; ai < n_agg_out; ai++) { if (agg_results[ai]) ray_release(agg_results[ai]); }
                        ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                        return ray_error("oom", NULL);
                    }
                    expr_bind_table_names(val_expr_item, eval_tbl);
                    ray_t* full_val = ray_eval(val_expr_item);
                    ray_env_pop_scope();
                    if (RAY_IS_ERR(full_val)) {
                        for (int ai = 0; ai < n_agg_out; ai++) { if (agg_results[ai]) ray_release(agg_results[ai]); }
                        ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return full_val;
                    }

                    /* Build LIST column: pre-allocate, then gather per group.
                     * Direct pointer assignment avoids ray_list_append overhead. */
                    ray_t* list_col = ray_alloc(n_groups * sizeof(ray_t*));
                    if (!list_col || RAY_IS_ERR(list_col)) {
                        ray_release(full_val);
                        for (int ai = 0; ai < n_agg_out; ai++) { if (agg_results[ai]) ray_release(agg_results[ai]); }
                        ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                        return ray_error("oom", NULL);
                    }
                    list_col->type = RAY_LIST;
                    /* Track filled length incrementally — see the DAG
                     * scatter above for rationale (no memset, exact
                     * cleanup via v->len walk in ray_release). */
                    list_col->len = 0;
                    ray_t** list_out = (ray_t**)ray_data(list_col);

                    /* Decide per-group disposition of full_val:
                     *   - expression references a column → result must
                     *     be row-aligned; a typed-vec or LIST whose len
                     *     matches eval_tbl's nrows → gather, otherwise
                     *     that's a genuine bug and we error out.
                     *   - expression is constant (no column refs) →
                     *     broadcast as-is to every group cell. */
                    int64_t eval_nrows = ray_table_nrows(eval_tbl);
                    int refs_column = expr_refs_row_column(val_expr_item, eval_tbl);
                    int is_indexable =
                        ray_is_vec(full_val) || full_val->type == RAY_LIST;
                    int full_is_row_aligned =
                        is_indexable && full_val->len == eval_nrows;

                    if (refs_column && !full_is_row_aligned) {
                        /* Non-streaming aggregation fallback: the full-table
                         * eval didn't produce a row-aligned shape (e.g. a
                         * user lambda returned a scalar from a vector arg),
                         * so collect per-group and post-apply the expression
                         * to each group's slice.  Each cell can be any shape;
                         * homogeneous-scalar cells collapse to a typed vec. */
                        ray_release(full_val);
                        ray_release(list_col);  /* len=0, walks nothing */
                        ray_t* per_group = nonagg_eval_per_group(
                            val_expr_item, eval_tbl, groups, n_groups);
                        if (RAY_IS_ERR(per_group)) {
                            for (int ai = 0; ai < n_agg_out; ai++) { if (agg_results[ai]) ray_release(agg_results[ai]); }
                            ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                            return per_group;
                        }
                        /* core produces typed vec or list as appropriate */
                        agg_names[n_agg_out] = kid;
                        agg_results[n_agg_out] = per_group;
                        n_agg_out++;
                        continue;
                    }

                    ray_t** gi_items = (ray_t**)ray_data(groups);
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        ray_t* idx_list = gi_items[gi * 2 + 1];
                        ray_t* cell;
                        if (full_is_row_aligned) {
                            cell = gather_by_idx(full_val,
                                (int64_t*)ray_data(idx_list), idx_list->len);
                        } else {
                            /* Pure constant (no column refs) → broadcast */
                            ray_retain(full_val);
                            cell = full_val;
                        }
                        list_out[gi] = cell;
                        list_col->len = gi + 1;  /* commit slot */
                    }
                    ray_release(full_val);
                    agg_names[n_agg_out] = kid;
                    agg_results[n_agg_out] = list_col;
                    n_agg_out++;
                }
            }

            /* Build result table: key column + aggregation columns */
            ray_t* result = ray_table_new(1 + n_agg_out);
            if (RAY_IS_ERR(result)) { ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return result; }

            /* Key column: build a typed vector matching the source column type */
            ray_t** grp_items = (ray_t**)ray_data(groups);
            ray_t* key_col_src = ray_table_get_col(eval_tbl, by_key_sym);
            {
                int8_t ktype = key_col_src ? key_col_src->type : RAY_I64;
                if (RAY_IS_PARTED(ktype)) ktype = (int8_t)RAY_PARTED_BASETYPE(ktype);
                ray_t* key_vec;
                if (ktype == RAY_STR) {
                    key_vec = ray_vec_new(RAY_STR, n_groups);
                    for (int64_t gi = 0; gi < n_groups && key_vec && !RAY_IS_ERR(key_vec); gi++) {
                        ray_t* k = grp_items[gi * 2];
                        const char* sp = ray_str_ptr(k);
                        size_t slen = ray_str_len(k);
                        key_vec = ray_str_vec_append(key_vec, sp ? sp : "", sp ? slen : 0);
                    }
                } else {
                    uint8_t kattrs = key_col_src ? key_col_src->attrs : 0;
                    if (ktype == RAY_SYM)
                        key_vec = ray_sym_vec_new(kattrs & RAY_SYM_W_MASK, n_groups);
                    else
                        key_vec = ray_vec_new(ktype, n_groups);
                    if (key_vec && !RAY_IS_ERR(key_vec)) {
                        key_vec->len = n_groups;
                        /* Zero-fill data region so skipped GUID/null slots are safe */
                        memset(ray_data(key_vec), 0, (size_t)n_groups * ray_sym_elem_size(ktype, key_vec->attrs));
                        for (int64_t gi = 0; gi < n_groups; gi++)
                            store_typed_elem(key_vec, gi, grp_items[gi * 2]);
                    }
                }
                if (!key_vec || RAY_IS_ERR(key_vec)) {
                    for (int i = 0; i < n_agg_out; i++) { if (agg_results[i]) ray_release(agg_results[i]); }
                    ray_release(result); ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl);
                    return key_vec ? key_vec : ray_error("oom", NULL);
                }
                result = ray_table_add_col(result, by_key_sym, key_vec);
                ray_release(key_vec);
            }

            for (int i = 0; i < n_agg_out; i++) {
                if (agg_results[i])
                    result = ray_table_add_col(result, agg_names[i], agg_results[i]);
                if (agg_results[i]) ray_release(agg_results[i]);
            }

            /* No explicit aggs: gather first-of-group for all non-key columns */
            if (n_agg_out == 0 && n_groups > 0) {
                ray_t** gi_items = (ray_t**)ray_data(groups);
                /* Collect first index per group */
                int64_t fi_stack[256];
                ray_t* fi_hdr = NULL;
                int64_t* fi = (n_groups <= 256) ? fi_stack : NULL;
                if (!fi) {
                    fi_hdr = ray_alloc((size_t)n_groups * sizeof(int64_t));
                    if (!fi_hdr) { ray_release(result); ray_release(groups); if (eval_tbl != tbl) ray_release(eval_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                    fi = (int64_t*)ray_data(fi_hdr);
                }
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    ray_t* il = gi_items[gi * 2 + 1];
                    int a = 0; ray_t* i0 = collection_elem(il, 0, &a);
                    fi[gi] = as_i64(i0);
                    if (a) ray_release(i0);
                }
                int64_t nc = ray_table_ncols(eval_tbl);
                for (int64_t c = 0; c < nc && !RAY_IS_ERR(result); c++) {
                    int64_t cn = ray_table_col_name(eval_tbl, c);
                    if (cn == by_key_sym) continue;
                    ray_t* sc = ray_table_get_col_idx(eval_tbl, c);
                    ray_t* dst = NULL;
                    if (sc->type == RAY_STR) {
                        dst = ray_vec_new(RAY_STR, n_groups);
                        bool src_has_nulls = (sc->attrs & RAY_ATTR_HAS_NULLS) != 0;
                        if (src_has_nulls) {
                            for (int64_t gi = 0; gi < n_groups && dst && !RAY_IS_ERR(dst); gi++) {
                                if (ray_vec_is_null(sc, fi[gi])) {
                                    dst = ray_str_vec_append(dst, "", 0);
                                    if (dst && !RAY_IS_ERR(dst))
                                        ray_vec_set_null(dst, dst->len - 1, true);
                                } else {
                                    size_t slen = 0;
                                    const char* sp = ray_str_vec_get(sc, fi[gi], &slen);
                                    dst = ray_str_vec_append(dst, sp ? sp : "", sp ? slen : 0);
                                }
                            }
                        } else {
                            for (int64_t gi = 0; gi < n_groups && dst && !RAY_IS_ERR(dst); gi++) {
                                size_t slen = 0;
                                const char* sp = ray_str_vec_get(sc, fi[gi], &slen);
                                dst = ray_str_vec_append(dst, sp ? sp : "", sp ? slen : 0);
                            }
                        }
                    } else if (sc->type == RAY_LIST) {
                        dst = ray_alloc(n_groups * sizeof(ray_t*));
                        if (dst) {
                            dst->type = RAY_LIST; dst->len = n_groups;
                            ray_t** dout = (ray_t**)ray_data(dst);
                            ray_t** sitems = (ray_t**)ray_data(sc);
                            for (int64_t gi = 0; gi < n_groups; gi++) { dout[gi] = sitems[fi[gi]]; ray_retain(dout[gi]); }
                        }
                    } else {
                        dst = ray_vec_new(sc->type, n_groups);
                        if (dst && !RAY_IS_ERR(dst)) {
                            /* len BEFORE the loop: store_typed_elem's null
                             * path routes through ray_vec_set_null which
                             * silently drops out-of-range writes — post-
                             * loop assignment would lose the null bit on
                             * every nullable row in this gather. */
                            dst->len = n_groups;
                            for (int64_t gi = 0; gi < n_groups; gi++) {
                                int a = 0; ray_t* v = collection_elem(sc, fi[gi], &a);
                                store_typed_elem(dst, gi, v);
                                if (a) ray_release(v);
                            }
                        }
                    }
                    if (!dst || RAY_IS_ERR(dst)) {
                        if (dst) ray_release(dst);
                        ray_release(result);
                        result = ray_error("oom", NULL);
                        break;
                    }
                    result = ray_table_add_col(result, cn, dst);
                    ray_release(dst);
                }
                if (fi_hdr) ray_free(fi_hdr);
            }

            ray_release(groups);
            if (eval_tbl != tbl) ray_release(eval_tbl);
            ray_release(tbl);
            return apply_sort_take(result, dict_elems, dict_n, asc_id, desc_id, take_id);
        }

        /* Pre-scan: any non-aggregation expressions that need a flat
         * (post-filter) table?  Most non-agg expressions evaluate via
         * ray_eval over the whole table and require a materialized
         * filtered_tbl when WHERE is present.
         *
         * The exception is `(count (distinct col_ref))`: its scatter
         * runs through ray_count_distinct_per_group, which reads the
         * source column directly and skips rows where row_gid[r] < 0.
         * As long as the row→gid build masks filtered-out rows to -1
         * (using the selection saved across the path-A graph free),
         * count(distinct col_ref) doesn't need the materialization.
         * That's worth ~100 ms on Q14 (937 K rows × 105 cols filtered
         * → 937 K rows × 105 cols copy). */
        int has_nonagg_needing_flat = 0;
        for (int64_t i = 0; i + 1 < dict_n; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            if (kid == from_id || kid == where_id || kid == by_id ||
                kid == take_id || kid == asc_id || kid == desc_id) continue;
            ray_t* expr = dict_elems[i + 1];
            if (is_group_dag_agg_expr(expr)) continue;
            ray_t* cd_inner = match_count_distinct(expr);
            int is_simple_cd = cd_inner && cd_inner->type == -RAY_SYM &&
                               (cd_inner->attrs & RAY_ATTR_NAME);
            if (!is_simple_cd) { has_nonagg_needing_flat = 1; break; }
        }

        /* The post-DAG scatter needs a flat single-segment table: it
         * reads key columns directly and runs ray_eval over the whole
         * input.  Detect parted tables up front — if the source is
         * parted and there's no WHERE to materialize it, return nyi. */
        int table_is_parted = 0;
        if (has_nonagg_needing_flat) {
            int64_t ncols = ray_table_ncols(tbl);
            for (int64_t c = 0; c < ncols; c++) {
                ray_t* col = ray_table_get_col_idx(tbl, c);
                if (col && RAY_IS_PARTED(col->type)) { table_is_parted = 1; break; }
            }
            if (table_is_parted && !where_expr) {
                ray_graph_free(g); ray_release(tbl);
                return ray_error("nyi", "non-agg expression on parted table without WHERE");
            }
        }

        /* WHERE + BY handling.  Two paths:
         *
         *   (A) Fused path — applicable when there are no non-agg
         *       output expressions and the source table is flat
         *       (not parted).  Execute the filter node in-place
         *       via exec_node; OP_FILTER on a TABLE input installs
         *       a lazy RAY_SEL bitmap on g->selection and returns
         *       the original uncompacted table.  The subsequent
         *       ray_group call builds its own key/agg scans over
         *       g->table, and exec_group honours g->selection in
         *       the radix / DA / sequential paths — so no rows are
         *       materialized twice.  This is the fast path for
         *       `select ... by ... where` queries.
         *
         *   (B) Materialize path — applicable when (A) is not.
         *       Pre-execute the filter and flatten into a new
         *       table, then rebuild the graph.  Needed because
         *       the non-agg scatter runs ray_eval over a flat
         *       single-segment table, and parted tables need
         *       segment-level flattening before group anyway.
         *
         * (This also fixes a pre-existing WHERE-vs-by bug: any
         * WHERE clause on a `select ... by` query was silently
         * ignored before the filter was wired through the group
         * pipeline.) */
        if (where_expr && !can_fuse_phase1) {
            bool can_fuse = !has_nonagg_needing_flat && !table_is_parted;
            if (can_fuse) {
                root = ray_optimize(g, root);
                /* exec_node populates g->selection as a side effect
                 * of OP_FILTER on a table input, and returns the
                 * uncompacted table (== g->table).  Discard the
                 * result — we only needed the side effect. */
                ray_t* fres = exec_node(g, root);
                if (!fres || RAY_IS_ERR(fres)) {
                    if (g->selection) {
                        ray_release(g->selection);
                        g->selection = NULL;
                    }
                    ray_graph_free(g); ray_release(tbl);
                    return fres ? fres : ray_error("domain", NULL);
                }
                /* OP_CONST/OP_FILTER both retain, so the returned
                 * table has an extra refcount we must release.
                 * g->table still owns tbl via the graph, so this
                 * only drops the exec-node-side retain. */
                ray_release(fres);
                /* Retain a copy of the selection so it survives the
                 * later ray_graph_free.  count(distinct col_ref) needs
                 * this in the n_nonaggs scatter to mask filtered-out
                 * rows in the row→gid build. */
                if (g->selection) {
                    saved_selection = g->selection;
                    ray_retain(saved_selection);
                }
            } else {
                root = ray_optimize(g, root);
                ray_t* fres = ray_execute(g, root);
                ray_graph_free(g); g = NULL;
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                if (ray_is_lazy(fres)) fres = ray_lazy_materialize(fres);
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                ray_release(tbl);
                tbl = fres;
                g = ray_graph_new(tbl);
                if (!g) { ray_release(tbl); return ray_error("oom", NULL); }
                root = ray_const_table(g, tbl);
            }
        }

        /* Compile group key(s) */
        ray_op_t* key_ops[16];
        uint8_t n_keys = 0;

        if (by_expr->type == RAY_SYM) {
            /* Multiple keys as SYM vector: [col1 col2 ...] */
            int64_t nk = ray_len(by_expr);
            int64_t* sym_ids = (int64_t*)ray_data(by_expr);
            for (int64_t i = 0; i < nk && n_keys < 16; i++) {
                ray_t* name_str = ray_sym_str(sym_ids[i]);
                if (!name_str) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
                key_ops[n_keys] = ray_scan(g, ray_str_ptr(name_str));
                if (!key_ops[n_keys]) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
                n_keys++;
            }
        } else {
            /* Single key expression */
            key_ops[0] = compile_expr_dag(g, by_expr);
            if (!key_ops[0]) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
            n_keys = 1;
        }

        /* Collect aggregation expressions from output columns.
         * Non-agg expressions are tracked separately for post-DAG scatter. */
        uint16_t agg_ops[16];
        ray_op_t* agg_ins[16];
        uint8_t n_aggs = 0;

        for (int64_t i = 0; i + 1 < dict_n; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            if (kid == from_id || kid == where_id || kid == by_id || kid == take_id || kid == asc_id || kid == desc_id) continue;

            ray_t* val_expr = dict_elems[i + 1];
            if (is_group_dag_agg_expr(val_expr) && n_aggs < 16) {
                ray_t** agg_elems = (ray_t**)ray_data(val_expr);
                agg_ops[n_aggs] = resolve_agg_opcode(agg_elems[0]->i64);
                /* Compile the aggregation input (the column reference) */
                agg_ins[n_aggs] = compile_expr_dag(g, agg_elems[1]);
                if (!agg_ins[n_aggs]) { ray_graph_free(g); ray_release(tbl); return ray_error("domain", NULL); }
                n_aggs++;
            } else if (!is_group_dag_agg_expr(val_expr) && n_nonaggs < 16) {
                nonagg_names[n_nonaggs] = kid;
                nonagg_exprs[n_nonaggs] = val_expr;
                n_nonaggs++;
            }
        }

        if (n_aggs > 0 || n_nonaggs > 0) {
            if (n_aggs > 0) {
                int agg_kinds_ok = (n_aggs <= 8);
                for (uint8_t i = 0; agg_kinds_ok && i < n_aggs; i++) {
                    if (agg_ops[i] != OP_COUNT && agg_ops[i] != OP_SUM &&
                        agg_ops[i] != OP_MIN   && agg_ops[i] != OP_MAX &&
                        agg_ops[i] != OP_AVG)
                        agg_kinds_ok = 0;
                }
                if (can_fuse_phase1 && fused_pred_op != NULL
                    && n_nonaggs == 0 && agg_kinds_ok)
                {
                    /* exec_filtered_group dispatches: count1 (single key,
                     * single COUNT) → Phase 3 fast path; everything else →
                     * multi path with packed composite key. */
                    root = ray_filtered_group(g, fused_pred_op,
                                              key_ops, n_keys,
                                              agg_ops, agg_ins, n_aggs);
                } else {
                    root = ray_group(g, key_ops, n_keys, agg_ops, agg_ins, n_aggs);
                }
            } else {
                /* No aggs but non-agg expressions exist — still need group
                 * boundaries.  Use GROUP+COUNT on the key to get group keys.
                 * The count column will be dropped after execution. */
                uint16_t cnt_op = OP_COUNT;
                ray_op_t* cnt_in = key_ops[0];
                root = ray_group(g, key_ops, n_keys, &cnt_op, &cnt_in, 1);
                synth_count_col = 1;
            }
        } else {
            /* No explicit aggregations — apply WHERE filter first (if any),
             * then use DAG GROUP+COUNT for fast hash-parallel group boundaries,
             * then gather first-of-group from the filtered table. */
            ray_t* filtered_tbl = tbl;
            if (where_expr) {
                root = ray_optimize(g, root);
                ray_t* fres = ray_execute(g, root);
                ray_graph_free(g); g = NULL;
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                if (ray_is_lazy(fres)) fres = ray_lazy_materialize(fres);
                if (!fres || RAY_IS_ERR(fres)) { ray_release(tbl); return fres ? fres : ray_error("domain", NULL); }
                filtered_tbl = fres;
                /* Rebuild graph on filtered table for GROUP+COUNT */
                g = ray_graph_new(filtered_tbl);
                if (!g) { if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                n_keys = 0;
                if (by_expr->type == RAY_SYM) {
                    int64_t nk = ray_len(by_expr);
                    int64_t* sym_ids = (int64_t*)ray_data(by_expr);
                    for (int64_t i = 0; i < nk && n_keys < 16; i++) {
                        ray_t* ns = ray_sym_str(sym_ids[i]);
                        if (ns) key_ops[n_keys++] = ray_scan(g, ray_str_ptr(ns));
                    }
                } else {
                    key_ops[0] = compile_expr_dag(g, by_expr);
                    if (key_ops[0]) n_keys = 1;
                }
            }

            uint16_t cnt_op = OP_COUNT;
            ray_op_t* cnt_in = key_ops[0];
            root = ray_group(g, key_ops, n_keys, &cnt_op, &cnt_in, 1);
            root = ray_optimize(g, root);
            ray_t* grouped = ray_execute(g, root);
            ray_graph_free(g); g = NULL;
            if (!grouped || RAY_IS_ERR(grouped)) { if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return grouped; }
            if (ray_is_lazy(grouped)) grouped = ray_lazy_materialize(grouped);

            int64_t n_groups = ray_table_nrows(grouped);

            /* Resolve key column sym early — needed for empty result schema.
             * A dotted name like `Timestamp.date` compiles to a scan + trunc
             * chain, not a direct column lookup, so it must land in the
             * computed-key fallback path below (key_sym stays -1).  Otherwise
             * downstream `ray_table_get_col(filtered_tbl, key_sym)` would
             * return NULL for the non-existent "Timestamp.date" column and
             * the subsequent deref would crash. */
            int64_t key_sym = -1;
            if (by_expr->type == -RAY_SYM && (by_expr->attrs & RAY_ATTR_NAME)
                && !ray_sym_is_dotted(by_expr->i64))
                key_sym = by_expr->i64;
            else if (by_expr->type == RAY_SYM && ray_len(by_expr) == 1)
                key_sym = ((int64_t*)ray_data(by_expr))[0];

            if (n_groups == 0) {
                ray_release(grouped);
                int64_t nc0 = ray_table_ncols(filtered_tbl);
                ray_t* empty = ray_table_new(nc0 + 1);
                if (!RAY_IS_ERR(empty)) {
                    /* Key column.  For a plain/column key, key_sym
                     * names a real source column and we mirror its
                     * type.  For a computed key (dotted, xbar, ...)
                     * we evaluate by_expr against the filtered (empty)
                     * table to learn the key's type and name without
                     * duplicating schema derivation logic. */
                    int64_t empty_key_name = key_sym;
                    ray_t* empty_key_vec = NULL;
                    if (key_sym >= 0) {
                        ray_t* sc = ray_table_get_col(filtered_tbl, key_sym);
                        if (sc) {
                            empty_key_vec = (sc->type == RAY_STR)
                                            ? ray_vec_new(RAY_STR, 0)
                                            : ray_vec_new(sc->type, 0);
                        }
                    } else {
                        /* Match the computed-key fallback's naming
                         * rules (dotted tail / last name arg) and
                         * collision handling. */
                        int64_t ck_name = -1;
                        int64_t ck_full = -1;
                        int64_t ck_head = -1;
                        if (by_expr->type == -RAY_SYM && (by_expr->attrs & RAY_ATTR_NAME)) {
                            ck_full = by_expr->i64;
                            if (ray_sym_is_dotted(by_expr->i64)) {
                                const int64_t* segs;
                                int nsegs = ray_sym_segs(by_expr->i64, &segs);
                                if (nsegs > 0) { ck_name = segs[nsegs - 1]; ck_head = segs[0]; }
                            } else {
                                ck_name = by_expr->i64;
                            }
                        } else if (by_expr->type == RAY_LIST && by_expr->len >= 2) {
                            ray_t** be = (ray_t**)ray_data(by_expr);
                            for (int64_t i = by_expr->len - 1; i >= 1; i--) {
                                if (be[i]->type == -RAY_SYM && (be[i]->attrs & RAY_ATTR_NAME)) {
                                    ck_name = be[i]->i64;
                                    break;
                                }
                            }
                        }
                        if (ck_name < 0) ck_name = ray_sym_intern("key", 3);
                        if (ck_head >= 0 && ck_full >= 0 && ck_name != ck_full) {
                            for (int64_t c = 0; c < nc0; c++) {
                                int64_t cn = ray_table_col_name(filtered_tbl, c);
                                if (cn == ck_name && cn != ck_head) {
                                    ck_name = ck_full;
                                    break;
                                }
                            }
                        }
                        empty_key_name = ck_name;

                        /* Evaluate by_expr against the (empty) filtered table
                         * to get a length-0 key vector typed like the
                         * non-empty path would produce it. */
                        ray_env_push_scope();
                        for (int64_t c = 0; c < nc0; c++) {
                            ray_env_set_local(ray_table_col_name(filtered_tbl, c),
                                              ray_table_get_col_idx(filtered_tbl, c));
                        }
                        ray_t* ck_vec = ray_eval(by_expr);
                        ray_env_pop_scope();
                        if (ck_vec && !RAY_IS_ERR(ck_vec) && ray_is_vec(ck_vec)) {
                            int8_t kt = ck_vec->type;
                            empty_key_vec = (kt == RAY_STR)
                                            ? ray_vec_new(RAY_STR, 0)
                                            : (kt == RAY_LIST)
                                              ? ray_list_new(0)
                                              : ray_vec_new(kt, 0);
                        }
                        if (ck_vec && !RAY_IS_ERR(ck_vec)) ray_release(ck_vec);
                    }
                    if (empty_key_vec && !RAY_IS_ERR(empty_key_vec)) {
                        empty = ray_table_add_col(empty, empty_key_name, empty_key_vec);
                        ray_release(empty_key_vec);
                    }

                    for (int64_t c = 0; c < nc0; c++) {
                        int64_t cn = ray_table_col_name(filtered_tbl, c);
                        if (cn == empty_key_name) continue;
                        ray_t* sc = ray_table_get_col_idx(filtered_tbl, c);
                        ray_t* ev = (sc->type == RAY_STR) ? ray_vec_new(RAY_STR, 0) :
                                    (sc->type == RAY_LIST) ? ray_list_new(0) :
                                    ray_vec_new(sc->type, 0);
                        if (!RAY_IS_ERR(ev)) { empty = ray_table_add_col(empty, cn, ev); ray_release(ev); }
                    }
                }
                if (filtered_tbl != tbl) ray_release(filtered_tbl);
                ray_release(tbl);
                return empty;
            }

            /* Build first_idx: scan filtered key column once, record first
             * occurrence of each group key value. */
            if (key_sym < 0) {
                /* Computed group key (e.g., xbar) — fall back to eval-level groupby */
                ray_release(grouped);
                int64_t tbl_ncols = ray_table_ncols(filtered_tbl);
                ray_env_push_scope();
                for (int64_t c = 0; c < tbl_ncols; c++) {
                    int64_t cn = ray_table_col_name(filtered_tbl, c);
                    ray_t* cv = ray_table_get_col_idx(filtered_tbl, c);
                    ray_env_set_local(cn, cv);
                }
                ray_t* computed_key = ray_eval(by_expr);
                ray_env_pop_scope();
                if (!computed_key || RAY_IS_ERR(computed_key)) {
                    if (filtered_tbl != tbl) ray_release(filtered_tbl);
                    ray_release(tbl);
                    return computed_key ? computed_key : ray_error("domain", NULL);
                }
                ray_t* groups2_dict = ray_group_fn(computed_key);
                if (!groups2_dict || RAY_IS_ERR(groups2_dict)) {
                    ray_release(computed_key);
                    if (filtered_tbl != tbl) ray_release(filtered_tbl);
                    ray_release(tbl);
                    return groups2_dict ? groups2_dict : ray_error("domain", NULL);
                }
                ray_t* groups2 = groups_to_pair_list(groups2_dict);
                ray_release(groups2_dict);
                if (RAY_IS_ERR(groups2)) {
                    ray_release(computed_key);
                    if (filtered_tbl != tbl) ray_release(filtered_tbl);
                    ray_release(tbl);
                    return groups2;
                }
                int64_t ng2 = ray_len(groups2) / 2;
                if (ng2 == 0) { ray_release(groups2); ray_release(computed_key); if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_table_new(0); }
                ray_t** gi2 = (ray_t**)ray_data(groups2);

                /* fi2 must sweep EVERY group, not just the first 256 —
                 * the downstream result-column loops iterate up to ng2
                 * and indexed reads beyond a fixed-size stack slot would
                 * pick up uninitialised bytes.  Stack-fast for small
                 * group counts, heap-fallback once we need more. */
                int64_t fi2_stack[256];
                ray_t*  fi2_hdr = NULL;
                int64_t* fi2 = fi2_stack;
                if (ng2 > 256) {
                    fi2_hdr = ray_alloc((size_t)ng2 * sizeof(int64_t));
                    if (!fi2_hdr) {
                        ray_release(groups2); ray_release(computed_key);
                        if (filtered_tbl != tbl) ray_release(filtered_tbl);
                        ray_release(tbl);
                        return ray_error("oom", NULL);
                    }
                    fi2 = (int64_t*)ray_data(fi2_hdr);
                }
                for (int64_t g2 = 0; g2 < ng2; g2++) {
                    int alloc2 = 0;
                    ray_t* i02 = collection_elem(gi2[g2 * 2 + 1], 0, &alloc2);
                    fi2[g2] = as_i64(i02);
                    if (alloc2) ray_release(i02);
                }
                /* Name for the synthesized key column:
                 *  - dotted sym `a.b.c` → tail segment (`c`) so `Timestamp.ss`
                 *    surfaces as an `ss` column (pretty in the common case).
                 *    If the tail collides with an *unrelated* source column
                 *    (not the head of the dotted path), fall back to the
                 *    full dotted name so we don't silently drop real data.
                 *  - list expr `(xbar N col)` / `(+ col 1)` → last name-typed
                 *    argument, so the transform's output deliberately
                 *    replaces the source column (matches xbar convention).
                 *  - fall back to an interned "key" if nothing more specific
                 *    can be derived. */
                int64_t ckey_name = -1;
                int64_t ckey_full = -1;       /* full dotted sym, for collision fallback */
                int64_t ckey_head = -1;       /* head segment of dotted expr (input column) */
                if (by_expr->type == -RAY_SYM && (by_expr->attrs & RAY_ATTR_NAME)) {
                    ckey_full = by_expr->i64;
                    if (ray_sym_is_dotted(by_expr->i64)) {
                        const int64_t* segs;
                        int nsegs = ray_sym_segs(by_expr->i64, &segs);
                        if (nsegs > 0) {
                            ckey_name = segs[nsegs - 1];
                            ckey_head = segs[0];
                        }
                    } else {
                        ckey_name = by_expr->i64;
                    }
                } else if (by_expr->type == RAY_LIST && by_expr->len >= 2) {
                    ray_t** be = (ray_t**)ray_data(by_expr);
                    for (int64_t i = by_expr->len - 1; i >= 1; i--) {
                        if (be[i]->type == -RAY_SYM && (be[i]->attrs & RAY_ATTR_NAME)) {
                            ckey_name = be[i]->i64;
                            break;
                        }
                    }
                }
                if (ckey_name < 0) ckey_name = ray_sym_intern("key", 3);

                /* Collision check for dotted tail: if the tail name matches
                 * a source column that isn't the head of the dotted expr,
                 * the old code silently dropped that source column from the
                 * result.  Promote to the full dotted sym so both stay. */
                if (ckey_head >= 0 && ckey_full >= 0 && ckey_name != ckey_full) {
                    for (int64_t c = 0; c < tbl_ncols; c++) {
                        int64_t cn = ray_table_col_name(filtered_tbl, c);
                        if (cn == ckey_name && cn != ckey_head) {
                            ckey_name = ckey_full;
                            break;
                        }
                    }
                }

                ray_t* res2 = ray_table_new(tbl_ncols + 1);
                /* Key column: computed_key's first-of-group values, which
                 * are the distinct grouping-key values surfaced to the
                 * user.  Using the source column at fi2 indices would lose
                 * the transform (e.g. raw Timestamp instead of its `.ss`). */
                if (ray_is_vec(computed_key)) {
                    ray_t* kv = ray_vec_new(computed_key->type, ng2);
                    if (!RAY_IS_ERR(kv)) {
                        /* len BEFORE store loop — ray_vec_set_null (called
                         * by store_typed_elem for null atoms) range-checks
                         * idx against vec->len and silently no-ops
                         * otherwise. */
                        kv->len = ng2;
                        for (int64_t g2 = 0; g2 < ng2; g2++) {
                            int a2 = 0;
                            ray_t* v2 = collection_elem(computed_key, fi2[g2], &a2);
                            store_typed_elem(kv, g2, v2);
                            if (a2) ray_release(v2);
                        }
                        res2 = ray_table_add_col(res2, ckey_name, kv);
                        ray_release(kv);
                    }
                }
                for (int64_t c = 0; c < tbl_ncols; c++) {
                    int64_t cn = ray_table_col_name(filtered_tbl, c);
                    /* Avoid duplicating a column name already used by the
                     * key: e.g. `by: Timestamp` (plain, non-dotted) would
                     * collide with the source Timestamp column. */
                    if (cn == ckey_name) continue;
                    ray_t* sc = ray_table_get_col_idx(filtered_tbl, c);
                    ray_t* dc = ray_vec_new(sc->type, ng2);
                    dc->len = ng2;    /* see note above — hoisted for null bits */
                    for (int64_t g2 = 0; g2 < ng2; g2++) { int a2 = 0; ray_t* v2 = collection_elem(sc, fi2[g2], &a2); store_typed_elem(dc, g2, v2); if (a2) ray_release(v2); }
                    res2 = ray_table_add_col(res2, cn, dc); ray_release(dc);
                }
                if (fi2_hdr) ray_free(fi2_hdr);
                ray_release(groups2); ray_release(computed_key);
                if (filtered_tbl != tbl) ray_release(filtered_tbl);
                ray_release(tbl);
                return res2;
            }

            ray_t* orig_key_col = ray_table_get_col(filtered_tbl, key_sym);
            int64_t nrows_orig = orig_key_col ? orig_key_col->len : 0;

            /* Read group key values from grouped table BEFORE releasing it.
             * grp_key_col points into grouped — must not access after release. */
            ray_t* grp_key_col = ray_table_get_col(grouped, key_sym);
            int8_t kt = orig_key_col ? orig_key_col->type : 0;

            /* Heap-allocate gk_vals when n_groups > 256 */
            int64_t gk_stack[256];
            ray_t* gk_heap_hdr = NULL;
            int64_t* gk_vals = gk_stack;
            if (n_groups > 256) {
                gk_heap_hdr = ray_alloc((size_t)n_groups * sizeof(int64_t));
                if (!gk_heap_hdr) { ray_release(grouped); if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                gk_vals = (int64_t*)ray_data(gk_heap_hdr);
            }

            /* Copy group key values while grouped is still alive.
             * STR/LIST/GUID keys are routed through eval-level fallback
             * above, so only integer-like types reach here.  Use
             * read_col_i64 for non-F64 types — it dispatches on the
             * column type (I32/I16/I8/BOOL/SYM adaptive width etc.),
             * whereas ray_read_sym interprets `attrs` as SYM width and
             * silently truncates to 1 byte for plain integer columns
             * where attrs doesn't carry width bits.
             *
             * We also record a per-group null flag.  The DAG GROUP path
             * stores null keys with value=0 and differentiates via a
             * null mask — if we hashed raw bits only, a null group would
             * collide with non-null value 0 (for I64 / I32 / SYM / DATE
             * / TIME etc.) or with +0.0 for F64 (ray_hash_f64 normalises
             * -0.0 to +0.0, and F64's null bit pattern on this platform
             * is the -0.0 pattern).  The null flag keeps those groups
             * distinct. */
            uint8_t gk_null_stack[256];
            ray_t*  gk_null_hdr = NULL;
            uint8_t* gk_null = gk_null_stack;
            if (n_groups > 256) {
                gk_null_hdr = ray_alloc((size_t)n_groups * sizeof(uint8_t));
                if (!gk_null_hdr) {
                    if (gk_heap_hdr) ray_free(gk_heap_hdr);
                    ray_release(grouped);
                    if (filtered_tbl != tbl) ray_release(filtered_tbl);
                    ray_release(tbl);
                    return ray_error("oom", NULL);
                }
                gk_null = (uint8_t*)ray_data(gk_null_hdr);
            }
            memset(gk_null, 0, (size_t)n_groups * sizeof(uint8_t));

            if (grp_key_col) {
                bool gk_has_nulls = (grp_key_col->attrs & RAY_ATTR_HAS_NULLS) != 0;
                if (gk_has_nulls) {
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        if (kt == RAY_F64)
                            memcpy(&gk_vals[gi], &((double*)ray_data(grp_key_col))[gi], 8);
                        else
                            gk_vals[gi] = read_col_i64(ray_data(grp_key_col), gi, kt, grp_key_col->attrs);
                        if (ray_vec_is_null(grp_key_col, gi))
                            gk_null[gi] = 1;
                    }
                } else {
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        if (kt == RAY_F64)
                            memcpy(&gk_vals[gi], &((double*)ray_data(grp_key_col))[gi], 8);
                        else
                            gk_vals[gi] = read_col_i64(ray_data(grp_key_col), gi, kt, grp_key_col->attrs);
                    }
                }
            }
            ray_release(grouped); /* grp_key_col is now invalid */

            /* Allocate first_idx */
            int64_t first_idx_stack[256];
            ray_t* fi_heap_hdr = NULL;
            int64_t* first_idx = first_idx_stack;
            if (n_groups > 256) {
                fi_heap_hdr = ray_alloc((size_t)n_groups * sizeof(int64_t));
                if (!fi_heap_hdr) { if (gk_heap_hdr) ray_free(gk_heap_hdr); if (filtered_tbl != tbl) ray_release(filtered_tbl); ray_release(tbl); return ray_error("oom", NULL); }
                first_idx = (int64_t*)ray_data(fi_heap_hdr);
            }

            /* Build {key_bits -> group_index} hash table from gk_vals so the
             * scan below is O(nrows_orig + n_groups) instead of
             * O(nrows_orig * n_groups).  Without this a 1M-row / 1M-group
             * float-key grouping hangs for tens of seconds — I64 has a
             * low-cardinality direct-array fast path upstream, but F64
             * and other non-GUID scalar keys fall through to this scan. */
            for (int64_t gi = 0; gi < n_groups; gi++) first_idx[gi] = -1;
            {
                uint32_t fi_cap = 64;
                while ((uint64_t)fi_cap < (uint64_t)n_groups * 2 && fi_cap < (1u << 30))
                    fi_cap <<= 1;
                uint32_t fi_mask = fi_cap - 1;
                ray_t* fi_ht_hdr = ray_alloc((size_t)fi_cap * sizeof(uint32_t));
                if (!fi_ht_hdr) {
                    if (gk_heap_hdr) ray_free(gk_heap_hdr);
                    if (fi_heap_hdr) ray_free(fi_heap_hdr);
                    if (filtered_tbl != tbl) ray_release(filtered_tbl);
                    ray_release(tbl);
                    return ray_error("oom", NULL);
                }
                uint32_t* fi_ht = (uint32_t*)ray_data(fi_ht_hdr);
                memset(fi_ht, 0xFF, (size_t)fi_cap * sizeof(uint32_t));

                /* Insert every group key into the HT keyed by bit pattern.
                 * For F64 keys, hash via the float path; memcpy bit pattern
                 * out of gk_vals to dodge strict-aliasing.  Null groups
                 * get a distinct hash so they don't collide with zero-valued
                 * groups (F64 null has the -0.0 bit pattern, which
                 * ray_hash_f64 normalises to +0.0; integer-flavoured
                 * nulls are stored as value=0). */
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    uint64_t h;
                    if (gk_null[gi]) {
                        h = ray_hash_i64((int64_t)0xDEADBEEFCAFEBABEULL);
                    } else if (kt == RAY_F64) {
                        double dv;
                        memcpy(&dv, &gk_vals[gi], 8);
                        h = ray_hash_f64(dv);
                    } else {
                        h = ray_hash_i64(gk_vals[gi]);
                    }
                    uint32_t slot = (uint32_t)(h & fi_mask);
                    while (fi_ht[slot] != UINT32_MAX) slot = (slot + 1) & fi_mask;
                    fi_ht[slot] = (uint32_t)gi;
                }

                /* Single linear scan of the source column; for each row
                 * hash-lookup its group index and record the first row
                 * that maps to it.  Terminate early once every group has
                 * a first-row. */
                bool orig_nulls_flag = orig_key_col
                    && (orig_key_col->attrs & RAY_ATTR_HAS_NULLS) != 0;
                int64_t found = 0;
                for (int64_t r = 0; r < nrows_orig && found < n_groups; r++) {
                    bool r_null = orig_nulls_flag && ray_vec_is_null(orig_key_col, r);
                    int64_t ov;
                    if (kt == RAY_F64) memcpy(&ov, &((double*)ray_data(orig_key_col))[r], 8);
                    else ov = read_col_i64(ray_data(orig_key_col), r, kt, orig_key_col->attrs);
                    uint64_t h;
                    if (r_null) {
                        h = ray_hash_i64((int64_t)0xDEADBEEFCAFEBABEULL);
                    } else if (kt == RAY_F64) {
                        double dv;
                        memcpy(&dv, &ov, 8);
                        h = ray_hash_f64(dv);
                    } else {
                        h = ray_hash_i64(ov);
                    }
                    uint32_t slot = (uint32_t)(h & fi_mask);
                    while (fi_ht[slot] != UINT32_MAX) {
                        uint32_t cand = fi_ht[slot];
                        bool match = (r_null && gk_null[cand])
                                     || (!r_null && !gk_null[cand] && gk_vals[cand] == ov);
                        if (match) {
                            if (first_idx[cand] < 0) {
                                first_idx[cand] = r;
                                found++;
                            }
                            break;
                        }
                        slot = (slot + 1) & fi_mask;
                    }
                }
                ray_free(fi_ht_hdr);
            }
            if (gk_null_hdr) ray_free(gk_null_hdr);
            if (gk_heap_hdr) ray_free(gk_heap_hdr);

            /* Now build the result table using first_idx gathered above.
             * key_sym and n_groups are already set. */

            /* Build result table: key column first, then others */
            int64_t ncols = ray_table_ncols(filtered_tbl);
            ray_t* result = ray_table_new(ncols);
            if (RAY_IS_ERR(result)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); return result; }

            /* Add key column first */
            ray_t* key_vec_src = ray_table_get_col(filtered_tbl, key_sym);
            if (key_vec_src->type == RAY_STR) {
                ray_t* key_vec_dst = ray_vec_new(RAY_STR, n_groups);
                if (!key_vec_dst || RAY_IS_ERR(key_vec_dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return key_vec_dst ? key_vec_dst : ray_error("oom", NULL); }
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    size_t slen = 0;
                    const char* sp = ray_str_vec_get(key_vec_src, first_idx[gi], &slen);
                    key_vec_dst = ray_str_vec_append(key_vec_dst, sp ? sp : "", sp ? slen : 0);
                    if (RAY_IS_ERR(key_vec_dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return key_vec_dst; }
                }
                result = ray_table_add_col(result, key_sym, key_vec_dst);
                ray_release(key_vec_dst);
            } else {
                ray_t* key_vec_dst = ray_vec_new(key_vec_src->type, n_groups);
                if (RAY_IS_ERR(key_vec_dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return key_vec_dst; }
                /* Set len BEFORE the store loop: store_typed_elem routes
                 * null atoms through ray_vec_set_null, which range-checks
                 * idx against vec->len and silently returns RAY_ERR_RANGE
                 * otherwise.  Postponing len=n_groups until after the loop
                 * therefore dropped the null bit on every nullable key row
                 * — the result would read back the raw zero/-0.0 bits with
                 * no HAS_NULLS flag, corrupting the grouped key column. */
                key_vec_dst->len = n_groups;
                for (int64_t gi = 0; gi < n_groups; gi++) {
                    int alloc = 0;
                    ray_t* val = collection_elem(key_vec_src, first_idx[gi], &alloc);
                    store_typed_elem(key_vec_dst, gi, val);
                    if (alloc) ray_release(val);
                }
                result = ray_table_add_col(result, key_sym, key_vec_dst);
                ray_release(key_vec_dst);
            }

            /* Add non-key columns */
            for (int64_t c = 0; c < ncols; c++) {
                int64_t col_name = ray_table_col_name(filtered_tbl, c);
                if (col_name == key_sym) continue;
                ray_t* src_col = ray_table_get_col_idx(filtered_tbl, c);
                int8_t ct = src_col->type;

                if (ct == RAY_STR) {
                    /* String column: build STR vector */
                    ray_t* dst = ray_vec_new(RAY_STR, n_groups);
                    if (!dst || RAY_IS_ERR(dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return dst ? dst : ray_error("oom", NULL); }
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        size_t slen = 0;
                        const char* sp = ray_str_vec_get(src_col, first_idx[gi], &slen);
                        dst = ray_str_vec_append(dst, sp ? sp : "", sp ? slen : 0);
                        if (RAY_IS_ERR(dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return dst; }
                    }
                    result = ray_table_add_col(result, col_name, dst);
                    ray_release(dst);
                } else if (ct == RAY_LIST) {
                    /* List column: pick items */
                    ray_t* dst = ray_alloc(n_groups * sizeof(ray_t*));
                    if (!dst) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return ray_error("oom", NULL); }
                    dst->type = RAY_LIST;
                    dst->len = n_groups;
                    ray_t** dout = (ray_t**)ray_data(dst);
                    ray_t** src_items = (ray_t**)ray_data(src_col);
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        dout[gi] = src_items[first_idx[gi]];
                        ray_retain(dout[gi]);
                    }
                    result = ray_table_add_col(result, col_name, dst);
                    ray_release(dst);
                } else {
                    /* Typed vector: copy elements at first indices.
                     * len must be set before the store loop so null bits
                     * propagate through store_typed_elem → ray_vec_set_null
                     * (same reason as the key column above). */
                    ray_t* dst = ray_vec_new(ct, n_groups);
                    if (RAY_IS_ERR(dst)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); ray_release(result); return dst; }
                    dst->len = n_groups;
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        int alloc = 0;
                        ray_t* val = collection_elem(src_col, first_idx[gi], &alloc);
                        store_typed_elem(dst, gi, val);
                        if (alloc) ray_release(val);
                    }
                    result = ray_table_add_col(result, col_name, dst);
                    ray_release(dst);
                }
                if (RAY_IS_ERR(result)) { if (fi_heap_hdr) ray_free(fi_heap_hdr); ray_release(tbl); return result; }
            }

            if (fi_heap_hdr) ray_free(fi_heap_hdr);
            if (filtered_tbl != tbl) ray_release(filtered_tbl);
            ray_release(tbl);
            return apply_sort_take(result, dict_elems, dict_n, asc_id, desc_id, take_id);
        }
    } else if (n_out > 0) {
        /* No `by:` but explicit output expressions.
         *
         * Two sub-cases:
         *   (a) All outputs are aggregates → scalar reduction.  Route
         *       through ray_group(n_keys=0) so the result is ONE row,
         *       not the input row count broadcast.  The naive ray_select
         *       path lowers `(sum c)` to OP_SUM as a column expression;
         *       OP_SELECT then broadcasts the scalar atom to nrows
         *       (exec.c: vec->type < 0 → broadcast_scalar), producing
         *       N copies of the same value.
         *   (b) At least one non-agg output → keep the existing
         *       projection (broadcast-as-column), matching q's
         *       per-row evaluation semantics.
         *
         * Mixed agg+non-agg without `by:` continues to flow through (b);
         * q's semantics there imply LIST/scalar mixing that is out of
         * scope for this fix. */
        int has_agg = 0;
        int has_nonagg_out = 0;
        for (int64_t i = 0; i + 1 < dict_n; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            if (kid == from_id || kid == where_id || kid == by_id ||
                kid == take_id || kid == asc_id || kid == desc_id || kid == nearest_id) continue;
            if (is_agg_expr(dict_elems[i + 1])) has_agg = 1;
            else has_nonagg_out = 1;
        }

        if (has_agg && !has_nonagg_out && !nearest_expr) {
            /* Scalar reduction.  Pre-execute the WHERE filter (already
             * wired as ray_filter at the top) so OP_FILTER on the table
             * input populates g->selection, which exec_group then
             * honours in its n_keys==0 fast path. */
            if (where_expr) {
                root = ray_optimize(g, root);
                ray_t* fres = exec_node(g, root);
                if (!fres || RAY_IS_ERR(fres)) {
                    if (g->selection) {
                        ray_release(g->selection);
                        g->selection = NULL;
                    }
                    ray_graph_free(g); ray_release(tbl);
                    return fres ? fres : ray_error("domain", NULL);
                }
                ray_release(fres);
            }

            uint16_t  s_agg_ops[16];
            ray_op_t* s_agg_ins[16];
            uint8_t   s_n_aggs = 0;
            for (int64_t i = 0; i + 1 < dict_n && s_n_aggs < 16; i += 2) {
                int64_t kid = dict_elems[i]->i64;
                if (kid == from_id || kid == where_id || kid == by_id ||
                    kid == take_id || kid == asc_id || kid == desc_id || kid == nearest_id) continue;
                ray_t*  val_expr  = dict_elems[i + 1];
                ray_t** agg_elems = (ray_t**)ray_data(val_expr);
                s_agg_ops[s_n_aggs] = resolve_agg_opcode(agg_elems[0]->i64);
                s_agg_ins[s_n_aggs] = compile_expr_dag(g, agg_elems[1]);
                if (!s_agg_ins[s_n_aggs]) {
                    if (g->selection) {
                        ray_release(g->selection);
                        g->selection = NULL;
                    }
                    ray_graph_free(g); ray_release(tbl);
                    return ray_error("domain", NULL);
                }
                s_n_aggs++;
            }
            root = ray_group(g, NULL, 0, s_agg_ops, s_agg_ins, s_n_aggs);
        } else {
            /* Projection only (no group by) — select specific columns */
            ray_op_t* col_ops[16];
            uint8_t nc = 0;
            for (int64_t i = 0; i + 1 < dict_n; i += 2) {
                int64_t kid = dict_elems[i]->i64;
                if (kid == from_id || kid == where_id || kid == by_id || kid == take_id || kid == asc_id || kid == desc_id || kid == nearest_id) continue;
                if (nc < 16) {
                    col_ops[nc] = compile_expr_dag(g, dict_elems[i + 1]);
                    if (!col_ops[nc]) {
                        /* Nearest-path resources must be freed here too — the
                         * rerank handle/query buffers are held across the whole
                         * ray_select_fn body, not just inside the nearest block. */
                        if (nearest_handle_owned) ray_release(nearest_handle_owned);
                        if (nearest_query_owned)  ray_sys_free(nearest_query_owned);
                        ray_graph_free(g); ray_release(tbl);
                        return ray_error("domain", NULL);
                    }
                    nc++;
                }
            }
            root = ray_select(g, root, col_ops, nc);
        }
    }

    /* Sort: collect asc/desc columns in dict iteration order.
     * Only add to the DAG when there's no group-by — group-by changes the
     * output schema, so sort on output columns must happen post-execution.
     * Values are unevaluated — a SYM atom is a column name, a SYM vector
     * is multiple column names.  No ray_eval needed. */
    if (has_sort && !by_expr) {
        ray_op_t* sort_keys[16];
        uint8_t   sort_descs[16];
        uint8_t   n_sort = 0;
        for (int64_t i = 0; i + 1 < dict_n && n_sort < 16; i += 2) {
            int64_t kid = dict_elems[i]->i64;
            uint8_t is_desc = 0;
            if (kid == asc_id) is_desc = 0;
            else if (kid == desc_id) is_desc = 1;
            else continue;
            ray_t* val = dict_elems[i + 1];
            if (val->type == -RAY_SYM) {
                /* Single column name */
                ray_t* s = ray_sym_str(val->i64);
                sort_keys[n_sort] = ray_scan(g, ray_str_ptr(s));
                sort_descs[n_sort] = is_desc;
                n_sort++;
            } else if (ray_is_vec(val) && val->type == RAY_SYM) {
                /* Multiple column names */
                for (int64_t c = 0; c < val->len && n_sort < 16; c++) {
                    int64_t sid = ray_read_sym(ray_data(val), c, val->type, val->attrs);
                    ray_t* s = ray_sym_str(sid);
                    sort_keys[n_sort] = ray_scan(g, ray_str_ptr(s));
                    sort_descs[n_sort] = is_desc;
                    n_sort++;
                }
            } else {
                ray_graph_free(g); ray_release(tbl);
                return ray_error("domain", NULL);
            }
        }
        if (n_sort > 0)
            root = ray_sort_op(g, root, sort_keys, sort_descs, NULL, n_sort);
    }

    /* Take: add to DAG only when no group-by and no nearest (rerank
     * absorbs the take into its k parameter). */
    ray_t* take_range = NULL;
    if (take_expr && !by_expr && !nearest_expr) {
        ray_t* tv = ray_eval(take_expr);
        if (!tv || RAY_IS_ERR(tv)) { ray_graph_free(g); ray_release(tbl); return tv ? tv : ray_error("domain", NULL); }
        if (ray_is_atom(tv) && (tv->type == -RAY_I64 || tv->type == -RAY_I32)) {
            int64_t n_take = (tv->type == -RAY_I64) ? tv->i64 : tv->i32;
            ray_release(tv);
            if (n_take >= 0)
                root = ray_head(g, root, n_take);
            else
                root = ray_tail(g, root, -n_take);
        } else if (ray_is_vec(tv) && (tv->type == RAY_I64 || tv->type == RAY_I32) && tv->len == 2) {
            take_range = tv;  /* apply after DAG execution */
        } else {
            ray_release(tv);
            ray_graph_free(g); ray_release(tbl);
            return ray_error("domain", NULL);
        }
    }

    /* Optimize and execute */
    root = ray_optimize(g, root);
    ray_t* result = ray_execute(g, root);

    ray_graph_free(g);
    /* The nearest-query buffer was only referenced by ext->rerank.query_vec
     * and is safe to free once the graph (and thus the op ext) is gone. */
    if (nearest_query_owned) ray_sys_free(nearest_query_owned);
    /* The HNSW handle was kept alive through ray_execute so the rerank
     * ext's idx pointer stayed valid.  Safe to release now that the
     * graph (and its ext nodes) has been freed. */
    if (nearest_handle_owned) ray_release(nearest_handle_owned);

    /* Post-process: range take [start count] applied after execution */
    if (take_range && result && !RAY_IS_ERR(result)) {
        ray_t* sliced = ray_take_fn(result, take_range);
        ray_release(result);
        ray_release(take_range);
        result = sliced;
    } else if (take_range) {
        ray_release(take_range);
    }

    /* Post-process: reorder GROUP BY BOOL results to match first-occurrence
     * order in the original table (exec.c radix sort puts false before true) */
    if (by_expr && result && !RAY_IS_ERR(result) && result->type == RAY_TABLE) {
        if (ray_is_lazy(result)) result = ray_lazy_materialize(result);
        if (result && !RAY_IS_ERR(result) && result->type == RAY_TABLE) {
            ray_t* key_col = ray_table_get_col_idx(result, 0);
            if (key_col && key_col->type == RAY_BOOL && key_col->len >= 2) {
                /* Find first-occurrence order of bool values in original
                 * table.  Accept both scalar `-RAY_SYM` and single-element
                 * `RAY_SYM` vector forms. */
                int64_t by_sym = -1;
                if (by_expr->type == -RAY_SYM)
                    by_sym = by_expr->i64;
                else if (by_expr->type == RAY_SYM && ray_len(by_expr) == 1)
                    by_sym = ((int64_t*)ray_data(by_expr))[0];
                ray_t* orig_key = (by_sym >= 0) ? ray_table_get_col(tbl, by_sym) : NULL;
                if (orig_key && orig_key->type == RAY_BOOL && orig_key->len > 0) {
                    bool first_val = ((bool*)ray_data(orig_key))[0];
                    bool result_first = ((bool*)ray_data(key_col))[0];
                    if (first_val != result_first) {
                        /* Swap rows: reverse row order in all columns */
                        int64_t nrows_r = ray_table_nrows(result);
                        int64_t ncols_r = ray_table_ncols(result);
                        ray_t* reordered = ray_table_new((int32_t)ncols_r);
                        if (reordered && !RAY_IS_ERR(reordered)) {
                            int ok = 1;
                            for (int64_t c = 0; c < ncols_r && ok; c++) {
                                int64_t cn = ray_table_col_name(result, c);
                                ray_t* col = ray_table_get_col_idx(result, c);
                                int esz = ray_elem_size(col->type);
                                ray_t* new_col = ray_vec_new(col->type, nrows_r);
                                if (RAY_IS_ERR(new_col)) { ok = 0; break; }
                                new_col->len = nrows_r;
                                char* src = (char*)ray_data(col);
                                char* dst = (char*)ray_data(new_col);
                                bool has_nulls = (col->attrs & RAY_ATTR_HAS_NULLS) != 0;
                                if (has_nulls) {
                                    for (int64_t r = 0; r < nrows_r; r++) {
                                        memcpy(dst + r * esz, src + (nrows_r - 1 - r) * esz, esz);
                                        if (ray_vec_is_null(col, nrows_r - 1 - r))
                                            ray_vec_set_null(new_col, r, true);
                                    }
                                } else {
                                    for (int64_t r = 0; r < nrows_r; r++)
                                        memcpy(dst + r * esz, src + (nrows_r - 1 - r) * esz, esz);
                                }
                                reordered = ray_table_add_col(reordered, cn, new_col);
                                ray_release(new_col);
                                if (RAY_IS_ERR(reordered)) { ok = 0; break; }
                            }
                            if (ok) {
                                ray_release(result);
                                result = reordered;
                            } else if (reordered && !RAY_IS_ERR(reordered)) {
                                ray_release(reordered);
                            }
                        }
                    }
                }
            }
        }
    }

    /* Drop the synthesized COUNT column (used only to get group
     * boundaries when n_aggs == 0 && n_nonaggs > 0).  Must happen
     * before the rename/sort_take steps so they don't see a phantom
     * column. */
    if (synth_count_col && by_expr && result && !RAY_IS_ERR(result)) {
        if (ray_is_lazy(result)) result = ray_lazy_materialize(result);
        if (result && !RAY_IS_ERR(result) && result->type == RAY_TABLE) {
            int64_t nc = ray_table_ncols(result);
            if (nc >= 1) {
                ray_t* rebuilt = ray_table_new(nc - 1);
                if (rebuilt && !RAY_IS_ERR(rebuilt)) {
                    for (int64_t c = 0; c < nc - 1; c++) {
                        int64_t cn = ray_table_col_name(result, c);
                        ray_t* col = ray_table_get_col_idx(result, c);
                        rebuilt = ray_table_add_col(rebuilt, cn, col);
                    }
                    ray_release(result);
                    result = rebuilt;
                }
            }
        }
    }

    /* NOTE: tbl is released below AFTER the non-agg scatter, which
     * runs post-rename and post-sort_take so LIST columns do not
     * flow through the scalar-only apply_sort_take DAG. */

    /* Rename output columns if user specified names */
    if (result && !RAY_IS_ERR(result) && n_out > 0) {
        /* Materialize lazy results if needed */
        if (ray_is_lazy(result)) result = ray_lazy_materialize(result);
    }
    if (result && !RAY_IS_ERR(result) && result->type == RAY_TABLE && n_out > 0) {
        ray_t* schema = ray_table_schema(result);
        if (schema && !RAY_IS_ERR(schema) && schema->type > 0 && schema->type < RAY_TYPE_COUNT) {
            int64_t ncols = schema->len;
            /* Count key columns in by clause */
            int n_key_cols = 0;
            if (by_expr) {
                if (ray_is_vec(by_expr) && by_expr->type == RAY_SYM) n_key_cols = (int)ray_len(by_expr);
                else n_key_cols = 1;
            }
            /* Collect user-defined output column names.
             * For group-by, the result layout is [keys, aggs..., nonaggs...].
             * Non-agg columns were added by the post-DAG scatter block
             * with correct names already — only agg columns need renaming,
             * in dict-iteration order of the agg entries. */
            int64_t agg_user_names[16];
            int64_t all_user_names[16];
            int n_agg_user = 0;
            int n_all_user = 0;
            for (int64_t i = 0; i + 1 < dict_n; i += 2) {
                int64_t kid = dict_elems[i]->i64;
                if (kid == from_id || kid == where_id || kid == by_id ||
                    kid == take_id || kid == asc_id || kid == desc_id) continue;
                if (n_all_user < 16) all_user_names[n_all_user++] = kid;
                if (by_expr && !is_group_dag_agg_expr(dict_elems[i + 1])) continue;
                if (n_agg_user < 16) agg_user_names[n_agg_user++] = kid;
            }
            if (by_expr) {
                /* Rename only the agg columns (positions after keys).
                 * Non-agg LIST columns were named at scatter time. */
                for (int j = 0; j < n_agg_user && n_key_cols + j < ncols; j++)
                    ray_table_set_col_name(result, n_key_cols + j, agg_user_names[j]);
            } else {
                /* Projection-only: columns are in dict order */
                for (int j = 0; j < n_all_user && n_key_cols + j < ncols; j++)
                    ray_table_set_col_name(result, n_key_cols + j, all_user_names[j]);
            }
        }
    }

    /* Post-process: scatter non-agg expressions into LIST columns.
     * Must run BEFORE apply_sort_take so the sort clause can
     * reference non-agg output columns (and so the take clause
     * slices the fully-populated result).  apply_sort_take handles
     * LIST columns in the result table (same path used by the
     * eval_group branch).
     *
     * Reads group keys from the DAG result and builds row→group_id
     * against the original tbl. */
    if (n_nonaggs > 0 && by_expr && result && !RAY_IS_ERR(result)) {
        if (ray_is_lazy(result)) result = ray_lazy_materialize(result);
        if (result && !RAY_IS_ERR(result) && result->type == RAY_TABLE) {
            int64_t n_groups = ray_table_nrows(result);

            /* R8 fast path: every non-agg is a literal atom expression
             * with no column refs.  Skip the entire row→gid mapping —
             * each non-agg becomes a typed broadcast vec the same width
             * as n_groups, no idx_buf or per-group slicing required.
             *
             * Q35 = `{one: 1, c: count(URL), by: URL desc: c take: 10}`
             * is the canonical case: with all-literal nonaggs we go
             * directly to apply_sort_take and the top-K fast path
             * downstream of it. */
            if (n_groups > 0) {
                /* Pre-check ALL nonaggs first so we don't half-apply on
                 * an unhandled atom type and then have to roll back. */
                int all_broadcastable = 1;
                for (uint8_t ni = 0; ni < n_nonaggs && all_broadcastable; ni++) {
                    if (!can_atom_broadcast(nonagg_exprs[ni]))
                        all_broadcastable = 0;
                }
                if (all_broadcastable) {
                    for (uint8_t ni = 0; ni < n_nonaggs; ni++) {
                        ray_t* col = atom_broadcast_vec(nonagg_exprs[ni], n_groups);
                        if (!col) {
                            /* can_atom_broadcast vetted these — anything
                             * after that is an OOM in atom_broadcast_vec. */
                            ray_release(result); ray_release(tbl);
                            return ray_error("oom", NULL);
                        }
                        result = ray_table_add_col(result, nonagg_names[ni], col);
                        ray_release(col);
                        if (RAY_IS_ERR(result)) {
                            ray_release(tbl);
                            return result;
                        }
                    }
                    goto nonagg_done;
                }
            }

            /* Resolve key sym — gated to single scalar key above. */
            int64_t ks = -1;
            if (by_expr->type == -RAY_SYM && (by_expr->attrs & RAY_ATTR_NAME))
                ks = by_expr->i64;
            else if (by_expr->type == RAY_SYM && ray_len(by_expr) == 1)
                ks = ((int64_t*)ray_data(by_expr))[0];

            if (ks < 0) {
                ray_release(result); ray_release(tbl);
                return ray_error("domain", NULL);
            }

            ray_t* orig_key = ray_table_get_col(tbl, ks);
            ray_t* grp_key  = ray_table_get_col(result, ks);
            int64_t nrows = orig_key ? orig_key->len : 0;

            if (!orig_key || !grp_key) {
                ray_release(result); ray_release(tbl);
                return ray_error("domain", NULL);
            }

            if (n_groups > 0 && nrows > 0) {
                int8_t okt = orig_key->type;
                int8_t gkt = grp_key->type;
                if (RAY_IS_PARTED(okt)) okt = (int8_t)RAY_PARTED_BASETYPE(okt);
                if (RAY_IS_PARTED(gkt)) gkt = (int8_t)RAY_PARTED_BASETYPE(gkt);

                /* Type-aware key element reader.  Normalizes any
                 * comparable scalar key into an int64_t so linear
                 * scans can use equality.  For floats we bitcast so
                 * NaN and -0/+0 match the DAG's hash-equality. */
                #define KEY_READ(dst, vec, base_type, idx) do {                \
                    const void* _d = ray_data(vec);                            \
                    switch (base_type) {                                       \
                    case RAY_BOOL:                                             \
                    case RAY_U8:   (dst) = ((const uint8_t* )_d)[idx]; break;  \
                    case RAY_I16:  (dst) = ((const int16_t* )_d)[idx]; break;  \
                    case RAY_I32:  (dst) = ((const int32_t* )_d)[idx]; break;  \
                    case RAY_I64:  (dst) = ((const int64_t* )_d)[idx]; break;  \
                    case RAY_F32: { uint32_t _u;                               \
                        memcpy(&_u, &((const float*)_d)[idx], 4);              \
                        (dst) = (int64_t)_u; break; }                          \
                    case RAY_F64: { int64_t _u;                                \
                        memcpy(&_u, &((const double*)_d)[idx], 8);             \
                        (dst) = _u; break; }                                   \
                    case RAY_DATE: case RAY_TIME:                              \
                        (dst) = ((const int32_t*)_d)[idx]; break;              \
                    case RAY_TIMESTAMP:                                        \
                        (dst) = ((const int64_t*)_d)[idx]; break;              \
                    case RAY_SYM:                                              \
                        (dst) = ray_read_sym(_d, (idx), (base_type),           \
                                             (vec)->attrs); break;             \
                    default: {                                                 \
                        /* Unsupported key type: signal via sentinel so the    \
                         * caller's type-mismatch guard catches it.  Should    \
                         * not actually reach here because okt == gkt is       \
                         * checked above and only known types pass. */         \
                        (dst) = 0; break;                                      \
                    }                                                          \
                    }                                                          \
                } while (0)

                /* Whitelist of key types supported by KEY_READ.  Any
                 * other type (LIST, STR, GUID, unknown) must error out —
                 * otherwise KEY_READ silently returns 0 and collapses
                 * all rows into a single (wrong) group.  LIST/STR/GUID
                 * are already routed through use_eval_group earlier;
                 * this is the last-line defense for future additions. */
                int key_supported =
                    (okt == RAY_BOOL || okt == RAY_U8   ||
                     okt == RAY_I16  || okt == RAY_I32  || okt == RAY_I64 ||
                     okt == RAY_F32  || okt == RAY_F64  ||
                     okt == RAY_DATE || okt == RAY_TIME || okt == RAY_TIMESTAMP ||
                     okt == RAY_SYM);
                if (!key_supported) {
                    ray_release(result); ray_release(tbl);
                    return ray_error("nyi", "non-agg scatter: unsupported group key type");
                }

                /* The DAG group result key column must have a base
                 * type comparable to the input.  If types differ
                 * unexpectedly, fall back to error rather than mis-
                 * compare. */
                if (okt != gkt) {
                    ray_release(result); ray_release(tbl);
                    return ray_error("type", "group key type mismatch");
                }

                /* Allocations — any failure errors out rather than
                 * silently returning partial results. */
                ray_t* gk_hdr  = ray_alloc((size_t)n_groups * sizeof(int64_t));
                ray_t* rg_hdr  = ray_alloc((size_t)nrows    * sizeof(int64_t));
                ray_t* cnt_hdr = ray_alloc((size_t)n_groups * sizeof(int64_t));
                ray_t* off_hdr = ray_alloc((size_t)n_groups * sizeof(int64_t));
                ray_t* pos_hdr = ray_alloc((size_t)n_groups * sizeof(int64_t));
                if (!gk_hdr || !rg_hdr || !cnt_hdr || !off_hdr || !pos_hdr) {
                    if (gk_hdr)  ray_free(gk_hdr);
                    if (rg_hdr)  ray_free(rg_hdr);
                    if (cnt_hdr) ray_free(cnt_hdr);
                    if (off_hdr) ray_free(off_hdr);
                    if (pos_hdr) ray_free(pos_hdr);
                    ray_release(result); ray_release(tbl);
                    return ray_error("oom", NULL);
                }
                int64_t* gk      = (int64_t*)ray_data(gk_hdr);
                int64_t* row_gid = (int64_t*)ray_data(rg_hdr);
                int64_t* grp_cnt = (int64_t*)ray_data(cnt_hdr);
                int64_t* offsets = (int64_t*)ray_data(off_hdr);
                int64_t* pos     = (int64_t*)ray_data(pos_hdr);

                /* Copy group key values from the (possibly sliced) result */
                for (int64_t gi = 0; gi < n_groups; gi++)
                    KEY_READ(gk[gi], grp_key, gkt, gi);

                /* Build row→group_id map.  Rows whose key isn't in the
                 * surviving group set get row_gid = -1 and are skipped.
                 *
                 * For high group cardinality (n_groups large), the naive
                 * O(nrows * n_groups) double loop dominated runtime —
                 * 5M * 730K ≈ 4T comparisons.  Build a value→gid hash
                 * instead so each row is one O(1) probe. */
                int rgid_did_mask = 0;
                {
                    /* Capacity: 2 * n_groups rounded up to power of 2.
                     * Slot stores gid+1 (0 = empty) and the int64 key. */
                    uint64_t cap = (uint64_t)n_groups * 2;
                    if (cap < 32) cap = 32;
                    uint64_t c = 1;
                    while (c && c < cap) c <<= 1;
                    if (!c) {
                        ray_free(gk_hdr); ray_free(rg_hdr); ray_free(cnt_hdr);
                        ray_free(off_hdr); ray_free(pos_hdr);
                        ray_release(result); ray_release(tbl);
                        return ray_error("oom", NULL);
                    }
                    cap = c;
                    uint64_t mask = cap - 1;
                    ray_t* gk_keys_hdr = NULL;
                    ray_t* gk_idx_hdr  = NULL;
                    int64_t* hk_keys = (int64_t*)scratch_alloc(&gk_keys_hdr,
                        (size_t)cap * sizeof(int64_t));
                    int32_t* hk_gid_p1 = (int32_t*)scratch_calloc(&gk_idx_hdr,
                        (size_t)cap * sizeof(int32_t));
                    if (!hk_keys || !hk_gid_p1) {
                        if (gk_keys_hdr) scratch_free(gk_keys_hdr);
                        if (gk_idx_hdr)  scratch_free(gk_idx_hdr);
                        ray_free(gk_hdr); ray_free(rg_hdr); ray_free(cnt_hdr);
                        ray_free(off_hdr); ray_free(pos_hdr);
                        ray_release(result); ray_release(tbl);
                        return ray_error("oom", NULL);
                    }

                    /* If n_groups exceeds the int32 sentinel range we'd
                     * lose distinct gids — fall back to the int64 store
                     * (rare: n_groups > ~2.1 B).  Otherwise i32+1 fits. */
                    int use_i64_gid = (n_groups >= ((int64_t)1 << 31) - 1);
                    ray_t* gk64_hdr = NULL;
                    int64_t* hk_gid64 = NULL;
                    if (use_i64_gid) {
                        hk_gid64 = (int64_t*)scratch_calloc(&gk64_hdr,
                            (size_t)cap * sizeof(int64_t));
                        if (!hk_gid64) {
                            scratch_free(gk_keys_hdr); scratch_free(gk_idx_hdr);
                            ray_free(gk_hdr); ray_free(rg_hdr); ray_free(cnt_hdr);
                            ray_free(off_hdr); ray_free(pos_hdr);
                            ray_release(result); ray_release(tbl);
                            return ray_error("oom", NULL);
                        }
                    }

                    /* Insert (gk[gi] -> gi) into the hash. */
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        int64_t k = gk[gi];
                        uint64_t h = (uint64_t)k * 0x9E3779B97F4A7C15ULL;
                        h ^= h >> 33;
                        uint64_t s = h & mask;
                        for (;;) {
                            int64_t cur_p1 = use_i64_gid ? hk_gid64[s]
                                                         : (int64_t)hk_gid_p1[s];
                            if (cur_p1 == 0) {
                                if (use_i64_gid) hk_gid64[s] = gi + 1;
                                else hk_gid_p1[s] = (int32_t)(gi + 1);
                                hk_keys[s] = k;
                                break;
                            }
                            if (hk_keys[s] == k) break; /* dup gk — keep first */
                            s = (s + 1) & mask;
                        }
                    }

                    /* Probe each row to assign its gid.  Parallelise when
                     * the input is large enough to amortise dispatch
                     * overhead — the hash is read-only at this point so
                     * workers don't need to synchronise. */
                    ray_pool_t* pool = ray_pool_get();
                    if (pool && nrows >= 200000 && ray_pool_total_workers(pool) >= 2) {
                        rgid_probe_ctx_t pctx = {
                            .hk_keys       = hk_keys,
                            .hk_gid_p1     = use_i64_gid ? NULL : hk_gid_p1,
                            .hk_gid64      = use_i64_gid ? hk_gid64 : NULL,
                            .mask          = mask,
                            .orig_key_data = ray_data(orig_key),
                            .okt           = okt,
                            .okt_attrs     = orig_key->attrs,
                            .row_gid       = row_gid,
                            .selection     = saved_selection,
                            .sel_flg       = NULL,
                            .sel_offs      = NULL,
                            .sel_idx       = NULL,
                            .sel_n_segs    = 0,
                        };
                        if (saved_selection) {
                            ray_rowsel_t* sm = ray_rowsel_meta(saved_selection);
                            pctx.sel_flg    = ray_rowsel_flags(saved_selection);
                            pctx.sel_offs   = ray_rowsel_offsets(saved_selection);
                            pctx.sel_idx    = ray_rowsel_idx(saved_selection);
                            pctx.sel_n_segs = sm->n_segs;
                        }
                        ray_pool_dispatch(pool, rgid_probe_fn, &pctx, nrows);
                        rgid_did_mask = (saved_selection != NULL);
                    } else {
                        for (int64_t r = 0; r < nrows; r++) {
                            int64_t rv;
                            KEY_READ(rv, orig_key, okt, r);
                            uint64_t h = (uint64_t)rv * 0x9E3779B97F4A7C15ULL;
                            h ^= h >> 33;
                            uint64_t s = h & mask;
                            int64_t found = -1;
                            for (;;) {
                                int64_t cur_p1 = use_i64_gid ? hk_gid64[s]
                                                             : (int64_t)hk_gid_p1[s];
                                if (cur_p1 == 0) break;
                                if (hk_keys[s] == rv) { found = cur_p1 - 1; break; }
                                s = (s + 1) & mask;
                            }
                            row_gid[r] = found;
                        }
                    }

                    scratch_free(gk_keys_hdr);
                    scratch_free(gk_idx_hdr);
                    if (gk64_hdr) scratch_free(gk64_hdr);
                }
                #undef KEY_READ

                /* When path A was taken (no materialisation), the probe
                 * above looked up gids for every row in the original
                 * (unfiltered) table — including rows that the WHERE
                 * clause filtered out.  Mask those rows to -1 here so
                 * downstream count_distinct (and grp_cnt) only count
                 * the surviving rows.  Walks the morsel-segmented
                 * rowsel directly to avoid building a full bitmap.
                 *
                 * When the parallel rgid_probe path was taken with the
                 * selection threaded through (rgid_did_mask), this
                 * mask was already applied per-segment inside the probe
                 * worker — skip the redundant serial walk. */
                if (saved_selection && !rgid_did_mask) {
                    ray_rowsel_t*   sm   = ray_rowsel_meta(saved_selection);
                    const uint8_t*  flg  = ray_rowsel_flags(saved_selection);
                    const uint32_t* offs = ray_rowsel_offsets(saved_selection);
                    const uint16_t* lidx = ray_rowsel_idx(saved_selection);
                    for (uint32_t seg = 0; seg < sm->n_segs; seg++) {
                        int64_t s_lo = (int64_t)seg * RAY_MORSEL_ELEMS;
                        int64_t s_hi = s_lo + RAY_MORSEL_ELEMS;
                        if (s_hi > nrows) s_hi = nrows;
                        uint8_t f = flg[seg];
                        if (f == RAY_SEL_NONE) {
                            for (int64_t r = s_lo; r < s_hi; r++) row_gid[r] = -1;
                        } else if (f == RAY_SEL_ALL) {
                            /* every row in this segment passed — leave gid */
                        } else { /* RAY_SEL_MIX */
                            uint8_t in_seg[RAY_MORSEL_ELEMS / 8] = {0};
                            uint32_t off  = offs[seg];
                            uint32_t cnt  = offs[seg + 1] - off;
                            for (uint32_t i = 0; i < cnt; i++) {
                                uint16_t loc = lidx[off + i];
                                in_seg[loc >> 3] |= (uint8_t)(1u << (loc & 7));
                            }
                            for (int64_t r = s_lo; r < s_hi; r++) {
                                uint16_t loc = (uint16_t)(r - s_lo);
                                if (!(in_seg[loc >> 3] & (1u << (loc & 7))))
                                    row_gid[r] = -1;
                            }
                        }
                    }
                }

                /* Decide whether the per-group-slice bookkeeping
                 * (grp_cnt / offsets / pos / idx_buf) is needed.  It
                 * powers count_distinct_per_group_buf, the streaming
                 * aggr-unary path, nonagg_eval_per_group_buf, and the
                 * full-table-eval+gather path.  When ALL non-aggs are
                 * `count(distinct col_ref)` AND the n_groups gate
                 * routes them to the global-hash kernel, none of those
                 * consumers run — and building the slice index is dead
                 * weight (~15-20 ms on Q14).
                 *
                 * The global-hash path is taken when:
                 *   - the non-agg matches `match_count_distinct`,
                 *   - the inner expression is a column ref (SYM atom
                 *     with NAME attr), and
                 *   - n_groups > 50 000 (the per-group-slice cross-
                 *     over from the threshold dispatch above).
                 *
                 * If any non-agg falls outside that, we still need the
                 * index. */
                int needs_slice_idx = 0;
                for (uint8_t ni = 0; ni < n_nonaggs && !needs_slice_idx; ni++) {
                    ray_t* cd_inner = match_count_distinct(nonagg_exprs[ni]);
                    int simple_cd_global = (cd_inner &&
                                            cd_inner->type == -RAY_SYM &&
                                            (cd_inner->attrs & RAY_ATTR_NAME) &&
                                            n_groups > 50000);
                    if (!simple_cd_global) needs_slice_idx = 1;
                }

                int64_t* idx_buf = NULL;
                ray_t*   idx_hdr = NULL;
                if (needs_slice_idx) {
                    /* Parallel histogram + scatter when the row count is
                     * large enough to amortise dispatch overhead.  For
                     * 5 M-row Q11 the serial loop was 8-10 ms; the
                     * parallel path drops it to ~0.5 ms at 28 workers. */
                    ray_pool_t* idx_pool = ray_pool_get();
                    int64_t total = 0;
                    int parallel_idx_done = 0;
                    if (idx_pool && nrows >= 200000 &&
                        ray_pool_total_workers(idx_pool) >= 2 &&
                        n_groups > 0 && n_groups <= 65536)
                    {
                        int64_t grain = (int64_t)RAY_DISPATCH_MORSELS *
                                        RAY_MORSEL_ELEMS;
                        int64_t n_tasks = (nrows + grain - 1) / grain;
                        if (n_tasks > 65536) {
                            n_tasks = 65536;
                            grain = (nrows + n_tasks - 1) / n_tasks;
                        }
                        ray_t* hist_hdr = NULL;
                        ray_t* cur_hdr  = NULL;
                        int64_t* hist = (int64_t*)scratch_calloc(&hist_hdr,
                            (size_t)n_tasks * (size_t)n_groups *
                            sizeof(int64_t));
                        int64_t* cur  = (int64_t*)scratch_alloc(&cur_hdr,
                            (size_t)n_tasks * (size_t)n_groups *
                            sizeof(int64_t));
                        if (hist && cur) {
                            idxbuf_par_ctx_t pctx = {
                                .row_gid  = row_gid,
                                .hist     = hist,
                                .cursor   = cur,
                                .idx_buf  = NULL,
                                .n_groups = n_groups,
                                .grain    = grain,
                            };
                            ray_pool_dispatch(idx_pool, idxbuf_hist_fn,
                                              &pctx, nrows);

                            /* Prefix: per-group total + per-task cursor. */
                            for (int64_t gi = 0; gi < n_groups; gi++) {
                                int64_t cum = total;
                                for (int64_t t = 0; t < n_tasks; t++) {
                                    int64_t c = hist[t * n_groups + gi];
                                    cur[t * n_groups + gi] = cum;
                                    cum += c;
                                }
                                grp_cnt[gi] = cum - total;
                                offsets[gi] = total;
                                total = cum;
                            }

                            idx_hdr = ray_alloc((size_t)total *
                                                sizeof(int64_t));
                            if (!idx_hdr) {
                                scratch_free(hist_hdr); scratch_free(cur_hdr);
                                ray_free(gk_hdr); ray_free(rg_hdr);
                                ray_free(cnt_hdr); ray_free(off_hdr);
                                ray_free(pos_hdr);
                                ray_release(result); ray_release(tbl);
                                return ray_error("oom", NULL);
                            }
                            idx_buf = (int64_t*)ray_data(idx_hdr);
                            pctx.idx_buf = idx_buf;
                            ray_pool_dispatch(idx_pool, idxbuf_scat_fn,
                                              &pctx, nrows);
                            parallel_idx_done = 1;
                        }
                        if (hist_hdr) scratch_free(hist_hdr);
                        if (cur_hdr)  scratch_free(cur_hdr);
                    }

                    if (!parallel_idx_done) {
                        memset(grp_cnt, 0, (size_t)n_groups * sizeof(int64_t));
                        for (int64_t r = 0; r < nrows; r++)
                            if (row_gid[r] >= 0) grp_cnt[row_gid[r]]++;

                        total = 0;
                        for (int64_t gi = 0; gi < n_groups; gi++)
                            total += grp_cnt[gi];
                        idx_hdr = ray_alloc((size_t)total * sizeof(int64_t));
                        if (!idx_hdr) {
                            ray_free(gk_hdr); ray_free(rg_hdr); ray_free(cnt_hdr);
                            ray_free(off_hdr); ray_free(pos_hdr);
                            ray_release(result); ray_release(tbl);
                            return ray_error("oom", NULL);
                        }
                        idx_buf = (int64_t*)ray_data(idx_hdr);

                        offsets[0] = 0;
                        for (int64_t gi = 1; gi < n_groups; gi++)
                            offsets[gi] = offsets[gi - 1] + grp_cnt[gi - 1];

                        memcpy(pos, offsets,
                               (size_t)n_groups * sizeof(int64_t));
                        for (int64_t r = 0; r < nrows; r++) {
                            int64_t gi = row_gid[r];
                            if (gi >= 0) idx_buf[pos[gi]++] = r;
                        }
                    }
                }

                ray_t* scatter_err = NULL;
                for (uint8_t ni = 0; ni < n_nonaggs && !scatter_err; ni++) {
                    /* Per-group count(distinct) — dispatch directly to
                     * exec_count_distinct on each group's slice using
                     * the same idx_buf+offsets+grp_cnt layout the
                     * streaming-AGG branch uses.
                     *
                     * High-cardinality grouping: try the single-pass
                     * global-hash kernel first.  Falls back to the
                     * per-group slice path on type miss / error. */
                    ray_t* cd_inner = match_count_distinct(nonagg_exprs[ni]);
                    if (cd_inner) {
                        ray_t* col = NULL;
                        /* Resolve the inner column for the global-hash
                         * fast path.  Direct column refs hit the path;
                         * computed expressions use the per-group fallback. */
                        ray_t* src_for_global = NULL;
                        int    src_owned = 0;
                        if (cd_inner->type == -RAY_SYM &&
                            (cd_inner->attrs & RAY_ATTR_NAME)) {
                            src_for_global = ray_table_get_col(tbl, cd_inner->i64);
                        }
                        if (src_for_global) {
                            /* Path selection: global-hash kernel scales
                             * with n_rows (per-row probe of one shared
                             * hash table); per-group-slice scales with
                             * n_groups (per-group setup + small dedup).
                             * Empirically the cross-over is around 50 K
                             * groups on the local hardware — beyond
                             * that, per-group setup overhead dominates. */
                            if (n_groups <= 50000) {
                                col = count_distinct_per_group_buf(
                                    cd_inner, tbl, idx_buf, offsets, grp_cnt, n_groups);
                            } else {
                                col = ray_count_distinct_per_group(
                                    src_for_global, row_gid, nrows, n_groups);
                            }
                            /* col == NULL → unsupported type, fall through. */
                        }
                        if (src_owned && src_for_global) ray_release(src_for_global);
                        if (!col) {
                            col = count_distinct_per_group_buf(
                                cd_inner, tbl, idx_buf, offsets, grp_cnt, n_groups);
                        }
                        if (RAY_IS_ERR(col)) { scatter_err = col; break; }
                        result = ray_table_add_col(result, nonagg_names[ni], col);
                        ray_release(col);
                        if (RAY_IS_ERR(result)) {
                            scatter_err = result; result = NULL; break;
                        }
                        continue;
                    }

                    /* Streaming-style fast path for `(aggr_fn col_or_expr)`
                     * where aggr_fn is RAY_FN_AGGR + RAY_UNARY (sum/avg/...,
                     * med/dev/var/stddev/...).  Bypasses the full-table eval
                     * + non-row-aligned fallback by slicing the source per
                     * group and calling the unary fn directly into a typed
                     * vec.  Equivalent perf-class to the streaming AGG path
                     * the eval-fallback uses for the same shapes. */
                    if (is_streaming_aggr_unary_call(nonagg_exprs[ni])) {
                        ray_t* col = aggr_unary_per_group_buf(
                            nonagg_exprs[ni], tbl,
                            idx_buf, offsets, grp_cnt, n_groups);
                        if (RAY_IS_ERR(col)) { scatter_err = col; break; }
                        result = ray_table_add_col(result, nonagg_names[ni], col);
                        ray_release(col);
                        if (RAY_IS_ERR(result)) {
                            scatter_err = result; result = NULL; break;
                        }
                        continue;
                    }

                    if (is_agg_expr(nonagg_exprs[ni])) {
                        ray_t* per_group = nonagg_eval_per_group_buf(
                            nonagg_exprs[ni], tbl, idx_buf, offsets, grp_cnt, n_groups);
                        if (RAY_IS_ERR(per_group)) {
                            scatter_err = per_group; break;
                        }
                        result = ray_table_add_col(result, nonagg_names[ni], per_group);
                        ray_release(per_group);
                        if (RAY_IS_ERR(result)) {
                            scatter_err = result; result = NULL; break;
                        }
                        continue;
                    }

                    if (ray_env_push_scope() != RAY_OK) {
                        scatter_err = ray_error("oom", NULL); break;
                    }
                    expr_bind_table_names(nonagg_exprs[ni], tbl);
                    ray_t* full_val = ray_eval(nonagg_exprs[ni]);
                    ray_env_pop_scope();
                    if (!full_val || RAY_IS_ERR(full_val)) {
                        scatter_err = full_val ? full_val : ray_error("domain", NULL);
                        break;
                    }

                    ray_t* list_col = ray_alloc(n_groups * sizeof(ray_t*));
                    if (!list_col) {
                        ray_release(full_val);
                        scatter_err = ray_error("oom", NULL); break;
                    }
                    list_col->type = RAY_LIST;
                    /* Track filled length incrementally: ray_release of
                     * a RAY_LIST walks exactly v->len children, so
                     * keeping len in sync with the number of initialized
                     * slots lets error paths free without touching
                     * uninitialized memory — and avoids a memset. */
                    list_col->len = 0;
                    ray_t** list_out = (ray_t**)ray_data(list_col);

                    /* Decide per-group disposition of full_val:
                     *   - expression references a column → result must
                     *     be row-aligned; otherwise that's a bug and
                     *     we error out rather than silently broadcast.
                     *   - constant expression (no column refs) →
                     *     broadcast the value into every group cell. */
                    int refs_column = expr_refs_row_column(nonagg_exprs[ni], tbl);
                    int is_indexable =
                        ray_is_vec(full_val) || full_val->type == RAY_LIST;
                    int full_is_row_aligned =
                        is_indexable && full_val->len == nrows;

                    if (refs_column && !full_is_row_aligned) {
                        /* Non-streaming fallback: the expression didn't
                         * produce a row-aligned full-table result (e.g. a
                         * user lambda collapsed a vector to a scalar), so
                         * collect per-group and post-apply.  Cells can be
                         * any shape; homogeneous-scalar cells collapse to
                         * a typed vec. */
                        ray_release(full_val);
                        ray_release(list_col);  /* len=0, walks nothing */
                        ray_t* per_group = nonagg_eval_per_group_buf(
                            nonagg_exprs[ni], tbl, idx_buf, offsets, grp_cnt, n_groups);
                        if (RAY_IS_ERR(per_group)) {
                            scatter_err = per_group; break;
                        }
                        /* core produces typed vec or list as appropriate */
                        result = ray_table_add_col(result, nonagg_names[ni], per_group);
                        ray_release(per_group);
                        if (RAY_IS_ERR(result)) {
                            scatter_err = result; result = NULL; break;
                        }
                        continue;
                    }

                    /* R8 fallback: a non-literal expression that
                     * eval-collapses to an atom (constant within scope
                     * but not a parser-direct literal) takes the existing
                     * per-cell LIST broadcast.  The all-literal fast path
                     * at the top of the n_nonaggs block already handles
                     * the parser-literal case for Q35-shaped queries. */

                    int gather_ok = 1;
                    for (int64_t gi = 0; gi < n_groups; gi++) {
                        ray_t* cell;
                        if (full_is_row_aligned) {
                            cell = gather_by_idx(full_val,
                                &idx_buf[offsets[gi]], grp_cnt[gi]);
                            if (!cell || RAY_IS_ERR(cell)) {
                                gather_ok = 0;
                                break;
                            }
                        } else {
                            /* Constant (no column refs): broadcast */
                            ray_retain(full_val);
                            cell = full_val;
                        }
                        list_out[gi] = cell;
                        list_col->len = gi + 1;  /* commit slot */
                    }
                    ray_release(full_val);

                    if (!gather_ok) {
                        ray_release(list_col);  /* releases exactly len filled slots */
                        scatter_err = ray_error("oom", NULL); break;
                    }

                    result = ray_table_add_col(result, nonagg_names[ni], list_col);
                    ray_release(list_col);
                    if (RAY_IS_ERR(result)) {
                        scatter_err = result; result = NULL; break;
                    }
                }

                ray_free(gk_hdr); ray_free(rg_hdr); ray_free(cnt_hdr);
                ray_free(off_hdr); ray_free(pos_hdr);
                if (idx_hdr) ray_free(idx_hdr);

                if (scatter_err) {
                    if (result) ray_release(result);
                    ray_release(tbl);
                    return scatter_err;
                }
            } else {
                /* Empty group set: add empty LIST columns so the
                 * output schema still includes the user-declared
                 * non-agg columns. */
                for (uint8_t ni = 0; ni < n_nonaggs; ni++) {
                    ray_t* empty_list = ray_list_new(0);
                    if (!empty_list || RAY_IS_ERR(empty_list)) {
                        ray_release(result); ray_release(tbl);
                        return empty_list ? empty_list : ray_error("oom", NULL);
                    }
                    result = ray_table_add_col(result, nonagg_names[ni], empty_list);
                    ray_release(empty_list);
                    if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }
                }
            }
        nonagg_done: ;  /* R8 fast-path target; nothing else to do here */
        }
    }

    ray_release(tbl);

    /* Post-process: apply sort/take for group-by queries.  Runs
     * last so non-agg LIST columns are already in the result,
     * allowing sort clauses to reference non-agg output columns. */
    if (by_expr && (has_sort || take_expr))
        result = apply_sort_take(result, dict_elems, dict_n, asc_id, desc_id, take_id);

    if (by_sym_vec_owned) ray_release(by_sym_vec_owned);
    if (saved_selection) ray_release(saved_selection);

    return result;
}

/* (xbar col bucket) — time/value bucketing: floor(col/bucket)*bucket */
/* Parallel inner loops for ray_xbar_fn fast path.  Dispatch one task per
 * morsel range so 5M-row temporal columns scale across the worker pool
 * (Q43's xbar was ~6ms serial, ~0.5ms with 28 workers). */
typedef struct {
    int8_t out_type;
    const void* in;
    void* out;
    int64_t b;
    int pow2;
} xbar_par_ctx_t;

static void xbar_par_fn(void* vctx, uint32_t worker_id,
                        int64_t start, int64_t end) {
    (void)worker_id;
    xbar_par_ctx_t* c = (xbar_par_ctx_t*)vctx;
    int64_t b = c->b;
    if (c->out_type == RAY_I64 || c->out_type == RAY_TIMESTAMP) {
        const int64_t* in = (const int64_t*)c->in;
        int64_t* o = (int64_t*)c->out;
        if (c->pow2) {
            int64_t mask = ~(b - 1);
            for (int64_t i = start; i < end; i++) o[i] = in[i] & mask;
        } else {
            for (int64_t i = start; i < end; i++) {
                int64_t a = in[i];
                int64_t q = a / b;
                if ((a ^ b) < 0 && q * b != a) q--;
                o[i] = q * b;
            }
        }
    } else if (c->out_type == RAY_I32 || c->out_type == RAY_DATE ||
               c->out_type == RAY_TIME) {
        const int32_t* in = (const int32_t*)c->in;
        int32_t* o = (int32_t*)c->out;
        int32_t b32 = (int32_t)b;
        if (c->pow2) {
            int32_t mask = (int32_t)~((uint32_t)b32 - 1);
            for (int64_t i = start; i < end; i++) o[i] = in[i] & mask;
        } else {
            for (int64_t i = start; i < end; i++) {
                int32_t a = in[i];
                int32_t q = a / b32;
                if ((a ^ b32) < 0 && q * b32 != a) q--;
                o[i] = q * b32;
            }
        }
    } else { /* RAY_I16 */
        const int16_t* in = (const int16_t*)c->in;
        int16_t* o = (int16_t*)c->out;
        int16_t b16 = (int16_t)b;
        for (int64_t i = start; i < end; i++) {
            int16_t a = in[i];
            int16_t q = a / b16;
            if ((a ^ b16) < 0 && q * b16 != a) q--;
            o[i] = q * b16;
        }
    }
}

ray_t* ray_xbar_fn(ray_t* col, ray_t* bucket) {
    /* Vectorised fast path for `(xbar VEC scalar_int)` on integer or
     * temporal columns.  The generic atomic_map_binary path was
     * allocating one ray_t* atom per row and calling ray_xbar_fn
     * recursively — at 5M rows this dominates (≥100 ms).  A direct
     * tight loop computes floor-div + multiply per element with no
     * allocations.  When the bucket is a power of two we lower the
     * divide further to mask + arithmetic.  Parallelised across the
     * worker pool for large columns.
     *
     * Short-circuited only when both bucket and col are well-typed;
     * everything else falls through to the recursive
     * atomic_map_binary path. */
    if (col && ray_is_vec(col) && bucket && ray_is_atom(bucket) &&
        (bucket->type == -RAY_I64 || bucket->type == -RAY_I32 ||
         bucket->type == -RAY_I16) &&
        (col->type == RAY_I64 || col->type == RAY_I32 ||
         col->type == RAY_I16 || col->type == RAY_TIMESTAMP ||
         col->type == RAY_DATE || col->type == RAY_TIME) &&
        !RAY_ATOM_IS_NULL(bucket)) {
        int64_t b = bucket->i64;
        if (b == 0) return ray_error("domain", NULL);
        int64_t n = col->len;
        ray_t* out = ray_vec_new(col->type, n);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        out->len = n;

        int8_t out_type = col->type;
        int pow2 = 0;
        if (out_type == RAY_I64 || out_type == RAY_TIMESTAMP) {
            pow2 = (b > 0 && (b & (b - 1)) == 0);
        } else if (out_type == RAY_I32 || out_type == RAY_DATE || out_type == RAY_TIME) {
            int32_t b32 = (int32_t)b;
            pow2 = (b32 > 0 && ((uint32_t)b32 & ((uint32_t)b32 - 1)) == 0);
        }

        ray_pool_t* pool = ray_pool_get();
        if (pool && n >= 200000 && ray_pool_total_workers(pool) >= 2) {
            xbar_par_ctx_t ctx = {
                .out_type = out_type,
                .in       = ray_data(col),
                .out      = ray_data(out),
                .b        = b,
                .pow2     = pow2,
            };
            ray_pool_dispatch(pool, xbar_par_fn, &ctx, n);
        } else {
            xbar_par_ctx_t ctx = {
                .out_type = out_type,
                .in       = ray_data(col),
                .out      = ray_data(out),
                .b        = b,
                .pow2     = pow2,
            };
            xbar_par_fn(&ctx, 0, 0, n);
        }

        /* Propagate null bitmap if present.  Walk the source nullmap
         * byte-by-byte and only clobber positions where the source is
         * null — same trick as fix_null_comparisons.  Cheap for the
         * common case of HAS_NULLS attr set with mostly-empty bitmap. */
        if (col->attrs & RAY_ATTR_HAS_NULLS) {
            int64_t off_bits = 0, len_bits = 0;
            const uint8_t* nbits = ray_vec_nullmap_bytes(col, &off_bits, &len_bits);
            if (nbits && (off_bits % 8) == 0) {
                int64_t byte0 = off_bits / 8;
                for (int64_t i = 0; i + 8 <= n; i += 8) {
                    uint8_t bb = nbits[byte0 + (i >> 3)];
                    if (bb) {
                        for (int64_t k = 0; k < 8; k++)
                            if ((bb >> k) & 1)
                                ray_vec_set_null(out, i + k, true);
                    }
                }
                for (int64_t i = (n & ~7); i < n; i++) {
                    if ((nbits[byte0 + (i >> 3)] >> (i & 7)) & 1)
                        ray_vec_set_null(out, i, true);
                }
            } else {
                for (int64_t i = 0; i < n; i++)
                    if (ray_vec_is_null(col, i))
                        ray_vec_set_null(out, i, true);
            }
        }
        return out;
    }

    /* Recursive unwrap for nested collections (list of vectors) */
    if (is_collection(col) || is_collection(bucket))
        return atomic_map_binary(ray_xbar_fn, col, bucket);
    /* Both are integer types (i64, i32, i16) → integer xbar */
    if (is_numeric(col) && is_numeric(bucket) && !is_float_op(col, bucket)) {
        int64_t a = as_i64(col), b = as_i64(bucket);
        if (b == 0 || RAY_ATOM_IS_NULL(col) || RAY_ATOM_IS_NULL(bucket))
            return ray_error("domain", NULL);
        int64_t q = a / b;
        if ((a ^ b) < 0 && q * b != a) q--;
        int64_t result = q * b;
        /* Result type follows the wider of the two operands */
        if (col->type == -RAY_I32 && bucket->type == -RAY_I32) return make_i32((int32_t)result);
        if (col->type == -RAY_I16 && bucket->type == -RAY_I16) return make_i16((int16_t)result);
        return make_i64(result);
    }
    /* Float path: either operand is f64 */
    if (is_numeric(col) && is_numeric(bucket)) {
        if (RAY_ATOM_IS_NULL(col) || RAY_ATOM_IS_NULL(bucket))
            return ray_error("domain", NULL);
        double c = as_f64(col), b = as_f64(bucket);
        if (b == 0.0) return ray_error("domain", NULL);
        double fq = floor(c / b);
        return make_f64(fq * b);
    }
    /* Temporal xbar: col is temporal, bucket is integer or temporal (not float) */
    if (is_temporal(col) && (is_temporal(bucket) ||
        (is_numeric(bucket) && bucket->type != -RAY_F64))) {
        int64_t a = col->i64, b;
        if (is_temporal(bucket)) {
            b = bucket->i64;
            /* Cross-temporal conversion: TIME(ms) bucket on TIMESTAMP(ns) col */
            if (col->type == -RAY_TIMESTAMP && bucket->type == -RAY_TIME)
                b *= 1000000LL;
        } else {
            b = as_i64(bucket);
        }
        if (b == 0 || RAY_ATOM_IS_NULL(bucket)) return ray_error("domain", NULL);
        int64_t q = a / b;
        if ((a ^ b) < 0 && q * b != a) q--;
        int64_t result = q * b;
        if (col->type == -RAY_TIME) return ray_time(result);
        if (col->type == -RAY_DATE) return ray_date(result);
        return ray_timestamp(result);
    }
    return ray_error("type", NULL);
}

/* ══════════════════════════════════════════
 * Update, Insert, Upsert
 * ══════════════════════════════════════════ */

/* Helper: convert a Rayfall list of atoms into a typed column vector by
 * appending to an existing column (for insert/upsert). */
static ray_t* append_atom_to_col(ray_t* col_vec, ray_t* atom) {
    if (RAY_ATOM_IS_NULL(atom)) {
        int64_t idx = col_vec->len;
        uint8_t zero[16] = {0};
        col_vec = ray_vec_append(col_vec, zero);
        if (!RAY_IS_ERR(col_vec))
            ray_vec_set_null(col_vec, idx, true);
        return col_vec;
    }
    int8_t ct = col_vec->type;
    if (ct == RAY_I64) {
        if (atom->type != -RAY_I64)
            return ray_error("type", NULL);
        int64_t v = atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_SYM) {
        if (atom->type != -RAY_SYM)
            return ray_error("type", NULL);
        int64_t v = atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_F64) {
        if (atom->type != -RAY_F64 && atom->type != -RAY_I64)
            return ray_error("type", NULL);
        double v = (atom->type == -RAY_F64) ? atom->f64 : (double)atom->i64;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_BOOL) {
        if (atom->type != -RAY_BOOL)
            return ray_error("type", NULL);
        uint8_t v = atom->b8;
        return ray_vec_append(col_vec, &v);
    } else if (ct == RAY_STR && atom->type == -RAY_STR) {
        const char *sptr = ray_str_ptr(atom);
        size_t slen = ray_str_len(atom);
        return ray_str_vec_append(col_vec, sptr, slen);
    }
    return ray_error("type", NULL);
}

/* (update {col: expr ... from: t [where: pred]})
 * Special form — receives unevaluated dict arg.
 * For rows matching where (or all if no where), evaluate column expressions
 * and replace those column values. Returns a new table. */
/* Forward declarations */

ray_t* ray_update_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("domain", NULL);
    ray_t* dict = args[0];
    if (!dict || dict->type != RAY_DICT)
        return ray_error("type", NULL);

    ray_t* from_expr = dict_get(dict, "from");
    if (!from_expr) return ray_error("domain", NULL);
    /* Detect in-place update: from: 't means quoted symbol */
    int64_t inplace_sym = -1;
    ray_t* tbl = ray_eval(from_expr);
    if (RAY_IS_ERR(tbl)) return tbl;
    if (tbl->type == -RAY_SYM) {
        /* from: 't — resolve symbol to table variable */
        inplace_sym = tbl->i64;
        ray_release(tbl);
        tbl = ray_env_get(inplace_sym);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", NULL);
        ray_retain(tbl);
    }
    if (tbl->type != RAY_TABLE) { ray_release(tbl); return ray_error("type", NULL); }

    ray_t* where_expr = dict_get(dict, "where");
    ray_t* by_expr = dict_get(dict, "by");

    /* UPDATE WITH BY: group, compute aggregate, broadcast back */
    if (by_expr && !where_expr) {
        DICT_VIEW_DECL(updv);
        DICT_VIEW_OPEN(dict, updv);
        if (DICT_VIEW_OVERFLOW(updv)) {
            ray_release(tbl);
            return ray_error("domain", "update clause has too many keys");
        }
        int64_t dict_n = updv_n;
        ray_t** dict_elems = updv;
        int64_t from_id  = ray_sym_intern("from",  4);
        int64_t where_id = ray_sym_intern("where", 5);
        int64_t by_id    = ray_sym_intern("by",    2);

        /* Resolve group key column name.
         * by_expr is a name reference (not evaluated) — extract sym_id directly */
        int64_t by_col_name = -1;
        if (by_expr->type == -RAY_SYM) {
            by_col_name = by_expr->i64;
        }
        if (by_col_name < 0) { ray_release(tbl); return ray_error("type", NULL); }

        /* Find group column in table */
        ray_t* grp_col = ray_table_get_col(tbl, by_col_name);
        if (!grp_col) { ray_release(tbl); return ray_error("domain", NULL); }
        int64_t nrows2 = ray_table_nrows(tbl);

        /* Use ray_group_fn to get group indices: {key: [indices]}.
         * Flatten the resulting RAY_DICT into the legacy interleaved
         * [k0,v0,…] LIST shape this branch was written against. */
        ray_t* groups = NULL;
        {
            ray_t* gd = ray_group_fn(grp_col);
            if (!gd || RAY_IS_ERR(gd)) { ray_release(tbl); return gd ? gd : ray_error("oom", NULL); }
            groups = groups_to_pair_list(gd);
            ray_release(gd);
            if (RAY_IS_ERR(groups)) { ray_release(tbl); return groups; }
        }

        /* Start with a copy of the original table */
        int64_t ncols = ray_table_ncols(tbl);
        ray_t* result = ray_table_new((int32_t)ncols);
        if (RAY_IS_ERR(result)) { ray_release(groups); ray_release(tbl); return result; }
        for (int64_t c = 0; c < ncols; c++) {
            int64_t cn = ray_table_col_name(tbl, c);
            ray_t* col = ray_table_get_col_idx(tbl, c);
            ray_retain(col);
            result = ray_table_add_col(result, cn, col);
            ray_release(col);
            if (RAY_IS_ERR(result)) { ray_release(groups); ray_release(tbl); return result; }
        }

        /* For each aggregate expression, compute per group and broadcast */
        for (int64_t d = 0; d + 1 < dict_n; d += 2) {
            int64_t kid = dict_elems[d]->i64;
            if (kid == from_id || kid == where_id || kid == by_id) continue;
            ray_t* agg_expr = dict_elems[d + 1];

            /* Evaluate the aggregate for each group and broadcast */
            ray_t* grp_items = (ray_t**)ray_data(groups) ? groups : NULL;
            if (!grp_items) { ray_release(result); ray_release(groups); ray_release(tbl); return ray_error("oom", NULL); }
            int64_t ngroups = groups->len / 2;
            ray_t** gdata = (ray_t**)ray_data(groups);

            /* We need to evaluate the aggregate per group.
             * Build the result column by evaluating the expression on each group's subset. */
            ray_t* out_col = ray_vec_new(RAY_I64, nrows2); /* will be resized to correct type */
            if (RAY_IS_ERR(out_col)) { ray_release(result); ray_release(groups); ray_release(tbl); return out_col; }

            int8_t out_type = RAY_I64;
            int first_group = 1;

            for (int64_t gi = 0; gi < ngroups; gi++) {
                ray_t* idx_vec = gdata[gi * 2 + 1]; /* index vector for this group */
                int64_t gsize = ray_len(idx_vec);

                /* Build a sub-table for this group */
                ray_t* sub_tbl = ray_table_new((int32_t)ncols);
                if (RAY_IS_ERR(sub_tbl)) { ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return sub_tbl; }
                for (int64_t c = 0; c < ncols; c++) {
                    int64_t cn = ray_table_col_name(tbl, c);
                    ray_t* full_col = ray_table_get_col_idx(tbl, c);
                    int8_t ct = full_col->type;
                    ray_t* sub_col = ray_vec_new(ct, gsize);
                    if (RAY_IS_ERR(sub_col)) { ray_release(sub_tbl); ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return sub_col; }
                    sub_col->len = gsize;
                    int esz = ray_elem_size(ct);
                    char* src = (char*)ray_data(full_col);
                    char* dst = (char*)ray_data(sub_col);
                    int64_t* idxs = (int64_t*)ray_data(idx_vec);
                    for (int64_t r = 0; r < gsize; r++)
                        memcpy(dst + r * esz, src + idxs[r] * esz, esz);
                    sub_tbl = ray_table_add_col(sub_tbl, cn, sub_col);
                    ray_release(sub_col);
                    if (RAY_IS_ERR(sub_tbl)) { ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return sub_tbl; }
                }

                /* Evaluate expression on sub-table via DAG */
                ray_graph_t* ug = ray_graph_new(sub_tbl);
                ray_op_t* expr_op = compile_expr_dag(ug, agg_expr);
                if (!expr_op) { ray_graph_free(ug); ray_release(sub_tbl); ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return ray_error("domain", NULL); }
                expr_op = ray_optimize(ug, expr_op);
                ray_t* agg_result = ray_execute(ug, expr_op);
                ray_graph_free(ug);
                ray_release(sub_tbl);

                if (RAY_IS_ERR(agg_result)) { ray_release(out_col); ray_release(result); ray_release(groups); ray_release(tbl); return agg_result; }

                /* Determine output type from first group */
                if (first_group) {
                    if (ray_is_atom(agg_result)) out_type = -agg_result->type;
                    else if (ray_is_vec(agg_result)) out_type = agg_result->type;
                    ray_release(out_col);
                    out_col = ray_vec_new(out_type, nrows2);
                    if (RAY_IS_ERR(out_col)) { ray_release(agg_result); ray_release(result); ray_release(groups); ray_release(tbl); return out_col; }
                    out_col->len = nrows2;
                    first_group = 0;
                }

                /* Broadcast aggregate value to all rows in this group */
                int64_t* idxs = (int64_t*)ray_data(idx_vec);
                if (ray_is_atom(agg_result)) {
                    for (int64_t r = 0; r < gsize; r++)
                        store_typed_elem(out_col, idxs[r], agg_result);
                }
                ray_release(agg_result);
            }

            /* Add the new column to the result table */
            result = ray_table_add_col(result, kid, out_col);
            ray_release(out_col);
            if (RAY_IS_ERR(result)) { ray_release(groups); ray_release(tbl); return result; }
        }

        ray_release(groups);
        /* Store in-place if needed */
        if (inplace_sym >= 0) {
            ray_env_set(inplace_sym, result);
        }
        ray_release(tbl);
        return result;
    }

    /* Evaluate WHERE using the DAG to get a boolean mask */
    int64_t nrows = ray_table_nrows(tbl);
    uint8_t* mask = NULL;

    if (where_expr) {
        /* Try DAG compilation first, fall back to eval-level */
        ray_t* mask_vec = NULL;
        ray_graph_t* g = ray_graph_new(tbl);
        if (g) {
            ray_op_t* pred = compile_expr_dag(g, where_expr);
            if (pred) {
                pred = ray_optimize(g, pred);
                mask_vec = ray_execute(g, pred);
            }
            ray_graph_free(g);
        }
        /* Fallback: eval-level predicate evaluation */
        if (!mask_vec || RAY_IS_ERR(mask_vec)) {
            /* Bind column names to column vectors in env, then eval */
            int64_t ncols2 = ray_table_ncols(tbl);
            ray_env_push_scope();
            for (int64_t c = 0; c < ncols2; c++) {
                int64_t cn = ray_table_col_name(tbl, c);
                ray_t* col = ray_table_get_col_idx(tbl, c);
                ray_env_set(cn, col);
            }
            mask_vec = ray_eval(where_expr);
            ray_env_pop_scope();
        }
        if (!mask_vec || RAY_IS_ERR(mask_vec)) { ray_release(tbl); return mask_vec ? mask_vec : ray_error("type", NULL); }
        if (mask_vec->type != RAY_BOOL || mask_vec->len != nrows) {
            ray_release(mask_vec);
            ray_release(tbl);
            return ray_error("type", NULL);
        }
        mask = (uint8_t*)ray_data(mask_vec);
        /* Keep mask_vec alive until we're done */

        /* Build a new table with updated columns */
        int64_t ncols = ray_table_ncols(tbl);
        DICT_VIEW_DECL(updw);
        DICT_VIEW_OPEN(dict, updw);
        if (DICT_VIEW_OVERFLOW(updw)) {
            ray_release(mask_vec); ray_release(tbl);
            return ray_error("domain", "update clause has too many keys");
        }
        int64_t dict_n = updw_n;
        ray_t** dict_elems = updw;
        int64_t from_id = ray_sym_intern("from", 4);
        int64_t where_id = ray_sym_intern("where", 5);

        ray_t* result = ray_table_new(ncols);
        if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }

        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            ray_t* orig_col = ray_table_get_col_idx(tbl, c);

            /* Check if this column has an update expression */
            ray_t* update_expr = NULL;
            for (int64_t d = 0; d + 1 < dict_n; d += 2) {
                int64_t kid = dict_elems[d]->i64;
                if (kid == from_id || kid == where_id) continue;
                if (kid == col_name) { update_expr = dict_elems[d + 1]; break; }
            }

            if (!update_expr) {
                /* No update for this column — copy as-is */
                ray_retain(orig_col);
                result = ray_table_add_col(result, col_name, orig_col);
                ray_release(orig_col);
            } else {
                /* Evaluate the expression for each row and apply to matching rows */
                int8_t ct = orig_col->type;
                ray_t* new_col = ray_vec_new(ct, nrows);
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }

                /* Evaluate expression via DAG, fallback to eval-level */
                ray_t* expr_vec = NULL;
                {
                    ray_graph_t* ug = ray_graph_new(tbl);
                    if (ug) {
                        ray_op_t* expr_op = compile_expr_dag(ug, update_expr);
                        if (expr_op) {
                            expr_op = ray_optimize(ug, expr_op);
                            expr_vec = ray_execute(ug, expr_op);
                        }
                        ray_graph_free(ug);
                    }
                }
                if (!expr_vec || RAY_IS_ERR(expr_vec)) {
                    /* Fallback: eval with column bindings */
                    int64_t ncols_e = ray_table_ncols(tbl);
                    ray_env_push_scope();
                    for (int64_t c2 = 0; c2 < ncols_e; c2++) {
                        int64_t cn = ray_table_col_name(tbl, c2);
                        ray_t* col2 = ray_table_get_col_idx(tbl, c2);
                        ray_env_set(cn, col2);
                    }
                    expr_vec = ray_eval(update_expr);
                    ray_env_pop_scope();
                }
                if (!expr_vec || RAY_IS_ERR(expr_vec)) { ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return expr_vec ? expr_vec : ray_error("type", NULL); }

                /* WHERE update: expression result replaces ONLY masked rows.
                 * When type differs (e.g., I64 col, F64 expr from (* col 1.1)),
                 * keep original column type and cast expr results.
                 * Only numeric promotions are allowed — STR↔numeric is a type error. */
                int8_t expr_type = (expr_vec->type < 0) ? -expr_vec->type : expr_vec->type;
                if (expr_type != ct && expr_type > 0 && ray_is_vec(expr_vec)) {
                    /* Only allow numeric promotions (I64↔F64, I32↔F64) */
                    int is_numeric_promo = (ct == RAY_I64 || ct == RAY_I32 || ct == RAY_F64) &&
                                           (expr_type == RAY_I64 || expr_type == RAY_I32 || expr_type == RAY_F64);
                    if (!is_numeric_promo) {
                        ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl);
                        return ray_error("type", NULL);
                    }
                    /* Copy original column values first */
                    int esz = ray_elem_size(ct);
                    memcpy(ray_data(new_col), ray_data(orig_col), (size_t)(nrows * esz));
                    new_col->len = nrows;
                    /* Overlay masked rows with type conversion */
                    for (int64_t r = 0; r < nrows; r++) {
                        if (!mask[r]) continue;
                        if (ct == RAY_I64 && expr_type == RAY_F64)
                            ((int64_t*)ray_data(new_col))[r] = (int64_t)((double*)ray_data(expr_vec))[r];
                        else if (ct == RAY_I32 && expr_type == RAY_F64)
                            ((int32_t*)ray_data(new_col))[r] = (int32_t)((double*)ray_data(expr_vec))[r];
                        else if (ct == RAY_F64 && expr_type == RAY_I64)
                            ((double*)ray_data(new_col))[r] = (double)((int64_t*)ray_data(expr_vec))[r];
                    }
                    /* Null-bit propagation: memcpy above only copies values,
                     * not the nullmap.  Carry over orig_col's nulls for the
                     * untouched rows, and pull expr_vec's nulls in for the
                     * masked rows.  Without this, casting a null F64 expr
                     * back to an I64 column silently produces 0. */
                    for (int64_t r = 0; r < nrows; r++) {
                        ray_t* src = mask[r] ? expr_vec : orig_col;
                        if (ray_vec_is_null(src, r))
                            ray_vec_set_null(new_col, r, true);
                    }
                    ray_release(expr_vec);
                    result = ray_table_add_col(result, col_name, new_col);
                    ray_release(new_col);
                    if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }
                    continue;
                }

                /* Broadcast scalar atom to full column vector if needed */
                if (expr_vec->type < 0) {
                    /* Type check atom against column type BEFORE broadcast */
                    int ok = (expr_vec->type == -ct);
                    if (!ok && ct == RAY_F64 && expr_vec->type == -RAY_I64) ok = 1;
                    if (!ok && ct == RAY_LIST && expr_vec->type == -RAY_SYM) ok = 1;
                    if (!ok && ct == RAY_SYM && expr_vec->type == -RAY_SYM) ok = 1;
                    if (!ok) {
                        ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl);
                        return ray_error("type", NULL);
                    }
                    /* SYM atom to LIST column: build boxed list, merge with mask */
                    if (ct == RAY_LIST && expr_vec->type == -RAY_SYM) {
                        ray_free(new_col);
                        ray_t* new_list = ray_list_new((int32_t)nrows);
                        if (RAY_IS_ERR(new_list)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_list; }
                        ray_t** orig_elems = (ray_t**)ray_data(orig_col);
                        for (int64_t r = 0; r < nrows; r++) {
                            ray_t* elem = mask[r] ? expr_vec : orig_elems[r];
                            ray_retain(elem);
                            new_list = ray_list_append(new_list, elem);
                            ray_release(elem);
                            if (RAY_IS_ERR(new_list)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_list; }
                        }
                        ray_release(expr_vec);
                        result = ray_table_add_col(result, col_name, new_list);
                        ray_release(new_list);
                        if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }
                        continue;
                    }
                    ray_t* bcast = ray_vec_new(ct, nrows);
                    if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return bcast; }
                    if (ct == RAY_STR && expr_vec->type == -RAY_STR) {
                        const char* sp = ray_str_ptr(expr_vec);
                        size_t sl = ray_str_len(expr_vec);
                        for (int64_t r = 0; r < nrows; r++) {
                            bcast = ray_str_vec_append(bcast, sp, sl);
                            if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return bcast; }
                        }
                    } else {
                        size_t esz = (ct == RAY_BOOL) ? 1 : 8;
                        uint8_t elem[8] = {0};
                        if (ct == RAY_F64 && expr_vec->type == -RAY_I64) {
                            double promoted = (double)expr_vec->i64;
                            memcpy(elem, &promoted, 8);
                        } else {
                            memcpy(elem, &expr_vec->i64, esz);
                        }
                        for (int64_t r = 0; r < nrows; r++) {
                            bcast = ray_vec_append(bcast, elem);
                            if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return bcast; }
                        }
                    }
                    /* Preserve typed-null markers across broadcast.  Without
                     * this, (update {a: 0N from: t}) silently writes plain
                     * zeros into the I64 column — the value bits get copied
                     * but the null bitmap doesn't, so (nil? a) reports false
                     * on what should be null cells. */
                    if (RAY_ATOM_IS_NULL(expr_vec)) {
                        for (int64_t r = 0; r < nrows; r++)
                            ray_vec_set_null(bcast, r, true);
                    }
                    ray_release(expr_vec);
                    expr_vec = bcast;
                }

                /* Promote I64 vector to F64 if column is F64 */
                if (expr_vec->type == RAY_I64 && ct == RAY_F64) {
                    int64_t nr = ray_len(expr_vec);
                    ray_t* promoted = ray_vec_new(RAY_F64, nr);
                    if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return promoted; }
                    int64_t* src_data = (int64_t*)ray_data(expr_vec);
                    for (int64_t r = 0; r < nr; r++) {
                        double v = (double)src_data[r];
                        promoted = ray_vec_append(promoted, &v);
                        if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl); return promoted; }
                    }
                    /* Carry the nullmap across the I64→F64 promotion. */
                    for (int64_t r = 0; r < nr; r++)
                        if (ray_vec_is_null(expr_vec, r))
                            ray_vec_set_null(promoted, r, true);
                    ray_release(expr_vec);
                    expr_vec = promoted;
                }

                /* Type check: expr_vec must match original column type */
                if (expr_vec->type != ct) {
                    ray_release(expr_vec); ray_release(new_col); ray_release(result); ray_release(mask_vec); ray_release(tbl);
                    return ray_error("type", NULL);
                }

                /* Merge: use expr_vec for matching rows, orig_col for non-matching.
                 * Null-bit propagation applies to STR/SYM as well — a null in
                 * either the orig column (unmasked rows) or the expr (masked
                 * rows) must travel into new_col's nullmap. */
                if (ct == RAY_STR) {
                    for (int64_t r = 0; r < nrows; r++) {
                        ray_t* src_vec = mask[r] ? expr_vec : orig_col;
                        size_t slen = 0;
                        const char* sp = ray_str_vec_get(src_vec, r, &slen);
                        new_col = ray_str_vec_append(new_col, sp ? sp : "", sp ? slen : 0);
                        if (RAY_IS_ERR(new_col)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }
                        if (ray_vec_is_null(src_vec, r))
                            ray_vec_set_null(new_col, new_col->len - 1, true);
                    }
                } else if (ct == RAY_SYM) {
                    for (int64_t r = 0; r < nrows; r++) {
                        ray_t* src_vec = mask[r] ? expr_vec : orig_col;
                        int64_t sym_val = ray_read_sym(ray_data(src_vec), r, src_vec->type, src_vec->attrs);
                        new_col = ray_vec_append(new_col, &sym_val);
                        if (RAY_IS_ERR(new_col)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }
                        if (ray_vec_is_null(src_vec, r))
                            ray_vec_set_null(new_col, new_col->len - 1, true);
                    }
                } else {
                    size_t elem_sz = (ct == RAY_BOOL) ? 1 : 8;
                    uint8_t* orig_data = (uint8_t*)ray_data(orig_col);
                    uint8_t* expr_data = (uint8_t*)ray_data(expr_vec);
                    for (int64_t r = 0; r < nrows; r++) {
                        ray_t* src_vec = mask[r] ? expr_vec : orig_col;
                        uint8_t* base  = mask[r] ? expr_data : orig_data;
                        new_col = ray_vec_append(new_col, base + r * elem_sz);
                        if (RAY_IS_ERR(new_col)) { ray_release(expr_vec); ray_release(result); ray_release(mask_vec); ray_release(tbl); return new_col; }
                        /* Propagate null bit from whichever side supplied
                         * the value.  Without this, masking in a typed-null
                         * broadcast would copy zero bytes into the slot but
                         * leave the destination's nullmap clear → silent
                         * loss of null marker. */
                        if (ray_vec_is_null(src_vec, r))
                            ray_vec_set_null(new_col, new_col->len - 1, true);
                    }
                }
                result = ray_table_add_col(result, col_name, new_col);
                ray_release(new_col);
                ray_release(expr_vec);
            }
            if (RAY_IS_ERR(result)) { ray_release(mask_vec); ray_release(tbl); return result; }
        }

        ray_release(mask_vec);
        if (inplace_sym >= 0 && result && !RAY_IS_ERR(result)) {
            ray_env_set(inplace_sym, result);
        }
        ray_release(tbl);
        return result;
    }

    /* No WHERE — update all rows */
    int64_t ncols = ray_table_ncols(tbl);
    DICT_VIEW_DECL(upda);
    DICT_VIEW_OPEN(dict, upda);
    if (DICT_VIEW_OVERFLOW(upda)) {
        ray_release(tbl);
        return ray_error("domain", "update clause has too many keys");
    }
    int64_t dict_n = upda_n;
    ray_t** dict_elems = upda;
    int64_t from_id = ray_sym_intern("from", 4);

    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }

    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(tbl, c);
        ray_t* orig_col = ray_table_get_col_idx(tbl, c);

        ray_t* update_expr = NULL;
        for (int64_t d = 0; d + 1 < dict_n; d += 2) {
            int64_t kid = dict_elems[d]->i64;
            if (kid == from_id) continue;
            if (kid == col_name) { update_expr = dict_elems[d + 1]; break; }
        }

        if (!update_expr) {
            ray_retain(orig_col);
            result = ray_table_add_col(result, col_name, orig_col);
            ray_release(orig_col);
        } else {
            ray_t* expr_vec = NULL;
            {
                ray_graph_t* ug = ray_graph_new(tbl);
                if (ug) {
                    ray_op_t* expr_op = compile_expr_dag(ug, update_expr);
                    if (expr_op) {
                        expr_op = ray_optimize(ug, expr_op);
                        expr_vec = ray_execute(ug, expr_op);
                    }
                    ray_graph_free(ug);
                }
            }
            if (!expr_vec || RAY_IS_ERR(expr_vec)) {
                /* Fallback: eval with column bindings */
                int64_t ncols_f = ray_table_ncols(tbl);
                ray_env_push_scope();
                for (int64_t cf = 0; cf < ncols_f; cf++) {
                    int64_t cn = ray_table_col_name(tbl, cf);
                    ray_t* colf = ray_table_get_col_idx(tbl, cf);
                    ray_env_set(cn, colf);
                }
                expr_vec = ray_eval(update_expr);
                ray_env_pop_scope();
            }
            if (!expr_vec || RAY_IS_ERR(expr_vec)) { ray_release(result); ray_release(tbl); return expr_vec ? expr_vec : ray_error("type", NULL); }

            /* Broadcast scalar atom to full column vector if needed */
            if (expr_vec->type < 0) {
                int64_t nrows = ray_table_nrows(tbl);
                int8_t ct = orig_col->type;
                /* Type check atom against column type BEFORE broadcast */
                int ok = (expr_vec->type == -ct);
                if (!ok && ct == RAY_F64 && expr_vec->type == -RAY_I64) ok = 1;
                /* SYM atom → LIST column (LIST of SYM atoms) */
                if (!ok && ct == RAY_LIST && expr_vec->type == -RAY_SYM) ok = 1;
                if (!ok) {
                    ray_release(expr_vec); ray_release(result); ray_release(tbl);
                    return ray_error("type", NULL);
                }
                /* SYM atom to LIST column: broadcast as boxed list */
                if (ct == RAY_LIST && expr_vec->type == -RAY_SYM) {
                    ray_t* bcast = ray_list_new((int32_t)nrows);
                    if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    for (int64_t r = 0; r < nrows; r++) {
                        ray_retain(expr_vec);
                        bcast = ray_list_append(bcast, expr_vec);
                        ray_release(expr_vec);
                        if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    }
                    ray_release(expr_vec);
                    expr_vec = bcast;
                    goto no_where_add_col;
                }
                ray_t* bcast = ray_vec_new(ct, nrows);
                if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                if (ct == RAY_STR && expr_vec->type == -RAY_STR) {
                    const char* sp = ray_str_ptr(expr_vec);
                    size_t sl = ray_str_len(expr_vec);
                    for (int64_t r = 0; r < nrows; r++) {
                        bcast = ray_str_vec_append(bcast, sp, sl);
                        if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    }
                } else {
                    size_t esz = (ct == RAY_BOOL) ? 1 : 8;
                    uint8_t elem[8] = {0};
                    if (ct == RAY_F64 && expr_vec->type == -RAY_I64) {
                        double promoted = (double)expr_vec->i64;
                        memcpy(elem, &promoted, 8);
                    } else {
                        memcpy(elem, &expr_vec->i64, esz);
                    }
                    for (int64_t r = 0; r < nrows; r++) {
                        bcast = ray_vec_append(bcast, elem);
                        if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
                    }
                }
                /* Preserve typed-null markers across broadcast (mirrors the
                 * WHERE branch fix at the analogous site above). */
                if (RAY_ATOM_IS_NULL(expr_vec)) {
                    for (int64_t r = 0; r < nrows; r++)
                        ray_vec_set_null(bcast, r, true);
                }
                ray_release(expr_vec);
                expr_vec = bcast;
            }

            /* Promote I64 vector to F64 if column is F64 */
            if (expr_vec->type == RAY_I64 && orig_col->type == RAY_F64) {
                int64_t nr = ray_len(expr_vec);
                ray_t* promoted = ray_vec_new(RAY_F64, nr);
                if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return promoted; }
                int64_t* src_data = (int64_t*)ray_data(expr_vec);
                for (int64_t r = 0; r < nr; r++) {
                    double v = (double)src_data[r];
                    promoted = ray_vec_append(promoted, &v);
                    if (RAY_IS_ERR(promoted)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return promoted; }
                }
                /* Carry the nullmap across the I64→F64 promotion. */
                for (int64_t r = 0; r < nr; r++)
                    if (ray_vec_is_null(expr_vec, r))
                        ray_vec_set_null(promoted, r, true);
                ray_release(expr_vec);
                expr_vec = promoted;
            }

            /* No-WHERE update: allow type change for same-category types.
             * Atoms (type<0) will be broadcast later, check after broadcast.
             * For vectors, check now: only numeric promotions or same type.
             * Also allow SYM/LIST interop (columns may be stored as LIST). */
            if (expr_vec->type > 0 && expr_vec->type != orig_col->type) {
                int is_ok = 0;
                /* Numeric promotions */
                if ((orig_col->type == RAY_I64 || orig_col->type == RAY_I32 || orig_col->type == RAY_F64) &&
                    (expr_vec->type == RAY_I64 || expr_vec->type == RAY_I32 || expr_vec->type == RAY_F64))
                    is_ok = 1;
                /* SYM/LIST interop */
                if ((orig_col->type == RAY_SYM || orig_col->type == RAY_LIST) &&
                    (expr_vec->type == RAY_SYM || expr_vec->type == RAY_LIST))
                    is_ok = 1;
                if (!is_ok) {
                    ray_release(expr_vec); ray_release(result); ray_release(tbl);
                    return ray_error("type", NULL);
                }
            }

no_where_add_col:
            result = ray_table_add_col(result, col_name, expr_vec);
            ray_release(expr_vec);
        }
        if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }
    }

    /* Add NEW columns from dict (columns not already in the table) */
    for (int64_t d = 0; d + 1 < dict_n; d += 2) {
        int64_t kid = dict_elems[d]->i64;
        if (kid == from_id) continue;
        /* Check if this column already exists */
        int exists = 0;
        for (int64_t c = 0; c < ncols; c++) {
            if (ray_table_col_name(tbl, c) == kid) { exists = 1; break; }
        }
        if (exists) continue;

        /* New column: evaluate expression and add */
        ray_t* update_expr = dict_elems[d + 1];
        ray_graph_t* ug = ray_graph_new(tbl);
        ray_op_t* expr_op = compile_expr_dag(ug, update_expr);
        if (!expr_op) { ray_release(result); ray_release(tbl); ray_graph_free(ug); return ray_error("domain", NULL); }
        expr_op = ray_optimize(ug, expr_op);
        ray_t* expr_vec = ray_execute(ug, expr_op);
        ray_graph_free(ug);
        if (RAY_IS_ERR(expr_vec)) { ray_release(result); ray_release(tbl); return expr_vec; }

        /* Broadcast scalar to column */
        if (expr_vec->type < 0) {
            int64_t nrows = ray_table_nrows(tbl);
            int8_t ct = -expr_vec->type;
            ray_t* bcast = ray_vec_new(ct, nrows);
            if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
            size_t esz = ray_elem_size(ct);
            uint8_t elem[8] = {0};
            memcpy(elem, &expr_vec->i64, esz > 8 ? 8 : esz);
            for (int64_t r = 0; r < nrows; r++) {
                bcast = ray_vec_append(bcast, elem);
                if (RAY_IS_ERR(bcast)) { ray_release(expr_vec); ray_release(result); ray_release(tbl); return bcast; }
            }
            /* Preserve typed-null markers across broadcast (mirrors the
             * existing-column branches above).  Without this,
             * (update {c: 0N from: t}) would silently materialise a
             * brand-new column of plain zeros. */
            if (RAY_ATOM_IS_NULL(expr_vec)) {
                for (int64_t r = 0; r < nrows; r++)
                    ray_vec_set_null(bcast, r, true);
            }
            ray_release(expr_vec);
            expr_vec = bcast;
        }

        result = ray_table_add_col(result, kid, expr_vec);
        ray_release(expr_vec);
        if (RAY_IS_ERR(result)) { ray_release(tbl); return result; }
    }

    /* Store in-place if from: 't */
    if (inplace_sym >= 0 && result && !RAY_IS_ERR(result)) {
        ray_env_set(inplace_sym, result);
    }
    ray_release(tbl);
    return result;
}

/* (insert table (list val1 val2 ...)) — append a row to a table */
ray_t* ray_insert_fn(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("domain", NULL);

    /* Special form: detect 'sym (quoted symbol for in-place insert) */
    int64_t inplace_sym = -1;
    ray_t* tbl_raw = args[0];
    ray_t* tbl;

    /* Detect calling convention: already-evaluated args (from upsert) vs raw parse tree */
    int already_eval = (tbl_raw && tbl_raw->type == RAY_TABLE);

    if (!already_eval && tbl_raw && tbl_raw->type == -RAY_SYM && !(tbl_raw->attrs & RAY_ATTR_NAME)) {
        /* Quoted symbol 'sym (no ATTR_NAME) — in-place insert */
        inplace_sym = tbl_raw->i64;
        tbl = ray_env_get(inplace_sym);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", NULL);
        ray_retain(tbl);
    } else if (already_eval) {
        tbl = tbl_raw;
        ray_retain(tbl);
    } else {
        tbl = ray_eval(tbl_raw);
        if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : ray_error("type", NULL);
    }

    /* ====================================================================
     * Vec/list dispatch — n==2 append, n==3 positional insert.
     * Tables with n==2 fall through to the legacy table-row append below.
     * ==================================================================== */
    if (tbl->type != RAY_TABLE) {
        if (already_eval) { ray_release(tbl); return ray_error("type", NULL); }
        if (tbl->attrs & RAY_ATTR_ARENA) { ray_release(tbl); return ray_error("type", NULL); }

        /* Slice → materialise so cow can mutate. Lists never slice. */
        if (tbl->attrs & RAY_ATTR_SLICE) {
            if (tbl->type == RAY_LIST) { ray_release(tbl); return ray_error("type", NULL); }
            ray_t* empty = ray_vec_new(tbl->type, 0);
            if (!empty || RAY_IS_ERR(empty)) {
                ray_release(tbl);
                return empty ? empty : ray_error("oom", NULL);
            }
            ray_t* mat = ray_vec_concat(tbl, empty);
            ray_release(empty);
            ray_release(tbl);
            if (!mat || RAY_IS_ERR(mat)) return mat ? mat : ray_error("oom", NULL);
            tbl = mat;
        }

        bool is_target_list = (tbl->type == RAY_LIST);
        bool is_target_vec  = ray_is_vec(tbl);
        if (!is_target_list && !is_target_vec) {
            ray_release(tbl);
            return ray_error("type", NULL);
        }
        if (n != 2 && n != 3) {
            ray_release(tbl);
            return ray_error("domain", NULL);
        }

        ray_t* result = NULL;
        int8_t tt = tbl->type;

        if (n == 2) {
            /* APPEND */
            ray_t* val = ray_eval(args[1]);
            if (!val || RAY_IS_ERR(val)) {
                ray_release(tbl);
                return val ? val : ray_error("type", NULL);
            }
            if (is_target_list) {
                /* Always one slot — never splice on append. */
                tbl = ray_list_append(tbl, val);
                result = tbl;
            } else if (val->type == -tt) {
                /* Atom of matching type → element append */
                int64_t new_idx = tbl->len;
                if (tt == RAY_STR) {
                    tbl = ray_str_vec_append(tbl, ray_str_ptr(val), ray_str_len(val));
                } else if (tt == RAY_SYM) {
                    int64_t s = val->i64;
                    tbl = ray_vec_append(tbl, &s);
                } else if (tt == RAY_GUID) {
                    /* GUID atom's 16-byte payload lives in val->obj; typed-null
                     * atoms have obj==NULL — write zeros and let the post-call
                     * RAY_ATOM_IS_NULL check mark the slot. */
                    static const uint8_t zero_guid[16] = {0};
                    const void* src = val->obj ? ray_data(val->obj) : zero_guid;
                    tbl = ray_vec_append(tbl, src);
                } else {
                    tbl = ray_vec_append(tbl, &val->u8);
                }
                if (tbl && !RAY_IS_ERR(tbl) && RAY_ATOM_IS_NULL(val))
                    ray_vec_set_null(tbl, new_idx, true);
                result = tbl;
            } else if (val->type == tt) {
                /* Same-type vec → splice at end */
                result = ray_vec_concat(tbl, val);
                ray_release(tbl);
            } else {
                ray_release(tbl);
                ray_release(val);
                return ray_error("type", NULL);
            }
            ray_release(val);
        } else {
            /* n == 3 — POSITIONAL */
            ray_t* idx_arg = ray_eval(args[1]);
            if (!idx_arg || RAY_IS_ERR(idx_arg)) {
                ray_release(tbl);
                return idx_arg ? idx_arg : ray_error("type", NULL);
            }
            ray_t* val = ray_eval(args[2]);
            if (!val || RAY_IS_ERR(val)) {
                ray_release(tbl);
                ray_release(idx_arg);
                return val ? val : ray_error("type", NULL);
            }

            if (is_target_list) {
                if (idx_arg->type == -RAY_I64) {
                    tbl = ray_list_insert_at(tbl, idx_arg->i64, val);
                    result = tbl;
                } else if (idx_arg->type == RAY_I64) {
                    if (val->type != RAY_LIST) {
                        ray_release(tbl); ray_release(idx_arg); ray_release(val);
                        return ray_error("type", NULL);
                    }
                    result = ray_list_insert_many(tbl, idx_arg, val);
                    ray_release(tbl);
                } else {
                    ray_release(tbl); ray_release(idx_arg); ray_release(val);
                    return ray_error("type", NULL);
                }
            } else {
                /* vec target */
                if (idx_arg->type == -RAY_I64) {
                    int64_t i = idx_arg->i64;
                    if (val->type == -tt) {
                        if (tt == RAY_STR) {
                            result = ray_str_vec_insert_at(tbl, i,
                                        ray_str_ptr(val), ray_str_len(val));
                            ray_release(tbl);
                        } else if (tt == RAY_SYM) {
                            int64_t s = val->i64;
                            tbl = ray_vec_insert_at(tbl, i, &s);
                            result = tbl;
                        } else if (tt == RAY_GUID) {
                            static const uint8_t zero_guid[16] = {0};
                            const void* src = val->obj ? ray_data(val->obj) : zero_guid;
                            tbl = ray_vec_insert_at(tbl, i, src);
                            result = tbl;
                        } else {
                            tbl = ray_vec_insert_at(tbl, i, &val->u8);
                            result = tbl;
                        }
                        if (result && !RAY_IS_ERR(result) && RAY_ATOM_IS_NULL(val))
                            ray_vec_set_null(result, i, true);
                    } else if (val->type == tt) {
                        result = ray_vec_insert_vec_at(tbl, i, val);
                        ray_release(tbl);
                    } else {
                        ray_release(tbl); ray_release(idx_arg); ray_release(val);
                        return ray_error("type", NULL);
                    }
                } else if (idx_arg->type == RAY_I64) {
                    if (tt == RAY_STR) {
                        ray_release(tbl); ray_release(idx_arg); ray_release(val);
                        return ray_error("type", NULL);
                    }
                    if (val->type != tt && val->type != -tt) {
                        ray_release(tbl); ray_release(idx_arg); ray_release(val);
                        return ray_error("type", NULL);
                    }
                    result = ray_vec_insert_many(tbl, idx_arg, val);
                    ray_release(tbl);
                } else {
                    ray_release(tbl); ray_release(idx_arg); ray_release(val);
                    return ray_error("type", NULL);
                }
            }
            ray_release(idx_arg);
            ray_release(val);
        }

        if (inplace_sym >= 0 && result && !RAY_IS_ERR(result)) {
            ray_env_set(inplace_sym, result);
            ray_retain(result);
        }
        return result;
    }

    /* Table target: arity-3 positional row insert is not implemented. */
    if (n != 2) { ray_release(tbl); return ray_error("nyi", NULL); }

    /* Evaluate the row argument (skip if already evaluated) */
    ray_t* row = already_eval ? (ray_retain(args[1]), args[1]) : ray_eval(args[1]);
    if (!row || RAY_IS_ERR(row)) { ray_release(tbl); return row ? row : ray_error("type", NULL); }
    if (tbl->type != RAY_TABLE) { ray_release(tbl); ray_release(row); return ray_error("type", NULL); }

    int64_t ncols = ray_table_ncols(tbl);
    ray_t* row_orig = row; /* keep original eval result for cleanup */

    if (!is_list(row) && row->type != RAY_TABLE && row->type != RAY_DICT) { ray_release(tbl); ray_release(row); return ray_error("type", NULL); }

    /* Table row: convert to list of column vectors */
    ray_t* tbl_row_list = NULL;
    if (row->type == RAY_TABLE) {
        int64_t src_ncols = ray_table_ncols(row);
        if (src_ncols != ncols) { ray_release(tbl); ray_release(row); return ray_error("domain", NULL); }
        tbl_row_list = ray_alloc(ncols * sizeof(ray_t*));
        if (!tbl_row_list) { ray_release(tbl); ray_release(row_orig); return ray_error("oom", NULL); }
        tbl_row_list->type = RAY_LIST;
        tbl_row_list->len = ncols;
        ray_t** trl = (ray_t**)ray_data(tbl_row_list);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            ray_t* src_col = ray_table_get_col(row, col_name);
            if (!src_col) src_col = ray_table_get_col_idx(row, c);
            if (!src_col) {
                tbl_row_list->len = 0;
                ray_free(tbl_row_list);
                ray_release(tbl); ray_release(row_orig);
                return ray_error("domain", NULL);
            }
            trl[c] = src_col;
            ray_retain(src_col);
        }
        row = tbl_row_list;
    }

    /* Dict row: extract values in table column order */
    ray_t* dict_vals = NULL;
    if (row->type == RAY_DICT) {
        ray_t* dkeys = ray_dict_keys(row);
        ray_t* dvals = ray_dict_vals(row);
        if (!dkeys || dkeys->type != RAY_SYM || !dvals) {
            ray_release(tbl); ray_release(row_orig);
            return ray_error("type", NULL);
        }
        int64_t dict_len = dkeys->len;

        dict_vals = ray_alloc(ncols * sizeof(ray_t*));
        if (!dict_vals) { ray_release(tbl); ray_release(row_orig); return ray_error("oom", NULL); }
        dict_vals->type = RAY_LIST;
        dict_vals->len = ncols;
        ray_t** dv = (ray_t**)ray_data(dict_vals);

        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            dv[c] = NULL;
            for (int64_t d = 0; d < dict_len; d++) {
                int64_t dk = ray_read_sym(ray_data(dkeys), d, RAY_SYM, dkeys->attrs);
                if (dk != col_name) continue;
                if (dvals->type == RAY_LIST) {
                    dv[c] = ((ray_t**)ray_data(dvals))[d];
                    if (dv[c]) ray_retain(dv[c]);
                } else {
                    int alloc = 0;
                    dv[c] = collection_elem(dvals, d, &alloc);
                    if (!alloc && dv[c]) ray_retain(dv[c]);
                }
                break;
            }
        }
        /* Verify all dict keys exist as table columns */
        for (int64_t d = 0; d < dict_len; d++) {
            int64_t dk = ray_read_sym(ray_data(dkeys), d, RAY_SYM, dkeys->attrs);
            int found_in_tbl = 0;
            for (int64_t c = 0; c < ncols; c++) {
                if (ray_table_col_name(tbl, c) == dk) { found_in_tbl = 1; break; }
            }
            if (!found_in_tbl) {
                for (int64_t c = 0; c < ncols; c++) if (dv[c]) ray_release(dv[c]);
                dict_vals->len = 0;
                ray_free(dict_vals);
                ray_release(tbl); ray_release(row_orig);
                return ray_error("value", NULL);
            }
        }
        row = dict_vals;
    }

    if (ray_len(row) != ncols) {
        if (dict_vals) {
            for (int64_t c = 0; c < ncols; c++) ray_release(((ray_t**)ray_data(dict_vals))[c]);
            dict_vals->len = 0;
            ray_free(dict_vals);
        }
        ray_release(tbl); ray_release(row_orig);
        return ray_error("domain", NULL);
    }

    ray_t** row_elems = (ray_t**)ray_data(row);
    int64_t nrows = ray_table_nrows(tbl);

    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) return result;

    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(tbl, c);
        ray_t* orig_col = ray_table_get_col_idx(tbl, c);
        int8_t ct = orig_col->type;

        ray_t* new_col = ray_vec_new(ct, nrows + 1);
        if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }

        /* Copy existing data */
        bool src_has_nulls = (orig_col->attrs & RAY_ATTR_HAS_NULLS) != 0;
        if (ct == RAY_STR) {
            for (int64_t r = 0; r < nrows; r++) {
                if (src_has_nulls && ray_vec_is_null(orig_col, r)) {
                    new_col = ray_str_vec_append(new_col, "", 0);
                    if (!RAY_IS_ERR(new_col))
                        ray_vec_set_null(new_col, new_col->len - 1, true);
                } else {
                    size_t slen = 0;
                    const char* sp = ray_str_vec_get(orig_col, r, &slen);
                    new_col = ray_str_vec_append(new_col, sp ? sp : "", sp ? slen : 0);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
            }
        } else if (ct == RAY_SYM) {
            for (int64_t r = 0; r < nrows; r++) {
                int64_t sym_val = ray_read_sym(ray_data(orig_col), r, orig_col->type, orig_col->attrs);
                new_col = ray_vec_append(new_col, &sym_val);
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
                if (src_has_nulls && ray_vec_is_null(orig_col, r))
                    ray_vec_set_null(new_col, new_col->len - 1, true);
            }
        } else {
            size_t elem_sz = (ct == RAY_BOOL) ? 1 : 8;
            uint8_t* src = (uint8_t*)ray_data(orig_col);
            for (int64_t r = 0; r < nrows; r++) {
                new_col = ray_vec_append(new_col, src + r * elem_sz);
                if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }
                if (src_has_nulls && ray_vec_is_null(orig_col, r))
                    ray_vec_set_null(new_col, new_col->len - 1, true);
            }
        }

        /* Append new row value(s) — atom for single row, vector for multi-row */
        if (!row_elems[c]) {
            /* NULL = null value for this column type */
            ray_t* null_atom = ray_typed_null(-ct);
            new_col = append_atom_to_col(new_col, null_atom);
            ray_release(null_atom);
        } else if (ray_is_atom(row_elems[c])) {
            new_col = append_atom_to_col(new_col, row_elems[c]);
        } else if (ray_is_vec(row_elems[c]) || row_elems[c]->type == RAY_LIST) {
            ray_t* merged = ray_concat_fn(new_col, row_elems[c]);
            ray_release(new_col);
            new_col = merged;
        } else {
            new_col = append_atom_to_col(new_col, row_elems[c]);
        }
        if (RAY_IS_ERR(new_col)) { ray_release(result); return new_col; }

        result = ray_table_add_col(result, col_name, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) return result;
    }

    /* Cleanup dict_vals, tbl_row_list, and original row */
    if (dict_vals) {
        ray_t** dv = (ray_t**)ray_data(dict_vals);
        for (int64_t c = 0; c < ncols; c++) if (dv[c]) ray_release(dv[c]);
        dict_vals->len = 0; /* prevent ray_free from double-releasing children */
        ray_free(dict_vals);
    }
    if (tbl_row_list) {
        ray_t** trl = (ray_t**)ray_data(tbl_row_list);
        for (int64_t c = 0; c < ncols; c++) if (trl[c]) ray_release(trl[c]);
        tbl_row_list->len = 0;
        ray_free(tbl_row_list);
    }
    ray_release(tbl);
    ray_release(row_orig);

    /* In-place: update the variable in the env */
    if (inplace_sym >= 0 && !RAY_IS_ERR(result)) {
        ray_env_set(inplace_sym, result);
        ray_retain(result);
        return result;
    }
    return result;
}

/* (upsert table key_col (list val1 val2 ...)) — update row if key matches, else insert.
 * Special form: first arg may be 'sym for in-place, other args are evaluated. */
ray_t* ray_upsert_fn(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("domain", NULL);

    /* Detect calling convention: already-evaluated args (from recursive call) vs raw parse tree */
    int64_t inplace_sym = -1;
    ray_t* tbl_raw = args[0];
    int already_eval = (tbl_raw && tbl_raw->type == RAY_TABLE);
    ray_t* tbl;

    if (!already_eval && tbl_raw && tbl_raw->type == -RAY_SYM && !(tbl_raw->attrs & RAY_ATTR_NAME)) {
        inplace_sym = tbl_raw->i64;
        tbl = ray_env_get(inplace_sym);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", NULL);
        ray_retain(tbl);
    } else if (already_eval) {
        tbl = tbl_raw;
        ray_retain(tbl);
    } else {
        tbl = ray_eval(tbl_raw);
        if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : ray_error("type", NULL);
    }

    ray_t* key_sym = already_eval ? (ray_retain(args[1]), args[1]) : ray_eval(args[1]);
    if (!key_sym || RAY_IS_ERR(key_sym)) { ray_release(tbl); return key_sym ? key_sym : ray_error("type", NULL); }

    ray_t* row = already_eval ? (ray_retain(args[2]), args[2]) : ray_eval(args[2]);
    if (!row || RAY_IS_ERR(row)) { ray_release(tbl); ray_release(key_sym); return row ? row : ray_error("type", NULL); }

    if (tbl->type != RAY_TABLE) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("type", NULL); }
    if (!is_list(row) && row->type != RAY_TABLE && row->type != RAY_DICT) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("type", NULL); }

    int64_t ncols = ray_table_ncols(tbl);

    /* Table row: iterate row-by-row for proper upsert semantics */
    if (row->type == RAY_TABLE) {
        int64_t src_nrows = ray_table_nrows(row);
        int64_t src_ncols = ray_table_ncols(row);

        /* Zero-row payload → upsert is a no-op regardless of payload
         * schema.  Skip all schema-strictness here: rejecting an empty
         * partial payload (e.g. for missing key columns) regresses the
         * pre-existing "empty input = do nothing" behavior.  No data
         * flows, so neither silent-drop nor null-key crashes are
         * possible below. */
        if (src_nrows == 0) {
            ray_release(key_sym); ray_release(row);
            return tbl;
        }

        /* Schema-strictness (table payload is a PARTIAL view — columns
         * in target but not in source are intentionally null-filled).
         * We only need to reject:
         *   (a) a source column whose name isn't in the target (extra
         *       → silent drop of user data);
         *   (b) a source column name that appears more than once in the
         *       source (ambiguous);
         *   (c) a source column name whose target column appears more
         *       than once in `tbl` (name-keyed gather can't tell which
         *       target slot the value belongs to → silent duplication).
         * Duplicate target columns whose names don't appear in `row`
         * are harmless — they get null-filled like any other missing
         * column. */
        for (int64_t sc = 0; sc < src_ncols; sc++) {
            int64_t scn = ray_table_col_name(row, sc);
            int64_t tbl_matches = 0, src_matches = 0;
            for (int64_t i = 0; i < ncols;     i++) if (ray_table_col_name(tbl, i) == scn) tbl_matches++;
            for (int64_t i = 0; i < src_ncols; i++) if (ray_table_col_name(row, i) == scn) src_matches++;
            if (tbl_matches != 1 || src_matches != 1) {
                ray_release(tbl); ray_release(key_sym); ray_release(row);
                return ray_error("value", NULL);
            }
        }

        /* Partial updates may null-fill ordinary columns, but the key
         * column(s) MUST be present — otherwise the recursive upsert
         * reads a NULL from row_elems[key_col] and segfaults.  Resolve
         * key names from key_sym and require each to appear in row. */
        int64_t key_names[16];
        int64_t n_key = 0;
        if (key_sym->type == -RAY_SYM) {
            key_names[n_key++] = key_sym->i64;
        } else if (key_sym->type == -RAY_I64) {
            int64_t k = key_sym->i64;
            if (k <= 0 || k > ncols || k > 16) {
                ray_release(tbl); ray_release(key_sym); ray_release(row);
                return ray_error("domain", NULL);
            }
            for (int64_t i = 0; i < k; i++)
                key_names[n_key++] = ray_table_col_name(tbl, i);
        } else {
            ray_release(tbl); ray_release(key_sym); ray_release(row);
            return ray_error("type", NULL);
        }
        for (int64_t k = 0; k < n_key; k++) {
            int found = 0;
            for (int64_t i = 0; i < src_ncols; i++)
                if (ray_table_col_name(row, i) == key_names[k]) { found = 1; break; }
            if (!found) {
                ray_release(tbl); ray_release(key_sym); ray_release(row);
                return ray_error("value", NULL);
            }
        }

        /* Gather source columns in target order (now guaranteed 1-to-1). */
        ray_t* src_cols[64];
        for (int64_t c = 0; c < ncols && c < 64; c++) {
            int64_t cn = ray_table_col_name(tbl, c);
            src_cols[c] = ray_table_get_col(row, cn);
        }
        ray_t* cur_tbl = tbl;
        ray_retain(cur_tbl);
        for (int64_t r = 0; r < src_nrows; r++) {
            ray_t* single = ray_alloc(ncols * sizeof(ray_t*));
            if (!single) { ray_release(cur_tbl); ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("oom", NULL); }
            single->type = RAY_LIST;
            single->len = ncols;
            ray_t** sr = (ray_t**)ray_data(single);
            for (int64_t c = 0; c < ncols; c++) {
                int alloc = 0;
                sr[c] = src_cols[c] ? collection_elem(src_cols[c], r, &alloc) : NULL;
                if (!alloc && sr[c]) ray_retain(sr[c]);
            }
            ray_t* upsert_args[3] = { cur_tbl, key_sym, single };
            ray_t* new_tbl = ray_upsert_fn(upsert_args, 3);
            for (int64_t c = 0; c < ncols; c++) if (sr[c]) ray_release(sr[c]);
            single->len = 0;
            ray_free(single);
            ray_release(cur_tbl);
            if (RAY_IS_ERR(new_tbl)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return new_tbl; }
            cur_tbl = new_tbl;
        }
        ray_release(tbl);
        ray_release(key_sym);
        ray_release(row);
        if (inplace_sym >= 0 && !RAY_IS_ERR(cur_tbl)) {
            ray_env_set(inplace_sym, cur_tbl);
            ray_retain(cur_tbl);
        }
        return cur_tbl;
    }

    /* Dict row: extract values in column order to create a plain list */
    ray_t* dict_row_list = NULL;
    if (row->type == RAY_DICT) {
        ray_t* dkeys = ray_dict_keys(row);
        ray_t* dvals = ray_dict_vals(row);
        if (!dkeys || dkeys->type != RAY_SYM || !dvals) {
            ray_release(tbl); ray_release(key_sym); ray_release(row);
            return ray_error("type", NULL);
        }
        int64_t n_pairs = dkeys->len;

        /* Schema-strictness: every column name must appear exactly once
         * on each side.  Mirrors the table-payload path. */
        if (n_pairs != ncols) {
            ray_release(tbl); ray_release(key_sym); ray_release(row);
            return ray_error("value", NULL);
        }
        for (int64_t c = 0; c < ncols; c++) {
            int64_t cn = ray_table_col_name(tbl, c);
            int64_t tbl_matches = 0, dict_matches = 0;
            for (int64_t i = 0; i < ncols; i++)
                if (ray_table_col_name(tbl, i) == cn) tbl_matches++;
            for (int64_t d = 0; d < n_pairs; d++) {
                int64_t dk = ray_read_sym(ray_data(dkeys), d, RAY_SYM, dkeys->attrs);
                if (dk == cn) dict_matches++;
            }
            if (tbl_matches != 1 || dict_matches != 1) {
                ray_release(tbl); ray_release(key_sym); ray_release(row);
                return ray_error("value", NULL);
            }
        }

        dict_row_list = ray_alloc(ncols * sizeof(ray_t*));
        if (!dict_row_list) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("oom", NULL); }
        dict_row_list->type = RAY_LIST;
        dict_row_list->len = ncols;
        ray_t** drl = (ray_t**)ray_data(dict_row_list);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t col_name = ray_table_col_name(tbl, c);
            drl[c] = NULL;
            for (int64_t d = 0; d < n_pairs; d++) {
                int64_t dk = ray_read_sym(ray_data(dkeys), d, RAY_SYM, dkeys->attrs);
                if (dk != col_name) continue;
                if (dvals->type == RAY_LIST) {
                    drl[c] = ((ray_t**)ray_data(dvals))[d];
                    if (drl[c]) ray_retain(drl[c]);
                } else {
                    int alloc = 0;
                    drl[c] = collection_elem(dvals, d, &alloc);
                    if (!alloc && drl[c]) ray_retain(drl[c]);
                }
                break;
            }
        }
        ray_release(row);
        row = dict_row_list;
    }

    if (ray_len(row) != ncols) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("domain", NULL); }

    ray_t** row_elems = (ray_t**)ray_data(row);
    int64_t nrows = ray_table_nrows(tbl);

    /* Determine key columns — integer N means "first N columns are keys" */
    int64_t n_key_cols = 1;
    int64_t key_col_indices[16];
    if (key_sym->type == -RAY_SYM) {
        key_col_indices[0] = -1;
        for (int64_t c = 0; c < ncols; c++) {
            if (ray_table_col_name(tbl, c) == key_sym->i64) {
                key_col_indices[0] = c;
                break;
            }
        }
        if (key_col_indices[0] < 0) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("domain", NULL); }
    } else if (key_sym->type == -RAY_I64) {
        n_key_cols = key_sym->i64;
        if (n_key_cols <= 0 || n_key_cols > ncols || n_key_cols > 16) { ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("domain", NULL); }
        for (int64_t k = 0; k < n_key_cols; k++) key_col_indices[k] = k;
    } else {
        ray_release(tbl); ray_release(key_sym); ray_release(row);
        return ray_error("type", NULL);
    }

    /* Multi-row upsert: if row values are vectors, iterate row-by-row */
    ray_t* key_elem = row_elems[key_col_indices[0]];
    if (ray_is_vec(key_elem) || key_elem->type == RAY_LIST) {
        int64_t new_nrows = ray_len(key_elem);
        ray_t* cur_tbl = tbl;
        ray_retain(cur_tbl);
        for (int64_t r = 0; r < new_nrows; r++) {
            /* Build single-row list from multi-row columns */
            ray_t* single_row = ray_alloc(ncols * sizeof(ray_t*));
            if (!single_row) { ray_release(cur_tbl); ray_release(tbl); ray_release(key_sym); ray_release(row); return ray_error("oom", NULL); }
            single_row->type = RAY_LIST;
            single_row->len = ncols;
            ray_t** sr = (ray_t**)ray_data(single_row);
            for (int64_t c = 0; c < ncols; c++) {
                int alloc = 0;
                sr[c] = collection_elem(row_elems[c], r, &alloc);
                if (!alloc && sr[c]) ray_retain(sr[c]);
            }
            /* Upsert single row into current table */
            ray_t* upsert_args[3] = { cur_tbl, key_sym, single_row };
            ray_t* new_tbl = ray_upsert_fn(upsert_args, 3);
            /* Clean up single_row */
            for (int64_t c = 0; c < ncols; c++) if (sr[c]) ray_release(sr[c]);
            single_row->len = 0;
            ray_free(single_row);
            ray_release(cur_tbl);
            if (RAY_IS_ERR(new_tbl)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return new_tbl; }
            cur_tbl = new_tbl;
        }
        ray_release(tbl);
        ray_release(key_sym);
        ray_release(row);
        if (inplace_sym >= 0 && !RAY_IS_ERR(cur_tbl)) {
            ray_env_set(inplace_sym, cur_tbl);
            ray_retain(cur_tbl);
        }
        return cur_tbl;
    }

    /* Type-check key columns before searching */
    for (int64_t k = 0; k < n_key_cols; k++) {
        int64_t kci = key_col_indices[k];
        ray_t* key_col = ray_table_get_col_idx(tbl, kci);
        ray_t* key_atom = row_elems[kci];
        int8_t kt = key_col->type;
        if (kt == RAY_STR && key_atom->type != -RAY_STR) {
            ray_release(tbl); ray_release(key_sym); ray_release(row);
            return ray_error("type", NULL);
        }
        if (kt == RAY_SYM && key_atom->type != -RAY_SYM) {
            ray_release(tbl); ray_release(key_sym); ray_release(row);
            return ray_error("type", NULL);
        }
    }

    /* Find the row to update by composite key match */
    int64_t match_row = -1;
    for (int64_t r = 0; r < nrows; r++) {
        int match = 1;
        for (int64_t k = 0; k < n_key_cols && match; k++) {
            int64_t kci = key_col_indices[k];
            ray_t* key_col = ray_table_get_col_idx(tbl, kci);
            ray_t* key_atom = row_elems[kci];
            int8_t kt = key_col->type;
            if (kt == RAY_F64) {
                double needle = (key_atom->type == -RAY_F64) ? key_atom->f64 : (double)key_atom->i64;
                if (((double*)ray_data(key_col))[r] != needle) match = 0;
            } else if (kt == RAY_SYM) {
                if (ray_read_sym(ray_data(key_col), r, key_col->type, key_col->attrs) != key_atom->i64) match = 0;
            } else if (kt == RAY_STR) {
                const char* ns = ray_str_ptr(key_atom);
                size_t nl = ray_str_len(key_atom);
                size_t rl = 0;
                const char* rs = ray_str_vec_get(key_col, r, &rl);
                if (rl != nl || (nl > 0 && (!rs || !ns || memcmp(rs, ns, nl) != 0))) match = 0;
            } else {
                int64_t needle = elem_as_i64(key_atom);
                int64_t existing = (kt == RAY_I64 || kt == RAY_TIMESTAMP) ?
                    ((int64_t*)ray_data(key_col))[r] :
                    (kt == RAY_I32 || kt == RAY_DATE || kt == RAY_TIME) ?
                    (int64_t)((int32_t*)ray_data(key_col))[r] :
                    (kt == RAY_BOOL) ? (int64_t)((uint8_t*)ray_data(key_col))[r] :
                    ((int64_t*)ray_data(key_col))[r];
                if (existing != needle) match = 0;
            }
        }
        if (match) { match_row = r; break; }
    }

    if (match_row < 0) {
        /* Key not found — insert: pass pre-evaluated args */
        ray_t* insert_args[2] = { tbl, row };
        ray_t* result = ray_insert_fn(insert_args, 2);
        ray_release(tbl);
        ray_release(key_sym);
        ray_release(row);
        if (inplace_sym >= 0 && !RAY_IS_ERR(result)) {
            ray_env_set(inplace_sym, result);
            ray_retain(result);
        }
        return result;
    }

    /* Key found — update that row */
    ray_t* result = ray_table_new(ncols);
    if (RAY_IS_ERR(result)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return result; }

    for (int64_t c = 0; c < ncols; c++) {
        int64_t col_name = ray_table_col_name(tbl, c);
        ray_t* orig_col = ray_table_get_col_idx(tbl, c);
        int8_t ct = orig_col->type;

        ray_t* new_col = ray_vec_new(ct, nrows);
        if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }

        /* If row_elems[c] is NULL (missing column), keep original values */
        int has_new_val = (row_elems[c] != NULL);

        if (ct == RAY_STR) {
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row && has_new_val) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    size_t slen = 0;
                    const char* sp = ray_str_vec_get(orig_col, r, &slen);
                    new_col = ray_str_vec_append(new_col, sp ? sp : "", sp ? slen : 0);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }
            }
        } else if (ct == RAY_SYM) {
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row && has_new_val) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    int64_t sym_val = ray_read_sym(ray_data(orig_col), r, orig_col->type, orig_col->attrs);
                    new_col = ray_vec_append(new_col, &sym_val);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }
            }
        } else {
            size_t elem_sz = (ct == RAY_BOOL) ? 1 : 8;
            uint8_t* src = (uint8_t*)ray_data(orig_col);
            for (int64_t r = 0; r < nrows; r++) {
                if (r == match_row && has_new_val) {
                    new_col = append_atom_to_col(new_col, row_elems[c]);
                } else {
                    new_col = ray_vec_append(new_col, src + r * elem_sz);
                }
                if (RAY_IS_ERR(new_col)) { ray_release(result); ray_release(tbl); ray_release(key_sym); ray_release(row); return new_col; }
            }
        }

        result = ray_table_add_col(result, col_name, new_col);
        ray_release(new_col);
        if (RAY_IS_ERR(result)) { ray_release(tbl); ray_release(key_sym); ray_release(row); return result; }
    }

    ray_release(tbl);
    ray_release(key_sym);
    ray_release(row);

    if (inplace_sym >= 0 && !RAY_IS_ERR(result)) {
        ray_env_set(inplace_sym, result);
        ray_retain(result);
    }
    return result;
}

/* ══════════════════════════════════════════
 * Join operations
 * ══════════════════════════════════════════ */

/* Shared implementation for left-join (join_type=1) and inner-join (join_type=0).
 * (left-join t1 t2 [key ...]) / (inner-join t1 t2 [key ...]) */
static ray_t* join_impl(ray_t** args, int64_t n, uint8_t join_type) {
    if (n < 3) return ray_error("domain", NULL);

    ray_t* left_tbl  = args[0];
    ray_t* right_tbl = args[1];
    ray_t* keys      = args[2];

    /* Detect alternative calling convention: (join [keys] t1 t2) */
    if (left_tbl->type != RAY_TABLE && args[1]->type == RAY_TABLE && args[2]->type == RAY_TABLE) {
        keys      = args[0];
        left_tbl  = args[1];
        right_tbl = args[2];
    }

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return ray_error("type", NULL);
    ray_t* _bxk = NULL;
    keys = unbox_vec_arg(keys, &_bxk);
    if (RAY_IS_ERR(keys)) return keys;
    if (!is_list(keys))
        { if (_bxk) ray_release(_bxk); return ray_error("type", NULL); }

    int64_t nk = ray_len(keys);
    if (nk == 0 || nk > 16) { if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    ray_t** key_elems = (ray_t**)ray_data(keys);

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) { if (_bxk) ray_release(_bxk); return ray_error("oom", NULL); }

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_op_t* lk[16], *rk[16];
    for (int64_t i = 0; i < nk; i++) {
        if (key_elems[i]->type != -RAY_SYM) {
            ray_graph_free(g); if (_bxk) ray_release(_bxk);
            return ray_error("type", NULL);
        }
        ray_t* name_str = ray_sym_str(key_elems[i]->i64);
        if (!name_str) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
        lk[i] = ray_scan(g, ray_str_ptr(name_str));
        rk[i] = ray_scan(g, ray_str_ptr(name_str));
        if (!lk[i] || !rk[i]) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    }

    if (_bxk) ray_release(_bxk);

    ray_op_t* jn = ray_join(g, left_node, lk, right_node, rk,
                           (uint8_t)nk, join_type);
    if (!jn) { ray_graph_free(g); return ray_error("oom", NULL); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}

ray_t* ray_left_join_fn(ray_t** args, int64_t n)  { return join_impl(args, n, 1); }
ray_t* ray_inner_join_fn(ray_t** args, int64_t n) { return join_impl(args, n, 0); }

/* (antijoin left right [keys])
 * Anti-semi-join: keep rows from left that have NO match in right on keys. */
static ray_t* antijoin_impl(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("domain", NULL);

    ray_t* left_tbl  = args[0];
    ray_t* right_tbl = args[1];
    ray_t* keys      = args[2];

    /* Detect alternative calling convention: (antijoin [keys] t1 t2) */
    if (left_tbl->type != RAY_TABLE && args[1]->type == RAY_TABLE && args[2]->type == RAY_TABLE) {
        keys      = args[0];
        left_tbl  = args[1];
        right_tbl = args[2];
    }

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return ray_error("type", NULL);
    ray_t* _bxk = NULL;
    keys = unbox_vec_arg(keys, &_bxk);
    if (RAY_IS_ERR(keys)) return keys;
    if (!is_list(keys))
        { if (_bxk) ray_release(_bxk); return ray_error("type", NULL); }

    int64_t nk = ray_len(keys);
    if (nk == 0 || nk > 16) { if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    ray_t** key_elems = (ray_t**)ray_data(keys);

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) { if (_bxk) ray_release(_bxk); return ray_error("oom", NULL); }

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_op_t* lk[16], *rk[16];
    for (int64_t i = 0; i < nk; i++) {
        if (key_elems[i]->type != -RAY_SYM) {
            ray_graph_free(g); if (_bxk) ray_release(_bxk);
            return ray_error("type", NULL);
        }
        ray_t* name_str = ray_sym_str(key_elems[i]->i64);
        if (!name_str) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
        lk[i] = ray_scan(g, ray_str_ptr(name_str));
        rk[i] = ray_scan(g, ray_str_ptr(name_str));
        if (!lk[i] || !rk[i]) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    }

    if (_bxk) ray_release(_bxk);

    ray_op_t* jn = ray_antijoin(g, left_node, lk, right_node, rk, (uint8_t)nk);
    if (!jn) { ray_graph_free(g); return ray_error("oom", NULL); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}

ray_t* ray_anti_join_fn(ray_t** args, int64_t n) { return antijoin_impl(args, n); }

/* ------------------------------------------------------------------------ */
/* window-join parallel worker                                              */
/* ------------------------------------------------------------------------ */

#define WJ_MAX_AGG 16

typedef struct {
    int64_t cnt;
    int64_t sum_i;
    double  sum_f;
    int64_t sum_sq_i;
    double  sum_sq_f;
    int64_t extreme_i;
    double  extreme_f;
    int64_t prod_i;
    double  prod_f;
} wj_acc_t;

typedef struct {
    int64_t  left_nrows;
    int64_t  right_nrows;
    int64_t  n_eq;
    int64_t  n_agg;

    /* Left-row metadata — pre-extracted to int64 so workers can read
     * without touching any ray_t objects (no locking, no allocation). */
    const int64_t*  lo_arr;
    const int64_t*  hi_arr;
    const int64_t*  left_eq_arr[WJ_MAX_AGG];

    /* Right-side sort order and time column (sorted rank -> original idx) */
    const int64_t*  right_sort;
    const int64_t*  rt_time_i;

    /* Right equality columns (raw), kept for binary-search compares */
    const void*     eq_data[WJ_MAX_AGG];
    int8_t          eq_type[WJ_MAX_AGG];
    uint8_t         eq_attrs[WJ_MAX_AGG];

    /* Per-agg metadata and preloaded sorted source vectors */
    uint8_t         agg_raw[WJ_MAX_AGG];
    uint16_t        agg_ops[WJ_MAX_AGG];
    int8_t          agg_result_types[WJ_MAX_AGG];
    int             agg_is_float[WJ_MAX_AGG];
    const int64_t*  sorted_i[WJ_MAX_AGG];
    const double*   sorted_f[WJ_MAX_AGG];
    const uint8_t*  sorted_nn[WJ_MAX_AGG];

    /* Per-agg result output — writers index by lr directly */
    void*           result_data[WJ_MAX_AGG];
    uint8_t*        result_null[WJ_MAX_AGG];  /* 1 byte per row: 1 = null */
} wj_scan_ctx_t;

static void wj_scan_fn(void* ctx_, uint32_t worker_id, int64_t start, int64_t end) {
    (void)worker_id;
    wj_scan_ctx_t* c = (wj_scan_ctx_t*)ctx_;
    wj_acc_t acc[WJ_MAX_AGG];
    int64_t  n_eq   = c->n_eq;
    int64_t  n_agg  = c->n_agg;
    int64_t  rn     = c->right_nrows;
    const int64_t* right_sort = c->right_sort;
    const int64_t* rt_time_i  = c->rt_time_i;

    for (int64_t lr = start; lr < end; lr++) {
        int64_t lo = c->lo_arr[lr];
        int64_t hi = c->hi_arr[lr];

        int64_t target_eq[WJ_MAX_AGG];
        for (int64_t e = 0; e < n_eq; e++)
            target_eq[e] = c->left_eq_arr[e][lr];

        /* lower_bound: first rank with (eq, time) >= (target_eq, lo) */
        int64_t lb = 0, lb_hi = rn;
        while (lb < lb_hi) {
            int64_t m = (lb + lb_hi) >> 1;
            int64_t ri = right_sort[m];
            int cmp = 0;
            for (int64_t e = 0; e < n_eq && cmp == 0; e++) {
                int64_t rv = read_col_i64(c->eq_data[e], ri, c->eq_type[e], c->eq_attrs[e]);
                if (rv < target_eq[e]) cmp = -1;
                else if (rv > target_eq[e]) cmp = 1;
            }
            if (cmp == 0 && rt_time_i[ri] < lo) cmp = -1;
            if (cmp < 0) lb = m + 1; else lb_hi = m;
        }
        int64_t ub = lb, ub_hi = rn;
        while (ub < ub_hi) {
            int64_t m = (ub + ub_hi) >> 1;
            int64_t ri = right_sort[m];
            int cmp = 0;
            for (int64_t e = 0; e < n_eq && cmp == 0; e++) {
                int64_t rv = read_col_i64(c->eq_data[e], ri, c->eq_type[e], c->eq_attrs[e]);
                if (rv < target_eq[e]) cmp = -1;
                else if (rv > target_eq[e]) cmp = 1;
            }
            if (cmp == 0 && rt_time_i[ri] <= hi) cmp = -1;
            if (cmp < 0) ub = m + 1; else ub_hi = m;
        }

        memset(acc, 0, sizeof(acc));
        for (int64_t a = 0; a < n_agg; a++) {
            if (c->agg_ops[a] == OP_PROD) { acc[a].prod_i = 1; acc[a].prod_f = 1.0; }
        }

        /* Per-agg tight scan (hoisted switch, sequential SIMD-friendly read) */
        for (int64_t a = 0; a < n_agg; a++) {
            if (c->agg_raw[a]) continue;
            wj_acc_t* A = &acc[a];
            uint16_t op = c->agg_ops[a];
            if (op == OP_COUNT) { A->cnt += (ub - lb); continue; }

            const uint8_t* nn = c->sorted_nn[a];
            if (c->agg_is_float[a]) {
                const double* ss = c->sorted_f[a];
                switch (op) {
                case OP_SUM: case OP_AVG: {
                    double sum = 0; int64_t cnt = 0;
                    if (nn) { for (int64_t k = lb; k < ub; k++) if (nn[k]) { sum += ss[k]; cnt++; } }
                    else    { for (int64_t k = lb; k < ub; k++) sum += ss[k]; cnt = ub - lb; }
                    A->sum_f = sum; A->cnt = cnt; break;
                }
                case OP_VAR: case OP_VAR_POP:
                case OP_STDDEV: case OP_STDDEV_POP: {
                    double sum = 0, sum2 = 0; int64_t cnt = 0;
                    if (nn) {
                        for (int64_t k = lb; k < ub; k++)
                            if (nn[k]) { double v = ss[k]; sum += v; sum2 += v * v; cnt++; }
                    } else {
                        for (int64_t k = lb; k < ub; k++) { double v = ss[k]; sum += v; sum2 += v * v; }
                        cnt = ub - lb;
                    }
                    A->sum_f = sum; A->sum_sq_f = sum2; A->cnt = cnt; break;
                }
                case OP_PROD: {
                    double p = 1.0; int64_t cnt = 0;
                    if (nn) { for (int64_t k = lb; k < ub; k++) if (nn[k]) { p *= ss[k]; cnt++; } }
                    else    { for (int64_t k = lb; k < ub; k++) p *= ss[k]; cnt = ub - lb; }
                    A->prod_f = p; A->cnt = cnt; break;
                }
                case OP_MIN: {
                    int64_t k = lb;
                    if (nn) {
                        double best = 0; int64_t cnt = 0;
                        for (; k < ub; k++) if (nn[k]) { best = ss[k]; cnt = 1; k++; break; }
                        for (; k < ub; k++) if (nn[k]) { double v = ss[k]; if (v < best) best = v; cnt++; }
                        A->extreme_f = best; A->cnt = cnt;
                    } else if (k < ub) {
                        double best = ss[k++];
                        for (; k < ub; k++) { double v = ss[k]; if (v < best) best = v; }
                        A->extreme_f = best; A->cnt = ub - lb;
                    }
                    break;
                }
                case OP_MAX: {
                    int64_t k = lb;
                    if (nn) {
                        double best = 0; int64_t cnt = 0;
                        for (; k < ub; k++) if (nn[k]) { best = ss[k]; cnt = 1; k++; break; }
                        for (; k < ub; k++) if (nn[k]) { double v = ss[k]; if (v > best) best = v; cnt++; }
                        A->extreme_f = best; A->cnt = cnt;
                    } else if (k < ub) {
                        double best = ss[k++];
                        for (; k < ub; k++) { double v = ss[k]; if (v > best) best = v; }
                        A->extreme_f = best; A->cnt = ub - lb;
                    }
                    break;
                }
                case OP_FIRST: {
                    if (nn) {
                        int64_t cnt = 0;
                        for (int64_t k = lb; k < ub; k++) if (nn[k]) {
                            if (cnt == 0) A->extreme_f = ss[k];
                            cnt++;
                        }
                        A->cnt = cnt;
                    } else if (lb < ub) {
                        A->extreme_f = ss[lb]; A->cnt = ub - lb;
                    }
                    break;
                }
                case OP_LAST: {
                    if (nn) {
                        int64_t cnt = 0, last_k = -1;
                        for (int64_t k = lb; k < ub; k++) if (nn[k]) { last_k = k; cnt++; }
                        if (last_k >= 0) A->extreme_f = ss[last_k];
                        A->cnt = cnt;
                    } else if (lb < ub) {
                        A->extreme_f = ss[ub - 1]; A->cnt = ub - lb;
                    }
                    break;
                }
                default: break;
                }
            } else {
                const int64_t* ss = c->sorted_i[a];
                switch (op) {
                case OP_SUM: case OP_AVG: {
                    int64_t sum = 0; int64_t cnt = 0;
                    if (nn) { for (int64_t k = lb; k < ub; k++) if (nn[k]) { sum += ss[k]; cnt++; } }
                    else    { for (int64_t k = lb; k < ub; k++) sum += ss[k]; cnt = ub - lb; }
                    A->sum_i = sum; A->cnt = cnt; break;
                }
                case OP_VAR: case OP_VAR_POP:
                case OP_STDDEV: case OP_STDDEV_POP: {
                    int64_t sum = 0, sum2 = 0; int64_t cnt = 0;
                    if (nn) {
                        for (int64_t k = lb; k < ub; k++)
                            if (nn[k]) { int64_t v = ss[k]; sum += v; sum2 += v * v; cnt++; }
                    } else {
                        for (int64_t k = lb; k < ub; k++) { int64_t v = ss[k]; sum += v; sum2 += v * v; }
                        cnt = ub - lb;
                    }
                    A->sum_i = sum; A->sum_sq_i = sum2; A->cnt = cnt; break;
                }
                case OP_PROD: {
                    int64_t p = 1; int64_t cnt = 0;
                    if (nn) { for (int64_t k = lb; k < ub; k++) if (nn[k]) { p *= ss[k]; cnt++; } }
                    else    { for (int64_t k = lb; k < ub; k++) p *= ss[k]; cnt = ub - lb; }
                    A->prod_i = p; A->cnt = cnt; break;
                }
                case OP_MIN: {
                    int64_t k = lb;
                    if (nn) {
                        int64_t best = 0, cnt = 0;
                        for (; k < ub; k++) if (nn[k]) { best = ss[k]; cnt = 1; k++; break; }
                        for (; k < ub; k++) if (nn[k]) { int64_t v = ss[k]; if (v < best) best = v; cnt++; }
                        A->extreme_i = best; A->cnt = cnt;
                    } else if (k < ub) {
                        int64_t best = ss[k++];
                        for (; k < ub; k++) { int64_t v = ss[k]; if (v < best) best = v; }
                        A->extreme_i = best; A->cnt = ub - lb;
                    }
                    break;
                }
                case OP_MAX: {
                    int64_t k = lb;
                    if (nn) {
                        int64_t best = 0, cnt = 0;
                        for (; k < ub; k++) if (nn[k]) { best = ss[k]; cnt = 1; k++; break; }
                        for (; k < ub; k++) if (nn[k]) { int64_t v = ss[k]; if (v > best) best = v; cnt++; }
                        A->extreme_i = best; A->cnt = cnt;
                    } else if (k < ub) {
                        int64_t best = ss[k++];
                        for (; k < ub; k++) { int64_t v = ss[k]; if (v > best) best = v; }
                        A->extreme_i = best; A->cnt = ub - lb;
                    }
                    break;
                }
                case OP_FIRST: {
                    if (nn) {
                        int64_t cnt = 0;
                        for (int64_t k = lb; k < ub; k++) if (nn[k]) {
                            if (cnt == 0) A->extreme_i = ss[k];
                            cnt++;
                        }
                        A->cnt = cnt;
                    } else if (lb < ub) {
                        A->extreme_i = ss[lb]; A->cnt = ub - lb;
                    }
                    break;
                }
                case OP_LAST: {
                    if (nn) {
                        int64_t cnt = 0, last_k = -1;
                        for (int64_t k = lb; k < ub; k++) if (nn[k]) { last_k = k; cnt++; }
                        if (last_k >= 0) A->extreme_i = ss[last_k];
                        A->cnt = cnt;
                    } else if (lb < ub) {
                        A->extreme_i = ss[ub - 1]; A->cnt = ub - lb;
                    }
                    break;
                }
                default: break;
                }
            }
        }

        /* Finalize → indexed write at slot lr */
        for (int64_t a = 0; a < n_agg; a++) {
            wj_acc_t* A = &acc[a];
            int8_t rty = c->agg_result_types[a];
            bool null_out = false;
            int64_t out_i = 0;
            double  out_f = 0.0;

            if (c->agg_raw[a]) {
                null_out = true;
            } else {
                switch (c->agg_ops[a]) {
                case OP_COUNT: out_i = A->cnt; break;
                case OP_SUM:
                    if (c->agg_is_float[a]) out_f = A->sum_f; else out_i = A->sum_i;
                    break;
                case OP_PROD:
                    if (A->cnt == 0) null_out = true;
                    else if (c->agg_is_float[a]) out_f = A->prod_f; else out_i = A->prod_i;
                    break;
                case OP_MIN: case OP_MAX: case OP_FIRST: case OP_LAST:
                    if (A->cnt == 0) null_out = true;
                    else if (c->agg_is_float[a]) out_f = A->extreme_f; else out_i = A->extreme_i;
                    break;
                case OP_AVG:
                    if (A->cnt == 0) null_out = true;
                    else out_f = c->agg_is_float[a]
                        ? A->sum_f / (double)A->cnt
                        : (double)A->sum_i / (double)A->cnt;
                    break;
                case OP_VAR: case OP_VAR_POP:
                case OP_STDDEV: case OP_STDDEV_POP: {
                    bool sample = (c->agg_ops[a] == OP_VAR || c->agg_ops[a] == OP_STDDEV);
                    bool insuf = sample ? (A->cnt <= 1) : (A->cnt <= 0);
                    if (insuf) { null_out = true; break; }
                    double mean, var_pop;
                    if (c->agg_is_float[a]) {
                        mean    = A->sum_f / (double)A->cnt;
                        var_pop = A->sum_sq_f / (double)A->cnt - mean * mean;
                    } else {
                        mean    = (double)A->sum_i / (double)A->cnt;
                        var_pop = (double)A->sum_sq_i / (double)A->cnt - mean * mean;
                    }
                    if (var_pop < 0) var_pop = 0;
                    if      (c->agg_ops[a] == OP_VAR_POP)    out_f = var_pop;
                    else if (c->agg_ops[a] == OP_VAR)        out_f = var_pop * A->cnt / (A->cnt - 1);
                    else if (c->agg_ops[a] == OP_STDDEV_POP) out_f = sqrt(var_pop);
                    else                                     out_f = sqrt(var_pop * A->cnt / (A->cnt - 1));
                    break;
                }
                default: null_out = true; break;
                }
            }

            c->result_null[a][lr] = null_out ? 1 : 0;
            if (null_out) continue;

            void* rd = c->result_data[a];
            if (rty == RAY_F64)       ((double*)rd)[lr] = out_f;
            else if (rty == RAY_F32)  ((float*)rd)[lr]  = (float)out_f;
            else if (rty == RAY_I64 || rty == RAY_TIMESTAMP)
                ((int64_t*)rd)[lr] = out_i;
            else if (rty == RAY_I32 || rty == RAY_DATE || rty == RAY_TIME)
                ((int32_t*)rd)[lr] = (int32_t)out_i;
            else if (rty == RAY_I16)  ((int16_t*)rd)[lr] = (int16_t)out_i;
            else                       ((uint8_t*)rd)[lr] = (uint8_t)out_i;
        }
    }
}

/* (window-join t1 t2 [eq-keys] time-col)
 * ASOF join: for each left row, find closest right row with time <= left.time
 * within the same equality partition. */
ray_t* ray_window_join_fn(ray_t** args, int64_t n) {
    if (n < 4) return ray_error("domain", NULL);

    /* Special form: evaluate first 4 args, keep agg dict (args[4]) unevaluated */
    ray_t* eargs[5];
    for (int i = 0; i < 4 && i < (int)n; i++) {
        eargs[i] = ray_eval(args[i]);
        if (!eargs[i] || RAY_IS_ERR(eargs[i])) {
            for (int j = 0; j < i; j++) ray_release(eargs[j]);
            return eargs[i] ? eargs[i] : ray_error("type", NULL);
        }
    }
    eargs[4] = (n >= 5) ? args[4] : NULL; /* agg dict stays unevaluated */

    /* Rayforce calling convention:
     * (window-join [eq+time keys] intervals left right {agg}) */
    if (n >= 5 && ray_is_vec(eargs[0]) && eargs[0]->type == RAY_SYM &&
        eargs[2]->type == RAY_TABLE && eargs[3]->type == RAY_TABLE) {
        /* Rayforce convention: implement at eval level.
         * See file-scope wj_scan_fn / wj_scan_ctx_t for the parallel worker. */
        ray_t* keys_vec = eargs[0];      /* [Sym Time] — equality + time keys */
        ray_t* intervals = eargs[1];     /* list of [lo hi] time windows */
        ray_t* left_tbl = eargs[2];      /* trades */
        ray_t* right_tbl = eargs[3];     /* quotes */
        ray_t* agg_dict = eargs[4];      /* unevaluated dict */

        int64_t nkeys = ray_len(keys_vec);
        if (nkeys < 2) return ray_error("domain", NULL);
        int64_t* key_ids = (int64_t*)ray_data(keys_vec);

        /* Last key is the time key, rest are equality keys */
        int64_t time_key = key_ids[nkeys - 1];
        int64_t n_eq = nkeys - 1;

        int64_t left_nrows = ray_table_nrows(left_tbl);
        int64_t right_nrows = ray_table_nrows(right_tbl);

        /* Get left time column */
        ray_t* left_time = ray_table_get_col(left_tbl, time_key);
        ray_t* right_time = ray_table_get_col(right_tbl, time_key);
        if (!left_time || !right_time) return ray_error("domain", NULL);

        /* Get equality columns */
        ray_t* left_eq[16], *right_eq[16];
        for (int64_t e = 0; e < n_eq && e < 16; e++) {
            left_eq[e] = ray_table_get_col(left_tbl, key_ids[e]);
            right_eq[e] = ray_table_get_col(right_tbl, key_ids[e]);
            if (!left_eq[e] || !right_eq[e]) return ray_error("domain", NULL);
        }

        /* Parse every (name, (op src)) pair from the agg dict.  The dict's
         * physical layout is [keys (SYM vec), vals (LIST)] — read keys[i]
         * via ray_read_sym and pair it with vals[i] from the LIST.
         * WJ_MAX_AGG is defined at file scope (for wj_scan_ctx_t). */
        int64_t  agg_names[WJ_MAX_AGG];
        uint16_t agg_ops[WJ_MAX_AGG];
        int64_t  agg_src_ids[WJ_MAX_AGG];
        ray_t*   agg_src_vecs[WJ_MAX_AGG] = {0};
        int8_t   agg_types[WJ_MAX_AGG];
        int      agg_is_float[WJ_MAX_AGG];
        ray_t*   agg_result_vecs[WJ_MAX_AGG] = {0};
        int      agg_raw[WJ_MAX_AGG] = {0};  /* {name: Col} bare-column form — legacy placeholder */
        int64_t  n_agg = 0;

        if (agg_dict && agg_dict->type == RAY_DICT) {
            ray_t* dkeys = ray_dict_keys(agg_dict);
            ray_t* dvals = ray_dict_vals(agg_dict);
            int64_t adn = (dkeys && dkeys->type == RAY_SYM) ? dkeys->len : 0;
            ray_t** lvals = (dvals && dvals->type == RAY_LIST) ? (ray_t**)ray_data(dvals) : NULL;
            for (int64_t di = 0; di < adn && n_agg < WJ_MAX_AGG; di++) {
                int64_t kname_id = ray_read_sym(ray_data(dkeys), di, RAY_SYM, dkeys->attrs);
                ray_t* expr = lvals ? lvals[di] : NULL;
                if (!expr) continue;
                /* (op col) aggregation form */
                if (expr->type == RAY_LIST && expr->len >= 2) {
                    ray_t** ae = (ray_t**)ray_data(expr);
                    if (!(ae[0]->type == -RAY_SYM && (ae[0]->attrs & RAY_ATTR_NAME))) continue;
                    if (!(ae[1]->type == -RAY_SYM && (ae[1]->attrs & RAY_ATTR_NAME))) continue;
                    agg_names[n_agg]   = kname_id;
                    agg_ops[n_agg]     = resolve_agg_opcode(ae[0]->i64);
                    agg_src_ids[n_agg] = ae[1]->i64;
                    agg_raw[n_agg]     = 0;
                    n_agg++;
                    continue;
                }
                /* Bare column reference — legacy map-group form, emitted as null column */
                if (expr->type == -RAY_SYM && (expr->attrs & RAY_ATTR_NAME)) {
                    agg_names[n_agg]   = kname_id;
                    agg_ops[n_agg]     = OP_MIN;
                    agg_src_ids[n_agg] = expr->i64;
                    agg_raw[n_agg]     = 1;
                    n_agg++;
                    continue;
                }
            }
        }

        /* Resolve sources, pick result types, allocate result vectors.
         * Raw bare-column form ({name: Col}) is a legacy placeholder — it
         * accepts any column type (numeric or not) and always produces a
         * nullable i64 column filled with nulls. All true aggregation ops
         * require a numeric source column. */
        int8_t agg_result_types[WJ_MAX_AGG];
        for (int64_t a = 0; a < n_agg; a++) {
            if (agg_raw[a]) {
                agg_src_vecs[a]    = NULL;
                agg_types[a]       = RAY_I64;
                agg_is_float[a]    = 0;
                agg_result_types[a] = RAY_I64;
                agg_result_vecs[a] = ray_vec_new(RAY_I64, left_nrows);
                if (RAY_IS_ERR(agg_result_vecs[a])) {
                    ray_t* err = agg_result_vecs[a];
                    for (int64_t b = 0; b < a; b++) ray_release(agg_result_vecs[b]);
                    for (int i = 0; i < 4; i++) ray_release(eargs[i]);
                    return err;
                }
                continue;
            }
            if (agg_ops[a] == 0) {
                for (int64_t b = 0; b < a; b++) ray_release(agg_result_vecs[b]);
                for (int i = 0; i < 4; i++) ray_release(eargs[i]);
                return ray_error("domain", NULL);
            }
            ray_t* src = ray_table_get_col(right_tbl, agg_src_ids[a]);
            if (!src) {
                for (int64_t b = 0; b < a; b++) ray_release(agg_result_vecs[b]);
                for (int i = 0; i < 4; i++) ray_release(eargs[i]);
                return ray_error("domain", NULL);
            }
            int8_t t = src->type;
            /* COUNT never reads source values — accept any column type. Every
             * other aggregation reads v_i/v_f and requires a numeric source. */
            if (agg_ops[a] != OP_COUNT) {
                switch (t) {
                case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
                case RAY_F64: case RAY_F32: case RAY_BOOL:
                case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
                    break;
                default:
                    for (int64_t b = 0; b < a; b++) ray_release(agg_result_vecs[b]);
                    for (int i = 0; i < 4; i++) ray_release(eargs[i]);
                    return ray_error("type", NULL);
                }
            }
            agg_src_vecs[a]  = src;
            agg_types[a]     = t;
            agg_is_float[a]  = (t == RAY_F64 || t == RAY_F32);

            int8_t rt;
            switch (agg_ops[a]) {
            case OP_COUNT: rt = RAY_I64; break;
            case OP_AVG:
            case OP_VAR: case OP_VAR_POP:
            case OP_STDDEV: case OP_STDDEV_POP: rt = RAY_F64; break;
            case OP_SUM: case OP_PROD:
                rt = agg_is_float[a] ? RAY_F64 : RAY_I64; break;
            default: /* MIN/MAX/FIRST/LAST */ rt = t; break;
            }
            agg_result_types[a] = rt;
            agg_result_vecs[a] = ray_vec_new(rt, left_nrows);
            if (RAY_IS_ERR(agg_result_vecs[a])) {
                ray_t* err = agg_result_vecs[a];
                for (int64_t b = 0; b < a; b++) ray_release(agg_result_vecs[b]);
                for (int i = 0; i < 4; i++) ray_release(eargs[i]);
                return err;
            }
        }

        /* wj_acc_t is defined at file scope now (used by wj_scan_fn). */

        /* Sort right table by (eq_keys..., time) once so each left row only
         * scans the quote rows whose (eq,time) fall inside its window.
         * Per-row cost drops from O(right_nrows) to O(log right_nrows + window). */
        ray_t*   rs_hdr = NULL, *rt_hdr = NULL, *tmp_hdr = NULL;
        int64_t* right_sort = NULL;
        int64_t* rt_time_i  = NULL;
        int64_t* tmp_sort   = NULL;
        const void* eq_data[16];
        int8_t      eq_type[16];
        uint8_t     eq_attrs[16];
        for (int64_t e = 0; e < n_eq; e++) {
            eq_data[e]  = ray_data(right_eq[e]);
            eq_type[e]  = right_eq[e]->type;
            eq_attrs[e] = right_eq[e]->attrs;
        }
        if (right_nrows > 0) {
            right_sort = (int64_t*)scratch_alloc(&rs_hdr,  (size_t)right_nrows * sizeof(int64_t));
            rt_time_i  = (int64_t*)scratch_alloc(&rt_hdr,  (size_t)right_nrows * sizeof(int64_t));
            tmp_sort   = (int64_t*)scratch_alloc(&tmp_hdr, (size_t)right_nrows * sizeof(int64_t));
            if (!right_sort || !rt_time_i || !tmp_sort) {
                if (rs_hdr)  scratch_free(rs_hdr);
                if (rt_hdr)  scratch_free(rt_hdr);
                if (tmp_hdr) scratch_free(tmp_hdr);
                for (int64_t a = 0; a < n_agg; a++) ray_release(agg_result_vecs[a]);
                for (int i = 0; i < 4; i++) ray_release(eargs[i]);
                return ray_error("oom", NULL);
            }
            /* Cache time column access so the sort compare avoids reloading them */
            int8_t  rt_type  = right_time->type;
            uint8_t rt_attrs = right_time->attrs;
            const void* rt_data = ray_data(right_time);
            for (int64_t rr = 0; rr < right_nrows; rr++) {
                right_sort[rr] = rr;
                rt_time_i[rr]  = read_col_i64(rt_data, rr, rt_type, rt_attrs);
            }
            /* Bottom-up merge sort on index array */
            for (int64_t width = 1; width < right_nrows; width *= 2) {
                for (int64_t lo = 0; lo < right_nrows; lo += 2 * width) {
                    int64_t mid = lo + width;
                    int64_t hi  = lo + 2 * width;
                    if (mid > right_nrows) mid = right_nrows;
                    if (hi  > right_nrows) hi  = right_nrows;
                    int64_t a = lo, b = mid, t = lo;
                    while (a < mid && b < hi) {
                        int64_t ai = right_sort[a], bi = right_sort[b];
                        int cmp = 0;
                        for (int64_t e = 0; e < n_eq && cmp == 0; e++) {
                            int64_t va = read_col_i64(eq_data[e], ai, eq_type[e], eq_attrs[e]);
                            int64_t vb = read_col_i64(eq_data[e], bi, eq_type[e], eq_attrs[e]);
                            if (va < vb) cmp = -1;
                            else if (va > vb) cmp = 1;
                        }
                        if (cmp == 0) {
                            if      (rt_time_i[ai] < rt_time_i[bi]) cmp = -1;
                            else if (rt_time_i[ai] > rt_time_i[bi]) cmp = 1;
                        }
                        tmp_sort[t++] = (cmp <= 0) ? right_sort[a++] : right_sort[b++];
                    }
                    while (a < mid) tmp_sort[t++] = right_sort[a++];
                    while (b < hi)  tmp_sort[t++] = right_sort[b++];
                    for (int64_t c = lo; c < hi; c++) right_sort[c] = tmp_sort[c];
                }
            }
            scratch_free(tmp_hdr);
            tmp_hdr  = NULL;
            tmp_sort = NULL;
        }

        /* Preload one sorted source column per aggregation.
         * After sorting right_sort, the hot loop wants *sequential* access
         * (SIMD + prefetch friendly) — not an indirect gather through
         * right_sort[k]. We materialize sorted_src_i[a][k] = value at
         * right_sort[k] once, then every left row's window scans are a
         * plain array walk.
         *
         * COUNT / raw form carry no source; nothing to preload. PROD and
         * ops on null-containing columns still go through the slow scan
         * (see below), so the preload is gated on the easy numeric cases. */
        int64_t* sorted_i[WJ_MAX_AGG]  = {0};
        double*  sorted_f[WJ_MAX_AGG]  = {0};
        uint8_t* sorted_nn[WJ_MAX_AGG] = {0};  /* 0 = null, 1 = value present */
        ray_t*   sorted_i_hdr[WJ_MAX_AGG] = {0};
        ray_t*   sorted_f_hdr[WJ_MAX_AGG] = {0};
        ray_t*   sorted_nn_hdr[WJ_MAX_AGG] = {0};
        for (int64_t a = 0; a < n_agg; a++) {
            if (agg_raw[a] || agg_ops[a] == OP_COUNT) continue;
            ray_t* src = agg_src_vecs[a];
            if (!src || right_nrows == 0) continue;
            bool has_nulls = (src->attrs & RAY_ATTR_HAS_NULLS) != 0;
            if (has_nulls) {
                sorted_nn[a] = (uint8_t*)scratch_alloc(&sorted_nn_hdr[a],
                                                        (size_t)right_nrows);
                if (!sorted_nn[a]) { goto wj_preload_oom; }
            }
            if (agg_is_float[a]) {
                sorted_f[a] = (double*)scratch_alloc(&sorted_f_hdr[a],
                                                      (size_t)right_nrows * sizeof(double));
                if (!sorted_f[a]) { goto wj_preload_oom; }
                int8_t t = agg_types[a];
                const void* sd = ray_data(src);
                for (int64_t k = 0; k < right_nrows; k++) {
                    int64_t rr = right_sort[k];
                    double v = (t == RAY_F32)
                        ? (double)((const float*)sd)[rr]
                        : ((const double*)sd)[rr];
                    sorted_f[a][k] = v;
                    if (has_nulls) sorted_nn[a][k] = ray_vec_is_null(src, rr) ? 0 : 1;
                }
            } else {
                sorted_i[a] = (int64_t*)scratch_alloc(&sorted_i_hdr[a],
                                                       (size_t)right_nrows * sizeof(int64_t));
                if (!sorted_i[a]) { goto wj_preload_oom; }
                const void* sd = ray_data(src);
                int8_t t = agg_types[a];
                uint8_t at = src->attrs;
                for (int64_t k = 0; k < right_nrows; k++) {
                    int64_t rr = right_sort[k];
                    sorted_i[a][k] = read_col_i64(sd, rr, t, at);
                    if (has_nulls) sorted_nn[a][k] = ray_vec_is_null(src, rr) ? 0 : 1;
                }
            }
        }

        #define WJ_CLEANUP_TEMP() do {                              \
            if (rs_hdr) scratch_free(rs_hdr);                       \
            if (rt_hdr) scratch_free(rt_hdr);                       \
            if (tmp_hdr) scratch_free(tmp_hdr);                     \
            for (int64_t _a = 0; _a < n_agg; _a++) {                \
                if (sorted_i_hdr[_a])  scratch_free(sorted_i_hdr[_a]);  \
                if (sorted_f_hdr[_a])  scratch_free(sorted_f_hdr[_a]);  \
                if (sorted_nn_hdr[_a]) scratch_free(sorted_nn_hdr[_a]); \
            }                                                       \
        } while (0)

        if (0) {
        wj_preload_oom:
            WJ_CLEANUP_TEMP();
            for (int64_t a = 0; a < n_agg; a++) ray_release(agg_result_vecs[a]);
            for (int i = 0; i < 4; i++) ray_release(eargs[i]);
            return ray_error("oom", NULL);
        }

        /* Pre-extract left-row metadata (interval endpoints + eq-key tuples)
         * into flat int64 arrays. This hoists all ray_t allocation and the
         * width-aware reads out of the hot loop so the parallel worker can
         * process rows without touching any ref-counted objects. */
        ray_t* lo_hdr = NULL, *hi_hdr = NULL;
        int64_t* lo_arr = (int64_t*)scratch_alloc(&lo_hdr, (size_t)left_nrows * sizeof(int64_t));
        int64_t* hi_arr = (int64_t*)scratch_alloc(&hi_hdr, (size_t)left_nrows * sizeof(int64_t));
        if ((!lo_arr || !hi_arr) && left_nrows > 0) {
            if (lo_hdr) scratch_free(lo_hdr);
            if (hi_hdr) scratch_free(hi_hdr);
            WJ_CLEANUP_TEMP();
            for (int64_t a = 0; a < n_agg; a++) ray_release(agg_result_vecs[a]);
            for (int i = 0; i < 4; i++) ray_release(eargs[i]);
            return ray_error("oom", NULL);
        }
        for (int64_t lr = 0; lr < left_nrows; lr++) {
            int alloc_iv = 0;
            ray_t* iv = collection_elem(intervals, lr, &alloc_iv);
            if (!iv || RAY_IS_ERR(iv) || ray_len(iv) < 2) {
                if (alloc_iv && iv) ray_release(iv);
                if (lo_hdr) scratch_free(lo_hdr);
                if (hi_hdr) scratch_free(hi_hdr);
                WJ_CLEANUP_TEMP();
                for (int64_t a = 0; a < n_agg; a++) ray_release(agg_result_vecs[a]);
                for (int i = 0; i < 4; i++) ray_release(eargs[i]);
                return ray_error("domain", NULL);
            }
            int alloc_lo = 0, alloc_hi = 0;
            ray_t* lo_atom = collection_elem(iv, 0, &alloc_lo);
            ray_t* hi_atom = collection_elem(iv, 1, &alloc_hi);
            lo_arr[lr] = as_i64(lo_atom);
            hi_arr[lr] = as_i64(hi_atom);
            if (alloc_lo) ray_release(lo_atom);
            if (alloc_hi) ray_release(hi_atom);
            if (alloc_iv) ray_release(iv);
        }

        ray_t*   left_eq_hdr[WJ_MAX_AGG] = {0};
        int64_t* left_eq_arr[WJ_MAX_AGG] = {0};
        for (int64_t e = 0; e < n_eq; e++) {
            left_eq_arr[e] = (int64_t*)scratch_alloc(&left_eq_hdr[e],
                                                     (size_t)left_nrows * sizeof(int64_t));
            if (!left_eq_arr[e] && left_nrows > 0) {
                if (lo_hdr) scratch_free(lo_hdr);
                if (hi_hdr) scratch_free(hi_hdr);
                for (int64_t f = 0; f < e; f++)
                    if (left_eq_hdr[f]) scratch_free(left_eq_hdr[f]);
                WJ_CLEANUP_TEMP();
                for (int64_t a = 0; a < n_agg; a++) ray_release(agg_result_vecs[a]);
                for (int i = 0; i < 4; i++) ray_release(eargs[i]);
                return ray_error("oom", NULL);
            }
            const void* sd = ray_data(left_eq[e]);
            int8_t  t  = left_eq[e]->type;
            uint8_t at = left_eq[e]->attrs;
            for (int64_t lr = 0; lr < left_nrows; lr++)
                left_eq_arr[e][lr] = read_col_i64(sd, lr, t, at);
        }

        /* Pre-size each result vector and allocate a 1-byte-per-row null
         * staging array — writers index by lr without touching the nullmap. */
        ray_t*   null_stage_hdr[WJ_MAX_AGG] = {0};
        uint8_t* null_stage[WJ_MAX_AGG]     = {0};
        for (int64_t a = 0; a < n_agg; a++) {
            agg_result_vecs[a]->len = left_nrows;
            null_stage[a] = (uint8_t*)scratch_alloc(&null_stage_hdr[a], (size_t)left_nrows);
            if (!null_stage[a] && left_nrows > 0) {
                if (lo_hdr) scratch_free(lo_hdr);
                if (hi_hdr) scratch_free(hi_hdr);
                for (int64_t f = 0; f < n_eq; f++) if (left_eq_hdr[f]) scratch_free(left_eq_hdr[f]);
                for (int64_t b = 0; b < a; b++) if (null_stage_hdr[b]) scratch_free(null_stage_hdr[b]);
                WJ_CLEANUP_TEMP();
                for (int64_t b = 0; b < n_agg; b++) ray_release(agg_result_vecs[b]);
                for (int i = 0; i < 4; i++) ray_release(eargs[i]);
                return ray_error("oom", NULL);
            }
            memset(null_stage[a], 0, (size_t)left_nrows);
        }

        /* Build the scan context and dispatch. */
        wj_scan_ctx_t wctx;
        memset(&wctx, 0, sizeof(wctx));
        wctx.left_nrows  = left_nrows;
        wctx.right_nrows = right_nrows;
        wctx.n_eq        = n_eq;
        wctx.n_agg       = n_agg;
        wctx.lo_arr      = lo_arr;
        wctx.hi_arr      = hi_arr;
        wctx.right_sort  = right_sort;
        wctx.rt_time_i   = rt_time_i;
        for (int64_t e = 0; e < n_eq; e++) {
            wctx.left_eq_arr[e] = left_eq_arr[e];
            wctx.eq_data[e]     = eq_data[e];
            wctx.eq_type[e]     = eq_type[e];
            wctx.eq_attrs[e]    = eq_attrs[e];
        }
        for (int64_t a = 0; a < n_agg; a++) {
            wctx.agg_raw[a]          = (uint8_t)agg_raw[a];
            wctx.agg_ops[a]          = agg_ops[a];
            wctx.agg_result_types[a] = agg_result_types[a];
            wctx.agg_is_float[a]     = agg_is_float[a];
            wctx.sorted_i[a]         = sorted_i[a];
            wctx.sorted_f[a]         = sorted_f[a];
            wctx.sorted_nn[a]        = sorted_nn[a];
            wctx.result_data[a]      = ray_data(agg_result_vecs[a]);
            wctx.result_null[a]      = null_stage[a];
        }

        ray_pool_t* pool = ray_pool_get();
        if (pool && left_nrows >= 2048) {
            ray_pool_dispatch(pool, wj_scan_fn, &wctx, left_nrows);
        } else {
            wj_scan_fn(&wctx, 0, 0, left_nrows);
        }

        /* Apply staged null flags to each result vec's null bitmap sequentially. */
        for (int64_t a = 0; a < n_agg; a++) {
            ray_t* rv = agg_result_vecs[a];
            const uint8_t* stage = null_stage[a];
            for (int64_t lr = 0; lr < left_nrows; lr++)
                if (stage[lr]) ray_vec_set_null(rv, lr, true);
        }

        /* Free pre-extract scratch */
        if (lo_hdr) scratch_free(lo_hdr);
        if (hi_hdr) scratch_free(hi_hdr);
        for (int64_t e = 0; e < n_eq; e++)
            if (left_eq_hdr[e]) scratch_free(left_eq_hdr[e]);
        for (int64_t a = 0; a < n_agg; a++)
            if (null_stage_hdr[a]) scratch_free(null_stage_hdr[a]);


        WJ_CLEANUP_TEMP();
        #undef WJ_CLEANUP_TEMP

        /* Build result table: left columns + every aggregation column */
        int64_t ncols = ray_table_ncols(left_tbl);
        ray_t* result = ray_table_new(ncols + n_agg);
        for (int64_t c = 0; c < ncols; c++) {
            int64_t cn = ray_table_col_name(left_tbl, c);
            ray_t* cv = ray_table_get_col_idx(left_tbl, c);
            ray_retain(cv);
            result = ray_table_add_col(result, cn, cv);
            ray_release(cv);
        }
        for (int64_t a = 0; a < n_agg; a++) {
            result = ray_table_add_col(result, agg_names[a], agg_result_vecs[a]);
            ray_release(agg_result_vecs[a]);
        }
        for (int i = 0; i < 4; i++) ray_release(eargs[i]);
        return result;
        #undef WJ_MAX_AGG
    }

    ray_t* left_tbl  = eargs[0];
    ray_t* right_tbl = eargs[1];
    ray_t* eq_keys   = eargs[2];
    ray_t* time_sym  = eargs[3];

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return ray_error("type", NULL);
    if (time_sym->type != -RAY_SYM)
        return ray_error("type", NULL);

    uint8_t n_eq = 0;
    ray_t** eq_elems = NULL;
    ray_t* _bxeq = NULL;
    eq_keys = unbox_vec_arg(eq_keys, &_bxeq);
    if (is_list(eq_keys)) {
        n_eq = (uint8_t)ray_len(eq_keys);
        eq_elems = (ray_t**)ray_data(eq_keys);
    }

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) return ray_error("oom", NULL);

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_t* tname = ray_sym_str(time_sym->i64);
    if (!tname) { ray_graph_free(g); return ray_error("domain", NULL); }
    ray_op_t* time_op = ray_scan(g, ray_str_ptr(tname));
    if (!time_op) { ray_graph_free(g); return ray_error("domain", NULL); }

    ray_op_t* eq_ops[16];
    for (uint8_t i = 0; i < n_eq; i++) {
        if (eq_elems[i]->type != -RAY_SYM) {
            ray_graph_free(g);
            return ray_error("type", NULL);
        }
        ray_t* nm = ray_sym_str(eq_elems[i]->i64);
        if (!nm) { ray_graph_free(g); return ray_error("domain", NULL); }
        eq_ops[i] = ray_scan(g, ray_str_ptr(nm));
        if (!eq_ops[i]) { ray_graph_free(g); return ray_error("domain", NULL); }
    }

    if (_bxeq) ray_release(_bxeq);

    ray_op_t* jn = ray_asof_join(g, left_node, right_node,
                                time_op, eq_ops, n_eq, 1);
    if (!jn) { ray_graph_free(g); return ray_error("oom", NULL); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}

/* (asof-join [key1 key2 ... timeKey] leftTable rightTable)
 * Last key is the time/asof column, rest are equality keys. */
ray_t* ray_asof_join_fn(ray_t** args, int64_t n) {
    if (n < 3) return ray_error("arity", NULL);
    ray_t* keys_vec   = args[0];
    ray_t* left_tbl   = args[1];
    ray_t* right_tbl  = args[2];

    if (left_tbl->type != RAY_TABLE || right_tbl->type != RAY_TABLE)
        return ray_error("type", NULL);

    /* Keys vector must be a SYM vector with at least 2 elements (eq + time) */
    ray_t* _bxk = NULL;
    keys_vec = unbox_vec_arg(keys_vec, &_bxk);
    if (!is_list(keys_vec) || ray_len(keys_vec) < 2) {
        if (_bxk) ray_release(_bxk);
        return ray_error("domain", NULL);
    }
    ray_t** kelems = (ray_t**)ray_data(keys_vec);
    int64_t nkeys = ray_len(keys_vec);

    /* Last key is the time column */
    ray_t* time_sym = kelems[nkeys - 1];
    if (time_sym->type != -RAY_SYM) {
        if (_bxk) ray_release(_bxk);
        return ray_error("type", NULL);
    }

    /* Remaining keys are equality keys */
    uint8_t n_eq = (uint8_t)(nkeys - 1);
    ray_t** eq_syms = kelems; /* first n_eq elements */

    ray_graph_t* g = ray_graph_new(left_tbl);
    if (!g) { if (_bxk) ray_release(_bxk); return ray_error("oom", NULL); }

    ray_op_t* left_node  = ray_const_table(g, left_tbl);
    ray_op_t* right_node = ray_const_table(g, right_tbl);

    ray_t* tname = ray_sym_str(time_sym->i64);
    if (!tname) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    ray_op_t* time_op = ray_scan(g, ray_str_ptr(tname));
    if (!time_op) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }

    ray_op_t* eq_ops[16];
    for (uint8_t i = 0; i < n_eq; i++) {
        if (eq_syms[i]->type != -RAY_SYM) {
            ray_graph_free(g); if (_bxk) ray_release(_bxk);
            return ray_error("type", NULL);
        }
        ray_t* nm = ray_sym_str(eq_syms[i]->i64);
        if (!nm) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
        eq_ops[i] = ray_scan(g, ray_str_ptr(nm));
        if (!eq_ops[i]) { ray_graph_free(g); if (_bxk) ray_release(_bxk); return ray_error("domain", NULL); }
    }

    if (_bxk) ray_release(_bxk);

    ray_op_t* jn = ray_asof_join(g, left_node, right_node,
                                time_op, eq_ops, n_eq, 1);
    if (!jn) { ray_graph_free(g); return ray_error("oom", NULL); }

    jn = ray_optimize(g, jn);
    ray_t* result = ray_execute(g, jn);
    ray_graph_free(g);
    return result;
}
