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


#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "ops/idxop.h"
#include "ops/colbind.h"
#include "ops/fused_pred.h"
#include "ops/fused_topk.h"
#include "ops/internal.h"
#include "ops/rowsel.h"
#include "ops/agg_engine.h"
#include "ops/fused_group.h"
#include "lang/parse.h"
#include "table/sym.h"
#include "table/table.h"
#include "core/runtime.h"
#include "lang/eval.h"
#include "core/platform.h"
#include "store/col.h"
#include "store/colrange.h"
#include "table/domain.h"
#include "store/encfmt.h"
#include "store/enccol.h"
#include "store/splay.h"
#include "store/part.h"
#include "store/serde.h"
#include "store/stream.h"
#include "vec/str.h"
#include "ipc_harness.h"
#include "ops/datalog.h"
#include "core/ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CS_TMP "/tmp/rayforce_compress"

static void cs_setup(void)    { ray_heap_init(); (void)ray_sym_init(); (void)ray_test_rm_rf(ray_test_tmp(CS_TMP)); (void)ray_test_mkdir_p(ray_test_tmp(CS_TMP)); }
static void cs_teardown(void) { ray_test_hooks_off(); (void)ray_test_rm_rf(ray_test_tmp(CS_TMP)); ray_sym_destroy(); ray_heap_destroy(); }

static ray_t* i64_col(int64_t n, int64_t mul) {
    ray_t* v = ray_vec_new(RAY_I64, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = i * mul;
    return v;
}

/* A loaded table whose column `a` is generation 2 and `b` generation 0. */
static ray_t* mixed_table(void) {
    char p[256];
    (void)ray_test_mkdir_p(ray_test_tmp(CS_TMP "/mix"));
    ray_t* a = i64_col(3000, 3); ray_t* b = i64_col(3000, 5);
    snprintf(p, sizeof p, "%s/mix/a", ray_test_tmp(CS_TMP)); ray_col_save_gen(a, p, 2);
    snprintf(p, sizeof p, "%s/mix/b", ray_test_tmp(CS_TMP)); ray_col_save_gen(b, p, 0);
    ray_release(a); ray_release(b);
    ray_t* names = ray_vec_new(RAY_STR, 2);
    names = ray_str_vec_append(names, "a", 1); names = ray_str_vec_append(names, "b", 1);
    snprintf(p, sizeof p, "%s/mix/.d", ray_test_tmp(CS_TMP)); ray_col_save(names, p); ray_release(names);
    return ray_read_splayed(ray_test_tmp(CS_TMP "/mix"), NULL);
}

static test_result_t test_meta_without_decode(void) {
    ray_t* t = mixed_table();
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_enc_counters_reset();
    ray_col_meta_t m;
    TEST_ASSERT_TRUE(ray_table_col_meta(t, ray_sym_intern("a", 1), &m));
    TEST_ASSERT_EQ_I(m.type, RAY_I64);
    TEST_ASSERT_EQ_I(m.len, 3000);
    TEST_ASSERT_TRUE(m.enc != NULL);
    TEST_ASSERT_EQ_I(m.null_count, 0);
    TEST_ASSERT_TRUE(ray_table_col_meta(t, ray_sym_intern("b", 1), &m));
    TEST_ASSERT_TRUE(m.enc == NULL);
    TEST_ASSERT_EQ_I(m.null_count, -1);
    TEST_ASSERT_FALSE(ray_table_col_meta(t, ray_sym_intern("zz", 2), &m));
    ray_t* ix = (ray_t*)1;
    TEST_ASSERT_NULL(ray_table_col_index(t, ray_sym_intern("a", 1), &ix));
    TEST_ASSERT_NULL(ix);                                   /* no index, no decode */
    TEST_ASSERT_FALSE(ray_table_col_has_nulls(t, ray_sym_intern("a", 1)));
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    ray_t* a = ray_table_col(t, ray_sym_intern("a", 1));
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a) && a->type == RAY_I64);
    TEST_ASSERT_EQ_PTR(ray_table_col_idx(t, 0), a);         /* cached */
    TEST_ASSERT_TRUE(ray_table_get_col_enc(t, ray_sym_intern("a", 1)) != NULL);
    TEST_ASSERT_NULL(ray_table_get_col_enc(t, ray_sym_intern("b", 1)));
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    ray_release(t);
    PASS();
}

/* Legacy getters: absent, non-table, success and decode failure, each after
 * a stale error in the slot; only the failure replaces its message. */
static test_result_t test_legacy_getters(void) {
    ray_t* t = mixed_table();
    ray_t* stale;
    stale = ray_error("domain", "stale"); ray_error_free(stale);
    TEST_ASSERT_NULL(ray_table_get_col(t, ray_sym_intern("zz", 2)));
    TEST_ASSERT_STR_EQ(ray_error_msg(), "stale");
    stale = ray_error("domain", "stale"); ray_error_free(stale);
    ray_t* atom = ray_i64(3);
    TEST_ASSERT_NULL(ray_table_get_col(atom, 0));
    TEST_ASSERT_STR_EQ(ray_error_msg(), "stale");
    ray_release(atom);
    stale = ray_error("domain", "stale"); ray_error_free(stale);
    TEST_ASSERT_NOT_NULL(ray_table_get_col(t, ray_sym_intern("a", 1)));
    TEST_ASSERT_STR_EQ(ray_error_msg(), "stale");
    ray_release(t);
    /* corrupt a descriptor of column a and reload */
    FILE* f = fopen(ray_test_tmp(CS_TMP "/mix/a"), "r+b"); TEST_ASSERT_TRUE(f != NULL); fseek(f, 160 + 6, SEEK_SET); fputc(1, f); fclose(f);
    t = ray_read_splayed(ray_test_tmp(CS_TMP "/mix"), NULL);
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    for (int i = 0; i < 4; i++) {
        stale = ray_error("domain", "stale"); ray_error_free(stale);
        TEST_ASSERT_NULL(ray_table_get_col(t, ray_sym_intern("a", 1)));
        TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), ray_test_tmp(CS_TMP "/mix/a")));
    }
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);   /* no leaked error objects */
    ray_t* r = ray_table_col(t, ray_sym_intern("a", 1));
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    ray_error_free(r);
    ray_release(t);
    PASS();
}

typedef struct { ray_t* t; int64_t id; ray_t* got; } cs_thr_t;
static void cs_col_thread(void* raw) {
    cs_thr_t* x = raw;
    x->got = ray_table_col(x->t, x->id);
    ray_heap_abandon();
}

static test_result_t test_concurrent_one_materialization(void) {
    ray_t* t = mixed_table();
    ray_enc_counters_reset();
    cs_thr_t th[8]; ray_thread_t id[8];
    for (int i = 0; i < 8; i++) { th[i].t = t; th[i].id = ray_sym_intern("a", 1); th[i].got = NULL;
        TEST_ASSERT_EQ_I(ray_thread_create(&id[i], cs_col_thread, &th[i]), RAY_OK); }
    for (int i = 0; i < 8; i++) ray_thread_join(id[i]);
    for (int i = 1; i < 8; i++) TEST_ASSERT_EQ_PTR(th[i].got, th[0].got);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    ray_release(t);
    PASS();
}

typedef struct { ray_t* t; int64_t id; ray_t* ix; ray_t* err; } cs_ix_thr_t;
static void cs_index_thread(void* raw) {
    cs_ix_thr_t* x = raw;
    x->err = ray_table_col_index(x->t, x->id, &x->ix);
    ray_heap_abandon();
}

/* Many threads asking one fresh UNIQUE column for its index: one
 * materialization, one root for all. */
static test_result_t test_concurrent_col_index(void) {
    (void)ray_test_mkdir_p(ray_test_tmp(CS_TMP "/ux"));
    ray_t* a = i64_col(3000, 7);
    ray_t* root = ray_index_new_root(RAY_IDX_NONE, RAY_I64, 3000);
    ray_index_payload(root)->markers = RAY_MARK_UNIQUE;
    ray_t* ua = ray_index_attach_persisted(&a, root);
    TEST_ASSERT_TRUE(ua && !RAY_IS_ERR(ua)); a = ua;
    TEST_ASSERT_EQ_I(ray_col_save_gen(a, ray_test_tmp(CS_TMP "/ux/a"), 2), RAY_OK);
    ray_release(a);
    ray_t* names = ray_vec_new(RAY_STR, 1); names = ray_str_vec_append(names, "a", 1);
    ray_col_save(names, ray_test_tmp(CS_TMP "/ux/.d")); ray_release(names);
    ray_t* t = ray_read_splayed(ray_test_tmp(CS_TMP "/ux"), NULL);
    ray_enc_counters_reset();
    cs_ix_thr_t th[8]; ray_thread_t id[8];
    for (int i = 0; i < 8; i++) { th[i] = (cs_ix_thr_t){ t, ray_sym_intern("a", 1), NULL, NULL };
        TEST_ASSERT_EQ_I(ray_thread_create(&id[i], cs_index_thread, &th[i]), RAY_OK); }
    for (int i = 0; i < 8; i++) ray_thread_join(id[i]);
    for (int i = 0; i < 8; i++) { TEST_ASSERT_NULL(th[i].err); TEST_ASSERT_EQ_PTR(th[i].ix, th[0].ix); }
    TEST_ASSERT_NOT_NULL(th[0].ix);
    TEST_ASSERT_TRUE(ray_index_payload(th[0].ix)->markers & RAY_MARK_UNIQUE);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    ray_release(t);
    PASS();
}

/* A transient failure through the legacy getter: NULL with the message in
 * the slot, nothing leaked, the next call succeeds. */
static test_result_t test_legacy_transient_no_leak(void) {
    ray_t* t = mixed_table();
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    for (int i = 0; i < 4; i++) {
        ray_enccol_test_fail_alloc(1);
        TEST_ASSERT_NULL(ray_table_get_col(t, ray_sym_intern("a", 1)));
        ray_enccol_test_fail_alloc(0);
        TEST_ASSERT_NOT_NULL(ray_error_msg());
    }
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);
    ray_error_clear();
    TEST_ASSERT_NOT_NULL(ray_table_get_col(t, ray_sym_intern("a", 1)));
    TEST_ASSERT_NULL(ray_error_msg());
    ray_release(t);
    PASS();
}

/* Serializing a table that holds an encoded column sends its values. */
static test_result_t test_serde_encoded_table(void) {
    ray_t* t = mixed_table();
    ray_t* w = ray_ser(t);
    TEST_ASSERT_TRUE(w && !RAY_IS_ERR(w));
    ray_t* back = ray_de(w);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    ray_t* a = ray_table_col(back, ray_sym_intern("a", 1));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(a))[2999], 2999 * 3);
    ray_release(back); ray_release(w); ray_release(t);
    PASS();
}

static void rt_setup(void)    { ray_runtime_create(0, NULL); (void)ray_test_rm_rf(ray_test_tmp(CS_TMP)); (void)ray_test_mkdir_p(ray_test_tmp(CS_TMP)); }
static void rt_teardown(void) { ray_test_hooks_off(); (void)ray_test_rm_rf(ray_test_tmp(CS_TMP)); ray_runtime_destroy(__RUNTIME); }

/* A table `name` (columns k, v: I64 3000 rows, k = i, v = 10 i) saved as
 * generation 2, loaded and bound to the global `t`.  Returns the retained
 * loaded table. */
static ray_t* bind_gen2_table(const char* name) {
    char dir[256], p[300];
    snprintf(dir, sizeof dir, "%s/%s", ray_test_tmp(CS_TMP), name);
    (void)ray_test_mkdir_p(dir);
    ray_t* k = i64_col(3000, 1); ray_t* v = i64_col(3000, 10);
    snprintf(p, sizeof p, "%s/k", dir); if (ray_col_save_gen(k, p, 2) != RAY_OK) return NULL;
    snprintf(p, sizeof p, "%s/v", dir); if (ray_col_save_gen(v, p, 2) != RAY_OK) return NULL;
    ray_release(k); ray_release(v);
    ray_t* names = ray_vec_new(RAY_STR, 2);
    names = ray_str_vec_append(names, "k", 1); names = ray_str_vec_append(names, "v", 1);
    snprintf(p, sizeof p, "%s/.d", dir);
    ray_err_t de = ray_col_save(names, p); ray_release(names);
    if (de != RAY_OK) return NULL;
    ray_t* t = ray_read_splayed(dir, NULL);
    if (!t || RAY_IS_ERR(t)) return NULL;
    ray_env_set(ray_sym_intern("t", 1), t);
    return t;
}

static int64_t cell(ray_t* tbl, const char* col, int64_t row) {
    ray_t* c = ray_table_col(tbl, ray_sym_intern(col, (int64_t)strlen(col)));
    return c && !RAY_IS_ERR(c) ? ((int64_t*)ray_data(c))[row] : INT64_MIN;
}

/* The in-place insert, upsert, update and delete arms write a table's slots
 * as plain vectors: a table loaded with encoded columns must take the copying
 * arms.  Each verb gets its own freshly loaded table; the retained original
 * must keep its values. */
static test_result_t dml_case(const char* name, const char* src, int64_t nrows_after,
                              const char* col, int64_t row, int64_t want, bool keep) {
    ray_t* orig = bind_gen2_table(name);
    TEST_ASSERT_TRUE(orig != NULL);
    /* keep: a second reference forces the copying arms; without it the
     * binding is the only owner and the in-place gates are what protect the
     * encoded slots. */
    if (!keep) { ray_release(orig); orig = NULL; }
    TEST_ASSERT_TRUE(!orig || ray_table_get_col_enc(orig, ray_sym_intern("k", 1)) != NULL);
    ray_t* r = ray_eval_str(src);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(r);
    ray_t* now = ray_env_get(ray_sym_intern("t", 1));
    TEST_ASSERT_EQ_I(ray_table_nrows(now), nrows_after);
    TEST_ASSERT_EQ_I(cell(now, col, row), want);
    if (keep) {
        TEST_ASSERT_EQ_I(ray_table_nrows(orig), 3000);          /* the retained original is intact */
        for (int64_t i = 0; i < 3000; i += 499) {
            TEST_ASSERT_EQ_I(cell(orig, "k", i), i);
            TEST_ASSERT_EQ_I(cell(orig, "v", i), i * 10);
        }
        ray_release(orig);
    }
    PASS();
}
#define DML_TESTS(NAME, SRC, NROWS, COL, ROW, WANT) \
    static test_result_t test_dml_##NAME##_copy(void)    { return dml_case(#NAME "c", SRC, NROWS, COL, ROW, WANT, true); } \
    static test_result_t test_dml_##NAME##_inplace(void) { return dml_case(#NAME "i", SRC, NROWS, COL, ROW, WANT, false); }
DML_TESTS(upsert, "(upsert 't 'k (list 5 -1))", 3000, "v", 5, -1)
DML_TESTS(insert, "(insert 't (list 9999 7))", 3001, "k", 3000, 9999)
DML_TESTS(update, "(update {v: 1 from: 't where: (== k 7)})", 3000, "v", 7, 1)
DML_TESTS(delete, "(delete {from: 't where: (== k 7)})", 2999, "k", 7, 8)

static const char* ev_code(const char* src);

/* Value operations on a table whose slots are encoded copy the column: the
 * cached materialization and other tables sharing the encoded object keep
 * the old values (spec "Table core: Mutation"). */
static test_result_t test_value_ops_copy_encoded(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['k 'v] (list [1 2 3 4] [10 20 30 40])))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/vo\" t {compress: true})")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set a (.db.splayed.get \"" CS_TMP "/vo\"))")), "");
    TEST_ASSERT_STR_EQ(ev_code("(set b a)"), "");
    ray_enc_counters_reset();
    TEST_ASSERT_STR_EQ(ev_code("(set a (update {v: (* v 2) from: a}))"), "");
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);              /* (* v 2) range-reads v */
    ray_t* r = ray_eval_str("(sum (at b 'v))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 100);
    ray_release(r);
    r = ray_eval_str("(sum (at a 'v))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 200);
    ray_release(r);
    TEST_ASSERT_STR_EQ(ev_code("(set a (insert a (list 5 50)))"), "");
    TEST_ASSERT_STR_EQ(ev_code("(set a (delete {from: a where: (== k 1)}))"), "");
    r = ray_eval_str("(count a)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 4);
    ray_release(r);
    r = ray_eval_str("(sum (at b 'v))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 100);
    ray_release(r);
    PASS();
}

static int64_t ev_i64(const char* src) {
    ray_t* r = ray_eval_str(src);
    int64_t v = (r && !RAY_IS_ERR(r) && r->type == -RAY_I64) ? r->i64 : INT64_MIN;
    if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
    return v;
}

/* Engine-built tables (a select's output) may carry the source's encoded
 * slots: update, insert, upsert and delete on such a result work, by name and
 * by value, and the generation-2 source keeps its values. */
static test_result_t test_dml_on_select_result(void) {
    ray_t* g = bind_gen2_table("sr");
    TEST_ASSERT_TRUE(g != NULL);
    ray_release(g);
    static const char* const steps[] = {
        "(set s (select {from: t}))",
        "(set s (update {v: (* v 2) from: s where: (< k 10)}))",
        "(set s (insert s (list 3000 1)))",
        "(set s (upsert s 'k (list 5 -1)))",
        "(set s (delete {from: s where: (== k 7)}))",
        "(set u (select {k: k v: v from: t}))",
        "(update {v: 0 from: 'u where: (== k 1)})",
        "(insert 'u (list 3000 1))",
        "(upsert 'u 'k (list 2 -2))",
        "(delete {from: 'u where: (== k 3)})",
    };
    for (size_t i = 0; i < sizeof steps / sizeof *steps; i++) {
        const char* code = ev_code(steps[i]);
        TEST_ASSERT_FMT(strcmp(code, "") == 0, "%s: %s", steps[i], code);
    }
    TEST_ASSERT_EQ_I(ev_i64("(count s)"), 3000);
    TEST_ASSERT_EQ_I(ev_i64("(at (at s 'v) 4)"), 80);
    TEST_ASSERT_EQ_I(ev_i64("(at (at s 'v) 5)"), -1);
    TEST_ASSERT_EQ_I(ev_i64("(at (at s 'k) 7)"), 8);
    TEST_ASSERT_EQ_I(ev_i64("(at (at s 'v) 2999)"), 1);
    TEST_ASSERT_EQ_I(ev_i64("(count u)"), 3000);
    TEST_ASSERT_EQ_I(ev_i64("(at (at u 'v) 1)"), 0);
    TEST_ASSERT_EQ_I(ev_i64("(at (at u 'v) 2)"), -2);
    TEST_ASSERT_EQ_I(ev_i64("(at (at u 'k) 3)"), 4);
    TEST_ASSERT_EQ_I(ev_i64("(sum (at t 'v))"), 10 * (2999LL * 3000 / 2));
    TEST_ASSERT_EQ_I(ev_i64("(count t)"), 3000);
    PASS();
}

/* A generation-2 table whose column `a` loads but fails to materialize
 * (corrupt descriptor, as test_legacy_getters), bound to `t`. */
static ray_t* bind_corrupt_table(void) {
    ray_t* t = mixed_table();
    if (!t || RAY_IS_ERR(t)) return NULL;
    ray_release(t);
    FILE* f = fopen(ray_test_tmp(CS_TMP "/mix/a"), "r+b");
    if (!f) return NULL;
    fseek(f, 160 + 6, SEEK_SET); fputc(1, f); fclose(f);
    t = ray_read_splayed(ray_test_tmp(CS_TMP "/mix"), NULL);
    if (t && !RAY_IS_ERR(t)) ray_env_set(ray_sym_intern("t", 1), t);
    return t;
}

static test_result_t expect_corrupt(ray_t* r, const char* what) {
    TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: expected an error", what);
    TEST_ASSERT_FMT(strcmp(ray_err_code(r), "corrupt") == 0, "%s: code %s (%s)", what, ray_err_code(r), ray_error_msg() ? ray_error_msg() : "no message");
    TEST_ASSERT_FMT(ray_error_msg() && strstr(ray_error_msg(), ray_test_tmp(CS_TMP "/mix/a")), "%s: message %s", what,
                    ray_error_msg() ? ray_error_msg() : "(none)");
    ray_error_free(r);
    PASS();
}

/* Every verb that reads columns through the legacy getters reports the
 * decode failure as itself: no NULL dereference, no "oom"/"schema" label. */
static test_result_t test_failed_decode_propagates(void) {
    ray_t* t = bind_corrupt_table();
    TEST_ASSERT_TRUE(t != NULL);
    static const char* const verbs[] = {
        "(upsert 't 'a (list 5 1))", "(upsert t 'a (list 5 1))",
        "(insert 't (list 1 2))", "(insert t (list 1 2))",
        "(update {b: 1 from: 't where: (== a 5)})", "(update {b: a from: t})",
        "(delete {from: 't where: (== b 5)})", "(delete {from: t where: (== b 5)})",
        "(select {s: (sum a) from: t})", "(select {from: t where: (> a 5)})",
    };
    for (size_t i = 0; i < sizeof verbs / sizeof *verbs; i++) {
        ray_error_clear();
        ray_t* r = ray_eval_str(verbs[i]);
        { test_result_t rr = expect_corrupt(r, verbs[i]); if (rr.status != TEST_PASS) return rr; }
    }
    /* An update that never reads the failing column does not decode it: the
     * verbs read encoded columns lazily, so it succeeds and passes `a` on. */
    static const char* const lazy[] = { "(update {b: 1 from: t where: (== b 5)})", "(update {b: 1 from: t})" };
    for (size_t i = 0; i < 2; i++) {
        ray_t* ok = ray_eval_str(lazy[i]);
        TEST_ASSERT_FMT(ok && !RAY_IS_ERR(ok) && ok->type == RAY_TABLE, "%s", lazy[i]);
        TEST_ASSERT_TRUE(ray_table_get_col_enc(ok, ray_sym_intern("a", 1)) != NULL);
        ray_release(ok);
    }
    /* a splayed re-save */
    ray_error_clear();
    TEST_ASSERT_EQ_I(ray_splay_save(t, ray_test_tmp(CS_TMP "/resave"), NULL), RAY_ERR_CORRUPT);
    TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), ray_test_tmp(CS_TMP "/mix/a")));
    /* serialization reports the same error */
    ray_error_clear();
    TEST_ASSERT_TRUE(ray_serde_size(t) < 0);
    ray_t* w = ray_ser(t);
    { test_result_t rr = expect_corrupt(w, "ray_ser"); if (rr.status != TEST_PASS) return rr; }
    ray_release(t);
    PASS();
}

/* Builtins, formatting, links, graph and rerank ops read table columns through
 * the accessors: a column that fails to materialize surfaces as its own error
 * (code and column path), never a crash or a wrong value. */
static test_result_t test_builtins_failed_decode(void) {
    ray_t* t = bind_corrupt_table();
    TEST_ASSERT_TRUE(t != NULL);
    static const char* const verbs[] = {
        "(format \"%\" t)", "(print \"%\" t)", "(value t)", "(at t 'a)", "(at t 0)", "(at t [0 1 2])",
        "(take t 2)", "(take t [1 2])", "(filter t (> (til 3000) 5))",
        "(fills t)", "(concat t t)", "(resolve t)", "(as 'DICT t)", "t.a",
        "(concat (table ['a 'b] (list (til 3000) (til 3000))) t)",
        "(.graph.build t 'b 'b 'a)", "(.graph.build t 'a 'b)",
        "(do (set lk (.col.link 't [0 1 2])) lk.a)",
    };
    for (size_t i = 0; i < sizeof verbs / sizeof *verbs; i++) {
        ray_error_clear();
        ray_t* r = ray_eval_str(verbs[i]);
        { test_result_t rr = expect_corrupt(r, verbs[i]); if (rr.status != TEST_PASS) return rr; }
    }
    /* ungroup: a nested column beside a flat column that fails to load */
    {
        ray_t* nested = ray_list_new(3000);
        for (int i = 0; i < 3000; i++) { ray_t* e = ray_i64(i); nested = ray_list_append(nested, e); ray_release(e); }
        ray_t* h = ray_table_new(2);
        h = ray_table_add_col(h, ray_sym_intern("n", 1), nested); ray_release(nested);
        ray_t* enc = ray_table_get_col_enc(t, ray_sym_intern("a", 1));
        TEST_ASSERT_TRUE(enc != NULL);
        h = ray_table_add_col(h, ray_sym_intern("a", 1), enc);
        TEST_ASSERT_TRUE(h && !RAY_IS_ERR(h));
        ray_error_clear();
        ray_t* u = ray_ungroup_fn(h);
        test_result_t rr = expect_corrupt(u, "ungroup");
        ray_release(h);
        if (rr.status != TEST_PASS) return rr;
    }
    /* the formatter: every mode, and the streamed one */
    for (int mode = 0; mode <= 2; mode++) {
        ray_error_clear();
        ray_t* f = ray_fmt(t, mode);
        { test_result_t rr = expect_corrupt(f, "ray_fmt"); if (rr.status != TEST_PASS) return rr; }
    }
    FILE* sink = tmpfile();
    TEST_ASSERT_TRUE(sink != NULL);
    ray_t* cause = NULL;
    TEST_ASSERT_TRUE(ray_fmt_write(sink, t, &cause) != RAY_OK);
    { test_result_t rr = expect_corrupt(cause, "ray_fmt_write"); if (rr.status != TEST_PASS) return rr; }
    fclose(sink);
    /* show reports the failed column as an error, freeing it (no leaked object) */
    {
        ray_error_clear();
        ray_t* w0 = ray_eval_str("(show t)");   /* warm */
        { test_result_t rr = expect_corrupt(w0, "show"); if (rr.status != TEST_PASS) return rr; }
        ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
        for (int i = 0; i < 4; i++) {
            ray_error_clear();
            ray_t* w = ray_eval_str("(show t)");
            { test_result_t rr = expect_corrupt(w, "show"); if (rr.status != TEST_PASS) return rr; }
        }
        ray_mem_stats(&m1);
        TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);
    }
    /* a healthy column of the same table still reads */
    ray_t* b = ray_eval_str("(at t 'b)");
    TEST_ASSERT_TRUE(b && !RAY_IS_ERR(b) && b->len == 3000);
    ray_release(b);
    ray_release(t);
    PASS();
}

/* A table whose columns `names` all alias the encoded column `a` of `t`. */
static ray_t* alias_enc_table(ray_t* t, const char* const* names, int n) {
    ray_t* enc = ray_table_get_col_enc(t, ray_sym_intern("a", 1));
    if (!enc) return NULL;
    ray_t* h = ray_table_new(n);
    for (int i = 0; i < n; i++)
        h = ray_table_add_col(h, ray_sym_intern(names[i], (int64_t)strlen(names[i])), enc);
    return h;
}

/* Graph traversals read the edge-weight and coordinate columns through the
 * accessors; one that fails to load is reported, not read as garbage. */
static test_result_t test_traverse_failed_decode(void) {
    ray_t* t = bind_corrupt_table();
    TEST_ASSERT_TRUE(t != NULL);
    static const char* const wn[] = { "weight" };
    static const char* const cn[] = { "lat", "lon" };
    ray_t* props = alias_enc_table(t, wn, 1);
    ray_t* np    = alias_enc_table(t, cn, 2);
    TEST_ASSERT_TRUE(props && np && !RAY_IS_ERR(props) && !RAY_IS_ERR(np));
    int64_t esrc[] = {0, 1}, edst[] = {1, 2};
    ray_t* sv = ray_vec_from_raw(RAY_I64, esrc, 2);
    ray_t* dv = ray_vec_from_raw(RAY_I64, edst, 2);
    ray_t* edges = ray_table_new(2);
    edges = ray_table_add_col(edges, ray_sym_intern("src", 3), sv); ray_release(sv);
    edges = ray_table_add_col(edges, ray_sym_intern("dst", 3), dv); ray_release(dv);
    ray_rel_t* rel = ray_rel_from_edges(edges, "src", "dst", 3, 3, false);
    TEST_ASSERT_TRUE(rel != NULL);
    ray_rel_set_props(rel, props);
    for (int which = 0; which < 4; which++) {
        ray_graph_t* g = ray_graph_new(NULL);
        ray_t* s0 = ray_i64(0); ray_t* d2 = ray_i64(2);
        ray_op_t* so = ray_const_atom(g, s0); ray_op_t* dop = ray_const_atom(g, d2);
        ray_release(s0); ray_release(d2);
        ray_op_t* op = which == 0 ? ray_dijkstra(g, so, NULL, rel, "weight", 10)
                     : which == 1 ? ray_mst(g, rel, "weight")
                     : which == 2 ? ray_astar(g, so, dop, rel, "weight", "lat", "lon", np, 10)
                                  : ray_k_shortest(g, so, dop, rel, "weight", 2);
        TEST_ASSERT_TRUE(op != NULL);
        ray_error_clear();
        ray_t* r = ray_execute(g, op);
        test_result_t rr = expect_corrupt(r, which == 0 ? "dijkstra" : which == 1 ? "mst" : which == 2 ? "astar" : "k-shortest");
        ray_graph_free(g);
        if (rr.status != TEST_PASS) return rr;
    }
    /* astar: a healthy weight column, failing coordinate columns */
    {
        ray_t* wv = ray_vec_new(RAY_F64, 2); wv->len = 2;
        ((double*)ray_data(wv))[0] = 1.0; ((double*)ray_data(wv))[1] = 1.0;
        ray_t* good = ray_table_new(1);
        good = ray_table_add_col(good, ray_sym_intern("weight", 6), wv); ray_release(wv);
        ray_rel_set_props(rel, good); ray_release(good);
        ray_graph_t* g = ray_graph_new(NULL);
        ray_t* s0 = ray_i64(0); ray_t* d2 = ray_i64(2);
        ray_op_t* so = ray_const_atom(g, s0); ray_op_t* dop = ray_const_atom(g, d2);
        ray_release(s0); ray_release(d2);
        ray_op_t* op = ray_astar(g, so, dop, rel, "weight", "lat", "lon", np, 10);
        TEST_ASSERT_TRUE(op != NULL);
        ray_error_clear();
        ray_t* r = ray_execute(g, op);
        test_result_t rr = expect_corrupt(r, "astar coordinates");
        ray_graph_free(g);
        if (rr.status != TEST_PASS) return rr;
    }
    ray_rel_free(rel);
    ray_release(edges); ray_release(props); ray_release(np);
    ray_release(t);
    PASS();
}

/* LIKE over a scanned column that fails to load reports that failure, and a
 * dotted env path through such a table is undefined for the borrowed lookup
 * and an error for the evaluator's. */
static test_result_t test_like_env_failed_decode(void) {
    ray_t* t = bind_corrupt_table();
    TEST_ASSERT_TRUE(t != NULL);
    ray_graph_t* g = ray_graph_new(t);
    ray_op_t* col = ray_scan(g, "a");
    ray_op_t* pat = ray_const_str(g, "1*", 2);
    ray_op_t* lk  = ray_like(g, col, pat);
    ray_error_clear();
    ray_t* r = ray_execute(g, lk);
    test_result_t rr = expect_corrupt(r, "like");
    ray_graph_free(g);
    if (rr.status != TEST_PASS) return rr;
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    for (int i = 0; i < 3; i++)
        TEST_ASSERT_NULL(ray_env_get(ray_sym_intern("t.a", 3)));
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);   /* the error was freed */
    TEST_ASSERT_NOT_NULL(ray_env_get(ray_sym_intern("t.b", 3)));
    ray_release(t);
    PASS();
}

/* A row projection after a nearest-neighbour step reads every column of the
 * source: the one that fails to load surfaces as the result. */
static test_result_t test_rerank_failed_decode(void) {
    ray_t* t = bind_corrupt_table();
    TEST_ASSERT_TRUE(t != NULL);
    ray_t* emb = ray_list_new(3000);
    float row[3] = {1.0f, 0.0f, 0.0f};
    for (int i = 0; i < 3000; i++) {
        ray_t* v = ray_vec_from_raw(RAY_F32, row, 3);
        emb = ray_list_append(emb, v); ray_release(v);
    }
    ray_t* h = ray_table_new(2);
    h = ray_table_add_col(h, ray_sym_intern("emb", 3), emb); ray_release(emb);
    ray_t* enc = ray_table_get_col_enc(t, ray_sym_intern("a", 1));
    TEST_ASSERT_TRUE(enc != NULL);
    h = ray_table_add_col(h, ray_sym_intern("a", 1), enc);
    TEST_ASSERT_TRUE(h && !RAY_IS_ERR(h));
    /* run the rerank op itself (the query path decodes a table before it gets there) */
    static const float q[3] = {1.0f, 0.0f, 0.0f};
    int64_t cols[2] = { ray_sym_intern("emb", 3), ray_sym_intern("a", 1) };
    for (int i = 0; i < 2; i++) {
        ray_graph_t* g = ray_graph_new(h);
        ray_op_t* root = ray_const_table(g, h);
        root = ray_knn_rerank(g, root, cols[i], q, 3, 3, RAY_HNSW_COSINE);
        TEST_ASSERT_TRUE(root != NULL);
        ray_error_clear();
        ray_t* r = ray_execute(g, root);
        { test_result_t rr = expect_corrupt(r, i == 0 ? "rerank projection" : "rerank column"); if (rr.status != TEST_PASS) { ray_graph_free(g); return rr; } }
        ray_graph_free(g);
    }
    ray_release(h);
    ray_release(t);
    PASS();
}

/* A generation-0 table and the same table loaded from generation 2 serialize
 * to identical bytes. */
static test_result_t test_serde_byte_identity(void) {
    ray_t* ref = ray_table_new(2);
    ray_t* a = i64_col(3000, 3);
    ray_t* f = ray_vec_new(RAY_F64, 3000); f->len = 3000;
    for (int i = 0; i < 3000; i++) ((double*)ray_data(f))[i] = i * 0.5;
    ref = ray_table_add_col(ref, ray_sym_intern("a", 1), a);
    ref = ray_table_add_col(ref, ray_sym_intern("f", 1), f);
    ray_release(a); ray_release(f);
    TEST_ASSERT_EQ_I(ray_splay_save(ref, ray_test_tmp(CS_TMP "/g0"), NULL), RAY_OK);
    (void)ray_test_mkdir_p(ray_test_tmp(CS_TMP "/g2"));
    ray_t* names = ray_vec_new(RAY_STR, 2);
    for (int64_t c = 0; c < 2; c++) {
        char p[300]; ray_t* nm = ray_sym_str(ray_table_col_name(ref, c));
        snprintf(p, sizeof p, "%s/g2/%.*s", ray_test_tmp(CS_TMP), (int)ray_str_len(nm), ray_str_ptr(nm));
        TEST_ASSERT_EQ_I(ray_col_save_gen(ray_table_get_col_idx(ref, c), p, 2), RAY_OK);
        names = ray_str_vec_append(names, ray_str_ptr(nm), ray_str_len(nm));
    }
    ray_col_save(names, ray_test_tmp(CS_TMP "/g2/.d")); ray_release(names);
    ray_t* t0 = ray_read_splayed(ray_test_tmp(CS_TMP "/g0"), NULL);
    ray_t* t2 = ray_read_splayed(ray_test_tmp(CS_TMP "/g2"), NULL);
    TEST_ASSERT_TRUE(t0 && !RAY_IS_ERR(t0) && t2 && !RAY_IS_ERR(t2));
    TEST_ASSERT_TRUE(ray_table_get_col_enc_idx(t2, 0) != NULL);
    int64_t n0 = ray_serde_size(t0), n2 = ray_serde_size(t2);
    TEST_ASSERT_TRUE(n0 > 0);
    TEST_ASSERT_EQ_I(n0, n2);
    ray_t* w0 = ray_ser(t0); ray_t* w2 = ray_ser(t2);
    TEST_ASSERT_TRUE(w0 && !RAY_IS_ERR(w0) && w2 && !RAY_IS_ERR(w2));
    TEST_ASSERT_EQ_I(w0->len, w2->len);
    TEST_ASSERT_TRUE(memcmp(ray_data(w0), ray_data(w2), (size_t)w0->len) == 0);
    ray_release(w0); ray_release(w2); ray_release(t0); ray_release(t2); ray_release(ref);
    PASS();
}

/* A success must not erase a pending message; a failure replaces it. */
static test_result_t test_getter_keeps_pending_message(void) {
    ray_t* t = ray_table_new(1);
    ray_t* a = i64_col(10, 1);
    t = ray_table_add_col(t, ray_sym_intern("a", 1), a);
    ray_release(a);
    ray_error_free(ray_error("domain", "x"));
    TEST_ASSERT_NOT_NULL(ray_table_get_col(t, ray_sym_intern("a", 1)));
    TEST_ASSERT_TRUE(ray_error_msg() && strcmp(ray_error_msg(), "x") == 0);
    TEST_ASSERT_NOT_NULL(ray_table_get_col_idx(t, 0));
    TEST_ASSERT_TRUE(ray_error_msg() && strcmp(ray_error_msg(), "x") == 0);
    TEST_ASSERT_NULL(ray_table_get_col(t, ray_sym_intern("zz", 2)));       /* absent: not a failure */
    TEST_ASSERT_TRUE(ray_error_msg() && strcmp(ray_error_msg(), "x") == 0);
    ray_release(t);
    t = mixed_table();
    FILE* f = fopen(ray_test_tmp(CS_TMP "/mix/a"), "r+b"); TEST_ASSERT_TRUE(f != NULL); fseek(f, 160 + 6, SEEK_SET); fputc(1, f); fclose(f);
    ray_release(t);
    t = ray_read_splayed(ray_test_tmp(CS_TMP "/mix"), NULL);
    ray_error_free(ray_error("domain", "x"));
    TEST_ASSERT_NULL(ray_table_get_col(t, ray_sym_intern("a", 1)));
    TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), ray_test_tmp(CS_TMP "/mix/a")));
    ray_release(t);
    PASS();
}

/* .mem.objsize counts the decoded cache of an encoded column. */
static test_result_t test_objsize_counts_cache(void) {
    ray_t* t = bind_gen2_table("os");
    TEST_ASSERT_TRUE(t != NULL);
    ray_t* before = ray_eval_str("(.mem.objsize t)");
    TEST_ASSERT_TRUE(before && !RAY_IS_ERR(before));
    int64_t b = before->i64; ray_release(before);
    TEST_ASSERT_TRUE(ray_table_col(t, ray_sym_intern("k", 1)) != NULL);
    ray_t* after = ray_eval_str("(.mem.objsize t)");
    TEST_ASSERT_TRUE(after && !RAY_IS_ERR(after));
    TEST_ASSERT_TRUE(after->i64 >= b + 3000 * 8);
    ray_release(after); ray_release(t);
    PASS();
}

/* A generation-0 parted column's null flag lives on its segments. */
static test_result_t test_has_nulls_parted(void) {
    ray_t* s0 = i64_col(5, 1); ray_t* s1 = i64_col(5, 1);
    s1->attrs |= RAY_ATTR_HAS_NULLS;
    ray_t* w = ray_alloc(2 * sizeof(ray_t*));
    w->type = RAY_PARTED_BASE + RAY_I64; w->len = 2; w->attrs = 0;
    memset(w->aux, 0, 16);
    ((ray_t**)ray_data(w))[0] = s0; ((ray_t**)ray_data(w))[1] = s1;
    ray_t* t = ray_table_new(1);
    t = ray_table_add_col(t, ray_sym_intern("p", 1), w);
    ray_release(w);
    TEST_ASSERT_TRUE(ray_table_col_has_nulls(t, ray_sym_intern("p", 1)));
    s1->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    TEST_ASSERT_FALSE(ray_table_col_has_nulls(t, ray_sym_intern("p", 1)));
    ray_release(t);
    PASS();
}

/* .db.parted.get loads partitions of either generation, mixed, and
 * decodes a generation-2 segment only on materialization. */
static void write_part(const char* day, int gen) {
    char dir[256], p[300];
    snprintf(dir, sizeof dir, "%s/pdb/%s/tb", ray_test_tmp(CS_TMP), day);
    (void)ray_test_mkdir_p(dir);
    ray_t* a = i64_col(3000, 3);
    snprintf(p, sizeof p, "%s/a", dir); ray_col_save_gen(a, p, gen);
    ray_release(a);
    ray_t* names = ray_vec_new(RAY_STR, 1); names = ray_str_vec_append(names, "a", 1);
    snprintf(p, sizeof p, "%s/.d", dir); ray_col_save(names, p); ray_release(names);
}
static test_result_t test_parted_gen2_loads(void) {
    write_part("2024.01.01", 0); write_part("2024.01.02", 0);
    ray_t* ok = ray_read_parted(ray_test_tmp(CS_TMP "/pdb"), "tb");
    TEST_ASSERT_TRUE(ok && !RAY_IS_ERR(ok));
    TEST_ASSERT_EQ_I(ray_table_ncols(ok), 2);
    ray_release(ok);
    ray_t* a = NULL;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) { write_part("2024.01.01", 2); write_part("2024.01.02", 0); }
        else           { write_part("2024.01.01", 0); write_part("2024.01.02", 2); }
        ray_t* t = ray_read_parted(ray_test_tmp(CS_TMP "/pdb"), "tb");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        TEST_ASSERT_EQ_I(ray_table_ncols(t), 2);
        a = ray_table_col(t, ray_sym_intern("a", 1));
        TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a));
        TEST_ASSERT_EQ_I(a->type, RAY_PARTED_BASE + RAY_I64);
        TEST_ASSERT_EQ_I(ray_parted_nrows(a), 6000);
        ray_t** sg = (ray_t**)ray_data(a);
        for (int i = 0; i < 2; i++) {
            TEST_ASSERT_TRUE(sg[i] && sg[i]->type == RAY_I64 && sg[i]->len == 3000);
            for (int64_t r = 0; r < 3000; r++)
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(sg[i]))[r], r * 3);
        }
        ray_release(t);
    }
    PASS();
}

static void ipc_setup(void) {
    rt_setup();
    ray_poll_t* p = ray_poll_create();
    if (p) ray_runtime_set_poll(p);
}
static void ipc_teardown(void) {
    ray_poll_t* p = (ray_poll_t*)ray_runtime_get_poll();
    if (p) { ray_runtime_set_poll(NULL); ray_poll_destroy(p); }
    rt_teardown();
}

/* A result holding a column that fails to decode reaches the client as that
 * error (code and column path), not as "not serializable". */
static test_result_t test_ipc_failed_decode(void) {
    ray_t* t = bind_corrupt_table();
    TEST_ASSERT_TRUE(t != NULL);
    ray_test_server_t srv;
    RAY_TEST_SERVER_START(srv);
    int64_t h = ray_ipc_connect("127.0.0.1", srv.port, NULL, NULL, 0);
    TEST_ASSERT_TRUE(h >= 0);
    ray_t* msg = ray_str("t", 1);
    ray_t* r = ray_ipc_send(h, msg);
    ray_release(msg);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "corrupt");
    /* an error frame carries only its code: the message stays on the server */
    ray_error_free(r);
    ray_ipc_close(h);
    ray_test_server_stop(&srv);
    ray_release(t);
    PASS();
}

static uint8_t file_gen(const char* path) {
    uint8_t h[32] = {0};
    FILE* f = fopen(path, "rb");
    if (!f) return 255;
    size_t n = fread(h, 1, 32, f); fclose(f);
    if (n != 32) return 255;
    int g = ray_col_gen_of(h);
    return g < 0 ? 254 : (uint8_t)g;
}

static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)sz + 1); *n = fread(b, 1, (size_t)sz, f); fclose(f);
    return b;
}

static ray_err_t save_ctx(ray_t* col, const char* path, uint8_t gen, struct ray_sym_domain_s* dom) {
    ray_col_write_t w; memset(&w, 0, sizeof w);
    ray_t* e = ray_col_src_bind(col, &w.src);
    if (e) { ray_error_free(e); return RAY_ERR_TYPE; }
    w.name = "c"; w.gen = gen; w.target_dom = dom; w.durable = false;
    ray_err_t r = ray_col_save_ctx(&w, path);
    ray_col_src_release(&w.src);
    return r;
}

static test_result_t test_matrix_flat(void) {
    ray_t* v = i64_col(140000, 7);
    /* plain -> 0 is today's writer, byte for byte */
    TEST_ASSERT_EQ_I(save_ctx(v, ray_test_tmp(CS_TMP "/p0"), 0, NULL), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(v, ray_test_tmp(CS_TMP "/ref0")), RAY_OK);
    size_t na, nb; uint8_t* a = slurp(ray_test_tmp(CS_TMP "/p0"), &na); uint8_t* b = slurp(ray_test_tmp(CS_TMP "/ref0"), &nb);
    TEST_ASSERT_TRUE(na == nb && memcmp(a, b, na) == 0); free(a); free(b);
    /* plain -> 2 */
    TEST_ASSERT_EQ_I(save_ctx(v, ray_test_tmp(CS_TMP "/p2"), 2, NULL), RAY_OK);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/p2")), 2);
    /* encoded -> 2: range reads, no materialization, identical bytes */
    ray_t* enc = ray_col_mmap(ray_test_tmp(CS_TMP "/p2"));
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(save_ctx(enc, ray_test_tmp(CS_TMP "/e2"), 2, NULL), RAY_OK);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    a = slurp(ray_test_tmp(CS_TMP "/p2"), &na); b = slurp(ray_test_tmp(CS_TMP "/e2"), &nb);
    TEST_ASSERT_TRUE(na == nb && memcmp(a, b, na) == 0); free(a); free(b);
    /* encoded -> 0: materializes once, then today's writer */
    TEST_ASSERT_EQ_I(save_ctx(enc, ray_test_tmp(CS_TMP "/e0"), 0, NULL), RAY_OK);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/e0")), 0);
    a = slurp(ray_test_tmp(CS_TMP "/e0"), &na); b = slurp(ray_test_tmp(CS_TMP "/ref0"), &nb);
    TEST_ASSERT_TRUE(na == nb && memcmp(a, b, na) == 0); free(a); free(b);
    ray_release(enc); ray_release(v);
    PASS();
}

/* A SYM column over domain A (insertion order z, a, m) re-encoded into a
 * domain B with another order and a wider width class. */
static test_result_t test_matrix_sym_remap(void) {
    struct ray_sym_domain_s* A = ray_sym_domain_open_or_create(ray_test_tmp(CS_TMP "/A.sym"));
    struct ray_sym_domain_s* B = ray_sym_domain_open_or_create(ray_test_tmp(CS_TMP "/B.sym"));
    TEST_ASSERT_TRUE(A && B);
    ray_sym_domain_intern(A, "", 0); ray_sym_domain_intern(B, "", 0);
    static const char* w[3] = { "z", "a", "m" };
    for (int i = 0; i < 3; i++) ray_sym_domain_intern(A, w[i], 1);
    for (int i = 0; i < 300; i++) { char b[16]; int n = snprintf(b, sizeof b, "pad%d", i); ray_sym_domain_intern(B, b, (size_t)n); }
    for (int i = 2; i >= 0; i--) ray_sym_domain_intern(B, w[i], 1);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(A, false), RAY_OK);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(B, false), RAY_OK);
    ray_t* v = ray_sym_vec_new(RAY_SYM_W8, 5000); v->len = 5000;
    for (int64_t i = 0; i < 5000; i++) ray_write_sym(ray_data(v), i, (uint64_t)(i % 4), RAY_SYM, RAY_SYM_W8);
    v->sym_domain = A; ray_sym_domain_retain(A);
    v->attrs |= RAY_ATTR_HAS_NULLS;
    /* plain -> 2 over A, then encoded -> 2 into B, then encoded -> 0 into B */
    TEST_ASSERT_EQ_I(save_ctx(v, ray_test_tmp(CS_TMP "/sa"), 2, A), RAY_OK);
    ray_t* enc = ray_col_mmap_splayed_dom(ray_test_tmp(CS_TMP "/sa"), A);
    TEST_ASSERT_TRUE(enc && !RAY_IS_ERR(enc));
    TEST_ASSERT_EQ_I(save_ctx(enc, ray_test_tmp(CS_TMP "/sb2"), 2, B), RAY_OK);
    TEST_ASSERT_EQ_I(save_ctx(enc, ray_test_tmp(CS_TMP "/sb0"), 0, B), RAY_OK);
    const char* outs[2] = { ray_test_tmp(CS_TMP "/sb2"), ray_test_tmp(CS_TMP "/sb0") };
    for (int o = 0; o < 2; o++) {
        ray_t* r = ray_col_load_dom(outs[o], B);
        TEST_ASSERT_FMT(r && !RAY_IS_ERR(r), "reload %s", outs[o]);
        TEST_ASSERT_EQ_I(r->attrs & RAY_SYM_W_MASK, RAY_SYM_W16);
        for (int64_t i = 0; i < 5000; i++) {
            ray_t* s0 = ray_sym_vec_cell(v, i); ray_t* s1 = ray_sym_vec_cell(r, i);
            TEST_ASSERT_TRUE(ray_str_len(s0) == ray_str_len(s1) &&
                             memcmp(ray_str_ptr(s0), ray_str_ptr(s1), ray_str_len(s0)) == 0);
        }
        TEST_ASSERT_TRUE(r->attrs & RAY_ATTR_HAS_NULLS);
        ray_release(r);
    }
    ray_release(enc); ray_release(v);
    ray_sym_domain_release(A); ray_sym_domain_release(B);
    PASS();
}

/* Equal long strings at distinct pool offsets with differing cached hashes
 * are one value (storage equality ignores pool_off and hash32): CONST, and
 * the round trip gives both rows the bytes. */
static test_result_t test_str_const_distinct_offsets(void) {
    static const char txt[] = "a twenty-one-byte str";      /* 21 bytes */
    ray_t* v = ray_vec_new(RAY_STR, 2); v->len = 2;
    ray_t* pool = ray_alloc(42); pool->type = RAY_U8; pool->len = 42;
    memcpy(ray_data(pool), txt, 21); memcpy((char*)ray_data(pool) + 21, txt, 21);
    v->str_pool = pool;
    ray_str_t* d = ray_data(v);
    for (int i = 0; i < 2; i++) {
        memset(&d[i], 0, sizeof d[i]);
        d[i].len = 21; memcpy(d[i].prefix, txt, 4);
        d[i].pool_off = (uint32_t)(21 * i); d[i].hash32 = i ? 9u : 5u;
    }
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(CS_TMP "/sc"), 2), RAY_OK);
    ray_t* c = ray_col_mmap(ray_test_tmp(CS_TMP "/sc"));
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    ray_enc_desc_t d0; memcpy(&d0, ray_enccol_of(c)->base + 160, 48);
    TEST_ASSERT_EQ_I(d0.encoding, RAY_ENC_CONST);
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    for (int i = 0; i < 2; i++) {
        size_t n; const char* p = ray_str_vec_get(m, i, &n);
        TEST_ASSERT_TRUE(n == 21 && memcmp(p, txt, 21) == 0);
    }
    TEST_ASSERT_EQ_I(m->str_pool->len, 42);                 /* once per occurrence */
    ray_release(c); ray_release(v);
    PASS();
}

/* A failed encoded -> 2 save leaves no file and no staging file. */
static test_result_t test_matrix_failure_cleanup(void) {
    ray_t* v = i64_col(3000, 1);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(CS_TMP "/src"), 2), RAY_OK);
    ray_release(v);
    FILE* f = fopen(ray_test_tmp(CS_TMP "/src"), "r+b"); TEST_ASSERT_TRUE(f != NULL); fseek(f, 160 + 6, SEEK_SET); fputc(1, f); fclose(f);
    ray_t* enc = ray_col_mmap(ray_test_tmp(CS_TMP "/src"));
    TEST_ASSERT_EQ_I(save_ctx(enc, ray_test_tmp(CS_TMP "/dst"), 2, NULL), RAY_ERR_CORRUPT);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/dst")), 255);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/dst.tmp")), 255);
    TEST_ASSERT_EQ_I(save_ctx(enc, ray_test_tmp(CS_TMP "/dst"), 0, NULL), RAY_ERR_CORRUPT);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/dst")), 255);
    ray_release(enc);
    PASS();
}

/* Failure injection: the range writer's buffers (at least one injection is
 * asserted to fire), the encoded source's own allocations and a mid-write I/O
 * failure leave no file, no
 * staging file and no leaked bytes. */
static test_result_t test_matrix_failure_inject(void) {
    ray_t* v = ray_vec_new(RAY_STR, 70000); v->len = 70000;
    ray_t* pool = ray_alloc(70000 * 24); pool->type = RAY_U8; pool->len = 70000 * 24;
    ray_str_t* d = ray_data(v);
    for (int64_t i = 0; i < 70000; i++) {
        memset(&d[i], 0, sizeof d[i]);
        d[i].len = 24; d[i].pool_off = (uint32_t)(24 * i);
        for (int k = 0; k < 24; k++) ((char*)ray_data(pool))[24 * i + k] = (char)('a' + (i * 7 + k * 13) % 26);
        memcpy(d[i].prefix, (char*)ray_data(pool) + 24 * i, 4);
    }
    v->str_pool = pool;
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(CS_TMP "/s"), 2), RAY_OK);
    ray_release(v);
    ray_t* enc = ray_col_mmap(ray_test_tmp(CS_TMP "/s"));
    TEST_ASSERT_TRUE(enc && !RAY_IS_ERR(enc));
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    bool done = false;
    int injected = 0;
    for (int nth = 1; nth <= 8 && !done; nth++) {
        ray_col_test_fail_alloc(nth);
        ray_err_t r = save_ctx(enc, ray_test_tmp(CS_TMP "/d"), 2, NULL);
        ray_col_test_fail_alloc(0);
        if (r == RAY_OK) { done = true; break; }
        injected++;
        TEST_ASSERT_EQ_I(r, RAY_ERR_OOM);
        TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/d")), 255);
        TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/d.tmp")), 255);
        ray_mem_stats(&m1);
        TEST_ASSERT_FMT(m1.bytes_allocated == m0.bytes_allocated, "nth %d leaked", nth);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(injected >= 1);                        /* the writer's buffers were really failed */
    {   /* encoded -> 2 keeps a STR column byte for byte */
        size_t na, nb; uint8_t* a = slurp(ray_test_tmp(CS_TMP "/s"), &na); uint8_t* b = slurp(ray_test_tmp(CS_TMP "/d"), &nb);
        TEST_ASSERT_TRUE(a && b && na == nb && memcmp(a, b, na) == 0); free(a); free(b);
    }
    (void)remove(ray_test_tmp(CS_TMP "/d"));
    /* decode-side allocation failures, encoded -> 2 and encoded -> 0 */
    for (int g = 0; g <= 2; g += 2) {
        for (int nth = 1; nth <= 16; nth++) {
            ray_enccol_test_fail_alloc(nth);
            ray_err_t r = save_ctx(enc, ray_test_tmp(CS_TMP "/g"), (uint8_t)g, NULL);
            ray_enccol_test_fail_alloc(0);
            if (r == RAY_OK) { (void)remove(ray_test_tmp(CS_TMP "/g")); break; }
            TEST_ASSERT_EQ_I(r, RAY_ERR_OOM);
            TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/g")), 255);
            TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/g.tmp")), 255);
            ray_mem_stats(&m1);
            TEST_ASSERT_TRUE(m1.bytes_allocated == m0.bytes_allocated);
        }
    }
    (void)remove(ray_test_tmp(CS_TMP "/g"));
    ray_mem_stats(&m0);                                     /* the successful encoded -> 0 cached its materialization */
    /* staging path unusable: the open fails, nothing appears */
    TEST_ASSERT_EQ_I(mkdir(ray_test_tmp(CS_TMP "/e.tmp"), 0700), 0);
    TEST_ASSERT_EQ_I(save_ctx(enc, ray_test_tmp(CS_TMP "/e"), 2, NULL), RAY_ERR_IO);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/e")), 255);
    (void)rmdir(ray_test_tmp(CS_TMP "/e.tmp"));
    /* I/O failure while writing: file size limit */
    struct rlimit old, lim;
    if (getrlimit(RLIMIT_FSIZE, &old) == 0) {
        lim = old; lim.rlim_cur = 65536;
        void (*oldsig)(int) = signal(SIGXFSZ, SIG_IGN);
        if (setrlimit(RLIMIT_FSIZE, &lim) == 0) {
            ray_err_t r = save_ctx(enc, ray_test_tmp(CS_TMP "/f"), 2, NULL);
            setrlimit(RLIMIT_FSIZE, &old);
            TEST_ASSERT_EQ_I(r, RAY_ERR_IO);
            TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/f")), 255);
            TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/f.tmp")), 255);
        }
        signal(SIGXFSZ, oldsig);
    }
    ray_mem_stats(&m1);
    TEST_ASSERT_TRUE(m1.bytes_allocated == m0.bytes_allocated);
    ray_release(enc);
    PASS();
}

static ray_t* sym_vec_over(struct ray_sym_domain_s* d, const int64_t* ids, int64_t n) {
    ray_t* v = ray_sym_vec_new(RAY_SYM_W8, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ray_write_sym(ray_data(v), i, (uint64_t)ids[i], RAY_SYM, RAY_SYM_W8);
    v->sym_domain = d; ray_sym_domain_retain(d);
    return v;
}

static test_result_t test_write_sym_domains(void) {
    struct ray_sym_domain_s* A = ray_sym_domain_open_or_create(ray_test_tmp(CS_TMP "/A.sym"));
    struct ray_sym_domain_s* C = ray_sym_domain_open_or_create(ray_test_tmp(CS_TMP "/C.sym"));
    TEST_ASSERT_TRUE(A && C);
    static const char* w[4] = { "", "q", "r", "s" };
    for (int i = 0; i < 4; i++) ray_sym_domain_intern(A, w[i], strlen(w[i]));
    for (int i = 0; i < 3; i++) ray_sym_domain_intern(C, w[i], strlen(w[i]));   /* no "s" */
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(A, false), RAY_OK);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(C, false), RAY_OK);

    /* an id past the target's count is corrupt in both generations, nothing left behind */
    int64_t bad[3] = { 1, 2, 9 };
    ray_t* vb = sym_vec_over(A, bad, 3);
    for (int g = 0; g <= 2; g += 2) {
        TEST_ASSERT_EQ_I(save_ctx(vb, ray_test_tmp(CS_TMP "/bad"), (uint8_t)g, A), RAY_ERR_CORRUPT);
        TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/bad")), 255);
        TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/bad.tmp")), 255);
    }
    ray_release(vb);

    /* absent from the target domain: corrupt, encoded -> 0 leaves no staging file */
    int64_t ok[6] = { 1, 1, 2, 3, 3, 3 };                   /* sorted */
    ray_t* v = sym_vec_over(A, ok, 6);
    v->attrs |= RAY_ATTR_SORTED;
    TEST_ASSERT_EQ_I(save_ctx(v, ray_test_tmp(CS_TMP "/pa"), 2, A), RAY_OK);
    ray_t* enc = ray_col_mmap_splayed_dom(ray_test_tmp(CS_TMP "/pa"), A);
    TEST_ASSERT_TRUE(enc && !RAY_IS_ERR(enc));
    for (int g = 0; g <= 2; g += 2) {
        TEST_ASSERT_EQ_I(save_ctx(enc, ray_test_tmp(CS_TMP "/miss"), (uint8_t)g, C), RAY_ERR_CORRUPT);
        TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/miss")), 255);
        TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/miss.tmp")), 255);
    }
    /* same domain: byte identical to plain -> 2, SORTED kept, rc = count */
    TEST_ASSERT_EQ_I(save_ctx(enc, ray_test_tmp(CS_TMP "/same"), 2, A), RAY_OK);
    size_t na, nb; uint8_t* a = slurp(ray_test_tmp(CS_TMP "/pa"), &na); uint8_t* b = slurp(ray_test_tmp(CS_TMP "/same"), &nb);
    TEST_ASSERT_TRUE(a && b && na == nb && memcmp(a, b, na) == 0);
    uint32_t rc; memcpy(&rc, b + 20, 4);
    TEST_ASSERT_EQ_I(rc, 4); free(a); free(b);
    ray_t* r = ray_col_load_dom(ray_test_tmp(CS_TMP "/same"), A);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && (r->attrs & RAY_ATTR_SORTED));
    ray_release(r);
    /* another domain that has every symbol: SORTED dropped */
    struct ray_sym_domain_s* D = ray_sym_domain_open_or_create(ray_test_tmp(CS_TMP "/D.sym"));
    for (int i = 3; i >= 0; i--) ray_sym_domain_intern(D, w[i], strlen(w[i]));
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(D, false), RAY_OK);
    TEST_ASSERT_EQ_I(save_ctx(enc, ray_test_tmp(CS_TMP "/other"), 2, D), RAY_OK);
    r = ray_col_load_dom(ray_test_tmp(CS_TMP "/other"), D);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && !(r->attrs & RAY_ATTR_SORTED));
    ray_release(r);
    /* bare file from a FILE-domain source: process-local ids through the LUT */
    for (int g = 0; g <= 2; g += 2) {
        const char* o = g ? ray_test_tmp(CS_TMP "/bare2") : ray_test_tmp(CS_TMP "/bare0");
        TEST_ASSERT_EQ_I(save_ctx(enc, o, (uint8_t)g, NULL), RAY_OK);
        r = ray_col_load(o);
        TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
        for (int64_t i = 0; i < 6; i++) {
            ray_t* s0 = ray_sym_vec_cell(v, i); ray_t* s1 = ray_sym_vec_cell(r, i);
            TEST_ASSERT_TRUE(ray_str_len(s0) == ray_str_len(s1) &&
                             memcmp(ray_str_ptr(s0), ray_str_ptr(s1), ray_str_len(s0)) == 0);
        }
        ray_release(r);
    }
    /* P1 keeps the index as is */
    ray_col_write_t cw; memset(&cw, 0, sizeof cw);
    TEST_ASSERT_TRUE(ray_col_src_bind(v, &cw.src) == NULL);
    cw.gen = 2; cw.target_dom = A; cw.index_decision = RAY_COL_IDX_DROP;
    TEST_ASSERT_EQ_I(ray_col_save_ctx(&cw, ray_test_tmp(CS_TMP "/idx")), RAY_ERR_NYI);
    ray_col_src_release(&cw.src);
    ray_release(enc); ray_release(v);
    ray_sym_domain_release(A); ray_sym_domain_release(C); ray_sym_domain_release(D);
    PASS();
}

/* A parted source is refused up front in both generations, undecoded. */
static test_result_t test_write_parted_refused(void) {
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) { write_part("2024.01.01", 2); write_part("2024.01.02", 2); }
        else           { write_part("2024.01.01", 2); write_part("2024.01.02", 0); }
        ray_t* t = ray_read_parted(ray_test_tmp(CS_TMP "/pdb"), "tb");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_t* a = ray_table_col(t, ray_sym_intern("a", 1));
        TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a));
        ray_enc_counters_reset();
        for (int g = 0; g <= 2; g += 2) {
            TEST_ASSERT_EQ_I(save_ctx(a, ray_test_tmp(CS_TMP "/pq"), (uint8_t)g, NULL), RAY_ERR_NYI);
            TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/pq")), 255);
            TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/pq.tmp")), 255);
        }
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        ray_release(t);
    }
    PASS();
}


/* ---- Task 15: generation resolution in splayed saves ---- */

static uint8_t col_gen_in(const char* dir, const char* name) {
    char cur[1024], p[1200];
    if (ray_splay_resolve_dir(dir, cur, sizeof cur) != RAY_OK) return 253;
    snprintf(p, sizeof p, "%s/%s", cur, name);
    return file_gen(p);
}

static ray_t* plain_tbl(int64_t n) {
    ray_t* t = ray_table_new(2);
    ray_t* a = i64_col(n, 3); ray_t* b = i64_col(n, 5);
    t = ray_table_add_col(t, ray_sym_intern("a", 1), a);
    t = ray_table_add_col(t, ray_sym_intern("b", 1), b);
    ray_release(a); ray_release(b);
    return t;
}

static test_result_t test_inherit_destination(void) {
    const char* d = ray_test_tmp(CS_TMP "/inh");
    ray_t* t = plain_tbl(3000);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, RAY_COMPRESS_ON), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 2);
    TEST_ASSERT_EQ_I(col_gen_in(d, "b"), 2);
    /* a fresh plain table, no option: the destination keeps generation 2 */
    TEST_ASSERT_EQ_I(ray_splay_save(t, d, NULL), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 2);
    /* the loaded (encoded) table into a new directory: the source rule */
    ray_t* l = ray_read_splayed(d, NULL);
    TEST_ASSERT_EQ_I(ray_splay_save(l, ray_test_tmp(CS_TMP "/inh2"), NULL), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/inh2"), "b"), 2);
    ray_release(l);
    /* explicit false writes generation 0 */
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, RAY_COMPRESS_OFF), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 0);
    TEST_ASSERT_EQ_I(col_gen_in(d, "b"), 0);
    /* absent over a generation-0 destination stays 0 */
    TEST_ASSERT_EQ_I(ray_splay_save(t, d, NULL), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 0);
    ray_release(t);
    PASS();
}

static test_result_t test_update_then_save(void) {
    const char* d = ray_test_tmp(CS_TMP "/upd");
    ray_t* t = plain_tbl(3000);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, RAY_COMPRESS_ON), RAY_OK);
    ray_release(t);
    ray_t* l = ray_read_splayed(d, NULL);
    ray_t* mut = i64_col(3000, 11);                     /* a value update's plain result */
    ray_table_set_col_idx(l, 0, mut);
    ray_release(mut);
    TEST_ASSERT_EQ_I(ray_splay_save(l, d, NULL), RAY_OK);         /* same directory */
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 2);
    TEST_ASSERT_EQ_I(col_gen_in(d, "b"), 2);
    TEST_ASSERT_EQ_I(ray_splay_save(l, ray_test_tmp(CS_TMP "/upd2"), NULL), RAY_OK);  /* new directory */
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/upd2"), "a"), 0);
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/upd2"), "b"), 2);
    /* the mixed directory keeps each column's generation on a plain resave */
    ray_release(l);
    l = plain_tbl(3000);
    TEST_ASSERT_EQ_I(ray_splay_save(l, ray_test_tmp(CS_TMP "/upd2"), NULL), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/upd2"), "a"), 0);
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/upd2"), "b"), 2);
    ray_release(l);
    PASS();
}

static test_result_t test_forced_generation0(void) {
    const char* d = ray_test_tmp(CS_TMP "/forced");
    ray_t* t = ray_table_new(3);
    ray_t* lnk = i64_col(100, 1);
    lnk->attrs |= RAY_ATTR_HAS_LINK; lnk->link_target = ray_sym_intern("other", 5);
    ray_t* hx = i64_col(100, 2);
    ray_t* hi = ray_index_attach_hash(&hx);
    TEST_ASSERT_TRUE(hi && !RAY_IS_ERR(hi)); hx = hi;
    ray_t* lst = ray_list_new(100);
    for (int i = 0; i < 100; i++) { ray_t* x = ray_i64(i); lst = ray_list_append(lst, x); ray_release(x); }
    t = ray_table_add_col(t, ray_sym_intern("l", 1), lnk);
    t = ray_table_add_col(t, ray_sym_intern("h", 1), hx);
    t = ray_table_add_col(t, ray_sym_intern("x", 1), lst);
    ray_release(lnk); ray_release(hx); ray_release(lst);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, RAY_COMPRESS_ON), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(d, "l"), 0);
    TEST_ASSERT_EQ_I(col_gen_in(d, "h"), 0);
    char cur[1024], p[1200];
    ray_splay_resolve_dir(d, cur, sizeof cur);
    snprintf(p, sizeof p, "%s/l.link", cur);
    TEST_ASSERT_EQ_I(access(p, F_OK), 0);               /* the .link sidecar was written */
    ray_release(t);
    PASS();
}

/* The four-way matrix over partitions, with ZONE / UNIQUE / linked inputs:
 * plain -> 2, encoded -> 2 (index kind and flag carried), encoded -> 0
 * (materialized, the synthesized index persisted the generation-0 way), and
 * linked columns stay generation 0 with their sidecar. */
static test_result_t test_matrix_parted_indexed(void) {
    for (int p = 0; p < 2; p++) {
        char dir[256]; snprintf(dir, sizeof dir, ray_test_tmp(CS_TMP "/mp/2024.01.0%d/t"), p + 1);
        ray_t* z = i64_col(3000, 3 + p);
        ray_t* zi = ray_index_attach_zone(&z); TEST_ASSERT_TRUE(zi && !RAY_IS_ERR(zi)); z = zi;
        ray_t* u = i64_col(3000, 1);
        ray_t* root = ray_index_new_root(RAY_IDX_NONE, RAY_I64, 3000);
        ray_index_payload(root)->markers = RAY_MARK_UNIQUE;
        ray_t* ui = ray_index_attach_persisted(&u, root); TEST_ASSERT_TRUE(ui && !RAY_IS_ERR(ui)); u = ui;
        ray_t* l = i64_col(3000, 1);
        l->attrs |= RAY_ATTR_HAS_LINK; l->link_target = ray_sym_intern("other", 5);
        ray_t* t = ray_table_new(3);
        t = ray_table_add_col(t, ray_sym_intern("z", 1), z);
        t = ray_table_add_col(t, ray_sym_intern("u", 1), u);
        t = ray_table_add_col(t, ray_sym_intern("l", 1), l);
        ray_release(z); ray_release(u); ray_release(l);
        TEST_ASSERT_EQ_I(ray_splay_save_mode(t, dir, NULL, RAY_COMPRESS_ON), RAY_OK);
        ray_release(t);
        TEST_ASSERT_EQ_I(col_gen_in(dir, "z"), 2);
        TEST_ASSERT_EQ_I(col_gen_in(dir, "u"), 2);
        TEST_ASSERT_EQ_I(col_gen_in(dir, "l"), 0);
    }
    for (int p = 0; p < 2; p++) {
        char src[256], d2[256], d0[256], cur[1024], lp[1200];
        snprintf(src, sizeof src, ray_test_tmp(CS_TMP "/mp/2024.01.0%d/t"), p + 1);
        snprintf(d2, sizeof d2, ray_test_tmp(CS_TMP "/mp2/2024.01.0%d/t"), p + 1);
        snprintf(d0, sizeof d0, ray_test_tmp(CS_TMP "/mp0/2024.01.0%d/t"), p + 1);
        ray_t* t = ray_read_splayed(src, NULL);
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_enc_counters_reset();
        TEST_ASSERT_EQ_I(ray_splay_save(t, d2, NULL), RAY_OK);                 /* encoded -> 2 */
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d0, NULL, RAY_COMPRESS_OFF), RAY_OK);  /* encoded -> 0 */
        ray_release(t);
        TEST_ASSERT_EQ_I(col_gen_in(d2, "z"), 2);
        TEST_ASSERT_EQ_I(col_gen_in(d0, "z"), 0);
        ray_splay_resolve_dir(d2, cur, sizeof cur);
        snprintf(lp, sizeof lp, "%s/l.link", cur);
        TEST_ASSERT_EQ_I(access(lp, F_OK), 0);
        ray_t* t2 = ray_read_splayed(d2, NULL);
        ray_col_meta_t m;
        TEST_ASSERT_TRUE(ray_table_col_meta(t2, ray_sym_intern("z", 1), &m) && m.index_kind == RAY_IDX_ZONE);
        TEST_ASSERT_TRUE(ray_table_col_meta(t2, ray_sym_intern("u", 1), &m) && m.unique);
        ray_release(t2);
        ray_t* t0 = ray_read_splayed(d0, NULL);
        ray_t* z0 = ray_table_col(t0, ray_sym_intern("z", 1));
        TEST_ASSERT_EQ_I(ray_index_kind(z0), RAY_IDX_ZONE);
        ray_t* u0 = ray_table_col(t0, ray_sym_intern("u", 1));
        TEST_ASSERT_TRUE(ray_index_has(u0) && (ray_index_payload(u0->index)->markers & RAY_MARK_UNIQUE));
        ray_release(t0);
    }
    ray_t* pt = ray_read_parted(ray_test_tmp(CS_TMP "/mp2"), "t");
    TEST_ASSERT_TRUE(pt && !RAY_IS_ERR(pt));
    TEST_ASSERT_EQ_I(ray_table_nrows(pt), 6000);
    ray_release(pt);
    PASS();
}

static test_result_t test_chunk_zone_recorded(void) {
    const char* d = ray_test_tmp(CS_TMP "/cz");
    ray_t* t = plain_tbl(70000);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, RAY_COMPRESS_ON), RAY_OK);
    ray_release(t);
    ray_t* l = ray_read_splayed(d, NULL);
    ray_col_meta_t m;
    TEST_ASSERT_TRUE(ray_table_col_meta(l, ray_sym_intern("a", 1), &m));
    TEST_ASSERT_EQ_I(m.index_kind, RAY_IDX_CHUNK_ZONE);
    ray_t* a = ray_table_col(l, ray_sym_intern("a", 1));
    TEST_ASSERT_EQ_I(ray_index_kind(a), RAY_IDX_CHUNK_ZONE);
    ray_release(l);
    PASS();
}

static test_result_t test_staged_and_sym(void) {
    ray_t* t = ray_table_new(1);
    ray_t* s = ray_sym_vec_new(RAY_SYM_W64, 2000); s->len = 2000;
    int64_t ids[3] = { ray_sym_intern("x", 1), ray_sym_intern("y", 1), ray_sym_intern("z", 1) };
    for (int i = 0; i < 2000; i++) ((int64_t*)ray_data(s))[i] = ids[i % 3];
    t = ray_table_add_col(t, ray_sym_intern("s", 1), s);
    ray_release(s);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, ray_test_tmp(CS_TMP "/sy"), ray_test_tmp(CS_TMP "/sy.sym"), RAY_COMPRESS_ON), RAY_OK);
    ray_release(t);
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/sy"), "s"), 2);
    ray_t* l = ray_read_splayed(ray_test_tmp(CS_TMP "/sy"), ray_test_tmp(CS_TMP "/sy.sym"));
    TEST_ASSERT_TRUE(l && !RAY_IS_ERR(l));
    /* private staging path, absent mode, encoded source: generation 2, into
     * another symfile.  Like the CSV/Parquet staging callers, the test holds
     * the target domain alive across the save and flushes it afterwards. */
    struct ray_sym_domain_s* d2 = ray_sym_domain_open_or_create(ray_test_tmp(CS_TMP "/sy2.sym"));
    TEST_ASSERT_NOT_NULL(d2);
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(ray_splay_save_staged_bulk_mode(l, ray_test_tmp(CS_TMP "/sy2"), ray_test_tmp(CS_TMP "/sy2.sym"), RAY_COMPRESS_ABSENT), RAY_OK);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);              /* merge and write by range */
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(d2, false), RAY_OK);
    ray_sym_domain_release(d2);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/sy2/s")), 2);
    ray_t* l2 = ray_read_splayed(ray_test_tmp(CS_TMP "/sy2"), ray_test_tmp(CS_TMP "/sy2.sym"));
    TEST_ASSERT_TRUE(l2 && !RAY_IS_ERR(l2));
    ray_t* c1 = ray_table_col(l, ray_sym_intern("s", 1));
    ray_t* c2 = ray_table_col(l2, ray_sym_intern("s", 1));
    for (int64_t i = 0; i < 2000; i++) {
        ray_t* x = ray_sym_vec_cell(c1, i); ray_t* y = ray_sym_vec_cell(c2, i);
        TEST_ASSERT_TRUE(ray_str_len(x) == ray_str_len(y) && memcmp(ray_str_ptr(x), ray_str_ptr(y), ray_str_len(x)) == 0);
    }
    ray_release(l2); ray_release(l);
    PASS();
}

/* Preflight admits a single encoded column but keeps refusing parted kinds
 * (a parted RAY_ENCCOL and a plain parted wrapper) and the partition key
 * column: a splayed save of a table carrying one is nyi before anything is
 * written, and nothing is decoded. */
static test_result_t test_save_refuses_parted_kinds(void) {
    for (int pass = 0; pass < 2; pass++) {
        write_part("2024.01.01", pass ? 0 : 2); write_part("2024.01.02", pass ? 0 : 2);
        ray_t* pt = ray_read_parted(ray_test_tmp(CS_TMP "/pdb"), "tb");
        TEST_ASSERT_TRUE(pt && !RAY_IS_ERR(pt));
        TEST_ASSERT_EQ_I(ray_table_ncols(pt), 2);
        ray_t* key = ray_table_get_col_enc_idx(pt, 0) ? NULL : ray_table_col_idx(pt, 0);
        TEST_ASSERT_TRUE(key && key->type == RAY_MAPCOMMON);
        ray_t* enc = ray_table_get_col_enc_idx(pt, 1);
        ray_t* slot = enc ? enc : ray_table_col_idx(pt, 1);
        if (pass == 0) TEST_ASSERT_TRUE(enc && ray_enccol_is_parted(enc));
        else           TEST_ASSERT_TRUE(!enc && RAY_IS_PARTED(slot->type));
        ray_enc_counters_reset();
        TEST_ASSERT_EQ_I(ray_col_save_preflight(slot), RAY_ERR_NYI);
        TEST_ASSERT_EQ_I(ray_col_save_preflight(key), RAY_ERR_NYI);
        for (int which = 0; which < 3; which++) {
            ray_t* t;
            if (which == 2) { t = pt; ray_retain(t); }
            else {
                t = ray_table_new(1);
                t = ray_table_add_col(t, ray_sym_intern("c", 1), which ? key : slot);
                TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
            }
            for (int m = 0; m < 3; m++) {
                TEST_ASSERT_EQ_I(ray_splay_save_mode(t, ray_test_tmp(CS_TMP "/pref"), NULL, (ray_compress_t)m), RAY_ERR_NYI);
                TEST_ASSERT_FALSE(ray_splay_has_schema(ray_test_tmp(CS_TMP "/pref")));
                TEST_ASSERT_EQ_I(access(ray_test_tmp(CS_TMP "/pref/c"), F_OK), -1);
            }
            ray_release(t);
        }
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        ray_release(pt);
    }
    /* a single encoded column passes preflight */
    ray_t* v = i64_col(3000, 3);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(CS_TMP "/one"), 2), RAY_OK);
    ray_release(v);
    ray_t* e = ray_col_mmap(ray_test_tmp(CS_TMP "/one"));
    TEST_ASSERT_TRUE(e && !RAY_IS_ERR(e) && e->type == RAY_ENCCOL);
    TEST_ASSERT_EQ_I(ray_col_save_preflight(e), RAY_OK);
    ray_release(e);
    PASS();
}

/* The resolver on its own: explicit modes, the source rule, the destination
 * rule, the forced kinds, and the equivalence-run hook. */
static test_result_t test_resolver_rules(void) {
    ray_t* v = i64_col(100, 1);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ON, v, NULL, "a", 1), 2);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_OFF, v, NULL, "a", 1), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ABSENT, v, NULL, "a", 1), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ABSENT, v, "", "a", 1), 0);
    (void)ray_test_mkdir_p(ray_test_tmp(CS_TMP "/rd"));
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(CS_TMP "/rd/a"), 2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(CS_TMP "/rd/b"), 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_file_gen(ray_test_tmp(CS_TMP "/rd/a")), 2);
    TEST_ASSERT_EQ_I(ray_col_file_gen(ray_test_tmp(CS_TMP "/rd/b")), 0);
    TEST_ASSERT_EQ_I(ray_col_file_gen(ray_test_tmp(CS_TMP "/rd/missing")), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ABSENT, v, ray_test_tmp(CS_TMP "/rd"), "a", 1), 2);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ABSENT, v, ray_test_tmp(CS_TMP "/rd"), "b", 1), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_OFF, v, ray_test_tmp(CS_TMP "/rd"), "a", 1), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen_type(RAY_COMPRESS_ABSENT, RAY_I64, ray_test_tmp(CS_TMP "/rd"), "a", 1), 2);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen_type(RAY_COMPRESS_ABSENT, RAY_I64, ray_test_tmp(CS_TMP "/rd"), "b", 1), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen_type(RAY_COMPRESS_ON, RAY_LIST, NULL, "a", 1), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen_type(RAY_COMPRESS_ON, RAY_I64, NULL, "a", 1), 2);
    ray_t* e = ray_col_mmap(ray_test_tmp(CS_TMP "/rd/a"));
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ABSENT, e, NULL, "x", 1), 2);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_OFF, e, NULL, "x", 1), 0);
    ray_release(e);
    ray_t* lnk = i64_col(10, 1);
    lnk->attrs |= RAY_ATTR_HAS_LINK; lnk->link_target = ray_sym_intern("other", 5);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ON, lnk, ray_test_tmp(CS_TMP "/rd"), "a", 1), 0);
    ray_release(lnk);
    ray_store_test_force_compress(true);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ABSENT, v, NULL, "a", 1), 2);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_OFF, v, NULL, "a", 1), 0);
    ray_store_test_force_compress(false);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ABSENT, v, NULL, "a", 1), 0);
    ray_release(v);
    PASS();
}


static ray_t* sym_tbl(const char* pfx, int nd, int64_t n) {
    ray_t* s = ray_sym_vec_new(RAY_SYM_W64, n); s->len = n;
    for (int64_t i = 0; i < n; i++) {
        char b[32]; int k = snprintf(b, sizeof b, "%s%d", pfx, (int)(i % nd));
        ((int64_t*)ray_data(s))[i] = ray_sym_intern(b, (size_t)k);
    }
    ray_t* t = ray_table_new(1);
    t = ray_table_add_col(t, ray_sym_intern("s", 1), s);
    ray_release(s);
    return t;
}

static bool sym_cells_eq(ray_t* x, ray_t* y, int64_t n) {
    if (!x || !y || RAY_IS_ERR(x) || RAY_IS_ERR(y) || x->len != n || y->len != n) return false;
    for (int64_t i = 0; i < n; i++) {
        ray_t* a = ray_sym_vec_cell(x, i); ray_t* b = ray_sym_vec_cell(y, i);
        if (!a || !b || ray_str_len(a) != ray_str_len(b) ||
            memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) != 0) return false;
    }
    return true;
}

/* Sources of different SYM id widths (an 8-bit and a 16-bit generation-2
 * column over two symfiles, and a plain 64-bit runtime column) through one
 * vocabulary merge into a third symfile: decode-free, ids correct. */
static test_result_t test_merge_mixed_widths(void) {
    const int64_t n = 3000;
    ray_t* t1 = sym_tbl("p", 2, n);
    ray_t* t2 = sym_tbl("k", 300, n);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t1, ray_test_tmp(CS_TMP "/m1"), ray_test_tmp(CS_TMP "/m1.sym"), RAY_COMPRESS_ON), RAY_OK);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t2, ray_test_tmp(CS_TMP "/m2"), ray_test_tmp(CS_TMP "/m2.sym"), RAY_COMPRESS_ON), RAY_OK);
    ray_t* l1 = ray_read_splayed(ray_test_tmp(CS_TMP "/m1"), ray_test_tmp(CS_TMP "/m1.sym"));
    ray_t* l2 = ray_read_splayed(ray_test_tmp(CS_TMP "/m2"), ray_test_tmp(CS_TMP "/m2.sym"));
    TEST_ASSERT_TRUE(l1 && !RAY_IS_ERR(l1) && l2 && !RAY_IS_ERR(l2));
    ray_t* e1 = ray_table_get_col_enc_idx(l1, 0);
    ray_t* e2 = ray_table_get_col_enc_idx(l2, 0);
    TEST_ASSERT_TRUE(e1 && e2);
    ray_col_meta_t m1, m2;
    TEST_ASSERT_TRUE(ray_col_meta_of(e1, &m1) && ray_col_meta_of(e2, &m2));
    TEST_ASSERT_EQ_I(m1.attrs & RAY_SYM_W_MASK, RAY_SYM_W8);
    TEST_ASSERT_EQ_I(m2.attrs & RAY_SYM_W_MASK, RAY_SYM_W16);
    ray_t* c3 = ray_table_col_idx(t2, 0);                  /* plain W64 runtime ids */
    ray_t* t = ray_table_new(3);
    t = ray_table_add_col(t, ray_sym_intern("a", 1), e1);
    t = ray_table_add_col(t, ray_sym_intern("b", 1), e2);
    t = ray_table_add_col(t, ray_sym_intern("c", 1), c3);
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    struct ray_sym_domain_s* d3 = ray_sym_domain_open_or_create(ray_test_tmp(CS_TMP "/m3.sym"));
    TEST_ASSERT_NOT_NULL(d3);
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(ray_splay_save_staged_bulk_mode(t, ray_test_tmp(CS_TMP "/m3"), ray_test_tmp(CS_TMP "/m3.sym"), RAY_COMPRESS_ABSENT), RAY_OK);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(d3, false), RAY_OK);
    ray_sym_domain_release(d3);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/m3/a")), 2);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/m3/b")), 2);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/m3/c")), 0);
    ray_t* l3 = ray_read_splayed(ray_test_tmp(CS_TMP "/m3"), ray_test_tmp(CS_TMP "/m3.sym"));
    TEST_ASSERT_TRUE(l3 && !RAY_IS_ERR(l3));
    TEST_ASSERT_TRUE(sym_cells_eq(ray_table_col_idx(l3, 0), ray_table_col_idx(t1, 0), n));
    TEST_ASSERT_TRUE(sym_cells_eq(ray_table_col_idx(l3, 1), ray_table_col_idx(t2, 0), n));
    TEST_ASSERT_TRUE(sym_cells_eq(ray_table_col_idx(l3, 2), ray_table_col_idx(t2, 0), n));
    ray_release(l3); ray_release(t);
    ray_release(l1); ray_release(l2); ray_release(t1); ray_release(t2);
    PASS();
}

static bool files_eq(const char* a, const char* b) {
    size_t na, nb;
    uint8_t* x = slurp(a, &na); uint8_t* y = slurp(b, &nb);
    bool eq = x && y && na == nb && memcmp(x, y, na) == 0;
    free(x); free(y);
    return eq;
}

/* Existing users: an absent option onto no destination and onto a
 * generation-0 destination writes exactly what the per-column generation-0
 * writers produce (the pre-resolution path). */
static test_result_t test_absent_byte_identity(void) {
    const int64_t n = 3000;
    ray_t* t = sym_tbl("v", 7, n);
    ray_t* a = i64_col(n, 3);
    ray_t* st = ray_vec_new(RAY_STR, n);
    for (int64_t i = 0; i < n; i++) {
        char b[48]; int k = snprintf(b, sizeof b, i % 3 ? "x%d" : "a long string value %d", (int)i);
        st = ray_str_vec_append(st, b, (size_t)k);
    }
    t = ray_table_add_col(t, ray_sym_intern("a", 1), a);
    t = ray_table_add_col(t, ray_sym_intern("t", 1), st);
    ray_release(a); ray_release(st);
    TEST_ASSERT_EQ_I(ray_splay_save(t, ray_test_tmp(CS_TMP "/bi"), ray_test_tmp(CS_TMP "/bi.sym")), RAY_OK);
    /* the reference: today's per-column writers */
    (void)ray_test_mkdir_p(ray_test_tmp(CS_TMP "/ref"));
    struct ray_sym_domain_s* dom = ray_sym_domain_open_or_create(ray_test_tmp(CS_TMP "/bi.sym"));
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_EQ_I(ray_col_save_sym_encoded(ray_table_col_idx(t, 0), ray_test_tmp(CS_TMP "/ref/s"), dom, true), RAY_OK);
    ray_sym_domain_release(dom);
    TEST_ASSERT_EQ_I(ray_col_save(ray_table_col_idx(t, 1), ray_test_tmp(CS_TMP "/ref/a")), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save(ray_table_col_idx(t, 2), ray_test_tmp(CS_TMP "/ref/t")), RAY_OK);
    ray_t* names = ray_vec_new(RAY_STR, 3);
    names = ray_str_vec_append(names, "s", 1);
    names = ray_str_vec_append(names, "a", 1);
    names = ray_str_vec_append(names, "t", 1);
    TEST_ASSERT_EQ_I(ray_col_save(names, ray_test_tmp(CS_TMP "/ref/.d")), RAY_OK);
    ray_release(names);
    const char* files[4] = { "s", "a", "t", ".d" };
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1)   /* again, onto the generation-0 destination */
            TEST_ASSERT_EQ_I(ray_splay_save(t, ray_test_tmp(CS_TMP "/bi"), ray_test_tmp(CS_TMP "/bi.sym")), RAY_OK);
        char cur[1024], p[1200], r[256];
        TEST_ASSERT_EQ_I(ray_splay_resolve_dir(ray_test_tmp(CS_TMP "/bi"), cur, sizeof cur), RAY_OK);
        for (int f = 0; f < 4; f++) {
            snprintf(p, sizeof p, "%s/%s", cur, files[f]);
            snprintf(r, sizeof r, ray_test_tmp(CS_TMP "/ref/%s"), files[f]);
            TEST_ASSERT_TRUE(files_eq(p, r));
        }
    }
    ray_release(t);
    PASS();
}

/* An absent option never drops inheritance silently: an unreadable
 * destination column or generation pointer fails the save, and nothing is
 * published.  A destination file in a header-less list format is
 * generation 0, not an error. */
static test_result_t test_unreadable_destination(void) {
    if (geteuid() == 0) SKIP("running as root: permissions are not enforced");
    const char* d = ray_test_tmp(CS_TMP "/ur");
    ray_t* t = plain_tbl(3000);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, RAY_COMPRESS_OFF), RAY_OK);
    char cur[1024], cur2[1024], p[1200];
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(d, cur, sizeof cur), RAY_OK);
    snprintf(p, sizeof p, "%s/a", cur);
    uint8_t g = 9;
    TEST_ASSERT_EQ_I(chmod(p, 0), 0);
    TEST_ASSERT_EQ_I(ray_col_file_gen_err(p, &g), RAY_ERR_IO);
    TEST_ASSERT_EQ_I(ray_splay_save(t, d, NULL), RAY_ERR_IO);
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(d, cur2, sizeof cur2), RAY_OK);
    TEST_ASSERT_TRUE(strcmp(cur2, cur) == 0);           /* nothing published */
    /* an explicit option does not read the destination */
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, RAY_COMPRESS_OFF), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 0);
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(d, cur, sizeof cur), RAY_OK);
    snprintf(p, sizeof p, "%s/a", cur);
    TEST_ASSERT_EQ_I(chmod(p, 0), 0);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, RAY_COMPRESS_ON), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 2);
    /* the generation pointer itself unreadable: an inheriting save fails,
     * explicit ones proceed as before */
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(d, cur, sizeof cur), RAY_OK);
    snprintf(p, sizeof p, "%s/.current", d);
    TEST_ASSERT_EQ_I(chmod(p, 0), 0);
    TEST_ASSERT_TRUE(ray_splay_save(t, d, NULL) != RAY_OK);
    TEST_ASSERT_EQ_I(chmod(p, 0644), 0);
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(d, cur2, sizeof cur2), RAY_OK);
    TEST_ASSERT_TRUE(strcmp(cur2, cur) == 0);           /* nothing published */
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 2);
    for (int m = RAY_COMPRESS_ON; m <= RAY_COMPRESS_OFF; m++) {
        TEST_ASSERT_EQ_I(chmod(p, 0), 0);
        TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, (ray_compress_t)m), RAY_OK);
        TEST_ASSERT_EQ_I(chmod(p, 0644), 0);
        TEST_ASSERT_EQ_I(col_gen_in(d, "a"), m == RAY_COMPRESS_ON ? 2 : 0);
    }
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(d, cur, sizeof cur), RAY_OK);
    /* a short (truncated) destination column is corrupt, not 0 */
    snprintf(p, sizeof p, "%s/b", cur);
    TEST_ASSERT_EQ_I(truncate(p, 10), 0);
    TEST_ASSERT_EQ_I(ray_col_file_gen_err(p, &g), RAY_ERR_CORRUPT);
    TEST_ASSERT_EQ_I(ray_col_file_gen_err(ray_test_tmp(CS_TMP "/nope"), &g), RAY_OK);
    TEST_ASSERT_EQ_I(g, 0);
    TEST_ASSERT_EQ_I(ray_splay_save(t, d, NULL), RAY_ERR_CORRUPT);
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(d, cur2, sizeof cur2), RAY_OK);
    TEST_ASSERT_TRUE(strcmp(cur2, cur) == 0);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, RAY_COMPRESS_OFF), RAY_OK);   /* explicit: replaced */
    TEST_ASSERT_EQ_I(col_gen_in(d, "b"), 0);
    ray_release(t);
    /* a string-list column file at the name of a now-plain column */
    ray_t* lt = ray_table_new(1);
    ray_t* lst = ray_list_new(3);
    for (int i = 0; i < 3; i++) { ray_t* x = ray_str("ab", 2); lst = ray_list_append(lst, x); ray_release(x); }
    lt = ray_table_add_col(lt, ray_sym_intern("a", 1), lst);
    ray_release(lst);
    TEST_ASSERT_EQ_I(ray_splay_save(lt, ray_test_tmp(CS_TMP "/ul"), NULL), RAY_OK);
    ray_release(lt);
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(ray_test_tmp(CS_TMP "/ul"), cur, sizeof cur), RAY_OK);
    snprintf(p, sizeof p, "%s/a", cur);
    TEST_ASSERT_EQ_I(ray_col_file_gen_err(p, &g), RAY_OK);
    TEST_ASSERT_EQ_I(g, 0);
    ray_t* pt = ray_table_new(1);
    ray_t* v = i64_col(3, 1);
    pt = ray_table_add_col(pt, ray_sym_intern("a", 1), v);
    ray_release(v);
    TEST_ASSERT_EQ_I(ray_splay_save(pt, ray_test_tmp(CS_TMP "/ul"), NULL), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/ul"), "a"), 0);
    ray_release(pt);
    PASS();
}

/* Evaluate and return the error code ("" for success), releasing the value. */
static const char* ev_code(const char* src) {
    static char code[16];
    ray_t* r = ray_eval_str(src);
    snprintf(code, sizeof code, "%s", (r && RAY_IS_ERR(r)) ? ray_err_code(r) : "");
    if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
    return code;
}

/* The user-visible values built from a generation-2 table are plain. */
static test_result_t test_user_values_are_plain(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['a 'b] (list [1 2 3] [4 5 6])))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/uv\" t {compress: true})")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u (.db.splayed.get \"" CS_TMP "/uv\"))")), "");
    ray_t* a = ray_eval_str("(at u 'a)");
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a) && a->type == RAY_I64 && a->len == 3);
    ray_release(a);
    ray_t* s = ray_eval_str("(format \"%\" u)");
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s));
    ray_release(s);
    PASS();
}

static test_result_t test_db_splayed_set_forms(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['a 'b] (list [1 2 3] [4 5 6])))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f1\" t)")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/f1"), "a"), 0);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f2\" t \"" CS_TMP "/f2.sym\")")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f3\" t {compress: true})")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/f3"), "a"), 2);
    /* absent over a generation-2 destination inherits it */
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f3\" t)")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/f3"), "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f3\" t \"" CS_TMP "/f3.sym\" {compress: false})")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/f3"), "a"), 0);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f4\" t {compress: true} {compress: true})")), "domain");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f4\" t {compress: 1})")), "domain");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f4\" t {other: true})")), "domain");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(sum (at (.db.splayed.get \"" CS_TMP "/f3\") 'b))")), "");
    /* round trip through a generation-2 destination */
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f5\" t {compress: true})")), "");
    ray_t* r = ray_eval_str(ray_test_tmp("(sum (at (.db.splayed.get \"" CS_TMP "/f5\") 'b))"));
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 15);
    ray_release(r);
    r = ray_eval_str(ray_test_tmp("(all (== (at (.db.splayed.get \"" CS_TMP "/f5\") 'a) [1 2 3]))"));
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == -RAY_BOOL && r->b8);
    ray_release(r);
    /* {} is absent; a null compress is a domain error */
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f6\" t {})")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/f6"), "a"), 0);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f6\" t {compress: null})")), "domain");
    /* an empty symfile path keeps its old meaning: no convention fallback */
    TEST_ASSERT_STR_EQ(ev_code("(set ts (table ['s] (list ['x 'y])))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/f7\" ts \"\")")), "domain");
    PASS();
}

static test_result_t test_csv_parted_option(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['a 'b] (list [1 2 3 4] [10 20 30 40])))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.write t \"" CS_TMP "/in.csv\")")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.parted {compress: true} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr\" 't)")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/pr/0/t"), "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.parted \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr0\" 't)")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/pr0/0/t"), "a"), 0);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.parted {compress: true} {compress: true} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr1\" 't)")), "domain");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.parted {zone: true} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr1\" 't)")), "domain");
    /* existing root: explicit modes and inheritance, typed + rows_per_part */
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.parted {compress: false} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr\" 't)")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/pr/0/t"), "a"), 0);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.parted {compress: true} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr\" 't)")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/pr/0/t"), "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.parted \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr\" 't)")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/pr/0/t"), "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.parted {compress: true} [I64 I64] \"" CS_TMP "/in.csv\" 2 \"" CS_TMP "/pt\" 't)")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/pt/1/t"), "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.parted {compress: null} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr1\" 't)")), "domain");
    ray_t* r = ray_eval_str(ray_test_tmp("(sum (at (.db.parted.get \"" CS_TMP "/pr\" 't) 'b))"));
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 100);
    ray_release(r);
    PASS();
}

/* ---- Task 17: generation-2 stream writer; .csv.splayed and Parquet ---- */

/* Streaming the same I64 values in two appends gives the resident bytes. */
static test_result_t test_stream_matches_resident(void) {
    int64_t n = 140000;
    ray_t* v = i64_col(n, 13);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(CS_TMP "/res"), 2), RAY_OK);
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, ray_test_tmp(CS_TMP), ray_sym_intern("str1", 4), RAY_I64, NULL, 2), RAY_OK);
    ray_t* p1 = ray_vec_new(RAY_I64, 70000); p1->len = 70000;
    ray_t* p2 = ray_vec_new(RAY_I64, n - 70000); p2->len = n - 70000;
    for (int64_t i = 0; i < 70000; i++) ((int64_t*)ray_data(p1))[i] = i * 13;
    for (int64_t i = 0; i < n - 70000; i++) ((int64_t*)ray_data(p2))[i] = (70000 + i) * 13;
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, p1), RAY_OK);   /* streams reject slices */
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, p2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    ray_col_stream_abort(&w);                                    /* always valid; nothing left */
    size_t na, nb; uint8_t* a = slurp(ray_test_tmp(CS_TMP "/res"), &na); uint8_t* b = slurp(ray_test_tmp(CS_TMP "/str1"), &nb);
    TEST_ASSERT_TRUE(a && b);
    TEST_ASSERT_TRUE(na == nb && memcmp(a, b, na) == 0);   /* no zone accumulator: identical */
    free(a); free(b);
    /* with the converters' zone accumulator the stream records CHUNK_ZONE,
     * as the generation-0 stream persists one */
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, ray_test_tmp(CS_TMP), ray_sym_intern("str2", 4), RAY_I64, NULL, 2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_index_begin(&w, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, p1), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, p2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    ray_col_stream_abort(&w);
    b = slurp(ray_test_tmp(CS_TMP "/str2"), &nb);
    TEST_ASSERT_TRUE(b && nb >= 96);
    ray_enc_hdr_t hb; memcpy(&hb, b + 32, 64);
    TEST_ASSERT_EQ_I(hb.index_kind, RAY_IDX_CHUNK_ZONE);
    free(b);
    ray_release(p1); ray_release(p2); ray_release(v);
    PASS();
}

/* STR rows [lo, hi): empties, short inline cells and repeating long cells. */
static ray_t* str_rows(int64_t lo, int64_t hi) {
    ray_t* v = ray_vec_new(RAY_STR, hi - lo);
    char b[96];
    for (int64_t i = lo; i < hi; i++) {
        int len;
        if (i % 7 == 0) len = 0;
        else if (i % 3 == 0) len = snprintf(b, sizeof b, "s%lld", (long long)(i % 50));
        else len = snprintf(b, sizeof b, "a much longer string value number %lld", (long long)(i % 1000));
        v = ray_str_vec_append(v, b, (size_t)len);
    }
    return v;
}

/* Same bytes from offset `from` on. */
static bool same_file_from(const char* x, const char* y, size_t from) {
    size_t na, nb; uint8_t* a = slurp(x, &na); uint8_t* b = slurp(y, &nb);
    bool ok = a && b && na == nb && na >= from && memcmp(a + from, b + from, na - from) == 0;
    free(a); free(b);
    return ok;
}

/* A STR column across several chunks, streamed in three appends (each its
 * own pool) gives the resident generation-2 bytes; generation 0 streams
 * stay today's bytes (the resident generation-0 writer's). */
static test_result_t test_stream_str_matches_resident(void) {
    int64_t n = 200000, cut[4] = { 0, 50000, 130001, 200000 };
    ray_t* v = str_rows(0, n);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, ray_test_tmp(CS_TMP "/sres"), 2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(v, ray_test_tmp(CS_TMP "/sres0")), RAY_OK);
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, ray_test_tmp(CS_TMP), ray_sym_intern("sst2", 4), RAY_STR, NULL, 2), RAY_OK);
    for (int k = 0; k < 3; k++) {
        ray_t* p = str_rows(cut[k], cut[k + 1]);
        TEST_ASSERT_EQ_I(ray_col_stream_append(&w, p), RAY_OK);
        ray_release(p);
    }
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    ray_col_stream_abort(&w);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/sst2")), 2);
    TEST_ASSERT_TRUE(same_file_from(ray_test_tmp(CS_TMP "/sres"), ray_test_tmp(CS_TMP "/sst2"), 0));
    /* generation 0, one append: the resident generation-0 bytes */
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, ray_test_tmp(CS_TMP), ray_sym_intern("sst0", 4), RAY_STR, NULL, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, v), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    ray_col_stream_abort(&w);
    TEST_ASSERT_TRUE(same_file_from(ray_test_tmp(CS_TMP "/sres0"), ray_test_tmp(CS_TMP "/sst0"), 0));
    /* the generation-2 file loads back to the same cells */
    ray_t* back = ray_col_load(ray_test_tmp(CS_TMP "/sst2"));
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back) && back->len == n);
    for (int64_t i = 0; i < n; i += 997) {
        size_t la, lb; const char* a = ray_str_vec_get(v, i, &la); const char* b = ray_str_vec_get(back, i, &lb);
        TEST_ASSERT_TRUE(la == lb && (la == 0 || memcmp(a, b, la) == 0));
    }
    ray_release(back);
    /* I64 generation 0 streamed in two appends: the resident bytes */
    ray_t* iv = i64_col(140000, 13);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(iv, ray_test_tmp(CS_TMP "/ires0")), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, ray_test_tmp(CS_TMP), ray_sym_intern("ist0", 4), RAY_I64, NULL, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, iv), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    ray_col_stream_abort(&w);
    TEST_ASSERT_TRUE(same_file_from(ray_test_tmp(CS_TMP "/ires0"), ray_test_tmp(CS_TMP "/ist0"), 0));
    /* any other generation is refused before a file exists */
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, ray_test_tmp(CS_TMP), ray_sym_intern("bad", 3), RAY_I64, NULL, 1), RAY_ERR_VERSION);
    ray_col_stream_abort(&w);
    ray_release(iv); ray_release(v);
    PASS();
}

/* SYM rows [lo, hi) as strings: a null ("") every 11th row. */
static ray_t* sym_text_rows(int64_t lo, int64_t hi) {
    ray_t* v = ray_vec_new(RAY_STR, hi - lo);
    char b[32];
    for (int64_t i = lo; i < hi; i++) {
        int len = i % 11 == 0 ? 0 : snprintf(b, sizeof b, "v%lld", (long long)(i % 300));
        v = ray_str_vec_append(v, b, (size_t)len);
    }
    return v;
}

static ray_err_t stream_sym(struct ray_sym_domain_s* d, const char* name, uint8_t gen, int64_t n) {
    ray_col_stream_t w;
    ray_err_t e = ray_col_stream_open(&w, ray_test_tmp(CS_TMP), ray_sym_intern(name, strlen(name)), RAY_SYM, d, gen);
    int64_t cut[4] = { 0, n / 3, n / 3 * 2 + 1, n };
    for (int k = 0; e == RAY_OK && k < 3; k++) {
        ray_t* p = sym_text_rows(cut[k], cut[k + 1]);
        e = ray_col_stream_append(&w, p);
        ray_release(p);
    }
    if (e == RAY_OK) e = ray_col_stream_close(&w, false);
    ray_col_stream_abort(&w);
    return e;
}

/* A generation-2 SYM stream (three chunks, null ids) loads back to the
 * generation-0 stream of the same values; its outer header keeps the
 * symfile count in rc, the W32 width it was opened with and HAS_NULLS. */
static test_result_t test_stream_sym_gen2(void) {
    struct ray_sym_domain_s* d = ray_sym_domain_open_or_create(ray_test_tmp(CS_TMP "/s.sym"));
    TEST_ASSERT_TRUE(d != NULL);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(d, "", 0), 0);
    int64_t n = 140000;
    TEST_ASSERT_EQ_I(stream_sym(d, "sy0", 0, n), RAY_OK);
    TEST_ASSERT_EQ_I(stream_sym(d, "sy2", 2, n), RAY_OK);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(d, false), RAY_OK);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/sy0")), 0);
    TEST_ASSERT_EQ_I(file_gen(ray_test_tmp(CS_TMP "/sy2")), 2);
    size_t nb; uint8_t* b = slurp(ray_test_tmp(CS_TMP "/sy2"), &nb);
    TEST_ASSERT_TRUE(b && nb >= 32);
    ray_t hdr; memcpy(&hdr, b, 32); free(b);
    TEST_ASSERT_EQ_I(hdr.type, RAY_SYM);
    TEST_ASSERT_EQ_I((int64_t)hdr.rc, ray_sym_domain_count(d));
    TEST_ASSERT_EQ_I(hdr.attrs & RAY_SYM_W_MASK, RAY_SYM_W32);
    TEST_ASSERT_TRUE(hdr.attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_EQ_I(hdr.len, n);
    ray_t* a = ray_col_load_dom(ray_test_tmp(CS_TMP "/sy0"), d);
    ray_t* c = ray_col_load_dom(ray_test_tmp(CS_TMP "/sy2"), d);
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a) && c && !RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(a->len, n); TEST_ASSERT_EQ_I(c->len, n);
    TEST_ASSERT_TRUE(c->attrs & RAY_ATTR_HAS_NULLS);
    int64_t nulls = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* s0 = ray_sym_vec_cell(a, i); ray_t* s1 = ray_sym_vec_cell(c, i);
        TEST_ASSERT_TRUE(s0 && s1 && ray_str_len(s0) == ray_str_len(s1) &&
                         memcmp(ray_str_ptr(s0), ray_str_ptr(s1), ray_str_len(s0)) == 0);
        if (!ray_str_len(s1)) nulls++;
    }
    TEST_ASSERT_EQ_I(nulls, (n + 10) / 11);
    ray_release(a); ray_release(c);
    ray_sym_domain_release(d);
    PASS();
}

/* A generation-2 transcode that cannot write its output (RLIMIT_FSIZE at
 * the raw staging size, so only the encoded file overflows) fails the close
 * and leaves nothing: no column file, no .stream-* staging or .g2 file. */
static bool cs_dir_has_stream_tmp(const char* dir) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "ls -A %s | grep -q '^\\.stream-'", dir);
    return system(cmd) == 0;
}
static test_result_t test_stream_gen2_transcode_failure(void) {
#ifdef RAY_OS_LINUX
    int64_t n = 140000;
    ray_t* v = ray_vec_new(RAY_I64, n); v->len = n;
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (int64_t i = 0; i < n; i++) {                 /* incompressible: RAW vectors */
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        ((int64_t*)ray_data(v))[i] = (int64_t)(x & 0x7FFFFFFFFFFFFFFFull);
    }
    pid_t pid = fork(); TEST_ASSERT_TRUE(pid >= 0);
    if (!pid) {
        ray_col_stream_t w;
        struct rlimit limit;
        if (getrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        limit.rlim_cur = (rlim_t)(32 + n * 8);          /* the raw staging file fits exactly */
        if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR || setrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        if (ray_col_stream_open(&w, ray_test_tmp(CS_TMP), ray_sym_intern("tf", 2), RAY_I64, NULL, 2) != RAY_OK) _exit(2);
        if (ray_col_stream_append(&w, v) != RAY_OK) _exit(2);
        ray_err_t e = ray_col_stream_close(&w, false);
        ray_col_stream_abort(&w);
        _exit(e == RAY_ERR_IO ? 0 : 1);
    }
    int status;
    TEST_ASSERT_EQ_I(waitpid(pid, &status, 0), pid);
    TEST_ASSERT_TRUE(WIFEXITED(status)); TEST_ASSERT_EQ_I(WEXITSTATUS(status), 0);
    TEST_ASSERT_TRUE(access(ray_test_tmp(CS_TMP "/tf"), F_OK) != 0);
    TEST_ASSERT_FALSE(cs_dir_has_stream_tmp(ray_test_tmp(CS_TMP)));
    ray_release(v);
    PASS();
#else
    SKIP("requires Linux RLIMIT_FSIZE fault injection");
#endif
}

/* The resident generation-0 STR writer used to copy the runtime str_pool
 * pointer (aux bytes 8..15) into the file when HAS_NULLS was set: two saves
 * of equal vectors now give identical bytes, with those bytes zero. */
static test_result_t test_str_header_no_pointer(void) {
    ray_t* a = str_rows(0, 5000);
    ray_t* b = str_rows(0, 5000);
    TEST_ASSERT_TRUE(a->attrs & RAY_ATTR_HAS_NULLS);   /* the case that kept aux */
    TEST_ASSERT_EQ_I(ray_col_save(a, ray_test_tmp(CS_TMP "/h1")), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save(b, ray_test_tmp(CS_TMP "/h2")), RAY_OK);
    TEST_ASSERT_TRUE(same_file_from(ray_test_tmp(CS_TMP "/h1"), ray_test_tmp(CS_TMP "/h2"), 0));
    size_t n; uint8_t* p = slurp(ray_test_tmp(CS_TMP "/h1"), &n);
    TEST_ASSERT_TRUE(p && n >= 32);
    static const uint8_t zero[8];
    TEST_ASSERT_TRUE(memcmp(p + 8, zero, 8) == 0);
    free(p);
    ray_t* back = ray_col_load(ray_test_tmp(CS_TMP "/h1"));
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back) && back->len == 5000);
    for (int64_t i = 0; i < 5000; i++) {
        size_t la, lb; const char* x = ray_str_vec_get(a, i, &la); const char* y = ray_str_vec_get(back, i, &lb);
        TEST_ASSERT_TRUE(la == lb && (la == 0 || memcmp(x, y, la) == 0));
    }
    ray_release(back); ray_release(a); ray_release(b);
    PASS();
}

/* Parquet: one options dict per call. */
static test_result_t test_parquet_two_dicts(void) {
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.parquet.splayed {compress: true} {compress: true} \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/p2d\")")), "domain");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.parquet.parted {compress: true} {rows: 2} \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/p2p\" 't)")), "domain");
    PASS();
}

static test_result_t test_csv_splayed_option(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['a 's] (list [1 2 3 4] [\"alpha\" \"a much longer value\" \"\" \"x\"])))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.write t \"" CS_TMP "/in.csv\")")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.splayed {compress: true} [I64 STR] \"" CS_TMP "/in.csv\" \"" CS_TMP "/cs\")")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/cs"), "a"), 2);
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/cs"), "s"), 2);
    ray_t* r = ray_eval_str(ray_test_tmp("(at (.db.splayed.get \"" CS_TMP "/cs\") 's)"));
    ray_t* want = ray_eval_str("[\"alpha\" \"a much longer value\" \"\" \"x\"]");
    ray_t* fa = ray_fmt(r, 0); ray_t* fb = ray_fmt(want, 0);
    TEST_ASSERT_TRUE(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0);
    ray_release(fa); ray_release(fb); ray_release(r); ray_release(want);
    /* absent over the generation-2 destination inherits it */
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.splayed [I64 STR] \"" CS_TMP "/in.csv\" \"" CS_TMP "/cs\")")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/cs"), "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.splayed {compress: true} {compress: true} \"" CS_TMP "/in.csv\" \"" CS_TMP "/cs2\")")), "domain");
    PASS();
}

/* .csv.splayed {compress: true} with a SYMBOL column (nulls, several
 * chunks) loads back equal to the generation-0 import. */
static test_result_t test_csv_splayed_symbol(void) {
    FILE* f = fopen(ray_test_tmp(CS_TMP "/sy.csv"), "w");
    TEST_ASSERT_TRUE(f != NULL);
    fprintf(f, "a,s\n");
    for (int64_t i = 0; i < 140000; i++) {
        if (i % 13 == 0) fprintf(f, "%lld,\n", (long long)i);
        else fprintf(f, "%lld,k%lld\n", (long long)i, (long long)(i % 400));
    }
    fclose(f);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.splayed {compress: true} [I64 SYMBOL] \"" CS_TMP "/sy.csv\" \"" CS_TMP "/sy2\")")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.csv.splayed {compress: false} [I64 SYMBOL] \"" CS_TMP "/sy.csv\" \"" CS_TMP "/sy0\")")), "");
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/sy2"), "s"), 2);
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/sy2"), "a"), 2);
    TEST_ASSERT_EQ_I(col_gen_in(ray_test_tmp(CS_TMP "/sy0"), "s"), 0);
    ray_t* x = ray_eval_str(ray_test_tmp("(at (.db.splayed.get \"" CS_TMP "/sy2\") 's)"));
    ray_t* y = ray_eval_str(ray_test_tmp("(at (.db.splayed.get \"" CS_TMP "/sy0\") 's)"));
    TEST_ASSERT_TRUE(x && !RAY_IS_ERR(x) && y && !RAY_IS_ERR(y));
    TEST_ASSERT_EQ_I(x->len, 140000); TEST_ASSERT_EQ_I(y->len, 140000);
    int64_t nulls = 0;
    for (int64_t i = 0; i < 140000; i++) {
        ray_t* s0 = ray_sym_vec_cell(y, i); ray_t* s1 = ray_sym_vec_cell(x, i);
        TEST_ASSERT_TRUE(s0 && s1 && ray_str_len(s0) == ray_str_len(s1) &&
                         memcmp(ray_str_ptr(s0), ray_str_ptr(s1), ray_str_len(s0)) == 0);
        if (!ray_str_len(s1)) nulls++;
    }
    TEST_ASSERT_EQ_I(nulls, (140000 + 12) / 13);
    ray_release(x); ray_release(y);
    PASS();
}

static test_result_t test_parquet_option(void) {
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.parquet.splayed {compress: true} \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/pq\")")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.parquet.splayed \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/pq0\")")), "");
    ray_t* a = ray_eval_str(ray_test_tmp("(.db.splayed.get \"" CS_TMP "/pq\")"));
    ray_t* b = ray_eval_str(ray_test_tmp("(.db.splayed.get \"" CS_TMP "/pq0\")"));
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a) && b && !RAY_IS_ERR(b));
    for (int64_t c = 0; c < ray_table_ncols(a); c++) {
        ray_t* nm = ray_sym_str(ray_table_col_name(a, c));
        char cn[256]; snprintf(cn, sizeof cn, "%.*s", (int)ray_str_len(nm), ray_str_ptr(nm));
        ray_col_meta_t m; ray_table_col_meta_idx(a, c, &m);
        uint8_t want = (m.type >= RAY_BOOL && m.type <= RAY_STR) ? 2 : 0;
        TEST_ASSERT_FMT(col_gen_in(ray_test_tmp(CS_TMP "/pq"), cn) == want, "column %s", cn);
        TEST_ASSERT_FMT(col_gen_in(ray_test_tmp(CS_TMP "/pq0"), cn) == 0, "column %s (absent)", cn);
    }
    ray_t* fa = ray_fmt(a, 0); ray_t* fb = ray_fmt(b, 0);
    TEST_ASSERT_TRUE(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0);
    ray_release(fa); ray_release(fb); ray_release(a); ray_release(b);
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.parquet.parted {compress: true rows: 2} \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/pqp\" 't)")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.parquet.splayed {compress: 1} \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/pq2\")")), "domain");
    PASS();
}

/* A loaded table whose three columns are generation 2. */
static ray_t* gen2_tbl3(void) {
    ray_t* t = ray_table_new(3);
    ray_t* k = i64_col(5000, 1);
    ray_t* v = ray_vec_new(RAY_F64, 5000); v->len = 5000;
    for (int i = 0; i < 5000; i++) ((double*)ray_data(v))[i] = i * 0.5;
    ray_t* s = ray_vec_new(RAY_STR, 5000);
    for (int i = 0; i < 5000; i++) s = ray_str_vec_append(s, (i & 1) ? "odd value text" : "ev", (i & 1) ? 14 : 2);
    t = ray_table_add_col(t, ray_sym_intern("k", 1), k);
    t = ray_table_add_col(t, ray_sym_intern("v", 1), v);
    t = ray_table_add_col(t, ray_sym_intern("s", 1), s);
    ray_release(k); ray_release(v); ray_release(s);
    ray_err_t e = ray_splay_save_mode(t, ray_test_tmp(CS_TMP "/plan"), NULL, RAY_COMPRESS_ON);
    ray_release(t);
    if (e != RAY_OK) return ray_error("io", "save failed");
    return ray_read_splayed(ray_test_tmp(CS_TMP "/plan"), NULL);
}

static test_result_t test_planning_never_materializes(void) {
    ray_t* t = gen2_tbl3();
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_enc_counters_reset();
    ray_graph_t* g = ray_graph_new(t);
    ray_op_t* keys[] = { ray_scan(g, "k") };
    ray_op_t* val = ray_scan(g, "v");
    ray_op_t* ins[] = { val };
    uint16_t ops[] = { OP_SUM };
    ray_op_t* grp = ray_group(g, keys, 1, ops, ins, 1);
    (void)agg_v2_admission(g, grp, t);
    ray_op_t* p = ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 5));
    (void)ray_optimize(g, p);
    ray_t* e1 = ray_parse("(> k 5)");
    ray_t* e2 = ray_parse("(like s \"odd*\")");
    ray_t* e3 = ray_parse("(in k [1 2 3])");
    (void)ray_fused_group_supported(e1, t);
    (void)ray_fused_group_supported(e2, t);
    (void)ray_fused_group_supported(e3, t);
    ray_enc_counters_t c; ray_enc_counters_get(&c);
    TEST_ASSERT_EQ_U(c.materialized, 0);
    TEST_ASSERT_EQ_U(c.decoded_bytes, 0);
    ray_release(e1); ray_release(e2); ray_release(e3);
    ray_graph_free(g);
    ray_release(t);
    PASS();
}

static test_result_t test_exec_start_snapshot(void) {
    ray_t* t = gen2_tbl3();
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_enc_counters_reset();
    ray_enc_counters_t c; ray_enc_counters_get(&c);
    TEST_ASSERT_EQ_U(c.at_first_exec, UINT64_MAX);
    ray_graph_t* g = ray_graph_new(t);
    ray_op_t* s = ray_scan(g, "k");
    ray_t* out = ray_execute(g, s);
    TEST_ASSERT_TRUE(out && !RAY_IS_ERR(out));
    ray_enc_counters_get(&c);
    TEST_ASSERT_EQ_U(c.at_first_exec, 0);         /* nothing before execution */
    ray_release(out); ray_graph_free(g); ray_release(t);
    PASS();
}

/* first (asc v) over a parted column with a null must not become OP_MIN
 * (MIN skips nulls; asc puts them first).  The wrapper's own attrs are
 * always 0, so the check reads the segments' null flags. */
static int first_asc_stays(ray_t* t, const char* col) {
    ray_graph_t* g = ray_graph_new(t);
    ray_op_t* f = ray_first(g, ray_asc_op(g, ray_scan(g, col)));
    ray_op_t* r = ray_optimize(g, f);
    int stays = r && r->opcode != OP_MIN;
    ray_graph_free(g);
    return stays;
}

static test_result_t test_first_asc_parted_nulls_gen0(void) {
    ray_t* s0 = i64_col(5, 1); ray_t* s1 = i64_col(5, 1);
    ray_vec_set_null(s1, 2, true);
    ray_t* w = ray_alloc(2 * sizeof(ray_t*));
    w->type = RAY_PARTED_BASE + RAY_I64; w->len = 2; w->attrs = 0;
    memset(w->aux, 0, 16);
    ((ray_t**)ray_data(w))[0] = s0; ((ray_t**)ray_data(w))[1] = s1;
    ray_t* t = ray_table_new(1);
    t = ray_table_add_col(t, ray_sym_intern("p", 1), w);
    ray_release(w);
    TEST_ASSERT_TRUE(first_asc_stays(t, "p"));
    /* without a null the rewrite is still taken */
    ray_vec_set_null(s1, 2, false);
    s1->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    TEST_ASSERT_FALSE(first_asc_stays(t, "p"));
    ray_release(t);
    PASS();
}

/* A text segment: SYM ids or STR cells; `null_at` >= 0 holds the null (id
 * 0, the empty string) there. */
static ray_t* text_seg(int8_t ty, int64_t n, int64_t null_at) {
    static const char* const w[3] = { "aa", "bb", "cc" };
    if (ty == RAY_SYM) {
        ray_t* v = ray_sym_vec_new(RAY_SYM_W64, n); v->len = n;
        for (int64_t i = 0; i < n; i++)
            ((int64_t*)ray_data(v))[i] = i == null_at ? 0 : ray_sym_intern(w[i % 3], 2);
        return v;
    }
    ray_t* v = ray_vec_new(RAY_STR, n);
    for (int64_t i = 0; i < n; i++)
        v = i == null_at ? ray_str_vec_append(v, "", 0) : ray_str_vec_append(v, w[i % 3], 2);
    /* the in-band null alone: appending "" sets the flag, which a segment
     * need not carry (SYM ids never do), and the flag would answer for it */
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    return v;
}

/* (first (asc p)) takes the same route over a parted SYM/STR column as over
 * the flat column with the same cells: text nulls are in-band (no flag), so
 * the parted check reads each plain segment's payload. */
static test_result_t test_first_asc_parted_text_nulls(void) {
    static const int8_t tys[2] = { RAY_SYM, RAY_STR };
    for (int ti = 0; ti < 2; ti++) {
        for (int with_null = 0; with_null < 2; with_null++) {
            ray_t* s0 = text_seg(tys[ti], 5, -1);
            ray_t* s1 = text_seg(tys[ti], 5, with_null ? 2 : -1);
            ray_t* w = ray_alloc(2 * sizeof(ray_t*));
            w->type = RAY_PARTED_BASE + tys[ti]; w->len = 2; w->attrs = 0;
            memset(w->aux, 0, 16);
            ((ray_t**)ray_data(w))[0] = s0; ((ray_t**)ray_data(w))[1] = s1;
            ray_t* t = ray_table_new(1);
            t = ray_table_add_col(t, ray_sym_intern("p", 1), w);
            ray_release(w);
            ray_t* fl = text_seg(tys[ti], 10, with_null ? 7 : -1);   /* the same cells, flat */
            ray_t* tf = ray_table_new(1);
            tf = ray_table_add_col(tf, ray_sym_intern("p", 1), fl);
            ray_release(fl);
            bool has = ray_table_col_has_nulls(t, ray_sym_intern("p", 1));
            /* the answer is cached on the wrapper: later calls scan nothing */
            bool cached = (ray_table_slot(t, ray_sym_intern("p", 1))->aux[0] & 0x01) != 0;
            bool again = ray_table_col_has_nulls(t, ray_sym_intern("p", 1));
            int parted = first_asc_stays(t, "p"), flat = first_asc_stays(tf, "p");
            ray_release(t); ray_release(tf);
            TEST_ASSERT_FMT(has == (bool)with_null, "type %d null %d: has_nulls %d", tys[ti], with_null, has);
            TEST_ASSERT_FMT(cached && again == has, "type %d null %d: cached %d", tys[ti], with_null, cached);
            TEST_ASSERT_FMT(parted == flat, "type %d null %d: parted route %d, flat %d", tys[ti], with_null, parted, flat);
        }
    }
    PASS();
}

/* A parted STR column whose generation-0 segment holds the null beside a
 * generation-2 segment: the encoded wrapper reports it too. */
static void write_str_part(const char* day, int gen, bool with_null) {
    char dir[256], p[300];
    snprintf(dir, sizeof dir, "%s/pdt/%s/tb", ray_test_tmp(CS_TMP), day);
    (void)ray_test_mkdir_p(dir);
    ray_t* a = text_seg(RAY_STR, 3000, with_null ? 7 : -1);
    snprintf(p, sizeof p, "%s/a", dir); ray_col_save_gen(a, p, gen);
    ray_release(a);
    ray_t* names = ray_vec_new(RAY_STR, 1); names = ray_str_vec_append(names, "a", 1);
    snprintf(p, sizeof p, "%s/.d", dir); ray_col_save(names, p); ray_release(names);
}

static test_result_t test_parted_text_nulls_mixed_gen(void) {
    for (int pass = 0; pass < 2; pass++) {
        (void)ray_test_rm_rf(ray_test_tmp(CS_TMP "/pdt"));
        write_str_part("2024.01.01", 0, pass == 0);
        write_str_part("2024.01.02", 2, false);
        ray_t* t = ray_read_parted(ray_test_tmp(CS_TMP "/pdt"), "tb");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        TEST_ASSERT_TRUE(ray_table_get_col_enc(t, ray_sym_intern("a", 1)) != NULL);   /* the encoded wrapper */
        bool has = ray_table_col_has_nulls(t, ray_sym_intern("a", 1));
        ray_release(t);
        TEST_ASSERT_EQ_I(has, pass == 0);
    }
    PASS();
}

static void write_null_part(const char* day, int gen, bool with_null) {
    char dir[256], p[300];
    snprintf(dir, sizeof dir, "%s/pdb/%s/tb", ray_test_tmp(CS_TMP), day);
    (void)ray_test_mkdir_p(dir);
    ray_t* a = i64_col(3000, 3);
    if (with_null) ray_vec_set_null(a, 7, true);
    snprintf(p, sizeof p, "%s/a", dir); ray_col_save_gen(a, p, gen);
    ray_release(a);
    ray_t* names = ray_vec_new(RAY_STR, 1); names = ray_str_vec_append(names, "a", 1);
    snprintf(p, sizeof p, "%s/.d", dir); ray_col_save(names, p); ray_release(names);
}

static test_result_t test_first_asc_parted_nulls_gen2(void) {
    for (int pass = 0; pass < 2; pass++) {
        (void)ray_test_rm_rf(ray_test_tmp(CS_TMP "/pdb"));
        write_null_part("2024.01.01", 2, pass == 0);
        write_null_part("2024.01.02", 2, false);
        ray_t* t = ray_read_parted(ray_test_tmp(CS_TMP "/pdb"), "tb");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        TEST_ASSERT_EQ_I(first_asc_stays(t, "a"), pass == 0);
        ray_release(t);
    }
    PASS();
}

/* Binding never decodes; literal translation reads the bound domain; the
 * plain column is produced only on request; the graph releases its binds. */
static test_result_t test_bind_stage(void) {
    ray_t* t = ray_table_new(1);
    ray_t* s = ray_sym_vec_new(RAY_SYM_W64, 3000); s->len = 3000;
    int64_t ids[3] = { ray_sym_intern("x", 1), ray_sym_intern("y", 1), ray_sym_intern("z", 1) };
    for (int i = 0; i < 3000; i++) ((int64_t*)ray_data(s))[i] = ids[i % 3];
    t = ray_table_add_col(t, ray_sym_intern("s", 1), s);
    ray_release(s);
    ray_err_t se = ray_splay_save_mode(t, ray_test_tmp(CS_TMP "/bd"), ray_test_tmp(CS_TMP "/bd.sym"), RAY_COMPRESS_ON);
    ray_release(t);
    TEST_ASSERT_EQ_I(se, RAY_OK);
    t = ray_read_splayed(ray_test_tmp(CS_TMP "/bd"), ray_test_tmp(CS_TMP "/bd.sym"));
    ray_enc_counters_reset();
    ray_graph_t* g = ray_graph_new(t);
    ray_t* err = NULL;
    ray_col_src_t* a = ray_graph_bind_col(g, t, ray_sym_intern("s", 1), &err);
    bool dedup = a && ray_graph_bind_col(g, t, ray_sym_intern("s", 1), &err) == a;
    bool absent = ray_graph_bind_col(g, t, ray_sym_intern("nope", 4), &err) == NULL && !err;
    int64_t py = -1, pabs = 0, dc = 0;
    bool file_dom = false, ty_ok = false;
    if (a) {
        ty_ok = a->type == RAY_SYM;
        file_dom = a->dom != ray_sym_runtime_domain();
        py = ray_col_src_sym_find(a, "y", 1);
        pabs = ray_col_src_sym_find(a, "absent", 6);
        dc = ray_col_src_dict_code(a, "y", 1);
    }
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    uint64_t mat_before = k.materialized;
    ray_t* plain = NULL;
    ray_t* perr = a ? ray_col_src_plain(a, &plain) : NULL;
    int64_t cell1 = (plain && plain->type == RAY_SYM) ? ray_read_sym(ray_data(plain), 1, RAY_SYM, plain->attrs) : -9;
    ray_enc_counters_get(&k);
    uint32_t nb = ray_graph_binds_count(g);
    ray_graph_free(g);                                  /* releases the bind (ASan) */
    ray_release(t);
    TEST_ASSERT_TRUE(a && !err && dedup && absent && ty_ok);
    TEST_ASSERT_TRUE(file_dom);                         /* the file domain */
    TEST_ASSERT_TRUE(py > 0);
    TEST_ASSERT_EQ_I(pabs, -1);
    TEST_ASSERT_EQ_I(dc, -2);
    TEST_ASSERT_EQ_U(mat_before, 0);                    /* bind and literal translation never decode */
    TEST_ASSERT_NULL(perr);
    TEST_ASSERT_EQ_I(cell1, py);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    TEST_ASSERT_EQ_I(nb, 1);
    PASS();
}


/* A bind is keyed by the table's address and name; a column replaced under the
 * same table (or a table released and reallocated at that address) must not
 * return the old binding.  The replacement is made in the slot list itself. */
static test_result_t test_bind_dedup_validates(void) {
    int64_t sa = ray_sym_intern("a", 1);
    ray_t* c1 = ray_vec_new(RAY_I64, 4); c1->len = 4;
    for (int i = 0; i < 4; i++) ((int64_t*)ray_data(c1))[i] = i;
    ray_t* t = ray_table_new(1);
    t = ray_table_add_col(t, sa, c1);
    ray_release(c1);
    ray_graph_t* g = ray_graph_new(t);
    ray_t* err = NULL;
    ray_col_src_t* s1 = ray_graph_bind_col(g, t, sa, &err);
    TEST_ASSERT_TRUE(s1 && !err);
    TEST_ASSERT_TRUE(ray_graph_bind_col(g, t, sa, &err) == s1);       /* same column: the same binding */
    ray_t* o1 = s1->obj;
    ray_t* c2 = ray_vec_new(RAY_I64, 4); c2->len = 4;
    for (int i = 0; i < 4; i++) ((int64_t*)ray_data(c2))[i] = 100 + i;
    ray_t* cols = ((ray_t**)ray_data(t))[1];
    ray_t* old = ((ray_t**)ray_data(cols))[0];
    ((ray_t**)ray_data(cols))[0] = c2;                                 /* takes the reference */
    ray_release(old);
    ray_col_src_t* s2 = ray_graph_bind_col(g, t, sa, &err);
    TEST_ASSERT_TRUE(s2 && !err && s2 != s1);
    ray_t* plain = NULL;
    TEST_ASSERT_NULL(ray_col_src_plain(s2, &plain));
    TEST_ASSERT_TRUE(s2->obj != o1 && s2->obj == c2 && ((int64_t*)ray_data(plain))[0] == 100);
    TEST_ASSERT_EQ_I(ray_graph_binds_count(g), 2);
    ray_graph_free(g);
    ray_release(t);
    PASS();
}

/* The predicate fixture: 3000 rows of row id, a SYM, an I64 and an F64 with
 * nulls, saved at generation 0 and 2 under CS_TMP "/<name>0" and "2". */
enum { FQ_N = 3000 };
static ray_t* fq_table(void) {
    ray_t* t = ray_table_new(5);
    ray_t* w = ray_vec_new(RAY_I64, FQ_N); w->len = FQ_N;
    for (int i = 0; i < FQ_N; i++) ((int64_t*)ray_data(w))[i] = i % 13;
    ray_t* id = ray_vec_new(RAY_I64, FQ_N); id->len = FQ_N;
    ray_t* s = ray_sym_vec_new(RAY_SYM_W64, FQ_N); s->len = FQ_N;
    ray_t* v = ray_vec_new(RAY_I64, FQ_N); v->len = FQ_N;
    ray_t* d = ray_vec_new(RAY_F64, FQ_N); d->len = FQ_N;
    const char* names[4] = { "alpha", "beta", "gamma", "alps" };
    for (int i = 0; i < FQ_N; i++) {
        ((int64_t*)ray_data(id))[i] = i;
        ((int64_t*)ray_data(s))[i] = ray_sym_intern(names[i % 4], strlen(names[i % 4]));
        ((int64_t*)ray_data(v))[i] = i % 17;
        ((double*)ray_data(d))[i] = (double)(i % 11) * 0.5;
    }
    for (int i = 0; i < FQ_N; i += 7) { ray_vec_set_null(v, i, true); ray_vec_set_null(d, i, true); }
    t = ray_table_add_col(t, ray_sym_intern("i", 1), id);
    t = ray_table_add_col(t, ray_sym_intern("s", 1), s);
    t = ray_table_add_col(t, ray_sym_intern("v", 1), v);
    t = ray_table_add_col(t, ray_sym_intern("d", 1), d);
    t = ray_table_add_col(t, ray_sym_intern("w", 1), w);
    ray_release(id); ray_release(s); ray_release(v); ray_release(d); ray_release(w);
    return t;
}

static bool fq_save(const char* name) {
    char d0[128], d2[128], s0[136], s2[136];
    snprintf(d0, sizeof d0, "%s/%s0", ray_test_tmp(CS_TMP), name); snprintf(s0, sizeof s0, "%s.sym", d0);
    snprintf(d2, sizeof d2, "%s/%s2", ray_test_tmp(CS_TMP), name); snprintf(s2, sizeof s2, "%s.sym", d2);
    ray_t* t = fq_table();
    bool ok = ray_splay_save_mode(t, d0, s0, RAY_COMPRESS_OFF) == RAY_OK &&
              ray_splay_save_mode(t, d2, s2, RAY_COMPRESS_ON) == RAY_OK;
    ray_release(t);
    return ok;
}

static ray_t* fq_run(const char* name, int gen, const char* where, const char* tail) {
    char q[512];
    snprintf(q, sizeof q, "(select {i:i v:v d:d from: (.db.splayed.get \"%s/%s%d\" \"%s/%s%d.sym\") where: %s %s})",
             ray_test_tmp(CS_TMP), name, gen, ray_test_tmp(CS_TMP), name, gen, where, tail);
    return ray_eval_str(q);
}

/* Same columns, same bytes (SYM is not projected: row ids pin the rows). */
static bool fq_same(ray_t* a, ray_t* b, int64_t want_rows) {
    if (!a || !b || RAY_IS_ERR(a) || RAY_IS_ERR(b)) return false;
    static const char* const cols[] = { "i", "v", "d" };
    for (int c = 0; c < 3; c++) {
        ray_t* x = ray_table_get_col(a, ray_sym_intern(cols[c], 1));
        ray_t* y = ray_table_get_col(b, ray_sym_intern(cols[c], 1));
        if (!x || !y || x->len != y->len || x->len != want_rows) return false;
        if (memcmp(ray_data(x), ray_data(y), (size_t)x->len * 8) != 0) return false;
    }
    return true;
}

/* Fused take and fused top-k, through select with take:, on generation 0
 * and generation 2: the fused path compiled the predicate on both (never
 * declined) and the full results are identical. */
static test_result_t test_fused_pred_bound(void) {
    TEST_ASSERT_TRUE(fq_save("fb"));
    static const char* const w[] = {
        "(== s 'beta)", "(!= s 'beta)", "(== s 'nowhere)", "(!= s 'nowhere)",
        "(in w [1 2 3])", "(like s \"alp*\")", "(like s \"*mm*\")",
        "(> v 5)", "(<= v 3)", "(== v 4)", "(!= v 4)", "(< d 2.0)", "(>= d 1.5)",
        "(and (== s 'alpha) (> v 5))",
    };
    static const char* const tails[] = { "take: 10", "desc: i take: 10" };
    for (size_t i = 0; i < sizeof w / sizeof *w; i++)
        for (int m = 0; m < 2; m++) {
            ray_fused_topk_counters_reset();
            ray_t* a = fq_run("fb", 0, w[i], tails[m]);
            ray_t* b = fq_run("fb", 2, w[i], tails[m]);
            uint64_t ran, dec; ray_fused_topk_counters(&ran, &dec);
            int64_t rows = (a && !RAY_IS_ERR(a)) ? ray_table_nrows(a) : -1;
            bool same = fq_same(a, b, rows);
            bool absent = strstr(w[i], "nowhere") && strstr(w[i], "==");
            bool ok = same && ran == 2 && dec == 0 && (absent ? rows == 0 : rows == 10);
            if (!ok) fprintf(stderr, "fused_pred_bound: %s %s rows=%lld same=%d ran=%llu declined=%llu\n",
                             w[i], tails[m], (long long)rows, same, (unsigned long long)ran, (unsigned long long)dec);
            if (a) ray_release(a);
            if (b) ray_release(b);
            TEST_ASSERT_TRUE(ok);
        }
    PASS();
}

/* The compiled predicate over bound encoded sources (no query-entry decode)
 * selects the same rows as over generation 0. */
static test_result_t test_fused_pred_enc_source(void) {
    TEST_ASSERT_TRUE(fq_save("fe"));
    static const char* const w[] = {
        "(== s 'beta)", "(!= s 'nowhere)", "(in w [1 2 3])", "(like s \"alp*\")",
        "(> v 5)", "(== v 4)", "(< d 2.0)", "(and (== s 'alpha) (> v 5))",
    };
    ray_t* t0 = ray_read_splayed(ray_test_tmp(CS_TMP "/fe0"), ray_test_tmp(CS_TMP "/fe0.sym"));
    ray_t* t2 = ray_read_splayed(ray_test_tmp(CS_TMP "/fe2"), ray_test_tmp(CS_TMP "/fe2.sym"));
    TEST_ASSERT_TRUE(t0 && t2);
    ray_enc_counters_reset();
    for (size_t i = 0; i < sizeof w / sizeof *w; i++) {
        uint8_t* bits[2] = { ray_alloc_raw(FQ_N + 64), ray_alloc_raw(FQ_N + 64) };
        ray_t* tb[2] = { t0, t2 };
        int rc[2] = { 0, 0 };
        bool rerr = false;
        for (int g = 0; g < 2; g++) {
            ray_graph_t* gr = ray_graph_new(tb[g]);
            ray_t* e = ray_parse(w[i]);
            ray_op_t* dag = compile_expr_dag(gr, e);
            fp_pred_t p; ray_t* err = NULL;
            rc[g] = dag ? fp_compile_pred(gr, dag, tb[g], &p, &err) : -1;
            if (err) { ray_error_free(err); rc[g] = -2; }
            memset(bits[g], 0, FQ_N + 64);
            if (rc[g] == 0) {                  /* encoded children read by range */
                fp_scratch_t sc; fp_scratch_init(&sc);
                for (int64_t st = 0; st < FQ_N && !rerr; st += 1024) {
                    ray_t* ee = fp_eval_pred_rs(&p, st, st + 1024 < FQ_N ? st + 1024 : FQ_N, bits[g] + st, &sc);
                    if (ee) { ray_error_free(ee); rerr = true; }
                }
                fp_scratch_free(&sc);
                fp_pred_cleanup(&p);
            }
            ray_release(e);
            ray_graph_free(gr);
        }
        bool same = rc[0] == 0 && rc[1] == 0 && !rerr && memcmp(bits[0], bits[1], FQ_N) == 0;
        bool any = false;
        for (int r = 0; r < FQ_N; r++) if (bits[0][r]) { any = true; break; }
        ray_free_raw(bits[0]); ray_free_raw(bits[1]);
        if (!same || !any) fprintf(stderr, "fused_pred_enc_source: %s rc=%d,%d any=%d\n", w[i], rc[0], rc[1], any);
        if (!(same && any)) { ray_release(t0); ray_release(t2); }
        TEST_ASSERT_TRUE(same && any);
    }
    ray_release(t0); ray_release(t2);
    PASS();
}

/* A bind failure reaches the query as its error; a bound encoded column
 * compiles without a decode (a failed range read is the scan's error,
 * scanrange/topk_take_error); a decline stays a decline. */
static test_result_t test_fused_pred_bind_failure(void) {
    TEST_ASSERT_TRUE(fq_save("ff"));
    ray_fused_topk_counters_reset();
    ray_graph_bind_test_fail_alloc(1);
    ray_t* r = fq_run("ff", 0, "(> v 5)", "take: 10");
    ray_graph_bind_test_fail_alloc(0);
    bool is_err = r && RAY_IS_ERR(r);
    bool code_ok = is_err && strcmp(ray_err_code(r), "oom") == 0;
    bool msg_ok = is_err && ray_error_msg() && strstr(ray_error_msg(), "column bind");
    if (is_err) ray_error_free(r); else if (r) ray_release(r);
    TEST_ASSERT_TRUE(is_err && code_ok && msg_ok);
    /* the same through the top-k executor */
    ray_graph_bind_test_fail_alloc(1);
    r = fq_run("ff", 0, "(> v 5)", "desc: i take: 10");
    ray_graph_bind_test_fail_alloc(0);
    is_err = r && RAY_IS_ERR(r);
    if (is_err) ray_error_free(r); else if (r) ray_release(r);
    TEST_ASSERT_TRUE(is_err);
    /* after the injection the query answers normally */
    r = fq_run("ff", 0, "(> v 5)", "take: 10");
    bool ok = r && !RAY_IS_ERR(r) && ray_table_nrows(r) == 10;
    if (r && !RAY_IS_ERR(r)) ray_release(r);
    TEST_ASSERT_TRUE(ok);

    /* a bound encoded column: compiling decodes nothing (the child reads by
     * range), so an injected decode failure is not reached */
    ray_t* t2 = ray_read_splayed(ray_test_tmp(CS_TMP "/ff2"), ray_test_tmp(CS_TMP "/ff2.sym"));
    TEST_ASSERT_TRUE(t2 && !RAY_IS_ERR(t2));
    ray_graph_t* gr = ray_graph_new(t2);
    ray_op_t* dag = ray_gt(gr, ray_scan(gr, "v"), ray_const_i64(gr, 5));   /* no column lookup: nothing decoded yet */
    fp_pred_t p; ray_t* err = NULL;
    ray_enccol_test_fail_alloc(1);
    int rc = dag ? fp_compile_pred(gr, dag, t2, &p, &err) : -1;
    bool compiled = rc == 0 && !err && p.n_children == 1 && p.children[0].enc;
    ray_enccol_test_fail_alloc(0);
    if (rc == 0) fp_pred_cleanup(&p);
    if (err) ray_error_free(err);
    ray_graph_free(gr);
    ray_t* e;
    /* an unsupported leaf declines without an error */
    gr = ray_graph_new(t2);
    e = ray_parse("(== nosuchcol 1)");
    dag = compile_expr_dag(gr, e);
    err = NULL;
    int rc2 = dag ? fp_compile_pred(gr, dag, t2, &p, &err) : -1;
    bool decline = rc2 != 0 && err == NULL;
    if (rc2 == 0) fp_pred_cleanup(&p);
    ray_release(e);
    ray_graph_free(gr);
    ray_release(t2);
    TEST_ASSERT_TRUE(compiled);
    TEST_ASSERT_TRUE(decline);
    PASS();
}

/* ---- Task 20: expression, aggregate and top-k compilation from bound sources ----
 *
 * Query-level checks go through select, which still decodes at its entry
 * (transitional: Tasks 30-32 remove it), so select sees plain columns.  The
 * direct-compile checks below bind a table read from generation-2 files (its
 * slots are RAY_ENCCOL), which is where the converted sites read encoded
 * sources. */

/* Same formatted output. */
static bool cp_fmt_same(ray_t* x, ray_t* y) {
    if (!x || !y || RAY_IS_ERR(x) || RAY_IS_ERR(y)) return false;
    ray_t* fx = ray_fmt(x, 0); ray_t* fy = ray_fmt(y, 0);
    bool same = fx && fy && ray_str_len(fx) == ray_str_len(fy) &&
                memcmp(ray_str_ptr(fx), ray_str_ptr(fy), ray_str_len(fx)) == 0;
    if (fx) ray_release(fx);
    if (fy) ray_release(fy);
    return same;
}

/* An injected bind failure arrives as the oom "column bind" error. */
static bool cp_is_bind_error(ray_t* r) {
    if (!r || !RAY_IS_ERR(r)) return false;
    bool ok = strcmp(ray_err_code(r), "oom") == 0 && ray_error_msg() && strstr(ray_error_msg(), "column bind");
    return ok;
}

/* Brief: group-by with sum / avg over an expression and top-k over a
 * generation-2 table equal the generation-0 answers.  The compiled paths
 * ran (expression compiles counted, fused top-k executions counted). */
static test_result_t test_compile_paths_bound(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['k 'v 'w] (list (take [1 2 3] 3000) (til 3000) (* 2 (til 3000)))))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/cp2\" t {compress: true})")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/cp0\" t {compress: false})")), "");
    static const char* const q[] = {
        "(select {s: (sum (+ v w)) a: (avg (* 3 v)) from: (.db.splayed.get \"%s\") by: k})",
        "(select {from: (.db.splayed.get \"%s\") where: (> v 10) desc: v take: 5})",
        "(select {from: (.db.splayed.get \"%s\") where: (< w 100) take: 3})",
        NULL,
    };
    for (int i = 0; q[i]; i++) {
        char a2[512], a0[512];
        snprintf(a2, sizeof a2, q[i], ray_test_tmp(CS_TMP "/cp2"));
        snprintf(a0, sizeof a0, q[i], ray_test_tmp(CS_TMP "/cp0"));
        uint64_t ok0 = ray_expr_compile_ok, ran0, dec0, ran1, dec1;
        ray_fused_topk_counters(&ran0, &dec0);
        ray_t* x = ray_eval_str(a2); ray_t* y = ray_eval_str(a0);
        ray_fused_topk_counters(&ran1, &dec1);
        TEST_ASSERT_FMT(x && y && !RAY_IS_ERR(x) && !RAY_IS_ERR(y), "query %d", i);
        TEST_ASSERT_FMT(cp_fmt_same(x, y), "query %d differs", i);
        if (i == 0) TEST_ASSERT_FMT(ray_expr_compile_ok >= ok0 + 2, "query %d: expression compiled on both", i);
        else TEST_ASSERT_FMT(ran1 >= ran0 + 2, "query %d: fused top-k ran on both (ran %llu->%llu)", i,
                             (unsigned long long)ran0, (unsigned long long)ran1);
        ray_release(x); ray_release(y);
    }
    /* A failed bind anywhere in the group-by planners (expression inputs, the
     * linear / affine plans) is the query's error, never a silent fallback. */
    static const char* const qb[] = {
        "(select {s: (sum (+ v w)) a: (avg (* 3 v)) from: (.db.splayed.get \"%s\") by: k})",
        "(select {s: (sum (+ (* 2 v) w)) from: (.db.splayed.get \"%s\")})",
        "(select {s: (sum (+ v 1)) a: (avg (- w 2)) from: (.db.splayed.get \"%s\") by: k})",
        NULL,
    };
    for (int i = 0; qb[i]; i++) {
        char a0[512];
        snprintf(a0, sizeof a0, qb[i], ray_test_tmp(CS_TMP "/cp0"));
        int n = 1;
        for (; n < 20; n++) {
            ray_graph_bind_test_fail_alloc(n);
            ray_t* r = ray_eval_str(a0);
            ray_graph_bind_test_fail_alloc(0);
            bool is_err = r && RAY_IS_ERR(r);
            bool be = cp_is_bind_error(r);
            if (is_err) ray_error_free(r); else if (r) ray_release(r);
            if (!is_err) break;
            TEST_ASSERT_FMT(be, "query %d bind %d: not the bind error", i, n);
        }
        TEST_ASSERT_FMT(n - 1 == 2, "query %d: %d binds", i, n - 1);   /* v and w, each failing in turn */
    }
    PASS();
}


static ray_t* cp_tiny(int64_t base) {
    ray_t* t = ray_table_new(1);
    ray_t* c = ray_vec_new(RAY_I64, 4); c->len = 4;
    for (int i = 0; i < 4; i++) ((int64_t*)ray_data(c))[i] = base + i;
    t = ray_table_add_col(t, ray_sym_intern("a", 1), c);
    ray_release(c);
    return t;
}

/* A released temporary table's address is reused by another table with other
 * columns: the bind must not return the first table's column.  Tables of the
 * same shape are built until the allocator hands back the freed address. */
static test_result_t test_bind_address_reuse(void) {
    int64_t sa = ray_sym_intern("a", 1);
    ray_t* t1 = cp_tiny(0);
    ray_t* host = cp_tiny(500);
    ray_graph_t* g = ray_graph_new(host);
    ray_t* err = NULL;
    ray_col_src_t* s1 = ray_graph_bind_col(g, t1, sa, &err);
    TEST_ASSERT_TRUE(s1 && !err);
    ray_t* o1 = s1->obj;
    ray_t* addr1 = t1;
    ray_release(t1);                         /* released without ray_graph_unbind_table */
    ray_t* pile[256]; int np = 0;
    ray_t* t2 = NULL;
    while (np < 256 && !t2) {
        pile[np] = cp_tiny(100);
        if (pile[np] == addr1) t2 = pile[np];
        np++;
    }
    TEST_ASSERT_TRUE(t2 != NULL);            /* the allocator reused the address */
    ray_col_src_t* s2 = ray_graph_bind_col(g, t2, sa, &err);
    TEST_ASSERT_TRUE(s2 && !err && s2 != s1 && s2->obj != o1);
    ray_t* plain = NULL;
    TEST_ASSERT_NULL(ray_col_src_plain(s2, &plain));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(plain))[0], 100);
    for (int i = 0; i < np; i++) ray_release(pile[i]);
    ray_graph_free(g);
    ray_release(host);
    PASS();
}

/* Binds of a released temporary are dropped by ray_graph_unbind_table: the
 * column memory goes back with the table, and a gen-0 group-by over an
 * expression drops the binds of its synthetic sub-table with unchanged results. */
static test_result_t test_unbind_releases_temporaries(void) {
    int64_t sa = ray_sym_intern("a", 1);
    enum { N = 1 << 17 };
    ray_t* host = cp_tiny(0);
    ray_graph_t* g = ray_graph_new(host);
    ray_mem_stats_t m0, m1, m2;
    ray_err_t dummy = RAY_OK; (void)dummy;
    ray_t* err = NULL;
    ray_mem_stats(&m0);
    /* unbound before the release: the column memory goes back with the table */
    ray_t* big = ray_vec_new(RAY_I64, N); big->len = N;
    ray_t* t = ray_table_new(1);
    t = ray_table_add_col(t, sa, big);
    ray_release(big);
    TEST_ASSERT_TRUE(ray_graph_bind_col(g, t, sa, &err) && !err);
    TEST_ASSERT_EQ_I(ray_graph_binds_count(g), 1);
    ray_graph_unbind_table(g, t);
    TEST_ASSERT_EQ_I(ray_graph_binds_count(g), 0);
    ray_release(t);
    ray_mem_stats(&m1);
    TEST_ASSERT_TRUE(m1.bytes_allocated < m0.bytes_allocated + 4096);
    /* released without unbinding: the bind keeps the column */
    big = ray_vec_new(RAY_I64, N); big->len = N;
    t = ray_table_new(1);
    t = ray_table_add_col(t, sa, big);
    ray_release(big);
    TEST_ASSERT_TRUE(ray_graph_bind_col(g, t, sa, &err) && !err);
    ray_release(t);
    ray_mem_stats(&m2);
    TEST_ASSERT_TRUE(m2.bytes_allocated >= m0.bytes_allocated + (size_t)N * 8);
    ray_graph_free(g);
    ray_release(host);

    /* gen-0 group-by over an expression: the sub-table's binds are dropped */
    TEST_ASSERT_STR_EQ(ev_code("(set T (table ['k 'v 'w] (list (take [1 2 3] 200000) (til 200000) (* 2 (til 200000)))))"), "");
    const char* q = "(select {s: (sum (+ v w)) a: (avg (* 3 v)) from: T by: k})";
    uint64_t d0 = ray_graph_binds_dropped();
    ray_t* r = ray_eval_str(q);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(ray_graph_binds_dropped() > d0);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);
    ray_t* s = ray_table_get_col(r, ray_sym_intern("s", 1));
    int64_t tot = 0;
    for (int i = 0; i < 3; i++) tot += ((int64_t*)ray_data(s))[i];
    TEST_ASSERT_EQ_I(tot, 3 * ((int64_t)199999 * 200000 / 2));                       /* sum of v + w = 3 * v */
    ray_release(r);

    /* gen-0 IF projection: each branch runs over its rows' compacted
     * sub-table, whose binds are dropped with it; the memory is back after
     * the query */
    ray_mem_stats(&m0);
    d0 = ray_graph_binds_dropped();
    r = ray_eval_str("(select {x: (if (> v 100000) (+ v w) (- v w)) from: T})");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_TRUE(ray_graph_binds_dropped() >= d0 + 2);                         /* both branches */
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 200000);
    ray_t* x = ray_table_get_col(r, ray_sym_intern("x", 1));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(x))[7], -7);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(x))[150000], 450000);
    ray_release(r);
    ray_mem_stats(&m1);
    TEST_ASSERT_TRUE(m1.bytes_allocated < m0.bytes_allocated + 65536);
    PASS();
}

/* The compile fixture: CP_N rows saved at generation 0 and 2 under
 * CS_TMP "/cp0g" and "/cp2g".  Group keys k0 (4 dense), k1 (4 sparse),
 * k2 (8192 dense), k4 (all distinct); null-free I64 a, b; null-free F64 f,
 * g with exactly representable sums; I64 v and SYM x with nulls; SYM y
 * without. */
enum { CP_N = 70000 };
static ray_t* cp_table(void) {
    ray_t* t = ray_table_new(11);
    ray_t* c[11];
    int8_t ty[11] = { RAY_I64, RAY_I64, RAY_I64, RAY_I64, RAY_I64, RAY_I64, RAY_F64, RAY_F64, RAY_I64, RAY_SYM, RAY_SYM };
    static const char* const nm[11] = { "k0", "k1", "k2", "k4", "a", "b", "f", "g", "v", "x", "y" };
    for (int j = 0; j < 11; j++) {
        c[j] = ty[j] == RAY_SYM ? ray_sym_vec_new(RAY_SYM_W64, CP_N) : ray_vec_new(ty[j], CP_N);
        c[j]->len = CP_N;
    }
    const char* ys[4] = { "ya", "yb", "yc", "yd" };
    const char* xs[3] = { "xa", "xb", "xc" };
    for (int64_t i = 0; i < CP_N; i++) {
        ((int64_t*)ray_data(c[0]))[i] = i % 4;
        ((int64_t*)ray_data(c[1]))[i] = (i % 4) * 1000000;
        ((int64_t*)ray_data(c[2]))[i] = i % 8192;
        ((int64_t*)ray_data(c[3]))[i] = i * 1000000;
        ((int64_t*)ray_data(c[4]))[i] = i % 1000;
        ((int64_t*)ray_data(c[5]))[i] = (i * 7) % 1000;
        ((double*)ray_data(c[6]))[i] = (double)(i % 64) * 0.5;
        ((double*)ray_data(c[7]))[i] = (double)(i % 32) * 0.25;
        ((int64_t*)ray_data(c[8]))[i] = i % 17;
        ((int64_t*)ray_data(c[9]))[i] = ray_sym_intern(xs[i % 3], 2);
        ((int64_t*)ray_data(c[10]))[i] = ray_sym_intern(ys[i % 4], 2);
    }
    for (int64_t i = 0; i < CP_N; i += 9) { ray_vec_set_null(c[8], i, true); ray_vec_set_null(c[9], i, true); }
    for (int j = 0; j < 11; j++) { t = ray_table_add_col(t, ray_sym_intern(nm[j], strlen(nm[j])), c[j]); ray_release(c[j]); }
    return t;
}

static bool cp_save(void) {
    ray_t* t = cp_table();
    bool ok = ray_splay_save_mode(t, ray_test_tmp(CS_TMP "/cp0g"), ray_test_tmp(CS_TMP "/cp0g.sym"), RAY_COMPRESS_OFF) == RAY_OK &&
              ray_splay_save_mode(t, ray_test_tmp(CS_TMP "/cp2g"), ray_test_tmp(CS_TMP "/cp2g.sym"), RAY_COMPRESS_ON) == RAY_OK;
    ray_release(t);
    return ok;
}

static ray_t* cp_read(int gen) {
    return gen == 2 ? ray_read_splayed(ray_test_tmp(CS_TMP "/cp2g"), ray_test_tmp(CS_TMP "/cp2g.sym"))
                    : ray_read_splayed(ray_test_tmp(CS_TMP "/cp0g"), ray_test_tmp(CS_TMP "/cp0g.sym"));
}

static ray_op_t* cp_dag(ray_graph_t* g, const char* src, ray_t** e) {
    *e = ray_parse(src);
    return *e ? compile_expr_dag(g, *e) : NULL;
}

/* The three sum/avg planners over bound sources, generation 0 and 2: the
 * plans read the right data, decline exactly as before, decode nothing on a
 * decline, and report bind and plain failures through their error out. */
static test_result_t test_try_plans_bound(void) {
    TEST_ASSERT_TRUE(cp_save());
    for (int gen = 0; gen <= 2; gen += 2) {
        ray_t* t = cp_read(gen);
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        TEST_ASSERT_EQ_I(ray_table_get_col_enc(t, ray_sym_intern("a", 1)) != NULL, gen == 2);
        /* The DAG is built over the table itself: column presence is read from
         * metadata, so building it decodes nothing (asserted below). */
        ray_graph_t* gr = ray_graph_new(t);
        ray_t *e1, *e2, *e3, *e4, *e5, *e6, *e7, *e8;
        ray_t* err = NULL;

        /* a decline decodes nothing: a nullable term, int x int, an unknown column */
        agg_linear_t lin; agg_prod_t prod; agg_affine_t aff; ray_t* vec = NULL;
        ray_enc_counters_reset();
        ray_op_t* d_null = cp_dag(gr, "(+ (* 2 b) v)", &e1);
        ray_op_t* d_ii   = cp_dag(gr, "(* a b)", &e2);
        ray_op_t* d_fv   = cp_dag(gr, "(* f v)", &e3);
        ray_op_t* d_lin = cp_dag(gr, "(+ (+ (* 2 b) a) 7)", &e4);
        ray_op_t* d_fg = cp_dag(gr, "(* f g)", &e5);
        ray_op_t* d_fa = cp_dag(gr, "(* f a)", &e6);
        ray_op_t* d_af = cp_dag(gr, "(- f 1.5)", &e7);
        ray_op_t* d_ai = cp_dag(gr, "(+ a 5)", &e8);
        TEST_ASSERT_TRUE(d_null && d_ii && d_fv && d_lin && d_fg && d_fa && d_af && d_ai);
        ray_enc_counters_t kb; ray_enc_counters_get(&kb);
        TEST_ASSERT_EQ_U(kb.materialized, 0);               /* DAG construction decodes nothing */
        ray_enc_counters_reset();
        TEST_ASSERT_FALSE(try_linear_sumavg_input_i64(gr, t, d_null, &lin, &err)); TEST_ASSERT_NULL(err);
        TEST_ASSERT_FALSE(try_prod_sumavg_input_f64(gr, t, d_ii, &prod, &err));    TEST_ASSERT_NULL(err);
        TEST_ASSERT_FALSE(try_prod_sumavg_input_f64(gr, t, d_fv, &prod, &err));    TEST_ASSERT_NULL(err);
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        TEST_ASSERT_TRUE(ray_graph_binds_count(gr) >= 3);

        /* linear: 2 * b + a + 7 */
        TEST_ASSERT_TRUE(d_lin && try_linear_sumavg_input_i64(gr, t, d_lin, &lin, &err));
        TEST_ASSERT_NULL(err);
        TEST_ASSERT_TRUE(lin.enabled && lin.n_terms == 2 && lin.bias_i64 == 7);
        for (int64_t i = 0; i < CP_N; i += 997) {
            int64_t sum = lin.bias_i64;
            for (uint8_t j = 0; j < lin.n_terms; j++) sum += lin.coeff_i64[j] * ((const int64_t*)lin.term_ptrs[j])[i];
            TEST_ASSERT_EQ_I(sum, 2 * ((i * 7) % 1000) + (i % 1000) + 7);
        }
        /* product: f * g (F64 x F64) and f * a (F64 x I64) */
        TEST_ASSERT_TRUE(d_fg && try_prod_sumavg_input_f64(gr, t, d_fg, &prod, &err));
        TEST_ASSERT_NULL(err);
        TEST_ASSERT_TRUE(prod.enabled && prod.ta == RAY_F64 && prod.tb == RAY_F64);
        for (int64_t i = 0; i < CP_N; i += 997)
            TEST_ASSERT_TRUE(((const double*)prod.pa)[i] * ((const double*)prod.pb)[i] == (double)(i % 64) * 0.5 * ((double)(i % 32) * 0.25));
        TEST_ASSERT_TRUE(d_fa && try_prod_sumavg_input_f64(gr, t, d_fa, &prod, &err));
        TEST_ASSERT_TRUE(prod.ta == RAY_F64 && prod.tb == RAY_I64);
        /* affine: f - 1.5, a + 5 */
        memset(&aff, 0, sizeof aff);
        TEST_ASSERT_TRUE(d_af && try_affine_sumavg_input(gr, t, d_af, &vec, &aff, &err));
        TEST_ASSERT_NULL(err);
        TEST_ASSERT_TRUE(aff.enabled && aff.bias_f64 == -1.5 && vec && vec->type == RAY_F64 && vec->len == CP_N);
        TEST_ASSERT_TRUE(((const double*)ray_data(vec))[5] == 2.5);
        memset(&aff, 0, sizeof aff);
        TEST_ASSERT_TRUE(d_ai && try_affine_sumavg_input(gr, t, d_ai, &vec, &aff, &err));
        TEST_ASSERT_TRUE(aff.enabled && aff.bias_i64 == 5 && vec->type == RAY_I64 && ((const int64_t*)ray_data(vec))[1234] == 234);
        if (gen == 2) { ray_enc_counters_get(&k); TEST_ASSERT_EQ_U(k.materialized, 4); }

        ray_release(e1); ray_release(e2); ray_release(e3); ray_release(e4);
        ray_release(e5); ray_release(e6); ray_release(e7); ray_release(e8);
        ray_graph_free(gr);
        ray_release(t);

        /* a failed bind is the planner's error, not a decline */
        t = cp_read(gen);
        gr = ray_graph_new(t);
        ray_op_t* dl = cp_dag(gr, "(+ (* 2 b) a)", &e1);
        ray_op_t* dp = cp_dag(gr, "(* f g)", &e2);
        ray_op_t* da = cp_dag(gr, "(- f 1.5)", &e3);
        TEST_ASSERT_TRUE(dl && dp && da);
        ray_graph_bind_test_fail_alloc(1);
        bool r = try_linear_sumavg_input_i64(gr, t, dl, &lin, &err);
        ray_graph_bind_test_fail_alloc(0);
        bool bind_err = !r && cp_is_bind_error(err);
        if (err) ray_error_free(err);
        err = NULL;
        ray_graph_bind_test_fail_alloc(1);
        r = try_prod_sumavg_input_f64(gr, t, dp, &prod, &err);
        ray_graph_bind_test_fail_alloc(0);
        bool bind_err2 = !r && cp_is_bind_error(err);
        if (err) ray_error_free(err);
        err = NULL;
        ray_graph_bind_test_fail_alloc(1);
        r = try_affine_sumavg_input(gr, t, da, &vec, &aff, &err);
        ray_graph_bind_test_fail_alloc(0);
        bool bind_err3 = !r && cp_is_bind_error(err);
        if (err) ray_error_free(err);
        err = NULL;
        TEST_ASSERT_TRUE(bind_err && bind_err2 && bind_err3);
        /* a failed materialization of a bound encoded column */
        if (gen == 2) {
            ray_enccol_test_fail_alloc(1);
            r = try_linear_sumavg_input_i64(gr, t, dl, &lin, &err);
            ray_enccol_test_fail_alloc(0);
            bool plain_err = !r && err && RAY_IS_ERR(err);
            if (err) ray_error_free(err);
            err = NULL;
            ray_enccol_test_fail_alloc(1);
            r = try_prod_sumavg_input_f64(gr, t, dp, &prod, &err);
            ray_enccol_test_fail_alloc(0);
            bool plain_err2 = !r && err && RAY_IS_ERR(err);
            if (err) ray_error_free(err);
            err = NULL;
            ray_enccol_test_fail_alloc(1);
            memset(&aff, 0, sizeof aff);
            r = try_affine_sumavg_input(gr, t, da, &vec, &aff, &err);
            ray_enccol_test_fail_alloc(0);
            bool plain_err3 = !r && err && RAY_IS_ERR(err);
            if (err) ray_error_free(err);
            TEST_ASSERT_TRUE(plain_err && plain_err2 && plain_err3);
        }
        ray_release(e1); ray_release(e2); ray_release(e3);
        ray_graph_free(gr);
        ray_release(t);
    }
    PASS();
}

/* Run `body` with the nth bind failing, n = 1, 2, ...: every failure before
 * the first success must be the bind error (never a decline or a silently
 * different answer); returns how many binds the body makes. */
typedef ray_t* (*cp_body_fn)(void* ctx);
static int cp_bind_sweep(cp_body_fn body, void* ctx, bool* all_errors) {
    *all_errors = true;
    int n = 1;
    for (; n < 40; n++) {
        ray_graph_bind_test_fail_alloc(n);
        ray_t* r = body(ctx);
        ray_graph_bind_test_fail_alloc(0);
        bool is_err = r && RAY_IS_ERR(r);
        bool bind_err = cp_is_bind_error(r);
        if (is_err) ray_error_free(r); else if (r) ray_release(r);
        if (!is_err) break;
        if (!bind_err) *all_errors = false;
    }
    return n - 1;
}

typedef struct { ray_t* t; const char* src; } cp_expr_ctx_t;
static ray_t* cp_expr_body(void* c) {
    cp_expr_ctx_t* x = c;
    ray_graph_t* g = ray_graph_new(x->t);
    ray_t* e = ray_parse(x->src);
    ray_op_t* dag = compile_expr_dag(g, e);
    ray_t* r = dag ? ray_execute(g, dag) : NULL;
    if (r && ray_is_lazy(r)) r = ray_lazy_materialize(r);
    ray_release(e);
    ray_graph_free(g);
    return r;
}

/* expr_compile over bound sources: the scan arm decides from the source
 * (generation 0 and 2 alike), the registers carry their source, and the
 * three execution routes (root compile, per-node fusion, predicate to
 * selection) answer identically and propagate a failed bind. */
static test_result_t test_expr_compile_bound(void) {
    TEST_ASSERT_TRUE(cp_save());
    ray_t* res[2][3] = {{ NULL }};
    bool nullable[2][3];
    for (int gi = 0; gi < 2; gi++) {
        int gen = gi * 2;
        ray_t* t = cp_read(gen);
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_graph_t* gr = ray_graph_new(t);
        ray_t *e3, *e4, *e5;
        ray_t* err = NULL;
        ray_expr_t ex;
        static const char* const srcs[3] = { "(+ (* a 3) b)", "(+ v 1)", "(+ a 1)" };
        for (int s = 0; s < 3; s++) {
            ray_t* e;
            ray_op_t* d = cp_dag(gr, srcs[s], &e);
            TEST_ASSERT_TRUE(d && expr_compile(gr, t, d, &ex, &err));
            TEST_ASSERT_NULL(err);
            nullable[gi][s] = false;
            bool src_set = false;
            for (int r = 0; r < ex.n_regs; r++) {
                if (ex.regs[r].kind != REG_SCAN) continue;
                if (ex.regs[r].nullable) nullable[gi][s] = true;
                /* the register reads a source bound on the graph: the
                 * registry's entry for one of the scanned columns (a hit,
                 * no new bind), encoded exactly at generation 2, where it
                 * is range-read (enc, no captured pointer) */
                src_set = false;
                static const char* const cn[3] = { "a", "b", "v" };
                for (int k = 0; k < 3 && !src_set; k++) {
                    uint32_t nb = ray_graph_binds_count(gr);
                    ray_col_src_t* bs = ray_graph_bind_col(gr, t, ray_sym_intern(cn[k], 1), &err);
                    /* a column this expression scans is already bound: a hit */
                    bool scanned = strstr(srcs[s], cn[k]) != NULL;
                    if (scanned) TEST_ASSERT_EQ_I(ray_graph_binds_count(gr), nb);
                    TEST_ASSERT_NULL(err);
                    if (!bs || !bs->obj || ex.regs[r].src != bs) continue;
                    if (gen == 2) {
                        src_set = bs->obj->type == RAY_ENCCOL && ex.regs[r].enc && !ex.regs[r].data;
                    } else {
                        ray_t* plain = NULL;
                        if (ray_col_src_plain(bs, &plain) || !plain) continue;
                        src_set = bs->obj->type != RAY_ENCCOL && !ex.regs[r].enc &&
                                  ex.regs[r].data == ray_data(plain);
                    }
                }
                if (!src_set) break;
            }
            TEST_ASSERT_TRUE(src_set);
            res[gi][s] = expr_eval_full(&ex, CP_N);
            TEST_ASSERT_TRUE(res[gi][s] && !RAY_IS_ERR(res[gi][s]));
            ray_release(e);
        }
        /* a: no nulls; v: nulls (the directory count at generation 2) */
        TEST_ASSERT_FALSE(nullable[gi][0]);
        TEST_ASSERT_TRUE(nullable[gi][1]);
        TEST_ASSERT_FALSE(nullable[gi][2]);

        /* SYM columns read from files resolve over the file's domain: both generations
         * bail on the bound source's domain (the fused program reads raw ids) */
        uint64_t nb = ray_expr_bail_counts[EXPR_BAIL_SYM_DOMAIN];
        ray_op_t* dx = ray_isnull(gr, ray_scan(gr, "x"));
        TEST_ASSERT_FALSE(expr_compile(gr, t, dx, &ex, &err));
        TEST_ASSERT_NULL(err);
        ray_op_t* dy = ray_eq(gr, ray_scan(gr, "y"), ray_const_str(gr, "yb", 2));
        TEST_ASSERT_FALSE(expr_compile(gr, t, dy, &ex, &err));
        TEST_ASSERT_NULL(err);
        TEST_ASSERT_EQ_U(ray_expr_bail_counts[EXPR_BAIL_SYM_DOMAIN], nb + 2);
        ray_t *e1 = NULL;
        /* a partition key column and an unknown column are bails, not errors */
        ray_op_t* dz = cp_dag(gr, "(+ nosuch 1)", &e3);
        TEST_ASSERT_FALSE(dz && expr_compile(gr, t, dz, &ex, &err));
        TEST_ASSERT_NULL(err);
        ray_release(e3);

        /* the failure paths of the scan arm */
        ray_graph_free(gr);
        ray_release(t);
        t = cp_read(gen);
        gr = ray_graph_new(t);
        ray_op_t* d = cp_dag(gr, "(+ a b)", &e4);
        ray_graph_bind_test_fail_alloc(1);
        bool r = expr_compile(gr, t, d, &ex, &err);
        ray_graph_bind_test_fail_alloc(0);
        TEST_ASSERT_TRUE(!r && cp_is_bind_error(err));
        ray_error_free(err); err = NULL;
        if (gen == 2) {
            /* P2: the scan arm no longer decodes (its registers are
             * range-read), so a failing decode allocation cannot fail the
             * compile; range-read failures are scanrange/expr_error's */
            ray_enccol_test_fail_alloc(1);
            ray_enc_counters_reset();
            r = expr_compile(gr, t, d, &ex, &err);
            ray_enccol_test_fail_alloc(0);
            TEST_ASSERT_TRUE(r && !err);
            ray_enc_counters_t kc; ray_enc_counters_get(&kc);
            TEST_ASSERT_EQ_U(kc.materialized, 0);
        }
        ray_release(e4);

        /* the predicate-to-selection route */
        ray_op_t* dp = cp_dag(gr, "(> a 500)", &e5);
        bool all_pass = false;
        ray_t* sel = exec_pred_to_selection(gr, dp, CP_N, &all_pass);
        TEST_ASSERT_TRUE(sel && !RAY_IS_ERR(sel));
        int64_t tp = ray_rowsel_meta(sel)->total_pass;
        ray_release(sel);
        TEST_ASSERT_EQ_I(tp, 70 * 499);                  /* a > 500 over 70 blocks of a = i % 1000 */
        ray_graph_free(gr);
        gr = ray_graph_new(t);             /* fresh graph: nothing bound yet */
        ray_graph_bind_test_fail_alloc(1);
        dp = cp_dag(gr, "(> a 500)", &e1);
        sel = exec_pred_to_selection(gr, dp, CP_N, &all_pass);
        ray_graph_bind_test_fail_alloc(0);
        TEST_ASSERT_TRUE(cp_is_bind_error(sel));
        ray_error_free(sel);
        ray_release(e1); ray_release(e5);
        ray_graph_free(gr);
        ray_release(t);
    }
    /* generation 0 and 2 expressions are the same values, nulls included */
    for (int s = 0; s < 3; s++) {
        TEST_ASSERT_TRUE(res[0][s]->len == res[1][s]->len && res[0][s]->type == res[1][s]->type);
        TEST_ASSERT_TRUE(memcmp(ray_data(res[0][s]), ray_data(res[1][s]), (size_t)res[0][s]->len * 8) == 0);
        TEST_ASSERT_EQ_I(nullable[0][s], nullable[1][s]);
        ray_release(res[0][s]); ray_release(res[1][s]);
    }

    /* the execution routes: root compile and per-node fusion (the root bails on
     * the nullable SYM, its subtree compiles) answer the same on both generations
     * and every failed bind is returned */
    static const char* const routes[2] = { "(+ a b)", "(+ (as 'I64 (nil? x)) (+ a b))" };
    for (int ri = 0; ri < 2; ri++) {
        ray_t* out[2] = { NULL, NULL };
        int nbinds[2] = { 0, 0 };
        for (int gi = 0; gi < 2; gi++) {
            ray_t* t = cp_read(gi * 2);
            cp_expr_ctx_t cx = { t, routes[ri] };
            uint64_t ok0 = ray_expr_compile_ok;
            out[gi] = cp_expr_body(&cx);
            TEST_ASSERT_TRUE(out[gi] && !RAY_IS_ERR(out[gi]));
            TEST_ASSERT_TRUE(ray_expr_compile_ok > ok0);                  /* the compile ran */
            bool all_err = false;
            nbinds[gi] = cp_bind_sweep(cp_expr_body, &cx, &all_err);
            TEST_ASSERT_TRUE(all_err);
            TEST_ASSERT_FMT(nbinds[gi] == (ri == 0 ? 2 : 3), "route %d: %d binds", ri, nbinds[gi]);   /* every bind of a clean run fails in turn */
            ray_release(t);
        }
        TEST_ASSERT_EQ_I(nbinds[0], nbinds[1]);
        TEST_ASSERT_TRUE(out[0]->len == out[1]->len);
        TEST_ASSERT_TRUE(memcmp(ray_data(out[0]), ray_data(out[1]), (size_t)out[0]->len * 8) == 0);
        ray_release(out[0]); ray_release(out[1]);
    }
    PASS();
}

typedef struct { ray_t* t; const char* key; uint16_t* ops; const char* const* cols; int na; } cp_group_ctx_t;
static ray_t* cp_group_body(void* c) {
    cp_group_ctx_t* x = c;
    ray_graph_t* g = ray_graph_new(x->t);
    ray_op_t* keys[1] = { ray_scan(g, x->key) };
    ray_op_t* ins[4];
    for (int i = 0; i < x->na; i++) ins[i] = ray_scan(g, x->cols[i]);
    ray_t* r = ray_execute(g, ray_group(g, keys, 1, x->ops, ins, (uint8_t)x->na));
    if (r && ray_is_lazy(r)) r = ray_lazy_materialize(r);
    ray_graph_free(g);
    return r;
}

typedef struct { int64_t key; int64_t row; } cp_krow_t;
static int cp_krow_cmp(const void* a, const void* b) {
    int64_t x = ((const cp_krow_t*)a)->key, y = ((const cp_krow_t*)b)->key;
    return x < y ? -1 : x > y;
}

/* Group results equal as maps from key to the 8-byte aggregate cells. */
static bool cp_groups_same(ray_t* x, ray_t* y, int64_t want_groups, int ncols) {
    if (!x || !y || RAY_IS_ERR(x) || RAY_IS_ERR(y)) return false;
    if (ray_table_nrows(x) != want_groups || ray_table_nrows(y) != want_groups) return false;
    cp_krow_t* kx = ray_alloc_raw((size_t)want_groups * sizeof *kx);
    cp_krow_t* ky = ray_alloc_raw((size_t)want_groups * sizeof *ky);
    bool ok = kx && ky;
    for (int64_t i = 0; ok && i < want_groups; i++) {
        kx[i] = (cp_krow_t){ ((const int64_t*)ray_data(ray_table_get_col_idx(x, 0)))[i], i };
        ky[i] = (cp_krow_t){ ((const int64_t*)ray_data(ray_table_get_col_idx(y, 0)))[i], i };
    }
    if (ok) {
        qsort(kx, (size_t)want_groups, sizeof *kx, cp_krow_cmp);
        qsort(ky, (size_t)want_groups, sizeof *ky, cp_krow_cmp);
        for (int64_t i = 0; ok && i < want_groups; i++) {
            if (kx[i].key != ky[i].key) { ok = false; break; }
            for (int c = 1; c <= ncols; c++) {
                const int64_t* cx = ray_data(ray_table_get_col_idx(x, c));
                const int64_t* cy = ray_data(ray_table_get_col_idx(y, c));
                if (cx[kx[i].row] != cy[ky[i].row]) { ok = false; break; }
            }
        }
    }
    ray_free_raw(kx); ray_free_raw(ky);
    return ok;
}

/* agg_desc_init, exercised through every parallel strategy that builds a
 * descriptor (dense, small-hash/table-stack, radix, shared sample, partition
 * payload) over bound sources: generation 0 and 2 answer the same, the route
 * is the expected one, and a failed bind of any value column is the
 * query's error. */
static test_result_t test_group_desc_bound(void) {
    TEST_ASSERT_TRUE(cp_save());
    uint32_t cores = ray_pool_total_workers(ray_pool_get());
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(2), RAY_OK);
    static uint16_t sum_avg[2] = { OP_SUM, OP_AVG };
    static uint16_t min_max[2] = { OP_MIN, OP_MAX };
    static const char* const cols_af[2] = { "a", "f" };
    static const struct { const char* key; uint16_t* ops; int64_t groups; agg_route_t route; } cases[] = {
        { "k0", sum_avg, 4,       AGG_ROUTE_V2_DENSE },
        { "k1", sum_avg, 4,       AGG_ROUTE_V2_TABLESTACK },
        { "k4", sum_avg, CP_N,    AGG_ROUTE_V2_RADIX },
        { "k2", sum_avg, 8192,    AGG_ROUTE_V2_DENSE },       /* partition payload estimate */
        { "k2", min_max, 8192,    AGG_ROUTE_V2_DENSE },       /* shared-state sample */
    };
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        ray_t* out[2] = { NULL, NULL };
        for (int gi = 0; gi < 2; gi++) {
            ray_t* t = cp_read(gi * 2);
            TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
            cp_group_ctx_t cx = { t, cases[c].key, cases[c].ops, cols_af, 2 };
            agg_route_reset();
            out[gi] = cp_group_body(&cx);
            agg_route_stats_t st = agg_route_stats();
            TEST_ASSERT_FMT(out[gi] && !RAY_IS_ERR(out[gi]), "case %zu gen %d ran", c, gi * 2);
            TEST_ASSERT_FMT(st.routes[cases[c].route] == 1, "case %zu gen %d route", c, gi * 2);
            /* every failure before the first success is the bind error: both value columns */
            bool all_err = false;
            int nb = cp_bind_sweep(cp_group_body, &cx, &all_err);
            TEST_ASSERT_FMT(all_err && nb == 2, "case %zu gen %d sweep nb=%d all_err=%d", c, gi * 2, nb, all_err);
            ray_release(t);
        }
        TEST_ASSERT_FMT(cp_groups_same(out[0], out[1], cases[c].groups, 2), "case %zu answers differ", c);
        ray_release(out[0]); ray_release(out[1]);
    }
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(cores), RAY_OK);
    PASS();
}

typedef struct {
    ray_t* t; ray_t* where; int mode;          /* 0 top-k, 1 take, 2 take from the end */
    int64_t sk[2]; uint8_t desc[2]; int64_t outs[4];
} cp_topk_ctx_t;
static ray_t* cp_topk_body(void* c) {
    cp_topk_ctx_t* x = c;
    if (x->mode == 0)
        return ray_fused_topk_select(x->t, x->where, x->sk, x->desc, 2, 10, x->outs, NULL, 4);
    return ray_fused_take_select(x->t, x->where, x->mode == 1 ? 7 : -7, x->outs, NULL, 4);
}

/* The fused top-k and take executors on bound sources: sort keys and gathers
 * (generation 0 and 2, direct on a table whose slots are encoded) give the
 * same rows, run (not decline), and return a failed bind of a sort key, a
 * predicate column or a gathered column. */
static test_result_t test_topk_bound(void) {
    TEST_ASSERT_TRUE(fq_save("tk"));
    int64_t si = ray_sym_intern("i", 1), sv = ray_sym_intern("v", 1), sd = ray_sym_intern("d", 1);
    int64_t ss = ray_sym_intern("s", 1);
    struct { int mode; int64_t k0, k1; uint8_t d0; const char* where; int min_binds; } cases[] = {
        { 0, sv, si, 1, "(> v 5)",  4 },     /* nullable I64 desc, then i; where on v */
        { 0, ss, si, 0, "(!= s 'beta)", 4 }, /* SYM sort key (file domain at generation 2) */
        { 0, sd, si, 1, "(< d 2.0)", 4 },    /* F64 desc with nulls */
        { 1, 0, 0, 0, "(> v 5)", 4 },        /* take from the front */
        { 2, 0, 0, 0, "(== s 'gamma)", 4 },  /* take from the end */
    };
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        ray_t* out[2] = { NULL, NULL };
        int nbinds[2] = { 0, 0 };
        for (int gi = 0; gi < 2; gi++) {
            ray_t* t = ray_read_splayed(gi ? ray_test_tmp(CS_TMP "/tk2") : ray_test_tmp(CS_TMP "/tk0"), gi ? ray_test_tmp(CS_TMP "/tk2.sym") : ray_test_tmp(CS_TMP "/tk0.sym"));
            TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
            TEST_ASSERT_EQ_I(ray_table_get_col_enc(t, si) != NULL, gi);
            ray_t* w = ray_parse(cases[c].where);
            cp_topk_ctx_t cx = { .t = t, .where = w, .mode = cases[c].mode,
                                 .sk = { cases[c].k0, cases[c].k1 }, .desc = { cases[c].d0, 0 },
                                 .outs = { si, sv, sd, ss } };
            ray_fused_topk_counters_reset();
            out[gi] = cp_topk_body(&cx);
            uint64_t ran, dec; ray_fused_topk_counters(&ran, &dec);
            TEST_ASSERT_FMT(out[gi] && !RAY_IS_ERR(out[gi]) && ran == 1 && dec == 0,
                            "case %zu gen %d ran=%llu declined=%llu", c, gi * 2,
                            (unsigned long long)ran, (unsigned long long)dec);
            bool all_err = false;
            nbinds[gi] = cp_bind_sweep(cp_topk_body, &cx, &all_err);
            TEST_ASSERT_FMT(all_err && nbinds[gi] == cases[c].min_binds, "case %zu gen %d sweep nb=%d all_err=%d",
                            c, gi * 2, nbinds[gi], all_err);
            ray_release(w);
            ray_release(t);
        }
        TEST_ASSERT_EQ_I(nbinds[0], nbinds[1]);
        TEST_ASSERT_FMT(cp_fmt_same(out[0], out[1]), "case %zu rows differ", c);
        TEST_ASSERT_TRUE(ray_table_nrows(out[0]) == (cases[c].mode == 0 ? 10 : 7));
        ray_release(out[0]); ray_release(out[1]);
    }
    /* the plain request of an encoded sort key failing is returned too */
    ray_t* t = ray_read_splayed(ray_test_tmp(CS_TMP "/tk2"), ray_test_tmp(CS_TMP "/tk2.sym"));
    ray_t* w = ray_parse("(> v 5)");
    cp_topk_ctx_t cx = { .t = t, .where = w, .mode = 0, .sk = { sv, si }, .desc = { 1, 0 }, .outs = { si, sv, sd, ss } };
    ray_enccol_test_fail_alloc(1);
    ray_t* r = cp_topk_body(&cx);
    ray_enccol_test_fail_alloc(0);
    bool plain_err = r && RAY_IS_ERR(r);
    if (plain_err) ray_error_free(r); else if (r) ray_release(r);
    ray_release(w); ray_release(t);
    TEST_ASSERT_TRUE(plain_err);
    PASS();
}


/* ---- Task 27: executor, filter, sort, window, pivot sites ---- */

/* `n` rows of a (generation 2, 3 i) and b (generation 0, 5 i); with `corrupt`
 * the descriptor of a's first vector is damaged after the load checks pass,
 * so a loads and fails to materialize. */
static ray_t* ex_table(int64_t n, bool corrupt) {
    char p[256];
    (void)ray_test_rm_rf(ray_test_tmp(CS_TMP "/ex"));
    (void)ray_test_mkdir_p(ray_test_tmp(CS_TMP "/ex"));
    ray_t* a = i64_col(n, 3); ray_t* b = i64_col(n, 5);
    snprintf(p, sizeof p, "%s/ex/a", ray_test_tmp(CS_TMP)); ray_col_save_gen(a, p, 2);
    snprintf(p, sizeof p, "%s/ex/b", ray_test_tmp(CS_TMP)); ray_col_save_gen(b, p, 0);
    ray_release(a); ray_release(b);
    ray_t* names = ray_vec_new(RAY_STR, 2);
    names = ray_str_vec_append(names, "a", 1); names = ray_str_vec_append(names, "b", 1);
    snprintf(p, sizeof p, "%s/ex/.d", ray_test_tmp(CS_TMP)); ray_col_save(names, p); ray_release(names);
    if (corrupt) {
        int64_t n_chunks = (n + 65535) / 65536;
        FILE* f = fopen(ray_test_tmp(CS_TMP "/ex/a"), "r+b");
        if (!f) ray_test_fatal("cannot open %s", ray_test_tmp(CS_TMP "/ex/a"));
        fseek(f, (long)(96 + 64 * n_chunks + 6), SEEK_SET); fputc(1, f); fclose(f);
    }
    return ray_read_splayed(ray_test_tmp(CS_TMP "/ex"), NULL);
}

static test_result_t ex_corrupt(ray_t* r, const char* what) {
    TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: expected an error", what);
    TEST_ASSERT_FMT(strcmp(ray_err_code(r), "corrupt") == 0, "%s: code %s (%s)", what, ray_err_code(r),
                    ray_error_msg() ? ray_error_msg() : "no message");
    TEST_ASSERT_FMT(ray_error_msg() && strstr(ray_error_msg(), ray_test_tmp(CS_TMP "/ex/a")), "%s: message %s", what,
                    ray_error_msg() ? ray_error_msg() : "(none)");
    ray_error_free(r);
    PASS();
}

typedef enum { EX_HEAD, EX_TAIL, EX_FILTER, EX_FILTER_SOME, EX_SORT_B, EX_SORT_A, EX_TOPK_B,
               EX_PIVOT, EX_WINDOW_FN_A, EX_SELECT_PASS, EX_WINDOW_PASS, EX_MERGE, EX_FILTER_NONE,
               EX_HEAD_FILTER, EX_EAGER_FILTER, EX_TOPK_A, EX_N } ex_case_t;

static ray_t* ex_run(ray_t* t, ex_case_t k) {
    if (k == EX_MERGE) return ray_result_merge(t, t);
    if (k == EX_EAGER_FILTER) {                   /* the eager gather: the lazy path never reaches it over a table */
        int64_t n = ray_table_nrows(t);
        ray_t* pred = ray_vec_new(RAY_BOOL, n); pred->len = n;
        for (int64_t i = 0; i < n; i++) ((uint8_t*)ray_data(pred))[i] = i % 3 != 0;
        ray_error_clear();
        ray_t* r = exec_filter(NULL, NULL, t, pred);
        ray_release(pred);
        return r;
    }
    ray_graph_t* g = ray_graph_new(t);
    ray_op_t* tb = ray_const_table(g, t);
    ray_op_t* sb = ray_scan(g, "b");
    ray_op_t* root = NULL;
    uint8_t nf0[1] = { 0 }, d0[1] = { 0 };
    switch (k) {
    case EX_HEAD:  root = ray_head(g, tb, 5); break;
    case EX_TAIL:  root = ray_tail(g, tb, 5); break;
    case EX_FILTER: root = ray_filter(g, tb, ray_ge(g, sb, ray_const_i64(g, 5))); break;
    case EX_HEAD_FILTER: root = ray_head(g, ray_filter(g, tb, ray_gt(g, sb, ray_const_i64(g, 50))), 4); break;
    case EX_FILTER_SOME: root = ray_filter(g, tb, ray_gt(g, sb, ray_const_i64(g, 500))); break;
    case EX_FILTER_NONE: root = ray_filter(g, tb, ray_lt(g, sb, ray_const_i64(g, -1))); break;
    case EX_SORT_B: { ray_op_t* ks[1] = { sb }; root = ray_sort_op(g, tb, ks, d0, nf0, 1); break; }
    case EX_SORT_A: { ray_op_t* ks[1] = { ray_scan(g, "a") }; root = ray_sort_op(g, tb, ks, d0, nf0, 1); break; }
    case EX_TOPK_A: { ray_op_t* ks[1] = { ray_scan(g, "a") }; root = ray_head(g, ray_sort_op(g, tb, ks, d0, nf0, 1), 3); break; }
    case EX_TOPK_B: { ray_op_t* ks[1] = { sb }; root = ray_head(g, ray_sort_op(g, tb, ks, d0, nf0, 1), 3); break; }
    case EX_PIVOT: { ray_op_t* ix[1] = { sb };
                     root = ray_pivot_op(g, ix, 1, ray_scan(g, "a"), sb, OP_SUM); break; }
    case EX_WINDOW_FN_A: case EX_WINDOW_PASS: {
        ray_op_t* ord[1] = { sb };
        uint8_t kinds[1] = { RAY_WIN_SUM };
        ray_op_t* fin[1] = { k == EX_WINDOW_FN_A ? ray_scan(g, "a") : sb };
        int64_t prm[1] = { 0 };
        root = ray_window_op(g, tb, NULL, 0, ord, d0, 1, kinds, fin, prm, 1, RAY_FRAME_ROWS,
                             RAY_BOUND_UNBOUNDED_PRECEDING, RAY_BOUND_UNBOUNDED_FOLLOWING, 0, 0);
        break; }
    case EX_SELECT_PASS: { ray_op_t* cs[2] = { ray_scan(g, "a"), sb }; root = ray_select_op(g, tb, cs, 2); break; }
    default: break;
    }
    ray_error_clear();
    ray_t* r = root ? ray_execute(g, root) : NULL;
    ray_graph_free(g);
    return r;
}

/* Every executor, filter, sort, window and pivot site that reads a column
 * reports a failed materialization as itself (code and column path); the
 * sites that only pass or measure a column never decode it. */
static test_result_t ex_failed_decode_one(int64_t n, bool big) {
    static const char* const names[EX_N] = { "head", "tail", "filter", "filter-some", "sort-b", "sort-a",
        "top-k-b", "pivot", "window-fn-a", "select-pass", "window-pass", "result-merge", "filter-none",
        "head-filter", "eager-filter", "top-k-a" };
    int64_t sa = ray_sym_intern("a", 1);
    for (int k = 0; k < EX_N; k++) {
        if (big && k != EX_FILTER && k != EX_FILTER_SOME && k != EX_SORT_B && k != EX_EAGER_FILTER) continue;
        ray_t* t = ex_table(n, true);
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_enc_counters_reset();
        ray_t* r = ex_run(t, (ex_case_t)k);
        if (k == EX_SELECT_PASS || k == EX_WINDOW_PASS || k == EX_FILTER_NONE) {
            TEST_ASSERT_FMT(r && !RAY_IS_ERR(r) && r->type == RAY_TABLE, "%s: expected a table", names[k]);
            ray_enc_counters_t c; ray_enc_counters_get(&c);
            if (k == EX_FILTER_NONE) {
                TEST_ASSERT_FMT(ray_table_nrows(r) == 0 && ray_table_ncols(r) == 2, "%s: schema", names[k]);
                TEST_ASSERT_FMT(c.materialized == 0, "%s: materialized %llu", names[k], (unsigned long long)c.materialized);
            } else {
                TEST_ASSERT_FMT(ray_table_get_col_enc(r, sa) != NULL, "%s: a is passed encoded", names[k]);
                TEST_ASSERT_FMT(c.materialized == 0, "%s: materialized %llu", names[k], (unsigned long long)c.materialized);
            }
            ray_release(r);
        } else {
            test_result_t rr = ex_corrupt(r, names[k]);
            if (rr.status != TEST_PASS) { ray_release(t); return rr; }
        }
        ray_release(t);
    }
    PASS();
}

static test_result_t test_exec_sites_failed_decode(void) {
    test_result_t r = ex_failed_decode_one(3000, false);
    if (r.status != TEST_PASS) return r;
    return ex_failed_decode_one(600000, true);        /* the parallel gather */
}

/* A healthy generation-2 table gives the same answers as the plain one at
 * every site above. */
static test_result_t test_exec_sites_gen2_equal(void) {
    ray_t* t2 = ex_table(3000, false);
    TEST_ASSERT_TRUE(t2 && !RAY_IS_ERR(t2));
    ray_t* t0 = ray_table_new(2);
    t0 = ray_table_add_col(t0, ray_sym_intern("a", 1), i64_col(3000, 3));
    t0 = ray_table_add_col(t0, ray_sym_intern("b", 1), i64_col(3000, 5));
    for (int k = 0; k < EX_N; k++) {
        /* pivot: its output columns follow the hash table's order, so rows are compared by the
         * dedicated pivot tests; merge and the eager filter take no graph. */
        if (k == EX_PIVOT || k == EX_MERGE || k == EX_EAGER_FILTER) continue;
        ray_t* r2 = ex_run(t2, (ex_case_t)k);
        ray_t* r0 = ex_run(t0, (ex_case_t)k);
        TEST_ASSERT_FMT(r2 && r0 && !RAY_IS_ERR(r2) && !RAY_IS_ERR(r0), "case %d", k);
        TEST_ASSERT_FMT(cp_fmt_same(r0, r2), "case %d rows differ", k);
        ray_release(r2); ray_release(r0);
    }
    ray_release(t0); ray_release(t2);
    PASS();
}


/* Rows of a graph FILTER on column s (SYM) or v over the fixture table, run on generation 0 or 2. */
static int64_t fq_filter_rows(const char* name, int gen, const char* col, ray_t* atom, ray_enc_counters_t* c) {
    char d[160], sy[170];
    snprintf(d, sizeof d, "%s/%s%d", ray_test_tmp(CS_TMP), name, gen); snprintf(sy, sizeof sy, "%s.sym", d);
    ray_t* t = ray_read_splayed(d, sy);
    if (!t || RAY_IS_ERR(t)) return -1;
    ray_graph_t* g = ray_graph_new(t);
    ray_op_t* root = ray_filter(g, ray_const_table(g, t),
                                ray_eq(g, ray_scan(g, col), ray_const_atom(g, atom)));
    ray_enc_counters_reset();
    ray_t* r = ray_execute(g, root);
    ray_enc_counters_get(c);
    int64_t n = (r && !RAY_IS_ERR(r)) ? (g->selection ? ray_rowsel_meta(g->selection)->total_pass : ray_table_nrows(r)) : -1;
    if (r && !RAY_IS_ERR(r)) ray_release(r);
    ray_graph_free(g);
    ray_release(t);
    return n;
}

/* An equality on a symbol missing from the column's domain selects no rows from the domain alone: a
 * generation-2 column is not decoded to learn that; a present symbol, and a predicate on an encoded
 * column with no index, give the same rows on both generations. */
static test_result_t test_filter_absent_symbol(void) {
    TEST_ASSERT_TRUE(fq_save("as"));
    ray_t* none = ray_sym(ray_sym_intern("nowhere_at_all", 14));
    ray_t* beta = ray_sym(ray_sym_intern("beta", 4));
    ray_t* four = ray_i64(4);
    ray_enc_counters_t c0, c2;
    TEST_ASSERT_EQ_I(fq_filter_rows("as", 0, "s", none, &c0), 0);
    TEST_ASSERT_EQ_I(fq_filter_rows("as", 2, "s", none, &c2), 0);
    TEST_ASSERT_EQ_U(c2.materialized, 0);
    TEST_ASSERT_EQ_I(fq_filter_rows("as", 0, "s", beta, &c0), 750);
    TEST_ASSERT_EQ_I(fq_filter_rows("as", 2, "s", beta, &c2), 750);
    TEST_ASSERT_TRUE(c2.materialized >= 1);                /* the scan binds s; the result gather reads the rest */
    int64_t v0 = fq_filter_rows("as", 0, "v", four, &c0);
    int64_t v2 = fq_filter_rows("as", 2, "v", four, &c2);
    TEST_ASSERT_TRUE(v0 > 0 && v0 == v2);
    TEST_ASSERT_TRUE(c2.materialized <= 5);                /* no index: each column decodes at most once */
    ray_release(none); ray_release(beta); ray_release(four);
    PASS();
}

/* ---- Task 23: pruned parted execution ---- */

/* Three partitions 2024.01.01..03 (dates 8766..8768).  Partition p holds a =
 * i * (3 + p), b = 5 i + p over rows[p] rows (3000 for pdb_make); bit p of `gen2` selects
 * generation 2, and `ragged` drops column b from the last partition. */
static ray_t* pdb_make_rows(const char* root, unsigned gen2, bool ragged, const int64_t rows[3]) {
    for (int p = 0; p < 3; p++) {
        char dir[256]; snprintf(dir, sizeof dir, "%s/2024.01.0%d/t", root, p + 1);
        ray_t* t = ray_table_new(2);
        ray_t* a = i64_col(rows[p], 3 + p);
        ray_t* b = i64_col(rows[p], 5);
        for (int64_t i = 0; i < rows[p]; i++) ((int64_t*)ray_data(b))[i] += p;
        t = ray_table_add_col(t, ray_sym_intern("a", 1), a);
        if (!(ragged && p == 2)) t = ray_table_add_col(t, ray_sym_intern("b", 1), b);
        ray_release(a); ray_release(b);
        if (ray_splay_save_mode(t, dir, NULL, (gen2 >> p) & 1 ? RAY_COMPRESS_ON : RAY_COMPRESS_OFF) != RAY_OK) {
            ray_release(t); return NULL;
        }
        ray_release(t);
    }
    ray_t* t = ray_read_parted(root, "t");
    return (t && !RAY_IS_ERR(t)) ? t : NULL;
}
static ray_t* pdb_make(const char* root, unsigned gen2, bool ragged) {
    static const int64_t rows[3] = { 3000, 3000, 3000 };
    return pdb_make_rows(root, gen2, ragged, rows);
}

/* Predicates over the partition key and the roots under test. */
enum { Q_EQ_02, Q_LE_02, Q_01_03, Q_NONE };
enum { R_SUM, R_A, R_DATE };

/* The shared plan: filter(root expr, pred) over default-table scans (a
 * const-table root never streams).  Reports whether the graph's bind list
 * is empty once the run is over (per-segment tables unbound). */
static ray_t* pruned_run(ray_t* t, int q, int root_kind, bool* binds_clean) {
    ray_graph_t* g = ray_graph_new(t);
    ray_t* l1 = ray_date(8766); ray_t* l2 = ray_date(8767); ray_t* l3 = ray_date(8768);
    ray_op_t* pred = NULL;
    switch (q) {
    case Q_EQ_02: pred = ray_eq(g, ray_scan(g, "date"), ray_const_atom(g, l2)); break;
    case Q_LE_02: pred = ray_le(g, ray_scan(g, "date"), ray_const_atom(g, l2)); break;
    case Q_01_03:
        pred = ray_or(g, ray_eq(g, ray_scan(g, "date"), ray_const_atom(g, l1)),
                         ray_eq(g, ray_scan(g, "date"), ray_const_atom(g, l3)));
        break;
    default: pred = ray_gt(g, ray_scan(g, "date"), ray_const_atom(g, l3)); break;
    }
    ray_op_t* e = root_kind == R_SUM ? ray_add(g, ray_scan(g, "a"), ray_scan(g, "b"))
                : root_kind == R_A ? ray_scan(g, "a") : ray_scan(g, "date");
    ray_op_t* root = ray_optimize(g, ray_filter(g, e, pred));
    ray_t* out = ray_execute(g, root);
    if (binds_clean) *binds_clean = ray_graph_binds_count(g) == 0;
    ray_release(l1); ray_release(l2); ray_release(l3); ray_graph_free(g);
    return out;
}

/* Same query on two tables: both outcomes are an error with the same code,
 * or vectors of the same type and bytes. */
static bool same_outcome(ray_t* x, ray_t* y) {
    if (!x || !y) return false;
    if (RAY_IS_ERR(x) || RAY_IS_ERR(y))
        return RAY_IS_ERR(x) && RAY_IS_ERR(y) && strcmp(ray_err_code(x), ray_err_code(y)) == 0;
    if (x->type != y->type || x->len != y->len) return false;
    size_t esz = (size_t)ray_type_sizes[(uint8_t)x->type];
    return memcmp(ray_data(x), ray_data(y), (size_t)x->len * esz) == 0;
}
static void drop_out(ray_t* o) { if (!o) return; if (RAY_IS_ERR(o)) ray_error_free(o); else ray_release(o); }

static test_result_t test_parted_pruning_fetch(void) {
    ray_t* t = pdb_make(ray_test_tmp(CS_TMP "/pdb"), 7, false);
    TEST_ASSERT_NOT_NULL(t);
    ray_t* t0 = pdb_make(ray_test_tmp(CS_TMP "/pdb0"), 0, false);
    TEST_ASSERT_NOT_NULL(t0);
    ray_enc_counters_reset();
    bool clean = false;
    ray_t* out = pruned_run(t, Q_EQ_02, R_SUM, &clean);
    TEST_ASSERT_TRUE(out && !RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I(out->len, 3000);
    TEST_ASSERT_TRUE(clean);                             /* per-segment tables unbound */
    ray_enc_counters_t c; ray_enc_counters_get(&c);
    /* columns a and b of the surviving partition are range-read by the
     * fused (+ a b): nothing is materialized */
    TEST_ASSERT_FMT(c.materialized == 0, "materialized %llu segment columns", (unsigned long long)c.materialized);
    const int64_t* d = (const int64_t*)ray_data(out);
    for (int i = 0; i < 3000; i++)
        TEST_ASSERT_EQ_I(d[i], (int64_t)i * 4 + (int64_t)i * 5 + 1);   /* partition 1: 4 i + 5 i + 1 */
    ray_t* o0 = pruned_run(t0, Q_EQ_02, R_SUM, NULL);
    TEST_ASSERT_TRUE(same_outcome(out, o0));
    drop_out(out); drop_out(o0); ray_release(t); ray_release(t0);
    PASS();
}

/* Per-segment execution on generation 0 is unchanged: nothing decoded and
 * the answers match generation 2 for every query shape. */
static test_result_t test_parted_pruning_gen0(void) {
    ray_t* t0 = pdb_make(ray_test_tmp(CS_TMP "/p0"), 0, false);
    ray_t* t2 = pdb_make(ray_test_tmp(CS_TMP "/p2"), 7, false);
    TEST_ASSERT_TRUE(t0 && t2);
    ray_enc_counters_reset();
    ray_t* o0 = pruned_run(t0, Q_EQ_02, R_SUM, NULL);
    ray_enc_counters_t c; ray_enc_counters_get(&c);
    TEST_ASSERT_EQ_U(c.materialized, 0);
    TEST_ASSERT_TRUE(o0 && !RAY_IS_ERR(o0) && o0->len == 3000);
    ray_t* o2 = pruned_run(t2, Q_EQ_02, R_SUM, NULL);
    TEST_ASSERT_TRUE(same_outcome(o0, o2));
    drop_out(o0); drop_out(o2); ray_release(t0); ray_release(t2);
    PASS();
}

/* Nothing survives: the empty result has generation 0's schema (the key's
 * DATE type from the MAPCOMMON, the I64 base of the parted columns) and
 * nothing is decoded. */
static test_result_t test_parted_pruning_empty(void) {
    ray_t* t0 = pdb_make(ray_test_tmp(CS_TMP "/e0"), 0, false);
    ray_t* t2 = pdb_make(ray_test_tmp(CS_TMP "/e2"), 7, false);
    TEST_ASSERT_TRUE(t0 && t2);
    static const int8_t want[] = { RAY_I64, RAY_I64, RAY_DATE };
    for (int rk = R_SUM; rk <= R_DATE; rk++) {
        ray_enc_counters_reset();
        bool clean = false;
        ray_t* o2 = pruned_run(t2, Q_NONE, rk, &clean);
        ray_enc_counters_t c; ray_enc_counters_get(&c);
        ray_t* o0 = pruned_run(t0, Q_NONE, rk, NULL);
        TEST_ASSERT_TRUE(o0 && o2 && !RAY_IS_ERR(o0) && !RAY_IS_ERR(o2));
        TEST_ASSERT_EQ_I(o2->len, 0);
        TEST_ASSERT_EQ_I(o2->type, want[rk]);
        TEST_ASSERT_EQ_I(o0->type, o2->type);
        TEST_ASSERT_EQ_I(o0->len, o2->len);
        TEST_ASSERT_EQ_U(c.materialized, 0);
        TEST_ASSERT_TRUE(clean);
        ray_release(o0); ray_release(o2);
    }
    ray_release(t0); ray_release(t2);
    PASS();
}

static test_result_t test_parted_pruning_multi(void) {
    ray_t* t0 = pdb_make(ray_test_tmp(CS_TMP "/m0"), 0, false);
    ray_t* t2 = pdb_make(ray_test_tmp(CS_TMP "/m2"), 7, false);
    TEST_ASSERT_TRUE(t0 && t2);
    ray_enc_counters_reset();
    bool clean = false;
    ray_t* o2 = pruned_run(t2, Q_LE_02, R_SUM, &clean);
    ray_enc_counters_t c; ray_enc_counters_get(&c);
    TEST_ASSERT_TRUE(o2 && !RAY_IS_ERR(o2) && o2->len == 6000);
    TEST_ASSERT_TRUE(clean);
    /* the fused (+ a b) range-reads both partitions' columns */
    TEST_ASSERT_FMT(c.materialized == 0, "materialized %llu segment columns", (unsigned long long)c.materialized);
    ray_t* o0 = pruned_run(t0, Q_LE_02, R_SUM, NULL);
    TEST_ASSERT_TRUE(same_outcome(o0, o2));
    drop_out(o0); drop_out(o2); ray_release(t0); ray_release(t2);
    PASS();
}

/* The second surviving segment fails: the query returns that segment's
 * error (code and path), no partial table, and nothing leaks. */
static test_result_t test_parted_pruning_failure(void) {
    ray_mem_stats_t m0, m1;
    ray_t* t = pdb_make(ray_test_tmp(CS_TMP "/f2"), 7, false);
    TEST_ASSERT_NOT_NULL(t);
    ray_release(t);
    uint8_t one = 1;
    FILE* f = fopen(ray_test_tmp(CS_TMP "/f2/2024.01.02/t/a"), "r+b");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 160 + 6, SEEK_SET); fwrite(&one, 1, 1, f); fclose(f);
    ray_mem_stats(&m0);
    t = ray_read_parted(ray_test_tmp(CS_TMP "/f2"), "t");                 /* corruption passes load */
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_t* out = pruned_run(t, Q_LE_02, R_SUM, NULL);
    TEST_ASSERT_TRUE(out && RAY_IS_ERR(out));
    TEST_ASSERT_STR_EQ(ray_err_code(out), "corrupt");
    TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), "2024.01.02/t/a"));
    ray_error_free(out);
    /* a deterministic failure is cached: the next run reports it again */
    out = pruned_run(t, Q_LE_02, R_SUM, NULL);
    TEST_ASSERT_TRUE(out && RAY_IS_ERR(out));
    TEST_ASSERT_STR_EQ(ray_err_code(out), "corrupt");
    ray_error_free(out);
    ray_release(t);
    ray_mem_stats(&m1);
    TEST_ASSERT_EQ_U(m1.bytes_allocated, m0.bytes_allocated);
    PASS();
}

/* Mixed generations and a ragged tail partition go through the
 * per-segment tables like all generation 0. */
static test_result_t test_parted_pruning_mixed(void) {
    static const unsigned masks[] = { 0x5, 0x2 };
    for (int k = 0; k < 2; k++) {
        for (int rag = 0; rag < 2; rag++) {
            char r0[128], rm[128];
            snprintf(r0, sizeof r0, ray_test_tmp(CS_TMP "/x0_%d_%d"), k, rag);
            snprintf(rm, sizeof rm, ray_test_tmp(CS_TMP "/xm_%d_%d"), k, rag);
            ray_t* t0 = pdb_make(r0, 0, rag);
            ray_t* tm = pdb_make(rm, masks[k], rag);
            TEST_ASSERT_TRUE(t0 && tm);
            for (int q = Q_EQ_02; q <= Q_NONE; q++) {
                for (int rk = R_SUM; rk <= R_A; rk++) {
                    bool clean = false;
                    ray_t* a = pruned_run(t0, q, rk, NULL);
                    ray_t* b = pruned_run(tm, q, rk, &clean);
                    TEST_ASSERT_FMT(same_outcome(a, b), "mask %u ragged %d query %d root %d", masks[k], rag, q, rk);
                    TEST_ASSERT_TRUE(clean);
                    /* ragged too (#724): 2024.01.03's missing b reads as nulls */
                    if (rk == R_SUM && q == Q_01_03) TEST_ASSERT_TRUE(!RAY_IS_ERR(b) && b->len == 6000);
                    drop_out(a); drop_out(b);
                }
            }
            ray_release(t0); ray_release(tm);
        }
    }
    PASS();
}


/* The slice-group kernel's shape admission binds the product columns
 * (sg_shape_eligible through the early settle hook): the answer equals the
 * plain-sym table's and a failed bind is the query's error. */
static test_result_t test_slice_group_bound(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set SY ['aa 'bb 'cc 'dd 'ee 'ff 'gg 'hh])"), "");
    TEST_ASSERT_STR_EQ(ev_code("(set KS (as 'SYM (at SY (% (til 4000) 8))))"), "");
    TEST_ASSERT_STR_EQ(ev_code("(set FF (* 0.5 (as 'F64 (% (til 4000) 64))))"), "");
    TEST_ASSERT_STR_EQ(ev_code("(set GG (* 0.25 (as 'F64 (% (til 4000) 32))))"), "");
    TEST_ASSERT_STR_EQ(ev_code("(set TP (table [s f g] (list KS FF GG)))"), "");
    TEST_ASSERT_STR_EQ(ev_code("(set TI (table [s f g] (list (.attr.set 'grouped KS) FF GG)))"), "");
    const char* qi = "(select {sv: (sum (* f g)) from: TI by: s asc: s where: (in s ['aa 'cc])})";
    const char* qp = "(select {sv: (sum (* f g)) from: TP by: s asc: s where: (in s ['aa 'cc])})";
    uint64_t hits0 = ray_idx_hits[IDX_SITE_GROUP_SLICE];
    ray_t* a = ray_eval_str(qi);
    ray_t* b = ray_eval_str(qp);
    TEST_ASSERT_TRUE(ray_idx_hits[IDX_SITE_GROUP_SLICE] == hits0 + 1);        /* the slice path ran once */
    TEST_ASSERT_TRUE(cp_fmt_same(a, b) && ray_table_nrows(a) == 2);
    ray_release(a); ray_release(b);
    ray_graph_bind_test_fail_alloc(1);
    a = ray_eval_str(qi);
    ray_graph_bind_test_fail_alloc(0);
    TEST_ASSERT_TRUE(cp_is_bind_error(a));
    ray_error_free(a);
    PASS();
}

/* ---- Task 28: group-by and the aggregate engine ---- */

typedef struct { ray_t* t; const char* key; uint16_t op; const char* c1; const char* c2; } gp_ctx_t;
static ray_t* gp_body(void* c) {
    gp_ctx_t* x = c;
    ray_graph_t* g = ray_graph_new(x->t);
    ray_op_t* keys[1] = { ray_scan(g, x->key) };
    uint16_t ops[1] = { x->op };
    ray_op_t* ins[1] = { ray_scan(g, x->c1) };
    ray_op_t* ins2[1] = { x->c2 ? ray_scan(g, x->c2) : NULL };
    ray_t* r = ray_execute(g, ray_group_build(g, keys, 1, ops, ins, x->c2 ? ins2 : NULL, NULL, 1));
    if (r && ray_is_lazy(r)) r = ray_lazy_materialize(r);
    ray_graph_free(g);
    return r;
}

/* Every group-by entry that reads a column reports a failed materialization
 * as itself (code and column path), on a generation-2 table loaded from disk
 * and on a select result whose slot is passed encoded. */
static test_result_t gp_failed_decode_one(int64_t n) {
    static const struct { const char* key; uint16_t op; const char* c1; const char* c2; const char* what; } cases[] = {
        { "a", OP_COUNT,         "b", NULL, "key a" },
        { "b", OP_SUM,           "a", NULL, "sum a" },
        { "b", OP_MEDIAN,        "a", NULL, "median a (indexed)" },
        { "b", OP_PEARSON_CORR,  "a", "b",  "pearson a b" },
        { "b", OP_PEARSON_CORR,  "b", "a",  "pearson b a" },
        { "b", OP_FIRST,         "a", NULL, "first a" },
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        for (int via_select = 0; via_select < 2; via_select++) {
            ray_t* t = ex_table(n, true);
            TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
            ray_t* in = t;
            if (via_select) {
                in = ex_run(t, EX_SELECT_PASS);                 /* a passed encoded into the result */
                TEST_ASSERT_TRUE(in && !RAY_IS_ERR(in) && in->type == RAY_TABLE);
                TEST_ASSERT_TRUE(ray_table_get_col_enc(in, ray_sym_intern("a", 1)) != NULL);
            }
            gp_ctx_t cx = { in, cases[k].key, cases[k].op, cases[k].c1, cases[k].c2 };
            ray_error_clear();
            ray_t* r = gp_body(&cx);
            char what[96]; snprintf(what, sizeof what, "%s%s", cases[k].what, via_select ? " (select result)" : "");
            test_result_t rr = ex_corrupt(r, what);
            if (in != t) ray_release(in);
            ray_release(t);
            if (rr.status != TEST_PASS) return rr;
        }
    }
    PASS();
}

static test_result_t test_group_sites_failed_decode(void) {
    test_result_t r = gp_failed_decode_one(3000);
    if (r.status != TEST_PASS) return r;
    return gp_failed_decode_one(200000);
}

/* A healthy generation-2 table, directly and through a select result, groups
 * to the generation-0 answer, and takes the same route with the same
 * planning outcome. */
static test_result_t gp_same_routes(ray_t* t0, ray_t* t2, const char* key, uint16_t op, const char* c1,
                                    const char* c2, const char* what) {
    gp_ctx_t c0 = { t0, key, op, c1, c2 }, cg = { t2, key, op, c1, c2 };
    agg_route_reset();
    ray_t* r0 = gp_body(&c0);
    agg_route_stats_t s0 = agg_route_stats();
    agg_route_reset();
    ray_t* r2 = gp_body(&cg);
    agg_route_stats_t s2 = agg_route_stats();
    TEST_ASSERT_FMT(r0 && r2 && !RAY_IS_ERR(r0) && !RAY_IS_ERR(r2), "%s: ran", what);
    TEST_ASSERT_FMT(memcmp(s0.routes, s2.routes, sizeof s0.routes) == 0, "%s: routes differ", what);
    TEST_ASSERT_FMT(s0.last_v2_reason == s2.last_v2_reason && s0.nullable_key == s2.nullable_key &&
                    s0.dense_plan_available == s2.dense_plan_available &&
                    s0.dense_strategy == s2.dense_strategy && s0.dense_tasks == s2.dense_tasks,
                    "%s: planning differs", what);
    TEST_ASSERT_FMT(cp_fmt_same(r0, r2), "%s: answers differ", what);
    ray_release(r0); ray_release(r2);
    PASS();
}

static test_result_t test_group_sites_routes_same(void) {
    TEST_ASSERT_TRUE(cp_save());
    uint32_t cores = ray_pool_total_workers(ray_pool_get());
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(2), RAY_OK);
    ray_t* t0 = cp_read(0); ray_t* t2 = cp_read(2);
    TEST_ASSERT_TRUE(t0 && t2 && !RAY_IS_ERR(t0) && !RAY_IS_ERR(t2));
    static const struct { const char* key; uint16_t op; const char* c1; const char* c2; } cases[] = {
        { "k0", OP_SUM, "a", NULL },            /* dense */
        { "k1", OP_SUM, "a", NULL },            /* sparse: table-stack */
        { "k2", OP_AVG, "f", NULL },            /* dense, 8192 groups */
        { "k4", OP_SUM, "a", NULL },            /* radix */
        { "f",  OP_SUM, "a", NULL },            /* F64 key: indexed */
        { "x",  OP_SUM, "a", NULL },            /* nullable SYM key */
        { "k0", OP_MEDIAN, "a", NULL },         /* holistic: indexed */
        { "k0", OP_FIRST, "a", NULL },          /* first/last */
        { "k0", OP_PEARSON_CORR, "a", "b" },    /* binary */
        { "k0", OP_COUNT, "a", NULL },
    };
    test_result_t res = { TEST_PASS, NULL };
    for (size_t i = 0; i < sizeof cases / sizeof *cases && res.status == TEST_PASS; i++) {
        char w[64]; snprintf(w, sizeof w, "case %zu (%s)", i, cases[i].key);
        res = gp_same_routes(t0, t2, cases[i].key, cases[i].op, cases[i].c1, cases[i].c2, w);
    }
    /* the same shapes over a select result whose slots are encoded */
    if (res.status == TEST_PASS) {
        ray_graph_t* g = ray_graph_new(t2);
        ray_op_t* cs[3] = { ray_scan(g, "k0"), ray_scan(g, "k4"), ray_scan(g, "a") };
        ray_t* sel = ray_execute(g, ray_select_op(g, ray_const_table(g, t2), cs, 3));
        ray_graph_free(g);
        TEST_ASSERT_TRUE(sel && !RAY_IS_ERR(sel) && sel->type == RAY_TABLE);
        TEST_ASSERT_TRUE(ray_table_get_col_enc(sel, ray_sym_intern("a", 1)) != NULL);
        res = gp_same_routes(t0, sel, "k0", OP_SUM, "a", NULL, "select result k0");
        if (res.status == TEST_PASS) res = gp_same_routes(t0, sel, "k4", OP_SUM, "a", NULL, "select result k4");
        ray_release(sel);
    }
    /* an expression aggregate (exec_group_v2_exprs passes the encoded slots on) */
    if (res.status == TEST_PASS) {
        ray_t* r[2]; agg_route_stats_t st[2];
        for (int gi = 0; gi < 2; gi++) {
            ray_t* t = gi ? t2 : t0;
            ray_graph_t* g = ray_graph_new(t);
            ray_op_t* keys[1] = { ray_scan(g, "k0") };
            uint16_t ops[1] = { OP_SUM };
            ray_op_t* ins[1] = { ray_add(g, ray_scan(g, "a"), ray_mul(g, ray_scan(g, "b"), ray_const_i64(g, 3))) };
            agg_route_reset();
            r[gi] = ray_execute(g, ray_group(g, keys, 1, ops, ins, 1));
            if (r[gi] && ray_is_lazy(r[gi])) r[gi] = ray_lazy_materialize(r[gi]);
            st[gi] = agg_route_stats();
            ray_graph_free(g);
        }
        TEST_ASSERT_TRUE(r[0] && r[1] && !RAY_IS_ERR(r[0]) && !RAY_IS_ERR(r[1]));
        TEST_ASSERT_TRUE(memcmp(st[0].routes, st[1].routes, sizeof st[0].routes) == 0);
        TEST_ASSERT_TRUE(cp_fmt_same(r[0], r[1]));
        ray_release(r[0]); ray_release(r[1]);
    }
    ray_release(t0); ray_release(t2);
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(cores), RAY_OK);
    return res;
}

/* The slice-group route is taken on a table loaded from generation 2 with a
 * recorded index, as it is on generation 0. */
static test_result_t gp_slice_route(const char* dir, bool compress, ray_t** out) {
    TEST_ASSERT_STR_EQ(ev_code("(set SY ['aa 'bb 'cc 'dd 'ee 'ff 'gg 'hh])"), "");
    TEST_ASSERT_STR_EQ(ev_code("(set KS (as 'SYM (at SY (% (til 4000) 8))))"), "");
    TEST_ASSERT_STR_EQ(ev_code("(set FF (* 0.5 (as 'F64 (% (til 4000) 64))))"), "");
    TEST_ASSERT_STR_EQ(ev_code("(set TI (table [s f] (list (.attr.set 'grouped KS) FF)))"), "");
    char cmd[400];
    snprintf(cmd, sizeof cmd, "(.db.splayed.set \"%s\" TI {compress: %s})", dir, compress ? "true" : "false");
    TEST_ASSERT_STR_EQ(ev_code(cmd), "");
    snprintf(cmd, sizeof cmd, "(set TL (.db.splayed.get \"%s\"))", dir);
    TEST_ASSERT_STR_EQ(ev_code(cmd), "");
    uint64_t hits0 = ray_idx_hits[IDX_SITE_GROUP_SLICE];
    *out = ray_eval_str("(select {sv: (sum f) c: (count f) from: TL by: s asc: s where: (in s ['aa 'cc])})");
    TEST_ASSERT_TRUE(*out && !RAY_IS_ERR(*out));
    TEST_ASSERT_FMT(ray_idx_hits[IDX_SITE_GROUP_SLICE] == hits0 + 1, "%s: slice path ran %llu times", compress ? "gen 2" : "gen 0",
                    (unsigned long long)(ray_idx_hits[IDX_SITE_GROUP_SLICE] - hits0));
    PASS();
}

static test_result_t test_group_sites_slice_route(void) {
    ray_t *a = NULL, *b = NULL;
    test_result_t r = gp_slice_route(ray_test_tmp(CS_TMP "/sl0"), false, &a);
    if (r.status != TEST_PASS) return r;
    r = gp_slice_route(ray_test_tmp(CS_TMP "/sl2"), true, &b);
    if (r.status != TEST_PASS) return r;
    TEST_ASSERT_TRUE(cp_fmt_same(a, b) && ray_table_nrows(a) == 2);
    ray_release(a); ray_release(b);
    PASS();
}

/* Parted group-by: the per-partition route (MAPCOMMON key), the concat
 * fallback (a parted key) and the pass of encoded segments take the same
 * routes and give the same answers on generation 0 and 2; a segment that
 * fails to materialize is the query's error. */

/* An expression key ((b % 7), per-partition materialized over the borrowed
 * segments) or an expression aggregate ((a + b)) over a group. */
static ray_t* gp_expr_body(ray_t* t, bool expr_key) {
    ray_graph_t* g = ray_graph_new(t);
    ray_op_t* keys[1] = { expr_key ? ray_mod(g, ray_scan(g, "b"), ray_const_i64(g, 7)) : ray_scan(g, "date") };
    uint16_t ops[1] = { OP_SUM };
    ray_op_t* ins[1] = { expr_key ? ray_scan(g, "a") : ray_add(g, ray_scan(g, "a"), ray_scan(g, "b")) };
    ray_t* r = ray_execute(g, ray_group(g, keys, 1, ops, ins, 1));
    if (r && ray_is_lazy(r)) r = ray_lazy_materialize(r);
    ray_graph_free(g);
    return r;
}
static test_result_t gp_expr_same(ray_t* t0, ray_t* t2, bool expr_key, bool want_parted, const char* what) {
    agg_route_reset(); ray_t* r0 = gp_expr_body(t0, expr_key); agg_route_stats_t s0 = agg_route_stats();
    agg_route_reset(); ray_t* r2 = gp_expr_body(t2, expr_key); agg_route_stats_t s2 = agg_route_stats();
    TEST_ASSERT_FMT(r0 && r2 && !RAY_IS_ERR(r0) && !RAY_IS_ERR(r2), "%s: ran", what);
    if (want_parted) TEST_ASSERT_FMT(s0.routes[AGG_ROUTE_PARTED] >= 1, "%s: parted route", what);
    TEST_ASSERT_FMT(memcmp(s0.routes, s2.routes, sizeof s0.routes) == 0, "%s: routes differ", what);
    TEST_ASSERT_FMT(cp_fmt_same(r0, r2), "%s: answers differ", what);
    ray_release(r0); ray_release(r2);
    PASS();
}
static test_result_t test_group_sites_parted(void) {
    ray_t* t0 = pdb_make(ray_test_tmp(CS_TMP "/gp0"), 0, false);
    ray_t* t2 = pdb_make(ray_test_tmp(CS_TMP "/gp2"), 7, false);
    TEST_ASSERT_TRUE(t0 && t2);
    static const struct { const char* key; uint16_t op; const char* c1; } cases[] = {
        { "date", OP_SUM, "a" },           /* per-partition */
        { "date", OP_AVG, "b" },           /* per-partition with the AVG decomposition */
        { "a",    OP_SUM, "b" },           /* high cardinality: concat fallback */
    };
    test_result_t res = { TEST_PASS, NULL };
    for (size_t i = 0; i < sizeof cases / sizeof *cases && res.status == TEST_PASS; i++) {
        gp_ctx_t c0 = { t0, cases[i].key, cases[i].op, cases[i].c1, NULL };
        gp_ctx_t c2 = { t2, cases[i].key, cases[i].op, cases[i].c1, NULL };
        agg_route_reset(); ray_t* r0 = gp_body(&c0); agg_route_stats_t s0 = agg_route_stats();
        agg_route_reset(); ray_t* r2 = gp_body(&c2); agg_route_stats_t s2 = agg_route_stats();
        TEST_ASSERT_FMT(r0 && r2 && !RAY_IS_ERR(r0) && !RAY_IS_ERR(r2), "case %zu ran", i);
        TEST_ASSERT_FMT(s0.routes[AGG_ROUTE_PARTED] >= 1, "case %zu: parted route", i);
        TEST_ASSERT_FMT(memcmp(s0.routes, s2.routes, sizeof s0.routes) == 0, "case %zu: routes differ", i);
        TEST_ASSERT_FMT(cp_fmt_same(r0, r2), "case %zu: answers differ", i);
        ray_release(r0); ray_release(r2);
    }
    /* a table mixing generations (partitions 1 and 3 on generation 2) and the
     * expression key / aggregate shapes over encoded and mixed segments */
    ray_t* t5 = pdb_make(ray_test_tmp(CS_TMP "/gp5"), 5, false);
    TEST_ASSERT_TRUE(t5);
    for (int i = 0; i < 3 && res.status == TEST_PASS; i++) {
        gp_ctx_t c0 = { t0, cases[i].key, cases[i].op, cases[i].c1, NULL };
        gp_ctx_t c5 = { t5, cases[i].key, cases[i].op, cases[i].c1, NULL };
        agg_route_reset(); ray_t* r0 = gp_body(&c0); agg_route_stats_t s0 = agg_route_stats();
        agg_route_reset(); ray_t* r5 = gp_body(&c5); agg_route_stats_t s5 = agg_route_stats();
        TEST_ASSERT_FMT(r0 && r5 && !RAY_IS_ERR(r0) && !RAY_IS_ERR(r5), "mixed case %d ran", i);
        TEST_ASSERT_FMT(memcmp(s0.routes, s5.routes, sizeof s0.routes) == 0, "mixed case %d: routes differ", i);
        TEST_ASSERT_FMT(cp_fmt_same(r0, r5), "mixed case %d: answers differ", i);
        ray_release(r0); ray_release(r5);
    }
    res = gp_expr_same(t0, t2, true, true, "expression key, encoded");
    if (res.status == TEST_PASS) res = gp_expr_same(t0, t5, true, true, "expression key, mixed");
    if (res.status == TEST_PASS) res = gp_expr_same(t0, t2, false, true, "expression aggregate, encoded");
    if (res.status == TEST_PASS) res = gp_expr_same(t0, t5, false, true, "expression aggregate, mixed");
    ray_release(t5);
    ray_release(t0); ray_release(t2);
    return res;
}

static test_result_t test_group_sites_parted_failed_decode(void) {
    static const struct { const char* key; uint16_t op; const char* c1; const char* what; } cases[] = {
        { "date", OP_SUM, "a", "per-partition" },
        { "a",    OP_SUM, "b", "concat fallback key" },
        { "date", OP_SUM, "b", "per-partition (healthy column)" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        (void)ray_test_rm_rf(ray_test_tmp(CS_TMP "/gpf"));
        ray_t* t = pdb_make(ray_test_tmp(CS_TMP "/gpf"), 7, false);
        TEST_ASSERT_TRUE(t);
        ray_release(t);
        /* damage partition 2's column a, then load again: the damage passes the load checks */
        FILE* f = fopen(ray_test_tmp(CS_TMP "/gpf/2024.01.02/t/a"), "r+b");
        TEST_ASSERT_TRUE(f != NULL);
        fseek(f, 160 + 6, SEEK_SET); fputc(1, f); fclose(f);
        t = ray_read_parted(ray_test_tmp(CS_TMP "/gpf"), "t");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        gp_ctx_t cx = { t, cases[i].key, cases[i].op, cases[i].c1, NULL };
        ray_error_clear();
        ray_t* r = gp_body(&cx);
        if (i == 2) {   /* column a is never read: the answer is the healthy one */
            TEST_ASSERT_FMT(r && !RAY_IS_ERR(r), "%s: expected a result", cases[i].what);
            ray_release(r);
        } else {
            TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: expected an error", cases[i].what);
            TEST_ASSERT_FMT(strcmp(ray_err_code(r), "corrupt") == 0, "%s: code %s", cases[i].what, ray_err_code(r));
            TEST_ASSERT_FMT(ray_error_msg() && strstr(ray_error_msg(), "2024.01.02/t/a"), "%s: message %s",
                            cases[i].what, ray_error_msg() ? ray_error_msg() : "(none)");
            ray_error_free(r);
        }
        ray_release(t);
    }
    PASS();
}

/* ---- Task 29: joins, table ops and Datalog ---- */

/* A table of plain I64 columns saved under `dir` at generation `gen` and
 * loaded back; with `corrupt` >= 0 the descriptor of that column's first
 * vector is damaged after the load checks pass (one chunk, generation 2). */
static ray_t* t29_disk(const char* dir, const char* const* names, const int64_t* mul, int nc, int64_t n,
                       int gen, int corrupt) {
    char p[300];
    (void)ray_test_rm_rf(dir);
    (void)ray_test_mkdir_p(dir);
    ray_t* nm = ray_vec_new(RAY_STR, nc);
    for (int j = 0; j < nc; j++) {
        ray_t* c = i64_col(n, mul[j]);
        snprintf(p, sizeof p, "%s/%s", dir, names[j]);
        ray_col_save_gen(c, p, gen);
        ray_release(c);
        nm = ray_str_vec_append(nm, names[j], strlen(names[j]));
    }
    snprintf(p, sizeof p, "%s/.d", dir);
    ray_col_save(nm, p);
    ray_release(nm);
    if (corrupt >= 0) {
        snprintf(p, sizeof p, "%s/%s", dir, names[corrupt]);
        FILE* f = fopen(p, "r+b");
        if (!f) ray_test_fatal("cannot open %s", p);
        fseek(f, (long)(96 + 64 * ((n + 65535) / 65536) + 6), SEEK_SET); fputc(1, f); fclose(f);
    }
    return ray_read_splayed(dir, NULL);
}

static ray_t* t29_plain(const char* n0, const int64_t* v0, const char* n1, const int64_t* v1, int64_t n) {
    ray_t* t = ray_table_new(2);
    ray_t* a = ray_vec_from_raw(RAY_I64, v0, n);
    ray_t* b = ray_vec_from_raw(RAY_I64, v1, n);
    t = ray_table_add_col(t, ray_sym_intern(n0, strlen(n0)), a);
    t = ray_table_add_col(t, ray_sym_intern(n1, strlen(n1)), b);
    ray_release(a); ray_release(b);
    return t;
}

/* kind 0 join (type jt), 1 anti-join, 2 window join (time key lk, optional eq key). */
static ray_t* t29_join(ray_t* lt, ray_t* rt, int kind, const char* lk, const char* rk, uint8_t jt, const char* eq) {
    ray_graph_t* g = ray_graph_new(lt);
    ray_op_t* ln = ray_const_table(g, lt);
    ray_op_t* rn = ray_const_table(g, rt);
    ray_op_t* root;
    if (kind == 2) {
        ray_op_t* eqk[1] = { eq ? ray_scan(g, eq) : NULL };
        root = ray_asof_join(g, ln, rn, ray_scan(g, lk), eq ? eqk : NULL, eq ? 1 : 0, jt);
    } else {
        ray_op_t* l1[1] = { ray_scan(g, lk) };
        ray_op_t* r1[1] = { ray_scan(g, rk) };
        root = kind == 1 ? ray_antijoin(g, ln, l1, rn, r1, 1) : ray_join(g, ln, l1, rn, r1, 1, jt);
        root = ray_optimize(g, root);
    }
    ray_error_clear();
    ray_t* r = ray_execute(g, root);
    if (r && ray_is_lazy(r)) r = ray_lazy_materialize(r);
    ray_graph_free(g);
    return r;
}

static test_result_t t29_corrupt_at(ray_t* r, const char* what, const char* path) {
    TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: expected an error", what);
    TEST_ASSERT_FMT(strcmp(ray_err_code(r), "corrupt") == 0, "%s: code %s (%s)", what, ray_err_code(r),
                    ray_error_msg() ? ray_error_msg() : "no message");
    TEST_ASSERT_FMT(ray_error_msg() && strstr(ray_error_msg(), path), "%s: message %s", what,
                    ray_error_msg() ? ray_error_msg() : "(none)");
    ray_error_free(r);
    PASS();
}

/* Equal serialized bytes: same shape, names, values and nulls. */
static bool t29_same(ray_t* x, ray_t* y) {
    if (!x || !y || RAY_IS_ERR(x) || RAY_IS_ERR(y)) return false;
    ray_t* sx = ray_ser(x); ray_t* sy = ray_ser(y);
    bool same = sx && sy && !RAY_IS_ERR(sx) && !RAY_IS_ERR(sy) && sx->len == sy->len &&
                memcmp(ray_data(sx), ray_data(sy), (size_t)sx->len) == 0;
    if (sx && !RAY_IS_ERR(sx)) ray_release(sx);
    if (sy && !RAY_IS_ERR(sy)) ray_release(sy);
    return same;
}

/* Every join entry that reads a column of an input reports a failed
 * materialization as itself (code and column path), on a generation-2 table
 * loaded from disk and on a select result whose slot is passed encoded. */
static test_result_t t29_join_failed_one(int64_t n) {
    int64_t pv[3000], qv[3000];
    int64_t m = n < 3000 ? n : 3000;
    for (int64_t i = 0; i < m; i++) { pv[i] = 5 * i; qv[i] = i; }
    ray_t* P = t29_plain("b", pv, "c", qv, m);       /* b matches L.b */
    ray_t* Q = t29_plain("a", pv, "c", qv, m);       /* a matches nothing needed; key on a */
    static const struct { int kind; bool left_is_l; const char* key; uint8_t jt; const char* eq; const char* what; } cs[] = {
        { 0, true,  "b", 0, NULL, "inner, a gathered from the left" },
        { 0, true,  "b", 1, NULL, "left join, a gathered from the left" },
        { 0, false, "b", 0, NULL, "inner, a gathered from the right" },
        { 0, true,  "a", 0, NULL, "inner, a is the left key" },
        { 0, false, "a", 0, NULL, "inner, a is the right key" },
        { 1, true,  "b", 0, NULL, "anti, a gathered" },
        { 1, true,  "a", 0, NULL, "anti, a is the left key" },
        { 1, false, "a", 0, NULL, "anti, a is the right key" },
        { 2, true,  "b", 0, NULL, "window join, a gathered from the left" },
        { 2, true,  "a", 0, NULL, "window join, a is the time key" },
        { 2, false, "b", 0, NULL, "window join, a gathered from the right" },
        { 2, true,  "b", 0, "a",  "window join, a is the equality key" },
    };
    for (size_t k = 0; k < sizeof cs / sizeof *cs; k++) {
        for (int via_select = 0; via_select < 2; via_select++) {
            ray_t* t = ex_table(n, true);
            TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
            ray_t* in = t;
            if (via_select) {
                in = ex_run(t, EX_SELECT_PASS);
                TEST_ASSERT_TRUE(in && !RAY_IS_ERR(in) && in->type == RAY_TABLE);
                TEST_ASSERT_TRUE(ray_table_get_col_enc(in, ray_sym_intern("a", 1)) != NULL);
            }
            ray_t* other = strcmp(cs[k].key, "a") == 0 ? Q : P;
            /* the right side of an eq-key window join needs the key too */
            ray_t* Pe = NULL;
            if (cs[k].eq) { Pe = t29_plain("a", pv, "b", qv, m); other = Pe; }
            ray_t* r = cs[k].left_is_l ? t29_join(in, other, cs[k].kind, cs[k].key, cs[k].key, cs[k].jt, cs[k].eq)
                                       : t29_join(other, in, cs[k].kind, cs[k].key, cs[k].key, cs[k].jt, cs[k].eq);
            char what[128]; snprintf(what, sizeof what, "%s%s (n=%lld)", cs[k].what, via_select ? ", select result" : "", (long long)n);
            test_result_t rr = t29_corrupt_at(r, what, ray_test_tmp(CS_TMP "/ex/a"));
            if (Pe) ray_release(Pe);
            if (in != t) ray_release(in);
            ray_release(t);
            if (rr.status != TEST_PASS) { ray_release(P); ray_release(Q); return rr; }
        }
    }
    ray_release(P); ray_release(Q);
    PASS();
}

static test_result_t test_join_sites_failed_decode(void) {
    test_result_t r = t29_join_failed_one(3000);
    if (r.status != TEST_PASS) return r;
    return t29_join_failed_one(200000);
}

/* Generation 0 and 2 (and a select result of generation 2) join to the same
 * rows, and the null-free key proof is taken on exactly the same joins. */
static test_result_t test_join_sites_gen2_equal(void) {
    TEST_ASSERT_TRUE(cp_save());
    ray_t* t0 = cp_read(0); ray_t* t2 = cp_read(2);
    TEST_ASSERT_TRUE(t0 && t2 && !RAY_IS_ERR(t0) && !RAY_IS_ERR(t2));
    int64_t k0v[4] = { 0, 1, 2, 3 }, wv[4] = { 10, 20, 30, 40 };
    ray_t* RK = t29_plain("k0", k0v, "w", wv, 4);
    enum { RT_N = 10000 };
    static int64_t tv[RT_N], rvv[RT_N], rk0[RT_N];
    for (int64_t j = 0; j < RT_N; j++) { tv[j] = j * 7000000; rvv[j] = j; rk0[j] = j % 4; }
    ray_t* RT = t29_plain("k4", tv, "rv", rvv, RT_N);
    ray_t* RT2 = ray_table_new(3);
    {
        ray_t* a = ray_vec_from_raw(RAY_I64, rk0, RT_N); ray_t* b = ray_vec_from_raw(RAY_I64, tv, RT_N);
        ray_t* c = ray_vec_from_raw(RAY_I64, rvv, RT_N);
        RT2 = ray_table_add_col(RT2, ray_sym_intern("k0", 2), a);
        RT2 = ray_table_add_col(RT2, ray_sym_intern("k4", 2), b);
        RT2 = ray_table_add_col(RT2, ray_sym_intern("rv", 2), c);
        ray_release(a); ray_release(b); ray_release(c);
    }
    ray_t* RX = ray_table_new(2);
    {
        ray_t* x = ray_sym_vec_new(RAY_SYM_W64, 2); x->len = 2;
        ((int64_t*)ray_data(x))[0] = ray_sym_intern("xa", 2); ((int64_t*)ray_data(x))[1] = ray_sym_intern("xb", 2);
        ray_t* w = ray_vec_from_raw(RAY_I64, wv, 2);
        RX = ray_table_add_col(RX, ray_sym_intern("x", 1), x);
        RX = ray_table_add_col(RX, ray_sym_intern("wx", 2), w);
        ray_release(x); ray_release(w);
    }
    /* a select result of each: the encoded slots of generation 2 pass through */
    ray_t* sel[2];
    for (int gi = 0; gi < 2; gi++) {
        ray_t* t = gi ? t2 : t0;
        ray_graph_t* g = ray_graph_new(t);
        ray_op_t* cs[4] = { ray_scan(g, "k0"), ray_scan(g, "x"), ray_scan(g, "k4"), ray_scan(g, "a") };
        sel[gi] = ray_execute(g, ray_select_op(g, ray_const_table(g, t), cs, 4));
        ray_graph_free(g);
        TEST_ASSERT_TRUE(sel[gi] && !RAY_IS_ERR(sel[gi]) && sel[gi]->type == RAY_TABLE);
    }
    TEST_ASSERT_TRUE(ray_table_get_col_enc(sel[1], ray_sym_intern("a", 1)) != NULL);
    static const struct { int src; int kind; bool t_left; const char* lk; const char* rk; uint8_t jt; const char* eq; int rhs; const char* what; } cs[] = {
        { 0, 0, true,  "k0", "k0", 0, NULL, 0, "inner k0" },
        { 0, 0, true,  "k0", "k0", 1, NULL, 0, "left k0" },
        { 0, 1, true,  "k0", "k0", 0, NULL, 0, "anti k0" },
        { 0, 0, false, "k0", "k0", 0, NULL, 0, "inner, table on the right" },
        { 0, 0, true,  "x",  "x",  0, NULL, 1, "inner nullable SYM key" },
        { 0, 0, true,  "x",  "x",  1, NULL, 1, "left nullable SYM key" },
        { 0, 1, true,  "x",  "x",  0, NULL, 1, "anti nullable SYM key" },
        { 0, 2, true,  "k4", "k4", 0, NULL, 2, "window join" },
        { 0, 2, true,  "k4", "k4", 1, NULL, 2, "window join, left" },
        { 0, 2, true,  "k4", "k4", 0, "k0", 3, "window join with an equality key" },
        { 1, 0, true,  "k0", "k0", 0, NULL, 0, "select result, inner k0" },
        { 1, 0, true,  "x",  "x",  1, NULL, 1, "select result, left nullable SYM key" },
        { 1, 1, true,  "k0", "k0", 0, NULL, 0, "select result, anti k0" },
        { 1, 2, true,  "k4", "k4", 0, NULL, 2, "select result, window join" },
    };
    test_result_t res = { TEST_PASS, NULL };
    ray_t* rhs[4] = { RK, RX, RT, RT2 };
    for (size_t i = 0; i < sizeof cs / sizeof *cs && res.status == TEST_PASS; i++) {
        ray_t* a = cs[i].src ? sel[0] : t0;
        ray_t* b = cs[i].src ? sel[1] : t2;
        ray_t* r[2]; uint64_t nf[2];
        for (int gi = 0; gi < 2; gi++) {
            ray_t* t = gi ? b : a;
            uint64_t before = ray_join_nullfree_keys;
            r[gi] = cs[i].t_left ? t29_join(t, rhs[cs[i].rhs], cs[i].kind, cs[i].lk, cs[i].rk, cs[i].jt, cs[i].eq)
                                 : t29_join(rhs[cs[i].rhs], t, cs[i].kind, cs[i].lk, cs[i].rk, cs[i].jt, cs[i].eq);
            nf[gi] = ray_join_nullfree_keys - before;
        }
        if (!(r[0] && r[1] && !RAY_IS_ERR(r[0]) && !RAY_IS_ERR(r[1]))) {
            res = (test_result_t){ TEST_FAIL, "join did not run" };
            fprintf(stderr, "  %s: join did not run (%s / %s)\n", cs[i].what,
                    r[0] && RAY_IS_ERR(r[0]) ? ray_err_code(r[0]) : "ok", r[1] && RAY_IS_ERR(r[1]) ? ray_err_code(r[1]) : "ok");
        } else if (nf[0] != nf[1]) {
            res = (test_result_t){ TEST_FAIL, "null-free proof differs" };
            fprintf(stderr, "  %s: null-free proofs %llu vs %llu\n", cs[i].what, (unsigned long long)nf[0], (unsigned long long)nf[1]);
        } else if (!t29_same(r[0], r[1])) {
            res = (test_result_t){ TEST_FAIL, "join answers differ" };
            fprintf(stderr, "  %s: answers differ\n", cs[i].what);
        }
        for (int gi = 0; gi < 2; gi++) if (r[gi]) { if (RAY_IS_ERR(r[gi])) ray_error_free(r[gi]); else ray_release(r[gi]); }
    }
    ray_release(sel[0]); ray_release(sel[1]);
    ray_release(RK); ray_release(RT); ray_release(RT2); ray_release(RX);
    ray_release(t0); ray_release(t2);
    return res;
}

/* A table of 2000 rows (a unique id, a key, a nullable value, a float and a
 * symbol) saved at generation 0 and 2 under tb0 / tb2. */
static bool t29_tb_save(void) {
    enum { N = 2000 };
    ray_t* t = ray_table_new(5);
    ray_t* u = ray_vec_new(RAY_I64, N); u->len = N;
    ray_t* k = ray_vec_new(RAY_I64, N); k->len = N;
    ray_t* v = ray_vec_new(RAY_I64, N); v->len = N;
    ray_t* f = ray_vec_new(RAY_F64, N); f->len = N;
    ray_t* s = ray_sym_vec_new(RAY_SYM_W64, N); s->len = N;
    static const char* const nm[4] = { "pa", "pb", "pc", "pd" };
    for (int64_t i = 0; i < N; i++) {
        ((int64_t*)ray_data(u))[i] = i;
        ((int64_t*)ray_data(k))[i] = i % 10;
        ((int64_t*)ray_data(v))[i] = i * 3;
        ((double*)ray_data(f))[i] = (double)(i % 7) * 0.5;
        ((int64_t*)ray_data(s))[i] = ray_sym_intern(nm[i % 4], 2);
    }
    for (int64_t i = 0; i < N; i += 9) ray_vec_set_null(v, i, true);
    t = ray_table_add_col(t, ray_sym_intern("u", 1), u);
    t = ray_table_add_col(t, ray_sym_intern("k", 1), k);
    t = ray_table_add_col(t, ray_sym_intern("v", 1), v);
    t = ray_table_add_col(t, ray_sym_intern("f", 1), f);
    t = ray_table_add_col(t, ray_sym_intern("s", 1), s);
    ray_release(u); ray_release(k); ray_release(v); ray_release(f); ray_release(s);
    bool ok = ray_splay_save_mode(t, ray_test_tmp(CS_TMP "/tb0"), ray_test_tmp(CS_TMP "/tb0.sym"), RAY_COMPRESS_OFF) == RAY_OK &&
              ray_splay_save_mode(t, ray_test_tmp(CS_TMP "/tb2"), ray_test_tmp(CS_TMP "/tb2.sym"), RAY_COMPRESS_ON) == RAY_OK;
    ray_release(t);
    return ok;
}

/* pivot and the table builtins give the generation-0 answer on a generation-2
 * table and on tables that carry its encoded slots (xcol, xcols, modify). */
static test_result_t test_tblop_sites_gen2_equal(void) {
    TEST_ASSERT_TRUE(t29_tb_save());
    ray_t* t0 = ray_read_splayed(ray_test_tmp(CS_TMP "/tb0"), ray_test_tmp(CS_TMP "/tb0.sym"));
    ray_t* t2 = ray_read_splayed(ray_test_tmp(CS_TMP "/tb2"), ray_test_tmp(CS_TMP "/tb2.sym"));
    TEST_ASSERT_TRUE(t0 && t2 && !RAY_IS_ERR(t0) && !RAY_IS_ERR(t2));
    TEST_ASSERT_TRUE(ray_table_get_col_enc(t2, ray_sym_intern("u", 1)) != NULL);
    TEST_ASSERT_TRUE(ray_table_get_col_enc(t0, ray_sym_intern("u", 1)) == NULL);
    static const char* const verbs[] = {
        "(pivot T 'k 's 'u sum)", "(pivot T 'k 'f 'u sum)", "(pivot T ['k] 's 'v avg)",
        "(pivot T 'k 's 'u (fn [x] (sum x)))",
        "(xcol T ['A 'B 'C 'D 'E])", "(xcols T ['v 'k])", "(xkey T ['u])", "(xgroup T ['k])", "(fkeys T)",
        "(modify T 'v (fn [c] (+ c 1)))", "(modify T 'u (fn [c] c))", "(union-all T T)",
        "(distinct (xcols T ['k 's]))",
    };
    int64_t sT = ray_sym_intern("T", 1);
    for (size_t i = 0; i < sizeof verbs / sizeof *verbs; i++) {
        ray_t* r[2];
        for (int gi = 0; gi < 2; gi++) {
            ray_env_set(sT, gi ? t2 : t0);
            ray_error_clear();
            r[gi] = ray_eval_str(verbs[i]);
        }
        bool ok = r[0] && r[1] && !RAY_IS_ERR(r[0]) && !RAY_IS_ERR(r[1]) && t29_same(r[0], r[1]);
        if (!ok) fprintf(stderr, "  %s: %s / %s\n", verbs[i], r[0] && RAY_IS_ERR(r[0]) ? ray_err_code(r[0]) : "ok",
                         r[1] && RAY_IS_ERR(r[1]) ? ray_err_code(r[1]) : "ok");
        for (int gi = 0; gi < 2; gi++) if (r[gi]) { if (RAY_IS_ERR(r[gi])) ray_error_free(r[gi]); else ray_release(r[gi]); }
        TEST_ASSERT_FMT(ok, "%s: answers differ", verbs[i]);
    }
    /* the slot-passing builtins keep the encoded object (no decode) */
    ray_env_set(sT, t2);
    ray_t* xc = ray_eval_str("(xcols T ['u])");
    TEST_ASSERT_TRUE(xc && !RAY_IS_ERR(xc) && ray_table_get_col_enc(xc, ray_sym_intern("u", 1)) != NULL);
    ray_release(xc);
    ray_env_set(sT, NULL);
    ray_release(t0); ray_release(t2);
    PASS();
}

/* Builtins over a table whose column a fails to materialize report it as
 * itself; the ones that never read the column still work. */
static test_result_t test_tblop_sites_failed_decode(void) {
    ray_t* t = bind_corrupt_table();
    TEST_ASSERT_TRUE(t != NULL);
    static const char* const bad[] = {
        "(pivot t 'b 'a 'b sum)", "(pivot t 'a 'b 'b sum)", "(pivot t 'b 'b 'a sum)", "(pivot t ['b] 'b 'a (fn [x] (sum x)))",
        "(xkey t ['a])", "(xkey t ['b])", "(xgroup t ['a])", "(xgroup t ['b])",
        "(modify t 'a (fn [c] c))", "(union-all t t)",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        ray_error_clear();
        ray_t* r = ray_eval_str(bad[i]);
        test_result_t rr = expect_corrupt(r, bad[i]);
        if (rr.status != TEST_PASS) return rr;
    }
    static const char* const good[] = {
        "(xcol t ['x 'y])", "(xcols t ['a 'b])", "(fkeys t)", "(modify t 'b (fn [c] c))",
    };
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
        ray_error_clear();
        ray_t* r = ray_eval_str(good[i]);
        TEST_ASSERT_FMT(r && !RAY_IS_ERR(r), "%s: must not decode column a", good[i]);
        ray_release(r);
    }
    ray_release(t);
    PASS();
}

/* Datalog: a datoms table whose column e fails to materialize (env D). */
static test_result_t test_datalog_sites_failed_decode(void) {
    static const char* const nm[3] = { "e", "a", "v" };
    static const int64_t mul[3] = { 1, 2, 3 };
    ray_t* d = t29_disk(ray_test_tmp(CS_TMP "/dl"), nm, mul, 3, 3000, 2, 0);
    TEST_ASSERT_TRUE(d && !RAY_IS_ERR(d));
    ray_env_set(ray_sym_intern("D", 1), d);
    static const char* const verbs[] = {
        "(scan-eav D 'x)", "(scan-eav D 1 'x)", "(retract-fact D 1 'x 2)", "(assert-fact D 1 'x 2)", "(pull D 1)",
        "(pull D 1 ['x])",
    };
    for (size_t i = 0; i < sizeof verbs / sizeof *verbs; i++) {
        ray_error_clear();
        ray_t* r = ray_eval_str(verbs[i]);
        test_result_t rr = t29_corrupt_at(r, verbs[i], ray_test_tmp(CS_TMP "/dl/e"));
        if (rr.status != TEST_PASS) return rr;
    }
    /* the rule engine: an EDB with a failing column fails the evaluation, no leak */
    dl_program_t* prog = dl_program_new();
    TEST_ASSERT_TRUE(prog != NULL);
    TEST_ASSERT_TRUE(dl_add_edb(prog, "edge", d, 3) >= 0);
    dl_rule_t rule;
    dl_rule_init(&rule, "p", 3);
    dl_rule_head_var(&rule, 0, 0); dl_rule_head_var(&rule, 1, 1); dl_rule_head_var(&rule, 2, 2);
    int body = dl_rule_add_atom(&rule, "edge", 3);
    dl_body_set_var(&rule, body, 0, 0); dl_body_set_var(&rule, body, 1, 1); dl_body_set_var(&rule, body, 2, 2);
    rule.n_vars = 3;
    TEST_ASSERT_EQ_I(dl_add_rule(prog, &rule), 0);
    ray_error_clear();
    TEST_ASSERT_TRUE(dl_eval(prog) != 0);
    TEST_ASSERT_TRUE(prog->first_err != NULL && RAY_IS_ERR(prog->first_err) &&
                     strcmp(ray_err_code(prog->first_err), "corrupt") == 0);
    dl_program_free(prog);                       /* releases the kept error */
    {   /* a query over the datoms table returns the column's own error */
        ray_error_clear();
        ray_t* q = ray_eval_str("(query D (find ?x) (where (eav ?x ?p ?v)))");
        test_result_t qr = t29_corrupt_at(q, "query", ray_test_tmp(CS_TMP "/dl/e"));
        if (qr.status != TEST_PASS) return qr;
    }
    ray_env_set(ray_sym_intern("D", 1), NULL);
    ray_release(d);
    PASS();
}

/* A recursive program with provenance over an EDB loaded from generation 0
 * and 2 derives the same relation and the same proofs; so does a query over
 * env-bound tables. */
static ray_t* t29_dl_run(ray_t* edge, ray_t** prov_off, ray_t** prov_data) {
    dl_program_t* prog = dl_program_new();
    if (!prog) return NULL;
    prog->flags |= DL_FLAG_PROVENANCE;
    dl_add_edb(prog, "edge", edge, 2);
    dl_rule_t r0; dl_rule_init(&r0, "path", 2);
    dl_rule_head_var(&r0, 0, 0); dl_rule_head_var(&r0, 1, 1);
    int b = dl_rule_add_atom(&r0, "edge", 2);
    dl_body_set_var(&r0, b, 0, 0); dl_body_set_var(&r0, b, 1, 1);
    r0.n_vars = 2;
    dl_add_rule(prog, &r0);
    dl_rule_t r1; dl_rule_init(&r1, "path", 2);
    dl_rule_head_var(&r1, 0, 0); dl_rule_head_var(&r1, 1, 2);
    int b0 = dl_rule_add_atom(&r1, "path", 2);
    dl_body_set_var(&r1, b0, 0, 0); dl_body_set_var(&r1, b0, 1, 1);
    int b1 = dl_rule_add_atom(&r1, "edge", 2);
    dl_body_set_var(&r1, b1, 0, 1); dl_body_set_var(&r1, b1, 1, 2);
    r1.n_vars = 3;
    dl_add_rule(prog, &r1);
    ray_t* out = NULL;
    if (dl_eval(prog) == 0) {
        out = dl_query(prog, "path");
        if (out) ray_retain(out);
        ray_t* o = dl_get_provenance_src_offsets(prog, "path");
        ray_t* d = dl_get_provenance_src_data(prog, "path");
        *prov_off = o ? (ray_retain(o), o) : NULL;
        *prov_data = d ? (ray_retain(d), d) : NULL;
    }
    dl_program_free(prog);
    return out;
}

static test_result_t test_datalog_sites_gen2_equal(void) {
    static const char* const nm[2] = { "s", "d" };
    static const int64_t mul[2] = { 1, 1 };
    ray_t* e[2]; ray_t* out[2]; ray_t* po[2] = { NULL, NULL }; ray_t* pd[2] = { NULL, NULL };
    for (int gi = 0; gi < 2; gi++) {
        e[gi] = t29_disk(gi ? ray_test_tmp(CS_TMP "/e2") : ray_test_tmp(CS_TMP "/e0"), nm, mul, 2, 30, gi ? 2 : 0, -1);
        TEST_ASSERT_TRUE(e[gi] && !RAY_IS_ERR(e[gi]));
        /* d = s + 1: shift the destination column through a derived table */
        ray_t* s = ray_table_col(e[gi], ray_sym_intern("s", 1));
        ray_t* dd = ray_vec_new(RAY_I64, 30); dd->len = 30;
        for (int64_t i = 0; i < 30; i++) ((int64_t*)ray_data(dd))[i] = ((int64_t*)ray_data(s))[i] + 1;
        ray_t* te = ray_table_new(2);
        ray_t* sc = gi ? ray_table_get_col_enc(e[gi], ray_sym_intern("s", 1)) : s;
        te = ray_table_add_col(te, ray_sym_intern("s", 1), sc);
        te = ray_table_add_col(te, ray_sym_intern("d", 1), dd);
        ray_release(dd);
        ray_release(e[gi]);
        e[gi] = te;
        out[gi] = t29_dl_run(e[gi], &po[gi], &pd[gi]);
    }
    TEST_ASSERT_TRUE(ray_table_get_col_enc(e[1], ray_sym_intern("s", 1)) != NULL);
    TEST_ASSERT_TRUE(out[0] && out[1] && ray_table_nrows(out[0]) == 30 * 31 / 2);
    TEST_ASSERT_TRUE(t29_same(out[0], out[1]));
    TEST_ASSERT_TRUE(po[0] && po[1] && t29_same(po[0], po[1]));
    TEST_ASSERT_TRUE(pd[0] && pd[1] && t29_same(pd[0], pd[1]));
    for (int gi = 0; gi < 2; gi++) { ray_release(out[gi]); ray_release(po[gi]); ray_release(pd[gi]); ray_release(e[gi]); }
    PASS();
}

static test_result_t test_datalog_query_gen2_equal(void) {
    static const char* const nm3[3] = { "e", "a", "v" };
    static const int64_t mul3[3] = { 1, 1, 10 };
    static const char* const nm2[2] = { "p", "q" };
    static const int64_t mul2[2] = { 1, 100 };
    ray_t* r[2];
    for (int gi = 0; gi < 2; gi++) {
        ray_t* db = t29_disk(gi ? ray_test_tmp(CS_TMP "/q2") : ray_test_tmp(CS_TMP "/q0"), nm3, mul3, 3, 50, gi ? 2 : 0, -1);
        ray_t* ex = t29_disk(gi ? ray_test_tmp(CS_TMP "/x2") : ray_test_tmp(CS_TMP "/x0"), nm2, mul2, 2, 50, gi ? 2 : 0, -1);
        TEST_ASSERT_TRUE(db && ex && !RAY_IS_ERR(db) && !RAY_IS_ERR(ex));
        if (gi) TEST_ASSERT_TRUE(ray_table_get_col_enc(ex, ray_sym_intern("p", 1)) != NULL);
        ray_env_set(ray_sym_intern("qdb", 3), db);
        ray_env_set(ray_sym_intern("extra", 5), ex);
        ray_error_clear();
        r[gi] = ray_eval_str("(query qdb (find ?x ?b) (where (eav ?x ?p ?v) (extra ?x ?b)))");
        ray_env_set(ray_sym_intern("qdb", 3), NULL);
        ray_env_set(ray_sym_intern("extra", 5), NULL);
        ray_release(db); ray_release(ex);
    }
    TEST_ASSERT_TRUE(r[0] && r[1] && !RAY_IS_ERR(r[0]) && !RAY_IS_ERR(r[1]));
    TEST_ASSERT_EQ_I((int)ray_table_nrows(r[0]), 50);
    TEST_ASSERT_TRUE(t29_same(r[0], r[1]));
    ray_release(r[0]); ray_release(r[1]);
    PASS();
}

/* Update, delete, upsert refusal and the live-tail insert over a parted table
 * whose segments are partly or wholly encoded give the generation-0 answer:
 * the verbs read the encoded wrappers through the accessors. */
static test_result_t test_dml_parted_gen2(void) {
    static const unsigned masks[] = { 0, 7, 5 };
    static const char* const qs[] = {
        "(sum (at (update {a: (* a 2) from: pt where: (> b 6)}) 'a))",
        "(sum (at (update {a: (+ a b) from: pt}) 'a))",
        "(count (delete {from: pt where: (== b 6)}))",
        "(sum (at (delete {from: pt where: (== b 6)}) 'a))",
        "(count (insert pt 2024.01.03 (list 7 8)))",
        "(sum (at (insert pt 2024.01.04 (list 7 8)) 'b))",
    };
    int64_t want[sizeof qs / sizeof *qs];
    for (int m = 0; m < 3; m++) {
        char dir[64]; snprintf(dir, sizeof dir, ray_test_tmp(CS_TMP "/dp%d"), m);
        ray_t* t = pdb_make(dir, masks[m], false);
        TEST_ASSERT_NOT_NULL(t);
        if (m) TEST_ASSERT_TRUE(ray_table_get_col_enc(t, ray_sym_intern("a", 1)) != NULL);
        ray_env_set(ray_sym_intern("pt", 2), t);
        ray_release(t);
        int64_t sum0 = ev_i64("(sum (at pt 'a))"), sumb0 = ev_i64("(sum (at pt 'b))");
        TEST_ASSERT_TRUE(sum0 != INT64_MIN && sumb0 != INT64_MIN);
        for (size_t i = 0; i < sizeof qs / sizeof *qs; i++) {
            int64_t v = ev_i64(qs[i]);
            TEST_ASSERT_FMT(v != INT64_MIN, "%s (mask %u): %s", qs[i], masks[m], ev_code(qs[i]));
            if (m == 0) want[i] = v;
            else TEST_ASSERT_FMT(v == want[i], "%s (mask %u): %lld vs %lld", qs[i], masks[m],
                                 (long long)v, (long long)want[i]);
        }
        TEST_ASSERT_STR_EQ(ev_code("(upsert pt 'a (list 1 2))"), "nyi");
        /* the by-value verbs left the bound table as it was */
        TEST_ASSERT_EQ_I(ev_i64("(count pt)"), 9000);
        TEST_ASSERT_EQ_I(ev_i64("(sum (at pt 'a))"), sum0);
        TEST_ASSERT_EQ_I(ev_i64("(sum (at pt 'b))"), sumb0);
        ray_env_set(ray_sym_intern("pt", 2), NULL);
    }
    PASS();
}

/* The live-tail insert still refuses a parted target whose column-list
 * container is not canonical (a LIST with zero attrs, as long as the schema),
 * and accepts the canonical one. */
static test_result_t test_parted_insert_container(void) {
    ray_t* t = pdb_make(ray_test_tmp(CS_TMP "/pc"), 7, false);
    TEST_ASSERT_NOT_NULL(t);
    ray_env_set(ray_sym_intern("pt", 2), t);
    ray_t* cols = ((ray_t**)ray_data(t))[1];       /* test-only container edits, restored */
    const char* q = "(insert pt 2024.01.03 (list 7 8))";
    uint8_t a0 = cols->attrs;
    cols->attrs = 1;
    ray_error_clear();
    TEST_ASSERT_STR_EQ(ev_code(q), "corrupt");
    cols->attrs = a0;
    int8_t t0 = cols->type;
    cols->type = RAY_I64;
    TEST_ASSERT_STR_EQ(ev_code(q), "corrupt");
    cols->type = t0;
    int64_t n0 = cols->len;
    cols->len = n0 + 1;
    TEST_ASSERT_STR_EQ(ev_code(q), "corrupt");
    cols->len = n0;
    TEST_ASSERT_EQ_I(ev_i64("(count (insert pt 2024.01.03 (list 7 8)))"), 9001);
    ray_env_set(ray_sym_intern("pt", 2), NULL);
    ray_release(t);
    PASS();
}

/* A join over a parted table whose segments are partly or wholly encoded
 * (join_parted_seg_count, join_table_has_parted_col, the per-segment and
 * flatten routes) gives the generation-0 answer, on either side. */
static test_result_t test_join_sites_parted(void) {
    ray_t* t0 = pdb_make(ray_test_tmp(CS_TMP "/jp0"), 0, false);
    TEST_ASSERT_NOT_NULL(t0);
    static const unsigned masks[] = { 7, 5 };
    int64_t pv[3000], cv[3000];
    for (int64_t i = 0; i < 3000; i++) { pv[i] = 5 * i + (i % 3); cv[i] = i; }
    ray_t* P = t29_plain("b", pv, "c", cv, 3000);
    for (int m = 0; m < 2; m++) {
        char dir[64]; snprintf(dir, sizeof dir, ray_test_tmp(CS_TMP "/jp%d"), m + 1);
        ray_t* tm = pdb_make(dir, masks[m], false);
        TEST_ASSERT_NOT_NULL(tm);
        for (int c = 0; c < 6; c++) {
            int kind = c % 3 == 2 ? 1 : 0;
            uint8_t jt = c % 3 == 1 ? 1 : 0;
            bool parted_left = c < 3;
            ray_t* r0 = parted_left ? t29_join(t0, P, kind, "b", "b", jt, NULL) : t29_join(P, t0, kind, "b", "b", jt, NULL);
            ray_t* rm = parted_left ? t29_join(tm, P, kind, "b", "b", jt, NULL) : t29_join(P, tm, kind, "b", "b", jt, NULL);
            bool ok = r0 && rm && !RAY_IS_ERR(r0) && !RAY_IS_ERR(rm) && t29_same(r0, rm);
            if (r0) { if (RAY_IS_ERR(r0)) ray_error_free(r0); else ray_release(r0); }
            if (rm) { if (RAY_IS_ERR(rm)) ray_error_free(rm); else ray_release(rm); }
            TEST_ASSERT_FMT(ok, "mask %u case %d: answers differ", masks[m], c);
        }
        ray_release(tm);
    }
    ray_release(P); ray_release(t0);
    PASS();
}

/* ---- Task 30: query.c helpers before the select implementation ---- */

/* A scalar aggregate over a column large enough to record CHUNK_ZONE:
 * planning must not materialize it to read the synthesized zone. */
static test_result_t test_scalar_agg_planning_zoned(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['v] (list (til 70000))))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/za\" t {compress: true})")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u (.db.splayed.get \"" CS_TMP "/za\"))")), "");
    ray_col_meta_t m;
    ray_t* u = ray_eval_str("u");
    TEST_ASSERT_TRUE(ray_table_col_meta(u, ray_sym_intern("v", 1), &m) && m.index_kind == RAY_IDX_CHUNK_ZONE);
    ray_release(u);
    static const char* const qs[] = {
        "(select {s: (sum v) from: u})", "(select {m: (max v) n: (min v) from: u})", "(select {c: (count v) a: (avg v) from: u})", NULL,
    };
    for (int i = 0; qs[i]; i++) {
        ray_enc_counters_reset();
        ray_t* r = ray_eval_str(qs[i]);
        TEST_ASSERT_FMT(r && !RAY_IS_ERR(r), "query %d", i);
        ray_release(r);
        ray_enc_counters_t c; ray_enc_counters_get(&c);
        /* The query reaches execution (answered from the zone during
         * planning would leave UINT64_MAX) and nothing decodes before it. */
        TEST_ASSERT_FMT(c.at_first_exec == 0, "query %d: %llu materialized before execution", i,
                        (unsigned long long)c.at_first_exec);
    }
    ray_t* r = ray_eval_str("(sum (at u 'v))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == (int64_t)69999 * 70000 / 2);
    ray_release(r);
    PASS();
}

/* DAG construction names columns by metadata: over a generation-2 table whose
 * column cannot decode, compiling every name form succeeds without decoding,
 * and the failure surfaces when the DAG runs, as the column's own error. */
static test_result_t test_dag_presence_without_decode(void) {
    ray_t* t = ex_table(3000, true);
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    static const char* const forms[] = { "a", "(+ a 1)", "(sum a)", "'a", "(neg a)", NULL };
    for (int i = 0; forms[i]; i++) {
        ray_graph_t* g = ray_graph_new(t);
        ray_t* e = ray_parse(forms[i]);
        ray_enc_counters_reset();
        ray_op_t* dag = compile_expr_dag(g, e);
        ray_enc_counters_t c; ray_enc_counters_get(&c);
        TEST_ASSERT_FMT(dag != NULL, "form %s: compiled", forms[i]);
        TEST_ASSERT_FMT(c.materialized == 0, "form %s: construction decoded", forms[i]);
        if (dag) {
            ray_error_clear();
            ray_t* r = ray_execute(g, ray_optimize(g, dag));
            if (r && ray_is_lazy(r)) r = ray_lazy_materialize(r);
            test_result_t rr = ex_corrupt(r, forms[i]);
            if (rr.status != TEST_PASS) { ray_release(e); ray_graph_free(g); ray_release(t); return rr; }
        }
        ray_release(e);
        ray_graph_free(g);
    }
    /* an absent name is still absent: a column of another table is not found */
    ray_graph_t* g = ray_graph_new(t);
    ray_t* e = ray_parse("nosuchcol");
    ray_op_t* dag = compile_expr_dag(g, e);
    ray_t* r = dag ? ray_execute(g, ray_optimize(g, dag)) : NULL;
    TEST_ASSERT_TRUE(!dag || (r && RAY_IS_ERR(r)));
    if (r && RAY_IS_ERR(r)) ray_error_free(r);
    ray_release(e);
    ray_graph_free(g);
    ray_release(t);
    PASS();
}

/* Queries over the helpers of this part, on a table read from generation-0
 * and generation-2 files, give the same answer. */
static test_result_t test_query_helpers_gen2_equal(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['k 'm 'v 's] (list (% (til 80000) 7) (% (til 80000) 5) (% (* (til 80000) 13) 1000) (take ['aa 'bb 'cc 'dd] 80000))))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/q0\" t {compress: false})")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/q2\" t {compress: true})")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u0 (.db.splayed.get \"" CS_TMP "/q0\"))")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u2 (.db.splayed.get \"" CS_TMP "/q2\"))")), "");
    ray_t* u2 = ray_eval_str("u2");
    TEST_ASSERT_TRUE(u2 && ray_table_get_col_enc(u2, ray_sym_intern("v", 1)) != NULL);
    ray_release(u2);
    static const char* const qs[] = {
        "(select {c: (count (distinct v)) by: k from: %s})",
        "(select {c: (count (distinct v)) by: [k m] from: %s})",
        "(select {c: (count (distinct v)) by: k from: %s where: (> v 100)})",
        "(select {s: (sum v) a: (avg v) by: k from: %s desc: s take: 3})",
        "(select {s: (sum v) by: [k m] from: %s asc: s take: 4})",
        "(select {c: (count v) from: %s where: (== k 3)})",
        "(select {c: (count v) from: %s where: (> v 500)})",
        "(select {c: (count v) by: s from: %s})",
        "(select {c: (count v) by: (+ k 1) from: %s})",
        "(select {x: (+ (sum v) (count v)) by: k from: %s})",
        "(select {x: (med v) y: (max v) by: k from: %s})",
        "(select {k: k w: (+ v 1) from: %s take: 5})",
        "(select {w: (* v 2) from: %s where: (< k 2) take: 6})",
        "(select {s: (sum v) m: (min v) from: %s})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        char q0[256], q2[256];
        snprintf(q0, sizeof q0, qs[i], "u0");
        snprintf(q2, sizeof q2, qs[i], "u2");
        ray_t* a = ray_eval_str(q0);
        ray_t* b = ray_eval_str(q2);
        bool ok = a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b) && cp_fmt_same(a, b);
        if (!ok) fprintf(stderr, "query_helpers_gen2_equal: %s\n", q2);
        if (a && RAY_IS_ERR(a)) ray_error_free(a); else if (a) ray_release(a);
        if (b && RAY_IS_ERR(b)) ray_error_free(b); else if (b) ray_release(b);
        TEST_ASSERT_FMT(ok, "query %d", i);
    }
    PASS();
}

/* Damage the descriptor of a generation-2 file's first vector after the load
 * checks pass: the column loads and fails to materialize.  The byte is a
 * reserved descriptor field, so writing 0 back restores the file. */
static bool qb_set_reserved(const char* path, int64_t n, int byte) {
    FILE* f = fopen(path, "r+b");
    if (!f) return false;
    int64_t n_chunks = (n + 65535) / 65536;
    bool ok = fseek(f, (long)(96 + 64 * n_chunks + 6), SEEK_SET) == 0 && fputc(byte, f) != EOF;
    return fclose(f) == 0 && ok;
}
static bool qb_damage(const char* path, int64_t n) { return qb_set_reserved(path, n, 1); }

/* `n` rows of b (5 i) and k (i mod 7), generation 0, then, generation 2:
 * a1 .. a9 (3 i), kb (BOOL, i mod 3 == 0), ks (STR, 5 values), kd (i mod 5),
 * kh (i mod 60000), kf (F64, (i mod 9) / 2).  `damage` NULL damages a1 .. a9
 * (each loads intact and fails to materialize), "" none, else only the named column:
 * a query's error names the one column the failing site read.  NULL when a
 * file cannot be written or damaged. */
static ray_t* qb_table(int64_t n, const char* damage) {
    char p[256];
    (void)ray_test_rm_rf(ray_test_tmp(CS_TMP "/qb"));
    (void)ray_test_mkdir_p(ray_test_tmp(CS_TMP "/qb"));
    ray_t* names = ray_vec_new(RAY_STR, 16);
    ray_t* b = i64_col(n, 5);
    ray_t* k = ray_vec_new(RAY_I64, n); k->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(k))[i] = i % 7;
    snprintf(p, sizeof p, "%s/qb/b", ray_test_tmp(CS_TMP)); ray_col_save_gen(b, p, 0);
    snprintf(p, sizeof p, "%s/qb/k", ray_test_tmp(CS_TMP)); ray_col_save_gen(k, p, 0);
    ray_release(b); ray_release(k);
    names = ray_str_vec_append(names, "b", 1); names = ray_str_vec_append(names, "k", 1);
    static const char* const enc[] = { "a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9",
                                       "kb", "ks", "kd", "kh", "kf", NULL };
    bool ok = true;
    for (int c = 0; enc[c] && ok; c++) {
        const char* nm = enc[c];
        ray_t* v;
        if (nm[0] == 'a') v = i64_col(n, 3);
        else if (nm[1] == 'b') {
            v = ray_vec_new(RAY_BOOL, n); v->len = n;
            for (int64_t i = 0; i < n; i++) ((uint8_t*)ray_data(v))[i] = i % 3 == 0;
        } else if (nm[1] == 's') {
            v = ray_vec_new(RAY_STR, n);
            static const char* const sv[] = { "red", "green", "blue", "cyan", "magenta" };
            for (int64_t i = 0; i < n; i++) v = ray_str_vec_append(v, sv[i % 5], strlen(sv[i % 5]));
        } else if (nm[1] == 'f') {
            v = ray_vec_new(RAY_F64, n); v->len = n;
            for (int64_t i = 0; i < n; i++) ((double*)ray_data(v))[i] = (double)(i % 9) / 2;
        } else {
            int64_t mod = nm[1] == 'd' ? 5 : 60000;
            v = ray_vec_new(RAY_I64, n); v->len = n;
            for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = i % mod;
        }
        snprintf(p, sizeof p, "%s/qb/%s", ray_test_tmp(CS_TMP), nm);
        ok = ray_col_save_gen(v, p, 2) == RAY_OK;
        ray_release(v);
        bool hit = damage ? strcmp(damage, nm) == 0 : nm[0] == 'a';
        if (ok && hit) ok = qb_damage(p, n);
        names = ray_str_vec_append(names, nm, strlen(nm));
    }
    snprintf(p, sizeof p, "%s/qb/.d", ray_test_tmp(CS_TMP)); ray_col_save(names, p); ray_release(names);
    return ok ? ray_read_splayed(ray_test_tmp(CS_TMP "/qb"), NULL) : NULL;
}

/* A select whose source cannot materialize a column reports that column's
 * error from the helper that read it (the select entry no longer decodes the
 * source): each query references its own damaged column, so the file named
 * in the message proves which column the failing site read.  The row-wise
 * grouped expression binds every column (bind_all_columns' error path) and
 * fails on the first damaged column in table order, a1. */
static test_result_t test_query_helpers_failed_decode(void) {
    ray_t* t = qb_table(3000, NULL);
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_env_set(ray_sym_intern("bad", 3), t);
    ray_release(t);
    static const struct { const char* q; const char* col; } qs[] = {
        { "(select {c: (count (distinct a2)) by: b from: bad})",   "a2" },  /* count-distinct rewrite */
        { "(select {s: (sum a3) by: b from: bad desc: s take: 2})", "a3" },
        { "(select {c: (count a4) from: bad where: (== a4 3)})",    "a4" },
        { "(select {x: (+ a5 1) from: bad})",                       "a5" },
        { "(select {m: (max a6) from: bad})",                       "a6" },  /* metadata declines: execution reads */
        { "(select {s: (sum b) by: (+ a7 1) from: bad})",           "a7" },  /* computed-key plan */
        { "(select {x: (+ a8 b) by: k from: bad})",                 "a8" },  /* grouped row-wise output */
        { NULL, NULL },
    };
    for (int i = 0; qs[i].q; i++) {
        char path[128];
        snprintf(path, sizeof path, "%s/qb/%s", ray_test_tmp(CS_TMP), qs[i].col);
        ray_error_clear();
        ray_t* r = ray_eval_str(qs[i].q);
        test_result_t rr = t29_corrupt_at(r, qs[i].q, path);
        if (rr.status != TEST_PASS) return rr;
    }
    PASS();
}

/* ---- Task 31: ray_select_impl ---- */

/* A select over a table whose encoded columns cannot materialize reports the
 * failure from the site that read the column: each query below references
 * one damaged column, and its error names that column's file.  The select
 * entry no longer decodes the source, so a query naming a2 failing on a1
 * would mean some site read a column the query never references (the
 * row-wise expression in a grouped select included: it binds the columns it
 * names). */
static test_result_t test_select_sites_failed_decode(void) {
    static const struct { const char* q; const char* col; } qs[] = {
        { "(select {c: (count (distinct a2)) by: k from: bad})",            "a2" },
        { "(select {s: (sum a3) by: k from: bad desc: s take: 2})",          "a3" },
        { "(select {c: (count b) from: bad where: (== a4 3)})",              "a4" },
        { "(select {x: (+ a5 1) from: bad})",                                "a5" },
        { "(select {m: (max a6) from: bad})",                                "a6" },
        { "(select {s: (sum b) by: (+ a7 1) from: bad})",                    "a7" },
        { "(select {x: a8 from: bad where: (> b 10) asc: a8 take: 3})",      "a8" },
        { "(select {f: (first a9) by: k from: bad})",                        "a9" },
        { "(select {s: (sum a2) by: k from: bad where: (> b 10)})",          "a2" },
        { "(select {x: a3 y: b from: bad where: (> b 10)})",                 "a3" },
        { "(select {x: a4 from: bad asc: a4})",                              "a4" },
        { "(select {s: (sum b) by: a5 from: bad})",                          "a5" },
        { "(select {s: (sum a6) l: (last b) by: [k b] from: bad})",          "a6" },
        { "(select {x: (+ a5 b) by: k from: bad})",                          "a5" },  /* grouped row-wise output */
        { NULL, NULL },
    };
    for (int n = 0; n < 2; n++) {
        int64_t rows = n ? 200000 : 3000;
        ray_t* t = qb_table(rows, NULL);
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_env_set(ray_sym_intern("bad", 3), t);
        ray_release(t);
        for (int i = 0; qs[i].q; i++) {
            char path[128], what[200];
            snprintf(path, sizeof path, "%s/qb/%s", ray_test_tmp(CS_TMP), qs[i].col);
            snprintf(what, sizeof what, "%s (n=%lld)", qs[i].q, (long long)rows);
            ray_error_clear();
            ray_t* r = ray_eval_str(qs[i].q);
            test_result_t rr = t29_corrupt_at(r, what, path);
            if (rr.status != TEST_PASS) return rr;
        }
    }
    PASS();
}

/* The routes past the first read: only the named column is damaged, so the
 * failure is the one site of that shape reading it (no-agg by and its
 * first-row gather, by-dict renames, computed and dependent keys, BOOL and
 * STR keys, eval-level groups and their gathers, multi-key first rows, the
 * row-expression scatter, the global count-distinct hash).  A zero-group
 * select builds its empty columns from metadata and does not fail. */
static test_result_t test_select_routes_failed_decode(void) {
    static const struct { const char* q; const char* col; bool ok; } qs[] = {
        { "(select {from: bad by: k})",                                      "a3", false },
        { "(select {from: bad by: kd})",                                     "kd", false },
        { "(select {s: (sum a2) by: {k: k} from: bad})",                     "a2", false },
        { "(select {s: (sum b) by: {kk: (+ kd 1)} from: bad})",              "kd", false },
        { "(select {s: (sum b) by: {kk: kd} from: bad})",                    "kd", false },
        { "(select {c: (count b) by: {kd: kd km: (- kd 1)} from: bad})",      "kd", false },
        { "(select {s: (sum b) by: kb from: bad})",                          "kb", false },
        { "(select {s: (sum a4) by: kb from: bad})",                         "a4", false },
        { "(select {x: (+ b 1) by: ks from: bad})",                          "ks", false },
        { "(select {m: (max a5) x: (+ b 1) by: ks from: bad})",              "a5", false },
        { "(select {x: (+ a6 1) by: ks from: bad})",                         "a6", false },
        { "(select {from: bad by: ks})",                                     "a6", false },
        { "(select {from: bad by: [k kf]})",                                 "a7", false },
        { "(select {from: bad by: [k kf]})",                                 "kf", false },
        { "(select {d: (count (distinct b)) by: (+ kd 1) from: bad})",       "kd", false },
        { "(select {s: (sum b) by: (+ kd 1) from: bad where: (< b 0)})",     "kd", true  },
        { "(select {s: (sum b) x: (+ a9 1) by: k from: bad})",               "a9", false },
        { "(select {c: (count (distinct a1)) s: (sum b) by: kh from: bad})", "a1", false },
        { "(select {from: bad by: k where: (< b 0)})",                       "a8", true  },
        { "(select {from: bad by: ks where: (< b 0)})",                      "ks", true  },
        { NULL, NULL, false },
    };
    /* The table is written once per size, intact; each query damages its one
     * column in place, loads a fresh table, and restores the byte after. */
    for (int n = 0; n < 2; n++) {
        int64_t rows = n ? 200000 : 3000;
        ray_t* t0 = qb_table(rows, "");
        TEST_ASSERT_TRUE(t0 && !RAY_IS_ERR(t0));
        ray_release(t0);
        for (int i = 0; qs[i].q; i++) {
            char path[128], what[200];
            snprintf(path, sizeof path, "%s/qb/%s", ray_test_tmp(CS_TMP), qs[i].col);
            snprintf(what, sizeof what, "%s, %s damaged (n=%lld)", qs[i].q, qs[i].col, (long long)rows);
            TEST_ASSERT_TRUE(qb_damage(path, rows));
            ray_t* t = ray_read_splayed(ray_test_tmp(CS_TMP "/qb"), NULL);
            TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
            ray_env_set(ray_sym_intern("bad", 3), t);
            ray_release(t);
            ray_error_clear();
            ray_t* r = ray_eval_str(qs[i].q);
            ray_env_set(ray_sym_intern("bad", 3), NULL);
            TEST_ASSERT_TRUE(qb_set_reserved(path, rows, 0));
            if (qs[i].ok) {
                bool fine = r && !RAY_IS_ERR(r) && r->type == RAY_TABLE && ray_table_nrows(r) == 0;
                if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
                TEST_ASSERT_FMT(fine, "%s: an empty table", what);
                continue;
            }
            test_result_t rr = t29_corrupt_at(r, what, path);
            if (rr.status != TEST_PASS) return rr;
        }
    }
    PASS();
}

/* The slice-group probe reads the key's index only after metadata admits it
 * (a SYM key with a HASH or PART index).  Generation 2 records neither kind
 * in P1 (such a key is saved as generation 0), so the probe's key read never
 * meets an encoded slot; the slice route still runs over an encoded
 * aggregate column, and that column failing is the query's error. */
static test_result_t test_select_slice_probe_failed_decode(void) {
    static const char* const cols[] = { "f", NULL };
    for (int c = 0; cols[c]; c++) {
        TEST_ASSERT_STR_EQ(ev_code("(set SY ['aa 'bb 'cc 'dd 'ee 'ff 'gg 'hh])"), "");
        TEST_ASSERT_STR_EQ(ev_code("(set KS (as 'SYM (at SY (% (til 4000) 8))))"), "");
        TEST_ASSERT_STR_EQ(ev_code("(set FF (* 0.5 (as 'F64 (% (til 4000) 64))))"), "");
        TEST_ASSERT_STR_EQ(ev_code("(set TI (table [s f] (list (.attr.set 'grouped KS) FF)))"), "");
        TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/sp\" TI {compress: true})")), "");
        char p[128]; snprintf(p, sizeof p, "%s/sp/%s", ray_test_tmp(CS_TMP), cols[c]);
        TEST_ASSERT_EQ_U(file_gen(p), 2);
        TEST_ASSERT_TRUE(qb_damage(p, 4000));
        TEST_ASSERT_EQ_U(file_gen(ray_test_tmp(CS_TMP "/sp/s")), 0);   /* the HASH-indexed key */
        TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set TL (.db.splayed.get \"" CS_TMP "/sp\"))")), "");
        uint64_t hits0 = ray_idx_hits[IDX_SITE_GROUP_SLICE];
        ray_error_clear();
        ray_t* r = ray_eval_str("(select {sv: (sum f) c: (count f) from: TL by: s asc: s where: (in s ['aa 'cc])})");
        TEST_ASSERT_FMT(ray_idx_hits[IDX_SITE_GROUP_SLICE] == hits0 + 1, "the slice route ran %llu times",
                        (unsigned long long)(ray_idx_hits[IDX_SITE_GROUP_SLICE] - hits0));
        test_result_t rr = t29_corrupt_at(r, cols[c], p);
        if (rr.status != TEST_PASS) return rr;
    }
    PASS();
}

/* by-dict values and computed keys bind only the encoded columns they name:
 * over a 20-column generation-2 table each query materializes exactly the
 * columns it references, and answers as generation 0 does. */
static test_result_t test_select_bydict_binds_referenced(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['k1 'k2 'a 'c03 'c04 'c05 'c06 'c07 'c08 'c09 'c10 'c11 'c12 'c13 'c14 'c15 'c16 'c17 'c18 'c19] "
        "(list (% (til 70000) 7) (% (til 70000) 5) (% (* 3 (til 70000)) 1001) (til 70000) (til 70000) (til 70000) (til 70000) (til 70000) "
        "(til 70000) (til 70000) (til 70000) (til 70000) (til 70000) (til 70000) (til 70000) (til 70000) (til 70000) (til 70000) (til 70000) (til 70000))))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/w0\" t {compress: false})")), "");
    TEST_ASSERT_STR_EQ(ev_code("(set lam_k2 (fn [] (% k2 2)))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/w2\" t {compress: true})")), "");
    static const struct { const char* q; uint64_t cols; } qs[] = {
        { "(select {c: (count a) by: {k1: k1 k2: k2} from: %s})",              2 },
        { "(select {s: (sum a) by: {k1: k1 k2: k2} from: %s})",                3 },
        { "(select {c: (count a) by: {m: (%% a 3)} from: %s})",                1 },
        { "(select {s: (sum a) by: {k1: k1 m: (%% k2 3)} from: %s})",          3 },
        { "(select {s: (sum a) by: {g: k2} from: %s where: (> a 10)})",        2 },
        { "(select {d: (count (distinct a)) by: (%% k2 3) from: %s})",         2 },
        { "(select {c: (count a) by: {g: (lam_k2)} from: %s})",                20 },  /* a lambda may read any column */
        { NULL, 0 },
    };
    for (int i = 0; qs[i].q; i++) {
        char q0[256], q2[256];
        snprintf(q0, sizeof q0, qs[i].q, "u0");
        snprintf(q2, sizeof q2, qs[i].q, "u2");
        TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u0 (.db.splayed.get \"" CS_TMP "/w0\"))")), "");
        TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u2 (.db.splayed.get \"" CS_TMP "/w2\"))")), "");
        ray_t* a = ray_eval_str(q0);
        ray_enc_counters_reset();
        ray_t* b = ray_eval_str(q2);
        ray_enc_counters_t c; ray_enc_counters_get(&c);
        bool same = a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b) && t29_same(a, b);
        if (a && RAY_IS_ERR(a)) ray_error_free(a); else if (a) ray_release(a);
        if (b && RAY_IS_ERR(b)) ray_error_free(b); else if (b) ray_release(b);
        TEST_ASSERT_FMT(same, "%s: answers differ", q2);
        TEST_ASSERT_FMT(c.materialized == qs[i].cols, "%s: materialized %llu, references %llu",
                        q2, (unsigned long long)c.materialized, (unsigned long long)qs[i].cols);
    }
    PASS();
}

/* A select over a generation-2 table materializes exactly the columns it
 * references, never the table's width, for where, by, aggregate and take
 * shapes. */
static test_result_t test_select_decodes_referenced_only(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['k 'm 'v 'w 'x 'y] (list (% (til 80000) 7) (% (til 80000) 5) (% (* (til 80000) 13) 1000) (% (til 80000) 11) (til 80000) (* 2 (til 80000)))))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/rr\" t {compress: true})")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u2 (.db.splayed.get \"" CS_TMP "/rr\"))")), "");
    static const char* const cn[] = { "k", "m", "v", "w", "x", "y" };
    for (int c = 0; c < 6; c++) {
        ray_t* u = ray_eval_str("u2");
        bool enc = u && ray_table_get_col_enc(u, ray_sym_intern(cn[c], 1)) != NULL;
        ray_release(u);
        TEST_ASSERT_FMT(enc, "column %s is encoded", cn[c]);
    }
    static const struct { const char* q; uint64_t cols; } qs[] = {
        { "(select {c: (count v) from: u2 where: (> v 100)})",                1 },
        { "(select {s: (sum v) from: u2 where: (== k 3)})",                   1 },  /* P2: k's consult reads the directory */
        { "(select {from: u2 where: (== k 3)})",                              6 },  /* every column is output */
        { "(select {s: (sum v) by: k from: u2})",                             2 },
        { "(select {s: (sum v) c: (max w) by: [k m] from: u2})",              4 },
        { "(select {s: (sum v) by: k from: u2 where: (> w 5)})",              2 },  /* P2: w's consult reads the directory */
        { "(select {s: (sum v) by: k from: u2 desc: s take: 2})",             2 },
        { "(select {d: (med v) e: (first w) from: u2})",                      2 },
        { "(select {d: (avg v) e: (dev w) from: u2 where: (< x 1000)})",      2 },  /* P2: x's consult reads the directory */
        { "(select {k: k v: v from: u2 take: 5})",                            2 },
        { "(select {k: k v: v from: u2 where: (> w 5) take: 5})",             2 },  /* P2: the take scan range-reads w */
        { "(select {v: v from: u2 desc: v take: 3})",                         1 },
        { "(select {v: v x: x from: u2 where: (> w 5) asc: x take: 4})",      2 },  /* P2: the top-k scan range-reads w */
        { "(select {z: (+ v x) from: u2 where: (> w 5)})",                    0 },  /* P2: a fused side range-reads; w's consult reads the directory */
        { NULL, 0 },
    };
    for (int i = 0; qs[i].q; i++) {
        ray_t* u = ray_eval_str(ray_test_tmp("(set u2 (.db.splayed.get \"" CS_TMP "/rr\"))"));   /* a fresh load: no cached decode */
        TEST_ASSERT_TRUE(u && !RAY_IS_ERR(u));
        ray_release(u);
        ray_enc_counters_reset();
        ray_t* r = ray_eval_str(qs[i].q);
        ray_enc_counters_t c; ray_enc_counters_get(&c);
        bool ok = r && !RAY_IS_ERR(r);
        if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
        TEST_ASSERT_FMT(ok, "%s: answered", qs[i].q);
        TEST_ASSERT_FMT(c.materialized == qs[i].cols, "%s: materialized %llu, references %llu",
                        qs[i].q, (unsigned long long)c.materialized, (unsigned long long)qs[i].cols);
    }
    PASS();
}

/* Generation 0 and 2 give the same answers over a broad set of select shapes. */
static test_result_t test_select_gen2_equal(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['k 'm 'v 'w 's 'f 'bl 'st 'h] (list (% (til 80000) 7) (% (til 80000) 5) (% (* (til 80000) 13) 1000) (- 40000 (til 80000)) (take ['aa 'bb 'cc 'dd] 80000) (* 0.5 (% (til 80000) 333)) "
        "(as 'BOOL (== 0 (% (til 80000) 3))) (as 'STR (take ['red 'green 'blue 'cyan 'magenta] 80000)) (% (til 80000) 60000))))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/e0\" t {compress: false})")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/e2\" t {compress: true})")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u0 (.db.splayed.get \"" CS_TMP "/e0\"))")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u2 (.db.splayed.get \"" CS_TMP "/e2\"))")), "");
    static const char* const qs[] = {
        "(select {from: %s})",
        "(select {from: %s where: (== k 3)})",
        "(select {from: %s take: 7})",
        "(select {from: %s take: -7})",
        "(select {from: %s asc: v take: 9})",
        "(select {from: %s desc: [k v] take: 9})",
        "(select {v: v w: w from: %s where: (> v 990) asc: w})",
        "(select {v: v w: w from: %s where: (> v 990) desc: w take: 5})",
        "(select {v: v from: %s where: (> w 0) take: 6})",
        "(select {v: v from: %s where: (> w 0) take: -6})",
        "(select {x: (+ v w) y: (* f 2) from: %s where: (< k 2) take: 8})",
        "(select {c: (count v) from: %s})",
        "(select {s: (sum v) a: (avg f) mn: (min w) mx: (max w) from: %s})",
        "(select {s: (sum v) d: (dev f) from: %s where: (in k [1 2])})",
        "(select {d: (med v) from: %s where: (== s 'bb)})",
        "(select {s: (sum v) by: k from: %s})",
        "(select {s: (sum v) by: s from: %s})",
        "(select {s: (sum f) c: (count v) by: [k s] from: %s})",
        "(select {s: (sum v) by: k from: %s where: (> w 100)})",
        "(select {s: (sum v) by: m from: %s where: (== s 'cc) desc: s})",
        "(select {s: (sum v) by: k from: %s asc: s take: 3})",
        "(select {f1: (first v) l1: (last w) by: k from: %s})",
        "(select {mx: (max f) mn: (min v) by: [m k] from: %s})",
        "(select {c: (count (distinct v)) by: m from: %s})",
        "(select {c: (count (distinct s)) by: k from: %s where: (> v 10)})",
        "(select {x: (+ v 1) by: k from: %s where: (< w -39990)})",
        "(select {s: (sum v) by: (%% w 3) from: %s})",
        "(select {s: (sum v) by: k from: %s where: (> v 500) desc: s take: 2})",
        "(select {a: (avg v) by: k from: %s where: (== s 'aa)})",
        "(select {v: v from: %s where: (and (> v 100) (< w 0)) asc: v take: 4})",
        "(select {k: k s: s from: %s where: (== v 13) take: 3})",
        "(select {q: (* v 2) r: (- w 1) from: %s where: (== k 6) desc: v take: 2})",
        "(select {from: %s by: k})",
        "(select {from: %s by: m})",
        "(select {s: (sum v) by: {k: k} from: %s})",
        "(select {s: (sum v) by: {kk: (+ k 1) mm: m} from: %s})",
        "(select {c: (count v) by: {k: k kk: (- k 1)} from: %s})",
        "(select {s: (sum v) by: bl from: %s})",
        "(select {from: %s by: bl})",
        "(select {x: (+ v 1) by: st from: %s where: (< w -39990)})",
        "(select {m: (max v) x: (+ v 1) by: st from: %s where: (< w -39980)})",
        "(select {from: %s by: st})",
        "(select {from: %s by: [k f] where: (< w -39900)})",
        "(select {from: %s by: k where: (< w -50000)})",
        "(select {from: %s by: s where: (< w -50000)})",
        "(select {from: %s by: st where: (< w -50000)})",
        "(select {s: (sum v) by: k from: %s where: (< w -50000)})",
        "(select {s: (sum v) by: (+ k 1) from: %s where: (< w -50000)})",
        "(select {s: (sum v) x: (+ v 1) by: k from: %s where: (< w -39990)})",
        "(select {c: (count (distinct v)) s: (sum w) by: h from: %s})",
        "(select {d: (count (distinct v)) by: (%% m 2) from: %s})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        char q0[256], q2[256];
        snprintf(q0, sizeof q0, qs[i], "u0");
        snprintf(q2, sizeof q2, qs[i], "u2");
        ray_t* a = ray_eval_str(q0);
        ray_t* b = ray_eval_str(q2);
        bool ok = a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b) && t29_same(a, b) && cp_fmt_same(a, b);
        if (!ok) fprintf(stderr, "select_gen2_equal: %s\n", q2);
        if (a && RAY_IS_ERR(a)) ray_error_free(a); else if (a) ray_release(a);
        if (b && RAY_IS_ERR(b)) ray_error_free(b); else if (b) ray_release(b);
        TEST_ASSERT_FMT(ok, "query %d", i);
    }
    PASS();
}

/* ---- Task 31b: select over a parted table prunes before fetching ---- */

/* Select shapes over the parted table t; %s is the where predicate. */
static const char* const sp_shapes[] = {
    "(select {from: t where: %s})",
    "(select {s: (sum a) from: t where: %s})",
    "(select {c: (count a) from: t by: b where: %s})",
    "(select {a: a from: t where: %s take: 10})",
    "(select {a: a from: t where: %s take: -4})",
    "(select {a: a b: b from: t where: %s desc: a take: 5})",
    "(select {s: (sum a) m: (max b) by: date from: t where: %s})",
    "(select {c: (count b) from: t where: (and %s (> a 100))})",
    "(select {x: (+ a b) from: t where: %s})",
    "(select {from: t where: %s asc: b})",
    "(select {c: (count (distinct b)) by: date from: t where: %s})",
    /* shapes reading rows the where rejects, or positional: not pruned */
    "(select {r: (sums a) from: t where: %s})",
    "(select {r: (deltas b) from: t where: %s})",
    "(select {r: (- a (avg a)) from: t where: %s})",
    "(select {a: a from: t where: (and (> (count a) 6000) %s)})",
    "(select {c: (count (distinct date)) from: t where: %s})",
    "(select {s: (sum a) by: (sums b) from: t where: %s})",
    NULL,
};
/* Prunable predicates (the second rules every partition out). */
static const char* const sp_preds[] = {
    "(== date 2024.01.02)", "(> date 2024.01.03)", "(<= date 2024.01.02)",
    "(in date [2024.01.01 2024.01.03])", "(!= date 2024.01.02)", "(>= 2024.01.02 date)", NULL,
};

/* The query of shape i over predicate p; `unpruned` wraps the predicate in
 * an `or` the pruner does not read, so the whole table is scanned. */
static ray_t* sp_eval(int i, const char* p, bool unpruned) {
    char pred[160], q[400];
    if (unpruned) snprintf(pred, sizeof pred, "(or %s %s)", p, p);
    else snprintf(pred, sizeof pred, "%s", p);
    snprintf(q, sizeof q, sp_shapes[i], pred);
    return ray_eval_str(q);
}
static void sp_bind(ray_t* t) { ray_env_set(ray_sym_intern("t", 1), t); }

/* Both outcomes are errors with one code, or the same serialized value. */
static bool sp_same(ray_t* x, ray_t* y) {
    if (!x || !y) return false;
    if (RAY_IS_ERR(x) || RAY_IS_ERR(y))
        return RAY_IS_ERR(x) && RAY_IS_ERR(y) && strcmp(ray_err_code(x), ray_err_code(y)) == 0;
    return t29_same(x, y) && cp_fmt_same(x, y);
}

/* A select over a parted generation-2 table decodes only the surviving
 * partitions' referenced columns: the where on the partition key prunes
 * before any column is fetched.  Before this change the four shapes
 * materialized 6, 2, 6 and 3 segment columns. */
static test_result_t test_select_parted_prunes_fetch(void) {
    ray_t* t = pdb_make(ray_test_tmp(CS_TMP "/sp"), 7, false);
    ray_t* t0 = pdb_make(ray_test_tmp(CS_TMP "/sp0"), 0, false);
    TEST_ASSERT_TRUE(t && t0);
    ray_release(t);
    static const struct { const char* q; uint64_t cols; int64_t rows; } qs[] = {
        { "(select {from: t where: (== date 2024.01.02)})",                      2, 3000 },  /* a and b of 01.02 */
        { "(select {s: (sum a) from: t where: (== date 2024.01.02)})",           1, 1 },     /* a of 01.02 */
        { "(select {c: (count a) from: t by: b where: (== date 2024.01.02)})",   2, 3000 },  /* a and b of 01.02 */
        { "(select {a: a from: t where: (<= date 2024.01.02) take: 10})",        2, 10 },    /* a of 01.01 and 01.02 */
        { "(select {a: a from: t where: (<= date 2024.01.02) asc: b})",          4, 6000 },  /* a and b of 01.01, 01.02 */
        { "(select {c: (count a) from: t where: (> date 2024.01.03)})",          0, 1 },     /* the stand-in alone */
        { NULL, 0, 0 },
    };
    for (int i = 0; qs[i].q; i++) {
        t = ray_read_parted(ray_test_tmp(CS_TMP "/sp"), "t");                     /* a fresh load: nothing cached */
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        sp_bind(t);
        ray_release(t);
        ray_enc_counters_reset();
        ray_t* r = ray_eval_str(qs[i].q);
        ray_enc_counters_t c; ray_enc_counters_get(&c);
        sp_bind(t0);
        ray_t* r0 = ray_eval_str(qs[i].q);
        bool ok = r && !RAY_IS_ERR(r) && r->type == RAY_TABLE && ray_table_nrows(r) == qs[i].rows;
        bool same = sp_same(r, r0);
        drop_out(r); drop_out(r0);
        TEST_ASSERT_FMT(ok, "%s: answered", qs[i].q);
        TEST_ASSERT_FMT(same, "%s: generation 0 and 2 agree", qs[i].q);
        TEST_ASSERT_FMT(c.materialized == qs[i].cols, "%s: materialized %llu, expected %llu",
                        qs[i].q, (unsigned long long)c.materialized, (unsigned long long)qs[i].cols);
    }
    sp_bind(NULL);
    ray_release(t0);
    PASS();
}

/* Pruned answers (or errors) equal the unpruned scan's, on generation 0 and
 * 2, for every shape and prunable predicate (nothing surviving included);
 * generation 0 decodes nothing.  On these even partitions that includes the
 * whole scan's oddities (an empty first-partition group under by: on the
 * key, a `type` error for a distinct of the key).  On uneven partitions the
 * whole scan's spurious empty first-partition group under by: on the key
 * no longer appears when pruning applies (see prunes_by_key_uneven); it
 * still appears when nothing survives, as on dev, and for unpruned shapes. */
static test_result_t test_select_parted_prunes_same(void) {
    ray_t* t0 = pdb_make(ray_test_tmp(CS_TMP "/sq0"), 0, false);
    ray_t* t2 = pdb_make(ray_test_tmp(CS_TMP "/sq2"), 7, false);
    TEST_ASSERT_TRUE(t0 && t2);
    for (int i = 0; sp_shapes[i]; i++) {
        for (int k = 0; sp_preds[k]; k++) {
            sp_bind(t0);
            ray_enc_counters_reset();
            ray_t* p0 = sp_eval(i, sp_preds[k], false);
            ray_enc_counters_t c; ray_enc_counters_get(&c);
            ray_t* u0 = sp_eval(i, sp_preds[k], true);
            sp_bind(t2);
            ray_t* p2 = sp_eval(i, sp_preds[k], false);
            bool gen = sp_same(p0, p2);
            /* the count-in-where shape (14) is not pruned, and its `or` form
             * answers differently on the whole scan already (on dev too) */
            bool unp = i == 14 || sp_same(p0, u0);
            drop_out(p0); drop_out(u0); drop_out(p2);
            TEST_ASSERT_FMT(gen, "shape %d pred %s: generations agree", i, sp_preds[k]);
            TEST_ASSERT_FMT(unp, "shape %d pred %s: pruned equals unpruned", i, sp_preds[k]);
            TEST_ASSERT_EQ_U(c.materialized, 0);
        }
    }
    sp_bind(NULL);
    ray_release(t0); ray_release(t2);
    PASS();
}

/* Mixed generations and a ragged tail (the last partition lacks b): the
 * pruned answer equals all-generation-0's.  A where that prunes the ragged
 * partition (or prunes everything) answers as the whole table would, never
 * touching it; one that keeps it, or a shape that is not pruned, leaves the
 * source unpruned (the existing ragged-table behaviour, not compared here). */
static test_result_t test_select_parted_prunes_mixed(void) {
    static const unsigned masks[] = { 0x5, 0x2 };
    ray_t* t0 = pdb_make(ray_test_tmp(CS_TMP "/sx0"), 0, false);
    TEST_ASSERT_NOT_NULL(t0);
    for (int rag = 0; rag < 2; rag++) {
        for (int m = 0; m < 2; m++) {
            char rm[128];
            snprintf(rm, sizeof rm, ray_test_tmp(CS_TMP "/sxm_%d_%d"), m, rag);
            ray_t* tm = pdb_make(rm, masks[m], rag);
            TEST_ASSERT_NOT_NULL(tm);
            for (int i = 0; sp_shapes[i]; i++) {
                for (int k = 0; sp_preds[k]; k++) {
                    if (rag && (k == 3 || k == 4)) continue;     /* keeps 01.03 */
                    if (rag && i >= 11) continue;                /* not pruned: reads 01.03 */
                    sp_bind(t0);
                    ray_t* a = sp_eval(i, sp_preds[k], false);
                    sp_bind(tm);
                    ray_t* b = sp_eval(i, sp_preds[k], false);
                    bool same = sp_same(a, b) && !RAY_IS_ERR(b);
                    drop_out(a); drop_out(b);
                    TEST_ASSERT_FMT(same, "mask %u ragged %d shape %d pred %s", masks[m], rag, i, sp_preds[k]);
                }
            }
            ray_release(tm);
        }
    }
    sp_bind(NULL);
    ray_release(t0);
    PASS();
}

/* A lambda formal named like the key shadows it: the where compares the
 * formal, so nothing is pruned and the answer is the whole table's. */
static test_result_t test_select_parted_prunes_shadow(void) {
    ray_t* t = pdb_make(ray_test_tmp(CS_TMP "/sh"), 7, false);
    TEST_ASSERT_NOT_NULL(t);
    sp_bind(t);
    ray_release(t);
    ray_t* a = ray_eval_str("((fn [date] (select {s: (sum a) from: t where: (<= date 2024.01.01)})) 2024.01.01)");
    ray_t* b = ray_eval_str("(select {s: (sum a) from: t})");
    ray_t* c = ray_eval_str("((fn [date] (select {a: a from: t where: (== date 2024.01.02)})) 2024.01.01)");
    bool same = sp_same(a, b);
    bool none = c && !RAY_IS_ERR(c) && c->type == RAY_TABLE && ray_table_nrows(c) == 0;
    drop_out(a); drop_out(b); drop_out(c);
    sp_bind(NULL);
    TEST_ASSERT_TRUE(same);
    TEST_ASSERT_TRUE(none);
    PASS();
}

/* Nothing survives, partitions of different sizes: the source is the
 * stand-in alone (01.01's key, one synthetic row), nothing is decoded, and
 * the answers equal the whole scan's — an empty table with the source's
 * schema, a zero count, and (the whole scan's empty first-partition group)
 * one 2024.01.01 group with count 0 under by: date. */
static test_result_t test_select_parted_prunes_none(void) {
    static const int64_t rows[3] = { 2500, 700, 1800 };
    ray_t* t = pdb_make_rows(ray_test_tmp(CS_TMP "/sn"), 7, false, rows);
    TEST_ASSERT_NOT_NULL(t);
    ray_release(t);
    static const char* const qs[] = {
        "(select {from: t where: %s})",
        "(select {c: (count a) s: (sum b) from: t where: %s})",
        "(select {c: (count a) by: date from: t where: %s})",
        "(select {c: (count a) by: b from: t where: %s})",
        "(select {a: a from: t where: %s desc: a take: 3})",
        NULL,
    };
    ray_t* got[5] = {0};
    for (int i = 0; qs[i]; i++) {
        t = ray_read_parted(ray_test_tmp(CS_TMP "/sn"), "t");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        sp_bind(t);
        ray_release(t);
        char q[256], u[256];
        snprintf(q, sizeof q, qs[i], "(> date 2024.01.03)");
        snprintf(u, sizeof u, qs[i], "(or (> date 2024.01.03) (> date 2024.01.03))");
        ray_enc_counters_reset();
        got[i] = ray_eval_str(q);
        ray_enc_counters_t c; ray_enc_counters_get(&c);
        ray_t* ref = ray_eval_str(u);
        bool same = sp_same(got[i], ref);
        drop_out(ref);
        TEST_ASSERT_FMT(same, "%s: equals the whole scan", q);
        TEST_ASSERT_FMT(c.materialized == 0, "%s: materialized %llu", q, (unsigned long long)c.materialized);
    }
    sp_bind(NULL);
    bool shape = got[0] && got[0]->type == RAY_TABLE && ray_table_nrows(got[0]) == 0 && ray_table_ncols(got[0]) == 3;
    ray_t* cnt = got[1] && got[1]->type == RAY_TABLE ? ray_table_get_col(got[1], ray_sym_intern("c", 1)) : NULL;
    bool zero = cnt && cnt->len == 1 && ((const int64_t*)ray_data(cnt))[0] == 0;
    ray_t* key = got[2] && got[2]->type == RAY_TABLE ? ray_table_get_col(got[2], ray_sym_intern("date", 4)) : NULL;
    bool group = key && key->type == RAY_DATE && key->len == 1 && ((const int32_t*)ray_data(key))[0] == 8766;
    bool by_b = got[3] && got[3]->type == RAY_TABLE && ray_table_nrows(got[3]) == 0;
    bool topk = got[4] && got[4]->type == RAY_TABLE && ray_table_nrows(got[4]) == 0;
    for (int i = 0; i < 5; i++) drop_out(got[i]);
    TEST_ASSERT_TRUE(shape);
    TEST_ASSERT_TRUE(zero);
    TEST_ASSERT_TRUE(group);
    TEST_ASSERT_TRUE(by_b);
    TEST_ASSERT_TRUE(topk);
    PASS();
}

/* Uneven partitions (50/2000/3/4000/10 rows): a group by the key over a where
 * selecting the third partition answers the single correct group, on
 * generation 0 and 2.  The whole scan (dev) also answers an empty group for
 * 2024.01.01 here (its first partition has no survivor and fewer rows pass
 * than it holds); pruning drops that partition, and the spurious group with
 * it. */
static test_result_t test_select_parted_prunes_by_key_uneven(void) {
    static const int64_t rows[5] = { 50, 2000, 3, 4000, 10 };
    ray_t* t[2] = { NULL, NULL };
    for (int g = 0; g < 2; g++) {
        const char* root = g ? ray_test_tmp(CS_TMP "/bu2") : ray_test_tmp(CS_TMP "/bu0");
        for (int p = 0; p < 5; p++) {
            char dir[256]; snprintf(dir, sizeof dir, "%s/2024.01.0%d/t", root, p + 1);
            ray_t* x = ray_table_new(2);
            ray_t* a = i64_col(rows[p], 1);
            ray_t* b = i64_col(rows[p], 7);
            x = ray_table_add_col(x, ray_sym_intern("a", 1), a);
            x = ray_table_add_col(x, ray_sym_intern("b", 1), b);
            ray_release(a); ray_release(b);
            TEST_ASSERT_TRUE(ray_splay_save_mode(x, dir, NULL, g ? RAY_COMPRESS_ON : RAY_COMPRESS_OFF) == RAY_OK);
            ray_release(x);
        }
        t[g] = ray_read_parted(root, "t");
        TEST_ASSERT_TRUE(t[g] && !RAY_IS_ERR(t[g]));
    }
    static const struct { const char* q; int ng; int32_t d[2]; int64_t c[2]; } qs[] = {
        { "(select {c: (count a) by: date from: t where: (== date 2024.01.03)})",               1, { 8768, 0 }, { 3, 0 } },
        { "(select {c: (count a) by: date from: t where: (in date [2024.01.03 2024.01.05])})",  2, { 8768, 8770 }, { 3, 10 } },
        { "(select {c: (count a) by: date from: t where: (and (== date 2024.01.03) (> a 0))})", 1, { 8768, 0 }, { 2, 0 } },
        { NULL, 0, { 0, 0 }, { 0, 0 } },
    };
    for (int i = 0; qs[i].q; i++) {
        ray_t* r[2];
        for (int g = 0; g < 2; g++) { sp_bind(t[g]); r[g] = ray_eval_str(qs[i].q); }
        bool same = sp_same(r[0], r[1]);
        ray_t* d = r[0] && !RAY_IS_ERR(r[0]) && r[0]->type == RAY_TABLE ? ray_table_get_col(r[0], ray_sym_intern("date", 4)) : NULL;
        ray_t* c = r[0] && !RAY_IS_ERR(r[0]) && r[0]->type == RAY_TABLE ? ray_table_get_col(r[0], ray_sym_intern("c", 1)) : NULL;
        bool ok = d && c && d->type == RAY_DATE && d->len == qs[i].ng && c->len == qs[i].ng;
        for (int k = 0; ok && k < qs[i].ng; k++)
            ok = ((const int32_t*)ray_data(d))[k] == qs[i].d[k] && ((const int64_t*)ray_data(c))[k] == qs[i].c[k];
        drop_out(r[0]); drop_out(r[1]);
        TEST_ASSERT_FMT(same, "%s: generations agree", qs[i].q);
        TEST_ASSERT_FMT(ok, "%s: the surviving groups only", qs[i].q);
    }
    sp_bind(NULL);
    ray_release(t[0]); ray_release(t[1]);
    PASS();
}

/* A damaged surviving segment surfaces as `corrupt` with its path; a damaged
 * pruned segment is never read, and the query answers as undamaged. */
static test_result_t test_select_parted_prunes_failure(void) {
    ray_t* t = pdb_make(ray_test_tmp(CS_TMP "/sf"), 7, false);
    TEST_ASSERT_NOT_NULL(t);
    sp_bind(t);
    ray_t* want = ray_eval_str("(select {s: (sum a) from: t where: (== date 2024.01.02)})");
    TEST_ASSERT_TRUE(want && !RAY_IS_ERR(want));
    sp_bind(NULL);
    ray_release(t);
    uint8_t one = 1;
    FILE* f = fopen(ray_test_tmp(CS_TMP "/sf/2024.01.01/t/a"), "r+b");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 160 + 6, SEEK_SET); fwrite(&one, 1, 1, f); fclose(f);
    t = ray_read_parted(ray_test_tmp(CS_TMP "/sf"), "t");                       /* corruption passes load */
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    sp_bind(t);
    ray_release(t);
    static const char* const hit[] = {
        "(select {s: (sum a) from: t where: (<= date 2024.01.02)})",
        "(select {from: t where: (in date [2024.01.01])})",
        "(select {c: (count a) from: t by: b where: (!= date 2024.01.03)})",
        NULL,
    };
    for (int i = 0; hit[i]; i++) {
        ray_error_clear();
        ray_t* r = ray_eval_str(hit[i]);
        bool corrupt = r && RAY_IS_ERR(r) && strcmp(ray_err_code(r), "corrupt") == 0;
        bool path = ray_error_msg() && strstr(ray_error_msg(), "2024.01.01/t/a");
        drop_out(r);
        TEST_ASSERT_FMT(corrupt && path, "%s: corrupt with the path", hit[i]);
    }
    ray_t* got = ray_eval_str("(select {s: (sum a) from: t where: (== date 2024.01.02)})");
    bool same = sp_same(got, want);
    drop_out(got);
    ray_t* any = ray_eval_str("(select {from: t where: (> date 2024.01.01)})");
    bool ok = any && !RAY_IS_ERR(any) && ray_table_nrows(any) == 6000;
    drop_out(any);
    sp_bind(NULL);
    ray_release(want);
    TEST_ASSERT_TRUE(same);
    TEST_ASSERT_TRUE(ok);
    PASS();
}

/* A grouped non-aggregate output over a generation-2 table decodes only the
 * columns it names (and the key): the full-table evaluation behind the
 * per-group scatter binds the referenced columns, not every column.  The
 * DAG scatter (an integer key) and the eval-level group (a STR key) both. */
static test_result_t test_grouped_nonagg_binds_referenced(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['k 's 'v 'w 'z] (list (% (til 3000) 7) (at (list \"a\" \"b\" \"c\") (% (til 3000) 3)) (til 3000) (* 2 (til 3000)) (* 3 (til 3000)))))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/gnb\" t {compress: true})")), "");
    static const char* const qs[] = {
        "(select {x: (+ v 1) by: k from: u})",
        "(select {x: (+ v 1) by: s from: u})",
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u (.db.splayed.get \"" CS_TMP "/gnb\"))")), "");   /* nothing cached */
        ray_enc_counters_reset();
        ray_t* r = ray_eval_str(qs[i]);
        ray_enc_counters_t c; ray_enc_counters_get(&c);
        bool ok = r && !RAY_IS_ERR(r) && r->type == RAY_TABLE;
        if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
        TEST_ASSERT_FMT(ok, "%s: answered", qs[i]);
        TEST_ASSERT_FMT(c.materialized == 2, "%s: materialized %llu, expected 2 (the key and v)",
                        qs[i], (unsigned long long)c.materialized);
    }
    PASS();
}

/* simplify_agg_idiom: (first (asc c)) -> min is taken only for a null-free
 * column.  Over a parted generation-0 column with a null in one segment the
 * rewrite must decline (it would answer the min of the non-null cells); the
 * parted null-free column still takes it. */
static test_result_t test_query_first_asc_parted_nulls_gen0(void) {
    ray_t* s0 = i64_col(5, 1); ray_t* s1 = i64_col(5, 1);
    ray_vec_set_null(s1, 2, true);
    ray_t* w = ray_alloc(2 * sizeof(ray_t*));
    w->type = RAY_PARTED_BASE + RAY_I64; w->len = 2; w->attrs = 0;
    memset(w->aux, 0, 16);
    ((ray_t**)ray_data(w))[0] = s0; ((ray_t**)ray_data(w))[1] = s1;
    ray_t* t = ray_table_new(1);
    t = ray_table_add_col(t, ray_sym_intern("p", 1), w);
    ray_release(w);
    ray_t* kc = ray_vec_new(RAY_I64, 10); kc->len = 10;
    memset(ray_data(kc), 0, 10 * sizeof(int64_t));
    t = ray_table_add_col(t, ray_sym_intern("k", 1), kc);
    ray_release(kc);
    ray_env_set(ray_sym_intern("pt", 2), t);
    ray_t* r = ray_eval_str("(select {m: (first (asc p)) by: k from: pt})");
    ray_t* mc = (r && !RAY_IS_ERR(r) && r->type == RAY_TABLE) ? ray_table_col(r, ray_sym_intern("m", 1)) : NULL;
    bool declined = !(mc && mc->type == RAY_I64 && mc->len == 1 && !ray_vec_is_null(mc, 0));
    if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
    TEST_ASSERT_TRUE(declined);
    /* without a null the rewrite is taken: the answer is the minimum */
    ray_vec_set_null(s1, 2, false);
    ((int64_t*)ray_data(s1))[2] = 7;
    s1->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    r = ray_eval_str("(select {m: (first (asc p)) by: k from: pt})");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_TABLE);
    ray_t* col = ray_table_col(r, ray_sym_intern("m", 1));
    TEST_ASSERT_TRUE(col && col->len == 1 && ((const int64_t*)ray_data(col))[0] == 0);
    ray_release(r);
    ray_release(t);
    PASS();
}

/* Planning a full select reads metadata only: no column is materialized
 * before the first ray_execute. */
static test_result_t test_select_planning_no_materialize(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['k 'v 's] (list [1 2 3 4 5 6] [1.5 2.5 3.5 4.5 5.5 6.5] [\"a\" \"b\" \"a\" \"c\" \"b\" \"a\"])))"), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/sp\" t {compress: true})")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u (.db.splayed.get \"" CS_TMP "/sp\"))")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(.db.splayed.set \"" CS_TMP "/spz\" (table ['v] (list (til 70000))) {compress: true})")), "");
    TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set w (.db.splayed.get \"" CS_TMP "/spz\"))")), "");
    static const char* const qs[] = {
        "(select {from: u where: (> k 2)})",
        "(select {s: (sum v) from: u by: k})",
        "(select {from: u where: (in k [1 3 5])})",
        "(select {from: u where: (== s \"a\")})",           /* a scalar reduction or group-by under a filter pre-executes it in query.c, never reaching ray_execute */
        "(select {s: (sum v) from: w})",                /* 70000 rows: CHUNK_ZONE recorded */
        NULL,
    };
    for (int i = 0; qs[i]; i++) {
        /* a fresh table per query: a column decoded by an earlier one stays cached */
        TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set u (.db.splayed.get \"" CS_TMP "/sp\"))")), "");
        TEST_ASSERT_STR_EQ(ev_code(ray_test_tmp("(set w (.db.splayed.get \"" CS_TMP "/spz\"))")), "");
        ray_enc_counters_reset();
        ray_t* r = ray_eval_str(qs[i]);
        TEST_ASSERT_FMT(r && !RAY_IS_ERR(r), "query %d: %s", i, r ? ray_err_code(r) : "null");
        ray_release(r);
        ray_enc_counters_t c; ray_enc_counters_get(&c);
        TEST_ASSERT_FMT(c.at_first_exec == 0, "query %d materialized %llu columns before execution",
                        i, (unsigned long long)c.at_first_exec);
    }
    PASS();
}

/* A computed key beside a renamed one over a parted generation-2 table is
 * deferred to the per-partition group kernel, as over generation 0.  The
 * select's streamable-key gate read an encoded parted slot's type as the
 * parted type (RAY_PARTED_BASE + SYM), not its base type, so the by-dict was
 * flattened eagerly and the group never streamed. */
static test_result_t test_select_parted_bydict_streams(void) {
    static const char* const gens[] = { "true", "false" };
    ray_t* res[2] = { NULL, NULL };
    for (int g = 0; g < 2; g++) {
        char q[900];
        snprintf(q, sizeof q,
                 "(do (set P (table [ts sg v] (list (til 400) (as 'SYM (%% (til 400) 2)) (as 'F64 (til 400)))))"
                 " (.db.splayed.set \"%s/pb%d/1/t/\" P {compress: %s})"
                 " (.db.splayed.set \"%s/pb%d/2/t/\" P {compress: %s})"
                 " (set t (.db.parted.get \"%s/pb%d/\" 't))"
                 " (set B (at (.sys.mem) 'group-perpart-runs))"
                 " (set R (select {sv: (sum v) by: {tb: (xbar ts 200) s: sg} from: t asc: tb}))"
                 " (list (- (at (.sys.mem) 'group-perpart-runs) B) R))",
                 ray_test_tmp(CS_TMP), g, gens[g], ray_test_tmp(CS_TMP), g, gens[g], ray_test_tmp(CS_TMP), g);
        res[g] = ray_eval_str(q);
        TEST_ASSERT_FMT(res[g] && !RAY_IS_ERR(res[g]) && res[g]->type == RAY_LIST && res[g]->len == 2,
                        "compress %s: answered", gens[g]);
        ray_t* runs = ((ray_t**)ray_data(res[g]))[0];
        TEST_ASSERT_FMT(runs->i64 == 1, "compress %s: per-partition runs %lld, expected 1",
                        gens[g], (long long)runs->i64);
    }
    ray_t* r2 = ((ray_t**)ray_data(res[0]))[1];
    ray_t* r0 = ((ray_t**)ray_data(res[1]))[1];
    TEST_ASSERT_EQ_I(ray_table_nrows(r2), 4);
    TEST_ASSERT_TRUE(sp_same(r2, r0));
    ray_release(res[0]); ray_release(res[1]);
    PASS();
}

/* The empty-key and nullability helpers of the select planner, over a parted
 * generation-2 table, answer as over generation 0: a grouped select whose
 * where leaves no row (zero groups: the empty key column is built from the
 * filtered table), and a nullable sort key under a positional take.  The
 * parted arms of those helpers (an encoded parted slot's base type, a plain
 * parted wrapper's nulls from its segments) sit behind gates no select
 * reaches today: an unfiltered zero-group select needs an empty parted table,
 * which the parted group refuses first (nyi, both generations), and the
 * nullable-key take needs SORTED, which no parted slot carries. */
static test_result_t test_select_parted_empty_nullable(void) {
    static const char* const qs[] = {
        "(select {from: t by: sg where: (> v 1000.0)})",
        "(select {from: t by: n where: (> v 1000.0)})",
        "(select {from: t by: s where: (> v 1000.0)})",
        "(select {x: (count v) from: t by: n where: (> v 1000.0)})",
        "(select {from: t where: (> v 10.0) asc: n take: 3})",
        "(select {from: t where: (> v 10.0) desc: n take: 3})",
        "(select {from: t by: sg})",
        NULL,   /* (select {from: t}) stays parted: no serialized form to compare (#728) */
    };
    ray_t* tb[2] = { NULL, NULL };
    static const char* const gens[] = { "true", "false" };
    for (int g = 0; g < 2; g++) {
        char q[900];
        snprintf(q, sizeof q,
                 "(do (set P (table [sg v n s] (list (as 'SYM (%% (til 400) 2)) (as 'F64 (til 400))"
                 " (concat [0N 0N] (til 398)) (at [\"a\" \"\" \"c\"] (%% (til 400) 3)))))"
                 " (.db.splayed.set \"%s/pe%d/1/t/\" P {compress: %s})"
                 " (.db.splayed.set \"%s/pe%d/2/t/\" P {compress: %s})"
                 " (.db.parted.get \"%s/pe%d/\" 't))",
                 ray_test_tmp(CS_TMP), g, gens[g], ray_test_tmp(CS_TMP), g, gens[g], ray_test_tmp(CS_TMP), g);
        tb[g] = ray_eval_str(q);
        TEST_ASSERT_FMT(tb[g] && !RAY_IS_ERR(tb[g]) && tb[g]->type == RAY_TABLE,
                        "compress %s: loaded", gens[g]);
    }
    TEST_ASSERT_TRUE(ray_table_get_col_enc(tb[0], ray_sym_intern("n", 1)) != NULL);
    for (int i = 0; qs[i]; i++) {
        sp_bind(tb[0]);
        ray_t* r2 = ray_eval_str(qs[i]);
        sp_bind(tb[1]);
        ray_t* r0 = ray_eval_str(qs[i]);
        bool same = sp_same(r2, r0);
        drop_out(r2); drop_out(r0);
        TEST_ASSERT_FMT(same, "%s: generation 0 and 2 agree", qs[i]);
    }
    sp_bind(NULL);
    ray_release(tb[0]); ray_release(tb[1]);
    PASS();
}

/* The parted group's cardinality estimate over an I64 key whose first
 * partition holds a null (INT64_MIN) beside positive values: the span is
 * taken in u64 and capped, never a signed overflow (UBSan), and the grouped
 * answer equals the flat one, in both generations. */
static test_result_t test_parted_group_null_i64_estimate(void) {
    static const char* const gens[] = { "true", "false" };
    for (int g = 0; g < 2; g++) {
        char q[900];
        snprintf(q, sizeof q,
                 "(do (set P (table [n v] (list (concat [0N 0N] (%% (til 398) 3)) (as 'F64 (til 400)))))"
                 " (.db.splayed.set \"%s/pn%d/1/t/\" P {compress: %s})"
                 " (.db.splayed.set \"%s/pn%d/2/t/\" P {compress: %s})"
                 " (set t (.db.parted.get \"%s/pn%d/\" 't))"
                 " (set F (table [n v] (list (concat (at P 'n) (at P 'n)) (concat (at P 'v) (at P 'v)))))"
                 " (list (select {s: (sum v) c: (count v) from: t by: n asc: n})"
                 "       (select {s: (sum v) c: (count v) from: F by: n asc: n})))",
                 ray_test_tmp(CS_TMP), g, gens[g], ray_test_tmp(CS_TMP), g, gens[g], ray_test_tmp(CS_TMP), g);
        ray_t* r = ray_eval_str(q);
        bool ok = r && !RAY_IS_ERR(r) && r->type == RAY_LIST && r->len == 2 &&
                  ray_table_nrows(((ray_t**)ray_data(r))[0]) == 4 &&
                  sp_same(((ray_t**)ray_data(r))[0], ((ray_t**)ray_data(r))[1]);
        drop_out(r);
        TEST_ASSERT_FMT(ok, "compress %s: parted group equals flat", gens[g]);
    }
    PASS();
}

const test_entry_t compress_entries[] = {
    { "compress/select_planning_no_materialize", test_select_planning_no_materialize, rt_setup, rt_teardown },
    { "compress/stream_matches_resident", test_stream_matches_resident, cs_setup, cs_teardown },
    { "compress/stream_str_matches_resident", test_stream_str_matches_resident, cs_setup, cs_teardown },
    { "compress/stream_sym_gen2",          test_stream_sym_gen2,          cs_setup, cs_teardown },
    { "compress/stream_gen2_transcode_failure", test_stream_gen2_transcode_failure, cs_setup, cs_teardown },
    { "compress/str_header_no_pointer",    test_str_header_no_pointer,    cs_setup, cs_teardown },
    { "compress/csv_splayed_symbol",       test_csv_splayed_symbol,       rt_setup, rt_teardown },
    { "compress/csv_splayed_option",      test_csv_splayed_option,      rt_setup, rt_teardown },
    { "compress/parquet_option",          test_parquet_option,          rt_setup, rt_teardown },
    { "compress/parquet_two_dicts",       test_parquet_two_dicts,       rt_setup, rt_teardown },
    { "compress/merge_mixed_widths",  test_merge_mixed_widths,  cs_setup, cs_teardown },
    { "compress/absent_byte_identity", test_absent_byte_identity, cs_setup, cs_teardown },
    { "compress/unreadable_destination", test_unreadable_destination, cs_setup, cs_teardown },
    { "compress/inherit_destination", test_inherit_destination, cs_setup, cs_teardown },
    { "compress/update_then_save",    test_update_then_save,    cs_setup, cs_teardown },
    { "compress/forced_generation0",  test_forced_generation0,  cs_setup, cs_teardown },
    { "compress/chunk_zone_recorded", test_chunk_zone_recorded, cs_setup, cs_teardown },
    { "compress/matrix_parted_indexed", test_matrix_parted_indexed, cs_setup, cs_teardown },
    { "compress/staged_and_sym",      test_staged_and_sym,      cs_setup, cs_teardown },
    { "compress/save_refuses_parted_kinds", test_save_refuses_parted_kinds, rt_setup, rt_teardown },
    { "compress/resolver_rules",      test_resolver_rules,      cs_setup, cs_teardown },
    { "compress/write_sym_domains",      test_write_sym_domains,      cs_setup, cs_teardown },
    { "compress/write_parted_refused",   test_write_parted_refused,   rt_setup, rt_teardown },
    { "compress/matrix_failure_inject",  test_matrix_failure_inject,  cs_setup, cs_teardown },
    { "compress/matrix_flat",            test_matrix_flat,            cs_setup, cs_teardown },
    { "compress/matrix_sym_remap",       test_matrix_sym_remap,       cs_setup, cs_teardown },
    { "compress/matrix_failure_cleanup", test_matrix_failure_cleanup, cs_setup, cs_teardown },
    { "compress/str_const_offsets",      test_str_const_distinct_offsets, cs_setup, cs_teardown },
    { "compress/ipc_failed_decode",             test_ipc_failed_decode,              ipc_setup, ipc_teardown },
    { "compress/dml_upsert_copy", test_dml_upsert_copy, rt_setup, rt_teardown },
    { "compress/dml_upsert_inplace", test_dml_upsert_inplace, rt_setup, rt_teardown },
    { "compress/dml_insert_copy", test_dml_insert_copy, rt_setup, rt_teardown },
    { "compress/dml_insert_inplace", test_dml_insert_inplace, rt_setup, rt_teardown },
    { "compress/dml_update_copy", test_dml_update_copy, rt_setup, rt_teardown },
    { "compress/dml_update_inplace", test_dml_update_inplace, rt_setup, rt_teardown },
    { "compress/dml_delete_copy", test_dml_delete_copy, rt_setup, rt_teardown },
    { "compress/dml_delete_inplace", test_dml_delete_inplace, rt_setup, rt_teardown },
    { "compress/value_ops_copy_encoded", test_value_ops_copy_encoded, rt_setup, rt_teardown },
    { "compress/dml_on_select_result", test_dml_on_select_result, rt_setup, rt_teardown },
    { "compress/failed_decode_propagates",      test_failed_decode_propagates,       rt_setup, rt_teardown },
    { "compress/serde_byte_identity",           test_serde_byte_identity,            rt_setup, rt_teardown },
    { "compress/getter_keeps_pending_message",  test_getter_keeps_pending_message,   cs_setup, cs_teardown },
    { "compress/objsize_counts_cache",          test_objsize_counts_cache,           rt_setup, rt_teardown },
    { "compress/has_nulls_parted",              test_has_nulls_parted,               cs_setup, cs_teardown },
    { "compress/parted_gen2_loads",           test_parted_gen2_loads,            rt_setup, rt_teardown },
    { "compress/meta_without_decode",           test_meta_without_decode,            cs_setup, cs_teardown },
    { "compress/user_values_are_plain",         test_user_values_are_plain,          rt_setup, rt_teardown },
    { "compress/builtins_failed_decode",        test_builtins_failed_decode,         rt_setup, rt_teardown },
    { "compress/rerank_failed_decode",          test_rerank_failed_decode,           rt_setup, rt_teardown },
    { "compress/traverse_failed_decode",        test_traverse_failed_decode,         rt_setup, rt_teardown },
    { "compress/like_env_failed_decode",        test_like_env_failed_decode,         rt_setup, rt_teardown },
    { "compress/legacy_getters",                test_legacy_getters,                 cs_setup, cs_teardown },
    { "compress/concurrent_one_materialization",test_concurrent_one_materialization, cs_setup, cs_teardown },
    { "compress/serde_encoded_table",           test_serde_encoded_table,            cs_setup, cs_teardown },
    { "compress/concurrent_col_index",          test_concurrent_col_index,           cs_setup, cs_teardown },
    { "compress/legacy_transient_no_leak",      test_legacy_transient_no_leak,       cs_setup, cs_teardown },
    { "compress/db_splayed_set_forms", test_db_splayed_set_forms, rt_setup, rt_teardown },
    { "compress/csv_parted_option",    test_csv_parted_option,    rt_setup, rt_teardown },
    { "compress/planning_never_materializes", test_planning_never_materializes, cs_setup, cs_teardown },
    { "compress/exec_start_snapshot",         test_exec_start_snapshot,         cs_setup, cs_teardown },
    { "compress/first_asc_parted_nulls_gen0", test_first_asc_parted_nulls_gen0, cs_setup, cs_teardown },
    { "compress/query_first_asc_parted_nulls_gen0", test_query_first_asc_parted_nulls_gen0, rt_setup, rt_teardown },
    { "compress/grouped_nonagg_binds_referenced", test_grouped_nonagg_binds_referenced, rt_setup, rt_teardown },
    { "compress/first_asc_parted_nulls_gen2", test_first_asc_parted_nulls_gen2, rt_setup, rt_teardown },
    { "compress/first_asc_parted_text_nulls", test_first_asc_parted_text_nulls, rt_setup, rt_teardown },
    { "compress/parted_text_nulls_mixed_gen", test_parted_text_nulls_mixed_gen, rt_setup, rt_teardown },
    { "compress/bind_stage",        test_bind_stage,       cs_setup, cs_teardown },
    { "compress/fused_pred_bound",  test_fused_pred_bound, rt_setup, rt_teardown },
    { "compress/fused_pred_enc_source", test_fused_pred_enc_source, rt_setup, rt_teardown },
    { "compress/fused_pred_bind_failure", test_fused_pred_bind_failure, rt_setup, rt_teardown },
    { "compress/bind_dedup_validates",    test_bind_dedup_validates,    rt_setup, rt_teardown },
    { "compress/bind_address_reuse",      test_bind_address_reuse,      rt_setup, rt_teardown },
    { "compress/unbind_releases_temporaries", test_unbind_releases_temporaries, rt_setup, rt_teardown },
    { "compress/compile_paths_bound",     test_compile_paths_bound,     rt_setup, rt_teardown },
    { "compress/try_plans_bound",         test_try_plans_bound,         rt_setup, rt_teardown },
    { "compress/expr_compile_bound",      test_expr_compile_bound,      rt_setup, rt_teardown },
    { "compress/group_desc_bound",        test_group_desc_bound,        rt_setup, rt_teardown },
    { "compress/topk_bound",              test_topk_bound,              rt_setup, rt_teardown },
    { "compress/slice_group_bound",       test_slice_group_bound,       rt_setup, rt_teardown },
    { "compress/parted_pruning_fetch",    test_parted_pruning_fetch,    rt_setup, rt_teardown },
    { "compress/parted_pruning_gen0",     test_parted_pruning_gen0,     rt_setup, rt_teardown },
    { "compress/parted_pruning_empty",    test_parted_pruning_empty,    rt_setup, rt_teardown },
    { "compress/parted_pruning_multi",    test_parted_pruning_multi,    rt_setup, rt_teardown },
    { "compress/parted_pruning_failure",  test_parted_pruning_failure,  rt_setup, rt_teardown },
    { "compress/parted_pruning_mixed",    test_parted_pruning_mixed,    rt_setup, rt_teardown },
    { "compress/exec_sites_failed_decode", test_exec_sites_failed_decode, rt_setup, rt_teardown },
    { "compress/exec_sites_gen2_equal",    test_exec_sites_gen2_equal,    rt_setup, rt_teardown },
    { "compress/filter_absent_symbol",     test_filter_absent_symbol,     rt_setup, rt_teardown },
    { "compress/group_sites_failed_decode", test_group_sites_failed_decode, rt_setup, rt_teardown },
    { "compress/group_sites_routes_same",   test_group_sites_routes_same,   rt_setup, rt_teardown },
    { "compress/group_sites_slice_route",   test_group_sites_slice_route,   rt_setup, rt_teardown },
    { "compress/group_sites_parted",        test_group_sites_parted,        rt_setup, rt_teardown },
    { "compress/group_sites_parted_failed_decode", test_group_sites_parted_failed_decode, rt_setup, rt_teardown },
    { "compress/join_sites_failed_decode",   test_join_sites_failed_decode,   rt_setup, rt_teardown },
    { "compress/join_sites_gen2_equal",      test_join_sites_gen2_equal,      rt_setup, rt_teardown },
    { "compress/tblop_sites_gen2_equal",     test_tblop_sites_gen2_equal,     rt_setup, rt_teardown },
    { "compress/tblop_sites_failed_decode",  test_tblop_sites_failed_decode,  rt_setup, rt_teardown },
    { "compress/datalog_sites_failed_decode", test_datalog_sites_failed_decode, rt_setup, rt_teardown },
    { "compress/datalog_sites_gen2_equal",   test_datalog_sites_gen2_equal,   rt_setup, rt_teardown },
    { "compress/join_sites_parted",          test_join_sites_parted,          rt_setup, rt_teardown },
    { "compress/dml_parted_gen2",            test_dml_parted_gen2,            rt_setup, rt_teardown },
    { "compress/parted_insert_container",    test_parted_insert_container,    rt_setup, rt_teardown },
    { "compress/datalog_query_gen2_equal",   test_datalog_query_gen2_equal,   rt_setup, rt_teardown },
    { "compress/scalar_agg_planning_zoned", test_scalar_agg_planning_zoned, rt_setup, rt_teardown },
    { "compress/dag_presence_without_decode", test_dag_presence_without_decode, rt_setup, rt_teardown },
    { "compress/query_helpers_gen2_equal",   test_query_helpers_gen2_equal,   rt_setup, rt_teardown },
    { "compress/query_helpers_failed_decode", test_query_helpers_failed_decode, rt_setup, rt_teardown },
    { "compress/select_sites_failed_decode", test_select_sites_failed_decode, rt_setup, rt_teardown },
    { "compress/select_routes_failed_decode", test_select_routes_failed_decode, rt_setup, rt_teardown },
    { "compress/select_slice_probe_failed_decode", test_select_slice_probe_failed_decode, rt_setup, rt_teardown },
    { "compress/select_bydict_binds_referenced", test_select_bydict_binds_referenced, rt_setup, rt_teardown },
    { "compress/select_decodes_referenced_only", test_select_decodes_referenced_only, rt_setup, rt_teardown },
    { "compress/select_parted_prunes_fetch",   test_select_parted_prunes_fetch,   rt_setup, rt_teardown },
    { "compress/select_parted_prunes_same",    test_select_parted_prunes_same,    rt_setup, rt_teardown },
    { "compress/select_parted_prunes_mixed",   test_select_parted_prunes_mixed,   rt_setup, rt_teardown },
    { "compress/select_parted_prunes_shadow",  test_select_parted_prunes_shadow,  rt_setup, rt_teardown },
    { "compress/select_parted_prunes_none",    test_select_parted_prunes_none,    rt_setup, rt_teardown },
    { "compress/select_parted_prunes_by_key_uneven", test_select_parted_prunes_by_key_uneven, rt_setup, rt_teardown },
    { "compress/select_parted_prunes_failure", test_select_parted_prunes_failure, rt_setup, rt_teardown },
    { "compress/select_gen2_equal",          test_select_gen2_equal,          rt_setup, rt_teardown },
    { "compress/select_parted_bydict_streams", test_select_parted_bydict_streams, rt_setup, rt_teardown },
    { "compress/select_parted_empty_nullable", test_select_parted_empty_nullable, rt_setup, rt_teardown },
    { "compress/parted_group_null_i64_estimate", test_parted_group_null_i64_estimate, rt_setup, rt_teardown },
    { NULL, NULL, NULL, NULL },
};
