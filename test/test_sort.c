/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/*
 * Focused unit tests for src/ops/sort.c coverage push (pass-7).
 *
 * Targets uncovered functions / regions:
 *   - ray_xrank_fn: first C-level call
 *   - sort_table_by_keys: list-of-sym-atoms path (is_list branch)
 *   - sort_table_by_keys: error paths (wrong type, missing column)
 *   - radix_decode_into: I32/I16/U8/desc and I64-desc via non-packed
 *     path (key_nbytes > 3, use_packed=false)
 *   - detect_sortedness parallel path (n > SMALL_POOL_THRESHOLD=8192,
 *     key_nbytes > 3 → use_packed=false)
 *   - xrank edge cases: n_groups=0, empty vec, non-numeric first arg
 *   - xasc/xdesc with list-of-sym-atoms key
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "table/sym.h"
#include "lang/internal.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ─── Helpers ────────────────────────────────────────────────────── */

/* Make a single sym-atom ray_t* (type=-RAY_SYM, i64=id).
 * Note: i64 and len share the same union slot; set i64 AFTER len. */
static ray_t* make_sym_atom(int64_t id) {
    ray_t* a = ray_alloc(0);
    if (!a) return NULL;
    a->type  = -RAY_SYM;
    a->attrs = 0;
    a->i64   = id;   /* Must be LAST: i64 aliases len in the union */
    return a;
}

/* ══════════════════════════════════════════════════════════════════
 * ray_xrank_fn tests (via lang/internal.h declaration)
 * ══════════════════════════════════════════════════════════════════ */

