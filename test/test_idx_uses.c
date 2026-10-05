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

const test_entry_t idx_uses_entries[] = {
    { "idx_uses/counts_hash_lookup", test_idx_uses_counts_hash_lookup, iu_setup, iu_teardown },
    { "idx_uses/ignores_sorted_range", test_idx_uses_ignores_sorted_range, iu_setup, iu_teardown },
    { "idx_uses/rejects_non_table",  test_idx_uses_rejects_non_table,  iu_setup, iu_teardown },
    { NULL, NULL, NULL, NULL },
};
