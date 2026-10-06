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

static void cs_setup(void)    { ray_heap_init(); (void)ray_sym_init(); (void)ray_test_rm_rf(CS_TMP); (void)ray_test_mkdir_p(CS_TMP); }
static void cs_teardown(void) { (void)ray_test_rm_rf(CS_TMP); ray_sym_destroy(); ray_heap_destroy(); }

static ray_t* i64_col(int64_t n, int64_t mul) {
    ray_t* v = ray_vec_new(RAY_I64, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = i * mul;
    return v;
}

/* A loaded table whose column `a` is generation 2 and `b` generation 0. */
static ray_t* mixed_table(void) {
    char p[256];
    (void)ray_test_mkdir_p(CS_TMP "/mix");
    ray_t* a = i64_col(3000, 3); ray_t* b = i64_col(3000, 5);
    snprintf(p, sizeof p, "%s/mix/a", CS_TMP); ray_col_save_gen(a, p, 2);
    snprintf(p, sizeof p, "%s/mix/b", CS_TMP); ray_col_save_gen(b, p, 0);
    ray_release(a); ray_release(b);
    ray_t* names = ray_vec_new(RAY_STR, 2);
    names = ray_str_vec_append(names, "a", 1); names = ray_str_vec_append(names, "b", 1);
    snprintf(p, sizeof p, "%s/mix/.d", CS_TMP); ray_col_save(names, p); ray_release(names);
    return ray_read_splayed(CS_TMP "/mix", NULL);
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
    FILE* f = fopen(CS_TMP "/mix/a", "r+b"); fseek(f, 160 + 6, SEEK_SET); fputc(1, f); fclose(f);
    t = ray_read_splayed(CS_TMP "/mix", NULL);
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    for (int i = 0; i < 4; i++) {
        stale = ray_error("domain", "stale"); ray_error_free(stale);
        TEST_ASSERT_NULL(ray_table_get_col(t, ray_sym_intern("a", 1)));
        TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), CS_TMP "/mix/a"));
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
    (void)ray_test_mkdir_p(CS_TMP "/ux");
    ray_t* a = i64_col(3000, 7);
    ray_t* root = ray_index_new_root(RAY_IDX_NONE, RAY_I64, 3000);
    ray_index_payload(root)->markers = RAY_MARK_UNIQUE;
    ray_t* ua = ray_index_attach_persisted(&a, root);
    TEST_ASSERT_TRUE(ua && !RAY_IS_ERR(ua)); a = ua;
    TEST_ASSERT_EQ_I(ray_col_save_gen(a, CS_TMP "/ux/a", 2), RAY_OK);
    ray_release(a);
    ray_t* names = ray_vec_new(RAY_STR, 1); names = ray_str_vec_append(names, "a", 1);
    ray_col_save(names, CS_TMP "/ux/.d"); ray_release(names);
    ray_t* t = ray_read_splayed(CS_TMP "/ux", NULL);
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

static void rt_setup(void)    { ray_runtime_create(0, NULL); (void)ray_test_rm_rf(CS_TMP); (void)ray_test_mkdir_p(CS_TMP); }
static void rt_teardown(void) { (void)ray_test_rm_rf(CS_TMP); ray_runtime_destroy(__RUNTIME); }

/* A table `name` (columns k, v: I64 3000 rows, k = i, v = 10 i) saved as
 * generation 2, loaded and bound to the global `t`.  Returns the retained
 * loaded table. */
static ray_t* bind_gen2_table(const char* name) {
    char dir[256], p[300];
    snprintf(dir, sizeof dir, "%s/%s", CS_TMP, name);
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

/* A generation-2 table whose column `a` loads but fails to materialize
 * (corrupt descriptor, as test_legacy_getters), bound to `t`. */
static ray_t* bind_corrupt_table(void) {
    ray_t* t = mixed_table();
    if (!t || RAY_IS_ERR(t)) return NULL;
    ray_release(t);
    FILE* f = fopen(CS_TMP "/mix/a", "r+b"); fseek(f, 160 + 6, SEEK_SET); fputc(1, f); fclose(f);
    t = ray_read_splayed(CS_TMP "/mix", NULL);
    if (t && !RAY_IS_ERR(t)) ray_env_set(ray_sym_intern("t", 1), t);
    return t;
}

static test_result_t expect_corrupt(ray_t* r, const char* what) {
    TEST_ASSERT_FMT(r && RAY_IS_ERR(r), "%s: expected an error", what);
    TEST_ASSERT_FMT(strcmp(ray_err_code(r), "corrupt") == 0, "%s: code %s (%s)", what, ray_err_code(r), ray_error_msg() ? ray_error_msg() : "no message");
    TEST_ASSERT_FMT(ray_error_msg() && strstr(ray_error_msg(), CS_TMP "/mix/a"), "%s: message %s", what,
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
        "(update {b: 1 from: 't where: (== b 5)})", "(update {b: 1 from: t})",
        "(delete {from: 't where: (== b 5)})", "(delete {from: t where: (== b 5)})",
        "(select {s: (sum a) from: t})", "(select {from: t where: (> a 5)})",
    };
    for (size_t i = 0; i < sizeof verbs / sizeof *verbs; i++) {
        ray_error_clear();
        ray_t* r = ray_eval_str(verbs[i]);
        { test_result_t rr = expect_corrupt(r, verbs[i]); if (rr.status != TEST_PASS) return rr; }
    }
    /* a splayed re-save */
    ray_error_clear();
    TEST_ASSERT_EQ_I(ray_splay_save(t, CS_TMP "/resave", NULL), RAY_ERR_CORRUPT);
    TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), CS_TMP "/mix/a"));
    /* serialization reports the same error */
    ray_error_clear();
    TEST_ASSERT_TRUE(ray_serde_size(t) < 0);
    ray_t* w = ray_ser(t);
    { test_result_t rr = expect_corrupt(w, "ray_ser"); if (rr.status != TEST_PASS) return rr; }
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
    TEST_ASSERT_EQ_I(ray_splay_save(ref, CS_TMP "/g0", NULL), RAY_OK);
    (void)ray_test_mkdir_p(CS_TMP "/g2");
    ray_t* names = ray_vec_new(RAY_STR, 2);
    for (int64_t c = 0; c < 2; c++) {
        char p[300]; ray_t* nm = ray_sym_str(ray_table_col_name(ref, c));
        snprintf(p, sizeof p, "%s/g2/%.*s", CS_TMP, (int)ray_str_len(nm), ray_str_ptr(nm));
        TEST_ASSERT_EQ_I(ray_col_save_gen(ray_table_get_col_idx(ref, c), p, 2), RAY_OK);
        names = ray_str_vec_append(names, ray_str_ptr(nm), ray_str_len(nm));
    }
    ray_col_save(names, CS_TMP "/g2/.d"); ray_release(names);
    ray_t* t0 = ray_read_splayed(CS_TMP "/g0", NULL);
    ray_t* t2 = ray_read_splayed(CS_TMP "/g2", NULL);
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
    FILE* f = fopen(CS_TMP "/mix/a", "r+b"); fseek(f, 160 + 6, SEEK_SET); fputc(1, f); fclose(f);
    ray_release(t);
    t = ray_read_splayed(CS_TMP "/mix", NULL);
    ray_error_free(ray_error("domain", "x"));
    TEST_ASSERT_NULL(ray_table_get_col(t, ray_sym_intern("a", 1)));
    TEST_ASSERT_TRUE(ray_error_msg() && strstr(ray_error_msg(), CS_TMP "/mix/a"));
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
    snprintf(dir, sizeof dir, "%s/pdb/%s/tb", CS_TMP, day);
    (void)ray_test_mkdir_p(dir);
    ray_t* a = i64_col(3000, 3);
    snprintf(p, sizeof p, "%s/a", dir); ray_col_save_gen(a, p, gen);
    ray_release(a);
    ray_t* names = ray_vec_new(RAY_STR, 1); names = ray_str_vec_append(names, "a", 1);
    snprintf(p, sizeof p, "%s/.d", dir); ray_col_save(names, p); ray_release(names);
}
static test_result_t test_parted_gen2_loads(void) {
    write_part("2024.01.01", 0); write_part("2024.01.02", 0);
    ray_t* ok = ray_read_parted(CS_TMP "/pdb", "tb");
    TEST_ASSERT_TRUE(ok && !RAY_IS_ERR(ok));
    TEST_ASSERT_EQ_I(ray_table_ncols(ok), 2);
    ray_release(ok);
    ray_t* a = NULL;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) { write_part("2024.01.01", 2); write_part("2024.01.02", 0); }
        else           { write_part("2024.01.01", 0); write_part("2024.01.02", 2); }
        ray_t* t = ray_read_parted(CS_TMP "/pdb", "tb");
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
    TEST_ASSERT_EQ_I(save_ctx(v, CS_TMP "/p0", 0, NULL), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(v, CS_TMP "/ref0"), RAY_OK);
    size_t na, nb; uint8_t* a = slurp(CS_TMP "/p0", &na); uint8_t* b = slurp(CS_TMP "/ref0", &nb);
    TEST_ASSERT_TRUE(na == nb && memcmp(a, b, na) == 0); free(a); free(b);
    /* plain -> 2 */
    TEST_ASSERT_EQ_I(save_ctx(v, CS_TMP "/p2", 2, NULL), RAY_OK);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/p2"), 2);
    /* encoded -> 2: range reads, no materialization, identical bytes */
    ray_t* enc = ray_col_mmap(CS_TMP "/p2");
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(save_ctx(enc, CS_TMP "/e2", 2, NULL), RAY_OK);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    a = slurp(CS_TMP "/p2", &na); b = slurp(CS_TMP "/e2", &nb);
    TEST_ASSERT_TRUE(na == nb && memcmp(a, b, na) == 0); free(a); free(b);
    /* encoded -> 0: materializes once, then today's writer */
    TEST_ASSERT_EQ_I(save_ctx(enc, CS_TMP "/e0", 0, NULL), RAY_OK);
    ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 1);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/e0"), 0);
    a = slurp(CS_TMP "/e0", &na); b = slurp(CS_TMP "/ref0", &nb);
    TEST_ASSERT_TRUE(na == nb && memcmp(a, b, na) == 0); free(a); free(b);
    ray_release(enc); ray_release(v);
    PASS();
}

