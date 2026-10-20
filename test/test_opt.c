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

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "store/csr.h"
#include <string.h>

/* Helper: create a test table with columns id1(I64), v1(I64), v3(F64) */
static ray_t* make_test_table(void) {
    (void)ray_sym_init();

    int64_t n = 10;
    int64_t id1_data[] = {1, 1, 2, 2, 3, 3, 1, 2, 3, 1};
    int64_t v1_data[]  = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    double  v3_data[]  = {1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5};

    ray_t* id1_vec = ray_vec_from_raw(RAY_I64, id1_data, n);
    ray_t* v1_vec  = ray_vec_from_raw(RAY_I64, v1_data, n);
    ray_t* v3_vec  = ray_vec_from_raw(RAY_F64, v3_data, n);

    int64_t name_id1 = ray_sym_intern("id1", 3);
    int64_t name_v1  = ray_sym_intern("v1", 2);
    int64_t name_v3  = ray_sym_intern("v3", 2);

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, name_id1, id1_vec);
    tbl = ray_table_add_col(tbl, name_v1, v1_vec);
    tbl = ray_table_add_col(tbl, name_v3, v3_vec);

    ray_release(id1_vec);
    ray_release(v1_vec);
    ray_release(v3_vec);

    return tbl;
}

/* Direct proof fixtures intentionally do not execute or optimize malformed DAGs. */
static test_result_t test_subset_proof_pointwise(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();
    int64_t labels[10];
    for (int i = 0; i < 10; i++) labels[i] = ray_sym_intern("abc", 3);
    ray_t* strings = ray_vec_from_raw(RAY_SYM, labels, 10);
    tbl = ray_table_add_col(tbl, ray_sym_intern("label", 5), strings);
    ray_release(strings);
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "v1");
    ray_op_t* c = ray_const_i64(g, 2);
    ray_op_t* add = ray_add(g, x, c);
    ray_op_t* cmp = ray_gt(g, add, c);
    ray_op_t* shared = ray_and(g, cmp, cmp);
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, shared->id));
    ray_op_t* scalar = ray_add(g, c, c);
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, scalar->id));
    ray_op_t* text = ray_const_str(g, "ab*", 3);
    ray_op_t* strcol = ray_scan(g, "label");
    ray_op_t* upper = ray_upper(g, strcol);
    ray_op_t* like = ray_like(g, upper, text);
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, like->id));
    ray_op_t* ilike = ray_ilike(g, strcol, text);
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, ilike->id));
    ray_op_t* args[] = {strcol, text};
    ray_op_t* concat = ray_concat(g, args, 2);
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, concat->id));
    ray_op_t* length = ray_strlen(g, text);
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, length->id));
    /* Same table object still has alternate-source provenance. */
    uint16_t tid = ray_graph_add_table(g, tbl);
    ray_op_t* foreign = ray_scan_table(g, tid, "v1");
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, foreign->id));
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(NULL, 0));
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, RAY_OP_NONE));
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_subset_proof_membership_roles(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "v1");
    ray_op_t* c = ray_const_i64(g, 1);
    int64_t raw[] = {1, 2, 3};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_op_t* set = ray_const_vec(g, v);
    ray_release(v);
    ray_op_t* in = ray_in(g, x, set);
    ray_op_t* notin = ray_not_in(g, x, set);
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, in->id));
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, notin->id));
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, set->id));
    ray_op_t* misuse = ray_eq(g, x, set);
    ray_op_t* both = ray_and(g, in, misuse);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, both->id));
    /* Set success in one edge/call must never certify a vector value edge. */
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, in->id));
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, misuse->id));
    ray_op_t* scalar_in = ray_in(g, c, set);
    ray_op_t* column_set = ray_in(g, x, x);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, scalar_in->id));
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, column_set->id));
    v = ray_vec_from_raw(RAY_I64, raw, 1);
    ray_op_t* onevec = ray_const_vec(g, v);
    ray_release(v);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, onevec->id));
    v = ray_vec_new(RAY_I64, 0);
    ray_op_t* empty = ray_const_vec(g, v);
    ray_release(v);
    ray_op_t* empty_in = ray_in(g, x, empty);
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, empty_in->id));
    v = ray_list_new(0);
    ray_op_t* list = ray_const_vec(g, v);
    ray_release(v);
    ray_op_t* list_in = ray_in(g, x, list);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, list_in->id));
    ray_op_t* table = ray_const_table(g, tbl);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, table->id));
    /* ray_is_atom also accepts positive object types: those are not data. */
    ray_op_ext_t* ce = find_ext(g, c->id);
    int8_t saved_type = ce->literal->type;
    ce->literal->type = RAY_UNARY;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, c->id));
    ce->literal->type = saved_type;
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, c->id));
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_subset_proof_hidden_crossrow(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* zero = ray_const_i64(g, 0); /* ID 0 is a required third child */
    TEST_ASSERT_EQ_I(zero->id, 0);
    ray_op_t* x = ray_scan(g, "v1");
    ray_op_t* safe = ray_gt(g, x, zero);
    ray_op_t* choose = ray_if(g, safe, x, zero);
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, choose->id));
    ray_op_ext_t* e = find_ext(g, choose->id);
    e->third_in = RAY_OP_NONE;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, choose->id));
    e->third_in = 0;
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, choose->id));
    ray_op_t* scalar_cond = ray_const_bool(g, true);
    ray_op_t* ambiguous = ray_if(g, scalar_cond, x, zero);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, ambiguous->id));
    ray_op_t* avg = ray_avg(g, x);
    ray_op_t* running = ray_sums_op(g, x);
    ray_op_t* badavg = ray_gt(g, x, avg);
    ray_op_t* badrun = ray_gt(g, running, zero);
    ray_op_t* nest = ray_and(g, safe, safe);
    ray_op_t* whole = ray_and(g, nest, badavg);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, whole->id));
    whole = ray_and(g, badrun, nest);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, whole->id));
    e->third_in = badrun->id;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, choose->id));
    e->third_in = badavg->id;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, choose->id));
    ray_op_t* filter = ray_filter(g, x, safe);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, filter->id));
    ray_op_t* text = ray_const_str(g, "x", 1);
    ray_op_t* args[] = {text, text, badrun};
    ray_op_t* concat = ray_concat(g, args, 3);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, concat->id));
    /* Even safe long CONCAT is refused: no recorded tail allocation size. */
    args[2] = text;
    concat = ray_concat(g, args, 3);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, concat->id));
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_subset_proof_malformed_and_cycles(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* x = ray_scan(g, "v1");
    ray_op_t* c = ray_const_i64(g, 1);
    ray_op_t* pred = ray_gt(g, x, c);
    uint32_t pid = pred->id;
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, pid));
    x->flags |= OP_FLAG_DEAD;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, pid));
    x->flags &= ~OP_FLAG_DEAD;
    x->flags |= OP_FLAG_INVALID_SCAN;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, pid));
    x->flags &= ~OP_FLAG_INVALID_SCAN;
    pred->arity = 1;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, pid));
    pred->arity = 2;
    pred->in_id[1] = g->node_count;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, pid));
    pred->in_id[1] = pid;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, pid)); /* self cycle */
    pred->in_id[1] = c->id;
    ray_op_t* neg = ray_neg(g, pred);
    pred->in_id[1] = neg->id;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, pid)); /* two-node cycle */
    pred->in_id[1] = c->id;
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, pid));
    uint32_t saved_id = x->id;
    x->id = c->id;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, pid));
    x->id = saved_id;
    ray_op_ext_t* xe = find_ext(g, x->id);
    uint16_t source = UINT16_MAX;
    memcpy(xe->base.pad, &source, sizeof(source));
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, pid));
    source = 0;
    memcpy(xe->base.pad, &source, sizeof(source));
    /* Unused input slots may be zero; only required edges are traversed. */
    x->in_id[0] = x->in_id[1] = 0;
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, pid));
    ray_t* literal = find_ext(g, c->id)->literal;
    find_ext(g, c->id)->literal = NULL;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, pid));
    find_ext(g, c->id)->literal = literal;
    uint32_t ext_count = g->ext_count;
    g->ext_count = 0;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, pid));
    g->ext_count = ext_count;
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, pid));
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_subset_proof_budgets(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);
    uint32_t root = ray_scan(g, "v1")->id;
    for (int i = 0; i < 63; i++) root = ray_neg(g, &g->nodes[root])->id;
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, root));
    root = ray_neg(g, &g->nodes[root])->id;
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, root));
    while (g->node_count < 4096) (void)ray_const_i64(g, 1);
    TEST_ASSERT_TRUE(ray_expr_subset_invariant(g, 0));
    (void)ray_const_i64(g, 1);
    TEST_ASSERT_FALSE(ray_expr_subset_invariant(g, 0));
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Deterministic subset oracle: generator choices below are NOT the trait's
 * whitelist.  Only the dedicated SUM self-control bypasses admission. */
static void subset_drop(ray_t* v) {
    if (v && RAY_IS_ERR(v)) ray_error_free(v);
    else ray_release(v);
}

static bool subset_ok(ray_t* v) { return v && !RAY_IS_ERR(v); }

/* Copy every column, without asking filtering/selection code for the oracle. */
static ray_t* subset_table(ray_t* src, const int* rows, int n) {
    static const char* names[] = {"rid", "x", "y", "b"};
    ray_t* t = ray_table_new(4);
    if (!subset_ok(t)) return t;
    for (int c = 0; c < 4; c++) {
        ray_t* v = ray_vec_new(c == 3 ? RAY_BOOL : RAY_I64, n);
        if (!subset_ok(v)) { subset_drop(t); return v; }
        v->len = n;
        v->attrs &= ~RAY_ATTR_HAS_NULLS;
        ray_t* from = src ? ray_table_col_idx(src, c) : NULL; /* borrowed */
        if (src && !subset_ok(from)) { ray_error_free(from); subset_drop(v); subset_drop(t); return NULL; }
        for (int j = 0; j < n; j++) {
            int i = rows ? rows[j] : j;
            if (c == 3) {
                ((uint8_t*)ray_data(v))[j] = src ? ((uint8_t*)ray_data(from))[i] : i % 2;
            } else {
                int64_t x = src ? ((int64_t*)ray_data(from))[i] :
                    c == 0 ? 100 + i : c == 1 ? i % 7 - 3 : (i * 3) % 7 - 3;
                if (!src && ((c == 1 && (i == 1 || i == 8)) || (c == 2 && i == 4))) x = NULL_I64;
                ((int64_t*)ray_data(v))[j] = x;
                if (x == NULL_I64) v->attrs |= RAY_ATTR_HAS_NULLS;
            }
        }
        int64_t name = ray_sym_intern(names[c], strlen(names[c]));
        if (name < 0) { subset_drop(v); subset_drop(t); return NULL; }
        t = ray_table_add_col(t, name, v); /* consumes table, retains vector */
        subset_drop(v);
        if (!subset_ok(t)) return t;
    }
    return t;
}

static bool subset_rids(ray_t* t, ray_t* u, const int* rows, int n) {
    if (ray_table_nrows(u) != n) return false;
    ray_t* a = ray_table_col_idx(t, 0);
    ray_t* b = ray_table_col_idx(u, 0);
    if (!subset_ok(a) || !subset_ok(b) || b->type != RAY_I64 || b->len != n) return false;
    for (int j = 0; j < n; j++)
        if (((int64_t*)ray_data(a))[rows[j]] != ((int64_t*)ray_data(b))[j]) return false;
    return true;
}

static bool subset_shape(ray_t* v, int n, bool scalar_recipe) {
    if (!subset_ok(v)) return false;
    if (v->type == -RAY_BOOL) return RAY_ATOM_IS_NULL(v) || v->b8 <= 1;
    /* Scalar-only binary evaluation may return a one-cell BOOL vector.
     * Broadcast it only for recipes known to have no row-valued input;
     * a literal/row-valued one-cell vector is not proof of scalar shape. */
    if (v->type != RAY_BOOL || (v->len != n && !(scalar_recipe && v->len == 1))) return false;
    for (int64_t i = 0; i < v->len; i++) if (((uint8_t*)ray_data(v))[i] > 1) return false;
    return true;
}

/* Logical cell codes: null=2, false=0, true=1. Never read null atom payload. */
static int subset_cell(ray_t* v, int i) {
    return v->type == -RAY_BOOL ? (RAY_ATOM_IS_NULL(v) ? 2 : v->b8) : ((uint8_t*)ray_data(v))[v->len == 1 ? 0 : i];
}

static bool subset_equal(ray_t* a, ray_t* b, const int* rows, int n, int* bad) {
    for (int j = 0; j < n; j++) {
        int x = subset_cell(a, rows[j]), y = subset_cell(b, j);
        if (x > 2 || y > 2 || x != y) { *bad = j; return false; }
    }
    return true;
}

typedef ray_op_t* (*subset_binary_t)(ray_graph_t*, ray_op_t*, ray_op_t*);

/* One small topological recipe, with seeded source/operator choices. IDs, not
 * node pointers, survive growth. Repeated indices preserve DAG sharing. Finite
 * inputs <=3 and arithmetic depth<=3 bound magnitude by 6561. */
static bool subset_recipe(ray_graph_t* g, uint32_t seed, int root, uint32_t* out) {
    static subset_binary_t arith[] = {ray_add, ray_sub, ray_mul};
    static subset_binary_t cmp[] = {ray_eq, ray_ne, ray_lt, ray_le, ray_gt, ray_ge};
    uint32_t ids[37] = {0};
    uint32_t rng = seed;
    for (int i = 0; i <= root; i++) {
        ray_op_t* p = NULL;
        ray_t* tmp = NULL;
#define SP_NODE(k) (&g->nodes[ids[(k)]])
        rng = rng * 1664525u + 1013904223u; /* fixed unsigned draw per node */
        switch (i) {
        case 0: p = ray_scan(g, "x"); break;
        case 1: p = ray_scan(g, "y"); break;
        case 2: p = ray_scan(g, "b"); break;
        case 3: p = ray_const_i64(g, (int64_t)(rng % 7) - 3); break;
        case 4: p = ray_const_i64(g, NULL_I64); break;
        case 5: case 6: p = ray_const_bool(g, i == 6); break;
        case 7:
            tmp = ray_typed_null(-RAY_BOOL);
            if (subset_ok(tmp)) p = ray_const_atom(g, tmp);
            break;
        case 8: {
            int64_t set[] = {-3, 0, 3, NULL_I64};
            tmp = ray_vec_from_raw(RAY_I64, set, seed % 3 == 0 ? 0 : seed % 3 == 1 ? 3 : 4);
            if (subset_ok(tmp)) {
                tmp->attrs &= ~RAY_ATTR_HAS_NULLS;
                if (tmp->len == 4) tmp->attrs |= RAY_ATTR_HAS_NULLS;
                p = ray_const_vec(g, tmp);
            }
            break;
        }
        case 9: p = arith[rng % 3](g, SP_NODE(rng % 2), SP_NODE(3)); break;
        case 10: case 11: p = arith[rng % 3](g, SP_NODE(i - 1), SP_NODE(i - 1)); break;
        case 12: case 13: case 14: case 15: case 16: case 17:
            p = cmp[i - 12](g, SP_NODE(11), SP_NODE((rng & 1) ? 3 : 4)); break;
        case 18: p = ray_and(g, SP_NODE(12), SP_NODE(13)); break;
        case 19: p = ray_or(g, SP_NODE(14), SP_NODE(15)); break;
        case 20: p = ray_not(g, SP_NODE(19)); break;
        case 21: p = ray_if(g, SP_NODE(2), SP_NODE(9), SP_NODE(1)); break;
        case 22: p = ray_gt(g, SP_NODE(21), SP_NODE(4)); break;
        case 23: p = ray_if(g, SP_NODE(18), SP_NODE(22), SP_NODE(20)); break;
        case 24: p = ray_and(g, SP_NODE(23), SP_NODE(23)); break;
        case 25: p = ray_in(g, SP_NODE(0), SP_NODE(8)); break;
        case 26: p = ray_sum(g, SP_NODE(0)); break;
        case 27: p = ray_gt(g, SP_NODE(0), SP_NODE(26)); break;
        case 28: p = ray_sums_op(g, SP_NODE(0)); break;
        case 29: p = ray_gt(g, SP_NODE(28), SP_NODE(3)); break;
        case 30: {
            uint16_t tid = ray_graph_add_table(g, g->table);
            if (tid == UINT16_MAX) return false;
            p = ray_scan_table(g, tid, "x"); break;
        }
        case 31: p = ray_gt(g, SP_NODE(30), SP_NODE(3)); break;
        case 32: p = ray_and(g, SP_NODE(18), SP_NODE(27)); break;
        case 33: p = ray_if(g, SP_NODE(2), SP_NODE(29), SP_NODE(18)); break;
        case 34: p = ray_eq(g, SP_NODE(4), SP_NODE(4)); break;
        case 35: p = ray_ne(g, SP_NODE(4), SP_NODE(3)); break;
        case 36: p = ray_lt(g, SP_NODE(4), SP_NODE(3)); break;
        }
#undef SP_NODE
        subset_drop(tmp);
        if (!p) return false;
        ids[i] = p->id;
        if (p->opcode == OP_CONST && !subset_ok(find_ext(g, p->id)->literal)) return false;
    }
    *out = ids[root];
    return true;
}

