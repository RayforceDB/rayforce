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

/* .db.parted.get refuses generation-2 partitions until P1 Task 12; a
 * generation-0 parted table is unchanged. */
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
static test_result_t test_parted_refuses_gen2(void) {
    write_part("2024.01.01", 0); write_part("2024.01.02", 0);
    ray_t* ok = ray_read_parted(CS_TMP "/pdb", "tb");
    TEST_ASSERT_TRUE(ok && !RAY_IS_ERR(ok));
    TEST_ASSERT_EQ_I(ray_table_ncols(ok), 2);
    ray_release(ok);
    write_part("2024.01.01", 2);
    ray_t* e = ray_read_parted(CS_TMP "/pdb", "tb");
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "nyi");
    ray_error_free(e);
    write_part("2024.01.01", 0); write_part("2024.01.02", 2);   /* a later partition too */
    e = ray_read_parted(CS_TMP "/pdb", "tb");
    TEST_ASSERT_TRUE(e && RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "nyi");
    ray_error_free(e);
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

const test_entry_t compress_entries[] = {
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
    { "compress/parted_refuses_gen2",           test_parted_refuses_gen2,            rt_setup, rt_teardown },
    { "compress/meta_without_decode",           test_meta_without_decode,            cs_setup, cs_teardown },
    { "compress/legacy_getters",                test_legacy_getters,                 cs_setup, cs_teardown },
    { "compress/concurrent_one_materialization",test_concurrent_one_materialization, cs_setup, cs_teardown },
    { "compress/serde_encoded_table",           test_serde_encoded_table,            cs_setup, cs_teardown },
    { "compress/concurrent_col_index",          test_concurrent_col_index,           cs_setup, cs_teardown },
    { "compress/legacy_transient_no_leak",      test_legacy_transient_no_leak,       cs_setup, cs_teardown },
    { NULL, NULL, NULL, NULL },
};