/* A SYM column over domain A (insertion order z, a, m) re-encoded into a
 * domain B with another order and a wider width class. */
static test_result_t test_matrix_sym_remap(void) {
    struct ray_sym_domain_s* A = ray_sym_domain_open_or_create(CS_TMP "/A.sym");
    struct ray_sym_domain_s* B = ray_sym_domain_open_or_create(CS_TMP "/B.sym");
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
    TEST_ASSERT_EQ_I(save_ctx(v, CS_TMP "/sa", 2, A), RAY_OK);
    ray_t* enc = ray_col_mmap_splayed_dom(CS_TMP "/sa", A);
    TEST_ASSERT_TRUE(enc && !RAY_IS_ERR(enc));
    TEST_ASSERT_EQ_I(save_ctx(enc, CS_TMP "/sb2", 2, B), RAY_OK);
    TEST_ASSERT_EQ_I(save_ctx(enc, CS_TMP "/sb0", 0, B), RAY_OK);
    const char* outs[2] = { CS_TMP "/sb2", CS_TMP "/sb0" };
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
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CS_TMP "/sc", 2), RAY_OK);
    ray_t* c = ray_col_mmap(CS_TMP "/sc");
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
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CS_TMP "/src", 2), RAY_OK);
    ray_release(v);
    FILE* f = fopen(CS_TMP "/src", "r+b"); fseek(f, 160 + 6, SEEK_SET); fputc(1, f); fclose(f);
    ray_t* enc = ray_col_mmap(CS_TMP "/src");
    TEST_ASSERT_EQ_I(save_ctx(enc, CS_TMP "/dst", 2, NULL), RAY_ERR_CORRUPT);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/dst"), 255);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/dst.tmp"), 255);
    TEST_ASSERT_EQ_I(save_ctx(enc, CS_TMP "/dst", 0, NULL), RAY_ERR_CORRUPT);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/dst"), 255);
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
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CS_TMP "/s", 2), RAY_OK);
    ray_release(v);
    ray_t* enc = ray_col_mmap(CS_TMP "/s");
    TEST_ASSERT_TRUE(enc && !RAY_IS_ERR(enc));
    ray_mem_stats_t m0, m1; ray_mem_stats(&m0);
    bool done = false;
    int injected = 0;
    for (int nth = 1; nth <= 8 && !done; nth++) {
        ray_col_test_fail_alloc(nth);
        ray_err_t r = save_ctx(enc, CS_TMP "/d", 2, NULL);
        ray_col_test_fail_alloc(0);
        if (r == RAY_OK) { done = true; break; }
        injected++;
        TEST_ASSERT_EQ_I(r, RAY_ERR_OOM);
        TEST_ASSERT_EQ_I(file_gen(CS_TMP "/d"), 255);
        TEST_ASSERT_EQ_I(file_gen(CS_TMP "/d.tmp"), 255);
        ray_mem_stats(&m1);
        TEST_ASSERT_FMT(m1.bytes_allocated == m0.bytes_allocated, "nth %d leaked", nth);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(injected >= 1);                        /* the writer's buffers were really failed */
    {   /* encoded -> 2 keeps a STR column byte for byte */
        size_t na, nb; uint8_t* a = slurp(CS_TMP "/s", &na); uint8_t* b = slurp(CS_TMP "/d", &nb);
        TEST_ASSERT_TRUE(a && b && na == nb && memcmp(a, b, na) == 0); free(a); free(b);
    }
    (void)remove(CS_TMP "/d");
    /* decode-side allocation failures, encoded -> 2 and encoded -> 0 */
    for (int g = 0; g <= 2; g += 2) {
        for (int nth = 1; nth <= 16; nth++) {
            ray_enccol_test_fail_alloc(nth);
            ray_err_t r = save_ctx(enc, CS_TMP "/g", (uint8_t)g, NULL);
            ray_enccol_test_fail_alloc(0);
            if (r == RAY_OK) { (void)remove(CS_TMP "/g"); break; }
            TEST_ASSERT_EQ_I(r, RAY_ERR_OOM);
            TEST_ASSERT_EQ_I(file_gen(CS_TMP "/g"), 255);
            TEST_ASSERT_EQ_I(file_gen(CS_TMP "/g.tmp"), 255);
            ray_mem_stats(&m1);
            TEST_ASSERT_TRUE(m1.bytes_allocated == m0.bytes_allocated);
        }
    }
    (void)remove(CS_TMP "/g");
    ray_mem_stats(&m0);                                     /* the successful encoded -> 0 cached its materialization */
    /* staging path unusable: the open fails, nothing appears */
    TEST_ASSERT_EQ_I(mkdir(CS_TMP "/e.tmp", 0700), 0);
    TEST_ASSERT_EQ_I(save_ctx(enc, CS_TMP "/e", 2, NULL), RAY_ERR_IO);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/e"), 255);
    (void)rmdir(CS_TMP "/e.tmp");
    /* I/O failure while writing: file size limit */
    struct rlimit old, lim;
    if (getrlimit(RLIMIT_FSIZE, &old) == 0) {
        lim = old; lim.rlim_cur = 65536;
        void (*oldsig)(int) = signal(SIGXFSZ, SIG_IGN);
        if (setrlimit(RLIMIT_FSIZE, &lim) == 0) {
            ray_err_t r = save_ctx(enc, CS_TMP "/f", 2, NULL);
            setrlimit(RLIMIT_FSIZE, &old);
            TEST_ASSERT_EQ_I(r, RAY_ERR_IO);
            TEST_ASSERT_EQ_I(file_gen(CS_TMP "/f"), 255);
            TEST_ASSERT_EQ_I(file_gen(CS_TMP "/f.tmp"), 255);
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
    struct ray_sym_domain_s* A = ray_sym_domain_open_or_create(CS_TMP "/A.sym");
    struct ray_sym_domain_s* C = ray_sym_domain_open_or_create(CS_TMP "/C.sym");
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
        TEST_ASSERT_EQ_I(save_ctx(vb, CS_TMP "/bad", (uint8_t)g, A), RAY_ERR_CORRUPT);
        TEST_ASSERT_EQ_I(file_gen(CS_TMP "/bad"), 255);
        TEST_ASSERT_EQ_I(file_gen(CS_TMP "/bad.tmp"), 255);
    }
    ray_release(vb);

    /* absent from the target domain: corrupt, encoded -> 0 leaves no staging file */
    int64_t ok[6] = { 1, 1, 2, 3, 3, 3 };                   /* sorted */
    ray_t* v = sym_vec_over(A, ok, 6);
    v->attrs |= RAY_ATTR_SORTED;
    TEST_ASSERT_EQ_I(save_ctx(v, CS_TMP "/pa", 2, A), RAY_OK);
    ray_t* enc = ray_col_mmap_splayed_dom(CS_TMP "/pa", A);
    TEST_ASSERT_TRUE(enc && !RAY_IS_ERR(enc));
    for (int g = 0; g <= 2; g += 2) {
        TEST_ASSERT_EQ_I(save_ctx(enc, CS_TMP "/miss", (uint8_t)g, C), RAY_ERR_CORRUPT);
        TEST_ASSERT_EQ_I(file_gen(CS_TMP "/miss"), 255);
        TEST_ASSERT_EQ_I(file_gen(CS_TMP "/miss.tmp"), 255);
    }
    /* same domain: byte identical to plain -> 2, SORTED kept, rc = count */
    TEST_ASSERT_EQ_I(save_ctx(enc, CS_TMP "/same", 2, A), RAY_OK);
    size_t na, nb; uint8_t* a = slurp(CS_TMP "/pa", &na); uint8_t* b = slurp(CS_TMP "/same", &nb);
    TEST_ASSERT_TRUE(a && b && na == nb && memcmp(a, b, na) == 0);
    uint32_t rc; memcpy(&rc, b + 20, 4);
    TEST_ASSERT_EQ_I(rc, 4); free(a); free(b);
    ray_t* r = ray_col_load_dom(CS_TMP "/same", A);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && (r->attrs & RAY_ATTR_SORTED));
    ray_release(r);
    /* another domain that has every symbol: SORTED dropped */
    struct ray_sym_domain_s* D = ray_sym_domain_open_or_create(CS_TMP "/D.sym");
    for (int i = 3; i >= 0; i--) ray_sym_domain_intern(D, w[i], strlen(w[i]));
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(D, false), RAY_OK);
    TEST_ASSERT_EQ_I(save_ctx(enc, CS_TMP "/other", 2, D), RAY_OK);
    r = ray_col_load_dom(CS_TMP "/other", D);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && !(r->attrs & RAY_ATTR_SORTED));
    ray_release(r);
    /* bare file from a FILE-domain source: process-local ids through the LUT */
    for (int g = 0; g <= 2; g += 2) {
        const char* o = g ? CS_TMP "/bare2" : CS_TMP "/bare0";
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
    TEST_ASSERT_EQ_I(ray_col_save_ctx(&cw, CS_TMP "/idx"), RAY_ERR_NYI);
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
        ray_t* t = ray_read_parted(CS_TMP "/pdb", "tb");
        TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
        ray_t* a = ray_table_col(t, ray_sym_intern("a", 1));
        TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a));
        ray_enc_counters_reset();
        for (int g = 0; g <= 2; g += 2) {
            TEST_ASSERT_EQ_I(save_ctx(a, CS_TMP "/pq", (uint8_t)g, NULL), RAY_ERR_NYI);
            TEST_ASSERT_EQ_I(file_gen(CS_TMP "/pq"), 255);
            TEST_ASSERT_EQ_I(file_gen(CS_TMP "/pq.tmp"), 255);
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
    const char* d = CS_TMP "/inh";
    ray_t* t = plain_tbl(3000);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, RAY_COMPRESS_ON), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 2);
    TEST_ASSERT_EQ_I(col_gen_in(d, "b"), 2);
    /* a fresh plain table, no option: the destination keeps generation 2 */
    TEST_ASSERT_EQ_I(ray_splay_save(t, d, NULL), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 2);
    /* the loaded (encoded) table into a new directory: the source rule */
    ray_t* l = ray_read_splayed(d, NULL);
    TEST_ASSERT_EQ_I(ray_splay_save(l, CS_TMP "/inh2", NULL), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/inh2", "b"), 2);
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
    const char* d = CS_TMP "/upd";
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
    TEST_ASSERT_EQ_I(ray_splay_save(l, CS_TMP "/upd2", NULL), RAY_OK);  /* new directory */
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/upd2", "a"), 0);
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/upd2", "b"), 2);
    /* the mixed directory keeps each column's generation on a plain resave */
    ray_release(l);
    l = plain_tbl(3000);
    TEST_ASSERT_EQ_I(ray_splay_save(l, CS_TMP "/upd2", NULL), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/upd2", "a"), 0);
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/upd2", "b"), 2);
    ray_release(l);
    PASS();
}

static test_result_t test_forced_generation0(void) {
    const char* d = CS_TMP "/forced";
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
        char dir[256]; snprintf(dir, sizeof dir, CS_TMP "/mp/2024.01.0%d/t", p + 1);
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
        snprintf(src, sizeof src, CS_TMP "/mp/2024.01.0%d/t", p + 1);
        snprintf(d2, sizeof d2, CS_TMP "/mp2/2024.01.0%d/t", p + 1);
        snprintf(d0, sizeof d0, CS_TMP "/mp0/2024.01.0%d/t", p + 1);
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
    ray_t* pt = ray_read_parted(CS_TMP "/mp2", "t");
    TEST_ASSERT_TRUE(pt && !RAY_IS_ERR(pt));
    TEST_ASSERT_EQ_I(ray_table_nrows(pt), 6000);
    ray_release(pt);
    PASS();
}

static test_result_t test_chunk_zone_recorded(void) {
    const char* d = CS_TMP "/cz";
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
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, CS_TMP "/sy", CS_TMP "/sy.sym", RAY_COMPRESS_ON), RAY_OK);
    ray_release(t);
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/sy", "s"), 2);
    ray_t* l = ray_read_splayed(CS_TMP "/sy", CS_TMP "/sy.sym");
    TEST_ASSERT_TRUE(l && !RAY_IS_ERR(l));
    /* private staging path, absent mode, encoded source: generation 2, into
     * another symfile.  Like the CSV/Parquet staging callers, the test holds
     * the target domain alive across the save and flushes it afterwards. */
    struct ray_sym_domain_s* d2 = ray_sym_domain_open_or_create(CS_TMP "/sy2.sym");
    TEST_ASSERT_NOT_NULL(d2);
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(ray_splay_save_staged_bulk_mode(l, CS_TMP "/sy2", CS_TMP "/sy2.sym", RAY_COMPRESS_ABSENT), RAY_OK);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);              /* merge and write by range */
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(d2, false), RAY_OK);
    ray_sym_domain_release(d2);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/sy2/s"), 2);
    ray_t* l2 = ray_read_splayed(CS_TMP "/sy2", CS_TMP "/sy2.sym");
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
        ray_t* pt = ray_read_parted(CS_TMP "/pdb", "tb");
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
                TEST_ASSERT_EQ_I(ray_splay_save_mode(t, CS_TMP "/pref", NULL, (ray_compress_t)m), RAY_ERR_NYI);
                TEST_ASSERT_FALSE(ray_splay_has_schema(CS_TMP "/pref"));
                TEST_ASSERT_EQ_I(access(CS_TMP "/pref/c", F_OK), -1);
            }
            ray_release(t);
        }
        ray_enc_counters_t k; ray_enc_counters_get(&k);
        TEST_ASSERT_EQ_U(k.materialized, 0);
        ray_release(pt);
    }
    /* a single encoded column passes preflight */
    ray_t* v = i64_col(3000, 3);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CS_TMP "/one", 2), RAY_OK);
    ray_release(v);
    ray_t* e = ray_col_mmap(CS_TMP "/one");
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
    (void)ray_test_mkdir_p(CS_TMP "/rd");
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CS_TMP "/rd/a", 2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CS_TMP "/rd/b", 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_file_gen(CS_TMP "/rd/a"), 2);
    TEST_ASSERT_EQ_I(ray_col_file_gen(CS_TMP "/rd/b"), 0);
    TEST_ASSERT_EQ_I(ray_col_file_gen(CS_TMP "/rd/missing"), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ABSENT, v, CS_TMP "/rd", "a", 1), 2);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ABSENT, v, CS_TMP "/rd", "b", 1), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_OFF, v, CS_TMP "/rd", "a", 1), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen_type(RAY_COMPRESS_ABSENT, RAY_I64, CS_TMP "/rd", "a", 1), 2);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen_type(RAY_COMPRESS_ABSENT, RAY_I64, CS_TMP "/rd", "b", 1), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen_type(RAY_COMPRESS_ON, RAY_LIST, NULL, "a", 1), 0);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen_type(RAY_COMPRESS_ON, RAY_I64, NULL, "a", 1), 2);
    ray_t* e = ray_col_mmap(CS_TMP "/rd/a");
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ABSENT, e, NULL, "x", 1), 2);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_OFF, e, NULL, "x", 1), 0);
    ray_release(e);
    ray_t* lnk = i64_col(10, 1);
    lnk->attrs |= RAY_ATTR_HAS_LINK; lnk->link_target = ray_sym_intern("other", 5);
    TEST_ASSERT_EQ_I(ray_col_resolve_gen(RAY_COMPRESS_ON, lnk, CS_TMP "/rd", "a", 1), 0);
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
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t1, CS_TMP "/m1", CS_TMP "/m1.sym", RAY_COMPRESS_ON), RAY_OK);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t2, CS_TMP "/m2", CS_TMP "/m2.sym", RAY_COMPRESS_ON), RAY_OK);
    ray_t* l1 = ray_read_splayed(CS_TMP "/m1", CS_TMP "/m1.sym");
    ray_t* l2 = ray_read_splayed(CS_TMP "/m2", CS_TMP "/m2.sym");
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
    struct ray_sym_domain_s* d3 = ray_sym_domain_open_or_create(CS_TMP "/m3.sym");
    TEST_ASSERT_NOT_NULL(d3);
    ray_enc_counters_reset();
    TEST_ASSERT_EQ_I(ray_splay_save_staged_bulk_mode(t, CS_TMP "/m3", CS_TMP "/m3.sym", RAY_COMPRESS_ABSENT), RAY_OK);
    ray_enc_counters_t k; ray_enc_counters_get(&k);
    TEST_ASSERT_EQ_U(k.materialized, 0);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(d3, false), RAY_OK);
    ray_sym_domain_release(d3);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/m3/a"), 2);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/m3/b"), 2);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/m3/c"), 0);
    ray_t* l3 = ray_read_splayed(CS_TMP "/m3", CS_TMP "/m3.sym");
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
    TEST_ASSERT_EQ_I(ray_splay_save(t, CS_TMP "/bi", CS_TMP "/bi.sym"), RAY_OK);
    /* the reference: today's per-column writers */
    (void)ray_test_mkdir_p(CS_TMP "/ref");
    struct ray_sym_domain_s* dom = ray_sym_domain_open_or_create(CS_TMP "/bi.sym");
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_EQ_I(ray_col_save_sym_encoded(ray_table_col_idx(t, 0), CS_TMP "/ref/s", dom, true), RAY_OK);
    ray_sym_domain_release(dom);
    TEST_ASSERT_EQ_I(ray_col_save(ray_table_col_idx(t, 1), CS_TMP "/ref/a"), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save(ray_table_col_idx(t, 2), CS_TMP "/ref/t"), RAY_OK);
    ray_t* names = ray_vec_new(RAY_STR, 3);
    names = ray_str_vec_append(names, "s", 1);
    names = ray_str_vec_append(names, "a", 1);
    names = ray_str_vec_append(names, "t", 1);
    TEST_ASSERT_EQ_I(ray_col_save(names, CS_TMP "/ref/.d"), RAY_OK);
    ray_release(names);
    const char* files[4] = { "s", "a", "t", ".d" };
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1)   /* again, onto the generation-0 destination */
            TEST_ASSERT_EQ_I(ray_splay_save(t, CS_TMP "/bi", CS_TMP "/bi.sym"), RAY_OK);
        char cur[1024], p[1200], r[256];
        TEST_ASSERT_EQ_I(ray_splay_resolve_dir(CS_TMP "/bi", cur, sizeof cur), RAY_OK);
        for (int f = 0; f < 4; f++) {
            snprintf(p, sizeof p, "%s/%s", cur, files[f]);
            snprintf(r, sizeof r, CS_TMP "/ref/%s", files[f]);
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
    const char* d = CS_TMP "/ur";
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
    TEST_ASSERT_EQ_I(ray_col_file_gen_err(CS_TMP "/nope", &g), RAY_OK);
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
    TEST_ASSERT_EQ_I(ray_splay_save(lt, CS_TMP "/ul", NULL), RAY_OK);
    ray_release(lt);
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(CS_TMP "/ul", cur, sizeof cur), RAY_OK);
    snprintf(p, sizeof p, "%s/a", cur);
    TEST_ASSERT_EQ_I(ray_col_file_gen_err(p, &g), RAY_OK);
    TEST_ASSERT_EQ_I(g, 0);
    ray_t* pt = ray_table_new(1);
    ray_t* v = i64_col(3, 1);
    pt = ray_table_add_col(pt, ray_sym_intern("a", 1), v);
    ray_release(v);
    TEST_ASSERT_EQ_I(ray_splay_save(pt, CS_TMP "/ul", NULL), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/ul", "a"), 0);
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

static test_result_t test_db_splayed_set_forms(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['a 'b] (list [1 2 3] [4 5 6])))"), "");
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f1\" t)"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/f1", "a"), 0);
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f2\" t \"" CS_TMP "/f2.sym\")"), "");
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f3\" t {compress: true})"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/f3", "a"), 2);
    /* absent over a generation-2 destination inherits it */
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f3\" t)"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/f3", "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f3\" t \"" CS_TMP "/f3.sym\" {compress: false})"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/f3", "a"), 0);
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f4\" t {compress: true} {compress: true})"), "domain");
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f4\" t {compress: 1})"), "domain");
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f4\" t {other: true})"), "domain");
    TEST_ASSERT_STR_EQ(ev_code("(sum (at (.db.splayed.get \"" CS_TMP "/f3\") 'b))"), "");
    /* round trip through a generation-2 destination */
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f5\" t {compress: true})"), "");
    ray_t* r = ray_eval_str("(sum (at (.db.splayed.get \"" CS_TMP "/f5\") 'b))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 15);
    ray_release(r);
    r = ray_eval_str("(all (== (at (.db.splayed.get \"" CS_TMP "/f5\") 'a) [1 2 3]))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == -RAY_BOOL && r->b8);
    ray_release(r);
    /* {} is absent; a null compress is a domain error */
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f6\" t {})"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/f6", "a"), 0);
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f6\" t {compress: null})"), "domain");
    /* an empty symfile path keeps its old meaning: no convention fallback */
    TEST_ASSERT_STR_EQ(ev_code("(set ts (table ['s] (list ['x 'y])))"), "");
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/f7\" ts \"\")"), "domain");
    PASS();
}