static test_result_t test_xrank_basic(void) {
    ray_heap_init();
    ray_sym_init();

    /* Build an I64 atom for n_groups */
    ray_t* n3 = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(n3);
    n3->type = -RAY_I64;
    n3->i64  = 3;

    /* Build a 9-element I64 vector: [9 3 6 1 7 2 8 4 5] */
    int64_t data[] = {9, 3, 6, 1, 7, 2, 8, 4, 5};
    ray_t* vec = ray_vec_from_raw(RAY_I64, data, 9);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_t* result = ray_xrank_fn(n3, vec);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), 9);

    /* Verify: sum of all group ids should be <= 3*(n-1) and groups in [0,2] */
    const int64_t* rd = (const int64_t*)ray_data(result);
    for (int64_t i = 0; i < 9; i++) {
        TEST_ASSERT_FMT(rd[i] >= 0 && rd[i] < 3,
                        "xrank group %lld out of range [0,3)", (long long)rd[i]);
    }

    ray_release(result);
    ray_release(vec);
    ray_release(n3);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_xrank_single_group(void) {
    ray_heap_init();
    ray_sym_init();

    ray_t* n1 = ray_alloc(0);
    n1->type = -RAY_I64; n1->i64 = 1;
    int64_t data[] = {5, 3, 1, 4, 2};
    ray_t* vec = ray_vec_from_raw(RAY_I64, data, 5);

    ray_t* result = ray_xrank_fn(n1, vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const int64_t* rd = (const int64_t*)ray_data(result);
    for (int64_t i = 0; i < 5; i++)
        TEST_ASSERT_EQ_I(rd[i], 0);

    ray_release(result);
    ray_release(vec);
    ray_release(n1);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_xrank_zero_groups(void) {
    ray_heap_init();
    ray_sym_init();

    ray_t* n0 = ray_alloc(0);
    n0->type = -RAY_I64; n0->i64 = 0;
    int64_t data[] = {1, 2, 3};
    ray_t* vec = ray_vec_from_raw(RAY_I64, data, 3);

    /* n_groups=0 → empty result */
    ray_t* result = ray_xrank_fn(n0, vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), 0);

    ray_release(result);
    ray_release(vec);
    ray_release(n0);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_xrank_empty_vec(void) {
    ray_heap_init();
    ray_sym_init();

    ray_t* n3 = ray_alloc(0);
    n3->type = -RAY_I64; n3->i64 = 3;
    ray_t* vec = ray_vec_new(RAY_I64, 0);
    vec->len = 0;

    ray_t* result = ray_xrank_fn(n3, vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), 0);

    ray_release(result);
    ray_release(vec);
    ray_release(n3);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_xrank_non_numeric_first_arg(void) {
    ray_heap_init();
    ray_sym_init();

    /* Pass a string atom as first arg → type error */
    int64_t col_id = ray_sym_intern("x", 1);
    ray_t* sym_atom = make_sym_atom(col_id);
    int64_t data[] = {1, 2, 3};
    ray_t* vec = ray_vec_from_raw(RAY_I64, data, 3);

    ray_t* result = ray_xrank_fn(sym_atom, vec);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    /* sym_atom is released by caller */
    ray_release(sym_atom);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_xrank_non_vec_second_arg(void) {
    ray_heap_init();
    ray_sym_init();

    ray_t* n3 = ray_alloc(0);
    n3->type = -RAY_I64; n3->i64 = 3;

    /* Pass an atom as second arg → type error */
    ray_t* atom = ray_alloc(0);
    atom->type = -RAY_I64; atom->i64 = 42;

    ray_t* result = ray_xrank_fn(n3, atom);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    ray_release(n3);
    ray_release(atom);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_xrank_f64(void) {
    ray_heap_init();
    ray_sym_init();

    ray_t* n2 = ray_alloc(0);
    n2->type = -RAY_I64; n2->i64 = 2;

    double data[] = {3.0, 1.0, 4.0, 1.0, 5.0};
    ray_t* vec = ray_vec_from_raw(RAY_F64, data, 5);

    ray_t* result = ray_xrank_fn(n2, vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), 5);

    /* groups should be in [0,1] */
    const int64_t* rd = (const int64_t*)ray_data(result);
    for (int64_t i = 0; i < 5; i++)
        TEST_ASSERT_FMT(rd[i] == 0 || rd[i] == 1,
                        "xrank f64 group %lld not 0 or 1", (long long)rd[i]);

    ray_release(result);
    ray_release(vec);
    ray_release(n2);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 * sort_table_by_keys: list-of-sym-atoms branch (is_list path)
 * ══════════════════════════════════════════════════════════════════ */

static test_result_t test_xasc_list_of_sym_atoms(void) {
    ray_heap_init();
    ray_sym_init();

    /* Build table: a=[3,1,2], b=[30,10,20] */
    int64_t name_a = ray_sym_intern("a", 1);
    int64_t name_b = ray_sym_intern("b", 1);

    int64_t adata[] = {3, 1, 2};
    int64_t bdata[] = {30, 10, 20};
    ray_t* acol = ray_vec_from_raw(RAY_I64, adata, 3);
    ray_t* bcol = ray_vec_from_raw(RAY_I64, bdata, 3);

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name_a, acol);
    tbl = ray_table_add_col(tbl, name_b, bcol);
    ray_release(acol); ray_release(bcol);

    /* Build a LIST of sym atoms: (list 'a) — passes through is_list branch */
    ray_t* sym_a = make_sym_atom(name_a);
    ray_t* keys_list = ray_list_new(1);
    keys_list = ray_list_append(keys_list, sym_a);
    /* sym_a is now retained by the list */
    ray_release(sym_a);

    ray_t* sorted = ray_xasc_fn(tbl, keys_list);
    TEST_ASSERT_NOT_NULL(sorted);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sorted));
    TEST_ASSERT_EQ_I(ray_table_nrows(sorted), 3);

    /* First row should have a=1 */
    ray_t* sorted_a = ray_table_get_col(sorted, name_a);
    TEST_ASSERT_NOT_NULL(sorted_a);
    const int64_t* sa = (const int64_t*)ray_data(sorted_a);
    TEST_ASSERT_EQ_I(sa[0], 1);
    TEST_ASSERT_EQ_I(sa[1], 2);
    TEST_ASSERT_EQ_I(sa[2], 3);

    ray_release(sorted);
    ray_release(keys_list);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_xdesc_list_of_sym_atoms(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t name_x = ray_sym_intern("x", 1);
    int32_t xdata[] = {1, 3, 2};
    ray_t* xcol = ray_vec_from_raw(RAY_I32, xdata, 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_x, xcol);
    ray_release(xcol);

    /* list-of-sym-atoms key for xdesc */
    ray_t* sym_x = make_sym_atom(name_x);
    ray_t* keys_list = ray_list_new(1);
    keys_list = ray_list_append(keys_list, sym_x);
    ray_release(sym_x);

    ray_t* sorted = ray_xdesc_fn(tbl, keys_list);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sorted));
    ray_t* sorted_x = ray_table_get_col(sorted, name_x);
    const int32_t* sx = (const int32_t*)ray_data(sorted_x);
    TEST_ASSERT_EQ_I(sx[0], 3);
    TEST_ASSERT_EQ_I(sx[1], 2);
    TEST_ASSERT_EQ_I(sx[2], 1);

    ray_release(sorted);
    ray_release(keys_list);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_xasc_list_non_sym_atom_error(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t name_a = ray_sym_intern("a", 1);
    int64_t adata[] = {1, 2, 3};
    ray_t* acol = ray_vec_from_raw(RAY_I64, adata, 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_a, acol);
    ray_release(acol);

    /* Build a list with a non-sym element to trigger type error in is_list path */
    ray_t* bad_elem = ray_alloc(0);
    bad_elem->type = -RAY_I64;
    bad_elem->i64  = 42;
    ray_t* keys_list = ray_list_new(1);
    keys_list = ray_list_append(keys_list, bad_elem);
    ray_release(bad_elem);

    ray_t* result = ray_xasc_fn(tbl, keys_list);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    ray_release(keys_list);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_xasc_wrong_key_type_error(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t name_a = ray_sym_intern("a", 1);
    int64_t adata[] = {1, 2, 3};
    ray_t* acol = ray_vec_from_raw(RAY_I64, adata, 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_a, acol);
    ray_release(acol);

    /* Pass an I64 atom as key — should trigger the else/error branch */
    ray_t* bad_key = ray_alloc(0);
    bad_key->type = -RAY_I64;
    bad_key->i64 = 42;

    ray_t* result = ray_xasc_fn(tbl, bad_key);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    ray_release(bad_key);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_xasc_missing_column_error(void) {
    ray_heap_init();
    ray_sym_init();

    /* Intern two syms; only add 'a' to the table, then sort by 'b'. */
    int64_t name_a = ray_sym_intern("sortcov_a", 9);
    int64_t name_b = ray_sym_intern("sortcov_b", 9);
    /* Verify they are different IDs */
    TEST_ASSERT_FMT(name_a != name_b,
                    "sym IDs must differ: a=%lld b=%lld",
                    (long long)name_a, (long long)name_b);

    int64_t adata[] = {1, 2, 3};
    ray_t* acol = ray_vec_from_raw(RAY_I64, adata, 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_a, acol);
    ray_release(acol);

    /* Sort by 'b which is not in the table → domain error */
    ray_t* sym_b = make_sym_atom(name_b);
    ray_t* result = ray_xasc_fn(tbl, sym_b);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    ray_release(sym_b);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_xasc_wrong_first_arg_type(void) {
    ray_heap_init();
    ray_sym_init();

    /* Pass non-table as first arg to xasc */
    int64_t data[] = {1, 2, 3};
    ray_t* vec = ray_vec_from_raw(RAY_I64, data, 3);
    int64_t name_a = ray_sym_intern("a", 1);
    ray_t* sym_a = make_sym_atom(name_a);

    ray_t* result = ray_xasc_fn(vec, sym_a);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    ray_release(sym_a);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 * radix_decode_into non-packed paths
 * (key_nbytes > 3 → use_packed=false → sorted_keys returned)
 * ══════════════════════════════════════════════════════════════════ */

/* I64 with large range forces key_nbytes=5+, non-packed, and
 * radix_decode_into for I64-desc. */
static test_result_t test_sort_i64_large_range_desc(void) {
    ray_heap_init();
    ray_sym_init();

    /* Create 8193 I64 values with spread > 2^32 to force key_nbytes=5 */
    int64_t n = 8193;
    ray_t* vec = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(vec);
    int64_t* d = (int64_t*)ray_data(vec);
    /* Alternating large and small values */
    int64_t base[] = {10000000000LL, 1LL, 5000000000LL, 2LL,
                      9999999999LL, 3LL, 7500000000LL, 4LL,
                      2500000000LL, 5LL};
    for (int64_t i = 0; i < n; i++)
        d[i] = base[i % 10];
    vec->len = n;

    uint8_t desc = 1;
    ray_t* result = ray_sort(&vec, &desc, NULL, 1, n);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), n);

    /* First element should be the largest */
    int64_t* res = (int64_t*)ray_data(result);
    TEST_ASSERT_FMT(res[0] >= res[1],
                    "desc sort: first %lld should >= second %lld",
                    (long long)res[0], (long long)res[1]);
    TEST_ASSERT_EQ_I(res[0], 10000000000LL);

    ray_release(result);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* I32 with large range (spread > 2^24) forces key_nbytes=4, non-packed. */
static test_result_t test_sort_i32_large_range_asc(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t n = 8193;
    ray_t* vec = ray_vec_new(RAY_I32, n);
    int32_t* d = (int32_t*)ray_data(vec);
    int32_t base[] = {20000000, 1, 10000000, 2, 19999999, 3, 15000000, 4, 5000000, 5};
    for (int64_t i = 0; i < n; i++)
        d[i] = base[i % 10];
    vec->len = n;

    uint8_t desc = 0;
    ray_t* result = ray_sort(&vec, &desc, NULL, 1, n);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), n);

    int32_t* res = (int32_t*)ray_data(result);
    TEST_ASSERT_EQ_I(res[0], 1);
    /* Verify sorted */
    for (int64_t i = 1; i < n; i++)
        TEST_ASSERT_FMT(res[i] >= res[i-1],
                        "asc sort broken at idx %lld: %d > %d",
                        (long long)i, (int)res[i-1], (int)res[i]);

    ray_release(result);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_sort_i32_large_range_desc(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t n = 8193;
    ray_t* vec = ray_vec_new(RAY_I32, n);
    int32_t* d = (int32_t*)ray_data(vec);
    int32_t base[] = {20000000, 1, 10000000, 2, 19999999, 3, 15000000, 4, 5000000, 5};
    for (int64_t i = 0; i < n; i++)
        d[i] = base[i % 10];
    vec->len = n;

    uint8_t desc = 1;
    ray_t* result = ray_sort(&vec, &desc, NULL, 1, n);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    int32_t* res = (int32_t*)ray_data(result);
    TEST_ASSERT_EQ_I(res[0], 20000000);
    for (int64_t i = 1; i < n; i++)
        TEST_ASSERT_FMT(res[i] <= res[i-1],
                        "desc sort broken at idx %lld", (long long)i);

    ray_release(result);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* I64 asc with large range, non-packed path for radix_decode_into I64-asc */
static test_result_t test_sort_i64_large_range_asc(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t n = 8193;
    ray_t* vec = ray_vec_new(RAY_I64, n);
    int64_t* d = (int64_t*)ray_data(vec);
    int64_t base[] = {10000000000LL, 1LL, 5000000000LL, 2LL,
                      9999999999LL, 3LL, 7500000000LL, 4LL,
                      2500000000LL, 5LL};
    for (int64_t i = 0; i < n; i++)
        d[i] = base[i % 10];
    vec->len = n;

    uint8_t desc = 0;
    ray_t* result = ray_sort(&vec, &desc, NULL, 1, n);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    int64_t* res = (int64_t*)ray_data(result);
    TEST_ASSERT_EQ_I(res[0], 1LL);
    for (int64_t i = 1; i < n; i++)
        TEST_ASSERT_FMT(res[i] >= res[i-1],
                        "asc sort broken at idx %lld", (long long)i);

    ray_release(result);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 * detect_sortedness parallel path
 * (n > 8192 AND key_nbytes > 3 → sk_pool → parallel sortedness)
 * ══════════════════════════════════════════════════════════════════ */

static test_result_t test_sort_i64_parallel_sortedness(void) {
    ray_heap_init();
    ray_sym_init();

    /* 8193 rows with large-range I64 → key_nbytes=5, use_packed=false,
     * detect_sortedness called with sk_pool (nrows >= 8192),
     * n > SMALL_POOL_THRESHOLD → parallel sortedness_fn branch */
    int64_t n = 8193;
    ray_t* vec = ray_vec_new(RAY_I64, n);
    int64_t* d = (int64_t*)ray_data(vec);
    /* Unsorted large values */
    for (int64_t i = 0; i < n; i++)
        d[i] = ((i * 1234567891LL + 987654321LL) % 100000000LL) * 100LL;
    vec->len = n;

    uint8_t desc = 0;
    ray_t* result = ray_sort_indices(&vec, &desc, NULL, 1, n);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), n);

    /* Verify indices are a valid permutation */
    const int64_t* idx = (const int64_t*)ray_data(result);
    /* First few should be ascending by original value */
    int64_t prev = d[idx[0]];
    for (int64_t i = 1; i < n; i++) {
        int64_t cur = d[idx[i]];
        TEST_ASSERT_FMT(cur >= prev, "sort permutation not ascending at %lld",
                        (long long)i);
        prev = cur;
    }

    ray_release(result);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 * ray_sort / ray_sort_indices edge cases
 * ══════════════════════════════════════════════════════════════════ */

static test_result_t test_sort_indices_zero_cols(void) {
    ray_heap_init();
    ray_sym_init();

    /* n_cols=0 → empty indices */
    ray_t* result = ray_sort_indices(NULL, NULL, NULL, 0, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), 0);

    ray_release(result);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_sort_indices_zero_rows(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t data[] = {3, 1, 2};
    ray_t* vec = ray_vec_from_raw(RAY_I64, data, 3);
    uint8_t desc = 0;

    /* nrows=0 → empty indices */
    ray_t* result = ray_sort_indices(&vec, &desc, NULL, 1, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), 0);

    ray_release(result);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_sort_indices_many_cols(void) {
    ray_heap_init();
    ray_sym_init();

    /* n_cols=17 (one past the fixed 16-key radix fast path):
     * sort_indices_ex has no n_cols limit of its own — the fixed
     * 16-key radix_encode_ctx_t fast path just gates itself off above 16
     * and falls through to the n_cols-unbounded comparator merge sort.
     * 16 constant columns (every row ties) + one discriminator column
     * last: ascending order is fully decided by the discriminator. */
    int64_t n = 2;
    ray_t* cols[17];
    int64_t zeros[2] = {0, 0};
    int64_t disc[2]  = {2, 1};
    for (int i = 0; i < 16; i++) {
        cols[i] = ray_vec_from_raw(RAY_I64, zeros, n);
        TEST_ASSERT_NOT_NULL(cols[i]);
    }
    cols[16] = ray_vec_from_raw(RAY_I64, disc, n);
    TEST_ASSERT_NOT_NULL(cols[16]);

    uint8_t descs[17];
    for (int i = 0; i < 17; i++) descs[i] = 0;

    ray_t* result = ray_sort_indices(cols, descs, NULL, 17, n);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), n);
    const int64_t* idx = (const int64_t*)ray_data(result);
    /* disc = [2, 1] ascending → row 1 (disc=1) before row 0 (disc=2) */
    TEST_ASSERT_EQ_I(idx[0], 1);
    TEST_ASSERT_EQ_I(idx[1], 0);

    ray_release(result);
    for (int i = 0; i < 17; i++) ray_release(cols[i]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 * xasc/xdesc multi-column list-of-sym-atoms
 * ══════════════════════════════════════════════════════════════════ */

static test_result_t test_xasc_two_sym_atoms_list(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t name_a = ray_sym_intern("a", 1);
    int64_t name_b = ray_sym_intern("b", 1);

    /* Table: 6 rows with clear (a,b) ordering */
    int64_t adata[] = {3, 1, 3, 1, 2, 2};
    int64_t bdata[] = {30, 10, 10, 20, 20, 10};
    ray_t* acol = ray_vec_from_raw(RAY_I64, adata, 6);
    ray_t* bcol = ray_vec_from_raw(RAY_I64, bdata, 6);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, name_a, acol);
    tbl = ray_table_add_col(tbl, name_b, bcol);
    ray_release(acol); ray_release(bcol);

    /* Build list ['a 'b] of sym atoms — exercises is_list branch */
    ray_t* sym_a = make_sym_atom(name_a);
    ray_t* sym_b = make_sym_atom(name_b);
    ray_t* keys_list = ray_list_new(2);
    keys_list = ray_list_append(keys_list, sym_a);
    keys_list = ray_list_append(keys_list, sym_b);
    ray_release(sym_a);
    ray_release(sym_b);

    ray_t* sorted = ray_xasc_fn(tbl, keys_list);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sorted));
    TEST_ASSERT_EQ_I(ray_table_nrows(sorted), 6);

    /* Sorted by (a,b) asc:
     * (1,10), (1,20), (2,10), (2,20), (3,10), (3,30) */
    ray_t* sorted_a = ray_table_get_col(sorted, name_a);
    ray_t* sorted_b = ray_table_get_col(sorted, name_b);
    const int64_t* sa = (const int64_t*)ray_data(sorted_a);
    const int64_t* sb = (const int64_t*)ray_data(sorted_b);

    /* Verify first row */
    TEST_ASSERT_EQ_I(sa[0], 1);
    TEST_ASSERT_EQ_I(sb[0], 10);
    /* Verify last row */
    TEST_ASSERT_EQ_I(sa[5], 3);

    /* Verify overall ordering: a is non-decreasing */
    for (int i = 1; i < 6; i++)
        TEST_ASSERT_FMT(sa[i] >= sa[i-1],
                        "xasc two-sym: a[%d]=%lld < a[%d]=%lld",
                        i, (long long)sa[i], i-1, (long long)sa[i-1]);

    ray_release(sorted);
    ray_release(keys_list);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 * ray_asc_fn / ray_desc_fn edge cases
 * ══════════════════════════════════════════════════════════════════ */

static test_result_t test_asc_atom_passthrough(void) {
    ray_heap_init();
    ray_sym_init();

    /* Atom input: should be returned as-is (retained) */
    ray_t* atom = ray_alloc(0);
    atom->type = -RAY_I64;
    atom->i64  = 42;
    ray_retain(atom);  /* retain before passing to asc */

    ray_t* result = ray_asc_fn(atom);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 42);

    ray_release(result);
    ray_release(atom);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_asc_single_element(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t data[] = {42};
    ray_t* vec = ray_vec_from_raw(RAY_I64, data, 1);

    ray_t* result = ray_asc_fn(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), 1);

    ray_release(result);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_asc_not_vec_error(void) {
    ray_heap_init();
    ray_sym_init();

    /* Pass non-vec/non-atom: a table → type error */
    int64_t name_a = ray_sym_intern("a", 1);
    int64_t data[] = {1, 2, 3};
    ray_t* col = ray_vec_from_raw(RAY_I64, data, 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name_a, col);
    ray_release(col);

    ray_t* result = ray_asc_fn(tbl);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 * Null-aware sort: ray_sort_indices with nulls
 * ══════════════════════════════════════════════════════════════════ */

static test_result_t test_sort_nulls_first(void) {
    ray_heap_init();
    ray_sym_init();

    /* Create a 5-element I64 vec with nulls at positions 1 and 3 */
    int64_t data[] = {3, 0, 1, 0, 2};
    ray_t* vec = ray_vec_from_raw(RAY_I64, data, 5);
    ray_vec_set_null(vec, 1, true);
    ray_vec_set_null(vec, 3, true);

    uint8_t desc = 0;
    uint8_t nf = 1;  /* nulls first */
    ray_t* idx = ray_sort_indices(&vec, &desc, &nf, 1, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(idx));

    const int64_t* idxd = (const int64_t*)ray_data(idx);
    /* First two positions should be null rows */
    TEST_ASSERT_TRUE(ray_vec_is_null(vec, idxd[0]));
    TEST_ASSERT_TRUE(ray_vec_is_null(vec, idxd[1]));
    /* Remaining should be ascending: 1, 2, 3 */
    TEST_ASSERT_EQ_I(data[idxd[2]], 1);
    TEST_ASSERT_EQ_I(data[idxd[3]], 2);
    TEST_ASSERT_EQ_I(data[idxd[4]], 3);

    ray_release(idx);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 * ray_sort multi-column path (n_cols > 1, lines 3104-3109)
 * ══════════════════════════════════════════════════════════════════ */

static test_result_t test_sort_multi_col(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t n = 12;
    /* col0: primary key [3,1,3,1,2,2,3,1,2,3,1,2] */
    int64_t d0[] = {3, 1, 3, 1, 2, 2, 3, 1, 2, 3, 1, 2};
    /* col1: secondary key [30,10,10,20,20,10,20,30,30,10,40,40] */
    int64_t d1[] = {30, 10, 10, 20, 20, 10, 20, 30, 30, 10, 40, 40};
    ray_t* col0 = ray_vec_from_raw(RAY_I64, d0, n);
    ray_t* col1 = ray_vec_from_raw(RAY_I64, d1, n);
    TEST_ASSERT_NOT_NULL(col0);
    TEST_ASSERT_NOT_NULL(col1);

    ray_t* cols[2] = { col0, col1 };
    uint8_t descs[2] = { 0, 0 };

    /* ray_sort with n_cols=2 → multi-column path (lines 3104-3109) */
    ray_t* result = ray_sort(cols, descs, NULL, 2, n);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* Result is the sorted col0 (primary key) */
    TEST_ASSERT_EQ_I(ray_len(result), n);

    const int64_t* rd = (const int64_t*)ray_data(result);
    /* Verify col0 values are non-decreasing */
    for (int64_t i = 1; i < n; i++)
        TEST_ASSERT_FMT(rd[i] >= rd[i-1],
                        "multi-col sort: col0[%lld]=%lld < col0[%lld]=%lld",
                        (long long)i, (long long)rd[i],
                        (long long)(i-1), (long long)rd[i-1]);

    ray_release(result);
    ray_release(col0);
    ray_release(col1);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 * Narrow-int single-key sort with nulls — regression for
 * radix_encode_fn (I16, BOOL, U8) which previously did not consult
 * the column's null bitmap and aliased nulls onto whatever the
 * underlying byte data happened to be.  Each test forces nrows above
 * the comparison-merge threshold (64) so we exercise the radix path,
 * and seeds non-null values around the encoded null sentinel so the
 * underlying byte data cannot mask the bug.
 * ══════════════════════════════════════════════════════════════════ */

/* Build an I16 vec with `data` and mark `null_pos` rows null. */
static ray_t* build_i16_with_nulls(const int16_t* data, int64_t n,
                                    const int64_t* null_pos, int64_t n_nulls) {
    ray_t* vec = ray_vec_from_raw(RAY_I16, data, n);
    for (int64_t i = 0; i < n_nulls; i++)
        ray_vec_set_null(vec, null_pos[i], true);
    return vec;
}

static test_result_t test_sort_i16_nulls_first_with_negatives(void) {
    ray_heap_init();
    ray_sym_init();

    /* 96 rows: span includes negatives so the encoded null sentinel
     * (0 ASC) cannot land "by accident" between negatives and positives.
     * Without the fix, nulls land where the underlying data byte (here
     * zeroed by the helper) happens to encode, i.e. between -1 and 0. */
    enum { N = 96 };
    int16_t data[N];
    for (int i = 0; i < N; i++) data[i] = (int16_t)(i - 48); /* -48..47 */
    int64_t null_pos[] = {3, 17, 50, 80};
    ray_t* vec = build_i16_with_nulls(data, N, null_pos, 4);

    uint8_t desc = 0, nf = 1;
    ray_t* idx = ray_sort_indices(&vec, &desc, &nf, 1, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(idx));
    const int64_t* idxd = (const int64_t*)ray_data(idx);

    /* First 4 must be null rows */
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_FMT(ray_vec_is_null(vec, idxd[i]),
                        "i16 nulls-first: position %d is not null", i);
    /* Remaining must be non-decreasing and non-null */
    for (int64_t i = 5; i < N; i++) {
        TEST_ASSERT_FALSE(ray_vec_is_null(vec, idxd[i]));
        TEST_ASSERT_FMT(data[idxd[i]] >= data[idxd[i-1]],
                        "i16 asc: out of order at %lld", (long long)i);
    }

    ray_release(idx);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_sort_i16_nulls_last_desc(void) {
    ray_heap_init();
    ray_sym_init();

    enum { N = 96 };
    int16_t data[N];
    for (int i = 0; i < N; i++) data[i] = (int16_t)(i - 48);
    int64_t null_pos[] = {1, 9, 60, 95};
    ray_t* vec = build_i16_with_nulls(data, N, null_pos, 4);

    uint8_t desc = 1, nf = 0;  /* DESC, nulls LAST */
    ray_t* idx = ray_sort_indices(&vec, &desc, &nf, 1, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(idx));
    const int64_t* idxd = (const int64_t*)ray_data(idx);

    /* Last 4 must be null rows */
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_FMT(ray_vec_is_null(vec, idxd[N - 1 - i]),
                        "i16 nulls-last: position %d from end is not null", i);
    /* Leading non-null prefix must be non-increasing */
    for (int64_t i = 1; i < N - 4; i++) {
        TEST_ASSERT_FALSE(ray_vec_is_null(vec, idxd[i]));
        TEST_ASSERT_FMT(data[idxd[i]] <= data[idxd[i-1]],
                        "i16 desc: out of order at %lld", (long long)i);
    }

    ray_release(idx);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_sort_i16_nulls_first_desc(void) {
    /* DESC × nulls-first: encoded null sentinel UINT64_MAX, must beat
     * even INT16_MIN at the leading edge. */
    ray_heap_init();
    ray_sym_init();

    enum { N = 96 };
    int16_t data[N];
    for (int i = 0; i < N; i++) data[i] = (int16_t)(i - 48);
    int64_t null_pos[] = {0, 47, 70};
    ray_t* vec = build_i16_with_nulls(data, N, null_pos, 3);

    uint8_t desc = 1, nf = 1;
    ray_t* idx = ray_sort_indices(&vec, &desc, &nf, 1, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(idx));
    const int64_t* idxd = (const int64_t*)ray_data(idx);

    for (int i = 0; i < 3; i++)
        TEST_ASSERT_TRUE(ray_vec_is_null(vec, idxd[i]));

    ray_release(idx);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_sort_u8_nulls_last_asc(void) {
    /* Post-Phase-1: U8 is non-nullable; ray_vec_set_null returns
     * RAY_ERR_TYPE.  Sort still works on non-null U8 columns. */
    ray_heap_init();
    ray_sym_init();

    ray_t* vec = ray_vec_new(RAY_U8, 4);
    uint8_t z = 0;
    for (int i = 0; i < 4; i++) vec = ray_vec_append(vec, &z);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vec, 1, true), RAY_ERR_TYPE);

    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_sort_u8_nulls_first_desc(void) {
    ray_heap_init();
    ray_sym_init();

    ray_t* vec = ray_vec_new(RAY_U8, 4);
    uint8_t z = 0;
    for (int i = 0; i < 4; i++) vec = ray_vec_append(vec, &z);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vec, 0, true), RAY_ERR_TYPE);

    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_sort_bool_nulls_first(void) {
    /* See test_sort_u8_nulls_last_asc — BOOL is non-nullable. */
    ray_heap_init();
    ray_sym_init();

    ray_t* vec = ray_vec_new(RAY_BOOL, 4);
    uint8_t b = 1;
    for (int i = 0; i < 4; i++) vec = ray_vec_append(vec, &b);
    TEST_ASSERT_EQ_I(ray_vec_set_null_checked(vec, 0, true), RAY_ERR_TYPE);

    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 * top / bot (partial top-N / bottom-N) — happy path
 *
 * Targets ray_top_fn / ray_bot_fn (sort.c:3448, 3453) which dispatch
 * through topk_take_vec → topk_indices_single → either the radix-
 * encoded heap path (numeric types) or topk_indices_cmp_single +
 * topk_indices_cmp + topk_cmp_sift_down (SYM type, sort.c:3173).
 *
 * Happy-path only: correct-type / correct-shape inputs.  Null /
 * wrong-type / K-edge cases are covered elsewhere (top_bot.rfl).
 * ══════════════════════════════════════════════════════════════════ */

/* (top vec K) over an I64 vec with K < N — exercises the numeric
 * radix-encoded bounded-heap path inside topk_indices_single. */
static test_result_t test_top_i64_k_lt_n(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t data[] = {3, 1, 5, 2, 7, 4, 9, 6, 8};
    ray_t* v = ray_vec_from_raw(RAY_I64, data, 9);
    TEST_ASSERT_NOT_NULL(v);

    ray_t* k = ray_i64(3);
    ray_t* res = ray_top_fn(v, k);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_len(res), 3);
    TEST_ASSERT_EQ_I(res->type, RAY_I64);

    /* Top 3 of {3,1,5,2,7,4,9,6,8} desc = {9,8,7}. */
    const int64_t* r = (const int64_t*)ray_data(res);
    TEST_ASSERT_EQ_I(r[0], 9);
    TEST_ASSERT_EQ_I(r[1], 8);
    TEST_ASSERT_EQ_I(r[2], 7);

    ray_release(res); ray_release(k); ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (top vec 1) — degenerate K=1 path: heap-of-one == max. */
static test_result_t test_top_i64_k_eq_one(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t data[] = {3, 1, 5, 2, 7, 4, 9, 6, 8};
    ray_t* v = ray_vec_from_raw(RAY_I64, data, 9);
    ray_t* k = ray_i64(1);
    ray_t* res = ray_top_fn(v, k);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_len(res), 1);
    TEST_ASSERT_EQ_I(((const int64_t*)ray_data(res))[0], 9);

    ray_release(res); ray_release(k); ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (bot vec K) — mirror path with desc=0; verifies bot's heap orientation. */
static test_result_t test_bot_i64_k_lt_n(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t data[] = {3, 1, 5, 2, 7, 4, 9, 6, 8};
    ray_t* v = ray_vec_from_raw(RAY_I64, data, 9);
    ray_t* k = ray_i64(3);
    ray_t* res = ray_bot_fn(v, k);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_len(res), 3);

    /* Bot 3 asc = {1,2,3}. */
    const int64_t* r = (const int64_t*)ray_data(res);
    TEST_ASSERT_EQ_I(r[0], 1);
    TEST_ASSERT_EQ_I(r[1], 2);
    TEST_ASSERT_EQ_I(r[2], 3);

    ray_release(res); ray_release(k); ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (top vec K) over F64 — exercises the F64 branch of the radix encode
 * inside the bounded-heap path. */
static test_result_t test_top_f64_k_lt_n(void) {
    ray_heap_init();
    ray_sym_init();

    double data[] = {1.5, 2.5, 0.5, 3.5, -1.0, 4.5, 2.0};
    ray_t* v = ray_vec_from_raw(RAY_F64, data, 7);
    ray_t* k = ray_i64(3);
    ray_t* res = ray_top_fn(v, k);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_len(res), 3);
    TEST_ASSERT_EQ_I(res->type, RAY_F64);

    /* Top 3 desc of {1.5,2.5,0.5,3.5,-1.0,4.5,2.0} = {4.5, 3.5, 2.5}. */
    const double* r = (const double*)ray_data(res);
    TEST_ASSERT_EQ_F(r[0], 4.5, 1e-9);
    TEST_ASSERT_EQ_F(r[1], 3.5, 1e-9);
    TEST_ASSERT_EQ_F(r[2], 2.5, 1e-9);

    ray_release(res); ray_release(k); ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (bot vec K) over F64 — F64 branch with desc=0. */
static test_result_t test_bot_f64_k_lt_n(void) {
    ray_heap_init();
    ray_sym_init();

    double data[] = {1.5, 2.5, 0.5, 3.5, -1.0, 4.5, 2.0};
    ray_t* v = ray_vec_from_raw(RAY_F64, data, 7);
    ray_t* k = ray_i64(2);
    ray_t* res = ray_bot_fn(v, k);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_len(res), 2);

    const double* r = (const double*)ray_data(res);
    TEST_ASSERT_EQ_F(r[0], -1.0, 1e-9);
    TEST_ASSERT_EQ_F(r[1], 0.5, 1e-9);

    ray_release(res); ray_release(k); ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (top vec K=N) — k>=len → falls through to ray_desc_fn (full sort),
 * which returns a lazy chain that must be materialized.  Exercises
 * the K==N short-circuit in topk_take_vec. */
static test_result_t test_top_i64_k_eq_n(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t data[] = {3, 1, 5, 2, 7};
    ray_t* v = ray_vec_from_raw(RAY_I64, data, 5);
    ray_t* k = ray_i64(5);
    ray_t* res = ray_top_fn(v, k);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    if (ray_is_lazy(res)) res = ray_lazy_materialize(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_len(res), 5);

    /* Full desc = {7,5,3,2,1}. */
    const int64_t* r = (const int64_t*)ray_data(res);
    TEST_ASSERT_EQ_I(r[0], 7);
    TEST_ASSERT_EQ_I(r[4], 1);
    for (int64_t i = 1; i < 5; i++)
        TEST_ASSERT_FMT(r[i] <= r[i-1],
                        "top k==n not desc-sorted at %lld", (long long)i);

    ray_release(res); ray_release(k); ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (bot vec K=N) mirror. */
static test_result_t test_bot_i64_k_eq_n(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t data[] = {3, 1, 5, 2, 7};
    ray_t* v = ray_vec_from_raw(RAY_I64, data, 5);
    ray_t* k = ray_i64(5);
    ray_t* res = ray_bot_fn(v, k);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    if (ray_is_lazy(res)) res = ray_lazy_materialize(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_len(res), 5);

    const int64_t* r = (const int64_t*)ray_data(res);
    TEST_ASSERT_EQ_I(r[0], 1);
    TEST_ASSERT_EQ_I(r[4], 7);

    ray_release(res); ray_release(k); ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (top symvec K) — RAY_SYM dispatches to topk_indices_cmp_single
 * (sort.c:3173), which calls topk_indices_cmp + topk_cmp_sift_down.
 * Exercises the comparator-heap branch of the top-K fast path that
 * the numeric radix encoding doesn't cover. */
static test_result_t test_top_sym_k_lt_n(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t s_apple  = ray_sym_intern("apple",  5);
    int64_t s_banana = ray_sym_intern("banana", 6);
    int64_t s_cherry = ray_sym_intern("cherry", 6);
    int64_t s_date   = ray_sym_intern("date",   4);
    int64_t s_elder  = ray_sym_intern("elder",  5);
    int64_t s_fig    = ray_sym_intern("fig",    3);

    /* SYM_W64 width: index slot is int64_t */
    int64_t N = 12;
    ray_t* sv = ray_sym_vec_new(RAY_SYM_W64, N);
    TEST_ASSERT_NOT_NULL(sv);
    sv->len = N;
    int64_t syms[6] = { s_apple, s_banana, s_cherry, s_date, s_elder, s_fig };
    int64_t* sd = (int64_t*)ray_data(sv);
    for (int64_t i = 0; i < N; i++) sd[i] = syms[i % 6];

    /* (top sv 3) → top 3 lex-desc symbols.  Lex order:
     *   apple < banana < cherry < date < elder < fig
     * Each symbol appears twice (N=12, 6 syms), so the desc top 3 must
     * draw from the {fig, fig, elder} multiset (two fig + one elder)
     * since fig and elder are the two highest symbols. */
    ray_t* k = ray_i64(3);
    ray_t* res = ray_top_fn(sv, k);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_len(res), 3);
    TEST_ASSERT_TRUE(RAY_IS_SYM(res->type));

    /* Read all three sym ids — the result is desc-sorted so r0 ≥ r1 ≥ r2
     * in lex order.  Expected (with stable tie-break): fig, fig, elder. */
    const int64_t r0 = ray_read_sym(ray_data(res), 0, res->type, res->attrs);
    const int64_t r1 = ray_read_sym(ray_data(res), 1, res->type, res->attrs);
    const int64_t r2 = ray_read_sym(ray_data(res), 2, res->type, res->attrs);
    TEST_ASSERT_EQ_I(r0, s_fig);
    TEST_ASSERT_EQ_I(r1, s_fig);
    TEST_ASSERT_EQ_I(r2, s_elder);

    ray_release(res); ray_release(k); ray_release(sv);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (bot symvec K) — mirror direction over SYM, exercising
 * topk_indices_cmp_single with desc=0. */
static test_result_t test_bot_sym_k_lt_n(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t s_apple  = ray_sym_intern("apple",  5);
    int64_t s_banana = ray_sym_intern("banana", 6);
    int64_t s_cherry = ray_sym_intern("cherry", 6);
    int64_t s_date   = ray_sym_intern("date",   4);
    int64_t s_elder  = ray_sym_intern("elder",  5);

    int64_t N = 10;
    ray_t* sv = ray_sym_vec_new(RAY_SYM_W64, N);
    sv->len = N;
    int64_t syms[5] = { s_apple, s_banana, s_cherry, s_date, s_elder };
    int64_t* sd = (int64_t*)ray_data(sv);
    for (int64_t i = 0; i < N; i++) sd[i] = syms[i % 5];

    ray_t* k = ray_i64(2);
    ray_t* res = ray_bot_fn(sv, k);
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_len(res), 2);

    /* Bot 2 asc = {apple, apple}: 'apple' appears at rows 0 and 5. */
    const int64_t r0 = ray_read_sym(ray_data(res), 0, res->type, res->attrs);
    const int64_t r1 = ray_read_sym(ray_data(res), 1, res->type, res->attrs);
    TEST_ASSERT_EQ_I(r0, s_apple);
    TEST_ASSERT_EQ_I(r1, s_apple);

    ray_release(res); ray_release(k); ray_release(sv);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (top symvec K=N) — K==N → falls through to ray_desc_fn (full sort
 * over SYM), still a happy-path traverse. */
static test_result_t test_top_sym_k_eq_n(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t s_a = ray_sym_intern("aa", 2);
    int64_t s_b = ray_sym_intern("bb", 2);
    int64_t s_c = ray_sym_intern("cc", 2);

    int64_t N = 3;
    ray_t* sv = ray_sym_vec_new(RAY_SYM_W64, N);
    sv->len = N;
    int64_t* sd = (int64_t*)ray_data(sv);
    sd[0] = s_b; sd[1] = s_a; sd[2] = s_c;

    ray_t* k = ray_i64(3);
    ray_t* res = ray_top_fn(sv, k);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    if (ray_is_lazy(res)) res = ray_lazy_materialize(res);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res));
    TEST_ASSERT_EQ_I(ray_len(res), 3);
    /* desc lex: cc, bb, aa */
    TEST_ASSERT_EQ_I(ray_read_sym(ray_data(res), 0, res->type, res->attrs), s_c);
    TEST_ASSERT_EQ_I(ray_read_sym(ray_data(res), 1, res->type, res->attrs), s_b);
    TEST_ASSERT_EQ_I(ray_read_sym(ray_data(res), 2, res->type, res->attrs), s_a);

    ray_release(res); ray_release(k); ray_release(sv);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 * MSD bucket sort — msd_radix_sort_run dispatches to
 * msd_bucket_sort_fn + bucket_lsb_sort only when both
 *   nrows  > 1,000,000   AND
 *   key_nbytes > 5       (range needs ≥6 bytes)
 * apply (sort.c:810).  Build a 1.1M-row I64 vec with a 56-bit value
 * range that ensures compute_key_nbytes returns ≥6, so we drop into
 * the MSD path with 256 buckets and per-bucket LSB radix.
 * ══════════════════════════════════════════════════════════════════ */

static test_result_t test_sort_msd_bucket_i64(void) {
    ray_heap_init();
    ray_sym_init();

    /* Just over 1M rows so we exceed the `n > 1000000` gate. */
    const int64_t N = 1000001;
    ray_t* vec = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_NOT_NULL(vec);
    int64_t* d = (int64_t*)ray_data(vec);

    /* Spread values across ~2^56 so the encoded key_nbytes is 7,
     * tripping the n_bytes > 5 gate.  Use a simple deterministic
     * pseudo-random pattern that's neither sorted nor reverse-sorted. */
    const int64_t big = (int64_t)1 << 56;
    for (int64_t i = 0; i < N; i++) {
        /* Mix bits in the upper 7 bytes so every key_nbytes byte is
         * non-uniform → no MSD-uniform fallback. */
        uint64_t m = (uint64_t)(i * 2654435761ULL);
        d[i] = (int64_t)(m % (uint64_t)big);
    }
    vec->len = N;

    uint8_t desc = 0;
    ray_t* result = ray_sort(&vec, &desc, NULL, 1, N);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), N);

    /* Verify ascending order at sparse checkpoints; full O(n) scan is
     * also fine but costly.  Walk every 137th element (coprime with
     * 64 / 128 / 256) so we land on every bucket boundary class. */
    const int64_t* r = (const int64_t*)ray_data(result);
    int64_t prev = r[0];
    for (int64_t i = 137; i < N; i += 137) {
        TEST_ASSERT_FMT(r[i] >= prev,
                        "msd asc out of order at %lld: %lld < %lld",
                        (long long)i, (long long)r[i], (long long)prev);
        prev = r[i];
    }
    /* Sanity: adjacent pairs at start and end. */
    TEST_ASSERT_TRUE(r[1] >= r[0]);
    TEST_ASSERT_TRUE(r[N-1] >= r[N-2]);

    ray_release(result);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* MSD bucket sort, descending — same path, exercises the desc branch
 * of radix_encode_fn that feeds the bucketed sort.  Smaller checks
 * keep runtime moderate. */
static test_result_t test_sort_msd_bucket_i64_desc(void) {
    ray_heap_init();
    ray_sym_init();

    const int64_t N = 1000001;
    ray_t* vec = ray_vec_new(RAY_I64, N);
    int64_t* d = (int64_t*)ray_data(vec);

    /* Same big spread as asc test, different seed. */
    const int64_t big = (int64_t)1 << 56;
    for (int64_t i = 0; i < N; i++) {
        uint64_t m = (uint64_t)((i + 17) * 2246822519ULL);
        d[i] = (int64_t)(m % (uint64_t)big);
    }
    vec->len = N;

    uint8_t desc = 1;
    ray_t* result = ray_sort(&vec, &desc, NULL, 1, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), N);

    const int64_t* r = (const int64_t*)ray_data(result);
    int64_t prev = r[0];
    for (int64_t i = 211; i < N; i += 211) {
        TEST_ASSERT_FMT(r[i] <= prev,
                        "msd desc out of order at %lld: %lld > %lld",
                        (long long)i, (long long)r[i], (long long)prev);
        prev = r[i];
    }

    ray_release(result);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* gather_by_idx W8 SYM path (eval.c line 1146, case 1)
 * A RAY_SYM_W8 vec (IDs stored as uint8_t) sorted via ray_sort forces
 * the SYM branch of gather_by_idx to take the esz==1 / case-1 path.
 * A fresh sym table keeps all IDs ≤ 3 so ray_sym_dict_width returns W8. */
static test_result_t test_sort_sym_w8_gather(void) {
    ray_heap_init();
    ray_sym_init();

    int64_t s_a = ray_sym_intern("ga", 2);
    int64_t s_b = ray_sym_intern("gb", 2);
    int64_t s_c = ray_sym_intern("gc", 2);

    /* IDs 1, 2, 3 — all ≤ 255 → W8 storage */
    const int64_t N = 6;
    ray_t* sv = ray_sym_vec_new(RAY_SYM_W8, N);
    TEST_ASSERT_NOT_NULL(sv);
    sv->len = N;
    uint8_t* d = (uint8_t*)ray_data(sv);
    /* Unsorted: c, a, b, c, a, b */
    d[0] = (uint8_t)s_c;
    d[1] = (uint8_t)s_a;
    d[2] = (uint8_t)s_b;
    d[3] = (uint8_t)s_c;
    d[4] = (uint8_t)s_a;
    d[5] = (uint8_t)s_b;

    uint8_t asc_flag = 0;
    ray_t* result = ray_sort(&sv, &asc_flag, NULL, 1, N);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_len(result), N);
    TEST_ASSERT_TRUE(RAY_IS_SYM(result->type));

    /* Sorted asc: a, a, b, b, c, c */
    const uint8_t* r = (const uint8_t*)ray_data(result);
    TEST_ASSERT_EQ_I(r[0], (uint8_t)s_a);
    TEST_ASSERT_EQ_I(r[1], (uint8_t)s_a);
    TEST_ASSERT_EQ_I(r[2], (uint8_t)s_b);
    TEST_ASSERT_EQ_I(r[3], (uint8_t)s_b);
    TEST_ASSERT_EQ_I(r[4], (uint8_t)s_c);
    TEST_ASSERT_EQ_I(r[5], (uint8_t)s_c);

    ray_release(result);
    ray_release(sv);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Both sides of the comparison budget must retain the typed SYM-first
 * route. Signed secondary values and a late secondary inversion exercise
 * its raw-i64 compare and the borrowed-rank path after many symbol runs. */
static test_result_t test_sorted_sym_i64_probe(void) {
    ray_heap_init(); ray_sym_init();
    int64_t* ids = calloc(2048, sizeof(*ids));
    TEST_ASSERT_NOT_NULL(ids);
    for (int i = 2047; i >= 0; i--) {
        char name[32]; int len = snprintf(name, sizeof(name), "typed-%04d", i);
        ids[i] = ray_sym_intern(name, (size_t)len);
    }
    int64_t sn = ray_sym_intern("s", 1), tn = ray_sym_intern("t", 1);
    int64_t rn = ray_sym_intern("row", 3);
    int64_t key_ids[] = {sn, tn};
    ray_t* keys = ray_vec_from_raw(RAY_SYM, key_ids, 2);
    const int8_t types[] = {RAY_I64, RAY_TIMESTAMP};
    for (int type = 0; type < 2; type++) {
        for (int busy = 0; busy < 2; busy++) {
            int64_t runs = busy ? 2048 : 100, run_rows = busy ? 100 : 2000;
            int64_t n = runs * run_rows;
            for (int inversion = 0; inversion < 2; inversion++) {
                ray_t* sym = ray_vec_new(RAY_SYM, n);
                ray_t* time = ray_vec_new(types[type], n);
                ray_t* row = ray_vec_new(RAY_I64, n);
                sym->len = time->len = row->len = n;
                for (int64_t i = 0; i < n; i++) {
                    ((int64_t*)ray_data(sym))[i] = ids[i / run_rows];
                    ((int64_t*)ray_data(time))[i] = (i % run_rows - run_rows / 2) * 1000000;
                    ((int64_t*)ray_data(row))[i] = i;
                }
                if (inversion) ((int64_t*)ray_data(time))[n - 1] = ((int64_t*)ray_data(time))[n - 2] - 1;
                ray_t* table = ray_table_new(3);
                table = ray_table_add_col(table, sn, sym);
                table = ray_table_add_col(table, tn, time);
                table = ray_table_add_col(table, rn, row);
                ray_t* result = ray_xasc_fn(table, keys);
                TEST_ASSERT_NOT_NULL(result);
                TEST_ASSERT_FALSE(RAY_IS_ERR(result));
                TEST_ASSERT_EQ_I(ray_table_nrows(result), n);
                ray_t* os = ray_table_get_col(result, sn);
                ray_t* ot = ray_table_get_col(result, tn);
                ray_t* orow = ray_table_get_col(result, rn);
                for (int64_t i = 0; i < n; i++) {
                    int64_t r = inversion && i >= n - 2 ? (i == n - 2 ? n - 1 : n - 2) : i;
                    TEST_ASSERT_EQ_I(((int64_t*)ray_data(orow))[i], r);
                    TEST_ASSERT_EQ_I(ray_read_sym(ray_data(os), i, os->type, os->attrs), ids[r / run_rows]);
                    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ot))[i], ((int64_t*)ray_data(time))[r]);
                }
                ray_release(result); ray_release(table);
                ray_release(sym); ray_release(time); ray_release(row);
            }
        }
    }
    free(ids); ray_release(keys);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* A large late-used constant/low-transition key must finish the probe,
 * rather than resolving an entire intern prefix merely for passing 64K. */
static test_result_t test_sorted_large_constant_symbols(void) {
    ray_heap_init(); ray_sym_init();
    for (int i = 0; i < 70000; i++) {
        char name[40]; int len = snprintf(name, sizeof(name), "constant-padding-%d", i);
        ray_sym_intern(name, (size_t)len);
    }
    int64_t z = ray_sym_intern("constant-z", 10);
    int64_t a = ray_sym_intern("constant-a", 10);
    TEST_ASSERT_TRUE(a > 65536);
    int64_t sn = ray_sym_intern("s", 1), tn = ray_sym_intern("t", 1);
    int64_t rn = ray_sym_intern("row", 3);
    int64_t key_ids[] = {tn, sn};
    ray_t* one_key = make_sym_atom(sn);
    ray_t* two_keys = ray_vec_from_raw(RAY_SYM, key_ids, 2);
    const int64_t n = RAY_PARALLEL_THRESHOLD + 1;
    for (int desc = 0; desc < 2; desc++) {
        for (int kind = 0; kind < 3; kind++) {
            ray_t* sym = ray_vec_new(RAY_SYM, n);
            ray_t* time = ray_vec_new(RAY_I64, n);
            ray_t* row = ray_vec_new(RAY_I64, n);
            sym->len = time->len = row->len = n;
            for (int64_t i = 0; i < n; i++) {
                bool first = kind == 2 ? i % 2 == 0 : i < n / 2;
                ((int64_t*)ray_data(sym))[i] = kind == 0 ? a : (first ? (desc ? z : a) : (desc ? a : z));
                ((int64_t*)ray_data(time))[i] = desc ? n - 1 - i : i;
                ((int64_t*)ray_data(row))[i] = i;
            }
            ray_t* table = ray_table_new(3);
            table = ray_table_add_col(table, sn, sym);
            table = ray_table_add_col(table, tn, time);
            table = ray_table_add_col(table, rn, row);
            ray_t* keys = kind == 2 ? two_keys : one_key;
            ray_t* result = desc ? ray_xdesc_fn(table, keys) : ray_xasc_fn(table, keys);
            TEST_ASSERT_NOT_NULL(result);
            TEST_ASSERT_FALSE(RAY_IS_ERR(result));
            if (kind != 2) TEST_ASSERT_TRUE(result == table);
            TEST_ASSERT_EQ_I(ray_table_nrows(result), n);
            ray_t* result_rows = ray_table_get_col(result, rn);
            ray_t* result_syms = ray_table_get_col(result, sn);
            for (int64_t i = 0; i < n; i++) {
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(result_rows))[i], i);
                TEST_ASSERT_EQ_I(ray_read_sym(ray_data(result_syms), i, result_syms->type, result_syms->attrs), ((int64_t*)ray_data(sym))[i]);
            }
            ray_release(result); ray_release(table);
            ray_release(sym); ray_release(time); ray_release(row);
        }
    }
    ray_release(one_key); ray_release(two_keys);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* The query/partition helper is also callable with an unbounded key
 * count. All keys tie, forcing it to visit the entire heap-backed list. */
static test_result_t test_sorted_many_repeated_keys(void) {
    ray_heap_init(); ray_sym_init();
    const int64_t n_keys = 300000;
    int64_t id = ray_sym_intern("repeated-key", 12);
    int64_t values[] = {id, id};
    ray_t* sym = ray_vec_from_raw(RAY_SYM, values, 2);
    ray_t** cols = calloc((size_t)n_keys, sizeof(*cols));
    TEST_ASSERT_NOT_NULL(cols);
    for (int64_t k = 0; k < n_keys; k++) cols[k] = sym;
    TEST_ASSERT_TRUE(ray_key_cols_sorted(cols, n_keys, 0, 2));
    TEST_ASSERT_TRUE(ray_key_cols_sorted(cols, n_keys, 1, 2));
    free(cols); ray_release(sym);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* A late inversion must lend prepared ranks to the real sort without
 * freeing them too early; early/small cases avoid precheck rank setup.
 * Row identities independently verify stability of the repeated keys. */
static test_result_t test_unsorted_symbol_rank_reuse(void) {
    ray_heap_init(); ray_sym_init();
    int64_t z = ray_sym_intern("reuse-z", 7);
    int64_t a = ray_sym_intern("reuse-a", 7);
    int64_t ts_name = ray_sym_intern("reuse-time", 10);
    int64_t sym_name = ray_sym_intern("reuse-sym", 9);
    int64_t row_name = ray_sym_intern("reuse-row", 9);
    int64_t key_ids[] = {ts_name, sym_name};
    ray_t* keys = ray_vec_from_raw(RAY_SYM, key_ids, 2);
    for (int desc = 0; desc < 2; desc++) {
        for (int kind = 0; kind < 3; kind++) {
            int64_t n = kind == 0 ? 8 : 200000;
            int64_t block = kind == 0 ? n : 100;
            int64_t inversion = (kind == 2 ? n - block : 0) + block / 2 - 1;
            ray_t* sym = ray_vec_new(RAY_SYM, n);
            ray_t* time = ray_vec_new(RAY_I64, n);
            ray_t* row = ray_vec_new(RAY_I64, n);
            sym->len = time->len = row->len = n;
            for (int64_t i = 0; i < n; i++) {
                bool first = i % block < block / 2;
                ((int64_t*)ray_data(sym))[i] = first ? (desc ? z : a) : (desc ? a : z);
                ((int64_t*)ray_data(time))[i] = (desc ? n - 1 - i : i) / block;
                ((int64_t*)ray_data(row))[i] = i;
            }
            int64_t* sv = ray_data(sym);
            int64_t swap = sv[inversion];
            sv[inversion] = sv[inversion + 1]; sv[inversion + 1] = swap;
            ray_t* table = ray_table_new(3);
            table = ray_table_add_col(table, ts_name, time);
            table = ray_table_add_col(table, sym_name, sym);
            table = ray_table_add_col(table, row_name, row);
            ray_t* result = desc ? ray_xdesc_fn(table, keys) : ray_xasc_fn(table, keys);
            TEST_ASSERT_NOT_NULL(result);
            TEST_ASSERT_FALSE(RAY_IS_ERR(result));
            TEST_ASSERT_EQ_I(ray_table_nrows(result), n);
            ray_t* out_sym = ray_table_get_col(result, sym_name);
            ray_t* out_row = ray_table_get_col(result, row_name);
            for (int64_t i = 0; i < n; i++) {
                bool first = i % block < block / 2;
                int64_t expected_sym = first ? (desc ? z : a) : (desc ? a : z);
                int64_t expected_row = i == inversion ? i + 1 : (i == inversion + 1 ? i - 1 : i);
                TEST_ASSERT_EQ_I(ray_read_sym(ray_data(out_sym), i, out_sym->type, out_sym->attrs), expected_sym);
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(out_row))[i], expected_row);
            }
            ray_release(result); ray_release(table);
            ray_release(sym); ray_release(time); ray_release(row);
        }
    }
    ray_release(keys);
    ray_sym_destroy(); ray_heap_destroy();
    PASS();
}

/* The optional LUT must not turn a sparse but sorted column into a
 * declined shortcut. Intern padding BEFORE the used names. */
static test_result_t test_sorted_symbol_sparse_fallback(void) {
    ray_heap_init();
    ray_sym_init();
    for (int i = 0; i < 70000; i++) {
        char name[40];
        int len = snprintf(name, sizeof(name), "unused-rank-padding-%d", i);
        ray_sym_intern(name, (size_t)len);
    }
    int64_t z = ray_sym_intern("late-z", 6);
    int64_t a = ray_sym_intern("late-a", 6);
    TEST_ASSERT_TRUE(a > 65536 && z > 65536);
    int64_t values[] = {a, a, a, a, z, z, z, z};
    ray_t* sym = ray_vec_from_raw(RAY_SYM, values, 8);
    ray_t* hdr = NULL;
    TEST_ASSERT_TRUE(build_enum_rank(sym, 8, &hdr) == NULL);
    TEST_ASSERT_TRUE(hdr == NULL); /* density guard, not allocation failure */
    TEST_ASSERT_TRUE(ray_key_cols_sorted(&sym, 1, 0, 8));
    TEST_ASSERT_FALSE(ray_key_cols_sorted(&sym, 1, 1, 8));
    int64_t name = ray_sym_intern("sparse-key", 10);
    ray_t* keys = make_sym_atom(name);
    ray_t* table = ray_table_new(1);
    table = ray_table_add_col(table, name, sym);
    ray_t* result = ray_xasc_fn(table, keys);
    TEST_ASSERT_TRUE(result == table); /* shortcut retained, not a full sort */
    ray_release(result);
    int64_t* data = ray_data(sym);
    for (int i = 0; i < 8; i++) data[i] = i < 4 ? z : a;
    TEST_ASSERT_TRUE(ray_key_cols_sorted(&sym, 1, 1, 8));
    result = ray_xdesc_fn(table, keys);
    TEST_ASSERT_TRUE(result == table);
    ray_release(result);
    data[7] = z;
    TEST_ASSERT_FALSE(ray_key_cols_sorted(&sym, 1, 1, 8));
    ray_release(table);
    ray_release(keys);
    ray_release(sym);

    /* Within the LUT budget, repeated constant segments still need only
     * the equal-ID scan; ray_key_cols_sorted must not build dense ranks. */
    ray_t* constant = ray_vec_new(RAY_SYM, 10000);
    constant->len = 10000;
    data = ray_data(constant);
    for (int i = 0; i < 10000; i++) data[i] = a;
    for (int i = 0; i < 20; i++)
        TEST_ASSERT_TRUE(ray_key_cols_sorted(&constant, 1, i % 2, 10000));
    ray_release(constant);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Lexical rank checks must not compare raw intern order. Exercise the
 * generic and specialized key orders, both directions, and a late inversion
 * beyond a dispatch boundary. The runner's pool supplies parallel coverage. */
static test_result_t test_sorted_symbol_rank_check(void) {
    ray_heap_init();
    ray_sym_init();
    int64_t z = ray_sym_intern("rank-z", 6);
    int64_t a = ray_sym_intern("rank-a", 6);
    TEST_ASSERT_TRUE(z < a);
    const int64_t n = 200000;
    ray_t* sym = ray_vec_new(RAY_SYM, n);
    ray_t* time = ray_vec_new(RAY_I64, n);
    sym->len = time->len = n;
    int64_t* sv = ray_data(sym);
    int64_t* tv = ray_data(time);
    ray_t* cols[] = {time, sym};
    for (int direction = 0; direction < 2; direction++) {
        for (int64_t i = 0; i < n; i++) {
            int64_t j = direction ? n - 1 - i : i;
            tv[i] = j / 2;
            sv[i] = j % 2 ? z : a;
        }
        TEST_ASSERT_TRUE(ray_key_cols_sorted(cols, 2, direction, n));
        TEST_ASSERT_FALSE(ray_key_cols_sorted(cols, 2, !direction, n));
        /* Swap a same-time symbol pair late in the column. */
        int64_t v = sv[n - 2]; sv[n - 2] = sv[n - 1]; sv[n - 1] = v;
        TEST_ASSERT_FALSE(ray_key_cols_sorted(cols, 2, direction, n));
        cols[0] = sym; cols[1] = time;
        for (int64_t i = 0; i < n; i++) {
            int64_t j = direction ? n - 1 - i : i;
            sv[i] = j < n / 2 ? a : z;
            tv[i] = j % (n / 2);
        }
        TEST_ASSERT_TRUE(ray_key_cols_sorted(cols, 2, direction, n));
        tv[n - 1] = direction ? n : -1;
        TEST_ASSERT_FALSE(ray_key_cols_sorted(cols, 2, direction, n));
        cols[0] = time; cols[1] = sym;
    }
    /* Symbol nulls must still decline the shortcut, even for one row. */
    int64_t null_id = 0;
    ray_t* nullable = ray_vec_from_raw(RAY_SYM, &null_id, 1);
    TEST_ASSERT_FALSE(ray_key_cols_sorted(&nullable, 1, 0, 1));
    ray_release(nullable);
    ray_release(time);
    ray_release(sym);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* A valid TIME slice makes the incorrect 64-bit read reproducible without
 * relying on allocation padding: packed adjacent backing pairs look ordered,
 * while the two logical 32-bit TIME cells are descending. Exercise both users
 * of sorted_check_fn, and aligned/non-8-byte-aligned zero-copy views. */
static test_result_t test_time_key_width(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);
    for (int negative = 0; negative < 2; negative++) {
        for (int64_t offset = 0; offset < 2; offset++) {
            int32_t raw[5] = {42, 42, 42, 42, 42};
            raw[offset] = negative ? -1000 : 2000;
            raw[offset + 1] = negative ? -2000 : 1000;
            raw[offset + 2] = negative ? -1000 : 3000;
            raw[offset + 3] = negative ? -1000 : 2000;
            ray_t* parent = ray_vec_from_raw(RAY_TIME, raw, offset + 4);
            TEST_ASSERT_TRUE(parent && !RAY_IS_ERR(parent));
            ray_t* times = ray_vec_slice(parent, offset, 2);
            TEST_ASSERT_TRUE(times && !RAY_IS_ERR(times));
            TEST_ASSERT_TRUE((times->attrs & RAY_ATTR_SLICE) != 0);
            ray_t* syms = ray_eval_str("[a a]");
            TEST_ASSERT_TRUE(syms && !RAY_IS_ERR(syms) && syms->type == RAY_SYM && syms->len == 2);
            ray_t* cols[] = {syms, times};
            TEST_ASSERT_FALSE(ray_key_cols_sorted(cols, 2, 0, 2));
            TEST_ASSERT_TRUE(ray_key_cols_sorted(cols, 2, 1, 2));
            ray_t* table = ray_table_new(2);
            TEST_ASSERT_TRUE(table && !RAY_IS_ERR(table));
            table = ray_table_add_col(table, ray_sym_intern("s", 1), syms);
            TEST_ASSERT_TRUE(table && !RAY_IS_ERR(table));
            table = ray_table_add_col(table, ray_sym_intern("t", 1), times);
            TEST_ASSERT_TRUE(table && !RAY_IS_ERR(table));
            ray_t* keys = ray_eval_str("[s t]");
            TEST_ASSERT_TRUE(keys && !RAY_IS_ERR(keys));
            ray_t* out = ray_xasc_fn(table, keys);
            TEST_ASSERT_TRUE(out && !RAY_IS_ERR(out));
            ray_t* got = ray_table_col(out, ray_sym_intern("t", 1));
            TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got) && got->type == RAY_TIME && got->len == 2);
            TEST_ASSERT_EQ_I(((int32_t*)ray_data(got))[0], raw[offset + 1]);
            TEST_ASSERT_EQ_I(((int32_t*)ray_data(got))[1], raw[offset]);
            ray_t* sorted_cols[] = {syms, got};
            TEST_ASSERT_TRUE(ray_key_cols_sorted(sorted_cols, 2, 0, 2));
            TEST_ASSERT_FALSE(ray_key_cols_sorted(sorted_cols, 2, 1, 2));
            TEST_ASSERT_TRUE(memcmp(ray_data(parent), raw, (size_t)(offset + 4) * sizeof(int32_t)) == 0);
            ray_release(out); ray_release(keys); ray_release(table);
            ray_release(syms); ray_release(times); ray_release(parent);
        }
    }
    ray_runtime_destroy(rt);
    PASS();
}

/* The sole logical inversion straddles the usual 8192-row morsel edge.
 * Every adjacent pair in the wider backing view still looks increasing.
 * The test also runs correctly without background workers; eligibility does
 * not claim that every configured worker physically participated. */
static test_result_t test_time_key_width_parallel(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);
    const int64_t n = RAY_PARALLEL_THRESHOLD + 1;
    const int64_t edge = (int64_t)RAY_DISPATCH_MORSELS * RAY_MORSEL_ELEMS;
    TEST_ASSERT_TRUE(edge > 0 && edge < n);
    int32_t* raw = malloc((size_t)(2 * n) * sizeof(int32_t));
    TEST_ASSERT_NOT_NULL(raw);
    for (int64_t i = 0; i < 2 * n; i++) raw[i] = (int32_t)i;
    raw[edge - 1] = (int32_t)edge; raw[edge] = (int32_t)(edge - 1);
    ray_t* parent = ray_vec_from_raw(RAY_TIME, raw, 2 * n);
    free(raw);
    TEST_ASSERT_TRUE(parent && !RAY_IS_ERR(parent));
    ray_t* times = ray_vec_slice(parent, 0, n);
    TEST_ASSERT_TRUE(times && !RAY_IS_ERR(times) && (times->attrs & RAY_ATTR_SLICE));
    char expr[64];
    snprintf(expr, sizeof(expr), "(take [a] %lld)", (long long)n);
    ray_t* syms = ray_eval_str(expr);
    TEST_ASSERT_TRUE(syms && !RAY_IS_ERR(syms) && syms->type == RAY_SYM && syms->len == n);
    ray_t* cols[] = {syms, times};
    TEST_ASSERT_FALSE(ray_key_cols_sorted(cols, 2, 0, n));
    ray_t* table = ray_table_new(2);
    TEST_ASSERT_TRUE(table && !RAY_IS_ERR(table));
    table = ray_table_add_col(table, ray_sym_intern("s", 1), syms);
    TEST_ASSERT_TRUE(table && !RAY_IS_ERR(table));
    table = ray_table_add_col(table, ray_sym_intern("t", 1), times);
    TEST_ASSERT_TRUE(table && !RAY_IS_ERR(table));
    ray_t* keys = ray_eval_str("[s t]");
    TEST_ASSERT_TRUE(keys && !RAY_IS_ERR(keys));
    ray_t* out = ray_xasc_fn(table, keys);
    TEST_ASSERT_TRUE(out && !RAY_IS_ERR(out));
    ray_t* got = ray_table_col(out, ray_sym_intern("t", 1));
    TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got) && got->type == RAY_TIME && got->len == n);
    const int32_t* data = ray_data(got);
    for (int64_t i = 0; i < n; i++) TEST_ASSERT_EQ_I(data[i], i);
    ray_t* sorted_cols[] = {syms, got};
    TEST_ASSERT_TRUE(ray_key_cols_sorted(sorted_cols, 2, 0, n));
    TEST_ASSERT_FALSE(ray_key_cols_sorted(sorted_cols, 2, 1, n));
    const int32_t* input = ray_data(parent);
    for (int64_t i = 0; i < 2 * n; i++) {
        int32_t expected = i == edge - 1 ? (int32_t)edge :
                           i == edge ? (int32_t)(edge - 1) : (int32_t)i;
        TEST_ASSERT_EQ_I(input[i], expected);
    }
    ray_release(out); ray_release(keys); ray_release(table);
    ray_release(syms); ray_release(times); ray_release(parent);
    ray_runtime_destroy(rt);
    PASS();
}

