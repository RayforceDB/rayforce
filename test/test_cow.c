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
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include "vec/str.h"
#include "lang/eval.h"
#include "core/runtime.h"

/* ---- Setup / Teardown -------------------------------------------------- */

static void cow_setup(void) {
    ray_heap_init();
}

static void cow_teardown(void) {
    ray_heap_destroy();
}

/* ---- retain/release basic ---------------------------------------------- */

static test_result_t test_retain_release(void) {
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* rc starts at 1 */
    TEST_ASSERT_EQ_U(v->rc, 1);

    /* retain -> rc=2 */
    ray_retain(v);
    TEST_ASSERT_EQ_U(v->rc, 2);

    /* release -> rc=1 */
    ray_release(v);
    TEST_ASSERT_EQ_U(v->rc, 1);

    /* release -> rc=0, block freed (don't access v after this) */
    ray_release(v);

    PASS();
}

/* ---- cow sole owner ---------------------------------------------------- */

static test_result_t test_cow_sole_owner(void) {
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    v->type = -RAY_I64;
    v->i64 = 42;

    /* rc=1, sole owner -> cow returns same pointer */
    ray_t* w = ray_cow(v);
    TEST_ASSERT_EQ_PTR(v, w);
    TEST_ASSERT_EQ_U(w->rc, 1);

    ray_release(w);
    PASS();
}

/* ---- cow shared -------------------------------------------------------- */

static test_result_t test_cow_shared(void) {
    ray_t* v = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(v);
    v->type = -RAY_I64;
    v->i64 = 99;

    /* retain to rc=2 (shared) */
    ray_retain(v);
    TEST_ASSERT_EQ_U(v->rc, 2);

    /* cow on shared object -> returns different pointer */
    ray_t* w = ray_cow(v);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(w));
    TEST_ASSERT_TRUE((void*)w != (void*)v);

    /* Copy should have rc=1 */
    TEST_ASSERT_EQ_U(w->rc, 1);

    /* Original should have rc=1 (cow decremented from 2 to 1) */
    TEST_ASSERT_EQ_U(v->rc, 1);

    /* Value should be preserved */
    TEST_ASSERT_EQ_I(w->type, -RAY_I64);
    TEST_ASSERT_EQ_I(w->i64, 99);

    ray_release(v);
    ray_release(w);
    PASS();
}

/* ---- null/error safety ------------------------------------------------- */

static test_result_t test_null_error_safety(void) {
    /* These should not crash */
    ray_retain(NULL);
    ray_release(NULL);
    ray_t* r = ray_cow(NULL);
    TEST_ASSERT_NULL(r);

    /* Error objects (new model: ray_error returns a real ray_t with type RAY_ERROR) */
    ray_t* err = ray_error("oom", NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err));
    ray_retain(err);
    ray_release(err);
    ray_t* r2 = ray_cow(err);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    ray_release(err);

    PASS();
}

/* ---- cow with vector data ---------------------------------------------- */

static test_result_t test_cow_vector(void) {
    /* Create a vector with actual data */
    size_t data_size = 10 * sizeof(int64_t);
    ray_t* v = ray_alloc(data_size);
    TEST_ASSERT_NOT_NULL(v);
    v->type = RAY_I64;
    v->len = 10;
    int64_t* data = (int64_t*)ray_data(v);
    for (int i = 0; i < 10; i++) {
        data[i] = (int64_t)(i * 100);
    }

    /* Share and cow */
    ray_retain(v);
    ray_t* w = ray_cow(v);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_TRUE((void*)w != (void*)v);
    TEST_ASSERT_EQ_I(w->type, RAY_I64);
    TEST_ASSERT_EQ_I(w->len, 10);

    /* Verify data was copied */
    int64_t* wdata = (int64_t*)ray_data(w);
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQ_I(wdata[i], (int64_t)(i * 100));
    }

    /* Modifying copy should not affect original */
    wdata[0] = 999;
    TEST_ASSERT_EQ_I(data[0], 0);

    ray_release(v);
    ray_release(w);
    PASS();
}

/* ---- block_copy retains children --------------------------------------- */

extern ray_t* ray_block_copy(ray_t* src);