static test_result_t test_csv_parted_option(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['a 'b] (list [1 2 3 4] [10 20 30 40])))"), "");
    TEST_ASSERT_STR_EQ(ev_code("(.csv.write t \"" CS_TMP "/in.csv\")"), "");
    TEST_ASSERT_STR_EQ(ev_code("(.csv.parted {compress: true} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr\" 't)"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/pr/0/t", "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code("(.csv.parted \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr0\" 't)"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/pr0/0/t", "a"), 0);
    TEST_ASSERT_STR_EQ(ev_code("(.csv.parted {compress: true} {compress: true} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr1\" 't)"), "domain");
    TEST_ASSERT_STR_EQ(ev_code("(.csv.parted {zone: true} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr1\" 't)"), "domain");
    /* existing root: explicit modes and inheritance, typed + rows_per_part */
    TEST_ASSERT_STR_EQ(ev_code("(.csv.parted {compress: false} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr\" 't)"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/pr/0/t", "a"), 0);
    TEST_ASSERT_STR_EQ(ev_code("(.csv.parted {compress: true} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr\" 't)"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/pr/0/t", "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code("(.csv.parted \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr\" 't)"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/pr/0/t", "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code("(.csv.parted {compress: true} [I64 I64] \"" CS_TMP "/in.csv\" 2 \"" CS_TMP "/pt\" 't)"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/pt/1/t", "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code("(.csv.parted {compress: null} \"" CS_TMP "/in.csv\" \"" CS_TMP "/pr1\" 't)"), "domain");
    ray_t* r = ray_eval_str("(sum (at (.db.parted.get \"" CS_TMP "/pr\" 't) 'b))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 100);
    ray_release(r);
    PASS();
}

/* ---- Task 17: generation-2 stream writer; .csv.splayed and Parquet ---- */

/* Streaming the same I64 values in two appends gives the resident bytes. */
static test_result_t test_stream_matches_resident(void) {
    int64_t n = 140000;
    ray_t* v = i64_col(n, 13);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CS_TMP "/res", 2), RAY_OK);
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, CS_TMP, ray_sym_intern("str1", 4), RAY_I64, NULL, 2), RAY_OK);
    ray_t* p1 = ray_vec_new(RAY_I64, 70000); p1->len = 70000;
    ray_t* p2 = ray_vec_new(RAY_I64, n - 70000); p2->len = n - 70000;
    for (int64_t i = 0; i < 70000; i++) ((int64_t*)ray_data(p1))[i] = i * 13;
    for (int64_t i = 0; i < n - 70000; i++) ((int64_t*)ray_data(p2))[i] = (70000 + i) * 13;
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, p1), RAY_OK);   /* streams reject slices */
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, p2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    ray_col_stream_abort(&w);                                    /* always valid; nothing left */
    size_t na, nb; uint8_t* a = slurp(CS_TMP "/res", &na); uint8_t* b = slurp(CS_TMP "/str1", &nb);
    TEST_ASSERT_TRUE(a && b);
    TEST_ASSERT_TRUE(na == nb && memcmp(a, b, na) == 0);   /* no zone accumulator: identical */
    free(a); free(b);
    /* with the converters' zone accumulator the stream records CHUNK_ZONE,
     * as the generation-0 stream persists one */
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, CS_TMP, ray_sym_intern("str2", 4), RAY_I64, NULL, 2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_index_begin(&w, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, p1), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, p2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    ray_col_stream_abort(&w);
    b = slurp(CS_TMP "/str2", &nb);
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
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CS_TMP "/sres", 2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(v, CS_TMP "/sres0"), RAY_OK);
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, CS_TMP, ray_sym_intern("sst2", 4), RAY_STR, NULL, 2), RAY_OK);
    for (int k = 0; k < 3; k++) {
        ray_t* p = str_rows(cut[k], cut[k + 1]);
        TEST_ASSERT_EQ_I(ray_col_stream_append(&w, p), RAY_OK);
        ray_release(p);
    }
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    ray_col_stream_abort(&w);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/sst2"), 2);
    TEST_ASSERT_TRUE(same_file_from(CS_TMP "/sres", CS_TMP "/sst2", 0));
    /* generation 0, one append: the resident generation-0 bytes */
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, CS_TMP, ray_sym_intern("sst0", 4), RAY_STR, NULL, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, v), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    ray_col_stream_abort(&w);
    TEST_ASSERT_TRUE(same_file_from(CS_TMP "/sres0", CS_TMP "/sst0", 0));
    /* the generation-2 file loads back to the same cells */
    ray_t* back = ray_col_load(CS_TMP "/sst2");
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back) && back->len == n);
    for (int64_t i = 0; i < n; i += 997) {
        size_t la, lb; const char* a = ray_str_vec_get(v, i, &la); const char* b = ray_str_vec_get(back, i, &lb);
        TEST_ASSERT_TRUE(la == lb && (la == 0 || memcmp(a, b, la) == 0));
    }
    ray_release(back);
    /* I64 generation 0 streamed in two appends: the resident bytes */
    ray_t* iv = i64_col(140000, 13);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(iv, CS_TMP "/ires0"), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, CS_TMP, ray_sym_intern("ist0", 4), RAY_I64, NULL, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, iv), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    ray_col_stream_abort(&w);
    TEST_ASSERT_TRUE(same_file_from(CS_TMP "/ires0", CS_TMP "/ist0", 0));
    /* any other generation is refused before a file exists */
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, CS_TMP, ray_sym_intern("bad", 3), RAY_I64, NULL, 1), RAY_ERR_VERSION);
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
    ray_err_t e = ray_col_stream_open(&w, CS_TMP, ray_sym_intern(name, strlen(name)), RAY_SYM, d, gen);
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
    struct ray_sym_domain_s* d = ray_sym_domain_open_or_create(CS_TMP "/s.sym");
    TEST_ASSERT_TRUE(d != NULL);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(d, "", 0), 0);
    int64_t n = 140000;
    TEST_ASSERT_EQ_I(stream_sym(d, "sy0", 0, n), RAY_OK);
    TEST_ASSERT_EQ_I(stream_sym(d, "sy2", 2, n), RAY_OK);
    TEST_ASSERT_EQ_I(ray_sym_domain_flush(d, false), RAY_OK);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/sy0"), 0);
    TEST_ASSERT_EQ_I(file_gen(CS_TMP "/sy2"), 2);
    size_t nb; uint8_t* b = slurp(CS_TMP "/sy2", &nb);
    TEST_ASSERT_TRUE(b && nb >= 32);
    ray_t hdr; memcpy(&hdr, b, 32); free(b);
    TEST_ASSERT_EQ_I(hdr.type, RAY_SYM);
    TEST_ASSERT_EQ_I((int64_t)hdr.rc, ray_sym_domain_count(d));
    TEST_ASSERT_EQ_I(hdr.attrs & RAY_SYM_W_MASK, RAY_SYM_W32);
    TEST_ASSERT_TRUE(hdr.attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_EQ_I(hdr.len, n);
    ray_t* a = ray_col_load_dom(CS_TMP "/sy0", d);
    ray_t* c = ray_col_load_dom(CS_TMP "/sy2", d);
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
        if (ray_col_stream_open(&w, CS_TMP, ray_sym_intern("tf", 2), RAY_I64, NULL, 2) != RAY_OK) _exit(2);
        if (ray_col_stream_append(&w, v) != RAY_OK) _exit(2);
        ray_err_t e = ray_col_stream_close(&w, false);
        ray_col_stream_abort(&w);
        _exit(e == RAY_ERR_IO ? 0 : 1);
    }
    int status;
    TEST_ASSERT_EQ_I(waitpid(pid, &status, 0), pid);
    TEST_ASSERT_TRUE(WIFEXITED(status)); TEST_ASSERT_EQ_I(WEXITSTATUS(status), 0);
    TEST_ASSERT_TRUE(access(CS_TMP "/tf", F_OK) != 0);
    TEST_ASSERT_FALSE(cs_dir_has_stream_tmp(CS_TMP));
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
    TEST_ASSERT_EQ_I(ray_col_save(a, CS_TMP "/h1"), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save(b, CS_TMP "/h2"), RAY_OK);
    TEST_ASSERT_TRUE(same_file_from(CS_TMP "/h1", CS_TMP "/h2", 0));
    size_t n; uint8_t* p = slurp(CS_TMP "/h1", &n);
    TEST_ASSERT_TRUE(p && n >= 32);
    static const uint8_t zero[8];
    TEST_ASSERT_TRUE(memcmp(p + 8, zero, 8) == 0);
    free(p);
    ray_t* back = ray_col_load(CS_TMP "/h1");
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
    TEST_ASSERT_STR_EQ(ev_code("(.parquet.splayed {compress: true} {compress: true} \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/p2d\")"), "domain");
    TEST_ASSERT_STR_EQ(ev_code("(.parquet.parted {compress: true} {rows: 2} \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/p2p\" 't)"), "domain");
    PASS();
}