/* More than one morsel of unequal symbol boundaries spends the table
 * preparation budget. A late within-run TIME inversion must still reach
 * the same width-correct worker scan after ranks are prepared. */
static test_result_t test_time_key_width_rank_budget(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);
    const int64_t n = RAY_PARALLEL_THRESHOLD + 1;
    ray_t* syms = ray_vec_new(RAY_SYM, n);
    ray_t* times = ray_vec_new(RAY_TIME, n);
    ray_t* rows = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_TRUE(syms && !RAY_IS_ERR(syms));
    TEST_ASSERT_TRUE(times && !RAY_IS_ERR(times));
    TEST_ASSERT_TRUE(rows && !RAY_IS_ERR(rows));
    syms->len = times->len = rows->len = n;
    int64_t* sd = ray_data(syms);
    int32_t* td = ray_data(times);
    int64_t* rd = ray_data(rows);
    int64_t sid = 0;
    for (int64_t i = 0; i < n; i++) {
        if ((i % 32) == 0) {
            char name[32];
            int len = snprintf(name, sizeof(name), "time-width-%04lld", (long long)(i / 32));
            sid = ray_sym_intern(name, (size_t)len);
        }
        sd[i] = sid; td[i] = (int32_t)((i % 32 - 16) * 1000); rd[i] = i;
    }
    ray_t* cols[] = {syms, times};
    TEST_ASSERT_TRUE(ray_key_cols_sorted(cols, 2, 0, n));
    int32_t tmp = td[n - 3]; td[n - 3] = td[n - 2]; td[n - 2] = tmp;
    TEST_ASSERT_FALSE(ray_key_cols_sorted(cols, 2, 0, n));
    ray_t* table = ray_table_new(3);
    TEST_ASSERT_TRUE(table && !RAY_IS_ERR(table));
    table = ray_table_add_col(table, ray_sym_intern("s", 1), syms);
    TEST_ASSERT_TRUE(table && !RAY_IS_ERR(table));
    table = ray_table_add_col(table, ray_sym_intern("t", 1), times);
    TEST_ASSERT_TRUE(table && !RAY_IS_ERR(table));
    table = ray_table_add_col(table, ray_sym_intern("r", 1), rows);
    TEST_ASSERT_TRUE(table && !RAY_IS_ERR(table));
    ray_t* keys = ray_eval_str("[s t]");
    TEST_ASSERT_TRUE(keys && !RAY_IS_ERR(keys));
    ray_t* out = ray_xasc_fn(table, keys);
    TEST_ASSERT_TRUE(out && !RAY_IS_ERR(out));
    ray_t* os = ray_table_col(out, ray_sym_intern("s", 1));
    ray_t* ot = ray_table_col(out, ray_sym_intern("t", 1));
    ray_t* rr = ray_table_col(out, ray_sym_intern("r", 1));
    TEST_ASSERT_TRUE(os && !RAY_IS_ERR(os) && os->len == n);
    TEST_ASSERT_TRUE(ot && !RAY_IS_ERR(ot) && ot->type == RAY_TIME && ot->len == n);
    TEST_ASSERT_TRUE(rr && !RAY_IS_ERR(rr) && rr->type == RAY_I64 && rr->len == n);
    for (int64_t i = 0; i < n; i++) {
        int64_t id = i == n - 3 ? n - 2 : i == n - 2 ? n - 3 : i;
        TEST_ASSERT_EQ_I(((int32_t*)ray_data(ot))[i], (i % 32 - 16) * 1000);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(rr))[i], id);
        TEST_ASSERT_EQ_I(ray_read_sym(ray_data(os), i, RAY_SYM, os->attrs), sd[i]);
        TEST_ASSERT_EQ_I(td[i], (id % 32 - 16) * 1000);
        TEST_ASSERT_EQ_I(rd[i], i);
    }
    ray_t* sorted[] = {os, ot};
    TEST_ASSERT_TRUE(ray_key_cols_sorted(sorted, 2, 0, n));
    ray_release(out); ray_release(keys); ray_release(table);
    ray_release(syms); ray_release(times); ray_release(rows);
    ray_runtime_destroy(rt);
    PASS();
}