typedef struct { ray_t* t[2]; ray_t* v[2]; ray_graph_t* g; } subset_fixture_t;

static test_result_t subset_property_body(subset_fixture_t* f) {
    static const int roots[] = {2, 5, 6, 7, 12, 13, 14, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 34, 35, 36};
    static const int negatives[] = {27, 29, 31, 32, 33};
    static const int masks[5][17] = {
        {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}, {0}, {8},
        {0,2,4,6,8,10,12,14,16}, {1,3,4,8,12,15}
    };
    static const int sizes[] = {17, 0, 1, 9, 6};
    unsigned coverage = 0, executed = 0, declined = 0;
    f->t[0] = subset_table(NULL, NULL, 17);
    TEST_ASSERT_TRUE(subset_ok(f->t[0]));
    for (int c = 0; c < 6; c++) {
        uint32_t seed = 904u + (uint32_t)c;
        for (int r = 0; r < 20; r++) {
            bool admitted = false;
            for (int m = 0; m < 5; m++) {
                f->t[1] = subset_table(f->t[0], masks[m], sizes[m]);
                TEST_ASSERT_TRUE(subset_ok(f->t[1]));
                TEST_ASSERT_TRUE(subset_rids(f->t[0], f->t[1], masks[m], sizes[m]));
                for (int side = 0; side < 2; side++) {
                    f->g = ray_graph_new(f->t[side]);
                    TEST_ASSERT_NOT_NULL(f->g);
                    uint32_t id;
                    TEST_ASSERT_FMT(subset_recipe(f->g, seed, roots[r], &id),
                                    "seed=%u root=%d mask=%d side=%d recipe failed: %s", seed, roots[r], m, side,
                                    ray_error_msg() ? ray_error_msg() : "no error message");
                    bool accept = ray_expr_subset_invariant(f->g, id);
                    if (side == 0) admitted = accept;
                    else TEST_ASSERT_EQ_I(accept, admitted);
                    if (accept) {
                        f->v[side] = ray_execute(f->g, &f->g->nodes[id]);
                        TEST_ASSERT_FMT(subset_shape(f->v[side], side ? sizes[m] : 17, (roots[r] >= 5 && roots[r] <= 7) || roots[r] >= 34),
                                        "seed=%u root=%d mask=%d side=%d invalid BOOL result type=%d attrs=%u payload=%lld err=%s",
                                        seed, roots[r], m, side, f->v[side] ? f->v[side]->type : 0,
                                        f->v[side] ? (unsigned)f->v[side]->attrs : 0,
                                        subset_ok(f->v[side]) ? (long long)f->v[side]->i64 : 0,
                                        ray_error_msg() ? ray_error_msg() : "none");
                    }
                    ray_graph_free(f->g); f->g = NULL;
                }
                if (admitted) {
                    int bad = -1;
                    TEST_ASSERT_FMT(subset_equal(f->v[0], f->v[1], masks[m], sizes[m], &bad),
                                    "seed=%u root=%d mask=%d cell=%d rid=%d full=%d subset=%d", seed, roots[r], m, bad,
                                    bad < 0 ? -1 : 100 + masks[m][bad],
                                    bad < 0 ? -1 : subset_cell(f->v[0], masks[m][bad]),
                                    bad < 0 ? -1 : subset_cell(f->v[1], bad));
                    /* Pin scalar/null behavior and the engine's null ordering,
                     * independently of two executions agreeing with each other. */
                    if (r >= 1 && r <= 3)
                        TEST_ASSERT_EQ_I(subset_cell(f->v[0], 0), r == 1 ? 0 : r == 2 ? 1 : 2);
                    if (r >= 17) TEST_ASSERT_EQ_I(subset_cell(f->v[0], 0), 1);
                    coverage |= 1u << r; executed++;
                } else declined++;
                for (int side = 0; side < 2; side++) { subset_drop(f->v[side]); f->v[side] = NULL; }
                subset_drop(f->t[1]); f->t[1] = NULL;
            }
            if (r < 4) TEST_ASSERT_TRUE(admitted); /* scan + scalar/broadcast/null controls */
        }
    }
    TEST_ASSERT_FMT(executed > 0 && coverage == (1u << 20) - 1,
                    "coverage=%x executed=%u declined=%u", coverage, executed, declined);
    for (int i = 0; i < 5; i++) {
        uint32_t id;
        f->g = ray_graph_new(f->t[0]);
        TEST_ASSERT_NOT_NULL(f->g);
        TEST_ASSERT_TRUE(subset_recipe(f->g, 904, negatives[i], &id));
        TEST_ASSERT_FALSE(ray_expr_subset_invariant(f->g, id));
        ray_graph_free(f->g); f->g = NULL;
    }
    /* Independent known-nonlocal oracle: full SUM=0, subset SUM=3.
     * Deliberately bypass the trait ONLY here, on well-typed graphs. */
    subset_drop(f->t[0]); f->t[0] = subset_table(NULL, NULL, 3);
    TEST_ASSERT_TRUE(subset_ok(f->t[0]));
    ray_t* x = ray_table_col_idx(f->t[0], 1);
    int64_t values[] = {1, -3, 2};
    memcpy(ray_data(x), values, sizeof(values)); x->attrs &= ~RAY_ATTR_HAS_NULLS;
    const int pick[] = {0, 2};
    f->t[1] = subset_table(f->t[0], pick, 2);
    TEST_ASSERT_TRUE(subset_ok(f->t[1]));
    TEST_ASSERT_TRUE(subset_rids(f->t[0], f->t[1], pick, 2));
    ray_t* rid = ray_table_col_idx(f->t[1], 0);
    ((int64_t*)ray_data(rid))[0]++;
    TEST_ASSERT_FALSE(subset_rids(f->t[0], f->t[1], pick, 2));
    ((int64_t*)ray_data(rid))[0]--;
    ((int64_t*)ray_data(rid))[1]++;
    TEST_ASSERT_FALSE(subset_rids(f->t[0], f->t[1], pick, 2));
    ((int64_t*)ray_data(rid))[1]--;
    for (int side = 0; side < 2; side++) {
        uint32_t id;
        f->g = ray_graph_new(f->t[side]);
        TEST_ASSERT_NOT_NULL(f->g);
        TEST_ASSERT_TRUE(subset_recipe(f->g, 904, 27, &id));
        f->v[side] = ray_execute(f->g, &f->g->nodes[id]);
        TEST_ASSERT_TRUE(subset_shape(f->v[side], side ? 2 : 3, false));
        ray_graph_free(f->g); f->g = NULL;
    }
    for (int j = 0; j < 2; j++) {
        TEST_ASSERT_EQ_I(subset_cell(f->v[0], pick[j]), 1);
        TEST_ASSERT_EQ_I(subset_cell(f->v[1], j), 0);
    }
    int bad = -1;
    TEST_ASSERT_FALSE(subset_equal(f->v[0], f->v[1], pick, 2, &bad));
    TEST_ASSERT_EQ_I(bad, 0);
    /* Comparator sensitivity without relying on admission or engine mutation. */
    subset_drop(f->v[1]);
    uint8_t flipped[] = {1, 1};
    f->v[1] = ray_vec_from_raw(RAY_BOOL, flipped, 2);
    TEST_ASSERT_TRUE(subset_shape(f->v[1], 2, false));
    TEST_ASSERT_TRUE(subset_equal(f->v[0], f->v[1], pick, 2, &bad));
    ((uint8_t*)ray_data(f->v[1]))[1] = 0;
    TEST_ASSERT_FALSE(subset_equal(f->v[0], f->v[1], pick, 2, &bad));
    TEST_ASSERT_EQ_I(bad, 1);
    PASS();
}

static test_result_t test_subset_property(void) {
    subset_fixture_t f = {0};
    ray_heap_init();
    test_result_t result;
    if (ray_sym_init() != RAY_OK) result = (test_result_t){TEST_FAIL, "subset sym init"};
    else result = subset_property_body(&f);
    if (f.g) ray_graph_free(f.g);
    for (int i = 0; i < 2; i++) { subset_drop(f.v[i]); subset_drop(f.t[i]); }
    ray_sym_destroy();
    ray_heap_destroy();
    return result;
}

/*
 * Test: filter with AND-combined predicates in "wrong" order.
 *
 * DAG: FILTER(AND(id1_eq, v3_gt), SCAN(v1))
 *   - id1_eq is cheap (I64 eq const) but listed first
 *   - v3_gt is expensive (F64 range cmp) but listed second
 *   - Optimizer should later split AND and reorder so cheap is innermost
 *
 * Baseline correctness: id1=1 AND v3>5.0 → count=2
 */
