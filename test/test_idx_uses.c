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
#include "core/runtime.h"
#include "lang/eval.h"
#include "ops/idxop.h"
#include "lang/env.h"

static void iu_setup(void)    { ray_runtime_create(0, NULL); }
static void iu_teardown(void) { ray_runtime_destroy(__RUNTIME); }

static test_result_t test_idx_uses_counts_hash_lookup(void) {
    ray_t* s = ray_eval_str("(set t (table [k v] (list (til 100000) (til 100000))))");
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s)); ray_release(s);
    s = ray_eval_str("(set t (update {from: t k: (.idx.hash k)}))");
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s)); ray_release(s);
    s = ray_eval_str("(.idx.uses t)");                      /* reset */
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s)); ray_release(s);
    s = ray_eval_str("(select {from: t where: (== k 777)})");
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s)); ray_release(s);
    ray_t* r = ray_eval_str("(.idx.uses t)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_DICT);
    TEST_ASSERT_EQ_I(ray_dict_len(r), 1);
    ray_t* again = ray_eval_str("(.idx.uses t)");           /* reset happened */
    TEST_ASSERT_TRUE(again && !RAY_IS_ERR(again));
    TEST_ASSERT_EQ_I(ray_dict_len(again), 0);
    ray_release(r); ray_release(again);
    PASS();
}

/* A sorted column that also carries a HASH index, filtered by a range, is
 * answered by the sorted-range rowsel, which never reads col->index. */
static test_result_t test_idx_uses_ignores_sorted_range(void) {
    ray_t* s = ray_eval_str("(set t (table [k v] (list (til 300000) (til 300000))))");
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s)); ray_release(s);
    s = ray_eval_str("(set t (update {from: t k: (.idx.hash k)}))");
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s)); ray_release(s);
    ray_t* tt = ray_eval_str("t");
    TEST_ASSERT_TRUE(tt && !RAY_IS_ERR(tt));
    ray_t* k = ray_table_get_col(tt, ray_sym_intern("k", 1));
    if (k && (k->attrs & RAY_ATTR_HAS_INDEX)) k->attrs |= RAY_ATTR_SORTED;   /* til is ascending: verified by construction */
    bool ok = k && (k->attrs & RAY_ATTR_HAS_INDEX) && ray_attr_is_sorted(k);
    ray_release(tt);
    TEST_ASSERT_TRUE(ok);
    s = ray_eval_str("(.idx.uses t)");                      /* reset, arms counting */
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s)); ray_release(s);
    uint64_t hits = ray_idx_hits[IDX_SITE_FILTER_RANGE];
    s = ray_eval_str("(select {from: t where: (>= k 100000)})");
    TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s));
    TEST_ASSERT_EQ_I(ray_table_nrows(s), 200000); ray_release(s);
    TEST_ASSERT_TRUE(ray_idx_hits[IDX_SITE_FILTER_RANGE] > hits);   /* took the sorted-range path */
    ray_t* r = ray_eval_str("(.idx.uses t)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_dict_len(r), 0);
    ray_release(r);
    PASS();
}

static test_result_t test_idx_uses_rejects_non_table(void) {
    ray_t* r = ray_eval_str("(.idx.uses 5)");
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    ray_error_free(r);
    PASS();
}

/* A parted column wraps segments of `base`; the wrapper owns a ref per segment. */
static ray_t* iu_parted(int8_t base, ray_t** segs, int64_t n) {
    ray_t* p = ray_alloc((size_t)n * sizeof(ray_t*));
    if (!p) return NULL;
    p->type = RAY_PARTED_BASE + base;
    p->len = n;
    for (int64_t i = 0; i < n; i++) { ray_retain(segs[i]); ((ray_t**)ray_data(p))[i] = segs[i]; }
    return p;
}