static test_result_t test_block_copy_retains_children(void) {
    (void)ray_sym_init();

    int64_t vals[] = {1, 2, 3};
    ray_t* vec = ray_vec_from_raw(RAY_I64, vals, 3);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    /* Tables now hold a 2-pointer block [schema, cols_list]; retaining the
     * table retains both slots.  Probe the cols list's rc rather than an
     * individual column's, since cols inside the list are owned by the list
     * (the table block doesn't reach through to bump them on copy). */
    ray_t** slots = (ray_t**)ray_data(tbl);
    ray_t* cols_list = slots[1];
    uint32_t rc_before = cols_list->rc;

    /* Copy the table block */
    ray_t* copy = ray_block_copy(tbl);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_FALSE(RAY_IS_ERR(copy));

    /* cols-list ref count should have increased by 1 */
    uint32_t rc_after = cols_list->rc;
    TEST_ASSERT_EQ_U(rc_after, rc_before + 1);

    ray_release(copy);
    ray_release(tbl);
    ray_sym_destroy();

    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

#ifdef DEBUG
/* ---- a failed growing write leaves a shared input as it was (#1857) ---- */

/* A full vector (its next append must grow it): I64 0, 1, 2, ... */
static ray_t* cow_full_i64(void) {
    ray_t* v = ray_vec_new(RAY_I64, 1);
    if (!v || RAY_IS_ERR(v)) return NULL;
    int64_t x = 0;
    while (!v->len || (size_t)v->len * sizeof(int64_t) < ray_block_data_bytes(v)) {
        ray_t* nv = ray_vec_append(v, &x);
        if (!nv || RAY_IS_ERR(nv)) return NULL;
        v = nv; x++;
    }
    return v;
}
static ray_t* cow_full_list(void) {
    ray_t* l = ray_list_new(1);
    if (!l || RAY_IS_ERR(l)) return NULL;
    while (!l->len || (size_t)l->len * sizeof(ray_t*) < ray_block_data_bytes(l)) {
        ray_t* a = ray_i64(l->len);
        ray_t* nl = ray_list_append(l, a);
        ray_release(a);
        if (!nl || RAY_IS_ERR(nl)) return NULL;
        l = nl;
    }
    return l;
}
static ray_t* cow_full_str(void) {
    ray_t* v = ray_vec_new(RAY_STR, 1);
    if (!v || RAY_IS_ERR(v)) return NULL;
    while (!v->len || (size_t)v->len * sizeof(ray_str_t) < ray_block_data_bytes(v)) {
        ray_t* nv = ray_str_vec_append(v, "a string longer than inline", 27);
        if (!nv || RAY_IS_ERR(nv)) return NULL;
        v = nv;
    }
    return v;
}

typedef ray_t* (*cow_op_t)(ray_t*);
static ray_t* op_vec_append(ray_t* v)     { int64_t x = 99; return ray_vec_append(v, &x); }
static ray_t* op_vec_append_raw(ray_t* v) { int64_t x[3] = {7, 8, 9}; return ray_vec_append_raw(v, x, 3); }
static ray_t* op_vec_insert_at(ray_t* v)  { int64_t x = 99; return ray_vec_insert_at(v, 1, &x); }
static ray_t* op_str_append(ray_t* v)     { return ray_str_vec_append(v, "another long string here", 24); }
static ray_t* op_str_set(ray_t* v)        { return ray_str_vec_set(v, 0, "a replacement longer than inline", 32); }
static ray_t* op_list_append(ray_t* l)    { return ray_list_append(l, RAY_NULL_OBJ); }
static ray_t* op_list_insert_at(ray_t* l) { return ray_list_insert_at(l, 1, RAY_NULL_OBJ); }

/* The nth allocation of `op` on a shared, full input fails, for every n it
 * reaches.  A failure answers an error and leaves the input with both its
 * references, its length and its cells; a success answers a new block.
 * Returns the count of failures after the copy was made, or -1. */
static int cow_sweep(ray_t* (*make)(void), cow_op_t op) {
    int after_copy = 0;
    for (int64_t n = 1; n <= 16; n++) {
        ray_t* v = make();
        if (!v) return -1;
        ray_retain(v);                         /* a second owner: shared */
        int64_t len0 = v->len;
        size_t bytes = (size_t)len0 * (v->type == RAY_LIST ? sizeof(ray_t*) : v->type == RAY_STR ? sizeof(ray_str_t) : 8);
        void* snap = malloc(bytes ? bytes : 1);
        memcpy(snap, ray_data(v), bytes);
        ray_alloc_fail_set_for_test(n, 1);
        ray_t* r = op(v);
        bool fired = ray_alloc_fail_armed_for_test() == 0;
        ray_alloc_fail_set_for_test(0, 0);
        if (r && RAY_IS_ERR(r)) {
            bool ok = v->rc == 2 && v->len == len0 && memcmp(snap, ray_data(v), bytes) == 0;
            ray_error_free(r);
            free(snap);
            if (!ok) { fprintf(stderr, "  allocation %lld fails: the input has rc %u, len %lld (was 2, %lld)\n", (long long)n, (unsigned)v->rc, (long long)v->len, (long long)len0); return -1; }
            if (n > 1) after_copy++;
            ray_release(v); ray_release(v);    /* both owners */
        } else {
            free(snap);
            if (!r || r == v || v->rc != 1) { fprintf(stderr, "  allocation %lld: a success left rc %u (same block %d, fired %d, len %lld cap %zu)\n", (long long)n, (unsigned)v->rc, r == v, fired, (long long)v->len, ray_block_data_bytes(v)); return -1; }
            ray_release(r); ray_release(v);    /* the answer, and the other owner */
        }
        if (!fired) break;
    }
    return after_copy;
}

static test_result_t test_cow_failed_write_keeps_input(void) {
    struct { const char* name; ray_t* (*make)(void); cow_op_t op; } cases[] = {
        { "ray_vec_append",      cow_full_i64,  op_vec_append },
        { "ray_vec_append_raw",  cow_full_i64,  op_vec_append_raw },
        { "ray_vec_insert_at",   cow_full_i64,  op_vec_insert_at },
        { "ray_str_vec_append",  cow_full_str,  op_str_append },
        { "ray_str_vec_set",     cow_full_str,  op_str_set },
        { "ray_list_append",     cow_full_list, op_list_append },
        { "ray_list_insert_at",  cow_full_list, op_list_insert_at },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        int after = cow_sweep(cases[i].make, cases[i].op);
        TEST_ASSERT_FMT(after >= 0, "%s: a failure did not leave the shared input as it was", cases[i].name);
        TEST_ASSERT_FMT(after > 0, "%s: no failure after the copy", cases[i].name);
    }
    PASS();
}

/* #1857's repro: a select whose `format` output is added to a copy of the
 * table under an allocation failure.  A failed append to the shared schema
 * dropped the source table's reference, and the next select read the freed
 * schema (a SIGSEGV under ASan).  Every run answers the answer or `oom`, and
 * the table stays readable. */
static void cow_rt_setup(void) { ray_runtime_create(0, NULL); }
static void cow_rt_teardown(void) { ray_runtime_destroy(__RUNTIME); }

static test_result_t test_cow_table_add_col_fail(void) {
    ray_t* t = ray_eval_str("(set TB57 (table [k s v f] (list (% (til 40) 4) (at ['a 'b 'c] (% (til 40) 3)) (til 40) (as 'F64 (% (til 40) 5)))))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "setup");
    ray_release(t);
    const char* q = "(select {r: (format \"% %\" s v) from: TB57})";
    int ooms = 0;
    for (int64_t n = 1; n <= 4000; n++) {
        ray_alloc_fail_set_for_test(n, 1);
        ray_t* r = ray_eval_str(q);
        bool fired = ray_alloc_fail_armed_for_test() == 0;
        ray_alloc_fail_set_for_test(0, 0);
        if (r && RAY_IS_ERR(r)) {
            TEST_ASSERT_FMT(strcmp(ray_err_code(r), "oom") == 0, "allocation %lld fails: %s, not oom", (long long)n, ray_err_code(r));
            ray_error_free(r);
            ooms++;
        } else if (r) ray_release(r);
        ray_heap_gc();   /* a block freed too early goes back: the read below faults */
        ray_t* c = ray_eval_str("(count (select {s: s from: TB57}))");   /* the table is still whole */
        TEST_ASSERT_FMT(c && !RAY_IS_ERR(c) && c->i64 == 40, "allocation %lld fails: the table is not whole after", (long long)n);
        ray_release(c);
        if (!fired) break;
    }
    TEST_ASSERT_TRUE(ooms > 0);
    ray_t* z = ray_eval_str("(set TB57 0)");
    if (z && !RAY_IS_ERR(z)) ray_release(z);
    PASS();
}

/* assert-fact retains each column of the facts table and appends to it (a
 * copy, as the table shares it).  A failed append leaves the column with
 * that retain (#1857), which assert-fact must drop: a reference kept would
 * keep the table's columns alive after the table goes. */
static test_result_t test_cow_assert_fact_fail(void) {
    /* symbols interned once, before the trace: they are kept for good */
    ray_t* w = ray_eval_str("(assert-fact (assert-fact (datoms) 9 'age 1) 8 'age 2)");
    if (w && !RAY_IS_ERR(w)) ray_release(w); else ray_error_free(w);
    w = ray_eval_str("(set Db9 0)");   /* bound before: rebinding it frees the atom it held */
    if (w && !RAY_IS_ERR(w)) ray_release(w);
    ray_clear_error_trace();
    ray_heap_gc();
    TEST_ASSERT_TRUE(ray_mem_trace_begin());
    ray_t* t = ray_eval_str("(set Db9 (assert-fact (assert-fact (datoms) 1 'age 30) 2 'age 40))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "setup");
    ray_release(t);
    int ooms = 0;
    for (int64_t n = 1; n <= 2000; n++) {
        ray_alloc_fail_set_for_test(n, 1);
        ray_t* r = ray_eval_str("(assert-fact Db9 3 'age 50)");
        bool fired = ray_alloc_fail_armed_for_test() == 0;
        ray_alloc_fail_set_for_test(0, 0);
        if (r && RAY_IS_ERR(r)) { ray_error_free(r); ooms++; }
        else if (r) ray_release(r);
        ray_clear_error_trace();
        if (!fired) break;
    }
    ray_t* z = ray_eval_str("(set Db9 0)");
    if (z && !RAY_IS_ERR(z)) ray_release(z);
    ray_clear_error_trace();
    ray_heap_gc();
    ray_mem_trace_t tr;
    ray_mem_trace_end(&tr);
    TEST_ASSERT_TRUE(ooms > 0);
    TEST_ASSERT_FMT(tr.net_bytes == 0, "the facts table's columns outlive it: %lld bytes kept", (long long)tr.net_bytes);
    PASS();
}

#include "lang/parse.h"
/* One query under an allocation failure, for every n it reaches: of the
 * parsed tree, or of the text (parse included) when `text`.  Each run
 * answers the unfailed answer or `oom`, keeping nothing.  The oom count,
 * or -1 (reported). */
static int take_sweep_one(const char* q, bool text) {
    ray_t* ref = ray_eval_str(q);
    if (!ref || RAY_IS_ERR(ref)) { fprintf(stderr, "  %s: no reference\n", q); return -1; }
    ray_t* rf = ray_fmt(ref, 1);
    ray_release(ref);
    ray_t* parsed = ray_parse(q);
    if (!rf || RAY_IS_ERR(rf) || !parsed || RAY_IS_ERR(parsed)) return -1;
    int ooms = 0;
    for (int64_t n = 1; n <= 20000; n++) {
        ray_heap_gc();
        bool traced = ray_mem_trace_begin();
        ray_clear_error_trace();
        ray_raise_clear();
        ray_alloc_fail_set_for_test(n, 1);
        ray_t* got = text ? ray_eval_str(q) : ray_eval(parsed);
        if (!text) ray_eval_request_end();
        if (got && !RAY_IS_ERR(got) && ray_is_lazy(got)) got = ray_lazy_materialize(got);
        bool fired = ray_alloc_fail_armed_for_test() == 0;
        ray_alloc_fail_set_for_test(0, 0);
        ray_clear_error_trace();
        bool ok;
        if (got && RAY_IS_ERR(got)) {
            ok = strcmp(ray_err_code(got), "oom") == 0;
            if (!ok) fprintf(stderr, "  %s, allocation %lld fails: %s, not oom\n", q, (long long)n, ray_err_code(got));
            ray_error_free(got);
            ooms++;
        } else {
            ray_t* gf = got ? ray_fmt(got, 1) : NULL;
            ok = gf && !RAY_IS_ERR(gf) && ray_str_len(gf) == ray_str_len(rf) &&
                 memcmp(ray_str_ptr(gf), ray_str_ptr(rf), ray_str_len(rf)) == 0;
            if (!ok) fprintf(stderr, "  %s, allocation %lld fails: an answer, not the reference\n", q, (long long)n);
            if (gf && RAY_IS_ERR(gf)) ray_error_free(gf); else if (gf) ray_release(gf);
            if (got) ray_release(got);
        }
        ray_heap_gc();
        ray_mem_trace_t tr = {0};
        if (traced) ray_mem_trace_end(&tr);
        if (ok && tr.net_bytes != 0) {
            fprintf(stderr, "  %s, allocation %lld fails: %lld bytes kept\n", q, (long long)n, (long long)tr.net_bytes);
            ok = false;
        }
        if (!ok) { ray_release(rf); ray_release(parsed); return -1; }
        if (!fired) break;
    }
    ray_release(rf);
    ray_release(parsed);
    return ooms;
}

/* The consuming `_take` writes in the overwrite form (#1864), a site of
 * each file that has one and that a query reaches: on a failure the input
 * is released, never kept, and the answer is the answer or `oom`. */
static test_result_t test_cow_take_sites_fail(void) {
    static const char* const setup[] = {
        "(set T9 (table [k v] (list (% (til 20) 4) (til 20))))",
        "(set Db9 (assert-fact (assert-fact (datoms) 1 'age 30) 2 'age 40))",
        "(set E9 (table [src dst] (list [0 1 2] [1 2 0])))",
        "(set V9 (list [1.0 0.0] [0.0 1.0] [1.0 1.0] [0.5 0.5]))",
    };
    for (size_t i = 0; i < sizeof setup / sizeof *setup; i++) {
        ray_t* r = ray_eval_str(setup[i]);
        TEST_ASSERT(r && !RAY_IS_ERR(r), "setup");
        ray_release(r);
    }
    static const struct { const char* q; bool text; } qs[] = {
        { "(count (.sys.querylog.stats))", false },                                   /* core/qlog.c */
        { "(.json.parse \"[1,2,null,3]\")", false },                                    /* io/json.c */
        { "(.json.parse \"{\\\"a\\\":1,\\\"b\\\":[1.5,2],\\\"c\\\":[\\\"x\\\",null]}\")", false },
        { "((fn [x] (+ x 1)) 2)", true },                                             /* lang/compile.c */
        { "(table [a b] (list 1 2))", false },                                        /* lang/eval.c */
        { "{a: 1 b: [1 2]}", true },                                                  /* lang/parse.c */
        { "{a: 1 b: 2 c: 3 d: 4 e: 5 f: 6 g: 7 h: 8 i: 9 j: 10 k: 11 l: 12 m: 13 n: 14 o: 15 p: 16 q: 17 r: 18 s: 19 t: 20}", true },
        { "(group [1 2 1])", false },                                                 /* ops/builtins.c */
        { "(as 'STR [1 2])", false },
        { "(dict [1 2] [3 4])", false },
        { "(not (list 1 0))", false },                                                /* ops/cmp.c */
        { "(== {a: 1} {a: 1})", false },
        { "(cross [1 2] [3 4])", false },                                             /* ops/collection.c */
        { "(reverse (list 1 \"a\"))", false },
        { "(at {a: 1 b: 2} ['a 'b])", false },
        { "(pull Db9 1)", false },                                                    /* ops/datalog.c */
        { "(select {a: 1 b: v from: T9})", false },                                   /* ops/exec.c */
        { "(if [true false] [1 2] [3 4])", false },                                   /* ops/pivot.c */
        { "(substr [\"hello\" \"world\"] 1 3)", false },                              /* ops/string.c */
        { "(split \"a,b,c\" \",\")", false },                                         /* ops/strop.c */
        { "(meta T9)", false },                                                       /* ops/system.c */
        { "(unify T9 T9)", false },                                                   /* ops/tblop.c */
        { "(select {s: (sum v) from: T9 by: k})", false },                            /* ops/query.c */
        { "(insert T9 (list 1 2))", false },
        { "(update {v: (+ v 1) from: T9 by: k})", false },
        { "(.idx.uses T9)", false },                                                  /* ops/idxop.c */
        { "(insert (list 1 \"a\" 2 3 4 5 6 7) 9)", false },                              /* ops/query.c, grows */
        { "(insert (list \"a\" \"b\" \"c\" \"d\" \"e\" \"f\" \"g\") [\"x\" \"y\"])", false },
        { "(insert [1 2 3 4 5 6 7] 8)", false },
        { "(insert [\"a\" \"b\" \"c\" \"d\" \"e\" \"f\" \"g\"] [\"x\" \"y\"])", false },
        { "(pivot (table [s c q] (list (til 40) (% (til 40) 2) (til 40))) 's 'c 'q sum)", false },   /* ops/tblop.c, grows */
        { "(flip (list [1 2] [3 4]))", false },                                       /* ops/linalg.c */
        { "(.repl.signature 'min)", false },                                          /* lang/inspect.c */
        { "((fn [x] (do (let y 2) ((fn [z] (+ z y)) x))) 1)", true },                 /* lang/env.c */
        { "(.graph.info (.graph.build E9 'src 'dst))", false },                       /* ops/graph_builtin.c */
        { "(.mc.stats)", false },                                                     /* core/mcast.c */
        { "(count (.emb.hnsw.info (.emb.hnsw.build V9 'l2 8 100)))", false },         /* ops/embedding.c */
    };
    for (size_t i = 0; i < sizeof qs / sizeof *qs; i++)
        TEST_ASSERT_FMT(take_sweep_one(qs[i].q, qs[i].text) >= 0, "%s: an allocation failure answered other than the answer or oom keeping nothing", qs[i].q);
    /* The writes and reads of stored data, on a fixture of this test's own:
     * a journal, a splayed table with a list column (store/col.c) written
     * (store/splay.c) and read back. */
    const char* dir = ray_test_tmp("/tmp/rf_cow_take9");
    (void)ray_test_rm_rf(dir);
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    char path[512], q[1024];
    snprintf(path, sizeof path, "%s/j.log", dir);
    FILE* f = fopen(path, "wb");
    TEST_ASSERT(f != NULL, "fixture");
    fclose(f);
    snprintf(q, sizeof q, "(.log.validate \"%s\")", path);                           /* ops/journal.c */
    TEST_ASSERT_FMT(take_sweep_one(q, false) >= 0, "%s", q);
    snprintf(q, sizeof q, "(.db.splayed.set \"%s/s/\" (table [a b] (list (list [1 2] [3]) (list \"p\" \"q\"))))", dir);
    ray_t* r = ray_eval_str(q);
    TEST_ASSERT(r && !RAY_IS_ERR(r), "fixture");
    ray_release(r);
    snprintf(q, sizeof q, "(.db.splayed.get \"%s/s/\")", dir);                         /* store/col.c */
    TEST_ASSERT_FMT(take_sweep_one(q, false) >= 0, "%s", q);
    snprintf(q, sizeof q, "(count (.db.splayed.set \"%s/w/\" T9))", dir);               /* store/splay.c */
    TEST_ASSERT_FMT(take_sweep_one(q, false) >= 0, "%s", q);
    (void)ray_test_rm_rf(dir);
    ray_t* z = ray_eval_str("(do (set T9 0) (set Db9 0) (set E9 0) (set V9 0))");
    if (z && !RAY_IS_ERR(z)) ray_release(z);
    PASS();
}

/* The consuming forms themselves (#1864), on a full input so the write
 * must grow: a failure releases the caller's reference and answers an
 * error.  A shared input keeps its other owner's reference and cells; a
 * sole one is freed, nothing kept.  A success answers the grown block. */
static ray_t* op_vec_append_take(ray_t* v)     { int64_t x = 99; return ray_vec_append_take(v, &x); }
static ray_t* op_vec_append_raw_take(ray_t* v) { int64_t x[3] = {7, 8, 9}; return ray_vec_append_raw_take(v, x, 3); }
static ray_t* op_vec_insert_at_take(ray_t* v)  { int64_t x = 99; return ray_vec_insert_at_take(v, 1, &x); }
static ray_t* op_str_append_take(ray_t* v)     { return ray_str_vec_append_take(v, "another long string here", 24); }
static ray_t* op_str_set_take(ray_t* v)        { return ray_str_vec_set_take(v, 0, "a replacement longer than inline", 32); }
static ray_t* op_list_append_take(ray_t* l)    { return ray_list_append_take(l, RAY_NULL_OBJ); }
static ray_t* op_list_insert_at_take(ray_t* l) { return ray_list_insert_at_take(l, 1, RAY_NULL_OBJ); }

static test_result_t test_cow_take_forms_fail(void) {
    struct { const char* name; ray_t* (*make)(void); cow_op_t op; } cases[] = {
        { "ray_vec_append_take",      cow_full_i64,  op_vec_append_take },
        { "ray_vec_append_raw_take",  cow_full_i64,  op_vec_append_raw_take },
        { "ray_vec_insert_at_take",   cow_full_i64,  op_vec_insert_at_take },
        { "ray_str_vec_append_take",  cow_full_str,  op_str_append_take },
        { "ray_str_vec_set_take",     cow_full_str,  op_str_set_take },
        { "ray_list_append_take",     cow_full_list, op_list_append_take },
        { "ray_list_insert_at_take",  cow_full_list, op_list_insert_at_take },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        for (int shared = 0; shared <= 1; shared++) {
            int fails = 0;
            for (int64_t n = 1; n <= 16; n++) {
                ray_heap_gc();
                bool traced = ray_mem_trace_begin();
                ray_t* v = cases[i].make();
                TEST_ASSERT(v != NULL, "fixture");
                if (shared) ray_retain(v);
                int64_t len0 = v->len;
                ray_alloc_fail_set_for_test(n, 1);
                ray_t* r = cases[i].op(v);
                bool fired = ray_alloc_fail_armed_for_test() == 0;
                ray_alloc_fail_set_for_test(0, 0);
                TEST_ASSERT_FMT(r != NULL, "%s: C NULL", cases[i].name);
                if (RAY_IS_ERR(r)) {
                    fails++;
                    ray_error_free(r);
                    if (shared) {
                        TEST_ASSERT_FMT(v->rc == 1 && v->len == len0, "%s, allocation %lld fails: the other owner's input has rc %u, len %lld", cases[i].name, (long long)n, (unsigned)v->rc, (long long)v->len);
                        ray_release(v);
                    }
                } else {
                    TEST_ASSERT_FMT(r != v || !shared, "%s: a shared input written in place", cases[i].name);
                    ray_release(r);
                    if (shared) ray_release(v);
                }
                ray_heap_gc();
                ray_mem_trace_t tr = {0};
                if (traced) ray_mem_trace_end(&tr);
                TEST_ASSERT_FMT(tr.net_bytes == 0, "%s (%s), allocation %lld: %lld bytes kept", cases[i].name, shared ? "shared" : "sole", (long long)n, (long long)tr.net_bytes);
                if (!fired) break;
            }
            /* a sole input's set writes in place: no allocation to fail */
            if (shared || cases[i].op != op_str_set_take)
                TEST_ASSERT_FMT(fails > 0, "%s (%s): no failure reached", cases[i].name, shared ? "shared" : "sole");
        }
    }
    PASS();
}

/* The in-place insert into a bound table (#1864 review): its appends write
 * through a pointer that aliases the table's column slot, and the roll-back
 * reads that slot after a failure, so a failed append must leave the column
 * where it is.  A consuming write there freed the table's column: the
 * roll-back wrote into the freed block and a later insert spun.  Each run
 * answers the insert or `oom`; after every failure the table is whole, its
 * two rows as they were, and the next insert is answered. */
static test_result_t test_cow_insert_inplace_fail(void) {
    const char* bind = "(set TZ (table [a s] (list (list 1 2) [\"u\" \"v\"])))";
    const char* qs[] = {
        "(insert 'TZ (table [a s] (list (list 1 2 3) [\"x\" \"y\" \"z\"])))",
        "(insert 'TZ (list (list 7 8) [\"p\" \"q\"]))",
    };
    for (size_t qi = 0; qi < sizeof qs / sizeof *qs; qi++) {
        int ooms = 0;
        for (int64_t n = 1; n <= 4000; n++) {
            ray_t* b = ray_eval_str(bind);
            TEST_ASSERT(b && !RAY_IS_ERR(b), "setup");
            ray_release(b);
            ray_alloc_fail_set_for_test(n, 1);
            ray_t* r = ray_eval_str(qs[qi]);
            bool fired = ray_alloc_fail_armed_for_test() == 0;
            ray_alloc_fail_set_for_test(0, 0);
            ray_clear_error_trace();
            if (r && RAY_IS_ERR(r)) {
                TEST_ASSERT_FMT(strcmp(ray_err_code(r), "oom") == 0, "%s, allocation %lld fails: %s, not oom", qs[qi], (long long)n, ray_err_code(r));
                ray_error_free(r);
                ooms++;
                ray_heap_gc();   /* a column freed too early goes back: the reads below see it */
                /* as it was, or (a failure after the rows went in, as for the
                 * answer's symbol) with every row: never a part of them */
                ray_t* f = ray_eval_str("(format \"%\" TZ)");
                ray_t* w0 = ray_eval_str("(format \"%\" (table [a s] (list (list 1 2) [\"u\" \"v\"])))");
                ray_t* w1 = ray_eval_str(qi == 0
                    ? "(format \"%\" (table [a s] (list (list 1 2 1 2 3) [\"u\" \"v\" \"x\" \"y\" \"z\"])))"
                    : "(format \"%\" (table [a s] (list (list 1 2 7 8) [\"u\" \"v\" \"p\" \"q\"])))");
                TEST_ASSERT(f && !RAY_IS_ERR(f) && w0 && !RAY_IS_ERR(w0) && w1 && !RAY_IS_ERR(w1), "format");
                bool was = ray_str_len(f) == ray_str_len(w0) && memcmp(ray_str_ptr(f), ray_str_ptr(w0), ray_str_len(w0)) == 0;
                bool all = ray_str_len(f) == ray_str_len(w1) && memcmp(ray_str_ptr(f), ray_str_ptr(w1), ray_str_len(w1)) == 0;
                TEST_ASSERT_FMT(was || all, "%s, allocation %lld fails: the table is neither as it was nor whole: %.*s",
                                qs[qi], (long long)n, (int)ray_str_len(f), ray_str_ptr(f));
                int64_t rows = was ? 2 : qi == 0 ? 5 : 4;
                ray_release(f); ray_release(w0); ray_release(w1);
                ray_t* c = ray_eval_str("(do (insert 'TZ (list (list 5 6) [\"m\" \"n\"])) (count TZ))");
                TEST_ASSERT_FMT(c && !RAY_IS_ERR(c) && c->i64 == rows + 2, "%s, allocation %lld fails: the next insert is not answered", qs[qi], (long long)n);
                ray_release(c);
            } else if (r) ray_release(r);
            if (!fired) break;
        }
        TEST_ASSERT_FMT(ooms > 0, "%s: no failure reached", qs[qi]);
    }
    ray_t* z = ray_eval_str("(set TZ 0)");
    if (z && !RAY_IS_ERR(z)) ray_release(z);
    PASS();
}
#endif

const test_entry_t cow_entries[] = {
    { "cow/retain_release", test_retain_release, cow_setup, cow_teardown },
    { "cow/cow_sole_owner", test_cow_sole_owner, cow_setup, cow_teardown },
    { "cow/cow_shared", test_cow_shared, cow_setup, cow_teardown },
    { "cow/null_error_safety", test_null_error_safety, cow_setup, cow_teardown },
    { "cow/cow_vector", test_cow_vector, cow_setup, cow_teardown },
    { "cow/block_copy_retains", test_block_copy_retains_children, cow_setup, cow_teardown },
#ifdef DEBUG
    { "cow/failed_write_keeps_input", test_cow_failed_write_keeps_input, cow_setup, cow_teardown },
    { "cow/table_add_col_fail", test_cow_table_add_col_fail, cow_rt_setup, cow_rt_teardown },
    { "cow/assert_fact_fail", test_cow_assert_fact_fail, cow_rt_setup, cow_rt_teardown },
    { "cow/take_sites_fail", test_cow_take_sites_fail, cow_rt_setup, cow_rt_teardown },
    { "cow/take_forms_fail", test_cow_take_forms_fail, cow_setup, cow_teardown },
    { "cow/insert_inplace_fail", test_cow_insert_inplace_fail, cow_rt_setup, cow_rt_teardown },
#endif
    { NULL, NULL, NULL, NULL },
};