static test_result_t test_filter_reorder_by_type(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1    = ray_scan(g, "v1");
    ray_op_t* id1   = ray_scan(g, "id1");
    ray_op_t* v3    = ray_scan(g, "v3");
    ray_op_t* c1    = ray_const_i64(g, 1);
    ray_op_t* c5    = ray_const_f64(g, 5.0);

    ray_op_t* id1_eq = ray_eq(g, id1, c1);    /* cheap: const cmp + eq */
    ray_op_t* v3_gt  = ray_gt(g, v3, c5);     /* more expensive: range */

    /* AND with "wrong" order: cheap pred first, expensive second */
    ray_op_t* combined = ray_and(g, id1_eq, v3_gt);
    ray_op_t* filt = ray_filter(g, v1, combined);
    ray_op_t* cnt = ray_count(g, filt);

    /* Execute and verify correctness: id1=1 AND v3>5.0
     * Rows: id1={1,1,2,2,3,3,1,2,3,1}, v3={1.5,2.5,...,10.5}
     * id1=1 rows: indices 0,1,6,9 → v3={1.5,2.5,7.5,10.5}
     * v3>5.0 from those: indices 6,9 → count=2 */
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: AND(pred_a, pred_b) in a single filter gets split into
 * two chained filters for independent reordering.
 *
 * DAG: FILTER(AND(v3 > 5.0, id1 = 1), SCAN(v1))
 * After: FILTER(v3 > 5.0, FILTER(id1 = 1, SCAN(v1)))
 * Verify via correctness — same result as test above.
 */
static test_result_t test_filter_and_split(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1    = ray_scan(g, "v1");
    ray_op_t* id1   = ray_scan(g, "id1");
    ray_op_t* v3    = ray_scan(g, "v3");
    ray_op_t* c1    = ray_const_i64(g, 1);
    ray_op_t* c5    = ray_const_f64(g, 5.0);

    ray_op_t* id1_eq = ray_eq(g, id1, c1);
    ray_op_t* v3_gt  = ray_gt(g, v3, c5);
    ray_op_t* combined = ray_and(g, v3_gt, id1_eq);

    ray_op_t* filt = ray_filter(g, v1, combined);
    ray_op_t* cnt = ray_count(g, filt);

    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 2);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: verify that after optimization, the inner filter (closest to scan)
 * has the cheaper predicate.
 *
 * Build: FILTER(eq_on_i64, FILTER(gt_on_f64, SCAN))
 * eq_on_i64 costs: const(+0) + i64_width(+3) + eq(+0) = 3
 * gt_on_f64 costs: const(+0) + f64_width(+3) + range(+2) = 5
 *
 * eq is cheaper, so after reorder the chain should be:
 *   FILTER(gt_on_f64, FILTER(eq_on_i64, SCAN))
 * i.e., outer predicate = gt_on_f64, inner predicate = eq_on_i64
 */
static test_result_t test_filter_reorder_dag(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1     = ray_scan(g, "v1");
    ray_op_t* id1    = ray_scan(g, "id1");
    ray_op_t* v3     = ray_scan(g, "v3");
    ray_op_t* c1     = ray_const_i64(g, 1);
    ray_op_t* c5     = ray_const_f64(g, 5.0);

    ray_op_t* eq_pred = ray_eq(g, id1, c1);     /* cost=3: const+i64+eq */
    ray_op_t* gt_pred = ray_gt(g, v3, c5);      /* cost=5: const+f64+gt */

    /* Build in WRONG order: cheap eq is outer, expensive gt is inner */
    ray_op_t* filt_inner = ray_filter(g, v1, gt_pred);
    ray_op_t* filt_outer = ray_filter(g, filt_inner, eq_pred);

    uint32_t eq_pred_id = eq_pred->id;
    uint32_t gt_pred_id = gt_pred->id;

    ray_op_t* opt = ray_optimize(g, filt_outer);
    TEST_ASSERT_NOT_NULL(opt);

    /* After reorder: outer should have gt (expensive), inner should have eq (cheap).
     * The pass swaps predicates, so:
     *   chain[0] (outer) gets the higher cost pred
     *   chain[1] (inner) gets the lower cost pred */
    TEST_ASSERT_EQ_I(opt->opcode, OP_FILTER);
    ray_op_t* inner = op_child(g, opt, 0);
    TEST_ASSERT_EQ_I(inner->opcode, OP_FILTER);

    /* Inner pred should be eq (cheaper), outer pred should be gt (more expensive) */
    TEST_ASSERT_EQ_I(inner->in_id[1], eq_pred_id);
    TEST_ASSERT_EQ_I(opt->in_id[1], gt_pred_id);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: selectivity-aware reorder. A cheap-to-evaluate but NON-selective
 * predicate (flag == 0 on a U8 column, filter_cost 0) must NOT beat a
 * super-selective wide-equality (hkey == K on an I64 column, filter_cost 3)
 * for the innermost (runs-first) slot. Under cost-only ordering the cheap
 * flag predicate wins the inner slot; selectivity ordering puts the wide
 * equality inner.
 *
 * Chain built: FILTER(hkey==K, FILTER(flag==0, SCAN(hkey)))
 * Expected after reorder: inner predicate = hkey==K, outer = flag==0.
 */
static test_result_t test_filter_reorder_selectivity(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 8;
    int64_t hkey_data[] = {10, 20, 30, 40, 50, 60, 70, 80};
    uint8_t flag_data[] = {0, 1, 0, 1, 0, 1, 0, 1};
    ray_t* hkey_vec = ray_vec_from_raw(RAY_I64, hkey_data, n);
    ray_t* flag_vec = ray_vec_from_raw(RAY_U8,  flag_data, n);
    int64_t name_hkey = ray_sym_intern("hkey", 4);
    int64_t name_flag = ray_sym_intern("flag", 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name_hkey, hkey_vec);
    tbl = ray_table_add_col(tbl, name_flag, flag_vec);
    ray_release(hkey_vec);
    ray_release(flag_vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* base    = ray_scan(g, "hkey");
    ray_op_t* hkey    = ray_scan(g, "hkey");
    ray_op_t* flag    = ray_scan(g, "flag");
    ray_op_t* hkey_eq = ray_eq(g, hkey, ray_const_i64(g, 50));  /* wide eq: selective, cost 3 */
    ray_op_t* flag_eq = ray_eq(g, flag, ray_const_i64(g, 0));   /* narrow eq: non-selective, cost 0 */

    ray_op_t* filt_inner = ray_filter(g, base, flag_eq);
    ray_op_t* filt_outer = ray_filter(g, filt_inner, hkey_eq);

    uint32_t hkey_eq_id = hkey_eq->id;
    uint32_t flag_eq_id = flag_eq->id;

    ray_op_t* opt = ray_optimize(g, filt_outer);
    TEST_ASSERT_NOT_NULL(opt);
    TEST_ASSERT_EQ_I(opt->opcode, OP_FILTER);
    ray_op_t* inner = op_child(g, opt, 0);
    TEST_ASSERT_EQ_I(inner->opcode, OP_FILTER);

    /* Selective wide-eq must be inner (runs first); flag-eq outer (runs last). */
    TEST_ASSERT_EQ_I(inner->in_id[1], hkey_eq_id);
    TEST_ASSERT_EQ_I(opt->in_id[1], flag_eq_id);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: predicate pushdown past projection.
 *
 * Build: FILTER(id1 = 1, PROJECT([id1, v1], SCAN))
 * After pushdown: PROJECT([id1, v1], FILTER(id1 = 1, SCAN))
 *
 * Verify both correctness and DAG structure.
 * id1=1 rows: indices 0,1,6,9 → count=4
 */
static test_result_t test_pushdown_past_select(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* Build: FILTER(pred, SELECT([id1, v1], SCAN)) */
    ray_op_t* v1   = ray_scan(g, "v1");
    ray_op_t* id1  = ray_scan(g, "id1");
    ray_op_t* c1   = ray_const_i64(g, 1);
    ray_op_t* pred = ray_eq(g, id1, c1);

    ray_op_t* sel_cols[] = { id1, v1 };
    ray_op_t* sel = ray_select_op(g, v1, sel_cols, 2);
    uint32_t sel_id = sel->id;
    ray_op_t* filt = ray_filter(g, sel, pred);

    /* Optimize and capture the new root (pushdown moves filter below select) */
    ray_op_t* opt_root = ray_optimize(g, filt);

    /* Verify DAG structure: filter should have been pushed below select */
    ray_op_t* sel_after = &g->nodes[sel_id];
    TEST_ASSERT_EQ_I(sel_after->opcode, OP_SELECT);
    TEST_ASSERT_EQ_I(op_child(g, sel_after, 0)->opcode, OP_FILTER);

    /* Verify the optimized root is the select node (filter was pushed below) */
    TEST_ASSERT_EQ_U(opt_root->id, sel_id);

    /* Execute COUNT from the pushed-down filter to validate correctness.
     * The filter (now below select) should still produce the right row count. */
    ray_op_t* cnt = ray_count(g, op_child(g, sel_after, 0));
    ray_t* result = ray_execute(g, cnt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 4);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: FILTER above GROUP correctness (GROUP pushdown is disabled).
 *
 * Build: FILTER(id1 = 1, GROUP(id1, SUM(v1)))
 * After: unchanged (GROUP pushdown disabled — executor key/agg scans
 *        bypass filter, so pushdown would produce wrong results).
 *
 * Verify correctness of filter-above-group execution. Result: id1=1, sum=200
 */
static test_result_t test_pushdown_past_group(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* key = ray_scan(g, "id1");
    ray_op_t* val = ray_scan(g, "v1");
    ray_op_t* keys[] = { key };
    uint16_t agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { val };
    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);

    /* Filter on the group key column (id1 = 1).
     * GROUP pushdown is disabled (executor key/agg scans bypass filter),
     * so this tests FILTER-above-GROUP correctness only. */
    ray_op_t* id1_scan = ray_scan(g, "id1");
    ray_op_t* c1 = ray_const_i64(g, 1);
    ray_op_t* pred = ray_eq(g, id1_scan, c1);
    ray_op_t* filt = ray_filter(g, grp, pred);

    /* Verify correctness */
    ray_t* result = ray_execute(g, filt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 1);
    ray_t* sum_col = ray_table_get_col_idx(result, 1);
    TEST_ASSERT_NOT_NULL(sum_col);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(sum_col))[0], 200);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: projection pushdown marks unreachable SCAN nodes dead.
 *
 * Build: SUM(SCAN("v1")) — only v1 is referenced.
 * After optimization, id1 and v3 scans are not in the DAG,
 * so they should not affect the result.
 * sum(v1) = 10+20+30+40+50+60+70+80+90+100 = 550
 */
static test_result_t test_projection_pushdown(void) {
    ray_heap_init();
    ray_t *tbl = make_test_table();
    ray_graph_t *g = ray_graph_new(tbl);

    /* Only reference v1 — id1 and v3 should not affect result */
    ray_op_t *v1 = ray_scan(g, "v1");
    ray_op_t *s = ray_sum(g, v1);
    ray_op_t *opt = ray_optimize(g, s);

    ray_t *result = ray_execute(g, opt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 550);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning smoke test.
 *
 * Build: SUM(FILTER(EQ(SCAN(id1), CONST(1)), SCAN(v1)))
 * Verify correctness: id1=1 rows: indices 0,1,6,9 → v1={10,20,70,100} → sum=200
 *
 * The partition pruning pass only activates for RAY_MAPCOMMON columns,
 * so with regular I64 columns this verifies the pass is a safe no-op.
 */
static test_result_t test_partition_pruning_smoke(void) {
    ray_heap_init();
    ray_t *tbl = make_test_table();
    ray_graph_t *g = ray_graph_new(tbl);

    ray_op_t *id1 = ray_scan(g, "id1");
    ray_op_t *v1 = ray_scan(g, "v1");
    ray_op_t *c1 = ray_const_i64(g, 1);
    ray_op_t *pred = ray_eq(g, id1, c1);
    ray_op_t *flt = ray_filter(g, v1, pred);
    ray_op_t *s = ray_sum(g, flt);

    ray_op_t *opt = ray_optimize(g, s);
    ray_t *result = ray_execute(g, opt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    /* id1=1 rows: v1={10,20,70,100} -> sum=200 */
    TEST_ASSERT_EQ_I(result->i64, 200);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning produces correct seg_mask bitmap.
 *
 * Build a parted table with 4 partitions keyed by I64 values [100, 200, 300, 400].
 * Filter: pkey >= 300.  Expected: bits 2,3 set (300,400), bits 0,1 clear (100,200).
 */
static test_result_t test_partition_pruning_mask(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build MAPCOMMON column with 4 I64 partition keys */
    ray_t* key_values = ray_vec_new(RAY_I64, 4);
    TEST_ASSERT_NOT_NULL(key_values);
    key_values->len = 4;
    int64_t keys[] = {100, 200, 300, 400};
    memcpy(ray_data(key_values), keys, sizeof(keys));

    ray_t* row_counts = ray_vec_new(RAY_I64, 4);
    TEST_ASSERT_NOT_NULL(row_counts);
    row_counts->len = 4;
    int64_t counts[] = {5, 5, 5, 5};
    memcpy(ray_data(row_counts), counts, sizeof(counts));

    /* MAPCOMMON: stores [key_values, row_counts] as pointer array */
    ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(mapcommon);
    mapcommon->type = RAY_MAPCOMMON;
    mapcommon->len = 2; /* 2 pointers */
    ((ray_t**)ray_data(mapcommon))[0] = key_values;
    ((ray_t**)ray_data(mapcommon))[1] = row_counts;

    /* Build 4 segments for data column */
    ray_t* segs[4];
    for (int i = 0; i < 4; i++) {
        segs[i] = ray_vec_new(RAY_I64, 5);
        TEST_ASSERT_NOT_NULL(segs[i]);
        segs[i]->len = 5;
        int64_t* d = (int64_t*)ray_data(segs[i]);
        for (int j = 0; j < 5; j++) d[j] = (i + 1) * 10 + j;
    }

    /* Build parted data column */
    ray_t* val_parted = ray_alloc(4 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(val_parted);
    val_parted->type = RAY_PARTED_BASE + RAY_I64;
    val_parted->len = 4;
    for (int i = 0; i < 4; i++)
        ((ray_t**)ray_data(val_parted))[i] = segs[i];

    /* Build table: pkey (MAPCOMMON), val (parted I64) */
    int64_t sym_pkey = ray_sym_intern("pkey", 4);
    int64_t sym_val  = ray_sym_intern("val", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pkey, mapcommon);
    tbl = ray_table_add_col(tbl, sym_val, val_parted);

    /* Build DAG: FILTER(SCAN(val), GE(SCAN(pkey), CONST(300))) */
    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);

    ray_op_t* scan_val  = ray_scan(g, "val");
    ray_op_t* scan_pkey = ray_scan(g, "pkey");
    ray_op_t* c300      = ray_const_i64(g, 300);
    ray_op_t* ge_pred   = ray_ge(g, scan_pkey, c300);
    ray_op_t* filt      = ray_filter(g, scan_val, ge_pred);

    /* Optimize — should produce seg_mask */
    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    /* Find the ext node for scan_val — it should have seg_mask set */
    ray_op_ext_t* val_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == scan_val->id) {
            val_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(val_ext);
    TEST_ASSERT_NOT_NULL(val_ext->seg_mask);

    /* Verify bitmap: bits 2,3 set (keys 300,400 >= 300), bits 0,1 clear */
    uint64_t expected = (1ULL << 2) | (1ULL << 3);
    TEST_ASSERT_TRUE(val_ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(mapcommon);
    ray_release(val_parted);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning works for OP_IN predicates.
 *
 * 4 partitions keyed by I64 values [100, 200, 300, 400].
 * Filter: pkey IN [100, 300].  Expected seg_mask: bits 0,2 set.
 */
static test_result_t test_partition_pruning_in(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* key_values = ray_vec_new(RAY_I64, 4);
    key_values->len = 4;
    int64_t keys[] = {100, 200, 300, 400};
    memcpy(ray_data(key_values), keys, sizeof(keys));

    ray_t* row_counts = ray_vec_new(RAY_I64, 4);
    row_counts->len = 4;
    int64_t counts[] = {5, 5, 5, 5};
    memcpy(ray_data(row_counts), counts, sizeof(counts));

    ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
    mapcommon->type = RAY_MAPCOMMON;
    mapcommon->len = 2;
    ((ray_t**)ray_data(mapcommon))[0] = key_values;
    ((ray_t**)ray_data(mapcommon))[1] = row_counts;

    ray_t* segs[4];
    for (int i = 0; i < 4; i++) {
        segs[i] = ray_vec_new(RAY_I64, 5);
        segs[i]->len = 5;
        int64_t* d = (int64_t*)ray_data(segs[i]);
        for (int j = 0; j < 5; j++) d[j] = (i + 1) * 10 + j;
    }

    ray_t* val_parted = ray_alloc(4 * sizeof(ray_t*));
    val_parted->type = RAY_PARTED_BASE + RAY_I64;
    val_parted->len = 4;
    for (int i = 0; i < 4; i++)
        ((ray_t**)ray_data(val_parted))[i] = segs[i];

    int64_t sym_pkey = ray_sym_intern("pkey", 4);
    int64_t sym_val  = ray_sym_intern("val", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pkey, mapcommon);
    tbl = ray_table_add_col(tbl, sym_val, val_parted);

    /* Build DAG: FILTER(SCAN(val), IN(SCAN(pkey), CONST(vec [100 300]))) */
    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);

    ray_op_t* scan_val  = ray_scan(g, "val");
    ray_op_t* scan_pkey = ray_scan(g, "pkey");

    /* Build literal set vector [100, 300] */
    ray_t* set_vec = ray_vec_new(RAY_I64, 2);
    set_vec->len = 2;
    ((int64_t*)ray_data(set_vec))[0] = 100;
    ((int64_t*)ray_data(set_vec))[1] = 300;
    ray_op_t* set_const = ray_const_vec(g, set_vec);
    ray_release(set_vec);  /* graph retained it */

    ray_op_t* in_pred = ray_in(g, scan_pkey, set_const);
    ray_op_t* filt    = ray_filter(g, scan_val, in_pred);

    /* Optimize — should produce seg_mask with bits 0,2 set */
    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* val_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == scan_val->id) {
            val_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(val_ext);
    TEST_ASSERT_NOT_NULL(val_ext->seg_mask);

    /* bits 0,2 set — partitions keyed 100 and 300 are in the set */
    uint64_t expected = (1ULL << 0) | (1ULL << 2);
    TEST_ASSERT_TRUE(val_ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(mapcommon);
    ray_release(val_parted);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning must NOT fire when the key type class and
 * the literal type class differ (SYM vs int-family).  Raw bit
 * comparison across namespaces would produce random spurious
 * matches — safer to skip pruning entirely and let the per-row
 * executor filter handle it.
 *
 * Setup: SYM-keyed partitions with a literal i64 set.  After
 * optimization, the scan's seg_mask must remain unset (no pruning).
 */
static test_result_t test_partition_pruning_in_type_mismatch(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Partition keys are SYM IDs (interned). */
    int64_t s_aapl = ray_sym_intern("AAPL", 4);
    int64_t s_goog = ray_sym_intern("GOOG", 4);
    int64_t s_msft = ray_sym_intern("MSFT", 4);
    int64_t s_ibm  = ray_sym_intern("IBM",  3);

    ray_t* key_values = ray_vec_new(RAY_SYM, 4);
    key_values->len = 4;
    int64_t* kd = (int64_t*)ray_data(key_values);
    kd[0] = s_aapl; kd[1] = s_goog; kd[2] = s_msft; kd[3] = s_ibm;

    ray_t* row_counts = ray_vec_new(RAY_I64, 4);
    row_counts->len = 4;
    int64_t counts[] = {5, 5, 5, 5};
    memcpy(ray_data(row_counts), counts, sizeof(counts));

    ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
    mapcommon->type = RAY_MAPCOMMON;
    mapcommon->len = 2;
    ((ray_t**)ray_data(mapcommon))[0] = key_values;
    ((ray_t**)ray_data(mapcommon))[1] = row_counts;

    ray_t* segs[4];
    for (int i = 0; i < 4; i++) {
        segs[i] = ray_vec_new(RAY_I64, 5);
        segs[i]->len = 5;
        int64_t* d = (int64_t*)ray_data(segs[i]);
        for (int j = 0; j < 5; j++) d[j] = (i + 1) * 10 + j;
    }

    ray_t* val_parted = ray_alloc(4 * sizeof(ray_t*));
    val_parted->type = RAY_PARTED_BASE + RAY_I64;
    val_parted->len = 4;
    for (int i = 0; i < 4; i++)
        ((ray_t**)ray_data(val_parted))[i] = segs[i];

    int64_t sym_pkey = ray_sym_intern("pkey", 4);
    int64_t sym_val  = ray_sym_intern("val", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pkey, mapcommon);
    tbl = ray_table_add_col(tbl, sym_val, val_parted);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_val  = ray_scan(g, "val");
    ray_op_t* scan_pkey = ray_scan(g, "pkey");

    /* TYPE-MISMATCH SET: sym-keyed partition, int literal set.
     * The sym IDs for AAPL/GOOG/MSFT/IBM could randomly equal 1, 2,
     * or 3 — and even if they don't, comparing sym IDs to raw ints
     * is nonsensical.  Pruning must skip this predicate entirely. */
    ray_t* set_vec = ray_vec_new(RAY_I64, 3);
    set_vec->len = 3;
    ((int64_t*)ray_data(set_vec))[0] = 1;
    ((int64_t*)ray_data(set_vec))[1] = 2;
    ((int64_t*)ray_data(set_vec))[2] = 3;
    ray_op_t* set_const = ray_const_vec(g, set_vec);
    ray_release(set_vec);

    ray_op_t* in_pred = ray_in(g, scan_pkey, set_const);
    ray_op_t* filt    = ray_filter(g, scan_val, in_pred);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* val_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == scan_val->id) {
            val_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(val_ext);
    /* Pruning must NOT have fired — seg_mask stays NULL. */
    TEST_ASSERT_EQ_PTR(val_ext->seg_mask, NULL);

    ray_graph_free(g);
    ray_release(mapcommon);
    ray_release(val_parted);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning for OP_NOT_IN is the complement.
 * pkey NOT IN [100, 300] → bits 1,3 set (keys 200 and 400).
 */
static test_result_t test_partition_pruning_not_in(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* key_values = ray_vec_new(RAY_I64, 4);
    key_values->len = 4;
    int64_t keys[] = {100, 200, 300, 400};
    memcpy(ray_data(key_values), keys, sizeof(keys));

    ray_t* row_counts = ray_vec_new(RAY_I64, 4);
    row_counts->len = 4;
    int64_t counts[] = {5, 5, 5, 5};
    memcpy(ray_data(row_counts), counts, sizeof(counts));

    ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
    mapcommon->type = RAY_MAPCOMMON;
    mapcommon->len = 2;
    ((ray_t**)ray_data(mapcommon))[0] = key_values;
    ((ray_t**)ray_data(mapcommon))[1] = row_counts;

    ray_t* segs[4];
    for (int i = 0; i < 4; i++) {
        segs[i] = ray_vec_new(RAY_I64, 5);
        segs[i]->len = 5;
        int64_t* d = (int64_t*)ray_data(segs[i]);
        for (int j = 0; j < 5; j++) d[j] = (i + 1) * 10 + j;
    }

    ray_t* val_parted = ray_alloc(4 * sizeof(ray_t*));
    val_parted->type = RAY_PARTED_BASE + RAY_I64;
    val_parted->len = 4;
    for (int i = 0; i < 4; i++)
        ((ray_t**)ray_data(val_parted))[i] = segs[i];

    int64_t sym_pkey = ray_sym_intern("pkey", 4);
    int64_t sym_val  = ray_sym_intern("val", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pkey, mapcommon);
    tbl = ray_table_add_col(tbl, sym_val, val_parted);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_val  = ray_scan(g, "val");
    ray_op_t* scan_pkey = ray_scan(g, "pkey");

    ray_t* set_vec = ray_vec_new(RAY_I64, 2);
    set_vec->len = 2;
    ((int64_t*)ray_data(set_vec))[0] = 100;
    ((int64_t*)ray_data(set_vec))[1] = 300;
    ray_op_t* set_const = ray_const_vec(g, set_vec);
    ray_release(set_vec);

    ray_op_t* nin_pred = ray_not_in(g, scan_pkey, set_const);
    ray_op_t* filt     = ray_filter(g, scan_val, nin_pred);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* val_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == scan_val->id) {
            val_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(val_ext);
    TEST_ASSERT_NOT_NULL(val_ext->seg_mask);

    uint64_t expected = (1ULL << 1) | (1ULL << 3);
    TEST_ASSERT_TRUE(val_ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(mapcommon);
    ray_release(val_parted);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: OP_WINDOW DCE & projection-pushdown walk.
 *
 * Build: SCAN(grp), SCAN(val) -> WINDOW(part=[grp], order=[val], func=ROW_NUMBER(val))
 * Run ray_optimize.  This forces projection_pushdown's BFS, mark_live's
 * DCE walk, and pass_type_inference to walk through ext->window.{part_keys,
 * order_keys, func_inputs}.  Verify all three columns survived (none
 * marked OP_FLAG_DEAD).
 */
static test_result_t test_opt_window_dce(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 6;
    int64_t grp_data[] = {1, 1, 1, 2, 2, 2};
    int64_t val_data[] = {10, 20, 30, 40, 50, 60};
    ray_t* grp_v = ray_vec_from_raw(RAY_I64, grp_data, n);
    ray_t* val_v = ray_vec_from_raw(RAY_I64, val_data, n);
    int64_t n_grp = ray_sym_intern("grp", 3);
    int64_t n_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n_grp, grp_v);
    tbl = ray_table_add_col(tbl, n_val, val_v);
    ray_release(grp_v);
    ray_release(val_v);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_op = ray_const_table(g, tbl);
    ray_op_t* grp_op = ray_scan(g, "grp");
    ray_op_t* val_op = ray_scan(g, "val");

    ray_op_t* parts[] = { grp_op };
    ray_op_t* orders[] = { val_op };
    uint8_t order_descs[] = { 0 };
    uint8_t func_kinds[] = { RAY_WIN_ROW_NUMBER };
    ray_op_t* func_inputs[] = { val_op };
    int64_t func_params[] = { 0 };
    ray_op_t* win = ray_window_op(g, tbl_op,
                                  parts, 1,
                                  orders, order_descs, 1,
                                  func_kinds, func_inputs, func_params, 1,
                                  RAY_FRAME_ROWS,
                                  RAY_BOUND_UNBOUNDED_PRECEDING,
                                  RAY_BOUND_UNBOUNDED_FOLLOWING,
                                  0, 0);
    TEST_ASSERT_NOT_NULL(win);

    uint32_t grp_id = grp_op->id;
    uint32_t val_id = val_op->id;

    ray_op_t* opt = ray_optimize(g, win);
    TEST_ASSERT_NOT_NULL(opt);
    TEST_ASSERT_EQ_I(opt->opcode, OP_WINDOW);

    /* Both scans referenced through window's ext (part_keys, order_keys,
     * func_inputs) must survive DCE/projection-pushdown. */
    TEST_ASSERT_FALSE(g->nodes[grp_id].flags & OP_FLAG_DEAD);
    TEST_ASSERT_FALSE(g->nodes[val_id].flags & OP_FLAG_DEAD);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: predicate pushdown past OP_EXPAND.
 *
 * Build: FILTER(EXPAND(SCAN_const_vec, rel), pred(SCAN_pred_const))
 * The predicate references a CONST scan in the source subtree, so the
 * pushdown must rewrite into:
 *      EXPAND(FILTER(SCAN_const_vec, pred), rel)
 *
 * Note: predicate must reference scans reachable from EXPAND's source
 * input.  We use a const-only predicate that has NO scans (n_scans==0,
 * which collect_pred_scans returns) -- no, we need n_scans > 0 for the
 * pushdown to fire.  So we build a graph with a small table that has
 * a column we can scan + a const compare.
 */
static test_result_t test_opt_pushdown_past_expand(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build node table with a column 'flag' we can filter on. */
    int64_t n = 4;
    int64_t flag_data[] = {1, 0, 1, 0};
    int64_t id_data[]   = {0, 1, 2, 3};
    ray_t* flag_v = ray_vec_from_raw(RAY_I64, flag_data, n);
    ray_t* id_v   = ray_vec_from_raw(RAY_I64, id_data, n);
    int64_t s_flag = ray_sym_intern("flag", 4);
    int64_t s_id   = ray_sym_intern("id", 2);
    ray_t* node_tbl = ray_table_new(2);
    node_tbl = ray_table_add_col(node_tbl, s_id, id_v);
    node_tbl = ray_table_add_col(node_tbl, s_flag, flag_v);
    ray_release(flag_v);
    ray_release(id_v);

    /* Edges: 0->1, 0->2, 1->3, 2->3 */
    int64_t src_data[] = {0, 0, 1, 2};
    int64_t dst_data[] = {1, 2, 3, 3};
    ray_t* src_v = ray_vec_from_raw(RAY_I64, src_data, 4);
    ray_t* dst_v = ray_vec_from_raw(RAY_I64, dst_data, 4);
    int64_t s_src = ray_sym_intern("src", 3);
    int64_t s_dst = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, s_src, src_v);
    edges = ray_table_add_col(edges, s_dst, dst_v);
    ray_release(src_v);
    ray_release(dst_v);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(node_tbl);

    /* Source: scan 'flag' (so the predicate's scan is reachable from
     * EXPAND's source subtree).  Predicate: flag = 1. */
    ray_op_t* flag_scan = ray_scan(g, "flag");
    ray_op_t* c1   = ray_const_i64(g, 1);
    ray_op_t* pred = ray_eq(g, flag_scan, c1);

    ray_op_t* expand = ray_expand(g, flag_scan, rel, 0);
    TEST_ASSERT_NOT_NULL(expand);
    uint32_t expand_id = expand->id;

    ray_op_t* filt = ray_filter(g, expand, pred);
    TEST_ASSERT_NOT_NULL(filt);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    /* After pushdown, root is the EXPAND (filter pushed below it). */
    TEST_ASSERT_EQ_U(opt->id, expand_id);
    TEST_ASSERT_EQ_I(opt->opcode, OP_EXPAND);
    /* EXPAND.inputs[0] should now be a FILTER node (the pushed filter). */
    TEST_ASSERT_NOT_NULL(op_child(g, opt, 0));
    TEST_ASSERT_EQ_I(op_child(g, opt, 0)->opcode, OP_FILTER);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(node_tbl);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: predicate-pushdown past EXPAND must NOT fire when the predicate
 * references a scan that is NOT in the EXPAND's source subtree.
 *
 * We can't easily build this in C without a multi-table graph, but we can
 * still exercise collect_pred_scans's truncation/unknown path by
 * building a predicate with multiple chained scans where the source is a
 * const-vec (not a scan).  In that case is_reachable_from will return
 * false for the predicate's scan, all_source becomes false, and pushdown
 * is skipped.  This still walks collect_pred_scans + is_reachable_from.
 */
static test_result_t test_opt_pushdown_expand_blocked(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 4;
    int64_t flag_data[] = {1, 0, 1, 0};
    int64_t id_data[]   = {0, 1, 2, 3};
    ray_t* flag_v = ray_vec_from_raw(RAY_I64, flag_data, n);
    ray_t* id_v   = ray_vec_from_raw(RAY_I64, id_data, n);
    int64_t s_flag = ray_sym_intern("flag", 4);
    int64_t s_id   = ray_sym_intern("id", 2);
    ray_t* node_tbl = ray_table_new(2);
    node_tbl = ray_table_add_col(node_tbl, s_id, id_v);
    node_tbl = ray_table_add_col(node_tbl, s_flag, flag_v);
    ray_release(flag_v);
    ray_release(id_v);

    int64_t src_data[] = {0, 1, 2};
    int64_t dst_data[] = {1, 2, 3};
    ray_t* src_v = ray_vec_from_raw(RAY_I64, src_data, 3);
    ray_t* dst_v = ray_vec_from_raw(RAY_I64, dst_data, 3);
    int64_t s_src = ray_sym_intern("src", 3);
    int64_t s_dst = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, s_src, src_v);
    edges = ray_table_add_col(edges, s_dst, dst_v);
    ray_release(src_v);
    ray_release(dst_v);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(node_tbl);

    /* EXPAND's source is a const vec literal — has no SCAN.
     * Predicate references a SCAN, which is NOT in source subtree.
     * Pushdown must skip; we just verify it doesn't crash and
     * the filter remains above the expand. */
    int64_t start_data[] = {0, 1};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 2);
    ray_op_t* src = ray_const_vec(g, start_vec);
    ray_release(start_vec);

    ray_op_t* expand = ray_expand(g, src, rel, 0);
    TEST_ASSERT_NOT_NULL(expand);
    uint32_t expand_id = expand->id;

    /* Predicate: flag = 1 — its SCAN is NOT in the expand source subtree. */
    ray_op_t* flag_scan = ray_scan(g, "flag");
    ray_op_t* c1 = ray_const_i64(g, 1);
    ray_op_t* pred = ray_eq(g, flag_scan, c1);

    ray_op_t* filt = ray_filter(g, expand, pred);
    uint32_t filt_id = filt->id;

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);
    /* Pushdown blocked: filter remains the root, expand stays below. */
    TEST_ASSERT_EQ_U(opt->id, filt_id);
    TEST_ASSERT_EQ_I(opt->opcode, OP_FILTER);
    TEST_ASSERT_EQ_U(opt->in_id[0], expand_id);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(node_tbl);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: graph_alloc_node_opt realloc fix-up.
 *
 * Build a graph with > GRAPH_INIT_CAP (4096) nodes, then add one
 * filter with an AND predicate.  When the optimizer's split_and_filter
 * pass calls graph_alloc_node_opt to allocate the new outer-filter
 * node, the array is at capacity and must realloc + fix up all stored
 * input pointers.
 *
 * We pad the graph by allocating many ray_const_i64 nodes (cheap, each
 * adds one node) until we're just shy of cap, then build the AND filter
 * on top.  The split must produce the correct structure after realloc.
 *
 * We also include OP_GROUP / OP_SELECT / OP_SORT / OP_WINDOW nodes
 * (with structural ext->keys/columns/part_keys pointers) to exercise
 * the per-opcode fix-up branches in graph_alloc_node_opt.  Those nodes
 * are not in the optimized root's live subgraph, so DCE will mark
 * them dead — but the realloc fix-up still walks them.
 */
static test_result_t test_opt_realloc_during_split(void) {
    ray_heap_init();

    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* Build the predicate FIRST, before we cause a realloc — we want
     * stored input pointers across the AND/EQ predicate so the fix-up
     * has something to relocate. */
    ray_op_t* v1   = ray_scan(g, "v1");
    ray_op_t* id1  = ray_scan(g, "id1");
    ray_op_t* v3   = ray_scan(g, "v3");
    ray_op_t* c1   = ray_const_i64(g, 1);
    ray_op_t* c5   = ray_const_f64(g, 5.0);
    ray_op_t* eq   = ray_eq(g, id1, c1);
    ray_op_t* gt   = ray_gt(g, v3, c5);
    ray_op_t* combined = ray_and(g, eq, gt);
    ray_op_t* filt = ray_filter(g, v1, combined);
    uint32_t filt_id = filt->id;
    uint32_t eq_id   = eq->id;
    uint32_t gt_id   = gt->id;

    /* Build extra structural ops to exercise the per-opcode fix-up
     * branches in graph_alloc_node_opt.  These are NOT in the live
     * subgraph reachable from filt — DCE will mark them dead — but
     * they exist in g->ext_nodes when realloc fires. */
    {
        /* OP_GROUP with key + agg input */
        ray_op_t* key = ray_scan(g, "id1");
        ray_op_t* val = ray_scan(g, "v1");
        ray_op_t* keys[] = { key };
        uint16_t agg_ops[] = { OP_SUM };
        ray_op_t* agg_ins[] = { val };
        (void)ray_group(g, keys, 1, agg_ops, agg_ins, 1);

        /* OP_SELECT with column array */
        ray_op_t* sel_v1 = ray_scan(g, "v1");
        ray_op_t* sel_id1 = ray_scan(g, "id1");
        ray_op_t* sel_cols[] = { sel_v1, sel_id1 };
        (void)ray_select_op(g, sel_v1, sel_cols, 2);

        /* OP_WINDOW with part/order/func keys */
        ray_op_t* tbl_op = ray_const_table(g, tbl);
        ray_op_t* w_grp = ray_scan(g, "id1");
        ray_op_t* w_val = ray_scan(g, "v1");
        ray_op_t* w_parts[] = { w_grp };
        ray_op_t* w_orders[] = { w_val };
        uint8_t w_descs[] = { 0 };
        uint8_t w_kinds[] = { RAY_WIN_ROW_NUMBER };
        ray_op_t* w_ins[] = { w_val };
        int64_t w_params[] = { 0 };
        (void)ray_window_op(g, tbl_op,
                            w_parts, 1,
                            w_orders, w_descs, 1,
                            w_kinds, w_ins, w_params, 1,
                            RAY_FRAME_ROWS,
                            RAY_BOUND_UNBOUNDED_PRECEDING,
                            RAY_BOUND_UNBOUNDED_FOLLOWING,
                            0, 0);
    }

    /* Now pad the graph until we're at exactly node_cap, so the very
     * next allocation (split_and_filter's outer node) forces a realloc. */
    while (g->node_count < g->node_cap) {
        (void)ray_const_i64(g, 0);
    }
    /* node_count == node_cap now — next allocation will realloc */
    TEST_ASSERT_EQ_U(g->node_count, g->node_cap);

    uint32_t cap_before = g->node_cap;

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    /* After AND-split, the original FILTER node (filt_id) should have
     * one of the predicates (eq or gt), and a NEW outer FILTER (id ==
     * old node_count == cap_before) wraps it with the other predicate. */
    TEST_ASSERT_TRUE(g->node_cap > cap_before);  /* realloc fired */

    /* Walk from new root: must be FILTER -> FILTER -> SCAN(v1) chain,
     * with predicates pointing at eq and gt (post-fix-up). */
    TEST_ASSERT_EQ_I(opt->opcode, OP_FILTER);
    TEST_ASSERT_NOT_NULL(op_child(g, opt, 0));
    TEST_ASSERT_EQ_I(op_child(g, opt, 0)->opcode, OP_FILTER);

    /* After AND-split + reorder, the inner predicate is the cheaper
     * (eq on i64) and outer predicate is gt on f64 — but here we just
     * need the predicates to point to valid live nodes (no use-after-
     * realloc).  Both pred nodes' IDs must still resolve to live ops. */
    ray_op_t* outer_pred = op_child(g, opt, 1);
    ray_op_t* inner = op_child(g, opt, 0);
    ray_op_t* inner_pred = op_child(g, inner, 1);
    TEST_ASSERT_NOT_NULL(outer_pred);
    TEST_ASSERT_NOT_NULL(inner_pred);
    TEST_ASSERT_TRUE(outer_pred->id == eq_id || outer_pred->id == gt_id);
    TEST_ASSERT_TRUE(inner_pred->id == eq_id || inner_pred->id == gt_id);
    TEST_ASSERT_TRUE(outer_pred->id != inner_pred->id);
    /* Pointer correctness: the resolved nodes should match g->nodes[id]. */
    TEST_ASSERT_EQ_PTR(outer_pred, &g->nodes[outer_pred->id]);
    TEST_ASSERT_EQ_PTR(inner_pred, &g->nodes[inner_pred->id]);

    /* And the filter chain bottoms out at SCAN(v1) — verifying the
     * input-pointer fix-up walked the whole chain correctly. */
    ray_op_t* scan_node = op_child(g, inner, 0);
    TEST_ASSERT_NOT_NULL(scan_node);
    TEST_ASSERT_EQ_I(scan_node->opcode, OP_SCAN);

    /* Original filter id is still valid (it's now the inner filter
     * after split, since split_and_filter rewrites filter_node in
     * place to be the inner). */
    TEST_ASSERT_TRUE(filt_id < g->node_count);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * New targeted tests for uncovered regions
 * -------------------------------------------------------------------------- */

/* Helper: create fresh single-op const fold graph */
#define FOLD_F64_TEST(name, op_fn, ca, cb)                              \
static test_result_t name(void) {                                       \
    ray_heap_init();                                                     \
    ray_t* tbl = make_test_table();                                     \
    ray_graph_t* g = ray_graph_new(tbl);                                \
    ray_op_t* ca_op = ray_const_f64(g, (ca));                          \
    ray_op_t* cb_op = ray_const_f64(g, (cb));                          \
    ray_op_t* binop = op_fn(g, ca_op, cb_op);                          \
    ray_op_t* opt   = ray_optimize(g, binop);                           \
    TEST_ASSERT_NOT_NULL(opt);                                           \
    TEST_ASSERT_EQ_I(opt->opcode, OP_CONST);                            \
    ray_graph_free(g);                                                   \
    ray_release(tbl);                                                    \
    ray_sym_destroy();                                                   \
    ray_heap_destroy();                                                  \
    PASS();                                                              \
}

/*
 * Test: constant folding of arithmetic ops over f64 constants.
 *
 * Exercises fold_binary_const F64 branch: ADD, SUB, MUL, DIV, MOD.
 * Also exercises MIN2 and MAX2 F64 paths via ray_min2 / ray_max2.
 *
 * Each op gets its own graph so DCE in one optimize call
 * doesn't mark sibling nodes dead before they're tested.
 */
FOLD_F64_TEST(test_const_fold_f64_sub, ray_sub, 3.0, 2.0)
FOLD_F64_TEST(test_const_fold_f64_mul, ray_mul, 3.0, 2.0)
FOLD_F64_TEST(test_const_fold_f64_div, ray_div, 3.0, 2.0)
FOLD_F64_TEST(test_const_fold_f64_mod, ray_mod, 3.0, 2.0)
FOLD_F64_TEST(test_const_fold_f64_min, ray_min2, 3.0, 2.0)
FOLD_F64_TEST(test_const_fold_f64_max, ray_max2, 3.0, 2.0)

static test_result_t test_const_fold_f64_arith(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* c3 = ray_const_f64(g, 3.0);
    ray_op_t* c2 = ray_const_f64(g, 2.0);

    /* 3.0 + 2.0 = 5.0 — single optimize call on the add node */
    ray_op_t* add_op = ray_add(g, c3, c2);
    ray_op_t* opt_add = ray_optimize(g, add_op);
    TEST_ASSERT_NOT_NULL(opt_add);
    TEST_ASSERT_EQ_I(opt_add->opcode, OP_CONST);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Helper macros for i64 fold tests — each in its own graph */
#define FOLD_I64_TEST(name, op_fn, ca, cb)                              \
static test_result_t name(void) {                                       \
    ray_heap_init();                                                     \
    ray_t* tbl = make_test_table();                                     \
    ray_graph_t* g = ray_graph_new(tbl);                                \
    ray_op_t* ca_op = ray_const_i64(g, (ca));                          \
    ray_op_t* cb_op = ray_const_i64(g, (cb));                          \
    ray_op_t* binop = op_fn(g, ca_op, cb_op);                          \
    ray_op_t* opt   = ray_optimize(g, binop);                           \
    TEST_ASSERT_NOT_NULL(opt);                                           \
    TEST_ASSERT_EQ_I(opt->opcode, OP_CONST);                            \
    ray_graph_free(g);                                                   \
    ray_release(tbl);                                                    \
    ray_sym_destroy();                                                   \
    ray_heap_destroy();                                                  \
    PASS();                                                              \
}

/*
 * Test: constant folding of integer DIV, MIN2, MAX2 (i64).
 *
 * Exercises fold_binary_const I64 branch: DIV and MIN2, MAX2 arms.
 * Each op gets its own graph.
 */
FOLD_I64_TEST(test_const_fold_i64_div, ray_div, 10, 3)
FOLD_I64_TEST(test_const_fold_i64_min, ray_min2, 10, 3)
FOLD_I64_TEST(test_const_fold_i64_max, ray_max2, 10, 3)
/* Swapped operands: lv < rv so fold_binary_const I64 MIN2 takes the lv branch,
 * and I64 MAX2 takes the rv branch — covers both previously-uncovered ternary arms. */
FOLD_I64_TEST(test_const_fold_i64_min_swap, ray_min2, 3, 10)
FOLD_I64_TEST(test_const_fold_i64_max_swap, ray_max2, 3, 10)

static test_result_t test_const_fold_i64_div_min_max(void) {
    /* Wrapper test that exercises the i64 DIV path inline */
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* c10 = ray_const_i64(g, 10);
    ray_op_t* c3  = ray_const_i64(g, 3);
    ray_op_t* div_op = ray_div(g, c10, c3);

    ray_op_t* opt_div = ray_optimize(g, div_op);
    TEST_ASSERT_NOT_NULL(opt_div);
    TEST_ASSERT_EQ_I(opt_div->opcode, OP_CONST);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Helper macros for i32 const fold tests */
#define FOLD_I32_TEST(name, op_fn, ca, cb)                              \
static test_result_t name(void) {                                       \
    ray_heap_init();                                                     \
    ray_t* tbl = make_test_table();                                     \
    ray_graph_t* g = ray_graph_new(tbl);                                \
    ray_t* _a = ray_i32((int32_t)(ca));                                 \
    ray_t* _b = ray_i32((int32_t)(cb));                                 \
    ray_op_t* ca_op = ray_const_atom(g, _a);                           \
    ray_op_t* cb_op = ray_const_atom(g, _b);                           \
    ray_release(_a); ray_release(_b);                                   \
    ray_op_t* binop = op_fn(g, ca_op, cb_op);                          \
    ray_op_t* opt   = ray_optimize(g, binop);                           \
    TEST_ASSERT_NOT_NULL(opt);                                           \
    TEST_ASSERT_EQ_I(opt->opcode, OP_CONST);                            \
    ray_graph_free(g);                                                   \
    ray_release(tbl);                                                    \
    ray_sym_destroy();                                                   \
    ray_heap_destroy();                                                  \
    PASS();                                                              \
}

/*
 * Test: constant folding over I32 types.
 *
 * Exercises fold_binary_const I32 branch including ADD, DIV and MOD.
 * ray_const_atom with -RAY_I32 atoms exercises atom_to_numeric -RAY_I32 arm.
 * Each op gets its own graph.
 */
FOLD_I32_TEST(test_const_fold_i32_add, ray_add, 7, 3)
FOLD_I32_TEST(test_const_fold_i32_div, ray_div, 7, 3)
FOLD_I32_TEST(test_const_fold_i32_mod, ray_mod, 7, 3)
/* I32 MIN2 and MAX2: covers fold_binary_const I32 lines 469-470 (previously 0 hits) */
FOLD_I32_TEST(test_const_fold_i32_min, ray_min2, 7, 3)
FOLD_I32_TEST(test_const_fold_i32_max, ray_max2, 7, 3)
/* Swapped I32 MIN2/MAX2: lv < rv to cover the alternate branches */
FOLD_I32_TEST(test_const_fold_i32_min_swap, ray_min2, 3, 7)
FOLD_I32_TEST(test_const_fold_i32_max_swap, ray_max2, 3, 7)

static test_result_t test_const_fold_i32_ops(void) {
    /* Wrapper: run the i32 ADD fold inline */
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_t* a7 = ray_i32(7);
    ray_t* a3 = ray_i32(3);
    ray_op_t* c7 = ray_const_atom(g, a7);
    ray_op_t* c3 = ray_const_atom(g, a3);
    ray_release(a7);
    ray_release(a3);

    ray_op_t* add_op  = ray_add(g, c7, c3);
    ray_op_t* opt_add = ray_optimize(g, add_op);
    TEST_ASSERT_NOT_NULL(opt_add);
    TEST_ASSERT_EQ_I(opt_add->opcode, OP_CONST);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: constant folding with i16 atoms exercises atom_to_numeric -RAY_I16 arm.
 *
 * The -RAY_I16 case in atom_to_numeric is the last uncovered scalar type.
 * We use ray_const_atom with a ray_i16() atom.
 *
 * Note: two i16 consts ADD together. The graph's fold_binary_const sees
 * the promote_type result as I16; since there's no I16 case in fold_binary_const
 * it falls through to default → returns false, leaving the node as OP_ADD.
 * But atom_to_numeric -RAY_I16 IS hit on the way in.
 * We just verify the optimize call doesn't crash and node remains valid.
 */
static test_result_t test_const_fold_i16_atom(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_t* a4  = ray_i16(4);
    ray_t* a5  = ray_i16(5);
    ray_op_t* c4 = ray_const_atom(g, a4);
    ray_op_t* c5 = ray_const_atom(g, a5);
    ray_release(a4);
    ray_release(a5);

    /* 4 + 5 — both i16 → triggers atom_to_numeric -RAY_I16 arm in is_const check.
     * The fold may or may not succeed (I16 is not in fold_binary_const switch),
     * but the optimizer must not crash. */
    ray_op_t* add_op = ray_add(g, c4, c5);
    ray_op_t* opt = ray_optimize(g, add_op);
    TEST_ASSERT_NOT_NULL(opt);
    /* After folding: either OP_CONST (folded) or OP_ADD (not folded, still valid) */
    TEST_ASSERT_TRUE(opt->opcode == OP_CONST || opt->opcode == OP_ADD);

    /* Also verify atom_to_bool: build NOT(c4) — unary fold uses atom_to_numeric */
    ray_graph_t* g2 = ray_graph_new(tbl);
    ray_t* a0 = ray_i16(0);
    ray_op_t* cz = ray_const_atom(g2, a0);
    ray_release(a0);
    ray_op_t* not_op = ray_not(g2, cz);
    ray_op_t* opt2 = ray_optimize(g2, not_op);
    TEST_ASSERT_NOT_NULL(opt2);
    /* NOT(0) = true → should fold to OP_CONST */
    TEST_ASSERT_EQ_I(opt2->opcode, OP_CONST);
    ray_graph_free(g2);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning with EQ / NE / LT / LE scalar comparisons.
 *
 * Exercises the currently uncovered scalar comparison arms in the inner loop:
 *   OP_EQ: key == const_val
 *   OP_NE: key != const_val
 *   OP_LT: key <  const_val
 *   OP_LE: key <= const_val
 *
 * Setup: 4 partitions keyed [100, 200, 300, 400].
 *   EQ  300  → bit 2 only
 *   NE  300  → bits 0,1,3
 *   LT  300  → bits 0,1
 *   LE  300  → bits 0,1,2
 */
static void make_parted_tbl(ray_t** out_tbl, ray_graph_t** out_g,
                             ray_op_t** out_scan_val, ray_op_t** out_scan_pkey,
                             int64_t* pkeys, int n_keys) {
    (void)ray_sym_init();

    ray_t* key_values = ray_vec_new(RAY_I64, n_keys);
    key_values->len = n_keys;
    memcpy(ray_data(key_values), pkeys, (size_t)n_keys * sizeof(int64_t));

    ray_t* row_counts = ray_vec_new(RAY_I64, n_keys);
    row_counts->len = n_keys;
    int64_t* rc = (int64_t*)ray_data(row_counts);
    for (int i = 0; i < n_keys; i++) rc[i] = 5;

    ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
    mapcommon->type = RAY_MAPCOMMON;
    mapcommon->len = 2;
    ((ray_t**)ray_data(mapcommon))[0] = key_values;
    ((ray_t**)ray_data(mapcommon))[1] = row_counts;

    ray_t* val_parted = ray_alloc((size_t)n_keys * sizeof(ray_t*));
    val_parted->type = RAY_PARTED_BASE + RAY_I64;
    val_parted->len = n_keys;
    for (int i = 0; i < n_keys; i++) {
        ray_t* seg = ray_vec_new(RAY_I64, 5);
        seg->len = 5;
        ((ray_t**)ray_data(val_parted))[i] = seg;
    }

    int64_t sym_pkey = ray_sym_intern("pkey", 4);
    int64_t sym_val  = ray_sym_intern("val", 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pkey, mapcommon);
    tbl = ray_table_add_col(tbl, sym_val, val_parted);

    ray_graph_t* g = ray_graph_new(tbl);
    *out_scan_val  = ray_scan(g, "val");
    *out_scan_pkey = ray_scan(g, "pkey");
    *out_tbl = tbl;
    *out_g = g;
}

static ray_op_ext_t* find_scan_ext_for(ray_graph_t* g, uint32_t scan_id) {
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == scan_id)
            return g->ext_nodes[i];
    }
    return NULL;
}

static test_result_t test_partition_pruning_eq(void) {
    ray_heap_init();
    int64_t pkeys[] = {100, 200, 300, 400};
    ray_t* tbl; ray_graph_t* g;
    ray_op_t* sv; ray_op_t* sp;
    make_parted_tbl(&tbl, &g, &sv, &sp, pkeys, 4);

    ray_op_t* c300 = ray_const_i64(g, 300);
    ray_op_t* pred = ray_eq(g, sp, c300);
    ray_op_t* filt = ray_filter(g, sv, pred);
    ray_op_t* opt  = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* ext = find_scan_ext_for(g, sv->id);
    TEST_ASSERT_NOT_NULL(ext);
    TEST_ASSERT_NOT_NULL(ext->seg_mask);
    /* Only partition 2 (key=300) matches EQ 300 */
    TEST_ASSERT_TRUE(ext->seg_mask[0] == (1ULL << 2));

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_partition_pruning_ne(void) {
    ray_heap_init();
    int64_t pkeys[] = {100, 200, 300, 400};
    ray_t* tbl; ray_graph_t* g;
    ray_op_t* sv; ray_op_t* sp;
    make_parted_tbl(&tbl, &g, &sv, &sp, pkeys, 4);

    ray_op_t* c300 = ray_const_i64(g, 300);
    ray_op_t* pred = ray_ne(g, sp, c300);
    ray_op_t* filt = ray_filter(g, sv, pred);
    ray_op_t* opt  = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* ext = find_scan_ext_for(g, sv->id);
    TEST_ASSERT_NOT_NULL(ext);
    TEST_ASSERT_NOT_NULL(ext->seg_mask);
    /* Partitions 0,1,3 (keys 100,200,400) match NE 300 */
    uint64_t expected = (1ULL << 0) | (1ULL << 1) | (1ULL << 3);
    TEST_ASSERT_TRUE(ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_partition_pruning_lt(void) {
    ray_heap_init();
    int64_t pkeys[] = {100, 200, 300, 400};
    ray_t* tbl; ray_graph_t* g;
    ray_op_t* sv; ray_op_t* sp;
    make_parted_tbl(&tbl, &g, &sv, &sp, pkeys, 4);

    ray_op_t* c300 = ray_const_i64(g, 300);
    ray_op_t* pred = ray_lt(g, sp, c300);
    ray_op_t* filt = ray_filter(g, sv, pred);
    ray_op_t* opt  = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* ext = find_scan_ext_for(g, sv->id);
    TEST_ASSERT_NOT_NULL(ext);
    TEST_ASSERT_NOT_NULL(ext->seg_mask);
    /* Partitions 0,1 (keys 100,200 < 300) */
    uint64_t expected = (1ULL << 0) | (1ULL << 1);
    TEST_ASSERT_TRUE(ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_partition_pruning_le(void) {
    ray_heap_init();
    int64_t pkeys[] = {100, 200, 300, 400};
    ray_t* tbl; ray_graph_t* g;
    ray_op_t* sv; ray_op_t* sp;
    make_parted_tbl(&tbl, &g, &sv, &sp, pkeys, 4);

    ray_op_t* c300 = ray_const_i64(g, 300);
    ray_op_t* pred = ray_le(g, sp, c300);
    ray_op_t* filt = ray_filter(g, sv, pred);
    ray_op_t* opt  = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* ext = find_scan_ext_for(g, sv->id);
    TEST_ASSERT_NOT_NULL(ext);
    TEST_ASSERT_NOT_NULL(ext->seg_mask);
    /* Partitions 0,1,2 (keys 100,200,300 <= 300) */
    uint64_t expected = (1ULL << 0) | (1ULL << 1) | (1ULL << 2);
    TEST_ASSERT_TRUE(ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning with two filters — exercises the AND-merge path
 * (sn_ext->seg_mask already set when second filter runs).
 *
 * First filter: val >= 200  → bits 1,2,3 (200,300,400)
 * Second filter: val <= 300 → bits 0,1,2 (100,200,300)
 * AND-result: bits 1,2 only (200,300)
 */
static test_result_t test_partition_pruning_and_merge(void) {
    ray_heap_init();
    int64_t pkeys[] = {100, 200, 300, 400};
    ray_t* tbl; ray_graph_t* g;
    ray_op_t* sv; ray_op_t* sp;
    make_parted_tbl(&tbl, &g, &sv, &sp, pkeys, 4);

    /* Re-scan pkey for second predicate — need a fresh scan node */
    ray_op_t* sp2 = ray_scan(g, "pkey");

    ray_op_t* c200 = ray_const_i64(g, 200);
    ray_op_t* c300 = ray_const_i64(g, 300);

    /* FILTER(val >= 200, FILTER(val <= 300, SCAN(val))) */
    ray_op_t* pred1 = ray_ge(g, sp, c200);
    ray_op_t* pred2 = ray_le(g, sp2, c300);
    ray_op_t* filt1 = ray_filter(g, sv, pred1);
    ray_op_t* filt2 = ray_filter(g, filt1, pred2);

    ray_op_t* opt = ray_optimize(g, filt2);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* ext = find_scan_ext_for(g, sv->id);
    TEST_ASSERT_NOT_NULL(ext);
    TEST_ASSERT_NOT_NULL(ext->seg_mask);
    /* AND of (1,2,3) and (0,1,2) = (1,2) */
    uint64_t expected = (1ULL << 1) | (1ULL << 2);
    TEST_ASSERT_TRUE(ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: promote_type — exercises the type-promotion helper used when a node
 * has out_type == 0 and two inputs.
 *
 * To reach promote_type we need a binary op node that somehow has out_type=0
 * when infer_type_for_node runs.  We can achieve this by building a raw binop
 * via ray_binop and then calling ray_optimize.  After graph construction the
 * type should be set, but we can check it arrives at the expected promoted type
 * at execution time by simply folding two consts of different widths.
 *
 * F64 + I64 const → promote to F64.  After folding the result is F64 CONST.
 */
static test_result_t test_type_promote_f64_i64(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* mix: f64 * i64 — graph will infer F64 output via promote_type */
    ray_op_t* cf = ray_const_f64(g, 2.5);
    ray_op_t* ci = ray_const_i64(g, 4);
    ray_op_t* mul = ray_mul(g, cf, ci);
    TEST_ASSERT_NOT_NULL(mul);

    /* Force out_type=0 to trigger infer_type_for_node code paths */
    mul->out_type = 0;
    g->nodes[mul->id].out_type = 0;

    ray_op_t* opt = ray_optimize(g, mul);
    TEST_ASSERT_NOT_NULL(opt);
    /* Should have been constant-folded since both inputs are CONST */
    TEST_ASSERT_EQ_I(opt->opcode, OP_CONST);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: type inference for comparison/bool ops with out_type==0.
 *
 * Build an EQ node then force its out_type to 0 before optimization.
 * infer_type_for_node should set it back to RAY_BOOL.
 *
 * Each test uses its own graph to avoid DCE marking sibling nodes dead.
 */
static test_result_t test_type_infer_cmp_bool(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();

    /* Test 1: AND with out_type=0 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* c1 = ray_const_i64(g, 1);
        ray_op_t* c2 = ray_const_i64(g, 2);
        ray_op_t* eq_op = ray_eq(g, c1, c2);
        eq_op->out_type = 0;
        g->nodes[eq_op->id].out_type = 0;
        ray_op_t* c3 = ray_const_bool(g, true);
        ray_op_t* and_op = ray_and(g, eq_op, c3);
        and_op->out_type = 0;
        g->nodes[and_op->id].out_type = 0;
        ray_op_t* opt_and = ray_optimize(g, and_op);
        TEST_ASSERT_NOT_NULL(opt_and);
        TEST_ASSERT_EQ_I(opt_and->opcode, OP_CONST);
        ray_graph_free(g);
    }

    /* Test 2: OR with out_type=0 */
    {
        ray_graph_t* g = ray_graph_new(tbl);
        ray_op_t* c1 = ray_const_i64(g, 1);
        ray_op_t* c2 = ray_const_i64(g, 2);
        ray_op_t* eq_op = ray_eq(g, c1, c2);
        eq_op->out_type = 0;
        g->nodes[eq_op->id].out_type = 0;
        ray_op_t* c4 = ray_const_bool(g, false);
        ray_op_t* or_op = ray_or(g, eq_op, c4);
        or_op->out_type = 0;
        g->nodes[or_op->id].out_type = 0;
        ray_op_t* opt_or = ray_optimize(g, or_op);
        TEST_ASSERT_NOT_NULL(opt_or);
        TEST_ASSERT_EQ_I(opt_or->opcode, OP_CONST);
        ray_graph_free(g);
    }

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: filter_cost returns cost += 1 for RAY_I16 type.
 *
 * Build a FILTER with a predicate whose input type is I16.
 * With two I16 CONST inputs the optimizer can fold, but before folding
 * filter_cost is called on the predicate.  The test just verifies the
 * optimize + execute path works end-to-end with I16 atom constants.
 */
static test_result_t test_filter_cost_i16_type(void) {
    ray_heap_init();

    /* Build a small table with I16 column */
    int16_t raw[] = {1, 2, 3, 4, 5};
    ray_t* col = ray_vec_from_raw(RAY_I16, raw, 5);
    int64_t sym_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, sym_v, col);
    ray_release(col);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan = ray_scan(g, "v");

    /* Predicate: v >= const_i16(3) — uses i16 atom path */
    ray_t* a3 = ray_i16(3);
    ray_op_t* c3 = ray_const_atom(g, a3);
    ray_release(a3);

    ray_op_t* pred = ray_ge(g, scan, c3);
    ray_op_t* filt = ray_filter(g, scan, pred);
    /* Chain two filters so collect_filter_chain finds len >= 2 */
    ray_t* a4 = ray_i16(4);
    ray_op_t* c4 = ray_const_atom(g, a4);
    ray_release(a4);
    ray_op_t* scan2 = ray_scan(g, "v");
    ray_op_t* pred2 = ray_ge(g, scan2, c4);
    ray_op_t* filt2 = ray_filter(g, filt, pred2);

    /* Optimize — this calls filter_cost on I16-typed predicate inputs */
    ray_op_t* opt = ray_optimize(g, filt2);
    TEST_ASSERT_NOT_NULL(opt);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning with an I32-keyed partition column.
 *
 * Exercises the int32_t partition-key read path in pass_partition_pruning:
 *   if (key_values->type == RAY_DATE || RAY_I32 || RAY_TIME) { memcpy v32 }
 *
 * Setup: 4 partitions keyed by I32 values [10, 20, 30, 40].
 * Filter: pkey >= 30 → bits 2,3 set.
 */
static test_result_t test_partition_pruning_i32_keys(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build I32 partition keys */
    ray_t* key_values = ray_vec_new(RAY_I32, 4);
    key_values->len = 4;
    int32_t keys32[] = {10, 20, 30, 40};
    memcpy(ray_data(key_values), keys32, sizeof(keys32));

    ray_t* row_counts = ray_vec_new(RAY_I64, 4);
    row_counts->len = 4;
    int64_t counts[] = {5, 5, 5, 5};
    memcpy(ray_data(row_counts), counts, sizeof(counts));

    ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
    mapcommon->type = RAY_MAPCOMMON;
    mapcommon->len = 2;
    ((ray_t**)ray_data(mapcommon))[0] = key_values;
    ((ray_t**)ray_data(mapcommon))[1] = row_counts;

    ray_t* val_parted = ray_alloc(4 * sizeof(ray_t*));
    val_parted->type = RAY_PARTED_BASE + RAY_I64;
    val_parted->len = 4;
    for (int i = 0; i < 4; i++) {
        ray_t* seg = ray_vec_new(RAY_I64, 5);
        seg->len = 5;
        ((ray_t**)ray_data(val_parted))[i] = seg;
    }

    int64_t sym_pkey = ray_sym_intern("pkey", 4);
    int64_t sym_val  = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pkey, mapcommon);
    tbl = ray_table_add_col(tbl, sym_val, val_parted);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_val  = ray_scan(g, "val");
    ray_op_t* scan_pkey = ray_scan(g, "pkey");

    /* Predicate: pkey >= 30 using an i32 atom */
    ray_t* a30 = ray_i32(30);
    ray_op_t* c30 = ray_const_atom(g, a30);
    ray_release(a30);
    ray_op_t* pred = ray_ge(g, scan_pkey, c30);
    ray_op_t* filt = ray_filter(g, scan_val, pred);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* val_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == scan_val->id) {
            val_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(val_ext);
    TEST_ASSERT_NOT_NULL(val_ext->seg_mask);
    /* keys 30,40 >= 30 → bits 2,3 */
    uint64_t expected = (1ULL << 2) | (1ULL << 3);
    TEST_ASSERT_TRUE(val_ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(mapcommon);
    ray_release(val_parted);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: factorize_pass with EXPAND -> GROUP(_src) pattern.
 *
 * Builds: GROUP(_src, SUM(expand_result)) over EXPAND(SCAN(id), rel).
 * The factorize_pass looks for expand->GROUP where the group key is "_src".
 * This exercises the factorized=1 branch (currently 0 coverage).
 */
static test_result_t test_factorize_expand_group_src(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a simple directed graph: edges 0->1, 1->2 */
    int64_t src_data[] = {0, 1};
    int64_t dst_data[] = {1, 2};
    ray_t* src_v = ray_vec_from_raw(RAY_I64, src_data, 2);
    ray_t* dst_v = ray_vec_from_raw(RAY_I64, dst_data, 2);
    int64_t s_src = ray_sym_intern("src", 3);
    int64_t s_dst = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, s_src, src_v);
    edges = ray_table_add_col(edges, s_dst, dst_v);
    ray_release(src_v);
    ray_release(dst_v);

    /* node table: id column with 3 nodes */
    int64_t id_data[] = {0, 1, 2};
    int64_t val_data[] = {10, 20, 30};
    ray_t* id_v  = ray_vec_from_raw(RAY_I64, id_data, 3);
    ray_t* val_v = ray_vec_from_raw(RAY_I64, val_data, 3);
    int64_t s_id  = ray_sym_intern("id", 2);
    int64_t s_val = ray_sym_intern("val", 3);
    ray_t* node_tbl = ray_table_new(2);
    node_tbl = ray_table_add_col(node_tbl, s_id, id_v);
    node_tbl = ray_table_add_col(node_tbl, s_val, val_v);
    ray_release(id_v);
    ray_release(val_v);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 3, 3, false);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(node_tbl);

    /* EXPAND from all node IDs */
    int64_t start_data[] = {0, 1, 2};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 3);
    ray_op_t* src_op = ray_const_vec(g, start_vec);
    ray_release(start_vec);

    ray_op_t* expand = ray_expand(g, src_op, rel, 0);
    TEST_ASSERT_NOT_NULL(expand);

    /* Build GROUP with key = _src scan */
    ray_op_t* src_scan = ray_scan(g, "_src");
    ray_op_t* val_scan = ray_scan(g, "_val");
    ray_op_t* keys[] = { src_scan };
    uint16_t agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { val_scan };
    (void)agg_ins;  /* may not execute, just build the DAG */
    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(grp);

    /* Attach group as consumer of expand */
    grp->in_id[0] = expand->id;
    g->nodes[grp->id].in_id[0] = expand->id;

    ray_op_t* opt = ray_optimize(g, grp);
    TEST_ASSERT_NOT_NULL(opt);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(node_tbl);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: filter_const_predicate fold — FILTER with const-false pred → OP_HEAD(0).
 *
 * Exercises fold_filter_const_predicate "false" branch (OP_HEAD result).
 * Also exercises atom_to_bool with is_f64 branch (f64 0.0 → false).
 */
static test_result_t test_filter_const_false_pred(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1  = ray_scan(g, "v1");
    /* Const false predicate from a bool false const */
    ray_op_t* cf  = ray_const_bool(g, false);
    ray_op_t* filt = ray_filter(g, v1, cf);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);
    /* After fold: FILTER(false, input) → OP_HEAD (0 rows) */
    TEST_ASSERT_EQ_I(opt->opcode, OP_HEAD);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: filter_const_predicate fold with f64 const zero — exercises
 * atom_to_bool's is_f64 branch (vf == 0.0 → false).
 */
static test_result_t test_filter_const_f64_zero_pred(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* v1   = ray_scan(g, "v1");
    ray_op_t* cf   = ray_const_f64(g, 0.0);
    ray_op_t* filt = ray_filter(g, v1, cf);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);
    /* 0.0 → false → OP_HEAD */
    TEST_ASSERT_EQ_I(opt->opcode, OP_HEAD);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_idiom_first_last_asc_scan_no_nulls(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t data[] = { 4, 1, 7, 3 };
    ray_t* vec = ray_vec_from_raw(RAY_I64, data, 4);
    ray_t* tbl = ray_table_new(1);
    int64_t name_v = ray_sym_intern("v", 1);
    tbl = ray_table_add_col(tbl, name_v, vec);
    ray_release(vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_first = ray_scan(g, "v");
    ray_op_t* first = ray_first(g, ray_asc_op(g, scan_first));
    ray_op_t* scan_last = ray_scan(g, "v");
    ray_op_t* last = ray_last(g, ray_asc_op(g, scan_last));
    ray_op_t* root = ray_add(g, first, last);

    ray_op_t* opt = ray_optimize(g, root);
    TEST_ASSERT_NOT_NULL(opt);
    TEST_ASSERT_EQ_I(opt->opcode, OP_ADD);
    TEST_ASSERT_EQ_I(op_child(g, opt, 0)->opcode, OP_MIN);
    TEST_ASSERT_EQ_I(op_child(g, opt, 1)->opcode, OP_MAX);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_idiom_first_asc_scan_with_nulls_stays_safe(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t data[] = { 4, 1, 7, 3 };
    ray_t* vec = ray_vec_from_raw(RAY_I64, data, 4);
    ray_vec_set_null(vec, 1, true);

    ray_t* tbl = ray_table_new(1);
    int64_t name_v = ray_sym_intern("v", 1);
    tbl = ray_table_add_col(tbl, name_v, vec);
    ray_release(vec);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* first = ray_first(g, ray_asc_op(g, ray_scan(g, "v")));
    ray_op_t* root = ray_add(g, first, ray_const_i64(g, 0));

    ray_op_t* opt = ray_optimize(g, root);
    TEST_ASSERT_NOT_NULL(opt);
    TEST_ASSERT_EQ_I(opt->opcode, OP_ADD);
    TEST_ASSERT_EQ_I(op_child(g, opt, 0)->opcode, OP_FIRST);
    TEST_ASSERT_EQ_I(op_child(g, op_child(g, opt, 0), 0)->opcode, OP_ASC);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * New targeted coverage tests for opt.c uncovered regions
 * -------------------------------------------------------------------------- */

/*
 * Test: partition pruning with OP_GT comparison.
 * Exercises the case OP_GT arm in the inner switch (line ~1954 in opt.c).
 * 4 partitions keyed [100, 200, 300, 400].
 * pkey > 200 → partitions 2,3 (keys 300,400) → bits 2,3.
 */
static test_result_t test_partition_pruning_gt(void) {
    ray_heap_init();
    int64_t pkeys[] = {100, 200, 300, 400};
    ray_t* tbl; ray_graph_t* g;
    ray_op_t* sv; ray_op_t* sp;
    make_parted_tbl(&tbl, &g, &sv, &sp, pkeys, 4);

    ray_op_t* c200 = ray_const_i64(g, 200);
    ray_op_t* pred = ray_gt(g, sp, c200);
    ray_op_t* filt = ray_filter(g, sv, pred);
    ray_op_t* opt  = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* ext = find_scan_ext_for(g, sv->id);
    TEST_ASSERT_NOT_NULL(ext);
    TEST_ASSERT_NOT_NULL(ext->seg_mask);
    /* Partitions 2,3 (keys 300,400 > 200) */
    uint64_t expected = (1ULL << 2) | (1ULL << 3);
    TEST_ASSERT_TRUE(ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning with swapped operands — CONST on lhs, SCAN on rhs.
 * Exercises the swapped=true path (lines ~1776-1777, ~1925-1930) where
 * LT is flipped to GT for effective comparison.
 *
 * FILTER( SCAN(val), LT(CONST(200), SCAN(pkey)) )
 * → equivalent to pkey > 200 → bits 2,3.
 */
static test_result_t test_partition_pruning_swapped(void) {
    ray_heap_init();
    int64_t pkeys[] = {100, 200, 300, 400};
    ray_t* tbl; ray_graph_t* g;
    ray_op_t* sv; ray_op_t* sp;
    make_parted_tbl(&tbl, &g, &sv, &sp, pkeys, 4);

    /* Swapped: CONST(200) < SCAN(pkey), i.e. lhs=CONST, rhs=SCAN */
    ray_op_t* c200 = ray_const_i64(g, 200);
    ray_op_t* pred = ray_lt(g, c200, sp);  /* CONST < SCAN → swapped OP_LT */
    ray_op_t* filt = ray_filter(g, sv, pred);
    ray_op_t* opt  = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* ext = find_scan_ext_for(g, sv->id);
    TEST_ASSERT_NOT_NULL(ext);
    TEST_ASSERT_NOT_NULL(ext->seg_mask);
    /* swapped LT → eff_op=GT → pkey > 200 → partitions 2,3 */
    uint64_t expected = (1ULL << 2) | (1ULL << 3);
    TEST_ASSERT_TRUE(ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning with large IN set (> 32 elements).
 * Exercises the heap allocation path (set_len > 32 → ray_alloc) in
 * pass_partition_pruning (lines ~1877-1881).
 *
 * Build 6 partitions keyed [10,20,30,40,50,60].
 * IN set has 33 elements covering keys 10,20,30,40 → bits 0,1,2,3.
 */
static test_result_t test_partition_pruning_in_large(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 6 I64 partition keys */
    int64_t pkey_arr[] = {10, 20, 30, 40, 50, 60};
    int n_parts = 6;

    ray_t* key_values = ray_vec_new(RAY_I64, n_parts);
    key_values->len = n_parts;
    memcpy(ray_data(key_values), pkey_arr, (size_t)n_parts * sizeof(int64_t));

    ray_t* row_counts = ray_vec_new(RAY_I64, n_parts);
    row_counts->len = n_parts;
    int64_t* rc = (int64_t*)ray_data(row_counts);
    for (int i = 0; i < n_parts; i++) rc[i] = 5;

    ray_t* mapcommon = ray_alloc(2 * sizeof(ray_t*));
    mapcommon->type = RAY_MAPCOMMON;
    mapcommon->len = 2;
    ((ray_t**)ray_data(mapcommon))[0] = key_values;
    ((ray_t**)ray_data(mapcommon))[1] = row_counts;

    ray_t* val_parted = ray_alloc((size_t)n_parts * sizeof(ray_t*));
    val_parted->type = RAY_PARTED_BASE + RAY_I64;
    val_parted->len = n_parts;
    for (int i = 0; i < n_parts; i++) {
        ray_t* seg = ray_vec_new(RAY_I64, 5);
        seg->len = 5;
        ((ray_t**)ray_data(val_parted))[i] = seg;
    }

    int64_t sym_pkey = ray_sym_intern("pkey", 4);
    int64_t sym_val  = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pkey, mapcommon);
    tbl = ray_table_add_col(tbl, sym_val, val_parted);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_val  = ray_scan(g, "val");
    ray_op_t* scan_pkey = ray_scan(g, "pkey");

    /* Build IN set with 33 elements: 10,20,30,40, then 29 extra values
     * not in partition keys (100..128).  Only 10,20,30,40 are in keys. */
    int64_t set_data[33];
    set_data[0] = 10;
    set_data[1] = 20;
    set_data[2] = 30;
    set_data[3] = 40;
    for (int i = 4; i < 33; i++) set_data[i] = 100 + i;  /* not in keys */
    ray_t* set_vec = ray_vec_new(RAY_I64, 33);
    set_vec->len = 33;
    memcpy(ray_data(set_vec), set_data, 33 * sizeof(int64_t));

    ray_op_t* set_op  = ray_const_vec(g, set_vec);
    ray_release(set_vec);
    ray_op_t* in_pred = ray_in(g, scan_pkey, set_op);
    ray_op_t* filt    = ray_filter(g, scan_val, in_pred);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* val_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == scan_val->id) {
            val_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(val_ext);
    TEST_ASSERT_NOT_NULL(val_ext->seg_mask);
    /* Keys 10,20,30,40 are in the set → bits 0,1,2,3 */
    uint64_t expected = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 3);
    TEST_ASSERT_TRUE(val_ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(mapcommon);
    ray_release(val_parted);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning with empty IN set (set_len == 0).
 * Exercises the early goto attach_mask path when no elements match any
 * partition (lines ~1867-1876).  For OP_IN, empty → mask stays all-0,
 * meaning no partitions pass.
 */
static test_result_t test_partition_pruning_in_empty(void) {
    ray_heap_init();
    int64_t pkeys[] = {100, 200, 300, 400};
    ray_t* tbl; ray_graph_t* g;
    ray_op_t* sv; ray_op_t* sp;
    make_parted_tbl(&tbl, &g, &sv, &sp, pkeys, 4);

    /* Empty I64 vector as the IN set */
    ray_t* empty_vec = ray_vec_new(RAY_I64, 1);  /* capacity=1, len=0 */
    empty_vec->len = 0;

    ray_op_t* set_op  = ray_const_vec(g, empty_vec);
    ray_release(empty_vec);
    ray_op_t* in_pred = ray_in(g, sp, set_op);
    ray_op_t* filt    = ray_filter(g, sv, in_pred);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* ext = find_scan_ext_for(g, sv->id);
    TEST_ASSERT_NOT_NULL(ext);
    /* Empty IN → mask all-0 → seg_mask attached but zero */
    TEST_ASSERT_NOT_NULL(ext->seg_mask);
    TEST_ASSERT_TRUE(ext->seg_mask[0] == 0);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning with NOT_IN and empty set.
 * For NOT_IN with empty set → all partitions pass → mask is all-1s.
 * Exercises the is_nin branch in the empty-set path (lines ~1871-1875).
 */
static test_result_t test_partition_pruning_not_in_empty(void) {
    ray_heap_init();
    int64_t pkeys[] = {100, 200, 300, 400};
    ray_t* tbl; ray_graph_t* g;
    ray_op_t* sv; ray_op_t* sp;
    make_parted_tbl(&tbl, &g, &sv, &sp, pkeys, 4);

    /* Empty I64 vector */
    ray_t* empty_vec = ray_vec_new(RAY_I64, 1);
    empty_vec->len = 0;

    ray_op_t* set_op    = ray_const_vec(g, empty_vec);
    ray_release(empty_vec);
    ray_op_t* nin_pred  = ray_not_in(g, sp, set_op);
    ray_op_t* filt      = ray_filter(g, sv, nin_pred);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* ext = find_scan_ext_for(g, sv->id);
    TEST_ASSERT_NOT_NULL(ext);
    TEST_ASSERT_NOT_NULL(ext->seg_mask);
    /* NOT_IN empty → all 4 partitions pass → bits 0-3 set */
    uint64_t expected = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 3);
    TEST_ASSERT_TRUE(ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: partition pruning with IN set containing null elements.
 * Exercises the set_has_nulls path (lines ~1885-1897) where null entries
 * in the literal vector are skipped.
 *
 * Setup: 4 partitions keyed [100, 200, 300, 400].
 * IN set: [100, NULL, 300] → only 100 and 300 match → bits 0,2.
 */
static test_result_t test_partition_pruning_in_with_nulls(void) {
    ray_heap_init();
    int64_t pkeys[] = {100, 200, 300, 400};
    ray_t* tbl; ray_graph_t* g;
    ray_op_t* sv; ray_op_t* sp;
    make_parted_tbl(&tbl, &g, &sv, &sp, pkeys, 4);

    /* Build [100, NULL, 300] as an I64 vec with null at index 1 */
    int64_t set_data[] = {100, 0, 300};  /* 0 is null placeholder */
    ray_t* set_vec = ray_vec_new(RAY_I64, 3);
    set_vec->len = 3;
    memcpy(ray_data(set_vec), set_data, 3 * sizeof(int64_t));
    ray_vec_set_null(set_vec, 1, true);  /* index 1 is null */

    ray_op_t* set_op  = ray_const_vec(g, set_vec);
    ray_release(set_vec);
    ray_op_t* in_pred = ray_in(g, sp, set_op);
    ray_op_t* filt    = ray_filter(g, sv, in_pred);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    ray_op_ext_t* ext = find_scan_ext_for(g, sv->id);
    TEST_ASSERT_NOT_NULL(ext);
    TEST_ASSERT_NOT_NULL(ext->seg_mask);
    /* Only keys 100 and 300 are non-null in the set → bits 0,2 */
    uint64_t expected = (1ULL << 0) | (1ULL << 2);
    TEST_ASSERT_TRUE(ext->seg_mask[0] == expected);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: predicate pushdown past EXPAND with OP_IF predicate.
 * Exercises collect_pred_scans and is_reachable_from when the predicate
 * subtree contains an OP_IF node (which has a third operand stored in ext).
 *
 * This covers the OP_IF/OP_SUBSTR/OP_REPLACE case in collect_pred_scans
 * (lines ~1221-1229) and is_reachable_from (lines ~1283-1289).
 *
 * Predicate: IF(flag == 1, flag, 0) — has three sub-nodes including flag_scan.
 * The flag_scan is reachable from EXPAND's source subtree, so pushdown fires.
 */
static test_result_t test_opt_pushdown_expand_if_pred(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 4;
    int64_t flag_data[] = {1, 0, 1, 0};
    int64_t id_data[]   = {0, 1, 2, 3};
    ray_t* flag_v = ray_vec_from_raw(RAY_I64, flag_data, n);
    ray_t* id_v   = ray_vec_from_raw(RAY_I64, id_data, n);
    int64_t s_flag = ray_sym_intern("flag", 4);
    int64_t s_id   = ray_sym_intern("id", 2);
    ray_t* node_tbl = ray_table_new(2);
    node_tbl = ray_table_add_col(node_tbl, s_id, id_v);
    node_tbl = ray_table_add_col(node_tbl, s_flag, flag_v);
    ray_release(flag_v);
    ray_release(id_v);

    /* Simple edge graph: 0->1, 1->2 */
    int64_t src_data[] = {0, 1};
    int64_t dst_data[] = {1, 2};
    ray_t* src_v = ray_vec_from_raw(RAY_I64, src_data, 2);
    ray_t* dst_v = ray_vec_from_raw(RAY_I64, dst_data, 2);
    int64_t s_src = ray_sym_intern("src", 3);
    int64_t s_dst = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, s_src, src_v);
    edges = ray_table_add_col(edges, s_dst, dst_v);
    ray_release(src_v);
    ray_release(dst_v);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 4, 4, false);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(node_tbl);

    /* Build predicate: IF(flag == 1, flag, 0) */
    ray_op_t* flag_scan = ray_scan(g, "flag");
    ray_op_t* c1   = ray_const_i64(g, 1);
    ray_op_t* c0   = ray_const_i64(g, 0);
    ray_op_t* cond = ray_eq(g, flag_scan, c1);
    ray_op_t* pred = ray_if(g, cond, flag_scan, c0);  /* IF has ext-stored third op */

    /* EXPAND from flag column (so flag_scan is in source subtree) */
    ray_op_t* expand = ray_expand(g, flag_scan, rel, 0);
    TEST_ASSERT_NOT_NULL(expand);
    uint32_t expand_id = expand->id;

    /* FILTER(expand, if_pred) — collect_pred_scans must walk OP_IF ext */
    ray_op_t* filt = ray_filter(g, expand, pred);
    TEST_ASSERT_NOT_NULL(filt);

    /* Optimize — pushdown should fire since flag_scan is reachable from
     * expand's source input */
    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);

    /* After pushdown, root should be EXPAND with FILTER as its source input */
    TEST_ASSERT_EQ_U(opt->id, expand_id);
    TEST_ASSERT_EQ_I(opt->opcode, OP_EXPAND);
    TEST_ASSERT_NOT_NULL(op_child(g, opt, 0));
    TEST_ASSERT_EQ_I(op_child(g, opt, 0)->opcode, OP_FILTER);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(node_tbl);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: factorize_pass with EXPAND where consumer is NOT an OP_GROUP.
 * Exercises the factorize_pass early-exit when consumer->opcode != OP_GROUP
 * (line ~1004 in opt.c).
 */
static test_result_t test_factorize_expand_non_group(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src_data[] = {0, 1};
    int64_t dst_data[] = {1, 2};
    ray_t* src_v = ray_vec_from_raw(RAY_I64, src_data, 2);
    ray_t* dst_v = ray_vec_from_raw(RAY_I64, dst_data, 2);
    int64_t s_src = ray_sym_intern("src", 3);
    int64_t s_dst = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, s_src, src_v);
    edges = ray_table_add_col(edges, s_dst, dst_v);
    ray_release(src_v);
    ray_release(dst_v);

    int64_t id_data[] = {0, 1, 2};
    ray_t* id_v = ray_vec_from_raw(RAY_I64, id_data, 3);
    int64_t s_id = ray_sym_intern("id", 2);
    ray_t* node_tbl = ray_table_new(1);
    node_tbl = ray_table_add_col(node_tbl, s_id, id_v);
    ray_release(id_v);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 3, 3, false);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(node_tbl);
    int64_t start_data[] = {0, 1, 2};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 3);
    ray_op_t* src_op = ray_const_vec(g, start_vec);
    ray_release(start_vec);

    ray_op_t* expand = ray_expand(g, src_op, rel, 0);
    TEST_ASSERT_NOT_NULL(expand);

    /* Consumer is OP_FILTER (not OP_GROUP) — factorize_pass must skip */
    ray_op_t* c0   = ray_const_i64(g, 0);
    ray_op_t* pred = ray_gt(g, expand, c0);
    ray_op_t* filt = ray_filter(g, expand, pred);

    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);
    /* factorized flag must NOT be set since consumer was OP_FILTER */
    ray_op_ext_t* expand_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == expand->id) {
            expand_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(expand_ext);
    TEST_ASSERT_TRUE(expand_ext->graph.factorized == 0);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(node_tbl);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: factorize_pass with EXPAND → GROUP where key is NOT "_src".
 * Exercises the key-sym check (line ~1013) when key_ext->sym != src_sym,
 * so factorized is NOT set.
 */
static test_result_t test_factorize_expand_group_non_src(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t src_data[] = {0, 1};
    int64_t dst_data[] = {1, 2};
    ray_t* src_v = ray_vec_from_raw(RAY_I64, src_data, 2);
    ray_t* dst_v = ray_vec_from_raw(RAY_I64, dst_data, 2);
    int64_t s_src = ray_sym_intern("src", 3);
    int64_t s_dst = ray_sym_intern("dst", 3);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, s_src, src_v);
    edges = ray_table_add_col(edges, s_dst, dst_v);
    ray_release(src_v);
    ray_release(dst_v);

    int64_t id_data[] = {0, 1, 2};
    int64_t val_data[] = {10, 20, 30};
    ray_t* id_v  = ray_vec_from_raw(RAY_I64, id_data, 3);
    ray_t* val_v = ray_vec_from_raw(RAY_I64, val_data, 3);
    int64_t s_id  = ray_sym_intern("id", 2);
    int64_t s_val = ray_sym_intern("val", 3);
    ray_t* node_tbl = ray_table_new(2);
    node_tbl = ray_table_add_col(node_tbl, s_id, id_v);
    node_tbl = ray_table_add_col(node_tbl, s_val, val_v);
    ray_release(id_v);
    ray_release(val_v);

    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 3, 3, false);
    TEST_ASSERT_NOT_NULL(rel);

    ray_graph_t* g = ray_graph_new(node_tbl);
    int64_t start_data[] = {0, 1, 2};
    ray_t* start_vec = ray_vec_from_raw(RAY_I64, start_data, 3);
    ray_op_t* src_op = ray_const_vec(g, start_vec);
    ray_release(start_vec);

    ray_op_t* expand = ray_expand(g, src_op, rel, 0);
    TEST_ASSERT_NOT_NULL(expand);

    /* GROUP with key = "val" (not "_src") — factorize should NOT fire */
    ray_op_t* val_scan = ray_scan(g, "val");
    ray_op_t* keys[] = { val_scan };
    uint16_t agg_ops[] = { OP_COUNT };
    ray_op_t* agg_ins[] = { val_scan };
    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);
    TEST_ASSERT_NOT_NULL(grp);
    grp->in_id[0] = expand->id;
    g->nodes[grp->id].in_id[0] = expand->id;

    ray_op_t* opt = ray_optimize(g, grp);
    TEST_ASSERT_NOT_NULL(opt);

    /* factorized must NOT be set — key is "val", not "_src" */
    ray_op_ext_t* expand_ext = NULL;
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == expand->id) {
            expand_ext = g->ext_nodes[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(expand_ext);
    TEST_ASSERT_TRUE(expand_ext->graph.factorized == 0);

    ray_graph_free(g);
    ray_rel_free(rel);
    ray_release(node_tbl);
    ray_release(edges);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: graph_alloc_node_opt fix-up for JOIN and ANTIJOIN ext nodes.
 * When graph realloc fires during split_and_filter, the fix-up loop must
 * also relocate input pointers stored in OP_JOIN and OP_ANTIJOIN ext nodes.
 *
 * Exercises the OP_JOIN and OP_ANTIJOIN branches (lines ~1075-1103) of the
 * pointer fix-up inside graph_alloc_node_opt.
 */
static test_result_t test_opt_realloc_with_join_ext(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a table with two I64 columns for join */
    int64_t key_data[] = {1, 2, 3, 4};
    int64_t val_data[] = {10, 20, 30, 40};
    ray_t* key_v = ray_vec_from_raw(RAY_I64, key_data, 4);
    ray_t* val_v = ray_vec_from_raw(RAY_I64, val_data, 4);
    int64_t s_k = ray_sym_intern("k", 1);
    int64_t s_v = ray_sym_intern("v", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, s_k, key_v);
    tbl = ray_table_add_col(tbl, s_v, val_v);
    ray_release(key_v);
    ray_release(val_v);

    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* scan_k_l = ray_scan(g, "k");
    ray_op_t* scan_v_l = ray_scan(g, "v");
    ray_op_t* scan_k_r = ray_scan(g, "k");
    ray_op_t* scan_v_r = ray_scan(g, "v");

    /* Build JOIN and ANTIJOIN — both have ext nodes with key pointers */
    ray_op_t* lkeys[] = { scan_k_l };
    ray_op_t* rkeys[] = { scan_k_r };
    (void)ray_join(g, scan_v_l, lkeys, scan_v_r, rkeys, 1, 0);
    (void)ray_antijoin(g, scan_v_l, lkeys, scan_v_r, rkeys, 1);

    /* Build AND filter to trigger realloc via split */
    ray_op_t* scan_k2 = ray_scan(g, "k");
    ray_op_t* scan_v2 = ray_scan(g, "v");
    ray_op_t* c1 = ray_const_i64(g, 1);
    ray_op_t* c5 = ray_const_i64(g, 5);
    ray_op_t* eq1 = ray_eq(g, scan_k2, c1);
    ray_op_t* gt5 = ray_gt(g, scan_v2, c5);
    ray_op_t* and_p = ray_and(g, eq1, gt5);
    ray_op_t* filt = ray_filter(g, scan_v2, and_p);

    /* Force realloc by filling node cap */
    while (g->node_count < g->node_cap) {
        (void)ray_const_i64(g, 0);
    }

    uint32_t cap_before = g->node_cap;
    ray_op_t* opt = ray_optimize(g, filt);
    TEST_ASSERT_NOT_NULL(opt);
    TEST_ASSERT_TRUE(g->node_cap > cap_before);  /* realloc fired */

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/*
 * Test: infer_type_for_node unary branch.
 * Exercises the else-if branch (node->arity >= 1 && node->inputs[0])
 * that propagates out_type from the single input (lines 82-84 in opt.c).
 *
 * Build a unary op node with out_type=0 and force type inference.
 * We use ray_neg (arity=1) and manually zero out_type before optimize.
 */
static test_result_t test_type_infer_unary(void) {
    ray_heap_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* neg(scan(v1)) — after zeroing out_type, infer should copy from input */
    ray_op_t* v1  = ray_scan(g, "v1");
    ray_op_t* neg = ray_neg(g, v1);
    TEST_ASSERT_NOT_NULL(neg);

    /* Force out_type to 0 — will trigger infer_type_for_node arity>=1 branch */
    neg->out_type = 0;
    g->nodes[neg->id].out_type = 0;

    ray_op_t* opt = ray_optimize(g, neg);
    TEST_ASSERT_NOT_NULL(opt);
    /* After inference: neg's out_type should match v1's type (RAY_I64) */
    /* The neg node may be dead after DCE, but the graph should still be valid */

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* A source-domain-dependent predicate is a boundary, not a rankable cheap
 * comparison.  The equality on AVG would otherwise sort ahead of a range. */
static test_result_t test_filter_domain_barriers(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_test_table();
    ray_graph_t* g = ray_graph_new(tbl);
    uint32_t input = ray_const_table(g, tbl)->id;
    uint32_t scan = ray_scan(g, "v1")->id;
    uint32_t threshold = ray_const_i64(g, 50)->id;
    uint32_t scalar = ray_const_f64(g, 80.0)->id;
    uint32_t pointwise = ray_gt(g, &g->nodes[scan], &g->nodes[threshold])->id;
    uint32_t avg = ray_avg(g, &g->nodes[scan])->id;
    uint32_t nonlocal = ray_eq(g, &g->nodes[avg], &g->nodes[scalar])->id;
    uint32_t inner = ray_filter(g, &g->nodes[input], &g->nodes[pointwise])->id;
    ray_op_t* root = ray_filter(g, &g->nodes[inner], &g->nodes[nonlocal]);
    root = ray_optimize(g, root);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQ_I(root->opcode, OP_FILTER);
    TEST_ASSERT_EQ_I(root->in_id[1], nonlocal);
    TEST_ASSERT_EQ_I(g->nodes[inner].in_id[1], pointwise);
    ray_graph_free(g);

    /* No extracting a safe inner limb of an unsafe whole conjunction. */
    g = ray_graph_new(tbl);
    input = ray_const_table(g, tbl)->id;
    scan = ray_scan(g, "v1")->id;
    threshold = ray_const_i64(g, 50)->id;
    scalar = ray_const_f64(g, 80.0)->id;
    pointwise = ray_gt(g, &g->nodes[scan], &g->nodes[threshold])->id;
    avg = ray_avg(g, &g->nodes[scan])->id;
    nonlocal = ray_eq(g, &g->nodes[avg], &g->nodes[scalar])->id;
    uint32_t conjunction = ray_and(g, &g->nodes[pointwise], &g->nodes[nonlocal])->id;
    root = ray_filter(g, &g->nodes[input], &g->nodes[conjunction]);
    root = ray_optimize(g, root);
    TEST_ASSERT_NOT_NULL(root);
    int filters = 0;
    for (uint32_t i = 0; i < g->node_count; i++)
        if (!(g->nodes[i].flags & OP_FLAG_DEAD) && g->nodes[i].opcode == OP_FILTER)
            filters++;
    TEST_ASSERT_EQ_I(filters, 1);
    TEST_ASSERT_EQ_I(g->nodes[root->in_id[1]].opcode, OP_AND);
    ray_graph_free(g);

    /* Positive route control: don't repair this by disabling all splitting. */
    g = ray_graph_new(tbl);
    input = ray_const_table(g, tbl)->id;
    scan = ray_scan(g, "v1")->id;
    uint32_t scan2 = ray_scan(g, "id1")->id;
    threshold = ray_const_i64(g, 50)->id;
    uint32_t one = ray_const_i64(g, 1)->id;
    pointwise = ray_gt(g, &g->nodes[scan], &g->nodes[threshold])->id;
    uint32_t other = ray_eq(g, &g->nodes[scan2], &g->nodes[one])->id;
    conjunction = ray_and(g, &g->nodes[pointwise], &g->nodes[other])->id;
    root = ray_filter(g, &g->nodes[input], &g->nodes[conjunction]);
    root = ray_optimize(g, root);
    TEST_ASSERT_NOT_NULL(root);
    filters = 0;
    for (uint32_t i = 0; i < g->node_count; i++)
        if (!(g->nodes[i].flags & OP_FLAG_DEAD) && g->nodes[i].opcode == OP_FILTER)
            filters++;
    TEST_ASSERT_EQ_I(filters, 2);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t opt_entries[] = {
    { "opt/subset_property", test_subset_property, NULL, NULL },
    { "opt/filter_domain_barriers", test_filter_domain_barriers, NULL, NULL },
    { "opt/subset_proof_pointwise", test_subset_proof_pointwise, NULL, NULL },
    { "opt/subset_proof_membership_roles", test_subset_proof_membership_roles, NULL, NULL },
    { "opt/subset_proof_hidden_crossrow", test_subset_proof_hidden_crossrow, NULL, NULL },
    { "opt/subset_proof_malformed_and_cycles", test_subset_proof_malformed_and_cycles, NULL, NULL },
    { "opt/subset_proof_budgets", test_subset_proof_budgets, NULL, NULL },
    { "opt/filter_reorder_type", test_filter_reorder_by_type, NULL, NULL },
    { "opt/filter_and_split", test_filter_and_split, NULL, NULL },
    { "opt/filter_reorder_dag", test_filter_reorder_dag, NULL, NULL },
    { "opt/filter_reorder_sel", test_filter_reorder_selectivity, NULL, NULL },
    { "opt/pushdown_select", test_pushdown_past_select, NULL, NULL },
    { "opt/pushdown_group", test_pushdown_past_group, NULL, NULL },
    { "opt/projection_pushdown", test_projection_pushdown, NULL, NULL },
    { "opt/partition_pruning", test_partition_pruning_smoke, NULL, NULL },
    { "opt/partition_pruning_mask", test_partition_pruning_mask, NULL, NULL },
    { "opt/partition_pruning_in", test_partition_pruning_in, NULL, NULL },
    { "opt/partition_pruning_not_in", test_partition_pruning_not_in, NULL, NULL },
    { "opt/partition_pruning_in_type_mismatch", test_partition_pruning_in_type_mismatch, NULL, NULL },
    { "opt/window_dce", test_opt_window_dce, NULL, NULL },
    { "opt/pushdown_past_expand", test_opt_pushdown_past_expand, NULL, NULL },
    { "opt/pushdown_expand_blocked", test_opt_pushdown_expand_blocked, NULL, NULL },
    { "opt/realloc_during_split", test_opt_realloc_during_split, NULL, NULL },
    { "opt/const_fold_f64_arith", test_const_fold_f64_arith, NULL, NULL },
    { "opt/const_fold_f64_sub", test_const_fold_f64_sub, NULL, NULL },
    { "opt/const_fold_f64_mul", test_const_fold_f64_mul, NULL, NULL },
    { "opt/const_fold_f64_div", test_const_fold_f64_div, NULL, NULL },
    { "opt/const_fold_f64_mod", test_const_fold_f64_mod, NULL, NULL },
    { "opt/const_fold_f64_min", test_const_fold_f64_min, NULL, NULL },
    { "opt/const_fold_f64_max", test_const_fold_f64_max, NULL, NULL },
    { "opt/const_fold_i64_div_min_max", test_const_fold_i64_div_min_max, NULL, NULL },
    { "opt/const_fold_i64_div", test_const_fold_i64_div, NULL, NULL },
    { "opt/const_fold_i64_min", test_const_fold_i64_min, NULL, NULL },
    { "opt/const_fold_i64_max", test_const_fold_i64_max, NULL, NULL },
    { "opt/const_fold_i64_min_swap", test_const_fold_i64_min_swap, NULL, NULL },
    { "opt/const_fold_i64_max_swap", test_const_fold_i64_max_swap, NULL, NULL },
    { "opt/const_fold_i32_ops", test_const_fold_i32_ops, NULL, NULL },
    { "opt/const_fold_i32_add", test_const_fold_i32_add, NULL, NULL },
    { "opt/const_fold_i32_div", test_const_fold_i32_div, NULL, NULL },
    { "opt/const_fold_i32_mod", test_const_fold_i32_mod, NULL, NULL },
    { "opt/const_fold_i32_min", test_const_fold_i32_min, NULL, NULL },
    { "opt/const_fold_i32_max", test_const_fold_i32_max, NULL, NULL },
    { "opt/const_fold_i32_min_swap", test_const_fold_i32_min_swap, NULL, NULL },
    { "opt/const_fold_i32_max_swap", test_const_fold_i32_max_swap, NULL, NULL },
    { "opt/const_fold_i16_atom", test_const_fold_i16_atom, NULL, NULL },
    { "opt/partition_pruning_eq", test_partition_pruning_eq, NULL, NULL },
    { "opt/partition_pruning_ne", test_partition_pruning_ne, NULL, NULL },
    { "opt/partition_pruning_lt", test_partition_pruning_lt, NULL, NULL },
    { "opt/partition_pruning_le", test_partition_pruning_le, NULL, NULL },
    { "opt/partition_pruning_and_merge", test_partition_pruning_and_merge, NULL, NULL },
    { "opt/type_promote_f64_i64", test_type_promote_f64_i64, NULL, NULL },
    { "opt/type_infer_cmp_bool", test_type_infer_cmp_bool, NULL, NULL },
    { "opt/filter_cost_i16_type", test_filter_cost_i16_type, NULL, NULL },
    { "opt/partition_pruning_i32_keys", test_partition_pruning_i32_keys, NULL, NULL },
    { "opt/factorize_expand_group_src", test_factorize_expand_group_src, NULL, NULL },
    { "opt/filter_const_false_pred", test_filter_const_false_pred, NULL, NULL },
    { "opt/filter_const_f64_zero_pred", test_filter_const_f64_zero_pred, NULL, NULL },
    { "opt/idiom_first_last_asc_scan_no_nulls", test_idiom_first_last_asc_scan_no_nulls, NULL, NULL },
    { "opt/idiom_first_asc_scan_with_nulls_stays_safe", test_idiom_first_asc_scan_with_nulls_stays_safe, NULL, NULL },
    { "opt/partition_pruning_gt", test_partition_pruning_gt, NULL, NULL },
    { "opt/partition_pruning_swapped", test_partition_pruning_swapped, NULL, NULL },
    { "opt/partition_pruning_in_large", test_partition_pruning_in_large, NULL, NULL },
    { "opt/partition_pruning_in_empty", test_partition_pruning_in_empty, NULL, NULL },
    { "opt/partition_pruning_not_in_empty", test_partition_pruning_not_in_empty, NULL, NULL },
    { "opt/partition_pruning_in_with_nulls", test_partition_pruning_in_with_nulls, NULL, NULL },
    { "opt/pushdown_expand_if_pred", test_opt_pushdown_expand_if_pred, NULL, NULL },
    { "opt/factorize_expand_non_group", test_factorize_expand_non_group, NULL, NULL },
    { "opt/factorize_expand_group_non_src", test_factorize_expand_group_non_src, NULL, NULL },
    { "opt/realloc_with_join_ext", test_opt_realloc_with_join_ext, NULL, NULL },
    { "opt/type_infer_unary", test_type_infer_unary, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

