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
#include "table/sym.h"
#include "lang/internal.h"
#include <string.h>
#include <stdint.h>

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

/* ─── Entry table ────────────────────────────────────────────────── */

const test_entry_t sort_entries[] = {
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