/* ─── Entry table ────────────────────────────────────────────────── */

const test_entry_t sort_entries[] = {
    { "sort/time_key_width_rank_budget", test_time_key_width_rank_budget, NULL, NULL },
    { "sort/time_key_width_parallel", test_time_key_width_parallel, NULL, NULL },
    { "sort/time_key_width", test_time_key_width, NULL, NULL },
    { "sort/sorted_sym_i64_probe", test_sorted_sym_i64_probe, NULL, NULL },
    { "sort/sorted_large_constant_symbols", test_sorted_large_constant_symbols, NULL, NULL },
    { "sort/sorted_many_repeated_keys", test_sorted_many_repeated_keys, NULL, NULL },
    { "sort/unsorted_symbol_rank_reuse", test_unsorted_symbol_rank_reuse, NULL, NULL },
    { "sort/sorted_symbol_sparse_fallback", test_sorted_symbol_sparse_fallback, NULL, NULL },
    { "sort/sorted_symbol_rank_check", test_sorted_symbol_rank_check, NULL, NULL },
    /* xrank tests */
    { "sort/xrank_basic",               test_xrank_basic,               NULL, NULL },
    { "sort/xrank_single_group",        test_xrank_single_group,        NULL, NULL },
    { "sort/xrank_zero_groups",         test_xrank_zero_groups,         NULL, NULL },
    { "sort/xrank_empty_vec",           test_xrank_empty_vec,           NULL, NULL },
    { "sort/xrank_non_numeric_first",   test_xrank_non_numeric_first_arg, NULL, NULL },
    { "sort/xrank_non_vec_second",      test_xrank_non_vec_second_arg,  NULL, NULL },
    { "sort/xrank_f64",                 test_xrank_f64,                 NULL, NULL },
    /* sort_table_by_keys list-of-sym-atoms */
    { "sort/xasc_list_of_sym_atoms",    test_xasc_list_of_sym_atoms,    NULL, NULL },
    { "sort/xdesc_list_of_sym_atoms",   test_xdesc_list_of_sym_atoms,   NULL, NULL },
    { "sort/xasc_two_sym_atoms_list",   test_xasc_two_sym_atoms_list,   NULL, NULL },
    { "sort/xasc_list_non_sym_error",   test_xasc_list_non_sym_atom_error, NULL, NULL },
    { "sort/xasc_wrong_key_type",       test_xasc_wrong_key_type_error, NULL, NULL },
    { "sort/xasc_missing_column",       test_xasc_missing_column_error, NULL, NULL },
    { "sort/xasc_wrong_first_arg",      test_xasc_wrong_first_arg_type, NULL, NULL },
    /* radix_decode_into non-packed paths */
    { "sort/i64_large_range_asc",       test_sort_i64_large_range_asc,  NULL, NULL },
    { "sort/i64_large_range_desc",      test_sort_i64_large_range_desc, NULL, NULL },
    { "sort/i32_large_range_asc",       test_sort_i32_large_range_asc,  NULL, NULL },
    { "sort/i32_large_range_desc",      test_sort_i32_large_range_desc, NULL, NULL },
    /* detect_sortedness parallel path */
    { "sort/i64_parallel_sortedness",   test_sort_i64_parallel_sortedness, NULL, NULL },
    /* edge cases */
    { "sort/indices_zero_cols",         test_sort_indices_zero_cols,    NULL, NULL },
    { "sort/indices_zero_rows",         test_sort_indices_zero_rows,    NULL, NULL },
    { "sort/indices_many_cols",         test_sort_indices_many_cols,    NULL, NULL },
    { "sort/asc_atom_passthrough",      test_asc_atom_passthrough,      NULL, NULL },
    { "sort/asc_single_element",        test_asc_single_element,        NULL, NULL },
    { "sort/asc_not_vec_error",         test_asc_not_vec_error,         NULL, NULL },
    { "sort/nulls_first",               test_sort_nulls_first,          NULL, NULL },
    /* ray_sort multi-column path */
    { "sort/multi_col",                 test_sort_multi_col,            NULL, NULL },
    /* Narrow-int single-key null encoding (regression for radix_encode_fn
     * I16/BOOL/U8 cases that ignored the null bitmap). */
    { "sort/i16_nulls_first_negs",      test_sort_i16_nulls_first_with_negatives, NULL, NULL },
    { "sort/i16_nulls_last_desc",       test_sort_i16_nulls_last_desc,  NULL, NULL },
    { "sort/i16_nulls_first_desc",      test_sort_i16_nulls_first_desc, NULL, NULL },
    { "sort/u8_nulls_last_asc",         test_sort_u8_nulls_last_asc,    NULL, NULL },
    { "sort/u8_nulls_first_desc",       test_sort_u8_nulls_first_desc,  NULL, NULL },
    { "sort/bool_nulls_first",          test_sort_bool_nulls_first,     NULL, NULL },
    /* top / bot — partial top-N / bottom-N happy paths.  Drive
     * ray_top_fn / ray_bot_fn over numeric and SYM vectors with
     * K<N, K=1, K=N to cover topk_indices_single (radix path) and
     * topk_indices_cmp_single (SYM comparator-heap path). */
    { "sort/top_i64_k_lt_n",            test_top_i64_k_lt_n,            NULL, NULL },
    { "sort/top_i64_k_eq_one",          test_top_i64_k_eq_one,          NULL, NULL },
    { "sort/bot_i64_k_lt_n",            test_bot_i64_k_lt_n,            NULL, NULL },
    { "sort/top_f64_k_lt_n",            test_top_f64_k_lt_n,            NULL, NULL },
    { "sort/bot_f64_k_lt_n",            test_bot_f64_k_lt_n,            NULL, NULL },
    { "sort/top_i64_k_eq_n",            test_top_i64_k_eq_n,            NULL, NULL },
    { "sort/bot_i64_k_eq_n",            test_bot_i64_k_eq_n,            NULL, NULL },
    { "sort/top_sym_k_lt_n",            test_top_sym_k_lt_n,            NULL, NULL },
    { "sort/bot_sym_k_lt_n",            test_bot_sym_k_lt_n,            NULL, NULL },
    { "sort/top_sym_k_eq_n",            test_top_sym_k_eq_n,            NULL, NULL },
    /* MSD bucket sort — 1M+ rows × 7-byte key range triggers the
     * msd_bucket_sort_fn / bucket_lsb_sort path in msd_radix_sort_run. */
    { "sort/msd_bucket_i64_asc",        test_sort_msd_bucket_i64,       NULL, NULL },
    { "sort/msd_bucket_i64_desc",       test_sort_msd_bucket_i64_desc,  NULL, NULL },
    /* gather_by_idx W8 SYM path (eval.c line 1146) */
    { "sort/sym_w8_gather",             test_sort_sym_w8_gather,        NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