static int64_t iu_eval_i64(const char* src) {
    ray_t* r = ray_eval_str(src);
    int64_t v = (r && !RAY_IS_ERR(r) && r->type == -RAY_I64) ? r->i64 : INT64_MIN;
    if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
    return v;
}

/* #1422: a where: over a parted column probes each segment's own hash.  A
 * segment whose hash was built for another length is stale: it is never
 * trusted, the scan answers its rows, and .idx.uses does not count it. */
static test_result_t test_idx_uses_parted_stale_segment(void) {
    const char* mk[] = {
        "(set s0 (.idx.hash (% (til 2000) 50)))",
        "(set s1 (.idx.hash (% (til 1500) 50)))",
        "(set s2 (% (til 1000) 50))",
        "(set v0 (til 2000))", "(set v1 (+ 2000 (til 1500)))", "(set v2 (+ 3500 (til 1000)))",
        "(set F (table [k v] (list (concat s0 (concat s1 s2)) (til 4500))))",
    };
    for (size_t i = 0; i < sizeof(mk) / sizeof(mk[0]); i++) {
        ray_t* s = ray_eval_str(mk[i]);
        TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s)); ray_release(s);
    }
    ray_t* ks[3] = { ray_eval_str("s0"), ray_eval_str("s1"), ray_eval_str("s2") };
    ray_t* vs[3] = { ray_eval_str("v0"), ray_eval_str("v1"), ray_eval_str("v2") };
    TEST_ASSERT_TRUE(ks[0]->index && ks[1]->index && !ks[2]->index);
    ray_t* pk = iu_parted(RAY_I64, ks, 3);
    ray_t* pv = iu_parted(RAY_I64, vs, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), pk);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), pv);
    ray_release(pk); ray_release(pv);
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("P", 1), tbl), RAY_OK);
    ray_release(tbl);

    const char* same = "(sum (== (at (select {from: P where: (== k 7)}) 'v) (at (select {from: F where: (== k 7)}) 'v)))";
    TEST_ASSERT_EQ_I(iu_eval_i64("(count (select {from: F where: (== k 7)}))"), 90);
    /* every index fresh: both indexed segments are probed */
    TEST_ASSERT_EQ_I(iu_eval_i64("(do (.idx.uses P) 0)"), 0);
    TEST_ASSERT_EQ_I(iu_eval_i64(same), 90);
    TEST_ASSERT_EQ_I(iu_eval_i64("(at (.idx.uses P) 'k)"), 2);
    /* the middle segment's hash goes stale: the same rows, one use */
    ray_index_t* ix = ray_index_payload(ks[1]->index);
    int64_t built = ix->built_for_len;
    ix->built_for_len = built - 1;
    TEST_ASSERT_EQ_I(iu_eval_i64(same), 90);
    TEST_ASSERT_EQ_I(iu_eval_i64("(at (.idx.uses P) 'k)"), 1);
    TEST_ASSERT_EQ_I(iu_eval_i64("(sum (== (at (select {from: P where: (in k [7 9 4999])}) 'v) (at (select {from: F where: (in k [7 9 4999])}) 'v)))"), 180);
    TEST_ASSERT_EQ_I(iu_eval_i64("(at (.idx.uses P) 'k)"), 1);
    ix->built_for_len = built;
    for (int i = 0; i < 3; i++) { ray_release(ks[i]); ray_release(vs[i]); }
    PASS();
}

const test_entry_t idx_uses_entries[] = {
    { "idx_uses/counts_hash_lookup", test_idx_uses_counts_hash_lookup, iu_setup, iu_teardown },
    { "idx_uses/ignores_sorted_range", test_idx_uses_ignores_sorted_range, iu_setup, iu_teardown },
    { "idx_uses/rejects_non_table",  test_idx_uses_rejects_non_table,  iu_setup, iu_teardown },
    { "idx_uses/parted_stale_segment", test_idx_uses_parted_stale_segment, iu_setup, iu_teardown },
    { NULL, NULL, NULL, NULL },
};
