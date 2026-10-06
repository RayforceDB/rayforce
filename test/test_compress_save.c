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
#include "store/serde.h"
#include "vec/str.h"
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
 * a stale error in the slot; only the failure leaves it set. */
static test_result_t test_legacy_getters(void) {
    ray_t* t = mixed_table();
    ray_t* stale;
    stale = ray_error("domain", "stale"); ray_error_free(stale);
    TEST_ASSERT_NULL(ray_table_get_col(t, ray_sym_intern("zz", 2)));
    TEST_ASSERT_NULL(ray_error_msg());
    stale = ray_error("domain", "stale"); ray_error_free(stale);
    ray_t* atom = ray_i64(3);
    TEST_ASSERT_NULL(ray_table_get_col(atom, 0));
    TEST_ASSERT_NULL(ray_error_msg());
    ray_release(atom);
    stale = ray_error("domain", "stale"); ray_error_free(stale);
    TEST_ASSERT_NOT_NULL(ray_table_get_col(t, ray_sym_intern("a", 1)));
    TEST_ASSERT_NULL(ray_error_msg());
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

/* The in-place insert, upsert and update arms write a table's slots as plain
 * vectors: a table loaded with encoded columns must take the copying arms. */
static test_result_t test_dml_on_encoded_table(void) {
    (void)ray_test_mkdir_p(CS_TMP "/dml");
    ray_t* k = i64_col(3000, 1); ray_t* v = i64_col(3000, 10);
    TEST_ASSERT_EQ_I(ray_col_save_gen(k, CS_TMP "/dml/k", 2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_gen(v, CS_TMP "/dml/v", 2), RAY_OK);
    ray_release(k); ray_release(v);
    ray_t* names = ray_vec_new(RAY_STR, 2);
    names = ray_str_vec_append(names, "k", 1); names = ray_str_vec_append(names, "v", 1);
    ray_col_save(names, CS_TMP "/dml/.d"); ray_release(names);
    ray_t* t = ray_read_splayed(CS_TMP "/dml", NULL);
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    TEST_ASSERT_TRUE(ray_table_get_col_enc(t, ray_sym_intern("k", 1)) != NULL);
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("t", 1), t), RAY_OK);
    ray_release(t);
    ray_t* r = ray_eval_str("(upsert 't 'k (list 5 -1))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(r);
    r = ray_env_get(ray_sym_intern("t", 1));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3000);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col(r, ray_sym_intern("v", 1))))[5], -1);
    r = ray_eval_str("(insert 't (list 9999 7))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(r);
    r = ray_env_get(ray_sym_intern("t", 1));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3001);
    r = ray_eval_str("(update {v: 1 from: 't where: (== k 7)})");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    ray_release(r);
    r = ray_env_get(ray_sym_intern("t", 1));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col(r, ray_sym_intern("v", 1))))[7], 1);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col(r, ray_sym_intern("v", 1))))[5], -1);
    PASS();
}

const test_entry_t compress_entries[] = {
    { "compress/dml_on_encoded_table",          test_dml_on_encoded_table,           rt_setup, rt_teardown },
    { "compress/meta_without_decode",           test_meta_without_decode,            cs_setup, cs_teardown },
    { "compress/legacy_getters",                test_legacy_getters,                 cs_setup, cs_teardown },
    { "compress/concurrent_one_materialization",test_concurrent_one_materialization, cs_setup, cs_teardown },
    { "compress/serde_encoded_table",           test_serde_encoded_table,            cs_setup, cs_teardown },
    { "compress/concurrent_col_index",          test_concurrent_col_index,           cs_setup, cs_teardown },
    { "compress/legacy_transient_no_leak",      test_legacy_transient_no_leak,       cs_setup, cs_teardown },
    { NULL, NULL, NULL, NULL },
};
