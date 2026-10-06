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
#include "vec/str.h"
#include "ipc_harness.h"
#include "core/ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
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
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, d, NULL, RAY_COMPRESS_ON), RAY_OK);
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 2);
    /* the generation pointer itself unreadable: begin fails */
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(d, cur, sizeof cur), RAY_OK);
    snprintf(p, sizeof p, "%s/.current", d);
    TEST_ASSERT_EQ_I(chmod(p, 0), 0);
    TEST_ASSERT_TRUE(ray_splay_save(t, d, NULL) != RAY_OK);
    TEST_ASSERT_EQ_I(chmod(p, 0644), 0);
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(d, cur2, sizeof cur2), RAY_OK);
    TEST_ASSERT_TRUE(strcmp(cur2, cur) == 0);
    TEST_ASSERT_EQ_I(col_gen_in(d, "a"), 2);
    /* a short (truncated) destination column is corrupt, not 0 */
    snprintf(p, sizeof p, "%s/b", cur);
    TEST_ASSERT_EQ_I(truncate(p, 10), 0);
    TEST_ASSERT_EQ_I(ray_col_file_gen_err(p, &g), RAY_ERR_CORRUPT);
    TEST_ASSERT_EQ_I(ray_col_file_gen_err(CS_TMP "/nope", &g), RAY_OK);
    TEST_ASSERT_EQ_I(g, 0);
    TEST_ASSERT_EQ_I(ray_splay_save(t, d, NULL), RAY_ERR_CORRUPT);
    TEST_ASSERT_EQ_I(ray_splay_resolve_dir(d, cur2, sizeof cur2), RAY_OK);
    TEST_ASSERT_TRUE(strcmp(cur2, cur) == 0);
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

const test_entry_t compress_entries[] = {
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
    { NULL, NULL, NULL, NULL },
};