static test_result_t test_csv_splayed_option(void) {
    TEST_ASSERT_STR_EQ(ev_code("(set t (table ['a 's] (list [1 2 3 4] [\"alpha\" \"a much longer value\" \"\" \"x\"])))"), "");
    TEST_ASSERT_STR_EQ(ev_code("(.csv.write t \"" CS_TMP "/in.csv\")"), "");
    TEST_ASSERT_STR_EQ(ev_code("(.csv.splayed {compress: true} [I64 STR] \"" CS_TMP "/in.csv\" \"" CS_TMP "/cs\")"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/cs", "a"), 2);
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/cs", "s"), 2);
    ray_t* r = ray_eval_str("(at (.db.splayed.get \"" CS_TMP "/cs\") 's)");
    ray_t* want = ray_eval_str("[\"alpha\" \"a much longer value\" \"\" \"x\"]");
    ray_t* fa = ray_fmt(r, 0); ray_t* fb = ray_fmt(want, 0);
    TEST_ASSERT_TRUE(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0);
    ray_release(fa); ray_release(fb); ray_release(r); ray_release(want);
    /* absent over the generation-2 destination inherits it */
    TEST_ASSERT_STR_EQ(ev_code("(.csv.splayed [I64 STR] \"" CS_TMP "/in.csv\" \"" CS_TMP "/cs\")"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/cs", "a"), 2);
    TEST_ASSERT_STR_EQ(ev_code("(.csv.splayed {compress: true} {compress: true} \"" CS_TMP "/in.csv\" \"" CS_TMP "/cs2\")"), "domain");
    PASS();
}

/* .csv.splayed {compress: true} with a SYMBOL column (nulls, several
 * chunks) loads back equal to the generation-0 import. */
static test_result_t test_csv_splayed_symbol(void) {
    FILE* f = fopen(CS_TMP "/sy.csv", "w");
    TEST_ASSERT_TRUE(f != NULL);
    fprintf(f, "a,s\n");
    for (int64_t i = 0; i < 140000; i++) {
        if (i % 13 == 0) fprintf(f, "%lld,\n", (long long)i);
        else fprintf(f, "%lld,k%lld\n", (long long)i, (long long)(i % 400));
    }
    fclose(f);
    TEST_ASSERT_STR_EQ(ev_code("(.csv.splayed {compress: true} [I64 SYMBOL] \"" CS_TMP "/sy.csv\" \"" CS_TMP "/sy2\")"), "");
    TEST_ASSERT_STR_EQ(ev_code("(.csv.splayed {compress: false} [I64 SYMBOL] \"" CS_TMP "/sy.csv\" \"" CS_TMP "/sy0\")"), "");
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/sy2", "s"), 2);
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/sy2", "a"), 2);
    TEST_ASSERT_EQ_I(col_gen_in(CS_TMP "/sy0", "s"), 0);
    ray_t* x = ray_eval_str("(at (.db.splayed.get \"" CS_TMP "/sy2\") 's)");
    ray_t* y = ray_eval_str("(at (.db.splayed.get \"" CS_TMP "/sy0\") 's)");
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
    TEST_ASSERT_STR_EQ(ev_code("(.parquet.splayed {compress: true} \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/pq\")"), "");
    TEST_ASSERT_STR_EQ(ev_code("(.parquet.splayed \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/pq0\")"), "");
    ray_t* a = ray_eval_str("(.db.splayed.get \"" CS_TMP "/pq\")");
    ray_t* b = ray_eval_str("(.db.splayed.get \"" CS_TMP "/pq0\")");
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a) && b && !RAY_IS_ERR(b));
    for (int64_t c = 0; c < ray_table_ncols(a); c++) {
        ray_t* nm = ray_sym_str(ray_table_col_name(a, c));
        char cn[256]; snprintf(cn, sizeof cn, "%.*s", (int)ray_str_len(nm), ray_str_ptr(nm));
        ray_col_meta_t m; ray_table_col_meta_idx(a, c, &m);
        uint8_t want = (m.type >= RAY_BOOL && m.type <= RAY_STR) ? 2 : 0;
        TEST_ASSERT_FMT(col_gen_in(CS_TMP "/pq", cn) == want, "column %s", cn);
        TEST_ASSERT_FMT(col_gen_in(CS_TMP "/pq0", cn) == 0, "column %s (absent)", cn);
    }
    ray_t* fa = ray_fmt(a, 0); ray_t* fb = ray_fmt(b, 0);
    TEST_ASSERT_TRUE(ray_str_len(fa) == ray_str_len(fb) && memcmp(ray_str_ptr(fa), ray_str_ptr(fb), ray_str_len(fa)) == 0);
    ray_release(fa); ray_release(fb); ray_release(a); ray_release(b);
    TEST_ASSERT_STR_EQ(ev_code("(.parquet.parted {compress: true rows: 2} \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/pqp\" 't)"), "");
    TEST_ASSERT_STR_EQ(ev_code("(.parquet.splayed {compress: 1} \"test/data/parquet/flat-0-v1.parquet\" \"" CS_TMP "/pq2\")"), "domain");
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
    ray_err_t e = ray_splay_save_mode(t, CS_TMP "/plan", NULL, RAY_COMPRESS_ON);
    ray_release(t);
    if (e != RAY_OK) return ray_error("io", "save failed");
    return ray_read_splayed(CS_TMP "/plan", NULL);
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

static void write_null_part(const char* day, int gen, bool with_null) {
    char dir[256], p[300];
    snprintf(dir, sizeof dir, "%s/pdb/%s/tb", CS_TMP, day);
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
        (void)ray_test_rm_rf(CS_TMP "/pdb");
        write_null_part("2024.01.01", 2, pass == 0);
        write_null_part("2024.01.02", 2, false);
        ray_t* t = ray_read_parted(CS_TMP "/pdb", "tb");
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
    ray_err_t se = ray_splay_save_mode(t, CS_TMP "/bd", CS_TMP "/bd.sym", RAY_COMPRESS_ON);
    ray_release(t);
    TEST_ASSERT_EQ_I(se, RAY_OK);
    t = ray_read_splayed(CS_TMP "/bd", CS_TMP "/bd.sym");
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
    snprintf(d0, sizeof d0, "%s/%s0", CS_TMP, name); snprintf(s0, sizeof s0, "%s.sym", d0);
    snprintf(d2, sizeof d2, "%s/%s2", CS_TMP, name); snprintf(s2, sizeof s2, "%s.sym", d2);
    ray_t* t = fq_table();
    bool ok = ray_splay_save_mode(t, d0, s0, RAY_COMPRESS_OFF) == RAY_OK &&
              ray_splay_save_mode(t, d2, s2, RAY_COMPRESS_ON) == RAY_OK;
    ray_release(t);
    return ok;
}

static ray_t* fq_run(const char* name, int gen, const char* where, const char* tail) {
    char q[512];
    snprintf(q, sizeof q, "(select {i:i v:v d:d from: (.db.splayed.get \"%s/%s%d\" \"%s/%s%d.sym\") where: %s %s})",
             CS_TMP, name, gen, CS_TMP, name, gen, where, tail);
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
    ray_t* t0 = ray_read_splayed(CS_TMP "/fe0", CS_TMP "/fe0.sym");
    ray_t* t2 = ray_read_splayed(CS_TMP "/fe2", CS_TMP "/fe2.sym");
    TEST_ASSERT_TRUE(t0 && t2);
    ray_enc_counters_reset();
    for (size_t i = 0; i < sizeof w / sizeof *w; i++) {
        uint8_t* bits[2] = { ray_alloc_raw(FQ_N + 64), ray_alloc_raw(FQ_N + 64) };
        ray_t* tb[2] = { t0, t2 };
        int rc[2] = { 0, 0 };
        for (int g = 0; g < 2; g++) {
            ray_graph_t* gr = ray_graph_new(tb[g]);
            ray_t* e = ray_parse(w[i]);
            ray_op_t* dag = compile_expr_dag(gr, e);
            fp_pred_t p; ray_t* err = NULL;
            rc[g] = dag ? fp_compile_pred(gr, dag, tb[g], &p, &err) : -1;
            if (err) { ray_error_free(err); rc[g] = -2; }
            memset(bits[g], 0, FQ_N + 64);
            if (rc[g] == 0) for (int64_t st = 0; st < FQ_N; st += 1024) fp_eval_pred(&p, st, st + 1024 < FQ_N ? st + 1024 : FQ_N, bits[g] + st);
            if (rc[g] == 0) fp_pred_cleanup(&p);
            ray_release(e);
            ray_graph_free(gr);
        }
        bool same = rc[0] == 0 && rc[1] == 0 && memcmp(bits[0], bits[1], FQ_N) == 0;
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

/* A bind failure reaches the query as its error; a plain-request failure
 * reaches fp_compile_pred's error out-parameter; a decline stays a decline. */
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

    /* a failed materialization of a bound encoded column */
    ray_t* t2 = ray_read_splayed(CS_TMP "/ff2", CS_TMP "/ff2.sym");
    TEST_ASSERT_TRUE(t2 && !RAY_IS_ERR(t2));
    ray_graph_t* gr = ray_graph_new(t2);
    ray_op_t* dag = ray_gt(gr, ray_scan(gr, "v"), ray_const_i64(gr, 5));   /* no column lookup: nothing decoded yet */
    fp_pred_t p; ray_t* err = NULL;
    ray_enccol_test_fail_alloc(1);
    int rc = dag ? fp_compile_pred(gr, dag, t2, &p, &err) : 0;
    ray_enccol_test_fail_alloc(0);
    bool plain_err = rc != 0 && err && RAY_IS_ERR(err);
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
    TEST_ASSERT_TRUE(plain_err);
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
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/cp2\" t {compress: true})"), "");
    TEST_ASSERT_STR_EQ(ev_code("(.db.splayed.set \"" CS_TMP "/cp0\" t {compress: false})"), "");
    static const char* const q[] = {
        "(select {s: (sum (+ v w)) a: (avg (* 3 v)) from: (.db.splayed.get \"%s\") by: k})",
        "(select {from: (.db.splayed.get \"%s\") where: (> v 10) desc: v take: 5})",
        "(select {from: (.db.splayed.get \"%s\") where: (< w 100) take: 3})",
        NULL,
    };
    for (int i = 0; q[i]; i++) {
        char a2[512], a0[512];
        snprintf(a2, sizeof a2, q[i], CS_TMP "/cp2");
        snprintf(a0, sizeof a0, q[i], CS_TMP "/cp0");
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
        snprintf(a0, sizeof a0, qb[i], CS_TMP "/cp0");
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
    bool ok = ray_splay_save_mode(t, CS_TMP "/cp0g", CS_TMP "/cp0g.sym", RAY_COMPRESS_OFF) == RAY_OK &&
              ray_splay_save_mode(t, CS_TMP "/cp2g", CS_TMP "/cp2g.sym", RAY_COMPRESS_ON) == RAY_OK;
    ray_release(t);
    return ok;
}

static ray_t* cp_read(int gen) {
    return gen == 2 ? ray_read_splayed(CS_TMP "/cp2g", CS_TMP "/cp2g.sym")
                    : ray_read_splayed(CS_TMP "/cp0g", CS_TMP "/cp0g.sym");
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
        /* The DAG is built in a graph over a separate read of the files: building
         * it reads column types and decodes encoded columns, which would hide
         * what the planners themselves decode from the counters. */
        ray_t* tplan = cp_read(gen);
        TEST_ASSERT_TRUE(tplan && !RAY_IS_ERR(tplan));
        ray_graph_t* gr = ray_graph_new(tplan);
        ray_t *e1, *e2, *e3, *e4, *e5, *e6, *e7, *e8;
        ray_t* err = NULL;

        /* a decline decodes nothing: a nullable term, int x int, an unknown column */
        agg_linear_t lin; agg_prod_t prod; agg_affine_t aff; ray_t* vec = NULL;
        ray_op_t* d_null = cp_dag(gr, "(+ (* 2 b) v)", &e1);
        ray_op_t* d_ii   = cp_dag(gr, "(* a b)", &e2);
        ray_op_t* d_fv   = cp_dag(gr, "(* f v)", &e3);
        ray_op_t* d_lin = cp_dag(gr, "(+ (+ (* 2 b) a) 7)", &e4);
        ray_op_t* d_fg = cp_dag(gr, "(* f g)", &e5);
        ray_op_t* d_fa = cp_dag(gr, "(* f a)", &e6);
        ray_op_t* d_af = cp_dag(gr, "(- f 1.5)", &e7);
        ray_op_t* d_ai = cp_dag(gr, "(+ a 5)", &e8);
        TEST_ASSERT_TRUE(d_null && d_ii && d_fv && d_lin && d_fg && d_fa && d_af && d_ai);
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
        ray_release(tplan);
        ray_release(t);

        /* a failed bind is the planner's error, not a decline */
        t = cp_read(gen);
        tplan = cp_read(gen);
        gr = ray_graph_new(tplan);
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
        ray_release(tplan);
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
                src_set = ex.regs[r].src != NULL && ex.regs[r].src->obj != NULL &&
                          (ex.regs[r].src->obj->type == RAY_ENCCOL) == (gen == 2);
                if (ex.regs[r].nullable) nullable[gi][s] = true;
                if (!src_set) break;
                ray_t* plain = NULL;
                TEST_ASSERT_NULL(ray_col_src_plain(ex.regs[r].src, &plain));
                TEST_ASSERT_TRUE(ex.regs[r].data == ray_data(plain));
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
        ray_t* tplan = cp_read(gen);       /* the DAG decodes its own table, not t */
        gr = ray_graph_new(tplan);
        ray_op_t* d = cp_dag(gr, "(+ a b)", &e4);
        ray_graph_bind_test_fail_alloc(1);
        bool r = expr_compile(gr, t, d, &ex, &err);
        ray_graph_bind_test_fail_alloc(0);
        TEST_ASSERT_TRUE(!r && cp_is_bind_error(err));
        ray_error_free(err); err = NULL;
        if (gen == 2) {
            ray_enccol_test_fail_alloc(1);
            r = expr_compile(gr, t, d, &ex, &err);
            ray_enccol_test_fail_alloc(0);
            TEST_ASSERT_TRUE(!r && err && RAY_IS_ERR(err));
            ray_error_free(err); err = NULL;
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
        ray_release(tplan);
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
            ray_t* t = ray_read_splayed(gi ? CS_TMP "/tk2" : CS_TMP "/tk0", gi ? CS_TMP "/tk2.sym" : CS_TMP "/tk0.sym");
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
    ray_t* t = ray_read_splayed(CS_TMP "/tk2", CS_TMP "/tk2.sym");
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

const test_entry_t compress_entries[] = {
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
    { "compress/failed_decode_propagates",      test_failed_decode_propagates,       rt_setup, rt_teardown },
    { "compress/serde_byte_identity",           test_serde_byte_identity,            rt_setup, rt_teardown },
    { "compress/getter_keeps_pending_message",  test_getter_keeps_pending_message,   cs_setup, cs_teardown },
    { "compress/objsize_counts_cache",          test_objsize_counts_cache,           rt_setup, rt_teardown },
    { "compress/has_nulls_parted",              test_has_nulls_parted,               cs_setup, cs_teardown },
    { "compress/parted_gen2_loads",           test_parted_gen2_loads,            rt_setup, rt_teardown },
    { "compress/meta_without_decode",           test_meta_without_decode,            cs_setup, cs_teardown },
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
    { "compress/first_asc_parted_nulls_gen2", test_first_asc_parted_nulls_gen2, rt_setup, rt_teardown },
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
    { NULL, NULL, NULL, NULL },
};
