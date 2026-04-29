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
#include "lang/format.h"
#include <string.h>
#include <limits.h>
#include <math.h>

/* ---- Setup / Teardown ---- */

static void fmt_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void fmt_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- Test: format i64 atom ---- */
static test_result_t test_fmt_i64(void) {
    ray_t* result = ray_fmt(ray_i64(42), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_STR_EQ(s, "42");
    ray_release(result);

    PASS();
}

/* ---- Test: format negative i64 atom ---- */
static test_result_t test_fmt_i64_neg(void) {
    ray_t* result = ray_fmt(ray_i64(-1), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "-1"));
    ray_release(result);

    PASS();
}

/* ---- Test: format f64 with 2-decimal precision ---- */
static test_result_t test_fmt_f64(void) {
    ray_t* result = ray_fmt(ray_f64(3.14159), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "3.14"));
    ray_release(result);

    PASS();
}

/* ---- Test: format f64 scientific notation ---- */
static test_result_t test_fmt_f64_sci(void) {
    ray_t* result = ray_fmt(ray_f64(1e7), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    /* Scientific notation: may be "e+07" or "e+7" depending on platform */
    TEST_ASSERT_TRUE(strstr(s, "e+07") != NULL || strstr(s, "e+7") != NULL);
    ray_release(result);

    PASS();
}

/* ---- Test: format f64 zero ---- */
static test_result_t test_fmt_f64_zero(void) {
    ray_t* result = ray_fmt(ray_f64(0.0), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "0.0"));
    ray_release(result);

    PASS();
}

/* ---- Test: format bool true ---- */
static test_result_t test_fmt_bool_true(void) {
    ray_t* result = ray_fmt(ray_bool(true), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "true"));
    ray_release(result);

    PASS();
}

/* ---- Test: format bool false ---- */
static test_result_t test_fmt_bool_false(void) {
    ray_t* result = ray_fmt(ray_bool(false), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "false"));
    ray_release(result);

    PASS();
}

/* ---- Test: format null i64 (INT64_MIN -> 0Nl) ---- */
static test_result_t test_fmt_null_i64(void) {
    ray_t* result = ray_fmt(ray_typed_null(-RAY_I64), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "0Nl"));
    ray_release(result);

    PASS();
}

/* ---- Test: format null f64 (NaN -> 0Nf) ---- */
static test_result_t test_fmt_null_f64(void) {
    ray_t* result = ray_fmt(ray_typed_null(-RAY_F64), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "0Nf"));
    ray_release(result);

    PASS();
}

/* ---- Test: format i64 vector ---- */
static test_result_t test_fmt_vec_i64(void) {
    int64_t raw[] = {1, 2, 3};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "[1 2 3]"));
    ray_release(result);
    ray_release(vec);

    PASS();
}

/* ---- Test: format empty i64 vector ---- */
static test_result_t test_fmt_vec_empty(void) {
    ray_t* vec = ray_vec_new(RAY_I64, 0);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "[]"));
    ray_release(result);
    ray_release(vec);

    PASS();
}

/* ---- Test: format table with box-drawing ---- */
static test_result_t test_fmt_table(void) {
    /* Build a 2-column, 3-row table */
    ray_t* tbl = ray_table_new(3);
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int64_t id_x = ray_sym_intern("x", 1);
    int64_t id_y = ray_sym_intern("y", 1);

    int64_t raw_x[] = {10, 20, 30};
    double  raw_y[] = {1.1, 2.2, 3.3};

    ray_t* col_x = ray_vec_from_raw(RAY_I64, raw_x, 3);
    ray_t* col_y = ray_vec_from_raw(RAY_F64, raw_y, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col_x));
    TEST_ASSERT_FALSE(RAY_IS_ERR(col_y));

    tbl = ray_table_add_col(tbl, id_x, col_x);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_y, col_y);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_t* result = ray_fmt(tbl, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);

    /* Check box-drawing characters */
    TEST_ASSERT_NOT_NULL(strstr(s, "\xe2\x94\x8c")); /* U+250C top-left corner */
    TEST_ASSERT_NOT_NULL(strstr(s, "\xe2\x94\x82")); /* U+2502 vertical bar */
    TEST_ASSERT_NOT_NULL(strstr(s, "\xe2\x94\x94")); /* U+2514 bottom-left corner */

    /* Check column names present */
    TEST_ASSERT_NOT_NULL(strstr(s, "x"));
    TEST_ASSERT_NOT_NULL(strstr(s, "y"));

    /* Check type names present (uppercase — columns are vectors) */
    TEST_ASSERT_NOT_NULL(strstr(s, "I64"));
    TEST_ASSERT_NOT_NULL(strstr(s, "F64"));

    /* Check footer with row count */
    TEST_ASSERT_NOT_NULL(strstr(s, "3 rows"));

    ray_release(result);
    ray_release(tbl);

    PASS();
}

/* ---- Test: ray_type_name ---- */
static test_result_t test_type_name_i64(void) {
    /* Positive = vector type → uppercase */
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_I64), "I64");
    /* Negative = atom type → lowercase */
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_I64), "i64");
    PASS();
}

static test_result_t test_type_name_f64(void) {
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_F64), "F64");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_F64), "f64");
    PASS();
}

static test_result_t test_type_name_table(void) {
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_TABLE), "TABLE");
    PASS();
}

static test_result_t test_type_name_sym(void) {
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_SYM), "SYM");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_SYM), "sym");
    PASS();
}

/* ---- Suite definition ---- */

const test_entry_t format_entries[] = {
    { "format/atom/i64", test_fmt_i64, fmt_setup, fmt_teardown },
    { "format/atom/i64_neg", test_fmt_i64_neg, fmt_setup, fmt_teardown },
    { "format/atom/f64", test_fmt_f64, fmt_setup, fmt_teardown },
    { "format/atom/f64_sci", test_fmt_f64_sci, fmt_setup, fmt_teardown },
    { "format/atom/f64_zero", test_fmt_f64_zero, fmt_setup, fmt_teardown },
    { "format/atom/bool_true", test_fmt_bool_true, fmt_setup, fmt_teardown },
    { "format/atom/bool_false", test_fmt_bool_false, fmt_setup, fmt_teardown },
    { "format/null/i64", test_fmt_null_i64, fmt_setup, fmt_teardown },
    { "format/null/f64", test_fmt_null_f64, fmt_setup, fmt_teardown },
    { "format/vec/i64", test_fmt_vec_i64, fmt_setup, fmt_teardown },
    { "format/vec/empty", test_fmt_vec_empty, fmt_setup, fmt_teardown },
    { "format/table/box", test_fmt_table, fmt_setup, fmt_teardown },
    { "format/type/i64", test_type_name_i64, fmt_setup, fmt_teardown },
    { "format/type/f64", test_type_name_f64, fmt_setup, fmt_teardown },
    { "format/type/table", test_type_name_table, fmt_setup, fmt_teardown },
    { "format/type/sym", test_type_name_sym, fmt_setup, fmt_teardown },
    { NULL, NULL, NULL, NULL },
};


